#include "voxel/svraster/TerrainGPUV2Publication.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace svr = voxel::svraster;

namespace
{
int failures = 0;
void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
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

void abiAndAtomicOracle()
{
  check(
      sizeof(svr::TerrainGPUV2PublicationMemberGPU) == 96u && sizeof(svr::TerrainGPUV2PublishedOwnerGPU) == 96u && sizeof(svr::TerrainGPUV2OwnershipGroupStateGPU) == 64u &&
          sizeof(svr::TerrainGPUV2PublicationRequestGPU) == 64u && sizeof(svr::TerrainGPUV2PublicationBatchGPU) == 32u,
      "Phase 7 publication ABIs are frozen");
  constexpr uint64_t coarseRevision = 0x1122334455667788ull;
  constexpr uint64_t fineRevision = 0x8877665544332211ull;
  const svr::TerrainGPUV2PublicationCapacities capacities{2u, 9u, 9u, 1u, 16u};
  std::vector<svr::TerrainGPUV2OwnershipGroupStateGPU> groups(1u);
  groups[0].coverage = box(0, 0, 0, 16, 16, 16);
  std::vector<svr::TerrainGPUV2PublishedOwnerGPU> owners(16u);
  std::vector<svr::TerrainGPUV2PayloadResultGPU> payloads{payload(3u, coarseRevision)};
  std::vector<svr::TerrainGPUV2PublicationMemberGPU> members{member(key(0, 0, 0, 1u, 3u), groups[0].coverage, 0u, 1u)};
  svr::TerrainGPUV2PublicationRequestGPU coarse{};
  coarse.memberCount = 1u;
  coarse.ownerCapacity = 1u;
  coarse.replacementTerrainGeneration = 3u;
  coarse.replacementPayloadRevisionLow = static_cast<uint32_t>(coarseRevision);
  coarse.replacementPayloadRevisionHigh = static_cast<uint32_t>(coarseRevision >> 32u);
  std::vector<svr::TerrainGPUV2PublicationResultGPU> results;
  std::string error;
  check(
      svr::applyTerrainGPUV2PublicationOracle(capacities, {coarse}, members, payloads, groups, owners, results, &error) && results[0].status == static_cast<uint32_t>(svr::TerrainGPUV2PublicationStatus::Committed) &&
          groups[0].ownerCount == 1u && groups[0].epoch == 2u,
      "a complete coarse owner publishes atomically: " + error);

  payloads.clear();
  members.clear();
  for (uint32_t octant = 0u; octant < 8u; ++octant)
  {
    const int32_t x = static_cast<int32_t>(octant & 1u);
    const int32_t y = static_cast<int32_t>((octant >> 1u) & 1u);
    const int32_t z = static_cast<int32_t>((octant >> 2u) & 1u);
    payloads.push_back(payload(4u, fineRevision));
    members.push_back(member(key(x, y, z, 0u, 4u), box(x * 8, y * 8, z * 8, x * 8 + 8, y * 8 + 8, z * 8 + 8), octant, 2u + octant));
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
  fine.ownerOffset = 1u;
  fine.ownerCapacity = 8u;
  fine.expectedOwnerCount = 1u;
  const auto beforeGroup = groups[0];
  const auto beforeOwners = owners;
  payloads[7].status = 0u;
  check(
      svr::applyTerrainGPUV2PublicationOracle(capacities, {fine}, members, payloads, groups, owners, results, &error) && results[0].status == static_cast<uint32_t>(svr::TerrainGPUV2PublicationStatus::Rejected) &&
          std::memcmp(&groups[0], &beforeGroup, sizeof(beforeGroup)) == 0 && std::memcmp(owners.data(), beforeOwners.data(), owners.size() * sizeof(owners[0])) == 0,
      "an interrupted eight-child replacement leaves the complete coarse owner visible");
  payloads[7] = payload(4u, fineRevision);
  check(
      svr::applyTerrainGPUV2PublicationOracle(capacities, {fine}, members, payloads, groups, owners, results, &error) && results[0].status == static_cast<uint32_t>(svr::TerrainGPUV2PublicationStatus::Committed) &&
          groups[0].ownerCount == 8u && groups[0].epoch == 4u,
      "all eight fine chunks replace the coarse owner in one ownership-group commit");
  auto stale = fine;
  stale.expectedEpoch = 2u;
  const auto committed = groups[0];
  check(
      svr::applyTerrainGPUV2PublicationOracle(capacities, {stale}, members, payloads, groups, owners, results, &error) &&
          results[0].rejectionReason == static_cast<uint32_t>(svr::TerrainGPUV2PublicationRejection::StaleOwner) && std::memcmp(&groups[0], &committed, sizeof(committed)) == 0,
      "stale generation/revision CAS cannot overwrite a newer ownership group");
}

svr::TerrainGPUV2ResidentFootprint footprint(const uint64_t bytes)
{
  svr::TerrainGPUV2ResidentFootprint value{};
  value.bytes[static_cast<size_t>(svr::TerrainGPUV2ResidentCategory::RasterNodes)] = bytes / 4u;
  value.bytes[static_cast<size_t>(svr::TerrainGPUV2ResidentCategory::RasterOccupancy)] = bytes / 4u;
  value.bytes[static_cast<size_t>(svr::TerrainGPUV2ResidentCategory::RasterMaterials)] = bytes / 8u;
  value.bytes[static_cast<size_t>(svr::TerrainGPUV2ResidentCategory::RasterTopology)] = bytes / 8u;
  value.bytes[static_cast<size_t>(svr::TerrainGPUV2ResidentCategory::Tree64Nodes)] = bytes / 4u;
  return value;
}

void residencyChurnAndBudget()
{
  constexpr uint64_t revision = 0x123456789abcdef0ull;
  svr::TerrainGPUV2ResidencyManager residency({4096u, 3072u, 3840u, 64u, 32u});
  std::string error;
  svr::TerrainGPUV2ResidentFootprint external{};
  external.bytes[static_cast<size_t>(svr::TerrainGPUV2ResidentCategory::Atlas)] = 256u;
  external.bytes[static_cast<size_t>(svr::TerrainGPUV2ResidentCategory::Scratch)] = 128u;
  external.bytes[static_cast<size_t>(svr::TerrainGPUV2ResidentCategory::Metadata)] = 64u;
  check(residency.setExternalFootprint(external, 0u, &error), "atlas, scratch, and metadata enter the same hard budget: " + error);
  uint64_t coarseHandle = 0u;
  const auto coarseKey = key(0, 0, 0, 1u, 1u);
  const auto whole = box(0, 0, 0, 16, 16, 16);
  check(
      residency.reserveHidden(1u, coarseKey, whole, revision, payload(1u, revision), footprint(192u), 1u, 0u, true, false, coarseHandle, &error) && residency.visible(1u).empty() &&
          residency.hidden(coarseHandle).has_value(),
      "a generated 3D chunk remains hidden until group publication: " + error);
  svr::TerrainGPUV2GroupPublication coarsePublication{};
  coarsePublication.expected.group = 1u;
  coarsePublication.coverage = whole;
  coarsePublication.replacementTerrainGeneration = 1u;
  coarsePublication.replacementPayloadRevision = revision;
  coarsePublication.hiddenHandles = {coarseHandle};
  coarsePublication.allowLegacyVisibleCutPublication = true;
  svr::TerrainGPUV2PublicationResultGPU result{};
  check(residency.publish(coarsePublication, 2u, result, &error) && residency.visible(1u).size() == 1u, "coarse publication exposes exactly one complete owner: " + error);
  check(
      residency.diagnostics().pageInstallCount == 1u &&
          residency.diagnostics().pageUninstallCount == 0u,
      "coarse publication reports one page install and no uninstall");

  std::vector<uint64_t> children;
  for (uint32_t octant = 0u; octant < 8u; ++octant)
  {
    const int32_t x = static_cast<int32_t>(octant & 1u);
    const int32_t y = static_cast<int32_t>((octant >> 1u) & 1u);
    const int32_t z = static_cast<int32_t>((octant >> 2u) & 1u);
    uint64_t handle = 0u;
    check(
        residency.reserveHidden(1u, key(x, y, z, 0u, 2u), box(x * 8, y * 8, z * 8, x * 8 + 8, y * 8 + 8, z * 8 + 8), revision + 1u, payload(2u, revision + 1u), footprint(128u), 2u, 2u, true, false, handle, &error),
        "fine replacement hidden output reserves under the hard budget: " + error);
    children.push_back(handle);
  }
  svr::TerrainGPUV2GroupPublication finePublication{};
  finePublication.expected = residency.expectation(1u);
  finePublication.coverage = whole;
  finePublication.replacementTerrainGeneration = 2u;
  finePublication.replacementPayloadRevision = revision + 1u;
  finePublication.hiddenHandles = children;
  finePublication.allowLegacyVisibleCutPublication = true;
  check(
      residency.publish(finePublication, 5u, result, &error) && residency.visible(1u).size() == 8u && residency.diagnostics().retiredChunks == 1u,
      "fine ownership commits without a parent/child overlap and retires the old owner by fence: " + error);
  check(
      residency.diagnostics().pageInstallCount == 9u &&
          residency.diagnostics().pageUninstallCount == 1u,
      "fine publication reports eight child installs and one parent uninstall");
  check(residency.reclaim(4u) == 0u && residency.reclaim(5u) == 1u, "retired coarse storage is not reusable before its completion fence");
  residency.setGroupRequired(1u, false, 6u);
  std::vector<svr::TerrainGPUV2ChunkIdentity> evicted;
  check(residency.evictUnrequired(9u, evicted, &error) && evicted.empty() && residency.visible(1u).size() == 8u, "unrequired 3D ownership stays drawable until a replacement publication commits");
  check(residency.reclaim(8u) == 0u && residency.reclaim(9u) == 0u, "published owners are not retired by cache eviction");
  const auto rebuilds = residency.takeRebuildRequests();
  check(rebuilds.empty(), "visible ownership groups do not turn into rebuild metadata holes");
  auto newerRebuild = key(0, 0, 0, 0u, 3u);
  ++newerRebuild.expectedGeneration;
  const bool rebuildAdvanced = residency.require(newerRebuild);
  const auto advancedRebuilds = residency.takeRebuildRequests();
  check(
      rebuildAdvanced && advancedRebuilds.size() == 1u && advancedRebuilds[0].expectedGeneration == newerRebuild.expectedGeneration,
      "a newer terrain generation replaces stale rebuild metadata without retaining payload bytes");
  const auto diagnostics = residency.diagnostics();
  const uint64_t expectedActiveBytes = 8u * footprint(128u).total();
  check(
      diagnostics.accountedBytes == external.total() + expectedActiveBytes && diagnostics.peakAccountedBytes <= 4096u && diagnostics.cpuPayloadBytes == 0u && diagnostics.activeChunks == 8u &&
          diagnostics.hiddenChunks == 0u && diagnostics.retiredChunks == 0u,
      "peak accounting includes active, hidden, retired, atlas, scratch, and metadata categories");

  svr::TerrainGPUV2ResidencyManager twoGiB;
  svr::TerrainGPUV2ResidentFootprint largeExternal{};
  largeExternal.bytes[static_cast<size_t>(svr::TerrainGPUV2ResidentCategory::Atlas)] = 512ull << 20u;
  check(twoGiB.setExternalFootprint(largeExternal, 0u, &error), "2048 MiB profile accepts its atlas reservation");
  uint64_t oversized = 0u;
  check(
      !twoGiB.reserveHidden(2u, key(4, 0, 0, 0u, 1u), box(0, 0, 0, 8, 8, 8), revision, payload(1u, revision), footprint(1600ull << 20u), 1u, 0u, true, true, oversized, &error) &&
          twoGiB.diagnostics().peakAccountedBytes <= (2048ull << 20u),
      "the 2048 MiB hard budget rejects a chunk before resident storage can overflow");

  svr::TerrainGPUV2ResidencyManager watermark({1024u, 512u, 896u, 64u, 32u});
  error.clear();
  uint64_t watermarkHandle = 0u;
  for (int32_t x = 0; x < 3; ++x)
    check(
        watermark.reserveHidden(3u + x, key(x + 10, 0, 0, 0u, 1u), box(x * 8, 0, 0, x * 8 + 8, 8, 8), revision, payload(1u, revision), footprint(192u), x, 0u, false, false, watermarkHandle, &error),
        "incoming allocation is admitted through the resident watermark controller: " + error);
  check(
      watermark.diagnostics().accountedBytes == 512u && watermark.diagnostics().hiddenChunks == 2u && watermark.diagnostics().highWatermarkTrims == 1u,
      "high-watermark trimming budgets the incoming allocation and reclaims the oldest hidden output");

  svr::TerrainGPUV2ResidencyManager chunkCap({1024u, 896u, 960u, 64u, 1u});
  error.clear();
  uint64_t first = 0u;
  uint64_t second = 0u;
  check(
      chunkCap.reserveHidden(20u, key(20, 0, 0, 0u, 1u), box(0, 0, 0, 8, 8, 8), revision, payload(1u, revision), footprint(64u), 1u, 0u, false, false, first, &error) &&
          chunkCap.reserveHidden(21u, key(21, 0, 0, 0u, 1u), box(8, 0, 0, 16, 8, 8), revision, payload(1u, revision), footprint(64u), 2u, 0u, false, false, second, &error) && !chunkCap.hidden(first).has_value() &&
          chunkCap.hidden(second).has_value() && chunkCap.diagnostics().hiddenChunks == 1u,
      "maximumResidentChunks evicts an unrequired hidden output before admitting its replacement: " + error);

  svr::TerrainGPUV2ResidencyManager transactional({1024u, 800u, 960u, 64u, 8u});
  svr::TerrainGPUV2ResidentFootprint acceptedExternal{};
  acceptedExternal.bytes[static_cast<size_t>(svr::TerrainGPUV2ResidentCategory::Atlas)] = 768u;
  svr::TerrainGPUV2ResidentFootprint rejectedExternal{};
  rejectedExternal.bytes[static_cast<size_t>(svr::TerrainGPUV2ResidentCategory::Atlas)] = 1088u;
  check(
      transactional.setExternalFootprint(acceptedExternal, 0u, &error) && !transactional.setExternalFootprint(rejectedExternal, 0u, &error) && transactional.diagnostics().accountedBytes == acceptedExternal.total(),
      "a rejected external reservation leaves the previously accounted atlas footprint intact");
}
} // namespace

int main()
{
  std::cout << "Sparse voxel terrain GPU V2 Phase 7 tests\n";
  abiAndAtomicOracle();
  residencyChurnAndBudget();
  if (failures != 0)
    std::cerr << failures << " Phase 7 checks failed\n";
  return failures == 0 ? 0 : 1;
}
