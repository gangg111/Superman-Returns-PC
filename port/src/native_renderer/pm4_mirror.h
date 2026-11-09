// Ported from crazyriddler/rexglue-native-kit @136bc6c4,
// reference/conan/port/src/native/pm4_mirror.h (Conan native renderer).
// The kit ships no license file at that revision; parts derived from Xenia /
// ReXGlue keep their BSD license. Changes for Superman Returns are listed in
// docs/native-port-plan.md section 2.
//
// Mirror of the Xenos register state as the GPU will see it, rebuilt from the
// PM4 packets the XDK writes into its command segments.
//
// The XDK device shadow (dev+0x700 constants, dev+0x400 fetch constants) does
// not see every write: shader literal constants (LOAD_ALU_CONSTANT at shader
// bind), GpuBeginShaderConstantF4 / inline SET_CONSTANT packets and inline
// type-0 constant blocks (SpeedTree tables, per-object lights) go straight to
// the command buffer. Parsing the packets between draws gives the exact state.
#pragma once

#include <cstdint>
#include <array>
#include <cstring>
#include <functional>
#include <span>

namespace superman_returns::native {

class Pm4Mirror {
public:
  using MemoryReader =
      std::function<std::span<const uint8_t>(uint32_t, uint32_t)>;
  static constexpr uint32_t kRegisterCount = 0x5003;
  static constexpr uint32_t kAluConstantBase =
      0x4000; // VS c0-255, PS c256-511 (x4 dwords)
  static constexpr uint32_t kFetchConstantBase = 0x4800;
  static constexpr uint32_t kBoolConstantBase = 0x4900;
  static constexpr uint32_t kLoopConstantBase = 0x4908;

  // Parses the command stream between guest virtual addresses [begin, end).
  void Scan(const uint8_t *base, uint32_t begin, uint32_t end, int depth = 0) {
    ScanFrom(base, base, begin, end, depth);
  }
  // Parses a copy of a command segment (`size` bytes at `words`, big-endian);
  // packets that reference memory (LOAD_ALU_CONSTANT, INDIRECT_BUFFER) read
  // guest memory through `base`.
  void ScanCopy(const uint8_t *base, const uint8_t *words, uint32_t size) {
    ScanFrom(base, words, 0, size, 0);
  }
  // Same parser, but every referenced byte comes from an owned capture.
  // Returned spans must remain valid for the entire (possibly nested) scan.
  void ScanCopyUsing(const uint8_t *words, uint32_t size,
                     const MemoryReader &reader) {
    ScanFrom(nullptr, words, 0, size, 0, &reader);
  }

  uint32_t reg(uint32_t index) const {
    return index < kRegisterCount ? regs_[index] : 0;
  }
  const uint32_t *regs() const { return regs_; }
  // Owned by the same recording thread as this mirror. Callers copy the bank
  // into packets/uploads; the raw GPU register values remain untouched.
  const std::array<uint32_t,1024>& FloatConstants(bool pixel) const;
  // Registers written at least once since startup (0x4000-0x4A00 range
  // tracked).
  bool written(uint32_t index) const {
    return index >= kAluConstantBase &&
           index < kAluConstantBase + kTrackedCount &&
           written_[index - kAluConstantBase];
  }

  uint64_t packets = 0;
  uint64_t dwords = 0;
  uint64_t unknown_packets = 0;
  uint64_t indirect_buffers = 0;
  // LOAD_ALU_CONSTANT sources that were not readable host memory (skipped).
  uint64_t unreadable_alu_loads = 0;
  uint64_t unreadable_indirect_buffers = 0;
  bool follow_indirect = true;
  // RB_COPY_* state captured at the last resolve (copy-mode draw); the XDK
  // resets these registers right after the copy.
  uint32_t last_copy_dest_info = 0;
  uint32_t last_copy_dest_base = 0;
  uint64_t copy_draws = 0;
  // Bumped when VS / PS float constant contents change (buffer reuse).
  uint64_t vs_version = 1, ps_version = 1;
  uint64_t indirect_dwords = 0;

private:
  // Main stream read from `stream` + address; memory references via `base`.
  void ScanFrom(const uint8_t *base, const uint8_t *stream, uint32_t begin,
                uint32_t end, int depth, const MemoryReader *reader = nullptr);
  static constexpr uint32_t kTrackedCount = 0xA00;
  void WriteRange(uint32_t first,const uint8_t* big_endian_words,uint32_t count);
  void Write(uint32_t index, uint32_t value) {
    if (index >= kRegisterCount)
      return;
    if (index - kAluConstantBase < 0x800u && regs_[index] != value) {
      ++(index < kAluConstantBase + 0x400 ? vs_version : ps_version);
      MarkDirty(index - kAluConstantBase);
    }
    regs_[index] = value;
    if (index >= kAluConstantBase && index < kAluConstantBase + kTrackedCount) {
      written_[index - kAluConstantBase] = true;
    }
  }
  uint32_t regs_[kRegisterCount] = {};
  bool written_[kTrackedCount] = {};
  mutable std::array<std::array<uint32_t,1024>,2> float_cache_{};
  mutable std::array<uint64_t,2> float_cache_version_{};
  // Float constants changed since FloatConstants last sanitized them (bit per
  // register, vertex bank then pixel bank): only those are redone (a draw
  // usually changes a few world-matrix registers, not all 1024).
  mutable std::array<uint64_t,32> dirty_{};
  mutable std::array<bool,2> float_cache_full_{};
  void MarkDirty(uint32_t alu_index) const { dirty_[alu_index >> 6] |= 1ull << (alu_index & 63); }
};

} // namespace superman_returns::native
