#include "backend_selection.h"
namespace superman_returns::graphics {
bool ParseNativeApi(std::string_view text,NativeApi& api,std::string& error) {
  if(text=="d3d12") api=NativeApi::kD3D12;
  else if(text=="vulkan") api=NativeApi::kVulkan;
  else {error="Invalid sr_native_api: expected d3d12 or vulkan";return false;}
  error.clear();return true;
}
bool ValidateNativeApi(NativeApi api,bool compiled,const NativeVulkanOptions& options,std::string& error,bool lsfg_compiled) {
  error.clear();if(api==NativeApi::kD3D12) return true;
  if(api!=NativeApi::kVulkan) {error="Invalid native API";return false;}
  if(!compiled) error="Native Vulkan unavailable: rebuild with SR_VULKAN_GAME=ON and SR_VULKAN_FOUNDATION=ON";
  else if(options.fxaa) error="Native Vulkan does not yet support FXAA (sr_native_fxaa)";
  else if(options.ssao) error="Native Vulkan does not yet support SSAO (sr_native_ambient_occlusion)";
  else if(options.lsfg && !lsfg_compiled) error="Native Vulkan frame generation unavailable: rebuild with SR_LSFG=ON";
  else if(options.ab_mode) error="Native Vulkan cannot use the Xenos A/B presenter";
  // 0 = automatic: Vulkan has no internal scaling yet and renders at 720p.
  else if(options.render_scale!=1 && options.render_scale!=0) error="Native Vulkan currently requires sr_native_render_scale=1 (or 0, automatic)";
  else if(options.msaa_samples!=1) error="Native Vulkan currently requires sr_native_msaa_samples=1";
  else if(options.shadow_quality!=1) error="Native Vulkan currently requires sr_native_shadow_quality=1";
  else if(options.bloom_quality!=1) error="Native Vulkan currently requires sr_native_bloom_quality=1";
  else if(options.foliage_antialiasing) error="Native Vulkan does not yet support sr_native_foliage_antialiasing";
  return error.empty();
}
}
