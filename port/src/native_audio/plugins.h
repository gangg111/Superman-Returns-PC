// Native rewrites of the RenderWare Audio Core plug-ins the game uses.
// Specs: docs/natywny-dzwiek/spec-proste-wtyczki.md, spec-efekty-wyjscie.md,
// spec-pitch-resampler.md. Each class keeps the guest object's state as
// named fields (guest offsets in comments) so the self-test can compare them.
#pragma once

#include <cstdint>

#include "native_audio/dsp.h"

namespace native_audio {

// Double-buffered planar block the plug-ins of one voice pass along
// (the guest "io" object: +4/+8 buffers, +12 rate, +16 gain, +28 current).
struct Io {
  float* buf[2] = {nullptr, nullptr};
  float rate = 1.0f;   // +12
  float gain = 1.0f;   // +16
  uint8_t cur = 0;     // +28
  float* Cur() const { return buf[cur]; }
  float* Next() const { return buf[cur ^ 1]; }
  void Toggle() { cur = (cur == 0); }
};

// LowPassIir2 "LI20" / HighPassIir2 "HI20" (spec 1).
class BiquadFilter {
 public:
  BiquadFilter(bool high_pass, int channels);
  void Hook(const Io& io, float voice_rate_in);   // +20 hook: ratio = io.rate / voice+44
  int Process(Io& io, float mixer_rate);          // returns 0 = chain stops

  bool high_pass;
  int channels;
  float cutoff;          // +32, param 0 (Hz)
  float a1 = 0, a2 = 0, b0 = 0, b1 = 0, b2 = 0;  // +44..+60
  float last_w;          // +64
  float rate_ratio = 0;  // +68
  float xh[6][2] = {}, yh[6][2] = {};

 private:
  void Coefficients(float w);
  void Clear();
};

// Gain "Gai0" (spec 2).
class Gain {
 public:
  explicit Gain(int channels) : channels(channels) {}
  int Process(Io& io, bool reset);

  int channels;
  float gain = 1.0f;  // +32, param 0 (linear)
  float prev = 1.0f;  // +40
};

}  // namespace native_audio
