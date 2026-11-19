#ifdef _WIN32
#include "../../port/src/native_renderer/guest_hash.h"
#include "test_main.h"
#include "../../port/src/native_renderer/buffer_content.h"
#include <vector>
using superman_returns::native::HashGuestRange;
namespace {
// Qualquer hash com seed serve: o helper precisa devolver exatamente o que o hash de uma cópia devolve.
uint64_t Fnv(const void* data, size_t size, uint64_t seed) {
  uint64_t h = seed ^ 0xcbf29ce484222325ull;
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}
}  // namespace
SR_TEST(buffer_content_checks_page_writes_within_the_frame_and_ignores_neighbors) {
  superman_returns::native::BufferContent content;
  uint64_t bytes=10;int hashes=0;
  auto hash=[&] {++hashes;return bytes;};
  SR_CHECK(!content.Refresh(1,false,hash));SR_CHECK_EQ(hashes,1);
  SR_CHECK(!content.Refresh(1,false,hash));SR_CHECK_EQ(hashes,1);
  SR_CHECK(!content.Refresh(1,true,hash));SR_CHECK_EQ(hashes,2);
  bytes=20;
  SR_CHECK(content.Refresh(1,true,hash));SR_CHECK_EQ(hashes,3);
  SR_CHECK(!content.Refresh(1,true,hash));SR_CHECK_EQ(hashes,4);
  bytes=30; // A virtual-alias write can escape the physical page callback.
  SR_CHECK(content.Refresh(2,false,hash));SR_CHECK_EQ(hashes,5);
  SR_CHECK(!content.Refresh(3,false,hash));SR_CHECK_EQ(hashes,6);
}
SR_TEST(buffer_content_every_use_detects_unnotified_changes_in_the_same_frame) {
  superman_returns::native::BufferContent content;
  uint64_t bytes=17;unsigned hashes=0;
  auto hash=[&] {++hashes;return bytes;};
  SR_CHECK(!content.Refresh(12,false,hash,true));
  SR_CHECK(!content.Refresh(12,false,hash,true));SR_CHECK_EQ(hashes,2u);
  bytes=23;
  SR_CHECK(content.Refresh(12,false,hash,true));SR_CHECK_EQ(hashes,3u);
  SR_CHECK(!content.Refresh(12,false,hash,true));SR_CHECK_EQ(hashes,4u);
  // Original mode still uses the once-per-frame fast path after a full check.
  SR_CHECK(!content.Refresh(12,false,hash));SR_CHECK_EQ(hashes,4u);
}
SR_TEST(buffer_content_backoff_spaces_checks_of_an_unchanged_buffer_and_resets_on_change) {
  superman_returns::native::BufferContent content;
  uint64_t bytes=5;std::vector<uint64_t> checked;uint64_t now=0;
  auto hash=[&] {checked.push_back(now);return bytes;};
  for(now=10;now<=40;++now) content.Refresh(now,false,hash,false,2);
  // Checks at 10 (new), 11 (1 frame), 13 (2 frames), then every 4: 17, 21 ... 37.
  SR_CHECK_EQ(checked.size(),size_t(9));
  SR_CHECK_EQ(checked[0],uint64_t(10));SR_CHECK_EQ(checked[1],uint64_t(11));
  SR_CHECK_EQ(checked[2],uint64_t(13));SR_CHECK_EQ(checked[3],uint64_t(17));
  SR_CHECK_EQ(checked[8],uint64_t(37));
  // A page write is checked at once, and the next frame is checked again.
  bytes=6;checked.clear();now=38;
  SR_CHECK(content.Refresh(now,true,hash,false,2));SR_CHECK_EQ(checked.size(),size_t(1));
  now=39;SR_CHECK(!content.Refresh(now,false,hash,false,2));SR_CHECK_EQ(checked.size(),size_t(2));
  // An unnotified change (virtual alias) is found at the next scheduled check, never lost.
  bytes=7;bool found=false;
  for(now=40;now<60 && !found;++now) found=content.Refresh(now,false,hash,false,2);
  SR_CHECK(found);SR_CHECK(now-1<=39+4);
  // max_backoff_log2 0 is the old once-a-frame check.
  superman_returns::native::BufferContent every;checked.clear();
  for(now=1;now<=5;++now) every.Refresh(now,false,hash);
  SR_CHECK_EQ(checked.size(),size_t(5));
}
SR_TEST(guest_hash_in_place_matches_hashing_a_copy) {
  for (uint32_t size : {1u, 7u, 4096u, 100003u}) {
    std::vector<uint8_t> bytes(size);
    for (uint32_t i = 0; i < size; ++i) bytes[i] = uint8_t(i * 31 + 7);
    for (uint64_t seed : {uint64_t(0), uint64_t(0xcbf29ce484222325ull), ~uint64_t(0)}) {
      const std::vector<uint8_t> copy = bytes;
      uint64_t out = 0;
      SR_CHECK(HashGuestRange(bytes.data(), size, seed, Fnv, out));
      SR_CHECK_EQ(out, Fnv(copy.data(), copy.size(), seed));
    }
  }
}
SR_TEST(guest_hash_chains_the_seed_across_ranges) {
  std::vector<uint8_t> a(100, 1), b(50, 2);
  uint64_t h = 0xcbf29ce484222325ull;
  SR_CHECK(HashGuestRange(a.data(), 100, h, Fnv, h));
  SR_CHECK(HashGuestRange(b.data(), 50, h, Fnv, h));
  SR_CHECK_EQ(h, Fnv(b.data(), 50, Fnv(a.data(), 100, 0xcbf29ce484222325ull)));
}
SR_TEST(guest_hash_fails_on_inaccessible_partial_and_empty_ranges_without_touching_out) {
  auto* pages = static_cast<uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  SR_CHECK(pages != nullptr);
  if (!pages) return;
  DWORD previous = 0;
  SR_CHECK(VirtualProtect(pages + 4096, 4096, PAGE_NOACCESS, &previous));
  uint64_t out = 0x1234;
  SR_CHECK(HashGuestRange(pages, 4096, 0, Fnv, out));
  out = 0x1234;
  SR_CHECK(!HashGuestRange(pages + 4096, 1, 0, Fnv, out));
  SR_CHECK_EQ(out, uint64_t(0x1234));
  SR_CHECK(!HashGuestRange(pages, 4097, 0, Fnv, out));  // o final da faixa cai na página inacessível
  SR_CHECK_EQ(out, uint64_t(0x1234));
  SR_CHECK(!HashGuestRange(nullptr, 1, 0, Fnv, out));
  SR_CHECK(!HashGuestRange(pages, 0, 0, Fnv, out));
  SR_CHECK(VirtualFree(pages, 0, MEM_RELEASE));
}
#endif
