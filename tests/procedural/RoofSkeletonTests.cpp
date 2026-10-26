// The roof, on every shape a building plan can hand it.
//
// This is the piece that kept breaking, and it had no test of its own: it was
// exercised only through a dwelling gallery that measured whole buildings, so
// a roof could lose a fifth of its area or come out combed with flat triangles
// and the only signal was a render. Every fault that actually happened is a
// case here.
//
// What each shape is for:
//
//   square, rect      the cases every implementation gets right
//   trapezoid, wedge  edges not square to the ridge, which is what a lot cut
//                     from a Voronoi block gives
//   L, T, U, E, cross reflex corners: split events, several ridges, valleys
//   L-uneven          arms of different width - one arm ridges while the other
//                     is still open, and closing both loops threw the second
//                     away
//   notched           a staircase a few decimetres across, left by trimming a
//                     mass against its neighbours
//   courtyard         a ring of building: the wavefront runs inward from the
//                     outer wall and outward from the court
//   comb              three notches, the shape that catches anything assuming
//                     one ridge
#include "procedural/geom2/Boolean.hpp"
#include "procedural/geom2/Polygon.hpp"
#include "procedural/geom2/Quantise.hpp"
#include "procedural/geom2/Skeleton.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <vector>

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

using Pt = std::pair<double, double>;

Ring ringOf(const std::vector<Pt> &points)
{
  Ring ring;

  for (const Pt &p : points)
  {
    ring.push_back({toUnit(p.first), toUnit(p.second)});
  }

  return ring;
}

double planArea(const std::vector<RoofTriangle> &roof)
{
  double total = 0;

  for (const RoofTriangle &t : roof)
  {
    total += std::abs((t.bx - t.ax) * (t.cz - t.az) - (t.cx - t.ax) * (t.bz - t.az)) * .5;
  }

  return total;
}

// A roof plane is a direction, not a triangle. Counting distinct ones is how a
// plan that turns a corner is told from a box: a box has four however it is
// tessellated, an L has more.
std::map<std::tuple<int, int, int>, double> planesOf(const std::vector<RoofTriangle> &roof)
{
  std::map<std::tuple<int, int, int>, double> planes;

  for (const RoofTriangle &t : roof)
  {
    const double ux = t.bx - t.ax, uy = t.by - t.ay, uz = t.bz - t.az;
    const double vx = t.cx - t.ax, vy = t.cy - t.ay, vz = t.cz - t.az;
    double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    const double length = std::sqrt(nx * nx + ny * ny + nz * nz);

    if (length < 1e-9)
    {
      continue;
    }

    nx /= length;
    ny /= length;
    nz /= length;

    if (ny < 0)
    {
      nx = -nx;
      ny = -ny;
      nz = -nz;
    }

    planes[{int(std::lround(nx * 16)), int(std::lround(ny * 16)), int(std::lround(nz * 16))}] += std::abs((t.bx - t.ax) * (t.cz - t.az) - (t.cx - t.ax) * (t.bz - t.az)) * .5;
  }

  return planes;
}

// Holes, measured rather than looked at.
//
// A roof that covers exactly its own plan can still be torn: the surface folds
// somewhere and falls short somewhere else, and the two cancel. Total area
// cannot see that, and a render cannot either - a slit and a crease shade the
// same - so the question has to be asked of the plan: which part of this
// building has no roof over it?
//
// Tried first as a question about edges, which is the elegant form: every edge
// of a closed surface is shared, so an unshared edge inside the plan is a
// hole. It does not survive contact with this surface. The faces do not share
// vertices - a merged plane meets a row of strip triangles - so the pairing is
// per line rather than per vertex, and two edges a tenth of a millimetre apart
// are the same line to the roof and different lines to any keying of it. Every
// tolerance that stopped it inventing slits started missing real ones.
//
// Tried second as a grid, which is honest and slow, and reported a quarter of
// a square metre of hole along the hip of a plain rectangle: cell centres
// landing on the diagonal between two triangles are inside both in exact
// arithmetic and inside neither once the cross products have rounded.
//
// So: the plan, less the shadow of every face of the roof, as a boolean. No
// sampling and no tolerance of its own beyond the quantisation the whole
// library already works in.
double holeArea(const std::vector<RoofTriangle> &roof, const Polygon &ground)
{
  std::vector<Operand> shadows;

  for (const RoofTriangle &t : roof)
  {
    if (std::abs((t.bx - t.ax) * (t.cz - t.az) - (t.cx - t.ax) * (t.bz - t.az)) * .5 < 1e-7)
    {
      continue;
    }

    Polygon face;
    face.outer = {
      {toUnit(t.ax), toUnit(t.az)},
      {toUnit(t.bx), toUnit(t.bz)},
      {toUnit(t.cx), toUnit(t.cz)},
    };

    // Quantising can bring two corners of a thin triangle onto the same
    // micrometre. Dropping the ring then is right - it has no area left - but
    // dropping a face that merely fails some other check is not: its own area
    // comes back as a hole in the answer, and something downstream believes
    // it. This is the false hole that the skeleton's patching pass was built
    // to fill in.
    Ring deduped;

    for (const Point &corner : face.outer)
    {
      if (deduped.empty() || corner.x != deduped.back().x || corner.y != deduped.back().y)
      {
        deduped.push_back(corner);
      }
    }

    if (deduped.size() < 3 || (deduped.front().x == deduped.back().x
                               && deduped.front().y == deduped.back().y))
    {
      continue;
    }

    face.outer = std::move(deduped);

    if (!counterClockwise(face.outer))
    {
      reverse(face.outer);
    }

    shadows.push_back(toOperand(face));
  }

  if (shadows.empty())
  {
    return std::abs(areaMetres(ground));
  }

  std::vector<Polygon> bare;

  if (!differenceOf(toOperand(ground), shadows, bare, nullptr))
  {
    return 0;
  }

  double total = 0;

  for (const Polygon &piece : bare)
  {
    total += std::abs(areaMetres(piece));

    if (getenv("HOLE_MAP") != nullptr && std::abs(areaMetres(piece)) > 1e-4)
    {
      std::printf("      hole %.4f m2 :", std::abs(areaMetres(piece)));

      for (const Point &q : piece.outer)
      {
        std::printf(" %.3f,%.3f", toMetres(q.x), toMetres(q.y));
      }

      std::printf("\n");
    }
  }

  return total;
}

// Is this a surface at all?
//
// Two questions, and between them they say everything a render cares about.
//
// First: is it single-valued. A roof is a height over the ground, so exactly
// one triangle stands over any point inside the plan. Two means one wall won
// the competition and another got a face anyway - the fault that put a second
// surface six metres over a peasant cottage, sampling as a spike and shading as
// a tear. Zero means a hole.
//
// Second: is it closed. Snap every vertex to the millimetre and count each
// undirected edge; inside the surface an edge belongs to two triangles, and
// only the edges lying on the plan's own boundary may belong to one. An edge
// used once anywhere else is a crack, and a crack is invisible to any measure
// of area because the two sides of it cover the same ground.
//
// The first is sampled and the second is exact, which is the right way round:
// stacking is an area fault and shows at any resolution, a crack has no area at
// all and would need a grid finer than the crack to find.
uint32_t coverFault(const Polygon &ground, const std::vector<RoofTriangle> &roof,
                    uint32_t &under, uint32_t &over)
{
  under = 0;
  over = 0;

  double lowX = 1e30, highX = -1e30, lowZ = 1e30, highZ = -1e30;

  for (const Point &corner : ground.outer)
  {
    lowX = std::min(lowX, toMetres(corner.x));
    highX = std::max(highX, toMetres(corner.x));
    lowZ = std::min(lowZ, toMetres(corner.y));
    highZ = std::max(highZ, toMetres(corner.y));
  }

  const uint32_t steps = 90;
  uint32_t inside = 0;

  for (uint32_t j = 0; j < steps; ++j)
  {
    const double z = lowZ + (highZ - lowZ) * (double(j) + .5) / double(steps);

    for (uint32_t i = 0; i < steps; ++i)
    {
      const double x = lowX + (highX - lowX) * (double(i) + .5) / double(steps);

      if (!contains(ground, Point{toUnit(x), toUnit(z)}))
      {
        continue;
      }

      ++inside;

      // Counting how many triangles cover a point is the wrong question: a
      // sample landing exactly on a hip is in both of the triangles that meet
      // there, and a grid over a square puts ninety of them on each diagonal.
      // What matters is whether they agree about the height - two triangles
      // meeting along an edge do, two surfaces stacked over each other do not.
      double lowest = 1e30, highest = -1e30;

      for (const RoofTriangle &t : roof)
      {
        // A gable is vertical. Its shadow on the ground is a line, and a line
        // read as a triangle contains every point there is - so counting them
        // here makes every plan look perfectly covered, which is exactly how
        // the sampler came to disagree with the boolean about a real hole.
        if (t.gable
            || std::abs((t.bx - t.ax) * (t.cz - t.az) - (t.cx - t.ax) * (t.bz - t.az)) * .5 < 1e-9)
        {
          continue;
        }

        const double d1 = (x - t.bx) * (t.az - t.bz) - (t.ax - t.bx) * (z - t.bz);
        const double d2 = (x - t.cx) * (t.bz - t.cz) - (t.bx - t.cx) * (z - t.cz);
        const double d3 = (x - t.ax) * (t.cz - t.az) - (t.cx - t.ax) * (z - t.az);

        if ((d1 < -1e-9 || d2 < -1e-9 || d3 < -1e-9) && (d1 > 1e-9 || d2 > 1e-9 || d3 > 1e-9))
        {
          continue;
        }

        const double den = (t.bz - t.cz) * (t.ax - t.cx) + (t.cx - t.bx) * (t.az - t.cz);

        if (std::abs(den) < 1e-12)
        {
          continue;
        }

        const double w1 = ((t.bz - t.cz) * (x - t.cx) + (t.cx - t.bx) * (z - t.cz)) / den;
        const double w2 = ((t.cz - t.az) * (x - t.cx) + (t.ax - t.cx) * (z - t.cz)) / den;
        const double y = w1 * t.ay + w2 * t.by + (1 - w1 - w2) * t.cy;

        lowest = std::min(lowest, y);
        highest = std::max(highest, y);
      }

      under += highest < -1e29 ? 1 : 0;
      over += highest > -1e29 && highest - lowest > .01 ? 1 : 0;

      if (highest > -1e29 && highest - lowest > .01 && getenv("PILE_MAP") != nullptr)
      {
        std::printf("      pile at %.2f,%.2f  %.3f vs %.3f\n", x, z, lowest, highest);
      }
    }
  }

  return inside;
}

uint32_t openEdges(const Polygon &ground, const std::vector<RoofTriangle> &roof)
{
  std::map<std::array<int64_t, 6>, uint32_t> edges;

  // In space, not in plan.
  //
  // Keyed on the ground alone, two faces that meet along a line and disagree
  // about the height on it pair up like any other - the key cannot tell a
  // continuous surface from one with a vertical step in it, and a step is
  // exactly the fault worth finding. A dispensary's top floor roofed perfectly
  // by this test and came out of the planner with nineteen metres of open edge,
  // and the only difference between the two was the height.
  const auto key = [](const double ax, const double ay, const double az, const double bx, const double by, const double bz) {
    std::array<int64_t, 6> k{
      int64_t(std::llround(ax * 1e9)), int64_t(std::llround(ay * 1e9)), int64_t(std::llround(az * 1e9)), int64_t(std::llround(bx * 1e9)), int64_t(std::llround(by * 1e9)), int64_t(std::llround(bz * 1e9))};

    if (std::lexicographical_compare(k.begin(), k.begin() + 3, k.begin() + 3, k.end()))
    {
      std::swap_ranges(k.begin(), k.begin() + 3, k.begin() + 3);
    }

    return k;
  };

  for (const RoofTriangle &t : roof)
  {
    ++edges[key(t.ax, t.ay, t.az, t.bx, t.by, t.bz)];
    ++edges[key(t.bx, t.by, t.bz, t.cx, t.cy, t.cz)];
    ++edges[key(t.cx, t.cy, t.cz, t.ax, t.ay, t.az)];
  }

  // An edge on the plan's own outline is meant to be used once; the wall is
  // what is on its other side.
  const auto onOutline = [&](const std::array<int64_t, 6> &k) {
    const auto lies = [&](const Ring &ring) {
      for (size_t i = 0; i < ring.size(); ++i)
      {
        const double ax = toMetres(ring[i].x), az = toMetres(ring[i].y);
        const double bx = toMetres(ring[(i + 1) % ring.size()].x);
        const double bz = toMetres(ring[(i + 1) % ring.size()].y);

        const double ex = bx - ax, ez = bz - az;
        const double span = std::hypot(ex, ez);

        if (span < 1e-9)
        {
          continue;
        }

        // On the segment, not on its line.
        //
        // A stepped outline has walls whose lines run right across the plan: a
        // step a metre deep at one corner puts a line through the middle of
        // everything. Excusing an edge for lying on that line excused edges in
        // the interior that had nothing on their other side - which is how a
        // dispensary's top floor passed here and came out of the planner with
        // nineteen metres of open edge.
        // On the segment, within the grain of the key. Edges are snapped to
        // the millimetre before they are counted, so an endpoint of a wall can
        // land half a millimetre past the end of it - and measured in the
        // parameter along the wall that is a rejection, which threw out edges
        // lying exactly on the wall they belonged to.
        const auto off = [&](const double x, const double z) {
          const double at = ((x - ax) * ex + (z - az) * ez) / (span * span);
          const double slack = .002 / span;

          if (at < -slack || at > 1. + slack)
          {
            return 1e30;
          }

          return std::abs((x - ax) * ez - (z - az) * ex) / span;
        };

        if (off(double(k[0]) / 1e9, double(k[2]) / 1e9) < .002 && off(double(k[3]) / 1e9, double(k[5]) / 1e9) < .002)
        {
          return true;
        }
      }

      return false;
    };

    if (lies(ground.outer))
    {
      return true;
    }

    for (const Ring &hole : ground.holes)
    {
      if (lies(hole))
      {
        return true;
      }
    }

    return false;
  };

  uint32_t loose = 0;

  for (const auto &entry : edges)
  {
    // A vertical face projects to a line, and both ends of its edge land on the
    // same place on the ground. That is not an edge of the surface seen from
    // above and it has nothing to pair with here.
    if (entry.first[0] == entry.first[3] && entry.first[2] == entry.first[5])
    {
      continue;
    }

    // Odd is open. Even is closed - and more than two means the same face was
    // built more than once, which is waste rather than a hole, so it is counted
    // where it belongs and not here.
    if (entry.second % 2 == 1 && !onOutline(entry.first))
    {
      ++loose;

      if (getenv("EDGE_MAP") != nullptr)
      {
        std::printf(
            "      open x%u (%.3f,%.3f,%.3f)-(%.3f,%.3f,%.3f)\n",
            entry.second,
            double(entry.first[0]) / 1e9,
            double(entry.first[1]) / 1e9,
            double(entry.first[2]) / 1e9,
            double(entry.first[3]) / 1e9,
            double(entry.first[4]) / 1e9,
            double(entry.first[5]) / 1e9);
      }
    }
  }

  return loose;
}

struct Shape
{
  const char *name;
  std::vector<Pt> outer;
  std::vector<std::vector<Pt>> holes;
  int leastPlanes; // Unused where the shape's own directions decide; see below.
};

} // namespace

int main()
{
  std::cout << "Roofs over every shape a plan can produce\n";

  std::vector<Shape> shapes = {
    {"square", {{0, 0}, {12, 0}, {12, 12}, {0, 12}}, {}, 4},
    {"rect", {{0, 0}, {20, 0}, {20, 9}, {0, 9}}, {}, 4},
    {"long-range", {{0, 0}, {34, 0}, {34, 7}, {0, 7}}, {}, 4},
    {"trapezoid", {{0, 0}, {16, 0}, {12, 9}, {4, 9}}, {}, 4},
    {"wedge", {{0, 0}, {18, 0}, {13, 10}, {0, 6}}, {}, 4},
    {"pentagon", {{0, 0}, {11, 0}, {14, 8}, {6, 13}, {-3, 8}}, {}, 5},
    {"L", {{0, 0}, {18, 0}, {18, 8}, {8, 8}, {8, 18}, {0, 18}}, {}, 6},
    {"L-uneven", {{0, 0}, {15, 0}, {15, 8}, {6, 8}, {6, 16}, {0, 16}}, {}, 6},
    {"T", {{0, 0}, {20, 0}, {20, 7}, {13, 7}, {13, 16}, {7, 16}, {7, 7}, {0, 7}}, {}, 8},
    {"U", {{0, 0}, {20, 0}, {20, 16}, {14, 16}, {14, 6}, {6, 6}, {6, 16}, {0, 16}}, {}, 8},
    {"E", {{0, 0}, {7, 0}, {7, 6}, {16, 6}, {16, 0}, {23, 0}, {23, 22}, {16, 22}, {16, 16}, {7, 16}, {7, 22}, {0, 22}}, {}, 8},
    {"cross", {{7, 0}, {14, 0}, {14, 7}, {21, 7}, {21, 14}, {14, 14}, {14, 21}, {7, 21}, {7, 14}, {0, 14}, {0, 7}, {7, 7}}, {}, 8},
    {"comb", {{0, 0}, {21, 0}, {21, 13}, {17, 13}, {17, 5}, {13, 5}, {13, 13}, {9, 13}, {9, 5}, {5, 5}, {5, 13}, {0, 13}}, {}, 10},
    // A staircase a few decimetres across, of the kind the overhang trim leaves.
    {"notched", {{4.60, 0.25}, {4.60, -0.32}, {16.50, -0.32}, {16.50, 7.90}, {5.36, 7.90}, {5.36, 7.45}, {5.05, 7.45}, {5.05, 0.25}}, {}, 4},
    // A terrace, as the trim against its neighbours leaves it: a rectangle
    // with a 63 cm jog on one side and a 22 cm jog on the other, both at the
    // same height. Copied off `terrace-narrow` in the showcase, whose roof had
    // a slit along the hip and whose every area check passed.
    {"trimmed-terrace", {{7.65, 8.323}, {7.434, 8.323}, {7.434, 13.15}, {.479, 13.15}, {.479, 8.323}, {-.15, 8.323}, {-.15, -.15}, {7.65, -.15}}, {}, 4},
    // Off a temple's ground floor, where its tower stands on one corner: two
    // steps down one side and a long run along the other. Every synthetic shape
    // above roofed cleanly and this one piled a second surface over a fifth of
    // itself, which is the difference between a shape drawn to exercise a case
    // and a shape a plan actually produced.
    {"temple-step",
     {{-7.853197, -17.042529}, {-7.853197, -18.257419}, {6.968815, -18.257419}, {6.968815, -12.990782}, {-4.148047, -12.990782}, {-4.148047, -15.341248}, {-5.864712, -15.341248}, {-5.864712, -17.042529}},
     {},
     4},
    // A commerce house's upper floor: a long range with a shallow bay pushed
    // out of one side.
    //
    // THIS ONE FAILS, and it is here to. Its roof covers its plan exactly, is
    // single-valued everywhere, and is still slit: a chord runs under a ridge
    // that bends twice, and along it the faces that got the bends right stand
    // half a metre higher with nothing between them. Eight open edges at every
    // pitch it is asked for.
    //
    // The cause is the bound on how far a wall's plane carries past its own
    // ends, which is a cut at forty-five degrees from the end. That cut lands
    // where the wall's plane still has height, so the envelope steps there
    // rather than crossing. Taking the bound away is much worse - ninety-nine
    // failures against seven, because without it a far wall dips under a near
    // one across a whole arm - so the answer is a bound that ends a wall where
    // its plane is already beaten, which is what the motorcycle graph is for.
    // Fifty-five pieces of the eighteen classes are slit this way.
    {"shallow-bay", {{-11.037428, -0.541720}, {-11.037428, -6.625794}, {-3.243328, -6.625794}, {-3.243328, -7.504937}, {4.975523, -7.504937}, {4.975523, -6.625794}, {7.687961, -6.625794}, {7.687961, -0.541720}}, {}, 4},
    // A dispensary's top floor, as the storey above and the range cut leave
    // it: a long block with a two-step staircase bitten out of one corner, the
    // steps a metre and three quarters of a metre deep. Its roof came out with
    // nineteen metres of open edge in the building and the shape is the whole
    // of the reason.
    {"dispensary-steps",
     {{-5.241066, 2.458816},
      {-5.241066, -3.207652},
      {8.926282, -3.207652},
      {8.926282, 1.047137},
      {5.630431, 1.047137},
      {5.630431, 0.023740},
      {4.645537, 0.023740},
      {4.645537, -0.736053},
      {3.886188, -0.736053},
      {3.886188, 2.458816}},
     {},
     4},
    {"courtyard", {{0, 0}, {30, 0}, {30, 24}, {0, 24}}, {{{8, 7}, {8, 17}, {22, 17}, {22, 7}}}, 8},
    {"court-narrow", {{0, 0}, {26, 0}, {26, 20}, {0, 20}}, {{{7, 6}, {7, 14}, {19, 14}, {19, 6}}}, 8},
  };

  Shape rotated{"rotated-L", {}, {}, 6};
  for (const Pt &q : std::vector<Pt>{{0, 0}, {18, 0}, {18, 8}, {8, 8}, {8, 18}, {0, 18}})
  {
    rotated.outer.emplace_back(q.first * .8 - q.second * .6 + 30000., q.first * .6 + q.second * .8 - 20000.);
  }
  shapes.push_back(rotated);

  const double ridge = 3.0;

  for (const Shape &shape : shapes)
  {
    Polygon ground;
    ground.outer = ringOf(shape.outer);

    for (const auto &hole : shape.holes)
    {
      ground.holes.push_back(ringOf(hole));
    }

    const double area = areaMetres(ground);

    RoofOptions options;
    options.ridgeHeight = ridge;

    std::vector<RoofTriangle> roof;
    std::string error;

    if (!roofFromSkeleton(ground, options, roof, &error))
    {
      check(false, std::string(shape.name) + ": roofed at all (" + error + ")");

      continue;
    }

    // What the roof covers, seen from above, must be the building and nothing
    // more. Short means bare wall; over means the surface has folded onto
    // itself, and both once happened by a fifth or more.
    const double swept = planArea(roof);
    const double slip = area > 1e-9 ? std::abs(swept - area) / area : 1.0;

    // Nothing above the ridge and nothing below the eaves.
    double lowest = 1e30, highest = -1e30;

    for (const RoofTriangle &t : roof)
    {
      lowest = std::min({lowest, t.ay, t.by, t.cy});
      highest = std::max({highest, t.ay, t.by, t.cy});
    }

    // Flat triangles among the slopes.
    //
    // This is the comb. Triangulating the band between two contours as one
    // polygon produces triangles with all three corners on the same contour -
    // level where the roof should fall - and interleaved with the sloping ones
    // they read as gaps torn in the surface. The cap at the very peak is
    // legitimately flat and is tiny, so the bar is on area rather than count.
    double flat = 0;

    for (const RoofTriangle &t : roof)
    {
      if (std::abs(t.ay - t.by) < 1e-6 && std::abs(t.ay - t.cy) < 1e-6)
      {
        flat += std::abs((t.bx - t.ax) * (t.cz - t.az) - (t.cx - t.ax) * (t.bz - t.az)) * .5;
      }
    }

    const auto planes = planesOf(roof);

    std::printf("    %-13s %5.1f m2, %4zu faces, %2zu planes, peak %.3f, slip %.2f%%, flat %.2f m2\n", shape.name, area, roof.size(), planes.size(), highest, 100.0 * slip, flat);

    // The same shape at a pitch, where nothing rescales the result: asked for
    // a ridge height the tallest vertex is normalised to the number given, and
    // a surface built twice over the same ground looks exactly like one.
    //
    // At several pitches, because the fault is not always pitch-free. A temple
    // step off a real plan roofed cleanly at the pitch this file used and piled
    // a second surface over a fifth of itself at the shallower one the planner
    // chose for it - which one wall reaches past another depends on how far it
    // climbs on the way.
    for (const double slope : {.18, .30, .42, .55, .80, 1.15})
    {
      RoofOptions pitched;
      pitched.slope = slope;

      std::vector<RoofTriangle> sloped;
      uint32_t uncovered = 0, doubled = 0, sampled = 0, loose = 0;

      if (roofFromSkeleton(ground, pitched, sloped, nullptr))
      {
        sampled = coverFault(ground, sloped, uncovered, doubled);
        loose = openEdges(ground, sloped);
      }

      const std::string at = std::string(shape.name) + " at " + std::to_string(slope).substr(0, 4);

      std::printf("    %-15s pitch %.2f: %u of %u bare, %u doubled, %u open edges\n",
                  shape.name, slope, uncovered, sampled, doubled, loose);

      check(sampled > 0 && uncovered == 0, at + ": every point of the plan has a roof over it");
      check(doubled == 0, at + ": and only one, so the roof is a height and not a pile");
      check(loose == 0, at + ": and it is closed everywhere but its own outline");
      // Aichholzer et al. (1995) define a roof as a terrain whose facets come
      // from the halfplanes pi(e) - each bounded by the *line of an edge of the
      // plan*, at the pitch, leaning inwards. That is the definition, not a
      // nicety, and it is the one thing the checks above cannot see: a facet
      // standing in a plane through no edge at all still covers its plan
      // exactly once, closes, and holds the pitch.
      //
      // So ask it directly. Rebuild the eaves the way the roof walks them -
      // outer ring first, then each court, skipping zero-length edges - and
      // require every face to stand at the pitch above the *line* of the eave
      // it names. A face that is really the neighbour's, or one of the square
      // end caps the construction cuts a wall's claim with, fails here and
      // nowhere else.
      struct Eave
      {
        double ax, az, nx, nz;
      };

      std::vector<Eave> eaves;
      {
        std::vector<const Ring *> rings{&ground.outer};
        for (const Ring &hole : ground.holes)
        {
          rings.push_back(&hole);
        }
        for (const Ring *ring : rings)
        {
          for (size_t i = 0; i < ring->size(); ++i)
          {
            const double ax = toMetres((*ring)[i].x), az = toMetres((*ring)[i].y);
            const double bx = toMetres((*ring)[(i + 1) % ring->size()].x);
            const double bz = toMetres((*ring)[(i + 1) % ring->size()].y);
            const double length = std::hypot(bx - ax, bz - az);
            if (length < 1e-6)
            {
              continue;
            }
            eaves.push_back({ax, az, -(bz - az) / length, (bx - ax) / length});
          }
        }
      }

      bool correctPitch = true, sourceEdges = true, ownPlane = true;
      double worstOffPlane = 0;
      uint32_t offPlaneEdge = 0;
      for (const RoofTriangle &t : sloped)
      {
        sourceEdges = sourceEdges && t.edge < eaves.size();
        if (t.gable || t.edge >= eaves.size())
        {
          continue;
        }
        const double ux = t.bx - t.ax, uy = t.by - t.ay, uz = t.bz - t.az;
        const double vx = t.cx - t.ax, vy = t.cy - t.ay, vz = t.cz - t.az;
        const double ny = uz * vx - ux * vz;
        if (std::abs(ny) > 1e-12)
        {
          correctPitch = correctPitch && std::hypot(uy * vz - uz * vy, ux * vy - uy * vx) <= slope * std::abs(ny) * 1.002;
        }

        const Eave &e = eaves[t.edge];
        const double x[3]{t.ax, t.bx, t.cx}, y[3]{t.ay, t.by, t.cy}, z[3]{t.az, t.bz, t.cz};
        for (int k = 0; k < 3; ++k)
        {
          const double expected = slope * ((x[k] - e.ax) * e.nx + (z[k] - e.az) * e.nz);
          if (std::abs(y[k] - expected) > worstOffPlane)
          {
            worstOffPlane = std::abs(y[k] - expected);
            offPlaneEdge = t.edge;
          }
        }
      }
      // A millimetre: the grid is micrometres and a face spans metres, so
      // anything this construction rounds stays orders below it.
      ownPlane = worstOffPlane <= 1e-3;
      if (!ownPlane)
      {
        std::printf("      off its own eave's plane by %.4f m, worst on eave %u\n", worstOffPlane, offPlaneEdge);
      }
      // Aichholzer et al., Lemma 1: the skeleton "consists of exactly n
      // connected faces", one for each edge of the plan. So count the faces a
      // plane actually got - not triangles, but connected pieces sharing a
      // plane - because a plane that arrives in pieces is a face the
      // construction took apart, and every piece of it costs its neighbours a
      // vertex apiece through `conform` as well as its own fan.
      //
      // One is the answer. This asserts a ceiling rather than the answer,
      // because a face genuinely split by the plan (a wall seen from both sides
      // of a court) is legitimately more than one piece, and because a triangle
      // count is not a face count.
      //
      // Sixteen, and the reason it is not eight is worth knowing: every
      // axis-aligned plan here comes in at eight or under, and `rotated-L`
      // comes in at thirteen. A plan square to the axes puts every bisector
      // intersection on the half-grid exactly, so the shards of a face share
      // their seams to the last digit and the union welds all of them. Turn the
      // plan and those intersections are irrational, the seams weld only where
      // rounding agrees, and the face keeps about twice the pieces. Before this
      // construction one wall of one plan arrived as twenty-eight, which is
      // what the ceiling is really guarding against.
      std::map<uint32_t, size_t> trianglesPerEave;
      for (const RoofTriangle &t : sloped)
      {
        if (!t.gable)
        {
          ++trianglesPerEave[t.edge];
        }
      }
      size_t worstEave = 0;
      for (const auto &entry : trianglesPerEave)
      {
        worstEave = std::max(worstEave, entry.second);
      }
      check(worstEave <= 16, at + ": no single eave's face is a heap of triangles (worst " + std::to_string(worstEave) + ")");

      check(correctPitch, at + ": every face respects the requested pitch");
      check(sourceEdges, at + ": every face identifies an original eave");
      check(ownPlane, at + ": and stands in that eave's own plane, so it is a roof facet");
    }

    {
      // And the same three questions of the roof this file builds to a ridge
      // height, which is a different call and has its own pitch.
      uint32_t uncovered = 0, doubled = 0;
      const uint32_t sampled = coverFault(ground, roof, uncovered, doubled);
      const uint32_t loose = openEdges(ground, roof);

      std::printf("    %-15s to a ridge: %u of %u bare, %u doubled, %u open edges\n",
                  shape.name, uncovered, sampled, doubled, loose);

      check(sampled > 0 && uncovered == 0,
            std::string(shape.name) + " to a ridge: every point of the plan has a roof over it");
      check(doubled == 0,
            std::string(shape.name) + " to a ridge: and only one");
      check(loose == 0,
            std::string(shape.name) + " to a ridge: and it is closed everywhere but its outline");
    }

    check(slip < .03, std::string(shape.name) + ": the roof covers its own plan and no more");
    check(std::abs(highest - ridge) < 1e-6, std::string(shape.name) + ": it reaches exactly the ridge height asked for");
    check(lowest > -1e-6, std::string(shape.name) + ": and nothing hangs below the eaves");
    check(flat < area * .02, std::string(shape.name) + ": and its slopes slope");
    // One slope per direction the outline runs in.
    //
    // Counting corners was the obvious expectation and it is wrong: every edge
    // facing the same way carries the same plane however many of them there
    // are, so an axis-aligned L has four slopes and not six. What the shape
    // guarantees is one plane per distinct edge direction - which is also the
    // test that a plan turning a corner gets more than a box does, since a box
    // has four directions and an L still has four but a pentagon has five.
    std::map<std::pair<int, int>, int> directions;

    const auto note = [&](const std::vector<Pt> &ring) {
      for (size_t i = 0; i < ring.size(); ++i)
      {
        const Pt &a = ring[i], &b = ring[(i + 1) % ring.size()];
        const double dx = b.first - a.first, dz = b.second - a.second;
        const double length = std::hypot(dx, dz);

        if (length > 1e-6)
        {
          directions[{int(std::lround(dx / length * 32)), int(std::lround(dz / length * 32))}]++;
        }
      }
    };

    note(shape.outer);

    for (const auto &hole : shape.holes)
    {
      note(hole);
    }

    check(int(planes.size()) >= int(directions.size()), std::string(shape.name) + ": and it has a slope for every direction its outline runs in");

    // What is left of the plan once every face has been subtracted from it.
    //
    // This was the arbiter, and it is kept as a second opinion rather than a
    // verdict. Subtracting twenty-odd thin triangles from a plan, each
    // quantised to the micrometre first, does not always leave nothing when
    // nothing is what is there: on a staircase off a temple's ground floor it
    // reported 1.25 square metres of daylight through a surface that a grid of
    // six thousand points found entirely covered, that covered exactly its own
    // plan area to the last square centimetre, and whose every edge paired.
    //
    // Believing it is what put the patching pass into the skeleton, and the
    // patch laid a flat-topped second surface over ground that already had a
    // roof. So the question is now asked directly - is there a point of this
    // plan with no triangle over it - and the boolean is printed beside it.
    const double bare = holeArea(roof, ground);

    if (bare > .01)
    {
      std::printf("    %-13s %.3f m2 where subtracting the faces leaves plan behind\n",
                  shape.name, bare);
    }
  }

  // Gables.
  //
  // A hipped roof falls on every side. A gabled one rises to the wall at its
  // ends and stops, and the wall carries on up as a triangle. Both are roofs a
  // plan asks for by name, and for a while every building in the town came out
  // hipped because the request never reached the surface. What a gable has to
  // show, measurably: a vertical face over the gabled edge, a ridge that runs
  // all the way out to that edge, and the same plan area as before - the roof
  // still covers the building and no more.
  {
    struct Case
    {
      const char *name;
      std::vector<Pt> outer;
      std::vector<size_t> gabled;
    };

    const std::vector<Case> cases = {
      {"rect-both-ends", {{0, 0}, {20, 0}, {20, 9}, {0, 9}}, {1, 3}},
      {"rect-one-end", {{0, 0}, {20, 0}, {20, 9}, {0, 9}}, {1}},
      {"square-gabled", {{0, 0}, {12, 0}, {12, 12}, {0, 12}}, {1, 3}},
      {"L-gabled-ends", {{0, 0}, {18, 0}, {18, 8}, {8, 8}, {8, 18}, {0, 18}}, {1, 4}},
      {"T-gabled-ends", {{0, 0}, {20, 0}, {20, 7}, {13, 7}, {13, 16}, {7, 16}, {7, 7}, {0, 7}}, {2, 4, 7}},
    };

    for (const Case &one : cases)
    {
      Polygon ground;
      ground.outer = ringOf(one.outer);

      const double area = areaMetres(ground);

      RoofOptions options;
      options.ridgeHeight = ridge;
      options.gables.assign(one.outer.size(), false);

      for (const size_t e : one.gabled)
      {
        options.gables[e] = true;
      }

      std::vector<RoofTriangle> roof;
      std::string error;

      if (!roofFromSkeleton(ground, options, roof, &error))
      {
        check(false, std::string(one.name) + ": roofed at all (" + error + ")");

        continue;
      }

      // A vertical face is the gable itself: the triangle of wall between the
      // two slopes. Measured by area rather than count, because how the wall
      // is tessellated is nobody's business.
      double vertical = 0, swept = 0, peak = 0;

      for (const RoofTriangle &t : roof)
      {
        const double ux = t.bx - t.ax, uy = t.by - t.ay, uz = t.bz - t.az;
        const double vx = t.cx - t.ax, vy = t.cy - t.ay, vz = t.cz - t.az;
        const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
        const double plan = std::abs((t.bx - t.ax) * (t.cz - t.az) - (t.cx - t.ax) * (t.bz - t.az)) * .5;

        swept += plan;
        peak = std::max({peak, t.ay, t.by, t.cy});

        if (length > 1e-9 && std::abs(ny / length) < .05)
        {
          vertical += length * .5;
        }
      }

      // The ridge has to reach the gabled wall. On a hip it stops a half-width
      // short of it, which is exactly the difference the eye reads.
      double reach = 1e30;

      for (const size_t e : one.gabled)
      {
        const Pt &a = one.outer[e], &b = one.outer[(e + 1) % one.outer.size()];
        const double ex = b.first - a.first, ez = b.second - a.second;
        const double elen = std::hypot(ex, ez);
        double nearest = 1e30;

        for (const RoofTriangle &t : roof)
        {
          const double px[3] = {t.ax, t.bx, t.cx}, py[3] = {t.ay, t.by, t.cy}, pz[3] = {t.az, t.bz, t.cz};

          for (int k = 0; k < 3; ++k)
          {
            if (py[k] > ridge - 1e-3)
            {
              nearest = std::min(nearest, std::abs((px[k] - a.first) * (-ez / elen) + (pz[k] - a.second) * (ex / elen)));
            }
          }
        }

        reach = std::min(reach, nearest);
      }

      const double slip = area > 1e-9 ? std::abs(swept - area) / area : 1.0;

      std::printf("    %-15s %5.1f m2, %4zu faces, gable wall %5.2f m2, ridge reaches within %.2f m, slip %.2f%%\n", one.name, area, roof.size(), vertical, reach, 100.0 * slip);

      std::map<std::array<int64_t, 6>, std::pair<int, int>> directed;
      for (const RoofTriangle &t : roof)
      {
        const double x[]{t.ax, t.bx, t.cx}, y[]{t.ay, t.by, t.cy}, z[]{t.az, t.bz, t.cz};
        for (int i = 0; i < 3; ++i)
        {
          const int j = (i + 1) % 3;
          std::array<int64_t, 6> edge{std::llround(x[i] * 1e9), std::llround(y[i] * 1e9), std::llround(z[i] * 1e9), std::llround(x[j] * 1e9), std::llround(y[j] * 1e9), std::llround(z[j] * 1e9)};
          int sign = 1;
          if (std::lexicographical_compare(edge.begin() + 3, edge.end(), edge.begin(), edge.begin() + 3))
          {
            std::swap_ranges(edge.begin(), edge.begin() + 3, edge.begin() + 3);
            sign = -1;
          }
          ++directed[edge].first;
          directed[edge].second += sign;
        }
      }
      bool oriented = true;
      for (const auto &entry : directed)
      {
        oriented = oriented && (entry.second.first != 2 || entry.second.second == 0);
      }
      check(oriented, std::string(one.name) + ": shared edges have opposite winding");
      check(vertical > .5, std::string(one.name) + ": the gabled end is a wall, not a hip");
      check(reach < .35, std::string(one.name) + ": and the ridge runs out to it");
      check(std::abs(peak - ridge) < 1e-6, std::string(one.name) + ": and the roof still reaches the height asked for");
      check(slip < .03, std::string(one.name) + ": and still covers its own plan and no more");
    }
  }

  // Pitch, independent of plan.
  //
  // `ridgeHeight` sets the peak whatever the outline; `slope` sets the rise
  // per metre of shrink. One of them has to be in charge, and getting that
  // backwards makes a large building flat and a small one a spire.
  {
    Polygon ground;
    ground.outer = ringOf({{0, 0}, {16, 0}, {16, 10}, {0, 10}});

    RoofOptions steep;
    steep.ridgeHeight = 0;
    steep.slope = 1.0;

    std::vector<RoofTriangle> roof;
    roofFromSkeleton(ground, steep, roof, nullptr);

    double peak = 0;

    for (const RoofTriangle &t : roof)
    {
      peak = std::max({peak, t.ay, t.by, t.cy});
    }

    // Half the narrow way across, at one metre of rise per metre in.
    std::printf("    %-13s peak %.2f at slope 1.0 (half-width 5.0)\n", "pitch", peak);
    check(std::abs(peak - 5.0) < .25, "a slope of one rises half the narrow span");
  }

  // Degenerate input is refused rather than half-roofed.
  {
    std::vector<RoofTriangle> roof;
    std::string error;

    Polygon sliver;
    sliver.outer = ringOf({{0, 0}, {10, 0}, {10, 0}});

    check(!roofFromSkeleton(sliver, {}, roof, &error) && roof.empty(), "a degenerate outline is refused, not half-roofed");
  }

  // The plan contract: every reflex corner square, at any rotation, with convex
  // corners free.
  //
  // This is the exact condition under which the L-infinity medial axis is the
  // straight skeleton (Aichholzer et al. 1995). An oblique reflex corner is
  // refused rather than approximated - measured against a mitred-offset
  // skeleton, `oblique-notch` came out 0.257 m wrong on a 4.47 m ridge, and a
  // sheared L reached 1.82 m on a 3.95 m ridge. There is no local repair for
  // it: the same paper proves a roof is not the lower envelope of per-edge
  // functions, because a distant reflex vertex changes the answer here.
  {
    std::vector<RoofTriangle> roof;
    std::string error;

    Polygon oblique;
    oblique.outer = ringOf({{0, 0}, {18, 0}, {17, 8}, {8, 7}, {6, 17}, {-2, 15}});
    check(!roofFromSkeleton(oblique, {}, roof, &error) && roof.empty() && !error.empty(),
          "an oblique reflex corner is refused, not approximated");

    // A right angle stays a right angle when the plan is turned, and so does
    // this: rotation is not what the construction depends on.
    for (const double degrees : {0., 17.3, 30., 41.})
    {
      const double t = degrees * 3.14159265358979323846 / 180, c = std::cos(t), s = std::sin(t);
      std::vector<Pt> turned;

      for (const Pt &p : std::vector<Pt>{{0, 0}, {18, 0}, {18, 8}, {8, 8}, {8, 18}, {0, 18}})
      {
        turned.push_back({p.first * c - p.second * s + 3.7, p.first * s + p.second * c - 2.1});
      }

      Polygon rotated;
      rotated.outer = ringOf(turned);
      const bool ok = roofFromSkeleton(rotated, {}, roof, &error);
      check(ok && !roof.empty(),
            "a square-cornered L is roofed at " + std::to_string(int(degrees)) + " degrees of rotation"
              + (ok ? "" : " [" + error + "]"));
    }

    // Convex corners are free at any angle: the neighbouring plane takes over
    // on the bisector without being told.
    Polygon mixed;
    mixed.outer = ringOf({{0, 0}, {20, 0}, {20, 8}, {12, 8}, {12, 16}, {0, 16}, {-5, 8}});
    check(roofFromSkeleton(mixed, {}, roof, &error) && !roof.empty(),
          "oblique convex corners are allowed beside a square reflex corner");

    // Where the line sits, pinned. A plan out of a boolean is square to about a
    // degree and must still be roofed; the error there is linear in the
    // deviation, about 0.066 m per degree per unit pitch, so a degree is
    // centimetres. Five degrees is refused, not because anything changes in
    // kind at five, but because that is where this stops being worth the error.
    for (const double off : {0.25, 0.7, 0.85, 1.5})
    {
      const double d = off * 3.14159265358979323846 / 180;
      Polygon bent;
      bent.outer = ringOf({{0, 0}, {18, 0}, {18, 8}, {8, 8},
                           {8 + 10 * std::sin(d), 8 + 10 * std::cos(d)},
                           {10 * std::sin(d), 8 + 10 * std::cos(d)}});
      check(roofFromSkeleton(bent, {}, roof, &error) && !roof.empty(),
            "a reflex corner " + std::to_string(off).substr(0, 4) + " degrees off square is still roofed");
    }

    for (const double off : {5.0, 15.0})
    {
      const double d = off * 3.14159265358979323846 / 180;
      Polygon bent;
      bent.outer = ringOf({{0, 0}, {18, 0}, {18, 8}, {8, 8},
                           {8 + 10 * std::sin(d), 8 + 10 * std::cos(d)},
                           {10 * std::sin(d), 8 + 10 * std::cos(d)}});
      check(!roofFromSkeleton(bent, {}, roof, &error) && roof.empty(),
            "and " + std::to_string(off).substr(0, 4) + " degrees off square is refused");
    }
  }

  // Invalid requests must fail explicitly, with no substitute hipped roof.
  {
    Polygon ground;
    ground.outer = ringOf({{0, 0}, {10, 0}, {10, 6}, {0, 6}});
    std::vector<RoofTriangle> roof;
    std::string error;
    RoofOptions invalid;
    invalid.gables = {true, true, true, true};
    check(!roofFromSkeleton(ground, invalid, roof, &error) && roof.empty() && !error.empty(), "all-gabled boundaries are rejected without a fallback roof");
    invalid = {};
    invalid.slope = std::numeric_limits<double>::quiet_NaN();
    check(!roofFromSkeleton(ground, invalid, roof, &error) && roof.empty(), "non-finite pitch is rejected");
    invalid.slope = -1;
    check(!roofFromSkeleton(ground, invalid, roof, &error) && roof.empty(), "negative pitch is rejected");
  }

  if (failures != 0)
  {
    std::cerr << failures << " roof test(s) failed\n";

    return 1;
  }

  std::cout << "All roof tests passed\n";

  return 0;
}
