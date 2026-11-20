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

// Calls f(fetch, words) for every instruction the exec clauses run. Shader {physicalOffset, size};
// the microcode starts virtualSize + physicalOffset into the container: control flow pairs (two
// 48-bit instructions in three words) until the first exec's instructions, then 3-word ALU/fetch
// instructions. False when the container cannot be read.
template <class F>
bool ForEachInstruction(Reader& r, uint32_t virtual_size, uint32_t shader, F&& f) {
  const uint32_t physical = r.U32(size_t(shader)), size = r.U32(size_t(shader) + 4);
  if (!r.ok) return false;
  const size_t code = size_t(virtual_size) + physical;
  if (code + size > r.bytes.size()) return false;
  size_t cf_end = size;  // bytes of control flow: shrinks to the lowest exec address
  for (size_t at = 0; at + 12 <= cf_end; at += 12) {
    const uint32_t w0 = r.U32(code + at), w1 = r.U32(code + at + 4), w2 = r.U32(code + at + 8);
    const uint32_t cf[2][2]{{w0, w1 & 0xFFFF}, {(w1 >> 16) | (w2 << 16), w2 >> 16}};
    for (const auto& c : cf) {
      if (!IsExec((c[1] >> 12) & 15)) continue;
      const uint32_t address = c[0] & 0xFFF, count = (c[0] >> 12) & 7, sequence = (c[0] >> 16) & 0xFFF;
      cf_end = std::min(cf_end, size_t(address) * 12);
      for (uint32_t i = 0; i < count; ++i) {
        const size_t instr = code + (size_t(address) + i) * 12;
        if (instr + 12 > code + size) return false;
        const uint32_t words[3]{r.U32(instr), r.U32(instr + 4), r.U32(instr + 8)};
        f(((sequence >> (i * 2)) & 1) != 0, words);  // the sequence bit: a fetch instruction
      }
    }
  }
  return r.ok;
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

  const bool ok = ForEachInstruction(r, virtual_size, shader, [&](bool fetch, const uint32_t (&w)[3]) {
    const uint32_t opcode = w[0] & 31;  // 0: vertex fetch, the rest read a texture fetch constant
    if (fetch && opcode != 0) mask |= 1u << ((w[0] >> 20) & 31);
  });
  return ok ? mask : kAll;
}

ShaderConstantUse ShaderFloatConstants(std::span<const uint8_t> container) {
  Reader r{container};
  if ((r.U32(0) & 0xFFFFFF00u) != 0x102A1100u) return {};
  const uint32_t virtual_size = r.U32(4), shader = r.U32(24);
  if (!r.ok) return {};
  ShaderConstantUse use;
  bool relative = false;
  auto add = [&](uint32_t reg) { use.used[reg >> 6] |= uint64_t(1) << (reg & 63); };
  // AluInstruction (XenosRecomp shader_code.h). Word 0: scalarOpcode in bits 26-31. Word 1: bit 29
  // constAddressRegisterRelative, 30 const1Relative, 31 const0Relative. Word 2: src3/src2/src1
  // register in bits 0-7/8-15/16-23, src3/src2/src1 select (1 = temporary) in bits 29/30/31.
  const bool ok = ForEachInstruction(r, virtual_size, shader, [&](bool fetch, const uint32_t (&w)[3]) {
    if (fetch) return;
    bool constant = false;
    for (uint32_t s = 0; s < 3; ++s) {
      if ((w[2] >> (29 + s)) & 1) continue;  // a temporary
      add((w[2] >> (8 * s)) & 0xFF);
      constant = true;
    }
    // mulsc/addsc/subsc (scalar opcodes 42-47) read src3's register as a constant whatever its select.
    if (const uint32_t scalar = w[0] >> 26; scalar >= 42 && scalar <= 47) {
      add(w[2] & 0xFF);
      constant = true;
    }
    // A constant read through a0/aL can reach any register (shader_recompiler.cpp: CONST_REL).
    if (constant && (w[1] >> 30)) relative = true;
  });
  // The port's own code in the shaders (xenosrecomp patches 0033-0037: day/night tint, night windows,
  // water, rooms, glow) reads c200-c211, which native_renderer.cpp writes into the banks; the game's
  // microcode does not mention them. tools/shaders/check_shader_constants.py fails if the HLSL reads more.
  for (uint32_t reg = 200; reg <= 211; ++reg) add(reg);
  use.all = !ok || relative;
  return use;
}
}  // namespace superman_returns::graphics::guest
