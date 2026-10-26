#include "voxel/svraster/SparseVoxelTerrainTriangleRaster.hpp"

#include <cstdint>
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

svr::SparseVoxelPageProducer producer()
{
  svr::SparseVoxelPageProducer result{};
  result.producerId = 0x545249414e473330ull;
  result.kind = svr::SparseVoxelPageProducerKind::CPUGenerated;
  result.producerRevision = 9u;
  result.producesOnCPU = true;
  result.debugName = "triangle-phase3";
  return result;
}

svr::SparseVoxelSharedPageTableSettings tableSettings()
{
  svr::SparseVoxelSharedPageTableSettings settings{};
  settings.hierarchyNodeCapacity = 8u;
  settings.childIndexCapacity = 8u;
  settings.brickNodeCapacity = 8u;
  settings.brickOccupancyCapacity = 8u;
  settings.materialCapacity = 8u;
  settings.materialCellCapacity = 8u;
  settings.traceNodeCapacity = 8u;
  settings.triangleClusterCapacity = 8u;
  settings.triangleVertexCapacity = 64u;
  settings.triangleIndexCapacity = 96u;
  return settings;
}

svr::SparseVoxelProducedPagePayload brickRootPayload(
    const svr::SparseVoxelPageId page,
    const uint64_t expectedGeneration,
    const uint32_t producerRevision)
{
  svr::SparseVoxelProducedPagePayload result{};
  result.page = page;
  result.expectedPageGeneration = expectedGeneration;
  result.requiredProducerRevision = producerRevision;
  result.payloadRevision = 31u;
  result.geometryRevision = 0x301u;
  result.materialRevision = 0x301u;
  result.requirements.page = page;
  result.requirements.geometryRevision = result.geometryRevision;
  result.requirements.materialRevision = result.materialRevision;
  result.requirements.traceFormat = svr::kVoxelTraceFormatCubic64;
  result.requirements.hierarchyNodes.count = 1u;
  result.requirements.brickNodes.count = 1u;
  result.requirements.brickOccupancy.count = 1u;
  result.requirements.materials.count = 1u;
  result.requirements.materialCells.count = 1u;
  result.requirements.traceNodes.count = 1u;
  result.installRewriteCount = 1u;
  result.uninstallRewriteCount = 1u;
  result.payloadComplete = true;
  return result;
}

svr::SparseVoxelPageInstallRewrite installRewrite(
    const svr::SparseVoxelReservedPagePayload &reservation)
{
  svr::SparseVoxelPageInstallRewrite rewrite{};
  rewrite.page = reservation.produced.page;
  rewrite.allocation = reservation.allocation;
  rewrite.requiredProducerRevision = reservation.produced.requiredProducerRevision;
  rewrite.rewrite.nodeIndex = reservation.allocation.hierarchyNodes.first;
  rewrite.rewrite.flags = 1u;
  return rewrite;
}

svr::SparseVoxelStreamedInstance instance(
    const svr::SparseVoxelPageProducer &source,
    const svr::SparseVoxelPageId root)
{
  svr::SparseVoxelStreamedInstance result{};
  result.instanceIndex = 3u;
  result.producer = source;
  result.rootPage = root;
  result.firstRootNode = 0u;
  result.rootNodeCount = 1u;
  for (uint32_t diagonal = 0u; diagonal < 4u; ++diagonal)
    result.modelToWorld[diagonal * 4u + diagonal] = 1.0f;
  return result;
}

svr::SparseVoxelTraversalCandidate candidate(
    const svr::SparseVoxelPageProducer &source,
    const svr::SparseVoxelPageId root,
    const svr::SparseVoxelPageId desired)
{
  svr::SparseVoxelTraversalCandidate result{};
  result.instance = instance(source, root);
  result.desiredPage = desired;
  result.visible = true;
  return result;
}

svr::SparseVoxelTerrainTrianglePageSidecar twoClusterSidecar()
{
  svr::SparseVoxelTerrainTrianglePageSidecar sidecar{};
  sidecar.chunkIndex = 4u;
  sidecar.brickPayloadRevision = 0x401u;
  sidecar.tree64Revision = 0x501u;
  sidecar.clipmapLOD = 0u;
  sidecar.regionMappingId = 23u;
  sidecar.voxelSizeMetres = 1.0;
  sidecar.edgeMetres = 8.0;
  sidecar.pageMinimum = {0.0, 0.0, 0.0};
  sidecar.occupiedCells = 2u;
  sidecar.exposedFaces = 2u;
  sidecar.greedyQuads = 2u;

  svr::SparseVoxelBrickMaterialGPU material{};
  material.packed[0] = 77u;
  const std::array<std::array<float, 3>, 8> positions{{
      {{0.0f, 0.0f, 0.0f}},
      {{1.0f, 0.0f, 0.0f}},
      {{1.0f, 0.0f, 1.0f}},
      {{0.0f, 0.0f, 1.0f}},
      {{2.0f, 0.0f, 0.0f}},
      {{3.0f, 0.0f, 0.0f}},
      {{3.0f, 0.0f, 1.0f}},
      {{2.0f, 0.0f, 1.0f}},
  }};
  for (uint32_t index = 0u; index < positions.size(); ++index)
  {
    svr::SparseVoxelTerrainTriangleVertex vertex{};
    vertex.position = positions[index];
    vertex.normal = {0, 1, 0};
    vertex.face = 3u;
    vertex.uv = {
        static_cast<float>((index % 4u) == 1u || (index % 4u) == 2u),
        static_cast<float>((index % 4u) >= 2u)};
    vertex.material = material;
    sidecar.vertices.push_back(vertex);
  }
  sidecar.indices = {
      0u, 1u, 2u, 0u, 2u, 3u,
      4u, 5u, 6u, 4u, 6u, 7u};

  for (uint32_t clusterIndex = 0u; clusterIndex < 2u; ++clusterIndex)
  {
    svr::SparseVoxelTerrainTriangleCluster cluster{};
    cluster.firstVertex = clusterIndex * 4u;
    cluster.vertexCount = 4u;
    cluster.firstIndex = clusterIndex * 6u;
    cluster.indexCount = 6u;
    cluster.firstQuad = clusterIndex;
    cluster.quadCount = 1u;
    cluster.sourceBrickNode = 10u + clusterIndex;
    cluster.lodGroup = clusterIndex;
    cluster.boundsMin = {
        clusterIndex == 0u ? 0.0f : 2.0f,
        0.0f,
        0.0f};
    cluster.boundsMax = {
        clusterIndex == 0u ? 1.0f : 3.0f,
        0.0f,
        1.0f};
    cluster.parentError = 1.0f;
    sidecar.clusters.push_back(cluster);
  }
  return sidecar;
}

bool reserveCopyInstall(
    svr::SparseVoxelSharedPageTable &table,
    const svr::SparseVoxelProducedPagePayload &payload,
    const std::vector<svr::SparseVoxelPageInstallRewrite> &rewrites,
    const uint64_t copyFence,
    svr::SparseVoxelReservedPagePayload &reservation,
    std::string &error)
{
  if (!table.reserveProducedPagePayload(payload, reservation, &error))
    return false;
  if (!table.markPayloadCopied(reservation, copyFence, &error))
    return false;
  return table.installCopiedPagePayload(reservation, rewrites, &error);
}

void installRoot(
    svr::SparseVoxelSharedPageTable &table,
    const svr::SparseVoxelPageProducer &source,
    const svr::SparseVoxelPageId root,
    std::string &error)
{
  svr::SparseVoxelReservedPagePayload rootReservation{};
  check(
      table.reserveProducedPagePayload(
          brickRootPayload(root, table.entry(root)->generation, source.producerRevision),
          rootReservation,
          &error),
      "root brick payload reserves: " + error);
  check(table.markPayloadCopied(rootReservation, 1u, &error), "root brick payload copy completes: " + error);
  check(
      table.installCopiedPagePayload(rootReservation, {installRewrite(rootReservation)}, &error),
      "root brick payload installs: " + error);
}

void installTriangleChild(
    svr::SparseVoxelSharedPageTable &table,
    const svr::SparseVoxelPageProducer &source,
    const svr::SparseVoxelPageId child,
    const svr::SparseVoxelTerrainTrianglePageSidecar &sidecar,
    std::string &error)
{
  svr::SparseVoxelProducedPagePayload produced{};
  svr::SparseVoxelTerrainTriangleMeshSettings meshSettings{};
  check(
      svr::sparseVoxelProducedPagePayloadFromTerrainTriangleSidecar(
          sidecar,
          meshSettings,
          child,
          table.entry(child)->generation,
          source.producerRevision,
          41u,
          produced,
          &error),
      "triangle child sidecar becomes produced payload: " + error);
  svr::SparseVoxelReservedPagePayload reservation{};
  check(
      reserveCopyInstall(table, produced, {}, 2u, reservation, error),
      "triangle child sidecar installs: " + error);
}

void testRendererDrawSelectionFallsBackUntilSidecarIsResident()
{
  std::cout << "renderer draw selection falls back until triangle sidecar is resident\n";
  const auto source = producer();
  const svr::SparseVoxelPageId root{source.producerId, 0u};
  const svr::SparseVoxelPageId child{source.producerId, 1u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure(tableSettings(), &error), "phase3 table configures: " + error);
  check(table.addPage(root, source, {}, true, &error), "phase3 root registers: " + error);
  check(table.addPage(child, source, {root}, false, &error), "phase3 child registers: " + error);
  installRoot(table, source, root, error);

  svr::SparseVoxelTerrainTriangleRasterDrawList drawList{};
  svr::SparseVoxelTerrainTriangleMeshSettings meshSettings{};
  check(
      svr::buildSparseVoxelTerrainTriangleRasterDrawList(
          table,
          {candidate(source, root, child)},
          {},
          meshSettings,
          drawList,
          &error),
      "missing triangle sidecar draw list builds: " + error);
  check(drawList.selections.size() == 1u && drawList.selections[0].path == svr::SparseVoxelTerrainRasterPath::BrickFallback, "missing child triangle sidecar selects brick fallback");
  check(drawList.selections[0].selectedPage == root && drawList.selections[0].requestedDesiredPage, "missing child selection keeps requesting desired page");
  check(drawList.triangleDraws.empty() && drawList.brickFallbackCount == 1u, "missing child emits no triangle draws");
}

void testResidentTriangleSidecarEmitsIndexedClusterDraws()
{
  std::cout << "resident triangle sidecar emits indexed cluster draws\n";
  const auto source = producer();
  const svr::SparseVoxelPageId root{source.producerId, 10u};
  const svr::SparseVoxelPageId child{source.producerId, 11u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure(tableSettings(), &error), "resident triangle table configures: " + error);
  check(table.addPage(root, source, {}, true, &error), "resident triangle root registers: " + error);
  check(table.addPage(child, source, {root}, false, &error), "resident triangle child registers: " + error);
  installRoot(table, source, root, error);
  const auto sidecar = twoClusterSidecar();
  installTriangleChild(table, source, child, sidecar, error);

  const auto *entry = table.entry(child);
  check(entry != nullptr && entry->allocation.hasTriangleRasterPayload(), "child owns resident triangle raster ranges");

  svr::SparseVoxelTerrainTriangleRasterDrawList drawList{};
  svr::SparseVoxelTerrainTriangleMeshSettings meshSettings{};
  const svr::SparseVoxelTerrainTriangleRasterPageView view{
      child,
      entry != nullptr ? entry->geometryRevision : 0u,
      &sidecar};
  check(
      svr::buildSparseVoxelTerrainTriangleRasterDrawList(
          table,
          {candidate(source, root, child)},
          {view},
          meshSettings,
          drawList,
          &error),
      "resident triangle draw list builds: " + error);
  check(drawList.selections.size() == 1u && drawList.selections[0].path == svr::SparseVoxelTerrainRasterPath::TriangleClusters, "resident child selects triangle cluster path");
  check(drawList.triangleSelectionCount == 1u && drawList.triangleDraws.size() == sidecar.clusters.size(), "one draw is emitted per triangle cluster");
  check(drawList.brickFallbackCount == 0u && drawList.staleSidecarCount == 0u, "resident triangle path does not fall back");

  for (uint32_t index = 0u; index < drawList.triangleDraws.size(); ++index)
  {
    const auto &draw = drawList.triangleDraws[index];
    const auto &cluster = sidecar.clusters[index];
    check(draw.page == child && draw.clusterIndex == index, "draw record keeps page and cluster identity");
    check(draw.indirect.indexCount == cluster.indexCount && draw.indirect.instanceCount == 1u, "draw indirect count matches cluster");
    check(draw.indirect.firstIndex == entry->allocation.triangleIndices.first + cluster.firstIndex, "draw indirect first index maps into resident index range");
    check(draw.indirect.vertexOffset == static_cast<int32_t>(entry->allocation.triangleVertices.first), "draw indirect vertex offset maps into resident vertex range");
    check(draw.indirect.firstInstance == index, "draw indirect firstInstance references draw metadata");
  }
}

void testStaleTriangleViewFallsBackToBrickRoot()
{
  std::cout << "stale triangle metadata falls back to brick root\n";
  const auto source = producer();
  const svr::SparseVoxelPageId root{source.producerId, 20u};
  const svr::SparseVoxelPageId child{source.producerId, 21u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure(tableSettings(), &error), "stale triangle table configures: " + error);
  check(table.addPage(root, source, {}, true, &error), "stale triangle root registers: " + error);
  check(table.addPage(child, source, {root}, false, &error), "stale triangle child registers: " + error);
  installRoot(table, source, root, error);
  auto sidecar = twoClusterSidecar();
  installTriangleChild(table, source, child, sidecar, error);

  sidecar.tree64Revision += 1u;
  const auto *entry = table.entry(child);
  const svr::SparseVoxelTerrainTriangleRasterPageView staleView{
      child,
      entry != nullptr ? entry->geometryRevision : 0u,
      &sidecar};

  svr::SparseVoxelTerrainTriangleRasterDrawList drawList{};
  svr::SparseVoxelTerrainTriangleMeshSettings meshSettings{};
  check(
      svr::buildSparseVoxelTerrainTriangleRasterDrawList(
          table,
          {candidate(source, root, child)},
          {staleView},
          meshSettings,
          drawList,
          &error),
      "stale triangle draw list builds: " + error);
  check(drawList.staleSidecarCount == 1u, "stale sidecar mismatch is counted");
  check(drawList.triangleDraws.empty(), "stale sidecar emits no triangle draws");
  check(drawList.selections.size() == 1u && drawList.selections[0].path == svr::SparseVoxelTerrainRasterPath::BrickFallback, "stale sidecar selects brick fallback");
  check(drawList.selections[0].selectedPage == root && drawList.selections[0].selectedFallback, "stale sidecar falls back to root page");
}

} // namespace

int main()
{
  testRendererDrawSelectionFallsBackUntilSidecarIsResident();
  testResidentTriangleSidecarEmitsIndexedClusterDraws();
  testStaleTriangleViewFallsBackToBrickRoot();
  if (failures != 0)
  {
    std::cerr << failures << " triangle phase3 checks failed\n";
    return 1;
  }
  std::cout << "Sparse voxel terrain triangle Phase 3 renderer tests passed\n";
  return 0;
}
