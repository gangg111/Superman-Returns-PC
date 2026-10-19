#include "native_audio/bridge.h"

#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "generated/default/superman_returns_init.h"
#include "native_audio/bank_source.h"
#include "native_audio/engine.h"
#include "native_audio/sample_bank.h"
#include "native_audio/snd_player.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace native_audio::bridge {

namespace {

uint32_t R32(const uint8_t* base, uint32_t a) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + a));
}
float RF(const uint8_t* base, uint32_t a) { return std::bit_cast<float>(R32(base, a)); }
double RD(const uint8_t* base, uint32_t a) {
  return std::bit_cast<double>((uint64_t(R32(base, a)) << 32) | R32(base, a + 4));
}

// Samples the game's decoder produced, per (guest player, decoder slot).
struct Capture {
  std::deque<uint32_t> blocks;  // frames of each input, in set-input order
  std::deque<float> frames;     // interleaved
  int channels = 0;
};

class CaptureSource;

struct State {
  std::mutex mutex;
  Engine engine{48000.0f};
  uint32_t system = 0;        // guest System*
  uint32_t master = 0;        // guest voice holding the Dac
  Voice* native_master = nullptr;
  std::unordered_map<uint32_t, Voice*> voices;    // guest Voice* -> native
  std::unordered_map<uint32_t, Plugin*> plugins;  // guest plug-in -> native
  std::map<std::string, Bus*> buses_by_name;
  std::vector<uint32_t> players;                  // guest SnP0 plug-ins
  std::map<std::pair<uint32_t, int>, Capture> captures;
  // statistics
  uint64_t blocks = 0, exact = 0, ret_diff = 0, isdone_diff = 0, id_diff = 0, unmapped = 0;
  uint64_t bank_hits = 0, bank_misses = 0;
  SampleBank bank;
  bool bank_open = false;
  double max_diff = 0.0;
  bool first_logged = false;
  int last_native_ret = 0;  // of the master voice, for the Dac finalize hook
};

State& S() {
  static State s;
  return s;
}

class CaptureSource : public SoundSource {
 public:
  CaptureSource(uint32_t player, int channels) : player_(player), channels_(channels) {}
  int Channels() const override { return channels_; }
  int BlockCount() const override { return 1 << 30; }
  int BlockFrames(int) const override { return 1 << 30; }
  void Bind(int slot) override {
    slot_ = slot;
    Capture& c = S().captures[{player_, slot_}];
    c = Capture{};
    c.channels = channels_;
  }
  int OpenBlockFrames(int) override {
    Capture& c = S().captures[{player_, slot_}];
    if (c.blocks.empty()) return 0;
    const int n = int(c.blocks.front());
    c.blocks.pop_front();
    return n;
  }
  int Decode(int, int, float* dst, int frames) override {
    Capture& c = S().captures[{player_, slot_}];
    const int have = int(c.frames.size()) / (channels_ ? channels_ : 1);
    const int n = frames < have ? frames : have;
    for (int i = 0; i < n; ++i)
      for (int ch = 0; ch < channels_; ++ch) {
        dst[ch * n + i] = c.frames.front();
        c.frames.pop_front();
      }
    return n;
  }

 private:
  uint32_t player_;
  int channels_;
  int slot_ = -1;
};

std::string ReadString(const uint8_t* base, uint32_t p) {
  std::string s;
  for (uint32_t i = 0; p && i < 64 && base[p + i]; ++i) s += char(base[p + i]);
  return s;
}

}  // namespace

namespace {
std::string ModeName() {
  const char* v = std::getenv("SR_NATIVE_AUDIO");
  return v ? v : "";
}

// shadow-bank: the native SndPlayers play the converted sounds from the bank
// (<exe dir>/native_audio or SR_NATIVE_AUDIO_DIR) instead of the captured
// decoder output; unresolved sounds fall back to the capture.
bool BankMode() {
  static const bool on =
      ModeName() == "shadow-bank" || ModeName() == "native" || ModeName() == "full";
  return on;
}

// full: the original engine no longer renders (no executor, DSP or XMA); the
// game reads its audio state (IsDone, positions, voice release) from the
// native engine. The guest command queue is still drained (System+20).
bool FullMode() {
  static const bool on = ModeName() == "full";
  return on;
}

// native: what reaches the speakers is the native engine's mix (bank sounds);
// the original engine still runs alongside for now (game-visible state).
bool NativeOutput() {
  static const bool on = ModeName() == "native" || ModeName() == "full";
  return on;
}

std::string BankDir() {
  if (const char* d = std::getenv("SR_NATIVE_AUDIO_DIR"); d && *d) return d;
  char path[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  std::string dir(path);
  return dir.substr(0, dir.find_last_of("/\\")) + "/native_audio";
}

// shadow-bank diagnostics: the first chunk of each sound from the bank against
// what the game's decoder produced for it, with the best time shift (lag in
// frames, bank relative to the game) and the mean absolute residual.
class DiagSource : public BankSource {
 public:
  DiagSource(std::shared_ptr<const Pcm> pcm, uint32_t player, std::string name)
      : BankSource(std::move(pcm)), player_(player), name_(std::move(name)) {}
  void Bind(int slot) override {
    slot_ = slot;
    S().captures[{player_, slot_}] = Capture{};
  }
  int Decode(int block, int offset, float* dst, int frames) override {
    const int n = BankSource::Decode(block, offset, dst, frames);
    Capture& c = S().captures[{player_, slot_}];
    const int ch = Channels();
    // Channel 0 of what both sides produced for this sound, in decode order.
    for (int i = 0; i < n; ++i) {
      bank_.push_back(dst[i]);
      game_.push_back(size_t(i * ch) < c.frames.size() ? c.frames[size_t(i * ch)] : 0.0f);
    }
    for (int i = 0; i < n * ch && !c.frames.empty(); ++i) c.frames.pop_front();
    static int logged = 0;
    constexpr size_t kWant = 8192;
    if (!checked_ && bank_.size() >= kWant && logged < 40) {
      checked_ = true;
      ++logged;
      auto first_nz = [](const std::vector<float>& v) {
        for (size_t i = 0; i < v.size(); ++i)
          if (std::fabs(v[i]) > 1e-4f) return long(i);
        return -1L;
      };
      // window with sound on the game side, lag = bank index - game index
      const long g0 = first_nz(game_), b0 = first_nz(bank_);
      double best_r = 1e9;
      long best = 0;
      const long w0 = g0 < 0 ? 0 : g0 + 64, wn = 1024;
      auto residual = [&](long lag) {
        double sum = 0.0;
        long cnt = 0;
        for (long i = w0; i < w0 + wn && i < long(game_.size()); ++i) {
          const long j = i + lag;
          if (j < 0 || j >= long(bank_.size())) continue;
          sum += std::fabs(double(bank_[size_t(j)]) - game_[size_t(i)]);
          ++cnt;
        }
        return cnt > wn / 2 ? sum / cnt : 1e9;
      };
      for (long lag = -3000; lag <= 3000; ++lag) {
        const double r = residual(lag);
        if (r < best_r) { best_r = r; best = lag; }
      }
      double level = 0.0;
      for (long i = w0; i < w0 + wn && i < long(game_.size()); ++i) level += std::fabs(game_[size_t(i)]);
      level /= wn;
      REXAPU_INFO("native audio bank check: {}: first sound game frame {} bank frame {}; level {:.4f}, "
                  "residual at 0 {:.5f}, best lag {} residual {:.5f}", name_, g0, b0, level,
                  residual(0), best, best_r);
    }
    return n;
  }

 private:
  std::vector<float> bank_, game_;
  uint32_t player_;
  std::string name_;
  int slot_ = -1;
  bool checked_ = false;
};

std::shared_ptr<SoundSource> FromBank(uint8_t* base, State& s, uint32_t req, uint32_t player) {
  if (!s.bank_open) {
    s.bank_open = true;
    const std::string dir = BankDir();
    const bool ok = s.bank.Open(dir);
    REXAPU_INFO("native audio: sample bank {} ({} sounds) from {}", ok ? "open" : "missing",
                s.bank.size(), dir);
  }
  const uint32_t snr = R32(base, req + 20);
  if (!snr) return nullptr;
  const SoundInfo* info = nullptr;
  const uint32_t type = R32(base, snr + 4) >> 30;
  std::string file;
  uint32_t off = 0;
  if (type == 0) {
    info = s.bank.FindByFnv(SampleBank::Fnv1a(base + snr, 256));
  } else {
    file = ReadString(base, R32(base, req + 16));
    off = uint32_t(RD(base, req + 8));
    info = s.bank.FindStream(file, off);
  }
  std::shared_ptr<const Pcm> pcm = info ? s.bank.Load(*info) : nullptr;
  if (!pcm) {
    if (++s.bank_misses <= 20)
      REXAPU_INFO("native audio: no bank sound for type {} snr {:08X}{:08X} fnv {:08X} file '{}' "
                  "offset {}", type, R32(base, snr), R32(base, snr + 4),
                  SampleBank::Fnv1a(base + snr, 256), file, off);
    return nullptr;
  }
  ++s.bank_hits;
  return std::make_shared<DiagSource>(pcm, player, info->name);
}
}  // namespace

bool Active() {
  static const bool on = [] {
    const std::string m = ModeName();
    return m == "shadow" || m == "shadow-bank" || m == "native" || m == "full";
  }();
  return on;
}

void OnVoiceCreate(uint8_t* base, uint32_t count, uint32_t entries, uint32_t gvoice) {
  if (!Active() || !gvoice) return;
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.system) s.system = R32(base, gvoice + 32);
  std::vector<Engine::Entry> ne;
  std::vector<uint32_t> fourcc;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t e = entries + i * 12;
    const uint32_t desc = R32(base, e + 4);
    const uint32_t id = desc ? R32(base, desc + 4) : 0;
    const uint32_t args = R32(base, e);
    Engine::Entry en{PluginClass(id), int(base[e + 8]), 0.0f};
    if (args && (id == uint32_t(PluginClass::kSndPlayer) || id == uint32_t(PluginClass::kSend)))
      en.arg = RF(base, args);
    ne.push_back(en);
    fourcc.push_back(id);
  }
  Voice* v = s.engine.CreateVoice(ne);
  if (!v) {
    ++s.unmapped;
    return;
  }
  s.voices[gvoice] = v;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t gp = R32(base, gvoice + 72 + i * 4);
    s.plugins[gp] = v->plugins[i].get();
    if (fourcc[i] == uint32_t(PluginClass::kDac)) {
      s.master = gvoice;
      s.native_master = v;
    }
    if (fourcc[i] == uint32_t(PluginClass::kSndPlayer)) s.players.push_back(gp);
    if (fourcc[i] == uint32_t(PluginClass::kSubMix)) {
      const uint32_t args = R32(base, entries + i * 12);
      if (args) s.buses_by_name[ReadString(base, R32(base, args))] = v->plugins[i]->AsBus();
    }
  }
}

void OnVoiceRelease(uint8_t*, uint32_t gvoice) {
  if (!Active()) return;
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  auto it = s.voices.find(gvoice);
  if (it == s.voices.end()) return;
  s.engine.ReleaseVoice(it->second);
}

void OnSetParam(uint8_t*, uint32_t plugin, uint32_t index, float value) {
  if (!Active()) return;
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  auto it = s.plugins.find(plugin);
  if (it != s.plugins.end()) s.engine.SetParam(it->second, int(index), value);
}

void OnControl(uint8_t* base, uint32_t plugin, uint32_t index, uint32_t data) {
  if (!Active()) return;
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  auto it = s.plugins.find(plugin);
  if (it == s.plugins.end()) return;
  Plugin* p = it->second;
  switch (p->cls) {
    case PluginClass::kSndPlayer: {
      auto* player = static_cast<SndPlayer*>(p);
      if (index != 0) {
        player->Stop();
        break;
      }
      PlayRequest r;
      r.start_time = RD(base, data);
      r.stream_offset = RD(base, data + 8);
      const uint32_t snr = R32(base, data + 20);
      if (snr) std::memcpy(r.snr, base + snr, 16);
      r.flag = RF(base, data + 28);
      const int sound_ch = int(((R32(base, snr) >> 18) & 0x3F) + 1);
      if (BankMode()) r.source = FromBank(base, s, data, plugin);
      if (!r.source) r.source = std::make_shared<CaptureSource>(plugin, sound_ch);
      const float id = player->Start(r);
      if (R32(base, data + 32) != std::bit_cast<uint32_t>(id)) ++s.id_diff;
      break;
    }
    case PluginClass::kSend:
    case PluginClass::kRoute: {
      if (p->cls == PluginClass::kSend && index != 0) {  // attach by bus name
        auto b = s.buses_by_name.find(ReadString(base, data));
        s.engine.AttachToBus(p, b == s.buses_by_name.end() ? nullptr : b->second);
        break;
      }
      const uint32_t gbus = data ? R32(base, data) : 0;
      Bus* bus = nullptr;
      if (gbus) {
        auto b = s.plugins.find(gbus);
        if (b != s.plugins.end()) bus = b->second->AsBus();
      }
      if (p->cls == PluginClass::kRoute)
        s.engine.AttachToBus(p, bus, int(RF(base, data + 4)), int(RF(base, data + 8)),
                             int(RF(base, data + 12)));
      else
        s.engine.AttachToBus(p, bus);
      break;
    }
    case PluginClass::kDac:
      if (index == 3) s.engine.Enqueue([&s] { s.engine.dac_fade = true; });
      break;
    case PluginClass::kVuMeter:
      s.engine.Enqueue([p] { p->Control(0); });
      break;
    default:
      break;
  }
}

int IsDoneOverride(uint32_t plugin) {
  if (!Active() || !FullMode()) return -1;
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  auto it = s.plugins.find(plugin);
  if (it == s.plugins.end() || it->second->cls != PluginClass::kSndPlayer) return -1;
  return static_cast<SndPlayer*>(it->second)->IsDone() ? 1 : 0;
}

void OnIsDone(uint8_t*, uint32_t plugin, bool guest) {
  if (!Active()) return;
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  auto it = s.plugins.find(plugin);
  if (it == s.plugins.end()) return;
  if (static_cast<SndPlayer*>(it->second)->IsDone() != guest) ++s.isdone_diff;
}

// --------------------------------------------------------------- capture

namespace {
std::pair<uint32_t, int> FindDecoder(const uint8_t* base, State& s, uint32_t dec) {
  for (uint32_t p : s.players)
    for (int i = 0; i < 20; ++i)
      if (R32(base, p + 136 + 8 * i) == dec) return {p, i};
  return {0, -1};
}
}  // namespace

void OnSetInput(uint8_t* base, uint32_t dec, uint32_t slot) {
  if (!Active()) return;
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  const auto key = FindDecoder(base, s, dec);
  if (key.second < 0) return;
  s.captures[key].blocks.push_back(R32(base, slot + 4));
}

void OnDecode(uint8_t* base, uint32_t dec, uint32_t dst, int frames) {
  if (!Active() || frames <= 0) return;
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  const auto key = FindDecoder(base, s, dec);
  if (key.second < 0) return;
  Capture& c = s.captures[key];
  const int ch = base[dec + 8];
  for (int i = 0; i < frames; ++i)
    for (int k = 0; k < ch; ++k) c.frames.push_back(RF(base, dst + uint32_t(k * frames + i) * 4));
}

}  // namespace native_audio::bridge

using namespace native_audio::bridge;

// System drain (once per mixed block on the Dac thread): the native queue too.
REX_HOOK_RAW(sub_8264C540) {
  const uint32_t sys = ctx.r3.u32;
  __imp__sub_8264C540(ctx, base);
  if (!Active()) return;
  auto& s = native_audio::bridge::S();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.system && sys == s.system) s.engine.Drain();
}

// Voice graph executor: after the original renders the master voice, render
// the native graph for the same block and compare the master bus outputs.
REX_HOOK_RAW(sub_8265A148) {
  const uint32_t voice = ctx.r3.u32, io = ctx.r4.u32;
  if (Active() && FullMode()) {
    auto& s = native_audio::bridge::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.native_master && voice == s.master) {
      s.engine.clock = RD(base, s.system);
      const int nret = s.engine.RenderGraph(s.native_master, s.engine.io);
      s.last_native_ret = nret;
      ctx.r3.u64 = uint32_t(nret);
      if (++s.blocks % 1000 == 0)
        REXAPU_INFO("native audio full: {} blocks, voices {}, bank {} hits {} misses", s.blocks,
                    s.voices.size(), s.bank_hits, s.bank_misses);
      return;
    }
  }
  __imp__sub_8265A148(ctx, base);
  if (!Active()) return;
  auto& s = native_audio::bridge::S();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.native_master || voice != s.master) return;
  const int gret = int32_t(ctx.r3.u32);
  s.engine.clock = RD(base, s.system);
  const int nret = s.engine.RenderGraph(s.native_master, s.engine.io);
  s.last_native_ret = nret;
  // The Dac only finalizes and submits when the render returned 1.
  if (NativeOutput()) ctx.r3.u64 = uint32_t(nret);
  ++s.blocks;
  if (gret != nret) {
    ++s.ret_diff;
  } else if (gret == 1) {
    const uint32_t cur = R32(base, io + 4 + 4 * base[io + 28]);
    const float* n = s.engine.io.Cur();
    size_t diff = 0;
    double md = 0.0;
    for (int i = 0; i < 6 * 256; ++i) {
      const uint32_t gv = R32(base, cur + uint32_t(i) * 4);
      if (gv != std::bit_cast<uint32_t>(n[i])) {
        ++diff;
        md = std::max(md, std::fabs(double(std::bit_cast<float>(gv)) - double(n[i])));
      }
    }
    if (!diff) {
      ++s.exact;
    } else {
      s.max_diff = std::max(s.max_diff, md);
      if (!s.first_logged) {
        s.first_logged = true;
        REXAPU_INFO("native audio shadow: first difference at block {}: {} samples, max {}",
                    s.blocks, diff, md);
      }
    }
  } else {
    ++s.exact;
  }
  if (s.blocks % 1000 == 0)
    REXAPU_INFO("native audio shadow: {} blocks, {} exact, {} return diffs, max abs diff {}, "
                "IsDone diffs {}, id diffs {}, unmapped voices {}, bank {} hits {} misses",
                s.blocks, s.exact, s.ret_diff, s.max_diff, s.isdone_diff, s.id_diff, s.unmapped,
                s.bank_hits, s.bank_misses);
}

// Dac finalize (interleave, reorder, fade-in, clamp) of the block the Dac then
// copies into its packet: in native mode the packet gets the native mix.
REX_HOOK_RAW(sub_82650810) {
  const uint32_t dac = ctx.r3.u32;
  // In full mode the guest graph never ran, so its io buffers may not even be
  // allocated: skip the original finalize and give the Dac a valid buffer.
  if (!(Active() && FullMode())) __imp__sub_82650810(ctx, base);
  if (!Active() || !NativeOutput()) return;
  auto& s = native_audio::bridge::S();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.last_native_ret != 1) return;
  float out[6 * 256];
  native_audio::DacFinalize(s.engine.io.Cur(), out, 6, &s.engine.dac_fade);
  const uint32_t io = R32(base, dac + 56);
  const uint32_t cur = base[io + 28];
  uint32_t buf = R32(base, io + 4 + 4 * cur);
  if (!buf || R32(base, io + 20 + 4 * cur) < 6 * 256 * 4) {
    buf = REX_KERNEL_MEMORY()->SystemHeapAlloc(6 * 256 * 4, 128);
    *reinterpret_cast<uint32_t*>(base + io + 4 + 4 * cur) = __builtin_bswap32(buf);
    *reinterpret_cast<uint32_t*>(base + io + 20 + 4 * cur) = __builtin_bswap32(6u * 256 * 4);
  }
  for (int i = 0; i < 6 * 256; ++i)
    *reinterpret_cast<uint32_t*>(base + buf + uint32_t(i) * 4) =
        __builtin_bswap32(std::bit_cast<uint32_t>(out[i]));
}

// full mode: the game's queries of voice and plug-in state answered natively.
namespace {
native_audio::Voice* MappedVoice(uint32_t v) {
  auto& s = native_audio::bridge::S();
  auto it = s.voices.find(v);
  return it == s.voices.end() ? nullptr : it->second;
}
native_audio::Plugin* MappedPlugin(uint32_t p) {
  auto& s = native_audio::bridge::S();
  auto it = s.plugins.find(p);
  return it == s.plugins.end() ? nullptr : it->second;
}
}  // namespace

// Voice::IsReleaseDone(voice) -> u8
REX_HOOK_RAW(sub_826500F8) {
  if (Active() && FullMode()) {
    auto& s = native_audio::bridge::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (native_audio::Voice* v = MappedVoice(ctx.r3.u32)) {
      ctx.r3.u64 = v->IsStopped() ? 1 : 0;
      return;
    }
  }
  __imp__sub_826500F8(ctx, base);
}

// Voice::GetLatencySeconds(voice) -> f1 = tail / System+12
REX_HOOK_RAW(sub_826500C8) {
  if (Active() && FullMode()) {
    auto& s = native_audio::bridge::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (native_audio::Voice* v = MappedVoice(ctx.r3.u32)) {
      ctx.f1.f64 = double(v->TailNow() / s.engine.rate);
      return;
    }
  }
  __imp__sub_826500C8(ctx, base);
}

// Plugin::GetParamD(plugin, index, double* out)
REX_HOOK_RAW(sub_8264CE00) {
  if (Active() && FullMode()) {
    auto& s = native_audio::bridge::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (native_audio::Plugin* p = MappedPlugin(ctx.r3.u32)) {
      const uint64_t b = std::bit_cast<uint64_t>(p->GetParam(int(ctx.r4.u32)));
      *reinterpret_cast<uint64_t*>(base + ctx.r5.u32) = __builtin_bswap64(b);
      return;
    }
  }
  __imp__sub_8264CE00(ctx, base);
}

// Plugin::GetParamF(plugin, index, float* out)
REX_HOOK_RAW(sub_8264CDE8) {
  if (Active() && FullMode()) {
    auto& s = native_audio::bridge::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (native_audio::Plugin* p = MappedPlugin(ctx.r3.u32)) {
      const uint32_t b = std::bit_cast<uint32_t>(float(p->GetParam(int(ctx.r4.u32))));
      *reinterpret_cast<uint32_t*>(base + ctx.r5.u32) = __builtin_bswap32(b);
      return;
    }
  }
  __imp__sub_8264CDE8(ctx, base);
}
