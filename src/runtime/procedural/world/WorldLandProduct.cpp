#include "WorldLandProduct.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <tuple>

namespace procedural::world::rewrite {
namespace {
bool fail(std::string *error, const char *message)
{
  if (error)
  {
    *error = message;
  }
  return false;
}

using Key = std::pair<int64_t, int64_t>;
constexpr double grid = 1e-6;

struct Use
{
  uint32_t cell;
  bool reversed;
};
} // namespace

bool buildLandProduct(const WorldSpec &spec, const std::vector<VoronoiCell> &partition, const LandSettings &settings, LandProduct &output, std::string *error)
{
  if (!spec.validate(error))
  {
    return false;
  }
  if (spec.extentMetres > 1e9 || partition.empty() || partition.size() > 100000 || !std::isfinite(settings.landFraction) || settings.landFraction <= 0 || settings.landFraction >= 1 || settings.cores.empty() ||
      !std::isfinite(settings.oceanMarginMetres) || settings.oceanMarginMetres < 0 || settings.oceanMarginMetres * 2 >= spec.extentMetres)
  {
    return fail(error, "Invalid land settings or partition size");
  }
  LandProduct next;
  next.spec = spec;
  std::map<Key, uint64_t> points;
  std::vector<std::vector<Key>> rings;
  for (const auto &cell : partition)
  {
    if (!std::isfinite(cell.site.x) || !std::isfinite(cell.site.y) || cell.area.size() < 3)
    {
      return fail(error, "Partition cell has fewer than three corners");
    }
    std::vector<Key> ring;
    for (const auto &p : cell.area)
    {
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x < 0 || p.y < 0 || p.x > spec.extentMetres || p.y > spec.extentMetres)
      {
        return fail(error, "Partition corner outside world");
      }
      Key key{std::llround(p.x / grid), std::llround(p.y / grid)};
      if (std::find(ring.begin(), ring.end(), key) != ring.end())
      {
        return fail(error, "Partition corner collapses at canonical precision");
      }
      ring.push_back(key);
      points.emplace(key, 0);
    }
    rings.push_back(std::move(ring));
  }
  uint64_t id = 0;
  for (auto &p : points)
  {
    p.second = ++id;
  }
  std::map<std::pair<uint64_t, uint64_t>, std::vector<Use>> edges;
  for (uint32_t c = 0; c < rings.size(); ++c)
  {
    const auto &r = rings[c];
    for (size_t j = 0; j < r.size(); ++j)
    {
      const auto a = points.at(r[j]), b = points.at(r[(j + 1) % r.size()]);
      edges[{std::min(a, b), std::max(a, b)}].push_back({c, a > b});
    }
  }
  std::vector<Position> positions(points.size() + 1);
  for (const auto &p : points)
  {
    positions[p.second] = {p.first.first * grid, spec.seaLevelMetres, p.first.second * grid};
  }
  std::vector<BoundaryInput> inputs;
  std::vector<std::vector<uint32_t>> adjacency(partition.size());
  next.cells.resize(partition.size());
  for (uint32_t c = 0; c < partition.size(); ++c)
  {
    next.cells[c].id = c;
    next.cells[c].patch.id = c + 1;
  }
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> edgeIds;
  id = 0;
  for (const auto &entry : edges)
  {
    const auto &uses = entry.second;
    auto a = entry.first.first, b = entry.first.second;
    if (uses.size() > 2 || (uses.size() == 2 && uses[0].reversed == uses[1].reversed))
    {
      return fail(error, "Partition has nonmanifold or inconsistently wound edges");
    }
    if (uses.size() == 1)
    {
      const auto &p = positions[a], &q = positions[b];
      double e = std::round(spec.extentMetres / grid) * grid;
      if (!((p.x == q.x && (p.x == 0 || p.x == e)) || (p.z == q.z && (p.z == 0 || p.z == e))))
      {
        return fail(error, "Partition has an internal crack or unmatched subdivision");
      }
    }
    else
    {
      adjacency[uses[0].cell].push_back(uses[1].cell);
      adjacency[uses[1].cell].push_back(uses[0].cell);
    }
    edgeIds[entry.first] = ++id;
    inputs.push_back({id, {a, positions[a]}, {b, positions[b]}, {}});
  }
  if (!next.boundaries.commit(inputs, error))
  {
    return false;
  }
  double largest = 0;
  for (uint32_t c = 0; c < rings.size(); ++c)
  {
    const auto &r = rings[c];
    for (size_t j = 0; j < r.size(); ++j)
    {
      auto a = points.at(r[j]), b = points.at(r[(j + 1) % r.size()]);
      next.cells[c].patch.outer.push_back({edgeIds.at({std::min(a, b), std::max(a, b)}), a > b});
      next.cells[c].areaMetres2 += (positions[a].x * positions[b].z - positions[b].x * positions[a].z) * .5;
    }
    PatchMesh validated;
    if (next.cells[c].areaMetres2 <= 0 || !compilePatchMesh(
                                              next.cells[c].patch,
                                              next.boundaries,
                                              {},
                                              [&](double, double) {
                                                return spec.seaLevelMetres;
                                              },
                                              validated,
                                              error))
    {
      return false;
    }
    next.totalAreaMetres2 += next.cells[c].areaMetres2;
    largest = std::max(largest, next.cells[c].areaMetres2);
  }
  if (std::abs(next.totalAreaMetres2 - spec.extentMetres * spec.extentMetres) > std::max(1e-4, spec.extentMetres * grid * 8))
  {
    return fail(error, "Partition does not cover the world square");
  }
  using Candidate = std::pair<double, uint32_t>;
  std::priority_queue<Candidate, std::vector<Candidate>, std::greater<Candidate>> queue;
  std::vector<double> costs(partition.size(), std::numeric_limits<double>::infinity());
  std::vector<bool> eligible(partition.size(), true);
  if (settings.oceanMarginMetres > 0)
  {
    for (size_t i = 0; i < partition.size(); ++i)
    {
      for (const auto &p : partition[i].area)
      {
        eligible[i] =
            eligible[i] && p.x >= settings.oceanMarginMetres && p.y >= settings.oceanMarginMetres && p.x <= spec.extentMetres - settings.oceanMarginMetres && p.y <= spec.extentMetres - settings.oceanMarginMetres;
      }
    }
  }
  std::set<uint32_t> cores(settings.cores.begin(), settings.cores.end());
  double coreArea = 0;
  for (auto c : cores)
  {
    if (c >= partition.size() || !eligible[c])
    {
      return fail(error, "Land core outside partition");
    }
    costs[c] = 0;
    queue.push({0, c});
    coreArea += next.cells[c].areaMetres2;
  }
  double target = settings.landFraction * next.totalAreaMetres2;
  if (coreArea > target)
  {
    return fail(error, "Land budget cannot retain all cores");
  }
  while (!queue.empty())
  {
    auto [cost, c] = queue.top();
    queue.pop();
    if (next.cells[c].land || cost != costs[c])
    {
      continue;
    }
    if (cost > 0 && next.landAreaMetres2 >= target)
    {
      break;
    }
    next.cells[c].land = true;
    next.landAreaMetres2 += next.cells[c].areaMetres2;
    for (auto n : adjacency[c])
    {
      if (!eligible[n])
      {
        continue;
      }
      const auto &a = partition[c].site, &b = partition[n].site;
      if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y))
      {
        return fail(error, "Invalid partition site");
      }
      double jitter = double(featureSeed(spec.seed, spec.generatorVersion, n, 0x4c414e44) >> 11) / 9007199254740992.0;
      double candidate = cost + std::hypot(a.x - b.x, a.y - b.y) * (0.8 + 0.4 * jitter);
      if (candidate < costs[n])
      {
        costs[n] = candidate;
        queue.push({candidate, n});
      }
    }
  }
  if (next.landAreaMetres2 < target || next.landAreaMetres2 - target > largest + 1e-5)
  {
    return fail(error, "Land growth cannot satisfy area budget");
  }
  std::vector<bool> visited(partition.size());
  for (uint32_t c = 0; c < partition.size(); ++c)
  {
    if (next.cells[c].land && !visited[c])
    {
      ++next.components;
      std::vector<uint32_t> pending{c};
      visited[c] = true;
      while (!pending.empty())
      {
        auto n = pending.back();
        pending.pop_back();
        for (auto a : adjacency[n])
        {
          if (next.cells[a].land && !visited[a])
          {
            visited[a] = true;
            pending.push_back(a);
          }
        }
      }
    }
  }

  struct CoastEdge
  {
    uint64_t end;
    BoundaryUse use;
  };

  std::map<uint64_t, CoastEdge> coast;
  for (const auto &entry : edges)
  {
    const auto &uses = entry.second;
    unsigned count = 0;
    Use land{};
    for (auto u : uses)
    {
      if (next.cells[u.cell].land)
      {
        ++count;
        land = u;
      }
    }
    if (count != 1)
    {
      continue;
    }
    auto a = entry.first.first, b = entry.first.second;
    if (land.reversed)
    {
      std::swap(a, b);
    }
    if (!coast.emplace(a, CoastEdge{b, {edgeIds.at(entry.first), land.reversed}}).second)
    {
      return fail(error, "Land boundary touches itself at a vertex; repair planning topology");
    }
  }
  while (!coast.empty())
  {
    CoastLoop loop;
    uint64_t start = coast.begin()->first, current = start;
    do
    {
      auto it = coast.find(current);
      if (it == coast.end())
      {
        return fail(error, "Open land boundary");
      }
      auto edge = it->second;
      coast.erase(it);
      loop.edges.push_back(edge.use);
      const auto &a = positions[current], &b = positions[edge.end];
      loop.signedAreaMetres2 += (a.x * b.z - b.x * a.z) * .5;
      current = edge.end;
    } while (current != start);
    next.coasts.push_back(std::move(loop));
  }
  output = std::move(next);
  if (error)
  {
    error->clear();
  }
  return true;
}

bool ReliefControls::build(std::vector<ReliefProvince> provinces, ReliefSample lowland, std::string *error)
{
  auto valid = [](ReliefSample p) {
    return std::isfinite(p.riverSlope) && p.riverSlope > 0 && p.riverSlope <= .25 && std::isfinite(p.terrainLambda) && p.terrainLambda >= 0 && p.terrainLambda <= .25;
  };
  if (!valid(lowland))
  {
    return fail(error, "Invalid lowland slope controls");
  }
  std::sort(provinces.begin(), provinces.end(), [](auto &a, auto &b) {
    return a.id < b.id;
  });
  for (size_t i = 0; i < provinces.size(); ++i)
  {
    auto &p = provinces[i];
    if ((i && p.id == provinces[i - 1].id) || !valid({p.riverSlope, p.terrainLambda}) || !std::isfinite(p.strength) || p.strength <= 0 || p.strength > 100 || !std::isfinite(p.radiusMetres) || p.radiusMetres <= 0 ||
        !std::isfinite(p.from.x) || !std::isfinite(p.from.z) || !std::isfinite(p.to.x) || !std::isfinite(p.to.z))
    {
      return fail(error, "Invalid relief province");
    }
  }
  provinces_ = std::move(provinces);
  lowland_ = lowland;
  if (error)
  {
    error->clear();
  }
  return true;
}

ReliefSample ReliefControls::sample(double x, double z) const
{
  // Convex, compact-support blending; no elevation or radial island bias.
  ReliefSample sum = lowland_;
  double weight = 1;
  for (const auto &p : provinces_)
  {
    double dx = p.to.x - p.from.x, dz = p.to.z - p.from.z, l2 = dx * dx + dz * dz;
    double t = l2 > 0 ? std::clamp(((x - p.from.x) * dx + (z - p.from.z) * dz) / l2, 0.0, 1.0) : 0;
    double d = std::hypot(x - p.from.x - t * dx, z - p.from.z - t * dz) / p.radiusMetres;
    double w = p.strength * std::pow(std::max(0.0, 1 - d * d), 2);
    weight += w;
    sum.riverSlope += w * p.riverSlope;
    sum.terrainLambda += w * p.terrainLambda;
  }
  return {sum.riverSlope / weight, sum.terrainLambda / weight};
}
} // namespace procedural::world::rewrite
