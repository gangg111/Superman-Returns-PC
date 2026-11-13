// PlayStation pads (DualShock 4, DualSense) through the runtime's SDL3 input
// driver: GameInput (gameinput_driver.h) sees only Xbox pads.
//
// SDL is linked into rexruntime.dll and its functions are not exported, so it
// is limited to Sony pads by its hints as environment variables, set before
// SDL first reads the environment (ps_pad_driver.cpp): only the HIDAPI PS4 and
// PS5 drivers, no XInput, Raw Input, WGI, GameInput or DirectInput, so an Xbox
// pad is not seen twice. A pad seen both ways (DS4Windows, Steam Input) feeds
// guest user 0 from both drivers, merged into one state.
#pragma once

#include <memory>
#include <vector>

#include <rex/input/input_driver.h>

namespace rex::input::sdl {
class SDLInputDriver;
}

namespace superman_returns::input {

class PlayStationPadDriver final : public rex::input::InputDriver {
 public:
  PlayStationPadDriver();
  ~PlayStationPadDriver() override;

  rex::X_STATUS Setup() override;
  void OnWindowAvailable(rex::ui::Window* window) override;
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override;
  rex::X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out_state) override;
  rex::X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                      rex::input::X_INPUT_CAPABILITIES* out_caps) override;
  rex::X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION* vibration) override;
  rex::X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                                   rex::input::X_INPUT_KEYSTROKE* out_keystroke) override;

 private:
  std::unique_ptr<rex::input::sdl::SDLInputDriver> sdl_;
  int logged_count_ = -1;
};

}  // namespace superman_returns::input
