#pragma once

#include "procedural/world/MapExportGpuCache.hpp"
#include "procedural/world/MapExportPageCache.hpp"
#include "procedural/world/MapExportStreaming.hpp"
#include "voxel/svraster/TerrainGPUBuildScheduler.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace voxel::svraster
{

struct TerrainGPUWorldMapAtlasConfig
{
  procedural::world::MapExportPageCacheConfig pageCache{};
  // These roots remain resident even with no active build job. Sampling-group
  // companions are expanded automatically by the streaming planner.
  std::vector<std::string> pinnedRootFields{};
  uint32_t maximumTrackedJobs = 64u;
};

struct TerrainGPUWorldMapAtlasDiagnostics
{
  uint32_t trackedJobs = 0u;
  uint32_t readyJobs = 0u;
  uint32_t waitingJobs = 0u;
  uint32_t pinnedPages = 0u;
  uint64_t committedRevision = 0u;
  uint64_t uploadedBytes = 0u;
  uint64_t deferredUnits = 0u;
  uint64_t publications = 0u;
};

// Returns the required and currently available optional fields consumed by
// the terrain shell generator. A refined elevation field supersedes the base
// field, while optional appearance/placement fields never block residency.
bool terrainGPUWorldMapFields(const procedural::world::MapExportBundle &bundle, std::vector<std::string> &fields, std::string *error = nullptr);

// Renderer-independent atlas residency and job-pinning coordinator. The
// render thread publishes a transaction to MapExportGpuPageCache first, then
// calls commit(). Only commit may transition scheduler jobs out of
// WaitingForMaps, so no build can observe CPU-resident but GPU-unpublished data.
class TerrainGPUWorldMapAtlas
{
public:
  bool initialize(const procedural::world::MapExportBundle &bundle, const TerrainGPUWorldMapAtlasConfig &config, std::string *error = nullptr);
  void clear();

  bool request(uint64_t jobId, const procedural::world::MapExportStreamRequest &request, std::string *error = nullptr);
  bool release(uint64_t jobId, std::string *error = nullptr);

  bool waitForPreparation(std::chrono::milliseconds timeout, std::string *error = nullptr);
  bool publish(size_t maximumUploadBytes, procedural::world::MapExportPagePublication &publication, std::string *error = nullptr);
  bool commit(const procedural::world::MapExportPagePublication &publication, TerrainGPUBuildScheduler &scheduler, std::string *error = nullptr);

  bool ready(uint64_t jobId) const;
  TerrainGPUWorldMapAtlasDiagnostics diagnostics() const;
  const procedural::world::MapExportGpuPageLayout &layout() const
  {
    return layout_;
  }

private:
  struct Job
  {
    std::vector<procedural::world::MapExportStreamKey> pages{};
    bool ready = false;
  };

  bool refreshTarget(std::string *error);
  bool committed(const Job &job) const;

  const procedural::world::MapExportBundle *bundle_ = nullptr;
  TerrainGPUWorldMapAtlasConfig config_{};
  procedural::world::MapExportGpuPageLayout layout_{};
  procedural::world::MapExportPageCache pageCache_{};
  std::vector<procedural::world::MapExportStreamKey> baseline_{};
  std::map<uint64_t, Job> jobs_{};
  std::set<std::string> committedKeys_{};
  std::set<std::string> pendingKeys_{};
  uint64_t pendingRevision_ = 0u;
  size_t pendingUploadedBytes_ = 0u;
  size_t pendingDeferredUnits_ = 0u;
  bool publicationPending_ = false;
  TerrainGPUWorldMapAtlasDiagnostics diagnostics_{};
};

} // namespace voxel::svraster
