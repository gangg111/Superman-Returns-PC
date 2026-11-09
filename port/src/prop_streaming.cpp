// The city's small props (street lamps, traffic lights, benches...) exist as game objects only near
// the camera. sub_821D9880 (a prop manager, r3) walks its list of 48-byte entries (position at +0,
// squared distance to the camera at +36, state at +40, object at +32) and hands each one to
// sub_821D3268 with f1 = the manager's squared radius at +120: an entry within it gets its object
// created, one beyond it (with some hysteresis) has it released. For street lamps that radius is
// about 300 units, so from above the city no lamp beyond it is drawn or lit (found 2026-11-07:
// a lamp's position scanned in memory, a watchpoint on its squared distance).
// sr_prop_distance_scale multiplies the radius (the squared value by its square).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#undef far
#undef near

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "generated/default/superman_returns_init.h"
#include "prop_streaming.h"

// 9: 2700 units, as far as the loaded cells hold props with sr_stream_fixed 4. It needs
// sr_heap_scale (the 19 MB heap ran out at 10 and the game read through a null pointer).
REXCVAR_DEFINE_DOUBLE(sr_prop_distance_scale, 1.0, "Superman Returns",
                      "Distance at which the city's small props (street lamps, traffic lights) exist, times "
                      "the game's (1 = original, about 300 units for lamps)")
    .range(1.0, 50.0);
// The world is a grid of 600 x 600 cells (x -9104..7009, z -7887..4796); the streamer keeps the
// cells within PVSFixed of the camera's (1 = 3 x 3 cells, props to about 1300 units). 5 and more
// (121+ cells) overran the game's lists (room for 100 units in a 1268-byte object); the native
// streamer (world_streamer.cpp, sr_stream_native) has none and applies this radius outside loading
// screens. 27 covers the whole grid (27 x 22 cells): all 1326 units, bay route median 16.73 ms
// against 16.85 at 4 (2026-11-07). BUT (2026-11-08) a unit outside the game's window never gets
// past the loader's state 1 (it waits for a "ready" list another system fills): the far units
// stayed queued for the whole flight and only blocked the game's waits. So the default is the
// game's 1 until that system is found; wider is an experiment.
REXCVAR_DEFINE_INT32(sr_stream_fixed, 1, "Superman Returns",
                     "Radius in 600-unit cells of the world kept loaded around the camera (the game's 1; "
                     "above 4 only with sr_stream_native)")
    .range(0, 27);
REXCVAR_DEFINE_BOOL(sr_lod_no_cull, false, "Superman Returns",
                    "Models past their last detail threshold (street lamps at 800) keep their farthest "
                    "level instead of vanishing");
REXCVAR_DEFINE_DOUBLE(sr_heap_scale, 1.0, "Superman Returns",
                      "Size of the game's heaps times this (they were cut to the console's 512 MB)")
    .range(1.0, 8.0);
REXCVAR_DEFINE_BOOL(sr_crash_log, true, "Superman Returns",
                    "Log failed allocations of the game's heaps and guest null-pointer faults");
REXCVAR_DEFINE_INT32(sr_command_ring_mb, 2, "Superman Returns",
                     "Size of the D3D secondary command ring in MB (the game's is 2; the whole city in full "
                     "detail outgrew it and the game froze)")
    .range(2, 256);
REXCVAR_DEFINE_BOOL(sr_lod_instance_room, false, "Superman Returns",
                    "Give every detail level of the city's objects room for all their instances (the game's "
                    "fixed shares overflow when the far cuts are raised)");
REXCVAR_DEFINE_BOOL(sr_prop_log, false, "Superman Returns", "Debug: log the prop managers' lists");
REXCVAR_DEFINE_BOOL(sr_prop_scan, false, "Superman Returns",
                    "Scan every prop manager twice a second on the game thread (street lamps lit at night)");
REXCVAR_DECLARE(bool, sr_stream_native);
REXCVAR_DECLARE(bool, sr_stream_check);

namespace {

float LoadF(uint8_t* base, uint32_t addr) {
  uint32_t u;
  std::memcpy(&u, base + addr, 4);
  u = __builtin_bswap32(u);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

void StoreF(uint8_t* base, uint32_t addr, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  u = __builtin_bswap32(u);
  std::memcpy(base + addr, &u, 4);
}

std::mutex g_entries_mutex;
std::vector<superman_returns::PropEntry> g_entries;  // every prop manager's entries, refreshed twice a second
uint32_t g_manager_vtable = 0;

// The game's update list (sub_821C2468): [0x8296DD0C] -> {begin, end} of 12-byte items, the
// object at +4. Prop managers are told apart by their vtable; their entries are [+100, +104).
void ScanManagers(uint8_t* base) {
  auto u32 = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); };
  const uint32_t list = u32(0x8296DD0Cu);
  if (!list || !g_manager_vtable) return;
  const uint32_t begin = u32(list), end = u32(list + 4);
  if (end < begin || (end - begin) / 12 > 100000) return;
  if (REXCVAR_GET(sr_prop_log)) {
    static int logged = 0;
    if (logged++ % 20 == 0)
      REXLOG_INFO("props: update list {:08X} items {} ({:08X}..{:08X})", list, (end - begin) / 12, begin, end);
  }
  std::vector<superman_returns::PropEntry> out;
  static int scans = 0;
  const bool dump = REXCVAR_GET(sr_prop_log) && ++scans == 60;
  for (uint32_t it = begin; it < end; it += 12) {
    const uint32_t obj = u32(it + 4);
    if (!obj || u32(obj) != g_manager_vtable) continue;
    const uint32_t first = u32(obj + 100), last = u32(obj + 104);
    if (!first || last < first || (last - first) % 48 || (last - first) / 48 > 20000) continue;
    if (dump) {
      std::string h;
      for (uint32_t k = 0; k < 160; k += 4) h += fmt::format(" {:08X}", u32(obj + k));
      std::string ps;
      for (uint32_t e = first; e < last && e < first + 48 * 6; e += 48)
        ps += fmt::format(" ({:.1f} {:.1f} {:.1f})", LoadF(base, e), LoadF(base, e + 4), LoadF(base, e + 8));
      REXLOG_INFO("prop manager {:08X} n {}:{} |{}", obj, (last - first) / 48, h, ps);
    }
    if (REXCVAR_GET(sr_prop_log)) {
      // A manager seen for the first time: where its props are (the sector that just loaded).
      static std::unordered_set<uint32_t> seen;
      if (seen.insert(obj).second) {
        double c[3] = {};
        const uint32_t n = (last - first) / 48;
        for (uint32_t e = first; e < last; e += 48)
          for (int k = 0; k < 3; ++k) c[k] += LoadF(base, e + 4 * k) / n;
        REXLOG_INFO("props: new manager {:08X} type {:08X} n {} centre ({:.0f} {:.0f} {:.0f})", obj, u32(obj + 116), n,
                    c[0], c[1], c[2]);
      }
    }
    for (uint32_t e = first; e < last; e += 48) {
      superman_returns::PropEntry p;
      for (int k = 0; k < 3; ++k) p.pos[k] = LoadF(base, e + 4 * k);
      p.kind = u32(obj + 116);  // the manager's prop type (a name hash; street lamps D190586A)
      if (std::isfinite(p.pos[0]) && std::isfinite(p.pos[1]) && std::isfinite(p.pos[2])) out.push_back(p);
    }
  }
  std::lock_guard lock(g_entries_mutex);
  g_entries.swap(out);
}

struct Radius {
  float original;  // the game's squared radius
  float written;   // what we last wrote
};
std::unordered_map<uint32_t, Radius> g_radius;  // per manager

}  // namespace

// sr_crash_log: on an access violation, log the faulting instruction and the return addresses
// found on the stack, as offsets into the exe (look them up in superman_returns.map).
namespace superman_returns::native {
void ArmGuestWatch(uint8_t* base, const uint32_t* guest, int n);
}

static LONG CALLBACK CrashLogHandler(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  // Only the guest's first 64 KB (a null pointer in guest code): MMIO and write watches also
  // fault on purpose elsewhere.
  const uint64_t fault = info->ExceptionRecord->ExceptionInformation[1];
  if (fault < 0x100000000ull || fault >= 0x100010000ull) return EXCEPTION_CONTINUE_SEARCH;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) >= 6) return EXCEPTION_CONTINUE_SEARCH;
  const uint64_t module = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
  MODULEINFO mi{};
  GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &mi, sizeof(mi));
  const uint64_t end = module + mi.SizeOfImage;
  const CONTEXT* c = info->ContextRecord;
  std::string s = fmt::format("crash: rip exe+{:X} addr {:X} rw {} |", c->Rip - module,
                              info->ExceptionRecord->ExceptionInformation[1],
                              info->ExceptionRecord->ExceptionInformation[0]);
  const uint64_t* sp = reinterpret_cast<const uint64_t*>(c->Rsp);
  int found = 0;
  for (int i = 0; i < 2048 && found < 24; ++i) {
    const uint64_t v = sp[i];
    if (v > module && v < end) {
      s += fmt::format(" {:X}", v - module);
      ++found;
    }
  }
  REXLOG_ERROR("{}", s);
  return EXCEPTION_CONTINUE_SEARCH;
}
static const bool g_crash_log_installed = [] {
  AddVectoredExceptionHandler(1, CrashLogHandler);
  return true;
}();

std::vector<superman_returns::PropEntry> superman_returns::PropEntries() {
  std::lock_guard lock(g_entries_mutex);
  return g_entries;
}

namespace {
void LogManagerSpan(uint8_t* base, uint32_t obj, const char* what) {
  if (!REXCVAR_GET(sr_prop_log) || !obj) return;
  auto u32 = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); };
  const uint32_t first = u32(obj + 100), last = u32(obj + 104);
  if (!first || last <= first || (last - first) % 48 || (last - first) / 48 > 20000) {
    REXLOG_INFO("props: manager {:08X} {} (no entries)", obj, what);
    return;
  }
  double c[3] = {}, r = 0.0;
  const uint32_t n = (last - first) / 48;
  for (uint32_t e = first; e < last; e += 48)
    for (int k = 0; k < 3; ++k) c[k] += LoadF(base, e + 4 * k) / n;
  for (uint32_t e = first; e < last; e += 48) {
    const double dx = LoadF(base, e) - c[0], dz = LoadF(base, e + 8) - c[2];
    r = std::max(r, std::sqrt(dx * dx + dz * dz));
  }
  REXLOG_INFO("props: manager {:08X} {} type {:08X} n {} centre ({:.0f} {:.0f} {:.0f}) spread {:.0f}", obj, what,
              u32(obj + 116), n, c[0], c[1], c[2], r);
}
}  // namespace

// sr_prop_log: sub_823055A8 (r4 = the instanced object) writes each instance's matrix into the
// buffer of its detail level ([obj + 236 + 4 l], counted at [obj + 216 + 4 l], [obj + 212] levels,
// [obj + 152] instances) without a bound: log when a level's count passes the room before the
// next level's buffer.
// The Xbox game splits an object's instance buffer between its detail levels in the shares it
// expected (an object of 50 instances: room for 25 at full detail) and writes past them without
// a check. With the far cuts raised (sr_lod_far_scale, sr_lod_small_scale) nearly every instance
// is full detail: 34 of 50 went into room for 25, ran over the next buffers and the game froze.
// Each level gets its own block in guest memory with room for every instance of the object. The
// game's buffers sit inside the object's own allocation, so nothing frees these pointers.
struct LevelRoom {
  uint32_t block = 0, instances = 0;
};
static std::unordered_map<uint32_t, LevelRoom> g_level_room;

static std::mutex g_level_room_mutex;

static void GiveLevelsRoom(uint8_t* base, uint32_t obj) {
  if (!obj) return;
  std::lock_guard lock(g_level_room_mutex);
  auto u32 = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); };
  auto put = [&](uint32_t a, uint32_t v) { *reinterpret_cast<uint32_t*>(base + a) = __builtin_bswap32(v); };
  const uint32_t levels = u32(obj + 212), inst = u32(obj + 152);
  if (levels < 1 || levels > 5 || inst == 0 || inst > 100000) return;
  LevelRoom& r = g_level_room[obj];
  const uint32_t stride = inst * 64;
  bool ours = r.block != 0 && r.instances >= inst;
  for (uint32_t l = 0; ours && l < levels; ++l) ours = u32(obj + 236 + 4 * l) == r.block + l * r.instances * 64;
  if (ours) return;
  if (r.block == 0 || r.instances < inst) {
    // A new object here (or one with more instances): a fresh block; an old one too small leaks.
    r.block = REX_KERNEL_MEMORY()->SystemHeapAlloc(levels * stride, 128);
    r.instances = inst;
    if (!r.block) {
      g_level_room.erase(obj);
      return;
    }
  }
  for (uint32_t l = 0; l < levels; ++l) put(obj + 236 + 4 * l, r.block + l * r.instances * 64);
}

static void CheckInstanceLevels(uint8_t* base, uint32_t render_object) {
  if (!REXCVAR_GET(sr_prop_log) || !render_object) return;
  auto u32 = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); };
  // sub_823055A8 keeps the render object (sub_8230F638's r3) in r29: the fields are its own.
  const uint32_t obj = render_object;
  const uint32_t levels = u32(obj + 212), inst = u32(obj + 152);
  if (levels < 1 || levels > 4) return;
  {
    // Is a known street lamp (x -2297.904, z -410.922) one of this object's instances
    // ([obj + 148]: 64-byte matrices, position in the 4th row)?
    static std::unordered_set<uint32_t> told;
    const uint32_t arr = u32(obj + 148);
    for (uint32_t i = 0; arr && i < inst && i < 5000; ++i) {
      const float x = LoadF(base, arr + i * 64 + 48), z = LoadF(base, arr + i * 64 + 56);
      if (std::fabs(x + 2297.904f) < 0.05f && std::fabs(z + 410.922f) < 0.05f && told.insert(obj).second) {
        REXLOG_INFO("instances: lamp at index {} of object {:08X}: instances {} levels {} counts {} {} {}", i, obj,
                    inst, levels, u32(obj + 216), u32(obj + 220), u32(obj + 224));
        break;
      }
    }
  }
  static uint32_t logged = 0, checked = 0;
  ++checked;
  for (uint32_t l = 0; l < levels; ++l) {
    const uint32_t count = u32(obj + 216 + 4 * l), buf = u32(obj + 236 + 4 * l);
    uint32_t room = ~0u;
    for (uint32_t m = 0; m < levels; ++m) {
      const uint32_t other = u32(obj + 236 + 4 * m);
      if (other > buf) room = std::min(room, (other - buf) / 64);
    }
    if (room != ~0u && count > room && logged < 40) {
      ++logged;
      REXLOG_INFO("instances: object {:08X} level {} count {} > room {} (instances {}, levels {}, buffers {:08X} {:08X})",
                  obj, l, count, room, inst, levels, u32(obj + 236), u32(obj + 240));
    }
  }
  if (checked % 200000 == 0)
    REXLOG_INFO("instances: {} objects checked, {} overflows logged; sample {:08X} inst {} levels {} counts {} {} "
                "buffers {:08X} {:08X}", checked, logged, obj, inst, levels, u32(obj + 216), u32(obj + 220),
                u32(obj + 236), u32(obj + 240));
}

// The D3D device's command ring (sub_820FDE90, r4 = D3DRING_BUFFER_PARAMETERS: +4 primary size,
// +8 primary address, +12 secondary size, +16 secondary address, +20 segments; 0 = 32 KB, 2 MB,
// 32). With the whole city in full detail one frame's commands outgrew the 2 MB secondary ring:
// the game waited (sub_820FCB30) for the GPU to read past commands it had not sent yet, and froze.
// A PC has the memory: sr_command_ring_mb sets the secondary ring (when the game asks for less).
// sr_crash_log: a failed allocation of the game's allocator (sub_8278D258: r3 size, r5 heap or 0 =
// the current one): which heap ran out, its vtable and header.
REX_HOOK_RAW(sub_8278D258) {
  const uint32_t size = ctx.r3.u32, heap = ctx.r5.u32, lr = uint32_t(ctx.lr);
  __imp__sub_8278D258(ctx, base);
  if (ctx.r3.u32 == 0 && REXCVAR_GET(sr_crash_log)) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 8) {
      auto u32 = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); };
      std::string h;
      for (uint32_t k = 0; heap && k < 96; k += 4) h += fmt::format(" {:08X}", u32(heap + k));
      REXLOG_ERROR("alloc failed: size {} heap {:08X} from {:08X}:{}", size, heap, lr, h);
    }
  }
}

// The game's heaps (sub_8278E1F8: r3 the heap, r4 its parameters, +0 the size, +4 a buffer to
// use or 0). Their sizes add up to about 100 MB, cut to the console's 512: with the props'
// radius raised the 19 MB heap 82967FF8 ran out, an allocation returned 0 and the game read
// through it (sub_82305B88). sr_heap_scale multiplies every heap the game allocates itself.
REX_HOOK_RAW(sub_8278E1F8) {
  const uint32_t heap = ctx.r3.u32, params = ctx.r4.u32, lr = uint32_t(ctx.lr);
  if (params) {
    auto u32 = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); };
    auto put = [&](uint32_t a, uint32_t v) { *reinterpret_cast<uint32_t*>(base + a) = __builtin_bswap32(v); };
    const double scale = REXCVAR_GET(sr_heap_scale);
    const uint32_t size = u32(params), buffer = u32(params + 4);
    if (scale > 1.0 && size && !buffer) {
      const uint64_t want = (uint64_t(double(size) * scale) + 0xFFFF) & ~0xFFFFull;
      if (want < 0x40000000ull) {
        put(params, uint32_t(want));
        REXLOG_INFO("heap: {:08X} raised from {} to {} MB", heap, size >> 20, uint32_t(want) >> 20);
      }
    }
  }
  if (REXCVAR_GET(sr_crash_log)) {
    auto u32 = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); };
    std::string p;
    for (uint32_t k = 0; params && k < 64; k += 4) p += fmt::format(" {:08X}", u32(params + k));
    REXLOG_INFO("heap: build {:08X} from {:08X} params {:08X}:{}", heap, lr, params, p);
  }
  __imp__sub_8278E1F8(ctx, base);
  if (REXCVAR_GET(sr_crash_log)) {
    auto u32 = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); };
    std::string h;
    for (uint32_t k = 0; k < 96; k += 4) h += fmt::format(" {:08X}", u32(heap + k));
    REXLOG_INFO("heap: built {:08X}:{}", heap, h);
  }
}

REX_HOOK_RAW(sub_820FDE90) {
  const uint32_t params = ctx.r4.u32;
  if (params) {
    auto u32 = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); };
    auto put = [&](uint32_t a, uint32_t v) { *reinterpret_cast<uint32_t*>(base + a) = __builtin_bswap32(v); };
    const uint32_t size = u32(params + 12), address = u32(params + 16);
    const uint32_t want = uint32_t(REXCVAR_GET(sr_command_ring_mb)) << 20;
    const uint32_t current = size ? size : (2u << 20);
    REXLOG_INFO("d3d: command ring primary {} B, secondary {} B at {:08X}, segments {}", u32(params + 4), size,
                address, u32(params + 20));
    if (address == 0 && want > current) {
      put(params + 12, want);
      REXLOG_INFO("d3d: secondary command ring raised to {} MB", want >> 20);
    }
  }
  __imp__sub_820FDE90(ctx, base);
}

// sr_prop_log: sub_82318718 (vtable 0x820211C0 + 88) stores a prop render object's squared distance
// to the camera at +32: log a few objects that are about 500 units away (the props are cut at 800).
REX_HOOK_RAW(sub_82318718) {
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_82318718(ctx, base);
  if (!REXCVAR_GET(sr_prop_log)) return;
  static int logged = 0;
  static uint64_t calls = 0;
  const float d2 = LoadF(base, obj + 32);
  // A prop just past the 800 cut: watch who reads its distance (+32) and its flags (+16).
  static bool watched = false;
  if (!watched && calls > 3000000 && d2 > 820.0f * 820.0f && d2 < 900.0f * 900.0f) {
    watched = true;
    const uint32_t at[2] = {obj + 32, obj + 16};
    REXLOG_INFO("props: watching object {:08X} at {:.0f}", obj, std::sqrt(d2));
    superman_returns::native::ArmGuestWatch(base, at, 2);
  }
  // Late in the run: the menu's background city is released when the save loads.
  if (++calls > 3000000 && logged < 6 && d2 > 450.0f * 450.0f && d2 < 550.0f * 550.0f) {
    ++logged;
    auto u32 = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); };
    auto dump = [&](uint32_t at, uint32_t words) {
      std::string s;
      for (uint32_t k = 0; at && k < words; ++k) {
        const uint32_t w = u32(at + k * 4);
        float f;
        std::memcpy(&f, &w, 4);
        s += std::fabs(f) > 1e-3f && std::fabs(f) < 1e6f ? fmt::format(" {}:{:.3f}", k * 4, f) : fmt::format(" {}:#{:X}", k * 4, w);
      }
      return s;
    };
    const uint32_t m1 = u32(obj + 88), m2 = m1 ? u32(m1 + 12) : 0, model = m2 ? u32(m2 + 12) : 0;
    REXLOG_INFO("prop object {:08X} distance {:.0f}:{}", obj, std::sqrt(d2), dump(obj, 64));
    REXLOG_INFO("prop object {:08X} m1 {:08X}:{}", obj, m1, dump(m1, 32));
    REXLOG_INFO("prop object {:08X} model {:08X}:{} | +68 -> {}", obj, model, dump(model, 48),
                model ? dump(u32(model + 68), 24) : std::string());
  }
}

// A model's detail level (sub_822EA908, from sub_822FFB28): how many of the model's thresholds
// (r5, r4 of them: squared distances) the squared distance passes. A level equal to the count is
// past the last one and the object is not drawn: street lamps (thresholds 100 and 800 units)
// vanished at 800. sr_lod_no_cull keeps the last level, drawn at any distance.
REX_HOOK_RAW(sub_822EA908) {
  const uint32_t thresholds = ctx.r4.u32;
  __imp__sub_822EA908(ctx, base);
  if (REXCVAR_GET(sr_lod_no_cull) && thresholds > 0 && ctx.r3.u32 >= thresholds) ctx.r3.u64 = thresholds - 1;
}

REX_HOOK_RAW(sub_8230F638) {
  if (REXCVAR_GET(sr_prop_log)) {
    static int n = 0;
    if (n++ % 20000 == 0) {
      // The instance culling frustum (sub_82319108 tests 5 planes: x y z d each).
      const uint32_t fr = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + 0x82970154u));
      std::string s;
      for (int p = 0; p < 5 && fr; ++p)
        s += fmt::format(" ({:.3f} {:.3f} {:.3f} {:.1f})", LoadF(base, fr + p * 16), LoadF(base, fr + p * 16 + 4),
                         LoadF(base, fr + p * 16 + 8), LoadF(base, fr + p * 16 + 12));
      REXLOG_INFO("render frustum {:08X}:{}", fr, s);
      // The props' frustum (sub_82305B88 tests against [0x829A9390]).
      const uint32_t pf = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + 0x829A9390u));
      std::string ps;
      for (int p = 0; p < 5 && pf; ++p)
        ps += fmt::format(" ({:.3f} {:.3f} {:.3f} {:.1f})", LoadF(base, pf + p * 16), LoadF(base, pf + p * 16 + 4),
                          LoadF(base, pf + p * 16 + 8), LoadF(base, pf + p * 16 + 12));
      REXLOG_INFO("prop frustum {:08X}:{}", pf, ps);
    }
  }
  const uint32_t render_object = ctx.r3.u32;
  if (REXCVAR_GET(sr_lod_instance_room)) GiveLevelsRoom(base, render_object);
  __imp__sub_8230F638(ctx, base);
  CheckInstanceLevels(base, render_object);
}

// The guest's call chain: each frame's back chain at 0(sp), the caller's return address saved at
// back chain - 8 (prologues do "stw r12,-8(r1)" before "stwu r1,-N(r1)").
static std::string GuestStack(uint8_t* base, uint32_t sp, int frames) {
  auto u32 = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); };
  auto readable = [&](uint32_t a) {
    MEMORY_BASIC_INFORMATION mbi{};
    return VirtualQuery(base + a, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
           !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
  };
  std::string s;
  for (int i = 0; i < frames && sp > 0x1000 && sp < 0xF0000000u && readable(sp); ++i) {
    const uint32_t caller = u32(sp);
    if (caller <= sp || caller - sp > 0x100000 || !readable(caller - 8)) break;
    s += fmt::format(" {:08X}", u32(caller - 8));
    sp = caller;
  }
  return s;
}

// sr_prop_log: each load unit the streaming config registers (sub_821A0AF0: r3 the streaming
// manager, r4 the unit's name; its PVSFixed / PVSSliding numbers were just read into +436 / +440).
REX_HOOK_RAW(sub_821A0AF0) {
  if (REXCVAR_GET(sr_prop_log)) {
    static int n = 0;
    if (n++ < 900) {
      std::string name;
      for (uint32_t k = 0; k < 64 && base[ctx.r4.u32 + k]; ++k) name += char(base[ctx.r4.u32 + k]);
      REXLOG_INFO("streaming: load unit '{}' fixed {} sliding {} (manager {:08X} count {})", name,
                  int32_t(__builtin_bswap32(*reinterpret_cast<uint32_t*>(base + ctx.r3.u32 + 436))),
                  int32_t(__builtin_bswap32(*reinterpret_cast<uint32_t*>(base + ctx.r3.u32 + 440))), ctx.r3.u32,
                  int32_t(__builtin_bswap32(*reinterpret_cast<uint32_t*>(base + ctx.r3.u32 + 444))));
    }
  }
  __imp__sub_821A0AF0(ctx, base);
}

// The world streamer (sub_8278B610: r5 the grid of sub_8278B6E0, r7 PVSFixed, r8 PVSSliding from
// the streaming config; "StreamingMapBig" gives 1 and 0). sr_stream_fixed overrides PVSFixed.
REX_HOOK_RAW(sub_8278B610) {
  const int32_t fixed = int32_t(ctx.r7.u32), sliding = int32_t(ctx.r8.u32);
  // The game's window stays in the object: any wider one made the boot loading screen wait forever
  // (2026-11-08, also with the game's own streamer at 4). The native streamer widens it to
  // sr_stream_fixed outside loading screens (world_streamer.cpp).
  REXLOG_INFO("streaming: streamer grid {:08X} fixed {} -> {} sliding {}", ctx.r5.u32, fixed, int32_t(ctx.r7.u32),
              sliding);
  __imp__sub_8278B610(ctx, base);
}

// The grid (sub_8278B6E0: r4..r9 integers = world bounds and cell sizes, order as the caller
// passes them): logged.
REX_HOOK_RAW(sub_8278B6E0) {
  REXLOG_INFO("streaming: grid {} {} {} {} cells {} x {}", int32_t(ctx.r4.u32), int32_t(ctx.r5.u32),
              int32_t(ctx.r6.u32), int32_t(ctx.r7.u32), int32_t(ctx.r8.u32), int32_t(ctx.r9.u32));
  __imp__sub_8278B6E0(ctx, base);
}

REX_HOOK_RAW(sub_821A0448) {
  if (REXCVAR_GET(sr_prop_log)) {
    static int n = 0;
    if (n++ < 6)
      REXLOG_INFO("props: loader sub_821A0448 from {:08X} r3 {:08X} r4 {:08X} r5 {:08X} r6 {:08X} stack{}",
                  uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, GuestStack(base, ctx.r1.u32, 16));
  }
  __imp__sub_821A0448(ctx, base);
}

REX_HOOK_RAW(sub_821D6078) {
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_821D6078(ctx, base);
  LogManagerSpan(base, obj, "created");
}

REX_HOOK_RAW(sub_821D0580) {
  LogManagerSpan(base, ctx.r3.u32, "destroyed");
  __imp__sub_821D0580(ctx, base);
}

REX_HOOK_RAW(sub_821D9880) {
  const uint32_t manager = ctx.r3.u32;
  const float scale = float(REXCVAR_GET(sr_prop_distance_scale));
  if (manager) {
    g_manager_vtable = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + manager));
    static auto last_scan = std::chrono::steady_clock::time_point{};
    const auto now_t = std::chrono::steady_clock::now();
    if (REXCVAR_GET(sr_prop_scan) && now_t - last_scan > std::chrono::milliseconds(500)) {
      last_scan = now_t;
      ScanManagers(base);
    }
    const float now = LoadF(base, manager + 120);
    auto it = g_radius.find(manager);
    // A new manager, or the game set its radius again: that value is the original.
    if (it == g_radius.end() || now != it->second.written) {
      Radius r{now, now};
      it = g_radius.insert_or_assign(manager, r).first;
      if (REXCVAR_GET(sr_prop_log))  // dozens at once when a sector loads: not on the game thread by default
        REXLOG_INFO("props: manager {:08X} radius {:.1f} (squared {:.1f}), scale {}", manager,
                    std::sqrt(std::fabs(now)), now, scale);
    }
    if (REXCVAR_GET(sr_prop_log)) {
      static uint32_t calls = 0;
      if (calls < 3) REXLOG_INFO("props: called from {:08X} (r4 {:08X} r5 {:08X} f1 {})", uint32_t(ctx.lr),
                                 ctx.r4.u32, ctx.r5.u32, ctx.f1.f64);
      if (++calls % 2000 == 0) {
        const uint32_t first = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + manager + 100));
        const uint32_t last = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + manager + 104));
        uint32_t n = 0, state[4] = {};
        float far = 0.0f;
        for (uint32_t e = first; e && e < last && n < 100000; e += 48, ++n) {
          far = std::max(far, LoadF(base, e + 36));
          ++state[base[e + 40] & 3];
        }
        REXLOG_INFO("props: manager {:08X} {} entries, farthest {:.0f}, states {} {} {} {}", manager, n,
                    std::sqrt(far), state[0], state[1], state[2], state[3]);
      }
    }
    const float want = it->second.original * scale * scale;
    if (std::isfinite(want) && want > 0.0f && now != want) {
      StoreF(base, manager + 120, want);
      it->second.written = want;
    }
  }
  __imp__sub_821D9880(ctx, base);
}
