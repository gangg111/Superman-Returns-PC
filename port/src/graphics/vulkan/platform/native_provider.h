#pragma once
#include "../../shaders/vulkan_shader_service.h"
#include "../frame_generation.h"
#include <rex/ui/graphics_provider.h>
namespace superman_returns::graphics::vulkan {
struct NativeProviderConfig {
  shaders::VulkanShaderConfig shaders;
  std::filesystem::path driver_cache;
  std::string gpu_uuid;
  bool validation=false,vsync=true;
  FrameGenerationConfig frame_generation;
  int anisotropy=-1;  // sr_native_anisotropic_filtering
};
std::unique_ptr<rex::ui::GraphicsProvider> CreateNativeVulkanProvider(NativeProviderConfig);
}
