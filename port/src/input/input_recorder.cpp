#include "input/input_recorder.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/state_merge.h>
#include <rex/logging.h>

#include "frame_stats.h"

REXCVAR_DEFINE_STRING(sr_input_record, "", "Input",
                      "Automation: write every change of the pad state the game reads (pads, "
                      "keyboard, mouse) to this file, for sr_input_replay");
REXCVAR_DEFINE_STRING(sr_input_replay, "", "Input",
                      "Automation: the game reads the pad state recorded with sr_input_record "
                      "instead of any device");

namespace superman_returns::input {

using namespace rex;
using rex::input::DeviceId;
using rex::input::X_INPUT_GAMEPAD;
using rex::input::X_INPUT_STATE;

namespace {

// Both recording and replay count from the game's first input poll: in
// milliseconds and in guest frames. A recording with frame numbers is replayed
// by frame, so a load that takes longer than when it was recorded does not
// move every later input to another moment of the game.
double ElapsedMs() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
int64_t ElapsedFrames() {
  static const uint64_t start = GuestFrameCount();
  return int64_t(GuestFrameCount() - start);
}

struct Sample {
  double t_ms = 0;
  int64_t frame = -1;  // guest frames since the first poll (-1: not recorded)
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  bool operator==(const Sample& o) const {
    return buttons == o.buttons && lt == o.lt && rt == o.rt && lx == o.lx && ly == o.ly &&
           rx == o.rx && ry == o.ry;
  }
};

Sample FromGamepad(const X_INPUT_GAMEPAD& g) {
  Sample s;
  s.buttons = uint16_t(g.buttons);
  s.lt = g.left_trigger;
  s.rt = g.right_trigger;
  s.lx = int16_t(g.thumb_lx);
  s.ly = int16_t(g.thumb_ly);
  s.rx = int16_t(g.thumb_rx);
  s.ry = int16_t(g.thumb_ry);
  return s;
}

std::atomic<uint32_t> g_a_presses{0};

// A newly down on this device: one more press.
void NoteButtons(DeviceId id, uint16_t buttons) {
  static std::mutex mutex;
  static std::map<uint64_t, bool> down;
  std::lock_guard lock(mutex);
  const bool a = (buttons & 0x1000) != 0;
  bool& was = down[uint64_t(id)];
  if (a && !was) g_a_presses.fetch_add(1, std::memory_order_relaxed);
  was = a;
}

constexpr DeviceId kReplayDevice = static_cast<DeviceId>(0x52504C59);  // "RPLY"

class ReplayDriver final : public rex::input::InputDriver {
 public:
  explicit ReplayDriver(std::map<uint64_t, std::vector<Sample>> tracks, bool by_frame)
      : InputDriver(nullptr, 0), tracks_(std::move(tracks)), by_frame_(by_frame) {
    for (auto& [id, track] : tracks_) cursors_[id] = 0;
  }
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = kReplayDevice;
    info.name = "Input replay";
    out.push_back(info);
  }
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    if (id != kReplayDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
    std::lock_guard lock(mutex_);
    const double t = ElapsedMs();
    const int64_t frame = ElapsedFrames();
    auto due = [&](const Sample& s) { return by_frame_ ? s.frame <= frame : s.t_ms <= t; };
    X_INPUT_STATE merged = {};
    bool any = false;
    for (auto& [device, track] : tracks_) {
      size_t& i = cursors_[device];
      while (i + 1 < track.size() && due(track[i + 1])) ++i;
      if (track.empty() || !due(track[i])) continue;
      const Sample& s = track[i];
      X_INPUT_STATE st = {};
      st.gamepad.buttons = s.buttons;
      st.gamepad.left_trigger = s.lt;
      st.gamepad.right_trigger = s.rt;
      st.gamepad.thumb_lx = s.lx;
      st.gamepad.thumb_ly = s.ly;
      st.gamepad.thumb_rx = s.rx;
      st.gamepad.thumb_ry = s.ry;
      if (!any) {
        merged = st;
        any = true;
      } else {
        rex::input::MergeInto(merged, st);
      }
    }
    NoteButtons(kReplayDevice, uint16_t(merged.gamepad.buttons));
    const Sample now = FromGamepad(merged.gamepad);
    if (!(now == last_)) ++packet_;
    last_ = now;
    if (out_state) {
      *out_state = merged;
      out_state->packet_number = packet_;
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, rex::input::X_INPUT_CAPABILITIES* out_caps) override {
    if (id != kReplayDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
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
  X_RESULT SetDeviceVibration(DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
    return id == kReplayDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, rex::input::X_INPUT_KEYSTROKE*) override {
    return id == kReplayDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  std::mutex mutex_;
  std::map<uint64_t, std::vector<Sample>> tracks_;
  std::map<uint64_t, size_t> cursors_;
  Sample last_;
  uint32_t packet_ = 0;
  bool by_frame_ = false;
};

}  // namespace

uint32_t PadAPresses() { return g_a_presses.load(std::memory_order_relaxed); }

void RecordInputState(DeviceId id, const X_INPUT_STATE& state) {
  NoteButtons(id, uint16_t(state.gamepad.buttons));
  static std::mutex mutex;
  static std::FILE* file = [] {
    const std::string path = REXCVAR_GET(sr_input_record);
    std::FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "w");
    if (f) {
      std::fputs("# sr input recording 2: t_ms device buttons lt rt lx ly rx ry frame\n", f);
      REXLOG_INFO("input: recording to {}", path);
    }
    return f;
  }();
  if (!file) return;
  std::lock_guard lock(mutex);
  static std::map<uint64_t, Sample> last;
  Sample s = FromGamepad(state.gamepad);
  s.t_ms = ElapsedMs();
  s.frame = ElapsedFrames();
  auto it = last.find(uint64_t(id));
  if (it != last.end() && it->second == s) return;
  last[uint64_t(id)] = s;
  std::fprintf(file, "%.1f %llX %u %u %u %d %d %d %d %lld\n", s.t_ms, (unsigned long long)uint64_t(id),
               s.buttons, s.lt, s.rt, s.lx, s.ly, s.rx, s.ry, (long long)s.frame);
  std::fflush(file);
}

std::unique_ptr<rex::input::InputDriver> CreateReplayDriver() {
  const std::string path = REXCVAR_GET(sr_input_replay);
  if (path.empty()) return nullptr;
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (!f) {
    REXLOG_ERROR("input: cannot open the replay {}", path);
    return nullptr;
  }
  std::map<uint64_t, std::vector<Sample>> tracks;
  bool by_frame = true;
  char line[256];
  size_t samples = 0;
  while (std::fgets(line, sizeof(line), f)) {
    if (line[0] == '#') continue;
    Sample s;
    unsigned long long device = 0;
    unsigned buttons = 0, lt = 0, rt = 0;
    int lx = 0, ly = 0, rx = 0, ry = 0;
    long long frame = -1;
    const int fields = std::sscanf(line, "%lf %llX %u %u %u %d %d %d %d %lld", &s.t_ms, &device, &buttons, &lt,
                                   &rt, &lx, &ly, &rx, &ry, &frame);
    if (fields < 9) continue;
    s.frame = fields == 10 ? frame : -1;
    if (s.frame < 0) by_frame = false;
    s.buttons = uint16_t(buttons);
    s.lt = uint8_t(lt);
    s.rt = uint8_t(rt);
    s.lx = int16_t(lx);
    s.ly = int16_t(ly);
    s.rx = int16_t(rx);
    s.ry = int16_t(ry);
    tracks[device].push_back(s);
    ++samples;
  }
  std::fclose(f);
  by_frame = by_frame && samples > 0;
  REXLOG_INFO("input: replaying {} ({} changes, {} devices, by {})", path, samples, tracks.size(),
              by_frame ? "guest frame" : "time");
  return std::make_unique<ReplayDriver>(std::move(tracks), by_frame);
}

}  // namespace superman_returns::input
