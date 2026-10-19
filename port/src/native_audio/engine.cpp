#include "native_audio/engine.h"

#include "native_audio/snd_player.h"

#include <cstring>

namespace native_audio {

namespace {

// ------------------------------------------------------------ class adapters

class PitchPlugin : public Plugin {
 public:
  int Process(Io&, bool) override { return 1; }
  bool HasProcess() const override { return false; }
  bool HasHook() const override { return true; }
  void Hook(Io& io, float) override {  // sub_82662608
    ScalarScope fpu;
    io.rate = pitch * io.rate;
  }
  void SetParam(int i, float v) override {
    if (i == 0) pitch = v;
  }
  double GetParam(int i) const override { return i == 0 ? pitch : 0.0; }
  float pitch = 1.0f;  // +32
};

class FilterPlugin : public Plugin {
 public:
  FilterPlugin(bool hp, int ch) : f(hp, ch) {}
  int Process(Io& io, bool) override { return f.Process(io, voice_rate); }
  bool HasHook() const override { return true; }
  void Hook(Io& io, float rate_in) override { f.Hook(io, rate_in); }
  float Latency() const override { return 450.0f; }  // 0x82082668
  void SetParam(int i, float v) override {
    if (i == 0) f.cutoff = v;
  }
  double GetParam(int i) const override { return i == 0 ? f.cutoff : 0.0; }
  BiquadFilter f;
  float voice_rate = 48000.0f;  // System+12
};

class GainPlugin : public Plugin {
 public:
  explicit GainPlugin(int ch) : g(ch) {}
  int Process(Io& io, bool reset) override { return g.Process(io, reset); }
  void SetParam(int i, float v) override {
    if (i == 0) g.gain = v;
  }
  double GetParam(int i) const override { return i == 0 ? g.gain : 0.0; }
  Gain g;
};

class PanPlugin : public Plugin {
 public:
  PanPlugin(int in, int out) : p(in, out) {}
  int Process(Io& io, bool) override { return p.Process(io); }
  void SetParam(int i, float v) override {
    if (i == 0) p.distance = v;
    else if (i == 1) p.azimuth = v;
    else if (i == 2) p.half_width = v;
  }
  double GetParam(int i) const override {
    return i == 0 ? p.distance : i == 1 ? p.azimuth : i == 2 ? p.half_width : 0.0;
  }
  PanSurround p;
};

class SendPlugin : public Plugin {
 public:
  SendPlugin(int in, bool ignore) : s(in, ignore) {}
  int Process(Io& io, bool reset) override { return s.Process(io, reset); }
  bool HasHook() const override { return true; }
  void Hook(Io& io, float) override { s.Hook(io); }
  void SetParam(int i, float v) override {
    if (i == 0) s.level = v;
  }
  double GetParam(int i) const override { return i == 0 ? s.level : 0.0; }
  void AttachToBus(Bus* b, int, int, int) override { s.Attach(b); }
  void Release() override { s.Release(); }
  Send s;
};

class SubMixPlugin : public Plugin {
 public:
  explicit SubMixPlugin(int ch) : b(ch) {}
  int Process(Io& io, bool) override { return b.Process(io); }
  Bus* AsBus() override { return &b; }
  void Release() override { b.Release(); }
  Bus b;
};

class RoutePlugin : public Plugin {
 public:
  int Process(Io& io, bool) override { return r.Process(io); }
  void AttachToBus(Bus* b, int src, int dst, int count) override { r.Attach(b, src, dst, count); }
  void Release() override { r.Release(); }
  Route r;
};

class ReverbPlugin : public Plugin {
 public:
  explicit ReverbPlugin(int ch) : r(ch) {}
  int Process(Io& io, bool) override { return r.Process(io, mixer_rate); }
  float Latency() const override { return r.tail; }
  void SetParam(int i, float v) override {
    if (i == 0) r.p0 = v;
    else if (i == 1) r.p1 = v;
    else if (i == 2) r.p2 = v;
  }
  double GetParam(int i) const override {
    return i == 0 ? r.p0 : i == 1 ? r.p1 : i == 2 ? r.p2 : 0.0;
  }
  Reverb r;
  float mixer_rate = 48000.0f;
};

class DelayPlugin : public Plugin {
 public:
  explicit DelayPlugin(int ch) : d(ch) {}
  int Process(Io& io, bool) override { return d.Process(io, mixer_rate); }
  float Latency() const override { return d.tail24; }
  void SetParam(int i, float v) override {
    if (i == 0) d.p0 = v;
    else if (i == 1) d.p1 = v;
  }
  double GetParam(int i) const override { return i == 0 ? d.p0 : i == 1 ? d.p1 : 0.0; }
  Delay d;
  float mixer_rate = 48000.0f;
};

class VuPlugin : public Plugin {
 public:
  VuPlugin(int ch, float rate) : v(ch, rate) {}
  int Process(Io& io, bool) override { return v.Process(io); }
  void SetParam(int i, float x) override {
    if (i >= 0 && i < 18) v.params[i] = x;
  }
  double GetParam(int i) const override { return (i >= 0 && i < 18) ? v.params[i] : 0.0; }
  VuMeter v;
};

class AiffPlugin : public Plugin {  // debug writer, never opened by the game
 public:
  int Process(Io&, bool) override { return 1; }
};

class DacPlugin : public Plugin {
 public:
  int Process(Io&, bool) override { return 1; }
  bool HasProcess() const override { return false; }
};

}  // namespace

// ------------------------------------------------------------ voice

float Voice::TailNow() const {
  ScalarScope fpu;
  float t = 0.0f;
  for (size_t i = 1; i < plugins.size(); ++i) t = t + plugins[i]->Latency();
  return t;
}

bool Voice::IsStopped() {
  if (state == 0) return false;
  if (state == 1) {
    const float t = TailNow();
    if (tail < t) tail = t;
    if (silent >= tail) state = 2;
  }
  return state == 2;
}

// ------------------------------------------------------------ engine

Engine::Engine(float r) : rate(r), period(256.0f / r) {
  // Two block buffers, 128-byte aligned, large enough for the SndPlayer's
  // decode scratch (6 channels x 4096 frames).
  constexpr size_t kFloats = 6 * 4096;
  io_mem_.resize(2 * kFloats + 64);
  float* base = reinterpret_cast<float*>((uintptr_t(io_mem_.data()) + 127) & ~uintptr_t(127));
  io.buf[0] = base;
  io.buf[1] = base + kFloats;
}

void Engine::Enqueue(std::function<void()> cmd) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  queue_.push_back(std::move(cmd));
}

void Engine::Drain() {
  std::vector<std::function<void()>> cmds;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    cmds.swap(queue_);
  }
  for (auto& c : cmds) c();
  ++counter;
}

Voice* Engine::CreateVoice(const std::vector<Entry>& entries) {
  auto v = std::make_unique<Voice>();
  int prev = 0;
  for (const Entry& e : entries) {
    std::unique_ptr<Plugin> p;
    switch (e.cls) {
      case PluginClass::kPitch: p = std::make_unique<PitchPlugin>(); break;
      case PluginClass::kLowPass: {
        auto f = std::make_unique<FilterPlugin>(false, e.channels);
        f->voice_rate = rate;
        p = std::move(f);
        break;
      }
      case PluginClass::kHighPass: {
        auto f = std::make_unique<FilterPlugin>(true, e.channels);
        f->voice_rate = rate;
        p = std::move(f);
        break;
      }
      case PluginClass::kGain: p = std::make_unique<GainPlugin>(e.channels); break;
      case PluginClass::kPanSurround: p = std::make_unique<PanPlugin>(prev, e.channels); break;
      case PluginClass::kSend: {
        auto s = std::make_unique<SendPlugin>(prev, e.arg != 0.0f);
        s->s.owner = v.get();
        p = std::move(s);
        break;
      }
      case PluginClass::kSubMix: p = std::make_unique<SubMixPlugin>(e.channels); break;
      case PluginClass::kRoute: {
        auto r = std::make_unique<RoutePlugin>();
        r->r.owner = v.get();
        p = std::move(r);
        break;
      }
      case PluginClass::kReverb: {
        auto r = std::make_unique<ReverbPlugin>(e.channels);
        r->mixer_rate = rate;
        p = std::move(r);
        break;
      }
      case PluginClass::kDelay: {
        auto d = std::make_unique<DelayPlugin>(e.channels);
        d->mixer_rate = rate;
        p = std::move(d);
        break;
      }
      case PluginClass::kVuMeter: p = std::make_unique<VuPlugin>(e.channels, rate); break;
      case PluginClass::kAiffWriter: p = std::make_unique<AiffPlugin>(); break;
      case PluginClass::kDac: p = std::make_unique<DacPlugin>(); break;
      case PluginClass::kSndPlayer:
        p = std::make_unique<SndPlayer>(e.channels, e.arg > 0.0f ? int(e.arg) : 1, this);
        break;
    }
    p->cls = e.cls;
    p->in_channels = prev;
    p->out_channels = e.channels;
    p->voice = v.get();
    prev = e.channels;
    v->plugins.push_back(std::move(p));
  }
  Voice* raw = v.get();
  voices.push_back(std::move(v));
  // Voice::Create queues the link into the active list (0x82650190).
  Enqueue([raw] { raw->linked = true; });
  return raw;
}

void Engine::ReleaseVoice(Voice* v) {
  Enqueue([v] {
    for (auto& p : v->plugins) p->Release();
    v->linked = false;
    v->destroyed = true;
  });
}

void Voice::Stop(bool graceful) {
  if (state == 2) return;
  if (!graceful) {
    state = 2;
  } else if (state == 0) {
    state = 1;
    tail = TailNow();
  }
}

void Engine::StopVoice(Voice* v, bool graceful) {
  Enqueue([v, graceful] { v->Stop(graceful); });
}

void Engine::SetParam(Plugin* p, int index, float value) {
  Enqueue([p, index, value] { p->SetParam(index, value); });
}

void Engine::AttachToBus(Plugin* p, Bus* bus, int src, int dst, int count) {
  Enqueue([p, bus, src, dst, count] { p->AttachToBus(bus, src, dst, count); });
}

// sub_8265A148, written recursively: hooks (n-1..1), then the bus children
// (each starting from the rate saved after the bus voice's hooks), then the
// chain 0..n-1. A voice runs at most once per block (stamp).
void Engine::RunVoice(Voice* v, Io& io, uint32_t stamp, int* ret) {
  if (v->destroyed || v->stamp == stamp || v->IsStopped()) return;
  io.gain = 1.0f;
  v->rate_in = io.rate;
  const int n = int(v->plugins.size());
  for (int i = n - 1; i > 0; --i)
    if (v->plugins[i]->HasHook()) v->plugins[i]->Hook(io, v->rate_in);
  if (Bus* bus = v->plugins[0]->AsBus()) {
    const float saved = io.rate;
    // The guest iterates the live list; the links do not change during a block.
    const std::vector<Bus::Link> links = bus->links;
    for (const Bus::Link& l : links) {
      io.rate = saved;
      RunVoice(l.owner, io, stamp, ret);
    }
  }
  int i;
  for (i = 0; i < n; ++i) {
    Plugin* p = v->plugins[i].get();
    if (!p->HasProcess()) continue;
    const bool first = i >= v->ran_last;
    *ret = p->Process(io, first);
    if (*ret) {
      if (i == 0) v->silent = 0.0f;
      continue;
    }
    if (i != 0) break;
    // The source produced nothing: ring out the tail with silence.
    const float t = v->TailNow();
    if (v->tail < t) v->tail = v->TailNow();
    {
      ScalarScope fpu;
      if (!(v->silent < v->tail)) break;
    }
    std::memset(io.Next(), 0, size_t(p->out_channels) * kBlock * 4);
    io.Toggle();
    *ret = 1;
    ScalarScope fpu;
    v->silent = v->silent + 256.0f;
  }
  v->ran_last = uint8_t(i);
  v->stamp = stamp;
}

int Engine::RenderGraph(Voice* master, Io& io_ref) {
  io_ref.rate = 1.0f;
  int ret = 0;
  RunVoice(master, io_ref, counter, &ret);
  return ret;
}

bool Engine::RenderBlock(Voice* master, float* out) {
  Drain();
  const int ok = RenderGraph(master, io);
  clock += period;
  if (ok == 1) {
    DacFinalize(io.Cur(), out, 6, &dac_fade);
    return true;
  }
  std::memset(out, 0, 6 * kBlock * 4);
  return false;
}

}  // namespace native_audio
