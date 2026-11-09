#include "audio.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

namespace srb {

namespace {

// --- FFmpeg, loaded from the builder's own copy -------------------------------------

struct Ff {
  decltype(&avformat_alloc_context) alloc_format;
  decltype(&avio_alloc_context) alloc_io;
  decltype(&avio_context_free) free_io;
  decltype(&av_find_input_format) find_input;
  decltype(&avformat_open_input) open_input;
  decltype(&avformat_close_input) close_input;
  decltype(&av_read_frame) read_frame;
  decltype(&avcodec_find_decoder) find_decoder;
  decltype(&avcodec_alloc_context3) alloc_codec;
  decltype(&avcodec_free_context) free_codec;
  decltype(&avcodec_parameters_to_context) params_to_context;
  decltype(&avcodec_open2) open_codec;
  decltype(&avcodec_send_packet) send_packet;
  decltype(&avcodec_receive_frame) receive_frame;
  decltype(&av_packet_alloc) packet_alloc;
  decltype(&av_packet_free) packet_free;
  decltype(&av_packet_unref) packet_unref;
  decltype(&av_frame_alloc) frame_alloc;
  decltype(&av_frame_free) frame_free;
  decltype(&av_malloc) malloc_;
  decltype(&av_free) free_;
};

template <typename T>
void Load(HMODULE m, const char* name, T& fn) {
  fn = reinterpret_cast<T>(GetProcAddress(m, name));
  if (!fn) throw std::runtime_error(std::string("FFmpeg has no ") + name);
}

Ff LoadFfmpeg(const fs::path& dir) {
  SetDllDirectoryW(dir.c_str());
  auto lib = [&](const wchar_t* prefix) {
    for (auto& e : fs::directory_iterator(dir)) {
      const std::wstring n = e.path().filename().wstring();
      if (n.rfind(prefix, 0) == 0 && e.path().extension() == L".dll") {
        HMODULE m = LoadLibraryExW(e.path().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (m) return m;
      }
    }
    throw std::runtime_error("cannot load FFmpeg");
  };
  HMODULE util = lib(L"avutil-"), codec = lib(L"avcodec-"), format = lib(L"avformat-");
  Ff f{};
  Load(format, "avformat_alloc_context", f.alloc_format);
  Load(format, "avio_alloc_context", f.alloc_io);
  Load(format, "avio_context_free", f.free_io);
  Load(format, "av_find_input_format", f.find_input);
  Load(format, "avformat_open_input", f.open_input);
  Load(format, "avformat_close_input", f.close_input);
  Load(format, "av_read_frame", f.read_frame);
  Load(codec, "avcodec_find_decoder", f.find_decoder);
  Load(codec, "avcodec_alloc_context3", f.alloc_codec);
  Load(codec, "avcodec_free_context", f.free_codec);
  Load(codec, "avcodec_parameters_to_context", f.params_to_context);
  Load(codec, "avcodec_open2", f.open_codec);
  Load(codec, "avcodec_send_packet", f.send_packet);
  Load(codec, "avcodec_receive_frame", f.receive_frame);
  Load(codec, "av_packet_alloc", f.packet_alloc);
  Load(codec, "av_packet_free", f.packet_free);
  Load(codec, "av_packet_unref", f.packet_unref);
  Load(util, "av_frame_alloc", f.frame_alloc);
  Load(util, "av_frame_free", f.frame_free);
  Load(util, "av_malloc", f.malloc_);
  Load(util, "av_free", f.free_);
  // the decoder's notes (e.g. xma2's skipped-sample timestamps) are not the player's business
  decltype(&av_log_set_level) set_level = nullptr;
  Load(util, "av_log_set_level", set_level);
  set_level(AV_LOG_QUIET);
  return f;
}

// --- reading ------------------------------------------------------------------------------

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t LeN(const uint8_t* p, int n) {
  uint64_t v = 0;
  for (int i = n - 1; i >= 0; --i) v = v << 8 | p[i];
  return v;
}

// A BGFA archive in a file (from `base`) or in memory: the table of contents at the
// end, fields of the widths its header names (tools/audio/ea_ast.py Bgfa).
struct Entry {
  std::string name;
  uint64_t offset = 0, stored = 0, size = 0;
};

class Archive {
 public:
  Archive(HANDLE file, uint64_t base) : file_(file), base_(base) { Parse(); }
  explicit Archive(std::vector<uint8_t> data) : data_(std::move(data)) { Parse(); }

  const std::vector<Entry>& Entries() const { return entries_; }
  std::vector<uint8_t> Read(const Entry& e, uint64_t limit = UINT64_MAX) const {
    return Bytes(e.offset, std::min(e.stored, limit));
  }

 private:
  std::vector<uint8_t> Bytes(uint64_t at, uint64_t n) const {
    if (!data_.empty() || file_ == INVALID_HANDLE_VALUE) {
      if (at > data_.size()) return {};
      n = std::min<uint64_t>(n, data_.size() - at);
      return std::vector<uint8_t>(data_.begin() + ptrdiff_t(at), data_.begin() + ptrdiff_t(at + n));
    }
    std::vector<uint8_t> out(static_cast<size_t>(n));
    uint64_t pos = base_ + at;
    size_t done = 0;
    while (done < out.size()) {
      OVERLAPPED o{};
      o.Offset = DWORD(pos);
      o.OffsetHigh = DWORD(pos >> 32);
      DWORD got = 0;
      if (!ReadFile(file_, out.data() + done, DWORD(std::min<size_t>(out.size() - done, 1u << 30)), &got, &o) || !got)
        break;
      done += got;
      pos += got;
    }
    out.resize(done);
    return out;
  }

  void Parse() {
    const auto head = Bytes(0, 0x30);
    if (head.size() < 0x30 || std::memcmp(head.data(), "BGFA", 4) != 0) throw std::runtime_error("not a BGFA archive");
    const uint32_t files = Le32(&head[0x0C]), toc_off = Le32(&head[0x10]), toc_size = Le32(&head[0x18]);
    const int flag_w = head[0x21], hash_w = head[0x22], off_w = head[0x23], zsize_w = head[0x24], size_w = head[0x25],
              shift = head[0x26];
    const uint32_t num = Le32(&head[0x28]), name_w = Le32(&head[0x2C]);
    const auto toc = Bytes(toc_off, toc_size);
    size_t p = size_t(num) * 4;
    for (uint32_t i = 0; i < files; ++i) {
      if (p + (flag_w ? 2 : 1) + hash_w + off_w + zsize_w + size_w + name_w > toc.size()) break;
      p += flag_w ? 2 : 1;
      p += hash_w;
      Entry e;
      e.offset = LeN(&toc[p], off_w) << shift;
      p += off_w;
      e.stored = zsize_w ? LeN(&toc[p], zsize_w) : 0;
      p += zsize_w;
      e.size = size_w ? LeN(&toc[p], size_w) : 0;
      p += size_w;
      const char* n = reinterpret_cast<const char*>(&toc[p]);
      e.name.assign(n, strnlen(n, name_w));
      p += name_w;
      entries_.push_back(std::move(e));
    }
  }

  HANDLE file_ = INVALID_HANDLE_VALUE;
  uint64_t base_ = 0;
  std::vector<uint8_t> data_;
  std::vector<Entry> entries_;
};

// --- sound records (ea_ast.py) ---------------------------------------------------------------

struct Snr {
  int codec = 0, channels = 0, kind = 0;
  uint32_t rate = 0, samples = 0, loop_start = 0, header_size = 8;
  bool loop = false;
};

struct Record {
  Snr snr;
  size_t snr_pos = 0;
};

Record ParseSabr(const std::vector<uint8_t>& d) {
  if (d.size() < 24 || std::memcmp(d.data(), "rbas", 4) != 0) throw std::runtime_error("not a sabr record");
  // the property list from 8: {tag, type, length, count} + data; "numc" holds the count
  size_t p = 8;
  int total = -1, n = 0;
  while (total < 0 || n < total) {
    if (p + 16 > d.size() || n > 64) throw std::runtime_error("property list does not end");
    const uint32_t length = Le32(&d[p + 8]);
    if (std::memcmp(&d[p], "cmun", 4) == 0 && p + 20 <= d.size()) total = int(Le32(&d[p + 16]));
    p += 16 + length;
    ++n;
  }
  Record r;
  r.snr_pos = (p + 15) & ~size_t(15);
  if (r.snr_pos + 8 > d.size()) throw std::runtime_error("no SNR header");  // 8 bytes, 12 with a loop
  const uint32_t h1 = Be32(&d[r.snr_pos]), h2 = Be32(&d[r.snr_pos + 4]);
  if (h1 >> 28) throw std::runtime_error("SNR version");
  r.snr.codec = int((h1 >> 24) & 15);
  r.snr.channels = int(((h1 >> 18) & 0x3F) + 1);
  r.snr.rate = h1 & 0x3FFFF;
  r.snr.kind = int(h2 >> 30);
  r.snr.loop = (h2 >> 29) & 1;
  r.snr.samples = h2 & 0x1FFFFFFF;
  if (r.snr.loop && r.snr_pos + 12 > d.size()) throw std::runtime_error("no SNR loop start");
  r.snr.loop_start = r.snr.loop ? Be32(&d[r.snr_pos + 8]) : 0;
  r.snr.header_size = r.snr.loop ? 12 : 8;
  return r;
}

bool IsBank(const std::vector<uint8_t>& d) { return d.size() >= 12 && std::memcmp(&d[8], "cmun", 4) != 0; }

std::vector<uint32_t> BlockList(const std::vector<uint8_t>& buf, size_t pos) {
  std::vector<uint32_t> blocks;
  while (pos + 8 <= buf.size()) {
    const uint8_t flag = buf[pos];
    const uint32_t size = Be32(&buf[pos]) & 0xFFFFFF;
    if (!size) break;
    blocks.push_back(Be32(&buf[pos + 4]));
    pos += size;
    if (flag == 0x80) break;
  }
  return blocks;
}

// One stream's XMA packets from the EAAC blocks, padded to 2048-byte packets.
std::vector<uint8_t> Deblock(const std::vector<uint8_t>& buf, size_t pos, int stream, uint32_t* samples) {
  std::vector<uint8_t> out;
  *samples = 0;
  while (pos + 8 <= buf.size()) {
    const uint8_t flag = buf[pos];
    const uint32_t size = Be32(&buf[pos]) & 0xFFFFFF;
    if (!size) break;
    *samples += Be32(&buf[pos + 4]);
    size_t skip = 8;
    for (int s = 0; s < stream && pos + skip + 4 <= buf.size(); ++s) skip += Be32(&buf[pos + skip]) / 4;
    if (pos + skip + 4 > buf.size()) break;
    const size_t dsz = Be32(&buf[pos + skip]) / 4;
    const size_t from = pos + skip + 4, to = std::min(buf.size(), pos + skip + dsz);
    if (to > from) out.insert(out.end(), buf.begin() + ptrdiff_t(from), buf.begin() + ptrdiff_t(to));
    if (out.size() % 0x800) out.resize(out.size() + (0x800 - out.size() % 0x800), 0xFF);
    pos += size;
    if (flag == 0x80) break;
  }
  return out;
}

void Put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
void Put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }

// The standard XMA2 RIFF around raw packets (ea_ast.py xma2_riff).
std::vector<uint8_t> Xma2Riff(const std::vector<uint8_t>& data, int channels, uint32_t rate, uint32_t samples) {
  std::vector<uint8_t> fmt;
  Put16(fmt, 0x166); Put16(fmt, uint16_t(channels)); Put32(fmt, rate); Put32(fmt, rate * channels * 2);
  Put16(fmt, 0x800); Put16(fmt, 16); Put16(fmt, 34);
  Put16(fmt, 1); Put32(fmt, channels == 2 ? 3 : 4); Put32(fmt, samples); Put32(fmt, 0x10000); Put32(fmt, 0);
  Put32(fmt, samples); Put32(fmt, 0); Put32(fmt, 0); fmt.push_back(0); fmt.push_back(4);
  Put16(fmt, uint16_t(std::max<size_t>(1, data.size() / 0x10000 + 1)));
  std::vector<uint8_t> body = {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
  Put32(body, uint32_t(fmt.size()));
  body.insert(body.end(), fmt.begin(), fmt.end());
  body.insert(body.end(), {'d', 'a', 't', 'a'});
  Put32(body, uint32_t(data.size()));
  body.insert(body.end(), data.begin(), data.end());
  std::vector<uint8_t> out = {'R', 'I', 'F', 'F'};
  Put32(out, uint32_t(body.size()));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

// --- decoding ------------------------------------------------------------------------------

struct MemIo {
  const std::vector<uint8_t>* d;
  size_t pos;
};

int IoRead(void* opaque, uint8_t* buf, int n) {
  auto* m = static_cast<MemIo*>(opaque);
  const size_t left = m->d->size() - m->pos;
  if (!left) return AVERROR_EOF;
  const size_t k = std::min<size_t>(left, size_t(n));
  std::memcpy(buf, m->d->data() + m->pos, k);
  m->pos += k;
  return int(k);
}

int64_t IoSeek(void* opaque, int64_t off, int whence) {
  auto* m = static_cast<MemIo*>(opaque);
  if (whence == AVSEEK_SIZE) return int64_t(m->d->size());
  int64_t to = whence == SEEK_SET ? off : whence == SEEK_CUR ? int64_t(m->pos) + off : int64_t(m->d->size()) + off;
  if (to < 0 || to > int64_t(m->d->size())) return -1;
  m->pos = size_t(to);
  return to;
}

// int16 frames x channels of a RIFF (as `ffmpeg -f wav -i - -f s16le -`: float to
// 16 bit as libswresample does, lrintf(x * 32768) clipped).
std::vector<int16_t> Decode(const Ff& f, const std::vector<uint8_t>& riff, int channels) {
  MemIo mem{&riff, 0};
  const int buf_size = 1 << 16;
  auto* io_buf = static_cast<uint8_t*>(f.malloc_(buf_size));
  AVIOContext* io = f.alloc_io(io_buf, buf_size, 0, &mem, IoRead, nullptr, IoSeek);
  AVFormatContext* fmt = f.alloc_format();
  fmt->pb = io;
  std::vector<int16_t> out;
  AVCodecContext* cc = nullptr;
  AVPacket* pkt = f.packet_alloc();
  AVFrame* frame = f.frame_alloc();
  auto cleanup = [&] {
    f.frame_free(&frame);
    f.packet_free(&pkt);
    if (cc) f.free_codec(&cc);
    f.close_input(&fmt);
    if (io) {
      f.free_(io->buffer);
      f.free_io(&io);
    }
  };
  try {
    if (f.open_input(&fmt, nullptr, f.find_input("wav"), nullptr) < 0) throw std::runtime_error("open");
    const AVCodecParameters* par = fmt->streams[0]->codecpar;
    const AVCodec* codec = f.find_decoder(par->codec_id);
    if (!codec) throw std::runtime_error("no decoder");
    cc = f.alloc_codec(codec);
    f.params_to_context(cc, par);
    cc->thread_count = 1;
    if (f.open_codec(cc, codec, nullptr) < 0) throw std::runtime_error("codec");
    auto drain = [&] {
      while (f.receive_frame(cc, frame) == 0) {
        const int ch = frame->ch_layout.nb_channels;
        for (int i = 0; i < frame->nb_samples; ++i)
          for (int c = 0; c < channels; ++c) {
            float x = 0;
            const int src = c < ch ? c : ch - 1;
            if (frame->format == AV_SAMPLE_FMT_FLTP)
              x = reinterpret_cast<const float*>(frame->extended_data[src])[i];
            else if (frame->format == AV_SAMPLE_FMT_FLT)
              x = reinterpret_cast<const float*>(frame->extended_data[0])[i * ch + src];
            else if (frame->format == AV_SAMPLE_FMT_S16P)
              x = reinterpret_cast<const int16_t*>(frame->extended_data[src])[i] / 32768.0f;
            const long v = lrintf(x * 32768.0f);
            out.push_back(int16_t(std::clamp<long>(v, -32768, 32767)));
          }
      }
    };
    while (f.read_frame(fmt, pkt) >= 0) {
      if (pkt->stream_index == 0) {
        f.send_packet(cc, pkt);
        drain();
      }
      f.packet_unref(pkt);
    }
    f.send_packet(cc, nullptr);
    drain();
  } catch (...) {
    cleanup();
    throw;
  }
  cleanup();
  return out;
}

std::vector<uint8_t> WavBytes(const std::vector<int16_t>& pcm, int channels, uint32_t rate, bool loop,
                              uint32_t loop_start, uint32_t loop_end) {
  std::vector<uint8_t> chunks = {'f', 'm', 't', ' '};
  Put32(chunks, 16);
  Put16(chunks, 1); Put16(chunks, uint16_t(channels)); Put32(chunks, rate); Put32(chunks, rate * channels * 2);
  Put16(chunks, uint16_t(channels * 2)); Put16(chunks, 16);
  chunks.insert(chunks.end(), {'d', 'a', 't', 'a'});
  Put32(chunks, uint32_t(pcm.size() * 2));
  const auto* raw = reinterpret_cast<const uint8_t*>(pcm.data());
  chunks.insert(chunks.end(), raw, raw + pcm.size() * 2);
  if (loop) {
    chunks.insert(chunks.end(), {'s', 'm', 'p', 'l'});
    Put32(chunks, 60);
    const uint32_t smpl[15] = {0, 0, uint32_t(std::lround(1e9 / rate)), 60, 0, 0, 0, 1, 0,
                               0, 0, loop_start, loop_end, 0, 0};
    for (uint32_t v : smpl) Put32(chunks, v);
  }
  std::vector<uint8_t> out = {'R', 'I', 'F', 'F'};
  Put32(out, uint32_t(4 + chunks.size()));
  out.insert(out.end(), {'W', 'A', 'V', 'E'});
  out.insert(out.end(), chunks.begin(), chunks.end());
  return out;
}

uint32_t Fnv1a(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
  return h;
}

// --- the run ----------------------------------------------------------------------------------

struct Job {
  std::string label, name;
  std::vector<uint8_t> body, sns;
  uint32_t sns_offset = 0xFFFFFFFF;
};

struct Sound {  // one index.bin entry (build_index.py)
  std::string label, name, file;
  uint32_t fnv = 0;
  bool has_fnv = false, streamed = false, loop = false, exact = false;
  int channels = 0;
  uint32_t rate = 0, samples = 0, loop_start = 0;
  uint8_t snr16[16] = {};
  std::vector<uint32_t> blocks;
  uint32_t sns_offset = 0xFFFFFFFF;
  std::string error;  // why it is not in index.bin
  size_t decoded = 0;
};

void Collect(const Archive& arc, const std::string& label, bool top, std::vector<Job>& jobs,
             std::atomic<bool>& cancel) {
  std::map<std::string, const Entry*> by_name;
  for (const Entry& e : arc.Entries()) by_name[e.name] = &e;
  for (const Entry& e : arc.Entries()) {
    if (cancel) throw Failure("cancelled");
    if (e.size || e.stored < 4) continue;  // compressed entries hold no sounds (measured)
    const auto head = arc.Read(e, 4);
    if (head.size() == 4 && std::memcmp(head.data(), "BGFA", 4) == 0) {
      Collect(Archive(arc.Read(e)), label + "/" + e.name, false, jobs, cancel);
      continue;
    }
    if (std::memcmp(head.data(), "rbas", 4) != 0) continue;
    Job j;
    j.label = label;
    j.name = e.name;
    j.body = arc.Read(e);
    if (IsBank(j.body)) continue;
    auto sns = by_name.find(e.name + "_SNS");
    if (sns != by_name.end()) {
      j.sns = arc.Read(*sns->second);
      if (top && !sns->second->size) j.sns_offset = uint32_t(sns->second->offset);
    }
    jobs.push_back(std::move(j));
  }
}

Sound Convert(const Ff& f, const Job& j, const fs::path& out) {
  Sound s;
  s.label = j.label;
  s.name = j.name;
  s.sns_offset = j.sns_offset;
  try {
    const Record rec = ParseSabr(j.body);
    const Snr& snr = rec.snr;
    std::memcpy(s.snr16, &j.body[rec.snr_pos], std::min<size_t>(16, j.body.size() - rec.snr_pos));
    if (j.body.size() - rec.snr_pos >= 256) {
      s.fnv = Fnv1a(&j.body[rec.snr_pos], 256);
      s.has_fnv = true;
    }
    if (snr.codec != 3) {
      s.error = "codec " + std::to_string(snr.codec);
      return s;
    }
    const std::vector<uint8_t>& buf = snr.kind == 1 ? j.sns : j.body;
    const size_t pos = snr.kind == 1 ? 0 : rec.snr_pos + snr.header_size;
    if (snr.kind == 1 && buf.empty()) {
      s.error = "streamed, no _SNS entry";
      return s;
    }
    s.blocks = BlockList(buf, pos);
    std::vector<std::vector<int16_t>> parts;
    std::vector<int> part_channels;
    for (int stream = 0; stream < (snr.channels + 1) / 2; ++stream) {
      const int ch = std::min(2, snr.channels - 2 * stream);
      uint32_t block_samples = 0;
      const auto data = Deblock(buf, pos, stream, &block_samples);
      parts.push_back(Decode(f, Xma2Riff(data, ch, snr.rate, block_samples), ch));
      part_channels.push_back(ch);
    }
    size_t frames = SIZE_MAX;
    for (size_t i = 0; i < parts.size(); ++i) frames = std::min(frames, parts[i].size() / size_t(part_channels[i]));
    std::vector<int16_t> pcm;
    pcm.reserve(frames * size_t(snr.channels));
    for (size_t t = 0; t < frames; ++t)
      for (size_t i = 0; i < parts.size(); ++i)
        for (int c = 0; c < part_channels[i]; ++c) pcm.push_back(parts[i][t * size_t(part_channels[i]) + size_t(c)]);
    s.channels = snr.channels;
    s.rate = snr.rate;
    s.samples = snr.samples;
    s.streamed = snr.kind != 0;
    s.loop = snr.loop;
    s.loop_start = snr.loop_start;
    s.exact = frames == snr.samples;
    s.decoded = frames;
    if (!s.exact) s.error = "decoded " + std::to_string(frames) + " of " + std::to_string(snr.samples);
    s.file = j.label + "/" + j.name + ".wav";
    const auto wav = WavBytes(pcm, snr.channels, snr.rate, snr.loop, snr.loop_start, snr.samples - 1);
    WriteAll(out / fs::path(Widen(s.file)), wav.data(), wav.size());
  } catch (const Failure&) {
    throw;
  } catch (const std::exception& e) {
    s.exact = false;
    s.error = e.what();
  }
  return s;
}

void WriteIndex(const std::vector<Sound>& sounds, const fs::path& path) {
  std::vector<uint8_t> out = {'S', 'R', 'A', 'I'};
  uint32_t n = 0;
  for (const Sound& s : sounds) n += s.exact && !s.file.empty();
  Put32(out, 3);
  Put32(out, n);
  for (const Sound& s : sounds) {
    if (!s.exact || s.file.empty()) continue;
    Put32(out, s.has_fnv ? s.fnv : 0);
    out.push_back(s.has_fnv);
    out.push_back(uint8_t(s.channels));
    out.push_back(s.streamed);
    out.push_back(s.loop);
    Put32(out, s.rate);
    Put32(out, s.samples);
    Put32(out, s.loop_start);
    out.insert(out.end(), s.snr16, s.snr16 + 16);
    for (const std::string& t : {s.file, s.label + "/" + s.name}) {
      Put16(out, uint16_t(t.size()));
      out.insert(out.end(), t.begin(), t.end());
    }
    Put32(out, uint32_t(s.blocks.size()));
    for (uint32_t b : s.blocks) Put32(out, b);
    Put32(out, s.sns_offset);
  }
  WriteAll(path, out.data(), out.size());
}

}  // namespace

AudioResult ConvertAudio(const fs::path& data, const fs::path& out, const fs::path& ffmpeg,
                         std::atomic<bool>& cancel, const std::function<void(double, const std::string&)>& progress) {
  const Ff f = LoadFfmpeg(ffmpeg);
  std::vector<std::wstring> files;
  for (auto& e : fs::directory_iterator(data)) {
    std::wstring n = e.path().filename().wstring();
    std::wstring up = n;
    for (wchar_t& c : up) c = wchar_t(towupper(c));
    if (up.size() > 4 && up.substr(up.size() - 4) == L".AST" && up.find(L"_SR.AST") == std::wstring::npos)
      files.push_back(n);
  }
  std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) {
    return Narrow(a) < Narrow(b);  // Python's sorted(os.listdir): by code point
  });
  std::vector<Job> jobs;
  for (const std::wstring& name : files) {
    progress(0, Narrow(name));
    HANDLE h = CreateFileW((data / name).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) continue;
    try {
      Collect(Archive(h, 0), Narrow(name), true, jobs, cancel);
    } catch (const Failure&) {
      CloseHandle(h);
      throw;
    } catch (...) {
    }
    CloseHandle(h);
  }

  std::vector<Sound> sounds(jobs.size());
  std::atomic<size_t> next{0}, done{0};
  std::mutex mtx;
  const unsigned workers = std::max(1u, std::thread::hardware_concurrency() > 2 ? std::thread::hardware_concurrency() - 2 : 1);
  std::vector<std::thread> pool;
  for (unsigned w = 0; w < workers; ++w)
    pool.emplace_back([&] {
      for (size_t i; !cancel && (i = next++) < jobs.size();) {
        sounds[i] = Convert(f, jobs[i], out);
        const size_t d = ++done;
        if (d % 50 == 0 || d == jobs.size()) {
          std::lock_guard lock(mtx);
          progress(double(d) / double(jobs.size()), jobs[i].label + "/" + jobs[i].name);
        }
      }
    });
  for (auto& t : pool) t.join();
  if (cancel) throw Failure("cancelled");
  std::sort(sounds.begin(), sounds.end(), [](const Sound& a, const Sound& b) {
    return std::tie(a.label, a.name) < std::tie(b.label, b.name);
  });
  WriteIndex(sounds, out / L"index.bin");
  AudioResult r;
  r.sounds = int(sounds.size());
  for (const Sound& s : sounds) {
    r.exact += s.exact;
    r.failed += s.file.empty();
  }
  return r;
}

}  // namespace srb
