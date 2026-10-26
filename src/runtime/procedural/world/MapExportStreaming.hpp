#pragma once

#include "procedural/world/MapExport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world
{

struct MapExportStreamKey
{
  std::string field;
  MapExportPageAddress address{};
};

bool operator==(const MapExportStreamKey &left, const MapExportStreamKey &right);

struct MapExportStreamRequest
{
  // min u, min v, max u, max v. Reversed and out-of-range bounds are
  // normalized and clamped; non-finite bounds are rejected.
  std::array<double, 4> normalizedBounds{0.0, 0.0, 1.0, 1.0};
  uint8_t level = 0u;
  uint32_t guardPages = 0u;
  size_t byteBudget = 64u * 1024u * 1024u;
  std::vector<std::string> fields;
};

struct MapExportStreamUpdate
{
  std::vector<MapExportStreamKey> loads;
  std::vector<MapExportStreamKey> evictions;
  size_t residentBytes = 0u;
  size_t residentPages = 0u;
  size_t requestedUnits = 0u;
  size_t deferredUnits = 0u;
  size_t ancestorUnits = 0u;
};

// Deterministic residency planner for decoded export pages. The bundle must
// outlive the cache. It does not perform I/O or GPU writes; loads and evictions
// are the exact transaction a renderer/streamer should apply at a safe frame
// boundary. Sampling-group pages are selected and evicted atomically.
class MapExportStreamingCache
{
public:
  bool initialize(const MapExportBundle &bundle, std::string *error = nullptr);
  void clear();

  bool update(const MapExportStreamRequest &request, MapExportStreamUpdate &update, std::string *error = nullptr);

  bool resident(const MapExportStreamKey &key) const;
  const MapExportPage *page(const MapExportStreamKey &key) const;
  const MapExportPage *bestResidentPage(const std::string &field, MapExportPageAddress address) const;

  size_t residentBytes() const
  {
    return residentBytes_;
  }

  const std::vector<MapExportStreamKey> &residentPages() const
  {
    return resident_;
  }

private:
  const MapExportBundle *bundle_ = nullptr;
  std::vector<MapExportStreamKey> resident_;
  size_t residentBytes_ = 0u;
};

} // namespace procedural::world
