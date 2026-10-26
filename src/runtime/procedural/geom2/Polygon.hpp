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

// Drop the debris a boolean leaves behind, in metres of tolerance.
//
// A ring that has been through a boolean or two carries vertices that are not
// features of the shape: pairs a fraction of a millimetre apart, chains of
// edges a few centimetres long that wander to and fro across a straight wall,
// and vertices that sit on the line between their neighbours. None of it is a
// fault - `ringFault` passes all of it, because two points a micrometre apart
// are two points - and none of it is visible at the scale a building is drawn
// at. It matters anyway, because the next thing to touch the ring may amplify
// it: pushing every wall out by an eaves overhang turns a wall a centimetre
// long into a corner half a metre across, and a roof is refused over corners
// that were never in the plan.
//
// Three rules, applied until nothing changes: weld a vertex onto its
// predecessor when they are within `tolerance`, collapse an edge shorter than
// `shortestEdge` into its midpoint, and drop a vertex within `tolerance` of the
// line joining its neighbours. The first two move the outline by at most half
// their own threshold; the third by at most the tolerance.
//
// `false` when the result would not be a sound ring - fewer than three
// vertices, or a fault the simplification introduced - and the ring is left
// exactly as it was. Simplifying is an improvement, never a requirement.
bool simplifyRing(Ring &ring, double toleranceMetres, double shortestEdgeMetres);

// The same on an outer ring and every hole, each independently. A hole that
// collapses is dropped rather than kept unsimplified, since a hole too small to
// survive welding is too small to build.
void simplifyPolygon(Polygon &polygon, double toleranceMetres, double shortestEdgeMetres);

// The largest axis-aligned rectangle that fits inside a polygon, in a frame the
// caller chooses. `false` when nothing usable fits.
//
// The frame matters more than it sounds. A town lot is a quadrilateral with no
// two sides parallel, and the building on it is not: it is a set of ranges laid
// along one axis, because that is what a building is. So the question is not
// "the largest rectangle in this lot" but "the largest rectangle *along the
// street*", and the axis is an argument rather than something to discover.
//
// What it is for is to stop a planner laying its ranges in the lot's bounding
// box and intersecting afterwards. A rectangle clipped against an oblique lot
// edge is no longer a rectangle - it is a shape with a shaved corner, and the
// shaving is what the eaves amplify and the roof refuses. Laid inside a
// rectangle that is already in the lot, a range needs no clipping and keeps
// its right angles.
//
// Conservative by construction: a cell counts as ground only when all four of
// its corners are in the polygon, and the result is verified against the
// polygon before it is returned, so the rectangle is inside the lot rather
// than nearly inside it. `resolutionMetres` is the raster step - the answer is
// exact to about that much, and a lot is metres across, so centimetres are
// wasted work.
bool largestInscribedRectangle(const Polygon &polygon,
                               double axisX,
                               double axisY,
                               double resolutionMetres,
                               double &lowU,
                               double &lowV,
                               double &highU,
                               double &highV);

// Conversions at the library boundary. `false` when any coordinate is outside
// the representable grid (Quantise.hpp), which is a caller error rather than
// something to clamp.
bool toRing(const std::vector<Point2> &metres, Ring &ring);

std::vector<Point2> toMetres(const Ring &ring);

} // namespace procedural::geom2
