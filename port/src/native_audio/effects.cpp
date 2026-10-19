#include "native_audio/effects.h"

#include <bit>
#include <cmath>
#include <cstring>

#include "native_audio/rw_tables.h"

namespace native_audio {

namespace {

float F(uint32_t bits) { return std::bit_cast<float>(bits); }

// fctiwz: truncate toward zero, saturating.
int32_t Fctiwz(float x) {
  if (!(x == x)) return INT32_MIN;
  if (x >= 2147483648.0f) return INT32_MAX;
  if (x < -2147483648.0f) return INT32_MIN;
  return int32_t(x);
}

int32_t RoundHalfAway(float x) { return Fctiwz(x >= 0.0f ? x + 0.5f : x - 0.5f); }

// log10 as the game computes it (sub_827A3EC0): ln in double times 1/ln(10).
float Log10(float x) {
  return float(std::log(double(x)) * std::bit_cast<double>(0x3FDBCB7B1526E50Eull));
}

int Mod(int v, int m) {
  v %= m;
  return v < 0 ? v + m : v;
}

}  // namespace

// ================================================================ ReverbModel1

Reverb::Reverb(int ch) : channels(ch) {
  ap_count = ch == 1 ? 1 : (ch == 2 || ch == 4) ? 2 : 3;
}

void Reverb::Rooms() {
  float x = p1;
  if (x > F(0x42A6999A) || x < 2.0f) {
    x = (x > F(0x42A6999A)) ? F(0x42A6999A) : 2.0f;
    p1 = x;
  }
  float mn = x * F(0x3F4CCCCD);
  float mx = mn * 1.5f;
  if (mx > 100.0f) {
    p1 = F(0x42A6AAAA);
    mx = 100.0f;
    mn = F(0x42855555);
  }
  const float st = (mx - mn) * F(0x3E4CCCCD);
  t[0] = mn;
  t[1] = mn + st;
  t[2] = t[1] + st;
  t[3] = t[2] + st;
  t[4] = t[3] + st;
  t[5] = mx;
}

void Reverb::Lengths(float rate) {
  lengths[5] = 0;
  int k = 0;
  for (int i = 0; i < 6; ++i) {
    const float f11 = t[i] * F(0x3B3E11D2);
    float r, scale;
    if (rate > 48000.0f) {
      r = 48000.0f;
      scale = rate * F(0x37AEC33E);
    } else {
      r = rate;
      scale = 1.0f;
    }
    const float s = r * f11;
    while (k < 1652 && !(F(rw_tables::kPrimes[k]) > s)) ++k;
    if (k < 1652) {
      lengths[i] = int(F(rw_tables::kPrimes[k]));
      ++k;
    }
    if (scale > 1.0f) lengths[i] = Fctiwz(float(lengths[i]) * scale);
  }
}

void Reverb::Damping(float rate) {
  const uint32_t* rec = rw_tables::kDampRecords;  // 3 x {x[9], y[9]}
  int seg;
  float f;
  if (rate >= 50000.0f) {
    seg = 1;
    f = (50000.0f - 50000.0f) * F(0x3827C5AC);
  } else if (rate > 25000.0f) {
    seg = 1;
    f = (50000.0f - rate) * F(0x3827C5AC);
  } else {
    const float r0 = rate < 10000.0f ? 10000.0f : rate;
    seg = 0;
    f = (25000.0f - r0) * F(0x388BCF65);
  }
  const float g = 1.0f - f;
  float Y[9];
  for (int k = 0; k < 9; ++k)
    Y[k] = Fmadds(F(rec[(seg + 1) * 18 + 9 + k]), g, F(rec[seg * 18 + 9 + k]) * f);
  const float* x = nullptr;
  float xs[9];
  for (int k = 0; k < 9; ++k) xs[k] = F(rec[k]);
  x = xs;
  const float inv = 1.0f / (x[1] - x[0]);
  for (int i = 0; i < 6; ++i) {
    int idx = 1;
    while (t[i] > x[idx]) ++idx;
    const float u = (x[idx] - t[i]) * inv;
    const float v = 1.0f - u;
    a[i] = Fmadds(Y[idx], v, Y[idx - 1] * u);
  }
  if (p2 != prev_p2) {
    const float lim = a[5] + F(0x3A83126F);
    if (p2 < lim) p2 = lim;
    for (int i = 0; i < 6; ++i) {
      if (!comb[i].buf.empty()) std::fill(comb[i].buf.begin(), comb[i].buf.end(), 0.0f);
      comb[i].pos = 0;
      a[i] = a[i] / p2;
    }
  }
}

void Reverb::Feedback() {
  if (p0 < F(0x3EBB645A)) p0 = F(0x3EBB645A);
  const float d = 1.0f - F(0x3EBB645A) / p0;
  for (int i = 0; i < 6; ++i) b[i] = (1.0f - a[i]) * d;
}

void Reverb::Tail() {
  const float l5 = float(lengths[5]);
  const float lc = Log10(b[0]);
  const float p = l5 / lc;
  const float tc = Fnmsubs(p, 10.0f, l5);
  float gm = 0.0f;
  int lm = 0;
  for (int i = 0; i < ap_count; ++i) {
    if (ap[i].g > gm) gm = ap[i].g;
    if (ap[i].len > lm) lm = ap[i].len;
  }
  const float flm = float(lm);
  const float ta = Fnmsubs(flm / Log10(gm), 10.0f, flm);
  tail = ta + tc;
}

bool Reverb::Update(float rate) {
  bool room_changed = false, rate_changed = false;
  if (p1 != prev_p1) {
    Rooms();
    Lengths(rate);
    Damping(rate);
    Feedback();
    room_changed = true;
  } else if (rate != cached_rate) {
    Lengths(rate);
    Damping(rate);
    Feedback();
    rate_changed = true;
  } else {
    if (p2 != prev_p2) Damping(rate);
    Feedback();
  }
  for (int i = 0; i < 6; ++i) {
    Comb& c = comb[i];
    c.len = lengths[i];
    c.c4 = -a[i];
    c.c8 = -b[i];
    c.c12 = -a[i];
    if (int(c.buf.size()) != lengths[i] + 2) {
      c.buf.assign(size_t(lengths[i] + 2), 0.0f);
      c.pos = 0;
    }
  }
  if (ap_init && !rate_changed) {
    if (room_changed)
      for (int i = 0; i < ap_count; ++i) {
        std::fill(ap[i].buf.begin(), ap[i].buf.end(), 0.0f);
        ap[i].pos = 0;
      }
  } else {
    static const uint32_t kC[3][3][2] = {
        {{0x3BC49BA6, 0x3F333333}, {0, 0}, {0, 0}},
        {{0x3BDA740E, 0x3F2147AE}, {0x3BB0F27B, 0x3F471C72}, {0, 0}},
        {{0x3BDA740E, 0x3F2147AE}, {0x3BC49BA6, 0x3F333333}, {0x3BB0F27B, 0x3F471C72}}};
    const int row = ap_count == 1 ? 0 : ap_count == 2 ? 1 : 2;
    for (int i = 0; i < ap_count; ++i) {
      ap[i].len = RoundHalfAway(rate * F(kC[row][i][0]));
      ap[i].g = F(kC[row][i][1]);
      ap[i].buf.assign(size_t(ap[i].len + 1), 0.0f);
      ap[i].pos = 0;
    }
    ap_init = true;
  }
  prev_p0 = p0;
  prev_p1 = p1;
  prev_p2 = p2;
  cached_rate = rate;
  Tail();
  return true;
}

void Reverb::CombBlock(Comb& c, const float* in, float* out, bool overwrite) {
  const float k = F(0x3E2AAAAB);
  const int size = int(c.buf.size());
  float* w = c.buf.data();
  for (int n = 0; n < kBlock; ++n) {
    const int p = c.pos;
    const float tt = Fnmsubs(w[Mod(p - 1, size)], c.c4, in[n]);
    const float wn = Fnmsubs(w[Mod(p - c.len, size)], c.c8, tt);
    w[p] = wn;
    const float y = Fmadds(w[Mod(p - c.len - 1, size)], c.c12, w[Mod(p - c.len, size)]);
    out[n] = overwrite ? y * k : Fmadds(y, k, out[n]);
    c.pos = p + 1 >= size ? 0 : p + 1;
  }
}

void Reverb::AllpassBlock(Allpass& p, const float* in, float* out) {
  float chan = float(channels);
  if (channels > 4) chan = chan - 1.0f;
  const float scale = 2.0f / chan;
  const int size = int(p.buf.size());
  float* v = p.buf.data();
  for (int n = 0; n < kBlock; ++n) {
    const int q = p.pos;
    const float vn = Fnmsubs(v[Mod(q - p.len, size)], p.g, in[n]);
    v[q] = vn;
    const float y = Fmadds(vn, p.g, v[Mod(q - p.len, size)]);
    out[n] = y * scale;
    p.pos = q + 1 >= size ? 0 : q + 1;
  }
}

int Reverb::Process(Io& io, float rate) {
  ScalarScope fpu;
  if (!(p0 > 0.0f)) {
    if (prev_p0 > 0.0f) {
      for (Comb& c : comb) {
        std::fill(c.buf.begin(), c.buf.end(), 0.0f);
        c.pos = 0;
      }
      for (int i = 0; i < ap_count; ++i) {
        std::fill(ap[i].buf.begin(), ap[i].buf.end(), 0.0f);
        ap[i].pos = 0;
      }
    }
    prev_p0 = p0;
    return 0;
  }
  if (p0 != prev_p0 || p1 != prev_p1 || p2 != prev_p2 || rate != cached_rate)
    if (!Update(rate)) return 0;
  float* in = io.Cur();
  float* s = io.Next();
  CombBlock(comb[0], in, s, true);
  for (int i = 1; i < 6; ++i) CombBlock(comb[i], in, s, false);
  switch (channels) {
    case 1:
      AllpassBlock(ap[0], s, in);
      return 1;
    case 2:
      AllpassBlock(ap[1], s, s + 256);
      AllpassBlock(ap[0], s, s);
      break;
    case 4:
      AllpassBlock(ap[0], s, s + 512);
      AllpassBlock(ap[1], s, s + 768);
      std::memcpy(s, s + 512, 1024);
      std::memcpy(s + 256, s + 768, 1024);
      break;
    default:
      AllpassBlock(ap[0], s, s + 768);
      AllpassBlock(ap[1], s, s + 256);
      AllpassBlock(ap[2], s, s + 1024);
      std::memcpy(s, s + 768, 1024);
      std::memcpy(s + 512, s + 1024, 1024);
      std::memset(s + 1280, 0, 1024);
      break;
  }
  io.Toggle();
  return 1;
}

// ================================================================ Delay

void Delay::Grow(int n) {
  const int new_size = n + 1;
  std::vector<float> nb(size_t(channels) * new_size, 0.0f);
  if (!buf.empty() && state != 0) {
    for (int c = 0; c < channels; ++c) {
      const float* o = buf.data() + size_t(c) * size;
      float* d = nb.data() + size_t(c) * new_size;
      int j = 0;
      for (int i = pos + 1; i < size; ++i) d[j++] = o[i];
      for (int i = 0; i <= pos; ++i) d[j++] = o[i];
    }
    pos = size - 1;
  } else {
    pos = 0;
  }
  buf.swap(nb);
  size = new_size;
}

void Delay::UpdateTail() {
  int n;
  float fb;
  if (state != 3) {
    n = cur_n;
    fb = cur_fb;
  } else {
    n = cur_n > old_n ? cur_n : old_n;
    fb = std::fabs(cur_fb) > std::fabs(old_fb) ? cur_fb : old_fb;
  }
  if (n == tail_n && fb == tail_fb) return;
  tail_n = n;
  tail_fb = fb;
  const float d = float(n);
  tail20 = d;
  tail24 = (fb == 0.0f) ? d : Fnmsubs(d / Log10(std::fabs(fb)), 5.0f, d);
}

int Delay::Process(Io& io, float rate) {
  ScalarScope fpu;
  if (rate != cached_rate) {
    if (!buf.empty()) {
      std::fill(buf.begin(), buf.end(), 0.0f);
      pos = 0;
    }
    cached_rate = rate;
    state = 0;
  }
  const float tt = p0;
  const int n = Fctiwz(tt * cached_rate);
  float fb = -p1;
  if (std::fabs(fb) > F(0x3F7D70A4)) {
    if (fb > 0.0f) {
      fb = F(0x3F7D70A4);
      p1 = -F(0x3F7D70A4);
    } else {
      p1 = F(0x3F7D70A4);
      fb = -F(0x3F7D70A4);
    }
  }
  switch (state) {
    case 0:
      if (!(tt > 0.0f)) return 1;
      cur_t = tt;
      cur_fb = fb;
      cur_n = n;
      if (n > size - 1) Grow(n);
      state = 1;
      break;
    case 1:
      if (tt == cur_t && fb == cur_fb) break;
      old_t = cur_t;
      new_t = tt;
      new_fb = fb;
      old_fb = cur_fb;
      new_n = n;
      old_n = cur_n;
      if (n > size - 1) {
        Grow(n);
        cnt = n - old_n + 1;
        if (cnt < 256) {
          cur_t = tt;
          cur_fb = fb;
          cur_n = n;
          state = 3;
        } else {
          state = 2;
        }
      } else {
        cur_t = tt;
        cur_fb = fb;
        cnt = 0;
        cur_n = n;
        state = 3;
      }
      break;
    case 2:
      if (cnt < 256) {
        cur_t = new_t;
        cur_fb = new_fb;
        cur_n = new_n;
        state = 3;
      }
      break;
    default:
      break;
  }
  const float* in = io.Cur();
  float* out = io.Next();
  if (state == 1 || state == 2) {
    // Segments as sub_8264EC80 cuts them; the write position wraps at the
    // start of a segment, so it can be left equal to size after the block.
    int done = 0;
    while (done < kBlock) {
      if (pos >= size) pos = 0;
      int rd = pos - cur_n, count;
      if (rd >= 0) count = size - pos;
      else { rd += size; count = size - rd; }
      if (count > kBlock - done) count = kBlock - done;
      for (int c = 0; c < channels; ++c) {
        float* b = buf.data() + size_t(c) * size;
        for (int k = 0; k < count; ++k) {
          b[pos + k] = Fnmsubs(b[rd + k], cur_fb, in[c * kBlock + done + k]);
          out[c * kBlock + done + k] = b[rd + k];
        }
      }
      pos += count;
      done += count;
    }
    if (state == 2) cnt -= 256;
  } else {
    // make_ramp(ramp, 256, 1.0, 0.0, -cnt, 128) (sub_82659F98)
    if (ramp.empty()) ramp.assign(256, 0.0f);
    const float step = (0.0f - 1.0f) / 128.0f;
    for (int i = 0; i < kBlock; ++i) {
      const int tt2 = -cnt + i;
      if (tt2 < 0) ramp[i] = 1.0f;
      else if (tt2 <= 127) ramp[i] = Fnmsubs(128.0f - float(tt2 + 1), step, 0.0f);
      else ramp[i] = 0.0f;
    }
    // Segments as sub_8264F140 cuts them (no wrap of the write or either read
    // inside a segment), kernel sub_8264ED28 per segment: unrolled by 4 while
    // i + 3 < count, and the original compiler reassociated lane 2 as
    // (old * r) * oldFb instead of (old * oldFb) * r. The remainder loop uses
    // the normal order.
    int done = 0;
    while (done < kBlock) {
      if (pos >= size) pos = 0;
      const int rem = kBlock - done;
      int rn = pos - cur_n, cnt_n;
      if (rn >= 0) cnt_n = size - pos;
      else { rn += size; cnt_n = size - rn; }
      if (cnt_n > rem) cnt_n = rem;
      int ro = pos - old_n, cnt_o;
      if (ro >= 0) cnt_o = size - pos;
      else { ro += size; cnt_o = size - ro; }
      if (cnt_o > rem) cnt_o = rem;
      const int count = cnt_o < cnt_n ? cnt_o : cnt_n;
      for (int c = 0; c < channels; ++c) {
        float* b = buf.data() + size_t(c) * size;
        const float* x = in + c * kBlock + done;
        float* y = out + c * kBlock + done;
        const float* rr = ramp.data() + done;
        int i = 0;
        auto one = [&](int k, bool lane2) {
          const float r = rr[k];
          const float om = 1.0f - r;
          const float old = b[ro + k];
          const float cc = lane2 ? (old * r) * old_fb : (old * old_fb) * r;
          const float bn = b[rn + k] * om;
          const float d = Fmadds(bn, cur_fb, cc);
          b[pos + k] = x[k] - d;
          const float e = b[ro + k] * r;
          y[k] = Fmadds(b[rn + k], om, e);
        };
        for (; i < count - 3; i += 4) {
          one(i, false);
          one(i + 1, false);
          one(i + 2, true);
          one(i + 3, false);
        }
        for (; i < count; ++i) one(i, false);
      }
      pos += count;
      done += count;
    }
    cnt -= 256;
    if (-cnt >= 128) state = 1;
  }
  io.Toggle();
  if (state == 1 && !(cur_t > 0.0f)) {
    std::fill(buf.begin(), buf.end(), 0.0f);
    pos = 0;
    state = 0;
  }
  UpdateTail();
  return 1;
}

}  // namespace native_audio
