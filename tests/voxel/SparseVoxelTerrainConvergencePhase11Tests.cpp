#include "TerrainGenerationPhase0Utils.hpp"
#include "procedural/world/MapPlacement.hpp"
#include "voxel/svraster/SparseVoxelStreaming.hpp"
#include "voxel/svraster/SparseVoxelTerrainPlacement.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{
namespace world = procedural::world;
namespace svr = voxel::svraster;

int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

void appendU16(std::vector<uint8_t> &bytes, const uint16_t value)
{
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8u));
}

std::vector<uint8_t> u16(const uint16_t value)
{
  std::vector<uint8_t> bytes;
  appendU16(bytes, value);
  return bytes;
}

std::vector<uint8_t> halfPair(const uint16_t first, const uint16_t second)
{
  std::vector<uint8_t> bytes;
  appendU16(bytes, first);
  appendU16(bytes, second);
  return bytes;
}

world::MapExportField constantField(
    const std::string &name,
    const world::MapExportFormat format,
    const world::MapExportFilter filter,
    const std::vector<uint8_t> &texel,
    const uint32_t pageSize,
    const uint16_t gutter,
    const bool available = true,
    const std::string &samplingGroup = {})
{
  world::MapExportField field{};
  field.name = name;
  field.format = format;
  field.filter = filter;
  field.width = pageSize;
  field.height = pageSize;
  field.available = available;
  field.samplingGroup = samplingGroup;
  if (!available)
    return field;
  world::MapExportPage page{};
  page.address = {0u, 0u, 0u};
  page.width = pageSize + gutter * 2u;
  page.height = page.width;
  page.data.reserve(static_cast<size_t>(page.width) * page.height * texel.size());
  for (size_t index = 0u; index < static_cast<size_t>(page.width) * page.height; ++index)
    page.data.insert(page.data.end(), texel.begin(), texel.end());
  field.pages.push_back(std::move(page));
  return field;
}

world::MapExportBundle placementFixture()
{
  constexpr uint32_t pageSize = 16u;
  constexpr uint16_t gutter = 1u;
  world::MapExportBundle bundle{};
  bundle.resolution = pageSize;
  bundle.pageSizePixels = pageSize;
  bundle.gutterPixels = gutter;
  bundle.maximumPageLevel = 0u;
  bundle.extentMetres = 6400.0;
  bundle.seed = 0x4f1bbcdc1234abcdull;
  bundle.bundleSignature = 0x1122334455667788ull;
  bundle.fields = {
    // Deliberately zero: Phase 11 should use the Phase 8 vegetation/rock maps
    // rather than forcing terrain to synthesize instance data.
    constantField("placement_potential", world::MapExportFormat::RGBA8Unorm, world::MapExportFilter::Linear, {0u, 0u, 0u, 0u}, pageSize, gutter),
    constantField("water_type", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest, {0u}, pageSize, gutter),
    constantField("water_level", world::MapExportFormat::R16Float, world::MapExportFilter::Linear, u16(0x0000u), pageSize, gutter),
    constantField("climate", world::MapExportFormat::RG16Float, world::MapExportFilter::Linear, halfPair(0x0000u, 0x3800u), pageSize, gutter),
    constantField("refined_elevation", world::MapExportFormat::R16Unorm, world::MapExportFilter::Linear, u16(49151u), pageSize, gutter),
    constantField("base_elevation", world::MapExportFormat::R16Unorm, world::MapExportFilter::Linear, u16(6554u), pageSize, gutter),
    constantField("river", world::MapExportFormat::RG16Float, world::MapExportFilter::Linear, halfPair(0x3c00u, 0x0000u), pageSize, gutter),
    constantField("substrate_id", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest, {3u}, pageSize, gutter),
    constantField("soil_depth", world::MapExportFormat::R16Float, world::MapExportFilter::Linear, u16(0x3c00u), pageSize, gutter),
    constantField("biome_mix_ids", world::MapExportFormat::RGBA8Uint, world::MapExportFilter::SparseMixture, {static_cast<uint8_t>(world::Biome::Grassland), 0u, 0u, 0u}, pageSize, gutter, true, "biome_mixture"),
    constantField("biome_mix_weights", world::MapExportFormat::RGBA8Unorm, world::MapExportFilter::SparseMixture, {255u, 0u, 0u, 0u}, pageSize, gutter, true, "biome_mixture"),
    constantField("object_exclusion", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest, {0u}, pageSize, gutter),
    constantField("vegetation_potential", world::MapExportFormat::RGBA8Unorm, world::MapExportFilter::Linear, {255u, 0u, 255u, 0u}, pageSize, gutter),
    constantField("rock_potential", world::MapExportFormat::RGBA8Unorm, world::MapExportFilter::Linear, {0u, 255u, 0u, 0u}, pageSize, gutter),
    constantField("exposed_bedrock", world::MapExportFormat::R16Float, world::MapExportFilter::Linear, u16(0x3c00u), pageSize, gutter),
    constantField("canopy_density", world::MapExportFormat::R16Float, world::MapExportFilter::Linear, u16(0x3c00u), pageSize, gutter),
    constantField("ground_density", world::MapExportFormat::R16Float, world::MapExportFilter::Linear, u16(0x3c00u), pageSize, gutter),
    constantField("riparian_mask", world::MapExportFormat::R16Float, world::MapExportFilter::Linear, u16(0x0000u), pageSize, gutter),
    constantField("vegetation_type", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest, {}, pageSize, gutter, false),
    constantField("rock_type", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest, {}, pageSize, gutter, false),
  };
  bundle.field("refined_elevation")->scale[0] = 1000.0;
  bundle.field("base_elevation")->scale[0] = 1000.0;
  bundle.field("river")->scale[0] = bundle.extentMetres;
  return bundle;
}

world::PlacementAssetRule asset(
    const std::string &key,
    const world::PlacementAssetKind kind,
    const world::PlacementPotentialChannel channel,
    const double spacing)
{
  world::PlacementAssetRule rule{};
  rule.assetKey = key;
  rule.collectionKey = key.find("rock") == std::string::npos ? "vegetation" : "rocks";
  rule.resourceKey = key.find("rock") == std::string::npos ? "voxel/" + key : "mesh/" + key;
  rule.kind = kind;
  rule.potential = channel;
  rule.selectionWeight = 1.0;
  rule.densityMultiplier = 1.0;
  rule.footprintRadiusMetres = spacing * 0.5;
  rule.minimumSpacingMetres = spacing;
  rule.scaleRange = {1.0, 1.0};
  return rule;
}

svr::SparseVoxelTerrainPlacementCandidateBufferKey keyFor(
    const world::MapExportBundle &bundle,
    const world::PlacementPalette &palette)
{
  svr::SparseVoxelTerrainPlacementCandidateBufferKey key{};
  key.worldDataPage = {0u, 0u, 0u};
  key.terrainPage = {0x7000u, 42u};
  key.worldSeed = bundle.seed;
  key.placementPaletteVersion = palette.version;
  key.placementPaletteSignature = svr::sparseVoxelTerrainPlacementPaletteSignature(palette);
  key.terrainOutputRevision = 17u;
  return key;
}

svr::SparseVoxelPageProducer traceIntegrationProducer(const uint64_t id = 0x50484153453131ull)
{
  svr::SparseVoxelPageProducer result{};
  result.producerId = id;
  result.kind = svr::SparseVoxelPageProducerKind::CPUGenerated;
  result.producerRevision = 11u;
  result.producesOnCPU = true;
  result.debugName = "phase11-trace-route";
  return result;
}

svr::SparseVoxelProducedPagePayload traceIntegrationPayload(
    const svr::SparseVoxelPageId page,
    const uint64_t expectedGeneration,
    const uint32_t producerRevision,
    const uint64_t payloadRevision,
    const uint64_t geometryRevision,
    const bool rasterPayload,
    const bool tracePayload,
    const bool materialPayload)
{
  svr::SparseVoxelProducedPagePayload result{};
  result.page = page;
  result.expectedPageGeneration = expectedGeneration;
  result.requiredProducerRevision = producerRevision;
  result.payloadRevision = payloadRevision;
  result.geometryRevision = geometryRevision;
  result.materialRevision = materialPayload ? geometryRevision : 0u;
  result.productionFence = payloadRevision;
  result.requirements.page = page;
  result.requirements.geometryRevision = geometryRevision;
  result.requirements.materialRevision = result.materialRevision;
  result.requirements.traceFormat = tracePayload ? svr::kVoxelTraceFormatCubic64 : svr::kVoxelTraceFormatInvalid;
  result.requirements.hierarchyNodes.count = rasterPayload ? 1u : 0u;
  result.requirements.childIndices.count = rasterPayload ? 1u : 0u;
  result.requirements.brickNodes.count = rasterPayload ? 1u : 0u;
  result.requirements.brickOccupancy.count = rasterPayload ? 1u : 0u;
  result.requirements.materials.count = materialPayload ? 1u : 0u;
  result.requirements.materialCells.count = materialPayload ? 1u : 0u;
  result.requirements.traceNodes.count = tracePayload ? 1u : 0u;
  result.requirements.traceErrorWords.count = tracePayload ? 1u : 0u;
  result.requirements.cullErrorWords.count = tracePayload ? 1u : 0u;
  result.installRewriteCount = 1u;
  result.uninstallRewriteCount = 1u;
  result.payloadComplete = true;
  return result;
}

svr::SparseVoxelPageInstallRewrite traceIntegrationInstallRewrite(
    const svr::SparseVoxelReservedPagePayload &reservation)
{
  svr::SparseVoxelPageInstallRewrite rewrite{};
  rewrite.page = reservation.produced.page;
  rewrite.allocation = reservation.allocation;
  rewrite.requiredProducerRevision = reservation.produced.requiredProducerRevision;
  rewrite.rewrite.nodeIndex = reservation.allocation.hierarchyNodes.empty() ? 0u : reservation.allocation.hierarchyNodes.first;
  rewrite.rewrite.firstChild = reservation.allocation.childIndices.empty() ? 0u : reservation.allocation.childIndices.first;
  rewrite.rewrite.childCount = reservation.allocation.childIndices.count;
  rewrite.rewrite.flags = 1u;
  return rewrite;
}

bool installTraceIntegrationPayload(
    svr::SparseVoxelSharedPageTable &table,
    const svr::SparseVoxelProducedPagePayload &payload,
    svr::SparseVoxelReservedPagePayload &reservation,
    std::string &error)
{
  if (!table.reserveProducedPagePayload(payload, reservation, &error))
    return false;
  if (!table.markPayloadCopied(reservation, payload.productionFence + 100u, &error))
    return false;
  return table.installCopiedPagePayload(reservation, {traceIntegrationInstallRewrite(reservation)}, &error);
}

svr::SparseVoxelStreamedInstance traceIntegrationInstance(
    const svr::SparseVoxelPageProducer &producer,
    const svr::SparseVoxelPageId root)
{
  svr::SparseVoxelStreamedInstance instance{};
  instance.instanceIndex = 110u;
  instance.producer = producer;
  instance.rootPage = root;
  instance.firstRootNode = 0u;
  instance.rootNodeCount = 1u;
  instance.firstPage = 0u;
  instance.pageCount = 2u;
  for (uint32_t diagonal = 0u; diagonal < 4u; ++diagonal)
    instance.modelToWorld[diagonal * 4u + diagonal] = 1.0f;
  return instance;
}

bool sameCandidates(
    const svr::SparseVoxelTerrainPlacementCandidateBuffer &left,
    const svr::SparseVoxelTerrainPlacementCandidateBuffer &right)
{
  if (!(left.key == right.key) || left.candidates.size() != right.candidates.size())
    return false;
  for (size_t index = 0u; index < left.candidates.size(); ++index)
  {
    const auto &a = left.candidates[index];
    const auto &b = right.candidates[index];
    if (a.stableId != b.stableId || a.assetIndex != b.assetIndex || a.position.x != b.position.x || a.position.y != b.position.y || a.yawRadians != b.yawRadians || a.scale != b.scale ||
        a.density != b.density || !(a.resolvedPage == b.resolvedPage))
      return false;
  }
  return true;
}

bool terrainEquivalent(const svr::PlanetVoxelChunkSet &left, const svr::PlanetVoxelChunkSet &right)
{
  terrain_phase0::SemanticTerrainPage leftPage{};
  terrain_phase0::SemanticTerrainPage rightPage{};
  std::string difference;
  return terrain_phase0::decodeSemanticTerrainPage(left, 0u, leftPage, &difference) && terrain_phase0::decodeSemanticTerrainPage(right, 0u, rightPage, &difference) &&
      terrain_phase0::compareSemanticTerrainPages(leftPage, rightPage, &difference) && terrain_phase0::terrainPayloadBytes(left) == terrain_phase0::terrainPayloadBytes(right);
}

void testCandidateBufferUsesPhase8MapsAndFootprintFeedback()
{
  std::cout << "Phase 11 placement candidate buffer contract\n";
  const world::MapExportBundle bundle = placementFixture();
  world::PlacementPalette palette{};
  palette.version = 3u;
  palette.assets = {
    asset("tree.oak", world::PlacementAssetKind::VoxelModel, world::PlacementPotentialChannel::Trees, 750.0),
    asset("rock.boulder", world::PlacementAssetKind::Mesh, world::PlacementPotentialChannel::ExposedRock, 750.0),
  };
  const auto key = keyFor(bundle, palette);
  svr::SparseVoxelTerrainPlacementBuildSettings settings{};
  settings.candidateCellMetres = 400.0;
  settings.maximumEvaluatedSites = 4096u;
  settings.maximumCandidates = 4096u;

  svr::SparseVoxelTerrainPlacementCandidateBuffer first{};
  svr::SparseVoxelTerrainPlacementCandidateBuffer repeat{};
  std::string error;
  check(svr::buildSparseVoxelTerrainPlacementCandidateBuffer(bundle, palette, key, settings, first, &error), "candidate buffer builds from world-data page and terrain page key: " + error);
  check(svr::buildSparseVoxelTerrainPlacementCandidateBuffer(bundle, palette, key, settings, repeat, &error) && sameCandidates(first, repeat), "candidate buffer is deterministic for seed, terrain revision and palette signature");
  check(!first.candidates.empty(), "Phase 8 vegetation/rock maps can produce candidates even when legacy placement_potential is zero");

  bool hasVoxelAsset = false;
  bool hasVirtualGeometryAsset = false;
  for (const auto &candidate : first.candidates)
  {
    hasVoxelAsset = hasVoxelAsset || palette.assets[candidate.assetIndex].kind == world::PlacementAssetKind::VoxelModel;
    hasVirtualGeometryAsset = hasVirtualGeometryAsset || palette.assets[candidate.assetIndex].kind == world::PlacementAssetKind::Mesh;
  }
  check(hasVoxelAsset && hasVirtualGeometryAsset, "candidate buffer can carry both vegetation voxel assets and rock virtual-geometry assets");

  svr::SparseVoxelTerrainPlacementBuildSettings blocked = settings;
  blocked.footprintExclusions.push_back({0xabcdu, first.candidates.front().position, palette.assets[first.candidates.front().assetIndex].footprintRadiusMetres + 1.0});
  svr::SparseVoxelTerrainPlacementCandidateBuffer blockedBuffer{};
  check(svr::buildSparseVoxelTerrainPlacementCandidateBuffer(bundle, palette, key, blocked, blockedBuffer, &error), "candidate buffer accepts opaque footprint feedback: " + error);
  check(blockedBuffer.footprintRejected > 0u && blockedBuffer.candidates.size() < first.candidates.size(), "footprint feedback rejects overlapping placements without terrain internals");
}

void testPlacementInstancesUseIndependentResidencyContracts()
{
  std::cout << "Phase 11 streamed instance boundary\n";
  const world::MapExportBundle bundle = placementFixture();
  world::PlacementPalette palette{};
  palette.version = 4u;
  palette.assets = {
    asset("tree.oak", world::PlacementAssetKind::VoxelModel, world::PlacementPotentialChannel::Trees, 10000.0),
    asset("rock.boulder", world::PlacementAssetKind::Mesh, world::PlacementPotentialChannel::ExposedRock, 10000.0),
  };
  const auto key = keyFor(bundle, palette);
  svr::SparseVoxelTerrainPlacementCandidateBuffer buffer{};
  buffer.key = key;
  world::PlacementCandidate tree{};
  tree.stableId = 10u;
  tree.assetIndex = 0u;
  tree.position = {12.0, 34.0};
  tree.elevationMetres = 56.0;
  tree.yawRadians = 0.25;
  tree.scale = 2.0;
  world::PlacementCandidate rock = tree;
  rock.stableId = 11u;
  rock.assetIndex = 1u;
  rock.position = {90.0, 12.0};
  rock.scale = 1.5;
  buffer.candidates = {tree, rock};

  svr::SparseVoxelTerrainPlacementAssetResidency treeResidency{};
  treeResidency.assetKey = "tree.oak";
  treeResidency.domain = svr::SparseVoxelTerrainPlacementResidencyDomain::SparseVoxel;
  treeResidency.sparseVoxelProducer = {0x9000u, svr::SparseVoxelPageProducerKind::PrebuiltSparseVoxel, 2u, true, false, "placement-tree"};
  treeResidency.sparseVoxelRootPage = {0x9000u, 0u};
  treeResidency.sparseVoxelDesiredPage = {0x9000u, 3u};
  treeResidency.sparseVoxelFirstRootNode = 7u;
  treeResidency.sparseVoxelRootNodeCount = 1u;
  treeResidency.sparseVoxelFirstPage = 0u;
  treeResidency.sparseVoxelPageCount = 4u;

  svr::SparseVoxelTerrainPlacementAssetResidency rockResidency{};
  rockResidency.assetKey = "rock.boulder";
  rockResidency.domain = svr::SparseVoxelTerrainPlacementResidencyDomain::VirtualGeometry;
  rockResidency.virtualGeometryObjectIndex = 5u;
  rockResidency.virtualGeometryRootPage = 2u;
  rockResidency.virtualGeometryDesiredPage = 9u;
  rockResidency.virtualGeometryPageTableOffset = 64u;

  std::vector<svr::SparseVoxelTerrainPlacementInstanceRequest> requests;
  std::string error;
  check(svr::buildSparseVoxelTerrainPlacementInstanceRequests(buffer, palette, {treeResidency, rockResidency}, 100u, requests, &error), "placement candidates emit streamed instance requests: " + error);
  check(requests.size() == 2u && requests[0].domain == svr::SparseVoxelTerrainPlacementResidencyDomain::SparseVoxel && requests[0].sparseVoxelInstance.valid() &&
            requests[0].sparseVoxelDesiredPage == treeResidency.sparseVoxelDesiredPage,
        "voxel-model placement becomes an ordinary sparse-voxel streamed instance");
  check(requests.size() == 2u && requests[1].domain == svr::SparseVoxelTerrainPlacementResidencyDomain::VirtualGeometry && requests[1].virtualGeometryObjectIndex == rockResidency.virtualGeometryObjectIndex &&
            requests[1].virtualGeometryDesiredPage == rockResidency.virtualGeometryDesiredPage,
        "mesh placement stays in the virtual-geometry page-table domain");
  check(requests.size() == 2u && !requests[0].ownsAssetPayload && !requests[1].ownsAssetPayload && requests[0].candidateKey == key && requests[1].candidateKey == key, "terrain placement requests never own mesh, voxel or Tree64 payloads");
  check(requests.size() == 2u && requests[0].modelToWorld[12] == 12.0f && requests[0].modelToWorld[13] == 56.0f && requests[0].modelToWorld[14] == 34.0f, "placement transform is emitted as instance data, not terrain geometry");
}

void testDisabledPlacementLeavesTerrainIdentical()
{
  std::cout << "Phase 11 disabled placement gate\n";
  const auto bundle = terrain_phase0::makeFixture(terrain_phase0::FixtureKind::Flat);
  const auto terrainSettings = terrain_phase0::fixtureSettings(terrain_phase0::FixtureKind::Flat, 0u);
  svr::PlanetVoxelChunkSet before{};
  svr::PlanetVoxelChunkSet after{};
  svr::WorldMapVoxelGenerationMetrics metrics{};
  std::string error;
  check(svr::buildWorldMapVoxelTerrain(bundle, terrainSettings, before, &error, {}, nullptr, &metrics), "baseline terrain builds: " + error);

  world::PlacementPalette palette{};
  palette.assets = {asset("tree.oak", world::PlacementAssetKind::VoxelModel, world::PlacementPotentialChannel::Trees, 100.0)};
  svr::SparseVoxelTerrainPlacementBuildSettings placementSettings{};
  placementSettings.enabled = false;
  svr::SparseVoxelTerrainPlacementCandidateBuffer disabled{};
  check(svr::buildSparseVoxelTerrainPlacementCandidateBuffer(bundle, palette, {}, placementSettings, disabled, &error) && disabled.candidates.empty(), "disabled placement produces no candidate buffer work");
  check(svr::buildWorldMapVoxelTerrain(bundle, terrainSettings, after, &error, {}, nullptr, &metrics), "terrain rebuild after disabled placement builds: " + error);
  check(terrainEquivalent(before, after), "disabling placement leaves terrain payload identical");
}

void testVirtualTraceRuntimeSwitchesKeepTree64Default()
{
  std::cout << "Phase 11 virtual trace runtime switch defaults\n";

  std::string error;
  const svr::VoxelTraceRuntimeSettingsCPU defaults{};
  const auto defaultRoute = svr::voxelTraceRuntimeRoute(defaults, &error);
  check(defaultRoute.valid && defaultRoute.useLegacyTree64 && !defaultRoute.useDirectVirtualTrace, "runtime tracing defaults to legacy Tree64 backend: " + error);
  check(!defaultRoute.requireDirectTraceView && defaultRoute.traceFormat == svr::kVoxelTraceFormatCubic64, "legacy route does not require direct virtual trace residency");
  check(defaultRoute.screenTracesEnabled && defaultRoute.exactMaterialResolution, "default route preserves screen tracing and exact material resolution switches");

  svr::VoxelTraceRuntimeSettingsCPU direct{};
  direct.backend = svr::VoxelTraceRuntimeBackend::DirectVirtual;
  direct.nodeFormat = svr::kVoxelTraceFormatCubic64;
  direct.traversalVariant = svr::VoxelTraceTraversalVariant::GroupedOctant;
  direct.errorSidecarMode = svr::VoxelTraceErrorSidecarMode::TraceLOD;
  const auto directRoute = svr::voxelTraceRuntimeRoute(direct, &error);
  check(directRoute.valid && directRoute.useDirectVirtualTrace && !directRoute.useLegacyTree64, "runtime tracing can opt into direct virtual backend: " + error);
  check(directRoute.requireDirectTraceView && directRoute.traversalVariant == svr::VoxelTraceTraversalVariant::GroupedOctant, "direct virtual route requires directly traceable installed pages");
  check(directRoute.useCullErrorSidecar && directRoute.useTraceLODErrorSidecar, "trace-lod route enables the error sidecar for direct trace LOD decisions");

  uint32_t parsedFormat = svr::kVoxelTraceFormatInvalid;
  svr::VoxelTraceRuntimeBackend parsedBackend = svr::VoxelTraceRuntimeBackend::Tree64;
  svr::VoxelTraceTraversalVariant parsedVariant = svr::VoxelTraceTraversalVariant::Child64;
  svr::VoxelTraceErrorSidecarMode parsedSidecar = svr::VoxelTraceErrorSidecarMode::Off;
  check(svr::voxelTraceParseFormat("bounds16", parsedFormat) && parsedFormat == svr::kVoxelTraceFormatBounds16, "runtime node-format parser accepts bounds16");
  check(svr::voxelTraceParseRuntimeBackend("direct-virtual", parsedBackend) && parsedBackend == svr::VoxelTraceRuntimeBackend::DirectVirtual, "runtime backend parser accepts direct-virtual");
  check(svr::voxelTraceParseTraversalVariant("grouped-octant", parsedVariant) && parsedVariant == svr::VoxelTraceTraversalVariant::GroupedOctant, "runtime traversal parser accepts grouped-octant");
  check(svr::voxelTraceParseErrorSidecarMode("trace-lod", parsedSidecar) && parsedSidecar == svr::VoxelTraceErrorSidecarMode::TraceLOD, "runtime error-sidecar parser accepts trace-lod");
}

void testVirtualTraceRuntimeRejectsUnsafeCombinations()
{
  std::cout << "Phase 11 virtual trace runtime rejects unsafe combinations\n";

  std::string error;
  svr::VoxelTraceRuntimeSettingsCPU invalidFormat{};
  invalidFormat.backend = svr::VoxelTraceRuntimeBackend::DirectVirtual;
  invalidFormat.nodeFormat = 0xf00du;
  check(!svr::voxelTraceRuntimeSettingsValid(invalidFormat, &error), "invalid node format is rejected");

  svr::VoxelTraceRuntimeSettingsCPU tree64Bounds{};
  tree64Bounds.backend = svr::VoxelTraceRuntimeBackend::Tree64;
  tree64Bounds.nodeFormat = svr::kVoxelTraceFormatBounds8;
  check(!svr::voxelTraceRuntimeSettingsValid(tree64Bounds, &error), "legacy Tree64 cannot silently select Bounds8/Bounds16 node formats");

  svr::VoxelTraceRuntimeSettingsCPU groupedBounds{};
  groupedBounds.backend = svr::VoxelTraceRuntimeBackend::DirectVirtual;
  groupedBounds.nodeFormat = svr::kVoxelTraceFormatBounds8;
  groupedBounds.traversalVariant = svr::VoxelTraceTraversalVariant::GroupedOctant;
  check(!svr::voxelTraceRuntimeSettingsValid(groupedBounds, &error), "grouped-octant traversal is restricted to Cubic64 nodes");

  svr::VoxelTraceRuntimeSettingsCPU tree64TraceLOD{};
  tree64TraceLOD.backend = svr::VoxelTraceRuntimeBackend::Tree64;
  tree64TraceLOD.errorSidecarMode = svr::VoxelTraceErrorSidecarMode::TraceLOD;
  check(!svr::voxelTraceRuntimeSettingsValid(tree64TraceLOD, &error), "trace-lod error sidecar cannot be enabled on the legacy Tree64 backend");

  svr::VoxelTraceRuntimeSettingsCPU directBounds{};
  directBounds.backend = svr::VoxelTraceRuntimeBackend::DirectVirtual;
  directBounds.nodeFormat = svr::kVoxelTraceFormatBounds16;
  directBounds.errorSidecarMode = svr::VoxelTraceErrorSidecarMode::CullOnly;
  check(svr::voxelTraceRuntimeSettingsValid(directBounds, &error), "direct virtual route may select Bounds16 with cull-only sidecar: " + error);
}

void testVirtualTraceRuntimeRouteDrivesStreamingFeedback()
{
  std::cout << "Phase 11 virtual trace runtime route drives streaming feedback\n";

  const auto producer = traceIntegrationProducer(0x50484153453132ull);
  const svr::SparseVoxelPageId root{producer.producerId, 0u};
  const svr::SparseVoxelPageId rasterOnly{producer.producerId, 1u};
  std::string error;
  svr::SparseVoxelSharedPageTable table{};
  check(table.configure({8u, 8u, 8u, 8u, 8u, 8u, 0u, 0u, 8u, 8u, 8u}, &error), "runtime route page table configures: " + error);
  check(table.addPage(root, producer, {}, true, &error), "runtime route root page registers: " + error);
  check(table.addPage(rasterOnly, producer, {root}, false, &error), "runtime route desired page registers: " + error);

  svr::SparseVoxelReservedPagePayload rootReservation{};
  check(
      installTraceIntegrationPayload(
          table,
          traceIntegrationPayload(root, table.entry(root)->generation, producer.producerRevision, 200u, 0x1100u, true, true, true),
          rootReservation,
          error),
      "direct trace root fallback installs: " + error);

  svr::SparseVoxelReservedPagePayload rasterReservation{};
  check(
      installTraceIntegrationPayload(
          table,
          traceIntegrationPayload(rasterOnly, table.entry(rasterOnly)->generation, producer.producerRevision, 201u, 0u, true, false, true),
          rasterReservation,
          error),
      "legacy raster desired page installs: " + error);
  check(table.entry(root) != nullptr && table.entry(root)->traceable(), "root page has a direct trace view available as fallback");
  check(table.entry(rasterOnly) != nullptr && table.entry(rasterOnly)->rasterVisible() && !table.entry(rasterOnly)->traceable(), "desired page is raster-visible but not direct-virtual traceable");

  svr::SparseVoxelTraceFeedbackInput input{};
  input.instance = traceIntegrationInstance(producer, root);
  input.desiredPage = rasterOnly;
  input.receiverPage = root;
  input.receiverGeneration = table.entry(root)->generation;
  input.desiredLOD = 1u;
  input.rayClass = svr::SparseVoxelTraceRayClass::Shadow;
  input.mode = svr::SparseVoxelTraceLODMode::ConservativeVisibility;
  input.priority = 90u;
  input.desiredError = 0.20f;
  input.rayConeWidth = 0.05f;
  input.maximumT = 250.0f;

  svr::SparseVoxelTraceRequestQueue queue{};
  queue.configure(4u);
  svr::SparseVoxelTraceFeedbackResult result{};
  const auto legacyRoute = svr::voxelTraceRuntimeRoute({}, &error);
  input.requireDirectTraceView = legacyRoute.requireDirectTraceView;
  check(svr::processSparseVoxelTraceFeedback(table, input, queue, result, &error), "legacy Tree64 route processes feedback: " + error);
  check(result.desiredResident && !result.desiredTraceable, "legacy route can observe raster-only residency without treating it as selected direct trace data");
  check(result.selectedPage == rasterOnly && !result.emittedRequest && queue.size() == 0u, "legacy Tree64 route keeps raster-selected desired page resident without a direct-trace request");

  svr::VoxelTraceRuntimeSettingsCPU directSettings{};
  directSettings.backend = svr::VoxelTraceRuntimeBackend::DirectVirtual;
  directSettings.errorSidecarMode = svr::VoxelTraceErrorSidecarMode::TraceLOD;
  const auto directRoute = svr::voxelTraceRuntimeRoute(directSettings, &error);
  input.requireDirectTraceView = directRoute.requireDirectTraceView;
  check(svr::processSparseVoxelTraceFeedback(table, input, queue, result, &error), "direct virtual route processes feedback: " + error);
  check(result.desiredResident && !result.desiredTraceable, "direct route distinguishes visible raster payload from trace payload");
  check(result.selectedConservativeFallback && result.selectedPage == root, "direct route falls back to the directly traceable root page");
  check(result.emittedRequest && queue.size() == 1u, "direct route requests the missing desired trace page through the streaming queue");
}

} // namespace

int main()
{
  testCandidateBufferUsesPhase8MapsAndFootprintFeedback();
  testPlacementInstancesUseIndependentResidencyContracts();
  testDisabledPlacementLeavesTerrainIdentical();
  testVirtualTraceRuntimeSwitchesKeepTree64Default();
  testVirtualTraceRuntimeRejectsUnsafeCombinations();
  testVirtualTraceRuntimeRouteDrivesStreamingFeedback();
  if (failures != 0)
  {
    std::cerr << failures << " Phase 11 test(s) failed\n";
    return 1;
  }
  std::cout << "Sparse voxel terrain convergence Phase 11 tests passed\n";
  return 0;
}
