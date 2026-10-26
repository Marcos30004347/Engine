#pragma once

#include "procedural/geom2/Polygon.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world::hierarchy {

using geom2::Point;
using geom2::Polygon;
using geom2::Ring;

using RegionId = uint64_t;
using EdgeId = uint64_t;

inline constexpr RegionId kNoRegion = 0;

// World-frame origin of a region's local coordinates.
//
// Children keep absolute geometric tolerances - a lot is planned to the
// centimetre whatever continent it sits on - so every region works near its own
// origin and the frame is what puts it back.
struct Frame
{
  double originX = 0;
  double originY = 0;

  Point toLocal(const Point &p) const
  {
    return {p.x - geom2::toUnit(originX), p.y - geom2::toUnit(originY)};
  }

  Point toWorld(const Point &p) const
  {
    return {p.x + geom2::toUnit(originX), p.y + geom2::toUnit(originY)};
  }
};

// What a region is made of. An explicit upstream label, never inferred from
// colour, shape or area.
enum class Cover : uint8_t
{
  Open,
  Forest,
  Rock,
  Water,
  Urban,
  Road,
  Building
};

const char *coverName(Cover);

// Measured upstream. Every value is a unit fraction except elevation.
struct Attributes
{
  double elevationMetres = 0;
  double slope = 0;
  double floodRisk = 0;
  double fertility = 0;
  double defence = 0;
  double demand = 0; // What this region has been asked to hold; sizes a power cell.
  bool buildable = true;
  bool navigableShore = false;
};

// What an edge represents. A **set**, not an enum: the same curve can be a
// river bank and a parish border and a farm-lane boundary at once, and the
// codebase's four existing representations of this - twelve bools on
// MapBoundaryEdge, an exclusive uint8_t on RoadGraphEdge, an exclusive enum on
// SurfaceContour, a vector of ids on SurfaceMapEdge - cannot all be right.
//
// A role set is also what makes retirement correct: revoking the farm
// producer's lane role leaves the parish border standing on the same curve
// instead of deleting a boundary someone else still references.
enum class EdgeRole : uint8_t
{
  RegionBoundary,
  Road,
  River,
  Coast,
  Wall,
  Rail,
  Administrative,
  LotBoundary
};

const char *edgeRoleName(EdgeRole);

struct EdgeRoleEntry
{
  EdgeRole role = EdgeRole::RegionBoundary;
  std::string producer; // Which generator claimed it, so it can be revoked alone.
  uint64_t feature = 0; // Which road, which river.
};

// One band of a widthed edge, as an offset range across the centreline.
//
// A zero-width edge splits a region in two. A widthed one splits it in three -
// side A, the ribbon, side B - and the ribbon is a region in its own right,
// with a cover and attributes, because that is what lets anything stand on it,
// collide with it or answer "am I on a road". Generalised to a stack, because
// the roles the codebase already has demand it:
//
//   road:  Ditch | Shoulder | Road      | Shoulder | Ditch
//   river: -     | RiverBank| RiverBed  | RiverBank| -
struct Band
{
  double fromMetres = 0; // Signed offset from the centreline, left negative.
  double toMetres = 0;
  Cover cover = Cover::Road;
  std::string note;
};

struct Edge
{
  EdgeId id = 0;
  std::vector<Point> path; // Two points for a straight edge; more for a curve.
  std::vector<EdgeRoleEntry> roles;
  std::vector<Band> bands; // Empty: a zero-width edge that splits without consuming.

  double halfWidthMetres() const;

  bool hasRole(EdgeRole) const;
};

// A shared edge between two sibling regions: the adjacency that turns the
// containment tree into the graph.
struct Portal
{
  RegionId neighbour = kNoRegion;
  EdgeId edge = 0;
  Point midpoint;
};

// The one geometric hand-off between a parent generator and its children.
//
// Concave and holed, because the shapes this generator actually produces are:
// a river corridor's banks, a road graph's faces, a wall's union outline, a
// coastline. Convexity is a measured property used to pick a faster path, never
// a precondition - nothing here rejects a region for being concave.
struct Region
{
  RegionId id = kNoRegion;
  RegionId parent = kNoRegion;
  Frame frame;
  Polygon boundary; // Local to `frame`.
  Cover cover = Cover::Open;
  Attributes attributes;
  std::vector<Portal> portals;

  // Which generator produced this region, and from which of its inputs.
  // Lineage, not containment: a ribbon crossing five regions yields five
  // pieces, each parented to the region it crosses, all naming one road.
  std::string producer;
  std::vector<RegionId> sources;

  bool convex = false; // Measured on construction.

  double areaMetres() const;
};

// Builds a region from a polygon, measuring convexity rather than asserting it.
Region makeRegion(RegionId id, RegionId parent, Polygon boundary, const Frame & = {});

// The whole world as one structure: containment through `parent`, adjacency
// through `portals`, and the edges every boundary is drawn from.
class RegionTree
{
public:
  RegionId add(Region);

  const Region *find(RegionId) const;

  Region *findMutable(RegionId);

  std::vector<RegionId> childrenOf(RegionId) const;

  const std::vector<Region> &regions() const
  {
    return regions_;
  }

  EdgeId addEdge(Edge);

  const Edge *findEdge(EdgeId) const;

  const std::vector<Edge> &edges() const
  {
    return edges_;
  }

  // Next free identity. Identities are never reused, so a tombstone stays
  // distinguishable from a fresh region with the same geometry.
  RegionId nextRegionId()
  {
    return ++lastRegion_;
  }

  EdgeId nextEdgeId()
  {
    return ++lastEdge_;
  }

private:
  std::vector<Region> regions_;
  std::vector<Edge> edges_;
  RegionId lastRegion_ = 0;
  EdgeId lastEdge_ = 0;
};

} // namespace procedural::world::hierarchy
