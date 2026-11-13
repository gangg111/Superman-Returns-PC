// Reader of the offline shader pack (tools/shaders/pack_shaders.py), the
// format of the kit (reference/conan/port/src/native/native_renderer.cpp,
// "Shader pack embedded in conan.exe"), moved to a pure header so it is
// unit-tested without the game (tests/native).
//
// Little-endian: 'CNSH' u32 version(1) u32 count, then count x
// {u64 hash, u32 stage (0 = vs, 1 = ps, 2 = ps without RayQuery), u32 offset, u32 size} sorted by
// (hash, stage), then the DXIL blobs. Entries are 20 bytes and unaligned:
// read with memcpy, never as an aligned struct.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace superman_returns::native {

struct ShaderPackEntry {
  uint64_t hash = 0;
  uint32_t stage = 0, offset = 0, size = 0;
};

class ShaderPackView {
 public:
  static constexpr size_t kHeaderSize = 12;
  static constexpr size_t kEntrySize = 20;

  // False (and count() == 0) unless `data` holds a well-formed table.
  bool Parse(const uint8_t* data, size_t size) {
    base_ = nullptr;
    size_ = 0;
    count_ = 0;
    if (!data || size < kHeaderSize || std::memcmp(data, "CNSH", 4) != 0) return false;
    uint32_t version, count;
    std::memcpy(&version, data + 4, 4);
    std::memcpy(&count, data + 8, 4);
    if (version != 1 || kHeaderSize + uint64_t(count) * kEntrySize > size) return false;
    base_ = data;
    size_ = size;
    count_ = count;
    return true;
  }

  uint32_t count() const { return count_; }
  const uint8_t* base() const { return base_; }

  ShaderPackEntry Entry(uint32_t i) const {
    const uint8_t* e = base_ + kHeaderSize + size_t(i) * kEntrySize;
    ShaderPackEntry r;
    std::memcpy(&r.hash, e, 8);
    std::memcpy(&r.stage, e + 8, 4);
    std::memcpy(&r.offset, e + 12, 4);
    std::memcpy(&r.size, e + 16, 4);
    return r;
  }

  // Binary search over (hash, stage). Entries whose blob would lie outside the
  // pack are treated as missing.
  bool Find(uint64_t hash, bool vertex, ShaderPackEntry& out) const {
    return FindStage(hash, vertex ? kStageVertex : kStagePixel, out);
  }

  static constexpr uint32_t kStageVertex = 0, kStagePixel = 1;
  // Pixel shader compiled for shader model 6.0 without RayQuery, for GPUs
  // without inline ray tracing (only for pixel shaders whose main entry is 6.5).
  static constexpr uint32_t kStagePixelNoRt = 2;

  bool FindStage(uint64_t hash, uint32_t stage, ShaderPackEntry& out) const {
    uint32_t lo = 0, hi = count_;
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      const ShaderPackEntry m = Entry(mid);
      if (m.hash < hash || (m.hash == hash && m.stage < stage)) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (lo >= count_) return false;
    const ShaderPackEntry e = Entry(lo);
    if (e.hash != hash || e.stage != stage) return false;
    if (uint64_t(e.offset) + e.size > size_) return false;
    out = e;
    return true;
  }

 private:
  const uint8_t* base_ = nullptr;
  size_t size_ = 0;
  uint32_t count_ = 0;
};

}  // namespace superman_returns::native
