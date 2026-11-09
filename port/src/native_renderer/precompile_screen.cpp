// First-start pipeline compilation behind a progress screen.
//
// The recorded pipelines (the set embedded in the executable, RCDATA 3, plus the
// local superman_returns_pipelines.bin) used to be compiled by 1-4 background
// threads while the game ran; on a cold driver shader cache whatever was not
// done yet compiled on the render worker when first drawn: 60 pipelines over
// 30 ms, 12.4 s in all, up to 1.1 s each, in the menus and the city load
// (a 3.3 s frame). Now the renderer's initialization waits until all of them are
// compiled, on all cores. When that takes longer than a moment (a cold driver
// cache: the first start, a new driver or a new build) it shows a progress
// screen; on later starts the driver's own cache answers within a second.
//
// The screen is drawn with ClearRenderTargetView rectangles: a background, a
// title and a percentage in a built-in 5x7 pixel font, scaled by whole pixels,
// and a progress bar. No textures, shaders or system fonts.

#include "native_renderer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_presenter.h>

REXCVAR_DECLARE(bool, sr_native_pipeline_cache);
REXCVAR_DECLARE(bool, sr_native_ab_mode);
REXCVAR_DECLARE(bool, sr_native_debug_no_present);
REXCVAR_DEFINE_BOOL(sr_precompile_screen, true, "Superman Returns Native",
                    "Compile the recorded pipelines on all cores before the first frame, with a "
                    "progress screen when it takes a while");

namespace superman_returns::native {
namespace {

// 5x7 glyphs, one byte per row, bit 4 = leftmost column.
struct Glyph {
  char c;
  uint8_t rows[7];
};
constexpr Glyph kGlyphs[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}},
};

const Glyph* FindGlyph(char c) {
  for (const Glyph& g : kGlyphs) {
    if (g.c == c) return &g;
  }
  return nullptr;  // space
}

// Glyph cells are 6 pixels wide (5 + spacing), rows 7 high.
LONG TextWidth(const char* text, LONG scale) { return LONG(std::strlen(text)) * 6 * scale - scale; }

// Rectangles of `text` at (x, y), one per horizontal run of lit pixels.
void TextRects(const char* text, LONG x, LONG y, LONG scale, std::vector<D3D12_RECT>& out) {
  for (const char* p = text; *p; ++p, x += 6 * scale) {
    const Glyph* g = FindGlyph(*p);
    if (!g) continue;
    for (LONG row = 0; row < 7; ++row) {
      for (LONG col = 0; col < 5;) {
        if (!(g->rows[row] & (0x10 >> col))) {
          ++col;
          continue;
        }
        LONG end = col;
        while (end < 5 && (g->rows[row] & (0x10 >> end))) ++end;
        out.push_back({x + col * scale, y + row * scale, x + end * scale, y + (row + 1) * scale});
        col = end;
      }
    }
  }
}

// Clears `rects` in batches of 16: the NVIDIA driver (nvwgf2umx) copies the
// rectangles of one ClearRenderTargetView into a fixed stack buffer and fails
// fast (0xC0000409, stack cookie) on a title's ~200.
void ClearRects(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                const float color[4], const std::vector<D3D12_RECT>& rects) {
  for (size_t i = 0; i < rects.size(); i += 16) {
    list->ClearRenderTargetView(rtv, color, UINT(std::min<size_t>(16, rects.size() - i)),
                                rects.data() + i);
  }
}

}  // namespace

void Renderer::RunPrecompileScreen() {
  if (!REXCVAR_GET(sr_precompile_screen) || !REXCVAR_GET(sr_native_pipeline_cache)) return;
  size_t done = 0, total = 0;
  pipeline_cache_.StartupProgress(done, total);
  if (done >= total) return;
  // The background threads plus one per remaining core; they leave when done.
  const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
  pipeline_cache_.AddStartupThreads(cores - 1);
  const auto start = std::chrono::steady_clock::now();
  bool shown = false;
  for (;;) {
    pipeline_cache_.StartupProgress(done, total);
    if (done >= total) break;
    // A warm driver cache finishes within a moment: no screen for that.
    if (shown || std::chrono::steady_clock::now() - start > std::chrono::milliseconds(1500)) {
      shown = true;
      DrawPrecompileFrame(done, total);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  if (shown) DrawPrecompileFrame(total, total);
  REXLOG_INFO("native: {} pipelines compiled before the first frame in {:.1f} s{}", total,
              std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
              shown ? " (progress screen)" : "");
}

void Renderer::DrawPrecompileFrame(size_t done, size_t total) {
  // This slot's previous frame and its present are done (as BeginFrame).
  if (frame_fence_values_[frame_index_] &&
      fence_->GetCompletedValue() < frame_fence_values_[frame_index_]) {
    fence_->SetEventOnCompletion(frame_fence_values_[frame_index_], nullptr);
  }
  if (present_fence_ && present_fence_values_[frame_index_] &&
      present_fence_->GetCompletedValue() < present_fence_values_[frame_index_]) {
    present_fence_->SetEventOnCompletion(present_fence_values_[frame_index_], nullptr);
  }
  allocators_[frame_index_]->Reset();
  command_list_->Reset(allocators_[frame_index_].Get(), nullptr);
  frame_open_ = true;
  Transition(output_rt(), output_rt_states_[frame_index_], D3D12_RESOURCE_STATE_RENDER_TARGET);

  const LONG w = LONG(output_width_), h = LONG(output_height_);
  const LONG scale = std::max<LONG>(2, h / 160);
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = output_rtvs_[frame_index_];
  const float background[4] = {0.012f, 0.016f, 0.03f, 1.0f};
  const float title_color[4] = {0.86f, 0.89f, 0.95f, 1.0f};
  const float track_color[4] = {0.07f, 0.09f, 0.15f, 1.0f};
  const float fill_color[4] = {0.13f, 0.38f, 0.92f, 1.0f};  // Superman blue
  const float accent_color[4] = {0.84f, 0.12f, 0.12f, 1.0f};  // and red
  command_list_->ClearRenderTargetView(rtv, background, 0, nullptr);

  const char* title = "COMPILING SHADERS";
  std::vector<D3D12_RECT> rects;
  const LONG title_y = h / 2 - 16 * scale;
  TextRects(title, (w - TextWidth(title, scale)) / 2, title_y, scale, rects);
  ClearRects(command_list_.Get(), rtv, title_color, rects);

  const LONG bar_w = std::min(w * 6 / 10, TextWidth(title, scale) * 3 / 2), bar_h = 2 * scale;
  const LONG bar_x = (w - bar_w) / 2, bar_y = h / 2;
  const D3D12_RECT track{bar_x, bar_y, bar_x + bar_w, bar_y + bar_h};
  command_list_->ClearRenderTargetView(rtv, track_color, 1, &track);
  const double fraction = total ? double(done) / double(total) : 1.0;
  const LONG fill_w = LONG(double(bar_w) * fraction);
  if (fill_w > 0) {
    const D3D12_RECT fill{bar_x, bar_y, bar_x + fill_w, bar_y + bar_h};
    command_list_->ClearRenderTargetView(rtv, fill_color, 1, &fill);
    const D3D12_RECT tip{bar_x + std::max<LONG>(0, fill_w - scale), bar_y, bar_x + fill_w,
                         bar_y + bar_h};
    command_list_->ClearRenderTargetView(rtv, accent_color, 1, &tip);
  }

  char percent[8];
  std::snprintf(percent, sizeof(percent), "%d%%", int(fraction * 100.0));
  rects.clear();
  TextRects(percent, (w - TextWidth(percent, scale)) / 2, bar_y + bar_h + 6 * scale, scale, rects);
  ClearRects(command_list_.Get(), rtv, title_color, rects);

  if (!REXCVAR_GET(sr_native_ab_mode) && !REXCVAR_GET(sr_native_debug_no_present)) {
    presenter_->RefreshGuestOutput(
        output_width_, output_height_, 1280, 720,
        [this](rex::ui::Presenter::GuestOutputRefreshContext& context) {
          return CopyFrameToPresenter(context);
        });
  }
  if (frame_open_) {
    command_list_->Close();
    ID3D12CommandList* lists[] = {command_list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    frame_open_ = false;
  }
  queue_->Signal(fence_.Get(), next_fence_value_);
  frame_fence_values_[frame_index_] = next_fence_value_++;
  // Debug: SR_PRECOMPILE_DUMP=1 dumps one screen past half way (sr_native_dump_dir).
  static bool dumped = false;
  if (!dumped && 2 * done >= total && std::getenv("SR_PRECOMPILE_DUMP")) {
    dumped = true;
    DumpFrameResources("precompile_", false, true);
  }
  frame_index_ = (frame_index_ + 1) % kFramesInFlight;
}

}  // namespace superman_returns::native
