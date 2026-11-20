#pragma once
#include "game_renderer.h"
#include <atomic>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <optional>
#include <thread>
namespace superman_returns::graphics::vulkan {
// Shares queue submission ordering with host presentation. Shader waits and
// CPU recording happen outside this mutex so the window can keep responding.
class GameFrame {
public:
  GameFrame(Context&,std::mutex&,ShaderLookup,TextureDecoder);
  ~GameFrame();
  bool Initialize(const std::filesystem::path&,Error&);
  bool Enqueue(guest::RenderPacket&&,std::shared_ptr<TextureResource>&,Error&);
  void Cancel();
  // Async mode: Enqueue(swap) hands the frame to a recording thread and
  // returns without an output; the thread calls `output` once the frame is
  // submitted. The packet producer accumulates the next frame meanwhile and
  // blocks at its swap only while the previous frame is still recording.
  using OutputCallback=std::function<void(std::shared_ptr<TextureResource>,const guest::SwapPacket&)>;
  void StartAsync(OutputCallback output);
  void StopAsync();
  std::function<void()> compilation_progress;
  GameRenderer& Renderer() {return renderer_;}
private:
  bool WaitShaders(const std::vector<guest::RenderPacket>&,Error&);
  bool RecordFrame(std::vector<guest::RenderPacket>&,const guest::SwapPacket&,std::shared_ptr<TextureResource>&,Error&);
  void RecorderMain();
  bool WaitSlot(size_t slot,Error&);
  void HarvestTimestamps(size_t slot);
  Context& c_;std::mutex& queue_mutex_;ShaderLookup shaders_;GameRenderer renderer_;
  std::vector<guest::RenderPacket> packets_;
  // Two frames in flight: frame N records while the GPU still runs frame N-1.
  // Each slot owns its pool, command buffers and fence; the members below
  // alias the slot of the frame being recorded.
  static constexpr size_t kSlots=2;
  std::array<VkCommandPool,kSlots> pools_{};std::array<VkCommandBuffer,kSlots> commands_{},uploads_{};std::array<VkFence,kSlots> fences_{};
  std::array<uint64_t,kSlots> slot_serial_{};std::array<bool,kSlots> slot_submitted_{};
  // SR_FRAME_TIMELINE: two timestamps per slot bracket the frame's GPU work.
  VkQueryPool timestamps_=VK_NULL_HANDLE;std::array<uint64_t,kSlots> slot_swap_{};uint64_t timestamp_mask_=0;double timestamp_period_ns_=0;uint64_t prev_end_ticks_=0;bool have_prev_end_=false;
  VkCommandPool pool_=VK_NULL_HANDLE;VkCommandBuffer command_=VK_NULL_HANDLE,upload_=VK_NULL_HANDLE;
  // Presentation snapshots, reused once neither a mailbox nor a submission holds them.
  std::vector<std::shared_ptr<TextureResource>> snapshots_;
  VkFence fence_=VK_NULL_HANDLE;uint64_t serial_=0;std::atomic<bool> failed_{false};
  std::thread recorder_;std::mutex job_mutex_;std::condition_variable job_cv_;OutputCallback output_;
  bool job_pending_=false,stop_=false;std::vector<guest::RenderPacket> job_packets_;std::optional<guest::SwapPacket> job_swap_;Error async_error_;
  std::atomic<bool> cancelled_{false};
  std::chrono::steady_clock::time_point cache_checkpoint_{};
  // SR_VULKAN_PROFILE=1: per-stage averages logged every 120 frames.
  bool profiling_=false;std::chrono::steady_clock::time_point last_swap_{};
  struct {uint64_t frames=0,packets=0,interval_us=0,shaders_us=0,fence_us=0,record_us=0,queue_us=0,tail_us=0;} timing_;
  // Destroys a recorded frame's packets on its own thread. Freeing ~2,800 packets (their vectors, the
  // arena blocks, the frozen batches) made scudo hand pages back to the system on the recorder thread:
  // madvise + munmap were 13-15% of it (Fold7 city, simpleperf, 2026-11-20), and mallopt(M_DECAY_TIME)
  // does not change Android's fixed release interval. SR_PACKET_REAPER=0 frees on the recorder (A/B).
  class PacketReaper {
  public:
    PacketReaper();
    ~PacketReaper();  // frees what is still queued
    // Takes the packets; `packets` comes back empty, with the capacity of an earlier frame's vector.
    void Release(std::vector<guest::RenderPacket>& packets);
  private:
    void Main();
    bool enabled_=true;std::mutex mutex_;std::condition_variable cv_;bool stop_=false;
    std::vector<std::vector<guest::RenderPacket>> dead_,spare_;
    std::thread thread_;
  };
  GpuPassProfiler gpu_passes_;  // SR_GPU_PASSES=1
  PacketReaper reaper_;  // last: destroyed first, while everything the packets reference is alive
};
}
