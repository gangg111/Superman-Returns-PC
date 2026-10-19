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

// PanSurroundPlugIn "PnS0" (spec 3): 1/2/4 input channels to 6 outputs in the
// engine order L, C, R, Ls, Rs, LFE (LFE always zero).
class PanSurround {
 public:
  PanSurround(int in_channels, int out_channels);
  int Process(Io& io);

  int in_channels, out_channels;
  float distance = 0.0f;    // +32, param 0
  float azimuth = 0.0f;     // +40, param 1 (degrees, 0 = front, + = left)
  float half_width;         // +48, param 2
  float prev_az = 0, prev_dist = 0, prev_width = 0;  // +56, +60, +64
  float spk[5][2];          // speaker directions (R+64 of row o)
  float cur[5][6] = {};     // R+4
  float prev[5][6] = {};    // R+28
  bool force = true;        // +576

 private:
  void Matrix();
};

// Channel mapping in -> out shared by Send and SndPlayer's converter
// (spec-proste-wtyczki.md 4.4, spec-pitch-resampler.md 3.6). Planar blocks of
// n frames. add: accumulate, never zero. ramp: start gain g, step s, 64 frames.
void MapChannels(float* out, int out_ch, const float* in, int in_ch, int n, float g, float s,
                 bool ramp, bool add);

// SubMix "Sub0" (spec 5): a bus the senders write into during a block.
class Bus {
 public:
  explicit Bus(int channels);
  // sub_826514B0: the buffer and the number of earlier contributions this block.
  float* Acquire(int* count) {
    *count = contributions++;
    return buffer;
  }
  void AddResidual(const float* residual);  // detach with residual (sub_82651518)
  int Process(Io& io);

  int channels;
  alignas(128) float storage[6 * kBlock + 32];
  float* buffer;
  uint16_t contributions = 0;  // +148
  uint16_t attached = 0;       // +146
  float residual[6] = {};      // +52
  bool residual_pending = false;  // +151
};

// Send "Sen0" (spec 4).
class Send {
 public:
  Send(int in_channels, bool ignore_voice_gain);
  void Hook(const Io& io);
  int Process(Io& io, bool reset);
  void Attach(Bus* b);  // Control(0, bus) handler: detach with residual, then attach

  int in_channels;
  float level = 1.0f;   // +40, param 0
  float last[6] = {};   // +64
  float prev = 1.0f;    // +88
  float target = 0.0f;  // +92
  bool ignore_voice_gain;  // +96
  bool need_snap = false;  // +97
  Bus* bus = nullptr;      // +56
};

// Route "Rou0" (spec 6).
class Route {
 public:
  int Process(Io& io);
  void Attach(Bus* b, int src, int dst, int count);  // Control handler

  Bus* bus = nullptr;
  uint8_t src = 0, dst = 0, count = 0;  // +52..+54
  float last[6] = {};                   // +56
};

}  // namespace native_audio
