// Recording and replay of the pad state the game reads, for repeatable
// performance runs of a route played by hand.
//
// sr_input_record=<file>: every change of a device's state as the native
// drivers hand it to the game (pads, and keyboard and mouse already turned
// into pad buttons and sticks), with the time since the game's first input
// poll. sr_input_replay=<file>: the game gets that state instead of any real
// device, merged across the recorded devices as the input system merges them.
#pragma once

#include <memory>

#include <rex/input/input_driver.h>

namespace superman_returns::input {

// Called by the native drivers with the state they return to the game.
void RecordInputState(rex::input::DeviceId id, const rex::input::X_INPUT_STATE& state);

// New presses of A on any device, counted from the states the drivers hand to
// the game on its own polling thread (read from any thread; the boot videos'
// skip compares it, skip_intro.cpp). Calling a driver from another thread
// corrupted the heap (2026-11-08).
uint32_t PadAPresses();

// The replay driver when sr_input_replay names a recording, else nullptr.
std::unique_ptr<rex::input::InputDriver> CreateReplayDriver();

}  // namespace superman_returns::input
