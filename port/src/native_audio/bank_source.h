// SoundSource over a converted sound from the SampleBank: the EAAC block
// layout of the original data (index v2+) and the PCM decoded once offline,
// converted like the game's EXm0 decoder does (int16 / 32768, exact).
#pragma once

#include <memory>
#include <vector>

#include "native_audio/sample_bank.h"
#include "native_audio/snd_player.h"

namespace native_audio {

class BankSource : public SoundSource {
 public:
  explicit BankSource(std::shared_ptr<const Pcm> pcm);
  int Channels() const override;
  int BlockCount() const override { return int(start_.size()) - 1; }
  int BlockFrames(int i) const override;
  int Decode(int block, int offset, float* dst, int frames) override;

 private:
  std::shared_ptr<const Pcm> pcm_;
  std::vector<uint32_t> start_;  // first frame of each block, plus the end
};

}  // namespace native_audio
