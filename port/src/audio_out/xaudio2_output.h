// Audio output through XAudio2 (Windows' native audio API) instead of the
// SDK's SDL backend (sr_audio_output=xaudio2, the default; =sdl restores SDL).
//
// Same contract as the SDK's SDL driver: the game submits one frame (256
// samples x 6 channels, big-endian float, channel-sequential, 48 kHz), the
// driver converts it with the SDK's conversion/downmix (stereo fold or 5.1
// passthrough, output gain, audio_mute) and queues it on a source voice; when
// XAudio2 has played the buffer, the client semaphore is released once, which
// paces the game's audio thread.
#pragma once

#include <memory>

#include <rex/audio/audio_system.h>

namespace superman_returns::audio_out {

using rex::X_STATUS;

class XAudio2AudioSystem final : public rex::audio::AudioSystem {
 public:
  static std::unique_ptr<rex::audio::AudioSystem> Create(
      rex::runtime::FunctionDispatcher* function_dispatcher);
  explicit XAudio2AudioSystem(rex::runtime::FunctionDispatcher* function_dispatcher);

 protected:
  X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                        rex::audio::AudioDriver** out_driver) override;
  void DestroyDriver(rex::audio::AudioDriver* driver) override;
};

// True when sr_audio_output selects XAudio2.
bool XAudio2Selected();

}  // namespace superman_returns::audio_out
