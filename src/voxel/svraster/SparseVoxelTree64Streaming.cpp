#include "voxel/svraster/SparseVoxelTree64Streaming.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace voxel::svraster
{
namespace
{

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

uint64_t effectiveLogicalPageId(const SparseVoxelTree64PageRequest &request)
{
  return request.logicalPageId != 0u
      ? request.logicalPageId
      : static_cast<uint64_t>(request.terrainSlot);
}

bool sameIdentity(
    const SparseVoxelTree64PageRequest &left,
    const SparseVoxelTree64PageRequest &right)
{
  return left.terrainSlot == right.terrainSlot &&
      left.terrainGeneration == right.terrainGeneration &&
      left.internalLOD == right.internalLOD &&
      left.geometryRevision == right.geometryRevision &&
      effectiveLogicalPageId(left) == effectiveLogicalPageId(right);
}

struct SparseVoxelTree64PageRequestKey
{
  uint64_t logicalPageId = 0u;
  uint32_t terrainSlot = 0u;
  uint32_t terrainGeneration = 0u;
  uint32_t internalLOD = 0u;
  uint64_t geometryRevision = 0u;

  bool operator<(const SparseVoxelTree64PageRequestKey &other) const
  {
    return std::tie(
               logicalPageId,
               terrainSlot,
               terrainGeneration,
               internalLOD,
               geometryRevision) <
        std::tie(
               other.logicalPageId,
               other.terrainSlot,
               other.terrainGeneration,
               other.internalLOD,
               other.geometryRevision);
  }
};

SparseVoxelTree64PageRequestKey keyFor(
    const SparseVoxelTree64PageRequest &request)
{
  return {
    effectiveLogicalPageId(request),
    request.terrainSlot,
    request.terrainGeneration,
    request.internalLOD,
    request.geometryRevision,
  };
}

bool sameLogicalTerrainPage(
    const SparseVoxelTree64PageRequest &left,
    const SparseVoxelTree64PageRequest &right)
{
  return effectiveLogicalPageId(left) == effectiveLogicalPageId(right);
}

bool supersededBy(
    const SparseVoxelTree64PageRequest &oldRequest,
    const SparseVoxelTree64PageRequest &newRequest)
{
  return sameLogicalTerrainPage(oldRequest, newRequest) &&
      (oldRequest.terrainSlot != newRequest.terrainSlot ||
       oldRequest.terrainGeneration != newRequest.terrainGeneration ||
       oldRequest.geometryRevision != newRequest.geometryRevision);
}

} // namespace

uint64_t sparseVoxelTree64PagePayloadCPUBytes(
    const SparseVoxelTerrainTree64PagePayload &payload)
{
  return payload.model.prebuiltNodes.size() *
          sizeof(SparseVoxelTree64NodeGPU) +
      payload.materialCells.size() *
          sizeof(SparseVoxelTree64MaterialCellGPU);
}

uint64_t sparseVoxelTree64PagePayloadUploadBytes(
    const SparseVoxelTerrainTree64PagePayload &payload)
{
  return payload.model.prebuiltNodes.size() *
          (sizeof(SparseVoxelTree64NodeGPU) +
           sizeof(SparseVoxelTree64NodePaddedGPU)) +
      payload.materialCells.size() *
          sizeof(SparseVoxelTree64MaterialCellGPU) +
      2u * sizeof(uint32_t);
}

struct SparseVoxelTree64PageScheduler::Impl
{
  struct Job
  {
    uint64_t id = 0u;
    uint64_t sequence = 0u;
    SparseVoxelTree64PageRequest request{};
    BuildFunction build;
    std::shared_ptr<std::atomic_bool> cancelled =
        std::make_shared<std::atomic_bool>(false);
    SparseVoxelTree64PageJobState state =
        SparseVoxelTree64PageJobState::Requested;
    SparseVoxelTerrainTree64PagePayload payload{};
    SparseVoxelTree64PageHandle page{};
    uint64_t cpuBytes = 0u;
    uint64_t uploadBytes = 0u;
  };

  mutable std::mutex mutex;
  std::condition_variable workAvailable;
  std::condition_variable resultCapacityAvailable;
  SparseVoxelTree64PageSchedulerSettings settings{};
  std::map<uint64_t, std::shared_ptr<Job>> jobs;
  std::map<SparseVoxelTree64PageRequestKey, uint64_t> currentByRequest;
  std::vector<uint64_t> requestQueue;
  std::vector<uint64_t> generatedQueue;
  std::vector<uint64_t> publicationQueue;
  std::vector<std::thread> workers;
  SparseVoxelTree64PageSchedulerStats stats{};
  uint64_t nextJobId = 1u;
  uint64_t nextSequence = 1u;
  uint32_t committedPages = 0u;
  uint32_t retiredPages = 0u;
  bool configured = false;
  bool stopping = false;

  bool currentLocked(const std::shared_ptr<Job> &job) const
  {
    const auto current = currentByRequest.find(keyFor(job->request));
    return current != currentByRequest.end() &&
        current->second == job->id &&
        !job->cancelled->load(std::memory_order_relaxed);
  }

  void eraseCurrentLocked(const std::shared_ptr<Job> &job)
  {
    const auto current = currentByRequest.find(keyFor(job->request));
    if (current != currentByRequest.end() && current->second == job->id)
      currentByRequest.erase(current);
  }

  void removeJobLocked(const std::shared_ptr<Job> &job)
  {
    stats.pendingCPUBytes = job->cpuBytes <= stats.pendingCPUBytes
        ? stats.pendingCPUBytes - job->cpuBytes : 0u;
    stats.pendingUploadBytes = job->uploadBytes <= stats.pendingUploadBytes
        ? stats.pendingUploadBytes - job->uploadBytes : 0u;
    eraseCurrentLocked(job);
    jobs.erase(job->id);
  }

  uint32_t stateCountLocked(
      const SparseVoxelTree64PageJobState state) const
  {
    return static_cast<uint32_t>(std::count_if(
        jobs.begin(), jobs.end(),
        [&](const auto &entry) { return entry.second->state == state; }));
  }

  void refreshStatsLocked()
  {
    stats.requestedJobs = stateCountLocked(
        SparseVoxelTree64PageJobState::Requested);
    stats.generatingJobs = stateCountLocked(
        SparseVoxelTree64PageJobState::Generating);
    stats.generatedJobs = stateCountLocked(
        SparseVoxelTree64PageJobState::Generated);
    stats.uploadingJobs = stateCountLocked(
        SparseVoxelTree64PageJobState::Uploading);
    stats.uploadedJobs = stateCountLocked(
        SparseVoxelTree64PageJobState::UploadedUnpublished);
    stats.publishQueuedJobs = stateCountLocked(
        SparseVoxelTree64PageJobState::PublishQueued);
    stats.activeJobs = static_cast<uint32_t>(jobs.size());
    stats.highWatermark = std::max(stats.highWatermark, stats.activeJobs);
    stats.requestQueueHighWatermark = std::max(
        stats.requestQueueHighWatermark, stats.requestedJobs);
    stats.generatedQueueHighWatermark = std::max(
        stats.generatedQueueHighWatermark, stats.generatedJobs);
    stats.publicationQueueHighWatermark = std::max(
        stats.publicationQueueHighWatermark,
        stats.uploadedJobs + stats.publishQueuedJobs);
    stats.pendingCPUBytesHighWatermark = std::max(
        stats.pendingCPUBytesHighWatermark, stats.pendingCPUBytes);
    stats.pendingUploadBytesHighWatermark = std::max(
        stats.pendingUploadBytesHighWatermark, stats.pendingUploadBytes);
  }

  uint32_t availableJobPagesLocked() const
  {
    const uint64_t occupied = uint64_t{committedPages} + retiredPages +
        settings.retirementHeadroomPages;
    if (occupied >= settings.physicalPageCapacity)
      return 0u;
    return std::min<uint32_t>(
        settings.maximumJobs,
        static_cast<uint32_t>(settings.physicalPageCapacity - occupied));
  }

  std::shared_ptr<Job> nextRequestLocked()
  {
    auto selected = requestQueue.end();
    std::shared_ptr<Job> selectedJob;
    for (auto iterator = requestQueue.begin();
         iterator != requestQueue.end(); ++iterator)
    {
      const auto found = jobs.find(*iterator);
      if (found == jobs.end() ||
          found->second->state != SparseVoxelTree64PageJobState::Requested ||
          !currentLocked(found->second))
        continue;
      const auto &candidate = found->second;
      if (!selectedJob ||
          candidate->request.priority > selectedJob->request.priority ||
          (candidate->request.priority == selectedJob->request.priority &&
           candidate->sequence < selectedJob->sequence))
      {
        selected = iterator;
        selectedJob = candidate;
      }
    }
    if (!selectedJob)
      return {};
    requestQueue.erase(selected);
    return selectedJob;
  }

  bool canAcceptResultLocked(
      const uint64_t cpuBytes, const uint64_t uploadBytes) const
  {
    return stateCountLocked(SparseVoxelTree64PageJobState::Generated) <
            settings.maximumGeneratedPages &&
        cpuBytes <= settings.maximumPendingCPUBytes -
            std::min(stats.pendingCPUBytes,
                     settings.maximumPendingCPUBytes) &&
        uploadBytes <= settings.maximumPendingUploadBytes -
            std::min(stats.pendingUploadBytes,
                     settings.maximumPendingUploadBytes);
  }

  void workerMain()
  {
    for (;;)
    {
      std::shared_ptr<Job> job;
      BuildFunction build;
      {
        std::unique_lock<std::mutex> lock(mutex);
        workAvailable.wait(lock, [&]
        {
          return stopping || stateCountLocked(
              SparseVoxelTree64PageJobState::Requested) != 0u;
        });
        if (stopping)
          return;
        job = nextRequestLocked();
        if (!job)
          continue;
        job->state = SparseVoxelTree64PageJobState::Generating;
        build = job->build;
        refreshStatsLocked();
      }

      SparseVoxelTerrainTree64PagePayload payload{};
      std::string buildError;
      bool success = false;
      try
      {
        success = build &&
            build(payload, *job->cancelled, &buildError);
      }
      catch (const std::exception &exception)
      {
        buildError = exception.what();
      }
      catch (...)
      {
        buildError = "unknown Tree64 page build exception";
      }
      const uint64_t cpuBytes = success
          ? sparseVoxelTree64PagePayloadCPUBytes(payload) : 0u;
      const uint64_t uploadBytes = success
          ? sparseVoxelTree64PagePayloadUploadBytes(payload) : 0u;

      std::unique_lock<std::mutex> lock(mutex);
      if (stopping)
        return;
      if (!success)
      {
        if (job->cancelled->load(std::memory_order_relaxed) ||
            !currentLocked(job))
          ++stats.cancelledJobs;
        else
          ++stats.failedBuilds;
        removeJobLocked(job);
        refreshStatsLocked();
        workAvailable.notify_all();
        resultCapacityAvailable.notify_all();
        continue;
      }
      if (!currentLocked(job))
      {
        ++stats.staleCompletions;
        removeJobLocked(job);
        refreshStatsLocked();
        workAvailable.notify_all();
        resultCapacityAvailable.notify_all();
        continue;
      }
      if (payload.terrainSlot != job->request.terrainSlot ||
          payload.terrainGeneration != job->request.terrainGeneration ||
          payload.internalLOD != job->request.internalLOD ||
          payload.geometryRevision != job->request.geometryRevision ||
          cpuBytes > settings.maximumPendingCPUBytes ||
          uploadBytes > settings.maximumPendingUploadBytes)
      {
        ++stats.failedBuilds;
        removeJobLocked(job);
        refreshStatsLocked();
        workAvailable.notify_all();
        resultCapacityAvailable.notify_all();
        continue;
      }
      resultCapacityAvailable.wait(lock, [&]
      {
        return stopping || !currentLocked(job) ||
            canAcceptResultLocked(cpuBytes, uploadBytes);
      });
      if (stopping)
        return;
      if (!currentLocked(job))
      {
        ++stats.staleCompletions;
        removeJobLocked(job);
        refreshStatsLocked();
        workAvailable.notify_all();
        continue;
      }
      job->payload = std::move(payload);
      job->cpuBytes = cpuBytes;
      job->uploadBytes = uploadBytes;
      job->state = SparseVoxelTree64PageJobState::Generated;
      stats.pendingCPUBytes += cpuBytes;
      stats.pendingUploadBytes += uploadBytes;
      generatedQueue.push_back(job->id);
      refreshStatsLocked();
    }
  }

  void stop()
  {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!configured && workers.empty())
        return;
      stopping = true;
      for (auto &entry : jobs)
        entry.second->cancelled->store(true, std::memory_order_relaxed);
    }
    workAvailable.notify_all();
    resultCapacityAvailable.notify_all();
    for (auto &worker : workers)
      if (worker.joinable())
        worker.join();
    workers.clear();
    std::lock_guard<std::mutex> lock(mutex);
    jobs.clear();
    currentByRequest.clear();
    requestQueue.clear();
    generatedQueue.clear();
    publicationQueue.clear();
    stats = {};
    settings = {};
    nextJobId = 1u;
    nextSequence = 1u;
    committedPages = 0u;
    retiredPages = 0u;
    configured = false;
    stopping = false;
  }
};

SparseVoxelTree64PageScheduler::SparseVoxelTree64PageScheduler()
    : impl_(std::make_unique<Impl>())
{
}

SparseVoxelTree64PageScheduler::~SparseVoxelTree64PageScheduler()
{
  shutdown();
}

bool SparseVoxelTree64PageScheduler::configure(
    const SparseVoxelTree64PageSchedulerSettings &settings,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (settings.workerCount == 0u || settings.maximumJobs == 0u ||
      settings.maximumQueuedRequests == 0u ||
      settings.maximumGeneratedPages == 0u ||
      settings.maximumPublicationPages == 0u ||
      settings.maximumPendingUploadBytes == 0u ||
      settings.maximumPendingCPUBytes == 0u ||
      settings.physicalPageCapacity == 0u)
    return fail(error, "Tree64 page scheduler limits must be non-zero");
  if (settings.maximumQueuedRequests > settings.maximumJobs ||
      settings.maximumGeneratedPages > settings.maximumJobs ||
      settings.maximumPublicationPages > settings.maximumJobs)
    return fail(error, "Tree64 page scheduler queue limits exceed job capacity");
  if (settings.retirementHeadroomPages >= settings.physicalPageCapacity)
    return fail(error, "Tree64 scheduler retirement headroom exhausts the pool");

  shutdown();
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->settings = settings;
    impl_->configured = true;
    impl_->workers.reserve(settings.workerCount);
  }
  try
  {
    for (uint32_t index = 0u; index < settings.workerCount; ++index)
      impl_->workers.emplace_back([this] { impl_->workerMain(); });
  }
  catch (...)
  {
    shutdown();
    return fail(error, "Tree64 page scheduler could not start workers");
  }
  return true;
}

void SparseVoxelTree64PageScheduler::shutdown()
{
  impl_->stop();
}

bool SparseVoxelTree64PageScheduler::request(
    const SparseVoxelTree64PageRequest &request,
    BuildFunction build,
    uint64_t &jobId,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  jobId = 0u;
  if (request.terrainGeneration == 0u || request.geometryRevision == 0u ||
      !std::isfinite(request.priority) || !build)
    return fail(error, "Tree64 page request identity or callback is invalid");

  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->configured || impl_->stopping)
    return fail(error, "Tree64 page scheduler is not configured");

  std::shared_ptr<Impl::Job> old;
  const auto requestKey = keyFor(request);
  const auto current = impl_->currentByRequest.find(requestKey);
  if (current != impl_->currentByRequest.end())
  {
    const auto found = impl_->jobs.find(current->second);
    if (found != impl_->jobs.end())
    {
      old = found->second;
      if (sameIdentity(old->request, request))
      {
        old->request.priority = request.priority;
        if (old->state == SparseVoxelTree64PageJobState::Requested)
          old->build = std::move(build);
        jobId = old->id;
        ++impl_->stats.coalescedRequests;
        impl_->workAvailable.notify_one();
        return true;
      }
    }
  }

  std::vector<std::shared_ptr<Impl::Job>> superseded;
  for (const auto &entry : impl_->currentByRequest)
  {
    const auto found = impl_->jobs.find(entry.second);
    if (found != impl_->jobs.end() &&
        supersededBy(found->second->request, request))
      superseded.push_back(found->second);
  }

  const auto replaceableJob = [](const std::shared_ptr<Impl::Job> &job)
  {
    return job &&
        (job->state == SparseVoxelTree64PageJobState::Requested ||
         job->state == SparseVoxelTree64PageJobState::Generated);
  };
  uint32_t replaceableCount = 0u;
  uint32_t replaceableRequestedCount = 0u;
  for (const auto &job : superseded)
  {
    if (!replaceableJob(job))
      continue;
    ++replaceableCount;
    if (job->state == SparseVoxelTree64PageJobState::Requested)
      ++replaceableRequestedCount;
  }
  const uint32_t activeAfterReplacement = static_cast<uint32_t>(
      impl_->jobs.size() - replaceableCount);
  const uint32_t queuedAfterReplacement =
      impl_->stateCountLocked(SparseVoxelTree64PageJobState::Requested) -
      replaceableRequestedCount;
  if (activeAfterReplacement >= impl_->availableJobPagesLocked() ||
      queuedAfterReplacement >= impl_->settings.maximumQueuedRequests)
  {
    ++impl_->stats.rejectedRequests;
    return fail(error, "Tree64 page scheduler backpressure rejected request");
  }

  for (const auto &job : superseded)
  {
    job->cancelled->store(true, std::memory_order_relaxed);
    impl_->eraseCurrentLocked(job);
    if (replaceableJob(job))
    {
      ++impl_->stats.cancelledJobs;
      impl_->removeJobLocked(job);
    }
  }

  if (impl_->nextJobId == 0u || impl_->nextSequence == 0u)
    return fail(error, "Tree64 page scheduler id overflow");
  auto job = std::make_shared<Impl::Job>();
  job->id = impl_->nextJobId++;
  job->sequence = impl_->nextSequence++;
  job->request = request;
  job->build = std::move(build);
  impl_->jobs.emplace(job->id, job);
  impl_->currentByRequest[requestKey] = job->id;
  impl_->requestQueue.push_back(job->id);
  ++impl_->stats.admittedRequests;
  impl_->refreshStatsLocked();
  jobId = job->id;
  impl_->workAvailable.notify_one();
  impl_->resultCapacityAvailable.notify_all();
  return true;
}

bool SparseVoxelTree64PageScheduler::takeGenerated(
    SparseVoxelTree64GeneratedPage &page)
{
  page = {};
  std::lock_guard<std::mutex> lock(impl_->mutex);
  while (!impl_->generatedQueue.empty())
  {
    const uint64_t id = impl_->generatedQueue.front();
    impl_->generatedQueue.erase(impl_->generatedQueue.begin());
    const auto found = impl_->jobs.find(id);
    if (found == impl_->jobs.end() ||
        found->second->state != SparseVoxelTree64PageJobState::Generated)
      continue;
    const auto &job = found->second;
    if (!impl_->currentLocked(job))
    {
      ++impl_->stats.cancelledJobs;
      impl_->removeJobLocked(job);
      impl_->refreshStatsLocked();
      impl_->resultCapacityAvailable.notify_all();
      continue;
    }
    page.jobId = job->id;
    page.request = job->request;
    page.payload = std::move(job->payload);
    page.uploadBytes = job->uploadBytes;
    job->state = SparseVoxelTree64PageJobState::Uploading;
    impl_->refreshStatsLocked();
    impl_->resultCapacityAvailable.notify_all();
    return true;
  }
  return false;
}

bool SparseVoxelTree64PageScheduler::finishUpload(
    const uint64_t jobId,
    const SparseVoxelTree64PageHandle page,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->jobs.find(jobId);
  if (found == impl_->jobs.end())
    return fail(error, "Tree64 upload completed for an unknown job");
  const auto &job = found->second;
  if (job->state != SparseVoxelTree64PageJobState::Uploading || !page.valid())
    return fail(error, "Tree64 upload completion has invalid state or page");
  if (!impl_->currentLocked(job))
    return fail(error, "Tree64 upload completed for a superseded job");
  if (impl_->stateCountLocked(
          SparseVoxelTree64PageJobState::UploadedUnpublished) +
          impl_->stateCountLocked(
              SparseVoxelTree64PageJobState::PublishQueued) >=
      impl_->settings.maximumPublicationPages)
    return fail(error, "Tree64 publication queue is full");
  job->page = page;
  job->state = SparseVoxelTree64PageJobState::UploadedUnpublished;
  impl_->publicationQueue.push_back(jobId);
  impl_->refreshStatsLocked();
  return true;
}

bool SparseVoxelTree64PageScheduler::takePublication(
    SparseVoxelTree64PublicationPage &page)
{
  page = {};
  std::lock_guard<std::mutex> lock(impl_->mutex);
  while (!impl_->publicationQueue.empty())
  {
    const uint64_t id = impl_->publicationQueue.front();
    impl_->publicationQueue.erase(impl_->publicationQueue.begin());
    const auto found = impl_->jobs.find(id);
    if (found == impl_->jobs.end() || found->second->state !=
            SparseVoxelTree64PageJobState::UploadedUnpublished)
      continue;
    const auto &job = found->second;
    page.jobId = job->id;
    page.request = job->request;
    page.page = job->page;
    page.uploadBytes = job->uploadBytes;
    page.current = impl_->currentLocked(job);
    job->state = SparseVoxelTree64PageJobState::PublishQueued;
    impl_->refreshStatsLocked();
    return true;
  }
  return false;
}

bool SparseVoxelTree64PageScheduler::finishPublication(
    const uint64_t jobId,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->jobs.find(jobId);
  if (found == impl_->jobs.end() || found->second->state !=
          SparseVoxelTree64PageJobState::PublishQueued)
    return fail(error, "Tree64 publication completed in the wrong state");
  if (!impl_->currentLocked(found->second))
    return fail(error, "Tree64 publication completed for a superseded job");
  impl_->removeJobLocked(found->second);
  impl_->refreshStatsLocked();
  impl_->resultCapacityAvailable.notify_all();
  impl_->workAvailable.notify_all();
  return true;
}

bool SparseVoxelTree64PageScheduler::discard(
    const uint64_t jobId,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->jobs.find(jobId);
  if (found == impl_->jobs.end())
    return fail(error, "Tree64 discard used an unknown job");
  const auto &job = found->second;
  job->cancelled->store(true, std::memory_order_relaxed);
  if (job->state == SparseVoxelTree64PageJobState::Generating)
  {
    impl_->eraseCurrentLocked(job);
    impl_->resultCapacityAvailable.notify_all();
    impl_->workAvailable.notify_all();
    return true;
  }
  ++impl_->stats.cancelledJobs;
  impl_->removeJobLocked(job);
  impl_->refreshStatsLocked();
  impl_->resultCapacityAvailable.notify_all();
  impl_->workAvailable.notify_all();
  return true;
}

bool SparseVoxelTree64PageScheduler::cancelTerrainSlot(
    const uint32_t terrainSlot)
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<std::shared_ptr<Impl::Job>> cancelled;
  for (const auto &entry : impl_->currentByRequest)
  {
    const auto found = impl_->jobs.find(entry.second);
    if (found != impl_->jobs.end() &&
        found->second->request.terrainSlot == terrainSlot)
      cancelled.push_back(found->second);
  }
  if (cancelled.empty())
    return false;
  for (const auto &job : cancelled)
  {
    job->cancelled->store(true, std::memory_order_relaxed);
    impl_->eraseCurrentLocked(job);
    if (job->state == SparseVoxelTree64PageJobState::Requested ||
        job->state == SparseVoxelTree64PageJobState::Generated)
    {
      ++impl_->stats.cancelledJobs;
      impl_->removeJobLocked(job);
    }
  }
  impl_->refreshStatsLocked();
  impl_->resultCapacityAvailable.notify_all();
  impl_->workAvailable.notify_all();
  return true;
}

bool SparseVoxelTree64PageScheduler::updatePagePressure(
    const uint32_t committedPages,
    const uint32_t retiredPages,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->configured || impl_->stopping)
    return fail(error, "Tree64 page scheduler is not configured");
  const uint64_t occupied = uint64_t{committedPages} + retiredPages +
      impl_->settings.retirementHeadroomPages;
  if (occupied > impl_->settings.physicalPageCapacity)
    return fail(error, "Tree64 page pressure exceeds physical capacity");
  impl_->committedPages = committedPages;
  impl_->retiredPages = retiredPages;
  return true;
}

bool SparseVoxelTree64PageScheduler::isCurrent(const uint64_t jobId) const
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto found = impl_->jobs.find(jobId);
  return found != impl_->jobs.end() && impl_->currentLocked(found->second);
}

SparseVoxelTree64PageSchedulerStats
SparseVoxelTree64PageScheduler::stats() const
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->stats;
}

bool SparseVoxelTree64PageScheduler::idle() const
{
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->jobs.empty();
}

} // namespace voxel::svraster
