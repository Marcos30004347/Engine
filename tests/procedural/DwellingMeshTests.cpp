// The mesh a blueprint turns into, checked as geometry.
//
// Every other test here measures the *plan*: how many doors survived, how much
// of the lot the house covers, whether a room is where the programme asked for
// it. None of that says whether what comes out is a solid: a roof can cover its
// plan exactly and still be torn, two faces can occupy the same ground at
// different heights, a wall can stand under nothing, a spike can shoot four
// metres out of a hip. Those are the faults that show in a render and hide from
// every number the planner reports about itself.
//
// So this takes the finished mesh - walls, roof, partitions - and asks it the
// questions you would ask of any mesh: is it closed where it should be, does it
// overlap itself, does the surface jump where it should be continuous, is
// anything degenerate, is anything absurdly far from where it belongs. It runs
// over every building class, because the faults are not evenly spread: a chapel
// is a box and a temple is not.
#include "procedural/world/generator/ProgramDwellingPlanner.hpp"
#include "procedural/world/generator/ProgrammeFit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace procedural;
using namespace procedural::world::generator;
namespace rewrite = procedural::world::rewrite;

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

using Point = rewrite::Position;

double shoelace(const std::vector<Point> &ring)
{
  double twice = 0;

  for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
  {
    twice += ring[j].x * ring[i].z - ring[i].x * ring[j].z;
  }

  return twice * .5;
}

bool within(const std::vector<Point> &ring, const double x, const double z)
{
  bool in = false;

  for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
  {
    const bool straddles = (ring[i].z > z) != (ring[j].z > z);

    if (straddles
        && x < (ring[j].x - ring[i].x) * (z - ring[i].z) / (ring[j].z - ring[i].z) + ring[i].x)
    {
      in = !in;
    }
  }

  return in;
}

// Every roof height over a point - one entry per face that covers it.
void roofHeights(const std::vector<rewrite::DwellingRoofFace> &roofs, const double x,
                 const double z, std::vector<double> &out)
{
  std::vector<double> boundaryHeights;
  for (const rewrite::DwellingRoofFace &face : roofs)
  {
    if (face.gable || face.polygon.size() < 3)
    {
      continue;
    }


    for (size_t k = 1; k + 1 < face.polygon.size(); ++k)
    {
      const Point &a = face.polygon[0];
      const Point &b = face.polygon[k];
      const Point &c = face.polygon[k + 1];

      const double d1 = (x - b.x) * (a.z - b.z) - (a.x - b.x) * (z - b.z);
      const double d2 = (x - c.x) * (b.z - c.z) - (b.x - c.x) * (z - c.z);
      const double d3 = (x - a.x) * (c.z - a.z) - (c.x - a.x) * (z - a.z);

      if ((d1 < -1e-9 || d2 < -1e-9 || d3 < -1e-9) && (d1 > 1e-9 || d2 > 1e-9 || d3 > 1e-9))
      {
        continue;
      }

      const double den = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);

      if (std::abs(den) < 1e-12)
      {
        continue;
      }

      const double w1 = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / den;
      const double w2 = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / den;
      const double height = w1 * a.y + w2 * b.y + (1 - w1 - w2) * c.y;
      const bool boundary = std::min({std::abs(w1), std::abs(w2), std::abs(1 - w1 - w2)}) < 1e-8;
      // Shared triangle edges are one surface. Still count overlapping
      // interiors, even when they have exactly the same height.
      if (boundary)
      {
        const bool shared = std::any_of(boundaryHeights.begin(), boundaryHeights.end(), [&](double y) {
          return std::abs(height - y) < 1e-7;
        });
        if (shared)
        {
          break;
        }
        boundaryHeights.push_back(height);
      }
      out.push_back(height);

      break;
    }
  }
}

// Everything wrong with one building's mesh.
struct Faults
{
  uint32_t degenerate = 0;   // A face with no area, or a NaN in it.
  uint32_t bare = 0;         // Ground that should be roofed and is not.
  uint32_t sampled = 0;
  uint32_t open = 0;         // A roof edge with nothing on its other side.
  uint32_t tee = 0;          // An edge that meets another one part-way along.
  uint32_t coincident = 0;   // Two roof faces over the same ground, same height.
  uint32_t stacked = 0;      // Two roof faces over the same ground, one above the other.
  double worstStack = 0;
  uint32_t buried = 0;       // A roof face standing inside a storey.
  uint32_t floating = 0;     // A partition with no storey around it.
  double peakOverWall = 0;   // How far the roof rises above the highest wall.
  double allowedRise = 1e30; // What its pitch and its span allow it.
  uint32_t steep = 0;        // A face far steeper than any roof pitch.
  uint32_t bent = 0;         // A face whose corners are not in one plane.
  uint32_t doubledFace = 0;  // An edge used by more faces than two.
  uint32_t lidded = 0;       // A court with a roof over it.
  uint32_t oversail = 0;     // Roof over ground no storey stands on.
  uint32_t outside = 0;
  uint32_t court = 0;
  double worstBend = 0;
  double worstSlope = 0;
  double openLength = 0;
};

// A roof is a surface, and a surface has two sides to every edge inside it.
//
// Sampling cannot see a crack: a vertical gap between two faces reads as a step
// in height, and a step in height is also what an honest eaves overhang looks
// like from above. The edges say it exactly. Snap every roof vertex to the
// roof intersection grid (one nanometre), count each undirected edge, and an edge used once is an edge with
// nothing on its other side. Gables count too - they are what closes the end of
// a range - so a slope that ends against its gable pairs up like any other.
//
// What is allowed to stay open is the eaves: the bottom edge of the roof, where
// the surface genuinely ends and the wall takes over.
struct EdgeTally
{
  uint32_t uses = 0;
  bool gable = false;
  rewrite::Position a, b;
};

int64_t snap(const double v)
{
  return int64_t(std::llround(v * 1e9));
}

void pairEdges(const std::vector<rewrite::DwellingRoofFace> &roofs,
               const std::vector<rewrite::DwellingStorey> &storeys, Faults &out)
{
  std::map<std::array<int64_t, 6>, EdgeTally> edges;
  double lowest = 1e30;

  for (const rewrite::DwellingRoofFace &face : roofs)
  {
    for (const Point &v : face.polygon)
    {
      lowest = std::min(lowest, v.y);
    }
  }

  for (const rewrite::DwellingRoofFace &face : roofs)
  {
    if (face.gable && std::getenv("MESH_NOGABLE") != nullptr)
    {
      continue;
    }

    for (size_t i = 0; i < face.polygon.size(); ++i)
    {
      const Point &a = face.polygon[i];
      const Point &b = face.polygon[(i + 1) % face.polygon.size()];

      std::array<int64_t, 6> key{snap(a.x), snap(a.y), snap(a.z),
                                 snap(b.x), snap(b.y), snap(b.z)};

      if (std::lexicographical_compare(key.begin(), key.begin() + 3, key.begin() + 3,
                                       key.end()))
      {
        std::swap_ranges(key.begin(), key.begin() + 3, key.begin() + 3);
      }

      if (std::equal(key.begin(), key.begin() + 3, key.begin() + 3))
      {
        continue;
      }

      EdgeTally &tally = edges[key];
      ++tally.uses;
      tally.gable = face.gable;
      tally.a = a;
      tally.b = b;
    }
  }

  std::vector<const EdgeTally *> loose;

  for (const auto &entry : edges)
  {
    // Odd is open; even is closed. More than twice means the same face was
    // built more than once - waste, and worth knowing, but not a hole, and
    // counting it as one sent me looking for tears that were not there.
    if (entry.second.uses % 2 == 1)
    {
      loose.push_back(&entry.second);
    }

    out.doubledFace += entry.second.uses > 2 ? 1 : 0;
  }

  for (const EdgeTally *edge : loose)
  {
    const double low = std::min(edge->a.y, edge->b.y);
    const double high = std::max(edge->a.y, edge->b.y);

    // The eaves: the bottom of the roof, at the head of a wall or at the lowest
    // the surface goes. An edge there is meant to be open.
    bool eaves = low < lowest + .06 && high < lowest + .06;

    for (const rewrite::DwellingStorey &storey : storeys)
    {
      eaves = eaves
              || (std::abs(low - storey.top) < .08 && std::abs(high - storey.top) < .08);
    }

    // A gable's outer edge runs straight down to the wall head, and the wall
    // is what closes it - walls are not in this surface, so a vertical edge
    // standing on a storey is not a hole.
    bool walled = std::hypot(edge->b.x - edge->a.x, edge->b.z - edge->a.z) < .002;

    if (walled)
    {
      walled = false;

      for (const rewrite::DwellingStorey &storey : storeys)
      {
        walled = walled || (low < storey.top + .08 && low > storey.base - .08);
      }
    }

    // And a lower roof that climbs to meet a taller mass ends against that
    // mass's wall. The wall is not part of this surface either, so an edge
    // lying along a storey's outline, between that storey's floor and its
    // ceiling, has something on its other side after all.
    if (!walled)
    {
      for (const rewrite::DwellingStorey &storey : storeys)
      {
        if (storey.outer.size() < 3 || low < storey.base - .08 || high > storey.top + .08)
        {
          continue;
        }

        const auto hugs = [&](const Point &p) {
          for (size_t w = 0; w < storey.outer.size(); ++w)
          {
            const Point &a = storey.outer[w];
            const Point &b = storey.outer[(w + 1) % storey.outer.size()];

            const double ex = b.x - a.x, ez = b.z - a.z;
            const double span = ex * ex + ez * ez;
            const double at =
              span < 1e-12
                ? 0.
                : std::min(1., std::max(0., ((p.x - a.x) * ex + (p.z - a.z) * ez) / span));

            if (std::hypot(p.x - (a.x + ex * at), p.z - (a.z + ez * at)) < .08)
            {
              return true;
            }
          }

          return false;
        };

        walled = walled || (hugs(edge->a) && hugs(edge->b));
      }
    }

    if (eaves || walled)
    {
      continue;
    }

    // Not at the eaves, so something should have been on its other side. If
    // another open edge runs along the same line and overlaps it, the two
    // surfaces do meet - just not vertex to vertex, which still shows as a
    // hairline in a render but is not a hole.
    const double dx = edge->b.x - edge->a.x;
    const double dy = edge->b.y - edge->a.y;
    const double dz = edge->b.z - edge->a.z;
    const double run = std::sqrt(dx * dx + dy * dy + dz * dz);

    bool met = false;

    for (const EdgeTally *other : loose)
    {
      if (other == edge || run < 1e-9)
      {
        continue;
      }

      const auto off = [&](const Point &p) {
        const double ax = p.x - edge->a.x, ay = p.y - edge->a.y, az = p.z - edge->a.z;
        const double along = (ax * dx + ay * dy + az * dz) / (run * run);

        return std::hypot(std::hypot(ax - dx * along, ay - dy * along), az - dz * along);
      };

      const auto at = [&](const Point &p) {
        return ((p.x - edge->a.x) * dx + (p.y - edge->a.y) * dy + (p.z - edge->a.z) * dz)
               / (run * run);
      };

      if (off(other->a) > .003 || off(other->b) > .003)
      {
        continue;
      }

      const double lowT = std::min(at(other->a), at(other->b));
      const double highT = std::max(at(other->a), at(other->b));

      met = met || (highT > .02 && lowT < .98);
    }

    if (met)
    {
      ++out.tee;
      if (std::getenv("MESH_DEBUG"))
      {
        std::printf("      tee%s (%.9f,%.9f,%.9f)-(%.9f,%.9f,%.9f)\n", edge->gable ? " gable" : "", edge->a.x, edge->a.y, edge->a.z, edge->b.x, edge->b.y, edge->b.z);
      }
    }
    else
    {
      ++out.open;
      out.openLength += run;

      if (std::getenv("MESH_DEBUG") != nullptr)
      {
        std::printf("      open%s (%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f)  %.2f m\n",
                    edge->gable ? " gable" : "      ", edge->a.x, edge->a.y, edge->a.z,
                    edge->b.x, edge->b.y, edge->b.z, run);
      }
    }
  }
}

Faults inspect(const rewrite::DwellingPlan &plan)
{
  Faults out;

  // --- degenerate faces ---
  // A face with no area is not a face. It renders as nothing, it confuses
  // every edge count that meets it, and it comes of a triangulator handed three
  // points that are really two. Area in space, not in plan: a gable is vertical
  // and has no plan area at all.
  const auto degenerate = [](const std::vector<Point> &face) {
    if (face.size() < 3)
    {
      return true;
    }

    for (const Point &v : face)
    {
      if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
      {
        return true;
      }
    }

    double area = 0;

    for (size_t k = 1; k + 1 < face.size(); ++k)
    {
      const Point &a = face[0];
      const Point &b = face[k];
      const Point &c = face[k + 1];

      const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
      const double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;

      area += .5
              * std::sqrt(std::pow(uy * vz - uz * vy, 2.) + std::pow(uz * vx - ux * vz, 2.)
                          + std::pow(ux * vy - uy * vx, 2.));
    }

    return area < 1e-12;
  };

  for (const rewrite::DwellingRoofFace &face : plan.roofs)
  {
    out.degenerate += degenerate(face.polygon) ? 1 : 0;
  }

  // --- how high the roof stands over the walls ---
  double wallTop = 0;
  bool anyWall = false;

  for (const rewrite::DwellingStorey &storey : plan.storeys)
  {
    wallTop = anyWall ? std::max(wallTop, storey.top) : storey.top;
    anyWall = true;
  }

  // How high a roof may stand over its walls is not a fixed number: a steep
  // medieval roof over a wide hall is meant to be tall. What it may not do is
  // out-rise the pitch it was built at, which caps it at half the widest span.
  double span = 0;

  for (const rewrite::DwellingStorey &storey : plan.storeys)
  {
    double lowX = 1e30, highX = -1e30, lowZ = 1e30, highZ = -1e30;

    for (const Point &p : storey.outer)
    {
      lowX = std::min(lowX, p.x);
      highX = std::max(highX, p.x);
      lowZ = std::min(lowZ, p.z);
      highZ = std::max(highZ, p.z);
    }

    span = std::max(span, std::max(highX - lowX, highZ - lowZ));
  }

  for (const rewrite::DwellingRoofFace &face : plan.roofs)
  {
    for (const Point &v : face.polygon)
    {
      out.peakOverWall = std::max(out.peakOverWall, v.y - wallTop);
    }
  }

  out.allowedRise = .55 * span * .5 + .4;

  // --- the roof over each storey, and whether it is continuous ---
  for (const rewrite::DwellingStorey &storey : plan.storeys)
  {
    if (storey.outer.size() < 3 || storey.base < 0)
    {
      continue;
    }

    const rewrite::DwellingStorey *above = nullptr;

    for (const rewrite::DwellingStorey &over : plan.storeys)
    {
      if (over.base > storey.base + .5 && (above == nullptr || over.base < above->base))
      {
        above = &over;
      }
    }

    double lowX = storey.outer.front().x, highX = lowX;
    double lowZ = storey.outer.front().z, highZ = lowZ;

    for (const Point &p : storey.outer)
    {
      lowX = std::min(lowX, p.x);
      highX = std::max(highX, p.x);
      lowZ = std::min(lowZ, p.z);
      highZ = std::max(highZ, p.z);
    }

    const double step = .2;
    const uint32_t across = std::min(200u, std::max(8u, uint32_t((highX - lowX) / step)));
    const uint32_t down = std::min(200u, std::max(8u, uint32_t((highZ - lowZ) / step)));


    for (uint32_t j = 0; j < down; ++j)
    {
      const double z = lowZ + (highZ - lowZ) * (double(j) + .5) / double(down);

      for (uint32_t i = 0; i < across; ++i)
      {
        const double x = lowX + (highX - lowX) * (double(i) + .5) / double(across);

        const bool courtyard = std::any_of(storey.holes.begin(), storey.holes.end(), [&](const auto &hole) {
          return within(hole, x, z);
        });
        if (!within(storey.outer, x, z) || courtyard)
        {
          continue;
        }

        // Under anything taller, not merely under the next storey up. A
        // mansion four storeys high has ground that the second floor has left
        // but the third still covers, and the roof over the third reads there
        // as a second roof stacked over the one covering the second - which is
        // a fault of the question, not of the building.
        bool sheltered = false;

        for (const rewrite::DwellingStorey &taller : plan.storeys)
        {
          sheltered = sheltered || (taller.base > storey.base + .5 && taller.outer.size() >= 3 && within(taller.outer, x, z) && std::none_of(taller.holes.begin(), taller.holes.end(), [&](const auto &hole) {
                                      return within(hole, x, z);
                                    }));
        }

        if (sheltered)
        {
          continue;
        }

        ++out.sampled;

        std::vector<double> over;
        roofHeights(plan.roofs, x, z, over);

        if (over.empty())
        {
          ++out.bare;
          if (std::getenv("MESH_DEBUG"))
          {
            std::printf("      bare at (%.9f,%.9f) storey %.6f\n", x, z, storey.base);
          }
          continue;
        }

        // Two faces over the same ground at the same height is one roof built
        // twice - the fault that used to put a second, differently pitched
        // surface over every building and left both of them showing.
        std::sort(over.begin(), over.end());

        for (size_t k = 1; k < over.size(); ++k)
        {
          const double apart = over[k] - over[k - 1];

          out.coincident += apart < .05 ? 1 : 0;

          // A roof is the lower envelope of its walls' planes: over any one
          // point on a storey there is one surface. A second face above the
          // first is a wall that lost the competition and got a face anyway.
          if (apart >= .05)
          {
            ++out.stacked;
            out.worstStack = std::max(out.worstStack, apart);
          }
        }
      }
    }

  }

  // --- a roof over ground the building does not have ---
  //
  // A storey's plate has holes in it where the plan is open: a court the
  // building wraps around, a light well, a yard the rooms look onto. Those are
  // outside. Roofing them is not a crack or a fold - every measure of the
  // surface is happy with it - it is a building with a lid over its courtyard,
  // and the only way to see it is to ask about the ground that was deliberately
  // left out.
  for (const rewrite::DwellingStorey &storey : plan.storeys)
  {
    for (const std::vector<Point> &hole : storey.holes)
    {
      if (hole.size() < 3)
      {
        continue;
      }

      double lowX = hole.front().x, highX = lowX;
      double lowZ = hole.front().z, highZ = lowZ;

      for (const Point &p : hole)
      {
        lowX = std::min(lowX, p.x);
        highX = std::max(highX, p.x);
        lowZ = std::min(lowZ, p.z);
        highZ = std::max(highZ, p.z);
      }

      for (uint32_t j = 0; j < 24; ++j)
      {
        const double z = lowZ + (highZ - lowZ) * (double(j) + .5) / 24.;

        for (uint32_t i = 0; i < 24; ++i)
        {
          const double x = lowX + (highX - lowX) * (double(i) + .5) / 24.;

          if (!within(hole, x, z))
          {
            continue;
          }

          ++out.court;

          std::vector<double> over;
          roofHeights(plan.roofs, x, z, over);

          for (const double y : over)
          {
            // Roofed from above, which is the fault. A court in an upper
            // storey is a light well, and the floor below it has a ceiling -
            // that ceiling is inside the court by construction and is not a lid
            // on it.
            out.lidded += y > storey.top + .2 ? 1 : 0;
          }
        }
      }
    }
  }

  // --- roof over ground the building does not have ---
  //
  // The general form of the court question, and the one that catches what the
  // court question cannot: a notch in the plan that is open to the outside
  // rather than enclosed by it is not a court, is not a hole in any plate, and
  // is just as wrong to put a roof over. So the question is asked of the whole
  // footprint - every point under a roof has to be on some storey's floor.
  {
    double lowX = 1e30, highX = -1e30, lowZ = 1e30, highZ = -1e30;

    for (const rewrite::DwellingStorey &storey : plan.storeys)
    {
      for (const Point &p : storey.outer)
      {
        lowX = std::min(lowX, p.x);
        highX = std::max(highX, p.x);
        lowZ = std::min(lowZ, p.z);
        highZ = std::max(highZ, p.z);
      }
    }

    if (highX > lowX && highZ > lowZ)
    {
      for (uint32_t j = 0; j < 90; ++j)
      {
        const double z = lowZ + (highZ - lowZ) * (double(j) + .5) / 90.;

        for (uint32_t i = 0; i < 90; ++i)
        {
          const double x = lowX + (highX - lowX) * (double(i) + .5) / 90.;

          bool standing = false;

          for (const rewrite::DwellingStorey &storey : plan.storeys)
          {
            if (storey.outer.size() < 3 || !within(storey.outer, x, z))
            {
              continue;
            }

            bool open = false;

            for (const std::vector<Point> &hole : storey.holes)
            {
              open = open || (hole.size() >= 3 && within(hole, x, z));
            }

            standing = standing || !open;
          }

          if (standing)
          {
            continue;
          }

          ++out.outside;

          std::vector<double> over;
          roofHeights(plan.roofs, x, z, over);

          out.oversail += over.empty() ? 0 : 1;
        }
      }
    }
  }

  // --- a face steeper than a roof ---
  //
  // Every sloping face of a roof lies in the plane rising from one wall at the
  // building's pitch, so its gradient *is* that pitch. A face at three times it
  // is not a slope, it is a fin: the triangulation reaching across a step in
  // the surface and standing a sliver on edge between the two sides. That is
  // what a tear looks like once it has been given geometry, and it is the thing
  // you actually see - a spike through the ridge of a commerce house, a wedge
  // torn out of a temple.
  //
  // Sampling cannot find it. A fin is vertical, so it covers no ground, and
  // every question asked of the plan - is it covered, is it covered once -
  // passes straight over it. Edge counting sees the tear but not the sliver
  // bridging it, and reports the same small number whether the gap is left open
  // or filled with a blade.
  for (const rewrite::DwellingRoofFace &face : plan.roofs)
  {
    if (face.gable || face.polygon.size() < 3)
    {
      continue;
    }

    const Point &a = face.polygon[0];
    const Point &b = face.polygon[1];
    const Point &c = face.polygon[2];

    const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
    const double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;

    const double nx = uy * vz - uz * vy;
    const double ny = uz * vx - ux * vz;
    const double nz = ux * vy - uy * vx;

    const double flat = std::hypot(nx, nz);

    if (flat < 1e-9 && std::abs(ny) < 1e-9)
    {
      continue;
    }

    // Gradient of the plane: how far it rises per metre travelled along the
    // ground. A vertical face has no gradient at all, which is the worst case
    // there is.
    const double gradient = std::abs(ny) < 1e-9 ? 1e30 : flat / std::abs(ny);

    if (gradient > 1.1)
    {
      ++out.steep;
      out.worstSlope = std::max(out.worstSlope, std::min(gradient, 99.));
    }

    // And every corner of it in the same plane as the first three.
    //
    // A face is a polygon, not a triangle, and whoever draws it will fan it
    // from one corner. If its corners are not coplanar the fan is not flat
    // either: the triangles of it tilt against one another and the face reads
    // as a crease or, where one corner is far out of plane, as a blade
    // standing out of the roof. Nothing that asks questions of the first three
    // corners can see it.
    const double len = std::sqrt(nx * nx + ny * ny + nz * nz);

    if (len < 1e-12)
    {
      continue;
    }

    for (size_t k = 3; k < face.polygon.size(); ++k)
    {
      const Point &q = face.polygon[k];
      const double off =
        std::abs((q.x - a.x) * nx + (q.y - a.y) * ny + (q.z - a.z) * nz) / len;

      if (off > .01)
      {
        ++out.bent;
        out.worstBend = std::max(out.worstBend, off);
      }
    }
  }

  // --- a roof face standing inside a storey ---
  for (const rewrite::DwellingRoofFace &face : plan.roofs)
  {
    if (face.polygon.size() < 3)
    {
      continue;
    }

    double x = 0, y = 0, z = 0;

    for (const Point &p : face.polygon)
    {
      x += p.x / double(face.polygon.size());
      y += p.y / double(face.polygon.size());
      z += p.z / double(face.polygon.size());
    }

    for (const rewrite::DwellingStorey &storey : plan.storeys)
    {
      if (storey.outer.size() < 3 || y < storey.base + .15 || y > storey.top - .15)
      {
        continue;
      }

      bool deep = within(storey.outer, x, z);

      for (size_t w = 0; w < storey.outer.size() && deep; ++w)
      {
        const Point &a = storey.outer[w];
        const Point &b = storey.outer[(w + 1) % storey.outer.size()];
        const double dx = b.x - a.x, dz = b.z - a.z;
        const double run = dx * dx + dz * dz;

        if (run < 1e-9)
        {
          continue;
        }

        const double t = std::min(1., std::max(0., ((x - a.x) * dx + (z - a.z) * dz) / run));
        const double offX = x - (a.x + dx * t), offZ = z - (a.z + dz * t);
        deep = offX * offX + offZ * offZ > .0625;
      }

      out.buried += deep ? 1 : 0;
    }
  }

  // --- a partition with no building around it ---
  for (const rewrite::DwellingWall &wall : plan.walls)
  {
    if (wall.exterior)
    {
      continue;
    }

    const double x = (wall.a.x + wall.b.x) * .5;
    const double z = (wall.a.z + wall.b.z) * .5;
    const double y = wall.a.y;

    bool housed = false;

    for (const rewrite::DwellingStorey &storey : plan.storeys)
    {
      if (storey.outer.size() < 3 || std::abs(storey.base - y) >= .6)
      {
        continue;
      }

      // A wall that lies along the outline is on the boundary, where an
      // inside-outside test has no answer; being within the thickness of a
      // wall of the storey is what "in this building" means here.
      double reach = 1e30;

      for (size_t w = 0; w < storey.outer.size(); ++w)
      {
        const Point &a = storey.outer[w];
        const Point &b = storey.outer[(w + 1) % storey.outer.size()];
        const double ex = b.x - a.x, ez = b.z - a.z;
        const double span = ex * ex + ez * ez;
        const double t = span < 1e-9
                           ? 0.
                           : std::min(1., std::max(0., ((x - a.x) * ex + (z - a.z) * ez) / span));
        reach = std::min(reach, std::hypot(x - (a.x + ex * t), z - (a.z + ez * t)));
      }

      housed = housed || within(storey.outer, x, z) || reach < .35;
    }

    out.floating += housed ? 0 : 1;
    if (!housed && std::getenv("MESH_DEBUG"))
    {
      std::printf("      unhoused wall floor %u at (%.6f,%.6f,%.6f)\n", wall.floor, x, y, z);
    }
  }

  pairEdges(plan.roofs, plan.storeys, out);

  return out;
}

}  // namespace

int main()
{
  std::cout << "the mesh a blueprint turns into\n";

  ProgrammePlanSettings settings;
  ComplexDwellingSettings massing;

  FloorPlanSettings optimise;
  optimise.iterations = 30000;

  const auto ground = [](const double side, const double aspect) {
    const double halfX = side * std::sqrt(aspect) * .5;
    const double halfZ = side / std::sqrt(aspect) * .5;

    return std::vector<Point>{{-halfX, 0, -halfZ},
                              {halfX, 0, -halfZ},
                              {halfX, 0, halfZ},
                              {-halfX, 0, halfZ}};
  };

  struct Ask
  {
    BuildingClass klass;
    PlotStanding standing;
    double side, aspect;
  };

  const std::vector<Ask> asks{
    {BuildingClass::PeasantCottage, PlotStanding::Rural, 17, 2.1},
    {BuildingClass::Dwelling, PlotStanding::Modest, 19, 1.4},
    {BuildingClass::ShopHouse, PlotStanding::Modest, 18, 1.7},
    {BuildingClass::CommerceHouse, PlotStanding::Comfortable, 22, 1.5},
    {BuildingClass::Inn, PlotStanding::Comfortable, 24, 1.4},
    {BuildingClass::Farmstead, PlotStanding::Rural, 30, 1.6},
    {BuildingClass::Dormitory, PlotStanding::Modest, 26, 1.8},
    {BuildingClass::School, PlotStanding::Comfortable, 24, 1.5},
    {BuildingClass::Dispensary, PlotStanding::Comfortable, 21, 1.4},
    {BuildingClass::Warehouse, PlotStanding::Modest, 26, 1.6},
    {BuildingClass::GuildHall, PlotStanding::Wealthy, 28, 1.4},
    {BuildingClass::ManorHouse, PlotStanding::Wealthy, 34, 1.5},
    {BuildingClass::Mansion, PlotStanding::Grand, 38, 1.4},
    {BuildingClass::Church, PlotStanding::Comfortable, 30, 2.2},
    {BuildingClass::Chapel, PlotStanding::Modest, 20, 1.8},
    {BuildingClass::Temple, PlotStanding::Wealthy, 26, 1.6},
    {BuildingClass::Castle, PlotStanding::Grand, 40, 1.2},
    {BuildingClass::Prison, PlotStanding::Comfortable, 30, 1.3},
  };

  uint32_t built = 0, withBare = 0, withOpen = 0, withTee = 0, withBuried = 0;
  uint32_t withFloating = 0, withCoincident = 0, withStacked = 0, withSteep = 0, withBent = 0;
  uint32_t withLid = 0, withOversail = 0, withDoubled = 0;
  double worstStack = 0, worstSlope = 0, worstBend = 0;
  uint32_t withDegenerate = 0, withSpike = 0;
  double openLength = 0, worstPeak = 0;

  // Several seeds each, because a fault that shows in one plan and not the next
  // is still a fault; one building of each proves nothing.
  const std::vector<uint64_t> seeds{0xC1A55u, 0x5EED1u, 0x5EED2u};

  uint32_t expected = 0;
  for (const uint64_t seed : seeds)
  {
    const char *onlySeed = std::getenv("MESH_SEED");
    if (onlySeed != nullptr && seed != std::strtoull(onlySeed, nullptr, 0))
    {
      continue;
    }
    for (uint32_t i = 0; i < asks.size(); ++i)
    {
      const Ask &ask = asks[i];
      const char *only = std::getenv("MESH_ONLY");
      if (only != nullptr && std::string(only) != name(ask.klass))
      {
        continue;
      }
      ++expected;

      ProgrammeBrief brief;
      brief.building = ask.klass;
      brief.standing = ask.standing;

      ProgrammeBuilding building;
      rewrite::DwellingPlan plan;
      std::string error;

      if (!planProgrammeDwelling(brief, seed + i * 6151ull, settings, massing,
                                 ground(ask.side, ask.aspect), building, plan, &error,
                                 &optimise))
      {
        std::cerr << "  build failed for " << name(ask.klass) << ": " << error << "\n";
        continue;
      }

      ++built;

      if (only != nullptr)
      {
        std::printf("  %s seed %#llx\n", name(ask.klass), (unsigned long long)seed);

        for (const rewrite::DwellingStorey &storey : plan.storeys)
        {
          std::printf("    storey base %.2f top %.2f\n", storey.base, storey.top);
        }

        if (std::getenv("MESH_FACES") != nullptr)
        {
          for (size_t f = 0; f < plan.roofs.size(); ++f)
          {
            std::printf("    face %zu%s:", f, plan.roofs[f].gable ? " gable" : "");

            for (const rewrite::Position &v : plan.roofs[f].polygon)
            {
              std::printf(" (%.2f,%.2f,%.2f)", v.x, v.y, v.z);
            }

            std::printf("\n");
          }
        }
      }

      const Faults faults = inspect(plan);

      withBare += faults.bare > 0 ? 1 : 0;
      withOpen += faults.open > 0 ? 1 : 0;
      withTee += faults.tee > 0 ? 1 : 0;
      withCoincident += faults.coincident > 0 ? 1 : 0;
      withStacked += faults.stacked > 0 ? 1 : 0;
      withSteep += faults.steep > 0 ? 1 : 0;
      withBent += faults.bent > 0 ? 1 : 0;
      withLid += faults.lidded > 0 ? 1 : 0;
      withDoubled += faults.doubledFace > 0 ? 1 : 0;
      withOversail += faults.oversail > 0 ? 1 : 0;
      worstBend = std::max(worstBend, faults.worstBend);
      worstSlope = std::max(worstSlope, faults.worstSlope);
      worstStack = std::max(worstStack, faults.worstStack);
      withBuried += faults.buried > 0 ? 1 : 0;
      withFloating += faults.floating > 0 ? 1 : 0;
      withDegenerate += faults.degenerate > 0 ? 1 : 0;
      withSpike += faults.peakOverWall > faults.allowedRise ? 1 : 0;
      openLength += faults.openLength;
      worstPeak = std::max(worstPeak, faults.peakOverWall);

      if (faults.bare > 0 || faults.open > 0 || faults.tee > 0 || faults.coincident > 0 || faults.stacked > 0 || faults.steep > 0 || faults.bent > 0 || faults.lidded > 0 || faults.oversail > 0 || faults.doubledFace > 0
          || faults.buried > 0 || faults.floating > 0 || faults.degenerate > 0
          || faults.peakOverWall > faults.allowedRise)
      {
        std::printf("  %-16s seed %#llx:", name(ask.klass), (unsigned long long)seed);

        if (faults.bare > 0)
        {
          std::printf(" %.2f%% bare", 100. * double(faults.bare) / double(faults.sampled));
        }

        if (faults.open > 0)
        {
          std::printf(" %u open roof edges (%.1f m)", faults.open, faults.openLength);
        }

        if (faults.tee > 0)
        {
          std::printf(" %u edges meeting part-way", faults.tee);
        }

        if (faults.doubledFace > 0)
        {
          std::printf(" %u edges built more than twice", faults.doubledFace);
        }

        if (faults.oversail > 0)
        {
          std::printf(" roof over %.1f m2 of ground it does not stand on",
                      double(faults.oversail) * .0);
        }

        if (faults.lidded > 0)
        {
          std::printf(" %.0f%% of its courts roofed over",
                      100. * double(faults.lidded) / double(std::max(faults.court, 1u)));
        }

        if (faults.bent > 0)
        {
          std::printf(" %u faces out of plane (worst %.2f m)", faults.bent, faults.worstBend);
        }

        if (faults.steep > 0)
        {
          std::printf(" %u fins (steepest %.1f per metre)", faults.steep, faults.worstSlope);
        }

        if (faults.stacked > 0)
        {
          std::printf(" %.1f%% of ground under stacked roof (worst %.2f m apart)",
                      100. * double(faults.stacked) / double(faults.sampled),
                      faults.worstStack);
        }

        if (faults.coincident > 0)
        {
          std::printf(" %u samples under two roofs at once", faults.coincident);
        }

        if (faults.buried > 0)
        {
          std::printf(" %u roof faces inside a storey", faults.buried);
        }

        if (faults.floating > 0)
        {
          std::printf(" %u partitions with no storey", faults.floating);
        }

        if (faults.degenerate > 0)
        {
          std::printf(" %u degenerate faces", faults.degenerate);
        }

        if (faults.peakOverWall > faults.allowedRise)
        {
          std::printf(" roof %.2f m above the wall head, %.2f m allowed",
                      faults.peakOverWall, faults.allowedRise);
        }

        std::printf("\n");
      }
    }
  }

  std::printf("\n  %u buildings inspected; %.1f m of open roof edge, tallest roof %.2f m"
              " over its wall head\n",
              built, openLength, worstPeak);

  check(built == expected && expected > 0, "every building is built");
  check(withDegenerate == 0, "no face is degenerate");
  check(withBare == 0, "no ground that needs a roof is without one");
  check(withBuried == 0, "no roof face stands inside a storey");
  check(withFloating == 0, "no partition stands outside its storey");
  std::printf("  worst stack %.2f m\n", worstStack);

  std::printf("  steepest face %.1f per metre\n", worstSlope);

  std::printf("  worst corner out of its face's plane %.2f m\n", worstBend);

  check(withDoubled == 0, "no face is built twice");
  check(withLid == 0, "no court has a roof over it");
  check(withOversail == 0, "no roof stands over ground the building has not got");
  check(withBent == 0, "every face is flat");
  check(withSteep == 0, "no face is steeper than a roof");
  check(withCoincident == 0, "no ground carries two roofs at once");
  check(withStacked == 0, "no ground carries one roof above another");
  check(withTee == 0, "no roof edge meets another part-way along");
  check(withOpen == 0, "the roof is closed everywhere but the eaves");
  check(withSpike == 0, "no roof rises absurdly above its walls");

  std::cout << (failures == 0 ? "\nall passed\n" : "\nFAILURES\n");

  return failures == 0 ? 0 : 1;
}
