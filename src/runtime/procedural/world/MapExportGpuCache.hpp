#pragma once

#include "procedural/world/MapExportPageCache.hpp"
#include "rendering/gpu/RHI.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace procedural::world {

struct MapExportGpuField
{
  std::string name;
  MapExportFormat format = MapExportFormat::R8Uint;
  MapExportFilter filter = MapExportFilter::Nearest;
  uint32_t texelBytes = 0u;
  uint32_t pageTableOffset = 0u;
};

// Word-addressed storage-buffer contract consumed by map_export_pages.wgsl.
// The header occupies eight words and every field descriptor twelve words.
struct MapExportGpuHeader
{
  uint32_t pageSizePixels = 0u;
  uint32_t gutterPixels = 0u;
  uint32_t storedPageSide = 0u;
  uint32_t maximumPageLevel = 0u;
  uint32_t physicalSlotBytes = 0u;
  uint32_t entriesPerField = 0u;
  uint32_t fieldCount = 0u;
  uint32_t resolution = 0u;
};

struct MapExportGpuFieldDescriptor
{
  uint32_t format = 0u;
  uint32_t filter = 0u;
  uint32_t texelBytes = 0u;
  uint32_t pageTableOffset = 0u;
  std::array<float, 4> scale{};
  std::array<float, 4> offset{};
};

static_assert(sizeof(MapExportGpuHeader) == 8u * sizeof(uint32_t));
static_assert(sizeof(MapExportGpuFieldDescriptor) == 12u * sizeof(uint32_t));

struct MapExportGpuPhysicalWrite
{
  MapExportStreamKey key;
  uint32_t slot = 0u;
  size_t byteOffset = 0u;
  size_t byteSize = 0u;
};

struct MapExportGpuPageTableDirtyRange
{
  uint32_t first = 0u;
  uint32_t count = 0u;

  bool valid() const
  {
    return count != 0u;
  }
};

std::vector<MapExportGpuPageTableDirtyRange> mapExportGpuPageTableDirtyRanges(const std::vector<uint32_t> &previous, const std::vector<uint32_t> &next);

struct MapExportGpuPublication
{
  uint64_t revision = 0u;
  std::vector<MapExportGpuPhysicalWrite> physicalWrites;
  // Dense field-major quadtree. Values are physical slot + 1; zero is missing.
  std::vector<uint32_t> pageTable;
};

// Renderer-independent encoder and validator for the actual GPU layout.
class MapExportGpuPageLayout
{
public:
  bool initialize(const MapExportBundle &manifest, uint32_t slotCapacity, std::string *error = nullptr);
  void clear();

  bool encode(const MapExportPagePublication &publication, MapExportGpuPublication &encoded, std::string *error = nullptr) const;

  const std::vector<MapExportGpuField> &fields() const
  {
    return fields_;
  }

  uint32_t slotCapacity() const
  {
    return slotCapacity_;
  }

  size_t physicalSlotBytes() const
  {
    return physicalSlotBytes_;
  }

  size_t physicalBufferBytes() const
  {
    return physicalSlotBytes_ * slotCapacity_;
  }

  uint32_t entriesPerField() const
  {
    return entriesPerField_;
  }

  const MapExportGpuHeader &header() const
  {
    return header_;
  }

  const std::vector<MapExportGpuFieldDescriptor> &descriptors() const
  {
    return descriptors_;
  }

  std::optional<uint32_t> fieldIndex(const std::string &name) const;
  std::optional<uint32_t> pageTableIndex(uint32_t fieldIndex, const MapExportPageAddress &address) const;

private:
  const MapExportBundle *manifest_ = nullptr;
  std::vector<MapExportGpuField> fields_;
  MapExportGpuHeader header_{};
  std::vector<MapExportGpuFieldDescriptor> descriptors_;
  uint32_t slotCapacity_ = 0u;
  uint32_t entriesPerField_ = 0u;
  size_t physicalSlotBytes_ = 0u;
};

// RHI owner for one frame-safe physical pool. apply() must be called only when
// this cache's buffers are not in use by the GPU (or on a per-frame replicated
// instance whose fence has completed). It uploads physical bytes first and the
// indirection table last.
class MapExportGpuPageCache
{
public:
  explicit MapExportGpuPageCache(::rendering::RHI &rhi);
  ~MapExportGpuPageCache();

  MapExportGpuPageCache(const MapExportGpuPageCache &) = delete;
  MapExportGpuPageCache &operator=(const MapExportGpuPageCache &) = delete;

  bool initialize(const MapExportBundle &manifest, uint32_t slotCapacity, std::string *error = nullptr);
  void clear();
  bool apply(const MapExportPagePublication &publication, std::string *error = nullptr);

  ::rendering::BufferId physicalBuffer() const
  {
    return physicalBuffer_;
  }

  ::rendering::BufferId pageTableBuffer() const
  {
    return pageTableBuffer_;
  }

  ::rendering::BufferId metadataBuffer() const
  {
    return metadataBuffer_;
  }

  const MapExportGpuPageLayout &layout() const
  {
    return layout_;
  }

  uint64_t publishedRevision() const
  {
    return publishedRevision_;
  }

private:
  ::rendering::RHI &rhi_;
  MapExportGpuPageLayout layout_;
  ::rendering::BufferId physicalBuffer_ = ::rendering::BufferId::Invalid;
  ::rendering::BufferId pageTableBuffer_ = ::rendering::BufferId::Invalid;
  ::rendering::BufferId metadataBuffer_ = ::rendering::BufferId::Invalid;
  std::vector<uint32_t> pageTableShadow_{};
  uint64_t publishedRevision_ = 0u;
};

} // namespace procedural::world
