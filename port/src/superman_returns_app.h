// superman_returns - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdlib>
#include <fstream>

#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/cvar.h>

#include "frame_stats.h"
#include "input/native_input.h"
#include "audio_out/xaudio2_output.h"

namespace native_audio {
bool RunAudioSelfTest(uint8_t* base);  // native_audio/selftest.cpp
}
#include "sr_settings.h"
#include "graphics/backend_selection.h"
#include "native_renderer/native_bridge.h"
#if SR_HAS_TRACE_GPU
#include "native_renderer/sr_graphics_system.h"
#endif

class SupermanReturnsApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<SupermanReturnsApp>(new SupermanReturnsApp(ctx, "superman_returns",
        PPCImageConfig));
  }

  // Pick the graphics backend. With the in-process GPU sources linked, the
  // xenos and trace modes use them (see CreateSrXenosGraphicsSystem);
  // otherwise fall back to the Xenos plugin DLL staged next to the executable.
  // sr_renderer=native (docs/native-port-plan.md) uses the native renderer
  // when the build and the game profile allow it, else xenos.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Native Windows input: GameInput controllers, Raw Input keyboard and mouse.
    config.input_factory = REX_INPUT_BACKEND(superman_returns::input::CreateInputSystem);
    // Native Windows audio output (XAudio2) unless sr_audio_output=sdl.
    if (superman_returns::audio_out::XAudio2Selected())
      config.audio_factory = REX_AUDIO_BACKEND(superman_returns::audio_out::XAudio2AudioSystem);
    // Before anything reads the options a preset manages (sr_post_effects
    // below, sr_render_scale in render_scale.cpp).
    ApplySrPreset();
#if SR_HAS_LSFG
    REXLOG_INFO("LSFG build enabled; requested={}, flag={}",
        rex::cvar::Query<bool>("sr_lsfg"), rex::cvar::GetFlagByName("sr_lsfg"));
#endif
#if !SR_HAS_LSFG
    if (rex::cvar::Query<bool>("sr_lsfg"))
      REXLOG_WARN("sr_lsfg requires a build with SR_LSFG=ON; using normal presentation");
#endif
    const std::string renderer = rex::cvar::Query<std::string>("sr_renderer");
    const bool trace = renderer == "trace";
    const bool native = renderer == "native";
    superman_returns::graphics::NativeApi native_api;std::string api_error;
    if(!superman_returns::graphics::ParseNativeApi(rex::cvar::Query<std::string>("sr_native_api"),native_api,api_error)) {
      rex::FatalError(api_error);return;
    }
    if(native_api==superman_returns::graphics::NativeApi::kVulkan && !native) {
      rex::FatalError("sr_native_api=vulkan requires sr_renderer=native");return;
    }
#if !SR_HAS_NATIVE || !SR_HAS_TRACE_GPU
    if(native_api==superman_returns::graphics::NativeApi::kVulkan) {
      rex::FatalError("Native Vulkan requires a build with SR_NATIVE=RENDERER and SR_VULKAN_GAME=ON");return;
    }
#endif
#if SR_HAS_TRACE_GPU
    // GPU defaults measured on Intel UHD (open world, paired runs); an explicit
    // command-line or config value always wins.
    //   render_target_path_d3d12=rtv: ~2 FPS with ROV, ~7 with RTV.
    //   depth_float24_convert_in_pixel_shader: without it the sky and far
    //     scenery fail the depth test and render black under RTV (~5% cost).
    //   native_stencil_value_output_d3d12_intel: the ROV-era Intel workaround
    //     is not needed on current drivers; 7.6 -> 10.2 FPS in the street.
    auto gpu_default = [](const char* name, const char* value) {
      if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) {
        rex::cvar::SetFlagByName(name, value);
      }
    };
    gpu_default("render_target_path_d3d12", "rtv");
    gpu_default("depth_float24_convert_in_pixel_shader", "true");
    gpu_default("native_stencil_value_output_d3d12_intel", "true");
    if (!config.graphics && native) {
#if SR_HAS_NATIVE
      config.graphics = superman_returns::native::CreateNativeGraphicsSystem(
          &CreateSrXenosGraphicsSystem, &CreateSrAbGraphicsSystem);
      if (!config.graphics) rex::FatalError("sr_renderer=native: the native renderer could not be created");
#else
      rex::FatalError("sr_renderer=native: this build has no native renderer (CMake "
                      "SR_NATIVE=RENDERER); select Xenos explicitly (sr_renderer=xenos)");
#endif
    }
    if (!config.graphics) {
      config.graphics = trace ? CreateSrTraceGraphicsSystem() : CreateSrXenosGraphicsSystem();
    }
#else
    if (trace) {
      REXLOG_ERROR("sr_renderer=trace requires the ReXGlue v0.10.0 GPU sources");
    }
    if (native) {
      rex::FatalError("sr_renderer=native requires the ReXGlue v0.10.0 GPU sources and CMake "
                      "SR_NATIVE=RENDERER; select Xenos explicitly (sr_renderer=xenos)");
    }
    if (rex::cvar::GetFlagByName("sr_post_effects") == "false") {
      REXLOG_WARN("sr_post_effects=false requires the ReXGlue v0.10.0 GPU sources "
                  "(tools/setup_gpu_source.ps1); post effects stay on");
    }
#endif
    if (!config.graphics && config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
  }

  // Debug aid: set SR_DUMP_IMAGE=<path> to write the loaded guest image
  // (0x82000000-0x829E0000) for offline analysis of missed functions.
  void OnPostLoadXexImage() override {
    const char* path = std::getenv("SR_DUMP_IMAGE");
    if (!path || !*path) return;
    constexpr uint32_t kImageBase = 0x82000000;
    constexpr uint32_t kImageEnd = 0x829E0000;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(runtime()->virtual_membase() + kImageBase),
              kImageEnd - kImageBase);
  }

  // Feed the F3 debug overlay with the guest frame rate.
  void OnPostSetup() override {
    SetGuestFrameStats(SampleGuestFrameStats);
    // SR_AUDIO_SELFTEST=<report>: native audio DSP vs the recompiled originals, then exit.
    if (native_audio::RunAudioSelfTest(runtime()->virtual_membase())) std::exit(0);
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};

