#include "range_allocator.h"
#include "test_main.h"

using namespace superman_returns::native;

// Expected offsets worked out by hand: 256-byte alignment, first fit from offset 0.
SR_TEST(range_allocator_rounds_to_alignment_and_fills_from_the_start) {
  RangeAllocator a(4096);
  SR_CHECK_EQ(a.Allocate(1), 0ull);     // 1 -> 256
  SR_CHECK_EQ(a.Allocate(300), 256ull); // 300 -> 512
  SR_CHECK_EQ(a.Allocate(256), 768ull);
  SR_CHECK_EQ(a.used(), 1024ull);
  SR_CHECK_EQ(a.Allocate(0), RangeAllocator::kFailed);
}

SR_TEST(range_allocator_fails_when_no_free_range_is_large_enough) {
  RangeAllocator a(1024);
  SR_CHECK_EQ(a.Allocate(512), 0ull);
  SR_CHECK_EQ(a.Allocate(512), 512ull);
  SR_CHECK_EQ(a.Allocate(1), RangeAllocator::kFailed);
}

SR_TEST(range_allocator_merges_released_neighbours) {
  RangeAllocator a(1024);
  const uint64_t x = a.Allocate(256), y = a.Allocate(256), z = a.Allocate(256);
  SR_CHECK_EQ(x, 0ull);
  SR_CHECK_EQ(y, 256ull);
  SR_CHECK_EQ(z, 512ull);
  a.Release(x, 256);
  a.Release(z, 256);  // free: [0,256) [512,1024) -> 2 ranges
  SR_CHECK_EQ(a.free_ranges(), size_t(2));
  a.Release(y, 256);  // all merged back into one range of 1024
  SR_CHECK_EQ(a.free_ranges(), size_t(1));
  SR_CHECK_EQ(a.used(), 0ull);
  SR_CHECK_EQ(a.Allocate(1024), 0ull);
}

SR_TEST(range_allocator_reuses_a_released_hole_first_fit) {
  RangeAllocator a(2048);
  a.Allocate(512);                       // [0,512)
  const uint64_t hole = a.Allocate(512); // [512,1024)
  a.Allocate(512);                       // [1024,1536)
  a.Release(hole, 512);
  SR_CHECK_EQ(a.Allocate(300), 512ull);  // fits in the hole (rounded to 512)
  SR_CHECK_EQ(a.Allocate(512), 1536ull); // hole used up: the tail
}
