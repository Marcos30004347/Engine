#pragma once

#include "voxel/svraster/rendering/TerrainGPUColumnBuildChain.hpp"
#include "voxel/svraster/rendering/TerrainGPUJointPublicationChain.hpp"
#include "voxel/svraster/rendering/TerrainGPURasterBuildChain.hpp"
#include "voxel/svraster/rendering/TerrainGPUTree64BuildChain.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace voxel::svraster::rendering
{

// One end-to-end, geometry-readback-free build lane:
//
//   GPU map atlas -> GPU columns -> raster + Tree64 -> joint publication
//
// The CPU writes only immutable page/configuration metadata, the material-id
// lookup, and the final publication request. Generated columns and hierarchy
// payloads never have a CPU representation. The only buffer intended for
// readback is publicationResults(), a small accepted/rejected record used for
// fence retirement and rebuild scheduling.
class TerrainGPUPageBuildChain final
{
public:
  struct Capacities
  {
    uint32_t maximumSide = 0u;
    uint32_t maximumHalo = 0u;
    TerrainGPURasterCapacity raster{};
    TerrainGPUTree64Capacity tree64{};
    TerrainGPUJointPublicationCapacities publication{};
  };

  struct StageMasks
  {
    TerrainGPUColumnBuildChain::StageMask columns =
        TerrainGPUColumnBuildChain::kAllStages;
    TerrainGPURasterBuildChain::StageMask raster =
        TerrainGPURasterBuildChain::kAllStages;
    TerrainGPUTree64BuildChain::StageMask tree64 =
        TerrainGPUTree64BuildChain::kAllStages;
    bool publish = true;
  };

  struct Update
  {
    TerrainGPUColumnConfigGPU columns{};
    TerrainGPURasterConfigGPU raster{};
    TerrainGPUTree64ConfigGPU tree64{};
    std::array<TerrainGPURasterMaterialGPU,
        kTerrainGPURasterMaterialTableSize> materialTable{};
    std::vector<TerrainGPUJointPublicationRequestGPU> publications{};
    StageMasks stages{};
  };

  TerrainGPUPageBuildChain(
      ::rendering::RenderGraph &graph,
      uint32_t &passIndex,
      const TerrainGPUColumnBuildChain::AtlasBuffers atlas,
      const Capacities capacities,
      const TerrainGPUJointPublicationChain::DestinationBuffers destinations,
      std::string name = "TerrainGPUBuild.FullGPU")
      : capacities_(capacities), name_(std::move(name))
  {
    const uint64_t maximumColumns =
        uint64_t{capacities.maximumSide} * capacities.maximumSide;
    if (capacities.maximumSide == 0u || capacities.maximumHalo == 0u ||
        maximumColumns > UINT32_MAX)
      throw std::invalid_argument("full GPU terrain build capacity is invalid");

    columns_ = std::make_unique<TerrainGPUColumnBuildChain>(
        graph, passIndex, atlas, capacities.maximumSide,
        capacities.maximumHalo, name_ + ".Columns");
    raster_ = std::make_unique<TerrainGPURasterBuildChain>(
        graph, passIndex, capacities.raster,
        static_cast<uint32_t>(maximumColumns), name_ + ".Raster",
        columns_->rasterColumns(), columns_->result());
    tree64_ = std::make_unique<TerrainGPUTree64BuildChain>(
        graph, passIndex, capacities.tree64,
        static_cast<uint32_t>(maximumColumns), name_ + ".Tree64",
        columns_->columns());
    publication_ = std::make_unique<TerrainGPUJointPublicationChain>(
        graph, passIndex, capacities.publication,
        TerrainGPUJointPublicationChain::SourceBuffers{
          raster_->result(), raster_->nodes(), raster_->occupancy(),
          raster_->materialWords(), raster_->microSurfaceData(),
          raster_->childIndices(), raster_->rootIndices(), tree64_->result(),
          tree64_->nodes()},
        destinations, name_ + ".Publish");
  }

  ~TerrainGPUPageBuildChain() = default;
  TerrainGPUPageBuildChain(const TerrainGPUPageBuildChain &) = delete;
  TerrainGPUPageBuildChain &operator=(const TerrainGPUPageBuildChain &) = delete;

  bool update(const Update &update, std::string *error = nullptr)
  {
    if (error != nullptr) error->clear();
    if (update.columns.side != update.raster.side ||
        update.columns.side != update.tree64.columnSide ||
        update.columns.expectedTerrainGeneration !=
            update.raster.expectedTerrainGeneration ||
        update.columns.expectedTerrainGeneration !=
            update.tree64.expectedTerrainGeneration)
    {
      if (error != nullptr)
        *error = "full GPU terrain stages disagree on page identity";
      return false;
    }
    if (!columns_->update(update.columns, update.stages.columns) ||
        !raster_->updateGPUColumns(
            update.raster, update.materialTable, update.stages.raster) ||
        !tree64_->updateGPUColumns(update.tree64, update.stages.tree64))
    {
      if (error != nullptr)
        *error = "full GPU terrain stage update exceeds its build lane";
      return false;
    }
    const std::vector<TerrainGPUJointPublicationRequestGPU> noPublications;
    return publication_->update(
        update.stages.publish ? update.publications : noPublications, error);
  }

  ::rendering::Buffer publicationResults() const
  {
    return publication_->results();
  }

  ::rendering::Buffer publicationRecords() const
  {
    return publication_->records();
  }

  ::rendering::Buffer columnResults() const { return columns_->result(); }
  ::rendering::Buffer rasterResults() const { return raster_->result(); }
  ::rendering::Buffer tree64Results() const { return tree64_->result(); }

  uint64_t scratchBytes() const
  {
    return columns_->scratchBytes() + raster_->scratchBytes() +
        tree64_->scratchBytes();
  }

  uint64_t hiddenOutputBytes() const
  {
    return columns_->outputBytes() + raster_->outputBytes() +
        tree64_->outputBytes();
  }

private:
  Capacities capacities_{};
  std::string name_;
  // Destruction order is the reverse of construction because publication
  // borrows all preceding stage outputs.
  std::unique_ptr<TerrainGPUColumnBuildChain> columns_{};
  std::unique_ptr<TerrainGPURasterBuildChain> raster_{};
  std::unique_ptr<TerrainGPUTree64BuildChain> tree64_{};
  std::unique_ptr<TerrainGPUJointPublicationChain> publication_{};
};

} // namespace voxel::svraster::rendering
