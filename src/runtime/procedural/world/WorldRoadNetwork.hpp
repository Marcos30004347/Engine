#pragma once
#include "WorldPatchMesh.hpp"
#include <functional>

namespace procedural::world::rewrite {
struct RoadAnchor
{
  uint64_t id = 0, parentTown = 0;
  Position position;
  bool town = true;
};

struct RoadLink
{
  uint64_t from = 0, to = 0;
};

// Town minimum spanning tree plus required village-parent links. Stable by ID.
bool selectRoadLinks(const std::vector<RoadAnchor> &, std::vector<RoadLink> &, std::string *error = nullptr);

struct RoadSettings
{
  double spacingMetres = 4, lateralSpacingMetres = 2, corridorHalfWidthMetres = 60;
  double halfWidthMetres = 3, shoulderMetres = 1;
  double crossSlope = 0, crownSlope = .02;
  double maximumGrade = .12, maximumGradeChangePerMetre = .02;
  double maximumEarthworkMetres = 4, minimumBendRadiusMetres = 12;
  double earthworkWeight = 2, resistanceWeight = 8, turningWeight = 8;
  uint32_t smoothingPasses = 4;
};

struct RoadHeightLock
{
  uint32_t station = 0;
  double heightMetres = 0;
};

struct RoadSection
{
  Position center;
  double distanceMetres = 0, naturalHeightMetres = 0;
  // From left shoulder through left tread, crown, right tread, right shoulder.
  std::array<Position, 5> points;
};

struct RoadTriangle
{
  std::array<Position, 3> points;
  std::array<uint32_t, 3> vertexIds{}; // Shared section/edge identity within this road.
  uint32_t panel = 0;
  bool shoulder = false;
};

struct RoadProduct
{
  std::vector<RoadSection> sections;
  std::vector<RoadTriangle> triangles;
  double routeCost = 0, maximumGrade = 0, maximumGradeChange = 0, maximumEarthwork = 0;
  uint64_t statesVisited = 0;
  // Exact piecewise planar surface, including shoulders. No infinite extension.
  bool height(double x, double z, double &y) const;
};

using RoadField = std::function<double(double, double)>;
// Non-finite fields mark forbidden terrain. Resistance must be nonnegative.
// Bounded forward position/heading search; never falls back to a straight line.
bool routeRoad(const Position &, const Position &, const RoadField &terrain, const RoadField &resistance, const RoadSettings &, RoadProduct &, std::string *error = nullptr);
// Explicit path and interior height locks support gates/bridge approaches/junctions.
// Endpoints are always locked. Water crossings themselves are Step 10 structures.
bool designRoad(const std::vector<Position> &path, const std::vector<RoadHeightLock> &, const RoadField &terrain, const RoadField &resistance, const RoadSettings &, RoadProduct &, std::string *error = nullptr);

struct RoutedRoad
{
  RoadLink link;
  RoadProduct design;
};

struct RoadNetworkProduct
{
  uint64_t dependencyRevision = 0;
  std::vector<RoutedRoad> roads;
};

// All required links must route successfully before publishing a network.
// Callers pass accepted Step 08 anchors and the revision of the final terrain/water fields.
bool buildRoadNetwork(const std::vector<RoadAnchor> &, const RoadField &terrain, const RoadField &resistance, const RoadSettings &, uint64_t dependencyRevision, RoadNetworkProduct &, std::string *error = nullptr);
} // namespace procedural::world::rewrite
