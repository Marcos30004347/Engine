#include "procedural/PlanetCubeMapping.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace procedural
{
namespace
{

bool validFace(const PlanetCubeFace face)
{
  return static_cast<uint8_t>(face) < kPlanetPackageCubeFaceCount;
}

SpherePoint normalize(const SpherePoint &point)
{
  const double squaredLength = point.x * point.x + point.y * point.y + point.z * point.z;
  if (!(squaredLength > 0.0) || !std::isfinite(squaredLength))
    return {};
  const double inverseLength = 1.0 / std::sqrt(squaredLength);
  return {point.x * inverseLength, point.y * inverseLength, point.z * inverseLength};
}

} // namespace

bool planetCubeDirection(const PlanetCubeCoordinate &coordinate, SpherePoint &direction)
{
  if (!validFace(coordinate.face) || !std::isfinite(coordinate.u) || !std::isfinite(coordinate.v))
    return false;
  const double u = coordinate.u * 2.0 - 1.0;
  const double v = coordinate.v * 2.0 - 1.0;
  switch (coordinate.face)
  {
  case PlanetCubeFace::PositiveX:
    direction = normalize({1.0, -v, -u});
    break;
  case PlanetCubeFace::NegativeX:
    direction = normalize({-1.0, -v, u});
    break;
  case PlanetCubeFace::PositiveY:
    direction = normalize({u, 1.0, v});
    break;
  case PlanetCubeFace::NegativeY:
    direction = normalize({u, -1.0, -v});
    break;
  case PlanetCubeFace::PositiveZ:
    direction = normalize({u, -v, 1.0});
    break;
  case PlanetCubeFace::NegativeZ:
    direction = normalize({-u, -v, -1.0});
    break;
  }
  return direction.x != 0.0 || direction.y != 0.0 || direction.z != 0.0;
}

bool planetCubeCoordinate(const SpherePoint &inputDirection, PlanetCubeCoordinate &coordinate)
{
  const SpherePoint direction = normalize(inputDirection);
  if (direction.x == 0.0 && direction.y == 0.0 && direction.z == 0.0)
    return false;
  const double ax = std::abs(direction.x);
  const double ay = std::abs(direction.y);
  const double az = std::abs(direction.z);
  double u = 0.0;
  double v = 0.0;
  if (ax >= ay && ax >= az)
  {
    if (direction.x >= 0.0)
    {
      coordinate.face = PlanetCubeFace::PositiveX;
      u = -direction.z / ax;
    }
    else
    {
      coordinate.face = PlanetCubeFace::NegativeX;
      u = direction.z / ax;
    }
    v = -direction.y / ax;
  }
  else if (ay >= az)
  {
    if (direction.y >= 0.0)
    {
      coordinate.face = PlanetCubeFace::PositiveY;
      u = direction.x / ay;
      v = direction.z / ay;
    }
    else
    {
      coordinate.face = PlanetCubeFace::NegativeY;
      u = direction.x / ay;
      v = -direction.z / ay;
    }
  }
  else
  {
    if (direction.z >= 0.0)
    {
      coordinate.face = PlanetCubeFace::PositiveZ;
      u = direction.x / az;
    }
    else
    {
      coordinate.face = PlanetCubeFace::NegativeZ;
      u = -direction.x / az;
    }
    v = -direction.y / az;
  }
  coordinate.u = std::clamp(u * 0.5 + 0.5, 0.0, 1.0);
  coordinate.v = std::clamp(v * 0.5 + 0.5, 0.0, 1.0);
  return true;
}

bool planetCubeTile(const SpherePoint &direction, const uint8_t level, PlanetTileAddress &tile, double *tileU, double *tileV)
{
  if (level > 31u)
    return false;
  PlanetCubeCoordinate coordinate{};
  if (!planetCubeCoordinate(direction, coordinate))
    return false;
  const uint32_t dimension = uint32_t{1u} << level;
  const double scaledX = coordinate.u * dimension;
  const double scaledY = coordinate.v * dimension;
  const uint32_t x = std::min(static_cast<uint32_t>(scaledX), dimension - 1u);
  const uint32_t y = std::min(static_cast<uint32_t>(scaledY), dimension - 1u);
  tile = {static_cast<uint8_t>(coordinate.face), level, x, y};
  if (tileU != nullptr)
    *tileU = scaledX - x;
  if (tileV != nullptr)
    *tileV = scaledY - y;
  return true;
}

bool planetCubeTileTexelDirection(const PlanetTileAddress &tile, const uint32_t interiorSizePixels, const uint16_t gutterPixels, const uint32_t storedX, const uint32_t storedY, SpherePoint &direction)
{
  if (!validPlanetTileAddress(tile) || interiorSizePixels == 0u)
    return false;
  const uint64_t storedSize = static_cast<uint64_t>(interiorSizePixels) + static_cast<uint64_t>(gutterPixels) * 2u;
  if (storedX >= storedSize || storedY >= storedSize)
    return false;
  const uint64_t dimension = uint64_t{1u} << tile.level;
  const double facePixels = static_cast<double>(dimension * interiorSizePixels);
  const double faceX = static_cast<double>(static_cast<uint64_t>(tile.x) * interiorSizePixels) + (static_cast<double>(storedX) - gutterPixels + 0.5);
  const double faceY = static_cast<double>(static_cast<uint64_t>(tile.y) * interiorSizePixels) + (static_cast<double>(storedY) - gutterPixels + 0.5);
  return planetCubeDirection({static_cast<PlanetCubeFace>(tile.face), faceX / facePixels, faceY / facePixels}, direction);
}

} // namespace procedural
