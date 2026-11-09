// The builder's run: the player's game and the builder's payload into
// <output>\Superman Returns, a clean folder ready to play.
#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "util.h"

namespace srb {

class Pack;

enum class Step { Check, Port, Game, Patch, Textures, Audio, Finish };

struct BuildOptions {
  fs::path source;  // .iso or a folder with default.xex and DATA
  fs::path output;  // the folder that gets "Superman Returns"
  bool textures = true;  // the remastered textures (textures.h)
};

// progress(step, done 0..1, file name); returns the game's folder. Throws Failure
// (a message key, see main.cpp) or another exception (its text) on an error; on
// cancel throws Failure("cancelled"). Nothing is left half made: the work goes to
// "Superman Returns.part" and becomes "Superman Returns" only when complete.
fs::path Build(const Pack& pack, const BuildOptions& options, std::atomic<bool>& cancel,
               const std::function<void(Step, double, const std::string&)>& progress);

constexpr const wchar_t* kGameFolder = L"Superman Returns";

}  // namespace srb
