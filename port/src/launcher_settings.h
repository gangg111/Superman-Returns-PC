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

// The folder for saves and achievements (`user_data` on entry: the runtime's
// default, Documents\superman_returns). When the game cannot write there
// (Windows' controlled folder access on Documents, a read-only or redirected
// folder) the saves could not be made and the game asked to sign in, then
// stopped at the save: falls back to %LOCALAPPDATA%\superman_returns, else to
// user_data next to the executable, copying the saves already in the default
// folder. Returns the folder to use; `note` says what was done (empty when the
// default is writable).
std::filesystem::path WritableUserDataRoot(const std::filesystem::path& user_data, std::string& note);

// The options menu's "restart now": the executable started again with the same
// command line (and this process's id in --sr_restart_wait_pid), then this game
// closed as its window's close button does. False when nothing was started.
bool RestartGame();

// In the restarted game, before anything else: waits (up to 30 s) for the
// previous one (sr_restart_wait_pid) to have exited.
void WaitForPreviousInstance();

}  // namespace superman_returns
