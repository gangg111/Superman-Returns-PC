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
#include <random>
#include <string>
#include <vector>

#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "generated/default/superman_returns_init.h"
#include "native_audio/resampler.h"

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
  std::fclose(rep.f);
  return true;
}

}  // namespace native_audio
