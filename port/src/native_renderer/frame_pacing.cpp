// Frame pacing of the native renderer's presents.
//
// The game's frame limiter starts frames on an even grid, but the host
// presented each frame as soon as the render worker had recorded it, and
// without vsync a frame reaches the display when its GPU work ends: the display
// intervals took on the variation of the worker's end time and of the GPU
// time, 10 to 23 ms at the 60 FPS limit (stdev 2.7 ms), while the game's
// animation advanced by an even step. Measured alternatives: pacing the present
// call alone left the GPU ends uneven (stdev 2.2 ms); pacing before the
// worker submits evened them but its sleep held back the game thread (44
// frames over 25 ms instead of 0).
//
// So with sr_frame_pacing (and the renderer's own queue) the worker submits the
// frame at once and hands {slot, fence} to a present thread. That thread waits
// for the frame's GPU work, then until one period of the limit after the
// previous present, and presents (the presenter copies the frame and calls
// Present on this thread, paint mode "guest output thread immediately"). A
// late frame is presented at once and the cadence restarts from it; the
// target creeps 1/256 period early per frame, so latency added by a late frame
// wears off within seconds. A frame slot is reused only after its present was
// issued (WaitPacedPresent).
//
// SR_PRESENT_TIMES=<file.csv>: per frame the time of the present and of the
// frame's last GPU timestamp (converted to the CPU clock with the queue's
// clock calibration), seconds since the first frame.

#include "native_renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include <windows.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_presenter.h>

REXCVAR_DECLARE(int32_t, sr_native_fps_limit);
REXCVAR_DECLARE(bool, sr_native_ab_mode);
REXCVAR_DECLARE(bool, sr_native_debug_no_present);
REXCVAR_DEFINE_BOOL(sr_frame_pacing, true, "Superman Returns Native",
                    "Present frames an even period of the frame rate limit apart, once their "
                    "GPU work is done (a present thread)");

namespace superman_returns::native {
namespace {

int64_t QpcNow() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

int64_t QpcFrequency() {
  static const int64_t f = [] {
    LARGE_INTEGER t;
    QueryPerformanceFrequency(&t);
    return t.QuadPart;
  }();
  return f;
}

FILE* PresentTimesFile() {
  static FILE* file = [] {
    const char* path = std::getenv("SR_PRESENT_TIMES");
    FILE* f = path && *path ? std::fopen(path, "w") : nullptr;
    if (f) std::fputs("frame,present_s,gpu_end_s,present_call_ms\n", f);
    return f;
  }();
  return file;
}

double Seconds(int64_t qpc) {
  static const int64_t start = qpc;
  return double(qpc - start) / double(QpcFrequency());
}

// Waits until `target` (QPC): a high-resolution timer to 200 us before it,
// then yields.
void WaitUntil(HANDLE timer, int64_t target) {
  const int64_t early = QpcFrequency() / 5000;
  int64_t now = QpcNow();
  if (timer && target - now > early) {
    LARGE_INTEGER due;
    due.QuadPart = -((target - now - early) * 10000000 / QpcFrequency());
    if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
      WaitForSingleObject(timer, 100);
    }
  }
  while (QpcNow() < target) SwitchToThread();
}

}  // namespace

bool Renderer::PacedPresentActive() const {
  return REXCVAR_GET(sr_frame_pacing) && REXCVAR_GET(sr_native_fps_limit) > 0 && own_queue_ &&
         !REXCVAR_GET(sr_native_ab_mode) && !REXCVAR_GET(sr_native_debug_no_present);
}

void Renderer::QueuePacedPresent(uint64_t fence) {
  std::lock_guard<std::mutex> lock(present_mutex_);
  if (!present_thread_.joinable() && !present_stop_) {
    present_thread_ = std::thread([this] { PresentThreadMain(); });
  }
  PresentJob job;
  job.slot = frame_index_;
  job.fence = fence;
  job.serial = ++present_jobs_queued_;
  job.output = output_rt();
  present_slot_job_[frame_index_] = job.serial;
  present_jobs_.push_back(job);
  present_cv_.notify_all();
}

void Renderer::WaitPacedPresent(uint32_t slot) {
  std::unique_lock<std::mutex> lock(present_mutex_);
  const uint64_t serial = present_slot_job_[slot];
  present_cv_.wait(lock, [&] { return present_stop_ || present_jobs_done_ >= serial; });
}

void Renderer::StopPresentThread() {
  {
    std::lock_guard<std::mutex> lock(present_mutex_);
    present_stop_ = true;
  }
  present_cv_.notify_all();
  if (present_thread_.joinable()) present_thread_.join();
}

void Renderer::PresentThreadMain() {
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  int64_t prev = 0;
  for (;;) {
    PresentJob job;
    {
      std::unique_lock<std::mutex> lock(present_mutex_);
      present_cv_.wait(lock, [&] { return present_stop_ || !present_jobs_.empty(); });
      // Queued presents are dropped on shutdown, as the worker drops frames.
      if (present_stop_) break;
      job = present_jobs_.front();
      present_jobs_.pop_front();
    }
    // The frame's GPU work.
    if (fence_->GetCompletedValue() < job.fence) {
      fence_->SetEventOnCompletion(job.fence, fence_event);
      WaitForSingleObject(fence_event, 2000);
    }
    // The cadence: a period after the previous present.
    const int32_t fps = REXCVAR_GET(sr_native_fps_limit);
    if (fps > 0 && prev) {
      const int64_t period = QpcFrequency() / fps;
      const int64_t target = prev + period - period / 256;
      const int64_t now = QpcNow();
      // More than a period early: the cadence was lost (a pause, a load).
      if (now < target && target - now <= period) {
        WaitUntil(timer, target);
        prev = target;
      } else {
        prev = now;
      }
    } else {
      prev = QpcNow();
    }
    present_qpc_[job.slot] = prev;
    const int64_t call_start = QpcNow();
    presenter_->RefreshGuestOutput(
        output_width_, output_height_, 1280, 720,
        [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
          auto& d3d12_context =
              static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context);
          ID3D12Resource* output = d3d12_context.resource_uav_capable();
          ID3D12CommandQueue* direct = provider_->GetDirectQueue();
          direct->Wait(fence_.Get(), job.fence);
          // This slot's previous copy is done before its allocator is reset.
          uint64_t previous;
          {
            std::lock_guard<std::mutex> lock(present_mutex_);
            previous = present_fence_values_[job.slot];
          }
          if (previous && present_fence_->GetCompletedValue() < previous) {
            present_fence_->SetEventOnCompletion(previous, nullptr);
          }
          present_allocators_[job.slot]->Reset();
          present_list_->Reset(present_allocators_[job.slot].Get(), nullptr);
          D3D12_RESOURCE_BARRIER barrier{};
          barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          barrier.Transition.pResource = output;
          barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          barrier.Transition.StateBefore = rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState;
          barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
          if (barrier.Transition.StateBefore != barrier.Transition.StateAfter) {
            present_list_->ResourceBarrier(1, &barrier);
          }
          present_list_->CopyResource(output, job.output);
          std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
          if (barrier.Transition.StateBefore != barrier.Transition.StateAfter) {
            present_list_->ResourceBarrier(1, &barrier);
          }
          present_list_->Close();
          ID3D12CommandList* present_lists[] = {present_list_.Get()};
          direct->ExecuteCommandLists(1, present_lists);
          std::lock_guard<std::mutex> lock(present_mutex_);
          direct->Signal(present_fence_.Get(), present_fence_next_);
          present_fence_values_[job.slot] = present_fence_next_++;
          context.SetIs8bpc(true);
          return true;
        });
    // How long the presenter's refresh and Present took (SR_PRESENT_TIMES).
    present_call_ms_[job.slot] = float(double(QpcNow() - call_start) * 1000.0 / double(QpcFrequency()));
    {
      std::lock_guard<std::mutex> lock(present_mutex_);
      present_jobs_done_ = job.serial;
    }
    present_cv_.notify_all();
  }
  if (timer) CloseHandle(timer);
  if (fence_event) CloseHandle(fence_event);
}

void Renderer::RecordGpuEnd(uint32_t slot, uint64_t gpu_end_ticks) {
  FILE* file = PresentTimesFile();
  if (!file || !ts_frequency_) return;
  uint64_t gpu = 0, cpu = 0;
  if (FAILED(queue_->GetClockCalibration(&gpu, &cpu))) return;
  const int64_t end_qpc =
      int64_t(cpu) + int64_t((double(int64_t(gpu_end_ticks - gpu)) * double(QpcFrequency())) /
                             double(ts_frequency_));
  std::fprintf(file, "%llu,%.5f,%.5f,%.2f\n", static_cast<unsigned long long>(ts_frame_[slot]),
               Seconds(present_qpc_[slot]), Seconds(end_qpc), present_call_ms_[slot]);
  static uint32_t lines = 0;
  if (++lines % 120 == 0) std::fflush(file);
}

}  // namespace superman_returns::native
