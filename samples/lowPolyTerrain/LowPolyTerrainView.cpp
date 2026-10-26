#include <map>
#include "LowPolyTerrainView.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace lowpoly
{

namespace
{
constexpr float kPi = 3.14159265358979323846f;
}

vkbackend::VulkanTextureView TerrainRHI::createDepthTextureView(
    const gpu::TextureId texture)
{
  gpu::TextureView view{};
  view.resourceId = texture;
  view.flags = gpu::ImageAspectFlags::Depth;
  view.baseMipLevel = 0u;
  view.levelCount = 1u;
  view.baseArrayLayer = 0u;
  view.layerCount = 1u;
  view.access = gpu::AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ |
      gpu::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE;
  view.layout = gpu::ResourceLayout::DEPTH_STENCIL_ATTACHMENT;
  return createTextureView(view);
}

void TerrainRHI::destroyDepthTextureView(vkbackend::VulkanTextureView &view)
{
  if (view.view != VK_NULL_HANDLE)
  {
    destroyTextureView(view);
    view = {};
  }
}

VkBuffer TerrainRHI::bufferHandle(const gpu::BufferId buffer)
{
  return getVulkanBuffer(buffer).buffer;
}

FramePass::FramePass(TerrainRHI &rhi) : rhi_(rhi)
{
}

FramePass::~FramePass()
{
  destroy();
}

void FramePass::initialize(const gpu::SwapChain swapChain)
{
  destroy();
  renderPass_ =
      createRenderPass(rhi_.getDevice(), rhi_.getSwapChainVkFormat(swapChain));
}

void FramePass::destroy()
{
  destroyFramebuffers();
  if (renderPass_ != VK_NULL_HANDLE)
  {
    vkDestroyRenderPass(rhi_.getDevice(), renderPass_, nullptr);
    renderPass_ = VK_NULL_HANDLE;
  }
}

void FramePass::destroyFramebuffers()
{
  for (auto &entry : framebuffers_)
  {
    if (entry.second.framebuffer != VK_NULL_HANDLE)
      vkDestroyFramebuffer(rhi_.getDevice(), entry.second.framebuffer, nullptr);
    rhi_.destroyDepthTextureView(entry.second.depthView);
    if (entry.second.depthTexture != gpu::TextureId::Invalid)
      rhi_.deleteTexture(entry.second.depthTexture);
  }
  framebuffers_.clear();
}

void FramePass::onSwapChainRecreated(const gpu::SwapChain swapChain)
{
  destroyFramebuffers();
  if (renderPass_ == VK_NULL_HANDLE)
    renderPass_ =
        createRenderPass(rhi_.getDevice(), rhi_.getSwapChainVkFormat(swapChain));
}

void FramePass::render(
    gpu::SwapChain &swapChain,
    const gpu::TextureView &swapChainView,
    ImDrawData *drawData,
    const std::function<void(VkCommandBuffer, VkExtent2D)> &beforeImGui)
{
  rhi_.presentWithOverlay(
      swapChain,
      swapChainView.resourceId,
      gpu::ResourceLayout::UNDEFINED,
      [this, drawData, beforeImGui,
          key = static_cast<uintptr_t>(swapChainView.resourceId)](
          VkCommandBuffer commandBuffer, VkImageView imageView, VkExtent2D extent)
      {
        Resources &resources = framebuffers_[key];
        if (resources.framebuffer == VK_NULL_HANDLE)
        {
          resources.depthTexture = rhi_.createTexture({
            .name = "lowPolyTerrain.depth",
            .format = gpu::Format::Format_Depth32Float,
            .memoryProperties = gpu::BufferUsage_None,
            .usage = gpu::ImageUsage_DepthStencilAttachment,
            .width = extent.width,
            .height = extent.height,
            .depth = 1u,
            .arrayLayers = 1u,
            .mipLevels = 1u,
            .initialLayout = gpu::ResourceLayout::UNDEFINED,
          });
          resources.depthView = rhi_.createDepthTextureView(resources.depthTexture);
          const std::array<VkImageView, 2> attachments{
              imageView, resources.depthView.view};
          VkFramebufferCreateInfo info{};
          info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
          info.renderPass = renderPass_;
          info.attachmentCount = static_cast<uint32_t>(attachments.size());
          info.pAttachments = attachments.data();
          info.width = extent.width;
          info.height = extent.height;
          info.layers = 1u;
          check(vkCreateFramebuffer(
              rhi_.getDevice(), &info, nullptr, &resources.framebuffer));
        }

        std::array<VkClearValue, 2> clearValues{};
        clearValues[0].color = {{0.52f, 0.66f, 0.82f, 1.0f}};
        // Reverse-Z: the far plane is zero.
        clearValues[1].depthStencil = {0.0f, 0u};
        VkRenderPassBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass = renderPass_;
        begin.framebuffer = resources.framebuffer;
        begin.renderArea.extent = extent;
        begin.clearValueCount = static_cast<uint32_t>(clearValues.size());
        begin.pClearValues = clearValues.data();
        vkCmdBeginRenderPass(commandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
        if (beforeImGui)
          beforeImGui(commandBuffer, extent);
        if (drawData != nullptr && drawData->CmdListsCount > 0)
          ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
        vkCmdEndRenderPass(commandBuffer);
      });
}

void FramePass::check(const VkResult result)
{
  if (result != VK_SUCCESS)
    throw std::runtime_error(
        "Low-poly terrain Vulkan call failed with VkResult=" +
        std::to_string(static_cast<int>(result)));
}

VkRenderPass FramePass::createRenderPass(
    const VkDevice device, const VkFormat format)
{
  VkAttachmentDescription colour{};
  colour.format = format;
  colour.samples = VK_SAMPLE_COUNT_1_BIT;
  colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colour.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  colour.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  colour.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentDescription depth{};
  depth.format = VK_FORMAT_D32_SFLOAT;
  depth.samples = VK_SAMPLE_COUNT_1_BIT;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference colourReference{};
  colourReference.attachment = 0u;
  colourReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference depthReference{};
  depthReference.attachment = 1u;
  depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1u;
  subpass.pColorAttachments = &colourReference;
  subpass.pDepthStencilAttachment = &depthReference;

  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0u;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dependency.dstStageMask = dependency.srcStageMask;
  dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

  const std::array<VkAttachmentDescription, 2> attachments{colour, depth};
  VkRenderPassCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  info.attachmentCount = static_cast<uint32_t>(attachments.size());
  info.pAttachments = attachments.data();
  info.subpassCount = 1u;
  info.pSubpasses = &subpass;
  info.dependencyCount = 1u;
  info.pDependencies = &dependency;
  VkRenderPass renderPass = VK_NULL_HANDLE;
  check(vkCreateRenderPass(device, &info, nullptr, &renderPass));
  return renderPass;
}

std::array<float, 3> TerrainCamera::forward() const
{
  const float cosPitch = std::cos(pitchRadians);
  return {std::sin(yawRadians) * cosPitch, std::sin(pitchRadians),
      std::cos(yawRadians) * cosPitch};
}

float TerrainCamera::effectiveSpeed() const
{
  if (!altitudeRelativeSpeed)
    return speedMetresPerSecond;
  // Above-ground height sets the scale, so the same input is usable at two
  // metres and at a hundred kilometres. Clamped below so landing does not
  // freeze the camera and above so a high pass does not become a teleport.
  const float altitude = std::clamp(y - groundHeightMetres, 2.0f, 60000.0f);
  return speedMetresPerSecond * std::clamp(altitude / 800.0f, 0.02f, 60.0f);
}

std::array<float, 16> TerrainCamera::viewProjection(const float aspect) const
{
  const std::array<float, 3> f = forward();
  const float upWorld[3] = {0.0f, 1.0f, 0.0f};
  // cross(up, forward), not cross(forward, up): with y up and forward down +z
  // this puts screen-right on +x, which is what the strafe keys assume. The
  // other order mirrors the world in x and leaves A and D moving the camera
  // the opposite way from the direction the picture moves.
  float s[3] = {upWorld[1] * f[2] - upWorld[2] * f[1],
      upWorld[2] * f[0] - upWorld[0] * f[2],
      upWorld[0] * f[1] - upWorld[1] * f[0]};
  const float sLength =
      std::max(1.0e-6f, std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]));
  s[0] /= sLength;
  s[1] /= sLength;
  s[2] /= sLength;
  const float u[3] = {f[1] * s[2] - f[2] * s[1], f[2] * s[0] - f[0] * s[2],
      f[0] * s[1] - f[1] * s[0]};

  // Reverse-Z infinite projection. The near plane follows altitude so a camera
  // at two metres keeps precision without the far field falling apart.
  const float near = std::clamp((y - groundHeightMetres) * 0.02f, 0.25f, 400.0f);
  const float fov = 60.0f * kPi / 180.0f;
  const float focal = 1.0f / std::tan(fov * 0.5f);

  // Column-major, as the uniform block expects. Transposing here leaves the
  // culling healthy and the frame empty.
  std::array<float, 16> m{};
  m[0] = s[0] * focal / aspect;
  m[4] = s[1] * focal / aspect;
  m[8] = s[2] * focal / aspect;
  m[12] = -(s[0] * x + s[1] * y + s[2] * z) * focal / aspect;

  m[1] = -u[0] * focal;
  m[5] = -u[1] * focal;
  m[9] = -u[2] * focal;
  m[13] = (u[0] * x + u[1] * y + u[2] * z) * focal;

  // Infinite far plane with reverse-Z: z' = near / (dot(f, p - eye)).
  m[2] = 0.0f;
  m[6] = 0.0f;
  m[10] = 0.0f;
  m[14] = near;

  m[3] = f[0];
  m[7] = f[1];
  m[11] = f[2];
  m[15] = -(f[0] * x + f[1] * y + f[2] * z);
  return m;
}

TerrainMeshPass::TerrainMeshPass(TerrainRHI &rhi, std::string shaderPath)
    : rhi_(rhi), shaderPath_(std::move(shaderPath))
{
}

TerrainMeshPass::~TerrainMeshPass()
{
  destroy();
}

void TerrainMeshPass::initialize(const VkRenderPass renderPass)
{
  uniformBuffer_ = rhi_.createBuffer({
    .name = "lowPolyTerrain.uniforms",
    .size = sizeof(Uniforms),
    .usage = static_cast<gpu::BufferUsage>(
        gpu::BufferUsage_Uniform | gpu::BufferUsage_Push),
  });
  createDescriptors();
  createPipeline(renderPass);
}

void TerrainMeshPass::destroyMesh()
{
  if (indexBuffer_ != gpu::BufferId::Invalid)
  {
    rhi_.deleteBuffer(indexBuffer_);
    indexBuffer_ = gpu::BufferId::Invalid;
  }
  if (vertexBuffer_ != gpu::BufferId::Invalid)
  {
    rhi_.deleteBuffer(vertexBuffer_);
    vertexBuffer_ = gpu::BufferId::Invalid;
  }
  vertexCount_ = 0u;
  indexCount_ = 0u;
}

void TerrainMeshPass::destroy()
{
  destroyMesh();
  if (wirePipeline_ != VK_NULL_HANDLE)
    vkDestroyPipeline(rhi_.getDevice(), wirePipeline_, nullptr);
  if (pipeline_ != VK_NULL_HANDLE)
    vkDestroyPipeline(rhi_.getDevice(), pipeline_, nullptr);
  if (pipelineLayout_ != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(rhi_.getDevice(), pipelineLayout_, nullptr);
  if (descriptorPool_ != VK_NULL_HANDLE)
    vkDestroyDescriptorPool(rhi_.getDevice(), descriptorPool_, nullptr);
  if (descriptorSetLayout_ != VK_NULL_HANDLE)
    vkDestroyDescriptorSetLayout(rhi_.getDevice(), descriptorSetLayout_, nullptr);
  if (shaderModule_ != VK_NULL_HANDLE)
    vkDestroyShaderModule(rhi_.getDevice(), shaderModule_, nullptr);
  if (uniformBuffer_ != gpu::BufferId::Invalid)
    rhi_.deleteBuffer(uniformBuffer_);
  wirePipeline_ = VK_NULL_HANDLE;
  pipeline_ = VK_NULL_HANDLE;
  pipelineLayout_ = VK_NULL_HANDLE;
  descriptorPool_ = VK_NULL_HANDLE;
  descriptorSetLayout_ = VK_NULL_HANDLE;
  shaderModule_ = VK_NULL_HANDLE;
  uniformBuffer_ = gpu::BufferId::Invalid;
}

void TerrainMeshPass::createDescriptors()
{
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0u;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  binding.descriptorCount = 1u;
  binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = 1u;
  layoutInfo.pBindings = &binding;
  FramePass::check(vkCreateDescriptorSetLayout(
      rhi_.getDevice(), &layoutInfo, nullptr, &descriptorSetLayout_));

  VkDescriptorPoolSize poolSize{};
  poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  poolSize.descriptorCount = 1u;
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = 1u;
  poolInfo.poolSizeCount = 1u;
  poolInfo.pPoolSizes = &poolSize;
  FramePass::check(vkCreateDescriptorPool(
      rhi_.getDevice(), &poolInfo, nullptr, &descriptorPool_));

  VkDescriptorSetAllocateInfo allocateInfo{};
  allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocateInfo.descriptorPool = descriptorPool_;
  allocateInfo.descriptorSetCount = 1u;
  allocateInfo.pSetLayouts = &descriptorSetLayout_;
  FramePass::check(
      vkAllocateDescriptorSets(rhi_.getDevice(), &allocateInfo, &descriptorSet_));

  VkDescriptorBufferInfo bufferInfo{};
  bufferInfo.buffer = rhi_.bufferHandle(uniformBuffer_);
  bufferInfo.range = sizeof(Uniforms);
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = descriptorSet_;
  write.dstBinding = 0u;
  write.descriptorCount = 1u;
  write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  write.pBufferInfo = &bufferInfo;
  vkUpdateDescriptorSets(rhi_.getDevice(), 1u, &write, 0u, nullptr);
}

void TerrainMeshPass::createPipeline(const VkRenderPass renderPass)
{
  std::ifstream file(shaderPath_, std::ios::binary);
  if (!file)
    throw std::runtime_error("Could not open terrain shader: " + shaderPath_);
  const std::string spirv(
      (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (spirv.empty() || (spirv.size() % sizeof(uint32_t)) != 0u)
    throw std::runtime_error("Terrain shader is not valid SPIR-V");
  VkShaderModuleCreateInfo moduleInfo{};
  moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  moduleInfo.codeSize = spirv.size();
  moduleInfo.pCode = reinterpret_cast<const uint32_t *>(spirv.data());
  FramePass::check(vkCreateShaderModule(
      rhi_.getDevice(), &moduleInfo, nullptr, &shaderModule_));

  VkPipelineLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutInfo.setLayoutCount = 1u;
  layoutInfo.pSetLayouts = &descriptorSetLayout_;
  FramePass::check(vkCreatePipelineLayout(
      rhi_.getDevice(), &layoutInfo, nullptr, &pipelineLayout_));

  std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = shaderModule_;
  stages[0].pName = "vs_main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = shaderModule_;
  stages[1].pName = "fs_main";

  VkVertexInputBindingDescription vertexBinding{};
  vertexBinding.binding = 0u;
  vertexBinding.stride = sizeof(Vertex);
  vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  std::array<VkVertexInputAttributeDescription, 3> attributes{};
  attributes[0] = {0u, 0u, VK_FORMAT_R32G32B32_SFLOAT,
      static_cast<uint32_t>(offsetof(Vertex, x))};
  attributes[1] = {1u, 0u, VK_FORMAT_R8G8B8A8_UNORM,
      static_cast<uint32_t>(offsetof(Vertex, colour))};
  attributes[2] = {2u, 0u, VK_FORMAT_R32G32B32_SFLOAT,
      static_cast<uint32_t>(offsetof(Vertex, nx))};
  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount = 1u;
  vertexInput.pVertexBindingDescriptions = &vertexBinding;
  vertexInput.vertexAttributeDescriptionCount =
      static_cast<uint32_t>(attributes.size());
  vertexInput.pVertexAttributeDescriptions = attributes.data();

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1u;
  viewportState.scissorCount = 1u;

  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  // No culling: a terrace riser seen from below is still terrain, and a mesh
  // this stylized reads worse with holes than with backfaces.
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_TRUE;
  // Reverse-Z.
  depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER;

  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
      VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
      VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = 1u;
  blend.pAttachments = &blendAttachment;

  const std::array<VkDynamicState, 2> dynamicStates{
      VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
  dynamic.pDynamicStates = dynamicStates.data();

  VkGraphicsPipelineCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.stageCount = static_cast<uint32_t>(stages.size());
  info.pStages = stages.data();
  info.pVertexInputState = &vertexInput;
  info.pInputAssemblyState = &inputAssembly;
  info.pViewportState = &viewportState;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depthStencil;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = pipelineLayout_;
  info.renderPass = renderPass;
  info.subpass = 0u;
  FramePass::check(vkCreateGraphicsPipelines(
      rhi_.getDevice(), VK_NULL_HANDLE, 1u, &info, nullptr, &pipeline_));

  VkPipelineRasterizationStateCreateInfo wireRaster = raster;
  wireRaster.polygonMode = VK_POLYGON_MODE_LINE;
  VkPipelineDepthStencilStateCreateInfo wireDepth = depthStencil;
  wireDepth.depthWriteEnable = VK_FALSE;
  wireDepth.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
  VkGraphicsPipelineCreateInfo wireInfo = info;
  wireInfo.pRasterizationState = &wireRaster;
  wireInfo.pDepthStencilState = &wireDepth;
  FramePass::check(vkCreateGraphicsPipelines(
      rhi_.getDevice(), VK_NULL_HANDLE, 1u, &wireInfo, nullptr, &wirePipeline_));
}

void TerrainRHI::writeChecked(gpu::BufferId id, uint64_t bytes, const void *data)
{
  const auto &buffer = getVulkanBuffer(id);
  if (!bytes || bytes > buffer.size || !data || !(buffer.memoryFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
    throw std::runtime_error("invalid terrain buffer write");
  void *mapped = nullptr;
  FramePass::check(vkMapMemory(getDevice(), buffer.memory, 0, VK_WHOLE_SIZE, 0, &mapped));
  std::memcpy(mapped, data, bytes);
  VkResult result = VK_SUCCESS;
  if (!(buffer.memoryFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
  {
    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = buffer.memory;
    range.size = VK_WHOLE_SIZE;
    result = vkFlushMappedMemoryRanges(getDevice(), 1, &range);
  }
  vkUnmapMemory(getDevice(), buffer.memory);
  FramePass::check(result);
}

bool TerrainMeshPass::upload(
    const procedural::geometry::TerrainMesh &mesh, std::string *error)
{
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  size_t totalVertices = 0u;
  size_t totalIndices = 0u;
  for (const procedural::geometry::MeshTile &tile : mesh.tiles)
  {
    if (tile.indices.size() != tile.materials.size()*3)
    { if (error) *error = "terrain material/index count mismatch"; return false; }
    for (auto index : tile.indices)
      if (index >= tile.vertices.size()) { if (error) *error = "terrain index out of range"; return false; }
    for (const auto &v : tile.vertices)
      if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
      { if (error) *error = "nonfinite terrain vertex"; return false; }
    totalVertices += tile.vertices.size();
    totalIndices += tile.indices.size();
  }
  if (flatShading_)
    for (const auto &tile : mesh.tiles)
      for (size_t i = 0; i < tile.indices.size(); i += 3)
      {
        std::array<std::array<float,3>,3> p;
        for (size_t k = 0; k < 3; ++k)
        {
          const auto &v = tile.vertices[tile.indices[i+k]];
          if (std::max({std::abs(v.x),std::abs(v.y),std::abs(v.z)}) > std::numeric_limits<float>::max())
          { if (error) *error = "rewrite coordinates exceed GPU float range"; return false; }
          p[k] = {float(v.x),float(v.y),float(v.z)};
        }
        const double ax=double(p[1][0])-p[0][0], ay=double(p[1][1])-p[0][1], az=double(p[1][2])-p[0][2];
        const double bx=double(p[2][0])-p[0][0], by=double(p[2][1])-p[0][1], bz=double(p[2][2])-p[0][2];
        if (ax*by-ay*bx == 0 && ay*bz-az*by == 0 && az*bx-ax*bz == 0)
        { if (error) *error = "rewrite triangle collapses at GPU precision"; return false; }
      }
  if (totalIndices == 0u)
  {
    if (error != nullptr)
      *error = "the terrain mesh has no triangles";
    return false;
  }
  const uint64_t residentBytes = vertexCount_ * sizeof(Vertex) + indexCount_ * sizeof(uint32_t);
  // Worst case: one vertex per index. Budget includes the retained old buffers.
  if (totalIndices > UINT32_MAX || totalIndices > uploadBudget_ / (sizeof(Vertex)+sizeof(uint32_t)) ||
      residentBytes > uploadBudget_ - totalIndices*(sizeof(Vertex)+sizeof(uint32_t)))
  { if (error) *error = "terrain GPU replacement budget exceeded"; return false; }
  vertices.reserve(totalVertices);
  indices.reserve(totalIndices);
  // Smooth normals, accumulated by world position rather than by vertex index.
  // Tiles are meshed independently, so a per-tile accumulation sees only half
  // the faces at a shared edge and leaves a lighting seam every tile.
  struct Accumulated { double x = 0, y = 0, z = 0; };
  std::map<std::pair<int64_t, int64_t>, Accumulated> normals;
  const auto positionKey = [](const double x, const double z)
  { return std::make_pair(int64_t(std::llround(x * 1000.0)), int64_t(std::llround(z * 1000.0))); };
  for (const procedural::geometry::MeshTile &tile : mesh.tiles)
    for (size_t face = 0; face < tile.materials.size(); ++face)
    {
      const auto &a = tile.vertices[tile.indices[face * 3]];
      const auto &b = tile.vertices[tile.indices[face * 3 + 1]];
      const auto &c = tile.vertices[tile.indices[face * 3 + 2]];
      // Cross product, unnormalised, so a larger face weighs more - which is
      // what keeps a scatter of slivers from dominating the average.
      double nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
      double ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
      double nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
      if (ny < 0) { nx = -nx; ny = -ny; nz = -nz; }
      for (const auto *vertex : {&a, &b, &c})
      {
        Accumulated &at = normals[positionKey(vertex->x, vertex->z)];
        at.x += nx; at.y += ny; at.z += nz;
      }
    }
  for (const procedural::geometry::MeshTile &tile : mesh.tiles)
  {
    // Keep semantic material vertices separate, but interpolate their cover
    // colors independently of the flat geometric normal.
    std::map<std::pair<uint32_t, uint8_t>, uint32_t> remap;
    for (size_t face = 0; face < tile.materials.size(); ++face)
      for (size_t corner = 0; corner < 3; ++corner)
      {
        const uint32_t sourceIndex = tile.indices[face * 3 + corner];
        const uint8_t material = tile.materials[face];
        const auto key = std::make_pair(flatShading_ ? uint32_t(face*3+corner) : sourceIndex, material);
        auto [it, inserted] = remap.emplace(key, static_cast<uint32_t>(vertices.size()));
        if (inserted)
        {
          const auto &source = tile.vertices[sourceIndex];
          Vertex vertex{};
          vertex.x = static_cast<float>(source.x);
          vertex.y = static_cast<float>(source.y);
          vertex.z = static_cast<float>(source.z);
          vertex.colour = materialColour_ ? materialColour_(material, source.x, source.z) : 0xff808080u;
          Accumulated at = normals[positionKey(source.x, source.z)];
          if (flatShading_)
          {
            const auto &a = tile.vertices[tile.indices[face*3]], &b = tile.vertices[tile.indices[face*3+1]], &c = tile.vertices[tile.indices[face*3+2]];
            at = {(b.y-a.y)*(c.z-a.z)-(b.z-a.z)*(c.y-a.y), (b.z-a.z)*(c.x-a.x)-(b.x-a.x)*(c.z-a.z), (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x)};
          }
          const double length = std::sqrt(at.x * at.x + at.y * at.y + at.z * at.z);
          if (length > 1e-12)
          {
            vertex.nx = static_cast<float>(at.x / length);
            vertex.ny = static_cast<float>(at.y / length);
            vertex.nz = static_cast<float>(at.z / length);
          }
          vertices.push_back(vertex);
        }
        indices.push_back(it->second);
      }
  }

  gpu::BufferId nextVertices = gpu::BufferId::Invalid, nextIndices = gpu::BufferId::Invalid;
  try
  {
    nextVertices = rhi_.createBuffer({.name = "lowPolyTerrain.vertices", .size = vertices.size()*sizeof(Vertex),
      .usage = static_cast<gpu::BufferUsage>(gpu::BufferUsage_Vertex | gpu::BufferUsage_Push)});
    nextIndices = rhi_.createBuffer({.name = "lowPolyTerrain.indices", .size = indices.size()*sizeof(uint32_t),
      .usage = static_cast<gpu::BufferUsage>(gpu::BufferUsage_Index | gpu::BufferUsage_Push)});
    rhi_.writeChecked(nextVertices, vertices.size()*sizeof(Vertex), vertices.data());
    rhi_.writeChecked(nextIndices, indices.size()*sizeof(uint32_t), indices.data());
    // Host-coherent/flushed writes become available on the subsequent queue
    // submit. Wait for old GPU readers before retiring the old allocation.
    FramePass::check(vkDeviceWaitIdle(rhi_.getDevice()));
  }
  catch (const std::exception &exception)
  {
    if (nextVertices != gpu::BufferId::Invalid) rhi_.deleteBuffer(nextVertices);
    if (nextIndices != gpu::BufferId::Invalid) rhi_.deleteBuffer(nextIndices);
    if (error) *error = exception.what();
    return false;
  }
  destroyMesh();
  vertexBuffer_ = nextVertices;
  indexBuffer_ = nextIndices;
  vertexCount_ = vertices.size();
  indexCount_ = indices.size();
  return true;
}

void TerrainMeshPass::render(
    const VkCommandBuffer commandBuffer,
    const VkExtent2D extent,
    const TerrainCamera &camera,
    const bool wireframe)
{
  if (!ready() || pipeline_ == VK_NULL_HANDLE)
    return;
  const float aspect = static_cast<float>(extent.width) /
      std::max(1.0f, static_cast<float>(extent.height));
  Uniforms uniforms{};
  uniforms.viewProjection = camera.viewProjection(aspect);
  uniforms.sunDirection = {0.42f, 0.72f, 0.55f, 0.0f};
  uniforms.skyColour = {0.46f, 0.58f, 0.76f, 0.55f};
  uniforms.groundColour = {0.24f, 0.22f, 0.18f, 0.0f};
  uniforms.options = {wireframe ? 1.0f : 0.0f, 0.0f, flatShading_ ? 1.0f : 0.0f, 0.0f};
  if (flatShading_) rhi_.writeChecked(uniformBuffer_, sizeof(Uniforms), &uniforms);
  else rhi_.bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);

  VkViewport viewport{};
  viewport.width = static_cast<float>(extent.width);
  viewport.height = static_cast<float>(extent.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  VkRect2D scissor{};
  scissor.extent = extent;
  vkCmdSetViewport(commandBuffer, 0u, 1u, &viewport);
  vkCmdSetScissor(commandBuffer, 0u, 1u, &scissor);

  const VkBuffer vertexBuffer = rhi_.bufferHandle(vertexBuffer_);
  const VkDeviceSize offset = 0u;
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
      pipelineLayout_, 0u, 1u, &descriptorSet_, 0u, nullptr);
  vkCmdBindVertexBuffers(commandBuffer, 0u, 1u, &vertexBuffer, &offset);
  vkCmdBindIndexBuffer(commandBuffer, rhi_.bufferHandle(indexBuffer_), 0u,
      VK_INDEX_TYPE_UINT32);
  vkCmdDrawIndexed(
      commandBuffer, static_cast<uint32_t>(indexCount_), 1u, 0u, 0, 0u);
  if (wireframe && wirePipeline_ != VK_NULL_HANDLE)
  {
    vkCmdBindPipeline(
        commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, wirePipeline_);
    vkCmdDrawIndexed(
        commandBuffer, static_cast<uint32_t>(indexCount_), 1u, 0u, 0, 0u);
  }
}

} // namespace lowpoly
