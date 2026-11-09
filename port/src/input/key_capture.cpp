#include "input/key_capture.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

#include "launcher_settings.h"

namespace superman_returns::input {

const char* const kKeyBindCvars[kKeyBindCount] = {
    // actions
    "keybind_a", "keybind_b", "keybind_x", "keybind_y", "keybind_left_shoulder", "keybind_right_shoulder",
    "keybind_left_trigger", "keybind_right_trigger", "keybind_start", "keybind_back",
    // movement
    "keybind_lstick_up", "keybind_lstick_down", "keybind_lstick_left", "keybind_lstick_right",
    "keybind_lstick_press", "keybind_dpad_up", "keybind_dpad_down", "keybind_dpad_left", "keybind_dpad_right",
    // camera
    "keybind_rstick_up", "keybind_rstick_down", "keybind_rstick_left", "keybind_rstick_right",
    "keybind_rstick_press",
};

// The defaults, in kKeyBindCvars' order: the user's own set (superman_returns.toml,
// 2026-11-09), keys and mouse buttons by the SDK's names; '' is unbound.
const char* const kDefaultKeyBinds[kKeyBindCount] = {
    // actions: A B X Y LB RB LT RT Start Back
    "F", "Control", "LMB", "Space", "", "Shift", "RMB", "LMB", "Escape", "M",
    // movement: left stick up down left right press, d-pad up down left right
    "W", "S", "A", "D", "", "1", "", "2", "3",
    // camera: right stick up down left right press
    "", "", "Left", "Right", "",
};

namespace {

struct KeyName {
  int code;
  const char* name;
};
// The SDK's names, one a code (tools/ui/key_names.py).
constexpr KeyName kKeyNames[] = {
#include "input/key_names.inc"
};

const char* NameOf(int vk) {
  for (const auto& k : kKeyNames)
    if (k.code == vk) return k.name;
  return nullptr;
}

std::atomic<int> g_target{-1};         // binding being captured, -1 none
std::atomic<int> g_held{0};            // the captured key, muted until let go
std::atomic<int64_t> g_started_ms{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::filesystem::path ConfigPath() {
  wchar_t module[MAX_PATH];
  GetModuleFileNameW(nullptr, module, MAX_PATH);
  std::filesystem::path exe(module);
  return exe.parent_path() / (exe.stem().wstring() + L".toml");
}

std::vector<std::string> Tokens(const std::string& binds) {
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
}

void Set(const char* cvar, const std::string& value) {
  rex::cvar::SetFlagByName(cvar, value);
  // A TOML literal string: key names hold no quotes.
  if (!superman_returns::SaveConfigSetting(ConfigPath(), cvar, "'" + value + "'"))
    REXLOG_WARN("keys: could not write {}", ConfigPath().string());
}

}  // namespace

void ApplyDefaultKeyBinds() {
  std::string text;
  if (FILE* f = _wfopen(ConfigPath().c_str(), L"rb")) {
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
  }
  int applied = 0;
  for (int i = 0; i < kKeyBindCount; ++i) {
    // a line "keybind_x = ..." in the settings file wins
    const std::string key = std::string("\n") + kKeyBindCvars[i] + " =";
    if (("\n" + text).find(key) != std::string::npos) continue;
    rex::cvar::SetFlagByName(kKeyBindCvars[i], kDefaultKeyBinds[i]);
    ++applied;
  }
  REXLOG_INFO("keys: {} default bindings applied (the rest from {})", applied, ConfigPath().string());
}

void ResetKeyBinds() {
  CancelKeyCapture();
  for (int i = 0; i < kKeyBindCount; ++i) Set(kKeyBindCvars[i], kDefaultKeyBinds[i]);
  REXLOG_INFO("keys: all bindings back to the defaults");
}

void BeginKeyCapture(int index) {
  if (index < 0 || index >= kKeyBindCount) return;
  g_started_ms.store(NowMs());
  g_target.store(index);
  REXLOG_INFO("keys: press a key for {}", kKeyBindCvars[index]);
}

bool KeyCaptureActive() { return g_target.load() >= 0; }

void CancelKeyCapture() {
  if (g_target.exchange(-1) >= 0) REXLOG_INFO("keys: capture cancelled");
}

int KeyBindCode(int index) {
  if (index < 0 || index >= kKeyBindCount) return 0;
  const auto tokens = Tokens(rex::cvar::GetFlagByName(kKeyBindCvars[index]));
  if (tokens.empty()) return 0;
  std::string_view t = tokens[0];
  int mods = 0;
  for (size_t plus; (plus = t.find('+')) != std::string_view::npos && plus > 0;) {
    const std::string_view head = t.substr(0, plus);
    const int bit = head == "Shift" ? 0x100 : head == "Ctrl" || head == "Control" ? 0x200 : head == "Alt" ? 0x400 : 0;
    if (!bit) break;
    mods |= bit;
    t.remove_prefix(plus + 1);
  }
  const int vk = int(rex::ui::ParseVirtualKey(t));
  return vk > 0 && vk < 256 ? vk | mods : 0;
}

int MenuHintKey(int index) {
  if (MenuActive() && index == 0) return VK_RETURN;
  if (MenuActive() && index == 1) return VK_ESCAPE;
  if (MenuActive() && index == 3) return 'X';
  // the d-pad and the left stick (bindings 15..18, 10..13): the arrows in the menus
  constexpr int kArrows[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT};
  if (MenuActive() && index >= 15 && index <= 18) return kArrows[index - 15];
  if (MenuActive() && index >= 10 && index <= 13) return kArrows[index - 10];
  return KeyBindCode(index) & 0xFF;
}

bool OfferCapturedKey(int vk) {
  const int index = g_target.load();
  if (index < 0) return false;
  // Shift, Ctrl and Alt are keys of their own too (the left and right ones as
  // the generic key the bindings name).
  if (vk == VK_LSHIFT || vk == VK_RSHIFT) vk = VK_SHIFT;
  if (vk == VK_LCONTROL || vk == VK_RCONTROL) vk = VK_CONTROL;
  if (vk == VK_LMENU || vk == VK_RMENU) vk = VK_MENU;
  const char* name = NameOf(vk);
  REXLOG_INFO("keys: offered VK 0x{:02X} ({})", vk, name ? name : "no name, still waiting");
  if (!name) return true;  // no name the bindings know: keep waiting
  if (g_target.exchange(-1) != index) return false;
  g_held.store(vk);
  // The key leaves every other binding (two pad buttons on one key would
  // press both), then becomes this one's only key.
  // Action (X, 2) and Superpower (right trigger, 7) may share a key: the Raw
  // Input driver presses Action with it, Superpower while Target Lock is held.
  auto partners = [](int a, int b) { return (a == 2 && b == 7) || (a == 7 && b == 2); };
  for (int i = 0; i < kKeyBindCount; ++i) {
    if (i == index || partners(i, index)) continue;
    const auto tokens = Tokens(rex::cvar::GetFlagByName(kKeyBindCvars[i]));
    std::string kept;
    bool removed = false;
    for (const auto& t : tokens) {
      if (t == name) {
        removed = true;
        continue;
      }
      kept += (kept.empty() ? "" : ",") + t;
    }
    if (removed) Set(kKeyBindCvars[i], kept);
  }
  // Shared with its partner: the partner keeps the key, this binding gets it too.
  Set(kKeyBindCvars[index], name);
  REXLOG_INFO("keys: {} = {}", kKeyBindCvars[index], name);
  return true;
}

namespace {
std::atomic<bool> g_menu{true};  // until the script says otherwise: boot videos and the front end
}
void SetMenuActive(bool active) {
  if (g_menu.exchange(active) != active) REXLOG_INFO("input: {}", active ? "menu open (mouse moves the cursor)" : "in game (mouse looks around)");
}
bool MenuActive() { return g_menu.load(); }

namespace {
std::atomic<void*> g_window{nullptr};
std::atomic<bool> g_click{false};
std::atomic<bool> g_mouse_last{false};
std::atomic<uint32_t> g_pad{0};  // buttons | polls left << 16
}

void SetGameWindow(void* hwnd) { g_window.store(hwnd); }
void SetMouseLast(bool last) { g_mouse_last.store(last); }
namespace {
std::atomic<bool> g_pad_icons{false};
}
void SetPadIcons(bool pad) {
  if (g_pad_icons.exchange(pad) != pad) REXLOG_INFO("input: button icons {}", pad ? "of the pad" : "of the keys");
}
bool PadIcons() { return g_pad_icons.load(); }

// The cursor in the stage: the 1280x720 menus drawn to fit the window's client
// area (letterboxed when its aspect differs).
static bool StagePoint(int& x, int& y) {
  HWND hwnd = HWND(g_window.load());
  if (!hwnd || !MenuActive() || !g_mouse_last.load() || GetForegroundWindow() != hwnd) return false;
  POINT p;
  RECT r;
  if (!GetCursorPos(&p) || !ScreenToClient(hwnd, &p) || !GetClientRect(hwnd, &r)) return false;
  const double cw = r.right - r.left, ch = r.bottom - r.top;
  if (cw <= 0 || ch <= 0) return false;
  const double scale = std::min(cw / 1280.0, ch / 720.0);
  const double ox = (cw - 1280.0 * scale) / 2, oy = (ch - 720.0 * scale) / 2;
  x = int((p.x - ox) / scale);
  y = int((p.y - oy) / scale);
  return x >= 0 && y >= 0 && x < 1280 && y < 720;
}

int MenuMouseX() {
  int x, y;
  return StagePoint(x, y) ? x : -1;
}
int MenuMouseY() {
  int x, y;
  return StagePoint(x, y) ? y : -1;
}
void NoteMenuClick() {
  if (MenuActive()) g_click.store(true);
}
bool TakeMenuClick() { return g_click.exchange(false); }
void PressPad(uint16_t buttons) { g_pad.store(buttons | (6u << 16)); }
uint16_t MenuPadButtons() {
  uint32_t v = g_pad.load();
  while (v >> 16) {
    // pressed for the first 3 polls, released for the last 3
    const uint32_t left = (v >> 16) - 1;
    if (g_pad.compare_exchange_weak(v, (v & 0xFFFF) | (left << 16))) return left >= 3 ? uint16_t(v) : 0;
  }
  return 0;
}

void KeyCaptureTick(bool game_in_foreground) {
  if (g_target.load() < 0) return;
  if (!game_in_foreground || NowMs() - g_started_ms.load() > 6000) CancelKeyCapture();
}

bool KeyCaptureMutesKeyboard(const bool (&down)[256]) {
  if (g_target.load() >= 0) return true;
  const int held = g_held.load();
  if (!held) return false;
  if (held > 0 && held < 256 && down[held]) return true;
  g_held.store(0);
  return false;
}

}  // namespace superman_returns::input
