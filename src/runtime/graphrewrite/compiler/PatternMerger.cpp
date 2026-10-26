#include "graphrewrite/compiler/PatternMerger.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace graphrewrite::compiler {
namespace {

uint32_t verticesOf(const std::vector<PatternEdge> &edges)
{
  uint32_t count = 0u;

  for (const PatternEdge &edge : edges)
  {
    count = std::max(count, std::max(edge.origin, edge.target) + 1u);
  }

  return count;
}

bool containsEdge(const std::vector<PatternEdge> &edges, const uint32_t origin, const EdgeTypeId label, const uint32_t target)
{
  for (const PatternEdge &edge : edges)
  {
    if (edge.origin == origin && edge.target == target && edge.label == label)
    {
      return true;
    }
  }

  return false;
}

struct Overlap
{
  std::vector<uint32_t> mapping; // candidate vertex -> merged vertex (or a fresh id)
  uint32_t matchedEdges = 0u;
  bool found = false;
};

// Exact largest-common-subgraph search by enumerating injective assignments of
// the candidate's vertices onto the merged graph's vertices plus fresh slots.
//
// The graphs here are rule patterns: a handful of vertices each. Choosing the
// exact maximum rather than a greedy walk is what makes the result independent
// of the order edges happen to sit in, and therefore reproducible.
void searchOverlap(const std::vector<PatternEdge> &merged, const std::vector<PatternEdge> &candidate, const uint32_t mergedVertices,
                   const uint32_t candidateVertices, std::vector<uint32_t> &current, std::vector<bool> &used, const uint32_t vertex,
                   Overlap &best)
{
  if (vertex == candidateVertices)
  {
    uint32_t matched = 0u;

    for (const PatternEdge &edge : candidate)
    {
      if (containsEdge(merged, current[edge.origin], edge.label, current[edge.target]))
      {
        ++matched;
      }
    }

    // Deterministic: more matched edges wins; ties break on the lexicographically
    // smallest mapping, never on enumeration order.
    if (!best.found || matched > best.matchedEdges || (matched == best.matchedEdges && current < best.mapping))
    {
      best.found = true;
      best.matchedEdges = matched;
      best.mapping = current;
    }

    return;
  }

  const uint32_t targets = mergedVertices + candidateVertices;

  for (uint32_t target = 0u; target < targets; ++target)
  {
    if (used[target])
    {
      continue;
    }

    used[target] = true;
    current[vertex] = target;

    searchOverlap(merged, candidate, mergedVertices, candidateVertices, current, used, vertex + 1u, best);

    used[target] = false;
  }
}

Overlap bestOverlap(const std::vector<PatternEdge> &merged, const uint32_t mergedVertices, const std::vector<PatternEdge> &candidate)
{
  const uint32_t candidateVertices = verticesOf(candidate);

  Overlap best;

  if (candidateVertices == 0u)
  {
    return best;
  }

  std::vector<uint32_t> current(candidateVertices, 0u);
  std::vector<bool> used(mergedVertices + candidateVertices, false);

  searchOverlap(merged, candidate, mergedVertices, candidateVertices, current, used, 0u, best);

  return best;
}

void appendRule(std::vector<uint32_t> &list, const uint32_t rule)
{
  if (std::find(list.begin(), list.end(), rule) == list.end())
  {
    list.push_back(rule);
  }
}

void seedFrom(const RuleGraph &graph, const uint32_t rule, MergedGraph &out)
{
  out.edges = graph.when;
  out.edgeRules.assign(out.edges.size(), std::vector<uint32_t>{rule});

  const uint32_t vertices = std::max(verticesOf(graph.when), graph.vertexCount());

  out.vertexRules.assign(vertices, std::vector<uint32_t>{rule});
  out.vertexIndices.resize(vertices);

  for (uint32_t i = 0u; i < vertices; ++i)
  {
    out.vertexIndices[i] = {i};
  }

  out.rules.push_back(rule);
}

bool mergeInto(MergedGraph &merged, const RuleGraph &graph, const uint32_t rule)
{
  const Overlap overlap = bestOverlap(merged.edges, merged.vertexCount(), graph.when);

  if (!overlap.found || overlap.matchedEdges == 0u)
  {
    return false;
  }

  const uint32_t candidateVertices = verticesOf(graph.when);

  // Fresh targets are numbered above the merged graph's vertices; give them real
  // ids now, in the order they are first referenced, so the result is stable.
  std::map<uint32_t, uint32_t> freshIds;
  std::vector<uint32_t> mapping(candidateVertices, 0u);

  for (uint32_t vertex = 0u; vertex < candidateVertices; ++vertex)
  {
    const uint32_t target = overlap.mapping[vertex];

    if (target < merged.vertexCount())
    {
      mapping[vertex] = target;

      continue;
    }

    const auto existing = freshIds.find(target);

    if (existing != freshIds.end())
    {
      mapping[vertex] = existing->second;

      continue;
    }

    const uint32_t id = merged.vertexCount();

    merged.vertexRules.emplace_back();
    merged.vertexIndices.emplace_back();

    freshIds.emplace(target, id);
    mapping[vertex] = id;
  }

  for (uint32_t vertex = 0u; vertex < candidateVertices; ++vertex)
  {
    appendRule(merged.vertexRules[mapping[vertex]], rule);
    merged.vertexIndices[mapping[vertex]].push_back(vertex);
  }

  for (const PatternEdge &edge : graph.when)
  {
    const uint32_t origin = mapping[edge.origin];
    const uint32_t target = mapping[edge.target];

    bool fused = false;

    for (uint32_t i = 0u; i < static_cast<uint32_t>(merged.edges.size()); ++i)
    {
      const PatternEdge &existing = merged.edges[i];

      if (existing.origin == origin && existing.target == target && existing.label == edge.label)
      {
        appendRule(merged.edgeRules[i], rule);
        fused = true;

        break;
      }
    }

    if (fused)
    {
      continue;
    }

    PatternEdge added;

    added.origin = origin;
    added.target = target;
    added.label = edge.label;
    added.index = static_cast<uint32_t>(merged.edges.size());

    merged.edges.push_back(added);
    merged.edgeRules.push_back(std::vector<uint32_t>{rule});
  }

  merged.rules.push_back(rule);

  return true;
}

} // namespace

std::vector<MergedGraph> mergeRuleGraphs(const std::vector<RuleGraph> &graphs)
{
  std::vector<MergedGraph> threads;

  if (graphs.empty())
  {
    return threads;
  }

  // Deterministic seed order: most edges first, ties broken by name. The
  // implementation this replaces compared with `>=` while walking an unordered
  // container, so the programme it produced could differ between runs.
  std::vector<uint32_t> order(graphs.size());

  for (uint32_t i = 0u; i < static_cast<uint32_t>(graphs.size()); ++i)
  {
    order[i] = i;
  }

  std::sort(order.begin(), order.end(), [&graphs](const uint32_t a, const uint32_t b) {
    if (graphs[a].when.size() != graphs[b].when.size())
    {
      return graphs[a].when.size() > graphs[b].when.size();
    }

    return graphs[a].name < graphs[b].name;
  });

  std::set<uint32_t> used;

  while (used.size() < graphs.size())
  {
    uint32_t seed = 0u;

    for (const uint32_t candidate : order)
    {
      if (used.count(candidate) == 0u)
      {
        seed = candidate;

        break;
      }
    }

    used.insert(seed);

    MergedGraph merged;

    seedFrom(graphs[seed], seed, merged);

    bool progressed = true;

    while (progressed)
    {
      progressed = false;

      for (const uint32_t candidate : order)
      {
        if (used.count(candidate) != 0u)
        {
          continue;
        }

        if (mergeInto(merged, graphs[candidate], candidate))
        {
          used.insert(candidate);
          progressed = true;
        }
      }
    }

    threads.push_back(std::move(merged));
  }

  return threads;
}

std::string dumpMergedGraph(const MergedGraph &graph, const EdgeTypeTable &table)
{
  std::string out;

  for (uint32_t i = 0u; i < static_cast<uint32_t>(graph.edges.size()); ++i)
  {
    const PatternEdge &edge = graph.edges[i];

    out += "edge" + std::to_string(i) + ": " + std::to_string(edge.origin) + " --(" + table[edge.label].pattern + ")--> " +
           std::to_string(edge.target) + "   rules";

    for (const uint32_t rule : graph.edgeRules[i])
    {
      out += " " + std::to_string(rule);
    }

    out += '\n';
  }

  return out;
}

} // namespace graphrewrite::compiler
