#include "ComplexDwellingPlanner.hpp"

#include "DwellingStairs.hpp"

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
#include <cstdlib>
#include <cstdio>
#include <map>
#include <set>
#include <functional>

namespace procedural::world::generator {

using rewrite::DwellingBrief;
using rewrite::DwellingDoor;
using rewrite::DwellingFloor;
using rewrite::DwellingPlan;
using rewrite::DwellingRoom;
using rewrite::DwellingStair;
using rewrite::DwellingWall;
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
// How far a point is from a ring, in metres.
//
// Asked of a wall's midpoint against a storey's outline, to decide whether the
// wall is on the outside of the building. A containment test cannot answer it:
// the wall is a boundary of a piece cut out of that outline, so it is inside
// the outline whichever side of the building it is on, and a point exactly on
// the line is inside by convention. Distance is the question.
double distanceToRing(const geom2::Ring &ring, const double x, const double z)
{
  double nearest = 1e300;

  for (size_t i = 0; i < ring.size(); ++i)
  {
    const geom2::Point &p = ring[i];
    const geom2::Point &q = ring[(i + 1) % ring.size()];

    const double ax = geom2::toMetres(p.x), az = geom2::toMetres(p.y);
    const double bx = geom2::toMetres(q.x), bz = geom2::toMetres(q.y);
    const double ex = bx - ax, ez = bz - az;
    const double length = ex * ex + ez * ez;

    const double t = length < 1e-12 ? 0 : std::clamp(((x - ax) * ex + (z - az) * ez) / length, 0., 1.);

    nearest = std::min(nearest, std::hypot(ax + ex * t - x, az + ez * t - z));
  }

  return nearest;
}

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

  // The ground a set of wings covers, in as many pieces as it comes in.
  const auto unionOfWings = [&](const std::vector<size_t> &members, std::vector<geom2::Polygon> &merged) {
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

    return !operands.empty() && geom2::unionOf(operands, merged, nullptr) && !merged.empty();
  };

  // A cluster has to be one connected mass, and the grouping cannot guarantee
  // it.
  //
  // Adjacency is tested point against segment, which is right - at a T the
  // stem's end wall lands in the middle of the bar's side and the two share no
  // vertex - but it also passes for two wings that meet at a single corner.
  // Their union is then still two polygons, and the roof was built over
  // whichever was larger: the other wing kept its walls, lost its roof, and
  // stood open to the sky. A tower notched out of a corner produces exactly
  // that arrangement, so it stopped being rare.
  //
  // So each cluster is split into the pieces its ground actually comes in,
  // and each piece is a cluster. Nothing downstream changes, because this is
  // what a cluster already claimed to be.
  {
    std::map<uint32_t, std::vector<size_t>> initial;

    for (size_t i = 0; i < wings.size(); ++i)
    {
      if (wings[i].footprint.size() >= 3)
      {
        initial[wings[i].cluster].push_back(i);
      }
    }

    uint32_t next = 0;

    for (const auto &[id, members] : initial)
    {
      std::vector<geom2::Polygon> merged;

      if (!unionOfWings(members, merged) || merged.size() < 2u)
      {
        const uint32_t label = next++;

        for (const size_t i : members)
        {
          wings[i].cluster = label;
        }

        continue;
      }

      std::vector<uint32_t> labels;

      for (size_t k = 0; k < merged.size(); ++k)
      {
        labels.push_back(next++);
      }

      for (const size_t i : members)
      {
        geom2::Polygon piece;

        for (const Position &q : wings[i].footprint)
        {
          piece.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
        }

        geom2::Point centre;
        uint32_t label = labels.front();

        if (geom2::centroid(piece, centre))
        {
          for (size_t k = 0; k < merged.size(); ++k)
          {
            label = geom2::contains(merged[k], centre) ? labels[k] : label;
          }
        }

        wings[i].cluster = label;
      }
    }
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
    std::vector<geom2::Polygon> merged;

    if (!unionOfWings(members, merged))
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

  // How high each mass's wall stands, and which of them are towers - both
  // asked of one mass about another, below.
  std::map<uint32_t, double> clusterEaves;
  std::map<uint32_t, bool> clusterTower;

  for (const auto &[id, members] : clusters)
  {
    geom2::Polygon ground;

    if (groundOf(members, ground))
    {
      // And again on the union, for a reason the wings cannot cover.
      //
      // Two wings cleaned apart no longer share an edge exactly: each settled
      // its own copy of the party wall a few micrometres from the other's. The
      // union of the two then carries a sliver down the join - debris made by
      // the repair rather than surviving it - and this is the ring the eaves
      // amplify, so it is the ring that has to be clean.
      // Two centimetres is below what any drawing of a building resolves; the
      // overhang is the length at which the eaves stop being able to tell a
      // wall from a notch, so it is the length below which an edge is not one.
      geom2::simplifyPolygon(ground, .02, overhang);

      grounds[id] = std::move(ground);
      clusterEaves[id] = wings[members.front()].eavesHeight;
      clusterTower[id] = wings[members.front()].tower;
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
    // A party wall needs no overhang - the roof rises to it and stops - so the
    // eaves polygon is the ground with every *free* wall pushed out and every
    // shared one left where it is.
    //
    // Two ways of getting there have already failed, and both failed the same
    // way, because both were booleans. Growing the mass and subtracting the
    // neighbours grown by the same amount left ledges one overhang deep where
    // the subtraction had cut and the union had put back. Growing it and
    // cutting on the *line* of each shared wall was worse in a different
    // direction: the line through the two metre side of a tower notch runs
    // the width of the building, so the cut took a quarter of a hall with it
    // and left its walls standing under open sky. Cutting a strip the length
    // of the wall instead fixed the hole and brought the ledges back - a manor
    // arrived with a hundred and fifty six roof faces where it had forty nine.
    //
    // Every step in that outline is a roof face. So the outline is not cut at
    // all: each wall's *line* is pushed out by its own distance, and the
    // corners are where consecutive pushed lines cross. One vertex per corner
    // of the ground, one face per wall, and nothing a boolean can scallop.
    if (overhang > 0 && ground.outer.size() >= 3u)
    {
      const size_t count = ground.outer.size();

      std::vector<double> ax(count), az(count), ux(count), uz(count), push(count);

      for (size_t i = 0; i < count; ++i)
      {
        ax[i] = geom2::toMetres(ground.outer[i].x);
        az[i] = geom2::toMetres(ground.outer[i].y);
      }

      for (size_t i = 0; i < count; ++i)
      {
        const size_t j = (i + 1) % count;
        const double length = std::hypot(ax[j] - ax[i], az[j] - az[i]);

        ux[i] = length > 1e-9 ? (ax[j] - ax[i]) / length : 1;
        uz[i] = length > 1e-9 ? (az[j] - az[i]) / length : 0;

        // Does this wall stand against another mass? Sampled along the wall,
        // because at a T the neighbour meets the middle of it and shares no
        // vertex with it at all.
        bool shared = false;

        for (const auto &[other, polygon] : grounds)
        {
          if (other == id || shared)
          {
            continue;
          }

          for (const double at : {.25, .5, .75})
          {
            const double px = ax[i] + ux[i] * length * at, pz = az[i] + uz[i] * length * at;

            for (size_t k = 0; k < polygon.outer.size() && !shared; ++k)
            {
              const double cx = geom2::toMetres(polygon.outer[k].x), cz = geom2::toMetres(polygon.outer[k].y);
              const double dx = geom2::toMetres(polygon.outer[(k + 1) % polygon.outer.size()].x), dz = geom2::toMetres(polygon.outer[(k + 1) % polygon.outer.size()].y);
              const double ex = dx - cx, ez = dz - cz;
              const double len2 = ex * ex + ez * ez;
              const double t = len2 > 1e-12 ? std::min(1.0, std::max(0.0, ((px - cx) * ex + (pz - cz) * ez) / len2)) : 0;

              shared = std::hypot(cx + ex * t - px, cz + ez * t - pz) < .2;
            }
          }
        }

        // A wall too short to sample honestly keeps whatever its neighbours
        // decide, which the intersection below gives it for nothing.
        push[i] = shared || length < .5 ? 0.0 : overhang;
      }

      // Each wall's line, pushed outward by its own distance, and the corner
      // between two of them where they cross. Counter clockwise, so the
      // outward normal of edge i is (uz, -ux).
      geom2::Polygon eaves;
      bool sound = true;

      for (size_t i = 0; i < count && sound; ++i)
      {
        const size_t h = (i + count - 1) % count;

        const double px = ax[h] + uz[h] * push[h], pz = az[h] - ux[h] * push[h];
        const double qx = ax[i] + uz[i] * push[i], qz = az[i] - ux[i] * push[i];

        const double cross = ux[h] * uz[i] - uz[h] * ux[i];

        if (std::abs(cross) < 1e-6)
        {
          // The two walls run the same way: the corner is a straight through
          // and the pushed point is the corner.
          eaves.outer.push_back(at(qx, qz));

          continue;
        }

        const double t = ((qx - px) * uz[i] - (qz - pz) * ux[i]) / cross;

        eaves.outer.push_back(at(px + ux[h] * t, pz + uz[h] * t));
      }

      // A deep reflex corner with a wide overhang can fold the outline back
      // over itself. The ground is always a legitimate eaves; a folded ring
      // is not.
      sound = sound && eaves.outer.size() == count && geom2::polygonFault(eaves).empty() && geom2::areaMetres(eaves) >= geom2::areaMetres(ground) - 1e-6;

      if (sound)
      {
        held.push_back(std::move(eaves));
        eavesPoly = &held.back();
      }
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

    // How far the roof rises across the mass it covers. The whole cluster
    // shares one pitch, which is what makes the surface continuous - and the
    // pitch itself is the lead wing's, because a hall and a tower are steeper
    // than the range beside them and a constant here cannot say so.
    const double pitch = lead.roofPitch > 0 ? lead.roofPitch : .34;

    // ...and what it rises across is a range, not a bounding box.
    //
    // `across` is the narrow extent of the whole cluster, which for an L or a
    // U is the diagonal of the shape and not the span of anything the roof
    // has to cover: the arms of an L nine metres deep gave a mass fifteen
    // metres across and a ridge half as tall again as the building under it.
    // A roof spans a range at a time. The deepest wing in the cluster is that
    // span, and the skeleton could not run the surface higher than it in any
    // case, because the ridge sits over the middle of the widest range.
    double deepest = 0;

    for (const size_t i : members)
    {
      geom2::Polygon piece;

      for (const Position &q : wings[i].footprint)
      {
        piece.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
      }

      if (piece.outer.size() < 3u)
      {
        continue;
      }

      double spanA = 0, spanB = 0;
      extentOn(piece, ux, uz, spanA, spanB);

      deepest = std::max(deepest, std::min(spanA, spanB));
    }

    double rise = std::max(1.2, (deepest > 1.0 ? std::min(across, deepest) : across) * pitch);
    const double eaves = lead.eavesHeight;

    // A lower range cannot ridge above the wall it leans on.
    //
    // Nothing stopped it, and on a steep pitch nothing had to: the one storey
    // service end of a hall house ridged at six metres four against a hall
    // whose eaves were at five six, so the low roof ran *into* the tall wall
    // and the two surfaces cut through each other. In the render it reads as
    // a slot in the roof with the building open behind it, which is exactly
    // what it is.
    //
    // Where a roof meets a taller mass it stops at that mass's wall, so its
    // ridge has to sit under that mass's eaves. Squeezed flat enough, it is a
    // lean-to, which is the right answer and the shape a real one has.
    {
      double ceiling = 1e300;

      for (const auto &[other, polygon] : grounds)
      {
        const auto height = clusterEaves.find(other);
        const auto isTower = clusterTower.find(other);

        if (other == id || height == clusterEaves.end() || height->second <= eaves + .05)
        {
          continue;
        }

        // ...but not a tower. A tower is not a wall the roof leans on, it is a
        // shaft that goes *through* it: the roof around it carries on at its
        // own pitch and dies into the shaft. Counted as a tall neighbour, a
        // three storey tower flattened the hall it stands on from a ridge of
        // eleven metres six to one of eight two, which is the opposite of what
        // a tower is for.
        if (isTower != clusterTower.end() && isTower->second)
        {
          continue;
        }

        // Corner to *edge*, not corner to corner.
        //
        // This compared every vertex of one outline against every vertex of the
        // other, and two rectangles that share a whole face have no vertex in
        // common unless they happen to be the same width: a cross wing eight
        // metres across joining a range twenty two metres long meets it along a
        // line whose ends are six and sixteen metres from the range's own
        // corners. So the wing did not abut anything, the cap below never
        // applied, and a single storey wing ridged two metres above the eaves
        // of the two storey range it leans on - the pale chevron standing out
        // of the roof in every render of a cross winged building.
        bool abuts = false;

        const auto nearEdge = [](const double x, const double z, const std::vector<geom2::Point> &of, const double within) {
          for (size_t i = 0; i < of.size(); ++i)
          {
            const double ax = geom2::toMetres(of[i].x), az = geom2::toMetres(of[i].y);
            const double bx = geom2::toMetres(of[(i + 1) % of.size()].x), bz = geom2::toMetres(of[(i + 1) % of.size()].y);
            const double ex = bx - ax, ez = bz - az;
            const double run = ex * ex + ez * ez;
            const double at = run > 1e-12 ? std::max(0., std::min(1., ((x - ax) * ex + (z - az) * ez) / run)) : 0.;

            if (std::hypot(ax + ex * at - x, az + ez * at - z) < within)
            {
              return true;
            }
          }

          return false;
        };

        for (size_t k = 0; k < ring.size() && !abuts; ++k)
        {
          abuts = nearEdge(ring[k].first, ring[k].second, polygon.outer, overhang + .6);
        }

        for (size_t k = 0; k < polygon.outer.size() && !abuts; ++k)
        {
          const double qx = geom2::toMetres(polygon.outer[k].x), qz = geom2::toMetres(polygon.outer[k].y);

          for (size_t m = 0; m < ring.size() && !abuts; ++m)
          {
            const double ax = ring[m].first, az = ring[m].second;
            const double bx = ring[(m + 1) % ring.size()].first, bz = ring[(m + 1) % ring.size()].second;
            const double ex = bx - ax, ez = bz - az;
            const double run = ex * ex + ez * ez;
            const double at = run > 1e-12 ? std::max(0., std::min(1., ((qx - ax) * ex + (qz - az) * ez) / run)) : 0.;

            abuts = std::hypot(ax + ex * at - qx, az + ez * at - qz) < overhang + .6;
          }
        }

        ceiling = abuts ? std::min(ceiling, height->second) : ceiling;
      }

      rise = ceiling < 1e299 ? std::max(.6, std::min(rise, ceiling - eaves - .2)) : rise;
    }

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

    // A tower has no ends.
    //
    // The rule below gables the edges that stand against another mass, which
    // is right for a range - hipping into a join leaves a gutter down the
    // middle of a building with nowhere to fall. A tower stands against the
    // building at its foot and against nothing at its head, and the roof it
    // wants is a spire: four faces meeting at a point, hipped the whole way
    // round. Gabled on the side it touches, a square tower came out under a
    // ridge and read as a chimney stack.
    const bool spire = lead.tower;

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

        if (spire || (!meetsAnother(mx, mz) && !elongated && across >= hipFromWidth))
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

          if (std::getenv("GABLE_WHY") != nullptr)
          {
            std::fprintf(stderr, "GABLE abuts   %7.2f,%7.2f -> %7.2f,%7.2f (%.1f m)\n", a.first, a.second, b.first, b.second, elen);
          }

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

          // Facing away from the body is half the test. The other half is that
          // an end is *across* its wing, and without it a wing's flank is
          // gabled too.
          //
          // A wing that projects on the diagonal has its long side facing away
          // from the body as well as its end: grand-wings' left arm runs
          // twenty-nine metres up the z axis while projecting down and left, so
          // its flank scores .768 against a bar of .7 and is gabled over its
          // whole length. The arm then has no wall left to ridge between, the
          // roof falls across it from both sides, and it runs out over the very
          // wall it should have run along - a twenty-nine metre lean-to where a
          // ridge belongs. Its bottom arm goes the same way at .952.
          //
          // The body already gets this right one branch below: a wall along the
          // mass is a flank and a roof falls off its flanks. A wing needs the
          // same said about *its own* axis, not the body's - which is the same
          // correction the comment above this block describes making for which
          // wall counts as the end, carried the rest of the way.
          double wax = 1, waz = 0;
          {
            const auto &foot = wings[owner].footprint;
            double twice = 0, fx = 0, fz = 0;

            for (size_t k = 0; k < foot.size(); ++k)
            {
              const Position &p = foot[k], &q = foot[(k + 1) % foot.size()];
              const double cross = p.x * q.z - q.x * p.z;

              twice += cross;
              fx += (p.x + q.x) * cross;
              fz += (p.z + q.z) * cross;
            }

            if (std::abs(twice) > 1e-9)
            {
              fx /= 3 * twice;
              fz /= 3 * twice;

              double xx = 0, zz = 0, xz = 0;

              for (size_t k = 0; k < foot.size(); ++k)
              {
                const double px = foot[k].x - fx, pz = foot[k].z - fz;
                const double qx = foot[(k + 1) % foot.size()].x - fx, qz = foot[(k + 1) % foot.size()].z - fz;
                const double cross = px * qz - qx * pz;

                xx += (px * px + px * qx + qx * qx) * cross;
                zz += (pz * pz + pz * qz + qz * qz) * cross;
                xz += (px * qz + 2 * px * pz + 2 * qx * qz + qx * pz) * cross;
              }

              // The direction the wing's own area is most spread along. Its
              // longest *edge* will not do: a stub wider than it is deep has
              // its long edge across itself, which is the trap this file
              // already fell into once.
              const double angle = .5 * std::atan2(xz / 2, (xx - zz) * .5);

              wax = std::cos(angle);
              waz = std::sin(angle);
            }
          }

          const bool acrossWing = std::abs((ex / elen) * wax + (ez / elen) * waz) < .5;

          if (acrossWing && reach > 1e-6 && nx * (outx / reach) + nz * (outz / reach) > .7)
          {
            roofing.gables[i] = true;

            if (std::getenv("GABLE_WHY") != nullptr)
            {
              std::fprintf(stderr, "GABLE wingend %7.2f,%7.2f -> %7.2f,%7.2f (%.1f m)\n", a.first, a.second, b.first, b.second, elen);
            }
          }

          if (std::getenv("WING_WHY") != nullptr)
          {
            std::fprintf(stderr,
                         "WING members=%zu body=%zu owner=%zu  wall %6.2f,%6.2f->%6.2f,%6.2f (%.1f m)  wingC %6.2f,%6.2f  bodyC %6.2f,%6.2f  faces %.3f %s\n",
                         members.size(), body, owner, a.first, a.second, b.first, b.second, elen, wcx, wcz, bcx, bcz,
                         reach > 1e-6 ? nx * (outx / reach) + nz * (outz / reach) : -1.0,
                         roofing.gables[i] ? "GABLE" : "");
          }

          continue;
        }

        if (std::abs((ex / elen) * ux + (ez / elen) * uz) >= .5)
        {
          continue; // A flank of the body. A roof falls off its flanks.
        }

        // And a gable caps the narrow way.
        //
        // The test above asks whether the wall lies across the mass's axis, and
        // a mass that turns a corner does not have one. manor-U's body is the
        // whole U: its axis comes out along the 20.9 m bottom range, so both
        // long flanks of the left arm - 11.0 m and 10.0 m, facing each other
        // across 6.9 m - read as *ends* of it and both were gabled. A roof that
        // rises to both its flanks has nowhere to ridge and has to fold down the
        // middle of the leg instead, which is the "one side grows weird" on that
        // arm. The comment above records this same trap being fixed for wings;
        // the body kept a single axis, and a U has three.
        //
        // Depth is local and needs no axis: walk inward from the middle of the
        // wall and see how far the mass goes. An end is narrower than the mass
        // behind it - that is what makes it an end - and a flank is longer than
        // the mass is deep there. A square comes out all hips by this test,
        // which is right, and a 20x9 range still gables its 9 m ends.
        double depth = 1e300;
        {
          // Inward is to the *left* of the edge on a counter-clockwise ring;
          // ez,-ex (used above for the wing test) is the outward one.
          const double mnx = -ez / elen, mnz = ex / elen;

          for (size_t k = 0; k < ring.size(); ++k)
          {
            const auto &p = ring[k], &q = ring[(k + 1) % ring.size()];
            const double px = q.first - p.first, pz = q.second - p.second;
            const double denominator = mnx * pz - mnz * px;

            if (std::abs(denominator) < 1e-12)
            {
              continue;
            }

            const double rx = p.first - mx, rz = p.second - mz;
            const double t = (rx * pz - rz * px) / denominator;
            const double u = (rx * mnz - rz * mnx) / denominator;

            if (t > 1e-6 && u > -1e-9 && u < 1 + 1e-9)
            {
              depth = std::min(depth, t);
            }
          }
        }

        // A range gables its ends, and what makes a range is length *or*
        // narrowness, not length alone.
        //
        // Requiring both left a gap that an ordinary town house falls straight
        // into: eleven metres across and fifteen long is too broad to count as
        // "narrower than a hall" and not long enough to count as elongated, so
        // nothing gabled and the roof hipped on all four sides. On a rectangle
        // that is a hip with a four metre ridge; on a plot that is not
        // rectangular the ridge shrinks to a point and the four faces radiate
        // from it, which reads as a pyramid set down at an angle to the
        // building under it. A mass hips all round only when it is genuinely
        // broad as well as squat.
        roofing.gables[i] = (style == RoofStyle::Gabled || (style == RoofStyle::CrossGabled && acrossMass && (elongated || across < hipFromWidth)))
          && elen < depth;

        if (roofing.gables[i] && std::getenv("GABLE_WHY") != nullptr)
        {
          std::fprintf(stderr, "GABLE bodyend %7.2f,%7.2f -> %7.2f,%7.2f (%.1f m)\n", a.first, a.second, b.first, b.second, elen);
        }
      }

      // ...none of which applies to a tower.
      //
      // Every rule above is about a range: which end of it stands in open air,
      // which wall of it is shared, which way its wing points. A tower is not
      // a range. It is a square that meets the building at its foot and
      // nothing at its head, and its roof is a spire - four faces to a point,
      // hipped the whole way round. Gabled where it abuts, a square tower
      // came out under a ridge and read as a chimney stack.
      if (spire)
      {
        roofing.gables.assign(ring.size(), false);
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

    // A refused roof is a building with no roof on it, and it used to leave no
    // trace at all: the error was thrown away and the empty surface read as
    // "nothing to add". Say it, because the reasons are all about the plan this
    // planner handed over - an oblique reflex corner, a court that pinched, an
    // outline that came back from a boolean self-touching - and none of them is
    // visible from the hole it leaves.
    std::vector<geom2::RoofTriangle> surface;
    std::string roofWhy;

    if (!geom2::roofFromSkeleton(eavesShape, roofing, surface, &roofWhy))
    {
      std::fprintf(stderr, "ComplexDwellingPlanner: no roof over a mass of %zu walls: %s\n", ring.size(), roofWhy.c_str());

      // Say so on the wings, not only on stderr. A mass the roof refused is a
      // building that will stand bare, and that has to be legible downstream.
      for (const size_t m : members)
      {
        wings[m].roofRefused = true;
      }

      if (std::getenv("ROOF_RING") != nullptr)
      {
        std::fprintf(stderr, "  ring:");
        for (const auto &q : ring)
        {
          std::fprintf(stderr, " %.4f,%.4f", q.first, q.second);
        }
        std::fprintf(stderr, "\n");
      }
    }

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

  // Ground the building stands over but not on: an entrance bay, taken out of
  // the ground storey's plate and left in every plate above it.
  //
  // The arcade above does the same thing to a whole street front by cutting
  // the plate back along a line. This is the same cut asked for by shape
  // rather than by depth, which is what a bay is: a piece of the front, not
  // the whole of it.
  std::vector<geom2::Polygon> recesses;
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
        // ...outwards, and not over the wing next door.
        //
        // An offset grows a plate in every direction, and one of those
        // directions is inward over the neighbouring wing - so a two storey
        // range beside a single storey one oversailed that one's roof by the
        // jetty, and its wall then stood a whole storey above the tiles it was
        // standing on. Forty of the fifty three places in the corpus where a
        // wall came through a roof were this, and the worst of them measured
        // 2.90 m, which is exactly one floor.
        //
        // A jetty oversails the *outside*: the street, the yard, the air. What
        // is not outside is ground a shorter wing is already using, and that
        // ground is taken back off.
        std::vector<geom2::Operand> lower;

        for (const rewrite::DwellingWing &w : wings)
        {
          if (w.storeys > level || w.footprint.size() < 3)
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
            lower.push_back(geom2::toOperand(ground));
          }
        }

        if (!lower.empty())
        {
          std::vector<geom2::Polygon> kept;

          for (const geom2::Polygon &piece : grown)
          {
            std::vector<geom2::Polygon> left;

            if (geom2::differenceOf(geom2::toOperand(piece), lower, left, nullptr))
            {
              for (geom2::Polygon &part : left)
              {
                if (part.outer.size() >= 3u && geom2::areaMetres(part) > .25)
                {
                  kept.push_back(std::move(part));
                }
              }
            }
            else
            {
              kept.push_back(piece);
            }
          }

          if (!kept.empty())
          {
            grown = std::move(kept);
          }
        }

        plates = std::move(grown);
      }
    }

    // The entrance bay: a piece of the ground storey the building stands over
    // rather than on.
    //
    // Cut here rather than out of the mass, because the mass is right - the
    // upper floors do reach the street, and it is only the ground that stops
    // short. Cut before the arcade, so a building may have both: a recessed
    // doorway inside an arcaded front is a porch in an undercroft, which is a
    // real thing and not a contradiction.
    if (!style.recesses.empty() && level == 0)
    {
      std::vector<geom2::Operand> difference;

      for (const geom2::Polygon &plate : plates)
      {
        difference.push_back(geom2::toOperand(plate));
      }

      double before = 0;

      for (const geom2::Polygon &plate : plates)
      {
        before += geom2::areaMetres(plate);
      }

      for (const geom2::Polygon &bay : style.recesses)
      {
        if (bay.outer.size() >= 3u && geom2::polygonFault(bay).empty())
        {
          difference.push_back(geom2::toOperand(bay));
        }
      }

      std::vector<geom2::Polygon> left;

      if (difference.size() > plates.size() && geom2::booleanOp(difference, geom2::Operation::Difference, {}, left, nullptr) && !left.empty())
      {
        double after = 0;

        for (const geom2::Polygon &piece : left)
        {
          after += geom2::areaMetres(piece);
        }

        // A bay that eats the ground floor is not a bay. The same guard the
        // arcade carries, for the same reason: the recess is a decision about
        // the front of a building and must not become a decision about
        // whether the building has a ground floor.
        if (after > before * .55)
        {
          plates = std::move(left);
        }
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
    // A roof rises across its own span at the pitch the building was given.
    // Recomputed per cluster below; this is the wing on its own.
    wing.roofPitch = s.roofPitch;
    wing.ridgeHeight = wing.eavesHeight + std::min(spanU, spanV) * s.roofPitch;
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
    if (!geom2::polygonFault(piece).empty())
    {
      continue;
    }

    // Clipping a wing to the plot leaves debris on it, and the debris is not
    // harmless.
    //
    // A plot boundary is not axis aligned and it is not straight, so cutting a
    // rectangle against it produces a stair of vertices along the cut - pairs
    // a fraction of a millimetre apart, and chains of two and three centimetre
    // edges zigzagging across what is meant to be one wall. All of it passes
    // `polygonFault`, and none of it is visible in a plan.
    //
    // It becomes visible at the eaves. Every wall is pushed out by the
    // overhang and consecutive pushed lines are intersected, so a two
    // centimetre edge between two walls that nearly meet is levered into a
    // corner the best part of a metre across, turning at whatever angle the
    // debris happened to have. The roof is then asked to cover an outline with
    // reflex corners at forty degrees, which it refuses - correctly, since it
    // is a medial axis and those corners were never in the plan. Fifty four of
    // the town's four hundred and thirty three buildings went up bare that
    // way.
    //
    // So the wall is settled here, before anything is measured off it. The
    // overhang is the threshold for an edge because it is the length at which
    // the eaves stop being able to tell a wall from a notch; the tolerance is
    // a tenth of a wall, which no drawing of a building resolves.
    geom2::simplifyPolygon(piece, s.wallThickness * .1, s.eavesOverhangMetres);

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
void massBuilding(const std::vector<Position> &outline, const double foundation, const double floorRise, const uint32_t maximumStoreys, const ComplexDwellingSettings &s, std::vector<rewrite::DwellingWing> &wings, std::vector<rewrite::DwellingRoofFace> &roofs, std::vector<rewrite::DwellingStorey> *storeys, std::vector<rewrite::DwellingPillar> *pillars, const double jettyMetres, const double arcadeDepthMetres, const uint32_t minimumStoreys, const std::vector<MassingPart> *parts)
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

    // A part stands on ground the carve may not have - except a tower, which
    // stands on ground the carve keeps.
    //
    // A tower does not take a bite out of the building. It rises *through* it:
    // the range underneath is whatever it would have been, its roof runs over
    // its own full outline, and the shaft passes up through that roof and
    // carries on. Cut out of the mass instead, the tower leaves a notch, and
    // every notch is two more reflex corners for the roof to deal with - a
    // stub ridge along the lip beside the shaft, stepping up where the range
    // proper begins, and a gable on each side of the bite. That is the jog in
    // the ridge next to every tower, and none of it needs to exist.
    std::vector<geom2::Polygon> remainder{ground};

    if (parts != nullptr)
    {
      for (const MassingPart &part : *parts)
      {
        // A recess is not subtracted from the mass. The mass is what stands
        // over the ground, and an entrance bay has the building's upper floors
        // over it - it is the *ground storey's plate* that loses the bay, and
        // nothing else does.
        if (part.footprint.size() < 3u || part.tower || part.recess)
        {
          continue;
        }

        geom2::Polygon stand;

        for (const Position &q : part.footprint)
        {
          stand.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
        }

        if (!geom2::polygonFault(stand).empty())
        {
          continue;
        }

        std::vector<geom2::Operand> difference;

        for (const geom2::Polygon &piece : remainder)
        {
          difference.push_back(geom2::toOperand(piece));
        }

        difference.push_back(geom2::toOperand(stand));

        std::vector<geom2::Polygon> left;

        if (geom2::booleanOp(difference, geom2::Operation::Difference, {}, left, nullptr) && !left.empty())
        {
          remainder = std::move(left);
        }
      }
    }

    for (const geom2::Polygon &piece : remainder)
    {
      if (geom2::areaMetres(piece) >= s.minimumRoomMetres)
      {
        carve(piece, 0);
      }
    }

    wingsFromBlocks(blocks, ux, uy, foundation, floorRise, maximumStoreys, s, wings, minimumStoreys, uint64_t(outline.size()) * 1315423911ull + uint64_t(std::llround(std::abs(outline.front().x + outline.front().z) * 128)));

    // ...and then the parts themselves, each a wing of its own height.
    if (parts != nullptr)
    {
      for (const MassingPart &part : *parts)
      {
        if (part.footprint.size() < 3u || part.storeys == 0u || part.recess)
        {
          continue;
        }

        geom2::Polygon stand;

        for (const Position &q : part.footprint)
        {
          stand.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
        }

        geom2::Point centre;

        if (!geom2::centroid(stand, centre))
        {
          continue;
        }

        double spanU = 0, spanV = 0;
        extentOn(stand, ux, uy, spanU, spanV);

        const bool ridgeAlongU = spanU >= spanV;
        const double rx = ridgeAlongU ? ux : -uy, ry = ridgeAlongU ? uy : ux;
        const double half = std::max(0.0, (ridgeAlongU ? spanU : spanV) * .5 - s.wallThickness);
        const double cx = geom2::toMetres(centre.x), cy = geom2::toMetres(centre.y);

        rewrite::DwellingWing wing;
        wing.storeys = part.storeys;
        wing.tower = part.tower;
        wing.roofPitch = part.roofPitch > 0 ? part.roofPitch : s.roofPitch;
        wing.eavesHeight = foundation + double(part.storeys) * floorRise;
        wing.ridgeHeight = wing.eavesHeight + std::min(spanU, spanV) * wing.roofPitch;
        wing.ridgeFrom = Position{cx - rx * half, wing.ridgeHeight, cy - ry * half};
        wing.ridgeTo = Position{cx + rx * half, wing.ridgeHeight, cy + ry * half};
        wing.cluster = part.storeys;
        wing.footprint = part.footprint;

        for (Position &q : wing.footprint)
        {
          q.y = foundation;
        }

        wings.push_back(std::move(wing));
      }
    }
  }


  groupWingsIntoClusters(wings, s.wallThickness);
  buildRoofs(wings, s.eavesOverhangMetres, s.fullHeightWidthMetres, roofs, s.roofStyle);

  // A tower stands clear of the roofs it comes up through.
  //
  // Its height was a storey count - the building's floors and a couple more -
  // and a storey count knows nothing about the roof it has to get past. A hall
  // under a steep pitch ridges at eleven metres six while a three storey
  // tower's eaves are at eight four, so the spire begins *below* the ridge
  // around it and the two roofs cut through one another. The tower is the
  // part that is meant to be taller than everything else, so it takes however
  // many storeys that needs.
  //
  // Which means roofing twice, because the ridge to clear is not known until
  // the roof is built. That is the price of the answer being a measurement
  // rather than a guess, and it is paid only by the buildings that have a
  // tower.
  {
    double highest = 0;
    bool anyTower = false;

    for (const rewrite::DwellingWing &w : wings)
    {
      highest = w.tower ? highest : std::max(highest, w.ridgeHeight);
      anyTower = anyTower || w.tower;
    }

    bool raised = false;

    if (anyTower && floorRise > .1)
    {
      for (rewrite::DwellingWing &w : wings)
      {
        if (!w.tower)
        {
          continue;
        }

        const uint32_t clear = uint32_t(std::ceil((highest + .6 - foundation) / floorRise));

        if (clear > w.storeys)
        {
          w.storeys = clear;
          w.eavesHeight = foundation + double(clear) * floorRise;
          raised = true;
        }
      }
    }

    if (raised)
    {
      roofs.clear();
      groupWingsIntoClusters(wings, s.wallThickness);
      buildRoofs(wings, s.eavesOverhangMetres, s.fullHeightWidthMetres, roofs, s.roofStyle);
    }
  }

  if (storeys != nullptr)
  {
    // The street is taken as the outline's longest edge. The plot planner
    // knows which edges are really street and the carving path does not; the
    // longest edge is the same guess a lot makes when it is handed to a
    // generator on its own.
    StoreyStyle style;
    style.jetty = jettyMetres;
    style.arcade = arcadeDepthMetres;

    if (parts != nullptr)
    {
      for (const MassingPart &part : *parts)
      {
        if (!part.recess || part.footprint.size() < 3u)
        {
          continue;
        }

        geom2::Polygon bay;

        for (const Position &q : part.footprint)
        {
          bay.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
        }

        if (geom2::polygonFault(bay).empty())
        {
          style.recesses.push_back(std::move(bay));
        }
      }
    }

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

    // Enough ground to be a storey, not merely enough to be a room.
    //
    // The test was twice the smallest room - twelve square metres - which is a
    // landing and a cupboard. A storey is worth building when it holds a room
    // or two and the circulation to reach them, and that is what the area is
    // measured against.
    if (geom2::areaMetres(clipped.front()) < std::max(s.minimumStoreyAreaMetres, s.minimumRoomMetres * 2))
    {
      break;
    }

    storeyPlan.push_back(clipped.front());
  }

  const uint32_t builtFloors = uint32_t(storeyPlan.size());

  // ---- the two rooms the subdivision cannot produce -----------------------
  //
  // A hall and a tower are both decided here, before a single cut is made,
  // and both are invisible to the subdivision afterwards.
  //
  // They have to be. The rule that cuts a plate into rooms halves the largest
  // piece first, so it can never leave a room that is most of the building -
  // ask it for a hall and it gives you two rooms where the hall was. And it
  // works one floor at a time on that floor's own plate, so it has no way to
  // say "nothing may stand here, because the floor below is open to the roof
  // and the tower goes up through it". Both facts are about ground that is
  // spoken for, and ground that is spoken for is reserved, not cut.
  //
  // So: the tower's square and the hall's band come out of the plate, the
  // subdivision gets what is left, and the reserved pieces rejoin the room
  // list afterwards with their purposes already settled.

  // A building of trade has one big room on the ground floor and the household
  // over it, and the subdivision cannot produce that room for the reason it
  // cannot produce a hall: the first thing it does to a large piece is halve
  // it. A merchant's shop is the same reservation as a taproom - a band across
  // the front, at the door - and it differs in exactly two ways, so those two
  // are the only things said separately about it.
  //
  //   - It is one storey, not two. A hall is open to its roof because that is
  //     what a hall is; a shop has a household's floor over it, which is the
  //     whole of what makes it a shop-house.
  //   - It is a shop and not a hall, which the plan, the furniture and the
  //     shop front all read.
  // Two households, one to a floor, with no way between them inside.
  //
  // Not a use: it cuts across every use. A shop with a household over it is one
  // dwelling with a trade in it; the same building let to two families is two
  // dwellings that share a roof and nothing else. What differs is the plan.
  const bool tenement = b.households > 1u && b.floors > 1u;

  const bool trading = b.use == rewrite::DwellingUse::ShopHouse;
  const bool wantsHall = b.use == rewrite::DwellingUse::Hall || b.use == rewrite::DwellingUse::Tavern || trading;
  const RoomUse hallUse = trading ? RoomUse::Shop : RoomUse::Hall;

  // A hall building has a tower because that is where its stair goes; anything
  // else draws for one.
  const bool wantsTower = builtFloors + s.towerExtraStoreys >= 2u && (b.use == rewrite::DwellingUse::Hall || draw(b.seed, 0x704511u, b.id, 3u) < s.towerChance);

  // (u, v) on the building's own axes, and back again.
  const auto toPlan = [&](const double x, const double z, double &u, double &v) {
    u = x * ux + z * uy;
    v = -x * uy + z * ux;
  };

  const auto toGround = [&](const double u, const double v) {
    return at(u * ux - v * uy, u * uy + v * ux);
  };

  // Is `piece` wholly inside `host`? Asked of a square against a storey plate,
  // which is the only way to know a tower stands on the building rather than
  // half off the side of it.
  const auto within = [&](const geom2::Polygon &piece, const geom2::Polygon &host, const double slack) {
    std::vector<geom2::Polygon> left;

    if (!geom2::booleanOp({geom2::toOperand(piece), geom2::toOperand(host)}, geom2::Operation::Difference, {}, left, nullptr))
    {
      return false;
    }

    double outside = 0;

    for (const geom2::Polygon &part : left)
    {
      outside += geom2::areaMetres(part);
    }

    return outside <= slack;
  };

  // ---- the tower ----------------------------------------------------------
  //
  // A square at a corner of the ground plate, near the door, standing on
  // ground no floor may otherwise use. Every corner is tried in each of the
  // four directions a square can sit in one, and the ones that fall off the
  // building are the ones the containment test throws away - which is why
  // this works on an L or a wedge and not only on a rectangle.
  geom2::Polygon towerPlan;

  // The shaft as the rest of the building meets it: the plan grown by a wall,
  // which is the *outside* of the tower's masonry.
  //
  // What every other structure has to stop at. Cut against `towerPlan` instead
  // - the inner face - a joist or a rafter runs through the whole thickness of
  // the tower wall before it ends, so the frame is buried in the masonry and
  // bears on nothing. It should land on the wall, and the wall's outer face is
  // where landing on it means.
  geom2::Polygon shaftOutline;

  uint32_t towerStoreys = 0;

  if (wantsTower && s.towerSideMetres > 1.8)
  {
    const geom2::Polygon &plate = storeyPlan.front();
    const double plateArea = geom2::areaMetres(plate);
    const double side = s.towerSideMetres;

    double bestScore = -1;

    if (plateArea - side * side >= s.minimumRoomMetres * 2)
    {
      for (size_t corner = 0; corner < plate.outer.size(); ++corner)
      {
        // A tower takes a corner of the building, not a bite out of the
        // middle of it.
        //
        // Every vertex of the ring is a candidate anchor, and at a *reflex*
        // one - the inner corner of a U, where the court cuts in - the square
        // lands in the middle of the front wall with plan on three sides of
        // it. The hall around it is then a band with a deep notch, and the
        // roof over a notch that deep leaves a wedge of the building with
        // nothing over it. A convex corner is where a tower stands.
        {
          const geom2::Point &prev = plate.outer[(corner + plate.outer.size() - 1) % plate.outer.size()];
          const geom2::Point &here = plate.outer[corner];
          const geom2::Point &next = plate.outer[(corner + 1) % plate.outer.size()];

          const double ix = geom2::toMetres(here.x) - geom2::toMetres(prev.x), iz = geom2::toMetres(here.y) - geom2::toMetres(prev.y);
          const double ox = geom2::toMetres(next.x) - geom2::toMetres(here.x), oz = geom2::toMetres(next.y) - geom2::toMetres(here.y);

          if (ix * oz - iz * ox <= 0)
          {
            continue;
          }
        }

        double cu = 0, cv = 0;
        toPlan(geom2::toMetres(plate.outer[corner].x), geom2::toMetres(plate.outer[corner].y), cu, cv);

        for (const double du : {-1.0, 1.0})
        {
          for (const double dv : {-1.0, 1.0})
          {
            geom2::Polygon square;
            square.outer = {
              toGround(cu, cv),
              toGround(cu + du * side, cv),
              toGround(cu + du * side, cv + dv * side),
              toGround(cu, cv + dv * side)};

            // Wound the way every other ring here is.
            if (du * dv < 0)
            {
              std::reverse(square.outer.begin(), square.outer.end());
            }

            if (!geom2::polygonFault(square).empty() || !within(square, plate, side * side * .02))
            {
              continue;
            }

            // Half a side of it, at least, has to be an outside wall.
            //
            // Sitting a square at a corner of the ring puts it on the outside
            // of the building at a *point*, and at a reflex corner a point is
            // all it touches: the shaft then stands in the middle of the plan
            // with rooms all round it. The cost is not that it looks odd. The
            // ground it takes is subtracted from whatever else is reserved, so
            // a tower inside a hall leaves the hall a ring with a hole in it -
            // and a room is one ring, so the hole is dropped and the plan says
            // the hall covers the tower. A tower is a mass with outside walls;
            // one buried in a plan is a pillar.
            Position touch;
            double run = 0;

            if (!sharedRun(square.outer, plate.outer, side * .5, touch, run))
            {
              continue;
            }

            geom2::Point centre;

            if (!geom2::centroid(square, centre))
            {
              continue;
            }

            // Near the door, because a tower that carries the stair is
            // entered from the way in and not from the back of the house.
            // The jitter is what stops every building on a street putting its
            // tower on the same corner.
            const double reach = std::hypot(geom2::toMetres(centre.x) - b.entrance.x, geom2::toMetres(centre.y) - b.entrance.z);
            const double score = 1.0 / (1.0 + reach) + .12 * draw(b.seed, corner, uint64_t(du + 2), uint64_t(dv + 2));

            if (score > bestScore)
            {
              bestScore = score;
              towerPlan = square;
            }
          }
        }
      }
    }

    if (!towerPlan.outer.empty())
    {
      std::vector<geom2::Polygon> grown;

      shaftOutline = geom2::offsetPolygon(towerPlan, s.wallThickness, {}, grown, nullptr) && grown.size() == 1u ? grown.front() : towerPlan;

      towerStoreys = builtFloors + s.towerExtraStoreys;
    }
  }

  // ---- the entrance bay ---------------------------------------------------
  //
  // A piece of the ground floor given back to the street: the wall stops short
  // and the storey above it does not, so you stand under the building before
  // you are inside it. Timber-framed town houses do this at their doors and
  // their shop fronts both, and it is the single feature that most separates a
  // house you can walk up to from a box with a door painted on it.
  //
  // It is reserved before the subdivision for the reason the hall is, and one
  // more: the bay is where the posts carrying the overhang stand, and a post
  // in the middle of a parlour is a fault with no fix downstream. The ground
  // has to be spoken for before anything is cut on it.
  geom2::Polygon porchPlan; // Inside the walls, as rooms are.
  geom2::Polygon porchMass; // Out to the building's face, as the recess is.

  // Which way is out: the direction the bay is cut back from. The bay's box is
  // built on it; which of its walls are open is settled later and by a
  // different question - whether the ground storey kept the ground under them.
  double porchOutX = 0, porchOutZ = 1;

  // A bay needs a floor over it to be a bay rather than a notch, and it is a
  // town-house feature: a house of trade, where the ground floor meets the
  // public and the private house is over it.
  const bool wantsPorch = builtFloors >= 2u && (b.use == rewrite::DwellingUse::ShopHouse || b.use == rewrite::DwellingUse::Tavern);

  // The bay at a given share of the front, or nothing if that share does not
  // make a bay. Kept so the hall can ask for a different one.
  // The bay comes in two shapes at once: the room, cut inside the walls like
  // every other room, and the ground the storey gives up, which has to reach
  // the *outer* face or a skin of wall is left standing across the opening.
  struct Bay
  {
    geom2::Polygon plan, mass;
  };

  std::function<Bay(double, double)> bayOfShare = [](double, double) { return Bay{}; };

  if (wantsPorch && s.entranceDepthMetres > .5)
  {
    const geom2::Polygon &plate = storeyPlan.front();
    const double plateArea = geom2::areaMetres(plate);

    // The front is the edge of the plan the door is on. Not the longest edge
    // and not the one nearest the street the lot knows about - the brief fixes
    // the door, and the wall the door is in is the front by definition.
    size_t front = plate.outer.size();
    double nearest = 1e300, alongFront = 0, frontLength = 0;
    double dirX = 1, dirZ = 0, baseX = 0, baseZ = 0;

    for (size_t i = 0; i < plate.outer.size(); ++i)
    {
      const geom2::Point &p = plate.outer[i];
      const geom2::Point &q = plate.outer[(i + 1) % plate.outer.size()];

      const double ax = geom2::toMetres(p.x), az = geom2::toMetres(p.y);
      const double bx = geom2::toMetres(q.x), bz = geom2::toMetres(q.y);
      const double ex = bx - ax, ez = bz - az;
      const double length = std::hypot(ex, ez);

      if (length < 1e-6)
      {
        continue;
      }

      const double t = std::clamp(((b.entrance.x - ax) * ex + (b.entrance.z - az) * ez) / (length * length), 0., 1.);
      const double distance = std::hypot(ax + ex * t - b.entrance.x, az + ez * t - b.entrance.z);

      if (distance < nearest)
      {
        nearest = distance;
        front = i;
        frontLength = length;
        alongFront = t * length;
        dirX = ex / length;
        dirZ = ez / length;
        baseX = ax;
        baseZ = az;
      }
    }

    // Counter clockwise, so the outward normal of an edge is its direction
    // turned clockwise.
    porchOutX = dirZ;
    porchOutZ = -dirX;

    // How much of the front the bay takes is not settled here: a bay across
    // part of the front is what a shop has, and a bay across the whole of it
    // is what a hall has, because a notch in a hall leaves a room the roof
    // cannot cover. So the shape is a function of the share, and which share
    // is right is decided once the hall is known.
    // Everything by value, deliberately.
    //
    // The front wall - which edge it is, how long, which way it faces - is
    // worked out here and the bay is asked for later, once the trade room
    // behind it is known and has said how much depth it can spare. By then
    // this block has ended and every one of those locals is gone, so a lambda
    // that captured them by reference would read whatever the stack now holds.
    bayOfShare = [=](const double share, const double depth) {
      Bay made;

      const double width = std::min(frontLength - s.wallThickness * 2, std::max(s.entranceMinimumWidthMetres, frontLength * share));

      if (front >= plate.outer.size() || width < s.entranceMinimumWidthMetres)
      {
        return made;
      }

      {
      // Centred on the door, but kept on the wall it is in.
      const double centre = std::clamp(alongFront, width * .5, frontLength - width * .5);

      // Out past the plate's own edge, so the intersection reaches the wall
      // line exactly rather than leaving a slice of wall the width of a
      // rounding error - which would be a room the subdivision then tries to
      // cut, and fails on.
      const double out = s.wallThickness * 3;

      // A full-width bay runs past the ends of the wall as well.
      //
      // Clamping it to the wall's own run leaves a finger of plate a couple of
      // hundred millimetres wide at each front corner, and those two fingers
      // are not a rounding artefact - they are ground, so they widen the plate
      // back to the street, and the band chosen behind the bay then has to
      // reach forward past them to make its area. What comes out is a band
      // three quarters air, which is not a rectangle, which is not a room: the
      // hall is judged unfit and put at the back of the building. Two slivers
      // moved a taproom sixteen metres.
      const double past = width >= frontLength - s.wallThickness * 2 - 1e-6 ? out : 0.;

      const auto corner = [&](const double along, const double into) {
        return geom2::Point{
          geom2::toUnit(baseX + dirX * along + porchOutX * -into),
          geom2::toUnit(baseZ + dirZ * along + porchOutZ * -into)};
      };

      geom2::Polygon box;
      box.outer = {corner(centre - width * .5 - past, -out), corner(centre + width * .5 + past, -out), corner(centre + width * .5 + past, depth), corner(centre - width * .5 - past, depth)};

      if (geom2::areaMetres(box) < 0)
      {
        std::reverse(box.outer.begin(), box.outer.end());
      }

      std::vector<geom2::Polygon> bay;

      if (geom2::polygonFault(box).empty() && geom2::booleanOp({geom2::toOperand(box), geom2::toOperand(plate)}, geom2::Operation::Intersection, {}, bay, nullptr) && !bay.empty())
      {
        std::sort(bay.begin(), bay.end(), [](const geom2::Polygon &l, const geom2::Polygon &r) {
          return geom2::areaMetres(l) > geom2::areaMetres(r);
        });

        geom2::Polygon candidate = std::move(bay.front());

        // Clear of the tower: the shaft owns its ground on every floor, and a
        // bay that overlaps it is two things standing in one place.
        if (towerStoreys > 0 && candidate.outer.size() >= 3u)
        {
          std::vector<geom2::Polygon> left;

          if (geom2::booleanOp({geom2::toOperand(candidate), geom2::toOperand(towerPlan)}, geom2::Operation::Difference, {}, left, nullptr) && left.size() == 1u)
          {
            candidate = std::move(left.front());
          }
        }

        // Wide enough to stand in, and small enough to leave a ground floor.
        std::vector<geom2::Polygon> wide;
        const bool broad = candidate.outer.size() >= 3u && geom2::offsetPolygon(candidate, -s.entranceMinimumWidthMetres * .35, {}, wide, nullptr) && !wide.empty();
        const double area = candidate.outer.size() >= 3u ? geom2::areaMetres(candidate) : 0;

        if (broad && area > s.entranceMinimumWidthMetres && area <= plateArea * s.entranceMaximumAreaShare)
        {
          // The ground the storey gives up is the same box against the
          // *outside* of the building rather than the inside of its walls.
          //
          // Not the room grown by a wall on every side, which is what this
          // was. Grown all round, the recess eats the walls *beside* the bay
          // as well as the one across its front - so the ground floor stopped
          // a hand's breadth short of the bay's own side wall, and the corner
          // post came out standing on its own in a slot with daylight either
          // side of it. Only the street side has a wall to get past.
          std::vector<geom2::Polygon> reach;

          if (geom2::booleanOp({geom2::toOperand(box), geom2::toOperand(footprint)}, geom2::Operation::Intersection, {}, reach, nullptr) && !reach.empty())
          {
            std::sort(reach.begin(), reach.end(), [](const geom2::Polygon &l, const geom2::Polygon &r) {
              return geom2::areaMetres(l) > geom2::areaMetres(r);
            });

            made.mass = std::move(reach.front());
            made.plan = std::move(candidate);
          }
        }
      }
      }

      return made;
    };

  }

  // Across the whole front when the building has a trade room behind it.
  //
  // A bay across part of a hall's front leaves the hall an L with a notch
  // bitten out of one long side, and a notch is two reflex corners for the
  // roof and a width the hall no longer has. Across the whole of it the bay is
  // a screens bay: the trade room keeps its shape and simply begins further
  // back, which is what a hall entered under a range, or a shop behind an open
  // front, actually is.
  const double bayShare = wantsHall ? 1. : s.entranceWidthShare;

  // The bay at a given depth, plan and mass together, or nothing.
  //
  // Both at once because they are one decision: the plan is cut inside the
  // walls and the recess has to reach the outside of them, or the bay is a
  // room with a skin of wall left standing across its open side. And callable
  // more than once because how deep the bay may be is not known until the
  // trade room behind it has been chosen.
  const auto setBay = [&](const double depth) {
    Bay made = depth > .8 ? bayOfShare(bayShare, depth) : Bay{};

    if (made.plan.outer.size() < 3u || made.mass.outer.size() < 3u)
    {
      porchPlan = geom2::Polygon{};
      porchMass = geom2::Polygon{};

      return false;
    }

    porchPlan = std::move(made.plan);
    porchMass = std::move(made.mass);

    return true;
  };

  setBay(s.entranceDepthMetres);

  // ---- the hall -----------------------------------------------------------
  //
  // A band across the full depth of the ground plate, at the end of it the
  // door is on. The end matters: a hall is entered at one end, through a
  // screens passage, and a taproom fronts its street - and a band taken out
  // of the middle leaves the rooms either side of it with no way between
  // them on the floor above, where the hall is a void.
  geom2::Polygon hallPlan;

  // The same band before the tower was taken out of it. The plan needs the
  // hole - two rooms cannot stand on one patch of floor - and the massing is
  // better off without it, because the shaft rises through the hall's roof
  // rather than being cut out of it.
  geom2::Polygon hallMass;

  uint32_t hallFloors = 0;

  // The bay gives way to the hall, by degrees.
  //
  // A hall is entered at its end. Everything else about it is negotiable - how
  // deep it is, how much of the plate it takes, whether it is open to the roof
  // - and that is not, because a hall at the far end of a building is a back
  // room the size of a hall. The two features want the same ground: the front.
  //
  // So the band is chosen with the bay taken out of the front, and if that
  // puts it at the back of the building the bay is tried shallower and then
  // not at all. An L whose bar is eight metres deep cannot give two of them to
  // a porch and still have a room at the door; it can give one.
  const double bayLadder[] = {1., .55, 0.};

  for (int attempt = 0; wantsHall && attempt < 3; ++attempt)
  {
    const bool withBay = setBay(s.entranceDepthMetres * bayLadder[attempt]);

    hallPlan = geom2::Polygon{};
    hallMass = geom2::Polygon{};
    hallFloors = 0;

    // Chosen on the whole storey plate, and the tower punched out of the
    // winner afterwards.
    //
    // Subtracting the tower first is the obvious order and it gives the wrong
    // hall: the tower stands at a corner of the end the door is on, so the
    // band at that end arrives with a notch in it, is judged not to be a
    // rectangular room, and the hall is put at the *far* end of the building
    // instead - fourteen metres from a front door nine metres from the middle.
    // Whether a band is a room is a question about the storey, not about what
    // has already been carved out of it.
    //
    // The entrance bay is the exception, and it is not an exception to that
    // rule but an instance of it: the bay is not carved out of the storey, it
    // is ground the storey does not have. Both want the front of the building,
    // so a band sized to the whole plate and then cut by the bay is a band
    // that is no longer wide enough to be a hall. Sized to what is there, it
    // simply starts behind the bay - and because a bay in front of a trade
    // room runs the full width of the front, what is left is still the
    // rectangle the band needs.
    geom2::Polygon behind = storeyPlan.front();

    if (withBay)
    {
      std::vector<geom2::Polygon> left;

      if (geom2::booleanOp({geom2::toOperand(behind), geom2::toOperand(porchPlan)}, geom2::Operation::Difference, {}, left, nullptr) && left.size() == 1u && geom2::areaMetres(left.front()) > s.minimumRoomMetres * 2)
      {
        behind = std::move(left.front());
      }
    }

    const geom2::Polygon &plate = behind;
    const double plateArea = geom2::areaMetres(plate);

    double loU = 1e300, hiU = -1e300, loV = 1e300, hiV = -1e300;

    for (const geom2::Point &q : plate.outer)
    {
      double u = 0, v = 0;
      toPlan(geom2::toMetres(q.x), geom2::toMetres(q.y), u, v);

      loU = std::min(loU, u);
      hiU = std::max(hiU, u);
      loV = std::min(loV, v);
      hiV = std::max(hiV, v);
    }

    // A shop band is held to a hall's proportions, and deliberately.
    //
    // Loosening them for a shop is the obvious move and it is wrong twice
    // over: it admits bands along the other axis of the plan, so a shop-house
    // on an L got a narrow band nearer the door than the front range and lost
    // the corridor that made the plan readable - and it did not buy what it
    // was for, which was a bay in front of a shop on a lot too tight to have
    // both. A room the building is arranged around has a shape whatever trade
    // is carried on in it.
    const double bandArea = s.hallMinimumAreaMetres;
    const double bandWidth = s.hallMinimumWidthMetres;

    const double target = std::max(plateArea * s.hallAreaShare, std::min(plateArea, bandArea));

    double entranceU = 0, entranceV = 0;
    toPlan(b.entrance.x, b.entrance.z, entranceU, entranceV);

    const double reach = (hiV - loV) + (hiU - loU) + 4.0;

    // The band of length `length` at one end of one axis, clipped to the
    // plate. Its area rises with its length, so the length that gives the
    // target area is a bisection.
    const auto bandOf = [&](const double length, const bool alongU, const bool fromLow, geom2::Polygon &out) {
      const double lo = alongU ? loU : loV, hi = alongU ? hiU : hiV;
      const double from = fromLow ? lo - 1.0 : hi - length;
      const double to = fromLow ? lo + length : hi + 1.0;

      const auto corner = [&](const double a, const double across) {
        return alongU ? toGround(a, across) : toGround(across, a);
      };

      geom2::Polygon strip;
      strip.outer = {
        corner(from, -reach),
        corner(to, -reach),
        corner(to, reach),
        corner(from, reach)};

      if (!alongU)
      {
        std::reverse(strip.outer.begin(), strip.outer.end());
      }

      std::vector<geom2::Polygon> clipped;

      if (!geom2::polygonFault(strip).empty() || !geom2::booleanOp({geom2::toOperand(strip), geom2::toOperand(plate)}, geom2::Operation::Intersection, {}, clipped, nullptr) || clipped.empty())
      {
        return false;
      }

      std::sort(clipped.begin(), clipped.end(), [](const geom2::Polygon &l, const geom2::Polygon &r) {
        return geom2::areaMetres(l) > geom2::areaMetres(r);
      });

      out = std::move(clipped.front());

      return true;
    };

    if (target >= plateArea * .92)
    {
      hallPlan = plate;
    }
    else
    {
      // Which end of which axis. Four bands, and the plan says which is a
      // hall.
      //
      // Taking the long axis of the footprint and banding across it is right
      // for a range and wrong for anything that turns a corner: a U twenty
      // metres wide and sixteen deep is longest across its front, so the band
      // came down one *arm* of it - a hall the shape of an L, with the bar of
      // the U left over. The bar is the hall. What says so is not the
      // bounding box but the piece itself: a hall is a rectangular room, and
      // the band that fills its own extent is the one that is a room rather
      // than a piece of leftover plan.
      struct Candidate
      {
        geom2::Polygon band;
        double depth = 0, away = 0, length = 0;
        bool alongU = true, fromLow = true;
      };

      std::vector<Candidate> candidates;
      double bestScore = -1;

      for (const bool alongU : {true, false})
      {
        for (const bool fromLow : {true, false})
        {
          double lo = 0, hi = alongU ? hiU - loU : hiV - loV;
          geom2::Polygon trial;

          for (int step = 0; step < 24; ++step)
          {
            const double mid = (lo + hi) * .5;

            geom2::Polygon piece;

            if (!bandOf(mid, alongU, fromLow, piece))
            {
              lo = mid;
              continue;
            }

            (geom2::areaMetres(piece) < target ? lo : hi) = mid;
            trial = std::move(piece);
          }

          if (trial.outer.size() < 3u)
          {
            continue;
          }

          double spanA = 0, spanB = 0;
          extentOn(trial, ux, uy, spanA, spanB);

          const double area = geom2::areaMetres(trial);
          const double fill = spanA * spanB > 1e-9 ? area / (spanA * spanB) : 0.0;
          const double narrow = std::min(spanA, spanB);

          if (fill < .95 || narrow < bandWidth || area < bandArea)
          {
            continue;
          }

          double middleA = 0, middleB = 0;
          geom2::Point centre;

          if (geom2::centroid(trial, centre))
          {
            toPlan(geom2::toMetres(centre.x), geom2::toMetres(centre.y), middleA, middleB);
          }

          candidates.push_back({std::move(trial), std::min(narrow, s.maximumWingDepthMetres), std::hypot(middleA - entranceU, middleB - entranceV), hi, alongU, fromLow});
        }
      }

      // Among the bands that are rooms: the deepest one a building of this age
      // can span, and of those the one the door is on.
      //
      // Two questions and not one blended number. Blended, the depth term and
      // the distance term trade against each other and neither decides
      // anything: a hall five metres deep at the door beat a hall eight metres
      // deep at the door by a margin that had nothing to do with either.
      // A hall is entered at its end, through a screens passage, and a taproom
      // fronts its street - so depth settles which bands are halls and the
      // door settles which of those is this one.
      for (const Candidate &candidate : candidates)
      {
        bestScore = std::max(bestScore, candidate.depth);
      }

      double nearest = 1e300;
      const Candidate *chosen = nullptr;

      for (const Candidate &candidate : candidates)
      {
        if (candidate.depth + 1.0 >= bestScore && candidate.away < nearest)
        {
          nearest = candidate.away;
          chosen = &candidate;
        }
      }

      if (chosen != nullptr)
      {
        hallPlan = chosen->band;

        // The hall's back wall lines up with the tower's, or clears it by a
        // room's width. Nothing in between.
        //
        // The band is sized by area and the tower by its own side, so the two
        // land within a hand's breadth of each other by coincidence: a hall
        // five metres deep against a tower four metres two left a lip eight
        // hundred millimetres wide running the width of the shaft. It is not
        // floor anybody can use, and the roof over it is worse - the skeleton
        // runs a stub ridge along it at six metres and then steps up to eight
        // and a half where the hall proper begins, which is the jog in the
        // ridge beside the tower.
        if (towerStoreys > 0)
        {
          double towerLo = 1e300, towerHi = -1e300;

          for (const geom2::Point &q : towerPlan.outer)
          {
            double a = 0, c = 0;
            toPlan(geom2::toMetres(q.x), geom2::toMetres(q.y), a, c);

            const double along = chosen->alongU ? a : c;

            towerLo = std::min(towerLo, along);
            towerHi = std::max(towerHi, along);
          }

          const double start = chosen->fromLow ? (chosen->alongU ? loU : loV) : (chosen->alongU ? hiU : hiV);
          const double reach = chosen->fromLow ? towerHi - start : start - towerLo;

          double bandLo = 1e300, bandHi = -1e300;

          for (const geom2::Point &q : hallPlan.outer)
          {
            double a = 0, c = 0;
            toPlan(geom2::toMetres(q.x), geom2::toMetres(q.y), a, c);

            const double along = chosen->alongU ? a : c;

            bandLo = std::min(bandLo, along);
            bandHi = std::max(bandHi, along);
          }

          const double band = chosen->fromLow ? bandHi - start : start - bandLo;
          const double lip = band - reach;

          if (lip > .01 && lip < s.minimumWidthMetres && reach > bandWidth)
          {
            geom2::Polygon snapped;

            if (bandOf(reach, chosen->alongU, chosen->fromLow, snapped) && geom2::areaMetres(snapped) >= bandArea)
            {
              hallPlan = std::move(snapped);
            }
          }
        }
      }
    }

    hallMass = hallPlan;

    // ...but the mass reaches the street. The band stops behind the bay
    // because the *ground floor* does; the storey over it does not, and a mass
    // that stopped there would step back at the front for no reason and take
    // its roof with it.
    if (withBay && hallPlan.outer.size() >= 3u)
    {
      std::vector<geom2::Polygon> joined;

      if (geom2::booleanOp({geom2::toOperand(hallPlan), geom2::toOperand(porchPlan)}, geom2::Operation::Union, {}, joined, nullptr) && joined.size() == 1u)
      {
        hallMass = std::move(joined.front());
      }
    }

    // ...and now the tower, which owns its ground on every floor including
    // this one.
    if (towerStoreys > 0 && hallPlan.outer.size() >= 3u)
    {
      std::vector<geom2::Polygon> left;

      if (geom2::booleanOp({geom2::toOperand(hallPlan), geom2::toOperand(towerPlan)}, geom2::Operation::Difference, {}, left, nullptr) && !left.empty())
      {
        std::sort(left.begin(), left.end(), [](const geom2::Polygon &l, const geom2::Polygon &r) {
          return geom2::areaMetres(l) > geom2::areaMetres(r);
        });

        hallPlan = std::move(left.front());
      }
    }

    // A hall has to be a hall in both directions. A band four metres deep and
    // twenty long is a barn.
    std::vector<geom2::Polygon> wide;

    const bool broad = hallPlan.outer.size() >= 3u && geom2::offsetPolygon(hallPlan, -bandWidth * .5, {}, wide, nullptr) && !wide.empty();

    if (hallPlan.outer.size() < 3u || geom2::areaMetres(hallPlan) < bandArea || !broad)
    {
      hallPlan = geom2::Polygon{};
      hallMass = geom2::Polygon{};
    }
    else
    {
      hallFloors = trading ? 1u : std::max(1u, std::min(s.hallHeightFloors, uint32_t(b.availableHeight / std::max(b.floorRise, .1))));

      // What is left upstairs once the hall is open through it. A tavern
      // keeps its chambers: if the hall would take the whole floor above, the
      // taproom stays one storey tall and the household sleeps over it. A
      // hall house has no such claim - an open hall is what it is for.
      if (hallFloors > 1u && builtFloors > 1u && b.use == rewrite::DwellingUse::Tavern)
      {
        std::vector<geom2::Operand> difference{geom2::toOperand(storeyPlan[1]), geom2::toOperand(hallPlan)};

        if (towerStoreys > 0)
        {
          difference.push_back(geom2::toOperand(towerPlan));
        }

        std::vector<geom2::Polygon> upstairs;
        double left = 0;

        if (geom2::booleanOp(difference, geom2::Operation::Difference, {}, upstairs, nullptr))
        {
          for (const geom2::Polygon &piece : upstairs)
          {
            left = std::max(left, geom2::areaMetres(piece));
          }
        }

        hallFloors = left < s.minimumRoomMetres * 3 ? 1u : hallFloors;
      }
    }

    if (!withBay)
    {
      break;
    }

    // Is the band at the door? Measured against the middle of the plate, which
    // is the same test the gallery makes of a finished plan: a hall whose
    // centre is further from the door than the centre of the building is, is
    // at the wrong end.
    geom2::Point middle;
    bool atTheDoor = hallPlan.outer.size() >= 3u && hallFloors > 0u;

    if (atTheDoor && geom2::centroid(storeyPlan.front(), middle))
    {
      geom2::Point centre;

      if (geom2::centroid(hallPlan, centre))
      {
        const double band = std::hypot(geom2::toMetres(centre.x) - b.entrance.x, geom2::toMetres(centre.y) - b.entrance.z);
        const double plan = std::hypot(geom2::toMetres(middle.x) - b.entrance.x, geom2::toMetres(middle.y) - b.entrance.z);

        atTheDoor = band <= plan + .5;
      }
    }

    if (atTheDoor)
    {
      break;
    }
  }

  // How tall the outside stands. A hall open through two storey heights is a
  // two storey wall with one floor behind it, and the shell has to know that
  // or the roof comes down through the hall.
  const uint32_t massStoreys = std::max(builtFloors, hallFloors);

  for (uint32_t floor = 0; floor < builtFloors; ++floor)
  {
    const double y = b.foundation + floor * b.floorRise;

    // The storey's whole plate, which is what decides whether a wall is an
    // outside wall, and separately the ground this floor may actually cut
    // rooms out of - the plate less the tower's shaft and less the hall's
    // open volume where it rises through this floor.
    const geom2::Polygon &enclosure = storeyPlan[floor];

    geom2::Polygon plate = enclosure;
    std::vector<geom2::Polygon> spare;

    {
      std::vector<geom2::Operand> difference{geom2::toOperand(enclosure)};

      if (towerStoreys > 0)
      {
        difference.push_back(geom2::toOperand(towerPlan));
      }

      if (hallFloors > 0 && floor < hallFloors)
      {
        difference.push_back(geom2::toOperand(hallPlan));
      }

      // The bay is ground floor only: the storey above it is the roof over it.
      if (floor == 0u && porchPlan.outer.size() >= 3u)
      {
        difference.push_back(geom2::toOperand(porchPlan));
      }

      if (difference.size() > 1u)
      {
        std::vector<geom2::Polygon> left;

        if (!geom2::booleanOp(difference, geom2::Operation::Difference, {}, left, nullptr))
        {
          left.clear();
        }

        std::sort(left.begin(), left.end(), [](const geom2::Polygon &l, const geom2::Polygon &r) {
          return geom2::areaMetres(l) > geom2::areaMetres(r);
        });

        // Above the ground floor, only the largest piece is kept.
        //
        // The hall and the tower are rooms on the ground floor, so they bridge
        // whatever the subtraction separated and every piece is still reached.
        // Above it the hall is a void, and a piece cut off on the far side of
        // one has no wall on anything - it is a gallery reached by a walkway
        // that does not exist. There is no floor there, and saying so makes
        // the hall wider rather than leaving a room stranded.
        if (floor > 0 && left.size() > 1u)
        {
          left.resize(1);
        }

        double kept = 0;

        for (const geom2::Polygon &piece : left)
        {
          kept += geom2::areaMetres(piece);
        }

        if (left.empty() || kept < s.minimumRoomMetres)
        {
          // The reserved spaces took the whole storey. It still has to have a
          // record, because a wall names its floor by index.
          DwellingFloor empty;
          empty.elevation = y;
          empty.height = b.floorRise;
          empty.grossArea = geom2::areaMetres(footprint);

          out.floors.push_back(std::move(empty));

          continue;
        }

        plate = std::move(left.front());

        // The subdivision works on one polygon, so anything else the cut left
        // is kept aside and divided on its own.
        for (size_t extra = 1; extra < left.size(); ++extra)
        {
          if (geom2::areaMetres(left[extra]) >= s.minimumRoomMetres)
          {
            spare.push_back(std::move(left[extra]));
          }
        }
      }
    }

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

    // Each connected piece of the floor is planned on its own.
    //
    // Reserving the hall can leave the rest of the storey in more than one
    // piece - a U whose bar is the hall is two arms with nothing between them
    // - and circulation is a question about a piece, not about a storey. Run
    // on the largest piece only, the second arm of a manor got no corridor at
    // all and its chambers were reached one through another, which is the
    // thing corridors exist to stop.
    std::vector<geom2::Polygon> plates{plate};

    for (geom2::Polygon &piece : spare)
    {
      plates.push_back(std::move(piece));
    }

    for (const geom2::Polygon &plate : plates)
    {
      const size_t roomsBefore = rooms.size(), corridorsBefore = corridors.size();

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
      // Carved to its finished width plus the walls either side of it.
      //
      // The strip is cut on the *centre lines* of the walls that will bound
      // it, and a room is then set back half a wall from each of those - so a
      // corridor cut at its nominal width comes out a wall thinner than it was
      // asked to be, which on a corridor is the difference between walking
      // down it and turning sideways.
      const double half = (s.corridorWidthMetres + s.interiorWallThickness) * .5;
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

        // The crossing belongs to the strip that got there first, and it is
        // settled here - before the bays are cut - because the bays have to be
        // cut against the corridor the building actually gets.
        //
        // Trimmed afterwards, a crossing that pinched to nothing was dropped
        // from the corridor while the rooms either side of it had already been
        // cut apart by it: the gap belonged to nobody, the rooms touched
        // nothing, and a manor lost its whole plan to a fourteen square metre
        // chamber with no door anywhere on it.
        //
        // Each band is clipped to the building and to nothing else, so a cross
        // band and the spine it crosses both claimed the square metre and a
        // half where they meet. The bays were cut against both, so the rooms
        // were right and only the circulation was wrong - two corridor rooms
        // standing on one patch of floor, its area counted twice and its walls
        // emitted twice.
        if (!strips.empty())
        {
          std::vector<geom2::Operand> difference;

          for (const geom2::Polygon &piece : strip)
          {
            difference.push_back(geom2::toOperand(piece));
          }

          for (const geom2::Polygon &piece : strips)
          {
            difference.push_back(geom2::toOperand(piece));
          }

          std::vector<geom2::Polygon> trimmed;

          if (geom2::booleanOp(difference, geom2::Operation::Difference, {}, trimmed, nullptr))
          {
            strip.clear();

            for (geom2::Polygon &piece : trimmed)
            {
              // Trimmed, a strip can end up narrower than it was, and width
              // is the question a corridor answers. Twice the area over the
              // perimeter is the width of a strip, and it falls wherever the
              // piece pinches - which is the same test the band itself had to
              // pass before it was offered.
              double perimeter = 0;

              for (size_t k = 0; k < piece.outer.size(); ++k)
              {
                const double px = geom2::toMetres(piece.outer[k].x), pz = geom2::toMetres(piece.outer[k].y);
                const double qx = geom2::toMetres(piece.outer[(k + 1) % piece.outer.size()].x), qz = geom2::toMetres(piece.outer[(k + 1) % piece.outer.size()].y);

                perimeter += std::hypot(qx - px, qz - pz);
              }

              if (perimeter > 1e-6 && 2 * geom2::areaMetres(piece) / perimeter >= s.minimumWalkableMetres)
              {
                strip.push_back(std::move(piece));
              }
            }
          }
        }


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
        // Appended, not assigned: each connected piece of the floor is planned
        // on its own, and the second piece's circulation does not replace the
        // first's. Assigned, the left arm of a manor kept the rooms its
        // corridor had cut apart and lost the corridor itself, so a chamber
        // between two of them had no door on anything.
        for (geom2::Polygon &piece : strips)
        {
          corridors.push_back(std::move(piece));
        }

        for (const geom2::Polygon &bay : bays)
        {
          if (geom2::areaMetres(bay) >= s.minimumRoomMetres)
          {
            divide(bay, 0);
          }
        }
      }
    }

    // Strips that earned their place and then left no bay big enough to be a
    // room are not circulation, they are the whole piece. The piece is cut
    // without them.
    if (rooms.size() == roomsBefore)
    {
      corridors.resize(corridorsBefore);
      divide(plate, 0);
    }
    }

    if (rooms.empty() && hallFloors == 0u && towerStoreys == 0u)
    {
      return fail(error, "floor produced no rooms");
    }

    // The carved strips join the room list, flagged; so do the reserved
    // spaces, whose purposes were settled before any of this ran.
    std::vector<bool> isCorridor(rooms.size(), false);
    std::vector<bool> isFixed(rooms.size(), false);
    std::vector<RoomUse> fixedUse(rooms.size(), RoomUse::Common);

    for (geom2::Polygon &piece : corridors)
    {
      rooms.push_back(std::move(piece));
      isCorridor.push_back(true);
      isFixed.push_back(true);
      fixedUse.push_back(RoomUse::Corridor);
    }

    // The hall stands on the ground floor and rises through the ones above it,
    // so it is a room once and a void thereafter. The tower is a room on the
    // ground floor and is emitted on its own for every floor above, because up
    // there it touches whatever the plan happens to leave beside it and the
    // door search must not be asked to reach it sideways.
    // "No such room", as a value that cannot become a room.
    //
    // These were `rooms.size()`, read at the moment each was declared - and
    // the list grows immediately afterwards, so the sentinel for a building
    // with no tower was the index the *hall* was about to be pushed to. Every
    // hall in a building without a tower therefore answered yes to "are you
    // the tower": a taproom was named a staircase and furnished as one, and
    // the search for the room a visitor arrives in skipped it on the grounds
    // that nobody arrives in a tower.
    const size_t noRoom = size_t(-1);

    size_t hallRoom = noRoom, towerRoom = noRoom;

    if (floor == 0u && hallFloors > 0u)
    {
      hallRoom = rooms.size();

      rooms.push_back(hallPlan);
      isCorridor.push_back(false);
      isFixed.push_back(true);
      fixedUse.push_back(hallUse);
    }

    if (floor == 0u && towerStoreys > 0u)
    {
      towerRoom = rooms.size();

      rooms.push_back(towerPlan);
      isCorridor.push_back(false);
      isFixed.push_back(true);
      fixedUse.push_back(RoomUse::Tower);
    }

    size_t porchRoom = noRoom;

    if (floor == 0u && porchPlan.outer.size() >= 3u)
    {
      porchRoom = rooms.size();

      rooms.push_back(porchPlan);
      isCorridor.push_back(false);
      isFixed.push_back(true);
      fixedUse.push_back(RoomUse::Entrance);
    }

    if (rooms.empty())
    {
      return fail(error, "floor produced no rooms");
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
      std::vector<bool> keptCorridor, keptFixed;
      std::vector<RoomUse> keptUse;

      size_t hallKept = noRoom, towerKept = noRoom, porchKept = noRoom;

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

        if (!touches && !isFixed[i] && geom2::areaMetres(rooms[i]) < s.minimumRoomMetres)
        {
          continue;
        }

        hallKept = i == hallRoom ? kept.size() : hallKept;
        towerKept = i == towerRoom ? kept.size() : towerKept;
        porchKept = i == porchRoom ? kept.size() : porchKept;

        kept.push_back(std::move(rooms[i]));
        keptCorridor.push_back(isCorridor[i]);
        keptFixed.push_back(isFixed[i]);
        keptUse.push_back(fixedUse[i]);
      }

      rooms = std::move(kept);
      isCorridor = std::move(keptCorridor);
      isFixed = std::move(keptFixed);
      fixedUse = std::move(keptUse);
      hallRoom = hallKept;
      towerRoom = towerKept;
      porchRoom = porchKept;
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

    order.erase(std::remove_if(order.begin(), order.end(), [&](const size_t i) { return isFixed[i]; }), order.end());

    std::sort(order.begin(), order.end(), [&](const size_t a, const size_t c) {
      return areas[a] != areas[c] ? areas[a] > areas[c] : a < c;
    });

    if (order.empty() && hallRoom == noRoom && towerRoom == noRoom)
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
      // A tower is a stair and not a hallway; nobody arrives in one - and the
      // bay is the outside of the building, so a visitor passes through it
      // rather than arriving in it. The arrival room is what its door opens
      // onto.
      if ((hasCorridor && !isCorridor[i]) || i == towerRoom || i == porchRoom)
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

    // The hall is already a room, with its purpose settled before the plate
    // was cut. Asking the programme for one as well hands the label to the
    // largest thing left over, which on a hall house is the kitchen.
    const bool hasReservedHall = hallRoom != noRoom;

    // Which programme this floor takes.
    //
    // A floor of a house is part of a dwelling - a ground floor of rooms you
    // receive in and an upper floor of rooms you sleep in - and a floor of a
    // tenement is a *whole* dwelling: it wants a room to live in, a room to
    // sleep in and somewhere to cook, the same as the one under it. Asked for
    // the upper storey's programme it comes out as chambers with no hearth and
    // nowhere to make a meal, which is a bedroom floor and not a home.
    const uint32_t programmeFloor = tenement ? 0u : floor;

    switch (b.use)
    {
    case rewrite::DwellingUse::ShopHouse:
      if (programmeFloor == 0u)
      {
        if (!hasReservedHall)
        {
          programme.push_back({RoomUse::Shop, 14, true, false});
        }

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

    case rewrite::DwellingUse::Tavern:
      // A tavern is two buildings on top of each other. Downstairs is the
      // trade: the taproom, which is the reserved hall, a kitchen behind it
      // and a cellar store. Upstairs is one household - a chamber and what
      // the house keeps - and nothing else, because the people downstairs are
      // strangers and that is the whole reason the two are not mixed.
      if (programmeFloor == 0u)
      {
        if (!hasReservedHall)
        {
          programme.push_back({RoomUse::Hall, 18, true, true});
        }

        programme.push_back({RoomUse::Service, 9, false, false});
        programme.push_back({RoomUse::Store, 6, false, false});

        filler = onlyStorey ? std::vector<RoomUse>{RoomUse::Store, RoomUse::Service, RoomUse::Sleeping, RoomUse::Common}
                            : std::vector<RoomUse>{RoomUse::Store, RoomUse::Service, RoomUse::Common};
      }
      else
      {
        programme.push_back({RoomUse::Sleeping, 9, false, true});
        programme.push_back({RoomUse::Store, 5, false, false});

        filler = {RoomUse::Sleeping, RoomUse::Store, RoomUse::Living};
      }
      break;

    case rewrite::DwellingUse::Hall:
      if (programmeFloor == 0u)
      {
        if (!hasReservedHall)
        {
          programme.push_back({RoomUse::Hall, 0, false, true});
        }

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
      if (programmeFloor == 0u)
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
      if (programmeFloor == 0u)
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
      if (isFixed[i])
      {
        uses[i] = fixedUse[i];
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

    // What to call a room, as against what class it is in.
    //
    // The subdivision produces pieces and the programme gives each a class -
    // service, living, store - which is as far as a rule that works on shapes
    // can get. The name is a second question and it is answered from the
    // building rather than from the room: the service room of a tavern is its
    // kitchen, the service room of a merchant's house is its counting house,
    // and the same piece of floor would be a scullery in a manor. Nothing
    // about the shape says which, and the building says all three.
    //
    // Coarse on purpose. A finer answer is what the programme network exists
    // for; this is the vocabulary made reachable from the planner that cuts
    // the rooms, which previously could not say "kitchen" at all.
    const auto nameOf = [&](const RoomUse use, const bool isPorch, const bool isTower, const uint32_t level, const bool corridor) {
      const bool tavern = b.use == rewrite::DwellingUse::Tavern;
      const bool shop = b.use == rewrite::DwellingUse::ShopHouse;
      const bool great = b.use == rewrite::DwellingUse::Hall;

      if (isPorch)
      {
        return rewrite::RoomKind::Porch;
      }

      if (isTower)
      {
        return rewrite::RoomKind::Staircase;
      }

      if (corridor)
      {
        // A ground floor passage in a hall house is its screens passage: the
        // cross passage at the low end, between the doors, which is the one
        // piece of circulation a building of this age is actually named for.
        return great && level == 0u ? rewrite::RoomKind::ScreensPassage : level == 0u ? rewrite::RoomKind::Passage : rewrite::RoomKind::Landing;
      }

      switch (use)
      {
      case RoomUse::Hall:
        return tavern ? rewrite::RoomKind::TapRoom : great ? rewrite::RoomKind::GreatHall : rewrite::RoomKind::Hall;

      case RoomUse::Shop:
        return shop ? rewrite::RoomKind::Shop : rewrite::RoomKind::Workshop;

      case RoomUse::Store:
        // Below the trade floor it is a cellar, over it a loft, and at the
        // back of a kitchen it is a larder. Level decides, because that is
        // what decides it in a real building.
        return level == 0u ? (tavern || shop ? rewrite::RoomKind::Cellar : rewrite::RoomKind::Store) : rewrite::RoomKind::Loft;

      case RoomUse::Service:
        return tavern ? rewrite::RoomKind::Kitchen : shop ? rewrite::RoomKind::CountingHouse : great ? rewrite::RoomKind::Buttery : rewrite::RoomKind::Scullery;

      case RoomUse::Sleeping:
        return b.use == rewrite::DwellingUse::Lodging ? rewrite::RoomKind::Dormitory : great ? rewrite::RoomKind::Solar : rewrite::RoomKind::Bedroom;

      case RoomUse::Living:
        return level == 0u ? rewrite::RoomKind::LivingRoom : rewrite::RoomKind::Parlour;

      case RoomUse::Common:
        return great ? rewrite::RoomKind::Solar : rewrite::RoomKind::Parlour;

      case RoomUse::Corridor:
        return rewrite::RoomKind::Passage;

      case RoomUse::Entrance:
        return rewrite::RoomKind::Porch;

      case RoomUse::Tower:
        return rewrite::RoomKind::Staircase;
      }

      return rewrite::RoomKind::Count;
    };

    // ---- the rooms, their walls and their furniture -----------------------
    DwellingFloor record;
    record.elevation = y;
    record.height = b.floorRise;
    record.grossArea = geom2::areaMetres(footprint);

    std::vector<uint64_t> ids(rooms.size());

    // Which walls each room emitted, in ring order. Openings are hung on wall
    // ids, and a wall is only identified once it has been given one.
    std::vector<std::vector<uint64_t>> roomWalls(rooms.size());

    for (size_t i = 0; i < rooms.size(); ++i)
    {
      DwellingRoom room;
      room.id = roomId++;
      room.floor = floor;
      room.use = uses[i];
      room.kind = nameOf(uses[i], i == porchRoom, i == towerRoom, floor, isCorridor[i]);
      room.usableArea = areas[i];

      // A hall is the room whose height is the point of it: open through the
      // storey that would otherwise stand over it, and open to the roof when
      // nothing stands over that either.
      room.heightFloors = i == hallRoom ? hallFloors : 1u;
      room.ceilingHeight = y + double(room.heightFloors) * b.floorRise;
      room.openToRoof = i == hallRoom && floor + room.heightFloors >= massStoreys;

      // The room, inside its own walls.
      //
      // The subdivision cut this piece out of the plate, so it runs to the
      // *centre line* of every wall around it: its boundary is shared with the
      // room next door, and the wall between them is a line with no depth. The
      // walls are still emitted on those lines - the door search, the shared
      // runs and the openings all work on them - and what the room reports is
      // the face of the plaster, half a wall in.
      //
      // Which is also the only honest floor area. Reported on the cut, a house
      // of eight rooms claims the wall between every pair of them twice.
      {
        geom2::Polygon inside = rooms[i];
        std::vector<geom2::Polygon> shrunk;

        if (s.interiorWallThickness > 0 && geom2::offsetPolygon(rooms[i], -s.interiorWallThickness * .5, {}, shrunk, nullptr) && shrunk.size() == 1u && geom2::areaMetres(shrunk.front()) > areas[i] * .4)
        {
          inside = std::move(shrunk.front());
          room.usableArea = geom2::areaMetres(inside);
        }

        for (const geom2::Point &p : inside.outer)
        {
          room.boundary.push_back(Position{geom2::toMetres(p.x), y, geom2::toMetres(p.y)});
        }
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
        // ...corrected to an inside wall's below, once it is known which it is.

        // An outside wall is one that stands *on* the storey's outline.
        //
        // Not "one whose midpoint is outside the plate", which is what this
        // asked before and which is never true of anything: a room is cut out
        // of the plate, so every one of its walls is inside it or on it, and
        // `contains` counts a point on the boundary as inside. So no wall in
        // any plan this planner produced was ever an outside wall - the flag
        // was there, it was read downstream, and it was uniformly false.
        //
        // Judged against the storey's whole plate rather than the ground this
        // floor was left to cut up, or the wall onto a hall's open volume -
        // which is as interior as a wall gets - would come out as an outside
        // wall of the building.
        const double midX = (geom2::toMetres(p.x) + geom2::toMetres(q.x)) * .5;
        const double midZ = (geom2::toMetres(p.y) + geom2::toMetres(q.y)) * .5;

        wall.exterior = distanceToRing(enclosure.outer, midX, midZ) < s.wallThickness * .5;
        wall.thickness = wall.exterior ? s.wallThickness : s.interiorWallThickness;
        wall.height = room.heightFloors > 1u ? double(room.heightFloors) * b.floorRise : 0.0;

        // The bay's open side is the one that faces the street.
        //
        // Every other room's outside wall is the building's skin. The bay's
        // front is the opening you walk through, and what holds the floor above
        // it up stands beside the opening rather than across it. Said here
        // because it cannot be said anywhere else: the ring is the same ring
        // either way, and a mesher reading only geometry bricks the porch in.
        //
        // Every outside wall of the bay, not only the one facing the street.
        //
        // The ground storey gives up the whole of the bay, so where the bay
        // runs the full width of the front the storey is cut clean through and
        // its returns are open too - you walk in from the side as well as from
        // the front. Restricting this to the street side left the plan saying
        // "wall" exactly where the mass says "air": no masonry, no marker, and
        // a post standing at the corner of a gap with nothing to explain it.
        //
        // A bay that takes only part of the front does not have this problem
        // and needs no special case: its returns are against the rooms beside
        // it, so they are interior walls and never come up here at all.
        if (i == porchRoom && wall.exterior)
        {
          wall.kind = rewrite::WallKind::Open;
        }

        roomWalls[i].push_back(wall.id);
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
          door.fromRoom = ids[from];
          door.floor = floor;
          door.center = Position{centre.x, y, centre.z};
          door.width = std::min(1.0, run * .6);
          door.privateAccess = uses[to] == RoomUse::Sleeping;

          out.doors.push_back(std::move(door));
        }
      }

      frontier = std::move(next);
    }

    // Then whatever the search could not reach, on the shortest wall it has.
    //
    // The search crosses a wall only where a comfortable door fits - nine
    // tenths of a metre - and in a tight plan a room can share less than that
    // with every one of its neighbours and still be perfectly reachable
    // through a narrower opening. Failing the whole building over it was the
    // answer, and once a hall and a tower are reserved out of the plate it
    // stopped being rare: a manor lost its plan to a fourteen square metre
    // chamber that shared three quarters of a metre with the room beside it.
    //
    // A room that shares nothing at all is still an error, because that is a
    // fault in the cut rather than a tight corner.
    for (bool progress = true; progress;)
    {
      progress = false;

      for (size_t to = 0; to < rooms.size(); ++to)
      {
        if (reached[to] >= 0)
        {
          continue;
        }

        size_t from = rooms.size();
        Position best;
        double longest = 0;

        for (size_t candidate = 0; candidate < rooms.size(); ++candidate)
        {
          if (reached[candidate] < 0 || candidate == to)
          {
            continue;
          }

          Position centre;
          double run = 0;

          if (sharedRun(rooms[candidate].outer, rooms[to].outer, .35, centre, run) && run > longest)
          {
            longest = run;
            best = centre;
            from = candidate;
          }
        }

        if (from == rooms.size())
        {
          continue;
        }

        reached[to] = int(from);
        progress = true;

        DwellingDoor door;
        door.room = ids[to];
        door.fromRoom = ids[from];
        door.floor = floor;
        door.center = Position{best.x, y, best.z};
        door.width = std::max(.7, std::min(1.0, longest * .6));
        door.privateAccess = uses[to] == RoomUse::Sleeping;

        out.doors.push_back(std::move(door));
      }
    }

    for (size_t i = 0; i < rooms.size(); ++i)
    {
      if (reached[i] < 0)
      {
        return fail(error, "a room on floor " + std::to_string(floor) + " shares no wall with any other");
      }
    }

    // The front door.
    //
    // Without a bay it is in the outside wall, where the brief put it. With
    // one it is in the wall *behind* the bay, because that is where the wall
    // is: the bay is roofed but it is not inside, so the door that makes you
    // inside is the one off it. The breadth-first search has already put a
    // door there - it is the wall the bay shares with the building - and what
    // is left to do is to say that this one is the front door.
    if (floor == 0 && porchRoom != noRoom)
    {
      for (DwellingDoor &door : out.doors)
      {
        if (door.floor == 0 && door.room == ids[porchRoom])
        {
          door.exterior = true;
          door.privateAccess = false;
          door.width = std::max(door.width, s.doorWidthMetres);
        }
      }
    }
    else if (floor == 0)
    {
      DwellingDoor entrance;
      entrance.room = ids[arrival];
      entrance.floor = 0;
      entrance.center = b.entrance;
      entrance.width = 1.0;
      entrance.exterior = true;

      out.doors.push_back(std::move(entrance));
    }

    // ---- the stair, in the room furthest from the door --------------------
    //
    // Furniture, not a room: a reserved core is what a rectangular wing can
    // afford and an awkward footprint cannot.
    //
    // At the back, and that is the whole of the rule. A stair is private and a
    // front room is not, so a house of trade puts its stair behind the room it
    // trades in - you do not walk through the taproom to go to bed. Taking the
    // largest room instead put it in the shop, which is the one room in the
    // building it may not be in. Largest breaks the tie, because two back
    // rooms are both at the back and only one of them has space for a flight.
    // ...unless the building has a tower, in which case the stair is in it and
    // is built once for the whole stack below.
    // No stair between two households.
    //
    // A stair from one dwelling into another is a hole in both of them. The
    // upper one is reached from outside - a flight to a gallery, and its front
    // door on that gallery - which is the whole arrangement of a building let
    // by the floor.
    // Against the storeys that were *built*, not the storeys that were asked
    // for.
    //
    // `b.floors` is the brief's wish and `builtFloors` is what the ground
    // could carry, and they differ whenever a plate came out too small to
    // divide. Gated on the wish, the top storey of a building that lost one
    // got a flight climbing to a floor that does not exist - and since the
    // stair is built per storey and the room search stops at the first that
    // fits, that useless flight was sometimes the only one the building had.
    // Every symptom of it was reported as the storey below being unreachable,
    // which is exactly what it was.
    if (floor + 1 < builtFloors && towerStoreys == 0u && !tenement && !order.empty())
    {
      // Ranked by how far back they are, and *tried* in that order rather
      // than picked outright.
      //
      // Picking the furthest room and stopping was right while a stair was a
      // square core - any room with a 1.2 m square in it could hold one. A
      // flight with a landing at each end needs a run, and the furthest room
      // is often a small back chamber that has no run in either direction. So
      // the rule is unchanged in what it wants - the stair goes as far back as
      // it can - and the search now keeps going until it finds a room that can
      // actually take a flight, instead of leaving the storey above with no
      // way up to it.
      std::vector<size_t> byDepth;

      // A corridor whatever its area, and any other room that is big enough.
      //
      // A stair belongs in the circulation of a plan - you come off it onto a
      // passage and not into somebody's chamber - and corridors were being
      // excluded from the search before it began: the filter asked for six
      // square metres and a corridor is a metre and a half wide, so almost
      // none of them qualified. The stair then went to the largest back room
      // every time, which is why so few of them landed on a passage.
      //
      // The entrance bay is never a candidate. It is the ground you walk in
      // over, its street side is not a wall at all, and a flight of steps
      // standing in it is a flight across the way into the building.
      for (const size_t i : order)
      {
        geom2::Point centre;

        if (!geom2::centroid(rooms[i], centre) || i == porchRoom || uses[i] == RoomUse::Entrance)
        {
          continue;
        }

        if (isCorridor[i] || areas[i] >= 6.)
        {
          byDepth.push_back(i);
        }
      }

      // Circulation first, and then the back of the house.
      //
      // A stair belongs in the circulation of a plan, not in a room you have
      // to cross a bedroom to reach - and both of its ends have to open onto
      // something you can walk along, or the flight arrives on the storey
      // above and there is nowhere to go. A corridor is exactly that on both
      // storeys at once, because the corridors of a building stack: they are
      // cut from the same plate by the same rule, so a landing over a corridor
      // is a landing on a corridor.
      //
      // Then depth, which is the older rule and still right: a stair is
      // private, so it goes behind the room the house trades in.
      const auto touchesCorridor = [&](const size_t room) {
        if (isCorridor[room])
        {
          return 2;
        }

        geom2::Point centre;

        if (!geom2::centroid(rooms[room], centre))
        {
          return 0;
        }

        for (size_t i = 0; i < rooms.size(); ++i)
        {
          if (i == room || !isCorridor[i])
          {
            continue;
          }

          if (distanceToRing(rooms[i].outer, geom2::toMetres(centre.x), geom2::toMetres(centre.y)) < 6.)
          {
            return 1;
          }
        }

        return 0;
      };

      std::stable_sort(byDepth.begin(), byDepth.end(), [&](const size_t a, const size_t c) {
        const int circulationA = touchesCorridor(a), circulationC = touchesCorridor(c);

        if (circulationA != circulationC)
        {
          return circulationA > circulationC;
        }

        geom2::Point pa, pc;
        geom2::centroid(rooms[a], pa);
        geom2::centroid(rooms[c], pc);

        return std::hypot(geom2::toMetres(pa.x) - b.entrance.x, geom2::toMetres(pa.y) - b.entrance.z) > std::hypot(geom2::toMetres(pc.x) - b.entrance.x, geom2::toMetres(pc.y) - b.entrance.z);
      });

      if (byDepth.empty() && !order.empty())
      {
        byDepth.push_back(order.front());
      }

      // The rooms, back to front, and then the whole storey.
      //
      // A metre-wide stair with a metre of landing at each end does not fit in
      // every back chamber, and ten storeys in the corpus had no way up to them
      // once the width stopped giving. A stair laid across the plan instead of
      // inside one room is what a real building does when no room can spare the
      // space - it becomes a stair hall - and the machinery for it already
      // exists: the pass that keeps walls out of a stairwell cuts the
      // partitions it crosses, and the pass that closes a storey's outline puts
      // back anything that leaves a hole.
      //
      // Last, though. A stair inside a room is better when a room will take it.
      std::vector<const geom2::Polygon *> candidates;

      for (const size_t i : byDepth)
      {
        candidates.push_back(&rooms[i]);
      }

      // Inside the walls, not up against them.
      //
      // The storey plate is the *outside* of the building, so a stair hall laid
      // to it puts the foot of the flight hard against the front wall - a
      // landing you cannot reach, because every edge of it is either the wall
      // or the steps. Pulled in by a wall's thickness it stands in the room.
      geom2::Polygon stairHall;

      if (floor < storeyPlan.size() && storeyPlan[floor].outer.size() >= 3u)
      {
        std::vector<geom2::Polygon> inner;
        geom2::Polygon ground = storeyPlan[floor];

        if (floor == 0u && porchPlan.outer.size() >= 3u)
        {
          std::vector<geom2::Polygon> left;

          if (geom2::booleanOp({geom2::toOperand(ground), geom2::toOperand(porchPlan)}, geom2::Operation::Difference, {}, left, nullptr) && !left.empty())
          {
            std::sort(left.begin(), left.end(), [](const geom2::Polygon &l, const geom2::Polygon &r) {
              return geom2::areaMetres(l) > geom2::areaMetres(r);
            });

            ground = std::move(left.front());
          }
        }

        if (geom2::offsetPolygon(ground, -s.wallThickness * 2, {}, inner, nullptr) && !inner.empty())
        {
          std::sort(inner.begin(), inner.end(), [](const geom2::Polygon &l, const geom2::Polygon &r) {
            return geom2::areaMetres(l) > geom2::areaMetres(r);
          });

          stairHall = std::move(inner.front());
        }
      }

      if (stairHall.outer.size() >= 3u)
      {
        candidates.push_back(&stairHall);
      }

      bool placed = false;

      for (const geom2::Polygon *host : candidates)
      {
        double loU = 1e300, hiU = -1e300, loV = 1e300, hiV = -1e300;

        for (const geom2::Point &p : host->outer)
        {
          const double x = geom2::toMetres(p.x), z = geom2::toMetres(p.y);
          const double u = x * ux + z * uy, v = -x * uy + z * ux;

          loU = std::min(loU, u);
          hiU = std::max(hiU, u);
          loV = std::min(loV, v);
          hiV = std::max(hiV, v);
        }

        // Laid out as pieces: a landing to start from, the flight, a landing
        // to arrive on - and a second flight with a turn between them where
        // the room is not long enough to take one.
        //
        // It used to be a square core, and the square was the whole stair: the
        // flight, both landings and the well through the floor above were all
        // the same rectangle. Everything that was wrong about a stair followed
        // from that one shortcut. The well was the whole core, so the floor
        // you climbed to was cut away exactly where you arrived. Both landings
        // were the flight, so there was nowhere to stand at either end - and a
        // door in that wall opened onto the third step, which is what the
        // picture of it shows.
        //
        // `planStairPath` already knows how to join legs, so the work here is
        // only choosing where they run: along the longer of the room's two
        // axes, turning at the corner if one leg will not fit.
        const double clearU = hiU - loU - .3, clearV = hiV - loV - .3;

        // A metre wide, always. Everything else gives first: the landing depth
        // down to the width, the riser up to a steep one, then the number of
        // turns, then the room.
        const double width = std::max(s.stairWidthMetres, 1.);

        // The landings are squares of the flight's own width, centred on the
        // steps, so the assembly is that wide across as well as along.
        if (!(clearU > width + .2 && clearV > width + .2))
        {
          continue;
        }

        // Tried from each corner of the room in turn.
        //
        // The assembly is laid out in +u and +v from one corner, and a room is
        // not always a rectangle: on an L the corner it starts from decides
        // whether the far end of the stair is in the room or in the notch. A
        // hundred and forty placements were being thrown away for that alone,
        // and ten storeys ended up with no way up to them - in rooms five
        // metres long, which plainly have room for a stair.
        for (const int su : {1, -1})
        {
        for (const int sv : {1, -1})
        {

        const auto toWorld = [&](const double u, const double v) {
          return Position{u * ux - v * uy, y, u * uy + v * ux};
        };

        // The landing is what gives, not the flight.
        //
        // A fixed metre at each end wanted six metres of run for one storey,
        // and most rooms in most houses have not got it - a third of the
        // storeys in the corpus came out with no way up to them at all, which
        // is a worse building than a tight landing is. So the flight takes the
        // run it needs and the landing takes what is left, down to the width
        // of a doorway. Below that there is no landing to be had and the stair
        // turns instead, which is the answer a real house gives to the same
        // problem.
        //
        // Only the *lower* landing is measured against this room. The upper
        // one belongs to the floor above and is fitted to that storey's plate
        // where the boards are laid, so it costs nothing here.
        // Tried as a small set of attempts rather than as one sum.
        //
        // A storey of 2.8 m at a comfortable riser is sixteen steps, and
        // sixteen steps at the shallowest tread anyone can stand on is 2.85 m
        // of going - so a straight flight with a landing at each end wants
        // better than four metres of room, and most rooms in most houses have
        // not got it. Insisting on it left a third of the storeys with no way
        // up to them, which is a worse building than a steep stair is.
        //
        // So the attempts are, in order of what a builder would rather have: a
        // straight flight at a comfortable riser, a straight flight at a steep
        // one, then the same two turning. A cottage stair *is* steep - that is
        // what cottage stairs are - and the steep figure here is still a stair
        // you walk up rather than a ladder.
        const bool alongU = clearU >= clearV;
        const double clear = alongU ? clearU : clearV;
        // The chosen corner, and the two directions pointing away from it into
        // the room. `at(a, b)` is a point `a` along the first and `b` along the
        // second, so the layout below reads the same whichever corner it is
        // laid out from.
        const double u0 = su > 0 ? loU + .15 : hiU - .15;
        const double v0 = sv > 0 ? loV + .15 : hiV - .15;
        const double aX = ux * double(su), aY = uy * double(su);
        const double bX = -uy * double(sv), bY = ux * double(sv);

        const auto at = [&](const double a, const double c) {
          return toWorld(u0 + double(su) * a, v0 + double(sv) * c);
        };

        std::vector<StairLeg> legs;
        double footprint = 0, landing = s.stairLandingDepthMetres, chosen = s.stairRiserMetres, run = 0, chosenWidth = width;

        // Tried narrower as well as steeper.
        //
        // The landing is a square and a square cannot be narrower than the
        // flight it serves, so the width of the steps sets the *depth* the
        // landings cost - and in a small room that, not the going, is what
        // does not fit. A metre-wide flight in a three-metre room wants a
        // metre of landing at each end and leaves one metre of going, which is
        // not a storey. The same room takes a 750 mm stair comfortably.
        // No narrow flights. A stair is a metre wide or it is not built here;
        // what gives instead is the landing depth, then the riser, then the
        // room - there is always another room to try, and a storey with no way
        // up to it is caught by its own check.
        for (const double narrow : {width})
        {

        for (const double riser : {s.stairRiserMetres, std::max(s.stairRiserMetres, .205)})
        {
          const double across = alongU ? clearV : clearU;
          const StairSteps one = stepsFor(b.floorRise, riser, std::max(clear - narrow * 2, .1));

          if (clear - one.going >= narrow * 2 && one.going > .5)
          {
            landing = std::clamp(std::min((clear - one.going) * .5, across - .2), narrow, s.stairLandingDepthMetres);

            const double spare = (landing - narrow) * .5;

            legs.push_back(alongU
                             ? StairLeg{at(landing, spare), aX, aY, bX, bY}
                             : StairLeg{at(spare, landing), bX, bY, aX, aY});
            footprint = narrow * (landing * 2 + one.going);
            chosen = riser;
            chosenWidth = narrow;
            run = clear - landing * 2;

            break;
          }

          // A dog leg: up one wall, a landing at the corner, back along the
          // next. Each leg climbs half the storey, so each needs half the
          // going - which is what makes this fit where one flight does not.
          const double square = std::min(clearU, clearV);
          const StairSteps half = stepsFor(b.floorRise * .5, riser, std::max(square - narrow * 2, .1));

          landing = std::clamp((square - half.going) * .5, narrow, s.stairLandingDepthMetres);

          // Compared with a hair of slack, because the landing is derived from
          // the smaller of the two extents and then checked against it: asked
          // strictly, the direction that set the landing can never satisfy the
          // test it was computed from, and the turning stair was unreachable
          // in exactly the rooms it exists for.
          if (half.going > .4 && clearU + 1e-6 >= half.going + landing * 2 && clearV + 1e-6 >= half.going + landing * 2)
          {
            const double spare = (landing - narrow) * .5;

            legs.push_back(StairLeg{at(landing, spare), aX, aY, bX, bY});
            legs.push_back(StairLeg{at(landing + half.going + landing - spare, landing), bX, bY, -aX, -aY});
            footprint = narrow * (landing * 3 + half.going * 2);
            chosen = riser;
            chosenWidth = narrow;
            run = square - landing * 2;

            break;
          }

          // Three flights round three sides of the room, turning twice.
          //
          // The same answer the tower needed, for the same reason: a flight is
          // a metre wide and a landing is as deep as a flight is wide, so a
          // straight run wants better than four and a half metres and a single
          // turn about four. Turning twice cuts each flight to a third of the
          // storey - five risers and about a metre of going - and that fits a
          // room three metres square, which most back chambers are.
          //
          // Without it, ten storeys in the corpus had no way up to them once
          // the width stopped giving.
          const StairSteps third = stepsFor(b.floorRise / 3., riser, std::max(square - narrow * 2, .1));
          const double turn = std::clamp((square - third.going) * .5, narrow, s.stairLandingDepthMetres);

          if (third.going > .4 && clearU + 1e-6 >= third.going + turn * 2 && clearV + 1e-6 >= third.going + turn * 2)
          {
            const double spare = (turn - narrow) * .5;
            const double far = turn + third.going + turn - spare;

            legs.push_back(StairLeg{at(turn, spare), aX, aY, bX, bY});
            legs.push_back(StairLeg{at(far, turn), bX, bY, -aX, -aY});
            legs.push_back(StairLeg{at(far - spare, far), -aX, -aY, -bX, -bY});

            landing = turn;
            footprint = narrow * (turn * 4 + third.going * 3);
            chosen = riser;
            chosenWidth = narrow;
            run = square - turn * 2;

            break;
          }
        }

          if (!legs.empty())
          {
            break;
          }
        }

        std::vector<DwellingStair> flights;
        std::vector<std::vector<Position>> turns;

        if (legs.empty() || !planStairPath(legs, y, y + b.floorRise, chosenWidth, chosen, s.floorBoardMetres, floor, flights, turns, landing, run))
        {
          continue;
        }

        // ...and the whole assembly has to be in the room.
        //
        // Checked rather than reasoned about. The arithmetic above says it
        // fits and the arithmetic has been wrong twice, in both cases by
        // leaving a landing out of the sum - and the symptom is a flight
        // finishing in mid air on the far side of the back wall, which is
        // invisible in every measurement of the stair itself. It costs a
        // bounding box, and the answer when it does not fit is to try the next
        // room rather than to build it anyway.
        // Inside the *room*, not inside the box round it.
        //
        // A bounding box is not a room. Rooms are cut from a plate and most of
        // them are rectangles, but the box of one that is not - or of one
        // whose neighbour bites into it - includes ground belonging to the room
        // next door. The landing then lands across the partition: the flight
        // arrives, and 70 mm in front of the top tread there is a wall, with
        // the rest of the landing on the other side of it.
        //
        // Asked of the room polygon this cannot happen, and it is the same
        // question - it was only ever a box because a box is cheaper.
        bool inside = true;

        for (const DwellingStair &flight : flights)
        {
          for (const std::vector<Position> *piece : {&flight.opening, &flight.lowerLanding, &flight.upperLanding})
          {
            for (const Position &q : *piece)
            {
              inside = inside && geom2::contains(*host, geom2::Point{geom2::toUnit(q.x), geom2::toUnit(q.z)});
            }
          }
        }

        if (!inside)
        {
          continue;
        }

        for (DwellingStair &flight : flights)
        {
          flight.privateAccess = true;
          out.stairs.push_back(std::move(flight));
        }

        for (std::vector<Position> &turn : turns)
        {
          record.circulation.push_back(std::move(turn));
        }

        record.coreArea += footprint;
        placed = true;

        break;
        }

        if (placed)
        {
          break;
        }
        }

        if (placed)
        {
          break;
        }
      }
    }

    // ---- the holes in the walls -------------------------------------------
    //
    // A door on a plan is a point on a line, which is all a plan needs. A
    // building needs the hole: a rectangle cut clean through the wall, floor
    // to lintel for a door and sill to head for a window, wide enough that a
    // frame will sit in it without the wall having to be re-cut around the
    // joinery. That is what these are, and they are deliberately the only
    // thing they are - the leaf, the frame, the mullions and the glass are a
    // mesh someone sets into the hole later.
    {
      // Where a wall runs, by id, so an opening can be given ends rather than
      // a centre and a width.
      std::vector<std::pair<uint64_t, size_t>> byId;

      for (size_t w = 0; w < out.walls.size(); ++w)
      {
        if (out.walls[w].floor == floor)
        {
          byId.push_back({out.walls[w].id, w});
        }
      }

      const auto wallOf = [&](const uint64_t id) -> const DwellingWall * {
        for (const auto &[key, index] : byId)
        {
          if (key == id)
          {
            return &out.walls[index];
          }
        }

        return nullptr;
      };

      // A hole in one wall, given where along it and how wide.
      //
      // The cut is the full height of the wall and wider than the joinery by a
      // margin at each end; `sill` and `head` only record where the glass or
      // the leaf is meant to sit inside it. See `DwellingOpening`: the point is
      // to hand whoever models the window a clean rectangle and decide nothing
      // else for them.
      const auto cut = [&](const uint64_t wallId, const uint64_t room, const double centre, const double width, const double sill, const double head, const rewrite::OpeningKind kind) {
        const DwellingWall *wall = wallOf(wallId);

        if (wall == nullptr || wall->kind != rewrite::WallKind::Solid)
        {
          return false;
        }

        const double length = std::hypot(wall->b.x - wall->a.x, wall->b.z - wall->a.z);
        const double margin = s.openingMarginMetres;
        const double span = width + margin * 2;

        // A wall shorter than the hole plus a pier each side is a pier, and a
        // hole in it is a wall that falls down.
        if (length < span + s.wallThickness * 2)
        {
          return false;
        }

        const double from = std::clamp(centre - span * .5, s.wallThickness, length - span - s.wallThickness);
        const double dx = (wall->b.x - wall->a.x) / length, dz = (wall->b.z - wall->a.z) / length;

        // Not where a flight of stairs is.
        //
        // A door opens onto floor. Where a flight runs past a wall there is no
        // floor at that wall - there are treads, at whatever height that part
        // of the climb has reached - so a doorway cut there is a doorway you
        // step out of into the middle of a staircase. It is the fault in the
        // picture and it is not a rendering artefact: the two were planned
        // without reference to each other.
        //
        // The landings are deliberately not included. A landing *is* floor, and
        // a door onto a landing is not a fault but the whole point of one: it
        // is where you walk to in order to start climbing, and where you arrive
        // and walk away. So the flight is what a door must clear, and the
        // landing is what a door may use.
        for (const DwellingStair &flight : out.stairs)
        {
          if (flight.opening.size() < 3u || (flight.lowerFloor != floor && flight.lowerFloor + 1u != floor))
          {
            continue;
          }

          geom2::Polygon run;

          for (const Position &q : flight.opening)
          {
            run.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
          }

          if (geom2::areaMetres(run) < 0)
          {
            std::reverse(run.outer.begin(), run.outer.end());
          }

          if (run.outer.size() < 3u || !geom2::polygonFault(run).empty())
          {
            continue;
          }

          // Sampled across the opening and a door's depth into the room on
          // each side, because what has to be clear is the floor you stand on
          // to use the door, not the line of the wall itself.
          for (int n = 0; n <= 4; ++n)
          {
            const double t = from + span * double(n) / 4.;
            const double px = wall->a.x + dx * t, pz = wall->a.z + dz * t;

            for (const double side : {-1., 1.})
            {
              const double qx = px - dz * side * s.doorClearanceMetres, qz = pz + dx * side * s.doorClearanceMetres;

              if (geom2::contains(run, geom2::Point{geom2::toUnit(qx), geom2::toUnit(qz)}))
              {
                return false;
              }
            }
          }
        }

        // Nothing may overlap anything already cut in this wall, or the two
        // holes merge into one and the pier between them is gone.
        for (const rewrite::DwellingOpening &had : out.openings)
        {
          if (had.wall != wallId)
          {
            continue;
          }

          const double ta = (had.a.x - wall->a.x) * dx + (had.a.z - wall->a.z) * dz;
          const double tb = (had.b.x - wall->a.x) * dx + (had.b.z - wall->a.z) * dz;

          if (std::min(ta, tb) < from + span + s.wallThickness && std::max(ta, tb) > from - s.wallThickness)
          {
            return false;
          }
        }

        rewrite::DwellingOpening hole;
        hole.wall = wallId;
        hole.room = room;
        hole.floor = floor;
        hole.a = Position{wall->a.x + dx * from, wall->a.y, wall->a.z + dz * from};
        hole.b = Position{wall->a.x + dx * (from + span), wall->a.y, wall->a.z + dz * (from + span)};
        // The cut: sized to the joinery it will hold, plus the margin, and
        // nothing else.
        //
        // It used to be the full height of the wall, which was the wrong
        // reading of "cut generously". Generous means the frame has room; it
        // does not mean the wall is gone. A doorway taken to the ceiling is a
        // gap with a door standing in the bottom of it, and a window taken to
        // the floor is a shop front.
        hole.base = std::max(0., sill - s.openingMarginMetres);
        hole.top = std::min((wall->height > 0 ? wall->height : b.floorRise) - s.openingHeadroomMetres, head + s.openingMarginMetres);
        hole.sill = sill;
        hole.head = head;
        hole.thickness = wall->thickness;
        hole.kind = kind;
        hole.exterior = wall->exterior;

        out.openings.push_back(std::move(hole));

        return true;
      };

      // Is another building against this wall?
      //
      // Sampled along the wall rather than at its middle, because a terrace
      // meets its neighbour along part of a flank and stands free along the
      // rest - a house deeper than the one beside it has windows at the back
      // of the same wall that is a party wall at the front. One point cannot
      // say that and five can.
      const auto blocked = [&](const DwellingWall &wall) {
        for (int n = 0; n <= 4; ++n)
        {
          const double t = double(n) / 4.;
          const double x = wall.a.x + (wall.b.x - wall.a.x) * t;
          const double z = wall.a.z + (wall.b.z - wall.a.z) * t;

          for (const std::vector<Position> &near : b.neighbours)
          {
            if (near.size() < 3u)
            {
              continue;
            }

            geom2::Ring ring;

            for (const Position &q : near)
            {
              ring.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
            }

            if (distanceToRing(ring, x, z) < s.windowNeighbourMetres)
            {
              return true;
            }
          }
        }

        return false;
      };

      // Doors: cut in both copies of the wall the two rooms share, and in
      // nothing else.
      //
      // Both, because each room emits the whole of its own ring, so a hole cut
      // in one copy leaves the other standing across it. Nothing else, because
      // "any wall this point lies on" is not a test - a straight run of plan
      // puts four rooms' walls on one line, and an inner door then punched a
      // doorway through the outside wall of a room at the far end of it,
      // opening onto nothing at all.
      for (const DwellingDoor &door : out.doors)
      {
        if (door.floor != floor)
        {
          continue;
        }

        for (size_t i = 0; i < rooms.size(); ++i)
        {
          if (ids[i] != door.room && ids[i] != door.fromRoom)
          {
            continue;
          }

          for (size_t k = 0; k < roomWalls[i].size(); ++k)
          {
            const DwellingWall *wall = wallOf(roomWalls[i][k]);

            if (wall == nullptr)
            {
              continue;
            }

            // Which of the two rooms' shared lines this door is in.
            //
            // Asked as "does it join two rooms", not as "is it the way in".
            // The two are not the same and a porch is where they come apart: a
            // front door behind an entrance bay is the way into the building
            // and it stands in an *interior* wall, because the bay is already
            // inside the outline. Keyed on `exterior` instead, that door
            // matched no wall at all - so the one opening every building needs
            // was never cut, and the furniture, which keeps clear of doorways,
            // had no doorway to keep clear of and put a table across it.
            //
            // A door with no room on the far side is the only one that must be
            // in an outside wall: that is the building with no bay, whose front
            // door is in the street front itself.
            if (wall->exterior != (door.fromRoom == 0))
            {
              continue;
            }

            const double length = std::hypot(wall->b.x - wall->a.x, wall->b.z - wall->a.z);

            if (length < 1e-6)
            {
              continue;
            }

            const double dx = (wall->b.x - wall->a.x) / length, dz = (wall->b.z - wall->a.z) / length;
            const double t = (door.center.x - wall->a.x) * dx + (door.center.z - wall->a.z) * dz;
            const double off = std::abs((door.center.x - wall->a.x) * -dz + (door.center.z - wall->a.z) * dx);

            // On this wall's line, within its run, and not round a corner.
            if (off > s.wallThickness * 1.5 || t < 0 || t > length)
            {
              continue;
            }

            cut(roomWalls[i][k], ids[i], t, std::max(door.width, s.doorWidthMetres), 0, s.doorHeadMetres, rewrite::OpeningKind::Door);
          }
        }
      }

      // Then windows, on the outside walls of rooms that want light. A store
      // does not, a corridor has none to spare, and a tower's shaft is a
      // stair - so those three get nothing, and the rest are lit.
      for (size_t i = 0; i < rooms.size(); ++i)
      {
        const RoomUse use = uses[i];

        if (use == RoomUse::Store || use == RoomUse::Corridor || use == RoomUse::Tower || use == RoomUse::Entrance)
        {
          continue;
        }

        // A merchant shows goods through the ground floor front. A tavern does
        // not, and that is the difference between the two buildings from the
        // street: a shop is a display and a taproom is a room full of people
        // who are already inside. So a taproom is lit like any other room and
        // what it presents to the street is its door.
        const bool trades = floor == 0u && use == RoomUse::Shop && b.use == rewrite::DwellingUse::ShopHouse;
        const rewrite::OpeningKind kind = trades ? rewrite::OpeningKind::ShopWindow : rewrite::OpeningKind::Window;
        // One size for every window of its kind, so one mesh fits all of them.
        const double width = trades ? s.shopWindowWidthMetres : s.windowWidthMetres;
        const double sill = trades ? s.shopWindowSillMetres : s.windowSillMetres;
        const double head = sill + (trades ? s.shopWindowHeightMetres : s.windowHeightMetres);

        for (const uint64_t id : roomWalls[i])
        {
          const DwellingWall *wall = wallOf(id);

          if (wall == nullptr || !wall->exterior || wall->kind != rewrite::WallKind::Solid)
          {
            continue;
          }

          const double length = std::hypot(wall->b.x - wall->a.x, wall->b.z - wall->a.z);

          // Not onto a neighbour. A wall with another building against it is
          // not an outside wall in the sense a window means.
          if (blocked(*wall))
          {
            continue;
          }

          // How many fit: as much of the wall as may be opening, divided by
          // one opening, with a pier's worth of wall between each pair.
          //
          // Counted from the share and not from a spacing, because what a
          // facade is judged on is how much of it is glass. A spacing gives a
          // cottage the same openings as a range and the cottage comes out a
          // screen.
          const int fits = int((length * s.windowWallShare) / width);
          const int spaced = int((length + s.windowPierMetres) / (width + s.windowPierMetres));
          const int count = std::min(std::max(fits, length > width + s.wallThickness * 4 ? 1 : 0), std::max(spaced, 0));

          for (int n = 0; n < count; ++n)
          {
            cut(id, ids[i], length * (double(n) + .5) / double(std::max(count, 1)), width, sill, head, kind);
          }
        }
      }
    }

    // ---- the posts under the bay ------------------------------------------
    //
    // What holds the floor above the entrance up. They stand on the open walls
    // rather than across them - the opening is the whole point - so one goes at
    // each end and the rest are spaced along.
    if (floor == 0u && porchRoom != noRoom)
    {
      for (size_t k = 0; k < roomWalls[porchRoom].size(); ++k)
      {
        const uint64_t id = roomWalls[porchRoom][k];
        const DwellingWall *wall = nullptr;

        for (const DwellingWall &candidate : out.walls)
        {
          wall = candidate.id == id ? &candidate : wall;
        }

        if (wall == nullptr || wall->kind != rewrite::WallKind::Open)
        {
          continue;
        }

        const double length = std::hypot(wall->b.x - wall->a.x, wall->b.z - wall->a.z);

        if (length < 1e-6)
        {
          continue;
        }

        const double dx = (wall->b.x - wall->a.x) / length, dz = (wall->b.z - wall->a.z) / length;

        // The ring is counter clockwise, so out of the room is to the right of
        // the way its edges run. Half a wall out puts the post in the wall's
        // own thickness, where a post in a timber frame is.
        const double ox = dz * s.wallThickness * .5, oz = -dx * s.wallThickness * .5;
        const int bays = std::max(1, int(std::round(length / std::max(s.entrancePostSpacingMetres, 1.))));
        const double half = std::max(s.entrancePostSideMetres, s.wallThickness) * .5;

        for (int n = 0; n <= bays; ++n)
        {
          // Pulled in by its own half width at the ends, so a corner post
          // stands on the building rather than off the end of it.
          const double t = std::clamp(length * double(n) / double(bays), half, length - half);
          const double px = wall->a.x + dx * t + ox, pz = wall->a.z + dz * t + oz;

          // One post to a place.
          //
          // A bay has two or three open walls and the posts are spaced along
          // each, so the corner where two of them meet gets a post from both -
          // and it is the same corner. Drawn, that is one post at double
          // thickness with a seam down it, which is what the renders show at
          // the front of every bay.
          bool already = false;

          for (const rewrite::DwellingPillar &had : out.pillars)
          {
            if (had.footprint.size() < 3u || std::abs(had.base - b.foundation) > .05)
            {
              continue;
            }

            double hx = 0, hz = 0;

            for (const Position &q : had.footprint)
            {
              hx += q.x / double(had.footprint.size());
              hz += q.z / double(had.footprint.size());
            }

            already = already || std::hypot(hx - px, hz - pz) < std::max(half * 2, .3);
          }

          if (already)
          {
            continue;
          }

          rewrite::DwellingPillar post;
          post.base = b.foundation;
          post.top = y + b.floorRise;
          post.footprint = {
            Position{px - dx * half - dz * half, post.base, pz - dz * half + dx * half},
            Position{px + dx * half - dz * half, post.base, pz + dz * half + dx * half},
            Position{px + dx * half + dz * half, post.base, pz + dz * half - dx * half},
            Position{px - dx * half + dz * half, post.base, pz - dz * half - dx * half}};

          out.pillars.push_back(std::move(post));
        }
      }
    }

    record.wallArea = std::max(0., record.grossArea - record.roomArea - record.circulationArea - record.coreArea);
    out.floors.push_back(std::move(record));
  }

  // ---- the outside -------------------------------------------------------
  //
  // The tower is handed over as a part rather than left to the carve, which
  // divides an outline at its reflex corners and has no way to know that one
  // square of it is meant to stand three storeys higher than the rest. Grown
  // by a wall's thickness, because the plan was cut inside the shell and the
  // tower's outside face is the building's.
  std::vector<MassingPart> parts;

  // The hall is a part too, and for the same reason the tower is: its height
  // is not a consequence of its width.
  //
  // Left to the carve, the block the hall stands in is measured and given the
  // storeys its width earns - and a tower notched out of one corner makes
  // that block narrow, so a hall open through two storey heights came out
  // under a one storey wall with the roof cutting through it. The hall knows
  // how tall it is. It says so here.
  const auto partOf = [&](const geom2::Polygon &plan, const uint32_t storeys, const double pitch, const bool tower, const geom2::Polygon *clearOf) {
    std::vector<geom2::Polygon> shell;

    if (!geom2::offsetPolygon(plan, s.wallThickness, {}, shell, nullptr) || shell.empty())
    {
      return;
    }

    std::sort(shell.begin(), shell.end(), [](const geom2::Polygon &l, const geom2::Polygon &r) {
      return geom2::areaMetres(l) > geom2::areaMetres(r);
    });

    geom2::Polygon stand = shell.front();

    // Grown, two parts that merely touched now overlap, and two parts on the
    // same ground is a wall inside a wall.
    if (clearOf != nullptr)
    {
      std::vector<geom2::Polygon> left;

      if (geom2::booleanOp({geom2::toOperand(stand), geom2::toOperand(*clearOf)}, geom2::Operation::Difference, {}, left, nullptr) && !left.empty())
      {
        std::sort(left.begin(), left.end(), [](const geom2::Polygon &l, const geom2::Polygon &r) {
          return geom2::areaMetres(l) > geom2::areaMetres(r);
        });

        stand = std::move(left.front());
      }
    }

    if (geom2::areaMetres(stand) < s.minimumRoomMetres)
    {
      return;
    }

    MassingPart part;
    part.storeys = storeys;
    part.roofPitch = pitch;
    part.tower = tower;

    for (const geom2::Point &q : stand.outer)
    {
      part.footprint.push_back(Position{geom2::toMetres(q.x), b.foundation, geom2::toMetres(q.y)});
    }

    parts.push_back(std::move(part));
  };

  if (towerStoreys > 0u)
  {
    // An open tower is still a tower to the massing - it stands past everything
    // around it and is roofed on its own - but what it is roofed with is
    // nothing. The pitch is dropped to a shallow cap so the shaft ends in a
    // line rather than a point, and the rail round the walk is built below.
    // An open tower is built one storey taller than it ends up.
    //
    // The massing raises a tower until its eaves clear the ridges around it,
    // and the top storey of an open one is then given back to the walk - so
    // asked for at its finished height it comes out a storey short of the roof
    // it is meant to stand over, and the shaft is swallowed by the range it
    // rises from. It asks for the storey it is about to lose.
    //
    // Two, not one. The massing raises a tower until its *eaves* clear the
    // ridges around it by a hand's breadth, which is the right margin for a
    // shaft carrying a spire on top of it - the spire is what you see. An open
    // tower has no spire, so its eaves are its whole height, and what has to
    // stand clear of the roof is the walk itself: a lookout level with the
    // ridge beside it looks at tiles.
    partOf(towerPlan, towerStoreys + (s.towerOpenTop ? 2u : 0u), s.towerOpenTop ? .06 : s.towerRoofPitch, true, nullptr);
  }

  // The hall is a part only when it is taller than the building around it.
  //
  // The part exists to say "this block is two storeys high whatever its width
  // earns", and a block that is exactly as tall as the rest of the building
  // says nothing - it only cuts the mass in two. And a mass cut in two is two
  // masses: the front of the range stops being one edge, so the rule that
  // gables a free end sees a five metre stub instead of a ten metre frontage,
  // and the building comes out gabled at the back and hipped at the street.
  // A merchant's shop and a taproom under a chamber are both exactly as tall
  // as the house over them.
  if (hallFloors > builtFloors && !trading)
  {
    // The hall's mass, less the shaft standing in it.
    //
    // It used to keep the tower's ground: the plan has to hold the two apart
    // because two rooms cannot stand on one patch of floor, and the massing was
    // said to be better roofed without the hole. That was true while nothing
    // trimmed the roof - a hole in a mass is reflex corners and a jog in the
    // ridge. It stopped being true once the roof is cut where the shaft passes
    // through it: the hall is a *separate wing* with a roof of its own, so it
    // roofs the shaft all over again, and cutting the range's roof achieves
    // nothing while the hall's roof is still there.
    if (towerStoreys > 0u && towerPlan.outer.size() >= 3u && !hallMass.outer.empty())
    {
      std::vector<geom2::Polygon> left;

      if (geom2::booleanOp({geom2::toOperand(hallMass), geom2::toOperand(towerPlan)}, geom2::Operation::Difference, {}, left, nullptr) && left.size() == 1u && geom2::areaMetres(left.front()) > s.hallMinimumAreaMetres * .5)
      {
        hallMass = std::move(left.front());
      }
    }

    // Not held clear of the tower at all: the hall runs under it, and the
    // shaft comes up through the hall's roof. The plan keeps the two apart,
    // because two rooms cannot stand on one patch of floor; the massing has no
    // such problem and is much better roofed without the hole.
    partOf(hallMass.outer.empty() ? hallPlan : hallMass, hallFloors, s.hallRoofPitch, false, nullptr);
  }

  // The entrance bay, as ground the building stands over rather than on. Not a
  // wing and not subtracted from the mass: the storeys above it are real and
  // reach the street, and it is the ground storey's plate alone that stops
  // short. Already grown to the building's outer face.
  if (porchMass.outer.size() >= 3u)
  {
    MassingPart bay;
    bay.recess = true;
    bay.storeys = 0;

    for (const geom2::Point &q : porchMass.outer)
    {
      bay.footprint.push_back(Position{geom2::toMetres(q.x), b.foundation, geom2::toMetres(q.y)});
    }

    parts.push_back(std::move(bay));
  }

  // A hall carries its height in its roof, so the roof over one is steeper
  // than the roof over the town around it.
  ComplexDwellingSettings massing = s;
  massing.roofPitch = hallFloors > 0u && !trading ? s.hallRoofPitch : s.roofPitch;

  // ---- how this building is framed ------------------------------------------
  //
  // One choice per building, not per wing: a house has one carpenter, and a
  // range framed one way with a cross wing framed another reads as two
  // buildings that grew together. Taken from the seed so a street varies, and
  // pinned by a setting where something wants to see a particular structure.
  //
  // A hammer beam roof is not put on a cottage. It is the most expensive roof
  // there is and it exists to clear the floor of a hall, so it is offered only
  // where there is a hall or a taproom to clear.
  const rewrite::RoomKind *grandest = nullptr;

  for (const rewrite::DwellingRoom &room : out.rooms)
  {
    if (room.kind == rewrite::RoomKind::GreatHall || room.kind == rewrite::RoomKind::Hall || room.kind == rewrite::RoomKind::TapRoom)
    {
      grandest = &room.kind;
    }
  }

  const RoofFrameStyle roofFrame = [&]() {
    if (!s.roofFrameFromSeed)
    {
      return s.roofFrameStyle;
    }

    const uint32_t pick = uint32_t(draw(b.seed, 0x0f7a, 0, 0) * 100.) % (grandest != nullptr ? 4u : 3u);

    return RoofFrameStyle(pick);
  }();


  // ---- where the chimney stands ---------------------------------------------
  //
  // Its position is settled here and its height later, and the split is not
  // tidiness: the floors have to be cut for a stack and the floors are framed
  // before the building is massed, while how tall the stack must be is not
  // known until there is a ridge to clear. So the footprint comes first and the
  // top is filled in once the roof exists.
  //
  // Nothing new decides where it goes. The hearth is already in the plan - it
  // is furniture, put against a wall in the room a household lives or cooks in
  // - so the stack stands on it, and a building with no hearth has no chimney,
  // which is right. A warehouse does not have one.
  geom2::Polygon chimneyPlan;

  if (s.chimneySideMetres > .2)
  {
    const rewrite::DwellingFurniture *hearth = nullptr;

    for (const rewrite::DwellingFurniture &piece : out.furniture)
    {
      if (piece.kind == rewrite::FurnitureKind::Hearth && piece.floor == 0u && piece.footprint.size() >= 3u && hearth == nullptr)
      {
        hearth = &piece;
      }
    }

    if (hearth != nullptr)
    {
      double cx = 0, cz = 0;

      for (const Position &q : hearth->footprint)
      {
        cx += q.x / double(hearth->footprint.size());
        cz += q.z / double(hearth->footprint.size());
      }

      // Drawn in towards the middle of the plan by half its own width.
      //
      // A hearth sits against a wall, so a stack centred on it straddles that
      // wall - half the masonry inside the room and half hanging in the air
      // outside it. The breast stands *in* the room, which is where the fire
      // is, so the stack moves in until it is clear of the wall.
      geom2::Point middle;

      if (geom2::centroid(footprint, middle))
      {
        const double mx = geom2::toMetres(middle.x), mz = geom2::toMetres(middle.y);
        const double away = std::hypot(mx - cx, mz - cz);

        if (away > 1e-6)
        {
          cx += (mx - cx) / away * s.chimneySideMetres * .5;
          cz += (mz - cz) / away * s.chimneySideMetres * .5;
        }
      }

      const double half = s.chimneySideMetres * .5;

      chimneyPlan.outer = {
        {geom2::toUnit(cx - half), geom2::toUnit(cz - half)},
        {geom2::toUnit(cx + half), geom2::toUnit(cz - half)},
        {geom2::toUnit(cx + half), geom2::toUnit(cz + half)},
        {geom2::toUnit(cx - half), geom2::toUnit(cz + half)}};

      if (!geom2::polygonFault(chimneyPlan).empty())
      {
        chimneyPlan = geom2::Polygon{};
      }
    }
  }

  // ---- the massing ----------------------------------------------------------
  //
  // The shell, the storeys it carries and the roof over them, with the parts
  // gathered above handed over rather than left to the carve: the tower stands
  // higher than its width earns, the hall is as tall as it says it is, and the
  // entrance bay is a recess taken out of the ground storey only.
  massBuilding(b.shell, b.foundation, b.floorRise, massStoreys, massing, out.wings, out.roofs, &out.storeys, &out.pillars, 0, 0, 1, &parts);

  // ---- the boards over the joists -------------------------------------------
  //
  // One surface per storey, at that storey's own level, over the plate the
  // massing gave it - less everything that must not be floored.
  //
  // Built by subtraction, because everything that must not be floored is a
  // shape the plan already holds: the void over a hall open to its roof, the
  // tower shaft the stair winds up inside, and the chimney passing through.
  // Laid without those, the boards are a lid: over the hall, over the inside of
  // the tower, over the flue.
  //
  // A board's thickness above the storey's own level, because that is where the
  // top of a board is - the joists reach the storey line and the boards lie on
  // them. Laid flush the two are coplanar, and the floor reads as a lid let
  // into its own structure.
  for (const rewrite::DwellingStorey &storey : out.storeys)
  {
    const uint32_t floor = storey.level;

    if (floor >= uint32_t(out.floors.size()) || storey.outer.size() < 3u)
    {
      continue;
    }

    geom2::Polygon plate;

    for (const Position &q : storey.outer)
    {
      plate.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
    }

    if (plate.outer.size() >= 3u && !geom2::counterClockwise(plate.outer))
    {
      geom2::reverse(plate.outer);
    }

    for (const std::vector<Position> &hole : storey.holes)
    {
      geom2::Ring ring;

      for (const Position &q : hole)
      {
        ring.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
      }

      if (ring.size() >= 3u)
      {
        if (geom2::counterClockwise(ring))
        {
          geom2::reverse(ring);
        }

        plate.holes.push_back(std::move(ring));
      }
    }

    if (!geom2::polygonFault(plate).empty())
    {
      continue;
    }

    std::vector<geom2::Operand> cuts;

    // The hall stands open from its floor to the roof, so every storey above
    // the one it starts on has a hole in it where the hall is. That hole is
    // what the gallery runs round.
    if (floor > 0u && floor < hallFloors && hallPlan.outer.size() >= 3u && geom2::polygonFault(hallPlan).empty())
    {
      cuts.push_back(geom2::toOperand(hallPlan));
    }

    // The shaft brings its own floor: a landing at each storey, which is what
    // a tower's floor is. A plate laid across it would be a lid over the
    // stair.
    if (towerStoreys > 0u && floor < towerStoreys && towerPlan.outer.size() >= 3u && geom2::polygonFault(towerPlan).empty())
    {
      cuts.push_back(geom2::toOperand(towerPlan));
    }

    // ...and the flue.
    if (chimneyPlan.outer.size() >= 3u && geom2::polygonFault(chimneyPlan).empty())
    {
      cuts.push_back(geom2::toOperand(chimneyPlan));
    }

    // The well a flight rises through.
    //
    // It belongs to the floor *above* the one the flight starts on, which is
    // why it is keyed on `lowerFloor + 1` and not on the flight's own storey:
    // boards across the top of a stair make it a stair to a ceiling.
    for (const rewrite::DwellingStair &flight : out.stairs)
    {
      if (flight.lowerFloor + 1u != floor || flight.opening.size() < 3u)
      {
        continue;
      }

      geom2::Polygon well;

      for (const Position &q : flight.opening)
      {
        well.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
      }

      if (well.outer.size() >= 3u && !geom2::counterClockwise(well.outer))
      {
        geom2::reverse(well.outer);
      }

      if (geom2::polygonFault(well).empty())
      {
        cuts.push_back(geom2::toOperand(well));
      }
    }

    // ...and the entrance bay, which is ground the building stands over rather
    // than on. The storey plate already gives it up; the boards must not put
    // it back.
    if (floor == 0u)
    {
      for (const MassingPart &part : parts)
      {
        if (!part.recess || part.footprint.size() < 3u)
        {
          continue;
        }

        geom2::Polygon bay;

        for (const Position &q : part.footprint)
        {
          bay.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
        }

        if (bay.outer.size() >= 3u && !geom2::counterClockwise(bay.outer))
        {
          geom2::reverse(bay.outer);
        }

        if (geom2::polygonFault(bay).empty())
        {
          cuts.push_back(geom2::toOperand(bay));
        }
      }
    }

    std::vector<geom2::Polygon> left{plate};

    if (!cuts.empty() && !geom2::differenceOf(geom2::toOperand(plate), cuts, left, nullptr))
    {
      left = {plate};
    }

    const double y = b.foundation + double(floor) * b.floorRise + s.floorBoardMetres;

    for (const geom2::Polygon &piece : left)
    {
      if (piece.outer.size() < 3u || geom2::areaMetres(piece) < .12)
      {
        continue;
      }

      std::vector<std::array<uint32_t, 3>> triangles;
      std::vector<geom2::Point> vertices;

      // Triangulated rather than fanned: a plate with a hole in it is not a fan
      // from one vertex, and a storey plate can be an L.
      if (!geom3::triangulate(piece, triangles, vertices, nullptr))
      {
        continue;
      }

      for (const std::array<uint32_t, 3> &face : triangles)
      {
        out.floors[floor].slabs.push_back({Position{geom2::toMetres(vertices[face[0]].x), y, geom2::toMetres(vertices[face[0]].y)},
                                           Position{geom2::toMetres(vertices[face[1]].x), y, geom2::toMetres(vertices[face[1]].y)},
                                           Position{geom2::toMetres(vertices[face[2]].x), y, geom2::toMetres(vertices[face[2]].y)}});
      }
    }
  }

  // ---- how tall the chimney is ----------------------------------------------
  //
  // Clear of the ridge it passes rather than level with it. Level with the
  // tiles a stack is a lump on a roof; the height above is what makes it read
  // from the street, and it is also what makes it draw.
  //
  // The ridge it passes, and the ones close enough to spoil the draw - not the
  // tallest on the building. Those are different numbers on anything with a
  // tower: mansion-wings hearths in a single storey range and carries a tower
  // seven storeys up twenty metres away, and clearing *that* gave a stack that
  // rose past the spire, thinner than a flagpole and taller than the house.
  //
  // A stack has to beat what stands over it because a roof rising beside it
  // drives the wind down the flue, and what does that is a roof close by. Far
  // enough off and it is a different building as far as the wind is concerned.
  // Two and a half metres is the distance the trade uses.
  if (chimneyPlan.outer.size() >= 3u)
  {
    double cx = 0, cz = 0;

    for (const geom2::Point &q : chimneyPlan.outer)
    {
      cx += geom2::toMetres(q.x);
      cz += geom2::toMetres(q.y);
    }

    cx /= double(chimneyPlan.outer.size());
    cz /= double(chimneyPlan.outer.size());

    double highest = b.foundation;

    for (const rewrite::DwellingWing &w : out.wings)
    {
      if (w.footprint.size() < 3u)
      {
        continue;
      }

      bool in = false;
      double away = 1e300;

      for (size_t i = 0, j = w.footprint.size() - 1; i < w.footprint.size(); j = i++)
      {
        const Position &p = w.footprint[i], &q = w.footprint[j];

        if ((p.z > cz) != (q.z > cz) && cx < (q.x - p.x) * (cz - p.z) / (q.z - p.z) + p.x)
        {
          in = !in;
        }

        const double ex = q.x - p.x, ez = q.z - p.z;
        const double length = ex * ex + ez * ez;
        const double at = length < 1e-12 ? 0 : std::clamp(((cx - p.x) * ex + (cz - p.z) * ez) / length, 0., 1.);

        away = std::min(away, std::hypot(p.x + ex * at - cx, p.z + ez * at - cz));
      }

      if (in || away < 2.5)
      {
        highest = std::max(highest, w.ridgeHeight);
      }
    }

    if (highest > b.foundation + 1)
    {
      rewrite::DwellingPillar stack;
      stack.base = b.foundation;
      stack.top = highest + s.chimneyAboveRidgeMetres;

      for (const geom2::Point &q : chimneyPlan.outer)
      {
        stack.footprint.push_back(Position{geom2::toMetres(q.x), stack.base, geom2::toMetres(q.y)});
      }

      out.chimneys.push_back(std::move(stack));
    }
  }

  // ---- the roof stops where the tower goes through it -----------------------
  //
  // A tower is deliberately not cut out of the mass: cut out, it leaves a notch
  // in the outline, and every notch is two more reflex corners for the roof to
  // deal with - a stub ridge along the lip beside the shaft, stepping up where
  // the range proper begins, and a gable each side of the bite. So the range
  // underneath is whatever it would have been and the shaft passes up through
  // it, which is right.
  //
  // What was missing is the other half of "passes through": the roof surface
  // was left whole, so it ran straight into the tower's walls and out the far
  // side. A shaft through a roof needs a hole in the roof.
  //
  // Trimmed in plan and lifted back onto each face's own plane. A roof face is
  // planar, so the height over any point of it is fixed by its plane, and the
  // vertices the trim introduces are on the face already - the boolean is the
  // whole of the work and the third coordinate follows.
  if ((towerStoreys > 0u && towerPlan.outer.size() >= 3u) || !out.chimneys.empty())
  {
    std::vector<geom2::Polygon> shafts;

    for (const MassingPart &part : parts)
    {
      if (!part.tower || part.footprint.size() < 3u)
      {
        continue;
      }

      geom2::Polygon shaft;

      for (const Position &q : part.footprint)
      {
        shaft.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
      }

      if (geom2::areaMetres(shaft) < 0)
      {
        std::reverse(shaft.outer.begin(), shaft.outer.end());
      }

      if (geom2::polygonFault(shaft).empty())
      {
        shafts.push_back(std::move(shaft));
      }
    }

    // ...and to a chimney, which is the same thing at a smaller scale: a
    // masonry stack passing up through a roof needs the roof to stop at it, or
    // the tiles run through the brickwork and out the other side.
    for (const rewrite::DwellingPillar &stack : out.chimneys)
    {
      geom2::Polygon flue;

      for (const Position &q : stack.footprint)
      {
        flue.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
      }

      if (geom2::areaMetres(flue) < 0)
      {
        std::reverse(flue.outer.begin(), flue.outer.end());
      }

      if (geom2::polygonFault(flue).empty())
      {
        shafts.push_back(std::move(flue));
      }
    }

    if (!shafts.empty())
    {
      std::vector<rewrite::DwellingRoofFace> kept;

      for (const rewrite::DwellingRoofFace &face : out.roofs)
      {
        const bool onTower = face.wing < out.wings.size() && out.wings[face.wing].tower;

        // The tower's own roof is what stands on the shaft; it is not the
        // thing being cut. Nor is a gable, which is a wall and not a surface
        // over the plan.
        if (onTower || face.gable || face.polygon.size() < 3u)
        {
          kept.push_back(face);

          continue;
        }

        // The plane of this face: y = (d - nx x - nz z) / ny, from its own
        // vertices. A face with no fall - one seen edge on - has no such
        // plane and is left alone.
        double nx = 0, ny = 0, nz = 0;

        for (size_t i = 0; i < face.polygon.size(); ++i)
        {
          const Position &u = face.polygon[i];
          const Position &v = face.polygon[(i + 1) % face.polygon.size()];

          nx += (u.y - v.y) * (u.z + v.z);
          ny += (u.z - v.z) * (u.x + v.x);
          nz += (u.x - v.x) * (u.y + v.y);
        }

        if (std::abs(ny) < 1e-6)
        {
          kept.push_back(face);

          continue;
        }

        const double d = nx * face.polygon.front().x + ny * face.polygon.front().y + nz * face.polygon.front().z;

        geom2::Polygon flat;

        for (const Position &q : face.polygon)
        {
          flat.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
        }

        if (geom2::areaMetres(flat) < 0)
        {
          std::reverse(flat.outer.begin(), flat.outer.end());
        }

        std::vector<geom2::Operand> difference{geom2::toOperand(flat)};

        for (const geom2::Polygon &shaft : shafts)
        {
          difference.push_back(geom2::toOperand(shaft));
        }

        std::vector<geom2::Polygon> left;

        if (!geom2::polygonFault(flat).empty() || !geom2::booleanOp(difference, geom2::Operation::Difference, {}, left, nullptr))
        {
          kept.push_back(face);

          continue;
        }

        // Untouched by the shaft: keep the face as it was, rather than as the
        // boolean's reconstruction of it.
        //
        // Judged on the *hole* as well as on the area. A shaft standing in the
        // middle of a roof face leaves one polygon whose outer ring is the
        // original and whose hole is the shaft - and an area that ignores holes
        // reports it as unchanged, so the face went back untrimmed and the roof
        // ran through the tower exactly as before. A tower at the edge of a
        // building leaves no hole, which is why those came out clean and the
        // one in the middle of a hall did not.
        if (left.size() == 1u && left.front().holes.empty() && std::abs(geom2::areaMetres(left.front()) - geom2::areaMetres(flat)) < .01)
        {
          kept.push_back(face);

          continue;
        }

        for (const geom2::Polygon &piece : left)
        {
          if (geom2::areaMetres(piece) < .05)
          {
            continue;
          }

          // Triangulated, because the difference can have a hole in it.
          //
          // A shaft standing in the *middle* of a roof face leaves a ring with
          // a hole, and rebuilding the face from its outer ring alone throws
          // the hole away - so the trim ran, reported success, and put back a
          // surface that covered the shaft exactly as before. That is why the
          // tower at the edge of a building came out clean and the one in the
          // middle of one did not.
          //
          // A roof face is a sampled wavefront anyway: it arrives as a few
          // dozen triangles and is drawn as them, so this changes nothing about
          // how it reads.
          std::vector<std::array<uint32_t, 3>> triangles;
          std::vector<geom2::Point> vertices;

          if (!geom3::triangulate(piece, triangles, vertices, nullptr))
          {
            continue;
          }

          for (const auto &tri : triangles)
          {
            rewrite::DwellingRoofFace cut;
            cut.wing = face.wing;
            cut.gable = face.gable;
            cut.soffit = face.soffit;

            for (int k = 0; k < 3; ++k)
            {
              const double x = geom2::toMetres(vertices[tri[k]].x), z = geom2::toMetres(vertices[tri[k]].y);

              cut.polygon.push_back(Position{x, (d - nx * x - nz * z) / ny, z});
            }

            kept.push_back(std::move(cut));
          }
        }
      }

      out.roofs = std::move(kept);
    }
  }

  // The massing raises a tower until its eaves clear the ridges around it, so
  // how tall the tower actually is, is settled out there and not in here. The
  // rooms and the stair follow the building rather than the other way round -
  // otherwise the shaft has three floors of plan inside five storeys of wall.
  for (const rewrite::DwellingWing &w : out.wings)
  {
    towerStoreys = w.tower ? std::max(towerStoreys, w.storeys) : towerStoreys;
  }

  // ---- the tower: the same square, every storey, all the way up -----------
  //
  // Its ground floor was planned with the rest. Above that it is built here,
  // because above that it is not part of any floor's plan: the top of a tower
  // stands over a building that has already stopped, with nothing beside it
  // to share a wall with, and the door search would call that a room it
  // cannot reach. What reaches it is the stair, which is the other half of
  // why the tower exists.
  if (towerStoreys > 0u)
  {
    const double side = std::sqrt(std::max(geom2::areaMetres(towerPlan), 1e-6));

    // The rooms on the floors above the ground one.
    for (uint32_t floor = 1; floor < towerStoreys; ++floor)
    {
      const double y = b.foundation + floor * b.floorRise;

      if (floor >= uint32_t(out.floors.size()))
      {
        DwellingFloor record;
        record.elevation = y;
        record.height = b.floorRise;
        record.grossArea = geom2::areaMetres(towerPlan);
        record.coreArea = geom2::areaMetres(towerPlan);

        out.floors.push_back(std::move(record));
      }

      DwellingRoom room;
      room.id = roomId++;
      room.floor = floor;
      room.use = RoomUse::Tower;
      room.kind = rewrite::RoomKind::Staircase;
      room.usableArea = geom2::areaMetres(towerPlan);
      room.ceilingHeight = y + b.floorRise;

      for (const geom2::Point &q : towerPlan.outer)
      {
        room.boundary.push_back(Position{geom2::toMetres(q.x), y, geom2::toMetres(q.y)});
      }

      // A door onto whatever the tower stands beside on this floor, where the
      // building still reaches it. Above the building there is nothing to
      // open onto, and the stair is the only way in - which is what the top
      // of a tower is like.
      for (const DwellingRoom &other : out.rooms)
      {
        if (other.floor != floor || other.use == RoomUse::Tower || other.boundary.size() < 3u)
        {
          continue;
        }

        geom2::Ring ring;

        for (const Position &q : other.boundary)
        {
          ring.push_back(at(q.x, q.z));
        }

        Position centre;
        double run = 0;

        if (!sharedRun(towerPlan.outer, ring, .9, centre, run))
        {
          continue;
        }

        DwellingDoor door;
        door.room = room.id;
        door.floor = floor;
        door.center = Position{centre.x, y, centre.z};
        door.width = std::min(1.0, run * .6);

        out.doors.push_back(std::move(door));

        break;
      }

      for (size_t k = 0; k < towerPlan.outer.size(); ++k)
      {
        const geom2::Point &q = towerPlan.outer[k];
        const geom2::Point &r = towerPlan.outer[(k + 1) % towerPlan.outer.size()];

        DwellingWall wall;
        wall.id = wallId++;
        wall.floor = floor;
        wall.a = Position{geom2::toMetres(q.x), y, geom2::toMetres(q.y)};
        wall.b = Position{geom2::toMetres(r.x), y, geom2::toMetres(r.y)};
        wall.thickness = s.wallThickness;
        wall.exterior = floor >= builtFloors || !geom2::contains(storeyPlan[std::min(floor, builtFloors - 1u)], at((geom2::toMetres(q.x) + geom2::toMetres(r.x)) * .5, (geom2::toMetres(q.y) + geom2::toMetres(r.y)) * .5));

        // A window in each outside face of the shaft.
        //
        // The tower was the one part of a building with no openings at all, and
        // it is the part most seen: it stands above everything around it. One
        // to a face and no more - a tower is a stair, and what a stair wants is
        // enough light to climb by.
        const double run = std::hypot(wall.b.x - wall.a.x, wall.b.z - wall.a.z);

        if (wall.exterior && run > s.windowWidthMetres + s.wallThickness * 4)
        {
          const double dx = (wall.b.x - wall.a.x) / run, dz = (wall.b.z - wall.a.z) / run;
          const double span = s.windowWidthMetres + s.openingMarginMetres * 2;
          const double from = (run - span) * .5;

          rewrite::DwellingOpening hole;
          hole.wall = wall.id;
          hole.room = room.id;
          hole.floor = floor;
          hole.a = Position{wall.a.x + dx * from, y, wall.a.z + dz * from};
          hole.b = Position{wall.a.x + dx * (from + span), y, wall.a.z + dz * (from + span)};
          hole.sill = s.windowSillMetres;
          hole.head = s.windowSillMetres + s.windowHeightMetres;
          hole.base = std::max(0., hole.sill - s.openingMarginMetres);
          hole.top = std::min(b.floorRise - s.openingHeadroomMetres, hole.head + s.openingMarginMetres);
          hole.thickness = wall.thickness;
          hole.kind = rewrite::OpeningKind::Window;
          hole.exterior = true;

          out.openings.push_back(std::move(hole));
        }

        out.walls.push_back(std::move(wall));
      }

      out.rooms.push_back(std::move(room));
    }

    // ---- the stair ---------------------------------------------------------
    //
    // Two kinds, and the difference is not how tall they are.
    //
    // A stair in a room joins one floor to the next: one flight, filling
    // whatever corner it was given. A stair in a tower climbs the inside face
    // of the shaft, turning at every corner, and serves every storey the tower
    // passes - which is the whole reason a building has a tower. It winds
    // around a newel, which is the wall in the middle, and the newel is not
    // decoration: it carries the inner end of every tread and it is what makes
    // a shaft safe to climb.
    //
    // The data model needs nothing new for it. `DwellingStair::startRise` was
    // put there so the second flight of a dog-leg could begin above the floor,
    // and a flight to each side of a square shaft is the same fact four times:
    // each starts a quarter of a storey higher than the one before.
    // How a newel stair fits this shaft, if it does: how wide the flights are
    // and how many of them there are to a storey.
    //
    // A stair is a metre wide, and a landing is as deep as the stair is wide,
    // so one side of the shaft has to hold a metre of landing, the going, and
    // another metre of landing. A storey of 2.8 m is fifteen risers and fifteen
    // risers want better than two and a half metres of going, which needs a
    // four and a half metre side - wider than most towers.
    //
    // So the stair turns more often. Two flights to a storey climb half of it
    // each, which is eight risers and about 1.7 m of going, and that fits a
    // four metre shaft with a metre of landing at both ends. That is what a
    // newel stair is: it winds, and how often it winds follows from the shaft.
    //
    // Nothing gives on the width. If the shaft cannot hold a metre-wide stair
    // with room to turn, it does not get a newel and falls through to the plain
    // one-flight-a-storey arrangement below.
    double newelWidth = 0;
    uint32_t newelLegs = 0;

    if (s.towerNewelStair && towerPlan.outer.size() == 4u)
    {
      for (const uint32_t legs : {1u, 2u, 3u})
      {
        const double want = std::max(s.towerStairWidthMetres, 1.);
        const double going = stepsFor(b.floorRise / double(legs), s.towerRiserMetres, std::max(side - want * 2, .2)).going;

        if (going > .4 && going + want * 2 <= side + .01)
        {
          newelWidth = want;
          newelLegs = legs;

          break;
        }
      }
    }

    const bool newel = newelLegs > 0u;

    // Where the shaft is entered on each storey: the corner the flights of that
    // storey begin at. The door goes here, and a door that opens onto a step
    // instead is the commonest way a generated stair is obviously wrong.
    std::vector<Position> landings(towerStoreys, Position{});

    // ...and whether each one was actually set. Not "is its height nought":
    // the ground floor *is* at nought, so that sentinel silently excluded the
    // one storey every tower has.
    std::vector<bool> hasLanding(towerStoreys, false);

    if (newel)
    {
      // The tread reaches the newel.
      //
      // A fixed width in from the wall and a fixed square in the middle leaves
      // whatever is between them, and what was between them was a gap the stair
      // fell into. The newel is the fixed thing - it is the core the flights
      // wind around and it carries their inner ends - so the tread is what is
      // left of the side once the core has had its half.
      const double width = newelWidth;
      const double core = std::max(side - width * 2, .0);

      // One flight to a storey, along one side of the shaft, turning to the
      // next side at every floor.
      //
      // Four flights to a storey was the first arrangement and it does not
      // work at this size. A quarter of a storey is seven hundred millimetres,
      // and a flight is as long as its steps make it - so each one ran a metre
      // and stopped, and a storey came out as four short ramps with gaps
      // between them. Stretching them to fill the side instead gave treads a
      // metre deep, which is a ramp with trip hazards.
      //
      // A side of the shaft is about four metres, and four metres of going at a
      // proper tread is fifteen steps, and fifteen steps at a proper riser is
      // very nearly one storey. So the side and the storey are the same length
      // of stair: one flight climbs one side and arrives at the next corner,
      // and the tower turns a quarter every floor. That is what a newel stair
      // in a square tower actually is.
      // Which side the climb begins on: the one you come in at.
      //
      // Starting at side nought is starting at whichever corner the outline
      // happens to list first, and the way into a tower is decided elsewhere -
      // by which wall the shaft shares most with the room beside it. Those two
      // agreed by luck, and when they did not the first flight began at the far
      // corner of the shaft from the only door into it.
      uint32_t first = 0;

      {
        double nearest = 1e300;

        for (const DwellingDoor &door : out.doors)
        {
          if (door.floor != 0u || !geom2::contains(towerPlan, at(door.center.x, door.center.z)))
          {
            continue;
          }

          for (uint32_t e = 0; e < 4u; ++e)
          {
            const geom2::Point &q = towerPlan.outer[e];
            const double reach = std::hypot(geom2::toMetres(q.x) - door.center.x, geom2::toMetres(q.y) - door.center.z);

            if (reach < nearest)
            {
              nearest = reach;
              first = e;
            }
          }
        }
      }

      for (uint32_t step = 0; (step / newelLegs) + 1u < towerStoreys; ++step)
      {
        const uint32_t floor = step / newelLegs;
        const uint32_t leg = step % newelLegs;
        const double climb = b.floorRise / double(newelLegs);
        const double y = b.foundation + floor * b.floorRise + climb * double(leg);
        const uint32_t k = (first + step) % 4u;

        const geom2::Point &from = towerPlan.outer[k];
        const geom2::Point &to = towerPlan.outer[(k + 1u) % 4u];

        const double ax = geom2::toMetres(from.x), az = geom2::toMetres(from.y);
        const double bx = geom2::toMetres(to.x), bz = geom2::toMetres(to.y);
        const double run = std::hypot(bx - ax, bz - az);

        if (run < width * 2)
        {
          continue;
        }

        const double dx = (bx - ax) / run, dz = (bz - az) / run;

        // Counter clockwise, so into the shaft is to the left of the way the
        // ring runs.
        const double nx = -dz, nz = dx;

        const auto corner = [&](const double along, const double into, const double height) {
          return Position{ax + dx * along + nx * into, height, az + dz * along + nz * into};
        };

        // The flight starts a landing in from the corner it leaves and ends a
        // landing short of the one it arrives at, so there is somewhere to
        // stand at both ends and the turn is made on the flat.
        //
        // The landing is a pace deep, not a tread deep. Tying it to the width
        // of the stair was the obvious spelling and it is circular: widening
        // the tread to reach the newel widened the landings with it, the two
        // of them ate the whole side, and every flight in every tower was
        // dropped for not fitting - so the towers came out with no stair at
        // all.
        // The landing is as deep as the flight is wide: a corner square you can
        // turn round in. It used to be capped at a pace, which is what made the
        // head of every flight a strip against the shaft wall.
        const double pace = width;
        const double foot = pace, head = run - pace;

        if (head - foot < 1.2)
        {
          continue;
        }

        // The steps: as many as the rise needs, and the going follows. A tread
        // shallower than it should be is better than a flight that stops half
        // way up, so the going is what the run allows once the number of steps
        // is fixed by the climb.
        const StairSteps steps = stepsFor(climb, s.towerRiserMetres, head - foot);
        const uint32_t risers = steps.risers;

        // The flight runs as far as its steps take it, or as far as the side
        // allows - whichever is shorter. What is left is landing.
        const double climbed = std::min(head - foot, steps.going);

        DwellingStair flight;
        flight.opening = {corner(foot, width, y), corner(foot, 0, y), corner(foot + climbed, 0, y), corner(foot + climbed, width, y)};
        flight.lowerLanding = {corner(0, width, y), corner(0, 0, y), corner(foot, 0, y), corner(foot, width, y)};
        flight.upperLanding = {corner(foot + climbed, width, y), corner(foot + climbed, 0, y), corner(run, 0, y), corner(run, width, y)};
        flight.lowerFloor = floor;
        flight.risers = risers;
        flight.tread = steps.tread;
        flight.rise = climb;
        flight.startRise = climb * double(leg);
        flight.width = width;
        flight.privateAccess = false;

        out.stairs.push_back(std::move(flight));

        // The door onto a storey is at the landing the climb *starts* from and
        // the one it *finishes* at - not at the turns in between.
        if (leg == 0u)
        {
          landings[floor] = corner(foot * .5, width * .5, y);
          hasLanding[floor] = true;
        }

        if (leg + 1u == newelLegs)
        {
          landings[floor + 1u] = corner(run - width * .5, width * .5, y + climb);
          hasLanding[floor + 1u] = true;
        }

        // The landings, as floor you can stand on.
        //
        // A tower storey does not want a floor across the whole shaft - the
        // stair is going through it - and it does want somewhere at each level
        // that is not a step: a square joining the flight that arrives, the
        // flight that leaves, and the door. That is the whole of what a tower's
        // floor is, and it is the landing.
        const auto platform = [&](const std::vector<Position> &quad, const double height, const uint32_t level) {
          if (level >= uint32_t(out.floors.size()) || quad.size() < 4u)
          {
            return;
          }

          out.floors[level].slabs.push_back({Position{quad[0].x, height, quad[0].z}, Position{quad[1].x, height, quad[1].z}, Position{quad[2].x, height, quad[2].z}});
          out.floors[level].slabs.push_back({Position{quad[0].x, height, quad[0].z}, Position{quad[2].x, height, quad[2].z}, Position{quad[3].x, height, quad[3].z}});
        };

        // A leg of a newel stair climbs a *quarter* of a storey, so its upper
        // landing is a quarter of a storey above its foot.
        //
        // This said `y + b.floorRise`, which is a whole one - so every landing
        // in every tower was laid three quarters of a storey too high, and the
        // ones belonging to the top storey came out above the walk entirely:
        // a metre of boards hanging in the air off the corner of an open
        // tower, which is the floating slab in every render of one.
        //
        // The level is only which bucket of floors the boards are filed under,
        // and a landing partway up a storey belongs to the storey it is in.
        platform(out.stairs.back().lowerLanding, y + s.floorBoardMetres, floor);
        platform(out.stairs.back().upperLanding, y + climb + s.floorBoardMetres, leg + 1u == newelLegs ? floor + 1u : floor);

        // ...and floor from the landing to the door, along the wall the door is
        // in. A landing at the stair's foot and a door somewhere else on the
        // wall is a door you step out of onto nothing.
        for (const DwellingDoor &door : out.doors)
        {
          if (door.floor != floor || !geom2::contains(towerPlan, at(door.center.x, door.center.z)))
          {
            continue;
          }

          for (uint32_t e = 0; e < 4u; ++e)
          {
            const geom2::Point &p0 = towerPlan.outer[e];
            const geom2::Point &p1 = towerPlan.outer[(e + 1u) % 4u];

            const double ex = geom2::toMetres(p0.x), ez = geom2::toMetres(p0.y);
            const double fx = geom2::toMetres(p1.x), fz = geom2::toMetres(p1.y);
            const double along = std::hypot(fx - ex, fz - ez);

            if (along < 1e-6)
            {
              continue;
            }

            const double ux = (fx - ex) / along, uz = (fz - ez) / along;
            const double vx = -uz, vz = ux;
            const double t = (door.center.x - ex) * ux + (door.center.z - ez) * uz;
            const double off = std::abs((door.center.x - ex) * vx + (door.center.z - ez) * vz);

            if (off > s.wallThickness * 3 || t < -.2 || t > along + .2)
            {
              continue;
            }

            // A strip of floor from the corner the stair starts at, out to the
            // door, one tread wide.
            const double lo = std::min(t, 0.) - s.doorWidthMetres * .6;
            const double hi = std::max(t, 0.) + s.doorWidthMetres * .6;

            const auto step = [&](const double a, const double c) {
              return Position{ex + ux * a + vx * c, y + s.floorBoardMetres, ez + uz * a + vz * c};
            };

            out.floors[floor].slabs.push_back({step(std::max(lo, 0.), 0), step(std::min(hi, along), 0), step(std::min(hi, along), width)});
            out.floors[floor].slabs.push_back({step(std::max(lo, 0.), 0), step(std::min(hi, along), width), step(std::max(lo, 0.), width)});
          }
        }
      }

      // The newel: a solid core in the middle of the well, one storey at a
      // time, that the flights wind around.
      if (s.towerNewelSideMetres > .2)
      {
        geom2::Point middle;

        if (geom2::centroid(towerPlan, middle))
        {
          const double half = core * .5;

          if (half > .15)
          {
            const double cx = geom2::toMetres(middle.x), cz = geom2::toMetres(middle.y);

            for (uint32_t floor = 0; floor < towerStoreys; ++floor)
            {
              const double y = b.foundation + floor * b.floorRise;

              const Position square[4] = {
                Position{cx - half, y, cz - half},
                Position{cx + half, y, cz - half},
                Position{cx + half, y, cz + half},
                Position{cx - half, y, cz + half}};

              for (uint32_t k = 0; k < 4u; ++k)
              {
                DwellingWall core;
                core.id = wallId++;
                core.floor = floor;
                core.a = square[k];
                core.b = square[(k + 1u) % 4u];
                core.thickness = s.wallThickness;
                core.exterior = false;

                out.walls.push_back(std::move(core));
              }
            }
          }
        }
      }
    }
    else
    {
      // The simple stair: one flight to a storey, filling the shaft.
      for (uint32_t floor = 0; floor + 1 < towerStoreys; ++floor)
      {
        const double y = b.foundation + floor * b.floorRise;

        const auto landing = [&](const double height) {
          std::vector<Position> ring;

          for (const geom2::Point &q : towerPlan.outer)
          {
            ring.push_back(Position{geom2::toMetres(q.x), height, geom2::toMetres(q.y)});
          }

          return ring;
        };

        DwellingStair stair;
        stair.opening = landing(y + b.floorRise);
        stair.lowerLanding = landing(y);
        stair.upperLanding = landing(y + b.floorRise);
        stair.lowerFloor = floor;
        const StairSteps plain = stepsFor(b.floorRise, .18, 0);

        stair.risers = plain.risers;
        stair.tread = plain.tread;
        stair.rise = b.floorRise;
        stair.width = std::min(1.1, std::max(.8, side * .4));
        stair.privateAccess = false;

        out.stairs.push_back(std::move(stair));
      }
    }

    // A window the stair runs across comes out.
    //
    // The flights wind up the inside face of the shaft, so they pass in front
    // of whatever is in that face - and a window with a flight of steps across
    // the middle of it is worse than no window. Moving it is not available: a
    // newel stair covers every side of every storey at some height, so there is
    // nowhere on that wall to move it to. What is available is not cutting it.
    if (newel)
    {
      out.openings.erase(std::remove_if(out.openings.begin(), out.openings.end(), [&](const rewrite::DwellingOpening &hole) {
                           if (hole.kind == rewrite::OpeningKind::Door || !geom2::contains(towerPlan, at((hole.a.x + hole.b.x) * .5, (hole.a.z + hole.b.z) * .5)))
                           {
                             return false;
                           }

                           const double mx = (hole.a.x + hole.b.x) * .5, mz = (hole.a.z + hole.b.z) * .5;
                           const double sill = hole.a.y + hole.base, head = hole.a.y + hole.top;

                           for (const DwellingStair &flight : out.stairs)
                           {
                             if (flight.opening.size() != 4u)
                             {
                               continue;
                             }

                             // Where along the flight this window sits, and how
                             // high the steps are there.
                             const Position &p = flight.opening[0];
                             const Position &q = flight.opening[3];
                             const double span = std::hypot(q.x - p.x, q.z - p.z);

                             if (span < 1e-6)
                             {
                               continue;
                             }

                             const double dx = (q.x - p.x) / span, dz = (q.z - p.z) / span;
                             const double t = (mx - p.x) * dx + (mz - p.z) * dz;
                             const double off = std::abs((mx - p.x) * -dz + (mz - p.z) * dx);

                             if (t < -.4 || t > span + .4 || off > flight.width + .4)
                             {
                               continue;
                             }

                             const double base = b.foundation + double(flight.lowerFloor) * b.floorRise + flight.startRise;
                             const double under = base + flight.rise * std::clamp(t / span, 0., 1.);

                             // The steps are in the way when they pass through
                             // the light, or when they stand over it - a window
                             // with a stair soffit across it lets in nothing.
                             if (under > sill - .4 && under < head + 1.2)
                             {
                               return true;
                             }
                           }

                           return false;
                         }),
                         out.openings.end());
    }

    // The door of each tower storey moves to that storey's landing.
    //
    // The shaft already emitted a door onto whatever the building left beside
    // it, chosen by which wall shared the longest run - which is a reasonable
    // rule and has nothing to do with the stair. A door on a flight is a door
    // you step out of onto a step.
    if (newel)
    {
      for (DwellingDoor &door : out.doors)
      {
        if (door.floor >= towerStoreys || !hasLanding[door.floor] || !geom2::contains(towerPlan, at(door.center.x, door.center.z)))
        {
          continue;
        }

        // Slid along its own wall to the landing nearest that wall.
        //
        // Not averaged with the landing, which was the first attempt: that
        // moves the door half way and leaves it off the wall, so it is neither
        // where it was nor where it should be. The landing says which *end* of
        // which wall; the wall says the rest, and a door has to stay in one.
        //
        // And not `landings[floor]`, which was the second: that is the one
        // landing the storey's arrival was recorded at, and a newel stair turns
        // at every corner - so a door on the wall opposite that corner was slid
        // to the end of its own wall and still finished a shaft's width from
        // any landing at all. Every one of the four towers in the corpus did
        // this, and the check could not see it because it measured to a
        // landing's *centre* with a tolerance wide enough to cover the whole
        // shaft.
        //
        // The landings are all there in the flights; the right one is whichever
        // is nearest the wall the door is in.
        const double level = b.foundation + double(door.floor) * b.floorRise;

        Position to = landings[door.floor];
        double closest = 1e300;

        for (const rewrite::DwellingStair &flight : out.stairs)
        {
          for (const std::vector<Position> *corner : {&flight.lowerLanding, &flight.upperLanding})
          {
            if (corner->size() < 3u || std::abs(corner->front().y - level) > .3)
            {
              continue;
            }

            Position middle{0, level, 0};

            for (const Position &q : *corner)
            {
              middle.x += q.x / double(corner->size());
              middle.z += q.z / double(corner->size());
            }

            const double reach = std::hypot(middle.x - door.center.x, middle.z - door.center.z);

            if (reach < closest)
            {
              closest = reach;
              to = middle;
            }
          }
        }

        double best = 1e300;
        Position slid = door.center;

        for (size_t e = 0; e < towerPlan.outer.size(); ++e)
        {
          const geom2::Point &p = towerPlan.outer[e];
          const geom2::Point &q = towerPlan.outer[(e + 1u) % towerPlan.outer.size()];

          const double ax = geom2::toMetres(p.x), az = geom2::toMetres(p.y);
          const double bx = geom2::toMetres(q.x), bz = geom2::toMetres(q.y);
          const double run = std::hypot(bx - ax, bz - az);

          if (run < s.doorWidthMetres + .4)
          {
            continue;
          }

          const double dx = (bx - ax) / run, dz = (bz - az) / run;

          // Is the door in this wall?
          //
          // Preferred, not required. A door pinned to the wall it started on
          // can be slid to the end of that wall and still finish a shaft's
          // width from every landing, which is what the last one in the corpus
          // did - so a wall the landing actually touches wins over the one the
          // door happened to be given, and the cost of changing wall is added
          // rather than made absolute.
          const double off = std::abs((door.center.x - ax) * -dz + (door.center.z - az) * dx);
          const double elsewhere = off > s.wallThickness * 3 ? 1.2 : 0.;

          // ...then put it at the point of this wall nearest the landing.
          const double t = std::clamp((to.x - ax) * dx + (to.z - az) * dz, s.doorWidthMetres * .6, run - s.doorWidthMetres * .6);
          const Position at{ax + dx * t, door.center.y, az + dz * t};
          const double reach = std::hypot(at.x - to.x, at.z - to.z) + elsewhere;

          if (reach < best)
          {
            best = reach;
            slid = at;
          }
        }

        door.center = slid;
      }
    }
  }



  // ---- after the tower ------------------------------------------------------
  //
  // Everything from here needs the tower's own rooms, walls and windows to
  // exist, and they are built above - last of all, because the top of a shaft
  // stands over a building that has already stopped.
  //
  // Run before it, as these were, the wall frame laid its studs and rails
  // across a shaft that had no openings in it yet, so every window in every
  // tower came out with timber straight through it; and the open tower could
  // not take a storey off a shaft whose storeys had not been made.

  // ---- an open tower has no roof -------------------------------------------
  //
  // Shallowing the pitch was not the same thing as leaving it off: a tower with
  // a six-in-a-hundred cap still ends in a roof, and what was asked for is a
  // tower you stand on. So the faces are dropped outright, and the walk gets
  // boards instead - because a tower with neither a roof nor a floor is a hole.
  if (towerStoreys > 0u && s.towerOpenTop)
  {
    // The top storey of the shaft comes off, and the rail takes its place.
    //
    // Dropping the roof alone left a closed room at the top of the tower with
    // a fence standing on its lid - which is a walled chamber, not a lookout.
    // What makes a tower open is that the *walls* stop: you climb the shaft and
    // come out on the leads, with nothing round you but the rail.
    //
    // So the topmost storey plate goes, and with it the rooms and the walls at
    // that level, and the walk is the base of what was there. Only the storeys
    // that are tower and nothing else may be taken - the shaft passes through
    // the range below it, and taking a level the range is also on would open
    // the side of the building.
    if (out.storeys.size() > 1u)
    {
      const rewrite::DwellingStorey &top = out.storeys.back();

      geom2::Polygon plate;

      for (const Position &q : top.outer)
      {
        plate.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
      }

      std::vector<geom2::Polygon> beyond;
      const bool shaftOnly = plate.outer.size() >= 3u && geom2::polygonFault(plate).empty() && geom2::booleanOp({geom2::toOperand(plate), geom2::toOperand(towerPlan)}, geom2::Operation::Difference, {}, beyond, nullptr) && geom2::areaMetres(plate) - [&] {
        double left = 0;

        for (const geom2::Polygon &piece : beyond)
        {
          left += geom2::areaMetres(piece);
        }

        return left;
      }() > geom2::areaMetres(plate) * .5;

      if (shaftOnly)
      {
        const uint32_t gone = top.level;

        out.storeys.pop_back();

        out.rooms.erase(std::remove_if(out.rooms.begin(), out.rooms.end(), [&](const DwellingRoom &room) { return room.floor == gone; }), out.rooms.end());
        out.walls.erase(std::remove_if(out.walls.begin(), out.walls.end(), [&](const DwellingWall &wall) { return wall.floor == gone; }), out.walls.end());
        out.openings.erase(std::remove_if(out.openings.begin(), out.openings.end(), [&](const rewrite::DwellingOpening &hole) { return hole.floor == gone; }), out.openings.end());
        out.doors.erase(std::remove_if(out.doors.begin(), out.doors.end(), [&](const DwellingDoor &door) { return door.floor == gone; }), out.doors.end());

        // The last flight now arrives on the leads rather than in a room, which
        // is what a stair to a walk does.
        out.stairs.erase(std::remove_if(out.stairs.begin(), out.stairs.end(), [&](const DwellingStair &stair) { return stair.lowerFloor >= gone; }), out.stairs.end());

        for (rewrite::DwellingWing &w : out.wings)
        {
          if (w.tower && w.storeys > 1u)
          {
            w.storeys -= 1u;
            w.eavesHeight = b.foundation + double(w.storeys) * b.floorRise;
            w.ridgeHeight = w.eavesHeight;
          }
        }
      }
    }

    std::vector<rewrite::DwellingRoofFace> kept;

    for (const rewrite::DwellingRoofFace &face : out.roofs)
    {
      const bool onTower = face.wing < out.wings.size() && out.wings[face.wing].tower;

      if (!onTower)
      {
        kept.push_back(face);
      }
    }

    out.roofs = std::move(kept);

    // The boards of the walk, at the top of the shaft - which is the shaft's
    // own eaves and nothing else.
    //
    // Taking the greater of that and `towerStoreys * floorRise` was a hedge
    // against the massing having raised the tower, and it became wrong the
    // moment a storey was taken back off: `towerStoreys` still held the raised
    // count, so the walk was laid a storey above the walls it stands on and the
    // whole platform floated over the shaft with daylight under it.
    double walk = 0;

    for (const rewrite::DwellingWing &w : out.wings)
    {
      walk = w.tower ? std::max(walk, w.eavesHeight) : walk;
    }

    std::vector<std::array<uint32_t, 3>> triangles;
    std::vector<geom2::Point> vertices;

    if (!out.floors.empty() && geom3::triangulate(towerPlan, triangles, vertices, nullptr))
    {
      for (const auto &tri : triangles)
      {
        std::vector<Position> face;

        for (int k = 0; k < 3; ++k)
        {
          face.push_back(Position{geom2::toMetres(vertices[tri[k]].x), walk, geom2::toMetres(vertices[tri[k]].y)});
        }

        out.floors.back().slabs.push_back(std::move(face));
      }
    }
  }

  // ---- the rail round an open tower's walk ---------------------------------
  //
  // A tower with no spire has a floor at the top of it that somebody stands on,
  // and the thing that stops them falling off is the only part of it anyone
  // ever sees. Built as posts and rails rather than as a wall, because that is
  // what it is: a fence round a walk.
  if (towerStoreys > 0u && s.towerOpenTop && towerPlan.outer.size() >= 3u)
  {
    // The top of the shaft, taken from the shaft and from nothing else.
    //
    // Not from `towerStoreys`: the massing raises a tower until its eaves clear
    // the ridges around it, and an open one then gives a storey back - so that
    // count is wrong in both directions at different moments, and the wing's
    // own eaves is the only thing that is right at every one of them.
    double walk = 0;

    for (const rewrite::DwellingWing &w : out.wings)
    {
      walk = w.tower ? std::max(walk, w.eavesHeight) : walk;
    }

    const uint32_t level = uint32_t(out.storeys.empty() ? 0 : out.storeys.size() - 1);

    for (size_t i = 0; i < towerPlan.outer.size(); ++i)
    {
      const geom2::Point &p = towerPlan.outer[i];
      const geom2::Point &q = towerPlan.outer[(i + 1) % towerPlan.outer.size()];

      const double ax = geom2::toMetres(p.x), az = geom2::toMetres(p.y);
      const double bx = geom2::toMetres(q.x), bz = geom2::toMetres(q.y);
      const double length = std::hypot(bx - ax, bz - az);

      if (length < .4)
      {
        continue;
      }

      const double dx = (bx - ax) / length, dz = (bz - az) / length;

      // A rail in the plan as well as in the timber.
      //
      // The walk is floor with a fence round it, and the model has to be able
      // to say so: without it the boards of the walk are floor with nothing on
      // their edge, which is the same thing a gallery deck looked like before
      // its rail became a `WallKind::Rail` too. Doors and windows already know
      // to keep out of a rail.
      {
        DwellingWall fence;
        fence.id = wallId++;
        fence.floor = level;
        fence.a = Position{ax, walk, az};
        fence.b = Position{bx, walk, bz};
        fence.thickness = std::min(s.wallThickness, .1);
        fence.exterior = true;
        fence.kind = rewrite::WallKind::Rail;
        fence.height = s.parapetHeightMetres > .4 ? s.parapetHeightMetres : 1.0;

        out.walls.push_back(std::move(fence));
      }

      // No rails and no posts.
      //
      // A fence was two rails and a post every stride, each one a six faced
      // box, so the parapet round one tower walk came to a hundred and twenty
      // faces of joinery - and at this scale it reads as a smear rather than
      // as a fence. What a fence is, for now, is a plane on each side of a
      // line; the `Rail` wall above says where it runs and how high it is, and
      // whatever draws it makes those two planes. Balusters are a later pass's
      // business and it will want the line, not somebody else's sticks.
    }
  }

  // ---- a flat top ----------------------------------------------------------
  //
  // A pitched roof is what a building has when nothing stands on it. Where
  // something does - a neighbour's gallery, the head of a stair, the storey
  // nobody has the money for yet - the top is a slab with a rail round it.
  //
  // Three pieces: drop the roof, bring the ridge down to the eaves, and lay
  // boards on the top plate. A roof left in place under a slab is the fault
  // this reads as - a laje with a pitched roof inside it.
  //
  // No parapet. It was built out of the open-topped tower's walk, which comes
  // with the fence round it, and a street of them reads as a street of castle
  // keeps: the one thing a tower's top has that an ordinary flat roof does not
  // is a battlement, and putting one on every house in the block was the whole
  // of why they looked like towers. A flat top is a ceiling. Whatever guards
  // it - because somebody's stair lands on it, because it is somebody's
  // terrace - is the neighbourhood's business and is decided per piece of edge
  // where the neighbourhood decides every other guard.
  if (b.flatTop && !out.storeys.empty())
  {
    for (rewrite::DwellingWing &w : out.wings)
    {
      if (!w.tower)
      {
        w.ridgeHeight = w.eavesHeight;
      }
    }

    std::vector<rewrite::DwellingRoofFace> kept;

    for (const rewrite::DwellingRoofFace &face : out.roofs)
    {
      if (face.wing < out.wings.size() && out.wings[face.wing].tower)
      {
        kept.push_back(face);
      }
    }

    out.roofs = std::move(kept);

    // The slab, per storey plate rather than per wing.
    //
    // A wing's own outline carries the wall it shares with the wing beside it,
    // so boards laid per wing meet along that line twice; and a building whose
    // wings stop at different heights has more than one top. The plate is the
    // union at a level, and what a slab covers is the plate at the level above
    // whatever is under it - so the top of every wing that stops here, and
    // nothing that is still built on.
    for (size_t i = 0; i < out.storeys.size(); ++i)
    {
      const rewrite::DwellingStorey &storey = out.storeys[i];

      // Only the topmost plate: a lower one has a storey standing on it and
      // its ceiling is that storey's boards.
      bool covered = false;

      for (const rewrite::DwellingStorey &above : out.storeys)
      {
        covered = covered || above.level == storey.level + 1u;
      }

      if (covered || storey.outer.size() < 3)
      {
        continue;
      }

      const double top = storey.top;

      const auto ringOf = [](const std::vector<Position> &from) {
        geom2::Ring ring;

        for (const Position &q : from)
        {
          ring.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
        }

        if (ring.size() >= 3 && !geom2::counterClockwise(ring))
        {
          geom2::reverse(ring);
        }

        return ring;
      };

      geom2::Polygon plate;
      plate.outer = ringOf(storey.outer);

      for (const std::vector<Position> &hole : storey.holes)
      {
        geom2::Ring ring = ringOf(hole);

        if (ring.size() >= 3)
        {
          if (geom2::counterClockwise(ring))
          {
            geom2::reverse(ring);
          }

          plate.holes.push_back(std::move(ring));
        }
      }

      // The stacks the slab has to give way to.
      //
      // A chimney passes through the top of the building on its way out, and a
      // slab laid across it is a slab with a chimney standing in it: the two
      // surfaces intersect, the render fights over which of them owns each
      // pixel, and the stack reads as sunk into the roof rather than as
      // passing through it. The roof already gives way to a stack this way;
      // the boards had simply never been asked to.
      //
      // Cut rather than avoided, because where a stack goes is settled by the
      // hearth it stands on and the slab has no say in it.
      {
        std::vector<geom2::Operand> stacks;

        for (const rewrite::DwellingPillar &stack : out.chimneys)
        {
          if (stack.footprint.size() >= 3 && stack.top > top - .05)
          {
            stacks.push_back(geom2::toOperand(ringOf(stack.footprint)));
          }
        }

        std::vector<geom2::Polygon> cut;

        if (!stacks.empty() && geom2::differenceOf(geom2::toOperand(plate), stacks, cut, nullptr) && !cut.empty())
        {
          size_t widest = 0;

          for (size_t k = 1; k < cut.size(); ++k)
          {
            widest = std::abs(geom2::areaMetres(cut[k])) > std::abs(geom2::areaMetres(cut[widest])) ? k : widest;
          }

          if (cut[widest].outer.size() >= 3)
          {
            plate = cut[widest];
          }
        }
      }

      std::vector<std::array<uint32_t, 3>> triangles;
      std::vector<geom2::Point> vertices;

      if (!out.floors.empty() && geom3::triangulate(plate, triangles, vertices, nullptr))
      {
        for (const auto &tri : triangles)
        {
          std::vector<Position> face;

          for (int k = 0; k < 3; ++k)
          {
            face.push_back(Position{geom2::toMetres(vertices[tri[k]].x), top, geom2::toMetres(vertices[tri[k]].y)});
          }

          out.floors.back().slabs.push_back(std::move(face));
        }
      }

    }
  }

  // ---- a stack of dwellings, one to a floor -------------------------------
  //
  // Three or four households in one building, each with a floor to itself and
  // its own front door, and no stair inside connecting any of them. The way up
  // is outside: a gallery along one wall at every upper floor, and flights
  // zigzagging between them up that same wall.
  //
  // The shape is a Z. A flight climbs from the ground to the first gallery and
  // arrives near one end of it; you walk the length of that gallery; the next
  // flight leaves from the far end and climbs back the other way to the second
  // gallery. Every gallery is the landing for the flight above it, which is
  // why nothing here needs a landing of its own.
  //
  // Three measurements hold the whole arrangement together and none of them is
  // a preference:
  //
  //   - the flight is W wide and the gallery is 2W, so the flight takes the
  //     outer half and the half against the wall is left to walk along. A
  //     gallery as deep as the flight that lands on it is a gallery the flight
  //     *is*, and the door beyond it is behind a staircase.
  //   - the gallery is cut open above the flight that comes up through it.
  //     Not over the whole run - most of a flight is well below the deck it
  //     arrives at - but over the stretch where there is no longer headroom to
  //     walk under it. That stretch is a stairwell and the rest is floor.
  //   - the door is on the walking half, clear of both the hole and the foot of
  //     the next flight.
  if (tenement && !b.externalAccess && out.storeys.size() >= 2u)
  {
    // The wall it all hangs on: the one furthest from the front door, which is
    // the back. One wall for the whole stack, because a gallery that changes
    // face between floors leaves the flight above it starting nowhere.
    const rewrite::DwellingStorey *first = nullptr;

    for (const rewrite::DwellingStorey &storey : out.storeys)
    {
      first = storey.level == 1u ? &storey : first;
    }

    const double stairWide = std::max(std::min(s.stairWidthMetres, 1.2), .8);
    const double galleryDeep = stairWide * 2;

    size_t back = first == nullptr ? 0 : first->outer.size();
    double furthest = -1;

    if (first != nullptr)
    {
      for (size_t i = 0; i < first->outer.size(); ++i)
      {
        const Position &p = first->outer[i];
        const Position &q = first->outer[(i + 1) % first->outer.size()];

        // Long enough to carry a flight and still have gallery either side of
        // it. Shorter than that and this building cannot be let by the floor.
        if (std::hypot(q.x - p.x, q.z - p.z) < galleryDeep * 2)
        {
          continue;
        }

        const double reach = std::hypot((p.x + q.x) * .5 - b.entrance.x, (p.z + q.z) * .5 - b.entrance.z);

        if (reach > furthest)
        {
          furthest = reach;
          back = i;
        }
      }
    }

    if (first != nullptr && back < first->outer.size())
    {
      const Position &p = first->outer[back];
      const Position &q = first->outer[(back + 1) % first->outer.size()];
      const double run = std::hypot(q.x - p.x, q.z - p.z);
      const double dx = (q.x - p.x) / run, dz = (q.z - p.z) / run;

      // Counter clockwise, so out of the building is to the right of the way
      // an edge runs.
      const double nx = dz, nz = -dx;

      const auto on = [&](const double along, const double out, const double height) {
        return Position{p.x + dx * along + nx * out, height, p.z + dz * along + nz * out};
      };

      // The flight, its landing at the bottom and its landing at the top all
      // have to stand on the wall they run along.
      //
      // `planStairPath` puts a landing a stride before the first tread and
      // another a stride past the last, so a flight started at the corner
      // hangs a metre of floor off each end of the building - which is what
      // the render showed as stubs of decking floating past both gables. The
      // run is what is left of the wall once both landings have had their
      // stride.
      const double landing = std::max({s.stairLandingDepthMetres, stairWide, .6});
      const StairSteps step = stepsFor(b.floorRise, s.stairRiserMetres, 0);
      const double going = std::max(std::min(step.going, run - landing * 2), .8);

      // How much of the gallery has to be open over the flight below it.
      //
      // Where the climb still leaves headroom you can walk under the deck; past
      // that you cannot, and the deck has to be a hole instead. The hole runs
      // from there to the top of the flight, which is where you step out of it.
      const double headroom = std::max(s.doorHeadMetres, 2.);
      const double openFrom = b.floorRise > headroom ? going * (1. - headroom / b.floorRise) : 0.;
      const double wellLength = std::max(going - openFrom, stairWide);

      const uint32_t upper = std::min(b.households, uint32_t(out.storeys.size())) - 1u;

      // The flights first, all of them, and the galleries afterwards.
      //
      // A gallery's rail has to know where every flight meets it, and two of
      // them do: the one arriving through its stairwell and the one leaving
      // from its far end for the floor above. Built in one pass with the
      // galleries, the second of those does not exist yet when the rail is
      // decided - so the rail went across the foot of it, and the edges the
      // flights did not touch got no rail at all.
      std::vector<std::vector<Position>> runs;

      for (uint32_t floor = 1; floor <= upper; ++floor)
      {
        const double base = b.foundation + double(floor) * b.floorRise;
        const bool forward = (floor % 2u) == 1u;

        StairLeg leg;
        leg.from = on(forward ? landing : run - landing, stairWide, base - b.floorRise);
        leg.alongX = forward ? dx : -dx;
        leg.alongZ = forward ? dz : -dz;
        leg.acrossX = nx;
        leg.acrossZ = nz;

        std::vector<rewrite::DwellingStair> flights;
        std::vector<std::vector<Position>> turns;

        if (planStairPath({leg}, base - b.floorRise + (floor > 1u ? s.floorBoardMetres : 0.), base, stairWide, s.stairRiserMetres, s.floorBoardMetres, floor - 1u, flights, turns, s.stairLandingDepthMetres, going))
        {
          for (rewrite::DwellingStair &flight : flights)
          {
            flight.privateAccess = false;
            runs.push_back(flight.opening);
            out.stairs.push_back(std::move(flight));
          }
        }
      }

      // Is this piece of a gallery's edge one a flight meets?
      const auto atAFlight = [&](const double x, const double z, const double level) {
        for (const std::vector<Position> &opening : runs)
        {
          if (opening.size() < 4 || std::abs(opening.front().y - level) > b.floorRise * 1.1)
          {
            continue;
          }

          // Inside the run, or within a stride of it: the tread you step onto.
          bool in = false;

          for (size_t i = 0, j = opening.size() - 1; i < opening.size(); j = i++)
          {
            if ((opening[i].z > z) != (opening[j].z > z) && x < (opening[j].x - opening[i].x) * (z - opening[i].z) / (opening[j].z - opening[i].z) + opening[i].x)
            {
              in = !in;
            }
          }

          if (in)
          {
            return true;
          }

          for (size_t i = 0; i < opening.size(); ++i)
          {
            const Position &a = opening[i];
            const Position &c = opening[(i + 1) % opening.size()];
            const double ex = c.x - a.x, ez = c.z - a.z;
            const double length = ex * ex + ez * ez;
            const double at = length > 1e-12 ? std::max(0., std::min(1., ((x - a.x) * ex + (z - a.z) * ez) / length)) : 0.;

            if (std::hypot(a.x + ex * at - x, a.z + ez * at - z) < .35)
            {
              return true;
            }
          }
        }

        return false;
      };

      for (uint32_t floor = 1; floor <= upper; ++floor)
      {
        const double base = b.foundation + double(floor) * b.floorRise;
        const double board = base + s.floorBoardMetres;

        // Which way this storey's flight runs, and therefore which end of the
        // gallery it arrives at. Alternating is the Z: you arrive at one end,
        // walk the gallery, and leave from the other.
        const bool forward = (floor % 2u) == 1u;
        const double foot = forward ? landing : run - landing;      // The bottom tread.
        const double head = forward ? foot + going : foot - going;  // Where it tops out.
        const double wellFrom = forward ? head - wellLength : head;
        const double wellTo = wellFrom + wellLength;

        // ---- the gallery, with the stairwell cut out of it ------------------
        //
        // An L: the full band along the wall, less the outer half over the
        // stairwell. Wound so a fan from its first corner triangulates it,
        // which it does because the notch is at one end.
        std::vector<Position> deckRing;

        if (forward)
        {
          deckRing = {on(0, 0, board), on(run, 0, board), on(run, galleryDeep, board), on(wellTo, galleryDeep, board), on(wellTo, stairWide, board), on(wellFrom, stairWide, board), on(wellFrom, galleryDeep, board), on(0, galleryDeep, board)};
        }
        else
        {
          deckRing = {on(0, 0, board), on(run, 0, board), on(run, galleryDeep, board), on(wellTo, galleryDeep, board), on(wellTo, stairWide, board), on(wellFrom, stairWide, board), on(wellFrom, galleryDeep, board), on(0, galleryDeep, board)};
        }

        // The boards. Triangulated properly rather than fanned: an L fanned
        // from one of its own vertices covers ground that is not in it, and the
        // ground it covers here is the stairwell.
        if (uint32_t(out.floors.size()) > floor)
        {
          geom2::Polygon deckPlan;

          for (const Position &corner : deckRing)
          {
            deckPlan.outer.push_back({geom2::toUnit(corner.x), geom2::toUnit(corner.z)});
          }

          if (!geom2::counterClockwise(deckPlan.outer))
          {
            geom2::reverse(deckPlan.outer);
          }

          std::vector<std::array<uint32_t, 3>> triangles;
          std::vector<geom2::Point> vertices;

          if (geom3::triangulate(deckPlan, triangles, vertices, nullptr))
          {
            for (const auto &tri : triangles)
            {
              std::vector<Position> face;

              for (int k = 0; k < 3; ++k)
              {
                face.push_back(Position{geom2::toMetres(vertices[tri[k]].x), board, geom2::toMetres(vertices[tri[k]].y)});
              }

              out.floors[floor].slabs.push_back(std::move(face));
            }
          }
        }

        // ---- what holds it up -----------------------------------------------
        const int bays = std::max(1, int(std::round(run / std::max(s.balconyPostSpacingMetres, .6))));

        for (int k = 0; k <= bays; ++k)
        {
          const double along = run * double(k) / double(bays);
          const double half = s.balconyPostSideMetres * .5;
          const Position foot = on(along, galleryDeep - half, b.foundation);

          rewrite::DwellingPillar post;
          post.footprint = {Position{foot.x - half, b.foundation, foot.z - half}, Position{foot.x + half, b.foundation, foot.z - half}, Position{foot.x + half, b.foundation, foot.z + half}, Position{foot.x - half, b.foundation, foot.z + half}};
          post.base = b.foundation;
          post.top = board;

          out.pillars.push_back(std::move(post));
        }

        // ---- the rail --------------------------------------------------------
        //
        // Walked round the gallery's own outline rather than along the wall it
        // hangs off, because the outline is the thing with edges: the two short
        // returns at the ends, the outer edge, and the three sides of the
        // stairwell cut into it. Railing only the outer edge left a metre and a
        // half of open floor edge at every return and all the way round the
        // well, which is a hole in a deck with nothing round it.
        //
        // Off wherever a flight meets it - the well you come up through, the
        // foot of the flight you leave by - because a rail across either is a
        // rail across the way out.
        for (size_t edge = 1; edge < deckRing.size(); ++edge)
        {
          const Position &a = deckRing[edge];
          const Position &c = deckRing[(edge + 1) % deckRing.size()];
          const double length = std::hypot(c.x - a.x, c.z - a.z);

          if (length < .2)
          {
            continue;
          }

          const int pieces = std::max(1, int(std::ceil(length / .6)));
          double openedAt = -1;

          for (int k = 0; k <= pieces; ++k)
          {
            const double at = length * double(k) / double(pieces);
            const double mid = length * (double(k) + .5) / double(pieces);
            const bool guard = k < pieces && !atAFlight(a.x + (c.x - a.x) * (mid / length), a.z + (c.z - a.z) * (mid / length), base);

            if (guard && openedAt < 0)
            {
              openedAt = at;
            }

            if (!guard && openedAt >= 0)
            {
              const Position from = Position{a.x + (c.x - a.x) * (openedAt / length), board, a.z + (c.z - a.z) * (openedAt / length)};
              const Position to = Position{a.x + (c.x - a.x) * (at / length), board, a.z + (c.z - a.z) * (at / length)};

              DwellingWall balustrade;
              balustrade.id = wallId++;
              balustrade.floor = floor;
              balustrade.a = from;
              balustrade.b = to;
              balustrade.thickness = std::min(s.wallThickness, .1);
              balustrade.exterior = true;
              balustrade.kind = rewrite::WallKind::Rail;
              balustrade.height = std::max(s.balconyRailHeightMetres, .9);

              out.walls.push_back(std::move(balustrade));


              openedAt = -1;
            }
          }
        }

        // ---- the door onto it --------------------------------------------------
        //
        // On the walking half, and in the stretch of it that is neither the
        // hole you came up through nor the foot of the flight you leave by.
        // Both of those are places a door would open onto a staircase.
        {
          double doorAt = run * .5;
          double clearest = -1;

          for (int k = 0; k <= 20; ++k)
          {
            const double along = run * double(k) / 20.;

            if (along < s.doorWidthMetres || along > run - s.doorWidthMetres)
            {
              continue;
            }

            const double fromWell = std::min(std::abs(along - wellFrom), std::abs(along - wellTo));
            const double clear = (along > wellFrom && along < wellTo) ? 0. : fromWell;

            if (clear > clearest)
            {
              clearest = clear;
              doorAt = along;
            }
          }

          const Position middle = on(doorAt, 0, base);
          uint64_t onto = 0, room = 0;
          double nearest = 1e300;

          for (const DwellingWall &wall : out.walls)
          {
            if (wall.floor != floor || wall.kind != rewrite::WallKind::Solid || !wall.exterior)
            {
              continue;
            }

            const double length = std::hypot(wall.b.x - wall.a.x, wall.b.z - wall.a.z);

            if (length < s.doorWidthMetres + .5)
            {
              continue;
            }

            const double reach = std::hypot((wall.a.x + wall.b.x) * .5 - middle.x, (wall.a.z + wall.b.z) * .5 - middle.z);

            if (reach < nearest)
            {
              nearest = reach;
              onto = wall.id;
            }
          }

          for (const DwellingRoom &r : out.rooms)
          {
            if (r.floor == floor && r.boundary.size() >= 3u && geom2::contains(footprint, at(middle.x, middle.z)))
            {
              room = room == 0 ? r.id : room;
            }
          }

          if (onto != 0 && nearest < galleryDeep + 2.)
          {
            for (const DwellingWall &wall : out.walls)
            {
              if (wall.id != onto)
              {
                continue;
              }

              const double length = std::hypot(wall.b.x - wall.a.x, wall.b.z - wall.a.z);
              const double ux = (wall.b.x - wall.a.x) / length, uz = (wall.b.z - wall.a.z) / length;
              const double t = std::clamp((middle.x - wall.a.x) * ux + (middle.z - wall.a.z) * uz, s.doorWidthMetres, length - s.doorWidthMetres);
              const double width = s.doorWidthMetres + s.openingMarginMetres * 2;

              DwellingDoor onward;
              onward.wall = onto;
              onward.room = room;
              onward.floor = floor;
              onward.center = Position{wall.a.x + ux * t, base, wall.a.z + uz * t};
              onward.width = s.doorWidthMetres;
              onward.exterior = true;

              out.doors.push_back(std::move(onward));

              rewrite::DwellingOpening hole;
              hole.wall = onto;
              hole.room = room;
              hole.floor = floor;
              hole.a = Position{wall.a.x + ux * (t - width * .5), base, wall.a.z + uz * (t - width * .5)};
              hole.b = Position{wall.a.x + ux * (t + width * .5), base, wall.a.z + uz * (t + width * .5)};
              hole.base = 0;
              hole.top = s.doorHeadMetres + s.openingMarginMetres;
              hole.sill = 0;
              hole.head = s.doorHeadMetres;
              hole.thickness = wall.thickness;
              hole.kind = rewrite::OpeningKind::Door;
              hole.exterior = true;

              out.openings.push_back(std::move(hole));

              break;
            }
          }
        }
      }
    }
  }

  // ---- the balcony ----------------------------------------------------------
  //
  // A floor standing outside the walls on posts, with a rail round it and a
  // door onto it. The mirror of the entrance bay, and built the same way out of
  // the same pieces - which is why neither of them is in a storey plate: a
  // plate is where the walls are, and what makes a balcony a balcony is that
  // there are none.
  //
  // On the back, because that is where one goes. The front of a house of this
  // age is its frontage and is worth money; the gallery, the stair to it and
  // the privy are all on the yard side, and a balcony over a street is a thing
  // a later century builds.
  // A tenement always has one, whatever the settings say: the gallery is the
  // only way into the upper dwelling, and a building let by the floor with no
  // gallery is a building with one dwelling in it and one sealed off.
  //
  // ...unless somebody else is building it. `externalAccess` says the gallery
  // outside this wall is shared with the building next door and belongs to the
  // layer that knows there is one; forcing a second against the same wall is
  // not a second way in.
  // The single balcony, for a building that is one dwelling.
  //
  // A stack of them has already been built above, galleries and flights
  // together, and the two must not both run: the stack's first gallery and
  // this one stand against the same wall at the same height.
  const bool stacked = tenement && !b.externalAccess && out.storeys.size() >= 2u;
  const double balconyDepth = s.balconyDepthMetres;

  if (!stacked && balconyDepth > .5 && out.storeys.size() >= 2u)
  {
    const rewrite::DwellingStorey *level = nullptr;

    for (const rewrite::DwellingStorey &storey : out.storeys)
    {
      level = storey.level == 1u ? &storey : level;
    }

    if (level != nullptr && level->outer.size() >= 3u)
    {
      // The wall furthest from the door: the back.
      size_t back = level->outer.size();
      double furthest = -1;

      for (size_t i = 0; i < level->outer.size(); ++i)
      {
        const Position &p = level->outer[i];
        const Position &q = level->outer[(i + 1) % level->outer.size()];
        const double run = std::hypot(q.x - p.x, q.z - p.z);

        if (run < balconyDepth * 2)
        {
          continue;
        }

        const double reach = std::hypot((p.x + q.x) * .5 - b.entrance.x, (p.z + q.z) * .5 - b.entrance.z);

        if (reach > furthest)
        {
          furthest = reach;
          back = i;
        }
      }

      if (back < level->outer.size())
      {
        const Position &p = level->outer[back];
        const Position &q = level->outer[(back + 1) % level->outer.size()];
        const double run = std::hypot(q.x - p.x, q.z - p.z);
        const double dx = (q.x - p.x) / run, dz = (q.z - p.z) / run;

        // Counter clockwise, so out of the building is to the right of the way
        // an edge runs.
        const double nx = dz, nz = -dx;

        // Where the gallery starts, and how much of the wall it takes.
        //
        // A tenement leaves room at the near end for the flight that climbs to
        // it. Centred on the wall like an ordinary balcony it takes four fifths
        // of it, and the stair then has nowhere along that wall to be - so it
        // was put alongside the gallery, which means underneath it.
        // Each flight of the turn climbs half the storey, so each is stepped
        // for half - which is what makes the two halves the same stair rather
        // than one flight and a ramp.
        const StairSteps stairStep = stepsFor(b.floorRise * .5, s.towerRiserMetres, 0);
        const uint32_t stairRisers = stairStep.risers * 2u;
        const double stairGoing = stairStep.going;
        // As wide as the gallery is deep.
        //
        // Narrower, the flight leaves a strip of nothing between itself and the
        // edge of the landing it arrives at - you can see the ground through the
        // gap, and the stair reads as a plank laid against a platform rather
        // than as part of it. The gallery's depth is the width the whole
        // arrangement is built to; the stair takes all of it.
        const double stairWidth = balconyDepth;

        const double from = tenement ? std::min(stairGoing, run * .45) : (run - std::max(1.6, run * s.balconyShare)) * .5;
        const double span = tenement ? std::max(1.6, run - from - .2) : std::max(1.6, run * s.balconyShare);
        const double depth = balconyDepth;
        const double deck = level->base;

        // Named for what it is, because `at` already means something here: the
        // file's own helper turns metres into a quantised point, and a second `at`
        // taking three arguments hides it inside this block.
        const auto deckAt = [&](const double along, const double out, const double height) {
          return Position{p.x + dx * along + nx * out, height, p.z + dz * along + nz * out};
        };

        // The boards, at the storey's own level so you step straight out onto
        // them.
        const double board = deck + s.floorBoardMetres;

        if (uint32_t(out.floors.size()) > 1u)
        {
          out.floors[1].slabs.push_back({deckAt(from, 0, board), deckAt(from + span, 0, board), deckAt(from + span, depth, board)});
          out.floors[1].slabs.push_back({deckAt(from, 0, board), deckAt(from + span, depth, board), deckAt(from, depth, board)});
        }

        // The posts that carry it, down to the ground, and the rail round it.
        const int bays = std::max(1, int(std::round(span / std::max(s.balconyPostSpacingMetres, .6))));

        for (int k = 0; k <= bays; ++k)
        {
          const double t = from + span * double(k) / double(bays);
          const Position foot = deckAt(t, depth - s.balconyPostSideMetres * .5, b.foundation);
          const double half = s.balconyPostSideMetres * .5;

          rewrite::DwellingPillar post;
          post.base = b.foundation;
          post.top = deck;
          post.footprint = {
            Position{foot.x - half, post.base, foot.z - half},
            Position{foot.x + half, post.base, foot.z - half},
            Position{foot.x + half, post.base, foot.z + half},
            Position{foot.x - half, post.base, foot.z + half}};

          out.pillars.push_back(std::move(post));

          // The rail's own post is gone with the rails: a fence is a plane on
          // each side of a line, and the `Rail` wall carries the line.
        }

        // The rails: along the front of the deck and round both returns.
        for (const double height : {1.0, .5})
        {
          const double y = board + s.balconyRailHeightMetres * height;

          // A balustrade, and a *wall* in the plan.
          //
          // It used to be two loose timbers with no place in the plan at all,
          // so nothing reading a plan could tell that the edge of a gallery
          // was enclosed - by the model, a balcony was floor with nothing round
          // it. It is the same boundary the edge of a hall void has, so it is
          // the same thing: a `WallKind::Rail`, which doors and windows already
          // know to keep out of, emitted once rather than once per height.
          const auto rail = [&](const Position &a, const Position &c) {

            if (height < 1.)
            {
              return;
            }

            DwellingWall balustrade;
            balustrade.id = wallId++;
            balustrade.floor = 1u;
            balustrade.a = Position{a.x, board, a.z};
            balustrade.b = Position{c.x, board, c.z};
            balustrade.thickness = std::min(s.wallThickness, .1);
            balustrade.exterior = false;
            balustrade.kind = rewrite::WallKind::Rail;
            balustrade.height = s.balconyRailHeightMetres > .4 ? s.balconyRailHeightMetres : 1.0;

            out.walls.push_back(std::move(balustrade));
          };

          rail(deckAt(from, depth - .06, y), deckAt(from + span, depth - .06, y));

          // The return the stair arrives at is left open: a gallery railed the
          // whole way round is a gallery you cannot get onto, and the flight
          // lands on exactly that end of it.
          // Open at the end the stair arrives at, which is the near one.
          //
          // It was opened at the far end - left over from an arrangement where
          // the flight climbed past the gallery and landed beyond it. The stair
          // turns at the corner now and arrives at `from`, so `from` is the end
          // that has to be open, and railing it while opening the other end
          // gives a gallery you can neither get onto nor fall off the wrong
          // side of.
          if (!tenement)
          {
            rail(deckAt(from, 0, y), deckAt(from, depth - .06, y));
          }

          rail(deckAt(from + span, 0, y), deckAt(from + span, depth - .06, y));
        }

        // The door onto it, cut through the wall behind.
        //
        // Found by the wall rather than made up: the balcony stands against a
        // storey outline and the rooms behind it were cut a wall's thickness
        // inside that, so the door belongs to whichever of their walls lies on
        // this line.
        {
          const Position middle = deckAt(from + span * .5, 0, deck);
          uint64_t onto = 0;
          uint64_t room = 0;
          double nearest = 1e300;

          for (const DwellingWall &wall : out.walls)
          {
            if (wall.floor != 1u || wall.kind != rewrite::WallKind::Solid || !wall.exterior)
            {
              continue;
            }

            const double length = std::hypot(wall.b.x - wall.a.x, wall.b.z - wall.a.z);

            if (length < s.doorWidthMetres + .5)
            {
              continue;
            }

            const double reach = std::hypot((wall.a.x + wall.b.x) * .5 - middle.x, (wall.a.z + wall.b.z) * .5 - middle.z);

            if (reach < nearest)
            {
              nearest = reach;
              onto = wall.id;
            }
          }

          for (const DwellingRoom &r : out.rooms)
          {
            if (r.floor == 1u && r.boundary.size() >= 3u && geom2::contains(footprint, at(middle.x, middle.z)))
            {
              room = room == 0 ? r.id : room;
            }
          }

          if (onto != 0 && nearest < depth + 1.5)
          {
            for (const DwellingWall &wall : out.walls)
            {
              if (wall.id != onto)
              {
                continue;
              }

              const double length = std::hypot(wall.b.x - wall.a.x, wall.b.z - wall.a.z);
              const double ux = (wall.b.x - wall.a.x) / length, uz = (wall.b.z - wall.a.z) / length;
              const double t = std::clamp((middle.x - wall.a.x) * ux + (middle.z - wall.a.z) * uz, s.doorWidthMetres, length - s.doorWidthMetres);
              const double width = s.doorWidthMetres + s.openingMarginMetres * 2;

              DwellingDoor onward;
              onward.wall = onto;
              onward.room = room;
              onward.floor = 1u;
              onward.center = Position{wall.a.x + ux * t, deck, wall.a.z + uz * t};
              onward.width = s.doorWidthMetres;
              onward.exterior = true;

              out.doors.push_back(std::move(onward));

              rewrite::DwellingOpening hole;
              hole.wall = onto;
              hole.room = room;
              hole.floor = 1u;
              hole.a = Position{wall.a.x + ux * (t - width * .5), deck, wall.a.z + uz * (t - width * .5)};
              hole.b = Position{wall.a.x + ux * (t + width * .5), deck, wall.a.z + uz * (t + width * .5)};
              hole.base = 0;
              hole.top = s.doorHeadMetres + s.openingMarginMetres;
              hole.sill = 0;
              hole.head = s.doorHeadMetres;
              hole.thickness = wall.thickness;
              hole.kind = rewrite::OpeningKind::Door;
              hole.exterior = true;

              out.openings.push_back(std::move(hole));

              break;
            }
          }
        }

        // The stair up to it: two flights round the corner of the building.
        //
        // One flight was tried twice and is wrong both ways. Run up to the
        // gallery's near end it has to begin a flight's length *before* the
        // gallery, which is several metres past the corner - the stair leaves
        // the building and stands on ground the plot may not own. Run alongside
        // the gallery instead it fits, and it fits *underneath* it: the deck is
        // over your head the whole way up and you arrive at a soffit.
        //
        // So it turns. A flight along the side wall, a landing at the corner,
        // and a second flight along the back wall arriving at the end of the
        // gallery - which is how a building with a gallery is actually reached,
        // and the only arrangement that is both on the plot and not under the
        // deck.
        if (tenement)
        {
          const double half = (deck - b.foundation) * .5;
          const uint32_t halfRisers = stairStep.risers;
          const double going = stairGoing;
          const double width = stairWidth;

          // Two legs, and nothing else said.
          //
          // The turn used to be written out here: two flights, a landing, four
          // rectangles each in its own coordinates, and every correction this
          // area needed was about one of the joints between them. The planner
          // owns the joints. What is left to say is where the legs run, and
          // that is a statement about this building rather than about stairs.
          //
          // Leg one climbs *towards* the back wall alongside the side wall -
          // the side wall is the line `along = 0` in this frame, so a leg at a
          // negative `along` running in `out` hugs it. Leg two turns at the
          // corner and climbs along the back wall to where the gallery begins.
          const double outX = nx, outZ = nz;

          std::vector<StairLeg> legs;
          legs.push_back(StairLeg{deckAt(-width, -going, b.foundation), outX, outZ, dx, dz});
          legs.push_back(StairLeg{deckAt(0, 0, b.foundation), dx, dz, outX, outZ});

          std::vector<rewrite::DwellingStair> flights;
          std::vector<std::vector<Position>> turns;

          if (planStairPath(legs, b.foundation, deck, width, s.towerRiserMetres, s.floorBoardMetres, 0u, flights, turns, s.stairLandingDepthMetres, 0))
          {
            for (rewrite::DwellingStair &flight : flights)
            {
              out.stairs.push_back(std::move(flight));
            }

            if (!out.floors.empty())
            {
              for (std::vector<Position> &turn : turns)
              {
                out.floors[0].slabs.push_back(std::move(turn));
              }
            }
          }
        }

        // Nothing is cut in the wall the stair climbs against.
        //
        // A window behind a flight of steps looks at the underside of them, and
        // a door behind one opens onto a stringer. The stair is outside and the
        // wall does not know it is there, so the wall has to be told.
        if (tenement)
        {
          const StairSteps steps = stepsFor(b.floorRise, s.towerRiserMetres, 0);
          const uint32_t risers = steps.risers;
          const double going = steps.going;
          const double head = std::min(from + span, run - .2);
          const double foot = std::max(head - going, .2);

          out.openings.erase(std::remove_if(out.openings.begin(), out.openings.end(), [&](const rewrite::DwellingOpening &hole) {
                               if (hole.floor != 0u || !hole.exterior)
                               {
                                 return false;
                               }

                               const double mx = (hole.a.x + hole.b.x) * .5, mz = (hole.a.z + hole.b.z) * .5;
                               const double t = (mx - p.x) * dx + (mz - p.z) * dz;
                               const double off = std::abs((mx - p.x) * -dz + (mz - p.z) * dx);

                               return off < .6 && t > foot - .4 && t < head + .4;
                             }),
                             out.openings.end());
        }

        // ...and a roof over it, where it is meant to have one. A lean-to: one
        // surface falling from the wall out to the rail.
        if (s.balconyRoofed)
        {
          const double eaves = deck + b.floorRise;

          rewrite::DwellingRoofFace shelter;
          shelter.wing = 0;
          shelter.polygon = {deckAt(from, 0, eaves), deckAt(from + span, 0, eaves), deckAt(from + span, depth + .2, eaves - depth * .34), deckAt(from, depth + .2, eaves - depth * .34)};

          out.roofs.push_back(std::move(shelter));

          for (const int k : {0, bays})
          {
            const double t = from + span * double(k) / double(bays);

            rewrite::DwellingTimber prop;
            prop.floor = 1u;
            prop.kind = rewrite::TimberKind::Post;
            prop.width = s.balconyPostSideMetres;
            prop.depth = s.balconyPostSideMetres;
            prop.a = deckAt(t, depth - .06, board);
            prop.b = deckAt(t, depth - .06, eaves - depth * .34);

            out.timbers.push_back(std::move(prop));
          }
        }
      }
    }
  }

  // ---- no frame on the outside wall -----------------------------------------
  //
  // The studs, the corner posts and the rails were taken out again.
  //
  // They are what a timber building actually has, and at this scale they are
  // not what it reads as: a stud every bay puts a line on the wall every couple
  // of feet, the corner posts put two more at every corner, and a facade ends
  // up as a field of hairlines with the windows lost in it. The wall is a
  // surface with holes in it, and that is what it should be until something
  // downstream can draw a frame at a size where a frame is legible.
  //
  // The interior frame stays. It is seen from inside a room, where there is one
  // of it rather than forty, and it is the thing that says a floor is carried
  // rather than floating.

  // ---- one upright per place ----------------------------------------------
  //
  // Three things put a post in a corner and each is right on its own: the bay
  // emits the pillars that carry it, the wall junctions emit a post where the
  // plan says the load goes, and the wall frame emits a corner post at each
  // turn of the storey outline. At a corner of the building all three describe
  // the same piece of timber, standing within a wall's thickness of each other,
  // and the renders showed two and three posts jammed together.
  //
  // Making the three generators agree about it was tried and is the wrong
  // shape of fix: each has a different idea of where the corner is - the room's
  // ring, the outline, the bisector between them - and a tolerance that
  // satisfies one lets another through. They are reconciled once, here, after
  // all three have run and on the only question that matters: is there already
  // an upright standing in this place?
  //
  // The corner post wins over the junction post where they clash. It is the one
  // on the outline, so it is the one you can see; the junction post is half a
  // wall inside it and would be buried.
  {
    std::vector<rewrite::DwellingTimber> kept;

    const auto standing = [](const rewrite::TimberKind kind) {
      return kind == rewrite::TimberKind::Post || kind == rewrite::TimberKind::Stud;
    };

    // Studs first, so a corner post is the one already standing when the
    // junction post asks.
    std::vector<size_t> order(out.timbers.size());

    for (size_t i = 0; i < order.size(); ++i)
    {
      order[i] = i;
    }

    std::stable_sort(order.begin(), order.end(), [&](const size_t l, const size_t r) {
      return int(out.timbers[l].kind == rewrite::TimberKind::Stud) > int(out.timbers[r].kind == rewrite::TimberKind::Stud);
    });

    std::vector<Position> placed;

    for (const rewrite::DwellingPillar &pillar : out.pillars)
    {
      double cx = 0, cz = 0;

      for (const Position &q : pillar.footprint)
      {
        cx += q.x / double(pillar.footprint.size());
        cz += q.z / double(pillar.footprint.size());
      }

      placed.push_back(Position{cx, 0, cz});
    }

    const auto occupied = [&](const uint32_t floor, const double x, const double z, const double within) {
      for (const Position &had : placed)
      {
        // A pillar is on the ground floor and has no storey of its own, which
        // is what the zero in its entry means.
        const bool sameFloor = had.y == 0. ? floor == 0u : std::abs(had.y - double(floor)) < .5;

        if (sameFloor && std::hypot(had.x - x, had.z - z) < within)
        {
          return true;
        }
      }

      return false;
    };

    // The uprights first, so the braces can be asked whether the post they
    // spring from survived.
    for (const size_t i : order)
    {
      const rewrite::DwellingTimber &timber = out.timbers[i];

      if (!standing(timber.kind) || occupied(timber.floor, timber.a.x, timber.a.z, .32))
      {
        continue;
      }

      placed.push_back(Position{timber.a.x, double(timber.floor), timber.a.z});
      kept.push_back(timber);
    }

    // ...and then everything else, less the braces whose post has gone.
    //
    // A brace springs from a post. Dropping the post and keeping its braces
    // leaves two diagonals meeting in the air at the corner, which looks more
    // wrong than the doubled post did - and they were emitted in the same
    // breath as a post that is no longer there.
    for (const rewrite::DwellingTimber &timber : out.timbers)
    {
      if (standing(timber.kind))
      {
        continue;
      }

      // Braces in the roof are not subject to this: it is a rule about the
      // braces off a wall post, and a brace in a roof springs from partway up
      // a principal rafter, which stands over no wall at all by design. Culled
      // with the rest, a collar braced roof lost every brace it had and came
      // out identical to a plain one.
      double wallHead = 1e300;

      for (const rewrite::DwellingWing &w : out.wings)
      {
        wallHead = std::min(wallHead, w.eavesHeight);
      }

      const bool inRoof = wallHead < 1e299 && timber.a.y > wallHead - .4;

      if (timber.kind == rewrite::TimberKind::Brace && !inRoof && !occupied(timber.floor, timber.a.x, timber.a.z, .45))
      {
        continue;
      }

      kept.push_back(timber);
    }

    out.timbers = std::move(kept);
  }

  // ---- the rail round the hall void -----------------------------------------
  //
  // A hall open to its roof leaves a hole in every storey above it, and the
  // floor that runs round that hole is a gallery: you stand on it and look down
  // into the hall. The edge between the two is a boundary in the plan - doors
  // and windows must not be cut in it, and the storey's rooms end there - but
  // it is not a wall in the building. It is a balustrade about a metre high
  // with posts in it, which is what `WallKind::Rail` says.
  //
  // Built after the storeys rather than with them, because the void is only
  // known once `hallFloors` has been settled against the storeys that were
  // actually built, and because the rail belongs to the *edge of the hole*
  // rather than to any one room beside it.
  if (hallFloors > 1u && hallPlan.outer.size() >= 3u)
  {
    for (uint32_t floor = 1; floor < hallFloors && floor < uint32_t(out.floors.size()); ++floor)
    {
      const double y = b.foundation + double(floor) * b.floorRise;
      size_t railed = 0;

      for (size_t i = 0; i < hallPlan.outer.size(); ++i)
      {
        const geom2::Point &p = hallPlan.outer[i];
        const geom2::Point &q = hallPlan.outer[(i + 1u) % hallPlan.outer.size()];

        const double ax = geom2::toMetres(p.x), az = geom2::toMetres(p.y);
        const double cx = geom2::toMetres(q.x), cz = geom2::toMetres(q.y);

        if (std::hypot(cx - ax, cz - az) < .4)
        {
          continue;
        }

        // Only where there is floor to stand on beside it. Where the void runs
        // up to the outside wall there is no gallery on that side, and a rail
        // there would be a fence bolted to the inside of a wall.
        const double mx = (ax + cx) * .5, mz = (az + cz) * .5;
        const double run = std::hypot(cx - ax, cz - az);
        const double outX = -(cz - az) / run, outZ = (cx - ax) / run;

        bool floorBeside = false;

        for (const std::vector<Position> &slab : out.floors[floor].slabs)
        {
          geom2::Polygon ring;

          for (const Position &corner : slab)
          {
            ring.outer.push_back({geom2::toUnit(corner.x), geom2::toUnit(corner.z)});
          }

          if (geom2::areaMetres(ring) < 0)
          {
            std::reverse(ring.outer.begin(), ring.outer.end());
          }

          // Both sides of the edge, because which one is "out" depends on how
          // the hall's own ring happens to be wound - and when it is wound the
          // other way this probes *into* the void, finds no floor there, and
          // decides there is no gallery anywhere. Every rail round every hall
          // in the corpus went missing that way while the hole itself was
          // perfectly correct.
          //
          // Testing both is not a fudge: the question is whether there is
          // floor beside this edge, and the side it is on does not change the
          // answer.
          for (const double side : {.5, -.5})
          {
            floorBeside = floorBeside || (ring.outer.size() >= 3u && geom2::contains(ring, geom2::Point{geom2::toUnit(mx + outX * side), geom2::toUnit(mz + outZ * side)}));
          }
        }

        if (!floorBeside)
        {
          continue;
        }

        DwellingWall rail;
        rail.id = wallId++;
        rail.floor = floor;
        rail.a = Position{ax, y, az};
        rail.b = Position{cx, y, cz};
        rail.thickness = std::min(s.wallThickness, .1);
        rail.exterior = false;
        rail.kind = rewrite::WallKind::Rail;
        rail.height = s.balconyRailHeightMetres > .4 ? s.balconyRailHeightMetres : 1.0;

        out.walls.push_back(std::move(rail));
        ++railed;

        // The posts in it. A balustrade is a rail carried on uprights, and
        // without them it is a parapet - a metre of blank wall standing on
        // nothing, which is what a gallery edge must not look like from the
        // hall below.
        const int posts = std::max(2, int(run / std::max(s.balconyPostSpacingMetres, .8)));

        for (int n = 0; n <= posts; ++n)
        {
          const double t = run * double(n) / double(posts);
          const double px = ax + (cx - ax) * t / run, pz = az + (cz - az) * t / run;
          const double side = std::max(s.balconyPostSideMetres, .08) * .5;

          // One baluster to a place: the rail runs round the void edge by edge
          // and each edge puts one at both of its ends, so every corner of the
          // void gets the same baluster twice.
          bool already = false;

          for (const rewrite::DwellingPillar &had : out.pillars)
          {
            if (had.footprint.size() < 3u || std::abs(had.base - y) > .05)
            {
              continue;
            }

            double hx = 0, hz = 0;

            for (const Position &q : had.footprint)
            {
              hx += q.x / double(had.footprint.size());
              hz += q.z / double(had.footprint.size());
            }

            // Half the spacing they are laid out at: two balusters closer
            // together than that are the same baluster, whichever edge of the
            // void put them there.
            already = already || std::hypot(hx - px, hz - pz) < std::max(s.balconyPostSpacingMetres * .5, .35);
          }

          if (already)
          {
            continue;
          }

          rewrite::DwellingPillar upright;
          upright.base = y;
          upright.top = y + (s.balconyRailHeightMetres > .4 ? s.balconyRailHeightMetres : 1.0);
          upright.footprint = {
            Position{px - side, upright.base, pz - side},
            Position{px + side, upright.base, pz - side},
            Position{px + side, upright.base, pz + side},
            Position{px - side, upright.base, pz + side}};

          out.pillars.push_back(std::move(upright));
        }
      }

      // The rooms that run round it are a gallery, and say so. A room named for
      // what it is is the difference between a plan and a picture of one.
      if (railed > 0u)
      {
        for (rewrite::DwellingRoom &room : out.rooms)
        {
          if (room.floor != floor || room.boundary.size() < 3u)
          {
            continue;
          }

          double rx = 0, rz = 0;

          for (const Position &corner : room.boundary)
          {
            rx += corner.x / double(room.boundary.size());
            rz += corner.z / double(room.boundary.size());
          }

          // Beside the void rather than anywhere on the storey: within a
          // stride of its edge.
          if (distanceToRing(hallPlan.outer, rx, rz) < 2.5 && room.use != rewrite::RoomUse::Tower && room.use != rewrite::RoomUse::Entrance)
          {
            room.kind = rewrite::RoomKind::Gallery;
            room.use = rewrite::RoomUse::Common;
          }
        }
      }
    }
  }

  // ---- no wall stands across a stair ----------------------------------------
  //
  // A flight is laid out in the room it starts in, on the storey it starts on,
  // and it arrives on the storey above - whose rooms were cut separately and
  // know nothing about it. So the upper landing regularly came down in the
  // middle of a partition: you climb the stair and walk into a wall. It is not
  // a stair fault and no measurement of the stair can see it, because the
  // stair is correct and the wall is somewhere else.
  //
  // The wall gives way, not the stair. A partition crossing a landing is a
  // partition drawn through a piece of circulation - the arrival on a storey,
  // which is the one place in a plan that has to be open - and the room it was
  // dividing is better off one room than divided by a wall you cannot walk
  // past. Doors and openings in it go with it.
  //
  // Landings and flights are recorded as circulation while we are here, which
  // is what they are: the stair, its two ends and the corridor they open onto
  // are one route through the building.
  {
  // Does a wall running in direction (dx, dz) reach *across* this route at the
  // point (x, z), rather than lying along the edge of it?
  //
  // The distinction the whole pass turns on, and two attempts got it wrong.
  // Shrinking the route hides a wall a hand's breadth inside its far edge -
  // which is a wall a hand's breadth in front of the top tread. Measuring how
  // deep the wall gets fails the same way for the same reason.
  //
  // What actually separates the two cases is *direction*. A landing stands
  // against walls by design and those walls run along its edges; a wall that
  // is in the way crosses them. So: inside the route, and not parallel to the
  // edge it is nearest.
  const auto crossesRoute = [](const geom2::Polygon &route, const double x, const double z, const double dx, const double dz) {
    if (!geom2::contains(route, geom2::Point{geom2::toUnit(x), geom2::toUnit(z)}))
    {
      return false;
    }

    double nearest = 1e300, alongX = 1, alongZ = 0;

    for (size_t i = 0; i < route.outer.size(); ++i)
    {
      const double ax = geom2::toMetres(route.outer[i].x), az = geom2::toMetres(route.outer[i].y);
      const double bx = geom2::toMetres(route.outer[(i + 1) % route.outer.size()].x);
      const double bz = geom2::toMetres(route.outer[(i + 1) % route.outer.size()].y);
      const double ex = bx - ax, ez = bz - az;
      const double run = ex * ex + ez * ez;

      if (run < 1e-9)
      {
        continue;
      }

      const double t = std::clamp(((x - ax) * ex + (z - az) * ez) / run, 0., 1.);
      const double away = std::hypot(ax + ex * t - x, az + ez * t - z);

      if (away < nearest)
      {
        nearest = away;
        alongX = ex / std::sqrt(run);
        alongZ = ez / std::sqrt(run);
      }
    }

    // Strictly inside, by more than the width of a pencil line.
    //
    // The exemption is for a wall that *is* the landing's edge - a landing
    // stands against walls by design, and `geom2::contains` counts a boundary
    // point as inside, so without some tolerance every such wall reads as a
    // wall across the landing and this pass knocks down the room the stair is
    // in. Fifty millimetres is enough for that and nothing more.
    //
    // It used to be 180 mm of inset, and then a depth test, and both let
    // through the case that matters: a partition 70 mm inside the far edge of
    // a landing is a partition 70 mm in front of the top tread. It is parallel
    // to that edge, so a direction test excuses it too. Nothing about it is
    // excusable - you walk into it.
    (void)dx;
    (void)dz;
    (void)alongX;
    (void)alongZ;

    return nearest > .05;
  };

    std::vector<std::pair<uint32_t, geom2::Polygon>> routes;

    for (const DwellingStair &flight : out.stairs)
    {
      // Four routes, not three: the flight, its two landings, and the flight
      // again *on the floor above*.
      //
      // A flight rises through the floor it arrives at, so the ground it
      // occupies belongs to two storeys - and only the lower one was being
      // registered. The upper storey was cut without knowing the stair was
      // there, so its partitions ran straight through the stairwell: you climb
      // the flight and there is a wall standing in it. Eleven flights in the
      // corpus, and invisible to every measurement of the stair itself, which
      // was correct in all of them.
      for (int end = 0; end < 4; ++end)
      {
        const std::vector<Position> &piece = end == 1 ? flight.lowerLanding : end == 2 ? flight.upperLanding : flight.opening;
        const uint32_t floor = end == 2 || end == 3 ? flight.lowerFloor + 1u : flight.lowerFloor;

        if (piece.size() < 3u)
        {
          continue;
        }

        // The route's own outline, not a shrunken copy of it.
        //
        // It was pulled in by a hand's breadth because `geom2::contains` counts
        // a boundary point as inside, and a landing stands *against* walls by
        // design - so tested at its own edges every wall it touches reads as a
        // wall across it, and this pass once knocked down the walls of the room
        // the stair was in.
        //
        // But the inset hides the fault it was meant to find. A wall 0.08 m
        // inside a landing's far edge is a wall 0.08 m in front of the top
        // tread, and a 0.18 m inset cannot see it. The distinction wanted is
        // not "how near the edge" but "does this wall reach *into* the
        // landing", and that is measured below by how deep it gets rather than
        // by moving the boundary.
        geom2::Polygon ring;

        for (const Position &q : piece)
        {
          ring.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
        }

        if (geom2::areaMetres(ring) < 0)
        {
          std::reverse(ring.outer.begin(), ring.outer.end());
        }

        if (geom2::areaMetres(ring) < 0)
        {
          std::reverse(ring.outer.begin(), ring.outer.end());
        }

        if (ring.outer.size() >= 3u && geom2::polygonFault(ring).empty())
        {
          if (end > 0 && end < 3 && floor < out.floors.size())
          {
            out.floors[floor].circulation.push_back(piece);
          }

          routes.push_back({floor, std::move(ring)});
        }
      }
    }

    // No post standing in a stair, either.
    //
    // A post carrying a gallery is spaced along the front of its deck, and the
    // flight up to that gallery arrives somewhere along the same front - so
    // one of them lands in the middle of the steps often enough to be the rule
    // rather than the exception. It is swept here rather than guarded where
    // the posts are made, because the balcony builds its own stair *after* its
    // posts: a guard there is a rule asked before the thing it asks about
    // exists. The bay it would have carried is better spanned than blocked.
    if (!routes.empty())
    {
      std::vector<rewrite::DwellingPillar> standingPosts;

      for (rewrite::DwellingPillar &post : out.pillars)
      {
        double px = 0, pz = 0;

        for (const Position &q : post.footprint)
        {
          px += q.x / double(post.footprint.size());
          pz += q.z / double(post.footprint.size());
        }

        // Against the stair's own outline, not the shrunk one.
        //
        // `routes` is pulled in by a hand's breadth so that a wall running
        // *along* a landing's edge does not read as a wall across it. A post
        // is a point, not a line, so it needs no such allowance - and tested
        // against the shrunk ring a post sitting just inside the edge of a
        // flight came out clear.
        bool inTheWay = false;

        for (const DwellingStair &flight : out.stairs)
        {
          for (const std::vector<Position> *piece : {&flight.opening, &flight.lowerLanding, &flight.upperLanding})
          {
            if (piece->size() < 3u || inTheWay)
            {
              continue;
            }

            geom2::Polygon ring;

            for (const Position &q : *piece)
            {
              ring.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
            }

            if (geom2::areaMetres(ring) < 0)
            {
              std::reverse(ring.outer.begin(), ring.outer.end());
            }

            inTheWay = inTheWay || (ring.outer.size() >= 3u && geom2::contains(ring, geom2::Point{geom2::toUnit(px), geom2::toUnit(pz)}));
          }
        }

        if (!inTheWay)
        {
          standingPosts.push_back(std::move(post));
        }
      }

      out.pillars = std::move(standingPosts);
    }

    if (!routes.empty())
    {
      std::vector<uint64_t> felled;
      std::vector<DwellingWall> standing;

      for (DwellingWall &wall : out.walls)
      {
        // Outside walls stay whatever crosses them: a stair that reaches the
        // outside wall is a different fault and knocking the wall down is not
        // the answer to it. A rail is not in the way of anything.
        bool across = false;

        if (!wall.exterior && wall.kind == rewrite::WallKind::Solid)
        {
          const double dx = wall.b.x - wall.a.x, dz = wall.b.z - wall.a.z;
          const double length = std::hypot(dx, dz);

          for (const std::pair<uint32_t, geom2::Polygon> &route : routes)
          {
            if (route.first != wall.floor || length < 1e-6 || across)
            {
              continue;
            }

            // Does this wall enter the route at all?
            //
            // Asked as a share of the wall's own length to begin with - more
            // than a quarter of it inside - and that is the wrong denominator.
            // A partition is long and a landing is a metre square, so a wall
            // cutting clean through a landing has a twentieth of itself inside
            // it and survived the test, which is how a flight came to arrive
            // with 0.08 m of clear floor in front of it. The route is the small
            // thing; any of the wall inside it is a wall in the way.
            //
            // Being liberal costs nothing here because the wall is *cut* rather
            // than felled: a wall that merely clips a corner loses that corner
            // and keeps the rest.
            // How far into the route this wall reaches.
            //
            // A wall lying along an edge is "inside" at every point and never
            // more than nothing deep; a wall crossing the landing gets a good
            // way in. That depth is the distinction, and measuring it here
            // rather than moving the boundary means a wall a hand's breadth
            // inside the far edge - a wall 80 mm in front of the top tread -
            // is seen, which an inset boundary cannot do.
            const int steps = std::max(8, int(length / .08));

            for (int n = 0; n <= steps && !across; ++n)
            {
              const double t = double(n) / double(steps);

              across = across || crossesRoute(route.second, wall.a.x + dx * t, wall.a.z + dz * t, dx / length, dz / length);
            }
          }
        }

        if (!across)
        {
          standing.push_back(std::move(wall));

          continue;
        }

        // Cut round the stair, not knocked down.
        //
        // Felling the whole wall opened the partition between two private
        // rooms - a parlour and a bedroom shared a boundary and a metre and a
        // half of it simply was not there. What is in the way is the stretch
        // that crosses the landing; the rest of the wall is a wall and stays.
        felled.push_back(wall.id);

        const double dx = wall.b.x - wall.a.x, dz = wall.b.z - wall.a.z;
        const double length = std::hypot(dx, dz);
        const int steps = std::max(4, int(length / .15));
        double runFrom = 0;
        bool open = false;

        const auto keep = [&](const double from, const double to) {
          if ((to - from) * length < .5)
          {
            return;
          }

          DwellingWall piece = wall;

          piece.id = wallId++;
          piece.a = Position{wall.a.x + dx * from, wall.a.y, wall.a.z + dz * from};
          piece.b = Position{wall.a.x + dx * to, wall.b.y, wall.a.z + dz * to};

          standing.push_back(std::move(piece));
        };

        for (int n = 0; n <= steps; ++n)
        {
          const double t = double(n) / double(steps);
          bool blocked = false;

          for (const std::pair<uint32_t, geom2::Polygon> &route : routes)
          {
            if (route.first != wall.floor)
            {
              continue;
            }

            blocked = blocked || crossesRoute(route.second, wall.a.x + dx * t, wall.a.z + dz * t, dx / length, dz / length);
          }

          if (blocked)
          {
            if (open)
            {
              keep(runFrom, t);
            }

            open = false;

            continue;
          }

          if (!open)
          {
            runFrom = t;
            open = true;
          }
        }

        if (open)
        {
          keep(runFrom, 1.);
        }
      }

      if (!felled.empty())
      {
        out.walls = std::move(standing);

        std::vector<rewrite::DwellingOpening> openings;

        for (rewrite::DwellingOpening &hole : out.openings)
        {
          bool gone = false;

          for (const uint64_t id : felled)
          {
            gone = gone || hole.wall == id;
          }

          if (!gone)
          {
            openings.push_back(std::move(hole));
          }
        }

        out.openings = std::move(openings);

        std::vector<rewrite::DwellingDoor> doors;

        for (rewrite::DwellingDoor &door : out.doors)
        {
          bool gone = false;

          for (const uint64_t id : felled)
          {
            gone = gone || door.wall == id;
          }

          if (!gone)
          {
            doors.push_back(std::move(door));
          }
        }

        out.doors = std::move(doors);
      }
    }
  }

  // ---- every storey is walled all the way round -----------------------------
  //
  // A building has to be closed. Nineteen stretches of storey outline in the
  // corpus had no wall anywhere near them - whole gable ends and, on every
  // tavern and pub, the entire street front of the upper storey.
  //
  // The cause is one line in the plate arithmetic and it is not worth
  // unpicking there. A storey's ground is the plate less the tower shaft, less
  // the hall where it rises through, and less the bay on the ground floor; on
  // a tavern the hall reaches the full width behind the bay, so subtracting it
  // cuts the upper plate into two pieces - the body of the floor, and a strip
  // over the bay - and only the larger piece survives. The strip is dropped,
  // no rooms are cut on it, and so no walls are built along the front of it,
  // while the *massing* quite rightly carries the storey out to the street.
  //
  // Closing the outline here fixes that and anything else that leaves a hole:
  // what is asserted about a building is that you cannot see through it, and
  // this is the pass that makes that true rather than one of the several
  // places that could each make it true for their own case.
  {
    const auto walled = [&](const uint32_t floor, const double x, const double z) {
      const double base = b.foundation + double(floor) * b.floorRise;

      for (const DwellingWall &wall : out.walls)
      {
        if (wall.floor > floor)
        {
          continue;
        }

        // A wall from a lower storey counts where it is tall enough to reach
        // this one, which is how a hall two storeys tall is walled.
        if (wall.floor < floor && !(wall.height > 0 && wall.a.y + wall.height > base + .1))
        {
          continue;
        }

        const double dx = wall.b.x - wall.a.x, dz = wall.b.z - wall.a.z;
        const double run = dx * dx + dz * dz;

        if (run < 1e-9)
        {
          continue;
        }

        const double t = std::clamp(((x - wall.a.x) * dx + (z - wall.a.z) * dz) / run, 0., 1.);

        if (std::hypot(wall.a.x + dx * t - x, wall.a.z + dz * t - z) < s.wallThickness * 1.5)
        {
          return true;
        }
      }

      return false;
    };

    std::vector<DwellingWall> closing;

    for (const rewrite::DwellingStorey &storey : out.storeys)
    {
      if (storey.outer.size() < 3u)
      {
        continue;
      }

      for (size_t i = 0; i < storey.outer.size(); ++i)
      {
        const Position &from = storey.outer[i], &to = storey.outer[(i + 1u) % storey.outer.size()];
        const double length = std::hypot(to.x - from.x, to.z - from.z);

        if (length < .6)
        {
          continue;
        }

        // The wall's line is half a thickness inside the outline, because the
        // outline is the outside face of the building and a wall is drawn from
        // its middle.
        const double dx = (to.x - from.x) / length, dz = (to.z - from.z) / length;
        const double inX = dz * s.wallThickness * .5, inZ = -dx * s.wallThickness * .5;

        const int steps = std::max(2, int(length / .3));
        double runFrom = 0;
        bool open = false;

        const auto close = [&](const double a, const double c) {
          if (c - a < .6)
          {
            return;
          }

          DwellingWall wall;
          wall.id = wallId++;
          wall.floor = storey.level;
          wall.a = Position{from.x + dx * a + inX, storey.base, from.z + dz * a + inZ};
          wall.b = Position{from.x + dx * c + inX, storey.base, from.z + dz * c + inZ};
          wall.thickness = s.wallThickness;
          wall.exterior = true;
          wall.kind = rewrite::WallKind::Solid;

          closing.push_back(std::move(wall));
        };

        for (int n = 0; n <= steps; ++n)
        {
          const double t = length * double(n) / double(steps);

          if (walled(storey.level, from.x + dx * t, from.z + dz * t))
          {
            if (open)
            {
              close(runFrom, t);
            }

            open = false;

            continue;
          }

          if (!open)
          {
            runFrom = t;
            open = true;
          }
        }

        if (open)
        {
          close(runFrom, length);
        }
      }
    }

    for (DwellingWall &wall : closing)
    {
      out.walls.push_back(std::move(wall));
    }
  }

  // ---- the ceiling ----------------------------------------------------------
  //
  // One surface per storey that has no storey over it, at that storey's own
  // head. A two storey range and the single storey wing beside it therefore
  // get one each, at their own heights, and where a storey *does* have another
  // above it the boards of that one are already its ceiling and nothing is
  // added.
  //
  // Built by subtraction, because everything that must not be ceiled is a
  // shape the plan already knows: the plate of the storey above (which has its
  // own boards), the tower shaft, the chimney, and the void over a hall open
  // to its roof. Laid without those subtractions the ceiling is a lid - over
  // the inside of the tower, over the flue, over the hall.
  //
  // Triangulated, because a plate with a hole in it is not a fan from one
  // vertex and a storey plate can be an L.
  // The roof over a point: the highest face covering it, read off its plane.
  //
  // Per point, because that is the grain the ceiling is checked at. Settled
  // for a whole piece instead, a polygon whose centroid happens to fall
  // outside every roof face gets no clamp at all while the triangles cut from
  // it plainly stand under one - and three ceilings came out through the tiles
  // for exactly that reason.
  const auto roofOver = [&](const double x, const double z) {
    double over = -1e300;

    for (const rewrite::DwellingRoofFace &roof : out.roofs)
    {
      if (roof.gable || roof.soffit || roof.polygon.size() < 3u)
      {
        continue;
      }

      geom2::Polygon ring;

      for (const Position &q : roof.polygon)
      {
        ring.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
      }

      if (geom2::areaMetres(ring) < 0)
      {
        std::reverse(ring.outer.begin(), ring.outer.end());
      }

      if (ring.outer.size() < 3u || !geom2::contains(ring, geom2::Point{geom2::toUnit(x), geom2::toUnit(z)}))
      {
        continue;
      }

      const Position &a = roof.polygon[0];
      double nx = 0, ny = 0, nz = 0;

      for (size_t k = 2; k < roof.polygon.size() && ny == 0; ++k)
      {
        const Position &q = roof.polygon[1], &r = roof.polygon[k];
        const double ux = q.x - a.x, uy = q.y - a.y, uz = q.z - a.z;
        const double vx = r.x - a.x, vy = r.y - a.y, vz = r.z - a.z;

        nx = uy * vz - uz * vy;
        ny = uz * vx - ux * vz;
        nz = ux * vy - uy * vx;
      }

      if (std::abs(ny) > 1e-7)
      {
        over = std::max(over, a.y - (nx * (x - a.x) + nz * (z - a.z)) / ny);
      }
    }

    return over;
  };

  for (uint32_t floor = 0; floor < uint32_t(storeyPlan.size()); ++floor)
  {
    if (storeyPlan[floor].outer.size() < 3u)
    {
      continue;
    }

    std::vector<geom2::Operand> difference{geom2::toOperand(storeyPlan[floor])};

    // The storey above, where there is one: its floor is this one's ceiling.
    if (floor + 1u < uint32_t(storeyPlan.size()) && storeyPlan[floor + 1u].outer.size() >= 3u)
    {
      difference.push_back(geom2::toOperand(storeyPlan[floor + 1u]));
    }

    // The shaft, which is open all the way up and is where the stair is.
    if (towerStoreys > floor + 1u && shaftOutline.outer.size() >= 3u)
    {
      difference.push_back(geom2::toOperand(shaftOutline));
    }

    // The flue.
    if (chimneyPlan.outer.size() >= 3u)
    {
      difference.push_back(geom2::toOperand(chimneyPlan));
    }

    // ...and the hall, where it is open to its roof: a ceiling over a hall at
    // the head of its first storey is the floor that being open to the roof
    // means it has not got.
    if (hallFloors > floor + 1u && hallPlan.outer.size() >= 3u)
    {
      difference.push_back(geom2::toOperand(hallPlan));
    }

    // ...and the ground under any wing whose roof is *below* this storey's
    // head, so that no piece spans two roofs at different heights.
    //
    // A ceiling is one plane, and a plane under two roofs has to go under the
    // lower of them - which on an L with a single storey cross wing drops the
    // whole main range's ceiling to the cross wing's eaves and loses it
    // through the floor. Cutting the piece at the wing boundary is the fix:
    // each ceiling then lies under one roof and sits flat under it, and the
    // wing that was cut out gets its own ceiling on its own storey.
    for (const rewrite::DwellingWing &wing : out.wings)
    {
      if (wing.footprint.size() < 3u || wing.eavesHeight > b.foundation + double(floor + 1u) * b.floorRise - .3)
      {
        continue;
      }

      geom2::Polygon lower;

      for (const Position &q : wing.footprint)
      {
        lower.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
      }

      if (geom2::areaMetres(lower) < 0)
      {
        std::reverse(lower.outer.begin(), lower.outer.end());
      }

      if (lower.outer.size() >= 3u && geom2::polygonFault(lower).empty())
      {
        difference.push_back(geom2::toOperand(lower));
      }
    }

    std::vector<geom2::Polygon> left{storeyPlan[floor]};

    if (difference.size() > 1u && !geom2::booleanOp(difference, geom2::Operation::Difference, {}, left, nullptr))
    {
      continue;
    }

    // Pulled in by the wall's thickness, so the ceiling stops at the inside
    // face of the wall rather than running out through it. The storey plate is
    // the *outside* of the building: a ceiling laid to it reaches the middle
    // of every wall and pokes out of the gable ends, which is the wall
    // crossing the ceiling seen from the other side.
    const double storeyHead = b.foundation + double(floor + 1u) * b.floorRise - s.floorBoardMetres;

    for (const geom2::Polygon &piece : left)
    {
      if (piece.outer.size() < 3u || geom2::areaMetres(piece) < .5)
      {
        continue;
      }

      const double y = storeyHead;

      std::vector<geom2::Polygon> inside;

      if (!geom2::offsetPolygon(piece, -s.wallThickness * .5, {}, inside, nullptr) || inside.empty())
      {
        inside = {piece};
      }

      for (const geom2::Polygon &room : inside)
      {
        if (room.outer.size() < 3u || geom2::areaMetres(room) < .5)
        {
          continue;
        }

        std::vector<std::array<uint32_t, 3>> triangles;
        std::vector<geom2::Point> vertices;

        if (!geom3::triangulate(room, triangles, vertices, nullptr))
        {
          continue;
        }

        // One height for the whole piece: the lowest the roof comes over any
        // part of it.
        //
        // Settled per triangle instead, the ceiling tears - adjacent triangles
        // sit at different heights and the surface reads as a flight of steps
        // with a diagonal seam across it, which is the triangulation showing
        // through. A ceiling is one plane; where the roof over it varies, the
        // plane goes under the lowest of it.
        double height = y;

        for (const std::array<uint32_t, 3> &face : triangles)
        {
          const double mx = (geom2::toMetres(vertices[face[0]].x) + geom2::toMetres(vertices[face[1]].x) + geom2::toMetres(vertices[face[2]].x)) / 3.;
          const double mz = (geom2::toMetres(vertices[face[0]].y) + geom2::toMetres(vertices[face[1]].y) + geom2::toMetres(vertices[face[2]].y)) / 3.;
          const double over = roofOver(mx, mz);

          if (over > -1e299)
          {
            height = std::min(height, over - .05);
          }
        }

        // The face first, and the triangles from it.
        //
        // This is the order the geometry is actually settled in: the planner
        // decides one polygon at one height, with the shaft and the flue and
        // the void over a hall taken out of it as holes, and triangulating
        // that is a separate and mechanical step. Emitting triangles directly
        // leaves nothing that says what the ceiling *is*, so nothing can be
        // held against the drawing of it - and a triangulation that quietly
        // fills a hole looks exactly like one that did not.
        {
          rewrite::DwellingFace surface;
          surface.height = height;
          surface.floor = floor;

          for (const geom2::Point &q : room.outer)
          {
            surface.outer.push_back(Position{geom2::toMetres(q.x), height, geom2::toMetres(q.y)});
          }

          for (const geom2::Ring &hole : room.holes)
          {
            std::vector<Position> ring;

            for (const geom2::Point &q : hole)
            {
              ring.push_back(Position{geom2::toMetres(q.x), height, geom2::toMetres(q.y)});
            }

            if (ring.size() >= 3u)
            {
              surface.holes.push_back(std::move(ring));
            }
          }

          if (surface.outer.size() >= 3u)
          {
            out.ceilingFaces.push_back(std::move(surface));
          }
        }

        for (const std::array<uint32_t, 3> &face : triangles)
        {
          const double ax = geom2::toMetres(vertices[face[0]].x), az = geom2::toMetres(vertices[face[0]].y);
          const double bx = geom2::toMetres(vertices[face[1]].x), bz = geom2::toMetres(vertices[face[1]].y);
          const double dx = geom2::toMetres(vertices[face[2]].x), dz = geom2::toMetres(vertices[face[2]].y);

          const Position a{ax, height, az};
          const Position c{bx, height, bz};
          const Position d{dx, height, dz};

          // Not the slivers a triangulation leaves at a thin corner. They are
          // invisible as surface and they render as a line, which on a ceiling
          // is a crack.
          if (std::abs((c.x - a.x) * (d.z - a.z) - (d.x - a.x) * (c.z - a.z)) * .5 < 1e-3)
          {
            continue;
          }

          out.ceilings.push_back({a, c, d});
        }
      }
    }
  }

  // ---- no doorway opens onto a flight of steps ------------------------------
  //
  // The openings are cut per storey, and three of the four stairs a building
  // can have are built after every storey is done - the tower's, the external
  // one to a gallery, the balcony's. So the rule that keeps a doorway off a
  // flight cannot be enforced only where doorways are made; it has to be
  // checked once more when everything that could block one exists.
  //
  // Slid first, removed second. A door is how a room is reached and deleting
  // one can strand it, so a doorway in the way is walked along its own wall
  // until it finds floor on both sides - which is usually a metre or so, since
  // a flight is narrower than the wall it climbs. Only where the whole wall is
  // stair does the opening go.
  {
    const auto wallById = [&](const uint64_t id) -> const DwellingWall * {
      for (const DwellingWall &wall : out.walls)
      {
        if (wall.id == id)
        {
          return &wall;
        }
      }

      return nullptr;
    };

    std::vector<geom2::Polygon> runs;
    std::vector<uint32_t> runFloor;

    for (const DwellingStair &flight : out.stairs)
    {
      if (flight.opening.size() < 3u)
      {
        continue;
      }

      geom2::Polygon run;

      for (const Position &q : flight.opening)
      {
        run.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
      }

      if (geom2::areaMetres(run) < 0)
      {
        std::reverse(run.outer.begin(), run.outer.end());
      }

      if (run.outer.size() >= 3u && geom2::polygonFault(run).empty())
      {
        runs.push_back(std::move(run));
        runFloor.push_back(flight.lowerFloor);
      }
    }

    if (!runs.empty())
    {
      // Is there floor to stand on either side of a doorway laid from `a` to
      // `b`? A stride in, on both sides, sampled across the run.
      const auto clear = [&](const uint32_t floor, const Position &a, const Position &c) {
        const double dx = c.x - a.x, dz = c.z - a.z;
        const double length = std::hypot(dx, dz);

        if (length < 1e-6)
        {
          return true;
        }

        for (size_t k = 0; k < runs.size(); ++k)
        {
          if (runFloor[k] != floor && runFloor[k] + 1u != floor)
          {
            continue;
          }

          for (int n = 0; n <= 4; ++n)
          {
            const double t = double(n) / 4.;
            const double px = a.x + dx * t, pz = a.z + dz * t;

            for (const double side : {-1., 1.})
            {
              const double qx = px - dz / length * side * s.doorClearanceMetres;
              const double qz = pz + dx / length * side * s.doorClearanceMetres;

              if (geom2::contains(runs[k], geom2::Point{geom2::toUnit(qx), geom2::toUnit(qz)}))
              {
                return false;
              }
            }
          }
        }

        return true;
      };

      std::vector<rewrite::DwellingOpening> kept;
      std::vector<uint64_t> lost;

      for (rewrite::DwellingOpening &hole : out.openings)
      {
        if (hole.kind != rewrite::OpeningKind::Door || clear(hole.floor, hole.a, hole.b))
        {
          kept.push_back(std::move(hole));

          continue;
        }

        const DwellingWall *wall = wallById(hole.wall);
        const double length = wall != nullptr ? std::hypot(wall->b.x - wall->a.x, wall->b.z - wall->a.z) : 0.;
        const double span = std::hypot(hole.b.x - hole.a.x, hole.b.z - hole.a.z);
        bool moved = false;

        if (wall != nullptr && length > span + s.wallThickness * 2)
        {
          const double dx = (wall->b.x - wall->a.x) / length, dz = (wall->b.z - wall->a.z) / length;

          // Walked along the wall a step at a time, nearest first, so a
          // doorway that only just clips a flight barely moves.
          for (int step = 1; step <= 16 && !moved; ++step)
          {
            for (const double way : {1., -1.})
            {
              const double was = (hole.a.x - wall->a.x) * dx + (hole.a.z - wall->a.z) * dz;
              const double from = was + way * double(step) * .15;

              if (from < s.wallThickness || from + span > length - s.wallThickness)
              {
                continue;
              }

              const Position a{wall->a.x + dx * from, hole.a.y, wall->a.z + dz * from};
              const Position c{wall->a.x + dx * (from + span), hole.b.y, wall->a.z + dz * (from + span)};

              if (clear(hole.floor, a, c))
              {
                hole.a = a;
                hole.b = c;
                moved = true;

                break;
              }
            }
          }
        }

        if (moved)
        {
          kept.push_back(std::move(hole));
        }
        else
        {
          lost.push_back(hole.wall);
        }
      }

      out.openings = std::move(kept);

      // A door with no hole left for it is not a door.
      if (!lost.empty())
      {
        std::vector<rewrite::DwellingDoor> doors;

        for (rewrite::DwellingDoor &door : out.doors)
        {
          bool gone = false;

          for (const uint64_t wall : lost)
          {
            gone = gone || door.wall == wall;
          }

          if (!gone)
          {
            doors.push_back(std::move(door));
          }
        }

        out.doors = std::move(doors);
      }
    }
  }

  // ---- a way off every landing ----------------------------------------------
  //
  // Both ends of a flight have to open onto the circulation of their own
  // floor. Preferring a room next to a corridor gets most of them there and
  // cannot get all of them: a corridor is narrow, so the room that can take a
  // flight is often not the room beside the corridor, and the storey above was
  // cut without knowing where the flight would arrive on it.
  //
  // So where a landing has no route off it, one is cut. A doorway in the wall
  // nearest the landing is what a builder would do and what the plan is short
  // of - the alternative, moving the stair until every storey agrees, is the
  // problem that has no solution on an arbitrary plan.
  //
  // After the flight-clearance pass, so the doors it cuts are not swept away
  // by it, and clear of the flights by construction instead.
  for (const DwellingStair &flight : out.stairs)
  {
    for (int end = 0; end < 2; ++end)
    {
      const std::vector<Position> &pad = end == 0 ? flight.lowerLanding : flight.upperLanding;
      const uint32_t floor = flight.lowerFloor + uint32_t(end);

      if (pad.size() < 3u || floor >= out.floors.size())
      {
        continue;
      }

      double cx = 0, cz = 0;

      for (const Position &q : pad)
      {
        cx += q.x / double(pad.size());
        cz += q.z / double(pad.size());
      }

      // Already connected? A corridor within a few strides, or a doorway.
      bool onRoute = false;

      for (const rewrite::DwellingRoom &room : out.rooms)
      {
        if (onRoute || room.floor != floor || room.use != rewrite::RoomUse::Corridor || room.boundary.size() < 3u)
        {
          continue;
        }

        geom2::Ring ring;

        for (const Position &q : room.boundary)
        {
          ring.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
        }

        onRoute = onRoute || distanceToRing(ring, cx, cz) < 2.6;
      }

      for (const rewrite::DwellingOpening &hole : out.openings)
      {
        onRoute = onRoute || (hole.kind == rewrite::OpeningKind::Door && hole.floor == floor && std::hypot((hole.a.x + hole.b.x) * .5 - cx, (hole.a.z + hole.b.z) * .5 - cz) < 2.4);
      }

      if (onRoute)
      {
        continue;
      }

      // The nearest interior wall that a door will fit in, and a door in it at
      // the point nearest the landing.
      const DwellingWall *best = nullptr;
      double bestAway = 1e300, bestAlong = 0;

      for (const DwellingWall &wall : out.walls)
      {
        if (wall.floor != floor || wall.kind != rewrite::WallKind::Solid)
        {
          continue;
        }

        const double dx = wall.b.x - wall.a.x, dz = wall.b.z - wall.a.z;
        const double length = std::hypot(dx, dz);
        const double span = s.doorWidthMetres + s.openingMarginMetres * 2;

        if (length < span + s.wallThickness * 2)
        {
          continue;
        }

        const double t = std::clamp(((cx - wall.a.x) * dx + (cz - wall.a.z) * dz) / (length * length), 0., 1.) * length;
        const double away = std::hypot(wall.a.x + dx / length * t - cx, wall.a.z + dz / length * t - cz);

        if (away < bestAway && away < 3.0)
        {
          bestAway = away;
          bestAlong = t;
          best = &wall;
        }
      }

      if (best == nullptr)
      {
        continue;
      }

      const double length = std::hypot(best->b.x - best->a.x, best->b.z - best->a.z);
      const double dx = (best->b.x - best->a.x) / length, dz = (best->b.z - best->a.z) / length;
      const double span = s.doorWidthMetres + s.openingMarginMetres * 2;
      const double from = std::clamp(bestAlong - span * .5, s.wallThickness, length - span - s.wallThickness);

      // Not over anything already cut in that wall.
      bool clash = false;

      for (const rewrite::DwellingOpening &had : out.openings)
      {
        if (had.wall != best->id)
        {
          continue;
        }

        const double ta = (had.a.x - best->a.x) * dx + (had.a.z - best->a.z) * dz;
        const double tb = (had.b.x - best->a.x) * dx + (had.b.z - best->a.z) * dz;

        clash = clash || (std::min(ta, tb) < from + span + s.wallThickness && std::max(ta, tb) > from - s.wallThickness);
      }

      if (clash)
      {
        continue;
      }

      // ...and not onto a flight. This runs after the pass that sweeps those
      // out, so a door cut here is a door nothing will check again: the rule
      // that a doorway opens onto floor rather than onto steps has to be kept
      // here, by construction.
      bool onSteps = false;

      for (const DwellingStair &other : out.stairs)
      {
        if (other.opening.size() < 3u || (other.lowerFloor != floor && other.lowerFloor + 1u != floor))
        {
          continue;
        }

        geom2::Polygon run;

        for (const Position &q : other.opening)
        {
          run.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
        }

        if (geom2::areaMetres(run) < 0)
        {
          std::reverse(run.outer.begin(), run.outer.end());
        }

        if (run.outer.size() < 3u)
        {
          continue;
        }

        for (int n = 0; n <= 4 && !onSteps; ++n)
        {
          const double t = from + span * double(n) / 4.;
          const double px = best->a.x + dx * t, pz = best->a.z + dz * t;

          for (const double side : {-1., 1.})
          {
            const double qx = px - dz * side * s.doorClearanceMetres, qz = pz + dx * side * s.doorClearanceMetres;

            // A point on the landing this door is being cut for is floor, not
            // an obstruction. Without this the rule contradicts itself - the
            // door exists to serve a landing that is by definition next to a
            // flight, so the flight beside it disqualifies every position the
            // door could take.
            bool onLanding = false;

            for (const DwellingStair &any : out.stairs)
            {
              for (const std::vector<Position> *other : {&any.lowerLanding, &any.upperLanding})
              {
                if (other->size() < 3u)
                {
                  continue;
                }

                geom2::Polygon patch;

                for (const Position &corner : *other)
                {
                  patch.outer.push_back({geom2::toUnit(corner.x), geom2::toUnit(corner.z)});
                }

                if (geom2::areaMetres(patch) < 0)
                {
                  std::reverse(patch.outer.begin(), patch.outer.end());
                }

                onLanding = onLanding || (patch.outer.size() >= 3u && geom2::contains(patch, geom2::Point{geom2::toUnit(qx), geom2::toUnit(qz)}));
              }
            }

            if (onLanding)
            {
              continue;
            }

            onSteps = onSteps || geom2::contains(run, geom2::Point{geom2::toUnit(qx), geom2::toUnit(qz)});
          }
        }
      }

      if (onSteps)
      {
        continue;
      }

      rewrite::DwellingOpening hole;
      hole.wall = best->id;
      hole.room = 0;
      hole.floor = floor;
      hole.a = Position{best->a.x + dx * from, best->a.y, best->a.z + dz * from};
      hole.b = Position{best->a.x + dx * (from + span), best->a.y, best->a.z + dz * (from + span)};
      hole.sill = 0;
      hole.head = s.doorHeadMetres;
      hole.base = 0;
      hole.top = std::min((best->height > 0 ? best->height : b.floorRise) - s.openingHeadroomMetres, s.doorHeadMetres + s.openingMarginMetres);
      hole.thickness = best->thickness;
      hole.kind = rewrite::OpeningKind::Door;
      hole.exterior = best->exterior;

      out.openings.push_back(std::move(hole));
    }
  }

  // ---- every landing is floor -----------------------------------------------
  //
  // The boards are laid per storey partway through, and three of the four
  // stairs are built after that - the tower's, the one to a gallery, the
  // balcony's. Their landings were therefore added to nothing: a flight
  // finished in mid air at the top, which is the same fault the stair core had
  // and in a place the core's fix could not reach.
  //
  // So the landings are swept once at the end. Anything already over floor is
  // left alone; anything not gets its own piece of deck, clipped to the storey
  // it belongs to so a landing overhanging the wall does not become a balcony.
  {
    const auto standing = [&](const uint32_t floor, const double x, const double z) {
      if (floor >= out.floors.size())
      {
        return true;
      }

      for (const std::vector<Position> &slab : out.floors[floor].slabs)
      {
        geom2::Polygon ring;

        for (const Position &q : slab)
        {
          ring.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
        }

        if (geom2::areaMetres(ring) < 0)
        {
          std::reverse(ring.outer.begin(), ring.outer.end());
        }

        if (ring.outer.size() >= 3u && geom2::contains(ring, geom2::Point{geom2::toUnit(x), geom2::toUnit(z)}))
        {
          return true;
        }
      }

      return false;
    };

    for (const DwellingStair &flight : out.stairs)
    {
      for (int end = 0; end < 2; ++end)
      {
        const std::vector<Position> &pad = end == 0 ? flight.lowerLanding : flight.upperLanding;
        const uint32_t floor = flight.lowerFloor + uint32_t(end);

        if (pad.size() < 3u || floor >= out.floors.size())
        {
          continue;
        }

        double cx = 0, cz = 0;

        for (const Position &q : pad)
        {
          cx += q.x / double(pad.size());
          cz += q.z / double(pad.size());
        }

        if (standing(floor, cx, cz))
        {
          continue;
        }

        geom2::Polygon patch;

        for (const Position &q : pad)
        {
          patch.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
        }

        if (geom2::areaMetres(patch) < 0)
        {
          std::reverse(patch.outer.begin(), patch.outer.end());
        }

        if (patch.outer.size() < 3u || !geom2::polygonFault(patch).empty())
        {
          continue;
        }

        std::vector<geom2::Polygon> pieces{patch};

        if (floor < storeyPlan.size() && storeyPlan[floor].outer.size() >= 3u)
        {
          std::vector<geom2::Polygon> inside;

          // ...and the raw landing where that leaves nothing. A tower is a
          // massing part rather than part of the storey plate, so a landing
          // inside a shaft clips away to nothing against a plate that was
          // never drawn round it - and the flight it belongs to is standing
          // right there, so the answer is not "no landing".
          if (geom2::booleanOp({geom2::toOperand(patch), geom2::toOperand(storeyPlan[floor])}, geom2::Operation::Intersection, {}, inside, nullptr) && !inside.empty())
          {
            pieces = std::move(inside);
          }
        }

        const double y = out.floors[floor].elevation + s.floorBoardMetres;

        for (const geom2::Polygon &piece : pieces)
        {
          if (geom2::areaMetres(piece) < .12)
          {
            continue;
          }

          std::vector<std::array<uint32_t, 3>> triangles;
          std::vector<geom2::Point> vertices;

          if (!geom3::triangulate(piece, triangles, vertices, nullptr))
          {
            continue;
          }

          for (const std::array<uint32_t, 3> &face : triangles)
          {
            out.floors[floor].slabs.push_back({
              Position{geom2::toMetres(vertices[face[0]].x), y, geom2::toMetres(vertices[face[0]].y)},
              Position{geom2::toMetres(vertices[face[1]].x), y, geom2::toMetres(vertices[face[1]].y)},
              Position{geom2::toMetres(vertices[face[2]].x), y, geom2::toMetres(vertices[face[2]].y)}});
          }
        }
      }
    }
  }

  // ---- the well every flight rises through ----------------------------------
  //
  // Last of the passes that touch the boards, and it has to be: the sweep that
  // gives every landing a floor runs after the storeys are boarded and adds
  // patches back, and one flight's landing can sit over the next flight's run -
  // so a well cut before that pass is a well the next pass boards over again.
  //
  // The boards are laid per storey partway through, and they cut a well for
  // every flight that exists *at that moment* - which is the flights planned
  // inside the floor loop and none of the others. The tower's, the gallery's
  // and the stair hall's are all built later, so eleven flights in the corpus
  // rose into a floor with boards across the top of them: a stair to a
  // ceiling.
  //
  // Swept once at the end, where every flight exists. A slab is a triangle, so
  // a triangle whose middle is over a flight is a triangle over the well.
  for (uint32_t floor = 1; floor < uint32_t(out.floors.size()); ++floor)
  {
    std::vector<geom2::Polygon> wells;

    for (const DwellingStair &flight : out.stairs)
    {
      if (flight.lowerFloor + 1u != floor || flight.opening.size() < 3u)
      {
        continue;
      }

      geom2::Polygon ring;

      for (const Position &q : flight.opening)
      {
        ring.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
      }

      if (geom2::areaMetres(ring) < 0)
      {
        std::reverse(ring.outer.begin(), ring.outer.end());
      }

      if (ring.outer.size() < 3u || !geom2::polygonFault(ring).empty())
      {
        continue;
      }

      // Less the landings, whosever they are.
      //
      // On a turning stair the corner square is the top of one flight and the
      // foot of the next, so it is a landing *and* inside the next flight's
      // run. Cut as part of the well it leaves the arrival on a hole - the
      // floor is taken away exactly where you step off. A landing is floor
      // wherever it appears, so it is taken out of the well first.
      std::vector<geom2::Operand> difference{geom2::toOperand(ring)};

      for (const DwellingStair &other : out.stairs)
      {
        for (const std::vector<Position> *pad : {&other.lowerLanding, &other.upperLanding})
        {
          if (pad->size() < 3u)
          {
            continue;
          }

          geom2::Polygon patch;

          for (const Position &q : *pad)
          {
            patch.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
          }

          if (geom2::areaMetres(patch) < 0)
          {
            std::reverse(patch.outer.begin(), patch.outer.end());
          }

          if (patch.outer.size() >= 3u && geom2::polygonFault(patch).empty())
          {
            difference.push_back(geom2::toOperand(patch));
          }
        }
      }

      std::vector<geom2::Polygon> open{ring};

      if (difference.size() > 1u && !geom2::booleanOp(difference, geom2::Operation::Difference, {}, open, nullptr))
      {
        open = {ring};
      }

      for (geom2::Polygon &piece : open)
      {
        if (piece.outer.size() >= 3u && geom2::areaMetres(piece) > .2)
        {
          wells.push_back(std::move(piece));
        }
      }
    }

    if (wells.empty())
    {
      continue;
    }

    // The well is cut out of each triangle rather than the triangle dropped.
    //
    // Dropping whole triangles is the cheap spelling and it misses: a slab is
    // triangulated for its own reasons, so one triangle can cover the flight
    // while its middle lies somewhere else entirely. Eight flights were still
    // boarded over after that. Cut the shape out and re-triangulate what is
    // left.
    std::vector<std::vector<Position>> kept;

    for (const std::vector<Position> &slab : out.floors[floor].slabs)
    {
      if (slab.size() < 3u)
      {
        continue;
      }

      const double y = slab.front().y;

      geom2::Polygon patch;

      for (const Position &q : slab)
      {
        patch.outer.push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
      }

      if (geom2::areaMetres(patch) < 0)
      {
        std::reverse(patch.outer.begin(), patch.outer.end());
      }

      if (patch.outer.size() < 3u || !geom2::polygonFault(patch).empty())
      {
        kept.push_back(slab);

        continue;
      }

      std::vector<geom2::Operand> difference{geom2::toOperand(patch)};

      for (const geom2::Polygon &well : wells)
      {
        difference.push_back(geom2::toOperand(well));
      }

      std::vector<geom2::Polygon> left;

      if (!geom2::booleanOp(difference, geom2::Operation::Difference, {}, left, nullptr))
      {
        // The cut failed. Keeping the board is the safe answer everywhere
        // except over the well itself, where it is the fault: better a missing
        // triangle than boards across the top of a flight.
        double cx = 0, cz = 0;

        for (const Position &q : slab)
        {
          cx += q.x / double(slab.size());
          cz += q.z / double(slab.size());
        }

        bool overAWell = false;

        for (const geom2::Polygon &well : wells)
        {
          overAWell = overAWell || geom2::contains(well, geom2::Point{geom2::toUnit(cx), geom2::toUnit(cz)});
        }

        if (!overAWell)
        {
          kept.push_back(slab);
        }

        continue;
      }

      for (const geom2::Polygon &piece : left)
      {
        if (piece.outer.size() < 3u || geom2::areaMetres(piece) < .01)
        {
          continue;
        }

        std::vector<std::array<uint32_t, 3>> triangles;
        std::vector<geom2::Point> vertices;

        if (!geom3::triangulate(piece, triangles, vertices, nullptr))
        {
          continue;
        }

        for (const std::array<uint32_t, 3> &face : triangles)
        {
          kept.push_back({
            Position{geom2::toMetres(vertices[face[0]].x), y, geom2::toMetres(vertices[face[0]].y)},
            Position{geom2::toMetres(vertices[face[1]].x), y, geom2::toMetres(vertices[face[1]].y)},
            Position{geom2::toMetres(vertices[face[2]].x), y, geom2::toMetres(vertices[face[2]].y)}});
        }
      }
    }

    out.floors[floor].slabs = std::move(kept);
  }

  // ---- no rooms on a storey the building does not have there -----------------
  //
  // Last, because the rooms and their partitions are not all emitted until
  // here: run straight after the massing - which is where this was, and where
  // the storeys it needs first exist - it looked at a list the subdivision had
  // not finished writing, and culled a few stragglers while the walls it was
  // aimed at were pushed a thousand lines later.
  //
  // The rooms are cut before the mass is made. Their storey plan comes from
  // offsetting the shell - "a part this wide carries another floor" - and knows
  // nothing about wings, while the massing decides per wing how many storeys
  // each one carries. Where a wing came out shorter than the shell's width
  // would allow, the two disagree, and what the disagreement looks like is a
  // floor of rooms standing over a single storey cross wing: its partitions
  // rise through that wing's roof and slice it into strips.
  //
  // Only the rooms and the walls *between* them go. The outside walls and the
  // boards stay, because they belong to the storey plate rather than to the
  // subdivision, and because the checks that hold this building together are
  // about the outline and the edge of the floor - take those away with the
  // partitions and a cross wing is fixed at the cost of a hole in the side of
  // a mansion.
  {
    std::vector<geom2::Ring> plate(out.storeys.size());

    for (const rewrite::DwellingStorey &storey : out.storeys)
    {
      if (storey.level < plate.size())
      {
        for (const Position &q : storey.outer)
        {
          plate[storey.level].push_back({geom2::toUnit(q.x), geom2::toUnit(q.z)});
        }
      }
    }

    const auto massed = [&](const uint32_t floor, const double x, const double z) {
      // The ground is never culled: its plate has the entrance bay taken out
      // of it by design, and a room standing in a bay is the arrangement.
      if (floor == 0u || floor >= plate.size() || plate[floor].size() < 3u)
      {
        return true;
      }

      for (const double probe : {0., .08, -.08})
      {
        if (geom2::contains(plate[floor], at(x + probe, z)) || geom2::contains(plate[floor], at(x, z + probe)))
        {
          return true;
        }
      }

      return false;
    };

    std::set<uint64_t> gone;

    for (const DwellingRoom &room : out.rooms)
    {
      if (room.boundary.size() < 3u)
      {
        continue;
      }

      // Judged on the whole of it, not on its middle.
      //
      // A room straddling the plate's edge has its centre over the storey and
      // its far wall out in the air above a lower wing's roof - so culled by
      // centre alone it survives, and the partitions on its outside edge
      // survive with it because a surviving room still wants its walls. Most
      // of the room has to be over the storey for the room to be.
      double cx = 0, cz = 0;
      size_t over = 0;

      for (const Position &q : room.boundary)
      {
        cx += q.x / double(room.boundary.size());
        cz += q.z / double(room.boundary.size());
      }

      for (size_t i = 0; i < room.boundary.size(); ++i)
      {
        const Position &p = room.boundary[i];
        const Position &q = room.boundary[(i + 1) % room.boundary.size()];

        // Pulled towards the middle, so a corner sitting exactly on the plate
        // boundary is not what decides a room.
        over += massed(room.floor, (p.x + q.x) * .5 * .8 + cx * .2, (p.z + q.z) * .5 * .8 + cz * .2) ? 1 : 0;
      }

      if (!massed(room.floor, cx, cz) || over * 2u < room.boundary.size())
      {
        gone.insert(room.id);
      }
    }

    if (!gone.empty())
    {
      out.rooms.erase(std::remove_if(out.rooms.begin(), out.rooms.end(), [&](const DwellingRoom &room) { return gone.count(room.id) != 0; }), out.rooms.end());

      // A partition goes only if no surviving room still needs it.
      //
      // Judged against the plate alone it is stricter than the rule that culled
      // the rooms - a room's middle can be over the plate while the wall along
      // its edge is a hand's breadth outside - so rooms were left with
      // stretches of boundary that had no wall on them, which is the one thing
      // the gallery will not have. A wall still standing on a surviving room's
      // boundary is still that room's wall, wherever the plate's edge falls.
      const auto stillWanted = [&](const DwellingWall &wall) {
        const double mx = (wall.a.x + wall.b.x) * .5, mz = (wall.a.z + wall.b.z) * .5;

        for (const DwellingRoom &room : out.rooms)
        {
          if (room.floor != wall.floor || room.boundary.size() < 3u)
          {
            continue;
          }

          for (size_t i = 0; i < room.boundary.size(); ++i)
          {
            const Position &p = room.boundary[i];
            const Position &q = room.boundary[(i + 1) % room.boundary.size()];
            const double ex = q.x - p.x, ez = q.z - p.z;
            const double run = ex * ex + ez * ez;
            const double t = run > 1e-12 ? std::max(0., std::min(1., ((mx - p.x) * ex + (mz - p.z) * ez) / run)) : 0.;

            if (std::hypot(p.x + ex * t - mx, p.z + ez * t - mz) < .45)
            {
              return true;
            }
          }
        }

        return false;
      };

      out.walls.erase(std::remove_if(out.walls.begin(), out.walls.end(), [&](const DwellingWall &wall) {
                        if (wall.exterior || wall.kind != rewrite::WallKind::Solid)
                        {
                          return false;
                        }

                        return !massed(wall.floor, (wall.a.x + wall.b.x) * .5, (wall.a.z + wall.b.z) * .5) && !stillWanted(wall);
                      }),
                      out.walls.end());

      out.doors.erase(std::remove_if(out.doors.begin(), out.doors.end(), [&](const DwellingDoor &door) { return gone.count(door.room) != 0; }), out.doors.end());
      out.openings.erase(std::remove_if(out.openings.begin(), out.openings.end(), [&](const rewrite::DwellingOpening &hole) { return gone.count(hole.room) != 0; }), out.openings.end());
      out.furniture.erase(std::remove_if(out.furniture.begin(), out.furniture.end(), [&](const rewrite::DwellingFurniture &piece) { return gone.count(piece.room) != 0; }), out.furniture.end());
    }
  }

  // ---- boards only where there are no boards already ------------------------
  //
  // A landing is floor laid because the storey has none there - a tower's shaft
  // is cut out of every plate it passes through, so the stair has to bring its
  // own. Where the storey *does* have a floor, or where another piece of
  // decking has already covered that ground, the landing is a second surface
  // over the first: two coplanar sheets arguing over every pixel.
  //
  // Most visible at the top of an open tower, where the walk's own boards cover
  // the whole shaft and every landing under them is laid again on top.
  //
  // Done here rather than at each site because there are five sites, and each
  // of them would have to know what the other four had already laid.
  for (rewrite::DwellingFloor &floor : out.floors)
  {
    std::vector<std::vector<Position>> kept;
    kept.reserve(floor.slabs.size());

    for (const std::vector<Position> &slab : floor.slabs)
    {
      if (slab.size() < 3u)
      {
        continue;
      }

      double cx = 0, cz = 0;

      for (const Position &p : slab)
      {
        cx += p.x / double(slab.size());
        cz += p.z / double(slab.size());
      }

      bool already = false;

      for (const std::vector<Position> &had : kept)
      {
        if (had.size() < 3u || std::abs(had.front().y - slab.front().y) > .06)
        {
          continue;
        }

        // Inside one already laid: the centre of a triangle is a fair test,
        // because a triangulation of one surface meets its neighbours at their
        // edges and not over their middles.
        bool in = false;

        for (size_t i = 0, j = had.size() - 1; i < had.size(); j = i++)
        {
          if ((had[i].z > cz) != (had[j].z > cz) && cx < (had[j].x - had[i].x) * (cz - had[i].z) / (had[j].z - had[i].z) + had[i].x)
          {
            in = !in;
          }
        }

        if (in)
        {
          already = true;

          break;
        }
      }

      if (!already)
      {
        kept.push_back(slab);
      }
    }

    floor.slabs = std::move(kept);
  }

  if ((b.use == rewrite::DwellingUse::Cottage || b.use == rewrite::DwellingUse::ShopHouse || b.use == rewrite::DwellingUse::Lodging || b.use == rewrite::DwellingUse::Tavern) && out.residentCapacity == 0u)
  {
    // A one room cottage sleeps its household in the room it cooks in; the
    // label was missing, not the bed.
    out.residentCapacity = 1u;
  }

  return true;
}

} // namespace procedural::world::generator
