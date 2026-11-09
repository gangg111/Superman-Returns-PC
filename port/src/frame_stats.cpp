// Guest frame counter for the F3 debug overlay.
//
// sub_82112050 is the game's present routine (the only caller of VdSwap).
// Overriding its weak symbol lets us count presented frames and then run the
// original recompiled body.

#include "frame_stats.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include <rex/hook.h>

#include "generated/default/superman_returns_init.h"
#include "mem_stats.h"
#include "native_renderer/native_bridge.h"
#include "renderdoc_capture.h"
#include "skip_intro.h"

namespace {

std::atomic<uint64_t> g_frames{0};

// SR_LOG_FPS=1 writes the frame rate to the log every 2 seconds, with the
// longest frame of the window and the frames over 50 ms (stutter). A frame
// over 100 ms is also logged on its own, so it lines up with what the log
// says the game was doing at that moment.
void MaybeLogFps(uint64_t frames) {
  using clock = std::chrono::steady_clock;
  static const bool enabled = [] {
    const char* v = std::getenv("SR_LOG_FPS");
    return v && *v && *v != '0';
  }();
  if (!enabled) return;
  static clock::time_point last_time = clock::now();
  static clock::time_point last_frame = last_time;
  static uint64_t last_frames = 0;
  static double max_ms = 0.0;
  static uint32_t hitches = 0;
  const auto now = clock::now();
  const double frame_ms = std::chrono::duration<double, std::milli>(now - last_frame).count();
  last_frame = now;
  max_ms = std::max(max_ms, frame_ms);
  if (frame_ms > 50.0) ++hitches;
  if (frame_ms > 100.0) REXLOG_INFO("guest frame hitch: {:.0f} ms", frame_ms);
  const double elapsed = std::chrono::duration<double>(now - last_time).count();
  if (elapsed < 2.0) return;
  REXLOG_INFO("guest fps: {:.1f} max {:.0f} ms, {} over 50 ms", (frames - last_frames) / elapsed,
              max_ms, hitches);
  last_time = now;
  last_frames = frames;
  max_ms = 0.0;
  hitches = 0;
}

// SR_FRAME_TIMES=<file.csv> writes every guest frame's time (present to
// present, ms) with the time since start (s): the stutter measurement
// (frames well over the frame-rate limit's period).
void MaybeWriteFrameTime() {
  using clock = std::chrono::steady_clock;
  static FILE* file = [] {
    const char* path = std::getenv("SR_FRAME_TIMES");
    FILE* f = path && *path ? std::fopen(path, "w") : nullptr;
    if (f) std::fputs("t_s,frame_ms\n", f);
    return f;
  }();
  if (!file) return;
  static const clock::time_point start = clock::now();
  static clock::time_point last = start;
  const auto now = clock::now();
  std::fprintf(file, "%.4f,%.3f\n", std::chrono::duration<double>(now - start).count(),
               std::chrono::duration<double, std::milli>(now - last).count());
  last = now;
  static uint32_t lines = 0;
  if (++lines % 120 == 0) std::fflush(file);
}

}  // namespace

REX_HOOK_RAW(sub_82112050) {
  const uint64_t frames = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  MaybeLogFps(frames);
  MaybeWriteFrameTime();
  superman_returns::MaybeLogMemory(base);
  OnGuestFrameForCapture(frames);
  superman_returns::IntroSkipOnFrame(ctx, base);
  // sub_82112050 is also the candidate D3DDevice_Swap(dev, front_buffer, ...)
  // of the native renderer (game_profile.h); a no-op unless that is built,
  // confirmed and active.
  const uint32_t device = ctx.r3.u32, front_buffer = ctx.r4.u32;
  __imp__sub_82112050(ctx, base);
  superman_returns::native::OnFrameStatsSwap(base, device, front_buffer);
}

uint64_t GuestFrameCount() { return g_frames.load(std::memory_order_relaxed); }

rex::ui::FrameStats SampleGuestFrameStats() {
  using clock = std::chrono::steady_clock;
  static std::mutex mutex;
  static clock::time_point last_time = clock::now();
  static uint64_t last_frames = 0;
  static rex::ui::FrameStats stats;

  std::lock_guard lock(mutex);
  const uint64_t frames = g_frames.load(std::memory_order_relaxed);
  const auto now = clock::now();
  const double elapsed = std::chrono::duration<double>(now - last_time).count();
  if (elapsed >= 0.5) {
    const uint64_t delta = frames - last_frames;
    stats.fps = delta / elapsed;
    stats.frame_time_ms = delta ? elapsed * 1000.0 / delta : 0.0;
    last_time = now;
    last_frames = frames;
  }
  stats.frame_count = frames;
  return stats;
}
