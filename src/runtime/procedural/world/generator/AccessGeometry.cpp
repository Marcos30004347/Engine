#include "AccessGeometry.hpp"

#include "procedural/geom2/Boolean.hpp"
#include "procedural/geom2/Offset.hpp"
#include "procedural/geom2/Polygon.hpp"
#include "procedural/geom2/Quantise.hpp"
#include "procedural/geom3/Prism.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <array>
#include <sstream>

namespace procedural::world::generator::access {

namespace {

constexpr double kSameHeight = .05;

geom2::Ring ringOf(const std::vector<Position> &from, const bool clockwise = false)
{
  geom2::Ring ring;
  ring.reserve(from.size());

  for (const Position &p : from)
  {
    ring.push_back(geom2::Point{geom2::toUnit(p.x), geom2::toUnit(p.z)});
  }

  if (ring.size() >= 3 && geom2::counterClockwise(ring) == clockwise)
  {
    geom2::reverse(ring);
  }

  return ring;
}

std::vector<Position> positionsOf(const geom2::Ring &ring, const double y)
{
  std::vector<Position> out;
  out.reserve(ring.size());

  for (const geom2::Point &p : ring)
  {
    out.push_back(Position{geom2::toMetres(p.x), y, geom2::toMetres(p.y)});
  }

  return out;
}

geom2::Polygon polygonOf(const Surface &surface)
{
  geom2::Polygon polygon;
  polygon.outer = ringOf(surface.outer);

  for (const std::vector<Position> &hole : surface.holes)
  {
    if (hole.size() >= 3)
    {
      polygon.holes.push_back(ringOf(hole, true));
    }
  }

  return polygon;
}

// How far a point is from a ring's boundary, negative inside.
double signedDistance(const std::vector<Position> &ring, const double x, const double z)
{
  if (ring.size() < 3)
  {
    return 1e30;
  }

  double nearest = 1e30;

  for (size_t i = 0; i < ring.size(); ++i)
  {
    const Position &a = ring[i];
    const Position &b = ring[(i + 1) % ring.size()];
    const double ex = b.x - a.x, ez = b.z - a.z;
    const double length = ex * ex + ez * ez;
    const double t = length > 1e-12 ? std::max(0., std::min(1., ((x - a.x) * ex + (z - a.z) * ez) / length)) : 0.;
    nearest = std::min(nearest, std::hypot(a.x + ex * t - x, a.z + ez * t - z));
  }

  return geom2::contains(ringOf(ring), geom2::Point{geom2::toUnit(x), geom2::toUnit(z)}) ? -nearest : nearest;
}

// ...and the same over a surface, which is its outer ring less its holes.
double depthInto(const Surface &surface, const double x, const double z)
{
  double into = signedDistance(surface.outer, x, z);

  for (const std::vector<Position> &hole : surface.holes)
  {
    const double in = signedDistance(hole, x, z);

    if (in < 0)
    {
      return -in; // Inside a hole is off the surface.
    }

    into = into < 0 ? std::max(into, -in) : into;
  }

  return into;
}

bool standsOn(const Surface &surface, const Position &p, const double slack)
{
  return depthInto(surface, p.x, p.z) < slack;
}

Position middleOf(const Position &a, const Position &b)
{
  return Position{(a.x + b.x) * .5, (a.y + b.y) * .5, (a.z + b.z) * .5};
}

double areaOf(const std::vector<Position> &ring)
{
  double twice = 0;

  for (size_t i = 0; i < ring.size(); ++i)
  {
    const Position &a = ring[i];
    const Position &b = ring[(i + 1) % ring.size()];
    twice += a.x * b.z - b.x * a.z;
  }

  return std::abs(twice) * .5;
}

bool crosses(const Position &a, const Position &b, const Position &c, const Position &d)
{
  const auto side = [](const Position &p, const Position &q, const Position &r) {
    const double v = (q.x - p.x) * (r.z - p.z) - (q.z - p.z) * (r.x - p.x);

    return v > 1e-9 ? 1 : (v < -1e-9 ? -1 : 0);
  };

  return side(a, b, c) * side(a, b, d) < 0 && side(c, d, a) * side(c, d, b) < 0;
}

// The plan of a ramp: its four corners flattened.
std::vector<Position> planOf(const Ramp &ramp)
{
  return {Position{ramp.footLeft.x, 0, ramp.footLeft.z}, Position{ramp.footRight.x, 0, ramp.footRight.z}, Position{ramp.headRight.x, 0, ramp.headRight.z}, Position{ramp.headLeft.x, 0, ramp.headLeft.z}};
}

// How high a ramp's surface is over a point in its plan, or nothing when the
// point is not under it.
bool heightOver(const Ramp &ramp, const double x, const double z, double &y)
{
  const std::vector<Position> plan = planOf(ramp);

  if (signedDistance(plan, x, z) > 0)
  {
    return false;
  }

  // The ramp runs from the middle of its foot edge to the middle of its head.
  const Position foot = middleOf(ramp.footLeft, ramp.footRight);
  const Position head = middleOf(ramp.headLeft, ramp.headRight);
  const double ax = head.x - foot.x, az = head.z - foot.z;
  const double length = ax * ax + az * az;
  const double t = length > 1e-12 ? std::max(0., std::min(1., ((x - foot.x) * ax + (z - foot.z) * az) / length)) : 0.;

  y = foot.y + (head.y - foot.y) * t;

  return true;
}

} // namespace

const char *nameOf(const FaultKind kind)
{
  switch (kind)
  {
  case FaultKind::SurfaceDegenerate:
    return "surface-degenerate";
  case FaultKind::SurfaceOverlaps:
    return "surfaces-overlap";
  case FaultKind::RampFootOffSurface:
    return "ramp-foot-off-surface";
  case FaultKind::RampHeadOffSurface:
    return "ramp-head-off-surface";
  case FaultKind::RampPierces:
    return "ramp-pierces-surface";
  case FaultKind::RampTooSteep:
    return "ramp-too-steep";
  case FaultKind::PortalOffSurface:
    return "portal-off-surface";
  case FaultKind::PortalPinched:
    return "portal-pinched";
  case FaultKind::PortalOnRamp:
    return "portal-on-ramp";
  case FaultKind::SurfaceInSolid:
    return "surface-in-solid";
  case FaultKind::RampInSolid:
    return "ramp-in-solid";
  case FaultKind::EntranceBlocked:
    return "entrance-blocked";
  case FaultKind::UnguardedEdge:
    return "unguarded-edge";
  case FaultKind::NoWayThrough:
    return "no-way-through";
  case FaultKind::Unreachable:
    return "unreachable";
  }

  return "unknown";
}

bool walkableRegion(const Surface &surface, const double clearWidth, std::vector<std::vector<Position>> &pieces)
{
  pieces.clear();

  if (surface.outer.size() < 3)
  {
    return false;
  }

  std::vector<geom2::Polygon> eroded;

  if (!geom2::offsetPolygon(polygonOf(surface), -clearWidth * .5, {}, eroded, nullptr))
  {
    return false;
  }

  for (const geom2::Polygon &piece : eroded)
  {
    if (piece.outer.size() >= 3)
    {
      pieces.push_back(positionsOf(piece.outer, surface.height));
    }
  }

  return true;
}

std::vector<std::vector<uint32_t>> connections(const Model &model)
{
  std::vector<std::vector<uint32_t>> joined(model.surfaces.size());

  const auto index = [&](const uint32_t id) {
    for (size_t i = 0; i < model.surfaces.size(); ++i)
    {
      if (model.surfaces[i].id == id)
      {
        return int32_t(i);
      }
    }

    return int32_t(-1);
  };

  const auto join = [&](const int32_t u, const int32_t v) {
    if (u < 0 || v < 0 || u == v)
    {
      return;
    }

    joined[size_t(u)].push_back(uint32_t(v));
    joined[size_t(v)].push_back(uint32_t(u));
  };

  // Surfaces at one height that meet.
  for (size_t i = 0; i < model.surfaces.size(); ++i)
  {
    for (size_t j = i + 1; j < model.surfaces.size(); ++j)
    {
      if (std::abs(model.surfaces[i].height - model.surfaces[j].height) > kSameHeight)
      {
        continue;
      }

      bool meet = false;

      for (const Position &p : model.surfaces[j].outer)
      {
        meet = meet || depthInto(model.surfaces[i], p.x, p.z) < .05;
      }

      for (const Position &p : model.surfaces[i].outer)
      {
        meet = meet || depthInto(model.surfaces[j], p.x, p.z) < .05;
      }

      if (meet)
      {
        join(int32_t(i), int32_t(j));
      }
    }
  }

  // ...and the ramps, by where they actually land rather than by what they
  // claim. A ramp's `from` and `to` are a statement to be checked; the walk
  // must not be built out of them.
  for (const Ramp &ramp : model.ramps)
  {
    const Position foot = middleOf(ramp.footLeft, ramp.footRight);
    const Position head = middleOf(ramp.headLeft, ramp.headRight);

    int32_t at = -1, onto = -1;

    for (size_t i = 0; i < model.surfaces.size(); ++i)
    {
      const Surface &surface = model.surfaces[i];

      if (std::abs(surface.height - foot.y) < .25 && standsOn(surface, foot, .35))
      {
        at = int32_t(i);
      }

      if (std::abs(surface.height - head.y) < .25 && standsOn(surface, head, .35))
      {
        onto = int32_t(i);
      }
    }

    join(at, onto);
  }

  return joined;
}

std::vector<bool> reachable(const Model &model)
{
  const std::vector<std::vector<uint32_t>> joined = connections(model);
  std::vector<bool> seen(model.surfaces.size(), false);
  std::deque<uint32_t> queue;

  for (size_t i = 0; i < model.surfaces.size(); ++i)
  {
    if (model.surfaces[i].kind == SurfaceKind::Ground)
    {
      seen[i] = true;
      queue.push_back(uint32_t(i));
    }
  }

  while (!queue.empty())
  {
    const uint32_t at = queue.front();
    queue.pop_front();

    for (const uint32_t next : joined[at])
    {
      if (!seen[next])
      {
        seen[next] = true;
        queue.push_back(next);
      }
    }
  }

  return seen;
}

std::vector<Entrance> entrances(const Model &model)
{
  std::vector<Entrance> out;

  for (const Portal &portal : model.portals)
  {
    // The step out: from the wall to the floor you stand on.
    Entrance door;
    door.a = portal.at;
    door.b = Position{portal.at.x + portal.outX * portal.into, portal.at.y, portal.at.z + portal.outZ * portal.into};
    door.height = portal.at.y;
    door.of = portal.id;
    door.fromRamp = false;

    out.push_back(std::move(door));
  }

  for (const Ramp &ramp : model.ramps)
  {
    const Position foot = middleOf(ramp.footLeft, ramp.footRight);
    const Position head = middleOf(ramp.headLeft, ramp.headRight);
    const double ax = head.x - foot.x, az = head.z - foot.z;
    const double length = std::hypot(ax, az);

    if (length < 1e-6)
    {
      continue;
    }

    // The step onto the bottom tread and the step off the top one, each half a
    // metre of the way the ramp runs. A rail laid along either end crosses
    // these; laid along the ramp's side it does not, which is right, because a
    // handrail down the side of a stair is a handrail.
    const double ux = ax / length, uz = az / length;
    const double reach = .5;

    Entrance onto;
    onto.a = Position{foot.x - ux * reach, foot.y, foot.z - uz * reach};
    onto.b = Position{foot.x + ux * reach * .5, foot.y, foot.z + uz * reach * .5};
    onto.height = foot.y;
    onto.of = ramp.id;
    onto.fromRamp = true;

    Entrance off;
    off.a = Position{head.x - ux * reach * .5, head.y, head.z - uz * reach * .5};
    off.b = Position{head.x + ux * reach, head.y, head.z + uz * reach};
    off.height = head.y;
    off.of = ramp.id;
    off.fromRamp = true;

    out.push_back(std::move(onto));
    out.push_back(std::move(off));
  }

  return out;
}

bool triangulate(const Model &model, std::vector<Face> &out, std::string *error)
{
  out.clear();

  const auto push = [&](const Position &a, const Position &b, const Position &c, const FaceKind kind, const uint32_t of) {
    out.push_back({a, b, c, kind, of});
  };

  for (const Surface &surface : model.surfaces)
  {
    if (surface.outer.size() < 3)
    {
      continue;
    }

    std::vector<std::array<uint32_t, 3>> triangles;
    std::vector<geom2::Point> vertices;

    if (!geom3::triangulate(polygonOf(surface), triangles, vertices, error))
    {
      return false;
    }

    for (const auto &tri : triangles)
    {
      push(Position{geom2::toMetres(vertices[tri[0]].x), surface.height, geom2::toMetres(vertices[tri[0]].y)},
           Position{geom2::toMetres(vertices[tri[1]].x), surface.height, geom2::toMetres(vertices[tri[1]].y)},
           Position{geom2::toMetres(vertices[tri[2]].x), surface.height, geom2::toMetres(vertices[tri[2]].y)},
           FaceKind::SurfaceFace, surface.id);
    }
  }

  for (const Ramp &ramp : model.ramps)
  {
    push(ramp.footLeft, ramp.footRight, ramp.headRight, FaceKind::RampFace, ramp.id);
    push(ramp.footLeft, ramp.headRight, ramp.headLeft, FaceKind::RampFace, ramp.id);
  }

  for (const Barrier &barrier : model.barriers)
  {
    // `a` and `b` normally share `base`; stair guards deliberately do not.
    // Keeping the endpoint heights lets one barrier face follow a flight
    // instead of becoming a horizontal fence cutting through it.
    const double ay = std::abs(barrier.a.y - barrier.base) < 1e-6 ? barrier.base : barrier.a.y;
    const double by = std::abs(barrier.b.y - barrier.base) < 1e-6 ? barrier.base : barrier.b.y;
    const auto quad = [&](const Position &a, const Position &b, const Position &c, const Position &d) {
      push(a, b, c, FaceKind::BarrierFace, barrier.id);
      push(a, c, d, FaceKind::BarrierFace, barrier.id);
    };
    if (barrier.wall)
    {
      quad({barrier.a.x, ay, barrier.a.z}, {barrier.b.x, by, barrier.b.z},
           {barrier.b.x, by + barrier.height, barrier.b.z}, {barrier.a.x, ay + barrier.height, barrier.a.z});
      continue;
    }

    // A rail is not a one-metre solid parapet.  Draw two narrow rails and an
    // end post at either end; the access validator still treats their whole
    // line as the guarded boundary, while the mesh lets the stair and balcony
    // remain visible through them.
    const double band = std::min(.12, barrier.height * .18);
    for (const double lift : {barrier.height * .5, barrier.height})
      quad({barrier.a.x, ay + lift - band, barrier.a.z}, {barrier.b.x, by + lift - band, barrier.b.z},
           {barrier.b.x, by + lift, barrier.b.z}, {barrier.a.x, ay + lift, barrier.a.z});
    const double dx = barrier.b.x - barrier.a.x, dz = barrier.b.z - barrier.a.z;
    const double length = std::hypot(dx, dz);
    if (length > 1e-6)
    {
      const double px = dx / length * std::min(.08, length * .25);
      const double pz = dz / length * std::min(.08, length * .25);
      quad({barrier.a.x, ay, barrier.a.z}, {barrier.a.x + px, ay + (by-ay) * std::min(.08, length*.25) / length, barrier.a.z + pz},
           {barrier.a.x + px, ay + (by-ay) * std::min(.08, length*.25) / length + barrier.height, barrier.a.z + pz},
           {barrier.a.x, ay + barrier.height, barrier.a.z});
      quad({barrier.b.x - px, by - (by-ay) * std::min(.08, length*.25) / length, barrier.b.z - pz}, {barrier.b.x, by, barrier.b.z},
           {barrier.b.x, by + barrier.height, barrier.b.z},
           {barrier.b.x - px, by - (by-ay) * std::min(.08, length*.25) / length + barrier.height, barrier.b.z - pz});
    }
  }

  // A portal is an opening in a later wall mesher, not a decorative point.
  // Carry its exact rectangle in the simple mesh too.  It is offset a hair
  // outwards so a diagnostic render can show the aperture without z-fighting
  // the deliberately simple, still-un-cut building prism behind it.
  for (const Portal &portal : model.portals)
  {
    const double length = std::hypot(portal.outX, portal.outZ);
    if (length < 1e-9 || portal.width <= 0 || portal.height <= 0) continue;
    const double ox = portal.outX / length, oz = portal.outZ / length;
    const double tx = -oz, tz = ox;
    const double half = portal.width * .5;
    const double lift = .012;
    const Position a{portal.at.x - tx * half + ox * lift, portal.at.y, portal.at.z - tz * half + oz * lift};
    const Position b{portal.at.x + tx * half + ox * lift, portal.at.y, portal.at.z + tz * half + oz * lift};
    const Position c{b.x, portal.at.y + portal.height, b.z};
    const Position d{a.x, portal.at.y + portal.height, a.z};
    push(a, b, c, FaceKind::PortalFace, portal.id);
    push(a, c, d, FaceKind::PortalFace, portal.id);
  }

  for (const Solid &solid : model.solids)
  {
    if (solid.footprint.size() < 3)
    {
      continue;
    }

    for (size_t i = 0; i < solid.footprint.size(); ++i)
    {
      const Position &p = solid.footprint[i];
      const Position &q = solid.footprint[(i + 1) % solid.footprint.size()];
      const double dx = q.x - p.x, dz = q.z - p.z, length = std::hypot(dx, dz);
      if (length < 1e-9) continue;
      std::vector<std::array<double, 4>> openings; // along0, along1, low, high
      std::vector<double> alongs{0., length}, levels{solid.base, solid.top};
      for (const Portal &portal : model.portals)
      {
        const double along = ((portal.at.x - p.x) * dx + (portal.at.z - p.z) * dz) / length;
        const double crossAway = std::abs((portal.at.x - p.x) * dz - (portal.at.z - p.z) * dx) / length;
        if (crossAway > .05 || along < -.05 || along > length + .05 ||
            portal.at.y + portal.height <= solid.base || portal.at.y >= solid.top) continue;
        const double a0 = std::clamp(along - portal.width * .5, 0., length);
        const double a1 = std::clamp(along + portal.width * .5, 0., length);
        const double low = std::clamp(portal.at.y, solid.base, solid.top);
        const double high = std::clamp(portal.at.y + portal.height, solid.base, solid.top);
        if (a1 <= a0 || high <= low) continue;
        openings.push_back({a0, a1, low, high});
        alongs.push_back(a0); alongs.push_back(a1); levels.push_back(low); levels.push_back(high);
      }
      std::sort(alongs.begin(), alongs.end()); alongs.erase(std::unique(alongs.begin(), alongs.end()), alongs.end());
      std::sort(levels.begin(), levels.end()); levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
      const auto at = [&](const double along, const double y) {
        return Position{p.x + dx * along / length, y, p.z + dz * along / length};
      };
      for (size_t x = 0; x + 1 < alongs.size(); ++x)
      for (size_t y = 0; y + 1 < levels.size(); ++y)
      {
        const double mx = (alongs[x] + alongs[x + 1]) * .5, my = (levels[y] + levels[y + 1]) * .5;
        bool aperture = false;
        for (const auto &opening : openings)
          aperture = aperture || (mx > opening[0] - 1e-9 && mx < opening[1] + 1e-9 &&
                                    my > opening[2] - 1e-9 && my < opening[3] + 1e-9);
        if (aperture) continue;
        const Position a = at(alongs[x], levels[y]), b = at(alongs[x + 1], levels[y]);
        const Position c = at(alongs[x + 1], levels[y + 1]), d = at(alongs[x], levels[y + 1]);
        push(a, b, c, FaceKind::SolidFace, solid.id); push(a, c, d, FaceKind::SolidFace, solid.id);
      }
    }

    geom2::Polygon lid;
    lid.outer = ringOf(solid.footprint);

    std::vector<std::array<uint32_t, 3>> triangles;
    std::vector<geom2::Point> vertices;

    if (geom3::triangulate(lid, triangles, vertices, nullptr))
    {
      for (const auto &tri : triangles)
      {
        push(Position{geom2::toMetres(vertices[tri[0]].x), solid.top, geom2::toMetres(vertices[tri[0]].y)},
             Position{geom2::toMetres(vertices[tri[1]].x), solid.top, geom2::toMetres(vertices[tri[1]].y)},
             Position{geom2::toMetres(vertices[tri[2]].x), solid.top, geom2::toMetres(vertices[tri[2]].y)},
             FaceKind::SolidFace, solid.id);
      }
    }
  }

  return true;
}

std::vector<Fault> validate(const Model &model)
{
  std::vector<Fault> faults;

  const auto fault = [&](const FaultKind kind, const uint32_t a, const uint32_t b, const Position &where, std::string what) {
    faults.push_back({kind, a, b, where, std::move(what)});
  };

  // ---- the surfaces are polygons ------------------------------------------
  for (const Surface &surface : model.surfaces)
  {
    if (surface.outer.size() < 3 || areaOf(surface.outer) < .05)
    {
      fault(FaultKind::SurfaceDegenerate, surface.id, 0, surface.outer.empty() ? Position{} : surface.outer.front(), "fewer than three corners, or no area");

      continue;
    }

    bool tangled = false;

    for (size_t i = 0; i < surface.outer.size() && !tangled; ++i)
    {
      for (size_t j = i + 2; j < surface.outer.size(); ++j)
      {
        if (i == 0 && j + 1 == surface.outer.size())
        {
          continue;
        }

        if (crosses(surface.outer[i], surface.outer[(i + 1) % surface.outer.size()], surface.outer[j], surface.outer[(j + 1) % surface.outer.size()]))
        {
          tangled = true;

          break;
        }
      }
    }

    if (tangled)
    {
      fault(FaultKind::SurfaceDegenerate, surface.id, 0, surface.outer.front(), "the outline crosses itself");
    }
  }

  // ---- no two of them stand on each other ---------------------------------
  for (size_t i = 0; i < model.surfaces.size(); ++i)
  {
    for (size_t j = i + 1; j < model.surfaces.size(); ++j)
    {
      const Surface &u = model.surfaces[i];
      const Surface &v = model.surfaces[j];

      if (std::abs(u.height - v.height) > kSameHeight || u.outer.size() < 3 || v.outer.size() < 3)
      {
        continue;
      }

      std::vector<geom2::Polygon> shared;

      if (!geom2::booleanOp({geom2::toOperand(polygonOf(u)), geom2::toOperand(polygonOf(v))}, geom2::Operation::Intersection, {}, shared, nullptr))
      {
        continue;
      }

      double over = 0;

      for (const geom2::Polygon &piece : shared)
      {
        over += std::abs(geom2::areaMetres(piece));
      }

      // Touching along an edge is how two surfaces are joined; standing on
      // each other is a different thing and has an area.
      if (over > .25)
      {
        fault(FaultKind::SurfaceOverlaps, u.id, v.id, u.outer.front(), "two surfaces at one height cover " + std::to_string(over) + " m2 of the same ground");
      }
    }
  }

  // ---- the ramps land where they say they do ------------------------------
  for (const Ramp &ramp : model.ramps)
  {
    const Position foot = middleOf(ramp.footLeft, ramp.footRight);
    const Position head = middleOf(ramp.headLeft, ramp.headRight);

    const Surface *at = nullptr;
    const Surface *onto = nullptr;

    for (const Surface &surface : model.surfaces)
    {
      at = surface.id == ramp.from ? &surface : at;
      onto = surface.id == ramp.to ? &surface : onto;
    }

    if (at == nullptr || !standsOn(*at, foot, .35) || std::abs(at->height - foot.y) > .25)
    {
      fault(FaultKind::RampFootOffSurface, ramp.id, ramp.from, foot, "the foot of the ramp is not on the surface it claims to start from");
    }

    if (onto == nullptr || !standsOn(*onto, head, .35) || std::abs(onto->height - head.y) > .25)
    {
      fault(FaultKind::RampHeadOffSurface, ramp.id, ramp.to, head, "the head of the ramp is not on the surface it claims to reach");
    }

    const double rise = head.y - foot.y;
    const double going = std::hypot(head.x - foot.x, head.z - foot.z);

    if (going < 1e-6 || rise / going > .9)
    {
      fault(FaultKind::RampTooSteep, ramp.id, 0, foot, "it rises " + std::to_string(rise) + " m over " + std::to_string(going) + " m");
    }
  }

  // ---- and nothing passes through a surface that has no hole for it -------
  //
  // A ramp climbing to a balcony passes under every surface between the two,
  // and where it runs out of headroom that surface has to be open. This is
  // measured on the finished polygons: the hole either is there or it is not.
  for (const Ramp &ramp : model.ramps)
  {
    const Position foot = middleOf(ramp.footLeft, ramp.footRight);
    const Position head = middleOf(ramp.headLeft, ramp.headRight);

    for (const Surface &surface : model.surfaces)
    {
      if (surface.height < foot.y - .05 || surface.height > head.y + .05 || surface.outer.size() < 3)
      {
        continue;
      }

      for (int k = 0; k <= 12; ++k)
      {
        const double t = double(k) / 12.;
        const double x = foot.x + (head.x - foot.x) * t, z = foot.z + (head.z - foot.z) * t;

        double y = 0;

        if (!heightOver(ramp, x, z, y))
        {
          continue;
        }

        // Clear underneath is fine, and so is standing on it.
        //
        // The second half of that was `y - surface.height > .05`, which asks
        // whether the ramp is above the surface - and at the foot of every
        // ramp it is neither above nor below, it is *on* it, by exactly zero.
        // So every ramp in every sound stack was reported as piercing the
        // balcony it starts from, with nought metres of headroom. A ramp
        // resting on a surface is not passing through it; what passes through
        // is a ramp genuinely below one.
        const double under = surface.height - y;

        if (under > model.headroom - .01 || under < .05)
        {
          continue;
        }

        if (depthInto(surface, x, z) < -.05)
        {
          fault(FaultKind::RampPierces, ramp.id, surface.id, Position{x, surface.height, z}, "the ramp passes through this surface with " + std::to_string(surface.height - y) + " m of headroom and no hole cut for it");

          break;
        }
      }
    }
  }

  // ---- every door opens onto floor ----------------------------------------
  for (const Portal &portal : model.portals)
  {
    const Position stand{portal.at.x + portal.outX * portal.into, portal.at.y, portal.at.z + portal.outZ * portal.into};
    const Surface *on = nullptr;

    for (const Surface &surface : model.surfaces)
    {
      if (std::abs(surface.height - portal.at.y) < .25 && standsOn(surface, stand, -.02))
      {
        on = &surface;
      }
    }

    if (on == nullptr)
    {
      fault(FaultKind::PortalOffSurface, portal.id, 0, stand, "there is no floor outside this door");

      continue;
    }

    // ...and enough of it to stand on.
    std::vector<std::vector<Position>> pieces;
    walkableRegion(*on, model.clearWidth, pieces);

    bool room = false;

    for (const std::vector<Position> &piece : pieces)
    {
      room = room || signedDistance(piece, stand.x, stand.z) < model.clearWidth * .5;
    }

    if (!room)
    {
      fault(FaultKind::PortalPinched, portal.id, on->id, stand, "the floor outside this door is narrower than a person");
    }

    // ...and it opens onto floor, not onto the stair itself.
    //
    // The first version of this measured the distance from the door to the
    // *end* of a ramp and complained under a clear width. That is wrong, and
    // the tower is what proved it: a newel stair turns on a corner landing a
    // metre square, the door is on that landing, and there is nowhere on it
    // that is a metre from both flights. Being near a stair is the
    // arrangement; every door on every landing ever built is near one.
    //
    // What is actually wrong is standing *on* it - opening your front door and
    // putting a foot on a tread instead of on floor. That is a containment
    // test, it is exact, and it says nothing about towers.
    for (const Ramp &ramp : model.ramps)
    {
      if (std::abs(ramp.footLeft.y - portal.at.y) > model.headroom && std::abs(ramp.headLeft.y - portal.at.y) > model.headroom)
      {
        continue;
      }

      const std::vector<Position> plan = planOf(ramp);

      if (signedDistance(plan, stand.x, stand.z) < -.05)
      {
        fault(FaultKind::PortalOnRamp, portal.id, ramp.id, stand, "the floor outside this door is the ramp");
      }
    }
  }

  // ---- and you can walk from the ramp to the door -------------------------
  //
  // On each surface, every place that matters - the head of a ramp arriving,
  // the foot of one leaving, a door - has to be in one piece of the walkable
  // region. Two of them in different pieces is a balcony cut in half by its
  // own stairwell, which reads as perfectly ordinary in any drawing of it.
  for (const Surface &surface : model.surfaces)
  {
    if (surface.outer.size() < 3)
    {
      continue;
    }

    std::vector<Position> wants;

    for (const Ramp &ramp : model.ramps)
    {
      const Position foot = middleOf(ramp.footLeft, ramp.footRight);
      const Position head = middleOf(ramp.headLeft, ramp.headRight);

      if (std::abs(surface.height - foot.y) < .25 && standsOn(surface, foot, .35))
      {
        wants.push_back(foot);
      }

      if (std::abs(surface.height - head.y) < .25 && standsOn(surface, head, .35))
      {
        wants.push_back(head);
      }
    }

    for (const Portal &portal : model.portals)
    {
      const Position stand{portal.at.x + portal.outX * portal.into, portal.at.y, portal.at.z + portal.outZ * portal.into};

      if (std::abs(surface.height - portal.at.y) < .25 && standsOn(surface, stand, .05))
      {
        wants.push_back(stand);
      }
    }

    if (wants.size() < 2)
    {
      continue;
    }

    std::vector<std::vector<Position>> pieces;
    walkableRegion(surface, model.clearWidth, pieces);

    // Which piece of the walkable region each wanted point belongs to. A point
    // just outside one - the head of a ramp, which stops at the edge - counts
    // as on the nearest piece within half a width.
    std::vector<int32_t> which(wants.size(), -1);

    for (size_t i = 0; i < wants.size(); ++i)
    {
      double nearest = model.clearWidth;

      for (size_t k = 0; k < pieces.size(); ++k)
      {
        const double away = signedDistance(pieces[k], wants[i].x, wants[i].z);

        if (away < nearest)
        {
          nearest = away;
          which[i] = int32_t(k);
        }
      }
    }

    for (size_t i = 0; i < wants.size(); ++i)
    {
      if (which[i] < 0)
      {
        fault(FaultKind::NoWayThrough, surface.id, 0, wants[i], "nowhere on this surface is wide enough to stand here");

        continue;
      }

      if (which[i] != which[0])
      {
        fault(FaultKind::NoWayThrough, surface.id, 0, wants[i], "this surface is in more than one walkable piece, and the things on it are in different ones");
      }
    }
  }

  // ---- nothing walkable is inside the building ----------------------------
  //
  // A balcony that reaches through the wall, a ramp that passes across a
  // storey: both draw perfectly and both are somebody standing inside a solid
  // wall. The building's mass is a prism, so the question is whether a piece
  // of walking geometry is inside it - which is a containment test and has an
  // exact answer, unlike anything involving how a render looked.
  for (const Solid &solid : model.solids)
  {
    if (solid.footprint.size() < 3 || solid.top <= solid.base)
    {
      continue;
    }

    for (const Surface &surface : model.surfaces)
    {
      if (surface.height < solid.base + .05 || surface.height > solid.top - .05 || surface.outer.size() < 3)
      {
        continue;
      }

      // Not merely touching: a balcony stands against a wall by design and
      // its edge lies on the outline. What is wrong is area inside.
      std::vector<geom2::Polygon> shared;

      if (!geom2::booleanOp({geom2::toOperand(polygonOf(surface)), geom2::toOperand(ringOf(solid.footprint))}, geom2::Operation::Intersection, {}, shared, nullptr))
      {
        continue;
      }

      double inside = 0;

      for (const geom2::Polygon &piece : shared)
      {
        inside += std::abs(geom2::areaMetres(piece));
      }

      if (inside > .25)
      {
        fault(FaultKind::SurfaceInSolid, surface.id, solid.id, surface.outer.front(), std::to_string(inside) + " m2 of this surface is inside the building");
      }
    }

    for (const Ramp &ramp : model.ramps)
    {
      const Position foot = middleOf(ramp.footLeft, ramp.footRight);
      const Position head = middleOf(ramp.headLeft, ramp.headRight);

      for (int k = 0; k <= 12; ++k)
      {
        const double s = double(k) / 12.;
        const double x = foot.x + (head.x - foot.x) * s, z = foot.z + (head.z - foot.z) * s;
        const double y = foot.y + (head.y - foot.y) * s;

        if (y < solid.base + .05 || y > solid.top - .05)
        {
          continue;
        }

        if (signedDistance(solid.footprint, x, z) < -.1)
        {
          fault(FaultKind::RampInSolid, ramp.id, solid.id, Position{x, y, z}, "the ramp passes through the building's own mass");

          break;
        }
      }
    }
  }

  // ---- and every entrance is open -----------------------------------------
  //
  // One rule for doors and for the two ends of every ramp, because it is one
  // rule: an entrance is a line you cross, and nothing may stand on it. A rail
  // carried round the whole edge of a balcony is the commonest way to build a
  // stair you cannot get off - the geometry is faultless and the handrail is
  // across the top step.
  {
    const std::vector<Entrance> ways = entrances(model);

    for (const Entrance &way : ways)
    {
      for (const Barrier &barrier : model.barriers)
      {
        // Only what stands at this height: a rail one storey down is not in
        // the way of anything here.
        if (way.height < barrier.base - .1 || way.height > barrier.base + barrier.height + .1)
        {
          continue;
        }

        if (crosses(way.a, way.b, barrier.a, barrier.b))
        {
          fault(FaultKind::EntranceBlocked, way.of, barrier.id, middleOf(way.a, way.b), way.fromRamp ? "a barrier stands across the end of this ramp" : "a barrier stands across this doorway");

          continue;
        }

        // ...and one that stops a hair short of crossing is still across it.
        const double ex = way.b.x - way.a.x, ez = way.b.z - way.a.z;
        const double length = ex * ex + ez * ez;

        for (const Position &end : {barrier.a, barrier.b})
        {
          const double at = length > 1e-12 ? std::max(0., std::min(1., ((end.x - way.a.x) * ex + (end.z - way.a.z) * ez) / length)) : 0.;

          if (at > .05 && at < .95 && std::hypot(way.a.x + ex * at - end.x, way.a.z + ez * at - end.z) < .08)
          {
            fault(FaultKind::EntranceBlocked, way.of, barrier.id, end, "a barrier ends in the middle of this entrance");
          }
        }
      }
    }
  }

  // ---- every edge you could walk off has something on it ------------------
  //
  // The mirror of the rule above, and the two are one rule seen from both
  // sides: an entrance is open and everything else is closed. A balcony fenced
  // all the way round is a stair you cannot get off; a balcony fenced nowhere
  // is a shelf you walk off the end of. What decides each piece of edge is what
  // is on the other side of it - another surface, the building, or nothing.
  {
    const std::vector<Entrance> ways = entrances(model);

    for (const Surface &surface : model.surfaces)
    {
      if (surface.outer.size() < 3 || surface.kind == SurfaceKind::Ground)
      {
        continue;
      }

      std::vector<const std::vector<Position> *> rings{&surface.outer};

      for (const std::vector<Position> &hole : surface.holes)
      {
        rings.push_back(&hole);
      }

      for (const std::vector<Position> *ring : rings)
      {
        for (size_t i = 0; i < ring->size(); ++i)
        {
          const Position &a = (*ring)[i];
          const Position &b = (*ring)[(i + 1) % ring->size()];
          const double ex = b.x - a.x, ez = b.z - a.z;
          const double length = std::hypot(ex, ez);

          if (length < .3)
          {
            continue;
          }

          const int pieces = std::max(1, int(std::ceil(length / .5)));

          for (int k = 0; k < pieces; ++k)
          {
            const double mid = length * (double(k) + .5) / double(pieces);
            const double mx = a.x + ex * (mid / length), mz = a.z + ez * (mid / length);

            // A hand's breadth beyond the edge, on the far side of it.
            const double nx = ez / length * .2, nz = -ex / length * .2;
            const double outX = depthInto(surface, mx + nx, mz + nz) > 0 ? nx : -nx;
            const double outZ = depthInto(surface, mx + nx, mz + nz) > 0 ? nz : -nz;
            const double px = mx + outX, pz = mz + outZ;

            // Another surface at this height carries on here.
            bool closed = false;

            for (const Surface &other : model.surfaces)
            {
              closed = closed || (other.id != surface.id && std::abs(other.height - surface.height) < kSameHeight && depthInto(other, px, pz) < 0);
            }

            // ...or the building stands there.
            for (const Solid &solid : model.solids)
            {
              closed = closed || (surface.height > solid.base - .05 && surface.height < solid.top + .05 && signedDistance(solid.footprint, px, pz) < .05);
            }

            // ...or a ramp arrives here, or a door opens here.
            for (const Entrance &way : ways)
            {
              if (std::abs(way.height - surface.height) > .25)
              {
                continue;
              }

              const double wx = way.b.x - way.a.x, wz = way.b.z - way.a.z;
              const double span = wx * wx + wz * wz;
              const double t = span > 1e-12 ? std::max(0., std::min(1., ((mx - way.a.x) * wx + (mz - way.a.z) * wz) / span)) : 0.;

              closed = closed || std::hypot(way.a.x + wx * t - mx, way.a.z + wz * t - mz) < model.clearWidth;
            }

            // ...or something has been built on it.
            for (const Barrier &barrier : model.barriers)
            {
              if (surface.height < barrier.base - .1 || surface.height > barrier.base + barrier.height + .1)
              {
                continue;
              }

              const double bx = barrier.b.x - barrier.a.x, bz = barrier.b.z - barrier.a.z;
              const double span = bx * bx + bz * bz;
              const double t = span > 1e-12 ? std::max(0., std::min(1., ((mx - barrier.a.x) * bx + (mz - barrier.a.z) * bz) / span)) : 0.;

              closed = closed || std::hypot(barrier.a.x + bx * t - mx, barrier.a.z + bz * t - mz) < .3;
            }

            if (!closed)
            {
              fault(FaultKind::UnguardedEdge, surface.id, 0, Position{mx, surface.height, mz}, "this piece of edge is a drop with nothing on it");
            }
          }
        }
      }
    }
  }

  // ---- and every door can be walked to from the ground --------------------
  {
    const std::vector<bool> seen = reachable(model);

    for (const Portal &portal : model.portals)
    {
      const Position stand{portal.at.x + portal.outX * portal.into, portal.at.y, portal.at.z + portal.outZ * portal.into};
      bool arrived = false;

      for (size_t i = 0; i < model.surfaces.size(); ++i)
      {
        arrived = arrived || (seen[i] && std::abs(model.surfaces[i].height - portal.at.y) < .25 && standsOn(model.surfaces[i], stand, .05));
      }

      if (!arrived)
      {
        fault(FaultKind::Unreachable, portal.id, 0, stand, "no walk from the ground reaches this door");
      }
    }
  }

  return faults;
}

std::string toJson(const Model &model, const std::vector<Fault> &faults)
{
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(4);

  const auto ring = [&](const std::vector<Position> &points) {
    out << "[";

    for (size_t i = 0; i < points.size(); ++i)
    {
      out << "[" << points[i].x << "," << points[i].y << "," << points[i].z << "]" << (i + 1 < points.size() ? "," : "");
    }

    out << "]";
  };

  static const char *kinds[] = {"ground", "storey", "balcony", "landing"};

  out << "{\n  \"surfaces\": [\n";

  for (size_t i = 0; i < model.surfaces.size(); ++i)
  {
    const Surface &surface = model.surfaces[i];
    out << "    {\"id\": " << surface.id << ", \"kind\": \"" << kinds[int(surface.kind)] << "\", \"height\": " << surface.height << ", \"note\": \"" << surface.note << "\", \"outer\": ";
    ring(surface.outer);
    out << ", \"holes\": [";

    for (size_t h = 0; h < surface.holes.size(); ++h)
    {
      ring(surface.holes[h]);
      out << (h + 1 < surface.holes.size() ? "," : "");
    }

    out << "]}" << (i + 1 < model.surfaces.size() ? ",\n" : "\n");
  }

  out << "  ],\n  \"ramps\": [\n";

  for (size_t i = 0; i < model.ramps.size(); ++i)
  {
    const Ramp &ramp = model.ramps[i];
    out << "    {\"id\": " << ramp.id << ", \"from\": " << ramp.from << ", \"to\": " << ramp.to << ", \"quad\": ";
    ring({ramp.footLeft, ramp.footRight, ramp.headRight, ramp.headLeft});
    out << "}" << (i + 1 < model.ramps.size() ? ",\n" : "\n");
  }

  out << "  ],\n  \"portals\": [\n";

  for (size_t i = 0; i < model.portals.size(); ++i)
  {
    const Portal &portal = model.portals[i];
    out << "    {\"id\": " << portal.id << ", \"at\": [" << portal.at.x << "," << portal.at.y << "," << portal.at.z << "], \"out\": [" << portal.outX << "," << portal.outZ << "], \"into\": " << portal.into << "}" << (i + 1 < model.portals.size() ? ",\n" : "\n");
  }

  out << "  ],\n  \"solids\": [\n";

  for (size_t i = 0; i < model.solids.size(); ++i)
  {
    const Solid &solid = model.solids[i];
    out << "    {\"id\": " << solid.id << ", \"base\": " << solid.base << ", \"top\": " << solid.top << ", \"footprint\": ";
    ring(solid.footprint);
    out << "}" << (i + 1 < model.solids.size() ? ",\n" : "\n");
  }

  out << "  ],\n  \"barriers\": [\n";

  for (size_t i = 0; i < model.barriers.size(); ++i)
  {
    const Barrier &barrier = model.barriers[i];
    out << "    {\"id\": " << barrier.id << ", \"a\": [" << barrier.a.x << "," << barrier.a.z << "], \"b\": [" << barrier.b.x << "," << barrier.b.z << "], \"base\": " << barrier.base << ", \"height\": " << barrier.height << ", \"thickness\": " << barrier.thickness << ", \"wall\": " << (barrier.wall ? "true" : "false") << "}" << (i + 1 < model.barriers.size() ? ",\n" : "\n");
  }

  // ...and the whole of it as triangles, so anything that draws it draws what
  // was checked rather than its own idea of the same thing.
  out << "  ],\n  \"faces\": [\n";

  std::vector<Face> faces;
  triangulate(model, faces, nullptr);

  static const char *facesOf[] = {"surface", "ramp", "barrier", "solid", "portal"};

  for (size_t i = 0; i < faces.size(); ++i)
  {
    const Face &face = faces[i];
    out << "    {\"kind\": \"" << facesOf[int(face.kind)] << "\", \"of\": " << face.of << ", \"tri\": ";
    ring({face.a, face.b, face.c});
    out << "}" << (i + 1 < faces.size() ? ",\n" : "\n");
  }

  out << "  ],\n  \"faults\": [\n";

  for (size_t i = 0; i < faults.size(); ++i)
  {
    out << "    {\"kind\": \"" << nameOf(faults[i].kind) << "\", \"a\": " << faults[i].a << ", \"b\": " << faults[i].b << ", \"where\": [" << faults[i].where.x << "," << faults[i].where.y << "," << faults[i].where.z << "], \"what\": \"" << faults[i].what << "\"}" << (i + 1 < faults.size() ? ",\n" : "\n");
  }

  out << "  ]\n}\n";

  return out.str();
}

// ---------------------------------------------------------------------------
// The stack.
// ---------------------------------------------------------------------------

bool planStack(const StackBrief &b, Model &out, std::string *error)
{
  out = Model{};
  out.clearWidth = b.clearWidth;
  out.headroom = b.headroom;

  const double run = std::hypot(b.wallTo.x - b.wallFrom.x, b.wallTo.z - b.wallFrom.z);

  if (b.floors < 2u || run < 1e-3 || b.rampWidth < .4 || b.storeyHeight < 1.)
  {
    if (error)
    {
      *error = "a stack needs at least two floors and a wall to hang off";
    }

    return false;
  }

  const double ax = (b.wallTo.x - b.wallFrom.x) / run, az = (b.wallTo.z - b.wallFrom.z) / run;

  // The building is on the left of the wall as it runs, so out is to the right.
  const double ox = az, oz = -ax;

  const double W = b.rampWidth;
  const double deep = W * 2;

  const auto at = [&](const double along, const double outward, const double height) {
    return Position{b.wallFrom.x + ax * along + ox * outward, height, b.wallFrom.z + az * along + oz * outward};
  };

  // How long a ramp has to be to climb a storey at the steepest slope allowed,
  // and whether the wall is long enough to hold one with a landing at each end.
  //
  // The landing is not decoration: it is where you stand to start climbing and
  // where you arrive, and a ramp that reaches the very corner of the building
  // has neither. A stride at each end is the least that is a place rather than
  // an edge.
  const double stride = std::max(b.clearWidth, .9);
  const double going = b.storeyHeight / std::max(b.maximumSlope, .05);

  if (going + stride * 2 > run)
  {
    if (error)
    {
      *error = "the wall is " + std::to_string(run) + " m and a ramp climbing " + std::to_string(b.storeyHeight) + " m needs " + std::to_string(going + stride * 2) + " m";
    }

    return false;
  }

  uint32_t nextSurface = 1, nextRamp = 1, nextPortal = 1;

  // ---- the ground ----------------------------------------------------------
  //
  // A strip in front of the wall, as deep as a balcony, so a ramp coming down
  // to it has somewhere to land whichever end it comes down at.
  {
    Surface ground;
    ground.id = nextSurface++;
    ground.kind = SurfaceKind::Ground;
    ground.height = b.ground;
    ground.storey = 0;
    ground.note = "the ground outside the wall";
    ground.outer = {at(-stride, 0, b.ground), at(run + stride, 0, b.ground), at(run + stride, deep, b.ground), at(-stride, deep, b.ground)};

    out.surfaces.push_back(std::move(ground));
  }

  const uint32_t groundId = out.surfaces.front().id;

  // ---- a balcony per floor above the ground, and a ramp up to each ---------
  //
  // The ramps alternate direction, which is what makes the arrangement a Z:
  // you arrive near one end of a balcony, walk its length, and leave from the
  // other end for the floor above. The balcony is the landing for the ramp
  // above it, so nothing here needs a landing of its own.
  std::vector<uint32_t> balconies;

  for (uint32_t floor = 1; floor < b.floors; ++floor)
  {
    const double height = b.ground + double(floor) * b.storeyHeight;
    const double below = height - b.storeyHeight;
    const bool forward = (floor % 2u) == 1u;

    const double foot = forward ? stride : run - stride;
    const double head = forward ? foot + going : foot - going;

    // The hole the ramp comes up through.
    //
    // Only where the ramp has run out of headroom under the balcony, which is
    // the last stretch of it. Cutting the whole plan of the ramp - which is
    // what a first version does - takes out floor that has two and a half
    // metres of clear air under it, and leaves a hole you cannot use with the
    // ramp nowhere near it.
    const double clearAt = b.storeyHeight > b.headroom ? going * (1. - b.headroom / b.storeyHeight) : 0.;
    const double wellLength = std::max(going - clearAt, W);
    const double wellFrom = forward ? head - wellLength : head;
    const double wellTo = wellFrom + wellLength;

    Surface balcony;
    balcony.id = nextSurface++;
    balcony.kind = SurfaceKind::Balcony;
    balcony.height = height;
    balcony.storey = floor;
    balcony.note = "balcony to floor " + std::to_string(floor);

    // The band, full length, with the stairwell taken out of its outer half.
    // A hole rather than a notch in the outline, so the walkable region is a
    // ring round it and the erosion has something to say about whether you can
    // get past.
    balcony.outer = {at(0, 0, height), at(run, 0, height), at(run, deep, height), at(0, deep, height)};

    // Where the well meets the outer edge it is a bite, not a hole: a hole
    // that touches the boundary is not a hole. Kept a hair inside instead.
    const double lip = std::min(.12, W * .1);

    if (wellFrom > lip && wellTo < run - lip)
    {
      balcony.holes.push_back({at(wellFrom, W, height), at(wellFrom, deep - lip, height), at(wellTo, deep - lip, height), at(wellTo, W, height)});
    }
    else
    {
      // Against an end of the wall, so the well opens onto it: take the bite
      // out of the outline instead of cutting a hole.
      const double from = std::max(wellFrom, 0.);
      const double to = std::min(wellTo, run);

      balcony.outer = {at(0, 0, height), at(run, 0, height), at(run, deep, height), at(to, deep, height), at(to, W, height), at(from, W, height), at(from, deep, height), at(0, deep, height)};
    }

    balconies.push_back(balcony.id);
    out.surfaces.push_back(std::move(balcony));

    // The ramp, in the outer half of the band.
    Ramp ramp;
    ramp.id = nextRamp++;
    ramp.from = floor == 1u ? groundId : balconies[floor - 2u];
    ramp.to = balconies.back();
    ramp.note = "floor " + std::to_string(floor - 1u) + " to " + std::to_string(floor);
    ramp.footLeft = at(foot, W, below);
    ramp.footRight = at(foot, deep, below);
    ramp.headLeft = at(head, W, height);
    ramp.headRight = at(head, deep, height);

    out.ramps.push_back(std::move(ramp));

    // The door onto it: on the wall, in the half against it, as far from the
    // well as the wall allows.
    // Clear of the stairwell *and* of the foot of the ramp leaving for the
    // floor above.
    //
    // Furthest from the well alone was the first rule, and the far end of a
    // balcony is precisely where the next ramp starts - so every door was put
    // at the bottom of the flight out of its own storey. The two are at
    // opposite ends, so what is furthest from both is the middle, which is
    // also where a door belongs.
    const double nextFoot = floor + 1u < b.floors ? (((floor + 1u) % 2u) == 1u ? stride : run - stride) : -1e9;

    double doorAt = run * .5;
    double clearest = -1;

    for (int k = 1; k < 40; ++k)
    {
      const double along = run * double(k) / 40.;

      if (along < stride || along > run - stride)
      {
        continue;
      }

      const double fromWell = (along > wellFrom && along < wellTo) ? 0. : std::min(std::abs(along - wellFrom), std::abs(along - wellTo));
      const double fromFoot = std::abs(along - nextFoot);
      const double clear = std::min(fromWell, fromFoot);

      if (clear > clearest)
      {
        clearest = clear;
        doorAt = along;
      }
    }

    Portal door;
    door.id = nextPortal++;
    door.at = at(doorAt, 0, height);
    door.into = W * .5;
    door.outX = ox;
    door.outZ = oz;
    door.storey = floor;
    door.note = "front door, floor " + std::to_string(floor);

    out.portals.push_back(std::move(door));
  }

  // ---- the building the whole thing hangs off ------------------------------
  //
  // Declared rather than implied, so the checks have something to measure
  // against: a balcony reaching through the wall and a ramp crossing a storey
  // are both invisible until there is a mass to be inside of.
  {
    Solid building;
    building.id = 1;
    building.base = b.ground;
    building.top = b.ground + double(b.floors) * b.storeyHeight;
    building.note = "the building";

    // Behind the wall, which is the side the balconies are not on.
    const double back = deep * 3;
    building.footprint = {at(0, 0, b.ground), at(0, -back, b.ground), at(run, -back, b.ground), at(run, 0, b.ground)};

    out.solids.push_back(std::move(building));
  }

  // ---- the rails ----------------------------------------------------------
  //
  // Round the outer edge of every balcony, and off it at both ends, because
  // both ends are where a ramp meets it. Left whole it is a handrail across
  // the top step - the geometry faultless, the stair unusable - which is
  // exactly what the entrance check is for.
  {
    uint32_t nextBarrier = 1;

    for (const Surface &surface : out.surfaces)
    {
      if (surface.kind != SurfaceKind::Balcony)
      {
        continue;
      }

      // The outline and every hole in it. The stairwell is a drop like any
      // other edge - a balcony railed round its outside and open round its own
      // well is a balcony you walk into a hole in.
      std::vector<const std::vector<Position> *> rings{&surface.outer};

      for (const std::vector<Position> &hole : surface.holes)
      {
        rings.push_back(&hole);
      }

      for (const std::vector<Position> *ring : rings)
      {
      for (size_t i = 0; i < ring->size(); ++i)
      {
        const Position &p = (*ring)[i];
        const Position &q = (*ring)[(i + 1) % ring->size()];

        // The edge against the wall is the wall; it needs no rail.
        const double outwardP = (p.x - b.wallFrom.x) * ox + (p.z - b.wallFrom.z) * oz;
        const double outwardQ = (q.x - b.wallFrom.x) * ox + (q.z - b.wallFrom.z) * oz;

        if (outwardP < .05 && outwardQ < .05)
        {
          continue;
        }

        // Split at every entrance that lands on this edge, and leave that
        // stretch out.
        const double ex = q.x - p.x, ez = q.z - p.z;
        const double length = std::hypot(ex, ez);

        if (length < .2)
        {
          continue;
        }

        // Work at hand-width resolution.  The earlier coarse .4 m sampling
        // plus a full clear-width halo removed metres of rail around a one
        // metre stair, leaving visible fall gaps beside otherwise valid
        // entrances.  Only the ramp's actual end edge (plus one hand-width)
        // is an opening; everything else is guard rail.
        const int pieces = std::max(1, int(std::ceil(length / .1)));
        double openedAt = -1;

        for (int k = 0; k <= pieces; ++k)
        {
          const double along = length * double(k) / double(pieces);
          const double mid = length * (double(k) + .5) / double(pieces);
          const double mx = p.x + ex * (mid / length), mz = p.z + ez * (mid / length);

          bool clear = k < pieces;

          for (const Ramp &ramp : out.ramps)
          {
            for (const std::pair<Position, Position> &edge : {std::make_pair(ramp.footLeft, ramp.footRight), std::make_pair(ramp.headLeft, ramp.headRight)})
            {
              if (std::abs(edge.first.y - surface.height) > .25)
              {
                continue;
              }

              const double fx = edge.second.x - edge.first.x, fz = edge.second.z - edge.first.z;
              const double span = fx * fx + fz * fz;
              const double t = span > 1e-12 ? std::max(0., std::min(1., ((mx - edge.first.x) * fx + (mz - edge.first.z) * fz) / span)) : 0.;

              clear = clear && std::hypot(edge.first.x + fx * t - mx, edge.first.z + fz * t - mz) > .08;
            }
          }

          if (clear && openedAt < 0)
          {
            openedAt = along;
          }

          if (!clear && openedAt >= 0)
          {
            if (along - openedAt > .08)
            {
              Barrier rail;
              rail.id = nextBarrier++;
              rail.a = Position{p.x + ex * (openedAt / length), surface.height, p.z + ez * (openedAt / length)};
              rail.b = Position{p.x + ex * (along / length), surface.height, p.z + ez * (along / length)};
              rail.base = surface.height;
              rail.height = 1.0;
              rail.wall = false;
              rail.note = "balcony rail";

              out.barriers.push_back(std::move(rail));
            }

            openedAt = -1;
          }
        }
      }
      }
    }

    // Continue the guard down both exposed sides of every flight.  These are
    // parallel to the direction of travel, so they close the fall edge without
    // blocking the foot or head entrance.
    for (const Ramp &ramp : out.ramps)
    {
      for (const auto &side : {std::make_pair(ramp.footLeft, ramp.headLeft),
                               std::make_pair(ramp.footRight, ramp.headRight)})
      {
        Barrier rail;
        rail.id = nextBarrier++; rail.a = side.first; rail.b = side.second;
        rail.base = std::min(side.first.y, side.second.y); rail.height = 1.;
        rail.wall = false; rail.note = "sloped stair rail";
        out.barriers.push_back(std::move(rail));
      }
    }
  }

  // ...and the ground floor's own door, which needs no ramp.
  {
    Portal door;
    door.id = nextPortal++;
    door.at = at(run * .5, 0, b.ground);
    door.into = W * .5;
    door.outX = ox;
    door.outZ = oz;
    door.storey = 0;
    door.note = "front door, ground floor";

    out.portals.push_back(std::move(door));
  }

  return true;
}

// ---------------------------------------------------------------------------
// The tower.
// ---------------------------------------------------------------------------

bool planTower(const TowerBrief &b, Model &out, std::string *error)
{
  out = Model{};
  out.clearWidth = b.clearWidth;
  out.headroom = b.headroom;

  const double W = b.rampWidth;

  // The tread is measured in from the wall and what is left in the middle is
  // the well. A shaft with no well has nowhere for the flights to wind around -
  // each one would end where the next begins, at a point, and the landings
  // would be on top of each other.
  const double well = b.side - W * 2;

  if (b.floors < 2u || b.side < 2.4 || W < .6 || well < W * .5)
  {
    if (error)
    {
      *error = "a shaft of " + std::to_string(b.side) + " m leaves a well of " + std::to_string(well) + " m, which is not a well";
    }

    return false;
  }

  const double half = b.side * .5;

  // The four corners of the inside of the shaft, counter clockwise seen from
  // above, and the landing squares that sit in them.
  const Position corner[4] = {
    Position{b.centre.x - half, b.ground, b.centre.z - half},
    Position{b.centre.x + half, b.ground, b.centre.z - half},
    Position{b.centre.x + half, b.ground, b.centre.z + half},
    Position{b.centre.x - half, b.ground, b.centre.z + half}};

  uint32_t nextSurface = 1, nextRamp = 1, nextPortal = 1;

  // A landing in a corner: a square of the tread's width, tucked into it.
  const auto landingAt = [&](const int which, const double height, const uint32_t storey, const bool arrival) {
    const Position &at = corner[which];
    const double towardsX = at.x < b.centre.x ? 1. : -1.;
    const double towardsZ = at.z < b.centre.z ? 1. : -1.;

    Surface landing;
    landing.id = nextSurface++;
    landing.kind = arrival ? SurfaceKind::Landing : SurfaceKind::Landing;
    landing.height = height;
    landing.storey = storey;
    landing.note = arrival ? "the landing at floor " + std::to_string(storey) : "a turn";
    landing.outer = {
      Position{at.x, height, at.z},
      Position{at.x + towardsX * W, height, at.z},
      Position{at.x + towardsX * W, height, at.z + towardsZ * W},
      Position{at.x, height, at.z + towardsZ * W}};

    // Counter clockwise, whichever corner it is.
    if (areaOf(landing.outer) > 0 && !geom2::counterClockwise(ringOf(landing.outer)))
    {
      std::reverse(landing.outer.begin(), landing.outer.end());
    }

    return landing;
  };

  // The ground inside the shaft: the whole square, at the bottom.
  {
    Surface floorOfIt;
    floorOfIt.id = nextSurface++;
    floorOfIt.kind = SurfaceKind::Ground;
    floorOfIt.height = b.ground;
    floorOfIt.storey = 0;
    floorOfIt.note = "the floor of the shaft";
    floorOfIt.outer = {corner[0], corner[1], corner[2], corner[3]};

    out.surfaces.push_back(std::move(floorOfIt));
  }

  uint32_t below = out.surfaces.front().id;

  // Four flights to a storey, one to a side, each climbing a quarter of it and
  // turning at the corner. That is the whole of a newel stair.
  const double climb = b.storeyHeight * .25;

  for (uint32_t floor = 1; floor < b.floors; ++floor)
  {
    for (int leg = 0; leg < 4; ++leg)
    {
      const int from = leg;
      const int to = (leg + 1) % 4;

      const double footHeight = b.ground + double(floor - 1u) * b.storeyHeight + climb * double(leg);
      const double headHeight = footHeight + climb;

      // The landing this leg arrives on: the next corner round.
      const bool arrival = leg == 3;
      Surface landing = landingAt(to, headHeight, arrival ? floor : floor - 1u, arrival);
      const uint32_t landingId = landing.id;
      out.surfaces.push_back(std::move(landing));

      // The flight itself, hugging the side between the two corners: from the
      // outer edge of the landing it leaves to the outer edge of the one it
      // arrives at.
      const double ax = corner[to].x - corner[from].x, az = corner[to].z - corner[from].z;
      const double side = std::hypot(ax, az);
      const double ux = ax / side, uz = az / side;

      // Inward, towards the middle of the shaft.
      const double inX = (b.centre.x - (corner[from].x + corner[to].x) * .5), inZ = (b.centre.z - (corner[from].z + corner[to].z) * .5);
      const double inLength = std::hypot(inX, inZ);
      const double nx = inX / inLength, nz = inZ / inLength;

      Ramp flight;
      flight.id = nextRamp++;
      flight.from = below;
      flight.to = landingId;
      flight.note = "floor " + std::to_string(floor - 1u) + ", leg " + std::to_string(leg + 1);
      flight.footLeft = Position{corner[from].x + ux * W, footHeight, corner[from].z + uz * W};
      flight.footRight = Position{flight.footLeft.x + nx * W, footHeight, flight.footLeft.z + nz * W};
      flight.headLeft = Position{corner[to].x - ux * W, headHeight, corner[to].z - uz * W};
      flight.headRight = Position{flight.headLeft.x + nx * W, headHeight, flight.headLeft.z + nz * W};

      out.ramps.push_back(std::move(flight));

      below = landingId;
    }

    // The door out of the shaft, on the landing this storey arrived at.
    if (b.doorsAtLandings)
    {
      const Surface &landing = out.surfaces.back();
      double cx = 0, cz = 0;

      for (const Position &p : landing.outer)
      {
        cx += p.x / double(landing.outer.size());
        cz += p.z / double(landing.outer.size());
      }

      // Out through the nearest face of the shaft.
      const double awayX = cx - b.centre.x, awayZ = cz - b.centre.z;
      const bool throughX = std::abs(awayX) > std::abs(awayZ);
      const double outX = throughX ? (awayX > 0 ? 1. : -1.) : 0.;
      const double outZ = throughX ? 0. : (awayZ > 0 ? 1. : -1.);

      Portal door;
      door.id = nextPortal++;
      door.at = Position{throughX ? b.centre.x + outX * half : cx, landing.height, throughX ? cz : b.centre.z + outZ * half};
      door.into = -W * .4; // Inward: the landing is inside the shaft.
      door.outX = outX;
      door.outZ = outZ;
      door.storey = floor;
      door.note = "out of the shaft at floor " + std::to_string(floor);

      out.portals.push_back(std::move(door));
    }
  }

  // ---- the shaft, and a rail round the well --------------------------------
  //
  // The four walls are what a landing's outer edges stand against, and the well
  // in the middle is what its inner edges look into. Without either, every
  // landing in the tower is a square of floor with four drops off it - which is
  // what the unguarded-edge check said about a shaft wide enough that its
  // landings do not touch each other.
  {
    uint32_t nextBarrier = 1;
    const double top = b.ground + double(b.floors) * b.storeyHeight;

    for (int i = 0; i < 4; ++i)
    {
      Barrier wall;
      wall.id = nextBarrier++;
      wall.a = corner[i];
      wall.b = corner[(i + 1) % 4];
      wall.base = b.ground;
      wall.height = top - b.ground;
      wall.wall = true;
      wall.thickness = .2;
      wall.note = "the shaft";

      out.barriers.push_back(std::move(wall));
    }

    // ...and the inner edge of every landing, which is the well.
    //
    // Every piece of it but the stretches a flight meets: the same rule the
    // balconies follow, and the reason a newel stair has a rail round its well
    // and an opening at each turn.
    for (const Surface &landing : out.surfaces)
    {
      if (landing.kind != SurfaceKind::Landing || landing.outer.size() < 3)
      {
        continue;
      }

      for (size_t i = 0; i < landing.outer.size(); ++i)
      {
        const Position &p = landing.outer[i];
        const Position &q = landing.outer[(i + 1) % landing.outer.size()];
        const double mx = (p.x + q.x) * .5, mz = (p.z + q.z) * .5;

        // Only the edges facing the middle: the others are the shaft.
        if (std::max(std::abs(mx - b.centre.x), std::abs(mz - b.centre.z)) > half - W + .05)
        {
          continue;
        }

        const double ex = q.x - p.x, ez = q.z - p.z;
        const double length = std::hypot(ex, ez);

        if (length < .2)
        {
          continue;
        }

        const int pieces = std::max(1, int(std::ceil(length / .35)));
        double openedAt = -1;

        for (int k = 0; k <= pieces; ++k)
        {
          const double along = length * double(k) / double(pieces);
          const double mid = length * (double(k) + .5) / double(pieces);
          const double sx = p.x + ex * (mid / length), sz = p.z + ez * (mid / length);

          bool clear = k < pieces;

          for (const Ramp &ramp : out.ramps)
          {
            for (const std::pair<Position, Position> &edge : {std::make_pair(ramp.footLeft, ramp.footRight), std::make_pair(ramp.headLeft, ramp.headRight)})
            {
              if (std::abs(edge.first.y - landing.height) > .2)
              {
                continue;
              }

              const double fx = edge.second.x - edge.first.x, fz = edge.second.z - edge.first.z;
              const double span = fx * fx + fz * fz;
              const double t = span > 1e-12 ? std::max(0., std::min(1., ((sx - edge.first.x) * fx + (sz - edge.first.z) * fz) / span)) : 0.;

              clear = clear && std::hypot(edge.first.x + fx * t - sx, edge.first.z + fz * t - sz) > b.clearWidth + .3;
            }
          }

          if (clear && openedAt < 0)
          {
            openedAt = along;
          }

          if (!clear && openedAt >= 0)
          {
            if (along - openedAt > .2)
            {
              Barrier rail;
              rail.id = nextBarrier++;
              rail.a = Position{p.x + ex * (openedAt / length), landing.height, p.z + ez * (openedAt / length)};
              rail.b = Position{p.x + ex * (along / length), landing.height, p.z + ez * (along / length)};
              rail.base = landing.height;
              rail.height = 1.0;
              rail.wall = false;
              rail.note = "the rail round the well";

              out.barriers.push_back(std::move(rail));
            }

            openedAt = -1;
          }
        }
      }
    }
  }

  return true;
}

} // namespace procedural::world::generator::access
