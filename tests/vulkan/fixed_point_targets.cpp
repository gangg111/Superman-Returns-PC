// The Vulkan tests link sr_vulkan_core without the guest packet decoder
// (render_packet.cpp, part of the game's native library), but render_targets.cpp
// asks it how k_16_16 targets are kept. Same answer as the game's definition.
#include <cstdlib>

namespace superman_returns::graphics::guest {
bool FixedPointTargetsAsFloat() {
  static const bool enabled = [] {const char* v = std::getenv("SR_FIXED_POINT_FLOAT");return !v || *v != '0';}();
  return enabled;
}
}  // namespace superman_returns::graphics::guest
