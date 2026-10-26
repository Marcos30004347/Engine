#include "voxel/svraster/SparseVoxelTerrainPlacement.hpp"

#include <algorithm>
#include <cmath>
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

uint64_t mix64(uint64_t value)
{
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
  return value ^ (value >> 31u);
}

void hashBytes(uint64_t &hash, const void *data, const size_t byteCount)
{
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t index = 0u; index < byteCount; ++index)
    hash = mix64(hash ^ bytes[index]);
}

void hashString(uint64_t &hash, const std::string &value)
{
  hash = mix64(hash ^ static_cast<uint64_t>(value.size()));
  if (!value.empty())
    hashBytes(hash, value.data(), value.size());
}

template <typename T> void hashScalar(uint64_t &hash, const T &value)
{
  hashBytes(hash, &value, sizeof(T));
}

bool validKey(
    const procedural::world::MapExportBundle &bundle,
    const procedural::world::PlacementPalette &palette,
    const SparseVoxelTerrainPlacementCandidateBufferKey &key,
    std::string *error)
{
  if (!procedural::world::validMapExportPageAddress(key.worldDataPage, bundle.maximumPageLevel) || !key.terrainPage.valid() || key.worldSeed != bundle.seed ||
      key.placementPaletteVersion != palette.version || key.placementPaletteSignature != sparseVoxelTerrainPlacementPaletteSignature(palette) || key.terrainOutputRevision == 0u)
    return fail(error, "terrain placement candidate buffer key does not match the world page, terrain page, seed, palette or terrain revision");
  return true;
}

procedural::world::PlacementTileRequest tileRequestFor(
    const procedural::world::MapExportBundle &bundle,
    const SparseVoxelTerrainPlacementCandidateBufferKey &key,
    const SparseVoxelTerrainPlacementBuildSettings &settings)
{
  const uint32_t pageCount = uint32_t{1u} << key.worldDataPage.level;
  const double invPageCount = pageCount == 0u ? 1.0 : 1.0 / static_cast<double>(pageCount);
  procedural::world::PlacementTileRequest request{};
  request.normalizedBounds = {
    std::clamp(static_cast<double>(key.worldDataPage.x) * invPageCount, 0.0, 1.0),
    std::clamp(static_cast<double>(key.worldDataPage.y) * invPageCount, 0.0, 1.0),
    std::clamp(static_cast<double>(key.worldDataPage.x + 1u) * invPageCount, 0.0, 1.0),
    std::clamp(static_cast<double>(key.worldDataPage.y + 1u) * invPageCount, 0.0, 1.0),
  };
  request.requestedLevel = std::min<uint8_t>(key.worldDataPage.level, bundle.maximumPageLevel);
  request.candidateCellMetres = settings.candidateCellMetres;
  request.maximumEvaluatedSites = settings.maximumEvaluatedSites;
  request.maximumCandidates = settings.maximumCandidates;
  return request;
}

bool overlapsFootprint(
    const procedural::world::PlacementCandidate &candidate,
    const procedural::world::PlacementAssetRule &asset,
    const SparseVoxelTerrainPlacementFootprintExclusion &footprint)
{
  if (!(footprint.radiusMetres >= 0.0) || !std::isfinite(footprint.radiusMetres))
    return false;
  const double separation = std::max(0.0, footprint.radiusMetres) + std::max(0.0, asset.footprintRadiusMetres);
  const double dx = candidate.position.x - footprint.center.x;
  const double dy = candidate.position.y - footprint.center.y;
  return dx * dx + dy * dy < separation * separation;
}

const SparseVoxelTerrainPlacementAssetResidency *findResidency(
    const std::vector<SparseVoxelTerrainPlacementAssetResidency> &residency,
    const std::string &assetKey)
{
  const auto found = std::find_if(
      residency.begin(),
      residency.end(),
      [&](const SparseVoxelTerrainPlacementAssetResidency &entry)
      {
        return entry.assetKey == assetKey;
      });
  return found == residency.end() ? nullptr : &*found;
}

std::array<float, 16> modelToWorldFor(const procedural::world::PlacementCandidate &candidate)
{
  const float scale = static_cast<float>(candidate.scale);
  const float c = static_cast<float>(std::cos(candidate.yawRadians)) * scale;
  const float s = static_cast<float>(std::sin(candidate.yawRadians)) * scale;
  std::array<float, 16> matrix{};
  matrix[0] = c;
  matrix[1] = 0.0f;
  matrix[2] = s;
  matrix[4] = 0.0f;
  matrix[5] = scale;
  matrix[6] = 0.0f;
  matrix[8] = -s;
  matrix[9] = 0.0f;
  matrix[10] = c;
  matrix[12] = static_cast<float>(candidate.position.x);
  matrix[13] = static_cast<float>(candidate.elevationMetres);
  matrix[14] = static_cast<float>(candidate.position.y);
  matrix[15] = 1.0f;
  return matrix;
}

bool sameDomain(
    const procedural::world::PlacementAssetRule &rule,
    const SparseVoxelTerrainPlacementAssetResidency &residency)
{
  if (rule.kind == procedural::world::PlacementAssetKind::VoxelModel)
    return residency.domain == SparseVoxelTerrainPlacementResidencyDomain::SparseVoxel;
  return residency.domain == SparseVoxelTerrainPlacementResidencyDomain::VirtualGeometry;
}

} // namespace

bool SparseVoxelTerrainPlacementCandidateBufferKey::operator==(const SparseVoxelTerrainPlacementCandidateBufferKey &other) const
{
  return worldDataPage == other.worldDataPage && terrainPage == other.terrainPage && worldSeed == other.worldSeed && placementPaletteVersion == other.placementPaletteVersion &&
      placementPaletteSignature == other.placementPaletteSignature && terrainOutputRevision == other.terrainOutputRevision;
}

bool SparseVoxelTerrainPlacementCandidateBufferKey::operator<(const SparseVoxelTerrainPlacementCandidateBufferKey &other) const
{
  return std::tie(worldDataPage.level, worldDataPage.x, worldDataPage.y, terrainPage.producer, terrainPage.localPage, worldSeed, placementPaletteVersion, placementPaletteSignature, terrainOutputRevision) <
      std::tie(
          other.worldDataPage.level,
          other.worldDataPage.x,
          other.worldDataPage.y,
          other.terrainPage.producer,
          other.terrainPage.localPage,
          other.worldSeed,
          other.placementPaletteVersion,
          other.placementPaletteSignature,
          other.terrainOutputRevision);
}

uint64_t sparseVoxelTerrainPlacementPaletteSignature(const procedural::world::PlacementPalette &palette)
{
  uint64_t hash = mix64(0x706c6163656d656eull);
  hashScalar(hash, palette.version);
  const uint64_t assetCount = palette.assets.size();
  hashScalar(hash, assetCount);
  for (const procedural::world::PlacementAssetRule &asset : palette.assets)
  {
    hashString(hash, asset.assetKey);
    hashString(hash, asset.collectionKey);
    hashString(hash, asset.resourceKey);
    hashScalar(hash, asset.kind);
    hashScalar(hash, asset.potential);
    hashScalar(hash, asset.selectionWeight);
    hashScalar(hash, asset.densityMultiplier);
    hashScalar(hash, asset.footprintRadiusMetres);
    hashScalar(hash, asset.minimumSpacingMetres);
    hashScalar(hash, asset.scaleRange);
    hashScalar(hash, asset.altitudeRangeMetres);
    hashScalar(hash, asset.slopeRangeDegrees);
    hashScalar(hash, asset.moistureRange);
    hashScalar(hash, asset.soilDepthRangeMetres);
    hashScalar(hash, asset.riverDistanceRangeMetres);
    const uint64_t biomeCount = asset.biomes.size();
    hashScalar(hash, biomeCount);
    for (const procedural::world::PlacementBiomeAffinity &affinity : asset.biomes)
    {
      hashScalar(hash, affinity.biome);
      hashScalar(hash, affinity.weight);
    }
    const uint64_t substrateCount = asset.substrates.size();
    hashScalar(hash, substrateCount);
    for (const uint8_t substrate : asset.substrates)
      hashScalar(hash, substrate);
  }
  return hash == 0u ? 1u : hash;
}

bool buildSparseVoxelTerrainPlacementCandidateBuffer(
    const procedural::world::MapExportBundle &bundle,
    const procedural::world::PlacementPalette &palette,
    const SparseVoxelTerrainPlacementCandidateBufferKey &key,
    const SparseVoxelTerrainPlacementBuildSettings &settings,
    SparseVoxelTerrainPlacementCandidateBuffer &buffer,
    std::string *error)
{
  buffer = {};
  buffer.key = key;
  if (!settings.enabled)
  {
    if (error != nullptr)
      error->clear();
    return true;
  }
  if (!validKey(bundle, palette, key, error))
    return false;

  procedural::world::PlacementCandidateSet source{};
  const procedural::world::PlacementTileRequest request = tileRequestFor(bundle, key, settings);
  if (!procedural::world::buildPlacementCandidates(bundle, palette, request, source, error))
  {
    buffer = {};
    return false;
  }

  buffer.evaluatedSites = source.evaluatedSites;
  buffer.densityRejected = source.densityRejected;
  buffer.spacingRejected = source.spacingRejected;
  buffer.waterRejected = source.waterRejected;
  buffer.mapExclusionRejected = source.exclusionRejected;
  buffer.ancestorSampledSites = source.ancestorSampledSites;
  buffer.candidates.reserve(source.candidates.size());
  for (const procedural::world::PlacementCandidate &candidate : source.candidates)
  {
    if (candidate.assetIndex >= palette.assets.size())
      return fail(error, "placement candidate references an invalid palette asset");
    const procedural::world::PlacementAssetRule &asset = palette.assets[candidate.assetIndex];
    bool rejected = false;
    for (const SparseVoxelTerrainPlacementFootprintExclusion &footprint : settings.footprintExclusions)
      if (overlapsFootprint(candidate, asset, footprint))
      {
        rejected = true;
        break;
      }
    if (rejected)
    {
      ++buffer.footprintRejected;
      continue;
    }
    buffer.candidates.push_back(candidate);
  }
  if (error != nullptr)
    error->clear();
  return true;
}

bool buildSparseVoxelTerrainPlacementInstanceRequests(
    const SparseVoxelTerrainPlacementCandidateBuffer &buffer,
    const procedural::world::PlacementPalette &palette,
    const std::vector<SparseVoxelTerrainPlacementAssetResidency> &residency,
    const uint32_t firstInstanceIndex,
    std::vector<SparseVoxelTerrainPlacementInstanceRequest> &requests,
    std::string *error)
{
  requests.clear();
  if (firstInstanceIndex == kInvalidSparseVoxelIndex)
    return fail(error, "placement instance range starts at an invalid instance index");
  requests.reserve(buffer.candidates.size());
  for (uint32_t candidateIndex = 0u; candidateIndex < buffer.candidates.size(); ++candidateIndex)
  {
    const procedural::world::PlacementCandidate &candidate = buffer.candidates[candidateIndex];
    if (candidate.assetIndex >= palette.assets.size())
    {
      requests.clear();
      return fail(error, "placement instance candidate references an invalid palette asset");
    }
    const procedural::world::PlacementAssetRule &asset = palette.assets[candidate.assetIndex];
    const SparseVoxelTerrainPlacementAssetResidency *assetResidency = findResidency(residency, asset.assetKey);
    if (assetResidency == nullptr || !sameDomain(asset, *assetResidency))
    {
      requests.clear();
      return fail(error, "placement asset has no matching streamed residency record");
    }

    SparseVoxelTerrainPlacementInstanceRequest request{};
    request.stableInstanceId = candidate.stableId;
    request.candidateKey = buffer.key;
    request.candidateIndex = candidateIndex;
    request.instanceIndex = firstInstanceIndex + candidateIndex;
    if (request.instanceIndex < firstInstanceIndex)
    {
      requests.clear();
      return fail(error, "placement instance index range overflows");
    }
    request.assetKey = asset.assetKey;
    request.collectionKey = asset.collectionKey;
    request.resourceKey = asset.resourceKey;
    request.domain = assetResidency->domain;
    request.modelToWorld = modelToWorldFor(candidate);
    request.footprintRadiusMetres = static_cast<float>(asset.footprintRadiusMetres * candidate.scale);
    request.ownsAssetPayload = false;

    if (assetResidency->domain == SparseVoxelTerrainPlacementResidencyDomain::SparseVoxel)
    {
      if (!assetResidency->sparseVoxelProducer.valid() || !assetResidency->sparseVoxelRootPage.valid() || assetResidency->sparseVoxelFirstRootNode == kInvalidSparseVoxelIndex ||
          assetResidency->sparseVoxelRootNodeCount == 0u || assetResidency->sparseVoxelPageCount == 0u)
      {
        requests.clear();
        return fail(error, "sparse voxel placement asset residency is incomplete");
      }
      request.sparseVoxelDesiredPage = assetResidency->sparseVoxelDesiredPage.valid() ? assetResidency->sparseVoxelDesiredPage : assetResidency->sparseVoxelRootPage;
      request.sparseVoxelInstance.instanceIndex = request.instanceIndex;
      request.sparseVoxelInstance.producer = assetResidency->sparseVoxelProducer;
      request.sparseVoxelInstance.rootPage = assetResidency->sparseVoxelRootPage;
      request.sparseVoxelInstance.firstRootNode = assetResidency->sparseVoxelFirstRootNode;
      request.sparseVoxelInstance.rootNodeCount = assetResidency->sparseVoxelRootNodeCount;
      request.sparseVoxelInstance.firstPage = assetResidency->sparseVoxelFirstPage;
      request.sparseVoxelInstance.pageCount = assetResidency->sparseVoxelPageCount;
      request.sparseVoxelInstance.modelToWorld = request.modelToWorld;
    }
    else
    {
      if (assetResidency->virtualGeometryObjectIndex == kInvalidSparseVoxelIndex || assetResidency->virtualGeometryRootPage == kInvalidSparseVoxelIndex ||
          assetResidency->virtualGeometryPageTableOffset == kInvalidSparseVoxelIndex)
      {
        requests.clear();
        return fail(error, "virtual geometry placement asset residency is incomplete");
      }
      request.virtualGeometryObjectIndex = assetResidency->virtualGeometryObjectIndex;
      request.virtualGeometryRootPage = assetResidency->virtualGeometryRootPage;
      request.virtualGeometryDesiredPage = assetResidency->virtualGeometryDesiredPage == kInvalidSparseVoxelIndex ? assetResidency->virtualGeometryRootPage : assetResidency->virtualGeometryDesiredPage;
      request.virtualGeometryPageTableOffset = assetResidency->virtualGeometryPageTableOffset;
    }
    requests.push_back(std::move(request));
  }
  if (error != nullptr)
    error->clear();
  return true;
}

} // namespace voxel::svraster
