// The world streamer, native (2026-11-07).
//
// The game keeps the world's sectors (600 x 600 cells, built by sub_8278B6E0) loaded around the
// camera with one object of 1268 bytes (vtable 0x82099E54, built by sub_8278B610 in sub_821A3390).
// Its methods:
//   [0] vt_8278B1B8  GetLoads(r4 = &count): appends the "to load" list to the loaded list, returns it
//   [1] sub_8278B210 GetUnloads(r4 = &count): drops the "to unload" list from the loaded list, returns it
//   [2] sub_8278AC40 Update(r4 = clear): the units of every cell within PVSFixed of the camera's cell
//   [5] sub_8278B690 destructor, [7] sub_8278B320 Init(fixed, sliding), [8] sub_8278AAE0 AtLoadLimit
// All three lists live inside the object with room for 100 units (+52 count +452, +456 count +856,
// +860 count +1260) and Update gathers the candidates on its stack (room for 120). The game's
// window of 1 (9 cells) fits; 5 (121 cells) overran them and the game crashed.
//
// Here the lists are host vectors without a size limit. The returned arrays are guest blocks that
// grow with them, and the object's own lists are mirrored as far as they fit (100), so the
// object reads the same as the original's whenever the original would fit. Only PVSSliding 0 is
// rewritten (the game's): with sliding the original runs.
//
// sr_stream_check runs the original beside the native code on the same object and compares the
// lists after every call (the game then gets the original's): the proof the rewrite matches.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <thread>
#include <tuple>
#include <vector>

#include "generated/default/superman_returns_init.h"

REXCVAR_DEFINE_BOOL(sr_stream_native, false, "Superman Returns",
                    "World streamer rewritten natively, its lists without the console's 100-unit room "
                    "(needed for sr_stream_fixed above 4)");
REXCVAR_DEFINE_BOOL(sr_stream_check, false, "Superman Returns",
                    "Debug: run the game's streamer beside the native one and log any difference (the game "
                    "gets the original's lists; only for sr_stream_fixed 4 and less)");

namespace superman_returns::native {
void ArmGuestWatch(uint8_t* base, const uint32_t* guest, int n);
}

REXCVAR_DECLARE(int32_t, sr_stream_fixed);
REXCVAR_DEFINE_BOOL(sr_stream_log, false, "Superman Returns",
                    "Debug: log the streamer's window and the world loaders' queues (a loading screen waits "
                    "for them to empty)");
REXCVAR_DEFINE_INT32(sr_stream_pending, 10, "Superman Returns",
                     "The streamer asks for no new world units while the loader has this many queued (the game "
                     "waits for its queues to empty)")
    .range(1, 2000);
REXCVAR_DEFINE_INT32(sr_slot_pool_scale, 1, "Superman Returns",
                     "Times the console's slot pools built by sub_8214DD20 (8192 slots in 2 MB, and 1024 in "
                     "30 KB); a wider sr_stream_fixed ran the first out")
    .range(1, 16);

namespace {

constexpr uint32_t kVtable = 0x82099E54u;
constexpr uint32_t kRoom = 100;  // each inline list

struct Streamer {
  std::vector<uint32_t> loads, unloads, loaded;
  uint32_t loads_block = 0, loads_cap = 0;
  uint32_t unloads_block = 0, unloads_cap = 0;
  size_t peak_cand = 0, peak_loaded = 0;
  int32_t radius = -2;
  uint64_t created_ms = 0;  // first Update
};

struct Mem {
  uint8_t* base;
  uint32_t u32(uint32_t a) const { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); }
  uint8_t u8(uint32_t a) const { return base[a]; }
  void put(uint32_t a, uint32_t v) const { *reinterpret_cast<uint32_t*>(base + a) = __builtin_bswap32(v); }
  float f32(uint32_t a) const {
    const uint32_t u = u32(a);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  }
};

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// A loading screen polls sub_82199BF0 (any world loader busy) until every unit the streamer asked
// for is in (2026-11-08: with a window above the game's 1, 71 units stayed queued in state 1, never
// ready, and the boot loading screen never ended). While it polls, the streamer keeps the game's
// window; the wide one comes a second after the polling stops.
std::atomic<uint64_t> g_last_poll_ms{0};
bool Loading() { return NowMs() - g_last_poll_ms.load() < 1000; }

// Units in the queues (states 0..7) of the loader paired with this streamer: the manager
// ([0x8296DC18]) keeps streamers at [+400][i] and loaders at [+60][i], count at +52.
// sub_82788840 gives a state's list, sub_82788A90 its count (as sub_82199328 sums them).
std::string g_last_states;
std::atomic<uint64_t> g_res_state[3];  // sub_826E4D80's results (sr_stream_log)
// The reader sub_821A5398 (sr_stream_log): calls, its queues on the last call (requests [+48],
// resources in flight [+80], final check [+84] as deques, ready [+88] as a vector) and the
// final check's results (sub_821995C0: 0 = not yet, 1 = done).
std::atomic<uint64_t> g_reader_calls, g_final_check[2];
std::atomic<uint64_t> g_links_dropped{0};  // sub_821E6AB0 hook
std::atomic<int32_t> g_reader_q[4];
int32_t DequeCount(const Mem& m, uint32_t q) {  // as sub_8278A128
  if (!q) return -1;
  return int32_t((((m.u32(q + 36) - m.u32(q + 20)) >> 2) << 6) + ((m.u32(q + 24) - m.u32(q + 28)) >> 2) +
                 ((m.u32(q + 16) - m.u32(q + 8)) >> 2)) - 64;
}
int32_t Pending(PPCContext& ctx, uint8_t* base, uint32_t obj) {
  const Mem m{base};
  const uint32_t mgr = m.u32(0x8296DC18u);
  if (!mgr) return 0;
  const uint32_t n = m.u32(mgr + 52), streamers = m.u32(mgr + 400), loaders = m.u32(mgr + 60);
  if (!streamers || !loaders || n > 64) return 0;
  uint32_t loader = 0;
  for (uint32_t i = 0; i < n; ++i)
    if (m.u32(streamers + 4 * i) == obj) loader = m.u32(loaders + 4 * i);
  if (!loader) return 0;
  int32_t sum = 0;
  g_last_states.clear();
  for (uint32_t st = 0; st < 8; ++st) {
    ctx.r3.u64 = loader;
    ctx.r4.u64 = st;
    sub_82788840(ctx, base);
    ctx.r4.u64 = ctx.r3.u64;
    ctx.r3.u64 = loader;
    sub_82788A90(ctx, base);
    sum += int32_t(ctx.r3.u32);
    g_last_states += fmt::format(" {}", int32_t(ctx.r3.u32));
  }
  return sum;
}

std::mutex g_mutex;
std::unordered_map<uint32_t, Streamer> g_streamers;

// The native path applies to the game's streamer with PVSSliding 0.
bool Native(const Mem& m, uint32_t obj) {
  return REXCVAR_GET(sr_stream_native) && obj && m.u32(obj) == kVtable && int32_t(m.u32(obj + 24)) <= 0;
}

// A guest array for a returned list (grows, the old block leaks: it happens a handful of times).
uint32_t Publish(const Mem& m, uint32_t& block, uint32_t& cap, const std::vector<uint32_t>& v) {
  if (v.size() > cap) {
    const uint32_t want = std::max<uint32_t>(uint32_t(v.size()) * 2, 1024);
    const uint32_t b = REX_KERNEL_MEMORY()->SystemHeapAlloc(want * 4, 128);
    if (!b) return 0;
    block = b;
    cap = want;
  }
  for (size_t i = 0; i < v.size(); ++i) m.put(block + uint32_t(4 * i), v[i]);
  return block;
}

// The object's inline lists, as far as they fit.
void Mirror(const Mem& m, uint32_t obj, uint32_t count_at, uint32_t first, const std::vector<uint32_t>& v) {
  const uint32_t n = uint32_t(std::min<size_t>(v.size(), kRoom));
  for (uint32_t i = 0; i < n; ++i) m.put(first + 4 * i, v[i]);
  m.put(obj + count_at, n);
}

std::vector<uint32_t> Inline(const Mem& m, uint32_t obj, uint32_t count_at, uint32_t first) {
  std::vector<uint32_t> v;
  const uint32_t n = std::min(m.u32(obj + count_at), 4096u);
  for (uint32_t i = 0; i < n; ++i) v.push_back(m.u32(first + 4 * i));
  return v;
}

void Compare(const char* what, uint32_t obj, const std::vector<uint32_t>& native,
             const std::vector<uint32_t>& game) {
  static uint64_t calls = 0, bad = 0;
  ++calls;
  if (native != game) {
    if (bad++ < 50) {
      std::string a, b;
      for (uint32_t u : native) a += fmt::format(" {:X}", u);
      for (uint32_t u : game) b += fmt::format(" {:X}", u);
      REXLOG_WARN("streamer check {:08X} {}: native [{}] game [{}]", obj, what, a, b);
    }
  }
  if (calls % 2000 == 0) REXLOG_INFO("streamer check: {} comparisons, {} differences", calls, bad);
}

// Update, the game's sub_8278AC40 for PVSSliding 0, into s.loads / s.unloads.
void Update(PPCContext& ctx, uint8_t* base, uint32_t obj, bool clear, Streamer& s) {
  const Mem m{base};
  const uint32_t grid = m.u32(obj + 12), pos = m.u32(obj + 4);
  const int32_t width = int32_t(m.u32(grid + 28)), rows = int32_t(m.u32(grid + 32));

  // The camera's cell: the game's sub_8278BAD0 (its float rounding kept), on a scratch frame.
  const uint32_t sp = ctx.r1.u32;
  const uint32_t frame = (sp - 256) & ~15u;
  ctx.r1.u64 = frame;
  m.put(frame + 80, 0xFFFFFFFFu);
  m.put(frame + 84, 0xFFFFFFFFu);
  ctx.r3.u64 = grid;
  ctx.f1.f64 = double(m.f32(pos + 0));
  ctx.f2.f64 = double(m.f32(pos + 8));
  ctx.r6.u64 = frame + 80;
  ctx.r7.u64 = frame + 84;
  sub_8278BAD0(ctx, base);
  int32_t row = int32_t(m.u32(frame + 80)), col = int32_t(m.u32(frame + 84));
  ctx.r1.u64 = sp;

  auto cell = [&](int32_t r, int32_t c) {  // sub_8278BC60
    if (c >= width || r >= rows || c < 0 || r < 0) return -1;
    return width * r + c;
  };
  if (cell(row, col) == -1) {  // sub_8278BDD0: clamped into the grid
    if (col >= width) col = width - 1;
    if (col < 0) col = 0;
    if (row >= rows) row = rows - 1;
    if (row < 0) row = 0;
  }

  // Every cell of the window, rows and columns in the order of the offsets (0, 1, -1, 2, -2...):
  // the object's own (the game's PVSFixed, from Init) while a loading screen waits, or with
  // sr_stream_check; otherwise sr_stream_fixed's.
  std::vector<int32_t> off;
  const int32_t want = REXCVAR_GET(sr_stream_fixed);
  // Also for the first 5 s of a new streamer: it updates before the loading screen first polls,
  // and a wide first update queued 25 units the loading screen then cut back to 9 while their
  // entities were loading; a near unit's AI line reference into one of them then crashed
  // sub_82288E20 (2026-11-08).
  if (!s.created_ms) s.created_ms = NowMs();
  const bool young = NowMs() - s.created_ms < 5000;
  const int32_t radius = young || Loading() || REXCVAR_GET(sr_stream_check) || want <= 1 ? -1 : want;
  if (radius < 0) {
    const uint32_t offsets = m.u32(obj + 16);
    for (int32_t i = 0, n = int32_t(m.u32(obj + 20)); i < n; ++i) off.push_back(int32_t(m.u32(offsets + 4 * i)));
  } else {
    off.push_back(0);
    for (int32_t k = 1; k <= radius; ++k) {
      off.push_back(k);
      off.push_back(-k);
    }
  }
  if (radius != s.radius) {
    REXLOG_INFO("streamer {:08X}: window {} ({} loaded)", obj,
                radius < 0 ? std::string("of the game (loading)") : std::to_string(radius), s.loaded.size());
    s.radius = radius;
  }
  const int32_t n_off = int32_t(off.size());
  const uint32_t table = m.u32(grid + 36);
  const int32_t slots = int32_t(m.u32(grid + 40));
  // The game's own window (its radius from Init): those units always load, as in the original.
  const int32_t game_radius = (int32_t(m.u32(obj + 20)) - 1) / 2;
  std::vector<uint32_t> cand, near_units;
  std::vector<std::pair<int32_t, uint32_t>> ranked;  // (ring * 100 + steps, unit)
  for (int32_t i = 0; i < n_off; ++i) {
    const int32_t r = off[i] + row;
    for (int32_t j = 0; j < n_off; ++j) {
      const int32_t c = cell(r, off[j] + col);
      if (c == -1) continue;
      const bool is_near = std::abs(off[i]) <= game_radius && std::abs(off[j]) <= game_radius;
      const int32_t rank = std::max(std::abs(off[i]), std::abs(off[j])) * 100 + std::abs(off[i]) + std::abs(off[j]);
      const uint32_t first = table + uint32_t(c * slots) * 4;  // sub_8278BA30
      for (int32_t k = 0; k < slots; ++k) {
        const uint32_t u = m.u32(first + 4 * k);
        if (u == 0xFFFFFFFFu) break;
        cand.push_back(u);
        ranked.push_back({rank, u});
        if (is_near) near_units.push_back(u);
      }
    }
  }
  // A wide window never wants more units than the loader has slots past state 2 ([mgr+68], the
  // LOADUNITn filters): the ones past it stayed half-loaded for good, their cells registered with
  // no data, and every AI line into them came back empty (2026-11-08: 25 units, 22 slots, over
  // 100 000 dropped links a few seconds, then sub_822833B0 crashed). The nearest go first.
  if (radius >= 0) {
    const uint32_t mgr = m.u32(0x8296DC18u);
    const size_t room = mgr ? size_t(m.u32(mgr + 68)) : 0;
    if (room && cand.size() > room) {
      std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
      cand.clear();
      for (size_t i = 0; i < room; ++i) cand.push_back(ranked[i].second);
    }
  }
  if (clear) cand.clear();
  s.peak_cand = std::max(s.peak_cand, cand.size());

  auto has = [](const std::vector<uint32_t>& v, uint32_t u) { return std::find(v.begin(), v.end(), u) != v.end(); };
  // Nothing new beyond the game's window while the loader still has sr_stream_pending units in its
  // queues: the game waits for them to empty (loading screens, the sky world when flying up), and
  // a wide window queued over a thousand at once (2026-11-08: Superman vanished and the camera
  // froze). The game's window always goes first and is never held back (held back, the city
  // around Superman stopped loading behind far units stuck in the queue).
  bool throttle = false;
  int32_t pending = -1;
  if (!REXCVAR_GET(sr_stream_check) && near_units.size() != cand.size()) {
    pending = Pending(ctx, base, obj);
    throttle = pending >= REXCVAR_GET(sr_stream_pending);
  }
  if (REXCVAR_GET(sr_stream_log)) {
    static uint64_t last = 0;
    if (NowMs() - last > 3000) {
      last = NowMs();
      const uint32_t pool = m.u32(0x829A93D4u);  // sub_82787D60
      if (pool) {
        ctx.r3.u64 = pool;
        sub_82787D70(ctx, base);
        REXLOG_INFO("streamer: memory unit pool {:08X} free {}, resource states seen 0:{} 1:{} 2:{}", pool,
                    int32_t(ctx.r3.u32), g_res_state[0].exchange(0), g_res_state[1].exchange(0),
                    g_res_state[2].exchange(0));
      }
      REXLOG_INFO("streamer: reader calls {}, queues requests {} resources {} final {} ready {}, final check 0:{} 1:{}, "
                  "AI links into loading units dropped {}",
                  g_reader_calls.exchange(0), g_reader_q[0].load(), g_reader_q[1].load(), g_reader_q[2].load(),
                  g_reader_q[3].load(), g_final_check[0].exchange(0), g_final_check[1].exchange(0),
                  g_links_dropped.load());
      REXLOG_INFO("streamer {:08X}: cell ({}, {}), {} candidates ({} near), {} loaded, loader queue {}{} [states{}]", obj,
                  row, col, cand.size(), near_units.size(), s.loaded.size(), pending, throttle ? " (held)" : "",
                  g_last_states);
    }
  }
  s.loads.clear();
  for (uint32_t u : near_units)
    if (!has(s.loaded, u) && !has(s.loads, u)) s.loads.push_back(u);
  if (!throttle)
    for (uint32_t u : cand)
      if (!has(s.loaded, u) && !has(s.loads, u)) s.loads.push_back(u);
  // AtLoadLimit (sub_8278AAE0): no more loads at once than the object's +8.
  const int32_t limit = int32_t(m.u32(obj + 8));
  if (int32_t(s.loads.size()) > limit) s.loads.resize(size_t(std::max(limit, 0)));
  s.unloads.clear();
  for (uint32_t u : s.loaded)
    if (!has(cand, u)) s.unloads.push_back(u);
  // The game never got more than 100 unloads at once (its loaded list held 100). Leaving a level
  // (clear) with the whole city loaded handed it 1326 in one call and the loading screen never
  // ended: the rest go in the next updates, 100 at a time, as the original's room allowed.
  if (s.unloads.size() > kRoom && !REXCVAR_GET(sr_stream_check)) s.unloads.resize(kRoom);
  if (clear && !s.loaded.empty())
    REXLOG_INFO("streamer {:08X}: clear, {} loaded, {} unloads now", obj, s.loaded.size(), s.unloads.size());
}

void DropUnloads(Streamer& s) {  // sub_8278B210: each unload's first match in the loaded list goes
  for (uint32_t u : s.unloads) {
    auto it = std::find(s.loaded.begin(), s.loaded.end(), u);
    if (it != s.loaded.end()) *it = 0xFFFFFFFFu;
  }
  s.loaded.erase(std::remove(s.loaded.begin(), s.loaded.end(), 0xFFFFFFFFu), s.loaded.end());
}

}  // namespace

REX_HOOK_RAW(sub_8278AC40) {
  const Mem m{base};
  const uint32_t obj = ctx.r3.u32;
  const bool clear = (ctx.r4.u32 & 0xFF) != 0;
  if (!Native(m, obj)) {
    __imp__sub_8278AC40(ctx, base);
    return;
  }
  std::lock_guard lock(g_mutex);
  auto [it, fresh] = g_streamers.try_emplace(obj);
  Streamer& s = it->second;
  if (fresh) {
    s.loaded = Inline(m, obj, 1260, obj + 860);
    REXLOG_INFO("streamer {:08X}: native, window {} cells, at most {} loads at once", obj,
                int32_t(m.u32(obj + 20)) * int32_t(m.u32(obj + 20)), int32_t(m.u32(obj + 8)));
  }
  const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64;
  Update(ctx, base, obj, clear, s);
  if (REXCVAR_GET(sr_stream_check)) {
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    __imp__sub_8278AC40(ctx, base);
    Compare("loads", obj, s.loads, Inline(m, obj, 452, obj + 52));
    Compare("unloads", obj, s.unloads, Inline(m, obj, 856, obj + 456));
    return;
  }
  Mirror(m, obj, 452, obj + 52, s.loads);
  Mirror(m, obj, 856, obj + 456, s.unloads);
  if (s.peak_cand > 120 || s.loaded.size() > s.peak_loaded) {
    if (s.loaded.size() > s.peak_loaded && s.loaded.size() % 25 == 0)
      REXLOG_INFO("streamer {:08X}: {} units loaded, {} candidates at most", obj, s.loaded.size(), s.peak_cand);
    s.peak_loaded = std::max(s.peak_loaded, s.loaded.size());
  }
}

REX_HOOK_RAW(vt_8278B1B8) {
  const Mem m{base};
  const uint32_t obj = ctx.r3.u32, out = ctx.r4.u32;
  std::unique_lock lock(g_mutex);
  auto it = Native(m, obj) ? g_streamers.find(obj) : g_streamers.end();
  if (it == g_streamers.end()) {
    lock.unlock();
    __imp__vt_8278B1B8(ctx, base);
    return;
  }
  Streamer& s = it->second;
  s.loaded.insert(s.loaded.end(), s.loads.begin(), s.loads.end());
  if (REXCVAR_GET(sr_stream_check)) {
    __imp__vt_8278B1B8(ctx, base);
    Compare("loaded after loads", obj, s.loaded, Inline(m, obj, 1260, obj + 860));
    return;
  }
  Mirror(m, obj, 1260, obj + 860, s.loaded);
  m.put(out, uint32_t(s.loads.size()));
  ctx.r3.u64 = Publish(m, s.loads_block, s.loads_cap, s.loads);
}

REX_HOOK_RAW(sub_8278B210) {
  const Mem m{base};
  const uint32_t obj = ctx.r3.u32, out = ctx.r4.u32;
  std::unique_lock lock(g_mutex);
  auto it = Native(m, obj) ? g_streamers.find(obj) : g_streamers.end();
  if (it == g_streamers.end()) {
    lock.unlock();
    __imp__sub_8278B210(ctx, base);
    return;
  }
  Streamer& s = it->second;
  DropUnloads(s);
  if (REXCVAR_GET(sr_stream_check)) {
    __imp__sub_8278B210(ctx, base);
    Compare("loaded after unloads", obj, s.loaded, Inline(m, obj, 1260, obj + 860));
    return;
  }
  Mirror(m, obj, 1260, obj + 860, s.loaded);
  m.put(out, uint32_t(s.unloads.size()));
  ctx.r3.u64 = Publish(m, s.unloads_block, s.unloads_cap, s.unloads);
}


// A world loader busy (sub_82199328, used by sub_82199BF0 = any loader busy): the sum of its
// counts in states 0..7 (sub_82788840 the state's list, sub_82788A90 its count) above 0.
REX_HOOK_RAW(sub_82199328) {
  if (!REXCVAR_GET(sr_stream_log)) {
    __imp__sub_82199328(ctx, base);
    return;
  }
  const uint32_t loader = ctx.r3.u32;
  uint32_t n[8];
  for (uint32_t s = 0; s < 8; ++s) {
    ctx.r3.u64 = loader;
    ctx.r4.u64 = s;
    sub_82788840(ctx, base);
    ctx.r4.u64 = ctx.r3.u64;
    ctx.r3.u64 = loader;
    sub_82788A90(ctx, base);
    n[s] = ctx.r3.u32;
  }
  {
    // Who marks units ready: watch the end of the ready vector ([alloc+88] -> {begin, end}) and of
    // the pool's deque ([alloc+48] +24), once.
    static bool armed = false;
    const Mem m{base};
    const uint32_t alloc = m.u32(loader + 4);
    if (false && !armed && alloc && m.u32(alloc + 88) && m.u32(alloc + 48)) {  // done (2026-11-08), slots reused
      armed = true;
      const uint32_t at[2] = {m.u32(alloc + 88) + 4, m.u32(alloc + 48) + 24};
      REXLOG_INFO("stream loader: watching ready vector end {:08X} and pool end {:08X}", at[0], at[1]);
      superman_returns::native::ArmGuestWatch(base, at, 2);
    }
  }
  static std::unordered_map<uint32_t, uint64_t> last;
  const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch()).count());
  if (now - last[loader] > 2000) {
    last[loader] = now;
    const Mem m{base};
    const uint32_t alloc = m.u32(loader + 4), vt = alloc ? m.u32(alloc) : 0;
    std::string dump;
    for (uint32_t k = 0; alloc && k < 96; k += 4) dump += fmt::format(" {:08X}", m.u32(alloc + k));
    // The memory-unit allocator the reader asks for a buffer ([alloc+76], vt[0]; 0 = none free).
    const uint32_t units = alloc ? m.u32(alloc + 76) : 0;
    dump += fmt::format(" || units {:08X}", units);
    if (units) {
      dump += fmt::format(" vt0 {:08X}:", m.u32(m.u32(units)));
      for (uint32_t k = 0; k < 64; k += 4) dump += fmt::format(" {:08X}", m.u32(units + k));
    }
    REXLOG_INFO("stream loader {:08X}: states {} {} {} {} {} {} {} {} | alloc {:08X} vt {:08X} [+12] {:08X} [+20] {:08X} |{}",
                loader, n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], alloc, vt, vt ? m.u32(vt + 12) : 0,
                vt ? m.u32(vt + 20) : 0, dump);
  }
  ctx.r3.u64 = loader;
  __imp__sub_82199328(ctx, base);
}

// The world's memory units (sub_8219DD00, the streaming manager in r3): a pool of [+36] blocks of
// [+40] bytes (10 x 458752 on the console) from which every streamed unit gets its buffer
// (sub_821A0530 -> sub_82787D70 free count). With 10, a unit past the tenth waits in the loader's
// state 1 forever: that is what kept every window above the game's from loading (2026-11-08).
// sr_stream_memory_units raises the count (0 = enough for sr_stream_fixed's window).
REXCVAR_DEFINE_INT32(sr_stream_memory_units, 0, "Superman Returns",
                     "World memory units (the console's 10; 0 = as many as the sr_stream_fixed window needs)")
    .range(0, 4096);
REX_HOOK_RAW(sub_8219DD00) {
  const Mem m{base};
  const uint32_t mgr = ctx.r3.u32;
  const uint32_t count = m.u32(mgr + 36), size = m.u32(mgr + 40);
  int32_t want = REXCVAR_GET(sr_stream_memory_units);
  if (want == 0) {  // only a wider window than the game's needs more
    const int32_t r = REXCVAR_GET(sr_stream_fixed);
    want = r > 1 ? (2 * r + 1) * (2 * r + 1) + 10 : 0;
  }
  // The second pool, built right after by sub_821A07A0([+44] slots of [+48] bytes from the heap
  // [0x8296D890], sub_82199650): before a unit is handed to the game its object takes a free slot
  // there (sub_821A0350 -> sub_82199738); with no slot free it answers "not yet" forever, which
  // held 67 units of a window of 4 after the first pool was raised (2026-11-08).
  // The third: each loader's table of units past state 2 (sub_821A4930 needs a free one), [+68]
  // slots given to the loader built later in the same sub_821A3390 (sub_821A2D10 r7 -> loader +56);
  // 10 on the console, all taken by a window of 4 (2026-11-08).
  const uint32_t count2 = m.u32(mgr + 44), size2 = m.u32(mgr + 48), count3 = m.u32(mgr + 68);
  for (uint32_t i = 0, names = m.u32(mgr + 72); names && i < count3 && i < 64; ++i)
    if (const uint32_t p = m.u32(names + 4 * i))
      REXLOG_INFO("streaming: loader slot filter {} = '{}'", i, reinterpret_cast<const char*>(base + p));
  const bool raise = REXCVAR_GET(sr_stream_native);
  auto up = [&](uint32_t a, uint32_t was) {
    if (raise && uint32_t(want) > was && was > 0 && was < 4096) m.put(a, uint32_t(want));
  };
  up(mgr + 36, count);
  up(mgr + 44, count2);
  // NOT raised: the loader slot also indexes the filter names [mgr+72] ('LOADUNIT0'..'LOADUNIT9',
  // parsed from STREAMINGINIT Filters with [+68] entries before this runs; sub_821A4930 ->
  // sub_821B3C18 turns the name into a 256-bit filter mask). 91 slots read past the 10 names and
  // crashed (2026-11-08). The game's filter file registers LOADUNIT0..LOADUNIT21 (sub_821B3A70,
  // registry [[0x8296DD08]], bits 143..164), so the slots go up to as many of those as exist:
  // a new names table, the game's 10 names first, then guest copies of "LOADUNITn" for every n the
  // registry knows (sub_821DC218 on its hash map at +84).
  if (raise && uint32_t(want) > count3 && count3 > 0 && count3 < 4096) {
    const uint32_t outer = m.u32(0x8296DD08u), reg = outer ? m.u32(outer) : 0, names = m.u32(mgr + 72);
    if (reg && names) {
      const PPCContext saved = ctx;
      std::vector<uint32_t> extra;
      for (uint32_t i = count3; i < uint32_t(want); ++i) {
        const std::string s = "LOADUNIT" + std::to_string(i);
        const uint32_t g = REX_KERNEL_MEMORY()->SystemHeapAlloc(uint32_t(s.size()) + 1, 16);
        if (!g) break;
        std::memcpy(base + g, s.c_str(), s.size() + 1);
        ctx.r3.u64 = reg + 84;
        ctx.r4.u64 = g;
        ctx.r5.u64 = s.size();
        sub_821DC218(ctx, base);
        if (!ctx.r3.u32) break;  // not a registered filter: stop here
        extra.push_back(g);
      }
      ctx = saved;
      if (!extra.empty()) {
        const uint32_t n = count3 + uint32_t(extra.size());
        const uint32_t table = REX_KERNEL_MEMORY()->SystemHeapAlloc(n * 4, 16);
        if (table) {
          for (uint32_t i = 0; i < count3; ++i) m.put(table + 4 * i, m.u32(names + 4 * i));
          for (uint32_t i = 0; i < extra.size(); ++i) m.put(table + 4 * (count3 + i), extra[i]);
          m.put(mgr + 72, table);
          m.put(mgr + 68, n);
        }
      }
    }
  }
  __imp__sub_8219DD00(ctx, base);
  REXLOG_INFO("streaming: memory units {} -> {} of {} bytes ({} MB), pool {:08X}; unit slots {} -> {} of {} bytes ({} MB); "
              "loader slots {} -> {}",
              count, m.u32(mgr + 36), size, uint64_t(m.u32(mgr + 36)) * size >> 20, m.u32(mgr + 408), count2,
              m.u32(mgr + 44), size2, uint64_t(m.u32(mgr + 44)) * size2 >> 20, count3, m.u32(mgr + 68));
}

// A resource's load state (sub_826E4D80: flags at [res]+28 & 0x18: 0x10 -> 2, 0x08 -> 1, else 0),
// counted for sr_stream_log.
REX_HOOK_RAW(sub_826E4D80) {
  __imp__sub_826E4D80(ctx, base);
  if (REXCVAR_GET(sr_stream_log)) ++g_res_state[std::min<uint32_t>(ctx.r3.u32, 2)];
}

REX_HOOK_RAW(sub_821A5398) {
  if (REXCVAR_GET(sr_stream_log)) {
    const Mem m{base};
    const uint32_t r = ctx.r3.u32, ready = m.u32(r + 88);
    ++g_reader_calls;
    g_reader_q[0] = DequeCount(m, m.u32(r + 48));
    g_reader_q[1] = DequeCount(m, m.u32(r + 80));
    g_reader_q[2] = DequeCount(m, m.u32(r + 84));
    g_reader_q[3] = ready ? int32_t((m.u32(ready + 4) - m.u32(ready + 0)) >> 2) : -1;
  }
  __imp__sub_821A5398(ctx, base);
}

// State 2 -> 3 of a loader (sub_821A4930, the loader in r3): the unit needs a slot of the loader's
// own table ([+56] slots at [+60], -1 = free) and the loader not busy ([+76]).
REX_HOOK_RAW(sub_821A4930) {
  if (REXCVAR_GET(sr_stream_log)) {
    static uint64_t last = 0;
    if (NowMs() - last > 3000) {
      last = NowMs();
      const Mem m{base};
      const uint32_t l = ctx.r3.u32, n = m.u32(l + 56), slots = m.u32(l + 60);
      int32_t free_slots = 0;
      for (uint32_t i = 0; i < n && i < 4096; ++i) free_slots += m.u32(slots + 4 * i) == 0xFFFFFFFFu;
      REXLOG_INFO("streamer: loader {:08X} state-3 slots {} free {}, busy {}", l, n, free_slots, m.u8(l + 76));
    }
  }
  __imp__sub_821A4930(ctx, base);
}

// A filter name added to the filter registry (sub_821B3A70, registry in r3, name in r4): the bit
// it takes is the counter [+0] (mask bytes at [+72] + 48 * entry), entries [+76] of [+80], names
// in the hash map at [+84] (buckets [map+8]). sr_stream_log.
REX_HOOK_RAW(sub_821B3A70) {
  const Mem m{base};
  const uint32_t reg = ctx.r3.u32, name = ctx.r4.u32;
  const uint32_t bit = m.u32(reg), entries = m.u32(reg + 76);
  __imp__sub_821B3A70(ctx, base);
  if (REXCVAR_GET(sr_stream_log))
    REXLOG_INFO("filters: {:08X} add '{}' bit {} entry {} -> {} of {}, buckets {}", reg,
                name ? reinterpret_cast<const char*>(base + name) : "", bit, entries, m.u32(reg + 76), m.u32(reg + 80),
                m.u32(reg + 84 + 8));
}

// A path graph lookup by sector id (sub_8225DDA0: list [r3+8], count [r3+40], each entry's id
// byte at +4; the id is the byte at r5's +2 as the caller packs it). NULL crashed sub_82288E20
// once units past the game's 10 loaded (2026-11-08). sr_stream_log logs the misses.
REX_HOOK_RAW(sub_8225DDA0) {
  const Mem m{base};
  const uint32_t list = ctx.r3.u32;
  const uint32_t id = uint32_t(ctx.r5.u64 >> 40) & 0xFF;
  const uint64_t saved_r4 = ctx.r4.u64, saved_r5 = ctx.r5.u64;
  __imp__sub_8225DDA0(ctx, base);
  if (!ctx.r3.u32 && REXCVAR_GET(sr_stream_log)) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 20) {
      std::string ids;
      const uint32_t n = m.u32(list + 40), arr = m.u32(list + 8);
      for (uint32_t i = 0; i < n && i < 64; ++i) ids += " " + std::to_string(m.u8(m.u32(arr + 4 * i) + 4));
      REXLOG_INFO("paths: graph {} missing in {:08X} ({} loaded:{}), current id {} (0x8296DF7B), r4 {:016X} r5 {:016X}",
                  id, list, n, ids, m.u8(0x8296DF7Bu), saved_r4, saved_r5);
      // The world graph's lookup (sub_82219F90 -> sub_82208AE8): pairs {key, object} at
      // [[0x8296DC20]+8 .. +12), key = the reference's first word.
      const uint32_t reg = m.u32(0x8296DC20u);
      std::string keys;
      if (reg)
        for (uint32_t p = m.u32(reg + 8), e = m.u32(reg + 12); p && p < e && keys.size() < 600; p += 8)
          keys += fmt::format(" {}:{:08X}", m.u32(p), m.u32(p + 4));
      REXLOG_INFO("paths: unit registry {:08X} keys{}", reg, keys);
      // The referenced space's sorted table (sub_8219A7A0): [+160] entries at [+176] (8 B,
      // index = ((w >> 14) & 0x1FF) * [+136] + ((w >> 23) & 0x1FF)).
      const uint32_t key = uint32_t(saved_r4 >> 32);
      if (reg)
        for (uint32_t p = m.u32(reg + 8), e = m.u32(reg + 12); p && p < e; p += 8)
          if (m.u32(p) == key) {
            const uint32_t sp = m.u32(p + 4), n2 = m.u32(sp + 160), t = m.u32(sp + 176), stride = m.u32(sp + 136);
            std::string idx;
            for (uint32_t i = 0; i < n2 && i < 40; ++i) {
              const uint32_t w = m.u32(t + 8 * i);
              idx += " " + std::to_string(((w >> 14) & 0x1FF) * stride + ((w >> 23) & 0x1FF));
            }
            REXLOG_INFO("paths: space {} ({:08X}) stride {} entries {}, wanted {}:{}", key, sp, stride, n2,
                        stride * uint32_t((saved_r4 >> 0) & 0xFFFF) + uint32_t((saved_r4 >> 16) & 0xFFFF), idx);
          }
    }
  }
}

// An entity's AI line links (sub_821E6AB0: r4 link type, r5 out buffer of 12-byte references,
// r6 room, r7 entity; returns the count). A reference into a unit still loading names a line
// object whose data is not there yet, so its lookup (sub_8225DDA0) is NULL, and sub_82288E20
// read through it and crashed as soon as units past the game's window loaded (2026-11-08: space
// 20, line 51 present in its table with no data). Such references are dropped here, which is
// the game's own "no link" path (sub_82288E20 skips to the next link when the count is 0).
REX_HOOK_RAW(sub_821E6AB0) {
  const Mem m{base};
  const uint32_t buf = ctx.r5.u32;
  const uint32_t caller = uint32_t(ctx.lr), type = ctx.r4.u32, entity = ctx.r7.u32;
  __imp__sub_821E6AB0(ctx, base);
  const uint32_t lines = m.u32(0x8296E0F4u);
  const int32_t n = int32_t(ctx.r3.u32);
  if (!REXCVAR_GET(sr_stream_native) || REXCVAR_GET(sr_stream_fixed) <= 1 || !lines || n <= 0 || !buf) return;
  const PPCContext saved = ctx;
  int32_t kept = 0;
  for (int32_t i = 0; i < n; ++i) {
    const uint32_t ref = buf + 12 * uint32_t(i);
    ctx.r3.u64 = lines;
    ctx.r4.u64 = (uint64_t(m.u32(ref)) << 32) | m.u32(ref + 4);
    ctx.r5.u64 = uint64_t(m.u32(ref + 8)) << 32;
    sub_8225DDA0(ctx, base);
    if (!ctx.r3.u32) {
      ++g_links_dropped;
      if (REXCVAR_GET(sr_stream_log)) {
        // Who asks (caller, link type) and how many distinct entities: sr_stream_log.
        static std::mutex mu;
        static std::map<uint64_t, std::pair<uint64_t, std::set<uint32_t>>> by;
        static uint64_t last = 0;
        std::lock_guard<std::mutex> lk(mu);
        auto& e = by[(uint64_t(caller) << 32) | type];
        ++e.first;
        if (e.second.size() < 4096) e.second.insert(entity);
        if (NowMs() - last > 3000) {
          last = NowMs();
          std::string s;
          for (auto& [k, v] : by)
            s += fmt::format(" {:08X}/t{}: {} drops {} entities;", uint32_t(k >> 32), uint32_t(k), v.first,
                             v.second.size());
          REXLOG_INFO("paths: dropped links by caller{}", s);
        }
      }
      continue;
    }
    if (kept != i)
      for (uint32_t k = 0; k < 12; k += 4) m.put(buf + 12 * uint32_t(kept) + k, m.u32(ref + k));
    ++kept;
  }
  ctx = saved;
  ctx.r3.u64 = uint32_t(kept);
}

// The path graph manager ([0x8296E0F4], 44 B, vtable 0x820177C0) built by sub_8228A730:
// sr_stream_log watches its graph count (+40) to find who adds and removes graphs.
REX_HOOK_RAW(sub_8228A730) {
  if (REXCVAR_GET(sr_stream_log)) {
    const uint32_t name = ctx.r3.u32;
    REXLOG_INFO("paths: manager built from '{}' (r3 {:08X})", name > 0x10000 ? reinterpret_cast<const char*>(base + name) : "?", name);
  }
  __imp__sub_8228A730(ctx, base);
  if (REXCVAR_GET(sr_stream_log)) {
    const Mem m{base};
    const uint32_t mgr = m.u32(0x8296E0F4u);
    if (mgr) {
      const uint32_t watch[1] = {mgr + 40};
      superman_returns::native::ArmGuestWatch(base, watch, 1);
      REXLOG_INFO("paths: manager {:08X}, watching its graph count", mgr);
    }
  }
}

// The slot pools of sub_8214DD20 (sub_82787940: r4 descriptors of 20 B at r5, {+0 arrays, +4
// slots, +8 bytes, +12 align byte, +16 allocator}; one pool object per descriptor built by
// sub_827871C8, slot count at +88, used-bit map at +100). sr_slot_pool_scale multiplies slots and
// bytes: 22 world units ran the first (8192 slots, 2 MB) out, the failed slot (-1) was freed by
// sub_82779800 and sub_827871A0 read the bit map 512 MB past its start (2026-11-08).
REX_HOOK_RAW(sub_82787940) {
  const Mem m{base};
  const int32_t scale = REXCVAR_GET(sr_slot_pool_scale);
  const uint32_t desc = ctx.r5.u32;
  const int32_t n = int32_t(ctx.r4.u32);
  for (int32_t i = 0; i < n && desc; ++i) {
    const uint32_t d = desc + 20 * uint32_t(i);
    const uint32_t slots = m.u32(d + 4), bytes = m.u32(d + 8);
    if (scale > 1) {
      m.put(d + 4, slots * uint32_t(scale));
      m.put(d + 8, bytes * uint32_t(scale));
    }
    REXLOG_INFO("slot pool {}: {} arrays, {} slots in {} bytes{}", i, m.u32(d), slots, bytes,
                scale > 1 ? fmt::format(" -> {} in {}", m.u32(d + 4), m.u32(d + 8)) : std::string());
  }
  __imp__sub_82787940(ctx, base);
}

// A slot freed in one of those pools (r3 pool, r4 slot): a slot past the pool's count is a
// failed allocation, logged (with the freeing caller) and left alone instead of writing far
// outside the bit map.
REX_HOOK_RAW(sub_827871A0) {
  const Mem m{base};
  const uint32_t pool = ctx.r3.u32, slot = ctx.r4.u32;
  if (pool && slot >= m.u32(pool + 88)) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 20)
      REXLOG_WARN("slot pool {:08X}: free of slot {} of {} (a failed allocation) from {:08X}", pool, int32_t(slot),
                  m.u32(pool + 88), uint32_t(ctx.lr));
    return;
  }
  __imp__sub_827871A0(ctx, base);
}

REX_HOOK_RAW(sub_821995C0) {
  if (REXCVAR_GET(sr_stream_log)) {
    static std::atomic<int> logged{0};
    const Mem m{base};
    const uint32_t o = ctx.r4.u32;
    if (o && logged.fetch_add(1) < 3)
      REXLOG_INFO("streamer: final check object {:08X} vtable {:08X} (vt0 {:08X})", o, m.u32(o), m.u32(m.u32(o)));
  }
  __imp__sub_821995C0(ctx, base);
  if (REXCVAR_GET(sr_stream_log)) ++g_final_check[(ctx.r3.u32 & 0xFF) == 1 ? 1 : 0];
}

// Any world loader busy (sub_82199BF0): what a loading screen polls before it ends.
REX_HOOK_RAW(sub_82199BF0) {
  g_last_poll_ms = NowMs();
  __imp__sub_82199BF0(ctx, base);
  if (REXCVAR_GET(sr_stream_log)) {
    static uint64_t last = 0, calls = 0, busy = 0;
    ++calls;
    busy += ctx.r3.u32 & 0xFF;
    const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch()).count());
    if (now - last > 2000) {
      REXLOG_INFO("stream busy polls: {} calls, {} busy in the last 2 s", calls, busy);
      last = now;
      calls = busy = 0;
    }
  }
}

REX_HOOK_RAW(sub_8278B690) {
  {
    std::lock_guard lock(g_mutex);
    auto it = g_streamers.find(ctx.r3.u32);
    if (it != g_streamers.end())
      REXLOG_INFO("streamer {:08X}: destroyed with {} units loaded", ctx.r3.u32, it->second.loaded.size());
    g_streamers.erase(ctx.r3.u32);
  }
  __imp__sub_8278B690(ctx, base);
}

// Diagnostic (sr_sleep_log): who in the game sleeps (sub_8246A898 = Sleep, via
// KeDelayExecutionThread) and how long. The hitch sampler found the game thread
// asleep in 70% of its slow frames in fast flight; the caller and its caller
// (saved LR in the guest frame) say what it waits for.
REXCVAR_DEFINE_BOOL(sr_sleep_log, false, "Superman Returns",
                    "Debug: log every 5 s the game's Sleep calls by caller (count, time asleep, argument)");

REX_HOOK_RAW(sub_8246A898) {
  if (!REXCVAR_GET(sr_sleep_log)) {
    __imp__sub_8246A898(ctx, base);
    return;
  }
  auto load32 = [base](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + a)); };
  const uint32_t lr = uint32_t(ctx.lr), ms = ctx.r3.u32;
  // The caller's own return address: back chain at 0(r1), saved LR at -8 of it.
  const uint32_t back = load32(ctx.r1.u32);
  const uint32_t lr2 = back ? load32(back - 8) : 0;
  const uint32_t tid = uint32_t(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFF);
  const auto t0 = std::chrono::steady_clock::now();
  __imp__sub_8246A898(ctx, base);
  const uint64_t us =
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
  struct Entry { uint64_t count = 0, us = 0; uint32_t ms = 0; };
  static std::mutex mutex;
  static std::map<std::tuple<uint32_t, uint32_t, uint32_t>, Entry> stats;
  static auto last = std::chrono::steady_clock::now();
  std::lock_guard lock(mutex);
  Entry& e = stats[{tid, lr, lr2}];
  ++e.count;
  e.us += us;
  e.ms = ms;
  if (std::chrono::steady_clock::now() - last < std::chrono::seconds(5)) return;
  last = std::chrono::steady_clock::now();
  std::vector<std::pair<uint64_t, std::string>> rows;
  for (const auto& [k, v] : stats)
    rows.emplace_back(v.us, fmt::format("thread {} caller {:08X} < {:08X}: {} sleeps, {} ms asleep, argument {} ms",
                                        std::get<0>(k), std::get<1>(k), std::get<2>(k), v.count, v.us / 1000, v.ms));
  std::sort(rows.rbegin(), rows.rend());
  for (size_t i = 0; i < rows.size() && i < 10; ++i) REXLOG_INFO("sleep: {}", rows[i].second);
  stats.clear();
}
