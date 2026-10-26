#include "voxel/svraster/SparseVoxelStreaming.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>

namespace svr = voxel::svraster;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

uint64_t mix(const uint64_t value, const uint64_t data)
{
  uint64_t hash = value ^ (data + 0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u));
  hash ^= hash >> 33u;
  hash *= 0xff51afd7ed558ccdull;
  hash ^= hash >> 33u;
  return hash;
}

svr::SparseVoxelPageProducer producer(const uint64_t id, const svr::SparseVoxelPageProducerKind kind, const uint32_t revision, const char *name)
{
  svr::SparseVoxelPageProducer result{};
  result.producerId = id;
  result.kind = kind;
  result.producerRevision = revision;
  result.producesOnCPU = kind == svr::SparseVoxelPageProducerKind::PrebuiltSparseVoxel || kind == svr::SparseVoxelPageProducerKind::CPUGenerated;
  result.producesOnGPU = kind == svr::SparseVoxelPageProducerKind::GPUGeneratedObject || kind == svr::SparseVoxelPageProducerKind::GPUGeneratedTerrain;
  result.debugName = name;
  return result;
}

svr::SparseVoxelStreamedInstance instance(const uint32_t index, const svr::SparseVoxelPageProducer &source, const svr::SparseVoxelPageId rootPage, const uint32_t firstRootNode, const uint32_t pageCount)
{
  svr::SparseVoxelStreamedInstance result{};
  result.instanceIndex = index;
  result.producer = source;
  result.rootPage = rootPage;
  result.firstRootNode = firstRootNode;
  result.rootNodeCount = 1u;
  result.firstPage = 0u;
  result.pageCount = pageCount;
  for (uint32_t diagonal = 0u; diagonal < 4u; ++diagonal)
    result.modelToWorld[diagonal * 4u + diagonal] = 1.0f;
  return result;
}

svr::VirtualSparseVoxelPageRewrite virtualRewrite()
{
  svr::VirtualSparseVoxelPageRewrite rewrite{};
  rewrite.nodeIndex = 0u;
  rewrite.firstChild = 0u;
  rewrite.childCount = 2u;
  rewrite.flags = 1u;
  return rewrite;
}

void fillTinyPageData(svr::VirtualSparseVoxelPageData &pageData)
{
  pageData.pageIndex = 0u;
  pageData.modelIndex = 0u;
  pageData.firstNode = 0u;
  pageData.nodes.resize(2u);
  pageData.pathsLevels.resize(2u);
  pageData.centersLengths.resize(2u);
  pageData.materials.resize(2u);

  pageData.nodes[0].firstChild = 0u;
  pageData.nodes[0].childCount = 1u;
  pageData.nodes[0].voxelIndex = 0u;
  pageData.nodes[0].level = 0u;
  pageData.nodes[1].firstChild = 0u;
  pageData.nodes[1].childCount = 0u;
  pageData.nodes[1].voxelIndex = 1u;
  pageData.nodes[1].level = 1u;

  pageData.pathsLevels[0].level = 0u;
  pageData.pathsLevels[1].pathLo = 7u;
  pageData.pathsLevels[1].level = 1u;

  pageData.centersLengths[0].center = {0.0f, 0.0f, 0.0f};
  pageData.centersLengths[0].length = 2.0f;
  pageData.centersLengths[1].center = {0.5f, 0.5f, 0.5f};
  pageData.centersLengths[1].length = 1.0f;

  pageData.materials[0].albedoRGBA8 = 0xff336699u;
  pageData.materials[0].materialId = 11u;
  pageData.materials[1].albedoRGBA8 = 0xff99cc33u;
  pageData.materials[1].materialId = 12u;
}

svr::SparseVoxelPrebuiltPagePackage prebuiltPackage(const svr::SparseVoxelPageId page, const uint64_t expectedGeneration, const uint32_t producerRevision, const uint64_t payloadRevision)
{
  svr::SparseVoxelPrebuiltPagePackage package{};
  package.page = page;
  package.expectedPageGeneration = expectedGeneration;
  package.requiredProducerRevision = producerRevision;
  package.payloadRevision = payloadRevision;
  fillTinyPageData(package.pageData);
  package.installRewrites = {virtualRewrite()};
  package.uninstallRewrites = {virtualRewrite()};
  package.childIndexCount = 2u;
  package.brickOccupancyCount = 2u;
  package.materialCellCount = 2u;
  package.tree64NodeCount = 1u;
  package.tree64MaterialCount = 1u;
  package.decoded = true;
  return package;
}

svr::SparseVoxelProducedPagePayload syntheticPayload(const svr::SparseVoxelPageId page, const uint64_t expectedGeneration, const uint32_t producerRevision, const uint64_t payloadRevision)
{
  svr::SparseVoxelProducedPagePayload payload{};
  payload.page = page;
  payload.expectedPageGeneration = expectedGeneration;
  payload.requiredProducerRevision = producerRevision;
  payload.payloadRevision = payloadRevision;
  payload.requirements.page = page;
  payload.requirements.hierarchyNodes.count = 2u;
  payload.requirements.childIndices.count = 2u;
  payload.requirements.brickNodes.count = 2u;
  payload.requirements.brickOccupancy.count = 2u;
  payload.requirements.materials.count = 2u;
  payload.requirements.materialCells.count = 2u;
  payload.requirements.tree64Nodes.count = 1u;
  payload.requirements.tree64Materials.count = 1u;
  payload.installRewriteCount = 1u;
  payload.uninstallRewriteCount = 1u;
  payload.payloadComplete = true;
  return payload;
}

svr::SparseVoxelPageInstallRewrite installRewrite(const svr::SparseVoxelReservedPagePayload &reservation)
{
  svr::SparseVoxelPageInstallRewrite rewrite{};
  rewrite.page = reservation.produced.page;
  rewrite.allocation = reservation.allocation;
  rewrite.requiredProducerRevision = reservation.produced.requiredProducerRevision;
  rewrite.rewrite.nodeIndex = reservation.allocation.hierarchyNodes.first;
  rewrite.rewrite.firstChild = reservation.allocation.childIndices.first;
  rewrite.rewrite.childCount = reservation.allocation.childIndices.count;
  rewrite.rewrite.flags = 1u;
  return rewrite;
}

struct RenderOracle
{
  uint64_t logicalPayload = 0u;
  uint32_t hierarchyNodeCount = 0u;
  uint32_t childIndexCount = 0u;
  uint32_t brickNodeCount = 0u;
  uint32_t materialCount = 0u;
  uint32_t tree64NodeCount = 0u;
  bool resident = false;
};

RenderOracle oracleFor(const svr::SparseVoxelSharedPageTable &table, const svr::SparseVoxelPageId page, const uint64_t logicalSignature)
{
  RenderOracle oracle{};
  oracle.logicalPayload = logicalSignature;
  const auto *entry = table.entry(page);
  if (entry == nullptr)
    return oracle;
  oracle.resident = entry->resident();
  oracle.hierarchyNodeCount = entry->allocation.hierarchyNodes.count;
  oracle.childIndexCount = entry->allocation.childIndices.count;
  oracle.brickNodeCount = entry->allocation.brickNodes.count;
  oracle.materialCount = entry->allocation.materials.count;
  oracle.tree64NodeCount = entry->allocation.tree64Nodes.count;
  return oracle;
}

bool equivalentTraversalAndRaster(const RenderOracle &a, const RenderOracle &b)
{
  return a.logicalPayload == b.logicalPayload && a.hierarchyNodeCount == b.hierarchyNodeCount && a.childIndexCount == b.childIndexCount && a.brickNodeCount == b.brickNodeCount && a.materialCount == b.materialCount &&
         a.tree64NodeCount == b.tree64NodeCount && a.resident && b.resident;
}

uint64_t tinyPageSignature()
{
  uint64_t hash = 0x504841534534ull;
  hash = mix(hash, 2u);
  hash = mix(hash, 2u);
  hash = mix(hash, 2u);
  hash = mix(hash, 1u);
  hash = mix(hash, 0xff336699u);
  hash = mix(hash, 0xff99cc33u);
  return hash;
}

void testPrebuiltAndSyntheticUseSameInstallPath()
{
  std::cout << "prebuilt CPU package and synthetic producer converge through copied install\n";

  svr::SparseVoxelSharedPageTable table{};
  std::string error;
  check(table.configure({8u, 8u, 8u, 8u, 8u, 8u, 4u, 4u}, &error), "phase4 shared table configures: " + error);

  const auto prebuilt = producer(0x4100u, svr::SparseVoxelPageProducerKind::PrebuiltSparseVoxel, 5u, "phase4-prebuilt-object");
  const auto generated = producer(0x4200u, svr::SparseVoxelPageProducerKind::CPUGenerated, 5u, "phase4-synthetic-object");
  const svr::SparseVoxelPageId prebuiltRoot{prebuilt.producerId, 0u};
  const svr::SparseVoxelPageId syntheticRoot{generated.producerId, 0u};
  const auto prebuiltInstance = instance(0u, prebuilt, prebuiltRoot, 0u, 1u);
  const auto syntheticInstance = instance(1u, generated, syntheticRoot, 64u, 1u);

  check(prebuiltInstance.valid(), "prebuilt object owns an independent streamed-instance root");
  check(syntheticInstance.valid(), "synthetic object owns an independent streamed-instance root");
  check(!(prebuiltInstance.rootPage == syntheticInstance.rootPage), "instances do not share a global terrain cut/root page");

  check(table.addPage(prebuiltRoot, prebuilt, {}, false, &error), "prebuilt root page registers independently: " + error);
  check(table.addPage(syntheticRoot, generated, {}, false, &error), "synthetic root page registers independently: " + error);

  svr::SparseVoxelProducedPagePayload prebuiltPayload{};
  const auto package = prebuiltPackage(prebuiltRoot, table.entry(prebuiltRoot)->generation, prebuilt.producerRevision, 7u);
  check(svr::sparseVoxelProducedPagePayloadFromPrebuilt(package, prebuiltPayload, &error), "decoded CPU package becomes neutral produced payload: " + error);
  check(prebuiltPayload.valid(), "prebuilt produced payload passes shared metadata validation");
  check(prebuiltPayload.installRewriteCount == package.installRewrites.size(), "prebuilt install rewrite count is preserved for the install transaction");

  svr::SparseVoxelReservedPagePayload prebuiltReservation{};
  check(table.reserveProducedPagePayload(prebuiltPayload, prebuiltReservation, &error), "prebuilt payload reserves shared physical buffers: " + error);
  check(table.entry(prebuiltRoot)->state == svr::SparseVoxelPageState::UploadQueued, "prebuilt reservation is upload-queued, not resident");
  check(table.entry(syntheticRoot)->state == svr::SparseVoxelPageState::Unloaded, "synthetic instance is still unloaded while prebuilt streams");
  check(table.markPayloadCopied(prebuiltReservation, 4u, &error), "prebuilt decoded bytes copy into shared buffers: " + error);
  check(table.installCopiedPagePayload(prebuiltReservation, {installRewrite(prebuiltReservation)}, &error), "prebuilt page installs through the shared rewrite path: " + error);

  auto generatedPayload = syntheticPayload(syntheticRoot, table.entry(syntheticRoot)->generation, generated.producerRevision, 7u);
  svr::SparseVoxelReservedPagePayload generatedReservation{};
  check(table.reserveProducedPagePayload(generatedPayload, generatedReservation, &error), "synthetic generated payload reserves shared physical buffers: " + error);
  check(table.entry(prebuiltRoot)->resident(), "prebuilt instance remains resident while synthetic streams");
  check(table.markPayloadCopied(generatedReservation, 5u, &error), "synthetic generated bytes copy into shared buffers: " + error);
  check(table.installCopiedPagePayload(generatedReservation, {installRewrite(generatedReservation)}, &error), "synthetic generated page installs through the same rewrite path: " + error);

  const auto signature = tinyPageSignature();
  const auto prebuiltOracle = oracleFor(table, prebuiltRoot, signature);
  const auto generatedOracle = oracleFor(table, syntheticRoot, signature);
  check(equivalentTraversalAndRaster(prebuiltOracle, generatedOracle), "CPU-loaded and equivalent generated page expose identical traversal/raster oracle output");
  check(table.installRewrites().size() == 2u, "both source types publish only through accepted install rewrite commands");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::HierarchyNodes).active == 4u, "independent instances occupy separate hierarchy ranges");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::Tree64Nodes).active == 2u, "Tree64 fixture/sidecar ranges stream per instance");
}

void testBadPrebuiltPackagesRejectedBeforeAllocation()
{
  std::cout << "bad prebuilt packages are rejected before physical allocation\n";

  svr::SparseVoxelSharedPageTable table{};
  std::string error;
  check(table.configure({4u, 4u, 4u, 4u, 4u, 4u, 2u, 2u}, &error), "validation table configures: " + error);

  const auto prebuilt = producer(0x4300u, svr::SparseVoxelPageProducerKind::PrebuiltSparseVoxel, 3u, "phase4-bad-prebuilt");
  const svr::SparseVoxelPageId root{prebuilt.producerId, 0u};
  check(table.addPage(root, prebuilt, {}, false, &error), "validation page registers: " + error);

  auto undecoded = prebuiltPackage(root, table.entry(root)->generation, prebuilt.producerRevision, 1u);
  undecoded.decoded = false;
  svr::SparseVoxelProducedPagePayload payload{};
  check(!svr::sparseVoxelProducedPagePayloadFromPrebuilt(undecoded, payload, &error), "undecoded prebuilt package is rejected");

  auto mismatched = prebuiltPackage(root, table.entry(root)->generation, prebuilt.producerRevision, 2u);
  mismatched.pageData.materials.pop_back();
  check(!svr::sparseVoxelProducedPagePayloadFromPrebuilt(mismatched, payload, &error), "prebuilt package with mismatched page arrays is rejected");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::HierarchyNodes).active == 0u, "invalid packages reserve no hierarchy storage");
  check(table.entry(root)->state == svr::SparseVoxelPageState::Unloaded, "invalid packages leave page state unloaded");
}

bool hasQueuedPage(const svr::SparseVoxelTraceRequestQueue &queue, const svr::SparseVoxelPageId page)
{
  return std::any_of(
      queue.requests().begin(),
      queue.requests().end(),
      [page](const svr::SparseVoxelTracePageRequestGPU &request)
      {
        return svr::sparseVoxelTraceRequestPageId(request) == page;
      });
}

void installSyntheticResidentPage(
    svr::SparseVoxelSharedPageTable &table,
    const svr::SparseVoxelPageProducer &source,
    const svr::SparseVoxelPageId page,
    std::string &error)
{
  const auto payload = syntheticPayload(page, table.entry(page)->generation, source.producerRevision, page.localPage + 10u);
  svr::SparseVoxelReservedPagePayload reservation{};
  check(table.reserveProducedPagePayload(payload, reservation, &error), "trace feedback fixture reserves root fallback page: " + error);
  check(table.markPayloadCopied(reservation, 6u, &error), "trace feedback fixture marks root fallback copy complete: " + error);
  check(table.installCopiedPagePayload(reservation, {installRewrite(reservation)}, &error), "trace feedback fixture installs root fallback page: " + error);
}

void testTraceRequestQueueDeduplicatesAndPreservesPriority()
{
  std::cout << "trace request queue deduplicates and preserves high-priority feedback\n";

  svr::SparseVoxelTraceRequestQueue queue{};
  queue.configure(2u);
  std::string error;

  const svr::SparseVoxelPageId pageA{0x5100u, 1u};
  const svr::SparseVoxelPageId pageB{0x5100u, 2u};
  const svr::SparseVoxelPageId pageC{0x5100u, 3u};
  const svr::SparseVoxelPageId receiver{0x5100u, 0u};

  check(
      queue.emit(
          svr::makeSparseVoxelTracePageRequestGPU(
              pageA,
              3u,
              svr::SparseVoxelTraceRayClass::Shadow,
              receiver,
              9u,
              svr::kSparseVoxelTraceRequestOffscreen | svr::kSparseVoxelTraceRequestConservativeFallback,
              10u,
              0.5f,
              0.1f,
              1.0f,
              100.0f),
          &error) == svr::SparseVoxelTraceRequestQueueStatus::Accepted,
      "first offscreen shadow request is accepted: " + error);
  check(
      queue.emit(
          svr::makeSparseVoxelTracePageRequestGPU(
              pageA,
              3u,
              svr::SparseVoxelTraceRayClass::Shadow,
              receiver,
              9u,
              svr::kSparseVoxelTraceRequestVisibleReceiver,
              30u,
              0.25f,
              0.2f,
              0.5f,
              120.0f),
          &error) == svr::SparseVoxelTraceRequestQueueStatus::Deduplicated,
      "duplicate shadow request coalesces by producer/page/lod/ray-class: " + error);
  check(queue.size() == 1u, "deduplicated request does not consume another queue slot");
  check(queue.requests().front().priority == 30u, "deduplicated request keeps the highest priority");
  check((queue.requests().front().flags & svr::kSparseVoxelTraceRequestOffscreen) != 0u, "deduplicated request preserves offscreen diagnostics");
  check((queue.requests().front().flags & svr::kSparseVoxelTraceRequestVisibleReceiver) != 0u, "deduplicated request merges visible receiver diagnostics");
  check(svr::sparseVoxelTraceRequestReceiverGeneration(queue.requests().front()) == 9u, "receiver generation survives request packing");

  check(
      queue.emit(
          svr::makeSparseVoxelTracePageRequestGPU(pageB, 2u, svr::SparseVoxelTraceRayClass::GlobalIllumination, receiver, 9u, svr::kSparseVoxelTraceRequestOffscreen, 4u, 1.0f, 0.4f, 0.0f, 50.0f),
          &error) == svr::SparseVoxelTraceRequestQueueStatus::Accepted,
      "second trace request fills remaining queue capacity: " + error);
  check(
      queue.emit(
          svr::makeSparseVoxelTracePageRequestGPU(
              pageC,
              0u,
              svr::SparseVoxelTraceRayClass::RasterFeedback,
              receiver,
              9u,
              svr::kSparseVoxelTraceRequestRasterFeedback | svr::kSparseVoxelTraceRequestVisibleReceiver,
              svr::quantizeSparseVoxelPagePriority(true, 64.0f, 1.0f),
              0.0f,
              0.0f,
              0.0f,
              0.0f),
          &error) == svr::SparseVoxelTraceRequestQueueStatus::ReplacedLowerPriority,
      "visible raster feedback replaces lower-priority trace feedback when bounded queue is full: " + error);
  check(queue.size() == 2u, "priority replacement keeps queue bounded");
  check(hasQueuedPage(queue, pageA), "higher-priority deduplicated shadow request remains queued");
  check(!hasQueuedPage(queue, pageB), "lowest-priority trace request is the one evicted under pressure");
  check(hasQueuedPage(queue, pageC), "visible raster request is retained under trace pressure");

  const auto sorted = queue.sortedRequests();
  check(sorted.size() == 2u && svr::sparseVoxelTraceRequestPageId(sorted.front()) == pageC, "sorted queue submits highest-priority visible request first");
  const auto stats = queue.stats();
  check(stats.emitted == 4u && stats.accepted == 2u && stats.deduplicated == 1u && stats.overflowed == 1u, "trace queue reports accepted, deduped and overflowed request counts");
  check(stats.traceRequests == 3u && stats.rasterRequests == 1u && stats.offscreenRequests == 2u && stats.fallbackTraceCount == 1u, "trace queue class diagnostics count offscreen/fallback/raster feedback");
}

void testTraceFeedbackRequestsMissingPagesWithoutFalseMisses()
{
  std::cout << "trace feedback requests missing pages without false misses\n";

  svr::SparseVoxelSharedPageTable table{};
  std::string error;
  check(table.configure({12u, 12u, 12u, 12u, 12u, 12u, 4u, 4u}, &error), "trace feedback table configures: " + error);

  const auto source = producer(0x5200u, svr::SparseVoxelPageProducerKind::CPUGenerated, 7u, "phase4-trace-feedback");
  const svr::SparseVoxelPageId root{source.producerId, 0u};
  const svr::SparseVoxelPageId fine{source.producerId, 1u};
  check(table.addPage(root, source, {}, true, &error), "trace feedback root registers pinned fallback: " + error);
  check(table.addPage(fine, source, {root}, false, &error), "trace feedback fine page registers behind root: " + error);
  installSyntheticResidentPage(table, source, root, error);

  const auto streamed = instance(0u, source, root, 0u, 2u);
  svr::SparseVoxelTraceRequestQueue queue{};
  queue.configure(4u);

  svr::SparseVoxelTraceFeedbackInput shadow{};
  shadow.instance = streamed;
  shadow.desiredPage = fine;
  shadow.receiverPage = root;
  shadow.receiverGeneration = table.entry(root)->generation;
  shadow.desiredLOD = 1u;
  shadow.rayClass = svr::SparseVoxelTraceRayClass::Shadow;
  shadow.mode = svr::SparseVoxelTraceLODMode::ConservativeVisibility;
  shadow.priority = 123u;
  shadow.desiredError = 0.25f;
  shadow.rayConeWidth = 0.05f;
  shadow.minimumT = 0.0f;
  shadow.maximumT = 100.0f;
  shadow.offscreen = true;

  svr::SparseVoxelTraceFeedbackResult result{};
  check(svr::processSparseVoxelTraceFeedback(table, shadow, queue, result, &error), "offscreen shadow trace feedback processes: " + error);
  check(result.selectedConservativeFallback && result.selectedPage == root, "offscreen missing shadow page traces conservative resident parent");
  check(result.emittedRequest && queue.size() == 1u, "offscreen missing shadow page emits one refinement request");
  check((queue.requests().front().flags & svr::kSparseVoxelTraceRequestOffscreen) != 0u, "trace request records offscreen source");
  check((queue.requests().front().flags & svr::kSparseVoxelTraceRequestConservativeFallback) != 0u, "trace request records conservative fallback use");

  check(svr::processSparseVoxelTraceFeedback(table, shadow, queue, result, &error), "repeated offscreen shadow feedback processes: " + error);
  check(queue.size() == 1u && queue.stats().deduplicated == 1u, "repeated ray to same missing page deduplicates");

  auto exact = shadow;
  exact.rayClass = svr::SparseVoxelTraceRayClass::Reflection;
  exact.mode = svr::SparseVoxelTraceLODMode::Exact;
  exact.allowCoarseExactFallback = false;
  check(svr::processSparseVoxelTraceFeedback(table, exact, queue, result, &error), "exact trace feedback processes missing page: " + error);
  check(result.unresolvedExact && !result.selectedPage.valid(), "exact missing trace reports unresolved instead of false empty miss");
  check(queue.size() == 2u, "exact unresolved request keeps a separate ray-class queue entry");
  const auto stats = queue.stats();
  check(stats.fallbackTraceCount == 2u && stats.unresolvedExactCount == 1u, "feedback diagnostics count conservative fallback rays and exact unresolved rays");

  svr::SparseVoxelTraceRequestQueue saturated{};
  saturated.configure(0u);
  check(svr::processSparseVoxelTraceFeedback(table, shadow, saturated, result, &error), "overflowed request buffer still processes visibility ray: " + error);
  check(result.selectedConservativeFallback && result.selectedPage == root, "overflow does not turn a visibility ray into a miss");
  check(result.requestOverflowed && saturated.overflowed(), "bounded request buffer reports overflow");
}

} // namespace

int main()
{
  std::cout << "Terrain / virtual geometry convergence Phase 4 trace request feedback\n";
  testPrebuiltAndSyntheticUseSameInstallPath();
  testBadPrebuiltPackagesRejectedBeforeAllocation();
  testTraceRequestQueueDeduplicatesAndPreservesPriority();
  testTraceFeedbackRequestsMissingPagesWithoutFalseMisses();

  if (failures != 0)
  {
    std::cout << failures << " checks failed\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
