#include "voxel/svraster/SparseVoxelStreaming.hpp"
#include "voxel/svraster/TerrainGPUV2Publication.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
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

svr::TerrainGPUChunkKey chunkKey(const uint32_t generation = 3u)
{
  return {91u, 0, 0, 0, 0u, 7u, 20u, generation};
}

svr::TerrainGPUV2PayloadResultGPU payloadResult(const uint32_t generation, const uint64_t revision, const uint32_t generatorRevision)
{
  svr::TerrainGPUV2PayloadResultGPU value{};
  value.status = static_cast<uint32_t>(svr::TerrainGPUV2PublicationStatus::Approved);
  value.flags = svr::TerrainGPUV2PayloadOccupancyOnly | svr::TerrainGPUV2PayloadNoMicroGeometry;
  value.rasterNodeCount = 4u;
  value.rasterMaterialCount = 31u;
  value.rasterChildCount = 1u;
  value.rasterRootCount = 1u;
  value.rasterOccupiedVoxelCount = 31u;
  value.terrainGeneration = generation;
  value.rasterGeneratorRevision = generatorRevision;
  value.treeNodeCount = 9u;
  value.treeVisitedNodeCount = 9u;
  value.treeOccupiedVoxelCount = 31u;
  value.treeGeneratorRevision = generatorRevision;
  value.geometryRevisionLow = static_cast<uint32_t>(revision);
  value.geometryRevisionHigh = static_cast<uint32_t>(revision >> 32u);
  return value;
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

svr::SparseVoxelProducedPagePayload producedPayload(
    const svr::SparseVoxelPageId page,
    const uint64_t generation,
    const uint32_t producerRevision,
    const uint64_t payloadRevision,
    const uint32_t nodes,
    const uint32_t children,
    const uint32_t tree64Nodes)
{
  svr::SparseVoxelProducedPagePayload result{};
  result.page = page;
  result.expectedPageGeneration = generation;
  result.requiredProducerRevision = producerRevision;
  result.payloadRevision = payloadRevision;
  result.requirements.page = page;
  result.requirements.hierarchyNodes.count = nodes;
  result.requirements.childIndices.count = children;
  result.requirements.brickNodes.count = nodes;
  result.requirements.brickOccupancy.count = nodes;
  result.requirements.materials.count = nodes;
  result.requirements.materialCells.count = nodes;
  result.requirements.tree64Nodes.count = tree64Nodes;
  result.requirements.tree64Materials.count = tree64Nodes;
  result.installRewriteCount = 1u;
  result.uninstallRewriteCount = 1u;
  result.payloadComplete = true;
  return result;
}

svr::SparseVoxelPageInstallRewrite installRewrite(const svr::SparseVoxelReservedPagePayload &reservation)
{
  svr::SparseVoxelPageInstallRewrite rewrite{};
  rewrite.page = reservation.produced.page;
  rewrite.allocation = reservation.allocation;
  rewrite.requiredProducerRevision = reservation.produced.requiredProducerRevision;
  rewrite.rewrite.nodeIndex = reservation.allocation.hierarchyNodes.first;
  rewrite.rewrite.firstChild = reservation.allocation.childIndices.empty() ? 0u : reservation.allocation.childIndices.first;
  rewrite.rewrite.childCount = reservation.allocation.childIndices.count;
  rewrite.rewrite.flags = 1u;
  return rewrite;
}

bool reserveCopyInstall(svr::SparseVoxelSharedPageTable &table, const svr::SparseVoxelProducedPagePayload &produced, const uint64_t copyFence, std::string &error)
{
  svr::SparseVoxelReservedPagePayload reservation{};
  return table.reserveProducedPagePayload(produced, reservation, &error) && table.markPayloadCopied(reservation, copyFence, &error) && table.installCopiedPagePayload(reservation, {installRewrite(reservation)}, &error);
}

svr::VoxelTracePageGPU benchmarkCubicPage()
{
  svr::VoxelTracePageGPU page{};
  page.pageId = 120u;
  page.generation = 12u;
  page.geometryRevisionLo = 0x1200u;
  page.format = svr::kVoxelTraceFormatCubic64;
  page.rootNode = 0u;
  page.traceNodeOffset = 0u;
  page.traceNodeCount = 1u;
  page.localOriginX = 0.0f;
  page.localOriginY = 0.0f;
  page.localOriginZ = 0.0f;
  page.rootSide = 4.0f;
  page.errorMin = 0.0f;
  page.errorMax = 1.0f;
  page.flags = svr::kVoxelTracePageValid;
  return page;
}

std::vector<svr::VoxelTraceNode64GPU> benchmarkCubicNodes()
{
  return {
    svr::voxelTraceNode64MakeTerminal(std::numeric_limits<uint64_t>::max(), 4096u),
  };
}

std::vector<svr::VoxelTraceRayCPU> benchmarkRays()
{
  std::vector<svr::VoxelTraceRayCPU> rays;
  rays.reserve(96u);
  for (uint32_t lane = 0u; lane < 32u; ++lane)
  {
    const float y = 0.125f + static_cast<float>(lane % 4u);
    const float z = 0.125f + static_cast<float>((lane / 4u) % 4u);
    svr::VoxelTraceRayCPU xRay{};
    xRay.origin = {-1.0f, y, z};
    xRay.direction = {1.0f, 0.0f, 0.0f};
    xRay.tMax = 16.0f;
    rays.push_back(xRay);

    svr::VoxelTraceRayCPU yRay{};
    yRay.origin = {static_cast<float>(lane % 4u) + 0.125f, -1.0f, z};
    yRay.direction = {0.0f, 1.0f, 0.0f};
    yRay.tMax = 16.0f;
    rays.push_back(yRay);

    svr::VoxelTraceRayCPU miss{};
    miss.origin = {-1.0f, 5.0f + static_cast<float>(lane), z};
    miss.direction = {1.0f, 0.0f, 0.0f};
    miss.tMax = 16.0f;
    rays.push_back(miss);
  }
  return rays;
}

std::vector<svr::VoxelTraceBoundsChildCPU> benchmarkBoundsChildren(const uint32_t count)
{
  std::vector<svr::VoxelTraceBoundsChildCPU> children;
  children.reserve(count);
  for (uint32_t slot = 0u; slot < count; ++slot)
  {
    const float x = 0.125f + static_cast<float>(slot % 4u);
    const float y = 0.125f + static_cast<float>((slot / 4u) % 4u);
    const float z = 0.125f + static_cast<float>((slot / 16u) % 4u);
    children.push_back({{x, y, z}, {x + 0.35f, y + 0.35f, z + 0.35f}, false, true});
  }
  return children;
}

svr::VoxelTracePageGPU benchmarkBoundsPage(const uint32_t format)
{
  svr::VoxelTracePageGPU page{};
  page.pageId = format == svr::kVoxelTraceFormatBounds8 ? 121u : 122u;
  page.generation = format == svr::kVoxelTraceFormatBounds8 ? 13u : 14u;
  page.geometryRevisionLo = 0x1201u + format;
  page.format = format;
  page.rootNode = 0u;
  page.traceNodeOffset = 0u;
  page.traceNodeCount = 1u;
  page.flags = svr::kVoxelTracePageValid;
  return page;
}

struct VirtualTraceBenchmarkCounters
{
  uint64_t rays = 0u;
  uint64_t hits = 0u;
  uint64_t misses = 0u;
  uint64_t invalid = 0u;
  uint64_t queryLimitExceeded = 0u;
  uint64_t coarseFallbacks = 0u;
  uint64_t traceRequests = 0u;
  uint64_t offscreenTraceRequests = 0u;
  uint64_t fallbackTraces = 0u;
  uint64_t unresolvedExact = 0u;
  uint64_t nodeVisits = 0u;
  uint64_t childTests = 0u;
  uint64_t groupTests = 0u;
  uint64_t skippedChildGroups = 0u;
  uint64_t estimatedNodeBytes = 0u;
  uint64_t elapsedNanoseconds = 0u;
};

uint64_t benchmarkNodeBytes(const uint32_t format)
{
  if (format == svr::kVoxelTraceFormatBounds8)
    return sizeof(svr::VoxelTraceBounds8NodeGPU);
  if (format == svr::kVoxelTraceFormatBounds16)
    return sizeof(svr::VoxelTraceBounds16NodeGPU);
  return sizeof(svr::VoxelTraceNode64GPU);
}

void accumulateBenchmarkHit(
    VirtualTraceBenchmarkCounters &counters,
    const svr::VoxelTraceHitCPU &hit,
    const uint32_t format)
{
  ++counters.rays;
  counters.nodeVisits += hit.nodeVisits;
  counters.childTests += hit.childTests;
  counters.groupTests += hit.groupTests;
  counters.skippedChildGroups += hit.skippedChildGroups;
  counters.estimatedNodeBytes += static_cast<uint64_t>(hit.nodeVisits) * benchmarkNodeBytes(format);
  if (hit.coarseFallback)
    ++counters.coarseFallbacks;
  switch (hit.status)
  {
  case svr::VoxelTraceTraversalStatus::Hit:
    ++counters.hits;
    break;
  case svr::VoxelTraceTraversalStatus::Miss:
    ++counters.misses;
    break;
  case svr::VoxelTraceTraversalStatus::Invalid:
    ++counters.invalid;
    break;
  case svr::VoxelTraceTraversalStatus::QueryLimitExceeded:
    ++counters.queryLimitExceeded;
    break;
  }
}

std::string benchmarkJson(
    const std::string &label,
    const svr::VoxelTraceRuntimeSettingsCPU &settings,
    const svr::VoxelTraceRuntimeRouteCPU &route,
    const VirtualTraceBenchmarkCounters &counters)
{
  std::ostringstream out;
  out << "{"
      << "\"label\":\"" << label << "\","
      << "\"backend\":\"" << svr::voxelTraceRuntimeBackendName(settings.backend) << "\","
      << "\"node_format\":\"" << svr::voxelTraceFormatName(route.traceFormat) << "\","
      << "\"traversal\":\"" << svr::voxelTraceTraversalVariantName(route.traversalVariant) << "\","
      << "\"error_sidecar\":\"" << svr::voxelTraceErrorSidecarModeName(settings.errorSidecarMode) << "\","
      << "\"rays\":" << counters.rays << ","
      << "\"hits\":" << counters.hits << ","
      << "\"misses\":" << counters.misses << ","
      << "\"invalid\":" << counters.invalid << ","
      << "\"query_limit_exceeded\":" << counters.queryLimitExceeded << ","
      << "\"trace_requests\":" << counters.traceRequests << ","
      << "\"offscreen_trace_requests\":" << counters.offscreenTraceRequests << ","
      << "\"fallback_traces\":" << counters.fallbackTraces << ","
      << "\"unresolved_exact\":" << counters.unresolvedExact << ","
      << "\"coarse_fallbacks\":" << counters.coarseFallbacks << ","
      << "\"node_visits\":" << counters.nodeVisits << ","
      << "\"child_tests\":" << counters.childTests << ","
      << "\"group_tests\":" << counters.groupTests << ","
      << "\"skipped_child_groups\":" << counters.skippedChildGroups << ","
      << "\"estimated_node_bytes\":" << counters.estimatedNodeBytes << ","
      << "\"elapsed_nanoseconds\":" << counters.elapsedNanoseconds
      << "}";
  return out.str();
}

VirtualTraceBenchmarkCounters runBenchmarkVariant(
    const svr::VoxelTraceRuntimeSettingsCPU &settings,
    std::string &error)
{
  const auto route = svr::voxelTraceRuntimeRoute(settings, &error);
  VirtualTraceBenchmarkCounters counters{};
  if (!route.valid)
    return counters;

  const auto rays = benchmarkRays();
  const auto start = std::chrono::steady_clock::now();
  if (route.traceFormat == svr::kVoxelTraceFormatBounds8)
  {
    svr::VoxelTraceBounds8NodeGPU node{};
    if (!svr::voxelTraceBuildBoundsNode({0.0f, 0.0f, 0.0f}, {4.0f, 4.0f, 4.0f}, benchmarkBoundsChildren(8u), 8u, 0u, 8192u, node, &error))
      return counters;
    const auto page = benchmarkBoundsPage(svr::kVoxelTraceFormatBounds8);
    const std::vector<svr::VoxelTraceBounds8NodeGPU> nodes{node};
    for (const auto &ray : rays)
    {
      svr::VoxelTraceTraversalOptionsCPU options{};
      options.expectedGeneration = page.generation;
      svr::VoxelTraceHitCPU hit{};
      if (!svr::voxelTraceBounds8(page, nodes, ray, options, hit))
        hit.status = svr::VoxelTraceTraversalStatus::Invalid;
      accumulateBenchmarkHit(counters, hit, route.traceFormat);
    }
  }
  else if (route.traceFormat == svr::kVoxelTraceFormatBounds16)
  {
    svr::VoxelTraceBounds16NodeGPU node{};
    if (!svr::voxelTraceBuildBoundsNode({0.0f, 0.0f, 0.0f}, {4.0f, 4.0f, 4.0f}, benchmarkBoundsChildren(16u), 16u, 0u, 12288u, node, &error))
      return counters;
    const auto page = benchmarkBoundsPage(svr::kVoxelTraceFormatBounds16);
    const std::vector<svr::VoxelTraceBounds16NodeGPU> nodes{node};
    for (const auto &ray : rays)
    {
      svr::VoxelTraceTraversalOptionsCPU options{};
      options.expectedGeneration = page.generation;
      svr::VoxelTraceHitCPU hit{};
      if (!svr::voxelTraceBounds16(page, nodes, ray, options, hit))
        hit.status = svr::VoxelTraceTraversalStatus::Invalid;
      accumulateBenchmarkHit(counters, hit, route.traceFormat);
    }
  }
  else
  {
    const auto page = benchmarkCubicPage();
    const auto nodes = benchmarkCubicNodes();
    for (const auto &ray : rays)
    {
      svr::VoxelTraceTraversalOptionsCPU options{};
      options.expectedGeneration = page.generation;
      options.variant = route.traversalVariant;
      svr::VoxelTraceHitCPU hit{};
      if (!svr::voxelTraceCubic64(page, nodes, ray, options, hit))
        hit.status = svr::VoxelTraceTraversalStatus::Invalid;
      accumulateBenchmarkHit(counters, hit, route.traceFormat);
    }
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - start);
  counters.elapsedNanoseconds = std::max<uint64_t>(1u, static_cast<uint64_t>(elapsed.count()));
  if (route.useDirectVirtualTrace)
  {
    counters.traceRequests = 1u;
    counters.offscreenTraceRequests = 1u;
    counters.fallbackTraces = counters.misses > 0u ? 1u : 0u;
  }
  return counters;
}

void testGeneratedTerrainUsesSharedPageResidency()
{
  std::cout << "Phase 12 generated terrain shared residency projection\n";
  constexpr uint64_t revision = 0x123456789abcdef0ull;
  const svr::TerrainGPUChunkKey key = chunkKey();
  svr::TerrainGPUV2StreamedChunkPages pages{};
  std::string error;
  check(svr::terrainGPUV2StreamedChunkPages(key, pages, &error) && pages.valid(), "terrain chunk has shared sparse-voxel producer/page identity: " + error);

  svr::TerrainGPUV2ResidencyManager residency({4096u, 3072u, 3840u, 64u, 32u});
  uint64_t handle = 0u;
  const auto payload = payloadResult(key.expectedGeneration, revision, key.generatorRevision);
  check(residency.reserveHidden(1u, key, box(0, 0, 0, 8, 8, 8), revision, payload, footprint(256u), 1u, 0u, true, false, handle, &error), "generated terrain payload reserves as hidden producer output: " + error);

  auto projected = residency.sharedPageResidency();
  check(projected.size() == 1u && projected[0].page == pages.desiredPage && projected[0].state == svr::SparseVoxelPageState::Decoded && projected[0].legacyHiddenHandle == handle,
        "hidden generated output projects to a decoded shared page, not a visible terrain owner");

  svr::TerrainGPUV2GroupPublication blockedPublication{};
  blockedPublication.expected.group = 1u;
  blockedPublication.coverage = box(0, 0, 0, 8, 8, 8);
  blockedPublication.replacementTerrainGeneration = key.expectedGeneration;
  blockedPublication.replacementPayloadRevision = revision;
  blockedPublication.hiddenHandles = {handle};
  svr::TerrainGPUV2PublicationResultGPU publicationResult{};
  check(!residency.publish(blockedPublication, 2u, publicationResult, &error) && residency.visible(1u).empty() && residency.hidden(handle).has_value(),
        "legacy terrain visible-cut publication is disabled unless explicitly requested");

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure({64u, 64u, 64u, 64u, 64u, 64u, 32u, 32u}, &error), "shared page table configures for generated terrain: " + error);
  check(table.addPage(pages.rootPage, pages.producer, {}, true, &error), "terrain coarse fallback registers as pinned shared page: " + error);
  check(table.addPage(pages.desiredPage, pages.producer, {pages.rootPage}, false, &error), "terrain generated detail registers as ordinary dependent page: " + error);
  check(reserveCopyInstall(table, producedPayload(pages.rootPage, table.entry(pages.rootPage)->generation, pages.producer.producerRevision, revision - 1u, 1u, 1u, 1u), 3u, error), "coarse fallback installs through shared page rewrites: " + error);

  svr::SparseVoxelProducedPagePayload produced{};
  check(svr::terrainGPUV2ProducedPagePayload(pages.desiredPage, table.entry(pages.desiredPage)->generation, pages.producer.producerRevision, payload, produced, &error), "terrain GPU payload converts to shared page payload: " + error);
  check(reserveCopyInstall(table, produced, 4u, error), "generated detail installs through shared page rewrites: " + error);

  svr::SparseVoxelStreamedInstance instance{};
  check(svr::terrainGPUV2StreamedChunkInstance(key, 0u, table.entry(pages.rootPage)->allocation.hierarchyNodes.first, 2u, instance, &error), "terrain chunk becomes an ordinary streamed instance: " + error);
  svr::SparseVoxelInstanceLODSelection selection{};
  check(svr::selectSparseVoxelStreamedInstanceLOD(table, instance, pages.desiredPage, selection, &error), "shared hierarchy selects terrain LOD: " + error);
  check(selection.selectedPage == pages.desiredPage && selection.desiredResident && !selection.selectedFallback, "renderer-visible terrain cut comes from the shared sparse-voxel table");

  blockedPublication.allowLegacyVisibleCutPublication = true;
  check(residency.publish(blockedPublication, 5u, publicationResult, &error) && residency.visible(1u).size() == 1u, "legacy owner publication remains only as an explicit compatibility path: " + error);
  projected = residency.sharedPageResidency();
  check(projected.size() == 1u && projected[0].page == pages.desiredPage && projected[0].state == svr::SparseVoxelPageState::Resident && projected[0].legacyHiddenHandle == 0u,
        "compatibility publication also projects back to the same shared resident page identity");
}

void testVirtualTraceBenchmarkSmokeEmitsPruneData()
{
  std::cout << "Phase 12 virtual trace benchmark smoke\n";

  struct Variant
  {
    std::string label;
    svr::VoxelTraceRuntimeSettingsCPU settings{};
  };

  std::vector<Variant> variants;
  variants.push_back({"tree64-legacy", {}});

  svr::VoxelTraceRuntimeSettingsCPU directCubic{};
  directCubic.backend = svr::VoxelTraceRuntimeBackend::DirectVirtual;
  variants.push_back({"direct-cubic64-child64", directCubic});

  svr::VoxelTraceRuntimeSettingsCPU grouped = directCubic;
  grouped.traversalVariant = svr::VoxelTraceTraversalVariant::GroupedOctant;
  grouped.errorSidecarMode = svr::VoxelTraceErrorSidecarMode::TraceLOD;
  variants.push_back({"direct-cubic64-grouped-octant", grouped});

  svr::VoxelTraceRuntimeSettingsCPU bounds8 = directCubic;
  bounds8.nodeFormat = svr::kVoxelTraceFormatBounds8;
  bounds8.errorSidecarMode = svr::VoxelTraceErrorSidecarMode::CullOnly;
  variants.push_back({"direct-bounds8", bounds8});

  svr::VoxelTraceRuntimeSettingsCPU bounds16 = directCubic;
  bounds16.nodeFormat = svr::kVoxelTraceFormatBounds16;
  bounds16.errorSidecarMode = svr::VoxelTraceErrorSidecarMode::CullOnly;
  variants.push_back({"direct-bounds16", bounds16});

  std::string combinedJson = "[";
  std::string error;
  uint64_t child64ChildTests = 0u;
  uint64_t groupedChildTests = 0u;
  uint64_t directVariantCount = 0u;
  for (size_t index = 0u; index < variants.size(); ++index)
  {
    const auto &variant = variants[index];
    const auto route = svr::voxelTraceRuntimeRoute(variant.settings, &error);
    check(route.valid, "benchmark variant route is valid for " + variant.label + ": " + error);
    const auto counters = runBenchmarkVariant(variant.settings, error);
    check(counters.rays == 96u, "benchmark variant traces the fixed ray set for " + variant.label);
    check(counters.hits > 0u && counters.misses > 0u, "benchmark variant records both hits and misses for " + variant.label);
    check(counters.invalid == 0u && counters.queryLimitExceeded == 0u, "benchmark variant has no invalid or query-limited traces for " + variant.label);
    check(counters.nodeVisits > 0u && counters.childTests > 0u && counters.estimatedNodeBytes > 0u && counters.elapsedNanoseconds > 0u, "benchmark variant records traversal cost counters for " + variant.label);
    if (route.useDirectVirtualTrace)
    {
      ++directVariantCount;
      check(counters.traceRequests > 0u && counters.offscreenTraceRequests > 0u, "direct benchmark variant records trace request counters for " + variant.label);
    }
    if (variant.label == "direct-cubic64-child64")
      child64ChildTests = counters.childTests;
    if (variant.label == "direct-cubic64-grouped-octant")
      groupedChildTests = counters.childTests;

    const auto json = benchmarkJson(variant.label, variant.settings, route, counters);
    for (const std::string required : {
             "\"label\"",
             "\"backend\"",
             "\"node_format\"",
             "\"traversal\"",
             "\"error_sidecar\"",
             "\"trace_requests\"",
             "\"offscreen_trace_requests\"",
             "\"fallback_traces\"",
             "\"unresolved_exact\"",
             "\"node_visits\"",
             "\"child_tests\"",
             "\"group_tests\"",
             "\"skipped_child_groups\"",
             "\"estimated_node_bytes\"",
             "\"elapsed_nanoseconds\"",
         })
    {
      check(json.find(required) != std::string::npos, "benchmark JSON contains " + required + " for " + variant.label);
    }
    if (index != 0u)
      combinedJson += ",";
    combinedJson += json;
  }
  combinedJson += "]";

  std::cout << "  BENCHMARK_JSON " << combinedJson << '\n';
  check(directVariantCount == 4u, "benchmark smoke covers every retained direct virtual trace variant");
  check(child64ChildTests > 0u && groupedChildTests > 0u && groupedChildTests < child64ChildTests, "grouped-octant benchmark exposes lower child-test count than child64 control");
  check(combinedJson.find("tree64-legacy") != std::string::npos && combinedJson.find("direct-bounds16") != std::string::npos, "benchmark smoke emits a JSON array for retained variants");

  uint32_t parsedFormat = svr::kVoxelTraceFormatInvalid;
  svr::VoxelTraceTraversalVariant parsedTraversal = svr::VoxelTraceTraversalVariant::Child64;
  svr::VoxelTraceErrorSidecarMode parsedSidecar = svr::VoxelTraceErrorSidecarMode::Off;
  check(!svr::voxelTraceParseFormat("hybrid64", parsedFormat), "removed hybrid64 format is not exposed as a runtime switch");
  check(!svr::voxelTraceParseTraversalVariant("wide-bounds", parsedTraversal), "unimplemented wide-bounds traversal is not exposed as a runtime switch");
  check(!svr::voxelTraceParseErrorSidecarMode("q8-batch", parsedSidecar), "unimplemented q8-batch error quantization is not exposed as a runtime switch");
}

} // namespace

int main()
{
  testGeneratedTerrainUsesSharedPageResidency();
  testVirtualTraceBenchmarkSmokeEmitsPruneData();
  if (failures != 0)
  {
    std::cout << failures << " Phase 12 check(s) failed\n";
    return 1;
  }
  std::cout << "Sparse voxel terrain convergence Phase 12 tests passed\n";
  return 0;
}
