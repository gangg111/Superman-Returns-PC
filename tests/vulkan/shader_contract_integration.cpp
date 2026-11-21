// GPU verification of the project-owned per-draw shader ABI; not a game
// renderer.
#include "../../port/src/graphics/guest/constant_snapshot.h"
#include "../../port/src/graphics/guest/draw_state.h"
#include "../../port/src/graphics/shaders/shader_requirements.h"
#include "context.h"
#include "descriptor_sets.h"
#include "constant_bank_cache.h"
#include "platform/win32_surface.h"
#include "triangle.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace superman_returns::graphics::vulkan;
namespace guest = superman_returns::graphics::guest;
namespace shaders = superman_returns::graphics::shaders;
#define CONTRACT_FUNCTIONS(X)                                                  \
  X(vkCreateImage)                                                             \
  X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)       \
      X(vkCreateSampler) X(vkDestroySampler) X(vkCreateDescriptorSetLayout)    \
          X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool)            \
              X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets)           \
                  X(vkUpdateDescriptorSets) X(vkCmdPipelineBarrier)            \
                      X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer)      \
                          X(vkCmdBindDescriptorSets) X(vkCmdBindIndexBuffer)   \
                              X(vkCmdDrawIndexed)                              \
                                  X(vkInvalidateMappedMemoryRanges)
struct TestDispatch {
#define MEMBER(n) PFN_##n n = nullptr;
  CONTRACT_FUNCTIONS(MEMBER)
#undef MEMBER
};
void Require(VkResult r, const char *what) {
  if (r != VK_SUCCESS)
    throw std::runtime_error(std::string(what) +
                             " result=" + std::to_string(r));
}
struct Buffer {
  VkBuffer handle{};
  VkDeviceMemory memory{};
  VkDeviceSize size{}, allocation{};
  bool coherent{};
};
struct Image {
  VkImage handle{};
  VkDeviceMemory memory{};
  VkImageView view{};
};
class Fixture {
public:
  Context &c;
  TestDispatch f;
  std::vector<Buffer> buffers;
  std::vector<Image> images;
  VkRenderPass pass{};
  VkFramebuffer framebuffer{};
  VkPipeline pipeline{};
  VkPipelineLayout pipeline_layout{};
  std::array<VkDescriptorSetLayout, 4> layouts{};
  std::array<VkDescriptorSet, 4> sets{};
  std::array<uint32_t, 3> dynamic_offsets{};uint32_t dynamic_count = 0;
  VkDescriptorPool descriptor_pool{};
  VkSampler sampler{};
  std::vector<VkShaderModule> modules;
  VkCommandPool command_pool{};
  VkCommandBuffer command{};
  VkFence fence{};
  Fixture(Context &context) : c(context) {
#define LOAD(n)                                                                \
  f.n = reinterpret_cast<PFN_##n>(c.f.vkGetDeviceProcAddr(c.device, #n));      \
  if (!f.n)                                                                    \
    throw std::runtime_error(#n " missing");
    CONTRACT_FUNCTIONS(LOAD)
#undef LOAD
  }
  ~Fixture() {
    c.f.vkDeviceWaitIdle(c.device);
    if (fence)
      c.f.vkDestroyFence(c.device, fence, nullptr);
    if (command_pool)
      c.f.vkDestroyCommandPool(c.device, command_pool, nullptr);
    if (pipeline)
      c.f.vkDestroyPipeline(c.device, pipeline, nullptr);
    for (auto m : modules)
      c.f.vkDestroyShaderModule(c.device, m, nullptr);
    if (pipeline_layout)
      c.f.vkDestroyPipelineLayout(c.device, pipeline_layout, nullptr);
    if (descriptor_pool)
      f.vkDestroyDescriptorPool(c.device, descriptor_pool, nullptr);
    for (auto l : layouts)
      if (l)
        f.vkDestroyDescriptorSetLayout(c.device, l, nullptr);
    if (sampler)
      f.vkDestroySampler(c.device, sampler, nullptr);
    if (framebuffer)
      c.f.vkDestroyFramebuffer(c.device, framebuffer, nullptr);
    if (pass)
      c.f.vkDestroyRenderPass(c.device, pass, nullptr);
    for (auto &i : images) {
      if (i.view)
        c.f.vkDestroyImageView(c.device, i.view, nullptr);
      if (i.handle)
        f.vkDestroyImage(c.device, i.handle, nullptr);
      if (i.memory)
        c.f.vkFreeMemory(c.device, i.memory, nullptr);
    }
    for (auto &b : buffers) {
      if (b.handle)
        c.f.vkDestroyBuffer(c.device, b.handle, nullptr);
      if (b.memory)
        c.f.vkFreeMemory(c.device, b.memory, nullptr);
    }
  }
  uint32_t MemoryType(uint32_t allowed, VkMemoryPropertyFlags required,
                      VkMemoryPropertyFlags preferred) {
    std::vector<VkMemoryPropertyFlags> flags;
    for (uint32_t i = 0; i < c.memory.memoryTypeCount; ++i)
      flags.push_back(c.memory.memoryTypes[i].propertyFlags);
    auto result = ChooseMemoryType(allowed, flags, required, preferred);
    if (!result)
      throw std::runtime_error("Required test memory type unavailable");
    return *result;
  }
  size_t NewBuffer(size_t bytes, VkBufferUsageFlags usage) {
    buffers.emplace_back();
    size_t index = buffers.size() - 1;
    auto &b = buffers[index];
    b.size = bytes;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Require(c.f.vkCreateBuffer(c.device, &info, nullptr, &b.handle),
            "CreateBuffer");
    VkMemoryRequirements mr;
    c.f.vkGetBufferMemoryRequirements(c.device, b.handle, &mr);
    b.allocation = mr.size;
    auto type =
        MemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    b.coherent = (c.memory.memoryTypes[type].propertyFlags &
                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = mr.size;
    alloc.memoryTypeIndex = type;
    Require(c.f.vkAllocateMemory(c.device, &alloc, nullptr, &b.memory),
            "AllocateBufferMemory");
    Require(c.f.vkBindBufferMemory(c.device, b.handle, b.memory, 0),
            "BindBufferMemory");
    return index;
  }
  void Write(size_t index, std::span<const std::byte> bytes) {
    auto &b = buffers[index];
    if (bytes.size() > b.size)
      throw std::runtime_error("Upload too large");
    void *mapped;
    Require(c.f.vkMapMemory(c.device, b.memory, 0, VK_WHOLE_SIZE, 0, &mapped),
            "Map upload");
    std::memset(mapped, 0, size_t(b.size));
    std::memcpy(mapped, bytes.data(), bytes.size());
    VkResult flushed = VK_SUCCESS;
    if (!b.coherent) {
      VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
      range.memory = b.memory;
      range.size = VK_WHOLE_SIZE;
      flushed = c.f.vkFlushMappedMemoryRanges(c.device, 1, &range);
    }
    c.f.vkUnmapMemory(c.device, b.memory);
    Require(flushed, "Flush upload");
  }
  size_t NewImage(uint32_t width, uint32_t height, VkImageUsageFlags usage) {
    images.emplace_back();
    size_t index = images.size() - 1;
    auto &image = images[index];
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Require(f.vkCreateImage(c.device, &info, nullptr, &image.handle),
            "CreateImage");
    VkMemoryRequirements mr;
    f.vkGetImageMemoryRequirements(c.device, image.handle, &mr);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = mr.size;
    alloc.memoryTypeIndex =
        MemoryType(mr.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Require(c.f.vkAllocateMemory(c.device, &alloc, nullptr, &image.memory),
            "Allocate image");
    Require(f.vkBindImageMemory(c.device, image.handle, image.memory, 0),
            "Bind image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.handle;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = info.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    Require(c.f.vkCreateImageView(c.device, &view, nullptr, &image.view),
            "Image view");
    return index;
  }
  void Commands() {
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.queueFamilyIndex = c.graphics_family;
    Require(c.f.vkCreateCommandPool(c.device, &pool, nullptr, &command_pool),
            "Command pool");
    VkCommandBufferAllocateInfo info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    info.commandPool = command_pool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    Require(c.f.vkAllocateCommandBuffers(c.device, &info, &command),
            "Command buffer");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    Require(c.f.vkCreateFence(c.device, &fi, nullptr, &fence), "Fence");
  }
  void Begin() {
    Require(c.f.vkResetCommandPool(c.device, command_pool, 0), "Reset pool");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Require(c.f.vkBeginCommandBuffer(command, &bi), "Begin commands");
  }
  void Submit() {
    Require(c.f.vkEndCommandBuffer(command), "End commands");
    Require(c.f.vkResetFences(c.device, 1, &fence), "Reset fence");
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.commandBufferCount = 1;
    info.pCommandBuffers = &command;
    Require(c.f.vkQueueSubmit(c.graphics_queue, 1, &info, fence), "Submit");
    Require(c.f.vkWaitForFences(c.device, 1, &fence, VK_TRUE, 10000000000ull),
            "Wait submission");
  }
  void Barrier(VkImage image, VkImageLayout from, VkImageLayout to,
               VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags first,
               VkPipelineStageFlags last) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.vkCmdPipelineBarrier(command, first, last, 0, 0, nullptr, 0, nullptr, 1,
                           &b);
  }
  void RenderTarget(size_t image_index) {
    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo pi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pi.attachmentCount = 1;
    pi.pAttachments = &attachment;
    pi.subpassCount = 1;
    pi.pSubpasses = &sub;
    pi.dependencyCount = 2;
    pi.pDependencies = deps;
    Require(c.f.vkCreateRenderPass(c.device, &pi, nullptr, &pass),
            "Render pass");
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = pass;
    fi.attachmentCount = 1;
    fi.pAttachments = &images[image_index].view;
    fi.width = fi.height = 64;
    fi.layers = 1;
    Require(c.f.vkCreateFramebuffer(c.device, &fi, nullptr, &framebuffer),
            "Framebuffer");
  }
  void Descriptors(size_t vs, size_t ps, size_t shared, size_t vertex,
                   size_t texture) {
    std::vector<std::vector<VkDescriptorSetLayoutBinding>> bindings(4);
    bindings[0] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                    VK_SHADER_STAGE_VERTEX_BIT, nullptr},
                   {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                   {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    nullptr}};
    bindings[1] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 32,
                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    bindings[2] = {{0, VK_DESCRIPTOR_TYPE_SAMPLER, 32,
                    VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    bindings[3] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32,
                    VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
    for (size_t i = 0; i < 4; ++i) {
      VkDescriptorSetLayoutCreateInfo info{
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      info.bindingCount = uint32_t(bindings[i].size());
      info.pBindings = bindings[i].data();
      Require(
          f.vkCreateDescriptorSetLayout(c.device, &info, nullptr, &layouts[i]),
          "Descriptor layout");
    }
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 35},
                                    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 32},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, 32}};
    VkDescriptorPoolCreateInfo pool{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 4;
    pool.poolSizeCount = 3;
    pool.pPoolSizes = sizes;
    Require(
        f.vkCreateDescriptorPool(c.device, &pool, nullptr, &descriptor_pool),
        "Descriptor pool");
    VkDescriptorSetAllocateInfo alloc{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = descriptor_pool;
    alloc.descriptorSetCount = 4;
    alloc.pSetLayouts = layouts.data();
    Require(f.vkAllocateDescriptorSets(c.device, &alloc, sets.data()),
            "Allocate sets");
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0;
    Require(f.vkCreateSampler(c.device, &si, nullptr, &sampler), "Sampler");
    VkDescriptorBufferInfo constant_infos[] = {
        {buffers[vs].handle, 0, 4096},
        {buffers[ps].handle, 0, 4096},
        {buffers[shared].handle, 0, 4096}};
    std::array<VkDescriptorBufferInfo, 32> vertex_infos;
    vertex_infos.fill({buffers[vertex].handle, 0, buffers[vertex].size});
    std::array<VkDescriptorImageInfo, 32> image_infos, sampler_infos;
    image_infos.fill({VK_NULL_HANDLE, images[texture].view,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    sampler_infos.fill({sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
    VkWriteDescriptorSet writes[6]{};
    for (uint32_t i = 0; i < 6; ++i)
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    for (uint32_t i = 0; i < 3; ++i) {
      writes[i].dstSet = sets[0];
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &constant_infos[i];
    }
    writes[3].dstSet = sets[3];
    writes[3].descriptorCount = 32;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = vertex_infos.data();
    writes[4].dstSet = sets[1];
    writes[4].descriptorCount = 32;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[4].pImageInfo = image_infos.data();
    writes[5].dstSet = sets[2];
    writes[5].descriptorCount = 32;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    writes[5].pImageInfo = sampler_infos.data();
    f.vkUpdateDescriptorSets(c.device, 6, writes, 0, nullptr);
  }
  void Pipeline(const std::filesystem::path &directory, std::span<const VkDescriptorSetLayout> game_layouts={}) {
    VkPipelineLayoutCreateInfo li{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 4;
    li.pSetLayouts = game_layouts.empty()?layouts.data():game_layouts.data();
    Require(
        c.f.vkCreatePipelineLayout(c.device, &li, nullptr, &pipeline_layout),
        "Pipeline layout");
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (int i = 0; i < 2; ++i) {
      std::vector<uint32_t> words;
      Error error;
      if (!ReadSpirv(directory / (i ? "ps.spv" : "vs.spv"), words, error))
        throw std::runtime_error(error.message);
      VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      mi.codeSize = words.size() * 4;
      mi.pCode = words.data();
      VkShaderModule module{};
      Require(c.f.vkCreateShaderModule(c.device, &mi, nullptr, &module),
              "Shader module");
      modules.push_back(module);
      stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stages[i].stage =
          i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
      stages[i].module = module;
      stages[i].pName = i ? "PSMain" : "VSMain";
    }
    VkPipelineVertexInputStateCreateInfo vertex{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    assembly.primitiveRestartEnable = VK_TRUE;
    VkViewport viewport{0, 0, 64, 64, 0, 1};
    VkRect2D scissor{{0, 0}, {64, 64}};
    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    vp.pViewports = &viewport;
    vp.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState color{};
    color.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &color;
    VkGraphicsPipelineCreateInfo info{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &vp;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &ms;
    info.pColorBlendState = &blend;
    info.layout = pipeline_layout;
    info.renderPass = pass;
    Require(c.f.vkCreateGraphicsPipelines(c.device, VK_NULL_HANDLE, 1, &info,
                                          nullptr, &pipeline),
            "Graphics pipeline");
  }
  std::vector<uint8_t> Draw(size_t indices, uint32_t count, size_t target,
                            size_t readback,const std::function<void()>& prepare={}) {
    Begin();
    if(prepare) prepare();
    VkClearValue clear{};
    clear.color.float32[3] = 1;
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = pass;
    bi.framebuffer = framebuffer;
    bi.renderArea = {{0, 0}, {64, 64}};
    bi.clearValueCount = 1;
    bi.pClearValues = &clear;
    c.f.vkCmdBeginRenderPass(command, &bi, VK_SUBPASS_CONTENTS_INLINE);
    c.f.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    f.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipeline_layout, 0, 4, sets.data(), dynamic_count, dynamic_offsets.data());
    f.vkCmdBindIndexBuffer(command, buffers[indices].handle, 0,
                           VK_INDEX_TYPE_UINT32);
    f.vkCmdDrawIndexed(command, count, 1, 0, 0, 0);
    c.f.vkCmdEndRenderPass(command);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {64, 64, 1};
    f.vkCmdCopyImageToBuffer(command, images[target].handle,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             buffers[readback].handle, 1, &copy);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    host.buffer = buffers[readback].handle;
    host.size = VK_WHOLE_SIZE;
    f.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host,
                           0, nullptr);
    Submit();
    auto &b = buffers[readback];
    void *mapped;
    Require(c.f.vkMapMemory(c.device, b.memory, 0, VK_WHOLE_SIZE, 0, &mapped),
            "Map readback");
    VkResult invalidated = VK_SUCCESS;
    if (!b.coherent) {
      VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
      range.memory = b.memory;
      range.size = VK_WHOLE_SIZE;
      invalidated = f.vkInvalidateMappedMemoryRanges(c.device, 1, &range);
    }
    if (invalidated != VK_SUCCESS) {
      c.f.vkUnmapMemory(c.device, b.memory);
      Require(invalidated, "Invalidate readback");
    }
    std::vector<uint8_t> pixels(64 * 64 * 4);
    std::memcpy(pixels.data(), mapped, pixels.size());
    c.f.vkUnmapMemory(c.device, b.memory);
    return pixels;
  }
};
void Pixel(const std::vector<uint8_t> &data, uint32_t x, uint32_t y,
           std::array<uint8_t, 4> expected) {
  for (int c = 0; c < 4; ++c)
    if (std::abs(int(data[(y * 64 + x) * 4 + c]) - expected[c]) > 1)
      throw std::runtime_error("Pixel mismatch at " + std::to_string(x) + "," +
                               std::to_string(y) + " channel " +
                               std::to_string(c) + " got " +
                               std::to_string(data[(y * 64 + x) * 4 + c]));
}
int Run(const std::filesystem::path &shaders_dir, const std::string &uuid,
        uint32_t &validation_errors,bool production=false) {
  Win32Window window;
  Context context;
  context.logger = [&](const std::string &s) {
    std::cout << s << std::endl;
    if (s.starts_with("validation ERROR:"))
      ++validation_errors;
  };
  Error e;
  const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                              VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
  if (!context.CreateInstance(extensions, true, e) || !window.Open(64, 64, e))
    throw std::runtime_error(e.operation + ": " + e.message);
  auto surface = window.CreateSurface(context, e);
  if (!surface)
    throw std::runtime_error(e.operation + ": " + e.message);
  VkPhysicalDeviceFeatures features{};
  features.shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
  features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
  if (!context.OpenDevice(surface, uuid, e, &features,production))
    throw std::runtime_error(
        e.operation + ": " + e.message +
        "; shaderStorageBufferArrayDynamicIndexing requested");
  shaders::ShaderRequirements needs;
  needs.storage_buffers = 34;
  needs.sampled_images = 32;
  needs.samplers = 32;
  needs.descriptor_sets = 4;
  needs.storage_buffer_dynamic_indexing = true;
  needs.sampled_image_dynamic_indexing = true;
  auto &limits = context.properties.limits;
  shaders::ShaderCapabilities available;
  available.storage_buffers = limits.maxPerStageDescriptorStorageBuffers;
  available.sampled_images = limits.maxPerStageDescriptorSampledImages;
  available.samplers = limits.maxPerStageDescriptorSamplers;
  available.descriptor_sets = limits.maxBoundDescriptorSets;
  available.storage_buffer_dynamic_indexing = context.enabled_features.shaderStorageBufferArrayDynamicIndexing;
  available.sampled_image_dynamic_indexing = context.enabled_features.shaderSampledImageArrayDynamicIndexing;
  auto missing = shaders::CheckRequirements(needs, available);
  if (!missing.empty())
    throw std::runtime_error(missing[0]);
  if (limits.maxDescriptorSetStorageBuffers < 35 ||
      limits.maxStorageBufferRange < 4096)
    throw std::runtime_error(
        "Descriptor-set storage buffer/range limit insufficient");
  Fixture fixture(context);
  ResourceStore resources(context);
  DescriptorStore descriptors(context);
  if(production && !descriptors.Initialize(e)) throw std::runtime_error(e.operation+": "+e.message);
  fixture.Commands();
  auto vs = fixture.NewBuffer(4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
       ps = fixture.NewBuffer(4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
       shared = fixture.NewBuffer(4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  std::array<uint32_t, 6> vertices{8u | (8u << 16),   24u | (8u << 16),
                                   8u | (24u << 16),  40u | (40u << 16),
                                   56u | (40u << 16), 40u | (56u << 16)};
  auto vertex =
      fixture.NewBuffer(sizeof(vertices), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  fixture.Write(vertex, std::as_bytes(std::span(vertices)));
  std::array<uint32_t, 7> guest_indices{0, 1, 2, 0xfffffe, 3, 4, 5};
  std::vector<uint32_t> normalized;
  std::string error;
  if (!guest::NormalizeIndices(
          {reinterpret_cast<const uint8_t *>(guest_indices.data()),
           sizeof(guest_indices)},
          0, 7, {true, 0, 0xfffffe}, guest::Primitive::kTriangleStrip,
          normalized, error))
    throw std::runtime_error(error);
  auto indices = fixture.NewBuffer(normalized.size() * 4,
                                   VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
  fixture.Write(indices, std::as_bytes(std::span(normalized)));
  auto readback =
      fixture.NewBuffer(64 * 64 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  auto staging = fixture.NewBuffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  std::array<uint8_t, 4> white{255, 255, 255, 255};
  fixture.Write(staging, std::as_bytes(std::span(white)));
  auto target = fixture.NewImage(64, 64,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
       texture = fixture.NewImage(
           1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  fixture.Begin();
  fixture.Barrier(
      fixture.images[texture].handle, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {1, 1, 1};
  fixture.f.vkCmdCopyBufferToImage(
      fixture.command, fixture.buffers[staging].handle,
      fixture.images[texture].handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
      &copy);
  fixture.Barrier(
      fixture.images[texture].handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  fixture.Submit();
  fixture.RenderTarget(target);
  if(!production) fixture.Descriptors(vs, ps, shared, vertex, texture);
  fixture.Pipeline(shaders_dir,production?std::span<const VkDescriptorSetLayout>(descriptors.Layouts()):std::span<const VkDescriptorSetLayout>{});
  guest::ConstantSnapshot state;
  state.ps[1] = 0x3f800000;
  state.ps[3] = 0x3f800000;
  guest::VertexFetchMeta meta{7, 0, 4, 0x2C2259};
  std::memcpy(state.shared.data() + 512, &meta, sizeof(meta));
  for (int variant = 0; variant < 3; ++variant) {
    state.vs[2] = variant == 2 ? 0x40000000 : 0;
    uint32_t boolean = variant == 1 ? 1 : 0, loop = 1;
    std::memcpy(state.shared.data() + 256, &boolean, 4);
    std::memcpy(state.shared.data() + 304, &loop, 4);
    fixture.Write(vs, std::as_bytes(std::span(state.vs)));
    fixture.Write(ps, std::as_bytes(std::span(state.ps)));
    fixture.Write(shared, std::as_bytes(std::span(state.shared)));
    auto pixels =
        fixture.Draw(indices, uint32_t(normalized.size()), target, readback,production?std::function<void()>([&] {
          if(!resources.BeginSubmission(fixture.command,uint64_t(variant)+1,e) || !resources.CreateDummies(e) ||
             !resources.UploadBuffer(7,std::as_bytes(std::span(vertices)),1,e)) throw std::runtime_error(e.operation+": "+e.message);
          guest::DrawPacket packet;packet.constants=state;packet.vertex_fetch[0]={7,0,4,0x2C2259};packet.attributes.push_back({7,0,0x2C2259,0,0});
          guest::VertexStream stream{};stream.stream=7;stream.size=sizeof(vertices);stream.stride=4;stream.update.plan.key=7;packet.streams.push_back(stream);
          packet.texture_fetch[7][0]=2|(3<<10)|(3<<13);packet.texture_fetch[7][1]=0x1006;packet.texture_fetch[7][5]=1<<9;
          guest::LinearTexture linear;linear.width=linear.height=1;linear.format=guest::LinearFormat::kRGBA8Unorm;linear.levels={{1,1,4,1,0}};linear.data={255,255,255,255};
          if(!resources.UploadTexture(TextureResourceId(packet.texture_fetch[7]),linear,1,e)) throw std::runtime_error(e.operation+": "+e.message);
          DescriptorDraw sets;
          ConstantBankCache cache;
          packet.constants.vs[2]=state.vs[2]^0x40000000u;
          packet.constants.ps[1]=state.ps[1]^0x3f800000u;
          for(uint32_t step=0;step<4;++step) {
            // Restore shader-consumed depth and color independently. The last
            // draw reuses both restored banks; stale offsets break the pixels.
            if(step==1) packet.constants.vs[2]=state.vs[2];
            if(step==2) packet.constants.ps[1]=state.ps[1];
            TransientSlice constants;if(!resources.MapTransient(sizeof(guest::ConstantSnapshot),constants,e)) throw std::runtime_error(e.operation+": "+e.message);
            auto reuse=cache.Find(uint64_t(variant)+1,constants.chunk.get(),packet.constants);
            DrawBindings bindings;if(!BuildBindings(packet,constants.data,bindings,e,(reuse.vs?0u:1u)|(reuse.ps?0u:2u))) throw std::runtime_error(e.message);
            if(!descriptors.Prepare(bindings,packet.texture_fetch,resources,uint64_t(variant)+1,constants,sets,e)) throw std::runtime_error(e.operation+": "+e.message);
            if(reuse.vs) sets.dynamic_offsets[0]=reuse.vs_offset;
            if(reuse.ps) sets.dynamic_offsets[1]=reuse.ps_offset;
            cache.Commit(uint64_t(variant)+1,constants.chunk.get(),uint32_t(constants.offset),packet.constants,reuse);
          }
          fixture.sets=sets.sets;
          fixture.dynamic_offsets=sets.dynamic_offsets;fixture.dynamic_count=uint32_t(sets.dynamic_offsets.size());
        }):std::function<void()>{});
    if(production) {descriptors.Retire(uint64_t(variant)+1);resources.Retire(uint64_t(variant)+1);}
    std::array<uint8_t, 4> color =
        variant == 0   ? std::array<uint8_t, 4>{0, 255, 0, 255}
        : variant == 1 ? std::array<uint8_t, 4>{255, 0, 0, 255}
                       : std::array<uint8_t, 4>{0, 0, 0, 255};
    Pixel(pixels, 10, 10, color);
    Pixel(pixels, 42, 42, color);
    Pixel(pixels, 32, 32, {0, 0, 0, 255});
    Pixel(pixels, 54, 54, {0, 0, 0, 255});
    if (variant == 2)
      for (size_t offset = 0; offset < pixels.size(); offset += 4)
        if (pixels[offset] || pixels[offset + 1] || pixels[offset + 2] ||
            pixels[offset + 3] != 255)
          throw std::runtime_error("Clipping variant not fully clear");
    std::cout << "contract variant " << variant << " pixels passed"
              << std::endl;
  }
  return context.validation_errors.load() ? 1 : 0;
}
int main(int argc, char **argv) {
  uint32_t validation_errors = 0;
  int result = 1;
  try {
    std::string uuid;
    bool production=argc==2 && std::string_view(argv[1])=="--production-bindings";
    if(production) {}
    else if (argc == 2 && std::string_view(argv[1]).starts_with("--gpu-uuid="))
      uuid = std::string(argv[1]).substr(11);
    else if (argc != 1)
      throw std::runtime_error(
          "Usage: sr_vulkan_contract_test [--gpu-uuid=32hex]");
    wchar_t path[32768];
    auto count = GetModuleFileNameW(nullptr, path, 32768);
    if (!count || count >= 32768)
      throw std::runtime_error("Executable path unavailable");
    result = Run(std::filesystem::path(path).parent_path() / "contract-shaders",
                 uuid, validation_errors,production);
  } catch (const std::exception &e) {
    std::cerr << "ERROR " << e.what() << std::endl;
  }
  std::cout << "validation_errors=" << validation_errors << " exit=" << result
            << std::endl;
  return validation_errors ? 1 : result;
}
