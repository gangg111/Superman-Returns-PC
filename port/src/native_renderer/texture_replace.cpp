#include "texture_replace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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
    for (const auto& e : fs::recursive_directory_iterator(r.dir, ec)) {
      if (!e.is_regular_file()) continue;
      std::string ext = e.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      if (ext == ".dds")
        r.files.insert_or_assign(e.path().stem().string(), e.path());
      else if (ext == ".png")
        r.files.emplace(e.path().stem().string(), e.path());
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
  return true;
}

}  // namespace

bool LoadReplacement(const std::string& name, DecodedTexture& out) {
  std::call_once(g_replace_init, [] { g_replace_enabled = ReplaceFromSwap() == 0; });
  if (!g_replace_enabled) return false;
  const ReplaceIndex& index = Index();
  const auto found = index.files.find(name);
  if (found == index.files.end()) return false;
  const fs::path& path = found->second;
  auto fail = [&](const char* why) {
    REXLOG_WARN("textures: {} not used: {}", path.string(), why);
    return false;
  };
  // A PNG the prefetch thread already decoded; else it is read and decoded here.
  const auto png_log = [&](const DecodedTexture& t, const char* how) {
    static std::mutex m;
    static std::set<std::string> logged;
    std::lock_guard<std::mutex> lock(m);
    if (logged.insert(name).second)
      REXLOG_INFO("textures: {} replaced by {} ({}x{}, {} mips, PNG {})", name, path.filename().string(), t.width,
                  t.height, t.mip_levels, how);
  };
  if (auto png = CachedPng(name)) {
    png_log(*png, "decoded in advance");
    out = *png;
    return true;
  }
  // The file's bytes from the in-memory copy the prefetch thread reads at start
  // (a read on the render worker stalled fast flight), else from disk now.
  std::shared_ptr<const std::vector<uint8_t>> bytes = CachedFile(name);
  if (!bytes) bytes = ReadWhole(path);
  const std::vector<uint8_t>& b = *bytes;
  if (IsPng(b)) {
    DecodedTexture t;
    const char* why = nullptr;
    if (!DecodePng(b, t, why)) return fail(why);
    png_log(t, "decoded on the render thread (not prefetched yet)");
    out = std::move(t);
    return true;
  }
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

}  // namespace superman_returns::native
