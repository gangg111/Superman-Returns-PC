// Differential self-test of the native audio DSP against the recompiled
// originals, in the game process (the originals need guest memory, nothing
// else: no game state, no audio device). docs/natywny-dzwiek-plan.md,
// "Metoda weryfikacji DSP" point 1.
//
// SR_AUDIO_SELFTEST=<report file> runs it right after setup, writes the
// report and exits the process. Each case runs the same random sequence of
// calls on the guest object (through sub_XXXXXXXX) and on the native one,
// and compares every return value, output sample (bit for bit) and the
// whole state after every call.

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "generated/default/superman_returns_init.h"
#include "native_audio/effects.h"
#include "native_audio/plugins.h"
#include "native_audio/resampler.h"

namespace native_audio {
namespace {
// While the self-test runs there is no System (and no allocator object at
// System+28): the RW allocator hooks below serve the test from the guest heap.
bool g_selftest_alloc = false;
}  // namespace
}  // namespace native_audio

// rw::audio::core allocator: (sys, size, name, align, allocator or 0) -> ptr
REX_HOOK_RAW(sub_8264CBB8) {
  if (!native_audio::g_selftest_alloc) {
    __imp__sub_8264CBB8(ctx, base);
    return;
  }
  const uint32_t align = ctx.r6.u32 < 16 ? 16 : ctx.r6.u32;
  ctx.r3.u64 = REX_KERNEL_MEMORY()->SystemHeapAlloc(ctx.r4.u32 ? ctx.r4.u32 : 16, align);
}

// rw::audio::core free: (sys, ptr, 0)
REX_HOOK_RAW(sub_8264C8F0) {
  if (!native_audio::g_selftest_alloc) {
    __imp__sub_8264C8F0(ctx, base);
    return;
  }
  if (ctx.r4.u32) REX_KERNEL_MEMORY()->SystemHeapFree(ctx.r4.u32);
}

namespace native_audio {

namespace {

struct Guest {
  uint8_t* base;
  uint32_t stack_top;

  uint32_t Alloc(uint32_t size) { return REX_KERNEL_MEMORY()->SystemHeapAlloc(size, 128); }
  void W32(uint32_t a, uint32_t v) { *reinterpret_cast<uint32_t*>(base + a) = __builtin_bswap32(v); }
  uint32_t R32(uint32_t a) const { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + a)); }
  void WF(uint32_t a, float f) { W32(a, std::bit_cast<uint32_t>(f)); }
  float RF(uint32_t a) const { return std::bit_cast<float>(R32(a)); }

  PPCContext Ctx() const {
    PPCContext ctx{};
    ctx.r1.u64 = stack_top;
    // A zero csr would be written to MXCSR on the first flush-mode switch and
    // unmask every FP exception (STATUS_FLOAT_INEXACT_RESULT on the next op).
    ctx.fpscr.InitHost();
    return ctx;
  }
};

struct Report {
  std::FILE* f;
  int cases = 0, failures = 0;
  // Coverage of the resampler paths actually exercised.
  long outputs_for = 0, processed = 0, passthrough = 0, short_reads = 0, history_only = 0;
  void Fail(const std::string& what) {
    if (failures < 40) std::fprintf(f, "  MISMATCH %s\n", what.c_str());
    ++failures;
  }
};

// Guest resampler object (spec-pitch-resampler.md 4.1) built by hand, so the
// test needs no System/allocator: header 32 bytes, history from +32.
uint32_t MakeGuestResampler(Guest& g, int ch) {
  const uint32_t rs = g.Alloc(32 + ch * Resampler::kWindow * 4 + 64);
  std::memset(g.base + rs, 0, 32 + ch * Resampler::kWindow * 4);
  g.WF(rs + 8, -1.0f);
  g.WF(rs + 12, -1.0f);
  g.W32(rs + 20, rs + 32);
  g.base[rs + 24] = uint8_t(ch);
  g.base[rs + 27] = Resampler::kHistory;
  return rs;
}

void CompareState(Guest& g, uint32_t rs, const Resampler& n, Report& rep, const char* after) {
  char buf[256];
  auto chk = [&](const char* name, uint32_t gv, uint32_t nv) {
    if (gv != nv) {
      std::snprintf(buf, sizeof(buf), "after %s: %s guest %08X native %08X", after, name, gv, nv);
      rep.Fail(buf);
    }
  };
  chk("step", g.R32(rs + 0), n.step);
  chk("frac", g.R32(rs + 4), n.frac);
  chk("rate", g.R32(rs + 8), std::bit_cast<uint32_t>(n.rate));
  chk("rate_eff", g.R32(rs + 12), std::bit_cast<uint32_t>(n.rate_eff));
  chk("off", g.R32(rs + 16), uint32_t(n.off));
  chk("primed", g.base[rs + 25], n.primed);
  chk("lead", g.base[rs + 26], n.lead);
  for (size_t i = 0; i < n.history.size(); ++i)
    chk("history", g.R32(rs + 32 + uint32_t(i) * 4), std::bit_cast<uint32_t>(n.history[i]));
}

float RandomRate(std::mt19937& rng) {
  static const float fixed[] = {1.0f, 44100.0f / 48000.0f, 22050.0f / 48000.0f,
                                32000.0f / 48000.0f, 1.000005f, 0.5f, 2.0f, 4.0f, 4.5f, 0.0f};
  std::uniform_int_distribution<int> pick(0, 19);
  const int k = pick(rng);
  if (k < 10) return fixed[k];
  std::uniform_real_distribution<float> u(0.05f, 5.0f);
  return u(rng);
}

// control: perturb one native input sample by one ulp; the case must then fail
// (proves the comparison can see a difference at all).
void ResamplerCase(Guest& g, Report& rep, int ch, uint32_t seed, bool control = false) {
  std::mt19937 rng(seed);
  Resampler nat(ch);
  const uint32_t rs = MakeGuestResampler(g, ch);
  // Input buffers with a guard band on both sides (the kernels read src[idx+1]
  // and index the input relative to the history offset).
  constexpr int kMaxIn = 1200, kGuard = 64, kMaxOut = 256;
  const uint32_t in_bytes = (kMaxIn * ch + 2 * kGuard) * 4;
  const uint32_t g_in = g.Alloc(in_bytes), g_out = g.Alloc((kMaxOut * ch + kGuard) * 4);
  std::vector<float> n_in(kMaxIn * ch + 2 * kGuard), n_out(kMaxOut * ch + kGuard);
  std::uniform_real_distribution<float> sample(-1.0f, 1.0f);
  std::uniform_int_distribution<int> op_pick(0, 9), out_pick(0, kMaxOut);
  float rate = RandomRate(rng);
  char what[160];
  for (int step = 0; step < 400; ++step) {
    if (op_pick(rng) < 2) rate = RandomRate(rng);
    const int op = op_pick(rng);
    PPCContext ctx = g.Ctx();
    ctx.r3.u64 = rs;
    ctx.f1.f64 = double(rate);
    if (op == 0) {  // OutputsFor
      const int n = out_pick(rng) * 3;
      ctx.r4.s64 = n;
      sub_8265D350(ctx, g.base);
      const int nv = nat.OutputsFor(n, rate);
      ++rep.outputs_for;
      if (int32_t(ctx.r3.u32) != nv) {
        std::snprintf(what, sizeof(what), "OutputsFor(%d, %g): guest %d native %d", n, rate,
                      int32_t(ctx.r3.u32), nv);
        rep.Fail(what);
      }
      CompareState(g, rs, nat, rep, "OutputsFor");
      continue;
    }
    // The planner's pattern: inputs for n_out, skip on a snapshot, then process.
    const int want_out = op == 1 ? out_pick(rng) : kMaxOut;
    ctx.r4.s64 = want_out;
    sub_8265C970(ctx, g.base);
    const int need_g = int32_t(ctx.r3.u32);
    const int need_n = nat.InputsFor(rate, want_out);
    if (need_g != need_n) {
      std::snprintf(what, sizeof(what), "InputsFor(%g, %d): guest %d native %d", rate, want_out,
                    need_g, need_n);
      rep.Fail(what);
      return;
    }
    CompareState(g, rs, nat, rep, "InputsFor");
    if (need_n > kMaxIn || rate <= 0.0f) continue;
    // A short read (decoder ran out) now and then: n_in < need.
    int n_in_count = need_n;
    int n_out_count = want_out;
    if (op == 2 && need_n > 2) {
      n_in_count = need_n / 2;
      PPCContext c2 = g.Ctx();
      c2.r3.u64 = rs;
      c2.r4.s64 = n_in_count;
      c2.f1.f64 = double(rate);
      sub_8265D350(c2, g.base);
      n_out_count = int32_t(c2.r3.u32);
      const int nv = nat.OutputsFor(n_in_count, rate);
      if (n_out_count != nv) {
        std::snprintf(what, sizeof(what), "OutputsFor(%d): guest %d native %d", n_in_count,
                      n_out_count, nv);
        rep.Fail(what);
        return;
      }
      if (n_out_count > kMaxOut) continue;
      ++rep.short_reads;
    }
    // Fill input (planar, stride n_in_count) identically on both sides.
    for (size_t i = 0; i < n_in.size(); ++i) {
      float v = sample(rng);
      if ((i % 97) == 0) v = 1e-41f;  // a denormal now and then
      n_in[i] = v;
      g.WF(g_in + uint32_t(i) * 4, v);
    }
    if (control) n_in[kGuard] = std::nextafter(n_in[kGuard], 2.0f);
    std::fill(n_out.begin(), n_out.end(), 0.0f);
    std::memset(g.base + g_out, 0, n_out.size() * 4);
    PPCContext c3 = g.Ctx();
    c3.r3.u64 = rs;
    c3.r4.u64 = g_out;
    c3.r5.u64 = g_in + kGuard * 4;
    c3.r6.s64 = n_out_count;
    c3.r7.s64 = n_in_count;
    c3.f1.f64 = double(rate);
    sub_8265CCB0(c3, g.base);
    const bool gw = (c3.r3.u32 & 0xFF) != 0;
    const bool nw = nat.Process(n_out.data(), n_in.data() + kGuard, n_out_count, n_in_count, rate);
    if (gw != nw) {
      std::snprintf(what, sizeof(what), "Process(%d, %d, %g) wrote: guest %d native %d", n_out_count,
                    n_in_count, rate, gw, nw);
      rep.Fail(what);
      return;
    }
    if (gw) ++rep.processed; else ++rep.passthrough;
    if (gw && n_out_count <= 8) ++rep.history_only;
    if (gw) {
      for (int i = 0; i < n_out_count * ch; ++i) {
        const uint32_t gv = g.R32(g_out + uint32_t(i) * 4);
        const uint32_t nv = std::bit_cast<uint32_t>(n_out[i]);
        if (gv != nv) {
          std::snprintf(what, sizeof(what), "Process(%d, %d, %g) out[%d]: guest %08X native %08X",
                        n_out_count, n_in_count, rate, i, gv, nv);
          rep.Fail(what);
          return;
        }
      }
    }
    CompareState(g, rs, nat, rep, "Process");
  }
}

// ------------------------------------------------------------ plug-ins

// Guest scaffolding for one plug-in instance: a fake System (+12 mixer rate,
// +8 block period), a fake Voice (+32 System, +44 rate at voice start), an io
// object with two preallocated 6-channel buffers (no reallocation ever), and
// the instance made by the class's own GetSize/Create.
struct GuestPlugin {
  Guest& g;
  uint32_t sys = 0, voice = 0, io = 0, buf[2] = {}, obj = 0;

  GuestPlugin(Guest& gg, uint32_t desc, int in_ch, int out_ch,
              void (*get_size)(PPCContext&, uint8_t*), void (*create)(PPCContext&, uint8_t*))
      : g(gg) {
    sys = g.Alloc(256);
    std::memset(g.base + sys, 0, 256);
    g.WF(sys + 12, 48000.0f);
    g.WF(sys + 8, 256.0f / 48000.0f);
    voice = g.Alloc(256);
    std::memset(g.base + voice, 0, 256);
    g.W32(voice + 32, sys);
    g.WF(voice + 44, 1.0f);
    io = g.Alloc(64);
    std::memset(g.base + io, 0, 64);
    for (int i = 0; i < 2; ++i) {
      buf[i] = g.Alloc(6 * 1024);
      std::memset(g.base + buf[i], 0, 6 * 1024);
      g.W32(io + 4 + 4 * i, buf[i]);
      g.W32(io + 20 + 4 * i, 6 * 1024);
    }
    g.WF(io + 12, 1.0f);
    g.WF(io + 16, 1.0f);
    const uint32_t entry = g.Alloc(16);
    std::memset(g.base + entry, 0, 16);
    g.W32(entry + 4, desc);
    g.base[entry + 8] = uint8_t(out_ch);
    PPCContext c = g.Ctx();
    c.r3.u64 = entry;
    get_size(c, g.base);
    const uint32_t size = c.r3.u32;
    obj = g.Alloc(size + 256);
    std::memset(g.base + obj, 0, size + 256);
    c = g.Ctx();
    c.r3.u64 = voice;
    c.r4.u64 = entry;
    c.r5.u64 = uint32_t(in_ch);
    c.r6.u64 = obj;
    create(c, g.base);
    g.W32(obj + 8, voice);
  }
  uint8_t Cur() const { return g.base[io + 28]; }
  uint32_t CurBuf() const { return buf[Cur()]; }
  int Call(void (*fn)(PPCContext&, uint8_t*), uint32_t reset = 0) {
    PPCContext c = g.Ctx();
    c.r3.u64 = obj;
    c.r4.u64 = io;
    c.r5.u64 = reset;
    fn(c, g.base);
    return int32_t(c.r3.u32);
  }
};

// Fills the current input buffer on both sides with the same random block.
void FillInput(GuestPlugin& gp, Io& io, int ch, std::mt19937& rng, int kind) {
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  for (int i = 0; i < ch * kBlock; ++i) {
    float v = u(rng);
    if (kind == 1) v *= 1e-38f;      // denormal-heavy
    else if (kind == 2) v = 0.0f;    // silence
    io.Cur()[i] = v;
    gp.g.WF(gp.CurBuf() + uint32_t(i) * 4, v);
  }
}

bool CompareOutput(GuestPlugin& gp, const Io& io, int ch, Report& rep, const char* what) {
  if (gp.Cur() != io.cur) {
    rep.Fail(std::string(what) + ": buffer toggle differs");
    return false;
  }
  for (int i = 0; i < ch * kBlock; ++i) {
    const uint32_t gv = gp.g.R32(gp.CurBuf() + uint32_t(i) * 4);
    const uint32_t nv = std::bit_cast<uint32_t>(io.Cur()[i]);
    if (gv != nv) {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "%s out[%d]: guest %08X native %08X", what, i, gv, nv);
      rep.Fail(buf);
      return false;
    }
  }
  return true;
}

struct NativeIo {
  float storage[2][6 * kBlock + 32];
  Io io;
  NativeIo() {
    // 128-byte aligned like the guest buffers (the VMX path depends on it).
    for (int i = 0; i < 2; ++i)
      io.buf[i] = reinterpret_cast<float*>((uintptr_t(storage[i]) + 127) & ~uintptr_t(127));
  }
};

alignas(128) static NativeIo g_nio;

void FilterCase(Guest& g, Report& rep, bool hp, int ch, uint32_t seed) {
  std::mt19937 rng(seed);
  GuestPlugin gp(g, hp ? 0x82937808 : 0x829377B0, ch, ch, hp ? sub_826636E8 : sub_826633F8,
                 hp ? sub_826637F8 : sub_82663508);
  BiquadFilter nat(hp, ch);
  Io& io = g_nio.io;
  io.cur = 0;
  std::uniform_int_distribution<int> pick(0, 9);
  std::uniform_real_distribution<float> hz(10.0f, 30000.0f);
  const char* name = hp ? "HI20" : "LI20";
  for (int block = 0; block < 60; ++block) {
    if (pick(rng) < 3) {
      const int k = pick(rng);
      const float cutoff = k == 0 ? (hp ? 0.0f : 1000000.0f) : k == 1 ? 23990.0f : hz(rng);
      g.WF(gp.obj + 32, cutoff);
      nat.cutoff = cutoff;
    }
    FillInput(gp, io, ch, rng, pick(rng) == 0 ? 1 : 0);
    // hook, then Process (as the executor does)
    PPCContext c = g.Ctx();
    c.r3.u64 = gp.obj;
    c.r4.u64 = gp.io;
    (hp ? sub_82663610 : sub_82663320)(c, g.base);
    nat.Hook(io, 1.0f);
    const int gr = gp.Call(hp ? sub_82663710 : sub_82663420);
    const uint8_t before = io.cur;
    const int nr = nat.Process(io, 48000.0f);
    char what[64];
    std::snprintf(what, sizeof(what), "%s ch=%d block %d", name, ch, block);
    if (gr != nr) rep.Fail(std::string(what) + ": return differs");
    if (!CompareOutput(gp, io, ch, rep, what)) return;
    if (io.cur != before) ++rep.processed; else ++rep.passthrough;
    const uint32_t coef[6] = {std::bit_cast<uint32_t>(nat.a1), std::bit_cast<uint32_t>(nat.a2),
                              std::bit_cast<uint32_t>(nat.b0), std::bit_cast<uint32_t>(nat.b1),
                              std::bit_cast<uint32_t>(nat.b2), std::bit_cast<uint32_t>(nat.last_w)};
    for (int i = 0; i < 6; ++i)
      if (g.R32(gp.obj + 44 + 4 * i) != coef[i]) {
        std::snprintf(what, sizeof(what), "%s ch=%d block %d coef %d: guest %08X native %08X",
                      name, ch, block, i, g.R32(gp.obj + 44 + 4 * i), coef[i]);
        rep.Fail(what);
        return;
      }
    // Keep both sides on the same input buffer index for the next block.
    io.cur = gp.Cur();
  }
}

void GainCase(Guest& g, Report& rep, int ch, uint32_t seed) {
  std::mt19937 rng(seed);
  GuestPlugin gp(g, 0x82937860, ch, ch, sub_8266BE78, sub_826639D8);
  Gain nat(ch);
  Io& io = g_nio.io;
  io.cur = 0;
  std::uniform_int_distribution<int> pick(0, 9);
  std::uniform_real_distribution<float> lin(0.0f, 2.0f);
  for (int block = 0; block < 60; ++block) {
    if (pick(rng) < 4) {
      const float v = pick(rng) == 0 ? 0.0f : lin(rng);
      g.WF(gp.obj + 32, v);
      nat.gain = v;
    }
    const uint32_t reset = (block == 0 || pick(rng) == 0) ? 1 : 0;
    FillInput(gp, io, ch, rng, pick(rng) == 0 ? 1 : 0);
    const int gr = gp.Call(sub_82663900, reset);
    const int nr = nat.Process(io, reset != 0);
    char what[64];
    std::snprintf(what, sizeof(what), "Gai0 ch=%d block %d", ch, block);
    if (gr != nr) rep.Fail(std::string(what) + ": return differs");
    if (!CompareOutput(gp, io, ch, rep, what)) return;
    if (g.R32(gp.obj + 40) != std::bit_cast<uint32_t>(nat.prev)) {
      rep.Fail(std::string(what) + ": prev gain differs");
      return;
    }
    io.cur = gp.Cur();
  }
}

void PanCase(Guest& g, Report& rep, int in_ch, uint32_t seed) {
  std::mt19937 rng(seed);
  // The runtime copy of the class descriptor is not made yet at self-test
  // time; put the angle scale (descriptor +68 = 2*pi) where Process reads it.
  g.W32(0x8292ED74, 0x40C90FDB);
  g.base[0x8299FDE0] = 0;  // debug orbit off
  GuestPlugin gp(g, 0x82936F30, in_ch, 6, sub_82652028, sub_826529B0);
  PanSurround nat(in_ch, 6);
  char what[96];
  for (int o = 0; o < 5; ++o)
    for (int k = 0; k < 2; ++k)
      if (g.R32(gp.obj + 176 + 80 * o + 64 + 4 * k) != std::bit_cast<uint32_t>(nat.spk[o][k])) {
        std::snprintf(what, sizeof(what), "PnS0 speaker %d.%d: guest %08X native %08X", o, k,
                      g.R32(gp.obj + 176 + 80 * o + 64 + 4 * k),
                      std::bit_cast<uint32_t>(nat.spk[o][k]));
        rep.Fail(what);
        return;
      }
  Io& io = g_nio.io;
  io.cur = 0;
  std::uniform_int_distribution<int> pick(0, 9);
  std::uniform_real_distribution<float> dist(0.0f, 40.0f), az(-360.0f, 360.0f), wid(0.0f, 10.0f);
  for (int block = 0; block < 60; ++block) {
    if (pick(rng) < 4) {
      const int which = pick(rng) % 3;
      const float v = which == 0 ? dist(rng) : which == 1 ? az(rng) : wid(rng);
      g.WF(gp.obj + 32 + 8 * which, v);
      (which == 0 ? nat.distance : which == 1 ? nat.azimuth : nat.half_width) = v;
    }
    FillInput(gp, io, in_ch, rng, pick(rng) == 0 ? 1 : 0);
    const int gr = gp.Call(sub_82652690);
    const int nr = nat.Process(io);
    std::snprintf(what, sizeof(what), "PnS0 in=%d block %d", in_ch, block);
    if (gr != nr) rep.Fail(std::string(what) + ": return differs");
    if (!CompareOutput(gp, io, 6, rep, what)) return;
    ++rep.processed;
    for (int o = 0; o < 5; ++o)
      for (int i = 0; i < 6; ++i)
        if (g.R32(gp.obj + 176 + 80 * o + 4 + 4 * i) != std::bit_cast<uint32_t>(nat.cur[o][i])) {
          std::snprintf(what, sizeof(what), "PnS0 in=%d block %d gain[%d][%d]: guest %08X native %08X",
                        in_ch, block, o, i, g.R32(gp.obj + 176 + 80 * o + 4 + 4 * i),
                        std::bit_cast<uint32_t>(nat.cur[o][i]));
          rep.Fail(what);
          return;
        }
    io.cur = gp.Cur();
  }
}

// A bus (Sub0 built by hand per spec 5.1: +29 channels, +32 buffer) fed by
// 1..3 senders (Sen0 made by their own Create, attached through the Control
// handler sub_826623E0), with level changes, resets and re-attachments.
void BusCase(Guest& g, Report& rep, int bus_ch, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> pick(0, 9);
  static const int kCh[4] = {1, 2, 4, 6};
  const uint32_t gbus = g.Alloc(256);
  std::memset(g.base + gbus, 0, 256);
  g.base[gbus + 29] = uint8_t(bus_ch);
  const uint32_t gbuf = g.Alloc(6 * 1024);
  std::memset(g.base + gbuf, 0, 6 * 1024);
  g.W32(gbus + 32, gbuf);
  Bus nbus(bus_ch);
  // The bus voice's own io (Sub0 Process writes its output there).
  GuestPlugin busio(g, 0x829375D8, 1, 1, sub_82662058, sub_82662428);  // only its io is used
  static NativeIo nbusio;
  nbusio.io.cur = 0;
  g.base[busio.io + 28] = 0;

  struct Sender {
    std::unique_ptr<GuestPlugin> gp;
    std::unique_ptr<Send> nat;
    std::unique_ptr<NativeIo> nio;
    int ch;
  };
  std::vector<Sender> senders;
  const int n_send = 1 + pick(rng) % 3;
  const uint32_t rec = g.Alloc(16);
  auto attach = [&](Sender& s, bool on) {
    g.W32(rec + 0, 0x826623E0);
    g.W32(rec + 4, s.gp->obj);
    g.W32(rec + 8, on ? gbus : 0);
    PPCContext c = g.Ctx();
    c.r3.u64 = rec;
    sub_826623E0(c, g.base);
    s.nat->Attach(on ? &nbus : nullptr);
  };
  for (int k = 0; k < n_send; ++k) {
    Sender s;
    s.ch = kCh[pick(rng) % 4];
    s.gp = std::make_unique<GuestPlugin>(g, 0x829375D8, s.ch, s.ch, sub_82662058, sub_82662428);
    s.nat = std::make_unique<Send>(s.ch, false);
    s.nio = std::make_unique<NativeIo>();
    s.nio->io.cur = 0;
    attach(s, true);
    senders.push_back(std::move(s));
  }
  std::uniform_real_distribution<float> lvl(0.0f, 1.5f);
  char what[96];
  for (int block = 0; block < 50; ++block) {
    for (size_t k = 0; k < senders.size(); ++k) {
      Sender& s = senders[k];
      if (pick(rng) == 0) attach(s, pick(rng) < 7);  // detach / re-attach now and then
      if (pick(rng) < 3) {
        const float v = lvl(rng);
        g.WF(s.gp->obj + 40, v);
        s.nat->level = v;
      }
      const uint32_t reset = (block == 0 || pick(rng) == 0) ? 1 : 0;
      FillInput(*s.gp, s.nio->io, s.ch, rng, 0);
      PPCContext c = g.Ctx();
      c.r3.u64 = s.gp->obj;
      c.r4.u64 = s.gp->io;
      sub_826620D8(c, g.base);
      s.nat->Hook(s.nio->io);
      const int gr = s.gp->Call(sub_82662100, reset);
      const int nr = s.nat->Process(s.nio->io, reset != 0);
      if (gr != nr || g.R32(s.gp->obj + 88) != std::bit_cast<uint32_t>(s.nat->prev)) {
        std::snprintf(what, sizeof(what), "Sen0 %d->%d block %d: return or prev level differs",
                      s.ch, bus_ch, block);
        rep.Fail(what);
        return;
      }
    }
    // Sub0 Process on the hand-made bus object, output into the bus voice's io.
    PPCContext c = g.Ctx();
    c.r3.u64 = gbus;
    c.r4.u64 = busio.io;
    c.r5.u64 = 0;
    sub_826513E8(c, g.base);
    const int gret = int32_t(c.r3.u32);
    const int nret = nbus.Process(nbusio.io);
    std::snprintf(what, sizeof(what), "Sub0 %dch block %d (%d senders)", bus_ch, block, n_send);
    if (gret != nret) {
      rep.Fail(std::string(what) + ": return differs");
      return;
    }
    if (gret) {
      if (!CompareOutput(busio, nbusio.io, bus_ch, rep, what)) return;
      ++rep.processed;
    } else {
      ++rep.passthrough;
    }
    nbusio.io.cur = busio.Cur();
  }
}

// Music layout: a 4-channel source through two Route plug-ins into one bus
// (Rou0 by its own Create, configured through the Control handler sub_82661EA0).
void RouteCase(Guest& g, Report& rep, int bus_ch, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> pick(0, 9);
  const uint32_t gbus = g.Alloc(256);
  std::memset(g.base + gbus, 0, 256);
  g.base[gbus + 29] = uint8_t(bus_ch);
  const uint32_t gbuf = g.Alloc(6 * 1024);
  std::memset(g.base + gbuf, 0, 6 * 1024);
  g.W32(gbus + 32, gbuf);
  Bus nbus(bus_ch);
  GuestPlugin r0(g, 0x82937510, 4, 4, sub_82661D30, sub_82661F28);
  GuestPlugin r1(g, 0x82937510, 4, 4, sub_82661D30, sub_82661F28);
  // Both routes read the same source block: give r1 the io of r0.
  Route n0, n1;
  const uint32_t rec = g.Alloc(32);
  auto config = [&](GuestPlugin& gp, Route& nr, bool on) {
    const int count = 1 + pick(rng) % (bus_ch < 4 ? bus_ch : 4);
    const int src = pick(rng) % (4 - count + 1), dst = pick(rng) % (bus_ch - count + 1);
    g.W32(rec + 0, 0x82661EA0);
    g.W32(rec + 4, gp.obj);
    g.W32(rec + 8, on ? gbus : 0);
    g.WF(rec + 12, float(src));
    g.WF(rec + 16, float(dst));
    g.WF(rec + 20, float(count));
    PPCContext c = g.Ctx();
    c.r3.u64 = rec;
    sub_82661EA0(c, g.base);
    nr.Attach(on ? &nbus : nullptr, src, dst, count);
  };
  config(r0, n0, true);
  config(r1, n1, true);
  GuestPlugin busio(g, 0x829375D8, 1, 1, sub_82662058, sub_82662428);
  static NativeIo nbusio;
  nbusio.io.cur = 0;
  g.base[busio.io + 28] = 0;
  Io& io = g_nio.io;
  io.cur = 0;
  char what[96];
  for (int block = 0; block < 50; ++block) {
    if (pick(rng) == 0) {
      const bool first = pick(rng) < 5;
      config(first ? r0 : r1, first ? n0 : n1, pick(rng) < 8);
    }
    FillInput(r0, io, 4, rng, 0);
    PPCContext c = g.Ctx();
    c.r3.u64 = r0.obj;
    c.r4.u64 = r0.io;
    sub_82661D58(c, g.base);
    c = g.Ctx();
    c.r3.u64 = r1.obj;
    c.r4.u64 = r0.io;
    sub_82661D58(c, g.base);
    n0.Process(io);
    n1.Process(io);
    c = g.Ctx();
    c.r3.u64 = gbus;
    c.r4.u64 = busio.io;
    sub_826513E8(c, g.base);
    const int gret = int32_t(c.r3.u32), nret = nbus.Process(nbusio.io);
    std::snprintf(what, sizeof(what), "Rou0 -> %dch block %d", bus_ch, block);
    if (gret != nret) {
      rep.Fail(std::string(what) + ": return differs");
      return;
    }
    if (gret) {
      if (!CompareOutput(busio, nbusio.io, bus_ch, rep, what)) return;
      ++rep.processed;
    } else {
      ++rep.passthrough;
    }
    nbusio.io.cur = busio.Cur();
  }
}

bool CheckWord(Guest& g, uint32_t addr, uint32_t native, Report& rep, const char* what,
               const char* field) {
  if (g.R32(addr) == native) return true;
  char buf[160];
  std::snprintf(buf, sizeof(buf), "%s %s: guest %08X native %08X", what, field, g.R32(addr),
                native);
  rep.Fail(buf);
  return false;
}

void ReverbCase(Guest& g, Report& rep, int ch, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> pick(0, 19);
  GuestPlugin gp(g, 0x82936968, 1, ch, sub_8264DF18, sub_8264E668);
  g.base[gp.voice + 8] = uint8_t(ch);
  Reverb nat(ch);
  Io& io = g_nio.io;
  io.cur = 0;
  std::uniform_real_distribution<float> t(0.0f, 6.0f), room(0.5f, 95.0f), damp(0.3f, 3.0f);
  char what[96];
  for (int block = 0; block < 80; ++block) {
    const int k = pick(rng);
    if (k == 0 || block == 2) {
      const float v = pick(rng) < 3 ? 0.0f : t(rng);
      g.WF(gp.obj + 32, v);
      nat.p0 = v;
    } else if (k == 1) {
      const float v = room(rng);
      g.WF(gp.obj + 40, v);
      nat.p1 = v;
    } else if (k == 2) {
      const float v = damp(rng);
      g.WF(gp.obj + 48, v);
      nat.p2 = v;
    }
    FillInput(gp, io, ch, rng, pick(rng) == 0 ? 2 : 0);
    const int gr = gp.Call(sub_8264E398);
    const int nr = nat.Process(io, 48000.0f);
    std::snprintf(what, sizeof(what), "RM10 %dch block %d", ch, block);
    if (gr != nr) {
      rep.Fail(std::string(what) + ": return differs");
      return;
    }
    if (gr) {
      if (!CompareOutput(gp, io, ch, rep, what)) return;
      ++rep.processed;
    } else {
      ++rep.passthrough;
    }
    if (!CheckWord(g, gp.obj + 32, std::bit_cast<uint32_t>(nat.p0), rep, what, "P0") ||
        !CheckWord(g, gp.obj + 40, std::bit_cast<uint32_t>(nat.p1), rep, what, "P1") ||
        !CheckWord(g, gp.obj + 48, std::bit_cast<uint32_t>(nat.p2), rep, what, "P2") ||
        (gr && !CheckWord(g, gp.obj + 24, std::bit_cast<uint32_t>(nat.tail), rep, what, "tail")))
      return;
    io.cur = gp.Cur();
  }
}

void DelayCase(Guest& g, Report& rep, int ch, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> pick(0, 19);
  GuestPlugin gp(g, 0x829369E0, ch, ch, sub_8264E980, sub_8264F848);
  g.base[gp.voice + 8] = uint8_t(ch);
  Delay nat(ch);
  Io& io = g_nio.io;
  io.cur = 0;
  std::uniform_real_distribution<float> tm(0.0f, 0.4f), fb(-1.3f, 1.3f);
  char what[96];
  int prev_state = 0;
  for (int block = 0; block < 120; ++block) {
    const int k = pick(rng);
    if (k < 2 || block == 1) {
      const float v = pick(rng) < 3 ? 0.0f : pick(rng) < 3 ? tm(rng) * 0.01f : tm(rng);
      g.WF(gp.obj + 32, v);
      nat.p0 = v;
    } else if (k == 2) {
      const float v = fb(rng);
      g.WF(gp.obj + 40, v);
      nat.p1 = v;
    }
    FillInput(gp, io, ch, rng, 0);
    const int gr = gp.Call(sub_8264F458);
    const uint8_t before = io.cur;
    const int nr = nat.Process(io, 48000.0f);
    std::snprintf(what, sizeof(what), "Del0 %dch block %d state %d", ch, block, nat.state);
    if (gr != nr) {
      rep.Fail(std::string(what) + ": return differs");
      return;
    }
    if (!CompareOutput(gp, io, ch, rep, what)) return;
    if (io.cur != before) ++rep.processed; else ++rep.passthrough;
    // The delay line itself (planar, +100 buffer, +104 frames per channel, +112 write pos).
    if (!nat.ramp.empty() && g.R32(gp.obj + 108)) {
      for (int q = 0; q < kBlock; ++q) {
        char f[32];
        std::snprintf(f, sizeof(f), "ramp[%d] cnt %d", q, nat.cnt);
        if (!CheckWord(g, g.R32(gp.obj + 108) + uint32_t(q) * 4,
                       std::bit_cast<uint32_t>(nat.ramp[q]), rep, what, f))
          return;
      }
    }
    if (!CheckWord(g, gp.obj + 68, uint32_t(nat.cur_n), rep, what, "cur_n") ||
        !CheckWord(g, gp.obj + 72, std::bit_cast<uint32_t>(nat.cur_fb), rep, what, "cur_fb") ||
        !CheckWord(g, gp.obj + 64, std::bit_cast<uint32_t>(nat.cur_t), rep, what, "cur_t") ||
        !CheckWord(g, gp.obj + 60, uint32_t(nat.cnt), rep, what, "cnt") ||
        !CheckWord(g, gp.obj + 104, uint32_t(nat.size), rep, what, "size") ||
        !CheckWord(g, gp.obj + 112, uint32_t(nat.pos), rep, what, "pos"))
      return;
    if (!nat.buf.empty()) {
      const uint32_t gb = g.R32(gp.obj + 100);
      for (size_t i = 0; i < nat.buf.size(); ++i)
        if (g.R32(gb + uint32_t(i) * 4) != std::bit_cast<uint32_t>(nat.buf[i])) {
          char f[64];
          std::snprintf(f, sizeof(f), "line[%zu of %d] (prev state %d)", i, nat.size, prev_state);
          CheckWord(g, gb + uint32_t(i) * 4, std::bit_cast<uint32_t>(nat.buf[i]), rep, what, f);
          return;
        }
    }
    prev_state = nat.state;
    if (!CheckWord(g, gp.obj + 48, uint32_t(nat.state), rep, what, "state") ||
        !CheckWord(g, gp.obj + 40, std::bit_cast<uint32_t>(nat.p1), rep, what, "P1") ||
        !CheckWord(g, gp.obj + 24, std::bit_cast<uint32_t>(nat.tail24), rep, what, "tail"))
      return;
    io.cur = gp.Cur();
  }
}

}  // namespace

// Returns true when the self-test ran (the caller then exits).
bool RunAudioSelfTest(uint8_t* base) {
  const char* path = std::getenv("SR_AUDIO_SELFTEST");
  if (!path || !*path) return false;
  Report rep{std::fopen(path, "w")};
  if (!rep.f) return true;
  Guest g{base, 0};
  const uint32_t stack = g.Alloc(256 * 1024);
  g.stack_top = stack + 256 * 1024 - 512;
  for (int ch : {1, 2, 4, 6}) {
    for (uint32_t seed = 1; seed <= 25; ++seed) {
      const int before = rep.failures;
      ResamplerCase(g, rep, ch, seed * 7919 + ch);
      ++rep.cases;
      if (rep.failures != before)
        std::fprintf(rep.f, "FAIL resampler ch=%d seed=%u\n", ch, seed);
    }
  }
  std::fprintf(rep.f, "resampler: %d cases, %d mismatches\n", rep.cases, rep.failures);
  std::fprintf(rep.f,
               "coverage: process %ld, passthrough %ld, short reads %ld, history-only %ld, "
               "outputs_for %ld\n",
               rep.processed, rep.passthrough, rep.short_reads, rep.history_only, rep.outputs_for);
  // Control: the same comparison with one perturbed native sample must fail.
  Report ctl{rep.f};
  for (uint32_t seed = 1; seed <= 4; ++seed) ResamplerCase(g, ctl, 2, seed, true);
  std::fprintf(rep.f, "control (one ulp changed): %d mismatches (must be > 0)\n", ctl.failures);

  auto run = [&](const char* name, auto&& one) {
    Report r{rep.f};
    int cases = 0;
    for (int ch : {1, 2, 4, 6})
      for (uint32_t seed = 1; seed <= 10; ++seed) {
        const int before = r.failures;
        one(r, ch, seed * 104729 + ch);
        ++cases;
        if (r.failures != before) std::fprintf(rep.f, "FAIL %s ch=%d seed=%u\n", name, ch, seed);
      }
    std::fprintf(rep.f, "%s: %d cases, %d mismatches (filtered blocks %ld, bypass %ld)\n", name,
                 cases, r.failures, r.processed, r.passthrough);
  };
  run("LI20", [&](Report& r, int ch, uint32_t s) { FilterCase(g, r, false, ch, s); });
  run("HI20", [&](Report& r, int ch, uint32_t s) { FilterCase(g, r, true, ch, s); });
  run("Gai0", [&](Report& r, int ch, uint32_t s) { GainCase(g, r, ch, s); });
  // PnS0 takes 1, 2 or 4 input channels (6 maps to "1", 4 stays).
  run("PnS0", [&](Report& r, int ch, uint32_t s) { PanCase(g, r, ch == 6 ? 1 : ch, s); });
  run("Sen0+Sub0", [&](Report& r, int ch, uint32_t s) { BusCase(g, r, ch, s); });
  run("Rou0+Sub0", [&](Report& r, int ch, uint32_t s) { RouteCase(g, r, ch, s); });
  g_selftest_alloc = true;
  run("RM10", [&](Report& r, int ch, uint32_t s) { ReverbCase(g, r, ch, s); });
  run("Del0", [&](Report& r, int ch, uint32_t s) { DelayCase(g, r, ch, s); });
  g_selftest_alloc = false;
  std::fclose(rep.f);
  return true;
}

}  // namespace native_audio
