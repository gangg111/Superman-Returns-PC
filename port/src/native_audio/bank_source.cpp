#include "native_audio/bank_source.h"

namespace native_audio {

BankSource::BankSource(std::shared_ptr<const Pcm> pcm) : pcm_(std::move(pcm)) {
  start_.push_back(0);
  if (!pcm_ || !pcm_->info) return;
  const SoundInfo& info = *pcm_->info;
  if (info.blocks.empty()) {
    start_.push_back(info.samples);  // index v1: one block holding everything
    return;
  }
  for (uint32_t b : info.blocks) start_.push_back(start_.back() + b);
}

int BankSource::Channels() const { return pcm_ && pcm_->info ? pcm_->info->channels : 0; }

int BankSource::BlockFrames(int i) const {
  if (i < 0 || i >= BlockCount()) return 0;
  return int(start_[size_t(i) + 1] - start_[size_t(i)]);
}

int BankSource::Decode(int block, int offset, float* dst, int frames) {
  const int avail = BlockFrames(block) - offset;
  const int n = frames < avail ? frames : (avail > 0 ? avail : 0);
  const int ch = Channels();
  const size_t total = pcm_ ? pcm_->frames.size() / (ch ? ch : 1) : 0;
  const size_t first = size_t(start_[size_t(block)]) + size_t(offset);
  for (int i = 0; i < n; ++i) {
    const size_t f = first + size_t(i);
    for (int c = 0; c < ch; ++c)
      dst[size_t(c) * n + i] =
          f < total ? float(pcm_->frames[f * ch + c]) * (1.0f / 32768.0f) : 0.0f;
  }
  return n;
}

}  // namespace native_audio
