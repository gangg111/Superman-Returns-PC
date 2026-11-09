// Keys Mapping (the options menu's screen): one keyboard key or mouse button
// captured for a pad button, written to its keybind_* option (read live by the
// Raw Input driver) and to superman_returns.toml.
//
// The screen starts a capture on its game thread (BeginKeyCapture); the Raw
// Input thread offers each new key or mouse button press (OfferCapturedKey);
// the screen polls KeyCaptureActive until it ends. A capture ends with the
// first key or button, after 6 s, or when the game loses the focus.
#pragma once

#include <cstdint>

namespace superman_returns::input {

// The bindings the screen lists, in its order (pages: actions, movement, camera).
constexpr int kKeyBindCount = 24;
extern const char* const kKeyBindCvars[kKeyBindCount];

// The default of each binding (the same order).
extern const char* const kDefaultKeyBinds[kKeyBindCount];
// At start: the defaults for the bindings superman_returns.toml does not name.
void ApplyDefaultKeyBinds();
// Keys Mapping's Y: every binding to its default, written to the settings file.
void ResetKeyBinds();

void BeginKeyCapture(int index);
bool KeyCaptureActive();
void CancelKeyCapture();

// The first key of binding `index`: its code | 0x100 Shift | 0x200 Ctrl | 0x400
// Alt; 0 when the binding is empty or unknown.
int KeyBindCode(int index);
// The key a hint shows for binding `index`: in the menus Enter for A, Escape
// for B and the arrows for the d-pad and the left stick (the Raw Input driver
// presses them so), else KeyBindCode's key (no
// modifiers); 0 when none.
int MenuHintKey(int index);

// Raw Input thread: `vk` newly down. True when the capture took it.
bool OfferCapturedKey(int vk);
// Raw Input thread, every 250 ms: the time limit and the focus.
void KeyCaptureTick(bool game_in_foreground);
// While a capture runs, and until the captured key is let go, the keyboard
// sends the game no pad state (the key would act in the menu at once).
bool KeyCaptureMutesKeyboard(const bool (&down)[256]);

// A menu is open (the front end, a pause or a loading screen): MAIN's script
// reports FlowMgr's state every 100 ms through SettingsAdapter.SetMenuActive.
// The mouse then moves the cursor instead of the camera.
void SetMenuActive(bool active);
bool MenuActive();

// Clicking the menus (MAIN's mouse poll, build_ui_mod.py): the cursor in the
// menus' 1280x720 stage (-1 when not over the game, not in a menu, or a pad
// was used last), a pending left click, and short pad presses the script asks
// for (A on a button, RIGHT on an option row). Right click (B) and the wheel
// (up/down) are pressed by the Raw Input driver itself.
void SetGameWindow(void* hwnd);
int MenuMouseX();
int MenuMouseY();
bool TakeMenuClick();
void NoteMenuClick();
enum : uint16_t { kPadA = 0x1000, kPadB = 0x2000, kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004,
                  kPadRight = 0x0008 };
void PressPad(uint16_t buttons);
// Raw Input driver, each poll: the buttons pressed for the menus (held for a
// few polls so the game sees a press and a release).
uint16_t MenuPadButtons();
// The mouse is the input last used (set by the Raw Input driver).
void SetMouseLast(bool last);
// The button icons the menus show (MAIN's ButtonIcon, build_ui_mod.py): a pad's
// while a connected pad was used after the keyboard and mouse, else the keys.
void SetPadIcons(bool pad);
bool PadIcons();

}  // namespace superman_returns::input
