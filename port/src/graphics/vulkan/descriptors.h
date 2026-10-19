#pragma once
#include "../guest/render_packet.h"
#include "loader.h"
#include <cstddef>
#include <span>
namespace superman_returns::graphics::vulkan {
enum class TextureDimension:uint32_t {k2D,k3D,kCube};
constexpr guest::ResourceId DummyTexture(TextureDimension d) {return UINT64_MAX-uint32_t(d);}
inline constexpr guest::ResourceId DummyBuffer=UINT64_MAX-3;
inline constexpr guest::ResourceId InlineBufferBase=uint64_t(1)<<63;
inline constexpr guest::ResourceId ExpandedIndexBufferBase=(uint64_t(1)<<63)|(uint64_t(1)<<62);
inline constexpr guest::ResourceId DescriptorConstantBufferBase=(uint64_t(1)<<63)|(uint64_t(1)<<61);
// The three constant blocks of a draw are bound as one contiguous 12 KiB range.
static_assert(sizeof(guest::ConstantSnapshot)==3*4096 && offsetof(guest::ConstantSnapshot,vs)==0 &&
              offsetof(guest::ConstantSnapshot,ps)==4096 && offsetof(guest::ConstantSnapshot,shared)==8192);
inline constexpr size_t kSharedConstantsOffset=offsetof(guest::ConstantSnapshot,shared);
struct TextureBindingRequest {
  uint32_t slot;
  guest::ResourceId resource;
  TextureDimension dimension;
  uint32_t flags;
};
// Resource identities and indices of one draw. The constants live in the draw's block (see BuildBindings).
struct DrawBindings {
  std::array<std::array<guest::ResourceId,32>,3> textures{};
  std::array<guest::ResourceId,32> vertex_buffers{};
  std::array<uint32_t,32> texture_indices{},sampler_indices{};
};
guest::ResourceId TextureResourceId(std::span<const uint32_t,6>);
// Validates every request first: on failure `bindings` is left untouched.
bool RemapTextureBindings(std::span<const TextureBindingRequest>,DrawBindings&,Error&);
// Fills `bindings` and writes the draw's constants into `block` (sizeof(guest::ConstantSnapshot) bytes, e.g. a
// ResourceStore::MapTransient slice): vs and ps as captured, shared patched with the texture and sampler
// indices and the vertex fetch metadata. Callers may patch the shared part further afterwards.
// copy_banks: bit 0 VS, bit 1 PS; omitted banks must be bound to a prior
// immutable slice by the caller. Shared metadata is always rebuilt.
bool BuildBindings(const guest::DrawPacket&,std::byte* block,DrawBindings& bindings,Error&,uint32_t copy_banks=3);
bool PlanSampler(std::span<const uint32_t,6>,int anisotropy_override,const VkPhysicalDeviceFeatures&,
                 const VkPhysicalDeviceLimits&,bool mirror_clamp,
                 VkSamplerCreateInfo&,Error&);
} // namespace superman_returns::graphics::vulkan
