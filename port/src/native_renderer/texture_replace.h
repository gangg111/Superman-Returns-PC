// Texture dump and replacement for the remaster (docs/tekstury-remaster.md).
//
// Every 2D game texture has a stable name: the XXH3 hash of its guest bytes
// (the content hash CreateTexture already computes before decoding), plus its
// size and Xenos format: <hash16>_<w>x<h>_f<format>.
//
// sr_texture_dump_dir: on the first upload of each texture, write
// <name>.dds (decoded data, DX10 header, as the game uses it before the fetch
// swizzle) and <name>.json (format, size, mips, gamma, swizzle).
// sr_texture_replace_dir (default <exe>/textures when it exists): a
// <name>.dds there is uploaded instead of the game's texture, with its own
// size, mip count and DXGI format (any DX10 DDS: RGBA8, BC1-BC7). It is
// authored after the swizzle (what the player sees), so it is sampled with
// the identity mapping; gamma textures keep their sRGB view. A <name>.rgb.png
// gives only the colour: the alpha stays the game texture's (GameAlpha below).
#pragma once

#include <cstdint>
#include <string>

#include "texture_decode.h"
#include "../graphics/guest/texture_capture.h"
#include "../graphics/guest/texture_layout.h"

namespace superman_returns::native {

// The content hash textures are named by (XXH3, native_renderer.cpp TextureHash).
uint64_t HashTextureBytes(const void* data, size_t size, uint64_t seed);

// The Vulkan renderer's texture replacement: `game` is the decoded game texture of
// `capture`; when the textures folder or the worn costume slot has a file for it,
// `game` becomes that file, with the same rules as the D3D12 renderer (identity
// mapping, except two-channel game textures keep theirs). `costume` tells the
// caller the texture is a costume one, so it must be uploaded again when another
// slot is worn (CustomCostumeGeneration). PC only (WIC decodes the PNGs).
bool ReplaceCapturedTexture(const graphics::guest::TextureCapture& capture,
                            graphics::guest::LinearTexture& game, bool& costume);

std::string TextureName(uint64_t content_hash, const DecodedTexture& decoded);

// Writes <dir>/<name>.dds and .json once per name; no-op without a dump dir.
void DumpTextureOnce(const std::string& name, const DecodedTexture& decoded, bool gamma);

// The base level of the game's own texture, for replacements named
// <name>.rgb.png (issue #2): their red, green and blue are used with the game's
// alpha, which in many textures is a mask (the face, BC2: 0 = skin) that paint
// programs lose. Read from BC1, BC2, BC3, RGBA8 and BGRA8.
struct GameAlpha {
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint32_t width = 0, height = 0;
  uint32_t row_pitch = 0;  // bytes per row of pixels, or of 4x4 blocks
  const uint8_t* data = nullptr;
  size_t size = 0;
  uint32_t select = 3;  // the view's alpha: decoded channel 0..3, 4 = zero, 5 = one
};

// Loads the replacement for `name`, if any, into `out` (2D, one slice). A
// <name>.rgb.png takes its alpha from `game` and is not used without it.
bool LoadReplacement(const std::string& name, DecodedTexture& out, const GameAlpha* game = nullptr);

bool IsBlockCompressed(DXGI_FORMAT format);

// sr_texture_replace_from_swap (A/B in one run): replacements start at this
// guest swap; the renderer then drops its texture cache so every texture is
// created again with them. 0 = from the start.
uint64_t ReplaceFromSwap();
void SetReplacementsEnabled(bool enabled);

// Custom costume slots (the Fortress's costumes screen, CUSTOM 1..3 after the
// game's three): <exe>/textures/Suits/Slot<n + 3>/ holds the player's PNGs under
// readable names (body.png, briefs_boots.png, cape.png), each standing for the
// game textures of costume 1 listed in texture_replace.cpp, and costume.toml
// with name = "...". While slot n is worn (sr_custom_costume = n) its files come
// before the textures folder; thumb.png and fullsize.png replace the slot's own
// pictures on the screen whichever slot is worn.
constexpr int kCustomCostumes = 3;
int CustomCostume();  // 0: the game's costume
// Wears slot `slot` (0..kCustomCostumes); the renderer recreates the costume's
// textures on its next frame. The caller saves the cvar.
void SetCustomCostume(int slot);
uint32_t CustomCostumeGeneration();
bool IsCostumeTexture(const std::string& name);
// The worn slot's costume.toml has cape = false: the renderer leaves the cape out
// (native_renderer.cpp, the cape's vertex shader). Read when the slot is worn.
bool CostumeCapeHidden();
// The worn slot's costume.toml has cape_inside = "#RRGGBB": that colour (0..1, the
// game's own scale) for the inside of the cape (native_renderer.cpp, the cape's PS
// constant c45). Read when the slot is worn.
bool CostumeCapeInside(float rgb[3]);
// Slot `slot` (1..kCustomCostumes): its costume.toml's cape setting (true unless
// cape = false), and setting it (the line replaced or added; applied at once
// when the slot is worn). The costumes screen's Y button.
bool CostumeCapeShown(int slot);
void SetCostumeCapeShown(int slot, bool shown);
// The slot's name from its costume.toml, else "SLOT <slot + 3>" (printable ASCII, at most 24).
std::string CustomCostumeName(int slot);

}  // namespace superman_returns::native
