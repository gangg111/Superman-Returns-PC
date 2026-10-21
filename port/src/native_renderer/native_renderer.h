// Ported from crazyriddler/rexglue-native-kit @136bc6c4,
// reference/conan/port/src/native/native_renderer.h (Conan native renderer).
// The kit ships no license file at that revision; parts derived from Xenia /
// ReXGlue keep their BSD license. Changes for Superman Returns are listed in
// docs/native-port-plan.md section 2.
//
// Native (non-Xenos) renderer - see docs/native-port-plan.md.
//
// Runs on the guest render thread, driven by hooks on the game's XDK D3D
// entry points. Uses the SDK's D3D12 device and direct queue (owned by the
// presenter's provider) and hands each finished frame to the presenter via
// Presenter::RefreshGuestOutput. The guest GPU contract (fences, interrupts,
// vblank) is served by NativeGraphicsSystem, or by the Xenos backend in
// sr_native_ab_mode (then the native output is only dumped, not presented).
//
// Binding model = XenosRecomp/UnleashedRecomp convention (shader_common.h):
//   b0/b1/b2 space4: VS float constants (256 float4), PS float constants,
//                    shared constants (bindless texture/sampler indices per
//                    fetch slot, bool file, loop constants, half-pixel offset)
//   t0 space0/1/2:   bindless Texture2D / Texture3D / TextureCube heap
//   s0 space3:       bindless sampler heap
//
// Frame model (docs/RENDERER_ANALYSIS.md section 9): guest render-target
// surfaces map to host render targets at full (untiled) size - predicated
// tiling is ignored, every guest draw is issued once. Resolves copy host RT
// regions into host textures registered by the destination's guest base
// address; binding a texture at such an address uses the host copy.
#pragma once
#include "native_frontend.h"
#include "../graphics/guest/capture_batch_pool.h"
#include "buffer_content.h"
#include "shared_constants_cache.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>

#include "game_profile.h"
#include "../graphics/guest/captured_batch.h"
#include "../graphics/guest/texture_capture.h"
#include "../graphics/guest/render_packet.h"
#include "pipeline_cache.h"
#include "pm4_mirror.h"

namespace rex::system {
class IGraphicsSystem;
}
#include <string>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/ui/d3d12/d3d12_api.h>

namespace rex::ui::d3d12 {
class D3D12Presenter;
class D3D12Provider;
}  // namespace rex::ui::d3d12

namespace superman_returns::native {

bool Enabled();

// Render pass currently executing (index into the pass-name table in
// d3d_capture.cpp); maintained by the pass hooks.
extern int g_current_pass;
// Pass as seen by the guest threads (set by the pass hooks). With the
// recording worker, g_current_pass is the pass of the command being executed.
extern int g_guest_pass;
// Pass roles from game_profile.h (Conan: 4, 12, 16, 21, 25, 27). -1 = the
// game's pass is unknown and the features keyed on it stay off.
constexpr int kPassRenderShadowMaps = profile::kPassShadowMaps;
constexpr int kPassRenderOpaque = profile::kPassOpaque;
constexpr int kPassEndTiling = profile::kPassEndTiling;
constexpr int kPassRenderSorted = profile::kPassSorted;
constexpr int kPassUpscale = profile::kPassUpscale;
constexpr int kPassRenderHud = profile::kPassHud;

// Last guest D3DDevice pointer seen in a hooked D3D call's r3 (used when the
// profile has no device global).
void NoteGuestDevice(uint32_t dev);
void FlushResourceUnlockAudit(uint64_t frame);
void ObserveTextureValidationForUnlockAudit(const uint32_t* fetch,
    std::span<const graphics::guest::TextureRange> ranges);

// Records an unsupported case once per (category, key) in the log; returns
// true the first time. The renderer keeps the kit's policy for the draw
// itself (counted skip); see docs/native-port-plan.md section 5.
bool LogUnsupportedOnce(const char* category, uint64_t key, const char* detail = nullptr);

// Offline shader corpus present: the embedded pack (RCDATA 2) or .dxil files
// in sr_native_shader_dir (default <repo>/artifacts/shaders/dxil). `where`
// describes the source that was checked.
bool ShaderCorpusAvailable(std::string* where);

// Guest memory captured with a worker command: `length` bytes of guest virtual
// address `address`, stored at `offset` in the batch's byte arena.
// Allocator whose value-initialization is a no-op: resize() of the capture
// byte arena must not zero memory that is overwritten right away (memcpy).
using graphics::guest::DefaultInitAllocator;
using graphics::guest::CaptureRange;

// Inverse of a 4x4 matrix (row-major, 16 floats); false if singular.
bool Invert4x4(const float m[16], float out[16]);

// Guest pixel coordinate -> host pixel coordinate of a surface rendered at
// `scale` (possibly fractional), rounded the same way everywhere.
inline uint32_t HostPx(uint32_t v, float scale) {
  return uint32_t(std::lround(double(v) * double(scale)));
}

// DLSS: the objects' motion target, bound as RT slot 4 for the scene's draws
// (rt_count 5).
constexpr DXGI_FORMAT kDlssMotionFormat = DXGI_FORMAT_R16G16_FLOAT;

struct DlssState;  // dlss.cpp
struct DlssDeleter {
  void operator()(DlssState* state) const;  // dlss.cpp
};

class Renderer : public NativeFrontend {
 public:
  static Renderer& Get();
  using PacketSink=std::function<bool(graphics::guest::RenderPacket&&,std::string&)>;
  // Installed once before guest execution. The sink owns the native API path;
  // every dependency is captured and no D3D executor is called for this mode.
  bool InstallPacketSink(PacketSink,std::function<void()> cancel);
  void ShutdownWorker();
  ~Renderer();

  // --- Guest D3D entry points -------------------------------------------
  void OnSwap(uint8_t* base, uint32_t front_buffer_texture, uint64_t swap_number = 0);
  // BeginVertices/EndVertices or DrawVerticesUP (vertex data at `data`).
  void DrawInlineVertices(uint8_t* base, uint32_t prim, uint32_t data, uint32_t vertex_count,
                          uint32_t stride);
  void DrawVertices(uint8_t* base, uint32_t prim, uint32_t start_vertex, uint32_t vertex_count);
  void DrawIndexedVertices(uint8_t* base, uint32_t prim, int32_t base_vertex,
                           uint32_t start_index, uint32_t index_count);
  // Resolve(dev, Flags, pSrcRect, pDestTexture, pDestPoint, Level, Slice,
  //         pClearColor, ClearZ, ClearStencil, pParameters)
  void Resolve(uint8_t* base, uint32_t flags, uint32_t src_rect, uint32_t dest_texture,
               uint32_t dest_point, uint32_t clear_color, float clear_z, uint32_t clear_stencil,
               uint32_t level=0,uint32_t slice=0);
  // BeginTiling(dev, Flags, Count, pTileRects, pClearColor, ClearZ, ClearStencil)
  void BeginTiling(uint8_t* base, uint32_t count, uint32_t rects, uint32_t clear_color,
                   float clear_z, uint32_t clear_stencil);
  void EndTiling();
  // Shared XDK clear entry; color is linear float4 (including HDR values).
  void Clear(uint8_t* base, uint32_t count, uint32_t rects, uint32_t flags, const float color[4],
             float z, uint32_t stencil);
  // GpuBeginShaderConstantF4 handed the game a ring pointer for `count`
  // float4 constants: the game writes the ring copy (not the device shadow).
  // Mirrored back into the shadow before the next draw.
  void NoteRingConstants(bool pixel, uint32_t start, uint32_t count, uint32_t ring_ptr);
  // PM4 mirror: parse the XDK command segment written since the last sync.
  void SyncRing(uint8_t* base, uint32_t dev);
  // A/B mode: after the Xenos CP processed guest swap N, dump the Xenos
  // results of this frame's resolves (read back into guest memory).
  void OnGpuSwap(uint64_t swap_number, const uint32_t* regs, uint32_t count);
  // After a command segment switch: restart parsing at the new write pointer.
  void ResyncRing(uint8_t* base, uint32_t dev);
  // Shader literal constants loaded by LOAD_ALU_CONSTANT at shader bind.
  void ApplyLoadAluConstants(uint8_t* base, uint32_t dev, uint32_t table, uint32_t data);
  // Pass hook bracket end (debug: optional mid-frame dump).
  void OnPassEnd(int pass);
  // Guest buffer contents changed (VB/IB Unlock).
  void InvalidateGuestRange(uint32_t address, uint32_t size);

  struct Stats {
    uint64_t packets_decoded = 0, packet_errors = 0, packet_swaps = 0;
    uint64_t packet_texture_refs = 0, packet_texture_errors = 0, packet_shader_missing = 0;
    uint64_t draws = 0;
    uint64_t draws_skipped = 0;
    uint64_t skip_shader = 0, skip_pso = 0, skip_prim = 0, skip_rt = 0, skip_vb = 0;
    // Draws skipped for want of a pipeline, per (vs, ps) pair and render pass.
    std::map<std::tuple<uint64_t, uint64_t, int>, uint64_t> skip_pso_pairs;
    uint64_t buffer_watch_dirty = 0;
    uint64_t buffer_hash_dirty = 0;
    uint64_t edram_reinterprets = 0;
    uint64_t shader_pending_draws = 0;
    uint64_t resolves = 0;
    uint64_t resolve_textures_created = 0;
    uint64_t pso_created = 0;
    double pso_create_ms = 0.0;
    uint64_t pso_precompiled_hits = 0;
    uint64_t pso_async_requests = 0, pso_async_skipped_draws = 0;
    uint64_t a2c_draws[32] = {}, alpha_test_draws[32] = {};
    uint64_t textures_created = 0;
    uint64_t textures_reloaded = 0;
    uint64_t edram_alias_clears = 0;
    uint64_t buffer_partial_uploads = 0;
    uint64_t gpu_wait_us = 0;
    uint64_t buffer_uploads = 0, buffer_upload_bytes = 0, invalidations = 0, invalidated_bytes = 0;
    // Per pass (index = g_current_pass): drawn / skipped, and last skip reason.
    uint64_t pass_draws[32] = {};
    uint64_t pass_skips[32] = {};
    const char* pass_last_skip[32] = {};
  };
  const Stats& stats() const { return stats_; }

 private:
  // --- Recording worker (sr_native_worker) ---------------------------------
  // The guest threads only capture what a command reads from guest memory
  // (PM4 segment words, device/object state, dirty buffer ranges, inline
  // vertices) and queue it; a worker thread records the D3D12 commands.
  using BufferPlan = graphics::guest::BufferPlan;
  using StreamPlan = graphics::guest::StreamPlan;
  using Op = graphics::guest::Op;
  using WorkCmd = graphics::guest::WorkCmd;
  using WorkBatch = graphics::guest::WorkBatch;
  // Front end (guest threads, front_mutex_).
  void BeginCmd(Op op);
  void EndCmd(uint8_t* base);
  void CaptureTextures(uint8_t* base);
  void CaptureBytes(uint8_t* base, uint32_t address, uint32_t length);
  void CaptureDevice(uint8_t* base, uint32_t dev);
  void CaptureRing(uint8_t* base, uint32_t dev);
  struct VertexRange;
  BufferPlan PlanBuffer(uint8_t* base, uint32_t address, uint32_t size, uint32_t decl,
                        uint32_t stride, uint32_t format, uint32_t phase, uint32_t need_begin,
                        uint32_t need_end, bool& ok, uint32_t reset_index = UINT32_MAX,
                        VertexRange* lazy_range = nullptr, uint32_t lazy_offset = 0);
  bool PlanStreams(uint8_t* base, uint32_t dev, uint32_t decl, VertexRange* range);
  void FlushBatch();
  void WaitWorkerIdle(uint64_t batches);
  bool worker_mode_ = false;
  bool worker_mode_checked_ = false;
  PacketSink packet_sink_;std::function<void()> cancel_packet_sink_;
  bool packet_sink_failed_=false;
  std::atomic<bool> worker_stop_{false};
  std::mutex front_mutex_;
  // SR_FRAME_TIMELINE (graphics/frame_timeline.h). Game thread: timeline_game_exit_ns_
  // (under front_mutex_). Worker thread: the three worker fields.
  uint64_t timeline_game_exit_ns_ = 0;
  uint64_t timeline_worker_begin_ns_ = 0, timeline_worker_exec_ns_ = 0, timeline_worker_blocked_ns_ = 0;
  std::unique_ptr<WorkBatch> batch_;
  WorkCmd cur_;
  uint8_t* guest_base_ = nullptr;
  uint64_t exec_copies_before_ = 0;
  std::atomic<uint64_t> front_invalidations_{0}, front_invalidated_bytes_{0};
  // Guest-side buffer contents tracking (decides what each draw uploads).
  struct TrackedBuffer {
    uint32_t address = 0, size = 0;
    bool dirty = false;
    std::vector<std::pair<uint32_t, uint32_t>> clean;  // [begin, end) while dirty
    uint64_t invalidation_stamp = 0;
    uint32_t watch_seq = 0, checked_seq = 0;  // physical write watch
    bool watched = false;
    BufferContent content;
  };
  static constexpr uint32_t kHashedBufferMax = 32 * 1024;
  uint64_t front_frame_ = 0;  // guest swaps seen by the front end
  // Node-based: TrackedBuffer pointers stay valid (page index below).
  std::unordered_map<uint64_t, TrackedBuffer> tracked_;
  uint64_t invalidation_stamp_ = 0;
  struct FrontStreamCache {
    uint32_t address = 0, size = 0, decl = 0, stride = 0, format = 0, phase = 0;
    uint64_t key = 0;
    TrackedBuffer* tracked = nullptr;
  } front_stream_cache_[17];
  // Worker.
  void WorkerMain();
  void Execute(uint8_t* base, const WorkBatch& batch, const WorkCmd& cmd,graphics::guest::DrawPacketOwner storage={},std::shared_ptr<const WorkBatch> capture_owner={});
  std::thread worker_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_, done_cv_;
  std::deque<std::unique_ptr<WorkBatch>> work_queue_;
  std::shared_ptr<graphics::guest::CaptureBatchPool> batch_pool_=std::make_shared<graphics::guest::CaptureBatchPool>();
  uint64_t batches_submitted_ = 0, batches_done_ = 0;
  uint64_t prev_swap_batches_ = 0;  // batches submitted up to the previous swap
  // Worker-side executors of the guest entry points.
  void ExecDrawVertices(uint8_t* base, const WorkBatch& batch, const WorkCmd& cmd);
  void ExecDrawIndexedVertices(uint8_t* base, const WorkBatch& batch, const WorkCmd& cmd);
  void ExecDrawInlineVertices(uint8_t* base, uint32_t prim, uint32_t data, uint32_t vertex_count,
                              uint32_t stride);
  void ExecResolve(uint8_t* base, uint32_t flags, uint32_t src_rect, uint32_t dest_texture,
                   uint32_t dest_point, uint32_t clear_color, float clear_z,
                   uint32_t clear_stencil);
  void ExecBeginTiling(uint8_t* base, uint32_t count, uint32_t rects, uint32_t clear_color,
                       float clear_z, uint32_t clear_stencil);
  void ExecEndTiling();
  void ExecClear(uint8_t* base, uint32_t count, uint32_t rects, uint32_t flags, const float color[4],
                 float z, uint32_t stencil);
  void ExecOnSwap(uint8_t* base, uint32_t front_buffer_texture, uint64_t swap_number);
  void ExecOnPassEnd(int pass);

  bool EnsureInitialized();
  // GPU time per native frame (timestamp queries), logged every 5 s.
  void BeginFrameTimestamp();
  void EndFrameTimestamp();
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> ts_heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> ts_readback_;
  const uint64_t* ts_cpu_ = nullptr;
  uint64_t ts_frequency_ = 0;
  bool ts_pending_[3] = {};
  uint64_t ts_frame_[3] = {};  // swap number of the frame measured in each slot (SR_FRAME_TIMELINE)
  uint64_t ts_prev_end_ = 0;  // end timestamp of the previously harvested frame (gpu_real / gpu_idle)
  bool ts_have_prev_ = false;
  bool ts_open_ = false;
  double ts_accum_ms_ = 0.0;
  // Per-pass split (sr_native_gpu_pass_timing): timestamps at pass changes.
  static constexpr uint32_t kTsPerFrame = 64;
  uint32_t ts_count_[3] = {};                  // timestamps written per frame slot
  uint8_t ts_pass_[3][kTsPerFrame] = {};       // pass of the segment starting at i
  int ts_last_pass_ = -1;
  double ts_pass_ms_[32] = {};
  void PassTimestamp(int pass);
  // Per-command GPU profile of one frame (sr_native_profile_frame_at_s).
  static constexpr uint32_t kProfMax = 16384;
  struct ProfMark {
    uint8_t op;
    bool drew;
    uint64_t vs, ps;
    std::string rt;
  };
  bool SkipGpuDraw() const;
  int32_t RtDebugMode() const;
  std::vector<int32_t> RtDebugModes() const;  // sr_rt_debug_modes  // sr_rt_shadow_debug, or this frame's step of sr_rt_shadow_debug_cycle
  uint64_t rt_measure_base_ = 0;  // first swap of the sr_rt_measure_at_s dumps
  float rt_frame_rows_[12] = {};  // light rows U, V, Z the frame's world reconstruction inverts
  bool ReadbackSubresource(
      ID3D12Resource* resource, D3D12_RESOURCE_STATES* state, uint32_t subresource,
      const std::function<void(const uint8_t*, const D3D12_RESOURCE_DESC&, uint32_t, uint32_t)>& use);
  void RecordFrame();  // sr_record_path
  std::FILE* record_pipe_ = nullptr;
  void* record_process_ = nullptr;  // ffmpeg process handle
  bool record_failed_ = false;
  std::array<uint32_t, 2> record_size_ = {};
  uint64_t record_frames_ = 0;
  bool shader_pending_ = false;  // LoadShader: translation still running
  void ProfileBegin();
  void ProfileMark(Op op, bool drew);
  void ProfileEnd();
  void ProfileReport();
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> prof_heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> prof_readback_;
  std::vector<ProfMark> prof_marks_;
  bool prof_active_ = false, prof_done_ = false;
  int64_t prof_frame_ = -1;
  uint64_t ts_frames_ = 0;
  std::chrono::steady_clock::time_point ts_last_log_{};
  bool CreateDrawResources();
  bool BeginFrame();
  void EndFrameAndPresent(uint32_t front_buffer_address);

  struct UploadAlloc {
    uint8_t* cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    ID3D12Resource* resource = nullptr;  // upload buffer holding the allocation
    uint64_t offset = 0;                 // offset within `resource`
  };
  // Overflow upload pages when the per-frame ring is exhausted (load spikes).
  Microsoft::WRL::ComPtr<ID3D12Resource> overflow_page_;
  uint8_t* overflow_cpu_ = nullptr;
  size_t overflow_size_ = 0, overflow_offset_ = 0;
  uint64_t overflow_pages_created_ = 0;
  size_t upload_peak_ = 0;
  bool Upload(size_t size, size_t alignment, UploadAlloc& out);

  // Host render target for a guest surface.
  struct HostSurface {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    DXGI_FORMAT resource_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;  // RTV / DSV format
    DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;   // format of resolve copies
    uint32_t width = 0, height = 0;
    bool depth = false;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    D3D12_CPU_DESCRIPTOR_HANDLE view{};
    uint32_t edram_base = 0;
    uint32_t guest_format = 0;
    uint32_t srv_index = 0;  // depth surfaces: SRV for shader-based depth resolves
    uint32_t stencil_srv_index = 0;
    uint32_t samples = 1;  // host MSAA sample count (guest RB_SURFACE_INFO msaa)
    // Host pixels per guest pixel (resolution / shadow quality settings, may be
    // fractional). width/height stay in guest pixels; the resource is
    // HostPx(width, scale).
    float scale = 1.0f;
    bool shadow = false;  // shadow map target (scale = shadow_quality)
    // EDRAM footprint (80x16-sample tiles, 2048 total) for aliasing: a surface
    // whose EDRAM was overwritten by another surface since its last write has
    // undefined contents on Xenos and must not carry data across (feedback).
    uint32_t edram_tiles = 0;
    uint64_t last_write = 0;
  };
  // full_scale: the surface at the output scale even where DLSS draws the
  // scene smaller (the HUD's twin of the scene depth).
  HostSurface* GetSurface(uint8_t* base, uint32_t surface_object, bool depth,
                          uint32_t min_width = 0, uint32_t min_height = 0, bool full_scale = false);
  void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& state,
                  D3D12_RESOURCE_STATES after);
  bool BindRenderTargets(uint8_t* base, uint32_t dev, DXGI_FORMAT rtv_formats[4],
                         uint32_t& rt_count, DXGI_FORMAT& dsv_format);

  // Host texture written by resolves, keyed by guest base address.
  struct ResolveTexture {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;  // guest pixels
    uint32_t layers = 1;             // stacked destinations: one array slice per layer
    float scale = 1.0f;              // host pixels per guest pixel (source surface)
    bool shadow = false;             // resolved from a shadow map target
    uint32_t plain_srv = 0;          // default-mapped SRV (soft particles' scene depth)
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    std::unordered_map<uint32_t, uint32_t> srv_by_mapping;  // component mapping -> srv index
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};  // depth resolves (R32_FLOAT) are drawn into
    uint32_t guest_fetch[6] = {};       // destination fetch constant (A/B dumps)
    bool swap_rb = false;  // RB_COPY_DEST_INFO.copy_dest_swap of the last resolve
    // Depth resolves: the raw D24S8 word as RGBA8 for k_8_8_8_8 fetches.
    Microsoft::WRL::ComPtr<ID3D12Resource> raw_resource;
    D3D12_RESOURCE_STATES raw_state = D3D12_RESOURCE_STATE_COMMON;
    D3D12_CPU_DESCRIPTOR_HANDLE raw_rtv{};
    std::unordered_map<uint32_t, uint32_t> raw_srv_by_mapping;
  };

  const std::vector<uint8_t>* LoadShader(uint64_t hash, bool vertex);
  void BuildInputLayout(uint8_t* base, uint32_t decl, const std::vector<uint8_t>& vs,
                        std::vector<D3D12_INPUT_ELEMENT_DESC>& elements, struct PsoRecord* record);
  ID3D12PipelineState* GetPipeline(uint8_t* base, uint32_t dev, uint64_t vs_hash, uint64_t ps_hash,
                                   uint32_t decl, D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type,
                                   const DXGI_FORMAT rtv_formats[4], uint32_t rt_count,
                                   DXGI_FORMAT dsv_format,
                                   D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut);
  // stacked_array: a TEXTURE2DARRAY view of a resolved texture (stacked 2D
  // read by tfetch3D, space6); 0 when the fetch has no such view.
  uint32_t GetTextureSrvIndex(uint8_t* base, const uint32_t fetch[6], bool stacked_array = false);
  uint32_t GetSamplerIndex(const uint32_t fetch[6], bool force_linear = false,
                           float lod_bias = 0.0f, bool render_target = false);
  bool UploadConstants(uint8_t* base, uint32_t dev);
  void ApplyFixedFunctionState(uint8_t* base, uint32_t dev);
  bool ScreenSpaceDraw(uint8_t* base, uint32_t dev);
  bool AlphaTestToCoverage(uint8_t* base, uint32_t dev);
  void ScreenSpaceTargetSize(float& w, float& h);
  // Common draw setup; returns false (and counts a skip) if the draw can't be
  // rendered natively yet.
  bool PrepareDraw(uint8_t* base, uint32_t dev, uint32_t prim, D3D12_PRIMITIVE_TOPOLOGY& topology,
                   bool& quads,
                   D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut =
                       D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED);
  // Vertex range of a draw, computed only when a dirty buffer needs it (PlanBuffer
  // resolves it lazily; indexed draws scan their indices).
  struct VertexRange {
    uint32_t first = 0, end = ~0u;
    bool resolved = true;
    // Indexed draw source (resolved lazily).
    uint32_t ib_phys = 0, ib_size = 0, start_index = 0, index_count = 0;
    int32_t base_vertex = 0;
    bool index32 = false;
    void Resolve();
    uint32_t index_endian = 0;
    uint32_t reset_index = UINT32_MAX;
  };
  bool DynamicVertexFetch(uint8_t* base, uint32_t dev);
  bool RefreshTrackedBuffer(TrackedBuffer& t);
  bool BindVertexStreams(uint8_t* base, const WorkBatch& batch, const WorkCmd& cmd);

  uint32_t AllocSrvIndex();

  static constexpr uint32_t kFramesInFlight = 3;
  static constexpr uint32_t kOutputWidth = profile::kOutputWidth;
  static constexpr uint32_t kOutputHeight = profile::kOutputHeight;
  static constexpr size_t kUploadBytesPerFrame = 128u << 20;
  static constexpr uint32_t kSrvHeapSize = 32768;
  // Ray-traced sun shadows (docs/cienie-rt-projekt.md): fixed descriptors the
  // receivers address directly (SR_RT_FRAME_INDEX in XenosRecomp patch 0017).
  static constexpr uint32_t kRtFrameSrv = kSrvHeapSize - 2;
  static constexpr uint32_t kRtTlasSrv = kSrvHeapSize - 1;
  static constexpr uint32_t kRtMaterialSrv = kSrvHeapSize - 3;  // per TLAS instance (reflections)
  static constexpr uint32_t kRtUvSrv = kSrvHeapSize - 4;        // mesh texture coordinates (reflections)
  static constexpr uint32_t kRtSkinSrv0 = kSrvHeapSize - 5;  // character buffers 0 and 1 (-6)
  static constexpr uint32_t kSrvAllocLimit = kSrvHeapSize - 6;
  static constexpr uint32_t kSamplerHeapSize = 2048;
  static constexpr uint32_t kRtvHeapSize = 256;
  static constexpr uint32_t kDsvHeapSize = 64;

  bool initialized_ = false;
  bool init_failed_ = false;

  rex::ui::d3d12::D3D12Presenter* presenter_ = nullptr;
  const rex::ui::d3d12::D3D12Provider* provider_ = nullptr;
  ID3D12Device* device_ = nullptr;
  ID3D12CommandQueue* queue_ = nullptr;
  // Own direct queue (sr_native_own_queue): isolated from the presenter/Xenos.
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> own_queue_;

  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocators_[kFramesInFlight];
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list_;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
  HANDLE fence_event_ = nullptr;
  uint64_t frame_fence_values_[kFramesInFlight] = {};
  uint64_t next_fence_value_ = 1;
  uint32_t frame_index_ = 0;
  uint64_t frame_count_ = 0;
  bool frame_open_ = false;

  // Presentation: fullscreen blit of the resolved front buffer into the
  // presenter's guest output (R10G10B10A2).
  Microsoft::WRL::ComPtr<ID3D12RootSignature> blit_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> blit_pipeline_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> depth_copy_root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> depth_copy_ms_pipeline_;
  uint32_t bound_samples_ = 1;  // sample count of the current bind set
  float bound_scale_ = 1.0f;   // host pixels per guest pixel of the current bind set
  // User settings (cvars), fixed at startup.
  float render_scale_ = 1.0f, shadow_scale_ = 1.0f, bloom_scale_ = 1.0f;
  float shadow_mul_ = 1.0f;  // sr_native_shadow_scale_mul (shape-detected shadow map)
  bool full_scene_resolution_ = false, foliage_aa_ = false;
  uint32_t msaa_samples_ = 0;
  int32_t anisotropy_ = -1;
  uint32_t output_width_ = kOutputWidth, output_height_ = kOutputHeight;
  // EDRAM same-base reinterpretation (edram_alias.hlsl), one PSO per format.
  Microsoft::WRL::ComPtr<ID3D12RootSignature> alias_root_signature_;
  std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> alias_pipelines_;
  bool ReinterpretSurface(HostSurface& dst, HostSurface& src);
  // FXAA graphics option (EXP-045): applied to the "Upscale" pass target when
  // the HUD pass starts, so the HUD stays sharp.
  void ApplyFxaa(HostSurface& target);
  // SSAO graphics option (EXP-045): after the opaque scene, before End Tiling.
  void CaptureCamera();
  void ApplySsao();
  // Ray-traced shadows: per-frame data (inverse cascade-0 light matrix).
  void UpdateRtFrame(uint8_t* base);
  Microsoft::WRL::ComPtr<ID3D12Resource> rt_frame_buffer_;
  D3D12_RESOURCE_STATES rt_frame_state_ = D3D12_RESOURCE_STATE_COMMON;
  uint64_t rt_frame_swap_ = ~0ull;
  // DXR scene (rt_shadows.cpp): step 2 holds one test cube.
  bool EnsureRtScene(const float inv_light[16]);
  Microsoft::WRL::ComPtr<ID3D12Device5> rt_device_;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> rt_list_;
  Microsoft::WRL::ComPtr<ID3D12Resource> rt_cube_vb_, rt_cube_ib_, rt_blas_, rt_tlas_,
      rt_instances_, rt_scratch_;
  bool rt_scene_ready_ = false;
  float rt_cube_center_[3] = {};
  // City geometry (step 3): one BLAS per static mesh drawn by the game, one
  // TLAS instance per (mesh, world matrix) seen recently.
  struct BufferEntry;
  void RtCaptureDraw(uint8_t* base, const WorkBatch& batch, const WorkCmd& cmd, const BufferEntry* ib,
                     uint32_t start_index, uint32_t index_count, int32_t base_vertex, bool strip);
  // Non-indexed instanced draws (the instance_transforms vertex shaders): many
  // copies of one mesh per draw, mesh indices in the POSITION1 stream.
  void RtCaptureInstanced(uint8_t* base, const WorkBatch& batch, const WorkCmd& cmd,
                          uint32_t start_vertex, uint32_t vertex_count);
  bool RtBuildCityTlas(const float sun_anchor[3]);
  // Colour of a surface for ray-traced reflections: the draw's colour texture
  // (SRV index | 0x8000 for PWL gamma, 0xFFFFFF = none; valid while the SRV
  // index keeps the generation it had, srv_generation_), its sampler and the
  // texture coordinates (interpolant captured with the mesh, scale, offset).
  struct RtMaterial {
    uint32_t albedo = 0xFFFFFF, albedo_gen = 0, sampler = 0;
    int32_t uv_interp = -1;  // interpolant the pixel shader takes the UV from (-1: mean colour)
    uint32_t uv_comps = 0;   // u component | v component << 2
    float uv[4] = {1.0f, 1.0f, 0.0f, 0.0f};  // uv = interpolant * uv[0..1] + uv[2..3]
    // Tint blend (building shaders, rt_tint_regs.inc): colour = sum(mask channel
    // (/ saturate(channel sum)) * R/G/B tint) * tint * colour texture.
    uint32_t mask = 0xFFFFFF, mask_gen = 0, mask_sampler = 0;
    uint32_t tint_flags = 0;  // R ch | G ch << 2 | B ch << 4 | normalised << 6 | tint << 7 | colour texture << 8
                              // | palette << 10
    float tints[4][4] = {};   // R_tint, G_tint, B_tint, tint (rgb); with a palette: [3] = building weights
    // Building palette (rt_palette_regs.inc): A_tint for the rest weight, the
    // palette (index into kPaletteRegs) and its index component and scale.
    float a_tint[4] = {};
    int32_t palette = -1, palette_interp = -1;
    uint32_t palette_comp = 0;
    float palette_scale = 0.0f;
  };
  bool RtDrawMaterial(int32_t captured_uv_interp, RtMaterial& out);
  struct RtMesh {
    Microsoft::WRL::ComPtr<ID3D12Resource> blas;
    Microsoft::WRL::ComPtr<ID3D12Resource> indices;  // own index buffer (instanced meshes)
    uint64_t last_swap = 0;
    uint32_t triangles = 0;
    uint32_t uv_base = 0xFFFFFFFFu;  // first vertex in rt_uv_pool_ (3 per triangle), none = ~0
    int32_t uv_interp = -1;          // the TEXCOORD interpolant captured there (+0 of each 32-byte vertex)
    int32_t palette_interp = -1;     // and the palette index interpolant (+16), -1 = none
    bool uv_tried = false;
    RtMaterial material;  // the last material seen on any copy (for copies never drawn textured)
  };
  struct RtInstance {
    uint64_t mesh = 0;
    float transform[12] = {};
    uint64_t first_swap = 0, last_swap = 0;
    uint8_t source = 0;  // 1 = indexed scene draw, 2 = shadow caster pass (Mobj), 4 = instanced draw
    RtMaterial material;  // from the last draw of it that had a colour texture
  };
  static constexpr uint32_t kRtNoAlbedo = 0xFFFFFF;
  uint32_t rt_tex2d_[16] = {};  // the draw's 2D texture SRV words (shared constants 0..15)
  uint32_t rt_samp_[16] = {};   // and their sampler indices (shared constants 48..63)
  // The city shaders' lighting tone: gShadCol rgb, Kramp_lo_hi_Kao.x, ramp_col_exp rgb, valid.
  float rt_tone_[8] = {};
  ID3D12PipelineState* RtSoPipeline(uint8_t* base, const char* semantic, uint32_t index, uint32_t components,
                                    const char* semantic2 = nullptr, uint32_t index2 = 0, uint32_t components2 = 0);
  int32_t RtDrawUvInterp() const;
  bool RtSoBeginDraw();
  void RtUploadMaterials(const void* data, size_t bytes);
  void RtUploadPalettes();
  // Material buffer (kRtMaterialSrv): 128-byte entries for every TLAS instance,
  // then the building palettes (16 x 8 float4 colours).
  static constexpr size_t kRtMaterialBytes = 32769 * 128, kRtPaletteBytes = 16 * 8 * 16;
  void RtSoDraw(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS dst, uint64_t bytes, bool indexed,
                uint32_t count, uint32_t start, int32_t base_vertex);
  void RtCaptureUv(RtMesh& mesh, uint8_t* base, int32_t interp, int32_t palette_interp, bool indexed,
                   uint32_t count, uint32_t start, int32_t base_vertex);
  uint64_t rt_uv_swap_ = 0;
  uint32_t rt_uv_this_frame_ = 0;
  Microsoft::WRL::ComPtr<ID3D12Resource> rt_uv_pool_, rt_material_buffer_;
  D3D12_RESOURCE_STATES rt_uv_state_ = D3D12_RESOURCE_STATE_STREAM_OUT;
  D3D12_RESOURCE_STATES rt_material_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  uint64_t rt_uv_cursor_ = 0;
  std::vector<uint32_t> srv_generation_;  // bumped when an SRV index is retired
  uint32_t SrvGeneration(uint32_t index) const {
    return index < srv_generation_.size() ? srv_generation_[index] : 0;
  }
  void RtAddInstance(const RtInstance& instance);
  void RtDumpObj(uint8_t* base, uint32_t vertex0, uint32_t stride, const std::vector<uint32_t>& idx,
                 const float transform[12], uint64_t instance_key);
  bool RtBuildBlas(const D3D12_RAYTRACING_GEOMETRY_DESC& geometry, RtMesh& mesh);
  // Skinned characters: each skinned receiver draw is drawn a second time with
  // a stream-output pipeline that keeps only SRLIGHT0 (the post-skinning
  // light-space position) as a triangle soup; at the next frame's TLAS build
  // all of them become one BLAS, instanced with the inverse light rows.
  struct RtSoTarget {  // render state of the draw the stream-output pass repeats
    uint32_t decl = 0, rt_count = 0;
    DXGI_FORMAT rtv[4] = {}, dsv = DXGI_FORMAT_UNKNOWN;
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    ID3D12PipelineState* pso = nullptr;
  } rt_so_target_;
  void RtCaptureSkinned(uint8_t* base, uint32_t u, uint32_t v, uint32_t z, uint32_t start_index,
                        uint32_t index_count, int32_t base_vertex, uint32_t so_vertices);
  bool RtBuildSkinnedBlas();
  bool RtEnsureScratch(uint64_t size);
  std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> rt_so_pipelines_;
  // Character stream output, 32 bytes a vertex (UV interpolant, light-space
  // position): written in one buffer while the other holds the previous
  // frame's, which the BLAS was built from and the reflection hits read.
  Microsoft::WRL::ComPtr<ID3D12Resource> rt_so_buffers_[2];
  D3D12_RESOURCE_STATES rt_so_states_[2] = {D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_STREAM_OUT};
  uint32_t rt_so_write_ = 0, rt_skin_read_ = 0;
  Microsoft::WRL::ComPtr<ID3D12Resource> rt_so_counters_, rt_skin_blas_;
  uint64_t rt_skin_blas_size_ = 0, rt_so_cursor_ = 0, rt_so_swap_ = 0;
  uint32_t rt_so_draws_ = 0;
  struct RtSkinDraw {
    uint64_t offset;  // bytes into the character buffer
    uint32_t vertices;
    RtMaterial material;
    bool has_material;
  };
  std::vector<RtSkinDraw> rt_so_pending_;  // this frame's character draws
  std::vector<RtSkinDraw> rt_skin_draws_;  // the ones the current character BLAS holds
  float rt_skin_transform_[12] = {};  // inverse light rows of the captured frame
  float rt_frame_inv_[12] = {};       // inverse light rows of this frame (light space to world)
  bool rt_skin_ready_ = false;
  bool RtMakeIndexBuffer(const std::vector<uint32_t>& indices, RtMesh& mesh);
  std::unordered_map<uint64_t, RtMesh> rt_meshes_;
  std::unordered_map<uint64_t, RtInstance> rt_city_instances_;
  Microsoft::WRL::ComPtr<ID3D12Resource> rt_city_tlas_, rt_build_scratch_;
  uint64_t rt_city_tlas_size_ = 0, rt_build_scratch_size_ = 0;
  uint64_t rt_builds_swap_ = 0;
  uint32_t rt_builds_this_frame_ = 0;
  bool rt_city_ready_ = false;
  struct RtStats {
    uint64_t captured = 0, new_meshes = 0, skip_format = 0, skip_prim = 0, skip_budget = 0,
             skip_skinned = 0, skinned_draws = 0, uv_meshes = 0, instanced_draws = 0, instanced_copies = 0, skip_instanced = 0;
  } rt_stats_;
  bool rt_scene_failed_ = false;
  bool ssao_ = false;
  HostSurface* scene_rt_ = nullptr;  // HDR scene target of the Opaque pass
  HostSurface* scene_ds_ = nullptr;
  float inv_view_proj_[16] = {};     // g_mProjectionToWorld of this frame
  bool camera_valid_ = false;
  bool ssao_done_ = false;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> ssao_root_signature_;
  std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> ssao_pipelines_;
  Microsoft::WRL::ComPtr<ID3D12Resource> ssao_ao_;
  D3D12_RESOURCE_STATES ssao_ao_state_ = D3D12_RESOURCE_STATE_COMMON;
  D3D12_CPU_DESCRIPTOR_HANDLE ssao_ao_rtv_{};
  uint32_t ssao_ao_srv_ = 0;
  bool fxaa_ = false;
  bool smooth_effects_ = true;
  int32_t shadow_smoothing_ = 0;
  // Soft particles option (EXP-045): latest scene depth resolve of the frame.
  bool soft_particles_ = false;
  uint64_t scene_depth_key_ = 0;
  HostSurface* upscale_rt_ = nullptr;  // last color target bound in the Upscale pass
  int last_exec_pass_ = -1;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> fxaa_root_signature_;
  std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> fxaa_pipelines_;
  Microsoft::WRL::ComPtr<ID3D12Resource> fxaa_temp_;
  D3D12_RESOURCE_STATES fxaa_temp_state_ = D3D12_RESOURCE_STATE_COMMON;
  uint32_t fxaa_srv_ = 0;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> depth_copy_pipeline_;
  // One output target per frame in flight: with the own queue, the copy into
  // the presenter's guest output runs on the presenter's (direct) queue.
  Microsoft::WRL::ComPtr<ID3D12Resource> output_rts_[kFramesInFlight];
  D3D12_RESOURCE_STATES output_rt_states_[kFramesInFlight] = {};
  D3D12_CPU_DESCRIPTOR_HANDLE output_rtvs_[kFramesInFlight] = {};
  ID3D12Resource* output_rt() const { return output_rts_[frame_index_].Get(); }
  // Copy into the presenter's resource on the direct queue.
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> present_allocators_[kFramesInFlight];
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> present_list_;
  Microsoft::WRL::ComPtr<ID3D12Fence> present_fence_;
  uint64_t present_fence_next_ = 1;
  uint64_t present_fence_values_[kFramesInFlight] = {};

  // Draw resources.
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> sampler_heap_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsv_heap_;
  // 0 = null 2D, 1 = null 3D, 2 = null cube (unbound / undecodable fetch slots).
  uint32_t srv_heap_next_ = 3;
  uint32_t sampler_heap_next_ = 0;
  uint32_t rtv_heap_next_ = 0;
  uint32_t dsv_heap_next_ = 0;
  Microsoft::WRL::ComPtr<ID3D12Resource> upload_buffers_[kFramesInFlight];
  Microsoft::WRL::ComPtr<ID3D12Resource> default_vertex_buffer_;
  D3D12_VERTEX_BUFFER_VIEW guest_vertex_views_[16] = {};
  uint8_t* upload_cpu_[kFramesInFlight] = {};
  size_t upload_offset_ = 0;
  bool frame_state_bound_ = false;

  // Currently bound render targets (to skip redundant OMSetRenderTargets).
  HostSurface* bound_rts_[4] = {};
  HostSurface* bound_ds_ = nullptr;
  uint32_t bound_rt_count_ = 0;

  // Tiling: while active, guest surfaces are allocated at the union extent of
  // the tile rects (full screen) instead of the per-tile surface size.
  bool tiling_active_ = false;
  uint32_t tiling_width_ = 0, tiling_height_ = 0;

  std::string shader_dir_;
  std::unordered_map<uint64_t, std::unique_ptr<std::vector<uint8_t>>> shader_bytecode_;
  std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D12PipelineState>> pipelines_;
  PipelineCache pipeline_cache_;  // precompiled / recorded PSOs (EXP-044)
  // Pipelines compiling asynchronously for draws that were skipped (EXP-053).
  std::unordered_set<uint64_t> async_pipelines_;

  struct TextureEntry {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    uint32_t srv_index = 0;
    // Content validation: all decoder source ranges and their combined hash. Dynamic
    // textures (fog tables, CPU-written lookup tables) are rewritten in place.
    uint32_t guest_base = 0;
    uint32_t guest_size = 0;
    std::vector<graphics::guest::TextureRange> guest_ranges;
    uint64_t guest_hash_bytes = 0;
    uint64_t content_hash = 0;
    uint64_t checked_frame = 0;
    uint64_t hashed_frame = 0;
    // Write watch: page write sequence when the watch was armed.
    uint32_t watch_seq = 0;
    uint32_t guest_format = 0;
    D3D12_RESOURCE_STATES debug_state = D3D12_RESOURCE_STATE_COMMON;  // frame dumps
  };
  // Guest physical memory write watches (page protection, like Xenia's shared
  // memory): per-4KB-page sequence of the last write notification.
  static std::pair<uint32_t, uint32_t> OnPhysicalWrite(void* context, uint32_t start,
                                                       uint32_t length, bool exact_range);
  uint32_t ArmTextureWatch(uint32_t base, uint32_t size);
  bool TextureWrittenSince(uint32_t base, uint32_t size, uint32_t seq) const;
  std::unique_ptr<std::atomic<uint32_t>[]> page_write_seq_;
  std::atomic<uint32_t> write_seq_{1};
  // Published after page_write_seq_: used only to reuse clean frontend scans.
  std::atomic<uint64_t> completed_texture_writes_{0};
  bool texture_watch_ = false;
  uint32_t CreateTexture(const uint32_t fetch[6], TextureEntry& entry);
  void RetireSrvIndex(uint32_t index);
  void CopyDepthRegion(HostSurface& src, ResolveTexture& dst, uint32_t x1, uint32_t y1,
                       uint32_t x2, uint32_t y2, uint32_t dx, uint32_t dy);
  std::vector<std::pair<uint64_t, uint32_t>> retired_srvs_;
  std::unordered_map<uint64_t, TextureEntry> textures_;
  std::unordered_map<uint64_t, uint32_t> samplers_;
  std::unordered_map<uint64_t, HostSurface> surfaces_;
  // Keyed by (guest base address, host format): the game aliases the same
  // guest memory for differently formatted resolve targets within a frame.
  std::unordered_map<uint64_t, ResolveTexture> resolve_textures_;
  // Latest resolve written to each guest base address (what a later bind of
  // that address should read).
  std::unordered_map<uint32_t, uint64_t> resolve_latest_;
  // Deferred releases: resources may still be referenced by in-flight frames.
  std::vector<std::pair<uint64_t, Microsoft::WRL::ComPtr<ID3D12Resource>>> retired_;
  void Retire(Microsoft::WRL::ComPtr<ID3D12Resource> resource);
  // Debug: read back all native surfaces/resolve textures after this frame.
  void DumpFrameResources(const std::string& prefix = "", bool surfaces_only = false,
                          bool output_only = false);
  bool frame_dump_done_ = false;
  bool dump_vs_after_pending_ = false;  // sr_native_dump_vs: dump again at the next draw
  // sr_native_watch_blue
  void WatchHdr(uint32_t index);
  Microsoft::WRL::ComPtr<ID3D12Resource> watch_readback_;
  uint64_t watch_total_ = 0;
  int64_t watch_frame_ = -1;
  bool watch_done_ = false;
  int watch_prev_count_ = -1;
  int watch_prev_sentinel_ = -1;
  uint32_t watch_last_index_ = 0;
  float watch_prev_probe_[4] = {-1, -1, -1, -1};
  std::string watch_ops_;
  // Frame trace (sr_native_trace_frame_at_s): 0 idle, 1 tracing this frame, 2 done.
  int trace_state_ = 0;
  bool dump_all_active_ = false;
  rex::system::IGraphicsSystem* graphics_ = nullptr;
  uint64_t current_ps_hash_ = 0;
  uint64_t current_vs_hash_ = 0;

  // DLSS (dlss.cpp): the scene is the colour surface resolved right after the
  // full-size depth resolve that ends the 3D draws (learned each frame); its
  // draws get a sub-pixel jitter, and at that colour resolve DLSS replaces the
  // surface's contents before the game's post-processing reads them.
  void DlssCaptureCamera();
  void DlssJitter(float half_pixel[2]);
  void DlssOnDepthResolve(HostSurface& src);
  void DlssOnColorResolve(HostSurface& src);
  void DlssEndFrame();
  // Scene resolution / output resolution of the current mode (1 = off or DLAA).
  float DlssSceneRatio() const;
  uint32_t DlssSceneVariant() const;  // the upscaling mode (2..4), 0 at full resolution
  // Set by DlssOnColorResolve when DLSS made an output-sized scene: the
  // resolve copies it (its texture at the output scale) instead of the surface.
  ID3D12Resource* dlss_resolve_source_ = nullptr;
  // The HUD's output-scale depth twin, cleared once a frame (BindRenderTargets).
  ID3D12Resource* dlss_twin_ = nullptr;
  uint64_t dlss_twin_swap_ = ~0ull;

  // Objects' motion (DLSS): every translated vertex shader also runs with the
  // previous frame's VS constants of the same draw (root CBV b3, XenosRecomp
  // patch 0030) and the pixel shader writes the difference to SV_Target4,
  // bound to the object-motion target for the scene's draws. Draws are
  // matched across frames by a key (shaders, vertex buffer, primitive, count,
  // occurrence); a draw without a match marks its pixels for the camera's
  // motion (g_SrMotionValid = 0).
  bool DlssMotionDraw() const;  // the bound targets are the jittered scene
  // The object-motion target's RTV, created and cleared for this frame on first
  // use; nullptr if there is none.
  const D3D12_CPU_DESCRIPTOR_HANDLE* DlssMotionTarget();
  void DlssDrawKey(uint64_t vs_hash, uint64_t ps_hash, uint32_t fetch0, uint32_t prim, uint32_t count);
  // The previous constants of this draw (uploaded) or 0, and whether they are.
  D3D12_GPU_VIRTUAL_ADDRESS DlssPrevVsConstants(bool& valid);
  uint32_t dlss_draw_count_ = 0;  // vertex or index count of the draw being prepared
  uint64_t dlss_draw_key_ = 0;
  // The draw without its vertex buffer address: a mesh rebuilt every frame in
  // a new buffer still has the same one (DlssPrevVsConstants).
  uint64_t dlss_draw_loose_key_ = 0;
  std::unordered_set<uint64_t> dlss_prev_loose_, dlss_cur_loose_;
  std::array<uint32_t, 1024> cb_vs_cpu_{};  // the current VS constants as uploaded
  std::unordered_map<uint64_t, std::array<uint32_t, 1024>> dlss_prev_consts_, dlss_cur_consts_;
  std::unordered_map<uint64_t, uint32_t> dlss_occurrence_;
  bool bound_motion_ = false;
  std::unique_ptr<DlssState, DlssDeleter> dlss_;
  ID3D12Resource* dlss_scene_ = nullptr;     // learned in the previous frame
  HostSurface* dlss_scene_depth_ = nullptr;  // this frame's last resolved depth
  bool dlss_depth_just_resolved_ = false;
  // The scene is resolved more than once a frame (the game composites its
  // quarter-resolution particles onto it and resolves again): DLSS runs at
  // the last one, counted the frame before.
  int dlss_scene_resolves_ = 0, dlss_scene_resolves_prev_ = 0;
  bool dlss_scene_done_ = false;  // DLSS ran this frame
  bool dlss_camera_ = false, dlss_prev_camera_ = false;
  float dlss_view_proj_[16] = {}, dlss_prev_view_proj_[16] = {};
  float dlss_jitter_[2] = {};  // this frame's jitter, render pixels (x right, y down)
  uint32_t dlss_frame_ = 0;
  int draw_calls_in_frame_ = 0;  // draw calls seen this frame (sr_native_dump_before_draw)
  int resolve_in_frame_ = 0;  // resolves seen this frame (sr_native_dump_before_resolve)
  // GPU hang diagnostics: WriteBufferImmediate breadcrumbs after each draw
  // into CPU-visible memory + a ring of draw descriptions.
  Microsoft::WRL::ComPtr<ID3D12Resource> crumb_buffer_;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList2> crumb_list_;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList1> resolve_list_;
  volatile uint32_t* crumb_cpu_ = nullptr;
  uint32_t crumb_seq_ = 0;
  struct CrumbDesc {
    uint32_t seq = 0;
    int pass = 0;
    uint64_t vs = 0, ps = 0;
    uint64_t frame = 0;
    const char* what = "draw";
    uint32_t prim = 0, count = 0;
  } crumbs_[4096];
  uint32_t crumb_prim_ = 0, crumb_count_ = 0;
  void Breadcrumb(const char* what = "draw");
  void ReportHang();
  // Float constant buffers of the previous draw, reused while the mirror's
  // constants are unchanged (valid within one frame's upload ring).
  uint64_t cb_vs_version_ = 0, cb_ps_version_ = 0;
  D3D12_GPU_VIRTUAL_ADDRESS cb_vs_gpu_ = 0, cb_ps_gpu_ = 0;
  SharedConstantsCache shared_constants_cache_;
  // Per fetch slot: last fetch constant and its SRV index this frame.
  struct SlotCache {
    uint32_t fetch[6] = {};
    uint32_t srv = 0;
    uint32_t stacked_srv = 0;  // array view for tfetch3D of a stacked 2D fetch
    uint64_t frame = ~0ull;
    bool smooth = false;  // point fetch of an upscaled color target -> linear
    bool render_target = false;  // a resolve texture: no forced anisotropy
  } slot_cache_[16];
  uint64_t edram_write_seq_ = 0;
  HostSurface* bind_set_[5] = {};
  void MarkWritten(HostSurface* s) {
    if (s) s->last_write = ++edram_write_seq_;
  }
  // Clears `s` if an overlapping surface was written after it (EDRAM aliasing).
  void ResolveEdramAliasing(HostSurface* s);
  uint64_t alias_clear_frame_ = ~0ull;
  // Development frontend capture uses the same parser; Vulkan will consume
  // frontend packets directly and will not rescan commands on its worker.
  // Register value as the GPU sees it (mirror), falling back to the XDK shadow
  // at dev+shadow_offset for registers no packet has written yet.
  uint32_t GpuConstant(uint8_t* base, uint32_t dev, uint32_t reg, uint32_t shadow_offset) const;
  std::string SurfaceName(const HostSurface* s) const;

  // Byte-swapped host copies of guest vertex/index buffers.
  struct BufferEntry {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    uint32_t raw_srv = 0;
    uint64_t raw_srv_fence = 0;
    uint32_t address = 0, size = 0;
    uint32_t decl = 0, stride = 0, index_format = 0, phase = 0;
    uint32_t reset_index = UINT32_MAX;
    // Dirty tracking lives on the guest side (TrackedBuffer / BufferPlan).
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  };
  std::unordered_map<uint64_t, BufferEntry> buffers_;
  uint32_t vertex_fetch_[224][4] = {}; // semantic -> SRV, offset, stride, XDK type
  // 64 KB guest page -> tracked buffer keys overlapping it (fast Unlock
  // invalidation, guest side).
  std::unordered_map<uint32_t, std::vector<TrackedBuffer*>> buffer_pages_;
  // For vertex data, `phase` = offset (mod stride) of the first vertex the
  // draws read, so the per-element byte swap lines up with real vertices.
  // need_begin/need_end: byte range (relative to address) the draw reads;
  // [0, size) when unknown.
  // Worker: host buffer for a guest-side plan (creates / uploads as planned).
  const BufferEntry* ApplyBuffer(uint8_t* base, const BufferPlan& plan);
  // Byte-swaps [begin, end) of the guest buffer into `dst` (host order).
  void SwapBufferRange(uint8_t* base, const BufferEntry& entry, uint32_t begin, uint32_t end,
                       uint8_t* dst);

  Stats stats_;
  // The game records from more than one guest thread (render thread draws,
  // Swap/resolves from the presenting thread): one lock covers all recording.
  std::recursive_mutex mutex_;
  void FlushRingConstants(uint8_t* base, uint32_t dev);
};

}  // namespace superman_returns::native

