#include "WorldRiverChannels.hpp"
#include "WorldPhysicalTerrain.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace procedural::world::rewrite {
namespace {
bool finite(double x)
{
  return std::isfinite(x) && std::abs(x) < 1e12;
}

double length(Position a, Position b)
{
  return std::hypot(b.x - a.x, b.z - a.z);
}

double smooth(double t)
{
  t = std::clamp(t, 0., 1.);
  return t * t * (3 - 2 * t);
}

bool valid(const ChannelSettings &s)
{
  for (double x :
       {s.minimumBottomWidthMetres, s.maximumBottomWidthMetres, s.widthPerSqrtDischarge, s.bankSideSlope, s.manningN, s.minimumEnergySlope, s.confluenceBlendMetres, s.bankTransitionMetres, s.maximumDepthMetres})
  {
    if (!finite(x))
    {
      return false;
    }
  }
  return s.minimumBottomWidthMetres > 0 && s.maximumBottomWidthMetres >= s.minimumBottomWidthMetres && s.widthPerSqrtDischarge >= 0 && s.bankSideSlope > 0 && s.manningN > 0 && s.minimumEnergySlope > 0 &&
         s.confluenceBlendMetres > 0 && s.bankTransitionMetres > 0 && s.maximumDepthMetres > 0;
}

ChannelSection dimensions(double q, double b, double d, double slope, const ChannelSettings &s)
{
  ChannelSection r;
  r.discharge = q;
  r.bottomWidth = b;
  r.depth = d;
  r.waterWidth = b + 2 * s.bankSideSlope * d;
  r.area = d * (b + s.bankSideSlope * d);
  double perimeter = b + 2 * d * std::sqrt(1 + s.bankSideSlope * s.bankSideSlope);
  r.capacity = r.area == 0 ? 0 : r.area * std::pow(r.area / perimeter, 2. / 3) * std::sqrt(slope) / s.manningN;
  r.velocity = r.area > 0 ? q / r.area : 0;
  return r;
}
} // namespace

bool solveChannelSection(double q, double slope, const ChannelSettings &s, ChannelSection &out, std::string *error)
{
  auto fail = [&](const char *t) {
    if (error)
    {
      *error = t;
    }
    return false;
  };
  if (!valid(s) || !finite(q) || q < 0 || !finite(slope) || slope <= 0)
  {
    return fail("Invalid Manning section settings");
  }
  double b = std::clamp(s.minimumBottomWidthMetres + s.widthPerSqrtDischarge * std::sqrt(q), s.minimumBottomWidthMetres, s.maximumBottomWidthMetres);
  double lo = 0, hi = s.maximumDepthMetres;
  if (dimensions(q, b, hi, slope, s).capacity < q)
  {
    return fail("Discharge exceeds supported channel depth");
  }
  for (int i = 0; i < 80; ++i)
  {
    double mid = (lo + hi) / 2;
    if (dimensions(q, b, mid, slope, s).capacity < q)
    {
      lo = mid;
    }
    else
    {
      hi = mid;
    }
  }
  out = dimensions(q, b, q == 0 ? 0 : (lo + hi) / 2, slope, s);
  return true;
}

bool WorldRiverChannels::build(std::vector<ChannelNode> nodes, std::vector<FlowLink> links, std::vector<CatchmentRunoff> runoff, const ChannelSettings &s, std::string *error, const std::vector<RiverPath> &paths)
{
  auto fail = [&](const char *t) {
    if (error)
    {
      *error = t;
    }
    return false;
  };
  if (!valid(s) || nodes.empty() || nodes.size() > 4096 || links.size() > 8192 || runoff.size() > 16384)
  {
    return fail("Invalid channel network settings or budget");
  }
  WorldRiverChannels result;
  result.settings_ = s;
  std::sort(nodes.begin(), nodes.end(), [](auto a, auto b) {
    return a.id < b.id;
  });
  std::sort(links.begin(), links.end(), [](auto a, auto b) {
    return a.from != b.from ? a.from < b.from : a.to < b.to;
  });
  std::sort(runoff.begin(), runoff.end(), [](auto a, auto b) {
    return a.id < b.id;
  });
  std::map<uint64_t, size_t> index;
  for (size_t i = 0; i < nodes.size(); ++i)
  {
    auto n = nodes[i];
    if (!n.id || index.count(n.id) || !finite(n.water.x) || !finite(n.water.y) || !finite(n.water.z) || !finite(n.lossCumecs) || n.lossCumecs < 0)
    {
      return fail("Invalid channel node");
    }
    index[n.id] = i;
  }
  std::vector<double> local(nodes.size()), area(nodes.size());
  uint64_t last = 0;
  for (auto r : runoff)
  {
    if (!r.id || r.id == last || !index.count(r.nodeId) || !finite(r.areaMetres2) || r.areaMetres2 < 0 || !finite(r.rainfallMmPerHour) || r.rainfallMmPerHour < 0 || !finite(r.runoffCoefficient) ||
        r.runoffCoefficient < 0 || r.runoffCoefficient > 1)
    {
      return fail("Duplicate catchment or invalid incremental runoff");
    }
    last = r.id;
    size_t i = index[r.nodeId];
    area[i] += r.areaMetres2;
    local[i] += r.areaMetres2 * r.rainfallMmPerHour * r.runoffCoefficient / 3600000.;
  }
  std::vector<FlowNode> flow;
  for (size_t i = 0; i < nodes.size(); ++i)
  {
    flow.push_back({nodes[i].id, local[i], nodes[i].lossCumecs});
  }
  std::vector<FlowResult> totals;
  if (!accumulateDischarge(flow, links, totals, error))
  {
    return false;
  }
  std::map<uint64_t, FlowResult> byId;
  for (auto f : totals)
  {
    byId[f.id] = f;
  }
  for (size_t i = 0; i < nodes.size(); ++i)
  {
    auto f = byId[nodes[i].id];
    result.ledger_.push_back({f.id, area[i], local[i], f.incomingCumecs, nodes[i].lossCumecs, f.outgoingCumecs});
  }
  if (!paths.empty() && paths.size() != nodes.size())
  {
    return fail("Paths must use sorted node indices");
  }
  result.nodes_ = nodes;
  result.junctions_.resize(nodes.size());
  result.gradients_.resize(nodes.size());
  result.degree_.resize(nodes.size());
  std::vector<double> nodeSlope(nodes.size(), s.minimumEnergySlope);
  std::set<std::pair<uint64_t, uint64_t>> used;
  for (auto link : links)
  {
    if (!index.count(link.from) || !index.count(link.to) || !used.emplace(link.from, link.to).second)
    {
      return fail("Invalid duplicate channel reach");
    }
    auto a = nodes[index[link.from]].water, b = nodes[index[link.to]].water;
    RiverPath path;
    auto points = paths.empty() || paths[index[link.from]].points.empty() ? std::vector<Position>{a, b} : paths[index[link.from]].points;
    if (!makeRiverPath(std::move(points), a, b, path))
    {
      return fail("Invalid channel centerline");
    }
    double l = path.length(), q = byId[link.from].outgoingCumecs * link.fraction;
    if (l < .01 || l > 1e6 || a.y < b.y)
    {
      return fail("Uphill or collapsed channel reach");
    }
    double slope = std::max(s.minimumEnergySlope, (a.y - b.y) / l);
    ChannelSection section;
    if (!solveChannelSection(q, slope, s, section, error))
    {
      return false;
    }
    ++result.degree_[index[link.from]];
    ++result.degree_[index[link.to]];
    result.reaches_.push_back({link.from, link.to, a, b, l, q, slope, section, {}, {}, path});
    for (auto id : {link.from, link.to})
    {
      auto &j = result.junctions_[index[id]];
      j.bottomWidth = std::max(j.bottomWidth, section.bottomWidth);
      j.depth = std::max(j.depth, section.depth);
      nodeSlope[index[id]] = std::max(nodeSlope[index[id]], slope);
    }
    auto &g = result.gradients_[index[link.from]];
    auto tangent = path.points[1];
    double tangentLength = length(a, tangent);
    g.x += (tangent.x - a.x) / tangentLength * (b.y - a.y) / l * link.fraction;
    g.z += (tangent.z - a.z) / tangentLength * (b.y - a.y) / l * link.fraction;
  }
  for (size_t i = 0; i < nodes.size(); ++i)
  {
    auto j = result.junctions_[i];
    ChannelSection localSection;
    if (!solveChannelSection(std::max(byId[nodes[i].id].incomingCumecs, byId[nodes[i].id].outgoingCumecs), nodeSlope[i], s, localSection, error))
    {
      return false;
    }
    j.bottomWidth = std::max(j.bottomWidth, localSection.bottomWidth);
    j.depth = std::max(j.depth, localSection.depth);
    result.junctions_[i] = dimensions(byId[nodes[i].id].outgoingCumecs, j.bottomWidth, j.depth, nodeSlope[i], s);
  }
  for (auto &r : result.reaches_)
  {
    r.fromSection = result.junctions_[index[r.from]];
    r.toSection = result.junctions_[index[r.to]];
    double previousWater = r.a.y, previousBed = 1e30;
    for (int k = 0; k <= 200; ++k)
    {
      double t = k / 200.;
      ChannelSection section;
      double water;
      size_t idx = &r - result.reaches_.data();
      result.section(idx, t * r.length, section, water);
      auto position = r.path.at(t * r.length);
      double x = position.x, z = position.z;
      water = result.resolvedWater(r, x, z, t);
      if (!finite(water) || water > previousWater + 1e-8 || water - section.depth > previousBed + 1e-8)
      {
        return fail("Confluence profile would introduce an uphill water/bed step");
      }
      previousWater = water;
      previousBed = water - section.depth;
    }
  }
  *this = std::move(result);
  return true;
}

bool WorldRiverChannels::buildFromWatersheds(const RiverGrowthProduct &g, const WatershedProduct &w, double rain, double coefficient, const ChannelSettings &s, bool allowTruncated, std::string *error)
{
  auto fail = [&](const char *t) {
    if (error)
    {
      *error = t;
    }
    return false;
  };
  if ((g.budgetReached || w.sourceGraphTruncated) && !allowTruncated)
  {
    return fail("Truncated drainage requires explicit opt-in");
  }
  if (w.cells.size() != g.nodes.size() || !finite(w.landAreaMetres2) || w.landAreaMetres2 <= 0)
  {
    return fail("Watershed ownership does not cover graph");
  }
  std::vector<ChannelNode> nodes;
  std::vector<FlowLink> links;
  std::vector<CatchmentRunoff> runoff;
  std::set<uint32_t> seen;
  double area = 0;
  for (size_t i = 0; i < g.nodes.size(); ++i)
  {
    auto n = g.nodes[i];
    nodes.push_back({i + 1, n.position, 0});
    if (n.receiver != UINT32_MAX)
    {
      if (n.receiver >= i)
      {
        return fail("Invalid fixed receiver graph");
      }
      links.push_back({i + 1, n.receiver + 1, 1});
    }
  }
  for (auto c : w.cells)
  {
    if (c.riverNode >= nodes.size() || !seen.insert(c.riverNode).second || !finite(c.areaMetres2) || c.areaMetres2 < 0)
    {
      return fail("Duplicate watershed ownership");
    }
    area += c.areaMetres2;
    runoff.push_back({uint64_t(c.riverNode) + 1, uint64_t(c.riverNode) + 1, c.areaMetres2, rain, coefficient});
  }
  if (std::abs(area - w.landAreaMetres2) > std::max(1., area) * 1e-8)
  {
    return fail("Incremental watershed areas do not sum to land area");
  }
  std::vector<RiverPath> paths;
  if (!resolveRiverPaths(g, paths))
  {
    return fail("Invalid refined watershed paths");
  }
  return build(nodes, links, runoff, s, error, paths);
}

bool WorldRiverChannels::validateReservations(const WorldPhysicalTerrain &terrain, std::string *error) const
{
  auto fail = [&](const char *s) {
    if (error)
    {
      *error = s;
    }
    return false;
  };
  const auto &graph = terrain.rivers();
  if (nodes_.size() != graph.nodes.size())
  {
    return fail("Final channels changed fixed river graph");
  }
  std::vector<RiverPath> reservedPaths;
  if (!resolveRiverPaths(graph, reservedPaths))
  {
    return fail("Invalid reserved paths");
  }
  size_t edgeCount = 0;
  for (auto node : graph.nodes)
  {
    edgeCount += node.receiver != UINT32_MAX;
  }
  if (reaches_.size() != edgeCount)
  {
    return fail("Final channels changed fixed river links");
  }
  std::map<uint32_t, RiverReservation> reservations;
  for (auto r : terrain.reservations())
  {
    reservations[r.upstreamNode] = r;
  }
  for (size_t i = 0; i < reaches_.size(); ++i)
  {
    const auto &r = reaches_[i];
    if (r.from == 0 || r.from > graph.nodes.size() || graph.nodes[r.from - 1].receiver == UINT32_MAX || graph.nodes[r.from - 1].receiver + 1 != r.to)
    {
      return fail("Final receiver differs from reserved river");
    }
    auto a = graph.nodes[r.from - 1].position, b = graph.nodes[r.to - 1].position;
    if (a.x != r.a.x || a.y != r.a.y || a.z != r.a.z || b.x != r.b.x || b.y != r.b.y || b.z != r.b.z)
    {
      return fail("Final channels moved fixed water controls");
    }
    const auto &reservedPath = reservedPaths[r.from - 1];
    if (reservedPath.points.size() != r.path.points.size())
    {
      return fail("Refined paths require rebuilding physical reservations");
    }
    for (size_t k = 0; k < reservedPath.points.size(); ++k)
    {
      auto p = reservedPath.points[k], q = r.path.points[k];
      if (p.x != q.x || p.y != q.y || p.z != q.z)
      {
        return fail("Changed centerline requires rebuilding physical reservations");
      }
    }
    auto found = reservations.find(uint32_t(r.from - 1));
    int count = std::max(1, int(std::ceil(r.length)));
    for (int k = 0; k <= count; ++k)
    {
      double t = double(k) / count;
      ChannelSection p;
      double water;
      section(i, r.length * t, p, water);
      auto position = r.path.at(t * r.length);
      double x = position.x, z = position.z;
      SplineSurfaceSample original;
      if (!terrain.beforeModifications(x, z, original))
      {
        return fail("Unresolved final channel terrain");
      }
      if (found == reservations.end())
      {
        if (original.influence > 0 && original.height > water)
        {
          return fail("Final channel lacks landform crossing reservation");
        }
      }
      else
      {
        auto reserve = found->second;
        if (p.waterWidth * .5 > reserve.halfWidthMetres || p.waterWidth * .5 + settings_.bankTransitionMetres > reserve.halfWidthMetres + reserve.bankTransitionMetres)
        {
          return fail("Final channel exceeds reserved corridor width");
        }
        if (original.height - (water - p.depth) > reserve.maximumCutMetres)
        {
          return fail("Final channel exceeds reserved incision limit");
        }
      }
    }
  }
  return true;
}

bool WorldRiverChannels::section(size_t i, double distance, ChannelSection &out, double &water) const
{
  if (i >= reaches_.size() || !finite(distance))
  {
    return false;
  }
  const auto &r = reaches_[i];
  if (distance < 0 || distance > r.length)
  {
    return false;
  }
  double blend = std::min(settings_.confluenceBlendMetres, r.length / 3), b = r.normal.bottomWidth, d = r.normal.depth;
  double waterGrade = (r.a.y - r.b.y) / r.length;
  double startBlend = std::min(r.length - blend, std::max(blend, 1.5 * std::max(0., r.fromSection.depth - d) / std::max(1e-9, waterGrade)));
  if (distance < startBlend)
  {
    double t = distance / startBlend;
    b = r.fromSection.bottomWidth + t * (b - r.fromSection.bottomWidth);
    d = r.fromSection.depth + t * (d - r.fromSection.depth);
  }
  else if (distance > r.length - blend)
  {
    double t = (distance - r.length + blend) / blend;
    b += t * (r.toSection.bottomWidth - b);
    d += t * (r.toSection.depth - d);
  }
  out = dimensions(r.discharge, b, d, r.energySlope, settings_);
  auto position = r.path.at(distance);
  water = resolvedWater(r, position.x, position.z, distance / r.length);
  return true;
}

double WorldRiverChannels::resolvedWater(const ChannelReach &r, double x, double z, double t) const
{
  double water = r.a.y + (r.b.y - r.a.y) * t;
  for (auto id : {r.from, r.to})
  {
    auto it = std::lower_bound(nodes_.begin(), nodes_.end(), id, [](auto n, uint64_t id) {
      return n.id < id;
    });
    size_t i = it - nodes_.begin();
    if (degree_[i] < 3)
    {
      continue;
    }
    auto p = it->water;
    double d = std::hypot(x - p.x, z - p.z), radius = settings_.confluenceBlendMetres;
    double weight = 1 - smooth((d - radius) / radius);
    double common = p.y + gradients_[i].x * (x - p.x) + gradients_[i].z * (z - p.z);
    water += (common - water) * weight;
  }
  return water;
}

bool WorldRiverChannels::sample(double x, double z, const BaseSurface &base, ChannelSample &out) const
{
  if (!finite(x) || !finite(z) || !base)
  {
    return false;
  }
  double terrain = base(x, z);
  if (!finite(terrain))
  {
    return false;
  }
  ChannelSample result;
  result.terrainHeight = terrain;
  double best = -1e30;
  for (size_t i = 0; i < reaches_.size(); ++i)
  {
    const auto &r = reaches_[i];
    double station = 0, d = r.path.project(x, z, station);
    ChannelSection profile;
    double water;
    section(i, station, profile, water);
    // Evaluate a shared junction plane at the query, not at different branch projections.
    water = resolvedWater(r, x, z, station / r.length);
    double depth = profile.depth - std::max(0., d - profile.bottomWidth * .5) / settings_.bankSideSlope;
    if (depth > best)
    {
      best = depth;
      result.waterHeight = water;
    }
  }
  if (best > -settings_.bankTransitionMetres / settings_.bankSideSlope)
  {
    double bankDistance = std::max(0., -best * settings_.bankSideSlope);
    double weight = 1 - smooth(bankDistance / settings_.bankTransitionMetres);
    result.terrainHeight = terrain + (result.waterHeight - best - terrain) * weight;
  }

  result.signedWaterDepth = best;
  result.wet = best > 0;
  if (result.wet && result.terrainHeight > result.waterHeight - best + 1e-8)
  {
    return false;
  }
  out = result;
  return true;
}
} // namespace procedural::world::rewrite
