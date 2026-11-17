#include "input/native_input.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/logging.h>

#include "input/gameinput_driver.h"
#include "input/ps_pad_driver.h"
#include "input/input_recorder.h"
#include "input/rawinput_kbm_driver.h"
#include "generated/default/superman_returns_init.h"

REXCVAR_DECLARE(bool, sr_mouse_direct);

REXCVAR_DEFINE_STRING(sr_input, "native", "Superman Returns",
                      "Input drivers: native (GameInput controllers, Raw Input keyboard and "
                      "mouse) or sdk (the SDK's SDL/XInput and SDL-event drivers)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(sr_playstation_pads, true, "Superman Returns",
                    "DualShock 4 and DualSense pads through SDL (ps_pad_driver.h), beside GameInput's Xbox pads")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace superman_returns::input {

std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode) {
  if (tool_mode || REXCVAR_GET(sr_input) != "native") {
    REXLOG_INFO("input: SDK drivers");
    return rex::input::CreateDefaultInputSystem(tool_mode);
  }
  auto system = std::make_unique<rex::input::InputSystem>(nullptr);
  // sr_input_replay: the recording is the only input.
  if (auto replay = CreateReplayDriver()) {
    system->AddDriver(std::move(replay));
    system->AddDriver(std::make_unique<rex::input::nop::NopInputDriver>(nullptr, 1));
    system->SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());
    return system;
  }
  auto pads = std::make_unique<GameInputDriver>();
  if (pads->Setup() == X_STATUS_SUCCESS) system->AddDriver(std::move(pads));
  if (REXCVAR_GET(sr_playstation_pads)) {
    auto ps = std::make_unique<PlayStationPadDriver>();
    if (ps->Setup() == X_STATUS_SUCCESS) system->AddDriver(std::move(ps));
  }
  // Always present; it reports a device only while mnk_mode is on.
  auto kbm = std::make_unique<RawInputKbmDriver>();
  if (kbm->Setup() == X_STATUS_SUCCESS) system->AddDriver(std::move(kbm));
  system->AddDriver(std::make_unique<rex::input::nop::NopInputDriver>(nullptr, 1));
  // Every pad drives player one. Only player one is signed in (the SDK's
  // XamUserGetSigninState), so a second device on player two (Steam Input or
  // DS4Windows beside the real pad, a second controller) made START ask for a
  // gamer profile (issue #3). The game is single-player.
  system->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
  REXLOG_INFO("input: native (GameInput, SDL PlayStation pads, Raw Input)");
  return system;
}

}  // namespace superman_returns::input

// The mouse camera (sr_mouse_direct, the user's request 2026-11-17, as NX2navi's
// mouse_direct): the mouse turns the camera by an angle per count, past the game's
// stick dead zone, curve and smoothing.
//
// sub_821FD808 is the game's camera input (this = 0x00428760 on foot): it reads the
// processed right stick at +136, puts it through its curve (s^2 + k(|s| - s^2)) times
// the speed and dt, and turns the camera's orientation, a quaternion at +208, by that
// angle about the up axis, renormalised. After it, the mouse's counts turn the same
// quaternion: about the world's up axis (left product) for yaw, about the camera's
// own x for pitch (right product), so the next update starts from there. Measured:
// 1 degree a frame held 30.0 deg/s at 30 Hz; 75 counts a command turned 6 degrees.
// The game has no limit of its own on that pitch: it is held within kPitchLimit of level.
// The camera follows Superman until the right stick moves: rawinput_kbm_driver.cpp holds
// the stick just past the game's dead zone while the mouse moves (kMouseWakeStick).
// Flying has its own camera (sub_821855B8, the stick through an acceleration ramp,
// sub_82160A70): sub_821FD808 is not called then, and FlightMouse sets the ramp's
// outputs from the counts instead.
namespace {

constexpr float kPitchLimit = 1.13f;  // 65 degrees up or down

float LoadGuestF32(uint8_t* base, uint32_t at) {
  uint32_t u;
  std::memcpy(&u, base + at, 4);
  u = __builtin_bswap32(u);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

void StoreGuestF32(uint8_t* base, uint32_t at, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  u = __builtin_bswap32(u);
  std::memcpy(base + at, &u, 4);
}

// sub_821FD808 ran within this time: Superman is on foot (the flight camera's update
// runs on foot too, without effect)
std::atomic<int64_t> g_on_foot_ms{0};

int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The flight camera (sub_821855B8) turns by its ramped stick, sub_82160A70's outputs
// +168 (pitch) and +172 (yaw), at a rate linear in them past 1 (measured: 0.5 and 2.0
// turned the camera 58.7 and 234 deg/s of yaw, 0.5 turned it 34.4 deg/s of pitch, within
// the game's own +-40 degree pitch limit). After the ramp, the mouse's counts this update
// become those outputs, so the camera turns the counts' angle in this update.
constexpr double kFlightYawRate = 2.05;   // radians a second at 1
constexpr double kFlightPitchRate = 1.2;  // radians a second at 1

double yaw_left = 0, pitch_left = 0;  // FlightMouse's turn not yet given to the camera
int64_t g_flight_mouse_ms = 0;        // FlightMouse last turned the camera

void FlightMouse(uint8_t* base, uint32_t self, double dt) {
  if (!REXCVAR_GET(sr_mouse_direct) || !(dt > 0.0)) return;
  if (SteadyMs() - g_on_foot_ms.load(std::memory_order_relaxed) < 150) {
    yaw_left = pitch_left = 0;  // nothing carried from one flight to the next
    return;
  }
  double dx = 0, dy = 0;
  superman_returns::input::DirectMouseTake(dx, dy);
  double px, py;
  superman_returns::input::DirectMouseDegreesPerCount(px, py);
  px *= 0.017453292519943295, py *= 0.017453292519943295;
  // The outputs stay within the stick's +-1: past it the camera turned faster than the game
  // turns Superman and circled him (the user, 2026-11-17: "efekt drona"). The rest of the
  // turn (radians) carries to the next updates.
  // the right stick's directions: right is a negative yaw output, up (dy < 0) a negative
  // pitch output (as the mouse through the stick before: up looks up)
  yaw_left += -dx * px;
  pitch_left += dy * py;
  if (!yaw_left && !pitch_left) {
    // Just after the mouse stopped the stick still reports its last turn (rawinput_kbm_driver
    // DirectMouseFlightStick): the camera gets none of it. Otherwise a pad's stick keeps the
    // ramp's outputs.
    if (SteadyMs() - g_flight_mouse_ms < 200) {
      StoreGuestF32(base, self + 172, 0.0f);
      StoreGuestF32(base, self + 168, 0.0f);
      superman_returns::input::DirectMouseFlightStick(0.0f, 0.0f);
    }
    return;
  }
  g_flight_mouse_ms = SteadyMs();
  const double yaw_out = std::clamp(yaw_left / (kFlightYawRate * dt), -1.0, 1.0);
  const double pitch_out = std::clamp(pitch_left / (kFlightPitchRate * dt), -1.0, 1.0);
  yaw_left -= yaw_out * kFlightYawRate * dt;
  pitch_left -= pitch_out * kFlightPitchRate * dt;
  if (std::abs(yaw_left) < 1e-6) yaw_left = 0;
  if (std::abs(pitch_left) < 1e-6) pitch_left = 0;
  // the game holds the pitch within +-40 degrees: a turn past it is not kept for later
  pitch_left = std::clamp(pitch_left, -0.7, 0.7);
  StoreGuestF32(base, self + 172, float(yaw_out));
  StoreGuestF32(base, self + 168, float(pitch_out));
  superman_returns::input::DirectMouseFlightStick(float(yaw_out), float(pitch_out));
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("mouse camera (flight): {} {} counts, a turn of {:.3f} / {:.3f} degrees", dx, dy,
                dx * px * 57.29578, -dy * py * 57.29578);
  }
}

}  // namespace


REX_HOOK_RAW(sub_82160A70) {
  const uint32_t self = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  __imp__sub_82160A70(ctx, base);
  FlightMouse(base, self, dt);
}

REX_HOOK_RAW(sub_821FD808) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_821FD808(ctx, base);
  g_on_foot_ms.store(SteadyMs(), std::memory_order_relaxed);
  if (!REXCVAR_GET(sr_mouse_direct)) return;
  double dx = 0, dy = 0;
  superman_returns::input::DirectMouseTake(dx, dy);
  if (!dx && !dy) return;
  double px, py;
  superman_returns::input::DirectMouseDegreesPerCount(px, py);
  // right (dx > 0) turns as the stick right: the heading 2 atan2(y, w) down (measured: the
  // stick right took it from -17 to -112 degrees in 0.5 s), a negative yaw in the left product
  const float yaw = float(-dx * px * 0.017453292519943295);
  float pitch = float(-dy * py * 0.017453292519943295);  // mouse up (dy < 0) looks up
  const uint32_t q = self + 208;
  float x = LoadGuestF32(base, q), y = LoadGuestF32(base, q + 4), z = LoadGuestF32(base, q + 8),
        w = LoadGuestF32(base, q + 12);
  const float len2 = x * x + y * y + z * z + w * w;
  if (!std::isfinite(len2) || len2 < 1e-6f) return;
  // The quaternion's own y is the world's up while level, its z tilts with the pitch: its
  // world height is -sin(elevation) (measured: 3.2 degrees a step turned it by sin 3.2).
  // The pitch is kept within kPitchLimit of level.
  const float elevation = std::asin(std::clamp(-2 * (y * z - w * x) / len2, -1.0f, 1.0f));
  pitch = std::clamp(pitch, -kPitchLimit - elevation, kPitchLimit - elevation);
  if (yaw != 0.0f) {  // (0, s, 0, c) * q
    const float s = std::sin(yaw / 2), c = std::cos(yaw / 2);
    const float nx = c * x + s * z, ny = c * y + s * w, nz = c * z - s * x, nw = c * w - s * y;
    x = nx, y = ny, z = nz, w = nw;
  }
  if (pitch != 0.0f) {  // q * (s, 0, 0, c)
    const float s = std::sin(pitch / 2), c = std::cos(pitch / 2);
    const float nx = w * s + x * c, ny = y * c + z * s, nz = z * c - y * s, nw = w * c - x * s;
    x = nx, y = ny, z = nz, w = nw;
  }
  const float n = 1.0f / std::sqrt(x * x + y * y + z * z + w * w);
  StoreGuestF32(base, q, x * n);
  StoreGuestF32(base, q + 4, y * n);
  StoreGuestF32(base, q + 8, z * n);
  StoreGuestF32(base, q + 12, w * n);
  // SR_MOUSE_CAMERA_TRACE=1: every take (tests); else the first one
  static const bool trace = [] {
    const char* e = std::getenv("SR_MOUSE_CAMERA_TRACE");
    return e && *e && std::strcmp(e, "0");
  }();
  static bool logged = false;
  if (trace || !logged) {
    logged = true;
    REXLOG_INFO("mouse camera: {} {} counts turned the camera {:.3f} / {:.3f} degrees (elevation now {:.2f})",
                dx, dy, yaw * 57.29578f, pitch * 57.29578f, (elevation + pitch) * 57.29578f);
  }
}
