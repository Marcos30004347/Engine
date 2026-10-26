#include "procedural/PlanetCubeField.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{

uint32_t failures = 0u;

void expect(const bool condition, const std::string &message)
{
  if (condition)
    return;
  std::cerr << "FAILED: " << message << "\n";
  ++failures;
}

double dot(const procedural::SpherePoint &a, const procedural::SpherePoint &b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

double angleBetween(const procedural::SpherePoint &a, const procedural::SpherePoint &b)
{
  return std::acos(std::min(1.0, std::max(-1.0, dot(a, b))));
}

bool isCorner(const uint32_t side, const int32_t x, const int32_t y)
{
  const int32_t limit = static_cast<int32_t>(side);
  return (x < 0 || x >= limit) && (y < 0 || y >= limit);
}

// An interior offset must stay on its own face and index directly.
void testInteriorNeighboursAreLocal()
{
  const uint32_t side = 16u;
  for (uint32_t face = 0u; face < 6u; ++face)
  {
    const procedural::PlanetCubeCell cell = procedural::planetCubeFieldNeighbour(side, face, 5, 7);
    expect(cell.face == face && cell.x == 5u && cell.y == 7u, "interior neighbour must be local");
  }
}

// The load-bearing invariant: stepping off an edge and then stepping back must
// return to the original cell. Orientation errors in the face transform break
// this immediately, and a clamp fails it on every edge cell.
void testEdgeStepsAreReversible()
{
  const std::array<std::pair<int32_t, int32_t>, 4> steps{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
  for (const uint32_t side : {8u, 16u, 64u})
  {
    const int32_t limit = static_cast<int32_t>(side);
    uint32_t checked = 0u;
    for (uint32_t face = 0u; face < 6u; ++face)
      for (int32_t y = 0; y < limit; ++y)
        for (int32_t x = 0; x < limit; ++x)
        {
          for (const auto &[stepX, stepY] : steps)
          {
            const int32_t targetX = x + stepX;
            const int32_t targetY = y + stepY;
            if (targetX >= 0 && targetX < limit && targetY >= 0 && targetY < limit)
              continue;
            const procedural::PlanetCubeCell across = procedural::planetCubeFieldNeighbour(side, face, targetX, targetY);
            expect(across.face != face, "an off-edge step must land on another face");
            // Step back along the direction that points at the source face.
            // The returning step is found by trying all four, since the axes
            // may swap across a seam; exactly one must return home.
            uint32_t returned = 0u;
            for (const auto &[backX, backY] : steps)
            {
              const procedural::PlanetCubeCell home = procedural::planetCubeFieldNeighbour(
                  side, across.face, static_cast<int32_t>(across.x) + backX, static_cast<int32_t>(across.y) + backY);
              if (home.face == face && home.x == static_cast<uint32_t>(x) && home.y == static_cast<uint32_t>(y))
                ++returned;
            }
            expect(returned == 1u,
                "exactly one reverse step must return to the source cell at side " + std::to_string(side) +
                    " face " + std::to_string(face) + " cell " + std::to_string(x) + "," + std::to_string(y));
            ++checked;
          }
        }
    expect(checked == 6u * side * 4u, "every face must contribute one crossing per edge cell per side");
  }
}

// A cell and its cross-seam neighbour must be about one cell apart in angle. A
// transform that lands on the wrong cell still round-trips if it is symmetric,
// so distance is what catches an off-by-one or a mirrored axis.
void testSeamNeighboursAreOneCellApart()
{
  const uint32_t side = 32u;
  const int32_t limit = static_cast<int32_t>(side);
  const double nominal = (3.14159265358979323846 * 0.5) / side;
  for (uint32_t face = 0u; face < 6u; ++face)
    for (int32_t y = 0; y < limit; ++y)
    {
      const procedural::SpherePoint source = procedural::planetCubeFieldDirection(side, face, limit - 1, y);
      const procedural::PlanetCubeCell across = procedural::planetCubeFieldNeighbour(side, face, limit, y);
      const procedural::SpherePoint target = procedural::planetCubeFieldDirection(side, across.face, static_cast<int32_t>(across.x), static_cast<int32_t>(across.y));
      const double angle = angleBetween(source, target);
      // Cube cells vary in angular size by about 2x from face centre to corner,
      // so the bound is generous; a wrong cell is off by whole multiples.
      expect(angle > nominal * 0.3 && angle < nominal * 2.5,
          "seam neighbour must be about one cell away, got " + std::to_string(angle / nominal) + " cells");
    }
}

// Only the eight cube corners may be degenerate.
void testCornerDegeneracyIsBounded()
{
  const uint32_t side = 16u;
  const int32_t limit = static_cast<int32_t>(side);
  uint32_t degenerate = 0u;
  for (uint32_t face = 0u; face < 6u; ++face)
    for (int32_t y = -1; y <= limit; ++y)
      for (int32_t x = -1; x <= limit; ++x)
      {
        if (x >= 0 && x < limit && y >= 0 && y < limit)
          continue;
        const procedural::PlanetCubeCell cell = procedural::planetCubeFieldNeighbour(side, face, x, y);
        const procedural::SpherePoint source = procedural::planetCubeFieldDirection(side, face, x, y);
        const procedural::SpherePoint target = procedural::planetCubeFieldDirection(side, cell.face, static_cast<int32_t>(cell.x), static_cast<int32_t>(cell.y));
        const double angle = angleBetween(source, target);
        const double nominal = (3.14159265358979323846 * 0.5) / side;
        if (angle > nominal * 0.75)
        {
          expect(isCorner(side, x, y), "only cube corners may resolve to a distant cell");
          ++degenerate;
        }
      }
  expect(degenerate <= 6u * 4u, "at most the four corner diagonals per face may be degenerate, got " + std::to_string(degenerate));
}

// A smooth field must survive the atlas -> cube -> atlas round trip. This is
// the resampling budget the solver spends to run on a singularity-free domain.
void testEquirectRoundTripPreservesSmoothFields()
{
  const uint32_t width = 256u;
  const uint32_t height = 128u;
  std::vector<uint16_t> source(static_cast<size_t>(width) * height, 0u);
  for (uint32_t y = 0u; y < height; ++y)
    for (uint32_t x = 0u; x < width; ++x)
    {
      const double longitude = ((static_cast<double>(x) + 0.5) / width) * 2.0 * 3.14159265358979323846;
      const double latitude = (0.5 - (static_cast<double>(y) + 0.5) / height) * 3.14159265358979323846;
      // A low-order spherical harmonic: smooth everywhere including the poles,
      // so any large error is the resampling and not the signal.
      const double value = 0.5 + 0.25 * std::sin(latitude) + 0.2 * std::cos(longitude) * std::cos(latitude);
      source[static_cast<size_t>(y) * width + x] = static_cast<uint16_t>(std::lround(value * 65535.0));
    }

  procedural::PlanetCubeField field{};
  expect(procedural::planetCubeFieldFromEquirect(width, height, source, procedural::planetCubeFieldSideForEquirect(width), field),
      "atlas to cube projection must succeed");
  expect(field.valid() && field.side == 64u, "cube side must match a quarter of the atlas width");

  std::vector<float> restored;
  expect(procedural::planetEquirectFromCubeField(field, width, height, restored), "cube to atlas projection must succeed");
  expect(restored.size() == source.size(), "round trip must preserve the atlas size");

  double worst = 0.0;
  double accumulated = 0.0;
  for (size_t cell = 0u; cell < source.size(); ++cell)
  {
    const double error = std::abs(static_cast<double>(restored[cell]) - static_cast<double>(source[cell])) / 65535.0;
    worst = std::max(worst, error);
    accumulated += error;
  }
  const double mean = accumulated / source.size();
  expect(mean < 0.002, "round trip mean error must stay under 0.2 percent, got " + std::to_string(mean));
  expect(worst < 0.02, "round trip peak error must stay under 2 percent, got " + std::to_string(worst));
}

// Nearest sampling must keep a mask binary: an interpolated ocean mask invents
// a coastline that is neither land nor sea, and the solver's outlet boundary
// condition keys off exactly that mask.
void testNearestSamplingKeepsMasksBinary()
{
  const uint32_t width = 128u;
  const uint32_t height = 64u;
  std::vector<uint8_t> mask(static_cast<size_t>(width) * height, 0u);
  for (uint32_t y = 0u; y < height; ++y)
    for (uint32_t x = 0u; x < width; ++x)
      mask[static_cast<size_t>(y) * width + x] = x < width / 2u ? 255u : 0u;

  procedural::PlanetCubeField field{};
  expect(procedural::planetCubeFieldFromEquirect(width, height, mask, 32u, field, true), "mask projection must succeed");
  uint32_t intermediate = 0u;
  for (const float value : field.values)
    if (value > 0.5f && value < 254.5f)
      ++intermediate;
  expect(intermediate == 0u, "nearest sampling must not produce intermediate mask values, got " + std::to_string(intermediate));
}

} // namespace

int main()
{
  std::cout << "Planet cube field tests\n";
  testInteriorNeighboursAreLocal();
  testEdgeStepsAreReversible();
  testSeamNeighboursAreOneCellApart();
  testCornerDegeneracyIsBounded();
  testEquirectRoundTripPreservesSmoothFields();
  testNearestSamplingKeepsMasksBinary();
  if (failures == 0u)
    std::cout << "Planet cube field tests passed\n";
  return failures == 0u ? 0 : 1;
}
