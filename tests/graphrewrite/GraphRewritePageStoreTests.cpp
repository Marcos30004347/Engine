// Phase 3.1: paged edge storage on the CPU.

#include "TestHarness.hpp"
#include "graphrewrite/compiler/Compiler.hpp"
#include "graphrewrite/runtime/EdgePageStore.hpp"

#include <algorithm>
#include <fstream>
#include <random>
#include <set>
#include <sstream>

using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace graphrewrite::testing;

namespace {

std::vector<Edge> sortedCopy(std::vector<Edge> edges)
{
  std::sort(edges.begin(), edges.end());

  return edges;
}

void buildPreservesEveryEdge()
{
  std::mt19937 rng(7u);

  for (uint32_t trial = 0u; trial < 40u; ++trial)
  {
    const uint32_t count = rng() % 2000u;

    std::vector<Edge> edges;

    for (uint32_t i = 0u; i < count; ++i)
    {
      edges.push_back(Edge{rng() % 200u, rng() % 200u});
    }

    Partition partition(0u, KeyOrder::ByOrigin, 0u);

    partition.build(edges);

    const std::vector<Edge> got = sortedCopy(partition.snapshot());
    const std::vector<Edge> want = sortedCopy(edges);

    if (got != want)
    {
      check(false, "build preserves every edge as a multiset");

      return;
    }
  }

  check(true, "build preserves every edge as a multiset over 40 random inputs");
}

void pagesAreBoundedAndSorted()
{
  std::vector<Edge> edges;

  for (uint32_t i = 0u; i < 1000u; ++i)
  {
    edges.push_back(Edge{i % 97u, i});
  }

  Partition partition(0u, KeyOrder::ByOrigin, 0u);

  partition.build(edges);

  check(partition.pageCount() >= 4u, "1000 edges need several pages of 256");

  std::string problem;

  check(partition.checkInvariants(problem), "invariants hold after build: " + problem);

  for (uint32_t position = 0u; position < partition.pageCount(); ++position)
  {
    check(partition.pageAt(position).size() <= pageEdges, "no page exceeds its capacity");
  }
}

void keyOrderSelectsTheKey()
{
  const std::vector<Edge> edges = {{5u, 1u}, {3u, 9u}, {7u, 0u}};

  Partition byOrigin(0u, KeyOrder::ByOrigin, 0u);
  Partition byTarget(0u, KeyOrder::ByTarget, 1u);

  byOrigin.build(edges);
  byTarget.build(edges);

  checkEqual(byOrigin.pageAt(0u).front().origin, 3u, "origin order starts at the smallest origin");
  checkEqual(byTarget.pageAt(0u).front().target, 0u, "target order starts at the smallest target");
}

void insertSplitsAndKeepsIdsStable()
{
  std::vector<Edge> edges;

  for (uint32_t i = 0u; i < pageEdges; ++i)
  {
    edges.push_back(Edge{i, i});
  }

  Partition partition(0u, KeyOrder::ByOrigin, 0u);

  partition.build(edges);

  checkEqual(partition.pageCount(), 1u, "exactly one full page to start");

  const PageId before = partition.pageIdAt(0u);

  partition.insert(Edge{500u, 500u});

  checkEqual(partition.pageCount(), 2u, "the overflowing insert split the page");

  // The original page keeps its id; the split allocates a NEW one. Nothing that
  // already had a page id sees it change.
  const bool stillPresent = partition.pageIdAt(0u) == before || partition.pageIdAt(1u) == before;

  check(stillPresent, "the pre-existing PageId survives the split");
  check(partition.pageIdAt(0u) != partition.pageIdAt(1u), "the new page has a distinct id");

  std::string problem;

  check(partition.checkInvariants(problem), "invariants hold after a split: " + problem);
}

void insertAndRemoveRoundTrip()
{
  std::mt19937 rng(99u);

  std::vector<Edge> edges;

  for (uint32_t i = 0u; i < 600u; ++i)
  {
    edges.push_back(Edge{rng() % 50u, rng() % 50u});
  }

  Partition partition(0u, KeyOrder::ByOrigin, 0u);

  partition.build(edges);

  std::vector<Edge> expected = edges;

  for (uint32_t i = 0u; i < 400u; ++i)
  {
    if ((rng() % 2u) == 0u || expected.empty())
    {
      const Edge added{rng() % 50u, rng() % 50u};

      partition.insert(added);
      expected.push_back(added);
    }
    else
    {
      const size_t index = rng() % expected.size();
      const Edge removed = expected[index];

      check(partition.remove(removed), "remove finds an edge that is present");

      expected.erase(expected.begin() + static_cast<long>(index));
    }

    std::string problem;

    if (!partition.checkInvariants(problem))
    {
      check(false, "invariants hold through insert/remove: " + problem);

      return;
    }
  }

  check(sortedCopy(partition.snapshot()) == sortedCopy(expected), "contents match after 400 mixed operations");
}

void removingTheLowestEdgeMovesMinKey()
{
  // The failure this guards is silent: a directory that still claims the old
  // range stops returning the page for keys it still holds, and matches simply
  // stop appearing.
  std::vector<Edge> edges;

  for (uint32_t i = 10u; i < 20u; ++i)
  {
    edges.push_back(Edge{i, i});
  }

  Partition partition(0u, KeyOrder::ByOrigin, 0u);

  partition.build(edges);

  checkEqual(partition.minKeyAt(0u), 10u, "minKey starts at the lowest origin");

  check(partition.remove(Edge{10u, 10u}), "the lowest edge is removed");

  checkEqual(partition.minKeyAt(0u), 11u, "minKey moved up with it");
  checkEqual(partition.maxKeyAt(0u), 19u, "maxKey is unchanged");

  check(partition.remove(Edge{19u, 19u}), "the highest edge is removed");

  checkEqual(partition.maxKeyAt(0u), 18u, "maxKey moved down with it");
}

void removingSomethingAbsentIsFalse()
{
  Partition partition(0u, KeyOrder::ByOrigin, 0u);

  partition.build({{1u, 2u}, {3u, 4u}});

  check(!partition.remove(Edge{9u, 9u}), "removing an absent edge reports false");
  check(!partition.remove(Edge{1u, 9u}), "and so does a near miss on an existing key");
  checkEqual(partition.snapshot().size(), size_t(2), "and nothing is lost");
}

void storeBuildsOnlyTheDirectionsUsed()
{
  compiler::EdgeTypeTable types;

  const EdgeTypeId forwardOnly = types.intern("? a ?", 2u);
  const EdgeTypeId both = types.intern("? b ?", 2u);
  const EdgeTypeId unused = types.intern("? c ?", 2u);

  types.mutableType(forwardOnly).usedForward = true;
  types.mutableType(both).usedForward = true;
  types.mutableType(both).usedBackward = true;

  EdgePageStore store;

  store.configure(types);

  checkEqual(store.partitionCount(), 4u, "one origin partition per type, plus a target partition for the two-way one");

  check(store.has(forwardOnly, KeyOrder::ByOrigin), "forward-only type has an origin partition");
  check(!store.has(forwardOnly, KeyOrder::ByTarget), "and no target partition - that is where the saving is");
  check(store.has(both, KeyOrder::ByOrigin), "two-way type has an origin partition");
  check(store.has(both, KeyOrder::ByTarget), "and a target partition");

  // A type no rule matches on still needs somewhere to keep its edges: a rewrite
  // can insert one, and it must not vanish.
  check(store.has(unused, KeyOrder::ByOrigin), "an untraversed type still has an origin partition");
  check(!store.has(unused, KeyOrder::ByTarget), "but no target one");
}

void addEdgeReachesEveryPartition()
{
  compiler::EdgeTypeTable types;

  const EdgeTypeId type = types.intern("? a ?", 2u);

  types.mutableType(type).usedForward = true;
  types.mutableType(type).usedBackward = true;

  EdgePageStore store;

  store.configure(types);
  store.addEdge(type, Edge{4u, 9u});

  checkEqual(store.partition(type, KeyOrder::ByOrigin).snapshot().size(), size_t(1), "origin partition has it");
  checkEqual(store.partition(type, KeyOrder::ByTarget).snapshot().size(), size_t(1), "target partition has it too");

  checkEqual(store.vertexCount(), 10u, "the vertex allocator advanced past the edge's ids");

  check(store.removeEdge(type, Edge{4u, 9u}), "removal reports success");
  checkEqual(store.partition(type, KeyOrder::ByOrigin).snapshot().size(), size_t(0), "and clears both partitions");
  checkEqual(store.partition(type, KeyOrder::ByTarget).snapshot().size(), size_t(0), "including the target one");
}

void vertexAllocatorIsMonotone()
{
  EdgePageStore store;

  const uint32_t first = store.allocateVertex();
  const uint32_t second = store.allocateVertex();

  checkEqual(second, first + 1u, "vertex ids are handed out in sequence");

  store.reserveVertices(100u);

  checkEqual(store.allocateVertex(), 100u, "reserving skips the allocator forward");
}

void configuredFromTheCorpusProgramme()
{
  std::ifstream file(std::string(GRAPHREWRITE_ASSET_DIR) + "/basic.graphwrite");

  if (!file)
  {
    check(false, "corpus asset found");

    return;
  }

  std::ostringstream buffer;

  buffer << file.rdbuf();

  DiagnosticSink sink;
  compiler::CompiledProgram program;
  compiler::UsageValues usage;

  check(compiler::compile(buffer.str(), "basic.graphwrite", compiler::CompileOptions(), sink, program, usage), "corpus compiles");

  EdgePageStore store;

  store.configure(program.edgeTypes);

  uint32_t forward = 0u;
  uint32_t backward = 0u;

  for (EdgeTypeId type = 0u; type < program.edgeTypes.size(); ++type)
  {
    forward += program.edgeTypes[type].usedForward ? 1u : 0u;
    backward += program.edgeTypes[type].usedBackward ? 1u : 0u;
  }

  checkEqual(store.partitionCount(), program.edgeTypes.size() + backward, "one origin partition per type, plus a target one where needed");

  check(store.partitionCount() < program.edgeTypes.size() * 2u, "strictly fewer than two partitions per edge type");

  std::cout << "    corpus: " << store.partitionCount() << " partitions for " << program.edgeTypes.size() << " edge types ("
            << backward << " need a second, target-ordered partition)\n";
  (void)forward;
}

} // namespace

int main()
{
  buildPreservesEveryEdge();
  pagesAreBoundedAndSorted();
  keyOrderSelectsTheKey();
  insertSplitsAndKeepsIdsStable();
  insertAndRemoveRoundTrip();
  removingTheLowestEdgeMovesMinKey();
  removingSomethingAbsentIsFalse();
  storeBuildsOnlyTheDirectionsUsed();
  addEdgeReachesEveryPartition();
  vertexAllocatorIsMonotone();
  configuredFromTheCorpusProgramme();

  return report("GraphRewritePageStoreTests");
}
