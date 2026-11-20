#pragma once
#include "resolve.h"
namespace superman_returns::graphics::vulkan {
struct AliasOptions {bool hdr_from_ldr_black=true;float clear_alpha=0;};
class DepthResolver {
public:
  explicit DepthResolver(Context& c):c_(c) {}
  ~DepthResolver();
  bool Initialize(Error&);
  bool Record(VkCommandBuffer,const ResolvePlan&,guest::ResourceId raw_destination,TargetStore&,ResourceStore&,ImageState&,Error&);
  bool RecordAlias(VkCommandBuffer,TargetId,const AliasPlan&,TargetStore&,ResourceStore&,ImageState&,Error&,AliasOptions={});
  void Retire(uint64_t serial);
  uint64_t CreatedSets() const {return created_sets_;}
private:
  // A descriptor set (and the stencil view a depth resolve reads) per source target and scratch buffer,
  // created once and reused: a pool, a set and a view per resolve were Vulkan objects made and freed in
  // every frame (mobile renderer plan, rule 7). The set is never rewritten, so reusing it while an
  // earlier submission still reads it is safe. The scratch is tracked weakly: holding it would keep
  // ResourceStore's pool from reusing the buffer.
  struct CachedSet {
    std::shared_ptr<TargetResource> source;std::weak_ptr<BufferResource> scratch;const BufferResource* scratch_key=nullptr;
    VkDeviceSize range=0;bool depth=false;
    VkImageView stencil=VK_NULL_HANDLE;VkDescriptorPool pool=VK_NULL_HANDLE;VkDescriptorSet set=VK_NULL_HANDLE;uint64_t last_serial=0;
  };
  VkDescriptorSet CachedDescriptors(bool depth,const std::shared_ptr<TargetResource>& source,const std::shared_ptr<BufferResource>& scratch,VkDeviceSize range,uint64_t serial,Error&);
  void Destroy(CachedSet&);
  Context& c_;VkDescriptorSetLayout descriptors_=VK_NULL_HANDLE;VkPipelineLayout layout_=VK_NULL_HANDLE;VkPipeline pipeline_=VK_NULL_HANDLE,alias_pipeline_=VK_NULL_HANDLE;SubmissionResources pending_;
  std::vector<CachedSet> sets_;uint64_t created_sets_=0;
};
}
