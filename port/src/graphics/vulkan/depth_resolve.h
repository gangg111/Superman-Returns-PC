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
  // A colour copy resolve multiplied by `scale` (2^copy_dest_exp_bias) into an R32G32_SFLOAT texture
  // (shaders/resolve_scale_copy.hlsl). False with e empty when unavailable: the caller copies plainly.
  bool RecordScaledCopy(VkCommandBuffer,const ResolvePlan&,float scale,TargetStore&,ResourceStore&,ImageState&,Error&);
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
  // The depth conversion straight into the two resolve textures (shaders/depth_resolve_image.hlsl), when
  // the device can store to both formats; SR_DEPTH_RESOLVE_IMAGE=0 keeps the scratch buffer and copies.
  struct ImageSet {
    std::shared_ptr<TargetResource> source;std::shared_ptr<TextureResource> depth,raw;uint32_t mip=0,layer=0;
    VkImageView stencil=VK_NULL_HANDLE,depth_view=VK_NULL_HANDLE,raw_view=VK_NULL_HANDLE;
    VkDescriptorPool pool=VK_NULL_HANDLE;VkDescriptorSet set=VK_NULL_HANDLE;uint64_t last_serial=0;
  };
  bool InitializeImagePath(Error&);
  VkDescriptorSet ImageDescriptors(const std::shared_ptr<TargetResource>& source,const std::shared_ptr<TextureResource>& depth,const std::shared_ptr<TextureResource>& raw,uint32_t mip,uint32_t layer,uint64_t serial,Error&);
  void Destroy(ImageSet&);
  bool RecordToImages(VkCommandBuffer,const ResolvePlan&,guest::ResourceId raw_id,TargetStore&,ResourceStore&,ImageState&,Error&);
  Context& c_;VkDescriptorSetLayout descriptors_=VK_NULL_HANDLE;VkPipelineLayout layout_=VK_NULL_HANDLE;VkPipeline pipeline_=VK_NULL_HANDLE,alias_pipeline_=VK_NULL_HANDLE;SubmissionResources pending_;
  std::vector<CachedSet> sets_;uint64_t created_sets_=0;
  VkDescriptorSetLayout image_descriptors_=VK_NULL_HANDLE;VkPipelineLayout image_layout_=VK_NULL_HANDLE;VkPipeline image_pipeline_=VK_NULL_HANDLE;
  std::vector<ImageSet> image_sets_;
  // The scaled copy: sets per source target and destination texture layer (ImageSet with depth = the
  // destination, raw unused, stencil unused).
  VkDescriptorSetLayout scale_descriptors_=VK_NULL_HANDLE;VkPipelineLayout scale_layout_=VK_NULL_HANDLE;VkPipeline scale_pipeline_=VK_NULL_HANDLE;
  std::vector<ImageSet> scale_sets_;
  bool InitializeScaledCopy(Error&);
};
}
