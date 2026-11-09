#include "input/rawinput_kbm_driver.h"
#include "input/input_recorder.h"
#include "input/key_capture.h"
#include "input/gameinput_driver.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include <commctrl.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

// The mouse camera (MOUSE OPTIONS' slider). The SDK's mnk_sensitivity stops at
// 10, which the user found too slow.
// The user's mouse: the right button locks on a target (Left Trigger), the left
// one hits (X, Action) and, while locked on, uses the power (Right Trigger).
REXCVAR_DEFINE_BOOL(sr_lock_action_is_power, true, "Superman Returns",
                    "While Target Lock (left trigger) is held, Action (X) presses Superpower (right trigger)");
REXCVAR_DEFINE_DOUBLE(sr_mouse_sensitivity, 6.0, "Superman Returns",
                      "Mouse camera sensitivity: right stick units per mouse count / 200")
    .range(0.01, 1000.0);
REXCVAR_DEFINE_BOOL(sr_kbm_auto, true, "Input",
                    "Keyboard and mouse always on beside the pads: the input last used decides the cursor and "
                    "mouse look (off: mnk_mode and mnk_mouse decide)");
REXCVAR_DEFINE_BOOL(sr_shield_cursor, true, "Input", "The mouse cursor over the game is the S shield");
REXCVAR_DEFINE_BOOL(sr_hide_cursor_with_pad, true, "Input",
                    "Hide the mouse cursor over the game window while a controller is the input in use "
                    "(shown again on a key, click or mouse move)");
REXCVAR_DEFINE_STRING(sr_input_script, "", "Input",
                      "Automation: keys pressed inside the game without focus or system input, "
                      "\"seconds:Key[:ms];...\" from the driver start (Key as in keybind_*, "
                      "e.g. \"25:Return;29:Shift+Down;97:Up:1500\"; default hold 120 ms)");

namespace superman_returns::input {

using rex::input::DeviceId;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

namespace {

constexpr DeviceId kDevice = static_cast<DeviceId>(0x52494B4D);  // "RIKM"

// Bindings: the SDK's keybind_* syntax (mnk_input_driver.cpp), a comma
// separated list of alternatives, each with optional modifiers (held at least)
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
  // A key named without modifiers works whatever else is held (Shift bound to
  // a power, W still moves); one named with them needs at least those.
  if ((want & live) != want) return false;
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

// A key in both bindings' lists that is held now.
bool SharedKeyDown(const bool (&down)[256], const char* a, const char* b) {
  auto tokens = [](const std::string& binds) {
    std::vector<std::string> out;
    std::string_view rest(binds);
    while (!rest.empty()) {
      const size_t comma = rest.find(',');
      std::string_view t = rest.substr(0, comma);
      rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
      while (!t.empty() && t.front() == ' ') t.remove_prefix(1);
      while (!t.empty() && t.back() == ' ') t.remove_suffix(1);
      if (!t.empty()) out.emplace_back(t);
    }
    return out;
  };
  const auto ta = tokens(rex::cvar::Query<std::string>(a));
  const auto tb = tokens(rex::cvar::Query<std::string>(b));
  const uint8_t live = LiveModifiers(down);
  for (const auto& t : ta)
    if (std::find(tb.begin(), tb.end(), t) != tb.end() && TokenPressed(down, t, live)) return true;
  return false;
}

// sr_input_script: the keys (modifiers included) held at `elapsed_ms`.
// Returns whether any scripted key is held.
bool ScriptKeys(double elapsed_ms, bool (&down)[256]) {
  struct Step {
    double start_ms, end_ms;
    std::vector<uint8_t> keys;
  };
  static const std::vector<Step> steps = [] {
    std::vector<Step> out;
    std::string_view rest = rex::cvar::Query<std::string>("sr_input_script");
    static std::string text;  // keeps the cvar text alive for the views
    text = std::string(rest);
    rest = text;
    while (!rest.empty()) {
      const size_t semi = rest.find(';');
      std::string_view item = rest.substr(0, semi);
      rest = semi == std::string_view::npos ? std::string_view() : rest.substr(semi + 1);
      const size_t c1 = item.find(':');
      if (c1 == std::string_view::npos) continue;
      const size_t c2 = item.find(':', c1 + 1);
      Step s;
      s.start_ms = std::atof(std::string(item.substr(0, c1)).c_str()) * 1000.0;
      const double hold = c2 == std::string_view::npos ? 120.0 : std::atof(std::string(item.substr(c2 + 1)).c_str());
      s.end_ms = s.start_ms + hold;
      std::string_view combo = item.substr(c1 + 1, c2 == std::string_view::npos ? std::string_view::npos : c2 - c1 - 1);
      while (!combo.empty()) {
        const size_t plus = combo.find('+');
        const std::string_view name = combo.substr(0, plus);
        combo = plus == std::string_view::npos ? std::string_view() : combo.substr(plus + 1);
        const uint16_t vk = name == "Shift" ? VK_SHIFT : name == "Ctrl" ? VK_CONTROL : name == "Alt" ? VK_MENU
                                                  : uint16_t(rex::ui::ParseVirtualKey(name));
        if (vk && vk < 256) s.keys.push_back(uint8_t(vk));
      }
      if (!s.keys.empty()) out.push_back(std::move(s));
    }
    if (!out.empty()) REXLOG_INFO("Raw Input: input script with {} steps", out.size());
    return out;
  }();
  bool any = false;
  for (const Step& s : steps) {
    if (elapsed_ms < s.start_ms || elapsed_ms >= s.end_ms) continue;
    for (uint8_t vk : s.keys) down[vk] = true;
    any = true;
  }
  return any;
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
  } else if (msg == WM_TIMER) {
    if (auto* driver = reinterpret_cast<RawInputKbmDriver*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
      driver->OnCursorTimer();
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
  ApplyDefaultKeyBinds();  // the settings file is loaded by now
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
  SetTimer(hwnd, 1, 250, nullptr);  // the cursor follows connected pads (OnCursorTimer)
  REXLOG_INFO("Raw Input: keyboard and mouse ready");
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
}

namespace {
// The S shield cursor (tools/ui/make_cursor.py): 48x48 BGRA with alpha.
#include "input/cursor_s.inc"

HCURSOR ShieldCursor() {
  static const HCURSOR cursor = [] {
    BITMAPV5HEADER h{};
    h.bV5Size = sizeof(h);
    h.bV5Width = kCursorSize;
    h.bV5Height = -kCursorSize;  // top-down rows
    h.bV5Planes = 1;
    h.bV5BitCount = 32;
    h.bV5Compression = BI_BITFIELDS;
    h.bV5RedMask = 0x00FF0000;
    h.bV5GreenMask = 0x0000FF00;
    h.bV5BlueMask = 0x000000FF;
    h.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&h), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return HCURSOR(nullptr);
    std::memcpy(bits, kCursorBgra, sizeof(kCursorBgra));
    HBITMAP mask = CreateBitmap(kCursorSize, kCursorSize, 1, 1, nullptr);
    ICONINFO info{FALSE, kCursorHotX, kCursorHotY, mask, color};
    HCURSOR made = HCURSOR(CreateIconIndirect(&info));
    DeleteObject(color);
    DeleteObject(mask);
    return made;
  }();
  return cursor;
}

// The game window's messages before SDL's: in the client area, while the cursor
// shows, the shield instead of the arrow (SDL3 sets its own on WM_SETCURSOR).
LRESULT CALLBACK CursorSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
  if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
    auto* driver = reinterpret_cast<RawInputKbmDriver*>(data);
    if (driver->CursorShown()) {
      if (HCURSOR shield = ShieldCursor()) {
        SetCursor(shield);
        return TRUE;
      }
    }
  }
  return DefSubclassProc(hwnd, msg, wp, lp);
}
}  // namespace

void RawInputKbmDriver::OnWindowAvailable(rex::ui::Window* window) {
  window_ = window;
  game_hwnd_ = window ? window->GetNativeWindowHandle() : nullptr;
  SetGameWindow(game_hwnd_.load());
  if (window && REXCVAR_GET(sr_shield_cursor))
    window->app_context().CallInUIThreadDeferred([this] {
      if (HWND hwnd = HWND(game_hwnd_.load())) SetWindowSubclass(hwnd, CursorSubclass, 1, DWORD_PTR(this));
    });
}

bool RawInputKbmDriver::CursorShown() const { return !mouse_captured_ && !pad_hides_cursor_.load(); }

bool RawInputKbmDriver::Enabled() const {
  // sr_kbm_auto: keyboard and mouse always there, beside the pads (the input
  // last used drives the cursor and mouse look); else only with mnk_mode.
  return REXCVAR_GET(sr_kbm_auto) || rex::cvar::Query<bool>("mnk_mode");
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
      // Keys Mapping takes the press for itself (key_capture.h).
      if (down && !key_down_[k.VKey]) OfferCapturedKey(k.VKey);
      if (down && !key_down_[k.VKey] && ++key_presses_ <= 32)
        REXLOG_INFO("Raw Input: key down VK 0x{:02X} (#{})", k.VKey, key_presses_);
      key_down_[k.VKey] = down;
      if (down) last_kbm_use_ms_.store(NowMs(), std::memory_order_relaxed);
    }
  } else if (raw.header.dwType == RIM_TYPEMOUSE) {
    const RAWMOUSE& m = raw.data.mouse;
    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {
      mouse_dx_ += m.lLastX;
      mouse_dy_ += m.lLastY;
    }
    if (std::abs(m.lLastX) + std::abs(m.lLastY) > 2 || m.usButtonFlags != 0)
      last_kbm_use_ms_.store(NowMs(), std::memory_order_relaxed);
    const USHORT f = m.usButtonFlags;
    constexpr struct {
      USHORT down, up;
      int vk;
    } kMouseButtons[] = {{RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, VK_LBUTTON},
                         {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, VK_RBUTTON},
                         {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, VK_MBUTTON},
                         {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, VK_XBUTTON1},
                         {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, VK_XBUTTON2}};
    if ((f & RI_MOUSE_WHEEL) && MenuActive()) PressPad(SHORT(m.usButtonData) > 0 ? kPadUp : kPadDown);
    for (const auto& b : kMouseButtons) {
      if ((f & b.down) && !key_down_[b.vk] && !OfferCapturedKey(b.vk) && MenuActive()) {
        // The menus: left click through MAIN's mouse poll, right click is B.
        if (b.vk == VK_LBUTTON) NoteMenuClick();
        if (b.vk == VK_RBUTTON) PressPad(kPadB);
      }
      if (f & b.down) key_down_[b.vk] = true;
      if (f & b.up) key_down_[b.vk] = false;
    }
  }
}

void RawInputKbmDriver::UpdateMouseCapture(bool capture) {
  if (!window_ || capture == mouse_captured_) return;
  mouse_captured_ = capture;
  ApplyCursor();
  if (!capture) ClipCursor(nullptr);
}

void RawInputKbmDriver::ApplyCursor() {
  if (!window_) return;
  const bool hidden = mouse_captured_ || pad_hides_cursor_.load();
  window_->SetCursorVisibility(hidden ? rex::ui::Window::CursorVisibility::kHidden
                                      : rex::ui::Window::CursorVisibility::kVisible);
  // Shown again: the shield at once, not only after the next mouse move.
  if (!hidden && REXCVAR_GET(sr_shield_cursor)) {
    POINT at;
    HWND hwnd = HWND(game_hwnd_.load());
    if (hwnd && GetCursorPos(&at) && WindowFromPoint(at) == hwnd)
      if (HCURSOR shield = ShieldCursor()) SetCursor(shield);
  }
}

void RawInputKbmDriver::OnCursorTimer() {
  KeyCaptureTick(GameInForeground());
  // The input in use: a pad from its last button or stick, keyboard and mouse
  // from their last key, click or move (the game switches the same way).
  // Strictly later: at the start neither was used and the cursor shows.
  const bool pad_last = ConnectedPads() > 0 && LastPadUseMs() > last_kbm_use_ms_.load(std::memory_order_relaxed);
  SetPadIcons(pad_last);
  const bool hide = REXCVAR_GET(sr_hide_cursor_with_pad) && pad_last;
  if (window_ && pad_hides_cursor_.exchange(hide) != hide) {
    REXLOG_INFO("input: cursor {} (pads connected: {})", hide ? "hidden" : "shown", ConnectedPads());
    window_->app_context().CallInUIThreadDeferred([this] { ApplyCursor(); });
  }
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
  static const auto script_start = std::chrono::steady_clock::now();
  bool scripted[256] = {};
  const bool script_held = ScriptKeys(
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - script_start).count(), scripted);
  const bool foreground = GameInForeground();
  const bool live = (foreground || script_held) && is_active();
  // Mouse look only in the game itself (no menu open), and with sr_kbm_auto
  // only while keyboard and mouse are the input last used (a pad used after
  // them gives the camera back to the pad).
  const bool kbm_last = last_kbm_use_ms_.load(std::memory_order_relaxed) > LastPadUseMs();
  SetMouseLast(kbm_last);
  const bool mouse_look = live && !MenuActive() &&
                          (REXCVAR_GET(sr_kbm_auto) ? kbm_last : rex::cvar::Query<bool>("mnk_mouse"));

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
  if (!live) {
    RecordInputState(id, *out_state);
    return X_ERROR_SUCCESS;
  }

  bool k[256];
  for (int i = 0; i < 256; ++i) k[i] = (foreground && key_down_[i]) || scripted[i];
  if (KeyCaptureMutesKeyboard(key_down_)) std::memset(k, 0, sizeof(k));
  // In the menus the mouse buttons click (key_capture.h), whatever they are bound to.
  if (MenuActive()) k[VK_LBUTTON] = k[VK_RBUTTON] = k[VK_MBUTTON] = false;
  // In the menus Enter confirms (A) and Escape goes back (B), as on PC, whatever
  // they are bound to (Enter is also Start); in the game Escape opens the menu
  // (Start). A key still held across the switch (Enter or Escape opening the
  // pause menu, Escape closing it) counts only after it is let go.
  bool menu_enter = false, menu_escape = false, game_escape = false;
  uint16_t menu_arrows = 0;
  {
    static bool enter_held = false, escape_held = false, was_menu = false;
    const bool menu = MenuActive();
    if (menu != was_menu) {
      enter_held = k[VK_RETURN];
      escape_held = k[VK_ESCAPE];
      was_menu = menu;
    }
    if (!k[VK_RETURN]) enter_held = false;
    if (!k[VK_ESCAPE]) escape_held = false;
    if (menu) {
      menu_enter = k[VK_RETURN] && !enter_held;
      menu_escape = k[VK_ESCAPE] && !escape_held;
      k[VK_RETURN] = false;
      // The arrows move through the menus (the d-pad), not WASD (the left stick).
      menu_arrows = (k[VK_UP] ? rex::input::X_INPUT_GAMEPAD_DPAD_UP : 0) |
                    (k[VK_DOWN] ? rex::input::X_INPUT_GAMEPAD_DPAD_DOWN : 0) |
                    (k[VK_LEFT] ? rex::input::X_INPUT_GAMEPAD_DPAD_LEFT : 0) |
                    (k[VK_RIGHT] ? rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT : 0);
      k[VK_UP] = k[VK_DOWN] = k[VK_LEFT] = k[VK_RIGHT] = false;
      // X is the pad's Y (Keys Mapping: back to the default keys).
      if (k['X']) menu_arrows |= rex::input::X_INPUT_GAMEPAD_Y;
      k['X'] = false;
    } else {
      game_escape = k[VK_ESCAPE] && !escape_held;
    }
    k[VK_ESCAPE] = false;
  }
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
  uint16_t buttons = MenuPadButtons();
  for (const auto& b : kBinds)
    if (Pressed(k, b.cvar)) buttons |= b.button;
  if (menu_enter) buttons |= rex::input::X_INPUT_GAMEPAD_A;
  if (menu_escape) buttons |= rex::input::X_INPUT_GAMEPAD_B;
  if (game_escape) buttons |= rex::input::X_INPUT_GAMEPAD_START;
  buttons |= menu_arrows;
  if (buttons && buttons != last_buttons_ && ++button_logs_ <= 32)
    REXLOG_INFO("Raw Input: pad buttons 0x{:04X} sent to the game", buttons);
  last_buttons_ = buttons;

  auto axis = [&k](const char* neg, const char* pos) {
    return (Pressed(k, pos) ? 32767 : 0) - (Pressed(k, neg) ? 32767 : 0);
  };
  // In the menus the movement keys (WASD) do not move the selection: the arrows do.
  const bool menu_open = MenuActive();
  int32_t lx = menu_open ? 0 : axis("keybind_lstick_left", "keybind_lstick_right");
  int32_t ly = menu_open ? 0 : axis("keybind_lstick_down", "keybind_lstick_up");
  int32_t rx = axis("keybind_rstick_left", "keybind_rstick_right");
  int32_t ry = axis("keybind_rstick_down", "keybind_rstick_up");
  static double carry_x = 0, carry_y = 0;  // see below
  if (!mouse_look) carry_x = carry_y = 0;
  if (mouse_look) {
    // Same scale as the SDK's mnk driver: 200 stick units per count. What a
    // fast move gives beyond a full stick is carried to the next polls (up to
    // four full sticks), so a quick turn is not cut short by the stick's limit.
    const double s = REXCVAR_GET(sr_mouse_sensitivity) * 200.0;
    auto take = [](double want, double& carry, int32_t& stick) {
      want += carry + stick;
      const double out = std::clamp(want, -32767.0, 32767.0);
      carry = std::clamp(want - out, -4 * 32767.0, 4 * 32767.0);
      stick = int32_t(out);
    };
    take(dx * s, carry_x, rx);
    take(-dy * s, carry_y, ry);
  }
  if (mouse_look && (dx || dy) && ++mouse_logs_ <= 8)
    REXLOG_INFO("Raw Input: mouse {} {} -> right stick {} {}", dx, dy, rx, ry);
  auto clamp16 = [](int32_t v) { return int16_t(std::clamp(v, -32768, 32767)); };
  const bool lock_on = Pressed(k, "keybind_left_trigger");
  bool power = Pressed(k, "keybind_right_trigger");
  if (REXCVAR_GET(sr_lock_action_is_power) && lock_on && !menu_open && (buttons & rex::input::X_INPUT_GAMEPAD_X)) {
    buttons &= ~rex::input::X_INPUT_GAMEPAD_X;
    power = true;
  }
  // A key bound to both Action and Superpower (key_capture.cpp lets them share)
  // is Action alone while no target is locked.
  if (!lock_on && power && (buttons & rex::input::X_INPUT_GAMEPAD_X) &&
      SharedKeyDown(k, "keybind_x", "keybind_right_trigger"))
    power = false;
  out_state->gamepad.buttons = buttons;
  out_state->gamepad.left_trigger = lock_on ? 0xFF : 0;
  out_state->gamepad.right_trigger = power ? 0xFF : 0;
  out_state->gamepad.thumb_lx = clamp16(lx);
  out_state->gamepad.thumb_ly = clamp16(ly);
  out_state->gamepad.thumb_rx = clamp16(rx);
  out_state->gamepad.thumb_ry = clamp16(ry);
  RecordInputState(id, *out_state);
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
