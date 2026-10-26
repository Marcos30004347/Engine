// Phase 3.4: the directory search.
//
// Exhaustive rather than sampled: for every key in range, the returned run must
// contain EVERY page that holds that key. The failure mode this guards is
// silent - a search that misses a page simply returns fewer matches.

#include "TestHarness.hpp"
#include "graphrewrite/runtime/EdgePageStore.hpp"
#include "graphrewrite/runtime/PartitionDirectory.hpp"

#include <algorithm>
#include <random>
#include <set>

using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace graphrewrite::testing;

namespace {

// Brute force: every page whose key range covers `key`.
std::set<uint32_t> pagesHolding(const Partition &partition, const uint32_t key)
{
  std::set<uint32_t> out;

  for (uint32_t position = 0u; position < partition.pageCount(); ++position)
  {
    for (const Edge &edge : partition.pageAt(position))
    {
      if (keyOf(edge, partition.order()) == key)
      {
        out.insert(position);

        break;
      }
    }
  }

  return out;
}

void emptyDirectory()
{
  PartitionDirectory directory;

  const PageRun run = directoryRange(directory, 5u);

  check(run.empty(), "an empty directory returns an empty run");
  checkEqual(run.size(), 0u, "of size zero");
}

void singlePage()
{
  PartitionDirectory directory;

  directory.order = {0u};
  directory.minKey = {10u};
  directory.maxKey = {20u};

  check(directoryRange(directory, 9u).empty(), "a key below the range finds nothing");
  check(!directoryRange(directory, 10u).empty(), "the low bound is included");
  check(!directoryRange(directory, 15u).empty(), "an interior key is found");
  check(!directoryRange(directory, 20u).empty(), "the high bound is included");
  check(directoryRange(directory, 21u).empty(), "a key above the range finds nothing");

  // At key 0 the search takes the `mid == 0` path, which is where an unsigned
  // bound underflows.
  check(directoryRange(directory, 0u).empty(), "key zero is handled without underflow");
}

void keyInAGapFindsNothing()
{
  PartitionDirectory directory;

  directory.order = {0u, 1u};
  directory.minKey = {10u, 30u};
  directory.maxKey = {20u, 40u};

  check(directoryRange(directory, 25u).empty(), "a key between two pages finds nothing");
}

void straddlingKeySpansAnEntireRun()
{
  // A vertex with more edges than fit in a page occupies several adjacent pages,
  // which therefore SHARE a key. This is the case a single-page search gets
  // wrong.
  std::vector<Edge> edges;

  for (uint32_t i = 0u; i < pageEdges * 3u; ++i)
  {
    edges.push_back(Edge{42u, i}); // one origin, many targets
  }

  Partition partition(0u, KeyOrder::ByOrigin, 0u);

  partition.build(edges);

  checkEqual(partition.pageCount(), 3u, "the high-degree vertex needs three pages");

  const PageRun run = partition.rangeFor(42u);

  check(!run.empty(), "and the search finds it");
  checkEqual(run.first, 0u, "the run starts at the first page");
  checkEqual(run.last, 2u, "and ends at the last");
  checkEqual(run.size(), 3u, "covering all three - a single-page answer would drop two thirds of the edges");
}

void exhaustiveOverRandomMultisets()
{
  std::mt19937 rng(4242u);

  uint32_t checkedKeys = 0u;
  uint32_t mismatches = 0u;
  uint32_t nonEmpty = 0u;

  for (uint32_t trial = 0u; trial < 60u; ++trial)
  {
    const uint32_t keySpace = 3u + rng() % 40u;
    const uint32_t count = rng() % 1500u;

    std::vector<Edge> edges;

    for (uint32_t i = 0u; i < count; ++i)
    {
      edges.push_back(Edge{rng() % keySpace, rng()});
    }

    const KeyOrder order = (trial % 2u) == 0u ? KeyOrder::ByOrigin : KeyOrder::ByTarget;

    Partition partition(0u, order, 0u);

    partition.build(edges);

    for (uint32_t key = 0u; key <= keySpace + 2u; ++key)
    {
      const std::set<uint32_t> want = pagesHolding(partition, key);
      const PageRun run = partition.rangeFor(key);

      ++checkedKeys;

      if (!want.empty())
      {
        ++nonEmpty;
      }

      // Every page that actually holds the key must be inside the run.
      for (const uint32_t position : want)
      {
        if (run.empty() || position < run.first || position > run.last)
        {
          ++mismatches;

          break;
        }
      }

      // And the run must stay in bounds.
      if (!run.empty() && run.last >= partition.pageCount())
      {
        ++mismatches;
      }
    }
  }

  check(checkedKeys > 1000u, "enough keys were checked");
  check(nonEmpty > 200u, "and enough of them actually had pages");
  checkEqual(mismatches, 0u, "the run contains every page holding the key, on every trial");
}

void searchStaysInBoundsOnKeysFarOutside()
{
  std::vector<Edge> edges;

  for (uint32_t i = 100u; i < 200u; ++i)
  {
    edges.push_back(Edge{i, i});
  }

  Partition partition(0u, KeyOrder::ByOrigin, 0u);

  partition.build(edges);

  for (const uint32_t key : {0u, 1u, 99u, 200u, 100000u, 4294967295u})
  {
    const PageRun run = partition.rangeFor(key);

    check(run.empty() || run.last < partition.pageCount(), "key " + std::to_string(key) + " stays in bounds");
  }
}

void runIsStillCorrectAfterSplits()
{
  std::vector<Edge> edges;

  for (uint32_t i = 0u; i < pageEdges; ++i)
  {
    edges.push_back(Edge{i, i});
  }

  Partition partition(0u, KeyOrder::ByOrigin, 0u);

  partition.build(edges);

  std::mt19937 rng(31u);

  for (uint32_t i = 0u; i < 500u; ++i)
  {
    partition.insert(Edge{rng() % pageEdges, rng()});
  }

  check(partition.pageCount() > 1u, "inserts caused splits");

  uint32_t mismatches = 0u;

  for (uint32_t key = 0u; key < pageEdges; ++key)
  {
    const std::set<uint32_t> want = pagesHolding(partition, key);
    const PageRun run = partition.rangeFor(key);

    for (const uint32_t position : want)
    {
      if (run.empty() || position < run.first || position > run.last)
      {
        ++mismatches;

        break;
      }
    }
  }

  checkEqual(mismatches, 0u, "the search is still exact after 500 inserts and their splits");
}

void runIsCorrectAfterRemovalsMoveRanges()
{
  std::vector<Edge> edges;

  for (uint32_t i = 0u; i < 50u; ++i)
  {
    edges.push_back(Edge{i, i});
  }

  Partition partition(0u, KeyOrder::ByOrigin, 0u);

  partition.build(edges);

  // Removing the lowest edge moves minKey. A directory that does not follow
  // stops returning the page for keys it still holds.
  partition.remove(Edge{0u, 0u});
  partition.remove(Edge{1u, 1u});

  check(partition.rangeFor(0u).empty(), "the removed key is no longer found");
  check(!partition.rangeFor(2u).empty(), "but keys the page still holds are");

  uint32_t found = 0u;

  for (uint32_t key = 2u; key < 50u; ++key)
  {
    if (!partition.rangeFor(key).empty())
    {
      ++found;
    }
  }

  checkEqual(found, 48u, "every surviving key is still reachable");
}

} // namespace

int main()
{
  emptyDirectory();
  singlePage();
  keyInAGapFindsNothing();
  straddlingKeySpansAnEntireRun();
  exhaustiveOverRandomMultisets();
  searchStaysInBoundsOnKeysFarOutside();
  runIsStillCorrectAfterSplits();
  runIsCorrectAfterRemovalsMoveRanges();

  return report("GraphRewriteDirectoryTests");
}
