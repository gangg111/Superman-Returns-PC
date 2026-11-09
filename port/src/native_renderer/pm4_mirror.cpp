// Ported from crazyriddler/rexglue-native-kit @136bc6c4,
// reference/conan/port/src/native/pm4_mirror.cpp (Conan native renderer).
// The kit ships no license file at that revision; parts derived from Xenia /
// ReXGlue keep their BSD license. Changes for Superman Returns are listed in
// docs/native-port-plan.md section 2.
//
#include "pm4_mirror.h"
#include "../graphics/guest/constant_snapshot.h"
#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <immintrin.h>
#include <rex/logging.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace superman_returns::native {

const std::array<uint32_t,1024>& Pm4Mirror::FloatConstants(bool pixel) const {
  const uint32_t bank=pixel?1:0;
  const uint64_t version=pixel?ps_version:vs_version;
  if(float_cache_version_[bank]!=version) {
    const uint32_t* src=regs_+kAluConstantBase+bank*1024;
    if(!float_cache_full_[bank]) {
      std::string error;
      graphics::guest::CaptureFloatConstants({src,1024},false,float_cache_[bank],error);
      float_cache_full_[bank]=true;
      for(uint32_t w=0;w<16;++w) dirty_[bank*16+w]=0;
    } else {
      // Only the registers written with a new value since the last call, with
      // the same NaN rule as CaptureFloatConstants (NaN -> 0).
      for(uint32_t w=0;w<16;++w) {
        uint64_t bits=dirty_[bank*16+w];
        dirty_[bank*16+w]=0;
        while(bits) {
          const uint32_t i=w*64+uint32_t(__builtin_ctzll(bits));
          bits&=bits-1;
          const uint32_t v=src[i];
          float_cache_[bank][i]=((v&0x7f800000)==0x7f800000&&(v&0x007fffff))?0:v;
        }
      }
      // SR_FLOAT_CONST_CHECK=1: the incremental result against a full pass.
      static const bool check=[]{const char* e=std::getenv("SR_FLOAT_CONST_CHECK");return e&&*e=='1';}();
      if(check) {
        static uint64_t checked=0,mismatched=0;
        std::array<uint32_t,1024> full;
        std::string error;
        graphics::guest::CaptureFloatConstants({src,1024},false,full,error);
        ++checked;
        if(full!=float_cache_[bank]) ++mismatched;
        if((checked&0xFFFF)==0) REXLOG_INFO("pm4 float constants: {} incremental updates checked, {} mismatched",checked,mismatched);
      }
    }
    float_cache_version_[bank]=version;
  }
  return float_cache_[bank];
}

namespace {

inline uint32_t LoadBE(const uint8_t *base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

// 0xA0000000 is the 1:1 cached view of guest physical memory.
constexpr uint32_t kPhysicalView = 0xA0000000u;

// Copies [p, p + bytes) when it is committed, readable host memory, else
// returns null. The packets are scanned from guest command buffers that can
// already be recycled or hold a stale address; a read there must not take the
// process down. ReadProcessMemory checks only the requested pages: VirtualQuery
// walks the 512 MB physical mapping and took an eighth of the render worker.
const uint8_t *CopyIfReadable(const uint8_t *p, size_t bytes) {
  // LOAD_ALU_CONSTANT sizes are 12 bits of dwords.
  thread_local uint8_t copy[0x1000 * 4];
  if (bytes > sizeof(copy)) return nullptr;
#ifdef _WIN32
  // Pages ReadProcessMemory found readable in the last 500 ms are copied
  // directly: the system call per constant packet (one per draw) was 6% of the
  // render worker. A page not seen recently goes through the check again.
  thread_local std::unordered_map<uintptr_t, uint64_t> verified;
  const uint64_t now = GetTickCount64();
  const uintptr_t first_page = uintptr_t(p) >> 12, last_page = (uintptr_t(p) + bytes - 1) >> 12;
  bool known = bytes > 0;
  for (uintptr_t page = first_page; known && page <= last_page; ++page) {
    const auto it = verified.find(page);
    known = it != verified.end() && now - it->second < 500;
  }
  if (known) {
    std::memcpy(copy, p, bytes);
    return copy;
  }
  SIZE_T copied = 0;
  if (!ReadProcessMemory(GetCurrentProcess(), p, copy, bytes, &copied) || copied != bytes)
    return nullptr;
  for (uintptr_t page = first_page; page <= last_page; ++page) verified[page] = now;
  if (verified.size() > 65536) verified.clear();
#else
  std::memcpy(copy, p, bytes);
#endif
  return copy;
}

} // namespace

void Pm4Mirror::WriteRange(uint32_t first,const uint8_t* words,uint32_t count) {
  // Constant packets usually write an entire bank. Keep the same number of
  // version changes, but reduce it once per bank so endian conversion and
  // comparison can vectorize instead of branching on every register.
  if(first>=kAluConstantBase && first<kAluConstantBase+0x800 &&
     count<=kAluConstantBase+0x800-first) {
    while(count) {
      const bool pixel=first>=kAluConstantBase+0x400;
      const uint32_t end=kAluConstantBase+(pixel?0x800:0x400);
      const uint32_t length=std::min(count,end-first);
      uint32_t changes=0;
      auto* destination=regs_+first;
      size_t i=0;
      // Four registers at a time: swap, compare, store; only the changed ones
      // are marked dirty (the scalar loop was 5% of the render worker).
      const __m128i swap=_mm_setr_epi8(3,2,1,0,7,6,5,4,11,10,9,8,15,14,13,12);
      // The SSE loop hangs the GPU right after the city loads (4 of 4 runs,
      // cause not found yet): off unless SR_PM4_SIMD=1.
      static const bool scalar=[]{const char* e=std::getenv("SR_PM4_SIMD");return !(e&&*e=='1');}();
      static const bool check=[]{const char* e=std::getenv("SR_PM4_SIMD_CHECK");return e&&*e=='1';}();
      uint32_t check_regs[0x400];
      std::array<uint64_t,32> check_dirty{};
      uint32_t check_changes=0;
      if(check) {
        std::memcpy(check_regs,destination,length*4);
        check_dirty=dirty_;
        for(size_t k=0;k<length;++k) {
          uint32_t v;std::memcpy(&v,words+k*4,4);v=__builtin_bswap32(v);
          if(check_regs[k]!=v) {++check_changes;const uint32_t a=first-kAluConstantBase+uint32_t(k);check_dirty[a>>6]|=1ull<<(a&63);}
          check_regs[k]=v;
        }
      }
      for(;!scalar&&i+4<=length;i+=4) {
        const __m128i value=_mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(words+i*4)),swap);
        const __m128i old=_mm_loadu_si128(reinterpret_cast<const __m128i*>(destination+i));
        uint32_t differ=~uint32_t(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(value,old))))&0xF;
        if(differ) {
          _mm_storeu_si128(reinterpret_cast<__m128i*>(destination+i),value);
          changes+=uint32_t(__builtin_popcount(differ));
          while(differ) {
            MarkDirty(first-kAluConstantBase+uint32_t(i)+uint32_t(__builtin_ctz(differ)));
            differ&=differ-1;
          }
        }
      }
      for(;i<length;++i) {
        uint32_t value;
        std::memcpy(&value,words+i*4,4);
        value=__builtin_bswap32(value);
        if(destination[i]!=value) {
          ++changes;
          MarkDirty(first-kAluConstantBase+uint32_t(i));
        }
        destination[i]=value;
      }
      if(check) {
        static uint64_t checked=0,bad=0;
        ++checked;
        if(std::memcmp(check_regs,destination,length*4)||check_dirty!=dirty_||check_changes!=changes) {
          if(bad++<10) REXLOG_WARN("pm4 simd: mismatch first {:X} length {} changes {}/{} regs {} dirty {} words {}",
                                   first,length,changes,check_changes,
                                   std::memcmp(check_regs,destination,length*4)!=0,check_dirty!=dirty_,
                                   static_cast<const void*>(words));
        }
        if((checked&0x3FFFF)==0) REXLOG_INFO("pm4 simd: {} ranges checked, {} differed",checked,bad);
      }
      std::memset(written_+first-kAluConstantBase,1,length);
      (pixel?ps_version:vs_version)+=changes;
      first+=length;words+=length*4;count-=length;
    }
    return;
  }
  for(uint32_t i=0;i<count;++i) Write(first+i,LoadBE(words,i*4));
}

void Pm4Mirror::ScanFrom(const uint8_t *base, const uint8_t *stream,
                         uint32_t begin, uint32_t end, int depth,
                         const MemoryReader *reader) {
  uint32_t p = begin;
  while (p + 4 <= end) {
    uint32_t header = LoadBE(stream, p);
    p += 4;
    ++packets;
    switch (header >> 30) {
    case 0: {
      uint32_t count = ((header >> 16) & 0x3FFF) + 1;
      uint32_t index = header & 0x7FFF;
      bool one_register = (header >> 15) & 1;
      if (p + count * 4 > end)
        return;
      if(one_register) for(uint32_t i=0;i<count;++i) Write(index,LoadBE(stream,p+4*i));
      else WriteRange(index,stream+p,count);
      p += count * 4;
      dwords += count;
      break;
    }
    case 1: {
      if (p + 8 > end)
        return;
      Write(header & 0x7FF, LoadBE(stream, p));
      Write((header >> 11) & 0x7FF, LoadBE(stream, p + 4));
      p += 8;
      break;
    }
    case 2:
      break;
    case 3: {
      uint32_t opcode = (header >> 8) & 0x7F;
      uint32_t count = ((header >> 16) & 0x3FFF) + 1;
      if (p + count * 4 > end)
        return;
      uint32_t d0 = count > 0 ? LoadBE(stream, p) : 0;
      switch (opcode) {
      case 0x2D: { // SET_CONSTANT
        uint32_t index = d0 & 0x7FF;
        uint32_t first;
        switch ((d0 >> 16) & 0xFF) {
        case 0:
          first = kAluConstantBase + index;
          break;
        case 1:
          first = kFetchConstantBase + index;
          break;
        case 2:
          first = kBoolConstantBase + index;
          break;
        case 3:
          first = kLoopConstantBase + index;
          break;
        case 4:
          first = 0x2000 + index;
          break;
        default:
          first = kRegisterCount;
          break;
        }
        WriteRange(first,stream+p+4,count-1);
        break;
      }
      case 0x55:   // SET_CONSTANT2
      case 0x56: { // SET_SHADER_CONSTANTS
        uint32_t index = d0 & 0xFFFF;
        WriteRange(index,stream+p+4,count-1);
        break;
      }
      case 0x2F: { // LOAD_ALU_CONSTANT (from memory)
        if (count < 3)
          break;
        uint32_t address = d0 & 0x3FFFFFFF;
        uint32_t offset_type = LoadBE(stream, p + 4);
        uint32_t size = LoadBE(stream, p + 8) & 0xFFF;
        uint32_t index = offset_type & 0x7FF;
        uint32_t first;
        switch ((offset_type >> 16) & 0xFF) {
        case 0:
          first = kAluConstantBase + index;
          break;
        case 1:
          first = kFetchConstantBase + index;
          break;
        case 2:
          first = kBoolConstantBase + index;
          break;
        case 3:
          first = kLoopConstantBase + index;
          break;
        case 4:
          first = 0x2000 + index;
          break;
        default:
          first = kRegisterCount;
          break;
        }
        uint32_t src = kPhysicalView + (address & 0x1FFFFFFF);
        const uint8_t *words = nullptr;
        if (reader) {
          std::span<const uint8_t> captured = (*reader)(src, size * 4);
          if (captured.size() >= size * 4) words = captured.data();
        } else {
          words = CopyIfReadable(base + src, size * 4);
        }
        if (!words) {
          ++unreadable_alu_loads;
          break;
        }
        WriteRange(first,words,size);
        break;
      }
      case 0x22:                        // DRAW_INDX
      case 0x36: {                      // DRAW_INDX_2
        if ((regs_[0x2208] & 7) == 6) { // RB_MODECONTROL edram_mode = copy
          last_copy_dest_info = regs_[0x231B];
          last_copy_dest_base = regs_[0x2319];
          ++copy_draws;
        }
        break;
      }
      case 0x3F:   // INDIRECT_BUFFER
      case 0x37: { // INDIRECT_BUFFER_PFD
        ++indirect_buffers;
        if (count < 2 || depth > 2 || !follow_indirect)
          break;
        uint32_t address = kPhysicalView + (d0 & 0x1FFFFFFF);
        uint32_t size = LoadBE(stream, p + 4) & 0xFFFFF;
        if (size > (1u << 18))
          break; // not a plausible command buffer
        indirect_dwords += size;
        if (reader) {
          auto captured = (*reader)(address, size * 4);
          if (captured.size() >= size * 4)
            ScanFrom(nullptr, captured.data(), 0, size * 4, depth + 1, reader);
          else
            ++unreadable_indirect_buffers;
        } else {
          ScanFrom(base, base, address, address + size * 4, depth + 1);
        }
        break;
      }
      default:
        break;
      }
      p += count * 4;
      dwords += count;
      break;
    }
    }
  }
}

} // namespace superman_returns::native
