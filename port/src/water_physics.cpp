// Water physics (in progress): what the game does when something meets the
// water. sr_water_log logs the water reactions found by their names
// ("WaterCrash" -> sub_822792F8, "AboveWaterReaction" -> sub_82279620) with
// their arguments, to find the code that keeps objects on (or under) the water.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include "generated/default/superman_returns_init.h"

REXCVAR_DEFINE_BOOL(sr_water_log, false, "Superman Returns",
                    "Debug: log the game's water reactions (WaterCrash, AboveWaterReaction) and their arguments");

namespace {

float LoadF(const uint8_t* base, uint32_t addr) {
  uint32_t u;
  std::memcpy(&u, base + addr, 4);
  u = __builtin_bswap32(u);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

void StoreF(uint8_t* base, uint32_t addr, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  u = __builtin_bswap32(u);
  std::memcpy(base + addr, &u, 4);
}

void LogCall(const char* name, PPCContext& ctx, uint8_t* base) {
  static int logged = 0;
  if (!REXCVAR_GET(sr_water_log) || ++logged > 400) return;
  std::string v;
  const uint32_t obj = ctx.r3.u32;
  if (obj >= 0x10000 && obj < 0x80000000u)
    for (uint32_t off = 0; off < 64; off += 4) v += fmt::format(" {:.2f}", LoadF(base, obj + off));
  REXLOG_INFO("water: {} r3 {:08X} r4 {:08X} r5 {:08X} f1 {:.3f} lr {:08X} |{}", name, ctx.r3.u32, ctx.r4.u32,
              ctx.r5.u32, ctx.f1.f64, uint32_t(ctx.lr), v);
}

}  // namespace

// Havok 3.x motions (hkMotion, from the game's reflection data): motion state
// at +0x10 (its transform: rotation rows +0x10/+0x20/+0x30, position +0x40;
// maxLinearVelocity +0xB4), massInv +0xC0, linearDamping +0xC8,
// linearVelocity +0xD0, angularVelocity +0xE0.
REXCVAR_DEFINE_DOUBLE(sr_motion_probe_at_s, 0.0, "Superman Returns",
                      "Debug: at this time scan guest memory for Havok motions near (sr_motion_probe_x, _z) and "
                      "log them every 0.25 s for 15 s");
REXCVAR_DEFINE_DOUBLE(sr_motion_probe_x, 0.0, "Superman Returns", "Debug: see sr_motion_probe_at_s");
REXCVAR_DEFINE_DOUBLE(sr_motion_probe_z, 0.0, "Superman Returns", "Debug: see sr_motion_probe_at_s");
REXCVAR_DEFINE_DOUBLE(sr_motion_probe_r, 80.0, "Superman Returns", "Debug: see sr_motion_probe_at_s");

namespace superman_returns {

namespace {
float V3Len(const uint8_t* base, uint32_t a) {
  const float x = LoadF(base, a), y = LoadF(base, a + 4), z = LoadF(base, a + 8);
  return std::sqrt(x * x + y * y + z * z);
}
std::vector<uint32_t> g_motions;
double g_track_until = 0.0, g_last_track = 0.0;

std::string MotionText(const uint8_t* base, uint32_t m) {
  return fmt::format("{:08X} pos ({:.2f} {:.2f} {:.2f}) vel ({:.2f} {:.2f} {:.2f}) damp {:.3f} massInv {:.4f} maxV {:.1f}",
                     m, LoadF(base, m + 0x40), LoadF(base, m + 0x44), LoadF(base, m + 0x48), LoadF(base, m + 0xD0),
                     LoadF(base, m + 0xD4), LoadF(base, m + 0xD8), LoadF(base, m + 0xC8), LoadF(base, m + 0xC0),
                     LoadF(base, m + 0xB4));
}
}  // namespace

// Called every draw (the renderer's probe hook) with the elapsed time.
void MotionProbe(uint8_t* base, double now_s) {
  static bool scanned = false;
  const double at = REXCVAR_GET(sr_motion_probe_at_s);
  if (at <= 0.0) return;
  if (!scanned && now_s >= at) {
    scanned = true;
    const float cx = float(REXCVAR_GET(sr_motion_probe_x)), cz = float(REXCVAR_GET(sr_motion_probe_z));
    const float r = float(REXCVAR_GET(sr_motion_probe_r));
    for (uint64_t a = 0x10000; a < 0x80000000ull;) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) break;
      const uint64_t end = uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - base) + mbi.RegionSize;
      const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY));
      if (readable) {
        const uint32_t from = uint32_t(static_cast<uint8_t*>(mbi.BaseAddress) - base);
        for (uint64_t o = 0x40; o + 0xF0 <= mbi.RegionSize && g_motions.size() < 64; o += 16) {
          const uint32_t pos = from + uint32_t(o);
          const float x = LoadF(base, pos), z = LoadF(base, pos + 8);
          if (!(std::fabs(x - cx) < r && std::fabs(z - cz) < r)) continue;
          const uint32_t m = pos - 0x40;
          const float l0 = V3Len(base, m + 0x10), l1 = V3Len(base, m + 0x20), l2 = V3Len(base, m + 0x30);
          if (std::fabs(l0 - 1) > 0.02f || std::fabs(l1 - 1) > 0.02f || std::fabs(l2 - 1) > 0.02f) continue;
          const float damp = LoadF(base, m + 0xC8), inv = LoadF(base, m + 0xC0);
          if (!(damp >= 0.0f && damp < 20.0f && inv >= 0.0f && inv < 1000.0f)) continue;
          g_motions.push_back(m);
        }
      }
      if (end <= a) break;
      a = end;
    }
    REXLOG_INFO("motion probe: {} candidates near ({:.0f}, {:.0f})", g_motions.size(), cx, cz);
    g_track_until = now_s + 15.0;
  }
  if (!g_motions.empty() && now_s < g_track_until && now_s - g_last_track >= 0.25) {
    g_last_track = now_s;
    for (uint32_t m : g_motions) REXLOG_INFO("motion {:.2f} s {}", now_s, MotionText(base, m));
  }
}

}  // namespace superman_returns

// Water drag and buoyancy. sub_825A2258 integrates one Havok motion per
// physics step: r3 the motion, r4 the step (dt at +8), r5 the gravity
// velocity change it adds (gravity x dt); it then scales the velocity by
// (1 - linearDamping x dt) and clamps it to maxLinearVelocity. In the original
// game the water does nothing: a thrown object falls through it at full speed
// (measured 2026-11-05, the Daily Planet globe: -77 -> -94 m/s past the water
// level 9.29) and is dropped some metres below. Here a body under the surface
// gets, for this one call, a higher damping (more for deeper and faster) and
// part of its gravity taken back (buoyancy): heavy bodies sink slowly, light
// ones (under about 100 kg) float.
REXCVAR_DEFINE_BOOL(sr_water_physics, true, "Superman Returns",
                    "Water resistance and buoyancy for thrown and falling objects (the game has none)");
REXCVAR_DEFINE_DOUBLE(sr_water_level, 9.292, "Superman Returns",
                      "World height of the open water (the game's constant 820219C4)");

namespace {

struct WaterEffect {
  bool active = false;
  uint32_t motion = 0;
  uint32_t damping_bits = 0;  // the game's own damping, restored after the step
};

WaterEffect BeginWater(PPCContext& ctx, uint8_t* base) {
  WaterEffect fx;
  if (!REXCVAR_GET(sr_water_physics)) return fx;
  const uint32_t m = ctx.r3.u32, step = ctx.r4.u32, grav = ctx.r5.u32;
  if (m < 0x10000 || m >= 0x80000000u || step < 0x10000 || grav < 0x10000) return fx;
  const float mass_inv = LoadF(base, m + 0xC0);
  if (!(mass_inv > 0.0f) || mass_inv > 100.0f) return fx;  // fixed or kinematic (carried)
  const float y = LoadF(base, m + 0x44);
  const float radius = std::fmax(LoadF(base, m + 0xB0), 0.25f);  // motion state objectRadius
  const float depth = float(REXCVAR_GET(sr_water_level)) - y;    // of the centre below the surface
  if (depth < -radius) return fx;
  const float sub = std::clamp((depth + radius) / (2.0f * radius), 0.0f, 1.0f);  // submerged share
  const float vx = LoadF(base, m + 0xD0), vy = LoadF(base, m + 0xD4), vz = LoadF(base, m + 0xD8);
  const float speed = std::sqrt(vx * vx + vy * vy + vz * vz);
  // Damping per second: a steady part and a quadratic one (fast entries brake hard).
  const float drag = sub * (2.0f + 0.06f * speed);
  // Buoyancy as a share of gravity: heavy bodies (mass 500 kg and up) 0.6, light
  // ones (100 kg and under) 1.6, so they float with part of them above the surface.
  const float mass = 1.0f / mass_inv;
  const float light = std::clamp((500.0f - mass) / 400.0f, 0.0f, 1.0f);
  const float lift = sub * (0.6f + 1.0f * light);
  const float gy = LoadF(base, grav + 4);  // this step's gravity velocity change (negative)
  StoreF(base, m + 0xD4, vy - gy * lift);
  fx.active = true;
  fx.motion = m;
  std::memcpy(&fx.damping_bits, base + m + 0xC8, 4);
  StoreF(base, m + 0xC8, LoadF(base, m + 0xC8) + drag);
  static int logged = 0;
  if (REXCVAR_GET(sr_water_log) && logged++ < 60)
    REXLOG_INFO("water: motion {:08X} y {:.2f} depth {:.2f} sub {:.2f} v ({:.1f} {:.1f} {:.1f}) mass {:.0f} drag {:.2f} "
                "lift {:.2f}", m, y, depth, sub, vx, vy, vz, mass, drag, lift);
  return fx;
}

}  // namespace

REX_HOOK_RAW(sub_825A2258) {
  const WaterEffect fx = BeginWater(ctx, base);
  __imp__sub_825A2258(ctx, base);
  if (fx.active) std::memcpy(base + fx.motion + 0xC8, &fx.damping_bits, 4);
}

REX_HOOK_RAW(sub_822792F8) {
  LogCall("WaterCrash", ctx, base);
  __imp__sub_822792F8(ctx, base);
}

REX_HOOK_RAW(sub_82279620) {
  LogCall("AboveWaterReaction", ctx, base);
  __imp__sub_82279620(ctx, base);
}
