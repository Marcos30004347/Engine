#include "voxel/svraster/TerrainPageStreamingPipeline.hpp"

#include <algorithm>

namespace voxel::svraster
{

TerrainPageStreamingPipeline::~TerrainPageStreamingPipeline()
{
  stop();
}

uint64_t TerrainPageStreamingPipeline::groundKey(const WorldVoxelChunkAddress &address)
{
  // The ground a page covers, not the page: a request for a finer level of the
  // same ground is the same work item.
  return (static_cast<uint64_t>(static_cast<uint32_t>(address.x)) << 40u) ^
      (static_cast<uint64_t>(static_cast<uint32_t>(address.z)) << 8u) ^
      static_cast<uint64_t>(address.spatialLOD);
}

bool TerrainPageStreamingPipeline::start(
    const TerrainPageStreamingSettings &settings,
    Builder builder,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!builder)
  {
    if (error != nullptr)
      *error = "terrain page streaming pipeline needs a builder";
    return false;
  }
  if (settings.workerCount == 0u || settings.maximumPendingUploads == 0u ||
      settings.uploadBytesPerDrain == 0u)
  {
    if (error != nullptr)
      *error = "terrain page streaming pipeline settings are invalid";
    return false;
  }
  stop();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = settings;
    if (settings_.maximumInFlightBuilds == 0u)
      settings_.maximumInFlightBuilds = settings_.workerCount * 2u;
    builder_ = std::move(builder);
    stats_ = {};
    grounds_.clear();
    completed_.clear();
    while (!queue_.empty())
      queue_.pop();
    inFlight_ = 0u;
    nextSerial_ = 1u;
    stopping_ = false;
    cancelled_ = false;
  }
  workers_.reserve(settings_.workerCount);
  for (uint32_t worker = 0u; worker < settings_.workerCount; ++worker)
    workers_.emplace_back([this] { workerLoop(); });
  return true;
}

void TerrainPageStreamingPipeline::stop()
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (workers_.empty())
      return;
    stopping_ = true;
    cancelled_ = true;
  }
  work_.notify_all();
  for (auto &worker : workers_)
    if (worker.joinable())
      worker.join();
  workers_.clear();
  std::lock_guard<std::mutex> lock(mutex_);
  stopping_ = false;
  cancelled_ = false;
}

bool TerrainPageStreamingPipeline::running() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return !workers_.empty();
}

uint32_t TerrainPageStreamingPipeline::submitFrameRequests(
    const std::vector<TerrainStreamingRequest> &requests)
{
  uint32_t admitted = 0u;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.requestsSeen += requests.size();
    for (const auto &request : requests)
    {
      if (request.desiredLOD >= request.currentLOD)
        continue;
      // Publication back-pressure: a full upload queue means the renderer is
      // not draining as fast as the workers produce, so stop taking work
      // rather than building pages that would sit in memory.
      const size_t inFlightAndPending = completed_.size() + inFlight_ + queue_.size();
      if (completed_.size() >= settings_.maximumPendingUploads ||
          inFlightAndPending >= settings_.maximumPendingUploads + settings_.maximumInFlightBuilds)
      {
        ++stats_.requestsSuppressedBackPressure;
        continue;
      }
      const uint64_t key = groundKey(request.address);
      GroundState &ground = grounds_[key];
      if (ground.building && ground.buildingGeneration == request.generation &&
          ground.buildingLevel <= request.desiredLOD)
      {
        // Already building this ground at this level or finer: absorb it.
        ++stats_.requestsSuppressedInFlight;
        continue;
      }
      if (ground.built && ground.builtGeneration == request.generation &&
          ground.builtLevel <= request.desiredLOD)
      {
        ++stats_.requestsSuppressedBuilt;
        continue;
      }
      if (ground.building)
      {
        ++stats_.requestsSuppressedInFlight;
        continue;
      }
      ground.building = true;
      ground.buildingGeneration = request.generation;
      ground.buildingLevel = request.desiredLOD;
      queue_.push(QueueEntry{request, nextSerial_++});
      ++stats_.requestsAdmitted;
      ++admitted;
    }
    stats_.queuedRequests = static_cast<uint32_t>(queue_.size());
  }
  if (admitted != 0u)
    work_.notify_all();
  return admitted;
}

void TerrainPageStreamingPipeline::workerLoop()
{
  for (;;)
  {
    QueueEntry entry{};
    {
      std::unique_lock<std::mutex> lock(mutex_);
      work_.wait(lock, [this] { return stopping_ || (!queue_.empty() && inFlight_ < settings_.maximumInFlightBuilds); });
      if (stopping_)
        return;
      entry = queue_.top();
      queue_.pop();
      ++inFlight_;
      stats_.queuedRequests = static_cast<uint32_t>(queue_.size());
      stats_.inFlightBuilds = inFlight_;
    }
    TerrainBuiltPage built = builder_(entry.request, cancelled_);
    built.request = entry.request;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --inFlight_;
      stats_.inFlightBuilds = inFlight_;
      GroundState &ground = grounds_[groundKey(entry.request.address)];
      ground.building = false;
      if (built.succeeded)
      {
        ++stats_.pagesBuilt;
        ground.built = true;
        ground.builtGeneration = entry.request.generation;
        ground.builtLevel = entry.request.desiredLOD;
        completed_.push_back(std::move(built));
        stats_.pendingUploads = static_cast<uint32_t>(completed_.size());
      }
      else
      {
        ++stats_.pagesFailed;
      }
    }
    work_.notify_all();
  }
}

std::vector<TerrainBuiltPage> TerrainPageStreamingPipeline::takeUploads()
{
  std::vector<TerrainBuiltPage> uploads;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (completed_.empty())
      return uploads;
    // Worst error first, so the frame's upload budget buys the most image
    // error back.
    std::stable_sort(
        completed_.begin(), completed_.end(),
        [](const TerrainBuiltPage &left, const TerrainBuiltPage &right)
        { return left.request.projectedVoxelPixels > right.request.projectedVoxelPixels; });
    uint64_t bytes = 0u;
    size_t taken = 0u;
    for (auto &page : completed_)
    {
      // Always take one, or a page larger than the budget would never move.
      if (taken != 0u && bytes + page.uploadBytes > settings_.uploadBytesPerDrain)
        break;
      bytes += page.uploadBytes;
      uploads.push_back(std::move(page));
      ++taken;
    }
    completed_.erase(completed_.begin(), completed_.begin() + static_cast<std::ptrdiff_t>(taken));
    stats_.pendingUploads = static_cast<uint32_t>(completed_.size());
    stats_.pagesUploaded += taken;
  }
  work_.notify_all();
  return uploads;
}

void TerrainPageStreamingPipeline::notePublished(
    const WorldVoxelChunkAddress &address,
    const uint64_t generation,
    const uint32_t level)
{
  std::lock_guard<std::mutex> lock(mutex_);
  GroundState &ground = grounds_[groundKey(address)];
  ground.built = true;
  ground.builtGeneration = generation;
  ground.builtLevel = level;
}

void TerrainPageStreamingPipeline::clearQueued()
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty())
    {
      GroundState &ground = grounds_[groundKey(queue_.top().request.address)];
      ground.building = false;
      queue_.pop();
    }
    stats_.queuedRequests = 0u;
  }
  work_.notify_all();
}

TerrainPageStreamingStats TerrainPageStreamingPipeline::stats() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  TerrainPageStreamingStats stats = stats_;
  stats.queuedRequests = static_cast<uint32_t>(queue_.size());
  stats.inFlightBuilds = inFlight_;
  stats.pendingUploads = static_cast<uint32_t>(completed_.size());
  return stats;
}

} // namespace voxel::svraster
