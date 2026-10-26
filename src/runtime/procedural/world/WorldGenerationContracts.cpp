#include "procedural/world/WorldGenerationContracts.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <set>

namespace procedural::world::rewrite
{
namespace
{
bool fail(std::string *error, const std::string &message)
{
  if (error)
    *error = message;
  return false;
}
bool finite(Position p)
{
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
bool same(Position a, Position b)
{
  return a.x == b.x && a.y == b.y && a.z == b.z;
}
uint64_t mix(uint64_t x)
{
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}
} // namespace
bool WorldSpec::validate(std::string *error) const
{
  if (error)
    error->clear();
  return generatorVersion != 0 && std::isfinite(extentMetres) && extentMetres > 0 && std::isfinite(seaLevelMetres) ? true : fail(error, "invalid world units, extent or version");
}
uint64_t featureSeed(uint64_t seed, uint64_t version, uint64_t feature, uint64_t purpose, uint64_t sample)
{
  for (uint64_t value : {version, feature, purpose, sample})
    seed = mix(seed ^ mix(value));
  return seed;
}
bool BoundaryRegistry::commit(const std::vector<BoundaryInput> &inputs, std::string *error)
{
  if (error)
    error->clear();
  struct Pending
  {
    BoundaryEndpoint a, b;
    std::set<uint64_t> ticks;
  };
  std::map<uint64_t, Pending> edges;
  std::map<uint64_t, Position> endpoints;
  for (const auto &input : inputs)
  {
    if (!input.id || input.id == UINT64_MAX || !input.from.id || !input.to.id || input.from.id == input.to.id || !finite(input.from.position) || !finite(input.to.position) ||
        (input.from.position.x == input.to.position.x && input.from.position.z == input.to.position.z))
      return fail(error, "invalid or horizontally degenerate boundary");
    const bool reversed = input.from.id > input.to.id;
    const auto a = reversed ? input.to : input.from;
    const auto b = reversed ? input.from : input.to;
    for (const auto &endpoint : {a, b})
    {
      const auto entry = endpoints.emplace(endpoint.id, endpoint.position);
      if (!entry.second && !same(entry.first->second, endpoint.position))
        return fail(error, "shared vertex has inconsistent coordinates");
    }
    auto entry = edges.emplace(input.id, Pending{a, b, {0, parameterTicks}});
    auto &edge = entry.first->second;
    if (edge.a.id != a.id || edge.b.id != b.id)
      return fail(error, "edge ID has inconsistent endpoints");
    for (double t : input.splits)
    {
      if (!std::isfinite(t) || t < 0 || t > 1)
        return fail(error, "invalid boundary split parameter");
      // Quantize first, then reverse in integer space to avoid 1-(1-t) drift.
      auto tick = static_cast<uint64_t>(std::llround(t * parameterTicks));
      edge.ticks.insert(reversed ? parameterTicks - tick : tick);
    }
  }
  std::vector<BoundaryRecord> committed;
  for (const auto &[id, edge] : edges)
  {
    BoundaryRecord record;
    record.id = id;
    for (const auto tick : edge.ticks)
    {
      if (tick == 0)
        record.vertices.push_back({0, edge.a.id, edge.a.position});
      else if (tick == parameterTicks)
        record.vertices.push_back({0, edge.b.id, edge.b.position});
      else
      {
        const double t = double(tick) / double(parameterTicks);
        const auto a = edge.a.position, b = edge.b.position;
        const Position p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
        if (!finite(p))
          return fail(error, "boundary interpolation overflow");
        record.vertices.push_back({id, tick, p});
      }
    }
    committed.push_back(std::move(record));
  }
  records_.swap(committed);
  return true;
}
const BoundaryRecord *BoundaryRegistry::find(uint64_t id) const
{
  auto it = std::lower_bound(
      records_.begin(),
      records_.end(),
      id,
      [](const auto &record, uint64_t key)
      {
        return record.id < key;
      });
  return it != records_.end() && it->id == id ? &*it : nullptr;
}

bool accumulateDischarge(const std::vector<FlowNode> &nodes, const std::vector<FlowLink> &links, std::vector<FlowResult> &result, std::string *error)
{
  if (error)
    error->clear();
  struct State
  {
    FlowNode node;
    double incoming = 0;
    size_t indegree = 0;
    std::vector<FlowLink> outgoing;
  };
  std::map<uint64_t, State> states;
  for (const auto &node : nodes)
  {
    if (!node.id || !std::isfinite(node.localRunoffCumecs) || node.localRunoffCumecs < 0 || !std::isfinite(node.lossCumecs) || node.lossCumecs < 0 || !states.emplace(node.id, State{node}).second)
      return fail(error, "invalid or duplicate flow node");
  }
  std::set<std::pair<uint64_t, uint64_t>> seen;
  for (const auto &link : links)
  {
    if (!states.count(link.from) || !states.count(link.to) || link.from == link.to || !std::isfinite(link.fraction) || link.fraction <= 0 || link.fraction > 1 || !seen.emplace(link.from, link.to).second)
      return fail(error, "invalid or duplicate flow link");
    states.at(link.from).outgoing.push_back(link);
    ++states.at(link.to).indegree;
  }
  std::priority_queue<uint64_t, std::vector<uint64_t>, std::greater<uint64_t>> ready;
  for (auto &[id, state] : states)
  {
    std::sort(
        state.outgoing.begin(),
        state.outgoing.end(),
        [](auto a, auto b)
        {
          return a.to < b.to;
        });
    double total = 0;
    for (const auto &link : state.outgoing)
      total += link.fraction;
    if (!state.outgoing.empty() && std::abs(total - 1) > 1e-12)
      return fail(error, "outgoing flow fractions must sum to one");
    if (!state.indegree)
      ready.push(id);
  }
  std::map<uint64_t, FlowResult> committed;
  while (!ready.empty())
  {
    const auto id = ready.top();
    ready.pop();
    const auto &state = states.at(id);
    const double q = state.incoming + state.node.localRunoffCumecs - state.node.lossCumecs;
    if (!std::isfinite(q) || q < 0)
      return fail(error, "loss exceeds available water or discharge overflow");
    committed.emplace(id, FlowResult{id, state.incoming, q});
    for (const auto &link : state.outgoing)
    {
      auto &next = states.at(link.to);
      next.incoming += q * link.fraction;
      if (--next.indegree == 0)
        ready.push(link.to);
    }
  }
  if (committed.size() != nodes.size())
    return fail(error, "cycle in flow graph");
  std::vector<FlowResult> output;
  for (const auto &[id, value] : committed)
    output.push_back(value);
  result.swap(output);
  return true;
}

bool RoadDesignProfile::build(const std::vector<RoadStation> &stations, double gradeLimit, double crossLimit, std::string *error)
{
  if (error)
    error->clear();
  if (stations.size() < 2 || !std::isfinite(gradeLimit) || gradeLimit < 0 || !std::isfinite(crossLimit) || crossLimit < 0)
    return fail(error, "invalid road profile limits or station count");
  for (size_t i = 0; i < stations.size(); ++i)
  {
    const auto &s = stations[i];
    if (!std::isfinite(s.distanceMetres) || !std::isfinite(s.heightMetres) || !std::isfinite(s.halfWidthMetres) || s.halfWidthMetres <= 0 || !std::isfinite(s.crossSlope) || std::abs(s.crossSlope) > crossLimit)
      return fail(error, "invalid road station or cross slope");
    if (i)
    {
      const auto &p = stations[i - 1];
      const double ds = s.distanceMetres - p.distanceMetres;
      if (!(ds > 0) || !std::isfinite(ds))
        return fail(error, "road stations must increase in distance");
      // Bound the entire panel, not just the centerline, when camber changes.
      const double base = (s.heightMetres - p.heightMetres) / ds;
      const double lateral = std::abs(s.crossSlope - p.crossSlope) / ds * std::max(s.halfWidthMetres, p.halfWidthMetres);
      if (!std::isfinite(base) || std::abs(base) + lateral > gradeLimit + 1e-12)
        return fail(error, "road panel exceeds maximum longitudinal grade");
    }
  }
  stations_ = stations;
  return true;
}
bool RoadDesignProfile::height(double d, double l, double &h) const
{
  if (stations_.empty() || !std::isfinite(d) || !std::isfinite(l) || d < stations_.front().distanceMetres || d > stations_.back().distanceMetres)
    return false;
  auto upper = std::upper_bound(
      stations_.begin(),
      stations_.end(),
      d,
      [](double distance, const auto &s)
      {
        return distance < s.distanceMetres;
      });
  if (upper == stations_.end())
    --upper;
  const auto &b = *upper, &a = *(upper - 1);
  const double t = (d - a.distanceMetres) / (b.distanceMetres - a.distanceMetres);
  if (std::abs(l) > a.halfWidthMetres + t * (b.halfWidthMetres - a.halfWidthMetres))
    return false;
  h = a.heightMetres + t * (b.heightMetres - a.heightMetres) + l * (a.crossSlope + t * (b.crossSlope - a.crossSlope));
  return std::isfinite(h);
}
} // namespace procedural::world::rewrite
