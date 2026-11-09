// The remastered textures (the builder window's switch): our own files from the payload,
// and the ones made from the player's game: the night windows' maps (the window
// rectangles of a building's material map, tools/textures/make_window_map.py) and the sky
// without its painted sun (the game's sky plus our difference, tools/textures/nosun.py).
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "util.h"

namespace srb {

class Pack;

// RGBA8 rows of a game texture's first level, decoded as Pillow does (the tools read the
// renderer's dumps with Pillow): Xenos 2D tiling, 8-in-16 byte order, BC1 or BC3.
std::vector<uint8_t> DecodeGameTexture(const std::vector<uint8_t>& tiled, uint32_t width, uint32_t height,
                                       uint32_t guest_format);

// The window map DDS (make_window_map.py, byte for byte) from RGBA8 rows and the glass weights.
std::vector<uint8_t> WindowMap(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height,
                               const double kenv[3]);

// textures.txt in the payload (tools/builder/make_builder.py):
//   own <payload path>                         copied to textures\<path after textures/>
//   root <payload path>                        copied to the game's folder (the night's
//                                              lamp_cache.bin: street lamps' positions)
//   src <name> <game path> <offset> <size> <w> <h> <format> <sha256>
//   window <name> <kr> <kg> <kb>               textures\windows\<name>_win.dds
//   diff <name> <payload path>                 textures\<name>.dds: the header, then
//                                              (game RGBA + difference) mod 256
// Returns the number of files written.
int MakeTextures(const Pack& pack, const fs::path& game_dir, const fs::path& out_dir, std::atomic<bool>& cancel,
                 const std::function<void(double, const std::string&)>& progress);

}  // namespace srb
