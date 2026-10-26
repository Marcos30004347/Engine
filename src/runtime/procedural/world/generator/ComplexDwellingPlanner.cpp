#include "ComplexDwellingPlanner.hpp"

#include "procedural/geom2/Boolean.hpp"
#include "procedural/geom2/Offset.hpp"
#include "procedural/geom2/Partition.hpp"
#include "procedural/geom2/Polygon.hpp"
#include "procedural/geom2/Skeleton.hpp"
#include "procedural/geom2/Quantise.hpp"
#include "procedural/geom3/Prism.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <functional>

namespace procedural::world::generator {

using rewrite::DwellingBrief;
using rewrite::DwellingDoor;
using rewrite::DwellingFloor;
using rewrite::DwellingPlan;
using rewrite::DwellingRoom;
using rewrite::DwellingStair;
using rewrite::DwellingWall;
using rewrite::FurnitureKind;
using rewrite::Position;
using rewrite::RoomUse;

namespace {

constexpr double kPi = 3.14159265358979323846;

bool fail(std::string *error, const std::string &why)
{
  if (error != nullptr)
  {
    *error = why;
  }

  return false;
}

// A deterministic draw in [0, 1) addressed by purpose rather than by call
// order, so a value does not move when an unrelated loop is reordered.
double draw(const uint64_t seed, const uint64_t a, const uint64_t b, const uint64_t c)
{
  const auto mix = [](uint64_t value) {
    value ^= value >> 33u;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33u;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33u;
    return value;
  };

  uint64_t value = mix(seed ^ a * 0x9e3779b97f4a7c15ull);
  value = mix(value ^ b * 0xbf58476d1ce4e5b9ull);
  value = mix(value ^ c * 0x94d049bb133111ebull);

  return static_cast<double>(value >> 11u) / 9007199254740992.0;
}

geom2::Point at(const double x, const double y)
{
  return geom2::Point{geom2::toUnit(x), geom2::toUnit(y)};
}

// The footprint's own axes: its longest edge, and the perpendicular. Every cut
// runs along one of the two, so every wall is parallel or square to every other
// wall however odd the outline is.
void axesOf(const geom2::Ring &ring, double &ux, double &uy)
{
  ux = 1;
  uy = 0;

  double longest = -1;

  for (size_t i = 0; i < ring.size(); ++i)
  {
    const double ax = geom2::toMetres(ring[i].x), ay = geom2::toMetres(ring[i].y);
    const double bx = geom2::toMetres(ring[(i + 1) % ring.size()].x), by = geom2::toMetres(ring[(i + 1) % ring.size()].y);
    const double length = std::hypot(bx - ax, by - ay);

    if (length > longest)
    {
      longest = length;
      ux = (bx - ax) / length;
      uy = (by - ay) / length;
    }
  }
}

// The extent of a polygon on a pair of axes, and how square that makes it.
void extentOn(const geom2::Polygon &poly, const double ux, const double uy, double &spanU, double &spanV)
{
  double loU = 1e300, hiU = -1e300, loV = 1e300, hiV = -1e300;

  for (const geom2::Point &p : poly.outer)
  {
    const double x = geom2::toMetres(p.x), y = geom2::toMetres(p.y);
    const double u = x * ux + y * uy, v = -x * uy + y * ux;

    loU = std::min(loU, u);
    hiU = std::max(hiU, u);
    loV = std::min(loV, v);
    hiV = std::max(hiV, v);
  }

  spanU = hiU - loU;
  spanV = hiV - loV;
}

// Where an infinite line through (px, py) along (nx, ny) enters and leaves a
// ring, with a whisker past each end. False when it misses.
bool chordThrough(const geom2::Ring &ring, const double px, const double py, const double nx, const double ny, geom2::Point &from, geom2::Point &to)
{
  double entry = 0, exit = 0;
  int crossings = 0;

  for (size_t i = 0; i < ring.size(); ++i)
  {
    const double ex0 = geom2::toMetres(ring[i].x), ey0 = geom2::toMetres(ring[i].y);
    const double sx = geom2::toMetres(ring[(i + 1) % ring.size()].x) - ex0;
    const double sy = geom2::toMetres(ring[(i + 1) % ring.size()].y) - ey0;
    const double denominator = nx * sy - ny * sx;

    if (std::abs(denominator) < 1e-12)
    {
      continue;
    }

    const double u = ((ex0 - px) * ny - (ey0 - py) * nx) / denominator;

    if (u < 0 || u > 1)
    {
      continue;
    }

    const double t = ((ex0 - px) + sx * u) * nx + ((ey0 - py) + sy * u) * ny;

    entry = crossings == 0 ? t : std::min(entry, t);
    exit = crossings == 0 ? t : std::max(exit, t);
    ++crossings;
  }

  if (crossings < 2 || exit - entry < 1e-6)
  {
    return false;
  }

  from = at(px + nx * (entry - .05), py + ny * (entry - .05));
  to = at(px + nx * (exit + .05), py + ny * (exit + .05));

  return true;
}

// Two rings sharing a straight run of at least `minimum` metres, and where it
// lies. This is what a door needs: a stretch of wall two rooms both own.
bool sharedRun(const geom2::Ring &a, const geom2::Ring &b, const double minimum, Position &centre, double &run)
{
  bool found = false;
  run = 0;

  for (size_t i = 0; i < a.size(); ++i)
  {
    const double px = geom2::toMetres(a[i].x), py = geom2::toMetres(a[i].y);
    const double qx = geom2::toMetres(a[(i + 1) % a.size()].x), qy = geom2::toMetres(a[(i + 1) % a.size()].y);
    const double ex = qx - px, ey = qy - py;
    const double length = std::hypot(ex, ey);

    if (length < minimum)
    {
      continue;
    }

    for (size_t j = 0; j < b.size(); ++j)
    {
      const double ax = geom2::toMetres(b[j].x), ay = geom2::toMetres(b[j].y);
      const double bx = geom2::toMetres(b[(j + 1) % b.size()].x), by = geom2::toMetres(b[(j + 1) % b.size()].y);
      const double sx = bx - ax, sy = by - ay;
      const double slength = std::hypot(sx, sy);

      if (slength < minimum)
      {
        continue;
      }

      // Collinear within a wall's thickness, and overlapping along the line.
      if (std::abs(sx * (py - ay) - sy * (px - ax)) / slength > .25 || std::abs(sx * (qy - ay) - sy * (qx - ax)) / slength > .25)
      {
        continue;
      }

      const double t0 = ((px - ax) * sx + (py - ay) * sy) / slength;
      const double t1 = ((qx - ax) * sx + (qy - ay) * sy) / slength;
      const double lo = std::max(0.0, std::min(t0, t1)), hi = std::min(slength, std::max(t0, t1));

      if (hi - lo < minimum || hi - lo <= run)
      {
        continue;
      }

      const double mid = (lo + hi) * .5;

      run = hi - lo;
      centre = Position{ax + sx / slength * mid, 0, ay + sy / slength * mid};
      found = true;
    }
  }

  return found;
}

// Furniture, as rectangles against a wall, by purpose. The same vocabulary the
// rectangular planner uses, placed in the room's own bounding box because a
// room here is not a Rect.
void furnish(const geom2::Polygon &room, const uint64_t id, const uint32_t floor, const RoomUse use, const double y, const double ux, const double uy, const uint64_t seed, std::vector<rewrite::DwellingFurniture> &out)
{
  double loU = 1e300, hiU = -1e300, loV = 1e300, hiV = -1e300;

  for (const geom2::Point &p : room.outer)
  {
    const double x = geom2::toMetres(p.x), z = geom2::toMetres(p.y);
    const double u = x * ux + z * uy, v = -x * uy + z * ux;

    loU = std::min(loU, u);
    hiU = std::max(hiU, u);
    loV = std::min(loV, v);
    hiV = std::max(hiV, v);
  }

  const double width = hiU - loU, depth = hiV - loV;

  if (width < 1.3 || depth < 1.3)
  {
    return;
  }

  const auto place = [&](const FurnitureKind kind, const double w, const double d, const int wall, const double along) {
    if (w > width - .3 || d > depth - .3)
    {
      return;
    }

    const double u0 = wall == 2 ? loU + .15 : wall == 3 ? hiU - w - .15 : loU + .15 + (width - w - .3) * along;
    const double v0 = wall == 0 ? hiV - d - .15 : wall == 1 ? loV + .15 : loV + .15 + (depth - d - .3) * along;

    const auto toWorld = [&](const double u, const double v) {
      return Position{u * ux - v * uy, y, u * uy + v * ux};
    };

    rewrite::DwellingFurniture piece;
    piece.room = id;
    piece.floor = floor;
    piece.kind = kind;
    piece.footprint = {toWorld(u0, v0), toWorld(u0 + w, v0), toWorld(u0 + w, v0 + d), toWorld(u0, v0 + d)};

    // Only if it actually lands in the room: a bounding box is not the room,
    // and on an L-shaped one the corner of the box is outside the walls.
    for (const Position &corner : piece.footprint)
    {
      if (!geom2::contains(room, at(corner.x, corner.z)))
      {
        return;
      }
    }

    out.push_back(std::move(piece));
  };

  switch (use)
  {
  case RoomUse::Sleeping:
    place(FurnitureKind::Bed, 1.0, 2.0, 0, .15);

    if (width >= 3.4)
    {
      place(FurnitureKind::Bed, 1.0, 2.0, 0, .85);
    }

    place(FurnitureKind::Chest, .9, .5, 1, draw(seed, id, 2, 0));
    break;

  case RoomUse::Living:
    place(FurnitureKind::Hearth, 1.4, .5, 0, .5);
    place(FurnitureKind::Table, std::min(1.8, width * .45), 1.0, 1, .5);
    break;

  case RoomUse::Shop:
    place(FurnitureKind::Counter, std::min(width * .7, 3.0), .6, 1, .5);
    place(FurnitureKind::Chest, .9, .6, 0, draw(seed, id, 3, 0));
    break;

  case RoomUse::Store:
    place(FurnitureKind::Chest, .9, 1.0, 2, .2);
    place(FurnitureKind::Chest, .9, 1.0, 2, .7);
    break;

  case RoomUse::Common:
    place(FurnitureKind::Table, std::min(width * .55, 2.4), 1.0, 0, .5);
    place(FurnitureKind::Bench, .5, std::min(depth * .5, 2.0), 2, .5);
    break;

  case RoomUse::Service:
    place(FurnitureKind::Workbench, std::min(width * .6, 2.2), .7, 0, .5);
    break;

  case RoomUse::Hall:
    place(FurnitureKind::Table, std::min(width * .5, 2.0), .9, 0, .5);
    break;

  case RoomUse::Corridor:
    // Nothing: a corridor you have to walk around is not a corridor.
    break;
  }
}

} // namespace

// Which wings are roofed as one mass.
//
// "Wings are grouped into 'clusters' - sets of wings whose roofs can be merged
// organically." Two conditions, and both matter: the wings have to be the same
// height, because a roof cannot merge with one at another level, and they have
// to touch, because roofs that merge are roofs that meet. Equal height alone -
// which is what this used to say - puts the two single-storey ends of a manor
// in one cluster with the cross wing between them at a different height, so a
// roofer reads one mass spanning a building it is not continuous across.
//
// So: connected components over "same storey count and sharing a wall".
static void groupWingsIntoClusters(std::vector<rewrite::DwellingWing> &wings, const double wallThickness)
{
  const size_t count = wings.size();

  std::vector<size_t> parent(count);

  for (size_t i = 0; i < count; ++i)
  {
    parent[i] = i;
  }

  const std::function<size_t(size_t)> find = [&](size_t i) {
    while (parent[i] != i)
    {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }

    return i;
  };

  // Wings are cut from one plate and butt against each other, so "touching" is
  // a pair of boundary points close enough to be the same wall. A tolerance of
  // a wall's thickness is what the cut itself can leave between them.
  const double tolerance = std::max(wallThickness, .05) * 2;

  for (size_t i = 0; i < count; ++i)
  {
    for (size_t j = i + 1; j < count; ++j)
    {
      if (wings[i].storeys != wings[j].storeys || find(i) == find(j))
      {
        continue;
      }

      // Touching means walls that meet, not corners that coincide.
      //
      // Comparing vertex to vertex was the mistake, and it is wrong exactly
      // where buildings actually join: at a T, the stem's end wall lands in
      // the middle of the bar's side and the two share no vertex at all. Those
      // wings were judged strangers, given a cluster each and roofed
      // separately, so a three-wing house came out as three buildings with
      // gutters between them. Point against segment, both ways round.
      const auto nearRing = [&](const std::vector<Position> &ring, const Position &p) {
        for (size_t k = 0; k < ring.size(); ++k)
        {
          const Position &u = ring[k];
          const Position &v = ring[(k + 1) % ring.size()];
          const double ex = v.x - u.x, ez = v.z - u.z;
          const double length2 = ex * ex + ez * ez;
          const double t = length2 > 1e-12 ? std::min(1.0, std::max(0.0, ((p.x - u.x) * ex + (p.z - u.z) * ez) / length2)) : 0;

          if (std::hypot(u.x + ex * t - p.x, u.z + ez * t - p.z) <= tolerance)
          {
            return true;
          }
        }

        return false;
      };

      bool adjacent = false;

      for (const Position &a : wings[i].footprint)
      {
        adjacent = adjacent || nearRing(wings[j].footprint, a);
      }

      for (const Position &b : wings[j].footprint)
      {
        adjacent = adjacent || nearRing(wings[i].footprint, b);
      }

      if (adjacent)
      {
        parent[find(i)] = find(j);
      }
    }
  }

  std::map<size_t, uint32_t> labels;

  for (size_t i = 0; i < count; ++i)
  {
    const size_t root = find(i);
    const auto it = labels.find(root);

    wings[i].cluster = it != labels.end() ? it->second : (labels[root] = uint32_t(labels.size()));
  }
}

// The roof over every wing.
//
// A wing carried a ridge and nothing else, so the exterior existed as a line
// drawing. This builds the surfaces.
//
// The construction is the one a roof actually has. Take the wing's ground,
// pushed out by the overhang so the roof oversails its walls, and walk its
// edges at eaves height. Project each edge's two ends onto the ridge, clamped
// to the ridge's own extent, and the face is the quad between the edge and
// those two ridge points. On a rectangle that is exactly two trapezoids down
// the long sides and two triangles at the ends, which is a hip roof; and when
// the two projections coincide the quad collapses to the triangle by itself,
// so there is no special case to write.
//
// A gable falls out of the same loop. Push the ridge end all the way out to
// the eaves edge and that end's face becomes vertical - the end wall carried
// up to the ridge, which is what a gable is. So hip and gable differ by where
// the ridge stops, not by which code runs.
//
// Which end gets which: an end that meets another wing is always gabled,
// because the two roofs have to run into each other rather than each growing a
// hip into the junction - that is what "roofs merged organically" has to mean
// geometrically. A free end is gabled on a narrow wing and hipped on a broad
// one, which is the ordinary distinction between a cottage and a hall.
static bool rayHitsRing(const std::vector<std::pair<double, double>> &ring, const double px, const double py, const double dx, const double dy, double &distance)
{
  double best = -1;

  for (size_t i = 0; i < ring.size(); ++i)
  {
    const auto &a = ring[i], &b = ring[(i + 1) % ring.size()];
    const double ex = b.first - a.first, ey = b.second - a.second;
    const double denominator = dx * ey - dy * ex;

    if (std::abs(denominator) < 1e-12)
    {
      continue;
    }

    const double t = ((a.first - px) * ey - (a.second - py) * ex) / denominator;
    const double u = ((a.first - px) * dy - (a.second - py) * dx) / denominator;

    if (t > 1e-9 && u >= -1e-9 && u <= 1 + 1e-9 && (best < 0 || t < best))
    {
      best = t;
    }
  }

  distance = best;

  return best > 0;
}

// The roof over a building, one surface per cluster of wings.
//
// It used to be one roof per wing, and that is what the renders kept showing
// as wrong: two wings that meet grew two independent roofs, each hipped and
// gabled as if the other were not there, and where they overlapped the result
// was a pair of V-shaped surfaces cutting through one another. No amount of
// trimming the eaves fixes that, because the fault is that there are two
// surfaces at all.
//
// "Wings are grouped into 'clusters' - sets of wings whose roofs can be merged
// organically." A cluster is one mass, so it gets one roof: the union of its
// wings' ground is taken first, and the straight skeleton of *that* outline is
// the roof. The ridge network then comes out of the plan rather than being
// assembled from per-wing pieces - an L gives two ridges meeting at a valley,
// a T gives three, and none of them can overlap because there is only ever one
// surface.
//
// Clusters at different heights still meet the ordinary way: the lower one's
// eaves stop against the taller one's wall.
static void buildRoofs(std::vector<rewrite::DwellingWing> &wings, const double overhang, const double hipFromWidth, std::vector<rewrite::DwellingRoofFace> &out, const RoofStyle style = RoofStyle::CrossGabled)
{
  if (wings.empty())
  {
    return;
  }

  // The wings of each cluster, and the ground each cluster covers.
  std::map<uint32_t, std::vector<size_t>> clusters;

  for (size_t i = 0; i < wings.size(); ++i)
  {
    if (wings[i].footprint.size() >= 3)
    {
      clusters[wings[i].cluster].push_back(i);
    }
  }

  const auto groundOf = [&](const std::vector<size_t> &members, geom2::Polygon &ground) {
    std::vector<geom2::Operand> operands;

    for (const size_t i : members)
    {
      geom2::Polygon piece;

      for (const Position &q : wings[i].footprint)
      {
        piece.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
      }

      if (geom2::polygonFault(piece).empty())
      {
        operands.push_back(geom2::toOperand(piece));
      }
    }

    if (operands.empty())
    {
      return false;
    }

    std::vector<geom2::Polygon> merged;

    if (!geom2::unionOf(operands, merged, nullptr) || merged.empty())
    {
      return false;
    }

    size_t largest = 0;

    for (size_t i = 1; i < merged.size(); ++i)
    {
      largest = geom2::areaMetres(merged[i]) > geom2::areaMetres(merged[largest]) ? i : largest;
    }

    ground = merged[largest];

    return geom2::areaMetres(ground) > .5;
  };

  std::map<uint32_t, geom2::Polygon> grounds;

  for (const auto &[id, members] : clusters)
  {
    geom2::Polygon ground;

    if (groundOf(members, ground))
    {
      grounds[id] = std::move(ground);
    }
  }

  for (const auto &[id, members] : clusters)
  {
    const auto found = grounds.find(id);

    if (found == grounds.end())
    {
      continue;
    }

    const geom2::Polygon &ground = found->second;
    const rewrite::DwellingWing &lead = wings[members.front()];

    // ---- the eaves ------------------------------------------------------
    //
    // The cluster's ground pushed out by the overhang, cut back off the other
    // clusters grown by the same overhang - so the roof oversails the outside
    // walls and stops flush where it meets a taller mass - and then unioned
    // with its own ground again, or the trim would leave a slot between the
    // roof and the wall it covers.
    const geom2::Polygon *eavesPoly = &ground;
    std::vector<geom2::Polygon> grown, held;

    // ---- the eaves: overhang where it is free, flush where it is not ------
    //
    // This used to grow the mass, subtract the neighbours grown by the same
    // amount, and union the mass back. Every one of those steps is a boolean,
    // and what came out was a boolean's shape: a range with four walls
    // arrived at the roof with eight edges, the extra four being ledges
    // exactly one overhang deep where the subtraction had cut a strip off and
    // the union had put it back. Those ledges are what stalls the wavefront,
    // so two of the plainest rectangles in a steading were roofed by the
    // sampled path with a hundred faces between them.
    //
    // A party wall does not need an overhang - the roof rises to it - so the
    // answer is not to subtract anything but to *not grow that edge*. Grow the
    // whole mass, then cut the result back on the line of each wall it shares.
    // A rectangle keeps four edges however many neighbours it has.
    if (overhang > 0 && geom2::offsetPolygon(ground, overhang, {}, grown, nullptr) && !grown.empty())
    {
      size_t largest = 0;

      for (size_t i = 1; i < grown.size(); ++i)
      {
        largest = geom2::areaMetres(grown[i]) > geom2::areaMetres(grown[largest]) ? i : largest;
      }

      geom2::Polygon shaped = grown[largest];

      for (size_t i = 0; i < ground.outer.size(); ++i)
      {
        const double ax = geom2::toMetres(ground.outer[i].x), az = geom2::toMetres(ground.outer[i].y);
        const double bx = geom2::toMetres(ground.outer[(i + 1) % ground.outer.size()].x), bz = geom2::toMetres(ground.outer[(i + 1) % ground.outer.size()].y);
        const double length = std::hypot(bx - ax, bz - az);

        if (length < .5)
        {
          continue;
        }

        const double ux = (bx - ax) / length, uz = (bz - az) / length;
        const double nx = -uz, nz = ux; // Inward, for a counter clockwise ring.

        // Does this wall stand against another mass?
        bool shared = false;

        for (const auto &[other, polygon] : grounds)
        {
          if (other == id)
          {
            continue;
          }

          for (double t : {.25, .5, .75})
          {
            const double px = ax + ux * length * t, pz = az + uz * length * t;

            for (size_t k = 0; k < polygon.outer.size() && !shared; ++k)
            {
              const double cx = geom2::toMetres(polygon.outer[k].x), cz = geom2::toMetres(polygon.outer[k].y);
              const double dx = geom2::toMetres(polygon.outer[(k + 1) % polygon.outer.size()].x), dz = geom2::toMetres(polygon.outer[(k + 1) % polygon.outer.size()].y);
              const double ex = dx - cx, ez = dz - cz;
              const double len2 = ex * ex + ez * ez;
              const double at = len2 > 1e-12 ? std::min(1.0, std::max(0.0, ((px - cx) * ex + (pz - cz) * ez) / len2)) : 0;

              shared = std::hypot(cx + ex * at - px, cz + ez * at - pz) < .2;
            }
          }
        }

        if (!shared)
        {
          continue;
        }

        const double reach = std::sqrt(std::max(1.0, geom2::areaMetres(ground))) * 4;
        std::vector<geom2::Polygon> parts;

        if (!geom2::cutPolygon(shaped, geom2::Point{geom2::toUnit(ax - ux * reach), geom2::toUnit(az - uz * reach)}, geom2::Point{geom2::toUnit(bx + ux * reach), geom2::toUnit(bz + uz * reach)}, parts, nullptr) || parts.size() < 2)
        {
          continue;
        }

        const geom2::Polygon *inner = nullptr;

        for (const geom2::Polygon &part : parts)
        {
          geom2::Point middle;

          if (!geom2::centroid(part, middle))
          {
            continue;
          }

          if ((geom2::toMetres(middle.x) - ax) * nx + (geom2::toMetres(middle.y) - az) * nz > 0 && (inner == nullptr || geom2::areaMetres(part) > geom2::areaMetres(*inner)))
          {
            inner = &part;
          }
        }

        if (inner != nullptr && geom2::areaMetres(*inner) > geom2::areaMetres(ground) * .8)
        {
          shaped = *inner;
        }
      }

      held.push_back(std::move(shaped));
      eavesPoly = &held.back();
    }

    // ---- clean the ring --------------------------------------------------
    //
    // Every edge becomes a roof face, and this ring has been through an
    // offset, a difference and two unions. A vertex that neither moves the
    // outline by a few centimetres nor turns it by a degree is a leftover of
    // how it was computed, not a corner of the roof, and left in it produces
    // slivers.
    // How small a jog counts as noise.
    //
    // It was four centimetres, which is the right scale for quantisation and
    // the wrong one for this ring. The eaves polygon is the union of the wings
    // grown by the overhang and trimmed against their neighbours, and a
    // boolean on a quantised grid leaves that outline scalloped: the worst mass
    // in a town arrived at the roof with seventy-six walls, and the roof over
    // it was a faithful skeleton of seventy-six walls. Nothing was wrong with
    // the skeleton. A building does not have a four-centimetre wall.
    //
    // Fifteen centimetres, and by deviation rather than by length. Dropping
    // short edges alone cannot tell a trim notch from a mitre - it cut the
    // corners off a trapezoid when that was tried - but a vertex that moves
    // the outline by less than this is noise whatever its edges measure, and a
    // mitred corner moves it by far more. The bar stays under the overhang, so
    // the roof still oversails every wall it covers.
    const double simplifyTo = .15;
    std::vector<std::pair<double, double>> ring;

    for (const geom2::Point &q : eavesPoly->outer)
    {
      ring.push_back({geom2::toMetres(q.x), geom2::toMetres(q.y)});
    }

    // The vertex that matters least, until none of them is beneath the bar.
    //
    // Deviation from the chord its two neighbours make, which is what "this
    // corner is not a corner" actually means: a vertex sitting on a straight
    // run scores zero however long its edges are, and the tip of a mitre
    // scores the whole of its own depth however short they are.
    for (bool dropped = true; dropped && ring.size() > 4;)
    {
      dropped = false;

      size_t worst = ring.size();
      double least = simplifyTo;

      for (size_t i = 0; i < ring.size(); ++i)
      {
        const auto &prev = ring[(i + ring.size() - 1) % ring.size()];
        const auto &here = ring[i];
        const auto &next = ring[(i + 1) % ring.size()];

        const double ex = next.first - prev.first, ez = next.second - prev.second;
        const double span = std::hypot(ex, ez);
        const double off = span > 1e-9 ? std::abs((here.first - prev.first) * ez - (here.second - prev.second) * ex) / span : std::hypot(here.first - prev.first, here.second - prev.second);

        if (off < least)
        {
          least = off;
          worst = i;
        }
      }

      if (worst < ring.size())
      {
        ring.erase(ring.begin() + long(worst));
        dropped = true;
      }
    }

    for (bool dropped = true; dropped && ring.size() > 3;)
    {
      dropped = false;

      for (size_t i = 0; i < ring.size() && ring.size() > 3; ++i)
      {
        const auto &prev = ring[(i + ring.size() - 1) % ring.size()];
        const auto &here = ring[i];
        const auto &next = ring[(i + 1) % ring.size()];

        const double ax = here.first - prev.first, az = here.second - prev.second;
        const double bx = next.first - here.first, bz = next.second - here.second;
        const double la = std::hypot(ax, az), lb = std::hypot(bx, bz);

        if (la < 1e-9 || lb < 1e-9 || std::abs(ax * bz - az * bx) / (la * lb) < .017)
        {
          ring.erase(ring.begin() + long(i));
          dropped = true;

          break;
        }
      }
    }

    if (ring.size() < 3)
    {
      continue;
    }

    // ---- the cluster's own axis and height -------------------------------
    double ux = 1, uz = 0, longest = -1;

    for (size_t i = 0; i < ring.size(); ++i)
    {
      const auto &a = ring[i], &b = ring[(i + 1) % ring.size()];
      const double length = std::hypot(b.first - a.first, b.second - a.second);

      if (length > longest)
      {
        longest = length;
        ux = (b.first - a.first) / length;
        uz = (b.second - a.second) / length;
      }
    }

    double across = 0, along = 0;
    {
      double lo = 1e300, hi = -1e300, lowU = 1e300, highU = -1e300;

      for (const auto &q : ring)
      {
        const double t = -q.first * uz + q.second * ux;
        lo = std::min(lo, t);
        hi = std::max(hi, t);

        const double u = q.first * ux + q.second * uz;
        lowU = std::min(lowU, u);
        highU = std::max(highU, u);
      }

      across = hi - lo;
      along = highU - lowU;
    }

    // Hip or gable, from the shape of the mass rather than its size alone.
    //
    // "Narrower than a hall" was the whole test, and on a cluster - which is
    // the union of every wing in the mass, not one wing - almost nothing is
    // narrow, so a street of terraces came out as a field of pyramids. A long
    // range is gabled at its ends whatever its width, because that is what a
    // range is; a squat mass is hipped. A mass that also meets another keeps
    // its gable there, since two roofs have to run into each other.
    const bool elongated = across > 1e-6 && along / across > 1.5;

    // A roof rises about a third of the narrow way across the mass it covers:
    // steep enough to shed, low enough that a long range is not a spire. The
    // whole cluster shares it, which is what makes the surface continuous.
    const double rise = std::max(1.2, across * .34);
    const double eaves = lead.eavesHeight;

    for (const size_t i : members)
    {
      wings[i].ridgeHeight = eaves + rise;
    }

    // ---- gables ----------------------------------------------------------
    //
    // The two ends of the mass: the edges square to its long axis, nearest
    // either end of it. Only two, because a weight-zero edge never collapses
    // and a wavefront with several of them stalls before the outline closes.
    geom2::RoofOptions roofing;
    roofing.ridgeHeight = rise;
    roofing.gables.assign(ring.size(), false);

    const auto meetsAnother = [&](const double px, const double pz) {
      for (const auto &[other, polygon] : grounds)
      {
        if (other == id)
        {
          continue;
        }

        for (const geom2::Point &q : polygon.outer)
        {
          if (std::hypot(geom2::toMetres(q.x) - px, geom2::toMetres(q.y) - pz) < across * .5 + overhang + 1.0)
          {
            return true;
          }
        }
      }

      return false;
    };

    {
      double centreU = 0;

      for (const auto &q : ring)
      {
        centreU += q.first * ux + q.second * uz;
      }

      centreU /= double(ring.size());

      size_t lowEnd = ring.size(), highEnd = ring.size();
      double lowBest = 0, highBest = 0;

      for (size_t i = 0; i < ring.size(); ++i)
      {
        const auto &a = ring[i], &b = ring[(i + 1) % ring.size()];
        const double ex = b.first - a.first, ez = b.second - a.second;
        const double elen = std::hypot(ex, ez);

        if (elen < 1e-6 || std::abs((ex / elen) * ux + (ez / elen) * uz) >= .5)
        {
          continue;
        }

        const double mx = (a.first + b.first) * .5, mz = (a.second + b.second) * .5;

        if (!meetsAnother(mx, mz) && !elongated && across >= hipFromWidth)
        {
          continue;
        }

        const double t = mx * ux + mz * uz - centreU;

        if (t < 0 && -t > lowBest)
        {
          lowBest = -t;
          lowEnd = i;
        }
        else if (t > 0 && t > highBest)
        {
          highBest = t;
          highEnd = i;
        }
      }

      // A gable is a slow edge, not a stationary one.
      //
      // Weight zero is the exact gable - the edge never moves, so the surface
      // over it is vertical - and it is also an edge that never collapses, so
      // the wavefront can stall on it and never close the outline. Every
      // terrace in the showcase failed its coverage check that way and fell
      // back to a plain centre-fan hip, which is why a street of ranges came
      // out as a field of pyramids. A small positive weight always terminates
      // and leaves a face steep enough to read as a gable - a clipped gable,
      // which is a real roof and an honest one.
      // Every edge that stands against another mass, and the two ends of an
      // elongated one.
      //
      // An edge against a party wall is not an eaves. Hipped down to it, the
      // building gets a gutter along that wall with nowhere to fall, and the
      // rain runs into the plan rather than off it - which is what the last
      // renders showed. The roof has to rise to that edge and stop.
      const auto near = [&](const double px, const double pz, const double tolerance) {
        for (const auto &[other, polygon] : grounds)
        {
          if (other == id)
          {
            continue;
          }

          for (size_t k = 0; k < polygon.outer.size(); ++k)
          {
            const double ax = geom2::toMetres(polygon.outer[k].x), az = geom2::toMetres(polygon.outer[k].y);
            const double bx = geom2::toMetres(polygon.outer[(k + 1) % polygon.outer.size()].x), bz = geom2::toMetres(polygon.outer[(k + 1) % polygon.outer.size()].y);
            const double ex = bx - ax, ez = bz - az;
            const double length2 = ex * ex + ez * ez;
            const double t = length2 > 1e-12 ? std::min(1.0, std::max(0.0, ((px - ax) * ex + (pz - az) * ez) / length2)) : 0;

            if (std::hypot(ax + ex * t - px, az + ez * t - pz) < tolerance)
            {
              return true;
            }
          }
        }

        return false;
      };

      // A party wall is a wall shared along its length, not a wall with a
      // neighbour somewhere near it.
      //
      // Asking whether the midpoint came within an overhang and a bit of any
      // other mass said yes for almost every edge of every building that had
      // more than one: a mass of four sides arrived at the roof with three of
      // them gabled, which is a lean-to, and some with four, which is not a
      // roof at all but a box with a lid raised on stilts. Sampled along the
      // edge instead, and at a tolerance tighter than the overhang, so what it
      // answers is what it is named for.
      const auto abuts = [&](const size_t index) {
        const auto &a = ring[index], &b = ring[(index + 1) % ring.size()];
        const double ex = b.first - a.first, ez = b.second - a.second;

        if (std::hypot(ex, ez) < 1e-6)
        {
          return false;
        }

        int shared = 0;

        for (int k = 0; k < 5; ++k)
        {
          const double t = .1 + .2 * double(k);

          shared += near(a.first + ex * t, a.second + ez * t, .3) ? 1 : 0;
        }

        return shared >= 3;
      };

      // Which wing each edge of the ring belongs to.
      //
      // A cluster is a body with wings pushed off it, and the union that makes
      // the ring forgets which was which. It matters: the end of a wing is a
      // gable whatever the style, because a wing is a range and a range ends
      // in a wall, and it is the one the last renders got wrong - every
      // extrusion came out as a little hipped stub sat on the side of the
      // house. So the ring is put back against the wings it came from, and
      // each edge is asked whether it runs square to *its own* wing's axis
      // rather than to the whole mass's.
      const auto centreOf = [&](const size_t wing, double &cx, double &cz) {
        const auto &foot = wings[wing].footprint;
        cx = 0;
        cz = 0;

        for (const Position &q : foot)
        {
          cx += q.x;
          cz += q.z;
        }

        cx /= double(std::max<size_t>(foot.size(), 1));
        cz /= double(std::max<size_t>(foot.size(), 1));
      };

      const auto onWing = [&](const size_t wing, const double px, const double pz) {
        const auto &foot = wings[wing].footprint;
        double best = 1e300;

        for (size_t k = 0; k < foot.size(); ++k)
        {
          const Position &a = foot[k], &b = foot[(k + 1) % foot.size()];
          const double ex = b.x - a.x, ez = b.z - a.z;
          const double length2 = ex * ex + ez * ez;
          const double t = length2 > 1e-12 ? std::min(1.0, std::max(0.0, ((px - a.x) * ex + (pz - a.z) * ez) / length2)) : 0;

          best = std::min(best, std::hypot(a.x + ex * t - px, a.z + ez * t - pz));
        }

        return best;
      };

      // The body is the largest member; everything else is a wing off it.
      size_t body = members.front();
      double widest = -1;

      for (const size_t i : members)
      {
        double area = 0;
        const auto &foot = wings[i].footprint;

        for (size_t k = 0; k < foot.size(); ++k)
        {
          const Position &a = foot[k], &b = foot[(k + 1) % foot.size()];
          area += a.x * b.z - b.x * a.z;
        }

        area = std::abs(area) * .5;

        if (area > widest)
        {
          widest = area;
          body = i;
        }
      }

      for (size_t i = 0; i < ring.size(); ++i)
      {
        const auto &a = ring[i], &b = ring[(i + 1) % ring.size()];
        const double ex = b.first - a.first, ez = b.second - a.second;
        const double elen = std::hypot(ex, ez);

        if (elen < 1e-6)
        {
          continue;
        }

        // A jog is not an end.
        //
        // Trimming a mass against its neighbours leaves steps exactly one
        // overhang deep - a third of a metre - and those were being gabled
        // like any other edge. Each one then grew its own extension spur, and
        // a spur is a different polygon with a different skeleton: the wedges
        // and seams on a terrace were half a dozen of them pulling the ridge
        // about. An end of a building is at least a room wide.
        if (elen < std::max(.9, overhang * 2.0))
        {
          continue;
        }

        const double mx = (a.first + b.first) * .5, mz = (a.second + b.second) * .5;

        // A wall shared with another mass. The roof rises to it whatever the
        // style asks for and whichever way the wall faces: hipped down to a
        // party wall, the building gets a gutter along it with nowhere to
        // fall, and the rain runs into the plan instead of off it.
        if (abuts(i))
        {
          roofing.gables[i] = true;

          continue;
        }

        // Whose wall is this? The wing it runs closest to, out of the ones
        // this cluster is made of.
        size_t owner = body;
        double nearest = 1e300;

        for (const size_t candidate : members)
        {
          const double d = onWing(candidate, mx, mz);

          if (d < nearest)
          {
            nearest = d;
            owner = candidate;
          }
        }

        const bool acrossMass = std::abs((ex / elen) * ux + (ez / elen) * uz) < .5;

        // The end of a wing that is not the body is the end of an extrusion,
        // and an extrusion ends in a gable in every style: that is what makes
        // it read as a cross-wing rather than a hipped lump stuck on the side.
        //
        // Which edge is the end, though, is the way the wing *projects* and
        // not the way its own outline happens to be longest. Taking the wing's
        // longest edge for its axis gabled the two flanks of every stub wider
        // than it was deep - a leg nine metres across and six long has its long
        // axis across itself - and a roof that rises to both flanks has to fall
        // along the leg instead: the deep fold down the middle of a T, with the
        // surface plunging to the eaves between two walls it should have run
        // over. The end of a wing is the wall facing away from the body.
        if (owner != body)
        {
          double wcx = 0, wcz = 0, bcx = 0, bcz = 0;
          centreOf(owner, wcx, wcz);
          centreOf(body, bcx, bcz);

          const double outx = wcx - bcx, outz = wcz - bcz;
          const double reach = std::hypot(outx, outz);

          // Outward from a counter-clockwise ring is to the right of the edge.
          const double nx = ez / elen, nz = -ex / elen;

          if (reach > 1e-6 && nx * (outx / reach) + nz * (outz / reach) > .7)
          {
            roofing.gables[i] = true;
          }

          continue;
        }

        if (std::abs((ex / elen) * ux + (ez / elen) * uz) >= .5)
        {
          continue; // A flank of the body. A roof falls off its flanks.
        }

        roofing.gables[i] = style == RoofStyle::Gabled || (style == RoofStyle::CrossGabled && elongated && acrossMass);
      }

      // A roof has to fall somewhere.
      //
      // Every edge marked here is an edge the surface rises to, and a mass
      // where most of the perimeter rises has nowhere left to shed: the
      // extension runs out on all sides at once and what comes back is a
      // plateau standing on tall walls rather than a roof. So the ends are
      // kept - that is what a gable is for - and the shared walls are given up
      // once they would take the mass past half its perimeter. Water off two
      // sides is the least a roof can do.
      {
        double gabled = 0, perimeter = 0;

        for (size_t i = 0; i < ring.size(); ++i)
        {
          const auto &a = ring[i], &b = ring[(i + 1) % ring.size()];
          const double elen = std::hypot(b.first - a.first, b.second - a.second);

          perimeter += elen;
          gabled += roofing.gables[i] ? elen : 0;
        }

        if (perimeter > 1e-6 && gabled > perimeter * .5)
        {
          for (size_t i = 0; i < ring.size(); ++i)
          {
            const auto &a = ring[i], &b = ring[(i + 1) % ring.size()];
            const double ex = b.first - a.first, ez = b.second - a.second;
            const double elen = std::hypot(ex, ez);

            roofing.gables[i] = roofing.gables[i] && elen > 1e-6 && std::abs((ex / elen) * ux + (ez / elen) * uz) < .5;
          }
        }
      }

      (void)lowEnd;
      (void)highEnd;
    }

    // Generate the roof over the complete footprint, preserving courts.
    geom2::Polygon eavesShape;

    for (const auto &q : ring)
    {
      eavesShape.outer.push_back({geom2::toUnit(q.first), geom2::toUnit(q.second)});
    }

    for (const geom2::Ring &hole : eavesPoly->holes)
    {
      eavesShape.holes.push_back(hole);
    }

    std::vector<geom2::RoofTriangle> surface;
    geom2::roofFromSkeleton(eavesShape, roofing, surface, nullptr);

    for (const geom2::RoofTriangle &t : surface)
    {
      // Preserve small positive-area faces: their edges close the surface.
      // Only numerical zeroes can be omitted.
      const double ux3 = t.bx - t.ax, uy3 = t.by - t.ay, uz3 = t.bz - t.az;
      const double vx3 = t.cx - t.ax, vy3 = t.cy - t.ay, vz3 = t.cz - t.az;
      const double nx3 = uy3 * vz3 - uz3 * vy3, ny3 = uz3 * vx3 - ux3 * vz3, nz3 = ux3 * vy3 - uy3 * vx3;

      if (std::sqrt(nx3 * nx3 + ny3 * ny3 + nz3 * nz3) * .5 < 1e-12)
      {
        continue;
      }

      rewrite::DwellingRoofFace face;
      face.wing = uint32_t(members.front());
      face.gable = t.gable;
      face.polygon = {
        Position{t.ax, eaves + t.ay, t.az},
        Position{t.bx, eaves + t.by, t.bz},
        Position{t.cx, eaves + t.cy, t.cz},
      };

      out.push_back(std::move(face));
    }

    // ---- the soffit -------------------------------------------------------
    //
    // The underside of the overhang, from each eaves edge back in to the wall
    // it oversails. Where the eaves sit flush - at a wall shared with another
    // mass - the two coincide and this collapses to nothing, which is right.
    for (size_t i = 0; i < ring.size(); ++i)
    {
      const auto &a = ring[i], &b = ring[(i + 1) % ring.size()];

      if (std::hypot(b.first - a.first, b.second - a.second) <= 1e-6)
      {
        continue;
      }

      const auto onWall = [&](const double px, const double pz, double &qx, double &qz) {
        double best = 1e300;
        qx = px;
        qz = pz;

        for (size_t k = 0; k < ground.outer.size(); ++k)
        {
          const double ax = geom2::toMetres(ground.outer[k].x), az = geom2::toMetres(ground.outer[k].y);
          const double bx = geom2::toMetres(ground.outer[(k + 1) % ground.outer.size()].x), bz = geom2::toMetres(ground.outer[(k + 1) % ground.outer.size()].y);
          const double wx = bx - ax, wz = bz - az;
          const double length2 = wx * wx + wz * wz;
          const double t = length2 > 1e-12 ? std::min(1.0, std::max(0.0, ((px - ax) * wx + (pz - az) * wz) / length2)) : 0;
          const double sx = ax + wx * t, sz = az + wz * t;
          const double d = std::hypot(sx - px, sz - pz);

          if (d < best)
          {
            best = d;
            qx = sx;
            qz = sz;
          }
        }
      };

      double wax = 0, waz = 0, wbx = 0, wbz = 0;
      onWall(a.first, a.second, wax, waz);
      onWall(b.first, b.second, wbx, wbz);

      if (std::hypot(wax - a.first, waz - a.second) <= .02 && std::hypot(wbx - b.first, wbz - b.second) <= .02)
      {
        continue;
      }

      const Position ea{a.first, eaves, a.second}, eb{b.first, eaves, b.second};
      const Position sa{wax, eaves, waz}, sb{wbx, eaves, wbz};

      const auto emitSoffit = [&](const Position &p, const Position &q, const Position &r) {
        if (std::abs((q.x - p.x) * (r.z - p.z) - (r.x - p.x) * (q.z - p.z)) * .5 < 1e-4)
        {
          return;
        }

        rewrite::DwellingRoofFace under;
        under.wing = uint32_t(members.front());
        under.soffit = true;
        under.polygon = {p, q, r};

        out.push_back(std::move(under));
      };

      emitSoffit(ea, sa, sb);
      emitSoffit(ea, sb, eb);
    }
  }
}
// The exterior as a stack of plates, one per storey.
//
// "The number of storeys at any particular place of a building is defined by
// its ground plan." So the plate at level k is the union of the ground of
// every wing carrying more than k storeys: the ground floor is the whole
// plan, and each floor above it is only the parts tall enough to have one.
// The outline changes as the building rises, which is what makes a mansion
// read as a mansion rather than as a prism with a roof on it.
//
// The walls of a level are that plate's boundary. A wall shared by two wings
// is interior to the union and is never emitted, which is also the fix for
// the duplicate faces every building carried where its wings met.
struct StoreyStyle
{
  double jetty = 0, arcade = 0;
  bool street = false;
  double streetX = 0, streetZ = 0, axisX = 1, axisZ = 0;
};

static void buildStoreys(const std::vector<rewrite::DwellingWing> &wings, const double foundation, const double floorRise, std::vector<rewrite::DwellingStorey> &out, const StoreyStyle &style = {}, std::vector<rewrite::DwellingPillar> *pillars = nullptr, std::vector<rewrite::DwellingRoofFace> *undersides = nullptr)
{
  // What the floor below stood on, so the floor above knows what it oversails.
  std::vector<geom2::Polygon> below;

  uint32_t tallest = 0;

  for (const rewrite::DwellingWing &w : wings)
  {
    tallest = std::max(tallest, w.storeys);
  }

  for (uint32_t level = 0; level < tallest; ++level)
  {
    std::vector<geom2::Operand> operands;

    for (const rewrite::DwellingWing &w : wings)
    {
      if (w.storeys <= level || w.footprint.size() < 3)
      {
        continue;
      }

      geom2::Polygon ground;

      for (const Position &q : w.footprint)
      {
        ground.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
      }

      if (geom2::polygonFault(ground).empty())
      {
        operands.push_back(geom2::toOperand(ground));
      }
    }

    if (operands.empty())
    {
      continue;
    }

    std::vector<geom2::Polygon> plates;

    if (!geom2::unionOf(operands, plates, nullptr))
    {
      continue;
    }

    // The jetty: every floor above the ground oversails the one below it, by
    // one jetty per floor, so the building widens as it rises. Not clipped to
    // the plot, because oversailing the street is the whole point of one.
    if (style.jetty > 0 && level > 0)
    {
      std::vector<geom2::Polygon> grown;

      if (geom2::offsetPolygons(plates, style.jetty * double(level), {}, grown, nullptr) && !grown.empty())
      {
        plates = std::move(grown);
      }
    }

    // The arcade: the ground floor is cut back from the street and posts
    // carry what is above it.
    if (style.arcade > 0 && level == 0 && style.street && tallest > 1)
    {
      // Inward from the street is towards the middle of the plan.
      double mx = 0, mz = 0, n = 0;

      for (const rewrite::DwellingWing &w : wings)
      {
        for (const Position &q : w.footprint)
        {
          mx += q.x;
          mz += q.z;
          n += 1;
        }
      }

      mx /= std::max(n, 1.0);
      mz /= std::max(n, 1.0);

      double nx = -style.axisZ, nz = style.axisX;

      if ((mx - style.streetX) * nx + (mz - style.streetZ) * nz < 0)
      {
        nx = -nx;
        nz = -nz;
      }

      double reach = 0;

      for (const geom2::Polygon &plate : plates)
      {
        for (const geom2::Point &q : plate.outer)
        {
          reach = std::max(reach, std::hypot(geom2::toMetres(q.x) - style.streetX, geom2::toMetres(q.y) - style.streetZ));
        }
      }

      // Everything on the far side of a line one arcade in from the street.
      geom2::Polygon keep;
      const double cx = style.streetX + nx * style.arcade, cz = style.streetZ + nz * style.arcade;
      const double sx = style.axisX * (reach + 4), sz = style.axisZ * (reach + 4);
      const double dx = nx * (reach + 4), dz = nz * (reach + 4);

      keep.outer = {
        {geom2::toUnit(cx - sx), geom2::toUnit(cz - sz)},
        {geom2::toUnit(cx + sx), geom2::toUnit(cz + sz)},
        {geom2::toUnit(cx + sx + dx), geom2::toUnit(cz + sz + dz)},
        {geom2::toUnit(cx - sx + dx), geom2::toUnit(cz - sz + dz)},
      };

      std::vector<geom2::Polygon> recessed;

      if (geom2::polygonFault(keep).empty())
      {
        std::vector<geom2::Operand> operands;

        for (const geom2::Polygon &plate : plates)
        {
          operands.push_back(geom2::toOperand(plate));
        }

        std::vector<geom2::Polygon> shrunk;
        double before = 0, after = 0;

        for (const geom2::Polygon &plate : plates)
        {
          before += geom2::areaMetres(plate);
        }

        for (const geom2::Operand &operand : operands)
        {
          std::vector<geom2::Polygon> piece;

          if (geom2::intersectionOf({operand, geom2::toOperand(keep)}, piece, nullptr))
          {
            for (geom2::Polygon &part : piece)
            {
              after += geom2::areaMetres(part);
              shrunk.push_back(std::move(part));
            }
          }
        }

        // Only if the ground floor is still most of the building: an arcade
        // that eats half of it is not an arcade.
        if (!shrunk.empty() && after > before * .55)
        {
          // The posts, along the line the wall used to stand on.
          if (pillars != nullptr)
          {
            double lo = 1e300, hi = -1e300;

            for (const geom2::Polygon &plate : plates)
            {
              for (const geom2::Point &q : plate.outer)
              {
                const double t = (geom2::toMetres(q.x) - style.streetX) * style.axisX + (geom2::toMetres(q.y) - style.streetZ) * style.axisZ;
                lo = std::min(lo, t);
                hi = std::max(hi, t);
              }
            }

            const double run = hi - lo;
            const int posts = std::max(2, int(run / 2.4));

            for (int k = 0; k <= posts; ++k)
            {
              const double t = lo + run * double(k) / double(posts);
              const double px = style.streetX + style.axisX * t + nx * .25;
              const double pz = style.streetZ + style.axisZ * t + nz * .25;

              rewrite::DwellingPillar post;
              post.base = foundation;
              post.top = foundation + floorRise;

              const double half = .14;

              post.footprint = {
                Position{px - half, foundation, pz - half},
                Position{px + half, foundation, pz - half},
                Position{px + half, foundation, pz + half},
                Position{px - half, foundation, pz + half},
              };

              pillars->push_back(std::move(post));
            }
          }

          plates = std::move(shrunk);
        }
      }
    }

    // The underside of a jetty.
    //
    // An upper floor that oversails the one below it has a floor sticking out
    // into the air, and nothing was ever built for it: the walls of each level
    // are that level's own boundary, so the overhang was a lip with open air
    // under it and the storey below ended at a wall standing inside its own
    // roof. What is missing is the horizontal face between the two outlines -
    // this plate less the one under it - which is the jetty's soffit, the
    // thing you look up at from the street.
    if (undersides != nullptr && level > 0 && !below.empty() && !plates.empty())
    {
      std::vector<geom2::Operand> clips;

      for (const geom2::Polygon &under : below)
      {
        clips.push_back(geom2::toOperand(under));
      }

      const double base = foundation + double(level) * floorRise;

      for (const geom2::Polygon &plate : plates)
      {
        std::vector<geom2::Polygon> lip;

        if (!geom2::differenceOf(geom2::toOperand(plate), clips, lip, nullptr))
        {
          continue;
        }

        for (const geom2::Polygon &piece : lip)
        {
          if (std::abs(geom2::areaMetres(piece)) < .01)
          {
            continue;
          }

          std::vector<std::array<uint32_t, 3>> triangles;
          std::vector<geom2::Point> vertices;

          if (!geom3::triangulate(piece, triangles, vertices, nullptr))
          {
            continue;
          }

          for (const auto &tri : triangles)
          {
            rewrite::DwellingRoofFace face;
            face.soffit = true;

            for (int k = 0; k < 3; ++k)
            {
              face.polygon.push_back(Position{geom2::toMetres(vertices[tri[k]].x), base, geom2::toMetres(vertices[tri[k]].y)});
            }

            undersides->push_back(std::move(face));
          }
        }
      }
    }

    below = plates;

    for (const geom2::Polygon &plate : plates)
    {
      if (geom2::areaMetres(plate) < .5)
      {
        continue;
      }

      rewrite::DwellingStorey storey;
      storey.level = level;
      storey.base = foundation + double(level) * floorRise;
      storey.top = foundation + double(level + 1) * floorRise;

      for (const geom2::Point &q : plate.outer)
      {
        storey.outer.push_back(Position{geom2::toMetres(q.x), storey.base, geom2::toMetres(q.y)});
      }

      for (const geom2::Ring &hole : plate.holes)
      {
        std::vector<Position> ring;

        for (const geom2::Point &q : hole)
        {
          ring.push_back(Position{geom2::toMetres(q.x), storey.base, geom2::toMetres(q.y)});
        }

        storey.holes.push_back(std::move(ring));
      }

      out.push_back(std::move(storey));
    }
  }
}

// Ridge, storeys and ground for each rectangular block of a building.
//
// Split out because a building's blocks can come from two places: carved out
// of an outline it was handed, or composed from nothing. What is done with
// them afterwards is the same either way.
static void wingsFromBlocks(const std::vector<geom2::Polygon> &blocks, const double ux, const double uy, const double foundation, const double floorRise, const uint32_t maximumStoreys, const ComplexDwellingSettings &s, std::vector<rewrite::DwellingWing> &wings, const uint32_t minimumStoreys = 1, const uint64_t seed = 0)
{
  double largestBlock = 0;

  for (const geom2::Polygon &block : blocks)
  {
    largestBlock = std::max(largestBlock, geom2::areaMetres(block));
  }

  for (const geom2::Polygon &block : blocks)
  {
    geom2::Point centre;

    if (!geom2::centroid(block, centre))
    {
      continue;
    }

    double spanU = 0, spanV = 0;
    extentOn(block, ux, uy, spanU, spanV);

    // How many storeys this wing carries, from its own width.
    //
    // Asking whether the wing's centre lies inside the storey plan was the
    // obvious test and the wrong one: an inward offset followed by an
    // outward one spills back across a junction, so the seven metre bar of a
    // T tested inside the two-storey plan and every wing came out three
    // storeys tall. The wing knows its own width, and the rule is about
    // width - "one-cell wide pieces usually end up being 1 story tall".
    const double across = std::min(spanU, spanV);

    const uint32_t byWidth = across < s.storeyWidthMetres ? 1u : across < s.fullHeightWidthMetres ? 2u : maximumStoreys;
    // A brief can insist on a floor as well as a ceiling: a merchant's house
    // is two storeys because that is what a merchant's house is, not because
    // its wing happens to be wide. The width rule still decides above it.
    uint32_t storeys = std::max(std::max(1u, minimumStoreys), std::min<uint32_t>(byWidth, maximumStoreys));

    // ...and then the wing is allowed to differ from its neighbours.
    //
    // Width alone gives every wing of a compact house the same height, so the
    // building is a prism however many wings it has, and the storey plates
    // stacked to a constant outline. A range that never varies is not a range.
    // Two departures, both of which real buildings have: a small wing beside
    // a larger one is a tower and takes the full height whatever its width,
    // and a wing that is not the body sometimes stops a storey short, which
    // is what makes the silhouette step.
    if (maximumStoreys > 1)
    {
      const double cx = geom2::toMetres(centre.x), cy = geom2::toMetres(centre.y);
      uint64_t bits = seed ^ (uint64_t(int64_t(cx * 64)) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(int64_t(cy * 64)) * 0xC2B2AE3D27D4EB4Full);
      bits ^= bits >> 33;
      bits *= 0xff51afd7ed558ccdull;
      bits ^= bits >> 29;

      const double draw = double(bits >> 11) / 9007199254740992.0;
      const double area = geom2::areaMetres(block);

      if (area < largestBlock * .45 && across < s.storeyWidthMetres && draw < .3)
      {
        storeys = maximumStoreys; // A tower.
      }
      else if (area < largestBlock * .95 && draw > .72)
      {
        storeys = std::max(std::max(1u, minimumStoreys), storeys - 1u);
      }
    }

    const bool ridgeAlongU = spanU >= spanV;
    const double rx = ridgeAlongU ? ux : -uy, ry = ridgeAlongU ? uy : ux;
    const double half = (ridgeAlongU ? spanU : spanV) * .5 - s.wallThickness;

    const double cx = geom2::toMetres(centre.x), cy = geom2::toMetres(centre.y);

    rewrite::DwellingWing wing;
    wing.storeys = storeys;
    wing.eavesHeight = foundation + double(storeys) * floorRise;
    // A roof rises about a third of its own span: steep enough to shed, low
    // enough that a long wing does not become a spire.
    wing.ridgeHeight = wing.eavesHeight + std::min(spanU, spanV) * .34;
    wing.ridgeFrom = Position{cx - rx * half, wing.ridgeHeight, cy - ry * half};
    wing.ridgeTo = Position{cx + rx * half, wing.ridgeHeight, cy + ry * half};
    // Grouped below, once every wing is known.
    wing.cluster = storeys;

    for (const geom2::Point &q : block.outer)
    {
      wing.footprint.push_back(Position{geom2::toMetres(q.x), foundation, geom2::toMetres(q.y)});
    }

    wings.push_back(std::move(wing));
  }
}


// A building's ground, composed rather than carved.
//
// Every exterior so far was the plot's own shape, inset: the outline came in
// from outside and the massing only decided where to divide it. That can make
// an L out of an L-shaped plot and never anything else, so the shapes on offer
// are exactly the shapes the town's lots happen to have.
//
// A mansion "consists of 'wings' - rectangular blocks", and that is a
// construction, not a description of something already drawn. So build it the
// same way: lay down one wing, then repeatedly pick a wing already standing,
// pick a side of it, and push a new wing out from part of that side. Each
// addition has a length, a depth and an offset along the side drawn at random,
// and is kept only if it stays on the plot and clear of what is already built.
//
// Nothing here names a shape. An L appears when one wing is added off the end
// of another, a T when it is added to the middle, a U when two go on the same
// side, a courtyard when a fourth closes the ring - and so do the forms that
// have no name. The repertoire is the rule, not a list.
static void composeWings(const geom2::Polygon &plot, const uint64_t seed, const double coverage, const ComplexDwellingSettings &s, std::vector<geom2::Polygon> &blocks, const double ux, const double uy, const bool anchored, const double anchorX, const double anchorY)
{

  // Work on the plot's own axes, so a wing is an interval pair and overlap is
  // a rectangle test rather than a polygon one.
  double u0 = 1e30, u1 = -1e30, v0 = 1e30, v1 = -1e30;

  for (const geom2::Point &q : plot.outer)
  {
    const double x = geom2::toMetres(q.x), y = geom2::toMetres(q.y);
    const double u = x * ux + y * uy, v = -x * uy + y * ux;

    u0 = std::min(u0, u);
    u1 = std::max(u1, u);
    v0 = std::min(v0, v);
    v1 = std::max(v1, v);
  }

  const double width = u1 - u0, height = v1 - v0;

  if (width < 2.5 || height < 2.5)
  {
    return;
  }

  uint64_t state = seed * 6364136223846793005ull + 1442695040888963407ull;

  const auto random = [&]() {
    state ^= state >> 33;
    state *= 0xff51afd7ed558ccdull;
    state ^= state >> 29;

    return double(state >> 11) / 9007199254740992.0;
  };

  struct Rect
  {
    double u0, v0, u1, v1;
  };

  const double budget = geom2::areaMetres(plot) * coverage;

  // A wing is as deep as a building can span, but never deeper than the
  // ground is - and the floor scales with the ground too. Refusing any lot
  // narrower than a full wing declined half the town: a slum lot is thirty
  // square metres and its house is one shallow room deep, which is a wing.
  const double deep = std::min(s.maximumWingDepthMetres, std::max(width, height));
  const double shallow = std::min(s.minimumWingDepthMetres, std::min(deep, std::min(width, height) * .9));

  // A candidate is what survives inside the plot, not the rectangle asked
  // for. The bounding box equals the plot only on a rectangle; on a trapezoid
  // - which is what a lot cut from a Voronoi block is - a wing spanning the
  // box pokes past the sloping sides, and sixty-five of the town's houses
  // stood partly outside their lots with seven pairs overlapping across a
  // boundary. Clipping is what makes the house the lot's shape along the
  // street where it has to be, and rectangular where it can be.
  const auto clipToPlot = [&](const Rect &r, const double keepShare, geom2::Polygon &piece) {
    geom2::Polygon rect;
    const double cu[] = {r.u0, r.u1, r.u1, r.u0};
    const double cv[] = {r.v0, r.v0, r.v1, r.v1};

    for (int i = 0; i < 4; ++i)
    {
      rect.outer.push_back({geom2::toUnit(cu[i] * ux - cv[i] * uy), geom2::toUnit(cu[i] * uy + cv[i] * ux)});
    }

    if (!geom2::polygonFault(rect).empty())
    {
      return false;
    }

    std::vector<geom2::Polygon> inside;

    if (!geom2::intersectionOf({geom2::toOperand(rect), geom2::toOperand(plot)}, inside, nullptr) || inside.empty())
    {
      return false;
    }

    size_t best = 0;

    for (size_t i = 1; i < inside.size(); ++i)
    {
      best = geom2::areaMetres(inside[i]) > geom2::areaMetres(inside[best]) ? i : best;
    }

    if (geom2::areaMetres(inside[best]) < geom2::areaMetres(rect) * keepShare)
    {
      return false;
    }

    // A wing with a hole in it is not a wing. The plot may have ground cut
    // out of it - the court of a perimeter block - and a rectangle laid
    // across that comes back with the court as a hole; read as an outline it
    // is a building over the court, which is what two of the town's had.
    if (!inside[best].holes.empty())
    {
      return false;
    }

    piece = inside[best];

    return true;
  };

  std::vector<geom2::Polygon> kept;

  // The first wing runs along the axis - the street, when the caller knows
  // where it is - and stands on whichever side of the plot the anchor is.
  //
  // Placed at random it lands anywhere, and a house in the middle of its lot
  // with ground all round it is a villa. A town house stands on its street
  // with its yard behind, and that is what the anchor says.
  std::vector<Rect> placed;

  // ---- a steading: ranges laid round a court ------------------------------
  //
  // The reference is a nineteenth-century farm steading, and the thing to take
  // from it is that the yard is a cell of the plan rather than what is left
  // over. Ranges one room deep wrap it - fodder house, cart shed, cow house,
  // stable, dairy, wash-house, each a bay of the range it sits in - and the
  // court they enclose is as deliberate as any of them.
  //
  // Composed that way a courtyard, a U and an L are one construction with a
  // range dropped or two, and the building comes out of the generator whole
  // instead of being assembled from free-standing wings. The plates are clean
  // rectangles, which is also the outline a roof wants.
  //
  // Only where there is room and a reason: a plot built over three quarters is
  // a terrace and has no court to give.
  const double depth = std::min(deep, std::max(shallow, std::min(width, height) * .28));
  const bool roomForCourt = coverage < .55 && width > depth * 3.2 && height > depth * 3.2;

  if (roomForCourt)
  {
    const double cu0 = u0 + depth, cu1 = u1 - depth, cv0 = v0 + depth, cv1 = v1 - depth;

    const Rect ranges[4] = {
      {u0, v0, u1, v0 + depth}, // The street side, always built.
      {u0, v1 - depth, u1, v1},
      {u0, cv0, u0 + depth, cv1},
      {u1 - depth, cv0, u1, cv1},
    };

    // Which side the street is on, so the range facing it is never the one
    // dropped.
    size_t front = 0;

    if (anchored)
    {
      const double av = -anchorX * uy + anchorY * ux;
      front = std::abs(av - v0) <= std::abs(av - v1) ? 0 : 1;
    }

    // Three quarters of the time a range is dropped, which is what makes a U;
    // sometimes two, which makes an L.
    const int drop = random() < .35 ? 0 : (random() < .6 ? 1 : 2);
    std::vector<size_t> order{0, 1, 2, 3};

    for (size_t i = order.size(); i > 1; --i)
    {
      std::swap(order[i - 1], order[size_t(random() * double(i)) % i]);
    }

    std::vector<bool> keep(4, true);
    int dropped = 0;

    for (const size_t which : order)
    {
      if (dropped < drop && which != front)
      {
        keep[which] = false;
        ++dropped;
      }
    }

    for (size_t i = 0; i < 4; ++i)
    {
      if (!keep[i])
      {
        continue;
      }

      geom2::Polygon piece;

      if (!clipToPlot(ranges[i], .55, piece))
      {
        continue;
      }

      placed.push_back(ranges[i]);
      kept.push_back(std::move(piece));
    }

    // A steading needs at least two ranges to be one; with fewer, fall through
    // to the ordinary composition rather than leave a lone bar.
    if (kept.size() >= 2 && cu1 > cu0 && cv1 > cv0)
    {
      for (geom2::Polygon &piece : kept)
      {
        if (geom2::polygonFault(piece).empty())
        {
          blocks.push_back(std::move(piece));
        }
      }

      return;
    }

    placed.clear();
    kept.clear();
  }

  {
    // The first wing is the body of the house, and it is sized from the
    // budget rather than from the frontage.
    //
    // On a packed lot - coverage past a half - the body is the whole budget
    // and one lot deep, because a slum house is its lot with a setback. Any
    // rule that draws the depth at random or leaves part of the budget for
    // wings that will not fit leaves a strip of ground on a lot that is
    // supposed to have none; slum plots came out 37% open against a
    // standing of 22% that way. On an open plot the body is a squarish
    // block taking most of the budget, and what remains buys the wings.
    //
    // The floor is a room, not a wing: a rural cottage is four metres deep,
    // and forcing its run up to a full wing's depth made it half again the
    // size its standing allows.
    // "Packed" is a standing, not a size: a slum or a craftsman's plot, whose
    // coverage against the buildable ground comes out past seven tenths. At
    // a half the body still took the whole budget and the composed corpus
    // collapsed to single wings - 127 of 240 - so the threshold is set where
    // the standings actually fall, and an open plot's body leaves a fifth to
    // nearly half of the budget for the wings that make it a house rather
    // than a block.
    const bool packed = coverage >= .7;
    const double body = budget * (packed ? 1.0 : (.55 + .25 * random()));

    double depth = std::min(std::min(deep, height), std::max(4.0, std::sqrt(body / (1.2 + .6 * random()))));

    if (packed && height <= deep)
    {
      depth = height;
    }

    const double run = std::min(width, std::max(std::min(width, 4.0), body / std::max(depth, 1e-6)));
    const double runShare = run / width;
    const double start = u0 + (width - run) * random();

    double base = v0 + (height - depth) * random();

    if (anchored)
    {
      const double av = -anchorX * uy + anchorY * ux;
      base = std::abs(av - v0) <= std::abs(av - v1) ? v0 : v1 - depth;
    }

    // The first wing keeps whatever the lot leaves of it, down to a room -
    // and gets a few draws, because the first place tried may straddle a
    // court or a notch.
    bool stood = false;

    for (int attempt = 0; attempt < 6 && !stood; ++attempt)
    {
      const double runNow = attempt == 0 ? run : width * (runShare * .7 + (1.0 - runShare * .7) * random());
      const double startNow = attempt == 0 ? start : u0 + (width - runNow) * random();
      const Rect first{startNow, base, startNow + runNow, base + depth};
      geom2::Polygon piece;

      if (!clipToPlot(first, .0, piece) || geom2::areaMetres(piece) < s.minimumRoomMetres)
      {
        continue;
      }

      placed.push_back(first);
      kept.push_back(std::move(piece));
      stood = true;
    }

    if (!stood)
    {
      return;
    }
  }

  const auto area = [](const Rect &r) { return (r.u1 - r.u0) * (r.v1 - r.v0); };

  const auto insidePlot = [&](const Rect &r) {
    // Every corner and every edge midpoint on the plot. Corners alone let a
    // wing bridge a concave notch with its middle hanging over nothing.
    const double us[] = {r.u0, r.u1, r.u0, r.u1, (r.u0 + r.u1) * .5, (r.u0 + r.u1) * .5, r.u0, r.u1};
    const double vs[] = {r.v0, r.v0, r.v1, r.v1, r.v0, r.v1, (r.v0 + r.v1) * .5, (r.v0 + r.v1) * .5};

    for (int i = 0; i < 8; ++i)
    {
      const double x = us[i] * ux - vs[i] * uy, y = us[i] * uy + vs[i] * ux;

      if (!geom2::contains(plot, {geom2::toUnit(x), geom2::toUnit(y)}))
      {
        return false;
      }
    }

    return true;
  };

  const auto clear = [&](const Rect &r) {
    for (const Rect &other : placed)
    {
      const double overlapU = std::min(r.u1, other.u1) - std::max(r.u0, other.u0);
      const double overlapV = std::min(r.v1, other.v1) - std::max(r.v0, other.v0);

      // Touching along a wall is the point; overlapping is not. Both
      // overlaps have to be positive by more than a wall for it to be one.
      if (overlapU > s.wallThickness && overlapV > s.wallThickness)
      {
        return false;
      }
    }

    return true;
  };

  double built = area(placed.front());

  for (int attempt = 0; attempt < 48 && built < budget && placed.size() < 6; ++attempt)
  {
    const Rect &host = placed[size_t(random() * double(placed.size())) % placed.size()];
    const int side = int(random() * 4) % 4;

    const double depth = shallow + (deep - shallow) * random();
    const bool alongU = side < 2;
    const double hostRun = alongU ? host.u1 - host.u0 : host.v1 - host.v0;
    const double run = std::max(shallow, hostRun * (.35 + .65 * random()));
    const double slide = (hostRun - run) * random();

    Rect candidate{};

    if (alongU)
    {
      candidate.u0 = host.u0 + slide;
      candidate.u1 = candidate.u0 + run;
      candidate.v0 = side == 0 ? host.v0 - depth : host.v1;
      candidate.v1 = candidate.v0 + depth;
    }
    else
    {
      candidate.v0 = host.v0 + slide;
      candidate.v1 = candidate.v0 + run;
      candidate.u0 = side == 2 ? host.u0 - depth : host.u1;
      candidate.u1 = candidate.u0 + depth;
    }

    if (!clear(candidate))
    {
      continue;
    }

    // An added wing has to be nearly all there: one the lot cuts to a sliver
    // is not a wing, it is a shed leaning on the house.
    geom2::Polygon piece;

    if (!clipToPlot(candidate, .85, piece))
    {
      continue;
    }

    placed.push_back(candidate);
    kept.push_back(std::move(piece));
    built += area(candidate);
  }

  (void)insidePlot;

  for (geom2::Polygon &piece : kept)
  {
    if (geom2::polygonFault(piece).empty())
    {
      blocks.push_back(std::move(piece));
    }
  }
}

// The massing of a building composed on a plot, rather than carved out of a
// footprint it was given.
void massComposedBuilding(const MassingBrief &b, const ComplexDwellingSettings &s, std::vector<rewrite::DwellingWing> &wings, std::vector<rewrite::DwellingRoofFace> &roofs, std::vector<rewrite::DwellingStorey> *storeys, std::vector<rewrite::DwellingPillar> *pillars)
{
  if (b.plot.size() < 3 || b.maximumStoreys == 0)
  {
    return;
  }

  geom2::Polygon ground;

  for (const Position &q : b.plot)
  {
    ground.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
  }

  for (const auto &hole : b.holes)
  {
    geom2::Ring inner;

    for (const Position &q : hole)
    {
      inner.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
    }

    if (inner.size() >= 3)
    {
      ground.holes.push_back(std::move(inner));
    }
  }

  double ux = b.axisX, uy = b.axisZ;
  const double axisLength = std::hypot(ux, uy);

  if (axisLength > 1e-9)
  {
    ux /= axisLength;
    uy /= axisLength;
  }
  else
  {
    axesOf(ground.outer, ux, uy);
  }

  std::vector<geom2::Polygon> blocks;
  composeWings(ground, b.seed, b.coverage, s, blocks, ux, uy, b.anchored, b.anchorX, b.anchorZ);
  wingsFromBlocks(blocks, ux, uy, b.foundation, b.floorRise, b.maximumStoreys, s, wings, b.minimumStoreys, b.seed);

  groupWingsIntoClusters(wings, s.wallThickness);
  buildRoofs(wings, s.eavesOverhangMetres, s.fullHeightWidthMetres, roofs, s.roofStyle);
  if (storeys != nullptr)
  {
    StoreyStyle style;
    style.jetty = b.jettyMetres;
    style.arcade = b.arcadeDepthMetres;
    style.street = b.anchored;
    style.streetX = b.anchorX;
    style.streetZ = b.anchorZ;
    style.axisX = ux;
    style.axisZ = uy;

    buildStoreys(wings, b.foundation, b.floorRise, *storeys, style, pillars, &roofs);
  }
}

void massComposedBuilding(const std::vector<Position> &plot, const uint64_t seed, const double coverage, const double foundation, const double floorRise, const uint32_t maximumStoreys, const ComplexDwellingSettings &s, std::vector<rewrite::DwellingWing> &wings, std::vector<rewrite::DwellingRoofFace> &roofs, std::vector<rewrite::DwellingStorey> *storeys)
{
  MassingBrief b;
  b.plot = plot;
  b.seed = seed;
  b.coverage = coverage;
  b.foundation = foundation;
  b.floorRise = floorRise;
  b.maximumStoreys = maximumStoreys;

  massComposedBuilding(b, s, wings, roofs, storeys, nullptr);
}

// The massing of a building: its wings, the storeys each carries, which of
// them share a roof, and the surfaces of that roof.
//
// Separated from `planComplexDwelling` because it needs none of it. A building
// with no interior at all - one of the town's ordinary houses, which is a
// footprint and a height and nothing more - has the same outside as one that
// was planned room by room, and had no way to ask for it. The town could
// therefore only ever be drawn flat.
void massBuilding(const std::vector<Position> &outline, const double foundation, const double floorRise, const uint32_t maximumStoreys, const ComplexDwellingSettings &s, std::vector<rewrite::DwellingWing> &wings, std::vector<rewrite::DwellingRoofFace> &roofs, std::vector<rewrite::DwellingStorey> *storeys, std::vector<rewrite::DwellingPillar> *pillars, const double jettyMetres, const double arcadeDepthMetres, const uint32_t minimumStoreys)
{
  if (outline.size() < 3 || maximumStoreys == 0)
  {
    return;
  }

  geom2::Polygon ground;

  for (const Position &q : outline)
  {
    ground.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
  }

  double ux = 1, uy = 0;
  axesOf(ground.outer, ux, uy);

  // ---- the outside: wings, and the roof each one carries --------------------
  //
  // "A mansion consists of 'wings' - rectangular blocks, each with its own
  // roof", and "wings are grouped into 'clusters' - sets of wings whose roofs
  // can be merged organically". The consequence the author draws from it is
  // the one that matters: "there is a one-one match between its exterior and
  // floor plan". The outside is the plan seen from above, not a shape laid on
  // top of a finished interior.
  //
  // So the wings come from the same decomposition the rooms do, read at the
  // scale of the building: the ground plan cut on its own axes into pieces
  // about a wing wide. Each carries the storeys that part of the plan supports
  // - which is already computed - and its ridge runs along its long axis,
  // because that is where a roof's ridge goes.
  {
    std::vector<geom2::Polygon> blocks;
    uint64_t wingCuts = 0;

    const std::function<void(const geom2::Polygon &, uint32_t)> carve = [&](const geom2::Polygon &piece, const uint32_t depth) {
      // A wing is a rectangular block, so the plan is cut where it stops being
      // one: at a reflex corner.
      //
      // Testing the bounding extent instead was useless - a T thirty metres by
      // twenty is not elongated by that measure, so it never split and every
      // building came out as a single wing. The reflex corners *are* the
      // places the outline turns back on itself, which is exactly where one
      // wing ends and the next begins, and an L has one, a T two and a U two.
      size_t reflex = piece.outer.size();
      double sharpest = 0;

      for (size_t i = 0; i < piece.outer.size(); ++i)
      {
        const geom2::Point &prev = piece.outer[(i + piece.outer.size() - 1) % piece.outer.size()];
        const geom2::Point &here = piece.outer[i];
        const geom2::Point &next = piece.outer[(i + 1) % piece.outer.size()];

        const double ax = geom2::toMetres(here.x) - geom2::toMetres(prev.x), ay = geom2::toMetres(here.y) - geom2::toMetres(prev.y);
        const double bx = geom2::toMetres(next.x) - geom2::toMetres(here.x), by = geom2::toMetres(next.y) - geom2::toMetres(here.y);
        const double cross = ax * by - ay * bx;
        const double la = std::hypot(ax, ay), lb = std::hypot(bx, by);

        // Counter clockwise, so a negative turn is a corner that bites in.
        if (cross >= 0 || la < 1e-6 || lb < 1e-6)
        {
          continue;
        }

        const double turn = -cross / (la * lb);

        if (turn > sharpest)
        {
          sharpest = turn;
          reflex = i;
        }
      }

      double spanU = 0, spanV = 0;
      extentOn(piece, ux, uy, spanU, spanV);

      // Only reflex corners divide wings. Splitting a long piece as well
      // turned a T into seventeen blocks, because each half was still longer
      // than the threshold - a long wing is a long wing, and cutting it into
      // three does not make the building read better, it makes the roof read
      // as a terrace.
      (void)spanU;
      (void)spanV;

      if (depth >= 5u || reflex == piece.outer.size())
      {
        blocks.push_back(piece);

        return;
      }

      // Through the reflex corner, on whichever axis leaves the straighter
      // pieces - or through the middle when the piece is merely too long.
      double px = 0, py = 0;

      if (reflex < piece.outer.size())
      {
        px = geom2::toMetres(piece.outer[reflex].x);
        py = geom2::toMetres(piece.outer[reflex].y);
      }
      else
      {
        geom2::Point centre;
        geom2::centroid(piece, centre);

        px = geom2::toMetres(centre.x);
        py = geom2::toMetres(centre.y);
      }

      std::vector<geom2::Polygon> best;
      geom2::Point bestFrom, bestTo;
      double bestScore = -1;

      for (const auto &[nx, ny] : {std::pair<double, double>{ux, uy}, {-uy, ux}})
      {
        geom2::Point from, to;
        std::vector<geom2::Polygon> halves;

        if (!chordThrough(piece.outer, px, py, nx, ny, from, to) || !geom2::cutPolygon(piece, from, to, halves, nullptr) || halves.size() < 2u)
        {
          continue;
        }

        // The cut that leaves the most rectangular pieces wins.
        double score = 1e300;

        for (const geom2::Polygon &half : halves)
        {
          double halfU = 0, halfV = 0;
          extentOn(half, nx, ny, halfU, halfV);

          score = std::min(score, halfU * halfV > 1e-9 ? geom2::areaMetres(half) / (halfU * halfV) : 0.0);
        }

        if (score > bestScore)
        {
          bestScore = score;
          best = std::move(halves);
          bestFrom = from;
          bestTo = to;
        }
      }

      if (best.size() < 2u)
      {
        blocks.push_back(piece);

        return;
      }

      ++wingCuts;

      for (const geom2::Polygon &half : best)
      {
        if (geom2::areaMetres(half) >= s.minimumRoomMetres)
        {
          carve(half, depth + 1);
        }
      }
    };

    carve(ground, 0);

    wingsFromBlocks(blocks, ux, uy, foundation, floorRise, maximumStoreys, s, wings, minimumStoreys, uint64_t(outline.size()) * 1315423911ull + uint64_t(std::llround(std::abs(outline.front().x + outline.front().z) * 128)));
  }


  groupWingsIntoClusters(wings, s.wallThickness);
  buildRoofs(wings, s.eavesOverhangMetres, s.fullHeightWidthMetres, roofs, s.roofStyle);

  if (storeys != nullptr)
  {
    // The street is taken as the outline's longest edge. The plot planner
    // knows which edges are really street and the carving path does not; the
    // longest edge is the same guess a lot makes when it is handed to a
    // generator on its own.
    StoreyStyle style;
    style.jetty = jettyMetres;
    style.arcade = arcadeDepthMetres;

    double best = -1;

    for (size_t i = 0; i < outline.size(); ++i)
    {
      const Position &a = outline[i], &b = outline[(i + 1) % outline.size()];
      const double length = std::hypot(b.x - a.x, b.z - a.z);

      if (length > best)
      {
        best = length;
        style.street = true;
        style.streetX = (a.x + b.x) * .5;
        style.streetZ = (a.z + b.z) * .5;
        style.axisX = (b.x - a.x) / length;
        style.axisZ = (b.z - a.z) / length;
      }
    }

    buildStoreys(wings, foundation, floorRise, *storeys, style, pillars, &roofs);
  }
}

bool planComplexDwelling(const DwellingBrief &b, const ComplexDwellingSettings &s, DwellingPlan &out, std::string *error)
{
  out = DwellingPlan{};
  out.id = b.id;
  out.seed = b.seed;
  out.use = b.use;

  if (b.shell.size() < 3u || b.floors < 1u || b.floors > 4u)
  {
    return fail(error, "a complex dwelling needs a simple footprint and one to four floors");
  }

  if (!(s.roomAreaMetres > 0) || !(s.minimumRoomMetres > 0))
  {
    return fail(error, "room sizes must be positive");
  }

  geom2::Polygon footprint;

  for (const Position &p : b.shell)
  {
    footprint.outer.push_back(at(p.x, p.z));
  }

  if (geom2::areaMetres(footprint) < s.minimumRoomMetres * 2)
  {
    return fail(error, "footprint too small to hold two rooms");
  }

  // The walls have thickness, so the rooms are planned inside the shell rather
  // than on it. An inset that eats the whole footprint means the building is
  // all wall, which is a refusal rather than a plan.
  std::vector<geom2::Polygon> inside;

  if (!geom2::offsetPolygon(footprint, -s.wallThickness, {}, inside, error))
  {
    return false;
  }

  if (inside.empty())
  {
    return fail(error, "footprint is thinner than its own walls");
  }

  double ux = 1, uy = 0;
  axesOf(footprint.outer, ux, uy);

  uint64_t roomId = 1;
  uint64_t wallId = 1;

  // How much of the plan carries each storey.
  //
  // "The number of storeys at any particular place of a building is defined by
  // its ground plan" - Watabou's mansion generator, and the reason his mansions
  // read as mansions rather than as extruded outlines. A narrow wing is one
  // storey, the body is two or three, and the difference is measured on the
  // plan rather than declared.
  //
  // The width of the building at a place is how far you can go from it before
  // leaving: an inward offset of d survives exactly where the plan is more than
  // 2d wide, so offsetting by half the storey width leaves the ground that can
  // carry an upper floor.
  std::vector<geom2::Polygon> storeyPlan{inside.front()};

  for (uint32_t floor = 1; floor < b.floors; ++floor)
  {
    const double want = floor == 1u ? s.storeyWidthMetres : s.fullHeightWidthMetres;

    std::vector<geom2::Polygon> narrower;

    if (!geom2::offsetPolygon(storeyPlan.front(), -want * .5, {}, narrower, nullptr) || narrower.empty())
    {
      break;
    }

    std::sort(narrower.begin(), narrower.end(), [](const geom2::Polygon &l, const geom2::Polygon &r) {
      return geom2::areaMetres(l) > geom2::areaMetres(r);
    });

    // Grown back out, so the storey stands on the part of the plan that is
    // wide enough rather than on a shrunken copy of it.
    std::vector<geom2::Polygon> back;

    if (!geom2::offsetPolygon(narrower.front(), want * .5, {}, back, nullptr) || back.empty())
    {
      break;
    }

    std::vector<geom2::Polygon> clipped;

    if (!geom2::booleanOp({geom2::toOperand(back.front()), geom2::toOperand(inside.front())}, geom2::Operation::Intersection, {}, clipped, nullptr) || clipped.empty())
    {
      break;
    }

    std::sort(clipped.begin(), clipped.end(), [](const geom2::Polygon &l, const geom2::Polygon &r) {
      return geom2::areaMetres(l) > geom2::areaMetres(r);
    });

    if (geom2::areaMetres(clipped.front()) < s.minimumRoomMetres * 2)
    {
      break;
    }

    storeyPlan.push_back(clipped.front());
  }

  for (uint32_t floor = 0; floor < uint32_t(storeyPlan.size()); ++floor)
  {
    const double y = b.foundation + floor * b.floorRise;
    const geom2::Polygon &plate = storeyPlan[floor];

    // ---- the subdivision --------------------------------------------------
    //
    // Halve the area, judge the shape, keep to the footprint's two axes, and
    // stop only when a piece is both near the target size and square enough.
    // The same rule the town's lots use, because a room and a lot are the same
    // problem at different scales.
    std::vector<geom2::Polygon> rooms;

    // Declared before the subdivision so the cut scoring can see it: the
    // corridor is carved first and the rooms are cut around it.
    std::vector<geom2::Polygon> corridors;
    uint64_t cuts = 0;

    const std::function<void(const geom2::Polygon &, uint32_t)> divide = [&](const geom2::Polygon &piece, const uint32_t depth) {
      const double area = geom2::areaMetres(piece);

      if (piece.outer.size() < 3u || depth >= 9u)
      {
        rooms.push_back(piece);

        return;
      }

      double spanU = 0, spanV = 0;
      extentOn(piece, ux, uy, spanU, spanV);

      const double squareness = std::max(spanU, spanV) > 1e-9 ? std::min(spanU, spanV) / std::max(spanU, spanV) : 1.0;
      const double stop = s.roomAreaMetres * std::pow(2.0, 1.4 * (draw(b.seed, floor, depth, cuts) - .5));

      if (area < s.minimumRoomMetres * 2.2 || (area < stop && squareness >= s.squareEnoughRatio))
      {
        rooms.push_back(piece);

        return;
      }

      const double fraction = .5 + (draw(b.seed, floor, depth + 64, cuts) - .5) * .16;

      std::vector<geom2::Polygon> chosen;
      geom2::Point chosenFrom, chosenTo;
      double chosenScore = -1;

      for (const auto &[nx, ny] : {std::pair<double, double>{ux, uy}, {-uy, ux}})
      {
        // Bisection over the cut's offset: the only parameter with a monotone
        // relation to the area it cuts off, which is what makes it searchable.
        const double sx = -ny, sy = nx;
        double lo = 1e300, hi = -1e300;

        for (const geom2::Point &p : piece.outer)
        {
          const double t = geom2::toMetres(p.x) * sx + geom2::toMetres(p.y) * sy;

          lo = std::min(lo, t);
          hi = std::max(hi, t);
        }

        if (hi - lo < 1e-6)
        {
          continue;
        }

        std::vector<geom2::Polygon> halves;
        geom2::Point from, to;
        bool found = false;

        for (int step = 0; step < 22; ++step)
        {
          const double mid = (lo + hi) * .5;

          geom2::Point a, c;
          std::vector<geom2::Polygon> pieces;

          if (!chordThrough(piece.outer, sx * mid, sy * mid, nx, ny, a, c) || !geom2::cutPolygon(piece, a, c, pieces, nullptr) || pieces.size() < 2)
          {
            lo = lo + (hi - lo) * .25;
            hi = hi - (hi - lo) * .25;

            continue;
          }

          double low = 0, total = 0;

          for (const geom2::Polygon &part : pieces)
          {
            geom2::Point centre;
            const double partArea = geom2::areaMetres(part);

            total += partArea;

            if (geom2::centroid(part, centre) && geom2::toMetres(centre.x) * sx + geom2::toMetres(centre.y) * sy < mid)
            {
              low += partArea;
            }
          }

          if (total < 1e-9)
          {
            break;
          }

          halves = std::move(pieces);
          from = a;
          to = c;
          found = true;

          if (low / total < fraction)
          {
            lo = mid;
          }
          else
          {
            hi = mid;
          }
        }

        if (!found || halves.size() < 2u)
        {
          continue;
        }

        // Rank on the worse of the two halves: one good room and one wedge is
        // a bad cut, and an average would hide it.
        // Judge the cut on the pieces that are rooms, not on every fragment it
        // happens to produce.
        //
        // A chord across a concave piece can leave three or four parts, and
        // rejecting the cut because one of them is a sliver means an L-shaped
        // remainder has no acceptable cut at all: measured, an L-plan house
        // collapsed from thirteen rooms to one, its whole floor a single room
        // because every candidate produced one small fragment. The big pieces
        // are what the cut is for; the fragments are absorbed by not being
        // recursed on.
        std::sort(halves.begin(), halves.end(), [](const geom2::Polygon &l, const geom2::Polygon &r) {
          return geom2::areaMetres(l) > geom2::areaMetres(r);
        });

        double score = 1e300;
        bool usable = halves.size() >= 2u;

        for (size_t which = 0; which < 2u && usable; ++which)
        {
          const geom2::Polygon &half = halves[which];

          double halfU = 0, halfV = 0;
          extentOn(half, nx, ny, halfU, halfV);

          const double narrow = std::min(halfU, halfV), wide = std::max(halfU, halfV);

          if (geom2::areaMetres(half) < s.minimumRoomMetres || narrow < s.minimumWidthMetres)
          {
            usable = false;
            break;
          }

          const double fill = halfU * halfV > 1e-9 ? geom2::areaMetres(half) / (halfU * halfV) : 0.0;

          score = std::min(score, fill * (.45 + .55 * (wide > 1e-9 ? narrow / wide : 0.0)));
        }

        // A cut that takes a half off the corridor is worth far less than one
        // that leaves both halves on it.
        //
        // Carving the strip guarantees the first rank of rooms a wall on the
        // circulation and nothing more; everything cut after that can end up
        // behind another room, which is how coverage fell to two rooms in
        // seven. Scoring it makes the recursion slice *along* the corridor.
        // This only works because the corridor is carved before these cuts are
        // made - an earlier arrangement scored it while `corridors` was still
        // empty, and did nothing at all.
        if (!corridors.empty())
        {
          for (const geom2::Polygon &half : halves)
          {
            Position where;
            double run = 0;
            bool touches = false;

            for (const geom2::Polygon &corridor : corridors)
            {
              touches = touches || sharedRun(half.outer, corridor.outer, .9, where, run);
            }

            if (!touches)
            {
              score *= .25;
            }
          }
        }

        if (!usable || score <= chosenScore)
        {
          continue;
        }

        chosen = std::move(halves);
        chosenFrom = from;
        chosenTo = to;
        chosenScore = score;
      }

      // Nothing divides this piece well, so it stays one room. Two poor rooms
      // are worse than one good one.
      if (chosenScore < 0)
      {
        rooms.push_back(piece);

        return;
      }

      ++cuts;

      for (const geom2::Polygon &half : chosen)
      {
        if (geom2::areaMetres(half) >= s.minimumRoomMetres)
        {
          divide(half, depth + 1);
          continue;
        }

        // A fragment is still floor. Dropping it lost area - coverage fell
        // from 92 to 83 per cent - and, worse, left holes between rooms that
        // used to touch, so a room on an upper floor ended up sharing a wall
        // with nothing at all. Kept as a small room, it is a closet, which is
        // what a piece that size is.
        if (geom2::areaMetres(half) > 1e-6)
        {
          rooms.push_back(half);
        }
      }
    };

    // ---- the corridors: straight strips that span the plate ---------------
    //
    // A corridor is not a route between rooms, it is a piece of the building.
    //
    // An earlier version traced a path through the centres of a trial
    // subdivision - the graph's diameter - and carved a strip along it. The
    // numbers were acceptable and the plans were not: a polyline through
    // arbitrary centroids is a wiggle, and no building has a wiggling
    // corridor. Two other attempts failed the same way for the same reason,
    // that they treated circulation as a path found after the fact rather than
    // as part of the plan.
    //
    // Watabou's are strips. One runs the length of the plate, others cross it
    // at room-depth intervals, each spanning the footprint end to end, and the
    // rooms are the bays between them. Rooms in the middle of the plate then
    // touch two corridors and no outside wall at all, which is what an
    // internal room is - and there is no way to get one out of a subdivision
    // that only ever cuts a piece in two.
    {
      double loU = 1e300, hiU = -1e300, loV = 1e300, hiV = -1e300;

      for (const geom2::Point &p : plate.outer)
      {
        const double x = geom2::toMetres(p.x), y = geom2::toMetres(p.y);
        const double u = x * ux + y * uy, v = -x * uy + y * ux;

        loU = std::min(loU, u);
        hiU = std::max(hiU, u);
        loV = std::min(loV, v);
        hiV = std::max(hiV, v);
      }

      const double spanU = hiU - loU, spanV = hiV - loV;
      const double half = s.corridorWidthMetres * .5;
      const double roomDepth = std::sqrt(std::max(s.roomAreaMetres, 1.0));
      // Candidates every room depth, not every two. The budget below decides
      // which are affordable; offering them two room depths apart instead
      // meant a U-plan's were proposed in the void between its arms and never
      // in the arms themselves, and the plan had headroom it could not use.
      const double spacing = roomDepth + s.corridorWidthMetres;
      const double reach = spanU + spanV + 4.0;

      // The entrance in the footprint's own frame, so a cross corridor can be
      // put where the door is and the plan is entered through circulation
      // rather than through whichever room the door happens to fall in.
      const double entranceU = b.entrance.x * ux + b.entrance.z * uy;

      // A band of the corridor's width, centred on `where`, running along
      // `alongU` or across it, clipped to the building.
      const auto band = [&](const double where, const bool alongU, std::vector<geom2::Polygon> &out) {
        const double ax = alongU ? ux : -uy, ay = alongU ? uy : ux;
        const double nx = -ay, ny = ax;
        const double px = alongU ? -uy * where : ux * where;
        const double py = alongU ? ux * where : uy * where;

        geom2::Polygon strip;
        strip.outer = {
          at(px + nx * half - ax * reach, py + ny * half - ay * reach),
          at(px - nx * half - ax * reach, py - ny * half - ay * reach),
          at(px - nx * half + ax * reach, py - ny * half + ay * reach),
          at(px + nx * half + ax * reach, py + ny * half + ay * reach)};

        std::vector<geom2::Polygon> clipped;

        if (!geom2::booleanOp({geom2::toOperand(strip), geom2::toOperand(plate)}, geom2::Operation::Intersection, {}, clipped, nullptr))
        {
          return;
        }

        for (geom2::Polygon &piece : clipped)
        {
          // A corridor you cannot walk down is not a corridor.
          //
          // Clipping a band to the building leaves whatever the outline leaves:
          // a triangular stub in a corner, or a pinch where the strip crosses a
          // sloping wall at a shallow angle. Both are corridor by area and
          // neither has room to pass. The test is the one a person makes -
          // offset it inward by half a shoulder's width and see whether
          // anything survives - and it is a test about width, which area
          // cannot express.
          std::vector<geom2::Polygon> walkable;

          if (!geom2::offsetPolygon(piece, -s.minimumWalkableMetres * .5, {}, walkable, nullptr) || walkable.empty())
          {
            continue;
          }

          // ...and wide enough along its length, not merely somewhere.
          //
          // Surviving an inward offset says *some* part of the piece is wide
          // enough. A strip that is a metre and a half at one end and a
          // hand's breadth at the other survives it and cannot be walked: the
          // U-plan had a corridor 0.27 m across and the mansion one of 0.00.
          // Twice the area over the perimeter is the width of a strip - a
          // corridor of width w and length L has area wL and perimeter about
          // 2L - and it falls when any part of the piece pinches.
          double perimeter = 0;

          for (size_t i = 0; i < piece.outer.size(); ++i)
          {
            const double ax = geom2::toMetres(piece.outer[i].x), ay = geom2::toMetres(piece.outer[i].y);
            const double bx = geom2::toMetres(piece.outer[(i + 1) % piece.outer.size()].x), by = geom2::toMetres(piece.outer[(i + 1) % piece.outer.size()].y);

            perimeter += std::hypot(bx - ax, by - ay);
          }

          if (perimeter < 1e-6 || 2 * geom2::areaMetres(piece) / perimeter < s.minimumWalkableMetres)
          {
            continue;
          }

          out.push_back(std::move(piece));
        }
      };

      // Candidate strips, in the order they earn their place: the spine first,
      // then the crossing at the door, then the rest at intervals.
      std::vector<std::pair<double, bool>> candidates;

      if (geom2::areaMetres(plate) >= s.corridorFromAreaMetres && spanV >= 2 * s.minimumWidthMetres + s.corridorWidthMetres)
      {
        candidates.push_back({(loV + hiV) * .5, true});
      }

      if (geom2::areaMetres(plate) >= s.corridorBranchFromAreaMetres)
      {
        candidates.push_back({std::clamp(entranceU, loU + roomDepth, hiU - roomDepth), false});

        for (double u = loU + spacing; u < hiU - roomDepth * .5; u += spacing)
        {
          if (std::abs(u - entranceU) > roomDepth)
          {
            candidates.push_back({u, false});
          }
        }
      }

      // Taken one at a time, each kept only while the rooms still have most of
      // the plate. All or nothing was the wrong rule: an L-plan's cross bands
      // together took 35 per cent of the floor, so the whole set was rejected
      // and the house ended up with no corridor at all rather than with the
      // one or two it could afford.
      std::vector<geom2::Polygon> strips;
      std::vector<geom2::Polygon> bays{plate};

      const double plateArea = geom2::areaMetres(plate);

      for (const auto &[where, alongU] : candidates)
      {
        std::vector<geom2::Polygon> strip;
        band(where, alongU, strip);

        if (strip.empty())
        {
          continue;
        }

        std::vector<geom2::Operand> difference{geom2::toOperand(plate)};

        for (const geom2::Polygon &piece : strips)
        {
          difference.push_back(geom2::toOperand(piece));
        }

        for (const geom2::Polygon &piece : strip)
        {
          difference.push_back(geom2::toOperand(piece));
        }

        std::vector<geom2::Polygon> trial;

        if (!geom2::booleanOp(difference, geom2::Operation::Difference, {}, trial, nullptr) || trial.empty())
        {
          continue;
        }

        double kept = 0;

        for (const geom2::Polygon &bay : trial)
        {
          kept += geom2::areaMetres(bay) >= s.minimumRoomMetres ? geom2::areaMetres(bay) : 0.0;
        }

        if (kept < plateArea * .72)
        {
          continue;
        }

        for (geom2::Polygon &piece : strip)
        {
          strips.push_back(std::move(piece));
        }

        bays = std::move(trial);
      }

      if (!strips.empty())
      {
        corridors = std::move(strips);

        for (const geom2::Polygon &bay : bays)
        {
          if (geom2::areaMetres(bay) >= s.minimumRoomMetres)
          {
            divide(bay, 0);
          }
        }
      }
    }

    if (rooms.empty())
    {
      divide(plate, 0);
      corridors.clear();
    }

    if (rooms.empty())
    {
      return fail(error, "floor produced no rooms");
    }

    // The carved strips join the room list, flagged.
    std::vector<bool> isCorridor(rooms.size(), false);

    for (geom2::Polygon &piece : corridors)
    {
      rooms.push_back(std::move(piece));
      isCorridor.push_back(true);
    }

    // A fragment that touches nothing is not a room.
    //
    // Keeping fragments preserves floor area and adjacency, but a cut can
    // still leave one wedged in a corner with no wall long enough to be a door
    // on any of its neighbours. Failing the whole plan over one - which is
    // what happened to an L-plan house - is the wrong answer; so is leaving it
    // stranded. It is dropped, and only a *room*-sized piece with no
    // neighbours is still an error, because that is a real fault in the cut.
    {
      std::vector<geom2::Polygon> kept;
      std::vector<bool> keptCorridor;

      for (size_t i = 0; i < rooms.size(); ++i)
      {
        bool touches = false;

        for (size_t j = 0; j < rooms.size() && !touches; ++j)
        {
          if (j == i)
          {
            continue;
          }

          Position where;
          double run = 0;
          touches = sharedRun(rooms[i].outer, rooms[j].outer, .9, where, run);
        }

        if (touches || geom2::areaMetres(rooms[i]) >= s.minimumRoomMetres)
        {
          kept.push_back(std::move(rooms[i]));
          keptCorridor.push_back(isCorridor[i]);
        }
      }

      rooms = std::move(kept);
      isCorridor = std::move(keptCorridor);
    }

    if (rooms.empty())
    {
      return fail(error, "floor produced no rooms that touch anything");
    }

    std::vector<double> areas(rooms.size());

    for (size_t i = 0; i < rooms.size(); ++i)
    {
      areas[i] = geom2::areaMetres(rooms[i]);
    }

    // ---- purposes, fitted to the rooms that exist -------------------------
    //
    // Shape first, purpose second: the rooms are already cut, and the
    // programme is matched to them rather than the other way round. The room
    // nearest the entrance is the one a visitor arrives in.
    std::vector<size_t> order(rooms.size());

    for (size_t i = 0; i < rooms.size(); ++i)
    {
      order[i] = i;
    }

    order.erase(std::remove_if(order.begin(), order.end(), [&](const size_t i) { return isCorridor[i]; }), order.end());

    std::sort(order.begin(), order.end(), [&](const size_t a, const size_t c) {
      return areas[a] != areas[c] ? areas[a] > areas[c] : a < c;
    });

    if (order.empty())
    {
      return fail(error, "the corridor took the whole floor");
    }

    const auto centreOf = [&](const size_t i) {
      geom2::Point c;
      geom2::centroid(rooms[i], c);

      return Position{geom2::toMetres(c.x), 0, geom2::toMetres(c.y)};
    };

    // Where a visitor arrives: the corridor when the house has one, because
    // that is what a front door opens onto, and otherwise the nearest room.
    const bool hasCorridor = std::any_of(isCorridor.begin(), isCorridor.end(), [](const bool c) { return c; });

    size_t arrival = 0;
    double nearest = 1e300;

    for (size_t i = 0; i < rooms.size(); ++i)
    {
      if (hasCorridor && !isCorridor[i])
      {
        continue;
      }

      geom2::Point centre;

      if (!geom2::centroid(rooms[i], centre))
      {
        continue;
      }

      const double distance = std::hypot(geom2::toMetres(centre.x) - b.entrance.x, geom2::toMetres(centre.y) - b.entrance.z);

      if (distance < nearest)
      {
        nearest = distance;
        arrival = i;
      }
    }

    // The programme: a list of the rooms this building needs, then fitted to
    // the rooms it has.
    //
    // "First it creates floor plans made of rooms. Then it builds a list of
    // room types this house needs" - Dwellings, and the author is explicit
    // that the second stage sometimes fails, because "each room type has its
    // requirements (e.g. a bedroom can't be placed in the basement)". Ranking
    // rooms by area and handing out purposes down the list is not that: it
    // cannot say a house needs two bedrooms and one kitchen, and it produced a
    // mansion whose every room was "service".
    //
    // So the list is built first, proportional to the floor and its storey,
    // and each entry is matched to the best room that meets it. What is left
    // over is generic - which is the honest version of "sometimes fails".
    struct Want
    {
      RoomUse use = RoomUse::Living;
      double wantsAtLeast = 0;  // Square metres.
      bool wantsTheDoor = false; // Should be the room a visitor arrives in.
      bool wantsTheLargest = false;
    };

    std::vector<Want> programme;

    const size_t rank = order.size();
    const bool onlyStorey = storeyPlan.size() == 1u;

    // The filler: what a floor is made of once its named rooms are placed.
    //
    // Pushing bedrooms until the rooms run out is not a programme, it is a
    // default - and it showed: a mansion's ground floor came out with fifteen
    // bedrooms and one of everything else. Bedrooms belong upstairs unless
    // there is no upstairs, and a ground floor is living space and service.
    // Cycling through a list per floor keeps the mix proportional however many
    // rooms the subdivision happens to cut.
    std::vector<RoomUse> filler;

    switch (b.use)
    {
    case rewrite::DwellingUse::ShopHouse:
      if (floor == 0u)
      {
        programme.push_back({RoomUse::Shop, 14, true, false});
        programme.push_back({RoomUse::Shop, 10, false, false});
        programme.push_back({RoomUse::Store, 8, false, false});

        filler = onlyStorey ? std::vector<RoomUse>{RoomUse::Store, RoomUse::Living, RoomUse::Sleeping, RoomUse::Service}
                            : std::vector<RoomUse>{RoomUse::Store, RoomUse::Service, RoomUse::Shop};
      }
      else
      {
        programme.push_back({RoomUse::Living, 14, false, true});
        programme.push_back({RoomUse::Service, 7, false, false});

        filler = {RoomUse::Sleeping, RoomUse::Sleeping, RoomUse::Store};
      }
      break;

    case rewrite::DwellingUse::Warehouse:
      programme.push_back({RoomUse::Store, 0, false, true});

      filler = {RoomUse::Store, RoomUse::Store, RoomUse::Service};
      break;

    case rewrite::DwellingUse::Hall:
      if (floor == 0u)
      {
        programme.push_back({RoomUse::Hall, 0, false, true});
        programme.push_back({RoomUse::Service, 12, false, false});
        programme.push_back({RoomUse::Store, 8, false, false});
        programme.push_back({RoomUse::Common, 12, false, false});

        // A hall's ground floor is reception and service. The household
        // sleeps above it, and only sleeps here when there is no above.
        filler = onlyStorey ? std::vector<RoomUse>{RoomUse::Sleeping, RoomUse::Service, RoomUse::Common, RoomUse::Store}
                            : std::vector<RoomUse>{RoomUse::Common, RoomUse::Service, RoomUse::Store, RoomUse::Common};
      }
      else
      {
        programme.push_back({RoomUse::Common, 0, false, true});
        programme.push_back({RoomUse::Service, 7, false, false});

        filler = {RoomUse::Sleeping, RoomUse::Sleeping, RoomUse::Sleeping, RoomUse::Common};
      }
      break;

    case rewrite::DwellingUse::Lodging:
      if (floor == 0u)
      {
        programme.push_back({RoomUse::Common, 0, false, true});
        programme.push_back({RoomUse::Service, 10, false, false});
        programme.push_back({RoomUse::Store, 7, false, false});

        // A lodging house does take guests on the ground floor, but it is also
        // where the kitchen and the taproom are.
        filler = {RoomUse::Sleeping, RoomUse::Sleeping, RoomUse::Service, RoomUse::Common};
      }
      else
      {
        filler = {RoomUse::Sleeping, RoomUse::Sleeping, RoomUse::Sleeping, RoomUse::Store};
      }
      break;

    default:
      if (floor == 0u)
      {
        programme.push_back({RoomUse::Living, 14, true, true});
        programme.push_back({RoomUse::Service, 8, false, false});
        programme.push_back({RoomUse::Store, 6, false, false});

        filler = onlyStorey ? std::vector<RoomUse>{RoomUse::Sleeping, RoomUse::Sleeping, RoomUse::Living, RoomUse::Store}
                            : std::vector<RoomUse>{RoomUse::Living, RoomUse::Service, RoomUse::Store, RoomUse::Common};
      }
      else
      {
        programme.push_back({RoomUse::Living, 12, false, true});

        filler = {RoomUse::Sleeping, RoomUse::Sleeping, RoomUse::Sleeping, RoomUse::Store};
      }
      break;
    }

    if (filler.empty())
    {
      filler.push_back(RoomUse::Common);
    }

    for (size_t k = programme.size(); k < rank; ++k)
    {
      programme.push_back({filler[(k - programme.size() + k) % filler.size()], 0, false, false});
    }

    // Fit each entry to the best room that meets it. Largest first, so the
    // rooms that carry a requirement get one before the generic ones do.
    std::vector<RoomUse> uses(rooms.size(), RoomUse::Common);
    std::vector<bool> taken(rooms.size(), false);

    for (size_t i = 0; i < rooms.size(); ++i)
    {
      if (isCorridor[i])
      {
        uses[i] = RoomUse::Corridor;
        taken[i] = true;
      }
    }

    size_t unmet = 0;

    for (const Want &want : programme)
    {
      size_t best = rooms.size();
      double bestScore = -1;

      for (const size_t i : order)
      {
        if (taken[i] || areas[i] < want.wantsAtLeast)
        {
          continue;
        }

        // The room that suits the requirement, not merely the biggest free
        // one: a room asked for by the door is judged by how near the door it
        // is, and a room asked to be the largest by its area.
        double score = 1.0;

        if (want.wantsTheDoor)
        {
          score = i == arrival ? 1e6 : 1.0 / (1.0 + std::hypot(centreOf(i).x - b.entrance.x, centreOf(i).z - b.entrance.z));
        }
        else if (want.wantsTheLargest)
        {
          score = areas[i];
        }
        else
        {
          // Otherwise the smallest room that still meets the requirement, so
          // the big rooms are left for the entries that need them.
          score = 1.0 / std::max(areas[i], 1.0);
        }

        if (score > bestScore)
        {
          bestScore = score;
          best = i;
        }
      }

      if (best == rooms.size())
      {
        ++unmet;
        continue;
      }

      uses[best] = want.use;
      taken[best] = true;
    }

    if (unmet > 0 && out.note.empty())
    {
      out.note = std::to_string(unmet) + " room types found no room that met them";
    }

    // ---- the rooms, their walls and their furniture -----------------------
    DwellingFloor record;
    record.elevation = y;
    record.height = b.floorRise;
    record.grossArea = geom2::areaMetres(footprint);

    std::vector<uint64_t> ids(rooms.size());

    for (size_t i = 0; i < rooms.size(); ++i)
    {
      DwellingRoom room;
      room.id = roomId++;
      room.floor = floor;
      room.use = uses[i];
      room.usableArea = areas[i];

      for (const geom2::Point &p : rooms[i].outer)
      {
        room.boundary.push_back(Position{geom2::toMetres(p.x), y, geom2::toMetres(p.y)});
      }

      ids[i] = room.id;
      (isCorridor[i] ? record.circulationArea : record.roomArea) += room.usableArea;

      if (room.use == RoomUse::Sleeping && !isCorridor[i])
      {
        out.residentCapacity += uint32_t(room.usableArea / (b.use == rewrite::DwellingUse::Lodging ? 4. : 6.));
      }

      if (room.use == RoomUse::Shop || room.use == RoomUse::Hall || room.use == RoomUse::Store)
      {
        out.workerCapacity += uint32_t(room.usableArea / 8.);
      }

      furnish(rooms[i], room.id, floor, room.use, y, ux, uy, b.seed, out.furniture);

      for (size_t k = 0; k < rooms[i].outer.size(); ++k)
      {
        const geom2::Point &p = rooms[i].outer[k];
        const geom2::Point &q = rooms[i].outer[(k + 1) % rooms[i].outer.size()];

        DwellingWall wall;
        wall.id = wallId++;
        wall.floor = floor;
        wall.a = Position{geom2::toMetres(p.x), y, geom2::toMetres(p.y)};
        wall.b = Position{geom2::toMetres(q.x), y, geom2::toMetres(q.y)};
        wall.thickness = s.wallThickness;
        wall.exterior = !geom2::contains(plate, at((geom2::toMetres(p.x) + geom2::toMetres(q.x)) * .5, (geom2::toMetres(p.y) + geom2::toMetres(q.y)) * .5));

        out.walls.push_back(std::move(wall));
      }

      out.rooms.push_back(std::move(room));
    }

    // ---- doors: from the arrival room outward over shared walls -----------
    //
    // Breadth first, so every room is reached and the door that reaches it is
    // on a wall it actually shares with a room already reached. A room that
    // cannot be reached at all is reported rather than left stranded.
    std::vector<int> reached(rooms.size(), -1);
    std::vector<size_t> frontier{arrival};

    reached[arrival] = int(arrival);

    while (!frontier.empty())
    {
      std::vector<size_t> next;

      for (const size_t from : frontier)
      {
        for (size_t to = 0; to < rooms.size(); ++to)
        {
          if (reached[to] >= 0)
          {
            continue;
          }

          Position centre;
          double run = 0;

          if (!sharedRun(rooms[from].outer, rooms[to].outer, .9, centre, run))
          {
            continue;
          }

          reached[to] = int(from);
          next.push_back(to);

          DwellingDoor door;
          door.room = ids[to];
          door.floor = floor;
          door.center = Position{centre.x, y, centre.z};
          door.width = std::min(1.0, run * .6);
          door.privateAccess = uses[to] == RoomUse::Sleeping;

          out.doors.push_back(std::move(door));
        }
      }

      frontier = std::move(next);
    }

    for (size_t i = 0; i < rooms.size(); ++i)
    {
      if (reached[i] < 0)
      {
        return fail(error, "a room on floor " + std::to_string(floor) + " shares no wall with any other");
      }
    }

    // The front door, on the arrival room.
    if (floor == 0)
    {
      DwellingDoor entrance;
      entrance.room = ids[arrival];
      entrance.floor = 0;
      entrance.center = b.entrance;
      entrance.width = 1.0;
      entrance.exterior = true;

      out.doors.push_back(std::move(entrance));
    }

    // ---- the stair, in a corner of the largest room -----------------------
    //
    // Furniture, not a room: a reserved core is what a rectangular wing can
    // afford and an awkward footprint cannot.
    if (floor + 1 < b.floors)
    {
      const size_t host = order.front(); // The largest room; order excludes corridors.

      double loU = 1e300, hiU = -1e300, loV = 1e300, hiV = -1e300;

      for (const geom2::Point &p : rooms[host].outer)
      {
        const double x = geom2::toMetres(p.x), z = geom2::toMetres(p.y);
        const double u = x * ux + z * uy, v = -x * uy + z * ux;

        loU = std::min(loU, u);
        hiU = std::max(hiU, u);
        loV = std::min(loV, v);
        hiV = std::max(hiV, v);
      }

      const double side = std::min({1.9, hiU - loU - .4, hiV - loV - .4});

      if (side > 1.2)
      {
        const auto toWorld = [&](const double u, const double v, const double height) {
          return Position{u * ux - v * uy, height, u * uy + v * ux};
        };

        const auto square = [&](const double height) {
          return std::vector<Position>{
            toWorld(hiU - side - .2, loV + .2, height),
            toWorld(hiU - .2, loV + .2, height),
            toWorld(hiU - .2, loV + side + .2, height),
            toWorld(hiU - side - .2, loV + side + .2, height)};
        };

        DwellingStair stair;
        stair.opening = square(y + b.floorRise);
        stair.lowerLanding = square(y);
        stair.upperLanding = square(y + b.floorRise);
        stair.lowerFloor = floor;
        stair.risers = uint32_t(std::ceil(b.floorRise / .18));
        stair.rise = b.floorRise;
        stair.width = std::min(1.0, side * .5);
        stair.privateAccess = true;

        out.stairs.push_back(std::move(stair));

        record.coreArea += side * side;
      }
    }

    record.wallArea = std::max(0., record.grossArea - record.roomArea - record.circulationArea - record.coreArea);
    out.floors.push_back(std::move(record));
  }

  massBuilding(b.shell, b.foundation, b.floorRise, uint32_t(storeyPlan.size()), s, out.wings, out.roofs, &out.storeys);


  if ((b.use == rewrite::DwellingUse::Cottage || b.use == rewrite::DwellingUse::ShopHouse || b.use == rewrite::DwellingUse::Lodging) && out.residentCapacity == 0u)
  {
    // A one room cottage sleeps its household in the room it cooks in; the
    // label was missing, not the bed.
    out.residentCapacity = 1u;
  }

  return true;
}

} // namespace procedural::world::generator
