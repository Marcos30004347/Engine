#include "voxel/svraster/TerrainGPUV2Publication.hpp"

#include <algorithm>
#include <limits>
#include <tuple>

namespace voxel::svraster
{
namespace
{
bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool rangeFits(const uint32_t offset, const uint32_t count, const uint32_t capacity)
{
  return offset <= capacity && count <= capacity - offset;
}

bool validBox(const TerrainGPUV2CoverageBox &box)
{
  return box.minimum[0] < box.maximum[0] && box.minimum[1] < box.maximum[1] && box.minimum[2] < box.maximum[2];
}

bool boxVolume(const TerrainGPUV2CoverageBox &box, uint64_t &volume)
{
  if (!validBox(box))
    return false;
  volume = 1u;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const uint64_t extent = static_cast<uint64_t>(int64_t{box.maximum[axis]} - box.minimum[axis]);
    if (extent != 0u && volume > std::numeric_limits<uint64_t>::max() / extent)
      return false;
    volume *= extent;
  }
  return true;
}

bool overlap(const TerrainGPUV2CoverageBox &left, const TerrainGPUV2CoverageBox &right)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    if (left.maximum[axis] <= right.minimum[axis] || right.maximum[axis] <= left.minimum[axis])
      return false;
  return true;
}

bool partition(const TerrainGPUV2CoverageBox &whole, const std::vector<TerrainGPUV2CoverageBox> &parts)
{
  uint64_t wholeVolume = 0u;
  if (!boxVolume(whole, wholeVolume) || parts.empty())
    return false;
  uint64_t sum = 0u;
  for (size_t index = 0u; index < parts.size(); ++index)
  {
    uint64_t volume = 0u;
    if (!boxVolume(parts[index], volume))
      return false;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      if (parts[index].minimum[axis] < whole.minimum[axis] || parts[index].maximum[axis] > whole.maximum[axis])
        return false;
    if (sum > std::numeric_limits<uint64_t>::max() - volume)
      return false;
    sum += volume;
    for (size_t previous = 0u; previous < index; ++previous)
      if (overlap(parts[index], parts[previous]))
        return false;
  }
  return sum == wholeVolume;
}

uint64_t mixIdentity(const uint64_t value, const uint64_t data)
{
  uint64_t hash = value ^ (data + 0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u));
  hash ^= hash >> 33u;
  hash *= 0xff51afd7ed558ccdull;
  hash ^= hash >> 33u;
  return hash;
}

uint64_t terrainChunkLocalPage(const TerrainGPUChunkKey &key, const uint64_t pageKind)
{
  uint64_t hash = 0x7456473270616765ull;
  hash = mixIdentity(hash, key.worldBundleRevision);
  hash = mixIdentity(hash, static_cast<uint32_t>(key.chunkX));
  hash = mixIdentity(hash, static_cast<uint32_t>(key.chunkY));
  hash = mixIdentity(hash, static_cast<uint32_t>(key.chunkZ));
  hash = mixIdentity(hash, key.clipmapLOD);
  hash = mixIdentity(hash, key.generatorRevision);
  hash = mixIdentity(hash, pageKind);
  return hash == std::numeric_limits<uint64_t>::max() ? hash ^ 0x5a5a5a5a5a5a5a5aull : hash;
}

uint64_t revision(const uint32_t low, const uint32_t high)
{
  return uint64_t{low} | (uint64_t{high} << 32u);
}

uint16_t packedLOD(const TerrainGPUChunkKeyGPU &key)
{
  return static_cast<uint16_t>(key.packedLODAndGeneratorRevision & 0xffffu);
}

uint16_t packedGenerator(const TerrainGPUChunkKeyGPU &key)
{
  return static_cast<uint16_t>(key.packedLODAndGeneratorRevision >> 16u);
}

TerrainGPUV2ChunkIdentity identity(const TerrainGPUChunkKeyGPU &key)
{
  return {key.worldBundleRevision, key.chunkX, key.chunkY, key.chunkZ, packedLOD(key), packedGenerator(key)};
}

TerrainGPUV2PublishedOwnerGPU ownerFrom(const TerrainGPUV2PublicationMemberGPU &member, const uint32_t revisionLow, const uint32_t revisionHigh)
{
  TerrainGPUV2PublishedOwnerGPU owner{};
  owner.key = member.key;
  owner.coverage = member.coverage;
  owner.ownerFlags = member.ownerFlags;
  owner.payloadResultIndex = member.payloadResultIndex;
  owner.leaseId = member.leaseId;
  owner.leaseOffset = member.leaseOffset;
  owner.leaseBytes = member.leaseBytes;
  owner.payloadRevisionLow = revisionLow;
  owner.payloadRevisionHigh = revisionHigh;
  return owner;
}

bool payloadReady(const TerrainGPUV2PayloadResultGPU &payload)
{
  return payload.status == static_cast<uint32_t>(TerrainGPUV2PublicationStatus::Approved) && payload.validationErrors == 0u &&
         (payload.flags & (TerrainGPUV2PayloadOccupancyOnly | TerrainGPUV2PayloadNoMicroGeometry)) == (TerrainGPUV2PayloadOccupancyOnly | TerrainGPUV2PayloadNoMicroGeometry) && payload.rasterNodeCount != 0u &&
         payload.rasterRootCount != 0u && payload.treeNodeCount != 0u && payload.treeVisitedNodeCount == payload.treeNodeCount && payload.rasterOccupiedVoxelCount == payload.treeOccupiedVoxelCount &&
         payload.terrainGeneration != 0u;
}
} // namespace

bool terrainGPUV2PayloadResult(
    const TerrainGPUV2FineRasterResultGPU &fineRaster,
    const TerrainGPUV2RasterHierarchyResultGPU *hierarchy,
    const TerrainGPUV2Tree64ResultGPU &tree64,
    TerrainGPUV2PayloadResultGPU &result,
    std::string *error)
{
  result = {};
  if (fineRaster.status != static_cast<uint32_t>(TerrainGPUV2FineRasterStatus::Ready) || fineRaster.validationErrors != 0u || fineRaster.nodeCount == 0u || fineRaster.rootCount == 0u ||
      tree64.status != static_cast<uint32_t>(TerrainGPUV2Tree64Status::Ready) || tree64.validationErrors != 0u || tree64.nodeCount == 0u || tree64.visitedNodeCount != tree64.nodeCount)
    return fail(error, "terrain GPU V2 publication input is not ready");
  if (fineRaster.terrainGeneration == 0u || fineRaster.terrainGeneration != tree64.terrainGeneration)
    return fail(error, "terrain GPU V2 publication generations differ");
  if (fineRaster.occupiedVoxelCount != tree64.occupiedVoxelCount || tree64.rasterOccupiedVoxelCount != fineRaster.occupiedVoxelCount)
    return fail(error, "terrain GPU V2 publication occupancy differs");
  result.status = static_cast<uint32_t>(TerrainGPUV2PublicationStatus::Approved);
  result.flags = TerrainGPUV2PayloadOccupancyOnly | TerrainGPUV2PayloadNoMicroGeometry;
  result.rasterNodeCount = fineRaster.nodeCount;
  result.rasterMaterialCount = fineRaster.materialCount;
  result.rasterRootCount = fineRaster.rootCount;
  result.rasterOccupiedVoxelCount = fineRaster.occupiedVoxelCount;
  result.rasterGeneratorRevision = fineRaster.generatorRevision;
  if (hierarchy != nullptr)
  {
    if (hierarchy->status != static_cast<uint32_t>(TerrainGPUV2RasterHierarchyStatus::Ready) || hierarchy->validationErrors != 0u || hierarchy->terrainGeneration != fineRaster.terrainGeneration ||
        hierarchy->nodeCount == 0u || hierarchy->rootCount == 0u || hierarchy->microSurfaceBytes != 0u)
      return fail(error, "terrain GPU V2 hierarchy publication input is not ready");
    result.flags |= TerrainGPUV2PayloadRasterHierarchy;
    result.rasterNodeCount = hierarchy->nodeCount;
    result.rasterMaterialCount = hierarchy->materialCount;
    result.rasterChildCount = hierarchy->childCount;
    result.rasterRootCount = hierarchy->rootCount;
    result.rasterGeneratorRevision = hierarchy->generatorRevision;
  }
  result.terrainGeneration = fineRaster.terrainGeneration;
  result.treeNodeCount = tree64.nodeCount;
  result.treeVisitedNodeCount = tree64.visitedNodeCount;
  result.treeOccupiedVoxelCount = tree64.occupiedVoxelCount;
  result.treeGeneratorRevision = tree64.generatorRevision;
  result.geometryRevisionLow = tree64.descriptor.geometryRevisionLo;
  result.geometryRevisionHigh = tree64.descriptor.geometryRevisionHi;
  return true;
}

bool terrainGPUV2ProducedPagePayload(
    const SparseVoxelPageId page,
    const uint64_t expectedPageGeneration,
    const uint32_t requiredProducerRevision,
    const TerrainGPUV2PayloadResultGPU &result,
    SparseVoxelProducedPagePayload &payload,
    std::string *error)
{
  payload = {};
  if (!page.valid() || requiredProducerRevision == 0u)
    return fail(error, "terrain GPU V2 produced sparse voxel page identity is invalid");
  if (!payloadReady(result))
    return fail(error, "terrain GPU V2 payload result is not ready");
  if (result.rasterGeneratorRevision != 0u && result.rasterGeneratorRevision != requiredProducerRevision)
  {
    return fail(error, "terrain GPU V2 raster producer revision is stale");
  }
  if (result.treeGeneratorRevision != 0u && result.treeGeneratorRevision != requiredProducerRevision)
  {
    return fail(error, "terrain GPU V2 Tree64 producer revision is stale");
  }
  const uint64_t payloadRevision = revision(result.geometryRevisionLow, result.geometryRevisionHigh);
  if (payloadRevision == 0u)
    return fail(error, "terrain GPU V2 payload revision is invalid");

  payload.page = page;
  payload.expectedPageGeneration = expectedPageGeneration;
  payload.requiredProducerRevision = requiredProducerRevision;
  payload.payloadRevision = payloadRevision;
  payload.geometryRevision = payloadRevision;
  payload.requirements.page = page;
  payload.requirements.geometryRevision = payloadRevision;
  payload.requirements.hierarchyNodes.count = result.rasterNodeCount;
  payload.requirements.childIndices.count = result.rasterChildCount;
  payload.requirements.brickNodes.count = result.rasterNodeCount;
  payload.requirements.brickOccupancy.count = result.rasterOccupiedVoxelCount;
  payload.requirements.materials.count = result.rasterMaterialCount;
  payload.requirements.materialCells.count = result.rasterMaterialCount;
  payload.requirements.tree64Nodes.count = result.treeNodeCount;
  payload.requirements.tree64Materials.count = result.treeOccupiedVoxelCount;
  payload.installRewriteCount = 1u;
  payload.uninstallRewriteCount = 1u;
  payload.payloadComplete = true;
  if (error != nullptr)
    error->clear();
  return true;
}

uint64_t terrainGPUV2StreamedChunkProducerId(const uint64_t worldBundleRevision, const uint32_t generatorRevision)
{
  if (worldBundleRevision == 0u || generatorRevision == 0u)
    return 0u;
  uint64_t producer = 0x7456473270726f64ull;
  producer = mixIdentity(producer, worldBundleRevision);
  producer = mixIdentity(producer, generatorRevision);
  return producer == 0u ? 0x7456473270726f64ull : producer;
}

bool terrainGPUV2StreamedChunkPages(const TerrainGPUChunkKey &key, TerrainGPUV2StreamedChunkPages &pages, std::string *error)
{
  pages = {};
  const uint64_t producerId = terrainGPUV2StreamedChunkProducerId(key.worldBundleRevision, key.generatorRevision);
  if (producerId == 0u)
    return fail(error, "terrain GPU V2 streamed chunk identity is invalid");

  pages.producer.producerId = producerId;
  pages.producer.kind = SparseVoxelPageProducerKind::GPUGeneratedTerrain;
  pages.producer.producerRevision = key.generatorRevision;
  pages.producer.producesOnGPU = true;
  pages.producer.debugName = "terrain-gpu-v2-streamed-chunk";
  pages.rootPage = {producerId, terrainChunkLocalPage(key, 0u)};
  pages.desiredPage = {producerId, terrainChunkLocalPage(key, 1u)};
  if (error != nullptr)
    error->clear();
  return true;
}

bool terrainGPUV2StreamedChunkInstance(const TerrainGPUChunkKey &key, const uint32_t instanceIndex, const uint32_t firstRootNode, const uint32_t pageCount, SparseVoxelStreamedInstance &instance, std::string *error)
{
  instance = {};
  if (instanceIndex == kInvalidSparseVoxelIndex || firstRootNode == kInvalidSparseVoxelIndex || pageCount == 0u)
    return fail(error, "terrain GPU V2 streamed chunk instance parameters are invalid");

  TerrainGPUV2StreamedChunkPages pages{};
  if (!terrainGPUV2StreamedChunkPages(key, pages, error))
    return false;

  instance.instanceIndex = instanceIndex;
  instance.producer = pages.producer;
  instance.rootPage = pages.rootPage;
  instance.firstRootNode = firstRootNode;
  instance.rootNodeCount = 1u;
  instance.firstPage = 0u;
  instance.pageCount = pageCount;
  for (uint32_t diagonal = 0u; diagonal < 4u; ++diagonal)
    instance.modelToWorld[diagonal * 4u + diagonal] = 1.0f;

  if (error != nullptr)
    error->clear();
  return true;
}

bool validateTerrainGPUV2PublicationBatch(
    const TerrainGPUV2PublicationCapacities &capacities,
    const std::vector<TerrainGPUV2PublicationRequestGPU> &requests,
    const std::vector<TerrainGPUV2PublicationMemberGPU> &members,
    const std::vector<TerrainGPUV2PayloadResultGPU> &payloads,
    const std::vector<TerrainGPUV2OwnershipGroupStateGPU> &groups,
    std::string *error)
{
  if (capacities.requests == 0u || capacities.members == 0u || capacities.payloadResults == 0u || capacities.groups == 0u || capacities.owners == 0u || requests.size() > capacities.requests ||
      members.size() > capacities.members || payloads.size() > capacities.payloadResults || groups.size() > capacities.groups)
    return fail(error, "terrain GPU V2 publication capacities are invalid");
  std::set<uint32_t> usedGroups;
  for (const auto &request : requests)
  {
    if (request.groupIndex >= groups.size() || (request.expectedEpoch & 1u) != 0u || request.memberCount == 0u || request.memberCount > kTerrainGPUV2PublicationMaximumMembers ||
        !rangeFits(request.memberOffset, request.memberCount, static_cast<uint32_t>(members.size())) || request.ownerCapacity < request.memberCount ||
        !rangeFits(request.ownerOffset, request.ownerCapacity, capacities.owners) || !rangeFits(request.expectedOwnerOffset, request.expectedOwnerCount, capacities.owners) || request.replacementTerrainGeneration == 0u ||
        (request.replacementPayloadRevisionLow | request.replacementPayloadRevisionHigh) == 0u || !usedGroups.insert(request.groupIndex).second)
      return fail(error, "terrain GPU V2 publication request is invalid");
    const auto &group = groups[request.groupIndex];
    if (!validBox(group.coverage) || !rangeFits(group.ownerOffset, group.ownerCount, capacities.owners))
      return fail(error, "terrain GPU V2 publication group coverage is invalid");
    std::vector<TerrainGPUV2CoverageBox> boxes;
    for (uint32_t offset = 0u; offset < request.memberCount; ++offset)
    {
      const auto &member = members[request.memberOffset + offset];
      if (member.payloadResultIndex >= payloads.size() || member.key.worldBundleRevision == 0u || member.key.expectedGeneration == 0u || member.leaseId == 0u || member.leaseBytes == 0u)
        return fail(error, "terrain GPU V2 publication member is invalid");
      boxes.push_back(member.coverage);
    }
    if (!partition(group.coverage, boxes))
      return fail(error, "terrain GPU V2 publication members do not partition the ownership group");
  }
  return true;
}

bool applyTerrainGPUV2PublicationOracle(
    const TerrainGPUV2PublicationCapacities &capacities,
    const std::vector<TerrainGPUV2PublicationRequestGPU> &requests,
    const std::vector<TerrainGPUV2PublicationMemberGPU> &members,
    const std::vector<TerrainGPUV2PayloadResultGPU> &payloads,
    std::vector<TerrainGPUV2OwnershipGroupStateGPU> &groups,
    std::vector<TerrainGPUV2PublishedOwnerGPU> &owners,
    std::vector<TerrainGPUV2PublicationResultGPU> &results,
    std::string *error)
{
  if (!validateTerrainGPUV2PublicationBatch(capacities, requests, members, payloads, groups, error))
    return false;
  owners.resize(capacities.owners);
  results.assign(requests.size(), {});
  for (size_t requestIndex = 0u; requestIndex < requests.size(); ++requestIndex)
  {
    const auto &request = requests[requestIndex];
    auto &group = groups[request.groupIndex];
    auto &result = results[requestIndex];
    result.groupIndex = request.groupIndex;
    auto reject = [&](const TerrainGPUV2PublicationRejection reason)
    {
      result.status = static_cast<uint32_t>(TerrainGPUV2PublicationStatus::Rejected);
      result.rejectionReason = static_cast<uint32_t>(reason);
    };
    if (group.epoch != request.expectedEpoch || group.terrainGeneration != request.expectedTerrainGeneration || group.payloadRevisionLow != request.expectedPayloadRevisionLow ||
        group.payloadRevisionHigh != request.expectedPayloadRevisionHigh || group.ownerOffset != request.expectedOwnerOffset || group.ownerCount != request.expectedOwnerCount)
    {
      reject(TerrainGPUV2PublicationRejection::StaleOwner);
      continue;
    }
    bool valid = true;
    TerrainGPUV2PublicationRejection reason = TerrainGPUV2PublicationRejection::None;
    for (uint32_t offset = 0u; offset < request.memberCount; ++offset)
    {
      const auto &member = members[request.memberOffset + offset];
      const auto &payload = payloads[member.payloadResultIndex];
      if (!payloadReady(payload))
      {
        valid = false;
        reason = payload.rasterOccupiedVoxelCount != payload.treeOccupiedVoxelCount ? TerrainGPUV2PublicationRejection::OccupancyMismatch : TerrainGPUV2PublicationRejection::InvalidPayload;
        break;
      }
      if (payload.terrainGeneration != request.replacementTerrainGeneration || member.key.expectedGeneration != request.replacementTerrainGeneration)
      {
        valid = false;
        reason = TerrainGPUV2PublicationRejection::GenerationMismatch;
        break;
      }
      if (payload.geometryRevisionLow != request.replacementPayloadRevisionLow || payload.geometryRevisionHigh != request.replacementPayloadRevisionHigh)
      {
        valid = false;
        reason = TerrainGPUV2PublicationRejection::RevisionMismatch;
        break;
      }
    }
    if (!valid)
    {
      reject(reason);
      continue;
    }
    for (uint32_t offset = 0u; offset < request.memberCount; ++offset)
      owners[request.ownerOffset + offset] = ownerFrom(members[request.memberOffset + offset], request.replacementPayloadRevisionLow, request.replacementPayloadRevisionHigh);
    group.terrainGeneration = request.replacementTerrainGeneration;
    group.payloadRevisionLow = request.replacementPayloadRevisionLow;
    group.payloadRevisionHigh = request.replacementPayloadRevisionHigh;
    group.ownerOffset = request.ownerOffset;
    group.ownerCount = request.memberCount;
    group.flags = request.flags;
    group.epoch = request.expectedEpoch + 2u;
    result.status = static_cast<uint32_t>(TerrainGPUV2PublicationStatus::Committed);
    result.groupIndex = request.groupIndex;
    result.committedEpoch = group.epoch;
    result.ownerCount = request.memberCount;
    result.terrainGeneration = request.replacementTerrainGeneration;
    result.payloadRevisionLow = request.replacementPayloadRevisionLow;
    result.payloadRevisionHigh = request.replacementPayloadRevisionHigh;
  }
  return true;
}

uint64_t TerrainGPUV2ResidentFootprint::total() const
{
  uint64_t result = 0u;
  for (const uint64_t value : bytes)
  {
    if (result > std::numeric_limits<uint64_t>::max() - value)
      return std::numeric_limits<uint64_t>::max();
    result += value;
  }
  return result;
}

bool TerrainGPUV2ResidentFootprint::add(const TerrainGPUV2ResidentFootprint &other)
{
  for (size_t index = 0u; index < bytes.size(); ++index)
  {
    if (bytes[index] > std::numeric_limits<uint64_t>::max() - other.bytes[index])
      return false;
    bytes[index] += other.bytes[index];
  }
  return true;
}

TerrainGPUV2ChunkIdentity TerrainGPUV2ChunkIdentity::from(const TerrainGPUChunkKey &key)
{
  return {key.worldBundleRevision, key.chunkX, key.chunkY, key.chunkZ, key.clipmapLOD, key.generatorRevision};
}

bool TerrainGPUV2ChunkIdentity::operator==(const TerrainGPUV2ChunkIdentity &other) const
{
  return std::tie(worldBundleRevision, chunkX, chunkY, chunkZ, clipmapLOD, generatorRevision) == std::tie(other.worldBundleRevision, other.chunkX, other.chunkY, other.chunkZ, other.clipmapLOD, other.generatorRevision);
}

bool TerrainGPUV2ChunkIdentity::operator<(const TerrainGPUV2ChunkIdentity &other) const
{
  return std::tie(worldBundleRevision, chunkX, chunkY, chunkZ, clipmapLOD, generatorRevision) < std::tie(other.worldBundleRevision, other.chunkX, other.chunkY, other.chunkZ, other.clipmapLOD, other.generatorRevision);
}

struct TerrainGPUV2ResidencyManager::Group
{
  uint64_t id = 0u;
  TerrainGPUV2CoverageBox coverage{};
  uint32_t epoch = 0u;
  uint32_t terrainGeneration = 0u;
  uint64_t payloadRevision = 0u;
  uint64_t lastRequiredFrame = 0u;
  bool required = false;
  bool pinned = false;
  std::vector<TerrainGPUV2ActiveOwner> owners{};
};

struct TerrainGPUV2ResidencyManager::Retired
{
  TerrainGPUV2Lease lease{};
  TerrainGPUV2ResidentFootprint footprint{};
  uint64_t fence = 0u;
};

TerrainGPUV2ResidencyManager::TerrainGPUV2ResidencyManager(const TerrainGPUV2ResidencySettings settings)
{
  configure(settings);
}

TerrainGPUV2ResidencyManager::~TerrainGPUV2ResidencyManager() = default;

bool TerrainGPUV2ResidencyManager::configure(const TerrainGPUV2ResidencySettings &settings, std::string *error)
{
  if (settings.hardBudgetBytes == 0u || settings.highWatermarkBytes == 0u || settings.highWatermarkBytes >= settings.emergencyWatermarkBytes || settings.emergencyWatermarkBytes > settings.hardBudgetBytes ||
      settings.minimumAllocationClassBytes == 0u || settings.maximumResidentChunks == 0u)
    return fail(error, "terrain GPU V2 residency settings are invalid");
  if (!groups_.empty() || !hidden_.empty() || !retired_.empty())
    return fail(error, "terrain GPU V2 residency manager is not empty");
  settings_ = settings;
  if (!arena_.configure(settings.hardBudgetBytes, settings.minimumAllocationClassBytes, error))
    return false;
  external_ = {};
  diagnostics_ = {};
  rebuild_.clear();
  nextHiddenHandle_ = 1u;
  completedFence_ = 0u;
  return true;
}

bool TerrainGPUV2ResidencyManager::setExternalFootprint(const TerrainGPUV2ResidentFootprint &footprint, const uint64_t retirementFence, std::string *error)
{
  if (retirementFence < completedFence_ || footprint.total() > settings_.hardBudgetBytes)
    return fail(error, "terrain GPU V2 external footprint is invalid");
  if (arena_.stats().reservedBytes > settings_.hardBudgetBytes - footprint.total())
  {
    ++diagnostics_.hardBudgetRejections;
    return fail(error, "terrain GPU V2 external footprint exceeds the hard resident budget");
  }
  const auto previous = external_;
  external_ = footprint;
  const uint64_t projected = arena_.stats().reservedBytes + external_.total();
  if (projected > settings_.highWatermarkBytes && !trim(projected, retirementFence, projected > settings_.emergencyWatermarkBytes, false, error))
  {
    external_ = previous;
    refreshDiagnostics();
    return false;
  }
  if (arena_.stats().reservedBytes + external_.total() > settings_.hardBudgetBytes)
  {
    ++diagnostics_.hardBudgetRejections;
    external_ = previous;
    refreshDiagnostics();
    return fail(error, "terrain GPU V2 external footprint exceeds the hard resident budget");
  }
  refreshDiagnostics();
  return true;
}

bool TerrainGPUV2ResidencyManager::reserveHidden(
    const uint64_t ownershipGroup,
    const TerrainGPUChunkKey &key,
    const TerrainGPUV2CoverageBox &coverage,
    const uint64_t payloadRevision,
    const TerrainGPUV2PayloadResultGPU &payload,
    const TerrainGPUV2ResidentFootprint &footprint,
    const uint64_t frameIndex,
    const uint64_t retirementFence,
    const bool required,
    const bool pinned,
    uint64_t &handle,
    std::string *error)
{
  handle = 0u;
  if (ownershipGroup == 0u || key.worldBundleRevision == 0u || key.generatorRevision == 0u || key.expectedGeneration == 0u || payloadRevision == 0u || !validBox(coverage) || !payloadReady(payload) ||
      payload.terrainGeneration != key.expectedGeneration || revision(payload.geometryRevisionLow, payload.geometryRevisionHigh) != payloadRevision || footprint.total() == 0u || retirementFence < completedFence_)
    return fail(error, "terrain GPU V2 hidden output is invalid");
  const auto chunkIdentity = TerrainGPUV2ChunkIdentity::from(key);
  for (const auto &[id, hidden] : hidden_)
    if (TerrainGPUV2ChunkIdentity::from(hidden.key) == chunkIdentity)
      return fail(error, "terrain GPU V2 hidden output is duplicate");
  for (const auto &[id, group] : groups_)
    for (const auto &owner : group.owners)
      if (TerrainGPUV2ChunkIdentity::from(owner.key) == chunkIdentity && owner.key.expectedGeneration == key.expectedGeneration)
        return fail(error, "terrain GPU V2 chunk is already resident");
  const uint64_t allocationClass = arena_.sizeClass(footprint.total());
  if (allocationClass == 0u)
    return fail(error, "terrain GPU V2 hidden output has no allocation class");
  uint64_t projected = arena_.stats().reservedBytes + external_.total();
  if (projected > std::numeric_limits<uint64_t>::max() - allocationClass)
    return fail(error, "terrain GPU V2 residency accounting overflows");
  projected += allocationClass;
  uint32_t liveChunks = static_cast<uint32_t>(hidden_.size());
  for (const auto &[id, group] : groups_)
    liveChunks += static_cast<uint32_t>(group.owners.size());
  if ((projected > settings_.highWatermarkBytes || liveChunks >= settings_.maximumResidentChunks) &&
      !trim(projected, retirementFence, projected > settings_.emergencyWatermarkBytes, liveChunks >= settings_.maximumResidentChunks, error))
    return false;
  liveChunks = static_cast<uint32_t>(hidden_.size());
  for (const auto &[id, group] : groups_)
    liveChunks += static_cast<uint32_t>(group.owners.size());
  if (liveChunks >= settings_.maximumResidentChunks)
    return fail(error, "terrain GPU V2 resident chunk limit is exhausted");
  projected = arena_.stats().reservedBytes + external_.total() + allocationClass;
  if (projected > settings_.hardBudgetBytes)
  {
    ++diagnostics_.hardBudgetRejections;
    refreshDiagnostics();
    return fail(error, "terrain GPU V2 hidden output exceeds the hard resident budget");
  }
  const auto lease = arena_.allocate(footprint.total());
  if (!lease)
  {
    ++diagnostics_.hardBudgetRejections;
    refreshDiagnostics();
    return fail(error, "terrain GPU V2 hidden output allocation failed");
  }
  TerrainGPUV2HiddenOutput output{};
  output.handle = nextHiddenHandle_++;
  output.ownershipGroup = ownershipGroup;
  output.key = key;
  output.coverage = coverage;
  output.payloadRevision = payloadRevision;
  output.payload = payload;
  output.lease = *lease;
  output.footprint = footprint;
  output.lastRequiredFrame = frameIndex;
  output.required = required;
  output.pinned = pinned;
  hidden_.emplace(output.handle, output);
  handle = output.handle;
  refreshDiagnostics();
  return true;
}

bool TerrainGPUV2ResidencyManager::retire(const TerrainGPUV2Lease &lease, const TerrainGPUV2ResidentFootprint &footprint, const uint64_t fence, std::string *error)
{
  if (!lease.valid() || fence < completedFence_ || !arena_.releaseDeferred(lease, fence))
    return fail(error, "terrain GPU V2 lease retirement failed");
  retired_.push_back({lease, footprint, fence});
  return true;
}

bool TerrainGPUV2ResidencyManager::publish(const TerrainGPUV2GroupPublication &publication, const uint64_t completionFence, TerrainGPUV2PublicationResultGPU &result, std::string *error)
{
  result = {};
  result.groupIndex = static_cast<uint32_t>(publication.expected.group);
  if (!publication.allowLegacyVisibleCutPublication)
    return fail(error, "terrain GPU V2 legacy ownership publication is disabled; publish generated pages through the shared sparse-voxel page table");
  if (completionFence < completedFence_ || publication.expected.group == 0u || publication.hiddenHandles.empty() || publication.hiddenHandles.size() > kTerrainGPUV2PublicationMaximumMembers ||
      !validBox(publication.coverage) || publication.replacementTerrainGeneration == 0u || publication.replacementPayloadRevision == 0u)
    return fail(error, "terrain GPU V2 ownership publication is invalid");
  auto found = groups_.find(publication.expected.group);
  if (found == groups_.end())
  {
    Group group{};
    group.id = publication.expected.group;
    group.coverage = publication.coverage;
    found = groups_.emplace(group.id, group).first;
  }
  auto &group = found->second;
  auto reject = [&](const TerrainGPUV2PublicationRejection reason)
  {
    result.status = static_cast<uint32_t>(TerrainGPUV2PublicationStatus::Rejected);
    result.rejectionReason = static_cast<uint32_t>(reason);
    ++diagnostics_.rejectedPublications;
    for (const uint64_t handle : publication.hiddenHandles)
    {
      const auto hidden = hidden_.find(handle);
      if (hidden == hidden_.end())
        continue;
      retire(hidden->second.lease, hidden->second.footprint, completionFence, nullptr);
      hidden_.erase(hidden);
    }
    refreshDiagnostics();
    return true;
  };
  std::vector<TerrainGPUV2ChunkIdentity> current;
  for (const auto &owner : group.owners)
    current.push_back(TerrainGPUV2ChunkIdentity::from(owner.key));
  if (!(group.coverage == publication.coverage) || group.epoch != publication.expected.epoch || group.terrainGeneration != publication.expected.terrainGeneration ||
      group.payloadRevision != publication.expected.payloadRevision || current != publication.expected.owners)
    return reject(TerrainGPUV2PublicationRejection::StaleOwner);
  std::set<uint64_t> unique;
  std::vector<TerrainGPUV2CoverageBox> boxes;
  std::vector<std::map<uint64_t, TerrainGPUV2HiddenOutput>::iterator> replacements;
  for (const uint64_t handle : publication.hiddenHandles)
  {
    auto hidden = hidden_.find(handle);
    if (!unique.insert(handle).second || hidden == hidden_.end() || hidden->second.ownershipGroup != group.id)
      return reject(TerrainGPUV2PublicationRejection::InvalidRequest);
    if (hidden->second.key.expectedGeneration != publication.replacementTerrainGeneration || hidden->second.payload.terrainGeneration != publication.replacementTerrainGeneration)
      return reject(TerrainGPUV2PublicationRejection::GenerationMismatch);
    if (hidden->second.payloadRevision != publication.replacementPayloadRevision)
      return reject(TerrainGPUV2PublicationRejection::RevisionMismatch);
    boxes.push_back(hidden->second.coverage);
    replacements.push_back(hidden);
  }
  if (!partition(publication.coverage, boxes))
    return reject(TerrainGPUV2PublicationRejection::CoverageGapOrOverlap);
  const uint64_t replacedOwnerCount = group.owners.size();
  for (const auto &owner : group.owners)
    if (!retire(owner.lease, owner.footprint, completionFence, error))
      return false;
  group.owners.clear();
  group.terrainGeneration = publication.replacementTerrainGeneration;
  group.payloadRevision = publication.replacementPayloadRevision;
  group.epoch += 2u;
  group.required = false;
  group.pinned = false;
  for (auto hidden : replacements)
  {
    TerrainGPUV2ActiveOwner owner{};
    owner.key = hidden->second.key;
    owner.coverage = hidden->second.coverage;
    owner.payloadRevision = hidden->second.payloadRevision;
    owner.lease = hidden->second.lease;
    owner.footprint = hidden->second.footprint;
    owner.lastRequiredFrame = hidden->second.lastRequiredFrame;
    owner.required = hidden->second.required;
    owner.pinned = hidden->second.pinned;
    group.lastRequiredFrame = std::max(group.lastRequiredFrame, owner.lastRequiredFrame);
    group.required |= owner.required;
    group.pinned |= owner.pinned;
    group.owners.push_back(owner);
  }
  for (const uint64_t handle : publication.hiddenHandles)
    hidden_.erase(handle);
  result.status = static_cast<uint32_t>(TerrainGPUV2PublicationStatus::Committed);
  result.committedEpoch = group.epoch;
  result.ownerCount = static_cast<uint32_t>(group.owners.size());
  result.terrainGeneration = group.terrainGeneration;
  result.payloadRevisionLow = static_cast<uint32_t>(group.payloadRevision);
  result.payloadRevisionHigh = static_cast<uint32_t>(group.payloadRevision >> 32u);
  ++diagnostics_.acceptedPublications;
  diagnostics_.pageInstallCount += replacements.size();
  diagnostics_.pageUninstallCount += replacedOwnerCount;
  refreshDiagnostics();
  return true;
}

bool TerrainGPUV2ResidencyManager::setGroupRequired(const uint64_t groupId, const bool required, const uint64_t frameIndex)
{
  const auto found = groups_.find(groupId);
  if (found == groups_.end())
    return false;
  found->second.required = required;
  found->second.lastRequiredFrame = frameIndex;
  for (auto &owner : found->second.owners)
  {
    owner.required = required;
    owner.lastRequiredFrame = frameIndex;
  }
  return true;
}

bool TerrainGPUV2ResidencyManager::trim(const uint64_t projectedBytes, const uint64_t fence, const bool emergency, const bool requireChunkSlot, std::string *error)
{
  if (fence < completedFence_)
    return fail(error, "terrain GPU V2 trim fence predates reclamation");
  struct Candidate
  {
    uint64_t frame = 0u;
    uint64_t id = 0u;
  };
  std::vector<Candidate> candidates;
  for (const auto &[id, hidden] : hidden_)
    if (!hidden.required && !hidden.pinned)
      candidates.push_back({hidden.lastRequiredFrame, id});
  std::sort(
      candidates.begin(),
      candidates.end(),
      [](const Candidate &left, const Candidate &right)
      {
        return std::tie(left.frame, left.id) < std::tie(right.frame, right.id);
      });
  if (emergency)
    ++diagnostics_.emergencyTrims;
  else
    ++diagnostics_.highWatermarkTrims;
  const uint64_t initialAccounted = arena_.stats().reservedBytes + external_.total();
  const uint64_t prospectiveBytes = projectedBytes > initialAccounted ? projectedBytes - initialAccounted : 0u;
  uint64_t scheduledBytes = 0u;
  auto liveChunkCount = [&]()
  {
    uint64_t count = hidden_.size();
    for (const auto &[id, group] : groups_)
      count += group.owners.size();
    return count;
  };
  for (const auto &candidate : candidates)
  {
    const uint64_t accounted = arena_.stats().reservedBytes + external_.total();
    const uint64_t afterRetirement = scheduledBytes < accounted ? accounted - scheduledBytes : 0u;
    const bool bytesSatisfied = prospectiveBytes <= settings_.highWatermarkBytes && afterRetirement <= settings_.highWatermarkBytes - prospectiveBytes;
    const bool slotSatisfied = !requireChunkSlot || liveChunkCount() < settings_.maximumResidentChunks;
    if (bytesSatisfied && slotSatisfied)
      break;
    const auto found = hidden_.find(candidate.id);
    if (found == hidden_.end())
      continue;
    if (!retire(found->second.lease, found->second.footprint, fence, error))
      return false;
    scheduledBytes += found->second.lease.reservedBytes;
    hidden_.erase(found);
    if (fence <= completedFence_)
      reclaim(completedFence_);
  }
  return true;
}

bool TerrainGPUV2ResidencyManager::evictUnrequired(const uint64_t retirementFence, std::vector<TerrainGPUV2ChunkIdentity> &evicted, std::string *error)
{
  evicted.clear();
  if (retirementFence < completedFence_)
    return fail(error, "terrain GPU V2 eviction fence predates reclamation");
  // Published ownership groups are part of the drawable hierarchy cut. Dropping
  // them from the residency cache without an already-built replacement exposes
  // empty coverage to the renderer and produces angle-dependent terrain holes.
  //
  // Fine/coarse transitions must therefore happen through publish(), which
  // atomically swaps a complete replacement partition for the previous owners
  // and retires the old storage only after the replacement is visible.
  refreshDiagnostics();
  return true;
}

uint32_t TerrainGPUV2ResidencyManager::reclaim(const uint64_t completedFence)
{
  completedFence_ = std::max(completedFence_, completedFence);
  const uint32_t count = arena_.reclaim(completedFence_);
  auto retained = retired_.begin();
  for (auto entry = retired_.begin(); entry != retired_.end(); ++entry)
  {
    if (entry->fence <= completedFence_)
    {
      ++diagnostics_.reclaimedChunks;
      continue;
    }
    if (retained != entry)
      *retained = *entry;
    ++retained;
  }
  retired_.erase(retained, retired_.end());
  refreshDiagnostics();
  return count;
}

bool TerrainGPUV2ResidencyManager::require(const TerrainGPUChunkKey &key)
{
  const auto wanted = TerrainGPUV2ChunkIdentity::from(key);
  for (const auto &[id, group] : groups_)
    for (const auto &owner : group.owners)
      if (TerrainGPUV2ChunkIdentity::from(owner.key) == wanted && owner.key.expectedGeneration == key.expectedGeneration)
        return false;
  for (const auto &[id, hidden] : hidden_)
    if (TerrainGPUV2ChunkIdentity::from(hidden.key) == wanted && hidden.key.expectedGeneration == key.expectedGeneration)
      return false;
  const auto found = rebuild_.find(wanted);
  if (found != rebuild_.end())
  {
    if (found->second.expectedGeneration >= key.expectedGeneration)
      return false;
    found->second = key;
    ++diagnostics_.rebuildRequests;
    return true;
  }
  rebuild_.emplace(wanted, key);
  ++diagnostics_.rebuildRequests;
  return true;
}

std::vector<TerrainGPUChunkKey> TerrainGPUV2ResidencyManager::takeRebuildRequests()
{
  std::vector<TerrainGPUChunkKey> requests;
  for (const auto &[identity, key] : rebuild_)
    requests.push_back(key);
  rebuild_.clear();
  return requests;
}

std::vector<TerrainGPUV2ActiveOwner> TerrainGPUV2ResidencyManager::visible(const uint64_t group) const
{
  const auto found = groups_.find(group);
  return found == groups_.end() || (found->second.epoch & 1u) != 0u ? std::vector<TerrainGPUV2ActiveOwner>{} : found->second.owners;
}

std::vector<TerrainGPUV2SharedPageResidency> TerrainGPUV2ResidencyManager::sharedPageResidency() const
{
  std::vector<TerrainGPUV2SharedPageResidency> pages;
  for (const auto &[groupId, group] : groups_)
  {
    if ((group.epoch & 1u) != 0u)
      continue;
    for (const auto &owner : group.owners)
    {
      TerrainGPUV2StreamedChunkPages chunkPages{};
      if (!terrainGPUV2StreamedChunkPages(owner.key, chunkPages, nullptr))
        continue;
      TerrainGPUV2SharedPageResidency page{};
      page.page = chunkPages.desiredPage;
      page.state = SparseVoxelPageState::Resident;
      page.terrainIdentity = TerrainGPUV2ChunkIdentity::from(owner.key);
      page.legacyOwnershipGroup = groupId;
      page.payloadRevision = owner.payloadRevision;
      page.terrainGeneration = owner.key.expectedGeneration;
      page.lastRequiredFrame = owner.lastRequiredFrame;
      page.required = owner.required;
      page.pinned = owner.pinned;
      pages.push_back(page);
    }
  }
  for (const auto &[handle, hidden] : hidden_)
  {
    TerrainGPUV2StreamedChunkPages chunkPages{};
    if (!terrainGPUV2StreamedChunkPages(hidden.key, chunkPages, nullptr))
      continue;
    TerrainGPUV2SharedPageResidency page{};
    page.page = chunkPages.desiredPage;
    page.state = SparseVoxelPageState::Decoded;
    page.terrainIdentity = TerrainGPUV2ChunkIdentity::from(hidden.key);
    page.legacyOwnershipGroup = hidden.ownershipGroup;
    page.legacyHiddenHandle = handle;
    page.payloadRevision = hidden.payloadRevision;
    page.terrainGeneration = hidden.key.expectedGeneration;
    page.lastRequiredFrame = hidden.lastRequiredFrame;
    page.required = hidden.required;
    page.pinned = hidden.pinned;
    pages.push_back(page);
  }
  std::sort(
      pages.begin(),
      pages.end(),
      [](const TerrainGPUV2SharedPageResidency &left, const TerrainGPUV2SharedPageResidency &right)
      {
        return std::tie(left.page.producer, left.page.localPage, left.legacyOwnershipGroup, left.legacyHiddenHandle) <
            std::tie(right.page.producer, right.page.localPage, right.legacyOwnershipGroup, right.legacyHiddenHandle);
      });
  return pages;
}

std::optional<TerrainGPUV2HiddenOutput> TerrainGPUV2ResidencyManager::hidden(const uint64_t handle) const
{
  const auto found = hidden_.find(handle);
  return found == hidden_.end() ? std::nullopt : std::optional<TerrainGPUV2HiddenOutput>{found->second};
}

TerrainGPUV2OwnershipExpectation TerrainGPUV2ResidencyManager::expectation(const uint64_t groupId) const
{
  TerrainGPUV2OwnershipExpectation result{};
  result.group = groupId;
  const auto found = groups_.find(groupId);
  if (found == groups_.end())
    return result;
  result.epoch = found->second.epoch;
  result.terrainGeneration = found->second.terrainGeneration;
  result.payloadRevision = found->second.payloadRevision;
  for (const auto &owner : found->second.owners)
    result.owners.push_back(TerrainGPUV2ChunkIdentity::from(owner.key));
  return result;
}

void TerrainGPUV2ResidencyManager::refreshDiagnostics()
{
  TerrainGPUV2ResidentFootprint categories = external_;
  uint32_t active = 0u;
  for (const auto &[id, group] : groups_)
    for (const auto &owner : group.owners)
    {
      categories.add(owner.footprint);
      ++active;
    }
  for (const auto &[id, hidden] : hidden_)
    categories.add(hidden.footprint);
  for (const auto &retired : retired_)
    categories.add(retired.footprint);
  const auto stats = arena_.stats();
  diagnostics_.categories = categories;
  diagnostics_.payloadReservedBytes = stats.reservedBytes;
  diagnostics_.fragmentationBytes = stats.internalFragmentationBytes;
  diagnostics_.accountedBytes = stats.reservedBytes + external_.total();
  diagnostics_.peakAccountedBytes = std::max(diagnostics_.peakAccountedBytes, diagnostics_.accountedBytes);
  diagnostics_.activeChunks = active;
  diagnostics_.hiddenChunks = static_cast<uint32_t>(hidden_.size());
  diagnostics_.retiredChunks = static_cast<uint32_t>(retired_.size());
  diagnostics_.cpuPayloadBytes = 0u;
}

TerrainGPUV2ResidencyDiagnostics TerrainGPUV2ResidencyManager::diagnostics() const
{
  return diagnostics_;
}

} // namespace voxel::svraster
