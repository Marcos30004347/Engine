#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "gpusigneddistancefields/GPUSignedDistanceFieldsManager.hpp"
#include "math/math.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsFallbackDebugPreparePass : public rendering::Pass
{
public:
  struct DrawIndirectCommandGPU
  {
    uint32_t vertexCount = 0u;
    uint32_t instanceCount = 0u;
    uint32_t firstVertex = 0u;
    uint32_t firstInstance = 0u;
  };
  static_assert(sizeof(DrawIndirectCommandGPU) == 16u, "DrawIndirectCommandGPU must match VkDrawIndirectCommand");

  explicit GPUSignedDistanceFieldsFallbackDebugPreparePass(GPUSignedDistanceFieldsManager &manager) : manager_(manager)
  {
  }

  ~GPUSignedDistanceFieldsFallbackDebugPreparePass() override
  {
    destroyResources();
  }

  void setEnabled(bool enabled)
  {
    enabled_ = enabled;
    writeUniforms();
    refreshPreparedCommandDiagnostics();
  }

  void updateRuntimeState(const math::Mat4f &viewProjection)
  {
    frustumPlanes_ = extractFrustumPlanes(viewProjection);
    writeUniforms();
    refreshPreparedCommandDiagnostics();
  }

  void setFrustumCullingEnabled(bool enabled)
  {
    frustumCullingEnabled_ = enabled;
    writeUniforms();
    refreshPreparedCommandDiagnostics();
  }

  const std::array<math::Vec4f, 6> &getFrustumPlanes() const
  {
    return frustumPlanes_;
  }

  static bool isVisible(
      const std::array<math::Vec4f, 6> &frustumPlanes,
      const GPUSignedDistanceFieldsManager::InstanceGPU &instance,
      uint32_t *outRejectingPlaneIndex = nullptr,
      float *outRejectingDistance = nullptr)
  {
    for (uint32_t planeIndex = 0u; planeIndex < frustumPlanes.size(); ++planeIndex)
    {
      const math::Vec4f &plane = frustumPlanes[planeIndex];
      math::Vec3f positiveVertex = instance.aabbMin;
      if (plane[0] >= 0.0f)
        positiveVertex[0] = instance.aabbMax[0];
      if (plane[1] >= 0.0f)
        positiveVertex[1] = instance.aabbMax[1];
      if (plane[2] >= 0.0f)
        positiveVertex[2] = instance.aabbMax[2];

      const float distance = plane[0] * positiveVertex[0] + plane[1] * positiveVertex[1] + plane[2] * positiveVertex[2] + plane[3];
      if (distance < 0.0f)
      {
        if (outRejectingPlaneIndex != nullptr)
          *outRejectingPlaneIndex = planeIndex;
        if (outRejectingDistance != nullptr)
          *outRejectingDistance = distance;
        return false;
      }
    }

    if (outRejectingPlaneIndex != nullptr)
      *outRejectingPlaneIndex = UINT32_MAX;
    if (outRejectingDistance != nullptr)
      *outRejectingDistance = 0.0f;
    return true;
  }

  const rendering::Buffer &getIndirectBuffer() const
  {
    return combinedIndirectBuffer_;
  }

  const rendering::Buffer &getIndirectBuffer(GPUSignedDistanceFieldsManager::Path) const
  {
    return combinedIndirectBuffer_;
  }

  uint32_t getIndirectDrawCount() const
  {
    return getPathDrawCount(GPUSignedDistanceFieldsManager::Path::Static) + getPathDrawCount(GPUSignedDistanceFieldsManager::Path::Skinned);
  }

  uint32_t getIndirectDrawCount(GPUSignedDistanceFieldsManager::Path path) const
  {
    return getPathDrawCount(path);
  }

  uint32_t getPathDrawCapacity() const
  {
    return manager_.getSettings().maxInstancesPerPath;
  }

  uint32_t getIndirectDrawCapacity() const
  {
    return getPathDrawCapacity() * 2u;
  }

  const std::vector<DrawIndirectCommandGPU> &getPreparedCommands(GPUSignedDistanceFieldsManager::Path path) const
  {
    return path == GPUSignedDistanceFieldsManager::Path::Skinned ? preparedSkinnedCommands_ : preparedStaticCommands_;
  }

  bool isEnabled() const
  {
    return enabled_;
  }

  bool isFrustumCullingEnabled() const
  {
    return frustumCullingEnabled_;
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    initializeResources();
    writeUniforms();
    refreshPreparedCommandDiagnostics();

    const uint32_t dispatchCount = (getIndirectDrawCapacity() + WORKGROUP_SIZE - 1u) / WORKGROUP_SIZE;
    if (dispatchCount == 0u)
      return;

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch(dispatchCount, 1u, 1u);
  }

private:
  static constexpr uint32_t WORKGROUP_SIZE = 64u;

  struct PrepareUniforms
  {
    std::array<math::Vec4f, 6> frustumPlanes{};
    uint32_t staticInstanceCount = 0u;
    uint32_t skinnedInstanceCount = 0u;
    uint32_t meshCount = 0u;
    uint32_t pathCapacity = 0u;
    uint32_t commandCapacity = 0u;
    uint32_t enableFrustumCulling = 1u;
    uint32_t enabled = 0u;
    uint32_t _padding0 = 0u;
  };
  static_assert(sizeof(PrepareUniforms) == 128u, "Fallback prepare uniforms must match WGSL layout");

  static std::array<math::Vec4f, 6> extractFrustumPlanes(const math::Mat4f &viewProjection)
  {
    auto row = [&](int r) -> std::array<float, 4>
    {
      return {viewProjection.at(r, 0), viewProjection.at(r, 1), viewProjection.at(r, 2), viewProjection.at(r, 3)};
    };
    auto negate = [](std::array<float, 4> value) -> std::array<float, 4>
    {
      return {-value[0], -value[1], -value[2], -value[3]};
    };
    auto normalizePlane = [](std::array<float, 4> a, std::array<float, 4> b) -> math::Vec4f
    {
      math::Vec4f plane(
          a[0] + b[0],
          a[1] + b[1],
          a[2] + b[2],
          a[3] + b[3]);
      const float length = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
      if (length > 1.0e-7f)
      {
        plane[0] /= length;
        plane[1] /= length;
        plane[2] /= length;
        plane[3] /= length;
      }
      return plane;
    };

    const auto r0 = row(0);
    const auto r1 = row(1);
    const auto r2 = row(2);
    const auto r3 = row(3);
    return {
        normalizePlane(r3, r0),
        normalizePlane(r3, negate(r0)),
        normalizePlane(r3, r1),
        normalizePlane(r3, negate(r1)),
        normalizePlane(r3, negate(r2)),
        normalizePlane(r2, {0.0f, 0.0f, 0.0f, 0.0f}),
    };
  }

  uint32_t getPathDrawCount(GPUSignedDistanceFieldsManager::Path path) const
  {
    const uint32_t instanceCount = manager_.getPathResources(path).instanceCount;
    return std::min(instanceCount, getPathDrawCapacity());
  }

  rendering::BindingsLayout createLayout()
  {
    return renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {
                rendering::BindingGroupLayout{
                    .buffers = {
                        {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "meshTable", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "staticInstances", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "skinnedInstances", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "drawCommands", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                },
            },
        });
  }

  rendering::BindingGroups createBindingGroups()
  {
    const auto &shared = manager_.getSharedResources();
    const auto &staticResources = manager_.getPathResources(GPUSignedDistanceFieldsManager::Path::Static);
    const auto &skinnedResources = manager_.getPathResources(GPUSignedDistanceFieldsManager::Path::Skinned);
    return renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_BindingGroups",
            .groups = {
                rendering::GroupInfo{
                    .name = "group0",
                    .buffers = {
                        {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(PrepareUniforms), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 1, .bufferView = {.buffer = shared.meshTable, .offset = 0u, .size = shared.meshTableSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 2, .bufferView = {.buffer = staticResources.instances, .offset = 0u, .size = staticResources.instancesSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 3, .bufferView = {.buffer = skinnedResources.instances, .offset = 0u, .size = skinnedResources.instancesSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 4, .bufferView = {.buffer = combinedIndirectBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(combinedIndirectBuffer_), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                    },
                },
            },
        });
  }

  void initializeResources()
  {
    if (layout_.isValid())
      return;

    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(PrepareUniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    combinedIndirectBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Indirect.buffer",
            .size = static_cast<uint64_t>(getIndirectDrawCapacity()) * sizeof(DrawIndirectCommandGPU),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_CopySrc,
        });

    layout_ = createLayout();
    bindingGroups_ = createBindingGroups();

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/gpusigneddistancefields-fallback-debug-prepare.spirv"),
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

  void destroyResources()
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    pipeline_ = {};
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    shader_ = {};
    if (bindingGroups_.isValid())
      renderGraph->deleteBindingGroups(bindingGroups_);
    bindingGroups_ = {};
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    layout_ = {};
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
    uniformBuffer_ = {};
    if (combinedIndirectBuffer_.isValid())
      renderGraph->deleteBuffer(combinedIndirectBuffer_);
    combinedIndirectBuffer_ = {};
  }

  void writeUniforms()
  {
    if (!uniformBuffer_.isValid())
      return;

    PrepareUniforms uniforms{};
    uniforms.frustumPlanes = frustumPlanes_;
    uniforms.staticInstanceCount = getPathDrawCount(GPUSignedDistanceFieldsManager::Path::Static);
    uniforms.skinnedInstanceCount = getPathDrawCount(GPUSignedDistanceFieldsManager::Path::Skinned);
    uniforms.meshCount = static_cast<uint32_t>(std::min<size_t>(manager_.getUploadedMeshTable().size(), manager_.getSettings().maxMeshes));
    uniforms.pathCapacity = getPathDrawCapacity();
    uniforms.commandCapacity = getIndirectDrawCapacity();
    uniforms.enableFrustumCulling = frustumCullingEnabled_ ? 1u : 0u;
    uniforms.enabled = enabled_ ? 1u : 0u;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(PrepareUniforms), &uniforms);
  }

  std::vector<DrawIndirectCommandGPU> buildPathCommandDiagnostics(GPUSignedDistanceFieldsManager::Path path) const
  {
    const auto &instances = manager_.getUploadedInstances(path);
    const auto &meshTable = manager_.getUploadedMeshTable();
    const uint32_t commandCount = std::min<uint32_t>(
        getPathDrawCount(path),
        static_cast<uint32_t>(instances.size()));
    std::vector<DrawIndirectCommandGPU> commands(commandCount);

    if (!enabled_)
      return commands;

    for (uint32_t instanceIndex = 0u; instanceIndex < commandCount; ++instanceIndex)
    {
      const auto &instance = instances[instanceIndex];
      if (instance.meshIndex >= meshTable.size())
        continue;

      if (frustumCullingEnabled_ && !isVisible(frustumPlanes_, instance))
        continue;

      const auto &mesh = meshTable[instance.meshIndex];
      DrawIndirectCommandGPU &command = commands[instanceIndex];
      command.vertexCount = mesh.triangleCount * 3u;
      command.instanceCount = command.vertexCount > 0u ? 1u : 0u;
      command.firstVertex = 0u;
      command.firstInstance = instanceIndex;
    }

    return commands;
  }

  void refreshPreparedCommandDiagnostics()
  {
    preparedStaticCommands_ = buildPathCommandDiagnostics(GPUSignedDistanceFieldsManager::Path::Static);
    preparedSkinnedCommands_ = buildPathCommandDiagnostics(GPUSignedDistanceFieldsManager::Path::Skinned);
  }

  GPUSignedDistanceFieldsManager &manager_;
  bool enabled_ = false;
  bool frustumCullingEnabled_ = true;
  std::array<math::Vec4f, 6> frustumPlanes_{};
  std::vector<DrawIndirectCommandGPU> preparedStaticCommands_{};
  std::vector<DrawIndirectCommandGPU> preparedSkinnedCommands_{};
  rendering::Buffer uniformBuffer_;
  rendering::Buffer combinedIndirectBuffer_;
  rendering::BindingsLayout layout_;
  rendering::BindingGroups bindingGroups_;
  rendering::Shader shader_;
  rendering::ComputePipeline pipeline_;
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
