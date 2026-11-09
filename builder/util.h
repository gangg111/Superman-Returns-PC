// Small helpers shared by the builder: UTF-8/UTF-16, SHA-256 (BCrypt), files.
#pragma once

#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace srb {

namespace fs = std::filesystem;

inline std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

inline std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

// A failure the window shows as it is (already in the user's language).
struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class Sha256 {
 public:
  Sha256() {
    if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) != 0)
      throw std::runtime_error("SHA-256 is not available");
  }
  ~Sha256() {
    if (hash_) BCryptDestroyHash(hash_);
    if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
  }
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;
  void Update(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (size) {
      const ULONG n = ULONG(size > 0x40000000 ? 0x40000000 : size);
      BCryptHashData(hash_, const_cast<PUCHAR>(p), n, 0);
      p += n;
      size -= n;
    }
  }
  std::string Hex() {
    uint8_t d[32];
    BCryptFinishHash(hash_, d, 32, 0);
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (uint8_t b : d) {
      s += digits[b >> 4];
      s += digits[b & 15];
    }
    return s;
  }
  std::vector<uint8_t> Digest() {
    std::vector<uint8_t> d(32);
    BCryptFinishHash(hash_, d.data(), 32, 0);
    return d;
  }

 private:
  BCRYPT_ALG_HANDLE alg_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
};

inline std::vector<uint8_t> ReadAll(const fs::path& path) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot read " + Narrow(path.wstring()));
  LARGE_INTEGER size;
  GetFileSizeEx(h, &size);
  std::vector<uint8_t> data(size_t(size.QuadPart));
  size_t done = 0;
  while (done < data.size()) {
    DWORD got = 0;
    const DWORD want = DWORD(std::min<size_t>(data.size() - done, 1u << 30));
    if (!ReadFile(h, data.data() + done, want, &got, nullptr) || got == 0) {
      CloseHandle(h);
      throw std::runtime_error("cannot read " + Narrow(path.wstring()));
    }
    done += got;
  }
  CloseHandle(h);
  return data;
}

inline void WriteAll(const fs::path& path, const void* data, size_t size) {
  fs::create_directories(path.parent_path());
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot write " + Narrow(path.wstring()));
  const auto* p = static_cast<const uint8_t*>(data);
  while (size) {
    DWORD put = 0;
    const DWORD want = DWORD(std::min<size_t>(size, 1u << 30));
    if (!WriteFile(h, p, want, &put, nullptr) || put == 0) {
      CloseHandle(h);
      throw std::runtime_error("cannot write " + Narrow(path.wstring()));
    }
    p += put;
    size -= put;
  }
  CloseHandle(h);
}

}  // namespace srb
