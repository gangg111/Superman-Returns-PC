#include "descriptors.h"
#include <cstring>
#include <array>
#include <algorithm>
namespace superman_returns::graphics::vulkan {
guest::ResourceId TextureResourceId(std::span<const uint32_t,6> words) {
  // IDs identify immutable fetch views, not an API's global descriptor index.
  uint64_t hash=14695981039346656037ull;
  for(auto word:words) for(uint32_t b=0;b<4;++b) {hash^=(word>>(b*8))&255;hash*=1099511628211ull;}
  return hash&~InlineBufferBase;
}
bool RemapTextureBindings(std::span<const TextureBindingRequest> requests,DrawBindings& out,Error& error) {
  if(requests.size()>32) {error={"Descriptor remap",VK_ERROR_FEATURE_NOT_PRESENT,"More than 32 texture slots"};return false;}
  std::array<bool,32> used{};
  for(const auto& r:requests) {  // validate everything before touching `out`
    auto dim=uint32_t(r.dimension);
    if(r.slot>=32 || dim>=3 || used[r.slot] || !r.resource || r.resource>=DummyBuffer) {
      error={"Descriptor remap",VK_ERROR_INITIALIZATION_FAILED,"Invalid or duplicate texture slot/resource/dimension"};return false;
    }
    used[r.slot]=true;
  }
  for(uint32_t dim=0;dim<3;++dim) out.textures[dim].fill(DummyTexture(TextureDimension(dim)));
  out.texture_indices.fill(31);
  std::array<uint32_t,3> count{};
  for(const auto& r:requests) {
    auto dim=uint32_t(r.dimension);
    uint32_t index=count[dim]++;
    out.textures[dim][index]=r.resource;
    out.texture_indices[r.slot]=(r.flags&~0x7fffu)|index;
  }
  error={};return true;
}
bool BuildBindings(const guest::DrawPacket& draw,std::byte* block,DrawBindings& out,Error& error,uint32_t copy_banks) {
  out=DrawBindings{};
  out.vertex_buffers.fill(DummyBuffer);
  std::array<TextureBindingRequest,32> requests;size_t request_count=0;
  for(uint32_t slot=0;slot<32;++slot) {
    const auto& fetch=draw.texture_fetch[slot];
    if((fetch[0]&3)!=2) continue;
    uint32_t dim=(fetch[5]>>9)&3;
    if(dim==0) dim=1; // SDK prepares 1D as a one-row 2D image.
    uint32_t gamma=0;
    if(((fetch[0]>>2)&3)==3 && ((fetch[0]>>4)&3)==3 && ((fetch[0]>>6)&3)==3) gamma=0x80000000u;
    requests[request_count++]={slot,TextureResourceId(fetch),TextureDimension(dim-1),gamma};
  }
  if(!RemapTextureBindings(std::span<const TextureBindingRequest>(requests.data(),request_count),out,error)) return false;
  std::array<uint8_t,32> stream_index;stream_index.fill(0xff);uint32_t streams=0;
  if(draw.inline_vertices) {
    out.vertex_buffers[0]=InlineBufferBase|draw.command_serial;
  } else {
    for(const auto& stream:draw.streams) {
      if(stream.stream>=32 || !stream.update.plan.key || stream_index[stream.stream]!=0xff || streams>=32) {
        error={"Vertex remap",VK_ERROR_INITIALIZATION_FAILED,"Invalid or duplicate vertex stream"};return false;
      }
      stream_index[stream.stream]=uint8_t(streams);out.vertex_buffers[streams++]=stream.update.plan.key;
    }
  }
  // The block is write-combined memory that is neither zeroed nor initialized, and reading the cold packet is the
  // expensive half of the copy. Take only what survives: vs, ps and shared[256,512). shared[0,256) (texture and
  // sampler indices) and shared[512,4096) (vertex fetch) are rewritten in full below.
  static_assert(offsetof(guest::ConstantSnapshot,vs)==0 && offsetof(guest::ConstantSnapshot,ps)==sizeof(guest::ConstantSnapshot::vs) && kSharedConstantsOffset==sizeof(guest::ConstantSnapshot::vs)+sizeof(guest::ConstantSnapshot::ps),"vs, ps and shared are contiguous");
  static_assert(sizeof(guest::ConstantSnapshot::shared)==512+224*16 && sizeof(draw.vertex_fetch)==224*sizeof(guest::VertexFetchMeta) && sizeof(guest::VertexFetchMeta)==16,"vertex fetch fills shared[512,4096)");
  std::byte* shared=block+kSharedConstantsOffset;
  if(copy_banks==3) std::memcpy(block,&draw.constants,kSharedConstantsOffset);
  else {
    if(copy_banks&1) std::memcpy(block,draw.constants.vs.data(),sizeof(draw.constants.vs));
    if(copy_banks&2) std::memcpy(block+sizeof(draw.constants.vs),draw.constants.ps.data(),sizeof(draw.constants.ps));
  }
  std::memcpy(shared+256,draw.constants.shared.data()+256,256);
  for(uint32_t i=0;i<draw.vertex_fetch.size();++i) {
    auto meta=draw.vertex_fetch[i];
    if(meta.type) {
      if(draw.inline_vertices) meta.buffer=0;
      else {
        if(meta.buffer>=32 || stream_index[meta.buffer]==0xff) {error={"Vertex remap",VK_ERROR_INITIALIZATION_FAILED,"Vertex metadata references missing stream"};return false;}
        meta.buffer=stream_index[meta.buffer];
      }
    } else {meta={31,0,0,0};}
    std::memcpy(shared+512+i*16,&meta,16);
  }
  for(uint32_t slot=0;slot<32;++slot) out.sampler_indices[slot]=slot;
  // Shader ABI of the Vulkan emitter (patch 0011, the same tables the D3D12 path
  // fills): four tables of 16 dwords by fetch register r, with the 2D index at
  // [4r], the 3D index at [64+4r], the cube index at [128+4r] and the sampler at
  // [192+4r]. Only the table of the texture's own dimension gets its index; the
  // others point at index 31, which RemapTextureBindings leaves on the dummy of
  // that dimension. Before this, 3D and cube fetches read another slot's index
  // and every sampler came from slot r+16 (usually an empty fetch constant).
  for(uint32_t slot=0;slot<16;++slot) {
    std::array<uint32_t,3> index{31u,31u,31u};
    const auto& fetch=draw.texture_fetch[slot];
    if((fetch[0]&3)==2) {
      uint32_t dim=(fetch[5]>>9)&3;
      dim=dim?dim-1:0;  // 1D and 2D share the 2D table, as in the requests above
      index[dim]=out.texture_indices[slot];
      // A stacked 2D texture (fetch dword 1 bit 10) read by tfetch3D: the 3D
      // table points at its array view (binding 3, same index as the 2D table).
      if(dim==0 && (fetch[1]&(1u<<10))) index[1]=out.texture_indices[slot]|0x10000000u;
    }
    for(uint32_t dim=0;dim<3;++dim) std::memcpy(shared+dim*64+slot*4,&index[dim],4);
    std::memcpy(shared+192+slot*4,&out.sampler_indices[slot],4);
  }
  error={};return true;
}
bool PlanSampler(std::span<const uint32_t,6> fetch,const VkPhysicalDeviceFeatures& features,
                 const VkPhysicalDeviceLimits& limits,bool mirror_clamp,VkSamplerCreateInfo& out,Error& e) {
  VkSamplerCreateInfo next{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  auto address=[&](uint32_t mode,VkSamplerAddressMode& result) {
    switch(mode) {
    case 0:result=VK_SAMPLER_ADDRESS_MODE_REPEAT;break;
    case 1:result=VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;break;
    case 3:case 5:case 7:
      if(!mirror_clamp) {e={"Sampler",VK_ERROR_FEATURE_NOT_PRESENT,"Mirror-clamp sampler extension was not enabled"};return false;}
      result=VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;break;
    case 6:result=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;break;
    default:result=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;break;
    }
    return true;
  };
  if(!address((fetch[0]>>10)&7,next.addressModeU) || !address((fetch[0]>>13)&7,next.addressModeV) || !address((fetch[0]>>16)&7,next.addressModeW)) return false;
  next.magFilter=((fetch[3]>>19)&3)==1?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
  next.minFilter=((fetch[3]>>21)&3)==1?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
  next.mipmapMode=((fetch[3]>>23)&3)==1?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;
  uint32_t aniso=(fetch[3]>>25)&7;
  if(aniso>1) {
    if(!features.samplerAnisotropy || limits.maxSamplerAnisotropy<1) {
      e={"Sampler",VK_ERROR_FEATURE_NOT_PRESENT,"Guest sampler needs enabled anisotropy"};return false;
    }
    next.anisotropyEnable=VK_TRUE;next.maxAnisotropy=std::min(float(std::min(16u,1u<<(aniso-1))),limits.maxSamplerAnisotropy);
    next.magFilter=next.minFilter=VK_FILTER_LINEAR;next.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;
  }
  // Match the reference's original-scale sampler. Fetch LOD bias is emitted
  // by the guest shader; adding it here would apply the bias twice.
  next.minLod=0;next.maxLod=VK_LOD_CLAMP_NONE;
  next.compareOp=VK_COMPARE_OP_NEVER;next.borderColor=VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  out=next;e={};return true;
}
} // namespace superman_returns::graphics::vulkan
