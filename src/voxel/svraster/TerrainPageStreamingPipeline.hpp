#pragma once

// Continuous, budgeted page streaming.
//
// The streaming path this replaces was one serialized job: the renderer took a
// single refinement request, planned, generated, published and applied it, and
// only then started the next. Measured on the world map with the camera on a
// ridge, that was three jobs in three hundred frames, about 4.5 s apart, of
// which the actual generation was 90-200 ms. The cut therefore advanced by one
// refinement every few seconds and the ground stayed at 120 px triangles.
//
// This pipeline is the shape that fixes it:
//   - the renderer hands it the current request snapshot every frame (the GPU
//     priority buffer is reset and rewritten per frame, so a request is a
//     statement about this frame, not durable work);
//   - requests are ordered by rendered error, so the chunk furthest from its
//     correct level - which is the one nearest the camera - is built first;
//   - a page already being built, or already built at that generation, absorbs
//     further requests for the same ground instead of queueing them again;
//   - N workers pull from that queue and build whole pages off the render
//     thread;
//   - finished pages wait in a bounded queue and are handed back a budgeted
//     number of bytes per frame, so publication cannot stall a frame;
//   - when that queue is full the pipeline stops admitting work, which is the
//     back-pressure that keeps generation from running ahead of upload.
//
// Ownership moves, never copies: a built page carries an opaque payload
// pointer the builder allocated and the consumer takes.
//
// The queue is a mutex-protected heap rather than a lock-free structure. The
// measured rates are hundreds of requests and single-digit completions per
// frame, so the critical sections are nanoseconds against a 4.5 s stall; a
// lock-free heap would add real complexity to remove nothing measurable.

#include "voxel/svraster/PlanetVoxelChunk.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace voxel::svraster
{

// One page the renderer wants at a finer level than it holds.
struct TerrainStreamingRequest
{
  WorldVoxelChunkAddress address{};
  // Level resident now and the level the rendered error asks for. A request
  // may span several levels: the pipeline hands the builder the whole span so
  // it can produce the destination page directly.
  uint32_t currentLOD = 0u;
  uint32_t desiredLOD = 0u;
  // Residency generation the request was made against. A request for a
  // generation already built is stale and is dropped.
  uint64_t generation = 0u;
  // The rendered error that produced this request: the projected size of the
  // page's voxel, in pixels. This is the sort key, so the queue drains
  // worst-error-first and nearby ground wins.
  float projectedVoxelPixels = 0.0f;
};

// A page a worker finished. `payload` is whatever the builder allocated; the
// consumer takes ownership.
struct TerrainBuiltPage
{
  TerrainStreamingRequest request{};
  std::shared_ptr<void> payload{};
  uint64_t uploadBytes = 0u;
  bool succeeded = false;
  std::string error;
};

struct TerrainPageStreamingSettings
{
  uint32_t workerCount = 5u;
  // Finished pages waiting to be uploaded. Reaching this stops admission.
  uint32_t maximumPendingUploads = 8u;
  // Builds allowed in flight at once. Zero means twice the worker count, so a
  // worker always has one queued behind it.
  uint32_t maximumInFlightBuilds = 0u;
  // Bytes of finished pages handed back per drain.
  uint64_t uploadBytesPerDrain = 32ull * 1024ull * 1024ull;
};

struct TerrainPageStreamingStats
{
  uint64_t requestsSeen = 0u;
  uint64_t requestsAdmitted = 0u;
  uint64_t requestsSuppressedInFlight = 0u;
  uint64_t requestsSuppressedBuilt = 0u;
  uint64_t requestsSuppressedBackPressure = 0u;
  uint64_t pagesBuilt = 0u;
  uint64_t pagesFailed = 0u;
  uint64_t pagesUploaded = 0u;
  uint32_t queuedRequests = 0u;
  uint32_t inFlightBuilds = 0u;
  uint32_t pendingUploads = 0u;
};

class TerrainPageStreamingPipeline
{
public:
  using Builder = std::function<TerrainBuiltPage(const TerrainStreamingRequest &, const std::atomic_bool &cancelled)>;

  TerrainPageStreamingPipeline() = default;
  ~TerrainPageStreamingPipeline();
  TerrainPageStreamingPipeline(const TerrainPageStreamingPipeline &) = delete;
  TerrainPageStreamingPipeline &operator=(const TerrainPageStreamingPipeline &) = delete;

  // Starts the workers. Calling it again reconfigures from a stopped state.
  bool start(const TerrainPageStreamingSettings &settings, Builder builder, std::string *error = nullptr);
  void stop();
  bool running() const;

  // The frame's request snapshot, in any order. Returns how many were
  // admitted; the rest were duplicates, stale, or refused by back-pressure.
  uint32_t submitFrameRequests(const std::vector<TerrainStreamingRequest> &requests);

  // Finished pages, worst-error-first, up to the byte budget. Ownership of
  // each payload moves to the caller.
  std::vector<TerrainBuiltPage> takeUploads();

  // Marks a ground as published at that generation, so later requests for the
  // same generation are dropped. A rebuilt chunk gets a new generation and is
  // requestable again.
  void notePublished(const WorldVoxelChunkAddress &address, uint64_t generation, uint32_t level);

  // Drops everything queued and not yet started, for a cut that moved.
  void clearQueued();

  TerrainPageStreamingStats stats() const;

private:
  struct QueueEntry
  {
    TerrainStreamingRequest request{};
    uint64_t serial = 0u;
    bool operator<(const QueueEntry &other) const
    {
      // std::priority_queue is a max-heap: worst error first, then oldest.
      if (request.projectedVoxelPixels != other.request.projectedVoxelPixels)
        return request.projectedVoxelPixels < other.request.projectedVoxelPixels;
      return serial > other.serial;
    }
  };
  struct GroundState
  {
    uint64_t buildingGeneration = 0u;
    uint32_t buildingLevel = 0u;
    bool building = false;
    uint64_t builtGeneration = 0u;
    uint32_t builtLevel = 0u;
    bool built = false;
  };

  void workerLoop();
  static uint64_t groundKey(const WorldVoxelChunkAddress &address);

  mutable std::mutex mutex_;
  std::condition_variable work_;
  std::vector<std::thread> workers_;
  std::atomic_bool stopping_{false};
  std::atomic_bool cancelled_{false};
  TerrainPageStreamingSettings settings_{};
  Builder builder_{};
  std::priority_queue<QueueEntry> queue_;
  std::unordered_map<uint64_t, GroundState> grounds_;
  std::vector<TerrainBuiltPage> completed_;
  uint64_t nextSerial_ = 1u;
  uint32_t inFlight_ = 0u;
  TerrainPageStreamingStats stats_{};
};

} // namespace voxel::svraster
