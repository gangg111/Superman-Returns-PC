// SndPlayer's resampler (RenderWare Audio Core, player+124), rewritten from
// docs/natywny-dzwiek/spec-pitch-resampler.md section 4: 2-point linear
// interpolation with a 16.16 phase, a 10-frame history window per channel and
// the planner's bookkeeping (inputs_for / outputs_for / skip). Every function
// mirrors one guest function and keeps its quirks, so the state can be
// compared field by field with the original (native_audio/selftest.cpp).
#pragma once

#include <cstdint>
#include <vector>

namespace native_audio {

// The guest's fmadds as the recompiled code computes it (fma in double, then
// rounded to float), the reference the native engine is compared against.
inline float GuestFmadds(float a, float b, float c);

class Resampler {
 public:
  static constexpr int kHistory = 2;            // H, rs+27
  static constexpr int kWindow = kHistory + 8;  // W, floats per channel
  static constexpr uint32_t kMaxStep = 0x40000;

  explicit Resampler(int channels);  // sub_8265D208

  // State mirrored from the guest object (offsets in comments).
  uint32_t step = 0;     // +0, 16.16
  uint32_t frac = 0;     // +4, 0..0xFFFF
  float rate = -1.0f;    // +8, last rate passed in
  float rate_eff = -1.0f;// +12, rate or 4.0 when capped
  int32_t off = 0;       // +16
  uint8_t channels = 0;  // +24
  uint8_t primed = 0;    // +25
  uint8_t lead = 0;      // +26
  uint8_t history_taps = kHistory;  // +27
  std::vector<float> history;       // +20, channels * kWindow

  struct Snapshot {  // sub_8265C3B0 / sub_8265C3F8 (history not saved)
    uint32_t step, frac;
    float rate, rate_eff;
    int32_t off;
    uint8_t primed, lead, history_taps;
  };
  Snapshot Save() const;
  void Restore(const Snapshot& s);

  void SetRate(float r);                        // sub_8265C4A8
  int InputsFor(float r, int n_out);            // sub_8265C970
  int OutputsFor(int n_in, float r);            // sub_8265D350
  int Search(int n, float r, bool with_state);  // sub_8265CA08
  void Skip(float r, int n_out, int n_in);      // sub_8265CBD0
  // sub_8265CCB0: in/out planar (channel stride n_in / n_out). Returns false
  // when the caller must use `in` unchanged (unity step) or n_out < 0.
  bool Process(float* out, const float* in, int n_out, int n_in, float r);

 private:
  float& Hist(int c, int j) { return history[size_t(c) * kWindow + j]; }
  void Kernel(const float* src, float* dst, int count, int* idx, uint32_t* frac_word,
              int src_stride, int dst_stride) const;
};

inline float GuestFmadds(float a, float b, float c) {
  return float(__builtin_fma(double(a), double(b), double(c)));
}

}  // namespace native_audio
