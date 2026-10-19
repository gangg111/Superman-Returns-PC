// Shared DSP building blocks of the native audio engine, written to give the
// same bits as the recompiled RenderWare Audio Core (the reference the
// self-test compares against, docs/natywny-dzwiek/spec-proste-wtyczki.md 0.5).
//
// Two arithmetic modes exist in the recompiled code and both are reproduced:
// * scalar FPU ops: plain float ops, fmadds = float(fma in double), denormals kept;
// * VMX ops: vmaddfp lowered to a multiply and a separate add (two roundings),
//   run with MXCSR flush-to-zero and denormals-are-zero (VmxScope).
#pragma once

#include <cstdint>
#include <cstring>

#include <xmmintrin.h>

#pragma clang fp contract(off)

namespace native_audio {

constexpr int kBlock = 256;  // frames per mixer block

inline float Fmadds(float a, float b, float c) {  // fmadds / fmaddsx
  return float(__builtin_fma(double(a), double(b), double(c)));
}
inline float Fnmsubs(float a, float c, float b) {  // fnmsubs: b - a*c, one rounding
  return float(-__builtin_fma(double(a), double(c), -double(b)));
}

// MXCSR flush mode for the duration of a VMX-derived kernel.
class VmxScope {
 public:
  VmxScope() : saved_(_mm_getcsr()) { _mm_setcsr(saved_ | 0x8040u); }
  ~VmxScope() { _mm_setcsr(saved_); }

 private:
  unsigned saved_;
};

// Scalar code must run with flush mode off, whatever the thread had before.
class ScalarScope {
 public:
  ScalarScope() : saved_(_mm_getcsr()) { _mm_setcsr(saved_ & ~0x8040u); }
  ~ScalarScope() { _mm_setcsr(saved_); }

 private:
  unsigned saved_;
};

// The guest kernels take the VMX path for 128-aligned buffers and N % 64 == 0.
inline bool VmxPath(const float* d, const float* s, int n) {
  return (((uintptr_t(d) & uintptr_t(s)) & 0x7F) == 0) && (n % 64) == 0;
}

// sub_8265B488: dst = g * src
void GainSet(float* dst, const float* src, float g, int n);
// sub_8265AF48: dst += g * src (VMX: two roundings; scalar: fmadds)
void GainAdd(float* dst, const float* src, float g, int n);
// sub_8265B708 / sub_8265B168: gain g + k*s over the first r samples, then
// fmadds(r, s, g) for the rest; Add accumulates into dst.
void GainRampSet(float* dst, const float* src, float g, float s, int n, int r);
void GainRampAdd(float* dst, const float* src, float g, float s, int n, int r);
// sub_8265AD70: fade a per-channel residual out over 16 samples.
void ResidualFade(float* out, float* residual, int channels, int stride);

}  // namespace native_audio
