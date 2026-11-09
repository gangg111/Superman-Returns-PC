#include "input/native_input.h"

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/logging.h>

#include "input/gameinput_driver.h"
#include "input/input_recorder.h"
#include "input/rawinput_kbm_driver.h"

REXCVAR_DEFINE_STRING(sr_input, "native", "Superman Returns",
                      "Input drivers: native (GameInput controllers, Raw Input keyboard and "
                      "mouse) or sdk (the SDK's SDL/XInput and SDL-event drivers)")
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
  // Always present; it reports a device only while mnk_mode is on.
  auto kbm = std::make_unique<RawInputKbmDriver>();
  if (kbm->Setup() == X_STATUS_SUCCESS) system->AddDriver(std::move(kbm));
  system->AddDriver(std::make_unique<rex::input::nop::NopInputDriver>(nullptr, 1));
  system->SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());
  REXLOG_INFO("input: native (GameInput, Raw Input)");
  return system;
}

}  // namespace superman_returns::input
