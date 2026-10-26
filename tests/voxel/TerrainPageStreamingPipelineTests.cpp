// The streaming pipeline's contract: worst rendered error first, one build per
// ground at a time, budgeted publication, and back-pressure that stops
// admission before memory grows.
#include "voxel/svraster/TerrainPageStreamingPipeline.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
namespace svr = voxel::svraster;
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  if (!condition)
    ++failures;
}

svr::TerrainStreamingRequest request(
    const int32_t x, const int32_t z, const uint32_t currentLOD, const uint32_t desiredLOD,
    const float pixels, const uint64_t generation = 1u)
{
  svr::TerrainStreamingRequest result{};
  result.address = {x, z, currentLOD};
  result.currentLOD = currentLOD;
  result.desiredLOD = desiredLOD;
  result.projectedVoxelPixels = pixels;
  result.generation = generation;
  return result;
}

// Waits for a predicate, so the tests never race the workers.
template <typename Predicate>
bool waitFor(Predicate predicate, const int milliseconds = 4000)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  while (std::chrono::steady_clock::now() < deadline)
  {
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

struct BuildLog
{
  std::mutex mutex;
  std::vector<svr::TerrainStreamingRequest> order;
  std::atomic_uint32_t active{0u};
  std::atomic_uint32_t peakActive{0u};
  std::atomic_bool hold{false};
};

svr::TerrainPageStreamingPipeline::Builder makeBuilder(BuildLog &log, const uint64_t bytes = 1024u)
{
  return [&log, bytes](const svr::TerrainStreamingRequest &request, const std::atomic_bool &cancelled)
  {
    const uint32_t active = ++log.active;
    uint32_t peak = log.peakActive.load();
    while (active > peak && !log.peakActive.compare_exchange_weak(peak, active))
      ;
    while (log.hold.load() && !cancelled.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    {
      std::lock_guard<std::mutex> lock(log.mutex);
      log.order.push_back(request);
    }
    --log.active;
    svr::TerrainBuiltPage page{};
    page.succeeded = true;
    page.uploadBytes = bytes;
    page.payload = std::make_shared<int>(static_cast<int>(request.address.x));
    return page;
  };
}

void testWorstErrorFirst()
{
  std::cout << "the queue drains worst rendered error first\n";
  BuildLog log;
  log.hold = true;
  svr::TerrainPageStreamingPipeline pipeline;
  svr::TerrainPageStreamingSettings settings{};
  settings.workerCount = 1u;             // one worker, so completion order is queue order
  settings.maximumInFlightBuilds = 1u;
  settings.maximumPendingUploads = 64u;
  std::string error;
  check(pipeline.start(settings, makeBuilder(log), &error), "pipeline starts: " + error);
  // Submitted in the wrong order on purpose.
  const std::vector<svr::TerrainStreamingRequest> requests{
      request(1, 0, 5u, 4u, 3.0f), request(2, 0, 5u, 4u, 91.0f), request(3, 0, 5u, 4u, 12.0f),
      request(4, 0, 5u, 4u, 47.0f)};
  check(pipeline.submitFrameRequests(requests) == 4u, "all four distinct grounds are admitted");
  log.hold = false;
  check(waitFor([&] { std::lock_guard<std::mutex> lock(log.mutex); return log.order.size() == 4u; }),
        "every admitted request is built");
  std::lock_guard<std::mutex> lock(log.mutex);
  // The first is whichever the worker picked up before the rest were queued;
  // the remainder must descend by error.
  bool descending = true;
  for (size_t index = 2u; index < log.order.size(); ++index)
    descending = descending && log.order[index - 1u].projectedVoxelPixels >= log.order[index].projectedVoxelPixels;
  check(descending, "the queue is drained in descending rendered error");
  check(log.order[1].projectedVoxelPixels == 91.0f || log.order[0].projectedVoxelPixels == 91.0f,
        "the worst page is built first");
  pipeline.stop();
}

void testOneBuildPerGround()
{
  std::cout << "a ground being built absorbs further requests\n";
  BuildLog log;
  log.hold = true;
  svr::TerrainPageStreamingPipeline pipeline;
  svr::TerrainPageStreamingSettings settings{};
  settings.workerCount = 2u;
  settings.maximumPendingUploads = 32u;
  std::string error;
  pipeline.start(settings, makeBuilder(log), &error);
  const auto one = request(7, 7, 6u, 5u, 20.0f);
  check(pipeline.submitFrameRequests({one}) == 1u, "the first request is admitted");
  check(pipeline.submitFrameRequests({one}) == 0u, "the same request while building is absorbed");
  check(pipeline.submitFrameRequests({request(7, 7, 6u, 5u, 99.0f)}) == 0u,
        "a louder request for the same ground is absorbed too");
  log.hold = false;
  check(waitFor([&] { return pipeline.stats().pagesBuilt == 1u; }), "it is built once");
  check(pipeline.submitFrameRequests({one}) == 0u, "a request for a ground already built at that generation is dropped");
  check(pipeline.submitFrameRequests({request(7, 7, 5u, 4u, 20.0f, 2u)}) == 1u,
        "the same ground at a new generation is admitted again");
  const auto stats = pipeline.stats();
  check(stats.requestsSuppressedInFlight >= 2u && stats.requestsSuppressedBuilt >= 1u,
        "suppressions are counted by reason (inFlight " + std::to_string(stats.requestsSuppressedInFlight) +
            ", built " + std::to_string(stats.requestsSuppressedBuilt) + ")");
  pipeline.stop();
}

void testBudgetedUploads()
{
  std::cout << "publication is budgeted, worst error first\n";
  BuildLog log;
  svr::TerrainPageStreamingPipeline pipeline;
  svr::TerrainPageStreamingSettings settings{};
  settings.workerCount = 3u;
  settings.maximumPendingUploads = 16u;
  settings.uploadBytesPerDrain = 2500u; // two pages of 1024 B, not three
  std::string error;
  pipeline.start(settings, makeBuilder(log, 1024u), &error);
  std::vector<svr::TerrainStreamingRequest> requests;
  for (int32_t index = 0; index < 6; ++index)
    requests.push_back(request(index, 3, 5u, 4u, static_cast<float>(10 + index)));
  check(pipeline.submitFrameRequests(requests) == 6u, "six grounds are admitted");
  check(waitFor([&] { return pipeline.stats().pagesBuilt == 6u; }), "all six are built");
  auto first = pipeline.takeUploads();
  check(first.size() == 2u, "a drain hands back only what the byte budget allows (" + std::to_string(first.size()) + ")");
  check(first.size() == 2u && first[0].request.projectedVoxelPixels >= first[1].request.projectedVoxelPixels,
        "the drain is ordered worst error first");
  check(!first.empty() && first[0].payload != nullptr, "the payload pointer moves to the caller");
  auto second = pipeline.takeUploads();
  auto third = pipeline.takeUploads();
  check(second.size() == 2u && third.size() == 2u, "the rest follow on later drains");
  check(pipeline.takeUploads().empty() && pipeline.stats().pendingUploads == 0u, "nothing is left behind");
  check(pipeline.stats().pagesUploaded == 6u, "every built page is published exactly once");
  pipeline.stop();
}

void testBackPressure()
{
  std::cout << "a full upload queue stops admission\n";
  BuildLog log;
  svr::TerrainPageStreamingPipeline pipeline;
  svr::TerrainPageStreamingSettings settings{};
  settings.workerCount = 2u;
  settings.maximumInFlightBuilds = 2u;
  settings.maximumPendingUploads = 4u;
  std::string error;
  pipeline.start(settings, makeBuilder(log), &error);
  std::vector<svr::TerrainStreamingRequest> requests;
  for (int32_t index = 0; index < 40; ++index)
    requests.push_back(request(index, 9, 5u, 4u, static_cast<float>(index)));
  uint32_t admitted = pipeline.submitFrameRequests(requests);
  check(admitted <= settings.maximumPendingUploads + settings.maximumInFlightBuilds,
        "admission stops at the pending-upload bound (" + std::to_string(admitted) + ")");
  check(waitFor([&] { return pipeline.stats().pendingUploads >= 4u || pipeline.stats().pagesBuilt >= admitted; }),
        "the workers fill the upload queue");
  const uint32_t refused = pipeline.submitFrameRequests(requests);
  check(refused == 0u, "nothing is admitted while the queue is full");
  check(pipeline.stats().requestsSuppressedBackPressure > 0u, "the refusals are counted");
  // Draining makes room again.
  pipeline.takeUploads();
  pipeline.takeUploads();
  check(waitFor([&] { return pipeline.submitFrameRequests(requests) > 0u; }),
        "admission resumes once the renderer drains");
  pipeline.stop();
}

void testStopIsClean()
{
  std::cout << "stop cancels in-flight work and joins\n";
  BuildLog log;
  log.hold = true;
  svr::TerrainPageStreamingPipeline pipeline;
  svr::TerrainPageStreamingSettings settings{};
  settings.workerCount = 4u;
  settings.maximumPendingUploads = 64u;
  std::string error;
  pipeline.start(settings, makeBuilder(log), &error);
  std::vector<svr::TerrainStreamingRequest> requests;
  for (int32_t index = 0; index < 12; ++index)
    requests.push_back(request(index, 11, 5u, 4u, static_cast<float>(index)));
  pipeline.submitFrameRequests(requests);
  check(waitFor([&] { return log.active.load() > 0u; }), "workers pick work up");
  log.hold = false;
  pipeline.stop();
  check(!pipeline.running(), "the pipeline stops");
  check(log.peakActive.load() <= settings.workerCount, "never more concurrent builds than workers");
}

} // namespace

int main()
{
  std::cout << "Terrain page streaming pipeline tests\n";
  testWorstErrorFirst();
  testOneBuildPerGround();
  testBudgetedUploads();
  testBackPressure();
  testStopIsClean();
  if (failures != 0)
  {
    std::cout << failures << " streaming pipeline checks failed\n";
    return 1;
  }
  std::cout << "TerrainPageStreamingPipeline: all checks passed\n";
  return 0;
}
