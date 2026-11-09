#include "textures.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include "pack.h"

namespace srb {

namespace {

// Xenos 2D tiled address in bytes (port/src/native_renderer/xenos_tiling.h, from Xenia, BSD).
int32_t TiledAddress2D(int32_t x, int32_t y, uint32_t pitch_macro_tiles, uint32_t bpb_log2) {
  const int32_t outer_blocks = ((y >> 5) * int32_t(pitch_macro_tiles) + (x >> 5)) << 6;
  const int32_t inner_blocks = (((y >> 1) & 0x7) << 3) | (x & 0x7);
  const int32_t oi = (outer_blocks | inner_blocks) << bpb_log2;
  const int32_t bank = (y >> 4) & 0x1;
  const int32_t pipe = ((x >> 3) & 0x3) ^ (((y >> 3) & 0x1) << 1);
  return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xF) | (((oi >> 4) & 0x1) << 5) |
         (((oi >> 5) & 0x7) << 8) | (oi >> 8 << 12);
}

struct Rgba {
  uint8_t r, g, b, a;
};

// Pillow's BcnDecode.c: 5:6:5 widened by repeating the top bits, thirds and halves rounded down.
Rgba Decode565(uint16_t x) {
  int r = (x & 0xf800) >> 8, g = (x & 0x7e0) >> 3, b = (x & 0x1f) << 3;
  r |= r >> 5;
  g |= g >> 6;
  b |= b >> 5;
  return {uint8_t(r), uint8_t(g), uint8_t(b), 0xff};
}

void DecodeColor(const uint8_t* s, bool separate_alpha, Rgba out[16]) {
  const uint16_t c0 = uint16_t(s[0] | s[1] << 8), c1 = uint16_t(s[2] | s[3] << 8);
  const uint32_t lut = uint32_t(s[4]) | uint32_t(s[5]) << 8 | uint32_t(s[6]) << 16 | uint32_t(s[7]) << 24;
  Rgba p[4];
  p[0] = Decode565(c0);
  p[1] = Decode565(c1);
  const int r0 = p[0].r, g0 = p[0].g, b0 = p[0].b, r1 = p[1].r, g1 = p[1].g, b1 = p[1].b;
  if (c0 > c1 || separate_alpha) {
    p[2] = {uint8_t((2 * r0 + r1) / 3), uint8_t((2 * g0 + g1) / 3), uint8_t((2 * b0 + b1) / 3), 0xff};
    p[3] = {uint8_t((r0 + 2 * r1) / 3), uint8_t((g0 + 2 * g1) / 3), uint8_t((b0 + 2 * b1) / 3), 0xff};
  } else {
    p[2] = {uint8_t((r0 + r1) / 2), uint8_t((g0 + g1) / 2), uint8_t((b0 + b1) / 2), 0xff};
    p[3] = {0, 0, 0, 0};
  }
  for (int n = 0; n < 16; ++n) out[n] = p[3 & (lut >> (2 * n))];
}

void DecodeAlpha(const uint8_t* s, Rgba out[16]) {
  int a[8];
  a[0] = s[0];
  a[1] = s[1];
  if (a[0] > a[1]) {
    for (int i = 1; i <= 6; ++i) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
  } else {
    for (int i = 1; i <= 4; ++i) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5;
    a[6] = 0;
    a[7] = 0xff;
  }
  const uint32_t lut1 = uint32_t(s[2]) | uint32_t(s[3]) << 8 | uint32_t(s[4]) << 16;
  const uint32_t lut2 = uint32_t(s[5]) | uint32_t(s[6]) << 8 | uint32_t(s[7]) << 16;
  for (int n = 0; n < 8; ++n) {
    out[n].a = uint8_t(a[7 & (lut1 >> (3 * n))]);
    out[8 + n].a = uint8_t(a[7 & (lut2 >> (3 * n))]);
  }
}

// make_window_map.py, the same operations in the same order (doubles as numpy's float64).
using Mask = std::vector<uint8_t>;

Mask Dilate(const Mask& m, int w, int h, int r) {
  Mask rows(m.size()), out(m.size());
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint8_t v = 0;
      for (int d = -r; d <= r && !v; ++d) v = m[size_t(y) * w + ((x + d) % w + w) % w];
      rows[size_t(y) * w + x] = v;
    }
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint8_t v = 0;
      for (int d = -r; d <= r && !v; ++d) v = rows[size_t(((y + d) % h + h) % h) * w + x];
      out[size_t(y) * w + x] = v;
    }
  return out;
}

Mask Not(Mask m) {
  for (auto& v : m) v = !v;
  return m;
}

// unwrapped_span: the smallest interval on the circle covering the occupied indices
std::pair<int, int> UnwrappedSpan(const std::vector<uint8_t>& occ, int size) {
  bool all = true;
  for (int i = 0; i < size; ++i) all &= occ[i] != 0;
  if (all) return {0, size};
  int best_len = 0, best_end = 0, run = 0;
  for (int i = 0; i < 2 * size; ++i) {
    if (!occ[i % size]) {
      ++run;
      if (run > best_len) {
        best_len = run;
        best_end = i;
      }
    } else {
      run = 0;
    }
  }
  const int start = (best_end + 1) % size;
  return {start, start + size - best_len};
}

std::vector<uint8_t> DdsHeader(uint32_t w, uint32_t h) {
  std::vector<uint8_t> d(128, 0);
  auto put = [&](size_t at, uint32_t v) { std::memcpy(&d[at], &v, 4); };
  std::memcpy(d.data(), "DDS ", 4);
  put(4, 124);
  put(8, 0x1 | 0x2 | 0x4 | 0x1000);
  put(12, h);
  put(16, w);
  put(20, w * 4);
  put(24, 0);
  put(28, 1);
  // 44 reserved bytes, then the pixel format
  const uint32_t pf[8] = {32, 0x41, 0, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000};
  for (int i = 0; i < 8; ++i) put(76 + 4 * i, pf[i]);
  put(108, 0x1000);
  return d;
}

std::vector<uint8_t> ReadRange(const fs::path& path, uint64_t offset, size_t size) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw Failure("missing:" + Narrow(path.filename().wstring()));
  in.seekg(std::streamoff(offset));
  std::vector<uint8_t> out(size);
  in.read(reinterpret_cast<char*>(out.data()), std::streamsize(size));
  if (size_t(in.gcount()) != size) out.resize(size_t(in.gcount()));
  return out;
}

}  // namespace

std::vector<uint8_t> DecodeGameTexture(const std::vector<uint8_t>& tiled, uint32_t width, uint32_t height,
                                       uint32_t guest_format) {
  if (guest_format != 18 && guest_format != 20) throw std::runtime_error("texture format " + std::to_string(guest_format));
  const bool bc3 = guest_format == 20;  // k_DXT4_5; 18 is k_DXT1
  const uint32_t bpb = bc3 ? 16 : 8, log2 = bc3 ? 4 : 3;
  const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4, pitch = (bw + 31) >> 5;
  std::vector<uint8_t> rgba(size_t(width) * height * 4);
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const size_t at = size_t(TiledAddress2D(int32_t(bx), int32_t(by), pitch, log2));
      if (at + bpb > tiled.size()) throw std::runtime_error("texture data too short");
      uint8_t block[16];
      for (uint32_t i = 0; i < bpb; ++i) block[i] = tiled[at + (i ^ 1)];  // 8-in-16
      Rgba px[16];
      DecodeColor(block + (bc3 ? 8 : 0), bc3, px);
      if (bc3) DecodeAlpha(block, px);
      for (int n = 0; n < 16; ++n) {
        const uint32_t x = bx * 4 + n % 4, y = by * 4 + n / 4;
        if (x >= width || y >= height) continue;
        std::memcpy(&rgba[(size_t(y) * width + x) * 4], &px[n], 4);
      }
    }
  return rgba;
}

std::vector<uint8_t> WindowMap(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height,
                               const double kenv[3]) {
  const int w = int(width), h = int(height);
  const size_t n = size_t(w) * h;
  // glass_mask: (rgb * k).sum / max(k.max, 1e-3) > GLASS
  const double k[3] = {std::max(kenv[0], 0.0), std::max(kenv[1], 0.0), std::max(kenv[2], 0.0)};
  const double kmax = std::max(std::max(std::max(k[0], k[1]), k[2]), 1e-3);
  Mask m(n);
  for (size_t i = 0; i < n; ++i) {
    const double s = rgba[i * 4] / 255.0 * k[0] + rgba[i * 4 + 1] / 255.0 * k[1] + rgba[i * 4 + 2] / 255.0 * k[2];
    m[i] = s / kmax > 0.5;
  }
  // close: erode(dilate(m, r), r) | m, r from CLOSE_FRAC = 0.03
  const int r = std::max(1, int(std::nearbyint(0.03 * double(std::max(w, h)))));
  Mask closed = Not(Dilate(Not(Dilate(m, w, h, r)), w, h, r));
  for (size_t i = 0; i < n; ++i) closed[i] |= m[i];

  // components (4-connected on the torus), each window's rectangle
  std::vector<int32_t> label(n, 0);
  std::vector<double> rect(n * 4, 0.0);
  std::vector<uint8_t> inside(n, 0);
  int count = 0;
  std::vector<uint32_t> pixels, stack;
  std::vector<uint8_t> occ_x(static_cast<size_t>(w)), occ_y(static_cast<size_t>(h));
  for (size_t start = 0; start < n; ++start) {
    if (!closed[start] || label[start]) continue;
    ++count;
    pixels.clear();
    stack.assign(1, uint32_t(start));
    label[start] = count;
    while (!stack.empty()) {
      const uint32_t p = stack.back();
      stack.pop_back();
      pixels.push_back(p);
      const int y = int(p / w), x = int(p % w);
      const int nb[4][2] = {{y - 1, x}, {y + 1, x}, {y, x - 1}, {y, x + 1}};
      for (auto& q : nb) {
        const size_t i = size_t((q[0] + h) % h) * w + size_t((q[1] + w) % w);
        if (closed[i] && !label[i]) {
          label[i] = count;
          stack.push_back(uint32_t(i));
        }
      }
    }
    std::fill(occ_x.begin(), occ_x.end(), 0);
    std::fill(occ_y.begin(), occ_y.end(), 0);
    for (uint32_t p : pixels) {
      occ_x[p % w] = 1;
      occ_y[p / w] = 1;
    }
    const auto [x0, x1] = UnwrappedSpan(occ_x, w);
    const auto [y0, y1] = UnwrappedSpan(occ_y, h);
    const double u0 = double(x0) / w, u1 = double(x1) / w, v0 = double(y0) / h, v1 = double(y1) / h;
    for (uint32_t p : pixels) {
      double* o = &rect[size_t(p) * 4];
      o[0] = u0;
      o[1] = v0;
      o[2] = u1;
      o[3] = v1;
      if (int(p % w) < x0) {
        o[0] -= 1.0;
        o[2] -= 1.0;
      }
      if (int(p / w) < y0) {
        o[1] -= 1.0;
        o[3] -= 1.0;
      }
      inside[p] = 1;
    }
  }
  // encode: (x + 0.5) / 2, clipped, * 255 + 0.5 truncated; 0 outside windows
  std::vector<uint8_t> out = DdsHeader(width, height);
  out.resize(128 + n * 4);
  for (size_t i = 0; i < n * 4; ++i) {
    const double e = inside[i / 4] ? (rect[i] + 0.5) / 2.0 : 0.0;
    out[128 + i] = uint8_t(std::min(std::max(e, 0.0), 1.0) * 255.0 + 0.5);
  }
  return out;
}

int MakeTextures(const Pack& pack, const fs::path& game_dir, const fs::path& out_dir, std::atomic<bool>& cancel,
                 const std::function<void(double, const std::string&)>& progress) {
  const std::vector<uint8_t> text = pack.Get("textures.txt");
  std::istringstream in(std::string(text.begin(), text.end()));
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) lines.push_back(line);
  }
  struct Source {
    std::string path, sha;
    uint64_t offset = 0;
    size_t size = 0;
    uint32_t w = 0, h = 0, format = 0;
  };
  std::map<std::string, Source> sources;
  int written = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (cancel) throw Failure("cancelled");
    std::istringstream words(lines[i]);
    std::string kind, name;
    words >> kind >> name;
    progress(double(i) / double(lines.size()), name);
    if (kind == "own" || kind == "root") {
      const std::vector<uint8_t> data = pack.Get(name);
      const fs::path dir = kind == "own" ? out_dir : out_dir.parent_path();
      WriteAll(dir / fs::path(Widen(name.substr(name.find('/') + 1))), data.data(), data.size());
      ++written;
    } else if (kind == "src") {
      Source s;
      words >> s.path >> s.offset >> s.size >> s.w >> s.h >> s.format >> s.sha;
      sources[name] = s;
    } else if (kind == "window" || kind == "diff") {
      const Source& s = sources.at(name);
      const std::vector<uint8_t> tiled = ReadRange(game_dir / fs::path(Widen(s.path)), s.offset, s.size);
      Sha256 h;
      h.Update(tiled.data(), tiled.size());
      if (tiled.size() != s.size || h.Hex() != s.sha) throw Failure("wrong_game:" + s.path);
      const std::vector<uint8_t> rgba = DecodeGameTexture(tiled, s.w, s.h, s.format);
      if (kind == "window") {
        double kenv[3];
        words >> kenv[0] >> kenv[1] >> kenv[2];
        const std::vector<uint8_t> dds = WindowMap(rgba, s.w, s.h, kenv);
        WriteAll(out_dir / L"windows" / Widen(name + "_win.dds"), dds.data(), dds.size());
      } else {
        std::string patch;
        words >> patch;
        std::vector<uint8_t> dds = pack.Get(patch);
        if (dds.size() != 128 + rgba.size()) throw std::runtime_error(patch + ": wrong size");
        for (size_t p = 0; p < rgba.size(); ++p) dds[128 + p] = uint8_t(dds[128 + p] + rgba[p]);
        WriteAll(out_dir / Widen(name + ".dds"), dds.data(), dds.size());
      }
      ++written;
    }
  }
  progress(1, "");
  return written;
}

}  // namespace srb
