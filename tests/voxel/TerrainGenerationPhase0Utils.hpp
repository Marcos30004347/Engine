#pragma once

#include "procedural/world/MapExport.hpp"
#include "voxel/svraster/PlanetVoxelChunk.hpp"
#include "voxel/svraster/SparseVoxelTerrainTree64.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace terrain_phase0
{

enum class FixtureKind : uint8_t
{
  Flat,
  Mountain,
  Cliff,
  Canyon,
  River,
  RoadSmoothed,
  Coastline,
  Snow,
  CrossLOD,
};

inline constexpr std::array kFixtureKinds{
  FixtureKind::Flat,
  FixtureKind::Mountain,
  FixtureKind::Cliff,
  FixtureKind::Canyon,
  FixtureKind::River,
  FixtureKind::RoadSmoothed,
  FixtureKind::Coastline,
  FixtureKind::Snow,
  FixtureKind::CrossLOD,
};

const char *fixtureName(FixtureKind kind);
procedural::world::MapExportBundle makeFixture(FixtureKind kind);
voxel::svraster::WorldMapVoxelTerrainSettings fixtureSettings(FixtureKind kind, uint32_t internalLOD, uint32_t workerCount = 1u);

struct SemanticTerrainCell
{
  std::array<int32_t, 3> coordinate{};
  voxel::svraster::SparseVoxelBrickMaterialGPU material{};
  uint32_t context = 0u;

  bool operator==(const SemanticTerrainCell &other) const;
};

struct SemanticTerrainNode
{
  std::array<int32_t, 4> canonicalOriginScale{};
  uint32_t occupiedCount = 0u;
  float selfError = 0.0f;
  float parentError = 0.0f;

  bool operator==(const SemanticTerrainNode &other) const;
};

struct SemanticTerrainPage
{
  double edgeMetres = 0.0;
  double voxelSizeMetres = 0.0;
  uint32_t internalLOD = 0u;
  uint64_t geometryRevision = 0u;
  std::vector<SemanticTerrainCell> cells;
  std::vector<SemanticTerrainNode> nodes;
};

bool decodeSemanticTerrainPage(const voxel::svraster::PlanetVoxelChunkSet &chunks, uint32_t chunkIndex, SemanticTerrainPage &page, std::string *error = nullptr);

bool compareSemanticTerrainPages(const SemanticTerrainPage &expected, const SemanticTerrainPage &actual, std::string *difference = nullptr);

bool compareRasterAndTree64Occupancy(const voxel::svraster::PlanetVoxelChunkSet &chunks, uint32_t chunkIndex, voxel::svraster::SparseVoxelTerrainPageTree64 *tree64 = nullptr, std::string *difference = nullptr);

uint64_t semanticTerrainPageHash(const SemanticTerrainPage &page);
uint64_t semanticTerrainRunCount(const SemanticTerrainPage &page);
uint64_t terrainPayloadBytes(const voxel::svraster::PlanetVoxelChunkSet &chunks);

} // namespace terrain_phase0
