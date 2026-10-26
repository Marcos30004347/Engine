#pragma once
#include "WorldCityMasterPlan.hpp"
#include "WorldSurfaceArrangement.hpp"
#include <array>
#include <functional>

namespace procedural::world::rewrite {
struct CityWallEra
{
  uint64_t id = 0;
  std::vector<uint64_t> districts, infillCells;
};

struct CityWallRoute
{
  uint64_t id = 0;
  double halfWidth = 2;
  std::vector<Position> path;
};

struct CityWallSettings
{
  double thickness = 1.5, height = 6, gateClearance = 1, minimumPier = 2;
  double maximumFoundationRelief = 2;
};

struct CityWallGate
{
  uint64_t id = 0, era = 0;
  std::vector<uint64_t> routes;
  Position a, b;
};

struct CityWallCircuit
{
  uint64_t era = 0;
  std::vector<Position> boundary;
  double perimeter = 0, openingLength = 0;
};

struct CityWallTriangle
{
  std::array<Position, 3> points;
  uint64_t era = 0;
};

struct CityWalls
{
  uint64_t revision = 0;
  std::vector<CityWallCircuit> circuits;
  std::vector<CityWallGate> gates;
  std::vector<SurfaceContour> foundations;
  std::vector<CityWallTriangle> triangles;
};

// Era IDs specify chronological order. New eras cumulatively include earlier
// land. Union outlines come from actual district/cell geometry, never convex hulls.
// V1 supports simple strictly nested circuits and open gate portals. Shared walls,
// holes, terrain terminations and gate arches are rejected/not synthesized.
// Roads are supplied engineered corridors; each segment's swept width cuts a
// gate, including oblique crossings. Atomic on invalid or inaccessible enclosure.
bool generateCityWalls(
    const CityMasterPlan &,
    const std::vector<CityPlanCell> &,
    const std::vector<CityWallEra> &,
    const std::vector<CityWallRoute> &,
    const CityWallSettings &,
    const std::function<double(double, double)> &terrain,
    CityWalls &,
    std::string *error = nullptr);
} // namespace procedural::world::rewrite
