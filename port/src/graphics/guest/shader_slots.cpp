#include "shader_slots.h"

#include <algorithm>

// Layouts from XenosRecomp (shader.h, shader_code.h, constant_table.h): big-endian words; the
// bitfields count from the low bit of each word once it is in host order.
namespace superman_returns::graphics::guest {
namespace {
constexpr uint32_t kAll = ~0u;

struct Reader {
  std::span<const uint8_t> bytes;
  bool ok = true;
  uint32_t U32(size_t at) {
    if (at + 4 > bytes.size()) return ok = false, 0;
    return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 | uint32_t(bytes[at + 2]) << 8 | bytes[at + 3];
  }
  uint32_t U16(size_t at) {
    if (at + 2 > bytes.size()) return ok = false, 0;
    return uint32_t(bytes[at]) << 8 | bytes[at + 1];
  }
};

bool IsExec(uint32_t opcode) {
  // Exec, ExecEnd, CondExec, CondExecEnd, CondExecPred, CondExecPredEnd, CondExecPredClean(End)
  return (opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14;
}
}  // namespace

uint32_t ShaderTextureSlots(std::span<const uint8_t> container) {
  Reader r{container};
  // ShaderContainer: flags, virtualSize, physicalSize, fieldC, constantTableOffset,
  // definitionTableOffset, shaderOffset.
  if ((r.U32(0) & 0xFFFFFF00u) != 0x102A1100u) return kAll;
  const uint32_t virtual_size = r.U32(4), constant_table = r.U32(16), shader = r.U32(24);
  if (!r.ok) return kAll;
  uint32_t mask = 0;

  // Constant table (after its container's size word): ConstantTable {size, creator, version,
  // constants, constantInfo, flags, target}; ConstantInfo (20 bytes) {name, registerSet (u16),
  // registerIndex (u16), registerCount (u16), reserved, typeInfo, defaultValue}. Sampler set = 3.
  if (constant_table) {
    const size_t table = size_t(constant_table) + 4;
    const uint32_t constants = r.U32(table + 12), info = r.U32(table + 16);
    if (!r.ok || constants > 4096) return kAll;
    for (uint32_t i = 0; i < constants; ++i) {
      const size_t at = table + info + size_t(i) * 20;
      if (r.U16(at + 4) != 3) continue;
      const uint32_t first = r.U16(at + 6), count = r.U16(at + 8);
      for (uint32_t s = first; s < first + count && s < 32; ++s) mask |= 1u << s;
    }
    if (!r.ok) return kAll;
  }

  // Shader {physicalOffset, size}; the microcode starts virtualSize + physicalOffset into the
  // container: control flow pairs (two 48-bit instructions in three words) until the first
  // exec's instructions, then 3-word ALU/fetch instructions.
  const uint32_t physical = r.U32(size_t(shader)), size = r.U32(size_t(shader) + 4);
  if (!r.ok) return kAll;
  const size_t code = size_t(virtual_size) + physical;
  if (code + size > container.size()) return kAll;
  size_t cf_end = size;  // bytes of control flow: shrinks to the lowest exec address
  for (size_t at = 0; at + 12 <= cf_end; at += 12) {
    const uint32_t w0 = r.U32(code + at), w1 = r.U32(code + at + 4), w2 = r.U32(code + at + 8);
    const uint32_t cf[2][2]{{w0, w1 & 0xFFFF}, {(w1 >> 16) | (w2 << 16), w2 >> 16}};
    for (const auto& c : cf) {
      if (!IsExec((c[1] >> 12) & 15)) continue;
      const uint32_t address = c[0] & 0xFFF, count = (c[0] >> 12) & 7, sequence = (c[0] >> 16) & 0xFFF;
      cf_end = std::min(cf_end, size_t(address) * 12);
      for (uint32_t i = 0; i < count; ++i) {
        if (!((sequence >> (i * 2)) & 1)) continue;  // an ALU instruction
        const size_t instr = code + (size_t(address) + i) * 12;
        if (instr + 12 > code + size) return kAll;
        const uint32_t word = r.U32(instr);
        const uint32_t opcode = word & 31;  // 0: vertex fetch, the rest read a texture fetch constant
        if (opcode != 0) mask |= 1u << ((word >> 20) & 31);
      }
    }
  }
  return r.ok ? mask : kAll;
}
}  // namespace superman_returns::graphics::guest
