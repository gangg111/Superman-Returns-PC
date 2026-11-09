#include "input/gameinput_driver.h"
#include "input/input_recorder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

#include <windows.h>
#include <GameInput.h>

#include <rex/logging.h>

namespace superman_returns::input {

using rex::input::DeviceId;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

namespace {

void CALLBACK DeviceCallback(GameInputCallbackToken, void* context, IGameInputDevice* device,
                             uint64_t, GameInputDeviceStatus current, GameInputDeviceStatus) {
  static_cast<GameInputDriver*>(context)->OnDevice(device,
                                                   (current & GameInputDeviceConnected) != 0);
}

int16_t Axis(float v) {
  return int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
}

uint8_t Trigger(float v) {
  return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

// GameInput button -> XInput button (the Xbox 360 layout the game expects).
constexpr struct {
  uint32_t from;
  uint16_t to;
} kButtons[] = {
    {GameInputGamepadMenu, rex::input::X_INPUT_GAMEPAD_START},
    {GameInputGamepadView, rex::input::X_INPUT_GAMEPAD_BACK},
    {GameInputGamepadA, rex::input::X_INPUT_GAMEPAD_A},
    {GameInputGamepadB, rex::input::X_INPUT_GAMEPAD_B},
    {GameInputGamepadX, rex::input::X_INPUT_GAMEPAD_X},
    {GameInputGamepadY, rex::input::X_INPUT_GAMEPAD_Y},
    {GameInputGamepadDPadUp, rex::input::X_INPUT_GAMEPAD_DPAD_UP},
    {GameInputGamepadDPadDown, rex::input::X_INPUT_GAMEPAD_DPAD_DOWN},
    {GameInputGamepadDPadLeft, rex::input::X_INPUT_GAMEPAD_DPAD_LEFT},
    {GameInputGamepadDPadRight, rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT},
    {GameInputGamepadLeftShoulder, rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER},
    {GameInputGamepadRightShoulder, rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER},
    {GameInputGamepadLeftThumbstick, rex::input::X_INPUT_GAMEPAD_LEFT_THUMB},
    {GameInputGamepadRightThumbstick, rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB},
};

}  // namespace

GameInputDriver::GameInputDriver() : InputDriver(nullptr, 0) {}

GameInputDriver::~GameInputDriver() {
  if (input_) {
    if (callback_token_) input_->UnregisterCallback(callback_token_, 5000000);
    std::lock_guard lock(mutex_);
    for (Pad& p : pads_) p.device->Release();
    pads_.clear();
    input_->Release();
  }
}

X_STATUS GameInputDriver::Setup() {
  const HRESULT hr = GameInputCreate(&input_);
  if (FAILED(hr) || !input_) {
    REXLOG_WARN("GameInput: unavailable (0x{:08X}), no controllers", uint32_t(hr));
    input_ = nullptr;
    return X_STATUS_UNSUCCESSFUL;
  }
  // Blocking enumeration: the pads already connected are listed before
  // Setup returns, so the first guest poll sees them.
  input_->RegisterDeviceCallback(nullptr, GameInputKindGamepad, GameInputDeviceConnected,
                                 GameInputBlockingEnumeration, this, DeviceCallback,
                                 &callback_token_);
  std::lock_guard lock(mutex_);
  REXLOG_INFO("GameInput: ready, {} controller(s)", pads_.size());
  return X_STATUS_SUCCESS;
}

namespace {
std::atomic<int> g_connected_pads{0};
std::atomic<int64_t> g_last_pad_use_ms{0};
}  // namespace

int ConnectedPads() { return g_connected_pads.load(std::memory_order_relaxed); }
int64_t LastPadUseMs() { return g_last_pad_use_ms.load(std::memory_order_relaxed); }
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void GameInputDriver::OnDevice(IGameInputDevice* device, bool connected) {
  std::lock_guard lock(mutex_);
  struct Count {
    std::vector<Pad>& pads;
    ~Count() { g_connected_pads.store(int(pads.size()), std::memory_order_relaxed); }
  } count{pads_};
  auto it = std::find_if(pads_.begin(), pads_.end(),
                         [device](const Pad& p) { return p.device == device; });
  if (connected && it == pads_.end()) {
    device->AddRef();
    pads_.push_back({DeviceId(next_id_++), device});
    REXLOG_INFO("GameInput: controller connected ({} total)", pads_.size());
  } else if (!connected && it != pads_.end()) {
    it->device->Release();
    pads_.erase(it);
    REXLOG_INFO("GameInput: controller disconnected ({} total)", pads_.size());
  }
}

GameInputDriver::Pad* GameInputDriver::Find(DeviceId id) {
  for (Pad& p : pads_)
    if (p.id == id) return &p;
  return nullptr;
}

void GameInputDriver::EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) {
  std::lock_guard lock(mutex_);
  for (const Pad& p : pads_) {
    rex::input::DeviceInfo info;
    info.id = p.id;
    info.name = "GameInput controller";
    out.push_back(info);
  }
}

X_RESULT GameInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  std::lock_guard lock(mutex_);
  Pad* pad = Find(id);
  if (!pad) return X_ERROR_DEVICE_NOT_CONNECTED;
  IGameInputReading* reading = nullptr;
  GameInputGamepadState s = {};
  bool ok = false;
  if (SUCCEEDED(input_->GetCurrentReading(GameInputKindGamepad, pad->device, &reading)) &&
      reading) {
    ok = reading->GetGamepadState(&s);
    // In use: a button, a trigger or a stick past its rest (the cursor hides).
    if (ok && (s.buttons != 0 || s.leftTrigger > 0.2f || s.rightTrigger > 0.2f ||
               std::fabs(s.leftThumbstickX) > 0.3f || std::fabs(s.leftThumbstickY) > 0.3f ||
               std::fabs(s.rightThumbstickX) > 0.3f || std::fabs(s.rightThumbstickY) > 0.3f))
      g_last_pad_use_ms.store(NowMs(), std::memory_order_relaxed);
    const uint64_t t = reading->GetTimestamp();
    if (t != pad->last_timestamp) {
      pad->last_timestamp = t;
      ++pad->packet;
    }
    reading->Release();
  }
  if (out_state) {
    std::memset(out_state, 0, sizeof(*out_state));
    out_state->packet_number = pad->packet;
    if (ok && is_active()) {
      uint16_t buttons = 0;
      for (const auto& b : kButtons)
        if (s.buttons & b.from) buttons |= b.to;
      out_state->gamepad.buttons = buttons;
      out_state->gamepad.left_trigger = Trigger(s.leftTrigger);
      out_state->gamepad.right_trigger = Trigger(s.rightTrigger);
      out_state->gamepad.thumb_lx = Axis(s.leftThumbstickX);
      out_state->gamepad.thumb_ly = Axis(s.leftThumbstickY);
      out_state->gamepad.thumb_rx = Axis(s.rightThumbstickX);
      out_state->gamepad.thumb_ry = Axis(s.rightThumbstickY);
    }
    RecordInputState(id, *out_state);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT GameInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t,
                                                X_INPUT_CAPABILITIES* out_caps) {
  std::lock_guard lock(mutex_);
  if (!Find(id)) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;      // gamepad
    out_caps->sub_type = 0x01;  // standard controller
    out_caps->gamepad.buttons = 0xF3FF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = int16_t(0x7FFF);
    out_caps->gamepad.thumb_ly = int16_t(0x7FFF);
    out_caps->gamepad.thumb_rx = int16_t(0x7FFF);
    out_caps->gamepad.thumb_ry = int16_t(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT GameInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) {
  std::lock_guard lock(mutex_);
  Pad* pad = Find(id);
  if (!pad) return X_ERROR_DEVICE_NOT_CONNECTED;
  GameInputRumbleParams params = {};
  if (vibration) {
    // Xbox 360: left = low-frequency motor, right = high-frequency motor.
    params.lowFrequency = float(uint16_t(vibration->left_motor_speed)) / 65535.0f;
    params.highFrequency = float(uint16_t(vibration->right_motor_speed)) / 65535.0f;
  }
  pad->device->SetRumbleState(&params);
  return X_ERROR_SUCCESS;
}

X_RESULT GameInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE*) {
  std::lock_guard lock(mutex_);
  return Find(id) ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

}  // namespace superman_returns::input
