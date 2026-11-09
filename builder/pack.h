// The builder's payload: files appended to the builder's own .exe by
// tools/builder/make_builder.py, read straight from the .exe (nothing unpacked
// to a temporary folder).
//
//   ... the program ...
//   file data (each stored or compressed with the Windows Compression API, LZMS)
//   index: u32 count, per file: u16 path length, path (UTF-8, '/'), u8 method
//          (0 stored, 1 LZMS), u64 offset, u64 packed size, u64 size, 32 bytes SHA-256
//   u64 index offset, "SRBPAK01"
#pragma once

#include <windows.h>
#include <compressapi.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "util.h"

namespace srb {

struct PackFile {
  std::string path;
  uint8_t method = 0;
  uint64_t offset = 0, packed = 0, size = 0;
  std::vector<uint8_t> sha;
};

class Pack {
 public:
  // The pack at the end of `exe`; false when there is none (the developer's build).
  bool Open(const fs::path& exe) {
    file_ = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    GetFileSizeEx(file_, &size);
    if (size.QuadPart < 16) return false;
    uint8_t tail[16];
    Read(uint64_t(size.QuadPart) - 16, tail, 16);
    if (std::memcmp(tail + 8, "SRBPAK01", 8) != 0) return false;
    uint64_t index_at;
    std::memcpy(&index_at, tail, 8);
    std::vector<uint8_t> index(size_t(uint64_t(size.QuadPart) - 16 - index_at));
    Read(index_at, index.data(), index.size());
    size_t p = 0;
    auto take = [&](void* dst, size_t n) {
      if (p + n > index.size()) throw std::runtime_error("the builder's file is damaged");
      std::memcpy(dst, index.data() + p, n);
      p += n;
    };
    uint32_t count;
    take(&count, 4);
    for (uint32_t i = 0; i < count; ++i) {
      PackFile f;
      uint16_t len;
      take(&len, 2);
      f.path.resize(len);
      take(f.path.data(), len);
      take(&f.method, 1);
      take(&f.offset, 8);
      take(&f.packed, 8);
      take(&f.size, 8);
      f.sha.resize(32);
      take(f.sha.data(), 32);
      files_[f.path] = f;
    }
    return true;
  }
  ~Pack() {
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
  }

  const std::map<std::string, PackFile>& Files() const { return files_; }
  bool Has(const std::string& path) const { return files_.count(path) != 0; }

  // A file's bytes, unpacked and checked against its SHA-256.
  std::vector<uint8_t> Get(const std::string& path) const {
    auto it = files_.find(path);
    if (it == files_.end()) throw std::runtime_error("the builder has no " + path);
    const PackFile& f = it->second;
    std::vector<uint8_t> packed(size_t(f.packed));
    Read(f.offset, packed.data(), packed.size());
    std::vector<uint8_t> out;
    if (f.method == 0) {
      out = std::move(packed);
    } else {
      DECOMPRESSOR_HANDLE d = nullptr;
      if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &d))
        throw std::runtime_error("the Windows decompressor is not available");
      out.resize(size_t(f.size));
      SIZE_T got = 0;
      const BOOL ok = Decompress(d, packed.data(), packed.size(), out.data(), out.size(), &got);
      CloseDecompressor(d);
      if (!ok || got != f.size) throw std::runtime_error("cannot unpack " + path);
    }
    Sha256 h;
    h.Update(out.data(), out.size());
    if (h.Digest() != f.sha) throw std::runtime_error("a damaged file in the builder: " + path);
    return out;
  }

 private:
  void Read(uint64_t at, void* dst, size_t n) const {
    OVERLAPPED o{};
    o.Offset = DWORD(at);
    o.OffsetHigh = DWORD(at >> 32);
    auto* p = static_cast<uint8_t*>(dst);
    while (n) {
      DWORD got = 0;
      const DWORD want = DWORD(std::min<size_t>(n, 1u << 30));
      if (!ReadFile(file_, p, want, &got, &o) || got == 0)
        throw std::runtime_error("cannot read the builder's file");
      p += got;
      n -= got;
      at += got;
      o.Offset = DWORD(at);
      o.OffsetHigh = DWORD(at >> 32);
    }
  }

  HANDLE file_ = INVALID_HANDLE_VALUE;
  std::map<std::string, PackFile> files_;
};

}  // namespace srb
