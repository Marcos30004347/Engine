// Cutting and offsetting.
//
// The concave cases are the point of this suite. An inward offset that closes a
// reflex notch, one that splits a shape in two, an outward offset that merges
// two arms of a U into one ring and then into a hole, and a cut that produces
// three pieces rather than two: all of these are ordinary results of the
// winding resolution, and all of them are what a convex-only offset gets wrong.

#include "procedural/geom2/Offset.hpp"

#include <algorithm>
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

Ring box(const double x0, const double y0, const double x1, const double y1)
{
  return {at(x0, y0), at(x1, y0), at(x1, y1), at(x0, y1)};
}

Polygon polygonOf(Ring ring)
{
  Polygon p;
  p.outer = std::move(ring);

  return p;
}

// A U whose base is one metre thick and whose arms are two metres wide. The
// base parts under an inward offset of more than half a metre while the arms
// survive, so the shape splits in two - the topology change a naive per-edge
// offset folds over instead of resolving.
Ring youShape()
{
  return {at(0, 0), at(6, 0), at(6, 5), at(4, 5), at(4, 1), at(2, 1), at(2, 5), at(0, 5)};
}

double areaOf(const std::vector<Polygon> &polygons)
{
  double area = 0.0;

  for (const auto &polygon : polygons)
  {
    const std::string fault = polygonFault(polygon);

    if (!fault.empty())
    {
      std::cerr << "    (invalid output polygon: " << fault << ")\n";
      ++failures;
    }

    area += areaMetres(polygon);
  }

  return area;
}

bool shrink(const Polygon &polygon, const double metres, std::vector<Polygon> &out)
{
  std::string error;
  OffsetOptions options;
  options.join = JoinStyle::Miter;

  if (!offsetPolygon(polygon, -metres, options, out, &error))
  {
    std::cerr << "    (offset failed: " << error << ")\n";

    return false;
  }

  return true;
}

bool grow(const Polygon &polygon, const double metres, std::vector<Polygon> &out)
{
  std::string error;
  OffsetOptions options;
  options.join = JoinStyle::Miter;

  if (!offsetPolygon(polygon, metres, options, out, &error))
  {
    std::cerr << "    (offset failed: " << error << ")\n";

    return false;
  }

  return true;
}

void convexOffsetTests()
{
  std::cout << "Offsetting a convex shape\n";

  const Polygon square = polygonOf(box(0, 0, 10, 10));
  std::vector<Polygon> out;

  check(shrink(square, 1.0, out) && out.size() == 1, "shrinking a square leaves one square");
  check(std::abs(areaOf(out) - 64.0) < 0.02, "and its area is the inset square's");

  check(grow(square, 1.0, out) && out.size() == 1, "growing a square leaves one square");
  check(std::abs(areaOf(out) - 144.0) < 0.02, "and its area is the outset square's, with mitred corners");

  // Shrinking past the half width empties the shape. A result, not a failure.
  check(shrink(square, 5.5, out) && out.empty(), "shrinking a square past its half width empties it");
  check(shrink(square, 5.0, out) && areaOf(out) < 0.01, "shrinking exactly to the centre leaves nothing measurable");

  // Round joins on a square cost area at the corners against a mitre.
  std::vector<Polygon> rounded;
  std::string error;
  OffsetOptions round;
  round.join = JoinStyle::Round;

  check(offsetPolygon(square, 1.0, round, rounded, &error) && rounded.size() == 1, "growing with round joins: " + error);

  // The exact answer is 100 + 4 x 10 x 1 + pi, and the join polygon
  // circumscribes its arc, so the result approaches it from above as the
  // tolerance tightens.
  check(areaOf(rounded) < 144.0 && std::abs(areaOf(rounded) - (140.0 + 3.14159265)) < 0.2, "a round join rounds the corners off the mitred area");

  std::vector<Polygon> finer;
  OffsetOptions tight = round;
  tight.arcToleranceMetres = 0.005;

  check(offsetPolygon(square, 1.0, tight, finer, &error) && std::abs(areaOf(finer) - (140.0 + 3.14159265)) < 0.02, "and a tighter arc tolerance converges on the true area");

  // Zero is the identity.
  check(grow(square, 0.0, out) && std::abs(areaOf(out) - 100.0) < 1e-9, "a zero offset is the identity");
}

void concaveOffsetTests()
{
  std::cout << "Offsetting concave shapes\n";

  const Polygon u = polygonOf(youShape());
  std::vector<Polygon> out;

  // The base of the U is one metre thick, so shrinking past half of it parts
  // the two arms: the topology change a naive per-edge offset folds over.
  check(shrink(u, 0.2, out) && out.size() == 1, "a small inward offset keeps the U in one piece");

  check(shrink(u, 0.6, out) && out.size() == 2, "shrinking past the base thickness splits the U in two");
  check(areaOf(out) > 0.0, "and both pieces are valid polygons");

  // A U's notch closes from its sides and its mouth at the same rate, so it
  // never encloses a void. A keyhole does: a narrow neck into a wide chamber,
  // which is a courtyard reached by an alley.
  const Polygon keyhole = polygonOf({at(0, 0), at(10, 0), at(10, 10), at(5.5, 10), at(5.5, 8), at(7, 8), at(7, 4), at(3, 4), at(3, 8), at(4.5, 8), at(4.5, 10), at(0, 10)});

  check(std::abs(areaMetres(keyhole) - 82.0) < 1e-9, "the keyhole is a hundred less its chamber and neck");

  check(grow(keyhole, 0.3, out) && out.size() == 1 && out[0].holes.empty(), "a small outward offset leaves the neck open");

  check(grow(keyhole, 0.6, out) && out.size() == 1 && out[0].holes.size() == 1, "growing past half the neck width seals it and makes the chamber a hole");

  check(grow(keyhole, 2.1, out) && out.size() == 1 && out[0].holes.empty(), "growing past half the chamber width fills the hole in");

  // An L: the reflex corner is where a miter join has nothing to do and a
  // naive per-edge offset would leave a gap.
  const Polygon l = polygonOf({at(0, 0), at(6, 0), at(6, 2), at(2, 2), at(2, 6), at(0, 6)});

  check(std::abs(areaMetres(l.outer) - 20.0) < 1e-9, "the L measures twenty square metres");

  check(shrink(l, 0.5, out) && out.size() == 1, "shrinking an L keeps it in one piece");
  check(std::abs(areaOf(out) - 9.0) < 0.01, "and gives the L inset by half a metre on every edge");

  check(grow(l, 0.5, out) && out.size() == 1 && out[0].holes.empty(), "growing an L keeps it simple");
  check(std::abs(areaOf(out) - 33.0) < 0.01, "and gives the L outset by half a metre, mitred at the reflex corner");
}

void holeOffsetTests()
{
  std::cout << "Offsetting through holes\n";

  Polygon ring;
  ring.outer = box(0, 0, 10, 10);
  Ring hole = box(4, 4, 6, 6);
  reverse(hole);
  ring.holes.push_back(hole);

  std::vector<Polygon> out;

  // Growing the polygon shrinks its hole, because the hole's boundary moves
  // the same way the outer one does: outward from the material.
  check(grow(ring, 0.5, out) && out.size() == 1 && out[0].holes.size() == 1, "growing a holed square keeps the hole");
  check(std::abs(areaOf(out) - (121.0 - 1.0)) < 0.05, "the outer grows and the hole shrinks");

  // Growing past the hole's half width closes it completely.
  check(grow(ring, 1.2, out) && out.size() == 1 && out[0].holes.empty(), "growing past the hole's half width closes it");

  // Shrinking opens the hole wider.
  check(shrink(ring, 0.5, out) && out.size() == 1 && out[0].holes.size() == 1, "shrinking a holed square keeps the hole");
  check(std::abs(areaOf(out) - (81.0 - 9.0)) < 0.05, "the outer shrinks and the hole grows");
}

void ribbonTests()
{
  std::cout << "Ribbons from centrelines\n";

  std::vector<Polygon> out;
  std::string error;

  const std::vector<Point> straight{at(0, 0), at(10, 0)};

  check(offsetPolyline(straight, 1.0, {}, out, &error) && out.size() == 1, "a straight centreline makes one ribbon: " + error);

  // Ten metres of centreline at a metre either side, plus a cap at each end.
  // A cap is a reversal, where no join style but a round one has anything to
  // span, so both ends are capped whatever the caller asked for.
  check(std::abs(areaOf(out) - (20.0 + 3.14159265)) < 0.15, "of the centreline length times the width, plus the two end caps");

  // A meandering centreline, which is what a river actually is. The ribbon
  // must stay one piece and stay valid through every bend.
  const std::vector<Point> meander{at(0, 0), at(3, 1.5), at(6, -1.5), at(9, 1.5), at(12, 0)};

  check(offsetPolyline(meander, 0.8, {}, out, &error) && out.size() == 1, "a meandering centreline makes one ribbon: " + error);
  check(areaOf(out) > 0.0, "which is a valid polygon");

  // A centreline that doubles back close to itself: the two passes merge into
  // one ribbon rather than crossing.
  const std::vector<Point> hairpin{at(0, 0), at(10, 0), at(10, 1), at(0, 1)};

  check(offsetPolyline(hairpin, 1.0, {}, out, &error) && out.size() == 1, "a hairpin merges into one ribbon: " + error);
  check(out.size() == 1 && out[0].holes.empty(), "with no hole left between the passes");
}

void cutTests()
{
  std::cout << "Cutting\n";

  std::vector<Polygon> out;
  std::string error;

  const Polygon square = polygonOf(box(0, 0, 10, 10));

  check(cutPolygon(square, at(5, -1), at(5, 11), out, &error) && out.size() == 2, "a line through a square cuts it in two: " + error);
  check(std::abs(areaOf(out) - 100.0) < 1e-9, "and the pieces still sum to the whole");

  // A cut that misses leaves the polygon whole rather than failing.
  check(cutPolygon(square, at(20, 0), at(20, 10), out, &error) && out.size() == 1, "a line that misses leaves one piece");
  check(std::abs(areaOf(out) - 100.0) < 1e-9, "of the original area");

  // A cut along an existing edge does not split anything.
  check(cutPolygon(square, at(0, 0), at(0, 10), out, &error) && out.size() == 1, "a cut along an edge leaves one piece");

  // The case a two-output signature cannot express: a chord across a concave
  // polygon producing three pieces. A comb cut across its teeth gives one
  // piece per tooth plus the base, and with the base removed, three.
  const Polygon comb = polygonOf({at(0, 0), at(5, 0), at(5, 3), at(4, 3), at(4, 1), at(3, 1), at(3, 3), at(2, 3), at(2, 1), at(1, 1), at(1, 3), at(0, 3)});

  check(cutPolygon(comb, at(-1, 2), at(6, 2), out, &error) && out.size() == 4, "a line across a comb makes four pieces: " + error);
  check(std::abs(areaOf(out) - 11.0) < 1e-9, "and the pieces still sum to the comb");

  // Cutting a polygon with a hole: the hole must follow the piece it lies in.
  Polygon holed;
  holed.outer = box(0, 0, 10, 10);
  Ring hole = box(1, 4, 3, 6);
  reverse(hole);
  holed.holes.push_back(hole);

  check(cutPolygon(holed, at(5, -1), at(5, 11), out, &error) && out.size() == 2, "cutting a holed square gives two pieces: " + error);

  size_t withHole = 0;

  for (const auto &piece : out)
  {
    if (!piece.holes.empty())
    {
      ++withHole;
    }
  }

  check(withHole == 1, "and the hole stays with exactly one of them");
  check(std::abs(areaOf(out) - 96.0) < 1e-9, "with the total area still the holed square's");

  check(!cutPolygon(square, at(3, 3), at(3, 3), out, &error), "a degenerate cut is refused");
}

void invariantTests()
{
  std::cout << "Offset invariants\n";

  const std::vector<Polygon> corpus = {polygonOf(box(0, 0, 10, 10)), polygonOf(youShape()), polygonOf({at(0, 0), at(6, 0), at(6, 2), at(2, 2), at(2, 6), at(0, 6)})};

  bool monotone = true;
  bool roundTripContained = true;

  for (const auto &polygon : corpus)
  {
    double previous = -1.0;

    for (const double d : {0.0, 0.1, 0.25, 0.5, 1.0})
    {
      std::vector<Polygon> grown;

      if (!grow(polygon, d, grown))
      {
        monotone = false;

        continue;
      }

      const double area = areaOf(grown);

      if (area + 1e-6 < previous)
      {
        monotone = false;
      }

      previous = area;
    }

    // Shrinking then growing by the same distance can never exceed the
    // original: the erosion loses every feature narrower than the offset and
    // the dilation cannot invent it back.
    std::vector<Polygon> shrunk;
    std::vector<Polygon> restored;
    std::string error;

    if (shrink(polygon, 0.3, shrunk) && offsetPolygons(shrunk, 0.3, {}, restored, &error))
    {
      if (areaOf(restored) > areaMetres(polygon) + 0.05)
      {
        roundTripContained = false;
      }
    }
    else
    {
      roundTripContained = false;
    }
  }

  check(monotone, "grown area is non-decreasing in the offset distance");
  check(roundTripContained, "shrinking then growing never exceeds the original");
}

} // namespace

int main()
{
  std::cout << "geom2 offset and cut tests\n";

  convexOffsetTests();
  concaveOffsetTests();
  holeOffsetTests();
  ribbonTests();
  cutTests();
  invariantTests();

  if (failures != 0)
  {
    std::cerr << failures << " geom2 offset test(s) failed\n";

    return 1;
  }

  std::cout << "All geom2 offset and cut tests passed\n";

  return 0;
}
