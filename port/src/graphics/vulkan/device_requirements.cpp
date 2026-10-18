#include "device_requirements.h"
#include <algorithm>
#include <map>
namespace superman_returns::graphics::vulkan {
DeviceCaps GetDeviceCaps(const VkPhysicalDeviceLimits& l,const VkPhysicalDeviceFeatures& f) {
  DeviceCaps c;
  c.stage.storage_buffers=l.maxPerStageDescriptorStorageBuffers;
  c.stage.uniform_buffers=l.maxPerStageDescriptorUniformBuffers;
  c.stage.sampled_images=l.maxPerStageDescriptorSampledImages;
  c.stage.samplers=l.maxPerStageDescriptorSamplers;
  c.stage.descriptor_sets=l.maxBoundDescriptorSets;
  c.stage.vertex_attributes=l.maxVertexInputAttributes;
  c.stage.vertex_output_components=l.maxVertexOutputComponents;
  c.stage.fragment_input_components=l.maxFragmentInputComponents;
  c.stage.sampled_image_dynamic_indexing=f.shaderSampledImageArrayDynamicIndexing;
  c.stage.storage_buffer_dynamic_indexing=f.shaderStorageBufferArrayDynamicIndexing;
  c.stage.clip_distance=f.shaderClipDistance;
  c.stage.cull_distance=f.shaderCullDistance;
  c.layout.storage_buffers=l.maxDescriptorSetStorageBuffers;
  c.layout.uniform_buffers=l.maxDescriptorSetUniformBuffers;
  c.layout.sampled_images=l.maxDescriptorSetSampledImages;
  c.layout.samplers=l.maxDescriptorSetSamplers;
  c.layout.descriptor_sets=l.maxBoundDescriptorSets;
  c.stage_resources=l.maxPerStageResources;
  return c;
}
std::vector<std::string> CheckDeviceRequirements(const shaders::ShaderRequirements& vs,
    const shaders::ShaderRequirements& ps,std::span<const LayoutBinding> bindings,const DeviceCaps& caps) {
  std::vector<std::string> errors;
  auto stage_check=[&](const auto& r,const char* name) {
    for(auto& e:shaders::CheckRequirements(r,caps.stage)) errors.push_back(std::string(name)+": "+e);
  };
  stage_check(vs,"VS");stage_check(ps,"PS");
  std::map<std::pair<uint32_t,uint32_t>,LayoutBinding> merged;
  for(const auto& b:bindings) {
    if(!b.count || !b.stages || (b.stages&~(VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT))) {
      errors.push_back("Invalid game descriptor count or visibility");continue;
    }
    auto [it,inserted]=merged.emplace(std::pair{b.set,b.binding},b);
    if(!inserted) {
      if(it->second.type!=b.type || it->second.count!=b.count) errors.push_back("Conflicting descriptor binding");
      else it->second.stages|=b.stages;
    }
  }
  // Count the actual layout, not just descriptors retained by shader optimization.
  struct Counts {uint64_t storage=0,uniform=0,images=0,samplers=0,sets=0;};
  Counts total,v,p;
  auto add=[&](Counts& c,const LayoutBinding& b) {
    c.sets=std::max(c.sets,uint64_t(b.set)+1);
    switch(b.type) {
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:c.storage+=b.count;break;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:c.uniform+=b.count;break;
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:c.images+=b.count;break;
    case VK_DESCRIPTOR_TYPE_SAMPLER:c.samplers+=b.count;break;
    default:errors.push_back("Descriptor type outside game ABI");break;
    }
  };
  for(auto& [key,b]:merged) {
    add(total,b);
    if(b.stages&VK_SHADER_STAGE_VERTEX_BIT) add(v,b);
    if(b.stages&VK_SHADER_STAGE_FRAGMENT_BIT) add(p,b);
  }
  auto check=[&](const Counts& c,const shaders::ShaderCapabilities& cap,const char* label,bool per_stage) {
    auto limit=[&](const char* name,uint64_t used,uint32_t available) {
      if(used>available) errors.push_back(std::string(label)+" "+name+" requires "+std::to_string(used)+", available "+std::to_string(available));
    };
    limit("storage_buffers",c.storage,cap.storage_buffers);
    limit("uniform_buffers",c.uniform,cap.uniform_buffers);
    limit("sampled_images",c.images,cap.sampled_images);
    limit("samplers",c.samplers,cap.samplers);
    limit("descriptor_sets",c.sets,cap.descriptor_sets);
    // Separate SAMPLER descriptors do not count against maxPerStageResources.
    if(per_stage) limit("resources",c.storage+c.uniform+c.images,caps.stage_resources);
  };
  check(total,caps.layout,"layout",false);check(v,caps.stage,"VS layout",true);check(p,caps.stage,"PS layout",true);
  return errors;
}
std::vector<LayoutBinding> GameBindingLayout() {
  constexpr auto both=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
  // Set 0 holds the draw's VS/PS/shared constants: dynamic offsets avoid allocating and updating a set per draw.
  return {{0,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_VERTEX_BIT},
          {0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_FRAGMENT_BIT},
          {0,2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,1,both},
          {1,0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,32,both},
          {1,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,32,both},
          {1,2,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,32,both},
          {1,3,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,32,both},  // 2D array views (stacked textures read by tfetch3D)
          {2,0,VK_DESCRIPTOR_TYPE_SAMPLER,32,both},
          {3,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,32,VK_SHADER_STAGE_VERTEX_BIT}};
}
} // namespace superman_returns::graphics::vulkan
