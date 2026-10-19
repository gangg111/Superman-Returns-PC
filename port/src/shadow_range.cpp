// Shadow range (remaster option): the dynamic shadow map object built by
// ShadowMapCreate_RW (sub_82309ED8) keeps its cascade layout in fields the
// rest of the shadow code derives everything from (measured 2026-10-19):
//   +712 float 30.0  size of the first cascade
//   +720 int   3     cascades
//   +724 float 5.0   ratio between cascades
// so the far edge is 30 * 5^2 = 750 units (the receivers' FalloffStart 562.5 =
// 0.75 * 750, FalloffRate 1 / 187.5). sr_shadow_range scales the first cascade
// size after the original constructor ran; every cascade and the falloff
// follow. At the same shadow map resolution each cascade then covers more
// ground with the same number of texels, so it pairs with
// sr_native_shadow_quality (range 4 at quality 4 keeps the console's texel
// density).

#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "generated/default/superman_returns_init.h"

REXCVAR_DEFINE_DOUBLE(sr_shadow_range, 4.0, "Superman Returns",
                      "Dynamic shadow range multiplier (1 = original 750 units; the first cascade "
                      "and every cascade after it grow by this factor)")
    .range(1.0, 16.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

float LoadFloat(uint8_t* base, uint32_t address) {
  uint32_t word;
  std::memcpy(&word, base + address, 4);
  return std::bit_cast<float>(std::byteswap(word));
}

void StoreFloat(uint8_t* base, uint32_t address, float value) {
  const uint32_t word = std::byteswap(std::bit_cast<uint32_t>(value));
  std::memcpy(base + address, &word, 4);
}

}  // namespace

// ShadowMapCreate_RW: r3 = the shadow map object it initialises.
REX_HOOK_RAW(sub_82309ED8) {
  const uint32_t object = ctx.r3.u32;
  __imp__sub_82309ED8(ctx, base);
  const float range = float(REXCVAR_GET(sr_shadow_range));
  if (!object || range == 1.0f) return;
  const float first = LoadFloat(base, object + 712);
  StoreFloat(base, object + 712, first * range);
  REXLOG_INFO("sr_shadow_range={}: first cascade {} -> {} (ratio {}, far edge {})", range, first,
              first * range, LoadFloat(base, object + 724),
              first * range * LoadFloat(base, object + 724) * LoadFloat(base, object + 724));
}
