#include "native_audio/plugins.h"

#include <bit>
#include <cmath>
#include <cstring>

namespace native_audio {

namespace {

const float kPi = std::bit_cast<float>(0x40490FDBu);    // 0x82082644 / 0x82082680
const float kWMin = std::bit_cast<float>(0x3B4DE32Fu);  // 0x82082660, 0.001*pi
const float kWMax = std::bit_cast<float>(0x4048DC62u);  // 0x82082664, 0.999*pi

}  // namespace

// ---------------------------------------------------------------- filters

BiquadFilter::BiquadFilter(bool hp, int ch)
    : high_pass(hp), channels(ch), cutoff(hp ? 0.0f : 1000000.0f), last_w(cutoff) {}

void BiquadFilter::Hook(const Io& io, float voice_rate_in) {
  ScalarScope fpu;
  rate_ratio = io.rate / voice_rate_in;
}

void BiquadFilter::Clear() {
  std::memset(xh, 0, sizeof(xh));
  std::memset(yh, 0, sizeof(yh));
}

void BiquadFilter::Coefficients(float w) {
  const float s = float(std::sin(double(w)));
  const float c = float(std::cos(double(w)));
  const float alpha = s * 0.5f;
  const float t = c * -2.0f;
  const float opa = alpha + 1.0f;
  const float oma = 1.0f - alpha;
  const float inv = 1.0f / opa;
  const float den = opa * 2.0f;
  a1 = t * inv;
  a2 = oma * inv;
  if (!high_pass) {
    const float u = 1.0f - c;
    b1 = u * inv;
    b0 = u / den;
    b2 = u / den;
  } else {
    const float u = c + 1.0f;
    b1 = -(u * inv);
    b0 = u / den;
    b2 = u / den;
  }
}

int BiquadFilter::Process(Io& io, float mixer_rate) {
  ScalarScope fpu;
  float w = ((kPi / mixer_rate) * rate_ratio) * cutoff;
  w = w * 2.0f;
  if (!high_pass) {
    if (!(w < kWMax)) {
      if (last_w < kWMax) Clear();
      last_w = w;
      return 1;
    }
    if (w < kWMin) w = kWMin;
  } else {
    if (!(w > kWMin)) {
      if (last_w > kWMin) Clear();
      last_w = w;
      return 1;
    }
    if (w > kWMax) w = kWMax;
  }
  if (w != last_w) {
    Coefficients(w);
    last_w = w;
  }
  // BiquadRun (sub_82668E18), direct form I.
  const float* in = io.Cur();
  float* out = io.Next();
  for (int ch = 0; ch < channels; ++ch) {
    const float* x = in + ch * kBlock;
    float* y = out + ch * kBlock;
    const float X1 = xh[ch][0], X2 = xh[ch][1], Y1 = yh[ch][0], Y2 = yh[ch][1];
    float t = b1 * X1;
    t = Fmadds(b2, X2, t);
    t = Fmadds(b0, x[0], t);
    t = Fnmsubs(a1, Y1, t);
    t = Fnmsubs(a2, Y2, t);
    y[0] = t;
    t = b2 * X1;
    t = Fmadds(x[1], b0, t);
    t = Fmadds(b1, x[0], t);
    t = Fnmsubs(y[0], a1, t);
    t = Fnmsubs(a2, Y1, t);
    y[1] = t;
  }
  for (int ch = 0; ch < channels; ++ch) {
    const float* x = in + ch * kBlock;
    float* y = out + ch * kBlock;
    for (int n = 2; n < kBlock; ++n) {
      float t = b1 * x[n - 1];
      t = Fmadds(b2, x[n - 2], t);
      t = Fmadds(b0, x[n], t);
      t = Fnmsubs(a1, y[n - 1], t);
      t = Fnmsubs(a2, y[n - 2], t);
      y[n] = t;
    }
  }
  for (int ch = 0; ch < channels; ++ch) {
    const float* x = in + ch * kBlock;
    const float* y = out + ch * kBlock;
    xh[ch][0] = x[255];
    xh[ch][1] = x[254];
    yh[ch][0] = y[255];
    yh[ch][1] = y[254];
  }
  io.Toggle();
  return 1;
}

// ---------------------------------------------------------------- gain

int Gain::Process(Io& io, bool reset) {
  const float* in = io.Cur();
  float* out = io.Next();
  float s;
  {
    ScalarScope fpu;
    if (reset) prev = gain;
    s = (gain - prev) * 0.015625f;
  }
  for (int c = 0; c < channels; ++c)
    GainRampSet(out + c * kBlock, in + c * kBlock, prev, s, kBlock, 64);
  io.Toggle();
  prev = gain;
  return 1;
}

}  // namespace native_audio
