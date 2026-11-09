// A patch made by tools/builder/srdelta.py: the target from the player's source
// file, COPY runs of the source and ADD bytes of our own; the SHA-256 of both
// files is checked (format in srdelta.py's docstring).
#pragma once

#include <cstring>
#include <vector>

#include "util.h"

namespace srb {

inline std::vector<uint8_t> ApplyDelta(const std::vector<uint8_t>& source, const std::vector<uint8_t>& patch) {
  auto bad = [] { return std::runtime_error("a damaged patch in the builder"); };
  if (patch.size() < 89 || std::memcmp(patch.data(), "SRDLT001", 8) != 0) throw bad();
  uint64_t src_size, dst_size;
  std::memcpy(&src_size, patch.data() + 8, 8);
  std::memcpy(&dst_size, patch.data() + 48, 8);
  Sha256 hs;
  hs.Update(source.data(), source.size());
  if (source.size() != src_size || hs.Digest() != std::vector<uint8_t>(patch.begin() + 16, patch.begin() + 48))
    throw std::runtime_error("source");  // the caller names the file
  std::vector<uint8_t> out;
  out.reserve(size_t(dst_size));
  size_t p = 88;
  for (;;) {
    if (p >= patch.size()) throw bad();
    const uint8_t op = patch[p++];
    if (op == 0) break;
    uint64_t a, n;
    if (op == 1) {
      if (p + 16 > patch.size()) throw bad();
      std::memcpy(&a, patch.data() + p, 8);
      std::memcpy(&n, patch.data() + p + 8, 8);
      p += 16;
      if (a + n > source.size()) throw bad();
      out.insert(out.end(), source.begin() + ptrdiff_t(a), source.begin() + ptrdiff_t(a + n));
    } else if (op == 2) {
      if (p + 8 > patch.size()) throw bad();
      std::memcpy(&n, patch.data() + p, 8);
      p += 8;
      if (p + n > patch.size()) throw bad();
      out.insert(out.end(), patch.begin() + ptrdiff_t(p), patch.begin() + ptrdiff_t(p + n));
      p += size_t(n);
    } else {
      throw bad();
    }
  }
  Sha256 ht;
  ht.Update(out.data(), out.size());
  if (out.size() != dst_size || ht.Digest() != std::vector<uint8_t>(patch.begin() + 56, patch.begin() + 88)) throw bad();
  return out;
}

}  // namespace srb
