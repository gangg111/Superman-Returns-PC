// Checks the ARM64 NEON path of Pm4Mirror::WriteRange against the scalar loop: the same seeded
// stream of type-0 constant writes, then one checksum of the mirror's registers, versions, written
// flags and both sanitized float banks (which depend on the dirty marks). Run it twice on the
// phone, with SR_PM4_NEON=1 and SR_PM4_NEON=0: the two lines must be identical.
//   aarch64-linux-android31-clang++ -std=c++20 -O2 -I<stub> -I<simde> -Iport/src/native_renderer
//     tests/native/pm4_neon_equivalence.cpp port/src/native_renderer/pm4_mirror.cpp
//     port/src/graphics/guest/constant_snapshot.cpp -o pm4_neon_equivalence
#include "../../port/src/native_renderer/pm4_mirror.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

using superman_returns::native::Pm4Mirror;

static uint64_t Mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001b3ull; }

int main() {
  Pm4Mirror mirror;
  std::mt19937 rng(20261119);
  // A small value pool, so many writes repeat what a register holds (the unchanged lanes).
  const uint32_t pool[]{0x3F800000u, 0x00000000u, 0x40000000u, 0xBF800000u, 0x12345678u, 0x7F7FFFFFu};
  uint64_t h = 0xcbf29ce484222325ull;
  std::vector<uint8_t> stream;
  for (int packet = 0; packet < 20000; ++packet) {
    const uint32_t first = Pm4Mirror::kAluConstantBase + rng() % 0x800;
    const uint32_t room = Pm4Mirror::kAluConstantBase + 0x800 - first;
    const uint32_t count = 1 + rng() % std::min<uint32_t>(room, 700);
    stream.clear();
    const uint32_t header = ((count - 1) << 16) | (first & 0x7FFF);  // type 0, not one-register
    for (int s = 24; s >= 0; s -= 8) stream.push_back(uint8_t(header >> s));
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t v = rng() % 3 ? pool[rng() % 6] : uint32_t(rng());
      for (int s = 24; s >= 0; s -= 8) stream.push_back(uint8_t(v >> s));
    }
    mirror.ScanCopy(nullptr, stream.data(), uint32_t(stream.size()));
    if (packet % 7 == 0) {
      for (bool pixel : {false, true})
        for (uint32_t v : mirror.FloatConstants(pixel)) h = Mix(h, v);
    }
  }
  for (uint32_t r = Pm4Mirror::kAluConstantBase; r < Pm4Mirror::kAluConstantBase + 0x800; ++r)
    h = Mix(Mix(h, mirror.reg(r)), mirror.written(r));
  for (bool pixel : {false, true})
    for (uint32_t v : mirror.FloatConstants(pixel)) h = Mix(h, v);
  h = Mix(Mix(h, mirror.vs_version), mirror.ps_version);
  std::printf("SR_PM4_NEON=%s checksum %016llx vs_version %llu ps_version %llu\n",
              std::getenv("SR_PM4_NEON") ? std::getenv("SR_PM4_NEON") : "(unset)",
              static_cast<unsigned long long>(h), static_cast<unsigned long long>(mirror.vs_version),
              static_cast<unsigned long long>(mirror.ps_version));
}
