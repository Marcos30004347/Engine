#include "WorldCityCompletion.hpp"
#include "WorldCitySurfaces.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <deque>
#include <set>

namespace procedural::world::rewrite {
namespace {
constexpr double epsilon = 1e-7;

uint64_t mixId(uint64_t h, uint64_t v)
{
  h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  return h;
}

uint64_t stableId(uint64_t domain, uint64_t a, uint64_t b = 0)
{
  return mixId(mixId(0xcbf29ce484222325ULL, domain), mixId(a, b));
}

int64_t quantize(double v) { return std::llround(v * 1000); }

uint64_t pointKey(Position p)
{
  return stableId(uint64_t(quantize(p.x)), uint64_t(quantize(p.y)), uint64_t(quantize(p.z)));
}

double distance2(Position a, Position b)
{
  const double x = a.x - b.x, z = a.z - b.z;
  return x * x + z * z;
}

double distance(Position a, Position b) { return std::sqrt(distance2(a, b)); }

Position closestPoint(Position p, Position a, Position b)
{
  const double x = b.x - a.x, z = b.z - a.z, dd = x * x + z * z;
  const double t = dd <= epsilon ? 0 : std::clamp(((p.x - a.x) * x + (p.z - a.z) * z) / dd, 0., 1.);
  return {a.x + x * t, a.y + (b.y - a.y) * t, a.z + z * t};
}

Position centre(const std::vector<Position> &p)
{
  Position c;
  for (auto v : p) { c.x += v.x; c.y += v.y; c.z += v.z; }
  if (!p.empty()) { c.x /= p.size(); c.y /= p.size(); c.z /= p.size(); }
  return c;
}

// Collapse the wiggle out of a centreline before anything rounds it.
//
// A street grown by the road L-system arrives as a chain of short segments
// that are very nearly collinear, and rounding each of those junctions turns
// a straight road into a slow curve made of forty triangles. Douglas-Peucker
// at a tolerance of about a metre says what a surveyor would: these eight
// points are one straight run. What is left is the bends that are actually
// bends, and those are the only ones worth rounding.
std::vector<Position> straighten(const std::vector<Position> &path, const double tolerance,
                                 const std::function<bool(Position)> &clear)
{
  if (path.size() < 3 || tolerance <= 0)
  {
    return path;
  }

  // A straightened run still has to fit.
  //
  // Simplifying a centreline moves it off its authored line, and the line was
  // authored where it was because a lot is next to it: pulled a metre over,
  // the ribbon crosses the plot and the surface boolean fails outright with
  // "boundary trace produced a hole with no containing ring". The smoother
  // below already refuses a curve it cannot fit; a straightening that does
  // not do the same is just a slower way to break the same thing.
  const auto fits = [&](const Position &a, const Position &b) {
    const double run = std::hypot(b.x - a.x, b.z - a.z);
    const uint32_t steps = std::max(2u, uint32_t(std::ceil(run / 1.5)));

    for (uint32_t k = 0; k <= steps; ++k)
    {
      const double t = double(k) / double(steps);

      if (!clear({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}))
      {
        return false;
      }
    }

    return true;
  };

  std::vector<bool> keep(path.size(), false);
  keep.front() = keep.back() = true;

  std::vector<std::pair<size_t, size_t>> spans{{0, path.size() - 1}};

  while (!spans.empty())
  {
    const auto [from, to] = spans.back();
    spans.pop_back();

    if (to <= from + 1)
    {
      continue;
    }

    const Position &a = path[from];
    const Position &b = path[to];
    const double dx = b.x - a.x, dz = b.z - a.z;
    const double run = std::hypot(dx, dz);

    size_t worst = from;
    double furthest = 0;

    for (size_t i = from + 1; i < to; ++i)
    {
      const double away = run > 1e-9 ? std::abs((path[i].x - a.x) * dz - (path[i].z - a.z) * dx) / run
                                     : std::hypot(path[i].x - a.x, path[i].z - a.z);

      if (away > furthest)
      {
        furthest = away;
        worst = i;
      }
    }

    if (furthest > tolerance || !fits(a, b))
    {
      keep[worst] = true;
      spans.push_back({from, worst});
      spans.push_back({worst, to});
    }
  }

  std::vector<Position> out;
  for (size_t i = 0; i < path.size(); ++i)
  {
    if (keep[i])
    {
      out.push_back(path[i]);
    }
  }

  return out.size() >= 2 ? out : path;
}

std::vector<Position> smoothPath(const std::vector<Position> &path, double halfWidth,
                                 const std::function<bool(Position)> &clear, const uint32_t cornerSteps)
{
  if (path.size() < 3 || cornerSteps == 0) return path;
  // Curve the authored bends BEFORE sampling. Subdividing straight edges to
  // two metres first made three Chaikin passes round only tiny corner tips.
  const auto lerp = [](Position a, Position b, double t) {
    return Position{a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t};
  };
  std::vector<Position> result{path.front()};
  for (size_t i=1;i+1<path.size();++i)
  {
    const auto a=path[i-1], b=path[i], c=path[i+1];
    const double incoming=distance(a,b), outgoing=distance(b,c);
    double reach=std::min({incoming*.45,outgoing*.45,halfWidth*4.});
    bool accepted=false;
    for (; reach>=.125 && !accepted; reach*=.5)
    {
      const auto p=lerp(b,a,reach/incoming), q=lerp(b,c,reach/outgoing);
      std::vector<Position> curve;
      // Low poly: a corner is a few chords, not a sampled arc.
      const uint32_t steps=std::clamp(uint32_t(std::ceil(2*reach/.7)),std::clamp(cornerSteps,2u,32u),32u);
      bool safe=true;
      for (uint32_t k=0;k<=steps;++k)
      {
        const double t=double(k)/steps;
        const auto v=lerp(lerp(p,b,t),lerp(b,q,t),t);
        curve.push_back(v);
        safe = safe && clear(v);
      }
      if (safe) { result.insert(result.end(),curve.begin(),curve.end()); accepted=true; }
    }
    if (!accepted) result.push_back(b);
  }
  result.push_back(path.back());
  return result;
}

double cross(Position a, Position b, Position p)
{
  return (b.x - a.x) * (p.z - a.z) - (b.z - a.z) * (p.x - a.x);
}

bool overlaps(Position a, Position b, Position c, Position d)
{
  const double length = distance(a, b);
  if (length <= epsilon || std::abs(cross(a, b, c)) > 1e-4 * length || std::abs(cross(a, b, d)) > 1e-4 * length)
    return false;
  const double x = (b.x - a.x) / length, z = (b.z - a.z) / length;
  double c0 = (c.x - a.x) * x + (c.z - a.z) * z, c1 = (d.x - a.x) * x + (d.z - a.z) * z;
  if (c0 > c1) std::swap(c0, c1);
  return std::min(length, c1) - std::max(0., c0) > 1e-4;
}

bool inside(Position p, const std::vector<Position> &poly)
{
  bool result = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    if ((poly[i].z > p.z) != (poly[j].z > p.z) && p.x < (poly[j].x - poly[i].x) * (p.z - poly[i].z) / (poly[j].z - poly[i].z) + poly[i].x)
      result = !result;
  return result;
}

double boundaryDistance(Position p, const std::vector<Position> &poly)
{
  double best = 1e100;
  for (size_t i = 0; i < poly.size(); ++i)
    best = std::min(best, distance(p, closestPoint(p, poly[i], poly[(i + 1) % poly.size()])));
  return best;
}

bool sharedWithCastle(Position a, Position b, const CityCastlePlan &castles)
{
  for (const auto &castle : castles.castles)
    for (size_t i = 0; i < castle.enclosure.size(); ++i)
      if (overlaps(a, b, castle.enclosure[i], castle.enclosure[(i + 1) % castle.enclosure.size()]))
        return true;
  return false;
}

std::array<CityWallFace, 6> prismFaces(Position a, Position b, double width, double height)
{
  const double length = distance(a, b);
  const double nx = length > epsilon ? -(b.z - a.z) * width * .5 / length : 0;
  const double nz = length > epsilon ? (b.x - a.x) * width * .5 / length : 0;
  Position al{a.x + nx, a.y, a.z + nz}, ar{a.x - nx, a.y, a.z - nz};
  Position bl{b.x + nx, b.y, b.z + nz}, br{b.x - nx, b.y, b.z - nz};
  Position alt = al, art = ar, blt = bl, brt = br;
  alt.y += height; art.y += height; blt.y += height; brt.y += height;
  return {{{CityWallFaceKind::Outside, {al, bl, blt, alt}},
           {CityWallFaceKind::Inside, {br, ar, art, brt}},
           {CityWallFaceKind::Up, {alt, blt, brt, art}},
           {CityWallFaceKind::Bottom, {ar, br, bl, al}},
           {CityWallFaceKind::Left, {ar, al, alt, art}},
           {CityWallFaceKind::Right, {bl, br, brt, blt}}}};
}

uint64_t nearestRoadNode(const CitySkeleton &skeleton, Position p, double *outDistance = nullptr)
{
  uint64_t id = 0;
  double best = 1e100;
  for (const auto &node : skeleton.roadNodes)
    if (distance2(p, node.position) < best)
    {
      best = distance2(p, node.position);
      id = node.id;
    }
  if (outDistance) *outDistance = id ? std::sqrt(best) : 1e100;
  return id;
}

void addRoadGate(CitySkeleton &skeleton, std::map<uint64_t, size_t> &roadIndex, uint64_t gateId, Position p, uint64_t &nodeId, std::vector<CitySmoothedRoute> *routes=nullptr)
{
  double d = 0;
  const uint64_t nearest = nearestRoadNode(skeleton, p, &d);
  if (nearest && d <= 1e-4)
  {
    nodeId = nearest;
    skeleton.roadNodes[roadIndex.at(nearest)].kind = CityRoadNodeKind::Gate;
    return;
  }
  nodeId = stableId(510, gateId, pointKey(p));
  while (roadIndex.count(nodeId)) nodeId = mixId(nodeId, gateId);
  roadIndex[nodeId] = skeleton.roadNodes.size();
  skeleton.roadNodes.push_back({nodeId, CityRoadNodeKind::Gate, p, {}});
  // A gate on an existing road splits that edge instead of adding a diagonal
  // shortcut to a nearby node (which could cross a neighbouring parcel).
  for(size_t i=0;i<skeleton.roadEdges.size();++i) {
    const auto old=skeleton.roadEdges[i];
    for(size_t k=1;k<old.path.size();++k) {
      if(distance(p,closestPoint(p,old.path[k-1],old.path[k]))>1e-4)continue;
      auto tail=old;tail.id=stableId(511,gateId,old.id);tail.from=nodeId;
      tail.path.assign(old.path.begin()+k,old.path.end());tail.path.insert(tail.path.begin(),p);
      auto &head=skeleton.roadEdges[i];head.to=nodeId;head.path.resize(k);head.path.push_back(p);
      auto &atEnd=skeleton.roadNodes[roadIndex.at(old.to)].edges;
      std::replace(atEnd.begin(),atEnd.end(),old.id,tail.id);
      skeleton.roadNodes[roadIndex.at(nodeId)].edges={old.id,tail.id};
      if(routes)for(auto &route:*routes) {
        auto it=std::find(route.edges.begin(),route.edges.end(),old.id);
        if(it!=route.edges.end())route.edges.insert(it+1,tail.id);
      }
      skeleton.roadEdges.push_back(std::move(tail));return;
    }
  }
  if (!nearest) return;
  CityRoadEdge edge;
  edge.id = stableId(511, gateId, nearest);
  edge.source = gateId;
  edge.from = nearest;
  edge.to = nodeId;
  edge.kind = CityRoadKind::Approach;
  edge.path = {skeleton.roadNodes[roadIndex.at(nearest)].position, p};
  edge.halfWidth = 1.5;
  skeleton.roadEdges.push_back(edge);
  skeleton.roadNodes[roadIndex.at(nearest)].edges.push_back(edge.id);
  skeleton.roadNodes[roadIndex.at(nodeId)].edges.push_back(edge.id);
}
} // namespace

bool realizeCityGrowthWalls(const CityGrowthPlan &growth, const CityCastlePlan &castles,
    const CityCompletionSettings &settings, CityCompletionPlan &out, std::string *error)
{
  auto fail=[&](const char *why){if(error)*error=why;return false;};
  out.skeleton.wallNodes.clear();out.skeleton.wallEdges.clear();out.skeleton.gates.clear();
  out.wallSegments.clear();out.gatehouses.clear();out.towers.clear();
  std::map<uint64_t,size_t> roadIndex,wallIndex;
  for(size_t i=0;i<out.skeleton.roadNodes.size();++i)roadIndex[out.skeleton.roadNodes[i].id]=i;
  std::vector<CityGrowthWall> defensiveWalls = growth.walls;
  uint64_t castleWall = 1000;
  for (const auto &castle : castles.castles)
    if (castle.enclosure.size() >= 3)
    {
      CityGrowthWall wall;
      wall.id = stableId(580, castle.id);
      wall.era = ++castleWall;
      wall.lifecycle = CityWallLifecycle::Standing;
      wall.boundary = castle.enclosure;
      wall.gates = {castle.gate};
      wall.towers = castle.enclosure;
      defensiveWalls.push_back(std::move(wall));
    }

  for (const auto &wall : defensiveWalls)
  {
    if (wall.palisade && !settings.timberDefences) continue;
    const double wallWidth=wall.palisade?.6:settings.wallWidth;
    const double wallHeight=wall.palisade?3.5:settings.wallHeight;
    std::map<uint64_t,double> openingWidths;
    std::map<uint64_t,bool> gatehouseFits;
    if (wall.boundary.size() < 3) return fail("historical wall has no circuit");
    std::vector<std::vector<std::pair<double, std::pair<uint64_t, Position>>>> gatesByEdge(wall.boundary.size());
    for (size_t g = 0; g < wall.gates.size(); ++g)
    {
      double best = 1e100;
      size_t edge = 0;
      Position p;
      for (size_t i = 0; i < wall.boundary.size(); ++i)
      {
        Position q = closestPoint(wall.gates[g], wall.boundary[i], wall.boundary[(i + 1) % wall.boundary.size()]);
        if (distance2(q, wall.gates[g]) < best) { best = distance2(q, wall.gates[g]); edge = i; p = q; }
      }
      const Position a = wall.boundary[edge], b = wall.boundary[(edge + 1) % wall.boundary.size()];
      const double length = distance(a, b);
      const double t = length <= epsilon ? 0 : distance(a, p) / length;
      const auto gateId=stableId(520, wall.id, g+1);
      openingWidths[gateId]=g<wall.gateWidths.size()?wall.gateWidths[g]:settings.gateOpeningWidth;
      gatehouseFits[gateId]=g>=wall.gatehouseFits.size() || wall.gatehouseFits[g];
      gatesByEdge[edge].push_back({t, {gateId, p}});
    }

    for (size_t i = 0; i < wall.boundary.size(); ++i)
    {
      const Position a = wall.boundary[i], b = wall.boundary[(i + 1) % wall.boundary.size()];
      std::vector<std::pair<double, std::pair<uint64_t, Position>>> points{{0, {0, a}}};
      points.insert(points.end(), gatesByEdge[i].begin(), gatesByEdge[i].end());
      points.push_back({1, {0, b}});
      std::sort(points.begin(), points.end(), [](const auto &x, const auto &y) { return x.first < y.first; });
      for (size_t k = 0; k < points.size(); ++k)
      {
        const Position p = points[k].second.second;
        const uint64_t nodeId = stableId(530, wall.id, pointKey(p));
        if (!wallIndex.count(nodeId))
        {
          wallIndex[nodeId] = out.skeleton.wallNodes.size();
          out.skeleton.wallNodes.push_back({nodeId, wall.era, p, {}, {}});
        }
        if (k + 1 == points.size()) continue;
        const Position q = points[k + 1].second.second;
        const uint64_t nextId = stableId(530, wall.id, pointKey(q));
        if (!wallIndex.count(nextId))
        {
          wallIndex[nextId] = out.skeleton.wallNodes.size();
          out.skeleton.wallNodes.push_back({nextId, wall.era, q, {}, {}});
        }
        const uint64_t edgeId = stableId(531, nodeId, nextId);
        out.skeleton.wallEdges.push_back({edgeId, wall.era, nodeId, nextId, p, q});
        out.skeleton.wallNodes[wallIndex[nodeId]].edges.push_back(edgeId);
        out.skeleton.wallNodes[wallIndex[nextId]].edges.push_back(edgeId);

        std::vector<std::pair<double,double>> spans{{points[k].first,points[k+1].first}};
        const double edgeLength=distance(a,b);
        if(wall.openings.empty()) {
          if(points[k].second.first)spans[0].first+=openingWidths.at(points[k].second.first)*.5/edgeLength;
          if(points[k+1].second.first)spans[0].second-=openingWidths.at(points[k+1].second.first)*.5/edgeLength;
        } else for(const auto &opening:wall.openings)if(opening.edge==i) {
          std::vector<std::pair<double,double>> remaining;
          for(auto [lo,hi]:spans) {
            if(opening.to<=lo || opening.from>=hi)remaining.push_back({lo,hi});
            else {
              if(lo<opening.from)remaining.push_back({lo,opening.from});
              if(opening.to<hi)remaining.push_back({opening.to,hi});
            }
          }
          spans=std::move(remaining);
        }
        for(auto [lo,hi]:spans)if((hi-lo)*edgeLength>wallWidth) {
          const Position structureA{a.x+(b.x-a.x)*lo,0,a.z+(b.z-a.z)*lo};
          const Position structureB{a.x+(b.x-a.x)*hi,0,a.z+(b.z-a.z)*hi};
          CityWallStructureSegment segment;
          segment.id=stableId(540,wall.id,wall.openings.empty()?edgeId:stableId(edgeId,pointKey(structureA),pointKey(structureB)));
          segment.wall=wall.id;segment.era=wall.era;segment.lifecycle=wall.lifecycle;
          segment.a=structureA;segment.b=structureB;segment.width=segment.depth=wallWidth;
          segment.palisade=wall.palisade;segment.height=wallHeight;
          segment.built=wall.lifecycle!=CityWallLifecycle::Planned;
          segment.sharedWithCastle=sharedWithCastle(structureA,structureB,castles);
          segment.faces=prismFaces(structureA,structureB,segment.width,segment.height);
          out.wallSegments.push_back(std::move(segment));
        }
      }
      for (const auto &entry : gatesByEdge[i])
      {
        const uint64_t gateId = entry.second.first;
        const Position p = entry.second.second;
        const uint64_t wallNode = stableId(530, wall.id, pointKey(p));
        uint64_t roadNode = 0;
        addRoadGate(out.skeleton, roadIndex, gateId, p, roadNode, &out.routes);
        const uint64_t skeletonGate = stableId(550, wall.id, gateId);
        out.skeleton.gates.push_back({skeletonGate, gateId, wallNode, roadNode, wall.era, p});
        out.skeleton.wallNodes[wallIndex.at(wallNode)].gates.push_back(skeletonGate);
        out.gatehouses.push_back({stableId(551, wall.id, gateId), wall.id, wall.era, wallNode, roadNode, p,
                                  openingWidths.at(gateId), settings.gatehouseDepth, settings.gatehouseHeight,
                                  !wall.palisade && gatehouseFits.at(gateId) && wall.lifecycle != CityWallLifecycle::Planned});
      }
    }
    // One tower per position, not two on the same stone.
    //
    // A wall vertex that survives smoothing next to another leaves two tower
    // sites a couple of metres apart - measured, the closest pair on this
    // circuit stood 1.97 m apart with a 2.5 m radius each, so they were one
    // fused lump in the mesh and an overlapping pair in the audit. Towers
    // closer than their own diameter are the same tower.
    for (size_t i = 0; i < wall.towers.size(); ++i)
    {
      bool shared = false;

      for (const auto &placed : out.towers)
      {
        if (placed.wall != wall.id) continue;
        const double gap = std::hypot(placed.position.x - wall.towers[i].x, placed.position.z - wall.towers[i].z);
        shared = shared || gap < settings.towerRadius * 2.;
      }

      if (shared) continue;

      out.towers.push_back({stableId(552, wall.id, i + 1), wall.id, wall.era, wall.lifecycle, wall.towers[i], settings.towerRadius, settings.towerHeight,
                            wall.lifecycle != CityWallLifecycle::Planned});
    }
  }

  if (growth.historical) {
    out.history.clear();
    for(size_t i=1;i<growth.walls.size();++i)
      out.history.push_back({growth.walls[i-1].id,growth.walls[i].id,growth.walls[i-1].lifecycle});
  }
  return true;
}

bool completeCityPlan(
    const CitySkeleton &base,
    const CityExteriorAllocation &exterior,
    const CityGrowthPlan &growth,
    const CityCastlePlan &castles,
    const CityCompletionSettings &settings,
    CityCompletionPlan &result,
    std::string *error)
{
  auto fail = [&](const char *message) { if (error) *error = message; return false; };
  for (double value : {settings.wallWidth, settings.wallHeight, settings.gateOpeningWidth, settings.gatehouseDepth, settings.gatehouseHeight, settings.suburbDensityDecay,
                       settings.towerRadius, settings.towerHeight, settings.wallClearanceDistance, settings.cemeteryMinimumArea, settings.approachHalfWidth,
                       settings.arterialHalfWidth, settings.localLaneMinimumHalfWidth, settings.localLaneMaximumHalfWidth})
    if (!std::isfinite(value) || value <= 0) return fail("invalid city completion dimensions");
  if (!base.revision || (growth.walls.empty() && !growth.historical)) return fail("city completion requires skeleton and wall history");

  CityCompletionPlan out;
  out.revision = std::max({base.revision, exterior.revision, growth.revision, castles.revision});
  out.skeleton = base;
  out.exterior = exterior;
  out.skeleton.wallNodes.clear();
  out.skeleton.wallEdges.clear();
  out.skeleton.gates.clear();


  const auto standing = std::find_if(growth.walls.begin(), growth.walls.end(), [](const auto &w) { return w.lifecycle == CityWallLifecycle::Standing; });
  const auto planned = std::find_if(growth.traces.begin(), growth.traces.end(), [](const auto &w) { return w.lifecycle == CityWallLifecycle::Planned; });
  if (standing != growth.walls.end())
  {
    // Reserve whole refined parcels rather than inventing a second overlapping
    // cadastral layer. Finer clearance/moat strips can later subdivide these
    // parcels while preserving their source lineage.
    for (auto &parcel : out.exterior.parcels)
      if (parcel.use == CityLandUse::Common || parcel.use == CityLandUse::Pasture)
      {
        const Position p = centre(parcel.boundary);
        if (!inside(p, standing->boundary) && boundaryDistance(p, standing->boundary) <= settings.wallClearanceDistance)
        {
          parcel.use = CityLandUse::WallClearance;
          parcel.tenure = CityLandTenure::Military;
          parcel.reason = "standing-wall clearance reserve";
        }
        else if (planned != growth.traces.end() && !inside(p, standing->boundary) && inside(p, planned->boundary))
        {
          parcel.use = CityLandUse::FutureGrowth;
          parcel.tenure = CityLandTenure::Civic;
          parcel.reason = "land inside the next wall era";
        }
      }
    auto cemetery = out.exterior.parcels.end();
    for (auto it = out.exterior.parcels.begin(); it != out.exterior.parcels.end(); ++it)
      if ((it->use == CityLandUse::Common || it->use == CityLandUse::FutureGrowth) && it->area >= settings.cemeteryMinimumArea && it->accessNode &&
          (cemetery == out.exterior.parcels.end() || it->area > cemetery->area))
        cemetery = it;
    if (cemetery != out.exterior.parcels.end())
    {
      cemetery->use = CityLandUse::Cemetery;
      cemetery->tenure = CityLandTenure::Civic;
      cemetery->reason = "largest accessible extramural civic parcel";
    }
  }

  if (!realizeCityGrowthWalls(growth, castles, settings, out, error)) return false;

  const auto absorbed = std::find_if(growth.traces.begin(), growth.traces.end(), [](const auto &w) { return w.lifecycle == CityWallLifecycle::Absorbed; });
  if (absorbed != growth.traces.end() && standing != growth.walls.end()) out.history.push_back({absorbed->id, standing->id, absorbed->lifecycle});
  if (standing != growth.walls.end() && planned != growth.traces.end()) out.history.push_back({standing->id, planned->id, standing->lifecycle});

  for (const auto &zone : growth.zones)
  {
    if (zone.kind != CityGrowthZoneKind::Suburb) continue;
    CitySuburbTopology suburb;
    suburb.id = stableId(560, zone.id);
    suburb.zone = zone.id;
    suburb.boundary = zone.boundary;
    suburb.accessNode = nearestRoadNode(out.skeleton, centre(zone.boundary), &suburb.gateDistance);
    suburb.density = suburb.accessNode ? std::exp(-suburb.gateDistance / settings.suburbDensityDecay) : 0;
    out.suburbs.push_back(std::move(suburb));
  }

  // Watabou-style outskirts are sparse gate growth, not a cadastral carpet.
  // Keep all lot ownership polygons but publish a deterministic development
  // state whose probability decays from the road network.
  using LotEdge=std::array<int64_t,4>;
  const auto lotEdge=[](Position a,Position b) {
    std::pair<int64_t,int64_t> x{quantize(a.x),quantize(a.z)},y{quantize(b.x),quantize(b.z)};
    if (y<x) std::swap(x,y);
    return LotEdge{x.first,x.second,y.first,y.second};
  };
  std::map<LotEdge,unsigned> frontageEdges;
  for (const auto &r:out.skeleton.regions) if (r.kind==CityRegionKind::Lot)
    for (size_t i=0;i<r.boundary.size();++i) ++frontageEdges[lotEdge(r.boundary[i],r.boundary[(i+1)%r.boundary.size()])];
  // The block commons: which lots are left as the green in the middle.
  //
  // Decided per block before any lot is allocated, because it is a property
  // of the block: in one block in three, the lots without street frontage
  // form the common - but only where they are a minority of the block (a
  // block whose lanes the skeleton does not carry reads as all back land,
  // and that is not a common, that is the block), and only the smallest
  // third of them by area, so a common is a green and not a field. The
  // first version took every frontage-less lot and cost the city 242 houses.
  std::set<uint64_t> commons;
  {
    std::map<uint64_t,std::vector<std::pair<double,uint64_t>>> backland; // block -> (area, lot)
    std::map<uint64_t,unsigned> lotsPerBlock;
    for (const auto &region : out.skeleton.regions)
    {
      if (region.kind != CityRegionKind::Lot) continue;
      ++lotsPerBlock[region.parent];
      bool fronted=false;
      for (size_t i=0;i<region.boundary.size() && !fronted;++i)
      {
        const Position p=region.boundary[i],q=region.boundary[(i+1)%region.boundary.size()];
        const Position m{(p.x+q.x)*.5,0,(p.z+q.z)*.5};
        for (const auto &road:out.skeleton.roadEdges)
        {
          for (size_t k=0;k+1<road.path.size() && !fronted;++k)
            if (distance(m,closestPoint(m,road.path[k],road.path[k+1]))<road.halfWidth+1.) fronted=true;
          if (fronted) break;
        }
      }
      if (!fronted && region.lotProgramme.empty()) backland[region.parent].push_back({region.area,region.id});
    }
    for (auto &[block,lots]:backland)
    {
      if (stableId(733,block)%3!=0 || lots.size()<2 || lots.size()*2>lotsPerBlock[block]) continue;
      std::sort(lots.begin(),lots.end());
      const size_t keep=std::max<size_t>(2,(lots.size()+2)/3);
      for (size_t k=0;k<keep && k<lots.size();++k) commons.insert(lots[k].second);
    }
  }

  uint32_t terraced=0,detachedDespiteParty=0;
  for (const auto &region : out.skeleton.regions)
    if (region.kind == CityRegionKind::Lot)
    {
      CityLotDevelopment lot;
      lot.lot = region.id;
      lot.merchantHouse = region.districtUse==DistrictUse::Merchant || region.districtUse==DistrictUse::Craftsmen;
      const bool enclosed = standing!=growth.walls.end() && inside(centre(region.boundary),standing->boundary);
      // Inside the walls the ground is paved; the mud is for the suburbs.
      if (enclosed && lot.groundCover==CityGroundCover::Automatic) lot.groundCover=CityGroundCover::Stone;
      // The middle of a block is a common, in one block in three.
      //
      // A third of the lots have no street frontage: the splitter cuts a
      // deep block into rows and the rows behind the front ones face nothing
      // but each other. Built on, they are back-land houses reached through
      // somebody's yard. Left open, together, they are the green in the
      // middle of the block that every old town has. So in one block in
      // three inside the walls, the lots without frontage are not developed
      // and the ground is grass; the fences never come, because there is no
      // house to fence, and the block's back yards open onto the common.
      const bool common=enclosed && commons.count(region.id);
      if (common) { lot.groundCover=CityGroundCover::Grass; lot.density=0; }
      const bool smallHouse = region.districtUse==DistrictUse::Suburb;
      // A suburb house is a small house, not a shed. Fifty five square metres
      // of footprint is two rooms and no stair, and on the lots the merge now
      // produces it left the house rattling around inside its own plot.
      // A floor under the request, but never one the lot cannot hold: asking
      // a twelve square metre plot for thirty four is asking for a house that
      // does not fit, and the planner then fits whatever it can anyway.
      lot.preferredFootprintMetres2 = std::clamp(region.area*.78,std::min(34.,region.area),smallHouse ? 90. : 128.);
      lot.desiredRooms = lot.merchantHouse ? 6 : smallHouse ? 2 : region.area>120 ? 10 : 3;
      lot.maximumFloors = smallHouse ? 2 : 4;
      // Estates are residential programmes, not additional fortified castles.
      const bool residential=region.districtUse==DistrictUse::WealthyResidential || region.districtUse==DistrictUse::Manor;
      if (residential && region.area>=180)
      {
        lot.residence=region.area>=420 ? CityResidenceKind::Manor : CityResidenceKind::Mansion;
        lot.preferredFootprintMetres2=lot.residence==CityResidenceKind::Manor ? 200 : 140;
        lot.desiredRooms=lot.residence==CityResidenceKind::Manor ? 28 : 22;
        lot.preferredRoomMetres2=20;
        lot.maximumFloors=4;
      }
      // Small hospitality programmes belong among everyday frontage lots,
      // not in another oversized landmark precinct. Merchant streets carry
      // more of them; a stable lot draw keeps the mix deterministic.
      const bool everyday=region.districtUse==DistrictUse::Residential || lot.merchantHouse || smallHouse || region.districtUse==DistrictUse::WorkerHousing;
      const uint64_t hospitality=stableId(592,region.id)%100;
      if (everyday && region.area>=55 && hospitality<(lot.merchantHouse ? 10u : 6u))
      {
        const bool inn=region.area>=90 && hospitality<2;
        lot.residence=inn ? CityResidenceKind::Inn : CityResidenceKind::Pub;
        lot.preferredFootprintMetres2=std::min(region.area,inn ? 100. : 72.);
        lot.desiredRooms=inn ? 14 : 5;
        lot.preferredRoomMetres2=18;
        lot.maximumFloors=inn ? 4 : 2;
      }
      if (everyday && lot.residence==CityResidenceKind::House)
      {
        // A house, not a hut.
        //
        // Every ordinary house was one storey and two rooms, and its
        // footprint was allowed to shrink to four square metres - so the
        // median house in the town came out at twenty four square metres of
        // single-storey floor, which is a shed with a door. Widening the lots
        // did nothing for this on its own, because what the planner builds is
        // set by what it is ASKED for and not by the ground it is given: the
        // lots got bigger and the same hut stood in the middle of them.
        //
        // Two storeys where the lot can carry it, a room more, and a floor
        // under the footprint request so a house is never asked to be smaller
        // than somebody could live in.
        lot.maximumFloors=region.area>=90 ? 3 : region.area>=52 ? 2 : 1;
        lot.desiredRooms=region.area>=90 ? 5 : 3;
        // A terrace wants a lot that is roughly square-cornered, not one
        // that is square to six thousandths of a degree.
        //
        // The old test asked for exactly four corners and a corner dot
        // product under 1e-4 of the edge lengths, which is what that
        // tolerance works out to - so a lot cut by a bisector at 89.9
        // degrees was not rectangular and did not glue. Almost nothing
        // qualified, and the streets read as detached houses with gaps
        // rather than as a terrace. Fifteen degrees is the tolerance a
        // builder would accept, four to six sides is the shape a bisected
        // block actually produces, and what makes it a TERRACE is the
        // shared party edge, which is tested exactly as before.
        const size_t sides=region.boundary.size();
        bool rectangular=sides>=4 && sides<=6,shared=false;
        for (size_t i=0;i<sides;++i) {
          const auto a=region.boundary[i],b=region.boundary[(i+1)%sides],c=region.boundary[(i+2)%sides];
          const double ab=distance(a,b),bc=distance(b,c);
          if (ab>.05 && bc>.05) {
            const double square=std::abs((b.x-a.x)*(c.x-b.x)+(b.z-a.z)*(c.z-b.z))/(ab*bc);
            rectangular=rectangular && (square<.5 || square>.999);
          }
          shared=shared || frontageEdges[lotEdge(a,b)]>1;
        }
        lot.rowHouse=rectangular && (shared || enclosed) && region.area<=260;
        if (lot.rowHouse) ++terraced; else if (shared) ++detachedDespiteParty;
        if (lot.rowHouse) lot.preferredFootprintMetres2=std::max(4.,region.area);
      }
      const Position p = centre(region.boundary);
      const bool poor=region.districtUse==DistrictUse::Suburb || region.districtUse==DistrictUse::WorkerHousing;
      if(poor && lot.residence==CityResidenceKind::House && !lot.merchantHouse) {
        // A poor house is small, not tiny: it takes the lot up to forty
        // square metres, and never less than twenty of a lot that has it.
        // The old floor of four made huts of four cells on roomy lots.
        lot.preferredFootprintMetres2=enclosed ? std::max(std::min(20.,region.area),region.area*.85)
                                               : std::clamp(region.area*.7,std::min(20.,region.area),40.);
        lot.preferredRoomMetres2=12;
        lot.desiredRooms=2;
      }
      const bool unlimited=residential || region.districtUse==DistrictUse::Castle || region.districtUse==DistrictUse::Citadel;
      lot.minimumFloors=poor ? 1u : 2u;
      lot.maximumFloors=unlimited ? 0u : poor ? 1u : 3u;
      // Poor housing remains one storey; hospitality is a mixed-use building,
      // not a household dwelling, and needs accommodation above its business.
      if (lot.merchantHouse || lot.residence==CityResidenceKind::Inn || lot.residence==CityResidenceKind::Pub)
      { lot.minimumFloors=2; if (!unlimited) lot.maximumFloors=3; }
      bool suburbLot = false;
      double density = 1;
      for (const auto &suburb : out.suburbs)
        if (inside(p, suburb.boundary))
        {
          suburbLot = true;
          density = std::min(density, suburb.density);
        }
      // Sparsity is already authored by ward lots/open ground. Do not discard
      // most of the remaining buildable plots a second time at mesh allocation.
      lot.density = suburbLot ? std::clamp(.85+density*.12,.85,.97) : 1;
      const double draw = double(stableId(590, region.id) % 1000000) / 1000000.;
      lot.developed = !common && (enclosed || !suburbLot || draw < lot.density);
      // The block owns the programme and attachment decision. Completion
      // supplies capacity, without randomly replacing the planned trades.
      if(!region.lotProgramme.empty()) {
        const auto &role=region.lotProgramme;
        lot.rowHouse=region.attached && role!="cottage";
        lot.developed=true;
        lot.residence=CityResidenceKind::House;
        lot.merchantHouse=role=="shop-house" || role=="passage";
        lot.preferredFootprintMetres2=std::min(region.area*.94,160.);
        lot.desiredRooms=region.area>=75 ? 5 : 3;lot.minimumFloors=1;lot.maximumFloors=3;
        if(role=="inn" || role=="pub") {
          lot.residence=role=="inn"?CityResidenceKind::Inn:CityResidenceKind::Pub;
          lot.desiredRooms=role=="inn"?14:5;lot.minimumFloors=2;
        } else if(role=="bakehouse" || role=="smithy" || role=="workshop") {
          lot.desiredRooms=role=="workshop"?5:3;lot.minimumFloors=2;lot.maximumFloors=3;
          lot.groundCover=CityGroundCover::Dirt;
          lot.trade=role=="workshop"?"carpentry":role;
        } else if(role=="farmstead") {
          lot.rowHouse=false;lot.merchantHouse=false;lot.residence=CityResidenceKind::House;
          lot.desiredRooms=4;lot.minimumFloors=1;lot.maximumFloors=2;
          lot.preferredFootprintMetres2=std::min(region.area*.9,110.);
          lot.groundCover=CityGroundCover::Dirt;
          lot.trade="farmstead";
        } else if(role=="watch-house") {
          lot.desiredRooms=5;lot.minimumFloors=3;lot.maximumFloors=4;
          lot.merchantHouse=false;lot.trade="guardhouse";
        } else if(role=="civic-hall") {
          lot.residence=CityResidenceKind::Mansion;lot.desiredRooms=10;lot.minimumFloors=3;lot.maximumFloors=3;
          lot.trade="hall";
        } else if(role=="cottage") {
          lot.maximumFloors=1;lot.desiredRooms=2;
          lot.preferredFootprintMetres2=std::min(region.area*.65,48.);
        } else if(lot.rowHouse || lot.merchantHouse) {
          lot.minimumFloors=region.area>=75 ? 3u : 2u;
          lot.maximumFloors=region.area>=150 ? 4u : 3u;
        }
      }
      out.lots.push_back(lot);
    }

  // Collapse degree-two graph chains into authored routes and smooth those
  // complete routes with locked endpoints. Smoothing isolated edge fragments
  // cannot round a junction and was why the earlier render stayed angular.
  std::map<uint64_t, const CityRoadEdge *> roadEdges;
  std::map<uint64_t, const CityRoadNode *> roadNodes;
  for (const auto &edge : out.skeleton.roadEdges)
    roadEdges[edge.id] = &edge;
  for (const auto &node : out.skeleton.roadNodes) roadNodes[node.id] = &node;
  std::set<uint64_t> visitedRoads;
  for (const auto &[seedId, seed] : roadEdges)
  {
    if (visitedRoads.count(seedId)) continue;
    auto usableDegree = [&](uint64_t node) {
      size_t n = 0;
      for (auto edge : roadNodes.at(node)->edges) n += roadEdges.count(edge);
      return n;
    };
    uint64_t current = usableDegree(seed->from) != 2 ? seed->from : usableDegree(seed->to) != 2 ? seed->to : seed->from;
    uint64_t edgeId = seedId;
    // IDs need not start at the end of a chain. Walk backwards first so a
    // middle seed does not split one bend into two independently locked roads.
    std::set<uint64_t> backwards{seedId};
    while (usableDegree(current)==2)
    {
      uint64_t other=0;
      for (auto candidate:roadNodes.at(current)->edges)
        if (candidate!=edgeId && roadEdges.count(candidate) && !visitedRoads.count(candidate)) other=candidate;
      if (!other) break;
      if (!backwards.insert(other).second) { current=seed->from; edgeId=seedId; break; }
      const auto &edge=*roadEdges.at(other);
      current=edge.from==current ? edge.to : edge.from;
      edgeId=other;
    }
    CitySmoothedRoute route;
    route.id = stableId(600, seedId);
    route.kind = seed->kind;
    while (edgeId && !visitedRoads.count(edgeId))
    {
      const auto &edge = *roadEdges.at(edgeId);
      auto path = edge.path;
      const bool forward = edge.from == current;
      if (!forward) std::reverse(path.begin(), path.end());
      if (!route.path.empty() && !path.empty() && distance(route.path.back(), path.front()) < 1e-4) path.erase(path.begin());
      route.path.insert(route.path.end(), path.begin(), path.end());
      route.edges.push_back(edge.id);
      route.halfWidth = std::max(route.halfWidth, edge.halfWidth);
      route.kind = CityRoadKind(std::min(unsigned(route.kind), unsigned(edge.kind)));
      visitedRoads.insert(edge.id);
      current = forward ? edge.to : edge.from;
      if (usableDegree(current) != 2) break;
      uint64_t next = 0;
      for (auto candidate : roadNodes.at(current)->edges)
        if (roadEdges.count(candidate) && !visitedRoads.count(candidate)) { next = candidate; break; }
      edgeId = next;
    }
    // Width belongs to the complete route, not its arbitrary source fragments.
    // Gate approaches are ceremonial/traffic spines; long chains are district
    // streets; short chains on block edges become varied lanes and alleys.
    if (route.kind == CityRoadKind::FieldTrack)
      route.halfWidth = std::max(.5, route.halfWidth);
    else if (route.kind == CityRoadKind::Approach)
      route.halfWidth = std::max(route.halfWidth, settings.approachHalfWidth);
    else if (route.halfWidth >= settings.arterialHalfWidth)
      route.halfWidth = std::max(route.halfWidth, settings.arterialHalfWidth);
    else
    {
      // The block and its sidewalks already reserve this carriageway.
      // Narrowing it here leaves a second verge inside the planned street.
      route.halfWidth = std::max(route.halfWidth, std::min(settings.localLaneMinimumHalfWidth, settings.localLaneMaximumHalfWidth));
    }
    // Only test nearby ownership polygons. Keep the entire ribbon clear of
    // lots; reducing the curve radius is preferable to cutting a building.
    double minX=1e100,minZ=1e100,maxX=-1e100,maxZ=-1e100;
    for (auto p:route.path) { minX=std::min(minX,p.x); minZ=std::min(minZ,p.z); maxX=std::max(maxX,p.x); maxZ=std::max(maxZ,p.z); }
    const double pad=route.halfWidth*5;
    std::vector<const std::vector<Position> *> obstacles;
    for (const auto &region:out.skeleton.regions)
    {
      if (region.kind!=CityRegionKind::Lot || region.boundary.empty()) continue;
      double x0=1e100,z0=1e100,x1=-1e100,z1=-1e100;
      for (auto p:region.boundary) { x0=std::min(x0,p.x); z0=std::min(z0,p.z); x1=std::max(x1,p.x); z1=std::max(z1,p.z); }
      if (x1>=minX-pad && x0<=maxX+pad && z1>=minZ-pad && z0<=maxZ+pad) obstacles.push_back(&region.boundary);
    }
    const auto clearOfEverything = [&](Position p) {
      for (const auto *poly:obstacles)
        if (inside(p,*poly) || boundaryDistance(p,*poly)<route.halfWidth+.05) return false;
      for (const auto &wall:out.wallSegments)
        if (wall.built && distance(p,closestPoint(p,wall.a,wall.b))<route.halfWidth+wall.width*.5) return false;
      return true;
    };
    route.path = straighten(route.path, settings.streetStraightenMetres, clearOfEverything);
    route.path = smoothPath(route.path,route.halfWidth,clearOfEverything,settings.streetCornerSteps);
    // A street inside the walls is paved, whatever its width; the dirt roads
    // run outside. Judged at the route's middle, so a road through a gate is
    // whichever side most of it lies on.
    if (route.path.size() >= 2 && route.groundCover==CityGroundCover::Automatic && standing!=growth.walls.end() &&
        inside(route.path[route.path.size()/2],standing->boundary))
      route.groundCover=CityGroundCover::Stone;
    // Urban streets do not become dirt merely because the settlement has
    // no defensive wall. Keep explicit earth tracks and rural approaches.
    if(route.groundCover==CityGroundCover::Automatic && obstacles.size()>=3 &&
       route.kind!=CityRoadKind::FieldTrack && route.kind!=CityRoadKind::Approach)
      route.groundCover=CityGroundCover::Stone;
    if (route.path.size() >= 2) out.routes.push_back(std::move(route));
  }

  // A harbour is a district programme, not a reclassification of world water.
  // Publish its longest boundary edge as the simple quay skeleton and attach it
  // to the nearest service-road node. More detailed docks can subdivide it.
  std::fprintf(stderr,"city completion: %u terraced lots, %u with a party edge left detached\n",terraced,detachedDespiteParty);
  for (const auto &region : out.skeleton.regions)
    if (region.districtUse == DistrictUse::Harbor && region.kind == CityRegionKind::District && region.boundary.size() >= 3)
    {
      size_t longest = 0;
      for (size_t i = 1; i < region.boundary.size(); ++i)
        if (distance(region.boundary[i], region.boundary[(i + 1) % region.boundary.size()]) > distance(region.boundary[longest], region.boundary[(longest + 1) % region.boundary.size()])) longest = i;
      CityHarbourTopology harbour;
      harbour.id = stableId(570, region.id);
      harbour.region = region.id;
      harbour.quay = {region.boundary[longest], region.boundary[(longest + 1) % region.boundary.size()]};
      harbour.accessNode = nearestRoadNode(out.skeleton, centre(harbour.quay));
      double best = 1e100;
      for (const auto &castle : castles.castles)
        if (castle.mode == CityCastleMode::HarbourFort && distance2(centre(castle.precinct), centre(harbour.quay)) < best)
        {
          best = distance2(centre(castle.precinct), centre(harbour.quay));
          harbour.controllingCastle = castle.id;
        }
      out.harbours.push_back(std::move(harbour));
    }

  auto byId = [](const auto &a, const auto &b) { return a.id < b.id; };
  std::sort(out.skeleton.wallNodes.begin(), out.skeleton.wallNodes.end(), byId);
  std::sort(out.skeleton.wallEdges.begin(), out.skeleton.wallEdges.end(), byId);
  std::sort(out.skeleton.gates.begin(), out.skeleton.gates.end(), byId);
  std::sort(out.skeleton.roadNodes.begin(), out.skeleton.roadNodes.end(), byId);
  std::sort(out.skeleton.roadEdges.begin(), out.skeleton.roadEdges.end(), byId);
  std::sort(out.wallSegments.begin(), out.wallSegments.end(), byId);
  std::sort(out.gatehouses.begin(), out.gatehouses.end(), byId);
  std::sort(out.towers.begin(), out.towers.end(), byId);
  std::sort(out.suburbs.begin(), out.suburbs.end(), byId);
  std::sort(out.harbours.begin(), out.harbours.end(), byId);
  std::sort(out.lots.begin(), out.lots.end(), [](const auto &a, const auto &b) { return a.lot < b.lot; });
  std::sort(out.routes.begin(), out.routes.end(), byId);
  if(!planCitySidewalks(out,1.4,error))return false;

  // ---- every lot reaches a way, or it is not built on -----------------------
  //
  // A lot's back and sides may abut whatever they like; its FRONT has to be
  // reachable. That was never tested: the subdivision cuts a block into lots
  // and the ones in the middle simply had no edge on anything, so a house
  // went up on ground nobody could walk to and its stair climbed out over a
  // neighbour's yard to find a street.
  //
  // So the network is grown until the invariant holds. A lot with no frontage
  // gets a connector cut from its nearest edge to the nearest ground that IS
  // reachable, and because each connector is itself reachable ground, the
  // lots behind it become reachable in turn - which is why this iterates
  // rather than passing once. What still cannot be reached after that is not
  // a building plot: it is the open ground in the middle of the block, and it
  // is left as common rather than built on.
  {
    const double reachMetres = 1.1;   // How close an edge must come to a way.
    const double connector = .75;     // Half width of a connector alley.
    uint32_t connectors = 0, common = 0, already = 0;

    struct Way { Position a, b; double halfWidth; };
    std::vector<Way> ways;

    for (const auto &route : out.routes)
      for (size_t k = 0; k + 1 < route.path.size(); ++k)
        ways.push_back({route.path[k], route.path[k + 1], std::max(.6, route.halfWidth)});

    for (const auto &space : out.skeleton.publicSpaces)
      if(space.kind==CityPublicSpace::Kind::Sidewalk)
        for(size_t k=0;k<space.boundary.size();++k)
          ways.push_back({space.boundary[k],space.boundary[(k+1)%space.boundary.size()],.1});

    for (const auto &alley : out.skeleton.alleys)
      for (size_t k = 0; k < alley.size(); ++k)
        ways.push_back({alley[k], alley[(k + 1) % alley.size()], .1});

    // The closest a lot's boundary comes to any way, and where.
    const auto frontage = [&](const std::vector<Position> &ring, Position &from, Position &to) {
      double best = 1e30;
      for (size_t i = 0; i < ring.size(); ++i)
      {
        const Position &u = ring[i];
        const Position &w = ring[(i + 1) % ring.size()];
        for (const double t : {0., .25, .5, .75, 1.})
        {
          const Position p{u.x + (w.x - u.x) * t, 0, u.z + (w.z - u.z) * t};
          for (const auto &way : ways)
          {
            const double dx = way.b.x - way.a.x, dz = way.b.z - way.a.z;
            const double len2 = dx * dx + dz * dz;
            const double s = len2 > 1e-12 ? std::clamp(((p.x - way.a.x) * dx + (p.z - way.a.z) * dz) / len2, 0., 1.) : 0.;
            const Position q{way.a.x + dx * s, 0, way.a.z + dz * s};
            const double d = std::hypot(p.x - q.x, p.z - q.z) - way.halfWidth;
            if (d < best) { best = d; from = p; to = q; }
          }
        }
      }
      return best;
    };

    std::map<uint64_t, const CitySkeletonRegion *> plots;
    for (const auto &region : out.skeleton.regions)
      if (region.kind == CityRegionKind::Lot && region.boundary.size() >= 3)
        plots[region.id] = &region;

    // Lot corners, welded: two lots that share an edge share its endpoints,
    // so welding by position turns every boundary into one graph.
    std::vector<Position> nodes;
    std::map<std::pair<int64_t, int64_t>, size_t> nodeIndex;
    const auto nodeOf = [&](const Position &p) {
      const std::pair<int64_t, int64_t> k{std::llround(p.x * 50.), std::llround(p.z * 50.)};
      auto found = nodeIndex.find(k);
      if (found != nodeIndex.end()) return found->second;
      nodes.push_back(p);
      nodeIndex.emplace(k, nodes.size() - 1);
      return nodes.size() - 1;
    };

    const auto fronts = [&](const Position &p) {
      for (const auto &way : ways)
      {
        const double dx = way.b.x - way.a.x, dz = way.b.z - way.a.z;
        const double len2 = dx * dx + dz * dz;
        const double s = len2 > 1e-12 ? std::clamp(((p.x - way.a.x) * dx + (p.z - way.a.z) * dz) / len2, 0., 1.) : 0.;
        if (std::hypot(p.x - way.a.x - dx * s, p.z - way.a.z - dz * s) - way.halfWidth <= reachMetres) return true;
      }
      return false;
    };

    std::set<uint64_t> reached;

    for (uint32_t pass = 0; pass < 6; ++pass)
    {
      uint32_t cut = 0;

      for (const auto &[id, region] : plots)
      {
        if (reached.count(id)) continue;
        Position from{}, to{};
        const double gap = frontage(region->boundary, from, to);

        if (gap <= reachMetres) { reached.insert(id); if (pass == 0) ++already; continue; }
        if (gap > 26.) continue; // Nothing near enough to connect to.

        // The connector runs ALONG lot edges, never across a lot.
        //
        // A ribbon cut straight from the lot to the nearest way slices
        // whatever lies between - and what lies between is other lots. That
        // is where the L-shaped plots came from: a quad with a strip taken
        // out of its middle is not a quad any more, and a house cannot be set
        // onto a shape like that, so a third of the town stayed square in a
        // skewed plot. A strip taken off a lot's EDGE leaves a quad.
        //
        // So the way out is found over the network the lot boundaries already
        // form. Shared edges are shared vertices, so walking that graph from
        // this lot to the nearest vertex that already fronts a way gives a
        // route between plots rather than through them.
        std::vector<Position> route;
        {
          std::map<size_t, std::vector<size_t>> links;
          std::map<size_t, uint64_t> from_;
          std::deque<size_t> queue;
          std::map<size_t, size_t> came;
          std::set<size_t> seen;

          const auto key = [&](const Position &p) { return nodeOf(p); };

          for (const auto &[other, plot] : plots)
            for (size_t i = 0; i < plot->boundary.size(); ++i)
            {
              const size_t a = key(plot->boundary[i]);
              const size_t b = key(plot->boundary[(i + 1) % plot->boundary.size()]);
              if (a != b) { links[a].push_back(b); links[b].push_back(a); }
            }

          for (const auto &p : region->boundary) { const size_t n = key(p); if (seen.insert(n).second) queue.push_back(n); }

          size_t arrived = SIZE_MAX;

          while (!queue.empty() && arrived == SIZE_MAX)
          {
            const size_t n = queue.front();
            queue.pop_front();
            Position dummy{}, unused{};
            if (fronts(nodes[n])) { arrived = n; break; }
            for (const size_t next : links[n])
              if (seen.insert(next).second) { came[next] = n; queue.push_back(next); }
          }

          if (arrived != SIZE_MAX)
          {
            for (size_t n = arrived;; )
            {
              route.push_back(nodes[n]);
              auto back = came.find(n);
              if (back == came.end()) break;
              n = back->second;
            }
          }
        }

        if (route.size() < 2)
        {
          // No route over the boundaries: fall back to the straight cut, which
          // is worth an awkward plot to keep the lot reachable at all.
          const double dx = to.x - from.x, dz = to.z - from.z, len = std::hypot(dx, dz);
          if (len < 1e-6) { reached.insert(id); continue; }
          route = {from, to};
        }

        for (size_t k = 0; k + 1 < route.size(); ++k)
        {
          const double dx = route[k + 1].x - route[k].x, dz = route[k + 1].z - route[k].z;
          const double len = std::hypot(dx, dz);
          if (len < 1e-6) continue;
          const double nx = -dz / len * connector, nz = dx / len * connector;
          out.skeleton.alleys.push_back({
              {route[k].x - nx, 0, route[k].z - nz}, {route[k + 1].x - nx, 0, route[k + 1].z - nz},
              {route[k + 1].x + nx, 0, route[k + 1].z + nz}, {route[k].x + nx, 0, route[k].z + nz}});

          const auto &ring = out.skeleton.alleys.back();
          for (size_t e = 0; e < 4; ++e) ways.push_back({ring[e], ring[(e + 1) % 4], .1});
        }

        reached.insert(id);
        ++connectors;
        ++cut;
      }

      if (cut == 0) break;
    }

    for (auto &lot : out.lots)
      if (lot.developed && plots.count(lot.lot) && !reached.count(lot.lot)) { lot.developed = false; ++common; }

    std::fprintf(stderr, "city completion: %u lots already fronted a way, %u connectors cut, %u left as common ground\n",
                 already, connectors, common);
  }

  out.skeleton.revision = out.revision;
  result = std::move(out);
  if (error) error->clear();
  return true;
}

const char *cityWallFaceKindName(CityWallFaceKind kind)
{
  static constexpr const char *names[] = {"outside", "inside", "up", "bottom", "left", "right"};
  return names[size_t(kind)];
}
} // namespace procedural::world::rewrite
