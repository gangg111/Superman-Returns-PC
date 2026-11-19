// Ported from crazyriddler/rexglue-native-kit @136bc6c4,
// reference/conan/port/src/native/native_graphics_system.h (Conan native renderer).
// The kit ships no license file at that revision; parts derived from Xenia /
// ReXGlue keep their BSD license. Changes for Superman Returns are listed in
// docs/native-port-plan.md section 2.
//
// Native graphics system: replaces the Xenos GPU plugin when the native
// renderer is active (docs/NATIVE_RENDERER_DESIGN.md, EXP-036).
//
// Rendering is done entirely by superman_returns::native::Renderer from the guest's D3D
// calls. What the guest still needs from "the GPU" is its synchronization
// contract, which this class provides without any Xenos emulation:
//   - GPU registers over MMIO (0x7FC80000), incl. the ring write pointer kick
//   - the PM4 ring consumer: read pointer write-back, fences (EVENT_WRITE_SHD,
//     MEM_WRITE, REG_TO_MEM, COND_WRITE, scratch registers), WAIT_REG_MEM,
//     INTERRUPT, XE_SWAP frame counting, the display gamma table (DC_LUT)
//   - the vblank timer and the guest graphics interrupt callback
//   - the D3D12 provider/presenter the renderer draws into
// Draw, shader, texture and render-target packets are skipped: the native
// renderer already consumed that state through its PM4 mirror.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include "../graphics/backend_selection.h"
#include "native_graphics_system_interface.h"

#include <rex/system/interfaces/graphics.h>
#include <rex/system/xobject.h>

namespace rex::memory {
class Memory;
}
namespace rex::system {
class XHostThread;
}
namespace rex::thread {
class Event;
}
namespace rex::ui::d3d12 {
class D3D12Provider;
}

namespace superman_returns::native {


// The running native graphics system, or nullptr (Xenos / A/B mode).


// GPU progress notification for guest threads that wait on the GPU (the XDK
// polls fences and the read pointer in a busy loop). GpuProgressGeneration()
// changes whenever the PM4 consumer writes memory (fences, write-backs) or
// advances the read pointer; WaitForGpuProgress blocks until it differs from
// `since` or the timeout expires. No-ops without the native graphics system.


// Blocks until `done()` holds (re-checked on every GPU progress signal) or the
// timeout expires.
void WaitForGpuCondition(const std::function<bool()>& done, uint32_t timeout_us);

class D3D12NativeGraphicsSystem : public INativeGraphicsSystem {
 public:
  explicit D3D12NativeGraphicsSystem(graphics::NativeApi api=graphics::NativeApi::kD3D12);
  ~D3D12NativeGraphicsSystem() override;

  rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override;
  rex::X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                         rex::system::KernelState* kernel_state) override;
  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override;
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }
  // Not IGraphicsSystem overrides: the stock SDK interface has neither (they
  // are additions of the kit's SDK fork). The renderer calls them directly.
  uint32_t guest_frame_counter() const override { return counter_.load(); }
  bool GetGammaRamp256(uint32_t* out_entries) const override;
  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override;
  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override;
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override;
  void Shutdown() override;

 private:
  static constexpr uint32_t kRegisterCount = 0x5003;
  // Render state and shader constants start here: not tracked (the native
  // renderer mirrors them itself from the same PM4 stream).
  static constexpr uint32_t kFirstUnusedRegister = 0x2000;

  static uint32_t ReadRegisterThunk(void* ppc_context, void* context, uint32_t addr);
  static void WriteRegisterThunk(void* ppc_context, void* context, uint32_t addr, uint32_t value);
  uint32_t ReadMmioRegister(uint32_t addr);
  void WriteMmioRegister(uint32_t addr, uint32_t value);
  void DispatchInterrupt(uint32_t source, uint32_t cpu);

  // PM4 consumer (command processor thread).
  void CommandThreadMain();
  void VsyncThreadMain();
  struct Reader;
  bool ExecutePacket(Reader& reader);
  bool ExecuteType3(Reader& reader, uint32_t packet);
  void ExecuteBuffer(uint32_t address, uint32_t dwords, int depth);
  void WriteRegister(uint32_t index, uint32_t value);
  uint32_t ReadRegister(uint32_t index) const;
  uint32_t LoadMemory(uint32_t address) const;
  void StoreMemory(uint32_t address, uint32_t value);
  void StoreBigEndian(uint32_t address, uint32_t value);  // write-back/scratch
  bool Compare(uint32_t func, uint32_t value, uint32_t ref) const;

  std::unique_ptr<rex::ui::GraphicsProvider> provider_;
  graphics::NativeApi native_api_;
  std::unique_ptr<rex::ui::Presenter> presenter_;
  rex::ui::WindowedAppContext* app_context_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  rex::runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;

  // Register file: MMIO reads/writes and PM4 register writes (WAIT_REG_MEM,
  // REG_TO_MEM, scratch write-back, gamma table).
  std::unique_ptr<std::atomic<uint32_t>[]> registers_;
  uint32_t gamma_ramp_[256] = {};  // DC_LUT_30_COLOR entries
  uint32_t gamma_component_ = 0;

  // Ring.
  uint32_t ring_base_ = 0;
  uint32_t ring_dwords_ = 0;
  uint32_t read_index_ = 0;
  std::atomic<uint32_t> write_index_{0xBAADF00D};
  uint32_t read_ptr_writeback_ = 0;
  std::unique_ptr<rex::thread::Event> write_event_;
  // Wakes the PM4 consumer while it waits in WAIT_REG_MEM on a register
  // (vblank, MMIO register writes) instead of spinning.
  std::unique_ptr<rex::thread::Event> cp_wake_;
  // A register WAIT_REG_MEM is waiting on cp_wake_: MMIO writes signal it only then. Setting the
  // event (mutex + broadcast, a futex syscall) on every guest register write was ~3% of the game's
  // render thread on the Fold7. Both sides are seq_cst: the waiter sets it before reading the
  // register, the writer reads it after storing the register, so one of them sees the other.
  std::atomic<bool> cp_waiting_{false};
 public:
  void SignalGpuProgress() override;
  uint64_t progress_generation() const override { return progress_generation_.load(); }
  void WaitProgress(uint64_t since, uint32_t timeout_us) override;
  void WaitCondition(const std::function<bool()>& done, uint32_t timeout_us);
 private:
  std::atomic<uint64_t> progress_generation_{0};
  std::mutex progress_mutex_;
  std::condition_variable progress_cv_;
  uint64_t bin_mask_ = ~0ull, bin_select_ = ~0ull;

  std::atomic<uint32_t> counter_{0};
  uint32_t interrupt_callback_ = 0, interrupt_callback_data_ = 0;

  std::atomic<bool> running_{false};
  rex::system::object_ref<rex::system::XHostThread> command_thread_;
  rex::system::object_ref<rex::system::XHostThread> vsync_thread_;
};

// Preserve existing factory callers while the graphics-system split is in progress.
using NativeGraphicsSystem = D3D12NativeGraphicsSystem;

}  // namespace superman_returns::native

