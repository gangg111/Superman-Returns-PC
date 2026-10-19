// Native replacement of RenderWare Audio Core's System, voices and voice graph
// executor (docs/natywny-dzwiek/spec-pitch-resampler.md section 1). The DSP of
// the plug-ins lives in plugins.h / effects.h; this file owns the graph:
// creation, the command queue drained once per block, and the per-block walk
// from the master voice through every bus to its sender voices, in the
// original order (the order of the bus sums decides the rounding).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "native_audio/effects.h"
#include "native_audio/plugins.h"

namespace native_audio {

class Voice;

// FourCC of the 14 classes the game uses.
enum class PluginClass : uint32_t {
  kSndPlayer = 0x536E5030,  // SnP0
  kPitch = 0x50697430,      // Pit0
  kLowPass = 0x4C493230,    // LI20
  kHighPass = 0x48493230,   // HI20
  kGain = 0x47616930,       // Gai0
  kPanSurround = 0x506E5330,// PnS0
  kSend = 0x53656E30,       // Sen0
  kSubMix = 0x53756230,     // Sub0
  kRoute = 0x526F7530,      // Rou0
  kReverb = 0x524D3130,     // RM10
  kDelay = 0x44656C30,      // Del0
  kVuMeter = 0x56754D30,    // VuM0
  kAiffWriter = 0x41695730, // AiW0
  kDac = 0x44616330,        // Dac0
};

// One plug-in instance in a voice chain.
class Plugin {
 public:
  virtual ~Plugin() = default;
  // Process(plugin, io, reset): 1 = block valid, 0 = nothing produced.
  virtual int Process(Io& io, bool reset) = 0;
  // Pitch and Dac have no Process (descriptor +24 = 0): the executor skips them.
  virtual bool HasProcess() const { return true; }
  // Descriptor +20 hook, called before Process from the last plug-in back to index 1.
  virtual bool HasHook() const { return false; }
  virtual void Hook(Io& /*io*/, float /*voice_rate_in*/) {}
  // +24: tail in frames (summed into the voice tail).
  virtual float Latency() const { return 0.0f; }
  virtual void SetParam(int /*index*/, float /*value*/) {}
  virtual double GetParam(int /*index*/) const { return 0.0; }
  virtual Bus* AsBus() { return nullptr; }
  // Send/Route Control handlers: detach (with residual), attach to `bus`.
  virtual void AttachToBus(Bus* /*bus*/, int /*src*/, int /*dst*/, int /*count*/) {}
  // vtbl[0], called when the voice is destroyed (sub_826501D0).
  virtual void Release() {}

  PluginClass cls;
  int in_channels = 0;   // +28
  int out_channels = 0;  // +29
  Voice* voice = nullptr;
};

class Voice {
 public:
  std::vector<std::unique_ptr<Plugin>> plugins;  // +72, index 0 = source
  uint8_t state = 0;      // +70: 0 playing, 1 stopping (tail), 2 stopped
  uint8_t ran_last = 0;   // +69
  float silent = 0.0f;    // +48
  float tail = 0.0f;      // +52
  float rate_in = 0.0f;   // +44
  uint32_t stamp = ~0u;   // +56
  bool linked = false;    // in the System's active list (+96)
  bool destroyed = false;

  float TailNow() const;  // sub_82650018: 0 + lat[1] + ... + lat[n-1]
  void Stop(bool graceful);  // sub_826502A8 applied directly (mixer thread)
  bool IsStopped();       // sub_826500F8
};

class Engine {
 public:
  explicit Engine(float rate = 48000.0f);

  // Guest-thread side: everything that changes mixer state goes through the
  // queue and is applied at the start of the next block, like System+32.
  void Enqueue(std::function<void()> cmd);
  // Mixer side, once per block (sub_8264C540): runs the queue, counter++.
  void Drain();

  // Voice::Create: entries {class, output channels, creation argument}.
  struct Entry {
    PluginClass cls;
    int channels;
    float arg = 0.0f;  // args[0]: Sen0 ignore voice gain, SnP0 request count
  };
  Voice* CreateVoice(const std::vector<Entry>& entries);
  // Voice::Release (sub_826505B8): queued destroy, every plug-in's release.
  void ReleaseVoice(Voice* v);
  // sub_826502A8: stop now (graceful = false) or let the tail ring out.
  void StopVoice(Voice* v, bool graceful);
  // SetParamF / Control: queued, applied at the next Drain().
  void SetParam(Plugin* p, int index, float value);
  void AttachToBus(Plugin* p, Bus* bus, int src = 0, int dst = 0, int count = 0);

  // Walks the graph from `master` for one block (sub_8265A148).
  int RenderGraph(Voice* master, Io& io);
  // Full block as the Dac thread does it: drain, render, clock, finalize.
  // `out` = 256 interleaved frames of 6 floats in the XAudio order.
  bool RenderBlock(Voice* master, float* out);

  float rate;
  float period;
  double clock = 0.0;     // System+0
  uint32_t counter = 1;   // System+20 (1 at create)
  bool dac_fade = false;  // Dac+144
  std::vector<std::unique_ptr<Voice>> voices;

 private:
  void RunVoice(Voice* v, Io& io, uint32_t stamp, int* ret);
  std::mutex queue_mutex_;
  std::vector<std::function<void()>> queue_;
  std::vector<float> io_mem_;  // the two block buffers of io
 public:
  Io io;
};

}  // namespace native_audio
