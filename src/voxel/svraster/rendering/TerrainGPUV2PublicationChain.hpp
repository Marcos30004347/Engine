#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2PublicationPass.hpp"

#include <array>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace voxel::svraster::rendering
{

class TerrainGPUV2PublicationChain final
{
public:
  struct PersistentBuffers
  {
    ::rendering::Buffer payloadResults;
    ::rendering::Buffer groups;
    ::rendering::Buffer owners;
  };

  TerrainGPUV2PublicationChain(::rendering::RenderGraph &graph, uint32_t &passIndex, const TerrainGPUV2PublicationCapacities capacities, const PersistentBuffers persistent, std::string name = "TerrainGPUV2.Publication")
      : graph_(&graph), capacities_(capacities), persistent_(persistent), name_(std::move(name))
  {
    if (capacities.requests == 0u || capacities.members == 0u || capacities.payloadResults == 0u || capacities.groups == 0u || capacities.owners == 0u || !persistent.payloadResults.isValid() ||
        !persistent.groups.isValid() || !persistent.owners.isValid())
      throw std::invalid_argument("terrain GPU V2 publication chain is invalid");
    const auto push = ::rendering::BufferUsage::BufferUsage_Push;
    batch_ = storage(".Batch", sizeof(TerrainGPUV2PublicationBatchGPU), push);
    requests_ = storage(".Requests", uint64_t{capacities.requests} * sizeof(TerrainGPUV2PublicationRequestGPU), push);
    members_ = storage(".Members", uint64_t{capacities.members} * sizeof(TerrainGPUV2PublicationMemberGPU), push);
    results_ = storage(".Results", uint64_t{capacities.requests} * sizeof(TerrainGPUV2PublicationResultGPU), ::rendering::BufferUsage::BufferUsage_CopySrc);
    indirectArgs_ = storage(".IndirectArgs", 2u * sizeof(IndirectDispatchArgsGPU), ::rendering::BufferUsage::BufferUsage_Indirect | ::rendering::BufferUsage::BufferUsage_Push);
    const TerrainGPUV2PublicationPass::Buffers buffers{batch_, requests_, members_, persistent.payloadResults, persistent.groups, persistent.owners, results_, indirectArgs_};
    validate_ = graph_->registerPass<TerrainGPUV2PublicationPass>(name_ + ".Validate", passIndex++, TerrainGPUV2PublicationPass::Stage::Validate, capacities_, buffers);
    commit_ = graph_->registerPass<TerrainGPUV2PublicationPass>(name_ + ".Commit", passIndex++, TerrainGPUV2PublicationPass::Stage::Commit, capacities_, buffers);
    update({}, {});
  }

  ~TerrainGPUV2PublicationChain()
  {
    delete commit_;
    delete validate_;
    graph_->deleteBuffer(results_);
    graph_->deleteBuffer(indirectArgs_);
    graph_->deleteBuffer(members_);
    graph_->deleteBuffer(requests_);
    graph_->deleteBuffer(batch_);
  }

  bool update(const std::vector<TerrainGPUV2PublicationRequestGPU> &requests, const std::vector<TerrainGPUV2PublicationMemberGPU> &members)
  {
    if (requests.size() > capacities_.requests || members.size() > capacities_.members || (requests.empty() != members.empty()))
      return false;
    std::set<uint32_t> groups;
    std::vector<std::pair<uint32_t, uint32_t>> ownerRanges;
    for (const auto &request : requests)
    {
      if (request.groupIndex >= capacities_.groups || (request.expectedEpoch & 1u) != 0u || request.memberCount == 0u || request.memberCount > kTerrainGPUV2PublicationMaximumMembers ||
          request.memberOffset > members.size() || request.memberCount > members.size() - request.memberOffset || request.ownerCapacity < request.memberCount || request.ownerOffset > capacities_.owners ||
          request.ownerCapacity > capacities_.owners - request.ownerOffset || request.expectedOwnerOffset > capacities_.owners || request.expectedOwnerCount > capacities_.owners - request.expectedOwnerOffset ||
          request.replacementTerrainGeneration == 0u || (request.replacementPayloadRevisionLow | request.replacementPayloadRevisionHigh) == 0u || !groups.insert(request.groupIndex).second)
        return false;
      const uint32_t end = request.ownerOffset + request.ownerCapacity;
      for (const auto [begin, previousEnd] : ownerRanges)
        if (request.ownerOffset < previousEnd && begin < end)
          return false;
      ownerRanges.emplace_back(request.ownerOffset, end);
      for (uint32_t offset = 0u; offset < request.memberCount; ++offset)
        if (members[request.memberOffset + offset].payloadResultIndex >= capacities_.payloadResults)
          return false;
    }
    TerrainGPUV2PublicationBatchGPU batch{};
    batch.requestCount = static_cast<uint32_t>(requests.size());
    batch.memberCount = static_cast<uint32_t>(members.size());
    batch.payloadResultCount = capacities_.payloadResults;
    batch.groupCount = capacities_.groups;
    batch.ownerCount = capacities_.owners;
    graph_->bufferWrite(batch_, 0u, sizeof(batch), &batch);
    if (!requests.empty())
    {
      graph_->bufferWrite(requests_, 0u, requests.size() * sizeof(requests[0]), const_cast<TerrainGPUV2PublicationRequestGPU *>(requests.data()));
      graph_->bufferWrite(members_, 0u, members.size() * sizeof(members[0]), const_cast<TerrainGPUV2PublicationMemberGPU *>(members.data()));
    }
    const uint32_t dispatchGroups = static_cast<uint32_t>((requests.size() + 63u) / 64u);
    std::array<IndirectDispatchArgsGPU, 2u> indirect{IndirectDispatchArgsGPU{dispatchGroups, 1u, 1u, 0u}, IndirectDispatchArgsGPU{dispatchGroups, 1u, 1u, 0u}};
    graph_->bufferWrite(indirectArgs_, 0u, sizeof(indirect), indirect.data());
    return true;
  }

  ::rendering::Buffer results() const
  {
    return results_;
  }
  ::rendering::Buffer indirectArgs() const
  {
    return indirectArgs_;
  }
  uint32_t passCount() const
  {
    return 2u;
  }
  uint64_t metadataBytes() const
  {
    return sizeof(TerrainGPUV2PublicationBatchGPU) + uint64_t{capacities_.requests} * (sizeof(TerrainGPUV2PublicationRequestGPU) + sizeof(TerrainGPUV2PublicationResultGPU)) +
           uint64_t{capacities_.members} * sizeof(TerrainGPUV2PublicationMemberGPU);
  }

private:
  ::rendering::Buffer storage(const std::string &suffix, const uint64_t size, const ::rendering::BufferUsage extra)
  {
    return graph_->createBuffer({.name = name_ + suffix, .size = size, .usage = ::rendering::BufferUsage::BufferUsage_Storage | extra});
  }

  ::rendering::RenderGraph *graph_ = nullptr;
  TerrainGPUV2PublicationCapacities capacities_{};
  PersistentBuffers persistent_{};
  std::string name_{};
  TerrainGPUV2PublicationPass *validate_ = nullptr;
  TerrainGPUV2PublicationPass *commit_ = nullptr;
  ::rendering::Buffer batch_{};
  ::rendering::Buffer requests_{};
  ::rendering::Buffer members_{};
  ::rendering::Buffer results_{};
  ::rendering::Buffer indirectArgs_{};
};

} // namespace voxel::svraster::rendering
