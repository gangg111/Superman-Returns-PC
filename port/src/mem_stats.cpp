// Guest physical memory: diagnostics and the pinned large-block fix (below).
//
// The game crashed after ~12 minutes with "PhysicalHeap::Alloc unable to alloc
// physical memory in parent heap": the runtime's 512 MB physical pool (shared
// by the A0000000/C0000000/E0000000 views) ran out. SR_LOG_MEM=<seconds>
// (1 = every 10 s) logs, from the swap hook:
//   - per physical view: committed allocations, bytes, and the sizes holding
//     the most bytes (a growing size class points at the leak);
//   - the parent pool: used bytes and the largest free run (fragmentation);
//   - live XPhysicalAlloc bytes per guest call stack (three return addresses).
//
// sub_82468008 is XPhysicalAlloc(size r3, physical_address r4, alignment r5,
// protect r6): it forwards to MmAllocatePhysicalMemoryEx and returns r3.
// sub_824680A8 is XPhysicalFree(address r3) (calls MmFreePhysicalMemory).

#include "mem_stats.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "generated/default/superman_returns_init.h"

namespace {

using rex::memory::BaseHeap;
using rex::memory::HeapAllocationInfo;

int LogIntervalSeconds() {
  static const int seconds = [] {
    const char* v = std::getenv("SR_LOG_MEM");
    if (!v || !*v || *v == '0') return 0;
    const int n = std::atoi(v);
    return n <= 1 ? 10 : n;
  }();
  return seconds;
}

struct CallStack {
  uint32_t lr[3]{};
  bool operator<(const CallStack& o) const {
    return std::lexicographical_compare(lr, lr + 3, o.lr, o.lr + 3);
  }
};

struct LiveAlloc {
  uint32_t size;
  CallStack stack;
};

std::mutex g_mutex;
std::unordered_map<uint32_t, LiveAlloc> g_live;  // guest address -> allocation
uint64_t g_alloc_calls = 0, g_free_calls = 0, g_failed = 0;

uint32_t LoadU32(uint8_t* base, uint32_t addr) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + addr));
}

bool IsStackAddress(uint32_t addr) {
  // Guest stacks live in the 0x70000000-0x7F000000 range (xthread.cpp).
  return addr >= 0x70000000u && addr < 0x7F000000u && (addr & 3) == 0;
}

// The caller's return address is in LR; older ones are saved 8 bytes below
// each frame's back chain (PPC ABI as emitted by the XDK compiler).
CallStack Capture(uint8_t* base, const PPCContext& ctx) {
  CallStack cs;
  cs.lr[0] = static_cast<uint32_t>(ctx.lr);
  uint32_t sp = ctx.r1.u32;
  for (int i = 1; i < 3; ++i) {
    if (!IsStackAddress(sp)) break;
    const uint32_t back_chain = LoadU32(base, sp);
    if (!IsStackAddress(back_chain) || back_chain <= sp) break;
    cs.lr[i] = LoadU32(base, back_chain - 8);
    sp = back_chain;
  }
  return cs;
}

struct HeapSummary {
  uint32_t allocations = 0;
  uint64_t bytes = 0;
  uint32_t largest_free = 0;
  std::map<uint32_t, std::pair<uint32_t, uint64_t>> by_size;  // size -> count, bytes
  std::string layout;  // address-ordered regions of at least 1 MB ("+" used, "-" free)
};

HeapSummary Walk(BaseHeap* heap, bool with_layout = false) {
  HeapSummary s;
  const uint64_t end = uint64_t(heap->heap_base()) + heap->heap_size();
  uint64_t addr = heap->heap_base();
  while (addr < end) {
    HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(static_cast<uint32_t>(addr), &info) || !info.region_size) {
      addr += heap->page_size();
      continue;
    }
    if (with_layout && info.region_size >= 0x100000) {
      s.layout += fmt::format(" {}{:08X}:{:.1f}M", info.state ? '+' : '-', info.base_address,
                              info.region_size / 1048576.0);
    }
    if (info.state == 0) {
      s.largest_free = std::max(s.largest_free, info.region_size);
    } else if (info.allocation_base == info.base_address) {
      ++s.allocations;
      s.bytes += info.allocation_size;
      auto& e = s.by_size[info.allocation_size];
      ++e.first;
      e.second += info.allocation_size;
    }
    addr += info.region_size;
  }
  return s;
}

std::string TopSizes(const HeapSummary& s, size_t n) {
  std::vector<std::pair<uint32_t, std::pair<uint32_t, uint64_t>>> v(s.by_size.begin(),
                                                                       s.by_size.end());
  std::sort(v.begin(), v.end(),
            [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
  std::string out;
  for (size_t i = 0; i < v.size() && i < n; ++i) {
    out += fmt::format(" {:#x}x{}={:.1f}MB", v[i].first, v[i].second.first,
                       v[i].second.second / 1048576.0);
  }
  return out;
}

void LogSnapshot(bool with_layout = false) {
  auto* memory = REX_KERNEL_MEMORY();
  BaseHeap* parent = memory->GetPhysicalHeap();
  const HeapSummary p = Walk(parent, with_layout);
  REXLOG_INFO("mem: physical pool used {:.1f}/{:.1f} MB, largest free run {:.1f} MB",
              p.bytes / 1048576.0, parent->heap_size() / 1048576.0, p.largest_free / 1048576.0);
  if (with_layout) REXLOG_INFO("mem: physical layout:{}", p.layout);
  struct View {
    const char* name;
    uint32_t page;
  };
  for (const View v : {View{"A0 64K", 0x10000}, View{"C0 16M", 0x1000000}, View{"E0 4K", 0x1000}}) {
    BaseHeap* heap = memory->LookupHeapByType(true, v.page);
    if (!heap) continue;
    const HeapSummary s = Walk(heap);
    REXLOG_INFO("mem: view {} {} allocs {:.1f} MB; top sizes:{}", v.name, s.allocations,
                s.bytes / 1048576.0, TopSizes(s, 6));
  }

  std::map<CallStack, std::pair<uint32_t, uint64_t>> by_stack;
  uint64_t live_bytes = 0;
  {
    std::lock_guard lock(g_mutex);
    for (const auto& [addr, a] : g_live) {
      auto& e = by_stack[a.stack];
      ++e.first;
      e.second += a.size;
      live_bytes += a.size;
    }
    REXLOG_INFO("mem: XPhysicalAlloc live {} blocks {:.1f} MB (calls alloc {} free {} failed {})",
                g_live.size(), live_bytes / 1048576.0, g_alloc_calls, g_free_calls, g_failed);
  }
  std::vector<std::pair<CallStack, std::pair<uint32_t, uint64_t>>> v(by_stack.begin(),
                                                                       by_stack.end());
  std::sort(v.begin(), v.end(),
            [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
  for (size_t i = 0; i < v.size() && i < 8; ++i) {
    REXLOG_INFO("mem:   {:08X}<{:08X}<{:08X} {} blocks {:.1f} MB", v[i].first.lr[0],
                v[i].first.lr[1], v[i].first.lr[2], v[i].second.first,
                v[i].second.second / 1048576.0);
  }
}

}  // namespace

namespace superman_returns {

void MaybeLogMemory(uint8_t* base) {
  (void)base;
  const int interval = LogIntervalSeconds();
  if (!interval) return;
  using clock = std::chrono::steady_clock;
  static clock::time_point last = clock::now();
  const auto now = clock::now();
  if (now - last < std::chrono::seconds(interval)) return;
  last = now;
  LogSnapshot(true);
}

}  // namespace superman_returns

namespace {

// Pinned large blocks.
//
// The runtime forces every physical allocation top-down. When the game frees
// its 224 MB level pool (XPhysicalAlloc(0xDFF0000)) between areas, the next
// small allocations land at the top of that hole and split it, so asking for
// the pool again fails although 380 MB are free (measured: largest free run
// 181 MB right after the free). The game then reads the level into address 0
// and crashes. On these large requests we keep the block reserved on free and
// hand the same pages back on the next request of the same or smaller size.
// The runtime does not zero reused physical pages either, so the game sees the
// same contents it would get from a fresh allocation here.
// SR_PIN_LARGE_PHYSICAL=0 turns this off.
constexpr uint32_t kPinThreshold = 64u << 20;

// At boot the game probes free memory: it allocates the largest block it can
// (0x12B00000 measured) and frees it at once. Keeping that one reserved starves
// the rest of the boot, so a block freed sooner than this is really released.
constexpr auto kPinMinLifetime = std::chrono::seconds(2);

struct PinnedBlock {
  uint32_t address;
  uint32_t size;
  uint32_t alignment;
  uint32_t protect;
  bool in_use;
  std::chrono::steady_clock::time_point allocated;
};
std::vector<PinnedBlock> g_pinned;  // guarded by g_mutex

bool PinEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("SR_PIN_LARGE_PHYSICAL");
    return !(v && *v == '0');
  }();
  return enabled;
}

void CallXPhysicalFree(PPCContext& ctx, uint8_t* base, uint32_t address) {
  const auto saved = ctx;
  ctx.r3.u64 = address;
  __imp__sub_824680A8(ctx, base);
  ctx = saved;
}

}  // namespace

REX_HOOK_RAW(sub_82468008) {
  const bool log = LogIntervalSeconds() != 0;
  const uint32_t size = ctx.r3.u32, physical = ctx.r4.u32, alignment = ctx.r5.u32,
                 protect = ctx.r6.u32;
  const bool pin = PinEnabled() && size >= kPinThreshold && physical == 0xFFFFFFFFu;
  const CallStack stack = log ? Capture(base, ctx) : CallStack{};

  uint32_t addr = 0;
  if (pin) {
    std::lock_guard lock(g_mutex);
    for (auto& b : g_pinned) {
      if (!b.in_use && b.size >= size && b.protect == protect &&
          (!alignment || (b.address % alignment) == 0)) {
        b.in_use = true;
        b.allocated = std::chrono::steady_clock::now();
        addr = b.address;
        REXLOG_INFO("mem: reusing pinned physical block {:08X} ({:#x}) for {:#x}", addr, b.size,
                    size);
        break;
      }
    }
  }
  if (!addr) {
    __imp__sub_82468008(ctx, base);
    addr = ctx.r3.u32;
    if (!addr && pin) {
      // No reusable block fits: give the idle pinned blocks back and retry.
      std::vector<uint32_t> idle;
      {
        std::lock_guard lock(g_mutex);
        for (auto it = g_pinned.begin(); it != g_pinned.end();) {
          if (!it->in_use) {
            idle.push_back(it->address);
            it = g_pinned.erase(it);
          } else {
            ++it;
          }
        }
      }
      if (!idle.empty()) {
        for (uint32_t a : idle) CallXPhysicalFree(ctx, base, a);
        ctx.r3.u64 = size;
        ctx.r4.u64 = physical;
        ctx.r5.u64 = alignment;
        ctx.r6.u64 = protect;
        __imp__sub_82468008(ctx, base);
        addr = ctx.r3.u32;
      }
    }
    if (addr && pin) {
      std::lock_guard lock(g_mutex);
      g_pinned.push_back(
          PinnedBlock{addr, size, alignment, protect, true, std::chrono::steady_clock::now()});
    }
  }
  ctx.r3.u64 = addr;

  {
    std::lock_guard lock(g_mutex);
    ++g_alloc_calls;
    if (addr) {
      if (log) g_live[addr] = LiveAlloc{size, stack};
      return;
    }
    ++g_failed;
  }
  REXLOG_WARN("mem: XPhysicalAlloc({:#x}) failed from {:08X}<{:08X}<{:08X}", size, stack.lr[0],
              stack.lr[1], stack.lr[2]);
  LogSnapshot(true);
}

REX_HOOK_RAW(sub_824680A8) {
  const uint32_t address = ctx.r3.u32;
  {
    std::lock_guard lock(g_mutex);
    ++g_free_calls;
    g_live.erase(address);
    for (auto it = g_pinned.begin(); it != g_pinned.end(); ++it) {
      if (it->address != address || !it->in_use) continue;
      if (std::chrono::steady_clock::now() - it->allocated < kPinMinLifetime) {
        g_pinned.erase(it);  // a probe: release it for real
        break;
      }
      it->in_use = false;
      REXLOG_INFO("mem: keeping pinned physical block {:08X} ({:#x}) reserved", address,
                  it->size);
      return;
    }
  }
  __imp__sub_824680A8(ctx, base);
}
