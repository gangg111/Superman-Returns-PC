// What the launcher (launcher/LauncherSettings.cs) did for the game, now done by
// the game itself: the game data folder next to the executable, and the
// launcher's settings carried over into superman_returns.toml once.

#include "launcher_settings.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(sr_restart_wait_pid, 0, "Superman Returns",
                     "Set by the options menu's restart: wait for this process to exit before starting");

namespace superman_returns {

namespace {

std::filesystem::path ExeFolder() {
  wchar_t module[MAX_PATH];
  GetModuleFileNameW(nullptr, module, MAX_PATH);
  return std::filesystem::path(module).parent_path();
}

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
  std::vector<std::string> lines;
  std::ifstream in(path, std::ios::binary);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
  }
  return lines;
}

std::string Trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// The keys a config line sets (`key = value`, comments and tables skipped).
std::set<std::string> ConfigKeys(const std::vector<std::string>& lines) {
  std::set<std::string> keys;
  for (const auto& line : lines) {
    const std::string t = Trim(line);
    if (t.empty() || t[0] == '#' || t[0] == '[') continue;
    const size_t eq = t.find('=');
    if (eq != std::string::npos) keys.insert(Trim(t.substr(0, eq)));
  }
  return keys;
}

int Choice(const std::string& value, int fallback, std::initializer_list<int> choices) {
  char* end = nullptr;
  const long v = std::strtol(value.c_str(), &end, 10);
  if (end == value.c_str() || *end) return fallback;
  return std::find(choices.begin(), choices.end(), int(v)) != choices.end() ? int(v) : fallback;
}

// The launcher's options (BuildArgumentList, Direct3D 12 and the native renderer)
// as config lines.
std::vector<std::pair<std::string, std::string>> FromLauncher(const std::map<std::string, std::string>& s) {
  auto get = [&](const char* key, const char* fallback) {
    const auto it = s.find(key);
    return it == s.end() ? std::string(fallback) : it->second;
  };
  std::vector<std::pair<std::string, std::string>> out;
  int width = 1280, height = 720;
  const std::string res = get("resolution", "1280x720");
  if (const size_t x = res.find('x'); x != std::string::npos) {
    const int w = std::atoi(res.substr(0, x).c_str()), h = std::atoi(res.substr(x + 1).c_str());
    if (w >= 640 && w <= 8192 && h >= 480 && h <= 8192) width = w, height = h;
  }
  out.emplace_back("window_width", std::to_string(width));
  out.emplace_back("window_height", std::to_string(height));
  out.emplace_back("fullscreen", get("display_mode", "Fullscreen") != "Windowed" ? "true" : "false");
  out.emplace_back("vsync", get("vsync", "true") == "false" ? "false" : "true");
  out.emplace_back("sr_native_fps_limit", std::to_string(Choice(get("fps_limit", "60"), 60, {0, 30, 60, 120})));
  const std::string scale = get("render_scale", "auto");
  if (scale == "auto") {
    out.emplace_back("sr_native_render_scale", "0.0");
  } else {
    const int percent = Choice(scale, 100, {50, 75, 100, 125, 150, 200, 300, 400});
    if (percent < 100) {
      out.emplace_back("sr_render_scale", std::to_string(percent));
      out.emplace_back("sr_native_render_scale", "1.0");
    } else {
      char text[16];
      std::snprintf(text, sizeof(text), "%.2f", percent / 100.0);
      out.emplace_back("sr_native_render_scale", text);
    }
  }
  const std::string filter = get("texture_filter", "Anisotropic16x");
  const int aniso = filter == "Anisotropic2x" ? 2 : filter == "Anisotropic4x" ? 4 : filter == "Anisotropic8x" ? 8
                    : filter == "Anisotropic16x" ? 16 : -1;
  out.emplace_back("sr_native_anisotropic_filtering", std::to_string(aniso));
  out.emplace_back("sr_native_fxaa", get("antialiasing", "FXAA") == "FXAA" ? "true" : "false");
  out.emplace_back("sr_native_shadow_quality", std::to_string(Choice(get("shadow_quality", "4"), 4, {1, 2, 4})));
  out.emplace_back("sr_native_msaa_samples", std::to_string(Choice(get("msaa", "8"), 8, {1, 4, 8})));
  const bool keyboard = get("input", "KeyboardMouse") == "KeyboardMouse";
  out.emplace_back("mnk_mode", keyboard ? "true" : "false");
  out.emplace_back("mnk_mouse", keyboard ? "true" : "false");
  out.emplace_back("sr_skip_intro", get("skip_intro", "false") == "true" ? "true" : "false");
  const std::string luid = get("gpu_luid", "");
  if (!luid.empty() && luid.find('\'') == std::string::npos) out.emplace_back("sr_native_gpu_luid", "'" + luid + "'");
  return out;
}

}  // namespace

std::filesystem::path DefaultGameDataRoot() {
  const auto game = ExeFolder() / "game";
  std::error_code ec;
  if (std::filesystem::exists(game / "default.xex", ec) && std::filesystem::is_directory(game / "DATA", ec))
    return game;
  return {};
}

bool SaveConfigSetting(const std::filesystem::path& config, const std::string& key, const std::string& value) {
  static std::mutex lock;
  std::lock_guard guard(lock);
  std::vector<std::string> lines = ReadLines(config);
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
  std::ofstream out(config, std::ios::binary | std::ios::trunc);
  for (const auto& line : lines) out << line << "\r\n";
  return bool(out);
}

void MigrateLauncherSettings(const std::filesystem::path& config) {
  const char* local = std::getenv("LOCALAPPDATA");
  if (!local) return;
  const std::filesystem::path folder = std::filesystem::path(local) / "SupermanReturns" / "launcher";
  std::error_code ec;
  if (!std::filesystem::is_directory(folder, ec)) return;
  // The launcher keeps one file per machine; the newest one.
  std::filesystem::path ini;
  std::filesystem::file_time_type newest{};
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    if (entry.path().extension() != ".ini") continue;
    const auto time = entry.last_write_time(ec);
    if (ini.empty() || time > newest) ini = entry.path(), newest = time;
  }
  if (ini.empty()) return;
  std::map<std::string, std::string> settings;
  for (const auto& line : ReadLines(ini)) {
    const std::string t = Trim(line);
    if (t.empty() || t[0] == '#') continue;
    const size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    std::string key = Trim(t.substr(0, eq));
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    settings[key] = Trim(t.substr(eq + 1));
  }
  const std::set<std::string> present = ConfigKeys(ReadLines(config));
  bool ok = true;
  for (const auto& [key, value] : FromLauncher(settings)) {
    if (present.count(key)) continue;  // the config's own value wins
    ok = SaveConfigSetting(config, key, value) && ok;
  }
  // Kept, not deleted: renamed so the migration runs once.
  if (ok) std::filesystem::rename(ini, std::filesystem::path(ini.string() + ".bak"), ec);
}

namespace {
BOOL CALLBACK CloseOwnWindow(HWND window, LPARAM) {
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  if (pid == GetCurrentProcessId() && IsWindowVisible(window) && !GetWindow(window, GW_OWNER))
    PostMessageW(window, WM_CLOSE, 0, 0);
  return TRUE;
}
}  // namespace

bool RestartGame() {
  wchar_t module[MAX_PATH];
  GetModuleFileNameW(nullptr, module, MAX_PATH);
  // The same command line without an earlier --sr_restart_wait_pid, plus ours.
  std::wstring line = GetCommandLineW();
  for (size_t at; (at = line.find(L" --sr_restart_wait_pid=")) != std::wstring::npos;) {
    const size_t end = line.find(L' ', at + 1);
    line.erase(at, end == std::wstring::npos ? std::wstring::npos : end - at);
  }
  line += L" --sr_restart_wait_pid=" + std::to_wstring(GetCurrentProcessId());
  std::vector<wchar_t> mutable_line(line.begin(), line.end());
  mutable_line.push_back(0);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const std::wstring folder = std::filesystem::path(module).parent_path().wstring();
  if (!CreateProcessW(module, mutable_line.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startup,
                      &process))
    return false;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  // As the window's close button: the game's own shutdown.
  EnumWindows(CloseOwnWindow, 0);
  return true;
}

void WaitForPreviousInstance() {
  const int32_t pid = REXCVAR_GET(sr_restart_wait_pid);
  if (pid <= 0) return;
  if (HANDLE previous = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid))) {
    WaitForSingleObject(previous, 30000);
    CloseHandle(previous);
  }
}

}  // namespace superman_returns
