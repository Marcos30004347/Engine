#pragma once
#include "WorldRiverPath.hpp"
#include "WorldSplineLandforms.hpp"
#include <set>
namespace procedural::world::rewrite
{
struct AccessRamp
{
  uint64_t id = 1, fromFeature = 0, toFeature = 0;
  std::vector<Position> stations; // Authored design heights; horizontal metres.
  double halfWidthMetres = 3, shoulderMetres = 4, maximumGrade = .1, maximumCrossSlope = .02;
};
struct RiverReservation
{
  uint32_t upstreamNode = 0;
  double halfWidthMetres = 8, bankTransitionMetres = 40, depthMetres = 1, maximumCutMetres = 100;
};
struct PhysicalSample
{
  double height = 0;
  uint64_t rampId = 0;
  bool protectedRiver = false;
  SplineRegion region = SplineRegion::Ground;
};
struct RampPanel
{
  uint64_t rampId = 0;
  std::array<Position, 3> vertices;
  double longitudinalGrade = 0, crossSlope = 0;
};
// Cut the terrain triangulation at every ramp panel boundary and CDT each
// resulting polygon. Role 2 identifies actual planar ramp geometry.
bool constrainRampMesh(const std::vector<RampPanel> &, const SplineFaceMesh &, SplineFaceMesh &, std::vector<uint8_t> &roles, std::string *error = nullptr);
// Frozen composition: landform/weathering -> graded ramps -> reserved river cuts.
class WorldPhysicalTerrain
{
public:
  using BaseSurface = WorldSplineLandforms::BaseSurface;
  bool build(const WorldSplineLandforms &, BaseSurface, std::vector<AccessRamp>, const RiverGrowthProduct &, std::vector<RiverReservation>, std::string *error = nullptr);
  bool sample(double x, double z, PhysicalSample &) const;
  bool verticalFaces(SplineFaceMesh &, std::string *error = nullptr) const;
  const std::vector<RampPanel> &rampPanels() const
  {
    return panels_;
  }
  const std::vector<AccessRamp> &ramps() const
  {
    return ramps_;
  }
  const std::set<uint64_t> &reachableFeatures() const
  {
    return reachable_;
  }
  const std::vector<RiverReservation> &reservations() const
  {
    return reservations_;
  }
  bool beforeModifications(double x, double z, SplineSurfaceSample &out) const
  {
    return landforms_.sample(x, z, base_, out);
  }
  const WorldSplineLandforms &landforms() const
  {
    return landforms_;
  }
  const RiverGrowthProduct &rivers() const
  {
    return rivers_;
  }

private:
  WorldSplineLandforms landforms_;
  BaseSurface base_;
  std::vector<AccessRamp> ramps_;
  std::vector<RampPanel> panels_;
  RiverGrowthProduct rivers_;
  std::vector<RiverPath> paths_;
  std::vector<RiverReservation> reservations_;
  std::set<uint64_t> reachable_;
  double modify(double, double, double, PhysicalSample &) const;
};
} // namespace procedural::world::rewrite
