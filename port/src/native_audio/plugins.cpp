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

// ---------------------------------------------------------------- pan

namespace {

// Angle scale: PnS0 class descriptor +68 (0x82936F74, copied at run time to
// 0x8292ED74), 0x40C90FDB = 2*pi.
const float kTwoPi = std::bit_cast<float>(0x40C90FDBu);
const float kInv360 = std::bit_cast<float>(0x3B360B61u);  // 0x82021BD4, 1/360

int VirtualInputs(int in_ch) { return (in_ch == 1 || in_ch == 4) ? 4 : (in_ch == 2) ? 2 : in_ch; }

}  // namespace

PanSurround::PanSurround(int in_ch, int out_ch)
    : in_channels(in_ch), out_channels(out_ch), half_width(std::bit_cast<float>(0x40A3EF9Eu)) {
  // Speaker vectors normalized at create (sub_82652130, VMX): len2 = x*x + y*y,
  // e = 1/sqrt(len2) (the recompiled vrsqrtefp), two Newton steps, v *= e.
  static const float raw[5][2] = {{-1, 1}, {0, 1}, {1, 1}, {-1, -1}, {1, -1}};
  VmxScope vmx;
  for (int o = 0; o < 5; ++o) {
    const float x = raw[o][0], y = raw[o][1];
    const float len2 = x * x + y * y;
    float e = 1.0f / std::sqrt(len2);
    for (int k = 0; k < 2; ++k) {
      const float ee = e * e;
      const float t = 1.0f - len2 * ee;
      const float h = e * 0.5f;
      e = h * t + e;
    }
    spk[o][0] = x * e;
    spk[o][1] = y * e;
  }
}

void PanSurround::Matrix() {
  ScalarScope fpu;
  const int n = VirtualInputs(in_channels);
  for (int o = 0; o < 5; ++o)
    for (int i = 0; i < n; ++i) cur[o][i] = 0.0f;
  float v[6][2] = {};
  const float a = kTwoPi * kInv360;
  const float th = (azimuth + 90.0f) * a;
  const float c = float(std::cos(double(th)));
  const float s = float(std::sin(double(th)));
  const float cx = c * distance, sy = s * distance;
  const float p2 = half_width;
  if (n == 2) {
    v[0][0] = Fmadds(-s, p2, cx);
    v[0][1] = Fmadds(c, p2, sy);
    v[1][0] = Fmadds(-s, -p2, cx);
    v[1][1] = Fmadds(c, -p2, sy);
  } else if (n == 4) {
    const float xl = Fmadds(-s, p2, cx), yl = Fmadds(c, p2, sy);
    const float xr = Fmadds(-s, -p2, cx), yr = Fmadds(c, -p2, sy);
    const float cp = c * p2, sp = s * p2;
    v[0][0] = Fmadds(p2, c, xl);
    v[0][1] = Fmadds(p2, s, yl);
    v[1][0] = Fmadds(p2, c, xr);
    v[1][1] = Fmadds(p2, s, yr);
    v[2][0] = xl - cp;
    v[2][1] = yl - sp;
    v[3][0] = xr - cp;
    v[3][1] = yr - sp;
  }
  for (int i = 0; i < n; ++i) {
    float gv[5];
    {
      VmxScope vmx;
      for (int o = 0; o < 5; ++o) gv[o] = spk[o][0] * v[i][0] + spk[o][1] * v[i][1];
    }
    gv[1] = gv[1] * 0.5f;
    float sum = 0.0f;
    for (int o = 0; o < 5; ++o) {
      if (gv[o] < 0.0f) gv[o] = 0.0f;
      sum = Fmadds(gv[o], gv[o], sum);
    }
    if (sum > 0.0f) {
      float k = 1.0f / std::sqrt(sum);
      k = k / float(n);
      for (int o = 0; o < 5; ++o) gv[o] = gv[o] * k;
    }
    for (int o = 0; o < 5; ++o) cur[o][i] = gv[o];
  }
}

int PanSurround::Process(Io& io) {
  const float* in = io.Cur();
  float* out = io.Next();
  bool changed;
  {
    ScalarScope fpu;
    changed = force || azimuth != prev_az || distance != prev_dist || half_width != prev_width;
  }
  if (changed) {
    force = false;
    std::memcpy(prev, cur, sizeof(cur));
    Matrix();
    prev_az = azimuth;
    prev_dist = distance;
    prev_width = half_width;
  }
  const float* src[4] = {};
  int n = 0;
  if (in_channels == 1) {
    src[0] = src[1] = src[2] = src[3] = in;
    n = 4;
  } else if (in_channels == 2) {
    src[0] = in;
    src[1] = in + kBlock;
    n = 2;
  } else if (in_channels == 4) {
    for (int i = 0; i < 4; ++i) src[i] = in + i * kBlock;
    n = 4;
  }
  float* outp = out;
  for (int o = 0; o < 5; ++o, outp += kBlock)
    for (int i = 0; i < n; ++i) {
      const float g0 = prev[o][i];
      float s;
      {
        ScalarScope fpu;
        s = (cur[o][i] - g0) * 0.015625f;
      }
      if (i == 0)
        changed ? GainRampSet(outp, src[i], g0, s, kBlock, 64) : GainSet(outp, src[i], g0, kBlock);
      else
        changed ? GainRampAdd(outp, src[i], g0, s, kBlock, 64) : GainAdd(outp, src[i], g0, kBlock);
    }
  if (out_channels > 5) std::memset(outp, 0, size_t(out_channels - 5) * kBlock * 4);
  std::memcpy(prev, cur, sizeof(cur));
  io.Toggle();
  return 1;
}

}  // namespace native_audio
