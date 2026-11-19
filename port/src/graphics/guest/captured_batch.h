#pragma once
#include "shader_capture.h"
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
namespace superman_returns::graphics::guest {
struct TextureCapture;
template <typename T> struct DefaultInitAllocator : std::allocator<T> {
  template <typename U> struct rebind {
    using other = DefaultInitAllocator<U>;
  };
  DefaultInitAllocator() = default;
  template <typename U>
  DefaultInitAllocator(const DefaultInitAllocator<U> &) noexcept {}
  template <typename U>
  void construct(U *p) noexcept(std::is_nothrow_default_constructible_v<U>) {
    ::new (static_cast<void *>(p)) U;
  }
  template <typename U, typename... Args> void construct(U *p, Args &&...args) {
    std::allocator_traits<std::allocator<T>>::construct(
        static_cast<std::allocator<T> &>(*this), p,
        std::forward<Args>(args)...);
  }
};

struct CaptureRange {
  uint32_t address, length, offset;
};
// Empty means that a bulk decode would hide a changed newer partial overlay
// (or miss). Byte-identical newer intersections may borrow the complete span.
std::span<const uint8_t> FindUnambiguousCapture(std::span<const CaptureRange>,
    std::span<const uint8_t>,uint32_t address,uint32_t length);

struct BufferPlan {
  uint64_t key = 0;
  uint32_t address = 0, size = 0, decl = 0, stride = 0, format = 0, phase = 0;
  uint32_t reset_index = UINT32_MAX;
  uint8_t action =
      0; // 0 = use as is, 1 = upload [begin, end), 2 = create + upload all
  uint32_t begin = 0, end = 0;
};
struct StreamPlan {
  uint32_t stream = 0, offset = 0, size = 0, stride = 0;
  BufferPlan buffer;
};
enum class Op : uint8_t {
  kDraw,
  kDrawIndexed,
  kDrawInline,
  kResolve,
  kBeginTiling,
  kEndTiling,
  kSwap,
  kPassEnd,
  kRing,
  kClear
};
struct WorkCmd {
  Op op = Op::kRing;
  uint32_t device =
      0; // Captured device identity, independent of current guest globals.
  std::shared_ptr<const ShaderCapture> vertex_shader, pixel_shader;
  std::array<std::shared_ptr<const TextureCapture>, 32> textures;
  std::vector<std::pair<uint32_t, std::string>> texture_errors;
  uint64_t command_serial = 0;
  int pass = 0;
  uint32_t u[8] = {};
  float f = 0.0f;
  uint64_t u64 = 0;
  std::shared_ptr<const std::array<uint32_t,256>> gamma;
  bool gamma_enabled=false;
  uint32_t ring_offset = 0, ring_bytes = 0;
  uint32_t range_first = 0, range_count = 0;
  uint32_t stream_first = 0, stream_count = 0;
  bool streams_ok = true;
  bool pm4_capture_ok = true;
  bool resolve_copy_draw=false;
  uint32_t resolve_copy_dest_info=0;
  bool tiling_active = false;
  bool packet_check = false;
  bool has_index = false;
  bool index32 = false;
  uint32_t index_size = 0;
  BufferPlan index;
};
struct WorkBatch {
  std::vector<WorkCmd> cmds;
  std::vector<uint8_t, DefaultInitAllocator<uint8_t>> bytes;
  std::vector<CaptureRange> ranges;
  std::vector<StreamPlan> streams;
  void Clear() {
    cmds.clear();
    bytes.clear();
    ranges.clear();
    streams.clear();
  }
};

// Frozen ownership boundary for neutral render packets. Unlike the legacy
// renderer's captured-load shim, a miss never reads live guest memory.
class CapturedMemory {
public:
  static bool Capture(const WorkBatch &, const WorkCmd &, CapturedMemory &,
                      std::string &);
  static bool CaptureOwned(std::shared_ptr<const WorkBatch>,const WorkCmd&,
                           CapturedMemory&,std::string&);
  std::span<const uint8_t> Read(uint32_t address, uint32_t length) const;
  std::span<const uint8_t> ReadUnambiguous(uint32_t address,uint32_t length) const;
  // CaptureOwned: the frozen batch (and so its commands) lives as long as this memory.
  bool OwnsBatch() const {return bool(batch_);}

private:
  struct Snapshot {
    std::vector<uint8_t> bytes;
    std::vector<CaptureRange> ranges;
  };
  std::shared_ptr<const Snapshot> snapshot_;
  // CaptureOwned requires a frozen batch that no producer can mutate. Reads
  // remain limited to this command's range slice even though bytes are shared.
  std::shared_ptr<const WorkBatch> batch_;
  uint32_t range_first_=0,range_count_=0;
  std::span<const CaptureRange> Ranges() const;
  std::span<const uint8_t> Bytes() const;
};
constexpr bool IsCaptureFrame(uint64_t frame,uint32_t every) {
  return frame % (every ? every : 1) == 0;
}
} // namespace superman_returns::graphics::guest
