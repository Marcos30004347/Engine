#pragma once

#include <algorithm>
#include <cstdint>

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "softwarebvh/SoftwareRayTraceScene.hpp"

namespace softwarebvh
{
namespace rendering
{

class SoftwareWorldBLASUpdatePass : public ::rendering::Pass
{
public:
  enum class Stage : uint32_t
  {
    ClearCounters,
    PrepareUpdateJobs,
    PrepareUpdateDispatches,
    TransformTriangles,
    RefitWorldBVH8,
    CopyLeafRemap,
    CompressBVH8ToCWBVH,
  };

  struct Buffers
  {
    ::rendering::Buffer instanceRanges;
    ::rendering::Buffer updateInstanceRangeIndices;
    ::rendering::Buffer updateCounters;
    ::rendering::Buffer dispatchArgs;
    ::rendering::Buffer triangleUpdateItems;
    ::rendering::Buffer bvh8UpdateItems;
    ::rendering::Buffer leafRemapUpdateItems;
    ::rendering::Buffer instanceTransforms;
    ::rendering::Buffer sourceTriangles;
    ::rendering::Buffer worldTriangles;
    ::rendering::Buffer sourceBVH8Nodes;
    ::rendering::Buffer worldBVH8Nodes;
    ::rendering::Buffer sourceLeafRemap;
    ::rendering::Buffer worldLeafRemap;
    ::rendering::Buffer sceneCWBVHNodes;
    ::rendering::Buffer sourcePrimitiveRemap;
  };

  struct Settings
  {
    uint32_t instanceRangeCount = 0u;
    uint32_t triangleUpdateItemCount = 0u;
    uint32_t bvh8UpdateItemCount = 0u;
    uint32_t leafRemapUpdateItemCount = 0u;
    uint32_t maxInstanceRangeCount = 1u;
    uint32_t maxUpdateInstanceCount = 1u;
    uint32_t maxTriangleUpdateItemCount = 1u;
    uint32_t maxBVH8UpdateItemCount = 1u;
    uint32_t maxLeafRemapUpdateItemCount = 1u;
    uint32_t maxInstanceTransformCount = 1u;
    uint32_t maxSourceTriangleCount = 1u;
    uint32_t maxWorldTriangleCount = 1u;
    uint32_t maxSourceBVH8NodeCount = 1u;
    uint32_t maxWorldBVH8NodeCount = 1u;
    uint32_t maxSourceLeafRemapCount = 1u;
    uint32_t maxWorldLeafRemapCount = 1u;
    uint32_t maxSceneCWBVHNodeCount = 1u;
    uint32_t workgroupSize = 64u;
    bool useIndirectDispatch = false;
    bool enabled = true;
  };

  SoftwareWorldBLASUpdatePass(Stage stage, Buffers buffers, Settings settings)
      : stage_(stage), buffers_(buffers), settings_(sanitizeSettings(settings))
  {
  }

  void setSettings(Settings settings)
  {
    settings_ = sanitizeSettings(settings);
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  const Settings &getSettings() const
  {
    return settings_;
  }

  ~SoftwareWorldBLASUpdatePass() override
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
    if (!hasRequiredBuffers())
    {
      return;
    }

    uniformBuffer_ = renderGraph->createBuffer(
        ::rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(Uniforms),
            .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
        });
    writeUniforms();

    layout_ = renderGraph->createBindingsLayout(
        ::rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {::rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "uniforms", .binding = 0u, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "instanceRanges", .binding = 1u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "updateInstanceRangeIndices", .binding = 2u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "updateCounters", .binding = 3u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "dispatchArgs", .binding = 4u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "triangleUpdateItems", .binding = 5u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "bvh8UpdateItems", .binding = 6u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "leafRemapUpdateItems", .binding = 7u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "instanceTransforms", .binding = 8u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "sourceTriangles", .binding = 9u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "worldTriangles", .binding = 10u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "sourceBVH8Nodes", .binding = 11u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "worldBVH8Nodes", .binding = 12u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "sourceLeafRemap", .binding = 13u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "worldLeafRemap", .binding = 14u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "sceneCWBVHNodes", .binding = 15u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "sourcePrimitiveRemap", .binding = 16u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    bindingGroups_ = renderGraph->createBindingGroups(
        ::rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_Groups",
            .groups = {::rendering::GroupInfo{
                .name = "group0",
                .buffers = {
                    makeBufferBinding(0u, uniformBuffer_, ::rendering::AccessPattern::SHADER_READ, sizeof(Uniforms)),
                    makeBufferBinding(1u, buffers_.instanceRanges, ::rendering::AccessPattern::SHADER_READ, instanceRangesSize()),
                    makeBufferBinding(2u, buffers_.updateInstanceRangeIndices, readWrite(), updateInstanceRangeIndicesSize()),
                    makeBufferBinding(3u, buffers_.updateCounters, readWrite(), sizeof(softwarebvh::SoftwareRayTraceScene::UpdateCountersGPU)),
                    makeBufferBinding(4u, buffers_.dispatchArgs, readWrite(), dispatchArgsSize()),
                    makeBufferBinding(5u, buffers_.triangleUpdateItems, ::rendering::AccessPattern::SHADER_READ, triangleUpdateItemsSize()),
                    makeBufferBinding(6u, buffers_.bvh8UpdateItems, ::rendering::AccessPattern::SHADER_READ, bvh8UpdateItemsSize()),
                    makeBufferBinding(7u, buffers_.leafRemapUpdateItems, ::rendering::AccessPattern::SHADER_READ, leafRemapUpdateItemsSize()),
                    makeBufferBinding(8u, buffers_.instanceTransforms, ::rendering::AccessPattern::SHADER_READ, instanceTransformsSize()),
                    makeBufferBinding(9u, buffers_.sourceTriangles, ::rendering::AccessPattern::SHADER_READ, sourceTrianglesSize()),
                    makeBufferBinding(10u, buffers_.worldTriangles, readWrite(), worldTrianglesSize()),
                    makeBufferBinding(11u, buffers_.sourceBVH8Nodes, ::rendering::AccessPattern::SHADER_READ, sourceBVH8NodesSize()),
                    makeBufferBinding(12u, buffers_.worldBVH8Nodes, readWrite(), worldBVH8NodesSize()),
                    makeBufferBinding(13u, buffers_.sourceLeafRemap, ::rendering::AccessPattern::SHADER_READ, sourceLeafRemapSize()),
                    makeBufferBinding(14u, buffers_.worldLeafRemap, readWrite(), worldLeafRemapSize()),
                    makeBufferBinding(15u, buffers_.sceneCWBVHNodes, readWrite(), sceneCWBVHNodesSize()),
                    makeBufferBinding(16u, buffers_.sourcePrimitiveRemap, ::rendering::AccessPattern::SHADER_READ, sourcePrimitiveRemapSize()),
                },
            }},
        });

    shader_ = renderGraph->createShader(
        ::rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/software-world-blas-update.spirv"),
            .type = ::rendering::ShaderType::SpirV,
        });
    pipeline_ = renderGraph->createComputePipeline(
        ::rendering::ComputePipelineInfo{
            .entry = entryName(),
            .layout = layout_,
            .name = passName + "_Pipeline",
            .shader = shader_,
        });

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    dispatch(commandBuffer);
  }

private:
  struct Uniforms
  {
    uint32_t instanceRangeCount = 0u;
    uint32_t triangleUpdateItemCount = 0u;
    uint32_t bvh8UpdateItemCount = 0u;
    uint32_t leafRemapUpdateItemCount = 0u;
    uint32_t workgroupSize = 64u;
    uint32_t enabled = 1u;
    uint32_t _padding[2]{};
  };
  static_assert(sizeof(Uniforms) == 32u, "SoftwareWorldBLASUpdatePass::Uniforms must stay tightly packed");

  static Settings sanitizeSettings(Settings settings)
  {
    settings.maxInstanceRangeCount = std::max(settings.maxInstanceRangeCount, 1u);
    settings.maxUpdateInstanceCount = std::max(settings.maxUpdateInstanceCount, 1u);
    settings.maxTriangleUpdateItemCount = std::max(settings.maxTriangleUpdateItemCount, 1u);
    settings.maxBVH8UpdateItemCount = std::max(settings.maxBVH8UpdateItemCount, 1u);
    settings.maxLeafRemapUpdateItemCount = std::max(settings.maxLeafRemapUpdateItemCount, 1u);
    settings.maxInstanceTransformCount = std::max(settings.maxInstanceTransformCount, 1u);
    settings.maxSourceTriangleCount = std::max(settings.maxSourceTriangleCount, 1u);
    settings.maxWorldTriangleCount = std::max(settings.maxWorldTriangleCount, 1u);
    settings.maxSourceBVH8NodeCount = std::max(settings.maxSourceBVH8NodeCount, 1u);
    settings.maxWorldBVH8NodeCount = std::max(settings.maxWorldBVH8NodeCount, 1u);
    settings.maxSourceLeafRemapCount = std::max(settings.maxSourceLeafRemapCount, 1u);
    settings.maxWorldLeafRemapCount = std::max(settings.maxWorldLeafRemapCount, 1u);
    settings.maxSceneCWBVHNodeCount = std::max(settings.maxSceneCWBVHNodeCount, 1u);
    settings.workgroupSize = std::max(settings.workgroupSize, 1u);
    return settings;
  }

  static ::rendering::AccessPattern readWrite()
  {
    return ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE;
  }

  static ::rendering::BindingBuffer makeBufferBinding(uint32_t binding, ::rendering::Buffer buffer, ::rendering::AccessPattern access, uint64_t size)
  {
    return ::rendering::BindingBuffer{
        .bufferView = {.buffer = buffer, .offset = 0u, .size = std::max<uint64_t>(size, 4u), .access = access},
        .binding = binding,
    };
  }

  bool hasRequiredBuffers() const
  {
    return buffers_.instanceRanges.isValid() &&
           buffers_.updateInstanceRangeIndices.isValid() &&
           buffers_.updateCounters.isValid() &&
           buffers_.dispatchArgs.isValid() &&
           buffers_.triangleUpdateItems.isValid() &&
           buffers_.bvh8UpdateItems.isValid() &&
           buffers_.leafRemapUpdateItems.isValid() &&
           buffers_.instanceTransforms.isValid() &&
           buffers_.sourceTriangles.isValid() &&
           buffers_.worldTriangles.isValid() &&
           buffers_.sourceBVH8Nodes.isValid() &&
           buffers_.worldBVH8Nodes.isValid() &&
           buffers_.sourceLeafRemap.isValid() &&
           buffers_.worldLeafRemap.isValid() &&
           buffers_.sceneCWBVHNodes.isValid() &&
           buffers_.sourcePrimitiveRemap.isValid();
  }

  const char *entryName() const
  {
    switch (stage_)
    {
    case Stage::ClearCounters:
      return "clear_update_counters";
    case Stage::PrepareUpdateJobs:
      return "prepare_update_jobs";
    case Stage::PrepareUpdateDispatches:
      return "prepare_update_dispatches";
    case Stage::TransformTriangles:
      return "transform_triangles";
    case Stage::RefitWorldBVH8:
      return "refit_world_bvh8";
    case Stage::CopyLeafRemap:
      return "copy_leaf_remap";
    case Stage::CompressBVH8ToCWBVH:
      return "compress_bvh8_to_cwbvh";
    }
    return "clear_update_counters";
  }

  uint32_t directGroupCount(uint32_t itemCount) const
  {
    return itemCount == 0u ? 0u : (itemCount + settings_.workgroupSize - 1u) / settings_.workgroupSize;
  }

  void dispatch(::rendering::CommandRecorder &commandBuffer) const
  {
    switch (stage_)
    {
    case Stage::ClearCounters:
    case Stage::PrepareUpdateDispatches:
      commandBuffer.cmdDispatch(1u, 1u, 1u);
      return;
    case Stage::PrepareUpdateJobs:
      commandBuffer.cmdDispatch(directGroupCount(settings_.instanceRangeCount), 1u, 1u);
      return;
    case Stage::TransformTriangles:
      if (settings_.useIndirectDispatch)
        commandBuffer.cmdDispatchIndirect(buffers_.dispatchArgs, 0u);
      else
        commandBuffer.cmdDispatch(directGroupCount(settings_.triangleUpdateItemCount), 1u, 1u);
      return;
    case Stage::RefitWorldBVH8:
      if (settings_.useIndirectDispatch)
        commandBuffer.cmdDispatchIndirect(buffers_.dispatchArgs, sizeof(softwarebvh::SoftwareRayTraceScene::DispatchIndirectArgsGPU));
      else
        commandBuffer.cmdDispatch(directGroupCount(settings_.bvh8UpdateItemCount), 1u, 1u);
      return;
    case Stage::CopyLeafRemap:
      if (settings_.useIndirectDispatch)
        commandBuffer.cmdDispatchIndirect(buffers_.dispatchArgs, 2u * sizeof(softwarebvh::SoftwareRayTraceScene::DispatchIndirectArgsGPU));
      else
        commandBuffer.cmdDispatch(directGroupCount(settings_.leafRemapUpdateItemCount), 1u, 1u);
      return;
    case Stage::CompressBVH8ToCWBVH:
      if (settings_.useIndirectDispatch)
        commandBuffer.cmdDispatchIndirect(buffers_.dispatchArgs, 3u * sizeof(softwarebvh::SoftwareRayTraceScene::DispatchIndirectArgsGPU));
      else
        commandBuffer.cmdDispatch(directGroupCount(settings_.bvh8UpdateItemCount), 1u, 1u);
      return;
    }
  }

  void writeUniforms()
  {
    Uniforms uniforms{};
    uniforms.instanceRangeCount = settings_.instanceRangeCount;
    uniforms.triangleUpdateItemCount = settings_.triangleUpdateItemCount;
    uniforms.bvh8UpdateItemCount = settings_.bvh8UpdateItemCount;
    uniforms.leafRemapUpdateItemCount = settings_.leafRemapUpdateItemCount;
    uniforms.workgroupSize = settings_.workgroupSize;
    uniforms.enabled = settings_.enabled ? 1u : 0u;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
  }

  uint64_t instanceRangesSize() const { return static_cast<uint64_t>(settings_.maxInstanceRangeCount) * sizeof(softwarebvh::SoftwareRayTraceScene::WorldInstanceRangeGPU); }
  uint64_t updateInstanceRangeIndicesSize() const { return static_cast<uint64_t>(settings_.maxUpdateInstanceCount) * sizeof(uint32_t); }
  uint64_t dispatchArgsSize() const { return 4u * sizeof(softwarebvh::SoftwareRayTraceScene::DispatchIndirectArgsGPU); }
  uint64_t triangleUpdateItemsSize() const { return static_cast<uint64_t>(settings_.maxTriangleUpdateItemCount) * sizeof(softwarebvh::SoftwareRayTraceScene::TriangleUpdateItemGPU); }
  uint64_t bvh8UpdateItemsSize() const { return static_cast<uint64_t>(settings_.maxBVH8UpdateItemCount) * sizeof(softwarebvh::SoftwareRayTraceScene::BVH8UpdateItemGPU); }
  uint64_t leafRemapUpdateItemsSize() const { return static_cast<uint64_t>(settings_.maxLeafRemapUpdateItemCount) * sizeof(softwarebvh::SoftwareRayTraceScene::LeafRemapUpdateItemGPU); }
  uint64_t instanceTransformsSize() const { return static_cast<uint64_t>(settings_.maxInstanceTransformCount) * sizeof(softwarebvh::SoftwareRayTraceScene::InstanceTransformGPU); }
  uint64_t sourceTrianglesSize() const { return static_cast<uint64_t>(settings_.maxSourceTriangleCount) * sizeof(softwarebvh::SoftwareRayTraceScene::TriBaryTransformGPU); }
  uint64_t worldTrianglesSize() const { return static_cast<uint64_t>(settings_.maxWorldTriangleCount) * sizeof(softwarebvh::SoftwareRayTraceScene::TriBaryTransformGPU); }
  uint64_t sourceBVH8NodesSize() const { return static_cast<uint64_t>(settings_.maxSourceBVH8NodeCount) * sizeof(softwarebvh::SoftwareRayTraceScene::BVH8NodeGPU); }
  uint64_t worldBVH8NodesSize() const { return static_cast<uint64_t>(settings_.maxWorldBVH8NodeCount) * sizeof(softwarebvh::SoftwareRayTraceScene::BVH8NodeGPU); }
  uint64_t sourceLeafRemapSize() const { return static_cast<uint64_t>(settings_.maxSourceLeafRemapCount) * sizeof(uint32_t); }
  uint64_t worldLeafRemapSize() const { return static_cast<uint64_t>(settings_.maxWorldLeafRemapCount) * sizeof(softwarebvh::SoftwareRayTraceScene::WorldLeafEntryGPU); }
  uint64_t sceneCWBVHNodesSize() const { return static_cast<uint64_t>(settings_.maxSceneCWBVHNodeCount) * sizeof(softwarebvh::SoftwareRayTraceScene::PackedCWBVHNodeGPU); }
  uint64_t sourcePrimitiveRemapSize() const { return static_cast<uint64_t>(settings_.maxSourceTriangleCount) * sizeof(uint32_t); }

  Stage stage_ = Stage::ClearCounters;
  Buffers buffers_{};
  Settings settings_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups bindingGroups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace rendering
} // namespace softwarebvh
