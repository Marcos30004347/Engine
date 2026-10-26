// Generated-world page determinism.
//
// The streaming design rests on one assumption: a page is a pure function of
// the bundle, the generator settings and the page address, so any page may be
// dropped and rebuilt instead of kept. TransientSparseVoxelPageStore states it
// outright - overflow "simply drops the oldest page because any page can be
// generated again" - and nothing tested it.
//
// A violation is unusually nasty. The page that is regenerated differs from the
// one that was evicted, so seams open against neighbours that were never
// evicted, and only after an eviction, in whatever region the camera happened to
// leave. It never reproduces on a fresh run. These are the assertions that turn
// that into a build failure instead.

#include "procedural/world/MapExport.hpp"
#include "voxel/svraster/PlanetVoxelChunk.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace svr = voxel::svraster;
using procedural::world::MapExportBundle;
using procedural::world::MapExportField;
using procedural::world::MapExportFilter;
using procedural::world::MapExportFormat;
using procedural::world::MapExportPage;

namespace
{

int failures = 0;

void check(const bool condition, const char *message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

void appendU16(std::vector<uint8_t> &bytes, const uint16_t value)
{
  bytes.push_back(static_cast<uint8_t>(value & 0xffu));
  bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
}

MapExportField constantField(
    const std::string &name,
    const MapExportFormat format,
    const MapExportFilter filter,
    const std::vector<uint8_t> &texel,
    const uint32_t pageSize,
    const uint16_t gutter,
    const std::string &samplingGroup = {})
{
  MapExportField field{};
  field.name = name;
  field.format = format;
  field.filter = filter;
  field.width = pageSize;
  field.height = pageSize;
  field.available = true;
  field.samplingGroup = samplingGroup;
  MapExportPage page{};
  page.address = {0u, 0u, 0u};
  page.width = pageSize + gutter * 2u;
  page.height = page.width;
  page.data.reserve(
      static_cast<size_t>(page.width) * page.height * texel.size());
  for (size_t index = 0u;
       index < static_cast<size_t>(page.width) * page.height; ++index)
    page.data.insert(page.data.end(), texel.begin(), texel.end());
  field.pages.push_back(std::move(page));
  return field;
}

// Relief rather than a constant. A flat fixture would satisfy every assertion
// below trivially: the shell extractor emits one identical top cell per column,
// so an ordering bug between workers could not change the result. This pattern
// gives every column a different height and every page different content.
MapExportField reliefField(
    const std::string &name,
    const uint32_t pageSize,
    const uint16_t gutter)
{
  MapExportField field{};
  field.name = name;
  field.format = MapExportFormat::R16Unorm;
  field.filter = MapExportFilter::Linear;
  field.width = pageSize;
  field.height = pageSize;
  field.available = true;
  MapExportPage page{};
  page.address = {0u, 0u, 0u};
  page.width = pageSize + gutter * 2u;
  page.height = page.width;
  for (uint32_t y = 0u; y < page.height; ++y)
    for (uint32_t x = 0u; x < page.width; ++x)
    {
      // Deterministic, non-separable, and varying at every scale the sampler
      // touches, so neighbouring columns rarely agree.
      uint32_t hash = x * 73856093u ^ y * 19349663u;
      hash ^= hash >> 13u;
      hash *= 1274126177u;
      hash ^= hash >> 16u;
      const uint16_t height =
          static_cast<uint16_t>(20000u + (hash % 30000u));
      appendU16(page.data, height);
    }
  field.pages.push_back(std::move(page));
  return field;
}

MapExportBundle reliefFixture()
{
  constexpr uint32_t pageSize = 32u;
  constexpr uint16_t gutter = 1u;
  MapExportBundle bundle{};
  bundle.resolution = pageSize;
  bundle.pageSizePixels = pageSize;
  bundle.gutterPixels = gutter;
  bundle.maximumPageLevel = 0u;
  bundle.extentMetres = 6400.0;
  bundle.seed = 0x4f1bbcdc1234abcdull;
  bundle.bundleSignature = 0x9e3779b97f4a7c15ull;
  bundle.fields = {
    constantField(
        "placement_potential", MapExportFormat::RGBA8Unorm,
        MapExportFilter::Linear, {255u, 255u, 255u, 255u}, pageSize, gutter),
    constantField(
        "water_type", MapExportFormat::R8Uint, MapExportFilter::Nearest, {0u},
        pageSize, gutter),
    constantField(
        "water_level", MapExportFormat::R16Float, MapExportFilter::Linear,
        {0u, 0u}, pageSize, gutter),
    constantField(
        "climate", MapExportFormat::RG16Float, MapExportFilter::Linear,
        {0u, 0u, 0u, 0x38u}, pageSize, gutter),
    reliefField("refined_elevation", pageSize, gutter),
    constantField(
        "base_elevation", MapExportFormat::R16Unorm, MapExportFilter::Linear,
        {0x9au, 0x19u}, pageSize, gutter),
    constantField(
        "relief_roughness", MapExportFormat::RG16Float,
        MapExportFilter::Linear, {0xb0u, 0x5cu, 0x00u, 0x38u}, pageSize,
        gutter),
    constantField(
        "landform_id", MapExportFormat::R8Uint, MapExportFilter::Nearest,
        {static_cast<uint8_t>(procedural::world::CellType::Hill)}, pageSize,
        gutter),
    constantField(
        "river", MapExportFormat::RG16Float, MapExportFilter::Linear,
        {0u, 0x3cu, 0u, 0u}, pageSize, gutter),
    constantField(
        "substrate_id", MapExportFormat::R8Uint, MapExportFilter::Nearest,
        {3u}, pageSize, gutter),
    constantField(
        "soil_depth", MapExportFormat::R16Float, MapExportFilter::Linear,
        {0u, 0x3cu}, pageSize, gutter),
    constantField(
        "biome_mix_ids", MapExportFormat::RGBA8Uint,
        MapExportFilter::SparseMixture, {1u, 0u, 0u, 0u}, pageSize, gutter,
        "biome_mixture"),
    constantField(
        "biome_mix_weights", MapExportFormat::RGBA8Unorm,
        MapExportFilter::SparseMixture, {255u, 0u, 0u, 0u}, pageSize, gutter,
        "biome_mixture"),
    constantField(
        "object_exclusion", MapExportFormat::R8Uint, MapExportFilter::Nearest,
        {0u}, pageSize, gutter),
  };
  bundle.field("refined_elevation")->scale[0] = 1000.0;
  bundle.field("base_elevation")->scale[0] = 1000.0;
  bundle.field("river")->scale[0] = bundle.extentMetres;
  return bundle;
}

svr::WorldMapVoxelTerrainSettings pageSettings(
    const double minimumU,
    const double minimumV,
    const double spanU,
    const uint32_t internalLOD,
    const uint32_t workerCount)
{
  svr::WorldMapVoxelTerrainSettings settings{};
  settings.normalizedBounds = {
    minimumU, minimumV, minimumU + spanU, minimumV + spanU};
  settings.requestedLevel = 0u;
  settings.internalLOD = internalLOD;
  settings.finestHorizontalVoxelCount = 32u;
  settings.finestVoxelSizeMetres = 1.0;
  settings.elevationAnchorLOD = 0u;
  settings.generatorRevision = 9u;
  settings.exteriorShellOnly = true;
  settings.boundaryStitchSubdivisions = 1u;
  settings.voxel.horizontalVoxelCount = 32u;
  settings.voxel.patchEdgeMetres = 32.0;
  settings.voxel.foundationDepthVoxels = 2u;
  settings.voxel.surfaceShellDepthVoxels = 1u;
  settings.voxel.maximumVerticalVoxels = 64u;
  settings.voxel.maximumBricks = 1u << 16u;
  settings.voxel.microGeometry = false;
  settings.voxel.clipmapLevelCount = 1u;
  settings.voxel.requestedLODMask = 1u;
  settings.voxel.workerCount = workerCount;
  return settings;
}

uint64_t hashBytes(const std::vector<uint8_t> &bytes)
{
  uint64_t hash = 1469598103934665603ull;
  for (const uint8_t value : bytes)
  {
    hash ^= value;
    hash *= 1099511628211ull;
  }
  return hash;
}

// Generates a page and hashes the exact bytes the streamer would cache. Zero
// means the build failed, which the caller reports rather than silently
// comparing two failures and calling them equal.
uint64_t generatedPageHash(
    const MapExportBundle &bundle,
    const svr::WorldMapVoxelTerrainSettings &settings,
    size_t *voxelCount = nullptr,
    std::string *error = nullptr)
{
  svr::PlanetVoxelChunkSet page{};
  std::string localError;
  if (!svr::buildWorldMapVoxelTerrain(bundle, settings, page, &localError))
  {
    if (error != nullptr)
      *error = localError;
    return 0u;
  }
  if (voxelCount != nullptr)
    *voxelCount = page.bricks.materialIndices.size();
  return hashBytes(svr::serializeWorldVoxelTransientPage(page));
}

svr::WorldVoxelChunkStreamingSettings keySettings()
{
  svr::WorldVoxelChunkStreamingSettings settings{};
  settings.chunkEdgeMetres = 32.0;
  settings.finestVoxelSizeMetres = 1.0;
  settings.finestHorizontalVoxelCount = 32u;
  settings.renderDistanceMetres = 500.0;
  settings.projectionScalePixels = 467.654;
  settings.generatorRevision = 9u;
  settings.lodCount = 1u;
  settings.voxel.foundationDepthVoxels = 2u;
  settings.voxel.surfaceShellDepthVoxels = 1u;
  settings.voxel.maximumVerticalVoxels = 64u;
  settings.voxel.surfaceEnvelopeRadius = 1u;
  settings.voxel.maximumBricks = 1u << 16u;
  settings.voxel.proceduralDetailStrength = 1.0;
  settings.voxel.microGeometry = false;
  settings.voxel.microMaterialRevision = 0u;
  return settings;
}

bool sameKey(
    const svr::TransientSparseVoxelPageKey &left,
    const svr::TransientSparseVoxelPageKey &right)
{
  return left.source == right.source && left.page == right.page;
}

bool cacheCoversWorldPoint(
    const svr::WorldVoxelChunkCache &cache,
    const svr::WorldVoxelChunkStreamingSettings &settings,
    const MapExportBundle &bundle,
    const double worldX,
    const double worldZ)
{
  const double mapX = worldX + bundle.extentMetres * 0.5;
  const double mapZ = worldZ + bundle.extentMetres * 0.5;
  return std::any_of(
      cache.entries.begin(), cache.entries.end(),
      [&](const svr::WorldVoxelChunkEntry &entry)
      {
        const double edge = std::ldexp(
            settings.chunkEdgeMetres,
            static_cast<int>(entry.address.spatialLOD));
        const double minimumX = static_cast<double>(entry.address.x) * edge;
        const double minimumZ = static_cast<double>(entry.address.z) * edge;
        return mapX >= minimumX && mapX < minimumX + edge &&
            mapZ >= minimumZ && mapZ < minimumZ + edge;
      });
}

bool allPublishedNodesHaveFlag(
    const svr::PlanetVoxelChunkSet &chunks,
    const uint32_t flag)
{
  bool sawNode = false;
  for (const auto &record : chunks.chunks)
  {
    const uint64_t end =
        static_cast<uint64_t>(record.brickNodeOffset) +
        record.brickNodeCapacity;
    if (end > chunks.bricks.nodes.size())
      return false;
    for (uint32_t index = record.brickNodeOffset;
         index < record.brickNodeOffset + record.brickNodeCapacity; ++index)
    {
      if (chunks.bricks.nodes[index].occupiedCount == 0u)
        continue;
      sawNode = true;
      if ((chunks.bricks.nodes[index].level & flag) == 0u)
        return false;
    }
  }
  return sawNode;
}

bool noPublishedNodeHasFlag(
    const svr::PlanetVoxelChunkSet &chunks,
    const uint32_t flag)
{
  bool sawNode = false;
  for (const auto &record : chunks.chunks)
  {
    const uint64_t end =
        static_cast<uint64_t>(record.brickNodeOffset) +
        record.brickNodeCapacity;
    if (end > chunks.bricks.nodes.size())
      return false;
    for (uint32_t index = record.brickNodeOffset;
         index < record.brickNodeOffset + record.brickNodeCapacity; ++index)
    {
      if (chunks.bricks.nodes[index].occupiedCount == 0u)
        continue;
      sawNode = true;
      if ((chunks.bricks.nodes[index].level & flag) != 0u)
        return false;
    }
  }
  return sawNode;
}

bool packedVisibleRootPrefixValid(const svr::PlanetVoxelChunkSet &chunks)
{
  if (chunks.bricks.rootCount > chunks.bricks.rootIndices.size())
    return false;
  for (uint32_t root = 0u; root < chunks.bricks.rootCount; ++root)
  {
    const uint32_t node = chunks.bricks.rootIndices[root];
    if (node == svr::kSparseVoxelBrickInvalidIndex ||
        node >= chunks.bricks.nodes.size())
      return false;
  }
  return true;
}

bool runtimePageTableValid(
    const svr::WorldVoxelChunkCache &cache,
    const svr::PlanetVoxelChunkSet &chunks,
    std::string &error)
{
  if (!svr::validateWorldVoxelTerrainRuntimePageTable(
          cache.pageDirectory, chunks.terrainRuntimePageTable, &error))
    return false;
  if (chunks.terrainRuntimePageTable.pages.size() !=
      cache.pageDirectory.entries.size())
    return false;
  const bool hasResidentNodeSpan = std::any_of(
      cache.pageDirectory.entries.begin(), cache.pageDirectory.entries.end(),
      [&](const svr::WorldVoxelPageDirectoryEntry &entry)
      {
        const uint32_t pageIndex = cache.pageDirectory.find(entry.address);
        return entry.state == svr::WorldVoxelPageState::Resident &&
            pageIndex < chunks.terrainRuntimePageTable.pages.size() &&
            chunks.terrainRuntimePageTable.pages[pageIndex].nodeCount != 0u;
      });
  const bool hasRequestableAbsentPage = std::any_of(
      cache.pageDirectory.entries.begin(), cache.pageDirectory.entries.end(),
      [&](const svr::WorldVoxelPageDirectoryEntry &entry)
      {
        const uint32_t pageIndex = cache.pageDirectory.find(entry.address);
        return entry.state == svr::WorldVoxelPageState::Absent &&
            pageIndex < chunks.terrainRuntimePageTable.pages.size() &&
            (chunks.terrainRuntimePageTable.pages[pageIndex].flags &
             svr::kWorldVoxelTerrainRuntimePageRequestable) != 0u &&
            chunks.terrainRuntimePageTable.pages[pageIndex].nodeCount == 0u;
      });
  return hasResidentNodeSpan && hasRequestableAbsentPage;
}

svr::SparseVoxelCenterLengthGPU unitLeafAtCell(
    const int32_t x, const int32_t y, const int32_t z)
{
  return {{
      static_cast<float>(x) + 0.5f,
      static_cast<float>(y) + 0.5f,
      static_cast<float>(z) + 0.5f}, 1.0f};
}

svr::SparseVoxelMaterialGPU solidMaterial(const uint32_t albedo)
{
  svr::SparseVoxelMaterialGPU material{};
  material.albedoRGBA8 = albedo | 0xff000000u;
  material.roughnessMetallicUNorm8 = 0x00000080u;
  return material;
}

bool nearlyEqual(const float left, const float right)
{
  return std::abs(left - right) <= 1.0e-5f *
      std::max(1.0f, std::max(std::abs(left), std::abs(right)));
}

void checkBrickLODGroupsAreStableClusterMetadata()
{
  std::vector<svr::SparseVoxelCenterLengthGPU> leaves{
      unitLeafAtCell(-8, 0, 0),
      unitLeafAtCell(0, 0, 0),
      unitLeafAtCell(8, 0, 0),
      unitLeafAtCell(15, 0, 0),
      unitLeafAtCell(16, 0, 0)};
  std::vector<svr::SparseVoxelMaterialGPU> materials{
      solidMaterial(0x00302010u),
      solidMaterial(0x00403020u),
      solidMaterial(0x00504030u),
      solidMaterial(0x00605040u),
      solidMaterial(0x00706050u)};
  const std::vector<uint32_t> indices{0u, 1u, 2u, 3u, 4u};

  svr::SparseVoxelBrickBuildSettings settings{};
  settings.voxelSize = 1.0f;
  settings.lodGroupSideBricks = 2u;

  std::string error;
  svr::SparseVoxelBrickHierarchy first{};
  const bool built = svr::buildSparseVoxelBrickHierarchy(
      leaves, materials, indices, settings, first, &error);
  check(built, ("brick LOD group fixture builds: " + error).c_str());
  if (!built)
    return;

  check(!first.lodGroups.empty(), "brick hierarchy emits LOD groups by default");
  check(
      first.nodeLODGroupIndices.size() == first.nodes.size(),
      "every brick node has a LOD group mapping");
  check(
      first.representativeLODGroupIndices.size() == first.nodes.size(),
      "every brick node has a representative-group mapping");
  check(
      first.lodGroupSideBricks == 2u,
      "LOD group side length is recorded in bricks");

  std::vector<uint32_t> seen(first.nodes.size(), 0u);
  bool validRanges = true;
  bool validErrors = true;
  bool validBounds = true;
  bool validBacklinks = true;
  bool validGroupFlags = true;
  constexpr uint32_t allowedLODGroupFlags =
      svr::kSparseVoxelBrickLODGroupBoundaryMask |
      svr::kSparseVoxelBrickLODGroupSurfaceRepresentativeCertified;
  bool validRepresentatives = true;
  for (uint32_t groupIndex = 0u; groupIndex < first.lodGroups.size(); ++groupIndex)
  {
    const svr::SparseVoxelBrickLODGroupGPU &group =
        first.lodGroups[groupIndex];
    validRanges = validRanges && group.nodeCount > 0u &&
        uint64_t{group.firstNode} + group.nodeCount <=
            first.lodGroupNodeIndices.size();
    validErrors = validErrors &&
        nearlyEqual(group.sharedError, group.maximumError) &&
        group.minimumError <= group.maximumError;
    validGroupFlags = validGroupFlags &&
        (group.flags & ~allowedLODGroupFlags) == 0u;
    const int64_t groupSide = group.canonicalGridOriginScale[3];
    validBounds = validBounds && groupSide > 0 &&
        nearlyEqual(
            group.minimumExtent[3],
            static_cast<float>(static_cast<double>(groupSide) *
                               static_cast<double>(first.finestVoxelSize)));
    for (uint32_t member = 0u; member < group.nodeCount; ++member)
    {
      const uint32_t nodeIndex =
          first.lodGroupNodeIndices[group.firstNode + member];
      if (nodeIndex >= first.nodes.size())
      {
        validRanges = false;
        continue;
      }
      ++seen[nodeIndex];
      validBacklinks = validBacklinks &&
          first.nodeLODGroupIndices[nodeIndex] == groupIndex;
      const svr::SparseVoxelBrickNodeGPU &node = first.nodes[nodeIndex];
      validErrors = validErrors &&
          node.selfError + 1.0e-6f >= group.minimumError &&
          node.selfError <= group.maximumError + 1.0e-6f &&
          node.selfError <= group.sharedError + 1.0e-6f;
      validBounds = validBounds && group.level == (node.level & 0xffu);
      const int64_t nodeSpan =
          int64_t{svr::kSparseVoxelBrickSide} *
          static_cast<int64_t>(node.canonicalGridOriginScale[3]);
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        const int64_t groupOrigin = group.canonicalGridOriginScale[axis];
        const int64_t nodeOrigin = node.canonicalGridOriginScale[axis];
        validBounds = validBounds &&
            nodeOrigin >= groupOrigin &&
            nodeOrigin + nodeSpan <= groupOrigin + groupSide &&
            nearlyEqual(
                group.minimumExtent[axis],
                static_cast<float>(
                    static_cast<double>(groupOrigin) *
                    static_cast<double>(first.finestVoxelSize)));
      }
    }
    if (group.representativeNode != svr::kSparseVoxelBrickLODGroupInvalidIndex)
    {
      validRepresentatives =
          validRepresentatives &&
          group.representativeNode < first.nodes.size();
      if (group.representativeNode < first.nodes.size())
      {
        const svr::SparseVoxelBrickNodeGPU &representative =
            first.nodes[group.representativeNode];
        validRepresentatives = validRepresentatives &&
            (representative.level & 0xffu) == group.level + 1u &&
            nearlyEqual(group.sharedError, representative.selfError);
        const int64_t representativeSpan =
            int64_t{svr::kSparseVoxelBrickSide} *
            static_cast<int64_t>(
                representative.canonicalGridOriginScale[3]);
        validRepresentatives =
            validRepresentatives &&
            representativeSpan == group.canonicalGridOriginScale[3];
        for (uint32_t axis = 0u; axis < 3u; ++axis)
          validRepresentatives = validRepresentatives &&
              representative.canonicalGridOriginScale[axis] ==
                  group.canonicalGridOriginScale[axis];
        validRepresentatives = validRepresentatives &&
            first.representativeLODGroupIndices[group.representativeNode] ==
                groupIndex;
      }
    }
  }
  check(validRanges, "LOD group member ranges are valid");
  check(validBacklinks, "node-to-LOD-group backlinks are valid");
  check(
      std::all_of(seen.begin(), seen.end(), [](const uint32_t count)
      {
        return count == 1u;
      }),
      "each brick node appears in exactly one LOD group");
  check(validErrors, "LOD group shared error conservatively encloses members");
  check(validBounds, "LOD group canonical bounds enclose member bricks");
  check(validGroupFlags, "LOD group flags use only silhouette and seam-certification bits");
  check(validRepresentatives, "LOD group representative nodes match group bounds");

  uint32_t groupAtNegativeX = svr::kSparseVoxelBrickLODGroupInvalidIndex;
  uint32_t groupAtZeroX = svr::kSparseVoxelBrickLODGroupInvalidIndex;
  uint32_t groupAtEightX = svr::kSparseVoxelBrickLODGroupInvalidIndex;
  uint32_t groupAtSixteenX = svr::kSparseVoxelBrickLODGroupInvalidIndex;
  for (uint32_t nodeIndex = 0u; nodeIndex < first.nodes.size(); ++nodeIndex)
  {
    const svr::SparseVoxelBrickNodeGPU &node = first.nodes[nodeIndex];
    if ((node.level & 0xffu) != 0u ||
        node.canonicalGridOriginScale[1] != 0 ||
        node.canonicalGridOriginScale[2] != 0)
      continue;
    switch (node.canonicalGridOriginScale[0])
    {
    case -8: groupAtNegativeX = first.nodeLODGroupIndices[nodeIndex]; break;
    case 0: groupAtZeroX = first.nodeLODGroupIndices[nodeIndex]; break;
    case 8: groupAtEightX = first.nodeLODGroupIndices[nodeIndex]; break;
    case 16: groupAtSixteenX = first.nodeLODGroupIndices[nodeIndex]; break;
    default: break;
    }
  }
  check(
      groupAtZeroX != svr::kSparseVoxelBrickLODGroupInvalidIndex &&
          groupAtZeroX == groupAtEightX,
      "adjacent bricks inside a macro-cell share one LOD group");
  check(
      groupAtNegativeX != svr::kSparseVoxelBrickLODGroupInvalidIndex &&
          groupAtNegativeX != groupAtZeroX,
      "negative brick coordinates use stable floor-divided LOD groups");
  check(
      groupAtSixteenX != svr::kSparseVoxelBrickLODGroupInvalidIndex &&
          groupAtSixteenX != groupAtZeroX,
      "LOD groups split on the configured brick macro-cell boundary");
  const bool zeroGroupMarksBothXSilhouetteEdges =
      groupAtZeroX < first.lodGroups.size() &&
      (first.lodGroups[groupAtZeroX].flags &
       svr::kSparseVoxelBrickLODGroupBoundaryMinusX) != 0u &&
      (first.lodGroups[groupAtZeroX].flags &
       svr::kSparseVoxelBrickLODGroupBoundaryPlusX) != 0u;
  check(
      zeroGroupMarksBothXSilhouetteEdges,
      "LOD group records occupied silhouettes on macro-cell boundaries");
  check(
      groupAtZeroX < first.lodGroups.size() &&
          first.lodGroups[groupAtZeroX].representativeNode !=
              svr::kSparseVoxelBrickLODGroupInvalidIndex,
      "2x2 child macro-cell stores the parent simplification node");

  svr::SparseVoxelBrickHierarchy second{};
  error.clear();
  const bool rebuilt = svr::buildSparseVoxelBrickHierarchy(
      leaves, materials, indices, settings, second, &error);
  check(rebuilt, ("brick LOD group fixture rebuilds: " + error).c_str());
  if (rebuilt)
  {
    const bool sameGroups =
        first.lodGroups.size() == second.lodGroups.size() &&
        (first.lodGroups.empty() ||
         std::memcmp(
             first.lodGroups.data(),
             second.lodGroups.data(),
             first.lodGroups.size() *
                 sizeof(svr::SparseVoxelBrickLODGroupGPU)) == 0);
    check(sameGroups, "LOD group metadata is deterministic across rebuilds");
    check(
        first.lodGroupNodeIndices == second.lodGroupNodeIndices &&
            first.nodeLODGroupIndices == second.nodeLODGroupIndices &&
            first.representativeLODGroupIndices ==
                second.representativeLODGroupIndices,
        "LOD group membership tables are deterministic across rebuilds");
  }

  settings.lodGroupSideBricks = 0u;
  svr::SparseVoxelBrickHierarchy disabled{};
  error.clear();
  const bool disabledBuilt = svr::buildSparseVoxelBrickHierarchy(
      leaves, materials, indices, settings, disabled, &error);
  check(
      disabledBuilt && disabled.lodGroups.empty() &&
          disabled.lodGroupNodeIndices.empty() &&
          disabled.nodeLODGroupIndices.empty() &&
          disabled.representativeLODGroupIndices.empty(),
      "LOD group side length zero disables sidecar metadata");
}

} // namespace

int main()
{
  std::cout << "generated-world page determinism\n";

  checkBrickLODGroupsAreStableClusterMetadata();

  const MapExportBundle bundle = reliefFixture();
  std::string error;

  size_t voxelCount = 0u;
  const auto baseSettings = pageSettings(0.40, 0.40, 0.005, 0u, 1u);
  svr::PlanetVoxelChunkSet firstPage{};
  svr::buildWorldMapVoxelTerrain(bundle, baseSettings, firstPage, &error);
  const uint64_t first = generatedPageHash(
      bundle, baseSettings, &voxelCount, &error);
  check(
      first != 0u,
      ("a page builds from the relief fixture: " + error).c_str());
  check(voxelCount > 64u, "the generated page holds a non-trivial voxel set");
  const bool hasAnalyticTerrainPatch = std::any_of(
      firstPage.bricks.microSurfaceData.begin(),
      firstPage.bricks.microSurfaceData.end(),
      [](const uint64_t packedContext)
      {
        return svr::unpackSparseVoxelMicroContextSurfacePatch(
                   packedContext) != 0u &&
            svr::sparseVoxelMicroContextIsTerrainSurface(
                svr::unpackSparseVoxelMicroContextValue(packedContext));
      });
  check(
      !hasAnalyticTerrainPatch,
      "generated relief keeps raster and shadow geometry on square voxels");

  auto tenCentimetreSettings =
      pageSettings(0.40, 0.40, 3.2 / bundle.extentMetres, 0u, 1u);
  tenCentimetreSettings.finestVoxelSizeMetres = 0.10;
  tenCentimetreSettings.voxel.patchEdgeMetres = 3.2;
  svr::PlanetVoxelChunkSet tenCentimetrePage{};
  error.clear();
  const bool tenCentimetreBuilt = svr::buildWorldMapVoxelTerrain(
      bundle, tenCentimetreSettings, tenCentimetrePage, &error);
  const auto tenCentimetreHeightRange = tenCentimetreBuilt &&
          !tenCentimetrePage.surfaceHeights.empty()
      ? std::minmax_element(
            tenCentimetrePage.surfaceHeights.begin(),
            tenCentimetrePage.surfaceHeights.end())
      : std::pair{
            tenCentimetrePage.surfaceHeights.end(),
            tenCentimetrePage.surfaceHeights.end()};
  check(
      tenCentimetreBuilt && !tenCentimetrePage.chunks.empty() &&
          std::abs(
              tenCentimetrePage.chunks.front().voxelSizeMetres - 0.10) <
              1.0e-9 &&
          std::abs(tenCentimetrePage.chunks.front().edgeMetres - 3.2) <
              1.0e-9,
      ("a generated page retains the default 10 cm voxel scale: " + error)
          .c_str());
  check(
      tenCentimetreBuilt &&
          tenCentimetreHeightRange.first !=
              tenCentimetrePage.surfaceHeights.end() &&
          *tenCentimetreHeightRange.second -
                  *tenCentimetreHeightRange.first >
              0.01f,
      "a 10 cm page preserves non-flat heightmap relief");

  MapExportBundle flatBundle = reliefFixture();
  *flatBundle.field("refined_elevation") = constantField(
      "refined_elevation", MapExportFormat::R16Unorm,
      MapExportFilter::Linear, {0x00u, 0x80u}, 32u, 1u);
  flatBundle.field("refined_elevation")->scale[0] = 1000.0;
  auto flatSettings = pageSettings(0.40, 0.40, 0.005, 0u, 1u);
  flatSettings.voxel.proceduralDetailStrength = 0.0;
  svr::PlanetVoxelChunkSet flatPage{};
  error.clear();
  const bool flatBuilt = svr::buildWorldMapVoxelTerrain(
      flatBundle, flatSettings, flatPage, &error);
  const auto flatRange = flatBuilt && !flatPage.surfaceHeights.empty()
      ? std::minmax_element(
            flatPage.surfaceHeights.begin(), flatPage.surfaceHeights.end())
      : std::pair{
            flatPage.surfaceHeights.end(), flatPage.surfaceHeights.end()};
  flatSettings.voxel.proceduralDetailStrength = 1.0;
  svr::PlanetVoxelChunkSet detailedPage{};
  const bool detailedBuilt = svr::buildWorldMapVoxelTerrain(
      flatBundle, flatSettings, detailedPage, &error);
  const auto detailedRange = detailedBuilt && !detailedPage.surfaceHeights.empty()
      ? std::minmax_element(
            detailedPage.surfaceHeights.begin(),
            detailedPage.surfaceHeights.end())
      : std::pair{
            detailedPage.surfaceHeights.end(),
            detailedPage.surfaceHeights.end()};
  check(
      flatBuilt && flatRange.first != flatPage.surfaceHeights.end() &&
          *flatRange.second - *flatRange.first < 1.0e-4f,
      "zero procedural detail preserves a flat exported height field");
  check(
      detailedBuilt &&
          detailedRange.first != detailedPage.surfaceHeights.end() &&
          *detailedRange.second - *detailedRange.first > 0.10f,
      "default procedural detail reconstructs sub-texel world-map relief");

  // Exercise the shipping finest page shape, not a reduced unit-test page.
  // The source map can have kilometre-wide elevation texels; this assertion
  // ensures that a 12.8 m page still receives terrain variation of its own.
  auto finestSettings = pageSettings(
      0.40, 0.40, 12.8 / flatBundle.extentMetres, 0u, 1u);
  finestSettings.finestHorizontalVoxelCount = 128u;
  finestSettings.finestVoxelSizeMetres = 0.10;
  finestSettings.voxel.horizontalVoxelCount = 128u;
  finestSettings.voxel.patchEdgeMetres = 12.8;
  finestSettings.voxel.maximumBricks = 1u << 20u;
  finestSettings.voxel.proceduralDetailStrength = 2.0;
  svr::PlanetVoxelChunkSet finestPage{};
  error.clear();
  const bool finestBuilt = svr::buildWorldMapVoxelTerrain(
      flatBundle, finestSettings, finestPage, &error);
  const auto finestRange = finestBuilt && !finestPage.surfaceHeights.empty()
      ? std::minmax_element(
            finestPage.surfaceHeights.begin(),
            finestPage.surfaceHeights.end())
      : std::pair{
            finestPage.surfaceHeights.end(),
            finestPage.surfaceHeights.end()};
  const size_t quantizedHeightCount = finestBuilt
      ? [&]
        {
          std::vector<int32_t> levels;
          levels.reserve(finestPage.surfaceHeights.size());
          for (const float height : finestPage.surfaceHeights)
            levels.push_back(static_cast<int32_t>(std::llround(height * 10.0)));
          std::sort(levels.begin(), levels.end());
          return static_cast<size_t>(
              std::unique(levels.begin(), levels.end()) - levels.begin());
        }()
      : 0u;
  check(
      finestBuilt && finestRange.first != finestPage.surfaceHeights.end() &&
          *finestRange.second - *finestRange.first > 0.20f &&
          quantizedHeightCount >= 3u,
      ("the shipping 12.8 m / 10 cm page has resolvable fine relief: " + error)
          .c_str());

  // The property the cache depends on: dropping a page and building it again
  // reproduces it byte for byte.
  const uint64_t regenerated = generatedPageHash(bundle, baseSettings);
  check(
      first != 0u && regenerated == first,
      "regenerating a page reproduces it byte for byte");

  // Worker count is a scheduling choice, not an input. If shell extraction or
  // brick assembly ever depends on the order pages or columns complete, this is
  // where it shows.
  bool workerStable = first != 0u;
  for (const uint32_t workers : {1u, 2u, 4u, 8u})
  {
    const auto settings = pageSettings(0.40, 0.40, 0.005, 0u, workers);
    workerStable &= generatedPageHash(bundle, settings) == first;
  }
  check(workerStable, "worker count does not change the generated bytes");

  // Guard against a fixture so uniform that everything hashes alike, which
  // would make every assertion above pass for the wrong reason.
  const uint64_t elsewhere =
      generatedPageHash(bundle, pageSettings(0.60, 0.30, 0.005, 0u, 1u));
  check(
      elsewhere != 0u && elsewhere != first,
      "a different page of the same world hashes differently");

  const uint64_t coarser =
      generatedPageHash(bundle, pageSettings(0.40, 0.40, 0.005, 1u, 1u));
  check(
      coarser != 0u && coarser != first,
      "a coarser internal LOD of the same footprint hashes differently");

  // Order independence. Building other pages in between must not change this
  // one: the streamer generates pages in whatever order the budget and the
  // priority ranking produce, and a page evicted early is rebuilt much later
  // beside an entirely different set of neighbours.
  const uint64_t afterOthers = generatedPageHash(bundle, baseSettings);
  check(
      afterOthers != 0u && afterOthers == first,
      "generating other pages first does not change this one");

  // Determinism has to survive the round trip the store actually performs,
  // not just repeated generation.
  {
    svr::PlanetVoxelChunkSet page{};
    const bool built =
        svr::buildWorldMapVoxelTerrain(bundle, baseSettings, page, &error);
    const std::vector<uint8_t> bytes =
        svr::serializeWorldVoxelTransientPage(page);
    svr::PlanetVoxelChunkSet restored{};
    error.clear();
    const bool decoded =
        svr::deserializeWorldVoxelTransientPage(bytes, restored, &error);
    check(
        built && decoded &&
            hashBytes(svr::serializeWorldVoxelTransientPage(restored)) == first,
        "a page restored from the cache hashes as the generator built it");
  }

  std::cout << "cache key sensitivity\n";

  const auto settings = keySettings();
  const svr::WorldVoxelChunkAddress address{3, 5, 2u};
  const auto reference = svr::worldTransientPageKey(0x1234u, settings, address, 0u);

  check(
      sameKey(reference, svr::worldTransientPageKey(0x1234u, settings, address, 0u)),
      "the key is stable for identical inputs");

  // Each of these changes the bytes the generator produces, so each has to
  // change the key. A key that ignores one of them restores a page built under
  // different rules.
  const auto differs = [&](svr::WorldVoxelChunkStreamingSettings changed,
                           const char *what)
  {
    check(
        !sameKey(
            reference, svr::worldTransientPageKey(0x1234u, changed, address, 0u)),
        what);
  };
  auto changed = settings;
  changed.generatorRevision = settings.generatorRevision + 1u;
  differs(changed, "the generator revision changes the key");
  changed = settings;
  changed.chunkEdgeMetres = 64.0;
  differs(changed, "the page edge changes the key");
  changed = settings;
  changed.finestVoxelSizeMetres = 0.5;
  differs(changed, "the finest voxel size changes the key");
  changed = settings;
  changed.finestHorizontalVoxelCount = 64u;
  differs(changed, "the page resolution changes the key");
  changed = settings;
  changed.voxel.surfaceShellDepthVoxels = 4u;
  differs(changed, "the shell depth changes the key");
  changed = settings;
  changed.voxel.foundationDepthVoxels = 8u;
  differs(changed, "the foundation depth changes the key");
  changed = settings;
  changed.voxel.maximumVerticalVoxels = 128u;
  differs(changed, "the vertical voxel bound changes the key");
  changed = settings;
  changed.voxel.surfaceEnvelopeRadius = 2u;
  differs(changed, "the envelope radius changes the key");
  changed = settings;
  changed.voxel.proceduralDetailStrength = 0.5;
  differs(changed, "the procedural detail strength changes the key");
  changed = settings;
  changed.voxel.proceduralTerraceStrength = 0.5;
  differs(changed, "the procedural terrace strength changes the key");
  changed = settings;
  changed.voxel.proceduralTerraceHeightMetres = 32.0;
  differs(changed, "the procedural terrace height changes the key");
  changed = settings;
  changed.voxel.proceduralTerraceTransitionFraction = 0.24;
  differs(changed, "the procedural terrace transition changes the key");
  changed = settings;
  changed.voxel.microGeometry = true;
  differs(changed, "enabling micro geometry changes the key");
  changed = settings;
  changed.voxel.microMaterialRevision = 7u;
  differs(changed, "the micro material revision changes the key");

  check(
      !sameKey(reference, svr::worldTransientPageKey(0x1235u, settings, address, 0u)),
      "a different bundle changes the key");
  check(
      !sameKey(
          reference,
          svr::worldTransientPageKey(0x1234u, settings, {4, 5, 2u}, 0u)),
      "a different page address changes the key");
  check(
      !sameKey(
          reference,
          svr::worldTransientPageKey(0x1234u, settings, {3, 5, 1u}, 0u)),
      "a different spatial level changes the key");
  check(
      !sameKey(reference, svr::worldTransientPageKey(0x1234u, settings, address, 1u)),
      "a different internal LOD changes the key");

  // Residency policy does not change a single generated byte, so folding it in
  // would evict the whole cache whenever a budget moved.
  changed = settings;
  changed.maximumCachedPayloads = 4096u;
  changed.maximumResidentChunks = 999u;
  changed.maximumGeneratedChunksPerUpdate = 7u;
  changed.readerFrame = 42u;
  changed.priorityViewDirectionX = 0.5;
  check(
      sameKey(reference, svr::worldTransientPageKey(0x1234u, changed, address, 0u)),
      "residency budgets and view priority leave the key alone");

  std::cout << "page source cache path\n";

  svr::WorldVoxelChunkStreamingSettings streaming = settings;
  streaming.renderDistanceMetres = 0.0;
  streaming.projectionScalePixels = 0.0;
  streaming.residentRadius = 0u;
  streaming.maximumResidentChunks = 1u;
  streaming.maximumGeneratedChunksPerUpdate = 1u;
  streaming.maximumTransientMemoryBytes = 1u;
  streaming.maximumTransientDiskBytes = 64ull * 1024ull * 1024ull;
  streaming.voxel.workerCount = 1u;

  svr::WorldVoxelChunkCache sourceCache{};
  svr::PlanetVoxelChunkSet sourceChunks{};
  svr::WorldVoxelChunkCacheUpdate sourceUpdate{};
  error.clear();
  const bool sourceBuilt = svr::updateWorldVoxelChunkCache(
      bundle, 0.0, 0.0, streaming, sourceCache, sourceChunks, &sourceUpdate,
      &error);
  check(
      sourceBuilt && sourceUpdate.storedTransientChunkCount == 1u &&
          sourceUpdate.restoredTransientDiskChunkCount == 0u &&
          sourceUpdate.restoredTransientMemoryChunkCount == 0u &&
          sourceCache.transientPages &&
          sourceCache.transientPages->statistics().diskPageCount == 1u,
      "generated pages are written to the transient page source immediately");

  svr::WorldVoxelChunkCache reloadCache{};
  reloadCache.transientPages = sourceCache.transientPages;
  svr::PlanetVoxelChunkSet restoredChunks{};
  svr::WorldVoxelChunkCacheUpdate reloadUpdate{};
  error.clear();
  const bool reloaded = svr::updateWorldVoxelChunkCache(
      bundle, 0.0, 0.0, streaming, reloadCache, restoredChunks, &reloadUpdate,
      &error);
  check(
      reloaded && reloadUpdate.restoredCachedChunkCount == 1u &&
          reloadUpdate.restoredTransientDiskChunkCount == 1u &&
          reloadUpdate.storedTransientChunkCount == 0u &&
          reloadUpdate.mapSamplingMilliseconds == 0.0 &&
          hashBytes(svr::serializeWorldVoxelTransientPage(restoredChunks)) ==
              hashBytes(svr::serializeWorldVoxelTransientPage(sourceChunks)),
      "a fresh cache restores generated pages from the transient source "
      "without CPU generation");

  std::cout << "spatial directory integration\n";

  svr::WorldVoxelChunkStreamingSettings spatial = settings;
  spatial.renderDistanceMetres = 64.0;
  spatial.projectionScalePixels = 128.0;
  spatial.splitErrorPixels = 1.0f;
  spatial.mergeErrorPixels = 0.5f;
  spatial.maximumSpatialLOD = 4u;
  spatial.maximumResidentChunks = 64u;
  spatial.maximumGeneratedChunksPerUpdate = 1u;
  spatial.maximumTransientMemoryBytes = 64ull * 1024ull * 1024ull;
  spatial.maximumTransientDiskBytes = 0u;
  spatial.voxel.workerCount = 1u;

  svr::WorldVoxelChunkCache spatialCache{};
  svr::PlanetVoxelChunkSet spatialChunks{};
  svr::WorldVoxelChunkCacheUpdate spatialUpdate{};
  error.clear();
  const bool spatialBuilt = svr::updateWorldVoxelChunkCache(
      bundle, 0.0, 0.0, spatial, spatialCache, spatialChunks, &spatialUpdate,
      &error);
  const auto hasPayloadKey =
      [](const svr::WorldVoxelPageDirectoryEntry &entry)
  {
    return entry.payloadKey.source != 0u && entry.payloadKey.page != 0u;
  };
  const auto isAbsent = [](const svr::WorldVoxelPageDirectoryEntry &entry)
  {
    return entry.state == svr::WorldVoxelPageState::Absent;
  };
  const auto isResident = [](const svr::WorldVoxelPageDirectoryEntry &entry)
  {
    return entry.state == svr::WorldVoxelPageState::Resident;
  };
  check(
      spatialBuilt && spatialUpdate.directoryEntryCount >
          spatialCache.entries.size(),
      "a spatial update publishes a directory beyond resident bootstrap pages");
  check(
      spatialBuilt && spatialUpdate.directoryCoveredCount >=
          spatialCache.entries.size(),
      "the directory is built from the requested cover before page install");
  check(
      spatialBuilt && spatialUpdate.directoryAbsentCount > 0u &&
          std::any_of(
              spatialCache.pageDirectory.entries.begin(),
              spatialCache.pageDirectory.entries.end(), isAbsent),
      "requested directory pages remain nameable while no payload is held");
  check(
      spatialBuilt && spatialUpdate.directoryPayloadRequestCount > 0u &&
          spatialUpdate.directoryBlockedInstallCount >=
              spatialUpdate.directoryPayloadRequestCount,
      "the spatial directory exposes absent pages as streaming requests");
  check(
      spatialBuilt &&
          std::all_of(
              spatialCache.pageDirectory.entries.begin(),
              spatialCache.pageDirectory.entries.end(), hasPayloadKey),
      "every spatial directory page has its deterministic payload key");
  check(
      spatialBuilt &&
          static_cast<size_t>(std::count_if(
              spatialCache.pageDirectory.entries.begin(),
              spatialCache.pageDirectory.entries.end(), isResident)) ==
              spatialCache.entries.size(),
      "resident bootstrap pages are reflected in directory state");
  check(
      spatialBuilt && spatialUpdate.physicalUploadPageCount ==
          spatialChunks.terrainPageUploads.size() &&
          spatialUpdate.physicalUploadBytes > 0u,
      "the bootstrap publication exposes physical page upload packets");
  check(
      spatialBuilt &&
          allPublishedNodesHaveFlag(
              spatialChunks, svr::kSparseVoxelBrickForceLeafRaster),
      "compatibility spatial terrain publication forces authored leaf raster");
  check(
      spatialBuilt && packedVisibleRootPrefixValid(spatialChunks),
      "spatial virtual pages publish a packed visible root prefix");
  check(
      spatialBuilt &&
          runtimePageTableValid(spatialCache, spatialChunks, error),
      "spatial virtual pages publish a runtime page/node table");

  const auto uploadRangesValid =
      [](const svr::PlanetVoxelChunkSet &chunkSet,
         const svr::PlanetVoxelTerrainPageUpload &upload)
  {
    const auto validRange = [](const uint32_t offset, const uint32_t count,
                               const size_t size)
    {
      return static_cast<uint64_t>(offset) + count <= size;
    };
    return upload.identity.valid() &&
        validRange(
            upload.nodes.offset, upload.nodes.count,
            chunkSet.bricks.nodes.size()) &&
        validRange(
            upload.occupancy.offset, upload.occupancy.count,
            chunkSet.bricks.occupancy.size()) &&
        validRange(
            upload.childIndices.offset, upload.childIndices.count,
            chunkSet.bricks.childIndices.size()) &&
        validRange(
            upload.roots.offset, upload.roots.count,
            chunkSet.bricks.rootIndices.size()) &&
        validRange(
            upload.materialIndices.offset, upload.materialIndices.count,
            chunkSet.bricks.materialIndices.size()) &&
        validRange(
            upload.microSurfaceData.offset, upload.microSurfaceData.count,
            chunkSet.bricks.microSurfaceData.size()) &&
        validRange(
            upload.palette.offset, upload.palette.count,
            chunkSet.bricks.palette.size()) &&
        validRange(
            upload.paletteDescriptorIds.offset,
            upload.paletteDescriptorIds.count,
            chunkSet.bricks.paletteDescriptorIds.size()) &&
        upload.payloadBytes > 0u && upload.publicationBytes > 0u &&
        !upload.installPatches.empty();
  };
  check(
      spatialBuilt &&
          std::all_of(
              spatialChunks.terrainPageUploads.begin(),
              spatialChunks.terrainPageUploads.end(),
              [&](const svr::PlanetVoxelTerrainPageUpload &upload)
              {
                return upload.visible &&
                    uploadRangesValid(spatialChunks, upload);
              }),
      "visible physical upload packets name valid arena ranges and patches");

  bool sawHiddenPhysicalUpload = false;
  for (uint32_t step = 0u;
       spatialBuilt && !sawHiddenPhysicalUpload && step < 8u; ++step)
  {
    svr::PlanetVoxelChunkSet nextSpatialChunks{};
    svr::WorldVoxelChunkCacheUpdate nextSpatialUpdate{};
    error.clear();
    const bool stepped = svr::updateWorldVoxelChunkCache(
        bundle, 0.0, 0.0, spatial, spatialCache, nextSpatialChunks,
        &nextSpatialUpdate, &error);
    check(stepped, ("bounded spatial refinement step succeeds: " + error).c_str());
    if (!stepped)
      break;
    check(
        nextSpatialUpdate.physicalUploadPageCount ==
            nextSpatialChunks.terrainPageUploads.size(),
        "physical upload accounting matches the emitted packet count");
    check(
        runtimePageTableValid(spatialCache, nextSpatialChunks, error),
        "bounded refinement publishes a matching runtime page/node table");
    sawHiddenPhysicalUpload =
        nextSpatialUpdate.hiddenPhysicalUploadPageCount > 0u;
    if (sawHiddenPhysicalUpload)
    {
      check(
          !spatialCache.stagedSpatialEntries.empty(),
          "a hidden upload belongs to an invisible staged page");
      check(
          std::all_of(
              nextSpatialChunks.terrainPageUploads.begin(),
              nextSpatialChunks.terrainPageUploads.end(),
              [&](const svr::PlanetVoxelTerrainPageUpload &upload)
              {
                return uploadRangesValid(nextSpatialChunks, upload);
              }),
          "hidden physical upload packets name valid arena ranges and patches");
      check(
          nextSpatialUpdate.residentCutChanged == false,
          "a hidden page upload does not publish a partial ownership group");
    }
  }
  check(
      sawHiddenPhysicalUpload,
      "bounded refinement streams at least one hidden child page by delta");

  std::cout << "feedback-driven spatial page streaming\n";

  svr::WorldVoxelChunkStreamingSettings feedbackSpatial = spatial;
  feedbackSpatial.feedbackDrivenPageStreaming = true;
  feedbackSpatial.rasterSafeCoarseTerrain = true;
  feedbackSpatial.maximumGeneratedChunksPerUpdate = 4u;
  feedbackSpatial.maximumRefinedChunksPerUpdate = 4u;
  {
    svr::WorldVoxelChunkStreamingSettings unstableFeedbackSpatial =
        feedbackSpatial;
    unstableFeedbackSpatial.mergeErrorPixels = 0.75f;
    svr::WorldVoxelChunkCache unstableFeedbackCache{};
    svr::PlanetVoxelChunkSet unstableFeedbackChunks{};
    svr::WorldVoxelChunkCacheUpdate unstableFeedbackUpdate{};
    error.clear();
    const bool unstableBuilt = svr::updateWorldVoxelChunkCache(
        bundle, 0.0, 0.0, unstableFeedbackSpatial,
        unstableFeedbackCache, unstableFeedbackChunks,
        &unstableFeedbackUpdate, &error);
    check(
        !unstableBuilt,
        "feedback-driven spatial streaming rejects split/merge bands that "
        "cannot settle on a quadtree level");
  }

  svr::WorldVoxelChunkCache feedbackCache{};
  svr::PlanetVoxelChunkSet feedbackChunks{};
  svr::WorldVoxelChunkCacheUpdate feedbackBootstrap{};
  error.clear();
  const bool feedbackBootstrapBuilt = svr::updateWorldVoxelChunkCache(
      bundle, 0.0, 0.0, feedbackSpatial, feedbackCache, feedbackChunks,
      &feedbackBootstrap, &error);
  check(
      feedbackBootstrapBuilt,
      ("feedback-driven bootstrap succeeds: " + error).c_str());
  check(
      feedbackBootstrapBuilt &&
          noPublishedNodeHasFlag(
              feedbackChunks, svr::kSparseVoxelBrickForceLeafRaster),
      "feedback-driven generated terrain publishes measured-error coarse nodes");
  svr::PlanetVoxelChunkSet idleChunks{};
  svr::WorldVoxelChunkCacheUpdate idleUpdate{};
  error.clear();
  const bool idleBuilt = svr::updateWorldVoxelChunkCache(
      bundle, 0.0, 0.0, feedbackSpatial, feedbackCache, idleChunks,
      &idleUpdate, &error);
  check(
      idleBuilt,
      ("feedback-driven idle update succeeds: " + error).c_str());
  check(
      idleBuilt && idleUpdate.builtChunkCount > 0u &&
          idleUpdate.builtChunkCount <=
              feedbackSpatial.maximumGeneratedChunksPerUpdate &&
          idleUpdate.physicalUploadPageCount > 0u &&
          idleUpdate.pendingChunkCount > 0u,
      "bootstrap convergence metric-deepens inside its page budget");
  check(
      idleBuilt &&
          idleChunks.chunks.size() == feedbackCache.entries.size(),
      "every published generated-world chunk remains an independent instance");

  // The renderer has one persistent GPU page-build lane. A large CPU planning
  // budget must therefore not create a large hidden serial transaction: one
  // complete four-child group should be selected and become publishable while
  // the remaining priority set is reconsidered by the next camera snapshot.
  svr::WorldVoxelChunkStreamingSettings gpuLatencySpatial = feedbackSpatial;
  gpuLatencySpatial.gpuOwnedPayloads = true;
  gpuLatencySpatial.feedbackDrivenFallbackConvergence = true;
  gpuLatencySpatial.maximumGeneratedChunksPerUpdate = 64u;
  gpuLatencySpatial.maximumRefinedChunksPerUpdate = 64u;
  gpuLatencySpatial.maximumGPUOwnedPagesPerTransaction = 4u;
  svr::WorldVoxelChunkCache gpuLatencyCache{};
  svr::PlanetVoxelChunkSet gpuLatencyBootstrapChunks{};
  svr::WorldVoxelChunkCacheUpdate gpuLatencyBootstrapUpdate{};
  error.clear();
  const bool gpuLatencyBootstrap = svr::updateWorldVoxelChunkCache(
      bundle, 0.0, 0.0, gpuLatencySpatial, gpuLatencyCache,
      gpuLatencyBootstrapChunks, &gpuLatencyBootstrapUpdate, &error);
  check(
      gpuLatencyBootstrap,
      ("GPU-owned latency bootstrap succeeds: " + error).c_str());
  svr::PlanetVoxelChunkSet gpuLatencyChunks{};
  svr::WorldVoxelChunkCacheUpdate gpuLatencyUpdate{};
  error.clear();
  const bool gpuLatencyAdvanced = gpuLatencyBootstrap &&
      svr::updateWorldVoxelChunkCache(
          bundle, 0.0, 0.0, gpuLatencySpatial, gpuLatencyCache,
          gpuLatencyChunks, &gpuLatencyUpdate, &error);
  check(
      gpuLatencyAdvanced && gpuLatencyUpdate.builtChunkCount == 4u &&
          gpuLatencyChunks.terrainPageUploads.size() == 4u &&
          gpuLatencyUpdate.residentCutChanged &&
          gpuLatencyUpdate.pendingChunkCount > 0u,
      "a 64-page throughput budget emits one complete four-page GPU "
      "ownership transaction for low publication latency");

  svr::WorldVoxelChunkStreamingSettings motionFeedbackSpatial =
      feedbackSpatial;
  motionFeedbackSpatial.voxel.maximumVerticalVoxels = 2048u;
  svr::WorldVoxelChunkCache movedFeedbackCache{};
  svr::PlanetVoxelChunkSet motionBootstrapChunks{};
  svr::WorldVoxelChunkCacheUpdate motionBootstrap{};
  error.clear();
  const bool motionBootstrapBuilt = svr::updateWorldVoxelChunkCache(
      bundle, 0.0, 0.0, motionFeedbackSpatial, movedFeedbackCache,
      motionBootstrapChunks, &motionBootstrap, &error);
  check(
      motionBootstrapBuilt,
      ("feedback-driven motion bootstrap succeeds: " + error).c_str());
  svr::PlanetVoxelChunkSet movedFeedbackChunks{};
  svr::WorldVoxelChunkCacheUpdate movedFeedbackUpdate{};
  error.clear();
  const bool movedFeedbackBuilt = svr::updateWorldVoxelChunkCache(
      bundle, 1024.0, 0.0, motionFeedbackSpatial, movedFeedbackCache,
      movedFeedbackChunks, &movedFeedbackUpdate, &error);
  check(
      movedFeedbackBuilt,
      ("feedback-driven camera move succeeds: " + error).c_str());
  const bool feedbackCutStable = movedFeedbackBuilt &&
      !movedFeedbackCache.entries.empty() &&
      movedFeedbackCache.centerChunkX != feedbackCache.centerChunkX &&
      packedVisibleRootPrefixValid(movedFeedbackChunks) &&
      runtimePageTableValid(
          movedFeedbackCache, movedFeedbackChunks, error);
  check(
      feedbackCutStable,
      "camera motion during bootstrap convergence preserves a complete cut");
  bool movedCameraCovered = movedFeedbackBuilt && cacheCoversWorldPoint(
      movedFeedbackCache, motionFeedbackSpatial, bundle, 1024.0, 0.0);
  for (uint32_t updateIndex = 0u;
       movedFeedbackBuilt && !movedCameraCovered && updateIndex < 8u;
       ++updateIndex)
  {
    error.clear();
    const bool advanced = svr::updateWorldVoxelChunkCache(
        bundle, 1024.0, 0.0, motionFeedbackSpatial, movedFeedbackCache,
        movedFeedbackChunks, &movedFeedbackUpdate, &error);
    check(
        advanced,
        ("feedback-driven moved cut advances: " + error).c_str());
    if (!advanced)
      break;
    movedCameraCovered = cacheCoversWorldPoint(
        movedFeedbackCache, motionFeedbackSpatial, bundle, 1024.0, 0.0);
  }
  check(
      movedCameraCovered,
      "feedback-driven motion installs an owner beneath the moved camera");

  svr::WorldVoxelChunkStreamingSettings onePixelFeedbackSpatial =
      motionFeedbackSpatial;
  onePixelFeedbackSpatial.splitErrorPixels = 1.0f;
  onePixelFeedbackSpatial.mergeErrorPixels = 0.5f;
  svr::PlanetVoxelChunkSet onePixelFeedbackChunks{};
  svr::WorldVoxelChunkCacheUpdate onePixelFeedbackUpdate{};
  error.clear();
  const bool onePixelFeedbackBuilt = svr::updateWorldVoxelChunkCache(
      bundle, 1024.0, 0.0, onePixelFeedbackSpatial, movedFeedbackCache,
      onePixelFeedbackChunks, &onePixelFeedbackUpdate, &error);
  check(
      onePixelFeedbackBuilt,
      ("feedback-driven one-pixel metric update succeeds: " + error).c_str());
  check(
      onePixelFeedbackBuilt &&
          movedFeedbackCache.settings.splitErrorPixels == 1.0f &&
          packedVisibleRootPrefixValid(onePixelFeedbackChunks) &&
          runtimePageTableValid(
              movedFeedbackCache, onePixelFeedbackChunks, error),
      "feedback-driven metric update preserves a complete converging cut");

  // Keep a converged coarse fixture for explicit GPU feedback behavior. The
  // startup fixture above intentionally keeps deepening toward its CPU metric,
  // so an injected request there would correctly yield to that known target.
  // Deliberately suppress CPU metric refinement for the explicit GPU-feedback
  // fixture below. Near-field pages now correctly report a large projected
  // error, so the old 16/8 threshold no longer creates a coarse idle cut.
  feedbackSpatial.splitErrorPixels = 256.0f;
  feedbackSpatial.mergeErrorPixels = 128.0f;
  feedbackCache = {};
  feedbackChunks = {};
  feedbackBootstrap = {};
  error.clear();
  const bool feedbackRequestFixtureBuilt =
      svr::updateWorldVoxelChunkCache(
          bundle, 0.0, 0.0, feedbackSpatial, feedbackCache,
          feedbackChunks, &feedbackBootstrap, &error);
  check(
      feedbackRequestFixtureBuilt && feedbackCache.pendingChunkCount == 0u,
      ("feedback request fixture converges at its coarse metric: " + error)
          .c_str());

  // Shipping GPU streaming treats one completed culling frame as the exact
  // refinement demand.  Once that request is consumed, an empty frame must
  // preserve the published cut. Re-enabling the CPU fallback target here can
  // schedule unrelated convergence after the measured request, even though
  // that work is absent from the next frame's demand.
  auto frameDemandSettings = feedbackSpatial;
  frameDemandSettings.feedbackDrivenFallbackConvergence = false;
  auto frameDemandCache = feedbackCache;
  auto frameDemandOwner = std::find_if(
      frameDemandCache.entries.begin(), frameDemandCache.entries.end(),
      [](const svr::WorldVoxelChunkEntry &entry)
      { return entry.address.spatialLOD > 0u; });
  bool frameDemandStable = frameDemandOwner != frameDemandCache.entries.end();
  std::vector<svr::WorldVoxelChunkAddress> frameDemandCut;
  const auto addressLess = [](const svr::WorldVoxelChunkAddress &left,
                              const svr::WorldVoxelChunkAddress &right)
  {
    return std::tie(left.spatialLOD, left.z, left.x) <
        std::tie(right.spatialLOD, right.z, right.x);
  };
  if (frameDemandStable)
  {
    const uint32_t currentLOD = frameDemandOwner->address.spatialLOD;
    const std::vector<svr::WorldVoxelChunkRequest> visibleRequest{{
      frameDemandOwner->slot, frameDemandOwner->generation, currentLOD,
      currentLOD - 1u, 0xffffffffu}};
    svr::PlanetVoxelChunkSet frameDemandChunks{};
    svr::WorldVoxelChunkCacheUpdate frameDemandUpdate{};
    error.clear();
    frameDemandStable = svr::updateWorldVoxelChunkCache(
        bundle, 0.0, 0.0, frameDemandSettings, frameDemandCache,
        frameDemandChunks, &frameDemandUpdate, &error, {}, nullptr,
        &visibleRequest);
    for (const auto &entry : frameDemandCache.entries)
      frameDemandCut.push_back(entry.address);
    std::sort(frameDemandCut.begin(), frameDemandCut.end(), addressLess);

    // Capacity-deferred GPU demand is a backlog, not a new CPU target. The
    // idle frame must not coarsen the published cut just because it is nonzero.
    frameDemandCache.pendingChunkCount = 32u;
    frameDemandCache.spatialCutMetricConvergencePending = false;

    svr::PlanetVoxelChunkSet idleFrameChunks{};
    svr::WorldVoxelChunkCacheUpdate idleFrameUpdate{};
    frameDemandStable = frameDemandStable &&
        svr::updateWorldVoxelChunkCache(
            bundle, 0.0, 0.0, frameDemandSettings, frameDemandCache,
            idleFrameChunks, &idleFrameUpdate, &error);
    std::vector<svr::WorldVoxelChunkAddress> idleFrameCut;
    for (const auto &entry : frameDemandCache.entries)
      idleFrameCut.push_back(entry.address);
    std::sort(idleFrameCut.begin(), idleFrameCut.end(), addressLess);
    frameDemandStable = frameDemandStable &&
        !idleFrameUpdate.residentCutChanged &&
        frameDemandCache.pendingChunkCount == 0u &&
        idleFrameCut == frameDemandCut;
  }
  check(
      frameDemandStable,
      ("frame-local GPU refinement stays published when the next frame has no "
       "demand instead of scheduling unrelated fallback work: " + error)
          .c_str());

  // A pending broad metric convergence is fallback work, not a reason to
  // discard a visible GPU request that has already been removed from the
  // priority queue. Reproduce that state directly: the requested owner must
  // split now, while the unrelated metric backlog remains available for later
  // transactions.
  auto metricPendingFeedbackCache = feedbackCache;
  metricPendingFeedbackCache.spatialCutMetricConvergencePending = true;
  const auto metricPendingOwner = std::find_if(
      metricPendingFeedbackCache.entries.begin(),
      metricPendingFeedbackCache.entries.end(),
      [](const svr::WorldVoxelChunkEntry &entry)
      { return entry.address.spatialLOD > 0u; });
  bool metricPendingRequestWon = metricPendingOwner !=
      metricPendingFeedbackCache.entries.end();
  if (metricPendingRequestWon)
  {
    const svr::WorldVoxelChunkAddress ownerAddress =
        metricPendingOwner->address;
    const uint32_t currentLOD = std::min(
        ownerAddress.spatialLOD + metricPendingOwner->residentLOD, 31u);
    const std::vector<svr::WorldVoxelChunkRequest> visibleRequest{{
      metricPendingOwner->slot, metricPendingOwner->generation,
      currentLOD, currentLOD - 1u, 0xffffffffu}};
    svr::PlanetVoxelChunkSet metricPendingChunks{};
    svr::WorldVoxelChunkCacheUpdate metricPendingUpdate{};
    error.clear();
    metricPendingRequestWon = svr::updateWorldVoxelChunkCache(
        bundle, 0.0, 0.0, feedbackSpatial, metricPendingFeedbackCache,
        metricPendingChunks, &metricPendingUpdate, &error, {}, nullptr,
        &visibleRequest);
    uint32_t installedChildren = 0u;
    const uint32_t childLOD = ownerAddress.spatialLOD - 1u;
    for (int32_t childZ = 0; childZ != 2; ++childZ)
      for (int32_t childX = 0; childX != 2; ++childX)
      {
        const svr::WorldVoxelChunkAddress child{
          ownerAddress.x * 2 + childX,
          ownerAddress.z * 2 + childZ, childLOD};
        installedChildren += std::any_of(
            metricPendingFeedbackCache.entries.begin(),
            metricPendingFeedbackCache.entries.end(),
            [&](const svr::WorldVoxelChunkEntry &entry)
            { return entry.address == child; }) ? 1u : 0u;
      }
    metricPendingRequestWon = metricPendingRequestWon &&
        installedChildren == 4u && metricPendingUpdate.residentCutChanged &&
        metricPendingUpdate.refinedChunkCount >= 4u;
  }
  check(
      metricPendingRequestWon,
      "visible GPU refinement preempts pending broad metric convergence");

  // A measured flat page still has a quarter-voxel error floor.  With the old
  // half-page distance clamp, a 128-cell page under the camera evaluated to
  // focal / 256 pixels at every spatial LOD.  At a representative 500-pixel
  // focal length that is 1.95 px, so the shipping 2 px threshold retained the
  // coarse root directly under the eye.  The camera owner must instead reach
  // LOD 0 even when its measured error is the minimum possible value.
  svr::WorldVoxelChunkStreamingSettings nearFieldSettings{};
  nearFieldSettings.chunkEdgeMetres = 12.8;
  nearFieldSettings.finestVoxelSizeMetres = 0.10;
  nearFieldSettings.finestHorizontalVoxelCount = 128u;
  nearFieldSettings.renderDistanceMetres = 100.0;
  nearFieldSettings.projectionScalePixels = 500.0;
  nearFieldSettings.splitErrorPixels = 2.0f;
  nearFieldSettings.mergeErrorPixels = 0.8f;
  nearFieldSettings.maximumSpatialLOD = 8u;
  nearFieldSettings.maximumResidentChunks = 4096u;
  const svr::WorldVoxelChunkAddress measuredCameraRoot{31, 31, 3u};
  const svr::WorldVoxelMeasuredPageErrors nearFieldMeasuredErrors{{
      measuredCameraRoot,
      static_cast<float>(
          nearFieldSettings.finestVoxelSizeMetres *
          static_cast<double>(uint32_t{1u} << measuredCameraRoot.spatialLOD) *
          svr::kSparseVoxelBrickErrorFloor)}};
  const auto nearFieldCut = svr::buildWorldVoxelChunkLeafCut(
      bundle.extentMetres, 1.0, 1.0, nearFieldSettings,
      &nearFieldMeasuredErrors);
  check(
      std::find(
          nearFieldCut.begin(), nearFieldCut.end(),
          svr::WorldVoxelChunkAddress{250, 250, 0u}) != nearFieldCut.end(),
      "a minimum-error page intersecting the camera still refines to LOD 0");

  svr::WorldVoxelChunkStreamingSettings queueSettings = feedbackSpatial;
  queueSettings.chunkEdgeMetres = 10.0;
  queueSettings.finestVoxelSizeMetres = 10.0 / 32.0;
  queueSettings.finestHorizontalVoxelCount = 32u;
  queueSettings.renderDistanceMetres = 1000.0;
  queueSettings.priorityNearChunkRadius = 1u;
  queueSettings.priorityViewDirectionX = 1.0;
  queueSettings.priorityViewDirectionZ = 0.0;
  queueSettings.priorityViewCosine = 0.5;
  svr::WorldVoxelChunkCache queueCache{};
  queueCache.settings = queueSettings;
  const auto addQueueEntry =
      [&](const svr::WorldVoxelChunkAddress &address,
          const uint32_t slot)
  {
    svr::WorldVoxelChunkEntry entry{};
    entry.address = address;
    entry.slot = slot;
    entry.generation = 7u;
    entry.residentLOD = 0u;
    entry.requestedLOD = 0u;
    entry.state = svr::WorldVoxelChunkState::Resident;
    queueCache.entries.push_back(std::move(entry));
  };
  addQueueEntry({3, 0, 1u}, 1u);
  addQueueEntry({-2, 0, 1u}, 2u);
  addQueueEntry({1, 0, 1u}, 3u);
  const std::vector<svr::WorldVoxelChunkRequest> queueRequests{
    {1u, 7u, 1u, 0u, 0xffffffffu},
    {2u, 7u, 1u, 0u, 0xffffffffu},
    {3u, 7u, 1u, 0u, 1u}};

  svr::WorldVoxelChunkRequestQueue requestQueue;
  requestQueue.update(
      queueCache, 0.0, 10.0, queueSettings, queueRequests);
  const auto orderedRequests = requestQueue.take(
      queueCache, 0.0, 10.0, queueSettings, 0u);
  check(
      orderedRequests.size() == 3u &&
          orderedRequests[0].slot == 1u &&
          orderedRequests[1].slot == 2u &&
          orderedRequests[2].slot == 3u,
      "feedback priority queue orders refinement by measured pixel error "
      "before CPU distance tiers");

  // Runtime feedback is an exact snapshot of the latest completed culling
  // frame. Unadmitted work from an older view must not survive a replacement;
  // if it matters again, the next snapshot can request it again normally.
  svr::WorldVoxelChunkRequestQueue frameLocalQueue;
  frameLocalQueue.replaceFrame({queueRequests[0], queueRequests[1]});
  frameLocalQueue.update(queueCache, 0.0, 10.0, queueSettings);
  frameLocalQueue.replaceFrame({queueRequests[2]});
  const auto currentFrameOnly = frameLocalQueue.take(
      queueCache, 0.0, 10.0, queueSettings, 0u);
  check(
      currentFrameOnly.size() == 1u &&
          currentFrameOnly.front().slot == queueRequests[2].slot,
      "feedback frame replacement drops unadmitted requests from the old "
      "camera view");
  frameLocalQueue.replaceFrame({});
  check(
      frameLocalQueue.take(
          queueCache, 0.0, 10.0, queueSettings, 0u).empty(),
      "an empty culling frame clears all unadmitted terrain demand");
  frameLocalQueue.replaceFrame({queueRequests[0]});
  const auto requestedAgain = frameLocalQueue.take(
      queueCache, 0.0, 10.0, queueSettings, 1u);
  check(
      requestedAgain.size() == 1u &&
          requestedAgain.front().slot == queueRequests[0].slot,
      "a still-visible terrain owner can be requested again by a later "
      "culling frame");

  svr::WorldVoxelChunkRequestQueue distanceTieQueue;
  auto farVisible = queueRequests[0];
  auto nearVisible = queueRequests[2];
  farVisible.priority = nearVisible.priority = 100u;
  distanceTieQueue.replaceFrame({farVisible, nearVisible});
  const auto distanceOrdered = distanceTieQueue.take(
      queueCache, 0.0, 10.0, queueSettings, 0u);
  check(
      distanceOrdered.size() == 2u &&
          distanceOrdered[0].slot == nearVisible.slot &&
          distanceOrdered[1].slot == farVisible.slot,
      "equal screen-error terrain requests prefer the chunk closer to the "
      "camera");

  // Taking a request transfers it to an asynchronous generation job. If that
  // job is cancelled or cannot publish, putting the request back must preserve
  // both it and the owners which never left the queue.
  svr::WorldVoxelChunkRequestQueue cancelledRequestQueue;
  cancelledRequestQueue.update(
      queueCache, 0.0, 10.0, queueSettings, queueRequests);
  const auto cancelledInFlight = cancelledRequestQueue.take(
      queueCache, 0.0, 10.0, queueSettings, 1u);
  cancelledRequestQueue.enqueue(cancelledInFlight);
  cancelledRequestQueue.update(
      queueCache, 0.0, 10.0, queueSettings);
  const auto restoredAfterCancellation = cancelledRequestQueue.take(
      queueCache, 0.0, 10.0, queueSettings, 0u);
  std::vector<uint32_t> restoredSlots;
  restoredSlots.reserve(restoredAfterCancellation.size());
  for (const auto &request : restoredAfterCancellation)
    restoredSlots.push_back(request.slot);
  std::sort(restoredSlots.begin(), restoredSlots.end());
  check(
      cancelledInFlight.size() == 1u &&
          restoredSlots == std::vector<uint32_t>({1u, 2u, 3u}),
      "feedback priority queue restores a cancelled in-flight request");

  requestQueue.update(
      queueCache, 0.0, 10.0, queueSettings, queueRequests);
  queueSettings.priorityViewDirectionX = -1.0;
  requestQueue.update(queueCache, 0.0, 10.0, queueSettings);
  const auto reprioritized = requestQueue.take(
      queueCache, 0.0, 10.0, queueSettings, 1u);
  check(
      reprioritized.size() == 1u && reprioritized.front().slot == 2u,
      "feedback priority queue supersedes old priorities after camera motion");

  svr::WorldVoxelChunkCache cameraOwnerCache{};
  cameraOwnerCache.settings = queueSettings;
  const auto addCameraOwner =
      [&](const svr::WorldVoxelChunkAddress &address,
          const uint32_t slot)
  {
    svr::WorldVoxelChunkEntry entry{};
    entry.address = address;
    entry.slot = slot;
    entry.generation = 9u;
    entry.state = svr::WorldVoxelChunkState::Resident;
    cameraOwnerCache.entries.push_back(std::move(entry));
  };
  // These pages meet at x=0. A synthetic camera-owner fallback must not hide
  // a much larger measured error from the current visible cut.
  addCameraOwner({0, 0, 2u}, 4u);
  addCameraOwner({-1, 0, 1u}, 5u);
  svr::WorldVoxelChunkRequestQueue cameraOwnerQueue;
  cameraOwnerQueue.update(
      cameraOwnerCache, 0.0, 10.0, queueSettings,
      {{4u, 9u, 2u, 1u, 0xffffffffu},
       {5u, 9u, 1u, 0u, 1u}});
  const auto cameraOwnerRequest = cameraOwnerQueue.take(
      cameraOwnerCache, 0.0, 10.0, queueSettings, 1u);
  check(
      cameraOwnerRequest.size() == 1u &&
          cameraOwnerRequest.front().slot == 4u,
      "measured visible error preempts a lower-error camera-owner fallback");

  svr::WorldVoxelChunkRequestQueue staleRequestQueue;
  staleRequestQueue.update(
      queueCache, 0.0, 10.0, queueSettings, {queueRequests.front()});
  queueCache.entries.front().generation = 8u;
  const auto staleRequests = staleRequestQueue.take(
      queueCache, 0.0, 10.0, queueSettings, 0u);
  check(
      staleRequests.empty() && staleRequestQueue.empty(),
      "feedback priority queue ignores requests for recycled slots");

  svr::WorldVoxelChunkRequestQueue partiallyStaleQueue;
  partiallyStaleQueue.update(
      queueCache, 0.0, 10.0, queueSettings,
      {queueRequests[1], queueRequests[2]});
  queueCache.entries[1].generation = 8u;
  const auto survivingRequests = partiallyStaleQueue.take(
      queueCache, 0.0, 10.0, queueSettings, 0u);
  check(
      survivingRequests.size() == 1u && survivingRequests.front().slot == 3u,
      "feedback priority queue drops only recycled owners and preserves "
      "other visible refinement requests");

  const auto rootOwner = std::max_element(
      feedbackCache.entries.begin(), feedbackCache.entries.end(),
      [](const svr::WorldVoxelChunkEntry &left,
         const svr::WorldVoxelChunkEntry &right)
      {
        return left.address.spatialLOD < right.address.spatialLOD;
      });
  check(
      rootOwner != feedbackCache.entries.end(),
      "feedback fixture has a directory-root owner");
  if (rootOwner != feedbackCache.entries.end())
  {
    const double feedbackBaseEdge = feedbackSpatial.chunkEdgeMetres > 0.0
        ? feedbackSpatial.chunkEdgeMetres
        : feedbackSpatial.finestVoxelSizeMetres *
            static_cast<double>(feedbackSpatial.finestHorizontalVoxelCount);
    const uint32_t feedbackMaximumRootLOD = std::min<uint32_t>(
        feedbackSpatial.maximumSpatialLOD,
        svr::kPlanetVoxelMaximumClipmapLevels - 1u);
    uint32_t feedbackDirectoryRootLOD = 0u;
    while (feedbackDirectoryRootLOD < feedbackMaximumRootLOD &&
           std::ldexp(feedbackBaseEdge,
                      static_cast<int>(feedbackDirectoryRootLOD)) <
               feedbackSpatial.renderDistanceMetres)
      ++feedbackDirectoryRootLOD;
    const uint32_t rootLOD = std::min(
        rootOwner->address.spatialLOD + rootOwner->residentLOD, 31u);
    const std::vector<svr::WorldVoxelChunkRequest> rootCoarsen{
      {rootOwner->slot, rootOwner->generation, rootLOD, rootLOD + 1u, 1u}};
    const auto accepted =
        svr::coalesceWorldVoxelChunkRequests(feedbackCache, rootCoarsen);
    (void)feedbackDirectoryRootLOD;
    check(
        accepted.empty(),
        "feedback coalescing rejects spatial parent fallback requests");
    const std::vector<svr::WorldVoxelChunkRequest> tooDeepRefine{
      {rootOwner->slot,
       rootOwner->generation,
       rootLOD,
       rootLOD > 1u ? rootLOD - 2u : 0u,
       1u}};
    const auto deepAccepted =
        svr::coalesceWorldVoxelChunkRequests(feedbackCache, tooDeepRefine);
    check(
        rootLOD <= 1u ||
            (deepAccepted.size() == 1u &&
             deepAccepted.front().desiredLOD == rootLOD - 1u),
        "feedback coalescing clamps deep refinements to one spatial level");
  }

  const auto splitOwner = std::find_if(
      feedbackCache.entries.begin(), feedbackCache.entries.end(),
      [](const svr::WorldVoxelChunkEntry &entry)
      {
        return entry.address.spatialLOD > 0u;
      });
  check(
      splitOwner != feedbackCache.entries.end(),
      "feedback fixture has a splittable resident spatial page");
  if (splitOwner != feedbackCache.entries.end())
  {
    const uint32_t currentLOD = std::min(
        splitOwner->address.spatialLOD + splitOwner->residentLOD, 31u);
    const svr::WorldVoxelChunkAddress splitAddress = splitOwner->address;
    std::vector<svr::WorldVoxelChunkRequest> splitRequests;
    splitRequests.push_back({
      splitOwner->slot,
      splitOwner->generation,
      currentLOD,
      currentLOD - 1u,
      1000u});

    svr::PlanetVoxelChunkSet splitChunks{};
    svr::WorldVoxelChunkCacheUpdate splitUpdate{};
    error.clear();
    const bool splitBuilt = svr::updateWorldVoxelChunkCache(
        bundle, 0.0, 0.0, feedbackSpatial, feedbackCache, splitChunks,
        &splitUpdate, &error, {}, nullptr, &splitRequests);
    check(
        splitBuilt,
        ("feedback-driven split update succeeds: " + error).c_str());
    const uint32_t childLOD = splitAddress.spatialLOD - 1u;
    uint32_t installedChildCount = 0u;
    for (int32_t childZ = 0; childZ != 2; ++childZ)
      for (int32_t childX = 0; childX != 2; ++childX)
      {
        const svr::WorldVoxelChunkAddress child{
          splitAddress.x * 2 + childX,
          splitAddress.z * 2 + childZ,
          childLOD};
        installedChildCount += std::any_of(
            feedbackCache.entries.begin(), feedbackCache.entries.end(),
            [&](const svr::WorldVoxelChunkEntry &entry)
            {
              return entry.address == child;
            }) ? 1u : 0u;
      }
    check(
        splitBuilt && splitUpdate.residentCutChanged &&
            splitUpdate.builtChunkCount >= installedChildCount &&
            splitUpdate.refinedChunkCount >= installedChildCount &&
            splitUpdate.physicalUploadPageCount > 0u &&
            installedChildCount > 0u,
        "feedback split reports finer generated pages as spatial refinement");
    check(
        splitBuilt && packedVisibleRootPrefixValid(splitChunks),
        "feedback split keeps visible roots packed for raster traversal");
    const auto installedChild = std::find_if(
        feedbackCache.entries.begin(), feedbackCache.entries.end(),
        [&](const svr::WorldVoxelChunkEntry &entry)
        {
          return entry.address.spatialLOD == childLOD;
        });
    check(
        installedChild != feedbackCache.entries.end(),
        "feedback split leaves a child page resident for coalescing checks");
    if (installedChild != feedbackCache.entries.end())
    {
      const uint32_t childCurrentLOD = std::min(
          installedChild->address.spatialLOD + installedChild->residentLOD,
          31u);
      const std::vector<svr::WorldVoxelChunkRequest> childCoarsen{
        {installedChild->slot,
         installedChild->generation,
         childCurrentLOD,
         childCurrentLOD + 1u,
         1u}};
      const auto childCoarsenAccepted =
          svr::coalesceWorldVoxelChunkRequests(feedbackCache, childCoarsen);
      check(
          childCoarsenAccepted.empty(),
          "feedback coalescing rejects a partial spatial sibling merge");
      auto coarsenCache = feedbackCache;
      svr::PlanetVoxelChunkSet coarsenChunks{};
      svr::WorldVoxelChunkCacheUpdate coarsenUpdate{};
      error.clear();
      const bool coarsenBuilt = svr::updateWorldVoxelChunkCache(
          bundle, 0.0, 0.0, feedbackSpatial, coarsenCache, coarsenChunks,
          &coarsenUpdate, &error, {}, nullptr, &childCoarsen);
      check(
          coarsenBuilt,
          ("feedback-driven parent fallback update succeeds: " + error).c_str());
      check(
          coarsenBuilt && !coarsenUpdate.residentCutChanged &&
              coarsenCache.entries.size() == feedbackCache.entries.size() &&
              packedVisibleRootPrefixValid(coarsenChunks),
          "feedback partial parent fallback keeps the resident cut stable");
      const auto pageHalf = [](const int32_t value)
      {
        return value >= 0 ? value / 2 : -((-value + 1) / 2);
      };
      const auto pageParent = [&](const svr::WorldVoxelChunkAddress &address)
      {
        return svr::WorldVoxelChunkAddress{
          pageHalf(address.x), pageHalf(address.z),
          address.spatialLOD + 1u};
      };
      std::vector<svr::WorldVoxelChunkRequest> siblingCoarsen;
      for (const auto &entry : feedbackCache.entries)
      {
        if (entry.address.spatialLOD != childLOD ||
            !(pageParent(entry.address) == splitAddress))
          continue;
        const uint32_t siblingLOD =
            std::min(entry.address.spatialLOD + entry.residentLOD, 31u);
        siblingCoarsen.push_back({
          entry.slot,
          entry.generation,
          siblingLOD,
          siblingLOD + 1u,
          1u});
      }
      const auto siblingCoarsenAccepted =
          svr::coalesceWorldVoxelChunkRequests(feedbackCache, siblingCoarsen);
      check(
          siblingCoarsen.size() == 4u &&
              siblingCoarsenAccepted.size() == siblingCoarsen.size(),
          "feedback coalescing accepts a complete spatial sibling merge");
      svr::WorldVoxelChunkRequestQueue coarseningQueue;
      coarseningQueue.update(
          feedbackCache,
          bundle.extentMetres * 0.5,
          bundle.extentMetres * 0.5,
          feedbackSpatial,
          siblingCoarsen);
      const auto queuedSiblingCoarsen = coarseningQueue.take(
          feedbackCache,
          bundle.extentMetres * 0.5,
          bundle.extentMetres * 0.5,
          feedbackSpatial,
          1u);
      check(
          queuedSiblingCoarsen.size() == siblingCoarsen.size(),
          "feedback priority queue keeps a sibling merge atomic at a batch boundary");
      auto mergeCache = feedbackCache;
      svr::PlanetVoxelChunkSet mergeChunks{};
      svr::WorldVoxelChunkCacheUpdate mergeUpdate{};
      error.clear();
      const bool mergeBuilt = svr::updateWorldVoxelChunkCache(
          bundle, 0.0, 0.0, feedbackSpatial, mergeCache, mergeChunks,
          &mergeUpdate, &error, {}, nullptr, &siblingCoarsen);
      check(
          mergeBuilt,
          ("feedback-driven complete parent fallback succeeds: " +
           error).c_str());
      check(
          mergeBuilt && mergeUpdate.residentCutChanged &&
              mergeUpdate.coarsenedChunkCount > 0u &&
              mergeCache.entries.size() + 3u == feedbackCache.entries.size() &&
              std::any_of(
                  mergeCache.entries.begin(), mergeCache.entries.end(),
                  [&](const svr::WorldVoxelChunkEntry &entry)
                  {
                    return entry.address == splitAddress;
                  }) &&
              std::none_of(
                  mergeCache.entries.begin(), mergeCache.entries.end(),
                  [&](const svr::WorldVoxelChunkEntry &entry)
                  {
                    return entry.address.spatialLOD == childLOD &&
                        pageParent(entry.address) == splitAddress;
                  }) &&
              packedVisibleRootPrefixValid(mergeChunks),
          "feedback complete parent fallback restores the parent owner");
    }
  }

  std::cout << "10 cm camera convergence\n";
  svr::WorldVoxelChunkStreamingSettings tenCentimetreStreaming =
      keySettings();
  tenCentimetreStreaming.chunkEdgeMetres = 3.2;
  tenCentimetreStreaming.finestVoxelSizeMetres = 0.10;
  tenCentimetreStreaming.finestHorizontalVoxelCount = 32u;
  tenCentimetreStreaming.renderDistanceMetres = 25.6;
  tenCentimetreStreaming.projectionScalePixels = 320.0;
  tenCentimetreStreaming.maximumSpatialLOD = 3u;
  tenCentimetreStreaming.maximumResidentChunks = 512u;
  tenCentimetreStreaming.maximumGeneratedChunksPerUpdate = 512u;
  tenCentimetreStreaming.maximumRefinedChunksPerUpdate = 512u;
  tenCentimetreStreaming.feedbackDrivenPageStreaming = true;
  tenCentimetreStreaming.feedbackDrivenFallbackConvergence = true;
  tenCentimetreStreaming.splitErrorPixels = 4.0f;
  tenCentimetreStreaming.mergeErrorPixels = 1.5f;
  tenCentimetreStreaming.distanceErrorGrowthPerKilometre = 0.10f;
  tenCentimetreStreaming.voxel.horizontalVoxelCount = 32u;
  tenCentimetreStreaming.voxel.patchEdgeMetres = 3.2;
  tenCentimetreStreaming.voxel.workerCount = 1u;

  // The GPU runtime keeps hierarchy storage resident and intentionally leaves
  // the CPU payload vectors empty. Renderer-facing hierarchy metadata must
  // still carry the physical finest scale: material addressing and stable
  // per-voxel colour both use this scalar.
  auto gpuMetadataSettings = tenCentimetreStreaming;
  gpuMetadataSettings.gpuOwnedPayloads = true;
  gpuMetadataSettings.feedbackDrivenPageStreaming = false;
  gpuMetadataSettings.feedbackDrivenFallbackConvergence = false;
  svr::WorldVoxelChunkCache gpuMetadataCache{};
  svr::PlanetVoxelChunkSet gpuMetadataChunks{};
  svr::WorldVoxelChunkCacheUpdate gpuMetadataUpdate{};
  error.clear();
  const bool gpuMetadataBuilt = svr::updateWorldVoxelChunkCache(
      bundle, 0.0, 0.0, gpuMetadataSettings, gpuMetadataCache,
      gpuMetadataChunks, &gpuMetadataUpdate, &error);
  const bool gpuMetadataPayloadFree = std::all_of(
      gpuMetadataChunks.terrainPageUploads.begin(),
      gpuMetadataChunks.terrainPageUploads.end(),
      [](const svr::PlanetVoxelTerrainPageUpload &upload)
      {
        return upload.payloadBytes == 0u;
      });
  const uint64_t maximumGPUInstallMetadataBytes =
      gpuMetadataUpdate.physicalUploadPageCount *
      static_cast<uint64_t>(sizeof(svr::TerrainPagePatch) * 4u);
  check(
      gpuMetadataBuilt && gpuMetadataChunks.gpuOwnedGeneratedPayloads &&
          gpuMetadataChunks.bricks.empty() &&
          gpuMetadataChunks.terrainPageUploads.size() ==
              gpuMetadataUpdate.physicalUploadPageCount &&
          gpuMetadataPayloadFree &&
          gpuMetadataUpdate.physicalUploadBytes <=
              maximumGPUInstallMetadataBytes &&
          gpuMetadataUpdate.cpuCacheBytes == 0u &&
          gpuMetadataUpdate.mapSamplingMilliseconds == 0.0 &&
          gpuMetadataUpdate.shellExtractionMilliseconds == 0.0 &&
          gpuMetadataUpdate.hierarchyBuildMilliseconds == 0.0 &&
          gpuMetadataUpdate.emittedShellVoxelCount == 0u &&
          std::abs(gpuMetadataChunks.bricks.finestVoxelSize - 0.10f) <
              1.0e-6f,
      ("GPU-owned terrain preserves the 10 cm renderer material grid without "
       "CPU payload generation, CPU payload uploads, or CPU page caches: uploads=" +
       std::to_string(gpuMetadataChunks.terrainPageUploads.size()) +
       "/" + std::to_string(gpuMetadataUpdate.physicalUploadPageCount) +
       " uploadBytes=" +
       std::to_string(gpuMetadataUpdate.physicalUploadBytes) +
       " maxInstallMetadataBytes=" +
       std::to_string(maximumGPUInstallMetadataBytes) +
       " cpuCacheBytes=" + std::to_string(gpuMetadataUpdate.cpuCacheBytes) +
       " sampleMs=" +
       std::to_string(gpuMetadataUpdate.mapSamplingMilliseconds) +
       " shellMs=" +
       std::to_string(gpuMetadataUpdate.shellExtractionMilliseconds) +
       " hierarchyMs=" +
       std::to_string(gpuMetadataUpdate.hierarchyBuildMilliseconds) +
       " emitted=" +
       std::to_string(gpuMetadataUpdate.emittedShellVoxelCount) +
       " error=" + error)
          .c_str());

  // A RenderGraph rebuild creates empty physical arenas. Reusing the live CPU
  // cache would classify these pages as resident and emit no uploads, even
  // though the new graph contains none of their payloads. A fresh cache must
  // reconstruct every owner and produce one build upload for every chunk.
  svr::PlanetVoxelChunkSet convergedGraphChunks = gpuMetadataChunks;
  svr::WorldVoxelChunkCacheUpdate convergedGraphUpdate = gpuMetadataUpdate;
  bool convergedGraphBuilt = gpuMetadataBuilt;
  for (uint32_t iteration = 0u;
       convergedGraphBuilt && gpuMetadataCache.pendingChunkCount != 0u &&
       iteration < 256u;
       ++iteration)
    convergedGraphBuilt = svr::updateWorldVoxelChunkCache(
        bundle, 0.0, 0.0, gpuMetadataSettings, gpuMetadataCache,
        convergedGraphChunks, &convergedGraphUpdate, &error);
  svr::PlanetVoxelChunkSet staleGraphChunks{};
  svr::WorldVoxelChunkCacheUpdate staleGraphUpdate{};
  error.clear();
  const bool staleGraphBuilt = svr::updateWorldVoxelChunkCache(
      bundle, 0.0, 0.0, gpuMetadataSettings, gpuMetadataCache,
      staleGraphChunks, &staleGraphUpdate, &error);
  svr::WorldVoxelChunkCache rebuiltGraphCache{};
  svr::PlanetVoxelChunkSet rebuiltGraphChunks{};
  svr::WorldVoxelChunkCacheUpdate rebuiltGraphUpdate{};
  error.clear();
  const bool rebuiltGraphBuilt = svr::updateWorldVoxelChunkCache(
      bundle, 0.0, 0.0, gpuMetadataSettings, rebuiltGraphCache,
      rebuiltGraphChunks, &rebuiltGraphUpdate, &error);
  check(
      convergedGraphBuilt && gpuMetadataCache.pendingChunkCount == 0u &&
          staleGraphBuilt && staleGraphChunks.terrainPageUploads.empty() &&
          rebuiltGraphBuilt && !rebuiltGraphChunks.chunks.empty() &&
          rebuiltGraphChunks.terrainPageUploads.size() ==
              rebuiltGraphChunks.chunks.size() &&
          rebuiltGraphUpdate.builtChunkCount ==
              rebuiltGraphChunks.chunks.size(),
      ("a fresh terrain CPU cache rebuilds and uploads the complete GPU-owned "
       "cut after graph recompilation: pending=" +
       std::to_string(gpuMetadataCache.pendingChunkCount) +
       " staleUploads=" +
       std::to_string(staleGraphChunks.terrainPageUploads.size()) +
       " rebuiltChunks=" + std::to_string(rebuiltGraphChunks.chunks.size()) +
       " rebuiltUploads=" +
       std::to_string(rebuiltGraphChunks.terrainPageUploads.size()) +
       " rebuilt=" + std::to_string(rebuiltGraphUpdate.builtChunkCount) +
       " error=" + error)
          .c_str());

  svr::WorldVoxelChunkCache tenCentimetreCache{};
  svr::PlanetVoxelChunkSet tenCentimetreChunks{};
  svr::WorldVoxelChunkCacheUpdate tenCentimetreUpdate{};
  error.clear();
  bool tenCentimetreConverged = svr::updateWorldVoxelChunkCache(
      bundle, 0.0, 0.0, tenCentimetreStreaming, tenCentimetreCache,
      tenCentimetreChunks, &tenCentimetreUpdate, &error);
  bool feedbackSplitRetainedFallback = false;
  const double mapCentre = bundle.extentMetres * 0.5;
  // Model the real feedback loop: each published owner emits a new measured
  // request if it is still too coarse. One high-error observation must split
  // only that owner by one level, not promote the entire CPU target cut.
  for (uint32_t feedbackIndex = 0u;
       tenCentimetreConverged && feedbackIndex < 8u; ++feedbackIndex)
  {
    const auto cameraOwner = std::find_if(
        tenCentimetreCache.entries.begin(), tenCentimetreCache.entries.end(),
        [&](const auto &entry)
        {
          const double edge = std::ldexp(
              tenCentimetreStreaming.chunkEdgeMetres,
              static_cast<int>(entry.address.spatialLOD));
          const double minimumX = static_cast<double>(entry.address.x) * edge;
          const double minimumZ = static_cast<double>(entry.address.z) * edge;
          return mapCentre >= minimumX && mapCentre < minimumX + edge &&
              mapCentre >= minimumZ && mapCentre < minimumZ + edge;
        });
    if (cameraOwner == tenCentimetreCache.entries.end() ||
        cameraOwner->address.spatialLOD == 0u)
      break;
    const uint32_t currentLOD = cameraOwner->address.spatialLOD;
    const std::vector<svr::WorldVoxelChunkRequest> cameraRequest{{
      cameraOwner->slot, cameraOwner->generation, currentLOD,
      currentLOD - 1u, 0xffffff00u}};
    tenCentimetreConverged = svr::updateWorldVoxelChunkCache(
        bundle, 0.0, 0.0, tenCentimetreStreaming, tenCentimetreCache,
        tenCentimetreChunks, &tenCentimetreUpdate, &error, {}, nullptr,
        &cameraRequest);
    if (feedbackIndex == 0u)
      feedbackSplitRetainedFallback =
          tenCentimetreCache.pendingChunkCount != 0u;
  }
  check(
      feedbackSplitRetainedFallback,
      "a feedback split retains the screen-error fallback target when the "
      "visible request queue becomes empty");
  for (uint32_t updateIndex = 0u;
       tenCentimetreConverged &&
       tenCentimetreCache.pendingChunkCount != 0u && updateIndex < 8u;
       ++updateIndex)
    tenCentimetreConverged = svr::updateWorldVoxelChunkCache(
        bundle, 0.0, 0.0, tenCentimetreStreaming, tenCentimetreCache,
        tenCentimetreChunks, &tenCentimetreUpdate, &error);

  double cameraVoxelSize = std::numeric_limits<double>::max();
  for (const auto &entry : tenCentimetreCache.entries)
  {
    const double edge = std::ldexp(
        tenCentimetreStreaming.chunkEdgeMetres,
        static_cast<int>(entry.address.spatialLOD));
    const double minimumX = static_cast<double>(entry.address.x) * edge;
    const double minimumZ = static_cast<double>(entry.address.z) * edge;
    if (mapCentre >= minimumX && mapCentre < minimumX + edge &&
        mapCentre >= minimumZ && mapCentre < minimumZ + edge)
      cameraVoxelSize = std::min(
          cameraVoxelSize,
          std::ldexp(
              tenCentimetreStreaming.finestVoxelSizeMetres,
              static_cast<int>(
                  entry.address.spatialLOD + entry.residentLOD)));
  }
  check(
      tenCentimetreConverged &&
          tenCentimetreCache.pendingChunkCount == 0u &&
          std::abs(cameraVoxelSize - 0.10) < 1.0e-9,
      ("repeated camera feedback converges one owner at a time to a resident "
       "10 cm page: " + error)
          .c_str());

  // Fill a constrained cut, then leave the initial near field. Incomplete
  // replacement groups must not pin transition slots at the old camera.
  auto movingSettings = tenCentimetreStreaming;
  movingSettings.maximumResidentChunks = 64u;
  // Constrain logical page slots, not the independently sized payload arena.
  // This tiny 32-cell fixture has a large coarse/fine node-count ratio.
  movingSettings.virtualArenaScale = 4.0f;
  movingSettings.maximumGeneratedChunksPerUpdate = 4u;
  movingSettings.maximumRefinedChunksPerUpdate = 4u;
  svr::WorldVoxelChunkCache movingCache{};
  bool movingValid = true;
  bool destinationFine = false;
  for (uint32_t step = 0u; movingValid && step < 96u; ++step)
  {
    const double cameraX = step < 32u ? 0.0 : 64.0;
    movingSettings.readerFrame = step * 8u;
    svr::PlanetVoxelChunkSet movingChunks{};
    svr::WorldVoxelChunkCacheUpdate movingUpdate{};
    error.clear();
    movingValid = svr::updateWorldVoxelChunkCache(
        bundle, cameraX, 0.0, movingSettings, movingCache,
        movingChunks, &movingUpdate, &error);
    movingValid = movingValid && packedVisibleRootPrefixValid(movingChunks);
    if (step >= 32u)
    {
      destinationFine = false;
      for (const auto &entry : movingCache.entries)
      {
        const double edge = std::ldexp(
            movingSettings.chunkEdgeMetres, entry.address.spatialLOD);
        const double x = bundle.extentMetres * 0.5 + cameraX;
        const double z = bundle.extentMetres * 0.5;
        if (x >= entry.address.x * edge && x < (entry.address.x + 1) * edge &&
            z >= entry.address.z * edge && z < (entry.address.z + 1) * edge)
          destinationFine = entry.address.spatialLOD == 0u && entry.residentLOD == 0u;
      }
    }
  }
  check(movingValid && destinationFine,
      ("capacity-limited cut refines new ground to 10 cm after leaving its "
       "initial settled near field: " + error).c_str());

  return failures == 0 ? 0 : 1;
}
