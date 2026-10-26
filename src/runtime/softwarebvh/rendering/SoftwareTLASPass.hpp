#pragma once

#include <algorithm>

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "softwarebvh/SoftwareTLASManager.hpp"

namespace softwarebvh
{
namespace rendering
{

class SoftwareTLASPass : public ::rendering::Pass
{
public:
  enum class Stage
  {
    PrepareRebuild,
    ClearBuildFlags,
    BuildHierarchyLeaves,
    BuildHierarchy,
    RefitHierarchy,
    InitializeCWBVH,
    BuildCWBVH,
    RefitCanonical,
    RefitCWBVH,
  };

  explicit SoftwareTLASPass(softwarebvh::SoftwareTLASManager &manager, const Stage stage) : manager_(manager), stage_(stage)
  {
  }

  ~SoftwareTLASPass() override
  {
    destroyResources();
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    if (stage_ == Stage::BuildHierarchyLeaves)
    {
      return;
    }

    ensureResources();
    if (!pipeline_.isValid() || !bindingGroups_.isValid())
    {
      return;
    }

    const uint32_t canonicalNodeCount = manager_.getCanonicalNodeCount();
    const uint32_t cwbvhNodeCount = manager_.getCwbvhNodeCount();
    if (!usesBuildIndirectDispatch() && (canonicalNodeCount == 0u || cwbvhNodeCount == 0u))
    {
      return;
    }

    writeUniforms();

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    if (usesBuildIndirectDispatch())
    {
      commandBuffer.cmdDispatchIndirect(manager_.getBuildIndirectArgsBuffer(), 0u);
    }
    else if (stage_ == Stage::RefitCanonical)
    {
      commandBuffer.cmdDispatchIndirect(manager_.getRefitIndirectArgsBuffer(), 0u);
    }
    else if (stage_ == Stage::RefitCWBVH)
    {
      commandBuffer.cmdDispatchIndirect(manager_.getRefitCwbvhIndirectArgsBuffer(), 0u);
    }
    else
    {
      commandBuffer.cmdDispatch(std::max(1u, (canonicalNodeCount + 63u) / 64u), 1u, 1u);
    }
  }

  void appendFrameOverrides(::rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    manager_.appendFrameOverrides(overrides, frameSlot);
  }

private:
  struct Uniforms
  {
    uint32_t canonicalNodeCount = 0u;
    uint32_t cwbvhNodeCount = 0u;
    uint32_t instanceCount = 0u;
    uint32_t maxInstanceCount = 0u;
  };
  static_assert(sizeof(Uniforms) == 16u, "SoftwareTLASPass::Uniforms must stay tightly packed");

  struct GenericCWBVHUniforms
  {
    struct AABB
    {
      float minPoint[3]{0.0f, 0.0f, 0.0f};
      float _padding0 = 0.0f;
      float maxPoint[3]{0.0f, 0.0f, 0.0f};
      float _padding1 = 0.0f;
    };
    static_assert(sizeof(AABB) == 32u, "SoftwareTLASPass::GenericCWBVHUniforms::AABB must stay tightly packed");

    uint32_t num = 0u;
    uint32_t targetPrimitivesPerNode = 1u;
    uint32_t targetNumberOfLeaves = 0u;
    uint32_t padding = 0u;
    AABB volumeAabb{};
  };
  static_assert(sizeof(GenericCWBVHUniforms) == 48u, "SoftwareTLASPass::GenericCWBVHUniforms must stay tightly packed");

  void destroyResources()
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

    pipeline_ = {};
    shader_ = {};
    bindingGroups_ = {};
    layout_ = {};
    uniformBuffer_ = {};
  }

  void ensureResources()
  {
    if (!uniformBuffer_.isValid())
    {
      uniformBuffer_ = createFrameLocalBuffer(
          ::rendering::BufferInfo{
              .name = passName + "_Uniforms.buffer",
              .size = std::max(sizeof(Uniforms), sizeof(GenericCWBVHUniforms)),
              .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
          });
      resourcesDirty_ = true;
    }

    if (!layout_.isValid())
    {
      if (usesGenericCwbvhBuildShader())
      {
        layout_ = renderGraph->createBindingsLayout(
            ::rendering::BindingsLayoutInfo{
                .name = passName + "_Layout",
                .groups = {::rendering::BindingGroupLayout{
                    .buffers = {
                        {.name = "uniforms", .binding = 0, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "aabbs", .binding = 1, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "inputLeafIndexRemap", .binding = 2, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "sortedMortonCodes", .binding = 3, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "flags", .binding = 4, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "signals", .binding = 5, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "hlbvhNodes", .binding = 6, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cwbvhNodes", .binding = 7, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "outputLeafIndicesRemap", .binding = 8, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "debug", .binding = 9, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                }},
            });
        resourcesDirty_ = true;
      }
      else
      {
        layout_ = renderGraph->createBindingsLayout(
            ::rendering::BindingsLayoutInfo{
                .name = passName + "_Layout",
                .groups = {::rendering::BindingGroupLayout{
                    .buffers = {
                        {.name = "uniforms", .binding = 0, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "instanceAabbs", .binding = 1, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "canonicalNodes", .binding = 2, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cwbvhNodes", .binding = 3, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cwbvhNodeRefs", .binding = 4, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "counters", .binding = 5, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "mortonCodes", .binding = 6, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "instanceIndices", .binding = 7, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "buildFlags", .binding = 8, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "buildSignals", .binding = 9, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "leafInstanceIndices", .binding = 10, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "buildDebug", .binding = 11, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                }},
            });
        resourcesDirty_ = true;
      }
    }

    if (!shader_.isValid())
    {
      shader_ = renderGraph->createShader(
          ::rendering::ShaderInfo{
              .name = passName + "_Shader",
              .layout = layout_,
              .src = os::io::readRelativeFile(usesGenericCwbvhBuildShader() ? "assets/shaders/spirv/cwbvh-build.spirv" : "assets/shaders/spirv/softwarebvh-tlas.spirv"),
              .type = ::rendering::ShaderType::SpirV,
          });
      resourcesDirty_ = true;
    }

    if (!pipeline_.isValid())
    {
      pipeline_ = renderGraph->createComputePipeline(
          ::rendering::ComputePipelineInfo{
              .entry = entryPoint(),
              .layout = layout_,
              .name = passName + "_Pipeline",
              .shader = shader_,
          });
      resourcesDirty_ = true;
    }

    if (resourcesDirty_)
    {
      if (bindingGroups_.isValid())
      {
        renderGraph->deleteBindingGroups(bindingGroups_);
      }

      if (usesGenericCwbvhBuildShader())
      {
        bindingGroups_ = renderGraph->createBindingGroups(
            ::rendering::BindingGroupsInfo{
                .layout = layout_,
                .name = passName + "_Bindings",
                .groups = {::rendering::GroupInfo{
                    .name = "group0",
                    .buffers = {
                        {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .access = ::rendering::AccessPattern::SHADER_READ, .offset = 0u, .size = sizeof(GenericCWBVHUniforms)}},
                        {.binding = 1, .bufferView = {.buffer = manager_.getInstanceAabbsBuffer(), .access = ::rendering::AccessPattern::SHADER_READ, .offset = 0u, .size = manager_.getInstanceAabbsBufferSize()}},
                        {.binding = 2, .bufferView = {.buffer = manager_.getBuildInstanceIndicesBuffer(), .access = buildSortAccess(), .offset = 0u, .size = manager_.getBuildInstanceIndicesBufferSize()}},
                        {.binding = 3, .bufferView = {.buffer = manager_.getBuildMortonCodesBuffer(), .access = ::rendering::AccessPattern::SHADER_READ, .offset = 0u, .size = manager_.getBuildMortonCodesBufferSize()}},
                        {.binding = 4, .bufferView = {.buffer = manager_.getBuildFlagsBuffer(), .access = buildScratchAccess(), .offset = 0u, .size = manager_.getBuildFlagsBufferSize()}},
                        {.binding = 5, .bufferView = {.buffer = manager_.getBuildSignalsBuffer(), .access = buildScratchAccess(), .offset = 0u, .size = manager_.getBuildSignalsBufferSize()}},
                        {.binding = 6, .bufferView = {.buffer = manager_.getBuildHLBVHNodesBuffer(), .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE, .offset = 0u, .size = manager_.getBuildHLBVHNodesBufferSize()}},
                        {.binding = 7, .bufferView = {.buffer = manager_.getCwbvhNodesBuffer(), .access = cwbvhNodeAccess(), .offset = 0u, .size = manager_.getCwbvhNodesBufferSize()}},
                        {.binding = 8, .bufferView = {.buffer = manager_.getLeafInstanceIndicesBuffer(), .access = leafInstanceAccess(), .offset = 0u, .size = manager_.getLeafInstanceIndicesBufferSize()}},
                        {.binding = 9, .bufferView = {.buffer = manager_.getBuildDebugBuffer(), .access = buildScratchAccess(), .offset = 0u, .size = manager_.getBuildDebugBufferSize()}},
                    },
                }},
            });
        resourcesDirty_ = false;
        return;
      }

      bindingGroups_ = renderGraph->createBindingGroups(
          ::rendering::BindingGroupsInfo{
              .layout = layout_,
              .name = passName + "_Bindings",
              .groups = {::rendering::GroupInfo{
                  .name = "group0",
                  .buffers = {
                      {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .access = ::rendering::AccessPattern::SHADER_READ, .offset = 0u, .size = sizeof(Uniforms)}},
                      {.binding = 1, .bufferView = {.buffer = manager_.getInstanceAabbsBuffer(), .access = ::rendering::AccessPattern::SHADER_READ, .offset = 0u, .size = manager_.getInstanceAabbsBufferSize()}},
                      {.binding = 2, .bufferView = {.buffer = manager_.getCanonicalNodesBuffer(), .access = canonicalNodeAccess(), .offset = 0u, .size = manager_.getCanonicalNodesBufferSize()}},
                      {.binding = 3, .bufferView = {.buffer = manager_.getCwbvhNodesBuffer(), .access = cwbvhNodeAccess(), .offset = 0u, .size = manager_.getCwbvhNodesBufferSize()}},
                      {.binding = 4, .bufferView = {.buffer = manager_.getCwbvhNodeRefsBuffer(), .access = cwbvhNodeRefAccess(), .offset = 0u, .size = manager_.getCwbvhNodeRefsBufferSize()}},
                      {.binding = 5, .bufferView = {.buffer = manager_.getCountersBuffer(), .access = ::rendering::AccessPattern::SHADER_READ, .offset = 0u, .size = manager_.getCountersBufferSize()}},
                      {.binding = 6, .bufferView = {.buffer = manager_.getBuildMortonCodesBuffer(), .access = buildSortAccess(), .offset = 0u, .size = manager_.getBuildMortonCodesBufferSize()}},
                      {.binding = 7, .bufferView = {.buffer = manager_.getBuildInstanceIndicesBuffer(), .access = buildSortAccess(), .offset = 0u, .size = manager_.getBuildInstanceIndicesBufferSize()}},
                      {.binding = 8, .bufferView = {.buffer = manager_.getBuildFlagsBuffer(), .access = buildScratchAccess(), .offset = 0u, .size = manager_.getBuildFlagsBufferSize()}},
                      {.binding = 9, .bufferView = {.buffer = manager_.getBuildSignalsBuffer(), .access = buildScratchAccess(), .offset = 0u, .size = manager_.getBuildSignalsBufferSize()}},
                      {.binding = 10, .bufferView = {.buffer = manager_.getLeafInstanceIndicesBuffer(), .access = leafInstanceAccess(), .offset = 0u, .size = manager_.getLeafInstanceIndicesBufferSize()}},
                      {.binding = 11, .bufferView = {.buffer = manager_.getBuildDebugBuffer(), .access = buildScratchAccess(), .offset = 0u, .size = manager_.getBuildDebugBufferSize()}},
                  },
              }},
          });
      resourcesDirty_ = false;
    }
  }

  void writeUniforms()
  {
    if (usesGenericCwbvhBuildShader())
    {
      GenericCWBVHUniforms uniforms{};
      uniforms.num = manager_.getLastBuildSummary().instanceCount;
      uniforms.targetPrimitivesPerNode = 1u;
      uniforms.targetNumberOfLeaves = manager_.getLastBuildSummary().instanceCount;
      renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
      return;
    }

    const Uniforms uniforms{
        .canonicalNodeCount = manager_.getCanonicalNodeCount(),
        .cwbvhNodeCount = manager_.getCwbvhNodeCount(),
        .instanceCount = manager_.getLastBuildSummary().instanceCount,
        .maxInstanceCount = manager_.getMaxInstanceCount(),
    };
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), const_cast<Uniforms *>(&uniforms));
  }

  ::rendering::AccessPattern canonicalNodeAccess() const
  {
    return (stage_ == Stage::RefitCanonical ||
            stage_ == Stage::BuildHierarchy ||
            stage_ == Stage::BuildHierarchyLeaves ||
            stage_ == Stage::RefitHierarchy)
               ? (::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE)
               : ::rendering::AccessPattern::SHADER_READ;
  }

  ::rendering::AccessPattern cwbvhNodeAccess() const
  {
    return (stage_ == Stage::RefitCWBVH ||
            stage_ == Stage::InitializeCWBVH ||
            stage_ == Stage::BuildCWBVH)
               ? (::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE)
               : ::rendering::AccessPattern::SHADER_READ;
  }

  ::rendering::AccessPattern cwbvhNodeRefAccess() const
  {
    return (stage_ == Stage::RefitCWBVH || stage_ == Stage::BuildCWBVH)
               ? (::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE)
               : ::rendering::AccessPattern::SHADER_READ;
  }

  ::rendering::AccessPattern buildSortAccess() const
  {
    return (stage_ == Stage::PrepareRebuild || usesGenericCwbvhBuildShader())
               ? (::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE)
               : ::rendering::AccessPattern::SHADER_READ;
  }

  ::rendering::AccessPattern buildScratchAccess() const
  {
    return (stage_ == Stage::ClearBuildFlags ||
            stage_ == Stage::RefitHierarchy ||
            stage_ == Stage::InitializeCWBVH ||
            stage_ == Stage::BuildCWBVH)
               ? (::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE)
               : ::rendering::AccessPattern::SHADER_READ;
  }

  ::rendering::AccessPattern leafInstanceAccess() const
  {
    return (stage_ == Stage::BuildCWBVH)
               ? (::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE)
               : ::rendering::AccessPattern::SHADER_READ;
  }

  bool usesGenericCwbvhBuildShader() const
  {
    return stage_ == Stage::ClearBuildFlags ||
           stage_ == Stage::BuildHierarchy ||
           stage_ == Stage::RefitHierarchy ||
           stage_ == Stage::InitializeCWBVH ||
           stage_ == Stage::BuildCWBVH;
  }

  bool usesBuildIndirectDispatch() const
  {
    return stage_ == Stage::PrepareRebuild ||
           stage_ == Stage::ClearBuildFlags ||
           stage_ == Stage::BuildHierarchy ||
           stage_ == Stage::RefitHierarchy ||
           stage_ == Stage::InitializeCWBVH ||
           stage_ == Stage::BuildCWBVH;
  }

  const char *entryPoint() const
  {
    if (usesGenericCwbvhBuildShader())
    {
      switch (stage_)
      {
      case Stage::ClearBuildFlags:
        return "set_flags_to_zero";
      case Stage::BuildHierarchy:
        return "hlbvh_build_hierarchy";
      case Stage::RefitHierarchy:
        return "hlbvh_refit_aabbs";
      case Stage::InitializeCWBVH:
        return "cwbvh_initialize";
      case Stage::BuildCWBVH:
        return "cwbvh_build";
      default:
        break;
      }
    }

    switch (stage_)
    {
    case Stage::PrepareRebuild:
      return "prepare_rebuild";
    case Stage::ClearBuildFlags:
      return "clear_build_flags";
    case Stage::BuildHierarchyLeaves:
      return "build_hierarchy_leaves";
    case Stage::BuildHierarchy:
      return "build_hierarchy";
    case Stage::RefitHierarchy:
      return "refit_hierarchy";
    case Stage::InitializeCWBVH:
      return "initialize_cwbvh_rebuild";
    case Stage::BuildCWBVH:
      return "build_cwbvh_rebuild";
    case Stage::RefitCanonical:
      return "refit_canonical";
    case Stage::RefitCWBVH:
      return "refit_cwbvh";
    }
    return "refit_canonical";
  }

  softwarebvh::SoftwareTLASManager &manager_;
  Stage stage_ = Stage::RefitCanonical;
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups bindingGroups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
  bool resourcesDirty_ = true;
};

} // namespace rendering
} // namespace softwarebvh
