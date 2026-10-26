#include "WorldRidgeFeatures.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
namespace procedural::world::rewrite
{
namespace
{
struct Sets
{
  std::vector<uint32_t> parent;
  explicit Sets(size_t n) : parent(n)
  {
    std::iota(parent.begin(), parent.end(), 0);
  }
  uint32_t find(uint32_t a)
  {
    while (parent[a] != a)
    {
      parent[a] = parent[parent[a]];
      a = parent[a];
    }
    return a;
  }
  void join(uint32_t a, uint32_t b)
  {
    a = find(a);
    b = find(b);
    parent[std::max(a, b)] = std::min(a, b);
  }
};
} // namespace
bool analyzeReliefTopology(const std::vector<Position> &vertices, const std::vector<std::array<uint32_t, 2>> &inputEdges, double base, double threshold, RidgeFeatures &output, std::string *error)
{
  auto fail = [&](const char *s)
  {
    if (error)
      *error = s;
    return false;
  };
  if (vertices.empty() || vertices.size() > 1000000 || !std::isfinite(base) || !std::isfinite(threshold) || threshold < 0)
    return fail("Invalid relief topology settings");
  for (auto p : vertices)
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || p.y < base)
      return fail("Invalid relief vertex or reference level above terrain");
  Sets plateaus(vertices.size());
  std::set<std::array<uint32_t, 2>> edges;
  for (auto e : inputEdges)
  {
    if (e[0] >= vertices.size() || e[1] >= vertices.size() || e[0] == e[1])
      return fail("Invalid relief graph edge");
    if (e[0] > e[1])
      std::swap(e[0], e[1]);
    edges.insert(e);
    if (vertices[e[0]].y == vertices[e[1]].y)
      plateaus.join(e[0], e[1]);
  }
  std::map<uint32_t, std::set<uint32_t>> adjacency;
  for (uint32_t i = 0; i < vertices.size(); ++i)
    adjacency[plateaus.find(i)];
  for (auto e : edges)
  {
    auto a = plateaus.find(e[0]), b = plateaus.find(e[1]);
    if (a != b)
    {
      adjacency[a].insert(b);
      adjacency[b].insert(a);
    }
  }
  std::vector<uint32_t> order;
  for (auto &entry : adjacency)
    order.push_back(entry.first);
  std::sort(
      order.begin(),
      order.end(),
      [&](auto a, auto b)
      {
        return vertices[a].y != vertices[b].y ? vertices[a].y > vertices[b].y : a < b;
      });
  Sets sets(vertices.size());
  std::vector<bool> active(vertices.size());
  std::vector<uint32_t> dominant(vertices.size(), UINT32_MAX);
  RidgeFeatures next;
  for (auto v : order)
  {
    std::set<uint32_t> higher;
    for (auto n : adjacency[v])
      if (active[n])
        higher.insert(sets.find(n));
    active[v] = true;
    if (higher.empty())
    {
      dominant[v] = uint32_t(next.peaks.size());
      next.peaks.push_back({v, UINT32_MAX, UINT32_MAX, vertices[v].y, 0, false});
      continue;
    }
    uint32_t elder = UINT32_MAX;
    for (auto root : higher)
    {
      auto peak = dominant[root];
      if (elder == UINT32_MAX || next.peaks[peak].heightMetres > next.peaks[elder].heightMetres || (next.peaks[peak].heightMetres == next.peaks[elder].heightMetres && next.peaks[peak].vertex < next.peaks[elder].vertex))
        elder = peak;
    }
    for (auto root : higher)
    {
      auto peak = dominant[root];
      if (peak != elder)
      {
        auto &p = next.peaks[peak];
        p.parentPeak = elder;
        p.keySaddle = v;
        p.prominenceMetres = p.heightMetres - vertices[v].y;
        p.significant = p.prominenceMetres >= threshold && p.prominenceMetres > 0;
        if (p.significant)
          next.passes.push_back({v, peak, elder, vertices[v].y, p.prominenceMetres});
      }
      sets.join(v, root);
    }
    dominant[sets.find(v)] = elder;
  }
  for (auto &p : next.peaks)
    if (p.parentPeak == UINT32_MAX)
    {
      ++next.components;
      p.prominenceMetres = p.heightMetres - base;
      p.significant = p.prominenceMetres >= threshold && p.prominenceMetres > 0;
    }
  output = std::move(next);
  if (error)
    error->clear();
  return true;
}
bool analyzeRidgeFeatures(const WatershedProduct &w, double base, double threshold, RidgeFeatures &output, std::string *error)
{
  std::set<uint32_t> used;
  for (auto edge : w.edges)
    if (!edge.riverCrossing)
    {
      if (edge.a >= w.vertices.size() || edge.b >= w.vertices.size())
      {
        if (error)
          *error = "Invalid crest edge";
        return false;
      }
      used.insert(edge.a);
      used.insert(edge.b);
    }
  if (used.empty())
  {
    output = {};
    if (error)
      error->clear();
    return true;
  }
  std::vector<Position> positions;
  std::vector<uint32_t> originals;
  std::map<uint32_t, uint32_t> indices;
  for (auto id : used)
  {
    indices[id] = uint32_t(positions.size());
    originals.push_back(id);
    positions.push_back(w.vertices[id].position);
  }
  std::vector<std::array<uint32_t, 2>> edges;
  for (auto edge : w.edges)
    if (!edge.riverCrossing)
      edges.push_back({indices.at(edge.a), indices.at(edge.b)});
  RidgeFeatures next;
  if (!analyzeReliefTopology(positions, edges, base, threshold, next, error))
    return false;
  for (auto &peak : next.peaks)
  {
    peak.vertex = originals[peak.vertex];
    if (peak.keySaddle != UINT32_MAX)
      peak.keySaddle = originals[peak.keySaddle];
  }
  for (auto &pass : next.passes)
    pass.vertex = originals[pass.vertex];
  output = std::move(next);
  return true;
}
} // namespace procedural::world::rewrite
