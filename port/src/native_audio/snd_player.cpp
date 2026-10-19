#include "native_audio/snd_player.h"

#include <bit>
#include <cstring>

namespace native_audio {

namespace {

enum CmdType : uint8_t { kResample = 0, kFinish = 1, kGraceful = 2, kSilence = 3, kSetPos = 4 };

// sub_8265AD70 ramp, used for the declick
void Declick(float* out, float* last, int ch) { ResidualFade(out, last, ch, kBlock); }

class BitReader {  // the SNR header reader at player+116 (sub_826534D8)
 public:
  explicit BitReader(const uint8_t* p) : p_(p) {}
  uint32_t Get(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i, ++pos_) v = (v << 1) | ((p_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1);
    return v;
  }

 private:
  const uint8_t* p_;
  int pos_ = 0;
};

}  // namespace

// The 88-byte request object (offsets in comments).
struct SndPlayer::Sound {
  Sound* next = nullptr;
  double start = 0.0;       // +8
  double offset = 0.0;      // +16
  float id = 0.0f;          // +24
  uint32_t rate = 0;        // +32
  int32_t samples = 0;      // +36
  int32_t loop_start = -1;  // +40
  int32_t loop_off = 0;     // +44
  int32_t ram_part = 0;     // +48
  uint8_t codec = 0;        // +72
  uint8_t channels = 0;     // +73
  uint8_t type = 0;         // +74: 0 RAM, 1 stream, 2 RAM head + stream
  uint8_t stop_after = 0;   // +75
  uint8_t started = 0;      // +76
  uint8_t dec_slot = 0;     // +77
  uint8_t stream_slot = 0;  // +78
  int next_block = 0;       // +80
  int loop_block = 0;       // +84
  int stream_block = 0;     // next streamed block (the file reads of the original)
  std::shared_ptr<SoundSource> src;
};

struct SndPlayer::Cmd {
  CmdType type;
  bool last = false;
  uint8_t slot = 0;
  uint16_t out_off = 0, out_cnt = 0, in_cnt = 0;
  int32_t pos = 0;
  float rate = 0.0f;
  SndPlayer::Sound* sound = nullptr;
};

SndPlayer::SndPlayer(int ch, int requests, Engine* engine)
    : engine_(engine), channels_(ch), rs_(ch) {
  for (int i = 0; i < (requests > 0 ? requests : 1); ++i) {
    pool_.push_back(std::make_unique<Sound>());
    free_.push_back(pool_.back().get());
  }
  last_id = -1.0f;  // +32 at Create
}

SndPlayer::~SndPlayer() = default;

double SndPlayer::GetParam(int index) const {
  return index == 0 ? param0_ : index == 1 ? pos_sec_ : index == 2 ? len_sec_ : 0.0;
}

void SndPlayer::SetParam(int index, float v) {
  if (index == 0) param0_ = v;
}

bool SndPlayer::IsDone() const { return !(last_id < id_counter) && pending_.empty(); }

// ---------------------------------------------------------------- control

float SndPlayer::Start(const PlayRequest& req) {
  // Game thread (sub_82654A98): the id counter wraps at 4194304 (0x82081D24).
  id_counter = id_counter + 1.0f;
  if (id_counter > 4194304.0f) id_counter = 1.0f;
  const float id = id_counter;
  engine_->Enqueue([this, req, id] { Setup(req, id); });
  return id;
}

void SndPlayer::Stop() {
  engine_->Enqueue([this] { StopAll(); });
}

void SndPlayer::Setup(const PlayRequest& req, float id) {
  if (free_.empty()) return;  // the original drops the request
  Sound* s = free_.back();
  free_.pop_back();
  *s = Sound{};
  s->id = id;
  last_id = id;
  planner_ = 1;
  s->start = req.start_time;
  s->offset = req.stream_offset;
  s->stop_after = uint8_t(int64_t(req.flag));
  s->src = req.source;
  // SNR header (sub_82653738)
  BitReader br(req.snr);
  br.Get(4);
  s->codec = uint8_t(br.Get(4));
  s->channels = uint8_t(br.Get(6) + 1);
  s->rate = br.Get(18);
  s->type = uint8_t(br.Get(2));
  const bool loop = br.Get(1) != 0;
  s->samples = int32_t(br.Get(29));
  s->loop_start = loop ? int32_t(br.Get(32)) : -1;
  if (s->type == 2) s->ram_part = int32_t(br.Get(32));
  if (loop) {
    if (s->type == 1 || (s->type == 2 && s->loop_start >= s->ram_part))
      s->loop_off = int32_t(br.Get(32));
    else
      s->loop_off = 0;
  }
  // decoder slot (+140 table)
  int i = 0;
  while (i < 20 && decoder_used_[i]) ++i;
  if (i < 20) decoder_used_[i] = true;
  s->dec_slot = uint8_t(i);
  pending_.push_back(s);
  error_ = false;
}

void SndPlayer::StopAll() {
  while (!pending_.empty()) Finish();
  cmds_.clear();
  declick_ = true;
  planner_ = 3;
}

void SndPlayer::Finish() {
  Sound* s = pending_.front();
  pending_.erase(pending_.begin());
  param0_ = s->id;
  pos_ = 0;
  pos_sec_ = 0.0;
  len_sec_ = 0.0;
  for (Slot& sl : slots_)
    if (sl.sound == s) {
      sl.state = 0;
      sl.sound = nullptr;
    }
  if (s->dec_slot < 20) decoder_used_[s->dec_slot] = false;
  s->src.reset();
  free_.push_back(s);
}

// ---------------------------------------------------------------- data

int SndPlayer::FreeSlot() const {
  int i = 0;
  while (i < 20 && slots_[i].state != 0) ++i;
  return i;
}

int SndPlayer::OpenBlock(Sound* s, int block, bool first) {
  (void)first;
  Slot& sl = slots_[s->stream_slot];
  sl.state = 1;
  sl.pos = 0;
  sl.block = block;
  sl.end = (s->src && block < s->src->BlockCount()) ? s->src->BlockFrames(block) : 0;
  sl.sound = s;
  return block + 1;
}

bool SndPlayer::NextBlock(Sound* s) {  // sub_82654BE0: next streamed block, always ready here
  const int i = FreeSlot();
  if (i >= 20 || !s->src || s->stream_block >= s->src->BlockCount()) return false;
  s->stream_slot = uint8_t(i);
  OpenBlock(s, s->stream_block++, true);
  return true;
}

bool SndPlayer::OpenDecoder(Sound* s) {  // sub_82654FF0
  if (!s->src) return false;
  if (s->type == 0 || s->type == 2) {
    s->stream_slot = uint8_t(FreeSlot());
    s->next_block = OpenBlock(s, 0, true);
    s->stream_block = s->next_block;
    return true;
  }
  return NextBlock(s);
}

bool SndPlayer::LoopJump(Sound* s) {  // sub_82654CB0
  if (s->type == 0) {
    s->loop_block = s->next_block;
    s->stream_slot = uint8_t(FreeSlot());
    s->next_block = OpenBlock(s, s->loop_block, true);
    return true;
  }
  if (s->type == 1) return NextBlock(s);
  if (s->loop_start < s->ram_part) {
    s->loop_block = s->next_block;
    s->stream_slot = uint8_t(FreeSlot());
    s->next_block = OpenBlock(s, s->loop_block, true);
    return true;
  }
  return NextBlock(s);
}

bool SndPlayer::EndOfData(Sound* s, bool* finished) {  // sub_82654DD0
  if (s->loop_start < 0) {
    *finished = true;
    return true;
  }
  *finished = false;
  if (s->type == 0) {
    if (s->loop_start == 0) s->loop_block = 0;
    s->stream_slot = uint8_t(FreeSlot());
    s->next_block = OpenBlock(s, s->loop_block, true);
    return true;
  }
  // Streams restart reading at the loop: the block that starts at loop_start.
  int b = 0, at = 0;
  while (s->src && b < s->src->BlockCount() && at < s->loop_start) at += s->src->BlockFrames(b++);
  s->stream_block = b;
  return NextBlock(s);
}

// ---------------------------------------------------------------- planner

bool SndPlayer::StartNow(Io& io, double start_time, int* produced) {  // sub_82653880
  // all in double: (start - (produced / rate + clock)), then fctiwz(rate * d)
  const double now = double(*produced) / double(engine_->rate) + engine_->clock;
  const double d = start_time - now;
  if (!(d > 0.0)) return true;
  const double x = double(engine_->rate) * d;
  const int delay = x >= 2147483648.0 ? INT32_MAX : int(x);
  if (delay >= 256 - *produced) return false;
  float* dst = io.Next();
  const float* cur = io.Cur();
  if (*produced > 0) std::memcpy(dst, cur, size_t(*produced) * channels_ * 4);
  std::memset(dst + *produced * channels_, 0, size_t(delay) * channels_ * 4);
  *produced += delay;
  io.Toggle();
  return true;
}

int SndPlayer::Plan(Io& io, int mode, int n_frames, int* produced) {
  std::vector<Cmd>& out = cmds_;
  Sound* s = pending_.empty() ? nullptr : pending_.front();
  size_t si = 0;
  int done = 0;
  int pos = pos_;
  int delay = 0;
  auto next_sound = [&]() -> Sound* {
    ++si;
    return si < pending_.size() ? pending_[si] : nullptr;
  };
  for (;;) {
    if (!s) {
      if (planner_ == 2) planner_ = 3;
      return done;
    }
    while (s->samples == 0) {
      out.push_back({kFinish, false, 0, 0, 0, 0, 0, 0.0f, s});
      if (s->stop_after == 1) {
        out.push_back({kGraceful, false, 0, 0, 0, 0, 0, 0.0f, nullptr});
        planner_ = 3;
        return done;
      }
      s = next_sound();
      if (!s) {
        planner_ = 3;
        return done;
      }
    }
    float rate;
    {
      ScalarScope fpu;
      rate = (io.rate / engine_->rate) * float(double(uint64_t(s->rate)));
    }
    if (!s->started) {
      if (mode == 0) {
        if (!StartNow(io, s->start, produced)) return done;
      } else {
        const double f11 = 256.0 / double(engine_->rate);
        double d = s->start - (f11 + engine_->clock);
        if (d < 0.0) d = 0.0;
        const double x = double(engine_->rate) * d;
        const int dl = (x >= 2147483648.0) ? INT32_MAX : int(x);
        if (dl >= 256) return done;
        delay = dl;
        if (done < delay) {
          Cmd c{kSilence};
          c.sound = s;
          c.out_off = uint16_t(done);
          c.out_cnt = uint16_t(delay - done);
          out.push_back(c);
          done = delay;
        }
      }
      if (!OpenDecoder(s)) return done;
      s->started = 1;
      pos = 0;
      Cmd c{kSetPos};
      c.sound = s;
      c.pos = 0;
      out.push_back(c);
    }
    planner_ = 2;
    int consumed = 0;
    bool more = true;
    while (more) {
      const int rem = n_frames - done;
      const int need = rs_.InputsFor(rate, rem);
      Slot& sl = slots_[s->stream_slot];
      const int avail = sl.end - sl.pos;
      if (need < avail) {
        rs_.Skip(rate, rem, need);
        Cmd c{kResample};
        c.last = false;
        c.slot = s->stream_slot;
        c.out_off = uint16_t(done);
        c.out_cnt = uint16_t(rem);
        c.in_cnt = uint16_t(need);
        c.rate = rate;
        c.sound = s;
        out.push_back(c);
        done += rem;
        consumed += need;
        more = false;
        break;
      }
      int outn = rs_.OutputsFor(avail, rate);
      if (outn > rem) outn = rem;
      rs_.Skip(rate, outn, avail);
      Cmd c{kResample};
      c.last = true;
      c.slot = s->stream_slot;
      c.out_off = uint16_t(done);
      c.out_cnt = uint16_t(outn);
      c.in_cnt = uint16_t(avail);
      c.rate = rate;
      c.sound = s;
      out.push_back(c);
      consumed += avail;
      const int p = consumed + pos;
      done += outn;
      bool ok = true;
      if (p == s->loop_start) {
        ok = LoopJump(s);
      } else if (p == s->samples) {
        bool finished = false;
        if (!EndOfData(s, &finished)) {
          error_ = true;
          planner_ = 3;
          return done;
        }
        if (!finished) {
          Cmd sp{kSetPos};
          sp.sound = s;
          sp.pos = s->loop_start;
          out.push_back(sp);
          consumed = 0;
          pos = s->loop_start;
        } else {
          out.push_back({kFinish, false, 0, 0, 0, 0, 0, 0.0f, s});
          planner_ = 3;
          if (s->stop_after == 1) {
            out.push_back({kGraceful, false, 0, 0, 0, 0, 0, 0.0f, nullptr});
            return done;
          }
          s = next_sound();
          more = false;
        }
      } else {
        ok = NextBlock(s);
      }
      if (!ok) {
        error_ = true;
        planner_ = 3;
        return done;
      }
      if (outn == rem) more = false;
    }
    if (done >= n_frames) return done;
  }
}

// ---------------------------------------------------------------- execution

int SndPlayer::Segment(Io& io, Sound* s, int slot, int out_cnt, int in_cnt, int out_off,
                       int* decoded, int* produced, float rate) {  // sub_82653B20
  const int pre = channels_ * out_off;
  float* sbuf = io.Next();
  // Decode into the scratch after the frames already produced this block. The
  // game's decoder consumes the blocks queued to it in order; the command
  // carries the slot of the block it was planned on.
  const Slot& sl = slots_[slot];
  int got = 0;
  if (s->src) got = s->src->Decode(sl.block, sl.pos, sbuf + pre, in_cnt);
  *decoded = got;
  *produced = (got != in_cnt) ? rs_.OutputsFor(got, rate) : out_cnt;
  if (pre) std::memcpy(sbuf, io.Cur(), size_t(pre) * 4);
  io.Toggle();
  float* in = io.Cur();
  if (channels_ != s->channels && got) {
    float* t = io.Next();
    if (pre) std::memcpy(t, in, size_t(pre) * 4);
    MapChannels(t + pre, channels_, in + pre, s->channels, got, 1.0f, 0.0f, false, false);
    io.Toggle();
    in = io.Cur();
  }
  float* o = (*produced + pre) ? io.Next() : nullptr;
  if (rs_.Process(o ? o + pre : nullptr, in + pre, *produced, got, rate) && o) {
    if (pre) std::memcpy(o, in, size_t(pre) * 4);
    io.Toggle();
  }
  if (pre) {
    float* m = io.Next();
    const float* cb = io.Cur();
    const int n = *produced + out_off;
    for (int c = 0; c < channels_; ++c) {
      std::memcpy(m + c * n, cb + c * out_off, size_t(out_off) * 4);
      std::memcpy(m + c * n + out_off, cb + pre + c * *produced, size_t(*produced) * 4);
    }
    io.Toggle();
  }
  return 0;
}

void SndPlayer::Execute(Io& io, int* produced) {  // sub_82654060
  std::vector<Cmd>& list = cmds_;
  for (const Cmd& c : list) {
    switch (c.type) {
      case kResample: {
        if (cur_slot_ != c.slot) {
          cur_slot_ = c.slot;
          slots_[c.slot].state = 2;
        }
        int dec = 0, prod = 0;
        Segment(io, c.sound, c.slot, c.out_cnt, c.in_cnt, c.out_off, &dec, &prod, c.rate);
        pos_ += dec;
        slots_[c.slot].pos += dec;
        *produced += prod;
        if (c.last) slots_[c.slot].state = 3;
        break;
      }
      case kFinish:
        Finish();
        cur_slot_ = -1;
        break;
      case kGraceful:  // sub_826502A8(voice, -1), right here on the mixer thread
        if (voice) voice->Stop(true);
        cur_slot_ = -1;
        break;
      case kSilence: {
        float* dst = io.Next();
        const float* cur = io.Cur();
        if (c.out_off) std::memcpy(dst, cur, size_t(c.out_off) * channels_ * 4);
        std::memset(dst + c.out_off * channels_, 0, size_t(c.out_cnt) * channels_ * 4);
        io.Toggle();
        *produced += c.out_cnt;
        break;
      }
      case kSetPos:
        pos_ = c.pos;
        if (c.pos == 0) {
          param0_ = c.sound->id;
          pos_sec_ = 0.0;
          len_sec_ = double(c.sound->samples) / double(c.sound->rate);
        }
        break;
    }
  }
}

int SndPlayer::Process(Io& io, bool) {  // sub_82655610
  // sub_82653600: free the slots whose block ended
  for (Slot& sl : slots_)
    if (sl.state == 3) {
      sl.state = 0;
      sl.sound = nullptr;
    }
  int produced = 0;
  Execute(io, &produced);
  cmds_.clear();
  Resampler::Snapshot snap = rs_.Save();
  if (produced < 256) {
    planned_ = Plan(io, 0, 256 - produced, &produced);
    rs_.Restore(snap);
    Execute(io, &produced);
    cmds_.clear();
  }
  snap = rs_.Save();
  planned_ = Plan(io, 1, 256, &produced);
  rs_.Restore(snap);
  if (produced == 0 && !declick_) return 0;
  if (!pending_.empty())
    pos_sec_ = double(int32_t(pos_)) / double(uint32_t(pending_.front()->rate));
  if (produced < 256) {
    const float* src = io.Cur();
    float* dst = io.Next();
    for (int c = 0; c < channels_; ++c) {
      std::memcpy(dst + c * 256, src + c * produced, size_t(produced) * 4);
      std::memset(dst + c * 256 + produced, 0, size_t(256 - produced) * 4);
    }
    io.Toggle();
  }
  if (declick_ && have_prev_) {
    Declick(io.Cur(), last_, channels_);
    declick_ = false;
  }
  const float* b = io.Cur();
  for (int c = 0; c < channels_; ++c) last_[c] = b[c * 256 + 255];
  have_prev_ = true;
  return 1;
}

}  // namespace native_audio
