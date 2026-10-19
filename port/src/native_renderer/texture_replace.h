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
// the identity mapping; gamma textures keep their sRGB view.
#pragma once

#include <cstdint>
#include <string>

#include "texture_decode.h"

namespace superman_returns::native {

std::string TextureName(uint64_t content_hash, const DecodedTexture& decoded);

// Writes <dir>/<name>.dds and .json once per name; no-op without a dump dir.
void DumpTextureOnce(const std::string& name, const DecodedTexture& decoded, bool gamma);

// Loads the replacement for `name`, if any, into `out` (2D, one slice).
bool LoadReplacement(const std::string& name, DecodedTexture& out);

bool IsBlockCompressed(DXGI_FORMAT format);

// sr_texture_replace_from_swap (A/B in one run): replacements start at this
// guest swap; the renderer then drops its texture cache so every texture is
// created again with them. 0 = from the start.
uint64_t ReplaceFromSwap();
void SetReplacementsEnabled(bool enabled);

}  // namespace superman_returns::native
