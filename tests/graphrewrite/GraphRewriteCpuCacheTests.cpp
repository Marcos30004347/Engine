// Phase 3.3: the CPU page cache, and its dirty path.

#include "TestHarness.hpp"
#include "graphrewrite/runtime/CpuPageCache.hpp"

#include <filesystem>
#include <unistd.h>

using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace graphrewrite::testing;

namespace {

struct Scratch
{
  std::string root;

  Scratch()
  {
    root = (std::filesystem::temp_directory_path() / ("graphrewrite-cache-" + std::to_string(::getpid()) + "-" + std::to_string(counter++)))
               .string();

    std::filesystem::remove_all(root);
  }

  ~Scratch()
  {
    std::error_code error;

    std::filesystem::remove_all(root, error);
  }

  static int counter;
};

int Scratch::counter = 0;

DiskStoreSettings diskSettings(const std::string &root)
{
  DiskStoreSettings settings;

  settings.root = root;
  settings.pagesPerSegment = 8u;
  settings.edgesPerPage = 4u;

  return settings;
}

CpuPageCacheSettings cacheOf(const uint32_t pages)
{
  CpuPageCacheSettings settings;

  settings.edgesPerPage = 4u;
  settings.byteBudget = static_cast<uint64_t>(pages) * sizeof(Edge) * 4u;

  return settings;
}

void seedDisk(DiskStore &disk, const uint32_t pages)
{
  DiagnosticSink sink;

  for (PageId id = 0u; id < pages; ++id)
  {
    DiskPageHeader header;

    header.minKey = id * 10u;
    header.maxKey = id * 10u + 1u;

    disk.writePage(0u, id, header, {{id * 10u, id}, {id * 10u + 1u, id}}, sink);
  }
}

void missThenHit()
{
  Scratch scratch;
  DiskStore disk(diskSettings(scratch.root));

  seedDisk(disk, 4u);

  CpuPageCache cache(disk, cacheOf(8u));
  DiagnosticSink sink;

  DiskPageHeader header;

  const std::vector<Edge> *first = cache.get(0u, 2u, header, sink);

  check(first != nullptr, "a cold page is fetched from disk");
  checkEqual(cache.stats().misses, uint64_t(1), "and counts as a miss");

  const std::vector<Edge> *second = cache.get(0u, 2u, header, sink);

  check(second != nullptr, "a warm page is served from memory");
  checkEqual(cache.stats().hits, uint64_t(1), "and counts as a hit");
  checkEqual(cache.stats().misses, uint64_t(1), "without a second read");
}

void dirtyPageSurvivesEviction()
{
  // The single most important behaviour in this class: an edit that is evicted
  // must come back, not be replaced by the stale on-disk copy.
  Scratch scratch;
  DiskStore disk(diskSettings(scratch.root));

  seedDisk(disk, 8u);

  CpuPageCache cache(disk, cacheOf(2u));
  DiagnosticSink sink;

  DiskPageHeader header;

  header.minKey = 999u;
  header.maxKey = 999u;

  cache.update(0u, 0u, header, {{999u, 7u}});

  check(cache.isDirty(0u, 0u), "the updated page is dirty");

  // Push it out by touching enough other pages.
  for (PageId id = 1u; id < 6u; ++id)
  {
    DiskPageHeader other;

    cache.get(0u, id, other, sink);
  }

  check(!cache.contains(0u, 0u), "the dirty page was evicted");
  check(cache.stats().writebacks >= 1u, "and written back on the way out");

  DiskPageHeader readHeader;

  const std::vector<Edge> *back = cache.get(0u, 0u, readHeader, sink);

  check(back != nullptr, "it fetches again");
  checkEqual(back->size(), size_t(1), "with the UPDATED contents");
  checkEqual((*back)[0].origin, 999u, "not the stale on-disk original");
  checkEqual(readHeader.minKey, 999u, "and the updated header");
}

void cleanPagesAreDroppedWithoutWriting()
{
  Scratch scratch;
  DiskStore disk(diskSettings(scratch.root));

  seedDisk(disk, 8u);

  CpuPageCache cache(disk, cacheOf(2u));
  DiagnosticSink sink;

  for (PageId id = 0u; id < 6u; ++id)
  {
    DiskPageHeader header;

    cache.get(0u, id, header, sink);
  }

  check(cache.stats().evictions >= 3u, "clean pages were evicted");
  checkEqual(cache.stats().writebacks, uint64_t(0), "and none of them cost a write - disk already had them");
}

void budgetIsRespected()
{
  Scratch scratch;
  DiskStore disk(diskSettings(scratch.root));

  seedDisk(disk, 16u);

  CpuPageCache cache(disk, cacheOf(3u));
  DiagnosticSink sink;

  for (PageId id = 0u; id < 16u; ++id)
  {
    DiskPageHeader header;

    cache.get(0u, id, header, sink);

    check(cache.entryCount() <= cache.capacityInPages(), "the cache never exceeds its budget");
  }

  checkEqual(cache.capacityInPages(), 3u, "capacity derives from the byte budget");
}

void singlePageBudgetStillProgresses()
{
  Scratch scratch;
  DiskStore disk(diskSettings(scratch.root));

  seedDisk(disk, 8u);

  CpuPageCache cache(disk, cacheOf(1u));
  DiagnosticSink sink;

  for (PageId id = 0u; id < 8u; ++id)
  {
    DiskPageHeader header;

    const std::vector<Edge> *page = cache.get(0u, id, header, sink);

    check(page != nullptr, "a one-page cache still serves page " + std::to_string(id));
  }

  checkEqual(sink.count(), 0u, "and reports nothing");
}

void flushClearsDirtyState()
{
  Scratch scratch;
  DiskStore disk(diskSettings(scratch.root));

  seedDisk(disk, 4u);

  CpuPageCache cache(disk, cacheOf(8u));
  DiagnosticSink sink;

  DiskPageHeader header;

  header.minKey = 1u;
  header.maxKey = 1u;

  cache.update(0u, 1u, header, {{1u, 1u}});
  cache.update(0u, 2u, header, {{2u, 2u}});

  check(cache.flush(sink), "flush succeeds");
  check(!cache.isDirty(0u, 1u), "page 1 is clean afterwards");
  check(!cache.isDirty(0u, 2u), "page 2 is clean afterwards");

  // And the flush really reached disk.
  DiskPageHeader readHeader;
  std::vector<Edge> edges;

  check(disk.readPage(0u, 1u, readHeader, edges, sink), "the page reads from disk");
  checkEqual(edges.size(), size_t(1), "with the flushed contents");
}

void updateOfAnAbsentPageInserts()
{
  Scratch scratch;
  DiskStore disk(diskSettings(scratch.root));

  CpuPageCache cache(disk, cacheOf(4u));
  DiagnosticSink sink;

  DiskPageHeader header;

  // Nothing on disk at all: a brand new page created by a rewrite.
  cache.update(0u, 5u, header, {{3u, 4u}});

  check(cache.contains(0u, 5u), "a page that was never on disk can be created in the cache");

  DiskPageHeader readHeader;

  const std::vector<Edge> *page = cache.get(0u, 5u, readHeader, sink);

  check(page != nullptr, "and read back");
  checkEqual(readHeader.count, 1u, "with its count set from the contents");
  checkEqual(sink.count(), 0u, "without touching disk");

  check(cache.flush(sink), "and it flushes to disk");
  check(disk.readPage(0u, 5u, readHeader, *const_cast<std::vector<Edge> *>(page), sink), "creating the segment as needed");
}

void beladyEvictsTheFurthestType()
{
  Scratch scratch;
  DiskStore disk(diskSettings(scratch.root));

  seedDisk(disk, 4u);

  CpuPageCache cache(disk, cacheOf(2u));
  DiagnosticSink sink;

  // Three partitions, one edge type each.
  cache.configure({0u, 1u, 2u});

  for (PageId id = 0u; id < 2u; ++id)
  {
    DiskPageHeader header;

    disk.writePage(1u, id, header, {{id, id}}, sink);
    disk.writePage(2u, id, header, {{id, id}}, sink);
  }

  compiler::CompiledProgram program;

  program.edgeTypes.intern("? a ?", 2u);
  program.edgeTypes.intern("? b ?", 2u);
  program.edgeTypes.intern("? c ?", 2u);

  const auto query = [](const uint32_t reg, const EdgeTypeId label) {
    compiler::Instruction instruction;

    instruction.op = compiler::Op::Where;
    instruction.reg = reg;
    instruction.label = label;
    instruction.rules = {0u};

    return instruction;
  };

  // type 0 next at 1, type 1 next at 5, type 2 never again.
  program.instructions.push_back(query(0u, 0u));
  program.instructions.push_back(query(1u, 0u));
  program.instructions.push_back(query(2u, 0u));
  program.instructions.push_back(query(3u, 0u));
  program.instructions.push_back(query(4u, 0u));
  program.instructions.push_back(query(5u, 1u));

  const compiler::UsageValues usage = compiler::computeUsageValues(program);

  cache.setEvictionOrder(&usage, 1u);

  DiskPageHeader header;

  cache.get(0u, 0u, header, sink); // partition 0 -> type 0, needed at 1
  cache.get(2u, 0u, header, sink); // partition 2 -> type 2, never needed again

  // Third fetch forces an eviction: the never-needed type must go.
  cache.get(1u, 0u, header, sink);

  check(cache.contains(0u, 0u), "the soon-needed page stays resident");
  check(!cache.contains(2u, 0u), "the never-needed page is the one evicted");
}

} // namespace

int main()
{
  missThenHit();
  dirtyPageSurvivesEviction();
  cleanPagesAreDroppedWithoutWriting();
  budgetIsRespected();
  singlePageBudgetStillProgresses();
  flushClearsDirtyState();
  updateOfAnAbsentPageInserts();
  beladyEvictsTheFurthestType();

  return report("GraphRewriteCpuCacheTests");
}
