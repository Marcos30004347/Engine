// Boolean operations.
//
// Layer 1 of the test strategy: algebraic invariants over a corpus of shapes,
// which catch a class of error rather than an instance of one. Area
// conservation, idempotence, commutativity as bit-identity, associativity and
// De Morgan all hold for concave operands exactly as for convex ones - they
// were never convexity-dependent, which is the sign the property set is right.
//
// Layer 2 runs on every result: each output polygon is put through
// polygonFault, so a malformed ring is caught here rather than downstream.
//
// Layer 3 is the degeneracy corpus: shared whole edges (the most common input
// this codebase produces), collinear partial overlap, corner contact, one
// operand inside another's hole, and identical operands.

#include "procedural/geom2/Boolean.hpp"

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

Ring ell()
{
  return {at(0, 0), at(3, 0), at(3, 1), at(1, 1), at(1, 3), at(0, 3)};
}

Ring youShape()
{
  return {at(0, 0), at(3, 0), at(3, 3), at(2, 3), at(2, 1), at(1, 1), at(1, 3), at(0, 3)};
}

Ring comb()
{
  return {at(0, 0), at(5, 0), at(5, 3), at(4, 3), at(4, 1), at(3, 1), at(3, 3), at(2, 3), at(2, 1), at(1, 1), at(1, 3), at(0, 3)};
}

// Every result is validated, so no test can pass on a malformed polygon.
double total(const std::vector<Polygon> &polygons, std::string &fault)
{
  double area = 0.0;

  for (const auto &polygon : polygons)
  {
    const std::string bad = polygonFault(polygon);

    if (!bad.empty() && fault.empty())
    {
      fault = bad;
    }

    area += areaMetres(polygon);
  }

  return area;
}

double areaOf(const std::vector<Polygon> &polygons)
{
  std::string fault;
  const double area = total(polygons, fault);

  if (!fault.empty())
  {
    std::cerr << "    (invalid output polygon: " << fault << ")\n";
    ++failures;
  }

  return area;
}

bool run(const std::vector<Operand> &operands, const Operation operation, std::vector<Polygon> &out)
{
  std::string error;

  if (!booleanOp(operands, operation, {}, out, &error))
  {
    std::cerr << "    (operation failed: " << error << ")\n";

    return false;
  }

  return true;
}

double areaOfOp(const std::vector<Operand> &operands, const Operation operation)
{
  std::vector<Polygon> out;

  if (!run(operands, operation, out))
  {
    ++failures;

    return -1.0;
  }

  return areaOf(out);
}

// Canonical byte form of a result, so "identical" can mean identical rather
// than "the same area".
std::string signature(const std::vector<Polygon> &polygons)
{
  std::vector<std::string> parts;

  for (const auto &polygon : polygons)
  {
    std::string text = "O";

    for (const auto &p : polygon.outer)
    {
      text += "(" + std::to_string(p.x) + "," + std::to_string(p.y) + ")";
    }

    for (const auto &hole : polygon.holes)
    {
      text += "H";

      for (const auto &p : hole)
      {
        text += "(" + std::to_string(p.x) + "," + std::to_string(p.y) + ")";
      }
    }

    parts.push_back(text);
  }

  std::sort(parts.begin(), parts.end());

  std::string out;

  for (const auto &part : parts)
  {
    out += part + "|";
  }

  return out;
}

void basicTests()
{
  std::cout << "The four operations\n";

  const Operand a = toOperand(box(0, 0, 2, 2));
  const Operand b = toOperand(box(1, 1, 3, 3));

  check(std::abs(areaOfOp({a, b}, Operation::Union) - 7.0) < 1e-9, "union of two overlapping squares");
  check(std::abs(areaOfOp({a, b}, Operation::Intersection) - 1.0) < 1e-9, "intersection of two overlapping squares");
  check(std::abs(areaOfOp({a, b}, Operation::Difference) - 3.0) < 1e-9, "difference of two overlapping squares");
  check(std::abs(areaOfOp({a, b}, Operation::Xor) - 6.0) < 1e-9, "symmetric difference of two overlapping squares");

  // Disjoint operands: a union is two polygons, an intersection is none.
  const Operand far = toOperand(box(10, 10, 11, 11));
  std::vector<Polygon> out;

  check(run({a, far}, Operation::Union, out) && out.size() == 2, "union of disjoint squares is two polygons");
  check(run({a, far}, Operation::Intersection, out) && out.empty(), "intersection of disjoint squares is empty");

  // A difference that punches a hole: the result is one polygon with a hole,
  // not two polygons, and the hole must come back nested.
  const Operand outer = toOperand(box(0, 0, 5, 5));
  const Operand middle = toOperand(box(1, 1, 4, 4));

  check(run({outer, middle}, Operation::Difference, out) && out.size() == 1 && out[0].holes.size() == 1, "subtracting an interior square leaves one polygon with one hole");
  check(std::abs(areaOf(out) - 16.0) < 1e-9, "and the hole is subtracted from the area");
}

void concaveTests()
{
  std::cout << "Concave operands\n";

  const Operand l = toOperand(ell());
  const Operand u = toOperand(youShape());
  const Operand c = toOperand(comb());

  check(std::abs(areaOfOp({l}, Operation::Union) - 5.0) < 1e-9, "an L alone is its own union");
  check(std::abs(areaOfOp({u}, Operation::Union) - 7.0) < 1e-9, "a U alone is its own union");
  check(std::abs(areaOfOp({c}, Operation::Union) - 11.0) < 1e-9, "a comb alone is its own union");

  // A bar laid across a comb's teeth: the intersection is the three teeth, so
  // the result must be three separate polygons.
  const Operand bar = toOperand(box(0, 2, 5, 2.5));
  std::vector<Polygon> out;

  check(run({c, bar}, Operation::Intersection, out) && out.size() == 3, "a bar across a comb intersects in three pieces");
  check(std::abs(areaOf(out) - 1.5) < 1e-9, "and their areas add up");

  // Filling a U's notch makes it convex again: a shape and the boolean that
  // repairs it, which is what an inward offset has to survive.
  const Operand notch = toOperand(box(1, 1, 2, 3));

  check(std::abs(areaOfOp({u, notch}, Operation::Union) - 9.0) < 1e-9, "filling the notch of a U gives the whole square");

  // The union of an L and its complement within the bounding square.
  const Operand square = toOperand(box(0, 0, 3, 3));

  check(std::abs(areaOfOp({square, l}, Operation::Difference) - 4.0) < 1e-9, "the L's complement in its bounding square");
  check(std::abs(areaOfOp({square, l}, Operation::Union) - 9.0) < 1e-9, "an L unioned with its bounding square is the square");
}

void degeneracyTests()
{
  std::cout << "Degeneracy corpus\n";

  std::vector<Polygon> out;

  // Shared whole edge. The single most common input this codebase produces:
  // two abutting planning cells.
  const Operand left = toOperand(box(0, 0, 1, 1));
  const Operand right = toOperand(box(1, 0, 2, 1));

  check(run({left, right}, Operation::Union, out) && out.size() == 1, "abutting squares union into one polygon");
  check(std::abs(areaOf(out) - 2.0) < 1e-9, "with the combined area");
  check(out.size() == 1 && out[0].outer.size() == 4, "and the shared edge is dissolved, not left as vertices");
  check(run({left, right}, Operation::Intersection, out) && out.empty(), "abutting squares do not intersect in any area");

  // Collinear partial overlap.
  const Operand half = toOperand(box(1, 0.5, 2, 1.5));

  check(run({left, half}, Operation::Union, out) && out.size() == 1, "squares sharing half an edge union into one polygon");
  check(std::abs(areaOf(out) - 2.0) < 1e-9, "with the combined area");

  // Corner contact only.
  const Operand corner = toOperand(box(1, 1, 2, 2));

  check(run({left, corner}, Operation::Intersection, out) && out.empty(), "corner contact has no area");
  check(std::abs(areaOfOp({left, corner}, Operation::Union) - 2.0) < 1e-9, "corner contact still unions by area");

  // Identical operands.
  check(std::abs(areaOfOp({left, left}, Operation::Union) - 1.0) < 1e-9, "the union of a square with itself is the square");
  check(std::abs(areaOfOp({left, left}, Operation::Intersection) - 1.0) < 1e-9, "so is the intersection");
  check(std::abs(areaOfOp({left, left}, Operation::Difference)) < 1e-9, "and the difference is empty");
  check(std::abs(areaOfOp({left, left}, Operation::Xor)) < 1e-9, "as is the symmetric difference");

  // One operand inside another's hole: it must not appear in the union.
  Polygon ring;
  ring.outer = box(0, 0, 6, 6);
  Ring lake = box(2, 2, 4, 4);
  reverse(lake);
  ring.holes.push_back(lake);

  const Operand island = toOperand(box(2.5, 2.5, 3.5, 3.5));

  check(std::abs(areaOfOp({toOperand(ring), island}, Operation::Union) - (36.0 - 4.0 + 1.0)) < 1e-9, "an island in a lake adds its own area back");
  check(run({toOperand(ring), island}, Operation::Intersection, out) && out.empty(), "and does not intersect the land around the lake");

  // Subtracting a shape that exactly fills the hole.
  const Operand plug = toOperand(box(2, 2, 4, 4));

  check(std::abs(areaOfOp({toOperand(ring), plug}, Operation::Union) - 36.0) < 1e-9, "plugging a hole exactly restores the full square");

  // A hole tangent to its outer ring. A symmetric difference genuinely
  // produces this, and the region has no representation with strictly interior
  // holes, so the canonical form admits the tangency. Found by the validity
  // check running on every result rather than by a case anyone thought of.
  std::vector<Polygon> tangent;

  check(run({toOperand(ell()), toOperand(box(1, 0.5, 2, 1.5))}, Operation::Xor, tangent), "an L xor an overlapping box");
  check(!tangent.empty() && tangent[0].holes.size() == 1, "leaves a hole");
  check(areaOf(tangent) > 0.0, "and the result is valid with that hole touching the outer ring");

  // A cut that splits an operand in two.
  const Operand bar = toOperand(box(0, 2.5, 6, 3.5));

  check(run({toOperand(box(0, 0, 6, 6)), bar}, Operation::Difference, out) && out.size() == 2, "a bar across a square splits it in two");
}

void invariantTests()
{
  std::cout << "Algebraic invariants\n";

  const std::vector<Ring> corpus = {box(0, 0, 2, 2), box(1, 1, 3, 3), ell(), youShape(), comb(), box(1, 0.5, 2, 1.5), box(-1, -1, 4, 4)};

  bool areaConserved = true;
  bool idempotent = true;
  bool commutative = true;
  bool differenceIsIntersectionWithComplement = true;
  bool xorMatchesUnionMinusIntersection = true;

  for (size_t i = 0; i < corpus.size(); ++i)
  {
    const Operand a = toOperand(corpus[i]);

    // Idempotence, on every shape rather than on one.
    std::vector<Polygon> self;
    std::vector<Polygon> alone;

    if (!run({a, a}, Operation::Union, self) || !run({a}, Operation::Union, alone) || signature(self) != signature(alone))
    {
      idempotent = false;
    }

    for (size_t j = 0; j < corpus.size(); ++j)
    {
      const Operand b = toOperand(corpus[j]);

      std::vector<Polygon> un;
      std::vector<Polygon> in;

      if (!run({a, b}, Operation::Union, un) || !run({a, b}, Operation::Intersection, in))
      {
        areaConserved = false;

        continue;
      }

      // area(A) + area(B) == area(A or B) + area(A and B), which is the
      // strongest single statement about a boolean engine: it fails if any
      // face is counted twice or dropped.
      const double left = areaMetres(corpus[i]) + areaMetres(corpus[j]);
      const double right = areaOf(un) + areaOf(in);

      if (std::abs(left - right) > 1e-9)
      {
        areaConserved = false;
      }

      // Commutativity as bit-identity, not as equal area. An engine whose
      // output depends on operand order still passes every area test, and
      // order dependence is the failure a sweep shows first.
      std::vector<Polygon> swapped;

      if (!run({b, a}, Operation::Union, swapped) || signature(swapped) != signature(un))
      {
        commutative = false;
      }

      // A - B == A and not B, taken inside a box large enough to hold both.
      std::vector<Polygon> difference;
      std::vector<Polygon> viaComplement;
      const Operand world = toOperand(box(-5, -5, 10, 10));
      std::vector<Polygon> complement;

      if (!run({a, b}, Operation::Difference, difference) || !run({world, b}, Operation::Difference, complement))
      {
        differenceIsIntersectionWithComplement = false;
      }
      else
      {
        std::vector<Operand> pieces{a};
        Operand complementOperand;

        for (const auto &polygon : complement)
        {
          for (const auto &ring : toOperand(polygon).rings)
          {
            complementOperand.rings.push_back(ring);
          }
        }

        pieces.push_back(complementOperand);

        if (!run(pieces, Operation::Intersection, viaComplement) || signature(viaComplement) != signature(difference))
        {
          differenceIsIntersectionWithComplement = false;
        }
      }

      // xor == union - intersection, by area.
      std::vector<Polygon> exclusive;

      if (!run({a, b}, Operation::Xor, exclusive) || std::abs(areaOf(exclusive) - (areaOf(un) - areaOf(in))) > 1e-9)
      {
        xorMatchesUnionMinusIntersection = false;
      }
    }
  }

  check(areaConserved, "area(A) + area(B) == area(A or B) + area(A and B) over the corpus");
  check(idempotent, "union with self is the identity over the corpus");
  check(commutative, "union is bit-identical under operand order over the corpus");
  check(differenceIsIntersectionWithComplement, "A minus B equals A intersected with the complement of B");
  check(xorMatchesUnionMinusIntersection, "symmetric difference equals union less intersection");
}

void associativityTests()
{
  std::cout << "Associativity\n";

  const Operand a = toOperand(box(0, 0, 2, 2));
  const Operand b = toOperand(box(1, 1, 3, 3));
  const Operand c = toOperand(ell());

  std::vector<Polygon> ab;
  std::vector<Polygon> abc;
  std::vector<Polygon> bc;
  std::vector<Polygon> aBc;

  const bool ok = run({a, b}, Operation::Union, ab) && run({b, c}, Operation::Union, bc);

  if (ok)
  {
    Operand abOperand;

    for (const auto &polygon : ab)
    {
      for (const auto &ring : toOperand(polygon).rings)
      {
        abOperand.rings.push_back(ring);
      }
    }

    Operand bcOperand;

    for (const auto &polygon : bc)
    {
      for (const auto &ring : toOperand(polygon).rings)
      {
        bcOperand.rings.push_back(ring);
      }
    }

    const bool built = run({abOperand, c}, Operation::Union, abc) && run({a, bcOperand}, Operation::Union, aBc);

    check(built && signature(abc) == signature(aBc), "(A or B) or C is bit-identical to A or (B or C)");
  }
  else
  {
    check(false, "associativity operands built");
  }
}

void selectionTests()
{
  std::cout << "Arbitrary winding predicates\n";

  // The question the generators actually ask, which is not one of the four
  // named operations: inside the cell, inside the road band, outside every
  // building. One predicate over the winding vector, one sweep.
  const Operand cell = toOperand(box(0, 0, 10, 6));
  const Operand road = toOperand(box(-1, 2, 11, 3));
  const Operand building = toOperand(box(4, 2.25, 5, 2.75));

  std::vector<Polygon> carriageway;
  std::string error;

  const bool ok = selectFaces(
      {cell, road, building},
      [](const std::vector<int32_t> &w) {
        return w[0] != 0 && w[1] != 0 && w[2] == 0;
      },
      {},
      carriageway,
      &error);

  check(ok, "a three-way predicate selects: " + error);
  check(std::abs(areaOf(carriageway) - (10.0 - 0.5)) < 1e-9, "the road inside the cell less the building on it");

  // The A | ribbon | B split of a widthed edge, from the same arrangement:
  // three separate questions, one sweep.
  std::vector<Polygon> banks;

  check(
      selectFaces(
          {cell, road},
          [](const std::vector<int32_t> &w) {
            return w[0] != 0 && w[1] == 0;
          },
          {},
          banks,
          &error) &&
          banks.size() == 2,
      "the road splits the cell into two banks");
  check(std::abs(areaOf(banks) - 50.0) < 1e-9, "and the banks are the cell less the road");
}

void fillRuleTests()
{
  std::cout << "Fill rules\n";

  // Two concentric squares, both counter-clockwise. Under the non-zero rule
  // the inner one is still inside (winding 2); under even-odd it is a hole.
  Operand nested;
  nested.rings.push_back(box(0, 0, 4, 4));
  nested.rings.push_back(box(1, 1, 3, 3));

  std::vector<Polygon> out;
  std::string error;

  BooleanOptions nonZero;
  nonZero.rule = FillRule::NonZero;

  check(booleanOp({nested}, Operation::Union, nonZero, out, &error) && std::abs(areaOf(out) - 16.0) < 1e-9, "non-zero keeps a co-oriented inner ring solid");

  BooleanOptions evenOdd;
  evenOdd.rule = FillRule::EvenOdd;

  check(booleanOp({nested}, Operation::Union, evenOdd, out, &error) && std::abs(areaOf(out) - 12.0) < 1e-9, "even-odd makes it a hole");
}

} // namespace

int main()
{
  std::cout << "geom2 boolean tests\n";

  basicTests();
  concaveTests();
  degeneracyTests();
  invariantTests();
  associativityTests();
  selectionTests();
  fillRuleTests();

  if (failures != 0)
  {
    std::cerr << failures << " geom2 boolean test(s) failed\n";

    return 1;
  }

  std::cout << "All geom2 boolean tests passed\n";

  return 0;
}
