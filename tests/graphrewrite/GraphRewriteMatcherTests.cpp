// Phase 2.3: the table matcher, against a brute-force oracle.
//
// The oracle enumerates every injective map from pattern vertices to data
// vertices and keeps the ones under which every pattern edge exists. That is
// the definition; the matcher is an optimisation of it, so any disagreement is
// the matcher's fault.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/PatternMatcher.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <vector>

using namespace graphrewrite::compiler;
using namespace graphrewrite::testing;

namespace {

struct Triple
{
  uint32_t origin;
  EdgeTypeId label;
  uint32_t target;

  bool operator<(const Triple &other) const
  {
    if (origin != other.origin)
    {
      return origin < other.origin;
    }

    if (label != other.label)
    {
      return label < other.label;
    }

    return target < other.target;
  }
};

LabelledGraph makeGraph(const std::vector<Triple> &triples)
{
  LabelledGraph graph;

  for (uint32_t i = 0u; i < static_cast<uint32_t>(triples.size()); ++i)
  {
    graph.add(triples[i].origin, triples[i].label, triples[i].target, i);
  }

  graph.build();

  return graph;
}

uint32_t vertexCountOf(const std::vector<Triple> &triples)
{
  uint32_t count = 0u;

  for (const Triple &triple : triples)
  {
    count = std::max(count, std::max(triple.origin, triple.target) + 1u);
  }

  return count;
}

// Every injective assignment of pattern vertices to data vertices under which
// all pattern edges exist, rendered as a sorted tuple.
std::set<std::vector<uint32_t>> bruteForce(const std::vector<Triple> &pattern, const std::vector<Triple> &data)
{
  const uint32_t patternVertices = vertexCountOf(pattern);
  const uint32_t dataVertices = vertexCountOf(data);

  std::set<Triple> present(data.begin(), data.end());
  std::set<std::vector<uint32_t>> out;

  std::vector<uint32_t> assignment(patternVertices, 0u);
  std::vector<bool> used(dataVertices, false);

  const std::function<void(uint32_t)> recurse = [&](const uint32_t vertex) {
    if (vertex == patternVertices)
    {
      for (const Triple &edge : pattern)
      {
        if (present.count(Triple{assignment[edge.origin], edge.label, assignment[edge.target]}) == 0u)
        {
          return;
        }
      }

      out.insert(assignment);

      return;
    }

    for (uint32_t candidate = 0u; candidate < dataVertices; ++candidate)
    {
      if (used[candidate])
      {
        continue;
      }

      used[candidate] = true;
      assignment[vertex] = candidate;

      recurse(vertex + 1u);

      used[candidate] = false;
    }
  };

  if (patternVertices > 0u && dataVertices > 0u)
  {
    recurse(0u);
  }

  return out;
}

// Runs the matcher to completion and renders its rows in pattern-vertex order.
std::set<std::vector<uint32_t>> runMatcher(const std::vector<Triple> &pattern, const std::vector<Triple> &data, bool &orderValid)
{
  const LabelledGraph patternGraph = makeGraph(pattern);
  const LabelledGraph dataGraph = makeGraph(data);

  TablePatternMatcher matcher(dataGraph, patternGraph, 0u);

  orderValid = matcher.valid();

  std::set<std::vector<uint32_t>> out;

  if (!orderValid)
  {
    return out;
  }

  matcher.initiate();
  matcher.removeNonInjectiveRows();

  while (!matcher.finished())
  {
    matcher.expand();
    matcher.removeNonInjectiveRows();
  }

  const MatchTable table = matcher.result();

  if (table.matchedEdges.size() != pattern.size())
  {
    return out; // did not consume the whole pattern
  }

  const uint32_t patternVertices = vertexCountOf(pattern);

  for (uint32_t row = 0u; row < table.rows; ++row)
  {
    std::vector<uint32_t> assignment(patternVertices, 0u);

    bool complete = true;

    for (uint32_t vertex = 0u; vertex < patternVertices; ++vertex)
    {
      const uint32_t column = table.vertexToColumn[vertex];

      if (column == noColumn)
      {
        complete = false;

        break;
      }

      assignment[vertex] = table.at(row, column);
    }

    if (complete)
    {
      out.insert(assignment);
    }
  }

  return out;
}

void handWrittenCases()
{
  {
    const std::vector<Triple> pattern = {{0u, 0u, 1u}};
    const std::vector<Triple> data = {{5u, 0u, 6u}, {6u, 0u, 7u}, {5u, 1u, 7u}};

    bool ok = false;

    const std::set<std::vector<uint32_t>> got = runMatcher(pattern, data, ok);

    check(ok, "single edge pattern has a valid order");
    checkEqual(got.size(), size_t(2), "two matches for a single edge of label 0");
  }

  {
    // A triangle: exercises the cyclic step, which filters instead of widening.
    const std::vector<Triple> pattern = {{0u, 0u, 1u}, {1u, 0u, 2u}, {2u, 0u, 0u}};
    const std::vector<Triple> data = {{0u, 0u, 1u}, {1u, 0u, 2u}, {2u, 0u, 0u}};

    bool ok = false;

    const std::set<std::vector<uint32_t>> got = runMatcher(pattern, data, ok);

    checkEqual(got.size(), bruteForce(pattern, data).size(), "triangle match count agrees with brute force");
    checkEqual(got.size(), size_t(3), "a triangle matches itself three ways");
  }

  {
    // No match at all.
    const std::vector<Triple> pattern = {{0u, 0u, 1u}, {1u, 1u, 2u}};
    const std::vector<Triple> data = {{0u, 0u, 1u}};

    bool ok = false;

    const std::set<std::vector<uint32_t>> got = runMatcher(pattern, data, ok);

    checkEqual(got.size(), size_t(0), "a pattern with no match yields nothing");
  }

  {
    // Self loop in the pattern must only match a self loop in the data.
    const std::vector<Triple> pattern = {{0u, 0u, 0u}};
    const std::vector<Triple> data = {{3u, 0u, 3u}, {4u, 0u, 5u}};

    bool ok = false;

    const std::set<std::vector<uint32_t>> got = runMatcher(pattern, data, ok);

    checkEqual(got.size(), size_t(1), "a self loop matches only the self loop");
  }
}

void randomisedAgainstBruteForce()
{
  std::mt19937 rng(12345u);

  uint32_t compared = 0u;
  uint32_t mismatches = 0u;
  uint32_t nonEmpty = 0u;

  for (uint32_t trial = 0u; trial < 400u; ++trial)
  {
    const uint32_t dataVertices = 5u + rng() % 3u;
    const uint32_t labels = 1u + rng() % 2u;

    std::set<Triple> dataSet;

    const uint32_t edgeCount = 6u + rng() % 10u;

    for (uint32_t i = 0u; i < edgeCount; ++i)
    {
      const uint32_t origin = rng() % dataVertices;
      const uint32_t target = rng() % dataVertices;

      if (origin != target)
      {
        dataSet.insert(Triple{origin, static_cast<EdgeTypeId>(rng() % labels), target});
      }
    }

    if (dataSet.size() < 3u)
    {
      continue;
    }

    const std::vector<Triple> data(dataSet.begin(), dataSet.end());

    // Build a connected pattern out of a couple of real data edges, renumbered.
    std::vector<Triple> chosen;
    std::vector<Triple> pool(data.begin(), data.end());

    std::shuffle(pool.begin(), pool.end(), rng);

    for (uint32_t i = 0u; i < 3u && i < pool.size(); ++i)
    {
      chosen.push_back(pool[i]);
    }

    std::map<uint32_t, uint32_t> remap;
    std::vector<Triple> pattern;

    for (const Triple &edge : chosen)
    {
      for (const uint32_t vertex : {edge.origin, edge.target})
      {
        if (remap.count(vertex) == 0u)
        {
          const uint32_t next = static_cast<uint32_t>(remap.size());

          remap.emplace(vertex, next);
        }
      }

      pattern.push_back(Triple{remap[edge.origin], edge.label, remap[edge.target]});
    }

    std::sort(pattern.begin(), pattern.end());
    pattern.erase(std::unique(pattern.begin(), pattern.end(),
                              [](const Triple &a, const Triple &b) { return !(a < b) && !(b < a); }),
                  pattern.end());

    bool ok = false;

    const std::set<std::vector<uint32_t>> got = runMatcher(pattern, data, ok);

    if (!ok)
    {
      continue; // disconnected pattern: not this matcher's contract
    }

    const std::set<std::vector<uint32_t>> want = bruteForce(pattern, data);

    ++compared;

    if (!want.empty())
    {
      ++nonEmpty;
    }

    if (got != want)
    {
      ++mismatches;

      if (mismatches <= 2u)
      {
        std::cerr << "    mismatch on trial " << trial << ": got " << got.size() << " want " << want.size() << "\n";
      }
    }
  }

  check(compared > 100u, "enough random cases were comparable");
  check(nonEmpty > 50u, "and enough of them had a non-empty expected result");
  checkEqual(mismatches, 0u, "matcher agrees with brute force on every random case");
}

void sortBoundaries()
{
  // The original's radix sort walked an unsigned counter to -1. These sizes pin
  // the boundaries that would expose such a bug.
  for (const uint32_t n : {0u, 1u, 2u, 255u, 256u, 257u})
  {
    LabelledGraph graph;

    for (uint32_t i = 0u; i < n; ++i)
    {
      graph.add(i, static_cast<EdgeTypeId>(i % 3u), i + 1u, i);
    }

    graph.build();

    checkEqual(graph.edgeCount(), n, "graph of " + std::to_string(n) + " edges builds");

    if (n > 0u)
    {
      uint32_t first = 0u;
      uint32_t last = 0u;

      graph.forwardRange(0u, 0u, first, last);

      check(last >= first, "forward range is well formed at n=" + std::to_string(n));
    }
  }
}

} // namespace

int main()
{
  handWrittenCases();
  randomisedAgainstBruteForce();
  sortBoundaries();

  return report("GraphRewriteMatcherTests");
}
