#include "buffer_stock.h"

#include <algorithm>

namespace superman_returns::native {

namespace {

// Bytes the stock may hold per kind, so that it never grows past what streaming
// needs. 1 GB: in fast flight with the console's distance limits lifted, 256 MB
// ran dry and the worker created buffers in the driver (frames of 100-300 ms).
constexpr uint64_t kStockBytesPerKind = 1024ull << 20;

}  // namespace

BufferStock::~BufferStock() { Stop(); }

uint32_t BufferStock::ClassOf(uint64_t bytes) {
  uint32_t log2 = kMinClassLog2;
  while (log2 < 63 && (1ull << log2) < bytes) ++log2;
  return log2 - kMinClassLog2;  // >= kClasses: too large for the stock
}

BufferStock::Buffer BufferStock::Create(Kind kind, uint64_t width) const {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = kind == Kind::kUpload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = width;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  Buffer b;
  switch (kind) {
    case Kind::kGuestData: b.state = D3D12_RESOURCE_STATE_COPY_DEST; break;
    case Kind::kUpload: b.state = D3D12_RESOURCE_STATE_GENERIC_READ; break;
    default:
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      b.state = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
      break;
  }
  if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, b.state, nullptr,
                                              IID_PPV_ARGS(&b.resource)))) {
    b.resource.Reset();
  }
  return b;
}

void BufferStock::Start(ID3D12Device* device) {
  device_ = device;
  filler_ = std::thread([this] { FillerMain(); });
}

void BufferStock::Stop() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  if (filler_.joinable()) filler_.join();
}

BufferStock::Buffer BufferStock::Take(Kind kind, uint64_t bytes) {
  const uint32_t cls = ClassOf(bytes);
  if (cls >= kClasses || !device_) return Create(kind, (bytes + 255) & ~255ull);
  {
    std::lock_guard lock(mutex_);
    Slot& slot = slots_[size_t(kind)][cls];
    if (!slot.target) slot.target = 32;
    // This size, else one or two sizes up (a larger buffer serves as well; a
    // creation on the worker costs a driver allocation and a wait for the GPU).
    for (uint32_t c = cls; c < kClasses && c <= cls + 2; ++c) {
      Slot& s = slots_[size_t(kind)][c];
      if (s.ready.empty()) continue;
      Buffer b = std::move(s.ready.back());
      s.ready.pop_back();
      ++hits_;
      if (c != cls) slot.target = std::min<uint32_t>(slot.target * 2, 512);
      pending_refill_ = true;
      wake_.notify_one();
      return b;
    }
    ++misses_;
    slot.target = std::min<uint32_t>(slot.target * 2, 512);
    pending_refill_ = true;
  }
  wake_.notify_one();
  return Create(kind, 1ull << (cls + kMinClassLog2));
}

void BufferStock::Give(Kind kind, Buffer buffer, uint64_t fence) {
  if (!buffer.resource) return;
  const uint32_t cls = ClassOf(buffer.resource->GetDesc().Width);
  std::lock_guard lock(mutex_);
  // Only exact class widths go back to the stock; any other is released (later,
  // by the filler thread).
  const bool fits = cls < kClasses && buffer.resource->GetDesc().Width == (1ull << (cls + kMinClassLog2));
  pending_.push_back({kind, fits ? cls : kClasses, std::move(buffer), fence});
}

void BufferStock::Collect(uint64_t completed_fence) {
  std::lock_guard lock(mutex_);
  bool trashed = false;
  while (!pending_.empty() && pending_.front().fence <= completed_fence) {
    Pending& p = pending_.front();
    if (p.cls < kClasses && StockBytes(p.kind) + (1ull << (p.cls + kMinClassLog2)) <= kStockBytesPerKind) {
      slots_[size_t(p.kind)][p.cls].ready.push_back(std::move(p.buffer));
    } else {
      trash_.push_back(std::move(p.buffer));
      trashed = true;
    }
    pending_.pop_front();
  }
  if (trashed) wake_.notify_one();
}

void BufferStock::TakeStats(uint64_t& hits, uint64_t& misses) {
  std::lock_guard lock(mutex_);
  hits = hits_;
  misses = misses_;
  hits_ = misses_ = 0;
}

uint64_t BufferStock::StockBytes(Kind kind) const {
  uint64_t bytes = 0;
  for (uint32_t c = 0; c < kClasses; ++c)
    bytes += uint64_t(slots_[size_t(kind)][c].ready.size()) << (c + kMinClassLog2);
  return bytes;
}

void BufferStock::FillerMain() {
  SetThreadDescription(GetCurrentThread(), L"Native buffer stock");
  std::unique_lock lock(mutex_);
  while (!stop_) {
    if (!trash_.empty()) {
      std::vector<Buffer> trash;
      trash.swap(trash_);
      lock.unlock();
      trash.clear();  // the driver's release, off the render worker
      lock.lock();
      continue;
    }
    // The emptiest class (relative to its target) first, one buffer at a time,
    // so that a burst of one size does not starve the others.
    int best_kind = -1, best_cls = -1;
    double best_fill = 1.0;
    for (size_t k = 0; k < size_t(Kind::kCount); ++k) {
      const uint64_t bytes = StockBytes(Kind(k));
      for (uint32_t c = 0; c < kClasses; ++c) {
        Slot& s = slots_[k][c];
        if (!s.target || bytes + (1ull << (c + kMinClassLog2)) > kStockBytesPerKind) continue;
        const double fill = double(s.ready.size()) / double(s.target);
        if (fill < best_fill) {
          best_fill = fill;
          best_kind = int(k);
          best_cls = int(c);
        }
      }
    }
    if (best_kind < 0) {
      wake_.wait(lock, [&] { return stop_ || !trash_.empty() || pending_refill_; });
      pending_refill_ = false;
      continue;
    }
    lock.unlock();
    Buffer b = Create(Kind(best_kind), 1ull << (best_cls + kMinClassLog2));
    lock.lock();
    if (b.resource) slots_[size_t(best_kind)][size_t(best_cls)].ready.push_back(std::move(b));
  }
}

}  // namespace superman_returns::native
