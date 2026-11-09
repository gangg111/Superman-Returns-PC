// Game textures placed in large pre-made heaps instead of one committed
// allocation each: in fast flight (the console's distance limits lifted) the
// render worker created dozens of textures a second and every
// CreateCommittedResource cost a driver allocation and a wait for the GPU
// (win32u NtGdiDdDDICreateAllocation / WaitForSynchronizationObjectFromCpu,
// frames of 50-300 ms in the hitch sampler). A background thread keeps one
// spare heap ready; the worker only bumps an offset. Space is not reused when a
// texture is released (game textures live for the session); past the heap cap
// the caller falls back to a committed resource.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace superman_returns::native {

class TextureHeap {
 public:
  static constexpr uint64_t kHeapBytes = 256ull << 20;
  static constexpr size_t kMaxHeaps = 16;  // 4 GB

  ~TextureHeap() { Stop(); }

  void Start(ID3D12Device* device) {
    device_ = device;
    filler_ = std::thread([this] { FillerMain(); });
  }

  void Stop() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    wake_.notify_all();
    if (filler_.joinable()) filler_.join();
  }

  // A texture of `desc` in the current heap, or false (the caller creates it
  // committed). Called on the render worker only.
  bool Place(const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state,
             Microsoft::WRL::ComPtr<ID3D12Resource>& out) {
    if (!device_) return false;
    const D3D12_RESOURCE_ALLOCATION_INFO info = device_->GetResourceAllocationInfo(0, 1, &desc);
    if (info.SizeInBytes == UINT64_MAX || info.SizeInBytes > kHeapBytes / 4) return false;
    const uint64_t align = info.Alignment ? info.Alignment : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    uint64_t offset = (offset_ + align - 1) / align * align;
    if (!current_ || offset + info.SizeInBytes > kHeapBytes) {
      std::lock_guard lock(mutex_);
      if (!spare_) {
        ++misses_;
        return false;
      }
      current_ = std::move(spare_);
      heaps_.push_back(current_);
      offset = 0;
      wake_.notify_one();
    }
    if (FAILED(device_->CreatePlacedResource(current_.Get(), offset, &desc, state, nullptr, IID_PPV_ARGS(&out))))
      return false;
    offset_ = offset + info.SizeInBytes;
    ++placed_;
    return true;
  }

  // Placed / not placed (no heap ready or past the cap) since the last call.
  void TakeStats(uint64_t& placed, uint64_t& misses) {
    std::lock_guard lock(mutex_);
    placed = placed_;
    misses = misses_;
    placed_ = misses_ = 0;
  }

 private:
  void FillerMain() {
    SetThreadDescription(GetCurrentThread(), L"Native texture heaps");
    std::unique_lock lock(mutex_);
    while (!stop_) {
      if (!spare_ && heaps_.size() + 1 < kMaxHeaps) {
        lock.unlock();
        D3D12_HEAP_DESC hd{};
        hd.SizeInBytes = kHeapBytes;
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.Alignment = D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
        hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
        Microsoft::WRL::ComPtr<ID3D12Heap> heap;
        const bool ok = SUCCEEDED(device_->CreateHeap(&hd, IID_PPV_ARGS(&heap)));
        lock.lock();
        if (!ok) break;
        spare_ = std::move(heap);
        continue;
      }
      wake_.wait(lock, [&] { return stop_ || !spare_; });
    }
  }

  ID3D12Device* device_ = nullptr;
  std::thread filler_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false;
  Microsoft::WRL::ComPtr<ID3D12Heap> spare_, current_;
  std::vector<Microsoft::WRL::ComPtr<ID3D12Heap>> heaps_;
  uint64_t offset_ = 0, placed_ = 0, misses_ = 0;
};

}  // namespace superman_returns::native
