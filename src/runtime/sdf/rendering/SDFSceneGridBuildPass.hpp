#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "sdf/SDFSceneManager.hpp"

namespace sdf::rendering
{

class SDFSceneGridBuildPass : public ::rendering::Pass
{
public:
  enum class Stage : uint32_t
  {
    Clear,
    Emit,
  };

  explicit SDFSceneGridBuildPass(SDFSceneManager &sceneManager, Stage stage = Stage::Emit)
      : sceneManager_(sceneManager), stage_(stage)
  {
  }

  ~SDFSceneGridBuildPass() override
  {
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

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    sceneManager_.setRenderGraph(renderGraph);
    if (!sceneManager_.gpuResources().isValid())
      sceneManager_.uploadGPUResources();
    if (!sceneManager_.gpuResources().isValid())
      throw std::runtime_error("SDFSceneGridBuildPass requires uploaded SDFSceneManager GPU resources");

    const SDFSceneManager::Settings &settings = sceneManager_.settings();
    if (!settings.enableSceneGrid || sceneManager_.instanceCount() == 0u)
      return;

    ensureResources();

    const Uniforms uniforms = makeUniforms();
    writeUniforms(uniforms);

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    const uint32_t itemCount = stage_ == Stage::Clear ? uniforms.cellCount : uniforms.maxRefJobs;
    commandBuffer.cmdDispatch((std::max(itemCount, 1u) + kWorkgroupSize - 1u) / kWorkgroupSize, 1u, 1u);
  }

private:
  static constexpr uint32_t kWorkgroupSize = 64u;
  static constexpr uint32_t kTraceRootCellCount = 512u;

  struct Uniforms
  {
    uint32_t instanceCount = 0u;
    uint32_t cellCount = 0u;
    uint32_t maxRefJobs = 0u;
    uint32_t pageRefEnabled = 0u;
    uint32_t hugeInstanceCellThreshold = 0u;
    uint32_t maxCellsPerRef = 0u;
    uint32_t _padding0 = 0u;
    uint32_t _padding1 = 0u;
  };

  static ::rendering::BufferView makeBufferView(::rendering::Buffer buffer, uint64_t size, ::rendering::AccessPattern access)
  {
    return ::rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
  }

  static uint32_t gridResolution(const SDFSceneManager::Settings &settings)
  {
    return std::max(1u, std::min(settings.sceneGridResolution, 128u));
  }

  Uniforms makeUniforms() const
  {
    const SDFSceneManager::Settings &settings = sceneManager_.settings();
    const uint32_t resolution = gridResolution(settings);
    const uint32_t instanceCount = sceneManager_.instanceCount();
    Uniforms uniforms{};
    uniforms.instanceCount = instanceCount;
    uniforms.cellCount = resolution * resolution * resolution;
    uniforms.maxRefJobs = instanceCount * kTraceRootCellCount;
    uniforms.pageRefEnabled = settings.enableSceneGridPageRefs ? 1u : 0u;
    uniforms.hugeInstanceCellThreshold = settings.sceneGridHugeInstanceCellThreshold;
    uniforms.maxCellsPerRef = settings.sceneGridMaxCellsPerRef;
    return uniforms;
  }

  void ensureResources()
  {
    if (!uniformBuffer_.isValid())
    {
      uniformBuffer_ = renderGraph->createBuffer(
          ::rendering::BufferInfo{
              .name = passName + "_Uniforms.buffer",
              .size = sizeof(Uniforms),
              .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
          });
    }

    if (!layout_.isValid())
    {
      layout_ = renderGraph->createBindingsLayout(
          ::rendering::BindingsLayoutInfo{
              .name = passName + "_Layout",
              .groups = {::rendering::BindingGroupLayout{
                  .buffers =
                      {
                          {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "sceneGridInfo", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "sceneGridCells", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "sceneGridRefs", .binding = 3u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "sceneGridGlobalRefs", .binding = 4u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "instances", .binding = 5u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "blases", .binding = 6u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "tracePageTables", .binding = 7u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                      },
              }},
          });
    }

    const SDFGPUResources &resources = sceneManager_.gpuResources();
    const uint64_t resourceRevision = sceneManager_.gpuResourceRevision();
    if (!bindingGroups_.isValid() || boundGpuResourceRevision_ != resourceRevision)
    {
      if (bindingGroups_.isValid())
        renderGraph->deleteBindingGroups(bindingGroups_);
      bindingGroups_ = renderGraph->createBindingGroups(
          ::rendering::BindingGroupsInfo{
              .layout = layout_,
              .name = passName + "_BindingGroups",
              .groups = {::rendering::GroupInfo{
                  .name = "group0",
                  .buffers =
                      {
                          {.binding = 0u, .bufferView = makeBufferView(uniformBuffer_, sizeof(Uniforms), ::rendering::AccessPattern::SHADER_READ)},
                          {.binding = 1u, .bufferView = makeBufferView(resources.sceneGridInfo, renderGraph->getBufferSize(resources.sceneGridInfo), ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE)},
                          {.binding = 2u, .bufferView = makeBufferView(resources.sceneGridCells, renderGraph->getBufferSize(resources.sceneGridCells), ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE)},
                          {.binding = 3u, .bufferView = makeBufferView(resources.sceneGridRefs, renderGraph->getBufferSize(resources.sceneGridRefs), ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE)},
                          {.binding = 4u, .bufferView = makeBufferView(resources.sceneGridGlobalRefs, renderGraph->getBufferSize(resources.sceneGridGlobalRefs), ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE)},
                          {.binding = 5u, .bufferView = makeBufferView(resources.instances, renderGraph->getBufferSize(resources.instances), ::rendering::AccessPattern::SHADER_READ)},
                          {.binding = 6u, .bufferView = makeBufferView(resources.blases, renderGraph->getBufferSize(resources.blases), ::rendering::AccessPattern::SHADER_READ)},
                          {.binding = 7u, .bufferView = makeBufferView(resources.tracePageTables, renderGraph->getBufferSize(resources.tracePageTables), ::rendering::AccessPattern::SHADER_READ)},
                      },
              }},
          });
      boundGpuResourceRevision_ = resourceRevision;
    }

    if (!shader_.isValid())
    {
      shader_ = renderGraph->createShader(
          ::rendering::ShaderInfo{
              .name = passName + "_Shader",
              .layout = layout_,
              .src = os::io::readRelativeFile("assets/shaders/spirv/sdf-scene-grid-build.spirv"),
              .type = ::rendering::ShaderType::SpirV,
          });
    }
    if (!pipeline_.isValid())
    {
      pipeline_ = renderGraph->createComputePipeline(
          ::rendering::ComputePipelineInfo{
              .entry = stage_ == Stage::Clear ? "clear_scene_grid" : "emit_scene_grid_refs",
              .layout = layout_,
              .name = passName + "_Pipeline",
              .shader = shader_,
          });
    }
  }

  void writeUniforms(const Uniforms &uniforms)
  {
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(Uniforms), const_cast<Uniforms *>(&uniforms));
  }

  SDFSceneManager &sceneManager_;
  Stage stage_ = Stage::Emit;
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups bindingGroups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
  uint64_t boundGpuResourceRevision_ = UINT64_MAX;
};

class SDFSceneGridClearPass : public SDFSceneGridBuildPass
{
public:
  explicit SDFSceneGridClearPass(SDFSceneManager &sceneManager)
      : SDFSceneGridBuildPass(sceneManager, Stage::Clear)
  {
  }
};

} // namespace sdf::rendering
