#pragma once

#include "Generator.hpp"
#include "PatchTiling.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace procedural::world::generator {

// Road growth after Parish and Muller 2001, "Procedural Modeling of Cities",
// section 3: an extended L-system whose modules are road segments.
//
//   road(delay, ruleAttributes, roadAttributes, state)
//   p1: delay < 0            -> removed
//   p2: delay > 0            -> road(delay - 1, ...)
//   p3: state == Unassigned  -> localConstraints(roadAttributes) sets Succeed or Failed
//   p4: state == Succeed     -> segment + the modules globalGoals proposes
//
// Global goals (3.2): highways follow the population density sampled along
// rays in a fan; streets follow the superimposed pattern of their zone:
// Basic (population only), Raster (a global grid), Radial (rings and spokes
// about a centre), Elevation (the least slope on the heightmap). Highways
// branch rarely, streets branch at right angles with a delay so highways are
// laid first.
//
// Local constraints (3.3): a segment that leaves the legal area is pruned,
// then rotated, then failed; a crossing with an existing road cuts it at the
// crossing; an end within the intersection radius snaps to that crossing; an
// end within the road radius is extended to the road. Crossings are limited
// in degree and in the angle between their roads.
enum class RoadPattern : uint8_t
{
  Basic,
  Raster,
  Radial,
  Elevation
};

struct RoadGrowthInput
{
  struct Seed
  {
    Position origin;
    double bearing = 0;
    bool highway = true;
    int delay = 0;
  };

  std::vector<Seed> seeds;
  RoadPattern pattern = RoadPattern::Basic;
  Position centre;              // Radial pattern centre.
  double rasterAxisRadians = 0; // Raster pattern axis.
  double highwayLength = 52, streetLength = 34;
  double intersectionSnapMetres = 10, roadSnapMetres = 8;
  double highwayBranchProbability = .15, streetBranchProbability = .6;
  int streetDelay = 4; // Streets off a highway wait this many steps.
  double maximumPruneFraction = .45, maximumRotationRadians = .5;
  double minimumAngleRadians = .45;
  unsigned maximumDegree = 4;
  double branchDensityFloor = .4;   // Branch probability floor where no population is (cities .4, a world 0).
  double terminalDensity = 0;       // A highway whose densest ray weighs less than this ends (0 = never).
  std::vector<Position> snapPoints; // Destinations (settlement centres): an end within the crossing radius snaps to one and stops.
  uint32_t maximumSegments = 200;
  std::function<bool(Position)> onLand;             // A point lies on legal land.
  std::function<bool(Position, Position)> covered;  // A whole segment lies on legal land.
  std::function<double(double, double)> terrain;    // Heightmap, metres.
  std::function<double(Position)> density;          // Population density, 0..1.
  std::function<double(uint64_t, uint64_t)> random; // Deterministic unit draws by key and purpose.
};

struct RoadSegment
{
  Position a, b;
  bool highway = false;
  uint32_t generation = 0;
};

struct RoadNetwork
{
  std::vector<RoadSegment> segments;
  uint32_t modules = 0, pruned = 0, rotated = 0, failedIllegal = 0, failedShort = 0, failedAngle = 0, failedDegree = 0, cut = 0, snappedCrossing = 0, snappedRoad = 0;
};

class RoadGrowth final : public Generator<RoadGrowthInput, RoadNetwork>
{
public:
  explicit RoadGrowth(uint64_t domain);

  // The layout calls the growth directly; the DAG reaches it through run().
  static bool grow(const RoadGrowthInput &input, RoadNetwork &output, Ledger &ledger, std::string *error);

protected:
  bool run(const RoadGrowthInput &input, RoadNetwork &output, Ledger &ledger, std::string *error) override;
};

} // namespace procedural::world::generator
