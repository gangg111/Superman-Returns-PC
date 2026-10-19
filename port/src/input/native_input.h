// Native Windows input for the game (replaces the SDK's SDL/XInput/SDL-event
// drivers): controllers through GameInput (Xbox Series and other GameInput
// gamepads), keyboard and mouse through Raw Input. The game still reads its
// pads through XamInputGetState; only the drivers behind the SDK's
// InputSystem change. sr_input=sdk restores the SDK's default drivers.
#pragma once

#include <memory>

#include <rex/system/interfaces/input.h>

namespace superman_returns::input {

std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode);

}  // namespace superman_returns::input
