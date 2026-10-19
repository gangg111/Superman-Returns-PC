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
#include <array>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "generated/default/superman_returns_init.h"
#include "native_audio/bridge.h"
#include "native_audio/effects.h"
#include "native_audio/engine.h"
#include "native_audio/plugins.h"
#include "native_audio/resampler.h"
#include "native_audio/snd_player.h"

namespace native_audio {
namespace {
// While the self-test runs there is no System (and no allocator object at
// System+28): the RW allocator hooks below serve the test from the guest heap.
bool g_selftest_alloc = false;

// Bump allocator over one region of the guest's virtual heap: the self-test
// allocates hundreds of megabytes over its cases, far more than the small
// system heap holds, and never needs to free.
struct Arena {
  uint32_t next = 0, end = 0;
  uint32_t Alloc(uint32_t size, uint32_t align) {
    if (!next) {
      constexpr uint32_t kSize = 512u << 20;
      uint32_t base = 0;
      auto* heap = REX_KERNEL_MEMORY()->LookupHeap(0x40000000);
      if (!heap || !heap->Alloc(kSize, 64 * 1024,
                                rex::memory::kMemoryAllocationReserve |
                                    rex::memory::kMemoryAllocationCommit,
                                rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                                false, &base))
        return 0;
      next = base;
      end = base + kSize;
    }
    if (align < 16) align = 16;
    const uint32_t p = (next + align - 1) & ~(align - 1);
    if (p + size > end) return 0;
    next = p + size;
    return p;
  }
};
Arena g_arena;
}  // namespace
}  // namespace native_audio

// rw::audio::core allocator: (sys, size, name, align, allocator or 0) -> ptr
REX_HOOK_RAW(sub_8264CBB8) {
  if (!native_audio::g_selftest_alloc) {
    __imp__sub_8264CBB8(ctx, base);
    return;
  }
  const uint32_t align = ctx.r6.u32 < 16 ? 16 : ctx.r6.u32;
  ctx.r3.u64 = native_audio::g_arena.Alloc(ctx.r4.u32 ? ctx.r4.u32 : 16, align);
}

// rw::audio::core free: (sys, ptr, 0)
REX_HOOK_RAW(sub_8264C8F0) {
  if (!native_audio::g_selftest_alloc) {
    __imp__sub_8264C8F0(ctx, base);
    return;
  }
  // The arena never frees.
}

// ---- test decoder: the EXm0 decoder's create / set input / decode / destroy,
// replaced while the self-test runs by a deterministic generator (no XMA).
namespace native_audio {
namespace {
bool g_selftest_decoder = false;
std::FILE* g_trace = nullptr;  // SR_AUDIO_SELFTEST_TRACE: the report file

// int16 / 32768 like the real decoder output, from a hash of the block key,
// the frame and the channel.
float TestSample(uint32_t key, uint32_t frame, uint32_t ch) {
  uint32_t h = key * 2654435761u ^ (frame + 0x9E3779B9u) * 40503u ^ ch * 2246822519u;
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  return float(int16_t(h & 0xFFFF)) * (1.0f / 32768.0f);
}

struct TestDecoderInput {
  uint32_t key, frames, pos, slot;  // slot = &stream slot +12 (the set-input argument)
};
std::unordered_map<uint32_t, std::deque<TestDecoderInput>> g_test_inputs;  // by decoder
}  // namespace
}  // namespace native_audio

// EXm0 create(channels, sys) -> decoder (the real vtable, hooked methods below)
REX_HOOK_RAW(sub_82659AF0) {
  if (!native_audio::g_selftest_decoder) {
    __imp__sub_82659AF0(ctx, base);
    return;
  }
  const uint32_t dec = native_audio::g_arena.Alloc(64, 16);
  std::memset(base + dec, 0, 64);
  *reinterpret_cast<uint32_t*>(base + dec) = __builtin_bswap32(0x82081FACu);
  native_audio::g_test_inputs[dec].clear();
  if (native_audio::g_trace) std::fprintf(native_audio::g_trace, "    create dec %08X\n", dec);
  ctx.r3.u64 = dec;
}

// set input (dec, &slot{+0 data, +4 frames, +8 flag})
REX_HOOK_RAW(sub_826592A8) {
  if (!native_audio::g_selftest_decoder) {
    const uint32_t dec = ctx.r3.u32, slot = ctx.r4.u32;
    __imp__sub_826592A8(ctx, base);
    native_audio::bridge::OnSetInput(base, dec, slot);
    return;
  }
  const uint32_t data = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + ctx.r4.u32));
  const uint32_t frames = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + ctx.r4.u32 + 4));
  native_audio::g_test_inputs[ctx.r3.u32].push_back({data, frames, 0, ctx.r4.u32});
  if (native_audio::g_trace)
    std::fprintf(native_audio::g_trace, "    set input dec %08X slot %08X data %08X frames %u\n",
                 ctx.r3.u32, ctx.r4.u32, data, frames);
}

// decode (dec, float* planar dst, frames) -> frames written (channel stride = that)
REX_HOOK_RAW(sub_826592F0) {
  if (!native_audio::g_selftest_decoder) {
    const uint32_t dec = ctx.r3.u32, dst = ctx.r4.u32;
    __imp__sub_826592F0(ctx, base);
    native_audio::bridge::OnDecode(base, dec, dst, int32_t(ctx.r3.u32));
    return;
  }
  const uint32_t dec = ctx.r3.u32, dst = ctx.r4.u32;
  const int want = int32_t(ctx.r5.u32);
  if (native_audio::g_trace)
    std::fprintf(native_audio::g_trace, "    decode dec %08X dst %08X want %d\n", dec, dst, want);
  const int ch = base[dec + 8];
  auto& q = native_audio::g_test_inputs[dec];
  // gather up to `want` frames across the queued inputs, then write planar
  std::vector<std::pair<uint32_t, uint32_t>> src;  // (key, frame)
  while (int(src.size()) < want && !q.empty()) {
    auto& in = q.front();
    while (int(src.size()) < want && in.pos < in.frames) src.push_back({in.key, in.pos++});
    if (in.pos >= in.frames) {
      // buffer fully consumed: the real decoder marks the stream slot (+28),
      // which lets sub_82653600 free it at the next Process
      *reinterpret_cast<uint32_t*>(base + in.slot + 16) = __builtin_bswap32(1u);
      q.pop_front();
    }
  }
  const int n = int(src.size());
  for (int c = 0; c < ch; ++c)
    for (int i = 0; i < n; ++i) {
      const float v = native_audio::TestSample(src[i].first, src[i].second, uint32_t(c));
      *reinterpret_cast<uint32_t*>(base + dst + uint32_t(c * n + i) * 4) =
          __builtin_bswap32(std::bit_cast<uint32_t>(v));
    }
  ctx.r3.u64 = uint32_t(n);
}



// destroy
REX_HOOK_RAW(sub_82658448) {
  if (!native_audio::g_selftest_decoder) {
    __imp__sub_82658448(ctx, base);
    return;
  }
  if (native_audio::g_trace) std::fprintf(native_audio::g_trace, "    destroy dec %08X\n", ctx.r3.u32);
}

namespace native_audio {

namespace {

struct Guest {
  uint8_t* base;
  uint32_t stack_top;

  uint32_t Alloc(uint32_t size) { return g_arena.Alloc(size, 128); }
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

void VuCase(Guest& g, Report& rep, int ch, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> pick(0, 9);
  GuestPlugin gp(g, 0x82936C58, ch, ch, sub_8264F950, sub_8264FD88);
  VuMeter nat(ch, 48000.0f);
  Io& io = g_nio.io;
  io.cur = 0;
  char what[64];
  for (int block = 0; block < 40; ++block) {
    FillInput(gp, io, ch, rng, pick(rng) == 0 ? 2 : 0);
    gp.Call(sub_8264FD20);
    nat.Process(io);
    std::snprintf(what, sizeof(what), "VuM0 %dch block %d", ch, block);
    for (int k = 0; k < 18; ++k) {
      char f[16];
      std::snprintf(f, sizeof(f), "slot %d", k);
      if (!CheckWord(g, gp.obj + 40 + 8 * k, std::bit_cast<uint32_t>(nat.params[k]), rep, what, f))
        return;
    }
    ++rep.processed;
  }
}

void DacCase(Guest& g, Report& rep, int ch, uint32_t seed) {
  std::mt19937 rng(seed);
  GuestPlugin gp(g, 0x82936C58, 6, 6, sub_8264F950, sub_8264FD88);  // only its io is used
  const uint32_t dac = g.Alloc(256);
  std::memset(g.base + dac, 0, 256);
  g.W32(dac + 56, gp.io);
  g.base[dac + 143] = 6;
  std::uniform_real_distribution<float> u(-1.5f, 1.5f);
  float out[6 * kBlock];
  char what[64];
  for (int block = 0; block < 10; ++block) {
    const bool fade = (block % 3) == 0;
    g.base[dac + 144] = fade ? 1 : 0;
    bool nfade = fade;
    float in[6 * kBlock];
    g.base[gp.io + 28] = 0;
    for (int i = 0; i < 6 * kBlock; ++i) {
      in[i] = u(rng);
      if (i == 77) in[i] = std::bit_cast<float>(0x7FC00000u);  // NaN passes the clamp
      g.WF(gp.buf[0] + uint32_t(i) * 4, in[i]);
    }
    PPCContext c = g.Ctx();
    c.r3.u64 = dac;
    sub_82650810(c, g.base);
    DacFinalize(in, out, 6, &nfade);
    std::snprintf(what, sizeof(what), "Dac0 block %d fade %d", block, fade);
    const uint32_t res = gp.buf[g.base[gp.io + 28]];
    for (int i = 0; i < 6 * kBlock; ++i)
      if (g.R32(res + uint32_t(i) * 4) != std::bit_cast<uint32_t>(out[i])) {
        char f[24];
        std::snprintf(f, sizeof(f), "out[%d]", i);
        CheckWord(g, res + uint32_t(i) * 4, std::bit_cast<uint32_t>(out[i]), rep, what, f);
        return;
      }
    if (g.base[dac + 144] != (nfade ? 1 : 0)) rep.Fail(std::string(what) + ": fade flag");
    ++rep.processed;
  }
  (void)ch;
}

// ------------------------------------------------------------ SndPlayer

// The test decoder's data as a native SoundSource: one block (RAM sounds).
class TestSource : public SoundSource {
 public:
  TestSource(uint32_t key, int ch, int frames) : key_(key), ch_(ch), frames_(frames) {}
  int Channels() const override { return ch_; }
  int BlockCount() const override { return 1; }
  int BlockFrames(int) const override { return frames_; }
  int Decode(int, int offset, float* dst, int frames) override {
    const int n = frames < frames_ - offset ? frames : frames_ - offset;
    for (int c = 0; c < ch_; ++c)
      for (int i = 0; i < n; ++i) dst[c * n + i] = TestSample(key_, uint32_t(offset + i), uint32_t(c));
    return n;
  }

 private:
  uint32_t key_;
  int ch_, frames_;
};

void SndCase(Guest& g, Report& rep, int player_ch, uint32_t seed);

// ------------------------------------------------------------ whole graph

// A guest System good enough for Voice::Create, Control/SetParamF (they queue
// into +32/+40) and the drain sub_8264C540: lock/unlock callbacks (+76/+80)
// point at the empty guest stub sub_8231C140, the update-callback gate +64 is 0.
struct GuestSystem {
  Guest& g;
  uint32_t sys;
  explicit GuestSystem(Guest& gg) : g(gg) {
    sys = g.Alloc(512);
    std::memset(g.base + sys, 0, 512);
    g.WF(sys + 12, 48000.0f);
    g.WF(sys + 8, 256.0f / 48000.0f);
    g.W32(sys + 20, 1);
    const uint32_t q = g.Alloc(256 * 1024);
    g.W32(sys + 32, q);
    g.W32(sys + 36, 256 * 1024);
    g.W32(sys + 76, 0x8231C140);
    g.W32(sys + 80, 0x8231C140);
  }
  void Drain() {
    PPCContext c = g.Ctx();
    c.r3.u64 = sys;
    sub_8264C540(c, g.base);
  }
};

struct GuestEntry {
  uint32_t desc;
  int channels;
  uint32_t args = 0;  // entry+0
};

uint32_t GuestCreateVoice(Guest& g, GuestSystem& s, const std::vector<GuestEntry>& es,
                          std::vector<uint32_t>* plugins) {
  const uint32_t entries = g.Alloc(uint32_t(es.size()) * 12 + 16);
  std::memset(g.base + entries, 0, es.size() * 12 + 16);
  for (size_t i = 0; i < es.size(); ++i) {
    g.W32(entries + uint32_t(i) * 12, es[i].args);
    g.W32(entries + uint32_t(i) * 12 + 4, es[i].desc);
    g.base[entries + uint32_t(i) * 12 + 8] = uint8_t(es[i].channels);
  }
  const uint32_t out = g.Alloc(64);
  PPCContext c = g.Ctx();
  c.r3.u64 = es.size();
  c.r4.u64 = entries;
  c.r5.u64 = out;
  c.r6.u64 = s.sys;
  sub_82650398(c, g.base);
  const uint32_t voice = c.r3.u32;
  plugins->clear();
  for (size_t i = 0; i < es.size(); ++i) plugins->push_back(g.R32(voice + 72 + uint32_t(i) * 4));
  return voice;
}

void GuestSetParam(Guest& g, uint32_t plugin, int idx, float v) {
  PPCContext c = g.Ctx();
  c.r3.u64 = plugin;
  c.r4.u64 = uint32_t(idx);
  c.f1.f64 = double(v);
  sub_8264CE48(c, g.base);
}

void GuestControl(Guest& g, uint32_t plugin, int idx, uint32_t data) {
  PPCContext c = g.Ctx();
  c.r3.u64 = plugin;
  c.r4.u64 = uint32_t(idx);
  c.r5.u64 = data;
  sub_8264CE18(c, g.base);
}

// The class descriptors in .data are still zero when the self-test runs (the
// game's static initializers fill them later); Voice::Create and the executor
// read +8 GetSize, +12 Create, +20 hook / bus flag, +24 Process. Values from
// docs/natywny-dzwiek/deskryptory.txt (read from the image at run time).
void InitDescriptors(Guest& g) {
  struct D {
    uint32_t at, id, get_size, create, p16, fn20, process, counts;
  };
  static const D kD[] = {
      {0x82937630, 0x50697430, 0x826625F0, 0x82662620, 0x8292F40C, 0x82662608, 0, 0x10000},
      {0x829377B0, 0x4C493230, 0x826633F8, 0x82663508, 0x8292F58C, 0x82663320, 0x82663420,
       0x10000},
      {0x82937808, 0x48493230, 0x826636E8, 0x826637F8, 0x8292F5E4, 0x82663610, 0x82663710,
       0x10000},
      {0x82937860, 0x47616930, 0x8266BE78, 0x826639D8, 0x8292F63C, 0, 0x82663900, 0x10000},
      {0x82936F30, 0x506E5330, 0x82652028, 0x826529B0, 0x8292ECC4, 0, 0x82652690, 0x30000},
      {0x829375D8, 0x53656E30, 0x82662058, 0x82662428, 0x8292F344, 0x826620D8, 0x82662100,
       0x1010200},
      {0x82936E28, 0x53756230, 0x826513A8, 0x826516C8, 0x8292EC04, 1, 0x826513E8, 0x1000000},
      {0x82937510, 0x526F7530, 0x82661D30, 0x82661F28, 0x8292F284, 0, 0x82661D58, 0x100},
      {0x82936968, 0x524D3130, 0x8264DF18, 0x8264E668, 0x8292E704, 0, 0x8264E398, 0x30000},
      {0x829369E0, 0x44656C30, 0x8264E980, 0x8264F848, 0x8292E79C, 0, 0x8264F458, 0x20000},
      {0x82936C58, 0x56754D30, 0x8264F950, 0x8264FD88, 0x8292E814, 0, 0x8264FD20, 0x120100},
      {0x82936E90, 0x41695730, 0x82651880, 0x82651990, 0x8292EC5C, 0, 0x82651C98, 0x200},
  };
  for (const D& d : kD) {
    g.W32(d.at + 4, d.id);
    g.W32(d.at + 8, d.get_size);
    g.W32(d.at + 12, d.create);
    g.W32(d.at + 16, d.p16);
    g.W32(d.at + 20, d.fn20);
    g.W32(d.at + 24, d.process);
    g.W32(d.at + 28, d.counts);
  }
}

void GraphCase(Guest& g, Report& rep, int variant, uint32_t seed) {
  InitDescriptors(g);
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> pick(0, 99);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  g.W32(0x8292ED74, 0x40C90FDB);  // PnS0 angle scale (runtime copy of the descriptor)
  g.base[0x8299FDE0] = 0;
  GuestSystem gs(g);
  Engine eng(48000.0f);
  const uint32_t name = g.Alloc(16), nameptr = g.Alloc(16);
  std::memcpy(g.base + name, "TESTBUS", 8);
  g.W32(nameptr, name);
  using PC = PluginClass;
  struct Pair {
    uint32_t gvoice;
    std::vector<uint32_t> gp;
    Voice* nv;
  };
  auto make = [&](const std::vector<std::pair<PC, int>>& chain) {
    static const std::pair<PC, uint32_t> kDesc[] = {
        {PC::kPitch, 0x82937630},    {PC::kLowPass, 0x829377B0}, {PC::kHighPass, 0x82937808},
        {PC::kGain, 0x82937860},     {PC::kPanSurround, 0x82936F30},
        {PC::kSend, 0x829375D8},     {PC::kSubMix, 0x82936E28},  {PC::kReverb, 0x82936968},
        {PC::kVuMeter, 0x82936C58},  {PC::kAiffWriter, 0x82936E90}, {PC::kDelay, 0x829369E0}};
    std::vector<GuestEntry> ge;
    std::vector<Engine::Entry> ne;
    for (auto& [cls, ch] : chain) {
      uint32_t d = 0;
      for (auto& [c2, a] : kDesc)
        if (c2 == cls) d = a;
      ge.push_back({d, ch, cls == PC::kSubMix ? nameptr : 0u});
      ne.push_back({cls, ch, 0.0f});
    }
    Pair p;
    p.gvoice = GuestCreateVoice(g, gs, ge, &p.gp);
    p.nv = eng.CreateVoice(ne);
    return p;
  };
  // master, two group buses, a reverb bus, three sources
  const int src_ch[3] = {1 + (variant & 1), 4, variant & 2 ? 2 : 1};
  Pair master = make({{PC::kSubMix, 6}, {PC::kVuMeter, 6}, {PC::kAiffWriter, 6}});
  std::vector<Pair> groups, sources;
  for (int k = 0; k < 2; ++k)
    groups.push_back(make({{PC::kSubMix, 2}, {PC::kLowPass, 2}, {PC::kHighPass, 2},
                           {PC::kGain, 2}, {PC::kPanSurround, 6}, {PC::kSend, 6}}));
  Pair reverb = make({{PC::kSubMix, 1}, {PC::kReverb, 1}, {PC::kVuMeter, 1}, {PC::kLowPass, 1},
                      {PC::kPanSurround, 6}, {PC::kSend, 6}});
  for (int k = 0; k < 3; ++k)
    sources.push_back(make({{PC::kSubMix, src_ch[k]}, {PC::kPitch, src_ch[k]},
                            {PC::kLowPass, src_ch[k]}, {PC::kHighPass, src_ch[k]},
                            {PC::kGain, src_ch[k]}, {PC::kPanSurround, 6}, {PC::kSend, 6}}));
  if (!master.gvoice || !reverb.gvoice) {
    rep.Fail("guest Voice::Create failed");
    return;
  }
  auto bus_of = [](Pair& p) { return p.gp[0]; };
  auto nbus_of = [](Pair& p) { return p.nv->plugins[0]->AsBus(); };
  // Attach: each Send (last plug-in) to its bus through Control(0, &bus).
  const uint32_t data = g.Alloc(16);
  auto attach = [&](Pair& from, Pair* to) {
    g.W32(data, to ? bus_of(*to) : 0);
    GuestControl(g, from.gp.back(), 0, data);
    eng.AttachToBus(from.nv->plugins.back().get(), to ? nbus_of(*to) : nullptr);
  };
  attach(groups[0], &master);
  attach(groups[1], &master);
  attach(reverb, &master);
  attach(sources[0], &groups[0]);
  attach(sources[1], &groups[1]);
  attach(sources[2], variant & 1 ? &reverb : &groups[0]);
  // Random parameter traffic like the game's (every plug-in with params).
  auto param = [&](Pair& p, int k, int idx, float v) {
    GuestSetParam(g, p.gp[k], idx, v);
    eng.SetParam(p.nv->plugins[k].get(), idx, v);
  };
  std::uniform_real_distribution<float> hz(50.0f, 22000.0f), gain(0.0f, 1.2f),
      az(-180.0f, 180.0f), dist(0.0f, 30.0f), pitch(0.5f, 2.0f);
  param(reverb, 1, 0, 2.0f);  // reverb on
  // io for the guest graph: two 6 x 4096-frame buffers, like the engine's.
  const uint32_t gio = g.Alloc(64);
  std::memset(g.base + gio, 0, 64);
  for (int i = 0; i < 2; ++i) {
    const uint32_t b = g.Alloc(6 * 4096 * 4);
    std::memset(g.base + b, 0, 6 * 4096 * 4);
    g.W32(gio + 4 + 4 * i, b);
    g.W32(gio + 20 + 4 * i, 6 * 4096 * 4);
  }
  char what[96];
  for (int block = 0; block < 120; ++block) {
    // parameters
    for (Pair* p : {&sources[0], &sources[1], &sources[2], &groups[0], &groups[1]}) {
      const bool src = p->nv->plugins[1]->cls == PC::kPitch;
      const int o = src ? 1 : 0;  // sources have Pitch at 1
      if (src && pick(rng) < 10) param(*p, 1, 0, pitch(rng));
      if (pick(rng) < 8) param(*p, 1 + o, 0, pick(rng) < 20 ? 1000000.0f : hz(rng));
      if (pick(rng) < 8) param(*p, 2 + o, 0, pick(rng) < 20 ? 0.0f : hz(rng) * 0.05f);
      if (pick(rng) < 30) param(*p, 3 + o, 0, gain(rng));
      if (pick(rng) < 30) param(*p, 4 + o, 1, az(rng));
      if (pick(rng) < 15) param(*p, 4 + o, 0, dist(rng));
      if (pick(rng) < 5) param(*p, 5 + o, 0, gain(rng));
    }
    if (pick(rng) < 3) param(reverb, 1, 1, dist(rng) + 2.0f);
    if (block == 70 && (variant & 2)) {  // graceful release of a source: tail mode
      PPCContext c = g.Ctx();
      c.r3.u64 = sources[1].gvoice;
      sub_826505B8(c, g.base);
      eng.ReleaseVoice(sources[1].nv);
    }
    // source signals straight into the sources' buses (contributions = 1)
    for (Pair* p : {&sources[0], &sources[1], &sources[2]}) {
      if (block >= 100 && p == &sources[2]) continue;  // silence: tail mode on a source
      Bus* nb = nbus_of(*p);
      const uint32_t gb = bus_of(*p);
      const uint32_t gbuf = g.R32(gb + 32);
      for (int i = 0; i < nb->channels * kBlock; ++i) {
        const float v = u(rng) * 0.5f;
        nb->buffer[i] = v;
        g.WF(gbuf + uint32_t(i) * 4, v);
      }
      nb->contributions = 1;
      *reinterpret_cast<uint16_t*>(g.base + gb + 148) = __builtin_bswap16(uint16_t(1));
    }
    // one block on both sides: drain, then the executor from the master voice
    gs.Drain();
    eng.Drain();
    g.base[gio + 28] = 0;
    g.WF(gio + 12, 1.0f);
    eng.io.cur = 0;
    PPCContext c = g.Ctx();
    c.r3.u64 = master.gvoice;
    c.r4.u64 = gio;
    c.r5.u64 = g.R32(gs.sys + 20);
    sub_8265A148(c, g.base);
    const int gr = int32_t(c.r3.u32);
    const int nr = eng.RenderGraph(master.nv, eng.io);
    std::snprintf(what, sizeof(what), "graph v%d block %d", variant, block);
    if (g.R32(gs.sys + 20) != eng.counter) {
      rep.Fail(std::string(what) + ": System+20 differs");
      return;
    }
    if (gr != nr) {
      std::snprintf(what, sizeof(what), "graph v%d block %d: return guest %d native %d", variant,
                    block, gr, nr);
      rep.Fail(what);
      return;
    }
    if (g.base[gio + 28] != eng.io.cur) {
      rep.Fail(std::string(what) + ": io buffer index differs");
      return;
    }
    if (gr == 1) {
      const uint32_t gcur = g.R32(gio + 4 + 4 * g.base[gio + 28]);
      for (int i = 0; i < 6 * kBlock; ++i)
        if (g.R32(gcur + uint32_t(i) * 4) != std::bit_cast<uint32_t>(eng.io.Cur()[i])) {
          char f[32];
          std::snprintf(f, sizeof(f), "master out[%d]", i);
          CheckWord(g, gcur + uint32_t(i) * 4, std::bit_cast<uint32_t>(eng.io.Cur()[i]), rep,
                    what, f);
          return;
        }
      ++rep.processed;
    } else {
      ++rep.passthrough;
    }
    // voice states (+70 state, +48 silent frames)
    for (Pair* p : {&sources[0], &sources[1], &sources[2], &groups[0], &groups[1], &reverb}) {
      if (g.base[p->gvoice + 70] != p->nv->state ||
          g.R32(p->gvoice + 48) != std::bit_cast<uint32_t>(p->nv->silent)) {
        std::snprintf(what, sizeof(what), "graph v%d block %d: voice state %d/%d silent %08X/%08X",
                      variant, block, g.base[p->gvoice + 70], p->nv->state,
                      g.R32(p->gvoice + 48), std::bit_cast<uint32_t>(p->nv->silent));
        rep.Fail(what);
        return;
      }
    }
  }
}

// A voice SnP0(player_ch) > Pit0 on both sides; random requests (looped and
// one-shot RAM sounds of 1, 2 or 4 channels, delayed starts, stop-after flags),
// pitch changes and stops. Compared: the voice output, return value, IsDone,
// the position the game reads (GetParamD 1) and param 0.
void SndCase(Guest& g, Report& rep, int player_ch, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> pick(0, 99);
  InitDescriptors(g);
  // SnP0 descriptor (also zero at self-test time) and the EXm0 codec entry.
  g.W32(0x829370D0 + 4, 0x536E5030);
  g.W32(0x829370D0 + 8, 0x82653EF8);
  g.W32(0x829370D0 + 12, 0x82654428);
  g.W32(0x829370D0 + 16, 0x8292EEBC);
  g.W32(0x829370D0 + 20, 0);
  g.W32(0x829370D0 + 24, 0x82655610);
  g.W32(0x829370D0 + 28, 0x01030200);
  GuestSystem gs(g);
  {
    PPCContext c = g.Ctx();
    c.r3.u64 = gs.sys;
    sub_8265C0A8(c, g.base);
    const uint32_t reg = c.r3.u32;
    g.W32(0x8292F0C4, 0x45586D30);  // 'EXm0'
    g.W32(0x8292F0C8, 0x82659AF0);
    g.W32(0x8292F0CC, 0);
    c = g.Ctx();
    c.r3.u64 = reg;
    c.r4.u64 = 0x8292F0C4;
    sub_8265C1E8(c, g.base);
  }
  Engine eng(48000.0f);
  const uint32_t args = g.Alloc(16);
  g.WF(args, 4.0f);  // 4 request objects
  // As in the game: the sound voice is a child of a bus voice (the executor
  // writes to the parent when a voice stops, so a stopped root would crash).
  const uint32_t name = g.Alloc(16), nameptr = g.Alloc(16);
  std::memcpy(g.base + name, "SNDBUS", 7);
  g.W32(nameptr, name);
  std::vector<uint32_t> gbp;
  const uint32_t gbusv = GuestCreateVoice(g, gs, {{0x82936E28, player_ch, nameptr}}, &gbp);
  Voice* nbusv = eng.CreateVoice({{PluginClass::kSubMix, player_ch, 0.0f}});
  std::vector<GuestEntry> ge = {
      {0x829370D0, player_ch, args}, {0x82937630, player_ch, 0}, {0x829375D8, player_ch, 0}};
  std::vector<uint32_t> gp;
  const uint32_t gvoice = GuestCreateVoice(g, gs, ge, &gp);
  Voice* nv = eng.CreateVoice({{PluginClass::kSndPlayer, player_ch, 4.0f},
                               {PluginClass::kPitch, player_ch, 0.0f},
                               {PluginClass::kSend, player_ch, 0.0f}});
  {
    const uint32_t d = g.Alloc(16);
    g.W32(d, gbp[0]);
    GuestControl(g, gp[2], 0, d);
    eng.AttachToBus(nv->plugins[2].get(), nbusv->plugins[0]->AsBus());
  }
  (void)gvoice;
  auto* nplayer = static_cast<SndPlayer*>(nv->plugins[0].get());
  const uint32_t gio = g.Alloc(64);
  std::memset(g.base + gio, 0, 64);
  for (int i = 0; i < 2; ++i) {
    const uint32_t b = g.Alloc(6 * 4096 * 4);
    std::memset(g.base + b, 0, 6 * 4096 * 4);
    g.W32(gio + 4 + 4 * i, b);
    g.W32(gio + 20 + 4 * i, 6 * 4096 * 4);
  }
  const uint32_t req = g.Alloc(64);
  std::uniform_real_distribution<float> pitch(0.5f, 2.5f);
  static const int kRates[4] = {44100, 22050, 32000, 48000};
  char what[128];
  double clock = 0.0;
  std::vector<std::string> history;  // printed when the case fails
  for (int block = 0; block < 200; ++block) {
    const int k = pick(rng);
    if (k < 6 || block == 0) {  // a start request
      const int sch = std::array<int, 3>{1, 2, 4}[pick(rng) % 3];
      const int rate = kRates[pick(rng) % 4];
      const int frames = 50 + pick(rng) * 120;
      const bool loop = pick(rng) < 30;
      // SNR header + one data block in guest memory (the block key = its address)
      const uint32_t snr = g.Alloc(64);
      std::memset(g.base + snr, 0, 64);
      const uint32_t h1 = (3u << 24) | (uint32_t(sch - 1) << 18) | uint32_t(rate);
      const uint32_t h2 = (0u << 30) | (uint32_t(loop) << 29) | uint32_t(frames);
      g.W32(snr, h1);
      g.W32(snr + 4, h2);
      const uint32_t hdr = loop ? 12 : 8;
      if (loop) g.W32(snr + 8, 0);  // loop start 0 (all RAM loops in the game)
      g.W32(snr + hdr, 0x80000000u | 64u);
      g.W32(snr + hdr + 4, uint32_t(frames));
      const double start = (pick(rng) < 25) ? clock + (pick(rng) % 8) * 0.002 : 0.0;
      const float flag = pick(rng) < 10 ? 1.0f : 0.0f;
      std::memset(g.base + req, 0, 64);
      auto wd = [&](uint32_t a, double v) {
        const uint64_t b = std::bit_cast<uint64_t>(v);
        g.W32(a, uint32_t(b >> 32));
        g.W32(a + 4, uint32_t(b));
      };
      wd(req, start);
      wd(req + 8, 0.0);
      g.W32(req + 20, snr);
      g.WF(req + 28, flag);
      GuestControl(g, gp[0], 0, req);
      PlayRequest pr;
      pr.start_time = start;
      std::memcpy(pr.snr, g.base + snr, 16);
      pr.flag = flag;
      pr.source = std::make_shared<TestSource>(snr + hdr + 8, sch, frames);
      const float nid = nplayer->Start(pr);
      char hb[160];
      std::snprintf(hb, sizeof(hb), "block %d: start ch %d rate %d frames %d loop %d start %.4f "
                    "(clock %.4f) flag %g", block, sch, rate, frames, loop, start, clock, flag);
      history.push_back(hb);
      if (g.R32(req + 32) != std::bit_cast<uint32_t>(nid)) {
        rep.Fail("SnP0 request id differs");
        return;
      }
    } else if (k < 8) {  // stop everything
      GuestControl(g, gp[0], 1, 0);
      nplayer->Stop();
      history.push_back("block " + std::to_string(block) + ": stop");
    }
    if (pick(rng) < 10) {
      const float p = pitch(rng);
      GuestSetParam(g, gp[1], 0, p);
      eng.SetParam(nv->plugins[1].get(), 0, p);
    }
    if (std::getenv("SR_AUDIO_SELFTEST_TRACE"))
      std::fprintf(rep.f, "  trace SnP0 %dch seed %u block %d: %s\n", player_ch, seed, block,
                   history.empty() ? "-" : history.back().c_str());
    gs.Drain();
    if (g_trace) std::fprintf(g_trace, "    guest drain done\n");
    eng.Drain();
    if (g_trace) std::fprintf(g_trace, "    native drain done\n");
    // the guest clock is advanced by the Dac; do it here on both sides
    const uint64_t cb = std::bit_cast<uint64_t>(clock);
    g.W32(gs.sys, uint32_t(cb >> 32));
    g.W32(gs.sys + 4, uint32_t(cb));
    eng.clock = clock;
    g.base[gio + 28] = 0;
    eng.io.cur = 0;
    PPCContext c = g.Ctx();
    c.r3.u64 = gbusv;
    c.r4.u64 = gio;
    c.r5.u64 = g.R32(gs.sys + 20);
    sub_8265A148(c, g.base);
    if (g_trace) std::fprintf(g_trace, "    guest render done\n");
    const int gr = int32_t(c.r3.u32);
    const int nr = eng.RenderGraph(nbusv, eng.io);
    if (g_trace) std::fprintf(g_trace, "    native render done\n");
    clock += 256.0 / 48000.0;
    std::snprintf(what, sizeof(what), "SnP0 %dch block %d", player_ch, block);
    {
      const SndPlayer::State st = nplayer->Debug();
      const uint32_t gpend = g.R32(gp[0] + 84);
      if (!CheckWord(g, gp[0] + 100, uint32_t(st.pos), rep, what, "pos") ||
          !CheckWord(g, gp[0] + 40, uint32_t(st.planner), rep, what, "planner") ||
          !CheckWord(g, gp[0] + 1176, uint32_t(st.planned), rep, what, "planned") ||
          !CheckWord(g, gp[0] + 104, uint32_t(st.cur_slot), rep, what, "cur slot") ||
          (gpend != 0) != (st.pending != 0)) {
        if ((gpend != 0) != (st.pending != 0)) rep.Fail(std::string(what) + ": pending differs");
        return;
      }
    }
    if (gr != nr) {
      std::snprintf(what, sizeof(what), "SnP0 %dch block %d: return guest %d native %d",
                    player_ch, block, gr, nr);
      rep.Fail(what);
      return;
    }
    if (gr == 1) {
      if (g.base[gio + 28] != eng.io.cur) {
        rep.Fail(std::string(what) + ": io index");
        const size_t from = history.size() > 6 ? history.size() - 6 : 0;
        for (size_t h = from; h < history.size(); ++h)
          std::fprintf(rep.f, "    %s\n", history[h].c_str());
        return;
      }
      const uint32_t gcur = g.R32(gio + 4 + 4 * g.base[gio + 28]);
      for (int i = 0; i < player_ch * kBlock; ++i)
        if (g.R32(gcur + uint32_t(i) * 4) != std::bit_cast<uint32_t>(eng.io.Cur()[i])) {
          char f[32];
          std::snprintf(f, sizeof(f), "out[%d]", i);
          CheckWord(g, gcur + uint32_t(i) * 4, std::bit_cast<uint32_t>(eng.io.Cur()[i]), rep, what,
                    f);
          return;
        }
      ++rep.processed;
    } else {
      ++rep.passthrough;
    }
    c = g.Ctx();
    c.r3.u64 = gp[0];
    sub_82653E70(c, g.base);
    const bool gdone = (c.r3.u32 & 0xFF) != 0;
    const uint64_t gpos = (uint64_t(g.R32(gp[0] + 56)) << 32) | g.R32(gp[0] + 60);
    if (gdone != nplayer->IsDone() || gpos != std::bit_cast<uint64_t>(nplayer->GetParam(1)) ||
        g.R32(gp[0] + 48) != std::bit_cast<uint32_t>(float(nplayer->GetParam(0)))) {
      std::snprintf(what, sizeof(what), "SnP0 %dch block %d: IsDone %d/%d or position/param0",
                    player_ch, block, gdone, nplayer->IsDone());
      rep.Fail(what);
      return;
    }
  }
}

}  // namespace

// Returns true when the self-test ran (the caller then exits).
bool RunAudioSelfTest(uint8_t* base) {
  const char* path = std::getenv("SR_AUDIO_SELFTEST");
  if (!path || !*path) return false;
  Report rep{std::fopen(path, "w")};
  if (!rep.f) return true;
  std::setvbuf(rep.f, nullptr, _IONBF, 0);  // a crash must not lose the lines before it
  if (std::getenv("SR_AUDIO_SELFTEST_TRACE")) g_trace = rep.f;
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
  run("VuM0", [&](Report& r, int ch, uint32_t s) { VuCase(g, r, ch, s); });
  run("Dac0 finalize", [&](Report& r, int ch, uint32_t s) { DacCase(g, r, ch, s); });
  run("graph", [&](Report& r, int ch, uint32_t s) { GraphCase(g, r, ch, s); });
  g_selftest_decoder = true;
  run("SnP0", [&](Report& r, int ch, uint32_t s) { SndCase(g, r, ch, s); });
  g_selftest_decoder = false;
  g_selftest_alloc = false;
  std::fclose(rep.f);
  return true;
}

}  // namespace native_audio
