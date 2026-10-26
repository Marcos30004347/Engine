#include "procedural/world/generator/FortressJunction.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world::generator {

namespace {

using rewrite::Position;
constexpr double pi = 3.14159265358979323846;

double cross(const double ax, const double az, const double bx, const double bz) { return ax * bz - az * bx; }

double area(const std::vector<Position> &ring)
{
  double twice = 0;
  for (size_t k = 0; k < ring.size(); ++k)
  {
    const Position &a = ring[k], &b = ring[(k + 1) % ring.size()];
    twice += a.x * b.z - b.x * a.z;
  }
  return twice * .5;
}

Position average(const std::vector<Position> &ring)
{
  Position p{};
  for (const Position &q : ring) { p.x += q.x; p.y += q.y; p.z += q.z; }
  if (!ring.empty()) { p.x /= ring.size(); p.y /= ring.size(); p.z /= ring.size(); }
  return p;
}

bool polygonTowerAccess(const FortressJunctionBrief &brief,
                        const std::vector<Position> &tower,
                        const std::vector<uint32_t> &doorFaces,
                        access::Model &out,
                        std::string *error)
{
  out = access::Model{};
  const uint32_t n = uint32_t(tower.size());
  if (n < 3 || brief.floors < 2) return true;
  const double W = brief.stairWidth;
  const double storey = brief.height / double(brief.floors);
  const Position centre{brief.centre.x, brief.base, brief.centre.z};
  const double side = std::hypot(tower[1].x - tower[0].x, tower[1].z - tower[0].z);
  if (side <= W + .5)
  {
    if (error) *error = "the tower faces are too short for one stair-width flight";
    return false;
  }

  uint32_t nextSurface = 1, nextRamp = 1, nextPortal = 1, nextBarrier = 1;
  // A person on a tower stair is as wide as the stair lets them be.
  // A person on a tower stair is as wide as the stair lets them be: the
  // flights are W wide, and the turning kites at the corners of an obtuse
  // polygon are narrower still.
  out.clearWidth = std::min(.9, W * .45);
  access::Surface ground;
  ground.id = nextSurface++; ground.kind = access::SurfaceKind::Ground;
  ground.height = brief.base; ground.outer = tower; ground.note = "tower ground";
  out.surfaces.push_back(ground);
  uint32_t below = ground.id;

  struct CornerTurn { Position incomingOuter, outgoingOuter, inner; };
  const auto turnAt = [&](const uint32_t vertex, const double y) {
    const Position &before = tower[(vertex + n - 1) % n], &at = tower[vertex], &after = tower[(vertex + 1) % n];
    const double pux = (at.x-before.x)/side, puz = (at.z-before.z)/side;
    const double vux = (after.x-at.x)/side, vuz = (after.z-at.z)/side;
    const double pnx = -puz, pnz = pux, vnx = -vuz, vnz = vux;
    const Position p{at.x + pnx*W, y, at.z + pnz*W};
    const Position q{at.x + vnx*W, y, at.z + vnz*W};
    const double den = cross(pux, puz, vux, vuz);
    const double t = cross(q.x-p.x, q.z-p.z, vux, vuz) / den;
    const Position inner{p.x + pux*t, y, p.z + puz*t};
    return CornerTurn{{inner.x-pnx*W, y, inner.z-pnz*W},
                      {inner.x-vnx*W, y, inner.z-vnz*W}, inner};
  };

  // A door from the wall walk needs level ground the width of a face to
  // open onto, and a corner's turning kite is nowhere near that. So the
  // landing at a door spans two faces - the door's and the one before it -
  // and the legs between landings share the storey's rise. The revolution
  // starts at the first such landing, so a floor's own landing is where the
  // first door is; a second door on the far side meets its landing part of
  // a storey higher, which is what a spiral is.
  std::vector<bool> landingStart(n, false);
  for (const uint32_t face : doorFaces)
  {
    if (face >= n) continue;
    const uint32_t v = (face + n - 1) % n;
    bool clash = false;
    for (int d = -2; d <= 2; ++d) clash = clash || landingStart[(v + n + d) % n];
    if (!clash) landingStart[v] = true;
  }
  uint32_t doubles = 0;
  for (const bool b : landingStart) doubles += b ? 1 : 0;
  if (2 * doubles + 2 > n)
  {
    if (error) *error = "more wall-walk doors than the tower has faces for";
    return false;
  }
  const uint32_t legs = n - 2 * doubles;
  const double riseEach = storey / double(legs);
  uint32_t s0 = 0;
  for (uint32_t v = 0; v < n; ++v) if (landingStart[v]) { s0 = v; break; }

  const auto at = [&](const uint32_t v, const double y) { return Position{tower[v % n].x, y, tower[v % n].z}; };
  const auto push = [&](access::Surface landing) {
    if (area(landing.outer) < 0) std::reverse(landing.outer.begin(), landing.outer.end());
    const uint32_t id = landing.id;
    out.surfaces.push_back(std::move(landing));
    return id;
  };
  const auto doubleLanding = [&](const uint32_t v, const double y, const uint32_t storeyIndex, const char *note) {
    const CornerTurn in = turnAt(v, y), mid = turnAt((v + 1) % n, y), far = turnAt((v + 2) % n, y);
    access::Surface landing;
    landing.id = nextSurface++; landing.kind = access::SurfaceKind::Landing;
    landing.height = y; landing.storey = storeyIndex; landing.note = note;
    landing.outer = {in.incomingOuter, at(v, y), at(v + 1, y), at(v + 2, y), far.outgoingOuter, far.inner, mid.inner, in.inner};
    return push(std::move(landing));
  };
  const auto kiteLanding = [&](const uint32_t v, const double y, const uint32_t storeyIndex, const char *note) {
    const CornerTurn t = turnAt(v, y);
    access::Surface landing;
    landing.id = nextSurface++; landing.kind = access::SurfaceKind::Landing;
    landing.height = y; landing.storey = storeyIndex; landing.note = note;
    landing.outer = {t.incomingOuter, at(v, y), t.outgoingOuter, t.inner};
    return push(std::move(landing));
  };

  // The lookout deck is the last revolution's floor landing; it is made
  // after the loop, so its id is taken now.
  const uint32_t lookoutId = nextSurface++;
  for (uint32_t floor = 1; floor <= brief.floors; ++floor)
  {
    double y = brief.base + double(floor - 1) * storey;
    uint32_t face = s0, consumed = 0;
    while (consumed < n)
    {
      if (landingStart[face])
      {
        // The landing here was made as the head of the leg that reached it
        // (or it is the ground, on the first floor).
        face = (face + 2) % n; consumed += 2;
        continue;
      }
      const double y1 = y + riseEach;
      const uint32_t to = (face + 1) % n;
      const bool last = consumed + 1 == n;
      const char *note = last ? "tower floor landing" : landingStart[to] ? "wall-walk landing" : "tower turn";
      const uint32_t storeyIndex = last ? floor : floor - 1;
      const uint32_t head = last && floor == brief.floors ? lookoutId
                          : landingStart[to] ? doubleLanding(to, y1, storeyIndex, note) : kiteLanding(to, y1, storeyIndex, note);
      const CornerTurn foot = turnAt(face, y), arrive = turnAt(to, y1);
      access::Ramp ramp;
      ramp.id = nextRamp++; ramp.from = below; ramp.to = head;
      ramp.footLeft = foot.outgoingOuter; ramp.footRight = foot.inner;
      ramp.headLeft = arrive.incomingOuter; ramp.headRight = arrive.inner;
      ramp.note = "spiral flight along tower face " + std::to_string(face);
      out.ramps.push_back(ramp);
      below = head; y = y1; face = to; ++consumed;
    }
  }
  (void)nextPortal;

  // The well side of every flight and every turn is fenced. A spiral stair
  // round a wall has the wall on one hand and the well on the other, and the
  // other hand had nothing: a rail follows each flight up on its inner edge,
  // and runs along the edges of each turn that face the well.
  for (const access::Ramp &ramp : out.ramps)
  {
    access::Barrier rail;
    rail.id = nextBarrier++; rail.a = ramp.footRight; rail.b = ramp.headRight;
    rail.base = std::min(ramp.footRight.y, ramp.headRight.y); rail.height = 1.; rail.wall = false;
    rail.thickness = .06; rail.note = "flight rail";
    out.barriers.push_back(rail);
  }
  for (const access::Surface &surface : out.surfaces)
  {
    if (surface.kind != access::SurfaceKind::Landing) continue;
    for (size_t e = 0; e < surface.outer.size(); ++e)
    {
      const Position a = surface.outer[e], b = surface.outer[(e + 1) % surface.outer.size()];
      const Position mid{(a.x + b.x) * .5, surface.height, (a.z + b.z) * .5};
      // Against the tower wall: nothing to fence.
      double wall = 1e300;
      for (uint32_t k = 0; k < n; ++k)
      {
        const Position &p = tower[k], &q = tower[(k + 1) % n];
        const double dx = q.x - p.x, dz = q.z - p.z, l2 = dx * dx + dz * dz;
        const double t = std::clamp(((mid.x - p.x) * dx + (mid.z - p.z) * dz) / l2, 0., 1.);
        wall = std::min(wall, std::hypot(mid.x - p.x - dx * t, mid.z - p.z - dz * t));
      }
      if (wall < .12) continue;
      // Shared with a flight's foot or head: that is the way on and off.
      bool mouth = false;
      for (const access::Ramp &ramp : out.ramps)
        for (const auto &end : {std::make_pair(ramp.footLeft, ramp.footRight), std::make_pair(ramp.headLeft, ramp.headRight)})
        {
          const Position m{(end.first.x + end.second.x) * .5, 0, (end.first.z + end.second.z) * .5};
          mouth = mouth || (std::abs(end.first.y - surface.height) < .05 && std::hypot(m.x - mid.x, m.z - mid.z) < .12);
        }
      if (mouth) continue;
      access::Barrier rail;
      rail.id = nextBarrier++; rail.a = a; rail.b = b; rail.a.y = rail.b.y = surface.height;
      rail.base = surface.height; rail.height = 1.; rail.wall = false; rail.thickness = .06;
      rail.note = "turn rail";
      out.barriers.push_back(rail);
    }
  }

  // The centre is floor.  What must be open is the footprint of the final
  // flight itself: a notch through the perimeter of the lookout floor, ending
  // at the triangular arrival landing.  A central hole disconnected from the
  // stair is not a stair opening.
  access::Surface lookout;
  lookout.id = lookoutId; lookout.kind = access::SurfaceKind::Balcony;
  lookout.height = brief.base + brief.height; lookout.storey = brief.floors;
  const access::Ramp &lastRamp = out.ramps.back();
  // The notch for the last flight sits in the face it climbs: between that
  // face's two corners, in ring order, or the deck's outline crosses itself.
  const uint32_t lastFace = (s0 + n - 1) % n;
  for (uint32_t v = 0; v < n; ++v)
  {
    lookout.outer.push_back({tower[v].x, lookout.height, tower[v].z});
    if (v == lastFace)
    {
      lookout.outer.push_back({lastRamp.footLeft.x, lookout.height, lastRamp.footLeft.z});
      lookout.outer.push_back({lastRamp.footRight.x, lookout.height, lastRamp.footRight.z});
      lookout.outer.push_back({lastRamp.headRight.x, lookout.height, lastRamp.headRight.z});
      lookout.outer.push_back({lastRamp.headLeft.x, lookout.height, lastRamp.headLeft.z});
    }
  }
  lookout.note = "belfry lookout deck with stair opening";
  out.surfaces.push_back(std::move(lookout));

  const std::vector<Position> opening{{lastRamp.footLeft.x, lookout.height, lastRamp.footLeft.z},
                                      {lastRamp.footRight.x, lookout.height, lastRamp.footRight.z},
                                      {lastRamp.headRight.x, lookout.height, lastRamp.headRight.z},
                                      {lastRamp.headLeft.x, lookout.height, lastRamp.headLeft.z}};

  for (uint32_t k = 0; k < n; ++k)
  {
    access::Barrier wall;
    wall.id = nextBarrier++; wall.a = tower[k]; wall.b = tower[(k + 1) % n];
    wall.base = brief.base; wall.height = brief.height; wall.wall = true;
    wall.thickness = .5; wall.note = "polygonal tower wall";
    out.barriers.push_back(wall);
  }
  for (uint32_t k = 0; k < n; ++k)
  {
    access::Barrier rail;
    rail.id = nextBarrier++; rail.a = tower[k]; rail.b = tower[(k + 1) % n];
    rail.base = brief.base + brief.height; rail.height = 1.; rail.wall = false;
    rail.a.y=rail.base; rail.b.y=rail.base;
    rail.thickness = .08; rail.note = "open belfry perimeter rail";
    out.barriers.push_back(rail);
  }
  for (const std::pair<uint32_t,uint32_t> edge : {std::make_pair(0u,1u), std::make_pair(1u,2u)})
  {
    access::Barrier rail;
    rail.id = nextBarrier++; rail.a = opening[edge.first]; rail.b = opening[edge.second];
    rail.base = brief.base + brief.height; rail.height = 1.; rail.wall = false;
    rail.thickness = .08; rail.note = "top stairwell guard";
    out.barriers.push_back(rail);
  }
  return true;
}

// Winds a planar face so that it looks the way `outward` points: the Newell
// normal of the polygon is compared with the direction, and the face is
// reversed when it looks the other way.
void facing(std::vector<Position> &face, const Position outward)
{
  double nx = 0, ny = 0, nz = 0;
  for (size_t k = 0; k < face.size(); ++k)
  {
    const Position &a = face[k], &b = face[(k + 1) % face.size()];
    nx += (a.y - b.y) * (a.z + b.z);
    ny += (a.z - b.z) * (a.x + b.x);
    nz += (a.x - b.x) * (a.y + b.y);
  }
  if (nx * outward.x + ny * outward.y + nz * outward.z < 0) std::reverse(face.begin(), face.end());
}

void prism(const std::vector<Position> &ring,
           const double base,
           const double top,
           std::vector<std::vector<Position>> &faces,
           FortressPiece &piece,
           const bool capBottom = true,
           const bool capTop = true)
{
  if (ring.size() < 3) return;
  std::vector<Position> bottom = ring, upper = ring;
  if (area(bottom) < 0) { std::reverse(bottom.begin(), bottom.end()); std::reverse(upper.begin(), upper.end()); }
  for (Position &p : bottom) p.y = base;
  for (Position &p : upper) p.y = top;
  if (capBottom) { piece.faces.push_back(uint32_t(faces.size())); faces.push_back(bottom); }
  if (capTop) { piece.faces.push_back(uint32_t(faces.size())); std::reverse(upper.begin(),upper.end()); faces.push_back(upper); std::reverse(upper.begin(),upper.end()); }
  // The ring is wound so its bottom cap faces down, which is clockwise seen
  // from above; a side quad walked the same way round faced into the prism.
  for (size_t k = 0; k < ring.size(); ++k)
  {
    const size_t q = (k + 1) % ring.size();
    piece.faces.push_back(uint32_t(faces.size()));
    faces.push_back({bottom[q], bottom[k], upper[k], upper[q]});
  }
}

} // namespace

bool planFortressJunction(const FortressJunctionBrief &brief,
                          FortressJunctionBlueprint &out,
                          std::string *error)
{
  out = FortressJunctionBlueprint{};
  if (brief.towerSides < 3 || brief.towerRadius < 1. || brief.height <= 0)
  {
    if (error) *error = "a fortress junction needs a polygonal tower with positive radius and height";
    return false;
  }
  out.base = brief.base; out.height = brief.height;
  out.belfryTop = brief.base + brief.height + std::max(.5, brief.belfryHeight);
  out.roofApex = out.belfryTop + std::max(.5, brief.roofHeight);
  for (uint32_t k = 0; k < brief.towerSides; ++k)
  {
    const double angle = brief.towerRotation -pi * .5 + 2. * pi * double(k) / double(brief.towerSides);
    out.tower.push_back({brief.centre.x + std::cos(angle) * brief.towerRadius,
                         brief.base, brief.centre.z + std::sin(angle) * brief.towerRadius});
  }
  if (area(out.tower) < 0) std::reverse(out.tower.begin(), out.tower.end());
  const double apothemFactor = std::cos(pi / double(brief.towerSides));
  const double innerRadius = brief.towerRadius - std::max(.15, brief.towerWallDepth) / apothemFactor;
  if (innerRadius <= brief.stairWidth * 1.5)
  {
    if (error) *error = "the tower wall leaves no clear polygon for its stair";
    return false;
  }
  for (const Position &corner : out.tower)
  {
    const double dx = corner.x - brief.centre.x, dz = corner.z - brief.centre.z;
    out.towerInner.push_back({brief.centre.x + dx / brief.towerRadius * innerRadius,
                              brief.base, brief.centre.z + dz / brief.towerRadius * innerRadius});
  }

  // Keep the shell ordinary: one full-depth prism per polygon face, meeting
  // the next at the original corner.  Stair-turn closure is separate joinery.
  for (uint32_t k = 0; k < brief.towerSides; ++k)
  {
    const uint32_t q = (k+1)%brief.towerSides;
    out.towerWallPlans.push_back({out.tower[k], out.tower[q], out.towerInner[q], out.towerInner[k]});
  }

  // The doorway, in the face that looks into the town.
  {
    const double reach = std::hypot(brief.inward.x, brief.inward.z);

    if (reach > 1e-6 && brief.doorWidth > .4 && brief.doorHeight > 1.2 && brief.height > brief.doorHeight + .6)
    {
      const double ix = brief.inward.x / reach, iz = brief.inward.z / reach;
      double best = -2;

      // A tower of ten sides has faces a metre and a half across, and asking
      // for a face half a metre wider than the door skipped every one of them
      // - which is why the towers stayed sealed. The door is as wide as the
      // face can carry, down to a doorway a person fits through, and a face
      // that cannot carry even that is not a door face.
      const double jamb = .22, narrowest = .8;

      for (uint32_t k = 0; k < brief.towerSides; ++k)
      {
        const Position &a = out.towerInner[k], &b = out.towerInner[(k + 1) % brief.towerSides];
        const double ex = b.x - a.x, ez = b.z - a.z;
        const double length = std::hypot(ex, ez);

        if (length < narrowest + 2. * jamb)
        {
          continue;
        }

        // Outward normal of a counter-clockwise ring.
        const double nx = ez / length, nz = -ex / length;
        const double facing = nx * ix + nz * iz;

        if (facing > best)
        {
          best = facing;
          out.doorFace = k;
        }
      }

      if (out.doorFace != UINT32_MAX)
      {
        const Position &a = out.towerInner[out.doorFace], &b = out.towerInner[(out.doorFace + 1) % brief.towerSides];
        const double ex = b.x - a.x, ez = b.z - a.z;
        const double length = std::hypot(ex, ez);
        const double half = std::max(narrowest, std::min(brief.doorWidth, length - 2. * jamb)) * .5;
        const double ux = ex / length, uz = ez / length;
        const Position middle{(a.x + b.x) * .5, brief.base, (a.z + b.z) * .5};
        out.doorway = {{middle.x - ux * half, brief.base, middle.z - uz * half},
                       {middle.x + ux * half, brief.base, middle.z + uz * half},
                       {middle.x + ux * half, brief.base + brief.doorHeight, middle.z + uz * half},
                       {middle.x - ux * half, brief.base + brief.doorHeight, middle.z - uz * half}};
      }
    }
  }

  for (const FortressWallArm &arm : brief.walls)
  {
    double dx = arm.farEnd.x - brief.centre.x, dz = arm.farEnd.z - brief.centre.z;
    const double distance = std::hypot(dx, dz);
    if (distance <= brief.towerRadius + .1 || arm.width <= .1) continue;
    dx /= distance; dz /= distance;
    double bestT = 1e300, bestU = 0;
    uint32_t face = UINT32_MAX;
    for (uint32_t k = 0; k < brief.towerSides; ++k)
    {
      const Position &a = out.tower[k], &b = out.tower[(k + 1) % brief.towerSides];
      const double ex = b.x - a.x, ez = b.z - a.z;
      const double den = cross(dx, dz, ex, ez);
      if (std::abs(den) < 1e-9) continue;
      const double ax = a.x - brief.centre.x, az = a.z - brief.centre.z;
      const double t = cross(ax, az, ex, ez) / den;
      const double u = cross(ax, az, dx, dz) / den;
      if (t > 0 && u >= -1e-6 && u <= 1. + 1e-6 && t < bestT) { bestT = t; bestU = u; face = k; }
    }
    if (face == UINT32_MAX) continue;
    const double px = -dz * arm.width * .5, pz = dx * arm.width * .5;
    const Position contact{brief.centre.x + dx * bestT, brief.base, brief.centre.z + dz * bestT};
    const Position left{contact.x + px, brief.base, contact.z + pz};
    const Position right{contact.x - px, brief.base, contact.z - pz};
    const Position farLeft{arm.farEnd.x + px, brief.base, arm.farEnd.z + pz};
    const Position farRight{arm.farEnd.x - px, brief.base, arm.farEnd.z - pz};
    std::vector<Position> wall{left, farLeft, farRight, right};
    if (area(wall) < 0) std::reverse(wall.begin(), wall.end());
    out.walls.push_back(wall); out.wallFace.push_back(face);

    const Position &a = out.tower[face], &b = out.tower[(face + 1) % brief.towerSides];
    const double ex = b.x - a.x, ez = b.z - a.z, e2 = ex * ex + ez * ez;
    const auto project = [&](const Position &p) {
      const double u = std::clamp(((p.x - a.x) * ex + (p.z - a.z) * ez) / e2, 0., 1.);
      return Position{a.x + ex * u, brief.base, a.z + ez * u};
    };
    const Position pl = project(left), pr = project(right);
    const double faceNormalX = ez / std::sqrt(e2), faceNormalZ = -ex / std::sqrt(e2);
    const bool flush = std::abs(dx * faceNormalX + dz * faceNormalZ) > .999;
    if (!flush)
    {
      AngularJoiner joiner;
      if (buildAngularJoiner(left, right, pl, pr, joiner, nullptr))
        for (const auto &triangle : joiner.triangles)
        {
          out.joiners.push_back({triangle[0], triangle[1], triangle[2]});
          out.joinerWall.push_back(uint32_t(out.walls.size() - 1));
        }
    }
  }

  out.inhabitedFloors=brief.inhabitedFloors;
  if(brief.inhabitedFloors) {
    if(innerRadius<3.6 || brief.floors<1 || brief.height/brief.floors<4.0) {
      if(error)*error="inhabited tower needs a 3.6m inner radius and 4m storeys";
      return false;
    }
    const auto p=[&](double x,double y,double z){return Position{brief.centre.x+x,y,brief.centre.z+z};};
    const double rise=brief.height/brief.floors;
    for(uint32_t floor=0;floor<=brief.floors;++floor) {
      access::Surface deck;deck.id=2*floor+1;deck.height=brief.base+floor*rise;
      deck.kind=floor?access::SurfaceKind::Storey:access::SurfaceKind::Ground;deck.storey=floor;
      deck.outer=out.towerInner;for(auto &v:deck.outer)v.y=deck.height;
      if(floor)deck.holes={{p(-1.2,deck.height,-1.5),p(-1.2,deck.height,2.6),p(1.2,deck.height,2.6),p(1.2,deck.height,-1.5)}};
      deck.note="inhabited tower floor with stairwell";out.towerAccess.surfaces.push_back(deck);
      if(floor==brief.floors)continue;
      access::Surface turn;turn.id=2*floor+2;turn.height=deck.height+rise*.5;
      turn.kind=access::SurfaceKind::Landing;turn.storey=floor;
      turn.outer={p(-1.2,turn.height,1.5),p(1.2,turn.height,1.5),p(1.2,turn.height,2.6),p(-1.2,turn.height,2.6)};
      turn.note="return stair landing";out.towerAccess.surfaces.push_back(turn);
      out.towerAccess.ramps.push_back({2*floor+1,p(-1.2,deck.height,-1.5),p(-.1,deck.height,-1.5),p(-1.2,turn.height,1.5),p(-.1,turn.height,1.5),deck.id,turn.id,"ascending flight"});
      out.towerAccess.ramps.push_back({2*floor+2,p(.1,turn.height,1.5),p(1.2,turn.height,1.5),p(.1,deck.height+rise,-1.5),p(1.2,deck.height+rise,-1.5),turn.id,2*floor+3,"return flight"});
    }
  }

  // Which face each wall-walk door goes through, decided before the stair,
  // because a spiral stair lays its landings where its doors are.
  struct WalkDoor { uint32_t face; double along, width; double ux, uz, y; };
  std::vector<WalkDoor> walkDoors;
  for(const auto &direction:brief.elevatedDoors) {
    const double length=std::hypot(direction.x,direction.z);if(length<1e-6)continue;
    const double ux=direction.x/length,uz=direction.z/length;
    // The wall walk is 2.8 m wide and centred on the curtain's axis, which
    // runs through the tower's centre. The door has to open onto it: every
    // point of the doorway within 1.35 m of the axis, sideways. So the
    // doorway is placed on the face the axis crosses, at the crossing, and
    // where the axis crosses a corner - a bevelled curtain on a tower turned
    // to its straight neighbours - the door slides along whichever of the two
    // faces keeps more of it on the walk.
    const auto lateral=[&](Position p){return ux*(p.z-brief.centre.z)-uz*(p.x-brief.centre.x);};
    uint32_t chosen=UINT32_MAX;double chosenAlong=.5,width=0;
    for(uint32_t k=0;k<brief.towerSides;++k) {
      const auto a=out.towerInner[k],b=out.towerInner[(k+1)%brief.towerSides];
      const double dx=b.x-a.x,dz=b.z-a.z,span=std::hypot(dx,dz);
      // Outward normal of the counter-clockwise ring; the axis leaves the
      // tower through a face it looks out of.
      if((dz*ux-dx*uz)/span<=0)continue;
      // Where the axis crosses this face, as a distance along it.
      const double den=dx*uz-dz*ux;if(std::abs(den)<1e-9)continue;
      const double hit=((brief.centre.x-a.x)*uz-(brief.centre.z-a.z)*ux)/den*span;
      if(hit<-.5 || hit>span+.5)continue;
      // The stretch of the face that is both inside the jambs and over the
      // walk. Lateral offset is linear along the face.
      const double jamb=.15,l0=lateral(a),l1=lateral(b);
      double low=jamb,high=span-jamb;
      if(std::abs(l1-l0)>1e-9)for(const double bound:{1.35,-1.35}) {
        const double t=(bound-l0)/(l1-l0)*span;
        // Points past `t` are beyond the bound when the offset grows that way.
        if((l1>l0)==(bound>0))high=std::min(high,t);else low=std::max(low,t);
      }
      const double usable=std::min(1.4,high-low);
      if(usable<=width)continue;
      const double centre=std::clamp(hit,low+usable*.5,high-usable*.5);
      chosen=k;chosenAlong=centre/span;width=usable;
    }
    if(chosen==UINT32_MAX || width<.8)continue;
    walkDoors.push_back({chosen,chosenAlong,width,ux,uz,direction.y});
  }
  if(!brief.inhabitedFloors) {
    std::vector<uint32_t> faces;
    for(const auto &door:walkDoors)faces.push_back(door.face);
    if(!polygonTowerAccess(brief,out.towerInner,faces,out.towerAccess,error))return false;
  }

  if(out.doorFace!=UINT32_MAX && out.doorway.size()==4)
    out.openings.push_back({out.doorFace,brief.base,out.doorway[2].y,
      std::hypot(out.doorway[1].x-out.doorway[0].x,out.doorway[1].z-out.doorway[0].z)});
  for(const auto &door:walkDoors) {
    const uint32_t chosen=door.face;double chosenAlong=door.along,width=door.width;
    const double ux=door.ux,uz=door.uz;
    const Position direction{ux,door.y,uz};
    const auto a=out.towerInner[chosen],b=out.towerInner[(chosen+1)%brief.towerSides];
    double sill=direction.y;
    if(!brief.inhabitedFloors && brief.floors>=2) {
      // A spiral stair has level ground only at its turns: the landing at
      // each corner, a leg's rise above the one before. The door goes at
      // whichever end of its face has a landing nearest the wall walk, and
      // its sill is that landing's height, so you step off the walk onto a
      // place to stand and not onto a flight in mid rise.
      const double storey=brief.height/double(brief.floors),span=std::hypot(b.x-a.x,b.z-a.z);
      const double nearWidth=std::min(width,brief.stairWidth+.1);
      // The landing at corner c on floor f stands at base + (f - 1 + c/n)
      // storey, and c = 0 is the floor's own landing after a full turn. Two
      // doors on opposite faces cannot both meet one at the same height - a
      // spiral rises as it goes round - so a door takes the landing at or
      // just above the walk, within half a storey, and the walk gets a few
      // steps up to its sill.
      // The landing that covers this face, at or just above the walk.
      (void)span;(void)nearWidth;
      const Position stand{a.x+(b.x-a.x)*chosenAlong-ux*.1,0,a.z+(b.z-a.z)*chosenAlong-uz*.1};
      double bestGap=1e300;
      for(const auto &surface:out.towerAccess.surfaces) {
        if(surface.kind!=access::SurfaceKind::Landing)continue;
        bool in=false;
        for(size_t i=0,j=surface.outer.size()-1;i<surface.outer.size();j=i++) {
          const auto &p=surface.outer[i],&q=surface.outer[j];
          if((p.z>stand.z)!=(q.z>stand.z) && stand.x<(q.x-p.x)*(stand.z-p.z)/(q.z-p.z)+p.x)in=!in;
        }
        if(!in)continue;
        const double gap=surface.height-direction.y;
        if(gap>=-.05 && gap<bestGap){bestGap=gap;sill=surface.height;}
      }
      if(bestGap>storey+.05)continue;
    }
    if(sill<brief.base || sill+2.4>brief.base+brief.height)continue;
    if(brief.inhabitedFloors) {
      const auto floor=std::find_if(out.towerAccess.surfaces.begin(),out.towerAccess.surfaces.end(),
        [&](const access::Surface &s){return s.kind==access::SurfaceKind::Storey && std::abs(s.height-direction.y)<1e-5;});
      if(floor==out.towerAccess.surfaces.end()) {
        if(error)*error="wall-walk doorway must meet a complete tower floor";
        return false;
      }
    }
    out.openings.push_back({chosen,sill,sill+2.4,width,chosenAlong});
    access::Portal portal;portal.id=uint32_t(out.towerAccess.portals.size()+1);
    portal.at={a.x+(b.x-a.x)*chosenAlong,sill,a.z+(b.z-a.z)*chosenAlong};portal.width=width;portal.height=2.4;
    portal.outX=ux;portal.outZ=uz;portal.into=brief.inhabitedFloors?-.15:-.1;
    portal.note="curtain wall walk access";out.towerAccess.portals.push_back(portal);
  }

  if(brief.inhabitedFloors) {
    uint32_t guardId=1;
    for(const auto &surface:out.towerAccess.surfaces) {
      if(surface.kind==access::SurfaceKind::Ground)continue;
      std::vector<std::vector<Position>> rings{surface.outer};
      rings.insert(rings.end(),surface.holes.begin(),surface.holes.end());
      for(size_t ring=0;ring<rings.size();++ring)for(size_t k=0;k<rings[ring].size();++k) {
        const auto a=rings[ring][k],b=rings[ring][(k+1)%rings[ring].size()];
        const double dx=b.x-a.x,dz=b.z-a.z,len=std::hypot(dx,dz);
        const auto project=[&](Position p){return ((p.x-a.x)*dx+(p.z-a.z)*dz)/len;};
        const auto distance=[&](Position p){return std::abs((p.x-a.x)*dz-(p.z-a.z)*dx)/len;};
        std::vector<std::pair<double,double>> holes;
        for(const auto &r:out.towerAccess.ramps)for(const auto &ends:{std::make_pair(r.footLeft,r.footRight),std::make_pair(r.headLeft,r.headRight)}) {
          if(std::abs(ends.first.y-surface.height)>.05 || distance(ends.first)>.05 || distance(ends.second)>.05)continue;
          const double p=project(ends.first),q=project(ends.second);holes.push_back({std::min(p,q)-.05,std::max(p,q)+.05});
        }
        // A flight leaving this floor along a hole edge is guarded by its
        // own rail, which climbs with it; a floor rail beside it would stand
        // inside that rail for the first metre and under the treads after.
        for(const auto &r:out.towerAccess.ramps)for(const auto &side:{std::make_pair(r.footLeft,r.headLeft),std::make_pair(r.footRight,r.headRight)}) {
          if(std::abs(side.first.y-surface.height)>.05 || distance(side.first)>.05 || distance(side.second)>.05)continue;
          const double p=project(side.first),q=project(side.second);holes.push_back({std::min(p,q)-.05,std::max(p,q)+.05});
        }
        for(const auto &door:out.towerAccess.portals)if(std::abs(door.at.y-surface.height)<.05 && distance(door.at)<.05)
          holes.push_back({project(door.at)-door.width*.5-.05,project(door.at)+door.width*.5+.05});
        std::sort(holes.begin(),holes.end());double from=0;
        const auto add=[&](double low,double high) {
          low=std::max(0.,low);high=std::min(len,high);if(high-low<.05)return;
          access::Barrier rail;rail.id=guardId++;rail.a={a.x+dx*low/len,surface.height,a.z+dz*low/len};
          rail.b={a.x+dx*high/len,surface.height,a.z+dz*high/len};rail.base=surface.height;rail.height=1.;rail.thickness=.08;
          rail.wall=ring==0 && surface.kind==access::SurfaceKind::Storey && surface.height<brief.base+brief.height-.1;
          rail.note=rail.wall?"tower shell proxy":"tower stairwell guard";out.towerAccess.barriers.push_back(rail);
        };
        for(const auto hole:holes){add(from,hole.first);from=std::max(from,hole.second);}add(from,len);
      }
    }
    // Every flight is fenced along its open side. The two flights of a
    // storey run side by side with the spine wall between them, so the
    // inner edges are closed by masonry and the outer edge of each - the one
    // the floor is beside, then below - carries a rail that climbs with the
    // treads. The access model keeps the rail's two heights, so it is drawn
    // as a sloping rail and not as a fence through the flight.
    for(const auto &r:out.towerAccess.ramps) {
      const auto reach=[&](Position p,Position q){return std::abs((p.x+q.x)*.5-brief.centre.x)+std::abs((p.z+q.z)*.5-brief.centre.z);};
      const bool leftOuter=reach(r.footLeft,r.headLeft)>reach(r.footRight,r.headRight);
      access::Barrier rail;rail.id=guardId++;
      rail.a=leftOuter?r.footLeft:r.footRight;rail.b=leftOuter?r.headLeft:r.headRight;
      rail.base=std::min(rail.a.y,rail.b.y);rail.height=1.;rail.thickness=.08;rail.wall=false;
      rail.note="flight rail";out.towerAccess.barriers.push_back(rail);
    }
  }

  // The first triangle turns between flights. The second uses its exact
  // wall-side edge and the unmodified inner wall corner, closing the remaining
  // triangular pocket without bevels, offsets or independently sampled edges.
  uint32_t turnOrdinal = 0;
  for (const access::Surface &surface : out.towerAccess.surfaces)
    if (surface.kind == access::SurfaceKind::Landing && surface.outer.size() == 4 && !brief.inhabitedFloors)
    {
      // The landing is the whole corner already; the turn is that one slab.
      FortressStairTurn turn;
      turn.walkable = surface.outer;
      out.stairTurns.push_back(std::move(turn));
      ++turnOrdinal;
    }
    else if (surface.kind == access::SurfaceKind::Landing && surface.outer.size() == 3)
    {
      const uint32_t corner = (turnOrdinal + 1) % brief.towerSides;
      FortressStairTurn turn;
      turn.walkable = surface.outer;
      // The walkable triangle's two wall-side vertices form the exact shared
      // edge of the closure triangle.  Choosing independent wall-trim points
      // here was the previous bug: the triangles only nearly met and left a
      // visible white sliver.
      std::array<uint32_t,3> order{0,1,2};
      std::sort(order.begin(), order.end(), [&](const uint32_t a, const uint32_t b) {
        const double adx = surface.outer[a].x-brief.centre.x, adz = surface.outer[a].z-brief.centre.z;
        const double bdx = surface.outer[b].x-brief.centre.x, bdz = surface.outer[b].z-brief.centre.z;
        return adx*adx+adz*adz > bdx*bdx+bdz*bdz;
      });
      turn.wallClosure = {surface.outer[order[0]], out.towerInner[corner], surface.outer[order[1]]};
      for (Position &p : turn.wallClosure) p.y = surface.height;
      if (area(turn.wallClosure) < 0) std::reverse(turn.wallClosure.begin(), turn.wallClosure.end());
      out.stairTurns.push_back(std::move(turn));
      ++turnOrdinal;
    }

  // The opening belongs to the ceiling/deck structure, rather than being an
  // instruction for a future mesher to guess where the stair was.
  for (const access::Surface &surface : out.towerAccess.surfaces)
    if (surface.note == "belfry lookout deck with stair opening")
    {
      // The opening is the last flight's footprint at deck height, wherever
      // in the outline the notch was cut.
      if (!out.towerAccess.ramps.empty())
      {
        const access::Ramp &lastRamp = out.towerAccess.ramps.back();
        out.stairOpening = {{lastRamp.footLeft.x, surface.height, lastRamp.footLeft.z}, {lastRamp.footRight.x, surface.height, lastRamp.footRight.z},
                            {lastRamp.headRight.x, surface.height, lastRamp.headRight.z}, {lastRamp.headLeft.x, surface.height, lastRamp.headLeft.z}};
      }
      std::vector<access::Face> triangles;
      if (!access::triangulate(out.towerAccess, triangles, error)) return false;
      for (const access::Face &face : triangles)
        if (face.kind == access::FaceKind::SurfaceFace && face.of == surface.id)
          out.topDeckFaces.push_back({face.a, face.b, face.c});
      break;
    }

  const double post = std::max(.15, brief.belfryPierSize) * .5;
  for (const Position &corner : out.tower)
  {
    double rx = corner.x - brief.centre.x, rz = corner.z - brief.centre.z;
    const double length = std::hypot(rx, rz); rx /= length; rz /= length;
    const double tx = -rz, tz = rx;
    out.belfryPiers.push_back({{corner.x-rx*post-tx*post, brief.base+brief.height, corner.z-rz*post-tz*post},
                               {corner.x+rx*post-tx*post, brief.base+brief.height, corner.z+rz*post-tz*post},
                               {corner.x+rx*post+tx*post, brief.base+brief.height, corner.z+rz*post+tz*post},
                               {corner.x-rx*post+tx*post, brief.base+brief.height, corner.z-rz*post+tz*post}});
  }

  std::vector<Position> eaves;
  for (const Position &corner : out.tower)
  {
    double rx = corner.x - brief.centre.x, rz = corner.z - brief.centre.z;
    const double length = std::hypot(rx, rz);
    const double scale = (length + std::max(0., brief.roofOverhang)) / length;
    eaves.push_back({brief.centre.x + rx * scale, out.belfryTop, brief.centre.z + rz * scale});
  }
  const Position apex{brief.centre.x, out.roofApex, brief.centre.z};
  if(brief.sweptSpire) {
    // Shared rings make a continuous shell. The eave flare gives way to a
    // steep crown, with a short collar and needle at its tip.
    const std::array<std::pair<double,double>,6> profile{{{0.,1.},{.09,.79},{.38,.48},{.70,.20},{.76,.23},{.91,.065}}};
    std::vector<Position> previous;
    for(const auto &[height,radius]:profile) {
      std::vector<Position> ring;
      for(const auto &e:eaves)ring.push_back({brief.centre.x+(e.x-brief.centre.x)*radius,
        out.belfryTop+(out.roofApex-out.belfryTop)*height,brief.centre.z+(e.z-brief.centre.z)*radius});
      if(!previous.empty())for(size_t k=0;k<ring.size();++k) {
        const size_t q=(k+1)%ring.size();
        auto face=std::vector<Position>{previous[k],previous[q],ring[q],ring[k]};
        facing(face,{previous[k].x-brief.centre.x,.2,previous[k].z-brief.centre.z});
        out.roofFaces.push_back(std::move(face));
      }
      previous=std::move(ring);
    }
    for(size_t k=0;k<previous.size();++k) {
      auto face=std::vector<Position>{previous[k],previous[(k+1)%previous.size()],apex};
      facing(face,{previous[k].x-brief.centre.x,.2,previous[k].z-brief.centre.z});
      out.roofFaces.push_back(std::move(face));
    }
    auto soffit=eaves;facing(soffit,{0,-1,0});out.roofFaces.push_back(std::move(soffit));
  } else {
    for (size_t k = 0; k < eaves.size(); ++k)
      out.roofFaces.push_back({eaves[(k + 1) % eaves.size()], eaves[k], apex});
  }
  return true;
}

FortressJunctionStructure buildFortressJunctionStructure(const FortressJunctionBlueprint &blueprint)
{
  FortressJunctionStructure out;
  for (size_t k = 0; k < blueprint.towerWallPlans.size(); ++k)
  {
    FortressPiece towerWall;
    towerWall.kind = FortressPieceKind::Tower;
    const std::vector<Position> &plan = blueprint.towerWallPlans[k];

    // Slice all apertures out of both skins, including elevated wall-walk doors.
    const double span=std::hypot(plan[2].x-plan[3].x,plan[2].z-plan[3].z);
    std::vector<double> xs{0,1},ys{blueprint.base,blueprint.base+blueprint.height};
    // An opening's extent along the face, as fractions: centred where it was
    // placed, kept a hair inside the corners.
    const auto extent=[&](const FortressJunctionBlueprint::Opening &door) {
      const double half=std::min(door.width*.5/span,.48);
      const double centre=std::clamp(door.along,.02+half,.98-half);
      return std::make_pair(centre-half,centre+half);
    };
    for(const auto &door:blueprint.openings)if(door.face==k) {
      const auto [x0,x1]=extent(door);
      xs.push_back(x0);xs.push_back(x1);ys.push_back(door.bottom);ys.push_back(door.top);
    }
    std::sort(xs.begin(),xs.end());xs.erase(std::unique(xs.begin(),xs.end()),xs.end());
    std::sort(ys.begin(),ys.end());ys.erase(std::unique(ys.begin(),ys.end()),ys.end());
    const auto along=[](Position a,Position b,double t){return Position{a.x+(b.x-a.x)*t,a.y,a.z+(b.z-a.z)*t};};
    for(size_t x=1;x<xs.size();++x)for(size_t y=1;y<ys.size();++y) {
      const double u=(xs[x-1]+xs[x])*.5,v=(ys[y-1]+ys[y])*.5;bool hole=false;
      for(const auto &door:blueprint.openings)if(door.face==k) {
        const auto [x0,x1]=extent(door);
        hole|=u>x0 && u<x1 && v>door.bottom && v<door.top;
      }
      const auto apertureAt=[&](double height) {
        for(const auto &door:blueprint.openings)if(door.face==k) {
          const auto [x0,x1]=extent(door);
          if(u>x0 && u<x1 && height>door.bottom && height<door.top)return true;
        }
        return false;
      };
      if(!hole)prism({along(plan[0],plan[1],xs[x-1]),along(plan[0],plan[1],xs[x]),
                     along(plan[3],plan[2],xs[x]),along(plan[3],plan[2],xs[x-1])},
                     ys[y-1],ys[y],out.faces,towerWall,
                     y==1 || apertureAt(ys[y-1]-.0001),
                     y+1==ys.size() || apertureAt(ys[y]+.0001));
    }
    for(const auto &door:blueprint.openings)if(door.face==k && door.bottom<=blueprint.base+.01) {
      const auto [x0,x1]=extent(door);
      FortressPiece threshold;threshold.kind=FortressPieceKind::TopDeck;
      prism({along(plan[0],plan[1],x0),along(plan[0],plan[1],x1),
             along(plan[3],plan[2],x1),along(plan[3],plan[2],x0)},
            door.bottom-.18,door.bottom,out.faces,threshold);
      // Insert after the shell pieces so their contact indices remain stable.
      towerWall.faces.insert(towerWall.faces.end(),threshold.faces.begin(),threshold.faces.end());
    }
    out.pieces.push_back(std::move(towerWall));
  }
  const uint32_t n = uint32_t(blueprint.tower.size());
  if (blueprint.towerWallPlans.size() == n)
    for (uint32_t k = 0; k < n; ++k)
    {
      const uint32_t q = (k+1)%n;
      out.contacts.push_back({k, q, average({blueprint.tower[q], blueprint.towerInner[q]})});
    }
  for (uint32_t k = 0; k < uint32_t(blueprint.stairTurns.size()); ++k)
  {
    const FortressStairTurn &turn = blueprint.stairTurns[k];
    FortressPiece piece; piece.kind = FortressPieceKind::StairTurn;
    // A landing is a slab, not a surface: its top, an underside a slab's
    // depth below, and a skirt round its edges, so the turn carries the
    // flights that meet on it instead of floating between them.
    const auto slab = [&](std::vector<Position> top) {
      if (top.size() < 3) return;
      piece.faces.push_back(uint32_t(out.faces.size())); out.faces.push_back(top);
      std::vector<Position> under = top;
      for (Position &p : under) p.y -= .18;
      facing(under, {0, -1, 0});
      piece.faces.push_back(uint32_t(out.faces.size())); out.faces.push_back(under);
      Position centre = average(top);
      for (size_t e = 0; e < top.size(); ++e)
      {
        const Position a = top[e], b = top[(e + 1) % top.size()];
        Position c = b, d = a; c.y -= .18; d.y -= .18;
        std::vector<Position> skirt{a, b, c, d};
        facing(skirt, {(a.x + b.x) * .5 - centre.x, 0, (a.z + b.z) * .5 - centre.z});
        piece.faces.push_back(uint32_t(out.faces.size())); out.faces.push_back(skirt);
      }
    };
    slab(turn.walkable);
    slab(turn.wallClosure);
    const uint32_t p = uint32_t(out.pieces.size()); out.pieces.push_back(std::move(piece));
    if (n != 0)
    {
      out.contacts.push_back({p, k%n, average(turn.wallClosure)});
      out.contacts.push_back({p, (k+1)%n, average(turn.wallClosure)});
    }
  }
  for (uint32_t w = 0; w < uint32_t(blueprint.walls.size()); ++w)
  {
    const auto &wall = blueprint.walls[w];
    FortressPiece piece; piece.kind = FortressPieceKind::Wall;
    prism(wall, blueprint.base, blueprint.base + blueprint.height, out.faces, piece);
    const uint32_t p = uint32_t(out.pieces.size()); out.pieces.push_back(std::move(piece));
    const bool joined = std::find(blueprint.joinerWall.begin(), blueprint.joinerWall.end(), w) != blueprint.joinerWall.end();
    if (!joined) out.contacts.push_back({p, w < blueprint.wallFace.size() ? blueprint.wallFace[w] : 0, average(wall)});
  }
  for (uint32_t j = 0; j < uint32_t(blueprint.joiners.size()); ++j)
  {
    const auto &joiner = blueprint.joiners[j];
    FortressPiece piece; piece.kind = FortressPieceKind::Joiner;
    prism(joiner, blueprint.base, blueprint.base + blueprint.height, out.faces, piece);
    const uint32_t p = uint32_t(out.pieces.size()); out.pieces.push_back(std::move(piece));
    const uint32_t wall = j < blueprint.joinerWall.size() ? blueprint.joinerWall[j] : 0;
    const uint32_t towerPiece = wall < blueprint.wallFace.size() ? blueprint.wallFace[wall] : 0;
    out.contacts.push_back({p, towerPiece, average(joiner)});
    if (j < blueprint.joinerWall.size() && blueprint.joinerWall[j] < blueprint.walls.size())
      out.contacts.push_back({p, uint32_t(blueprint.towerWallPlans.size() + blueprint.stairTurns.size()) + blueprint.joinerWall[j], average(joiner)});
  }
  if (!blueprint.topDeckFaces.empty())
  {
    FortressPiece deck; deck.kind = FortressPieceKind::TopDeck;
    for (const auto &face : blueprint.topDeckFaces)
    { deck.faces.push_back(uint32_t(out.faces.size())); out.faces.push_back(face); }
    const uint32_t p = uint32_t(out.pieces.size()); out.pieces.push_back(std::move(deck));
    out.contacts.push_back({0, p, average(blueprint.tower)});
  }
  for (const auto &ring : blueprint.belfryPiers)
  {
    FortressPiece pier; pier.kind = FortressPieceKind::BelfryPier;
    prism(ring, blueprint.base + blueprint.height, blueprint.belfryTop, out.faces, pier);
    const uint32_t p = uint32_t(out.pieces.size()); out.pieces.push_back(std::move(pier));
    out.contacts.push_back({0, p, average(ring)});
  }
  if (!blueprint.roofFaces.empty())
  {
    FortressPiece roof; roof.kind = FortressPieceKind::Roof;
    for (const auto &face : blueprint.roofFaces)
    { roof.faces.push_back(uint32_t(out.faces.size())); out.faces.push_back(face); }
    out.pieces.push_back(std::move(roof));
  }
  // Flights and guards used to exist only in the diagnostic gallery. Retain
  // them in the structural output so every consumer receives the same stair.
  uint32_t flightOrdinal=0;
  for (const auto &r : blueprint.towerAccess.ramps)
  {
    FortressPiece flight; flight.kind=FortressPieceKind::StairFlight;
    {
      // A flight is a solid: treads and risers on top, a stringer down each
      // side, a soffit underneath. It used to be the treads and risers alone,
      // which from the side of the well was a paper stair with the room
      // showing through it. The soffit starts under the floor slab at the
      // foot and ends a hand below the landing slab at the head, so the
      // flight rests on the floor it leaves and carries the landing it
      // reaches; what is under the next storey's flight is this one's soffit.
      const unsigned steps=unsigned(std::ceil((r.headLeft.y-r.footLeft.y)/.18));
      const auto lerp=[](Position a,Position b,double t){return Position{a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t};};
      const auto lowered=[](Position p,double by){p.y-=by;return p;};
      const double footSlab=.18,headSoffit=.28;
      const Position lateral{r.footRight.x-r.footLeft.x,0,r.footRight.z-r.footLeft.z};
      const Position ahead{r.headLeft.x-r.footLeft.x,0,r.headLeft.z-r.footLeft.z};
      const auto oriented=[&](std::vector<Position> face,Position outward) {
        facing(face,outward);
        flight.faces.push_back(uint32_t(out.faces.size()));out.faces.push_back(std::move(face));
      };
      std::vector<Position> leftSide{lowered(r.footLeft,footSlab),r.footLeft},rightSide{lowered(r.footRight,footSlab),r.footRight};
      for(unsigned step=0;step<steps;++step) {
        auto a=lerp(r.footLeft,r.headLeft,double(step)/steps),b=lerp(r.footRight,r.headRight,double(step)/steps);
        auto c=lerp(r.footRight,r.headRight,double(step+1)/steps),d=lerp(r.footLeft,r.headLeft,double(step+1)/steps);
        const auto lowA=a,lowB=b;a.y=b.y=c.y;
        flight.faces.push_back(uint32_t(out.faces.size()));out.faces.push_back({a,b,c,d});
        // The riser looks back down the flight, at whoever is climbing it.
        oriented({lowA,lowB,b,a},{-ahead.x,0,-ahead.z});
        leftSide.push_back(a);leftSide.push_back(d);
        rightSide.push_back(b);rightSide.push_back(c);
      }
      leftSide.push_back(lowered(r.headLeft,headSoffit));
      rightSide.push_back(lowered(r.headRight,headSoffit));
      oriented(leftSide,{-lateral.x,0,-lateral.z});
      oriented(rightSide,lateral);
      oriented({lowered(r.footLeft,footSlab),lowered(r.footRight,footSlab),lowered(r.headRight,headSoffit),lowered(r.headLeft,headSoffit)},{0,-1,0});
      oriented({lowered(r.headLeft,headSoffit),lowered(r.headRight,headSoffit),lowered(r.headRight,footSlab),lowered(r.headLeft,footSlab)},ahead);
    }
    const uint32_t piece=uint32_t(out.pieces.size());
    out.pieces.push_back(std::move(flight));
    if (flightOrdinal<blueprint.stairTurns.size())
      out.contacts.push_back({piece,n+flightOrdinal,average({r.headLeft,r.headRight})});
    if (flightOrdinal>0 && flightOrdinal<=blueprint.stairTurns.size())
      out.contacts.push_back({piece,n+flightOrdinal-1,average({r.footLeft,r.footRight})});
    ++flightOrdinal;
  }
  std::vector<access::Face> accessFaces;
  access::triangulate(blueprint.towerAccess,accessFaces,nullptr);
  if(blueprint.inhabitedFloors) {
    FortressPiece floors;floors.kind=FortressPieceKind::FloorSlab;
    for(const auto &face:accessFaces)if(face.kind==access::FaceKind::SurfaceFace) {
      floors.faces.push_back(uint32_t(out.faces.size()));out.faces.push_back({face.a,face.b,face.c});
      auto &upper=out.faces.back();if(area(upper)>0)std::reverse(upper.begin(),upper.end());
      auto a=upper[0],b=upper[1],c=upper[2];a.y-=.18;b.y-=.18;c.y-=.18;
      floors.faces.push_back(uint32_t(out.faces.size()));out.faces.push_back({c,b,a});
    }
    for(const auto &surface:blueprint.towerAccess.surfaces) {
      auto rings=surface.holes;rings.push_back(surface.outer);
      for(const auto &ring:rings)for(size_t k=0;k<ring.size();++k) {
        auto a=ring[k],b=ring[(k+1)%ring.size()];a.y=b.y=surface.height;
        auto c=b,d=a;c.y-=.18;d.y-=.18;
        floors.faces.push_back(uint32_t(out.faces.size()));out.faces.push_back({a,b,c,d});
      }
    }
    out.pieces.push_back(std::move(floors));

    // The spine: a wall between the two flights of every storey, from the
    // ground to a rail's height above the lookout. The flights of a return
    // stair run side by side and the strip between them was open - a slot
    // the width of a hand down the whole height of the tower, through which
    // every flight below showed. A closed-well stair has masonry there, and
    // it is the inner guard of both flights, so the rails go on the outer
    // edges only. Its top follows the uppermost flight, a metre above it.
    if(!blueprint.towerAccess.ramps.empty()) {
      const auto &first=blueprint.towerAccess.ramps.front(),&last=blueprint.towerAccess.ramps.back();
      const auto q0=first.footRight,q1=first.headRight;      // The ascending flights' inner edge, in plan.
      const auto p0=last.headLeft,p1=last.footLeft;          // The return flights' inner edge, at the top.
      const double top0=p0.y+1.,top1=p1.y+1.;
      const auto at=[](Position p,double y){p.y=y;return p;};
      Position centre{(q0.x+q1.x+p0.x+p1.x)*.25,0,(q0.z+q1.z+p0.z+p1.z)*.25};
      FortressPiece spine;spine.kind=FortressPieceKind::Wall;
      const auto add=[&](std::vector<Position> face) {
        Position mean{};for(auto v:face){mean.x+=v.x/face.size();mean.y+=v.y/face.size();mean.z+=v.z/face.size();}
        facing(face,{mean.x-centre.x,mean.y-(blueprint.base+top0)*.5,mean.z-centre.z});
        spine.faces.push_back(uint32_t(out.faces.size()));out.faces.push_back(std::move(face));
      };
      add({at(q0,blueprint.base),at(q1,blueprint.base),at(q1,top1),at(q0,top0)});
      add({at(p0,blueprint.base),at(p1,blueprint.base),at(p1,top1),at(p0,top0)});
      add({at(q0,blueprint.base),at(p0,blueprint.base),at(p0,top0),at(q0,top0)});
      add({at(q1,blueprint.base),at(p1,blueprint.base),at(p1,top1),at(q1,top1)});
      add({at(q0,top0),at(p0,top0),at(p1,top1),at(q1,top1)});
      out.pieces.push_back(std::move(spine));
    }
  }
  FortressPiece guards; guards.kind=FortressPieceKind::Guard;
  for (const auto &face : accessFaces) if (face.kind==access::FaceKind::BarrierFace)
  {
    const auto barrier=std::find_if(blueprint.towerAccess.barriers.begin(),blueprint.towerAccess.barriers.end(),
      [&](const access::Barrier &b) { return b.id==face.of; });
    // Wall barriers are collision proxies for the shell already emitted above.
    if (barrier==blueprint.towerAccess.barriers.end() || barrier->wall) continue;
    guards.faces.push_back(uint32_t(out.faces.size()));
    out.faces.push_back({face.a,face.b,face.c});
  }
  if (!guards.faces.empty()) out.pieces.push_back(std::move(guards));
  return out;
}

void meshFortressJunction(const FortressJunctionStructure &structure, hierarchy::Solid &out)
{
  out = hierarchy::Solid{};
  for (const FortressPiece &piece : structure.pieces)
  for (const uint32_t f : piece.faces)
  {
    if (f >= structure.faces.size()) continue;
    hierarchy::Solid::Face face;
    for (const Position &p : structure.faces[f]) face.push_back({p.x, p.y, p.z});
    if((piece.kind==FortressPieceKind::TopDeck || piece.kind==FortressPieceKind::StairTurn) && face.size()>=3) {
      const double normalY=(face[1].z-face[0].z)*(face[2].x-face[0].x)-(face[1].x-face[0].x)*(face[2].z-face[0].z);
      if(normalY<0)std::reverse(face.begin(),face.end());
    }
    if (piece.kind == FortressPieceKind::StairFlight) {
      if(face.size()>=3) {
        const double normalY=(face[1].z-face[0].z)*(face[2].x-face[0].x)-(face[1].x-face[0].x)*(face[2].z-face[0].z);
        if(normalY<0)std::reverse(face.begin(),face.end());
      }
      out.stairs.push_back(std::move(face));
    }
    else if (piece.kind == FortressPieceKind::Guard)
      out.joinery.push_back(std::move(face));
    else if (piece.kind == FortressPieceKind::Roof)
      out.slopes.push_back(std::move(face));
    else if (piece.kind == FortressPieceKind::BelfryPier)
      out.pillars.push_back(std::move(face));
    else if (piece.kind == FortressPieceKind::Joiner || piece.kind == FortressPieceKind::StairTurn)
      out.joinery.push_back(std::move(face));
    else if (piece.kind == FortressPieceKind::Tower)
      out.innerWalls.push_back(std::move(face));
    else if (structure.faces[f].size() >= 3 && std::abs(structure.faces[f][0].y - structure.faces[f][1].y) < 1e-6 &&
        std::abs(structure.faces[f][1].y - structure.faces[f][2].y) < 1e-6)
      out.decking.push_back(std::move(face));
    else
      out.walls.push_back(std::move(face));
  }
}

} // namespace procedural::world::generator
