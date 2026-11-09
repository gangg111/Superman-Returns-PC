#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace superman_returns {

// Once a guest frame on the game thread (the present hook): while the boot
// videos play, a new press of A or Escape stops the one playing.
void IntroSkipOnFrame(PPCContext& ctx, uint8_t* base);

}  // namespace superman_returns
