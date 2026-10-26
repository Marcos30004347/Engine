#pragma once

#include "os/File.hpp"
#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/rendering/SparseVoxelGBufferPass.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace voxel::svraster::rendering
{

class SparseVoxelDebugPresentPass final : public ::rendering::Pass
{
public:
  SparseVoxelDebugPresentPass(
      const SparseVoxelGBufferPass::FrameTarget target,
      const ::rendering::Buffer coverage,
      const ::rendering::Buffer linearDepth,
      const ::rendering::Texture output,
      const SparseVoxelDebugPresentUniformsGPU uniforms,
      const ::rendering::Buffer occupancyShadowResults = {},
      const ::rendering::Buffer stableSceneHits = {},
      const ::rendering::Buffer stableSceneVisibility = {},
      const ::rendering::Buffer sceneRays = {},
      const ::rendering::Buffer sceneLights = {},
      const ::rendering::Buffer sceneDIReservoirs = {},
      const ::rendering::Buffer screenSceneHits = {},
      const ::rendering::Buffer mergedSceneHits = {},
      const ::rendering::Buffer sceneRayStatistics = {},
      const ::rendering::Texture reactiveOutput = {},
      const SparseVoxelRasterUniformsGPU raster = {},
      const ::rendering::Buffer skyRays = {},
      const ::rendering::Buffer skyMergedHits = {},
      const ::rendering::Buffer skyStableVisibility = {},
      const ::rendering::Buffer brickOverdraw = {},
      const ::rendering::Buffer microPayloads = {},
      const ::rendering::Buffer terrainOwnershipIds = {},
      const ::rendering::Buffer terrainOwnershipTable = {})
      : target_(target), coverage_(coverage), linearDepth_(linearDepth),
        occupancyShadowResults_(occupancyShadowResults), stableSceneHits_(stableSceneHits), stableSceneVisibility_(stableSceneVisibility), sceneRays_(sceneRays), sceneLights_(sceneLights), sceneDIReservoirs_(sceneDIReservoirs), screenSceneHits_(screenSceneHits), mergedSceneHits_(mergedSceneHits), sceneRayStatistics_(sceneRayStatistics), skyRays_(skyRays), skyMergedHits_(skyMergedHits), skyStableVisibility_(skyStableVisibility), brickOverdraw_(brickOverdraw), microPayloads_(microPayloads), terrainOwnershipIds_(terrainOwnershipIds), terrainOwnershipTable_(terrainOwnershipTable), output_(output), reactiveOutput_(reactiveOutput),
        uniforms_(uniforms), raster_(raster)
  {
  }

  void updateRasterUniforms(const SparseVoxelRasterUniformsGPU &raster)
  {
    raster_ = raster;
    if (rasterBuffer_.isValid())
      renderGraph->bufferWrite(rasterBuffer_, 0u, sizeof(raster_), &raster_);
  }

  void updateUniforms(const SparseVoxelDebugPresentUniformsGPU &uniforms)
  {
    const uint32_t shadowWidth = uniforms_.shadowWidth;
    const uint32_t shadowHeight = uniforms_.shadowHeight;
    uniforms_ = uniforms;
    if (uniforms_.shadowWidth == 0u)
      uniforms_.shadowWidth = shadowWidth;
    if (uniforms_.shadowHeight == 0u)
      uniforms_.shadowHeight = shadowHeight;
    if (uniformBuffer_.isValid())
      renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms_), &uniforms_);
  }

  ~SparseVoxelDebugPresentPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
    if (rasterBuffer_.isValid())
      renderGraph->deleteBuffer(rasterBuffer_);
    if (ownsOccupancyShadowResults_ && occupancyShadowResults_.isValid())
      renderGraph->deleteBuffer(occupancyShadowResults_);
    if (ownsStableSceneHits_ && stableSceneHits_.isValid())
      renderGraph->deleteBuffer(stableSceneHits_);
    if (ownsStableSceneVisibility_ && stableSceneVisibility_.isValid())
      renderGraph->deleteBuffer(stableSceneVisibility_);
    if (ownsSceneRays_ && sceneRays_.isValid()) renderGraph->deleteBuffer(sceneRays_);
    if (ownsSceneLights_ && sceneLights_.isValid()) renderGraph->deleteBuffer(sceneLights_);
    if (ownsSceneDIReservoirs_ && sceneDIReservoirs_.isValid()) renderGraph->deleteBuffer(sceneDIReservoirs_);
    if (ownsScreenSceneHits_ && screenSceneHits_.isValid()) renderGraph->deleteBuffer(screenSceneHits_);
    if (ownsMergedSceneHits_ && mergedSceneHits_.isValid()) renderGraph->deleteBuffer(mergedSceneHits_);
    if (ownsSceneRayStatistics_ && sceneRayStatistics_.isValid()) renderGraph->deleteBuffer(sceneRayStatistics_);
    if (ownsSkyRays_ && skyRays_.isValid()) renderGraph->deleteBuffer(skyRays_);
    if (ownsSkyMergedHits_ && skyMergedHits_.isValid()) renderGraph->deleteBuffer(skyMergedHits_);
    if (ownsSkyStableVisibility_ && skyStableVisibility_.isValid()) renderGraph->deleteBuffer(skyStableVisibility_);
    if (ownsMicroPayloads_ && microPayloads_.isValid())
      renderGraph->deleteBuffer(microPayloads_);
    if (ownsTerrainOwnershipIds_ && terrainOwnershipIds_.isValid())
      renderGraph->deleteBuffer(terrainOwnershipIds_);
    if (ownsTerrainOwnershipTable_ && terrainOwnershipTable_.isValid())
      renderGraph->deleteBuffer(terrainOwnershipTable_);
    if (ownsReactiveOutput_ && reactiveOutput_.isValid()) renderGraph->deleteTexture(reactiveOutput_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms_), &uniforms_);
    renderGraph->bufferWrite(rasterBuffer_, 0u, sizeof(raster_), &raster_);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatch(std::max(1u, (uniforms_.width + 7u) / 8u), std::max(1u, (uniforms_.height + 7u) / 8u), 1u);
  }

private:
  static ::rendering::TextureView sampledView(const ::rendering::Texture texture, const ::rendering::ImageAspectFlags aspect)
  {
    return {
      .texture = texture,
      .access = ::rendering::AccessPattern::SHADER_READ,
      .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
      .index = 0u,
      .flags = aspect,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u,
    };
  }

  static ::rendering::TextureView storageView(const ::rendering::Texture texture)
  {
    return {
      .texture = texture,
      .access = ::rendering::AccessPattern::SHADER_WRITE,
      .layout = ::rendering::ResourceLayout::GENERAL,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u,
    };
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniformBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Uniforms.buffer", .size = sizeof(uniforms_), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push, .frameLocal = true});
    rasterBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Raster.buffer", .size = sizeof(raster_), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push, .frameLocal = true});
    std::vector<::rendering::BindingGroupLayoutTextureEntry> textureLayout;
    for (uint32_t binding = 1u; binding <= 11u; ++binding)
      textureLayout.push_back({.name = "texture" + std::to_string(binding), .binding = binding, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute});
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "coverage", .binding = 12u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "linearDepth", .binding = 13u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "occupancyShadowResults", .binding = 15u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "stableSceneHits", .binding = 16u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "sceneRays", .binding = 17u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "sceneLights", .binding = 18u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "sceneDIReservoirs", .binding = 19u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "screenSceneHits", .binding = 20u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "mergedSceneHits", .binding = 21u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "sceneRayStatistics", .binding = 22u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "skyRays", .binding = 23u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "skyMergedHits", .binding = 24u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "skyStableVisibility", .binding = 25u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "stableSceneVisibility", .binding = 26u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "brickOverdraw", .binding = 29u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "microPayloads", .binding = 30u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "terrainOwnershipIds", .binding = 31u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "terrainOwnershipTable", .binding = 32u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "raster", .binding = 28u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           },
           .textures = std::move(textureLayout),
           .storageTextures = {
             {.name = "outputTexture", .binding = 14u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "reactiveOutput", .binding = 27u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           },
         }}});

    const std::array<::rendering::Texture, 10> colorTextures = {
      target_.instanceId,
      target_.voxelId,
      target_.materialId,
      target_.materialUv,
      target_.baseColor,
      target_.materialProperties,
      target_.emissive,
      target_.geometryNormal,
      target_.normal,
      target_.motionVector,
    };
    std::vector<::rendering::BindingTextureInfo> textures;
    for (uint32_t index = 0u; index < colorTextures.size(); ++index)
      textures.push_back({.textureView = sampledView(colorTextures[index], ::rendering::ImageAspectFlags::Color), .binding = index + 1u});
    textures.push_back({.textureView = sampledView(target_.depth, ::rendering::ImageAspectFlags::Depth), .binding = 11u});
    const uint64_t pixelCount = std::max<uint64_t>(
        1u, uint64_t{uniforms_.width} * uniforms_.height);
    const uint32_t raysPerPixel =
        std::max((uniforms_.reserved0 >> 24u) & 0xfu, 1u);
    const uint64_t sceneRayCount = std::max<uint64_t>(
        1u,
        uint64_t{std::max(uniforms_.reserved0 & 0x00ffffffu, 1u)} *
            std::max(uniforms_.reserved1, 1u) * raysPerPixel);
    const uint64_t shadowPixelCount = std::max<uint64_t>(
        1u, uint64_t{uniforms_.shadowWidth} * uniforms_.shadowHeight);
    if (!occupancyShadowResults_.isValid())
    {
      occupancyShadowResults_ = renderGraph->createBuffer({
        .name = passName + "_EmptyOccupancyShadowResults",
        .size = sizeof(SparseVoxelOccupancyDDAResultGPU),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
            ::rendering::BufferUsage::BufferUsage_Push,
      });
      SparseVoxelOccupancyDDAResultGPU empty{};
      renderGraph->bufferWrite(
          occupancyShadowResults_, 0u, sizeof(empty), &empty);
      ownsOccupancyShadowResults_ = true;
    }
    if (!stableSceneHits_.isValid())
    {
      stableSceneHits_ = renderGraph->createBuffer({
        .name = passName + "_EmptyStableSceneHits",
        .size = sceneRayCount * sizeof(SparseVoxelSceneRayHitGPU),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
            ::rendering::BufferUsage::BufferUsage_Push,
      });
      std::vector<SparseVoxelSceneRayHitGPU> empty(sceneRayCount);
      renderGraph->bufferWrite(stableSceneHits_, 0u, empty.size() * sizeof(empty[0]), empty.data());
      ownsStableSceneHits_ = true;
    }
    if (!stableSceneVisibility_.isValid())
    {
      stableSceneVisibility_ = renderGraph->createBuffer({
        .name = passName + "_EmptyStableSceneVisibility",
        .size = sizeof(float),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
            ::rendering::BufferUsage::BufferUsage_Push,
      });
      float visible = 1.0f;
      renderGraph->bufferWrite(
          stableSceneVisibility_, 0u, sizeof(visible), &visible);
      ownsStableSceneVisibility_ = true;
    }
    if (!sceneRays_.isValid())
    {
      sceneRays_ = renderGraph->createBuffer({.name = passName + "_EmptySceneRays", .size = sceneRayCount * sizeof(SparseVoxelSceneRayGPU), .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push});
      std::vector<SparseVoxelSceneRayGPU> empty(sceneRayCount);
      renderGraph->bufferWrite(sceneRays_, 0u, sceneRayCount * sizeof(SparseVoxelSceneRayGPU), empty.data());
      ownsSceneRays_ = true;
    }
    if (!sceneLights_.isValid())
    {
      sceneLights_ = renderGraph->createBuffer({.name = passName + "_EmptySceneLights", .size = globalillumination::worldgi::worldGILightBufferSize(5u), .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push});
      std::vector<globalillumination::worldgi::WorldGILightGPU> empty(5u);
      renderGraph->bufferWrite(sceneLights_, 0u, empty.size() * sizeof(empty[0]), empty.data());
      ownsSceneLights_ = true;
    }
    if (!sceneDIReservoirs_.isValid())
    {
      // Probe lighting has no ReSTIR reservoir array. The debug shader bounds
      // checks this binding before every access, so one zero element satisfies
      // the static layout without recreating a full per-pixel allocation.
      sceneDIReservoirs_ = renderGraph->createBuffer({.name = passName + "_EmptySceneDIReservoirs", .size = sizeof(SparseVoxelDIReservoirGPU), .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push});
      SparseVoxelDIReservoirGPU empty{};
      renderGraph->bufferWrite(sceneDIReservoirs_, 0u, sizeof(empty), &empty);
      ownsSceneDIReservoirs_ = true;
    }
    if (!screenSceneHits_.isValid())
    {
      // Screen tracing is disabled in probe mode. Keep arrayLength non-zero so
      // the shader's clamped diagnostic reads remain valid without allocating
      // one hit record for every canonical probe ray.
      screenSceneHits_ = renderGraph->createBuffer({.name = passName + "_EmptyScreenSceneHits", .size = sizeof(SparseVoxelSceneRayHitGPU), .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push});
      SparseVoxelSceneRayHitGPU empty{};
      renderGraph->bufferWrite(screenSceneHits_, 0u, sizeof(empty), &empty);
      ownsScreenSceneHits_ = true;
    }
    const auto makeEmptySceneHits = [&](::rendering::Buffer &buffer, bool &owned, const std::string &name)
    {
      if (buffer.isValid()) return;
      buffer = renderGraph->createBuffer({.name = passName + name, .size = sceneRayCount * sizeof(SparseVoxelSceneRayHitGPU), .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push});
      std::vector<SparseVoxelSceneRayHitGPU> empty(sceneRayCount);
      renderGraph->bufferWrite(buffer, 0u, empty.size() * sizeof(empty[0]), empty.data());
      owned = true;
    };
    makeEmptySceneHits(mergedSceneHits_, ownsMergedSceneHits_, "_EmptyMergedSceneHits");
    makeEmptySceneHits(skyMergedHits_, ownsSkyMergedHits_, "_EmptySkyMergedHits");
    if (!skyRays_.isValid())
    {
      skyRays_ = renderGraph->createBuffer({.name = passName + "_EmptySkyRays", .size = sceneRayCount * sizeof(SparseVoxelSceneRayGPU), .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push});
      std::vector<SparseVoxelSceneRayGPU> empty(sceneRayCount);
      renderGraph->bufferWrite(skyRays_, 0u, sceneRayCount * sizeof(SparseVoxelSceneRayGPU), empty.data());
      ownsSkyRays_ = true;
    }
    if (!skyStableVisibility_.isValid())
    {
      skyStableVisibility_ = renderGraph->createBuffer({
        .name = passName + "_EmptySkyStableVisibility",
        .size = sceneRayCount * sizeof(float),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
            ::rendering::BufferUsage::BufferUsage_Push,
      });
      std::vector<float> visible(sceneRayCount, 1.0f);
      renderGraph->bufferWrite(skyStableVisibility_, 0u, visible.size() * sizeof(visible[0]), visible.data());
      ownsSkyStableVisibility_ = true;
    }
    if (!sceneRayStatistics_.isValid())
    {
      sceneRayStatistics_ = renderGraph->createBuffer({.name = passName + "_SceneRayStatistics", .size = sizeof(SparseVoxelSceneRayStatisticsGPU), .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push});
      ownsSceneRayStatistics_ = true;
    }
    // A one-word stub keeps the binding satisfied in every mode but the
    // overdraw view, whose graph build supplies the real per-pixel buffer.
    if (brickOverdraw_.isValid())
      brickOverdrawBytes_ = pixelCount * sizeof(uint32_t);
    if (!brickOverdraw_.isValid())
    {
      brickOverdraw_ = renderGraph->createBuffer({
        .name = passName + "_EmptyBrickOverdraw",
        .size = sizeof(uint32_t),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
            ::rendering::BufferUsage::BufferUsage_Push});
      ownsBrickOverdraw_ = true;
    }
    if (microPayloads_.isValid())
      microPayloadBytes_ = pixelCount * sizeof(uint32_t);
    if (!microPayloads_.isValid())
    {
      microPayloads_ = renderGraph->createBuffer({
        .name = passName + "_EmptyMicroPayloads",
        .size = sizeof(uint32_t),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
            ::rendering::BufferUsage::BufferUsage_Push});
      uint32_t empty = 0u;
      renderGraph->bufferWrite(microPayloads_, 0u, sizeof(empty), &empty);
      ownsMicroPayloads_ = true;
    }
    if (!terrainOwnershipIds_.isValid())
    {
      terrainOwnershipIds_ = renderGraph->createBuffer({
        .name = passName + "_EmptyTerrainOwnershipIds",
        .size = sizeof(uint32_t),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
            ::rendering::BufferUsage::BufferUsage_Push});
      uint32_t empty = 0u;
      renderGraph->bufferWrite(
          terrainOwnershipIds_, 0u, sizeof(empty), &empty);
      ownsTerrainOwnershipIds_ = true;
    }
    if (!terrainOwnershipTable_.isValid())
    {
      terrainOwnershipTable_ = renderGraph->createBuffer({
        .name = passName + "_EmptyTerrainOwnershipTable",
        .size = sizeof(SparseVoxelTerrainOwnershipGPU),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
            ::rendering::BufferUsage::BufferUsage_Push});
      SparseVoxelTerrainOwnershipGPU empty{};
      empty.physicalSlot = kInvalidSparseVoxelIndex;
      renderGraph->bufferWrite(
          terrainOwnershipTable_, 0u, sizeof(empty), &empty);
      ownsTerrainOwnershipTable_ = true;
    }
    if (!reactiveOutput_.isValid())
    {
      reactiveOutput_ = renderGraph->createTexture({
        .name = passName + "_EmptyReactiveOutput",
        .format = ::rendering::Format::Format_R16Float,
        .memoryProperties = ::rendering::BufferUsage::BufferUsage_Storage,
        .usage = ::rendering::ImageUsage::ImageUsage_Storage |
            ::rendering::ImageUsage::ImageUsage_Sampled,
        .width = uniforms_.width,
        .height = uniforms_.height,
        .depth = 1u,
        .mipLevels = 1u,
        .frameLocal = true,
      });
      ownsReactiveOutput_ = true;
    }
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u, .bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(uniforms_), .access = ::rendering::AccessPattern::UNIFORM_READ}},
             {.binding = 29u, .bufferView = {.buffer = brickOverdraw_, .offset = 0u, .size = brickOverdrawBytes_, .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 30u, .bufferView = {.buffer = microPayloads_, .offset = 0u, .size = microPayloadBytes_, .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 31u, .bufferView = {.buffer = terrainOwnershipIds_, .offset = 0u, .size = ownsTerrainOwnershipIds_ ? sizeof(uint32_t) : pixelCount * sizeof(uint32_t), .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 32u, .bufferView = {.buffer = terrainOwnershipTable_, .offset = 0u, .size = renderGraph->getBufferSize(terrainOwnershipTable_), .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 12u, .bufferView = {.buffer = coverage_, .offset = 0u, .size = pixelCount * sizeof(uint32_t), .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 13u, .bufferView = {.buffer = linearDepth_, .offset = 0u, .size = pixelCount * sizeof(float), .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 15u, .bufferView = {.buffer = occupancyShadowResults_, .offset = 0u, .size = shadowPixelCount * sizeof(SparseVoxelOccupancyDDAResultGPU), .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 16u, .bufferView = {.buffer = stableSceneHits_, .offset = 0u, .size = sceneRayCount * sizeof(SparseVoxelSceneRayHitGPU), .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 17u, .bufferView = {.buffer = sceneRays_, .offset = 0u, .size = sceneRayCount * sizeof(SparseVoxelSceneRayGPU), .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 18u, .bufferView = {.buffer = sceneLights_, .offset = 0u, .size = globalillumination::worldgi::worldGILightBufferSize(5u), .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 19u, .bufferView = {.buffer = sceneDIReservoirs_, .offset = 0u, .size = (ownsSceneDIReservoirs_ ? 1u : pixelCount) * sizeof(SparseVoxelDIReservoirGPU), .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 20u, .bufferView = {.buffer = screenSceneHits_, .offset = 0u, .size = (ownsScreenSceneHits_ ? 1u : sceneRayCount) * sizeof(SparseVoxelSceneRayHitGPU), .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 21u, .bufferView = {.buffer = mergedSceneHits_, .offset = 0u, .size = sceneRayCount * sizeof(SparseVoxelSceneRayHitGPU), .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 22u, .bufferView = {.buffer = sceneRayStatistics_, .offset = 0u, .size = sizeof(SparseVoxelSceneRayStatisticsGPU), .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE}},
             {.binding = 23u, .bufferView = {.buffer = skyRays_, .offset = 0u, .size = sceneRayCount * sizeof(SparseVoxelSceneRayGPU), .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 24u, .bufferView = {.buffer = skyMergedHits_, .offset = 0u, .size = sceneRayCount * sizeof(SparseVoxelSceneRayHitGPU), .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 25u, .bufferView = {.buffer = skyStableVisibility_, .offset = 0u, .size = sceneRayCount * sizeof(float), .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 26u, .bufferView = {.buffer = stableSceneVisibility_, .offset = 0u, .size = (ownsStableSceneVisibility_ ? 1u : sceneRayCount) * sizeof(float), .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 28u, .bufferView = {.buffer = rasterBuffer_, .offset = 0u, .size = sizeof(raster_), .access = ::rendering::AccessPattern::UNIFORM_READ}},
           },
           .textures = std::move(textures),
           .storageTextures = {
             {.textureView = storageView(output_), .binding = 14u},
             {.textureView = storageView(reactiveOutput_), .binding = 27u},
           },
         }}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader", .layout = layout_, .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-present.spirv"), .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline(
        {.entry = "debugPresent", .layout = layout_, .name = passName + "_Pipeline", .shader = shader_});
  }

  SparseVoxelGBufferPass::FrameTarget target_{};
  ::rendering::Buffer coverage_{};
  ::rendering::Buffer linearDepth_{};
  ::rendering::Buffer brickOverdraw_{};
  bool ownsBrickOverdraw_ = false;
  uint64_t brickOverdrawBytes_ = sizeof(uint32_t);
  ::rendering::Buffer microPayloads_{};
  bool ownsMicroPayloads_ = false;
  uint64_t microPayloadBytes_ = sizeof(uint32_t);
  ::rendering::Buffer terrainOwnershipIds_{};
  ::rendering::Buffer terrainOwnershipTable_{};
  ::rendering::Buffer occupancyShadowResults_{};
  ::rendering::Buffer stableSceneHits_{};
  ::rendering::Buffer stableSceneVisibility_{};
  ::rendering::Buffer sceneRays_{};
  ::rendering::Buffer sceneLights_{};
  ::rendering::Buffer sceneDIReservoirs_{};
  ::rendering::Buffer screenSceneHits_{};
  ::rendering::Buffer mergedSceneHits_{};
  ::rendering::Buffer sceneRayStatistics_{};
  ::rendering::Buffer skyRays_{};
  ::rendering::Buffer skyMergedHits_{};
  ::rendering::Buffer skyStableVisibility_{};
  ::rendering::Texture output_{};
  ::rendering::Texture reactiveOutput_{};
  SparseVoxelDebugPresentUniformsGPU uniforms_{};
  SparseVoxelRasterUniformsGPU raster_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::Buffer rasterBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
  bool ownsOccupancyShadowResults_ = false;
  bool ownsStableSceneHits_ = false;
  bool ownsStableSceneVisibility_ = false;
  bool ownsSceneRays_ = false;
  bool ownsSceneLights_ = false;
  bool ownsSceneDIReservoirs_ = false;
  bool ownsScreenSceneHits_ = false;
  bool ownsMergedSceneHits_ = false;
  bool ownsSceneRayStatistics_ = false;
  bool ownsSkyRays_ = false;
  bool ownsSkyMergedHits_ = false;
  bool ownsSkyStableVisibility_ = false;
  bool ownsTerrainOwnershipIds_ = false;
  bool ownsTerrainOwnershipTable_ = false;
  bool ownsReactiveOutput_ = false;
};

} // namespace voxel::svraster::rendering
