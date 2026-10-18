// Guest physical memory diagnostics (SR_LOG_MEM). See mem_stats.cpp.
#pragma once

#include <cstdint>

namespace superman_returns {

// Called once per presented guest frame; logs a heap snapshot every
// SR_LOG_MEM seconds when the variable is set.
void MaybeLogMemory(uint8_t* base);

}  // namespace superman_returns
