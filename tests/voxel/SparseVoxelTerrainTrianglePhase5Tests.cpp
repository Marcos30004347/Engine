#include "voxel/svraster/SparseVoxelStreaming.hpp"

#include <array>
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
  result.producerId = 0x545249414e473530ull;
  result.kind = svr::SparseVoxelPageProducerKind::CPUGenerated;
  result.producerRevision = 11u;
  result.producesOnCPU = true;
  result.debugName = "triangle-phase5";
  return result;
}

svr::SparseVoxelSharedPageTableSettings tableSettings()
{
  svr::SparseVoxelSharedPageTableSettings settings{};
  settings.hierarchyNodeCapacity = 16u;
  settings.childIndexCapacity = 8u;
  settings.brickNodeCapacity = 8u;
  settings.brickOccupancyCapacity = 8u;
  settings.materialCapacity = 8u;
  settings.materialCellCapacity = 8u;
  settings.traceNodeCapacity = 16u;
  settings.traceErrorWordCapacity = 16u;
  settings.cullErrorWordCapacity = 8u;
  settings.triangleClusterCapacity = 16u;
  settings.triangleVertexCapacity = 64u;
  settings.triangleIndexCapacity = 96u;
  return settings;
}

svr::SparseVoxelStreamedInstance instance(
    const svr::SparseVoxelPageProducer &source,
    const svr::SparseVoxelPageId root)
{
  svr::SparseVoxelStreamedInstance result{};
  result.instanceIndex = 5u;
  result.producer = source;
  result.rootPage = root;
  result.firstRootNode = 0u;
  result.rootNodeCount = 1u;
  for (uint32_t diagonal = 0u; diagonal < 4u; ++diagonal)
    result.modelToWorld[diagonal * 4u + diagonal] = 1.0f;
  return result;
}

svr::VirtualSparseVoxelFilePageRecord pageBounds()
{
  svr::VirtualSparseVoxelFilePageRecord result{};
  result.nodeCount = 1u;
  result.localBoundsMin = {-1.0f, -1.0f, 4.0f};
  result.localBoundsMax = {1.0f, 1.0f, 6.0f};
  return result;
}

svr::SparseVoxelPagePriorityView priorityView()
{
  svr::SparseVoxelPagePriorityView result{};
  for (uint32_t diagonal = 0u; diagonal < 4u; ++diagonal)
  {
    result.modelToWorld[diagonal * 4u + diagonal] = 1.0f;
    result.worldToCamera[diagonal * 4u + diagonal] = 1.0f;
  }
  result.focalPixels = {800.0f, 800.0f};
  result.cameraWorldPosition = {0.0f, 0.0f, 0.0f};
  result.nearPlane = 0.001f;
  return result;
}

svr::SparseVoxelProducedPagePayload brickTracePayload(
    const svr::SparseVoxelPageId page,
    const uint64_t expectedGeneration,
    const uint32_t producerRevision,
    const uint64_t revision)
{
  svr::SparseVoxelProducedPagePayload result{};
  result.page = page;
  result.expectedPageGeneration = expectedGeneration;
  result.requiredProducerRevision = producerRevision;
  result.payloadRevision = revision;
  result.geometryRevision = revision;
  result.materialRevision = revision;
  result.requirements.page = page;
  result.requirements.geometryRevision = revision;
  result.requirements.materialRevision = revision;
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

svr::SparseVoxelProducedPagePayload triangleOnlyPayload(
    const svr::SparseVoxelPageId page,
    const uint64_t expectedGeneration,
    const uint32_t producerRevision,
    const uint64_t revision)
{
  svr::SparseVoxelProducedPagePayload result{};
  result.page = page;
  result.expectedPageGeneration = expectedGeneration;
  result.requiredProducerRevision = producerRevision;
  result.payloadRevision = revision;
  result.geometryRevision = revision;
  result.materialRevision = revision;
  result.requirements.page = page;
  result.requirements.geometryRevision = revision;
  result.requirements.materialRevision = revision;
  result.requirements.triangleClusters.count = 2u;
  result.requirements.triangleVertices.count = 8u;
  result.requirements.triangleIndices.count = 12u;
  result.installRewriteCount = 0u;
  result.uninstallRewriteCount = 1u;
  result.payloadComplete = true;
  return result;
}

svr::SparseVoxelProducedPagePayload unifiedTriangleTracePayload(
    const svr::SparseVoxelPageId page,
    const uint64_t expectedGeneration,
    const uint32_t producerRevision,
    const uint64_t revision)
{
  auto result = triangleOnlyPayload(page, expectedGeneration, producerRevision, revision);
  result.requirements.traceFormat = svr::kVoxelTraceFormatCubic64;
  result.requirements.hierarchyNodes.count = 1u;
  result.requirements.traceNodes.count = 1u;
  result.requirements.traceErrorWords.count = 1u;
  result.installRewriteCount = 1u;
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

bool reserveCopyInstall(
    svr::SparseVoxelSharedPageTable &table,
    const svr::SparseVoxelProducedPagePayload &payload,
    const uint64_t copyFence,
    std::string &error)
{
  svr::SparseVoxelReservedPagePayload reservation{};
  if (!table.reserveProducedPagePayload(payload, reservation, &error))
    return false;
  if (!table.markPayloadCopied(reservation, copyFence, &error))
    return false;
  std::vector<svr::SparseVoxelPageInstallRewrite> rewrites;
  if (payload.installRewriteCount != 0u)
    rewrites.push_back(installRewrite(reservation));
  return table.installCopiedPagePayload(reservation, rewrites, &error);
}

void installRoot(
    svr::SparseVoxelSharedPageTable &table,
    const svr::SparseVoxelPageProducer &source,
    const svr::SparseVoxelPageId root,
    std::string &error)
{
  check(
      reserveCopyInstall(
          table,
          brickTracePayload(root, table.entry(root)->generation, source.producerRevision, 0x501u),
          1u,
          error),
      "root brick+trace fallback installs: " + error);
}

void testUnifiedCutRejectsTriangleOnlyChildUntilTraceArrives()
{
  std::cout << "unified cut rejects triangle-only child until trace arrives\n";
  const auto source = producer();
  const svr::SparseVoxelPageId root{source.producerId, 0u};
  const svr::SparseVoxelPageId child{source.producerId, 1u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure(tableSettings(), &error), "unified table configures: " + error);
  check(table.addPage(root, source, {}, true, &error), "unified root registers: " + error);
  check(table.addPage(child, source, {root}, false, &error), "unified child registers: " + error);
  installRoot(table, source, root, error);
  check(
      reserveCopyInstall(
          table,
          triangleOnlyPayload(child, table.entry(child)->generation, source.producerRevision, 0x601u),
          2u,
          error),
      "triangle-only child sidecar installs: " + error);

  svr::SparseVoxelUnifiedGeometryLODSelection selection{};
  svr::SparseVoxelUnifiedGeometryCutSettings settings{};
  check(
      svr::selectSparseVoxelUnifiedGeometryLOD(
          table,
          instance(source, root),
          child,
          settings,
          selection,
          &error),
      "unified selection runs while child trace is missing: " + error);
  check(selection.desiredRasterVisible && !selection.desiredTraceable, "child is raster-visible but not traceable");
  check(selection.selectedFallback && selection.selectedPage == root, "unified cut keeps the root selected instead of exposing raster-only child");
  check(selection.requestedDesiredPage && selection.missingTracePayload, "unified cut keeps requesting the child trace payload");

  svr::SparseVoxelUnifiedGeometryTraversalResult traversal{};
  svr::SparseVoxelTraversalCandidate candidate{};
  candidate.instance = instance(source, root);
  candidate.desiredPage = child;
  candidate.desiredPageBounds = pageBounds();
  candidate.visible = true;
  check(
      svr::traverseSparseVoxelUnifiedGeometryLOD(
          table,
          {candidate},
          priorityView(),
          settings,
          traversal,
          &error),
      "unified traversal runs with sidecar lag: " + error);
  check(traversal.pageRequests.size() == 1u && traversal.pageRequests[0].page == child, "resident-but-incomplete child is still requested for completion");
  check(traversal.counters.missingTracePageCount == 1u, "missing trace payload is counted");
  check(traversal.counters.brickFallbackPageCount == 1u, "brick fallback page is counted");
  check(traversal.counters.triangleRasterPageCount == 1u, "resident triangle sidecar page is counted");
}

void testUnifiedCutSelectsChildOnlyWhenRasterAndTraceMatch()
{
  std::cout << "unified cut selects child only when raster and trace match\n";
  const auto source = producer();
  const svr::SparseVoxelPageId root{source.producerId, 10u};
  const svr::SparseVoxelPageId child{source.producerId, 11u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure(tableSettings(), &error), "matching table configures: " + error);
  check(table.addPage(root, source, {}, true, &error), "matching root registers: " + error);
  check(table.addPage(child, source, {root}, false, &error), "matching child registers: " + error);
  installRoot(table, source, root, error);
  check(
      reserveCopyInstall(
          table,
          unifiedTriangleTracePayload(child, table.entry(child)->generation, source.producerRevision, 0x701u),
          2u,
          error),
      "child triangle+trace page installs atomically: " + error);

  svr::SparseVoxelUnifiedGeometryLODSelection selection{};
  svr::SparseVoxelUnifiedGeometryCutSettings settings{};
  check(
      svr::selectSparseVoxelUnifiedGeometryLOD(
          table,
          instance(source, root),
          child,
          settings,
          selection,
          &error),
      "matching unified selection runs: " + error);
  check(!selection.selectedFallback && selection.selectedPage == child, "child wins when both raster and trace views are present");
  check(selection.selectedRasterVisible && selection.selectedTraceable, "selected child exposes both raster and trace");

  svr::SparseVoxelTraceRequestQueue queue{};
  queue.configure(4u);
  const auto *entry = table.entry(child);
  const auto receiver = svr::makeSparseVoxelTraceReceiverGPU(child, entry != nullptr ? entry->generation : 0u, 0u, false);
  svr::SparseVoxelTraceFeedbackInput feedback{};
  feedback.instance = instance(source, root);
  feedback.desiredPage = child;
  feedback.receiverPage = child;
  feedback.receiverGeneration = entry != nullptr ? entry->generation : 0u;
  feedback.rayClass = svr::SparseVoxelTraceRayClass::Shadow;
  feedback.requireDirectTraceView = true;
  svr::SparseVoxelTraceCutDiagnostics diagnostics{};
  check(
      svr::evaluateSparseVoxelTraceCutConsistency(
          table,
          receiver,
          feedback,
          queue,
          diagnostics,
          &error),
      "trace receiver uses the same selected page generation: " + error);
  check(diagnostics.traceSelectedPage == child && !diagnostics.traceRequestEmitted, "trace cut matches the screen-selected child page");
}

void testOffscreenTraceRequestsDoNotDependOnScreenVisibility()
{
  std::cout << "off-screen trace requests can request missing unified pages\n";
  const auto source = producer();
  const svr::SparseVoxelPageId root{source.producerId, 20u};
  const svr::SparseVoxelPageId child{source.producerId, 21u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure(tableSettings(), &error), "offscreen table configures: " + error);
  check(table.addPage(root, source, {}, true, &error), "offscreen root registers: " + error);
  check(table.addPage(child, source, {root}, false, &error), "offscreen child registers: " + error);
  installRoot(table, source, root, error);

  svr::SparseVoxelTraceRequestQueue queue{};
  queue.configure(4u);
  svr::SparseVoxelTraceFeedbackInput feedback{};
  feedback.instance = instance(source, root);
  feedback.desiredPage = child;
  feedback.receiverPage = root;
  feedback.receiverGeneration = table.entry(root)->generation;
  feedback.rayClass = svr::SparseVoxelTraceRayClass::Shadow;
  feedback.mode = svr::SparseVoxelTraceLODMode::Exact;
  feedback.requireDirectTraceView = true;
  feedback.offscreen = true;
  feedback.priority = 77u;
  svr::SparseVoxelTraceFeedbackResult feedbackResult{};
  check(
      svr::processSparseVoxelTraceFeedback(
          table,
          feedback,
          queue,
          feedbackResult,
          &error),
      "offscreen trace feedback processes: " + error);
  check(feedbackResult.emittedRequest && feedbackResult.unresolvedExact, "offscreen exact trace emits an unresolved request");
  check(queue.size() == 1u && svr::sparseVoxelTraceRequestPageId(queue.requests()[0]) == child, "offscreen request targets the missing child page");
  check((queue.requests()[0].flags & svr::kSparseVoxelTraceRequestOffscreen) != 0u, "offscreen request carries the offscreen flag");

  svr::SparseVoxelUnifiedGeometryTraversalResult traversal{};
  svr::SparseVoxelTraversalCandidate candidate{};
  candidate.instance = instance(source, root);
  candidate.desiredPage = child;
  candidate.desiredPageBounds = pageBounds();
  candidate.visible = false;
  svr::SparseVoxelUnifiedGeometryCutSettings settings{};
  check(
      svr::traverseSparseVoxelUnifiedGeometryLOD(
          table,
          {candidate},
          priorityView(),
          settings,
          traversal,
          &error),
      "offscreen unified traversal requests the same page identity: " + error);
  check(traversal.pageRequests.size() == 1u && traversal.pageRequests[0].page == child && !traversal.pageRequests[0].visible, "offscreen unified request is independent from screen visibility");
  check(traversal.counters.offscreenRequestCount == 1u, "offscreen request counter is tracked");
}

void testConsumerProtectedEvictionWaitsForRasterAndTrace()
{
  std::cout << "consumer-protected eviction waits for raster and trace users\n";
  const auto source = producer();
  const svr::SparseVoxelPageId root{source.producerId, 30u};
  const svr::SparseVoxelPageId child{source.producerId, 31u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure(tableSettings(), &error), "eviction table configures: " + error);
  check(table.addPage(root, source, {}, true, &error), "eviction root registers: " + error);
  check(table.addPage(child, source, {root}, false, &error), "eviction child registers: " + error);
  installRoot(table, source, root, error);
  check(
      reserveCopyInstall(
          table,
          unifiedTriangleTracePayload(child, table.entry(child)->generation, source.producerRevision, 0x801u),
          2u,
          error),
      "eviction child triangle+trace page installs: " + error);

  const auto *childEntry = table.entry(child);
  const std::vector<svr::SparseVoxelPageConsumerReference> liveConsumers{
      {child, childEntry != nullptr ? childEntry->generation : 0u, svr::SparseVoxelPageConsumerKind::Raster},
      {child, childEntry != nullptr ? childEntry->generation : 0u, svr::SparseVoxelPageConsumerKind::Trace},
  };
  svr::SparseVoxelPageConsumerReferenceSummary summary{};
  check(
      !svr::canEvictSparseVoxelPageWithConsumers(table, child, liveConsumers, &summary, &error),
      "live raster/trace consumers block child eviction");
  check(summary.rasterConsumerCount == 1u && summary.traceConsumerCount == 1u, "both consumer classes are counted");
  check(
      svr::canEvictSparseVoxelPageWithConsumers(table, child, {}, &summary, &error),
      "child can evict once transient consumers drain: " + error);
  check(summary.evictable && summary.baseEvictable, "consumer-free child is evictable by base residency rules");
  check(
      !svr::canEvictSparseVoxelPageWithConsumers(table, root, {}, &summary, &error),
      "root fallback remains protected by the resident child dependency");

  const std::vector<svr::SparseVoxelPageConsumerReference> staleConsumer{
      {child, (childEntry != nullptr ? childEntry->generation : 0u) + 100u, svr::SparseVoxelPageConsumerKind::Raster},
  };
  check(
      svr::canEvictSparseVoxelPageWithConsumers(table, child, staleConsumer, &summary, &error),
      "stale generation consumers do not block retirement: " + error);
  check(summary.staleConsumerCount == 1u && summary.rasterConsumerCount == 0u, "stale consumers are diagnosed separately");
}

void testParentFallbackIsProtectedWhileChildUploadIsInFlight()
{
  std::cout << "parent fallback is protected while child sidecar upload is in flight\n";
  const auto source = producer();
  const svr::SparseVoxelPageId root{source.producerId, 40u};
  const svr::SparseVoxelPageId child{source.producerId, 41u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure(tableSettings(), &error), "upload table configures: " + error);
  check(table.addPage(root, source, {}, true, &error), "upload root registers: " + error);
  check(table.addPage(child, source, {root}, false, &error), "upload child registers: " + error);
  installRoot(table, source, root, error);

  svr::SparseVoxelReservedPagePayload reservation{};
  check(
      table.reserveProducedPagePayload(
          triangleOnlyPayload(child, table.entry(child)->generation, source.producerRevision, 0x901u),
          reservation,
          &error),
      "child sidecar reservation enters upload-queued state: " + error);
  svr::SparseVoxelPageEvictionDecision decision{};
  check(!table.canEvictPage(root, &decision, &error), "root cannot evict while child upload is live");
  check(decision.liveDependentCount == 1u, "upload-queued child counts as a live dependent");
}

} // namespace

int main()
{
  testUnifiedCutRejectsTriangleOnlyChildUntilTraceArrives();
  testUnifiedCutSelectsChildOnlyWhenRasterAndTraceMatch();
  testOffscreenTraceRequestsDoNotDependOnScreenVisibility();
  testConsumerProtectedEvictionWaitsForRasterAndTrace();
  testParentFallbackIsProtectedWhileChildUploadIsInFlight();
  if (failures != 0)
  {
    std::cerr << failures << " triangle phase5 checks failed\n";
    return 1;
  }
  std::cout << "Sparse voxel terrain triangle Phase 5 streaming/dependency tests passed\n";
  return 0;
}
