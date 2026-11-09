// Ready-made GPU buffers for the render worker.
//
// The city streams in new geometry while the player moves; every new guest
// vertex/index buffer (and every ray-tracing mesh) needed a D3D12 buffer, and
// creating one on the render worker (CreateCommittedResource: a driver
// allocation that also waits on a synchronization object) took about 15% of
// its time in bursts: the 50-350 ms hitches of the city flight. The worker now
// takes buffers from a stock in power-of-two size classes that a background
// thread keeps filled, and gives replaced ones back once their frame's fence
// has passed. A buffer is created on the worker only when its class ran dry.
#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

namespace superman_returns::native {

class BufferStock {
 public:
  // The kinds of buffer the renderer creates at run time.
  enum class Kind : uint32_t {
    kGuestData,    // default heap, guest vertex/index data (copy destination, then read)
    kUpload,       // upload heap, generic read (ray-tracing mesh indices)
    kAccelStruct,  // default heap, UAV, ray-tracing acceleration structure
    kCount
  };
  struct Buffer {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  };

  ~BufferStock();
  void Start(ID3D12Device* device);
  void Stop();
  // A buffer of at least `bytes` (its width is the size class).
  Buffer Take(Kind kind, uint64_t bytes);
  // Back to the stock once the GPU has passed `fence` (beyond the stock's size
  // limit it is released by the background thread, never by the worker).
  void Give(Kind kind, Buffer buffer, uint64_t fence);
  // Moves given-back buffers whose fence has completed into the stock.
  void Collect(uint64_t completed_fence);
  // Takes served from the stock / created on the worker since the last call.
  void TakeStats(uint64_t& hits, uint64_t& misses);

  static constexpr uint32_t kMinClassLog2 = 8;   // 256 bytes
  static constexpr uint32_t kMaxClassLog2 = 24;  // 16 MB; larger: created directly
  static constexpr uint32_t kClasses = kMaxClassLog2 - kMinClassLog2 + 1;
  static uint32_t ClassOf(uint64_t bytes);

 private:
  struct Slot {
    std::vector<Buffer> ready;
    uint32_t target = 0;  // 8 once used, doubled (up to 512) whenever a take finds it empty
  };
  struct Pending {
    Kind kind;
    uint32_t cls;
    Buffer buffer;
    uint64_t fence;
  };
  Buffer Create(Kind kind, uint64_t width) const;
  uint64_t StockBytes(Kind kind) const;  // under mutex_
  void FillerMain();

  ID3D12Device* device_ = nullptr;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false;
  bool pending_refill_ = false;  // a take since the filler last slept
  std::array<std::array<Slot, kClasses>, size_t(Kind::kCount)> slots_;
  std::deque<Pending> pending_;
  std::vector<Buffer> trash_;  // released by the filler thread
  uint64_t hits_ = 0, misses_ = 0;
  std::thread filler_;
};

}  // namespace superman_returns::native
