#pragma once
#include "../guest/render_packet.h"
#include <array>
#include <cstring>
namespace superman_returns::graphics::vulkan {
// Comparisons use bounded host storage, never mapped write-combined memory.
class ConstantBankCache {
public:
  struct Reuse {bool vs=false,ps=false;uint32_t vs_offset=0,ps_offset=0;};
  // A draw's two banks: 1,024 words each, where DrawPacket::VsWords / PsWords point (the packet's own
  // arrays or the replay's shared banks).
  struct Banks {const uint32_t* vs;const uint32_t* ps;};
  static Banks Of(const guest::DrawPacket& draw) {return {draw.VsWords(),draw.PsWords()};}
  void Reset() {chunk_=nullptr;serial_=0;}
  // A bank with a known mirror version (DrawPacket::vs_version, 0 unknown) is compared by version,
  // else by content: the two 4 KB compares and copies per draw were a good part of the recorder's copying.
  Reuse Find(uint64_t serial,const void* chunk,Banks value,uint64_t vs_version=0,uint64_t ps_version=0) const {
    if(!chunk || chunk!=chunk_ || !serial || serial!=serial_) return {};
    return {Same(vs_,vs_version_,value.vs,vs_version),Same(ps_,ps_version_,value.ps,ps_version),vs_offset_,ps_offset_};
  }
  void Commit(uint64_t serial,const void* chunk,uint32_t offset,Banks value,Reuse reuse,uint64_t vs_version=0,uint64_t ps_version=0) {
    if(chunk!=chunk_ || serial!=serial_) reuse={};
    if(!reuse.vs) {Keep(vs_,vs_version_,value.vs,vs_version);vs_offset_=offset;}
    if(!reuse.ps) {Keep(ps_,ps_version_,value.ps,ps_version);ps_offset_=offset;}
    serial_=serial;chunk_=chunk;
  }
  Reuse Find(uint64_t serial,const void* chunk,const guest::ConstantSnapshot& value,uint64_t vs_version=0,uint64_t ps_version=0) const {
    return Find(serial,chunk,Banks{value.vs.data(),value.ps.data()},vs_version,ps_version);
  }
  void Commit(uint64_t serial,const void* chunk,uint32_t offset,const guest::ConstantSnapshot& value,Reuse reuse,uint64_t vs_version=0,uint64_t ps_version=0) {
    Commit(serial,chunk,offset,Banks{value.vs.data(),value.ps.data()},reuse,vs_version,ps_version);
  }
private:
  using Bank=std::array<uint32_t,1024>;
  static bool Same(const Bank& kept,uint64_t kept_version,const uint32_t* bank,uint64_t version) {
    if(kept_version && version) return kept_version==version;
    return !kept_version && !version && std::memcmp(kept.data(),bank,sizeof(Bank))==0;
  }
  static void Keep(Bank& kept,uint64_t& kept_version,const uint32_t* bank,uint64_t version) {
    kept_version=version;
    if(!version) std::memcpy(kept.data(),bank,sizeof(Bank));  // compared by content later
  }
  Bank vs_{},ps_{};
  uint64_t vs_version_=0,ps_version_=0;
  uint64_t serial_=0;const void* chunk_=nullptr;uint32_t vs_offset_=0,ps_offset_=0;
};
}
