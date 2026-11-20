// Ported from crazyriddler/rexglue-native-kit @136bc6c4,
// reference/conan/port/src/native/shader_registry.cpp (Conan native renderer).
// The kit ships no license file at that revision; parts derived from Xenia /
// ReXGlue keep their BSD license. Changes for Superman Returns are listed in
// docs/native-port-plan.md section 2.
//
// Changes: hook addresses from game_profile.h (built only once confirmed),
// header validation (shader_container.h), an optional dump of every
// container the game creates (sr_native_dump_shader_dir), which feeds
// tools/shaders/extract_shaders.py --dump-dir when the game data stores its
// shaders compressed, and the pre-translated shader library
// (sr_native_preshaders, shader_library.h) that recognises shaders by their
// original containers as StevensND/nfsmw-nx does.
//
// Identification: the XDK creators (game_profile.h CREATE_SHADER_A / _B) get the
// finished container in r3 and return the new shader object, so the container
// is hashed there (XXH3 of virtual + physical bytes, the corpus key) and the
// object registered under it. Direct3D patches the object's copy at bind time,
// which is why hashing the object's bytes at draw time is not stable across
// runs; TryRegisterInlineShaders remains only as a fallback for objects made
// before the hooks ran. The pre-shader library (sr_native_preshaders) holds the
// DXIL for those hashes and the original containers.
#include "shader_registry.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "game_profile.h"
#include "../graphics/guest/shader_slots.h"
#include "native_bridge.h"
#include "shader_container.h"
#include "shader_library.h"
#include "shader_translator.h"

REXCVAR_DEFINE_STRING(sr_native_dump_shader_dir, "", "Superman Returns Native",
                      "Write every shader container seen by the XDK CreateShader hooks to "
                      "<dir>/<hash>.<vs|ps>.bin (input for tools/shaders/extract_shaders.py); "
                      "containers the pre-shader library does not recognise go to <dir>/unmatched")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(sr_native_preshaders, true, "Superman Returns Native",
                    "Use the pre-translated shader library (superman_returns_shaders.srsl, made "
                    "from your game by tools/shaders/make_preshaders.py): shaders are recognised "
                    "by their original containers and their DXIL comes from the library")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(sr_native_preshaders_path, "", "Superman Returns Native",
                      "Pre-shader library file; default: superman_returns_shaders.srsl next to "
                      "the executable, then <repo>/artifacts/shaders/")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace superman_returns::native {

namespace {
std::mutex g_mutex;
std::unordered_map<uint32_t, GuestShaderInfo> g_shaders;
// Objects seen at draw time that nothing recognised: not hashed again until a
// creator hook reuses the address.
std::unordered_set<uint32_t> g_unknown;
uint32_t g_match_counts[4] = {};


constexpr char kLibraryName[] = "superman_returns_shaders.srsl";

// Guest addresses whose next `bytes` stay inside the 4 GB guest space.
bool GuestRange(uint32_t addr, uint32_t bytes) {
  return addr && uint64_t(addr) + bytes <= 0xFFFFFF00ull;
}

size_t GuestAvailable(uint32_t addr) { return size_t(0x100000000ull - addr); }

// The immutable capture a draw keeps of a shader, with the texture slots it can read.
std::shared_ptr<graphics::guest::ShaderCapture> NewCapture(uint64_t hash, bool vertex, bool dynamic_vertex_fetch,
                                                           std::vector<uint8_t> container) {
  auto capture = std::make_shared<graphics::guest::ShaderCapture>();
  capture->hash = hash;
  capture->vertex = vertex;
  capture->dynamic_vertex_fetch = dynamic_vertex_fetch;
  capture->container = std::move(container);
  capture->texture_slots = graphics::guest::ShaderTextureSlots(capture->container);
  capture->float_constants = graphics::guest::ShaderFloatConstants(capture->container);
  return capture;
}

struct LoadedLibrary {
  ShaderLibrary library;
  std::string where;
  bool ok = false;
};

const LoadedLibrary& Library() {
  static const LoadedLibrary loaded = [] {
    LoadedLibrary l;
    if (!REXCVAR_GET(sr_native_preshaders)) {
      l.where = "disabled (sr_native_preshaders=false)";
      return l;
    }
    std::vector<std::filesystem::path> paths;
    const std::string cvar = REXCVAR_GET(sr_native_preshaders_path);
    if (!cvar.empty()) {
      paths.emplace_back(cvar);
    } else {
      const auto exe = rex::filesystem::GetExecutableFolder();
      paths.push_back(exe / kLibraryName);
      // Development layout: port/out/build/<preset>/superman_returns.exe.
      paths.push_back((exe / ".." / ".." / ".." / ".." / "artifacts" / "shaders" / kLibraryName)
                          .lexically_normal());
    }
    for (const auto& path : paths) {
      std::ifstream f(path, std::ios::binary | std::ios::ate);
      if (!f) continue;
      const auto n = f.tellg();
      if (n <= 0 || size_t(n) > ShaderLibrary::kMaxFile) {
        REXLOG_WARN("pre-shaders: {} has an invalid size", path.string());
        continue;
      }
      std::vector<uint8_t> data(static_cast<size_t>(n));
      f.seekg(0);
      if (!f.read(reinterpret_cast<char*>(data.data()), data.size())) continue;
      std::string error;
      if (!l.library.Load(data.data(), data.size(), &error)) {
        REXLOG_WARN("pre-shaders: {} rejected: {} (rebuild it with "
                    "tools/shaders/make_preshaders.py)", path.string(), error);
        continue;
      }
      l.ok = true;
      l.where = path.string();
      REXLOG_INFO("pre-shaders: {} shaders from {}", l.library.size(), l.where);
      return l;
    }
    l.where = "not found (" + paths.front().string() + ")";
    REXLOG_INFO("pre-shaders: library {}; using the legacy container hashes", l.where);
    return l;
  }();
  return loaded;
}

// Kit: hash [c, c + virtual + physical) exactly like the corpus (legacy path,
// without a library).
bool TryHashContainer(uint8_t* base, uint32_t container, GuestShaderInfo& info) {
  if (!GuestRange(container, 12)) return false;
  ShaderContainerHeader header;
  if (!ParseShaderContainerHeader(base + container, GuestAvailable(container), header)) {
    return false;
  }
  info.is_vertex = header.is_vertex;
  info.dynamic_vertex_fetch = header.is_vertex &&
      ShaderHasInstanceData(base + container, header.virtual_size);
  info.container_hash = XXH3_64bits(base + container, header.total_size());
  info.match = PreShaderMatch::kNone;
  info.capture = NewCapture(info.container_hash, info.is_vertex, info.dynamic_vertex_fetch,
                            std::vector<uint8_t>(base + container, base + container + header.total_size()));
  return true;
}

bool HasContainerMagic(uint8_t* base, uint32_t addr) {
  return GuestRange(addr, 16) && (LoadBigEndian32(base + addr) & 0xFFFFFF00u) == 0x102A1100u;
}

// Library first; without one, the kit's exact hash.
bool IdentifyAt(uint8_t* base, uint32_t container, GuestShaderInfo& info) {
  if (!HasContainerMagic(base, container)) return false;
  const LoadedLibrary& lib = Library();
  if (!lib.ok) return TryHashContainer(base, container, info);
  PreShaderMatch how;
  const PreShader* s = lib.library.Identify(base + container, GuestAvailable(container), &how);
  if (!s) return false;
  info.container_hash = s->container_hash;
  info.is_vertex = s->vertex;
  info.dynamic_vertex_fetch = s->dynamic_vertex_fetch;
  info.match = how;
  info.capture = NewCapture(info.container_hash, info.is_vertex, info.dynamic_vertex_fetch, s->container);
  return true;
}

// Containers embedded in the shader objects (draw-time fallback). The VS/PS
// object layouts are not confirmed per slot, so both offsets are tried and
// the stage of the match must agree with the slot.
uint32_t ResolveInline(uint8_t* base, uint32_t obj, bool vertex, GuestShaderInfo& info) {
  const LoadedLibrary& lib = Library();
  for (uint32_t off : {kVSContainerOffset, kPSContainerOffset}) {
    const uint32_t c = obj + off;
    if (!HasContainerMagic(base, c)) continue;
    GuestShaderInfo candidate;
    bool found = IdentifyAt(base, c, candidate);
    if (!found && lib.ok) {
      PreShaderMatch how;
      if (const PreShader* s = lib.library.IdentifyMicrocode(base + c, GuestAvailable(c), vertex,
                                                             &how)) {
        candidate.container_hash = s->container_hash;
        candidate.is_vertex = s->vertex;
        candidate.dynamic_vertex_fetch = s->dynamic_vertex_fetch;
        candidate.match = how;
        candidate.capture = NewCapture(s->container_hash, s->vertex, s->dynamic_vertex_fetch, s->container);
        found = true;
      }
    }
    if (found && candidate.is_vertex == vertex) {
      info = candidate;
      return c;
    }
  }
  return 0;
}

// Shader objects without a creation record (their container reached Direct3D
// by another route): the object's copy as it is at first draw, hashed like the
// corpus. Less stable than the creator hooks (bind-time patches), so the
// corpus is built from the union of several runs' dumps.
uint32_t ResolveRuntimeCopy(uint8_t* base, uint32_t obj, GuestShaderInfo& info) {
  for (uint32_t off : {kVSContainerOffset, kPSContainerOffset}) {
    const uint32_t c = obj + off;
    if (HasContainerMagic(base, c) && TryHashContainer(base, c, info)) return c;
  }
  return 0;
}

void WriteContainer(const std::filesystem::path& dir, const char* name, const uint8_t* data,
                    size_t size) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (std::FILE* f = std::fopen((dir / name).string().c_str(), "wb")) {
    std::fwrite(data, 1, size, f);
    std::fclose(f);
  }
}

const std::string& DumpDir() {
  static const std::string dir = REXCVAR_GET(sr_native_dump_shader_dir);
  return dir;
}

void DumpContainer(uint8_t* base, uint32_t container, const GuestShaderInfo& info) {
  if (DumpDir().empty()) return;
  static std::mutex mutex;
  static std::unordered_set<uint64_t> written;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!written.insert(info.container_hash).second) return;
  }
  // With a library the original bytes are the reference; dumping guest
  // memory would store the patched copy under the original's hash.
  const LoadedLibrary& lib = Library();
  char name[64];
  std::snprintf(name, sizeof(name), "%016llX.%s.bin", (unsigned long long)info.container_hash,
                info.is_vertex ? "vs" : "ps");
  if (lib.ok) {
    if (const PreShader* s = lib.library.Find(info.container_hash, info.is_vertex)) {
      WriteContainer(DumpDir(), name, s->container.data(), s->container.size());
      return;
    }
  }
  ShaderContainerHeader header;
  if (!ParseShaderContainerHeader(base + container, GuestAvailable(container), header)) return;
  WriteContainer(DumpDir(), name, base + container, header.total_size());
}

// Hands the container to the runtime translator (shader_translator.h) when no
// library supplies the shader: the hash is then the creation hash of these
// very bytes, which is what LoadShader will ask for.
void OfferTranslation(uint8_t* base, uint32_t container, const GuestShaderInfo& info) {
  if (const ShaderLibrary* lib = PreShaderLibrary();
      lib && lib->Find(info.container_hash, info.is_vertex)) return;
  ShaderTranslator& translator = ShaderTranslator::Get();
  if (!translator.Enabled()) return;
  static std::mutex mutex;
  static std::unordered_set<uint64_t> offered;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!offered.insert(info.container_hash ^ (info.is_vertex ? 1ull : 0ull)).second) return;
  }
  ShaderContainerHeader header;
  if (!ParseShaderContainerHeader(base + container, GuestAvailable(container), header)) return;
  translator.Offer(info.container_hash, info.is_vertex, base + container, header.total_size());
}

// Unrecognised containers, for comparing against artifacts/shaders/raw
// (tools/shaders/make_preshaders.py --diagnose). Size words may be zero:
// write a bounded window instead.
void DumpUnmatched(uint8_t* base, uint32_t container, const char* tag) {
  if (DumpDir().empty() || !HasContainerMagic(base, container)) return;
  static std::mutex mutex;
  static std::unordered_set<uint32_t> written;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (written.size() > 4096 || !written.insert(container).second) return;
  }
  ShaderContainerHeader header;
  size_t size = 4096;
  if (ParseShaderContainerHeader(base + container, GuestAvailable(container), header)) {
    size = header.total_size();
  }
  size = std::min(size, GuestAvailable(container));
  char name[64];
  std::snprintf(name, sizeof(name), "%s_%08X.bin", tag, container);
  WriteContainer(std::filesystem::path(DumpDir()) / "unmatched", name, base + container, size);
}

// Caller holds g_mutex.
void RegisterLocked(uint32_t obj, const GuestShaderInfo& info, const char* source) {
  g_shaders[obj] = info;
  g_unknown.erase(obj);
  static std::unordered_map<std::string, uint32_t> by_source;
  const uint32_t total = ++g_match_counts[int(info.match) & 3];
  const uint32_t from_source = ++by_source[source];
  if (total <= 4 || (total & 63) == 0) {
    REXLOG_INFO("shader {:08X} = {:016X}.{} ({}, {} match; exact {} body {} microcode {} legacy {}; "
                "{} registered via this source)",
                obj, info.container_hash, info.is_vertex ? "vs" : "ps", source,
                PreShaderMatchName(info.match), g_match_counts[int(PreShaderMatch::kExact)],
                g_match_counts[int(PreShaderMatch::kBody)],
                g_match_counts[int(PreShaderMatch::kMicrocode)],
                g_match_counts[int(PreShaderMatch::kNone)], from_source);
  }
}

// XDK shader object creators (CREATE_SHADER_A = vertex, _B = pixel): r3 is the
// finished container (header with the virtual / physical sizes, then both
// parts), the new shader object comes back in r3 (null on failure). Direct3D
// copies the container into the object and later patches the copy at bind
// time (vertex-fetch variants, PS/VS linking), so this is the only place where
// the bytes are the same on every run: the hash taken here is the corpus key.
void OnCreateShader(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*),
                    const char* role, bool /*vertex_hint*/) {
  const uint32_t container = ctx.r3.u32;
  GuestShaderInfo info;
  const bool known = HasContainerMagic(base, container) && TryHashContainer(base, container, info);
  original(ctx, base);
  const uint32_t shader_obj = ctx.r3.u32;
  NoteHookCall(role, container, shader_obj);
  if (!shader_obj) return;
  if (!known) {
    LogCaptureAnomalyOnce("CreateShader container not recognised", container);
    DumpUnmatched(base, container, "create");
    std::lock_guard<std::mutex> lock(g_mutex);
    // The address may be a reused one: drop any stale association.
    g_shaders.erase(shader_obj);
    g_unknown.erase(shader_obj);
    return;
  }
  DumpContainer(base, container, info);
  OfferTranslation(base, container, info);
  std::lock_guard<std::mutex> lock(g_mutex);
  RegisterLocked(shader_obj, info, role);
}
}  // namespace

const ShaderLibrary* PreShaderLibrary(std::string* where) {
  const LoadedLibrary& lib = Library();
  if (where) *where = lib.where;
  return lib.ok ? &lib.library : nullptr;
}

const GuestShaderInfo* LookupGuestShader(uint32_t guest_object) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_shaders.find(guest_object);
  return it == g_shaders.end() ? nullptr : &it->second;
}

std::shared_ptr<const graphics::guest::ShaderCapture> CaptureGuestShader(uint32_t guest_object) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_shaders.find(guest_object);
  return it == g_shaders.end() ? nullptr : it->second.capture;
}

void TryRegisterInlineShaders(uint8_t* base, uint32_t vs_obj, uint32_t ps_obj) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const auto& [obj, vertex] : {std::pair{vs_obj, true}, std::pair{ps_obj, false}}) {
    if (!GuestRange(obj, kPSContainerOffset + 16)) continue;
    if (g_shaders.count(obj) || g_unknown.count(obj)) continue;
    GuestShaderInfo info;
    if (ResolveInline(base, obj, vertex, info)) {
      RegisterLocked(obj, info, "draw-time");
      continue;
    }
    if (uint32_t c = ResolveRuntimeCopy(base, obj, info)) {
      DumpContainer(base, c, info);
      OfferTranslation(base, c, info);
      RegisterLocked(obj, info, "runtime copy");
      continue;
    }
    g_unknown.insert(obj);
    REXLOG_INFO("shader object {:08X} ({}) not recognised: +{} {:08X} {:08X} {:08X}, +{} {:08X} "
                "{:08X} {:08X}",
                obj, vertex ? "vs" : "ps", kVSContainerOffset,
                LoadBigEndian32(base + obj + kVSContainerOffset),
                LoadBigEndian32(base + obj + kVSContainerOffset + 4),
                LoadBigEndian32(base + obj + kVSContainerOffset + 8), kPSContainerOffset,
                LoadBigEndian32(base + obj + kPSContainerOffset),
                LoadBigEndian32(base + obj + kPSContainerOffset + 4),
                LoadBigEndian32(base + obj + kPSContainerOffset + 8));
    DumpUnmatched(base, obj + kVSContainerOffset, vertex ? "draw_vs40" : "draw_ps40");
    DumpUnmatched(base, obj + kPSContainerOffset, vertex ? "draw_vs872" : "draw_ps872");
  }
}

}  // namespace superman_returns::native

#define SR_SHADER_CREATE_HOOK_(addr, role, vertex)                                          \
  REX_EXTERN(__imp__sub_##addr);                                                            \
  extern "C" REX_FUNC(sub_##addr) {                                                         \
    superman_returns::native::OnCreateShader(                                               \
        ctx, base, [](PPCContext& c, uint8_t* b) { __imp__sub_##addr(c, b); }, role, vertex); \
  }
#define SR_SHADER_CREATE_HOOK(addr, role, vertex) SR_SHADER_CREATE_HOOK_(addr, role, vertex)

// game_profile.h: A = CreateVertexShader, B = CreatePixelShader.
#if SR_HOOK_ENABLED(CREATE_SHADER_A)
SR_SHADER_CREATE_HOOK(SR_ADDR_CREATE_SHADER_A, "CreateShaderA", true)
#endif
#if SR_HOOK_ENABLED(CREATE_SHADER_B)
SR_SHADER_CREATE_HOOK(SR_ADDR_CREATE_SHADER_B, "CreateShaderB", false)
#endif
