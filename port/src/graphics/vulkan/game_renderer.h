#pragma once
#include "game_pipeline.h"
#include "depth_resolve.h"
#include "state_shadow.h"
#include "constant_bank_cache.h"
#include "gpu_pass_profiler.h"
#include "../guest/texture_capture.h"
#include <atomic>
#include <chrono>
namespace superman_returns::graphics::vulkan {
using ShaderLookup=std::function<shaders::ShaderResult(const guest::ShaderCapture&)>;
using TextureDecoder=std::function<bool(const guest::TextureCapture&,guest::LinearTexture&,std::string&)>;
// alias_violations: draws that found alias work on an already open pass (SR_VULKAN_CHECK_ALIASES=1 only).
struct GameRenderStats {uint64_t draws=0,pending=0,failed=0,clears=0,alias_violations=0;};
// SR_VULKAN_PROFILE=1: CPU time of each recording phase, summed until read.
struct RecordProfile {
  enum Phase {kUploads,kTargets,kTextures,kBindings,kDescriptors,kPipeline,kCommands,kResolves,kClears,kCount};
  std::array<uint64_t,kCount> ns{};uint64_t draws=0,texture_uploads=0,texture_bytes=0,buffer_uploads=0,buffer_bytes=0;
  uint64_t passes=0,resolve_packets=0,clear_packets=0;  // render passes begun for draws, resolve and clear packets
};
class GameRenderer {
public:
  GameRenderer(Context& c,ShaderLookup shaders,TextureDecoder decoder={},AliasOptions aliases={}):c_(c),shaders_(std::move(shaders)),decoder_(std::move(decoder)),state_(c.f),resources_(c),targets_(c,state_),descriptors_(c),pipelines_(c),depth_resolver_(c),alias_options_(aliases),shadow_(StateFilterEnabled()) {
    // Any barrier on the recording command buffer ends the open render pass.
    state_.before_barrier=[this](VkCommandBuffer command) {if(command && command==recording_.load(std::memory_order_relaxed)) ClosePass();};
  }
  bool Initialize(const std::filesystem::path& driver_cache,Error&);
  // With an upload command buffer (submitted before `command`), uploads are
  // hoisted there and consecutive draws to the same targets share one render
  // pass. Without it each draw records its own pass, as before.
  bool BeginSubmission(VkCommandBuffer,uint64_t serial,Error&,VkCommandBuffer upload=VK_NULL_HANDLE);
  // Ends the open render pass and writes the shared upload barrier.
  void FinishSubmission();
  bool Record(const guest::RenderPacket&,VkCommandBuffer,Error&);
  // Selection alone does not submit or acknowledge a guest presentation.
  std::shared_ptr<TextureResource> SelectFrontbuffer(const guest::SwapPacket&,Error&) const;
  void Retire(uint64_t serial);
  TargetStore& Targets() {return targets_;}
  ImageState& Images() {return state_;}
  ResourceStore& Resources() {return resources_;}
  const GameRenderStats& Stats() const {return stats_;}
  const GamePipelineStore::Stats& PipelineStats() const {return pipelines_.stats();}
  uint64_t ResolveSetsCreated() const {return depth_resolver_.CreatedSets();}  // since start: stays flat once warm
  void SetGpuPasses(GpuPassProfiler* profiler) {gpu_passes_=profiler;}  // SR_GPU_PASSES, GameFrame's
  bool CheckpointCache(Error& e) {return pipelines_.CheckpointCache(e);}
  void EnableProfile(bool on) {profiling_=on;}
  RecordProfile TakeProfile() {auto p=profile_;profile_={};return p;}
  DescriptorStore::CacheStats TakeDescriptorStats() {return descriptors_.TakeCacheStats();}
  // Diagnostics (SR_VULKAN_DUMP_FRAME): while a dump is active, Record logs
  // every packet and reads back each resolve destination when it is written.
  struct DumpImage {std::string name;std::shared_ptr<BufferResource> buffer;VkFormat format=VK_FORMAT_UNDEFINED;VkExtent3D extent{};uint32_t texel=0;};
  void BeginDump() {dumping_=true;dump_log_.clear();dump_images_.clear();dump_packet_=0;}
  bool DumpTargets(VkCommandBuffer,Error&);
  bool WriteDump(const std::filesystem::path&,Error&);
private:
  bool Draw(const guest::DrawPacket&,VkCommandBuffer,Error&);
  void ClosePass();
  bool DumpImageNow(VkCommandBuffer,const std::string& name,VkImage,VkFormat,VkExtent3D,VkImageAspectFlags,bool target,Error&);
  bool dumping_=false;uint64_t dump_packet_=0;std::vector<std::string> dump_log_;std::vector<DumpImage> dump_images_;
  // A vertex update with a capture view (guest::BufferUpdate::view) is swapped with its stream's attributes.
  bool Upload(const guest::BufferUpdate&,bool indices,uint64_t version,Error&,uint32_t stream=0,std::span<const guest::VertexAttribute> attributes={});
  // The shared index buffer of non-indexed quad lists (0,1,2,0,2,3, then +4 per quad), drawn at the quad list's
  // first vertex as the vertex offset; grown to at least `quads` quads.
  bool QuadIndices(uint32_t quads,Error&);
  // A capture's final (ready or failed) shader result, by reference: the lookup copied a ShaderResult (a
  // shared_ptr and a string) through a std::function twice per draw (mobile renderer plan, E3). Captures
  // are long-lived registry objects; pointer, hash and size must match, as in GameFrame's MemoizeFinal.
  // A pending result is returned in `scratch` and asked again next time.
  const shaders::ShaderResult& ShaderFor(const guest::ShaderCapture&,shaders::ShaderResult& scratch);
  struct FinalShader {uint64_t hash=0;size_t size=0;shaders::ShaderResult result;};
  std::unordered_map<const guest::ShaderCapture*,FinalShader> final_shaders_;
  static bool StateFilterEnabled();
  static bool AliasCheckEnabled();
  static bool ConstantBankCacheEnabled();
  bool constant_bank_cache_enabled_=ConstantBankCacheEnabled();
  ConstantBankCache constant_bank_cache_;
  Context& c_;ShaderLookup shaders_;TextureDecoder decoder_;ImageState state_;ResourceStore resources_;TargetStore targets_;DescriptorStore descriptors_;GamePipelineStore pipelines_;
  DepthResolver depth_resolver_;bool depth_ready_=false;
  GpuPassProfiler* gpu_passes_=nullptr;
  AliasOptions alias_options_;
  std::map<guest::ResourceId,std::vector<uint8_t>> cpu_buffers_;
  struct ResolvedBinding {std::shared_ptr<TextureResource> image,raw;bool swap=false;uint64_t generation=0;};
  std::map<uint32_t,ResolvedBinding> resolved_latest_;
  // View of a resolved image: rebuilt only when the image or swizzle changes.
  // Resolves update the image in place, so a new generation needs no new view
  // (a new view object per frame defeated the descriptor cache). The view owns
  // its image, so the recorded owner pointer cannot be reused while it lives.
  struct ResolveView {const TextureResource* owner=nullptr;VkComponentMapping mapping{};};
  std::map<guest::ResourceId,ResolveView> resolve_views_;
  uint64_t resolve_generation_=0;
  VkCommandBuffer command_=VK_NULL_HANDLE;uint64_t serial_=0;bool dummies_=false;
  GameRenderStats stats_;
  std::map<uint64_t,std::vector<guest::ResourceId>> transient_buffers_;
  VkExtent2D tiling_{};
  std::shared_ptr<TargetPass> open_pass_;VkPipeline bound_pipeline_=VK_NULL_HANDLE;bool merge_passes_=false;
  // Last value emitted of each redundant command; invalidated wherever bound_pipeline_ is reset.
  StateShadow shadow_;
  // SR_VULKAN_CHECK_ALIASES=1 (read once, at construction): plan aliases on every draw, even on an open pass.
  bool check_aliases_=AliasCheckEnabled();
  std::atomic<VkCommandBuffer> recording_{VK_NULL_HANDLE};
  // Declared after resources_, so released before the store that made it.
  std::shared_ptr<BufferResource> quad_indices_;uint32_t quad_capacity_=0;
  std::vector<uint8_t> rect_scratch_;  // expanded rectangle vertices, reused across draws
  bool profiling_=false;RecordProfile profile_;
  // Adds the time since the previous lap to a phase, in nanoseconds (a lap per phase per draw would lose ~0.5 us
  // each to truncation); free when profiling is off.
  struct Lap {
    bool on;RecordProfile& p;std::chrono::steady_clock::time_point t;
    Lap(bool enabled,RecordProfile& profile):on(enabled),p(profile),t(enabled?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{}) {}
    void operator()(RecordProfile::Phase phase) {if(!on) return;auto now=std::chrono::steady_clock::now();p.ns[phase]+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now-t).count());t=now;}
  };
};
}
