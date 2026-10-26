#include "voxel/svraster/SparseVoxelStreaming.hpp"
#include "voxel/svraster/SparseVoxelTerrainTree64.hpp"
#include "voxel/svraster/SparseVoxelTree64Streaming.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace svr = voxel::svraster;
using namespace std::chrono_literals;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

svr::SparseVoxelTerrainTree64LODVariant variant(
    const uint32_t logicalPage,
    const uint32_t internalLOD,
    const uint64_t geometryRevision,
    const float maximumError,
    const svr::SparseVoxelTerrainTree64LODVariantState state,
    const bool conservative = true)
{
  svr::SparseVoxelTerrainTree64LODVariant result{};
  result.logicalPageId = logicalPage;
  result.internalLOD = internalLOD;
  result.geometryRevision = geometryRevision;
  result.maximumProjectedErrorPixels = maximumError;
  result.requestedCellsPerEdge = internalLOD == 0u ? 64u : (internalLOD == 1u ? 16u : 4u);
  result.payloadBytes = 4096u >> std::min<uint32_t>(internalLOD * 2u, 12u);
  result.state = state;
  result.conservative = conservative;
  return result;
}

svr::SparseVoxelTree64PageRequest request(
    const uint64_t logicalPage,
    const uint32_t terrainSlot,
    const uint32_t terrainGeneration,
    const uint32_t internalLOD,
    const uint64_t geometryRevision,
    const float priority)
{
  svr::SparseVoxelTree64PageRequest result{};
  result.terrainSlot = terrainSlot;
  result.terrainGeneration = terrainGeneration;
  result.internalLOD = internalLOD;
  result.geometryRevision = geometryRevision;
  result.priority = priority;
  result.logicalPageId = logicalPage;
  return result;
}

svr::SparseVoxelTree64PageScheduler::BuildFunction buildFor(
    const svr::SparseVoxelTree64PageRequest pageRequest,
    const std::function<void(const std::atomic_bool &)> &before = {})
{
  return [pageRequest, before](
             svr::SparseVoxelTerrainTree64PagePayload &payload,
             const std::atomic_bool &cancelled,
             std::string *error)
  {
    if (before)
      before(cancelled);
    if (cancelled.load(std::memory_order_relaxed))
    {
      if (error != nullptr)
        *error = "cancelled";
      return false;
    }
    payload = {};
    payload.terrainSlot = pageRequest.terrainSlot;
    payload.terrainGeneration = pageRequest.terrainGeneration;
    payload.internalLOD = pageRequest.internalLOD;
    payload.geometryRevision = pageRequest.geometryRevision;
    payload.model.prebuiltNodes.resize(1u);
    return true;
  };
}

bool waitUntil(const std::function<bool()> &predicate)
{
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  do
  {
    if (predicate())
      return true;
    std::this_thread::sleep_for(1ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return predicate();
}

svr::SparseVoxelTree64PageSchedulerSettings schedulerSettings()
{
  svr::SparseVoxelTree64PageSchedulerSettings settings{};
  settings.workerCount = 1u;
  settings.maximumJobs = 4u;
  settings.maximumQueuedRequests = 4u;
  settings.maximumGeneratedPages = 4u;
  settings.maximumPublicationPages = 4u;
  settings.maximumPendingUploadBytes = 1u << 20u;
  settings.maximumPendingCPUBytes = 1u << 20u;
  settings.physicalPageCapacity = 6u;
  settings.retirementHeadroomPages = 1u;
  return settings;
}

svr::SparseVoxelPageProducer descriptorProducer(const uint64_t id = 0x504841534531300ull)
{
  svr::SparseVoxelPageProducer result{};
  result.producerId = id;
  result.kind = svr::SparseVoxelPageProducerKind::CPUGenerated;
  result.producerRevision = 10u;
  result.producesOnCPU = true;
  result.debugName = "phase10-descriptor";
  return result;
}

svr::SparseVoxelProducedPagePayload descriptorPayload(
    const svr::SparseVoxelPageId page,
    const uint64_t expectedGeneration,
    const uint32_t producerRevision,
    const uint64_t payloadRevision,
    const uint64_t geometryRevision,
    const uint64_t materialRevision,
    const uint32_t hierarchyNodes,
    const uint32_t childIndices,
    const uint32_t brickNodes,
    const uint32_t materials,
    const uint32_t traceNodes,
    const uint32_t traceErrorWords,
    const uint32_t cullErrorWords,
    const uint32_t traceFormat = svr::kVoxelTraceFormatCubic64)
{
  svr::SparseVoxelProducedPagePayload result{};
  result.page = page;
  result.expectedPageGeneration = expectedGeneration;
  result.requiredProducerRevision = producerRevision;
  result.payloadRevision = payloadRevision;
  result.geometryRevision = geometryRevision;
  result.materialRevision = materialRevision;
  result.requirements.page = page;
  result.requirements.geometryRevision = geometryRevision;
  result.requirements.materialRevision = materialRevision;
  result.requirements.traceFormat = traceNodes != 0u ? traceFormat : svr::kVoxelTraceFormatInvalid;
  result.requirements.hierarchyNodes.count = hierarchyNodes;
  result.requirements.childIndices.count = childIndices;
  result.requirements.brickNodes.count = brickNodes;
  result.requirements.brickOccupancy.count = brickNodes;
  result.requirements.materials.count = materials;
  result.requirements.materialCells.count = materials;
  result.requirements.traceNodes.count = traceNodes;
  result.requirements.traceErrorWords.count = traceErrorWords;
  result.requirements.cullErrorWords.count = cullErrorWords;
  result.installRewriteCount = 1u;
  result.uninstallRewriteCount = 1u;
  result.payloadComplete = true;
  return result;
}

svr::SparseVoxelPageInstallRewrite descriptorInstallRewrite(const svr::SparseVoxelReservedPagePayload &reservation)
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

bool reserveCopyInstallDescriptor(
    svr::SparseVoxelSharedPageTable &table,
    const svr::SparseVoxelProducedPagePayload &payload,
    const uint64_t copyFence,
    svr::SparseVoxelReservedPagePayload &reservation,
    std::string &error)
{
  if (!table.reserveProducedPagePayload(payload, reservation, &error))
    return false;
  if (!table.markPayloadCopied(reservation, copyFence, &error))
    return false;
  return table.installCopiedPagePayload(reservation, {descriptorInstallRewrite(reservation)}, &error);
}

svr::SparseVoxelStreamedInstance descriptorInstance(
    const svr::SparseVoxelPageProducer &producer,
    const svr::SparseVoxelPageId root)
{
  svr::SparseVoxelStreamedInstance instance{};
  instance.instanceIndex = 10u;
  instance.producer = producer;
  instance.rootPage = root;
  instance.firstRootNode = 0u;
  instance.rootNodeCount = 1u;
  for (uint32_t diagonal = 0u; diagonal < 4u; ++diagonal)
    instance.modelToWorld[diagonal * 4u + diagonal] = 1.0f;
  return instance;
}

void testDescriptorPublicationIsAtomicAcrossViews()
{
  std::cout << "shared descriptor publication updates raster, trace, material and cull views atomically\n";

  const auto source = descriptorProducer();
  const svr::SparseVoxelPageId page{source.producerId, 0u};
  std::string error;
  svr::SparseVoxelSharedPageTable table{};
  check(table.configure({8u, 8u, 8u, 8u, 8u, 8u, 0u, 0u, 8u, 8u, 8u}, &error), "descriptor table configures: " + error);
  check(table.addPage(page, source, {}, false, &error), "descriptor page registers: " + error);

  const auto produced = descriptorPayload(
      page,
      table.entry(page)->generation,
      source.producerRevision,
      100u,
      0xabcdu,
      0xabcdu,
      2u,
      1u,
      2u,
      2u,
      2u,
      1u,
      1u);
  svr::SparseVoxelReservedPagePayload reservation{};
  check(table.reserveProducedPagePayload(produced, reservation, &error), "unified descriptor payload reserves: " + error);
  check(table.markPayloadCopied(reservation, 12u, &error), "unified descriptor payload copy completes: " + error);
  check(!table.installCopiedPagePayload(reservation, {}, &error), "rewrite-count mismatch rejects descriptor publication");

  const auto *pending = table.entry(page);
  check(pending != nullptr && pending->state == svr::SparseVoxelPageState::UploadQueued, "failed descriptor publication leaves page upload-queued, not visible");
  check(pending != nullptr && pending->generation == 0u && pending->geometryRevision == 0u && pending->materialRevision == 0u, "failed descriptor publication does not publish revisions");
  check(pending != nullptr && !pending->rasterVisible() && !pending->traceable() && !pending->materialTraceable() && !pending->hasCullErrorSidecar(), "failed descriptor publication exposes no partial view");
  check(table.installRewrites().empty(), "failed descriptor publication records no install rewrite");

  check(table.installCopiedPagePayload(reservation, {descriptorInstallRewrite(reservation)}, &error), "unified descriptor publication commits: " + error);
  const auto *entry = table.entry(page);
  check(entry != nullptr && entry->resident(), "committed descriptor is resident");
  check(entry != nullptr && entry->rasterVisible(), "committed descriptor exposes raster view");
  check(entry != nullptr && entry->traceable(), "committed descriptor exposes trace view");
  check(entry != nullptr && entry->materialTraceable(), "committed descriptor exposes exact trace-material view");
  check(entry != nullptr && entry->hasCullErrorSidecar(), "committed descriptor exposes culling sidecar");
  check(entry != nullptr && entry->geometryRevision == 0xabcdu && entry->materialRevision == 0xabcdu, "committed descriptor publishes matching geometry/material revisions");
  check(entry != nullptr && entry->allocation.materialRevision == 0xabcdu && entry->allocation.traceFormat == svr::kVoxelTraceFormatCubic64, "allocation carries the unified descriptor identity");
}

void testMaterialRevisionMismatchRejectsTraceMaterialResolution()
{
  std::cout << "material generation mismatch rejects exact trace-material descriptors\n";

  const auto source = descriptorProducer(0x504841534531301ull);
  const svr::SparseVoxelPageId page{source.producerId, 0u};
  std::string error;
  svr::SparseVoxelSharedPageTable table{};
  check(table.configure({4u, 4u, 4u, 4u, 4u, 4u, 0u, 0u, 4u, 4u, 4u}, &error), "material mismatch table configures: " + error);
  check(table.addPage(page, source, {}, false, &error), "material mismatch page registers: " + error);

  const auto mismatched = descriptorPayload(
      page,
      table.entry(page)->generation,
      source.producerRevision,
      101u,
      0x2000u,
      0x2001u,
      1u,
      0u,
      1u,
      1u,
      1u,
      0u,
      0u);
  svr::SparseVoxelReservedPagePayload reservation{};
  check(!table.reserveProducedPagePayload(mismatched, reservation, &error), "mismatched material revision is rejected before allocation");
  check(table.entry(page) != nullptr && table.entry(page)->state == svr::SparseVoxelPageState::Unloaded, "mismatched material revision leaves page unloaded");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::TraceNodes).active == 0u, "mismatched material revision reserves no trace storage");
  check(table.allocatorStats(svr::SparseVoxelPhysicalBufferKind::Materials).active == 0u, "mismatched material revision reserves no material storage");
}

void testDirectTraceFeedbackRejectsRasterOnlyDesiredPage()
{
  std::cout << "direct trace feedback treats raster-only residency as missing trace data\n";

  const auto source = descriptorProducer(0x504841534531302ull);
  const svr::SparseVoxelPageId root{source.producerId, 0u};
  const svr::SparseVoxelPageId rasterOnly{source.producerId, 1u};
  std::string error;
  svr::SparseVoxelSharedPageTable table{};
  check(table.configure({8u, 8u, 8u, 8u, 8u, 8u, 0u, 0u, 8u, 8u, 8u}, &error), "raster-only trace table configures: " + error);
  check(table.addPage(root, source, {}, true, &error), "direct trace root registers: " + error);
  check(table.addPage(rasterOnly, source, {root}, false, &error), "raster-only desired page registers: " + error);

  svr::SparseVoxelReservedPagePayload rootReservation{};
  check(
      reserveCopyInstallDescriptor(
          table,
          descriptorPayload(root, table.entry(root)->generation, source.producerRevision, 110u, 0x3000u, 0x3000u, 1u, 0u, 1u, 1u, 1u, 0u, 0u),
          20u,
          rootReservation,
          error),
      "direct trace root fallback installs: " + error);

  svr::SparseVoxelReservedPagePayload rasterReservation{};
  check(
      reserveCopyInstallDescriptor(
          table,
          descriptorPayload(rasterOnly, table.entry(rasterOnly)->generation, source.producerRevision, 111u, 0u, 0u, 1u, 0u, 1u, 1u, 0u, 0u, 0u),
          21u,
          rasterReservation,
          error),
      "raster-only desired page installs: " + error);
  check(table.entry(rasterOnly)->resident() && table.entry(rasterOnly)->rasterVisible() && !table.entry(rasterOnly)->traceable(), "desired page is visible to raster but not direct-traceable");

  svr::SparseVoxelTraceRequestQueue queue{};
  queue.configure(4u);
  svr::SparseVoxelTraceFeedbackInput input{};
  input.instance = descriptorInstance(source, root);
  input.desiredPage = rasterOnly;
  input.receiverPage = root;
  input.receiverGeneration = table.entry(root)->generation;
  input.desiredLOD = 1u;
  input.rayClass = svr::SparseVoxelTraceRayClass::Shadow;
  input.mode = svr::SparseVoxelTraceLODMode::ConservativeVisibility;
  input.priority = 50u;
  input.desiredError = 0.25f;
  input.maximumT = 100.0f;
  input.requireDirectTraceView = true;

  svr::SparseVoxelTraceFeedbackResult result{};
  check(svr::processSparseVoxelTraceFeedback(table, input, queue, result, &error), "direct trace feedback processes raster-only desired page: " + error);
  check(result.desiredResident && !result.desiredTraceable, "feedback diagnostics distinguish resident from directly traceable");
  check(result.selectedConservativeFallback && result.selectedPage == root, "raster-only desired page falls back to direct trace root");
  check(result.emittedRequest && queue.size() == 1u, "raster-only desired page emits a direct trace refinement request");

  input.mode = svr::SparseVoxelTraceLODMode::Exact;
  input.allowCoarseExactFallback = false;
  check(svr::processSparseVoxelTraceFeedback(table, input, queue, result, &error), "exact direct trace feedback processes raster-only desired page: " + error);
  check(result.desiredResident && !result.desiredTraceable && result.unresolvedExact && !result.selectedPage.valid(), "exact direct trace refuses raster-only fallback and reports unresolved");
}

void testTraceOnlyPageIsInvalidForVisibleRasterCut()
{
  std::cout << "trace-only descriptors are invalid for visible raster selection\n";

  const auto source = descriptorProducer(0x504841534531303ull);
  const svr::SparseVoxelPageId page{source.producerId, 0u};
  std::string error;
  svr::SparseVoxelSharedPageTable table{};
  check(table.configure({4u, 4u, 1u, 1u, 1u, 1u, 0u, 0u, 4u, 4u, 4u}, &error), "trace-only table configures: " + error);
  check(table.addPage(page, source, {}, false, &error), "trace-only page registers: " + error);

  svr::SparseVoxelReservedPagePayload reservation{};
  check(
      reserveCopyInstallDescriptor(
          table,
          descriptorPayload(page, table.entry(page)->generation, source.producerRevision, 120u, 0x4000u, 0u, 1u, 0u, 0u, 0u, 1u, 0u, 0u),
          30u,
          reservation,
          error),
      "trace-only page installs: " + error);

  const auto *entry = table.entry(page);
  check(entry != nullptr && entry->resident() && entry->traceable(), "trace-only page is valid for direct trace");
  check(entry != nullptr && !entry->rasterVisible(), "trace-only page is not valid for visible raster cut");
  check(entry != nullptr && !entry->materialTraceable(), "trace-only page cannot resolve exact trace material without material payload");
}

void publish(
    svr::SparseVoxelTree64PageScheduler &scheduler,
    const svr::SparseVoxelTree64GeneratedPage &generated,
    const uint32_t physicalSlot)
{
  std::string error;
  const svr::SparseVoxelTree64PageHandle handle{physicalSlot, 1u};
  check(scheduler.finishUpload(generated.jobId, handle, &error), "Tree64 LOD upload finishes: " + error);
  svr::SparseVoxelTree64PublicationPage publication{};
  check(scheduler.takePublication(publication), "Tree64 LOD publication is available");
  check(publication.jobId == generated.jobId && publication.current, "Tree64 LOD publication remains current");
  check(scheduler.finishPublication(publication.jobId, &error), "Tree64 LOD publication commits: " + error);
}

void testLightingSidecarSelectionRequiresMatchingGeneration()
{
  std::cout << "lighting sidecar selection tracks geometry generation and fallback\n";

  const uint32_t childPage = 7u;
  const uint32_t parentPage = 3u;
  const uint64_t childGeometryRevision = 51u;
  const uint64_t parentGeometryRevision = 22u;
  std::vector<svr::SparseVoxelTerrainTree64LODVariant> variants{
      variant(childPage, 0u, childGeometryRevision, 128.0f, svr::SparseVoxelTerrainTree64LODVariantState::Resident, false),
      variant(childPage, 1u, childGeometryRevision, 32.0f, svr::SparseVoxelTerrainTree64LODVariantState::Resident, true),
      variant(childPage, 2u, childGeometryRevision, 8.0f, svr::SparseVoxelTerrainTree64LODVariantState::Requested, true),
      variant(parentPage, 2u, parentGeometryRevision, 256.0f, svr::SparseVoxelTerrainTree64LODVariantState::Resident, true),
      variant(childPage, 1u, 50u, 32.0f, svr::SparseVoxelTerrainTree64LODVariantState::Resident, true),
  };

  svr::SparseVoxelTerrainTree64LightingSelection selection{};
  svr::SparseVoxelCutDiagnostics diagnostics{};
  std::string error;
  svr::SparseVoxelTerrainTree64LightingSelectionRequest nearRequest{};
  nearRequest.logicalPageId = childPage;
  nearRequest.geometryRevision = childGeometryRevision;
  nearRequest.projectedErrorPixels = 80.0f;
  check(svr::selectSparseVoxelTerrainTree64LightingSidecar(variants, nearRequest, selection, &diagnostics, &error), "near Tree64 sidecar selection runs: " + error);
  check(selection.traceable && !selection.usedFallback && !selection.requestedDesiredSidecar && selection.selectedInternalLOD == 0u, "near geometry traces matching LOD0 sidecar");
  check(diagnostics.tree64LightingMissCount == 0u && diagnostics.invalidGenerationFallbackCount == 0u, "matching Tree64 generation produces no lighting diagnostics");

  svr::SparseVoxelTerrainTree64LightingSelectionRequest farRequest = nearRequest;
  farRequest.projectedErrorPixels = 4.0f;
  diagnostics = {};
  check(svr::selectSparseVoxelTerrainTree64LightingSidecar(variants, farRequest, selection, &diagnostics, &error), "far Tree64 sidecar selection runs: " + error);
  check(selection.traceable && selection.usedFallback && selection.requestedDesiredSidecar && selection.selectedInternalLOD == 1u, "missing coarser desired sidecar falls back to resident conservative LOD1");
  check(diagnostics.tree64LightingMissCount == 1u, "missing desired Tree64 LOD emits a lighting-sidecar request diagnostic");

  std::vector<svr::SparseVoxelTerrainTree64LODVariant> parentOnly{
      variant(childPage, 2u, childGeometryRevision, 8.0f, svr::SparseVoxelTerrainTree64LODVariantState::Requested, true),
      variant(parentPage, 2u, parentGeometryRevision, 256.0f, svr::SparseVoxelTerrainTree64LODVariantState::Resident, true),
  };
  farRequest.fallbackLogicalPageId = parentPage;
  farRequest.fallbackGeometryRevision = parentGeometryRevision;
  farRequest.fallbackProjectedErrorPixels = 12.0f;
  diagnostics = {};
  check(svr::selectSparseVoxelTerrainTree64LightingSidecar(parentOnly, farRequest, selection, &diagnostics, &error), "parent fallback sidecar selection runs: " + error);
  check(selection.traceable && selection.usedFallback && selection.selectedLogicalPageId == parentPage && selection.selectedGeometryRevision == parentGeometryRevision, "lighting explicitly traces a resident conservative parent sidecar");
  check(diagnostics.tree64LightingMissCount == 1u, "parent fallback still requests the missing child Tree64 sidecar");

  std::vector<svr::SparseVoxelTerrainTree64LODVariant> staleOnly{
      variant(childPage, 1u, 50u, 32.0f, svr::SparseVoxelTerrainTree64LODVariantState::Resident, true),
  };
  diagnostics = {};
  farRequest.fallbackGeometryRevision = 0u;
  check(svr::selectSparseVoxelTerrainTree64LightingSidecar(staleOnly, farRequest, selection, &diagnostics, &error), "stale Tree64 sidecar selection runs: " + error);
  check(!selection.traceable && selection.generationMismatch && selection.status == svr::SparseVoxelTerrainTree64LightingSelectionStatus::GenerationMismatch, "stale Tree64 sidecar is rejected as a generation mismatch");
  check(diagnostics.tree64LightingMissCount == 1u && diagnostics.invalidGenerationFallbackCount == 1u, "stale sidecar increments lighting and generation diagnostics");

  std::vector<svr::SparseVoxelTerrainTree64LODVariant> unsafeFallback{
      variant(childPage, 2u, childGeometryRevision, 8.0f, svr::SparseVoxelTerrainTree64LODVariantState::Requested, true),
      variant(childPage, 1u, childGeometryRevision, 32.0f, svr::SparseVoxelTerrainTree64LODVariantState::Resident, false),
  };
  diagnostics = {};
  check(svr::selectSparseVoxelTerrainTree64LightingSidecar(unsafeFallback, farRequest, selection, &diagnostics, &error), "unsafe fallback Tree64 selection runs: " + error);
  check(!selection.traceable && !selection.generationMismatch && selection.status == svr::SparseVoxelTerrainTree64LightingSelectionStatus::MissingDesired, "non-conservative sidecar is not used as an implicit lighting fallback");
}

void testSchedulerKeepsLODVersionsForOneLogicalPage()
{
  std::cout << "Tree64 scheduler keeps LOD versions for one logical page\n";

  svr::SparseVoxelTree64PageScheduler scheduler{};
  std::string error;
  check(scheduler.configure(schedulerSettings(), &error), "Tree64 LOD scheduler configures: " + error);

  std::mutex gateMutex;
  std::condition_variable gateChanged;
  bool firstStarted = false;
  bool releaseFirst = false;
  const auto firstGate = [&](const std::atomic_bool &cancelled)
  {
    std::unique_lock<std::mutex> lock(gateMutex);
    firstStarted = true;
    gateChanged.notify_all();
    gateChanged.wait_for(lock, 1s, [&]
    {
      return releaseFirst || cancelled.load(std::memory_order_relaxed);
    });
  };

  const auto lod0 = request(900u, 12u, 3u, 0u, 77u, 1.0f);
  const auto lod1 = request(900u, 12u, 3u, 1u, 77u, 2.0f);
  uint64_t lod0Job = 0u;
  uint64_t lod1Job = 0u;
  check(scheduler.request(lod0, buildFor(lod0, firstGate), lod0Job, &error), "LOD0 Tree64 request starts: " + error);
  {
    std::unique_lock<std::mutex> lock(gateMutex);
    check(gateChanged.wait_for(lock, 2s, [&] { return firstStarted; }), "LOD0 generation reaches the worker");
  }
  check(scheduler.request(lod1, buildFor(lod1), lod1Job, &error), "LOD1 Tree64 request for the same logical page is admitted: " + error);
  check(lod0Job != 0u && lod1Job != 0u && lod0Job != lod1Job, "LOD0 and LOD1 requests own distinct jobs");
  check(scheduler.isCurrent(lod0Job) && scheduler.isCurrent(lod1Job), "both Tree64 LOD jobs are current simultaneously");

  {
    std::lock_guard<std::mutex> lock(gateMutex);
    releaseFirst = true;
  }
  gateChanged.notify_all();

  std::vector<svr::SparseVoxelTree64GeneratedPage> generatedPages;
  check(waitUntil([&]
  {
    svr::SparseVoxelTree64GeneratedPage generated{};
    while (scheduler.takeGenerated(generated))
      generatedPages.push_back(std::move(generated));
    return generatedPages.size() == 2u;
  }), "both Tree64 LOD versions are generated");
  bool sawLOD0 = false;
  bool sawLOD1 = false;
  for (const auto &generated : generatedPages)
  {
    sawLOD0 = sawLOD0 || generated.request.internalLOD == 0u;
    sawLOD1 = sawLOD1 || generated.request.internalLOD == 1u;
  }
  check(sawLOD0 && sawLOD1, "generated Tree64 pages preserve distinct internal LOD identities");

  for (uint32_t index = 0u; index < generatedPages.size(); ++index)
    publish(scheduler, generatedPages[index], index);
  check(scheduler.idle(), "scheduler drains after publishing both sidecar LODs");

  const auto stale = request(901u, 13u, 4u, 0u, 80u, 1.0f);
  const auto replacement = request(901u, 13u, 4u, 1u, 81u, 5.0f);
  uint64_t staleJob = 0u;
  uint64_t replacementJob = 0u;
  check(scheduler.request(stale, buildFor(stale, firstGate), staleJob, &error), "stale-revision Tree64 request starts: " + error);
  check(scheduler.request(replacement, buildFor(replacement), replacementJob, &error), "new geometry revision request is admitted: " + error);
  check(staleJob != replacementJob && !scheduler.isCurrent(staleJob) && scheduler.isCurrent(replacementJob), "new geometry revision supersedes older Tree64 jobs for the logical page");
  scheduler.cancelTerrainSlot(13u);
}

} // namespace

int main()
{
  std::cout << "Terrain / virtual geometry convergence Phase 10 descriptor unification\n";
  testDescriptorPublicationIsAtomicAcrossViews();
  testMaterialRevisionMismatchRejectsTraceMaterialResolution();
  testDirectTraceFeedbackRejectsRasterOnlyDesiredPage();
  testTraceOnlyPageIsInvalidForVisibleRasterCut();
  testLightingSidecarSelectionRequiresMatchingGeneration();
  testSchedulerKeepsLODVersionsForOneLogicalPage();

  if (failures != 0)
  {
    std::cout << failures << " checks failed\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
