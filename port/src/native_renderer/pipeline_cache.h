// Ported from crazyriddler/rexglue-native-kit @136bc6c4,
// reference/conan/port/src/native/pipeline_cache.h (Conan native renderer).
// The kit ships no license file at that revision; parts derived from Xenia /
// ReXGlue keep their BSD license. Changes for Superman Returns are listed in
// docs/native-port-plan.md section 2.
//
// Pipeline (PSO) cache against shader-compilation stutter (EXP-044).
//
// Every PSO the native renderer creates is recorded (its full description,
// keyed by the renderer's pipeline key) into superman_returns_pipelines.bin next to the
// executable. At startup, the records from that file plus a base set embedded
// in superman_returns.exe (RCDATA 3, captured from test playthroughs) are created on
// background threads while the logos/intro play: the driver compiles each
// one once and keeps it in its own shader cache, and the renderer finds it
// ready when the game first needs it.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/ui/d3d12/d3d12_api.h>

namespace superman_returns::native {

// Serializable pipeline description (no pointers).
struct PsoRecord {
  static constexpr uint32_t kMaxElements = 16;
  uint64_t key = 0;
  uint64_t vs_hash = 0, ps_hash = 0;
  D3D12_BLEND_DESC blend{};
  D3D12_RASTERIZER_DESC raster{};
  D3D12_DEPTH_STENCIL_DESC depth_stencil{};
  DXGI_FORMAT rtv_formats[8] = {};
  DXGI_FORMAT dsv_format = DXGI_FORMAT_UNKNOWN;
  uint32_t num_render_targets = 0;
  uint32_t sample_count = 1;
  uint32_t topology_type = 0;
  uint32_t strip_cut = 0;
  uint32_t element_count = 0;
  struct Element {
    uint8_t usage = 0, usage_index = 0, slot = 0, pad = 0;
    uint32_t format = 0;
    uint32_t offset = 0;
  } elements[kMaxElements];
};

class PipelineCache {
 public:
  // Shader bytecode lookup: returns false if the shader is unavailable.
  using ShaderLookup = bool (*)(uint64_t hash, bool vertex, const void** data, size_t* size);
  using SemanticName = const char* (*)(uint32_t usage);

  ~PipelineCache();

  // Loads the embedded base set and the local file, then starts the background
  // precompilation. `root_signature` is the renderer's draw root signature.
  // Settings variants (EXP-052): records captured on multisampled targets are
  // converted to `msaa_samples` (the sample count the renderer will use for
  // them) with alpha to coverage on/off as `foliage_a2c` allows, so pipelines
  // captured under any MSAA/foliage setting serve the current one.
  void Start(ID3D12Device* device, ID3D12RootSignature* root_signature, ShaderLookup shaders,
             SemanticName semantic, uint32_t msaa_samples, bool foliage_a2c);
  // A precompiled (or in-flight) pipeline for `key`: waits for an in-flight
  // one. Returns false if the key is unknown to the cache.
  bool Take(uint64_t key, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out);
  // Same, looked up by the full description (generated variants have no
  // renderer key). `record.key` is ignored.
  bool TakeByDesc(const PsoRecord& record, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out);
  // Content hash of a record's description (every field except the key).
  static uint64_t DescHash(const PsoRecord& r);
  // Records a pipeline the renderer created itself (new to the cache).
  void Record(const PsoRecord& record);
  // Asynchronous compilation during play (EXP-053): queues `record` ahead of
  // the startup list; poll with TryTake. The record should be Record()ed too.
  void CompileAsync(const PsoRecord& record);
  enum class Poll { kPending, kReady, kFailed };
  // Non-blocking: kReady moves the pipeline into `out`.
  Poll TryTake(uint64_t key, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out);
  // Startup precompilation: pipelines finished (compiled, failed or already
  // made by the renderer) of `total`. Done when they are equal.
  void StartupProgress(size_t& done, size_t& total);
  // `count` more threads at normal priority that leave once the startup list
  // is dispatched (the first start's progress screen).
  void AddStartupThreads(unsigned count);

  // Builds the D3D12 description of a record (element storage in `elements`).
  static D3D12_GRAPHICS_PIPELINE_STATE_DESC MakeDesc(
      const PsoRecord& r, ID3D12RootSignature* root_signature, const void* vs, size_t vs_size,
      const void* ps, size_t ps_size, SemanticName semantic,
      std::vector<D3D12_INPUT_ELEMENT_DESC>& elements);

 private:
  void Worker(bool startup_only = false);

  ID3D12Device* device_ = nullptr;
  ID3D12RootSignature* root_signature_ = nullptr;
  ShaderLookup shaders_ = nullptr;
  SemanticName semantic_ = nullptr;
  std::filesystem::path path_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<PsoRecord> queue_;  // to precompile, in first-seen order
  size_t next_ = 0;
  std::unordered_set<uint64_t> known_;    // keys in the local file or embedded set
  std::unordered_map<uint64_t, uint64_t> by_desc_;  // DescHash -> key of a queued record
  std::unordered_set<uint64_t> pending_;  // being compiled right now
  std::deque<PsoRecord> urgent_;          // requested during play (CompileAsync)
  std::unordered_set<uint64_t> urgent_keys_, failed_;
  std::condition_variable work_cv_;
  bool startup_logged_ = false;
  std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> ready_;
  std::vector<std::thread> threads_;
  std::atomic<bool> stop_{false};
  uint64_t compiled_ = 0;
  double compile_ms_ = 0.0;
  std::chrono::steady_clock::time_point start_time_{};
};

}  // namespace superman_returns::native
