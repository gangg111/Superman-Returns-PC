#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "native_audio/sample_bank.h"
#include "test_main.h"

using native_audio::SampleBank;

SR_TEST(Fnv1aMatchesTheConverter) {
  // convert_audio.py fnv1a(b"abc") == 0x1A47E90B
  const uint8_t abc[] = {'a', 'b', 'c'};
  SR_CHECK_EQ(SampleBank::Fnv1a(abc, 3), 0x1A47E90Bu);
  SR_CHECK_EQ(SampleBank::Fnv1a(nullptr, 0), 2166136261u);
}

SR_TEST(MissingIndexGivesAnEmptyBank) {
  SampleBank bank;
  SR_CHECK(!bank.Open("Z:/does/not/exist"));
  SR_CHECK(bank.empty());
}

// Needs the converted sounds: SR_NATIVE_AUDIO_DIR=<SupermanReturnsPC>/native_audio
SR_TEST(EveryInMemorySoundResolvesAndLoadsExactly) {
  const char* dir = std::getenv("SR_NATIVE_AUDIO_DIR");
  if (!dir) {
    std::printf("  skipped: SR_NATIVE_AUDIO_DIR not set\n");
    return;
  }
  SampleBank bank;
  SR_CHECK(bank.Open(dir));
  SR_CHECK_EQ(bank.size(), size_t(4648));
  // Every fnv key maps to a sound with that key, and a sample of sounds
  // (every 50th, both kinds) loads with exactly samples x channels values.
  size_t checked = 0, failed = 0;
  for (size_t i = 0; i < bank.size(); i += 50) {
    const auto& all = bank.FindBySnr(nullptr, 0);
    const native_audio::SoundInfo* s = all[i];
    if (s->has_fnv) SR_CHECK(bank.FindByFnv(s->fnv256)->fnv256 == s->fnv256);
    auto pcm = bank.Load(*s);
    ++checked;
    if (!pcm || pcm->frames.size() != size_t(s->samples) * s->channels) {
      ++failed;
      std::printf("  load failed: %s\n", s->name.c_str());
    }
  }
  std::printf("  loaded %zu sounds, %zu failed\n", checked, failed);
  SR_CHECK_EQ(failed, size_t(0));
}
