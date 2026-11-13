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

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
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

// The limiter's wait between polls (sub_825F7168 at 0x82720C2C) only yields: on
// the console each loop had its own hardware thread, here the four loops kept
// four host threads at 100% (measured 2026-11-10: 99/99/99/93% at the 60 FPS
// limit), which a 4-core CPU needs for the game and the renderer.
// The main thread's wait for a flag (sub_82176300: yield until byte this+24 is
// set) did the same with a fifth thread.
REXCVAR_DEFINE_INT32(sr_fps_loops, 2, "Superman Returns",
                     "Above 30 FPS: which of the game's 30 Hz loops run at the frame rate limit, a bit each: "
                     "1 the script loop (stalls the meteor scene and the first Metallo fight at 60 FPS), "
                     "2 the frame loop (sets the frame rate). 2, the default: the frame loop only")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(sr_wait_sleep, true, "Superman Returns",
                    "The game's busy waits sleep: the frame limiter loops until their next update, "
                    "the flag wait in short naps after a brief spin (false: the original busy waits)");

namespace {

// Return address of the limiter's wait call (bl sub_825F7168 at 0x82720C2C).
constexpr uint32_t kLimiterWaitReturn = 0x82720C30;
// The game's clock has 1 ms resolution, so its accumulator lags real time by
// up to 1 ms: wake this much before the computed update, the original polling
// covers the rest.
constexpr uint64_t kWakeEarlyNs = 1200000;
thread_local uint32_t tl_limiter_object = 0;
// Return address of the flag wait's call (bl sub_825F7168 at 0x82176330); it
// yields this many times as the original, then naps kFlagNapNs at a time.
constexpr uint32_t kFlagWaitReturn = 0x82176334;
constexpr uint32_t kFlagSpins = 64;
constexpr uint64_t kFlagNapNs = 250000;
thread_local uint32_t tl_flag_spins = 0;

// A high-resolution sleep of `ns`; false when the timer is unavailable.
bool Nap(uint64_t ns) {
  thread_local HANDLE timer =
      CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  LARGE_INTEGER due;
  due.QuadPart = -int64_t(ns / 100);
  if (!timer || !SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) return false;
  WaitForSingleObject(timer, 100);
  return true;
}

// Period of the game's 30 Hz loops in its clock (nanoseconds).
constexpr uint64_t kPeriod30 = 33333334;
// Classes (vtables) of the two 30 Hz GameKernel::Loop objects: the one that
// runs scripts and flight (object 05AE7780) and the one that sets the frame
// rate (05AE7830). Both share the update at slot 32 (sub_82196960).
constexpr uint32_t kScriptLoopVtable = 0x82005BC4;
constexpr uint32_t kFrameLoopVtable = 0x82005B98;

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
  if (Load64(base, ctx.r3.u32 + 72) == kPeriod30) {
    // Which 30 Hz loop sr_fps_loops speeds up, a bit each: bit 0 the loop that
    // runs scripts (the meteor scene and the first Metallo fight stall when
    // it runs at 60), bit 1 the one that sets the frame rate. Told apart by
    // their class, not start order: the two threads start within a
    // millisecond, so another CPU may start them the other way round. A loop
    // of any other class takes the next bit in start order.
    static std::atomic<int> count{0};
    const uint32_t loop_vtable = Load32(base, ctx.r3.u32);
    const int index = loop_vtable == kScriptLoopVtable  ? 0
                      : loop_vtable == kFrameLoopVtable ? 1
                                                        : 2 + count.fetch_add(1);
    const bool faster = (limit == 0 || limit > 30) && index < 31 && (REXCVAR_GET(sr_fps_loops) >> index & 1);
    wchar_t* name = nullptr;
    std::string thread;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &name)) && name) {
      for (const wchar_t* c = name; *c; ++c) thread += char(*c < 128 ? *c : '?');
      LocalFree(name);
    }
    REXLOG_INFO("frame limiter: 30 Hz loop #{} ({}) object {:08X} vtable {:08X} thread {} '{}': {}", index,
                index == 0 ? "scripts" : index == 1 ? "frame" : "unknown", ctx.r3.u32, loop_vtable,
                GetCurrentThreadId(), thread, faster ? "at the frame rate limit" : "kept at 30 Hz");
    if (faster) {
      const uint64_t period = limit ? 1000000000ull / uint64_t(limit) : 1000000000ull / 240;
      *reinterpret_cast<uint64_t*>(base + ctx.r3.u32 + 72) = __builtin_bswap64(period);
    }
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
  tl_limiter_object = ctx.r3.u32;
  __imp__sub_82720B50(ctx, base);
  tl_limiter_object = 0;
}

REX_HOOK_RAW(sub_82176300) {
  tl_flag_spins = 0;
  __imp__sub_82176300(ctx, base);
}

// The game's wait (yield, or a sleep when the word at r3 is non-zero), replaced
// at two busy loops: the limiter loop sleeps (high-resolution timer) until
// kWakeEarlyNs before its next update is due, the flag wait naps after
// kFlagSpins yields. Any other caller, a due update, or sr_wait_sleep=false
// runs the original.
REX_HOOK_RAW(sub_825F7168) {
  if (REXCVAR_GET(sr_wait_sleep)) {
    const uint32_t object = tl_limiter_object;
    if (object && ctx.lr == kLimiterWaitReturn) {
      const uint64_t period = Load64(base, object + 72);
      const uint64_t have = Load64(base, object + 104) + Load64(base, object + 112);
      if (have < period && period - have > kWakeEarlyNs &&
          Nap(std::min<uint64_t>(period - have - kWakeEarlyNs, period))) {
        return;
      }
    } else if (ctx.lr == kFlagWaitReturn && ++tl_flag_spins > kFlagSpins && Nap(kFlagNapNs)) {
      return;
    }
  }
  __imp__sub_825F7168(ctx, base);
}
