// The boot logo/legal videos (EA_HD_logo, EA_ID_OLD, DC_logo, WBIE_logo,
// DC_WB_legal): A on a pad or Escape skips the one playing (each press one
// video); sr_skip_intro skips them all.
//
// sub_822A8BC0 starts the sequence: unless a global "sequence disabled" flag
// is set, it prepares the player (sub_822B72F0), begins the sequence
// (sub_82470628) and plays table[this+560] via sub_822B7F38. sub_822A8C68
// advances on each video end (sub_822AB748, event 0) and, after the last one,
// marks the sequence done (this+560 = -1, this+564 = 1) and ends it with
// sub_82470700. With sr_skip_intro we run the same setup and go straight to
// that "done" state.
//
// A is counted from the states the game polls (input::PadAPresses); reading the
// pads from this thread corrupted the heap (two crashes in the menu, 2026-11-08).
//
// The video player is the object at [0x829761E0]: [+4] is the video playing,
// and sub_8235C780 stops it (as the game's own stop binding, sub_822A9130,
// does); the sequence then moves on as at the video's end.

#include "skip_intro.h"

#include <windows.h>

#include <rex/hook.h>
#include <rex/logging.h>

#include "generated/default/superman_returns_init.h"
#include "input/input_recorder.h"
#include "sr_settings.h"

namespace {

constexpr uint32_t kPlayer = 0x829761E0;

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + addr));
}

void Store32(uint8_t* base, uint32_t addr, uint32_t value) {
  *reinterpret_cast<uint32_t*>(base + addr) = __builtin_bswap32(value);
}

uint32_t g_sequence = 0;  // the sequence object while its videos play
uint32_t g_a_presses = 0;  // input::PadAPresses() at the last frame
bool g_escape = false;

// Escape or the left mouse button while one of the game's windows has the focus.
bool EscapeDown() {
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  return pid == GetCurrentProcessId() &&
         ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0 || (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
}

}  // namespace

void superman_returns::IntroSkipOnFrame(PPCContext& ctx, uint8_t* base) {
  if (!g_sequence) return;
  const int32_t index = int32_t(Load32(base, g_sequence + 560));
  if (index < 0 || base[g_sequence + 564]) {  // the sequence is over
    g_sequence = 0;
    return;
  }
  // A as the game's own polls saw it (never a driver call from this thread).
  const uint32_t presses = superman_returns::input::PadAPresses();
  const bool escape = EscapeDown();
  const bool a = presses != g_a_presses;
  const bool pressed = a || (escape && !g_escape);
  g_a_presses = presses;
  g_escape = escape;
  const uint32_t player = Load32(base, kPlayer);
  if (!pressed || !player || !Load32(base, player + 4)) return;
  REXLOG_INFO("intro: video {} skipped ({})", index, a ? "A" : "Escape or click");
  PPCContext saved = ctx;
  ctx.r3.u64 = player;
  sub_8235C780(ctx, base);
  ctx = saved;
}

REX_HOOK_RAW(sub_822A8BC0) {
  // Mirrors the original early-out: [[0x829761E0] + 4] != 0 disables the sequence.
  const bool sequence_disabled = Load32(base, Load32(base, kPlayer) + 4) != 0;
  if (!REXCVAR_GET(sr_skip_intro) || sequence_disabled) {
    if (!sequence_disabled) {
      g_sequence = ctx.r3.u32;
      g_a_presses = superman_returns::input::PadAPresses();  // presses before the videos do not count
      g_escape = true;  // Escape or a button already held is not a press
    }
    __imp__sub_822A8BC0(ctx, base);
    return;
  }

  const uint32_t self = ctx.r3.u32;
  sub_822B72F0(ctx, base);  // this = r3
  base[self + 553] = 0;
  sub_82470628(ctx, base);  // begin sequence
  Store32(base, self + 560, 0xFFFFFFFF);
  base[self + 564] = 1;
  sub_82470700(ctx, base);  // end sequence
}
