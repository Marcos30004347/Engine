#include "TerrainGenerationPhase0Utils.hpp"

#include "procedural/world/WorldBrief.hpp"
#include "voxel/svraster/SparseVoxelGenerator.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <sstream>
#include <tuple>
#include <utility>

namespace terrain_phase0
{
namespace
{
using procedural::world::CellType;
using procedural::world::MapExportBundle;
using procedural::world::MapExportField;
using procedural::world::MapExportFilter;
using procedural::world::MapExportFormat;
using procedural::world::MapExportPage;
using voxel::svraster::PlanetVoxelChunkSet;

constexpr uint32_t kFixtureResolution = 64u;
constexpr uint16_t kFixtureGutter = 1u;
constexpr double kFixtureExtentMetres = 256.0;

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

void appendU16(std::vector<uint8_t> &bytes, const uint16_t value)
{
  bytes.push_back(static_cast<uint8_t>(value & 0xffu));
  bytes.push_back(static_cast<uint8_t>(value >> 8u));
}

uint16_t unorm16(const double value)
{
  return static_cast<uint16_t>(std::llround(std::clamp(value, 0.0, 1.0) * 65535.0));
}

MapExportField
field(const std::string &name, const MapExportFormat format, const MapExportFilter filter, const std::function<void(uint32_t, uint32_t, std::vector<uint8_t> &)> &texel, const std::string &samplingGroup = {})
{
  MapExportField result{};
  result.name = name;
  result.format = format;
  result.filter = filter;
  result.width = kFixtureResolution;
  result.height = kFixtureResolution;
  result.available = true;
  result.samplingGroup = samplingGroup;
  MapExportPage page{};
  page.address = {0u, 0u, 0u};
  page.width = kFixtureResolution + kFixtureGutter * 2u;
  page.height = page.width;
  for (uint32_t y = 0u; y < page.height; ++y)
    for (uint32_t x = 0u; x < page.width; ++x)
    {
      const uint32_t sourceX = static_cast<uint32_t>(std::clamp(static_cast<int32_t>(x) - static_cast<int32_t>(kFixtureGutter), 0, static_cast<int32_t>(kFixtureResolution) - 1));
      const uint32_t sourceY = static_cast<uint32_t>(std::clamp(static_cast<int32_t>(y) - static_cast<int32_t>(kFixtureGutter), 0, static_cast<int32_t>(kFixtureResolution) - 1));
      texel(sourceX, sourceY, page.data);
    }
  result.pages.push_back(std::move(page));
  return result;
}

MapExportField constantField(const std::string &name, const MapExportFormat format, const MapExportFilter filter, const std::vector<uint8_t> &value, const std::string &samplingGroup = {})
{
  return field(
      name,
      format,
      filter,
      [value](uint32_t, uint32_t, std::vector<uint8_t> &bytes)
      {
        bytes.insert(bytes.end(), value.begin(), value.end());
      },
      samplingGroup);
}

MapExportField unormField(const std::string &name, const std::function<double(double, double)> &sample, const double scale)
{
  auto result = field(
      name,
      MapExportFormat::R16Unorm,
      MapExportFilter::Linear,
      [sample](const uint32_t x, const uint32_t y, std::vector<uint8_t> &bytes)
      {
        const double u = (static_cast<double>(x) + 0.5) / kFixtureResolution;
        const double v = (static_cast<double>(y) + 0.5) / kFixtureResolution;
        appendU16(bytes, unorm16(sample(u, v)));
      });
  result.scale[0] = scale;
  return result;
}

MapExportField byteField(const std::string &name, const std::function<uint8_t(double, double)> &sample)
{
  return field(
      name,
      MapExportFormat::R8Uint,
      MapExportFilter::Nearest,
      [sample](const uint32_t x, const uint32_t y, std::vector<uint8_t> &bytes)
      {
        const double u = (static_cast<double>(x) + 0.5) / kFixtureResolution;
        const double v = (static_cast<double>(y) + 0.5) / kFixtureResolution;
        bytes.push_back(sample(u, v));
      });
}

std::function<double(double, double)> elevationFor(const FixtureKind kind)
{
  switch (kind)
  {
  case FixtureKind::Flat:
    return [](double, double)
    {
      return 0.50;
    };
  case FixtureKind::Mountain:
  case FixtureKind::Snow:
    return [](const double u, const double v)
    {
      const double dx = (u - 0.5) * 2.0;
      const double dz = (v - 0.5) * 2.0;
      return 0.36 + std::max(0.0, 1.0 - std::sqrt(dx * dx + dz * dz)) * 0.46;
    };
  case FixtureKind::Cliff:
    return [](const double u, double)
    {
      return u < 0.5 ? 0.34 : 0.72;
    };
  case FixtureKind::Canyon:
    return [](const double u, const double v)
    {
      const double channel = std::max(0.0, 1.0 - std::abs(u - 0.5) * 10.0);
      return 0.68 - channel * 0.38 + (v - 0.5) * 0.04;
    };
  case FixtureKind::River:
    return [](const double u, const double v)
    {
      const double channel = std::max(0.0, 1.0 - std::abs(u - 0.5) * 18.0);
      return 0.56 + (v - 0.5) * 0.12 - channel * 0.08;
    };
  case FixtureKind::RoadSmoothed:
    return [](const double u, const double v)
    {
      return 0.48 + std::sin(u * 73.0) * 0.05 + std::cos(v * 61.0) * 0.04;
    };
  case FixtureKind::Coastline:
    return [](const double u, const double v)
    {
      return 0.48 + (u - 0.5) * 0.32 + std::sin(v * 18.0) * 0.015;
    };
  case FixtureKind::CrossLOD:
    return [](const double u, const double v)
    {
      return 0.50 + std::sin(u * 19.0) * 0.12 + std::cos(v * 23.0) * 0.10 + std::sin((u + v) * 11.0) * 0.04;
    };
  }
  return [](double, double)
  {
    return 0.5;
  };
}

template <typename T> uint64_t vectorBytes(const std::vector<T> &values)
{
  return uint64_t{values.size()} * sizeof(T);
}

uint32_t populationCount(uint32_t value)
{
  value -= (value >> 1u) & 0x55555555u;
  value = (value & 0x33333333u) + ((value >> 2u) & 0x33333333u);
  value = (value + (value >> 4u)) & 0x0f0f0f0fu;
  return (value * 0x01010101u) >> 24u;
}

std::string cellName(const std::array<int32_t, 3> &coordinate)
{
  return "(" + std::to_string(coordinate[0]) + "," + std::to_string(coordinate[1]) + "," + std::to_string(coordinate[2]) + ")";
}

} // namespace

const char *fixtureName(const FixtureKind kind)
{
  switch (kind)
  {
  case FixtureKind::Flat:
    return "flat";
  case FixtureKind::Mountain:
    return "mountain";
  case FixtureKind::Cliff:
    return "cliff";
  case FixtureKind::Canyon:
    return "canyon";
  case FixtureKind::River:
    return "river";
  case FixtureKind::RoadSmoothed:
    return "road-smoothed";
  case FixtureKind::Coastline:
    return "coastline";
  case FixtureKind::Snow:
    return "snow";
  case FixtureKind::CrossLOD:
    return "cross-lod";
  }
  return "unknown";
}

MapExportBundle makeFixture(const FixtureKind kind)
{
  const auto elevation = elevationFor(kind);
  MapExportBundle bundle{};
  bundle.resolution = kFixtureResolution;
  bundle.pageSizePixels = kFixtureResolution;
  bundle.gutterPixels = kFixtureGutter;
  bundle.maximumPageLevel = 0u;
  bundle.extentMetres = kFixtureExtentMetres;
  bundle.seed = 0x5f3759df12345678ull + static_cast<uint8_t>(kind);
  bundle.bundleSignature = 0x9e3779b97f4a7c15ull ^ (uint64_t{static_cast<uint8_t>(kind) + 1u} << 40u);

  const auto oneHalf = std::vector<uint8_t>{0u, 0x3cu};
  const auto climate = kind == FixtureKind::Snow ? std::vector<uint8_t>{0u, 0xc4u, 0u, 0x3cu} : std::vector<uint8_t>{0u, 0u, 0u, 0x38u};
  const auto landform = kind == FixtureKind::Mountain || kind == FixtureKind::Snow ? CellType::Mountain : kind == FixtureKind::Cliff ? CellType::Cliff : kind == FixtureKind::Canyon ? CellType::Canyon : CellType::Hill;

  bundle.fields = {
    constantField("placement_potential", MapExportFormat::RGBA8Unorm, MapExportFilter::Linear, {255u, 255u, 255u, 255u}),
    byteField(
        "water_type",
        [kind](const double u, double)
        {
          return kind == FixtureKind::Coastline && u < 0.5 ? 1u : 0u;
        }),
    unormField(
        "water_level",
        [](double, double)
        {
          return 0.48;
        },
        160.0),
    constantField("climate", MapExportFormat::RG16Float, MapExportFilter::Linear, climate),
    unormField("refined_elevation", elevation, 160.0),
    unormField("base_elevation", elevation, 160.0),
    constantField("relief_roughness", MapExportFormat::RG16Float, MapExportFilter::Linear, {0u, 0x50u, 0u, 0x38u}),
    constantField("landform_id", MapExportFormat::R8Uint, MapExportFilter::Nearest, {static_cast<uint8_t>(landform)}),
    constantField("river", MapExportFormat::RG16Float, MapExportFilter::Linear, kind == FixtureKind::River ? std::vector<uint8_t>{0u, 0u, 0u, 0x54u} : std::vector<uint8_t>{0u, 0x64u, 0u, 0u}),
    constantField("substrate_id", MapExportFormat::R8Uint, MapExportFilter::Nearest, {3u}),
    constantField("soil_depth", MapExportFormat::R16Float, MapExportFilter::Linear, oneHalf),
    constantField("biome_mix_ids", MapExportFormat::RGBA8Uint, MapExportFilter::SparseMixture, {1u, 0u, 0u, 0u}, "biome_mixture"),
    constantField("biome_mix_weights", MapExportFormat::RGBA8Unorm, MapExportFilter::SparseMixture, {255u, 0u, 0u, 0u}, "biome_mixture"),
    constantField("object_exclusion", MapExportFormat::R8Uint, MapExportFilter::Nearest, {0u}),
  };
  if (kind == FixtureKind::RoadSmoothed)
    bundle.fields.push_back(constantField("roads", MapExportFormat::R8Uint, MapExportFilter::Nearest, {1u}));
  return bundle;
}

voxel::svraster::WorldMapVoxelTerrainSettings fixtureSettings(const FixtureKind kind, const uint32_t internalLOD, const uint32_t workerCount)
{
  voxel::svraster::WorldMapVoxelTerrainSettings settings{};
  settings.normalizedBounds = {0.4375, 0.4375, 0.5625, 0.5625};
  settings.requestedLevel = 0u;
  settings.internalLOD = internalLOD;
  settings.finestHorizontalVoxelCount = 32u;
  settings.finestVoxelSizeMetres = 1.0;
  settings.elevationAnchorLOD = 2u;
  settings.generatorRevision = 13u;
  settings.exteriorShellOnly = true;
  settings.boundaryStitchSubdivisions = internalLOD == 0u ? 1u : 2u;
  settings.voxel.horizontalVoxelCount = 32u;
  settings.voxel.patchEdgeMetres = 32.0;
  settings.voxel.foundationDepthVoxels = 2u;
  settings.voxel.surfaceShellDepthVoxels = 2u;
  settings.voxel.maximumVerticalVoxels = 128u;
  settings.voxel.maximumBricks = 1u << 17u;
  settings.voxel.surfaceEnvelopeRadius = 1u;
  settings.voxel.proceduralDetailStrength = kind == FixtureKind::RoadSmoothed ? 1.0 : 0.0;
  settings.voxel.proceduralTerraceStrength = 0.0;
  settings.voxel.microGeometry = false;
  settings.voxel.workerCount = workerCount;
  return settings;
}

bool SemanticTerrainCell::operator==(const SemanticTerrainCell &other) const
{
  return coordinate == other.coordinate && material.packed == other.material.packed && context == other.context;
}

bool SemanticTerrainNode::operator==(const SemanticTerrainNode &other) const
{
  return canonicalOriginScale == other.canonicalOriginScale && occupiedCount == other.occupiedCount && selfError == other.selfError && parentError == other.parentError;
}

bool decodeSemanticTerrainPage(const PlanetVoxelChunkSet &chunks, const uint32_t chunkIndex, SemanticTerrainPage &page, std::string *error)
{
  page = {};
  if (error != nullptr)
    error->clear();
  if (chunkIndex >= chunks.chunks.size())
    return fail(error, "semantic terrain decoder needs a valid chunk");
  const auto &record = chunks.chunks[chunkIndex];
  if (!(record.voxelSizeMetres > 0.0) || !(record.edgeMetres > 0.0) || record.brickNodeOffset > chunks.bricks.nodes.size() || record.brickNodeCapacity > chunks.bricks.nodes.size() - record.brickNodeOffset)
    return fail(error, "semantic terrain decoder found invalid page ranges");

  page.edgeMetres = record.edgeMetres;
  page.voxelSizeMetres = record.voxelSizeMetres;
  page.internalLOD = record.clipmapLOD;
  page.geometryRevision = record.tree64Revision;
  const std::array<double, 3> minimum{
    record.worldCenterXMetres - record.edgeMetres * 0.5,
    record.elevationOriginMetres - chunks.elevationOriginMetres,
    record.worldCenterZMetres - record.edgeMetres * 0.5,
  };
  const uint32_t nodeEnd = record.brickNodeOffset + record.brickNodeCapacity;
  for (uint32_t nodeIndex = record.brickNodeOffset; nodeIndex < nodeEnd; ++nodeIndex)
  {
    const auto &node = chunks.bricks.nodes[nodeIndex];
    page.nodes.push_back({
      node.canonicalGridOriginScale,
      node.occupiedCount,
      node.selfError,
      node.parentError,
    });
    if (std::abs(static_cast<double>(node.minimumVoxelSize[3]) - record.voxelSizeMetres) > std::max(1.0e-6, record.voxelSizeMetres * 1.0e-5))
      continue;
    if (node.occupancyIndex >= chunks.bricks.occupancy.size())
      return fail(error, "semantic terrain decoder found invalid occupancy");
    const auto &occupancy = chunks.bricks.occupancy[node.occupancyIndex];
    for (uint32_t linearCell = 0u; linearCell < voxel::svraster::kSparseVoxelBrickVoxelCount; ++linearCell)
    {
      const uint32_t word = linearCell / 32u;
      const uint32_t bit = linearCell & 31u;
      const uint32_t bits = occupancy.cells[word].bits;
      if ((bits & (uint32_t{1u} << bit)) == 0u)
        continue;
      const uint32_t preceding = bit == 0u ? 0u : bits & ((uint32_t{1u} << bit) - 1u);
      const uint64_t materialAddress = uint64_t{node.materialBase} + occupancy.cells[word].prefix + populationCount(preceding);
      if (materialAddress >= chunks.bricks.materialIndices.size() || materialAddress >= chunks.bricks.microSurfaceData.size())
        return fail(error, "semantic terrain decoder found invalid sidecar address");
      const uint64_t paletteAddress = uint64_t{node.paletteBase} + chunks.bricks.materialIndices[materialAddress];
      if (paletteAddress >= chunks.bricks.palette.size())
        return fail(error, "semantic terrain decoder found invalid palette address");

      const std::array<uint32_t, 3> local{
        linearCell % voxel::svraster::kSparseVoxelBrickSide,
        (linearCell / voxel::svraster::kSparseVoxelBrickSide) % voxel::svraster::kSparseVoxelBrickSide,
        linearCell / (voxel::svraster::kSparseVoxelBrickSide * voxel::svraster::kSparseVoxelBrickSide),
      };
      SemanticTerrainCell cell{};
      bool coordinateValid = true;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        const double coordinate = (static_cast<double>(node.minimumVoxelSize[axis]) + static_cast<double>(local[axis]) * record.voxelSizeMetres - minimum[axis]) / record.voxelSizeMetres;
        const double rounded = std::round(coordinate);
        coordinateValid = coordinateValid && std::isfinite(coordinate) && std::abs(coordinate - rounded) <= std::max(1.0e-4, std::abs(coordinate) * 1.0e-6) && rounded >= std::numeric_limits<int32_t>::min() &&
                          rounded <= std::numeric_limits<int32_t>::max();
        cell.coordinate[axis] = static_cast<int32_t>(rounded);
      }
      if (!coordinateValid)
        return fail(error, "semantic terrain decoder found a misaligned cell");
      cell.material = chunks.bricks.palette[paletteAddress];
      cell.context = voxel::svraster::unpackSparseVoxelMicroContextValue(chunks.bricks.microSurfaceData[materialAddress]);
      page.cells.push_back(cell);
    }
  }
  const auto cellBefore = [](const auto &left, const auto &right)
  {
    return left.coordinate < right.coordinate;
  };
  std::sort(page.cells.begin(), page.cells.end(), cellBefore);
  if (std::adjacent_find(
          page.cells.begin(),
          page.cells.end(),
          [](const auto &left, const auto &right)
          {
            return left.coordinate == right.coordinate;
          }) != page.cells.end())
    return fail(error, "semantic terrain decoder found duplicate finest cells");
  std::sort(
      page.nodes.begin(),
      page.nodes.end(),
      [](const auto &left, const auto &right)
      {
        return left.canonicalOriginScale < right.canonicalOriginScale;
      });
  return !page.cells.empty();
}

bool compareSemanticTerrainPages(const SemanticTerrainPage &expected, const SemanticTerrainPage &actual, std::string *difference)
{
  if (difference != nullptr)
    difference->clear();
  if (expected.edgeMetres != actual.edgeMetres || expected.voxelSizeMetres != actual.voxelSizeMetres || expected.internalLOD != actual.internalLOD || expected.geometryRevision != actual.geometryRevision)
    return fail(difference, "terrain page identity differs");
  size_t expectedCell = 0u;
  size_t actualCell = 0u;
  while (expectedCell < expected.cells.size() || actualCell < actual.cells.size())
  {
    if (actualCell == actual.cells.size() || (expectedCell < expected.cells.size() && expected.cells[expectedCell].coordinate < actual.cells[actualCell].coordinate))
      return fail(difference, "actual occupancy is missing canonical cell " + cellName(expected.cells[expectedCell].coordinate));
    if (expectedCell == expected.cells.size() || actual.cells[actualCell].coordinate < expected.cells[expectedCell].coordinate)
      return fail(difference, "actual occupancy adds canonical cell " + cellName(actual.cells[actualCell].coordinate));
    const auto &left = expected.cells[expectedCell];
    const auto &right = actual.cells[actualCell];
    if (left.material.packed != right.material.packed)
      return fail(difference, "material differs at canonical cell " + cellName(left.coordinate));
    if (left.context != right.context)
      return fail(difference, "context differs at canonical cell " + cellName(left.coordinate));
    ++expectedCell;
    ++actualCell;
  }
  if (expected.nodes.size() != actual.nodes.size())
    return fail(difference, "brick node count differs");
  for (size_t index = 0u; index < expected.nodes.size(); ++index)
  {
    const auto &left = expected.nodes[index];
    const auto &right = actual.nodes[index];
    if (left.canonicalOriginScale != right.canonicalOriginScale)
      return fail(difference, "brick canonical address differs at node " + std::to_string(index));
    if (left.occupiedCount != right.occupiedCount)
      return fail(difference, "brick occupied count differs at node " + std::to_string(index));
    if (left.selfError != right.selfError || left.parentError != right.parentError)
      return fail(difference, "brick LOD error differs at node " + std::to_string(index));
  }
  return true;
}

bool compareRasterAndTree64Occupancy(const PlanetVoxelChunkSet &chunks, const uint32_t chunkIndex, voxel::svraster::SparseVoxelTerrainPageTree64 *tree64, std::string *difference)
{
  if (difference != nullptr)
    difference->clear();
  SemanticTerrainPage raster{};
  if (!decodeSemanticTerrainPage(chunks, chunkIndex, raster, difference))
    return false;
  const auto &record = chunks.chunks[chunkIndex];
  voxel::svraster::SparseVoxelTerrainPageTree64Source source{};
  source.bricks = &chunks.bricks;
  source.firstBrickRoot = record.firstBrickRoot;
  source.brickRootCount = record.brickRootCount;
  source.brickNodeOffset = record.brickNodeOffset;
  source.brickNodeCapacity = record.brickNodeCapacity;
  source.pageMinimumMetres = {
    record.worldCenterXMetres - record.edgeMetres * 0.5,
    record.elevationOriginMetres - chunks.elevationOriginMetres,
    record.worldCenterZMetres - record.edgeMetres * 0.5,
  };
  source.edgeMetres = record.edgeMetres;
  source.heightMetres = std::max(record.voxelSizeMetres, record.maximumSurfaceMetres - record.elevationOriginMetres + record.voxelSizeMetres);
  source.voxelSizeMetres = record.voxelSizeMetres;
  source.maximumSide = 1024u;
  voxel::svraster::SparseVoxelTerrainPageTree64 built{};
  if (!voxel::svraster::buildSparseVoxelTerrainPageTree64(source, built, difference))
    return false;
  if (built.cellScale != 1u)
    return fail(difference, "baseline Tree64 unexpectedly reduced occupancy");
  const uint64_t cellCount = uint64_t{built.side} * built.side * built.side;
  if (cellCount > std::numeric_limits<size_t>::max())
    return fail(difference, "baseline Tree64 cell lattice is too large");
  std::vector<uint8_t> rasterOccupancy(static_cast<size_t>(cellCount), 0u);
  const auto denseIndex = [&](const std::array<int32_t, 3> &coordinate)
  {
    return static_cast<size_t>(coordinate[0]) + static_cast<size_t>(built.side) * (static_cast<size_t>(coordinate[1]) + static_cast<size_t>(built.side) * static_cast<size_t>(coordinate[2]));
  };
  for (const auto &cell : raster.cells)
  {
    if (std::any_of(
            cell.coordinate.begin(),
            cell.coordinate.end(),
            [](const int32_t value)
            {
              return value < 0;
            }) ||
        std::any_of(
            cell.coordinate.begin(),
            cell.coordinate.end(),
            [&](const int32_t value)
            {
              return value >= static_cast<int32_t>(built.side);
            }))
      return fail(difference, "raster cell lies outside the Tree64 lattice at " + cellName(cell.coordinate));
    rasterOccupancy[denseIndex(cell.coordinate)] = 1u;
  }
  for (uint32_t z = 0u; z < built.side; ++z)
    for (uint32_t y = 0u; y < built.side; ++y)
      for (uint32_t x = 0u; x < built.side; ++x)
      {
        const std::array<uint32_t, 3> coordinate{x, y, z};
        const std::array<int32_t, 3> signedCoordinate{static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(z)};
        const bool rasterOccupied = rasterOccupancy[denseIndex(signedCoordinate)] != 0u;
        const bool treeOccupied = voxel::svraster::sparseVoxelTree64Occupied(built.tree, coordinate);
        if (rasterOccupied != treeOccupied)
          return fail(difference, std::string(treeOccupied ? "Tree64 adds" : "Tree64 misses") + " canonical cell " + cellName(signedCoordinate));
      }
  if (tree64 != nullptr)
    *tree64 = std::move(built);
  return true;
}

uint64_t semanticTerrainPageHash(const SemanticTerrainPage &page)
{
  uint64_t hash = 1469598103934665603ull;
  const auto add = [&](const void *data, const size_t bytes)
  {
    const auto *values = static_cast<const uint8_t *>(data);
    for (size_t index = 0u; index < bytes; ++index)
    {
      hash ^= values[index];
      hash *= 1099511628211ull;
    }
  };
  add(&page.edgeMetres, sizeof(page.edgeMetres));
  add(&page.voxelSizeMetres, sizeof(page.voxelSizeMetres));
  add(&page.internalLOD, sizeof(page.internalLOD));
  add(&page.geometryRevision, sizeof(page.geometryRevision));
  for (const auto &cell : page.cells)
  {
    add(cell.coordinate.data(), sizeof(cell.coordinate));
    add(cell.material.packed.data(), sizeof(cell.material.packed));
    add(&cell.context, sizeof(cell.context));
  }
  for (const auto &node : page.nodes)
  {
    add(node.canonicalOriginScale.data(), sizeof(node.canonicalOriginScale));
    add(&node.occupiedCount, sizeof(node.occupiedCount));
    add(&node.selfError, sizeof(node.selfError));
    add(&node.parentError, sizeof(node.parentError));
  }
  return hash == 0u ? 1u : hash;
}

uint64_t semanticTerrainRunCount(const SemanticTerrainPage &page)
{
  std::vector<std::array<int32_t, 3>> coordinates;
  coordinates.reserve(page.cells.size());
  for (const auto &cell : page.cells)
    coordinates.push_back(cell.coordinate);
  std::sort(
      coordinates.begin(),
      coordinates.end(),
      [](const auto &left, const auto &right)
      {
        return std::tie(left[0], left[2], left[1]) < std::tie(right[0], right[2], right[1]);
      });
  uint64_t runs = 0u;
  std::array<int32_t, 3> previous{};
  bool havePrevious = false;
  for (const auto &coordinate : coordinates)
  {
    if (!havePrevious || coordinate[0] != previous[0] || coordinate[2] != previous[2] || coordinate[1] != previous[1] + 1)
      ++runs;
    previous = coordinate;
    havePrevious = true;
  }
  return runs;
}

uint64_t terrainPayloadBytes(const PlanetVoxelChunkSet &chunks)
{
  return vectorBytes(chunks.bricks.nodes) + vectorBytes(chunks.bricks.occupancy) + vectorBytes(chunks.bricks.childIndices) + vectorBytes(chunks.bricks.rootIndices) + vectorBytes(chunks.bricks.materialIndices) +
         vectorBytes(chunks.bricks.microSurfaceData) + vectorBytes(chunks.bricks.palette) + vectorBytes(chunks.bricks.paletteDescriptorIds) + vectorBytes(chunks.surfaceHeights) + vectorBytes(chunks.surfaceEnvelope) +
         vectorBytes(chunks.surfaceMaterials);
}

} // namespace terrain_phase0
