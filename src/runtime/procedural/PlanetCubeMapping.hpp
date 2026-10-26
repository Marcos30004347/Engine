#pragma once

#include "procedural/PlanetPackage.hpp"
#include "procedural/SphericalVoronoiMesh.hpp"

#include <cstdint>

namespace procedural
{

enum class PlanetCubeFace : uint8_t
{
  PositiveX = 0u,
  NegativeX = 1u,
  PositiveY = 2u,
  NegativeY = 3u,
  PositiveZ = 4u,
  NegativeZ = 5u,
};

struct PlanetCubeCoordinate
{
  PlanetCubeFace face = PlanetCubeFace::PositiveX;
  double u = 0.5;
  double v = 0.5;
};

// Standard cube projection. U increases right and V increases down on every
// face. Coordinates outside [0, 1] are accepted by the direction conversion so
// tile gutters can sample continuously through an adjacent face.
bool planetCubeDirection(const PlanetCubeCoordinate &coordinate, SpherePoint &direction);
bool planetCubeCoordinate(const SpherePoint &direction, PlanetCubeCoordinate &coordinate);

// Resolves a direction to its tile and tile-local normalized coordinates at a
// cube-quadtree level.
bool planetCubeTile(const SpherePoint &direction, uint8_t level, PlanetTileAddress &tile, double *tileU = nullptr, double *tileV = nullptr);

// Returns the direction sampled at a stored texel centre. storedX/storedY are
// in the tile including its gutter. At cube boundaries the resulting direction
// naturally crosses onto the neighboring face rather than clamping.
bool planetCubeTileTexelDirection(const PlanetTileAddress &tile, uint32_t interiorSizePixels, uint16_t gutterPixels, uint32_t storedX, uint32_t storedY, SpherePoint &direction);

} // namespace procedural
