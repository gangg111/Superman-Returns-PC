// Native SndPlayer "SnP0" (RenderWare Audio Core source plug-in), rewritten
// from docs/natywny-dzwiek/spec-pitch-resampler.md section 3 and the guest
// functions named below. The XMA decoder and the file streaming are replaced
// by a SoundSource: decoded PCM in blocks with the same sample counts as the
// game's data blocks, always available (no starvation, no async reads).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "native_audio/engine.h"
#include "native_audio/resampler.h"

namespace native_audio {

// One sound's audio as the player consumes it: blocks of planar float frames
// (int16 / 32768, exactly what the game's EXm0 decoder produces).
class SoundSource {
 public:
  virtual ~SoundSource() = default;
  virtual int Channels() const = 0;
  // Number of data blocks and the frames in block i (the SNS/SNR block layout).
  virtual int BlockCount() const = 0;
  virtual int BlockFrames(int i) const = 0;
  // Writes `frames` planar frames (channel stride = frames) of block `block`
  // starting at `offset` into dst; returns the frames written.
  virtual int Decode(int block, int offset, float* dst, int frames) = 0;
};

// The start request the game passes to Control(0) (sub_82654A98).
struct PlayRequest {
  double start_time = 0.0;   // req+0
  double stream_offset = 0.0;// req+8
  uint8_t snr[16] = {};      // req+20 -> SNR header bytes
  float flag = 0.0f;         // req+28 (stored as u8: 1 = stop after this request)
  std::shared_ptr<SoundSource> source;
};

class SndPlayer : public Plugin {
 public:
  SndPlayer(int channels, int requests, Engine* engine);
  ~SndPlayer() override;

  int Process(Io& io, bool reset) override;
  double GetParam(int index) const override;
  void SetParam(int index, float v) override;

  // Control(0, req) on the game thread: returns the request id (req+32).
  float Start(const PlayRequest& req);
  // Control(!= 0): stop everything (queued, sub_82654328).
  void Stop();
  // sub_82653E70
  bool IsDone() const;

  struct State {  // for the self-test: guest offsets +100, +40, +1176, +104, +113, +112
    int pos, planner, planned, cur_slot;
    bool declick, have_prev;
    size_t pending;
  };
  State Debug() const {
    return {pos_, planner_, planned_, cur_slot_, declick_, have_prev_, pending_.size()};
  }

  float last_id = 0.0f;  // +32: id of the last started request
  float id_counter = 0.0f;  // +96

 private:
  struct Sound;  // the 88-byte request object
  struct Cmd;    // one record of the command list (+128)
  struct Slot {  // stream slot, pl+296 + 44*i
    int state = 0;  // 0 free, 1 opened, 2 active, 3 ended
    int pos = 0;    // +4 frames consumed
    int end = 0;    // +16 frames in the block
    int block = -1; // the block it holds
    Sound* sound = nullptr;
  };
  void Setup(const PlayRequest& req, float id);  // mixer side of Start (sub_82654620)
  void StopAll();                                // sub_82654328
  void Finish();                                 // command 1: sub_82653998
  bool OpenDecoder(Sound* s);                    // sub_82654FF0
  int OpenBlock(Sound* s, int block, bool first);// sub_82654958, returns the next block
  bool LoopJump(Sound* s);                       // sub_82654CB0
  bool EndOfData(Sound* s, bool* finished);      // sub_82654DD0
  bool NextBlock(Sound* s);                      // sub_82654BE0 (stream refill)
  bool StartNow(Io& io, double start_time, int* produced);      // sub_82653880
  int Plan(Io& io, int mode, int frames, int* produced);       // sub_82655170
  void Execute(Io& io, int* produced);                          // sub_82654060
  int Segment(Io& io, Sound* s, int slot, int out_cnt, int in_cnt, int out_off, int* decoded,
              int* produced, float rate);                       // sub_82653B20
  int FreeSlot() const;

  Engine* engine_;
  int channels_;           // +29
  std::vector<std::unique_ptr<Sound>> pool_;
  std::vector<Sound*> free_;     // +72 (head = back)
  std::vector<Sound*> pending_;  // +84 (head = front)
  Slot slots_[20];
  bool decoder_used_[20] = {};   // +140 + 8*i
  Resampler rs_;                 // +124
  std::vector<Cmd> cmds_;        // +128 / +132
  int pos_ = 0;                  // +100 frames
  int cur_slot_ = -1;            // +104
  float last_[6] = {};           // +108
  bool have_prev_ = false;       // +112
  bool declick_ = false;         // +113
  int planner_ = 0;              // +40
  bool error_ = false;           // +36
  float param0_ = 0.0f;          // +48
  double pos_sec_ = 0.0;         // +56 (param 1)
  double len_sec_ = 0.0;         // +64 (param 2)
  int planned_ = 0;              // +1176
};

}  // namespace native_audio
