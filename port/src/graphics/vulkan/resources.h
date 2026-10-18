#pragma once
#include "context.h"
#include "../guest/texture_layout.h"
#include "../guest/render_packet.h"
#include <atomic>
#include <map>
#include <memory>
#include <unordered_map>
namespace superman_returns::graphics::vulkan {
class ImageState;
class SubmissionResources {
public:
  void Keep(uint64_t serial,std::shared_ptr<void> resource);
  // Keeps a resource for the submission once: `held_serial` says it is already in that submission's list.
  // Invariant: everything a recorded command refers to is held before the command is recorded.
  template<class T> void Hold(uint64_t serial,const std::shared_ptr<T>& resource) {
    if(!resource || resource->held_serial==serial) return;
    resource->held_serial=serial;Keep(serial,resource);
  }
  void Retire(uint64_t completed_serial);
private:
  std::map<uint64_t,std::vector<std::shared_ptr<void>>> pending_;
  std::vector<std::vector<std::shared_ptr<void>>> spare_;  // emptied lists keep their capacity for the next submission
};
struct TextureUploadPlan {
  VkFormat format=VK_FORMAT_UNDEFINED;
  VkImageType image_type=VK_IMAGE_TYPE_2D;
  VkImageViewType view_type=VK_IMAGE_VIEW_TYPE_2D;
  VkImageCreateFlags flags=0;
  VkExtent3D extent{};
  uint32_t layers=1,mips=1;
  VkComponentMapping swizzle{};
  std::vector<VkBufferImageCopy> regions;
  std::vector<uint8_t> bytes;
};
bool PlanTextureUpload(const guest::LinearTexture&,VkDeviceSize alignment,TextureUploadPlan&,Error&);
// Live VkDeviceMemory owned by buffers/textures of this module (diagnostics).
struct MemoryCounters {std::atomic<int64_t> buffers{0},buffer_bytes{0},images{0},image_bytes{0};};
MemoryCounters& LiveMemory();
struct BufferResource {
  Context* context=nullptr;
  // A suballocation shares owner's handle; bind it at offset, never destroy it.
  std::shared_ptr<BufferResource> owner;
  VkBuffer handle=VK_NULL_HANDLE;
  VkDeviceMemory memory=VK_NULL_HANDLE;
  VkDeviceSize size=0,allocation=0,offset=0;
  uint64_t version=0;
  uint64_t held_serial=0;  // submission whose list already holds this resource (SubmissionResources::Hold)
  bool coherent=false;
  VkMemoryPropertyFlags memory_properties=0;
  void* mapped=nullptr;  // persistent mapping of host-visible allocations
  ~BufferResource();
};
// A host-visible slice of the transient arena that the caller fills in place (no copy). The arena chunk is
// kept by the submission (Place keeps it once per serial), so the slice needs no ownership of its own.
struct TransientSlice {
  std::shared_ptr<BufferResource> chunk;  // handle, memory and coherency come from the chunk
  VkDeviceSize offset=0,size=0;
  std::byte* data=nullptr;
};
struct TextureResource {
  Context* context=nullptr;
  ImageState* state=nullptr;
  // Optional lifetime for state attached to standalone presentation snapshots.
  std::shared_ptr<ImageState> state_owner;
  std::shared_ptr<TextureResource> image_owner;
  VkImage handle=VK_NULL_HANDLE;
  VkDeviceMemory memory=VK_NULL_HANDLE;
  VkImageView view=VK_NULL_HANDLE;
  // 2D_ARRAY view over every layer of a 2D image (stacked textures read by
  // tfetch3D; the 2D view above shows layer 0 only). Null for 3D and cube.
  VkImageView array_view=VK_NULL_HANDLE;
  VkFormat format=VK_FORMAT_UNDEFINED;
  VkExtent3D extent{};
  uint32_t layers=1,mips=1;
  VkImageViewType view_type=VK_IMAGE_VIEW_TYPE_2D;
  uint64_t version=0;VkDeviceSize allocation=0;
  VkImageUsageFlags usage=0;
  uint64_t held_serial=0;  // see BufferResource::held_serial
  ~TextureResource();
};
// Context must outlive the store and any resources retained by descriptor sets.
// All methods run on the render worker. BeginSubmission requires a recording
// command buffer; replacement is immutable and never destroys pending versions.
class ResourceStore {
public:
  explicit ResourceStore(Context& c):c_(c) {buffers_.reserve(4096);textures_.reserve(4096);}
  ~ResourceStore();
  // With a separate upload command buffer (submitted before the main one),
  // buffer/texture uploads record there and share one barrier written by
  // FinishUploads. Every upload creates a new version, so hoisting is safe.
  bool BeginSubmission(VkCommandBuffer,uint64_t serial,Error&,VkCommandBuffer upload=VK_NULL_HANDLE);
  void FinishUploads();
  void Retire(uint64_t completed_serial);
  // Host-visible per-submission suballocation; the returned view binds at
  // its offset and stays valid until this submission retires.
  std::shared_ptr<BufferResource> UploadTransient(guest::ResourceId,std::span<const std::byte>,VkDeviceSize reserve,Error&);
  // UploadTransient without the copy: write `size` bytes at slice.data, then FlushTransient. Valid until
  // this submission retires.
  bool MapTransient(VkDeviceSize size,TransientSlice&,Error&);
  bool FlushTransient(const TransientSlice&,Error&);
  bool UploadBuffer(guest::ResourceId,std::span<const std::byte>,uint64_t version,Error&);
  // Reuse device allocations only after submissions and descriptor leases release them.
  bool UploadDynamicBuffer(guest::ResourceId,std::span<const std::byte>,uint64_t version,Error&);
  bool UploadTexture(guest::ResourceId,const guest::LinearTexture&,uint64_t version,Error&);
  // Tracked resolve images remain GPU-authoritative and preserve partial writes.
  // The supplied ImageState must outlive this store and descriptor leases.
  std::shared_ptr<TextureResource> ResolveTexture(guest::ResourceId,VkFormat,VkExtent3D,uint32_t mips,uint32_t layers,bool cube,ImageState&,Error&,VkImageUsageFlags additional_usage=0);
  std::shared_ptr<BufferResource> ResolveScratch(VkDeviceSize,Error&);
  std::shared_ptr<BufferResource> ReadbackBuffer(VkDeviceSize,Error&);
  bool Readback(const std::shared_ptr<BufferResource>&,std::vector<uint8_t>&,Error&);
  bool BindTextureView(guest::ResourceId,std::shared_ptr<TextureResource>,VkComponentMapping,Error&);
  std::shared_ptr<BufferResource> Buffer(guest::ResourceId,Error&);
  // Lookups without registering the resource with the current submission;
  // the caller must keep what it binds alive (descriptor cache entries do).
  const std::shared_ptr<BufferResource>* FindBuffer(guest::ResourceId id) const {auto f=buffers_.find(id);return f==buffers_.end()?nullptr:&f->second;}
  const std::shared_ptr<TextureResource>* FindTexture(guest::ResourceId id) const {auto f=textures_.find(id);return f==textures_.end()?nullptr:&f->second;}
  std::shared_ptr<TextureResource> Texture(guest::ResourceId,Error&);
  bool CreateDummies(Error&);
  bool UploadHostBuffer(guest::ResourceId,std::span<const std::byte>,uint64_t version,Error&);
  uint64_t CurrentSerial() const {return serial_;}
  // Keeps a buffer or texture alive until the current submission retires (once per submission).
  template<class T> void Hold(const std::shared_ptr<T>& resource) {submissions_.Hold(serial_,resource);}
  // Bytes staged for buffer/texture uploads in the current submission.
  VkDeviceSize SubmissionBytes() const {return submission_bytes_;}
  void ForgetBuffer(guest::ResourceId id) {buffers_.erase(id);}
  void ForgetTexture(guest::ResourceId id) {textures_.erase(id);}
private:
  bool UploadBufferImpl(guest::ResourceId,std::span<const std::byte>,uint64_t version,bool dynamic,Error&);
  std::shared_ptr<BufferResource> NewBuffer(VkDeviceSize,VkBufferUsageFlags,VkMemoryPropertyFlags,Error&);
  bool Write(const std::shared_ptr<BufferResource>&,std::span<const std::byte>,Error&,VkDeviceSize offset=0);
  bool Ready(Error&);
  struct ArenaChunk {std::shared_ptr<BufferResource> buffer;VkDeviceSize used=0;uint64_t serial=0;};
  // `overflow` is the current chunk once the pooled budget is spent; it is
  // filled like a pooled chunk and released when its submission retires.
  struct Arena {VkBufferUsageFlags usage=0;std::vector<ArenaChunk> chunks;size_t current=SIZE_MAX;VkDeviceSize bytes=0;ArenaChunk overflow;};
  struct Placement {std::shared_ptr<BufferResource> chunk;VkDeviceSize offset=0;};
  bool Place(Arena&,VkDeviceSize size,VkDeviceSize alignment,Placement&,Error&);
  std::shared_ptr<BufferResource> Suballocate(Arena&,VkDeviceSize size,VkDeviceSize alignment,Error&);
  VkDeviceSize TransientAlignment() const;
  Context& c_;
  VkCommandBuffer command_=VK_NULL_HANDLE,upload_=VK_NULL_HANDLE;
  bool upload_barrier_=false;std::vector<VkImageMemoryBarrier> texture_barriers_;VkDeviceSize submission_bytes_=0;
  Arena staging_{VK_BUFFER_USAGE_TRANSFER_SRC_BIT},transient_{VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
  uint64_t serial_=0,completed_=0;
  SubmissionResources submissions_;
  // Hashed: ~17k live buffer versions make an ordered map cost ~15 dependent cache misses per lookup. Nothing
  // iterates these in order, and node storage keeps the pointers FindBuffer/FindTexture return stable.
  std::unordered_map<guest::ResourceId,std::shared_ptr<BufferResource>> buffers_;
  struct HostPool {std::vector<std::shared_ptr<BufferResource>> buffers;size_t cursor=0;};
  std::map<VkDeviceSize,HostPool> host_pool_;
  VkDeviceSize host_pool_bytes_=0;
  std::map<VkDeviceSize,std::vector<std::shared_ptr<BufferResource>>> dynamic_pool_;
  VkDeviceSize dynamic_pool_bytes_=0;
  std::unordered_map<guest::ResourceId,std::shared_ptr<TextureResource>> textures_;
};
} // namespace superman_returns::graphics::vulkan
