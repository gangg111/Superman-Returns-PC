// Day and night. The sun the game lights with is baked into every material's
// parameters (gLightPos, a far point ~20 degrees up), while the cascaded shadow
// maps come from the shadow object's light matrix, which sub_823028A8 (per
// frame, from sub_82302D90) reads at r3 + 16..64: three scaled orientation rows
// (an orthographic light projection; light travels along row0 x row1, ~44
// degrees down, the same azimuth as gLightPos), then a position row it snaps
// to texels. The rows are set when the level loads and stay put.
//
// Step 1 (shadows): sr_time_of_day >= 0 moves the sun along our own path and
// rotates the orientation rows (from the level's values, keeping their scale,
// i.e. the cascade sizes) by the smallest rotation that takes the game's light
// direction to the new one. The port's ray-traced sun follows the cascades.
// The path is anchored so that at kAnchorHour the sun stands where the game's
// is: same azimuth, same elevation.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#include "generated/default/superman_returns_init.h"
#include "daynight.h"

#include <atomic>

REXCVAR_DEFINE_BOOL(sr_daynight_log, false, "Superman Returns",
                    "Debug: log the shadow object's light matrix (day/night development)");
REXCVAR_DEFINE_DOUBLE(sr_time_of_day, -1.0, "Superman Returns",
                      "Day and night: the hour (0-24) the sun stands at; -1 = the game's own sun")
    .range(-1.0, 24.0);
REXCVAR_DEFINE_DOUBLE(sr_night_windows, 1.0, "Superman Returns",
                      "Brightness of the city's lit windows at night (0 = dark windows)");
REXCVAR_DEFINE_DOUBLE(sr_daynight_cycle_minutes, 0.0, "Superman Returns",
                      "Day and night: real minutes per 24-hour day from sr_time_of_day on; 0 = the time stays put")
    .range(0.0, 1440.0);

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kNoonElevation = 65.0;          // degrees
constexpr double kShadowMinElevation = 12.0;     // cascades degenerate near the horizon
constexpr double kShadowMaxElevation = 75.0;     // and the game's up vector near the zenith

struct V3 {
  double x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Norm(V3 a) {
  const double l = std::sqrt(Dot(a, a));
  return l > 0 ? a * (1.0 / l) : a;
}
// Rodrigues: rotate v about unit axis k by angle a.
V3 Rotate(V3 v, V3 k, double a) {
  const double c = std::cos(a), s = std::sin(a);
  return v * c + Cross(k, v) * s + k * (Dot(k, v) * (1.0 - c));
}

float LoadF(const uint8_t* base, uint32_t addr) {
  const uint32_t u = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + addr));
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
void StoreF(uint8_t* base, uint32_t addr, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  *reinterpret_cast<uint32_t*>(base + addr) = __builtin_bswap32(u);
}

// The level's orientation rows (xyz of +16, +32, +48) and what was last written.
struct Baseline {
  uint32_t obj = 0;
  V3 rows[3] = {};
  float written[9] = {};
  bool valid = false;
} g_base;

// The sun's elevation (degrees) at an hour: an arc from 6:00 to 18:00.
double SunElevation(double hour) { return kNoonElevation * std::sin(kPi * (hour - 6.0) / 12.0); }

// The game's shadow sun as the hook last saw it (the renderer reads these).
std::atomic<double> g_game_elevation{0.0};  // degrees, 0 = not known yet
std::atomic<double> g_anchor_hour{0.0};

// On: a set hour, or a running day (which starts from the game's own sun
// when no hour is set).
bool DayNightOn() {
  return REXCVAR_GET(sr_time_of_day) >= 0.0 || REXCVAR_GET(sr_daynight_cycle_minutes) > 0.0;
}

double CurrentHour() {
  const double set = REXCVAR_GET(sr_time_of_day);
  const double start = set >= 0.0 ? set : g_anchor_hour.load(std::memory_order_relaxed);
  const double minutes = REXCVAR_GET(sr_daynight_cycle_minutes);
  if (minutes <= 0.0) return start;
  // The day restarts from the start hour whenever the menu changes it.
  static std::mutex lock;
  static auto t0 = std::chrono::steady_clock::now();
  static double last_set = set, last_minutes = minutes;
  std::lock_guard guard(lock);
  if (set != last_set || minutes != last_minutes) {
    t0 = std::chrono::steady_clock::now();
    last_set = set;
    last_minutes = minutes;
  }
  const double real_min = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 60.0;
  return std::fmod(start + real_min / minutes * 24.0, 24.0);
}

double Smoothstep(double a, double b, double x) {
  const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}
float Lerp(float a, float b, double t) { return float(a + (b - a) * t); }

}  // namespace

namespace superman_returns::daynight {

bool CurrentLighting(LightingFrame& out) {
  const double el0 = g_game_elevation.load(std::memory_order_relaxed);
  if (!DayNightOn() || el0 <= 0.0) return false;
  const double h = CurrentHour();
  const double el = SunElevation(h);  // the true elevation (negative at night)
  out.active = true;
  out.azimuth_delta = (h - g_anchor_hour.load(std::memory_order_relaxed)) * (kPi / 12.0);
  out.elevation_ratio = el / el0;
  // Day amount (dusk from 8 degrees above to 16 below the horizon, about an
  // hour and a half: a long twilight) and the
  // warmth of a low sun; at the game's own elevation both leave colours as is.
  const double day = Smoothstep(-16.0, 8.0, el);
  const double warm = (1.0 - Smoothstep(4.0, 22.0, el)) * day;
  const float moon[3] = {0.10f, 0.13f, 0.22f}, sky_night[3] = {0.22f, 0.25f, 0.38f},
              fog_night[3] = {0.07f, 0.09f, 0.16f};
  const float sun_low[3] = {1.0f, 0.62f, 0.38f}, sky_low[3] = {0.95f, 0.78f, 0.70f},
              fog_low[3] = {1.0f, 0.70f, 0.50f};
  for (int i = 0; i < 3; ++i) {
    out.key[i] = Lerp(moon[i], Lerp(1.0f, sun_low[i], warm), day);
    out.ambient[i] = Lerp(sky_night[i], Lerp(1.0f, sky_low[i], warm * 0.6), day);
    out.fog[i] = Lerp(fog_night[i], Lerp(1.0f, fog_low[i], warm), day);
  }
  out.sun_visibility = float(Smoothstep(-2.0, 4.0, el));
  // Windows and lamps light up through dusk (sun from 6 degrees above to 8 below).
  out.night = float(1.0 - Smoothstep(-8.0, 6.0, el));
  out.windows = out.night * float(REXCVAR_GET(sr_night_windows));
  // Rec. 709 luminance of the mean of the key and ambient factors.
  float mean[3];
  for (int i = 0; i < 3; ++i) mean[i] = 0.5f * (out.key[i] + out.ambient[i]);
  out.exposure = 0.2126f * mean[0] + 0.7152f * mean[1] + 0.0722f * mean[2];
  return true;
}

void MoveSunPosition(const LightingFrame& frame, float xyz[3]) {
  const double x = xyz[0], y = xyz[1], z = xyz[2];
  const double r = std::sqrt(x * x + y * y + z * z);
  if (r <= 0.0) return;
  const double el = std::asin(std::clamp(y / r, -1.0, 1.0)) * frame.elevation_ratio;
  const double az = std::atan2(z, x) + frame.azimuth_delta;
  xyz[0] = float(r * std::cos(el) * std::cos(az));
  xyz[1] = float(r * std::sin(el));
  xyz[2] = float(r * std::cos(el) * std::sin(az));
}

}  // namespace superman_returns::daynight

REX_HOOK_RAW(sub_823028A8) {
  const uint32_t obj = ctx.r3.u32;
  if (DayNightOn()) {
    float cur[9];
    for (int i = 0; i < 3; ++i)
      for (int k = 0; k < 3; ++k) cur[i * 3 + k] = LoadF(base, obj + 16 + 16 * i + 4 * k);
    // A new level (or a different object): the game's own values become the baseline.
    if (!g_base.valid || g_base.obj != obj || std::memcmp(cur, g_base.written, sizeof(cur)) != 0) {
      g_base.obj = obj;
      for (int i = 0; i < 3; ++i) g_base.rows[i] = {cur[i * 3], cur[i * 3 + 1], cur[i * 3 + 2]};
      g_base.valid = true;
    }
    // The game's light travel direction and its azimuth / elevation.
    const V3 travel0 = Norm(Cross(g_base.rows[0], g_base.rows[1]));
    const V3 sun0 = travel0 * -1.0;
    const double el0 = std::asin(std::clamp(sun0.y, -1.0, 1.0)) * 180.0 / kPi;
    const double az0 = std::atan2(sun0.z, sun0.x);
    // Anchor: the afternoon hour at which our path has the game's elevation.
    const double anchor = 6.0 + 12.0 * (1.0 - std::asin(std::clamp(el0 / kNoonElevation, -1.0, 1.0)) / kPi);
    g_game_elevation.store(el0, std::memory_order_relaxed);
    g_anchor_hour.store(anchor, std::memory_order_relaxed);
    const double h = CurrentHour();
    const double el = std::clamp(SunElevation(h), kShadowMinElevation, kShadowMaxElevation) * kPi / 180.0;
    const double az = az0 + (h - anchor) * (kPi / 12.0);  // 15 degrees an hour
    const V3 sun{std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az)};
    // Smallest rotation sun0 -> sun, applied to every orientation row.
    const V3 axis = Cross(sun0, sun);
    const double s = std::sqrt(Dot(axis, axis)), c = Dot(sun0, sun);
    float out[9];
    for (int i = 0; i < 3; ++i) {
      const V3 r = s > 1e-9 ? Rotate(g_base.rows[i], axis * (1.0 / s), std::atan2(s, c)) : g_base.rows[i];
      out[i * 3] = float(r.x);
      out[i * 3 + 1] = float(r.y);
      out[i * 3 + 2] = float(r.z);
    }
    for (int i = 0; i < 3; ++i)
      for (int k = 0; k < 3; ++k) StoreF(base, obj + 16 + 16 * i + 4 * k, out[i * 3 + k]);
    std::memcpy(g_base.written, out, sizeof(out));
  }
  if (REXCVAR_GET(sr_daynight_log)) {
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    const auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::seconds(5)) {
      last = now;
      std::string s;
      for (uint32_t off = 16; off < 80; off += 16)
        s += fmt::format(" +{}=({:.4f} {:.4f} {:.4f} {:.4f})", off, LoadF(base, obj + off), LoadF(base, obj + off + 4),
                         LoadF(base, obj + off + 8), LoadF(base, obj + off + 12));
      REXLOG_INFO("daynight: hour {:.2f} shadow object {:08X}:{}", CurrentHour(), obj, s);
    }
  }
  __imp__sub_823028A8(ctx, base);
}
