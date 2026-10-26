#include "procedural/TerrainPackage.hpp"

#include "lz4.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <ios>
#include <limits>
#include <sstream>
#include <vector>

namespace procedural
{
namespace
{

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr) *error = message;
  return false;
}

uint64_t fnv1a(const uint64_t state, const uint8_t byte)
{
  return (state ^ byte) * 1099511628211ull;
}

uint64_t configurationHash(
    const PolygonTerrainSettings &terrain, const PolygonTerrainRasterSettings &raster,
    const TerrainPackageSettings &package)
{
  std::ostringstream canonical;
  canonical << std::setprecision(17) << terrain.widthMetres << ',' << terrain.heightMetres << ','
            << terrain.siteRadiusMetres << ',' << terrain.seed << ',' << terrain.riverSourceCount << ','
            << raster.width << ',' << raster.height << ',' << package.tileSizePixels << ','
            << package.voxelSizeMetres << ',' << package.heightRangeMetres;
  uint64_t hash = 1469598103934665603ull;
  for (const char character : canonical.str()) hash = fnv1a(hash, static_cast<uint8_t>(character));
  return hash;
}

std::string tileStem(const uint32_t column, const uint32_t row)
{
  std::ostringstream stream;
  stream << "x_" << std::setw(5) << std::setfill('0') << column << "_z_" << std::setw(5)
         << std::setfill('0') << row;
  return stream.str();
}

void appendU32(std::vector<uint8_t> &bytes, const uint32_t value)
{
  bytes.push_back(static_cast<uint8_t>(value & 0xffu));
  bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
  bytes.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
  bytes.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
}

uint32_t checksumFnv1a32(const std::vector<uint8_t> &bytes)
{
  uint32_t hash = 2166136261u;
  for (const uint8_t byte : bytes) hash = (hash ^ byte) * 16777619u;
  return hash;
}

struct PackedTile
{
  uint32_t width = 0u;
  uint32_t height = 0u;
  uint32_t heightOffset = 0u;
  uint32_t materialOffset = 0u;
  uint32_t regionOffset = 0u;
  std::vector<uint8_t> uncompressed;
  std::vector<uint8_t> compressed;
  uint32_t checksum = 0u;
};

bool packTile(
    const PolygonTerrainMaps &maps, const uint32_t originX, const uint32_t originY,
    const uint32_t interiorWidth, const uint32_t interiorHeight, PackedTile &tile,
    std::string *error)
{
  tile = {};
  tile.width = interiorWidth + 2u;
  tile.height = interiorHeight + 2u;
  const uint64_t texelCount = static_cast<uint64_t>(tile.width) * tile.height;
  const uint64_t heightBytes = texelCount * 2u;
  const uint64_t materialBytes = texelCount;
  const uint64_t regionBytes = texelCount * sizeof(uint32_t);
  const uint64_t headerBytes = 10u * sizeof(uint32_t);
  if (headerBytes + heightBytes + materialBytes + regionBytes > static_cast<uint64_t>(std::numeric_limits<int>::max()))
    return fail(error, "Terrain tile is too large for one LZ4 block");

  tile.heightOffset = static_cast<uint32_t>(headerBytes);
  tile.materialOffset = static_cast<uint32_t>(headerBytes + heightBytes);
  tile.regionOffset = static_cast<uint32_t>(headerBytes + heightBytes + materialBytes);
  tile.uncompressed.reserve(static_cast<size_t>(headerBytes + heightBytes + materialBytes + regionBytes));
  appendU32(tile.uncompressed, 0x52524554u); // TERR
  appendU32(tile.uncompressed, 2u);
  appendU32(tile.uncompressed, tile.width);
  appendU32(tile.uncompressed, tile.height);
  appendU32(tile.uncompressed, tile.heightOffset);
  appendU32(tile.uncompressed, tile.materialOffset);
  appendU32(tile.uncompressed, tile.regionOffset);
  appendU32(tile.uncompressed, static_cast<uint32_t>(heightBytes));
  appendU32(tile.uncompressed, static_cast<uint32_t>(materialBytes));
  appendU32(tile.uncompressed, static_cast<uint32_t>(regionBytes));

  for (uint32_t y = 0u; y < tile.height; ++y)
  {
    const uint32_t sourceY = std::min(maps.height - 1u, originY + (y == 0u ? 0u : y - 1u));
    for (uint32_t x = 0u; x < tile.width; ++x)
    {
      const uint32_t sourceX = std::min(maps.width - 1u, originX + (x == 0u ? 0u : x - 1u));
      const uint16_t value = maps.heightmap[static_cast<size_t>(sourceY) * maps.width + sourceX];
      tile.uncompressed.push_back(static_cast<uint8_t>(value & 0xffu));
      tile.uncompressed.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
    }
  }
  for (uint32_t y = 0u; y < tile.height; ++y)
  {
    const uint32_t sourceY = std::min(maps.height - 1u, originY + (y == 0u ? 0u : y - 1u));
    for (uint32_t x = 0u; x < tile.width; ++x)
    {
      const uint32_t sourceX = std::min(maps.width - 1u, originX + (x == 0u ? 0u : x - 1u));
      tile.uncompressed.push_back(maps.materials[static_cast<size_t>(sourceY) * maps.width + sourceX]);
    }
  }
  for (uint32_t y = 0u; y < tile.height; ++y)
  {
    const uint32_t sourceY = std::min(maps.height - 1u, originY + (y == 0u ? 0u : y - 1u));
    for (uint32_t x = 0u; x < tile.width; ++x)
    {
      const uint32_t sourceX = std::min(maps.width - 1u, originX + (x == 0u ? 0u : x - 1u));
      appendU32(tile.uncompressed, maps.sourceRegions[static_cast<size_t>(sourceY) * maps.width + sourceX]);
    }
  }
  tile.checksum = checksumFnv1a32(tile.uncompressed);

  const int uncompressedSize = static_cast<int>(tile.uncompressed.size());
  const int compressedBound = LZ4_compressBound(uncompressedSize);
  tile.compressed.resize(static_cast<size_t>(compressedBound));
  const int compressedSize = LZ4_compress_default(
      reinterpret_cast<const char *>(tile.uncompressed.data()),
      reinterpret_cast<char *>(tile.compressed.data()),
      uncompressedSize,
      compressedBound);
  if (compressedSize <= 0) return fail(error, "Could not LZ4-compress terrain tile");
  tile.compressed.resize(static_cast<size_t>(compressedSize));
  return true;
}

bool writeCompressedTile(const std::filesystem::path &path, const PackedTile &tile)
{
  std::ofstream file(path, std::ios::binary);
  if (!file) return false;
  file.write(
      reinterpret_cast<const char *>(tile.compressed.data()),
      static_cast<std::streamsize>(tile.compressed.size()));
  return static_cast<bool>(file);
}

bool writeManifest(
    const std::filesystem::path &path, const PolygonTerrainSettings &terrain,
    const PolygonTerrainRasterSettings &raster, const TerrainPackageSettings &package,
    const uint32_t columns, const uint32_t rows, const std::vector<PackedTile> &tiles)
{
  std::ofstream file(path);
  if (!file) return false;
  file << std::setprecision(17)
       << "{\n"
       << "  \"format\": \"polygon-terrain-package\",\n"
       << "  \"version\": 2,\n"
       << "  \"configurationHash\": \"0x" << std::hex << configurationHash(terrain, raster, package) << std::dec << "\",\n"
       << "  \"world\": {\"widthMetres\": " << terrain.widthMetres
       << ", \"heightMetres\": " << terrain.heightMetres << "},\n"
       << "  \"raster\": {\"width\": " << raster.width << ", \"height\": " << raster.height
       << ", \"voxelSizeMetres\": " << package.voxelSizeMetres
       << ", \"heightRangeMetres\": " << package.heightRangeMetres << "},\n"
       << "  \"tiles\": {\"interiorSizePixels\": " << package.tileSizePixels
       << ", \"apronPixels\": 1, \"columns\": " << columns << ", \"rows\": " << rows
       << ", \"compression\": \"lz4-block\"},\n"
       << "  \"channels\": {\n"
       << "    \"height\": {\"format\": \"r16-unorm-le\"},\n"
       << "    \"material\": {\"format\": \"r8-uint\"},\n"
       << "    \"sourceRegion\": {\"format\": \"r32-uint-le\"}\n"
       << "  },\n"
       << "  \"tileRecords\": [\n";
  for (uint32_t row = 0u; row < rows; ++row)
    for (uint32_t column = 0u; column < columns; ++column)
    {
      const size_t index = static_cast<size_t>(row) * columns + column;
      const PackedTile &tile = tiles[index];
      file << "    {\"column\": " << column << ", \"row\": " << row
           << ", \"path\": \"tiles/" << tileStem(column, row) << ".terrain-tile.lz4\""
           << ", \"width\": " << tile.width << ", \"height\": " << tile.height
           << ", \"heightOffset\": " << tile.heightOffset
           << ", \"materialOffset\": " << tile.materialOffset
           << ", \"regionOffset\": " << tile.regionOffset
           << ", \"uncompressedBytes\": " << tile.uncompressed.size()
           << ", \"compressedBytes\": " << tile.compressed.size()
           << ", \"checksumFnv1a32\": \"0x" << std::hex << std::setw(8) << std::setfill('0')
           << tile.checksum << std::dec << std::setfill(' ') << "\"}";
      if (index + 1u != tiles.size()) file << ',';
      file << '\n';
    }
  file << "  ]\n"
       << "}\n";
  return static_cast<bool>(file);
}

} // namespace

bool writeTerrainPackage(
    const std::filesystem::path &outputDirectory, const PolygonTerrainSettings &terrainSettings,
    const PolygonTerrainRasterSettings &rasterSettings, const PolygonTerrainMaps &maps,
    const TerrainPackageSettings &packageSettings, TerrainPackageStatistics *statistics,
    std::string *error)
{
  if (maps.width != rasterSettings.width || maps.height != rasterSettings.height ||
      maps.heightmap.size() != static_cast<size_t>(maps.width) * maps.height ||
      maps.materials.size() != maps.heightmap.size() || maps.sourceRegions.size() != maps.heightmap.size())
    return fail(error, "Terrain maps do not match package raster dimensions");
  if (packageSettings.tileSizePixels == 0u || packageSettings.voxelSizeMetres <= 0.0 ||
      packageSettings.heightRangeMetres <= 0.0)
    return fail(error, "Terrain package tile size, voxel size, and height range must be positive");

  const uint32_t columns = (maps.width + packageSettings.tileSizePixels - 1u) / packageSettings.tileSizePixels;
  const uint32_t rows = (maps.height + packageSettings.tileSizePixels - 1u) / packageSettings.tileSizePixels;
  std::error_code filesystemError;
  const std::filesystem::path tileDirectory = outputDirectory / "tiles";
  std::filesystem::create_directories(tileDirectory, filesystemError);
  if (filesystemError) return fail(error, "Could not create terrain package tile directory");

  TerrainPackageStatistics result{};
  result.tileColumns = columns;
  result.tileRows = rows;
  std::vector<PackedTile> packedTiles(static_cast<size_t>(columns) * rows);
  for (uint32_t row = 0u; row < rows; ++row)
    for (uint32_t column = 0u; column < columns; ++column)
    {
      const uint32_t originX = column * packageSettings.tileSizePixels;
      const uint32_t originY = row * packageSettings.tileSizePixels;
      const uint32_t width = std::min(packageSettings.tileSizePixels, maps.width - originX);
      const uint32_t height = std::min(packageSettings.tileSizePixels, maps.height - originY);
      const std::string stem = tileStem(column, row);
      PackedTile &tile = packedTiles[static_cast<size_t>(row) * columns + column];
      if (!packTile(maps, originX, originY, width, height, tile, error) ||
          !writeCompressedTile(tileDirectory / (stem + ".terrain-tile.lz4"), tile))
        return fail(error, error != nullptr && !error->empty() ? *error : "Could not write terrain package tile");
      const uint64_t texelCount = static_cast<uint64_t>(width + 2u) * (height + 2u);
      result.heightBytes += texelCount * 2u;
      result.materialBytes += texelCount;
      result.regionBytes += texelCount * sizeof(uint32_t);
      result.uncompressedBytes += static_cast<uint64_t>(tile.uncompressed.size());
      result.compressedBytes += static_cast<uint64_t>(tile.compressed.size());
    }
  if (!writeManifest(outputDirectory / "terrain-package.json", terrainSettings, rasterSettings, packageSettings, columns, rows, packedTiles))
    return fail(error, "Could not write terrain package manifest");
  if (statistics != nullptr) *statistics = result;
  return true;
}

} // namespace procedural
