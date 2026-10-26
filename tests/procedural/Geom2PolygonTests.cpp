// geom2 core types: the integer grid, the exact ring predicates and the
// validity definition every later operation is checked against.
//
// These tests are deliberately built out of the cases that break a float
// implementation - collinear vertices, a point exactly on an edge, a pinch
// vertex, a spike that folds back - because a suite of well-separated convex
// squares would pass against an implementation that is wrong everywhere it
// matters.
//
// Concave input is not a special section here. It appears throughout, because
// the shapes this generator produces (river corridors, road-graph faces, wall
// outlines, coastlines) are concave and the library has no convex-only stage.

#include "procedural/geom2/Polygon.hpp"

#include <cmath>
#include <iostream>
#include <string>

using namespace procedural;
using namespace procedural::geom2;

namespace {

int failures = 0;

void check(const bool condition, const std::string &what)
{
  if (condition)
  {
    std::cout << "  PASSED " << what << "\n";

    return;
  }

  std::cerr << "  FAILED " << what << "\n";
  ++failures;
}

Point at(const double x, const double y)
{
  return {toUnit(x), toUnit(y)};
}

// A unit square, counter-clockwise.
Ring square(const double size = 1.0)
{
  return {at(0, 0), at(size, 0), at(size, size), at(0, size)};
}

// The canonical concave shape: an L, counter-clockwise, with one reflex corner.
Ring ell()
{
  return {at(0, 0), at(3, 0), at(3, 1), at(1, 1), at(1, 3), at(0, 3)};
}

// A U, counter-clockwise: two arms and a notch between them. Two reflex
// corners, and the shape an outward offset merges into one ring.
Ring youShape()
{
  return {at(0, 0), at(3, 0), at(3, 3), at(2, 3), at(2, 1), at(1, 1), at(1, 3), at(0, 3)};
}

void quantisationTests()
{
  std::cout << "Quantisation\n";

  check(toUnit(1.0) == 1000000, "one metre is one million units");
  check(toUnit(-1.0) == -1000000, "negative metres scale symmetrically");
  check(toUnit(0.0000005) == 1, "half a micrometre rounds away from zero");
  check(toUnit(-0.0000005) == -1, "and does so on both sides of the origin");

  // Rounding must not depend on which side of the origin a shape sits: a
  // translation by a whole number of metres has to be a rigid motion on the
  // grid, or a polygon changes shape when the world origin moves.
  bool rigid = true;

  for (int i = -50; i <= 50; ++i)
  {
    const double v = double(i) * 0.1 + 0.00000025;

    if (toUnit(v + 7.0) - toUnit(v) != toUnit(7.0))
    {
      rigid = false;
    }
  }

  check(rigid, "translation by whole metres is rigid on the grid");

  check(representable(2.0e6) && !representable(3.0e6), "the representable range is about 2,200 km");
  check(!representable(std::nan("")) && !representable(1.0 / 0.0), "non-finite metres are not representable");

  Ring ring;
  check(!toRing({{1.0e9, 0.0}, {0.0, 0.0}, {0.0, 1.0}}, ring), "out-of-range input is rejected, not clamped");

  // Two metre coordinates a nanometre apart land on one grid point. The
  // zero-length edge between them is dropped rather than becoming a fault the
  // caller has no way to act on.
  check(toRing({{0.0, 0.0}, {1.0e-9, 0.0}, {1.0, 0.0}, {1.0, 1.0}}, ring) && ring.size() == 3, "coincident vertices collapse on quantisation");
}

void orientationTests()
{
  std::cout << "Orientation and area\n";

  check(orientation(at(0, 0), at(1, 0), at(0, 1)) > 0, "counter-clockwise triple is positive");
  check(orientation(at(0, 0), at(1, 0), at(0, -1)) < 0, "clockwise triple is negative");
  check(orientation(at(0, 0), at(1, 1), at(2, 2)) == 0, "collinear triple is exactly zero");

  // The reason the predicate is here at all. These three are collinear, and the
  // naive determinant on doubles reports a nonzero sign for cases of this shape
  // because the products lose their low bits.
  const Point a{0, 0};
  const Point b{1000000000000LL, 1000000000001LL};
  const Point c{2000000000000LL, 2000000000002LL};

  check(orientation(a, b, c) == 0, "collinear at grid extremes is still exactly zero");

  const Point d{2000000000000LL, 2000000000003LL};

  check(orientation(a, b, d) != 0, "a one-unit deviation at grid extremes is detected");

  check(std::abs(areaMetres(square(2.0)) - 4.0) < 1e-9, "square area in metres");
  check(areaMetres(ell()) > 0.0 && std::abs(areaMetres(ell()) - 5.0) < 1e-9, "concave L area is exact");
  check(counterClockwise(square()), "counter-clockwise ring reports so");

  Ring reversed = square();
  reverse(reversed);

  check(!counterClockwise(reversed), "reversed ring reports clockwise");
  check(std::abs(areaMetres(reversed) + 1.0) < 1e-9, "reversed ring has negated signed area");
}

void convexityTests()
{
  std::cout << "Convexity is measured, never required\n";

  check(convex(square()), "a square is convex");
  check(!convex(ell()), "an L is not convex");
  check(!convex(youShape()), "a U is not convex");

  // A square with an extra vertex in the middle of an edge is still convex: a
  // collinear vertex is not a corner. Rejecting it would make convexity depend
  // on how finely a caller sampled its own boundary.
  const Ring subdivided = {at(0, 0), at(1, 0), at(2, 0), at(2, 2), at(0, 2)};

  check(convex(subdivided), "collinear vertices do not make a ring concave");

  // A star polygon's turns all agree in sign, so a consistent-turn test alone
  // calls it convex. It winds twice, which the axis-reversal count catches.
  const Ring star = {at(0, 2), at(3, 3), at(0, 4), at(1.5, 1), at(3, 4)};

  check(!convex(star), "a star polygon is not reported convex");

  // Concavity must never be a reason to reject: every predicate above answers
  // for the L as readily as for the square.
  check(ringFault(ell()).empty(), "a concave ring is valid input");
  check(ringFault(youShape()).empty(), "a two-notch concave ring is valid input");
}

void containmentTests()
{
  std::cout << "Containment\n";

  const Ring l = ell();

  check(contains(l, at(0.5, 0.5)), "a point in the corner of the L is inside");
  check(contains(l, at(2.5, 0.5)), "a point in the arm of the L is inside");
  check(!contains(l, at(2.5, 2.5)), "a point in the notch of the L is outside");
  check(!contains(l, at(5, 5)), "a point far away is outside");

  // The cases a float implementation decides differently on different days.
  check(onBoundary(l, at(1.5, 1)), "a point exactly on an edge is on the boundary");
  check(onBoundary(l, at(3, 1)), "a vertex is on the boundary");
  check(contains(l, at(1.5, 1)), "a boundary point counts as contained");

  // The reflex vertex is the one a ray cast is most likely to miscount.
  check(onBoundary(l, at(1, 1)), "the reflex vertex is on the boundary");

  // A ray leaving the reflex vertex horizontally grazes two vertices at the
  // same height. The half-open crossing rule is what keeps that counted once.
  // One grid unit either side of the notch floor, which is the finest question
  // that can be asked: an offset below a micrometre quantises onto the edge
  // itself, and both answers would then be "on the boundary".
  check(!contains(l, at(2, 1.000001)), "a point one unit above the notch floor is outside");
  check(contains(l, at(2, 0.999999)), "a point one unit below the notch floor is inside");
  check(onBoundary(l, at(2, 1.0000001)), "a sub-unit offset lands on the edge, not beside it");

  const Ring u = youShape();

  check(!contains(u, at(1.5, 2)), "the notch of a U is outside");
  check(contains(u, at(0.5, 2)), "the left arm of a U is inside");
  check(contains(u, at(2.5, 2)), "the right arm of a U is inside");

  check(windingNumber(square(), at(0.5, 0.5)) == 1, "winding is one inside a CCW ring");

  Ring clockwise = square();
  reverse(clockwise);

  check(windingNumber(clockwise, at(0.5, 0.5)) == -1, "winding is minus one inside a CW ring");
}

void polygonTests()
{
  std::cout << "Polygons with holes\n";

  Polygon p;
  p.outer = square(10.0);
  Ring hole = {at(2, 2), at(4, 2), at(4, 4), at(2, 4)};
  reverse(hole); // Holes are clockwise.
  p.holes.push_back(hole);

  check(polygonFault(p).empty(), "a square with a square hole is valid");
  check(std::abs(areaMetres(p) - (100.0 - 4.0)) < 1e-9, "hole area is subtracted");
  check(contains(p, at(1, 1)), "a point outside the hole is inside the polygon");
  check(!contains(p, at(3, 3)), "a point inside the hole is outside the polygon");
  check(contains(p, at(3, 2)), "a point on the hole boundary is inside the polygon");

  Polygon wrongWinding = p;
  reverse(wrongWinding.holes[0]);

  check(!polygonFault(wrongWinding).empty(), "a counter-clockwise hole is a fault");

  Polygon escaped = p;
  escaped.holes[0] = {at(8, 8), at(12, 8), at(12, 12), at(8, 12)};
  reverse(escaped.holes[0]);

  check(!polygonFault(escaped).empty(), "a hole crossing the outer ring is a fault");

  // A concave outer ring with a hole: the combination the settlement fabric
  // already produces and the current code cannot represent.
  Polygon concave;
  concave.outer = ell();
  Ring inner = {at(0.25, 0.25), at(0.75, 0.25), at(0.75, 0.75), at(0.25, 0.75)};
  reverse(inner);
  concave.holes.push_back(inner);

  check(polygonFault(concave).empty(), "a concave polygon with a hole is valid");
}

void faultTests()
{
  std::cout << "Validity faults\n";

  check(!ringFault({at(0, 0), at(1, 1)}).empty(), "two vertices is not a ring");
  check(!ringFault({at(0, 0), at(1, 0), at(1, 0), at(1, 1)}).empty(), "a repeated consecutive vertex is a fault");
  check(!ringFault({at(0, 0), at(1, 0), at(2, 0)}).empty(), "a ring with no area is a fault");

  // A bowtie: the two edges cross. Its shoelace area is nonzero, so an area
  // test alone would accept it.
  check(!ringFault({at(0, 0), at(2, 2), at(2, 0), at(0, 2)}).empty(), "a self-crossing ring is a fault");

  // A pinch: the ring touches itself at a vertex without any edge crossing.
  check(!ringFault({at(0, 0), at(2, 0), at(1, 1), at(2, 2), at(0, 2), at(1, 1)}).empty(), "a ring visiting a vertex twice is a fault");

  // A spike: the ring runs out along a line and comes straight back. Every turn
  // is collinear, so no edges cross in the ordinary sense.
  check(!ringFault({at(0, 0), at(2, 0), at(3, 0), at(2, 0), at(2, 2), at(0, 2)}).empty(), "a zero-width spike is a fault");

  check(ringFault(square()).empty(), "a square is sound");
  check(ringFault(ell()).empty(), "an L is sound");
}

void segmentTests()
{
  std::cout << "Segment intersection\n";

  check(segmentsIntersect(at(0, 0), at(2, 2), at(0, 2), at(2, 0)), "crossing segments intersect");
  check(!segmentsIntersect(at(0, 0), at(1, 0), at(0, 1), at(1, 1)), "parallel segments do not");
  check(segmentsIntersect(at(0, 0), at(2, 0), at(1, 0), at(3, 0)), "collinear overlapping segments intersect");
  check(segmentsIntersect(at(0, 0), at(2, 0), at(2, 0), at(4, 0)), "collinear segments touching at a point intersect");
  check(!segmentsIntersect(at(0, 0), at(1, 0), at(2, 0), at(3, 0)), "disjoint collinear segments do not");
  check(segmentsIntersect(at(0, 0), at(2, 0), at(1, 0), at(1, 2)), "a T junction intersects");
  check(segmentsIntersect(at(0, 0), at(2, 2), at(1, 1), at(3, 0)), "an endpoint exactly on a segment intersects");
}

// Layer 1: properties that must hold for every ring, convex or not. These are
// the tests that catch a class of error rather than an instance of one.
void propertyTests()
{
  std::cout << "Invariants\n";

  const Ring shapes[] = {square(), square(7.0), ell(), youShape()};

  bool areaSignFollowsOrientation = true;
  bool areaIsTranslationInvariant = true;
  bool rotationOfStartIsIrrelevant = true;

  for (const auto &ring : shapes)
  {
    Ring flipped = ring;
    reverse(flipped);

    if (doubleSignedArea(ring) != -doubleSignedArea(flipped))
    {
      areaSignFollowsOrientation = false;
    }

    Ring moved = ring;

    for (auto &p : moved)
    {
      p.x += toUnit(1234.0);
      p.y -= toUnit(567.0);
    }

    if (doubleSignedArea(moved) != doubleSignedArea(ring))
    {
      areaIsTranslationInvariant = false;
    }

    // The starting vertex is an encoding detail, not part of the shape. Any
    // operation whose answer depends on it is order-dependent, which is the
    // failure mode a sweep shows first.
    for (size_t r = 1; r < ring.size(); ++r)
    {
      Ring rotated;

      for (size_t i = 0; i < ring.size(); ++i)
      {
        rotated.push_back(ring[(i + r) % ring.size()]);
      }

      if (doubleSignedArea(rotated) != doubleSignedArea(ring) || convex(rotated) != convex(ring) || ringFault(rotated).empty() != ringFault(ring).empty())
      {
        rotationOfStartIsIrrelevant = false;
      }
    }
  }

  check(areaSignFollowsOrientation, "reversing a ring negates its signed area exactly");
  check(areaIsTranslationInvariant, "signed area is invariant under integer translation");
  check(rotationOfStartIsIrrelevant, "area, convexity and validity do not depend on the starting vertex");

  // Containment and boundary must partition the plane with no overlap: a point
  // is inside, outside, or on the boundary, and never two of those.
  bool partitioned = true;

  for (const auto &ring : shapes)
  {
    const Box box = bounds(ring);

    for (int64_t x = box.minX - toUnit(1.0); x <= box.maxX + toUnit(1.0); x += toUnit(0.25))
    {
      for (int64_t y = box.minY - toUnit(1.0); y <= box.maxY + toUnit(1.0); y += toUnit(0.25))
      {
        const Point p{x, y};
        const bool boundary = onBoundary(ring, p);
        const bool inside = windingNumber(ring, p) != 0;

        if (boundary && inside && !contains(ring, p))
        {
          partitioned = false;
        }

        if (!boundary && inside != contains(ring, p))
        {
          partitioned = false;
        }
      }
    }
  }

  check(partitioned, "inside, outside and boundary agree over a dense sample");
}

// Simplification: what it removes, what it must not, and when it refuses.
void simplifyTests()
{
  std::cout << "Dropping the debris a boolean leaves behind\n";

  // A square is already as simple as it gets.
  {
    Ring square{at(0, 0), at(10, 0), at(10, 10), at(0, 10)};
    const Ring before = square;

    check(simplifyRing(square, .02, .45) && square == before,
          "a clean square is returned unchanged");
  }

  // A vertex in the middle of a straight wall carries no information.
  {
    Ring split{at(0, 0), at(5, 0), at(10, 0), at(10, 10), at(0, 10)};

    check(simplifyRing(split, .02, .45) && split.size() == 4u,
          "a vertex on the line between its neighbours is dropped");
  }

  // The case that sent fifty four buildings up bare: a wall cut by a boolean
  // into a chain of short edges that wanders a few millimetres either side of
  // where the wall is.
  {
    Ring ragged{at(0, 0), at(10, 0), at(10, 10)};

    for (int step = 9; step >= 1; --step)
    {
      ragged.push_back(at(double(step), 10 + (step % 2 == 0 ? .004 : -.004)));
    }

    ragged.push_back(at(0, 10));

    const double area = areaMetres(ragged);

    check(simplifyRing(ragged, .02, .45) && ragged.size() == 4u,
          "a wall diced into a wandering chain of short edges comes back one wall");
    check(std::abs(areaMetres(ragged) - area) < .5,
          "and the shape it comes back as is the shape it was");
  }

  // Two vertices a fraction of a millimetre apart are one vertex. This is what
  // `ringFault` cannot say, because they are genuinely two points.
  {
    Ring welded{at(0, 0), at(10, 0), at(10.00002, 0.00001), at(10, 10), at(0, 10)};

    check(ringFault(welded).empty(), "a pair a hundredth of a millimetre apart is not a fault");
    check(simplifyRing(welded, .02, .45) && welded.size() == 4u, "...and simplifying welds it");
  }

  // The refusal. A shape whose debris *is* the shape must come back untouched
  // rather than collapsed, because a caller that asked for an improvement did
  // not ask to lose its polygon.
  {
    Ring sliver{at(0, 0), at(.1, 0), at(.1, .1), at(0, .1)};
    const Ring before = sliver;

    check(!simplifyRing(sliver, .02, .45) && sliver == before,
          "a square smaller than the shortest edge is refused, not flattened");
  }

  // Orientation survives, or a hole would come back as an outer ring.
  {
    Ring clockwise{at(0, 10), at(10, 10), at(10, 0), at(0, 0)};
    clockwise.insert(clockwise.begin() + 1, at(5, 10.001));

    const bool ok = simplifyRing(clockwise, .02, .45);

    check(ok && !counterClockwise(clockwise), "a clockwise ring stays clockwise");
  }

  // A polygon simplifies its holes too, and keeps them holes.
  {
    Polygon plan;
    plan.outer = Ring{at(0, 0), at(20, 0), at(20, 20), at(0, 20)};
    plan.outer.insert(plan.outer.begin() + 1, at(10, .001));
    plan.holes.push_back(Ring{at(5, 15), at(15, 15), at(15, 5), at(5, 5)});
    plan.holes.front().insert(plan.holes.front().begin() + 1, at(10, 15.001));

    simplifyPolygon(plan, .02, .45);

    check(plan.outer.size() == 4u && plan.holes.size() == 1u && plan.holes.front().size() == 4u,
          "a polygon simplifies its outer ring and its holes");
    check(polygonFault(plan).empty(), "and the result is a sound polygon");
  }
}

} // namespace

int main()
{
  std::cout << "geom2 polygon tests\n";

  quantisationTests();
  orientationTests();
  convexityTests();
  containmentTests();
  polygonTests();
  faultTests();
  segmentTests();
  propertyTests();
  simplifyTests();

  if (failures != 0)
  {
    std::cerr << failures << " geom2 polygon test(s) failed\n";

    return 1;
  }

  std::cout << "All geom2 polygon tests passed\n";

  return 0;
}
