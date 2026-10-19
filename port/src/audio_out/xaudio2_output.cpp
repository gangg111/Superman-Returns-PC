#include "audio_out/xaudio2_output.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <xaudio2.h>
#include <mmreg.h>

#include <rex/audio/audio_driver.h>
#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(sr_audio_output, "xaudio2", "Superman Returns",
                      "Audio output: xaudio2 (Windows XAudio2) or sdl (the SDK's SDL backend)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(sr_audio_output_dump, "", "Superman Returns",
                      "Debug: WAV (float) of exactly what is handed to XAudio2, after conversion")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace superman_returns::audio_out {

namespace {

constexpr uint32_t kRate = 48000;
// Speaker masks and the IEEE float subtype (ksmedia.h values; ks.h is left
// out because its GUID_NULL macro breaks the cguid.h that xaudio2.h pulls in).
constexpr DWORD kSpeakersStereo = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
constexpr DWORD kSpeakers51 = kSpeakersStereo | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                              SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
constexpr GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010,
                                {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr uint32_t kFrameSamples = 256;  // per channel

// Float WAV of the converted output, header rewritten about once a second so
// the file stays valid when the process is killed.
class OutputDump {
 public:
  void Open(uint16_t channels) {
    const std::string path = REXCVAR_GET(sr_audio_output_dump);
    if (path.empty()) return;
    file_ = std::fopen(path.c_str(), "wb");
    channels_ = channels;
    if (file_) WriteHeader();
  }
  void Write(const float* data, size_t floats) {
    if (!file_) return;
    std::fwrite(data, sizeof(float), floats, file_);
    bytes_ += uint32_t(floats * sizeof(float));
    if (++frames_ % 188 == 0) {
      WriteHeader();
      std::fseek(file_, 0, SEEK_END);
      std::fflush(file_);
    }
  }
  ~OutputDump() {
    if (!file_) return;
    WriteHeader();
    std::fclose(file_);
  }

 private:
  void WriteHeader() {
    const uint16_t format = 3, bits = 32, align = uint16_t(channels_ * 4);
    const uint32_t fmt_size = 16, byte_rate = kRate * align, riff = 36 + bytes_;
    std::fseek(file_, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, file_);
    std::fwrite(&riff, 4, 1, file_);
    std::fwrite("WAVEfmt ", 1, 8, file_);
    std::fwrite(&fmt_size, 4, 1, file_);
    std::fwrite(&format, 2, 1, file_);
    std::fwrite(&channels_, 2, 1, file_);
    std::fwrite(&kRate, 4, 1, file_);
    std::fwrite(&byte_rate, 4, 1, file_);
    std::fwrite(&align, 2, 1, file_);
    std::fwrite(&bits, 2, 1, file_);
    std::fwrite("data", 1, 4, file_);
    std::fwrite(&bytes_, 4, 1, file_);
  }
  std::FILE* file_ = nullptr;
  uint16_t channels_ = 2;
  uint32_t bytes_ = 0, frames_ = 0;
};

class XAudio2Driver final : public rex::audio::AudioDriver,
                            public IXAudio2VoiceCallback,
                            public IXAudio2EngineCallback {
 public:
  XAudio2Driver(rex::memory::Memory* memory, rex::thread::Semaphore* semaphore)
      : AudioDriver(memory), semaphore_(semaphore) {}
  ~XAudio2Driver() override { Shutdown(); }

  bool Initialize() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com_initialized_ = SUCCEEDED(co);
    if (!CreateEngine()) return false;
    dump_.Open(channels_);
    return true;
  }

  void Shutdown() {
    DestroyEngine();
    for (float* b : all_buffers_) delete[] b;
    all_buffers_.clear();
    free_buffers_.clear();
    if (com_initialized_) CoUninitialize();
    com_initialized_ = false;
  }

  void SubmitFrame(uint32_t frame_ptr) override {
    if (device_lost_.exchange(false)) Recover();
    const auto input = memory_->TranslateVirtual<const float*>(frame_ptr);
    float* out = TakeBuffer();
    const size_t floats = size_t(kFrameSamples) * channels_;
    if (rex::cvar::Query<bool>("audio_mute")) {
      std::memset(out, 0, floats * sizeof(float));
    } else if (channels_ == 6) {
      rex::audio::conversion::sequential_6_BE_to_interleaved_6_LE(
          out, input, kFrameSamples, rex::audio::GetSurroundMix(), rex::audio::GetOutputGain());
    } else {
      rex::audio::conversion::sequential_6_BE_to_interleaved_2_LE(
          out, input, kFrameSamples, rex::audio::GetStereoFold(), rex::audio::GetOutputGain());
    }
    dump_.Write(out, floats);
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = UINT32(floats * sizeof(float));
    buffer.pAudioData = reinterpret_cast<const BYTE*>(out);
    buffer.pContext = out;
    ++queued_;
    submitted_any_ = true;
    if (!source_ || FAILED(source_->SubmitSourceBuffer(&buffer))) {
      // Never leave the game waiting for a buffer XAudio2 will not play.
      --queued_;
      ReturnBuffer(out);
      semaphore_->Release(1, nullptr);
    }
  }

  // IXAudio2VoiceCallback (XAudio2's thread: no blocking work).
  void STDMETHODCALLTYPE OnBufferEnd(void* context) override {
    ReturnBuffer(static_cast<float*>(context));
    --queued_;
    semaphore_->Release(1, nullptr);
    const uint64_t played = ++played_;
    if (played % 1875 == 0)  // about every 10 s
      REXAPU_INFO("XAudio2: {} buffers played, {} starved passes, {} queued", played,
                  starved_.load(), queued_.load());
  }
  void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32 bytes_required) override {
    if (bytes_required && submitted_any_ && queued_.load() == 0) ++starved_;
  }
  void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
  void STDMETHODCALLTYPE OnStreamEnd() override {}
  void STDMETHODCALLTYPE OnBufferStart(void*) override {}
  void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
  void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) override {
    REXAPU_WARN("XAudio2: voice error 0x{:08X}", uint32_t(error));
  }

  // IXAudio2EngineCallback: the device went away (unplugged, default changed).
  void STDMETHODCALLTYPE OnProcessingPassStart() override {}
  void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
  void STDMETHODCALLTYPE OnCriticalError(HRESULT error) override {
    REXAPU_WARN("XAudio2: critical error 0x{:08X}, recreating the output", uint32_t(error));
    device_lost_ = true;
  }

 private:
  bool CreateEngine() {
    HRESULT hr = XAudio2Create(&xaudio2_, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
      REXAPU_ERROR("XAudio2Create failed: 0x{:08X}", uint32_t(hr));
      return false;
    }
    xaudio2_->RegisterForCallbacks(this);
    hr = xaudio2_->CreateMasteringVoice(&master_);
    if (FAILED(hr)) {
      REXAPU_ERROR("XAudio2 mastering voice failed: 0x{:08X}", uint32_t(hr));
      return false;
    }
    DWORD mask = 0;
    master_->GetChannelMask(&mask);
    XAUDIO2_VOICE_DETAILS details{};
    master_->GetVoiceDetails(&details);
    // 5.1 passthrough only to an endpoint that has the 5.1 speakers; anything
    // smaller gets the SDK's stereo fold (as the SDL backend does).
    channels_ = (details.InputChannels >= 6 && (mask & kSpeakers51) == kSpeakers51)
                    ? 6 : 2;
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = WORD(channels_);
    format.Format.nSamplesPerSec = kRate;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = WORD(channels_ * 4);
    format.Format.nAvgBytesPerSec = kRate * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = channels_ == 6 ? kSpeakers51 : kSpeakersStereo;
    format.SubFormat = kSubtypeFloat;
    hr = xaudio2_->CreateSourceVoice(&source_, &format.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, this);
    if (FAILED(hr)) {
      REXAPU_ERROR("XAudio2 source voice failed: 0x{:08X}", uint32_t(hr));
      return false;
    }
    source_->Start(0);
    REXAPU_INFO("XAudio2: endpoint {} ch at {} Hz (mask 0x{:X}); submitting {} ch float at {} Hz",
                details.InputChannels, details.InputSampleRate, uint32_t(mask), channels_, kRate);
    return true;
  }

  void DestroyEngine() {
    // DestroyVoice waits for the voice's callbacks to finish.
    if (source_) source_->DestroyVoice();
    source_ = nullptr;
    if (master_) master_->DestroyVoice();
    master_ = nullptr;
    if (xaudio2_) {
      xaudio2_->UnregisterForCallbacks(this);
      xaudio2_->Release();
    }
    xaudio2_ = nullptr;
  }

  // Game thread, after a critical error: the buffers still queued on the old
  // voice will never end, so hand their semaphore counts back before going on.
  void Recover() {
    DestroyEngine();
    const int lost = queued_.exchange(0);
    if (lost > 0) semaphore_->Release(lost, nullptr);
    {
      std::lock_guard lock(mutex_);
      free_buffers_.assign(all_buffers_.begin(), all_buffers_.end());
    }
    if (!CreateEngine()) REXAPU_ERROR("XAudio2: the output could not be recreated");
  }

  float* TakeBuffer() {
    std::lock_guard lock(mutex_);
    if (free_buffers_.empty()) {
      all_buffers_.push_back(new float[size_t(kFrameSamples) * 6]);
      return all_buffers_.back();
    }
    float* b = free_buffers_.back();
    free_buffers_.pop_back();
    return b;
  }
  void ReturnBuffer(float* b) {
    std::lock_guard lock(mutex_);
    free_buffers_.push_back(b);
  }

  rex::thread::Semaphore* semaphore_;
  IXAudio2* xaudio2_ = nullptr;
  IXAudio2MasteringVoice* master_ = nullptr;
  IXAudio2SourceVoice* source_ = nullptr;
  uint32_t channels_ = 2;
  bool com_initialized_ = false;
  std::mutex mutex_;
  std::vector<float*> all_buffers_, free_buffers_;
  std::atomic<int> queued_{0};
  std::atomic<uint64_t> played_{0}, starved_{0};
  std::atomic<bool> submitted_any_{false}, device_lost_{false};
  OutputDump dump_;
};

}  // namespace

bool XAudio2Selected() {
  return REXCVAR_GET(sr_audio_output) != "sdl";
}

std::unique_ptr<rex::audio::AudioSystem> XAudio2AudioSystem::Create(
    rex::runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<XAudio2AudioSystem>(function_dispatcher);
}

XAudio2AudioSystem::XAudio2AudioSystem(rex::runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

X_STATUS XAudio2AudioSystem::CreateDriver(size_t, rex::thread::Semaphore* semaphore,
                                          rex::audio::AudioDriver** out_driver) {
  auto driver = std::make_unique<XAudio2Driver>(memory_, semaphore);
  if (!driver->Initialize()) return X_STATUS_UNSUCCESSFUL;
  *out_driver = driver.release();
  return X_STATUS_SUCCESS;
}

void XAudio2AudioSystem::DestroyDriver(rex::audio::AudioDriver* driver) {
  delete driver;
}

}  // namespace superman_returns::audio_out
