#pragma once

#include "procedural/PlanetCubeMapping.hpp"

#include <cstdint>
#include <vector>

namespace procedural
{

// A scalar field over the six cube faces: the domain the planet's fluvial
// solver runs on. Cells are laid out face-major then row-major with X fastest,
// and cell centres sit at ((x + 0.5) / side, (y + 0.5) / side) in the face UV
// that PlanetCubeMapping already defines, so one field cell and one package
// texel at the matching level address the same direction.
//
// This exists instead of the equirectangular atlas because the atlas has a
// polar singularity: its rows narrow to nothing at the poles, so a flow solver
// either needs a cos(latitude) metric with an arbitrary floor or produces
// angular artifacts there. Six square faces have no singularity, only seams,
// and the seams are exact — see planetCubeFieldNeighbour.
struct PlanetCubeField
{
  uint32_t side = 0u;
  std::vector<float> values;

  bool valid() const
  {
    return side > 0u && values.size() == cellCount(side);
  }

  static size_t cellCount(const uint32_t side)
  {
    return static_cast<size_t>(side) * side * kPlanetPackageCubeFaceCount;
  }

  static size_t index(const uint32_t side, const uint32_t face, const uint32_t x, const uint32_t y)
  {
    return (static_cast<size_t>(face) * side + y) * side + x;
  }

  float at(const uint32_t face, const uint32_t x, const uint32_t y) const
  {
    return values[index(side, face, x, y)];
  }

  float &at(const uint32_t face, const uint32_t x, const uint32_t y)
  {
    return values[index(side, face, x, y)];
  }
};

// Direction of a cell centre.
SpherePoint planetCubeFieldDirection(uint32_t side, uint32_t face, int32_t x, int32_t y);

struct PlanetCubeCell
{
  uint32_t face = 0u;
  uint32_t x = 0u;
  uint32_t y = 0u;
};

// Resolves a possibly out-of-range cell offset to a real cell, crossing onto
// the adjacent face where the offset leaves this one.
//
// This is not a clamp and not a gutter copy. It projects the offset cell centre
// to its direction and inverts the cube mapping, so a step off an edge lands on
// the neighbouring face's true adjacent cell with its correct orientation. The
// gnomonic projection makes that landing exact to within (0.5 / side)^2 of a
// cell, which rounds to the intended cell for every side we use.
//
// The eight cube corners are the one degenerate case: a diagonal step off a
// corner has no unique destination, and the inverse mapping resolves it to one
// of the three meeting faces deterministically.
PlanetCubeCell planetCubeFieldNeighbour(uint32_t side, uint32_t face, int32_t x, int32_t y);

// Resamples the generator's equirectangular atlas onto cube faces. Bilinear
// unless nearest is requested; masks and class indices must use nearest, since
// an interpolated ocean mask invents a coastline that is neither land nor sea.
bool planetCubeFieldFromEquirect(
    uint32_t width,
    uint32_t height,
    const std::vector<uint16_t> &source,
    uint32_t side,
    PlanetCubeField &field,
    bool nearest = false);
bool planetCubeFieldFromEquirect(
    uint32_t width,
    uint32_t height,
    const std::vector<uint8_t> &source,
    uint32_t side,
    PlanetCubeField &field,
    bool nearest = false);

// Resamples a cube field back onto the equirectangular atlas. Bilinear within
// a face; samples that fall outside the face are taken from the neighbouring
// face through the same exact transform used for neighbours.
bool planetEquirectFromCubeField(
    const PlanetCubeField &field,
    uint32_t width,
    uint32_t height,
    std::vector<float> &destination);

// Face side that matches an equirectangular width at equal ground resolution.
// A face spans a quarter turn against the atlas's full turn, so width / 4 puts
// one face cell and one equatorial atlas texel at the same size. This is the
// same rule the package baker uses for its face resolution.
uint32_t planetCubeFieldSideForEquirect(uint32_t width);

} // namespace procedural
