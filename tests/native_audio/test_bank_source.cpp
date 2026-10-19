#include <memory>

#include "native_audio/bank_source.h"
#include "test_main.h"

using namespace native_audio;

// Input: a 2-channel sound of 7 frames in 2 EAAC blocks (3 + 4 frames),
// interleaved int16 L/R with L = 100*f, R = -100*f - 1.
static std::shared_ptr<const Pcm> MakePcm(SoundInfo* info) {
  info->channels = 2;
  info->samples = 7;
  info->blocks = {3, 4};
  auto pcm = std::make_shared<Pcm>();
  pcm->info = info;
  for (int f = 0; f < 7; ++f) {
    pcm->frames.push_back(int16_t(100 * f));
    pcm->frames.push_back(int16_t(-100 * f - 1));
  }
  return pcm;
}

SR_TEST(BankSourceBlocksFollowTheIndex) {
  SoundInfo info;
  BankSource src(MakePcm(&info));
  SR_CHECK_EQ(src.Channels(), 2);
  SR_CHECK_EQ(src.BlockCount(), 2);    // źródło: blocks = {3, 4}
  SR_CHECK_EQ(src.BlockFrames(0), 3);
  SR_CHECK_EQ(src.BlockFrames(1), 4);
  SR_CHECK_EQ(src.BlockFrames(2), 0);  // poza listą
}

SR_TEST(BankSourceDecodesPlanarLikeTheGameDecoder) {
  SoundInfo info;
  BankSource src(MakePcm(&info));
  float out[16] = {};
  // block 1 starts at frame 3; offset 1 -> frames 4 and 5, planar, stride 2
  const int n = src.Decode(1, 1, out, 2);
  SR_CHECK_EQ(n, 2);
  // źródło: policzone ręcznie, int16 / 32768: 400/32768, 500/32768, -401/32768, -501/32768
  SR_CHECK(out[0] == 0.01220703125f);
  SR_CHECK(out[1] == 0.0152587890625f);
  SR_CHECK(out[2] == -0.012237548828125f);
  SR_CHECK(out[3] == -0.015289306640625f);
}

SR_TEST(BankSourceStopsAtTheBlockEnd) {
  SoundInfo info;
  BankSource src(MakePcm(&info));
  float out[16] = {};
  SR_CHECK_EQ(src.Decode(0, 2, out, 5), 1);  // źródło: blok 0 ma 3 ramki, od 2 zostaje 1
  SR_CHECK(out[0] == 200.0f / 32768.0f);     // źródło: L ramki 2 = 200
}
