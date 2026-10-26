#pragma once

#include "procedural/geom2/Boolean.hpp"

namespace procedural::geom2 {

// Partitioning a region among sites.
//
// Both diagrams here are built the same way: a cell is the intersection of the
// half-planes that beat every other site, clipped to the region. That costs a
// pass over the sites per cell rather than a sweep, and in exchange it is
// correct for a concave region with holes, which a dual-of-the-Delaunay
// construction is not - the dual gives convex cells over the whole plane and
// leaves the caller to clip them, which is the step that was never
// concave-safe.

struct Site
{
  Point position;

  // Power diagram weight. Zero everywhere gives the ordinary Voronoi diagram;
  // a larger weight claims more ground, so a district's cell can be sized by
  // its demand instead of by how its neighbours happen to be spaced.
  //
  // In metres squared: the boundary between two sites moves toward the lighter
  // one by (wHeavy - wLight) / (2 * distance).
  double weight = 0;
};

struct PartitionCell
{
  size_t site = 0;
  Polygon polygon;
};

struct PartitionOptions
{
  ArrangementLimits limits;
};

// One cell per site that claims any ground. A site whose cell is empty - buried
// under a heavier neighbour, or outside the region - is simply absent from the
// result rather than reported as a failure, because that is an ordinary
// outcome of weighting.
bool partitionRegion(const Polygon &region, const std::vector<Site> &sites, const PartitionOptions &, std::vector<PartitionCell> &out, std::string *error = nullptr);

// Lloyd relaxation: each site moves to the centroid of its own cell, repeated.
//
// Extracted here because it was previously written inline in the ward planner,
// where it could not be reused, tested, or applied to a weighted diagram.
bool relax(const Polygon &region, std::vector<Site> &sites, uint32_t iterations, const PartitionOptions &, std::string *error = nullptr);

// Area-weighted centroid of a polygon, holes included. Returns false for a
// polygon of no area, which has no centroid to report.
bool centroid(const Polygon &, Point &out);

} // namespace procedural::geom2
