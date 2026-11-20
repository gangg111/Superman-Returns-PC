#pragma once
// SR_GPU_PASSES=1 (measuring only): GPU time of every render pass, resolve and clear of a frame, from a
// timestamp pair around each, summed by a label (pass size and formats with the first draw's shaders,
// "resolve", "clear") and logged every 120 frames as ms/frame, most expensive first. Tells where the
// GPU's frame goes (mobile renderer plan, E6). Off: no query pool and no calls.
#include "context.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>
namespace superman_returns::graphics::vulkan {
class GpuPassProfiler {
public:
  static constexpr uint32_t kSpans=256;  // per frame slot; later spans of a frame are not timed
  static bool Requested() {const char* v=std::getenv("SR_GPU_PASSES");return v && *v && *v!='0';}
  bool Initialize(Context& c,size_t slots,uint32_t valid_bits) {
    c_=&c;slots_=slots;mask_=valid_bits>=64?~0ull:(1ull<<valid_bits)-1;period_ns_=c.properties.limits.timestampPeriod;
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=uint32_t(slots*kSpans*2);
    if(c.f.vkCreateQueryPool(c.device,&info,nullptr,&pool_)!=VK_SUCCESS) {pool_=VK_NULL_HANDLE;return false;}
    labels_.assign(slots,{});return true;
  }
  void Destroy() {if(pool_) c_->f.vkDestroyQueryPool(c_->device,pool_,nullptr);pool_=VK_NULL_HANDLE;}
  bool enabled() const {return pool_!=VK_NULL_HANDLE;}
  // Before any span of the slot's frame, on a command buffer outside a render pass.
  void BeginFrame(VkCommandBuffer command,size_t slot) {
    if(!pool_) return;
    slot_=slot;labels_[slot].clear();open_=false;
    c_->f.vkCmdResetQueryPool(command,pool_,uint32_t(slot*kSpans*2),kSpans*2);
  }
  void Open(VkCommandBuffer command,std::string label) {
    if(!pool_ || open_ || labels_[slot_].size()>=kSpans) return;
    const uint32_t q=uint32_t((slot_*kSpans+labels_[slot_].size())*2);
    c_->f.vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,pool_,q);
    labels_[slot_].push_back(std::move(label));open_=true;
  }
  void Close(VkCommandBuffer command) {
    if(!pool_ || !open_) return;
    const uint32_t q=uint32_t((slot_*kSpans+labels_[slot_].size()-1)*2+1);
    c_->f.vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,pool_,q);open_=false;
  }
  // The slot's frame has completed. Returns a log line every 120 frames, else empty.
  std::string Harvest(size_t slot) {
    if(!pool_ || labels_[slot].empty()) return {};
    const auto& labels=labels_[slot];
    std::vector<uint64_t> ticks(labels.size()*2);
    if(c_->f.vkGetQueryPoolResults(c_->device,pool_,uint32_t(slot*kSpans*2),uint32_t(ticks.size()),ticks.size()*8,ticks.data(),8,VK_QUERY_RESULT_64_BIT)!=VK_SUCCESS) return {};
    for(size_t i=0;i<labels.size();++i) {
      auto& t=totals_[labels[i]];t.ns+=uint64_t(double((ticks[i*2+1]-ticks[i*2])&mask_)*period_ns_);++t.count;
    }
    labels_[slot].clear();
    if(++frames_<120) return {};
    std::vector<std::pair<std::string,Total>> sorted(totals_.begin(),totals_.end());
    std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.second.ns>b.second.ns;});
    uint64_t all=0;for(auto& s:sorted) all+=s.second.ns;
    std::string out="GPU passes (ms/frame over 120): all="+Ms(all);
    for(size_t i=0;i<sorted.size() && i<16;++i)
      out+=" | "+sorted[i].first+" "+Ms(sorted[i].second.ns)+" x"+std::to_string(sorted[i].second.count/frames_);
    totals_.clear();frames_=0;return out;
  }
private:
  struct Total {uint64_t ns=0,count=0;};
  std::string Ms(uint64_t ns) const {const uint64_t t=ns/frames_/10000;return std::to_string(t/100)+"."+(t%100<10?"0":"")+std::to_string(t%100);}
  Context* c_=nullptr;VkQueryPool pool_=VK_NULL_HANDLE;size_t slots_=0,slot_=0;bool open_=false;
  uint64_t mask_=0;double period_ns_=1;uint64_t frames_=0;
  std::vector<std::vector<std::string>> labels_;
  std::unordered_map<std::string,Total> totals_;
};
}  // namespace superman_returns::graphics::vulkan
