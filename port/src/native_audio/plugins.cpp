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

// ---------------------------------------------------------------- buses

namespace {

const float kSendK = std::bit_cast<float>(0x3F34FDF4u);  // 0x8208228C, 0.707f

struct MapOp {
  int8_t o, i;
  bool k;  // scaled by kSendK
};

// Order of the kernel calls per (in, out), read in sub_8265D3C8.. / sub_82668400..
const MapOp* MapTable(int in, int out, int* n_ops, uint8_t* zero_mask) {
  static const MapOp k12[] = {{0, 0, true}, {1, 0, true}};
  static const MapOp k16[] = {{1, 0, false}};
  static const MapOp k21[] = {{0, 0, false}, {0, 1, false}};
  static const MapOp k24[] = {{0, 0, false}, {1, 1, false}};
  static const MapOp k26[] = {{0, 0, false}, {2, 1, false}};
  static const MapOp k41[] = {{0, 0, false}, {0, 1, false}, {0, 2, false}, {0, 3, false}};
  static const MapOp k42[] = {{0, 0, false}, {0, 2, false}, {1, 1, false}, {1, 3, false}};
  static const MapOp k46[] = {{0, 0, false}, {2, 1, false}, {3, 2, false}, {4, 3, false}};
  static const MapOp k61[] = {{0, 1, false}, {0, 2, false}, {0, 4, false}, {0, 3, false},
                              {0, 0, false}};
  static const MapOp k62[] = {{0, 1, true}, {0, 0, false}, {0, 3, false},
                              {1, 1, true}, {1, 2, false}, {1, 4, false}};
  static const MapOp k64[] = {{0, 1, true}, {0, 0, false}, {1, 1, true},
                              {1, 2, false}, {2, 3, false}, {3, 4, false}};
#define SR_MAP(tbl, z) \
  do {                 \
    *n_ops = int(sizeof(tbl) / sizeof(tbl[0])); \
    *zero_mask = (z);  \
    return tbl;        \
  } while (0)
  if (in == 1 && out == 2) SR_MAP(k12, 0);
  if (in == 1 && out == 4) SR_MAP(k12, 0x0C);
  if (in == 1 && out == 6) SR_MAP(k16, 0x3D);
  if (in == 2 && out == 1) SR_MAP(k21, 0);
  if (in == 2 && out == 4) SR_MAP(k24, 0x0C);
  if (in == 2 && out == 6) SR_MAP(k26, 0x3A);
  if (in == 4 && out == 1) SR_MAP(k41, 0);
  if (in == 4 && out == 2) SR_MAP(k42, 0);
  if (in == 4 && out == 6) SR_MAP(k46, 0x22);
  if (in == 6 && out == 1) SR_MAP(k61, 0);
  if (in == 6 && out == 2) SR_MAP(k62, 0);
  if (in == 6 && out == 4) SR_MAP(k64, 0);
#undef SR_MAP
  *n_ops = 0;
  *zero_mask = 0;
  return nullptr;
}

}  // namespace

void MapChannels(float* out, int out_ch, const float* in, int in_ch, int n, float g, float s,
                 bool ramp, bool add) {
  if (in_ch == out_ch) {
    if (ramp) {
      for (int c = 0; c < in_ch; ++c)
        add ? GainRampAdd(out + c * n, in + c * n, g, s, n, 64)
            : GainRampSet(out + c * n, in + c * n, g, s, n, 64);
    } else {
      add ? GainAdd(out, in, g, n * in_ch) : GainSet(out, in, g, n * in_ch);
    }
    return;
  }
  int n_ops;
  uint8_t zero;
  const MapOp* ops = MapTable(in_ch, out_ch, &n_ops, &zero);
  if (!ops) {  // other counts (not used by the game): copy what fits
    const int m = in_ch < out_ch ? in_ch : out_ch;
    add ? GainAdd(out, in, g, n * m) : GainSet(out, in, g, n * m);
    if (!add && out_ch > in_ch) std::memset(out + n * in_ch, 0, size_t(out_ch - in_ch) * n * 4);
    return;
  }
  float gk, sk;
  {
    ScalarScope fpu;
    gk = g * kSendK;
    sk = s * kSendK;
  }
  uint8_t written = 0;
  for (int k = 0; k < n_ops; ++k) {
    const MapOp& op = ops[k];
    float* d = out + op.o * n;
    const float* x = in + op.i * n;
    const bool set = !add && !(written & (1u << op.o));
    written |= uint8_t(1u << op.o);
    const float gg = op.k ? gk : g, ss = op.k ? sk : s;
    if (ramp)
      set ? GainRampSet(d, x, gg, ss, n, 64) : GainRampAdd(d, x, gg, ss, n, 64);
    else
      set ? GainSet(d, x, gg, n) : GainAdd(d, x, gg, n);
  }
  if (!add)
    for (int c = 0; c < out_ch; ++c)
      if (zero & (1u << c)) std::memset(out + c * n, 0, size_t(n) * 4);
}

Bus::Bus(int ch) : channels(ch) {
  buffer = reinterpret_cast<float*>((uintptr_t(storage) + 127) & ~uintptr_t(127));
}

void Bus::LinkSender(const void* sender, Voice* o, void (*clear)(const void*)) {
  links.insert(links.begin(), Link{sender, o, clear});
  ++attached;
}

void Bus::Release() {
  for (const Link& l : links) l.clear(l.sender);
  attached = uint16_t(attached - links.size());
  links.clear();
}

void Bus::UnlinkSender(const void* sender) {
  for (size_t i = 0; i < links.size(); ++i)
    if (links[i].sender == sender) {
      links.erase(links.begin() + i);
      break;
    }
  --attached;
}

void Bus::AddResidual(const float* res) {
  residual_pending = true;
  ScalarScope fpu;
  for (int c = 0; c < channels; ++c) residual[c] = residual[c] + res[c];
}

int Bus::Process(Io& io) {
  if (contributions == 0 && !residual_pending) return 0;
  float* out = io.Next();
  io.Toggle();
  if (contributions) {
    std::memcpy(out, buffer, size_t(channels) * kBlock * 4);
    contributions = 0;
  } else {
    std::memset(out, 0, size_t(channels) * kBlock * 4);
  }
  if (residual_pending) {
    ResidualFade(out, residual, channels, kBlock);
    residual_pending = false;
  }
  return 1;
}

Send::Send(int in_ch, bool ignore) : in_channels(in_ch), ignore_voice_gain(ignore) {}

void Send::Hook(const Io& io) {
  ScalarScope fpu;
  target = level;
  if (!ignore_voice_gain) target = io.gain * level;
}

void Send::Attach(Bus* b) {
  if (bus) {
    bus->UnlinkSender(this);
    bus->AddResidual(last);
    bus = nullptr;
  }
  if (b) {
    bus = b;
    b->LinkSender(this, owner, [](const void* p) { const_cast<Send*>(static_cast<const Send*>(p))->bus = nullptr; });
  }
}

void Send::Release() {
  if (!bus) return;
  float tmp[6] = {};
  MapChannels(tmp, bus->channels, last, in_channels, 1, 1.0f, 0.0f, false, false);
  bus->UnlinkSender(this);
  bus->AddResidual(tmp);
  bus = nullptr;
}

int Send::Process(Io& io, bool reset) {
  if (reset || need_snap) {
    prev = target;
    need_snap = false;
  }
  if (!bus) {
    need_snap = true;
    return 1;
  }
  const float* in = io.Cur();
  int count;
  float* b = bus->Acquire(&count);
  bool ramp;
  float s = 0.0f;
  {
    ScalarScope fpu;
    ramp = target != prev;
    if (ramp) s = (target - prev) * std::bit_cast<float>(0x3C7C0FC1u);  // 1/65
  }
  if (ramp)
    MapChannels(b, bus->channels, in, in_channels, kBlock, prev, s, true, count != 0);
  else
    MapChannels(b, bus->channels, in, in_channels, kBlock, target, 0.0f, false, count != 0);
  prev = target;
  ScalarScope fpu;
  for (int c = 0; c < in_channels; ++c) last[c] = in[c * kBlock + 255] * target;
  return 1;
}

void Route::Attach(Bus* b, int s, int d, int n) {
  if (bus) {
    bus->UnlinkSender(this);
    bus->AddResidual(last);
    bus = nullptr;
  }
  if (b) {
    bus = b;
    b->LinkSender(this, owner, [](const void* p) { const_cast<Route*>(static_cast<const Route*>(p))->bus = nullptr; });
    src = uint8_t(s);
    dst = uint8_t(d);
    count = uint8_t(n);
  }
}

void Route::Release() {
  if (!bus) return;
  bus->UnlinkSender(this);
  bus->AddResidual(last);
  bus = nullptr;
}

int Route::Process(Io& io) {
  if (!bus) return 1;
  const float* in = io.Cur();
  int n;
  float* b = bus->Acquire(&n);
  if (n == 0) std::memset(b, 0, size_t(bus->channels) * kBlock * 4);
  for (int i = 0; i < count; ++i)
    GainAdd(b + (dst + i) * kBlock, in + (src + i) * kBlock, 1.0f, kBlock);
  for (float& v : last) v = 0.0f;
  for (int i = 0; i < count; ++i) last[dst + i] = in[(src + i) * kBlock + 255];
  return 1;
}

}  // namespace native_audio
