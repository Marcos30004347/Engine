#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/TerrainGPUV2Publication.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2PublicationChain.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace svr = voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

template <typename T> std::vector<T> read(::rendering::RenderGraph &graph, const ::rendering::Buffer buffer, const uint32_t count)
{
  std::vector<T> output(count);
  graph.bufferRead(
      buffer,
      0u,
      uint64_t{count} * sizeof(T),
      [&](const void *data)
      {
        std::memcpy(output.data(), data, output.size() * sizeof(T));
      });
  return output;
}

svr::TerrainGPUV2CoverageBox box(const int32_t x0, const int32_t y0, const int32_t z0, const int32_t x1, const int32_t y1, const int32_t z1)
{
  return {{x0, y0, z0}, {x1, y1, z1}};
}

svr::TerrainGPUChunkKey key(const int32_t x, const int32_t y, const int32_t z, const uint16_t lod, const uint32_t generation)
{
  return {91u, x, y, z, lod, 7u, static_cast<uint32_t>(20 + x + 2 * y + 4 * z), generation};
}

svr::TerrainGPUV2PayloadResultGPU payload(const uint32_t generation, const uint64_t revision)
{
  svr::TerrainGPUV2PayloadResultGPU value{};
  value.status = static_cast<uint32_t>(svr::TerrainGPUV2PublicationStatus::Approved);
  value.flags = svr::TerrainGPUV2PayloadOccupancyOnly | svr::TerrainGPUV2PayloadNoMicroGeometry;
  value.rasterNodeCount = 4u;
  value.rasterMaterialCount = 31u;
  value.rasterRootCount = 1u;
  value.rasterOccupiedVoxelCount = 31u;
  value.terrainGeneration = generation;
  value.rasterGeneratorRevision = 1u;
  value.treeNodeCount = 9u;
  value.treeVisitedNodeCount = 9u;
  value.treeOccupiedVoxelCount = 31u;
  value.treeGeneratorRevision = 1u;
  value.geometryRevisionLow = static_cast<uint32_t>(revision);
  value.geometryRevisionHigh = static_cast<uint32_t>(revision >> 32u);
  return value;
}

svr::TerrainGPUV2PublicationMemberGPU member(const svr::TerrainGPUChunkKey &chunk, const svr::TerrainGPUV2CoverageBox coverage, const uint32_t payloadIndex, const uint64_t lease)
{
  svr::TerrainGPUV2PublicationMemberGPU value{};
  value.key = svr::terrainGPUChunkKeyGPU(chunk);
  value.coverage = coverage;
  value.payloadResultIndex = payloadIndex;
  value.leaseId = lease;
  value.leaseOffset = lease * 4096u;
  value.leaseBytes = 4096u;
  return value;
}

struct Capture
{
  svr::TerrainGPUV2PublicationResultGPU result{};
  svr::TerrainGPUV2OwnershipGroupStateGPU group{};
  std::vector<svr::TerrainGPUV2PublishedOwnerGPU> owners{};
};

bool equals(
    const Capture &actual,
    const std::vector<svr::TerrainGPUV2PublicationResultGPU> &results,
    const std::vector<svr::TerrainGPUV2OwnershipGroupStateGPU> &groups,
    const std::vector<svr::TerrainGPUV2PublishedOwnerGPU> &owners)
{
  return results.size() == 1u && std::memcmp(&actual.result, &results[0], sizeof(actual.result)) == 0 && std::memcmp(&actual.group, &groups[0], sizeof(actual.group)) == 0 && actual.owners.size() == owners.size() &&
         std::memcmp(actual.owners.data(), owners.data(), owners.size() * sizeof(owners[0])) == 0;
}

void testPublicationGPU(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  using namespace ::rendering;
  constexpr uint64_t coarseRevision = 0x1122334455667788ull;
  constexpr uint64_t fineRevision = 0x8877665544332211ull;
  const svr::TerrainGPUV2PublicationCapacities capacities{1u, 8u, 8u, 1u, 16u};
  std::vector<svr::TerrainGPUV2OwnershipGroupStateGPU> cpuGroups(1u);
  // A 2^33-volume group exercises the shader's checked 64-bit coverage math,
  // rather than allowing a 32-bit volume comparison to accept a wrapped sum.
  cpuGroups[0].coverage = box(0, 0, 0, 65536, 65536, 2);
  std::vector<svr::TerrainGPUV2PublishedOwnerGPU> cpuOwners(capacities.owners);
  std::vector<svr::TerrainGPUV2PublicationResultGPU> cpuResults;
  std::vector<svr::TerrainGPUV2PayloadResultGPU> payloads(capacities.payloadResults);

  RenderGraph graph(&rhi);
  const auto persistentUsage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc;
  const Buffer payloadBuffer = graph.createBuffer({.name = "TerrainGPUV2.P7.Payloads", .size = uint64_t{capacities.payloadResults} * sizeof(payloads[0]), .usage = persistentUsage});
  const Buffer groupBuffer = graph.createBuffer({.name = "TerrainGPUV2.P7.Groups", .size = uint64_t{capacities.groups} * sizeof(cpuGroups[0]), .usage = persistentUsage});
  const Buffer ownerBuffer = graph.createBuffer({.name = "TerrainGPUV2.P7.Owners", .size = uint64_t{capacities.owners} * sizeof(cpuOwners[0]), .usage = persistentUsage});
  graph.bufferWrite(groupBuffer, 0u, sizeof(cpuGroups[0]), cpuGroups.data());
  graph.bufferWrite(ownerBuffer, 0u, cpuOwners.size() * sizeof(cpuOwners[0]), cpuOwners.data());

  const auto pull = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst;
  const Buffer resultReadback = graph.createBuffer({.name = "TerrainGPUV2.P7.ResultReadback", .size = sizeof(svr::TerrainGPUV2PublicationResultGPU), .usage = pull});
  const Buffer groupReadback = graph.createBuffer({.name = "TerrainGPUV2.P7.GroupReadback", .size = sizeof(svr::TerrainGPUV2OwnershipGroupStateGPU), .usage = pull});
  const Buffer ownerReadback = graph.createBuffer({.name = "TerrainGPUV2.P7.OwnerReadback", .size = uint64_t{capacities.owners} * sizeof(svr::TerrainGPUV2PublishedOwnerGPU), .usage = pull});

  uint32_t passIndex = 0u;
  {
    svrRendering::TerrainGPUV2PublicationChain chain(graph, passIndex, capacities, {payloadBuffer, groupBuffer, ownerBuffer});
    std::vector<Pass *> copies;
    copies.push_back(graph.registerPass<gpgpu::CopyBufferPass>(
        "TerrainGPUV2.P7.CopyResult", passIndex++, chain.results(), 0u, sizeof(svr::TerrainGPUV2PublicationResultGPU), resultReadback, 0u, sizeof(svr::TerrainGPUV2PublicationResultGPU)));
    copies.push_back(graph.registerPass<gpgpu::CopyBufferPass>(
        "TerrainGPUV2.P7.CopyGroup", passIndex++, groupBuffer, 0u, sizeof(svr::TerrainGPUV2OwnershipGroupStateGPU), groupReadback, 0u, sizeof(svr::TerrainGPUV2OwnershipGroupStateGPU)));
    copies.push_back(graph.registerPass<gpgpu::CopyBufferPass>(
        "TerrainGPUV2.P7.CopyOwners",
        passIndex++,
        ownerBuffer,
        0u,
        uint64_t{capacities.owners} * sizeof(svr::TerrainGPUV2PublishedOwnerGPU),
        ownerReadback,
        0u,
        uint64_t{capacities.owners} * sizeof(svr::TerrainGPUV2PublishedOwnerGPU)));
    check(chain.passCount() == 2u && chain.metadataBytes() != 0u, "publication is a bounded validate/commit GPU transaction");
    graph.compile();

    const auto run = [&]()
    {
      RenderGraph::Frame frame{};
      RenderGraph::Overrides overrides{};
      graph.run(frame, overrides);
      graph.waitFrame(frame);
      Capture capture{};
      capture.result = read<svr::TerrainGPUV2PublicationResultGPU>(graph, resultReadback, 1u).front();
      capture.group = read<svr::TerrainGPUV2OwnershipGroupStateGPU>(graph, groupReadback, 1u).front();
      capture.owners = read<svr::TerrainGPUV2PublishedOwnerGPU>(graph, ownerReadback, capacities.owners);
      return capture;
    };
    const auto submit = [&](const std::vector<svr::TerrainGPUV2PublicationRequestGPU> &requests, const std::vector<svr::TerrainGPUV2PublicationMemberGPU> &members)
    {
      graph.bufferWrite(payloadBuffer, 0u, payloads.size() * sizeof(payloads[0]), payloads.data());
      check(chain.update(requests, members), "GPU publication chain accepts a structurally bounded ownership batch");
      std::string error;
      const bool oracle = svr::applyTerrainGPUV2PublicationOracle(capacities, requests, members, payloads, cpuGroups, cpuOwners, cpuResults, &error);
      check(oracle, "CPU publication oracle accepts the same ownership batch: " + error);
      const Capture capture = run();
      check(oracle && equals(capture, cpuResults, cpuGroups, cpuOwners), "GPU publication byte-matches the generation/revision CAS oracle");
      return capture;
    };

    payloads[0] = payload(3u, coarseRevision);
    std::vector<svr::TerrainGPUV2PublicationMemberGPU> coarseMembers{member(key(0, 0, 0, 1u, 3u), cpuGroups[0].coverage, 0u, 1u)};
    svr::TerrainGPUV2PublicationRequestGPU coarse{};
    coarse.memberCount = 1u;
    coarse.ownerCapacity = 1u;
    coarse.replacementTerrainGeneration = 3u;
    coarse.replacementPayloadRevisionLow = static_cast<uint32_t>(coarseRevision);
    coarse.replacementPayloadRevisionHigh = static_cast<uint32_t>(coarseRevision >> 32u);
    const auto coarseCapture = submit({coarse}, coarseMembers);
    check(coarseCapture.result.status == static_cast<uint32_t>(svr::TerrainGPUV2PublicationStatus::Committed) && coarseCapture.group.epoch == 2u, "one complete coarse owner becomes visible at an even epoch");

    std::vector<svr::TerrainGPUV2PublicationMemberGPU> fineMembers;
    for (uint32_t octant = 0u; octant < 8u; ++octant)
    {
      const int32_t x = static_cast<int32_t>(octant & 1u);
      const int32_t y = static_cast<int32_t>((octant >> 1u) & 1u);
      const int32_t z = static_cast<int32_t>((octant >> 2u) & 1u);
      payloads[octant] = payload(4u, fineRevision);
      fineMembers.push_back(member(key(x, y, z, 0u, 4u), box(x * 32768, y * 32768, z, x * 32768 + 32768, y * 32768 + 32768, z + 1), octant, 2u + octant));
    }
    svr::TerrainGPUV2PublicationRequestGPU fine{};
    fine.expectedEpoch = 2u;
    fine.expectedTerrainGeneration = 3u;
    fine.expectedPayloadRevisionLow = static_cast<uint32_t>(coarseRevision);
    fine.expectedPayloadRevisionHigh = static_cast<uint32_t>(coarseRevision >> 32u);
    fine.replacementTerrainGeneration = 4u;
    fine.replacementPayloadRevisionLow = static_cast<uint32_t>(fineRevision);
    fine.replacementPayloadRevisionHigh = static_cast<uint32_t>(fineRevision >> 32u);
    fine.memberCount = 8u;
    fine.ownerOffset = 8u;
    fine.ownerCapacity = 8u;
    fine.expectedOwnerCount = 1u;

    payloads[7].status = 0u;
    const auto interrupted = submit({fine}, fineMembers);
    check(
        interrupted.result.status == static_cast<uint32_t>(svr::TerrainGPUV2PublicationStatus::Rejected) && interrupted.group.epoch == 2u && interrupted.group.ownerCount == 1u,
        "an interrupted eight-child build retains the complete coarse ownership group");

    payloads[7] = payload(4u, fineRevision);
    const auto refined = submit({fine}, fineMembers);
    check(
        refined.result.status == static_cast<uint32_t>(svr::TerrainGPUV2PublicationStatus::Committed) && refined.group.epoch == 4u && refined.group.ownerOffset == 8u && refined.group.ownerCount == 8u,
        "eight non-overlapping 3D children replace their coarse owner in one commit");

    const auto stale = submit({fine}, fineMembers);
    check(
        stale.result.rejectionReason == static_cast<uint32_t>(svr::TerrainGPUV2PublicationRejection::StaleOwner) && stale.group.epoch == 4u && stale.group.ownerCount == 8u,
        "a stale GPU CAS cannot overwrite the newer complete owner set");

    for (auto iterator = copies.rbegin(); iterator != copies.rend(); ++iterator)
      delete *iterator;
  }

  graph.deleteBuffer(ownerReadback);
  graph.deleteBuffer(groupReadback);
  graph.deleteBuffer(resultReadback);
  graph.deleteBuffer(ownerBuffer);
  graph.deleteBuffer(groupBuffer);
  graph.deleteBuffer(payloadBuffer);
}
} // namespace

int main()
{
  os::Logger::start();
  ::rendering::DeviceRequiredLimits limits{.minimumMemory = 0u, .minimumComputeSharedMemory = 4096u, .minimumComputeWorkGroupInvocations = 64u};
  ::rendering::backend::vulkan::VulkanRHI rhi(::rendering::backend::vulkan::Vulkan_1_2, limits, ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  testPublicationGPU(rhi);
  os::Logger::shutdown();
  std::cout << (failures == 0 ? "GPU terrain V2 Phase 7 tests passed\n" : "GPU terrain V2 Phase 7 tests failed\n");
  return failures == 0 ? 0 : 1;
}
