// The planar arrangement: the sweep, the half-edge graph, face extraction and
// winding propagation.
//
// Every arrangement built here is run through arrangementFault, so a structural
// break is caught where it happens rather than three operations downstream.
// The interesting cases are all degenerate on purpose: shared edges, T
// junctions, collinear overlap, a vertex landing exactly on another edge, and
// concave input throughout.

#include "procedural/geom2/Arrangement.hpp"

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

Ring ell()
{
  return {at(0, 0), at(3, 0), at(3, 1), at(1, 1), at(1, 3), at(0, 3)};
}

bool build(const std::vector<InputRing> &rings, Arrangement &out, std::string &error)
{
  if (!buildArrangement(rings, {}, out, &error))
  {
    return false;
  }

  const std::string fault = arrangementFault(out);

  if (!fault.empty())
  {
    error = "structural fault: " + fault;

    return false;
  }

  return true;
}

// Number of bounded faces whose winding for `input` is non-zero, and their
// total area. The basic question every boolean is built on.
double insideArea(const Arrangement &a, const uint32_t input)
{
  double total = 0.0;

  for (uint32_t f = 0; f < a.faces.size(); ++f)
  {
    if (!a.faces[f].unbounded && a.faces[f].winding[input] != 0)
    {
      total += faceAreaMetres(a, f);
    }
  }

  return total;
}

void singleRingTests()
{
  std::cout << "One ring\n";

  Arrangement a;
  std::string error;

  check(build({{box(0, 0, 2, 2), 0}}, a, error), "a square arranges: " + error);
  check(a.faces.size() == 2, "a square makes two faces, inside and outside");
  check(std::abs(insideArea(a, 0) - 4.0) < 1e-9, "the inside face has the square's area");
  check(a.faces[a.unboundedFace].winding[0] == 0, "the unbounded face is outside the square");
  check(a.snapRounds == 1, "a square needs no snap iteration");

  check(build({{ell(), 0}}, a, error), "a concave L arranges: " + error);
  check(a.faces.size() == 2, "a concave ring still makes two faces");
  check(std::abs(insideArea(a, 0) - 5.0) < 1e-9, "the L's area survives the arrangement");

  // A clockwise ring is inside-out: winding is -1 rather than +1, and the
  // non-zero rule treats it as inside either way.
  Ring clockwise = box(0, 0, 2, 2);
  reverse(clockwise);

  check(build({{clockwise, 0}}, a, error), "a clockwise square arranges: " + error);

  bool negative = false;

  for (uint32_t f = 0; f < a.faces.size(); ++f)
  {
    if (!a.faces[f].unbounded && a.faces[f].winding[0] == -1)
    {
      negative = true;
    }
  }

  check(negative, "a clockwise ring winds to minus one");
}

void crossingTests()
{
  std::cout << "Crossings\n";

  Arrangement a;
  std::string error;

  // Two overlapping squares: nine faces would be a grid, but two offset
  // squares make four bounded regions.
  check(build({{box(0, 0, 2, 2), 0}, {box(1, 1, 3, 3), 1}}, a, error), "two overlapping squares arrange: " + error);

  double both = 0.0;
  double onlyFirst = 0.0;

  for (uint32_t f = 0; f < a.faces.size(); ++f)
  {
    if (a.faces[f].unbounded)
    {
      continue;
    }

    const double area = cycleAreaMetres(a, a.faces[f].cycles.front());

    if (a.faces[f].winding[0] != 0 && a.faces[f].winding[1] != 0)
    {
      both += area;
    }
    else if (a.faces[f].winding[0] != 0)
    {
      onlyFirst += area;
    }
  }

  check(std::abs(both - 1.0) < 1e-9, "the intersection of the two squares is one square metre");
  check(std::abs(onlyFirst - 3.0) < 1e-9, "the part of the first square outside the second is three");

  // A bowtie: one ring crossing itself. The arrangement must resolve it into
  // two faces rather than reject it.
  const Ring bowtie = {at(0, 0), at(2, 2), at(2, 0), at(0, 2)};

  check(build({{bowtie, 0}}, a, error), "a self-crossing ring arranges: " + error);
  check(a.faces.size() == 3, "a bowtie makes two bounded faces");

  // A proper crossing at a point that is not on the grid: the diagonals of a
  // unit square meet at its centre, which here is a whole number of
  // micrometres, so pick a triangle whose crossing is genuinely fractional.
  const Ring thin = {at(0, 0), at(3, 1), at(0, 2)};
  const Ring flat = {at(0, 1), at(3, 0), at(3, 2)};

  check(build({{thin, 0}, {flat, 1}}, a, error), "segments crossing off-grid arrange: " + error);
}

void touchingTests()
{
  std::cout << "Touching, sharing and overlap\n";

  Arrangement a;
  std::string error;

  // Two squares sharing a whole edge: the most common input this codebase
  // produces, and the case a boolean engine is most often wrong about.
  check(build({{box(0, 0, 1, 1), 0}, {box(1, 0, 2, 1), 1}}, a, error), "abutting squares arrange: " + error);
  check(a.faces.size() == 3, "abutting squares make two bounded faces");
  check(std::abs(insideArea(a, 0) - 1.0) < 1e-9, "the left square keeps its area");
  check(std::abs(insideArea(a, 1) - 1.0) < 1e-9, "the right square keeps its area");

  // A T junction: one square's corner lands in the middle of another's edge.
  check(build({{box(0, 0, 2, 2), 0}, {box(2, 0.5, 3, 1.5), 1}}, a, error), "a T junction arranges: " + error);

  // Identical rings: the crossings cancel to a winding of two under the
  // non-zero rule, which is what makes a self-union idempotent.
  check(build({{box(0, 0, 2, 2), 0}, {box(0, 0, 2, 2), 0}}, a, error), "duplicate rings arrange: " + error);
  check(std::abs(insideArea(a, 0) - 4.0) < 1e-9, "a duplicated ring covers its own area once");

  // The same ring traced both ways cancels: winding zero everywhere.
  Ring backwards = box(0, 0, 2, 2);
  reverse(backwards);

  check(build({{box(0, 0, 2, 2), 0}, {backwards, 0}}, a, error), "opposed duplicate rings arrange: " + error);
  check(std::abs(insideArea(a, 0)) < 1e-9, "a ring and its reverse cancel to nothing");

  // Collinear partial overlap: two squares sharing half of an edge.
  check(build({{box(0, 0, 2, 2), 0}, {box(2, 1, 4, 3), 1}}, a, error), "squares sharing half an edge arrange: " + error);

  // Touching at a single point only.
  check(build({{box(0, 0, 1, 1), 0}, {box(1, 1, 2, 2), 1}}, a, error), "squares touching at a corner arrange: " + error);
}

void holeTests()
{
  std::cout << "Holes and nesting\n";

  Arrangement a;
  std::string error;

  Ring hole = box(1, 1, 2, 2);
  reverse(hole);

  check(build({{box(0, 0, 3, 3), 0}, {hole, 0}}, a, error), "a square with a hole arranges: " + error);
  check(std::abs(insideArea(a, 0) - 8.0) < 1e-9, "the hole is not inside the polygon");

  // A ring inside a hole: the island in the lake. Winding returns to one.
  check(
      build(
          {{box(0, 0, 4, 4), 0},
           {[] {
              Ring r = box(1, 1, 3, 3);
              reverse(r);
              return r;
            }(),
            0},
           {box(1.5, 1.5, 2.5, 2.5), 0}},
          a,
          error),
      "an island in a lake arranges: " + error);
  check(std::abs(insideArea(a, 0) - (16.0 - 4.0 + 1.0)) < 1e-9, "the island counts back in");
}

void concaveTests()
{
  std::cout << "Concave input is ordinary input\n";

  Arrangement a;
  std::string error;

  // A river corridor: a meandering band crossing a cell, which is the case
  // that motivates the whole non-convex baseline. Both banks are concave.
  const Ring cell = box(0, 0, 10, 6);
  const Ring corridor = {at(-1, 2), at(3, 2), at(5, 3.5), at(7, 2), at(11, 2), at(11, 3), at(7, 3), at(5, 4.5), at(3, 3), at(-1, 3)};

  check(build({{cell, 0}, {corridor, 1}}, a, error), "a meandering corridor across a cell arranges: " + error);

  double dry = 0.0;
  double water = 0.0;
  size_t banks = 0;

  for (uint32_t f = 0; f < a.faces.size(); ++f)
  {
    if (a.faces[f].unbounded || a.faces[f].winding[0] == 0)
    {
      continue;
    }

    const double area = faceAreaMetres(a, f);

    if (a.faces[f].winding[1] != 0)
    {
      water += area;
    }
    else
    {
      dry += area;
      ++banks;
    }
  }

  check(water > 0.0, "the corridor itself is a face with its own area");
  check(banks == 2, "the corridor cuts the cell into exactly two banks");
  check(std::abs(dry + water - 60.0) < 1e-9, "bank and water areas sum to the cell");

  // A river with two branches: two rings carrying the same input index, which
  // is how a river system, a road network or any multi-part feature arrives.
  // Three dry pieces, and the winding must treat both branches as one input.
  const Ring branchA = {at(-1, 1), at(4, 1), at(6, 2), at(11, 1), at(11, 1.5), at(6, 2.5), at(4, 1.5), at(-1, 1.5)};
  const Ring branchB = {at(-1, 4), at(4, 4.5), at(6, 3.5), at(11, 4.5), at(11, 5), at(6, 4), at(4, 5), at(-1, 4.5)};

  check(build({{cell, 0}, {branchA, 1}, {branchB, 1}}, a, error), "a two-branch river arranges: " + error);

  size_t dryFaces = 0;
  double dryArea = 0.0;
  double wetArea = 0.0;

  for (uint32_t f = 0; f < a.faces.size(); ++f)
  {
    if (a.faces[f].unbounded || a.faces[f].winding[0] == 0)
    {
      continue;
    }

    if (a.faces[f].winding[1] == 0)
    {
      ++dryFaces;
      dryArea += faceAreaMetres(a, f);
    }
    else
    {
      wetArea += faceAreaMetres(a, f);
    }
  }

  check(dryFaces == 3, "two branches cut the cell into three dry pieces");
  check(std::abs(dryArea + wetArea - 60.0) < 1e-9, "the pieces still sum to the cell");
  check(wetArea > 0.0, "both branches count as the same input");
}

void determinismTests()
{
  std::cout << "Order independence\n";

  // The arrangement must not depend on the order rings are supplied in, nor on
  // which vertex each ring starts at. A sweep that gets this wrong still
  // passes every area test, which is why it is checked directly.
  const Ring a0 = box(0, 0, 2, 2);
  const Ring a1 = box(1, 1, 3, 3);
  const Ring a2 = ell();

  Arrangement reference;
  std::string error;

  check(build({{a0, 0}, {a1, 1}, {a2, 2}}, reference, error), "the reference arrangement builds: " + error);

  Arrangement permuted;
  check(build({{a2, 2}, {a1, 1}, {a0, 0}}, permuted, error), "the permuted arrangement builds: " + error);

  check(permuted.vertices == reference.vertices, "vertex set does not depend on ring order");
  check(permuted.faces.size() == reference.faces.size(), "face count does not depend on ring order");

  double referenceArea = 0.0;
  double permutedArea = 0.0;

  for (uint32_t i = 0; i < 3; ++i)
  {
    referenceArea += insideArea(reference, i);
    permutedArea += insideArea(permuted, i);
  }

  check(std::abs(referenceArea - permutedArea) < 1e-12, "measured areas do not depend on ring order");

  Ring rotated;

  for (size_t i = 0; i < a2.size(); ++i)
  {
    rotated.push_back(a2[(i + 3) % a2.size()]);
  }

  Arrangement started;
  check(build({{a0, 0}, {a1, 1}, {rotated, 2}}, started, error), "the rotated arrangement builds: " + error);
  check(started.vertices == reference.vertices, "vertex set does not depend on the starting vertex");
}

void limitTests()
{
  std::cout << "Budgets and rejection\n";

  Arrangement a;
  std::string error;

  ArrangementLimits tight;
  tight.maximumSegments = 3;

  check(!buildArrangement({{box(0, 0, 2, 2), 0}, {box(1, 1, 3, 3), 1}}, tight, a, &error), "an exceeded segment budget fails");
  check(!error.empty(), "and says why");

  check(!buildArrangement({}, {}, a, &error), "an empty input fails");
  check(!buildArrangement({{{at(0, 0), at(1, 1)}, 0}}, {}, a, &error), "a two-vertex ring fails");
}

} // namespace

int main()
{
  std::cout << "geom2 arrangement tests\n";

  singleRingTests();
  crossingTests();
  touchingTests();
  holeTests();
  concaveTests();
  determinismTests();
  limitTests();

  if (failures != 0)
  {
    std::cerr << failures << " geom2 arrangement test(s) failed\n";

    return 1;
  }

  std::cout << "All geom2 arrangement tests passed\n";

  return 0;
}
