#include "voxel/svraster/SparseVoxelTree64Streaming.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#undef NDEBUG
#include <cassert>

namespace
{

using namespace voxel::svraster;
using namespace std::chrono_literals;

SparseVoxelTerrainTree64PagePayload payloadFor(
    const SparseVoxelTree64PageRequest &request,
    const uint32_t nodeCount = 1u)
{
  SparseVoxelTerrainTree64PagePayload payload{};
  payload.terrainSlot = request.terrainSlot;
  payload.terrainGeneration = request.terrainGeneration;
  payload.internalLOD = request.internalLOD;
  payload.geometryRevision = request.geometryRevision;
  payload.model.prebuiltNodes.resize(nodeCount);
  return payload;
}

SparseVoxelTree64PageScheduler::BuildFunction buildFor(
    const SparseVoxelTree64PageRequest request,
    const std::function<void(const std::atomic_bool &)> &before = {})
{
  return [request, before](
             SparseVoxelTerrainTree64PagePayload &payload,
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
    payload = payloadFor(request);
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

void publish(
    SparseVoxelTree64PageScheduler &scheduler,
    const SparseVoxelTree64GeneratedPage &generated,
    const uint32_t physicalSlot)
{
  std::string error;
  const SparseVoxelTree64PageHandle handle{physicalSlot, 1u};
  assert(scheduler.finishUpload(generated.jobId, handle, &error));
  SparseVoxelTree64PublicationPage publication{};
  assert(scheduler.takePublication(publication));
  assert(publication.jobId == generated.jobId);
  assert(publication.page == handle);
  assert(publication.current);
  assert(scheduler.finishPublication(publication.jobId, &error));
}

SparseVoxelTree64PageSchedulerSettings settings(
    const uint32_t workers,
    const uint32_t jobs,
    const uint32_t generated,
    const uint32_t publications,
    const uint64_t cpuBytes,
    const uint64_t uploadBytes)
{
  return {
    .workerCount = workers,
    .maximumJobs = jobs,
    .maximumQueuedRequests = jobs,
    .maximumGeneratedPages = generated,
    .maximumPublicationPages = publications,
    .maximumPendingUploadBytes = uploadBytes,
    .maximumPendingCPUBytes = cpuBytes,
    .physicalPageCapacity = jobs + 2u,
    .retirementHeadroomPages = 1u,
  };
}

void testSlowGenerationPriorityAndNonBlockingDrain()
{
  SparseVoxelTree64PageScheduler scheduler;
  std::string error;
  assert(scheduler.configure(
      settings(1u, 3u, 2u, 2u, 1u << 20u, 1u << 20u), &error));

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

  const SparseVoxelTree64PageRequest first{0u, 1u, 0u, 10u, 0.0f};
  const SparseVoxelTree64PageRequest low{1u, 1u, 0u, 11u, 1.0f};
  const SparseVoxelTree64PageRequest high{2u, 1u, 0u, 12u, 10.0f};
  uint64_t firstId = 0u;
  uint64_t lowId = 0u;
  uint64_t highId = 0u;
  assert(scheduler.request(first, buildFor(first, firstGate), firstId, &error));
  {
    std::unique_lock<std::mutex> lock(gateMutex);
    assert(gateChanged.wait_for(lock, 2s, [&] { return firstStarted; }));
  }
  assert(scheduler.request(low, buildFor(low), lowId, &error));
  assert(scheduler.request(high, buildFor(high), highId, &error));

  SparseVoxelTree64GeneratedPage unavailable{};
  const auto drainStart = std::chrono::steady_clock::now();
  assert(!scheduler.takeGenerated(unavailable));
  assert(std::chrono::steady_clock::now() - drainStart < 50ms);
  assert(scheduler.stats().activeJobs <= 3u);

  {
    std::lock_guard<std::mutex> lock(gateMutex);
    releaseFirst = true;
  }
  gateChanged.notify_all();

  std::vector<SparseVoxelTree64GeneratedPage> generatedPages;
  assert(waitUntil([&]
  {
    SparseVoxelTree64GeneratedPage page{};
    while (scheduler.takeGenerated(page))
      generatedPages.push_back(std::move(page));
    return generatedPages.size() == 3u;
  }));
  assert(generatedPages[0].jobId == firstId);
  assert(generatedPages[1].jobId == highId);
  assert(generatedPages[2].jobId == lowId);
  for (uint32_t index = 0u; index < generatedPages.size(); ++index)
    publish(scheduler, generatedPages[index], index);
  assert(scheduler.idle());
}

void testCoalescingAndObsoleteWork()
{
  SparseVoxelTree64PageScheduler scheduler;
  std::string error;
  assert(scheduler.configure(
      settings(1u, 4u, 2u, 2u, 1u << 20u, 1u << 20u), &error));

  std::mutex gateMutex;
  std::condition_variable gateChanged;
  bool blockerStarted = false;
  bool releaseBlocker = false;
  const SparseVoxelTree64PageRequest blocker{3u, 1u, 0u, 20u, 0.0f};
  uint64_t blockerId = 0u;
  assert(scheduler.request(
      blocker,
      buildFor(blocker, [&](const std::atomic_bool &cancelled)
      {
        std::unique_lock<std::mutex> lock(gateMutex);
        blockerStarted = true;
        gateChanged.notify_all();
        gateChanged.wait_for(lock, 1s, [&]
        {
          return releaseBlocker || cancelled.load(std::memory_order_relaxed);
        });
      }),
      blockerId, &error));
  {
    std::unique_lock<std::mutex> lock(gateMutex);
    assert(gateChanged.wait_for(lock, 2s, [&] { return blockerStarted; }));
  }

  std::atomic_uint32_t obsoleteBuilds{0u};
  const SparseVoxelTree64PageRequest obsolete{4u, 7u, 0u, 30u, 2.0f};
  const SparseVoxelTree64PageRequest newest{4u, 8u, 2u, 31u, 3.0f};
  uint64_t obsoleteId = 0u;
  uint64_t newestId = 0u;
  assert(scheduler.request(
      obsolete,
      buildFor(obsolete, [&](const std::atomic_bool &) { ++obsoleteBuilds; }),
      obsoleteId, &error));
  assert(scheduler.request(newest, buildFor(newest), newestId, &error));
  assert(obsoleteId != newestId);

  uint64_t duplicateId = 0u;
  auto boosted = newest;
  boosted.priority = 9.0f;
  assert(scheduler.request(
      boosted, buildFor(boosted), duplicateId, &error));
  assert(duplicateId == newestId);
  {
    std::lock_guard<std::mutex> lock(gateMutex);
    releaseBlocker = true;
  }
  gateChanged.notify_all();

  std::vector<SparseVoxelTree64GeneratedPage> pages;
  assert(waitUntil([&]
  {
    SparseVoxelTree64GeneratedPage page{};
    while (scheduler.takeGenerated(page))
      pages.push_back(std::move(page));
    return pages.size() == 2u;
  }));
  assert(obsoleteBuilds.load() == 0u);
  assert(pages[1].jobId == newestId);
  assert(pages[1].request.priority == 9.0f);
  publish(scheduler, pages[0], 0u);
  publish(scheduler, pages[1], 1u);
  assert(scheduler.stats().coalescedRequests == 1u);
  assert(scheduler.stats().cancelledJobs >= 1u);
}

void testBudgetsPagePressureAndStalePublication()
{
  const SparseVoxelTree64PageRequest requestA{6u, 1u, 0u, 50u, 1.0f};
  const SparseVoxelTree64PageRequest requestB{7u, 1u, 0u, 51u, 1.0f};
  const SparseVoxelTree64PageRequest requestC{8u, 1u, 0u, 52u, 1.0f};
  const auto sample = payloadFor(requestA);
  const uint64_t cpuBytes = sparseVoxelTree64PagePayloadCPUBytes(sample);
  const uint64_t uploadBytes = sparseVoxelTree64PagePayloadUploadBytes(sample);
  SparseVoxelTree64PageScheduler scheduler;
  std::string error;
  auto limits = settings(1u, 2u, 1u, 1u, cpuBytes, uploadBytes);
  limits.physicalPageCapacity = 4u;
  limits.retirementHeadroomPages = 1u;
  assert(scheduler.configure(limits, &error));
  assert(scheduler.updatePagePressure(1u, 0u, &error));

  uint64_t idA = 0u;
  uint64_t idB = 0u;
  uint64_t idC = 0u;
  assert(scheduler.request(requestA, buildFor(requestA), idA, &error));
  assert(scheduler.request(requestB, buildFor(requestB), idB, &error));
  assert(!scheduler.request(requestC, buildFor(requestC), idC, &error));
  assert(idC == 0u);

  SparseVoxelTree64GeneratedPage pageA{};
  assert(waitUntil([&] { return scheduler.takeGenerated(pageA); }));
  // Hold the upload owner deliberately. The second completed build must stay
  // behind the byte/result budget, while render-thread polling remains
  // non-blocking and the queue/accounting stay bounded.
  std::this_thread::sleep_for(20ms);
  SparseVoxelTree64GeneratedPage blocked{};
  const auto blockedDrainStart = std::chrono::steady_clock::now();
  assert(!scheduler.takeGenerated(blocked));
  assert(std::chrono::steady_clock::now() - blockedDrainStart < 50ms);
  const auto heldStats = scheduler.stats();
  assert(heldStats.pendingCPUBytes <= cpuBytes);
  assert(heldStats.pendingUploadBytes <= uploadBytes);
  assert(heldStats.generatedQueueHighWatermark <= 1u);
  publish(scheduler, pageA, 0u);

  SparseVoxelTree64GeneratedPage pageB{};
  assert(waitUntil([&] { return scheduler.takeGenerated(pageB); }));
  assert(scheduler.finishUpload(idB, {1u, 1u}, &error));
  const SparseVoxelTree64PageRequest replacementB{7u, 2u, 2u, 53u, 2.0f};
  uint64_t replacementId = 0u;
  assert(scheduler.request(
      replacementB, buildFor(replacementB), replacementId, &error));

  SparseVoxelTree64PublicationPage stale{};
  assert(scheduler.takePublication(stale));
  assert(stale.jobId == idB);
  assert(!stale.current);
  assert(!scheduler.finishPublication(stale.jobId, &error));
  assert(scheduler.discard(stale.jobId, &error));

  SparseVoxelTree64GeneratedPage replacement{};
  assert(waitUntil([&] { return scheduler.takeGenerated(replacement); }));
  publish(scheduler, replacement, 1u);
  assert(scheduler.stats().pendingCPUBytes == 0u);
  assert(scheduler.stats().pendingUploadBytes == 0u);
  assert(scheduler.stats().highWatermark <= 2u);

  assert(scheduler.updatePagePressure(2u, 1u, &error));
  uint64_t pressured = 0u;
  assert(!scheduler.request(
      requestC, buildFor(requestC), pressured, &error));
  assert(scheduler.updatePagePressure(2u, 0u, &error));
  assert(scheduler.request(
      requestC, buildFor(requestC), pressured, &error));
  SparseVoxelTree64GeneratedPage pageC{};
  assert(waitUntil([&] { return scheduler.takeGenerated(pageC); }));
  publish(scheduler, pageC, 2u);
  assert(scheduler.idle());
}

void testFailedBuildDoesNotPoisonScheduler()
{
  SparseVoxelTree64PageScheduler scheduler;
  std::string error;
  assert(scheduler.configure(
      settings(1u, 2u, 1u, 1u, 1u << 20u, 1u << 20u), &error));

  const SparseVoxelTree64PageRequest failed{9u, 1u, 0u, 60u, 1.0f};
  uint64_t failedId = 0u;
  assert(scheduler.request(
      failed,
      [](SparseVoxelTerrainTree64PagePayload &,
         const std::atomic_bool &, std::string *buildError)
      {
        if (buildError != nullptr)
          *buildError = "forced node/material overflow";
        return false;
      },
      failedId, &error));
  assert(waitUntil([&]
  {
    const auto current = scheduler.stats();
    return current.failedBuilds == 1u && scheduler.idle();
  }));

  // A failed page owns no publication state and must not prevent a later,
  // coarser request for the same stable terrain slot from completing.
  const SparseVoxelTree64PageRequest recovered{9u, 1u, 1u, 61u, 1.0f};
  uint64_t recoveredId = 0u;
  assert(scheduler.request(
      recovered, buildFor(recovered), recoveredId, &error));
  assert(recoveredId != failedId);
  SparseVoxelTree64GeneratedPage generated{};
  assert(waitUntil([&] { return scheduler.takeGenerated(generated); }));
  publish(scheduler, generated, 3u);
  assert(scheduler.idle());
  assert(scheduler.stats().failedBuilds == 1u);
}

} // namespace

int main()
{
  testSlowGenerationPriorityAndNonBlockingDrain();
  testCoalescingAndObsoleteWork();
  testBudgetsPagePressureAndStalePublication();
  testFailedBuildDoesNotPoisonScheduler();
  std::cout << "Sparse voxel Tree64 asynchronous scheduler tests passed\n";
  return 0;
}
