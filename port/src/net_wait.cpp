// The boot's network wait. Early in the boot (sub_8229F460, the game's online setup) the game
// starts EA DirtySock's NetConn and then waits up to 10 s for it to come up:
//
//   deadline = now_ms() + 10000                       ; sub_82564E80
//   while (NetConnStatus('conn') != '+isp' && now_ms() < deadline)
//     NetConnIdle()                                    ; sub_82564FC8, pumps every 5 ms
//
// NetConnStatus is sub_82565FD0 (status selector in r3, four characters; 'conn' reads the
// connection state at module+308). The port has no Xbox network, so the state never becomes
// '+isp' and the boot always spent the whole 10 s with no frame on screen: measured on the
// Fold7 (2026-11-19, simpleperf from launch) as 11 s of the main thread at 100% of a core in
// this loop (NetConnStatus, the clock, XNotifyGetNext), on the PC as the watchdog's
// "no swap for 6002 ms" with the same stack.
//
// Only this loop is answered '+isp' (recognized by the return address of its two calls), so it
// ends at once; every other NetConnStatus caller sees the real state. sr_skip_net_wait=false
// restores the console's wait.
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "generated/default/superman_returns_init.h"

REXCVAR_DEFINE_BOOL(sr_skip_net_wait, true, "Superman Returns",
                    "Skip the boot's 10 s wait for a network connection the port never has");

namespace {
constexpr uint32_t kConn = 0x636F6E6E;  // 'conn'
constexpr uint32_t kIsp = 0x2B697370;   // '+isp'
// Return addresses of NetConnStatus('conn') in the wait (sub_8229F460): before the loop, in it.
constexpr uint32_t kWaitFirst = 0x8229F5BC, kWaitLoop = 0x8229F5EC;
}  // namespace

REX_HOOK_RAW(sub_82565FD0) {
  if (ctx.r3.u32 == kConn && (ctx.lr == kWaitFirst || ctx.lr == kWaitLoop) && REXCVAR_GET(sr_skip_net_wait)) {
    REXLOG_INFO("boot: skipped the 10 s network wait (no Xbox network in the port)");
    ctx.r3.u64 = kIsp;
    return;
  }
  __imp__sub_82565FD0(ctx, base);
}
