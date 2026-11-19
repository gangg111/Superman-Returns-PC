#include "draw_state.h"
#include <exception>
namespace superman_returns::graphics::guest {
bool NormalizeIndicesInto(std::span<const uint8_t> source, uint32_t first,
                          uint32_t count, IndexEncoding encoding,
                          Primitive primitive, uint32_t *out,
                          std::string &error) {
  error.clear();
  const uint32_t width = encoding.index32 ? 4 : 2;
  const uint64_t end = uint64_t(first) + count;
  if (end * width > source.size()) {
    error = "Index range exceeds captured bytes";
    return false;
  }
  const bool strip = primitive == Primitive::kTriangleStrip ||
                     primitive == Primitive::kLineStrip;
  const uint32_t reset = strip ? encoding.restart : UINT32_MAX;
  for (uint64_t i = first; i < end; ++i) {
    uint32_t value = 0;
    for (uint32_t b = 0; b < width; ++b) {
      uint64_t address = (i * width + b) ^ IndexByteXor(encoding.endian);
      if (address >= source.size()) {
        error = "Endian index load exceeds captured bytes";
        return false;
      }
      value |= uint32_t(source[size_t(address)]) << (b * 8);
    }
    value &= encoding.index32 ? 0xffffffu : 0xffffu;
    // The owned packet stores uint32_t indices, including ushort inputs.
    // Promote the cut to that output width without confusing a legitimate
    // ushort 0xFFFF with a different programmable restart value.
    *out++ = reset!=UINT32_MAX && value==reset ? UINT32_MAX : value;
  }
  return true;
}
bool NormalizeIndices(std::span<const uint8_t> source, uint32_t first,
                      uint32_t count, IndexEncoding encoding,
                      Primitive primitive, std::vector<uint32_t> &out,
                      std::string &error) {
  error.clear();
  // The range first, as before NormalizeIndicesInto: the same error for an incomplete quad out of range.
  if ((uint64_t(first) + count) * (encoding.index32 ? 4 : 2) > source.size()) {
    error = "Index range exceeds captured bytes";
    return false;
  }
  if (primitive == Primitive::kQuads && count % 4) {
    error = "Incomplete quad";
    return false;
  }
  try {
    std::vector<uint32_t> values(count);
    if (!NormalizeIndicesInto(source, first, count, encoding, primitive,
                              values.data(), error))
      return false;
    if (primitive == Primitive::kQuads) {
      std::vector<uint32_t> triangles;
      triangles.reserve(size_t(count / 4) * 6);
      for (size_t i = 0; i < values.size(); i += 4)
        triangles.insert(triangles.end(),
                         {values[i], values[i + 1], values[i + 2], values[i],
                          values[i + 2], values[i + 3]});
      values.swap(triangles);
    }
    out.swap(values);
    return true;
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  }
}
} // namespace superman_returns::graphics::guest
