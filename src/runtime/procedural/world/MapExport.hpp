#pragma once

#include "procedural/world/MapClimate.hpp"
#include "procedural/world/MapCoast.hpp"
#include "procedural/world/MapContours.hpp"
#include "procedural/world/MapRegions.hpp"
#include "procedural/world/MapSurface.hpp"
#include "procedural/world/land/Blend.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world
{

enum class MapExportFormat : uint8_t
{
  R8Uint,
  R16Float,
  R16Unorm,
  R32Uint,
  RG16Float,
  RGBA8Uint,
  RGBA8Unorm,
};

enum class MapExportFilter : uint8_t
{
  Nearest,
  Linear,
  // Four sparse IDs and their four companion weights must be sampled and
  // merged together; filtering either texture independently is invalid. Fetch
  // neighboring pairs, multiply weights by spatial interpolation coefficients,
  // accumulate equal IDs, then renormalize.
  SparseMixture,
};

enum class MapExportCompression : uint8_t
{
  None,
  Lz4,
};

const char *mapExportFormatName(MapExportFormat format);
const char *mapExportFilterName(MapExportFilter filter);
const char *mapExportCompressionName(MapExportCompression compression);
uint32_t mapExportTexelBytes(MapExportFormat format);
uint32_t mapExportComponentCount(MapExportFormat format);

struct MapExportPageAddress
{
  uint8_t level = 0u;
  uint32_t x = 0u;
  uint32_t y = 0u;
};

bool operator==(const MapExportPageAddress &left, const MapExportPageAddress &right);
bool validMapExportPageAddress(const MapExportPageAddress &address, uint8_t maximumLevel);
MapExportPageAddress mapExportPageParent(const MapExportPageAddress &address);
uint64_t mapExportPageMorton(const MapExportPageAddress &address);

struct MapExportPage
{
  MapExportPageAddress address{};
  std::string file;
  MapExportCompression compression = MapExportCompression::None;
  uint32_t width = 0u;
  uint32_t height = 0u;
  uint64_t contentHash = 0u;
  uint64_t storedHash = 0u;
  std::vector<uint8_t> data;
  std::vector<uint8_t> storedData;
};

struct MapExportField
{
  std::string name;
  MapExportFormat format = MapExportFormat::R8Uint;
  MapExportFilter filter = MapExportFilter::Nearest;
  uint32_t width = 0u;
  uint32_t height = 0u;
  bool available = false;
  std::vector<std::string> components;
  std::vector<std::string> units;
  // Non-empty for fields that form one semantic sampling unit. Currently the
  // biome ID/weight pair uses "biome_mixture".
  std::string samplingGroup;
  // Decoded component = stored component * scale + offset. Unused entries are
  // one and zero. This lets normalized half/unorm maps retain physical units.
  std::array<double, 4> scale{1.0, 1.0, 1.0, 1.0};
  std::array<double, 4> offset{0.0, 0.0, 0.0, 0.0};
  uint64_t contentHash = 0u;
  // Reconstructed full-resolution GPU-ready pixels. The on-disk transport is
  // the independently compressed page pyramid below, not a duplicate texture.
  std::vector<uint8_t> data;
  std::vector<MapExportPage> pages;

  const MapExportPage *page(const MapExportPageAddress &address) const;
  // Resolves a missing request to its nearest stored ancestor. Level zero is
  // required for every available field, making fallback total.
  const MapExportPage *bestPage(MapExportPageAddress address) const;
};

struct MapExportSettings
{
  uint32_t resolution = 1024u;
  bool compressFields = true;
  uint32_t pageSizePixels = 256u;
  uint16_t gutterPixels = 1u;
};

struct MapExportBundle
{
  // Version 4 adds paired sparse biome-mixture fields and the stable
  // placement/substrate semantic field contract. Version 3 introduced the
  // planar page pyramid.
  uint32_t schemaVersion = 4u;
  uint32_t resolution = 0u;
  uint32_t pageSizePixels = 0u;
  uint16_t gutterPixels = 0u;
  uint8_t maximumPageLevel = 0u;
  double extentMetres = 0.0;
  uint64_t seed = 0u;
  uint64_t graphSignature = 0u;
  uint64_t bundleSignature = 0u;
  std::vector<uint8_t> graphData;
  std::vector<uint8_t> featureData;
  std::vector<MapExportField> fields;
  std::string manifestJson;

  const MapExportField *field(const std::string &name) const;
  MapExportField *field(const std::string &name);
  bool validate(std::string *error = nullptr) const;
};

struct MapExportFeatures
{
  std::vector<CoastPolyline> coastline;
  RiverNetwork rivers;
  ContourSet contours;
  RegionMap regions;
};

// Builds the map-stage L0 export. The graph and vector features remain
// authoritative; the registered rasters are deterministic runtime caches.
// Planned fields without a producer are present with available=false and no
// payload, allowing a loader to bind a shared zero texture without confusing
// "not generated" with meaningful all-zero data.
bool buildMapExportBundle(
    const MapGraph &graph,
    const SiteIndex &index,
    const MapSurfaceField &surface,
    const ClimateMap &climate,
    const RiverNetwork &rivers,
    const std::vector<CoastPolyline> &coastline,
    const ContourSet &contours,
    const RegionMap &regions,
    const BlendSettings &blend,
    const HeightBlendSettings &heightBlend,
    const MapExportSettings &settings,
    MapExportBundle &bundle,
    std::string *error = nullptr);

// Writes payloads first and manifest.json last, so the manifest is the commit
// marker for a complete bundle. Existing unrelated files are not removed.
bool writeMapExportBundle(const MapExportBundle &bundle, const std::string &directory, std::string *error = nullptr);

// Reads only manifest metadata. Page payload vectors and reconstructed fields
// remain empty so MapExportPageCache can perform genuinely on-demand I/O.
bool readMapExportManifest(const std::string &directory, MapExportBundle &bundle, std::string *error = nullptr);

// Loads, decompresses and validates a bundle without decoding WMG1/WMF1 into
// live graph objects. Field data is always returned in its GPU-ready form while
// each page retains its independently compressed bytes for streaming/upload.
bool readMapExportBundle(const std::string &directory, MapExportBundle &bundle, std::string *error = nullptr);
// Drops the two transport copies a loaded bundle carries but no runtime
// consumer reads: every field's reconstructed full-resolution `data` (the
// sampler and the GPU atlas read pages) and every page's compressed
// `storedData` (only re-export and validate use it). 0.78 GiB -> 0.4 GiB for
// the 26-field, 2,180-page world export. Not for bundles that will be written
// or validated again.
void releaseMapExportBundleTransportCopies(MapExportBundle &bundle);

// Reads and validates one decoded page without reconstructing its complete
// field. Streaming workers use this after a manifest has established the page
// metadata, avoiding a whole-bundle read for every residency miss.
bool readMapExportPage(const std::string &directory, const MapExportField &field, const MapExportPage &page, std::vector<uint8_t> &decoded, std::string *error = nullptr);

// Decodes the canonical semantic payloads after transport validation. Both
// functions reject truncated data, invalid enum values, unreasonable counts,
// trailing bytes and structural/signature mismatches.
bool decodeMapExportGraph(const MapExportBundle &bundle, MapGraph &graph, std::string *error = nullptr);

bool decodeMapExportFeatures(const MapExportBundle &bundle, MapExportFeatures &features, std::string *error = nullptr);

} // namespace procedural::world
