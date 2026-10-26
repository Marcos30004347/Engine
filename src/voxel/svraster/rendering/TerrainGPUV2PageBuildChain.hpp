#pragma once

#include "voxel/svraster/rendering/TerrainGPUJointPublicationChain.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2ColumnChain.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2FineRasterChain.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2RasterHierarchyChain.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2RendererAdapterPass.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2Tree64Chain.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace voxel::svraster::rendering
{

// Persistent renderer bridge for a bounded GPU-v2 page batch. Every required
// stage is registered once and uses workgroup_id.z to select an immutable lane.
// The shipping leaf-only raster path deliberately
// does not register the legacy parent hierarchy: merely disabling its commands
// still lets cached render-graph command buffers create Metal encoders.
// Updates only replace configuration and indirect arguments; generated payload
// stays on the GPU and the final publication relocates it into stable arenas.
class TerrainGPUV2PageBuildChain final
{
public:
  struct Capacities
  {
    TerrainGPUV2ColumnChain::Capacities columns{};
    TerrainGPUV2FineRasterConfigGPU fineRaster{};
    TerrainGPUV2RasterHierarchyConfigGPU hierarchy{};
    TerrainGPUV2Tree64ConfigGPU tree64{};
    TerrainGPUJointPublicationCapacities publication{};
    uint32_t workgroup = 128u;
    uint32_t lanes = 1u;
  };

  struct StageMasks
  {
    uint32_t columns = 1u;
    uint32_t raster = 1u;
    uint32_t hierarchy = 1u;
    uint32_t tree64 = 1u;
    bool adapt = true;
    bool publish = true;
  };

  struct Update
  {
    TerrainGPUV2ColumnConfigGPU columns{};
    TerrainGPUV2FineRasterConfigGPU raster{};
    TerrainGPUV2RasterHierarchyConfigGPU hierarchy{};
    TerrainGPUV2Tree64ConfigGPU tree64{};
    std::vector<TerrainGPUJointPublicationRequestGPU> publications{};
    StageMasks stages{};
  };

  TerrainGPUV2PageBuildChain(
      ::rendering::RenderGraph &graph,
      uint32_t &passIndex,
      const TerrainGPUV2ColumnChain::AtlasBuffers atlas,
      const Capacities capacities,
      const TerrainGPUJointPublicationChain::DestinationBuffers destinations,
      const bool leafOnlyRaster = false,
      std::string name = "TerrainGPUV2.Renderer")
      : graph_(&graph), capacities_(capacities), leafOnlyRaster_(leafOnlyRaster), name_(std::move(name))
  {
    if (capacities.fineRaster.nodeCapacity == 0u || capacities.fineRaster.materialCapacity == 0u || (!leafOnlyRaster && capacities.hierarchy.nodeCapacity == 0u) || capacities.tree64.nodeCapacity == 0u ||
        capacities.publication.requests == 0u || capacities.lanes == 0u || capacities.lanes > capacities.publication.requests)
      throw std::invalid_argument("terrain GPU V2 renderer lane capacity is invalid");

    auto columnCapacities = capacities.columns;
    columnCapacities.lanes = capacities.lanes;
    columns_ = std::make_unique<TerrainGPUV2ColumnChain>(graph, passIndex, atlas, columnCapacities, name_ + ".Columns");
    fine_ = std::make_unique<TerrainGPUV2FineRasterChain>(
        graph, passIndex, TerrainGPUV2FineRasterChain::ColumnBuffers{columns_->columns(), columns_->result()}, capacities.fineRaster, capacities.workgroup, capacities.lanes, name_ + ".FineRaster");
    if (!leafOnlyRaster_)
    {
      hierarchy_ = std::make_unique<TerrainGPUV2RasterHierarchyChain>(
          graph,
          passIndex,
          TerrainGPUV2RasterHierarchyChain::LeafBuffers{
              fine_->nodes(),
              fine_->occupancy(),
              fine_->materialWords(),
              fine_->result(),
              columns_->heights()},
          capacities.hierarchy,
          capacities.workgroup,
          capacities.lanes,
          name_ + ".Hierarchy");
    }
    tree_ = std::make_unique<TerrainGPUV2Tree64Chain>(
        graph, passIndex, TerrainGPUV2Tree64Chain::RasterBuffers{fine_->nodes(), fine_->occupancy(), fine_->result()}, capacities.tree64, capacities.workgroup, capacities.lanes, name_ + ".Tree64");

    const auto storage = ::rendering::BufferUsage::BufferUsage_Storage;
    const auto copy = ::rendering::BufferUsage::BufferUsage_CopySrc;
    rasterCompatibilityResult_ = graph.createBuffer({.name = name_ + ".RasterCompatibilityResult", .size = uint64_t{capacities.lanes} * sizeof(TerrainGPURasterResultGPU), .usage = storage | copy});
    treeCompatibilityResult_ = graph.createBuffer({.name = name_ + ".TreeCompatibilityResult", .size = uint64_t{capacities.lanes} * sizeof(TerrainGPUTree64ResultGPU), .usage = storage | copy});
    dummyHierarchyResult_ = graph.createBuffer({.name = name_ + ".NoRasterHierarchyResult", .size = uint64_t{capacities.lanes} * sizeof(TerrainGPUV2RasterHierarchyResultGPU), .usage = storage});
    const uint64_t rasterMaterialSourceCapacity64 = leafOnlyRaster_ ? uint64_t{capacities.lanes} * capacities.fineRaster.materialWordCapacity * 4u : uint64_t{capacities.lanes} * capacities.hierarchy.materialCapacity;
    if (rasterMaterialSourceCapacity64 > std::numeric_limits<uint32_t>::max())
      throw std::invalid_argument("terrain GPU V2 material lane arena exceeds the publication ABI");
    const uint32_t rasterMaterialSourceCapacity = static_cast<uint32_t>(rasterMaterialSourceCapacity64);
    dummyMicroSurfaces_ = graph.createBuffer({.name = name_ + ".NoMicroSurfaces", .size = uint64_t{rasterMaterialSourceCapacity} * sizeof(uint64_t), .usage = storage});
    dummyChildren_ = graph.createBuffer({.name = name_ + ".NoChildren", .size = uint64_t{capacities.lanes} * sizeof(uint32_t), .usage = storage});
    adapterIndirect_ = graph.createBuffer(
        {.name = name_ + ".AdapterIndirect", .size = sizeof(IndirectDispatchArgsGPU), .usage = storage | ::rendering::BufferUsage::BufferUsage_Indirect | ::rendering::BufferUsage::BufferUsage_Push, .frameLocal = true});
    const IndirectDispatchArgsGPU idle{0u, 1u, 1u, 0u};
    graph.bufferWrite(adapterIndirect_, 0u, sizeof(idle), const_cast<IndirectDispatchArgsGPU *>(&idle));
    adapter_ = graph.registerPass<TerrainGPUV2RendererAdapterPass>(
        name_ + ".AdaptRendererABI",
        passIndex++,
        TerrainGPUV2RendererAdapterPass::Buffers{fine_->result(), leafOnlyRaster_ ? dummyHierarchyResult_ : hierarchy_->result(), tree_->result(), rasterCompatibilityResult_, treeCompatibilityResult_, adapterIndirect_},
        leafOnlyRaster_,
        capacities.lanes);

    auto publicationCapacities = capacities.publication;
    publicationCapacities.rasterResults = capacities.lanes;
    publicationCapacities.rasterNodes = leafOnlyRaster_ ? capacities.lanes * capacities.fineRaster.nodeCapacity : capacities.lanes * capacities.hierarchy.nodeCapacity;
    publicationCapacities.rasterChildren = leafOnlyRaster_ ? capacities.lanes : capacities.lanes * capacities.hierarchy.childCapacity;
    publicationCapacities.rasterMaterials = leafOnlyRaster_ ? rasterMaterialSourceCapacity : capacities.lanes * capacities.hierarchy.materialCapacity;
    publicationCapacities.rasterRoots = leafOnlyRaster_ ? capacities.lanes * capacities.fineRaster.nodeCapacity : capacities.lanes * capacities.hierarchy.rootCapacity;
    publicationCapacities.tree64Results = capacities.lanes;
    publicationCapacities.tree64Nodes = capacities.lanes * capacities.tree64.nodeCapacity;
    publication_ = std::make_unique<TerrainGPUJointPublicationChain>(
        graph,
        passIndex,
        publicationCapacities,
        TerrainGPUJointPublicationChain::SourceBuffers{
          rasterCompatibilityResult_,
          leafOnlyRaster_ ? fine_->nodes() : hierarchy_->nodes(),
          leafOnlyRaster_ ? fine_->occupancy() : hierarchy_->occupancy(),
          leafOnlyRaster_ ? fine_->materialWords() : hierarchy_->materialWords(),
          dummyMicroSurfaces_,
          leafOnlyRaster_ ? dummyChildren_ : hierarchy_->childIndices(),
          leafOnlyRaster_ ? fine_->rootIndices() : hierarchy_->rootIndices(),
          treeCompatibilityResult_,
          tree_->nodes()},
        destinations,
        name_ + ".Publish");
  }

  ~TerrainGPUV2PageBuildChain()
  {
    publication_.reset();
    delete adapter_;
    graph_->deleteBuffer(adapterIndirect_);
    graph_->deleteBuffer(dummyChildren_);
    graph_->deleteBuffer(dummyMicroSurfaces_);
    graph_->deleteBuffer(dummyHierarchyResult_);
    graph_->deleteBuffer(treeCompatibilityResult_);
    graph_->deleteBuffer(rasterCompatibilityResult_);
    tree_.reset();
    hierarchy_.reset();
    fine_.reset();
    columns_.reset();
  }

  bool update(const Update &update, std::string *error = nullptr)
  {
    return updateBatch(std::vector<Update>{update}, error);
  }

  bool updateBatch(const std::vector<Update> &updates, std::string *error = nullptr)
  {
    if (error != nullptr)
      error->clear();
    if (updates.empty() || updates.size() > capacities_.lanes)
    {
      if (error != nullptr)
        *error = "terrain GPU V2 batch lane count is invalid";
      return false;
    }
    const auto &stages = updates.front().stages;
    for (const auto &update : updates)
      if (update.stages.columns != stages.columns || update.stages.raster != stages.raster || update.stages.hierarchy != stages.hierarchy || update.stages.tree64 != stages.tree64 || update.stages.adapt != stages.adapt ||
          update.stages.publish != stages.publish)
      {
        if (error != nullptr)
          *error = "terrain GPU V2 batch stages must be uniform";
        return false;
      }
    const bool columnsEnabled = stages.columns != 0u;
    const bool rasterEnabled = stages.raster != 0u;
    const bool hierarchyEnabled = !leafOnlyRaster_ && stages.hierarchy != 0u;
    const bool treeEnabled = stages.tree64 != 0u;
    columns_->setEnabled(columnsEnabled);
    fine_->setEnabled(rasterEnabled);
    if (hierarchy_)
      hierarchy_->setEnabled(hierarchyEnabled);
    tree_->setEnabled(treeEnabled);
    const bool adapterEnabled = stages.adapt;
    const IndirectDispatchArgsGPU adapterArgs{adapterEnabled ? 1u : 0u, 1u, static_cast<uint32_t>(updates.size()), 0u};
    graph_->bufferWrite(adapterIndirect_, 0u, sizeof(adapterArgs), const_cast<IndirectDispatchArgsGPU *>(&adapterArgs));
    adapter_->setEnabled(adapterEnabled);
    std::vector<TerrainGPUV2ColumnConfigGPU> columnConfigs;
    std::vector<TerrainGPUV2FineRasterConfigGPU> rasterConfigs;
    std::vector<TerrainGPUV2Tree64ConfigGPU> treeConfigs;
    std::vector<TerrainGPUV2RasterHierarchyConfigGPU> hierarchyConfigs;
    columnConfigs.reserve(updates.size());
    rasterConfigs.reserve(updates.size());
    treeConfigs.reserve(updates.size());
    hierarchyConfigs.reserve(updates.size());
    for (const auto &update : updates)
    {
      columnConfigs.push_back(update.columns);
      rasterConfigs.push_back(update.raster);
      hierarchyConfigs.push_back(update.hierarchy);
      treeConfigs.push_back(update.tree64);
    }
    if ((columnsEnabled && !columns_->updateBatch(columnConfigs)) || (rasterEnabled && !fine_->updateBatch(rasterConfigs)) || (hierarchyEnabled && !hierarchy_->updateBatch(hierarchyConfigs)) ||
        (treeEnabled && !tree_->updateBatch(treeConfigs)))
    {
      if (error != nullptr)
        *error = "terrain GPU V2 update exceeds the persistent renderer lane";
      return false;
    }
    std::vector<TerrainGPUJointPublicationRequestGPU> publications;
    if (stages.publish)
      for (uint32_t lane = 0u; lane < updates.size(); ++lane)
        for (auto publication : updates[lane].publications)
        {
          publication.rasterResultIndex = lane;
          publication.tree64ResultIndex = lane;
          publication.rasterNodeSourceOffset += lane * (leafOnlyRaster_ ? capacities_.fineRaster.nodeCapacity : capacities_.hierarchy.nodeCapacity);
          publication.rasterChildSourceOffset += lane * (leafOnlyRaster_ ? 1u : capacities_.hierarchy.childCapacity);
          publication.rasterRootSourceOffset += lane * (leafOnlyRaster_ ? capacities_.fineRaster.nodeCapacity : capacities_.hierarchy.rootCapacity);
          publication.rasterMaterialSourceOffset += lane * (leafOnlyRaster_ ? capacities_.fineRaster.materialWordCapacity * 4u : capacities_.hierarchy.materialCapacity);
          publication.tree64NodeSourceOffset += lane * capacities_.tree64.nodeCapacity;
          publications.push_back(publication);
        }
    std::sort(
        publications.begin(),
        publications.end(),
        [](const auto &left, const auto &right)
        {
          return left.terrainSlot < right.terrainSlot;
        });
    return publication_->update(publications, error);
  }

  ::rendering::Buffer publicationResults() const
  {
    return publication_->results();
  }
  ::rendering::Buffer publicationRecords() const
  {
    return publication_->records();
  }
  ::rendering::Buffer columnResults() const
  {
    return columns_->result();
  }
  ::rendering::Buffer rasterResults() const
  {
    return rasterCompatibilityResult_;
  }
  ::rendering::Buffer fineRasterResults() const
  {
    return fine_->result();
  }
  ::rendering::Buffer fineRasterNodes() const
  {
    return fine_->nodes();
  }
  ::rendering::Buffer fineRasterOccupancy() const
  {
    return fine_->occupancy();
  }
  ::rendering::Buffer fineRasterMaterialWords() const
  {
    return fine_->materialWords();
  }
  ::rendering::Buffer fineRasterRootIndices() const
  {
    return fine_->rootIndices();
  }
  ::rendering::Buffer tree64Results() const
  {
    return treeCompatibilityResult_;
  }

  uint64_t scratchBytes() const
  {
    return columns_->scratchBytes() + fine_->scratchBytes() + (hierarchy_ ? hierarchy_->scratchBytes() : 0u) + tree_->scratchBytes();
  }
  uint64_t hiddenOutputBytes() const
  {
    return columns_->outputBytes() + fine_->outputBytes() + (hierarchy_ ? hierarchy_->outputBytes() : 0u) + tree_->outputBytes();
  }

private:
  ::rendering::RenderGraph *graph_ = nullptr;
  Capacities capacities_{};
  bool leafOnlyRaster_ = false;
  std::string name_{};
  std::unique_ptr<TerrainGPUV2ColumnChain> columns_{};
  std::unique_ptr<TerrainGPUV2FineRasterChain> fine_{};
  std::unique_ptr<TerrainGPUV2RasterHierarchyChain> hierarchy_{};
  std::unique_ptr<TerrainGPUV2Tree64Chain> tree_{};
  TerrainGPUV2RendererAdapterPass *adapter_ = nullptr;
  std::unique_ptr<TerrainGPUJointPublicationChain> publication_{};
  ::rendering::Buffer rasterCompatibilityResult_{};
  ::rendering::Buffer treeCompatibilityResult_{};
  ::rendering::Buffer dummyHierarchyResult_{};
  ::rendering::Buffer dummyMicroSurfaces_{};
  ::rendering::Buffer dummyChildren_{};
  ::rendering::Buffer adapterIndirect_{};
};

} // namespace voxel::svraster::rendering
