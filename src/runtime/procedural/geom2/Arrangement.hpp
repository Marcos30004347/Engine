#pragma once

#include "procedural/geom2/Polygon.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace procedural::geom2 {

// The planar arrangement every other operation is built on.
//
// Input is a set of rings, each tagged with the index of the input it belongs
// to. The rings may cross each other, cross themselves, touch, overlap, be
// concave or carry holes; none of that is a special case. Output is a
// half-edge graph in which no two edges cross, plus the faces of that graph,
// each labelled with a winding number **per input**.
//
// That per-input winding vector is the design decision this library turns on.
// A conventional boolean carries one inside/outside bit per operand and returns
// anonymous rings; carrying the whole vector costs the same sweep and yields
// every boolean at once, plus which inputs contributed each face and which lie
// either side of each edge. That is the provenance the generators need -
// which contour a face came from, what lies across a road - and it falls out of
// a sweep that has to happen anyway.

// One directed ring edge as supplied by a caller, before any splitting.
struct InputRing
{
  Ring ring;
  uint32_t input = 0; // Which logical polygon this ring belongs to.

  // An open path rather than a closed ring: the last vertex is not joined back
  // to the first, and the path contributes nothing to any winding number.
  //
  // This is how a cut line or a zero-width centreline enters the arrangement.
  // Such an edge still divides the faces either side of it, which a winding
  // contribution of zero would not express - so its half-edges are marked
  // `hard` and consumers treat them as boundaries that must not be dissolved.
  bool open = false;
};

struct HalfEdge
{
  uint32_t origin = 0; // Vertex index.
  uint32_t twin = 0;
  uint32_t next = 0; // Next half-edge of the same face, interior on the left.
  uint32_t face = 0;

  // Net count of input-i ring edges running along this half-edge in its own
  // direction, minus those running against it. Winding is propagated across
  // edges with this, so it is the only provenance the labelling needs.
  std::vector<int32_t> crossing;

  // Came from an open path. The faces either side may have identical winding
  // and must still be kept apart, so this edge is never dissolved.
  bool hard = false;
};

struct Face
{
  // Winding number per input. A face is inside input i exactly when
  // winding[i] != 0 (non-zero rule) or when it is odd (even-odd rule); both
  // are available because the count itself is kept, not a bit.
  std::vector<int32_t> winding;

  // One half-edge of each boundary cycle. The first is the outer cycle; the
  // rest bound holes. The unbounded face has no outer cycle.
  std::vector<uint32_t> cycles;

  bool unbounded = false;
};

struct Arrangement
{
  std::vector<Point> vertices;
  std::vector<HalfEdge> halfEdges;
  std::vector<Face> faces;
  uint32_t inputs = 0;
  uint32_t unboundedFace = 0;

  // Rounds of snap iteration the construction needed. One means the input's
  // intersections all landed on the grid already; a high count means the input
  // has many near-coincident features, which is worth seeing in a test.
  uint32_t snapRounds = 0;
};

struct ArrangementLimits
{
  // Bounded work. Generation must fail with a reason rather than run for an
  // unbounded time on pathological input.
  uint32_t maximumSegments = 200000;
  uint32_t maximumVertices = 200000;
  uint32_t maximumSnapRounds = 16;
};

// Builds the arrangement. Returns false and fills `error` on invalid input or
// an exceeded budget, leaving `out` unspecified.
//
// Intersections of two integer segments are rational, so each is snapped to the
// nearest grid point. Snapping can move a segment across a vertex it did not
// previously meet, so intersection detection is repeated until no new split is
// produced. This terminates: the grid is finite, splits only ever add vertices
// inside the input's bounding box, and segments only ever get shorter.
bool buildArrangement(const std::vector<InputRing> &rings, const ArrangementLimits &limits, Arrangement &out, std::string *error = nullptr);

// Every way an arrangement can be inconsistent, as a message, or empty when it
// is sound: twin involution, next/origin agreement, every half-edge reachable
// in exactly one face cycle, exactly one unbounded face, bounded faces with
// positive area and the unbounded face with negative area.
//
// Used by the tests on every arrangement they build, so that a structural
// break is caught where it happens rather than three operations downstream.
std::string arrangementFault(const Arrangement &);

// Signed area of the cycle starting at `halfEdge`, in metres squared.
double cycleAreaMetres(const Arrangement &, uint32_t halfEdge);

// Area of a face: its outer cycle less the inner cycles bounding its holes.
// Summing the cycles does that on its own, because an inner cycle is traced
// with the face on its left and so carries negative area.
double faceAreaMetres(const Arrangement &, uint32_t face);

} // namespace procedural::geom2
