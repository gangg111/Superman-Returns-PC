// Where a draw packet takes the state the original XDK code also writes into
// PM4: the parsed command stream (default) or the D3DDevice shadow in guest
// memory. The PM4 audit (SR_NATIVE_PM4_AUDIT) measured the shadow equal to
// PM4 except shader literals (written into the shadow by LoadShaderLiterals in
// device mode), RB_MODECONTROL (taken from the device already) and the inline
// vertex fetch slot; the scissor pair sits at device +0x2844.
//
// SR_NATIVE_STATE_SOURCE=device  build packets from the device shadow
// SR_NATIVE_STATE_COMPARE=1      build from PM4, also derive from the device and
//                                log field mismatches (no image change)
#pragma once

#include <cstdlib>
#include <cstring>

namespace superman_returns::graphics::guest {

inline bool StateFromDevice() {
  static const bool value = [] {
    const char* v = std::getenv("SR_NATIVE_STATE_SOURCE");
    return v && std::strcmp(v, "device") == 0;
  }();
  return value;
}

inline bool StateCompare() {
  static const bool value = [] {
    const char* v = std::getenv("SR_NATIVE_STATE_COMPARE");
    return v && *v && *v != '0';
  }();
  return value;
}

// Which original XDK calls stop running in device mode (Vulkan only):
// SR_NATIVE_SKIP_ORIGINAL=draw,resolve,clear,inline,fence (SR_NATIVE_SKIP_DRAW=1 = draw).
enum class OriginalCall { kDraw = 1, kResolve = 2, kClear = 4, kInline = 8, kFence = 16 };
inline bool SkipOriginalRequested(OriginalCall call) {
  static const unsigned mask = [] {
    unsigned m = 0;
    if (const char* v = std::getenv("SR_NATIVE_SKIP_DRAW"); v && *v && *v != '0') m |= 1;
    if (const char* v = std::getenv("SR_NATIVE_SKIP_ORIGINAL")) {
      if (std::strstr(v, "draw")) m |= 1;
      if (std::strstr(v, "resolve")) m |= 2;
      if (std::strstr(v, "clear")) m |= 4;
      if (std::strstr(v, "inline")) m |= 8;
      if (std::strstr(v, "fence")) m |= 16;
    }
    return StateFromDevice() ? m : 0u;
  }();
  return (mask & unsigned(call)) != 0;
}

// Resolve inputs the PM4 stream used to provide, measured over ~70k resolves
// (SR_NATIVE_RESOLVE_AUDIT): RB_COPY_DEST_INFO swaps endianness exactly for
// destination format 6 (k_8_8_8_8), and a copy draw exists when there is a
// destination texture (17 exceptions out of ~70k).
inline bool ResolveCopyDraw(uint32_t dest_texture) { return dest_texture != 0; }
inline uint32_t ResolveCopyDestInfo(uint32_t dest_fetch1) {
  return (dest_fetch1 & 0x3f) == 6 ? (1u << 24) : 0u;
}

// Device offsets of PA_SC_WINDOW_OFFSET / SCISSOR_TL / SCISSOR_BR (0x2080-0x2082).
inline constexpr uint32_t kDeviceScissor = 0x2840;

}  // namespace superman_returns::graphics::guest
