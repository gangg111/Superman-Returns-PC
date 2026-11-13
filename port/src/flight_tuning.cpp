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

#include <windows.h>

#include <cstdio>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

#include "generated/default/superman_returns_init.h"

REXCVAR_DECLARE(int32_t, sr_native_fps_limit);
REXCVAR_DEFINE_BOOL(sr_flight_log, false, "Superman Returns",
                    "Debug: log Superman's flight mode tuning as the game loads it");
REXCVAR_DEFINE_INT32(sr_flight_scale_mode, -1, "Superman Returns",
                     "Debug: the flight mode (r5 of sub_821F9FA0) whose top speeds sr_flight_scale multiplies");
REXCVAR_DEFINE_DOUBLE(sr_flight_scale, 1.0, "Superman Returns", "Debug: see sr_flight_scale_mode");
REXCVAR_DEFINE_BOOL(sr_flight_stop_30hz, true, "Superman Returns",
                    "Above 30 FPS the stop after super speed takes off as much speed in its one update as at 30 FPS");
REXCVAR_DEFINE_BOOL(sr_flight_probe, false, "Superman Returns",
                    "Debug: log the flight controller's float triples that look like a velocity, every 0.1 s");

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
// The stop after super speed: 223.5 m/s at 89.4 m/s^2 takes 2.5 s; with the
// decel scaled by the run's speed-up, any boosted speed stops in the same time.
constexpr int kBrakeWindowMs = 2600;

struct SuperSpeed {
  uint32_t mode = 0;            // the live mode struct (guest address)
  float top[3] = {};            // its own top speeds (+0, +4, +8)
  int stage = 0;                // booms in this run of super speed
  double run_s = 0.0;           // game time in this run
  double first_boom_s = -1.0;   // game time of the first boom, -1 = not yet
  float brake_scale = 1.0f;     // the last run's speed-up, for the stop after it
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

// The stop recording (sr_flight_probe), shared with the body's setters below.
FILE* g_stop_csv = nullptr;
std::chrono::steady_clock::time_point g_stop_start;
uint32_t g_stop_body = 0;  // controller + 100 of the recorded flight
struct ProbeSample {
  std::chrono::steady_clock::time_point at;
  float speed;
  uint32_t mode;
};
std::array<ProbeSample, 256> g_probe_ring{};  // speeds before the recording starts
size_t g_probe_ring_at = 0;
uint32_t g_probe_ring_body = 0;
uint32_t g_superman_body = 0;  // the flight controller's body (controller + 100)
bool g_in_super_speed = false; // the current flight motion update is in the super speed mode
uint32_t g_probe_mode = 0;

// The flight controller's update (controller r3, frame time f1).
REX_HOOK_RAW(sub_82224A90) {
  g_flight_dt = ctx.f1.f64;
  __imp__sub_82224A90(ctx, base);
}

// The flight motion (controller r3, mode r4), every frame.
REX_HOOK_RAW(sub_8220C878) {
  const uint32_t mode = ctx.r4.u32;
  if (ctx.r3.u32 >= 0x10000)
    g_superman_body = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + ctx.r3.u32 + 100));
  g_in_super_speed = mode && IsSuperSpeedMode(base, mode);
  if (REXCVAR_GET(sr_flight_probe)) {
    // Debug: from leaving super speed, 5 s of every update into
    // flight_stop_<fps>.csv next to the exe: time, dt, mode, then the words of
    // the controller (0x300 bytes) and of its body (controller + 100, 0x300).
    static uint32_t prev_mode = 0;
    FILE*& csv = g_stop_csv;
    auto& start = g_stop_start;
    const auto now = std::chrono::steady_clock::now();
    const uint32_t ctl = ctx.r3.u32;
    if (!csv && prev_mode && mode != prev_mode && IsSuperSpeedMode(base, prev_mode) && ctl >= 0x10000) {
      wchar_t exe[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, exe, MAX_PATH);
      std::wstring path(exe);
      path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"flight_stop_" +
             std::to_wstring(REXCVAR_GET(sr_native_fps_limit)) + L".csv";
      csv = _wfopen(path.c_str(), L"w");
      start = now;
      g_stop_body = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + ctl + 100));
      REXLOG_INFO("flight probe: recording the stop after super speed");
      // the last second before it, from the ring: "R,t,speed,mode" with t < 0
      const size_t n = std::min(g_probe_ring_at, g_probe_ring.size());
      for (size_t i = g_probe_ring_at - n; i < g_probe_ring_at; ++i) {
        const ProbeSample& s = g_probe_ring[i % g_probe_ring.size()];
        const double t = std::chrono::duration<double>(s.at - now).count();
        if (csv && t > -1.0) fprintf(csv, "R,%.4f,%.3f,%08X\n", t, s.speed, s.mode);
      }
    }
    prev_mode = mode;
    g_probe_mode = mode;
    if (ctl >= 0x10000) g_probe_ring_body = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + ctl + 100));
    {
      // the body's methods the flight motion calls: +44 (velocity), +52 (decel)
      static bool logged = false;
      const uint32_t b = ctl >= 0x10000 ? __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + ctl + 100)) : 0;
      const uint32_t vt = b >= 0x10000 ? __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + b)) : 0;
      if (!logged && vt >= 0x82000000u && vt < 0x83000000u) {
        logged = true;
        const auto at = [&](uint32_t o) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + vt + o)); };
        REXLOG_INFO("flight probe: body {:08X} vtable {:08X} +44 {:08X} +48 {:08X} +52 {:08X}", b, vt, at(44), at(48),
                    at(52));
      }
    }
    if (csv) {
      const double t = std::chrono::duration<double>(now - start).count();
      const uint32_t body = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + ctl + 100));
      fprintf(csv, "%.4f,%.5f,%08X", t, g_flight_dt, mode);
      for (uint32_t obj : {ctl, body})
        for (uint32_t off = 0; off < 0x300; off += 4)
          fprintf(csv, ",%08X", obj >= 0x10000 ? __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + obj + off)) : 0u);
      // One pointer further: for each word of the controller and its body
      // that points into guest memory, the lengths of the float triples of
      // the object it points to (0x400 bytes), as "src:off:len" (1..5000).
      for (uint32_t obj : {ctl, body}) {
        if (obj < 0x10000) continue;
        for (uint32_t off = 0; off < 0x300; off += 4) {
          const uint32_t p = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + obj + off));
          if (p < 0x00100000 || p >= 0x20000000 || (p & 3)) continue;
          for (uint32_t o = 0; o + 12 <= 0x400; o += 4) {
            const float x = LoadF(base, p + o), y = LoadF(base, p + o + 4), z = LoadF(base, p + o + 8);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
            const float len = std::sqrt(x * x + y * y + z * z);
            if (len >= 1.0f && len <= 5000.0f)
              fprintf(csv, ",%s%X:%X=%.2f", obj == ctl ? "c" : "b", off, o, len);
          }
        }
      }
      fputc('\n', csv);
      if (t > 5.0) {
        fclose(csv);
        csv = nullptr;
        REXLOG_INFO("flight probe: stop recorded");
      }
    }
  }
  if (false) {
    // Debug: every 0.1 s, each float triple in the controller's first 0x800
    // bytes whose length is 20..2000 (a velocity in m/s), with the mode.
    static auto next = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    // The controller's body (controller + 100: the game sets the mode's decel
    // on it through its vtable +52 and the velocity through +44).
    const uint32_t ctl = ctx.r3.u32;
    const uint32_t c = ctl >= 0x10000 ? __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + ctl + 100)) : 0;
    if (now >= next && c >= 0x10000) {
      next = now + std::chrono::milliseconds(100);
      std::string line;
      for (uint32_t off = 0; off + 12 <= 0x800; off += 4) {
        const float x = LoadF(base, c + off), y = LoadF(base, c + off + 4), z = LoadF(base, c + off + 8);
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
        const float len = std::sqrt(x * x + y * y + z * z);
        if (len >= 20.0f && len <= 2000.0f) line += fmt::format(" {:X}={:.1f}", off, len);
      }
      REXLOG_INFO("flight probe {:08X} mode {:08X}:{}", c, mode, line);
    }
  }
  if (REXCVAR_GET(sr_flight_log)) {
    // the flight mode in use, when it changes: which mode brakes and how long
    static uint32_t last = ~0u;
    static const auto t0 = std::chrono::steady_clock::now();
    if (mode != last && mode) {
      last = mode;
      REXLOG_INFO("flight mode now {:08X} at {:.2f} s: top {:.1f} decel {:.1f} min {:.1f}", mode,
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), LoadF(base, mode),
                  LoadF(base, mode + 56), LoadF(base, mode + 20));
    }
  }
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
    g_ss.brake_scale = scale;
  } else if (mode) {
    // Out of super speed after booms: the next mode (normal flight, decel
    // 89.4 m/s^2) has to take off the extra speed too; at its own decel that
    // kept Superman in the flying animation up to 2.5x as long (5.8 s from
    // 1250 mph instead of 2.0 s from 500; the player's report 2026-11-11).
    // Its decel times the run's speed-up while the stop lasts, then restored.
    static std::unordered_map<uint32_t, float> original;  // mode -> its decel
    const auto now = std::chrono::steady_clock::now();
    const bool braking = g_ss.brake_scale > 1.0f && now - g_ss.last_use < std::chrono::milliseconds(kBrakeWindowMs);
    for (auto it = original.begin(); it != original.end();) {
      if (!braking || it->first != mode) {
        StoreF(base, it->first + 56, it->second);
        it = original.erase(it);
      } else {
        ++it;
      }
    }
    if (braking && !original.count(mode)) {
      const float decel = LoadF(base, mode + 56);
      original[mode] = decel;
      StoreF(base, mode + 56, decel * g_ss.brake_scale);
      if (REXCVAR_GET(sr_flight_log))
        REXLOG_INFO("super speed: stop, mode {:08X} decel {:.1f} x {:.2f}", mode, decel, g_ss.brake_scale);
    }
    if (!braking) g_ss.brake_scale = 1.0f;
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

// The body's velocity (+44, r4 the vector) and decel (+52, f1) setters the
// flight motion calls every update: logged into the stop recording.
REX_HOOK_RAW(vt_821B1D10) {
  if (g_stop_csv && ctx.r3.u32 == g_stop_body) {
    const uint32_t v = ctx.r4.u32;
    const float x = LoadF(base, v), y = LoadF(base, v + 4), z = LoadF(base, v + 8);
    fprintf(g_stop_csv, "V,%.4f,%.3f,%.3f,%.3f,%.3f\n",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - g_stop_start).count(), x, y, z,
            std::sqrt(x * x + y * y + z * z));
    // the physics entity the setter forwards to (body +4 -> +112): its float
    // triples of length 1..2000 (the real velocity is among them)
    const auto u32 = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + a)); };
    const uint32_t inner = u32(ctx.r3.u32 + 4);
    const uint32_t ent = inner >= 0x10000 ? u32(inner + 112) : 0;
    if (ent >= 0x10000) {
      fprintf(g_stop_csv, "E,%.4f,%08X",
              std::chrono::duration<double>(std::chrono::steady_clock::now() - g_stop_start).count(), ent);
      for (uint32_t o = 0; o + 12 <= 0x400; o += 4) {
        const float a = LoadF(base, ent + o), b = LoadF(base, ent + o + 4), c = LoadF(base, ent + o + 8);
        if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c)) continue;
        const float len = std::sqrt(a * a + b * b + c * c);
        if (len >= 1.0f && len <= 2000.0f) fprintf(g_stop_csv, ",%X=%.2f", o, len);
      }
      fputc('\n', g_stop_csv);
    }
  }
  __imp__vt_821B1D10(ctx, base);
}

// The body's +48 method (between the velocity and decel setters): r4 an
// output vector, presumably the current velocity. Logged after the call.
REX_HOOK_RAW(vt_821B1D28) {
  const uint32_t body = ctx.r3.u32, out = ctx.r4.u32;
  __imp__vt_821B1D28(ctx, base);
  if (REXCVAR_GET(sr_flight_probe) && !g_stop_csv && out >= 0x10000 && g_probe_ring_body && body == g_probe_ring_body) {
    // the last second before the recording starts (the frames still in super speed)
    const float x = LoadF(base, out), y = LoadF(base, out + 4), z = LoadF(base, out + 8);
    g_probe_ring[g_probe_ring_at++ % g_probe_ring.size()] = {std::chrono::steady_clock::now(),
                                                            std::sqrt(x * x + y * y + z * z), g_probe_mode};
  }
  if (g_stop_csv && body == g_stop_body && out >= 0x10000) {
    const float x = LoadF(base, out), y = LoadF(base, out + 4), z = LoadF(base, out + 8);
    fprintf(g_stop_csv, "G,%.4f,%.3f,%.3f,%.3f,%.3f\n",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - g_stop_start).count(), x, y, z,
            std::sqrt(x * x + y * y + z * z));
  }
}

REX_HOOK_RAW(vt_821B1D40) {
  // The stop after super speed: the update the key is let go sets a strong
  // decel (4469.4) for that one update, then the normal mode's (89.4). The
  // physics steps at 1/60 s, so at 30 FPS that update spans two steps
  // (223.5 -> 74.5), at 60 FPS one (-> 149.0), and Superman glides twice as
  // long (measured 2026-11-11). Above 30 FPS a strong decel is held until
  // the updates since it make 1/30 s, as one console update.
  if (ctx.r3.u32 == g_superman_body && REXCVAR_GET(sr_flight_stop_30hz)) {
    static float strong = 0.0f;  // the strong decel being held, 0 = none
    static double held = 0.0;    // update time since it was set
    const double dt = std::clamp(g_flight_dt, 0.0, 0.1);
    // only the strong decel of the update that leaves super speed (the mode
    // is still super speed's): scripted flight sets strong decels too, and
    // holding those could keep Superman from where a script waits for him
    if (ctx.f1.f64 >= 1000.0 && g_in_super_speed) {
      strong = float(ctx.f1.f64);
      held = dt;
    } else if (strong > 0.0f && held < 1.0 / 30.0 - 1e-4) {
      if (REXCVAR_GET(sr_flight_log))
        REXLOG_INFO("flight: decel {:.1f} held instead of {:.1f} ({:.4f} s of 1/30)", strong, ctx.f1.f64, held);
      ctx.f1.f64 = strong;
      held += dt;
    } else {
      strong = 0.0f;
    }
  }
  if (g_stop_csv && ctx.r3.u32 == g_stop_body)
    fprintf(g_stop_csv, "D,%.4f,%.3f\n",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - g_stop_start).count(), ctx.f1.f64);
  __imp__vt_821B1D40(ctx, base);
}
