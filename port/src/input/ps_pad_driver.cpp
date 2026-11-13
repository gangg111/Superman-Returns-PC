#include "input/ps_pad_driver.h"

#include <windows.h>

#include <cstdlib>

#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>

#include "input/gameinput_driver.h"
#include "input/input_recorder.h"

namespace superman_returns::input {

namespace {

// SDL's hints as environment variables (SDL_GetHint reads SDL's copy of the
// environment, taken on its first use): set by a static initializer, before
// any SDL call. Only Sony pads: the HIDAPI PS4 and PS5 drivers, no other
// HIDAPI driver and no XInput, Raw Input, WGI, GameInput or DirectInput (the
// Xbox pads are GameInput's).
struct SdlHints {
  SdlHints() {
    static const char* const kOff[] = {
        "SDL_XINPUT_ENABLED", "SDL_JOYSTICK_RAWINPUT", "SDL_JOYSTICK_WGI", "SDL_JOYSTICK_GAMEINPUT",
        "SDL_JOYSTICK_DIRECTINPUT", "SDL_JOYSTICK_HIDAPI_GAMECUBE", "SDL_JOYSTICK_HIDAPI_JOY_CONS",
        "SDL_JOYSTICK_HIDAPI_LUNA", "SDL_JOYSTICK_HIDAPI_NINTENDO_CLASSIC", "SDL_JOYSTICK_HIDAPI_PS3",
        "SDL_JOYSTICK_HIDAPI_SHIELD", "SDL_JOYSTICK_HIDAPI_STADIA", "SDL_JOYSTICK_HIDAPI_STEAM",
        "SDL_JOYSTICK_HIDAPI_STEAMDECK", "SDL_JOYSTICK_HIDAPI_STEAM_HORI", "SDL_JOYSTICK_HIDAPI_LG4FF",
        "SDL_JOYSTICK_HIDAPI_8BITDO", "SDL_JOYSTICK_HIDAPI_SINPUT", "SDL_JOYSTICK_HIDAPI_ZUIKI",
        "SDL_JOYSTICK_HIDAPI_FLYDIGI", "SDL_JOYSTICK_HIDAPI_GAMESIR", "SDL_JOYSTICK_HIDAPI_SWITCH",
        "SDL_JOYSTICK_HIDAPI_SWITCH2", "SDL_JOYSTICK_HIDAPI_WII", "SDL_JOYSTICK_HIDAPI_XBOX",
        "SDL_JOYSTICK_HIDAPI_XBOX_360", "SDL_JOYSTICK_HIDAPI_XBOX_360_WIRELESS", "SDL_JOYSTICK_HIDAPI_XBOX_ONE",
        "SDL_JOYSTICK_HIDAPI_GIP",
    };
    static const char* const kOn[] = {"SDL_JOYSTICK_HIDAPI", "SDL_JOYSTICK_HIDAPI_PS4", "SDL_JOYSTICK_HIDAPI_PS5"};
    for (const char* name : kOff) Set(name, "0");
    for (const char* name : kOn) Set(name, "1");
  }
  static void Set(const char* name, const char* value) {
    SetEnvironmentVariableA(name, value);
    _putenv_s(name, value);
  }
};
const SdlHints g_sdl_hints;

bool InUse(const rex::input::X_INPUT_STATE& s) {
  const auto& g = s.gamepad;
  constexpr int kStick = 10000;  // about 0.3 of the stick, as GameInputDriver
  return g.buttons != 0 || g.left_trigger > 51 || g.right_trigger > 51 || std::abs(int(g.thumb_lx)) > kStick ||
         std::abs(int(g.thumb_ly)) > kStick || std::abs(int(g.thumb_rx)) > kStick ||
         std::abs(int(g.thumb_ry)) > kStick;
}

}  // namespace

PlayStationPadDriver::PlayStationPadDriver()
    : rex::input::InputDriver(nullptr, 0), sdl_(std::make_unique<rex::input::sdl::SDLInputDriver>(nullptr, 0)) {
  sdl_->set_is_active_callback([this] { return is_active(); });
}

PlayStationPadDriver::~PlayStationPadDriver() { SetPlayStationPads(0); }

rex::X_STATUS PlayStationPadDriver::Setup() { return sdl_->Setup(); }

void PlayStationPadDriver::OnWindowAvailable(rex::ui::Window* window) { sdl_->OnWindowAvailable(window); }

void PlayStationPadDriver::EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) {
  const size_t before = out.size();
  sdl_->EnumerateDevices(out);
  const int count = int(out.size() - before);
  SetPlayStationPads(count);
  if (count != logged_count_) {
    logged_count_ = count;
    for (size_t i = before; i < out.size(); ++i)
      REXLOG_INFO("PlayStation pad: {} connected", out[i].name.empty() ? "a pad" : out[i].name);
    REXLOG_INFO("PlayStation pads: {} connected (SDL HIDAPI)", count);
  }
}

rex::X_RESULT PlayStationPadDriver::GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out_state) {
  rex::input::X_INPUT_STATE state = {};
  const rex::X_RESULT result = sdl_->GetDeviceState(id, &state);
  if (result == X_ERROR_SUCCESS) {
    if (InUse(state)) NotePlayStationPadUse();
    RecordInputState(id, state);
  }
  if (out_state) *out_state = state;
  return result;
}

rex::X_RESULT PlayStationPadDriver::GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                                          rex::input::X_INPUT_CAPABILITIES* out_caps) {
  return sdl_->GetDeviceCapabilities(id, flags, out_caps);
}

rex::X_RESULT PlayStationPadDriver::SetDeviceVibration(rex::input::DeviceId id,
                                                       rex::input::X_INPUT_VIBRATION* vibration) {
  return sdl_->SetDeviceVibration(id, vibration);
}

rex::X_RESULT PlayStationPadDriver::GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                                                       rex::input::X_INPUT_KEYSTROKE* out_keystroke) {
  return sdl_->GetDeviceKeystroke(id, flags, out_keystroke);
}

}  // namespace superman_returns::input
