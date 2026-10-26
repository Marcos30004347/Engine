#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"
#include "voxel/svraster/SparseVoxelTree64Material.hpp"
#include "voxel/svraster/SparseVoxelGeometryFieldCache.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace voxel::svraster::rendering
{

class SparseVoxelTree64DynamicTracePass final : public ::rendering::Pass
{
public:
  // sizeof(SoftwareTLASManager::BVH4NodeGPU), restated so this pass does not
  // have to include the softwarebvh headers. The layouts are asserted equal
  // where the TLAS is actually built.
  static constexpr uint64_t kTlasBvh4NodeBytes = 128u;

  enum class Traversal : uint32_t
  {
    RegionMarcher,
    MirroredAncestor,
    // Same BLAS traversal as MirroredAncestor; only the instance broad phase
    // differs. A published TLAS node count selects it at runtime, so a build
    // carrying this still falls back to the clipmap directory when no TLAS has
    // been published yet.
    MirroredAncestorTlasBvh4,
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

  enum class Query : uint32_t
  {
    ClosestHit,
    // Visibility-only query. Dynamic instances are traversed once over the
    // complete ray interval instead of once per overlapping world chunk.
    AnyHit,
  };

  struct Buffers
  {
    ::rendering::Buffer state;
    ::rendering::Buffer chunks;
    ::rendering::Buffer staticNodes;
    ::rendering::Buffer rays;
    ::rendering::Buffer results;
    ::rendering::Buffer memberships;
    ::rendering::Buffer references;
    ::rendering::Buffer instances;
    ::rendering::Buffer models;
    ::rendering::Buffer dynamicNodes;
    ::rendering::Buffer materialCells;
    ::rendering::Buffer microMaterials;
    ::rendering::Buffer geometryPrograms;
    ::rendering::Buffer geometryInstructions;
    ::rendering::Buffer geometryParameters;
    ::rendering::Buffer materialCounters;
    ::rendering::Buffer materialFieldDefaults;
    ::rendering::Buffer geometryFieldCache;
    ::rendering::Buffer terrainPageStates;
    ::rendering::Buffer terrainDescriptors;
    ::rendering::Buffer terrainNodePageGenerations;
    // Compact nodes for closest-hit/primary variants, padded nodes for the
    // any-hit variant, matching the dynamicNodes representation.
    ::rendering::Buffer terrainPageNodes;
    ::rendering::Buffer terrainMaterialPageGenerations;
    ::rendering::Buffer terrainPageMaterialCells;
    // BVH4 TLAS nodes from SoftwareTLASManager. Optional: an absent or empty
    // buffer leaves the directory march in charge.
    ::rendering::Buffer tlasBvh4Nodes;
    // TLAS instance index -> Tree64 dynamic instance index, one u32 per TLAS
    // instance. Required whenever tlasBvh4Nodes is: the builder compacts its
    // input and the Tree64 layer keeps sparse stable slots, so a leaf index is
    // not an instance index. See sparseVoxelTree64TLASInstanceSlots.
    ::rendering::Buffer tlasInstanceSlots;
  };

  struct Capacities
  {
    uint32_t chunkCount = 0u;
    uint32_t staticNodeCount = 0u;
    uint32_t nodeSlotCapacity = 0u;
    uint32_t rayCount = 0u;
    uint32_t membershipCount = 0u;
    uint32_t referenceCount = 0u;
    uint32_t instanceCount = 0u;
    uint32_t modelCount = 0u;
    uint32_t dynamicNodeCount = 0u;
    uint32_t maximumReferencesPerChunk = 0u;
    uint32_t materialCellCount = 0u;
    uint32_t microMaterialCount = 0u;
    uint32_t geometryProgramCount = 0u;
    uint32_t geometryInstructionCount = 0u;
    uint32_t geometryParameterCount = 0u;
    uint32_t materialFieldDefaultCount = 0u;
    uint32_t geometryFieldCacheCount = 0u;
    uint32_t terrainPageStateCount = 0u;
    uint32_t terrainDescriptorCount = 0u;
    uint32_t terrainPhysicalPageCount = 0u;
    uint32_t terrainNodesPerPage = 0u;
    uint32_t terrainMaterialCellsPerPage = 0u;
    // TLAS instance capacity, matching SoftwareTLASManager::Settings
    // maxInstances. The manager allocates its node buffer from the same number,
    // so it also bounds the node array. Bindings are sized from this rather
    // than from the current build: the binding group is created once and the
    // TLAS is rebuilt underneath it every time the page cut changes.
    uint32_t tlasInstanceCapacity = 0u;
  };

  SparseVoxelTree64DynamicTracePass(
      const Buffers buffers,
      Capacities capacities,
      const Traversal traversal = Traversal::RegionMarcher,
      const ::rendering::Buffer externalDispatch = {},
      const ::rendering::Buffer indirectArgs = {},
      const RayGeneration rayGeneration = RayGeneration::RayBuffer,
      const Query query = Query::ClosestHit)
      : buffers_(buffers), capacities_(capacities), traversal_(traversal),
        externalDispatch_(externalDispatch), indirectArgs_(indirectArgs),
        rayGeneration_(rayGeneration), query_(query)
  {
    if (rayGeneration_ == RayGeneration::PrimaryCamera &&
        externalDispatch_.isValid())
      throw std::invalid_argument(
          "Tree64 dynamic primary-ray trace does not support "
          "external dispatch");
    if (query_ == Query::AnyHit &&
        (rayGeneration_ != RayGeneration::RayBuffer ||
         !externalDispatch_.isValid()))
      throw std::invalid_argument(
          "Tree64 dynamic any-hit trace requires storage dispatch rays");
    const bool materialGeometry = buffers_.materialCells.isValid();
    if (materialGeometry != buffers_.microMaterials.isValid() ||
        materialGeometry != buffers_.geometryPrograms.isValid() ||
        materialGeometry != buffers_.geometryInstructions.isValid() ||
        materialGeometry != buffers_.geometryParameters.isValid() ||
        materialGeometry != buffers_.materialFieldDefaults.isValid() ||
        materialGeometry != buffers_.geometryFieldCache.isValid() ||
        materialGeometry != buffers_.materialCounters.isValid())
      throw std::invalid_argument(
          "Tree64 dynamic material geometry requires all sidecar/program buffers");
    if (materialGeometry &&
        (capacities_.materialCellCount == 0u ||
         capacities_.microMaterialCount == 0u ||
         capacities_.geometryProgramCount == 0u ||
         capacities_.geometryInstructionCount == 0u ||
         capacities_.geometryParameterCount == 0u ||
         capacities_.materialFieldDefaultCount == 0u ||
         capacities_.geometryFieldCacheCount == 0u))
      throw std::invalid_argument(
          "Tree64 dynamic material geometry capacities are invalid");
    const bool terrainPages = buffers_.terrainDescriptors.isValid();
    if (terrainPages != buffers_.terrainNodePageGenerations.isValid() ||
        terrainPages != buffers_.terrainPageNodes.isValid() ||
        terrainPages != buffers_.terrainMaterialPageGenerations.isValid() ||
        terrainPages != buffers_.terrainPageMaterialCells.isValid())
      throw std::invalid_argument(
          "Tree64 terrain descriptor traversal requires every page buffer");
    if (terrainPages &&
        (capacities_.terrainDescriptorCount == 0u ||
         capacities_.terrainPhysicalPageCount == 0u ||
         capacities_.terrainNodesPerPage == 0u ||
         capacities_.terrainMaterialCellsPerPage == 0u))
      throw std::invalid_argument(
          "Tree64 terrain descriptor traversal capacities are invalid");
    if (traversal_ == Traversal::MirroredAncestorTlasBvh4)
    {
      // One shader variant carries TREE64_TLAS_BVH4 today, and it is the
      // storage-dispatch any-hit one the scene ray chain uses. Accepting the
      // enum in any other configuration would load a shader without the
      // bindings and silently fall back to the directory - the broad phase
      // would look selected and not be.
      if (rayGeneration_ != RayGeneration::RayBuffer ||
          !externalDispatch_.isValid() || query_ != Query::AnyHit)
        throw std::invalid_argument(
            "Tree64 BVH4 broad phase is only built for the storage-dispatch "
            "any-hit variant");
      if (!buffers_.tlasBvh4Nodes.isValid() ||
          !buffers_.tlasInstanceSlots.isValid() ||
          capacities_.tlasInstanceCapacity == 0u)
        throw std::invalid_argument(
            "Tree64 BVH4 broad phase requires TLAS nodes, the instance slot map "
            "and an instance capacity");
    }
    capacities_.chunkCount = std::max(1u, capacities_.chunkCount);
    capacities_.staticNodeCount = std::max(1u, capacities_.staticNodeCount);
    capacities_.nodeSlotCapacity = std::max(1u, capacities_.nodeSlotCapacity);
    capacities_.rayCount = std::max(1u, capacities_.rayCount);
    capacities_.membershipCount = std::max(1u, capacities_.membershipCount);
    capacities_.referenceCount = std::max(1u, capacities_.referenceCount);
    capacities_.instanceCount = std::max(1u, capacities_.instanceCount);
    capacities_.modelCount = std::max(1u, capacities_.modelCount);
    capacities_.dynamicNodeCount = std::max(1u, capacities_.dynamicNodeCount);
    if (capacities_.maximumReferencesPerChunk == 0u)
      throw std::invalid_argument("Tree64 dynamic reference limit is invalid");
  }

  ~SparseVoxelTree64DynamicTracePass() override
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
    if (terrainPageStatesFallback_.isValid())
      renderGraph->deleteBuffer(terrainPageStatesFallback_);
    if (terrainDescriptorsFallback_.isValid())
      renderGraph->deleteBuffer(terrainDescriptorsFallback_);
    if (terrainNodePageGenerationsFallback_.isValid())
      renderGraph->deleteBuffer(terrainNodePageGenerationsFallback_);
    if (terrainPageNodesFallback_.isValid())
      renderGraph->deleteBuffer(terrainPageNodesFallback_);
    if (terrainMaterialPageGenerationsFallback_.isValid())
      renderGraph->deleteBuffer(terrainMaterialPageGenerationsFallback_);
    if (terrainPageMaterialCellsFallback_.isValid())
      renderGraph->deleteBuffer(terrainPageMaterialCellsFallback_);
  }

  // Primary-ray variants only. Must describe the same camera the G-buffer uses
  // to reconstruct world positions, or shading lands off the traced surface.
  void updatePrimaryCamera(const SparseVoxelTree64PrimaryCameraGPU &camera)
  {
    primaryCamera_ = camera;
    if (primaryCameraBuffer_.isValid())
      renderGraph->bufferWrite(
          primaryCameraBuffer_, 0u, sizeof(primaryCamera_), &primaryCamera_);
  }

  // Traversal level of detail for every ray this pass traces. Defaults to
  // disabled, which is leaf-exact. Only correct for consumers that tolerate
  // conservative over-occlusion - shadow, occlusion and GI rays - never for the
  // primary rays that resolve the G-buffer.
  void setTraversalLOD(const SparseVoxelTree64TraceLODGPU lod)
  {
    lod_ = lod;
  }

  // Publish the BVH4 broad phase for this frame.
  //
  // A zero count - which is what an unbuilt or a cleared TLAS reports - leaves
  // the clipmap directory march in charge, so a build carrying the BVH4 variant
  // still renders correctly before the first TLAS build and after a failed one.
  // The counts are re-published every frame rather than latched, because the
  // terrain page cut streams and the TLAS is rebuilt underneath us.
  void updateTlas(const uint32_t rootNode, const uint32_t nodeCount, const uint32_t instanceCount)
  {
    const bool usable = traversal_ == Traversal::MirroredAncestorTlasBvh4 &&
        nodeCount != 0u && instanceCount != 0u && rootNode < nodeCount;
    dispatch_.tlasRootNode = usable ? rootNode : 0xffffffffu;
    dispatch_.tlasNodeCount = usable ? nodeCount : 0u;
  }

  bool
  update(const uint32_t rayCount, const uint32_t maximumQueries, const uint32_t modelCount, const uint32_t instanceCount, const uint32_t referenceCount, const uint32_t dynamicNodeCount, const uint32_t dynamicGeneration)
  {
    if (rayCount > capacities_.rayCount || maximumQueries == 0u || modelCount > capacities_.modelCount || instanceCount > capacities_.instanceCount || referenceCount > capacities_.referenceCount ||
        dynamicNodeCount > capacities_.dynamicNodeCount || dynamicGeneration == 0u)
      return false;
    dispatch_.rayCount = rayCount;
    dispatch_.maximumQueries = maximumQueries;
    dispatch_.chunkCapacity = capacities_.chunkCount;
    dispatch_.nodeCapacity = capacities_.staticNodeCount;
    dispatch_.nodeSlotCapacity = capacities_.nodeSlotCapacity;
    dispatch_.modelCount = modelCount;
    dispatch_.instanceCount = instanceCount;
    dispatch_.membershipCapacity = capacities_.membershipCount;
    dispatch_.referenceCount = referenceCount;
    dispatch_.dynamicNodeCount = dynamicNodeCount;
    dispatch_.maximumReferencesPerChunk = capacities_.maximumReferencesPerChunk;
    dispatch_.dynamicGeneration = dynamicGeneration;
    dispatch_.lodWorldSize = std::max(0.0f, lod_.worldSize);
    dispatch_.lodDistanceSlope = std::max(0.0f, lod_.distanceSlope);
    dispatch_.lodStartDistance = std::max(0.0f, lod_.startDistance);
    dispatch_.lodMaximumWorldSize = std::max(0.0f, lod_.maximumWorldSize);
    const ::rendering::Buffer dispatchBuffer =
        externalDispatch_.isValid() ? externalDispatch_ : uniforms_;
    if (dispatchBuffer.isValid())
      renderGraph->bufferWrite(
          dispatchBuffer, 0u, sizeof(dispatch_), &dispatch_);
    return true;
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    const ::rendering::Buffer dispatchBuffer =
        externalDispatch_.isValid() ? externalDispatch_ : uniforms_;
    renderGraph->bufferWrite(
        dispatchBuffer, 0u, sizeof(dispatch_), &dispatch_);
    if (primaryCameraBuffer_.isValid())
      renderGraph->bufferWrite(
          primaryCameraBuffer_, 0u, sizeof(primaryCamera_), &primaryCamera_);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    if (indirectArgs_.isValid())
      commandBuffer.cmdDispatchIndirect(indirectArgs_, 0u);
    else
      commandBuffer.cmdDispatch(
          std::max(1u, (capacities_.rayCount + 63u) / 64u), 1u, 1u);
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
        .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
                 ::rendering::BufferUsage::BufferUsage_Push,
        .frameLocal = true,
      });
    const bool primaryRays = rayGeneration_ == RayGeneration::PrimaryCamera;
    if (primaryRays)
      primaryCameraBuffer_ = renderGraph->createBuffer({
        .name = passName + "_PrimaryCamera",
        .size = sizeof(primaryCamera_),
        .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
                 ::rendering::BufferUsage::BufferUsage_Push,
        .frameLocal = true,
      });
    const ::rendering::Buffer dispatchBuffer =
        externalDispatch_.isValid() ? externalDispatch_ : uniforms_;
    const bool materialGeometry = buffers_.materialCells.isValid();
    if (!buffers_.terrainPageStates.isValid())
    {
      terrainPageStatesFallback_ = renderGraph->createBuffer({
        .name = passName + "_EmptyTerrainPageStates",
        .size = sizeof(SparseVoxelTerrainPageStateGPU),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
                 ::rendering::BufferUsage::BufferUsage_Push,
      });
      const SparseVoxelTerrainPageStateGPU empty{};
      renderGraph->bufferWrite(
          terrainPageStatesFallback_, 0u, sizeof(empty),
          const_cast<SparseVoxelTerrainPageStateGPU *>(&empty));
    }
    const ::rendering::Buffer terrainPageStateBuffer =
        buffers_.terrainPageStates.isValid()
        ? buffers_.terrainPageStates
        : terrainPageStatesFallback_;
    const uint32_t terrainPageStateCount =
        buffers_.terrainPageStates.isValid()
        ? std::max(1u, capacities_.terrainPageStateCount)
        : 1u;
    const bool terrainPages = buffers_.terrainDescriptors.isValid();
    if (!terrainPages)
    {
      const auto fallback = [&](const char *suffix, const uint64_t size)
      {
        return renderGraph->createBuffer({
          .name = passName + suffix,
          .size = std::max<uint64_t>(size, sizeof(uint32_t)),
          .usage = ::rendering::BufferUsage::BufferUsage_Storage |
                   ::rendering::BufferUsage::BufferUsage_Push,
        });
      };
      terrainDescriptorsFallback_ = fallback(
          "_EmptyTerrainDescriptors",
          sizeof(SparseVoxelTerrainTree64DescriptorGPU));
      terrainNodePageGenerationsFallback_ = fallback(
          "_EmptyTerrainNodePageGenerations", sizeof(uint32_t));
      terrainPageNodesFallback_ = fallback(
          "_EmptyTerrainPageNodes",
          query_ == Query::AnyHit
              ? sizeof(SparseVoxelTree64NodePaddedGPU)
              : sizeof(SparseVoxelTree64NodeGPU));
      terrainMaterialPageGenerationsFallback_ = fallback(
          "_EmptyTerrainMaterialPageGenerations", sizeof(uint32_t));
      terrainPageMaterialCellsFallback_ = fallback(
          "_EmptyTerrainPageMaterialCells",
          sizeof(SparseVoxelTree64MaterialCellGPU));
      const SparseVoxelTerrainTree64DescriptorGPU emptyDescriptor{};
      const uint32_t zero = 0u;
      const SparseVoxelTree64NodePaddedGPU emptyNode{};
      const SparseVoxelTree64MaterialCellGPU emptyMaterial{};
      renderGraph->bufferWrite(
          terrainDescriptorsFallback_, 0u, sizeof(emptyDescriptor),
          const_cast<SparseVoxelTerrainTree64DescriptorGPU *>(
              &emptyDescriptor));
      renderGraph->bufferWrite(
          terrainNodePageGenerationsFallback_, 0u, sizeof(zero),
          const_cast<uint32_t *>(&zero));
      renderGraph->bufferWrite(
          terrainPageNodesFallback_, 0u,
          query_ == Query::AnyHit
              ? sizeof(SparseVoxelTree64NodePaddedGPU)
              : sizeof(SparseVoxelTree64NodeGPU),
          const_cast<SparseVoxelTree64NodePaddedGPU *>(&emptyNode));
      renderGraph->bufferWrite(
          terrainMaterialPageGenerationsFallback_, 0u, sizeof(zero),
          const_cast<uint32_t *>(&zero));
      renderGraph->bufferWrite(
          terrainPageMaterialCellsFallback_, 0u, sizeof(emptyMaterial),
          const_cast<SparseVoxelTree64MaterialCellGPU *>(&emptyMaterial));
    }
    const ::rendering::Buffer terrainDescriptors = terrainPages
        ? buffers_.terrainDescriptors : terrainDescriptorsFallback_;
    const ::rendering::Buffer terrainNodePageGenerations = terrainPages
        ? buffers_.terrainNodePageGenerations
        : terrainNodePageGenerationsFallback_;
    const ::rendering::Buffer terrainPageNodes = terrainPages
        ? buffers_.terrainPageNodes : terrainPageNodesFallback_;
    const ::rendering::Buffer terrainMaterialPageGenerations = terrainPages
        ? buffers_.terrainMaterialPageGenerations
        : terrainMaterialPageGenerationsFallback_;
    const ::rendering::Buffer terrainPageMaterialCells = terrainPages
        ? buffers_.terrainPageMaterialCells
        : terrainPageMaterialCellsFallback_;
    const uint32_t terrainDescriptorCount = terrainPages
        ? capacities_.terrainDescriptorCount : 1u;
    const uint32_t terrainPhysicalPageCount = terrainPages
        ? capacities_.terrainPhysicalPageCount : 1u;
    const uint32_t terrainNodesPerPage = terrainPages
        ? capacities_.terrainNodesPerPage : 1u;
    const uint32_t terrainMaterialCellsPerPage = terrainPages
        ? capacities_.terrainMaterialCellsPerPage : 1u;
    // Only the TREE64_TLAS_BVH4 shader declares bindings 30 and 31, so only
    // that variant may carry them in its layout. The constructor already
    // refuses the enum in any other configuration.
    const bool tlasBvh4 = traversal_ == Traversal::MirroredAncestorTlasBvh4;
    const uint64_t tlasInstanceCapacity =
        std::max(1u, capacities_.tlasInstanceCapacity);
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
      storageLayout("dynamicMemberships", 6u),
      storageLayout("dynamicReferences", 7u),
      storageLayout("dynamicInstances", 8u),
      storageLayout("dynamicModels", 9u),
      storageLayout("dynamicNodes", 10u),
      storageLayout("terrainPageStates", 19u),
      storageLayout("terrainTree64Descriptors", 20u),
      storageLayout("terrainTree64NodePageGenerations", 21u),
      storageLayout("terrainTree64PageNodes", 22u),
      storageLayout("terrainTree64MaterialPageGenerations", 23u),
      storageLayout("terrainTree64PageMaterialCells", 24u),
    };
    if (tlasBvh4)
    {
      layoutBuffers.insert(layoutBuffers.end(), {
        storageLayout("tlasBvh4Nodes", 30u),
        storageLayout("tlasInstanceSlots", 31u),
      });
    }
    if (materialGeometry)
    {
      layoutBuffers.insert(layoutBuffers.end(), {
        storageLayout("tree64MaterialCells", 11u),
        storageLayout("tree64MicroMaterials", 12u),
        storageLayout("voxelGeometryPrograms", 13u),
        storageLayout("voxelGeometryInstructions", 14u),
        storageLayout("voxelGeometryParameters", 15u),
        storageLayout("tree64MaterialCounters", 16u),
        storageLayout("voxelMaterialFieldDefaults", 17u),
        storageLayout("voxelGeometryFieldCache", 18u),
      });
    }
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = std::move(layoutBuffers),
      }},
    });
    const auto read = ::rendering::AccessPattern::SHADER_READ;
    std::vector<::rendering::BindingBuffer> groupBuffers{
      {.bufferView = view(buffers_.state, sizeof(SparseVoxelTree64StateGPU), read), .binding = 0u},
      {.bufferView = view(buffers_.chunks, uint64_t{capacities_.chunkCount} * sizeof(SparseVoxelTree64ChunkGPU), read), .binding = 1u},
      {.bufferView = view(buffers_.staticNodes, uint64_t{capacities_.staticNodeCount} * sizeof(SparseVoxelTree64NodeGPU), read), .binding = 2u},
      {.bufferView = primaryRays ? view(primaryCameraBuffer_, sizeof(primaryCamera_), ::rendering::AccessPattern::UNIFORM_READ) : view(buffers_.rays, uint64_t{capacities_.rayCount} * sizeof(SparseVoxelOccupancyDDARayGPU), read), .binding = 3u},
      {.bufferView = view(buffers_.results, uint64_t{capacities_.rayCount} * sizeof(SparseVoxelOccupancyDDAResultGPU), ::rendering::AccessPattern::SHADER_WRITE), .binding = 4u},
      {.bufferView = view(dispatchBuffer, sizeof(dispatch_), externalDispatch_.isValid() ? read : ::rendering::AccessPattern::UNIFORM_READ), .binding = 5u},
      {.bufferView = view(buffers_.memberships, uint64_t{capacities_.membershipCount} * sizeof(SparseVoxelTree64DynamicMembershipGPU), read), .binding = 6u},
      {.bufferView = view(buffers_.references, uint64_t{capacities_.referenceCount} * sizeof(uint32_t), read), .binding = 7u},
      {.bufferView = view(buffers_.instances, uint64_t{capacities_.instanceCount} * sizeof(SparseVoxelTree64DynamicInstanceGPU), read), .binding = 8u},
      {.bufferView = view(buffers_.models, uint64_t{capacities_.modelCount} * sizeof(SparseVoxelTree64DynamicModelGPU), read), .binding = 9u},
      {.bufferView = view(buffers_.dynamicNodes, uint64_t{capacities_.dynamicNodeCount} * (query_ == Query::AnyHit ? sizeof(SparseVoxelTree64NodePaddedGPU) : sizeof(SparseVoxelTree64NodeGPU)), read), .binding = 10u},
      {.bufferView = view(terrainPageStateBuffer, uint64_t{terrainPageStateCount} * sizeof(SparseVoxelTerrainPageStateGPU), read), .binding = 19u},
      {.bufferView = view(terrainDescriptors, uint64_t{terrainDescriptorCount} * sizeof(SparseVoxelTerrainTree64DescriptorGPU), read), .binding = 20u},
      {.bufferView = view(terrainNodePageGenerations, uint64_t{terrainPhysicalPageCount} * sizeof(uint32_t), read), .binding = 21u},
      {.bufferView = view(terrainPageNodes, uint64_t{terrainPhysicalPageCount} * terrainNodesPerPage * (query_ == Query::AnyHit ? sizeof(SparseVoxelTree64NodePaddedGPU) : sizeof(SparseVoxelTree64NodeGPU)), read), .binding = 22u},
      {.bufferView = view(terrainMaterialPageGenerations, uint64_t{terrainPhysicalPageCount} * sizeof(uint32_t), read), .binding = 23u},
      {.bufferView = view(terrainPageMaterialCells, uint64_t{terrainPhysicalPageCount} * terrainMaterialCellsPerPage * sizeof(SparseVoxelTree64MaterialCellGPU), read), .binding = 24u},
    };
    if (tlasBvh4)
    {
      groupBuffers.insert(groupBuffers.end(), {
        {.bufferView = view(buffers_.tlasBvh4Nodes, tlasInstanceCapacity * kTlasBvh4NodeBytes, read), .binding = 30u},
        {.bufferView = view(buffers_.tlasInstanceSlots, tlasInstanceCapacity * sizeof(uint32_t), read), .binding = 31u},
      });
    }
    if (materialGeometry)
    {
      groupBuffers.insert(groupBuffers.end(), {
        {.bufferView = view(buffers_.materialCells, uint64_t{capacities_.materialCellCount} * sizeof(SparseVoxelTree64MaterialCellGPU), read), .binding = 11u},
        {.bufferView = view(buffers_.microMaterials, uint64_t{capacities_.microMaterialCount} * sizeof(SparseVoxelMicroMaterialGPU), read), .binding = 12u},
        {.bufferView = view(buffers_.geometryPrograms, uint64_t{capacities_.geometryProgramCount} * sizeof(material::VoxelGeometryProgramGPU), read), .binding = 13u},
        {.bufferView = view(buffers_.geometryInstructions, uint64_t{capacities_.geometryInstructionCount} * sizeof(material::VoxelGeometryInstructionGPU), read), .binding = 14u},
        {.bufferView = view(buffers_.geometryParameters, uint64_t{capacities_.geometryParameterCount} * sizeof(material::VoxelGeometryParameterBlockGPU), read), .binding = 15u},
        {.bufferView = view(buffers_.materialCounters, sizeof(SparseVoxelTree64MaterialCountersGPU), ::rendering::AccessPattern::SHADER_READ_WRITE), .binding = 16u},
        {.bufferView = view(buffers_.materialFieldDefaults, uint64_t{capacities_.materialFieldDefaultCount} * sizeof(material::VoxelMaterialFieldDefaultsGPU), read), .binding = 17u},
        {.bufferView = view(buffers_.geometryFieldCache, uint64_t{capacities_.geometryFieldCacheCount} * sizeof(SparseVoxelGeometryFieldCacheRecordGPU), read), .binding = 18u},
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
    const bool anyHit = query_ == Query::AnyHit;
    const std::string shaderFile =
        primaryRays
            ? ((traversal_ == Traversal::MirroredAncestor ||
             traversal_ == Traversal::MirroredAncestorTlasBvh4)
                   ? "sparse-voxel-tree64-trace-dynamic-primary-mirrored-ancestor.spirv"
                   : "sparse-voxel-tree64-trace-dynamic-primary.spirv")
        : externalDispatch_.isValid()
            ? ((traversal_ == Traversal::MirroredAncestor ||
             traversal_ == Traversal::MirroredAncestorTlasBvh4)
                   ? (anyHit
                          ? (traversal_ == Traversal::MirroredAncestorTlasBvh4
                                 ? "sparse-voxel-tree64-trace-dynamic-storage-dispatch-mirrored-ancestor-any-hit-tlas-bvh4.spirv"
                                 : "sparse-voxel-tree64-trace-dynamic-storage-dispatch-mirrored-ancestor-any-hit.spirv")
                          : "sparse-voxel-tree64-trace-dynamic-storage-dispatch-mirrored-ancestor.spirv")
                   : (anyHit
                          ? "sparse-voxel-tree64-trace-dynamic-storage-dispatch-any-hit.spirv"
                          : "sparse-voxel-tree64-trace-dynamic-storage-dispatch.spirv"))
            : ((traversal_ == Traversal::MirroredAncestor ||
             traversal_ == Traversal::MirroredAncestorTlasBvh4)
                   ? "sparse-voxel-tree64-trace-dynamic-mirrored-ancestor.spirv"
                   : "sparse-voxel-tree64-trace-dynamic.spirv");
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/" + shaderFile +
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

  Buffers buffers_{};
  Capacities capacities_{};
  Traversal traversal_ = Traversal::RegionMarcher;
  SparseVoxelTree64TraceLODGPU lod_{};
  RayGeneration rayGeneration_ = RayGeneration::RayBuffer;
  Query query_ = Query::ClosestHit;
  SparseVoxelTree64PrimaryCameraGPU primaryCamera_{};
  ::rendering::Buffer primaryCameraBuffer_{};
  ::rendering::Buffer terrainPageStatesFallback_{};
  ::rendering::Buffer terrainDescriptorsFallback_{};
  ::rendering::Buffer terrainNodePageGenerationsFallback_{};
  ::rendering::Buffer terrainPageNodesFallback_{};
  ::rendering::Buffer terrainMaterialPageGenerationsFallback_{};
  ::rendering::Buffer terrainPageMaterialCellsFallback_{};
  SparseVoxelTree64DynamicTraceDispatchGPU dispatch_{};
  ::rendering::Buffer externalDispatch_{};
  ::rendering::Buffer indirectArgs_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
