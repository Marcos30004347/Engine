#include "voxel/svraster/SparseVoxelTerrainVirtualGeometryGPUBuild.hpp"

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
  result.producerId = 0x5445525241494e47ull;
  result.kind = svr::SparseVoxelPageProducerKind::GPUGeneratedTerrain;
  result.producerRevision = 31u;
  result.producesOnGPU = true;
  result.debugName = "terrain-vg-phase9";
  return result;
}

svr::SparseVoxelSharedPageTableSettings tableSettings()
{
  svr::SparseVoxelSharedPageTableSettings settings{};
  settings.hierarchyNodeCapacity = 64u;
  settings.childIndexCapacity = 64u;
  settings.materialCapacity = 64u;
  settings.materialCellCapacity = 64u;
  settings.traceNodeCapacity = 128u;
  settings.traceErrorWordCapacity = 128u;
  settings.cullErrorWordCapacity = 128u;
  settings.virtualGeometryMeshletCapacity = 128u;
  settings.virtualGeometryHierarchyNodeCapacity = 128u;
  settings.virtualGeometryGroupCapacity = 128u;
  settings.virtualGeometryPositionWordCapacity = 4096u;
  settings.virtualGeometryNormalWordCapacity = 4096u;
  settings.virtualGeometryUVWordCapacity = 8192u;
  settings.virtualGeometryIndexByteCapacity = 8192u;
  settings.virtualGeometryBoneWeightWordCapacity = 1024u;
  return settings;
}

svr::SparseVoxelTerrainVGPUPageBuildSettings buildSettings()
{
  svr::SparseVoxelTerrainVGPUPageBuildSettings settings{};
  settings.requireVGRasterPayload = true;
  settings.requireTracePayload = true;
  settings.requireTraceErrorSidecar = true;
  settings.requireDependencyMetadata = true;
  settings.requireMaterialPayload = false;
  return settings;
}

svr::SparseVoxelStreamedInstance instance(
    const svr::SparseVoxelPageProducer &source,
    const svr::SparseVoxelPageId root)
{
  svr::SparseVoxelStreamedInstance result{};
  result.instanceIndex = 9u;
  result.producer = source;
  result.rootPage = root;
  result.firstRootNode = 0u;
  result.rootNodeCount = 1u;
  for (uint32_t diagonal = 0u; diagonal < 4u; ++diagonal)
    result.modelToWorld[diagonal * 4u + diagonal] = 1.0f;
  return result;
}

svr::SparseVoxelTerrainVGPUPageBuildResult readyBuild(
    const svr::SparseVoxelPageId page,
    const svr::SparseVoxelPageId parent,
    const uint64_t expectedGeneration,
    const uint32_t producerRevision,
    const uint64_t revision,
    const bool rootPage = false)
{
  svr::SparseVoxelTerrainVGPUPageBuildResult result{};
  result.page = page;
  result.parentPage = parent;
  result.fallbackPage = parent;
  result.expectedPageGeneration = expectedGeneration;
  result.requiredProducerRevision = producerRevision;
  result.payloadRevision = revision;
  result.geometryRevision = revision;
  result.materialRevision = 0u;
  result.productionFence = 14u;
  result.terrainGeneration = 55u;
  result.buildRevision = producerRevision;
  result.status = static_cast<uint32_t>(svr::SparseVoxelTerrainVGPUPageBuildStatus::Ready);
  result.stageFlags = svr::kSparseVoxelTerrainVGPURequiredStages;
  result.sourceVoxelCount = 512u;
  result.exposedFaceCount = 384u;
  result.greedyQuadCount = 384u;
  result.vgMeshletCount = 8u;
  result.vgGroupCount = 3u;
  result.vgHierarchyNodeCount = 5u;
  result.vgInstallRewriteCount = 2u;
  result.vgUninstallRewriteCount = 2u;
  result.vgVertexCount = 1536u;
  result.vgTriangleCount = 768u;
  result.vgMaxVerticesPerMeshlet = 192u;
  result.vgMaxTrianglesPerMeshlet = 96u;
  result.vgPositionWordCount = 1536u;
  result.vgNormalWordCount = 1536u;
  result.vgUVWordCount = 3072u;
  result.vgIndexByteCount = 2304u;
  result.vgBoneWeightWordCount = 0u;
  result.traceFormat = svr::kVoxelTraceFormatCubic64;
  result.traceNodeCount = 24u;
  result.traceErrorWordCount = 12u;
  result.cullErrorWordCount = 12u;
  result.materialCount = 4u;
  result.materialCellCount = 4u;
  result.dependencyCount = rootPage ? 0u : 1u;
  result.rootPage = rootPage;
  return result;
}

std::vector<svr::SparseVoxelPageInstallRewrite> installRewrites(
    const svr::SparseVoxelReservedPagePayload &reservation)
{
  std::vector<svr::SparseVoxelPageInstallRewrite> rewrites;
  rewrites.reserve(reservation.produced.installRewriteCount);
  for (uint32_t index = 0u; index < reservation.produced.installRewriteCount; ++index)
  {
    svr::SparseVoxelPageInstallRewrite rewrite{};
    rewrite.page = reservation.produced.page;
    rewrite.allocation = reservation.allocation;
    rewrite.requiredProducerRevision = reservation.produced.requiredProducerRevision;
    rewrite.rewrite.nodeIndex = reservation.allocation.hierarchyNodes.first + index;
    rewrite.rewrite.firstChild = 0u;
    rewrite.rewrite.childCount = 0u;
    rewrite.rewrite.flags = 1u;
    rewrites.push_back(rewrite);
  }
  return rewrites;
}

bool publishReadyBuild(
    svr::SparseVoxelSharedPageTable &table,
    const svr::SparseVoxelTerrainVGPUPageBuildResult &build,
    std::string &error)
{
  svr::SparseVoxelTerrainVGPUPagePublication publication{};
  if (!svr::prepareSparseVoxelTerrainVGPUPageBuildPublication(
          table,
          build,
          buildSettings(),
          build.productionFence,
          publication,
          &error))
  {
    return false;
  }
  return table.installCopiedPagePayload(
      publication.reservation,
      installRewrites(publication.reservation),
      &error);
}

bool allocationMatchesBuild(
    const svr::SparseVoxelPhysicalPageAllocation &allocation,
    const svr::SparseVoxelTerrainVGPUPageBuildResult &build)
{
  return allocation.virtualGeometryMeshlets.count == build.vgMeshletCount &&
      allocation.virtualGeometryHierarchyNodes.count ==
          build.vgHierarchyNodeCount &&
      allocation.virtualGeometryGroups.count == build.vgGroupCount &&
      allocation.virtualGeometryPositionWords.count ==
          build.vgPositionWordCount &&
      allocation.virtualGeometryNormalWords.count ==
          build.vgNormalWordCount &&
      allocation.virtualGeometryUVWords.count == build.vgUVWordCount &&
      allocation.virtualGeometryIndexBytes.count == build.vgIndexByteCount &&
      allocation.virtualGeometryBoneWeightWords.count ==
          build.vgBoneWeightWordCount &&
      allocation.traceNodes.count == build.traceNodeCount &&
      allocation.traceErrorWords.count == build.traceErrorWordCount &&
      allocation.cullErrorWords.count == build.cullErrorWordCount &&
      allocation.materials.count == build.materialCount &&
      allocation.materialCells.count == build.materialCellCount;
}

void testVGPUPageConvertsToSharedPayload()
{
  std::cout << "VG GPU build converts to a shared page payload with explicit VG raster ranges\n";
  const auto source = producer();
  const svr::SparseVoxelPageId root{source.producerId, 0u};
  auto build = readyBuild(root, {}, 0u, source.producerRevision, 0x9001u, true);

  svr::SparseVoxelProducedPagePayload payload{};
  std::string error;
  check(
      svr::sparseVoxelProducedPagePayloadFromTerrainVGPUPageBuild(
          build,
          buildSettings(),
          payload,
          &error),
      "complete VG GPU build converts to produced payload: " + error);
  check(payload.valid(), "produced payload is valid");
  check(payload.requirements.requiresVirtualGeometryRasterPayload(), "payload requires VG raster ranges");
  check(!payload.requirements.requiresTriangleRasterPayload(), "payload does not allocate legacy triangle sidecar ranges");
  check(payload.requirements.traceNodes.count == build.traceNodeCount, "payload carries trace node requirements");
  check(payload.installRewriteCount == build.vgInstallRewriteCount, "payload preserves scene install rewrite count");
}

void testVGPUPagePublishesAtomicallyAndWinsUnifiedCut()
{
  std::cout << "VG GPU pages publish atomically and win the unified cut only after dependencies are resident\n";
  const auto source = producer();
  const svr::SparseVoxelPageId root{source.producerId, 10u};
  const svr::SparseVoxelPageId child{source.producerId, 11u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure(tableSettings(), &error), "table configures: " + error);
  check(table.addPage(root, source, {}, true, &error), "root page registers: " + error);
  check(table.addPage(child, source, {root}, false, &error), "child page registers: " + error);

  auto rootBuild = readyBuild(root, {}, table.entry(root)->generation, source.producerRevision, 0x9101u, true);
  check(publishReadyBuild(table, rootBuild, error), "root VG GPU page publishes: " + error);
  const auto *rootEntry = table.entry(root);
  check(rootEntry != nullptr && rootEntry->resident() && rootEntry->unifiedGeometryVisible(), "root exposes unified VG raster + trace payload");
  check(rootEntry != nullptr && rootEntry->allocation.hasVirtualGeometryRasterPayload(), "root allocation owns VG raster ranges");
  check(rootEntry != nullptr && allocationMatchesBuild(rootEntry->allocation, rootBuild), "root allocation ranges exactly match the GPU build result");
  check(rootEntry != nullptr && rootEntry->generation == rootBuild.payloadRevision && rootEntry->geometryRevision == rootBuild.geometryRevision, "root entry publishes exact payload/geometry revisions");

  auto childBuild = readyBuild(child, root, table.entry(child)->generation, source.producerRevision, 0x9102u, false);
  svr::SparseVoxelTerrainVGPUPagePublication publication{};
  check(
      svr::prepareSparseVoxelTerrainVGPUPageBuildPublication(
          table,
          childBuild,
          buildSettings(),
          17u,
          publication,
          &error),
      "child VG GPU page validates, reserves, and copies hidden ranges: " + error);
  check(publication.validated && publication.reserved && publication.copied && !publication.committed, "child is copied but not committed before install");
  const auto *hiddenChild = table.entry(child);
  check(hiddenChild != nullptr && hiddenChild->state == svr::SparseVoxelPageState::UploadQueued && !hiddenChild->resident(), "hidden child is not render-visible while upload-queued");

  check(
      table.installCopiedPagePayload(
          publication.reservation,
          installRewrites(publication.reservation),
          &error),
      "child VG GPU payload commits after dependency is resident: " + error);
  const auto *childEntry = table.entry(child);
  check(childEntry != nullptr && childEntry->resident() && childEntry->unifiedGeometryVisible(), "child exposes unified VG raster + trace payload after commit");
  check(childEntry != nullptr && childEntry->allocation.hasVirtualGeometryRasterPayload() && childEntry->allocation.hasTracePayload(), "child owns both VG raster and trace ranges");
  check(childEntry != nullptr && allocationMatchesBuild(childEntry->allocation, childBuild), "child allocation ranges exactly match the GPU build result");
  check(childEntry != nullptr && childEntry->generation == childBuild.payloadRevision && childEntry->geometryRevision == childBuild.geometryRevision, "child entry publishes exact payload/geometry revisions");
  check(childEntry != nullptr && childEntry->producer.producerRevision == source.producerRevision, "child install preserves the producer revision guard");
  check(childEntry != nullptr && childEntry->allocation.packedSlot != svr::kSparseVoxelInvalidPackedPageSlot, "child owns a packed page slot/pagelet");

  const auto slots = table.packedPageSlots();
  check(!slots.empty() && slots.front().pageletCount >= 2u, "root and child can share packed pagelets inside one slot");

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
      "unified selector runs after VG GPU publication: " + error);
  check(!selection.selectedFallback && selection.selectedPage == child, "fully published VG child wins the unified cut");
}

void testPackedSlotOverflowRejectsBeforeReservation()
{
  std::cout << "packed slot overflow rejects before any VG ranges are reserved\n";
  const auto source = producer();
  const svr::SparseVoxelPageId page{source.producerId, 20u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  auto settings = tableSettings();
  settings.packedPageSlotBytes = 256u;
  check(table.configure(settings, &error), "overflow table configures: " + error);
  check(table.addPage(page, source, {}, true, &error), "overflow page registers: " + error);

  auto build = readyBuild(page, {}, table.entry(page)->generation, source.producerRevision, 0x9201u, true);
  svr::SparseVoxelTerrainVGPUPagePublication publication{};
  check(
      !svr::prepareSparseVoxelTerrainVGPUPageBuildPublication(
          table,
          build,
          buildSettings(),
          19u,
          publication,
          &error),
      "oversized VG GPU page rejects before hidden reservation");
  check(!publication.reserved, "oversized page reserves no hidden payload");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::VirtualGeometryMeshlets).active == 0u, "no VG meshlet range leaks after overflow");
}

void testDependencyFailureLeavesCopiedPageHidden()
{
  std::cout << "missing parent dependency leaves copied VG GPU child hidden\n";
  const auto source = producer();
  const svr::SparseVoxelPageId root{source.producerId, 30u};
  const svr::SparseVoxelPageId parent{source.producerId, 31u};
  const svr::SparseVoxelPageId child{source.producerId, 32u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure(tableSettings(), &error), "dependency table configures: " + error);
  check(table.addPage(root, source, {}, true, &error), "dependency root registers: " + error);
  check(table.addPage(parent, source, {root}, false, &error), "dependency parent registers but stays non-resident: " + error);
  check(table.addPage(child, source, {parent}, false, &error), "dependency child registers: " + error);

  auto rootBuild = readyBuild(root, {}, table.entry(root)->generation, source.producerRevision, 0x9301u, true);
  check(publishReadyBuild(table, rootBuild, error), "dependency root publishes: " + error);

  auto childBuild = readyBuild(child, parent, table.entry(child)->generation, source.producerRevision, 0x9302u, false);
  svr::SparseVoxelTerrainVGPUPagePublication publication{};
  check(
      svr::prepareSparseVoxelTerrainVGPUPageBuildPublication(
          table,
          childBuild,
          buildSettings(),
          21u,
          publication,
          &error),
      "child validates, reserves, and copies while parent is missing: " + error);
  check(
      !table.installCopiedPagePayload(
          publication.reservation,
          installRewrites(publication.reservation),
          &error),
      "child commit rejects while parent dependency is not resident");

  const auto *entry = table.entry(child);
  check(entry != nullptr && entry->state == svr::SparseVoxelPageState::UploadQueued, "failed dependency commit leaves child upload-queued");
  check(entry != nullptr && !entry->resident() && !entry->rasterVisible() && !entry->traceable(), "failed dependency commit exposes no partial VG raster or trace view");

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

  svr::SparseVoxelUnifiedGeometryTraversalResult traversal{};
  svr::SparseVoxelUnifiedGeometryCutSettings cutSettings{};
  check(
      svr::traverseSparseVoxelUnifiedGeometryLOD(
          table,
          {candidate},
          view,
          cutSettings,
          traversal,
          &error),
      "traversal still requests the missing dependency closure: " + error);
  check(traversal.selections.size() == 1u && traversal.selections[0].selectedPage == root, "root fallback remains selected");
  check(traversal.pageRequests.size() == 2u, "request closure includes missing parent and hidden child");
}

} // namespace

int main()
{
  testVGPUPageConvertsToSharedPayload();
  testVGPUPagePublishesAtomicallyAndWinsUnifiedCut();
  testPackedSlotOverflowRejectsBeforeReservation();
  testDependencyFailureLeavesCopiedPageHidden();

  if (failures != 0)
  {
    std::cerr << failures << " terrain VG Phase 9 residency/publication checks failed\n";
    return 1;
  }

  std::cout << "Sparse voxel terrain virtual-geometry Phase 9 residency/publication tests passed\n";
  return 0;
}
