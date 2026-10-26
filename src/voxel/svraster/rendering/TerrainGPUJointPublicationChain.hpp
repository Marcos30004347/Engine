#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/TerrainGPUJointPublication.hpp"
#include "voxel/svraster/rendering/TerrainGPUJointPublicationPass.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace voxel::svraster::rendering
{

class TerrainGPUJointPublicationChain final
{
public:
  struct SourceBuffers
  {
    ::rendering::Buffer rasterResults;
    ::rendering::Buffer rasterNodes;
    ::rendering::Buffer rasterOccupancy;
    ::rendering::Buffer rasterMaterialWords;
    ::rendering::Buffer rasterMicroSurfaces;
    ::rendering::Buffer rasterChildren;
    ::rendering::Buffer rasterRoots;
    ::rendering::Buffer tree64Results;
    ::rendering::Buffer tree64Nodes;
  };

  struct DestinationBuffers
  {
    ::rendering::Buffer nodes;
    ::rendering::Buffer occupancy;
    ::rendering::Buffer materialWords;
    ::rendering::Buffer microSurfaces;
    ::rendering::Buffer children;
    ::rendering::Buffer roots;
    ::rendering::Buffer tree64Nodes;
    ::rendering::Buffer tree64NodesPadded;
    ::rendering::Buffer tree64Instances;
    ::rendering::Buffer lodGroups;
    ::rendering::Buffer nodeLODGroupIndices;
    ::rendering::Buffer representativeLODGroupIndices;
  };

  TerrainGPUJointPublicationChain(
      ::rendering::RenderGraph &graph,
      uint32_t &passIndex,
      const TerrainGPUJointPublicationCapacities capacities,
      const SourceBuffers sources,
      const DestinationBuffers destinations,
      std::string name = "TerrainGPUBuild.Phase5.Publish")
      : graph_(&graph), capacities_(capacities), name_(std::move(name))
  {
    createBuffers();
    const TerrainGPUJointPublicationPass::Buffers buffers{
      requests_,
      sources.rasterResults,
      sources.rasterNodes,
      sources.rasterOccupancy,
      sources.rasterMaterialWords,
      sources.rasterMicroSurfaces,
      sources.rasterChildren,
      sources.rasterRoots,
      sources.tree64Results,
      sources.tree64Nodes,
      results_,
      records_,
      destinations.nodes,
      destinations.occupancy,
      destinations.materialWords,
      destinations.microSurfaces,
      destinations.children,
      destinations.roots,
      destinations.tree64Nodes,
      destinations.tree64NodesPadded,
      destinations.tree64Instances,
      destinations.lodGroups,
      destinations.nodeLODGroupIndices,
      destinations.representativeLODGroupIndices,
      indirectArgs_,
    };
    passes_[0] = graph_->registerPass<TerrainGPUJointPublicationPass>(name_ + ".Validate", passIndex++, TerrainGPUJointPublicationPass::Stage::Validate, capacities_, buffers);
    passes_[1] = graph_->registerPass<TerrainGPUJointPublicationPass>(name_ + ".Relocate", passIndex++, TerrainGPUJointPublicationPass::Stage::Relocate, capacities_, buffers);
    passes_[2] = graph_->registerPass<TerrainGPUJointPublicationPass>(name_ + ".Finalize", passIndex++, TerrainGPUJointPublicationPass::Stage::Finalize, capacities_, buffers);
  }

  ~TerrainGPUJointPublicationChain()
  {
    for (auto iterator = passes_.rbegin(); iterator != passes_.rend(); ++iterator)
      delete *iterator;
    graph_->deleteBuffer(records_);
    graph_->deleteBuffer(results_);
    graph_->deleteBuffer(requests_);
    graph_->deleteBuffer(indirectArgs_);
  }

  bool update(const std::vector<TerrainGPUJointPublicationRequestGPU> &requests, std::string *error = nullptr)
  {
    if (error != nullptr)
      error->clear();
    if (requests.size() > capacities_.requests)
    {
      if (error != nullptr)
        *error = "GPU terrain publication batch is too large";
      return false;
    }
    for (size_t index = 0u; index < requests.size(); ++index)
    {
      if (!validateTerrainGPUJointPublicationRequest(requests[index], capacities_, error))
        return false;
      if (index != 0u && requests[index - 1u].terrainSlot >= requests[index].terrainSlot)
      {
        if (error != nullptr)
          *error = "GPU terrain publication slots must be strictly increasing";
        return false;
      }
      const auto overlaps = [](const uint64_t aOffset, const uint64_t aCount, const uint64_t bOffset, const uint64_t bCount)
      {
        return aOffset < bOffset + bCount && bOffset < aOffset + aCount;
      };
      for (size_t previous = 0u; previous < index; ++previous)
      {
        const auto &left = requests[previous];
        const auto &right = requests[index];
        if (overlaps(left.nodeOffset, left.nodeCapacity, right.nodeOffset, right.nodeCapacity) || overlaps(left.childOffset, left.childCapacity, right.childOffset, right.childCapacity) ||
            overlaps(left.rootOffset, left.rootCapacity, right.rootOffset, right.rootCapacity) || overlaps(left.materialOffset, left.materialCapacity, right.materialOffset, right.materialCapacity) ||
            left.nodePageSlot == right.nodePageSlot)
        {
          if (error != nullptr)
            *error = "GPU terrain publication destinations overlap";
          return false;
        }
      }
    }
    if (!requests.empty())
    {
      auto upload = requests;
      graph_->bufferWrite(requests_, 0u, upload.size() * sizeof(upload.front()), upload.data());
    }
    const uint32_t count = static_cast<uint32_t>(requests.size());
    uint32_t maximumItems = 0u;
    for (const auto &request : requests)
      maximumItems = std::max({
        maximumItems, request.nodeCapacity, request.childCapacity,
        request.materialCapacity, request.rootCapacity,
        request.tree64NodesPerPage});
    const uint32_t requestGroups = (count + 63u) / 64u;
    const uint32_t relocationGroups = count == 0u
        ? 0u : (maximumItems + 63u) / 64u;
    const std::array<IndirectDispatchArgsGPU, 3u> indirect{
      IndirectDispatchArgsGPU{requestGroups, 1u, 1u, 0u},
      IndirectDispatchArgsGPU{relocationGroups, count, 1u, 0u},
      IndirectDispatchArgsGPU{requestGroups, 1u, 1u, 0u}};
    graph_->bufferWrite(
        indirectArgs_, 0u, sizeof(indirect),
        const_cast<IndirectDispatchArgsGPU *>(indirect.data()));
    for (auto *pass : passes_)
      if (!pass->update(static_cast<uint32_t>(requests.size())))
        return false;
    return true;
  }

  ::rendering::Buffer requests() const
  {
    return requests_;
  }

  ::rendering::Buffer results() const
  {
    return results_;
  }
  ::rendering::Buffer records() const
  {
    return records_;
  }

private:
  ::rendering::Buffer buffer(const std::string &suffix, const uint64_t size, const ::rendering::BufferUsage usage)
  {
    return graph_->createBuffer({
      .name = name_ + suffix,
      .size = size,
      .usage = usage,
    });
  }

  void createBuffers()
  {
    if (capacities_.requests == 0u)
      throw std::invalid_argument("GPU terrain publication capacity is zero");
    requests_ = buffer(".Requests", uint64_t{capacities_.requests} * sizeof(TerrainGPUJointPublicationRequestGPU), ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push);
    results_ = buffer(".Results", uint64_t{capacities_.requests} * sizeof(TerrainGPUJointPublicationResultGPU), ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_CopySrc);
    records_ = buffer(".Records", uint64_t{capacities_.requests} * sizeof(SparseVoxelTerrainPublicationRecordGPU), ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_CopySrc | ::rendering::BufferUsage::BufferUsage_Push);
    indirectArgs_ = graph_->createBuffer({
      .name = name_ + ".IndirectArgs",
      .size = 3u * sizeof(IndirectDispatchArgsGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Indirect |
          ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true});
    const std::array<IndirectDispatchArgsGPU, 3u> idle{
      IndirectDispatchArgsGPU{0u, 1u, 1u, 0u},
      IndirectDispatchArgsGPU{0u, 1u, 1u, 0u},
      IndirectDispatchArgsGPU{0u, 1u, 1u, 0u}};
    graph_->bufferWrite(
        indirectArgs_, 0u, sizeof(idle),
        const_cast<IndirectDispatchArgsGPU *>(idle.data()));
  }

  ::rendering::RenderGraph *graph_ = nullptr;
  TerrainGPUJointPublicationCapacities capacities_{};
  std::string name_;
  std::array<TerrainGPUJointPublicationPass *, 3u> passes_{};
  ::rendering::Buffer requests_{};
  ::rendering::Buffer results_{};
  ::rendering::Buffer records_{};
  ::rendering::Buffer indirectArgs_{};
};

} // namespace voxel::svraster::rendering
