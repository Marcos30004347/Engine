#pragma once

#include "procedural/world/MapExportStreaming.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace procedural::world {

struct MapExportPageCacheConfig
{
  // Empty reads the already-decoded page.data payloads. A directory makes the
  // worker read, hash-check and decompress individual page files instead.
  std::string directory;
  uint32_t slotCapacity = 256u;
};

struct MapExportPhysicalMapping
{
  MapExportStreamKey key;
  uint32_t slot = 0u;
};

bool operator==(const MapExportPhysicalMapping &left, const MapExportPhysicalMapping &right);

struct MapExportPhysicalUpload
{
  MapExportStreamKey key;
  uint32_t slot = 0u;
  std::vector<uint8_t> data;
};

// One atomic safe-frame transaction. A GPU consumer writes every upload into
// its assigned physical slot, then replaces/patches its logical page table from
// pageTable. Publishing the table last prevents shaders from observing a slot
// before its bytes are ready.
struct MapExportPagePublication
{
  uint64_t revision = 0u;
  std::vector<MapExportPhysicalUpload> uploads;
  std::vector<MapExportPhysicalMapping> evictions;
  std::vector<MapExportPhysicalMapping> pageTable;
  size_t uploadedBytes = 0u;
  size_t deferredUnits = 0u;
  size_t residentPages = 0u;
  size_t occupiedSlots = 0u;
  size_t preparingUnits = 0u;
  size_t preparedUnits = 0u;
};

// Executes MapExportStreamingCache targets. Page I/O/decompression happens on
// a worker; slot mutation and page-table publication happen only when publish
// is called by the render thread at a known-safe frame boundary. The bundle
// must outlive this object.
class MapExportPageCache
{
public:
  MapExportPageCache();
  ~MapExportPageCache();

  MapExportPageCache(const MapExportPageCache &) = delete;
  MapExportPageCache &operator=(const MapExportPageCache &) = delete;

  bool initialize(const MapExportBundle &bundle, const MapExportPageCacheConfig &config, std::string *error = nullptr);
  void clear();

  // target normally comes from MapExportStreamingCache::residentPages().
  // Roots and complete sampling groups are required and checked here again.
  bool setTarget(const std::vector<MapExportStreamKey> &target, std::string *error = nullptr);

  // Test/tool synchronization only; an interactive renderer polls publish.
  bool waitForPreparation(std::chrono::milliseconds timeout, std::string *error = nullptr);

  bool publish(size_t maximumUploadBytes, MapExportPagePublication &publication, std::string *error = nullptr);

  std::optional<uint32_t> residentSlot(const MapExportStreamKey &key) const;
  std::optional<uint32_t> bestResidentSlot(const std::string &field, MapExportPageAddress address) const;
  uint64_t revision() const;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace procedural::world
