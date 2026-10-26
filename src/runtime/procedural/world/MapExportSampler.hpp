#pragma once

#include "procedural/world/MapExport.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace procedural::world {

// Result of sampling one ordinary export field. Values are decoded into the
// physical units declared by the field's scale and offset. requestedPage is
// the page selected at requestedLevel; resolvedPage may be one of its ancestors
// when the requested tile is not resident.
struct MapExportFieldSample
{
  std::array<double, 4> values{};
  uint32_t componentCount = 0u;
  MapExportPageAddress requestedPage{};
  MapExportPageAddress resolvedPage{};

  bool usedAncestor() const
  {
    return !(requestedPage == resolvedPage);
  }
};

// Sparse biome mixtures are a semantic value, not two independently
// filterable RGBA textures. The sampler merges equal IDs from the neighboring
// texels, retains the strongest four, and returns normalized floating weights.
struct MapExportBiomeSample
{
  std::array<uint8_t, 4> ids{};
  std::array<double, 4> weights{};
  uint32_t count = 0u;
  MapExportPageAddress requestedPage{};
  MapExportPageAddress resolvedPage{};

  uint8_t dominantBiome() const
  {
    return ids[0];
  }

  // Selects one contributor from the normalized sparse mixture. `unitSample`
  // is deliberately caller-provided so terrain generation can hash a stable
  // world-space column instead of introducing mutable random state.
  uint8_t selectBiome(double unitSample) const
  {
    if (count == 0u)
    {
      return ids[0];
    }
    const double selected = std::clamp(unitSample, 0.0, 1.0);
    double cumulative = 0.0;
    for (uint32_t slot = 0u; slot < count; ++slot)
    {
      cumulative += weights[slot];
      if (selected < cumulative || slot + 1u == count)
      {
        return ids[slot];
      }
    }
    return ids[count - 1u];
  }

  bool usedAncestor() const
  {
    return !(requestedPage == resolvedPage);
  }
};

// Selects the quadtree page containing normalized map coordinates. Coordinates
// are clamped to the map extent, so the address is always valid when the level
// is valid.
bool mapExportPageAt(const MapExportBundle &bundle, double u, double v, uint8_t level, MapExportPageAddress &address);

// Samples a nearest or linear field at normalized map coordinates. Sparse
// mixture fields must be sampled with sampleMapExportBiome instead.
bool sampleMapExportField(const MapExportBundle &bundle, const std::string &fieldName, double u, double v, uint8_t requestedLevel, MapExportFieldSample &sample, std::string *error = nullptr);

// Samples the paired biome_mix_ids / biome_mix_weights fields. Both pages are
// resolved to the same ancestor before filtering, preventing mismatched LODs.
bool sampleMapExportBiome(const MapExportBundle &bundle, double u, double v, uint8_t requestedLevel, MapExportBiomeSample &sample, std::string *error = nullptr);

// World-space conveniences for the map's [0, extentMetres] domain.
bool sampleMapExportFieldAtWorld(const MapExportBundle &bundle, const std::string &fieldName, Point2 position, uint8_t requestedLevel, MapExportFieldSample &sample, std::string *error = nullptr);

bool sampleMapExportBiomeAtWorld(const MapExportBundle &bundle, Point2 position, uint8_t requestedLevel, MapExportBiomeSample &sample, std::string *error = nullptr);

// A field and page resolution held across a batch of samples.
//
// The plain entry points resolve both from scratch on every sample: the field
// by a linear scan over the bundle comparing names, then the page by a linear
// scan over that field's pages, repeated for each level `bestPage` has to walk
// up. Terrain generation samples roughly ten thousand positions per page from a
// dozen fields, and every one of those positions lies inside the same export
// page, so the resolution is identical every time and dominates the cost of the
// filter it precedes. A cursor keeps it. Reuse is exact: the cached page is
// taken only when the requested address matches the one that produced it.
//
// Cursors hold raw pointers into the bundle, so one must not outlive the bundle
// or survive a change to its resident page set. They are not thread safe; give
// each worker its own.
struct MapExportFieldCursor
{
  const MapExportField *field = nullptr;
  const MapExportPage *page = nullptr;
  MapExportPageAddress requested{};
  bool resolved = false;
};

// Biome mixtures need the id and weight fields together, so their cursor holds
// both pages.
struct MapExportBiomeCursor
{
  const MapExportField *ids = nullptr;
  const MapExportField *weights = nullptr;
  const MapExportPage *idPage = nullptr;
  const MapExportPage *weightPage = nullptr;
  MapExportPageAddress requested{};
  bool resolved = false;
};

// Interpolate contributions of categorical R8 IDs, never their numeric IDs.
// Hermite weights give a continuous height and slope across label boundaries.
struct MapExportCategorySample
{
  std::array<uint8_t, 4> ids{};
  std::array<double, 4> weights{};
  uint32_t count = 0u;
};

bool sampleMapExportCategoryAtWorld(
    const MapExportBundle &bundle,
    const std::string &fieldName,
    MapExportFieldCursor &cursor,
    Point2 position,
    uint8_t requestedLevel,
    MapExportCategorySample &sample,
    std::string *error = nullptr);

bool sampleMapExportFieldCached(
    const MapExportBundle &bundle,
    const std::string &fieldName,
    MapExportFieldCursor &cursor,
    double u,
    double v,
    uint8_t requestedLevel,
    MapExportFieldSample &sample,
    std::string *error = nullptr);

bool sampleMapExportBiomeCached(const MapExportBundle &bundle, MapExportBiomeCursor &cursor, double u, double v, uint8_t requestedLevel, MapExportBiomeSample &sample, std::string *error = nullptr);

bool sampleMapExportFieldAtWorld(
    const MapExportBundle &bundle,
    const std::string &fieldName,
    MapExportFieldCursor &cursor,
    Point2 position,
    uint8_t requestedLevel,
    MapExportFieldSample &sample,
    std::string *error = nullptr);

bool sampleMapExportBiomeAtWorld(const MapExportBundle &bundle, MapExportBiomeCursor &cursor, Point2 position, uint8_t requestedLevel, MapExportBiomeSample &sample, std::string *error = nullptr);

} // namespace procedural::world
