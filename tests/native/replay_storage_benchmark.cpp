// Manual Release diagnostic: time arena construction and complete owned replay
// separately. Keep decoded payloads alive as the Vulkan packet queue does.
#include "../../port/src/graphics/guest/render_packet.h"
#include "../../port/src/native_renderer/pm4_mirror.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
using namespace superman_returns::graphics::guest;
int main() {
  constexpr size_t draws=3008,rounds=10;
  auto batch=std::make_shared<WorkBatch>();WorkCmd cmd;cmd.op=Op::kDraw;cmd.u[0]=4;cmd.u[2]=3;
  superman_returns::native::Pm4Mirror mirror;
  std::vector<double> setup,decode,total;uint64_t checksum=0;
  for(size_t round=0;round<rounds;++round) {
    std::vector<RenderPacket> packets;packets.reserve(draws);std::string error;
    const auto begin=std::chrono::steady_clock::now();
    DrawPacketArena arena(draws,true);
    const auto initialized=std::chrono::steady_clock::now();
    for(size_t i=0;i<draws;++i) {
      cmd.command_serial=i+1;RenderPacket packet;
      if(!ReplayCapturedRenderPacket(*batch,cmd,mirror,packet,error,arena.Next(),batch)) {
        std::fprintf(stderr,"Replay failed: %s\n",error.c_str());return 1;
      }
      packets.push_back(std::move(packet));
    }
    const auto filled=std::chrono::steady_clock::now();
    for(auto& packet:packets) {
      const auto* draw=GetDraw(packet);if(!draw) return 2;
      checksum+=draw->command_serial+draw->constants.shared[4095]+draw->PsWords()[0];
    }
    if(round>=2) {
      auto ms=[](auto a,auto b){return std::chrono::duration<double,std::milli>(b-a).count();};
      setup.push_back(ms(begin,initialized));decode.push_back(ms(initialized,filled));total.push_back(ms(begin,filled));
    }
  }
  auto median=[](auto values){std::sort(values.begin(),values.end());return (values[values.size()/2-1]+values[values.size()/2])/2;};
  if(checksum!=rounds*draws*(draws+1)/2) return 3;
  std::printf("replay_storage_bench draws=%zu rounds=%zu arena_ms=%.4f decode_ms=%.4f total_ms=%.4f checksum=%llu\n",
              draws,rounds-2,median(setup),median(decode),median(total),static_cast<unsigned long long>(checksum));
}
