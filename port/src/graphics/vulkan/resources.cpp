#include "resources.h"
#include "descriptors.h"
#include "image_state.h"
#include <algorithm>
#include <cstring>
#include <numeric>
namespace superman_returns::graphics::vulkan {
void SubmissionResources::Keep(uint64_t serial,std::shared_ptr<void> resource) {
  if(!resource) return;
  auto found=pending_.find(serial);
  if(found==pending_.end()) {
    std::vector<std::shared_ptr<void>> list;
    if(!spare_.empty()) {list=std::move(spare_.back());spare_.pop_back();}
    found=pending_.emplace(serial,std::move(list)).first;
  }
  found->second.push_back(std::move(resource));
}
void SubmissionResources::Retire(uint64_t completed) {
  auto end=pending_.upper_bound(completed);
  for(auto it=pending_.begin();it!=end;++it) {
    auto list=std::move(it->second);list.clear();  // releases the resources
    if(spare_.size()<8) spare_.push_back(std::move(list));
  }
  pending_.erase(pending_.begin(),end);
}
namespace {
struct Format {VkFormat vk;uint32_t block,bytes,source_bytes;};
Format MapFormat(guest::LinearFormat f) {
  using F=guest::LinearFormat;
  switch(f) {
  case F::kR8Unorm:return {VK_FORMAT_R8_UNORM,1,1,1};
  case F::kRG8Unorm:return {VK_FORMAT_R8G8_UNORM,1,2,2};
  case F::kRGBA8Unorm:return {VK_FORMAT_R8G8B8A8_UNORM,1,4,4};
  case F::kRGB10A2Unorm:return {VK_FORMAT_A2B10G10R10_UNORM_PACK32,1,4,4};
  case F::kBGR565Unorm:return {VK_FORMAT_R5G6B5_UNORM_PACK16,1,2,2};
  case F::kBGR5A1Unorm:return {VK_FORMAT_A1R5G5B5_UNORM_PACK16,1,2,2};
  // Avoid requiring VK_EXT_4444: expand DXGI B4G4R4A4's little-endian layout.
  case F::kBGRA4Unorm:return {VK_FORMAT_R8G8B8A8_UNORM,1,4,2};
  case F::kBC1:return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK,4,8,8};
  case F::kBC2:return {VK_FORMAT_BC2_UNORM_BLOCK,4,16,16};
  case F::kBC3:return {VK_FORMAT_BC3_UNORM_BLOCK,4,16,16};
  case F::kBC5:return {VK_FORMAT_BC5_UNORM_BLOCK,4,16,16};
  case F::kR16Unorm:return {VK_FORMAT_R16_UNORM,1,2,2};
  case F::kRG16Unorm:return {VK_FORMAT_R16G16_UNORM,1,4,4};
  case F::kRGBA16Unorm:return {VK_FORMAT_R16G16B16A16_UNORM,1,8,8};
  case F::kR16Float:return {VK_FORMAT_R16_SFLOAT,1,2,2};
  case F::kRG16Float:return {VK_FORMAT_R16G16_SFLOAT,1,4,4};
  case F::kRGBA16Float:return {VK_FORMAT_R16G16B16A16_SFLOAT,1,8,8};
  case F::kR32Float:return {VK_FORMAT_R32_SFLOAT,1,4,4};
  case F::kRG32Float:return {VK_FORMAT_R32G32_SFLOAT,1,8,8};
  case F::kRGBA32Float:return {VK_FORMAT_R32G32B32A32_SFLOAT,1,16,16};
  default:return {VK_FORMAT_UNDEFINED,0,0,0};
  }
}
bool Fail(Error& e,const char* operation,const char* reason) {
  e={operation,VK_ERROR_INITIALIZATION_FAILED,reason};return false;
}
std::optional<uint32_t> MemoryType(Context& c,uint32_t allowed,VkMemoryPropertyFlags required,VkMemoryPropertyFlags preferred) {
  std::vector<VkMemoryPropertyFlags> flags;
  for(uint32_t i=0;i<c.memory.memoryTypeCount;++i) flags.push_back(c.memory.memoryTypes[i].propertyFlags);
  return ChooseMemoryType(allowed,flags,required,preferred);
}
}
bool PlanTextureUpload(const guest::LinearTexture& texture,VkDeviceSize alignment,TextureUploadPlan& result,Error& e) {
  TextureUploadPlan out;
  auto f=MapFormat(texture.format);
  if(!texture.width || !texture.height || !texture.depth || !texture.mip_levels || texture.mip_levels>32 ||
     f.vk==VK_FORMAT_UNDEFINED || texture.dimension<1 || texture.dimension>3 || alignment>UINT32_MAX)
    return Fail(e,"Texture upload plan","Invalid texture dimensions, format or alignment");
  bool volume=texture.dimension==2,cube=texture.dimension==3;
  if((cube&&(texture.depth!=6 || texture.width!=texture.height)) || (volume&&texture.mip_levels!=1))
    return Fail(e,"Texture upload plan","Invalid cube or unsupported volume mip layout");
  if(texture.levels.size()!=uint64_t(texture.depth)*texture.mip_levels)
    return Fail(e,"Texture upload plan","Missing texture subresources");
  out.format=f.vk;out.extent={texture.width,texture.height,volume?texture.depth:1};
  out.layers=volume?1:texture.depth;out.mips=texture.mip_levels;
  out.image_type=volume?VK_IMAGE_TYPE_3D:VK_IMAGE_TYPE_2D;
  out.view_type=volume?VK_IMAGE_VIEW_TYPE_3D:(cube?VK_IMAGE_VIEW_TYPE_CUBE:VK_IMAGE_VIEW_TYPE_2D);
  out.flags=cube?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0;
  VkComponentSwizzle swizzles[]{VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_G,VK_COMPONENT_SWIZZLE_B,VK_COMPONENT_SWIZZLE_A,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ONE};
  for(auto s:texture.swizzle) if(s>=6) return Fail(e,"Texture upload plan","Invalid component swizzle");
  out.swizzle={swizzles[texture.swizzle[0]],swizzles[texture.swizzle[1]],swizzles[texture.swizzle[2]],swizzles[texture.swizzle[3]]};
  alignment=std::lcm(std::max<VkDeviceSize>(alignment,4),VkDeviceSize(f.bytes));
  for(uint32_t mip=0;mip<texture.mip_levels;++mip) for(uint32_t slice=0;slice<texture.depth;++slice) {
    const auto& level=texture.levels[mip*texture.depth+slice];
    uint32_t w=std::max(1u,texture.width>>mip),h=std::max(1u,texture.height>>mip);
    uint64_t rows=(uint64_t(h)+f.block-1)/f.block,blocks=(uint64_t(w)+f.block-1)/f.block;
    uint64_t src_pitch=blocks*f.source_bytes,dst_pitch=blocks*f.bytes;
    if(level.width!=w || level.height!=h || level.rows!=rows || level.row_pitch<src_pitch ||
       level.offset>texture.data.size() || (rows-1)*level.row_pitch+src_pitch>texture.data.size()-level.offset)
      return Fail(e,"Texture upload plan","Texture subresource is truncated or has invalid row pitch");
    uint64_t start=(out.bytes.size()+alignment-1)/alignment*alignment;
    uint64_t end=start+rows*dst_pitch;
    if(end>0x20000000ull) return Fail(e,"Texture upload plan","Texture staging exceeds guest memory bound");
    out.bytes.resize(size_t(end));
    for(uint32_t y=0;y<rows;++y) {
      const auto* src=texture.data.data()+level.offset+size_t(y)*level.row_pitch;
      auto* dst=out.bytes.data()+start+y*dst_pitch;
      if(f.bytes==f.source_bytes) std::memcpy(dst,src,size_t(dst_pitch));
      else for(uint32_t x=0;x<w;++x) {
        uint16_t packed=uint16_t(src[x*2])|(uint16_t(src[x*2+1])<<8);
        dst[x*4]=uint8_t(((packed>>8)&15)*17);dst[x*4+1]=uint8_t(((packed>>4)&15)*17);
        dst[x*4+2]=uint8_t((packed&15)*17);dst[x*4+3]=uint8_t((packed>>12)*17);
      }
    }
    VkBufferImageCopy copy{};copy.bufferOffset=start;
    copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,volume?0:slice,1};
    copy.imageOffset={0,0,volume?int32_t(slice):0};copy.imageExtent={w,h,1};
    out.regions.push_back(copy);
  }
  result=std::move(out);e={};return true;
}
MemoryCounters& LiveMemory() {static MemoryCounters counters;return counters;}
BufferResource::~BufferResource() {
  if(!context || !context->device || owner) return;
  if(memory) {--LiveMemory().buffers;LiveMemory().buffer_bytes-=int64_t(allocation);}
  if(handle) context->f.vkDestroyBuffer(context->device,handle,nullptr);
  if(memory) context->f.vkFreeMemory(context->device,memory,nullptr);
}
TextureResource::~TextureResource() {
  if(!context || !context->device) return;
  if(view) context->f.vkDestroyImageView(context->device,view,nullptr);
  if(array_view) context->f.vkDestroyImageView(context->device,array_view,nullptr);
  if(image_owner) return;
  if(memory) {--LiveMemory().images;LiveMemory().image_bytes-=int64_t(allocation);}
  if(state && handle) state->Forget(handle);
  if(handle) context->f.vkDestroyImage(context->device,handle,nullptr);
  if(memory) context->f.vkFreeMemory(context->device,memory,nullptr);
}
ResourceStore::~ResourceStore() {
  if(c_.device) c_.f.vkDeviceWaitIdle(c_.device);
  submissions_.Retire(UINT64_MAX);
}
bool ResourceStore::BeginSubmission(VkCommandBuffer cmd,uint64_t serial,Error& e,VkCommandBuffer upload) {
  if(!cmd || serial<=completed_ || serial<serial_) return Fail(e,"Resource submission","Missing command buffer or invalid submission serial");
  if(serial!=serial_) for(auto& [size,pool]:host_pool_) pool.cursor=0;
  if(serial!=serial_) submission_bytes_=0;
  command_=cmd;upload_=upload;upload_barrier_=false;texture_barriers_.clear();serial_=serial;e={};return true;
}
void ResourceStore::FinishUploads() {
  if(!upload_) return;
  VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};memory.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
  memory.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT|VK_ACCESS_INDEX_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
  if(upload_barrier_ || !texture_barriers_.empty())
    c_.f.vkCmdPipelineBarrier(upload_,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT|VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,0,
      upload_barrier_?1:0,&memory,0,nullptr,uint32_t(texture_barriers_.size()),texture_barriers_.data());
  upload_barrier_=false;texture_barriers_.clear();
}
void ResourceStore::Retire(uint64_t completed) {completed_=std::max(completed_,completed);submissions_.Retire(completed_);}
bool ResourceStore::Ready(Error& e) {
  return command_ && serial_>completed_ ? true : Fail(e,"Resource upload","No recording submission");
}
std::shared_ptr<BufferResource> ResourceStore::NewBuffer(VkDeviceSize size,VkBufferUsageFlags usage,VkMemoryPropertyFlags required,Error& e) {
  auto b=std::make_shared<BufferResource>();b->context=&c_;b->size=size;
  VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=size;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
  if(!Check(c_.f.vkCreateBuffer(c_.device,&ci,nullptr,&b->handle),"Create resource buffer",e)) return {};
  VkMemoryRequirements mr{};c_.f.vkGetBufferMemoryRequirements(c_.device,b->handle,&mr);b->allocation=mr.size;
  auto type=MemoryType(c_,mr.memoryTypeBits,required,(required&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)?VK_MEMORY_PROPERTY_HOST_COHERENT_BIT:VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if(!type) {Fail(e,"Buffer memory","Required memory type unavailable");return {};}
  b->memory_properties=c_.memory.memoryTypes[*type].propertyFlags;
  b->coherent=b->memory_properties&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=mr.size;alloc.memoryTypeIndex=*type;
  if(!Check(c_.f.vkAllocateMemory(c_.device,&alloc,nullptr,&b->memory),"Allocate resource buffer",e) ||
     !Check(c_.f.vkBindBufferMemory(c_.device,b->handle,b->memory,0),"Bind resource buffer",e)) return {};
  ++LiveMemory().buffers;LiveMemory().buffer_bytes+=int64_t(b->allocation);
  // Host-visible memory stays mapped for its lifetime; freeing it unmaps.
  if((required&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !Check(c_.f.vkMapMemory(c_.device,b->memory,0,VK_WHOLE_SIZE,0,&b->mapped),"Map resource buffer",e)) return {};
  return b;
}
bool ResourceStore::Write(const std::shared_ptr<BufferResource>& b,std::span<const std::byte> bytes,Error& e,VkDeviceSize offset) {
  // Suballocations carry their own mapped pointer and memory offset.
  auto* owner=b->owner?b->owner.get():b.get();
  if(!b->mapped || offset+bytes.size()>b->allocation) return Fail(e,"Write staging","Buffer is not host mapped or range exceeds allocation");
  std::memcpy(static_cast<std::byte*>(b->mapped)+offset,bytes.data(),bytes.size());
  if(b->coherent || bytes.empty()) {e={};return true;}
  auto aligned=AlignFlushRange(b->offset+offset,bytes.size(),owner->allocation,std::max<VkDeviceSize>(1,c_.properties.limits.nonCoherentAtomSize));
  if(!aligned.valid) return Fail(e,"Flush staging","Invalid flush range");
  VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=b->memory;range.offset=aligned.offset;range.size=aligned.size;
  return Check(c_.f.vkFlushMappedMemoryRanges(c_.device,1,&range),"Flush staging",e);
}
bool ResourceStore::Place(Arena& arena,VkDeviceSize size,VkDeviceSize alignment,Placement& out,Error& e) {
  if(&arena==&staging_) submission_bytes_+=size;
  // Budget covers two frames in flight of constants (~36 MiB per 3000 draws).
  constexpr VkDeviceSize chunk_size=8ull*1024*1024,budget=128ull*1024*1024;
  auto fit=[&](ArenaChunk& chunk) {
    VkDeviceSize offset=(chunk.used+alignment-1)/alignment*alignment;
    if(offset+size>chunk.buffer->size) return false;
    if(chunk.serial!=serial_) {chunk.serial=serial_;submissions_.Keep(serial_,chunk.buffer);}
    chunk.used=offset+size;out.chunk=chunk.buffer;out.offset=offset;return true;
  };
  if(arena.current<arena.chunks.size() && arena.chunks[arena.current].serial==serial_) if(fit(arena.chunks[arena.current])) return true;
  if(arena.overflow.buffer && arena.overflow.serial==serial_) if(fit(arena.overflow)) return true;
  // A chunk referenced only by the arena has no pending submission or view.
  for(size_t i=0;i<arena.chunks.size();++i) {
    auto& chunk=arena.chunks[i];
    if(chunk.buffer.use_count()!=1 || chunk.buffer->size<size) continue;
    chunk.used=0;chunk.serial=0;arena.current=i;if(fit(chunk)) return true;
  }
  auto buffer=NewBuffer(std::max(chunk_size,size),arena.usage,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,e);if(!buffer) return false;
  if(buffer->size==chunk_size && arena.bytes+buffer->allocation<=budget) {
    arena.bytes+=buffer->allocation;arena.chunks.push_back({buffer,0,0});arena.current=arena.chunks.size()-1;return fit(arena.chunks.back());
  }
  // Oversized or over budget: lives for this submission only. Keep filling
  // it, otherwise every later small allocation would create another chunk.
  arena.overflow={buffer,0,0};return fit(arena.overflow);
}
std::shared_ptr<BufferResource> ResourceStore::Suballocate(Arena& arena,VkDeviceSize size,VkDeviceSize alignment,Error& e) {
  Placement at;if(!Place(arena,size,alignment,at,e)) return {};
  auto v=std::make_shared<BufferResource>();v->context=&c_;v->owner=at.chunk;v->handle=at.chunk->handle;v->memory=at.chunk->memory;
  v->size=size;v->allocation=size;v->offset=at.offset;v->coherent=at.chunk->coherent;v->memory_properties=at.chunk->memory_properties;v->mapped=static_cast<std::byte*>(at.chunk->mapped)+at.offset;
  return v;
}
VkDeviceSize ResourceStore::TransientAlignment() const {
  return std::lcm<VkDeviceSize>(std::max<VkDeviceSize>(16,c_.properties.limits.minStorageBufferOffsetAlignment),std::max<VkDeviceSize>(1,c_.properties.limits.nonCoherentAtomSize));
}
bool ResourceStore::MapTransient(VkDeviceSize size,TransientSlice& out,Error& e) {
  if(!Ready(e)) return false;
  if(!size || size>c_.properties.limits.maxStorageBufferRange) return Fail(e,"Map transient","Invalid storage buffer range");
  Placement at;if(!Place(transient_,size,TransientAlignment(),at,e)) return false;
  out.chunk=std::move(at.chunk);out.offset=at.offset;out.size=size;out.data=static_cast<std::byte*>(out.chunk->mapped)+at.offset;
  e={};return true;
}
bool ResourceStore::FlushTransient(const TransientSlice& slice,Error& e) {
  if(!slice.chunk || !slice.data) return Fail(e,"Flush transient","Slice was not mapped");
  if(slice.chunk->coherent) {e={};return true;}
  auto aligned=AlignFlushRange(slice.offset,slice.size,slice.chunk->allocation,std::max<VkDeviceSize>(1,c_.properties.limits.nonCoherentAtomSize));
  if(!aligned.valid) return Fail(e,"Flush transient","Invalid flush range");
  VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=slice.chunk->memory;range.offset=aligned.offset;range.size=aligned.size;
  return Check(c_.f.vkFlushMappedMemoryRanges(c_.device,1,&range),"Flush transient",e);
}
std::shared_ptr<BufferResource> ResourceStore::UploadTransient(guest::ResourceId id,std::span<const std::byte> bytes,VkDeviceSize reserve,Error& e) {
  if(!Ready(e)) return {};
  VkDeviceSize size=std::max<VkDeviceSize>(bytes.size(),reserve);
  if(!size || size>c_.properties.limits.maxStorageBufferRange) {Fail(e,"Upload transient","Invalid storage buffer range");return {};}
  auto alignment=TransientAlignment();
  auto buffer=Suballocate(transient_,size,alignment,e);
  if(!buffer || !Write(buffer,bytes,e)) return {};
  if(id) {buffer->version=serial_;buffers_[id]=buffer;}
  e={};return buffer;
}
bool ResourceStore::UploadBuffer(guest::ResourceId id,std::span<const std::byte> bytes,uint64_t version,Error& e) {
  return UploadBufferImpl(id,bytes,version,false,e);
}
bool ResourceStore::UploadDynamicBuffer(guest::ResourceId id,std::span<const std::byte> bytes,uint64_t version,Error& e) {
  return UploadBufferImpl(id,bytes,version,true,e);
}
bool ResourceStore::UploadBufferImpl(guest::ResourceId id,std::span<const std::byte> bytes,uint64_t version,bool dynamic,Error& e) {
  if(!Ready(e)) return false;
  if(!id || bytes.empty() || bytes.size()>c_.properties.limits.maxStorageBufferRange) return Fail(e,"Upload buffer","Invalid buffer ID or storage range");
  auto found=buffers_.find(id);
  if(found!=buffers_.end() && found->second->version==version) {submissions_.Hold(serial_,found->second);e={};return true;}
  if(found!=buffers_.end() && version<found->second->version) return Fail(e,"Upload buffer","Stale buffer version");
  auto staging_alignment=std::lcm<VkDeviceSize>(16,std::max<VkDeviceSize>(1,c_.properties.limits.nonCoherentAtomSize));
  auto upload=Suballocate(staging_,bytes.size(),staging_alignment,e);
  if(!upload || !Write(upload,bytes,e)) return false;
  std::shared_ptr<BufferResource> gpu;
  if(dynamic) {
    auto pool=dynamic_pool_.find(bytes.size());
    if(pool!=dynamic_pool_.end()) for(auto& candidate:pool->second) {
      // The pool must be the sole owner: current IDs, pending submissions and
      // live descriptor leases all prevent overwriting an older version.
      if(candidate.use_count()==1) {gpu=candidate;break;}
    }
  }
  if(!gpu) {
    gpu=NewBuffer(bytes.size(),VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,e);
    constexpr VkDeviceSize budget=64ull*1024*1024;
    if(dynamic && gpu && gpu->allocation<=budget-dynamic_pool_bytes_) {
      dynamic_pool_[bytes.size()].push_back(gpu);dynamic_pool_bytes_+=gpu->allocation;
    }
  }
  if(!gpu) return false;
  gpu->version=version;
  VkBufferCopy copy{upload->offset,0,bytes.size()};c_.f.vkCmdCopyBuffer(upload_?upload_:command_,upload->handle,gpu->handle,1,&copy);
  if(upload_) {upload_barrier_=true;submissions_.Hold(serial_,gpu);buffers_[id]=std::move(gpu);e={};return true;}
  VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT|VK_ACCESS_INDEX_READ_BIT;
  barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.buffer=gpu->handle;barrier.size=VK_WHOLE_SIZE;
  c_.f.vkCmdPipelineBarrier(command_,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,0,0,nullptr,1,&barrier,0,nullptr);
  submissions_.Hold(serial_,upload);submissions_.Hold(serial_,gpu);buffers_[id]=std::move(gpu);e={};return true;
}
bool ResourceStore::UploadTexture(guest::ResourceId id,const guest::LinearTexture& texture,uint64_t version,Error& e) {
  if(!Ready(e)) return false;
  if(!id) return Fail(e,"Upload texture","Missing resource ID");
  auto found=textures_.find(id);
  if(found!=textures_.end() && found->second->version==version) {submissions_.Hold(serial_,found->second);e={};return true;}
  if(found!=textures_.end() && version<found->second->version) return Fail(e,"Upload texture","Stale texture version");
  TextureUploadPlan plan;
  if(!PlanTextureUpload(texture,c_.properties.limits.optimalBufferCopyOffsetAlignment,plan,e)) return false;
  VkFormatProperties format{};c_.f.vkGetPhysicalDeviceFormatProperties(c_.physical,plan.format,&format);
  if(!(format.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return Fail(e,"Texture format","Sampled image format unavailable");
  VkImageFormatProperties image_caps{};
  constexpr auto usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if(!Check(c_.f.vkGetPhysicalDeviceImageFormatProperties(c_.physical,plan.format,plan.image_type,VK_IMAGE_TILING_OPTIMAL,usage,plan.flags,&image_caps),"Texture image support",e)) return false;
  if(plan.extent.width>image_caps.maxExtent.width || plan.extent.height>image_caps.maxExtent.height || plan.extent.depth>image_caps.maxExtent.depth || plan.mips>image_caps.maxMipLevels || plan.layers>image_caps.maxArrayLayers)
    return Fail(e,"Texture image limits","Texture exceeds format dimensions");
  auto staging_alignment=std::lcm<VkDeviceSize>(std::max<VkDeviceSize>(16,c_.properties.limits.optimalBufferCopyOffsetAlignment),std::max<VkDeviceSize>(1,c_.properties.limits.nonCoherentAtomSize));
  auto upload=Suballocate(staging_,plan.bytes.size(),staging_alignment,e);
  if(!upload || !Write(upload,std::as_bytes(std::span(plan.bytes)),e)) return false;
  for(auto& region:plan.regions) region.bufferOffset+=upload->offset;
  auto gpu=std::make_shared<TextureResource>();gpu->context=&c_;gpu->format=plan.format;gpu->extent=plan.extent;gpu->layers=plan.layers;gpu->mips=plan.mips;gpu->view_type=plan.view_type;gpu->version=version;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.flags=plan.flags;ci.imageType=plan.image_type;ci.format=plan.format;ci.extent=plan.extent;ci.mipLevels=plan.mips;ci.arrayLayers=plan.layers;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
  if(!Check(c_.f.vkCreateImage(c_.device,&ci,nullptr,&gpu->handle),"Create texture image",e)) return false;
  VkMemoryRequirements mr{};c_.f.vkGetImageMemoryRequirements(c_.device,gpu->handle,&mr);
  auto type=MemoryType(c_,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if(!type) return Fail(e,"Texture memory","Device-local memory unavailable");
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=mr.size;alloc.memoryTypeIndex=*type;
  if(!Check(c_.f.vkAllocateMemory(c_.device,&alloc,nullptr,&gpu->memory),"Allocate texture",e) || !Check(c_.f.vkBindImageMemory(c_.device,gpu->handle,gpu->memory,0),"Bind texture",e)) return false;
  gpu->allocation=mr.size;++LiveMemory().images;LiveMemory().image_bytes+=int64_t(mr.size);
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=gpu->handle;view.viewType=plan.view_type;view.format=plan.format;view.components=plan.swizzle;
  view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,plan.mips,0,plan.view_type==VK_IMAGE_VIEW_TYPE_CUBE?6u:1u};
  if(!Check(c_.f.vkCreateImageView(c_.device,&view,nullptr,&gpu->view),"Create texture view",e)) return false;
  if(plan.view_type==VK_IMAGE_VIEW_TYPE_2D) {
    view.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;view.subresourceRange.layerCount=plan.layers;
    if(!Check(c_.f.vkCreateImageView(c_.device,&view,nullptr,&gpu->array_view),"Create texture array view",e)) return false;
  }
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=gpu->handle;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,plan.mips,0,plan.layers};barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
  auto command=upload_?upload_:command_;
  c_.f.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
  c_.f.vkCmdCopyBufferToImage(command,upload->handle,gpu->handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,uint32_t(plan.regions.size()),plan.regions.data());
  barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
  if(upload_) texture_barriers_.push_back(barrier);
  else c_.f.vkCmdPipelineBarrier(command_,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
  submissions_.Hold(serial_,upload);submissions_.Hold(serial_,gpu);textures_[id]=std::move(gpu);e={};return true;
}
std::shared_ptr<TextureResource> ResourceStore::ResolveTexture(guest::ResourceId id,VkFormat format,VkExtent3D extent,uint32_t mips,uint32_t layers,bool cube,ImageState& state,Error& e,VkImageUsageFlags additional_usage) {
  if(!Ready(e)) return {};
  if(!id || format==VK_FORMAT_UNDEFINED || !extent.width || !extent.height || extent.depth!=1 || !mips || !layers || (cube&&(layers!=6 || extent.width!=extent.height))) {
    e={"Resolve texture",VK_ERROR_INITIALIZATION_FAILED,"Invalid dimensions/view: "+std::to_string(extent.width)+"x"+std::to_string(extent.height)+"x"+std::to_string(extent.depth)+", mips="+std::to_string(mips)+", layers="+std::to_string(layers)+", cube="+std::to_string(cube)+", format="+std::to_string(format)+", id="+std::to_string(id)};return {};
  }
  auto view_type=cube?VK_IMAGE_VIEW_TYPE_CUBE:VK_IMAGE_VIEW_TYPE_2D;
  const VkImageUsageFlags usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|additional_usage;
  auto old=textures_.find(id);
  if(old!=textures_.end()) {
    auto t=old->second;
    if(t->state==&state && t->format==format && t->extent.width==extent.width && t->extent.height==extent.height && t->mips==mips && t->layers==layers && t->view_type==view_type && t->usage==usage) {submissions_.Hold(serial_,t);e={};return t;}
  }
  auto flags=cube?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0u;
  VkImageFormatProperties caps{};
  if(!Check(c_.f.vkGetPhysicalDeviceImageFormatProperties(c_.physical,format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,usage,flags,&caps),"Resolve image support",e)) return {};
  if(extent.width>caps.maxExtent.width || extent.height>caps.maxExtent.height || mips>caps.maxMipLevels || layers>caps.maxArrayLayers) {Fail(e,"Resolve texture","Image exceeds device limits");return {};}
  auto t=std::make_shared<TextureResource>();t->context=&c_;t->format=format;t->extent=extent;t->mips=mips;t->layers=layers;t->view_type=view_type;t->usage=usage;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.flags=flags;ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent=extent;ci.mipLevels=mips;ci.arrayLayers=layers;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
  if(!Check(c_.f.vkCreateImage(c_.device,&ci,nullptr,&t->handle),"Create resolve texture",e)) return {};
  VkMemoryRequirements mr{};c_.f.vkGetImageMemoryRequirements(c_.device,t->handle,&mr);
  auto type=MemoryType(c_,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if(!type) {Fail(e,"Resolve memory","Device-local memory unavailable");return {};}
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=mr.size;allocation.memoryTypeIndex=*type;
  if(!Check(c_.f.vkAllocateMemory(c_.device,&allocation,nullptr,&t->memory),"Allocate resolve texture",e) || !Check(c_.f.vkBindImageMemory(c_.device,t->handle,t->memory,0),"Bind resolve texture",e)) return {};
  t->allocation=mr.size;++LiveMemory().images;LiveMemory().image_bytes+=int64_t(mr.size);
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=t->handle;view.viewType=view_type;view.format=format;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,mips,0,cube?layers:1};
  if(!Check(c_.f.vkCreateImageView(c_.device,&view,nullptr,&t->view),"Create resolve view",e) || !state.Register(t->handle,mips,layers,VK_IMAGE_ASPECT_COLOR_BIT,e)) return {};
  if(!cube) {
    view.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;view.subresourceRange.layerCount=layers;
    if(!Check(c_.f.vkCreateImageView(c_.device,&view,nullptr,&t->array_view),"Create resolve array view",e)) return {};
  }
  t->state=&state;
  VkImageSubresourceRange allocation_range{VK_IMAGE_ASPECT_COLOR_BIT,0,mips,0,layers};
  if(!state.Transition(command_,t->handle,allocation_range,ImageUsage::TransferDestination(),e)) return {};
  VkClearColorValue zero{};c_.f.vkCmdClearColorImage(command_,t->handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&zero,1,&allocation_range);
  if(!state.Transition(command_,t->handle,allocation_range,ImageUsage::Sampled(),e)) return {};
  submissions_.Hold(serial_,t);textures_[id]=t;e={};return t;
}
bool ResourceStore::BindTextureView(guest::ResourceId id,std::shared_ptr<TextureResource> owner,VkComponentMapping mapping,Error& e) {
  if(!Ready(e)) return false;
  if(!id || !owner || !owner->handle || !owner->state) return Fail(e,"Resolve texture view","Missing tracked image");
  // Always refer directly to the allocation, never build a chain of view aliases.
  while(owner->image_owner) owner=owner->image_owner;
  auto t=std::make_shared<TextureResource>();t->context=&c_;t->image_owner=owner;t->handle=owner->handle;t->state=owner->state;t->format=owner->format;t->extent=owner->extent;t->layers=owner->layers;t->mips=owner->mips;t->view_type=owner->view_type;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=t->handle;view.viewType=t->view_type;view.format=t->format;view.components=mapping;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,t->mips,0,t->view_type==VK_IMAGE_VIEW_TYPE_2D?1:t->layers};
  if(!Check(c_.f.vkCreateImageView(c_.device,&view,nullptr,&t->view),"Resolved fetch view",e)) return false;
  if(t->view_type==VK_IMAGE_VIEW_TYPE_2D) {
    view.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;view.subresourceRange.layerCount=t->layers;
    if(!Check(c_.f.vkCreateImageView(c_.device,&view,nullptr,&t->array_view),"Resolved fetch array view",e)) return false;
  }
  submissions_.Hold(serial_,t);textures_[id]=t;e={};return true;
}
std::shared_ptr<BufferResource> ResourceStore::ResolveScratch(VkDeviceSize size,Error& e) {
  if(!Ready(e)) return {};
  if(!size || size>c_.properties.limits.maxStorageBufferRange) {Fail(e,"Resolve scratch","Invalid storage buffer size");return {};}
  auto buffer=NewBuffer(size,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,e);
  if(buffer) {submissions_.Hold(serial_,buffer);e={};}return buffer;
}
bool ResourceStore::UploadHostBuffer(guest::ResourceId id,std::span<const std::byte> bytes,uint64_t version,Error& e) {
  if(!Ready(e)) return false;
  if(!id || bytes.empty() || bytes.size()>c_.properties.limits.maxStorageBufferRange) return Fail(e,"Upload constants","Invalid storage buffer range");
  auto& pool=host_pool_[bytes.size()];
  std::shared_ptr<BufferResource> buffer;
  // Pending submissions, descriptor leases and cached IDs retain ownership.
  // Only a buffer held exclusively by this pool can be written again.
  while(pool.cursor<pool.buffers.size()) {
    auto& candidate=pool.buffers[pool.cursor++];
    if(candidate.use_count()==1) {buffer=candidate;break;}
  }
  if(!buffer) {
    buffer=NewBuffer(bytes.size(),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,e);
    constexpr VkDeviceSize budget=64ull*1024*1024;
    if(buffer && buffer->allocation<=budget-host_pool_bytes_) {
      host_pool_bytes_+=buffer->allocation;pool.buffers.push_back(buffer);pool.cursor=pool.buffers.size();
    }
  }
  if(!buffer || !Write(buffer,bytes,e)) return false;
  buffer->version=version;submissions_.Hold(serial_,buffer);buffers_[id]=std::move(buffer);e={};return true;
}
std::shared_ptr<BufferResource> ResourceStore::ReadbackBuffer(VkDeviceSize size,Error& e) {
  if(!Ready(e)) return {};
  if(!size || size>256*1024*1024) {Fail(e,"Readback buffer","Image readback size exceeds limit");return {};}
  auto buffer=NewBuffer(size,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,e);if(buffer) submissions_.Hold(serial_,buffer);return buffer;
}
bool ResourceStore::Readback(const std::shared_ptr<BufferResource>& buffer,std::vector<uint8_t>& bytes,Error& e) {
  if(!buffer || buffer->context!=&c_) return Fail(e,"Readback buffer","Buffer is absent or belongs to another device");
  if(!buffer->mapped) return Fail(e,"Readback buffer","Buffer is not host mapped");
  if(!buffer->coherent) {VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=buffer->memory;range.offset=0;range.size=VK_WHOLE_SIZE;if(!Check(c_.f.vkInvalidateMappedMemoryRanges(c_.device,1,&range),"Invalidate readback",e)) return false;}
  auto* data=static_cast<uint8_t*>(buffer->mapped);bytes.assign(data,data+buffer->size);e={};return true;
}
std::shared_ptr<BufferResource> ResourceStore::Buffer(guest::ResourceId id,Error& e) {
  if(!Ready(e)) return {};
  auto found=buffers_.find(id);
  if(found==buffers_.end()) {Fail(e,"Buffer binding","Buffer has no captured upload");return {};}
  submissions_.Hold(serial_,found->second);e={};return found->second;
}
std::shared_ptr<TextureResource> ResourceStore::Texture(guest::ResourceId id,Error& e) {
  if(!Ready(e)) return {};
  auto found=textures_.find(id);
  if(found==textures_.end()) {Fail(e,"Texture binding","Texture has no upload or resolve");return {};}
  submissions_.Hold(serial_,found->second);e={};return found->second;
}
bool ResourceStore::CreateDummies(Error& e) {
  std::array<std::byte,16> zero{};
  if(!UploadBuffer(DummyBuffer,zero,0,e)) return false;
  for(uint32_t dim=0;dim<3;++dim) {
    guest::LinearTexture t;t.width=t.height=1;t.dimension=dim+1;t.depth=dim==2?6:1;t.format=guest::LinearFormat::kRGBA8Unorm;
    for(uint32_t layer=0;layer<t.depth;++layer) {t.levels.push_back({1,1,4,1,layer*4u});t.data.insert(t.data.end(),4,0);}
    if(!UploadTexture(DummyTexture(TextureDimension(dim)),t,0,e)) return false;
  }
  return true;
}
} // namespace superman_returns::graphics::vulkan
