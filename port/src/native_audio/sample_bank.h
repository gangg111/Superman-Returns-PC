// Converted game sounds (tools/audio/convert_audio.py + build_index.py) for the
// native audio engine. The game identifies a sound by a pointer to its SNR
// header in guest memory (SndPlayer start request, docs/natywny-dzwiek-api.md
// section 4b); the bank maps that to PCM decoded once from the game files.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace native_audio {

struct SoundInfo {
  uint32_t fnv256 = 0;
  bool has_fnv = false;
  uint8_t channels = 0;
  bool streamed = false;
  bool loop = false;
  uint32_t rate = 0;
  uint32_t samples = 0;
  uint32_t loop_start = 0;
  std::array<uint8_t, 16> snr16{};
  std::string file;  // relative to the bank directory
  std::string name;  // "archive/NAME"
  std::vector<uint32_t> blocks;  // frames per EAAC data block (index v2)
};

// Interleaved int16 frames, exactly SoundInfo::samples long.
struct Pcm {
  const SoundInfo* info = nullptr;
  std::vector<int16_t> frames;
};

class SampleBank {
 public:
  // Reads <dir>/index.bin. False (and an empty bank) when it is missing.
  bool Open(const std::string& dir);
  bool empty() const { return sounds_.empty(); }
  size_t size() const { return sounds_.size(); }

  // FNV-1a of the 256 bytes starting at the SNR header (in-memory sounds).
  const SoundInfo* FindByFnv(uint32_t fnv256) const;
  // All sounds whose SNR header starts with these bytes (streamed sounds have
  // no data after the header, so this narrows them before the file name does).
  std::vector<const SoundInfo*> FindBySnr(const uint8_t* snr, size_t len) const;

  // Loads (or returns the cached) PCM; null when the file cannot be read.
  std::shared_ptr<const Pcm> Load(const SoundInfo& info);

  static uint32_t Fnv1a(const uint8_t* data, size_t len);

 private:
  std::string dir_;
  std::vector<SoundInfo> sounds_;
  std::unordered_map<uint32_t, size_t> by_fnv_;
  std::mutex cache_mutex_;
  std::unordered_map<const SoundInfo*, std::weak_ptr<const Pcm>> cache_;
};

}  // namespace native_audio
