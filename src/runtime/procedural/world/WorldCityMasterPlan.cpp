#include "WorldCityMasterPlan.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <tuple>
namespace procedural::world::rewrite
{
namespace
{
double cross(Position a, Position b, Position c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}
double distance(Position a, Position b)
{
  return std::hypot(a.x - b.x, a.z - b.z);
}
double area(const std::vector<Position> &p)
{
  double a = 0;
  for (size_t i = 0; i < p.size(); ++i)
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  return a * .5;
}
bool overlap(const std::vector<Position> &a, const std::vector<Position> &b)
{
  for (auto p : {&a, &b})
    for (size_t i = 0; i < p->size(); ++i)
    {
      auto u = (*p)[i], v = (*p)[(i + 1) % p->size()];
      const auto &other = p == &a ? b : a;
      bool separated = true;
      for (auto q : other)
        if (cross(u, v, q) > 1e-7)
        {
          separated = false;
          break;
        }
      if (separated)
        return false;
    }
  return true;
}
struct Link
{
  size_t to;
  double length, width;
  Position portal;
};
} // namespace
const char *districtUseName(DistrictUse u)
{
  static const char *names[] = {
    "castle", "harbor", "civic", "merchant", "military", "prison", "manor", "residential", "suburb", "craftsmen", "guild", "temple", "church", "citadel", "wealthy-residential", "worker-housing"};
  return unsigned(u) < unsigned(DistrictUse::Count) ? names[unsigned(u)] : "invalid";
}
bool generateCityMasterPlan(const std::vector<CityPlanCell> &input, const std::vector<DistrictRequest> &requests, const std::vector<uint64_t> &gates, const CityPlanSettings &s, CityMasterPlan &out, std::string *error)
{
  auto fail = [&](std::string e)
  {
    if (error)
      *error = std::move(e);
    return false;
  };
  if (!s.revision || !s.maximumCells || s.maximumCells > 512 || input.empty() || input.size() > s.maximumCells || requests.empty() || requests.size() > 32 || gates.empty() || gates.size() > 64 ||
      !std::isfinite(s.maximumSlope) || s.maximumSlope < 0 || s.maximumSlope > 1 || !std::isfinite(s.maximumFloodRisk) || s.maximumFloodRisk < 0 || s.maximumFloodRisk > 1 || !std::isfinite(s.minimumPortalWidth) ||
      s.minimumPortalWidth <= 0)
    return fail("invalid city planning bounds");
  auto cells = input;
  auto req = requests;
  std::sort(
      cells.begin(),
      cells.end(),
      [](auto &a, auto &b)
      {
        return a.id < b.id;
      });
  std::sort(
      req.begin(),
      req.end(),
      [](auto &a, auto &b)
      {
        return a.id < b.id;
      });
  std::map<uint64_t, size_t> index;
  std::vector<double> areas;
  std::vector<Position> centers;
  std::vector<bool> usable;
  for (size_t i = 0; i < cells.size(); ++i)
  {
    auto &c = cells[i];
    if (!c.id || !index.emplace(c.id, i).second || c.boundary.size() < 3 || c.boundary.size() > 32)
      return fail("invalid cell ID or polygon budget");
    for (auto x : {c.slope, c.floodRisk, c.defense, c.fertility})
      if (!std::isfinite(x) || x < 0 || x > 1)
        return fail("invalid suitability metric");
    Position center;
    for (auto p : c.boundary)
    {
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || std::abs(p.x) > 1e6 || std::abs(p.z) > 1e6)
        return fail("invalid cell coordinate");
      center.x += p.x / c.boundary.size();
      center.z += p.z / c.boundary.size();
    }
    for (size_t k = 0; k < c.boundary.size(); ++k)
    {
      auto a = c.boundary[k], b = c.boundary[(k + 1) % c.boundary.size()];
      for (size_t j = k + 1; j < c.boundary.size(); ++j)
        if (distance(a, c.boundary[j]) < 1e-5)
          return fail("duplicate cell vertex");
      if (distance(a, b) < 1e-5)
        return fail("degenerate cell edge");
      for (auto p : c.boundary)
        if (cross(a, b, p) < -1e-7)
          return fail("cell must be convex CCW");
    }
    double a = area(c.boundary);
    if (a < 1e-4)
      return fail("degenerate cell area");
    areas.push_back(a);
    centers.push_back(center);
    usable.push_back(c.buildable && c.slope <= s.maximumSlope && c.floodRisk <= s.maximumFloodRisk);
  }
  for (size_t i = 0; i < req.size(); ++i)
    if (!req[i].id || (i && req[i].id == req[i - 1].id) || unsigned(req[i].use) >= unsigned(DistrictUse::Count) || !std::isfinite(req[i].minimumArea) || !std::isfinite(req[i].targetArea) || req[i].minimumArea <= 0 ||
        req[i].targetArea < req[i].minimumArea)
      return fail("invalid district request");
  std::vector<std::vector<Link>> graph(cells.size());
  for (size_t i = 0; i < cells.size(); ++i)
    for (size_t j = i + 1; j < cells.size(); ++j)
    {
      if (overlap(cells[i].boundary, cells[j].boundary))
        return fail("overlapping planning cells");
      if (!usable[i] || !usable[j])
        continue;
      // Longest shared collinear interval; corner-only contact is not access.
      double width = 0;
      Position portal;
      auto &p = cells[i].boundary, &q = cells[j].boundary;
      for (size_t k = 0; k < p.size(); ++k)
        for (size_t l = 0; l < q.size(); ++l)
        {
          auto a = p[k], b = p[(k + 1) % p.size()], c = q[l], d = q[(l + 1) % q.size()];
          double len = distance(a, b);
          if (std::abs(cross(a, b, c)) / len > 1e-6 || std::abs(cross(a, b, d)) / len > 1e-6)
            continue;
          auto t = [&](Position x)
          {
            return ((x.x - a.x) * (b.x - a.x) + (x.z - a.z) * (b.z - a.z)) / (len * len);
          };
          double lo = std::max(0., std::min(t(c), t(d))), hi = std::min(1., std::max(t(c), t(d)));
          if ((hi - lo) * len > width)
          {
            width = (hi - lo) * len;
            portal = {a.x + (b.x - a.x) * (lo + hi) * .5, 0, a.z + (b.z - a.z) * (lo + hi) * .5};
          }
        }
      if (width + 1e-7 >= s.minimumPortalWidth)
      {
        double cost = (distance(centers[i], portal) + distance(portal, centers[j])) * (1 + 4 * std::max(cells[i].slope, cells[j].slope));
        graph[i].push_back({j, cost, width, portal});
        graph[j].push_back({i, cost, width, portal});
      }
    }
  const double inf = std::numeric_limits<double>::infinity();
  std::vector<double> cost(cells.size(), inf);
  std::vector<size_t> parent(cells.size(), cells.size());
  std::vector<Position> portals(cells.size());
  std::priority_queue<std::pair<double, size_t>, std::vector<std::pair<double, size_t>>, std::greater<std::pair<double, size_t>>> queue;
  for (auto id : gates)
  {
    if (!index.count(id) || !usable[index.at(id)])
      return fail("gate names missing or excluded land");
    auto i = index.at(id);
    cost[i] = 0;
    queue.emplace(0, i);
  }
  while (!queue.empty())
  {
    auto [d, i] = queue.top();
    queue.pop();
    if (d != cost[i])
      continue;
    for (auto edge : graph[i])
      if (d + edge.length < cost[edge.to] - 1e-9)
      {
        cost[edge.to] = d + edge.length;
        parent[edge.to] = i;
        portals[edge.to] = edge.portal;
        queue.emplace(cost[edge.to], edge.to);
      }
  }
  // Preserve nearest-gate cost for placement, but route the published intent
  // through one connected gate tree. Otherwise separate gate catchments could
  // appear individually accessible without any city-wide connecting route.
  const size_t root = index.at(*std::min_element(gates.begin(), gates.end()));
  std::vector<double> rootCost(cells.size(), inf);
  std::fill(parent.begin(), parent.end(), cells.size());
  rootCost[root] = 0;
  queue.emplace(0, root);
  while (!queue.empty())
  {
    auto [d, i] = queue.top();
    queue.pop();
    if (d != rootCost[i])
      continue;
    for (auto edge : graph[i])
      if (d + edge.length < rootCost[edge.to] - 1e-9)
      {
        rootCost[edge.to] = d + edge.length;
        parent[edge.to] = i;
        portals[edge.to] = edge.portal;
        queue.emplace(rootCost[edge.to], edge.to);
      }
  }
  for (auto gate : gates)
    if (!std::isfinite(rootCost[index.at(gate)]))
      return fail("city gates lie in disconnected traversable components");
  // Commercial/civic centers serve every arrival, instead of competing for
  // the nearest gate cell. Keep nearest-gate cost for peripheral uses.
  std::vector<double> centerCost(cells.size(), 0);
  for (auto gate : std::set<uint64_t>(gates.begin(), gates.end()))
  {
    std::vector<double> distances(cells.size(), inf);
    distances[index.at(gate)] = 0;
    queue.emplace(0, index.at(gate));
    while (!queue.empty())
    {
      auto [d, i] = queue.top();
      queue.pop();
      if (d != distances[i])
        continue;
      for (auto edge : graph[i])
        if (d + edge.length < distances[edge.to] - 1e-9)
        {
          distances[edge.to] = d + edge.length;
          queue.emplace(distances[edge.to], edge.to);
        }
    }
    for (size_t i = 0; i < cells.size(); ++i)
      centerCost[i] = std::max(centerCost[i], distances[i]);
  }
  auto eligible = [&](const DistrictRequest &r, size_t i)
  {
    auto &c = cells[i];
    if (!usable[i] || !c.allocatable || !std::isfinite(rootCost[i]))
      return false;
    if (r.use == DistrictUse::Harbor)
      return c.navigableShore;
    if (r.use == DistrictUse::Citadel)
      return c.secure && c.defense >= .6;
    if (r.use == DistrictUse::Castle)
      return c.defense >= .6;
    if (r.use == DistrictUse::Prison || r.use == DistrictUse::Military)
      return c.secure;
    if (r.use == DistrictUse::Suburb)
      return c.outsideOldWall;
    if (r.use == DistrictUse::Manor)
      return c.fertility >= .5;
    return true;
  };
  // Required requests first; scarcer eligibility first within each tier.
  auto count = [&](auto &r)
  {
    size_t n = 0;
    for (size_t i = 0; i < cells.size(); ++i)
      n += eligible(r, i);
    return n;
  };
  std::sort(
      req.begin(),
      req.end(),
      [&](auto &a, auto &b)
      {
        return std::make_tuple(!a.required, count(a), a.id) < std::make_tuple(!b.required, count(b), b.id);
      });
  CityMasterPlan result;
  result.revision = s.revision;
  uint64_t fingerprint = s.seed;
  auto hash = [&](uint64_t value)
  {
    fingerprint = featureSeed(fingerprint, 1, value, 71);
  };
  auto hashDouble = [&](double value)
  {
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    hash(bits);
  };
  hash(s.revision);
  hash(s.maximumCells);
  hashDouble(s.maximumSlope);
  hashDouble(s.maximumFloodRisk);
  hashDouble(s.minimumPortalWidth);
  for (const auto &c : cells)
  {
    hash(c.id);
    hash(c.boundary.size());
    for (auto p : c.boundary)
    {
      hashDouble(p.x);
      hashDouble(p.y);
      hashDouble(p.z);
    }
    hashDouble(c.slope);
    hashDouble(c.floodRisk);
    hashDouble(c.defense);
    hashDouble(c.fertility);
    hash(c.buildable);
    hash(c.navigableShore);
    hash(c.secure);
    hash(c.outsideOldWall);
    hash(c.allocatable);
  }
  for (const auto &r : req)
  {
    hash(r.id);
    hash(unsigned(r.use));
    hashDouble(r.minimumArea);
    hashDouble(r.targetArea);
    hash(r.required);
  }
  for (auto gate : std::set<uint64_t>(gates.begin(), gates.end()))
  {
    hash(gate);
    std::vector<size_t> path;
    for (size_t i = index.at(gate); i != cells.size(); i = parent[i])
      path.push_back(i);
    std::reverse(path.begin(), path.end());
    std::vector<Position> route{centers[root]};
    for (size_t k = 1; k < path.size(); ++k)
    {
      route.push_back(portals[path[k]]);
      route.push_back(centers[path[k]]);
    }
    result.gateConnections.push_back(std::move(route));
  }
  result.inputFingerprint = fingerprint;

  std::vector<uint64_t> owner(cells.size(), 0);
  for (const auto &r : req)
  {
    // Try every eligible anchor in score order. A failed trial never consumes cells.
    std::vector<std::pair<double, size_t>> candidates;
    for (size_t i = 0; i < cells.size(); ++i)
      if (!owner[i] && eligible(r, i))
      {
        double preference = (r.use == DistrictUse::Castle || r.use == DistrictUse::Citadel) ? cells[i].defense * 200 : r.use == DistrictUse::Manor ? cells[i].fertility * 100 : 0;
        double jitter = double(featureSeed(s.seed, 1, r.id, 1, cells[i].id) % 1000000) / 1000000.;
        const double accessCost = r.use == DistrictUse::Merchant || r.use == DistrictUse::Civic || r.use == DistrictUse::Guild || r.use == DistrictUse::Temple ? centerCost[i] : cost[i];
        candidates.emplace_back(accessCost - preference + jitter, i);
      }
    std::sort(candidates.begin(), candidates.end());
    bool placed = false;
    for (auto [score, anchor] : candidates)
    {
      (void)score;
      std::set<size_t> selected{anchor};
      double total = areas[anchor];
      if (total > r.targetArea + 1e-7)
        continue;
      // Allocate the minimum before optional expansion, preventing target demand
      // from consuming land needed by later mandatory districts.
      while (total + 1e-7 < r.minimumArea)
      {
        size_t best = cells.size();
        double bestCost = inf;
        for (auto i : selected)
          for (auto e : graph[i])
            if (!owner[e.to] && !selected.count(e.to) && eligible(r, e.to) && total + areas[e.to] <= r.targetArea + 1e-7)
            {
              double c = distance(centers[anchor], centers[e.to]) + areas[e.to] / e.width;
              if (c < bestCost)
              {
                bestCost = c;
                best = e.to;
              }
            }
        if (best == cells.size())
          break;
        selected.insert(best);
        total += areas[best];
      }
      if (total + 1e-7 < r.minimumArea)
        continue;
      CityPlanDistrict district;
      district.id = r.id;
      district.use = r.use;
      district.anchorCell = cells[anchor].id;
      district.area = total;
      for (auto i : selected)
      {
        owner[i] = r.id;
        district.cells.push_back(cells[i].id);
        district.pieces.push_back(cells[i].boundary);
      }
      std::vector<size_t> path;
      for (size_t i = anchor; i != cells.size(); i = parent[i])
        path.push_back(i);
      std::reverse(path.begin(), path.end());
      district.access.push_back(centers[path.front()]);
      for (size_t k = 0; k < path.size(); ++k)
      {
        district.accessCells.push_back(cells[path[k]].id);
        if (k)
        {
          district.access.push_back(portals[path[k]]);
          district.access.push_back(centers[path[k]]);
        }
      }
      result.districts.push_back(std::move(district));
      placed = true;
      break;
    }
    if (!placed)
    {
      if (r.required)
        return fail("required district " + std::to_string(r.id) + " cannot meet eligibility/access/minimum area under bounded allocation");
      result.omissions.push_back({r.id, "no connected eligible allocation meeting minimum area"});
    }
  }
  // Simultaneous frontier growth: stable globally cheapest admissible addition.
  for (size_t iteration = 0; iteration < cells.size(); ++iteration)
  {
    double best = inf;
    size_t chosen = cells.size(), di = result.districts.size();
    for (size_t d = 0; d < result.districts.size(); ++d)
    {
      auto &district = result.districts[d];
      auto r = *std::find_if(
          req.begin(),
          req.end(),
          [&](auto &r)
          {
            return r.id == district.id;
          });
      for (auto id : district.cells)
        for (auto e : graph[index.at(id)])
          if (!owner[e.to] && eligible(r, e.to) && district.area + areas[e.to] <= r.targetArea + 1e-7)
          {
            double value = distance(centers[index.at(district.anchorCell)], centers[e.to]) + areas[e.to] / e.width;
            if (value < best)
            {
              best = value;
              chosen = e.to;
              di = d;
            }
          }
    }
    if (chosen == cells.size())
      break;
    auto &d = result.districts[di];
    owner[chosen] = d.id;
    d.cells.push_back(cells[chosen].id);
    d.pieces.push_back(cells[chosen].boundary);
    d.area += areas[chosen];
  }
  for (auto &d : result.districts)
  {
    auto r = std::find_if(
        req.begin(),
        req.end(),
        [&](auto &r)
        {
          return r.id == d.id;
        });
    d.unmetTargetArea = std::max(0., r->targetArea - d.area);
    result.assignedArea += d.area;
  }
  std::sort(
      result.districts.begin(),
      result.districts.end(),
      [](auto &a, auto &b)
      {
        return a.id < b.id;
      });
  for (size_t i = 0; i < cells.size(); ++i)
    if (!owner[i])
    {
      if (usable[i])
      {
        result.unassignedCells.push_back(cells[i].id);
        result.unassignedArea += areas[i];
      }
      else
        result.excludedArea += areas[i];
    }
  out = std::move(result);
  if (error)
    error->clear();
  return true;
}
std::vector<std::vector<Position>> cityLandmarkReservations(const CityMasterPlan &p)
{
  std::vector<std::vector<Position>> out;
  for (auto &d : p.districts)
    if (d.use != DistrictUse::Residential && d.use != DistrictUse::Suburb && d.use != DistrictUse::Craftsmen && d.use != DistrictUse::Merchant && d.use != DistrictUse::WealthyResidential &&
        d.use != DistrictUse::WorkerHousing)
      out.insert(out.end(), d.pieces.begin(), d.pieces.end());
  return out;
}
} // namespace procedural::world::rewrite
