#pragma once

#include <filesystem>
#include <string>

// The game starts from superman_returns.exe without the launcher: these do what
// the launcher did, before the runtime reads its config.
namespace superman_returns {

// <exe folder>\game when it holds the game (default.xex and DATA), else empty.
std::filesystem::path DefaultGameDataRoot();

// Once: the launcher's last settings (%LOCALAPPDATA%\SupermanReturns\launcher\*.ini)
// written into the config file as the options the launcher passed on its command
// line, for keys the config does not set yet. The .ini is then renamed to .ini.bak.
void MigrateLauncherSettings(const std::filesystem::path& config);

// Sets `key = value` in the config file, keeping its other lines (comments
// included); a key not there yet is added at the end. False when it could not
// be written.
bool SaveConfigSetting(const std::filesystem::path& config, const std::string& key, const std::string& value);

// The options menu's "restart now": the executable started again with the same
// command line (and this process's id in --sr_restart_wait_pid), then this game
// closed as its window's close button does. False when nothing was started.
bool RestartGame();

// In the restarted game, before anything else: waits (up to 30 s) for the
// previous one (sr_restart_wait_pid) to have exited.
void WaitForPreviousInstance();

}  // namespace superman_returns
