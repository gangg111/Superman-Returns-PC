// Controllers through Microsoft GameInput (GameInput.dll, Windows SDK
// gameinput.lib): one guest pad per connected GameInput gamepad, in
// connection order, with rumble on the two motors.
#pragma once

#include <mutex>
#include <vector>

#include <rex/input/input_driver.h>

struct IGameInput;
struct IGameInputDevice;

namespace superman_returns::input {

using rex::X_RESULT;
using rex::X_STATUS;

// Connected GameInput controllers, and when one was last used (steady clock ms;
// 0 = never): the cursor hides while a pad is the input in use.
int ConnectedPads();
int64_t LastPadUseMs();
int64_t NowMs();

class GameInputDriver final : public rex::input::InputDriver {
 public:
  GameInputDriver();
  ~GameInputDriver() override;

  X_STATUS Setup() override;
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override;
  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                 rex::input::X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(rex::input::DeviceId id,
                              rex::input::X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                              rex::input::X_INPUT_KEYSTROKE* out_keystroke) override;

  // GameInput device callback (system thread).
  void OnDevice(IGameInputDevice* device, bool connected);

 private:
  struct Pad {
    rex::input::DeviceId id;
    IGameInputDevice* device;  // AddRef'd while listed
    uint32_t packet = 0;
    uint64_t last_timestamp = 0;
  };
  Pad* Find(rex::input::DeviceId id);

  IGameInput* input_ = nullptr;
  uint64_t callback_token_ = 0;
  std::mutex mutex_;
  std::vector<Pad> pads_;
  uint64_t next_id_ = 0x47490001;  // "GI": never reused within a run
};

}  // namespace superman_returns::input
