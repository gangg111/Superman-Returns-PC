// Graphics Options: the game's own menu screen for the port's settings.
//
// The screen is an Apt movie like the game's other options screens, built by
// tools/ui/build_ui_mod.py into DATA/ui_sr.AST (with its texts in
// DATA/text_sr.AST) next to the game's own archives, which stay untouched:
// Control Room gets a fourth icon, Graphics Options, opening the screen
// SCRGRAPHICSOPT (Gameplay Options' movie with our rows).
//
// Two hooks make the game use it:
//   BigFile::Open (sub_826E83D0): the archive's name sits at this+8 without
//     its extension (".AST" is added here); when <name>_sr.AST exists, the
//     name is changed to it before the original opens it.
//   SettingsAdapter's setup (sub_822AF3E8, r3 the adapter): it binds the
//     natives Apt calls (GetMusicVol..StopRumble) through sub_826C7D00 (a
//     native function object from a guest code address) and sub_826A29A0
//     (object, name, function). After it, two more are bound the same way, on
//     guest code addresses the dispatcher hands out for host functions:
//       GetGraphicsOption(id) -> index of the row's current choice
//       SetGraphicsOption(id, index)
//     A native reads argument i with sub_826C3328(i) and sub_826815A0 (to
//     integer), returns an integer through sub_82698550 and nothing through
//     sub_82686910, as the game's own (GetYInversion, SetYInversion) do.
//
// A change applies at once (the renderer reads these cvars every frame) and is
// written to superman_returns.toml next to the executable, the file the
// runtime loads its settings from, so it holds on the next start.

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>

#include "generated/default/superman_returns_init.h"
#include "input/key_capture.h"
#include "launcher_settings.h"

REXCVAR_DEFINE_BOOL(sr_menu_mouse_log, true, "Superman Returns",
                    "Log each menu click: the cursor and the item it hit (calibration)");

namespace {

// The rows, as build_ui_mod.py's GRAPHICS_ROWS (ids 0..) and DISPLAY_ROWS (ids
// 100..): the cvars a row sets and each choice's value. A value "W x H" sets the
// first cvar to W and the second to H; otherwise every cvar of the row gets it.
struct Row {
  int id;
  std::vector<const char*> cvars;
  std::vector<std::string> values;
};
const std::vector<std::string> kOffOn = {"false", "true"};
const std::vector<Row> kRows = {
    {0, {"sr_rt_shadows"}, kOffOn},
    {1, {"sr_rt_shadow_rays"}, {"1", "4", "8", "16", "32"}},
    {2, {"sr_rt_characters"}, kOffOn},
    {3, {"sr_rt_reflections"}, kOffOn},
    // Off, Performance, Balanced, Quality, DLAA (sr_dlss's own numbering differs).
    {4, {"sr_dlss"}, {"0", "4", "3", "2", "1"}},
    // Game (the game's own sun), Midnight, Dawn, Morning, Noon, Afternoon,
    // Sunset (a low golden sun), Night.
    {5, {"sr_time_of_day"}, {"-1", "0", "6", "9", "12", "15", "17", "21"}},
    // Off, or real minutes per 24-hour day (10 min .. 6 h).
    {6, {"sr_daynight_cycle_minutes"}, {"0", "10", "30", "60", "120", "240", "360"}},
    // The launcher's options (launcher_settings.cpp), read by the renderer at start.
    {7, {"sr_native_anisotropic_filtering"}, {"-1", "2", "4", "8", "16"}},
    {8, {"sr_native_fxaa"}, kOffOn},
    {9, {"sr_native_shadow_quality"}, {"1", "2", "4"}},
    {10, {"sr_native_msaa_samples"}, {"1", "4", "8"}},
    {100, {"window_width", "window_height"},
     {"960x540", "1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160"}},
    {101, {"fullscreen"}, {"true", "false"}},
    {102, {"vsync"}, kOffOn},
    {103, {"sr_native_fps_limit"}, {"30", "60", "120", "0"}},
    {104, {"sr_native_render_scale"}, {"0", "1", "1.25", "1.5", "2", "3", "4"}},
    // MOUSE OPTIONS' slider (0..10): the mouse camera, read live by the Raw
    // Input driver. From 6x: the user found 6x (the old top) the slowest usable.
    {105, {"sr_mouse_sensitivity"}, {"6", "8", "10", "12", "15", "18", "22", "26", "30", "40", "50"}},
};

const Row* FindRow(int32_t id) {
  for (const Row& row : kRows)
    if (row.id == id) return &row;
  return nullptr;
}

std::string GuestString(uint8_t* base, uint32_t address, size_t max) {
  const char* text = reinterpret_cast<const char*>(base + address);
  return std::string(text, strnlen(text, max));
}

int32_t ArgInt(PPCContext& ctx, uint8_t* base, uint32_t index) {
  ctx.r3.u64 = index;
  sub_826C3328(ctx, base);
  sub_826815A0(ctx, base);
  return ctx.r3.s32;
}

bool SameValue(const std::string& a, const std::string& b) {
  if (a == b) return true;
  char *ea = nullptr, *eb = nullptr;
  const double x = std::strtod(a.c_str(), &ea), y = std::strtod(b.c_str(), &eb);
  return ea != a.c_str() && !*ea && eb != b.c_str() && !*eb && std::abs(x - y) < 1e-6;
}

// The value a row's cvars hold now, in the row's own form.
std::string RowCurrent(const Row& row) {
  std::string value = rex::cvar::GetFlagByName(row.cvars[0]);
  if (row.cvars.size() == 2 && row.values[0].find('x') != std::string::npos)
    value += "x" + rex::cvar::GetFlagByName(row.cvars[1]);
  return value;
}

int RowIndex(const Row& row) {
  const std::string current = RowCurrent(row);
  for (size_t i = 0; i < row.values.size(); ++i)
    if (SameValue(row.values[i], current)) return int(i);
  // A value set by hand: the nearest numeric choice below it, else the first.
  const double v = std::strtod(current.c_str(), nullptr);
  int index = 0;
  double best = -1e300;
  for (size_t i = 0; i < row.values.size(); ++i) {
    const double c = std::strtod(row.values[i].c_str(), nullptr);
    if (c <= v && c > best) best = c, index = int(i);
  }
  return index;
}

std::filesystem::path ConfigPath() {
  wchar_t module[MAX_PATH];
  GetModuleFileNameW(nullptr, module, MAX_PATH);
  std::filesystem::path exe(module);
  return exe.parent_path() / (exe.stem().wstring() + L".toml");
}

extern "C" void GetGraphicsOption(PPCContext& ctx, uint8_t* base) {
  const Row* row = FindRow(ArgInt(ctx, base, 0));
  ctx.r3.u64 = uint32_t(row ? RowIndex(*row) : 0);
  sub_82698550(ctx, base);
}

void SetRow(int32_t id, int32_t index) {
  if (const Row* row = FindRow(id)) {
    const std::string value = row->values[std::clamp(index, 0, int(row->values.size()) - 1)];
    const size_t x = value.find('x');
    for (size_t k = 0; k < row->cvars.size(); ++k) {
      const std::string part = x == std::string::npos ? value : k == 0 ? value.substr(0, x) : value.substr(x + 1);
      if (rex::cvar::SetFlagByName(row->cvars[k], part)) {
        if (!superman_returns::SaveConfigSetting(ConfigPath(), row->cvars[k], part))
          REXLOG_WARN("graphics menu: could not write {}", ConfigPath().string());
        REXLOG_INFO("graphics menu: {} = {}", row->cvars[k], part);
      } else {
        REXLOG_WARN("graphics menu: could not set {} = {}", row->cvars[k], part);
      }
    }
  }
}

extern "C" void SetGraphicsOption(PPCContext& ctx, uint8_t* base) {
  const int32_t id = ArgInt(ctx, base, 0);
  SetRow(id, ArgInt(ctx, base, 1));
  sub_82686910(ctx, base);
}

// MOUSE OPTIONS' slider (the Audio screen's, its natives renamed):
// GetMouseSens() -> 0..10, SetMouseSens(v).
extern "C" void GetMouseSensNative(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = uint32_t(RowIndex(*FindRow(105)));
  sub_82698550(ctx, base);
}
extern "C" void SetMouseSensNative(PPCContext& ctx, uint8_t* base) {
  SetRow(105, ArgInt(ctx, base, 0));
  sub_82686910(ctx, base);
}

// Keys Mapping (input/key_capture.h): GetKeyBind(i) -> the binding's first key
// (code | 0x100 Shift | 0x200 Ctrl | 0x400 Alt), BeginKeyCapture(i),
// KeyCaptureActive() -> 1 while waiting for a key, CancelKeyCapture().
extern "C" void GetKeyBindNative(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = uint32_t(superman_returns::input::KeyBindCode(ArgInt(ctx, base, 0)));
  sub_82698550(ctx, base);
}
extern "C" void BeginKeyCaptureNative(PPCContext& ctx, uint8_t* base) {
  superman_returns::input::BeginKeyCapture(ArgInt(ctx, base, 0));
  sub_82686910(ctx, base);
}
extern "C" void KeyCaptureActiveNative(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = superman_returns::input::KeyCaptureActive() ? 1 : 0;
  sub_82698550(ctx, base);
}
// Clicking the menus (input/key_capture.h): GetMouseX/GetMouseY() -> the cursor
// in the 1280x720 stage or -1, TakeMenuClick() -> 1 once a left click,
// PressPad(1 A, 2 RIGHT) a short press, MenuMouseLog(x, y, hx, hy, w, h) the
// hit test (sr_menu_mouse_log).
extern "C" void GetMouseXNative(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = uint32_t(superman_returns::input::MenuMouseX());
  sub_82698550(ctx, base);
}
extern "C" void GetMouseYNative(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = uint32_t(superman_returns::input::MenuMouseY());
  sub_82698550(ctx, base);
}
extern "C" void TakeMenuClickNative(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = superman_returns::input::TakeMenuClick() ? 1 : 0;
  sub_82698550(ctx, base);
}
extern "C" void PressPadNative(PPCContext& ctx, uint8_t* base) {
  const int32_t what = ArgInt(ctx, base, 0);
  using namespace superman_returns::input;
  PressPad(what == 2 ? kPadRight : what == 3 ? kPadLeft : kPadA);
  sub_82686910(ctx, base);
}
extern "C" void MenuMouseLogNative(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(sr_menu_mouse_log))
    REXLOG_INFO("menu mouse: click at {} {}, item at {} {} size {} x {}", ArgInt(ctx, base, 0), ArgInt(ctx, base, 1),
                ArgInt(ctx, base, 2), ArgInt(ctx, base, 3), ArgInt(ctx, base, 4), ArgInt(ctx, base, 5));
  sub_82686910(ctx, base);
}

// KbmActive() -> 1 when the menus show the keys' icons (no pad used last).
// MenuHintKey(i) -> the key a hint shows for binding i (input/key_capture.h).
extern "C" void MenuHintKeyNative(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = uint32_t(superman_returns::input::MenuHintKey(ArgInt(ctx, base, 0)));
  sub_82698550(ctx, base);
}
extern "C" void KbmActiveNative(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = superman_returns::input::PadIcons() ? 0 : 1;
  sub_82698550(ctx, base);
}

// SetMenuActive(0/1): MAIN's poll of FlowMgr (a menu open or the game).
extern "C" void SetMenuActiveNative(PPCContext& ctx, uint8_t* base) {
  superman_returns::input::SetMenuActive(ArgInt(ctx, base, 0) != 0);
  sub_82686910(ctx, base);
}
extern "C" void ResetKeyBindsNative(PPCContext& ctx, uint8_t* base) {
  superman_returns::input::ResetKeyBinds();
  sub_82686910(ctx, base);
}
extern "C" void CancelKeyCaptureNative(PPCContext& ctx, uint8_t* base) {
  superman_returns::input::CancelKeyCapture();
  sub_82686910(ctx, base);
}

// RestartGame(): the options' restart popup, "Restart now".
extern "C" void RestartGameNative(PPCContext& ctx, uint8_t* base) {
  REXLOG_INFO("graphics menu: restarting the game");
  if (!superman_returns::RestartGame()) REXLOG_WARN("graphics menu: could not start the game again");
  sub_82686910(ctx, base);
}

uint32_t GuestCString(const char* text) {
  const uint32_t size = uint32_t(std::strlen(text) + 1);
  const uint32_t at = REX_KERNEL_MEMORY()->SystemHeapAlloc(size, 4);
  std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual<char*>(at), text, size);
  return at;
}

}  // namespace

// BigFile::Open: r3 the BigFile, its name at +8.
REX_HOOK_RAW(sub_826E83D0) {
  const uint32_t name_at = ctx.r3.u32 + 8;
  const std::string name = GuestString(base, name_at, 256);
  const std::string ours = name + "_sr";
  // Only names on a device (d:\DATA\ui); the game also opens bare names that
  // the file system cannot resolve (and warns about).
  if (name.find(':') != std::string::npos && REX_KERNEL_FS()->ResolvePath(ours + ".AST")) {
    std::memcpy(base + name_at, ours.c_str(), ours.size() + 1);
    REXLOG_INFO("graphics menu: {}.AST opened as {}.AST", name, ours);
  } else {
    REXLOG_DEBUG("graphics menu: {}.AST (no {}.AST)", name, ours);
  }
  __imp__sub_826E83D0(ctx, base);
}

// SettingsAdapter's setup: r3 the adapter.
REX_HOOK_RAW(sub_822AF3E8) {
  const uint32_t adapter = ctx.r3.u32;
  __imp__sub_822AF3E8(ctx, base);
  auto* dispatcher = REX_KERNEL_STATE()->function_dispatcher();
  const std::pair<const char*, PPCFunc*> natives[] = {
      {"GetGraphicsOption", &GetGraphicsOption},
      {"SetGraphicsOption", &SetGraphicsOption},
      {"RestartGame", &RestartGameNative},
      {"GetKeyBind", &GetKeyBindNative},
      {"BeginKeyCapture", &BeginKeyCaptureNative},
      {"KeyCaptureActive", &KeyCaptureActiveNative},
      {"CancelKeyCapture", &CancelKeyCaptureNative},
      {"ResetKeyBinds", &ResetKeyBindsNative},
      {"SetMenuActive", &SetMenuActiveNative},
      {"GetMouseX", &GetMouseXNative},
      {"GetMouseY", &GetMouseYNative},
      {"TakeMenuClick", &TakeMenuClickNative},
      {"PressPad", &PressPadNative},
      {"MenuMouseLog", &MenuMouseLogNative},
      {"KbmActive", &KbmActiveNative},
      {"MenuHintKey", &MenuHintKeyNative},
      {"GetMouseSens", &GetMouseSensNative},
      {"SetMouseSens", &SetMouseSensNative},
  };
  for (const auto& [name, func] : natives) {
    const uint32_t code = dispatcher->AllocateThunk(func, 0);
    if (!code) {
      REXLOG_ERROR("graphics menu: no guest address for {}", name);
      continue;
    }
    ctx.r3.u64 = code;
    sub_826C7D00(ctx, base);
    const uint32_t function = ctx.r3.u32;
    ctx.r3.u64 = adapter;
    ctx.r4.u64 = GuestCString(name);
    ctx.r5.u64 = function;
    sub_826A29A0(ctx, base);
    REXLOG_INFO("graphics menu: SettingsAdapter.{} bound at {:08X}", name, code);
  }
}
