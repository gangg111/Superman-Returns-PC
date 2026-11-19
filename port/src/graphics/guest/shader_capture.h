#pragma once
#include <cstdint>
#include <vector>

namespace superman_returns::graphics::guest {
// Original container captured at creation, before XDK bind-time patching.
// Commands share immutable ownership; reused guest object addresses cannot
// change shaders that have already been enqueued.
struct ShaderCapture {
  uint64_t hash = 0;
  bool vertex = false;
  bool dynamic_vertex_fetch = false;
  std::vector<uint8_t> container;
  // Texture fetch slots the shader can read (shader_slots.h), ~0u when unknown.
  uint32_t texture_slots = ~0u;
};
} // namespace superman_returns::graphics::guest
