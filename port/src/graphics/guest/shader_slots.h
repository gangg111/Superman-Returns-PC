#pragma once
#include <cstdint>
#include <span>

namespace superman_returns::graphics::guest {
// The texture fetch slots (0-31, a bit each) a guest shader can read, from its XDK container: the
// constIndex of every texture fetch instruction its exec clauses run, plus every sampler register of
// its constant table (the recompiled shader may read a declared sampler the original never fetched,
// e.g. mat_map of the window shaders). ~0u (every slot) when the container cannot be read: callers
// treat that as "unknown", as before.
//
// Why: the game leaves fetch constants of earlier draws bound, so a draw saw many bound slots while
// its shaders read a few; capture, decode and recording did each one's work for every draw (on the
// Fold7 in the city: CaptureTextures alone 8.8% of the game's render thread, 2026-11-19).
uint32_t ShaderTextureSlots(std::span<const uint8_t> container);

// The float constant registers (c0-c255 of the shader's own bank, a bit each) a guest shader can read,
// from its microcode: every constant operand of the ALU instructions its exec clauses run. `all` when a
// constant operand is addressed relatively (a0/aL) or the container cannot be read: the whole bank.
//
// Why: the recorder copied both 4 KB banks into each draw's constant block when they changed, ~12 MB a
// frame of mapped memory on the Fold7 in the city (memcpy 14% of the recorder thread, 2026-11-20),
// while most shaders read a few registers.
struct ShaderConstantUse {
  bool all = true;
  uint64_t used[4]{};  // register r: used[r / 64] >> (r % 64) & 1
  bool Reads(uint32_t r) const { return all || (used[r >> 6] >> (r & 63) & 1); }
};
ShaderConstantUse ShaderFloatConstants(std::span<const uint8_t> container);
}  // namespace superman_returns::graphics::guest
