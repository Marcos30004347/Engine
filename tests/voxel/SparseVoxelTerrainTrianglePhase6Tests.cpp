#include "voxel/svraster/SparseVoxelTerrainTriangleGPUBuild.hpp"

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
  result.producerId = 0x545249414e473630ull;
  result.kind = svr::SparseVoxelPageProducerKind::GPUGeneratedTerrain;
  result.producerRevision = 13u;
  result.producesOnGPU = true;
  result.debugName = "triangle-phase6";
  return result;
}

svr::SparseVoxelSharedPageTableSettings tableSettings()
{
  svr::SparseVoxelSharedPageTableSettings settings{};
  settings.hierarchyNodeCapacity = 32u;
  settings.childIndexCapacity = 32u;
  settings.brickNodeCapacity = 8u;
  settings.brickOccupancyCapacity = 8u;
  settings.materialCapacity = 32u;
  settings.materialCellCapacity = 32u;
  settings.traceNodeCapacity = 64u;
  settings.traceErrorWordCapacity = 64u;
  settings.cullErrorWordCapacity = 32u;
  settings.triangleClusterCapacity = 64u;
  settings.triangleVertexCapacity = 1024u;
  settings.triangleIndexCapacity = 1536u;
  return settings;
}

svr::SparseVoxelStreamedInstance instance(
    const svr::SparseVoxelPageProducer &source,
    const svr::SparseVoxelPageId root)
{
  svr::SparseVoxelStreamedInstance result{};
  result.instanceIndex = 6u;
  result.producer = source;
  result.rootPage = root;
  result.firstRootNode = 0u;
  result.rootNodeCount = 1u;
  for (uint32_t diagonal = 0u; diagonal < 4u; ++diagonal)
    result.modelToWorld[diagonal * 4u + diagonal] = 1.0f;
  return result;
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
  result.payloadRevision = 0x6001u;
  result.geometryRevision = 0x6001u;
  result.materialRevision = 0x6001u;
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
  result.requirements.traceErrorWords.count = 1u;
  result.installRewriteCount = 1u;
  result.uninstallRewriteCount = 1u;
  result.payloadComplete = true;
  return result;
}

svr::SparseVoxelTerrainTriangleGPUPageBuildSettings buildSettings()
{
  svr::SparseVoxelTerrainTriangleGPUPageBuildSettings settings{};
  settings.requireMaterialPayload = true;
  return settings;
}

svr::SparseVoxelTerrainTriangleGPUPageBuildResult readyBuild(
    const svr::SparseVoxelPageId page,
    const svr::SparseVoxelPageId parent,
    const uint64_t expectedGeneration,
    const uint32_t producerRevision,
    const uint64_t revision)
{
  svr::SparseVoxelTerrainTriangleGPUPageBuildResult result{};
  result.page = page;
  result.parentPage = parent;
  result.fallbackPage = parent;
  result.expectedPageGeneration = expectedGeneration;
  result.requiredProducerRevision = producerRevision;
  result.payloadRevision = revision;
  result.geometryRevision = revision;
  result.materialRevision = revision;
  result.productionFence = 21u;
  result.terrainGeneration = 91u;
  result.buildRevision = producerRevision;
  result.status = static_cast<uint32_t>(svr::SparseVoxelTerrainTriangleGPUPageBuildStatus::Ready);
  result.stageFlags = svr::kSparseVoxelTerrainTriangleGPUBuildRequiredStages;
  result.sourceVoxelCount = 64u;
  result.exposedFaceCount = 96u;
  result.greedyQuadCount = 96u;
  result.hierarchyNodeCount = 2u;
  result.childIndexCount = 4u;
  result.triangleClusterCount = 3u;
  result.triangleVertexCount = 384u;
  result.triangleIndexCount = 576u;
  result.traceFormat = svr::kVoxelTraceFormatCubic64;
  result.traceNodeCount = 5u;
  result.traceErrorWordCount = 5u;
  result.cullErrorWordCount = 2u;
  result.materialCount = 3u;
  result.materialCellCount = 3u;
  result.dependencyCount = 1u;
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
  rewrite.rewrite.firstChild = reservation.allocation.childIndices.empty() ? 0u : reservation.allocation.childIndices.first;
  rewrite.rewrite.childCount = reservation.allocation.childIndices.count;
  rewrite.rewrite.flags = 1u;
  return rewrite;
}

bool reserveCopyInstallRoot(
    svr::SparseVoxelSharedPageTable &table,
    const svr::SparseVoxelPageId root,
    const svr::SparseVoxelPageProducer &source,
    std::string &error)
{
  svr::SparseVoxelReservedPagePayload reservation{};
  if (!table.reserveProducedPagePayload(
          rootPayload(root, table.entry(root)->generation, source.producerRevision),
          reservation,
          &error))
  {
    return false;
  }
  if (!table.markPayloadCopied(reservation, 1u, &error))
    return false;
  return table.installCopiedPagePayload(reservation, {installRewrite(reservation)}, &error);
}

void testIncompleteGPUBuildCannotBecomeProducedPayload()
{
  std::cout << "incomplete GPU build cannot become a produced page payload\n";
  const auto source = producer();
  const svr::SparseVoxelPageId root{source.producerId, 0u};
  const svr::SparseVoxelPageId child{source.producerId, 1u};
  auto build = readyBuild(child, root, 0u, source.producerRevision, 0x6101u);
  build.traceNodeCount = 0u;

  svr::SparseVoxelTerrainTriangleGPUPageBuildValidation validation{};
  std::string error;
  check(
      !svr::validateSparseVoxelTerrainTriangleGPUPageBuild(
          build,
          buildSettings(),
          validation,
          &error),
      "missing trace nodes reject the GPU build");
  check(validation.completeRasterPayload && !validation.completeTracePayload, "validation reports raster ready but trace incomplete");

  svr::SparseVoxelProducedPagePayload payload{};
  check(
      !svr::sparseVoxelProducedPagePayloadFromTerrainTriangleGPUBuild(
          build,
          buildSettings(),
          payload,
          &error),
      "partial GPU build is not converted into a streamable payload");
}

void testReadyGPUBuildConvertsToUnifiedPayload()
{
  std::cout << "ready GPU build converts to a unified raster+trace payload\n";
  const auto source = producer();
  const svr::SparseVoxelPageId root{source.producerId, 10u};
  const svr::SparseVoxelPageId child{source.producerId, 11u};
  const auto build = readyBuild(child, root, 0u, source.producerRevision, 0x6201u);

  svr::SparseVoxelTerrainTriangleGPUPageBuildValidation validation{};
  std::string error;
  check(
      svr::validateSparseVoxelTerrainTriangleGPUPageBuild(
          build,
          buildSettings(),
          validation,
          &error),
      "complete GPU build validates: " + error);
  check(validation.ready && validation.completeRasterPayload && validation.completeTracePayload && validation.completeDependencyPayload, "validation marks all payload views complete");
  check(validation.totalPayloadBytes == validation.rasterPayloadBytes + validation.tracePayloadBytes + validation.materialPayloadBytes, "validation reports a complete byte budget");

  svr::SparseVoxelProducedPagePayload payload{};
  check(
      svr::sparseVoxelProducedPagePayloadFromTerrainTriangleGPUBuild(
          build,
          buildSettings(),
          payload,
          &error),
      "complete GPU build converts to produced payload: " + error);
  check(payload.requirements.requiresTriangleRasterPayload(), "produced payload requires triangle raster ranges");
  check(payload.requirements.traceNodes.count == build.traceNodeCount && payload.requirements.traceFormat == build.traceFormat, "produced payload requires direct trace ranges");
  check(payload.requirements.materials.count == build.materialCount && payload.requirements.materialCells.count == build.materialCellCount, "produced payload requires material ranges");
  check(payload.requirements.geometryRevision == build.geometryRevision && payload.geometryRevision == build.geometryRevision, "produced payload preserves geometry revision");
  check(payload.installRewriteCount == 1u && payload.uninstallRewriteCount == 1u, "produced payload carries scene-tree rewrite counts");
}

void testWholeBrickFaceLocalGPUBuildCounters()
{
  std::cout << "whole-brick face-local GPU build counters validate\n";
  const auto source = producer();
  const svr::SparseVoxelPageId root{source.producerId, 14u};
  const svr::SparseVoxelPageId child{source.producerId, 15u};
  auto build = readyBuild(child, root, 0u, source.producerRevision, 0x6261u);
  build.sourceVoxelCount = svr::kSparseVoxelBrickVoxelCount;
  build.exposedFaceCount = 6u * 8u * 8u;
  build.greedyQuadCount = build.exposedFaceCount;
  build.triangleClusterCount = 6u;
  build.triangleVertexCount = build.exposedFaceCount * 4u;
  build.triangleIndexCount = build.exposedFaceCount * 6u;

  svr::SparseVoxelTerrainTriangleGPUPageBuildValidation validation{};
  std::string error;
  check(
      svr::validateSparseVoxelTerrainTriangleGPUPageBuild(
          build,
          buildSettings(),
          validation,
          &error),
      "face-local GPU build accepts an isolated full-brick exposed shell: " +
          error);
  check(
      validation.completeRasterPayload,
      "face-local GPU build reports complete raster payload");

  auto missingFaces = build;
  missingFaces.exposedFaceCount -= 64u;
  check(
      !svr::validateSparseVoxelTerrainTriangleGPUPageBuild(
          missingFaces,
          buildSettings(),
          validation,
          &error),
      "face-local GPU build rejects missing one brick side while vertex/index counts still describe the old payload");

  auto sharedVertices = build;
  sharedVertices.triangleVertexCount -= 4u;
  check(
      !svr::validateSparseVoxelTerrainTriangleGPUPageBuild(
          sharedVertices,
          buildSettings(),
          validation,
          &error),
      "face-local GPU build rejects shared or missing voxel-face vertices");
}

void testDefaultTriangleGPUBuildRejectsMergedVoxelFaces()
{
  std::cout << "default triangle GPU build rejects merged voxel-face payloads\n";
  const auto source = producer();
  const svr::SparseVoxelPageId root{source.producerId, 12u};
  const svr::SparseVoxelPageId child{source.producerId, 13u};
  auto build = readyBuild(child, root, 0u, source.producerRevision, 0x6251u);
  build.greedyQuadCount = build.exposedFaceCount / 2u;

  svr::SparseVoxelTerrainTriangleGPUPageBuildValidation validation{};
  std::string error;
  check(
      !svr::validateSparseVoxelTerrainTriangleGPUPageBuild(
          build,
          buildSettings(),
          validation,
          &error),
      "default settings reject a merged/greedy triangle payload");
  check(!validation.completeRasterPayload, "merged faces mark the triangle raster payload incomplete");

  auto settings = buildSettings();
  settings.requireFaceLocalVoxelFaces = false;
  error.clear();
  check(
      svr::validateSparseVoxelTerrainTriangleGPUPageBuild(
          build,
          settings,
          validation,
          &error),
      "explicit compression mode can still validate greedy triangle benchmark payloads: " + error);
}

void testGPUBuildPublishesAtomicallyAndThenWinsUnifiedCut()
{
  std::cout << "GPU build publishes atomically and then wins unified cut\n";
  const auto source = producer();
  const svr::SparseVoxelPageId root{source.producerId, 20u};
  const svr::SparseVoxelPageId child{source.producerId, 21u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure(tableSettings(), &error), "atomic table configures: " + error);
  check(table.addPage(root, source, {}, true, &error), "atomic root registers: " + error);
  check(table.addPage(child, source, {root}, false, &error), "atomic child registers: " + error);
  check(reserveCopyInstallRoot(table, root, source, error), "atomic root fallback installs: " + error);

  auto invalid = readyBuild(child, root, table.entry(child)->generation, source.producerRevision, 0x6301u);
  invalid.stageFlags &= ~svr::kSparseVoxelTerrainTriangleGPUBuildTraceBuilt;
  svr::SparseVoxelTerrainTriangleGPUPagePublication publication{};
  check(
      !svr::publishSparseVoxelTerrainTriangleGPUPageBuild(
          table,
          invalid,
          buildSettings(),
          2u,
          {},
          publication,
          &error),
      "invalid GPU build cannot publish");
  const auto *afterInvalid = table.entry(child);
  check(afterInvalid != nullptr && !afterInvalid->resident() && !afterInvalid->rasterVisible() && !afterInvalid->traceable(), "invalid publish exposes no partial child page");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::TriangleClusters).active == 0u, "invalid publish reserves no triangle storage");

  const auto valid = readyBuild(child, root, table.entry(child)->generation, source.producerRevision, 0x6302u);
  check(
      svr::prepareSparseVoxelTerrainTriangleGPUPageBuildPublication(
          table,
          valid,
          buildSettings(),
          3u,
          publication,
          &error),
      "valid GPU build validates, reserves, and copies hidden ranges: " + error);
  check(publication.validated && publication.reserved && publication.copied && !publication.committed, "prepared GPU publication is copied but not committed");
  check(
      table.installCopiedPagePayload(
          publication.reservation,
          {installRewrite(publication.reservation)},
          &error),
      "valid GPU payload commits atomically: " + error);

  const auto *entry = table.entry(child);
  check(entry != nullptr && entry->resident() && entry->unifiedGeometryVisible(), "committed child exposes unified raster+trace geometry");
  check(entry != nullptr && entry->allocation.hasTriangleRasterPayload() && entry->allocation.hasTracePayload(), "committed child owns triangle and trace ranges");

  svr::SparseVoxelUnifiedGeometryLODSelection selection{};
  svr::SparseVoxelUnifiedGeometryCutSettings cutSettings{};
  cutSettings.requireMaterialPayload = true;
  check(
      svr::selectSparseVoxelUnifiedGeometryLOD(
          table,
          instance(source, root),
          child,
          cutSettings,
          selection,
          &error),
      "unified selector runs after GPU publication: " + error);
  check(!selection.selectedFallback && selection.selectedPage == child, "fully published GPU page wins the unified cut");
}

void testPackedSlotOverflowLeavesFallbackSelected()
{
  std::cout << "packed slot overflow leaves fallback selected\n";
  const auto source = producer();
  const svr::SparseVoxelPageId root{source.producerId, 30u};
  const svr::SparseVoxelPageId child{source.producerId, 31u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure(tableSettings(), &error), "overflow table configures: " + error);
  check(table.addPage(root, source, {}, true, &error), "overflow root registers: " + error);
  check(table.addPage(child, source, {root}, false, &error), "overflow child registers: " + error);
  check(reserveCopyInstallRoot(table, root, source, error), "overflow root fallback installs: " + error);

  auto smallSettings = buildSettings();
  smallSettings.packedPageSlotBytes = 64u;
  const auto build = readyBuild(child, root, table.entry(child)->generation, source.producerRevision, 0x6401u);
  svr::SparseVoxelTerrainTriangleGPUPagePublication publication{};
  check(
      !svr::publishSparseVoxelTerrainTriangleGPUPageBuild(
          table,
          build,
          smallSettings,
          2u,
          {},
          publication,
          &error),
      "oversized GPU build rejects before publication");
  check(!publication.reserved, "oversized GPU build reserves no hidden page ranges");

  svr::SparseVoxelUnifiedGeometryLODSelection selection{};
  svr::SparseVoxelUnifiedGeometryCutSettings cutSettings{};
  check(
      svr::selectSparseVoxelUnifiedGeometryLOD(
          table,
          instance(source, root),
          child,
          cutSettings,
          selection,
          &error),
      "unified selection survives overflow: " + error);
  check(selection.selectedFallback && selection.selectedPage == root && selection.requestedDesiredPage, "overflow keeps protected root fallback selected and child requested");
}

void testDependencyFailureLeavesCopiedPageHidden()
{
  std::cout << "dependency failure leaves copied GPU page hidden\n";
  const auto source = producer();
  const svr::SparseVoxelPageId root{source.producerId, 40u};
  const svr::SparseVoxelPageId parent{source.producerId, 41u};
  const svr::SparseVoxelPageId child{source.producerId, 42u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure(tableSettings(), &error), "dependency table configures: " + error);
  check(table.addPage(root, source, {}, true, &error), "dependency root registers: " + error);
  check(table.addPage(parent, source, {root}, false, &error), "dependency parent registers but stays non-resident: " + error);
  check(table.addPage(child, source, {parent}, false, &error), "dependency child registers under non-resident parent: " + error);
  check(reserveCopyInstallRoot(table, root, source, error), "dependency root fallback installs: " + error);

  const auto build = readyBuild(child, parent, table.entry(child)->generation, source.producerRevision, 0x6501u);
  svr::SparseVoxelTerrainTriangleGPUPagePublication publication{};
  check(
      svr::prepareSparseVoxelTerrainTriangleGPUPageBuildPublication(
          table,
          build,
          buildSettings(),
          2u,
          publication,
          &error),
      "dependency child build validates, reserves, and copies hidden ranges: " + error);
  check(
      !table.installCopiedPagePayload(
          publication.reservation,
          {installRewrite(publication.reservation)},
          &error),
      "dependency child commit rejects while parent dependency is missing");

  const auto *entry = table.entry(child);
  check(entry != nullptr && entry->state == svr::SparseVoxelPageState::UploadQueued, "failed dependency commit leaves child upload-queued");
  check(entry != nullptr && !entry->resident() && !entry->rasterVisible() && !entry->traceable(), "failed dependency commit exposes no partial raster or trace view");

  svr::SparseVoxelUnifiedGeometryTraversalResult traversal{};
  svr::SparseVoxelTraversalCandidate candidate{};
  candidate.instance = instance(source, root);
  candidate.desiredPage = child;
  candidate.desiredPageBounds.localBoundsMin = {-1.0f, -1.0f, 4.0f};
  candidate.desiredPageBounds.localBoundsMax = {1.0f, 1.0f, 6.0f};
  candidate.visible = true;
  svr::SparseVoxelPagePriorityView view{};
  for (uint32_t diagonal = 0u; diagonal < 4u; ++diagonal)
  {
    view.modelToWorld[diagonal * 4u + diagonal] = 1.0f;
    view.worldToCamera[diagonal * 4u + diagonal] = 1.0f;
  }
  view.focalPixels = {800.0f, 800.0f};
  view.cameraWorldPosition = {0.0f, 0.0f, 0.0f};
  svr::SparseVoxelUnifiedGeometryCutSettings cutSettings{};
  check(
      svr::traverseSparseVoxelUnifiedGeometryLOD(
          table,
          {candidate},
          view,
          cutSettings,
          traversal,
          &error),
      "dependency failure traversal still requests closure: " + error);
  check(traversal.selections.size() == 1u && traversal.selections[0].selectedPage == root, "dependency failure keeps root fallback selected");
  check(traversal.pageRequests.size() == 2u, "dependency closure requests missing parent and incomplete child");
}

} // namespace

int main()
{
  testIncompleteGPUBuildCannotBecomeProducedPayload();
  testReadyGPUBuildConvertsToUnifiedPayload();
  testWholeBrickFaceLocalGPUBuildCounters();
  testDefaultTriangleGPUBuildRejectsMergedVoxelFaces();
  testGPUBuildPublishesAtomicallyAndThenWinsUnifiedCut();
  testPackedSlotOverflowLeavesFallbackSelected();
  testDependencyFailureLeavesCopiedPageHidden();
  if (failures != 0)
  {
    std::cerr << failures << " triangle phase6 checks failed\n";
    return 1;
  }
  std::cout << "Sparse voxel terrain triangle Phase 6 GPU-build publication tests passed\n";
  return 0;
}
