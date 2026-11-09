// Superman's flight tuning. sub_82237E98 parses the flight configuration (the
// compressed preload data) and, for each flight mode element ("default",
// "launch", "hover", "boost" twice, "brake", "turn", "locked", "strafe", ...),
// sub_821F9FA0 reads its values into a mode struct (r6):
//   +0 maxThrustVelMPH  +4 maxRevThrustVelMPH  +8 maxStrafeVelMPH
//   +16 maxHeightThrustVelMPH  +20 minVelocity  +24/+28 accelVelThresholdMin/MaxMPH
//   +32/+36 thrustAccelMPH / Max  +40/+44 revThrustAccel  +48/+52 strafeAccel
//   +56 decel  +60 angularDamping  +64/+68 rotVelThresholdMin/MaxMPH
//   +88 yawToCameraRateMultiplier  +104 liftAccelMPH  +116 hugDistance
// sr_flight_log logs every mode as it is loaded.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

#include "generated/default/superman_returns_init.h"

REXCVAR_DEFINE_BOOL(sr_flight_log, false, "Superman Returns",
                    "Debug: log Superman's flight mode tuning as the game loads it");
REXCVAR_DEFINE_INT32(sr_flight_scale_mode, -1, "Superman Returns",
                     "Debug: the flight mode (r5 of sub_821F9FA0) whose top speeds sr_flight_scale multiplies");
REXCVAR_DEFINE_DOUBLE(sr_flight_scale, 1.0, "Superman Returns", "Debug: see sr_flight_scale_mode");

namespace {

float LoadF(const uint8_t* base, uint32_t addr) {
  const uint32_t u = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + addr));
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

std::string GuestText(const uint8_t* base, uint32_t addr) {
  if (addr < 0x82000000u || addr >= 0x83000000u) return "-";
  std::string s;
  for (uint32_t i = 0; i < 32 && base[addr + i] >= 0x20 && base[addr + i] < 0x7F; ++i)
    s += char(base[addr + i]);
  return s;
}

uint32_t g_last_mode = ~0u;  // r5 of the last flight mode sub_821F9FA0 read

void StoreF(uint8_t* base, uint32_t addr, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  *reinterpret_cast<uint32_t*>(base + addr) = __builtin_bswap32(u);
}

// Super speed: every sonic boom raises the top speed a step. The mode is the
// one with the super speed tuning (top 500 mph = 223.472 m/s, thrust
// acceleration 1072.667, reverse 2145.333 m/s^2).
// The game booms three times in one run of super speed, on its own clock
// (measured 2026-11-05 on the user's route, the same with a faster Superman,
// so time and not distance): the first about 7.2 s in, the second 20.2 s and
// the third 56.2 s after the first. The first also throws the player camera's
// field of view from 1.70 to about 2.0 rad (sub_827858F0), which places it
// exactly; the later ones show no camera change and follow on the clock.
constexpr double kBoomAfterStart = 7.2;               // the first, when the camera does not show it
constexpr double kBoomAfterFirst[] = {20.2, 56.2};     // the second and third
constexpr double kBoomFov = 1.85;                      // the camera crossing this upwards: the first boom

struct SuperSpeed {
  uint32_t mode = 0;            // the live mode struct (guest address)
  float top[3] = {};            // its own top speeds (+0, +4, +8)
  int stage = 0;                // booms in this run of super speed
  double run_s = 0.0;           // game time in this run
  double first_boom_s = -1.0;   // game time of the first boom, -1 = not yet
  std::chrono::steady_clock::time_point last_use{};
} g_ss;
double g_flight_dt = 0.0;  // the flight controller's frame time (sub_82224A90 f1)

struct CameraFov {
  bool wide = false;
};
std::unordered_map<uint32_t, CameraFov> g_cameras;

bool IsSuperSpeedMode(const uint8_t* base, uint32_t mode) {
  const float accel = LoadF(base, mode + 32), reverse = LoadF(base, mode + 40);
  return std::fabs(accel - 1072.667f) < 0.01f && std::fabs(reverse - 2145.333f) < 0.01f;
}

}  // namespace

REXCVAR_DEFINE_DOUBLE(sr_superspeed_step, 0.5, "Superman Returns",
                      "Super speed: top speed added at every sonic boom, as a share of the game's 500 mph "
                      "(0 = the game's own fixed speed)");
REXCVAR_DEFINE_INT32(sr_superspeed_stages, 3, "Superman Returns", "Super speed: most booms that raise the speed");

namespace {

void Boom(const char* how) {
  if (g_ss.stage >= REXCVAR_GET(sr_superspeed_stages)) return;
  ++g_ss.stage;
  if (g_ss.first_boom_s < 0.0) g_ss.first_boom_s = g_ss.run_s;
  REXLOG_INFO("super speed: sonic boom {} ({}) at {:.2f} s of the run, {:.0f} mph", g_ss.stage, how, g_ss.run_s,
              500.0 * (1.0 + REXCVAR_GET(sr_superspeed_step) * g_ss.stage));
}

}  // namespace

// The flight controller's update (controller r3, frame time f1).
REX_HOOK_RAW(sub_82224A90) {
  g_flight_dt = ctx.f1.f64;
  __imp__sub_82224A90(ctx, base);
}

// The flight motion (controller r3, mode r4), every frame.
REX_HOOK_RAW(sub_8220C878) {
  const uint32_t mode = ctx.r4.u32;
  if (mode && REXCVAR_GET(sr_superspeed_step) > 0.0 && (mode == g_ss.mode || IsSuperSpeedMode(base, mode))) {
    const auto now = std::chrono::steady_clock::now();
    if (mode != g_ss.mode) {
      g_ss = {};
      g_ss.mode = mode;
      for (int k = 0; k < 3; ++k) g_ss.top[k] = LoadF(base, mode + 4 * k);
    }
    // A new run of super speed: back to the game's speed and clock.
    if (now - g_ss.last_use > std::chrono::milliseconds(500)) {
      if (g_ss.stage) REXLOG_INFO("super speed: new run, stage {} -> 0", g_ss.stage);
      g_ss.stage = 0;
      g_ss.run_s = 0.0;
      g_ss.first_boom_s = -1.0;
    }
    g_ss.last_use = now;
    g_ss.run_s += std::clamp(g_flight_dt, 0.0, 0.1);
    if (g_ss.first_boom_s < 0.0) {
      if (g_ss.run_s >= kBoomAfterStart + 1.5) Boom("clock");  // the camera did not show it
    } else if (g_ss.stage < 3 && g_ss.run_s >= g_ss.first_boom_s + kBoomAfterFirst[g_ss.stage - 1]) {
      Boom("clock");
    }
    const float scale = 1.0f + float(REXCVAR_GET(sr_superspeed_step)) * float(g_ss.stage);
    for (int k = 0; k < 3; ++k) StoreF(base, mode + 4 * k, g_ss.top[k] * scale);
  }
  __imp__sub_8220C878(ctx, base);
}

// A camera's horizontal field of view (camera r3, radians f1), when it changes.
REX_HOOK_RAW(sub_827858F0) {
  const double fov = ctx.f1.f64;
  if (REXCVAR_GET(sr_flight_log)) {
    static uint32_t last_cam = 0;
    static double last_fov = 0.0;
    if (ctx.r3.u32 != last_cam || std::fabs(fov - last_fov) > 0.01) {
      REXLOG_INFO("camera fov {:08X} {:.4f} (super speed mode {:08X} stage {})", ctx.r3.u32, fov, g_ss.mode, g_ss.stage);
      last_cam = ctx.r3.u32;
      last_fov = fov;
    }
  }
  if (g_ss.mode && REXCVAR_GET(sr_superspeed_step) > 0.0) {
    const bool flying = std::chrono::steady_clock::now() - g_ss.last_use < std::chrono::milliseconds(200);
    if (g_cameras.size() > 64) g_cameras.clear();
    CameraFov& cam = g_cameras[ctx.r3.u32];
    if (!cam.wide && fov > kBoomFov && flying && g_ss.first_boom_s < 0.0) Boom("camera");
    if (fov > kBoomFov) cam.wide = true;
    else if (fov < kBoomFov - 0.1) cam.wide = false;
  }
  __imp__sub_827858F0(ctx, base);
}

// sub_82231F98(owner r3, slot r4, mode r5): copies the 128-byte mode into the
// mode pool (base from sub_822438C0, index u16 at owner + (slot + 6) * 2).
// The parser's three stores (by owner kind) copy the mode the same way; the
// destination is memcpy's return value, still in r3.
void LogStore(const char* fn, uint32_t owner, uint32_t slot, uint32_t src, uint32_t dest, const uint8_t* base) {
  if (!REXCVAR_GET(sr_flight_log)) return;
  const bool same = dest >= 0x10000 && std::memcmp(base + dest, base + src, 128) == 0;
  REXLOG_INFO("flight mode {} stored by {}: owner {:08X} slot {} dest {:08X} same {} +0={:.3f}", g_last_mode, fn, owner,
              slot, dest, same, same ? LoadF(base, dest) : 0.0f);
}

REX_HOOK_RAW(sub_82231F98) {
  const uint32_t owner = ctx.r3.u32, slot = ctx.r4.u32, src = ctx.r5.u32;
  __imp__sub_82231F98(ctx, base);
  LogStore("82231F98", owner, slot, src, ctx.r3.u32, base);
}

REX_HOOK_RAW(sub_82232060) {
  const uint32_t owner = ctx.r3.u32, slot = ctx.r4.u32, src = ctx.r5.u32;
  __imp__sub_82232060(ctx, base);
  LogStore("82232060", owner, slot, src, ctx.r3.u32, base);
}

REX_HOOK_RAW(sub_822322B8) {
  const uint32_t owner = ctx.r3.u32, slot = ctx.r4.u32, src = ctx.r5.u32;
  __imp__sub_822322B8(ctx, base);
  LogStore("822322B8", owner, slot, src, ctx.r3.u32, base);
}

REX_HOOK_RAW(sub_821F9FA0) {
  const uint32_t mode = ctx.r6.u32;
  const uint32_t r4 = ctx.r4.u32, r5 = ctx.r5.u32;
  g_last_mode = r5;
  __imp__sub_821F9FA0(ctx, base);
  if (int32_t(r5) == REXCVAR_GET(sr_flight_scale_mode)) {
    for (uint32_t off = 0; off < 12; off += 4) {
      float f = LoadF(base, mode + off) * float(REXCVAR_GET(sr_flight_scale));
      uint32_t u;
      std::memcpy(&u, &f, 4);
      *reinterpret_cast<uint32_t*>(base + mode + off) = __builtin_bswap32(u);
    }
  }
  if (!REXCVAR_GET(sr_flight_log)) return;
  std::string v;
  for (uint32_t off = 0; off < 120; off += 4) v += fmt::format(" +{}={:.3f}", off, LoadF(base, mode + off));
  REXLOG_INFO("flight mode {:08X} r4 {:08X} '{}' r5 {:08X} '{}':{}", mode, r4, GuestText(base, r4), r5,
              GuestText(base, r5), v);
}
