#include "device_requirements.h"
#include "test_main.h"
using namespace superman_returns::graphics::vulkan;
namespace shaders=superman_returns::graphics::shaders;
SR_TEST(device_requirements_checks_stage_components_and_union) {
  shaders::ShaderRequirements vs{},ps{};
  DeviceCaps caps{};
  caps.stage.storage_buffers=34;caps.stage.vertex_output_components=64;caps.stage.descriptor_sets=4;
  caps.layout.storage_buffers=34;caps.layout.descriptor_sets=4;
  vs.vertex_output_components=80;
  SR_CHECK(!CheckDeviceRequirements(vs,ps,{},caps).empty());
  vs.vertex_output_components=0;vs.storage_buffers=18;ps.storage_buffers=17;
  std::vector<LayoutBinding> layout{{0,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,18,VK_SHADER_STAGE_VERTEX_BIT},
                                  {3,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,17,VK_SHADER_STAGE_FRAGMENT_BIT}};
  auto errors=CheckDeviceRequirements(vs,ps,layout,caps);
  SR_CHECK(!errors.empty());
  caps.layout.storage_buffers=35;
  SR_CHECK(CheckDeviceRequirements(vs,ps,layout,caps).empty());
}
SR_TEST(layout_shared_bindings_count_once_and_reject_conflicts) {
  DeviceCaps caps{};caps.stage.storage_buffers=34;caps.stage.descriptor_sets=4;
  caps.layout.storage_buffers=34;caps.layout.descriptor_sets=4;
  shaders::ShaderRequirements vs{},ps{};vs.storage_buffers=34;ps.storage_buffers=34;
  std::vector<LayoutBinding> layout{{0,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,34,VK_SHADER_STAGE_VERTEX_BIT},
                                  {0,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,34,VK_SHADER_STAGE_FRAGMENT_BIT}};
  SR_CHECK(CheckDeviceRequirements(vs,ps,layout,caps).empty());
  layout.back().count=33;
  SR_CHECK(!CheckDeviceRequirements(vs,ps,layout,caps).empty());
}

SR_TEST(game_layout_binds_the_constants_with_dynamic_offsets_inside_the_guaranteed_limits) {
  uint32_t dynamic = 0;
  for (const auto& b : GameBindingLayout()) {
    if (b.set == 0) {
      SR_CHECK(b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);
      SR_CHECK_EQ(b.count, 1u);
      ++dynamic;
    } else {
      SR_CHECK(b.type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);
    }
  }
  SR_CHECK_EQ(dynamic, 3u);  // o Vulkan garante maxDescriptorSetStorageBuffersDynamic >= 4
  DeviceCaps caps{};
  // 2D, 3D, cube and 2D-array tables of 32 images each.
  caps.stage.storage_buffers = 35;caps.stage.sampled_images = 128;caps.stage.samplers = 32;caps.stage.descriptor_sets = 4;
  caps.layout = caps.stage;
  SR_CHECK(CheckDeviceRequirements({}, {}, GameBindingLayout(), caps).empty());
  caps.layout.storage_buffers = 34;  // 3 dinâmicos + 32 de vertex buffers exigem 35
  SR_CHECK(!CheckDeviceRequirements({}, {}, GameBindingLayout(), caps).empty());
}
