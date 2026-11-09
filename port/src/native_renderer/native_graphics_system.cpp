// Ported from crazyriddler/rexglue-native-kit @136bc6c4,
// reference/conan/port/src/native/native_graphics_system.cpp (Conan native renderer).
// The kit ships no license file at that revision; parts derived from Xenia /
// ReXGlue keep their BSD license. Changes for Superman Returns are listed in
// docs/native-port-plan.md section 2.
//
// Native graphics system - see native_graphics_system.h.
//
// The packet semantics below follow the SDK's (Xenia-derived) CommandProcessor
// for everything the guest can observe; everything it cannot (draws, shader
// loads, render state) is skipped. (The kit forced REXGLUE_ENABLE_PERF_COUNTERS
// here; the stock SDK compiles the counters out of Release builds, so the
// frame-time counters below are only live in non-Release builds.)

#include "native_graphics_system.h"
#include "../graphics/guest/state_source.h"

#include <algorithm>
#include <charconv>
#include <string>

#include <fmt/format.h>
#include <chrono>
#include <cstring>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/presenter.h>
#include <rex/ui/graphics_provider.h>
#if SR_VULKAN_GAME
#include "../graphics/vulkan/platform/native_provider.h"
#endif
#include <rex/ui/windowed_app_context.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "game_profile.h"
#include "hang_watchdog.h"
#include "sdk_compat.h"
#if SR_NATIVE_RENDERER_BUILD
#include "native_renderer.h"
#endif
#if SR_HAS_LSFG
#include "presenter_factory.h"
#endif

REXCVAR_DEFINE_INT32(sr_native_fps_limit, 60, "Superman Returns Native",
                     "Native graphics system frame rate limit (30 = original, up to 120; 0 = "
                     "sr_native_unlocked_vblank_hz)")
    .range(0, 1000);
REXCVAR_DEFINE_BOOL(sr_native_gpu_trace, false, "Superman Returns Native", "Debug: log native GPU packets/registers");
REXCVAR_DEFINE_STRING(sr_native_gpu_luid, "", "Superman Returns Native",
                     "Launcher GPU identity (16 hexadecimal digits); empty uses d3d12_adapter");
// The kit's fork declares unlocked_vblank_rate in the runtime; the stock SDK
// does not, so the unlocked rate is a project cvar.
REXCVAR_DEFINE_INT32(sr_native_unlocked_vblank_hz, 240, "Superman Returns Native",
                     "Guest vblank rate with sr_native_fps_limit=0 (benchmarks)")
    .range(1, 2000);

namespace superman_returns::native {

using rex::X_STATUS;

namespace xenos = rex::graphics::xenos;
namespace gpu = rex::graphics;

namespace {
std::atomic<D3D12NativeGraphicsSystem*> g_active_system{nullptr};

// Windows graphics preferences can reorder EnumAdapters1 per executable.
// Resolve the launcher's LUID inside this process before creating the provider.
bool ResolveLauncherGpu() {
  const auto text = rex::cvar::GetFlagByName("sr_native_gpu_luid");
  if (text.empty()) return true;
  // "--sr_native_gpu_luid=" with no value makes the argument parser take the next
  // argument as the value. That is no GPU identity: use the automatic adapter.
  if (text.starts_with("--")) {
    REXLOG_WARN("native graphics: sr_native_gpu_luid has no value (read '{}'); using the automatic GPU", text);
    return true;
  }
  uint64_t luid = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), luid, 16);
  if (text.size() != 16 || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    REXLOG_ERROR("native graphics: invalid launcher GPU identity {}", text);
    return false;
  }
  Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
  for (UINT index = 0; ; ++index) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(index, &adapter) != S_OK) break;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) continue;
    const uint64_t candidate = (uint64_t(uint32_t(desc.AdapterLuid.HighPart)) << 32) |
                               desc.AdapterLuid.LowPart;
    if (candidate == luid) {
      REXLOG_INFO("native graphics: launcher GPU {} resolved to adapter {}", text, index);
      return rex::cvar::SetFlagByName("d3d12_adapter", std::to_string(index));
    }
  }
  REXLOG_ERROR("native graphics: selected launcher GPU {} is unavailable", text);
  return false;
}
}  // namespace

INativeGraphicsSystem* ActiveNativeGraphicsSystem() { return g_active_system.load(); }

uint64_t GpuProgressGeneration() {
  D3D12NativeGraphicsSystem* system = g_active_system.load();
  return system ? system->progress_generation() : 0;
}

void WaitForGpuProgress(uint64_t since, uint32_t timeout_us) {
  if (D3D12NativeGraphicsSystem* system = g_active_system.load()) {
    system->WaitProgress(since, timeout_us);
  }
}

void WaitForGpuCondition(const std::function<bool()>& done, uint32_t timeout_us) {
  if (D3D12NativeGraphicsSystem* system = g_active_system.load()) {
    system->WaitCondition(done, timeout_us);
  }
}

void D3D12NativeGraphicsSystem::WaitCondition(const std::function<bool()>& done,
                                         uint32_t timeout_us) {
  std::unique_lock<std::mutex> lock(progress_mutex_);
  progress_cv_.wait_for(lock, std::chrono::microseconds(timeout_us),
                        [&] { return !running_ || done(); });
}

void D3D12NativeGraphicsSystem::SignalGpuProgress() {
  {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_generation_.fetch_add(1);
  }
  progress_cv_.notify_all();
}

void D3D12NativeGraphicsSystem::WaitProgress(uint64_t since, uint32_t timeout_us) {
  std::unique_lock<std::mutex> lock(progress_mutex_);
  progress_cv_.wait_for(lock, std::chrono::microseconds(timeout_us),
                        [&] { return progress_generation_.load() != since || !running_; });
}

// Reads PM4 dwords from a ring (wrapping) or a linear buffer.
struct D3D12NativeGraphicsSystem::Reader {
  const uint8_t* base = nullptr;
  uint32_t capacity = 0;  // dwords
  uint32_t offset = 0;    // dwords
  uint32_t remaining = 0;
  uint32_t Read() {
    uint32_t v;
    std::memcpy(&v, base + 4 * size_t(offset), 4);
    offset = offset + 1 == capacity ? 0 : offset + 1;
    --remaining;
    return __builtin_bswap32(v);
  }
  void Skip(uint32_t n) {
    offset = uint32_t((uint64_t(offset) + n) % capacity);
    remaining -= n;
  }
};

D3D12NativeGraphicsSystem::D3D12NativeGraphicsSystem(graphics::NativeApi api)
    : native_api_(api),registers_(std::make_unique<std::atomic<uint32_t>[]>(kRegisterCount)) {
  for (uint32_t i = 0; i < kRegisterCount; ++i) registers_[i].store(0);
  write_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  cp_wake_ = rex::thread::Event::CreateAutoResetEvent(false);
}

D3D12NativeGraphicsSystem::~D3D12NativeGraphicsSystem() = default;

rex::ui::GraphicsProvider* D3D12NativeGraphicsSystem::provider() const { return provider_.get(); }

rex::X_STATUS D3D12NativeGraphicsSystem::SetupPresentation(rex::ui::WindowedAppContext* app_context) {
  if (presenter_) return X_STATUS_SUCCESS;
  const bool vulkan=native_api_==graphics::NativeApi::kVulkan;
  if(vulkan) {
#if SR_VULKAN_GAME
  // Release layout (tools/package_release.ps1): shader_tools/ next to the exe,
  // with its own Python; caches go to shader_cache/vulkan. Development builds
  // fall back to the source tree and python.exe on PATH.
  wchar_t module[MAX_PATH]{};GetModuleFileNameW(nullptr,module,MAX_PATH);
  const auto exe_dir=std::filesystem::path(module).parent_path(),tools=exe_dir/"shader_tools";
  const bool packaged=std::filesystem::is_regular_file(tools/"vulkan/runtime_vulkan_shader.py");
  std::filesystem::path python=tools/"python/python.exe";
  if(!packaged || !std::filesystem::is_regular_file(python)) {
    wchar_t found[MAX_PATH]{};if(!SearchPathW(nullptr,L"python.exe",nullptr,MAX_PATH,found,nullptr)) {REXLOG_ERROR("native Vulkan: python.exe not found (shader_tools/python or PATH)");return X_STATUS_UNSUCCESSFUL;}
    python=found;
  }
  graphics::vulkan::NativeProviderConfig config;
  if(packaged) {
    const auto cache=exe_dir/"shader_cache/vulkan";std::error_code cache_error;std::filesystem::create_directories(cache,cache_error);
    config.shaders={python,tools/"vulkan/runtime_vulkan_shader.py",cache,tools/"vulkan/sr_xenosrecomp.exe",tools/"vulkan/shader_common.h",tools/"dxc.exe"};
    config.driver_cache=cache/"driver.cache";
  } else {
    const std::filesystem::path root=SR_VULKAN_PROJECT_ROOT;
    config.shaders={python,root/"tools/shaders/runtime_vulkan_shader.py",root/"build/vulkan-m3-runtime",root/"build/vulkan-m2/emitter-build/XenosRecompCorpus.exe",root/"build/vulkan-m2/emitter-tree/src/XenosRecomp/shader_common.h",root/".tools/dxc/bin/x64/dxc.exe"};
    config.driver_cache=root/"build/vulkan-m3-runtime/driver.cache";
  }
  REXLOG_INFO("native Vulkan: shader tools from {} ({})",packaged?tools.string():std::string("source tree"),python.string());
  config.shaders.library=exe_dir/"superman_returns_vulkan.srvk";
  config.gpu_uuid=rex::cvar::Query<std::string>("sr_native_vulkan_gpu_uuid");
  config.frame_generation.enabled=rex::cvar::Query<bool>("sr_lsfg");
  config.anisotropy=rex::cvar::Query<int32_t>("sr_native_anisotropic_filtering");
  config.frame_generation.dll=std::filesystem::u8path(rex::cvar::Query<std::string>("sr_lsfg_dll"));
  config.frame_generation.performance=rex::cvar::Query<std::string>("sr_lsfg_mode")=="performance";
  provider_=graphics::vulkan::CreateNativeVulkanProvider(std::move(config));
#else
    REXLOG_ERROR("Native Vulkan unavailable: build with SR_VULKAN_GAME=ON");return X_STATUS_UNSUCCESSFUL;
#endif
  } else {
  if (!ResolveLauncherGpu()) return X_STATUS_UNSUCCESSFUL;
  provider_ = rex::ui::d3d12::D3D12Provider::Create();
  }
  if (!provider_) {
    REXLOG_ERROR("native graphics: unable to create the {} provider",vulkan?"Vulkan":"D3D12");
    return X_STATUS_UNSUCCESSFUL;
  }
  app_context_ = app_context;
  auto loss = [](bool, bool) {
    rex::FatalError("Graphics device lost (probably due to an internal error)");
  };
  auto create_presenter = [this, loss, vulkan]() {
#if SR_HAS_LSFG
    if(!vulkan) {
      presenter_ = sr::lsfg::CreateProjectPresenter(*static_cast<rex::ui::d3d12::D3D12Provider*>(provider_.get()), loss);return;
    }
#endif
    presenter_ = provider_->CreatePresenter(loss);
  };
  if (app_context_) {
    app_context_->CallInUIThreadSynchronous(create_presenter);
  } else {
    create_presenter();
  }
  if (!presenter_) {
    REXLOG_ERROR("native graphics: unable to create the presenter");
    return X_STATUS_UNSUCCESSFUL;
  }
  REXLOG_INFO("native graphics: {} presentation ready (no GPU plugin)",vulkan?"Vulkan":"D3D12");
  return X_STATUS_SUCCESS;
}

rex::X_STATUS D3D12NativeGraphicsSystem::SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                                             rex::system::KernelState* kernel_state) {
  memory_ = function_dispatcher->memory();
  function_dispatcher_ = function_dispatcher;
  kernel_state_ = kernel_state;
  if (!provider_) {
    if(native_api_==graphics::NativeApi::kVulkan) {
    REXLOG_ERROR("native Vulkan: presentation must be initialized before guest GPU setup");
    return X_STATUS_UNSUCCESSFUL;
    }
    if (!ResolveLauncherGpu()) return X_STATUS_UNSUCCESSFUL;
    provider_ = rex::ui::d3d12::D3D12Provider::Create();
    if (!provider_) return X_STATUS_UNSUCCESSFUL;
  }
  // GPU registers: 0x7FC80000-0x7FC8FFFF.
  memory_->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, this, &ReadRegisterThunk,
                                 &WriteRegisterThunk);
  running_ = true;
  g_active_system.store(this);
  command_thread_ = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(kernel_state_, 256 * 1024, 0, [this]() {
        CommandThreadMain();
        return 0;
      }));
  command_thread_->set_name("Native GPU Commands");
  command_thread_->Create();
  vsync_thread_ = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        VsyncThreadMain();
        return 0;
      }));
  vsync_thread_->set_name("GPU VSync");
  vsync_thread_->Create();
  REXLOG_INFO("native graphics: guest GPU (sync-only PM4 consumer) started");
  HangWatchdogBeat();  // arm the stall watchdog before the first swap
  return X_STATUS_SUCCESS;
}

void D3D12NativeGraphicsSystem::Shutdown() {
  g_active_system.store(nullptr);
  SignalGpuProgress();
  running_ = false;
  if (write_event_) write_event_->Set();
  if (command_thread_) {
    command_thread_->Wait(0, 0, 0, nullptr);
    command_thread_.reset();
  }
  if (vsync_thread_) {
    vsync_thread_->Wait(0, 0, 0, nullptr);
    vsync_thread_.reset();
  }
#if SR_NATIVE_RENDERER_BUILD
  Renderer::Get().ShutdownWorker();
#endif
  if (presenter_) {
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
    }
    presenter_.reset();
  }
  provider_.reset();
}

bool D3D12NativeGraphicsSystem::GetGammaRamp256(uint32_t* out_entries) const {
  if (!out_entries) return false;
  std::memcpy(out_entries, gamma_ramp_, sizeof(gamma_ramp_));
  return true;
}

void D3D12NativeGraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
  REXLOG_INFO("native graphics: interrupt callback {:08X} ({:08X})", callback, user_data);
}

void D3D12NativeGraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  ring_base_ = ptr;
  ring_dwords_ = (uint32_t(1) << (size_log2 + 3)) / 4;
  read_index_ = 0;
  REXLOG_INFO("native graphics: ring buffer {:08X} ({} dwords)", ptr, ring_dwords_);
}

void D3D12NativeGraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  (void)block_size_log2;
  read_ptr_writeback_ = ptr;
  REXLOG_INFO("native graphics: read pointer write-back {:08X}", ptr);
}

// --- MMIO -------------------------------------------------------------------

uint32_t D3D12NativeGraphicsSystem::ReadRegisterThunk(void*, void* context, uint32_t addr) {
  return static_cast<D3D12NativeGraphicsSystem*>(context)->ReadMmioRegister(addr);
}

void D3D12NativeGraphicsSystem::WriteRegisterThunk(void*, void* context, uint32_t addr,
                                              uint32_t value) {
  static_cast<D3D12NativeGraphicsSystem*>(context)->WriteMmioRegister(addr, value);
}

uint32_t D3D12NativeGraphicsSystem::ReadMmioRegister(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;
  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x194C: {  // R500_D1MODE_V_COUNTER
      rex::system::X_VIDEO_MODE mode;
      rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
      return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
    }
    case 0x1951:  // interrupt status
      return 1;   // vblank
    case 0x1961: {  // AVIVO_D1MODE_VIEWPORT_SIZE
      rex::system::X_VIDEO_MODE mode;
      rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
      return (std::min(uint32_t(mode.display_width), uint32_t(0x0FFF)) << 16) |
             std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
    }
    default:
      return ReadRegister(r);
  }
}

void D3D12NativeGraphicsSystem::WriteMmioRegister(uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;
  if (REXCVAR_GET(sr_native_gpu_trace) && r != 0x01C5) {
    static int logged = 0;
    if (logged++ < 40) REXLOG_INFO("native gpu trace: mmio write {:04X} = {:08X}", r, value);
  }
  if (r < kRegisterCount) registers_[r].store(value);
  cp_wake_->Set();  // a WAIT_REG_MEM on this register may now pass
  if (r == 0x01C5) {  // CP_RB_WPTR: the guest kicked the ring
    static int logged = 0;
    if (logged < 3) {
      ++logged;
      REXLOG_INFO("native graphics: ring kick wptr {}", value);
    }
    write_index_.store(value);
    write_event_->Set();
  }
}

void D3D12NativeGraphicsSystem::DispatchInterrupt(uint32_t source, uint32_t cpu) {
  if (!interrupt_callback_) return;
  auto* thread = rex::system::XThread::GetCurrentThread();
  if (!thread) return;
  thread->SetActiveCpu(cpu == 0xFFFFFFFF ? 2 : cpu);
  uint64_t args[] = {source, interrupt_callback_data_};
  function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args, 2);
}

// --- Vblank -------------------------------------------------------------------

void D3D12NativeGraphicsSystem::VsyncThreadMain() {
  // Conan presents every second vblank (D3DPRESENT_INTERVAL_TWO), so the
  // guest vblank rate is twice the frame rate limit. SR's cadence is
  // profile::kVblanksPerFrame (unconfirmed). Host present vsync is separate
  // (`vsync`, D3D12 presenter).
  uint64_t freq = rex::chrono::Clock::guest_tick_frequency();
  int32_t fps = REXCVAR_GET(sr_native_fps_limit);
  int32_t vblank_hz = fps > 0 ? int32_t(profile::kVblanksPerFrame) * std::clamp(fps, 10, 1000)
                              : std::max(int32_t(1), REXCVAR_GET(sr_native_unlocked_vblank_hz));  REXLOG_INFO("native graphics: frame rate limit {} (guest vblank {} Hz)", fps, vblank_hz);
  uint64_t interval = std::max(uint64_t(1), freq / uint64_t(vblank_hz));
  uint64_t last = rex::chrono::Clock::QueryGuestTickCount();
  // rex::thread::Sleep is ::Sleep in whole milliseconds at the system timer
  // resolution (up to 15.6 ms): vblanks came up to a frame late and the game's
  // frames alternated 21 / 12 ms at the 60 FPS limit. A high-resolution timer
  // wakes up 200 us early and the last stretch yields.
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  while (running_) {
    uint64_t now = rex::chrono::Clock::QueryGuestTickCount();
    if (now - last < interval) {
      uint64_t remaining_us = (interval - (now - last)) * 1000000ull / freq;
      if (remaining_us > 300 && timer) {
        LARGE_INTEGER due;
        due.QuadPart = -int64_t(std::min<uint64_t>(remaining_us - 200, 8000ull) * 10);
        if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
          WaitForSingleObject(timer, 20);
        }
      } else if (remaining_us > 300) {
        rex::thread::Sleep(std::chrono::microseconds(std::min<uint64_t>(remaining_us, 8000ull)));
      } else {
        SwitchToThread();
      }
      continue;
    }
    // Vblank: the guest sees progress and its vblank handler runs.
    counter_.fetch_add(1);
    cp_wake_->Set();
    DispatchInterrupt(0, 2);
    last += interval;
    if (rex::chrono::Clock::QueryGuestTickCount() - last >= interval) {
      last = rex::chrono::Clock::QueryGuestTickCount();
    }
  }
  if (timer) CloseHandle(timer);
}

// --- PM4 consumer -------------------------------------------------------------

void D3D12NativeGraphicsSystem::CommandThreadMain() {
  compat::RegisterSampledThread(1, "native_gpu_commands");
  while (running_) {
    uint32_t write_index = write_index_.load();
    if (write_index == 0xBAADF00D || write_index == read_index_ || !ring_dwords_) {
      // Spin briefly (the guest usually kicks again right away), then block.
      bool kicked = false;
      for (int i = 0; i < 64 && !kicked; ++i) {
        _mm_pause();
        write_index = write_index_.load();
        kicked = write_index != 0xBAADF00D && write_index != read_index_;
      }
      if (!kicked) {
        rex::thread::Wait(write_event_.get(), false, std::chrono::milliseconds(2));
        continue;
      }
    }
    write_index %= ring_dwords_;
    Reader reader;
    reader.base = memory_->TranslatePhysical<const uint8_t*>(ring_base_);
    reader.capacity = ring_dwords_;
    reader.offset = read_index_;
    reader.remaining = (write_index + ring_dwords_ - read_index_) % ring_dwords_;
    while (reader.remaining && running_) {
      if (!ExecutePacket(reader)) {
        REXLOG_ERROR("native graphics: bad PM4 packet in the ring at {:08X}",
                     ring_base_ + 4 * reader.offset);
        break;
      }
    }
    read_index_ = write_index;
    if (read_ptr_writeback_) StoreBigEndian(read_ptr_writeback_, read_index_);
    SignalGpuProgress();
  }
}

void D3D12NativeGraphicsSystem::ExecuteBuffer(uint32_t address, uint32_t dwords, int depth) {
  if (!dwords || depth > 4) return;
  Reader reader;
  reader.base = memory_->TranslatePhysical<const uint8_t*>(address);
  reader.capacity = dwords;
  reader.offset = 0;
  reader.remaining = dwords;
  while (reader.remaining && running_) {
    if (!ExecutePacket(reader)) {
      REXLOG_ERROR("native graphics: bad PM4 packet in the indirect buffer {:08X}", address);
      return;
    }
  }
}

namespace {
// SR_NATIVE_RING_STATS=1: what the ring still carries, every 10 s (cumulative):
// type-0 dwords for render state (>= kFirstUnusedRegister) vs sync registers,
// and type-3 packets per opcode with their payload dwords.
struct RingStats {
  bool on = [] {const char* v=std::getenv("SR_NATIVE_RING_STATS");return v && *v && *v!='0';}();
  std::atomic<uint64_t> state_dwords{0}, sync_dwords{0}, type3[128]{}, type3_dwords[128]{};
  std::atomic<int64_t> last{0};
  void MaybeLog() {
    const int64_t now=std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t prev=last.load();
    if(prev==0) {last.compare_exchange_strong(prev,now);return;}
    if(now-prev<10 || !last.compare_exchange_strong(prev,now)) return;
    std::string t;
    for(uint32_t i=0;i<128;++i) if(type3[i]) t+=fmt::format(" {:02X}:{}/{}",i,type3[i].load(),type3_dwords[i].load());
    REXLOG_INFO("native ring stats: type0 state_dwords={} sync_dwords={} type3 op:packets/dwords{}",
                state_dwords.load(),sync_dwords.load(),t);
  }
} ring_stats;
}  // namespace

bool D3D12NativeGraphicsSystem::ExecutePacket(Reader& reader) {
  uint32_t packet = reader.Read();
  if (packet == 0) return true;
  switch (packet >> 30) {
    case 0: {  // register writes
      uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
      if (reader.remaining < count) return false;
      uint32_t index = packet & 0x7FFF;
      bool one = (packet >> 15) & 1;
      // Render state / constants (>= 0x2000) are only mirrored by the native
      // renderer; this sync-only consumer never reads them: skip the burst.
      if (ring_stats.on) {
        (index >= kFirstUnusedRegister ? ring_stats.state_dwords : ring_stats.sync_dwords) += count;
        ring_stats.MaybeLog();
      }
      if (index >= kFirstUnusedRegister) {
        reader.Skip(count);
        return true;
      }
      for (uint32_t i = 0; i < count; ++i) WriteRegister(one ? index : index + i, reader.Read());
      return true;
    }
    case 1: {
      if (reader.remaining < 2) return false;
      uint32_t a = reader.Read(), b = reader.Read();
      WriteRegister(packet & 0x7FF, a);
      WriteRegister((packet >> 11) & 0x7FF, b);
      return true;
    }
    case 2:
      return true;
    default:
      return ExecuteType3(reader, packet);
  }
}

uint32_t D3D12NativeGraphicsSystem::ReadRegister(uint32_t index) const {
  if (index >= kFirstUnusedRegister && index < kRegisterCount) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
      REXLOG_WARN("native graphics: read of render-state register {:04X}, which the sync-only "
                  "consumer does not track", index);
    }
  }
  return index < kRegisterCount ? registers_[index].load() : 0;
}

uint32_t D3D12NativeGraphicsSystem::LoadMemory(uint32_t address) const {
  uint32_t v;
  std::memcpy(&v, memory_->TranslatePhysical<const uint8_t*>(address & ~3u), 4);
  // Like Xenia: the GPU reads host-order dwords and the low address bits select
  // its endian swap (k8in32 turns big-endian guest data into the value).
  return xenos::GpuSwap(v, xenos::Endian(address & 3));
}

void D3D12NativeGraphicsSystem::StoreMemory(uint32_t address, uint32_t value) {
  uint32_t v = xenos::GpuSwap(value, xenos::Endian(address & 3));
  std::memcpy(memory_->TranslatePhysical<uint8_t*>(address & ~3u), &v, 4);
  SignalGpuProgress();  // fences: wake guest threads waiting on the GPU
}

void D3D12NativeGraphicsSystem::StoreBigEndian(uint32_t address, uint32_t value) {
  uint32_t v = __builtin_bswap32(value);
  std::memcpy(memory_->TranslatePhysical<uint8_t*>(address), &v, 4);
}

bool D3D12NativeGraphicsSystem::Compare(uint32_t func, uint32_t value, uint32_t ref) const {
  switch (func & 7) {
    case 0: return false;
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    default: return true;
  }
}

void D3D12NativeGraphicsSystem::WriteRegister(uint32_t index, uint32_t value) {
  if (index >= kRegisterCount) return;
  // Fast path: render state and ALU/fetch/bool/loop constants (thousands per
  // frame) have no side effects here; a relaxed store is a plain mov instead of
  // a locked xchg. Every register with a side effect below is < 0x2000.
  if (index >= 0x2000) {
    registers_[index].store(value, std::memory_order_relaxed);
    return;
  }
  if (REXCVAR_GET(sr_native_gpu_trace)) {
    static int logged = 0;
    if (logged++ < 60) REXLOG_INFO("native gpu trace: reg {:04X} = {:08X}", index, value);
  }
  registers_[index].store(value);
  if (index >= gpu::XE_GPU_REG_SCRATCH_REG0 && index <= gpu::XE_GPU_REG_SCRATCH_REG0 + 7) {
    // Scratch register write-back to memory (interrupt sync, fences).
    uint32_t n = index - gpu::XE_GPU_REG_SCRATCH_REG0;
    if (ReadRegister(gpu::XE_GPU_REG_SCRATCH_UMSK) & (1u << n)) {
      uint32_t address = ReadRegister(gpu::XE_GPU_REG_SCRATCH_ADDR) + 4 * n;
      uint32_t v = __builtin_bswap32(value);
      std::memcpy(memory_->TranslatePhysical<uint8_t*>(address), &v, 4);
    }
    return;
  }
  switch (index) {
    case gpu::XE_GPU_REG_COHER_STATUS_HOST:
      // Pending until a WAIT_REG_MEM on it makes memory coherent.
      registers_[index].store(value | 0x80000000u);
      break;
    case gpu::XE_GPU_REG_DC_LUT_RW_INDEX:
      gamma_component_ = 0;
      break;
    case gpu::XE_GPU_REG_DC_LUT_SEQ_COLOR: {
      uint32_t rw_index = ReadRegister(gpu::XE_GPU_REG_DC_LUT_RW_INDEX) & 0xFF;
      // Red, green, blue order; the write enable mask is blue, green, red.
      if (ReadRegister(gpu::XE_GPU_REG_DC_LUT_WRITE_EN_MASK) & (1u << (2 - gamma_component_))) {
        uint32_t c = (value & 0xFFFF) >> 6;
        uint32_t shift = gamma_component_ == 0 ? 20 : gamma_component_ == 1 ? 10 : 0;
        gamma_ramp_[rw_index] = (gamma_ramp_[rw_index] & ~(0x3FFu << shift)) | (c << shift);
      }
      if (++gamma_component_ >= 3) {
        WriteRegister(gpu::XE_GPU_REG_DC_LUT_RW_INDEX, (rw_index + 1) & 0xFF);
      }
      break;
    }
    case gpu::XE_GPU_REG_DC_LUT_PWL_DATA: {
      uint32_t rw_index = ReadRegister(gpu::XE_GPU_REG_DC_LUT_RW_INDEX);
      if (++gamma_component_ >= 3) {
        WriteRegister(gpu::XE_GPU_REG_DC_LUT_RW_INDEX,
                      (rw_index & ~0x7Fu) | (((rw_index & 0x7F) + 1) & 0x7F));
      }
      break;
    }
    case gpu::XE_GPU_REG_DC_LUT_30_COLOR: {
      uint32_t rw_index = ReadRegister(gpu::XE_GPU_REG_DC_LUT_RW_INDEX) & 0xFF;
      uint32_t mask = ReadRegister(gpu::XE_GPU_REG_DC_LUT_WRITE_EN_MASK) & 7;
      uint32_t e = gamma_ramp_[rw_index];
      if (mask & 1) e = (e & ~0x3FFu) | (value & 0x3FF);
      if (mask & 2) e = (e & ~(0x3FFu << 10)) | (value & (0x3FFu << 10));
      if (mask & 4) e = (e & ~(0x3FFu << 20)) | (value & (0x3FFu << 20));
      gamma_ramp_[rw_index] = e;
      WriteRegister(gpu::XE_GPU_REG_DC_LUT_RW_INDEX, (rw_index + 1) & 0xFF);
      break;
    }
    default:
      break;
  }
}

bool D3D12NativeGraphicsSystem::ExecuteType3(Reader& reader, uint32_t packet) {
  uint32_t opcode = (packet >> 8) & 0x7F;
  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  if (reader.remaining < count) return false;
  if (ring_stats.on) {
    ++ring_stats.type3[opcode];
    ring_stats.type3_dwords[opcode] += count;
  }
  {
    static int logged = 0;
    if (logged < 60 && REXCVAR_GET(sr_native_gpu_trace)) {
      ++logged;
      std::string d;
      Reader peek = reader;
      for (uint32_t i = 0; i < std::min(count, 6u); ++i) d += fmt::format(" {:08X}", peek.Read());
      REXLOG_INFO("native gpu trace: type3 op {:02X} count {} pred {}:{}", opcode, count, packet & 1, d);
    }
  }
  uint32_t end_remaining = reader.remaining - count;
  auto finish = [&]() {
    if (reader.remaining > end_remaining) reader.Skip(reader.remaining - end_remaining);
    return true;
  };
  // Predicated packets (tiling bins); predicated swaps are never valid.
  if ((packet & 1) && (!(bin_select_ & bin_mask_) || opcode == xenos::PM4_XE_SWAP)) {
    return finish();
  }
  switch (opcode) {
    case xenos::PM4_INDIRECT_BUFFER:
    case xenos::PM4_INDIRECT_BUFFER_PFD: {
      uint32_t address = xenos::GpuToCpu(xenos::CpuToGpu(reader.Read()));
      uint32_t length = reader.Read() & 0xFFFFF;
      finish();
      ExecuteBuffer(address, length, 1);
      return true;
    }
    case xenos::PM4_INTERRUPT: {
      uint32_t cpu_mask = reader.Read();
      for (uint32_t n = 0; n < 6; ++n) {
        if (cpu_mask & (1u << n)) DispatchInterrupt(1, n);
      }
      return finish();
    }
    case xenos::PM4_XE_SWAP: {  // written by VdSwap
      static uint64_t last_tick = 0;
      uint64_t now = rex::chrono::Clock::QueryHostTickCount();
      if (last_tick) {
        uint64_t f = rex::chrono::Clock::QueryHostTickFrequency();
        PROFILE_FRAME_TIME_US(int64_t((now - last_tick) * 1000000 / f));
        PROFILE_FPS(int64_t(f / std::max<uint64_t>(1, now - last_tick)));
      }
      last_tick = now;
      rex::perf::Profiler::Flip();
      counter_.fetch_add(1);
      return finish();
    }
    case xenos::PM4_WAIT_REG_MEM: {
      uint32_t info = reader.Read(), poll = reader.Read(), ref = reader.Read(),
               mask = reader.Read(), wait = reader.Read();
      bool is_memory = (info & 0x10) != 0;
      auto start = std::chrono::steady_clock::now();
      bool reported = false;
      for (;;) {
        uint32_t value;
        if (is_memory) {
          value = LoadMemory(poll);
        } else {
          if (poll == gpu::XE_GPU_REG_COHER_STATUS_HOST) {
            registers_[poll].store(0);  // everything is coherent: there is no GPU cache
          }
          value = ReadRegister(poll);
        }
        if (Compare(info, value & mask, ref) || !running_) break;
        if (!reported && std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
          reported = true;
          REXLOG_WARN("native graphics: WAIT_REG_MEM {} {:08X} value {:08X} mask {:08X} func {} ref "
                      "{:08X} stalled > 2 s",
                      is_memory ? "mem" : "reg", poll, value, mask, info & 7, ref);
        }
        // Block instead of spinning: register polls wait for a vblank or an
        // MMIO register write; memory polls (written by guest CPU threads)
        // re-check every 100 us.
        if (is_memory || wait >= 0x100) {
          rex::thread::Sleep(std::chrono::microseconds(100));
        } else {
          rex::thread::Wait(cp_wake_.get(), false, std::chrono::milliseconds(1));
        }
        rex::thread::SyncMemory();
      }
      return finish();
    }
    case xenos::PM4_REG_RMW: {
      uint32_t info = reader.Read(), and_mask = reader.Read(), or_mask = reader.Read();
      uint32_t reg = info & 0x1FFF;
      uint32_t value = ReadRegister(reg);
      value &= (info >> 31) & 1 ? ReadRegister(and_mask & 0x1FFF) : and_mask;
      value |= (info >> 30) & 1 ? ReadRegister(or_mask & 0x1FFF) : or_mask;
      WriteRegister(reg, value);
      return finish();
    }
    case xenos::PM4_REG_TO_MEM: {
      uint32_t reg = reader.Read(), address = reader.Read();
      StoreMemory(address, ReadRegister(reg));
      return finish();
    }
    case xenos::PM4_MEM_WRITE: {
      uint32_t address = reader.Read();
      for (uint32_t i = 1; i < count; ++i, address += 4) StoreMemory(address, reader.Read());
      return finish();
    }
    case xenos::PM4_COND_WRITE: {
      uint32_t info = reader.Read(), poll = reader.Read(), ref = reader.Read(),
               mask = reader.Read(), target = reader.Read(), data = reader.Read();
      uint32_t value = (info & 0x10) ? LoadMemory(poll) : ReadRegister(poll);
      if (Compare(info, value & mask, ref)) {
        if (info & 0x100) {
          StoreMemory(target, data);
        } else {
          WriteRegister(target, data);
        }
      }
      return finish();
    }
    case xenos::PM4_EVENT_WRITE: {
      WriteRegister(gpu::XE_GPU_REG_VGT_EVENT_INITIATOR, reader.Read() & 0x3F);
      return finish();
    }
    case xenos::PM4_EVENT_WRITE_SHD: {  // fence value (or the frame counter) to memory
      uint32_t initiator = reader.Read(), address = reader.Read(), value = reader.Read();
      WriteRegister(gpu::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
      const uint32_t stored = (initiator >> 31) & 1 ? counter_.load() : value;
      // Native fences complete ahead of the ring; never move a value back.
      if (superman_returns::graphics::guest::SkipOriginalRequested(superman_returns::graphics::guest::OriginalCall::kFence)) {
        uint32_t current;
        std::memcpy(&current, memory_->TranslatePhysical<const uint8_t*>(address & ~3u), 4);
        current = xenos::GpuSwap(current, xenos::Endian(address & 3));
        if (int32_t(stored - current) < 0) return finish();
      }
      StoreMemory(address, stored);
      return finish();
    }
    case xenos::PM4_EVENT_WRITE_EXT: {  // screen extents of the previous draws
      uint32_t initiator = reader.Read(), address = reader.Read();
      WriteRegister(gpu::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
      const uint16_t extents[] = {0, uint16_t(xenos::kTexture2DCubeMaxWidthHeight >> 3), 0,
                                  uint16_t(xenos::kTexture2DCubeMaxWidthHeight >> 3), 0, 1};
      uint8_t* dst = memory_->TranslatePhysical<uint8_t*>(address & ~3u);
      for (uint32_t i = 0; i < 6; ++i) {
        uint16_t v = __builtin_bswap16(extents[i]);  // k8in16
        std::memcpy(dst + 2 * i, &v, 2);
      }
      return finish();
    }
    case xenos::PM4_EVENT_WRITE_ZPD: {  // occlusion queries report a fixed sample count
      WriteRegister(gpu::XE_GPU_REG_VGT_EVENT_INITIATOR, reader.Read() & 0x3F);
      uint32_t counts_address = ReadRegister(gpu::XE_GPU_REG_RB_SAMPLE_COUNT_ADDR);
      if (counts_address) {
        auto* counts = memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(counts_address);
        const uint32_t kFinished = __builtin_bswap32(0xFFFFFEED);
        bool end = counts->ZPass_A == kFinished || counts->ZPass_B == kFinished ||
                   counts->ZFail_A == kFinished || counts->ZFail_B == kFinished;
        std::memset(counts, 0, sizeof(*counts));
        if (end) {
          counts->ZPass_A = 1000;
          counts->Total_A = 1000;
        }
      }
      return finish();
    }
    case xenos::PM4_SET_CONSTANT: {
      uint32_t offset_type = reader.Read();
      uint32_t index = offset_type & 0x7FF;
      uint32_t first;
      switch ((offset_type >> 16) & 0xFF) {
        case 0: first = 0x4000 + index; break;
        case 1: first = 0x4800 + index; break;
        case 2: first = 0x4900 + index; break;
        case 3: first = 0x4908 + index; break;
        case 4: first = 0x2000 + index; break;
        default: return finish();
      }
      if (first >= kFirstUnusedRegister) return finish();  // constants: see type 0
      for (uint32_t i = 1; i < count; ++i) WriteRegister(first + i - 1, reader.Read());
      return finish();
    }
    case xenos::PM4_SET_CONSTANT2:
    case xenos::PM4_SET_SHADER_CONSTANTS: {
      uint32_t index = reader.Read() & 0xFFFF;
      if (index >= kFirstUnusedRegister) return finish();
      for (uint32_t i = 1; i < count; ++i) WriteRegister(index + i - 1, reader.Read());
      return finish();
    }
    case xenos::PM4_LOAD_ALU_CONSTANT: {  // registers from memory
      uint32_t address = reader.Read() & 0x3FFFFFFF;
      uint32_t offset_type = reader.Read();
      uint32_t size = reader.Read() & 0xFFF;
      uint32_t index = offset_type & 0x7FF;
      uint32_t first;
      switch ((offset_type >> 16) & 0xFF) {
        case 0: first = 0x4000 + index; break;
        case 1: first = 0x4800 + index; break;
        case 2: first = 0x4900 + index; break;
        case 3: first = 0x4908 + index; break;
        case 4: first = 0x2000 + index; break;
        default: return finish();
      }
      if (first >= kFirstUnusedRegister) return finish();
      const uint8_t* src = memory_->TranslatePhysical<const uint8_t*>(address);
      for (uint32_t i = 0; i < size; ++i) {
        uint32_t v;
        std::memcpy(&v, src + 4 * i, 4);
        WriteRegister(first + i, __builtin_bswap32(v));
      }
      return finish();
    }
    case xenos::PM4_VIZ_QUERY: {
      uint32_t d = reader.Read();
      uint32_t id = d & 0x3F;
      if (!(d & 0x100)) {
        WriteRegister(gpu::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_START);
      } else {
        WriteRegister(gpu::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_END);
        uint32_t reg = id < 32 ? gpu::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0
                               : gpu::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1;
        registers_[reg].fetch_or(1u << (id & 31));
      }
      return finish();
    }
    case xenos::PM4_SET_BIN_MASK_LO: {
      bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | reader.Read();
      return finish();
    }
    case xenos::PM4_SET_BIN_MASK_HI: {
      bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(reader.Read()) << 32);
      return finish();
    }
    case xenos::PM4_SET_BIN_SELECT_LO: {
      bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | reader.Read();
      return finish();
    }
    case xenos::PM4_SET_BIN_SELECT_HI: {
      bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(reader.Read()) << 32);
      return finish();
    }
    case xenos::PM4_SET_BIN_MASK: {
      uint64_t hi = reader.Read(), lo = reader.Read();
      bin_mask_ = (hi << 32) | lo;
      return finish();
    }
    case xenos::PM4_SET_BIN_SELECT: {
      uint64_t hi = reader.Read(), lo = reader.Read();
      bin_select_ = (hi << 32) | lo;
      return finish();
    }
    default:
      // ME_INIT, NOP, draws, IM_LOAD(_IMMEDIATE), INVALIDATE_STATE,
      // CONTEXT_UPDATE, WAIT_FOR_IDLE...: nothing the guest can observe.
      return finish();
  }
}

}  // namespace superman_returns::native

