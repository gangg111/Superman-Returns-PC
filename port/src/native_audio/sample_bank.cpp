#include "native_audio/sample_bank.h"

#include <cstdio>
#include <cstring>

namespace native_audio {

namespace {

struct Reader {
  const std::vector<uint8_t>& d;
  size_t p = 0;
  bool ok = true;
  template <typename T>
  T Get() {
    T v{};
    if (p + sizeof(T) > d.size()) {
      ok = false;
      return v;
    }
    std::memcpy(&v, d.data() + p, sizeof(T));
    p += sizeof(T);
    return v;
  }
  std::string Str() {
    const uint16_t n = Get<uint16_t>();
    if (!ok || p + n > d.size()) {
      ok = false;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(d.data() + p), n);
    p += n;
    return s;
  }
};

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out.resize(size > 0 ? size_t(size) : 0);
  const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

}  // namespace

uint32_t SampleBank::Fnv1a(const uint8_t* data, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; ++i) h = (h ^ data[i]) * 16777619u;
  return h;
}

bool SampleBank::Open(const std::string& dir) {
  dir_ = dir;
  sounds_.clear();
  by_fnv_.clear();
  std::vector<uint8_t> d;
  if (!ReadFile(dir + "/index.bin", d) || d.size() < 12 || std::memcmp(d.data(), "SRAI", 4))
    return false;
  Reader r{d, 4};
  const uint32_t version = r.Get<uint32_t>();
  if (version != 1 && version != 2) return false;
  const uint32_t count = r.Get<uint32_t>();
  sounds_.reserve(count);
  for (uint32_t i = 0; i < count && r.ok; ++i) {
    SoundInfo s;
    s.fnv256 = r.Get<uint32_t>();
    s.has_fnv = r.Get<uint8_t>() != 0;
    s.channels = r.Get<uint8_t>();
    s.streamed = r.Get<uint8_t>() != 0;
    s.loop = r.Get<uint8_t>() != 0;
    s.rate = r.Get<uint32_t>();
    s.samples = r.Get<uint32_t>();
    s.loop_start = r.Get<uint32_t>();
    for (auto& b : s.snr16) b = r.Get<uint8_t>();
    s.file = r.Str();
    s.name = r.Str();
    if (version >= 2) {
      const uint32_t n = r.Get<uint32_t>();
      for (uint32_t k = 0; k < n && r.ok; ++k) s.blocks.push_back(r.Get<uint32_t>());
    }
    if (r.ok) sounds_.push_back(std::move(s));
  }
  if (!r.ok) {
    sounds_.clear();
    return false;
  }
  for (size_t i = 0; i < sounds_.size(); ++i)
    if (sounds_[i].has_fnv) by_fnv_.emplace(sounds_[i].fnv256, i);  // first of identical audio
  return true;
}

const SoundInfo* SampleBank::FindByFnv(uint32_t fnv256) const {
  auto it = by_fnv_.find(fnv256);
  return it == by_fnv_.end() ? nullptr : &sounds_[it->second];
}

std::vector<const SoundInfo*> SampleBank::FindBySnr(const uint8_t* snr, size_t len) const {
  std::vector<const SoundInfo*> out;
  len = len > 16 ? 16 : len;
  for (const SoundInfo& s : sounds_)
    if (std::memcmp(s.snr16.data(), snr, len) == 0) out.push_back(&s);
  return out;
}

std::shared_ptr<const Pcm> SampleBank::Load(const SoundInfo& info) {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  if (auto it = cache_.find(&info); it != cache_.end())
    if (auto pcm = it->second.lock()) return pcm;
  std::vector<uint8_t> d;
  if (!ReadFile(dir_ + "/" + info.file, d) || d.size() < 12 || std::memcmp(d.data(), "RIFF", 4))
    return nullptr;
  // Walk the chunks; convert_audio.py writes fmt, data and an optional smpl.
  size_t p = 12;
  uint16_t channels = 0, bits = 0;
  const uint8_t* data = nullptr;
  uint32_t data_size = 0;
  while (p + 8 <= d.size()) {
    uint32_t size;
    std::memcpy(&size, d.data() + p + 4, 4);
    if (p + 8 + size > d.size()) break;
    if (!std::memcmp(d.data() + p, "fmt ", 4) && size >= 16) {
      std::memcpy(&channels, d.data() + p + 10, 2);
      std::memcpy(&bits, d.data() + p + 22, 2);
    } else if (!std::memcmp(d.data() + p, "data", 4)) {
      data = d.data() + p + 8;
      data_size = size;
    }
    p += 8 + size + (size & 1);
  }
  if (!data || channels != info.channels || bits != 16) return nullptr;
  auto pcm = std::make_shared<Pcm>();
  pcm->info = &info;
  pcm->frames.resize(data_size / 2);
  std::memcpy(pcm->frames.data(), data, pcm->frames.size() * 2);
  if (pcm->frames.size() != size_t(info.samples) * info.channels) return nullptr;
  cache_[&info] = pcm;
  return pcm;
}

}  // namespace native_audio
