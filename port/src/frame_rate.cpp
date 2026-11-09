// The game's own frame limiter (sub_82720B50, a thread's main loop).
//
// Four game threads each run this loop on their own object. It reads a clock
// (sub_827203A0, nanoseconds at 1 ms resolution), adds the time since the
// previous iteration to an accumulator at this+104 and only runs an update
// once the accumulator plus the carry at this+112 reaches the period at
// this+72; otherwise it yields (NtYieldExecution in sub_828A7978) and polls
// again. Two loops have a 1/60 s period and two 1/30 s: those two, not the
// vblank rate, hold the game at 30 FPS. The update itself is variable-step
// (sub_82720740 turns the elapsed time into the float dt at this+132).
//
// SR_LOG_FRAME_LIMITER=1 logs each loop's period, update count (this+128) and
// accumulated update time (this+80) every 10 s.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "generated/default/superman_returns_init.h"

REXCVAR_DECLARE(int32_t, sr_native_fps_limit);

// The game's limiter loops sleep through KeDelayExecutionThread -> Sleep(ms).
// Without a raised timer resolution Windows 11 rounds such a sleep up to the
// process default (15.6 ms), so a 1 ms wait could turn a 16 ms frame into 31.
// 1 ms for the process, as the game had on the console.
REXCVAR_DEFINE_BOOL(sr_timer_resolution_1ms, true, "Superman Returns",
                    "Raise the Windows timer resolution to 1 ms for the game's sleeps (frame limiter)");
#pragma comment(lib, "winmm.lib")
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);

namespace {

// Period of the game's 30 Hz loops in its clock (nanoseconds).
constexpr uint64_t kPeriod30 = 33333334;

std::array<std::atomic<uint32_t>, 8> g_loops{};

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + addr));
}

uint64_t Load64(const uint8_t* base, uint32_t addr) {
  return __builtin_bswap64(*reinterpret_cast<const uint64_t*>(base + addr));
}

void LogLimiter(uint8_t* base) {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    for (auto& loop : g_loops) {
      const uint32_t object = loop.load();
      if (!object) continue;
      REXLOG_INFO("frame limiter: loop {:08X} period {} updates {} update time {} ms", object,
                  Load64(base, object + 72), Load32(base, object + 128),
                  Load64(base, object + 80) / 1000000);
    }
  }
}

}  // namespace

REX_HOOK_RAW(sub_82720B50) {
  static const bool timer = [] {
    if (!REXCVAR_GET(sr_timer_resolution_1ms)) return false;
    const unsigned int r = timeBeginPeriod(1);
    REXLOG_INFO("frame limiter: timer resolution 1 ms ({})", r == 0 ? "set" : "refused");
    return r == 0;
  }();
  (void)timer;
  static const bool log = [] {
    const char* v = std::getenv("SR_LOG_FRAME_LIMITER");
    return v && *v && *v != '0';
  }();
  static std::atomic<bool> logging{false};
  // Above 30 FPS the 30 Hz loops get the period of the frame rate limit.
  // Their update is variable-step, so a shorter period gives more, shorter
  // updates rather than a faster game.
  const int32_t limit = REXCVAR_GET(sr_native_fps_limit);
  if ((limit == 0 || limit > 30) && Load64(base, ctx.r3.u32 + 72) == kPeriod30) {
    const uint64_t period = limit ? 1000000000ull / uint64_t(limit) : 1000000000ull / 240;
    *reinterpret_cast<uint64_t*>(base + ctx.r3.u32 + 72) = __builtin_bswap64(period);
  }
  if (log) {
    for (auto& loop : g_loops) {
      uint32_t empty = 0;
      if (loop.compare_exchange_strong(empty, ctx.r3.u32)) break;
    }
    REXLOG_INFO("frame limiter: loop entered, object {:08X} period {}", ctx.r3.u32,
                Load64(base, ctx.r3.u32 + 72));
    if (!logging.exchange(true)) std::thread(LogLimiter, base).detach();
  }
  __imp__sub_82720B50(ctx, base);
}
