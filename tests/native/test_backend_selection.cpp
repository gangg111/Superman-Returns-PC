#include "test_main.h"
#include "../../port/src/graphics/backend_selection.h"
using namespace superman_returns::graphics;
SR_TEST(native_vulkan_frame_generation_requires_compiled_backend) {
  NativeVulkanOptions options;options.lsfg=true;std::string error;
  SR_CHECK(ValidateNativeApi(NativeApi::kVulkan,true,options,error,true));
  options.fxaa=true;
  SR_CHECK(!ValidateNativeApi(NativeApi::kVulkan,true,options,error,true));
  SR_CHECK(error.find("FXAA")!=std::string::npos);
}
SR_TEST(native_api_selection_is_explicit_and_rejects_unavailable_vulkan) {
  NativeApi api=NativeApi::kD3D12;std::string error;
  SR_CHECK(ParseNativeApi("d3d12",api,error));SR_CHECK(api==NativeApi::kD3D12);
  SR_CHECK(ParseNativeApi("vulkan",api,error));SR_CHECK(api==NativeApi::kVulkan);
  SR_CHECK(!ParseNativeApi("auto",api,error));SR_CHECK(!error.empty());
  SR_CHECK(!ValidateNativeApi(NativeApi::kVulkan,false,{},error));
  SR_CHECK(error.find("SR_VULKAN_GAME")!=std::string::npos);
  SR_CHECK(ValidateNativeApi(NativeApi::kD3D12,false,{},error));
}
SR_TEST(native_vulkan_rejects_options_it_cannot_apply) {
  std::string error;NativeVulkanOptions options;
  SR_CHECK(ValidateNativeApi(NativeApi::kVulkan,true,options,error));
  options.fxaa=true;SR_CHECK(!ValidateNativeApi(NativeApi::kVulkan,true,options,error));SR_CHECK(error.find("FXAA")!=std::string::npos);
  options={};options.ssao=true;SR_CHECK(!ValidateNativeApi(NativeApi::kVulkan,true,options,error));
  options={};options.lsfg=true;SR_CHECK(!ValidateNativeApi(NativeApi::kVulkan,true,options,error));
  options={};options.render_scale=2;SR_CHECK(!ValidateNativeApi(NativeApi::kVulkan,true,options,error));
  options={};options.msaa_samples=4;SR_CHECK(!ValidateNativeApi(NativeApi::kVulkan,true,options,error));
  options={};options.ab_mode=true;SR_CHECK(!ValidateNativeApi(NativeApi::kVulkan,true,options,error));
  options={};options.shadow_quality=2;SR_CHECK(!ValidateNativeApi(NativeApi::kVulkan,true,options,error));
  options={};options.bloom_quality=2;SR_CHECK(!ValidateNativeApi(NativeApi::kVulkan,true,options,error));
  options={};options.anisotropic_filtering=16;SR_CHECK(ValidateNativeApi(NativeApi::kVulkan,true,options,error));  // supported since d7f483e
  options={};options.render_scale=0;SR_CHECK(ValidateNativeApi(NativeApi::kVulkan,true,options,error));  // automatic: 720p on Vulkan
  options={};options.foliage_antialiasing=true;SR_CHECK(!ValidateNativeApi(NativeApi::kVulkan,true,options,error));
  SR_CHECK(ValidateNativeApi(NativeApi::kD3D12,false,options,error));
}
