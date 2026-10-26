#include "voxel/svraster/SparseVoxelStreaming.hpp"

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
  result.producerId = 0x545249414e473230ull;
  result.kind = svr::SparseVoxelPageProducerKind::CPUGenerated;
  result.producerRevision = 7u;
  result.producesOnCPU = true;
  result.debugName = "triangle-phase2";
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
  settings.tree64NodeCapacity = 4u;
  settings.tree64MaterialCapacity = 4u;
  settings.traceNodeCapacity = 8u;
  settings.traceErrorWordCapacity = 8u;
  settings.cullErrorWordCapacity = 8u;
  settings.triangleClusterCapacity = 8u;
  settings.triangleVertexCapacity = 32u;
  settings.triangleIndexCapacity = 48u;
  return settings;
}

svr::SparseVoxelProducedPagePayload rootPayload(
    const svr::SparseVoxelPageId page,
    const uint64_t expectedGeneration,
    const uint32_t producerRevision)
{
  svr::SparseVoxelProducedPagePayload result{};
  result.page = page;
  result.expectedPageGeneration = expectedGeneration;
  result.requiredProducerRevision = producerRevision;
  result.payloadRevision = 11u;
  result.geometryRevision = 0x100u;
  result.materialRevision = 0x100u;
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
  rewrite.rewrite.firstChild = 0u;
  rewrite.rewrite.childCount = 0u;
  rewrite.rewrite.flags = 1u;
  return rewrite;
}

svr::SparseVoxelStreamedInstance instance(
    const svr::SparseVoxelPageProducer &source,
    const svr::SparseVoxelPageId root)
{
  svr::SparseVoxelStreamedInstance result{};
  result.instanceIndex = 1u;
  result.producer = source;
  result.rootPage = root;
  result.firstRootNode = 0u;
  result.rootNodeCount = 1u;
  for (uint32_t diagonal = 0u; diagonal < 4u; ++diagonal)
    result.modelToWorld[diagonal * 4u + diagonal] = 1.0f;
  return result;
}

svr::SparseVoxelTerrainTrianglePageSidecar oneQuadSidecar()
{
  svr::SparseVoxelTerrainTrianglePageSidecar sidecar{};
  sidecar.chunkIndex = 3u;
  sidecar.brickPayloadRevision = 0x200u;
  sidecar.tree64Revision = 0x300u;
  sidecar.clipmapLOD = 0u;
  sidecar.regionMappingId = 19u;
  sidecar.voxelSizeMetres = 1.0;
  sidecar.edgeMetres = 8.0;
  sidecar.pageMinimum = {0.0, 0.0, 0.0};
  sidecar.occupiedCells = 1u;
  sidecar.exposedFaces = 1u;
  sidecar.greedyQuads = 1u;

  svr::SparseVoxelBrickMaterialGPU material{};
  material.packed[0] = 42u;
  const std::array<float, 3> positions[4] = {
      std::array<float, 3>{0.0f, 0.0f, 0.0f},
      std::array<float, 3>{1.0f, 0.0f, 0.0f},
      std::array<float, 3>{1.0f, 0.0f, 1.0f},
      std::array<float, 3>{0.0f, 0.0f, 1.0f},
  };
  const std::array<float, 2> uvs[4] = {
      std::array<float, 2>{0.0f, 0.0f},
      std::array<float, 2>{1.0f, 0.0f},
      std::array<float, 2>{1.0f, 1.0f},
      std::array<float, 2>{0.0f, 1.0f},
  };
  for (uint32_t index = 0u; index < 4u; ++index)
  {
    svr::SparseVoxelTerrainTriangleVertex vertex{};
    vertex.position = positions[index];
    vertex.normal = {0, 1, 0};
    vertex.face = 3u;
    vertex.uv = uvs[index];
    vertex.material = material;
    sidecar.vertices.push_back(vertex);
  }
  sidecar.indices = {0u, 1u, 2u, 0u, 2u, 3u};

  svr::SparseVoxelTerrainTriangleCluster cluster{};
  cluster.firstVertex = 0u;
  cluster.vertexCount = 4u;
  cluster.firstIndex = 0u;
  cluster.indexCount = 6u;
  cluster.firstQuad = 0u;
  cluster.quadCount = 1u;
  cluster.sourceBrickNode = 5u;
  cluster.lodGroup = 2u;
  cluster.boundsMin = {0.0f, 0.0f, 0.0f};
  cluster.boundsMax = {1.0f, 0.0f, 1.0f};
  cluster.selfError = 0.0f;
  cluster.parentError = 1.0f;
  sidecar.clusters.push_back(cluster);
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

void testTriangleSidecarInstallsThroughSharedResidency()
{
  std::cout << "triangle raster sidecar installs through shared page residency\n";
  const auto source = producer();
  const svr::SparseVoxelPageId root{source.producerId, 0u};
  const svr::SparseVoxelPageId child{source.producerId, 1u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure(tableSettings(), &error), "triangle page table configures: " + error);
  check(table.addPage(root, source, {}, true, &error), "root fallback page registers: " + error);
  check(table.addPage(child, source, {root}, false, &error), "triangle child page registers with root dependency: " + error);

  svr::SparseVoxelReservedPagePayload rootReservation{};
  check(
      table.reserveProducedPagePayload(
          rootPayload(root, table.entry(root)->generation, source.producerRevision),
          rootReservation,
          &error),
      "root fallback reserves: " + error);
  check(table.markPayloadCopied(rootReservation, 1u, &error), "root fallback copy completes: " + error);
  check(
      table.installCopiedPagePayload(rootReservation, {installRewrite(rootReservation)}, &error),
      "root fallback installs: " + error);

  svr::SparseVoxelInstanceLODSelection selection{};
  check(
      svr::selectSparseVoxelStreamedInstanceLOD(table, instance(source, root), child, selection, &error),
      "missing triangle sidecar selection runs: " + error);
  check(selection.selectedFallback && selection.requestedDesiredPage && selection.selectedPage == root, "missing child sidecar falls back to protected root");

  const auto sidecar = oneQuadSidecar();
  svr::SparseVoxelTerrainTriangleMeshSettings meshSettings{};
  svr::SparseVoxelProducedPagePayload produced{};
  check(
      svr::sparseVoxelProducedPagePayloadFromTerrainTriangleSidecar(
          sidecar,
          meshSettings,
          child,
          table.entry(child)->generation,
          source.producerRevision,
          21u,
          produced,
          &error),
      "triangle sidecar becomes produced page payload: " + error);
  check(produced.installRewriteCount == 0u && produced.requirements.requiresTriangleRasterPayload(), "triangle sidecar payload requires triangle ranges and no hierarchy rewrite");
  check(produced.geometryRevision == sidecar.tree64Revision && produced.materialRevision == sidecar.tree64Revision, "triangle sidecar inherits the Tree64 geometry identity");

  svr::SparseVoxelReservedPagePayload triangleReservation{};
  check(
      reserveCopyInstall(table, produced, {}, 2u, triangleReservation, error),
      "triangle sidecar reserves, copies, and installs with no scene-tree rewrite: " + error);
  const auto *entry = table.entry(child);
  check(entry != nullptr && entry->resident(), "triangle sidecar page becomes resident");
  check(entry != nullptr && entry->rasterVisible() && entry->allocation.hasTriangleRasterPayload(), "triangle sidecar page is visible to raster");
  check(entry != nullptr && !entry->traceable(), "triangle sidecar alone is not a direct trace view");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::TriangleClusters).active == sidecar.clusters.size(), "triangle cluster range is tracked by the shared allocator");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::TriangleVertices).active == sidecar.vertices.size(), "triangle vertex range is tracked by the shared allocator");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::TriangleIndices).active == sidecar.indices.size(), "triangle index range is tracked by the shared allocator");

  check(
      svr::selectSparseVoxelStreamedInstanceLOD(table, instance(source, root), child, selection, &error),
      "resident triangle sidecar selection runs: " + error);
  check(!selection.selectedFallback && !selection.requestedDesiredPage && selection.selectedPage == child, "resident triangle sidecar can satisfy the raster cut");

  auto stale = produced;
  stale.expectedPageGeneration += 1u;
  svr::SparseVoxelReservedPagePayload staleReservation{};
  check(!table.reserveProducedPagePayload(stale, staleReservation, &error), "stale triangle sidecar generation is rejected");

  check(table.uninstallPage(child, {}, 5u, &error), "triangle sidecar page retires without scene-tree uninstall rewrite: " + error);
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::TriangleClusters).active == 0u, "triangle cluster range leaves active storage on retire");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::TriangleClusters).retiring == sidecar.clusters.size(), "triangle cluster range is fenced during retirement");
  check(table.reclaim(5u) >= sidecar.clusters.size(), "triangle sidecar storage reclaims after fence");
}

void testMalformedTriangleSidecarDoesNotReserve()
{
  std::cout << "malformed triangle sidecars cannot reserve shared page storage\n";
  const auto source = producer();
  const svr::SparseVoxelPageId page{source.producerId, 2u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure(tableSettings(), &error), "malformed sidecar table configures: " + error);
  check(table.addPage(page, source, {}, false, &error), "malformed sidecar page registers: " + error);

  auto sidecar = oneQuadSidecar();
  sidecar.indices.back() = 99u;
  svr::SparseVoxelProducedPagePayload produced{};
  svr::SparseVoxelTerrainTriangleMeshSettings meshSettings{};
  check(
      !svr::sparseVoxelProducedPagePayloadFromTerrainTriangleSidecar(
          sidecar,
          meshSettings,
          page,
          table.entry(page)->generation,
          source.producerRevision,
          22u,
          produced,
          &error),
      "malformed triangle sidecar is rejected before range reservation");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::TriangleClusters).active == 0u, "rejected triangle sidecar reserves no cluster storage");
}

} // namespace

int main()
{
  testTriangleSidecarInstallsThroughSharedResidency();
  testMalformedTriangleSidecarDoesNotReserve();
  if (failures != 0)
  {
    std::cerr << failures << " triangle phase2 checks failed\n";
    return 1;
  }
  std::cout << "Sparse voxel terrain triangle Phase 2 streaming tests passed\n";
  return 0;
}
