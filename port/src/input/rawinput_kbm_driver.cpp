#include "input/rawinput_kbm_driver.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

namespace superman_returns::input {

using rex::input::DeviceId;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

namespace {

constexpr DeviceId kDevice = static_cast<DeviceId>(0x52494B4D);  // "RIKM"

// Bindings: the SDK's keybind_* syntax (mnk_input_driver.cpp), a comma
// separated list of alternatives, each with optional exact modifiers
// ("Shift+Up"), key names parsed by rex::ui::ParseVirtualKey.
constexpr uint8_t kModShift = 1, kModCtrl = 2, kModAlt = 4;

uint8_t LiveModifiers(const bool (&down)[256]) {
  return (down[VK_SHIFT] ? kModShift : 0) | (down[VK_CONTROL] ? kModCtrl : 0) |
         (down[VK_MENU] ? kModAlt : 0);
}

bool TokenPressed(const bool (&down)[256], std::string_view token, uint8_t live) {
  uint8_t want = 0;
  for (size_t plus; (plus = token.find('+')) != std::string_view::npos && plus > 0;) {
    const std::string_view head = token.substr(0, plus);
    const uint8_t bit = head == "Shift" ? kModShift
                        : head == "Ctrl" || head == "Control" ? kModCtrl
                        : head == "Alt" ? kModAlt
                                         : 0;
    if (!bit) break;
    want |= bit;
    token.remove_prefix(plus + 1);
  }
  if (want != live) return false;
  const uint16_t vk = uint16_t(rex::ui::ParseVirtualKey(token));
  return vk && vk < 256 && down[vk];
}

bool Pressed(const bool (&down)[256], const char* cvar) {
  const std::string binds = rex::cvar::Query<std::string>(cvar);
  const uint8_t live = LiveModifiers(down);
  std::string_view rest(binds);
  while (!rest.empty()) {
    const size_t comma = rest.find(',');
    std::string_view token = rest.substr(0, comma);
    rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
    while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
    while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
    if (!token.empty() && TokenPressed(down, token, live)) return true;
  }
  return false;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_INPUT) {
    auto* driver = reinterpret_cast<RawInputKbmDriver*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    UINT size = 0;
    GetRawInputData(HRAWINPUT(lp), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
    alignas(8) uint8_t buf[256];
    if (driver && size && size <= sizeof(buf) &&
        GetRawInputData(HRAWINPUT(lp), RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == size)
      driver->OnRawInput(buf);
  } else if (msg == WM_CLOSE) {
    DestroyWindow(hwnd);
    return 0;
  } else if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

RawInputKbmDriver::RawInputKbmDriver() : InputDriver(nullptr, 0) {}

RawInputKbmDriver::~RawInputKbmDriver() {
  if (HWND h = HWND(message_hwnd_.load())) PostMessageW(h, WM_CLOSE, 0, 0);
  if (thread_.joinable()) thread_.join();
  ClipCursor(nullptr);
}

X_STATUS RawInputKbmDriver::Setup() {
  thread_ = std::thread([this] { ThreadMain(); });
  return X_STATUS_SUCCESS;
}

void RawInputKbmDriver::ThreadMain() {
  const HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSW wc = {};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = L"SrRawInputSink";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                              inst, nullptr);
  if (!hwnd) {
    REXLOG_WARN("Raw Input: no message window ({}), keyboard and mouse disabled", GetLastError());
    return;
  }
  SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(this));
  // INPUTSINK: delivered while another window (the game's) has the focus.
  // No NOLEGACY, so the game window keeps its normal key and mouse messages.
  RAWINPUTDEVICE devices[2] = {{0x01, 0x02, RIDEV_INPUTSINK, hwnd},   // mouse
                               {0x01, 0x06, RIDEV_INPUTSINK, hwnd}};  // keyboard
  if (!RegisterRawInputDevices(devices, 2, sizeof(devices[0]))) {
    REXLOG_WARN("Raw Input: registration failed ({})", GetLastError());
    DestroyWindow(hwnd);
    return;
  }
  message_hwnd_ = hwnd;
  REXLOG_INFO("Raw Input: keyboard and mouse ready");
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
}

void RawInputKbmDriver::OnWindowAvailable(rex::ui::Window* window) {
  window_ = window;
  game_hwnd_ = window ? window->GetNativeWindowHandle() : nullptr;
}

bool RawInputKbmDriver::Enabled() const {
  return rex::cvar::Query<bool>("mnk_mode");
}

bool RawInputKbmDriver::GameInForeground() const {
  void* game = game_hwnd_.load();
  return game && GetForegroundWindow() == HWND(game);
}

void RawInputKbmDriver::OnRawInput(const void* data) {
  if (!GameInForeground()) return;
  const RAWINPUT& raw = *static_cast<const RAWINPUT*>(data);
  std::lock_guard lock(mutex_);
  if (raw.header.dwType == RIM_TYPEKEYBOARD) {
    const RAWKEYBOARD& k = raw.data.keyboard;
    if (k.VKey < 255) {
      const bool down = !(k.Flags & RI_KEY_BREAK);
      if (down && !key_down_[k.VKey] && ++key_presses_ <= 32)
        REXLOG_INFO("Raw Input: key down VK 0x{:02X} (#{})", k.VKey, key_presses_);
      key_down_[k.VKey] = down;
    }
  } else if (raw.header.dwType == RIM_TYPEMOUSE) {
    const RAWMOUSE& m = raw.data.mouse;
    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {
      mouse_dx_ += m.lLastX;
      mouse_dy_ += m.lLastY;
    }
    const USHORT f = m.usButtonFlags;
    constexpr struct {
      USHORT down, up;
      int vk;
    } kMouseButtons[] = {{RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, VK_LBUTTON},
                         {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, VK_RBUTTON},
                         {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, VK_MBUTTON},
                         {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, VK_XBUTTON1},
                         {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, VK_XBUTTON2}};
    for (const auto& b : kMouseButtons) {
      if (f & b.down) key_down_[b.vk] = true;
      if (f & b.up) key_down_[b.vk] = false;
    }
  }
}

void RawInputKbmDriver::UpdateMouseCapture(bool capture) {
  if (!window_ || capture == mouse_captured_) return;
  mouse_captured_ = capture;
  window_->SetCursorVisibility(capture ? rex::ui::Window::CursorVisibility::kHidden
                                       : rex::ui::Window::CursorVisibility::kVisible);
  if (!capture) ClipCursor(nullptr);
}

void RawInputKbmDriver::EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) {
  if (!Enabled()) return;
  rex::input::DeviceInfo info;
  info.id = kDevice;
  info.name = "Keyboard and Mouse (Raw Input)";
  info.synthetic = true;
  out.push_back(info);
}

X_RESULT RawInputKbmDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  if (!Enabled() || id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
  const bool foreground = GameInForeground();
  const bool live = foreground && is_active();
  const bool mouse_look = live && rex::cvar::Query<bool>("mnk_mouse");

  // Mouse look: hidden cursor held at the window centre (Raw Input reports
  // device motion, so the clip does not limit it). Windows drops the clip on
  // activation changes, so it is re-applied while capturing.
  if (window_ && requested_capture_.exchange(mouse_look) != mouse_look) {
    window_->app_context().CallInUIThreadDeferred([this, mouse_look] {
      UpdateMouseCapture(mouse_look);
    });
  }
  if (mouse_look) {
    HWND game = HWND(game_hwnd_.load());
    RECT r;
    if (GetClientRect(game, &r)) {
      POINT c = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
      ClientToScreen(game, &c);
      const RECT clip = {c.x, c.y, c.x + 1, c.y + 1};
      ClipCursor(&clip);
    }
  }

  std::lock_guard lock(mutex_);
  if (!foreground && was_foreground_) {
    // Focus lost: no key stays held, no motion backlog.
    std::memset(key_down_, 0, sizeof(key_down_));
  }
  was_foreground_ = foreground;
  const double dx = mouse_dx_, dy = mouse_dy_;
  mouse_dx_ = mouse_dy_ = 0.0;
  ++packet_;
  if (!out_state) return X_ERROR_SUCCESS;
  std::memset(out_state, 0, sizeof(*out_state));
  out_state->packet_number = packet_;
  if (!live) return X_ERROR_SUCCESS;

  const auto& k = key_down_;
  constexpr struct {
    const char* cvar;
    uint16_t button;
  } kBinds[] = {
      {"keybind_a", rex::input::X_INPUT_GAMEPAD_A},
      {"keybind_b", rex::input::X_INPUT_GAMEPAD_B},
      {"keybind_x", rex::input::X_INPUT_GAMEPAD_X},
      {"keybind_y", rex::input::X_INPUT_GAMEPAD_Y},
      {"keybind_left_shoulder", rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER},
      {"keybind_right_shoulder", rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER},
      {"keybind_lstick_press", rex::input::X_INPUT_GAMEPAD_LEFT_THUMB},
      {"keybind_rstick_press", rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB},
      {"keybind_back", rex::input::X_INPUT_GAMEPAD_BACK},
      {"keybind_start", rex::input::X_INPUT_GAMEPAD_START},
      {"keybind_guide", rex::input::X_INPUT_GAMEPAD_GUIDE},
      {"keybind_dpad_up", rex::input::X_INPUT_GAMEPAD_DPAD_UP},
      {"keybind_dpad_down", rex::input::X_INPUT_GAMEPAD_DPAD_DOWN},
      {"keybind_dpad_left", rex::input::X_INPUT_GAMEPAD_DPAD_LEFT},
      {"keybind_dpad_right", rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT},
  };
  uint16_t buttons = 0;
  for (const auto& b : kBinds)
    if (Pressed(k, b.cvar)) buttons |= b.button;
  if (buttons && buttons != last_buttons_ && ++button_logs_ <= 32)
    REXLOG_INFO("Raw Input: pad buttons 0x{:04X} sent to the game", buttons);
  last_buttons_ = buttons;

  auto axis = [&k](const char* neg, const char* pos) {
    return (Pressed(k, pos) ? 32767 : 0) - (Pressed(k, neg) ? 32767 : 0);
  };
  int32_t lx = axis("keybind_lstick_left", "keybind_lstick_right");
  int32_t ly = axis("keybind_lstick_down", "keybind_lstick_up");
  int32_t rx = axis("keybind_rstick_left", "keybind_rstick_right");
  int32_t ry = axis("keybind_rstick_down", "keybind_rstick_up");
  if (mouse_look) {
    // Same scale as the SDK's mnk driver: 200 stick units per count.
    const double s = rex::cvar::Query<double>("mnk_sensitivity") * 200.0;
    rx += int32_t(dx * s);
    ry += int32_t(-dy * s);
  }
  if (mouse_look && (dx || dy) && ++mouse_logs_ <= 8)
    REXLOG_INFO("Raw Input: mouse {} {} -> right stick {} {}", dx, dy, rx, ry);
  auto clamp16 = [](int32_t v) { return int16_t(std::clamp(v, -32768, 32767)); };
  out_state->gamepad.buttons = buttons;
  out_state->gamepad.left_trigger = Pressed(k, "keybind_left_trigger") ? 0xFF : 0;
  out_state->gamepad.right_trigger = Pressed(k, "keybind_right_trigger") ? 0xFF : 0;
  out_state->gamepad.thumb_lx = clamp16(lx);
  out_state->gamepad.thumb_ly = clamp16(ly);
  out_state->gamepad.thumb_rx = clamp16(rx);
  out_state->gamepad.thumb_ry = clamp16(ry);
  return X_ERROR_SUCCESS;
}

X_RESULT RawInputKbmDriver::GetDeviceCapabilities(DeviceId id, uint32_t,
                                                  X_INPUT_CAPABILITIES* out_caps) {
  if (!Enabled() || id != kDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;
    out_caps->sub_type = 0x01;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = int16_t(0x7FFF);
    out_caps->gamepad.thumb_ly = int16_t(0x7FFF);
    out_caps->gamepad.thumb_rx = int16_t(0x7FFF);
    out_caps->gamepad.thumb_ry = int16_t(0x7FFF);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT RawInputKbmDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) {
  return Enabled() && id == kDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT RawInputKbmDriver::GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE*) {
  return Enabled() && id == kDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

}  // namespace superman_returns::input
