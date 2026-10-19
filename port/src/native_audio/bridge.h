// Bridge between the recompiled game and the native audio engine.
//
// SR_NATIVE_AUDIO=shadow: the original engine keeps playing; every boundary
// call (docs/natywny-dzwiek-api.md) is mirrored into the native engine, which
// renders the same block right after the original (same drain, same stamp,
// same clock) and the master bus outputs are compared sample by sample. The
// native SndPlayers take the samples the game's own decoder produced
// ("capture"), so any difference is the engine's, not the decoder's.
// Statistics go to the log every 1000 blocks (about 5.3 s).
//
// SR_NATIVE_AUDIO=shadow-bank: the same, but the native players use the
// converted sounds (sample bank) instead of the capture: measures how far the
// offline-decoded PCM is from the game's XMA decoder.
// SR_NATIVE_AUDIO=native: the speakers get the native mix (bank sounds); the
// original engine still runs alongside for the state the game reads.
// SR_NATIVE_AUDIO=full: the original engine no longer renders or decodes; the
// game's state queries (IsDone, GetParam, IsReleaseDone, latency) are answered
// by the native engine.
#pragma once

#include <cstdint>

namespace native_audio::bridge {

bool Active();

// Called from the boundary hooks (port/src/audio_api_log.cpp), after the original.
void OnVoiceCreate(uint8_t* base, uint32_t count, uint32_t entries, uint32_t guest_voice);
void OnVoiceRelease(uint8_t* base, uint32_t guest_voice);
void OnSetParam(uint8_t* base, uint32_t plugin, uint32_t index, float value);
void OnControl(uint8_t* base, uint32_t plugin, uint32_t index, uint32_t data);
void OnIsDone(uint8_t* base, uint32_t plugin, bool guest_answer);
// full mode: the native SndPlayer's IsDone for a mapped player, else -1.
int IsDoneOverride(uint32_t plugin);

// From the EXm0 decoder hooks (native_audio/selftest.cpp), after the original.
void OnSetInput(uint8_t* base, uint32_t decoder, uint32_t slot);
void OnDecode(uint8_t* base, uint32_t decoder, uint32_t dst, int frames);

}  // namespace native_audio::bridge
