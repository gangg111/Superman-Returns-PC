#include "input/native_input.h"

#include <algorithm>
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
// sub_82160A70): this hook is not called then, DirectMouseAlive turns false and the
// mouse is the right stick there, as before.
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

}  // namespace


REX_HOOK_RAW(sub_821FD808) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_821FD808(ctx, base);
  if (!REXCVAR_GET(sr_mouse_direct)) return;
  double dx = 0, dy = 0;
  superman_returns::input::DirectMouseTake(dx, dy);
  if (!dx && !dy) return;
  const double per = superman_returns::input::DirectMouseDegreesPerCount() * 0.017453292519943295;
  const float yaw = float(dx * per);
  float pitch = float(-dy * per);  // mouse up (dy < 0) looks up
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
