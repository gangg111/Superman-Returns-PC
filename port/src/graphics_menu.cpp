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

namespace {

// The rows, in build_ui_mod.py's ROWS order: the cvar, and the value each
// choice stands for (Off/On rows: false/true).
struct Row {
  const char* cvar;
  std::vector<int> values;  // empty: a bool
};
const std::array<Row, 5> kRows = {{
    {"sr_rt_shadows", {}},
    {"sr_rt_shadow_rays", {1, 4, 8, 16, 32}},
    {"sr_rt_characters", {}},
    {"sr_rt_reflections", {}},
    // Off, Performance, Balanced, Quality, DLAA (sr_dlss's own numbering differs).
    {"sr_dlss", {0, 4, 3, 2, 1}},
}};

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

int RowIndex(const Row& row) {
  const std::string value = rex::cvar::GetFlagByName(row.cvar);
  if (row.values.empty()) return value == "true" ? 1 : 0;
  const int current = std::atoi(value.c_str());
  // The choice with this value, else (a value set by hand) the nearest one
  // at or below it, the choices being ascending.
  int index = 0;
  for (size_t i = 0; i < row.values.size(); ++i) {
    if (row.values[i] == current) return int(i);
    if (row.values[i] <= current) index = int(i);
  }
  return index;
}

std::string RowValue(const Row& row, int index) {
  if (row.values.empty()) return index ? "true" : "false";
  index = std::clamp(index, 0, int(row.values.size()) - 1);
  return std::to_string(row.values[index]);
}

std::filesystem::path ConfigPath() {
  wchar_t module[MAX_PATH];
  GetModuleFileNameW(nullptr, module, MAX_PATH);
  std::filesystem::path exe(module);
  return exe.parent_path() / (exe.stem().wstring() + L".toml");
}

// Sets `key = value` in the config file, keeping its other lines (comments
// included); a key not there yet is added at the end.
void SaveSetting(const std::string& key, const std::string& value) {
  static std::mutex lock;
  std::lock_guard guard(lock);
  const auto path = ConfigPath();
  std::vector<std::string> lines;
  {
    std::ifstream in(path, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
  }
  bool found = false;
  for (auto& line : lines) {
    const size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos || line.compare(start, key.size(), key) != 0) continue;
    const size_t after = line.find_first_not_of(" \t", start + key.size());
    if (after == std::string::npos || line[after] != '=') continue;
    line = key + " = " + value;
    found = true;
  }
  if (!found) lines.push_back(key + " = " + value);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  for (const auto& line : lines) out << line << "\r\n";
  if (!out) REXLOG_WARN("graphics menu: could not write {}", path.string());
}

extern "C" void GetGraphicsOption(PPCContext& ctx, uint8_t* base) {
  const int32_t id = ArgInt(ctx, base, 0);
  const int index = id >= 0 && id < int32_t(kRows.size()) ? RowIndex(kRows[id]) : 0;
  ctx.r3.u64 = uint32_t(index);
  sub_82698550(ctx, base);
}

extern "C" void SetGraphicsOption(PPCContext& ctx, uint8_t* base) {
  const int32_t id = ArgInt(ctx, base, 0);
  const int32_t index = ArgInt(ctx, base, 1);
  if (id >= 0 && id < int32_t(kRows.size())) {
    const Row& row = kRows[id];
    const std::string value = RowValue(row, index);
    if (rex::cvar::SetFlagByName(row.cvar, value)) {
      SaveSetting(row.cvar, value);
      REXLOG_INFO("graphics menu: {} = {}", row.cvar, value);
    } else {
      REXLOG_WARN("graphics menu: could not set {} = {}", row.cvar, value);
    }
  }
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
