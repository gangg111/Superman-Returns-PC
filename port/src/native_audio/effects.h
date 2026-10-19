// Native rewrites of the effect plug-ins on the reverb and delay buses
// (docs/natywny-dzwiek/spec-efekty-wyjscie.md sections 1 and 2): ReverbModel1
// "RM10" and Delay "Del0". Field comments give the guest offsets.
#pragma once

#include <cstdint>
#include <vector>

#include "native_audio/plugins.h"

namespace native_audio {

class Reverb {
 public:
  explicit Reverb(int channels);  // channels = voice channel count (+60)
  int Process(Io& io, float mixer_rate);

  float p0 = 0.0f;   // +32 decay control (<= 0: off)
  float p1 = 15.0f;  // +40 room size, metres
  float p2 = 1.0f;   // +48 damping divisor
  float cached_rate = 0.0f;  // +56
  float tail = 0.0f;         // +24 / +64
  float prev_p0 = 0.0f, prev_p1 = 0.0f, prev_p2 = 1.0f;  // +68 +72 +76

  struct Comb {  // +80 + 32*i
    int len = 0;
    float c4 = 0, c8 = 0, c12 = 0;
    std::vector<float> buf;  // size len+2
    int pos = 0;
  };
  struct Allpass {  // +440..
    int len = 0;
    float g = 0;
    std::vector<float> buf;  // size len+1
    int pos = 0;
  };
  int channels;
  Comb comb[6];
  float t[6] = {};    // +272 distances
  int lengths[6] = {};// +296
  float a[6] = {};    // +344
  float b[6] = {};    // +368
  bool ap_init = false;  // +396
  int ap_count;          // +412
  Allpass ap[3];

 private:
  bool Update(float rate);
  void Rooms();
  void Lengths(float rate);
  void Damping(float rate);
  void Feedback();
  void Tail();
  void CombBlock(Comb& c, const float* in, float* out, bool overwrite);
  void AllpassBlock(Allpass& p, const float* in, float* out);
};

class Delay {
 public:
  explicit Delay(int channels) : channels(channels) {}
  int Process(Io& io, float mixer_rate);

  int channels;
  float p0 = 0.0f;  // +32 time, seconds
  float p1 = 0.0f;  // +40 feedback
  int state = 0;    // +48
  float cached_rate = 0.0f;  // +52
  int cnt = 0;               // +60
  float cur_t = 0; int cur_n = 0; float cur_fb = 0;  // +64 +68 +72
  float new_t = 0; int new_n = 0; float new_fb = 0;  // +76 +80 +84
  float old_t = 0; int old_n = 0; float old_fb = 0;  // +88 +92 +96
  std::vector<float> buf;  // +100, planar, channel c at c*size
  int size = 0;            // +104
  std::vector<float> ramp; // +108
  int pos = 0;             // +112
  int tail_n = 0; float tail_fb = 0.0f;  // +116 +120
  float tail20 = 0.0f, tail24 = 0.0f;    // +20 +24

 private:
  void Grow(int n);
  void UpdateTail();
};

}  // namespace native_audio
