#include "native_audio/resampler.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace native_audio {

namespace {

// 0x82082274: the fraction scale, NOT 1/65536 (1.52579996e-05f).
const float kFracScale = std::bit_cast<float>(0x377FFC9Cu);

}  // namespace

Resampler::Resampler(int ch) : channels(uint8_t(ch)), history(size_t(ch) * kWindow, 0.0f) {}

Resampler::Snapshot Resampler::Save() const {
  return {step, frac, rate, rate_eff, off, primed, lead, history_taps};
}

void Resampler::Restore(const Snapshot& s) {
  step = s.step;
  frac = s.frac;
  rate = s.rate;
  rate_eff = s.rate_eff;
  off = s.off;
  primed = s.primed;
  lead = s.lead;
  history_taps = s.history_taps;
}

void Resampler::SetRate(float r) {
  float t = r * 65536.0f;
  t = (t < 0.0f) ? (t - 0.5f) : (t + 0.5f);
  // fctiwz: truncate toward zero, saturating to int32.
  int32_t i;
  if (!(t == t)) i = INT32_MIN;
  else if (t >= 2147483648.0f) i = INT32_MAX;
  else if (t < -2147483648.0f) i = INT32_MIN;
  else i = int32_t(t);
  if (i > int32_t(kMaxStep)) {
    rate = r;
    rate_eff = 4.0f;
    step = kMaxStep;
  } else {
    rate_eff = r;
    step = uint32_t(i);
    rate = r;
  }
}

int Resampler::InputsFor(float r, int n_out) {
  if (r != rate) SetRate(r);
  if (r == 1.0f) return n_out;
  const int extra = primed ? 0 : (history_taps - lead);
  const int v = int((step * uint32_t(n_out) + frac) >> 16) - off + extra;
  return v < 0 ? 0 : v;
}

int Resampler::Search(int n, float r, bool with_state) {
  if (!(r > 0.0f)) return 8192;
  int base = n;
  if (with_state) {
    if (!primed) base = lead - history_taps + n;
    base += off;
  }
  const float q = float(base) / r;
  int k = (q >= 2147483648.0f) ? INT32_MAX : (q < -2147483648.0f || q != q) ? INT32_MIN : int(q);
  if (k > 8192) k = 8192;
  auto f = [&](int kk) {
    return with_state ? InputsFor(r, kk) : int((step * uint32_t(kk) + frac) >> 16);
  };
  const int v = f(k);
  if (v == n) return k;
  if (v > n) {
    while (k > 0) {
      --k;
      if (f(k) <= n) return k;
    }
    return 0;
  }
  for (;;) {
    ++k;
    if (k > 8192) return k - 1;
    if (f(k) >= n) return k - 1;
  }
}

int Resampler::OutputsFor(int n_in, float r) {
  if (n_in <= 0) return 0;
  if (r != rate) SetRate(r);
  if (r == 1.0f) return n_in;
  return Search(n_in, rate_eff, true);
}

void Resampler::Skip(float r, int n_out, int n_in) {
  if (r != rate) SetRate(r);
  if (step == 0x10000) {
    off = 0;
    primed = 1;
    return;
  }
  const uint32_t pos = step * uint32_t(n_out) + frac;
  int extra = 0;
  if (!primed) {
    extra = history_taps - lead;
    primed = 1;
  }
  int adv = int(pos >> 16) + extra;
  if (adv < 0) adv = 0;
  off = off - adv + n_in;
  if (off < -history_taps) off = -history_taps;
  frac = pos & 0xFFFF;
}

void Resampler::Kernel(const float* src, float* dst, int count, int* idx, uint32_t* frac_word,
                       int src_stride, int dst_stride) const {
  uint32_t fr = *frac_word >> 16;
  int i0 = *idx;
  for (int k = 0; k < count; ++k) {
    const float f = float(fr) * kFracScale;
    for (int c = 0; c < channels; ++c) {
      const float* s = src + size_t(c) * src_stride;
      const float s0 = s[i0], s1 = s[i0 + 1];
      dst[size_t(c) * dst_stride + k] = GuestFmadds(s1 - s0, f, s0);
    }
    const uint32_t a = fr + step;
    i0 += int(a >> 16);
    fr = a & 0xFFFF;
  }
  *frac_word = fr << 16;
  *idx = i0;
}

bool Resampler::Process(float* out, const float* in, int n_out, int n_in, float r) {
  if (n_out < 0) return false;
  if (r != rate) SetRate(r);
  const int H = history_taps;
  auto IN = [&](int c, int j) { return in[size_t(c) * n_in + j]; };
  if (step == 0x10000) {
    off = 0;
    const int m = std::min(H, n_in);
    for (int j = 0; j < m; ++j)
      for (int c = 0; c < channels; ++c) Hist(c, H - m + j) = IN(c, n_in - m + j);
    if (!primed) primed = 1;
    return false;
  }
  // (1) append the head of the new input to the history window
  if (!primed) {
    for (int k = 0; k < lead; ++k)
      for (int c = 0; c < channels; ++c) Hist(c, k) = 0.0f;
    const int m = std::min(kWindow - lead, n_in);
    for (int j = 0; j < m; ++j)
      for (int c = 0; c < channels; ++c) Hist(c, lead + j) = IN(c, j);
  } else {
    const int m = std::min(8 - off, n_in);
    for (int j = 0; j < m; ++j)
      for (int c = 0; c < channels; ++c) Hist(c, H + off + j) = IN(c, j);
  }
  // (2) interpolate, first from the history window, then from the input
  const uint32_t frac0 = frac;
  const int consumed = int((step * uint32_t(n_out) + frac0) >> 16);
  const float* hsrc = history.data() + lead;
  int idx = 0;
  uint32_t frw = frac0 << 16;
  int n_hist = Search(8, rate_eff, false);
  int keep = 0;
  if (n_out <= n_hist) {
    n_hist = n_out;
    const int top = off + H;
    if (consumed < top) keep = top - consumed;
  }
  Kernel(hsrc, out, n_hist, &idx, &frw, kWindow, n_out);
  const int rest = n_out - n_hist;
  const float* tail;
  if (rest > 0) {
    const float* isrc = in + (idx - off - H);
    int idx2 = 0;
    Kernel(isrc, out + n_hist, rest, &idx2, &frw, n_in, n_out);
    tail = isrc + (idx2 - lead);
  } else if (keep == 0) {
    tail = in + (idx - off - H);
  } else {
    for (int j = 0; j < keep; ++j)
      for (int c = 0; c < channels; ++c) Hist(c, j) = Hist(c, consumed + j);
    tail = in;
  }
  // (3) bookkeeping, as Skip
  int extra = 0;
  if (!primed) {
    primed = 1;
    extra = H - lead;
  }
  int adv = consumed + extra;
  if (adv < 0) adv = 0;
  off = off + n_in - adv;
  if (off < -H) off = -H;
  // (4) refill the history tail for the next call
  const int m = std::min(H - keep + off, n_in);
  for (int j = 0; j < m; ++j)
    for (int c = 0; c < channels; ++c) Hist(c, keep + j) = tail[size_t(c) * n_in + j];
  frac = frw >> 16;
  return true;
}

}  // namespace native_audio
