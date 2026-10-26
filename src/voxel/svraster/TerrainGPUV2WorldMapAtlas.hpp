#pragma once

#include "procedural/world/MapExportGpuCache.hpp"
#include "procedural/world/MapExportPageCache.hpp"
#include "procedural/world/MapExportStreaming.hpp"
#include "voxel/svraster/TerrainGPUV2Columns.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace voxel::svraster
{

struct TerrainGPUV2WorldMapAtlasConfig
{
  procedural::world::MapExportPageCacheConfig pageCache{};
  uint32_t maximumPinnedTiles = 256u;
};

struct TerrainGPUV2WorldMapAtlasDiagnostics
{
  uint32_t pinnedTiles = 0u;
  uint32_t readyTiles = 0u;
  uint32_t waitingTiles = 0u;
  uint32_t pinnedPages = 0u;
  uint64_t committedRevision = 0u;
  uint64_t uploadedBytes = 0u;
  uint64_t deferredUnits = 0u;
  uint64_t publications = 0u;
  uint64_t reportedFaults = 0u;
  uint64_t retryRequests = 0u;
  uint64_t generatedColumnReadbackBytes = 0u;
  uint64_t targetRefreshes = 0u;
  uint64_t skippedTargetRefreshes = 0u;
};

// Renderer-independent virtual-atlas control plane. GPU bytes are applied by
// MapExportGpuPageCache before commit(); this class owns only residency targets
// and pins, never generated terrain payloads.
class TerrainGPUV2WorldMapAtlas
{
public:
  bool initialize(const procedural::world::MapExportBundle &bundle, const TerrainGPUV2WorldMapAtlasConfig &config, std::string *error = nullptr);
  void clear();

  bool pin(uint64_t tileId, const procedural::world::MapExportStreamRequest &request, std::string *error = nullptr);
  bool unpin(uint64_t tileId, std::string *error = nullptr);
  bool reportFaults(uint64_t tileId, const std::vector<TerrainGPUV2MapFaultGPU> &faults, std::string *error = nullptr);

  bool waitForPreparation(std::chrono::milliseconds timeout, std::string *error = nullptr);
  bool publish(size_t maximumUploadBytes, procedural::world::MapExportPagePublication &publication, std::string *error = nullptr);
  bool commit(const procedural::world::MapExportPagePublication &publication, std::string *error = nullptr);

  bool ready(uint64_t tileId) const;
  TerrainGPUV2WorldMapAtlasDiagnostics diagnostics() const;
  const procedural::world::MapExportGpuPageLayout &layout() const
  {
    return layout_;
  }

private:
  struct Pin
  {
    std::vector<procedural::world::MapExportStreamKey> pages{};
    bool ready = false;
  };

  bool refreshTarget(std::string *error);
  bool committed(const Pin &pin) const;
  bool addFaultPage(Pin &pin, const TerrainGPUV2MapFaultGPU &fault, std::string *error);

  const procedural::world::MapExportBundle *bundle_ = nullptr;
  TerrainGPUV2WorldMapAtlasConfig config_{};
  procedural::world::MapExportGpuPageLayout layout_{};
  procedural::world::MapExportPageCache pageCache_{};
  std::vector<procedural::world::MapExportStreamKey> baseline_{};
  std::vector<procedural::world::MapExportStreamKey> currentTarget_{};
  std::map<uint64_t, Pin> pins_{};
  std::set<std::string> committedKeys_{};
  std::set<std::string> pendingKeys_{};
  uint64_t pendingRevision_ = 0u;
  size_t pendingUploadedBytes_ = 0u;
  size_t pendingDeferredUnits_ = 0u;
  bool publicationPending_ = false;
  TerrainGPUV2WorldMapAtlasDiagnostics diagnostics_{};
};

} // namespace voxel::svraster
