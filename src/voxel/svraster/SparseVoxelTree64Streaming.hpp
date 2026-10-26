#pragma once

#include "voxel/svraster/SparseVoxelTerrainTree64.hpp"
#include "voxel/svraster/SparseVoxelTree64Pages.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>

namespace voxel::svraster
{

struct SparseVoxelTree64PageRequest
{
  uint32_t terrainSlot = 0u;
  uint32_t terrainGeneration = 0u;
  uint32_t internalLOD = 0u;
  uint64_t geometryRevision = 0u;
  float priority = 0.0f;
  // Stable logical terrain page. When zero, terrainSlot is used for backwards
  // compatibility with older callers/tests. Different internal LODs for the
  // same logical page and geometry revision may coexist; a newer generation or
  // geometry revision supersedes older jobs for that logical page.
  uint64_t logicalPageId = 0u;
};

struct SparseVoxelTree64PageSchedulerSettings
{
  uint32_t workerCount = 1u;
  uint32_t maximumJobs = 64u;
  uint32_t maximumQueuedRequests = 64u;
  uint32_t maximumGeneratedPages = 8u;
  uint32_t maximumPublicationPages = 8u;
  uint64_t maximumPendingUploadBytes = 64ull * 1024ull * 1024ull;
  uint64_t maximumPendingCPUBytes = 64ull * 1024ull * 1024ull;
  uint32_t physicalPageCapacity = std::numeric_limits<uint32_t>::max();
  uint32_t retirementHeadroomPages = 0u;
};

enum class SparseVoxelTree64PageJobState : uint8_t
{
  Requested,
  Generating,
  Generated,
  Uploading,
  UploadedUnpublished,
  PublishQueued,
};

struct SparseVoxelTree64PageSchedulerStats
{
  uint32_t requestedJobs = 0u;
  uint32_t generatingJobs = 0u;
  uint32_t generatedJobs = 0u;
  uint32_t uploadingJobs = 0u;
  uint32_t uploadedJobs = 0u;
  uint32_t publishQueuedJobs = 0u;
  uint32_t activeJobs = 0u;
  uint32_t highWatermark = 0u;
  uint32_t requestQueueHighWatermark = 0u;
  uint32_t generatedQueueHighWatermark = 0u;
  uint32_t publicationQueueHighWatermark = 0u;
  uint64_t pendingCPUBytes = 0u;
  uint64_t pendingUploadBytes = 0u;
  uint64_t pendingCPUBytesHighWatermark = 0u;
  uint64_t pendingUploadBytesHighWatermark = 0u;
  uint64_t admittedRequests = 0u;
  uint64_t coalescedRequests = 0u;
  uint64_t cancelledJobs = 0u;
  uint64_t staleCompletions = 0u;
  uint64_t failedBuilds = 0u;
  uint64_t rejectedRequests = 0u;
};

struct SparseVoxelTree64GeneratedPage
{
  uint64_t jobId = 0u;
  SparseVoxelTree64PageRequest request{};
  SparseVoxelTerrainTree64PagePayload payload{};
  uint64_t uploadBytes = 0u;
};

struct SparseVoxelTree64PublicationPage
{
  uint64_t jobId = 0u;
  SparseVoxelTree64PageRequest request{};
  SparseVoxelTree64PageHandle page{};
  uint64_t uploadBytes = 0u;
  bool current = false;
};

// Bounded worker/control plane for immutable terrain Tree64 replacements.
// Render-thread handoffs are non-blocking. Payload and physical-page state do
// not become visible until finishPublication removes the accepted job.
class SparseVoxelTree64PageScheduler
{
public:
  using BuildFunction = std::function<bool(
      SparseVoxelTerrainTree64PagePayload &payload,
      const std::atomic_bool &cancelled,
      std::string *error)>;

  SparseVoxelTree64PageScheduler();
  ~SparseVoxelTree64PageScheduler();
  SparseVoxelTree64PageScheduler(
      const SparseVoxelTree64PageScheduler &) = delete;
  SparseVoxelTree64PageScheduler &operator=(
      const SparseVoxelTree64PageScheduler &) = delete;

  bool configure(
      const SparseVoxelTree64PageSchedulerSettings &settings,
      std::string *error = nullptr);
  void shutdown();

  bool request(
      const SparseVoxelTree64PageRequest &request,
      BuildFunction build,
      uint64_t &jobId,
      std::string *error = nullptr);

  bool takeGenerated(SparseVoxelTree64GeneratedPage &page);
  bool finishUpload(
      uint64_t jobId,
      SparseVoxelTree64PageHandle page,
      std::string *error = nullptr);
  bool takePublication(SparseVoxelTree64PublicationPage &page);
  bool finishPublication(uint64_t jobId, std::string *error = nullptr);

  bool discard(uint64_t jobId, std::string *error = nullptr);
  bool cancelTerrainSlot(uint32_t terrainSlot);
  bool updatePagePressure(
      uint32_t committedPages,
      uint32_t retiredPages,
      std::string *error = nullptr);

  bool isCurrent(uint64_t jobId) const;
  SparseVoxelTree64PageSchedulerStats stats() const;
  bool idle() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

uint64_t sparseVoxelTree64PagePayloadCPUBytes(
    const SparseVoxelTerrainTree64PagePayload &payload);
uint64_t sparseVoxelTree64PagePayloadUploadBytes(
    const SparseVoxelTerrainTree64PagePayload &payload);

} // namespace voxel::svraster
