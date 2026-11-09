// The native audio engine's sounds (port/src/native_audio, sample_bank.cpp) from
// the player's game, as tools/audio/convert_audio.py and build_index.py make them:
// every single-sound record ("sabr", EA-XMA) of every archive (and the archives
// nested in them) decoded to 16-bit WAV, and native_audio\index.bin naming them.
#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "util.h"

namespace srb {

struct AudioResult {
  int sounds = 0, exact = 0, failed = 0;
};

// data: the game's DATA folder; out: native_audio; ffmpeg: the folder of the
// FFmpeg DLLs. progress(done 0..1, name). Throws Failure("cancelled").
AudioResult ConvertAudio(const fs::path& data, const fs::path& out, const fs::path& ffmpeg,
                         std::atomic<bool>& cancel, const std::function<void(double, const std::string&)>& progress);

}  // namespace srb
