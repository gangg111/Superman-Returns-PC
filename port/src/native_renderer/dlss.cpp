// NVIDIA DLSS (DLAA and the upscaling modes) on the game's 3D scene.
//
// A traced frame (sr_native_trace_frame_at_s, 2026-10-21) shows the order:
// the 3D draws go to a float (HDR) 1280x720 colour surface with its depth,
// then a full-size depth resolve and the colour resolve end the scene; after
// them come the game's post-processing (luminance chain, bloom, tone mapping
// into an 8-bit surface) and the HUD. So:
//   - the scene surface is the colour surface resolved right after a
//     full-size depth resolve (learned each frame; the first frame has no
//     jitter);
//   - its draws get a sub-pixel jitter (Halton 2,3) through the shared
//     g_HalfPixelOffset every translated vertex shader already adds to its
//     position; screen-space draws and every other target are left alone;
//   - the camera is the view-projection of the vertex shaders that declare
//     gViewProjectionXform (camera_regs.inc), unjittered, kept a frame;
//   - at the scene's colour resolve, motion vectors are made from the depth
//     and the two cameras (motion_vectors.hlsl), DLSS runs on the HDR colour
//     (pre-tone-mapping, auto exposure, inverted depth: the game's depth test
//     is GEQUAL) and its output is copied back into the surface, so the
//     resolve and everything after it read the anti-aliased scene.
// Motion vectors cover the camera only: moving objects (Superman, cars,
// pedestrians) have none of their own yet.

#include "native_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

#include <windows.h>

#include <fmt/format.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_provider.h>

#include "shaders/motion_debug_cs.h"
#include "shaders/motion_vectors_cs.h"

REXCVAR_DEFINE_INT32(sr_dlss, 0, "Superman Returns Native",
                     "NVIDIA DLSS on the 3D scene: 0 = off, 1 = DLAA (native resolution), 2 = Quality "
                     "(the scene at 2/3 of the output), 3 = Balanced (0.58), 4 = Performance (1/2)")
    .range(0, 4);
REXCVAR_DEFINE_INT32(sr_dlss_preset, -1, "Superman Returns Native",
                     "DLSS model preset: -1 = NVIDIA's recommended transformer per mode (K for DLAA, "
                     "Quality and Balanced; M for Performance; L for Ultra Performance), 11 = K, 10 = J, "
                     "13 = M, 12 = L for every mode, 0 = the library's default")
    .range(-1, 13);
REXCVAR_DEFINE_INT32(sr_dlss_debug, 0, "Superman Returns Native",
                     "DLSS debug: 1 = the motion vectors in place of the scene (red/green = x/y, 0.5 = "
                     "none, 8 render pixels = full; blue = the draw's own motion, else the camera's), "
                     "2 = DLSS off but the jitter on (the raw jittered scene)")
    .range(0, 2);
REXCVAR_DEFINE_DOUBLE(sr_dlss_jitter_scale, 1.0, "Superman Returns Native",
                      "DLSS debug: jitter multiplier as DLSS is told it (-1 flips the convention)")
    .range(-1.0, 1.0);

extern bool g_dlss_hardware;  // native_renderer.cpp: an NVIDIA RTX card, NGX not refused
// sr_dlss as it applies: 0 on a GPU without DLSS (no jitter, no upscale ratio).
int SrDlssMode() { return g_dlss_hardware ? REXCVAR_GET(sr_dlss) : 0; }

namespace superman_returns::native {
extern float g_dlss_mip_bias;  // native_renderer.cpp

namespace {

struct CameraReg {
  uint64_t hash;
  uint32_t reg;
};
constexpr CameraReg kCameraRegs[] = {
#include "camera_regs.inc"
};

float Halton(uint32_t index, uint32_t base) {
  float f = 1.0f, r = 0.0f;
  for (uint32_t i = index; i > 0; i /= base) {
    f /= float(base);
    r += f * float(i % base);
  }
  return r;
}

// Scene resolution / output resolution per sr_dlss mode (NVIDIA's ratios).
constexpr float kSceneRatio[] = {1.0f, 1.0f, 2.0f / 3.0f, 0.58f, 0.5f};
constexpr NVSDK_NGX_PerfQuality_Value kQuality[] = {
    NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality,
    NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf};

}  // namespace

float Renderer::DlssSceneRatio() const {
  return kSceneRatio[std::clamp(SrDlssMode(), 0, 4)];
}

uint32_t Renderer::DlssSceneVariant() const {
  const int mode = std::clamp(SrDlssMode(), 0, 4);
  return kSceneRatio[mode] < 1.0f ? uint32_t(mode) : 0u;
}

struct DlssState {
  bool initialized = false, failed = false;
  NVSDK_NGX_Parameter* params = nullptr;
  NVSDK_NGX_Handle* feature = nullptr;
  uint32_t width = 0, height = 0, out_width = 0;
  int mode = 0;
  bool reset = true;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> mv_root;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> mv_pso;
  Microsoft::WRL::ComPtr<ID3D12Resource> motion, output;
  D3D12_RESOURCE_STATES motion_state = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_STATES output_state = D3D12_RESOURCE_STATE_COMMON;
  uint32_t motion_uav = 0;
  // Objects' motion, written by the scene's pixel shaders (NDC, previous minus
  // current; kNoHistory in x where the camera's motion is to be used).
  Microsoft::WRL::ComPtr<ID3D12Resource> objects;
  D3D12_RESOURCE_STATES objects_state = D3D12_RESOURCE_STATE_COMMON;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> objects_rtv_heap;
  D3D12_CPU_DESCRIPTOR_HANDLE objects_rtv{};
  uint32_t objects_srv = 0;
  uint64_t objects_cleared_swap = ~0ull;
  // sr_dlss_debug=1: the motion vectors drawn into the output.
  Microsoft::WRL::ComPtr<ID3D12PipelineState> debug_pso;
  uint32_t motion_srv = 0, output_uav = 0;
};

namespace {
constexpr float kNoHistory = 65000.0f;  // srMotionNdcFor's mark (shader_common.h)
}

void DlssDeleter::operator()(DlssState* state) const {
  if (!state) return;
  if (state->feature) NVSDK_NGX_D3D12_ReleaseFeature(state->feature);
  if (state->params) NVSDK_NGX_D3D12_DestroyParameters(state->params);
  if (state->initialized) NVSDK_NGX_D3D12_Shutdown1(nullptr);
  delete state;
}

bool Renderer::DlssMotionDraw() const {
  return SrDlssMode() && dlss_scene_ && !dlss_scene_done_ && bound_rts_[0] &&
         bound_rts_[0]->resource.Get() == dlss_scene_ && !(dlss_ && dlss_->failed);
}

const D3D12_CPU_DESCRIPTOR_HANDLE* Renderer::DlssMotionTarget() {
  if (!dlss_) dlss_.reset(new DlssState);
  DlssState& s = *dlss_;
  if (s.failed || !dlss_scene_) return nullptr;
  const D3D12_RESOURCE_DESC scene = dlss_scene_->GetDesc();
  if (!s.objects || s.objects->GetDesc().Width != scene.Width || s.objects->GetDesc().Height != scene.Height) {
    Retire(std::move(s.objects));
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = scene.Width;
    td.Height = scene.Height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = td.Format;
    clear.Color[0] = kNoHistory;
    s.objects_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, s.objects_state, &clear,
                                                IID_PPV_ARGS(&s.objects)))) {
      return nullptr;
    }
    if (!s.objects_rtv_heap) {
      D3D12_DESCRIPTOR_HEAP_DESC hd{};
      hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
      hd.NumDescriptors = 1;
      if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.objects_rtv_heap)))) return nullptr;
      s.objects_rtv = s.objects_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    }
    device_->CreateRenderTargetView(s.objects.Get(), nullptr, s.objects_rtv);
    if (!s.objects_srv) s.objects_srv = AllocSrvIndex();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = td.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(
        s.objects.Get(), &srv,
        provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(), s.objects_srv));
    s.objects_cleared_swap = ~0ull;
  }
  Transition(s.objects.Get(), s.objects_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  if (s.objects_cleared_swap != swap_number_) {
    s.objects_cleared_swap = swap_number_;
    const float clear[4] = {kNoHistory, 0.0f, 0.0f, 0.0f};
    command_list_->ClearRenderTargetView(s.objects_rtv, clear, 0, nullptr);
  }
  return &s.objects_rtv;
}

void Renderer::DlssDrawKey(uint64_t vs_hash, uint64_t ps_hash, uint32_t fetch0, uint32_t prim,
                           uint32_t count) {
  if (!SrDlssMode()) {
    dlss_draw_key_ = 0;
    return;
  }
  uint64_t loose = vs_hash * 0x9E3779B97F4A7C15ull ^ ps_hash;
  loose = loose * 0x100000001B3ull ^ (uint64_t(prim) << 24 | count);
  uint64_t key = loose * 0x100000001B3ull ^ (uint64_t(fetch0) << 32);
  // The same draw more than once a frame (pedestrians sharing a mesh) in order.
  key ^= uint64_t(dlss_occurrence_[key]++) * 0xC2B2AE3D27D4EB4Full;
  loose ^= uint64_t(dlss_occurrence_[loose ^ 1]++) * 0xC2B2AE3D27D4EB4Full;
  dlss_draw_key_ = key;
  dlss_draw_loose_key_ = loose;
}

// sr_dlss_debug: scene draws with / without a previous frame, and the vertex
// shaders of those without, logged every 300 frames.
struct MotionStats {
  uint32_t matched = 0, unmatched = 0, rebuilt = 0;
  std::unordered_map<uint64_t, uint32_t> unmatched_vs, matched_vs;
};
static MotionStats g_motion_stats;

D3D12_GPU_VIRTUAL_ADDRESS Renderer::DlssPrevVsConstants(bool& valid) {
  valid = false;
  if (!dlss_draw_key_ || !DlssMotionDraw()) return 0;
  const size_t vs_bytes = size_t(cb_vs_regs_) * 16;
  const uint32_t prev_slot = dlss_prev_store_.Find(dlss_draw_key_);
  const std::array<uint32_t, 1024>* prev_consts =
      prev_slot != UINT32_MAX ? &dlss_const_arena_.slots[prev_slot] : nullptr;
  // Not moved since the last frame (most of the city): this frame's entry is
  // the previous frame's slot.
  const bool same = prev_consts && std::memcmp(prev_consts->data(), cb_vs_cpu_.data(), vs_bytes) == 0;
  if (same) dlss_cur_store_.PutSame(dlss_const_arena_, dlss_draw_key_, dlss_draw_loose_key_, prev_slot);
  else dlss_cur_store_.Put(dlss_const_arena_, dlss_draw_key_, dlss_draw_loose_key_, cb_vs_cpu_, vs_bytes);
  // Put may have grown the arena: the slot's address again.
  if (prev_consts) prev_consts = &dlss_const_arena_.slots[prev_slot];
  if (!prev_consts && dlss_prev_store_.HasLoose(dlss_draw_loose_key_)) {
    // A mesh rebuilt every frame in a new buffer (Superman's cape: cloth
    // simulated on the CPU, world-space vertices): its previous vertices are
    // gone. It moves with what carries it, which the camera follows, so it
    // is given no motion of its own on screen (the previous constants = the
    // current ones) rather than the camera's.
    if (REXCVAR_GET(sr_dlss_debug)) ++g_motion_stats.rebuilt;
    valid = true;
    return cb_vs_gpu_;
  }
  if (REXCVAR_GET(sr_dlss_debug)) {
    if (!prev_consts) {
      ++g_motion_stats.unmatched;
      ++g_motion_stats.unmatched_vs[current_vs_hash_];
    } else {
      ++g_motion_stats.matched;
      ++g_motion_stats.matched_vs[current_vs_hash_];
    }
  }
  if (!prev_consts) return 0;
  // The current constants serve as the previous ones, no second upload.
  if (same) {
    valid = true;
    return cb_vs_gpu_;
  }
  UploadAlloc prev;
  if (!Upload(4096, 256, prev)) return 0;
  std::memcpy(prev.cpu, prev_consts->data(), vs_bytes);
  valid = true;
  return prev.gpu;
}

void Renderer::DlssCaptureCamera() {
  if (!SrDlssMode() || dlss_camera_) return;
  if (dlss_scene_ && (!bound_rts_[0] || bound_rts_[0]->resource.Get() != dlss_scene_)) return;
  const CameraReg* end = std::end(kCameraRegs);
  const CameraReg* it = std::lower_bound(std::begin(kCameraRegs), end, current_vs_hash_,
                                         [](const CameraReg& e, uint64_t h) { return e.hash < h; });
  if (it == end || it->hash != current_vs_hash_) return;
  const float* c = reinterpret_cast<const float*>(mirror_.regs() + Pm4Mirror::kAluConstantBase);
  std::memcpy(dlss_view_proj_, c + it->reg * 4, sizeof(dlss_view_proj_));
  dlss_camera_ = true;
}

void Renderer::DlssJitter(float half_pixel[2]) {
  if (!SrDlssMode() || !dlss_scene_ || dlss_scene_done_ || !bound_rts_[0] ||
      bound_rts_[0]->resource.Get() != dlss_scene_) {
    return;
  }
  const auto desc = dlss_scene_->GetDesc();
  // NDC: +2/width moves the picture one pixel right, -2/height one pixel down.
  half_pixel[0] += 2.0f * dlss_jitter_[0] / float(desc.Width);
  half_pixel[1] -= 2.0f * dlss_jitter_[1] / float(desc.Height);
}

void Renderer::DlssOnDepthResolve(HostSurface& src) {
  dlss_depth_just_resolved_ = !dlss_scene_done_ && src.samples == 1 && !src.shadow &&
                              src.resource && src.resource->GetDesc().Width >= 1024;
  if (dlss_depth_just_resolved_) dlss_scene_depth_ = &src;
}

void Renderer::DlssOnColorResolve(HostSurface& src) {
  const bool after_depth = dlss_depth_just_resolved_;
  dlss_depth_just_resolved_ = false;
  const int mode = SrDlssMode();
  if (!mode) g_dlss_mip_bias = 0.0f;
  if (!mode || dlss_scene_done_ || !after_depth || src.depth || src.samples != 1 || !src.resource ||
      !dlss_scene_depth_ || !dlss_scene_depth_->srv_index) {
    return;
  }
  const D3D12_RESOURCE_DESC desc = src.resource->GetDesc();
  const D3D12_RESOURCE_DESC depth_desc = dlss_scene_depth_->resource->GetDesc();
  if (desc.Width != depth_desc.Width || desc.Height != depth_desc.Height) return;
  const bool learned = dlss_scene_ == src.resource.Get();
  dlss_scene_ = src.resource.Get();  // jittered from the next frame on
  ++dlss_scene_resolves_;
  if (dlss_scene_resolves_ < dlss_scene_resolves_prev_) return;  // not the frame's last yet
  dlss_scene_done_ = true;
  if (!learned) return;  // this frame was drawn without jitter
  const int debug = REXCVAR_GET(sr_dlss_debug);
  if (debug == 2) return;

  if (!dlss_) dlss_.reset(new DlssState);
  DlssState& s = *dlss_;
  if (s.failed) return;
  const uint32_t w = uint32_t(desc.Width), h = desc.Height;
  // The output: the scene's own size (DLAA) or the output resolution.
  const bool upscale = DlssSceneRatio() < 1.0f;
  const uint32_t out_w = upscale ? HostPx(src.width, render_scale_) : w;
  const uint32_t out_h = upscale ? HostPx(src.height, render_scale_) : h;
  // Textures pick their mips for the output resolution, not the smaller scene (sr_texture_mip_bias).
  const float bias = upscale && out_w > 0 ? std::log2(float(w) / float(out_w)) : 0.0f;
  if (bias != g_dlss_mip_bias) REXLOG_INFO("dlss: texture mip bias {:.3f} (scene {} -> {})", bias, w, out_w);
  g_dlss_mip_bias = bias;

  if (!s.initialized) {
    wchar_t module[MAX_PATH];
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    const std::wstring dir = std::filesystem::path(module).parent_path().wstring();
    // NGX's own log lines about the model (preset, version) go to our log, so
    // which DLSS model really runs is on record.
    NVSDK_NGX_FeatureCommonInfo info{};
    info.LoggingInfo.LoggingCallback = [](const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
      std::string_view m(message ? message : "");
      if (m.find("reset") != m.npos || m.find("ersion") != m.npos || m.find("odel") != m.npos) {
        while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.remove_suffix(1);
        REXLOG_INFO("dlss: ngx: {}", m);
      }
    };
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(
        "6a0f5b5e-8a5c-4f8e-9d4c-5f2e1c3b7a90", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dir.c_str(), device_, &info);
    if (NVSDK_NGX_FAILED(r)) {
      REXLOG_ERROR("dlss: NGX init failed ({:08X}); is nvngx_dlss.dll next to the executable?", uint32_t(r));
      s.failed = true;
      ::g_dlss_hardware = false;  // greyed out in Graphics Options from now on
      return;
    }
    s.initialized = true;
    int available = 0;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&s.params)) ||
        NVSDK_NGX_FAILED(s.params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available)) ||
        !available) {
      REXLOG_ERROR("dlss: DLSS is not available on this GPU / driver");
      s.failed = true;
      ::g_dlss_hardware = false;
      return;
    }
    REXLOG_INFO("dlss: NGX initialized, DLSS available");
  }

  // The resources and the feature for this size and mode.
  if (!s.feature || s.width != w || s.height != h || s.out_width != out_w || s.mode != mode) {
    if (s.feature) {
      NVSDK_NGX_D3D12_ReleaseFeature(s.feature);
      s.feature = nullptr;
    }
    Retire(std::move(s.motion));
    Retire(std::move(s.output));
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    s.motion_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, s.motion_state, nullptr,
                                                IID_PPV_ARGS(&s.motion)))) {
      s.failed = true;
      return;
    }
    td.Format = src.view_format;
    td.Width = out_w;
    td.Height = out_h;
    s.output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, s.output_state, nullptr,
                                                IID_PPV_ARGS(&s.output)))) {
      s.failed = true;
      return;
    }
    if (!s.motion_uav) s.motion_uav = AllocSrvIndex();
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R16G16_FLOAT;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(
        s.motion.Get(), nullptr, &uav,
        provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(), s.motion_uav));
    // For sr_dlss_debug=1: the motion readable, the output writable.
    if (!s.motion_srv) s.motion_srv = AllocSrvIndex();
    if (!s.output_uav) s.output_uav = AllocSrvIndex();
    D3D12_SHADER_RESOURCE_VIEW_DESC msrv{};
    msrv.Format = DXGI_FORMAT_R16G16_FLOAT;
    msrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    msrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    msrv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(
        s.motion.Get(), &msrv,
        provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(), s.motion_srv));
    D3D12_UNORDERED_ACCESS_VIEW_DESC ouav{};
    ouav.Format = src.view_format;
    ouav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(
        s.output.Get(), nullptr, &ouav,
        provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(), s.output_uav));

    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = w;
    create.Feature.InHeight = h;
    create.Feature.InTargetWidth = out_w;
    create.Feature.InTargetHeight = out_h;
    create.Feature.InPerfQualityValue = kQuality[mode];
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                                  NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                                  NVSDK_NGX_DLSS_Feature_Flags_AlphaUpscaling;
    // The model: an explicit preset for every quality mode, so the transformer
    // model is the one used whatever the driver's default is.
    const int chosen = REXCVAR_GET(sr_dlss_preset);
    const struct { const char* hint; unsigned recommended; } hints[] = {
        {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_DLSS_Hint_Render_Preset_K},
        {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, NVSDK_NGX_DLSS_Hint_Render_Preset_K},
        {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, NVSDK_NGX_DLSS_Hint_Render_Preset_K},
        {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_DLSS_Hint_Render_Preset_K},
        {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, NVSDK_NGX_DLSS_Hint_Render_Preset_M},
        {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_DLSS_Hint_Render_Preset_L}};
    for (const auto& h : hints)
      NVSDK_NGX_Parameter_SetUI(s.params, h.hint, chosen < 0 ? h.recommended : unsigned(chosen));
    // The preset of this mode (sr_dlss 1..4 = DLAA, Quality, Balanced, Performance).
    const unsigned preset = chosen >= 0 ? unsigned(chosen) : mode == 4 ? 13u : 11u;
    NVSDK_NGX_Result r =
        NGX_D3D12_CREATE_DLSS_EXT(command_list_.Get(), 1, 1, &s.feature, s.params, &create);
    if (NVSDK_NGX_FAILED(r)) {
      REXLOG_ERROR("dlss: feature creation failed ({:08X})", uint32_t(r));
      s.failed = true;
      return;
    }
    s.width = w;
    s.height = h;
    s.out_width = out_w;
    s.mode = mode;
    s.reset = true;
    REXLOG_INFO("dlss: mode {} scene {}x{} -> {}x{} (scene format {}), preset {} ({})", mode, w, h, out_w, out_h,
                uint32_t(src.view_format), preset ? char('A' + preset - 1) : '-',
                preset >= 10 ? "transformer" : preset ? "other" : "library default");
  }

  // Motion vectors from the depth and the two cameras.
  if (!s.mv_root) {
    // t0 depth, u0 motion, t1 objects' motion, constants.
    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = 1;
    ranges[2].BaseShaderRegister = 1;
    D3D12_ROOT_PARAMETER params[4] = {};
    for (int i = 0; i < 3; ++i) {
      params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[i].DescriptorTable.NumDescriptorRanges = 1;
      params[i].DescriptorTable.pDescriptorRanges = &ranges[i];
    }
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[3].Constants.Num32BitValues = 36;
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 4;
    rd.pParameters = params;
    Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                            IID_PPV_ARGS(&s.mv_root)))) {
      s.failed = true;
      return;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = s.mv_root.Get();
    pd.CS = {kMotionVectorsCS, sizeof(kMotionVectorsCS)};
    if (FAILED(device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&s.mv_pso)))) {
      s.failed = true;
      return;
    }
  }
  float constants[36] = {};
  const bool cameras = dlss_camera_ && dlss_prev_camera_ && Invert4x4(dlss_view_proj_, constants);
  if (cameras) {
    std::memcpy(constants + 16, dlss_prev_view_proj_, 64);
  } else {
    // No camera this frame or the last: no motion, and DLSS starts over.
    float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::memcpy(constants, identity, 64);
    std::memcpy(constants + 16, identity, 64);
    s.reset = true;
  }
  constants[32] = float(w);
  constants[33] = float(h);
  constants[34] = dlss_jitter_[0];
  constants[35] = dlss_jitter_[1];
  HostSurface& depth = *dlss_scene_depth_;
  // The objects' motion (cleared to "use the camera" if no draw wrote it).
  if (!DlssMotionTarget()) {
    s.failed = true;
    return;
  }
  Transition(s.objects.Get(), s.objects_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(depth.resource.Get(), depth.state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(s.motion.Get(), s.motion_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), sampler_heap_.Get()};
  command_list_->SetDescriptorHeaps(2, heaps);
  command_list_->SetComputeRootSignature(s.mv_root.Get());
  const D3D12_GPU_DESCRIPTOR_HANDLE gpu = srv_heap_->GetGPUDescriptorHandleForHeapStart();
  command_list_->SetComputeRootDescriptorTable(0, provider_->OffsetViewDescriptor(gpu, depth.srv_index));
  command_list_->SetComputeRootDescriptorTable(1, provider_->OffsetViewDescriptor(gpu, s.motion_uav));
  command_list_->SetComputeRootDescriptorTable(2, provider_->OffsetViewDescriptor(gpu, s.objects_srv));
  command_list_->SetComputeRoot32BitConstants(3, 36, constants, 0);
  command_list_->SetPipelineState(s.mv_pso.Get());
  command_list_->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
  D3D12_RESOURCE_BARRIER uav_barrier{};
  uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uav_barrier.UAV.pResource = s.motion.Get();
  command_list_->ResourceBarrier(1, &uav_barrier);

  // DLSS: the scene into the output, then back into the scene surface.
  Transition(src.resource.Get(), src.state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(s.motion.Get(), s.motion_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(s.output.Get(), s.output_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (debug == 1) {
    // The motion vectors in place of DLSS's picture.
    if (!s.debug_pso) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
      pd.pRootSignature = s.mv_root.Get();
      pd.CS = {kMotionDebugCS, sizeof(kMotionDebugCS)};
      device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&s.debug_pso));
    }
    if (s.debug_pso) {
      command_list_->SetComputeRootSignature(s.mv_root.Get());
      command_list_->SetComputeRootDescriptorTable(0, provider_->OffsetViewDescriptor(gpu, s.motion_srv));
      command_list_->SetComputeRootDescriptorTable(1, provider_->OffsetViewDescriptor(gpu, s.output_uav));
      command_list_->SetComputeRootDescriptorTable(2, provider_->OffsetViewDescriptor(gpu, s.objects_srv));
      command_list_->SetComputeRoot32BitConstants(3, 36, constants, 0);
      command_list_->SetPipelineState(s.debug_pso.Get());
      command_list_->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
      Transition(s.output.Get(), s.output_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      if (upscale) {
        dlss_resolve_source_ = s.output.Get();
      } else {
        Transition(src.resource.Get(), src.state, D3D12_RESOURCE_STATE_COPY_DEST);
        command_list_->CopyResource(src.resource.Get(), s.output.Get());
      }
    }
    frame_state_bound_ = false;
    bound_rt_count_ = 0;
    bound_ds_ = nullptr;
    std::memset(bound_rts_, 0, sizeof(bound_rts_));
    return;
  }
  const float jitter_scale = float(REXCVAR_GET(sr_dlss_jitter_scale));
  NVSDK_NGX_D3D12_DLSS_Eval_Params eval{};
  eval.Feature.pInColor = src.resource.Get();
  eval.Feature.pInOutput = s.output.Get();
  eval.pInDepth = depth.resource.Get();
  eval.pInMotionVectors = s.motion.Get();
  eval.InJitterOffsetX = dlss_jitter_[0] * jitter_scale;
  eval.InJitterOffsetY = dlss_jitter_[1] * jitter_scale;
  eval.InRenderSubrectDimensions = {w, h};
  eval.InReset = s.reset ? 1 : 0;
  eval.InMVScaleX = 1.0f;
  eval.InMVScaleY = 1.0f;
  NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(command_list_.Get(), s.feature, s.params, &eval);
  if (NVSDK_NGX_FAILED(r)) {
    static int logged = 0;
    if (logged++ < 4) REXLOG_ERROR("dlss: evaluate failed ({:08X})", uint32_t(r));
  } else {
    s.reset = false;
    Transition(s.output.Get(), s.output_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (upscale) {
      // Larger than the surface: the resolve copies it into its texture.
      dlss_resolve_source_ = s.output.Get();
    } else {
      Transition(src.resource.Get(), src.state, D3D12_RESOURCE_STATE_COPY_DEST);
      command_list_->CopyResource(src.resource.Get(), s.output.Get());
    }
  }
  // DLSS and the motion pass changed the root signature, heaps and pipeline.
  frame_state_bound_ = false;
  bound_rt_count_ = 0;
  bound_ds_ = nullptr;
  std::memset(bound_rts_, 0, sizeof(bound_rts_));
}

void Renderer::DlssEndFrame() {
  std::swap(dlss_prev_store_, dlss_cur_store_);
  dlss_const_arena_.EndFrame();
  dlss_prev_store_.Sort();
  dlss_cur_store_.Clear();
  dlss_occurrence_.clear();
  if (REXCVAR_GET(sr_dlss_debug) && dlss_frame_ % 300 == 0 && g_motion_stats.matched + g_motion_stats.unmatched) {
    std::string top;
    for (const auto& [vs, n] : g_motion_stats.unmatched_vs) top += fmt::format(" {:016X}:{}", vs, n);
    std::string matched;
    for (const auto& [vs, n] : g_motion_stats.matched_vs) matched += fmt::format(" {:016X}:{}", vs, n);
    REXLOG_INFO("dlss: scene draws with a previous frame {}, rebuilt meshes {}, without {} (vs:count{}); "
                "with, by vs:{}",
                g_motion_stats.matched, g_motion_stats.rebuilt, g_motion_stats.unmatched, top, matched);
    g_motion_stats = {};
  }
  if (dlss_camera_) std::memcpy(dlss_prev_view_proj_, dlss_view_proj_, sizeof(dlss_view_proj_));
  dlss_prev_camera_ = dlss_camera_;
  dlss_camera_ = false;
  if (!dlss_scene_resolves_) dlss_scene_ = nullptr;  // no scene this frame (menus, loading)
  dlss_scene_resolves_prev_ = dlss_scene_resolves_;
  dlss_scene_resolves_ = 0;
  dlss_scene_done_ = false;
  dlss_depth_just_resolved_ = false;
  dlss_scene_depth_ = nullptr;
  ++dlss_frame_;
  // NVIDIA: at least 8 * (output / render)^2 phases.
  const float ratio = DlssSceneRatio();
  const uint32_t phases = uint32_t(std::ceil(8.0f / (ratio * ratio)));
  const uint32_t phase = dlss_frame_ % phases + 1;
  dlss_jitter_[0] = SrDlssMode() ? Halton(phase, 2) - 0.5f : 0.0f;
  dlss_jitter_[1] = SrDlssMode() ? Halton(phase, 3) - 0.5f : 0.0f;
}

}  // namespace superman_returns::native
