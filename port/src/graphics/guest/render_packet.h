#pragma once
#include "captured_batch.h"
#include "constant_snapshot.h"
#include "render_state.h"
#include "vertex_layout.h"
#include <array>
#include <stdexcept>
#include <variant>
namespace superman_returns::native {
class Pm4Mirror;
}
namespace superman_returns::graphics::guest {
using ResourceId = uint64_t;
struct BufferUpdate {
  BufferPlan plan;
  std::vector<uint8_t> bytes;
  std::vector<uint32_t> normalized;
};
struct VertexStream {
  uint32_t stream = 0, offset = 0, size = 0, stride = 0;
  BufferUpdate update;
};
struct SurfaceDesc {
  ResourceId id = 0;
  bool depth = false;
  SurfaceGeometry geometry{};
};
// The PM4 registers 0x2000-0x23FF a draw carries, indexed from 0x2000. Decoding sets only the device's shadowed
// ranges (game_profile.h kRegisterShadow, 121 registers, checked by a static_assert in render_packet.cpp), so
// every other one reads 0, as in the zero-filled 4 KB array this replaces: 484 bytes a packet instead of
// 4 KB, none of it zeroed for nothing (mobile renderer plan, E2). Writing outside the ranges is a bug.
struct DrawRegisterRange {uint32_t first, count;};
inline constexpr DrawRegisterRange kDrawRegisterRanges[] = {{0x000, 16}, {0x100, 21}, {0x180, 5}, {0x200, 12},
                                                            {0x280, 21}, {0x300, 38}, {0x380, 8}};
// Register index (from 0x2000) -> its word in DrawRegisters, 0xff outside the ranges.
inline constexpr std::array<uint8_t, 0x400> kDrawRegisterSlots = [] {
  std::array<uint8_t, 0x400> slots{};
  for (auto& s : slots) s = 0xff;
  uint32_t next = 0;
  for (const auto& r : kDrawRegisterRanges)
    for (uint32_t i = 0; i < r.count; ++i) slots[r.first + i] = uint8_t(next++);
  return slots;
}();
class DrawRegisters {
public:
  using Range = DrawRegisterRange;
  static constexpr const DrawRegisterRange (&kRanges)[7] = kDrawRegisterRanges;
  static constexpr uint32_t kCount = 121;
  static constexpr uint32_t kSize = 0x400;
  uint32_t operator[](uint32_t index) const {
    const uint8_t slot = index < kSize ? kSlots[index] : kNone;
    return slot == kNone ? 0u : words_[slot];
  }
  // Outside the ranges: a scratch word, zeroed at each access, so a read through a non-const packet still
  // gives 0 (FinalizeDrawConstants reads 0x80) and a stray write is lost instead of stopping the renderer.
  uint32_t& operator[](uint32_t index) {
    const uint8_t slot = index < kSize ? kSlots[index] : kNone;
    if (slot == kNone) { scratch_ = 0; return scratch_; }
    return words_[slot];
  }
  static constexpr bool Kept(uint32_t index) { return index < kSize && kSlots[index] != kNone; }
  bool operator==(const DrawRegisters& other) const { return words_ == other.words_; }
private:
  static constexpr uint8_t kNone = 0xff;
  static constexpr const std::array<uint8_t, kSize>& kSlots = kDrawRegisterSlots;
  std::array<uint32_t, kCount> words_{};
  uint32_t scratch_ = 0;
};
// A float constant bank as the replay mirror held it at one version, shared by every draw decoded while
// that version stood, instead of a 4 KB copy in each packet (mobile renderer plan, E2): only 24% of a
// city frame's ~2,700 draws keep the VS bank and 59% the PS bank, but a new version is one copy, not one
// per draw, and the packets' banks are never written.
struct ConstantBank {
  uint64_t version = 0;
  std::array<uint32_t, 1024> words;
};
struct DrawPacket {
  struct ReplayStorageTag {};
  // Keep this user-provided: value-initialization must not add a whole-packet
  // zeroing pass before the individual members initialize themselves.
  DrawPacket() {}
  // Only DecodeCapturedPacket may publish this storage: it fills both float
  // banks, polygon offset registers and the shared metadata suffix before their
  // first use. Shared prefix/device/vertex/fetch keep normal initialization.
  explicit DrawPacket(ReplayStorageTag) : constants(ConstantSnapshot::ReplayStorageTag{}) {}
  int pass = 0;
  Primitive primitive = Primitive::kTriangles;
  bool indexed = false, inline_vertices = false;
  bool primitive_restart = false;
  int32_t depth_bias=0;float slope_bias=0;
  int32_t base_vertex = 0;
  uint32_t first = 0, count = 0, inline_stride = 0;
  ConstantSnapshot constants;
  // The replay mirror's bank versions `constants` were taken at (Pm4Mirror vs_version / ps_version:
  // bumped on every register change), 0 when the banks came from elsewhere. Equal non-zero versions
  // mean equal contents: the recorder's bank cache compares them instead of 4 KB per bank per draw.
  uint64_t vs_version = 0, ps_version = 0;
  // The replay's shared banks (ConstantBank); null: constants.vs / constants.ps hold the bank. Read the banks
  // through VsWords / PsWords, never constants.vs / ps directly.
  std::shared_ptr<const ConstantBank> vs_bank, ps_bank;
  const uint32_t* VsWords() const { return vs_bank ? vs_bank->words.data() : constants.vs.data(); }
  const uint32_t* PsWords() const { return ps_bank ? ps_bank->words.data() : constants.ps.data(); }
  CapturedMemory memory;
  std::vector<VertexStream> streams;
  std::vector<VertexAttribute> attributes;
  std::array<VertexFetchMeta, 224> vertex_fetch{};
  BufferUpdate indices;
  std::vector<uint32_t> expanded_indices;
  std::vector<uint8_t> inline_data;
  Viewport viewport;
  Rect scissor{0, 0, 16384, 16384};
  bool tiling_active = false;
  DrawRegisters registers;
  // Only PM4 registers 0x2380..0x2383 are consumed by FinalizeDrawConstants.
  // Keep their exact bits; the other 1,020 mirrored registers are not read.
  std::array<uint32_t, 4> polygon_offset_registers{};
  std::array<std::array<uint32_t, 6>, 32> texture_fetch{};
  std::array<SurfaceDesc, 4> colors{};
  SurfaceDesc depth;
  ResourceId shader_a = 0, shader_b = 0;
  std::shared_ptr<const ShaderCapture> vertex_shader, pixel_shader;
  std::array<std::shared_ptr<const TextureCapture>, 32> textures;
  // Replay: the command's own array, alive as long as `memory` holds the frozen batch, instead of copying
  // 32 shared_ptrs (an atomic count update per bound texture) into every packet. Read through Texture().
  const std::array<std::shared_ptr<const TextureCapture>, 32>* command_textures = nullptr;
  const std::shared_ptr<const TextureCapture>& Texture(uint32_t slot) const {
    return command_textures ? (*command_textures)[slot] : textures[slot];
  }
  // Texture fetch slots the shaders can read (shader_slots.h): a bound slot outside it is not a
  // texture of this draw. ~0u when a shader is unknown.
  uint32_t texture_slots = ~0u;
  std::vector<std::pair<uint32_t, std::string>> texture_errors;
  uint64_t command_serial = 0;
  bool has_device_state = false;
};
struct ClearPacket {
  uint32_t flags;
  std::array<float, 4> color;
  float depth;
  uint32_t stencil;
  std::vector<Rect> rects;
  CapturedMemory memory;
  std::array<SurfaceDesc,4> colors{};
  SurfaceDesc depth_surface;
};
struct ResolvePacket {
  uint64_t command_serial = 0;
  uint32_t flags, destination;
  bool has_source_rect=false,has_destination_point=false,tiling_active=false;
  bool has_copy_draw=false,copy_dest_swap=false;
  Rect source{};
  std::array<int32_t, 2> destination_point{};
  std::array<float, 4> clear_color{};
  float clear_depth = 0;
  uint32_t clear_stencil = 0;
  uint32_t level=0,slice=0;
  SurfaceDesc source_surface;
  SurfaceDesc clear_color_surface,clear_depth_surface;
  std::array<uint32_t,6> destination_fetch{};
  CapturedMemory memory;
};
struct SwapPacket {
  ResourceId frontbuffer;
  uint64_t guest_swap;
  CapturedMemory memory;
  std::shared_ptr<const std::array<uint32_t,256>> gamma;
  bool gamma_enabled=false;
};
struct PassPacket {
  Op operation;
  int pass;
  std::vector<Rect> rects;
  std::array<float, 4> color{};
  float depth = 0;
  uint32_t stencil = 0;
  bool clear_color = false;
  SurfaceDesc color_surface, depth_surface;
};
// Draws contain large constant/register arrays. Moving an inline variant
// copied those arrays again at every queue handoff and vector growth. Keep
// exclusive payload ownership so its address and captured state remain stable.
struct DrawPacketOwner {
  std::shared_ptr<DrawPacket> payload;
  DrawPacketOwner() = default;
  DrawPacketOwner(DrawPacket packet)
      : payload(std::make_shared<DrawPacket>(std::move(packet))) {}
  DrawPacketOwner(std::shared_ptr<DrawPacket> packet)
      : payload(std::move(packet)) {}
  DrawPacketOwner(const DrawPacketOwner&)=delete;
  DrawPacketOwner& operator=(const DrawPacketOwner&)=delete;
  DrawPacketOwner(DrawPacketOwner&&)=default;
  DrawPacketOwner& operator=(DrawPacketOwner&&)=default;
};
// One allocation for a batch's payloads instead of one large allocation per
// draw. Each alias keeps the block alive after replay recycles the WorkBatch.
// Next hands each freshly initialized element out at most once.
// The memory is recycled: a batch's packets are ~22 KB each and a city frame has
// ~2,700, so a fresh block per batch cost ~60 MB of new pages a frame (14.5 ms for
// 3,008 draws on the PC benchmark, 18-21 ms a frame on the phone). When the last
// alias goes the packets are destroyed at once (their captures and textures are
// let go as before) and the bare memory returns to a pool; the next arena
// constructs fresh packets in it, the same initial state as a new block.
class DrawPacketArena {
public:
  explicit DrawPacketArena(size_t count,bool replay_storage=false);
  DrawPacketArena(const DrawPacketArena&)=delete;
  DrawPacketArena& operator=(const DrawPacketArena&)=delete;
  DrawPacketArena(DrawPacketArena&&)=delete;
  DrawPacketArena& operator=(DrawPacketArena&&)=delete;
  DrawPacketOwner Next() {
    if(next_>=block_->count) throw std::out_of_range("Draw packet arena exhausted");
    return std::shared_ptr<DrawPacket>(block_,block_->packets+next_++);
  }
  struct Block {
    DrawPacket* packets=nullptr;
    size_t count=0,capacity=0;  // constructed packets, room in the memory
    Block()=default;
    Block(const Block&)=delete;
    Block& operator=(const Block&)=delete;
    ~Block();  // destroys the packets, returns the memory to the pool
  };
private:
  std::shared_ptr<Block> block_;
  size_t next_=0;
};
using RenderPacket = std::variant<DrawPacketOwner, ClearPacket, ResolvePacket,
                                  SwapPacket, PassPacket>;
inline DrawPacket* GetDraw(RenderPacket& packet) {
  auto* owner=std::get_if<DrawPacketOwner>(&packet);
  return owner?owner->payload.get():nullptr;
}
inline const DrawPacket* GetDraw(const RenderPacket& packet) {
  auto* owner=std::get_if<DrawPacketOwner>(&packet);
  return owner?owner->payload.get():nullptr;
}
bool DecodeRenderPacket(const WorkBatch &, const WorkCmd &,
                        const native::Pm4Mirror &, RenderPacket &,
                        std::string &);
// Optional storage must be a fresh payload returned by DrawPacketArena::Next.
// Optional counters belong to the replay owner; null performs no clock reads.
struct ReplayProbe { uint64_t capture_ns=0,pm4_ns=0,decode_ns=0; };
bool ReplayCapturedRenderPacket(const WorkBatch&,const WorkCmd&,native::Pm4Mirror&,RenderPacket&,std::string&,DrawPacketOwner storage={},std::shared_ptr<const WorkBatch> capture_owner={},ReplayProbe* probe=nullptr);
// Original scale1/MSAA1 shader ABI state. Descriptor indices are populated by
// the backend after binding resources; these semantic constants are common.
void FinalizeDrawConstants(DrawPacket &);
} // namespace superman_returns::graphics::guest
