#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelGeometryFieldCache.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"
#include "voxel/svraster/SparseVoxelTree64Material.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace voxel::svraster::rendering
{

class SparseVoxelTree64TracePass final : public ::rendering::Pass
{
public:
  enum class NodeLayout : uint32_t
  {
    Packed12,
    Padded16,
  };

  enum class Traversal : uint32_t
  {
    RegionMarcher,
    MirroredAncestor,
  };

  enum class RayGeneration : uint32_t
  {
    // One ray read per invocation from `Buffers::rays`.
    RayBuffer,
    // Ray derived from the invocation index and a camera uniform. Binding 3
    // carries the camera instead of the ray array, so no ray array is written
    // or read at all.
    PrimaryCamera,
  };

  struct Buffers
  {
    ::rendering::Buffer state;
    ::rendering::Buffer chunks;
    ::rendering::Buffer nodes;
    ::rendering::Buffer rays;
    ::rendering::Buffer results;
    ::rendering::Buffer materialCells;
    ::rendering::Buffer microMaterials;
    ::rendering::Buffer geometryPrograms;
    ::rendering::Buffer geometryInstructions;
    ::rendering::Buffer geometryParameters;
    ::rendering::Buffer materialCounters;
    ::rendering::Buffer materialFieldDefaults;
    ::rendering::Buffer geometryFieldCache;
  };

  struct Capacities
  {
    uint32_t chunkCount = 0u;
    uint32_t nodeCount = 0u;
    uint32_t nodeSlotCapacity = 0u;
    uint32_t rayCount = 0u;
    uint32_t materialCellCount = 0u;
    uint32_t microMaterialCount = 0u;
    uint32_t geometryProgramCount = 0u;
    uint32_t geometryInstructionCount = 0u;
    uint32_t geometryParameterCount = 0u;
    uint32_t materialFieldDefaultCount = 0u;
    uint32_t geometryFieldCacheCount = 0u;
  };

  SparseVoxelTree64TracePass(
      const Buffers buffers,
      const Capacities capacities,
      const NodeLayout nodeLayout = NodeLayout::Packed12,
      const Traversal traversal = Traversal::RegionMarcher,
      const ::rendering::Buffer externalDispatch = {},
      const ::rendering::Buffer indirectArgs = {},
      const RayGeneration rayGeneration = RayGeneration::RayBuffer)
      : buffers_(buffers), capacities_(capacities), nodeLayout_(nodeLayout), traversal_(traversal), externalDispatch_(externalDispatch), indirectArgs_(indirectArgs),
        rayGeneration_(rayGeneration)
  {
    if (nodeLayout_ == NodeLayout::Padded16 && traversal_ == Traversal::MirroredAncestor)
      throw std::invalid_argument("Tree64 padded/mirrored-ancestor trace variant is not compiled");
    if (rayGeneration_ == RayGeneration::PrimaryCamera && (nodeLayout_ != NodeLayout::Packed12 || externalDispatch_.isValid()))
      throw std::invalid_argument("Tree64 primary-ray trace is only compiled for the packed/direct-dispatch variant");
    const bool materialGeometry = buffers_.materialCells.isValid();
    if (materialGeometry != buffers_.microMaterials.isValid() ||
        materialGeometry != buffers_.geometryPrograms.isValid() ||
        materialGeometry != buffers_.geometryInstructions.isValid() ||
        materialGeometry != buffers_.geometryParameters.isValid() ||
        materialGeometry != buffers_.materialFieldDefaults.isValid() ||
        materialGeometry != buffers_.geometryFieldCache.isValid() ||
        materialGeometry != buffers_.materialCounters.isValid())
      throw std::invalid_argument(
          "Tree64 material geometry requires all sidecar/program buffers");
    if (materialGeometry &&
        (capacities_.materialCellCount == 0u ||
         capacities_.microMaterialCount == 0u ||
         capacities_.geometryProgramCount == 0u ||
         capacities_.geometryInstructionCount == 0u ||
         capacities_.geometryParameterCount == 0u ||
         capacities_.materialFieldDefaultCount == 0u ||
         capacities_.geometryFieldCacheCount == 0u))
      throw std::invalid_argument(
          "Tree64 material geometry capacities are invalid");
    capacities_.chunkCount = std::max(1u, capacities_.chunkCount);
    capacities_.nodeCount = std::max(1u, capacities_.nodeCount);
    capacities_.nodeSlotCapacity = std::max(1u, capacities_.nodeSlotCapacity);
    capacities_.rayCount = std::max(1u, capacities_.rayCount);
  }

  ~SparseVoxelTree64TracePass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (uniforms_.isValid())
      renderGraph->deleteBuffer(uniforms_);
    if (primaryCameraBuffer_.isValid())
      renderGraph->deleteBuffer(primaryCameraBuffer_);
  }

  // Primary-ray variants only. Must describe the same camera the G-buffer uses
  // to reconstruct world positions, or shading lands off the traced surface.
  void updatePrimaryCamera(const SparseVoxelTree64PrimaryCameraGPU &camera)
  {
    primaryCamera_ = camera;
    if (primaryCameraBuffer_.isValid())
      renderGraph->bufferWrite(primaryCameraBuffer_, 0u, sizeof(primaryCamera_), &primaryCamera_);
  }

  // Traversal level of detail for every ray this pass traces. Defaults to
  // disabled, which is leaf-exact. Only correct for consumers that tolerate
  // conservative over-occlusion - shadow, occlusion and GI rays - never for the
  // primary rays that resolve the G-buffer.
  void setTraversalLOD(const SparseVoxelTree64TraceLODGPU lod)
  {
    lod_ = lod;
  }

  bool update(const uint32_t rayCount, const uint32_t maximumQueries)
  {
    if (rayCount > capacities_.rayCount || maximumQueries == 0u)
      return false;
    dispatch_.rayCount = rayCount;
    dispatch_.maximumQueries = maximumQueries;
    dispatch_.lodWorldSize = std::max(0.0f, lod_.worldSize);
    dispatch_.lodDistanceSlope = std::max(0.0f, lod_.distanceSlope);
    dispatch_.lodStartDistance = std::max(0.0f, lod_.startDistance);
    dispatch_.lodMaximumWorldSize = std::max(0.0f, lod_.maximumWorldSize);
    dispatch_.chunkCapacity = capacities_.chunkCount;
    dispatch_.nodeCapacity = capacities_.nodeCount;
    dispatch_.nodeSlotCapacity = capacities_.nodeSlotCapacity;
    const ::rendering::Buffer dispatchBuffer = externalDispatch_.isValid() ? externalDispatch_ : uniforms_;
    if (dispatchBuffer.isValid())
      renderGraph->bufferWrite(dispatchBuffer, 0u, sizeof(dispatch_), &dispatch_);
    return true;
  }

  // The compaction pass writes rayCount into this buffer and produces the
  // matching indirect arguments. Both buffers are owned by the caller.
  void setExternalIndirectDispatch(
      const ::rendering::Buffer dispatch,
      const ::rendering::Buffer indirectArgs)
  {
    if (layout_.isValid())
      throw std::logic_error("Tree64 external dispatch must be configured before recording");
    externalDispatch_ = dispatch;
    indirectArgs_ = indirectArgs;
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    const ::rendering::Buffer dispatchBuffer = externalDispatch_.isValid() ? externalDispatch_ : uniforms_;
    renderGraph->bufferWrite(dispatchBuffer, 0u, sizeof(dispatch_), &dispatch_);
    if (primaryCameraBuffer_.isValid())
      renderGraph->bufferWrite(primaryCameraBuffer_, 0u, sizeof(primaryCamera_), &primaryCamera_);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    const uint32_t workgroupSize = 64u;
    if (indirectArgs_.isValid())
      commandBuffer.cmdDispatchIndirect(indirectArgs_, 0u);
    else
      commandBuffer.cmdDispatch(std::max(1u, (capacities_.rayCount + workgroupSize - 1u) / workgroupSize), 1u, 1u);
  }

private:
  static ::rendering::BufferView view(const ::rendering::Buffer buffer, const uint64_t size, const ::rendering::AccessPattern access)
  {
    return {
      .buffer = buffer,
      .offset = 0u,
      .size = std::max<uint64_t>(size, sizeof(uint32_t)),
      .access = access,
    };
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    if (!externalDispatch_.isValid())
      uniforms_ = renderGraph->createBuffer({
        .name = passName + "_Uniforms",
        .size = sizeof(dispatch_),
        .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
        .frameLocal = true,
      });
    const bool primaryRays = rayGeneration_ == RayGeneration::PrimaryCamera;
    const bool materialGeometry = buffers_.materialCells.isValid();
    if (primaryRays)
      primaryCameraBuffer_ = renderGraph->createBuffer({
        .name = passName + "_PrimaryCamera",
        .size = sizeof(primaryCamera_),
        .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
        .frameLocal = true,
      });
    const ::rendering::Buffer dispatchBuffer = externalDispatch_.isValid() ? externalDispatch_ : uniforms_;
    const auto storageLayout = [](
                                   const char *name,
                                   const uint32_t binding)
    {
      return ::rendering::BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
        .isDynamic = false,
      };
    };
    std::vector<::rendering::BindingGroupLayoutBufferEntry> layoutBuffers{
      storageLayout("treeState", 0u),
      storageLayout("chunks", 1u),
      storageLayout("nodes", 2u),
      {.name = primaryRays ? "primaryCamera" : "rays",
       .binding = 3u,
       .type = primaryRays
           ? ::rendering::BufferBindingType::BufferBindingType_UniformBuffer
           : ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
       .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
       .isDynamic = false},
      storageLayout("results", 4u),
      {.name = "dispatch",
       .binding = 5u,
       .type = externalDispatch_.isValid()
           ? ::rendering::BufferBindingType::BufferBindingType_StorageBuffer
           : ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
       .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
       .isDynamic = false},
    };
    if (materialGeometry)
    {
      layoutBuffers.insert(layoutBuffers.end(), {
        storageLayout("tree64MaterialCells", 6u),
        storageLayout("tree64MicroMaterials", 7u),
        storageLayout("voxelGeometryPrograms", 8u),
        storageLayout("voxelGeometryInstructions", 9u),
        storageLayout("voxelGeometryParameters", 10u),
        storageLayout("tree64MaterialCounters", 11u),
        storageLayout("voxelMaterialFieldDefaults", 12u),
        storageLayout("voxelGeometryFieldCache", 13u),
      });
    }
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = std::move(layoutBuffers),
      }},
    });
    std::vector<::rendering::BindingBuffer> groupBuffers{
      {.bufferView = view(buffers_.state, sizeof(SparseVoxelTree64StateGPU), ::rendering::AccessPattern::SHADER_READ), .binding = 0u},
      {.bufferView = view(buffers_.chunks, uint64_t{capacities_.chunkCount} * sizeof(SparseVoxelTree64ChunkGPU), ::rendering::AccessPattern::SHADER_READ), .binding = 1u},
      {.bufferView = view(buffers_.nodes, uint64_t{capacities_.nodeCount} * nodeStride(), ::rendering::AccessPattern::SHADER_READ), .binding = 2u},
      {.bufferView = primaryRays ? view(primaryCameraBuffer_, sizeof(primaryCamera_), ::rendering::AccessPattern::UNIFORM_READ) : view(buffers_.rays, uint64_t{capacities_.rayCount} * sizeof(SparseVoxelOccupancyDDARayGPU), ::rendering::AccessPattern::SHADER_READ), .binding = 3u},
      {.bufferView = view(buffers_.results, uint64_t{capacities_.rayCount} * sizeof(SparseVoxelOccupancyDDAResultGPU), ::rendering::AccessPattern::SHADER_WRITE), .binding = 4u},
      {.bufferView = view(dispatchBuffer, sizeof(dispatch_), externalDispatch_.isValid() ? ::rendering::AccessPattern::SHADER_READ : ::rendering::AccessPattern::UNIFORM_READ), .binding = 5u},
    };
    if (materialGeometry)
    {
      groupBuffers.insert(groupBuffers.end(), {
        {.bufferView = view(buffers_.materialCells, uint64_t{capacities_.materialCellCount} * sizeof(SparseVoxelTree64MaterialCellGPU), ::rendering::AccessPattern::SHADER_READ), .binding = 6u},
        {.bufferView = view(buffers_.microMaterials, uint64_t{capacities_.microMaterialCount} * sizeof(SparseVoxelMicroMaterialGPU), ::rendering::AccessPattern::SHADER_READ), .binding = 7u},
        {.bufferView = view(buffers_.geometryPrograms, uint64_t{capacities_.geometryProgramCount} * sizeof(material::VoxelGeometryProgramGPU), ::rendering::AccessPattern::SHADER_READ), .binding = 8u},
        {.bufferView = view(buffers_.geometryInstructions, uint64_t{capacities_.geometryInstructionCount} * sizeof(material::VoxelGeometryInstructionGPU), ::rendering::AccessPattern::SHADER_READ), .binding = 9u},
        {.bufferView = view(buffers_.geometryParameters, uint64_t{capacities_.geometryParameterCount} * sizeof(material::VoxelGeometryParameterBlockGPU), ::rendering::AccessPattern::SHADER_READ), .binding = 10u},
        {.bufferView = view(buffers_.materialCounters, sizeof(SparseVoxelTree64MaterialCountersGPU), ::rendering::AccessPattern::SHADER_READ_WRITE), .binding = 11u},
        {.bufferView = view(buffers_.materialFieldDefaults, uint64_t{capacities_.materialFieldDefaultCount} * sizeof(material::VoxelMaterialFieldDefaultsGPU), ::rendering::AccessPattern::SHADER_READ), .binding = 12u},
        {.bufferView = view(buffers_.geometryFieldCache, uint64_t{capacities_.geometryFieldCacheCount} * sizeof(SparseVoxelGeometryFieldCacheRecordGPU), ::rendering::AccessPattern::SHADER_READ), .binding = 13u},
      });
    }
    groups_ = renderGraph->createBindingGroups({
      .name = passName + "_Groups",
      .layout = layout_,
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = std::move(groupBuffers),
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/" +
          std::string{
            primaryRays
                ? (traversal_ == Traversal::MirroredAncestor
                       ? "sparse-voxel-tree64-trace-primary-mirrored-ancestor.spirv"
                       : "sparse-voxel-tree64-trace-primary.spirv")
            : nodeLayout_ == NodeLayout::Padded16
                ? "sparse-voxel-tree64-trace-padded.spirv"
            : externalDispatch_.isValid()
                ? (traversal_ == Traversal::MirroredAncestor
                       ? "sparse-voxel-tree64-trace-storage-dispatch-mirrored-ancestor.spirv"
                       : "sparse-voxel-tree64-trace-storage-dispatch.spirv")
            : traversal_ == Traversal::MirroredAncestor
                ? "sparse-voxel-tree64-trace-mirrored-ancestor.spirv"
                : "sparse-voxel-tree64-trace.spirv"} +
          (materialGeometry ? ".material" : "")),
      .type = ::rendering::ShaderType::SpirV,
      .layout = layout_,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .name = passName + "_Pipeline",
      .shader = shader_,
      .entry = "traceTree64",
      .layout = layout_,
    });
  }

  uint64_t nodeStride() const
  {
    return nodeLayout_ == NodeLayout::Padded16 ? sizeof(SparseVoxelTree64NodePaddedGPU) : sizeof(SparseVoxelTree64NodeGPU);
  }

  Buffers buffers_{};
  Capacities capacities_{};
  NodeLayout nodeLayout_ = NodeLayout::Packed12;
  Traversal traversal_ = Traversal::RegionMarcher;
  RayGeneration rayGeneration_ = RayGeneration::RayBuffer;
  SparseVoxelTree64TraceLODGPU lod_{};
  SparseVoxelTree64TraceDispatchGPU dispatch_{};
  SparseVoxelTree64PrimaryCameraGPU primaryCamera_{};
  ::rendering::Buffer primaryCameraBuffer_{};
  ::rendering::Buffer externalDispatch_{};
  ::rendering::Buffer indirectArgs_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
