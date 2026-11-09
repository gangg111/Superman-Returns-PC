#pragma once
#include <rex/ppc.h>
#include <cstdint>
#include <cstring>

namespace superman_returns::native {

// The XDK SetVertexShaderConstantF / SetPixelShaderConstantF leaf
// (sub_820F4920 / sub_820F4A30): copies r6 float4s from r5 to
// dev + (r4 + 112 | 368) * 16, stores r3 at [r1-32] and r7 at [r1+48], then ORs
// the r7 dirty mask into dev+0 (VS) / dev+8 (PS). The generated code moves each
// float4 through lvlx/lvrx byte shuffles: 7% of the game thread with full
// Metropolis drawing. Volatile registers the caller cannot rely on are left
// as they are, except r9-r11, which get the values the original ends with.
struct ConstantCopyArgs {
  uint64_t device, start, source, count, mask, stack;
};

inline bool ConstantCopyEligible(const ConstantCopyArgs& a,bool pixel,uint64_t& destination) {
  if(!a.count || a.count>256 || a.start>256-a.count || (a.device&15) ||
     a.device>=0xa0000000ull || a.source>=0xa0000000ull) return false;
  destination=a.device+((pixel?368ull:112ull)+a.start)*16;
  const uint64_t bytes=a.count*16,stack=a.stack;
  // Physical aliases and address-wrap cases keep the generated path.
  if(a.source+bytes>0xa0000000ull || destination+bytes>0xa0000000ull ||
     stack<32 || stack+56>0xa0000000ull) return false;
  auto overlap=[](uint64_t x,uint64_t n,uint64_t y,uint64_t m) {return x<y+m && y<x+n;};
  return !(overlap(a.source,bytes,destination,bytes) ||
           overlap(a.source,bytes,stack-32,4) || overlap(a.source,bytes,stack+48,8) ||
           overlap(destination,bytes,stack-32,4) || overlap(destination,bytes,stack+48,8) ||
           overlap(destination,bytes,a.device+(pixel?8:0),8));
}

inline void ConstantCopyNative(PPCContext& ctx,uint8_t* base,const ConstantCopyArgs& a,
                               bool pixel,uint64_t destination) {
  std::memcpy(base+destination,base+a.source,size_t(a.count*16));
  const uint32_t device=uint32_t(a.device),stack=uint32_t(a.stack);
  const uint32_t device_be=__builtin_bswap32(device);
  std::memcpy(base+stack-32,&device_be,4);
  const uint64_t mask_be=__builtin_bswap64(a.mask);
  std::memcpy(base+stack+48,&mask_be,8);
  uint64_t dirty;
  std::memcpy(&dirty,base+device+(pixel?8:0),8);
  dirty=__builtin_bswap64(__builtin_bswap64(dirty)|a.mask);
  std::memcpy(base+device+(pixel?8:0),&dirty,8);
  ctx.r11.u64=device;
  ctx.r9.u64=a.mask;
  ctx.r10.u64=__builtin_bswap64(dirty);
}

// Kept for the SR_NATIVE_XDK_BULK_COPY prefix experiment.
inline bool TryCopyConstantPrefix(PPCContext& ctx,uint8_t* base,bool pixel) {
  const ConstantCopyArgs a{ctx.r3.u64,ctx.r4.u64,ctx.r5.u64,ctx.r6.u64,ctx.r7.u64,ctx.r1.u32};
  uint64_t destination=0;
  if(!base || a.count<8 || !ConstantCopyEligible(a,pixel,destination)) return false;
  const uint64_t prefix=(a.count/4-1)*4;
  std::memcpy(base+destination,base+a.source,size_t(prefix*16));
  ctx.r4.u64+=prefix;ctx.r5.u64+=prefix*16;ctx.r6.u64-=prefix;
  return true;
}
}
