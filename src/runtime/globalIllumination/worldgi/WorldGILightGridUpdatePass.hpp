#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "math/math.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace globalillumination::worldgi
{

class WorldGILightGridUpdatePass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t gridResolution = kWorldGILightGridDefaultResolution;
    uint32_t cascadeCount = kWorldGILightGridMaxCascades;
    uint32_t maxItemsPerCell = kWorldGILightGridDefaultMaxItemsPerCell;
    uint32_t lightCapacity = kWorldGILightGridMaxLights;
    uint32_t frameIndex = 0u;
    bool enabled = false;
    math::Vec3f gridCenter = math::Vec3f(0.0f);
    float baseExtent = 2000.0f;
    float cascadeScale = 2.0f;
    math::Vec3f directionalLightDirection = math::Vec3f(0.0f, -1.0f, 0.0f);
    math::Vec3f directionalLightColor = math::Vec3f(1.0f);
    float directionalLightIntensity = 1.0f;
    bool directionalLightEnabled = true;
    math::Vec3f pointLightPosition = math::Vec3f(0.0f);
    math::Vec3f pointLightColor = math::Vec3f(1.0f);
    float pointLightRange = 16.0f;
    float pointLightIntensity = 0.0f;
    bool pointLightEnabled = false;
  };

  WorldGILightGridUpdatePass(
      rendering::Buffer lightBuffer,
      rendering::Buffer gridCellBuffer,
      rendering::Buffer gridItemIdBuffer,
      Settings settings)
      : lightBuffer_(lightBuffer),
        gridCellBuffer_(gridCellBuffer),
        gridItemIdBuffer_(gridItemIdBuffer),
        settings_(sanitizeSettings(settings))
  {
  }

  ~WorldGILightGridUpdatePass() override
  {
    destroyFrameOverrideBuffers();
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (bindingGroups_.isValid())
      renderGraph->deleteBindingGroups(bindingGroups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
  }

  void setSettings(Settings settings)
  {
    settings_ = sanitizeSettings(settings);
    if (uniformBuffer_.isValid())
    {
      writeUniforms();
      writeLights();
    }
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    if (!frameOverrideUniformBufferIds_.empty())
      overrides.bufferOverrides.emplace(uniformBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = getFrameOverrideUniformBufferId(frameSlot)});
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    writeUniforms();
    writeLights();

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    const uint32_t cellCount = worldGILightGridCellCount(settings_.gridResolution, settings_.cascadeCount);
    commandBuffer.cmdDispatch((cellCount + 63u) / 64u, 1u, 1u);
  }

private:
  struct alignas(16) Uniforms
  {
    uint32_t gridResolution[3]{kWorldGILightGridDefaultResolution, kWorldGILightGridDefaultResolution, kWorldGILightGridDefaultResolution};
    uint32_t cascadeCount = kWorldGILightGridMaxCascades;
    uint32_t maxItemsPerCell = kWorldGILightGridDefaultMaxItemsPerCell;
    uint32_t lightCount = 0u;
    uint32_t enabled = 0u;
    uint32_t frameIndex = 0u;
    float gridCenterBaseExtent[4]{0.0f, 0.0f, 0.0f, 2000.0f};
    float cascadeScalePadding[4]{2.0f, 0.0f, 0.0f, 0.0f};
  };
  static_assert(sizeof(Uniforms) == 64u, "WorldGILightGridUpdatePass uniforms must match WGSL layout");

  static Settings sanitizeSettings(Settings settings)
  {
    settings.gridResolution = std::clamp(settings.gridResolution, 1u, 64u);
    settings.cascadeCount = std::clamp(settings.cascadeCount, 1u, kWorldGILightGridMaxCascades);
    settings.maxItemsPerCell = std::clamp(settings.maxItemsPerCell, 1u, 256u);
    settings.lightCapacity = std::clamp(settings.lightCapacity, 1u, kWorldGILightGridMaxLights);
    settings.baseExtent = std::isfinite(settings.baseExtent) ? std::max(settings.baseExtent, 0.001f) : 2000.0f;
    settings.cascadeScale = std::isfinite(settings.cascadeScale) ? std::max(settings.cascadeScale, 1.0f) : 2.0f;
    settings.directionalLightIntensity = std::max(0.0f, settings.directionalLightIntensity);
    settings.pointLightRange = std::max(0.0f, settings.pointLightRange);
    settings.pointLightIntensity = std::max(0.0f, settings.pointLightIntensity);
    return settings;
  }

  static rendering::BufferView makeBufferView(rendering::Buffer buffer, uint64_t size, rendering::AccessPattern access)
  {
    return rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
  }

  rendering::Buffer createFrameLocalBuffer(rendering::BufferInfo info) const
  {
    if (renderGraph != nullptr && renderGraph->getMaxFramesInFlight() > 1u)
      info.frameLocal = true;
    return renderGraph->createBuffer(info);
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;

    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(Uniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    initializeFrameOverrideBuffers();

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "lightGridUniforms", .binding = 0u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "worldGILights", .binding = 1u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "worldGILightGridCells", .binding = 2u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "worldGILightGridItemIds", .binding = 3u, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_Groups",
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers = {
                    {.binding = 0u, .bufferView = makeBufferView(uniformBuffer_, sizeof(Uniforms), rendering::AccessPattern::SHADER_READ)},
                    {.binding = 1u, .bufferView = makeBufferView(lightBuffer_, worldGILightBufferSize(settings_.lightCapacity), rendering::AccessPattern::SHADER_READ)},
                    {.binding = 2u, .bufferView = makeBufferView(gridCellBuffer_, worldGILightGridCellBufferSize(settings_.gridResolution, settings_.cascadeCount), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                    {.binding = 3u, .bufferView = makeBufferView(gridItemIdBuffer_, worldGILightGridItemBufferSize(settings_.gridResolution, settings_.cascadeCount, settings_.maxItemsPerCell), rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/globalIllumination/worldgi/worldgi-light-grid-update-cs.spirv"),
            .type = rendering::ShaderType::SpirV,
        });

    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "cs_main",
            .layout = layout_,
            .name = passName + "_Pipeline",
            .shader = shader_,
        });
  }

  void initializeFrameOverrideBuffers()
  {
    const uint32_t frameCount = std::max(1u, renderGraph->getMaxFramesInFlight());
    frameOverrideUniformBufferIds_.assign(frameCount, rendering::BufferId::Invalid);
    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
    {
      frameOverrideUniformBufferIds_[frameSlot] = renderGraph->getRHI()->createBuffer(
          rendering::BufferInfo{
              .name = passName + "_Uniforms.override_frame" + std::to_string(frameSlot),
              .size = sizeof(Uniforms),
              .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
          });
    }
  }

  void destroyFrameOverrideBuffers()
  {
    if (renderGraph == nullptr)
      return;
    for (const rendering::BufferId bufferId : frameOverrideUniformBufferIds_)
    {
      if (bufferId != rendering::BufferId::Invalid)
        renderGraph->getRHI()->deleteBuffer(bufferId);
    }
    frameOverrideUniformBufferIds_.clear();
  }

  rendering::BufferId getFrameOverrideUniformBufferId(uint32_t frameSlot) const
  {
    if (frameOverrideUniformBufferIds_.empty())
      return rendering::BufferId::Invalid;
    return frameOverrideUniformBufferIds_[frameSlot % frameOverrideUniformBufferIds_.size()];
  }

  uint32_t buildLightList(std::vector<WorldGILightGPU> &lights) const
  {
    lights.assign(settings_.lightCapacity, WorldGILightGPU{});
    uint32_t lightCount = 0u;

    auto pushLight = [&](const WorldGILightGPU &light)
    {
      if (lightCount < settings_.lightCapacity)
        lights[lightCount++] = light;
    };

    WorldGILightGPU directional{};
    directional.directionType[0] = settings_.directionalLightDirection.x();
    directional.directionType[1] = settings_.directionalLightDirection.y();
    directional.directionType[2] = settings_.directionalLightDirection.z();
    directional.colorIntensity[0] = settings_.directionalLightColor.x();
    directional.colorIntensity[1] = settings_.directionalLightColor.y();
    directional.colorIntensity[2] = settings_.directionalLightColor.z();
    directional.colorIntensity[3] = settings_.directionalLightEnabled ? settings_.directionalLightIntensity : 0.0f;
    directional.flags[0] = settings_.directionalLightEnabled ? WorldGILightFlag_Enabled : WorldGILightFlag_None;
    directional.flags[1] = WorldGILightType_Directional;
    pushLight(directional);

    WorldGILightGPU point{};
    point.positionRange[0] = settings_.pointLightPosition.x();
    point.positionRange[1] = settings_.pointLightPosition.y();
    point.positionRange[2] = settings_.pointLightPosition.z();
    point.positionRange[3] = settings_.pointLightRange;
    point.colorIntensity[0] = settings_.pointLightColor.x();
    point.colorIntensity[1] = settings_.pointLightColor.y();
    point.colorIntensity[2] = settings_.pointLightColor.z();
    point.colorIntensity[3] = settings_.pointLightEnabled ? settings_.pointLightIntensity : 0.0f;
    point.flags[0] = settings_.pointLightEnabled ? WorldGILightFlag_Enabled : WorldGILightFlag_None;
    point.flags[1] = WorldGILightType_Point;
    pushLight(point);

    return lightCount;
  }

  void writeUniforms()
  {
    std::vector<WorldGILightGPU> lights;
    const uint32_t lightCount = buildLightList(lights);

    Uniforms uniforms{};
    uniforms.gridResolution[0] = settings_.gridResolution;
    uniforms.gridResolution[1] = settings_.gridResolution;
    uniforms.gridResolution[2] = settings_.gridResolution;
    uniforms.cascadeCount = settings_.cascadeCount;
    uniforms.maxItemsPerCell = settings_.maxItemsPerCell;
    uniforms.lightCount = lightCount;
    uniforms.enabled = settings_.enabled ? 1u : 0u;
    uniforms.frameIndex = settings_.frameIndex;
    uniforms.gridCenterBaseExtent[0] = settings_.gridCenter.x();
    uniforms.gridCenterBaseExtent[1] = settings_.gridCenter.y();
    uniforms.gridCenterBaseExtent[2] = settings_.gridCenter.z();
    uniforms.gridCenterBaseExtent[3] = settings_.baseExtent;
    uniforms.cascadeScalePadding[0] = settings_.cascadeScale;
    if (frameOverrideUniformBufferIds_.empty())
    {
      renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
    }
    else
    {
      for (const rendering::BufferId bufferId : frameOverrideUniformBufferIds_)
      {
        if (bufferId != rendering::BufferId::Invalid)
          renderGraph->getRHI()->bufferWrite(bufferId, 0u, sizeof(uniforms), &uniforms);
      }
    }
  }

  void writeLights()
  {
    std::vector<WorldGILightGPU> lights;
    buildLightList(lights);
    if (!lights.empty())
      renderGraph->bufferWrite(lightBuffer_, 0u, static_cast<uint64_t>(lights.size()) * sizeof(WorldGILightGPU), lights.data());
  }

  rendering::Buffer lightBuffer_{};
  rendering::Buffer gridCellBuffer_{};
  rendering::Buffer gridItemIdBuffer_{};
  Settings settings_{};

  rendering::BindingsLayout layout_{};
  rendering::BindingGroups bindingGroups_{};
  rendering::Shader shader_{};
  rendering::ComputePipeline pipeline_{};
  rendering::Buffer uniformBuffer_{};
  std::vector<rendering::BufferId> frameOverrideUniformBufferIds_;
};

} // namespace globalillumination::worldgi
