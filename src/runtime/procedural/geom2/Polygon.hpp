#pragma once

#include "procedural/geom2/Quantise.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace procedural::geom2 {

// A closed ring of grid points. The closing edge from back() to front() is
// implicit; a repeated final vertex is a fault, not a convention.
//
// There is deliberately no ConvexRing here. Convexity is measured (see
// `convex()`) and used to pick a faster path; it is never a preconditionc and no
// operation in this library rejects a ring for being concave. The shapes this
// generator actually produces - river corridors, road-graph faces, wall
// outlines, coastlines - are concave, and a type that claims otherwise only
// moves the problem into the callers.
using Ring = std::vector<Point>;

// A simple polygon: one outer ring, counter-clockwise, and any number of holes,
// clockwise, each strictly inside the outer ring and disjoint from each other.
struct Polygon
{
  Ring outer;
  std::vector<Ring> holes;

  bool empty() const
  {
    return outer.size() < 3;
  }
};

struct Box
{
  int64_t minX = 0;
  int64_t minY = 0;
  int64_t maxX = 0;
  int64_t maxY = 0;

  bool empty() const
  {
    return maxX < minX || maxY < minY;
  }

  bool contains(const Point &p) const
  {
    return p.x >= minX && p.x <= maxX && p.y >= minY && p.y <= maxY;
  }

  bool overlaps(const Box &other) const
  {
    return !(other.minX > maxX || other.maxX < minX || other.minY > maxY || other.maxY < minY);
  }
};

// Exact sign of the cross product (b - a) x (c - a): +1 left turn, -1 right
// turn, 0 exactly collinear. Routed through the adaptive predicates, so the sign
// is correct even when the three points are very nearly collinear - which is the
// case every arrangement bug is made of.
int orientation(const Point &a, const Point &b, const Point &c);

// Twice the signed area, in grid units squared, as an exact integer.
//
// Doubled and integral so it is exact: the shoelace sum of integer coordinates
// is an integer, and halving it would be the only lossy step. Callers wanting
// real area use `areaMetres`, which halves once at the end. __int128 because a
// ring spanning the full grid can reach 2^88.
__int128 doubleSignedArea(const Ring &ring);

double areaMetres(const Ring &ring);

// Outer minus holes. Negative when the holes exceed the outer ring, which is a
// validity fault rather than something to clamp.
double areaMetres(const Polygon &polygon);

// Counter-clockwise by signed area. A degenerate ring has no orientation and
// reports false; ask `degenerate()` rather than reading meaning into it.
bool counterClockwise(const Ring &ring);

void reverse(Ring &ring);

// True when every turn has the same sign, ignoring collinear vertices. A ring
// with fewer than three distinct points is not convex.
bool convex(const Ring &ring);

Box bounds(const Ring &ring);

Box bounds(const Polygon &polygon);

// Winding number of `point` about `ring`. Exact: uses the orientation predicate
// for the crossing test rather than a computed intersection, so a point exactly
// on a vertex or an edge is decided consistently.
//
// Zero means outside for a simple ring. A point exactly on the boundary is
// reported by `onBoundary` and is not claimed to be inside or outside here,
// because the two callers that care disagree about which it should be.
int windingNumber(const Ring &ring, const Point &point);

bool onBoundary(const Ring &ring, const Point &point);

// Even-odd containment for a simple ring; boundary points count as inside.
bool contains(const Ring &ring, const Point &point);

// Inside the outer ring and outside every hole. Boundary counts as inside.
bool contains(const Polygon &polygon, const Point &point);

// Whether segments ab and cd share any point, touching and collinear overlap
// included. Exact.
bool segmentsIntersect(const Point &a, const Point &b, const Point &c, const Point &d);

// Whether segments ab and cd share more than a single point: a transversal
// crossing, or a collinear overlap of positive length. Touching at an endpoint,
// or a T where one endpoint lies on the other segment, is not a proper
// crossing.
//
// The distinction matters because touching is a legitimate outcome and
// crossing is not. A symmetric difference can leave a hole tangent to its outer
// ring at a point; that region has no representation with strictly interior
// holes, so the canonical form admits the tangency and forbids only the
// crossing.
bool segmentsProperlyCross(const Point &a, const Point &b, const Point &c, const Point &d);

// Every way a ring can be unusable, as a message, or an empty string when it is
// sound. One function so that every operation in this library validates its
// input and its output against the same definition.
//
// Faults: fewer than three vertices, a repeated consecutive vertex (the closing
// duplicate included), zero area, and any self-intersection - a pinch vertex
// visited twice, and edges that cross or overlap.
std::string ringFault(const Ring &ring);

// Ring faults on every ring, plus: outer not counter-clockwise, a hole not
// clockwise, a hole escaping or crossing the outer ring, and two holes that
// overlap. Holes may touch the outer ring and each other at isolated points,
// which a boolean genuinely produces and which no strictly-interior
// representation can express.
std::string polygonFault(const Polygon &polygon);

// Conversions at the library boundary. `false` when any coordinate is outside
// the representable grid (Quantise.hpp), which is a caller error rather than
// something to clamp.
bool toRing(const std::vector<Point2> &metres, Ring &ring);

std::vector<Point2> toMetres(const Ring &ring);

} // namespace procedural::geom2
