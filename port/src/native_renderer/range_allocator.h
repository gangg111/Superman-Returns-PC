// First-fit allocator of byte ranges inside one fixed-size block (a chunk of
// the renderer's pooled guest buffers). Free ranges are kept by offset and
// merged with their neighbours on release. No D3D12: unit tested in
// tests/native/test_range_allocator.cpp.
#pragma once

#include <cstdint>
#include <iterator>
#include <map>

namespace superman_returns::native {

class RangeAllocator {
 public:
  static constexpr uint64_t kFailed = ~0ull;

  explicit RangeAllocator(uint64_t size, uint64_t alignment = 256)
      : size_(size), alignment_(alignment) {
    free_[0] = size;
  }

  // Offset of `bytes` (rounded up to the alignment), or kFailed.
  uint64_t Allocate(uint64_t bytes) {
    const uint64_t need = Round(bytes);
    if (!need) return kFailed;
    for (auto it = free_.begin(); it != free_.end(); ++it) {
      if (it->second < need) continue;
      const uint64_t offset = it->first, left = it->second - need;
      free_.erase(it);
      if (left) free_[offset + need] = left;
      used_ += need;
      return offset;
    }
    return kFailed;
  }

  // Returns a range from Allocate (same `bytes`).
  void Release(uint64_t offset, uint64_t bytes) {
    uint64_t length = Round(bytes);
    used_ -= length;
    auto next = free_.lower_bound(offset);
    if (next != free_.end() && offset + length == next->first) {
      length += next->second;
      next = free_.erase(next);
    }
    if (next != free_.begin()) {
      auto prev = std::prev(next);
      if (prev->first + prev->second == offset) {
        prev->second += length;
        return;
      }
    }
    free_[offset] = length;
  }

  uint64_t Round(uint64_t bytes) const { return (bytes + alignment_ - 1) / alignment_ * alignment_; }
  uint64_t size() const { return size_; }
  uint64_t used() const { return used_; }
  size_t free_ranges() const { return free_.size(); }

 private:
  uint64_t size_, alignment_, used_ = 0;
  std::map<uint64_t, uint64_t> free_;  // offset -> length
};

}  // namespace superman_returns::native
