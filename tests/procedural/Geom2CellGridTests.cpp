// The cell layer: directions, boards, contours, and the measures the
// subdivision scores with.
//
// Two of these are the load-bearing ones and the rest are guards around them.
// `outline_area_roundtrip` says the two halves of the representation agree -
// an area turned into a boundary and back is the same area - which every later
// phase assumes without checking, because cutting a contour and re-deriving the
// cells is how a split room is computed. And `narrow_exhaustive` pins the one
// habitability primitive against a direct reading of its definition over every
// neighbourhood there is, because it is computed here by bit arithmetic over a
// whole board at once and that is not obviously the same function.

#include "procedural/geom2/CellGrid.hpp"
#include "procedural/geom2/CellShape.hpp"
#include "procedural/geom2/Polygon.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

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

// A connected polyomino grown by accretion, which is the shape family the
// footprint generator produces and the one every contour routine has to take.
Board randomArea(const CellGrid &grid, std::mt19937_64 &rng, const uint32_t target)
{
  Board area;
  const uint32_t i0 = uint32_t(rng() % grid.height());
  const uint32_t j0 = uint32_t(rng() % grid.width());
  area.set(i0, j0);

  for (uint32_t added = 1; added < target; ++added)
  {
    const std::vector<std::pair<uint32_t, uint32_t>> present = cells(area);
    bool grew = false;

    for (uint32_t attempt = 0; attempt < 32 && !grew; ++attempt)
    {
      const auto &[i, j] = present[rng() % present.size()];
      const Dir d = kCardinal[rng() % 4];
      const uint32_t n = grid.cell(int32_t(i) + deltaI(d), int32_t(j) + deltaJ(d));

      if (n != kNoCell && !grid.contains(area, n))
      {
        grid.add(area, n);
        grew = true;
      }
    }

    if (!grew)
    {
      break;
    }
  }

  return area;
}

// The definition of narrowness, read straight off: no two by two block of the
// area contains this cell.
bool narrowReference(const CellGrid &grid, const Board &area, const uint32_t i, const uint32_t j)
{
  for (int32_t di = -1; di <= 0; ++di)
  {
    for (int32_t dj = -1; dj <= 0; ++dj)
    {
      bool full = true;

      for (int32_t bi = 0; bi < 2 && full; ++bi)
      {
        for (int32_t bj = 0; bj < 2 && full; ++bj)
        {
          const uint32_t c = grid.cell(int32_t(i) + di + bi, int32_t(j) + dj + bj);
          full = c != kNoCell && grid.contains(area, c);
        }
      }

      if (full)
      {
        return false;
      }
    }
  }

  return true;
}

bool connectedReference(const CellGrid &grid, const Board &area)
{
  const std::vector<std::pair<uint32_t, uint32_t>> present = cells(area);

  if (present.empty())
  {
    return true;
  }

  std::set<uint32_t> seen;
  std::vector<uint32_t> stack;
  stack.push_back(grid.cell(int32_t(present.front().first), int32_t(present.front().second)));
  seen.insert(stack.back());

  while (!stack.empty())
  {
    const uint32_t c = stack.back();
    stack.pop_back();

    for (const Dir d : kCardinal)
    {
      const uint32_t n = grid.neighbour(c, d);

      if (n != kNoCell && grid.contains(area, n) && seen.insert(n).second)
      {
        stack.push_back(n);
      }
    }
  }

  return seen.size() == present.size();
}

void directions()
{
  bool ok = true;

  for (const Dir d : kCardinal)
  {
    ok = ok && cw(ccw(d)) == d;
    ok = ok && ccw(cw(d)) == d;
    ok = ok && opposite(opposite(d)) == d;
    ok = ok && cw(cw(d)) == opposite(d);
    ok = ok && deltaI(d) + deltaI(opposite(d)) == 0;
    ok = ok && deltaJ(d) + deltaJ(opposite(d)) == 0;
  }

  check(ok, "dir_algebra: quarter turns compose and opposites cancel");
}

void edgeIdentity()
{
  const CellGrid grid(7, 5);
  bool roundTrip = true;
  bool twins = true;
  bool neighbours = true;

  for (uint32_t i = 0; i < grid.height(); ++i)
  {
    for (uint32_t j = 0; j < grid.width(); ++j)
    {
      const uint32_t c = grid.cell(int32_t(i), int32_t(j));

      for (const Dir d : kCardinal)
      {
        const uint32_t e = grid.cellEdge(c, d);
        roundTrip = roundTrip && e != kNoEdge && grid.edgeCell(e) == c;
        twins = twins && grid.twin(grid.twin(e)) == e;
        neighbours = neighbours && grid.edgeCell(grid.twin(e)) == grid.neighbour(c, d);
      }
    }
  }

  check(roundTrip, "edge_cell_roundtrip: edgeCell(cellEdge(c, d)) == c for every cell and side");
  check(twins, "edge_twin: the twin of a twin is the edge");
  check(neighbours, "edge_twin_cell: the twin bounds the neighbour on that side");
}

void contours()
{
  const CellGrid grid(12, 12);
  std::mt19937_64 rng(20260918);

  bool roundTrip = true;
  bool closed = true;
  bool boundaryOnly = true;
  size_t sampled = 0;

  for (uint32_t trial = 0; trial < 2000; ++trial)
  {
    const Board area = randomArea(grid, rng, 1 + uint32_t(rng() % 60));
    const std::vector<Chain> loops = grid.outline(area);

    if (loops.empty())
    {
      continue;
    }

    ++sampled;

    // All the loops, not just the outer one: a court is a wall too, and a fill
    // that only knows the outer ring runs straight into it.
    roundTrip = roundTrip && grid.contourArea(loops) == area;

    for (const Chain &loop : loops)
    {
      for (size_t k = 0; k < loop.size(); ++k)
      {
        // Every edge leaves where the previous one arrived.
        const uint32_t previous = loop[(k + loop.size() - 1) % loop.size()];
        closed = closed && grid.edgeEndNode(previous) == grid.decode(loop[k]).node;

        // And every edge has the area on one side and not the other.
        const uint32_t inside = grid.edgeCell(loop[k]);
        const uint32_t outside = grid.edgeCell(grid.twin(loop[k]));
        boundaryOnly = boundaryOnly && grid.contains(area, inside) && !grid.contains(area, outside);
      }
    }
  }

  check(sampled > 1900, "outline: every sampled area produced a boundary");
  check(roundTrip, "outline_area_roundtrip: contourArea(outline(a)) == a over 2000 random areas");
  check(closed, "outline_is_closed: every loop is a closed cycle");
  check(boundaryOnly, "outline_is_boundary: every loop edge separates inside from outside");
}

void holes()
{
  const CellGrid grid(9, 9);
  Board ring;

  for (uint32_t i = 2; i <= 6; ++i)
  {
    for (uint32_t j = 2; j <= 6; ++j)
    {
      if (i == 4 && j == 4)
      {
        continue;
      }

      ring.set(i, j);
    }
  }

  const std::vector<Chain> loops = grid.outline(ring);
  check(loops.size() == 2, "outline_finds_holes: a ring of cells returns two loops, not one");

  if (loops.size() == 2)
  {
    check(loops.front().size() == 20 && loops.back().size() == 4,
          "outline_finds_holes: the outer loop is the long one and the hole is four edges");
  }

  Board one;
  one.set(3, 4);
  const std::vector<Chain> single = grid.outline(one);
  check(single.size() == 1 && single.front().size() == 4, "outline_single_cell: one cell gives one four-edge loop");

  // A polygon built from that ring keeps the court, which is the reason this
  // returns every loop instead of only the outer one.
  CellFrame frame;
  Polygon poly;
  const bool built = polygonOf(grid, frame, ring, poly, nullptr);
  check(built && poly.holes.size() == 1, "polygon_of: a court survives the conversion to metres");
  check(built && polygonFault(poly).empty(), "polygon_of: the result is a valid polygon");
}

void connectivity()
{
  const CellGrid grid(10, 10);
  std::mt19937_64 rng(77771);
  bool agrees = true;
  uint32_t split = 0;

  for (uint32_t trial = 0; trial < 2000; ++trial)
  {
    Board area = randomArea(grid, rng, 1 + uint32_t(rng() % 40));

    // Punch holes so that some of these really are disconnected.
    for (uint32_t k = 0; k < 3; ++k)
    {
      area.clear(uint32_t(rng() % grid.height()), uint32_t(rng() % grid.width()));
    }

    const bool reference = connectedReference(grid, area);
    agrees = agrees && grid.connected(area) == reference;

    if (!reference)
    {
      ++split;
    }
  }

  check(agrees, "connectivity: the board flood agrees with a reference walk over 2000 areas");
  check(split > 100, "connectivity: the sample actually contained disconnected areas");
}

void narrowness()
{
  // Every three by three neighbourhood there is, with the centre occupied.
  const CellGrid grid(3, 3);
  bool agrees = true;

  for (uint32_t bits = 0; bits < 512; ++bits)
  {
    if ((bits & (1u << 4)) == 0)
    {
      continue;
    }

    Board area;

    for (uint32_t k = 0; k < 9; ++k)
    {
      if ((bits & (1u << k)) != 0)
      {
        area.set(k / 3, k % 3);
      }
    }

    const Board computed = narrow(grid, area);
    agrees = agrees && computed.test(1, 1) == narrowReference(grid, area, 1, 1);
  }

  check(agrees, "narrow_exhaustive: bit-parallel narrowness matches its definition on all 256 neighbourhoods");

  // And on real shapes, for every cell rather than the middle one.
  const CellGrid wide(12, 12);
  std::mt19937_64 rng(4242);
  bool everywhere = true;

  for (uint32_t trial = 0; trial < 500; ++trial)
  {
    const Board area = randomArea(wide, rng, 1 + uint32_t(rng() % 50));
    const Board computed = narrow(wide, area);

    for (const auto &[i, j] : cells(area))
    {
      everywhere = everywhere && computed.test(i, j) == narrowReference(wide, area, i, j);
    }
  }

  check(everywhere, "narrow_everywhere: and over every cell of 500 random areas");

  // A one-wide strip is entirely narrow; a block is not narrow at all.
  Board strip;

  for (uint32_t j = 0; j < 6; ++j)
  {
    strip.set(3, j);
  }

  Board block;

  for (uint32_t i = 2; i < 5; ++i)
  {
    for (uint32_t j = 2; j < 5; ++j)
    {
      block.set(i, j);
    }
  }

  check(narrow(wide, strip).count() == 6, "narrow: a one-cell-wide corridor is entirely narrow");
  check(narrow(wide, block).count() == 0, "narrow: a three by three block has no narrow cell");
}

void chains()
{
  const CellGrid grid(10, 10);
  std::mt19937_64 rng(31337);
  bool sliceWhole = true;
  bool corners = true;

  for (uint32_t trial = 0; trial < 500; ++trial)
  {
    const Board area = randomArea(grid, rng, 6 + uint32_t(rng() % 40));
    const Chain loop = grid.outerLoop(area);

    if (loop.size() < 4)
    {
      continue;
    }

    const uint32_t a = grid.decode(loop[rng() % loop.size()]).node;
    const uint32_t b = grid.decode(loop[rng() % loop.size()]).node;

    if (a == b)
    {
      continue;
    }

    const Chain first = sliceChain(grid, loop, a, b);
    const Chain second = sliceChain(grid, loop, b, a);
    sliceWhole = sliceWhole && first.size() + second.size() == loop.size();

    // Every corner the merge reports really is one.
    for (const uint32_t n : mergeCollinear(grid, loop))
    {
      const std::optional<size_t> at = findEdgeFrom(grid, loop, n);
      corners = corners && at.has_value();

      if (at.has_value())
      {
        const size_t previous = (*at + loop.size() - 1) % loop.size();
        corners = corners && grid.decode(loop[*at]).dir != grid.decode(loop[previous]).dir;
      }
    }
  }

  check(sliceWhole, "chain_slice: the two cyclic slices between two nodes partition the loop");
  check(corners, "merge_collinear: every reported corner is a turn on the contour");

  Board rectangle;

  for (uint32_t i = 1; i < 5; ++i)
  {
    for (uint32_t j = 1; j < 7; ++j)
    {
      rectangle.set(i, j);
    }
  }

  const Chain loop = grid.outerLoop(rectangle);
  check(mergeCollinear(grid, loop).size() == 4, "merge_collinear: a rectangle merges to four corners");
  check(isRectangle(grid, loop), "is_rectangle: and is recognised as one");

  Board ell = rectangle;
  ell.clear(1, 6);
  ell.clear(2, 6);
  check(mergeCollinear(grid, grid.outerLoop(ell)).size() == 6, "merge_collinear: an L merges to six");
  check(!isRectangle(grid, grid.outerLoop(ell)), "is_rectangle: and an L is not a rectangle");
}

void measures()
{
  const CellGrid grid(16, 16);
  std::mt19937_64 rng(90210);

  Board rectangle;

  for (uint32_t i = 2; i < 7; ++i)
  {
    for (uint32_t j = 3; j < 11; ++j)
    {
      rectangle.set(i, j);
    }
  }

  check(compactness(rectangle) > .9999 && compactness(rectangle) < 1.0001, "compactness: a rectangle scores one");

  const CellBounds box = bounds(rectangle);
  check(box.i0 == 2 && box.i1 == 6 && box.j0 == 3 && box.j1 == 10, "bounds: the box is tight");

  // The histogram pass against brute force.
  bool matches = true;

  for (uint32_t trial = 0; trial < 300; ++trial)
  {
    const Board area = randomArea(grid, rng, 1 + uint32_t(rng() % 40));

    uint32_t best = 0;

    for (uint32_t i0 = 0; i0 < grid.height(); ++i0)
    {
      for (uint32_t j0 = 0; j0 < grid.width(); ++j0)
      {
        for (uint32_t i1 = i0; i1 < grid.height(); ++i1)
        {
          for (uint32_t j1 = j0; j1 < grid.width(); ++j1)
          {
            bool full = true;

            for (uint32_t i = i0; i <= i1 && full; ++i)
            {
              for (uint32_t j = j0; j <= j1 && full; ++j)
              {
                full = area.test(i, j);
              }
            }

            if (full)
            {
              best = std::max(best, (i1 - i0 + 1) * (j1 - j0 + 1));
            }
          }
        }
      }
    }

    CellBounds found;
    const bool any = largestRectangle(grid, area, found);
    const uint32_t size = any ? found.rows() * found.columns() : 0;
    matches = matches && size == best;
  }

  check(matches, "largest_rectangle: the histogram pass matches brute force over 300 areas");

  bool removal = true;

  for (uint32_t trial = 0; trial < 500; ++trial)
  {
    const Board area = randomArea(grid, rng, 4 + uint32_t(rng() % 30));

    for (const auto &[i, j] : cells(area))
    {
      const uint32_t c = grid.cell(int32_t(i), int32_t(j));
      Board rest = area;
      grid.remove(rest, c);
      removal = removal && removalKeepsConnected(grid, area, c) == connectedReference(grid, rest);
    }
  }

  check(removal, "removal_keeps_connected: agrees with a reference walk");
}

void metres()
{
  const CellGrid grid(12, 12);
  std::mt19937_64 rng(5150);
  CellFrame frame;
  frame.cellMetres = 2.2;

  bool valid = true;
  bool areaAgrees = true;
  bool counterClock = true;
  bool pinchRefused = true;
  uint32_t pinched = 0;
  uint32_t simple = 0;

  for (uint32_t trial = 0; trial < 500; ++trial)
  {
    const Board area = randomArea(grid, rng, 4 + uint32_t(rng() % 40));
    const std::vector<Chain> loops = grid.outline(area);

    if (loops.size() != 1)
    {
      continue;
    }

    Polygon poly;

    // An area whose boundary touches itself at a corner is four-connected and
    // is not a simple polygon. It has to be refused, with a reason, rather
    // than converted into a ring that visits a node twice.
    if (!grid.simpleOutline(loops))
    {
      ++pinched;
      pinchRefused = pinchRefused && !polygonOf(grid, frame, area, poly, nullptr);
      continue;
    }

    ++simple;

    if (!polygonOf(grid, frame, area, poly, nullptr))
    {
      valid = false;
      continue;
    }

    valid = valid && polygonFault(poly).empty();
    counterClock = counterClock && counterClockwise(poly.outer);

    const double expected = double(area.count()) * frame.cellMetres * frame.cellMetres;
    const double measured = areaMetres(poly);
    areaAgrees = areaAgrees && std::abs(measured - expected) < 1e-6;
  }

  check(simple > 300, "ring_of: the sample held enough areas with a simple outline");
  check(pinched > 0, "ring_of: and some that pinch at a corner");
  check(pinchRefused, "ring_of: a pinched area is refused rather than converted");
  check(valid, "ring_of: every area with a simple outline converts to a valid polygon");
  check(counterClock, "ring_of: the outer ring comes out counter-clockwise");
  check(areaAgrees, "ring_of: the polygon's area is the cell count times the cell area");

  // A rotated frame changes where the plan is, not how big it is.
  CellFrame turned;
  turned.cellMetres = 2.2;
  turned.axisX = std::cos(.7);
  turned.axisZ = std::sin(.7);
  turned.originX = 130;
  turned.originZ = -47;

  Board square;

  for (uint32_t i = 1; i < 4; ++i)
  {
    for (uint32_t j = 1; j < 4; ++j)
    {
      square.set(i, j);
    }
  }

  Polygon straight;
  Polygon rotated;
  polygonOf(grid, frame, square, straight, nullptr);
  polygonOf(grid, turned, square, rotated, nullptr);

  // A micrometre grid: a rotated corner rounds, so the area moves by about the
  // perimeter times half a unit. A millimetre-squared band is three orders of
  // magnitude above that and still catches any real error.
  check(std::abs(areaMetres(straight) - areaMetres(rotated)) < 1e-3,
        "cell_frame: rotating and moving the frame preserves area to within quantisation");
  check(polygonFault(rotated).empty(), "cell_frame: and the rotated polygon is still valid");
}

} // namespace

int main()
{
  std::cout << "Geom2CellGridTests\n";

  directions();
  edgeIdentity();
  contours();
  holes();
  connectivity();
  narrowness();
  chains();
  measures();
  metres();

  if (failures != 0)
  {
    std::cerr << failures << " check(s) failed\n";

    return 1;
  }

  std::cout << "all checks passed\n";

  return 0;
}
