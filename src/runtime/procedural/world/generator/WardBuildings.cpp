#include "WardBuildings.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world::generator {

namespace {

double distance(Position a, Position b)
{
  return std::hypot(b.x - a.x, b.z - a.z);
}

Position lerp(Position a, Position b, double t)
{
  return {a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t};
}

double area(const ConvexRing &p)
{
  return std::abs(signedArea(p));
}

// Watabou's Random: float(), normal() = mean of three floats, fuzzy(1) = normal().
struct Draws
{
  uint64_t key;
  uint64_t count = 0;
  const std::function<double(uint64_t, uint64_t)> &random;

  double next()
  {
    return random(key, ++count);
  }

  double normal()
  {
    return (next() + next() + next()) / 3;
  }

  bool chance(double p)
  {
    return next() < p;
  }
};

// Keep the half-plane nx x + nz z <= c of a convex ring.
ConvexRing clipHalfPlane(const ConvexRing &p, double nx, double nz, double c)
{
  ConvexRing q;

  for (size_t i = 0; i < p.size(); ++i)
  {
    const Position a = p[i], b = p[(i + 1) % p.size()];
    const double da = nx * a.x + nz * a.z - c, db = nx * b.x + nz * b.z - c;

    if (da <= 0)
    {
      q.push_back(a);
    }

    if ((da < 0) != (db < 0))
    {
      q.push_back(lerp(a, b, da / (da - db)));
    }
  }

  ConvexRing r;

  for (auto v : q)
  {
    if (r.empty() || distance(r.back(), v) > 1e-7)
    {
      r.push_back(v);
    }
  }

  while (r.size() > 1 && distance(r.front(), r.back()) < 1e-7)
  {
    r.pop_back();
  }

  return r;
}

// Polygon.cut(p1, p2, gap) for a convex polygon: the halves on either side
// of the line through p1 and p2, each peeled back by half the gap.
std::vector<ConvexRing> cut(const ConvexRing &poly, Position p1, Position p2, double gap)
{
  const double dx = p2.x - p1.x, dz = p2.z - p1.z, l = std::hypot(dx, dz);

  if (l < 1e-9)
  {
    return {poly};
  }

  const double nx = -dz / l, nz = dx / l, c = nx * p1.x + nz * p1.z;
  ConvexRing left = clipHalfPlane(poly, nx, nz, c - gap * .5), right = clipHalfPlane(poly, -nx, -nz, -(c + gap * .5));
  std::vector<ConvexRing> halves;

  if (left.size() >= 3 && area(left) > 1e-6)
  {
    halves.push_back(left);
  }

  if (right.size() >= 3 && area(right) > 1e-6)
  {
    halves.push_back(right);
  }

  if (halves.empty())
  {
    halves.push_back(poly);
  }

  return halves;
}

// Cutter.bisect: through the point at `ratio` along the edge starting at
// `vertex`, perpendicular to that edge turned by `angle`.
std::vector<ConvexRing> bisect(const ConvexRing &poly, size_t vertex, double ratio, double angle, double gap)
{
  const Position v = poly[vertex], next = poly[(vertex + 1) % poly.size()];
  const Position p1 = lerp(v, next, ratio);
  const double dx = next.x - v.x, dz = next.z - v.z;
  const double cosB = std::cos(angle), sinB = std::sin(angle);
  const double vx = dx * cosB - dz * sinB, vz = dz * cosB + dx * sinB;
  const Position p2{p1.x - vz, 0, p1.z + vx};
  return cut(poly, p1, p2, gap);
}

size_t longestEdge(const ConvexRing &p)
{
  size_t best = 0;
  double length = -1;

  for (size_t i = 0; i < p.size(); ++i)
  {
    const double l = distance(p[i], p[(i + 1) % p.size()]);

    if (l > length)
    {
      length = l;
      best = i;
    }
  }

  return best;
}

// Narrowest extent across any edge normal.
double narrowestWidth(const ConvexRing &p)
{
  double narrowest = 1e30;

  for (size_t k = 0; k < p.size(); ++k)
  {
    const Position u = p[k], v = p[(k + 1) % p.size()];
    const double l = distance(u, v);

    if (l < 1e-9)
    {
      continue;
    }

    const double ex = (v.z - u.z) / l, ez = (u.x - v.x) / l;
    double lo = 1e30, hi = -1e30;

    for (auto q : p)
    {
      lo = std::min(lo, ex * q.x + ez * q.z);
      hi = std::max(hi, ex * q.x + ez * q.z);
    }

    narrowest = std::min(narrowest, hi - lo);
  }

  return narrowest;
}

double sharpestCorner(const ConvexRing &p)
{
  double sharpest = 3.14159265358979;

  for (size_t k = 0; k < p.size(); ++k)
  {
    const Position u = p[(k + p.size() - 1) % p.size()], v = p[k], w = p[(k + 1) % p.size()];
    const double ax = u.x - v.x, az = u.z - v.z, bx = w.x - v.x, bz = w.z - v.z;
    const double d = std::max(1e-9, std::hypot(ax, az) * std::hypot(bx, bz));
    sharpest = std::min(sharpest, std::acos(std::clamp((ax * bx + az * bz) / d, -1., 1.)));
  }

  return sharpest;
}

// A piece a house can stand on: four corners or more, the width across
// every edge normal and every corner above the ward's minimums.
struct HouseRule
{
  double area = 30, width = 3.5, corner = .52;

  bool holds(const ConvexRing &p) const
  {
    return p.size() >= 4 && std::abs(signedArea(p)) >= area && narrowestWidth(p) >= width && sharpestCorner(p) >= corner;
  }

  // A half that will be cut again only needs the width and corners; the area
  // check comes when it becomes a building.
  bool shapely(const ConvexRing &p) const
  {
    return p.size() >= 3 && narrowestWidth(p) >= width && sharpestCorner(p) >= corner;
  }
};

// Sharp corners of a block are cut off before the bisection: the tip across
// the corner's bisector where the block is `width` wide becomes yard, so no
// building inherits the needle (Watabou leaves them; the request forbids them).
ConvexRing trimSharpCorners(ConvexRing p, double width, double minimumCorner, std::vector<ConvexRing> &yards)
{
  for (unsigned pass = 0; pass < 6; ++pass)
  {
    bool trimmed = false;

    for (size_t k = 0; k < p.size() && !trimmed; ++k)
    {
      const Position u = p[(k + p.size() - 1) % p.size()], v = p[k], w = p[(k + 1) % p.size()];
      const double al = distance(u, v), bl = distance(w, v);

      if (al < 1e-9 || bl < 1e-9)
      {
        continue;
      }

      const double ax = (u.x - v.x) / al, az = (u.z - v.z) / al, bx = (w.x - v.x) / bl, bz = (w.z - v.z) / bl;
      const double angle = std::acos(std::clamp(ax * bx + az * bz, -1., 1.));

      if (angle >= minimumCorner)
      {
        continue;
      }

      double dx = ax + bx, dz = az + bz;
      const double dl = std::hypot(dx, dz);

      if (dl < 1e-9)
      {
        continue;
      }

      dx /= dl;
      dz /= dl;
      const double reach = width / (2 * std::max(1e-3, std::sin(angle * .5)));
      const double c = dx * (v.x + dx * reach) + dz * (v.z + dz * reach);
      ConvexRing tip = clipHalfPlane(p, dx, dz, c), rest = clipHalfPlane(p, -dx, -dz, -c);

      if (rest.size() < 3 || area(rest) < area(p) * .5)
      {
        continue;
      }

      if (tip.size() >= 3 && area(tip) > 1e-6)
      {
        yards.push_back(tip);
      }

      p = rest;
      trimmed = true;
    }

    if (!trimmed)
    {
      break;
    }
  }

  return p;
}

// Area over the smallest rectangle aligned with one of the piece's edges: 1
// for a rectangle, small for a wedge or a sliver.
double rectangularity(const ConvexRing &p)
{
  const double a = area(p);

  if (p.size() < 3 || a < 1e-9)
  {
    return 0;
  }

  double smallest = 1e30;

  for (size_t k = 0; k < p.size(); ++k)
  {
    const Position u = p[k], v = p[(k + 1) % p.size()];
    const double l = distance(u, v);

    if (l < 1e-9)
    {
      continue;
    }

    const double ex = (v.x - u.x) / l, ez = (v.z - u.z) / l;
    double lo = 1e30, hi = -1e30, nlo = 1e30, nhi = -1e30;

    for (auto q : p)
    {
      const double along = ex * q.x + ez * q.z, across = -ez * q.x + ex * q.z;
      lo = std::min(lo, along);
      hi = std::max(hi, along);
      nlo = std::min(nlo, across);
      nhi = std::max(nhi, across);
    }

    smallest = std::min(smallest, (hi - lo) * (nhi - nlo));
  }

  return smallest > 1e-9 ? std::min(1., a / smallest) : 0;
}

// Whether a piece keeps a frontage: an edge of at least three metres along
// the block's outline (a street) or, when the cut left an alley, along the cut.
bool frontage(const ConvexRing &half, const ConvexRing &block, Position c1, Position c2, bool alley)
{
  for (size_t i = 0; i < half.size(); ++i)
  {
    const Position a = half[i], b = half[(i + 1) % half.size()];
    const double l = distance(a, b);

    if (l < 3)
    {
      continue;
    }

    for (size_t k = 0; k < block.size(); ++k)
    {
      const Position u = block[k], v = block[(k + 1) % block.size()];
      const double m = distance(u, v);

      if (m < 1e-9)
      {
        continue;
      }

      if (std::abs(cross(u, v, a)) / m < .05 && std::abs(cross(u, v, b)) / m < .05)
      {
        return true;
      }
    }

    if (alley)
    {
      const double m = distance(c1, c2);

      if (m > 1e-9 && std::abs(cross(c1, c2, a)) / m < .8 && std::abs(cross(c1, c2, b)) / m < .8)
      {
        return true;
      }
    }
  }

  return false;
}

// Ward.createAlleys, exactly: bisect at the longest edge, halves below
// minSq (spread by the size chaos) are buildings unless empty, larger halves
// are cut again, with an alley while they are large.
void createAlleys(
    const ConvexRing &p,
    const ConvexRing &block,
    double minSq,
    double gridChaos,
    double sizeChaos,
    double emptyProb,
    bool split,
    double alley,
    const HouseRule &rule,
    Draws &draws,
    WardBuildingsPlan &r,
    unsigned depth)
{
  if (p.size() < 3 || depth > 40)
  {
    return;
  }

  auto settle = [&](const ConvexRing &piece) {
    // The piece is a building when a house fits it, a yard otherwise.
    if (rule.holds(piece))
    {
      if (!draws.chance(emptyProb))
      {
        r.buildings.push_back(piece);
      }
      else
      {
        ++r.dropped;
      }
    }
    else
    {
      r.open.push_back(piece);
    }
  };

  // The cut is chosen, not drawn: every edge at least half as long as the
  // longest, cut square to it at a few ratios, is scored, and the best cut
  // wins. The score rewards rectangular halves (area over the smallest
  // edge-aligned rectangle), halves that keep a street or alley frontage and
  // balanced areas; a half without a house's width or corners disqualifies
  // the cut. The ward's grid chaos only jitters the tie-break, so a chaotic
  // ward still varies from block to block without a purposeless cut.
  const double gap = split ? alley : 0.;
  const double longest = distance(p[longestEdge(p)], p[(longestEdge(p) + 1) % p.size()]);
  std::vector<ConvexRing> halves;
  double bestScore = -1e30;
  Position alleyStart{}, alleyDirection{};

  for (size_t e = 0; e < p.size(); ++e)
  {
    const Position ea = p[e], eb = p[(e + 1) % p.size()];
    const double el = distance(ea, eb);

    if (el < longest * .5 || el < rule.width * 2)
    {
      continue;
    }

    for (double cutRatio : {.35, .42, .5, .58, .65})
    {
      std::vector<ConvexRing> attempt = bisect(p, e, cutRatio, 0., gap);
      ++r.cuts;

      if (attempt.size() != 2 || !rule.shapely(attempt[0]) || !rule.shapely(attempt[1]))
      {
        continue;
      }

      const double a0 = area(attempt[0]), a1 = area(attempt[1]);
      // The cut line, for the alley frontage of both halves.
      const Position p1 = lerp(ea, eb, cutRatio), p2{p1.x - (eb.z - ea.z), 0, p1.z + (eb.x - ea.x)};
      double score = rectangularity(attempt[0]) + rectangularity(attempt[1]) - .6 * std::abs(a0 - a1) / (a0 + a1);
      score += .5 * (frontage(attempt[0], block, p1, p2, gap > 0) + frontage(attempt[1], block, p1, p2, gap > 0));
      score += .05 * gridChaos * draws.next();

      if (score > bestScore)
      {
        bestScore = score;
        halves = std::move(attempt);
        alleyStart = p1;
        alleyDirection = {p2.x-p1.x,0,p2.z-p1.z};
      }
    }
  }

  if (halves.empty())
  {
    settle(p);
    return;
  }

  if (gap > 0)
  {
    std::vector<Position> hits;
    for (size_t i=0;i<p.size();++i)
    {
      const auto a=p[i], b=p[(i+1)%p.size()];
      const double da=(a.x-alleyStart.x)*alleyDirection.z-(a.z-alleyStart.z)*alleyDirection.x;
      const double db=(b.x-alleyStart.x)*alleyDirection.z-(b.z-alleyStart.z)*alleyDirection.x;
      if ((da<=0 && db>=0) || (db<=0 && da>=0))
        if (std::abs(da-db)>1e-9) hits.push_back(lerp(a,b,da/(da-db)));
    }
    if (hits.size()>=2) r.alleys.push_back({hits.front(),hits.back()});
  }

  for (const auto &half : halves)
  {
    if (area(half) < minSq * std::pow(2., 4 * sizeChaos * (draws.next() - .5)))
    {
      settle(half);
    }
    else
    {
      const double a = draws.next(), c = draws.next();
      createAlleys(half, block, minSq, gridChaos, sizeChaos, emptyProb, area(half) > minSq / std::max(1e-9, a * c), alley, rule, draws, r, depth + 1);
    }
  }
}

// Signed distance of (x0, z0) to the line through (x1, z1) with direction (dx, dz) (GeomUtils.distance2line).
double distanceToLine(double x1, double z1, double dx, double dz, double x0, double z0)
{
  return (dx * z0 - dz * x0 + (z1 + dz) * x1 - (x1 + dx) * z1) / std::sqrt(dx * dx + dz * dz);
}

} // namespace

WardBuildings::WardBuildings(uint64_t domain) : Generator("city.ward-buildings", domain, "city.ward-buildings-input", "city.ward-buildings-plan", GenerationScope::Patch)
{
}

std::vector<ConvexRing> WardBuildings::orthoBuilding(const ConvexRing &poly, double minBlockSq, double fill, uint64_t key, const std::function<double(uint64_t, uint64_t)> &random)
{
  Draws draws{key, 0, random};

  if (area(poly) < minBlockSq)
  {
    return {poly};
  }

  const size_t e = longestEdge(poly);
  const Position c1{poly[(e + 1) % poly.size()].x - poly[e].x, 0, poly[(e + 1) % poly.size()].z - poly[e].z};
  const Position c2{-c1.z, 0, c1.x};

  std::function<std::vector<ConvexRing>(const ConvexRing &, unsigned)> slice = [&](const ConvexRing &p, unsigned depth) {
    std::vector<ConvexRing> buildings;

    if (p.size() < 3 || depth > 24)
    {
      return buildings;
    }

    const size_t v0 = longestEdge(p);
    const Position a = p[v0], b = p[(v0 + 1) % p.size()];
    const double vx = b.x - a.x, vz = b.z - a.z;
    const double ratio = .4 + draws.next() * .2;
    const Position p1 = lerp(a, b, ratio);
    const Position c = std::abs(vx * c1.x + vz * c1.z) < std::abs(vx * c2.x + vz * c2.z) ? c1 : c2;
    const std::vector<ConvexRing> halves = cut(p, p1, {p1.x + c.x, 0, p1.z + c.z}, 0);

    if (halves.size() == 1)
    {
      if (draws.chance(fill))
      {
        buildings.push_back(p);
      }

      return buildings;
    }

    for (const auto &half : halves)
    {
      if (area(half) < minBlockSq * std::pow(2., draws.normal() * 2 - 1))
      {
        if (draws.chance(fill))
        {
          buildings.push_back(half);
        }
      }
      else
      {
        const auto more = slice(half, depth + 1);
        buildings.insert(buildings.end(), more.begin(), more.end());
      }
    }

    return buildings;
  };

  for (unsigned attempt = 0; attempt < 8; ++attempt)
  {
    auto blocks = slice(poly, 0);

    if (!blocks.empty())
    {
      return blocks;
    }
  }

  return {poly};
}

bool WardBuildings::plan(const WardBuildingsInput &s, WardBuildingsPlan &r, Ledger &ledger, std::string *error)
{
  auto fail = [&](const std::string &message) {
    ledger.add(LedgerKind::Rejected, 0, "ward buildings: " + message);

    if (error != nullptr)
    {
      *error = message;
    }

    return false;
  };

  r = WardBuildingsPlan{};
  ConvexRing patch = s.patch;

  if (patch.size() < 3 || !s.random || s.minSq <= 0)
  {
    return fail("a ward needs a patch, a minSq and a random source");
  }

  if (signedArea(patch) < 0)
  {
    std::reverse(patch.begin(), patch.end());
  }

  if (!s.edgeInset.empty() && s.edgeInset.size() != s.patch.size())
  {
    return fail("one inset per patch edge");
  }

  // getCityBlock: every edge moved inward by its inset (Polygon.shrink).
  ConvexRing block = patch;

  for (size_t i = 0; i < patch.size() && !s.edgeInset.empty() && !block.empty(); ++i)
  {
    const double d = signedArea(s.patch) < 0 ? s.edgeInset[(patch.size() - 1 - i) % patch.size()] : s.edgeInset[i];

    if (d <= 0)
    {
      continue;
    }

    const Position a = patch[i], b = patch[(i + 1) % patch.size()];
    const double l = distance(a, b);

    if (l < 1e-9)
    {
      continue;
    }

    const double nx = (b.z - a.z) / l, nz = (a.x - b.x) / l; // Outward for CCW.
    block = clipHalfPlane(block, nx, nz, nx * a.x + nz * a.z - d);
  }

  if (block.size() < 3 || area(block) < 1)
  {
    return fail("the block vanished in its insets");
  }

  r.block = block;
  Draws draws{s.key, 0, s.random};
  const HouseRule rule{s.minimumBuildingArea, s.minimumBuildingWidth, s.minimumCornerRadians};
  block = trimSharpCorners(block, s.minimumBuildingWidth, s.minimumCornerRadians, r.open);
  r.block = block;

  if (block.size() < 3 || area(block) < 1)
  {
    return fail("the block vanished trimming its corners");
  }

  createAlleys(block, block, std::max(s.minSq, s.minimumBuildingArea), s.gridChaos, s.sizeChaos, s.emptyProb, true, s.alley, rule, draws, r, 0);

  // A later alley can end on an earlier zero-gap lot boundary. Reserve its
  // junction cap in that neighbouring lot too, before declaring it buildable.
  for (auto &building:r.buildings) for (const auto &lane:r.alleys)
    for (const auto node:lane)
    {
      if (building.size()<3) break;
      double nearest=1e100;
      Position centre{};
      for (size_t k=0;k<building.size();++k) {
        nearest=std::min(nearest,segmentDistance(node,building[k],building[(k+1)%building.size()]));
        centre.x+=building[k].x/building.size(); centre.z+=building[k].z/building.size();
      }
      const double radius=s.alley*.5,length=distance(centre,node);
      if (nearest>=radius || length<=radius) continue;
      const double nx=(node.x-centre.x)/length,nz=(node.z-centre.z)/length;
      building=clipHalfPlane(building,nx,nz,nx*node.x+nz*node.z-radius);
    }

  // Only pieces that hold a small house are buildings; the rest are yards.
  {
    std::vector<ConvexRing> houses;

    for (auto &building : r.buildings)
    {
      if (building.size() < 4 || area(building) < s.minimumBuildingArea || narrowestWidth(building) < s.minimumBuildingWidth || sharpestCorner(building) < s.minimumCornerRadians)
      {
        r.open.push_back(building);
      }
      else
      {
        houses.push_back(building);
      }
    }

    r.buildings = std::move(houses);
  }

  if (s.outskirts && !s.populated.empty())
  {
    // filterOutskirts: a building stays when a fuzzy unit draw beats its
    // nearest normalised distance to a populated edge, divided by the ward's
    // vertex density interpolated at the building's centre.
    struct Edge
    {
      double x, z, dx, dz, d;
    };

    std::vector<Edge> edges;

    for (const auto &e : s.populated)
    {
      const double dx = e.b.x - e.a.x, dz = e.b.z - e.a.z;
      double deepest = 0;

      for (auto v : patch)
      {
        const bool end = distance(v, e.a) < 1e-9 || distance(v, e.b) < 1e-9;
        const double d = (end ? 0 : std::abs(distanceToLine(e.a.x, e.a.z, dx, dz, v.x, v.z))) * e.factor;
        deepest = std::max(deepest, d);
      }

      edges.push_back({e.a.x, e.a.z, dx, dz, deepest});
    }

    std::vector<ConvexRing> kept;

    for (const auto &building : r.buildings)
    {
      double minDist = 1;

      for (const auto &edge : edges)
      {
        for (auto v : building)
        {
          const double d = std::abs(distanceToLine(edge.x, edge.z, edge.dx, edge.dz, v.x, v.z));
          const double dist = edge.d > 1e-9 ? d / edge.d : 1;
          minDist = std::min(minDist, dist);
        }
      }

      Position c{0, 0, 0};

      for (auto v : building)
      {
        c.x += v.x / building.size();
        c.z += v.z / building.size();
      }

      // Polygon.interpolate: inverse-distance weights over the patch vertices.
      double sum = 0, p = 0;
      std::vector<double> weights;

      for (auto v : patch)
      {
        const double w = 1 / std::max(1e-9, distance(v, c));
        weights.push_back(w);
        sum += w;
      }

      for (size_t j = 0; j < patch.size() && j < s.vertexDensity.size(); ++j)
      {
        p += s.vertexDensity[j] * weights[j] / sum;
      }

      minDist = p > 1e-9 ? minDist / p : 1e30;

      if (draws.normal() > minDist)
      {
        kept.push_back(building);
      }
      else
      {
        ++r.filtered;
      }
    }

    r.buildings = std::move(kept);
  }

  ledger.add(
      LedgerKind::Repaired,
      0,
      "ward buildings: " + std::to_string(r.buildings.size()) + " buildings from " + std::to_string(r.cuts) + " cuts, " + std::to_string(r.dropped) + " empty, " + std::to_string(r.filtered) + " thinned");
  return true;
}

bool WardBuildings::run(const WardBuildingsInput &input, WardBuildingsPlan &output, Ledger &ledger, std::string *error)
{
  return plan(input, output, ledger, error);
}

} // namespace procedural::world::generator
