// Ported from crazyriddler/rexglue-native-kit @136bc6c4,
// reference/conan/port/src/native/pipeline_cache.cpp (Conan native renderer).
// The kit ships no license file at that revision; parts derived from Xenia /
// ReXGlue keep their BSD license. Changes for Superman Returns are listed in
// docs/native-port-plan.md section 2.
//
#include "pipeline_cache.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <rex/filesystem.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/hash.h>

REXCVAR_DEFINE_BOOL(sr_native_debug_no_embedded_pipelines, false, "Superman Returns Native",
                    "Debug: ignore the embedded pipeline base (worst case for the async path)");

namespace superman_returns::native {

namespace {

constexpr char kMagic[4] = {'C', 'N', 'P', 'S'};
// Bump when PsoRecord or the renderer's pipeline key changes.
constexpr uint32_t kVersion = 1;

struct FileHeader {
  char magic[4];
  uint32_t version;
  uint32_t record_size;
  uint32_t reserved;
};

bool ValidHeader(const FileHeader& h) {
  return std::memcmp(h.magic, kMagic, 4) == 0 && h.version == kVersion &&
         h.record_size == sizeof(PsoRecord);
}

// Records embedded in superman_returns.exe (RCDATA 3): same format as the local file.
void LoadEmbedded(std::vector<PsoRecord>& out) {
  HMODULE module = GetModuleHandleW(nullptr);
  HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(3), MAKEINTRESOURCEW(10));  // RT_RCDATA
  if (!res) return;
  HGLOBAL handle = LoadResource(module, res);
  const uint8_t* data = handle ? static_cast<const uint8_t*>(LockResource(handle)) : nullptr;
  DWORD size = SizeofResource(module, res);
  if (!data || size < sizeof(FileHeader)) return;
  FileHeader h;
  std::memcpy(&h, data, sizeof(h));
  if (!ValidHeader(h)) return;
  size_t count = (size - sizeof(FileHeader)) / sizeof(PsoRecord);
  size_t first = out.size();
  out.resize(first + count);
  std::memcpy(out.data() + first, data + sizeof(FileHeader), count * sizeof(PsoRecord));
}

void LoadFile(const std::filesystem::path& path, std::vector<PsoRecord>& out) {
  std::FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return;
  FileHeader h{};
  if (std::fread(&h, sizeof(h), 1, f) == 1 && ValidHeader(h)) {
    PsoRecord r;
    while (std::fread(&r, sizeof(r), 1, f) == 1) out.push_back(r);
  }
  std::fclose(f);
}

}  // namespace

D3D12_GRAPHICS_PIPELINE_STATE_DESC PipelineCache::MakeDesc(
    const PsoRecord& r, ID3D12RootSignature* root_signature, const void* vs, size_t vs_size,
    const void* ps, size_t ps_size, SemanticName semantic,
    std::vector<D3D12_INPUT_ELEMENT_DESC>& elements) {
  elements.clear();
  for (uint32_t i = 0; i < std::min(r.element_count, PsoRecord::kMaxElements); ++i) {
    const auto& e = r.elements[i];
    D3D12_INPUT_ELEMENT_DESC d{};
    d.SemanticName = semantic(e.usage);
    d.SemanticIndex = e.usage_index;
    d.Format = DXGI_FORMAT(e.format);
    d.InputSlot = e.slot;
    d.AlignedByteOffset = e.offset;
    d.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    elements.push_back(d);
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root_signature;
  desc.VS = {vs, vs_size};
  if (ps) desc.PS = {ps, ps_size};
  desc.InputLayout = {elements.data(), uint32_t(elements.size())};
  desc.BlendState = r.blend;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState = r.raster;
  desc.DepthStencilState = r.depth_stencil;
  desc.DSVFormat = r.dsv_format;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(r.topology_type);
  desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE(r.strip_cut);
  desc.NumRenderTargets = r.num_render_targets;
  for (uint32_t i = 0; i < 8; ++i) desc.RTVFormats[i] = r.rtv_formats[i];
  desc.SampleDesc.Count = r.sample_count;
  return desc;
}

uint64_t PipelineCache::DescHash(const PsoRecord& r) {
  // Field by field: the D3D12 structs contain padding bytes.
  std::vector<uint32_t> v;
  v.reserve(160);
  auto add64 = [&](uint64_t x) {
    v.push_back(uint32_t(x));
    v.push_back(uint32_t(x >> 32));
  };
  add64(r.vs_hash);
  add64(r.ps_hash);
  v.push_back(uint32_t(r.blend.AlphaToCoverageEnable));
  v.push_back(uint32_t(r.blend.IndependentBlendEnable));
  for (const auto& t : r.blend.RenderTarget) {
    v.insert(v.end(), {uint32_t(t.BlendEnable), uint32_t(t.LogicOpEnable), uint32_t(t.SrcBlend),
                       uint32_t(t.DestBlend), uint32_t(t.BlendOp), uint32_t(t.SrcBlendAlpha),
                       uint32_t(t.DestBlendAlpha), uint32_t(t.BlendOpAlpha), uint32_t(t.LogicOp),
                       uint32_t(t.RenderTargetWriteMask)});
  }
  const auto& ra = r.raster;
  uint32_t slope, clamp;
  std::memcpy(&slope, &ra.SlopeScaledDepthBias, 4);
  std::memcpy(&clamp, &ra.DepthBiasClamp, 4);
  v.insert(v.end(), {uint32_t(ra.FillMode), uint32_t(ra.CullMode), uint32_t(ra.FrontCounterClockwise),
                     uint32_t(ra.DepthBias), clamp, slope, uint32_t(ra.DepthClipEnable),
                     uint32_t(ra.MultisampleEnable), uint32_t(ra.AntialiasedLineEnable),
                     uint32_t(ra.ForcedSampleCount), uint32_t(ra.ConservativeRaster)});
  const auto& ds = r.depth_stencil;
  auto face = [&](const D3D12_DEPTH_STENCILOP_DESC& f) {
    v.insert(v.end(), {uint32_t(f.StencilFailOp), uint32_t(f.StencilDepthFailOp),
                       uint32_t(f.StencilPassOp), uint32_t(f.StencilFunc)});
  };
  v.insert(v.end(), {uint32_t(ds.DepthEnable), uint32_t(ds.DepthWriteMask), uint32_t(ds.DepthFunc),
                     uint32_t(ds.StencilEnable), uint32_t(ds.StencilReadMask),
                     uint32_t(ds.StencilWriteMask)});
  face(ds.FrontFace);
  face(ds.BackFace);
  for (DXGI_FORMAT f : r.rtv_formats) v.push_back(uint32_t(f));
  v.insert(v.end(), {uint32_t(r.dsv_format), r.num_render_targets, r.sample_count, r.topology_type,
                     r.strip_cut, r.element_count});
  for (uint32_t i = 0; i < std::min(r.element_count, PsoRecord::kMaxElements); ++i) {
    const auto& e = r.elements[i];
    v.insert(v.end(), {uint32_t(e.usage) | (uint32_t(e.usage_index) << 8) | (uint32_t(e.slot) << 16),
                       e.format, e.offset});
  }
  return XXH3_64bits(v.data(), v.size() * sizeof(uint32_t));
}

void PipelineCache::Start(ID3D12Device* device, ID3D12RootSignature* root_signature,
                          ShaderLookup shaders, SemanticName semantic, uint32_t msaa_samples,
                          bool foliage_a2c) {
  device_ = device;
  root_signature_ = root_signature;
  shaders_ = shaders;
  semantic_ = semantic;
  path_ = rex::filesystem::GetExecutableFolder() / "superman_returns_pipelines.bin";

  std::vector<PsoRecord> records;
  if (!REXCVAR_GET(sr_native_debug_no_embedded_pipelines)) LoadEmbedded(records);
  size_t embedded = records.size();
  LoadFile(path_, records);
  size_t variants = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto enqueue = [&](const PsoRecord& r) {
      uint64_t dh = DescHash(r);
      if (by_desc_.count(dh) || !known_.insert(r.key).second) return false;
      by_desc_[dh] = r.key;
      queue_.push_back(r);
      return true;
    };
    // Settings variants of records captured on multisampled targets: the same
    // draw at the current MSAA sample count, with and without alpha to
    // coverage (foliage antialiasing converts opaque alpha-tested draws). They
    // carry a synthetic key (never produced by the renderer) and are found by
    // description (TakeByDesc). Originals of another sample count are only
    // converted: the renderer cannot request them with these settings.
    std::vector<PsoRecord> generated;
    for (const PsoRecord& r : records) {
      if (r.sample_count <= 1) {
        generated.push_back(r);
        continue;
      }
      PsoRecord v = r;
      v.sample_count = msaa_samples;
      if (msaa_samples <= 1) v.blend.AlphaToCoverageEnable = FALSE;
      bool opaque = r.ps_hash && r.num_render_targets && !r.blend.RenderTarget[0].BlendEnable;
      if (r.sample_count == msaa_samples) generated.push_back(r);
      PsoRecord base_variant = v;
      base_variant.key = DescHash(v) | 1;  // synthetic (odd) key
      generated.push_back(base_variant);
      if (msaa_samples > 1 && opaque) {
        PsoRecord a2c = v;
        a2c.blend.AlphaToCoverageEnable = foliage_a2c ? TRUE : FALSE;
        a2c.key = DescHash(a2c) | 1;
        generated.push_back(a2c);
      }
    }
    std::unordered_set<uint64_t> original;
    for (const PsoRecord& r : records) original.insert(r.key);
    for (const PsoRecord& r : generated) {
      if (enqueue(r) && !original.count(r.key)) ++variants;
    }
  }
  // Start a fresh local file when missing or from another version.
  {
    std::FILE* f = _wfopen(path_.c_str(), L"rb");
    FileHeader h{};
    bool valid = f && std::fread(&h, sizeof(h), 1, f) == 1 && ValidHeader(h);
    if (f) std::fclose(f);
    if (!valid) {
      if (std::FILE* w = _wfopen(path_.c_str(), L"wb")) {
        FileHeader nh{};
        std::memcpy(nh.magic, kMagic, 4);
        nh.version = kVersion;
        nh.record_size = sizeof(PsoRecord);
        std::fwrite(&nh, sizeof(nh), 1, w);
        std::fclose(w);
      }
    }
  }
  REXLOG_INFO("native: pipeline cache: {} pipelines to precompile ({} embedded, {} local, {} "
              "settings variants for {}x MSAA{})",
              queue_.size(), embedded, records.size() - embedded, variants, msaa_samples,
              foliage_a2c ? " + foliage A2C" : "");
  start_time_ = std::chrono::steady_clock::now();
  // Leave cores for the game: a few background threads at low priority.
  unsigned threads = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
  for (unsigned i = 0; i < threads; ++i) threads_.emplace_back([this] { Worker(); });
}

void PipelineCache::StartupProgress(size_t& done, size_t& total) {
  std::lock_guard<std::mutex> lock(mutex_);
  total = queue_.size();
  // Dispatched (or skipped as made by the renderer) and no longer compiling.
  size_t in_flight = 0;
  for (size_t i = 0; i < next_ && i < queue_.size(); ++i) in_flight += pending_.count(queue_[i].key);
  done = std::min(next_, queue_.size()) - in_flight;
}

void PipelineCache::AddStartupThreads(unsigned count) {
  for (unsigned i = 0; i < count; ++i) threads_.emplace_back([this] { Worker(true); });
}

void PipelineCache::Worker(bool startup_only) {
  SetThreadPriority(GetCurrentThread(),
                    startup_only ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_BELOW_NORMAL);
  SetThreadDescription(GetCurrentThread(), L"Native PSO precompile");
  std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
  while (true) {
    PsoRecord r;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      auto skip_done = [&] {
        // Skip what the renderer already created or someone else compiles.
        while (next_ < queue_.size() &&
               (ready_.count(queue_[next_].key) || pending_.count(queue_[next_].key))) {
          ++next_;
        }
      };
      skip_done();
      if (next_ >= queue_.size() && pending_.empty() && compiled_ && !startup_logged_) {
        startup_logged_ = true;
        double wall =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time_).count();
        REXLOG_INFO("native: pipeline cache: {} pipelines precompiled in {:.1f} s ({:.0f} ms of "
                    "driver compile time)",
                    compiled_, wall, compile_ms_);
      }
      // Idle workers stay alive for pipelines requested during play; the extra
      // startup threads leave.
      if (startup_only && next_ >= queue_.size()) break;
      work_cv_.wait(lock, [&] {
        skip_done();
        return stop_ || !urgent_.empty() || next_ < queue_.size();
      });
      if (stop_) break;
      if (!urgent_.empty()) {
        r = urgent_.front();
        urgent_.pop_front();
      } else {
        r = queue_[next_++];
      }
      pending_.insert(r.key);
    }
    const void *vs = nullptr, *ps = nullptr;
    size_t vs_size = 0, ps_size = 0;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    if (shaders_(r.vs_hash, true, &vs, &vs_size) &&
        (!r.ps_hash || shaders_(r.ps_hash, false, &ps, &ps_size))) {
      auto t0 = std::chrono::steady_clock::now();
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc =
          MakeDesc(r, root_signature_, vs, vs_size, ps, ps_size, semantic_, elements);
      if (FAILED(device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)))) pso.Reset();
      double ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      std::lock_guard<std::mutex> lock(mutex_);
      compile_ms_ += ms;
      ++compiled_;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_.erase(r.key);
      if (pso) {
        ready_[r.key] = pso;
      } else if (urgent_keys_.count(r.key)) {
        failed_.insert(r.key);
      }
    }
    cv_.notify_all();
  }
}

void PipelineCache::CompileAsync(const PsoRecord& record) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    known_.insert(record.key);
    if (!urgent_keys_.insert(record.key).second || pending_.count(record.key)) return;
    auto it = ready_.find(record.key);
    if (it != ready_.end() && it->second) return;
    ready_.erase(record.key);  // a Take() miss marker must not hide the result
    urgent_.push_back(record);
  }
  work_cv_.notify_one();
}

PipelineCache::Poll PipelineCache::TryTake(uint64_t key,
                                           Microsoft::WRL::ComPtr<ID3D12PipelineState>& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (failed_.count(key)) return Poll::kFailed;
  auto it = ready_.find(key);
  if (it == ready_.end() || !it->second) return Poll::kPending;
  out = it->second;
  ready_.erase(it);
  return Poll::kReady;
}

bool PipelineCache::Take(uint64_t key, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!known_.count(key)) return false;
  // In flight on a precompile thread: waiting is never slower than compiling
  // the same pipeline again here.
  cv_.wait(lock, [&] { return !pending_.count(key); });
  auto it = ready_.find(key);
  if (it == ready_.end() || !it->second) {
    // Not reached yet: the caller compiles it now; the workers skip it.
    ready_[key] = nullptr;
    return false;
  }
  out = it->second;
  it->second = nullptr;
  return true;
}

bool PipelineCache::TakeByDesc(const PsoRecord& record,
                               Microsoft::WRL::ComPtr<ID3D12PipelineState>& out) {
  uint64_t key;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = by_desc_.find(DescHash(record));
    if (it == by_desc_.end()) return false;
    key = it->second;
  }
  return Take(key, out);
}

void PipelineCache::Record(const PsoRecord& record) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!known_.insert(record.key).second) return;
  }
  if (std::FILE* f = _wfopen(path_.c_str(), L"ab")) {
    std::fwrite(&record, sizeof(record), 1, f);
    std::fclose(f);
  }
}

PipelineCache::~PipelineCache() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  work_cv_.notify_all();
  for (std::thread& t : threads_) {
    if (t.joinable()) t.join();
  }
}

}  // namespace superman_returns::native
