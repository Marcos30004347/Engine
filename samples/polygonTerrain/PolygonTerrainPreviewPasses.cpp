#include "PolygonTerrainPreviewPasses.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace polygonterrain
{

vkbackend::VulkanTextureView TerrainPreviewRHI::createSampledTextureView(const gpu::TextureId texture)
{
  gpu::TextureView view{};
  view.resourceId = texture;
  view.flags = gpu::ImageAspectFlags::Color;
  view.baseMipLevel = 0u;
  view.levelCount = 1u;
  view.baseArrayLayer = 0u;
  view.layerCount = 1u;
  view.access = gpu::AccessPattern::SHADER_READ;
  view.layout = gpu::ResourceLayout::SHADER_READ_ONLY;
  return createTextureView(view);
}

vkbackend::VulkanTextureView TerrainPreviewRHI::createDepthTextureView(const gpu::TextureId texture)
{
  gpu::TextureView view{};
  view.resourceId = texture;
  view.flags = gpu::ImageAspectFlags::Depth;
  view.baseMipLevel = 0u;
  view.levelCount = 1u;
  view.baseArrayLayer = 0u;
  view.layerCount = 1u;
  view.access = gpu::AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ | gpu::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE;
  view.layout = gpu::ResourceLayout::DEPTH_STENCIL_ATTACHMENT;
  return createTextureView(view);
}

void TerrainPreviewRHI::destroySampledTextureView(vkbackend::VulkanTextureView &view)
{
  if (view.view != VK_NULL_HANDLE)
  {
    destroyTextureView(view);
    view = {};
  }
}

VkSampler TerrainPreviewRHI::samplerHandle(const gpu::SamplerId sampler)
{
  return getVulkanSampler(sampler).sampler;
}

VkBuffer TerrainPreviewRHI::bufferHandle(const gpu::BufferId buffer)
{
  return getVulkanBuffer(buffer).buffer;
}

VkDeviceSize TerrainPreviewRHI::bufferSize(const gpu::BufferId buffer)
{
  return getVulkanBuffer(buffer).size;
}

bool TerrainPreviewRHI::readTextureRGBA32F(const gpu::TextureId textureId, const uint32_t width, const uint32_t height, std::vector<float> &rgba)
{
  if (textureId == gpu::TextureId::Invalid || width == 0u || height == 0u)
    return false;
  const vkbackend::VulkanTexture &texture = getVulkanTexture(textureId);
  if (texture.format != VK_FORMAT_R32G32B32A32_SFLOAT || texture.extent.width != width || texture.extent.height != height)
    return false;

  const uint64_t byteSize = static_cast<uint64_t>(width) * height * 4u * sizeof(float);
  const gpu::BufferId readback = createBuffer({
      .name = "polygonTerrain.erosionReadback",
      .size = byteSize,
      .usage = static_cast<gpu::BufferUsage>(gpu::BufferUsage_Pull | gpu::BufferUsage_CopyDst),
  });

  VkCommandPool commandPool = VK_NULL_HANDLE;
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool success = false;
  try
  {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = getGraphicsQueueFamilyIndex();
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    TerrainPreviewImGuiOverlayPass::checkVkResult(vkCreateCommandPool(getDevice(), &poolInfo, nullptr, &commandPool));

    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1u;
    TerrainPreviewImGuiOverlayPass::checkVkResult(vkAllocateCommandBuffers(getDevice(), &allocateInfo, &commandBuffer));
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    TerrainPreviewImGuiOverlayPass::checkVkResult(vkBeginCommandBuffer(commandBuffer, &beginInfo));

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = texture.image;
    toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
    vkCmdPipelineBarrier(
        commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &toTransfer);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
    region.imageExtent = {width, height, 1u};
    vkCmdCopyImageToBuffer(commandBuffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bufferHandle(readback), 1u, &region);

    VkImageMemoryBarrier toSampled = toTransfer;
    toSampled.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSampled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toSampled.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(
        commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &toSampled);
    TerrainPreviewImGuiOverlayPass::checkVkResult(vkEndCommandBuffer(commandBuffer));

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    TerrainPreviewImGuiOverlayPass::checkVkResult(vkCreateFence(getDevice(), &fenceInfo, nullptr, &fence));
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1u;
    submitInfo.pCommandBuffers = &commandBuffer;
    TerrainPreviewImGuiOverlayPass::checkVkResult(vkQueueSubmit(getGraphicsQueue(), 1u, &submitInfo, fence));
    TerrainPreviewImGuiOverlayPass::checkVkResult(vkWaitForFences(getDevice(), 1u, &fence, VK_TRUE, UINT64_MAX));

    rgba.resize(static_cast<size_t>(width) * height * 4u);
    bufferRead(readback, 0u, byteSize, [&rgba, byteSize](const void *data) { std::memcpy(rgba.data(), data, static_cast<size_t>(byteSize)); });
    success = true;
  }
  catch (...)
  {
    success = false;
  }
  if (fence != VK_NULL_HANDLE)
    vkDestroyFence(getDevice(), fence, nullptr);
  if (commandPool != VK_NULL_HANDLE)
    vkDestroyCommandPool(getDevice(), commandPool, nullptr);
  deleteBuffer(readback);
  return success;
}

void destroyGpuPreviewTexture(TerrainPreviewRHI &rhi, GpuPreviewTexture &texture)
{
  if (texture.descriptor != VK_NULL_HANDLE)
  {
    ImGui_ImplVulkan_RemoveTexture(texture.descriptor);
    texture.descriptor = VK_NULL_HANDLE;
  }
  rhi.destroySampledTextureView(texture.view);
  if (texture.texture != gpu::TextureId::Invalid)
  {
    rhi.deleteTexture(texture.texture);
    texture.texture = gpu::TextureId::Invalid;
  }
}

void PreviewTextureSet::reset(TerrainPreviewRHI *rhi)
{
  if (rhi != nullptr)
  {
    destroyGpuPreviewTexture(*rhi, height);
    destroyGpuPreviewTexture(*rhi, rivers);
    destroyGpuPreviewTexture(*rhi, regions);
    destroyGpuPreviewTexture(*rhi, materials);
    destroyGpuPreviewTexture(*rhi, terrainForms);
    destroyGpuPreviewTexture(*rhi, temperature);
    destroyGpuPreviewTexture(*rhi, precipitation);
    destroyGpuPreviewTexture(*rhi, wind);
    destroyGpuPreviewTexture(*rhi, forests);
    destroyGpuPreviewTexture(*rhi, settlements);
    destroyGpuPreviewTexture(*rhi, continents);
    destroyGpuPreviewTexture(*rhi, tectonics);
    destroyGpuPreviewTexture(*rhi, mountains);
    destroyGpuPreviewTexture(*rhi, sphere);
  }
  else
  {
    height = {};
    rivers = {};
    regions = {};
    materials = {};
    terrainForms = {};
    temperature = {};
    precipitation = {};
    wind = {};
    forests = {};
    settlements = {};
    continents = {};
    tectonics = {};
    mountains = {};
    sphere = {};
  }
}

TerrainPreviewTextureUploadPass::TerrainPreviewTextureUploadPass(TerrainPreviewRHI &rhi, const gpu::SamplerId sampler) : rhi_(rhi), sampler_(sampler)
{
}

GpuPreviewTexture TerrainPreviewTextureUploadPass::uploadRGBA(const std::string &name, const uint32_t width, const uint32_t height, const std::vector<uint8_t> &rgba)
{
  GpuPreviewTexture texture{};
  if (width == 0u || height == 0u || rgba.size() != static_cast<size_t>(width) * height * 4u)
    return texture;

  texture.texture = rhi_.createTexture({
    .name = name,
    .format = gpu::Format::Format_RGBA8Unorm,
    .memoryProperties = gpu::BufferUsage_None,
    .usage = static_cast<gpu::ImageUsage>(gpu::ImageUsage_Sampled | gpu::ImageUsage_TransferDst),
    .width = width,
    .height = height,
    .depth = 1u,
    .arrayLayers = 1u,
    .mipLevels = 1u,
    .initialLayout = gpu::ResourceLayout::UNDEFINED,
  });
  rhi_.textureWrite(texture.texture, rgba.size(), const_cast<uint8_t *>(rgba.data()), gpu::ResourceLayout::UNDEFINED);
  texture.view = rhi_.createSampledTextureView(texture.texture);
  texture.descriptor = ImGui_ImplVulkan_AddTexture(rhi_.samplerHandle(sampler_), texture.view.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  return texture;
}

TerrainPreviewRasterPass::TerrainPreviewRasterPass(TerrainPreviewRHI &rhi, const gpu::SamplerId sampler, std::string shaderPath)
    : rhi_(rhi), sampler_(sampler), shaderPath_(std::move(shaderPath))
{
}

TerrainPreviewRasterPass::~TerrainPreviewRasterPass()
{
  destroy();
}

gpu::TextureView TerrainPreviewRasterPass::sampledView(const gpu::TextureId texture)
{
  gpu::TextureView view{};
  view.resourceId = texture;
  view.flags = gpu::ImageAspectFlags::Color;
  view.access = gpu::AccessPattern::SHADER_READ;
  view.layout = gpu::ResourceLayout::SHADER_READ_ONLY;
  return view;
}

gpu::TextureView TerrainPreviewRasterPass::storageView(const gpu::TextureId texture)
{
  gpu::TextureView view{};
  view.resourceId = texture;
  view.flags = gpu::ImageAspectFlags::Color;
  view.access = gpu::AccessPattern::SHADER_WRITE;
  view.layout = gpu::ResourceLayout::GENERAL;
  return view;
}

gpu::TextureId TerrainPreviewRasterPass::createInput(
    const std::string &name, const uint32_t width, const uint32_t height, const std::vector<uint8_t> &rgba)
{
  const gpu::TextureId texture = rhi_.createTexture({
      .name = name,
      .format = gpu::Format::Format_RGBA8Unorm,
      .memoryProperties = gpu::BufferUsage_None,
      .usage = static_cast<gpu::ImageUsage>(gpu::ImageUsage_Sampled | gpu::ImageUsage_TransferDst),
      .width = width,
      .height = height,
      .depth = 1u,
      .arrayLayers = 1u,
      .mipLevels = 1u,
      .initialLayout = gpu::ResourceLayout::UNDEFINED,
  });
  rhi_.textureWrite(texture, rgba.size(), const_cast<uint8_t *>(rgba.data()), gpu::ResourceLayout::UNDEFINED);
  return texture;
}

GpuPreviewTexture TerrainPreviewRasterPass::createOutput(const std::string &name, const uint32_t width, const uint32_t height)
{
  GpuPreviewTexture texture{};
  texture.texture = rhi_.createTexture({
      .name = name,
      .format = gpu::Format::Format_RGBA8Unorm,
      .memoryProperties = gpu::BufferUsage_None,
      .usage = static_cast<gpu::ImageUsage>(gpu::ImageUsage_Sampled | gpu::ImageUsage_Storage),
      .width = width,
      .height = height,
      .depth = 1u,
      .arrayLayers = 1u,
      .mipLevels = 1u,
      .initialLayout = gpu::ResourceLayout::UNDEFINED,
  });
  return texture;
}

void TerrainPreviewRasterPass::initializePipeline()
{
  if (pipelineId_ != gpu::ComputePipelineId::Invalid)
    return;
  const gpu::BindingsLayoutInfo layoutInfo{
      .name = "polygonTerrain.previewRaster.layout",
      .groups = {gpu::BindingGroupLayout{
          .buffers = {{.name = "uniforms", .binding = 0u, .type = gpu::BufferBindingType_UniformBuffer, .visibility = gpu::BindingVisibility_Compute, .isDynamic = false}},
          .textures = {
              {.name = "inputA", .binding = 1u, .visibility = gpu::BindingVisibility_Compute},
              {.name = "inputB", .binding = 2u, .visibility = gpu::BindingVisibility_Compute},
              {.name = "inputC", .binding = 3u, .visibility = gpu::BindingVisibility_Compute},
              {.name = "inputD", .binding = 4u, .visibility = gpu::BindingVisibility_Compute},
          },
          .storageTextures = {
              {.name = "output0", .binding = 5u, .visibility = gpu::BindingVisibility_Compute},
              {.name = "output1", .binding = 6u, .visibility = gpu::BindingVisibility_Compute},
              {.name = "output2", .binding = 7u, .visibility = gpu::BindingVisibility_Compute},
              {.name = "output3", .binding = 8u, .visibility = gpu::BindingVisibility_Compute},
              {.name = "output4", .binding = 9u, .visibility = gpu::BindingVisibility_Compute},
              {.name = "output5", .binding = 10u, .visibility = gpu::BindingVisibility_Compute},
              {.name = "output6", .binding = 11u, .visibility = gpu::BindingVisibility_Compute},
          },
      }},
  };
  bindingsLayoutId_ = rhi_.createBindingsLayout(layoutInfo);
  const gpu::BindingsLayout layout{.name = "polygonTerrain.previewRaster.layout", .id = bindingsLayoutId_};
  std::ifstream shaderFile(shaderPath_, std::ios::binary);
  if (!shaderFile)
    throw std::runtime_error("could not open preview raster compute shader: " + shaderPath_);
  const std::string spirv((std::istreambuf_iterator<char>(shaderFile)), std::istreambuf_iterator<char>());
  shaderId_ = rhi_.createShader({.name = "polygonTerrain.previewRaster.shader", .src = spirv, .type = gpu::ShaderType::SpirV, .layout = layout});
  pipelineId_ = rhi_.createComputePipeline({
      .name = "polygonTerrain.previewRaster.pipeline",
      .shader = gpu::Shader{.name = "polygonTerrain.previewRaster.shader", .id = shaderId_},
      .entry = "raster_main",
      .layout = layout,
  });
}

bool TerrainPreviewRasterPass::rasterize(
    const procedural::PolygonTerrainMaps &maps,
    const std::vector<uint8_t> &mountains,
    PreviewTextureSet &textures,
    std::string &error)
{
  error.clear();
  const size_t pixelCount = static_cast<size_t>(maps.width) * maps.height;
  const auto valid = [pixelCount](const auto &map) { return map.size() == pixelCount; };
  if (maps.width == 0u || maps.height == 0u || !valid(maps.heightmap) || !valid(maps.rivers) || !valid(maps.regions) || !valid(maps.materials) ||
      !valid(maps.terrainForms) || !valid(maps.temperature) || !valid(maps.precipitation) || !valid(maps.wind) || !valid(maps.forests) ||
      !valid(maps.settlements) || !valid(maps.continents) || !valid(maps.tectonics))
  {
    error = "GPU preview raster requires complete source maps";
    return false;
  }

  std::array<std::vector<uint8_t>, 4> packed;
  for (std::vector<uint8_t> &input : packed)
    input.resize(pixelCount * 4u);
  const bool hasMountains = mountains.size() == pixelCount;
  for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    packed[0][pixel * 4u] = static_cast<uint8_t>(maps.heightmap[pixel] >> 8u);
    packed[0][pixel * 4u + 1u] = static_cast<uint8_t>(maps.heightmap[pixel] & 0xffu);
    packed[0][pixel * 4u + 2u] = maps.rivers[pixel];
    packed[0][pixel * 4u + 3u] = maps.regions[pixel];
    packed[1][pixel * 4u] = maps.materials[pixel];
    packed[1][pixel * 4u + 1u] = maps.terrainForms[pixel];
    packed[1][pixel * 4u + 2u] = maps.temperature[pixel];
    packed[1][pixel * 4u + 3u] = maps.precipitation[pixel];
    packed[2][pixel * 4u] = maps.wind[pixel];
    packed[2][pixel * 4u + 1u] = maps.forests[pixel];
    packed[2][pixel * 4u + 2u] = maps.settlements[pixel];
    packed[2][pixel * 4u + 3u] = maps.continents[pixel];
    packed[3][pixel * 4u] = maps.tectonics[pixel];
    packed[3][pixel * 4u + 1u] = hasMountains ? mountains[pixel] : 0u;
  }

  std::array<gpu::TextureId, 4> inputs{};
  inputs.fill(gpu::TextureId::Invalid);
  std::array<gpu::BufferId, 2> uniformBuffers{};
  uniformBuffers.fill(gpu::BufferId::Invalid);
  std::array<gpu::BindingGroupsId, 2> groups{};
  groups.fill(gpu::BindingGroupsId::Invalid);
  gpu::TextureId dummy = gpu::TextureId::Invalid;
  try
  {
    initializePipeline();
    for (uint32_t input = 0u; input < inputs.size(); ++input)
      inputs[input] = createInput("polygonTerrain.previewRaster.input" + std::to_string(input), maps.width, maps.height, packed[input]);

    textures.height = createOutput("polygonTerrain.height", maps.width, maps.height);
    textures.rivers = createOutput("polygonTerrain.rivers", maps.width, maps.height);
    textures.regions = createOutput("polygonTerrain.regions", maps.width, maps.height);
    textures.materials = createOutput("polygonTerrain.materials", maps.width, maps.height);
    textures.terrainForms = createOutput("polygonTerrain.terrainForms", maps.width, maps.height);
    textures.temperature = createOutput("polygonTerrain.temperature", maps.width, maps.height);
    textures.precipitation = createOutput("polygonTerrain.precipitation", maps.width, maps.height);
    textures.wind = createOutput("polygonTerrain.wind", maps.width, maps.height);
    textures.forests = createOutput("polygonTerrain.forests", maps.width, maps.height);
    textures.settlements = createOutput("polygonTerrain.settlements", maps.width, maps.height);
    textures.continents = createOutput("polygonTerrain.continents", maps.width, maps.height);
    textures.tectonics = createOutput("polygonTerrain.tectonics", maps.width, maps.height);
    textures.mountains = createOutput("polygonTerrain.mountains", maps.width, maps.height);
    GpuPreviewTexture dummyTexture = createOutput("polygonTerrain.previewRaster.dummy", maps.width, maps.height);
    dummy = dummyTexture.texture;

    const Uniforms uniforms[2]{
        {.dimensions = {maps.width, maps.height, 0u, hasMountains ? 1u : 0u}},
        {.dimensions = {maps.width, maps.height, 1u, hasMountains ? 1u : 0u}},
    };
    const gpu::BindingsLayout layout{.name = "polygonTerrain.previewRaster.layout", .id = bindingsLayoutId_};
    const std::array<std::array<gpu::TextureId, 7>, 2> outputIds{{
        {textures.height.texture, textures.rivers.texture, textures.regions.texture, textures.materials.texture, textures.terrainForms.texture, textures.temperature.texture, textures.precipitation.texture},
        {textures.wind.texture, textures.forests.texture, textures.settlements.texture, textures.continents.texture, textures.tectonics.texture, textures.mountains.texture, dummy},
    }};
    for (uint32_t pass = 0u; pass < 2u; ++pass)
    {
      uniformBuffers[pass] = rhi_.createBuffer({
          .name = "polygonTerrain.previewRaster.uniform" + std::to_string(pass),
          .size = sizeof(Uniforms),
          .usage = static_cast<gpu::BufferUsage>(gpu::BufferUsage_Uniform | gpu::BufferUsage_Push),
      });
      rhi_.bufferWrite(uniformBuffers[pass], 0u, sizeof(Uniforms), const_cast<Uniforms *>(&uniforms[pass]));
      groups[pass] = rhi_.createBindingGroups({
          .name = "polygonTerrain.previewRaster.group" + std::to_string(pass),
          .layout = layout,
          .groups = {gpu::GroupInfo{
              .name = "group0",
              .buffers = {{.bufferView = {.resourceId = uniformBuffers[pass], .offset = 0u, .size = sizeof(Uniforms), .access = gpu::AccessPattern::SHADER_READ}, .binding = 0u}},
              .textures = {
                  {.textureView = sampledView(inputs[0]), .binding = 1u},
                  {.textureView = sampledView(inputs[1]), .binding = 2u},
                  {.textureView = sampledView(inputs[2]), .binding = 3u},
                  {.textureView = sampledView(inputs[3]), .binding = 4u},
              },
              .storageTextures = {
                  {.textureView = storageView(outputIds[pass][0]), .binding = 5u},
                  {.textureView = storageView(outputIds[pass][1]), .binding = 6u},
                  {.textureView = storageView(outputIds[pass][2]), .binding = 7u},
                  {.textureView = storageView(outputIds[pass][3]), .binding = 8u},
                  {.textureView = storageView(outputIds[pass][4]), .binding = 9u},
                  {.textureView = storageView(outputIds[pass][5]), .binding = 10u},
                  {.textureView = storageView(outputIds[pass][6]), .binding = 11u},
              },
          }},
      });
    }

    std::vector<gpu::CommandBuffer> commandBuffers = rhi_.allocateCommandBuffers(gpu::Queue::Graphics, 1u);
    if (commandBuffers.empty())
      throw std::runtime_error("could not allocate preview raster command buffer");
    const gpu::CommandBuffer commandBuffer = commandBuffers.front();
    rhi_.beginCommandBuffer(commandBuffer, true);
    for (const auto &passOutputs : outputIds)
      for (const gpu::TextureId output : passOutputs)
        rhi_.cmdImageBarrier(
            commandBuffer, output, gpu::PipelineStage::TOP_OF_PIPE, gpu::PipelineStage::COMPUTE_SHADER,
            gpu::AccessPattern::NONE, gpu::AccessPattern::SHADER_WRITE, gpu::ResourceLayout::UNDEFINED, gpu::ResourceLayout::GENERAL,
            gpu::ImageAspectFlags::Color, 0u, 1u, 0u, 1u, gpu::Queue::None, gpu::Queue::None);
    const gpu::ComputePipeline pipeline{.name = "polygonTerrain.previewRaster.pipeline", .id = pipelineId_};
    for (uint32_t pass = 0u; pass < 2u; ++pass)
    {
      rhi_.cmdBindComputePipeline(commandBuffer, pipeline);
      rhi_.cmdBindBindingGroups(commandBuffer, groups[pass], nullptr, 0u);
      rhi_.cmdDispatch(commandBuffer, (maps.width + 7u) / 8u, (maps.height + 7u) / 8u, 1u);
    }
    for (const auto &passOutputs : outputIds)
      for (const gpu::TextureId output : passOutputs)
        rhi_.cmdImageBarrier(
            commandBuffer, output, gpu::PipelineStage::COMPUTE_SHADER, gpu::PipelineStage::FRAGMENT_SHADER,
            gpu::AccessPattern::SHADER_WRITE, gpu::AccessPattern::SHADER_READ, gpu::ResourceLayout::GENERAL, gpu::ResourceLayout::SHADER_READ_ONLY,
            gpu::ImageAspectFlags::Color, 0u, 1u, 0u, 1u, gpu::Queue::None, gpu::Queue::None);
    rhi_.endCommandBuffer(commandBuffer);
    gpu::GPUFuture future = rhi_.submit(gpu::Queue::Graphics, commandBuffers.data(), static_cast<uint32_t>(commandBuffers.size()), nullptr, 0u, true);
    rhi_.blockUntil(future);

    const std::array<GpuPreviewTexture *, 13> results{
        &textures.height, &textures.rivers, &textures.regions, &textures.materials, &textures.terrainForms, &textures.temperature, &textures.precipitation,
        &textures.wind, &textures.forests, &textures.settlements, &textures.continents, &textures.tectonics, &textures.mountains};
    for (GpuPreviewTexture *texture : results)
    {
      texture->view = rhi_.createSampledTextureView(texture->texture);
      texture->descriptor = ImGui_ImplVulkan_AddTexture(rhi_.samplerHandle(sampler_), texture->view.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
  }
  catch (const std::exception &exception)
  {
    error = std::string("GPU preview raster failed: ") + exception.what();
  }

  for (const gpu::BindingGroupsId group : groups)
    if (group != gpu::BindingGroupsId::Invalid)
      rhi_.deleteBindingGroups(group);
  for (const gpu::BufferId buffer : uniformBuffers)
    if (buffer != gpu::BufferId::Invalid)
      rhi_.deleteBuffer(buffer);
  for (const gpu::TextureId input : inputs)
    if (input != gpu::TextureId::Invalid)
      rhi_.deleteTexture(input);
  if (dummy != gpu::TextureId::Invalid)
    rhi_.deleteTexture(dummy);
  if (!error.empty())
  {
    textures.reset(&rhi_);
    return false;
  }
  return true;
}

void TerrainPreviewRasterPass::destroy()
{
  if (pipelineId_ != gpu::ComputePipelineId::Invalid)
    rhi_.deleteComputePipeline(pipelineId_);
  pipelineId_ = gpu::ComputePipelineId::Invalid;
  if (shaderId_ != gpu::ShaderId::Invalid)
    rhi_.deleteShader(shaderId_);
  shaderId_ = gpu::ShaderId::Invalid;
  if (bindingsLayoutId_ != gpu::BindingsLayoutId::Invalid)
    rhi_.deleteBindingsLayout(bindingsLayoutId_);
  bindingsLayoutId_ = gpu::BindingsLayoutId::Invalid;
}

TerrainPreviewHydraulicErosionPass::TerrainPreviewHydraulicErosionPass(TerrainPreviewRHI &rhi, std::string shaderPath)
    : rhi_(rhi), shaderPath_(std::move(shaderPath))
{
}

TerrainPreviewHydraulicErosionPass::~TerrainPreviewHydraulicErosionPass()
{
  destroy();
}

gpu::TextureView TerrainPreviewHydraulicErosionPass::sampledView(const gpu::TextureId texture)
{
  gpu::TextureView view{};
  view.resourceId = texture;
  view.flags = gpu::ImageAspectFlags::Color;
  view.access = gpu::AccessPattern::SHADER_READ;
  view.layout = gpu::ResourceLayout::SHADER_READ_ONLY;
  return view;
}

gpu::TextureView TerrainPreviewHydraulicErosionPass::storageView(const gpu::TextureId texture)
{
  gpu::TextureView view{};
  view.resourceId = texture;
  view.flags = gpu::ImageAspectFlags::Color;
  view.access = gpu::AccessPattern::SHADER_WRITE;
  view.layout = gpu::ResourceLayout::GENERAL;
  return view;
}

bool TerrainPreviewHydraulicErosionPass::start(
    const uint32_t width,
    const uint32_t height,
    const std::vector<uint16_t> &heightmap,
    const std::vector<uint8_t> &precipitation,
    const std::vector<uint8_t> &tectonics,
    const TerrainPreviewHydraulicErosionSettings &settings,
    std::string &error)
{
  error.clear();
  if (running_)
  {
    error = "GPU erosion is already running";
    return false;
  }
  const size_t pixelCount = static_cast<size_t>(width) * height;
  constexpr size_t maximumGlobalAtlasPixels = 4096u * 2048u;
  if (width < 4u || height < 2u || heightmap.size() != pixelCount || precipitation.size() != pixelCount || tectonics.size() != pixelCount)
  {
    error = "GPU erosion requires matching height, precipitation, and tectonic atlases";
    return false;
  }
  if (pixelCount > maximumGlobalAtlasPixels)
  {
    error = "The current global GPU erosion prototype is limited to a 4096x2048 atlas; larger worlds require the planned halo-tiled/cube-face path";
    return false;
  }
  if (settings.iterations == 0u || settings.iterationsPerBatch == 0u)
  {
    error = "GPU erosion iteration counts must be non-zero";
    return false;
  }

  try
  {
    destroy();
    width_ = width;
    height_ = height;
    settings_ = settings;
    settings_.iterations = std::clamp(settings_.iterations, 1u, 4096u);
    settings_.iterationsPerBatch = std::clamp(settings_.iterationsPerBatch, 1u, 64u);
    settings_.timeStep = std::clamp(settings_.timeStep, 0.005f, 0.5f);
    settings_.rainfall = std::clamp(settings_.rainfall, 0.0f, 0.02f);
    settings_.evaporation = std::clamp(settings_.evaporation, 0.0f, 0.5f);
    settings_.gravity = std::clamp(settings_.gravity, 0.01f, 20.0f);
    settings_.sedimentCapacity = std::clamp(settings_.sedimentCapacity, 0.0f, 100.0f);
    settings_.depositionRate = std::clamp(settings_.depositionRate, 0.0f, 1.0f);
    settings_.erosionRate = std::clamp(settings_.erosionRate, 0.0f, 1.0f);
    settings_.minimumSlope = std::clamp(settings_.minimumSlope, 0.0f, 0.1f);
    createResources(heightmap, precipitation, tectonics);
    createBindings();
    completedIterations_ = 0u;
    submittedIterations_ = 0u;
    currentFlux_ = 0u;
    cancelRequested_ = false;
    running_ = true;
    submitBatch();
    return true;
  }
  catch (const std::exception &exception)
  {
    error = std::string("Could not start GPU erosion: ") + exception.what();
    destroy();
    return false;
  }
}

void TerrainPreviewHydraulicErosionPass::createResources(
    const std::vector<uint16_t> &heightmap,
    const std::vector<uint8_t> &precipitation,
    const std::vector<uint8_t> &tectonics)
{
  const size_t pixelCount = static_cast<size_t>(width_) * height_;
  std::vector<float> initialState(pixelCount * 4u, 0.0f);
  std::vector<float> climate(pixelCount * 4u, 0.0f);
  std::vector<float> zero(pixelCount * 4u, 0.0f);
  for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    const float elevation = static_cast<float>(heightmap[pixel]) / 65535.0f;
    const float rain = static_cast<float>(precipitation[pixel]) / 255.0f;
    const float tectonic = static_cast<float>(tectonics[pixel]) / 255.0f;
    const bool land = heightmap[pixel] != 0u;
    const uint32_t y = static_cast<uint32_t>(pixel / width_);
    const float cellArea = std::max(0.02f, std::sin((static_cast<float>(y) + 0.5f) / static_cast<float>(height_) * 3.14159265358979323846f));
    initialState[pixel * 4u] = elevation;
    climate[pixel * 4u] = (0.10f + rain * rain * 0.90f) * cellArea;
    climate[pixel * 4u + 1u] = land ? 1.0f : 0.0f;
    climate[pixel * 4u + 2u] = elevation;
    climate[pixel * 4u + 3u] = tectonic;
  }

  const gpu::ImageUsage usage = static_cast<gpu::ImageUsage>(
      gpu::ImageUsage_Sampled | gpu::ImageUsage_Storage | gpu::ImageUsage_TransferSrc | gpu::ImageUsage_TransferDst);
  const auto createTexture = [this, usage](const std::string &name)
  {
    return rhi_.createTexture({
        .name = name,
        .format = gpu::Format::Format_RGBA32Float,
        .memoryProperties = gpu::BufferUsage_None,
        .usage = usage,
        .width = width_,
        .height = height_,
        .depth = 1u,
        .arrayLayers = 1u,
        .mipLevels = 1u,
        .initialLayout = gpu::ResourceLayout::UNDEFINED,
    });
  };
  stateTextures_[0] = createTexture("polygonTerrain.erosion.stateA");
  stateTextures_[1] = createTexture("polygonTerrain.erosion.stateB");
  fluxTextures_[0] = createTexture("polygonTerrain.erosion.fluxA");
  fluxTextures_[1] = createTexture("polygonTerrain.erosion.fluxB");
  climateTexture_ = createTexture("polygonTerrain.erosion.climate");
  const uint64_t bytes = static_cast<uint64_t>(pixelCount) * 4u * sizeof(float);
  rhi_.textureWrite(stateTextures_[0], bytes, initialState.data());
  rhi_.textureWrite(stateTextures_[1], bytes, zero.data());
  rhi_.textureWrite(fluxTextures_[0], bytes, zero.data());
  rhi_.textureWrite(fluxTextures_[1], bytes, zero.data());
  rhi_.textureWrite(climateTexture_, bytes, climate.data());

  const Uniforms uniforms{
      .dimensions = {width_, height_, 0u, settings_.iterations},
      .water = {settings_.timeStep, settings_.rainfall, settings_.evaporation, settings_.gravity},
      .sediment = {settings_.sedimentCapacity, settings_.depositionRate, settings_.erosionRate, settings_.minimumSlope},
      .terrain = {0.0f, 0.0f, 0.0f, 0.0f},
  };
  uniformBuffer_ = rhi_.createBuffer({
      .name = "polygonTerrain.erosion.uniforms",
      .size = sizeof(Uniforms),
      .usage = static_cast<gpu::BufferUsage>(gpu::BufferUsage_Uniform | gpu::BufferUsage_Push),
  });
  rhi_.bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), const_cast<Uniforms *>(&uniforms));
}

void TerrainPreviewHydraulicErosionPass::createBindings()
{
  const gpu::BindingsLayoutInfo layoutInfo{
      .name = "polygonTerrain.erosion.layout",
      .groups = {gpu::BindingGroupLayout{
          .buffers = {{.name = "uniforms", .binding = 0u, .type = gpu::BufferBindingType_UniformBuffer, .visibility = gpu::BindingVisibility_Compute, .isDynamic = false}},
          .textures = {
              {.name = "inputState", .binding = 1u, .visibility = gpu::BindingVisibility_Compute},
              {.name = "inputFlux", .binding = 2u, .visibility = gpu::BindingVisibility_Compute},
              {.name = "climate", .binding = 3u, .visibility = gpu::BindingVisibility_Compute},
          },
          .storageTextures = {{.name = "output", .binding = 4u, .visibility = gpu::BindingVisibility_Compute}},
      }},
  };
  bindingsLayoutId_ = rhi_.createBindingsLayout(layoutInfo);
  const gpu::BindingsLayout layout{.name = "polygonTerrain.erosion.layout", .id = bindingsLayoutId_};

  const auto createGroup = [this, &layout](const std::string &name, const gpu::TextureId state, const gpu::TextureId flux, const gpu::TextureId output)
  {
    return rhi_.createBindingGroups({
        .name = name,
        .layout = layout,
        .groups = {gpu::GroupInfo{
            .name = "group0",
            .buffers = {{.bufferView = {.resourceId = uniformBuffer_, .offset = 0u, .size = sizeof(Uniforms), .access = gpu::AccessPattern::SHADER_READ}, .binding = 0u}},
            .textures = {
                {.textureView = sampledView(state), .binding = 1u},
                {.textureView = sampledView(flux), .binding = 2u},
                {.textureView = sampledView(climateTexture_), .binding = 3u},
            },
            .storageTextures = {{.textureView = storageView(output), .binding = 4u}},
        }},
    });
  };
  for (uint32_t flux = 0u; flux < 2u; ++flux)
  {
    const uint32_t other = 1u - flux;
    bindingGroupIds_[flux] = createGroup("polygonTerrain.erosion.flux" + std::to_string(flux), stateTextures_[0], fluxTextures_[flux], fluxTextures_[other]);
    bindingGroupIds_[2u + flux] = createGroup("polygonTerrain.erosion.hydraulic" + std::to_string(flux), stateTextures_[0], fluxTextures_[other], stateTextures_[1]);
    bindingGroupIds_[4u + flux] = createGroup("polygonTerrain.erosion.transport" + std::to_string(flux), stateTextures_[1], fluxTextures_[other], stateTextures_[0]);
  }

  std::ifstream shaderFile(shaderPath_, std::ios::binary);
  if (!shaderFile)
    throw std::runtime_error("could not open erosion compute shader: " + shaderPath_);
  const std::string spirv((std::istreambuf_iterator<char>(shaderFile)), std::istreambuf_iterator<char>());
  shaderId_ = rhi_.createShader({.name = "polygonTerrain.erosion.shader", .src = spirv, .type = gpu::ShaderType::SpirV, .layout = layout});
  const gpu::Shader shader{.name = "polygonTerrain.erosion.shader", .id = shaderId_};
  const std::array<const char *, 3> entries{"flux_main", "hydraulic_main", "transport_main"};
  for (uint32_t pass = 0u; pass < entries.size(); ++pass)
    pipelineIds_[pass] = rhi_.createComputePipeline({.name = "polygonTerrain.erosion.pipeline" + std::to_string(pass), .shader = shader, .entry = entries[pass], .layout = layout});
}

void TerrainPreviewHydraulicErosionPass::submitBatch()
{
  const uint32_t remaining = settings_.iterations - completedIterations_;
  submittedIterations_ = std::min(settings_.iterationsPerBatch, remaining);
  std::vector<gpu::CommandBuffer> commandBuffers = rhi_.allocateCommandBuffers(gpu::Queue::Graphics, 1u);
  if (commandBuffers.empty())
    throw std::runtime_error("could not allocate erosion command buffer");
  const gpu::CommandBuffer commandBuffer = commandBuffers.front();
  rhi_.beginCommandBuffer(commandBuffer, true);
  const uint32_t groupsX = (width_ + 7u) / 8u;
  const uint32_t groupsY = (height_ + 7u) / 8u;
  const auto transition = [this, commandBuffer](
                              const gpu::TextureId texture,
                              const gpu::ResourceLayout oldLayout,
                              const gpu::ResourceLayout newLayout,
                              const gpu::AccessPattern sourceAccess,
                              const gpu::AccessPattern destinationAccess)
  {
    rhi_.cmdImageBarrier(
        commandBuffer, texture, gpu::PipelineStage::COMPUTE_SHADER, gpu::PipelineStage::COMPUTE_SHADER,
        sourceAccess, destinationAccess, oldLayout, newLayout, gpu::ImageAspectFlags::Color, 0u, 1u, 0u, 1u, gpu::Queue::None, gpu::Queue::None);
  };
  const std::array<gpu::ComputePipeline, 3> pipelines{
      gpu::ComputePipeline{.name = "flux", .id = pipelineIds_[0]},
      gpu::ComputePipeline{.name = "hydraulic", .id = pipelineIds_[1]},
      gpu::ComputePipeline{.name = "transport", .id = pipelineIds_[2]},
  };
  for (uint32_t iteration = 0u; iteration < submittedIterations_; ++iteration)
  {
    const uint32_t flux = currentFlux_;
    const uint32_t other = 1u - flux;
    transition(fluxTextures_[other], gpu::ResourceLayout::SHADER_READ_ONLY, gpu::ResourceLayout::GENERAL, gpu::AccessPattern::SHADER_READ, gpu::AccessPattern::SHADER_WRITE);
    rhi_.cmdBindComputePipeline(commandBuffer, pipelines[0]);
    rhi_.cmdBindBindingGroups(commandBuffer, bindingGroupIds_[flux], nullptr, 0u);
    rhi_.cmdDispatch(commandBuffer, groupsX, groupsY, 1u);
    transition(fluxTextures_[other], gpu::ResourceLayout::GENERAL, gpu::ResourceLayout::SHADER_READ_ONLY, gpu::AccessPattern::SHADER_WRITE, gpu::AccessPattern::SHADER_READ);

    transition(stateTextures_[1], gpu::ResourceLayout::SHADER_READ_ONLY, gpu::ResourceLayout::GENERAL, gpu::AccessPattern::SHADER_READ, gpu::AccessPattern::SHADER_WRITE);
    rhi_.cmdBindComputePipeline(commandBuffer, pipelines[1]);
    rhi_.cmdBindBindingGroups(commandBuffer, bindingGroupIds_[2u + flux], nullptr, 0u);
    rhi_.cmdDispatch(commandBuffer, groupsX, groupsY, 1u);
    transition(stateTextures_[1], gpu::ResourceLayout::GENERAL, gpu::ResourceLayout::SHADER_READ_ONLY, gpu::AccessPattern::SHADER_WRITE, gpu::AccessPattern::SHADER_READ);

    transition(stateTextures_[0], gpu::ResourceLayout::SHADER_READ_ONLY, gpu::ResourceLayout::GENERAL, gpu::AccessPattern::SHADER_READ, gpu::AccessPattern::SHADER_WRITE);
    rhi_.cmdBindComputePipeline(commandBuffer, pipelines[2]);
    rhi_.cmdBindBindingGroups(commandBuffer, bindingGroupIds_[4u + flux], nullptr, 0u);
    rhi_.cmdDispatch(commandBuffer, groupsX, groupsY, 1u);
    transition(stateTextures_[0], gpu::ResourceLayout::GENERAL, gpu::ResourceLayout::SHADER_READ_ONLY, gpu::AccessPattern::SHADER_WRITE, gpu::AccessPattern::SHADER_READ);
    currentFlux_ = other;
  }
  rhi_.endCommandBuffer(commandBuffer);
  future_ = rhi_.submit(gpu::Queue::Graphics, commandBuffers.data(), static_cast<uint32_t>(commandBuffers.size()), nullptr, 0u, true);
}

bool TerrainPreviewHydraulicErosionPass::poll(std::vector<uint16_t> &erodedHeightmap, std::vector<uint8_t> &erosionFlowMap, std::string &error)
{
  if (!running_ || !future_.valid() || !rhi_.isCompleted(future_))
    return false;
  if (future_.checkStatus() == gpu::FenceStatus::ERROR)
  {
    error = "GPU erosion command submission failed";
    future_ = {};
    destroy();
    return true;
  }
  future_ = {};
  completedIterations_ += submittedIterations_;
  submittedIterations_ = 0u;
  if (cancelRequested_)
  {
    error = "GPU erosion cancelled";
    destroy();
    return true;
  }
  if (completedIterations_ < settings_.iterations)
  {
    try
    {
      submitBatch();
    }
    catch (const std::exception &exception)
    {
      error = std::string("GPU erosion failed: ") + exception.what();
      destroy();
      return true;
    }
    return false;
  }
  const bool success = readResult(erodedHeightmap, erosionFlowMap, error);
  destroy();
  if (!success && error.empty())
    error = "Could not read the eroded heightmap from the GPU";
  return true;
}

bool TerrainPreviewHydraulicErosionPass::readResult(std::vector<uint16_t> &heightmap, std::vector<uint8_t> &flowMap, std::string &error)
{
  std::vector<float> state;
  if (!rhi_.readTextureRGBA32F(stateTextures_[0], width_, height_, state))
  {
    error = "Could not read the GPU erosion state texture";
    return false;
  }
  const size_t pixelCount = static_cast<size_t>(width_) * height_;
  if (state.size() != pixelCount * 4u)
  {
    error = "GPU erosion returned an invalid state texture";
    return false;
  }
  heightmap.resize(pixelCount);
  for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    const float elevation = std::isfinite(state[pixel * 4u]) ? std::clamp(state[pixel * 4u], 0.0f, 1.0f) : 0.0f;
    heightmap[pixel] = static_cast<uint16_t>(std::lround(elevation * 65535.0f));
  }
  std::vector<float> positiveFlow;
  positiveFlow.reserve(pixelCount);
  for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    const float value = state[pixel * 4u + 3u];
    if (heightmap[pixel] != 0u && std::isfinite(value) && value > 0.0f)
      positiveFlow.push_back(value);
  }
  float threshold = 0.0f;
  float scale = 1.0f;
  if (!positiveFlow.empty())
  {
    const size_t thresholdIndex = std::min(positiveFlow.size() - 1u, static_cast<size_t>(static_cast<double>(positiveFlow.size()) * 0.94));
    std::nth_element(positiveFlow.begin(), positiveFlow.begin() + static_cast<std::ptrdiff_t>(thresholdIndex), positiveFlow.end());
    threshold = std::max(positiveFlow[thresholdIndex], 0.0f);
    const size_t scaleIndex = std::min(positiveFlow.size() - 1u, static_cast<size_t>(static_cast<double>(positiveFlow.size()) * 0.995));
    std::nth_element(positiveFlow.begin(), positiveFlow.begin() + static_cast<std::ptrdiff_t>(scaleIndex), positiveFlow.end());
    scale = std::max(positiveFlow[scaleIndex], threshold + 1.0e-8f);
  }
  flowMap.resize(pixelCount);
  for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    const float value = state[pixel * 4u + 3u];
    const float normalized = std::pow(std::clamp((value - threshold) / (scale - threshold), 0.0f, 1.0f), 0.46f);
    flowMap[pixel] = heightmap[pixel] == 0u || value <= threshold ? 0u : static_cast<uint8_t>(std::lround(32.0f + normalized * 223.0f));
  }
  return true;
}

void TerrainPreviewHydraulicErosionPass::cancel()
{
  cancelRequested_ = true;
}

float TerrainPreviewHydraulicErosionPass::progress() const
{
  if (settings_.iterations == 0u)
    return 0.0f;
  return std::clamp(static_cast<float>(completedIterations_) / static_cast<float>(settings_.iterations), 0.0f, 1.0f);
}

void TerrainPreviewHydraulicErosionPass::destroy()
{
  if (future_.valid())
  {
    rhi_.blockUntil(future_);
    future_ = {};
  }
  for (const gpu::BindingGroupsId group : bindingGroupIds_)
    if (group != gpu::BindingGroupsId::Invalid)
      rhi_.deleteBindingGroups(group);
  bindingGroupIds_.fill(gpu::BindingGroupsId::Invalid);
  for (const gpu::ComputePipelineId pipeline : pipelineIds_)
    if (pipeline != gpu::ComputePipelineId::Invalid)
      rhi_.deleteComputePipeline(pipeline);
  pipelineIds_.fill(gpu::ComputePipelineId::Invalid);
  if (shaderId_ != gpu::ShaderId::Invalid)
    rhi_.deleteShader(shaderId_);
  shaderId_ = gpu::ShaderId::Invalid;
  if (bindingsLayoutId_ != gpu::BindingsLayoutId::Invalid)
    rhi_.deleteBindingsLayout(bindingsLayoutId_);
  bindingsLayoutId_ = gpu::BindingsLayoutId::Invalid;
  if (uniformBuffer_ != gpu::BufferId::Invalid)
    rhi_.deleteBuffer(uniformBuffer_);
  uniformBuffer_ = gpu::BufferId::Invalid;
  for (gpu::TextureId &texture : stateTextures_)
  {
    if (texture != gpu::TextureId::Invalid)
      rhi_.deleteTexture(texture);
    texture = gpu::TextureId::Invalid;
  }
  for (gpu::TextureId &texture : fluxTextures_)
  {
    if (texture != gpu::TextureId::Invalid)
      rhi_.deleteTexture(texture);
    texture = gpu::TextureId::Invalid;
  }
  if (climateTexture_ != gpu::TextureId::Invalid)
    rhi_.deleteTexture(climateTexture_);
  climateTexture_ = gpu::TextureId::Invalid;
  running_ = false;
  cancelRequested_ = false;
}

TerrainPreviewImGuiOverlayPass::TerrainPreviewImGuiOverlayPass(TerrainPreviewRHI &rhi) : rhi_(rhi)
{
}

TerrainPreviewImGuiOverlayPass::~TerrainPreviewImGuiOverlayPass()
{
  destroy();
}

void TerrainPreviewImGuiOverlayPass::initialize(const gpu::SwapChain swapChain)
{
  destroy();
  renderPass_ = createRenderPass(rhi_.getDevice(), rhi_.getSwapChainVkFormat(swapChain), false);
  preserveRenderPass_ = createRenderPass(rhi_.getDevice(), rhi_.getSwapChainVkFormat(swapChain), true);
}

void TerrainPreviewImGuiOverlayPass::destroy()
{
  destroyFramebuffers();
  if (preserveRenderPass_ != VK_NULL_HANDLE)
  {
    vkDestroyRenderPass(rhi_.getDevice(), preserveRenderPass_, nullptr);
    preserveRenderPass_ = VK_NULL_HANDLE;
  }
  if (renderPass_ != VK_NULL_HANDLE)
  {
    vkDestroyRenderPass(rhi_.getDevice(), renderPass_, nullptr);
    renderPass_ = VK_NULL_HANDLE;
  }
}

void TerrainPreviewImGuiOverlayPass::destroyFramebuffers()
{
  for (auto &[_, resources] : framebuffers_)
  {
    if (resources.framebuffer != VK_NULL_HANDLE)
      vkDestroyFramebuffer(rhi_.getDevice(), resources.framebuffer, nullptr);
    rhi_.destroySampledTextureView(resources.depthView);
    if (resources.depthTexture != gpu::TextureId::Invalid)
      rhi_.deleteTexture(resources.depthTexture);
  }
  framebuffers_.clear();
}

void TerrainPreviewImGuiOverlayPass::onSwapChainRecreated(const gpu::SwapChain swapChain)
{
  destroyFramebuffers();
  if (renderPass_ == VK_NULL_HANDLE)
    renderPass_ = createRenderPass(rhi_.getDevice(), rhi_.getSwapChainVkFormat(swapChain), false);
  if (preserveRenderPass_ == VK_NULL_HANDLE)
    preserveRenderPass_ = createRenderPass(rhi_.getDevice(), rhi_.getSwapChainVkFormat(swapChain), true);
}

void TerrainPreviewImGuiOverlayPass::render(gpu::SwapChain &swapChain, const gpu::TextureView &swapChainView, ImDrawData *drawData, const std::function<void(VkCommandBuffer, VkExtent2D)> &beforeImGui, const bool preserveColor)
{
  rhi_.presentWithOverlay(
      swapChain,
      swapChainView.resourceId,
      // The globe's RenderGraph leaves the image as a colour attachment. Saying
      // so keeps presentWithOverlay from inserting a redundant transition that
      // would discard what the graph just drew.
      preserveColor ? gpu::ResourceLayout::COLOR_ATTACHMENT : gpu::ResourceLayout::UNDEFINED,
      [this, drawData, beforeImGui, preserveColor, framebufferKey = static_cast<uintptr_t>(swapChainView.resourceId)](VkCommandBuffer commandBuffer, VkImageView imageView, VkExtent2D extent)
      {
        FramebufferResources &resources = framebuffers_[framebufferKey];
        if (resources.framebuffer == VK_NULL_HANDLE)
        {
          resources.depthTexture = rhi_.createTexture({
            .name = "polygonTerrain.previewDepth",
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
          const std::array<VkImageView, 2> attachments{imageView, resources.depthView.view};
          VkFramebufferCreateInfo framebufferInfo{};
          framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
          // Either render pass works here: they share attachment formats and
          // sample counts, and framebuffer creation only needs compatibility.
          framebufferInfo.renderPass = renderPass_;
          framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
          framebufferInfo.pAttachments = attachments.data();
          framebufferInfo.width = extent.width;
          framebufferInfo.height = extent.height;
          framebufferInfo.layers = 1u;
          checkVkResult(vkCreateFramebuffer(rhi_.getDevice(), &framebufferInfo, nullptr, &resources.framebuffer));
        }

        std::array<VkClearValue, 2> clearValues{};
        clearValues[0].color = {{0.012f, 0.012f, 0.031f, 1.0f}};
        clearValues[1].depthStencil = {1.0f, 0u};
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = preserveColor ? preserveRenderPass_ : renderPass_;
        renderPassInfo.framebuffer = resources.framebuffer;
        renderPassInfo.renderArea.extent = extent;
        // Depth still clears in both variants, so both still need clear values.
        renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        renderPassInfo.pClearValues = clearValues.data();
        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        if (beforeImGui)
          beforeImGui(commandBuffer, extent);
        if (drawData != nullptr && drawData->CmdListsCount > 0)
          ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
        vkCmdEndRenderPass(commandBuffer);
      });
}

void TerrainPreviewImGuiOverlayPass::checkVkResult(const VkResult result)
{
  if (result != VK_SUCCESS)
    throw std::runtime_error("Polygon terrain preview Vulkan pass failed with VkResult=" + std::to_string(static_cast<int>(result)));
}

VkRenderPass TerrainPreviewImGuiOverlayPass::createRenderPass(const VkDevice device, const VkFormat swapChainFormat, const bool preserveColor)
{
  VkAttachmentDescription colorAttachment{};
  colorAttachment.format = swapChainFormat;
  colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  colorAttachment.loadOp = preserveColor ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentDescription depthAttachment{};
  depthAttachment.format = VK_FORMAT_D32_SFLOAT;
  depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference colorReference{};
  colorReference.attachment = 0u;
  colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference depthReference{};
  depthReference.attachment = 1u;
  depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1u;
  subpass.pColorAttachments = &colorReference;
  subpass.pDepthStencilAttachment = &depthReference;

  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0u;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

  const std::array<VkAttachmentDescription, 2> attachments{colorAttachment, depthAttachment};

  VkRenderPassCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  info.attachmentCount = static_cast<uint32_t>(attachments.size());
  info.pAttachments = attachments.data();
  info.subpassCount = 1u;
  info.pSubpasses = &subpass;
  info.dependencyCount = 1u;
  info.pDependencies = &dependency;

  VkRenderPass renderPass = VK_NULL_HANDLE;
  checkVkResult(vkCreateRenderPass(device, &info, nullptr, &renderPass));
  return renderPass;
}

TerrainPreviewSpherePass::TerrainPreviewSpherePass(TerrainPreviewRHI &rhi, const gpu::SamplerId sampler, std::string shaderPath) : rhi_(rhi), sampler_(sampler), shaderPath_(std::move(shaderPath))
{
}

TerrainPreviewSpherePass::~TerrainPreviewSpherePass()
{
  destroy();
}

bool TerrainPreviewSpherePass::ready() const
{
  return pipeline_ != VK_NULL_HANDLE && descriptorSet_ != VK_NULL_HANDLE && vertexBuffer_ != gpu::BufferId::Invalid && indexBuffer_ != gpu::BufferId::Invalid && uniformBuffer_ != gpu::BufferId::Invalid &&
         atlasView_ != VK_NULL_HANDLE && regionsView_ != VK_NULL_HANDLE && indexCount_ != 0u;
}

void TerrainPreviewSpherePass::initialize(const VkRenderPass renderPass)
{
  destroy();
  createMesh();
  createDescriptors();
  createPipeline(renderPass);
}

void TerrainPreviewSpherePass::destroy()
{
  const VkDevice device = rhi_.getDevice();
  if (pipeline_ != VK_NULL_HANDLE)
  {
    vkDestroyPipeline(device, pipeline_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
  }
  if (pipelineLayout_ != VK_NULL_HANDLE)
  {
    vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
    pipelineLayout_ = VK_NULL_HANDLE;
  }
  if (shaderModule_ != VK_NULL_HANDLE)
  {
    vkDestroyShaderModule(device, shaderModule_, nullptr);
    shaderModule_ = VK_NULL_HANDLE;
  }
  if (descriptorPool_ != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
    descriptorPool_ = VK_NULL_HANDLE;
    descriptorSet_ = VK_NULL_HANDLE;
  }
  if (descriptorSetLayout_ != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorSetLayout(device, descriptorSetLayout_, nullptr);
    descriptorSetLayout_ = VK_NULL_HANDLE;
  }
  if (uniformBuffer_ != gpu::BufferId::Invalid)
  {
    rhi_.deleteBuffer(uniformBuffer_);
    uniformBuffer_ = gpu::BufferId::Invalid;
  }
  if (indexBuffer_ != gpu::BufferId::Invalid)
  {
    rhi_.deleteBuffer(indexBuffer_);
    indexBuffer_ = gpu::BufferId::Invalid;
  }
  if (overlayIndexBuffer_ != gpu::BufferId::Invalid)
  {
    rhi_.deleteBuffer(overlayIndexBuffer_);
    overlayIndexBuffer_ = gpu::BufferId::Invalid;
  }
  if (overlayVertexBuffer_ != gpu::BufferId::Invalid)
  {
    rhi_.deleteBuffer(overlayVertexBuffer_);
    overlayVertexBuffer_ = gpu::BufferId::Invalid;
  }
  if (vertexBuffer_ != gpu::BufferId::Invalid)
  {
    rhi_.deleteBuffer(vertexBuffer_);
    vertexBuffer_ = gpu::BufferId::Invalid;
  }
  indexCount_ = 0u;
  overlayIndexCount_ = 0u;
  vertices_.clear();
  atlasView_ = VK_NULL_HANDLE;
  regionsView_ = VK_NULL_HANDLE;
}

void TerrainPreviewSpherePass::setAtlases(const GpuPreviewTexture &atlas, const GpuPreviewTexture &regions)
{
  const VkImageView nextView = atlas.view.view;
  const VkImageView nextRegionsView = regions.view.view != VK_NULL_HANDLE ? regions.view.view : nextView;
  if (atlasView_ == nextView && regionsView_ == nextRegionsView)
    return;
  atlasView_ = nextView;
  regionsView_ = nextRegionsView;
  updateDescriptorImage();
}

void TerrainPreviewSpherePass::setSurfaceMesh(
    const std::vector<procedural::SpherePoint> &positions,
    const std::vector<uint16_t> &elevations,
    const std::vector<uint32_t> &indices,
    const std::vector<uint32_t> *colors)
{
  if (positions.empty() || elevations.size() != positions.size() || indices.empty() || vertexBuffer_ == gpu::BufferId::Invalid || indexBuffer_ == gpu::BufferId::Invalid)
    return;

  const auto elevationColor = [](const float elevation)
  {
    using Color = std::array<float, 4>;
    if (elevation <= 0.0f)
      return Color{0.03f, 0.10f, 0.24f, 1.0f};
    struct Stop
    {
      float height;
      Color color;
    };
    static constexpr std::array<Stop, 7> stops{{
        {0.000f, {0.75f, 0.68f, 0.42f, 1.0f}},
        {0.025f, {0.29f, 0.56f, 0.24f, 1.0f}},
        {0.250f, {0.18f, 0.44f, 0.25f, 1.0f}},
        {0.500f, {0.46f, 0.38f, 0.23f, 1.0f}},
        {0.720f, {0.60f, 0.50f, 0.41f, 1.0f}},
        {0.880f, {0.82f, 0.81f, 0.78f, 1.0f}},
        {1.000f, {0.96f, 0.97f, 0.98f, 1.0f}},
    }};
    for (size_t index = 1u; index < stops.size(); ++index)
      if (elevation <= stops[index].height)
      {
        const float t = std::clamp((elevation - stops[index - 1u].height) / std::max(0.00001f, stops[index].height - stops[index - 1u].height), 0.0f, 1.0f);
        Color color{};
        for (uint32_t channel = 0u; channel < 4u; ++channel)
          color[channel] = stops[index - 1u].color[channel] + (stops[index].color[channel] - stops[index - 1u].color[channel]) * t;
        return color;
      }
    return stops.back().color;
  };
  const auto setTerrainVertex = [&elevationColor](Vertex &vertex, const float elevation, const std::array<float, 4> *overrideColor = nullptr)
  {
    const float normalized = std::clamp((elevation - 0.025f) / 0.955f, 0.0f, 0.999999f);
    const float rawElevation = elevation <= 0.0f ? 0.0f : -std::log(1.0f - normalized) / 1.55f;
    const float radius = elevation <= 0.0f ? 0.992f : 1.0f + rawElevation * 0.040f;
    vertex.position[0] = vertex.normal[0] * radius;
    vertex.position[1] = vertex.normal[1] * radius;
    vertex.position[2] = vertex.normal[2] * radius;
    vertex.uv[0] = 2.0f;
    vertex.uv[1] = rawElevation;
    const std::array<float, 4> color = overrideColor != nullptr ? *overrideColor : elevationColor(elevation);
    std::copy(color.begin(), color.end(), vertex.color);
  };

  std::vector<Vertex> refinedVertices;
  refinedVertices.reserve(positions.size() + indices.size() / 2u);
  for (size_t vertexIndex = 0u; vertexIndex < positions.size(); ++vertexIndex)
  {
    const procedural::SpherePoint &point = positions[vertexIndex];
    const float x = static_cast<float>(point.x);
    const float y = static_cast<float>(point.y);
    const float z = static_cast<float>(point.z);
    const float inverseLength = 1.0f / std::max(0.00001f, std::sqrt(x * x + y * y + z * z));
    const std::array<float, 3> normal{x * inverseLength, y * inverseLength, z * inverseLength};
    Vertex vertex{{normal[0], normal[1], normal[2]}, {2.0f, 0.0f}, {normal[0], normal[1], normal[2]}, {1.0f, 1.0f, 1.0f, 1.0f}};
    std::array<float, 4> overrideColor{};
    const std::array<float, 4> *overrideColorPointer = nullptr;
    if (colors != nullptr && colors->size() == positions.size())
    {
      const uint32_t packed = (*colors)[vertexIndex];
      overrideColor = {
          static_cast<float>((packed >> 24u) & 0xffu) / 255.0f,
          static_cast<float>((packed >> 16u) & 0xffu) / 255.0f,
          static_cast<float>((packed >> 8u) & 0xffu) / 255.0f,
          static_cast<float>(packed & 0xffu) / 255.0f};
      overrideColorPointer = &overrideColor;
    }
    setTerrainVertex(vertex, static_cast<float>(elevations[vertexIndex]) / 65535.0f, overrideColorPointer);
    refinedVertices.push_back(vertex);
  }

  // Keep display tessellation independent from simulation density. Smaller
  // graphs receive one visual subdivision; already-high-resolution graphs use
  // their native dual mesh so changing the quality slider cannot overflow the
  // fixed level-8 preview buffers.
  const bool subdivide =
      indices.size() * 4u * sizeof(uint32_t) < rhi_.bufferSize(indexBuffer_) * 9u / 10u &&
      (positions.size() + indices.size() / 2u) * sizeof(Vertex) < rhi_.bufferSize(vertexBuffer_) * 9u / 10u;
  std::unordered_map<uint64_t, uint32_t> midpoints;
  if (subdivide)
    midpoints.reserve(indices.size());
  const auto midpoint = [&refinedVertices, &midpoints, &setTerrainVertex](const uint32_t a, const uint32_t b)
  {
    const uint32_t lo = std::min(a, b);
    const uint32_t hi = std::max(a, b);
    const uint64_t key = (static_cast<uint64_t>(lo) << 32u) | hi;
    if (const auto found = midpoints.find(key); found != midpoints.end())
      return found->second;
    const Vertex &va = refinedVertices[a];
    const Vertex &vb = refinedVertices[b];
    const float x = va.normal[0] + vb.normal[0];
    const float y = va.normal[1] + vb.normal[1];
    const float z = va.normal[2] + vb.normal[2];
    const float inverseLength = 1.0f / std::max(0.00001f, std::sqrt(x * x + y * y + z * z));
    const uint32_t index = static_cast<uint32_t>(refinedVertices.size());
    Vertex vertex{{x * inverseLength, y * inverseLength, z * inverseLength}, {2.0f, 0.0f}, {x * inverseLength, y * inverseLength, z * inverseLength}, {1, 1, 1, 1}};
    const std::array<float, 4> color{
        (va.color[0] + vb.color[0]) * 0.5f,
        (va.color[1] + vb.color[1]) * 0.5f,
        (va.color[2] + vb.color[2]) * 0.5f,
        (va.color[3] + vb.color[3]) * 0.5f};
    const float rawElevation = (va.uv[1] + vb.uv[1]) * 0.5f;
    const float encodedElevation = rawElevation <= 0.0f ? 0.0f : std::clamp(0.025f + 0.955f * (1.0f - std::exp(-1.55f * rawElevation)), 0.0f, 0.995f);
    setTerrainVertex(vertex, encodedElevation, &color);
    refinedVertices.push_back(vertex);
    midpoints.emplace(key, index);
    return index;
  };
  std::vector<uint32_t> refinedIndices;
  if (!subdivide)
    refinedIndices = indices;
  else
  {
    refinedIndices.reserve(indices.size() * 4u);
    for (size_t triangle = 0u; triangle + 2u < indices.size(); triangle += 3u)
    {
      const uint32_t a = indices[triangle];
      const uint32_t b = indices[triangle + 1u];
      const uint32_t c = indices[triangle + 2u];
      const uint32_t ab = midpoint(a, b);
      const uint32_t bc = midpoint(b, c);
      const uint32_t ca = midpoint(c, a);
      refinedIndices.insert(refinedIndices.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
    }
  }
  const auto appendShell = [this, &refinedVertices, &refinedIndices](const uint32_t subdivisions, const float radius, const float marker)
  {
    std::vector<Vertex> shellVertices;
    std::vector<uint32_t> shellIndices;
    buildIcosphere(subdivisions, shellVertices, shellIndices);
    const uint32_t offset = static_cast<uint32_t>(refinedVertices.size());
    for (Vertex &vertex : shellVertices)
    {
      vertex.uv[0] = marker;
      vertex.uv[1] = 0.0f;
      vertex.position[0] = vertex.normal[0] * radius;
      vertex.position[1] = vertex.normal[1] * radius;
      vertex.position[2] = vertex.normal[2] * radius;
    }
    refinedVertices.insert(refinedVertices.end(), shellVertices.begin(), shellVertices.end());
    refinedIndices.reserve(refinedIndices.size() + shellIndices.size());
    for (const uint32_t index : shellIndices)
      refinedIndices.push_back(offset + index);
  };
  // A tiny offset compensates for the chord sag of the low-poly water shell;
  // otherwise its triangle centres sit visibly below the first beach band.
  appendShell(5u, 1.00025f, -2.0f);
  appendShell(5u, 1.10f, -3.0f);

  if (refinedVertices.size() * sizeof(Vertex) > rhi_.bufferSize(vertexBuffer_) || refinedIndices.size() * sizeof(uint32_t) > rhi_.bufferSize(indexBuffer_))
    throw std::runtime_error("Tessellated Red Blob dual sphere mesh exceeds the preview buffer capacity");

  vertices_ = std::move(refinedVertices);
  indexCount_ = static_cast<uint32_t>(refinedIndices.size());
  rhi_.bufferWrite(vertexBuffer_, 0u, vertices_.size() * sizeof(Vertex), vertices_.data());
  rhi_.bufferWrite(indexBuffer_, 0u, refinedIndices.size() * sizeof(uint32_t), refinedIndices.data());
}

namespace
{
float wrappedUnit(float value)
{
  value = std::fmod(value, 1.0f);
  if (value < 0.0f)
    value += 1.0f;
  return value;
}

std::array<float, 4> sampleBilinearRGBA(const uint32_t width, const uint32_t height, const std::vector<uint8_t> &rgba, const float u, const float v)
{
  if (width == 0u || height == 0u || rgba.size() != static_cast<size_t>(width) * height * 4u)
    return {1.0f, 1.0f, 1.0f, 1.0f};

  const float fx = wrappedUnit(u) * static_cast<float>(width) - 0.5f;
  const float fy = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(height) - 0.5f;
  const int32_t x0 = static_cast<int32_t>(std::floor(fx));
  const int32_t y0 = static_cast<int32_t>(std::floor(fy));
  const float tx = fx - static_cast<float>(x0);
  const float ty = fy - static_cast<float>(y0);
  const auto texel = [width, height, &rgba](int32_t x, int32_t y, const uint32_t channel)
  {
    while (x < 0)
      x += static_cast<int32_t>(width);
    x %= static_cast<int32_t>(width);
    y = std::clamp(y, 0, static_cast<int32_t>(height) - 1);
    const size_t index = (static_cast<size_t>(y) * width + static_cast<uint32_t>(x)) * 4u + channel;
    return static_cast<float>(rgba[index]) / 255.0f;
  };

  std::array<float, 4> out{};
  for (uint32_t channel = 0u; channel < 4u; ++channel)
  {
    const float a = texel(x0, y0, channel);
    const float b = texel(x0 + 1, y0, channel);
    const float c = texel(x0, y0 + 1, channel);
    const float d = texel(x0 + 1, y0 + 1, channel);
    const float top = a + (b - a) * tx;
    const float bottom = c + (d - c) * tx;
    out[channel] = top + (bottom - top) * ty;
  }
  return out;
}

float sampleBilinearHeight(const uint32_t width, const uint32_t height, const std::vector<uint16_t> &heightmap, const float u, const float v)
{
  if (width == 0u || height == 0u || heightmap.size() != static_cast<size_t>(width) * height)
    return 0.0f;
  const float fx = wrappedUnit(u) * static_cast<float>(width) - 0.5f;
  const float fy = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(height) - 0.5f;
  const int32_t x0 = static_cast<int32_t>(std::floor(fx));
  const int32_t y0 = static_cast<int32_t>(std::floor(fy));
  const float tx = fx - static_cast<float>(x0);
  const float ty = fy - static_cast<float>(y0);
  const auto texel = [width, height, &heightmap](int32_t x, int32_t y)
  {
    while (x < 0)
      x += static_cast<int32_t>(width);
    x %= static_cast<int32_t>(width);
    y = std::clamp(y, 0, static_cast<int32_t>(height) - 1);
    return static_cast<float>(heightmap[static_cast<size_t>(y) * width + static_cast<uint32_t>(x)]) / 65535.0f;
  };
  const float a = texel(x0, y0);
  const float b = texel(x0 + 1, y0);
  const float c = texel(x0, y0 + 1);
  const float d = texel(x0 + 1, y0 + 1);
  return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

float sampleNearestHeight(const uint32_t width, const uint32_t height, const std::vector<uint16_t> &heightmap, const float u, const float v)
{
  if (width == 0u || height == 0u || heightmap.size() != static_cast<size_t>(width) * height)
    return 0.0f;
  const uint32_t x = static_cast<uint32_t>(std::floor(wrappedUnit(u) * static_cast<float>(width))) % width;
  const uint32_t y = std::min(static_cast<uint32_t>(std::floor(std::clamp(v, 0.0f, 1.0f) * static_cast<float>(height))), height - 1u);
  return static_cast<float>(heightmap[static_cast<size_t>(y) * width + x]) / 65535.0f;
}
} // namespace

void TerrainPreviewSpherePass::setDirectAtlas(const uint32_t width, const uint32_t height, const std::vector<uint8_t> &rgba, const std::vector<uint16_t> *heightmap)
{
  const size_t pixelCount = static_cast<size_t>(width) * height;
  const bool hasColor = rgba.size() == pixelCount * 4u;
  const bool hasHeight = heightmap != nullptr && heightmap->size() == pixelCount;
  if (vertexBuffer_ == gpu::BufferId::Invalid || vertices_.empty() || (!hasColor && !hasHeight))
    return;

  constexpr float pi = 3.14159265358979323846f;
  for (Vertex &vertex : vertices_)
  {
    // Negative UV markers belong to the independently rendered ocean shell.
    // It must remain at sea level instead of being displaced by the terrain.
    if (vertex.uv[0] < 0.0f)
      continue;
    const float x = vertex.normal[0];
    const float y = std::clamp(vertex.normal[1], -1.0f, 1.0f);
    const float z = vertex.normal[2];
    const float longitude = std::atan2(z, x);
    const float latitude = std::asin(y);
    const float u = (longitude + pi) / (2.0f * pi);
    const float v = 0.5f - latitude / pi;
    if (hasColor)
    {
      std::array<float, 4> color = sampleBilinearRGBA(width, height, rgba, u, v);
      const float poleT = std::clamp((std::abs(y) - 0.925f) / 0.070f, 0.0f, 1.0f);
      const float poleBlend = poleT * poleT * (3.0f - 2.0f * poleT);
      if (poleBlend > 0.0f)
      {
        std::array<float, 4> poleColor{};
        for (const float meridian : {0.125f, 0.375f, 0.625f, 0.875f})
        {
          const std::array<float, 4> sample = sampleBilinearRGBA(width, height, rgba, meridian, v);
          for (uint32_t channel = 0u; channel < 4u; ++channel)
            poleColor[channel] += sample[channel] * 0.25f;
        }
        for (uint32_t channel = 0u; channel < 4u; ++channel)
          color[channel] += (poleColor[channel] - color[channel]) * poleBlend;
      }
      vertex.color[0] = color[0];
      vertex.color[1] = color[1];
      vertex.color[2] = color[2];
      vertex.color[3] = color[3];
    }
    float elevation = 0.0f;
    if (hasHeight)
    {
      elevation = sampleBilinearHeight(width, height, *heightmap, u, v);
      const float poleT = std::clamp((std::abs(y) - 0.925f) / 0.070f, 0.0f, 1.0f);
      const float poleBlend = poleT * poleT * (3.0f - 2.0f * poleT);
      if (poleBlend > 0.0f)
      {
        const float poleElevation =
            (sampleBilinearHeight(width, height, *heightmap, 0.125f, v) + sampleBilinearHeight(width, height, *heightmap, 0.375f, v) +
             sampleBilinearHeight(width, height, *heightmap, 0.625f, v) + sampleBilinearHeight(width, height, *heightmap, 0.875f, v)) *
            0.25f;
        elevation += (poleElevation - elevation) * poleBlend;
      }
    }
    if (hasHeight)
      vertex.uv[1] = vertex.uv[0] > 1.0f ? vertex.uv[1] * 0.32f + elevation * 0.68f : elevation;
    // Match the reference rendering model: signed ocean topography sits below
    // a separate radius-1 water shell, while land uses restrained displacement.
    // The stored atlas reserves zero for water, so nearest sampling supplies
    // the discrete land mask and bilinear sampling keeps visible relief smooth.
    const bool ocean = hasHeight && sampleNearestHeight(width, height, *heightmap, u, v) <= 0.0f;
    const float radius = ocean ? 0.992f + std::min(elevation * 0.12f, 0.007f) : 1.0f + elevation * 0.040f;
    vertex.position[0] = vertex.normal[0] * radius;
    vertex.position[1] = vertex.normal[1] * radius;
    vertex.position[2] = vertex.normal[2] * radius;
  }

  rhi_.bufferWrite(vertexBuffer_, 0u, vertices_.size() * sizeof(Vertex), vertices_.data());
}

namespace
{
std::array<float, 3> normalize3(const std::array<float, 3> &value)
{
  const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
  if (length <= 0.00001f)
    return {0.0f, 0.0f, 1.0f};
  return {value[0] / length, value[1] / length, value[2] / length};
}

std::array<float, 3> add3(const std::array<float, 3> &a, const std::array<float, 3> &b)
{
  return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

std::array<float, 3> subtract3(const std::array<float, 3> &a, const std::array<float, 3> &b)
{
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

std::array<float, 3> multiply3(const std::array<float, 3> &value, const float scale)
{
  return {value[0] * scale, value[1] * scale, value[2] * scale};
}

std::array<float, 3> cross3(const std::array<float, 3> &a, const std::array<float, 3> &b)
{
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

float dot3(const std::array<float, 3> &a, const std::array<float, 3> &b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
} // namespace

void TerrainPreviewSpherePass::setOverlayLines(const std::vector<TerrainPreviewSphereLine> &lines)
{
  if (overlayIndexBuffer_ != gpu::BufferId::Invalid)
  {
    rhi_.deleteBuffer(overlayIndexBuffer_);
    overlayIndexBuffer_ = gpu::BufferId::Invalid;
  }
  if (overlayVertexBuffer_ != gpu::BufferId::Invalid)
  {
    rhi_.deleteBuffer(overlayVertexBuffer_);
    overlayVertexBuffer_ = gpu::BufferId::Invalid;
  }
  overlayIndexCount_ = 0u;
  if (lines.empty())
    return;

  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  vertices.reserve(lines.size() * 4u);
  indices.reserve(lines.size() * 6u);
  constexpr float radius = 1.0025f;
  for (const TerrainPreviewSphereLine &line : lines)
  {
    const std::array<float, 3> a = normalize3(line.a);
    const std::array<float, 3> b = normalize3(line.b);
    const std::array<float, 3> tangent = normalize3(subtract3(b, a));
    const std::array<float, 3> mid = normalize3(add3(a, b));
    std::array<float, 3> side = normalize3(cross3(mid, tangent));
    if (dot3(side, side) < 0.0001f)
      continue;
    side = multiply3(side, std::max(0.0005f, line.width));
    const std::array<float, 3> p0 = multiply3(normalize3(add3(a, side)), radius);
    const std::array<float, 3> p1 = multiply3(normalize3(add3(b, side)), radius);
    const std::array<float, 3> p2 = multiply3(normalize3(subtract3(a, side)), radius);
    const std::array<float, 3> p3 = multiply3(normalize3(subtract3(b, side)), radius);
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    const auto makeVertex = [&line](const std::array<float, 3> &position)
    {
      const std::array<float, 3> normal = normalize3(position);
      Vertex vertex{{position[0], position[1], position[2]}, {-1.0f, -1.0f}, {normal[0], normal[1], normal[2]}, {line.color[0], line.color[1], line.color[2], line.color[3]}};
      return vertex;
    };
    vertices.push_back(makeVertex(p0));
    vertices.push_back(makeVertex(p1));
    vertices.push_back(makeVertex(p2));
    vertices.push_back(makeVertex(p3));
    std::array<uint32_t, 6> quad{base, base + 1u, base + 2u, base + 1u, base + 3u, base + 2u};
    const std::array<float, 3> edgeA = subtract3(p1, p0);
    const std::array<float, 3> edgeB = subtract3(p2, p0);
    if (dot3(cross3(edgeA, edgeB), mid) < 0.0f)
      quad = {base, base + 2u, base + 1u, base + 1u, base + 2u, base + 3u};
    indices.insert(indices.end(), quad.begin(), quad.end());
  }
  if (vertices.empty() || indices.empty())
    return;
  overlayIndexCount_ = static_cast<uint32_t>(indices.size());
  overlayVertexBuffer_ =
      rhi_.createBuffer({.name = "polygonTerrain.sphere.overlay.vertices", .size = vertices.size() * sizeof(Vertex), .usage = static_cast<gpu::BufferUsage>(gpu::BufferUsage_Vertex | gpu::BufferUsage_Push)});
  overlayIndexBuffer_ =
      rhi_.createBuffer({.name = "polygonTerrain.sphere.overlay.indices", .size = indices.size() * sizeof(uint32_t), .usage = static_cast<gpu::BufferUsage>(gpu::BufferUsage_Index | gpu::BufferUsage_Push)});
  rhi_.bufferWrite(overlayVertexBuffer_, 0u, vertices.size() * sizeof(Vertex), vertices.data());
  rhi_.bufferWrite(overlayIndexBuffer_, 0u, indices.size() * sizeof(uint32_t), indices.data());
}

std::string TerrainPreviewSpherePass::readBinaryFile(const std::string &path)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("Could not open polygon terrain sphere shader: " + path);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

VkShaderModule TerrainPreviewSpherePass::createShaderModule(const VkDevice device, const std::string &spirv)
{
  if (spirv.empty() || (spirv.size() % sizeof(uint32_t)) != 0u)
    throw std::runtime_error("Polygon terrain sphere shader is not valid SPIR-V");
  VkShaderModuleCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = spirv.size();
  info.pCode = reinterpret_cast<const uint32_t *>(spirv.data());
  VkShaderModule module = VK_NULL_HANDLE;
  TerrainPreviewImGuiOverlayPass::checkVkResult(vkCreateShaderModule(device, &info, nullptr, &module));
  return module;
}

void TerrainPreviewSpherePass::buildIcosphere(const uint32_t subdivisions, std::vector<Vertex> &vertices, std::vector<uint32_t> &indices)
{
  constexpr float phi = 1.6180339887498948482f;
  std::vector<std::array<float, 3>> positions{
    {-1.0f, phi, 0.0f}, {1.0f, phi, 0.0f}, {-1.0f, -phi, 0.0f}, {1.0f, -phi, 0.0f},
    {0.0f, -1.0f, phi}, {0.0f, 1.0f, phi}, {0.0f, -1.0f, -phi}, {0.0f, 1.0f, -phi},
    {phi, 0.0f, -1.0f}, {phi, 0.0f, 1.0f}, {-phi, 0.0f, -1.0f}, {-phi, 0.0f, 1.0f},
  };
  indices = {
    0u, 11u, 5u, 0u, 5u, 1u, 0u, 1u, 7u, 0u, 7u, 10u, 0u, 10u, 11u,
    1u, 5u, 9u, 5u, 11u, 4u, 11u, 10u, 2u, 10u, 7u, 6u, 7u, 1u, 8u,
    3u, 9u, 4u, 3u, 4u, 2u, 3u, 2u, 6u, 3u, 6u, 8u, 3u, 8u, 9u,
    4u, 9u, 5u, 2u, 4u, 11u, 6u, 2u, 10u, 8u, 6u, 7u, 9u, 8u, 1u,
  };

  // Rotate the topology away from the atlas poles. The sphere is unchanged,
  // but no vertex or high-valence fan sits at atan2(0, 0).
  constexpr float rotateX = 0.317f;
  constexpr float rotateY = 0.193f;
  const float cx = std::cos(rotateX);
  const float sx = std::sin(rotateX);
  const float cy = std::cos(rotateY);
  const float sy = std::sin(rotateY);
  for (std::array<float, 3> &position : positions)
  {
    const float rx = position[0];
    const float ry = position[1] * cx - position[2] * sx;
    const float rz = position[1] * sx + position[2] * cx;
    position = normalize3({rx * cy + rz * sy, ry, -rx * sy + rz * cy});
  }

  const uint32_t levels = std::min(subdivisions, 8u);
  for (uint32_t level = 0u; level < levels; ++level)
  {
    std::unordered_map<uint64_t, uint32_t> midpoints;
    midpoints.reserve(indices.size());
    const auto midpoint = [&](const uint32_t a, const uint32_t b)
    {
      const uint32_t low = std::min(a, b);
      const uint32_t high = std::max(a, b);
      const uint64_t key = (static_cast<uint64_t>(low) << 32u) | high;
      if (const auto found = midpoints.find(key); found != midpoints.end())
        return found->second;
      const uint32_t index = static_cast<uint32_t>(positions.size());
      positions.push_back(normalize3(add3(positions[a], positions[b])));
      midpoints.emplace(key, index);
      return index;
    };
    std::vector<uint32_t> refined;
    refined.reserve(indices.size() * 4u);
    for (size_t triangle = 0u; triangle < indices.size(); triangle += 3u)
    {
      const uint32_t a = indices[triangle];
      const uint32_t b = indices[triangle + 1u];
      const uint32_t c = indices[triangle + 2u];
      const uint32_t ab = midpoint(a, b);
      const uint32_t bc = midpoint(b, c);
      const uint32_t ca = midpoint(c, a);
      const std::array<uint32_t, 12> children{a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca};
      refined.insert(refined.end(), children.begin(), children.end());
    }
    indices = std::move(refined);
  }

  constexpr float pi = 3.14159265358979323846f;
  vertices.clear();
  vertices.reserve(positions.size());
  for (const std::array<float, 3> &position : positions)
  {
    const float longitude = std::atan2(-position[0], position[2]);
    const float latitude = std::asin(std::clamp(position[1], -1.0f, 1.0f));
    vertices.push_back(
        Vertex{{position[0], position[1], position[2]}, {(longitude + pi) / (2.0f * pi), 0.5f - latitude / pi}, {position[0], position[1], position[2]}, {1.0f, 1.0f, 1.0f, 1.0f}});
  }

  // Protect against an accidentally inverted base face table.
  for (size_t triangle = 0u; triangle < indices.size(); triangle += 3u)
  {
    const std::array<float, 3> &a = positions[indices[triangle]];
    const std::array<float, 3> &b = positions[indices[triangle + 1u]];
    const std::array<float, 3> &c = positions[indices[triangle + 2u]];
    if (dot3(cross3(subtract3(b, a), subtract3(c, a)), add3(add3(a, b), c)) < 0.0f)
      std::swap(indices[triangle + 1u], indices[triangle + 2u]);
  }
}

namespace
{
std::array<float, 16> identityMatrix()
{
  return {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
}

std::array<float, 16> multiplyMatrix(const std::array<float, 16> &a, const std::array<float, 16> &b)
{
  std::array<float, 16> out{};
  for (uint32_t column = 0u; column < 4u; ++column)
    for (uint32_t row = 0u; row < 4u; ++row)
      out[column * 4u + row] = a[0u * 4u + row] * b[column * 4u + 0u] + a[1u * 4u + row] * b[column * 4u + 1u] + a[2u * 4u + row] * b[column * 4u + 2u] + a[3u * 4u + row] * b[column * 4u + 3u];
  return out;
}

std::array<float, 16> rotationX(const float radians)
{
  std::array<float, 16> out = identityMatrix();
  const float c = std::cos(radians);
  const float s = std::sin(radians);
  out[5] = c;
  out[6] = s;
  out[9] = -s;
  out[10] = c;
  return out;
}

std::array<float, 16> rotationY(const float radians)
{
  std::array<float, 16> out = identityMatrix();
  const float c = std::cos(radians);
  const float s = std::sin(radians);
  out[0] = c;
  out[2] = -s;
  out[8] = s;
  out[10] = c;
  return out;
}

std::array<float, 16> perspective(const float fovyRadians, const float aspect, const float znear, const float zfar)
{
  std::array<float, 16> out{};
  const float f = 1.0f / std::tan(fovyRadians * 0.5f);
  out[0] = f / std::max(0.001f, aspect);
  out[5] = -f;
  out[10] = zfar / (znear - zfar);
  out[11] = -1.0f;
  out[14] = (zfar * znear) / (znear - zfar);
  return out;
}

std::array<float, 16> translation(const float x, const float y, const float z)
{
  std::array<float, 16> out = identityMatrix();
  out[12] = x;
  out[13] = y;
  out[14] = z;
  return out;
}
} // namespace

TerrainPreviewSpherePass::Uniforms TerrainPreviewSpherePass::makeUniforms(const TerrainPreviewSphereSettings &settings, const VkExtent2D extent)
{
  constexpr float pi = 3.14159265358979323846f;
  const float yaw = settings.yawDegrees * pi / 180.0f;
  const float pitch = settings.pitchDegrees * pi / 180.0f;
  const std::array<float, 16> model = multiplyMatrix(rotationY(yaw), rotationX(pitch));
  const std::array<float, 16> view = translation(0.0f, 0.0f, -3.0f / std::max(0.1f, settings.zoom));
  const float aspect = extent.height == 0u ? 1.0f : static_cast<float>(extent.width) / static_cast<float>(extent.height);
  const std::array<float, 16> projection = perspective(48.0f * pi / 180.0f, aspect, 0.1f, 32.0f);
  Uniforms uniforms{};
  uniforms.model = model;
  uniforms.mvp = multiplyMatrix(projection, multiplyMatrix(view, model));
  const float lightLength = std::sqrt(settings.lightX * settings.lightX + settings.lightY * settings.lightY + settings.lightZ * settings.lightZ);
  const float invLightLength = lightLength > 0.0001f ? 1.0f / lightLength : 1.0f;
  uniforms.lightAndLighting = {settings.lightX * invLightLength, settings.lightY * invLightLength, settings.lightZ * invLightLength, settings.lighting};
  uniforms.atlasInfo = {settings.showPlateBoundaries, settings.showOcean, 0.0f, 0.0f};
  return uniforms;
}

void TerrainPreviewSpherePass::createMesh()
{
  std::vector<uint32_t> indices;
  // Rendering resolution is deliberately independent from the 10k-region
  // Voronoi simulation graph. Level 8 gives 1,310,720 nearly uniform triangles
  // without introducing UV poles or changing procedural generation cost.
  buildIcosphere(8u, vertices_, indices);

  // Orogen's coast reads as a shoreline because water is a separate sphere,
  // not blue paint on the displaced terrain. A level-5 icosphere is already
  // visually smooth for this shell and keeps the additional preview memory
  // small relative to the level-8 terrain display mesh.
  std::vector<Vertex> oceanVertices;
  std::vector<uint32_t> oceanIndices;
  buildIcosphere(5u, oceanVertices, oceanIndices);
  const uint32_t oceanVertexOffset = static_cast<uint32_t>(vertices_.size());
  for (Vertex &vertex : oceanVertices)
  {
    vertex.uv[0] = -2.0f;
    vertex.uv[1] = 0.0f;
    vertex.position[0] = vertex.normal[0] * 1.00025f;
    vertex.position[1] = vertex.normal[1] * 1.00025f;
    vertex.position[2] = vertex.normal[2] * 1.00025f;
  }
  vertices_.insert(vertices_.end(), oceanVertices.begin(), oceanVertices.end());
  indices.reserve(indices.size() + oceanIndices.size());
  for (const uint32_t index : oceanIndices)
    indices.push_back(oceanVertexOffset + index);

  std::vector<Vertex> atmosphereVertices;
  std::vector<uint32_t> atmosphereIndices;
  buildIcosphere(5u, atmosphereVertices, atmosphereIndices);
  const uint32_t atmosphereVertexOffset = static_cast<uint32_t>(vertices_.size());
  for (Vertex &vertex : atmosphereVertices)
  {
    vertex.uv[0] = -3.0f;
    vertex.uv[1] = 0.0f;
    vertex.position[0] = vertex.normal[0] * 1.10f;
    vertex.position[1] = vertex.normal[1] * 1.10f;
    vertex.position[2] = vertex.normal[2] * 1.10f;
  }
  vertices_.insert(vertices_.end(), atmosphereVertices.begin(), atmosphereVertices.end());
  indices.reserve(indices.size() + atmosphereIndices.size());
  for (const uint32_t index : atmosphereIndices)
    indices.push_back(atmosphereVertexOffset + index);
  indexCount_ = static_cast<uint32_t>(indices.size());

  vertexBuffer_ = rhi_.createBuffer({.name = "polygonTerrain.sphere.vertices", .size = vertices_.size() * sizeof(Vertex), .usage = static_cast<gpu::BufferUsage>(gpu::BufferUsage_Vertex | gpu::BufferUsage_Push)});
  indexBuffer_ = rhi_.createBuffer({.name = "polygonTerrain.sphere.indices", .size = indices.size() * sizeof(uint32_t), .usage = static_cast<gpu::BufferUsage>(gpu::BufferUsage_Index | gpu::BufferUsage_Push)});
  uniformBuffer_ = rhi_.createBuffer({.name = "polygonTerrain.sphere.uniforms", .size = sizeof(Uniforms), .usage = static_cast<gpu::BufferUsage>(gpu::BufferUsage_Uniform | gpu::BufferUsage_Push)});
  rhi_.bufferWrite(vertexBuffer_, 0u, vertices_.size() * sizeof(Vertex), vertices_.data());
  rhi_.bufferWrite(indexBuffer_, 0u, indices.size() * sizeof(uint32_t), const_cast<uint32_t *>(indices.data()));
}

void TerrainPreviewSpherePass::createDescriptors()
{
  std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
  bindings[0].binding = 0u;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  bindings[0].descriptorCount = 1u;
  bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  bindings[1].binding = 1u;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  bindings[1].descriptorCount = 1u;
  bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  bindings[2].binding = 2u;
  bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  bindings[2].descriptorCount = 1u;
  bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  bindings[3].binding = 3u;
  bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  bindings[3].descriptorCount = 1u;
  bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  bindings[4].binding = 4u;
  bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  bindings[4].descriptorCount = 1u;
  bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
  layoutInfo.pBindings = bindings.data();
  TerrainPreviewImGuiOverlayPass::checkVkResult(vkCreateDescriptorSetLayout(rhi_.getDevice(), &layoutInfo, nullptr, &descriptorSetLayout_));

  std::array<VkDescriptorPoolSize, 3> poolSizes{};
  poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  poolSizes[0].descriptorCount = 1u;
  poolSizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  poolSizes[1].descriptorCount = 2u;
  poolSizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
  poolSizes[2].descriptorCount = 2u;
  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.maxSets = 1u;
  poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
  poolInfo.pPoolSizes = poolSizes.data();
  TerrainPreviewImGuiOverlayPass::checkVkResult(vkCreateDescriptorPool(rhi_.getDevice(), &poolInfo, nullptr, &descriptorPool_));

  VkDescriptorSetAllocateInfo allocateInfo{};
  allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocateInfo.descriptorPool = descriptorPool_;
  allocateInfo.descriptorSetCount = 1u;
  allocateInfo.pSetLayouts = &descriptorSetLayout_;
  TerrainPreviewImGuiOverlayPass::checkVkResult(vkAllocateDescriptorSets(rhi_.getDevice(), &allocateInfo, &descriptorSet_));

  VkDescriptorBufferInfo bufferInfo{};
  bufferInfo.buffer = rhi_.bufferHandle(uniformBuffer_);
  bufferInfo.offset = 0u;
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

void TerrainPreviewSpherePass::updateDescriptorImage()
{
  if (descriptorSet_ == VK_NULL_HANDLE || atlasView_ == VK_NULL_HANDLE)
    return;
  std::array<VkDescriptorImageInfo, 4> imageInfos{};
  imageInfos[0].sampler = rhi_.samplerHandle(sampler_);
  imageInfos[0].imageView = atlasView_;
  imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  imageInfos[1].sampler = rhi_.samplerHandle(sampler_);
  imageInfos[2].sampler = rhi_.samplerHandle(sampler_);
  imageInfos[2].imageView = regionsView_ != VK_NULL_HANDLE ? regionsView_ : atlasView_;
  imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  imageInfos[3].sampler = rhi_.samplerHandle(sampler_);

  std::array<VkWriteDescriptorSet, 4> writes{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = descriptorSet_;
  writes[0].dstBinding = 1u;
  writes[0].descriptorCount = 1u;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  writes[0].pImageInfo = &imageInfos[0];
  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = descriptorSet_;
  writes[1].dstBinding = 2u;
  writes[1].descriptorCount = 1u;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  writes[1].pImageInfo = &imageInfos[1];
  writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[2].dstSet = descriptorSet_;
  writes[2].dstBinding = 3u;
  writes[2].descriptorCount = 1u;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  writes[2].pImageInfo = &imageInfos[2];
  writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[3].dstSet = descriptorSet_;
  writes[3].dstBinding = 4u;
  writes[3].descriptorCount = 1u;
  writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  writes[3].pImageInfo = &imageInfos[3];
  vkUpdateDescriptorSets(rhi_.getDevice(), static_cast<uint32_t>(writes.size()), writes.data(), 0u, nullptr);
}

void TerrainPreviewSpherePass::createPipeline(const VkRenderPass renderPass)
{
  const std::string spirv = readBinaryFile(shaderPath_);
  shaderModule_ = createShaderModule(rhi_.getDevice(), spirv);

  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = 1u;
  pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout_;
  TerrainPreviewImGuiOverlayPass::checkVkResult(vkCreatePipelineLayout(rhi_.getDevice(), &pipelineLayoutInfo, nullptr, &pipelineLayout_));

  std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{};
  shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  shaderStages[0].module = shaderModule_;
  shaderStages[0].pName = "vs_main";
  shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  shaderStages[1].module = shaderModule_;
  shaderStages[1].pName = "fs_main";

  VkVertexInputBindingDescription binding{};
  binding.binding = 0u;
  binding.stride = sizeof(Vertex);
  binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  std::array<VkVertexInputAttributeDescription, 4> attributes{};
  attributes[0] = {0u, 0u, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)};
  attributes[1] = {1u, 0u, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)};
  attributes[2] = {2u, 0u, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)};
  attributes[3] = {3u, 0u, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)};
  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInput.vertexBindingDescriptionCount = 1u;
  vertexInput.pVertexBindingDescriptions = &binding;
  vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
  vertexInput.pVertexAttributeDescriptions = attributes.data();

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1u;
  viewportState.scissorCount = 1u;
  std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
  dynamicState.pDynamicStates = dynamicStates.data();

  VkPipelineRasterizationStateCreateInfo rasterization{};
  rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
  rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterization.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo depthStencil{};
  depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_TRUE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  depthStencil.depthBoundsTestEnable = VK_FALSE;
  depthStencil.stencilTestEnable = VK_FALSE;

  VkPipelineColorBlendAttachmentState colorBlendAttachment{};
  colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  colorBlendAttachment.blendEnable = VK_TRUE;
  colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
  colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
  VkPipelineColorBlendStateCreateInfo colorBlend{};
  colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlend.attachmentCount = 1u;
  colorBlend.pAttachments = &colorBlendAttachment;

  VkGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
  pipelineInfo.pStages = shaderStages.data();
  pipelineInfo.pVertexInputState = &vertexInput;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterization;
  pipelineInfo.pMultisampleState = &multisample;
  pipelineInfo.pDepthStencilState = &depthStencil;
  pipelineInfo.pColorBlendState = &colorBlend;
  pipelineInfo.pDynamicState = &dynamicState;
  pipelineInfo.layout = pipelineLayout_;
  pipelineInfo.renderPass = renderPass;
  pipelineInfo.subpass = 0u;
  TerrainPreviewImGuiOverlayPass::checkVkResult(vkCreateGraphicsPipelines(rhi_.getDevice(), VK_NULL_HANDLE, 1u, &pipelineInfo, nullptr, &pipeline_));
}

void TerrainPreviewSpherePass::render(VkCommandBuffer commandBuffer, const VkExtent2D extent, const TerrainPreviewSphereSettings &settings)
{
  if (!ready())
    return;
  const Uniforms uniforms = makeUniforms(settings, extent);
  rhi_.bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), const_cast<Uniforms *>(&uniforms));

  VkViewport viewport{};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = static_cast<float>(extent.width);
  viewport.height = static_cast<float>(extent.height);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  VkRect2D scissor{};
  scissor.extent = extent;
  vkCmdSetViewport(commandBuffer, 0u, 1u, &viewport);
  vkCmdSetScissor(commandBuffer, 0u, 1u, &scissor);
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  const VkBuffer vertexBuffer = rhi_.bufferHandle(vertexBuffer_);
  const VkDeviceSize offset = 0u;
  vkCmdBindVertexBuffers(commandBuffer, 0u, 1u, &vertexBuffer, &offset);
  vkCmdBindIndexBuffer(commandBuffer, rhi_.bufferHandle(indexBuffer_), 0u, VK_INDEX_TYPE_UINT32);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0u, 1u, &descriptorSet_, 0u, nullptr);
  vkCmdDrawIndexed(commandBuffer, indexCount_, 1u, 0u, 0, 0u);
  if (overlayIndexCount_ != 0u && overlayVertexBuffer_ != gpu::BufferId::Invalid && overlayIndexBuffer_ != gpu::BufferId::Invalid)
  {
    const VkBuffer overlayVertexBuffer = rhi_.bufferHandle(overlayVertexBuffer_);
    vkCmdBindVertexBuffers(commandBuffer, 0u, 1u, &overlayVertexBuffer, &offset);
    vkCmdBindIndexBuffer(commandBuffer, rhi_.bufferHandle(overlayIndexBuffer_), 0u, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(commandBuffer, overlayIndexCount_, 1u, 0u, 0, 0u);
  }
}

} // namespace polygonterrain
