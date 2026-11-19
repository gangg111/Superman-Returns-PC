#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace superman_returns::graphics::guest {
enum class Primitive {
  kPoints,
  kLines,
  kLineStrip,
  kTriangles,
  kTriangleStrip,
  kQuads,
  kRectangles
};
struct IndexEncoding {
  bool index32;
  uint32_t endian;
  uint32_t restart;
};
struct VertexFetchMeta {
  uint32_t buffer, offset, stride, type;
};
constexpr uint32_t IndexByteXor(uint32_t endian) {
  constexpr uint32_t masks[]{0, 1, 3, 2};
  return masks[endian & 3];
}
constexpr uint32_t HostStripIndex(uint32_t value, bool index32,
                                  uint32_t reset) {
  value &= index32 ? 0xffffffu : 0xffffu;
  return reset != UINT32_MAX && value == reset
             ? (index32 ? UINT32_MAX : UINT16_MAX)
             : value;
}
inline uint32_t LoadIndex(const uint8_t *bytes, uint32_t index, bool index32,
                          uint32_t endian) {
  uint32_t width = index32 ? 4 : 2;
  uint64_t offset = uint64_t(index) * width;
  uint32_t value = 0;
  for (uint32_t b = 0; b < width; ++b)
    value |= uint32_t(bytes[(offset + b) ^ IndexByteXor(endian)]) << (8 * b);
  return value;
}
// NormalizeIndices without the quad expansion: writes `count` values to `out`.
bool NormalizeIndicesInto(std::span<const uint8_t>, uint32_t first, uint32_t count,
                          IndexEncoding, Primitive, uint32_t *out, std::string &);
// Produces uint32 indices with UINT32_MAX as the enabled strip restart cut.
bool NormalizeIndices(std::span<const uint8_t>, uint32_t first, uint32_t count,
                      IndexEncoding, Primitive, std::vector<uint32_t> &,
                      std::string &);
} // namespace superman_returns::graphics::guest
