// Ocean waves. The game draws the open water as a disc of 417 vertices around
// the camera (object space [-1, 1], Mobj scales it to ~24600 units, rings ~1200
// units apart): far too coarse to carry waves. Its vertex shader is the same
// one the shore strips use, and in the original it only fed the pixel shader's
// ripples, the geometry stayed flat. Here the open-water draw gets a dense
// disc of rings in the game's own vertex format (same attributes, so the
// unchanged shader code computes the same texture coordinates), and the
// translated vertex shader moves each vertex by the wave model of
// water_waves.h (shader_common.h srWaterDisplace, shared c30.w = height scale).

#include "native_renderer.h"
#include "sdk_compat.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <thread>
#include <cmath>
#include <string>
#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_DOUBLE(sr_water_waves, 0.0, "Superman Returns Native",
                      "Ocean waves: height scale of the wave model on the open water (0 = the game's "
                      "flat water, 1 = waves up to about a metre)")
    .range(0.0, 3.0);
REXCVAR_DEFINE_DOUBLE(sr_water_probe_at_s, 0.0, "Superman Returns Native",
                      "Debug: at this time scan guest memory for the water level and arm sr_water_watch");
REXCVAR_DEFINE_DOUBLE(sr_probe_float, 0.0, "Superman Returns Native",
                      "Debug: with sr_water_probe_at_s, scan for this float instead of the water level");
REXCVAR_DEFINE_DOUBLE(sr_probe_tol, 1e-5, "Superman Returns Native",
                      "Debug: relative tolerance of the sr_probe_float scan");
REXCVAR_DEFINE_DOUBLE(sr_probe_rescan_s, 0.0, "Superman Returns Native",
                      "Debug: at this time re-read every scan hit and log those that changed by over 2%");
REXCVAR_DEFINE_DOUBLE(sr_probe_track_s, 0.0, "Superman Returns Native",
                      "Debug: log the memory around the lone scan hits every 0.2 s for this long");
REXCVAR_DEFINE_STRING(sr_probe_text,"", "Superman Returns Native",
                      "Debug: with sr_water_probe_at_s, dump guest memory around this text");
REXCVAR_DEFINE_STRING(sr_probe_text_dir, "", "Superman Returns Native",
                      "Debug: directory for the sr_probe_text dumps");
REXCVAR_DEFINE_STRING(sr_probe_vec, "", "Superman Returns Native",
                      "Debug: with sr_water_probe_at_s, find 'x y z' as three consecutive big-endian floats "
                      "(within 0.01) and log 64 floats around each hit");
REXCVAR_DEFINE_STRING(sr_probe_write, "", "Superman Returns Native",
                      "Debug: with sr_water_probe_at_s, write big-endian floats into guest memory at that time "
                      "(hex address=value, comma separated)");
REXCVAR_DEFINE_STRING(sr_water_watch, "", "Superman Returns Native",
                      "Debug: guest addresses (hex, comma separated, up to 4) to watch for reads/writes");

namespace superman_returns {
void MotionProbe(uint8_t* base, double now_s);  // water_physics.cpp
}

namespace superman_returns::native {

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint64_t kWaterVs = 0x31FE1648D1734C70ull;   // the water vertex shader (gerstner_wave_amp)
constexpr uint64_t kOceanPs = 0x2EFEE77C92B2F443ull;   // the open-water pixel shader
// The disc: a centre vertex, then rings from 1e-4 of the radius (~2.5 units)
// to the rim, geometrically spaced (~3% of the distance apart, like the
// circumference at 256 segments), so the density on screen stays even.
constexpr uint32_t kSegments = 256;
constexpr uint32_t kRings = 280;
constexpr float kInnerRadius = 1e-4f;

// The game's vertex (stride 32, host order after the byte swap): position
// float3, texture coordinate float2 (u = 5x + 5, v = 5z - 5 in the original
// disc), packed normal (straight up), two words the host layout ignores.
struct OceanVertex {
  float x, y, z;
  float u, v;
  uint32_t normal, w6, w7;
};
static_assert(sizeof(OceanVertex) == 32);

ComPtr<ID3D12Resource> MakeOceanBuffer(ID3D12Device* device, uint64_t size, D3D12_HEAP_TYPE type,
                                       D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = type;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> buffer;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                             IID_PPV_ARGS(&buffer))))
    return nullptr;
  return buffer;
}

}  // namespace

bool Renderer::IsOceanDraw() const {
  return current_vs_hash_ == kWaterVs && current_ps_hash_ == kOceanPs && WaterWaveScale() > 0.0f;
}

float Renderer::WaterWaveScale() const { return float(REXCVAR_GET(sr_water_waves)); }

bool Renderer::DrawOceanMesh() {
  if (ocean_failed_) return false;
  if (!ocean_vb_) {
    std::vector<OceanVertex> vertices;
    vertices.reserve(1 + size_t(kRings) * kSegments);
    auto add = [&](float x, float z) {
      vertices.push_back({x, 0.0f, z, 5.0f * x + 5.0f, 5.0f * z - 5.0f, 0x0007FC00u, 0x000001FFu,
                          0x20100000u});
    };
    add(0.0f, 0.0f);
    for (uint32_t r = 0; r < kRings; ++r) {
      const float radius = kInnerRadius * std::pow(1.0f / kInnerRadius, float(r) / float(kRings - 1));
      for (uint32_t s = 0; s < kSegments; ++s) {
        const float a = 6.2831853f * float(s) / float(kSegments);
        add(radius * std::cos(a), radius * std::sin(a));
      }
    }
    // Winding as the game's disc: counter-clockwise seen from above (+y), the
    // order its strips produce (the PSO culls back faces).
    std::vector<uint32_t> indices;
    indices.reserve(size_t(kSegments) * 3 + size_t(kRings - 1) * kSegments * 6);
    auto ring = [](uint32_t r, uint32_t s) { return 1 + r * kSegments + s % kSegments; };
    for (uint32_t s = 0; s < kSegments; ++s) {
      indices.insert(indices.end(), {0u, ring(0, s + 1), ring(0, s)});
    }
    for (uint32_t r = 0; r + 1 < kRings; ++r) {
      for (uint32_t s = 0; s < kSegments; ++s) {
        const uint32_t a = ring(r, s), b = ring(r, s + 1), c = ring(r + 1, s), d = ring(r + 1, s + 1);
        indices.insert(indices.end(), {a, b, c, b, d, c});
      }
    }
    const uint64_t vb_size = vertices.size() * sizeof(OceanVertex);
    const uint64_t ib_size = indices.size() * sizeof(uint32_t);
    ComPtr<ID3D12Resource> upload = MakeOceanBuffer(device_, vb_size + ib_size, D3D12_HEAP_TYPE_UPLOAD,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ);
    ocean_vb_ = MakeOceanBuffer(device_, vb_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    ocean_ib_ = MakeOceanBuffer(device_, ib_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    void* mapped = nullptr;
    D3D12_RANGE none{0, 0};
    if (!upload || !ocean_vb_ || !ocean_ib_ || FAILED(upload->Map(0, &none, &mapped))) {
      REXLOG_WARN("native: ocean waves: could not create the water mesh buffers");
      ocean_vb_.Reset();
      ocean_ib_.Reset();
      ocean_failed_ = true;
      return false;
    }
    std::memcpy(mapped, vertices.data(), vb_size);
    std::memcpy(static_cast<uint8_t*>(mapped) + vb_size, indices.data(), ib_size);
    upload->Unmap(0, nullptr);
    command_list_->CopyBufferRegion(ocean_vb_.Get(), 0, upload.Get(), 0, vb_size);
    command_list_->CopyBufferRegion(ocean_ib_.Get(), 0, upload.Get(), vb_size, ib_size);
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = ocean_vb_.Get();
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    barriers[1].Transition.pResource = ocean_ib_.Get();
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_INDEX_BUFFER;
    for (auto& b : barriers) {
      b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    command_list_->ResourceBarrier(2, barriers);
    Retire(std::move(upload));
    ocean_index_count_ = uint32_t(indices.size());
    ocean_vb_size_ = uint32_t(vb_size);
    REXLOG_INFO("native: ocean waves: water mesh {} vertices, {} triangles", vertices.size(),
                indices.size() / 3);
  }
  if (SkipGpuDraw()) return true;
  D3D12_VERTEX_BUFFER_VIEW vbv{ocean_vb_->GetGPUVirtualAddress(), ocean_vb_size_, sizeof(OceanVertex)};
  D3D12_INDEX_BUFFER_VIEW ibv{ocean_ib_->GetGPUVirtualAddress(), ocean_index_count_ * 4u,
                              DXGI_FORMAT_R32_UINT};
  command_list_->IASetVertexBuffers(0, 1, &vbv);
  command_list_->IASetIndexBuffer(&ibv);
  command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  command_list_->DrawIndexedInstanced(ocean_index_count_, 1, 0, 0, 0);
  // The next draws of this batch may reuse the game's stream 0.
  command_list_->IASetVertexBuffers(0, 1, &guest_vertex_views_[0]);
  return true;
}

// Physics reverse engineering. sr_water_probe_at_s: once, at that time, scan the
// guest address space for the water level the open-water draw carries (Mobj
// row 1 .w, a big-endian float) and log every address holding it.
// sr_water_watch: up to 4 guest addresses (hex, comma separated) get hardware
// read/write watchpoints on every thread; the host instructions touching them
// are counted and logged (RVA in superman_returns.exe, look up in the .map).
namespace {
constexpr int kWatchSlots = 4;
constexpr int kWatchRips = 256;
struct WatchHit {
  std::atomic<uint64_t> rip{0};
  std::atomic<uint32_t> count{0};
  std::atomic<uint32_t> slot{0};
};
WatchHit g_watch_hits[kWatchRips];
std::atomic<uint32_t> g_watch_total{0};
std::vector<uint32_t> g_track;
std::vector<uint32_t> g_track_all;
std::vector<std::pair<uint32_t, float>> g_scan_hits;  // every hit of the scan (sr_probe_rescan_s)  // every scan hit (up to 64), logged with sr_probe_track_s
double g_track_until = 0.0;

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT* c = info->ContextRecord;
  const uint64_t dr6 = c->Dr6;
  if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
  const uint32_t slot = uint32_t(__builtin_ctzll(dr6 & 0xF));
  const uint64_t rip = c->Rip;
  g_watch_total.fetch_add(1, std::memory_order_relaxed);
  for (uint32_t i = uint32_t(rip >> 2) % kWatchRips, n = 0; n < kWatchRips; ++n, i = (i + 1) % kWatchRips) {
    uint64_t expected = 0;
    if (g_watch_hits[i].rip.load(std::memory_order_relaxed) == rip ||
        g_watch_hits[i].rip.compare_exchange_strong(expected, rip)) {
      g_watch_hits[i].slot.store(slot, std::memory_order_relaxed);
      g_watch_hits[i].count.fetch_add(1, std::memory_order_relaxed);
      break;
    }
  }
  c->Dr6 = 0;
  return EXCEPTION_CONTINUE_EXECUTION;
}

void ArmWatchpoints(const uint64_t* host, int n) {
  AddVectoredExceptionHandler(1, WatchHandler);
  const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 te{sizeof(te)};
  int armed = 0;
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
    HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
    if (!t) continue;
    if (SuspendThread(t) != DWORD(-1)) {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(t, &c)) {
        uint64_t dr7 = c.Dr7;
        for (int i = 0; i < n; ++i) {
          (&c.Dr0)[i] = host[i];
          dr7 |= 1ull << (i * 2);                 // local enable
          dr7 &= ~(0xFull << (16 + i * 4));
          dr7 |= (0x3ull | (0x3ull << 2)) << (16 + i * 4);  // read/write, 4 bytes
        }
        c.Dr7 = dr7;
        if (SetThreadContext(t, &c)) ++armed;
      }
      ResumeThread(t);
    }
    CloseHandle(t);
  }
  CloseHandle(snap);
  REXLOG_INFO("water probe: {} watchpoints armed on {} threads", n, armed);
}
std::atomic<bool> g_external_armed{false};
}  // namespace

// Arms the hardware watchpoints on guest addresses chosen at run time (by a hook that found the
// object it wants watched); the hits are logged by WaterProbe every 5 s.
void ArmGuestWatch(uint8_t* base, const uint32_t* guest, int n) {
  uint64_t host[kWatchSlots];
  n = std::min(n, kWatchSlots);
  for (int i = 0; i < n; ++i) host[i] = reinterpret_cast<uint64_t>(base) + guest[i];
  // From a helper thread: ArmWatchpoints skips the thread it runs on, and the caller (a guest
  // thread) is usually the one to watch.
  std::thread([&] { ArmWatchpoints(host, n); }).join();
  g_external_armed = true;
}

void Renderer::WaterProbe(uint8_t* base) {
  superman_returns::MotionProbe(base, compat::BenchElapsedMs() / 1000.0);
  static const bool enabled = REXCVAR_GET(sr_water_probe_at_s) > 0.0;
  if (!enabled) return;
  static bool scanned = false, armed = false;
  static double last_log_s = 0.0;
  const double now_s = compat::BenchElapsedMs() / 1000.0;
  const double at = REXCVAR_GET(sr_water_probe_at_s);
  static bool first = true;
  if (first) {
    first = false;
    REXLOG_INFO("water probe: first open-water draw at {:.1f} s (probe at {:.1f} s)", now_s, at);
  }
  if (at > 0.0 && !scanned && now_s >= at) {
    scanned = true;
    uint32_t found = 0;
    std::vector<std::pair<uint32_t, float>> hits;
    for (uint64_t a = 0; a < 0x100000000ull;) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) break;
      const uint64_t region_end = uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - base) + mbi.RegionSize;
      const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                                            PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
      if (readable) {
        const uint8_t* p = static_cast<const uint8_t*>(mbi.BaseAddress);
        for (uint64_t o = 0; o + 4 <= mbi.RegionSize; o += 4) {
          uint32_t u;
          std::memcpy(&u, p + o, 4);
          u = __builtin_bswap32(u);
          float f;
          std::memcpy(&f, &u, 4);
          const float want = float(REXCVAR_GET(sr_probe_float));
          if (want != 0.0f ? std::fabs(f - want) < std::fabs(want) * float(REXCVAR_GET(sr_probe_tol))
                           : std::fabs(f) > 9.2915f && std::fabs(f) < 9.2925f) {
            ++found;
            hits.push_back({uint32_t((p + o) - base), f});
          }
        }
      }
      if (region_end <= a) break;
      a = region_end;
    }
    REXLOG_INFO("water probe: {} addresses hold the water level", found);
    {
      std::string list;
      for (size_t i = 0; i < hits.size() && i < 96; ++i) list += fmt::format(" {:08X}", hits[i].first);
      REXLOG_INFO("water probe: hits:{}", list);
    }
    // sr_probe_track_s: log 64 floats from 64 bytes before each lone hit (up
    // to 4, outside the image) every 0.2 s for that many seconds.
    g_track.clear();
    for (size_t i = 0; i < hits.size() && g_track.size() < 4; ++i) {
      const bool lone = (i == 0 || hits[i].first - hits[i - 1].first > 64) &&
                        (i + 1 == hits.size() || hits[i + 1].first - hits[i].first > 64);
      if (lone && hits[i].first < 0x80000000u) g_track.push_back(hits[i].first);
    }
    g_track_until = now_s + REXCVAR_GET(sr_probe_track_s);
    g_scan_hits = hits;
    g_track_all.clear();
    for (size_t i = 0; i < hits.size() && i < 64; ++i)
      if (hits[i].first < 0x80000000u) g_track_all.push_back(hits[i].first);
    // sr_probe_text: dump 32 KB around each occurrence of this text in guest
    // memory to sr_probe_text_dir (decompressed game data, e.g. tuning XML).
    const std::string text = REXCVAR_GET(sr_probe_text);
    const std::string dir = REXCVAR_GET(sr_probe_text_dir);
    if (!text.empty() && !dir.empty()) {
      uint32_t dumps = 0;
      for (uint64_t a = 0; a < 0x100000000ull && dumps < 16;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) break;
        const uint64_t region_end = uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - base) + mbi.RegionSize;
        const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                              (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                                              PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
        if (readable && a < 0x90000000ull) {  // skip the image mirrors and physical views
          const char* p = static_cast<const char*>(mbi.BaseAddress);
          const std::string_view region(p, mbi.RegionSize);
          for (size_t at = region.find(text); at != std::string_view::npos && dumps < 16;
               at = region.find(text, at + 1)) {
            const size_t from = at > 16384 ? at - 16384 : 0;
            const size_t len = std::min<size_t>(32768, mbi.RegionSize - from);
            const std::string path = fmt::format("{}\\text_{:08X}.bin", dir, uint32_t((p + at) - reinterpret_cast<const char*>(base)));
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
              std::fwrite(p + from, 1, len, f);
              std::fclose(f);
            }
            REXLOG_INFO("water probe: text at guest {:08X} -> {}", uint32_t((p + at) - reinterpret_cast<const char*>(base)), path);
            ++dumps;
            at += 32768;
            if (at >= region.size()) break;
          }
        }
        if (region_end <= a) break;
        a = region_end;
      }
    }
    // sr_probe_vec: an object's position as three consecutive floats.
    {
      float want[3];
      std::string vec = REXCVAR_GET(sr_probe_vec);
      std::replace(vec.begin(), vec.end(), ',', ' ');
      if (std::sscanf(vec.c_str(), "%f %f %f", &want[0], &want[1], &want[2]) == 3) {
        auto rd = [&](const uint8_t* q) {
          uint32_t u;
          std::memcpy(&u, q, 4);
          u = __builtin_bswap32(u);
          float f;
          std::memcpy(&f, &u, 4);
          return f;
        };
        uint32_t found_vec = 0;
        for (uint64_t a = 0; a < 0x100000000ull && found_vec < 24;) {
          MEMORY_BASIC_INFORMATION mbi{};
          if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) break;
          const uint64_t region_end = uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - base) + mbi.RegionSize;
          const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                                (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                                                PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY));
          if (readable && a < 0x90000000ull) {
            const uint8_t* p = static_cast<const uint8_t*>(mbi.BaseAddress);
            for (uint64_t o = 0; o + 12 <= mbi.RegionSize && found_vec < 24; o += 4) {
              if (!(std::fabs(rd(p + o) - want[0]) <= 0.01f && std::fabs(rd(p + o + 4) - want[1]) <= 0.01f &&
                    std::fabs(rd(p + o + 8) - want[2]) <= 0.01f))
                continue;  // (NaN never matches)
              ++found_vec;
              std::string s;
              for (int k = -32; k < 32; ++k) {
                const int64_t at = int64_t(o) + k * 4;
                if (at < 0 || uint64_t(at) + 4 > mbi.RegionSize) continue;
                const float f = rd(p + at);
                uint32_t raw;
                std::memcpy(&raw, p + at, 4);
                raw = __builtin_bswap32(raw);
                s += std::fabs(f) < 1e7f && (std::fabs(f) > 1e-6f || f == 0.0f) ? fmt::format(" {:+d}:{:.3f}", k * 4, f)
                                                                                 : fmt::format(" {:+d}:#{:08X}", k * 4, raw);
              }
              REXLOG_INFO("water probe: vec at guest {:08X}:{}", uint32_t((p + o) - base), s);
            }
          }
          if (region_end <= a) break;
          a = region_end;
        }
        REXLOG_INFO("water probe: vec hits {}", found_vec);
      }
    }
    // sr_probe_write: change values to test whether a copy is the live one.
    {
      const std::string w = REXCVAR_GET(sr_probe_write);
      for (size_t at = 0; at < w.size();) {
        size_t end = w.find(',', at);
        if (end == std::string::npos) end = w.size();
        const std::string item = w.substr(at, end - at);
        const size_t eq = item.find('=');
        if (eq != std::string::npos) {
          const uint32_t addr = uint32_t(std::strtoul(item.substr(0, eq).c_str(), nullptr, 16));
          const float value = std::strtof(item.substr(eq + 1).c_str(), nullptr);
          uint32_t u;
          std::memcpy(&u, &value, 4);
          u = __builtin_bswap32(u);
          uint32_t old;
          std::memcpy(&old, base + addr, 4);
          old = __builtin_bswap32(old);
          float oldf;
          std::memcpy(&oldf, &old, 4);
          std::memcpy(base + addr, &u, 4);
          REXLOG_INFO("water probe: wrote {} at guest {:08X} (was {})", value, addr, oldf);
        }
        at = end + 1;
      }
    }
    // Runs (neighbours within 64 bytes) are tables (vertices); a lone value is
    // more likely an object's field.
    for (size_t i = 0; i < hits.size();) {
      size_t j = i + 1;
      while (j < hits.size() && hits[j].first - hits[j - 1].first <= 64) ++j;
      if (j - i == 1)
        REXLOG_INFO("water probe: lone {:.5f} at guest {:08X}", hits[i].second, hits[i].first);
      else
        REXLOG_INFO("water probe: run of {} from {:08X} to {:08X}", j - i, hits[i].first, hits[j - 1].first);
      i = j;
    }
  }
  static bool rescanned = false;
  if (!rescanned && !g_scan_hits.empty() && REXCVAR_GET(sr_probe_rescan_s) > 0.0 &&
      now_s >= REXCVAR_GET(sr_probe_rescan_s)) {
    rescanned = true;
    uint32_t changed = 0;
    std::string s;
    for (const auto& [at, was] : g_scan_hits) {
      uint32_t u;
      std::memcpy(&u, base + at, 4);
      u = __builtin_bswap32(u);
      float f;
      std::memcpy(&f, &u, 4);
      if (std::fabs(f - was) > std::fabs(was) * 0.02f) {
        if (++changed <= 400) s += fmt::format(" {:08X}:{:.4f}->{:.4f}", at, was, f);
      }
    }
    REXLOG_INFO("probe rescan {:.2f} s: {} of {} changed:{}", now_s, changed, g_scan_hits.size(), s);
  }
  // Every hit's value every 0.1 s (which of them moves, and when).
  static double last_all_s = 0.0;
  if (!g_track_all.empty() && now_s < g_track_until && now_s - last_all_s >= 0.1) {
    last_all_s = now_s;
    std::string s;
    for (const uint32_t at : g_track_all) {
      uint32_t u;
      std::memcpy(&u, base + at, 4);
      u = __builtin_bswap32(u);
      float f;
      std::memcpy(&f, &u, 4);
      s += fmt::format(" {:08X}={:.4f}", at, f);
    }
    REXLOG_INFO("probe all {:.2f} s:{}", now_s, s);
  }
  static double last_track_s = 0.0;
  if (!g_track.empty() && now_s < g_track_until && now_s - last_track_s >= 0.2) {
    last_track_s = now_s;
    for (const uint32_t at : g_track) {
      std::string s;
      for (int k = -16; k < 48; ++k) {
        const uint32_t a = at + uint32_t(k * 4);
        uint32_t u;
        std::memcpy(&u, base + a, 4);
        u = __builtin_bswap32(u);
        float f;
        std::memcpy(&f, &u, 4);
        s += fmt::format(" {:.3f}", f);
      }
      REXLOG_INFO("probe track {:.2f} s {:08X}:{}", now_s, at, s);
    }
  }
  const std::string watch = REXCVAR_GET(sr_water_watch);
  if (!watch.empty() && !armed && at > 0.0 && now_s >= at) {
    armed = true;
    uint64_t host[kWatchSlots];
    int n = 0;
    for (size_t pos = 0; pos < watch.size() && n < kWatchSlots;) {
      const size_t comma = watch.find(',', pos);
      const std::string part = watch.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
      host[n++] = reinterpret_cast<uint64_t>(base) + std::stoull(part, nullptr, 16);
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
    ArmWatchpoints(host, n);
    last_log_s = now_s;
  }
  if ((armed || g_external_armed) && now_s - last_log_s >= 5.0) {
    last_log_s = now_s;
    const uint64_t module = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    std::string s;
    for (auto& h : g_watch_hits) {
      const uint64_t rip = h.rip.load();
      if (rip)
        s += fmt::format(" [{}] rva {:X} x{}", h.slot.load(), rip - module, h.count.load());
    }
    REXLOG_INFO("water probe: {} hits:{}", g_watch_total.load(), s);
  }
}

}  // namespace superman_returns::native
