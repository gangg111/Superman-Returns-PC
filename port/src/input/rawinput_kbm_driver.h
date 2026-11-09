// Keyboard and mouse through Raw Input, mapped to a virtual Xbox 360 pad with
// the SDK's binding options (mnk_mode, mnk_mouse, mnk_sensitivity,
// keybind_*), so existing command lines and configs keep working. Mouse
// buttons fill the same key table (VK_LBUTTON...), so "LButton" and friends
// can be bound. Raw Input is read on a dedicated thread with its own
// message-only window; the game window must be in the foreground.
#pragma once

#include <atomic>
#include <mutex>
#include <thread>

#include <rex/input/input_driver.h>

namespace superman_returns::input {

using rex::X_RESULT;
using rex::X_STATUS;

class RawInputKbmDriver final : public rex::input::InputDriver {
 public:
  RawInputKbmDriver();
  ~RawInputKbmDriver() override;

  X_STATUS Setup() override;
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override;
  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t flags,
                                 rex::input::X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(rex::input::DeviceId id,
                              rex::input::X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t flags,
                              rex::input::X_INPUT_KEYSTROKE* out_keystroke) override;
  void OnWindowAvailable(rex::ui::Window* window) override;

  // From the Raw Input thread.
  void OnRawInput(const void* raw);
  // UI thread: the cursor is visible (not mouse-looking, no pad in use).
  bool CursorShown() const;

 private:
  bool Enabled() const;
  bool GameInForeground() const;
  void UpdateMouseCapture(bool capture);
  void ApplyCursor();  // UI thread: hidden while mouse-looking or while a pad is connected
 public:
  void OnCursorTimer();  // Raw Input thread, every 250 ms
 private:
  void ThreadMain();

  std::mutex mutex_;
  bool key_down_[256] = {};
  double mouse_dx_ = 0.0, mouse_dy_ = 0.0;
  uint32_t packet_ = 0;
  bool was_foreground_ = false;
  int key_presses_ = 0;          // first presses logged (startup check)
  uint16_t last_buttons_ = 0;    // logged when the pressed set changes
  int button_logs_ = 0, mouse_logs_ = 0;

  rex::ui::Window* window_ = nullptr;
  std::atomic<void*> game_hwnd_{nullptr};
  std::atomic<void*> message_hwnd_{nullptr};
  std::thread thread_;
  std::atomic<bool> requested_capture_{false};  // last state sent to the UI thread
  bool mouse_captured_ = false;                 // UI thread
  std::atomic<bool> pad_hides_cursor_{false};   // a pad is the input in use (sr_hide_cursor_with_pad)
  std::atomic<int64_t> last_kbm_use_ms_{0};      // last key, click or mouse move (NowMs)
};

}  // namespace superman_returns::input
