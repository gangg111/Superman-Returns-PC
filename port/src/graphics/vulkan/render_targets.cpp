#include "render_targets.h"
#include <algorithm>
#include <cstring>
namespace superman_returns::graphics::vulkan {
namespace {
VkFormat ColorFormat(uint32_t f) {
  switch(f) {
  case 0:case 1:return VK_FORMAT_R8G8B8A8_UNORM;
  case 2:case 10:return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  case 3:case 5:case 7:case 12:return VK_FORMAT_R16G16B16A16_SFLOAT;
  // k_16_16 stores -32..32 on Xenos: a float target with the clamp in the shader (g_ColorRange), see
  // guest::FixedPointTargetsAsFloat. R16G16_SNORM clipped the sun's shadow map to -1..1.
  case 4:return guest::FixedPointTargetsAsFloat()?VK_FORMAT_R32G32_SFLOAT:VK_FORMAT_R16G16_SNORM;
  case 6:return VK_FORMAT_R16G16_SFLOAT;
  case 14:return VK_FORMAT_R32_SFLOAT;
  case 15:return VK_FORMAT_R32G32_SFLOAT;
  default:return VK_FORMAT_UNDEFINED;
  }
}
bool Fail(Error& e,const char* operation,const char* message) {e={operation,VK_ERROR_INITIALIZATION_FAILED,message};return false;}
guest::SurfaceDesc Canonical(guest::SurfaceDesc d) {
  // Match the reference's default merge of the two Xenos 7e3 RT formats.
  d.geometry.format=d.depth?0xff:(d.geometry.format==3?12:d.geometry.format);
  return d;
}
}
PassPlan PlanAttachments(const guest::DrawPacket& draw,Error& e,VkExtent2D tiling) {
  PassPlan out;
  for(uint32_t i=0;i<4;++i) {
    if(!draw.colors[i].id || (i && !((draw.registers[0x104]>>(i*4))&15))) continue;
    out.colors[i]=Canonical(draw.colors[i]);out.color_count=i+1;
    if(draw.tiling_active) {out.colors[i].geometry.width=std::max(out.colors[i].geometry.width,tiling.width);out.colors[i].geometry.height=std::max(out.colors[i].geometry.height,tiling.height);}
  }
  if(draw.depth.id) {
    out.depth=Canonical(draw.depth);
    if(draw.tiling_active) {out.depth.geometry.width=std::max(out.depth.geometry.width,tiling.width);out.depth.geometry.height=std::max(out.depth.geometry.height,tiling.height);}
    if(out.colors[0].id) {out.depth.geometry.width=std::max(out.depth.geometry.width,out.colors[0].geometry.width);out.depth.geometry.height=std::max(out.depth.geometry.height,out.colors[0].geometry.height);}
  }
  out.extent={UINT32_MAX,UINT32_MAX};
  auto include=[&](const guest::SurfaceDesc& d) {
    if(!d.id) return true;
    if(!d.geometry.width || !d.geometry.height || d.geometry.width>8192 || d.geometry.height>8192) return Fail(e,"Attachment plan","Invalid guest surface dimensions");
    out.extent.width=std::min(out.extent.width,d.geometry.width);out.extent.height=std::min(out.extent.height,d.geometry.height);return true;
  };
  for(auto& color:out.colors) if(!include(color)) return {};
  if(!include(out.depth)) return {};
  if(out.extent.width==UINT32_MAX) {Fail(e,"Attachment plan","Draw has no color or depth target");return {};}
  e={};return out;
}
bool ValidatePassPlan(const PassPlan& plan,Error& e) {
  if(plan.samples!=VK_SAMPLE_COUNT_1_BIT || plan.color_count>4) return Fail(e,"Render pass","Unsupported samples or color slot count");
  VkExtent2D extent{UINT32_MAX,UINT32_MAX};uint32_t count=0;
  auto include=[&](const guest::SurfaceDesc& d,bool depth) {
    if(!d.id) return true;
    if(d.depth!=depth || !d.geometry.width || !d.geometry.height || d.geometry.width>8192 || d.geometry.height>8192) return false;
    extent.width=std::min(extent.width,d.geometry.width);extent.height=std::min(extent.height,d.geometry.height);return true;
  };
  for(uint32_t i=0;i<4;++i) {if(!include(plan.colors[i],false)) return Fail(e,"Render pass","Invalid color description");if(plan.colors[i].id) count=i+1;}
  if(!include(plan.depth,true)) return Fail(e,"Render pass","Invalid depth description");
  if(extent.width==UINT32_MAX || plan.color_count!=count || plan.extent.width!=extent.width || plan.extent.height!=extent.height) return Fail(e,"Render pass","Framebuffer extent/count differs from attachments");
  e={};return true;
}
TargetResource::~TargetResource() {
  if(state && image) state->Forget(image);
  if(sampled) context->f.vkDestroyImageView(context->device,sampled,nullptr);
  if(attachment) context->f.vkDestroyImageView(context->device,attachment,nullptr);
  if(image) context->f.vkDestroyImage(context->device,image,nullptr);
  if(memory) context->f.vkFreeMemory(context->device,memory,nullptr);
}
TargetPass::~TargetPass() {
  if(!owns_handles) return;
  if(framebuffer) context->f.vkDestroyFramebuffer(context->device,framebuffer,nullptr);
  if(render_pass) context->f.vkDestroyRenderPass(context->device,render_pass,nullptr);
}
TargetStore::~TargetStore() {if(c_.device) c_.f.vkDeviceWaitIdle(c_.device);pending_.Retire(UINT64_MAX);passes_.clear();targets_.clear();}
bool TargetStore::BeginSubmission(VkCommandBuffer command,uint64_t serial,Error& e) {
  if(!command || !serial || serial<serial_ || serial<=completed_) return Fail(e,"Target submission","Invalid command buffer or serial");
  command_=command;serial_=serial;e={};return true;
}
TargetId TargetStore::Acquire(const guest::SurfaceDesc& guest_desc,Error& e) {
  auto d=Canonical(guest_desc);auto& g=d.geometry;
  if(!d.id || !g.width || !g.height || g.width>8192 || g.height>8192) {Fail(e,"Render target","Invalid surface description");return 0;}
  Key key{g.edram_base,d.depth?0x8000u:g.format,g.width,g.height,1};
  if(auto found=keys_.find(key);found!=keys_.end()) {guest_targets_[d.id]=found->second;e={};return found->second;}
  auto target=std::make_shared<TargetResource>();target->context=&c_;target->state=&state_;target->description=d;
  target->format=d.depth?VK_FORMAT_D24_UNORM_S8_UINT:ColorFormat(g.format);
  target->aspects=d.depth?(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT):VK_IMAGE_ASPECT_COLOR_BIT;
  if(target->format==VK_FORMAT_UNDEFINED) {e={"Render target format",VK_ERROR_FORMAT_NOT_SUPPORTED,"Unsupported guest color target format "+std::to_string(g.format)};return 0;}
  VkFormatProperties format{};c_.f.vkGetPhysicalDeviceFormatProperties(c_.physical,target->format,&format);
  VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|(d.depth?VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
  if((format.optimalTilingFeatures&needed)!=needed) {e={"Render target format",VK_ERROR_FORMAT_NOT_SUPPORTED,"Required attachment/sample format features unavailable"};return 0;}
  VkImageUsageFlags usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|(d.depth?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  VkImageFormatProperties capabilities{};
  if(!Check(c_.f.vkGetPhysicalDeviceImageFormatProperties(c_.physical,target->format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,usage,0,&capabilities),"Attachment image support",e)) return 0;
  if(g.width>capabilities.maxExtent.width || g.height>capabilities.maxExtent.height || !(capabilities.sampleCounts&VK_SAMPLE_COUNT_1_BIT)) {Fail(e,"Attachment image limits","Surface exceeds device image limits");return 0;}
  VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image.imageType=VK_IMAGE_TYPE_2D;image.format=target->format;image.extent={g.width,g.height,1};image.mipLevels=image.arrayLayers=1;image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;image.usage=usage;image.sharingMode=VK_SHARING_MODE_EXCLUSIVE;image.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
  if(!Check(c_.f.vkCreateImage(c_.device,&image,nullptr,&target->image),"Create attachment image",e)) return 0;
  VkMemoryRequirements requirements{};c_.f.vkGetImageMemoryRequirements(c_.device,target->image,&requirements);
  std::vector<VkMemoryPropertyFlags> types;for(uint32_t i=0;i<c_.memory.memoryTypeCount;++i) types.push_back(c_.memory.memoryTypes[i].propertyFlags);
  auto type=ChooseMemoryType(requirements.memoryTypeBits,types,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if(!type) {Fail(e,"Attachment memory","Device-local memory unavailable");return 0;}
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=*type;
  if(!Check(c_.f.vkAllocateMemory(c_.device,&allocation,nullptr,&target->memory),"Allocate attachment",e) || !Check(c_.f.vkBindImageMemory(c_.device,target->image,target->memory,0),"Bind attachment",e)) return 0;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=target->image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=target->format;view.subresourceRange={target->aspects,0,1,0,1};
  if(!Check(c_.f.vkCreateImageView(c_.device,&view,nullptr,&target->attachment),"Attachment view",e)) return 0;
  view.subresourceRange.aspectMask=d.depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
  if(!Check(c_.f.vkCreateImageView(c_.device,&view,nullptr,&target->sampled),"Attachment sample view",e) || !state_.Register(target->image,1,1,target->aspects,e)) return 0;
  target->id=++next_id_;targets_[target->id]=target;keys_[key]=target->id;guest_targets_[d.id]=target->id;e={};return target->id;
}
std::shared_ptr<const TargetResource> TargetStore::Find(const guest::SurfaceDesc& surface) const {
  auto canonical=Canonical(surface);
  if(auto assigned=guest_targets_.find(surface.id);assigned!=guest_targets_.end()) {
    auto target=targets_.at(assigned->second);auto& d=target->description;
    if(d.depth==canonical.depth && d.geometry.edram_base==canonical.geometry.edram_base && d.geometry.format==canonical.geometry.format) return target;
  }
  auto& g=canonical.geometry;Key key{g.edram_base,canonical.depth?0x8000u:g.format,g.width,g.height,1};
  if(auto found=keys_.find(key);found!=keys_.end()) return targets_.at(found->second);
  return {};
}
std::shared_ptr<TargetResource> TargetStore::Get(TargetId id,Error& e) {
  auto found=targets_.find(id);if(found==targets_.end()) {Fail(e,"Target lookup","Unknown render target");return {};}
  if(serial_) pending_.Keep(serial_,found->second);e={};return found->second;
}
bool TargetStore::Initialize(const std::shared_ptr<TargetResource>& target,Error& e) {
  if(target->initialized) return true;
  if(!state_.Transition(command_,target->image,{target->aspects,0,1,0,1},ImageUsage::TransferDestination(),e)) return false;
  VkImageSubresourceRange range{target->aspects,0,1,0,1};
  if(target->description.depth) {VkClearDepthStencilValue value{1,0};c_.f.vkCmdClearDepthStencilImage(command_,target->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&range);}
  else {VkClearColorValue value{};c_.f.vkCmdClearColorImage(command_,target->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&range);}
  target->initialized=true;return true;
}
std::shared_ptr<TargetPass> TargetStore::PreparePass(const PassPlan& plan,Error& e,const TargetPass* open) {
  if(!command_ || !serial_ || serial_<=completed_) {Fail(e,"Render pass","Missing active recording submission");return {};}
  if(!ValidatePassPlan(plan,e)) return {};
  std::array<TargetId,5> key{};
  for(uint32_t i=0;i<4;++i) if(plan.colors[i].id) {key[i]=Acquire(plan.colors[i],e);if(!key[i]) return {};}
  if(plan.depth.id) {key[4]=Acquire(plan.depth,e);if(!key[4]) return {};}
  for(uint32_t i=0;i<5;++i) for(uint32_t j=i+1;j<5;++j) if(key[i] && key[i]==key[j]) {Fail(e,"Render pass","Same target bound to multiple attachments");return {};}
  if(open) if(auto found=passes_.find(key);found!=passes_.end() && found->second.get()==open) {pending_.Hold(serial_,found->second);e={};return found->second;}
  std::vector<std::shared_ptr<TargetResource>> references;
  for(auto id:key) if(id) {
    auto target=Get(id,e);if(!target || !Initialize(target,e) || !state_.Transition(command_,target->image,{target->aspects,0,1,0,1},target->description.depth?ImageUsage::DepthAttachment():ImageUsage::ColorAttachment(),e)) return {};
    references.push_back(target);
  }
  if(auto found=passes_.find(key);found!=passes_.end()) {pending_.Hold(serial_,found->second);e={};return found->second;}
  auto pass=std::make_shared<TargetPass>();pass->context=&c_;pass->extent=plan.extent;pass->color_count=plan.color_count;pass->targets=references;
  std::vector<VkAttachmentDescription> attachments;std::vector<VkImageView> views;
  std::array<VkAttachmentReference,4> colors{};for(auto& color:colors) color={VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depth{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  for(uint32_t slot=0;slot<5;++slot) if(key[slot]) {
    auto target=Get(key[slot],e);bool is_depth=slot==4;
    VkAttachmentDescription attachment{};attachment.format=target->format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp=is_depth?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=is_depth?VK_ATTACHMENT_STORE_OP_STORE:VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout=attachment.finalLayout=is_depth?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{uint32_t(attachments.size()),attachment.initialLayout};
    if(is_depth) {depth=ref;pass->depth_format=target->format;}else {colors[slot]=ref;pass->formats[slot]=target->format;}
    attachments.push_back(attachment);views.push_back(target->attachment);
  }
  VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=plan.color_count;sub.pColorAttachments=colors.data();sub.pDepthStencilAttachment=key[4]?&depth:nullptr;
  VkSubpassDependency dependencies[2]{};
  dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL;dependencies[0].dstSubpass=0;
  dependencies[0].srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;dependencies[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dependencies[0].srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;dependencies[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dependencies[1].srcSubpass=0;dependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask=dependencies[0].dstStageMask;dependencies[1].dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  dependencies[1].srcAccessMask=dependencies[0].dstAccessMask;dependencies[1].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
  VkRenderPassCreateInfo create{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};create.attachmentCount=uint32_t(attachments.size());create.pAttachments=attachments.data();create.subpassCount=1;create.pSubpasses=&sub;create.dependencyCount=2;create.pDependencies=dependencies;
  if(!Check(c_.f.vkCreateRenderPass(c_.device,&create,nullptr,&pass->render_pass),"Create game render pass",e)) return {};
  VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};framebuffer.renderPass=pass->render_pass;framebuffer.attachmentCount=uint32_t(views.size());framebuffer.pAttachments=views.data();framebuffer.width=plan.extent.width;framebuffer.height=plan.extent.height;framebuffer.layers=1;
  if(!Check(c_.f.vkCreateFramebuffer(c_.device,&framebuffer,nullptr,&pass->framebuffer),"Create game framebuffer",e)) return {};
  passes_[key]=pass;pending_.Hold(serial_,pass);e={};return pass;
}
void TargetStore::MarkWritten(const TargetPass& pass) {uint64_t serial=++write_serial_;for(auto& target:pass.targets) target->last_write=serial;}
std::vector<std::pair<TargetId,AliasPlan>> TargetStore::Aliases(const TargetPass& pass) const {
  std::vector<AliasSurface> all;std::vector<uint64_t> bound;
  for(auto& [id,target]:targets_) all.push_back({id,target->description.depth,target->description.geometry,target->last_write});
  for(auto& target:pass.targets) bound.push_back(target->id);
  std::vector<std::pair<TargetId,AliasPlan>> plans;
  for(auto& target:pass.targets) {
    auto p=PlanEdramAlias({target->id,target->description.depth,target->description.geometry,target->last_write},all,bound);
    if(p.action!=AliasAction::kNone) plans.emplace_back(target->id,p);
  }
  return plans;
}
bool TargetStore::Clear(const guest::ClearPacket& clear,Error& e) {
  if(!command_ || !serial_ || serial_<=completed_) return Fail(e,"Clear targets","Missing active recording submission");
  std::vector<std::pair<guest::SurfaceDesc,uint32_t>> selected;
  for(uint32_t slot=0;slot<4;++slot) if((clear.flags&(1u<<slot)) && clear.colors[slot].id) {
    auto id=Acquire(clear.colors[slot],e);if(!id) return false;
    auto target=Get(id,e);
    for(auto& [other_id,other]:targets_) if(!other->description.depth && other->description.geometry.edram_base==target->description.geometry.edram_base && other->description.geometry.width==target->description.geometry.width && other->description.geometry.height==target->description.geometry.height) selected.emplace_back(other->description,VK_IMAGE_ASPECT_COLOR_BIT);
  }
  if((clear.flags&0x30) && clear.depth_surface.id) {
    auto depth=clear.depth_surface;
    if(clear.colors[0].id) {depth.geometry.width=std::max(depth.geometry.width,clear.colors[0].geometry.width);depth.geometry.height=std::max(depth.geometry.height,clear.colors[0].geometry.height);}
    uint32_t aspects=((clear.flags&0x10)?VK_IMAGE_ASPECT_DEPTH_BIT:0)|((clear.flags&0x20)?VK_IMAGE_ASPECT_STENCIL_BIT:0);
    selected.emplace_back(depth,aspects);
  }
  for(auto& [surface,aspects]:selected) {
    guest::DrawPacket draw;if(surface.depth) draw.depth=surface;else draw.colors[0]=surface;
    auto plan=PlanAttachments(draw,e);if(!e.message.empty()) return false;
    auto pass=PreparePass(plan,e);if(!pass) return false;
    std::vector<VkClearRect> rects;
    auto rect=[&](guest::Rect r) {
      int32_t x0=std::clamp(r.left,0,int32_t(plan.extent.width)),y0=std::clamp(r.top,0,int32_t(plan.extent.height));
      int32_t x1=std::clamp(r.right,x0,int32_t(plan.extent.width)),y1=std::clamp(r.bottom,y0,int32_t(plan.extent.height));
      if(x1>x0 && y1>y0) rects.push_back({{{x0,y0},{uint32_t(x1-x0),uint32_t(y1-y0)}},0,1});
    };
    if(clear.rects.empty()) rect({0,0,int32_t(plan.extent.width),int32_t(plan.extent.height)});else for(auto r:clear.rects) rect(r);
    if(rects.empty()) continue;
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=pass->render_pass;begin.framebuffer=pass->framebuffer;begin.renderArea={{0,0},plan.extent};
    VkClearAttachment attachment{};attachment.aspectMask=aspects;
    if(surface.depth) attachment.clearValue.depthStencil={std::clamp(clear.depth,0.0f,1.0f),clear.stencil&255};
    else std::memcpy(attachment.clearValue.color.float32,clear.color.data(),16);
    c_.f.vkCmdBeginRenderPass(command_,&begin,VK_SUBPASS_CONTENTS_INLINE);
    c_.f.vkCmdClearAttachments(command_,1,&attachment,uint32_t(rects.size()),rects.data());
    c_.f.vkCmdEndRenderPass(command_);MarkWritten(*pass);
  }
  e={};return true;
}
} // namespace superman_returns::graphics::vulkan
