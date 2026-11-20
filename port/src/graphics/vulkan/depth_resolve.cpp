#include "depth_resolve.h"
#include "depth_resolve_shader.h"
#include "edram_alias_shader.h"
#include <algorithm>
namespace superman_returns::graphics::vulkan {
namespace {
struct DepthJob {
  Context* context=nullptr;VkDescriptorPool pool=VK_NULL_HANDLE;VkImageView stencil=VK_NULL_HANDLE;
  std::shared_ptr<TargetResource> source,destination;std::shared_ptr<BufferResource> scratch;
  std::shared_ptr<TextureResource> depth,raw;
  ~DepthJob() {if(pool) context->f.vkDestroyDescriptorPool(context->device,pool,nullptr);if(stencil) context->f.vkDestroyImageView(context->device,stencil,nullptr);}
};
bool Fail(Error& e,const char* message) {e={"Depth resolve",VK_ERROR_INITIALIZATION_FAILED,message};return false;}
}
void DepthResolver::Destroy(CachedSet& s) {
  if(s.pool) c_.f.vkDestroyDescriptorPool(c_.device,s.pool,nullptr);
  if(s.stencil) c_.f.vkDestroyImageView(c_.device,s.stencil,nullptr);
  s={};
}
void DepthResolver::Retire(uint64_t serial) {
  pending_.Retire(serial);
  // Drop the sets of targets only this cache keeps and of scratch buffers gone, once no submission reads them.
  std::erase_if(sets_,[&](CachedSet& s) {
    if(s.last_serial>serial || (s.source.use_count()>1 && !s.scratch.expired())) return false;
    Destroy(s);return true;
  });
}
VkDescriptorSet DepthResolver::CachedDescriptors(bool depth,const std::shared_ptr<TargetResource>& source,const std::shared_ptr<BufferResource>& scratch,VkDeviceSize range,uint64_t serial,Error& e) {
  for(auto& s:sets_)
    if(s.depth==depth && s.source==source && s.scratch_key==scratch.get() && s.range==range && s.scratch.lock()==scratch) {s.last_serial=serial;return s.set;}
  CachedSet s;s.source=source;s.scratch=scratch;s.scratch_key=scratch.get();s.range=range;s.depth=depth;s.last_serial=serial;
  auto fail=[&]{Destroy(s);return VkDescriptorSet(VK_NULL_HANDLE);};
  if(depth) {
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=source->image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=source->format;view.subresourceRange={VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
    if(!Check(c_.f.vkCreateImageView(c_.device,&view,nullptr,&s.stencil),"Stencil resolve view",e)) return fail();
  }
  VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=1;pool.poolSizeCount=2;pool.pPoolSizes=sizes;
  if(!Check(c_.f.vkCreateDescriptorPool(c_.device,&pool,nullptr,&s.pool),depth?"Depth resolve descriptor pool":"Alias descriptor pool",e)) return fail();
  VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocate.descriptorPool=s.pool;allocate.descriptorSetCount=1;allocate.pSetLayouts=&descriptors_;
  if(!Check(c_.f.vkAllocateDescriptorSets(c_.device,&allocate,&s.set),depth?"Depth resolve descriptors":"Alias descriptor set",e)) return fail();
  const VkImageLayout layout=depth?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkDescriptorImageInfo images[]{{VK_NULL_HANDLE,source->sampled,layout},{VK_NULL_HANDLE,s.stencil,layout}};
  VkDescriptorBufferInfo buffer{scratch->handle,0,range};VkWriteDescriptorSet writes[3]{};uint32_t count=0;
  for(uint32_t binding=0;binding<3;++binding) {
    if(binding==1 && !depth) continue;  // the alias copy reads one image
    auto& w=writes[count++];w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=s.set;w.dstBinding=binding;w.descriptorCount=1;
    w.descriptorType=binding==2?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    if(binding==2) w.pBufferInfo=&buffer;else w.pImageInfo=&images[binding];
  }
  c_.f.vkUpdateDescriptorSets(c_.device,count,writes,0,nullptr);
  ++created_sets_;sets_.push_back(std::move(s));return sets_.back().set;
}
DepthResolver::~DepthResolver() {
  if(!c_.device) return;c_.f.vkDeviceWaitIdle(c_.device);pending_.Retire(UINT64_MAX);
  for(auto& s:sets_) Destroy(s);sets_.clear();
  if(pipeline_) c_.f.vkDestroyPipeline(c_.device,pipeline_,nullptr);
  if(alias_pipeline_) c_.f.vkDestroyPipeline(c_.device,alias_pipeline_,nullptr);
  if(layout_) c_.f.vkDestroyPipelineLayout(c_.device,layout_,nullptr);
  if(descriptors_) c_.f.vkDestroyDescriptorSetLayout(c_.device,descriptors_,nullptr);
}
bool DepthResolver::Initialize(Error& e) {
  if(layout_ || descriptors_) return Fail(e,"Helper already initialized");
  VkDescriptorSetLayoutBinding bindings[]{
    {0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
    {1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
    {2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
  VkDescriptorSetLayoutCreateInfo d{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};d.bindingCount=3;d.pBindings=bindings;
  if(!Check(c_.f.vkCreateDescriptorSetLayout(c_.device,&d,nullptr,&descriptors_),"Depth resolve descriptor layout",e)) return false;
  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,16};VkPipelineLayoutCreateInfo l{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};l.setLayoutCount=1;l.pSetLayouts=&descriptors_;l.pushConstantRangeCount=1;l.pPushConstantRanges=&push;
  if(!Check(c_.f.vkCreatePipelineLayout(c_.device,&l,nullptr,&layout_),"Depth resolve pipeline layout",e)) return false;
  VkShaderModule module{};VkShaderModuleCreateInfo s{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};s.codeSize=sizeof(depth_resolve_shader);s.pCode=depth_resolve_shader;
  if(!Check(c_.f.vkCreateShaderModule(c_.device,&s,nullptr,&module),"Depth resolve shader",e)) return false;
  VkComputePipelineCreateInfo p{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};p.layout=layout_;p.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};p.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;p.stage.module=module;p.stage.pName="CSMain";
  auto result=c_.f.vkCreateComputePipelines(c_.device,VK_NULL_HANDLE,1,&p,nullptr,&pipeline_);c_.f.vkDestroyShaderModule(c_.device,module,nullptr);
  if(!Check(result,"Depth resolve pipeline",e)) return false;
  s.codeSize=sizeof(edram_alias_shader);s.pCode=edram_alias_shader;
  if(!Check(c_.f.vkCreateShaderModule(c_.device,&s,nullptr,&module),"EDRAM alias shader",e)) return false;
  p.stage.module=module;result=c_.f.vkCreateComputePipelines(c_.device,VK_NULL_HANDLE,1,&p,nullptr,&alias_pipeline_);c_.f.vkDestroyShaderModule(c_.device,module,nullptr);
  return Check(result,"EDRAM alias pipeline",e);
}
bool DepthResolver::RecordAlias(VkCommandBuffer command,TargetId destination,const AliasPlan& plan,TargetStore& targets,ResourceStore& resources,ImageState& state,Error& e,AliasOptions options) {
  if(plan.action==AliasAction::kNone) {e={};return true;}
  if(!alias_pipeline_ || !command) return Fail(e,"Alias helper is not initialized");
  auto dst=targets.Get(destination,e);if(!dst) return false;
  if(plan.action==AliasAction::kClear) {
    VkImageSubresourceRange range{dst->aspects,0,1,0,1};if(!state.Transition(command,dst->image,range,ImageUsage::TransferDestination(),e)) return false;
    if(dst->description.depth) {VkClearDepthStencilValue value{1,0};c_.f.vkCmdClearDepthStencilImage(command,dst->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&range);}
    else {VkClearColorValue value{};value.float32[3]=options.clear_alpha;c_.f.vkCmdClearColorImage(command,dst->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&range);}
    if(!state.Transition(command,dst->image,range,dst->description.depth?ImageUsage::DepthAttachment():ImageUsage::ColorAttachment(),e)) return false;
    targets.MarkWritten(destination);e={};return true;
  }
  auto src=targets.Get(plan.source,e);if(!src) return false;
  if(options.hdr_from_ldr_black && !src->description.depth && !dst->description.depth && src->description.geometry.format==0 && dst->description.geometry.format==12) {
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};if(!state.Transition(command,dst->image,range,ImageUsage::TransferDestination(),e)) return false;
    VkClearColorValue value{};value.float32[3]=1;c_.f.vkCmdClearColorImage(command,dst->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&range);
    if(!state.Transition(command,dst->image,range,ImageUsage::ColorAttachment(),e)) return false;
    targets.MarkWritten(destination);e={};return true;
  }
  auto kind=[](uint32_t f)->int {switch(f) {case 0:case 1:return 0;case 2:case 10:return 1;case 3:case 12:return 2;default:return -1;}};
  int source_kind=kind(src->description.geometry.format),destination_kind=kind(dst->description.geometry.format);
  auto& g=dst->description.geometry;
  if(src==dst || src->description.depth || dst->description.depth || source_kind<0 || destination_kind<0 || src->description.geometry.width!=g.width || src->description.geometry.height!=g.height) {
    if(plan.newest_other) {
      VkImageSubresourceRange range{dst->aspects,0,1,0,1};if(!state.Transition(command,dst->image,range,ImageUsage::TransferDestination(),e)) return false;
      if(dst->description.depth) {VkClearDepthStencilValue value{1,0};c_.f.vkCmdClearDepthStencilImage(command,dst->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&range);}
      else {VkClearColorValue value{};value.float32[3]=options.clear_alpha;c_.f.vkCmdClearColorImage(command,dst->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&range);}
      if(!state.Transition(command,dst->image,range,dst->description.depth?ImageUsage::DepthAttachment():ImageUsage::ColorAttachment(),e)) return false;
      targets.MarkWritten(destination);
    }
    e={};return true;
  }
  uint64_t bytes=uint64_t(g.width)*g.height*(destination_kind==2?8:4);
  auto job=std::make_shared<DepthJob>();job->context=&c_;job->source=src;job->destination=dst;job->scratch=resources.ResolveScratch(bytes,e);if(!job->scratch) return false;
  if((g.width+7)/8>c_.properties.limits.maxComputeWorkGroupCount[0] || (g.height+7)/8>c_.properties.limits.maxComputeWorkGroupCount[1]) return Fail(e,"Alias dispatch exceeds device limits");
  const VkDescriptorSet set=CachedDescriptors(false,src,job->scratch,bytes,resources.CurrentSerial(),e);if(!set) return false;
  auto sampled=ImageUsage::Sampled();sampled.stages=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  if(!state.Transition(command,src->image,{src->aspects,0,1,0,1},sampled,e)) return false;
  uint32_t push[]{uint32_t(source_kind),uint32_t(destination_kind),g.width,g.height};
  c_.f.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,alias_pipeline_);c_.f.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&set,0,nullptr);c_.f.vkCmdPushConstants(command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);c_.f.vkCmdDispatch(command,(g.width+7)/8,(g.height+7)/8,1);
  pending_.Keep(resources.CurrentSerial(),job);
  VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=job->scratch->handle;barrier.size=VK_WHOLE_SIZE;
  c_.f.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&barrier,0,nullptr);
  if(!state.Transition(command,dst->image,{dst->aspects,0,1,0,1},ImageUsage::TransferDestination(),e)) return false;
  VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={g.width,g.height,1};c_.f.vkCmdCopyBufferToImage(command,job->scratch->handle,dst->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
  if(!state.Transition(command,dst->image,{dst->aspects,0,1,0,1},ImageUsage::ColorAttachment(),e)) return false;
  targets.MarkWritten(destination);e={};return true;
}
bool DepthResolver::Record(VkCommandBuffer command,const ResolvePlan& plan,guest::ResourceId raw_id,TargetStore& targets,ResourceStore& resources,ImageState& state,Error& e) {
  if(plan.empty) {e={};return true;}
  if(!pipeline_ || !command || !resources.CurrentSerial() || plan.operation!=ResolveOperation::kDepthConversion || plan.destination_format!=VK_FORMAT_R32_SFLOAT || raw_id==plan.destination) return Fail(e,"Invalid depth resolve operation or destinations");
  uint64_t pixels=uint64_t(plan.extent.width)*plan.extent.height;
  if(!pixels || pixels>UINT32_MAX/8 || (plan.extent.width+7)/8>c_.properties.limits.maxComputeWorkGroupCount[0] || (plan.extent.height+7)/8>c_.properties.limits.maxComputeWorkGroupCount[1]) return Fail(e,"Depth resolve dispatch exceeds device limits");
  auto job=std::make_shared<DepthJob>();job->context=&c_;job->source=targets.Get(plan.source,e);if(!job->source) return false;
  if(job->source->format!=VK_FORMAT_D24_UNORM_S8_UINT || !job->source->description.depth) return Fail(e,"Depth source must be D24S8");
  job->depth=resources.ResolveTexture(plan.destination,VK_FORMAT_R32_SFLOAT,plan.destination_size,plan.destination_mips,plan.destination_layers,plan.dimension==3,state,e);if(!job->depth) return false;
  if(raw_id) {job->raw=resources.ResolveTexture(raw_id,VK_FORMAT_R8G8B8A8_UNORM,plan.destination_size,plan.destination_mips,plan.destination_layers,plan.dimension==3,state,e);if(!job->raw) return false;}
  job->scratch=resources.ResolveScratch(pixels*8,e);if(!job->scratch) return false;
  const VkDescriptorSet set=CachedDescriptors(true,job->source,job->scratch,job->scratch->size,resources.CurrentSerial(),e);if(!set) return false;
  auto sampled=ImageUsage::Sampled(true);sampled.stages=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  if(!state.Transition(command,job->source->image,{job->source->aspects,0,1,0,1},sampled,e)) return false;
  uint32_t push[]{uint32_t(plan.source_offset.x),uint32_t(plan.source_offset.y),plan.extent.width,plan.extent.height};
  c_.f.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_);c_.f.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout_,0,1,&set,0,nullptr);c_.f.vkCmdPushConstants(command,layout_,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);c_.f.vkCmdDispatch(command,(plan.extent.width+7)/8,(plan.extent.height+7)/8,1);
  // Keep every API object alive even if a subsequent transition reports an error.
  pending_.Keep(resources.CurrentSerial(),job);
  VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=job->scratch->handle;barrier.size=VK_WHOLE_SIZE;
  c_.f.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&barrier,0,nullptr);
  std::shared_ptr<TextureResource> outputs[]{job->depth,job->raw};
  for(uint32_t i=0;i<2;++i) if(auto texture=outputs[i]) {
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,plan.destination_mip,1,plan.destination_layer,1};
    if(!state.Transition(command,texture->handle,range,ImageUsage::TransferDestination(),e)) return false;
    VkBufferImageCopy copy{};copy.bufferOffset=i*pixels*4;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,plan.destination_mip,plan.destination_layer,1};copy.imageOffset=plan.destination_offset;copy.imageExtent=plan.extent;
    c_.f.vkCmdCopyBufferToImage(command,job->scratch->handle,texture->handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    if(!state.Transition(command,texture->handle,range,ImageUsage::Sampled(),e)) return false;
  }
  e={};return true;
}
}
