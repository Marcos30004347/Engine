#include "procedural/PlanetCubeMapping.hpp"
#include "procedural/PlanetMeshExport.hpp"
#include "procedural/PlanetPackage.hpp"
#include "procedural/PolygonTerrain.hpp"
#include "procedural/SphericalTerrainPackage.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(const bool condition, const char *message)
{
  if (condition)
    std::cout << "  PASSED " << message << '\n';
  else
  {
    std::cerr << "  FAILED " << message << '\n';
    ++failures;
  }
}

uint32_t raster(const procedural::PlanetRasterChannel channel)
{
  return static_cast<uint32_t>(channel);
}

uint32_t graph(const procedural::PlanetGraphChannel channel)
{
  return static_cast<uint32_t>(channel);
}

uint16_t readU16(const std::vector<uint8_t> &bytes, const size_t offset)
{
  return static_cast<uint16_t>(bytes[offset]) |
      static_cast<uint16_t>(bytes[offset + 1u]) << 8u;
}

void testCubeQuadtreeAddresses()
{
  std::cout << "planet package cube quadtree\n";
  const procedural::PlanetTileAddress root{2u, 0u, 0u, 0u};
  const auto topRight = procedural::planetTileChild(root, 1u);
  const auto bottomRight = procedural::planetTileChild(topRight, 3u);
  check(procedural::validPlanetTileAddress(root), "cube face root is a valid address");
  check(topRight == procedural::PlanetTileAddress{2u, 1u, 1u, 0u}, "child bits subdivide a cube face in x and y");
  check(bottomRight == procedural::PlanetTileAddress{2u, 2u, 3u, 1u}, "nested children preserve stable quadtree coordinates");
  check(procedural::planetTileParent(bottomRight) == topRight, "quadtree parent is reconstructed without stored nodes");
  check(procedural::planetTileMorton({2u, 2u, 3u, 2u}) == 13u, "tile coordinates have a stable Morton key");
  check(!procedural::validPlanetTileAddress({6u, 0u, 0u, 0u}), "only six root cube faces are valid");

  const double earthRadius = 6371000.0;
  const uint8_t kilometreLevel = procedural::planetTileLevelForTargetEdge(earthRadius, 1000.0);
  check(kilometreLevel == 14u, "Earth-sized cube faces subdivide to kilometre patches near level fourteen");
  check(
      procedural::planetTileNominalEdgeMetres(earthRadius, kilometreLevel) <= 1000.0 && procedural::planetTileNominalEdgeMetres(earthRadius, kilometreLevel - 1u) > 1000.0,
      "target-edge selection returns the first sufficiently fine level");
}

void testCubeProjectionAndGutters()
{
  std::cout << "planet package cube projection\n";
  const std::vector<procedural::SpherePoint> axes{
    {1.0, 0.0, 0.0},
    {-1.0, 0.0, 0.0},
    {0.0, 1.0, 0.0},
    {0.0, -1.0, 0.0},
    {0.0, 0.0, 1.0},
    {0.0, 0.0, -1.0},
  };
  bool axesMatchFaces = true;
  for (uint8_t face = 0u; face < axes.size(); ++face)
  {
    procedural::PlanetCubeCoordinate coordinate{};
    axesMatchFaces = axesMatchFaces && procedural::planetCubeCoordinate(axes[face], coordinate) && static_cast<uint8_t>(coordinate.face) == face && coordinate.u == 0.5 && coordinate.v == 0.5;
  }
  check(axesMatchFaces, "six cardinal directions map to six cube-face centers");

  const procedural::PlanetCubeCoordinate original{procedural::PlanetCubeFace::PositiveZ, 0.27, 0.81};
  procedural::SpherePoint direction{};
  procedural::PlanetCubeCoordinate reconstructed{};
  check(
      procedural::planetCubeDirection(original, direction) && procedural::planetCubeCoordinate(direction, reconstructed) && reconstructed.face == original.face && std::abs(reconstructed.u - original.u) < 1.0e-12 &&
          std::abs(reconstructed.v - original.v) < 1.0e-12,
      "cube coordinates round-trip through a unit direction");

  procedural::PlanetTileAddress tile{};
  double tileU = 0.0;
  double tileV = 0.0;
  check(
      procedural::planetCubeTile(direction, 4u, tile, &tileU, &tileV) && tile.face == static_cast<uint8_t>(original.face) && tile.x == 4u && tile.y == 12u && tileU > 0.31 && tileU < 0.33 && tileV > 0.95 && tileV < 0.97,
      "direction lookup resolves a fine quadtree tile and local coordinates");

  procedural::SpherePoint gutterDirection{};
  procedural::PlanetCubeCoordinate gutterCoordinate{};
  check(
      procedural::planetCubeTileTexelDirection({0u, 0u, 0u, 0u}, 4u, 1u, 5u, 3u, gutterDirection) && procedural::planetCubeCoordinate(gutterDirection, gutterCoordinate) &&
          gutterCoordinate.face == procedural::PlanetCubeFace::NegativeZ,
      "a cube-edge gutter samples through the adjacent face instead of clamping");
}

void testPackageRoundTripAndSelectiveReads()
{
  std::cout << "planet package indexed LZ4 blocks\n";
  const std::filesystem::path directory = std::filesystem::temp_directory_path() / "engine-planet-package-tests";
  const std::filesystem::path path = directory / "round-trip.planetpkg";
  std::error_code filesystemError;
  std::filesystem::remove_all(directory, filesystemError);
  std::filesystem::create_directories(directory, filesystemError);
  check(!filesystemError, "temporary package directory is created");

  procedural::PlanetPackageMetadata metadata{};
  metadata.contentHash = 0x0123456789abcdefull;
  metadata.buildHash = 0xfedcba9876543210ull;
  metadata.planetRadiusMetres = 6371000.0;
  metadata.heightScaleMetres = 0.25;
  metadata.heightBiasMetres = -8192.0;
  metadata.nominalRenderPatchEdgeMetres = 1000.0;
  metadata.tileSizePixels = 256u;
  metadata.gutterPixels = 2u;
  metadata.maximumTileLevel = 16u;

  const std::vector<procedural::PlanetPackageChannel> channels{
    {procedural::PlanetPackageDomain::Graph, graph(procedural::PlanetGraphChannel::Neighbours), procedural::PlanetPackageFormat::R32UInt, procedural::PlanetPackageMipPolicy::None, 0u, 1.0, 0.0, "neighbours"},
    {procedural::PlanetPackageDomain::Raster, raster(procedural::PlanetRasterChannel::Material), procedural::PlanetPackageFormat::R8UInt, procedural::PlanetPackageMipPolicy::Mode, 0u, 1.0, 0.0, "material"},
    {procedural::PlanetPackageDomain::Raster, raster(procedural::PlanetRasterChannel::Height), procedural::PlanetPackageFormat::R16SInt, procedural::PlanetPackageMipPolicy::Average, 0u, 0.25, -8192.0, "height"},
  };

  const procedural::PlanetTileAddress tile{4u, 14u, 7312u, 9100u};
  std::vector<uint8_t> heights(258u * 258u * 2u);
  for (size_t byte = 0u; byte < heights.size(); ++byte)
    heights[byte] = static_cast<uint8_t>((byte / 128u) & 0x0fu);
  std::vector<uint8_t> materials(258u * 258u);
  uint32_t random = 0x12345678u;
  for (uint8_t &material : materials)
  {
    random = random * 1664525u + 1013904223u;
    material = static_cast<uint8_t>(random >> 24u);
  }
  std::vector<uint8_t> neighbours(4096u, 0u);
  for (size_t byte = 0u; byte < neighbours.size(); byte += 4u)
    neighbours[byte] = static_cast<uint8_t>((byte / 4u) & 0xffu);

  std::vector<procedural::PlanetPackageBlockInput> blocks;
  blocks.push_back({procedural::planetRasterBlockKey(raster(procedural::PlanetRasterChannel::Material), tile), materials});
  blocks.push_back({procedural::planetGraphBlockKey(graph(procedural::PlanetGraphChannel::Neighbours)), neighbours});
  procedural::PlanetPackageBlockInput heightBlock{};
  heightBlock.key = procedural::planetRasterBlockKey(raster(procedural::PlanetRasterChannel::Height), tile);
  heightBlock.payload = heights;
  heightBlock.hasValueRange = true;
  heightBlock.minimumValue = -120.0;
  heightBlock.maximumValue = 8848.75;
  blocks.push_back(std::move(heightBlock));

  procedural::PlanetPackageStatistics statistics{};
  std::string error;
  check(procedural::writePlanetPackage(path, metadata, channels, blocks, {}, &statistics, &error), "single-file planet package is written");
  if (!error.empty())
    std::cerr << "    " << error << '\n';
  check(statistics.channelCount == channels.size() && statistics.blockCount == blocks.size(), "writer reports channel and block counts");
  check(statistics.compressedBlockCount >= 2u, "compressible graph and height blocks use LZ4");
  check(statistics.storedPayloadBytes < statistics.uncompressedPayloadBytes, "independent compression reduces total payload bytes");

  procedural::PlanetPackageReader reader;
  check(reader.open(path, &error), "package header and directories open without decoding payloads");
  if (!error.empty())
    std::cerr << "    " << error << '\n';
  check(reader.metadata().contentHash == metadata.contentHash && reader.metadata().planetRadiusMetres == metadata.planetRadiusMetres, "planet identity and physical metadata round-trip");
  check(reader.channels().size() == channels.size() && reader.blocks().size() == blocks.size(), "channel and block directories round-trip");
  check(reader.findChannel(procedural::PlanetPackageDomain::Raster, raster(procedural::PlanetRasterChannel::Height)) != nullptr, "individual channel descriptors are indexed");

  const auto heightKey = procedural::planetRasterBlockKey(raster(procedural::PlanetRasterChannel::Height), tile);
  const auto materialKey = procedural::planetRasterBlockKey(raster(procedural::PlanetRasterChannel::Material), tile);
  const auto graphKey = procedural::planetGraphBlockKey(graph(procedural::PlanetGraphChannel::Neighbours));
  const procedural::PlanetPackageBlockRecord *heightRecord = reader.findBlock(heightKey);
  const procedural::PlanetPackageBlockRecord *materialRecord = reader.findBlock(materialKey);
  check(heightRecord != nullptr && heightRecord->compression == procedural::PlanetPackageCompression::LZ4, "height channel has its own compressed range");
  check(materialRecord != nullptr && heightRecord != nullptr && materialRecord->fileOffset != heightRecord->fileOffset, "material and height are independently addressable");
  check(heightRecord != nullptr && heightRecord->hasValueRange && heightRecord->minimumValue == -120.0 && heightRecord->maximumValue == 8848.75, "tile height bounds remain in the uncompressed index");

  std::vector<uint8_t> decoded;
  check(reader.readBlock(materialKey, decoded, &error) && decoded == materials, "reader selectively loads the material channel");
  check(reader.readBlock(heightKey, decoded, &error) && decoded == heights, "reader selectively decompresses the height channel");
  check(reader.readBlock(graphKey, decoded, &error) && decoded == neighbours, "reader independently loads graph topology");
  const procedural::PlanetTileAddress finerTile{tile.face, static_cast<uint8_t>(tile.level + 2u), tile.x * 4u + 3u, tile.y * 4u + 1u};
  check(reader.findBestRasterBlock(raster(procedural::PlanetRasterChannel::Height), finerTile) == heightRecord, "reader resolves a finer render request to its closest stored ancestor");
  check(!reader.readBlock(procedural::planetRasterBlockKey(raster(procedural::PlanetRasterChannel::River), tile), decoded, &error), "missing optional channels fail without reading another payload");

  if (heightRecord != nullptr && heightRecord->storedSize != 0u)
  {
    std::fstream corrupt(path, std::ios::binary | std::ios::in | std::ios::out);
    corrupt.seekg(static_cast<std::streamoff>(heightRecord->fileOffset), std::ios::beg);
    char byte = 0;
    corrupt.read(&byte, 1);
    byte ^= 0x5a;
    corrupt.seekp(static_cast<std::streamoff>(heightRecord->fileOffset), std::ios::beg);
    corrupt.write(&byte, 1);
    corrupt.close();
    check(!reader.readBlock(heightKey, decoded, &error), "corrupt payload is rejected by decompression or checksum validation");
  }

  reader.close();
  std::filesystem::remove_all(directory, filesystemError);
}

void testWriterRejectsAmbiguousKeys()
{
  std::cout << "planet package validation\n";
  procedural::PlanetPackageMetadata metadata{};
  metadata.planetRadiusMetres = 1000000.0;
  metadata.maximumTileLevel = 8u;
  const std::vector<procedural::PlanetPackageChannel> channels{
    {procedural::PlanetPackageDomain::Raster, raster(procedural::PlanetRasterChannel::Height), procedural::PlanetPackageFormat::R16SInt, procedural::PlanetPackageMipPolicy::Average, 0u, 1.0, 0.0, "height"},
  };
  const procedural::PlanetPackageBlockInput block{
    procedural::planetRasterBlockKey(raster(procedural::PlanetRasterChannel::Height), {0u, 2u, 1u, 1u}),
    {1u, 2u, 3u, 4u},
  };
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "invalid-planet-package.planetpkg";
  std::string error;
  check(!procedural::writePlanetPackage(path, metadata, channels, {block, block}, {}, nullptr, &error), "writer rejects duplicate tile/channel keys");
  std::error_code filesystemError;
  std::filesystem::remove(path, filesystemError);
}

void testSphericalGraphAdapter()
{
  std::cout << "spherical terrain graph package adapter\n";
  procedural::SphericalTerrainMaps maps{};
  maps.regionNodes = {
    {{1.0, 0.0, 0.0}, 100u, 1u, 10u, 20u, 30u, 40u, 50u, 60u, 70u, 80u, 90u, 100u, 110u, 5u},
    {{0.0, 1.0, 0.0}, 200u, 2u, 11u, 21u, 31u, 41u, 51u, 61u, 71u, 81u, 91u, 101u, 111u, 6u},
    {{0.0, 0.0, 1.0}, 300u, 3u, 12u, 22u, 32u, 42u, 52u, 62u, 72u, 82u, 92u, 102u, 112u, 7u},
  };
  maps.regionNeighbourOffsets = {0u, 2u, 4u, 6u};
  maps.regionNeighbourIndices = {1u, 2u, 0u, 2u, 0u, 1u};
  maps.regionNodes[0].terrainElevation = 90u;
  maps.regionNodes[0].downstreamRegion = 1u;
  maps.regionNodes[0].riverDischarge = 1.25f;
  maps.regionNodes[0].flags = procedural::SphericalTerrainNode::oceanFlag;
  maps.regionNodes[1].terrainElevation = 150u;
  maps.regionNodes[1].waterElevation = 200u;
  maps.regionNodes[1].downstreamRegion = 2u;
  maps.regionNodes[1].riverDischarge = 2.5f;
  maps.regionNodes[1].flags = procedural::SphericalTerrainNode::lakeFlag;
  maps.regionNodes[2].terrainElevation = 280u;
  maps.regionNodes[2].downstreamRegion = procedural::SphericalTerrainNode::noRegion;
  maps.regionNodes[2].riverDischarge = 5.0f;

  procedural::PlanetPackageMetadata metadata{};
  metadata.planetRadiusMetres = 5000000.0;
  metadata.heightScaleMetres = 0.5;
  metadata.heightBiasMetres = -1000.0;
  std::vector<procedural::PlanetPackageChannel> channels;
  std::vector<procedural::PlanetPackageBlockInput> blocks;
  std::string error;
  check(procedural::buildSphericalTerrainGraphPackage(maps, metadata, channels, blocks, &error), "authoritative region graph converts to package blocks");
  check(channels.size() == 19u && blocks.size() == channels.size(), "graph attributes remain independent structure-of-arrays channels");

  const auto findInput = [&blocks](const procedural::PlanetGraphChannel channel) -> const procedural::PlanetPackageBlockInput *
  {
    const auto key = procedural::planetGraphBlockKey(static_cast<uint32_t>(channel));
    const auto found = std::find_if(
        blocks.begin(),
        blocks.end(),
        [&key](const auto &block)
        {
          return block.key == key;
        });
    return found == blocks.end() ? nullptr : &*found;
  };
  const auto *positions = findInput(procedural::PlanetGraphChannel::Positions);
  const auto *offsets = findInput(procedural::PlanetGraphChannel::NeighbourOffsets);
  const auto *wind = findInput(procedural::PlanetGraphChannel::Wind);
  const auto *downstream = findInput(procedural::PlanetGraphChannel::DownstreamRegion);
  const auto *waterHeight = findInput(procedural::PlanetGraphChannel::WaterHeight);
  const auto *discharge = findInput(procedural::PlanetGraphChannel::RiverDischarge);
  const auto *flags = findInput(procedural::PlanetGraphChannel::Flags);
  check(positions != nullptr && positions->payload.size() == maps.regionNodes.size() * 3u * sizeof(float), "graph positions serialize as portable F32 triples");
  check(offsets != nullptr && offsets->payload.size() == maps.regionNeighbourOffsets.size() * sizeof(uint32_t), "CSR offsets serialize independently from adjacency IDs");
  check(wind != nullptr && wind->payload == std::vector<uint8_t>({80u, 90u, 81u, 91u, 82u, 92u}), "paired wind components preserve node order");
  check(downstream != nullptr && downstream->payload.size() == 12u && downstream->payload[0] == 1u && downstream->payload[4] == 2u, "directed drainage receivers are retained");
  check(waterHeight != nullptr && waterHeight->payload == std::vector<uint8_t>({0u, 0u, 200u, 0u, 0u, 0u}), "lake water surfaces remain distinct from terrain elevation");
  check(discharge != nullptr && discharge->payload.size() == maps.regionNodes.size() * sizeof(float), "river discharge remains an unquantized graph field");
  check(flags != nullptr && flags->payload == std::vector<uint8_t>({
      static_cast<uint8_t>(procedural::kPlanetSurfaceFlagWater | procedural::kPlanetSurfaceFlagOcean | procedural::kPlanetSurfaceFlagRiver),
      static_cast<uint8_t>(procedural::kPlanetSurfaceFlagWater | procedural::kPlanetSurfaceFlagLake | procedural::kPlanetSurfaceFlagRiver),
      procedural::kPlanetSurfaceFlagRiver}), "graph and raster blocks use the same public surface flag layout");

  const std::filesystem::path path = std::filesystem::temp_directory_path() / "spherical-graph.planetpkg";
  procedural::PlanetPackageStatistics statistics{};
  check(procedural::writeSphericalTerrainGraphPackage(path, maps, metadata, {}, &statistics, &error), "current spherical generator graph writes directly to a planet package");
  procedural::PlanetPackageReader reader;
  check(reader.open(path, &error), "graph-only planet package opens through the shared reader");
  std::vector<uint8_t> decoded;
  check(
      reader.readBlock(procedural::planetGraphBlockKey(graph(procedural::PlanetGraphChannel::Material)), decoded, &error) && decoded == std::vector<uint8_t>({5u, 6u, 7u}),
      "one graph attribute can be loaded without decoding graph topology");
  std::error_code filesystemError;
  std::filesystem::remove(path, filesystemError);

  maps.regionNeighbourIndices.back() = 99u;
  check(!procedural::buildSphericalTerrainGraphPackage(maps, metadata, channels, blocks, &error), "graph adapter rejects out-of-range neighbour IDs");
  maps.regionNeighbourIndices.back() = 1u;
  maps.regionNodes[0].downstreamRegion = 99u;
  check(!procedural::buildSphericalTerrainGraphPackage(maps, metadata, channels, blocks, &error), "graph adapter rejects out-of-range drainage receivers");
}

void testCubeRasterBakerAndCombinedPackage()
{
  std::cout << "spherical terrain cube raster package\n";
  procedural::SphericalTerrainMaps maps{};
  maps.width = 8u;
  maps.height = 4u;
  const size_t texelCount = static_cast<size_t>(maps.width) * maps.height;
  maps.heightmap.resize(texelCount);
  maps.oceanFloorHeightmap.resize(texelCount);
  maps.waterHeightmap.resize(texelCount);
  maps.oceanMask.resize(texelCount);
  maps.lakeMask.resize(texelCount);
  maps.sourceRegions.resize(texelCount);
  maps.plates.resize(texelCount);
  maps.continents.resize(texelCount);
  maps.tectonics.resize(texelCount);
  maps.mountains.resize(texelCount);
  maps.rivers.resize(texelCount);
  maps.temperature.resize(texelCount);
  maps.precipitation.resize(texelCount);
  maps.wind.resize(texelCount);
  maps.windEast.resize(texelCount);
  maps.windNorth.resize(texelCount);
  maps.plateMotionEast.resize(texelCount);
  maps.plateMotionNorth.resize(texelCount);
  maps.materials.resize(texelCount);
  for (uint32_t y = 0u; y < maps.height; ++y)
    for (uint32_t x = 0u; x < maps.width; ++x)
    {
      const size_t index = static_cast<size_t>(y) * maps.width + x;
      const bool lakeSide = x >= maps.width / 2u;
      maps.heightmap[index] = static_cast<uint16_t>(1000u + x * 100u + y * 10u);
      maps.oceanFloorHeightmap[index] = lakeSide ? 0u :
          static_cast<uint16_t>(2500u + x * 100u + y * 10u);
      maps.waterHeightmap[index] = lakeSide ? 1800u : 0u;
      maps.oceanMask[index] = lakeSide ? 0u : 255u;
      maps.lakeMask[index] = lakeSide ? 255u : 0u;
      maps.sourceRegions[index] = lakeSide ? 1u : 0u;
      maps.plates[index] = lakeSide ? 2u : 1u;
      maps.continents[index] = static_cast<uint8_t>(x * 24u);
      maps.tectonics[index] = static_cast<uint8_t>(y * 50u);
      maps.mountains[index] = static_cast<uint8_t>((x + y) * 20u);
      maps.rivers[index] = x == 3u || x == 4u ? 220u : 0u;
      maps.temperature[index] = static_cast<uint8_t>(80u + y * 20u);
      maps.precipitation[index] = static_cast<uint8_t>(30u + x * 20u);
      maps.wind[index] = static_cast<uint8_t>(10u + x * 5u);
      maps.windEast[index] = static_cast<uint8_t>(100u + x);
      maps.windNorth[index] = static_cast<uint8_t>(120u + y);
      maps.plateMotionEast[index] = static_cast<uint8_t>(130u + x);
      maps.plateMotionNorth[index] = static_cast<uint8_t>(140u + y);
      maps.materials[index] = static_cast<uint8_t>(lakeSide ? procedural::PolygonTerrainMaterial::Lake : procedural::PolygonTerrainMaterial::Ocean);
    }
  maps.regionNodes.resize(2u);
  maps.regionNodes[0].position = {-1.0, 0.0, 0.0};
  maps.regionNodes[0].flags = procedural::SphericalTerrainNode::oceanFlag;
  maps.regionNodes[0].downstreamRegion = procedural::SphericalTerrainNode::noRegion;
  maps.regionNodes[1].position = {1.0, 0.0, 0.0};
  maps.regionNodes[1].terrainElevation = 1500u;
  maps.regionNodes[1].waterElevation = 1800u;
  maps.regionNodes[1].flags = procedural::SphericalTerrainNode::lakeFlag;
  maps.regionNodes[1].downstreamRegion = 0u;
  maps.regionNeighbourOffsets = {0u, 1u, 2u};
  maps.regionNeighbourIndices = {1u, 0u};

  procedural::PlanetPackageMetadata metadata{};
  metadata.planetRadiusMetres = 6371000.0;
  metadata.heightScaleMetres = 0.25;
  metadata.heightBiasMetres = -1000.0;
  metadata.tileSizePixels = 4u;
  metadata.gutterPixels = 1u;
  metadata.maximumTileLevel = 1u;
  procedural::SphericalTerrainCubePackageSettings settings{};
  settings.minimumTileLevel = 0u;
  settings.maximumUncompressedBytes = 1024u * 1024u;
  std::vector<procedural::PlanetPackageChannel> channels;
  std::vector<procedural::PlanetPackageBlockInput> blocks;
  procedural::SphericalTerrainCubePackageStatistics cubeStatistics{};
  std::string error;
  check(procedural::buildSphericalTerrainCubePackage(maps, metadata, settings, channels, blocks, &cubeStatistics, &error), "all spherical maps reproject into cube-quadtree tiles");
  check(cubeStatistics.tileCount == 30u && cubeStatistics.channelCount == 18u && cubeStatistics.blockCount == 540u, "six roots and their twenty-four children contain eighteen independent channels");
  check(cubeStatistics.storedTexelCount == 30u * 36u && cubeStatistics.uncompressedBytes == cubeStatistics.storedTexelCount * 26u, "gutter texels and channel byte sizes are accounted before allocation");

  const procedural::PlanetTileAddress positiveXRoot{0u, 0u, 0u, 0u};
  const auto heightKey = procedural::planetRasterBlockKey(raster(procedural::PlanetRasterChannel::Height), positiveXRoot);
  const auto waterKey = procedural::planetRasterBlockKey(raster(procedural::PlanetRasterChannel::WaterHeight), positiveXRoot);
  const auto flagsKey = procedural::planetRasterBlockKey(raster(procedural::PlanetRasterChannel::Flags), positiveXRoot);
  const auto oceanMaskKey = procedural::planetRasterBlockKey(raster(procedural::PlanetRasterChannel::OceanMask), positiveXRoot);
  const auto lakeMaskKey = procedural::planetRasterBlockKey(raster(procedural::PlanetRasterChannel::LakeMask), positiveXRoot);
  const auto oceanFloorKey = procedural::planetRasterBlockKey(raster(procedural::PlanetRasterChannel::OceanFloorHeight), positiveXRoot);
  const auto findBlock = [&blocks](const procedural::PlanetPackageBlockKey &key) -> const procedural::PlanetPackageBlockInput *
  {
    const auto found = std::find_if(
        blocks.begin(),
        blocks.end(),
        [&key](const auto &block)
        {
          return block.key == key;
        });
    return found == blocks.end() ? nullptr : &*found;
  };
  const auto *height = findBlock(heightKey);
  const auto *water = findBlock(waterKey);
  const auto *flags = findBlock(flagsKey);
  const auto *oceanMask = findBlock(oceanMaskKey);
  const auto *lakeMask = findBlock(lakeMaskKey);
  const auto *oceanFloor = findBlock(oceanFloorKey);
  check(height != nullptr && height->payload.size() == 36u * sizeof(uint16_t) && height->hasValueRange, "height tile includes a one-texel gutter and indexed bounds");
  bool oceanHeightIsAbsent = height != nullptr && oceanMask != nullptr;
  bool oceanSurfaceIsExplicit = water != nullptr && oceanMask != nullptr;
  if (oceanHeightIsAbsent && oceanSurfaceIsExplicit)
    for (size_t texel = 0u; texel < oceanMask->payload.size(); ++texel)
      if (oceanMask->payload[texel] != 0u)
      {
        oceanHeightIsAbsent &= readU16(height->payload, texel * 2u) == 0u;
        oceanSurfaceIsExplicit &= readU16(water->payload, texel * 2u) == 4000u;
      }
  check(oceanHeightIsAbsent, "ocean texels emit no solid terrain height after cube filtering");
  check(oceanSurfaceIsExplicit, "ocean water map encodes the zero-metre level through scale and bias");
  bool oceanFloorIsSeparate = oceanFloor != nullptr && oceanMask != nullptr;
  if (oceanFloorIsSeparate)
    for (size_t texel = 0u; texel < oceanMask->payload.size(); ++texel)
    {
      if (oceanMask->payload[texel] != 0u)
        oceanFloorIsSeparate &=
            readU16(oceanFloor->payload, texel * 2u) != 0u;
      else
        oceanFloorIsSeparate &=
            readU16(oceanFloor->payload, texel * 2u) == 0u;
    }
  const auto oceanFloorDescriptor = std::find_if(
      channels.begin(), channels.end(), [](const auto &channel)
      {
        return channel.domain == procedural::PlanetPackageDomain::Raster &&
            channel.id == raster(
                procedural::PlanetRasterChannel::OceanFloorHeight);
      });
  check(oceanFloorIsSeparate && oceanFloorDescriptor != channels.end() &&
          oceanFloorDescriptor->scale < 0.0 &&
          oceanFloorDescriptor->bias == 0.0,
      "ocean floor exports as negative bathymetric elevation independently from water");
  check(
      water != nullptr && water->payload.size() == 36u * sizeof(uint16_t) &&
          std::any_of(
              water->payload.begin(),
              water->payload.end(),
              [](const uint8_t value)
              {
                return value != 0u;
              }),
      "lake water height exports separately from surface height");
  check(
      flags != nullptr && std::any_of(
                              flags->payload.begin(),
                              flags->payload.end(),
                              [](const uint8_t value)
                              {
                                return (value & procedural::kPlanetSurfaceFlagLake) != 0u;
                              }),
      "water classification flags survive cube reprojection");
  check(oceanMask != nullptr && lakeMask != nullptr &&
          std::any_of(oceanMask->payload.begin(), oceanMask->payload.end(),
              [](const uint8_t value) { return value != 0u; }) &&
          std::any_of(lakeMask->payload.begin(), lakeMask->payload.end(),
              [](const uint8_t value) { return value != 0u; }),
      "ocean and lake occupancy export as independent masks");
  const std::vector<uint8_t> expectedFlags = flags != nullptr ? flags->payload : std::vector<uint8_t>{};

  settings.maximumUncompressedBytes = cubeStatistics.uncompressedBytes - 1u;
  check(!procedural::buildSphericalTerrainCubePackage(maps, metadata, settings, channels, blocks, nullptr, &error), "cube baker rejects an output larger than its explicit memory budget");
  settings.maximumUncompressedBytes = 1024u * 1024u;

  const std::filesystem::path path = std::filesystem::temp_directory_path() / "combined-planet.planetpkg";
  procedural::PlanetPackageStatistics packageStatistics{};
  check(procedural::writeSphericalTerrainPackage(path, maps, metadata, settings, {}, &packageStatistics, &cubeStatistics, &error), "graph and cube rasters write into one indexed package");
  procedural::PlanetPackageReader reader;
  check(reader.open(path, &error) && reader.channels().size() == 37u && reader.blocks().size() == 559u, "combined package exposes graph and raster directories together");
  std::vector<uint8_t> decoded;
  check(reader.readBlock(flagsKey, decoded, &error) && decoded == expectedFlags, "runtime reader selectively loads one cube map from the combined package");
  std::error_code filesystemError;
  std::filesystem::remove(path, filesystemError);
}

void testSphericalMeshChannelsAndPortableExport()
{
  std::cout << "spherical terrain mesh export\n";
  procedural::SphericalTerrainMaps maps{};
  maps.regionNodes.resize(3u);
  maps.regionNodes[0].position = {1.0, 0.0, 0.0};
  maps.regionNodes[1].position = {0.0, 1.0, 0.0};
  maps.regionNodes[2].position = {0.0, 0.0, 1.0};
  maps.regionNeighbourOffsets = {0u, 2u, 4u, 6u};
  maps.regionNeighbourIndices = {1u, 2u, 0u, 2u, 0u, 1u};
  const double inverseRootThree = 1.0 / std::sqrt(3.0);
  maps.surfacePositions = {
    maps.regionNodes[0].position,
    maps.regionNodes[1].position,
    maps.regionNodes[2].position,
    {inverseRootThree, inverseRootThree, inverseRootThree},
  };
  maps.surfaceElevations = {100u, 200u, 300u, 150u};
  maps.surfaceContinents = {10u, 20u, 30u, 20u};
  maps.surfaceTectonics = {11u, 21u, 31u, 21u};
  maps.surfaceMountains = {12u, 22u, 32u, 22u};
  maps.surfaceTemperature = {13u, 23u, 33u, 23u};
  maps.surfacePrecipitation = {14u, 24u, 34u, 24u};
  maps.surfacePlates = {1u, 2u, 3u, 2u};
  maps.surfaceRivers = {0u, 80u, 0u, 40u};
  maps.surfaceMaterials = {4u, 5u, 6u, 5u};
  maps.surfaceIndices = {
    0u, 1u, 3u,
    1u, 2u, 3u,
    2u, 0u, 3u,
  };

  procedural::PlanetPackageMetadata metadata{};
  metadata.planetRadiusMetres = 6371000.0;
  metadata.heightScaleMetres = 2.0;
  metadata.heightBiasMetres = 0.0;
  std::vector<procedural::PlanetPackageChannel> channels;
  std::vector<procedural::PlanetPackageBlockInput> blocks;
  std::string error;
  check(
      procedural::buildSphericalTerrainGraphPackage(
          maps, metadata, channels, blocks, &error),
      "generated dual mesh appends to the authoritative graph package");
  check(
      channels.size() == 33u && blocks.size() == channels.size(),
      "fourteen independently readable mesh and mapping channels are exported");
  const auto findInput = [&blocks](const procedural::PlanetGraphChannel channel)
      -> const procedural::PlanetPackageBlockInput *
  {
    const auto key = procedural::planetGraphBlockKey(
        static_cast<uint32_t>(channel));
    const auto found = std::find_if(
        blocks.begin(), blocks.end(),
        [&key](const auto &block) { return block.key == key; });
    return found == blocks.end() ? nullptr : &*found;
  };
  const auto *directions = findInput(
      procedural::PlanetGraphChannel::SurfaceDirections);
  const auto *indices = findInput(
      procedural::PlanetGraphChannel::SurfaceIndices);
  const auto *faceRegions = findInput(
      procedural::PlanetGraphChannel::SurfaceFaceRegion);
  const auto *faceTriangles = findInput(
      procedural::PlanetGraphChannel::SurfaceFaceTriangle);
  const auto *triangleRegions = findInput(
      procedural::PlanetGraphChannel::TriangleRegions);
  check(
      directions != nullptr &&
          directions->payload.size() == 4u * 3u * sizeof(float),
      "mesh directions remain precision-safe unit vectors");
  check(
      indices != nullptr &&
          indices->payload.size() == maps.surfaceIndices.size() * sizeof(uint32_t),
      "mesh indices are exported as flattened u32 triangles");
  check(
      faceRegions != nullptr && faceRegions->payload.size() == 12u &&
          faceRegions->payload[0] == 0u &&
          faceRegions->payload[4] == 1u &&
          faceRegions->payload[8] == 2u,
      "every render face maps to one stable Voronoi region");
  check(
      faceTriangles != nullptr &&
          std::all_of(
              faceTriangles->payload.begin(), faceTriangles->payload.end(),
              [](const uint8_t value) { return value == 0u; }),
      "render faces map back to their source Delaunay triangle");
  check(
      triangleRegions != nullptr && triangleRegions->payload.size() == 12u &&
          triangleRegions->payload[0] == 0u &&
          triangleRegions->payload[4] == 1u &&
          triangleRegions->payload[8] == 2u,
      "source triangles retain their three stable region IDs");

  check(
      std::abs(procedural::planetRadiusKilometres(metadata) - 6371.0) <
          1.0e-12 &&
          std::abs(procedural::planetDiameterKilometres(metadata) - 12742.0) <
              1.0e-12,
      "physical metadata exposes kilometre-scale radius and diameter");

  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      "engine-planet-mesh-export-tests";
  std::error_code filesystemError;
  std::filesystem::remove_all(directory, filesystemError);
  std::filesystem::create_directories(directory, filesystemError);
  procedural::PlanetMeshExportStatistics meshStatistics{};
  check(
      !filesystemError && procedural::writeSphericalTerrainMeshPly(
          directory / "planet-mesh.ply", maps, metadata,
          &meshStatistics, &error),
      "planet mesh writes as portable binary PLY");
  check(
      meshStatistics.regionCount == 3u &&
          meshStatistics.sourceTriangleCount == 1u &&
          meshStatistics.vertexCount == 4u &&
          meshStatistics.faceCount == 3u &&
          meshStatistics.minimumElevationMetres == 200.0 &&
          meshStatistics.maximumElevationMetres == 600.0,
      "mesh export reports physical bounds and topology counts");
  check(
      procedural::writePlanetMetadataManifest(
          directory / "planet-metadata.json", maps, metadata,
          meshStatistics, &error),
      "human-readable physical and mapping metadata is exported");
  std::ifstream ply(directory / "planet-mesh.ply", std::ios::binary);
  std::string plyHeader;
  for (std::string line; std::getline(ply, line);)
  {
    plyHeader += line + '\n';
    if (line == "end_header")
      break;
  }
  check(
      plyHeader.find("element vertex 4") != std::string::npos &&
          plyHeader.find("property uint region_id") != std::string::npos &&
          plyHeader.find("property uint source_triangle_id") !=
              std::string::npos,
      "PLY schema carries face-to-region and source-triangle mappings");
  std::ifstream manifest(directory / "planet-metadata.json");
  const std::string manifestText{
    std::istreambuf_iterator<char>(manifest),
    std::istreambuf_iterator<char>()};
  check(
      manifestText.find("\"planetRadiusKilometres\": 6371") !=
              std::string::npos &&
          manifestText.find("\"triangleRegions\": 25") !=
              std::string::npos,
      "metadata manifest records physical dimensions and package channel IDs");

  maps.surfaceIndices[0] = 3u;
  check(
      !procedural::buildSphericalTerrainGraphPackage(
          maps, metadata, channels, blocks, &error) &&
          !procedural::writeSphericalTerrainMeshPly(
              directory / "invalid.ply", maps, metadata, nullptr, &error),
      "invalid face-to-region mappings are rejected");
  std::filesystem::remove_all(directory, filesystemError);
}

bool validateExternalPackage(const std::filesystem::path &path)
{
  std::cout << "validating external planet package " << path << '\n';
  procedural::PlanetPackageReader reader;
  std::string error;
  if (!reader.open(path, &error))
  {
    std::cerr << "  FAILED metadata validation: " << error << '\n';
    return false;
  }
  uint64_t storedBytes = 0u;
  uint64_t decodedBytes = 0u;
  uint32_t graphBlocks = 0u;
  uint32_t rasterBlocks = 0u;
  std::vector<uint8_t> payload;
  for (size_t index = 0u; index < reader.blocks().size(); ++index)
  {
    const auto &record = reader.blocks()[index];
    if (!reader.readBlock(record, payload, &error) ||
        payload.size() != record.uncompressedSize)
    {
      std::cerr << "  FAILED block " << index << " validation: "
                << error << '\n';
      return false;
    }
    storedBytes += record.storedSize;
    decodedBytes += record.uncompressedSize;
    if (record.key.domain == procedural::PlanetPackageDomain::Graph)
      ++graphBlocks;
    else
      ++rasterBlocks;
  }
  const auto &metadata = reader.metadata();
  std::cout << "  PASSED channels=" << reader.channels().size()
            << " blocks=" << reader.blocks().size()
            << " graphBlocks=" << graphBlocks
            << " rasterBlocks=" << rasterBlocks
            << " storedBytes=" << storedBytes
            << " decodedBytes=" << decodedBytes
            << " maximumTileLevel="
            << static_cast<uint32_t>(metadata.maximumTileLevel) << '\n';
  return !reader.channels().empty() && !reader.blocks().empty() &&
      graphBlocks != 0u && rasterBlocks != 0u;
}

} // namespace

int main(const int argc, char **argv)
{
  if (argc == 2)
    return validateExternalPackage(argv[1]) ? 0 : 1;
  if (argc != 1)
  {
    std::cerr << "Usage: PlanetPackageTests [planet.planetpkg]\n";
    return 1;
  }
  testCubeQuadtreeAddresses();
  testCubeProjectionAndGutters();
  testPackageRoundTripAndSelectiveReads();
  testWriterRejectsAmbiguousKeys();
  testSphericalGraphAdapter();
  testCubeRasterBakerAndCombinedPackage();
  testSphericalMeshChannelsAndPortableExport();
  return failures == 0 ? 0 : 1;
}
