#include "procedural/PlanetCubeField.hpp"

#include <algorithm>
#include <cmath>

namespace procedural
{
namespace
{

constexpr double kPi = 3.14159265358979323846264338327950288;

// Equirectangular sampling with the same topology the atlas really has:
// longitude wraps, and crossing a pole reflects latitude while advancing
// longitude by half a turn. Clamping latitude instead builds a wall at each
// pole, which is the artifact the cube domain exists to avoid reintroducing.
void wrapAtlas(int32_t &x, int32_t &y, const uint32_t width, const uint32_t height)
{
  const int32_t signedWidth = static_cast<int32_t>(width);
  const int32_t signedHeight = static_cast<int32_t>(height);
  if (y < 0)
  {
    y = -y - 1;
    x += signedWidth / 2;
  }
  else if (y >= signedHeight)
  {
    y = signedHeight * 2 - y - 1;
    x += signedWidth / 2;
  }
  x %= signedWidth;
  if (x < 0)
    x += signedWidth;
  y = std::clamp(y, 0, signedHeight - 1);
}

template <typename Sample>
double sampleAtlas(
    const uint32_t width,
    const uint32_t height,
    const SpherePoint &direction,
    const bool nearest,
    const Sample &sample)
{
  const double longitude = std::atan2(direction.z, direction.x);
  const double latitude = std::asin(std::clamp(direction.y, -1.0, 1.0));
  // Matches spherePoint() in SphericalTerrain.cpp: longitude 0 sits at the
  // atlas centre and latitude runs from the north pole down.
  const double u = std::fmod(longitude / (2.0 * kPi) + 1.5, 1.0) * width;
  const double v = (0.5 - latitude / kPi) * height;
  if (nearest)
  {
    int32_t x = static_cast<int32_t>(std::floor(u));
    int32_t y = static_cast<int32_t>(std::floor(v));
    wrapAtlas(x, y, width, height);
    return sample(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
  }
  const double centredU = u - 0.5;
  const double centredV = v - 0.5;
  const int32_t x0 = static_cast<int32_t>(std::floor(centredU));
  const int32_t y0 = static_cast<int32_t>(std::floor(centredV));
  const double fractionX = centredU - x0;
  const double fractionY = centredV - y0;
  double accumulated = 0.0;
  for (int32_t offsetY = 0; offsetY < 2; ++offsetY)
    for (int32_t offsetX = 0; offsetX < 2; ++offsetX)
    {
      int32_t x = x0 + offsetX;
      int32_t y = y0 + offsetY;
      wrapAtlas(x, y, width, height);
      const double weight = (offsetX == 0 ? 1.0 - fractionX : fractionX) * (offsetY == 0 ? 1.0 - fractionY : fractionY);
      accumulated += weight * sample(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
    }
  return accumulated;
}

template <typename Element>
bool projectFromEquirect(
    const uint32_t width,
    const uint32_t height,
    const std::vector<Element> &source,
    const uint32_t side,
    PlanetCubeField &field,
    const bool nearest)
{
  if (width < 2u || height < 2u || side == 0u || source.size() != static_cast<size_t>(width) * height)
    return false;
  field.side = side;
  field.values.assign(PlanetCubeField::cellCount(side), 0.0f);
  const auto sample = [&](const uint32_t x, const uint32_t y)
  {
    return static_cast<double>(source[static_cast<size_t>(y) * width + x]);
  };
  for (uint32_t face = 0u; face < kPlanetPackageCubeFaceCount; ++face)
    for (uint32_t y = 0u; y < side; ++y)
      for (uint32_t x = 0u; x < side; ++x)
      {
        const SpherePoint direction = planetCubeFieldDirection(side, face, static_cast<int32_t>(x), static_cast<int32_t>(y));
        field.at(face, x, y) = static_cast<float>(sampleAtlas(width, height, direction, nearest, sample));
      }
  return true;
}

} // namespace

SpherePoint planetCubeFieldDirection(const uint32_t side, const uint32_t face, const int32_t x, const int32_t y)
{
  SpherePoint direction{};
  if (side == 0u)
    return direction;
  const PlanetCubeCoordinate coordinate{
      static_cast<PlanetCubeFace>(face),
      (static_cast<double>(x) + 0.5) / side,
      (static_cast<double>(y) + 0.5) / side,
  };
  planetCubeDirection(coordinate, direction);
  return direction;
}

PlanetCubeCell planetCubeFieldNeighbour(const uint32_t side, const uint32_t face, const int32_t x, const int32_t y)
{
  const int32_t limit = static_cast<int32_t>(side);
  if (x >= 0 && y >= 0 && x < limit && y < limit)
    return {face, static_cast<uint32_t>(x), static_cast<uint32_t>(y)};
  const SpherePoint direction = planetCubeFieldDirection(side, face, x, y);
  PlanetCubeCoordinate coordinate{};
  if (!planetCubeCoordinate(direction, coordinate))
    return {face, static_cast<uint32_t>(std::clamp(x, 0, limit - 1)), static_cast<uint32_t>(std::clamp(y, 0, limit - 1))};
  const int32_t targetX = static_cast<int32_t>(std::floor(coordinate.u * side));
  const int32_t targetY = static_cast<int32_t>(std::floor(coordinate.v * side));
  return {
      static_cast<uint32_t>(coordinate.face),
      static_cast<uint32_t>(std::clamp(targetX, 0, limit - 1)),
      static_cast<uint32_t>(std::clamp(targetY, 0, limit - 1)),
  };
}

bool planetCubeFieldFromEquirect(
    const uint32_t width,
    const uint32_t height,
    const std::vector<uint16_t> &source,
    const uint32_t side,
    PlanetCubeField &field,
    const bool nearest)
{
  return projectFromEquirect(width, height, source, side, field, nearest);
}

bool planetCubeFieldFromEquirect(
    const uint32_t width,
    const uint32_t height,
    const std::vector<uint8_t> &source,
    const uint32_t side,
    PlanetCubeField &field,
    const bool nearest)
{
  return projectFromEquirect(width, height, source, side, field, nearest);
}

bool planetEquirectFromCubeField(
    const PlanetCubeField &field,
    const uint32_t width,
    const uint32_t height,
    std::vector<float> &destination)
{
  if (!field.valid() || width < 2u || height < 2u)
    return false;
  destination.assign(static_cast<size_t>(width) * height, 0.0f);
  const uint32_t side = field.side;
  for (uint32_t y = 0u; y < height; ++y)
    for (uint32_t x = 0u; x < width; ++x)
    {
      // Matches spherePoint() in SphericalTerrain.cpp.
      const double longitude = ((static_cast<double>(x) + 0.5) / width) * 2.0 * kPi - kPi;
      const double latitude = (0.5 - (static_cast<double>(y) + 0.5) / height) * kPi;
      const double cosLatitude = std::cos(latitude);
      const SpherePoint direction{
          std::cos(longitude) * cosLatitude,
          std::sin(latitude),
          std::sin(longitude) * cosLatitude,
      };
      PlanetCubeCoordinate coordinate{};
      if (!planetCubeCoordinate(direction, coordinate))
        continue;
      const uint32_t face = static_cast<uint32_t>(coordinate.face);
      const double centredU = coordinate.u * side - 0.5;
      const double centredV = coordinate.v * side - 0.5;
      const int32_t x0 = static_cast<int32_t>(std::floor(centredU));
      const int32_t y0 = static_cast<int32_t>(std::floor(centredV));
      const double fractionX = centredU - x0;
      const double fractionY = centredV - y0;
      double accumulated = 0.0;
      for (int32_t offsetY = 0; offsetY < 2; ++offsetY)
        for (int32_t offsetX = 0; offsetX < 2; ++offsetX)
        {
          // A sample that leaves the face is fetched from the neighbouring
          // face rather than clamped, so the atlas has no seam of its own.
          const PlanetCubeCell cell = planetCubeFieldNeighbour(side, face, x0 + offsetX, y0 + offsetY);
          const double weight = (offsetX == 0 ? 1.0 - fractionX : fractionX) * (offsetY == 0 ? 1.0 - fractionY : fractionY);
          accumulated += weight * field.at(cell.face, cell.x, cell.y);
        }
      destination[static_cast<size_t>(y) * width + x] = static_cast<float>(accumulated);
    }
  return true;
}

uint32_t planetCubeFieldSideForEquirect(const uint32_t width)
{
  return std::max<uint32_t>(8u, (width + 3u) / 4u);
}

} // namespace procedural
