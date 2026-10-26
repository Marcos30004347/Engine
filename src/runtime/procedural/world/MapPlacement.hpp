#pragma once

#include "procedural/world/MapExportSampler.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world
{

enum class PlacementAssetKind : uint8_t
{
  VoxelModel,
  Mesh,
};

enum class PlacementPotentialChannel : uint8_t
{
  Trees,
  Shrubs,
  Groundcover,
  ExposedRock,
};

struct PlacementBiomeAffinity
{
  Biome biome = Biome::Ocean;
  double weight = 1.0;
};

// Palette entries contain selection metadata only. resourceKey is resolved by
// a later asset collection and may name either a voxel model or a conventional
// mesh/material; this module never opens it or creates renderer instances.
struct PlacementAssetRule
{
  std::string assetKey;
  std::string collectionKey;
  std::string resourceKey;
  PlacementAssetKind kind = PlacementAssetKind::Mesh;
  PlacementPotentialChannel potential = PlacementPotentialChannel::Trees;
  double selectionWeight = 1.0;
  double densityMultiplier = 1.0;
  double footprintRadiusMetres = 1.0;
  double minimumSpacingMetres = 2.0;
  std::array<double, 2> scaleRange{1.0, 1.0};
  std::array<double, 2> altitudeRangeMetres{-100000.0, 100000.0};
  std::array<double, 2> slopeRangeDegrees{0.0, 90.0};
  std::array<double, 2> moistureRange{0.0, 1.0};
  std::array<double, 2> soilDepthRangeMetres{0.0, 100000.0};
  std::array<double, 2> riverDistanceRangeMetres{0.0, 1.0e12};
  std::vector<PlacementBiomeAffinity> biomes;
  // Empty accepts every substrate. Otherwise values are schema substrate IDs.
  std::vector<uint8_t> substrates;
};

struct PlacementPalette
{
  uint32_t version = 1u;
  std::vector<PlacementAssetRule> assets;

  bool validate(std::string *error = nullptr) const;
};

struct PlacementTileRequest
{
  // min u, min v, max u, max v; normalized, clamped and treated as half-open.
  std::array<double, 4> normalizedBounds{0.0, 0.0, 1.0, 1.0};
  uint8_t requestedLevel = 0u;
  // Global candidate lattice size. Jitter and conflict resolution are pure
  // functions of the world seed and lattice coordinate, so neighboring tile
  // requests produce the same candidates along their shared edge.
  double candidateCellMetres = 16.0;
  size_t maximumEvaluatedSites = 1u << 20u;
  size_t maximumCandidates = 1u << 18u;
};

struct PlacementCandidate
{
  uint64_t stableId = 0u;
  uint32_t assetIndex = 0u;
  Point2 position{};
  double yawRadians = 0.0;
  double scale = 1.0;
  double density = 0.0;
  double elevationMetres = 0.0;
  double slopeDegrees = 0.0;
  double moisture = 0.0;
  double soilDepthMetres = 0.0;
  double riverDistanceMetres = 0.0;
  uint8_t substrate = 0u;
  Biome dominantBiome = Biome::Ocean;
  MapExportPageAddress resolvedPage{};
};

struct PlacementCandidateSet
{
  std::vector<PlacementCandidate> candidates;
  size_t evaluatedSites = 0u;
  size_t densityRejected = 0u;
  size_t spacingRejected = 0u;
  size_t waterRejected = 0u;
  size_t exclusionRejected = 0u;
  size_t ancestorSampledSites = 0u;
};

// Deterministic, runtime-neutral placement planning over resident map pages.
// It selects palette entries and resolves cross-tile spacing, but deliberately
// stops before asset loading, collision queries, terrain projection or spawning.
bool buildPlacementCandidates(const MapExportBundle &bundle, const PlacementPalette &palette, const PlacementTileRequest &request, PlacementCandidateSet &result, std::string *error = nullptr);

} // namespace procedural::world
