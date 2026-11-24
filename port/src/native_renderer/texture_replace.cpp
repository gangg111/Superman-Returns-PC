#include "texture_replace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(sr_texture_dump_dir, "", "Superman Returns Native",
                      "Write every game texture once as <hash>_<w>x<h>_f<format>.dds + .json here")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(sr_texture_replace_dir, "", "Superman Returns Native",
                      "Replacement textures (<name>.dds or <name>.png); empty = <exe dir>/textures if present, "
                      "\"off\" = none")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(sr_texture_replace_from_swap, 0, "Superman Returns Native",
                     "A/B: start texture replacement at this guest swap (0 = from the start)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(sr_custom_costume, 0, "Superman Returns Native",
                     "Custom costume worn: 1..3 = the costumes screen's slots 4..6 (<exe>/textures/Suits/Slot4..6); 0 = the game's costume");

namespace superman_returns::native {

namespace {

namespace fs = std::filesystem;

struct ReplaceIndex {
  fs::path dir;
  std::unordered_map<std::string, fs::path> files;  // name -> file
};

// One read for the whole file (a per-byte stream iterator took seconds per
// texture and stalled the frame).
std::shared_ptr<const std::vector<uint8_t>> ReadWhole(const fs::path& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  auto b = std::make_shared<std::vector<uint8_t>>(f ? size_t(f.tellg()) : 0);
  if (!b->empty()) {
    f.seekg(0);
    f.read(reinterpret_cast<char*>(b->data()), std::streamsize(b->size()));
  }
  return b;
}

std::mutex g_file_cache_mutex;
std::unordered_map<std::string, std::shared_ptr<const std::vector<uint8_t>>> g_file_cache;

std::shared_ptr<const std::vector<uint8_t>> CachedFile(const std::string& name) {
  std::lock_guard lock(g_file_cache_mutex);
  const auto it = g_file_cache.find(name);
  return it == g_file_cache.end() ? nullptr : it->second;
}

// PNG replacements decoded by the prefetch thread (a decode took 19 ms for
// 1024x1024 on the render worker: a stall in flight, 2026-11-11).
std::unordered_map<std::string, std::shared_ptr<const DecodedTexture>> g_png_cache;

std::shared_ptr<const DecodedTexture> CachedPng(const std::string& name) {
  std::lock_guard lock(g_file_cache_mutex);
  const auto it = g_png_cache.find(name);
  return it == g_png_cache.end() ? nullptr : it->second;
}

bool IsPng(const std::vector<uint8_t>& b) {
  static const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  return b.size() >= 8 && std::memcmp(b.data(), kPng, 8) == 0;
}

bool DecodePng(const std::vector<uint8_t>& b, DecodedTexture& t, const char*& why);
void BuildRgbaMips(std::vector<uint8_t> level, uint32_t width, uint32_t height, DecodedTexture& t);
bool ApplyGameAlpha(DecodedTexture& t, const GameAlpha& g, const char*& why);

// <name>.rgb.png: the player's colour with the game texture's alpha (issue #2).
bool IsRgbOnly(const fs::path& path) {
  const std::string stem = path.stem().string();
  return stem.size() > 4 && _stricmp(stem.c_str() + stem.size() - 4, ".rgb") == 0;
}

// The texture name a file stands for: its stem, without ".rgb" for <name>.rgb.png.
std::string ReplacementKey(const fs::path& path) {
  const std::string stem = path.stem().string();
  return IsRgbOnly(path) ? stem.substr(0, stem.size() - 4) : stem;
}

const ReplaceIndex& Index() {
  static const ReplaceIndex index = [] {
    ReplaceIndex r;
    std::string dir = REXCVAR_GET(sr_texture_replace_dir);
    if (dir == "off") return r;
    if (dir.empty()) {
      wchar_t exe[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, exe, MAX_PATH);
      r.dir = fs::path(exe).parent_path() / "textures";
    } else {
      r.dir = fs::u8path(dir);
    }
    std::error_code ec;
    if (!fs::is_directory(r.dir, ec)) {
      r.dir.clear();
      return r;
    }
    // A .dds or a .png (the players' request 2026-11-11: an edited PNG under the
    // dumped name); with both, the DDS.
    for (auto it = fs::recursive_directory_iterator(r.dir, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      const auto& e = *it;
      // the custom costumes' files (textures/Suits/Slot<n>) count only while worn
      if (it.depth() == 0 && e.is_directory() && _wcsicmp(e.path().filename().c_str(), L"Suits") == 0) {
        it.disable_recursion_pending();
        continue;
      }
      if (!e.is_regular_file()) continue;
      std::string ext = e.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      if (ext == ".dds")
        r.files.insert_or_assign(e.path().stem().string(), e.path());
      else if (ext == ".png")
        r.files.emplace(ReplacementKey(e.path()), e.path());  // <name>.png or <name>.rgb.png
    }
    REXLOG_INFO("textures: {} replacement(s) in {}", r.files.size(), r.dir.string());
    // All of them into memory on a background thread (about 100 MB), so the
    // renderer never waits for the disk when a new building or texture appears.
    std::thread([files = r.files] {
      SetThreadDescription(GetCurrentThread(), L"Texture replacement prefetch");
      uint64_t total = 0;
      static thread_local const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
      (void)com;
      for (const auto& [stem, path] : files) {
        auto data = ReadWhole(path);
        total += data->size();
        if (IsPng(*data)) {
          // decoded here, ready for the renderer (the bytes are not kept)
          auto decoded = std::make_shared<DecodedTexture>();
          const char* why = nullptr;
          if (DecodePng(*data, *decoded, why)) {
            std::lock_guard lock(g_file_cache_mutex);
            g_png_cache.emplace(stem, std::move(decoded));
            continue;
          }
        }
        std::lock_guard lock(g_file_cache_mutex);
        g_file_cache.emplace(stem, std::move(data));
      }
      REXLOG_INFO("textures: {} replacement file(s) prefetched, {} MB", files.size(), total >> 20);
    }).detach();
    return r;
  }();
  return index;
}

// Bytes per 4x4 block (block compressed) or per pixel; 0 = not handled.
uint32_t FormatBytes(DXGI_FORMAT f, bool& block) {
  block = IsBlockCompressed(f);
  switch (f) {
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
      return 8;
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
      return 16;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
      return 8;
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_B5G6R5_UNORM:
      return 2;
    case DXGI_FORMAT_R8_UNORM:
      return 1;
    default:
      return 0;
  }
}

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 |
         uint32_t(uint8_t(d)) << 24;
}

DXGI_FORMAT LegacyFormat(const uint8_t* pf) {
  uint32_t flags, fourcc, bits, rmask;
  std::memcpy(&flags, pf + 4, 4);
  std::memcpy(&fourcc, pf + 8, 4);
  std::memcpy(&bits, pf + 12, 4);
  std::memcpy(&rmask, pf + 16, 4);
  if (flags & 0x4) {  // DDPF_FOURCC
    switch (fourcc) {
      case FourCC('D', 'X', 'T', '1'): return DXGI_FORMAT_BC1_UNORM;
      case FourCC('D', 'X', 'T', '3'): return DXGI_FORMAT_BC2_UNORM;
      case FourCC('D', 'X', 'T', '5'): return DXGI_FORMAT_BC3_UNORM;
      case FourCC('A', 'T', 'I', '2'): case FourCC('B', 'C', '5', 'U'): return DXGI_FORMAT_BC5_UNORM;
      case FourCC('A', 'T', 'I', '1'): case FourCC('B', 'C', '4', 'U'): return DXGI_FORMAT_BC4_UNORM;
      default: return DXGI_FORMAT_UNKNOWN;
    }
  }
  if (bits == 32) return rmask == 0xFF ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
  return DXGI_FORMAT_UNKNOWN;
}

}  // namespace

namespace {
std::atomic<bool> g_replace_enabled{true};
std::once_flag g_replace_init;
}  // namespace

uint64_t ReplaceFromSwap() {
  return uint64_t(std::max(0, REXCVAR_GET(sr_texture_replace_from_swap)));
}

void SetReplacementsEnabled(bool enabled) {
  std::call_once(g_replace_init, [] {});  // the initial state must not override this
  g_replace_enabled = enabled;
}

namespace {

// The player's files in a custom costume folder and the game textures each one
// stands for, by the first 16 digits of the name (the content hash). Measured
// by painting them (memory: kostium-i-peleryna-tekstury): the suit's body; the
// briefs and boots; the cape's outside, three byte-identical copies of the
// briefs' texture under other hashes (the scene decides which one is drawn).
struct CostumeFile {
  const char* file;
  std::vector<std::string> hashes;
  // The slots' own pictures on the costumes screen: hashes[n - 1] is slot n's,
  // replaced by that slot's file whichever slot is worn. Measured by the
  // renderer's dump on the screen (2026-11-16); they are the textures
  // tools/ui/build_ui_mod.py costume_picture_entries makes, and change with them
  // (tests/ui test_costume_slots holds their bytes' SHA-256).
  bool picture;
};
const std::vector<CostumeFile>& CostumeFiles() {
  static const std::vector<CostumeFile> files = {
      {"body.png", {"4817CA35041A47A3"}, false},
      {"briefs_boots.png", {"BABD6E1873DAE219"}, false},
      {"cape.png", {"1DDFD929C1386395", "37723AEE4F1D9AE1", "156E060938442B71"}, false},
      // The body's normal map (Xenos DXN, issue #7): the S and the belt are relief in it.
      {"body_normal.png", {"2CE8A7839B12F6D5"}, false},
      {"thumb.png", {"D597E26BDE5373A9", "A3EC202389B3DF1A", "ACF323EDE2C52B6B"}, true},
      {"fullsize.png", {"ECDE511A52384ADF", "0FAEA40EE38407CC", "F1B7D42AE0D96D82"}, true},
  };
  return files;
}

// <exe>/textures/Suits/Slot<4..6>: the screen's slots 4 to 6 (the user's names,
// 2026-11-16). Suits is left out of the textures folder's own index.
fs::path CostumeDir(int slot) {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return fs::path(exe).parent_path() / "textures" / "Suits" / ("Slot" + std::to_string(slot + 3));
}

std::atomic<uint32_t> g_costume_generation{0};
std::atomic<int> g_cape_hidden_slot{-1};  // the worn slot when its costume.toml says cape = false

// The value of `key = ...` in the slot's costume.toml (after '=', trimmed), empty when missing.
std::string CostumeSetting(int slot, const char* key) {
  const size_t n = std::strlen(key);
  std::ifstream in(CostumeDir(slot) / "costume.toml");
  for (std::string line; std::getline(in, line);) {
    const size_t at = line.find_first_not_of(" \t");
    if (at == std::string::npos || line.compare(at, n, key) != 0) continue;
    const size_t eq = line.find('=', at + n);
    if (eq == std::string::npos || line.find_first_not_of(" \t", at + n) != eq) continue;
    const std::string v = line.substr(eq + 1);
    const size_t a = v.find_first_not_of(" \t\r"), b = v.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
  }
  return {};
}

// The inside of the worn slot's cape, 0xRRGGBB, or -1: the game's own.
std::atomic<int64_t> g_cape_inside{-1};

// "#RRGGBB" (quotes and # optional) as 0xRRGGBB, or -1.
int64_t ParseRgb(std::string v) {
  std::erase(v, '"');
  if (!v.empty() && v[0] == '#') v.erase(0, 1);
  if (v.size() != 6 || v.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return -1;
  return std::stoll(v, nullptr, 16);
}

// cape = false in the slot's costume.toml: the slot's costume without the cape;
// cape_inside = "#RRGGBB": the colour of the cape's inside.
void NoteCapeSetting(int slot) {
  const bool hidden = slot > 0 && CostumeSetting(slot, "cape") == "false";
  g_cape_hidden_slot.store(hidden ? slot : -1);
  if (hidden) REXLOG_INFO("costumes: slot {} has no cape (cape = false)", slot + 3);
  const std::string inside = slot > 0 ? CostumeSetting(slot, "cape_inside") : std::string();
  const int64_t rgb = inside.empty() ? -1 : ParseRgb(inside);
  g_cape_inside.store(rgb);
  if (rgb >= 0) REXLOG_INFO("costumes: slot {} cape inside #{:06X}", slot + 3, uint32_t(rgb));
  else if (!inside.empty()) REXLOG_WARN("costumes: slot {} cape_inside = {} is not \"#RRGGBB\"", slot + 3, inside);
}

// A name starting with a game texture's content hash, as the textures folder
// names them (2CE8A7839B12F6D5_512x512_f49.png): 16 hex digits, then the end or '_'.
bool HashNamed(const std::string& stem) {
  if (stem.size() < 16 || (stem.size() > 16 && stem[16] != '_')) return false;
  return std::all_of(stem.begin(), stem.begin() + 16, [](char c) { return std::isxdigit(uint8_t(c)) != 0; });
}

// Each slot's files named by a texture's hash (upper case), PNG or DDS: any of the
// costume's textures, its normal maps included (issue #7), not only the named
// files above. Listed again when another slot is worn (the files are read then).
std::mutex g_slot_files_mutex;
std::array<std::unordered_map<std::string, fs::path>, kCustomCostumes + 1> g_slot_files;
uint32_t g_slot_files_generation = ~0u;

void ListSlotFilesLocked() {
  const uint32_t generation = g_costume_generation.load();
  if (generation == g_slot_files_generation) return;
  g_slot_files_generation = generation;
  for (int slot = 1; slot <= kCustomCostumes; ++slot) {
    auto& files = g_slot_files[slot];
    files.clear();
    std::error_code ec;
    for (fs::directory_iterator it(CostumeDir(slot), ec), end; !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      std::string ext = it->path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
      const std::string stem = it->path().stem().string();
      if ((ext != ".png" && ext != ".dds") || !HashNamed(stem)) continue;
      std::string hash = stem.substr(0, 16);
      std::transform(hash.begin(), hash.end(), hash.begin(), [](char c) { return char(std::toupper(uint8_t(c))); });
      // Two files for one texture: the DDS, as in the textures folder, else the first
      // name in sort order; never the listing's order, which the file system decides.
      auto [at, fresh] = files.emplace(hash, it->path());
      if (!fresh) {
        const bool old_dds = at->second.extension() == ".dds" || at->second.extension() == ".DDS";
        const bool dds = ext == ".dds";
        const bool wins = dds != old_dds ? dds : it->path().filename() < at->second.filename();
        const fs::path loser = wins ? at->second : it->path();
        if (wins) at->second = it->path();
        REXLOG_WARN("costumes: slot {} has two files for {}, {} not used", slot + 3, hash, loser.string());
      }
    }
    if (!files.empty()) REXLOG_INFO("costumes: slot {}: {} file(s) named by texture hash", slot + 3, files.size());
  }
}

// The player's file standing for texture `name`, if the worn slot has one.
bool CostumePath(const std::string& name, fs::path& out) {
  const std::string hash = name.substr(0, 16);
  for (const auto& f : CostumeFiles()) {
    const auto at = std::find(f.hashes.begin(), f.hashes.end(), hash);
    if (at == f.hashes.end()) continue;
    const int slot = f.picture ? int(at - f.hashes.begin()) + 1 : CustomCostume();
    if (slot <= 0) return false;
    std::error_code ec;
    const fs::path p = CostumeDir(slot) / f.file;
    if (!fs::is_regular_file(p, ec)) {
      if (f.picture) return false;  // the screen's pictures: only the slots' own files
      break;                        // a file named by the hash may stand for it
    }
    out = p;
    return true;
  }
  const int slot = CustomCostume();
  if (slot <= 0) return false;
  std::lock_guard lock(g_slot_files_mutex);
  ListSlotFilesLocked();
  const auto it = g_slot_files[slot].find(hash);
  if (it == g_slot_files[slot].end()) return false;
  out = it->second;
  return true;
}

}  // namespace

int CustomCostume() {
  const int slot = std::clamp(REXCVAR_GET(sr_custom_costume), 0, kCustomCostumes);
  // the slot worn from the toml at start: its cape setting read once
  static std::once_flag read;
  std::call_once(read, [slot] { NoteCapeSetting(slot); });
  return slot;
}

void SetCustomCostume(int slot) {
  slot = std::clamp(slot, 0, kCustomCostumes);
  if (slot == CustomCostume()) return;
  REXCVAR_SET(sr_custom_costume, slot);
  NoteCapeSetting(slot);
  g_costume_generation.fetch_add(1);
  REXLOG_INFO("costumes: custom slot {} worn ({})", slot,
              slot ? CostumeDir(slot).string() : std::string("the game's costume"));
}

uint32_t CustomCostumeGeneration() { return g_costume_generation.load(); }

bool IsCostumeTexture(const std::string& name) {
  const std::string hash = name.substr(0, 16);
  for (const auto& f : CostumeFiles())
    if (!f.picture && std::find(f.hashes.begin(), f.hashes.end(), hash) != f.hashes.end()) return true;
  // A texture any slot has a hash-named file for: recreated on every change, so
  // taking that slot off gives the game's texture back.
  std::lock_guard lock(g_slot_files_mutex);
  ListSlotFilesLocked();
  for (int slot = 1; slot <= kCustomCostumes; ++slot)
    if (g_slot_files[slot].count(hash)) return true;
  return false;
}

bool CostumeCapeShown(int slot) {
  return slot < 1 || slot > kCustomCostumes || CostumeSetting(slot, "cape") != "false";
}

void SetCostumeCapeShown(int slot, bool shown) {
  if (slot < 1 || slot > kCustomCostumes) return;
  const fs::path path = CostumeDir(slot) / "costume.toml";
  std::vector<std::string> lines;
  {
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
  }
  const std::string value = std::string("cape = ") + (shown ? "true" : "false");
  bool found = false;
  for (auto& line : lines) {
    const size_t at = line.find_first_not_of(" \t");
    if (at == std::string::npos || line.compare(at, 4, "cape") != 0) continue;
    const size_t eq = line.find_first_not_of(" \t", at + 4);
    if (eq == std::string::npos || line[eq] != '=') continue;
    line = value;
    found = true;
  }
  if (!found) {
    lines.push_back("# false: this costume without the cape.");
    lines.push_back(value);
  }
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  for (const auto& line : lines) out << line << "\r\n";
  if (!out) REXLOG_WARN("costumes: cannot write {}", path.string());
  REXLOG_INFO("costumes: slot {} cape {}", slot + 3, shown ? "shown" : "hidden");
  if (slot == CustomCostume()) NoteCapeSetting(slot);
}

bool CostumeCapeHidden() {
  const int slot = CustomCostume();
  return slot > 0 && g_cape_hidden_slot.load() == slot;
}

bool CostumeCapeInside(float rgb[3]) {
  if (CustomCostume() <= 0) return false;
  const int64_t v = g_cape_inside.load();
  if (v < 0) return false;
  for (int i = 0; i < 3; ++i) rgb[i] = float((v >> (16 - 8 * i)) & 0xFF) / 255.0f;
  return true;
}

std::string CustomCostumeName(int slot) {
  std::string name;
  const std::string v = CostumeSetting(slot, "name");
  const size_t open = v.find('"'), close = v.rfind('"');
  if (open != std::string::npos && close > open)
    for (char c : v.substr(open + 1, close - open - 1))
      if (c >= 32 && c < 127 && name.size() < 24) name += c;
  return name.find_first_not_of(' ') == std::string::npos ? "SLOT " + std::to_string(slot + 3) : name;
}

bool IsBlockCompressed(DXGI_FORMAT f) {
  return (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) ||
         (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB);
}

std::string TextureName(uint64_t content_hash, const DecodedTexture& d) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%016llX_%ux%u_f%u", (unsigned long long)content_hash, d.width,
                d.height, d.guest_format);
  return buf;
}

void DumpTextureOnce(const std::string& name, const DecodedTexture& d, bool gamma) {
  static const std::string dir = REXCVAR_GET(sr_texture_dump_dir);
  if (dir.empty() || d.dimension != 1 || d.depth != 1) return;
  static std::mutex mutex;
  static std::unordered_set<std::string> done;
  // New content at a guest address within 2 s of the previous one: a dynamic
  // texture (movie planes, CPU-written tables), dumped once, then skipped.
  // Slower changes are memory reused by a new level and are dumped.
  using Clock = std::chrono::steady_clock;
  struct Seen {
    std::string name;
    Clock::time_point at;
  };
  static std::unordered_map<uint32_t, Seen> by_address;
  static std::unordered_set<uint32_t> dynamic;
  {
    std::lock_guard lock(mutex);
    if (dynamic.count(d.base_address)) return;
    const auto now = Clock::now();
    auto [it, fresh] = by_address.try_emplace(d.base_address, Seen{name, now});
    if (!fresh && it->second.name != name) {
      if (now - it->second.at < std::chrono::seconds(2)) {
        dynamic.insert(d.base_address);
        REXLOG_INFO("textures: {:08X} is dynamic, no further dumps", d.base_address);
        return;
      }
      it->second = {name, now};
    }
    if (!done.insert(name).second) return;
    if (done.size() == 1) {
      std::error_code ec;
      fs::create_directories(fs::u8path(dir), ec);
    }
  }
  const std::string base = dir + "/" + name;
  if (!WriteDds(d, (base + ".dds").c_str())) {
    REXLOG_WARN("textures: cannot write {}.dds", base);
    return;
  }
  std::ofstream json(base + ".json");
  json << "{\"name\": \"" << name << "\", \"width\": " << d.width << ", \"height\": " << d.height
       << ", \"mips\": " << d.mip_levels << ", \"dxgi_format\": " << int(d.format)
       << ", \"guest_format\": " << d.guest_format << ", \"gamma\": " << (gamma ? "true" : "false")
       << ", \"component_mapping\": " << d.component_mapping << "}\n";
}

namespace {

// A PNG (any WIC can read) as RGBA8 with every mip level, each a 2x2 box of
// the one above (the last row or column kept for an odd size). Empty `why` on
// success.
bool DecodePng(const std::vector<uint8_t>& b, DecodedTexture& t, const char*& why) {
  static thread_local const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || true;
  (void)com;
  using Microsoft::WRL::ComPtr;
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> rgba;
  UINT width = 0, height = 0;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
      FAILED(factory->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(b.data()), DWORD(b.size()))) ||
      FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&rgba)) ||
      FAILED(rgba->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom)) ||
      FAILED(rgba->GetSize(&width, &height)) || !width || !height) {
    why = "not a readable PNG";
    return false;
  }
  if (width > 16384 || height > 16384) {
    why = "larger than 16384";
    return false;
  }
  std::vector<uint8_t> level(size_t(width) * height * 4);
  if (FAILED(rgba->CopyPixels(nullptr, width * 4, UINT(level.size()), level.data()))) {
    why = "PNG pixels unreadable";
    return false;
  }
  BuildRgbaMips(std::move(level), width, height, t);
  return true;
}

// `level` (RGBA8, width x height) and every mip below it into `t`, each a 2x2 box
// of the one above (the last row or column kept for an odd size).
void BuildRgbaMips(std::vector<uint8_t> level, uint32_t width, uint32_t height, DecodedTexture& t) {
  t = {};
  t.width = width;
  t.height = height;
  t.format = DXGI_FORMAT_R8G8B8A8_UNORM;
  t.dimension = 1;
  uint32_t w = width, h = height;
  for (;;) {
    t.levels.push_back({w, h, w * 4, h, t.data.size()});
    t.data.insert(t.data.end(), level.begin(), level.end());
    if (w == 1 && h == 1) break;
    const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
    std::vector<uint8_t> next(size_t(nw) * nh * 4);
    for (uint32_t y = 0; y < nh; ++y)
      for (uint32_t x = 0; x < nw; ++x)
        for (uint32_t c = 0; c < 4; ++c) {
          const uint32_t x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
          const uint32_t y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
          const uint32_t sum = level[(size_t(y0) * w + x0) * 4 + c] + level[(size_t(y0) * w + x1) * 4 + c] +
                               level[(size_t(y1) * w + x0) * 4 + c] + level[(size_t(y1) * w + x1) * 4 + c];
          next[(size_t(y) * nw + x) * 4 + c] = uint8_t((sum + 2) / 4);
        }
    level.swap(next);
    w = nw;
    h = nh;
  }
  t.mip_levels = uint32_t(t.levels.size());
}

// The game texture's alpha as it reaches the shader, one byte per pixel.
bool GameAlphaPlane(const GameAlpha& g, std::vector<uint8_t>& a, const char*& why) {
  const uint32_t w = g.width, h = g.height;
  if (!w || !h) {
    why = "the game texture has no size";
    return false;
  }
  a.assign(size_t(w) * h, 255);
  if (g.select == 4 || g.select == 5) {
    if (g.select == 4) std::fill(a.begin(), a.end(), uint8_t(0));
    return true;
  }
  if (g.select > 3 || !g.data) {
    why = "the game texture's alpha is not readable";
    return false;
  }
  const auto u16 = [](const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; };
  switch (g.format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: {
      const bool bgra = g.format == DXGI_FORMAT_B8G8R8A8_UNORM || g.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
      const uint32_t byte = bgra ? std::array<uint32_t, 4>{2, 1, 0, 3}[g.select] : g.select;
      if (g.row_pitch < w * 4 || g.size < size_t(g.row_pitch) * (h - 1) + w * 4) break;
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) a[size_t(y) * w + x] = g.data[size_t(y) * g.row_pitch + x * 4 + byte];
      return true;
    }
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: {
      if (g.select != 3) {
        why = "the game texture's alpha comes from a colour channel";
        return false;
      }
      const bool bc1 = g.format == DXGI_FORMAT_BC1_UNORM || g.format == DXGI_FORMAT_BC1_UNORM_SRGB;
      const bool bc2 = g.format == DXGI_FORMAT_BC2_UNORM || g.format == DXGI_FORMAT_BC2_UNORM_SRGB;
      const uint32_t block = bc1 ? 8 : 16, bw = (w + 3) / 4, bh = (h + 3) / 4;
      if (g.row_pitch < bw * block || g.size < size_t(g.row_pitch) * (bh - 1) + bw * block) break;
      for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
          const uint8_t* b = g.data + size_t(by) * g.row_pitch + size_t(bx) * block;
          uint8_t px[16];
          if (bc1) {
            // index 3 is transparent when colour 0 <= colour 1
            const bool punch = u16(b) <= u16(b + 2);
            const uint32_t idx = u16(b + 4) | u16(b + 6) << 16;
            for (int i = 0; i < 16; ++i) px[i] = punch && ((idx >> (2 * i)) & 3) == 3 ? 0 : 255;
          } else if (bc2) {
            for (int i = 0; i < 16; ++i) px[i] = uint8_t(((u16(b + 2 * (i / 4)) >> (4 * (i % 4))) & 0xF) * 17);
          } else {
            const uint32_t a0 = b[0], a1 = b[1];
            uint8_t pal[8] = {uint8_t(a0), uint8_t(a1)};
            if (a0 > a1)
              for (int k = 1; k < 7; ++k) pal[k + 1] = uint8_t(((7 - k) * a0 + k * a1 + 3) / 7);
            else {
              for (int k = 1; k < 5; ++k) pal[k + 1] = uint8_t(((5 - k) * a0 + k * a1 + 2) / 5);
              pal[6] = 0;
              pal[7] = 255;
            }
            uint64_t bits = 0;
            for (int k = 0; k < 6; ++k) bits |= uint64_t(b[2 + k]) << (8 * k);
            for (int i = 0; i < 16; ++i) px[i] = pal[(bits >> (3 * i)) & 7];
          }
          for (int i = 0; i < 16; ++i) {
            const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
            if (x < w && y < h) a[size_t(y) * w + x] = px[i];
          }
        }
      return true;
    }
    default:
      why = "the game texture's format has no alpha this reads (BC1, BC2, BC3, RGBA8)";
      return false;
  }
  why = "the game texture's data is shorter than its size";
  return false;
}

// `t` (an RGBA8 PNG, all mips) with the game's alpha in its base level, resampled
// to the PNG's size (bilinear, texel centres), and its mips made again.
bool ApplyGameAlpha(DecodedTexture& t, const GameAlpha& g, const char*& why) {
  std::vector<uint8_t> a;
  if (!GameAlphaPlane(g, a, why)) return false;
  const uint32_t w = t.width, h = t.height;
  std::vector<uint8_t> level(t.data.begin() + t.levels[0].offset,
                             t.data.begin() + t.levels[0].offset + size_t(w) * h * 4);
  const auto at = [&](int x, int y) {
    x = std::clamp(x, 0, int(g.width) - 1);
    y = std::clamp(y, 0, int(g.height) - 1);
    return float(a[size_t(y) * g.width + x]);
  };
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const float u = (x + 0.5f) * g.width / w - 0.5f, v = (y + 0.5f) * g.height / h - 0.5f;
      const int x0 = int(std::floor(u)), y0 = int(std::floor(v));
      const float fu = u - x0, fv = v - y0;
      const float value = (at(x0, y0) * (1 - fu) + at(x0 + 1, y0) * fu) * (1 - fv) +
                          (at(x0, y0 + 1) * (1 - fu) + at(x0 + 1, y0 + 1) * fu) * fv;
      level[(size_t(y) * w + x) * 4 + 3] = uint8_t(std::clamp(value + 0.5f, 0.0f, 255.0f));
    }
  BuildRgbaMips(std::move(level), w, h, t);
  return true;
}

}  // namespace

bool LoadReplacement(const std::string& name, DecodedTexture& out, const GameAlpha* game) {
  std::call_once(g_replace_init, [] { g_replace_enabled = ReplaceFromSwap() == 0; });
  if (!g_replace_enabled) return false;
  // The worn custom costume's file first; it is read now (the prefetched copies
  // are the textures folder's, under the same names).
  fs::path costume;
  const bool from_costume = CostumePath(name, costume);
  const ReplaceIndex& index = Index();
  const auto found = from_costume ? index.files.end() : index.files.find(name);
  if (!from_costume && found == index.files.end()) return false;
  const fs::path& path = from_costume ? costume : found->second;
  auto fail = [&](const char* why) {
    REXLOG_WARN("textures: {} not used: {}", path.string(), why);
    return false;
  };
  // A PNG the prefetch thread already decoded; else it is read and decoded here.
  const auto png_log = [&](const DecodedTexture& t, const char* how) {
    static std::mutex m;
    static std::set<std::string> logged;
    std::lock_guard<std::mutex> lock(m);
    if (logged.insert(path.string() + "|" + name).second)
      REXLOG_INFO("textures: {} replaced by {} ({}x{}, {} mips, PNG {})", name, path.string(), t.width,
                  t.height, t.mip_levels, how);
  };
  // A decoded PNG into `out`; a <name>.rgb.png first gets the game texture's alpha.
  const bool rgb_only = IsRgbOnly(path);
  const auto use_png = [&](DecodedTexture t, const char* how) {
    if (rgb_only) {
      const char* why = nullptr;
      if (!game) return fail("a .rgb.png needs the game's texture (not one of the port's own)");
      if (!ApplyGameAlpha(t, *game, why)) return fail(why);
    }
    png_log(t, rgb_only ? "with the game texture's alpha" : how);
    out = std::move(t);
    return true;
  };
  std::shared_ptr<const std::vector<uint8_t>> bytes;
  if (from_costume) {
    // A slot's PNG here; a slot's DDS (hash-named files) goes on as the textures folder's.
    bytes = ReadWhole(path);
    if (IsPng(*bytes)) {
      DecodedTexture t;
      const char* why = nullptr;
      if (!DecodePng(*bytes, t, why)) return fail(why);
      return use_png(std::move(t), "of the custom costume");
    }
  } else {
    if (auto png = CachedPng(name)) return use_png(*png, "decoded in advance");
    // The file's bytes from the in-memory copy the prefetch thread reads at start
    // (a read on the render worker stalled fast flight), else from disk now.
    bytes = CachedFile(name);
    if (!bytes) bytes = ReadWhole(path);
  }
  const std::vector<uint8_t>& b = *bytes;
  if (IsPng(b)) {
    DecodedTexture t;
    const char* why = nullptr;
    if (!DecodePng(b, t, why)) return fail(why);
    return use_png(std::move(t), "decoded on the render thread (not prefetched yet)");
  }
  if (rgb_only) return fail("a .rgb file must be a PNG");
  if (b.size() < 128 || std::memcmp(b.data(), "DDS ", 4) != 0) return fail("not a DDS or PNG file");
  uint32_t h[31];
  std::memcpy(h, b.data() + 4, sizeof(h));
  const uint32_t height = h[2], width = h[3], mips = std::max<uint32_t>(1, h[6]);
  size_t offset = 128;
  DXGI_FORMAT format;
  uint32_t fourcc;
  std::memcpy(&fourcc, b.data() + 4 + 72 + 8, 4);
  if ((h[19] & 0x4) && fourcc == FourCC('D', 'X', '1', '0')) {
    if (b.size() < 148) return fail("truncated DX10 header");
    uint32_t dx10[5];
    std::memcpy(dx10, b.data() + 128, sizeof(dx10));
    if (dx10[1] != 3 || dx10[3] > 1) return fail("only single 2D textures");
    format = DXGI_FORMAT(dx10[0]);
    offset = 148;
  } else {
    format = LegacyFormat(b.data() + 4 + 72);
  }
  bool block = false;
  const uint32_t unit = FormatBytes(format, block);
  if (!unit) return fail("unsupported DXGI format");
  DecodedTexture t;
  t.width = width;
  t.height = height;
  t.mip_levels = mips;
  t.format = format;
  t.dimension = 1;
  uint32_t w = width, hh = height;
  size_t pos = 0;
  for (uint32_t m = 0; m < mips; ++m) {
    const uint32_t bw = block ? std::max(1u, (w + 3) / 4) : w;
    const uint32_t bh = block ? std::max(1u, (hh + 3) / 4) : hh;
    t.levels.push_back({w, hh, bw * unit, bh, pos});
    pos += size_t(bw) * unit * bh;
    w = std::max(1u, w / 2);
    hh = std::max(1u, hh / 2);
  }
  if (b.size() < offset + pos) return fail("truncated data");
  t.data.assign(b.begin() + offset, b.begin() + offset + pos);
  {
    // Proof that a replacement is in use: the first upload of each name.
    static std::mutex m;
    static std::set<std::string> logged;
    std::lock_guard<std::mutex> lock(m);
    if (logged.insert(name).second)
      REXLOG_INFO("textures: {} replaced by {} ({}x{}, {} mips, DXGI format {})", name,
                  path.filename().string(), width, height, mips, uint32_t(format));
  }
  out = std::move(t);
  return true;
}

namespace {
// A replacement's DXGI format as the Vulkan renderer's linear format, kUnknown
// when it has none (the game's texture is kept and that is logged).
graphics::guest::LinearFormat LinearFromDxgi(DXGI_FORMAT f) {
  using L = graphics::guest::LinearFormat;
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return L::kRGBA8Unorm;
    // BGRA DDS files: the same bytes read as RGBA, red and blue swapped in the view.
    case DXGI_FORMAT_B8G8R8A8_UNORM: return L::kRGBA8Unorm;
    case DXGI_FORMAT_R8_UNORM: return L::kR8Unorm;
    case DXGI_FORMAT_R8G8_UNORM: return L::kRG8Unorm;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return L::kRGB10A2Unorm;
    case DXGI_FORMAT_BC1_UNORM: return L::kBC1;
    case DXGI_FORMAT_BC2_UNORM: return L::kBC2;
    case DXGI_FORMAT_BC3_UNORM: return L::kBC3;
    case DXGI_FORMAT_BC4_UNORM: return L::kBC4;
    case DXGI_FORMAT_BC5_UNORM: return L::kBC5;
    case DXGI_FORMAT_BC7_UNORM: return L::kBC7;
    case DXGI_FORMAT_R16_UNORM: return L::kR16Unorm;
    case DXGI_FORMAT_R16G16_UNORM: return L::kRG16Unorm;
    case DXGI_FORMAT_R16G16B16A16_UNORM: return L::kRGBA16Unorm;
    case DXGI_FORMAT_R16_FLOAT: return L::kR16Float;
    case DXGI_FORMAT_R16G16_FLOAT: return L::kRG16Float;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return L::kRGBA16Float;
    case DXGI_FORMAT_R32_FLOAT: return L::kR32Float;
    case DXGI_FORMAT_R32G32_FLOAT: return L::kRG32Float;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return L::kRGBA32Float;
    default: return L::kUnknown;
  }
}
}  // namespace

bool ReplaceCapturedTexture(const graphics::guest::TextureCapture& capture,
                            graphics::guest::LinearTexture& game, bool& costume) {
  using L = graphics::guest::LinearFormat;
  costume = false;
  if (game.dimension != 1 || game.depth != 1) return false;  // as the D3D12 renderer: 2D only
  // The A/B from a swap on is the D3D12 renderer's (it turns replacement on at that swap).
  static const bool ab_warned = [] {
    if (ReplaceFromSwap()) REXLOG_WARN("textures: sr_texture_replace_from_swap is D3D12 only; Vulkan uses no replacements");
    return true;
  }();
  (void)ab_warned;
  // The name D3D12's CreateTexture gives it: the same ranges in the same order,
  // read from the capture (cached view, as the capture stored them).
  uint64_t hash = 0;
  for (const auto& range : capture.ranges) {
    const auto bytes = capture.memory.Read(0xa0000000u + range.address, range.length);
    if (bytes.size() < range.length) return false;
    hash = HashTextureBytes(bytes.data(), range.length, hash);
  }
  DecodedTexture named;
  named.width = game.width;
  named.height = game.height;
  named.guest_format = game.guest_format;
  const std::string name = TextureName(hash, named);
  costume = IsCostumeTexture(name);
  // The game's base level, for a <name>.rgb.png (the Vulkan formats with alpha).
  GameAlpha alpha;
  switch (game.format) {
    case L::kBC1: alpha.format = DXGI_FORMAT_BC1_UNORM; break;
    case L::kBC2: alpha.format = DXGI_FORMAT_BC2_UNORM; break;
    case L::kBC3: alpha.format = DXGI_FORMAT_BC3_UNORM; break;
    case L::kRGBA8Unorm: alpha.format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    default: break;
  }
  if (!game.levels.empty() && game.levels[0].offset <= game.data.size()) {
    alpha.width = game.levels[0].width;
    alpha.height = game.levels[0].height;
    alpha.row_pitch = game.levels[0].row_pitch;
    alpha.data = game.data.data() + game.levels[0].offset;
    alpha.size = game.data.size() - game.levels[0].offset;
  }
  alpha.select = game.swizzle[3];
  DecodedTexture r;
  if (!LoadReplacement(name, r, &alpha)) return false;
  if (r.dimension != 1 || r.depth != 1 || r.levels.empty()) return false;
  const L format = LinearFromDxgi(r.format);
  if (format == L::kUnknown) {
    static std::mutex m;
    static std::set<std::string> logged;
    std::lock_guard lock(m);
    if (logged.insert(name).second)
      REXLOG_WARN("textures: {} replacement has DXGI format {}, which Vulkan does not take; game texture kept",
                  name, uint32_t(r.format));
    return false;
  }
  // The D3D12 rule (native_renderer.cpp CreateTexture): identity mapping, but a
  // two-channel game texture keeps its own unless the replacement has one channel.
  const bool two_channel = game.format == L::kBC5 || game.format == L::kRG8Unorm ||
                           game.format == L::kRG16Unorm || game.format == L::kRG16Float;
  const bool one_channel = format == L::kBC4 || format == L::kR8Unorm || format == L::kR16Unorm ||
                           format == L::kR16Float || format == L::kR32Float;
  graphics::guest::LinearTexture out;
  out.width = r.width;
  out.height = r.height;
  out.depth = 1;
  out.dimension = 1;
  out.mip_levels = r.mip_levels;
  out.format = format;
  // The file's own channel order (BGRA read as RGBA), then the game's mapping on
  // top where it applies: game component s of the view is the file's base[s].
  const std::array<uint32_t, 4> base = r.format == DXGI_FORMAT_B8G8R8A8_UNORM
                                           ? std::array<uint32_t, 4>{2, 1, 0, 3}
                                           : std::array<uint32_t, 4>{0, 1, 2, 3};
  out.swizzle = base;
  if (two_channel && !one_channel)
    for (int i = 0; i < 4; ++i) out.swizzle[i] = game.swizzle[i] < 4 ? base[game.swizzle[i]] : game.swizzle[i];
  out.guest_format = game.guest_format;
  out.base_address = game.base_address;
  out.base_size = game.base_size;
  for (const auto& l : r.levels) out.levels.push_back({l.width, l.height, l.row_pitch, l.rows, l.offset});
  out.data = std::move(r.data);
  {
    static std::mutex m;
    static std::set<std::string> logged;
    std::lock_guard lock(m);
    if (logged.insert(name).second)
      REXLOG_INFO("textures: {} replaced in Vulkan ({}x{} -> {}x{}, {} mips)", name, game.width, game.height,
                  out.width, out.height, out.mip_levels);
  }
  game = std::move(out);
  return true;
}

}  // namespace superman_returns::native
