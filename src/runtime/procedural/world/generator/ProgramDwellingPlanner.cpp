#include "procedural/world/generator/ProgramDwellingPlanner.hpp"

#include <cstdio>
#include <cstdlib>

#include "procedural/geom2/Boolean.hpp"
#include "procedural/geom2/Skeleton.hpp"
#include "procedural/geom2/Partition.hpp"
#include "procedural/geom2/Polygon.hpp"
#include "procedural/geom2/Quantise.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>

namespace procedural::world::generator {

using geom2::Point;
using geom2::Polygon;
using geom2::Ring;

namespace {

struct Rng
{
  uint64_t state;

  explicit Rng(const uint64_t seed) : state(seed ^ 0x6A09E667F3BCC909ull) {}

  uint64_t next()
  {
    state += 0x9E3779B97F4A7C15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;

    return z ^ (z >> 31);
  }

  double unit() { return double(next() >> 11) * 0x1.0p-53; }
  double around(const double half) { return (unit() * 2. - 1.) * half; }
};

// Circulation pulls the plan together: it is linked to more rooms than
// anything else, so a spring layout puts it in the middle on its own. Naming
// it here is only for the initial placement, which starts circulation at the
// centre rather than waiting for the springs to discover it.
bool circulation(const RoomUse use)
{
  return use == RoomUse::Hall || use == RoomUse::Passage || use == RoomUse::Landing
         || use == RoomUse::Staircase || use == RoomUse::Lobby;
}

double polygonArea(const Polygon &polygon)
{
  const auto ringArea = [](const Ring &ring) {
    long double twice = 0;

    for (size_t i = 0; i < ring.size(); ++i)
    {
      const Point &a = ring[i];
      const Point &b = ring[(i + 1) % ring.size()];
      twice += (long double)(a.x) * b.y - (long double)(b.x) * a.y;
    }

    return double(twice / 2) * geom2::kMetresPerUnit * geom2::kMetresPerUnit;
  };

  double area = std::abs(ringArea(polygon.outer));

  for (const Ring &hole : polygon.holes)
  {
    area -= std::abs(ringArea(hole));
  }

  return std::max(area, 0.);
}

// Whether two polygons share a stretch of boundary rather than merely touching
// at a point. A door needs a wall to go in.
//
// The test is collinear overlap and not edge equality. Two cells of one
// partition do share whole edges, so equality would do - until a room is the
// union of several cells, which re-welds its ring and splits the edges its
// neighbours still hold whole. Testing equality found those walls before the
// spine was introduced and stopped finding them after, which is a good
// reminder that the weaker test was only ever passing by accident.
bool shareAWall(const Polygon &a, const Polygon &b, const double leastMetres,
                double &midX, double &midZ)
{
  struct Segment
  {
    double x0, z0, x1, z1;
  };

  const auto edges = [](const Polygon &polygon) {
    std::vector<Segment> out;

    const auto ring = [&out](const Ring &r) {
      for (size_t i = 0; i < r.size(); ++i)
      {
        const Point &p = r[i];
        const Point &q = r[(i + 1) % r.size()];
        out.push_back({geom2::toMetres(p.x), geom2::toMetres(p.y),
                       geom2::toMetres(q.x), geom2::toMetres(q.y)});
      }
    };

    ring(polygon.outer);

    for (const Ring &hole : polygon.holes)
    {
      ring(hole);
    }

    return out;
  };

  // Half a millimetre: wide enough to absorb the grid, far below any wall.
  const double slack = 5e-4;

  double bestLength = 0;

  for (const Segment &p : edges(a))
  {
    const double dx = p.x1 - p.x0;
    const double dz = p.z1 - p.z0;
    const double length2 = dx * dx + dz * dz;

    if (length2 < slack * slack)
    {
      continue;
    }

    const double length = std::sqrt(length2);

    for (const Segment &q : edges(b))
    {
      // Parallel, and on the same line.
      const double cross = dx * (q.z1 - q.z0) - dz * (q.x1 - q.x0);
      const double offset = dx * (q.z0 - p.z0) - dz * (q.x0 - p.x0);

      if (std::abs(cross) > slack * length || std::abs(offset) > slack * length)
      {
        continue;
      }

      // Where q's ends fall along p, as a fraction of p.
      const double t0 = ((q.x0 - p.x0) * dx + (q.z0 - p.z0) * dz) / length2;
      const double t1 = ((q.x1 - p.x0) * dx + (q.z1 - p.z0) * dz) / length2;

      const double low = std::max(0., std::min(t0, t1));
      const double high = std::min(1., std::max(t0, t1));

      if (high <= low)
      {
        continue;
      }

      const double shared = (high - low) * length;

      if (shared < leastMetres || shared <= bestLength)
      {
        continue;
      }

      bestLength = shared;
      const double middle = (low + high) * .5;
      midX = p.x0 + dx * middle;
      midZ = p.z0 + dz * middle;
    }
  }

  return bestLength > 0;
}

Polygon rectangle(const double width, const double depth)
{
  Polygon polygon;
  const int64_t w = geom2::toUnit(width * .5);
  const int64_t d = geom2::toUnit(depth * .5);
  polygon.outer = {{-w, -d}, {w, -d}, {w, d}, {-w, d}};

  return polygon;
}

}  // namespace

bool planProgrammeBuilding(const ProgrammeBrief &brief,
                           const uint64_t seed,
                           const ProgrammePlanSettings &settings,
                           const std::vector<std::pair<double, double>> &footprint,
                           ProgrammeBuilding &out,
                           std::string *error,
                           const ProgrammeFloorRegions *regions)
{
  const auto fail = [error](const char *why) {
    if (error != nullptr)
    {
      *error = why;
    }

    return false;
  };

  out = ProgrammeBuilding{};

  if (!sampleProgramme(brief, seed, out.programme))
  {
    return fail("the brief has no programme: the network gives it probability zero");
  }

  const Programme &programme = out.programme;

  // --- the ground ---

  Polygon ground;

  if (footprint.size() >= 3)
  {
    for (const auto &point : footprint)
    {
      if (!geom2::representable(point.first) || !geom2::representable(point.second))
      {
        return fail("a footprint vertex is outside the grid");
      }

      ground.outer.push_back({geom2::toUnit(point.first), geom2::toUnit(point.second)});
    }
  }
  else
  {
    // The case the plan is really about: the network owns the footprint, so
    // the outline comes from the programme rather than from a share of a lot.
    //
    // But it owns the *shape*, not the size. Footprint and the room areas are
    // separate variables sampled from the same network and nothing makes them
    // agree, so a two-room cottage whose footprint node came out generous got
    // a 73 square metre kitchen: the partition has to fill the region it is
    // given, so any surplus is shared among the rooms. Taking the aspect from
    // the footprint and the area from the rooms themselves is the fix, and it
    // is the honest reading of which variable knows what.
    double widest = 0;
    std::map<int32_t, double> askedPerFloor;

    for (const RoomSpec &room : programme.rooms)
    {
      if (enclosesFloorArea(room.use))
      {
        askedPerFloor[room.floor] += room.areaMetres;
      }
    }

    for (const auto &floor : askedPerFloor)
    {
      widest = std::max(widest, floor.second);
    }

    const double aspect = programme.footprintZ > 1e-6
                            ? programme.footprintX / programme.footprintZ
                            : 1.;
    const double side = std::sqrt(std::max(widest, 9.));

    ground = rectangle(side * std::sqrt(aspect), side / std::sqrt(aspect));
  }

  if (polygonArea(ground) < 4.)
  {
    return fail("the footprint has no room in it");
  }

  for (const Point &p : ground.outer)
  {
    out.footprint.emplace_back(geom2::toMetres(p.x), geom2::toMetres(p.y));
  }

  geom2::Box box = geom2::bounds(ground.outer);
  const double spanX = geom2::toMetres(box.maxX - box.minX);
  const double spanZ = geom2::toMetres(box.maxY - box.minY);
  const double midX = geom2::toMetres((box.minX + box.maxX) / 2);
  const double midZ = geom2::toMetres((box.minY + box.maxY) / 2);

  // --- a floor at a time ---

  std::map<int32_t, std::vector<uint32_t>> byFloor;

  for (uint32_t r = 0; r < programme.rooms.size(); ++r)
  {
    // The yard is outside; it is a room of the programme and not of the
    // building, and partitioning the footprint among it would give a quarter
    // of the house to the garden.
    if (!enclosesFloorArea(programme.rooms[r].use))
    {
      continue;
    }

    byFloor[programme.rooms[r].floor].push_back(r);
  }

  // What each floor asks for, so a floor that wants a quarter of the building
  // is drawn at a quarter of it rather than stretched over the whole plan. A
  // belfry was coming out as a hundred and fifty square metres because it was
  // the only room on its floor and the floor was the whole footprint.
  std::map<int32_t, double> askedPerFloor;
  double widestFloor = 0;

  for (const auto &entry : byFloor)
  {
    double asked = 0;

    for (const uint32_t r : entry.second)
    {
      asked += programme.rooms[r].areaMetres;
    }

    askedPerFloor[entry.first] = asked;
    widestFloor = std::max(widestFloor, asked);
  }

  const double buildingArea = polygonArea(ground);
  (void)buildingArea;

  Rng rng(seed ^ 0x2545F4914F6CDD1Dull);

  // Which placed room each programme room became, for the doors afterwards.
  std::map<uint32_t, uint32_t> placedOf;

  for (const auto &entry : byFloor)
  {
    const int32_t level = entry.first;
    const std::vector<uint32_t> &rooms = entry.second;

    if (rooms.empty())
    {
      continue;
    }

    ++out.floorsPlanned;

    // The ground this floor stands on: the footprint, shrunk about its middle
    // when the floor asks for less than the widest one does. Never below a
    // third, because a storey that is a tenth of the one below it is a turret
    // and the programme would have said so.
    // A caller that has massed the building already says what ground this
    // floor stands on; otherwise the footprint is shrunk to what it asks for.
    Polygon floorGround = ground;

    if (regions != nullptr)
    {
      const auto given = regions->find(level);

      if (given != regions->end() && !given->second.empty())
      {
        floorGround = given->second;
      }
    }

    const double wants = askedPerFloor[level];
    const double share = widestFloor > 1e-6 ? wants / widestFloor : 1.;
    const double shrink = std::min(1., std::max(.34, std::sqrt(share)));

    const bool imposed = regions != nullptr && regions->count(level) > 0;

    if (shrink < .995 && !imposed)
    {
      geom2::Point centre{0, 0};

      if (geom2::centroid(ground, centre))
      {
        for (geom2::Point &point : floorGround.outer)
        {
          point.x = centre.x + int64_t(double(point.x - centre.x) * shrink);
          point.y = centre.y + int64_t(double(point.y - centre.y) * shrink);
        }

        for (Ring &hole : floorGround.holes)
        {
          for (geom2::Point &point : hole)
          {
            point.x = centre.x + int64_t(double(point.x - centre.x) * shrink);
            point.y = centre.y + int64_t(double(point.y - centre.y) * shrink);
          }
        }
      }
    }

    // --- seed the sites from the adjacency graph ---
    //
    // Linked rooms attract and everything repels, inside the footprint's box.
    // The result is not a plan, it is an arrangement: rooms that open into
    // each other end up near each other, which is what makes their cells touch
    // and therefore what makes the door deliverable.
    std::vector<double> x(rooms.size()), z(rooms.size());
    std::vector<std::vector<uint32_t>> neighbours(rooms.size());

    std::map<uint32_t, uint32_t> localOf;

    for (uint32_t i = 0; i < rooms.size(); ++i)
    {
      localOf[rooms[i]] = i;
    }

    for (const Adjacency &link : programme.links)
    {
      const auto a = localOf.find(link.a);
      const auto b = localOf.find(link.b);

      if (a != localOf.end() && b != localOf.end())
      {
        neighbours[a->second].push_back(b->second);
        neighbours[b->second].push_back(a->second);
      }
    }

    // Where the street is, as a point outside the building in that direction.
    const double streetLength =
      std::sqrt(settings.streetX * settings.streetX + settings.streetZ * settings.streetZ);
    const double streetUnitX = streetLength > 1e-9 ? settings.streetX / streetLength : 0.;
    const double streetUnitZ = streetLength > 1e-9 ? settings.streetZ / streetLength : -1.;
    const double frontX = midX + streetUnitX * spanX * .42;
    const double frontZ = midZ + streetUnitZ * spanZ * .42;

    for (uint32_t i = 0; i < rooms.size(); ++i)
    {
      const RoomSpec &room = programme.rooms[rooms[i]];

      if (room.frontsStreet)
      {
        // Started on the frontage, and held there below.
        x[i] = frontX + rng.around(spanX * .16);
        z[i] = frontZ + rng.around(spanZ * .16);

        continue;
      }

      const bool middle = circulation(room.use);
      x[i] = midX + rng.around(middle ? spanX * .08 : spanX * .35);
      z[i] = midZ + rng.around(middle ? spanZ * .08 : spanZ * .35);
    }

    const double spring = .02;
    const double repel = std::max(spanX, spanZ) * .25;

    for (uint32_t step = 0; step < settings.layoutIterations; ++step)
    {
      std::vector<double> dx(rooms.size(), 0), dz(rooms.size(), 0);

      for (uint32_t i = 0; i < rooms.size(); ++i)
      {
        for (const uint32_t j : neighbours[i])
        {
          dx[i] += (x[j] - x[i]) * spring;
          dz[i] += (z[j] - z[i]) * spring;
        }

        for (uint32_t j = 0; j < rooms.size(); ++j)
        {
          if (i == j)
          {
            continue;
          }

          const double ax = x[i] - x[j];
          const double az = z[i] - z[j];
          const double d2 = std::max(ax * ax + az * az, 1e-3);
          const double push = repel * repel / d2 * .002;

          dx[i] += ax * push;
          dz[i] += az * push;
        }
      }

      for (uint32_t i = 0; i < rooms.size(); ++i)
      {
        if (programme.rooms[rooms[i]].frontsStreet)
        {
          // Pulled back to the frontage every step, so the springs can slide
          // it along the street but never off it.
          dx[i] += (frontX - x[i]) * settings.streetPull;
          dz[i] += (frontZ - z[i]) * settings.streetPull;
        }

        x[i] = std::min(std::max(x[i] + dx[i], midX - spanX * .48), midX + spanX * .48);
        z[i] = std::min(std::max(z[i] + dz[i], midZ - spanZ * .48), midZ + spanZ * .48);
      }
    }

    // --- partition the floor, then drive the areas ---

    // A hall is not a compact room, it is a spine, and a power diagram's cells
    // are compact by construction - so one site cannot touch the six rooms
    // that open off a landing. Give circulation a site per few partners and
    // union their cells afterwards, and the diagram draws a corridor out of
    // pieces that are each compact.
    std::vector<uint32_t> roomOfSite;
    std::vector<geom2::Site> sites;

    for (uint32_t i = 0; i < rooms.size(); ++i)
    {
      const RoomUse use = programme.rooms[rooms[i]].use;
      const uint32_t wanted =
        circulation(use)
          ? std::min<uint32_t>(4, std::max<uint32_t>(1, uint32_t(neighbours[i].size() / 2)))
          : 1;

      // Strung along the line from the room toward the middle of its
      // neighbours, which is the direction a corridor would actually run.
      double towardX = 0, towardZ = 0;

      for (const uint32_t j : neighbours[i])
      {
        towardX += x[j] - x[i];
        towardZ += z[j] - z[i];
      }

      const double length = std::sqrt(towardX * towardX + towardZ * towardZ);
      const double stepX = length > 1e-6 ? towardX / length : 1.;
      const double stepZ = length > 1e-6 ? towardZ / length : 0.;
      const double reach = std::min(spanX, spanZ) * .18;

      for (uint32_t k = 0; k < wanted; ++k)
      {
        const double along =
          wanted == 1 ? 0. : (double(k) / double(wanted - 1) - .5) * 2. * reach;

        geom2::Site site;
        site.position = {geom2::toUnit(x[i] + stepX * along),
                         geom2::toUnit(z[i] + stepZ * along)};
        site.weight = 0;
        sites.push_back(site);
        roomOfSite.push_back(i);
      }
    }

    const geom2::PartitionOptions options;

    // Compactness first: a cluster the springs left behind comes out as a row
    // of slivers otherwise.
    std::string why;

    if (!geom2::relax(floorGround, sites, settings.relaxIterations, options, &why))
    {
      return fail("the floor could not be relaxed");
    }

    // The areas the programme asked for, scaled to the ground there actually
    // is. A programme routinely asks for more floor than its footprint holds -
    // it was sampled from a network, not measured off this plot - and honouring
    // the ratios is the most that can be done about it.
    double asked = 0;

    for (const uint32_t r : rooms)
    {
      asked += programme.rooms[r].areaMetres;
    }

    const double available = polygonArea(floorGround);
    const double scale = asked > 0 ? available / asked : 1.;

    std::vector<geom2::PartitionCell> cells;

    for (uint32_t round = 0; round < settings.areaIterations; ++round)
    {
      if (!geom2::partitionRegion(floorGround, sites, options, cells, &why))
      {
        return fail("the floor could not be partitioned");
      }

      std::vector<double> achieved(rooms.size(), 0);
      std::vector<uint32_t> drawn(rooms.size(), 0);

      for (const geom2::PartitionCell &cell : cells)
      {
        if (cell.site < roomOfSite.size())
        {
          achieved[roomOfSite[cell.site]] += polygonArea(cell.polygon);
          ++drawn[roomOfSite[cell.site]];
        }
      }

      // Feedback: a cell short of its target gets heavier, and the boundary
      // between two sites moves toward the lighter one. The gain is well
      // under one because the whole correction at once oscillates.
      // The correction is the room's, shared out over the sites that make it
      // up, so a spine of three gets a third of the push each.
      for (uint32_t siteIndex = 0; siteIndex < sites.size(); ++siteIndex)
      {
        const uint32_t i = roomOfSite[siteIndex];
        const RoomUse use = programme.rooms[rooms[i]].use;
        const double boost = circulation(use) ? settings.circulationBoost : 1.;
        const double target = programme.rooms[rooms[i]].areaMetres * scale * boost;
        const double share = std::max(1u, drawn[i]);

        sites[siteIndex].weight += (target - achieved[i]) * settings.areaGain / share;
      }
    }

    if (!geom2::partitionRegion(floorGround, sites, options, cells, &why))
    {
      return fail("the floor could not be partitioned");
    }

    std::vector<std::vector<Polygon>> pieces(rooms.size());

    for (const geom2::PartitionCell &cell : cells)
    {
      if (cell.site < roomOfSite.size())
      {
        pieces[roomOfSite[cell.site]].push_back(cell.polygon);
      }
    }

    for (uint32_t i = 0; i < rooms.size(); ++i)
    {
      if (pieces[i].empty())
      {
        ++out.roomsDropped;

        continue;
      }

      Polygon boundary = pieces[i].front();
      double area = 0;

      for (const Polygon &piece : pieces[i])
      {
        area += polygonArea(piece);
      }

      if (pieces[i].size() > 1)
      {
        // A spine: its pieces are neighbours in the diagram, so their union is
        // one region. If the union fails - it should not, they share whole
        // edges - keep the largest piece rather than losing the room.
        std::vector<geom2::Operand> operands;

        for (const Polygon &piece : pieces[i])
        {
          operands.push_back(geom2::toOperand(piece));
        }

        std::vector<Polygon> merged;

        if (geom2::unionOf(operands, merged, nullptr) && merged.size() == 1)
        {
          boundary = merged.front();
          area = polygonArea(boundary);
        }
        else
        {
          for (const Polygon &piece : pieces[i])
          {
            if (polygonArea(piece) > polygonArea(boundary))
            {
              boundary = piece;
            }
          }

          area = polygonArea(boundary);
        }
      }

      if (area < settings.minimumRoomMetres)
      {
        ++out.roomsDropped;

        continue;
      }

      PlacedRoom placed;
      placed.programmeRoom = rooms[i];
      placed.use = programme.rooms[placed.programmeRoom].use;
      placed.floor = level;
      placed.boundary = std::move(boundary);
      placed.askedMetres = programme.rooms[placed.programmeRoom].areaMetres * scale;
      placed.achievedMetres = area;

      placedOf[placed.programmeRoom] = uint32_t(out.rooms.size());
      out.rooms.push_back(std::move(placed));
      ++out.roomsPlaced;
    }
  }

  // --- the doors ---

  double totalError = 0;
  uint32_t measured = 0;

  for (const PlacedRoom &room : out.rooms)
  {
    if (room.askedMetres > 0)
    {
      totalError += std::abs(room.achievedMetres - room.askedMetres) / room.askedMetres;
      ++measured;
    }
  }

  out.areaError = measured > 0 ? totalError / double(measured) : 0;

  uint32_t wanted = 0;

  for (const Adjacency &link : programme.links)
  {
    const auto a = placedOf.find(link.a);
    const auto b = placedOf.find(link.b);

    if (a == placedOf.end() || b == placedOf.end())
    {
      // One of the rooms is not on the plan, so the door was never asked of
      // the geometry and must not count against it.
      continue;
    }

    // A link between two floors is made by the stair, not by a wall. Counting
    // it as a door the geometry failed to deliver is counting the stair as a
    // failure, and it was most of what the first measurement reported.
    const int32_t floorA = programme.rooms[link.a].floor;
    const int32_t floorB = programme.rooms[link.b].floor;

    if (floorA != floorB)
    {
      const bool byStair = programme.rooms[link.a].use == RoomUse::Staircase
                           || programme.rooms[link.b].use == RoomUse::Staircase;

      if (byStair)
      {
        // Delivered, and not by a shared wall - so it is not recorded as a
        // door with a position, because there is no wall to put one in.
        continue;
      }

      // Two rooms on different floors with no stair between them is a link the
      // programme should not have made; it counts against the plan.
      ++wanted;

      UndeliveredDoor crossed;
      crossed.programmeA = link.a;
      crossed.programmeB = link.b;
      crossed.useA = programme.rooms[link.a].use;
      crossed.useB = programme.rooms[link.b].use;
      crossed.floor = floorA;
      out.undelivered.push_back(crossed);

      continue;
    }

    ++wanted;

    double doorX = 0, doorZ = 0;

    if (shareAWall(out.rooms[a->second].boundary, out.rooms[b->second].boundary,
                   settings.wallThickness * 3., doorX, doorZ))
    {
      out.doors.push_back({a->second, b->second, link.door, doorX, doorZ});

      continue;
    }

    UndeliveredDoor missed;
    missed.programmeA = link.a;
    missed.programmeB = link.b;
    missed.useA = programme.rooms[link.a].use;
    missed.useB = programme.rooms[link.b].use;
    missed.floor = programme.rooms[link.a].floor;
    out.undelivered.push_back(missed);
  }

  out.doorsDelivered = wanted > 0 ? double(out.doors.size()) / double(wanted) : 1.;

  return out.roomsPlaced > 0;
}


// --- the programme as a building the rest of the engine understands --------

namespace {

std::vector<rewrite::Position> toPositions(const Ring &ring)
{
  std::vector<rewrite::Position> out;

  for (const Point &point : ring)
  {
    out.push_back({geom2::toMetres(point.x), 0., geom2::toMetres(point.y)});
  }

  return out;
}

Ring toRing(const std::vector<rewrite::Position> &points)
{
  Ring ring;

  for (const rewrite::Position &point : points)
  {
    ring.push_back({geom2::toUnit(point.x), geom2::toUnit(point.z)});
  }

  return ring;
}

// Whether a segment lies along the boundary of a region, which is what makes
// a wall an outside wall.
bool onBoundary(const Polygon &region, const double x0, const double z0,
                const double x1, const double z1)
{
  const double slack = 2e-3;

  const auto against = [&](const Ring &ring) {
    for (size_t i = 0; i < ring.size(); ++i)
    {
      const double ax = geom2::toMetres(ring[i].x);
      const double az = geom2::toMetres(ring[i].y);
      const double bx = geom2::toMetres(ring[(i + 1) % ring.size()].x);
      const double bz = geom2::toMetres(ring[(i + 1) % ring.size()].y);

      const double dx = bx - ax;
      const double dz = bz - az;
      const double length = std::sqrt(dx * dx + dz * dz);

      if (length < slack)
      {
        continue;
      }

      // Both ends of the wall on the boundary line, and within its span.
      const auto near = [&](const double px, const double pz) {
        const double cross = std::abs(dx * (pz - az) - dz * (px - ax)) / length;
        const double along = ((px - ax) * dx + (pz - az) * dz) / (length * length);

        return cross < slack && along > -.02 && along < 1.02;
      };

      if (near(x0, z0) && near(x1, z1))
      {
        return true;
      }
    }

    return false;
  };

  if (against(region.outer))
  {
    return true;
  }

  for (const Ring &hole : region.holes)
  {
    if (against(hole))
    {
      return true;
    }
  }

  return false;
}

}  // namespace


namespace {

// --- the lot's own frame --------------------------------------------------

// The direction a building on this lot should run.
//
// A rectilinear plan is rectilinear in some frame, and the world's axes are
// the wrong one: a town lot is a wedge off a bent street, and a house squared
// to north on it leaves a triangle of waste along every boundary and stands
// askew to its own frontage. Every real building on a street is parallel to it.
//
// So the frame is the lot's: of the directions its edges run, the one whose
// bounding box holds the lot most tightly. That is the minimum-area rectangle
// by rotating calipers - its side is always along an edge of the hull - and on
// a burgage strip it comes out along the strip, which is along the street.
double lotAxisRadians(const std::vector<rewrite::Position> &lot)
{
  double best = 0, bestArea = 0;
  bool found = false;

  for (size_t i = 0; i < lot.size(); ++i)
  {
    const rewrite::Position &a = lot[i];
    const rewrite::Position &b = lot[(i + 1) % lot.size()];
    const double length = std::hypot(b.x - a.x, b.z - a.z);

    if (length < .25)
    {
      continue;
    }

    const double angle = std::atan2(b.z - a.z, b.x - a.x);
    const double cosine = std::cos(-angle), sine = std::sin(-angle);

    double lowX = 0, highX = 0, lowZ = 0, highZ = 0;

    for (size_t k = 0; k < lot.size(); ++k)
    {
      const double x = lot[k].x * cosine - lot[k].z * sine;
      const double z = lot[k].x * sine + lot[k].z * cosine;

      lowX = k == 0 ? x : std::min(lowX, x);
      highX = k == 0 ? x : std::max(highX, x);
      lowZ = k == 0 ? z : std::min(lowZ, z);
      highZ = k == 0 ? z : std::max(highZ, z);
    }

    const double area = (highX - lowX) * (highZ - lowZ);

    if (!found || area < bestArea)
    {
      found = true;
      bestArea = area;
      best = angle;
    }
  }

  return best;
}

void turn(rewrite::Position &point, const double cosine, const double sine)
{
  const double x = point.x * cosine - point.z * sine;
  const double z = point.x * sine + point.z * cosine;
  point.x = x;
  point.z = z;
}

void turn(std::vector<rewrite::Position> &points, const double cosine, const double sine)
{
  for (rewrite::Position &point : points)
  {
    turn(point, cosine, sine);
  }
}

// Everything the planner produced, turned back into the world.
void turn(rewrite::DwellingPlan &plan, ProgrammeBuilding &building, const double radians)
{
  const double cosine = std::cos(radians), sine = std::sin(radians);

  for (rewrite::DwellingRoom &room : plan.rooms)
  {
    turn(room.boundary, cosine, sine);
  }

  for (rewrite::DwellingWall &wall : plan.walls)
  {
    turn(wall.a, cosine, sine);
    turn(wall.b, cosine, sine);
  }

  for (rewrite::DwellingDoor &door : plan.doors)
  {
    turn(door.center, cosine, sine);
  }

  for (rewrite::DwellingStair &stair : plan.stairs)
  {
    turn(stair.opening, cosine, sine);
    turn(stair.lowerLanding, cosine, sine);
    turn(stair.upperLanding, cosine, sine);
  }

  for (rewrite::DwellingWing &wing : plan.wings)
  {
    turn(wing.footprint, cosine, sine);
    turn(wing.ridgeFrom, cosine, sine);
    turn(wing.ridgeTo, cosine, sine);
  }

  for (rewrite::DwellingRoofFace &face : plan.roofs)
  {
    turn(face.polygon, cosine, sine);
  }

  for (rewrite::DwellingStorey &storey : plan.storeys)
  {
    turn(storey.outer, cosine, sine);

    for (auto &hole : storey.holes)
    {
      turn(hole, cosine, sine);
    }
  }

  for (rewrite::DwellingPillar &pillar : plan.pillars)
  {
    turn(pillar.footprint, cosine, sine);
  }

  for (auto &corner : building.footprint)
  {
    const double x = corner.first * cosine - corner.second * sine;
    const double z = corner.first * sine + corner.second * cosine;
    corner = {x, z};
  }

  for (PlacedRoom &room : building.rooms)
  {
    for (Point &point : room.boundary.outer)
    {
      const double x = geom2::toMetres(point.x), z = geom2::toMetres(point.y);
      point = {geom2::toUnit(x * cosine - z * sine), geom2::toUnit(x * sine + z * cosine)};
    }

    for (Ring &hole : room.boundary.holes)
    {
      for (Point &point : hole)
      {
        const double x = geom2::toMetres(point.x), z = geom2::toMetres(point.y);
        point = {geom2::toUnit(x * cosine - z * sine), geom2::toUnit(x * sine + z * cosine)};
      }
    }
  }

  for (PlacedDoor &door : building.doors)
  {
    const double x = door.x * cosine - door.z * sine;
    const double z = door.x * sine + door.z * cosine;
    door.x = x;
    door.z = z;
  }

  for (PlacedEntrance &entrance : building.entrances)
  {
    const double x = entrance.x * cosine - entrance.z * sine;
    const double z = entrance.x * sine + entrance.z * cosine;
    entrance.x = x;
    entrance.z = z;

    const double nx = entrance.normalX * cosine - entrance.normalZ * sine;
    const double nz = entrance.normalX * sine + entrance.normalZ * cosine;
    entrance.normalX = nx;
    entrance.normalZ = nz;
  }

  for (PlacedGround &ground : building.grounds)
  {
    for (Point &point : ground.boundary.outer)
    {
      const double x = geom2::toMetres(point.x), z = geom2::toMetres(point.y);
      point = {geom2::toUnit(x * cosine - z * sine), geom2::toUnit(x * sine + z * cosine)};
    }

    for (Ring &hole : ground.boundary.holes)
    {
      for (Point &point : hole)
      {
        const double x = geom2::toMetres(point.x), z = geom2::toMetres(point.y);
        point = {geom2::toUnit(x * cosine - z * sine), geom2::toUnit(x * sine + z * cosine)};
      }
    }
  }
}

}  // namespace

double ceilingMetres(const ProgrammePlanSettings &settings, const int32_t level,
                     const uint32_t storeys, const PlotStanding standing)
{
  // What the standing buys, in height. A grand house's ground floor is twelve
  // or thirteen feet; a cottage's is seven and a half, and the same seven and
  // a half upstairs.
  const double pretension = standing == PlotStanding::Slum      ? 0.
                            : standing == PlotStanding::Rural   ? .15
                            : standing == PlotStanding::Modest  ? .45
                            : standing == PlotStanding::Comfortable ? .75
                            : standing == PlotStanding::Wealthy ? 1.
                                                                : 1.25;

  const double base = settings.storeyMetres;

  if (level < 0)
  {
    // A basement is dug, not built, and nobody receives anyone in it.
    return base * .82;
  }

  if (level == 0 && storeys > 1)
  {
    return base * (1. + (settings.groundStoreyFactor - 1.) * pretension);
  }

  if (storeys >= 3 && uint32_t(level) + 1 == storeys)
  {
    // The top floor of a tall house is servants and children, in the roof.
    return base * (1. - (1. - settings.atticStoreyFactor) * pretension);
  }

  return base;
}

double floorElevation(const ProgrammePlanSettings &settings, const int32_t level,
                      const uint32_t storeys, const PlotStanding standing)
{
  double elevation = settings.foundationMetres;

  if (level < 0)
  {
    for (int32_t below = -1; below >= level; --below)
    {
      elevation -= ceilingMetres(settings, below, storeys, standing);
    }

    return elevation;
  }

  for (int32_t below = 0; below < level; ++below)
  {
    elevation += ceilingMetres(settings, below, storeys, standing);
  }

  return elevation;
}

bool planProgrammeDwelling(const ProgrammeBrief &brief,
                           const uint64_t seed,
                           const ProgrammePlanSettings &settings,
                           const ComplexDwellingSettings &massing,
                           const std::vector<rewrite::Position> &plot,
                           ProgrammeBuilding &out,
                           rewrite::DwellingPlan &plan,
                           std::string *error,
                           const FloorPlanSettings *optimise)
{
  const auto fail = [error](const char *why) {
    if (error != nullptr)
    {
      *error = why;
    }

    return false;
  };

  plan = rewrite::DwellingPlan{};

  // --- into the lot's own frame --------------------------------------------

  const double axis = plot.size() >= 3 ? lotAxisRadians(plot) : 0.;
  const double intoLot = std::cos(-axis), intoLotSine = std::sin(-axis);

  std::vector<rewrite::Position> squared = plot;
  turn(squared, intoLot, intoLotSine);

  ProgrammePlanSettings local = settings;

  {
    // The street is a direction, so it turns with everything else.
    const double streetX = settings.streetX * intoLot - settings.streetZ * intoLotSine;
    const double streetZ = settings.streetX * intoLotSine + settings.streetZ * intoLot;
    local.streetX = streetX;
    local.streetZ = streetZ;
  }

  // --- what the lot will allow ---------------------------------------------
  //
  // A lot the town allocated is a wedge off a bent street, a burgage strip
  // twelve metres deep, or the leftover triangle where three roads meet. The
  // programme has to be sampled to fit it and not the other way round - "the
  // ground is fixed and the network picks a programme that fits on it" - so
  // the brief carries the ground's own dimensions before anything is drawn.
  //
  // A brief that already names a footprint is left alone: the caller has said
  // what they want and it is not this function's place to overrule them.

  ProgrammeBrief fitted = brief;

  if (squared.size() >= 3 && (!fitted.footprintX || !fitted.footprintZ))
  {
    double lowX = squared.front().x, highX = squared.front().x;
    double lowZ = squared.front().z, highZ = squared.front().z;

    for (const rewrite::Position &point : squared)
    {
      lowX = std::min(lowX, point.x);
      highX = std::max(highX, point.x);
      lowZ = std::min(lowZ, point.z);
      highZ = std::max(highZ, point.z);
    }

    // The building covers a share of its lot, and the bounding box of a wedge
    // is not the wedge, so take the root of the ratio of areas rather than
    // the box itself: a triangular lot gives up about half its box.
    const double box = std::max((highX - lowX) * (highZ - lowZ), 1e-6);

    double twice = 0;

    for (size_t i = 0, j = squared.size() - 1; i < squared.size(); j = i++)
    {
      twice += squared[j].x * squared[i].z - squared[i].x * squared[j].z;
    }

    const double lot = std::max(std::abs(twice) * .5, 1e-6);
    const double share = std::sqrt(std::min(1., lot / box) * local.coverage);

    fitted.footprintX = std::max(3.5, (highX - lowX) * share);
    fitted.footprintZ = std::max(3.5, (highZ - lowZ) * share);

    // On a small lot in a town, the household builds upward.
    //
    // This has to be decided here and not by the rejection draw further down,
    // because the massing happens in between: the plates it lays down are what
    // the programme is then fitted to, so a two-storey programme drawn after
    // them has only one floor to stand on. The ground is the scarce thing and
    // the storey count is the answer to it, which is why a medieval town is
    // three storeys of timber over a shop and a village of the same population
    // is a field of single-storey cottages.
    const bool rural = fitted.building == BuildingClass::PeasantCottage
                       || fitted.building == BuildingClass::Farmstead;

    if (!fitted.storeys && !rural)
    {
      const double buildable = *fitted.footprintX * *fitted.footprintZ;

      if (buildable < 38.)
      {
        fitted.storeys = 3;
      }
      else if (buildable < 130.)
      {
        fitted.storeys = 2;
      }
    }
  }

  // --- a programme, to know how big the building has to be -----------------
  //
  // This first draw is provisional: it is here to say how many storeys the
  // building wants, which is what the massing needs before it can lay down any
  // ground. The programme that is actually built is drawn once the plates
  // exist - see below.

  Programme sketch;

  if (!sampleProgramme(fitted, seed, sketch))
  {
    return fail("the brief has no programme: the network gives it probability zero");
  }

  double widest = 0;
  std::map<int32_t, double> asked;

  for (const RoomSpec &room : sketch.rooms)
  {
    if (enclosesFloorArea(room.use))
    {
      asked[room.floor] += room.areaMetres;
      widest = std::max(widest, asked[room.floor]);
    }
  }

  // --- the ground --------------------------------------------------------

  std::vector<rewrite::Position> ground = squared;

  if (ground.size() < 3)
  {
    // A squared the composed building will cover about half of, shaped by the
    // programme's own footprint aspect.
    const double aspect =
      sketch.footprintZ > 1e-6 ? sketch.footprintX / sketch.footprintZ : 1.;
    const double area = std::max(widest, 12.) / .62;
    const double side = std::sqrt(area);
    const double halfX = side * std::sqrt(aspect) * .5;
    const double halfZ = side / std::sqrt(aspect) * .5;

    ground = {{-halfX, 0, -halfZ}, {halfX, 0, -halfZ}, {halfX, 0, halfZ}, {-halfX, 0, halfZ}};
  }

  // --- mass it -----------------------------------------------------------
  //
  // Rectangular wings, pushed off the sides of those already standing, each
  // with its own roof and grouped into clusters whose roofs merge. That is
  // Watabou's rule for a mansion and it is already written here.

  MassingBrief massingBrief;
  massingBrief.plot = ground;
  massingBrief.seed = seed ^ 0x9E3779B97F4A7C15ull;
  massingBrief.coverage = local.coverage;
  massingBrief.foundation = local.foundationMetres;
  // The massing wants one rise. Give it the average of the real ones, so the
  // eaves and the roof land at the height the storeys actually add up to.
  {
    double total = 0;

    for (uint32_t level = 0; level < std::max(1u, sketch.storeys); ++level)
    {
      total += ceilingMetres(local, int32_t(level), std::max(1u, sketch.storeys),
                             brief.standing);
    }

    massingBrief.floorRise = total / double(std::max(1u, sketch.storeys));
  }
  // Every part of it as tall as the programme asked for.
  //
  // massBuilding drops storeys off narrow parts, which is right for a mansion
  // whose wings are lower than its body and wrong for a town: a burgage plot
  // house is tall *because* it is narrow, having nowhere else to go, and the
  // width rule turns every one of them into a bungalow. The programme already
  // said how many floors the household needs, and that is the answer.
  massingBrief.minimumStoreys = std::max(1u, sketch.storeys);
  massingBrief.maximumStoreys = std::max(1u, sketch.storeys);

  // A jetty, on anything with a floor above the ground.
  //
  // Timber framing carries the upper floor's joists out past the wall below,
  // so a town house is wider at the top than at the bottom and leans over its
  // street. Without it every storey plate has the outline of the one under it
  // and the building is a prism however good its plan.
  massingBrief.jettyMetres = 0.;

  // The street is where the programme says it is, so the wing that carries the
  // shop lands on it.
  massingBrief.anchored = true;
  massingBrief.anchorX = local.streetX * 1e3;
  massingBrief.anchorZ = local.streetZ * 1e3;

  std::vector<rewrite::DwellingWing> wings;
  std::vector<rewrite::DwellingRoofFace> roofs;
  std::vector<rewrite::DwellingStorey> storeys;
  std::vector<rewrite::DwellingPillar> pillars;

  massComposedBuilding(massingBrief, massing, wings, roofs, &storeys, &pillars);

  if (wings.empty() || storeys.empty())
  {
    return fail("the building could not be massed on that ground");
  }

  // --- the region each floor stands on ------------------------------------

  ProgrammeFloorRegions regions;

  for (const rewrite::DwellingStorey &storey : storeys)
  {
    Polygon region;
    region.outer = toRing(storey.outer);

    for (const auto &hole : storey.holes)
    {
      region.holes.push_back(toRing(hole));
    }

    if (region.outer.size() >= 3)
    {
      regions[int32_t(storey.level)] = region;
    }
  }

  // A basement stands under the ground floor.
  if (regions.count(0) > 0 && sketch.basement)
  {
    regions[-1] = regions[0];
  }

  // --- the programme, drawn again against the ground that actually exists --
  //
  // Clamping the footprint conditions the total-area variable, and the room
  // areas are drawn from their own distributions given it - so the sum of the
  // rooms is only approximately the total, and on a small lot it comes out at
  // twice the ground there is. Asking for eleven rooms on forty square metres
  // is not a geometry problem and no optimiser can answer it.
  //
  // And the ground is not the lot. A wedge off a bent street carries a much
  // smaller rectangle than its area suggests, and the massing is where that
  // is discovered: the plates it lays down are the real answer to how much
  // floor there is. So the programme is conditioned on them and not on the
  // lot, which means drawing it after the massing rather than before.
  //
  // The draw is by rejection - crude, but it is conditioning and not a fudge.
  // What comes out is a draw from the network given "and it fits the ground",
  // which is exactly the thing being asked for. A dozen tries is enough;
  // beyond that the brief is asking for a manor on a market stall, and the
  // closest fit is the honest answer.

  double standing = 0;

  for (const auto &entry : regions)
  {
    standing += std::abs(polygonArea(entry.second));
  }

  if (standing > 4.)
  {
    const Ring &plate = regions.count(0) ? regions[0].outer : regions.begin()->second.outer;

    double lowX = geom2::toMetres(plate.front().x), highX = lowX;
    double lowZ = geom2::toMetres(plate.front().y), highZ = lowZ;

    for (const Point &point : plate)
    {
      lowX = std::min(lowX, geom2::toMetres(point.x));
      highX = std::max(highX, geom2::toMetres(point.x));
      lowZ = std::min(lowZ, geom2::toMetres(point.y));
      highZ = std::max(highZ, geom2::toMetres(point.y));
    }

    // The plate's own shape, at the plate's own area: an L covers about two
    // thirds of its box, and a programme told the box asks for half as much
    // house again as the building can hold.
    const double ground = std::abs(polygonArea(regions.count(0) ? regions[0]
                                                               : regions.begin()->second));
    const double box = std::max((highX - lowX) * (highZ - lowZ), 1e-6);
    const double shrink = std::sqrt(std::min(1., ground / box));

    fitted.footprintX = std::max(3., (highX - lowX) * shrink);
    fitted.footprintZ = std::max(3., (highZ - lowZ) * shrink);

    const auto asked = [](const Programme &programme) {
      double total = 0;

      for (const RoomSpec &room : programme.rooms)
      {
        if (enclosesFloorArea(room.use))
        {
          total += room.areaMetres;
        }
      }

      return total;
    };

    // Overshooting is much worse than undershooting: a room that will not fit
    // is a room the plan has to lose, and a plan with room to spare is only a
    // house with a wide hall.
    //
    // And on a small lot, a storey is worth more than a wider footprint. That
    // is the whole reason a town is built the way it is: ground on a street is
    // the scarce thing, so a household that needs more room puts it over what
    // it already has. A village has no such pressure - land is the one thing
    // there is - so the same household there builds a long low cottage and
    // this bonus is not applied to it.
    const bool crowded = *fitted.footprintX * *fitted.footprintZ < 130. && brief.building != BuildingClass::PeasantCottage
                         && brief.building != BuildingClass::Farmstead;

    const auto miss = [&](const Programme &programme) {
      const double want = asked(programme);
      const double error = want > standing ? 3. * (want - standing) : standing - want;
      const double upward =
        crowded ? std::min(3u, programme.storeys) * .16 * std::max(standing, 1.) : 0.;

      return error - upward;
    };

    Programme best = sketch;
    double bestMiss = miss(sketch);

    for (uint32_t draw = 0; draw < 16 && bestMiss > standing * .12; ++draw)
    {
      Programme candidate;

      if (!sampleProgramme(fitted, seed ^ (0x9E3779B97F4A7C15ull * (draw + 1)), candidate))
      {
        continue;
      }

      // A programme that wants storeys the building does not have is not a
      // fit however well its areas add up: the rooms would stand on a floor
      // that was never built.
      candidate.storeys = std::min(candidate.storeys, uint32_t(regions.size()));

      const double score = miss(candidate);

      if (score < bestMiss)
      {
        bestMiss = score;
        best = std::move(candidate);
      }
    }

    sketch = std::move(best);
  }

  // --- place the rooms inside it ------------------------------------------

  std::vector<std::pair<double, double>> outline;

  for (const Point &point : regions.count(0) ? regions[0].outer : Ring{})
  {
    outline.emplace_back(geom2::toMetres(point.x), geom2::toMetres(point.y));
  }

  if (optimise != nullptr)
  {
    // §5, on the ground the massing laid down. Every storey plate is a lot,
    // not just the ground floor's: a wing that stops at one storey has no
    // ground on it for a bedroom, and Cf's "stands over the floor below" does
    // not know that, because the floor below is there and the roof is what is
    // missing.
    std::map<int32_t, std::vector<std::pair<double, double>>> lots;

    for (const auto &entry : regions)
    {
      std::vector<std::pair<double, double>> ring;

      for (const Point &point : entry.second.outer)
      {
        ring.emplace_back(geom2::toMetres(point.x), geom2::toMetres(point.y));
      }

      if (ring.size() >= 3)
      {
        lots[entry.first] = std::move(ring);
      }
    }

    // The programme's street direction is the optimiser's too: a shop with its
    // window on the yard is a store room.
    FloorPlanSettings placement = *optimise;
    placement.streetX = local.streetX;
    placement.streetZ = local.streetZ;

    FloorPlan optimised;

    std::vector<std::pair<double, double>> site;

    for (const rewrite::Position &point : squared)
    {
      site.emplace_back(point.x, point.z);
    }

    if (!optimiseFloorPlan(sketch, lots, seed, placement, optimised, error, site))
    {
      return false;
    }

    // --- and the building is re-massed onto the plan it just made ---------
    //
    // §6 builds the exterior from the floor plan: "the walls of the building
    // are generated from the floor plan, and the roof from its outline". The
    // massing that came before was scaffolding - it existed to say how much
    // ground there is and how tall the building may be - and keeping its
    // shell would leave a building whose walls stand where no room reaches.
    //
    // Re-massing on the plan's own ground floor fixes that by construction,
    // and it cannot leave the lot, because the plan could not.
    if (!optimised.outlines.empty())
    {
      const auto ground = std::min_element(
        optimised.outlines.begin(), optimised.outlines.end(),
        [](const auto &a, const auto &b) { return a.first < b.first; });

      std::vector<rewrite::Position> shell;

      for (const auto &corner : ground->second)
      {
        shell.push_back({corner.first, 0, corner.second});
      }

      uint32_t levels = 0;

      for (const auto &floor : optimised.outlines)
      {
        if (floor.first >= 0)
        {
          levels = std::max(levels, uint32_t(floor.first + 1));
        }
      }

      if (shell.size() >= 4 && levels >= 1)
      {
        std::vector<rewrite::DwellingWing> remassedWings;
        std::vector<rewrite::DwellingRoofFace> remassedRoofs;
        std::vector<rewrite::DwellingStorey> remassedStoreys;
        std::vector<rewrite::DwellingPillar> remassedPillars;

        massBuilding(shell, local.foundationMetres, massingBrief.floorRise, levels, massing,
                     remassedWings, remassedRoofs, &remassedStoreys, &remassedPillars,
                     massingBrief.jettyMetres, 0, levels);

        if (!remassedWings.empty() && !remassedStoreys.empty())
        {
          wings = std::move(remassedWings);
          roofs = std::move(remassedRoofs);
          pillars = std::move(remassedPillars);
        }

        {
          // The storey plates are the plan's floors themselves and not the
          // massing's reading of them. massBuilding cuts an outline into
          // wings and insets what it cuts, which is right when the outline is
          // a squared and wrong when it is a floor plan: the walls it would
          // stand up are not the walls the rooms are bounded by, and the two
          // being a hand's breadth apart is what puts a bedroom half outside
          // its own building.
          storeys.clear();

          for (const auto &floor : optimised.outlines)
          {
            // The cellar is a storey too.
            //
            // It was skipped, because DwellingStorey numbers its levels from
            // zero and has no way to say "below the ground". What that left
            // was a cellar with partitions and no shell: the interior walls
            // were drawn, the exterior was not, and a manor house came out
            // with a row of triangular fins hanging under it. Its level is
            // recorded as zero and everything downstream orders storeys by
            // their height instead, which is the honest key anyway.
            if (floor.second.size() < 4)
            {
              continue;
            }

            rewrite::DwellingStorey storey;
            storey.level = uint32_t(std::max(floor.first, 0));
            storey.base = floorElevation(local, floor.first, sketch.storeys, brief.standing);
            storey.top =
              storey.base + ceilingMetres(local, floor.first, sketch.storeys, brief.standing);

            // The plate is the plan's floor and nothing else.
            //
            // It used to be jettied - pushed out a hand's breadth per storey,
            // which is what a timber-framed town house does - and the cost of
            // that was paid everywhere: the walls stood a foot outside the
            // rooms they enclose, the roof covered ground the floor did not
            // have, and on a small upper floor the plate came out half as big
            // again as the rooms in it. A jetty is worth having on the street
            // front of a town house and nowhere else, and until it is drawn
            // that way it is a defect wearing a period costume.
            for (const auto &corner : floor.second)
            {
              storey.outer.push_back({geom2::toMetres(geom2::toUnit(corner.first)), storey.base, geom2::toMetres(geom2::toUnit(corner.second))});
            }

            // The courts it wraps around, so they get walls and no roof.
            for (const auto &court : optimised.courts)
            {
              if (court.first != floor.first || court.second.size() < 3)
              {
                continue;
              }

              std::vector<rewrite::Position> ring;

              for (const auto &corner : court.second)
              {
                ring.push_back({geom2::toMetres(geom2::toUnit(corner.first)), storey.base, geom2::toMetres(geom2::toUnit(corner.second))});
              }

              if (geom2::contains(toRing(storey.outer), toRing(ring).front()))
              {
                storey.holes.push_back(std::move(ring));
              }
            }

            storeys.push_back(std::move(storey));
          }

          if (storeys.empty())
          {
            storeys = std::move(remassedStoreys);
          }

          // --- and a roof over every part of it ---------------------------
          //
          // The massing's roof goes first. It was built over the whole
          // building at its full height, with an overhang, before the storeys
          // were replaced by the plan's own floors - so keeping it alongside
          // the per-storey roofs below put two roofs on every building, one
          // of them passing through the walls and floors of the other. That is
          // most of what "broken models" looked like.
          roofs.clear();

          //
          // The massing roofs the top of the building. That is right for a
          // prism and wrong for anything with a wing: the parts of a storey
          // that have nothing standing on them are open to the sky, and a
          // mansion whose service range is a storey lower than its body was
          // coming out with no roof over the service range at all.
          //
          // So each storey is roofed over whatever the storey above does not
          // cover, by straight skeleton, at that storey's own top. It is the
          // lower envelope of the building read one level at a time, which is
          // what a roof over a building of several heights is.
          for (size_t st = 0; st < storeys.size(); ++st)
          {
            const rewrite::DwellingStorey &storey = storeys[st];

            Polygon plate;
            plate.outer = toRing(storey.outer);

            for (const auto &hole : storey.holes)
            {
              Ring inner = toRing(hole);

              if (inner.size() >= 3)
              {
                plate.holes.push_back(std::move(inner));
              }
            }

            if (plate.outer.size() < 3)
            {
              continue;
            }

            // What stands on this storey: the next one up by height. Levels
            // cannot say it now that a cellar is a storey at level zero, and
            // height is what the question is really about.
            const rewrite::DwellingStorey *over = nullptr;

            for (const rewrite::DwellingStorey &other : storeys)
            {
              if (other.base > storey.base + .5
                  && (over == nullptr || other.base < over->base))
              {
                over = &other;
              }
            }

            std::vector<geom2::Operand> above;

            // Any higher plate can cover this level, including an overhanging
            // wing several levels up. Preserve courtyards in the subtraction.
            for (const rewrite::DwellingStorey &other : storeys)
            {
              if (other.base <= storey.base + .5 || other.outer.size() < 3)
              {
                continue;
              }

              Polygon cover;
              cover.outer = toRing(other.outer);
              for (const auto &hole : other.holes)
              {
                cover.holes.push_back(toRing(hole));
              }
              above.push_back(geom2::toOperand(cover));
            }

            std::vector<Polygon> open;

            if (above.empty())
            {
              open = {plate};
            }
            else if (!geom2::differenceOf(geom2::toOperand(plate), above, open, nullptr))
            {
              return fail("could not subtract higher storeys from the roof plate");
            }

            // Preserve the boolean's shared boundaries. Simplifying the two
            // sides independently moves roof edges away from their walls.
            // A roof spans a range, not a building.
            //
            // The skeleton will happily pitch one hipped roof over five hundred
            // square metres, and what comes back is a tent: one ridge, a very
            // high one, and four enormous planes. No building of that size was
            // ever roofed that way, because nothing spans it - a range is one
            // or two rooms deep, its rafters reach from wall to ridge, and a
            // large house is several ranges with valleys between them. That is
            // the same rule the massing already uses for wings, applied to the
            // roof of whatever the plan actually left standing.
            //
            // So a piece deeper than a range is cut into strips across its
            // short axis and each strip is roofed on its own. The strips fall
            // to their shared edges, which is what makes a valley there rather
            // than one ridge over everything.
            std::vector<Polygon> ranges;

            for (const Polygon &rough : open)
            {
              const Polygon &cleaned = rough;

              if (cleaned.outer.size() < 3)
              {
                continue;
              }

              double lowX = geom2::toMetres(cleaned.outer.front().x), highX = lowX;
              double lowZ = geom2::toMetres(cleaned.outer.front().y), highZ = lowZ;

              for (const Point &at : cleaned.outer)
              {
                lowX = std::min(lowX, geom2::toMetres(at.x));
                highX = std::max(highX, geom2::toMetres(at.x));
                lowZ = std::min(lowZ, geom2::toMetres(at.y));
                highZ = std::max(highZ, geom2::toMetres(at.y));
              }

              const double spanX = highX - lowX, spanZ = highZ - lowZ;
              const double deep = std::min(spanX, spanZ);
              // What a roof of the period spans, wall to ridge to wall. Two
              // rooms and a passage between them is about nine metres; beyond
              // that the rafters cannot reach and the building is built as
              // more than one range.
              const double range = std::max(6., massing.maximumWingDepthMetres * .9);

              if (deep <= range)
              {
                ranges.push_back(cleaned);

                continue;
              }

              const bool acrossZ = spanZ <= spanX;
              const double across = acrossZ ? spanZ : spanX;
              const uint32_t strips = uint32_t(std::ceil(across / range));
              const double depth = across / double(strips);

              for (uint32_t strip = 0; strip < strips; ++strip)
              {
                const double from = (acrossZ ? lowZ : lowX) + depth * double(strip);
                const double to = from + depth;

                Polygon band;
                band.outer = acrossZ
                               ? toRing({{lowX - 1, 0, from},
                                         {highX + 1, 0, from},
                                         {highX + 1, 0, to},
                                         {lowX - 1, 0, to}})
                               : toRing({{from, 0, lowZ - 1},
                                         {to, 0, lowZ - 1},
                                         {to, 0, highZ + 1},
                                         {from, 0, highZ + 1}});

                std::vector<Polygon> cut;

                if (!geom2::intersectionOf({geom2::toOperand(cleaned), geom2::toOperand(band)},
                                           cut, nullptr))
                {
                  if (std::getenv("ROOF_DEBUG") != nullptr)
                  {
                    std::fprintf(stderr, "  range cut failed on %.1f m2, %.1f x %.1f\n",
                                 std::abs(polygonArea(cleaned)), spanX, spanZ);
                  }

                  return fail("could not partition the roof plate into ranges");
                }

                for (const Polygon &part : cut)
                {
                  // However small. A threshold here is a hole by another name:
                  // the ground floor of a house whose first floor oversails it
                  // by half a metre leaves a strip of exactly that size, and
                  // dropping it leaves the strip open to the sky.
                  if (std::abs(polygonArea(part)) > 1e-4)
                  {
                    ranges.push_back(part);
                  }
                }
              }
            }

            if (std::getenv("ROOF_DEBUG") != nullptr)
            {
              double want = 0, got = 0;

              for (const Polygon &rough : open)
              {
                want += std::abs(polygonArea(rough));
              }

              for (const Polygon &piece : ranges)
              {
                got += std::abs(polygonArea(piece));
              }

              if (std::abs(want - got) > .05)
              {
                std::fprintf(stderr, "  ranges cover %.2f m2 of %.2f m2 to roof\n", got, want);
              }
            }

            // The pitch for this storey, from the deepest point on it.
            //
            // One pitch for the whole storey is what makes its ranges meet:
            // pitching each piece to a fixed ridge *height* gives a narrow
            // range a steep roof and the wide one beside it a shallow one, and
            // where they meet the two surfaces are at different heights - a
            // vertical slot, which no measurement of the plan can see.
            //
            // The depth has to be measured, not estimated. Half the short side
            // of a piece's bounding box is right for a rectangle and wildly
            // wrong for the ring left when a storey is subtracted from the one
            // below it. What is wanted is the distance from the furthest point
            // inside the piece to its nearest wall, because that is where the
            // roof peaks and a peak is the pitch times that distance.
            //
            // Asking the skeleton for it at a slope of one was exact and could
            // fail, and when it failed it failed quietly: `deepest` stayed at
            // zero, the cap was skipped, and a peasant cottage got a lean-to
            // that climbed six metres from one eave to the far wall and stood
            // three metres proud of the storey it was leaning on. Sampling the
            // distance to the boundary is approximate and cannot fail, which
            // is the property that matters for a number whose whole job is to
            // bound something.
            // A wall the roof rises *to* is not a wall it falls from, so it
            // cannot be what bounds the depth. The edges a piece shares with
            // the storey above are gabled a few lines below - the lean-to
            // climbs to meet the taller mass - and measuring the depth to them
            // anyway is what let a peasant cottage's ground-floor roof rise six
            // metres across the ring around its upper floor and stand three
            // metres proud of the storey it was leaning on. The distance that
            // matters is to the nearest eave.
            const auto risesTo = [&](const double x, const double z) {
              for (const geom2::Operand &cover : above)
              {
                for (const Ring &boundary : cover.rings)
                {
                  for (size_t w = 0; w < boundary.size(); ++w)
                  {
                    const Point &a = boundary[w], &b = boundary[(w + 1) % boundary.size()];
                    const double ax = geom2::toMetres(a.x), az = geom2::toMetres(a.y);
                    const double ex = geom2::toMetres(b.x - a.x), ez = geom2::toMetres(b.y - a.y);
                    const double span = ex * ex + ez * ez;
                    const double along = span == 0 ? 0 : std::clamp(((x - ax) * ex + (z - az) * ez) / span, 0., 1.);
                    if (std::hypot(x - ax - ex * along, z - az - ez * along) <= 2e-6)
                    {
                      return true;
                    }
                  }
                }
              }
              return false;
            };

            const auto isAbutment = [&](double ax, double az, double bx, double bz) {
              return risesTo(ax, az) && risesTo(bx, bz) && risesTo((ax + bx) * .5, (az + bz) * .5);
            };

            const auto deepestIn = [&](const Polygon &piece) {
              if (piece.outer.size() < 3)
              {
                return 0.;
              }

              double lowX = geom2::toMetres(piece.outer.front().x), highX = lowX;
              double lowZ = geom2::toMetres(piece.outer.front().y), highZ = lowZ;

              for (const Point &corner : piece.outer)
              {
                lowX = std::min(lowX, geom2::toMetres(corner.x));
                highX = std::max(highX, geom2::toMetres(corner.x));
                lowZ = std::min(lowZ, geom2::toMetres(corner.y));
                highZ = std::max(highZ, geom2::toMetres(corner.y));
              }

              double deepest = 0;

              const uint32_t steps = 48;

              for (uint32_t j = 0; j < steps; ++j)
              {
                const double z = lowZ + (highZ - lowZ) * (double(j) + .5) / double(steps);

                for (uint32_t i = 0; i < steps; ++i)
                {
                  const double x = lowX + (highX - lowX) * (double(i) + .5) / double(steps);

                  if (!geom2::contains(piece, Point{geom2::toUnit(x), geom2::toUnit(z)}))
                  {
                    continue;
                  }

                  double reach = 1e30;

                  const auto against = [&](const Ring &ring) {
                    for (size_t w = 0; w < ring.size(); ++w)
                    {
                      const double ax = geom2::toMetres(ring[w].x);
                      const double az = geom2::toMetres(ring[w].y);
                      const double bx = geom2::toMetres(ring[(w + 1) % ring.size()].x);
                      const double bz = geom2::toMetres(ring[(w + 1) % ring.size()].y);

                      if (isAbutment(ax, az, bx, bz))
                      {
                        continue;
                      }

                      const double ex = bx - ax, ez = bz - az;
                      const double span = ex * ex + ez * ez;
                      const double along =
                        span < 1e-12
                          ? 0.
                          : std::min(1., std::max(0., ((x - ax) * ex + (z - az) * ez) / span));

                      reach = std::min(reach,
                                       std::hypot(x - (ax + ex * along), z - (az + ez * along)));
                    }
                  };

                  against(piece.outer);

                  for (const Ring &hole : piece.holes)
                  {
                    against(hole);
                  }

                  // Nothing to fall to, so the depth is the whole piece.
                  //
                  // Read as zero, this was the thing that let a lean-to shoot
                  // up. A thin ribbon of ground left over when a storey is
                  // taken out of the one below has its long walls against that
                  // storey - all gabled, all rising to it - and only its two
                  // far ends to fall to. Every sample in the middle of it then
                  // found no wall to measure from, contributed nothing, and the
                  // pitch came out uncapped at its steepest: nine metres of
                  // ridge over thirteen square metres of plan.
                  //
                  // What the sample is worth when nothing bounds it is the
                  // distance to the furthest thing that does, and half the
                  // piece's longest span is that, never smaller than the truth.
                  deepest = std::max(deepest,
                                     reach < 1e29
                                       ? reach
                                       : std::max(highX - lowX, highZ - lowZ) * .5);
                }
              }

              return deepest;
            };

            double deepest = 0;

            for (const Polygon &piece : ranges)
            {
              deepest = std::max(deepest, deepestIn(piece));
            }

            double pitch = .55;

            if (over != nullptr && deepest > .2)
            {
              // Two thirds of the way up the wall it leans on, which is where
              // a lean-to reaches.
              pitch = std::min(.55, (over->top - over->base) * .66 / deepest);
            }
            else if (deepest > .2)
            {
              // Nothing above to lean on, so the only thing bounding the ridge
              // is the pitch itself - but a roof that rises higher than the
              // storey it covers is a spire, not a roof, and the skeleton will
              // happily build one over a deep plan.
              pitch = std::min(.55, (storey.top - storey.base) * 1.35 / deepest);
            }

            if (std::getenv("ROOF_DEBUG") != nullptr)
            {
              double shared = 0;

              for (size_t a = 0; a < ranges.size(); ++a)
              {
                for (size_t b = a + 1; b < ranges.size(); ++b)
                {
                  std::vector<Polygon> both;

                  if (geom2::intersectionOf(
                        {geom2::toOperand(ranges[a]), geom2::toOperand(ranges[b])}, both,
                        nullptr))
                  {
                    for (const Polygon &part : both)
                    {
                      shared += std::abs(polygonArea(part));
                    }
                  }
                }
              }

              if (shared > 1e-3)
              {
                std::fprintf(stderr, "  RANGES OVERLAP by %.2f m2\n", shared);
              }

              std::fprintf(stderr,
                           "  storey base %.2f top %.2f: %zu ranges, deepest %.2f, pitch %.3f,"
                           " over %s\n",
                           storey.base, storey.top, ranges.size(), deepest, pitch,
                           over != nullptr ? "yes" : "no");
            }

            for (const Polygon &piece : ranges)
            {

              // No threshold. Anything the difference left is ground with
              // nothing over it, and the pieces it leaves are exactly the
              // awkward slivers along a wall - half a metre wide where a floor
              // oversails the one below. Skipping those was the last source of
              // holes in the roof.
              if (piece.outer.size() < 3 || std::abs(polygonArea(piece)) < 1e-4)
              {
                continue;
              }

              // Outer ring and holes both.
              //
              // Where the storey above stands in the middle of the one below,
              // the ground left to roof is an annulus - the lower plate with
              // the upper one punched out of it - and roofing its outer ring
              // alone puts a roof straight through the storey above. The
              // skeleton takes a polygon for exactly this reason: "a building
              // wrapped round a courtyard is an annulus, and the range between
              // its outer wall and the court is roofed by those two walls
              // meeting".
              std::vector<std::pair<double, double>> ring;

              for (const Point &point : piece.outer)
              {
                ring.emplace_back(geom2::toMetres(point.x), geom2::toMetres(point.y));
              }

              geom2::RoofOptions roofing;
              roofing.slope = .55;

              // One pitch for the whole storey, never a fixed ridge height.
              //
              // Setting the peak per piece is what cracked the temple open.
              // `ridgeHeight` makes the skeleton choose a pitch so *that*
              // piece's highest point lands there - so a narrow range gets a
              // steep roof and the wide one beside it a shallow one, and where
              // the two meet their surfaces are at different heights. The gap
              // between them is vertical, so no amount of measuring the plan
              // will find it; it shows as a slot straight through the roof.
              //
              // The pitch is a property of the building, not of a piece of it.
              // It is chosen once per storey - shallow enough that the deepest
              // range still clears whatever stands on it - and every range of
              // that storey is laid at it, which is what makes them meet.
              roofing.slope = pitch;
              // The edges shared with the storey above rise to it rather than
              // fall from it. A lower wing against a taller mass is a lean-to,
              // and roofed as though it stood alone its ridge runs up the
              // middle and straight through the wall of the part above -
              // which is what the first renders showed.
              std::vector<std::pair<double, double>> walk = ring;

              for (const Ring &hole : piece.holes)
              {
                for (const Point &point : hole)
                {
                  walk.emplace_back(geom2::toMetres(point.x), geom2::toMetres(point.y));
                }
              }

              if (!above.empty())
              {
                const auto markGables = [&](const Ring &boundary) {
                  for (size_t e = 0; e < boundary.size(); ++e)
                  {
                    const Point &from = boundary[e];
                    const Point &to = boundary[(e + 1) % boundary.size()];
                    if (std::hypot(geom2::toMetres(to.x - from.x), geom2::toMetres(to.y - from.y)) < 1e-6)
                    {
                      continue;
                    }
                    roofing.gables.push_back(isAbutment(geom2::toMetres(from.x), geom2::toMetres(from.y), geom2::toMetres(to.x), geom2::toMetres(to.y)));
                  }
                };
                markGables(piece.outer);
                for (const Ring &hole : piece.holes)
                {
                  markGables(hole);
                }
              }

              std::vector<geom2::RoofTriangle> surface;

              std::string roofWhy;

              if (!geom2::roofFromSkeleton(piece, roofing, surface, &roofWhy))
              {
                return fail(("could not construct roof: " + roofWhy).c_str());
              }

              if (std::getenv("ROOF_DEBUG") != nullptr)
              {
                double pitched = 0;

                for (const geom2::RoofTriangle &face : surface)
                {
                  pitched += face.gable
                               ? 0.
                               : std::abs((face.bx - face.ax) * (face.cz - face.az)
                                          - (face.cx - face.ax) * (face.bz - face.az))
                                   * .5;
                }

                const double want = std::abs(polygonArea(piece));

                if (pitched < want * .995)
                {
                  std::fprintf(stderr,
                               "  piece %.2f m2, %zu corners, %zu holes, ridge %.2f,"
                               " %zu gables -> %.2f m2 of roof\n",
                               want, piece.outer.size(), piece.holes.size(),
                               roofing.ridgeHeight, roofing.gables.size(), pitched);
                }
              }

              if (std::getenv("ROOF_VERIFY") != nullptr)
              {
                // Is this piece's surface single-valued? Sample it, and where
                // two faces disagree about the height print the ring, so the
                // shape can be added to the skeleton's own suite by name.
                double lowX = 1e30, highX = -1e30, lowZ = 1e30, highZ = -1e30;

                for (const auto &corner : ring)
                {
                  lowX = std::min(lowX, corner.first);
                  highX = std::max(highX, corner.first);
                  lowZ = std::min(lowZ, corner.second);
                  highZ = std::max(highZ, corner.second);
                }

                uint32_t clash = 0;

                for (uint32_t j = 0; j < 60; ++j)
                {
                  const double z = lowZ + (highZ - lowZ) * (double(j) + .5) / 60.;

                  for (uint32_t i = 0; i < 60; ++i)
                  {
                    const double x = lowX + (highX - lowX) * (double(i) + .5) / 60.;

                    if (!geom2::contains(piece, Point{geom2::toUnit(x), geom2::toUnit(z)}))
                    {
                      continue;
                    }

                    double least = 1e30, most = -1e30;

                    for (const geom2::RoofTriangle &face : surface)
                    {
                      if (face.gable)
                      {
                        continue;
                      }

                      const double d1 = (x - face.bx) * (face.az - face.bz)
                                        - (face.ax - face.bx) * (z - face.bz);
                      const double d2 = (x - face.cx) * (face.bz - face.cz)
                                        - (face.bx - face.cx) * (z - face.cz);
                      const double d3 = (x - face.ax) * (face.cz - face.az)
                                        - (face.cx - face.ax) * (z - face.az);

                      if ((d1 < -1e-9 || d2 < -1e-9 || d3 < -1e-9)
                          && (d1 > 1e-9 || d2 > 1e-9 || d3 > 1e-9))
                      {
                        continue;
                      }

                      const double den = (face.bz - face.cz) * (face.ax - face.cx)
                                         + (face.cx - face.bx) * (face.az - face.cz);

                      if (std::abs(den) < 1e-12)
                      {
                        continue;
                      }

                      const double w1 = ((face.bz - face.cz) * (x - face.cx)
                                         + (face.cx - face.bx) * (z - face.cz))
                                        / den;
                      const double w2 = ((face.cz - face.az) * (x - face.cx)
                                         + (face.ax - face.cx) * (z - face.cz))
                                        / den;
                      const double y = w1 * face.ay + w2 * face.by + (1 - w1 - w2) * face.cy;

                      least = std::min(least, y);
                      most = std::max(most, y);
                    }

                    clash += most > -1e29 && most - least > .01 ? 1 : 0;
                  }
                }

                // And whether the piece's own surface is closed: every edge
                // inside it used twice, and only edges on its outline used
                // once. A slit shows here and nowhere else - two faces meeting
                // along a line at different heights overlap nothing, so no
                // sampling of the plan can see them.
                std::map<std::array<int64_t, 4>, uint32_t> tally;

                const auto keyOf = [](const double ax, const double az, const double bx,
                                      const double bz) {
                  std::array<int64_t, 4> k{int64_t(std::llround(ax * 1000.)),
                                           int64_t(std::llround(az * 1000.)),
                                           int64_t(std::llround(bx * 1000.)),
                                           int64_t(std::llround(bz * 1000.))};

                  if (k[0] > k[2] || (k[0] == k[2] && k[1] > k[3]))
                  {
                    std::swap(k[0], k[2]);
                    std::swap(k[1], k[3]);
                  }

                  return k;
                };

                for (const geom2::RoofTriangle &face : surface)
                {
                  ++tally[keyOf(face.ax, face.az, face.bx, face.bz)];
                  ++tally[keyOf(face.bx, face.bz, face.cx, face.cz)];
                  ++tally[keyOf(face.cx, face.cz, face.ax, face.az)];
                }

                uint32_t loose = 0;

                for (const auto &entry : tally)
                {
                  if (entry.second == 2)
                  {
                    continue;
                  }

                  bool onRing = false;

                  for (size_t w = 0; w < walk.size() && !onRing; ++w)
                  {
                    const auto &a = walk[w];
                    const auto &b = walk[(w + 1) % walk.size()];
                    const double ex = b.first - a.first, ez = b.second - a.second;
                    const double span = std::hypot(ex, ez);

                    if (span < 1e-9)
                    {
                      continue;
                    }

                    const auto off = [&](const double x, const double z) {
                      return std::abs((x - a.first) * ez - (z - a.second) * ex) / span;
                    };

                    onRing = off(double(entry.first[0]) / 1000., double(entry.first[1]) / 1000.)
                               < .003
                             && off(double(entry.first[2]) / 1000.,
                                    double(entry.first[3]) / 1000.)
                                  < .003;
                  }

                  loose += onRing ? 0 : 1;
                }

                if (loose > 0)
                {
                  std::fprintf(stderr, "      SLIT %u edges, %zu gables, slope %.3f, ring:", loose,
                               roofing.gables.size(), roofing.slope);

                  for (const auto &corner : ring)
                  {
                    std::fprintf(stderr, " {%.9f, %.9f},", corner.first, corner.second);
                  }

                  std::fprintf(stderr, "\n");
                }

                if (clash > 0)
                {
                  std::fprintf(stderr, "      PILED %u samples, %zu gables, slope %.3f, ridge %.3f, ring:",
                               clash, roofing.gables.size(), roofing.slope,
                               roofing.ridgeHeight);

                  for (const auto &corner : ring)
                  {
                    std::fprintf(stderr, " {%.9f, %.9f},", corner.first, corner.second);
                  }

                  for (const Ring &hole : piece.holes)
                  {
                    std::fprintf(stderr, " | hole:");

                    for (const Point &corner : hole)
                    {
                      std::fprintf(stderr, " {%.3f, %.3f},", geom2::toMetres(corner.x),
                                   geom2::toMetres(corner.y));
                    }
                  }

                  std::fprintf(stderr, "\n");
                }
              }

              for (const geom2::RoofTriangle &face : surface)
              {
                // The gable against the storey above *is* built.
                //
                // It was skipped for a while, because the centre of such a
                // face lands inside the storey above by a hair and a check for
                // "roof inside a floor" counted it. What it actually is, is
                // the triangle that closes the gap between a lower roof and
                // the taller mass it leans on - and without it the temple had
                // a slot straight through its roof to the sky. A face in the
                // plane of a wall is not inside the building; it is the
                // building.
                rewrite::DwellingRoofFace piece3;
                piece3.gable = face.gable;
                piece3.polygon = {{face.ax, storey.top + face.ay, face.az},
                                  {face.bx, storey.top + face.by, face.bz},
                                  {face.cx, storey.top + face.cy, face.cz}};

                // Only discard numerical zeroes. Small positive-area faces
                // carry shared edges, so an architectural-size area cutoff
                // would open seams. Measure in 3D to include gables.
                const rewrite::Position &a3 = piece3.polygon[0];
                const rewrite::Position &b3 = piece3.polygon[1];
                const rewrite::Position &c3 = piece3.polygon[2];

                const double ux = b3.x - a3.x, uy = b3.y - a3.y, uz = b3.z - a3.z;
                const double vx = c3.x - a3.x, vy = c3.y - a3.y, vz = c3.z - a3.z;

                const double cross = std::sqrt(std::pow(uy * vz - uz * vy, 2.)
                                               + std::pow(uz * vx - ux * vz, 2.)
                                               + std::pow(ux * vy - uy * vx, 2.));

                if (cross * .5 < 1e-12)
                {
                  continue;
                }

                roofs.push_back(std::move(piece3));
              }
            }

          }

          // Note that this runs whether or not the re-massing worked. A
          // building too small to mass - a one-room hut on thirteen square
          // metres - keeps the roof it had, and that roof oversails; but its
          // walls stand where its room is, which is the part that has to be
          // true for anything downstream to read the plan at all.

          regions.clear();

          for (const rewrite::DwellingStorey &storey : storeys)
          {
            Polygon region;
            region.outer = toRing(storey.outer);

            for (const auto &hole : storey.holes)
            {
              region.holes.push_back(toRing(hole));
            }

            if (region.outer.size() >= 3)
            {
              // A cellar is a storey like any other, but it carries the ground
              // floor's level: nothing downstream has a use for a negative one,
              // so it was clamped when the storeys were built. Here it matters.
              // Keyed by level alone the cellar and the ground floor collide,
              // one overwrites the other, and `regions[-1] = regions[0]` then
              // hands the cellar an outline that is not its own - so every wall
              // around the cellar fails the test for standing on the boundary
              // and the cellar comes out with no exterior wall at all. Its
              // depth is what tells the two apart.
              regions[storey.base < 0 ? -1 : int32_t(storey.level)] = region;
            }
          }

          if (regions.count(0) > 0 && regions.count(-1) == 0 && sketch.basement)
          {
            regions[-1] = regions[0];
          }

          outline.clear();

          for (const auto &corner : ground->second)
          {
            outline.emplace_back(corner.first, corner.second);
          }
        }
      }
    }

    out.programme = optimised.programme;
    out.layoutCost = optimised.cost;
    out.startCost = optimised.startCost;
    out.sampledDimensionCost = optimised.sampledDimensionCost;
    out.optimised = true;
    out.footprint = outline;
    out.areaError = optimised.areaError;
    out.doorsDelivered =
      optimised.doorsAsked > 0
        ? double(optimised.doorsDelivered) / double(optimised.doorsAsked)
        : 1.;
    out.roomsPlaced = uint32_t(optimised.rooms.size());
    out.roomsDropped = uint32_t(sketch.rooms.size()) - out.roomsPlaced;
    out.floorsPlanned = uint32_t(optimised.outlines.size());

    for (const LayoutRoom &room : optimised.rooms)
    {
      PlacedRoom placed;
      placed.programmeRoom = room.programmeRoom;
      placed.use = room.use;
      placed.floor = room.floor;
      placed.askedMetres = sketch.rooms[room.programmeRoom].areaMetres;
      placed.achievedMetres = room.areaMetres;

      for (const auto &corner : room.outline)
      {
        placed.boundary.outer.push_back(
          {geom2::toUnit(corner.first), geom2::toUnit(corner.second)});
      }

      out.rooms.push_back(std::move(placed));
    }

    for (const LayoutDoor &door : optimised.doors)
    {
      out.doors.push_back({door.a, door.b, door.door, door.x, door.z});
    }

    for (const LayoutEntrance &entrance : optimised.entrances)
    {
      out.entrances.push_back(
        {entrance.room, entrance.x, entrance.z, entrance.normalX, entrance.normalZ});
    }
  }
  else if (!planProgrammeBuilding(brief, seed, settings, outline, out, error, &regions))
  {
    return false;
  }

  // --- fill the plan ------------------------------------------------------

  plan.id = brief.storeys ? *brief.storeys : 0;
  plan.seed = seed;
  plan.wings = std::move(wings);
  plan.roofs = std::move(roofs);
  plan.storeys = std::move(storeys);
  plan.pillars = std::move(pillars);

  const uint32_t levels = std::max(1u, out.programme.storeys);

  for (uint32_t r = 0; r < out.rooms.size(); ++r)
  {
    const PlacedRoom &placed = out.rooms[r];

    rewrite::DwellingRoom room;
    room.id = r + 1;
    room.floor = uint32_t(std::max(placed.floor, 0));
    room.usableArea = placed.achievedMetres;
    room.boundary = toPositions(placed.boundary.outer);

    const double base = floorElevation(local, placed.floor, levels, brief.standing);

    for (rewrite::Position &point : room.boundary)
    {
      point.y = base;
    }

    plan.rooms.push_back(std::move(room));

    // Walls: every edge of the room, exterior where it lies on the boundary of
    // the ground that floor stands on.

    for (size_t i = 0; i < placed.boundary.outer.size(); ++i)
    {
      const Point &a = placed.boundary.outer[i];
      const Point &b = placed.boundary.outer[(i + 1) % placed.boundary.outer.size()];

      rewrite::DwellingWall wall;
      wall.id = plan.walls.size() + 1;
      wall.floor = room.floor;
      wall.thickness = local.wallThickness;
      wall.a = {geom2::toMetres(a.x), base, geom2::toMetres(a.y)};
      wall.b = {geom2::toMetres(b.x), base, geom2::toMetres(b.y)};
      wall.exterior = false;
      for (const rewrite::DwellingStorey &storey : plan.storeys)
      {
        if (std::abs(storey.base - base) > 1e-6)
        {
          continue;
        }
        Polygon shell;
        shell.outer = toRing(storey.outer);
        for (const auto &hole : storey.holes)
        {
          shell.holes.push_back(toRing(hole));
        }
        wall.exterior = wall.exterior || onBoundary(shell, wall.a.x, wall.a.z, wall.b.x, wall.b.z);
      }

      plan.walls.push_back(std::move(wall));
    }
  }

  for (const PlacedDoor &door : out.doors)
  {
    if (door.a >= out.rooms.size() || door.b >= out.rooms.size())
    {
      continue;
    }

    const double base =
      floorElevation(local, out.rooms[door.a].floor, levels, brief.standing);

    rewrite::DwellingDoor opening;
    opening.floor = uint32_t(std::max(out.rooms[door.a].floor, 0));
    opening.room = door.a + 1;
    opening.center = {door.x, base, door.z};

    // An open wall is not a door: it is a cased opening, and the difference
    // is what the programme spent a variable on. Drawn wide and flagged, the
    // way a shop's selling opening already is.
    opening.width = door.door ? .9 : 1.6;
    opening.shopOpening = !door.door
                          && (out.rooms[door.a].use == RoomUse::Shop
                              || out.rooms[door.b].use == RoomUse::Shop);

    // The wall it sits in, found by the room it belongs to; walls were written
    // in room order so the first of this room's is a good enough anchor for a
    // consumer that wants one.
    for (const rewrite::DwellingWall &wall : plan.walls)
    {
      const double dx = wall.a.x - opening.center.x;
      const double dz = wall.a.z - opening.center.z;
      const double ex = wall.b.x - opening.center.x;
      const double ez = wall.b.z - opening.center.z;

      if (wall.floor == opening.floor
          && std::sqrt(dx * dx + dz * dz) + std::sqrt(ex * ex + ez * ez)
               < std::sqrt((wall.b.x - wall.a.x) * (wall.b.x - wall.a.x)
                           + (wall.b.z - wall.a.z) * (wall.b.z - wall.a.z))
                   + .05)
      {
        opening.wall = wall.id;

        break;
      }
    }

    plan.doors.push_back(std::move(opening));
  }

  // The stair, where the programme put one, running the height of the house.
  for (uint32_t r = 0; r < out.rooms.size(); ++r)
  {
    if (out.rooms[r].use != RoomUse::Staircase || out.rooms[r].floor != 0)
    {
      continue;
    }

    for (uint32_t level = 0; level + 1 < std::max(1u, out.programme.storeys); ++level)
    {
      // A flight climbs the storey it starts in, so its rise is that storey's
      // height and not a constant: the ground floor of a mansion is thirteen
      // feet and takes four more steps than the attic.
      const double climb = ceilingMetres(local, int32_t(level), levels, brief.standing);

      rewrite::DwellingStair stair;
      stair.lowerFloor = level;
      stair.rise = climb;
      stair.risers = uint32_t(std::max(1., std::round(climb / .19)));
      stair.opening = toPositions(out.rooms[r].boundary.outer);

      for (rewrite::Position &point : stair.opening)
      {
        point.y = floorElevation(local, int32_t(level), levels, brief.standing);
      }

      plan.stairs.push_back(std::move(stair));
    }

    break;
  }

  // The way in, as a door of the plan. Marked exterior, which is what tells
  // everything downstream - the path from the street, the porch, the person
  // walking in - that this is the one that matters.
  for (const PlacedEntrance &entrance : out.entrances)
  {
    if (entrance.room >= out.rooms.size())
    {
      continue;
    }

    rewrite::DwellingDoor opening;
    opening.floor = uint32_t(std::max(out.rooms[entrance.room].floor, 0));
    opening.room = entrance.room + 1;
    opening.center = {entrance.x,
                      floorElevation(local, out.rooms[entrance.room].floor,
                                     std::max(1u, out.programme.storeys), brief.standing),
                      entrance.z};
    opening.width = 1.;
    opening.exterior = true;

    plan.doors.push_back(std::move(opening));
  }

  // --- the ground around it -------------------------------------------------
  //
  // What is left of the lot once the building stands on it, allocated to the
  // outdoor rooms the programme drew: the yard by the back door, the croft
  // behind it, the pen, the midden downwind of the house, the garden.
  //
  // A power diagram over the leftover, as the old interior placer used: each
  // outdoor room is a site whose weight is the area it asked for, started
  // beside the indoor room its door opens onto. That last part is what makes
  // it a plan rather than a subdivision - a yard reached only across the
  // croft is not a yard, and a midden by the front door is not a midden.
  if (squared.size() >= 3)
  {
    std::vector<uint32_t> outdoor;

    for (uint32_t r = 0; r < sketch.rooms.size(); ++r)
    {
      if (!enclosesFloorArea(sketch.rooms[r].use))
      {
        outdoor.push_back(r);
      }
    }

    // A town house with no outdoor room drawn still has a back yard: the
    // privy is out there, the water is out there, and the washing is done out
    // there. The programme not naming it does not mean the ground is nothing -
    // it means nothing in the house's own description depended on it.
    const bool named = !outdoor.empty();

    {
      Polygon lotPolygon;
      lotPolygon.outer = toRing(squared);

      std::vector<geom2::Operand> built;

      for (const rewrite::DwellingStorey &storey : plan.storeys)
      {
        if (storey.level == 0 && storey.outer.size() >= 3)
        {
          built.push_back(geom2::toOperand(toRing(storey.outer)));
        }
      }

      std::vector<Polygon> leftover;

      if (built.empty()
          || !geom2::differenceOf(geom2::toOperand(lotPolygon), built, leftover, nullptr))
      {
        leftover = {lotPolygon};
      }

      // The largest piece. A lot cut into slivers by an L-shaped building has
      // ground on both sides of it, and the far side is a gap between houses
      // rather than a yard.
      size_t largest = 0;

      for (size_t i = 1; i < leftover.size(); ++i)
      {
        if (std::abs(polygonArea(leftover[i])) > std::abs(polygonArea(leftover[largest])))
        {
          largest = i;
        }
      }

      if (!leftover.empty() && std::abs(polygonArea(leftover[largest])) > 6.)
      {
        const Polygon &open = leftover[largest];

        // Where each outdoor room wants to be: beside the room it opens onto,
        // pulled into the open ground.
        double midX = 0, midZ = 0;

        for (const Point &point : open.outer)
        {
          midX += geom2::toMetres(point.x) / double(open.outer.size());
          midZ += geom2::toMetres(point.y) / double(open.outer.size());
        }

        std::vector<geom2::Site> sites;
        std::vector<uint32_t> which;

        if (!named)
        {
          PlacedGround yard;
          yard.use = RoomUse::Yard;
          yard.boundary = open;
          yard.askedMetres = std::abs(polygonArea(open));
          yard.achievedMetres = yard.askedMetres;
          out.grounds.push_back(std::move(yard));
        }

        for (const uint32_t r : outdoor)
        {
          double atX = midX, atZ = midZ;

          for (const Adjacency &link : sketch.links)
          {
            const uint32_t other = link.a == r ? link.b : link.a;

            if ((link.a != r && link.b != r) || other >= sketch.rooms.size())
            {
              continue;
            }

            for (const PlacedRoom &room : out.rooms)
            {
              if (room.programmeRoom != other || room.boundary.outer.empty())
              {
                continue;
              }

              double roomX = 0, roomZ = 0;

              for (const Point &point : room.boundary.outer)
              {
                roomX += geom2::toMetres(point.x) / double(room.boundary.outer.size());
                roomZ += geom2::toMetres(point.y) / double(room.boundary.outer.size());
              }

              // Started a third of the way from that room toward the middle of
              // the open ground: outside the building, and on its side.
              atX = roomX + (midX - roomX) * .34;
              atZ = roomZ + (midZ - roomZ) * .34;

              break;
            }

            break;
          }

          geom2::Site site;
          site.position = {geom2::toUnit(atX), geom2::toUnit(atZ)};
          // A yard is not a corner of the lot left over.
          //
          // The floor here was four square metres, which is a cupboard: a
          // programme that asked for a small yard got one, and the ground the
          // household actually works in went to whichever outdoor room had
          // asked for more. The working yard of a house of this period is
          // twenty-odd square metres before anything else gets a share - the
          // privy, the water, the washing and somewhere to stand while doing
          // any of it - and the rooms that are ground rather than yard keep
          // their own smaller floor.
          const RoomUse use = sketch.rooms[r].use;
          const bool working = use == RoomUse::Yard || use == RoomUse::Courtyard
                               || use == RoomUse::ExerciseYard;

          site.weight = std::max(sketch.rooms[r].areaMetres, working ? 22. : 8.);
          sites.push_back(site);
          which.push_back(r);
        }

        geom2::PartitionOptions options;
        std::vector<geom2::PartitionCell> cells;

        if (sites.size() == 1)
        {
          PlacedGround ground;
          ground.use = sketch.rooms[which.front()].use;
          ground.boundary = open;
          ground.askedMetres = sketch.rooms[which.front()].areaMetres;
          ground.achievedMetres = std::abs(polygonArea(open));
          out.grounds.push_back(std::move(ground));
        }
        else if (geom2::partitionRegion(open, sites, options, cells, nullptr))
        {
          for (const geom2::PartitionCell &cell : cells)
          {
            if (cell.site >= which.size() || cell.polygon.outer.size() < 3)
            {
              continue;
            }

            PlacedGround ground;
            ground.use = sketch.rooms[which[cell.site]].use;
            ground.boundary = cell.polygon;
            ground.askedMetres = sketch.rooms[which[cell.site]].areaMetres;
            ground.achievedMetres = std::abs(polygonArea(cell.polygon));
            out.grounds.push_back(std::move(ground));
          }
        }
      }
    }
  }

  // --- who is in it ---------------------------------------------------------

  {
    const Programme &made = out.programme;
    Household &household = out.household;

    const uint32_t beds = made.countOf(RoomUse::Bedroom);
    const uint32_t dormitories = made.countOf(RoomUse::Dormitory);

    // What the standing is worth, and how many of the household are staff.
    switch (made.standing)
    {
      case PlotStanding::Slum: household.wealth = .08; break;
      case PlotStanding::Rural: household.wealth = .22; break;
      case PlotStanding::Modest: household.wealth = .35; break;
      case PlotStanding::Comfortable: household.wealth = .55; break;
      case PlotStanding::Wealthy: household.wealth = .8; break;
      case PlotStanding::Grand: household.wealth = 1.; break;
    }

    // A bed sleeps two - a married pair, or two children. A dormitory sleeps
    // as many as it has floor for, at the four square metres a straw pallet
    // and the space to stand beside it actually took.
    household.residents = beds * 2;

    for (const PlacedRoom &room : out.rooms)
    {
      if (room.use == RoomUse::Dormitory)
      {
        household.residents += uint32_t(std::max(2., std::floor(room.achievedMetres / 4.)));
      }
    }

    if (household.residents == 0 && dormitories == 0)
    {
      // No bedroom is not an empty house. It is a cottage of one or two rooms
      // and the family sleeps in the room it lives in, which is how most
      // people have lived in most places: a household, on the floor, by the
      // hearth.
      double floor = 0;

      for (const PlacedRoom &room : out.rooms)
      {
        floor += enclosesFloorArea(room.use) ? room.achievedMetres : 0.;
      }

      household.residents = uint32_t(std::min(8., std::max(2., std::floor(floor / 7.))));
    }

    // Staff, in a house with somewhere to put them.
    household.servants = made.countOf(RoomUse::ServantsHall) * 3;

    if (household.servants == 0 && household.wealth > .7)
    {
      household.servants = made.has(RoomUse::Kitchen) ? 2 : 1;
    }

    household.residents += household.servants;

    // ...and who comes in to work without living here.
    for (const PlacedRoom &room : out.rooms)
    {
      switch (room.use)
      {
        case RoomUse::Shop:
        case RoomUse::CountingHouse:
        case RoomUse::Office: household.workers += 1; break;
        case RoomUse::Workshop:
        case RoomUse::Brewhouse:
          household.workers += uint32_t(std::max(1., std::floor(room.achievedMetres / 9.)));
          break;
        default: break;
      }
    }

    char line[256];
    std::snprintf(line, sizeof(line),
                  "a %s %s of %u rooms on %u storey%s, %u living here and %u working",
                  name(made.standing), name(made.building), out.roomsPlaced, made.storeys,
                  made.storeys == 1 ? "" : "s", household.residents, household.workers);
    household.description = line;

    plan.note = household.description;
    plan.residentCapacity = household.residents;
    plan.workerCapacity = household.workers;

    switch (made.building)
    {
      case BuildingClass::ShopHouse:
      case BuildingClass::CommerceHouse: plan.use = rewrite::DwellingUse::ShopHouse; break;
      case BuildingClass::Warehouse: plan.use = rewrite::DwellingUse::Warehouse; break;
      case BuildingClass::Inn:
      case BuildingClass::Dormitory: plan.use = rewrite::DwellingUse::Lodging; break;
      case BuildingClass::GuildHall:
      case BuildingClass::ManorHouse:
      case BuildingClass::Castle:
      case BuildingClass::Mansion: plan.use = rewrite::DwellingUse::Hall; break;
      default: plan.use = rewrite::DwellingUse::Cottage; break;
    }
  }

  // ...and back into the world, square to the lot rather than to north.
  turn(plan, out, axis);

  return true;
}


bool planProgrammeInside(const std::vector<rewrite::Position> &house,
                         const ProgrammeBrief &brief,
                         const uint64_t seed,
                         const ProgrammePlanSettings &settings,
                         const ComplexDwellingSettings &massing,
                         ProgrammeBuilding &out,
                         rewrite::DwellingPlan &plan,
                         std::string *error,
                         const FloorPlanSettings *optimise)
{
  ProgrammePlanSettings inside = settings;
  inside.coverage = 1.;

  return planProgrammeDwelling(brief, seed, inside, massing, house, out, plan, error,
                               optimise);
}

}  // namespace procedural::world::generator
