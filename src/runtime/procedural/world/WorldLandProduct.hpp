#pragma once
#include "procedural/world/MapPartition.hpp"
#include "procedural/world/WorldPatchMesh.hpp"

namespace procedural::world::rewrite
{
struct LandSettings
{
  double landFraction = 0.4;
  // Optional ocean reservation: every vertex of a land cell must lie inside it.
  double oceanMarginMetres = 0;
  // Indices into the input partition. Every core is retained; merging is allowed.
  std::vector<uint32_t> cores;
};
struct LandCell
{
  uint32_t id = 0;
  bool land = false;
  double areaMetres2 = 0;
  PatchDomain patch;
};
struct CoastLoop
{
  // Land is on the left; holes therefore have negative signed area.
  std::vector<BoundaryUse> edges;
  double signedAreaMetres2 = 0;
};
struct LandProduct
{
  WorldSpec spec;
  BoundaryRegistry boundaries;
  std::vector<LandCell> cells;
  std::vector<CoastLoop> coasts;
  double landAreaMetres2 = 0, totalAreaMetres2 = 0;
  uint32_t components = 0;
};
// Validates a complete conforming partition of the world square before growing
// connected fronts. Atomic publication. No elevation or coast noise is applied.
bool buildLandProduct(const WorldSpec &, const std::vector<VoronoiCell> &, const LandSettings &, LandProduct &, std::string *error = nullptr);
struct ReliefProvince
{
  uint64_t id = 0;
  Position from, to;
  double radiusMetres = 1000;
  double riverSlope = 0.02, terrainLambda = 0.15;
  double strength = 1; // Relative weight against the lowland control; never adds height.
};
struct ReliefSample
{
  double riverSlope = 0.001, terrainLambda = 0.02;
};
class ReliefControls
{
public:
  bool build(std::vector<ReliefProvince>, ReliefSample lowland, std::string *error = nullptr);
  ReliefSample sample(double x, double z) const;

private:
  std::vector<ReliefProvince> provinces_;
  ReliefSample lowland_;
};
} // namespace procedural::world::rewrite
