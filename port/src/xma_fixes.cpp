// XMA starvation fix for the audio mixer (sub_826592F0).
//
// The mixer busy-waits until each XMA context's output write offset gives it
// enough samples, retrying the same stream while r26 != 0 with no timeout.
// Under the runtime's XMA emulation some contexts never deliver:
//
//  * The runtime decodes one pass per kick and then disables the context,
//    while real hardware keeps decoding. A pass that produced too few samples
//    is never followed by another one. -> kick the context again.
//  * The stream's sound has finished and its context was already released
//    (the whole context block is zeroed), but the stream is still in the mix
//    list. A released context can never produce output. -> stop waiting.
//  * The context has run out of input (a streamed sound, such as Jor-El's
//    voice in the intro, waiting for its next disc read) or its output buffer
//    is full or flagged invalid ("full") while the game's read offset update
//    races the decoder. -> stop waiting as soon as a kick makes no progress.
//
// Stopping the wait (r26 = 0) makes the mixer move on to the next stream, so
// at worst one sound skips part of a mix pass. Before this fix the audio
// thread spun forever, stopped flushing its 1 MB command queue
// (sub_8264C540), and the queue overflowed into the heap, crashing a minute
// or more into 3D scenes (calls to 0xC003000C, writes to 0x8264CE28).
//
// The runtime's XMAEnableContext decodes inline (XmaDecoder::WriteRegister
// runs XmaContext::Work on the calling thread), so when the kick returns the
// context already holds everything it can decode. If the context did not
// change, nothing will arrive while the mixer keeps spinning: the data it
// needs comes from guest threads that run after the mix pass. An earlier
// version waited up to 4 ms per starved stream on every mix pass; with a
// streamed voice split over several contexts that added up to more than one
// audio frame per pass, the mixer fell behind real time and only the sound
// stuttered while the game kept running normally.
//
// Mid-asm hook at 0x826595B8 (`cmplwi cr6,r26,0` before the retry branch);
// r31 points at the stream, whose first word points at {xma_context*, ...}.

#include <atomic>
#include <chrono>
#include <map>
#include <cstdlib>
#include <thread>

#include <rex/hook.h>
#include <rex/logging.h>

#include "generated/default/superman_returns_init.h"

namespace {

using Clock = std::chrono::steady_clock;

// Safety net for a context that keeps making progress without ever giving
// the mixer enough samples. Well under one 256-sample audio frame (5.3 ms).
constexpr auto kMaxStarvedWait = std::chrono::milliseconds(1);

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + addr));
}

// The context words the decoder updates: input buffer valid bits and output
// write offset (dword 0), output buffer valid (dword 1), input read offset
// (dword 2) and output read offset (dword 9).
struct ContextState {
  uint32_t dword0;
  uint32_t dword1;
  uint32_t dword2;
  uint32_t dword9;

  bool operator==(const ContextState&) const = default;
};

ContextState ReadState(const uint8_t* base, uint32_t xma_context) {
  return {Load32(base, xma_context), Load32(base, xma_context + 4),
          Load32(base, xma_context + 8), Load32(base, xma_context + 36)};
}

// Nothing to decode and nothing to read: input buffers 0/1 and the output
// buffer are all invalid (a released context is fully zeroed).
bool IsDeadContext(const ContextState& state) {
  const bool input_valid = (state.dword0 >> 20) & 3;
  const bool output_valid = state.dword1 >> 31;
  return !input_valid && !output_valid;
}

// SR_LOG_XMA=1: every 10 s, how often the mixer found a stream short of
// samples (starved), how many kicks that took, and how many give-ups.
std::atomic<uint64_t> g_starved{0}, g_kicks{0}, g_giveups{0};

void MaybeLogCounters() {
  static const bool enabled = [] {
    const char* v = std::getenv("SR_LOG_XMA");
    return v && *v && *v != '0';
  }();
  if (!enabled) return;
  static std::atomic<int64_t> last{0};
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          Clock::now().time_since_epoch()).count();
  int64_t prev = last.load();
  if (prev == 0) {
    last.compare_exchange_strong(prev, now);
    return;
  }
  if (now - prev < 10 || !last.compare_exchange_strong(prev, now)) return;
  REXAPU_INFO("XMA fix: starved {} kicks {} give-ups {} (cumulative)", g_starved.load(),
              g_kicks.load(), g_giveups.load());
}

// SR_LOG_XMA=2: every 10 s, the streams the mixer gave up on, with their XMA
// context and reason (matches the context addresses of xma_log_contexts).
// Called from the single mixer thread only.
void CountGiveUp(const char* reason, uint32_t xma_context, uint32_t stream) {
  static const bool enabled = [] {
    const char* v = std::getenv("SR_LOG_XMA");
    return v && std::atoi(v) >= 2;
  }();
  if (!enabled) return;
  struct Entry { uint32_t context; const char* reason; uint32_t count; };
  static std::map<uint32_t, Entry> streams;
  static Clock::time_point last = Clock::now();
  auto& entry = streams[stream];
  entry.context = xma_context;
  entry.reason = reason;
  ++entry.count;
  if (Clock::now() - last < std::chrono::seconds(10)) return;
  last = Clock::now();
  for (const auto& [address, e] : streams) {
    REXAPU_INFO("XMA fix: stream {:08X} context {:08X} gave up {} times ({})", address, e.context,
                e.count, e.reason);
  }
  streams.clear();
}

void LogGiveUp(const char* reason, uint32_t xma_context, uint32_t stream) {
  static std::atomic<uint32_t> count{0};
  CountGiveUp(reason, xma_context, stream);
  g_giveups.fetch_add(1, std::memory_order_relaxed);
  if (count.fetch_add(1) < 10) {
    REXAPU_INFO("XMA fix: stopped waiting on context {:08X} (stream {:08X}): {}", xma_context,
                stream, reason);
  }
}

}  // namespace

void XmaKickStarvedContext(PPCRegister& r26, PPCRegister& r31) {
  // The audio mixer runs on a single guest thread.
  static thread_local uint32_t waiting_stream = 0;
  static thread_local Clock::time_point waiting_since;

  MaybeLogCounters();
  if (r26.u32 == 0) {  // Enough samples; the mixer is not waiting.
    waiting_stream = 0;
    return;
  }
  g_starved.fetch_add(1, std::memory_order_relaxed);

  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  const uint32_t stream = r31.u32;
  const uint32_t xma_context = Load32(base, Load32(base, stream));

  auto give_up = [&](const char* reason) {
    LogGiveUp(reason, xma_context, stream);
    waiting_stream = 0;
    r26.u64 = 0;
  };

  if (!xma_context) {
    give_up("released");
    return;
  }
  const ContextState before = ReadState(base, xma_context);
  if (IsDeadContext(before)) {
    give_up("released");
    return;
  }

  const auto now = Clock::now();
  if (stream != waiting_stream) {
    waiting_stream = stream;
    waiting_since = now;
  } else if (now - waiting_since > kMaxStarvedWait) {
    give_up("timed out");
    return;
  }

  PPCContext call_ctx{};
  call_ctx.r3.u64 = xma_context;
  g_kicks.fetch_add(1, std::memory_order_relaxed);
  __imp__XMAEnableContext(call_ctx, base);

  // The kick decoded synchronously. No change means no input left or no room
  // in the output buffer; spinning cannot fix either.
  if (ReadState(base, xma_context) == before) {
    give_up("no progress");
  }
}
