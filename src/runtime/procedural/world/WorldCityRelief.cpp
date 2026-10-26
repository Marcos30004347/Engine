#include "WorldCityRampTerrain.hpp"
#include "WorldCityRelief.hpp"
#include "WorldCityStreetProfiles.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>

namespace procedural::world::rewrite {
namespace {

uint64_t mix(uint64_t v)
{
  v ^= v >> 33;
  v *= 0xff51afd7ed558ccdull;
  v ^= v >> 33;
  v *= 0xc4ceb9fe1a85ec53ull;
  v ^= v >> 33;

  return v;
}

double hashUnit(const uint64_t seed, const int64_t x, const int64_t y)
{
  return double(mix(seed ^ mix(uint64_t(x) * 0x9e3779b97f4a7c15ull ^ uint64_t(y) * 0xc2b2ae3d27d4eb4full)) >> 11u) / 9007199254740992.;
}

double smoothStep(const double t)
{
  const double c = std::clamp(t, 0., 1.);

  return c * c * (3. - 2. * c);
}

double valueNoise(const uint64_t seed, const double x, const double y)
{
  const double fx = std::floor(x), fy = std::floor(y);
  const int64_t ix = int64_t(fx), iy = int64_t(fy);
  const double tx = smoothStep(x - fx), ty = smoothStep(y - fy);
  const double v00 = hashUnit(seed, ix, iy), v10 = hashUnit(seed, ix + 1, iy);
  const double v01 = hashUnit(seed, ix, iy + 1), v11 = hashUnit(seed, ix + 1, iy + 1);
  const double top = v00 + (v10 - v00) * tx, bottom = v01 + (v11 - v01) * tx;

  return top + (bottom - top) * ty;
}

double distanceToSegment(const double px, const double pz, const double ax, const double az, const double bx, const double bz, double *along)
{
  const double dx = bx - ax, dz = bz - az;
  const double length2 = dx * dx + dz * dz;
  double t = length2 > 1e-12 ? ((px - ax) * dx + (pz - az) * dz) / length2 : 0.;
  t = std::clamp(t, 0., 1.);

  if (along != nullptr)
  {
    *along = t;
  }

  const double qx = ax + dx * t, qz = az + dz * t;

  return std::hypot(px - qx, pz - qz);
}

// Positive inside, negative outside. Even-odd containment with the unsigned
// distance to the boundary, so a slightly self-touching ring still answers.
double signedDepth(const std::vector<Position> &ring, const double x, const double z)
{
  if (ring.size() < 3)
  {
    return -1e30;
  }

  bool in = false;
  double nearest = 1e30;

  for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
  {
    const auto &a = ring[j];
    const auto &b = ring[i];

    if ((a.z > z) != (b.z > z) && x < (b.x - a.x) * (z - a.z) / (b.z - a.z + 1e-300) + a.x)
    {
      in = !in;
    }

    nearest = std::min(nearest, distanceToSegment(x, z, a.x, a.z, b.x, b.z, nullptr));
  }

  return in ? nearest : -nearest;
}

Position centroidOf(const std::vector<Position> &ring)
{
  Position c;

  if (ring.empty())
  {
    return c;
  }

  for (const auto &p : ring)
  {
    c.x += p.x;
    c.z += p.z;
  }

  c.x /= double(ring.size());
  c.z /= double(ring.size());

  return c;
}

double pathLength(const std::vector<Position> &path)
{
  double sum = 0;

  for (size_t i = 1; i < path.size(); ++i)
  {
    sum += std::hypot(path[i].x - path[i - 1].x, path[i].z - path[i - 1].z);
  }

  return sum;
}

// One authority over the urban surface: inside it the height is exactly what
// it says, and its influence falls away with distance outside.
struct Authority
{
  enum class Kind : uint8_t
  {
    Road,
    Pad
  } kind = Kind::Road;

  // Road: the segment and its half width, heights at each end.
  // Pad: the ring, its level, and the band the retaining wall occupies.
  std::vector<Position> ring;
  uint64_t owner = 0;
  double ax = 0, az = 0, bx = 0, bz = 0, halfWidth = 0;
  double heightA = 0, heightB = 0, level = 0, band = 0;
  double minX = 0, minZ = 0, maxX = 0, maxZ = 0;

  double distance(const double x, const double z, double *height) const
  {
    if (kind == Kind::Road)
    {
      double t = 0;
      const double d = distanceToSegment(x, z, ax, az, bx, bz, &t);
      *height = heightA + (heightB - heightA) * t;

      return std::max(0., d - halfWidth);
    }

    *height = level;
    const double depth = signedDepth(ring, x, z);

    return std::max(0., band - depth);
  }
};

// The field is asked once per mesh vertex, so the candidates a sample can
// actually reach are bucketed rather than rediscovered.
struct AuthorityIndex
{
  std::vector<Authority> authorities;
  std::vector<std::vector<uint32_t>> cells;
  double originX = 0, originZ = 0, cell = 24.;
  // How far a street's level still speaks for the ground beside it. A block
  // interior beyond this reads the hill, which is what an unbuilt yard does.
  double roadReach = 26.;
  // How far a lot's own level speaks for the ground outside it: the verge
  // between a terrace and its street belongs to both.
  double padReach = 10.;
  uint32_t columns = 0, rows = 0;

  const std::vector<uint32_t> *at(const double x, const double z) const
  {
    if (columns == 0 || rows == 0)
    {
      return nullptr;
    }

    const int64_t cx = int64_t(std::floor((x - originX) / cell));
    const int64_t cz = int64_t(std::floor((z - originZ) / cell));

    if (cx < 0 || cz < 0 || cx >= int64_t(columns) || cz >= int64_t(rows))
    {
      return nullptr;
    }

    return &cells[size_t(cz) * columns + size_t(cx)];
  }
};

// The Lipschitz envelope of a seeded graph.
//
// Dijkstra from every node at once: the queue starts holding each node's own
// seed, and relaxing an edge offers the neighbour that value plus the edge's
// reach. What settles is the largest function below every seed that never
// climbs faster than the reaches allow. Negate the seeds and you get the
// smallest function above them; the mean of the two is admissible too, and is
// the one that balances cut against fill.
//
// Used twice: once over the road network, where the reach is a street's
// allowed grade times its length, and once over the lots, where it is how far
// a doorway may stand above the ground beside it.
using ReachGraph = std::unordered_map<uint64_t, std::vector<std::pair<uint64_t, double>>>;

std::unordered_map<uint64_t, double> lipschitzEnvelope(const std::unordered_map<uint64_t, double> &seed, const ReachGraph &neighbours, const bool upper)
{
  std::unordered_map<uint64_t, double> best;
  std::multimap<double, uint64_t> queue;

  for (const auto &[id, g] : seed)
  {
    const double value = upper ? g : -g;
    best[id] = value;
    queue.emplace(value, id);
  }

  while (!queue.empty())
  {
    const auto entry = queue.begin();
    const double value = entry->first;
    const uint64_t id = entry->second;
    queue.erase(entry);

    if (value > best[id] + 1e-12)
    {
      continue;
    }

    auto found = neighbours.find(id);

    if (found == neighbours.end())
    {
      continue;
    }

    for (const auto &[other, reach] : found->second)
    {
      const double offered = value + reach;
      auto seen = best.find(other);

      if (seen == best.end() || offered < seen->second - 1e-9)
      {
        best[other] = offered;
        queue.emplace(offered, other);
      }
    }
  }

  if (!upper)
  {
    for (auto &[id, value] : best)
    {
      value = -value;
    }
  }

  return best;
}

// The balanced admissible function: cut and fill share the difference.
std::unordered_map<uint64_t, double> settleLevels(const std::unordered_map<uint64_t, double> &seed, const ReachGraph &neighbours)
{
  const auto upper = lipschitzEnvelope(seed, neighbours, true);
  const auto lower = lipschitzEnvelope(seed, neighbours, false);
  std::unordered_map<uint64_t, double> settled;

  for (const auto &[id, g] : seed)
  {
    auto u = upper.find(id), l = lower.find(id);
    settled[id] = u != upper.end() && l != lower.end() ? (u->second + l->second) * .5 : g;
  }

  return settled;
}

} // namespace

std::function<double(double, double)> cityReliefSurface(
    std::function<double(double, double)> base,
    const CityReliefNoise &noise,
    const double centreX,
    const double centreZ,
    const double reachMetres,
    const double fadeMetres)
{
  if (!base || !(noise.amplitudeMetres > 0) || !(noise.wavelengthMetres > 1))
  {
    return base;
  }

  const CityReliefNoise n = noise;
  const double reach = std::max(1., reachMetres);
  const double fade = std::max(1., fadeMetres);

  return [base, n, centreX, centreZ, reach, fade](const double x, const double z) {
    double sum = 0, amplitude = 1, total = 0;
    double sx = x / n.wavelengthMetres, sz = z / n.wavelengthMetres;

    for (uint32_t octave = 0; octave < std::max(1u, n.octaves); ++octave)
    {
      sum += (valueNoise(n.seed + octave * 7919u, sx, sz) - .5) * amplitude;
      total += amplitude;
      amplitude *= .5;
      sx *= 2.;
      sz *= 2.;
    }

    // Faded to nothing outside the settlement's reach: past the fade the
    // hinterland is the world map's own surface, unaltered.
    const double distance = std::hypot(x - centreX, z - centreZ);
    const double keep = 1. - smoothStep((distance - reach) / fade);

    double h=(total > 0 ? sum / total : 0.) * 2. * n.amplitudeMetres;
    if(std::isfinite(n.terraceRiseMetres) && n.terraceRiseMetres>0) {
      const double level=h/n.terraceRiseMetres,whole=std::floor(level),fraction=level-whole;
      h=(whole+smoothStep((fraction-.72)/.28))*n.terraceRiseMetres;
    }
    return base(x,z)+h*keep;
  };
}

bool planCityRelief(
    const CitySkeleton &skeleton,
    const std::function<double(double, double)> &sourceSurface,
    const CityReliefSettings &settings,
    CityReliefPlan &out,
    std::string *error)
{
  auto fail = [&](const std::string &why) {
    if (error != nullptr)
    {
      *error = why;
    }

    return false;
  };

  const auto surface=cityRampTerrain(skeleton,sourceSurface,settings.terrainFacetMetres);
  if (!skeleton.revision || !surface)
  {
    return fail("city relief needs a composed skeleton and a surface");
  }

  if (!std::isfinite(settings.streetExcavationMetres) || settings.streetExcavationMetres<0 || !(settings.maximumStreetGrade > 0) || !(settings.levelStepMetres > 0) || !(settings.padSkirtMetres > 0) || !(settings.stepRiseMetres>0) || !(settings.stepGoingMetres>0) || !(settings.landingMetres>0) || !std::isfinite(settings.stepRiseMetres+settings.stepGoingMetres+settings.landingMetres))
  {
    return fail("invalid city relief settings");
  }

  CityReliefPlan plan;
  plan.revision = skeleton.revision;
  plan.padSkirtMetres = settings.padSkirtMetres;
  plan.stepRiseMetres = settings.stepRiseMetres;
  plan.stepGoingMetres = settings.stepGoingMetres;
  plan.maximumWalkGrade = settings.maximumWalkGrade;

  // --- the road network, graded ------------------------------------------
  //
  // A street network on a hill has to satisfy one condition: no two points on
  // it differ in height by more than the grade times the distance you walk
  // between them. That is a Lipschitz condition on the road graph, and it has
  // an exact answer rather than an iterative one.
  //
  // The first attempt did iterate - pull the ends of any edge that was too
  // steep toward each other, repeat - and it did not converge: on a network of
  // two thousand segments the steepest street was still at 93 % after ninety
  // six passes, because a correction has to travel the length of a chain one
  // edge per pass. The envelope below is two Dijkstras and is exact.
  //
  //   upper(v) = min over u of ground(u) + reach(u, v)   - cut to fit
  //   lower(v) = max over u of ground(u) - reach(u, v)   - fill to fit
  //
  // where `reach` is the graph distance weighted by each edge's own allowed
  // grade. Excavated streets use the upper envelope (cut only), then cap
  // it against samples across the full carriageway. The optional legacy
  // mode uses the mean of both envelopes to balance cut against fill.
  std::unordered_map<uint64_t, double> height, ground;
  std::unordered_map<uint64_t, Position> position;
  ReachGraph neighbours;

  for (const auto &node : skeleton.roadNodes)
  {
    const double h = surface(node.position.x, node.position.z);
    height[node.id] = h;
    ground[node.id] = h;
    position[node.id] = node.position;
  }

  // A way whose ground is steeper than any road can be is a stair, and a stair
  // may take the hill as it finds it: it constrains the network far less.
  std::unordered_map<uint64_t, bool> steppedEdge;
  std::unordered_map<uint64_t, double> edgeLength;

  for (const auto &edge : skeleton.roadEdges)
  {
    if (!ground.count(edge.from) || !ground.count(edge.to))
    {
      continue;
    }

    double length = pathLength(edge.path);

    if (length <= 1e-6)
    {
      length = std::hypot(position[edge.to].x - position[edge.from].x, position[edge.to].z - position[edge.from].z);
    }

    // A junction stub of a few centimetres is not a gradient, it is a joint.
    // Measured as one it reported a street at 238 %, because a height
    // difference the solve is entitled to leave over a quarter of a metre is
    // divided by that quarter metre. Below this the edge is a joint: its ends
    // are held together and it is never counted as a slope.
    length = std::max(1., length);
    edgeLength[edge.id] = length;

    // Every way is graded to the same limit, because every way has to be
    // walkable. Letting the steep ones keep the hill's own grade was what
    // made them stairs; without treads that is just a slope nobody can climb.
    // A way whose ground was too steep is still marked - it is paved in stone
    // rather than dirt, which is what a hill town does with its steep lanes -
    // but it is graded like any other street.
    const double groundGrade = std::abs(ground[edge.to] - ground[edge.from]) / length;
    steppedEdge[edge.id] = groundGrade > settings.steppedWayGrade;

    const double stairGrade=.8*settings.stepRiseMetres/(settings.stepGoingMetres+settings.landingMetres/std::max(1u,settings.stepsPerFlight));
    const double reach = (settings.stairStreets && steppedEdge[edge.id]?stairGrade:settings.maximumStreetGrade) * length;
    neighbours[edge.from].push_back({edge.to, reach});
    neighbours[edge.to].push_back({edge.from, reach});
  }

  height = settleLevels(ground,neighbours);
  CityStreetProfilePlan streetBlueprint;
  std::map<uint64_t,const CityStreetProfilePlan::Street *> streetProfiles;
  if(settings.excavateStreets && !settings.stairStreets) {
    if(!planCityStreetProfiles(skeleton,surface,settings,streetBlueprint,error))return false;
    for(const auto &[id,level]:streetBlueprint.junctions)height[id]=level;
    for(const auto &street:streetBlueprint.streets)streetProfiles[street.edge]=&street;
  }

  plan.nodeHeight = height;

  // --- the profile along every street ------------------------------------
  for (const auto &edge : skeleton.roadEdges)
  {
    auto from = height.find(edge.from), to = height.find(edge.to);

    if (from == height.end() || to == height.end())
    {
      continue;
    }

    CityReliefPlan::EdgeProfile profile;
    profile.edge = edge.id;
    profile.halfWidth = edge.halfWidth > 0 ? edge.halfWidth : 1.5;

    std::vector<Position> path = edge.path;

    if (path.size() < 2)
    {
      path = {position[edge.from], position[edge.to]};
    }

    // The same length the solve was given, so the reported grade is the one
    // the constraint actually bounded.
    auto measured = edgeLength.find(edge.id);
    const double total = measured != edgeLength.end() ? measured->second : std::max(1., pathLength(path));
    const double drawn = std::max(1e-6, pathLength(path));
    double travelled = 0;
    profile.path.reserve(path.size());

    for (size_t i = 0; i < path.size(); ++i)
    {
      if (i > 0)
      {
        travelled += std::hypot(path[i].x - path[i - 1].x, path[i].z - path[i - 1].z);
      }

      Position p = path[i];
      p.y = from->second + (to->second - from->second) * (travelled / drawn);
      profile.path.push_back(p);
    }

    profile.grade = std::abs(to->second - from->second) / total;
    if(auto solved=streetProfiles.find(edge.id);solved!=streetProfiles.end()) {
      profile.path=solved->second->path;profile.grade=solved->second->maximumGrade;
    }
    // Decided before the solve, against the ground: the graded profile of a
    // stepped way is within budget by construction, so asking it afterwards
    // would find no stairs at all.
    auto classified = steppedEdge.find(edge.id);
    profile.stepped = classified != steppedEdge.end() && classified->second;

    if (!profile.stepped)
    {
      plan.maximumGrade = std::max(plan.maximumGrade, profile.grade);
    }

    if (profile.stepped)
    {
      plan.steepRoads.insert(edge.id);
      ++plan.steppedWays;
    }

    if(settings.stairStreets && profile.stepped)for(size_t k=1;k<profile.path.size();++k) {
      auto a=profile.path[k-1],b=profile.path[k];if(a.y>b.y)std::swap(a,b);
      const double rise=b.y-a.y,length=std::hypot(b.x-a.x,b.z-a.z);
      if(rise<.25)continue;
      const unsigned steps=unsigned(std::ceil(rise/settings.stepRiseMetres));
      const unsigned breaks=(steps-1)/std::max(1u,settings.stepsPerFlight);
      const double going=(length-(breaks+2)*settings.landingMetres)/steps;
      if(going<settings.stepGoingMetres){++plan.impossibleWays;continue;}
      CityReliefPlan::Stairway stair;stair.edge=edge.id;stair.a=a;stair.b=b;stair.halfWidth=profile.halfWidth;
      double at=settings.landingMetres;stair.bands.push_back({at,a.y+.04});
      for(unsigned step=1;step<=steps;++step) {
        at+=going;const double top=a.y+rise*step/steps+.04;stair.bands.push_back({at,top});
        if(step<steps && step%std::max(1u,settings.stepsPerFlight)==0){at+=settings.landingMetres;stair.bands.push_back({at,top});}
      }
      stair.bands.push_back({length,b.y+.04});plan.stairs.push_back(std::move(stair));
    }
    plan.edges.push_back(std::move(profile));
  }

  // --- lot pads ----------------------------------------------------------
  //
  // Solved together, not one at a time.
  //
  // Deciding each lot's level on its own - from the street it fronts, clamped
  // to a doorstep - left a fifth of the town's platform area with no way onto
  // it from any street, against nothing at all on flat ground. Two lots side
  // by side would take levels metres apart because they were pinned to
  // different things, and a lot no street could serve was set on its own
  // ground as an island. Connectivity is not a property any lot has by
  // itself, so it cannot be decided one lot at a time.
  //
  // It is the same problem the streets have, on a different graph: no two
  // places you can walk between may differ by more than a step. So it gets
  // the same answer. Nodes are lots and the streets that serve them; an edge
  // joins two lots that share a boundary, or a lot to its street; the reach
  // across every edge is one doorstep. The envelope then gives every lot a
  // level that is within a step of each of its neighbours, which makes a
  // block one connected terrace by construction, and within a step of its
  // street wherever the two are compatible at all.
  std::unordered_map<uint64_t, std::vector<uint64_t>> accessNodes;

  for (const auto &access : skeleton.regionAccess)
  {
    accessNodes[access.region].push_back(access.roadNode);
  }

  // The nearest point on the road network, and how high it is there.
  auto nearestRoadPoint = [&](const Position &p, Position &at) {
    double best = 1e30, bestHeight = surface(p.x, p.z);
    at = p;

    for (const auto &profile : plan.edges)
    {
      for (size_t i = 1; i < profile.path.size(); ++i)
      {
        double t = 0;
        const auto &a = profile.path[i - 1];
        const auto &b = profile.path[i];
        const double d = distanceToSegment(p.x, p.z, a.x, a.z, b.x, b.z, &t);

        if (d < best)
        {
          best = d;
          bestHeight = a.y + (b.y - a.y) * t;
          at = {a.x + (b.x - a.x) * t, bestHeight, a.z + (b.z - a.z) * t};
        }
      }
    }

    return bestHeight;
  };

  struct LotSite
  {
    const CitySkeletonRegion *region = nullptr;
    Position centre;
    double own = 0, low = 0, high = 0;
  };

  std::unordered_map<uint64_t, LotSite> sites;
  std::unordered_map<uint64_t, double> seed;
  ReachGraph lotGraph;

  for (const auto &region : skeleton.regions)
  {
    if ((region.kind != CityRegionKind::Lot && region.kind != CityRegionKind::Block) || region.boundary.size() < 3)
    {
      continue;
    }

    LotSite site;
    site.region = &region;
    site.centre = centroidOf(region.boundary);
    site.own = surface(site.centre.x, site.centre.z);
    site.low = site.own;
    site.high = site.own;

    for (const auto &p : region.boundary)
    {
      const double h = surface(p.x, p.z);
      site.low = std::min(site.low, h);
      site.high = std::max(site.high, h);
    }

    sites[region.id] = site;
    seed[region.id] = site.own;
  }

  // Lots that share a boundary. The skeleton records contacts, but sparsely,
  // so the boundaries themselves are asked as well: a lot is adjacent to
  // whatever its own corners stand next to.
  const double doorstep = settings.doorstepMetres;

  auto join = [&](const uint64_t a, const uint64_t b, const double reach) {
    lotGraph[a].push_back({b, reach});
    lotGraph[b].push_back({a, reach});
  };

  for (const auto &contact : skeleton.regionContacts)
  {
    if (contact.kind == CityRegionContactKind::SharedBoundary && sites.count(contact.a) && sites.count(contact.b))
    {
      join(contact.a, contact.b, doorstep);
    }
  }

  {
    std::map<std::pair<int64_t, int64_t>, std::vector<uint64_t>> corners;
    const double cell = 1.5;

    for (const auto &[id, site] : sites)
    {
      for (const auto &p : site.region->boundary)
      {
        corners[{int64_t(std::floor(p.x / cell)), int64_t(std::floor(p.z / cell))}].push_back(id);
      }
    }

    std::set<std::pair<uint64_t, uint64_t>> joined;

    for (auto &[key, ids] : corners)
    {
      std::sort(ids.begin(), ids.end());
      ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

      for (size_t i = 0; i < ids.size(); ++i)
      {
        for (size_t j = i + 1; j < ids.size(); ++j)
        {
          if (joined.insert({ids[i], ids[j]}).second)
          {
            join(ids[i], ids[j], doorstep);
          }
        }
      }
    }
  }

  // The streets fix what they can reach, and the rest is pulled to them.
  //
  // Two phases, because one was wrong in both directions. Deciding every lot
  // from its own street left a thousand back plots that no street serves as
  // islands: a fifth of the town's platform area with no way onto it. Solving
  // every lot together with a doorstep between neighbours fixed that and cost
  // fifteen metres of fill, because forcing two lots twelve metres apart on a
  // thirty per cent slope to within half a metre flattens a whole block.
  //
  // So a lot its street can serve is put at its street, and nothing moves it.
  // A lot no street serves keeps its own ground, but only as far as it may:
  // the walkable envelope of the FIXED lots bounds it above and below, so it
  // ends up within a doorstep chain of ground you can reach, and no further
  // from the hill than that requires.
  std::unordered_map<uint64_t, double> fixed;

  for (auto &[id, site] : sites)
  {
    // Does this way serve this lot?
    //
    // The first version asked whether you could walk from the lot's own
    // ground up to the street - and that is the wrong question, because the
    // lot is not going to stay on its own ground. It is going to be cut or
    // filled to meet the street; that is what terracing is. Asking it the old
    // way refused a lot sitting a metre and a half below a road five metres
    // away, left it on the hillside, and made an island of it: measured, the
    // unreachable platforms were a median of 4.8 m from a road and 1.72 m
    // below it, which is a terrace nobody built rather than a lot nobody can
    // serve.
    //
    // So the question is whether the terrace is one you could build: near
    // enough to be this lot's street, and the cut and fill to reach it within
    // what a lot can hold.
    auto serves = [&](const Position &at, const double there) {
      const double reach = std::hypot(at.x - site.centre.x, at.z - site.centre.z);

      return reach <= settings.streetServiceMetres && there - site.low <= settings.maximumPlinthMetres && site.high - there <= settings.maximumPlinthMetres;
    };

    double level = 0;
    uint32_t samples = 0;
    auto nodes = accessNodes.find(id);

    if (nodes != accessNodes.end())
    {
      for (const uint64_t node : nodes->second)
      {
        auto found = height.find(node);
        auto where = position.find(node);

        if (found != height.end() && where != position.end() && serves(where->second, found->second))
        {
          level += found->second;
          ++samples;
        }
      }
    }

    if (samples == 0)
    {
      Position at{};
      const double nearest = nearestRoadPoint(site.centre, at);

      if (serves(at, nearest))
      {
        level = nearest;
        samples = 1;
      }
    }

    if (samples > 0)
    {
      fixed[id] = level / double(samples);
    }
  }

  const auto ceiling = lipschitzEnvelope(fixed, lotGraph, true);
  const auto floorOf = lipschitzEnvelope(fixed, lotGraph, false);

  for (const auto &[id, site] : sites)
  {
    double level = site.own;
    auto pinned = fixed.find(id);

    if (pinned != fixed.end())
    {
      level = pinned->second;
    }
    else
    {
      auto hi = ceiling.find(id), lo = floorOf.find(id);

      if (hi != ceiling.end() && lo != floorOf.end() && lo->second <= hi->second)
      {
        // As close to its own ground as reaching the fixed lots allows, and
        // never further from it than a lot may be cut or filled: without the
        // second clamp the envelope pulled back land nine metres off the hill
        // to meet a terrace it was chained to.
        level = std::clamp(site.own, lo->second, hi->second);
        level = std::clamp(level, site.own - settings.maximumPlinthMetres, site.own + settings.maximumPlinthMetres);
      }
      else
      {
        ++plan.unservedLots;
      }
    }

    if (site.high - site.low > settings.maximumRetainedMetres)
    {
      ++plan.groundedLots;
      plan.notes.push_back("region " + std::to_string(id) + " spans " + std::to_string(site.high - site.low) + " m and was left on the ground");
      continue;
    }

    const double fill = level - site.low, cut = site.high - level;

    if (fill > settings.maximumPlinthMetres || cut > settings.maximumPlinthMetres)
    {
      ++plan.overBudgetLots;
    }

    const double plinth = std::max(0., fill);
    plan.regionLevel[id] = level;
    plan.regionPlinth[id] = plinth;
    // The bank has to carry whichever is deeper, the fill below or the cut
    // above, and stay walkable doing it.
    plan.regionSkirt[id] = std::max(settings.padSkirtMetres, std::max(std::max(0., fill), std::max(0., cut)) / std::max(.05, settings.maximumBankGrade));
    plan.maximumPlinth = std::max(plan.maximumPlinth, plinth);

    if (plinth > .25)
    {
      ++plan.plinths;
    }

    if (site.region->kind == CityRegionKind::Lot)
    {
      ++plan.terracedLots;
    }
  }

  // --- the published surface ---------------------------------------------
  auto index = std::make_shared<AuthorityIndex>();

  double minX = 1e30, minZ = 1e30, maxX = -1e30, maxZ = -1e30;

  for (const auto &profile : plan.edges)
  {
    for (size_t i = 1; i < profile.path.size(); ++i)
    {
      Authority a;
      a.kind = Authority::Kind::Road;
      a.ax = profile.path[i - 1].x;
      a.az = profile.path[i - 1].z;
      a.bx = profile.path[i].x;
      a.bz = profile.path[i].z;
      a.heightA = profile.path[i - 1].y;
      a.heightB = profile.path[i].y;
      a.halfWidth = profile.halfWidth;
      a.minX = std::min(a.ax, a.bx) - a.halfWidth;
      a.maxX = std::max(a.ax, a.bx) + a.halfWidth;
      a.minZ = std::min(a.az, a.bz) - a.halfWidth;
      a.maxZ = std::max(a.az, a.bz) + a.halfWidth;
      index->authorities.push_back(std::move(a));
    }
  }

  for (const auto &region : skeleton.regions)
  {
    auto level = plan.regionLevel.find(region.id);

    if (level == plan.regionLevel.end() || region.kind != CityRegionKind::Lot || region.boundary.size() < 3)
    {
      continue;
    }

    Authority a;
    a.kind = Authority::Kind::Pad;
    a.ring = region.boundary;
    a.owner = region.id;
    a.level = level->second;
    auto skirt = plan.regionSkirt.find(region.id);
    a.band = skirt != plan.regionSkirt.end() ? skirt->second : settings.padSkirtMetres;
    a.minX = a.maxX = region.boundary.front().x;
    a.minZ = a.maxZ = region.boundary.front().z;

    for (const auto &p : region.boundary)
    {
      a.minX = std::min(a.minX, p.x);
      a.maxX = std::max(a.maxX, p.x);
      a.minZ = std::min(a.minZ, p.z);
      a.maxZ = std::max(a.maxZ, p.z);
    }

    index->authorities.push_back(std::move(a));
  }

  if (index->authorities.empty())
  {
    return fail("city relief found no streets or lots to level");
  }

  for (const auto &a : index->authorities)
  {
    minX = std::min(minX, a.minX);
    maxX = std::max(maxX, a.maxX);
    minZ = std::min(minZ, a.minZ);
    maxZ = std::max(maxZ, a.maxZ);
  }

  index->originX = minX - index->roadReach;
  index->originZ = minZ - index->roadReach;
  index->columns = uint32_t(std::floor((maxX + index->roadReach - index->originX) / index->cell)) + 1;
  index->rows = uint32_t(std::floor((maxZ + index->roadReach - index->originZ) / index->cell)) + 1;
  index->cells.assign(size_t(index->columns) * index->rows, {});

  for (uint32_t i = 0; i < index->authorities.size(); ++i)
  {
    const auto &a = index->authorities[i];
    // A street speaks for the ground within its reach, so it is bucketed with
    // that margin. A pad speaks only for the ground it is: inflating every lot
    // by twenty six metres would put a few thousand of them in every cell and
    // make the field, which is asked once per mesh vertex, quadratic.
    const double margin = a.kind == Authority::Kind::Road ? index->roadReach : index->padReach;
    const int64_t x0 = int64_t(std::floor((a.minX - margin - index->originX) / index->cell));
    const int64_t x1 = int64_t(std::floor((a.maxX + margin - index->originX) / index->cell));
    const int64_t z0 = int64_t(std::floor((a.minZ - margin - index->originZ) / index->cell));
    const int64_t z1 = int64_t(std::floor((a.maxZ + margin - index->originZ) / index->cell));

    for (int64_t cz = std::max<int64_t>(0, z0); cz <= std::min<int64_t>(int64_t(index->rows) - 1, z1); ++cz)
    {
      for (int64_t cx = std::max<int64_t>(0, x0); cx <= std::min<int64_t>(int64_t(index->columns) - 1, x1); ++cx)
      {
        index->cells[size_t(cz) * index->columns + size_t(cx)].push_back(i);
      }
    }
  }

  {
    double low = 1e30, high = -1e30;

    for (const auto &[id, level] : plan.regionLevel)
    {
      low = std::min(low, level);
      high = std::max(high, level);
    }

    for (const auto &[id, h] : height)
    {
      low = std::min(low, h);
      high = std::max(high, h);
    }

    if (low <= high)
    {
      plan.minimumLevel = low;
      plan.maximumLevel = high;
      plan.fallMetres = high - low;
    }
  }

  // The public surface, and the pads cut into it.
  //
  // These are two answers, not one blend, and the first attempt made them one.
  // Blending a pad by inverse square distance leaves the pad dominant at its
  // own boundary - a lot edge half a metre from the flat core still carries
  // weight 3 against a street's 0.007 - so the step never happened inside the
  // retaining band. It happened over the several metres outside the lot, and
  // the band came out very nearly flat: a town of terraces with no walls
  // between them, the whole point missed.
  //
  // So the public surface - streets, blended, falling back to the ground - is
  // computed first, and a pad then overrides it: exactly the level over the
  // flat core, and across the band a smooth handover from the public surface
  // up to that level. The step is spent in the band by construction, which is
  // what makes the band a wall.
  const double roadReach = index->roadReach;
  const double padReach = index->padReach;
  const double bankGrade = std::max(.05, settings.maximumBankGrade);
  auto base = surface;

  plan.ground = [index, base, roadReach, padReach, bankGrade](const double x, const double z) {
    const auto *bucket = index->at(x, z);

    if (bucket == nullptr || bucket->empty())
    {
      return base(x, z);
    }

    double weighted = 0, total = 0;
    const Authority *pad = nullptr;

    for (const uint32_t i : *bucket)
    {
      const auto &a = index->authorities[i];

      if (a.kind == Authority::Kind::Pad)
      {
        if (x < a.minX - padReach || x > a.maxX + padReach || z < a.minZ - padReach || z > a.maxZ + padReach)
        {
          continue;
        }

        // Lots do not overlap, so the first that contains the point is the
        // one whose terrace it is on.
        const double depth = signedDepth(a.ring, x, z);

        if (depth > 0)
        {
          // Lot boundaries overlap; lot SURFACES do not.
          //
          // A region's outline can cross a neighbour's, and the surface
          // builder resolves that by cutting each lot against the ones
          // already placed, walking them in ascending id - so the lowest id
          // owns the overlap. The field was taking whichever pad it met
          // first, which is bucket order, so the ground drawn as one lot's
          // platform was being given another lot's level and rose through the
          // floor of the house standing on it. Eleven hundred houses, and the
          // offending surface was a lot pad in all but a dozen. The field
          // settles ownership the same way the surfaces do.
          if (pad == nullptr || a.owner < pad->owner)
          {
            pad = &a;
          }

          continue;
        }

        // Outside, a pad still speaks for the ground beside it.
        //
        // Without this the strip between a lot and its own street was a blend
        // of the street and the RAW HILL, and a few metres from the kerb the
        // hill wins: the verge sagged a metre and a half below two surfaces
        // that were at the same level, and left a trench you could not cross
        // between a doorway and its road. A pad is as much an authority over
        // the ground near it as a street is.
        const double outside = -depth;

        if (outside >= padReach)
        {
          continue;
        }

        const double fade = 1. - outside / padReach;
        const double pull = fade * fade / (outside * outside + 1e-6);
        weighted += pull * a.level;
        total += pull;

        continue;
      }

      if (x < a.minX - roadReach || x > a.maxX + roadReach || z < a.minZ - roadReach || z > a.maxZ + roadReach)
      {
        continue;
      }

      double h = 0;
      const double d = a.distance(x, z, &h);

      if (d >= roadReach)
      {
        continue;
      }

      // A graded street needs a bank, or it stands in a cliff.
      //
      // The old kernel was inverse square, so a street laid across a hillside
      // reached its own level inside the ribbon and the hill's within two or
      // three metres of the kerb. On a forty per cent slope that is a wall:
      // measured, the ground beside these streets had faces spanning eight
      // and a half metres over two and a half metres of plan, and the gaps
      // between them are where the cracks were.
      //
      // The bank is as wide as the height it has to give up, so its gradient
      // is bounded whatever the hill does. Inside the ribbon the street is
      // exact; at a bank's width away the hill is untouched; between them the
      // fall is spread over ground you could walk.
      const double hill = base(x, z);
      const double bank = std::min(roadReach,std::max(5., std::abs(h - hill) / bankGrade));

      if (d >= bank)
      {
        continue;
      }

      const double falloff = 1. - d / bank;
      const double weight = falloff * falloff;
      weighted += weight * (h - hill);
      total += weight;
    }

    // What the streets add to the hill, never more than all of it.
    const double public_ = base(x, z) + (total > 1. ? weighted / total : weighted);

    if (pad == nullptr)
    {
      return public_;
    }

    // Flat, all of it.
    //
    // The ramp used to be INSIDE the region, running from its boundary to a
    // skirt's depth in. That was survivable at half a metre and became the
    // bug the moment the skirt was sized from the drop: a five metre cut
    // wants a thirteen metre bank, and a twenty metre lot ramped from edge to
    // edge with no flat ground left anywhere on it - so the houses standing
    // on it cut into their own ground. The region a house stands on is flat
    // everywhere inside it, and the bank belongs outside, which is where the
    // blend above already puts it.
    return pad->level;
  };

  // The same blend, stopping before the pads.
  plan.openGround = [index, base, roadReach, bankGrade](const double x, const double z) {
    const auto *bucket = index->at(x, z);

    if (bucket == nullptr || bucket->empty())
    {
      return base(x, z);
    }

    double weighted = 0, total = 0, treadHeight = 0, treadWeight = 0;

    for (const uint32_t i : *bucket)
    {
      const auto &a = index->authorities[i];

      if (a.kind != Authority::Kind::Road || x < a.minX - roadReach || x > a.maxX + roadReach || z < a.minZ - roadReach || z > a.maxZ + roadReach)
      {
        continue;
      }

      double h = 0;
      const double d = a.distance(x, z, &h);

      // Adjacent cut/fill banks must never tilt an occupied road ribbon.
      // Blend only the actual treads at junctions, fading each to its edge.
      if(d<=1e-9) {
        const double centreDistance=distanceToSegment(x,z,a.ax,a.az,a.bx,a.bz,nullptr);
        const double depth=std::max(1e-6,1.-centreDistance/std::max(.01,a.halfWidth));
        const double weight=depth*depth;
        treadHeight+=weight*h;treadWeight+=weight;
      }
      if (d >= roadReach)
      {
        continue;
      }

      // Transition outside the tread, with enough reach for substantial
      // excavation. Exact tread precedence prevents neighboring banks from
      // reintroducing the original hillside cross-slope onto the road.
      const double hill = base(x, z);
      const double bank = std::min(roadReach,std::max(5., std::abs(h - hill) / bankGrade));

      if (d >= bank)
      {
        continue;
      }

      const double falloff = 1. - d / bank;
      // Approach the exact tread height continuously, even when another
      // street's bank reaches this edge. The compact support still returns
      // to untouched terrain at the outside of the bank.
      const double weight = falloff * falloff / std::max(1e-12,d*d);
      weighted += weight * (h - hill);
      total += weight;
    }

    return treadWeight>0 ? treadHeight/treadWeight : base(x, z) + (total > 1. ? weighted / total : weighted);
  };

  // The stair mesh owns the walking surface. Recess its terrain support so
  // coarse terrain triangles cannot poke through flat treads and landings.
  if(!plan.stairs.empty()) {
    const auto stairs=std::make_shared<std::vector<CityReliefPlan::Stairway>>(plan.stairs);
    const auto recess=[stairs](const std::function<double(double,double)> &field) {
      return [field,stairs](double x,double z){
        for(const auto &s:*stairs) {
          if(x<std::min(s.a.x,s.b.x)-s.halfWidth || x>std::max(s.a.x,s.b.x)+s.halfWidth || z<std::min(s.a.z,s.b.z)-s.halfWidth || z>std::max(s.a.z,s.b.z)+s.halfWidth)continue;
          const double dx=s.b.x-s.a.x,dz=s.b.z-s.a.z,len=std::hypot(dx,dz),along=((x-s.a.x)*dx+(z-s.a.z)*dz)/len;
          if(along<0 || along>len || std::abs((x-s.a.x)*dz-(z-s.a.z)*dx)/len>s.halfWidth)continue;
          for(auto band:s.bands)if(along<=band.first)return band.second-.24;
        }
        return field(x,z);
      };
    };
    plan.ground=recess(plan.ground);plan.openGround=recess(plan.openGround);
  }
  out = std::move(plan);

  return true;
}

bool planRaisedCityBlocks(const CitySkeleton &city,
    const std::vector<CityReliefPlan::RaisedBlock> &requests,
    const CityReliefSettings &settings, CityReliefPlan &plan, std::string *error)
{
  auto fail=[&](const char *why){if(error)*error=why;return false;};
  if(!plan.openGround || !std::isfinite(settings.stepRiseMetres) || settings.stepRiseMetres<=0 ||
     !std::isfinite(settings.stepGoingMetres) || settings.stepGoingMetres<=0 ||
     !std::isfinite(settings.landingMetres) || settings.landingMetres<=0 || settings.stepsPerFlight==0)
    return fail("raised blocks require terrain and valid stair dimensions");
  auto candidate=plan;
  for(auto platform:requests) {
    for(const auto &existing:candidate.platforms)if(existing.block==platform.block)return fail("block already has a raised deck");
    if(platform.boundary.size()<3 || !std::isfinite(platform.level) ||
       !std::isfinite(platform.stairHalfWidth) || platform.stairHalfWidth<.9)
      return fail("invalid raised block dimensions");
    bool found=false;
    for(const auto &region:city.regions) {
      if(region.id==platform.block && region.kind==CityRegionKind::Block)found=true;
      if(region.parent!=platform.block || region.kind!=CityRegionKind::Lot)continue;
      for(auto p:region.boundary)if(signedDepth(platform.boundary,p.x,p.z)<-.001)
        return fail("raised block deck does not contain its lots");
      candidate.regionLevel[region.id]=platform.level;
    }
    if(!found)return fail("raised block is not in the city plan");
    double bottom=1e30,highest=-1e30;
    for(size_t i=0;i<platform.boundary.size();++i) {
      auto a=platform.boundary[i],b=platform.boundary[(i+1)%platform.boundary.size()];
      if(!std::isfinite(a.x) || !std::isfinite(a.z))return fail("non-finite deck boundary");
      for(int k=0;k<=8;++k){double t=k/8.;double h=plan.openGround(a.x+(b.x-a.x)*t,a.z+(b.z-a.z)*t);bottom=std::min(bottom,h);highest=std::max(highest,h);}
    }
    double minX=1e30,maxX=-1e30,minZ=1e30,maxZ=-1e30;
    for(auto p:platform.boundary){minX=std::min(minX,p.x);maxX=std::max(maxX,p.x);minZ=std::min(minZ,p.z);maxZ=std::max(maxZ,p.z);}
    for(double x=minX;x<=maxX;x+=2.)for(double z=minZ;z<=maxZ;z+=2.)if(signedDepth(platform.boundary,x,z)>=0) {
      const double h=plan.openGround(x,z);highest=std::max(highest,h);bottom=std::min(bottom,h);
    }
    if(!platform.terrainCut && platform.level<highest+.1)return fail("raised deck must stand above its terrain");
    if(platform.terrainCut && highest-platform.level>3.)return fail("terrace excavation exceeds three metres");
    platform.foundation=std::min(bottom,platform.level)-.3;
    auto path=platform.accessPath;
    if(path.empty())path={platform.accessFrom,platform.accessTo};
    if(path.size()<2 || std::abs(path.back().y-platform.level)>.001 ||
       std::abs(signedDepth(platform.boundary,path.back().x,path.back().z))>.02)
      return fail("stair must terminate on the raised deck boundary");
    platform.accessFrom=path.front();platform.accessTo=path.back();
    double arrival=plan.openGround(path.front().x,path.front().z);
    for(const auto &lower:candidate.platforms)if(signedDepth(lower.boundary,path.front().x,path.front().z)>=-.01)arrival=std::max(arrival,lower.level);
    if(std::abs(arrival-path.front().y)>.1)return fail("stair entrance does not meet terrain or an existing deck");

    for(size_t segment=1;segment<path.size();++segment) {
      auto a=path[segment-1],b=path[segment];
      const double length=std::hypot(b.x-a.x,b.z-a.z),rise=b.y-a.y;
      if(!std::isfinite(length) || !std::isfinite(rise) || length<.01 || rise<0)
        return fail("stair path must be finite and ascending");
      // Stair courts are reserved before houses; never tunnel a flight through
      // an occupied lot. Sample both edges as well as the centreline.
      for(double d=0;d<=length;d+=.4)for(double side:{-platform.stairHalfWidth,0.,platform.stairHalfWidth}) {
        const double x=a.x+(b.x-a.x)*d/length-(b.z-a.z)*side/length;
        const double z=a.z+(b.z-a.z)*d/length+(b.x-a.x)*side/length;
        for(const auto &lot:city.regions)if(lot.kind==CityRegionKind::Lot && signedDepth(lot.boundary,x,z)>.01)
          return fail("stair court intersects an occupied lot");
      }
      if(rise<.001) {
        // An overlapping landing slab closes the elbow between flights.
        const double ux=(b.x-a.x)/length,uz=(b.z-a.z)/length,w=platform.stairHalfWidth;
        const auto at=[&](double t,double side){return Position{a.x+ux*t-uz*side,0,a.z+uz*t+ux*side};};
        candidate.stairLandings.push_back({{at(-w,-w),at(length+w,-w),at(length+w,w),at(-w,w)},a.y+.04,platform.foundation});
        continue;
      }
      const double landing=std::max(settings.landingMetres,platform.stairHalfWidth+.15);
      const unsigned steps=unsigned(std::ceil(rise/settings.stepRiseMetres));
      const unsigned breaks=(steps-1)/settings.stepsPerFlight;
      const double going=(length-(breaks+2)*landing)/steps;
      if(going<settings.stepGoingMetres)return fail("raised block access is too short for stairs and landings");
      CityReliefPlan::Stairway stair;stair.edge=platform.block;stair.a=a;stair.b=b;stair.halfWidth=platform.stairHalfWidth;stair.supported=true;stair.foundation=platform.foundation;
      double distance=landing;stair.bands.push_back({distance,a.y+.04});
      for(unsigned k=1;k<=steps;++k){distance+=going;const double height=a.y+rise*k/steps+.04;stair.bands.push_back({distance,height});
        if(k<steps && k%settings.stepsPerFlight==0){distance+=landing;stair.bands.push_back({distance,height});}}
      stair.bands.push_back({length,b.y+.04});candidate.stairs.push_back(std::move(stair));
    }
    for(const auto &bridge:platform.bridges) {
      const double len=std::hypot(bridge.b.x-bridge.a.x,bridge.b.z-bridge.a.z);
      if(!std::isfinite(len) || !std::isfinite(bridge.halfWidth) || !std::isfinite(bridge.archRise) || !std::isfinite(bridge.thickness) || !std::isfinite(bridge.a.y) || !std::isfinite(bridge.b.y) || len<1 || bridge.halfWidth<.9 || bridge.thickness<.2 || bridge.archRise<0 ||
         std::abs(bridge.a.y-platform.level)>.001 || std::abs(bridge.b.y-platform.level)>.001 ||
         std::abs(signedDepth(platform.boundary,bridge.a.x,bridge.a.z))>.02)
        return fail("deck bridge must connect two supported deck edges");
      bool separateSupport=false;
      for(const auto &other:requests)if(other.block!=platform.block && std::abs(other.level-bridge.b.y)<.001 &&
        std::abs(signedDepth(other.boundary,bridge.b.x,bridge.b.z))<.02)separateSupport=true;
      for(const auto &other:candidate.platforms)if(other.block!=platform.block && std::abs(other.level-bridge.b.y)<.001 &&
        std::abs(signedDepth(other.boundary,bridge.b.x,bridge.b.z))<.02)separateSupport=true;
      if(!separateSupport)return fail("bridge requires two separate elevated platforms; stair-court crossings are not permitted");
      const double ux=(bridge.b.x-bridge.a.x)/len,uz=(bridge.b.z-bridge.a.z)/len;
      for(const auto &stair:candidate.stairs) {
        const double run=std::hypot(stair.b.x-stair.a.x,stair.b.z-stair.a.z);
        const double sx=(stair.b.x-stair.a.x)/run,sz=(stair.b.z-stair.a.z)/run;
        double last=0;
        for(auto band:stair.bands) {
          for(double along:{last,band.first})for(double side:{-stair.halfWidth,0.,stair.halfWidth}) {
            const double x=stair.a.x+sx*along-sz*side-bridge.a.x,z=stair.a.z+sz*along+sx*side-bridge.a.z;
            const double t=(x*ux+z*uz)/len,cross=-x*uz+z*ux;
            if(t<0 || t>1 || std::abs(cross)>bridge.halfWidth)continue;
            return fail("deck bridge crosses a staircase route");
          }
          last=band.first;
        }
      }
    }
    auto owner=std::find_if(candidate.landBlocks.begin(),candidate.landBlocks.end(),[&](const auto &b){return b.block==platform.block;});
    if(owner==candidate.landBlocks.end()) {
      CityReliefPlan::LandBlock block;block.block=platform.block;
      for(const auto &region:city.regions)if(region.id==platform.block)block.boundary=region.boundary;
      candidate.landBlocks.push_back(std::move(block));owner=std::prev(candidate.landBlocks.end());
    }
    owner->kind=CityReliefPlan::LandBlock::Kind::Retained;
    owner->minimum=platform.foundation;owner->maximum=platform.level;owner->lots.clear();
    for(const auto &region:city.regions)if(region.parent==platform.block && region.kind==CityRegionKind::Lot)owner->lots.push_back(region.id);
    owner->accessPath=path;owner->accessHalfWidth=platform.stairHalfWidth;
    candidate.regionLevel[platform.block]=platform.level;candidate.platforms.push_back(std::move(platform));
  }
  // Check the complete batch, including stairs belonging to later requests.
  // A bridge is a district-to-district route, never a roof over stair access.
  for(const auto &deck:candidate.platforms)for(const auto &bridge:deck.bridges) {
    const double len=std::hypot(bridge.b.x-bridge.a.x,bridge.b.z-bridge.a.z);
    const double ux=(bridge.b.x-bridge.a.x)/len,uz=(bridge.b.z-bridge.a.z)/len;
    for(const auto &stair:candidate.stairs) {
      const double run=std::hypot(stair.b.x-stair.a.x,stair.b.z-stair.a.z);
      const double sx=(stair.b.x-stair.a.x)/run,sz=(stair.b.z-stair.a.z)/run;
      for(double along=0;along<=run;along+=.2)for(double side:{-stair.halfWidth,0.,stair.halfWidth}) {
        const double x=stair.a.x+sx*along-sz*side-bridge.a.x,z=stair.a.z+sz*along+sx*side-bridge.a.z;
        const double t=x*ux+z*uz,cross=-x*uz+z*ux;
        if(t>=0 && t<=len && std::abs(cross)<=bridge.halfWidth)return fail("deck bridge crosses a staircase route");
      }
    }
  }
  // Validate actual tread elevations against every platform, including
  // later requests. Endpoints alone cannot detect a flight inside a wall.
  for(const auto &stair:candidate.stairs) {
    const double dx=stair.b.x-stair.a.x,dz=stair.b.z-stair.a.z,len=std::hypot(dx,dz);
    double start=0;
    for(const auto &band:stair.bands) {
      for(double along=start+.02;along<band.first;along+=.2)
      for(double side=-stair.halfWidth+.02;side<stair.halfWidth;side+=.2) {
        const double x=stair.a.x+dx*along/len-dz*side/len,z=stair.a.z+dz*along/len+dx*side/len;
        for(const auto &deck:candidate.platforms)
          if(deck.level+.04>band.second+.08 && deck.foundation<band.second+2.2 && signedDepth(deck.boundary,x,z)>.015)
            return fail("stair clearance intersects a raised block solid; reserve a wider or deeper stair court");
      }
      start=band.first;
    }
  }
  for(const auto &landing:candidate.stairLandings) {
    double x0=1e30,z0=1e30,x1=-1e30,z1=-1e30;
    for(auto p:landing.boundary){x0=std::min(x0,p.x);z0=std::min(z0,p.z);x1=std::max(x1,p.x);z1=std::max(z1,p.z);}
    for(double x=x0+.02;x<x1;x+=.2)for(double z=z0+.02;z<z1;z+=.2)if(signedDepth(landing.boundary,x,z)>.01) {
      for(const auto &deck:candidate.platforms)
        if(deck.level+.04>landing.level+.08 && deck.foundation<landing.level+2.2 && signedDepth(deck.boundary,x,z)>.015)
          return fail("turn landing intersects a raised block solid");
      for(const auto &lot:city.regions)if(lot.kind==CityRegionKind::Lot && signedDepth(lot.boundary,x,z)>.01)
        return fail("turn landing intersects an occupied lot");
    }
  }
  // Explicit excavation changes the terrain sampler, not just the visible
  // wall. Both the support and rendered ground remain beneath the deck.
  const auto cutDecks=std::make_shared<std::vector<CityReliefPlan::RaisedBlock>>(candidate.platforms);
  const auto cut=[cutDecks](const auto &original) {
    return [original,cutDecks](double x,double z) {
      double h=original(x,z);
      // Include the retaining-wall footing so quantized boundary vertices
      // cannot sample the uncut hill and create spikes through the coping.
      for(const auto &deck:*cutDecks)if(deck.terrainCut && signedDepth(deck.boundary,x,z)>=-.2)
        h=std::min(h,deck.level-.12);
      return h;
    };
  };
  candidate.openGround=cut(candidate.openGround);
  if(candidate.ground)candidate.ground=cut(candidate.ground);
  const auto source=candidate.openGround;
  const auto stairs=std::make_shared<std::vector<CityReliefPlan::Stairway>>(candidate.stairs);
  candidate.accessGround=[source,stairs](double x,double z) {
    double height=source(x,z);
    for(const auto &stair:*stairs) {
      const double dx=stair.b.x-stair.a.x,dz=stair.b.z-stair.a.z,len=std::hypot(dx,dz);
      const double along=((x-stair.a.x)*dx+(z-stair.a.z)*dz)/len;
      const double across=std::abs((x-stair.a.x)*dz-(z-stair.a.z)*dx)/len;
      if(along<0 || along>len || across>stair.halfWidth+1.4)continue;
      double floor=stair.b.y+.04;for(auto band:stair.bands)if(along<=band.first){floor=band.second;break;}
      const double blend=std::clamp(stair.halfWidth+1.4-across,0.,1.);
      height=std::min(height,height+(std::min(height,floor-.35)-height)*blend);
    }
    return height;
  };
  plan=std::move(candidate);if(error)error->clear();return true;
}

} // namespace procedural::world::rewrite
