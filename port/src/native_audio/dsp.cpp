#include "native_audio/dsp.h"

#include <bit>

namespace native_audio {

void GainSet(float* dst, const float* src, float g, int n) {
  if (VmxPath(dst, src, n)) {
    VmxScope vmx;
    for (int i = 0; i < n; ++i) dst[i] = g * src[i];
  } else {
    ScalarScope fpu;
    for (int i = 0; i < n; ++i) dst[i] = src[i] * g;
  }
}

void GainAdd(float* dst, const float* src, float g, int n) {
  if (VmxPath(dst, src, n)) {
    VmxScope vmx;
    for (int i = 0; i < n; ++i) dst[i] = g * src[i] + dst[i];
  } else {
    ScalarScope fpu;
    for (int i = 0; i < n; ++i) dst[i] = Fmadds(src[i], g, dst[i]);
  }
}

namespace {

template <bool kAdd>
void Ramp(float* dst, const float* src, float g, float s, int n, int r) {
  float base[4];
  float s4;
  {
    ScalarScope fpu;
    s4 = s * 4.0f;
    base[0] = g;
    base[1] = g + s;
    base[2] = Fmadds(s, 2.0f, g);
    base[3] = Fmadds(s, 3.0f, g);
  }
  {
    VmxScope vmx;
    for (int blk = 0; blk < r / 32; ++blk) {
      for (int k = 0; k < 8; ++k)
        for (int l = 0; l < 4; ++l) {
          const int i = blk * 32 + k * 4 + l;
          const float gain = (k == 0) ? base[l] : float(k) * s4 + base[l];
          const float y = gain * src[i];
          dst[i] = kAdd ? y + dst[i] : y;
        }
      for (int l = 0; l < 4; ++l) base[l] = 8.0f * s4 + base[l];
    }
  }
  float g_end;
  {
    ScalarScope fpu;
    g_end = Fmadds(float(r), s, g);
  }
  VmxScope vmx;
  for (int i = r; i < n; ++i) dst[i] = kAdd ? g_end * src[i] + dst[i] : g_end * src[i];
}

}  // namespace

void GainRampSet(float* dst, const float* src, float g, float s, int n, int r) {
  Ramp<false>(dst, src, g, s, n, r);
}

void GainRampAdd(float* dst, const float* src, float g, float s, int n, int r) {
  Ramp<true>(dst, src, g, s, n, r);
}

void ResidualFade(float* out, float* residual, int channels, int stride) {
  static const uint32_t kBits[16] = {0x3F70F0F1, 0x3F61E1E2, 0x3F52D2D3, 0x3F43C3C4,
                                     0x3F34B4B5, 0x3F25A5A6, 0x3F169697, 0x3F078788,
                                     0x3EF0F0F1, 0x3ED2D2D3, 0x3EB4B4B5, 0x3E969697,
                                     0x3E70F0F1, 0x3E34B4B5, 0x3DF0F0F1, 0x3D70F0F1};
  ScalarScope fpu;
  for (int c = 0; c < channels; ++c) {
    float* o = out + c * stride;
    for (int k = 0; k < 16; ++k) o[k] = Fmadds(residual[c], std::bit_cast<float>(kBits[k]), o[k]);
    residual[c] = 0.0f;
  }
}

}  // namespace native_audio
