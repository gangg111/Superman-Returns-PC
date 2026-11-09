// The player's game: an Xbox 360 disc image (XDVDFS) or a folder holding
// default.xex and DATA. Both give the same list of files and a way to copy one.
//
// XDVDFS: "MICROSOFT*XBOX*MEDIA" at 0x10000 into the game partition (0 for an
// extracted image, 0xFD90000 on XGD2 discs, 0x2080000 on XGD3); then the root
// directory's sector (2048 bytes) and size. A directory is a binary tree of
// entries: u16 left, u16 right (in 4-byte units, 0 none, 0xFFFF empty), u32
// sector, u32 size, u8 attributes (0x10 directory), u8 name length, name.
#pragma once

#include <windows.h>

#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "util.h"

namespace srb {

struct GameFile {
  uint64_t offset = 0;  // in the image (unused for a folder)
  uint64_t size = 0;
};

class GameSource {
 public:
  // Opens `path`: an .iso image or a folder. Throws when it is neither.
  void Open(const fs::path& path) {
    if (fs::is_directory(path)) {
      folder_ = path;
      if (fs::exists(path / "game" / "default.xex") && !fs::exists(path / "default.xex")) folder_ = path / "game";
      for (auto& e : fs::recursive_directory_iterator(folder_)) {
        if (!e.is_regular_file()) continue;
        std::string rel = Narrow(fs::relative(e.path(), folder_).generic_wstring());
        files_[Upper(rel)] = {rel, GameFile{0, uint64_t(e.file_size())}};
      }
      return;
    }
    image_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (image_ == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open the image");
    static const uint64_t kPartitions[] = {0, 0xFD90000, 0x2080000, 0x18300000};
    for (uint64_t part : kPartitions) {
      uint8_t head[0x24] = {};
      if (!TryRead(part + 0x10000, head, sizeof head) || std::memcmp(head, "MICROSOFT*XBOX*MEDIA", 20) != 0)
        continue;
      partition_ = part;
      uint32_t root, size;
      std::memcpy(&root, head + 20, 4);
      std::memcpy(&size, head + 24, 4);
      Walk(root, size, "");
      return;
    }
    throw std::runtime_error("not an Xbox 360 disc image");
  }
  ~GameSource() {
    if (image_ != INVALID_HANDLE_VALUE) CloseHandle(image_);
  }

  // The file's real name (as on the disc) and size, by name in any case; false when missing.
  bool Find(const std::string& name, std::string* real, uint64_t* size) const {
    auto it = files_.find(Upper(name));
    if (it == files_.end()) return false;
    if (real) *real = it->second.first;
    if (size) *size = it->second.second.size;
    return true;
  }
  // Every file under `dir` ("DATA"), real names.
  std::vector<std::string> List(const std::string& dir) const {
    std::vector<std::string> out;
    const std::string prefix = Upper(dir) + "/";
    for (auto& [key, v] : files_)
      if (key.rfind(prefix, 0) == 0) out.push_back(v.first);
    return out;
  }

  // Copies `name` to `dst`, feeding every chunk to `sha` and `progress(bytes)`.
  void Copy(const std::string& name, const fs::path& dst, Sha256& sha,
            const std::function<void(uint64_t)>& progress) const {
    auto it = files_.find(Upper(name));
    if (it == files_.end()) throw std::runtime_error("the game has no " + name);
    fs::create_directories(dst.parent_path());
    HANDLE out = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (out == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot write " + Narrow(dst.wstring()));
    HANDLE in = image_;
    uint64_t at = it->second.second.offset;
    bool own = false;
    if (image_ == INVALID_HANDLE_VALUE) {
      in = CreateFileW((folder_ / Widen(it->second.first)).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                       OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
      if (in == INVALID_HANDLE_VALUE) {
        CloseHandle(out);
        throw std::runtime_error("cannot read " + name);
      }
      own = true;
      at = 0;
    }
    std::vector<uint8_t> buf(8u << 20);
    uint64_t left = it->second.second.size;
    while (left) {
      const DWORD want = DWORD(std::min<uint64_t>(left, buf.size()));
      OVERLAPPED o{};
      o.Offset = DWORD(at);
      o.OffsetHigh = DWORD(at >> 32);
      DWORD got = 0, put = 0;
      if (!ReadFile(in, buf.data(), want, &got, &o) || got != want ||
          !WriteFile(out, buf.data(), got, &put, nullptr) || put != got) {
        if (own) CloseHandle(in);
        CloseHandle(out);
        throw std::runtime_error("cannot copy " + name);
      }
      sha.Update(buf.data(), got);
      at += got;
      left -= got;
      progress(got);
    }
    if (own) CloseHandle(in);
    CloseHandle(out);
  }

  // A file's bytes (for the small ones the patches start from).
  std::vector<uint8_t> Read(const std::string& name) const {
    auto it = files_.find(Upper(name));
    if (it == files_.end()) throw std::runtime_error("the game has no " + name);
    if (image_ == INVALID_HANDLE_VALUE) return ReadAll(folder_ / Widen(it->second.first));
    std::vector<uint8_t> data(size_t(it->second.second.size));
    if (!TryRead(it->second.second.offset, data.data(), data.size())) throw std::runtime_error("cannot read " + name);
    return data;
  }

 private:
  static std::string Upper(std::string s) {
    for (char& c : s) {
      if (c >= 'a' && c <= 'z') c = char(c - 32);
      if (c == '\\') c = '/';
    }
    return s;
  }

  bool TryRead(uint64_t at, void* dst, size_t n) const {
    auto* p = static_cast<uint8_t*>(dst);
    while (n) {
      OVERLAPPED o{};
      o.Offset = DWORD(at);
      o.OffsetHigh = DWORD(at >> 32);
      DWORD got = 0;
      const DWORD want = DWORD(std::min<size_t>(n, 1u << 30));
      if (!ReadFile(image_, p, want, &got, &o) || got == 0) return false;
      p += got;
      n -= got;
      at += got;
    }
    return true;
  }

  void Walk(uint32_t sector, uint32_t size, const std::string& dir) {
    if (size == 0 || size > (64u << 20)) return;
    std::vector<uint8_t> d(size);
    if (!TryRead(partition_ + uint64_t(sector) * 2048, d.data(), size)) throw std::runtime_error("cannot read the image");
    std::function<void(uint32_t, int)> node = [&](uint32_t off, int depth) {
      if (depth > 4096 || uint64_t(off) * 4 + 14 > d.size()) return;
      const uint8_t* e = d.data() + size_t(off) * 4;
      uint16_t left, right;
      uint32_t start, len;
      std::memcpy(&left, e, 2);
      std::memcpy(&right, e + 2, 2);
      if (left == 0xFFFF) return;
      std::memcpy(&start, e + 4, 4);
      std::memcpy(&len, e + 8, 4);
      const uint8_t attr = e[12], name_len = e[13];
      if (uint64_t(off) * 4 + 14 + name_len > d.size()) return;
      const std::string name(reinterpret_cast<const char*>(e + 14), name_len);
      if (left) node(left, depth + 1);
      if (attr & 0x10)
        Walk(start, len, dir + name + "/");
      else
        files_[Upper(dir + name)] = {dir + name, GameFile{partition_ + uint64_t(start) * 2048, len}};
      if (right) node(right, depth + 1);
    };
    node(0, 0);
  }

  HANDLE image_ = INVALID_HANDLE_VALUE;
  uint64_t partition_ = 0;
  fs::path folder_;
  std::map<std::string, std::pair<std::string, GameFile>> files_;  // upper-case name -> real name, place
};

}  // namespace srb
