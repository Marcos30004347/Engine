// Phase 3.2: the on-disk segment format.

#include "TestHarness.hpp"
#include "graphrewrite/runtime/DiskStore.hpp"

#include <filesystem>
#include <unistd.h>
#include <fstream>
#include <random>

using namespace graphrewrite;
using namespace graphrewrite::runtime;
using namespace graphrewrite::testing;

namespace {

struct Scratch
{
  std::string root;

  Scratch()
  {
    root = (std::filesystem::temp_directory_path() / ("graphrewrite-disk-" + std::to_string(::getpid()) + "-" + std::to_string(counter++)))
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

DiskStoreSettings smallSettings(const std::string &root)
{
  DiskStoreSettings settings;

  // Small on purpose: forces many segment files, so the segment/slot arithmetic
  // is exercised rather than taken on trust.
  settings.root = root;
  settings.pagesPerSegment = 4u;
  settings.edgesPerPage = 8u;

  return settings;
}

std::vector<Edge> makeEdges(const uint32_t count, const uint32_t seed)
{
  std::mt19937 rng(seed);

  std::vector<Edge> edges;

  for (uint32_t i = 0u; i < count; ++i)
  {
    edges.push_back(Edge{rng() % 1000u, rng() % 1000u});
  }

  return edges;
}

void writeThenReadIsIdentical()
{
  Scratch scratch;
  DiskStore store(smallSettings(scratch.root));
  DiagnosticSink sink;

  const std::vector<Edge> edges = makeEdges(8u, 1u);

  DiskPageHeader header;

  header.minKey = edges.front().origin;
  header.maxKey = edges.back().origin;

  check(store.writePage(0u, 3u, header, edges, sink), "page writes");

  DiskPageHeader readHeader;
  std::vector<Edge> readEdges;

  check(store.readPage(0u, 3u, readHeader, readEdges, sink), "page reads back");
  checkEqual(sink.count(), 0u, "with no diagnostics");

  checkEqual(readHeader.pageId, PageId(3u), "page id survives");
  checkEqual(readHeader.count, 8u, "count survives");
  checkEqual(readHeader.minKey, header.minKey, "minKey survives");
  checkEqual(readHeader.maxKey, header.maxKey, "maxKey survives");
  check(readEdges == edges, "every edge survives byte-identically");
}

void emptyAndPartialPages()
{
  Scratch scratch;
  DiskStore store(smallSettings(scratch.root));
  DiagnosticSink sink;

  DiskPageHeader header;

  check(store.writePage(0u, 0u, header, {}, sink), "an empty page writes");

  DiskPageHeader readHeader;
  std::vector<Edge> readEdges;

  check(store.readPage(0u, 0u, readHeader, readEdges, sink), "an empty page reads back");
  checkEqual(readEdges.size(), size_t(0), "and is empty");

  const std::vector<Edge> partial = makeEdges(3u, 2u);

  check(store.writePage(0u, 1u, header, partial, sink), "a partial page writes");
  check(store.readPage(0u, 1u, readHeader, readEdges, sink), "a partial page reads back");
  check(readEdges == partial, "with exactly its own edges and no stale tail");
}

void addressingSpansManySegments()
{
  Scratch scratch;
  DiskStore store(smallSettings(scratch.root));
  DiagnosticSink sink;

  // pagesPerSegment is 4, so these ids land across six different files.
  const std::vector<PageId> ids = {0u, 1u, 3u, 4u, 7u, 8u, 11u, 12u, 19u, 23u};

  for (const PageId id : ids)
  {
    const std::vector<Edge> edges = makeEdges(1u + (id % 8u), id + 100u);

    DiskPageHeader header;

    header.minKey = id;
    header.maxKey = id * 2u;

    check(store.writePage(0u, id, header, edges, sink), "page " + std::to_string(id) + " writes");
  }

  for (const PageId id : ids)
  {
    const std::vector<Edge> expected = makeEdges(1u + (id % 8u), id + 100u);

    DiskPageHeader header;
    std::vector<Edge> edges;

    check(store.readPage(0u, id, header, edges, sink), "page " + std::to_string(id) + " reads back");
    checkEqual(header.minKey, id, "page " + std::to_string(id) + " header survives");
    check(edges == expected, "page " + std::to_string(id) + " contents survive");
  }

  checkEqual(sink.count(), 0u, "no diagnostics across all segments");
  checkEqual(store.segmentCount(0u), 6u, "ids 0..23 at 4 per segment occupy six segment files");
}

void partitionsAreSeparateDirectories()
{
  Scratch scratch;
  DiskStore store(smallSettings(scratch.root));
  DiagnosticSink sink;

  const std::vector<Edge> a = {{1u, 2u}};
  const std::vector<Edge> b = {{9u, 9u}};

  DiskPageHeader header;

  store.writePage(0u, 0u, header, a, sink);
  store.writePage(1u, 0u, header, b, sink);

  DiskPageHeader readHeader;
  std::vector<Edge> edges;

  store.readPage(0u, 0u, readHeader, edges, sink);
  check(edges == a, "partition 0 keeps its own page 0");

  store.readPage(1u, 0u, readHeader, edges, sink);
  check(edges == b, "partition 1 keeps a different page 0");

  check(std::filesystem::exists(scratch.root + "/part0000/seg0000.gre"), "partition 0 has its own directory");
  check(std::filesystem::exists(scratch.root + "/part0001/seg0000.gre"), "partition 1 has its own directory");
}

void headerOnlyUpdateLeavesEdgesAlone()
{
  Scratch scratch;
  DiskStore store(smallSettings(scratch.root));
  DiagnosticSink sink;

  const std::vector<Edge> edges = makeEdges(6u, 3u);

  DiskPageHeader header;

  header.minKey = 5u;
  header.maxKey = 50u;

  store.writePage(0u, 2u, header, edges, sink);

  DiskPageHeader update = header;

  update.count = 4u; // as if two edges were removed
  update.maxKey = 40u;

  check(store.writePageHeader(0u, 2u, update, sink), "a header-only update writes");

  DiskPageHeader readHeader;
  std::vector<Edge> readEdges;

  check(store.readPage(0u, 2u, readHeader, readEdges, sink), "the page still reads");
  checkEqual(readHeader.count, 4u, "the new count took effect");
  checkEqual(readHeader.maxKey, 40u, "and the new range");
  checkEqual(readEdges.size(), size_t(4), "the read honours the count");
  check(readEdges[0] == edges[0], "the untouched edge block is still there");
}

void freedSlotsAreReusedBeforeGrowing()
{
  Scratch scratch;
  DiskStore store(smallSettings(scratch.root));
  DiagnosticSink sink;

  DiskPageHeader header;

  // Fill the first segment.
  for (PageId id = 0u; id < 4u; ++id)
  {
    store.writePage(0u, id, header, {{id, id}}, sink);
  }

  checkEqual(store.allocatePage(0u, sink), PageId(4u), "a full segment pushes allocation into the next one");

  check(store.freePage(0u, 2u, sink), "page 2 is freed");

  checkEqual(store.allocatePage(0u, sink), PageId(2u), "the freed slot is reused before a new segment is appended");

  DiskPageHeader readHeader;
  std::vector<Edge> edges;

  check(!store.readPage(0u, 2u, readHeader, edges, sink), "a freed slot cannot be read");
  check(sink.hasErrors(), "and says so");
}

void missingSegmentIsReported()
{
  Scratch scratch;
  DiskStore store(smallSettings(scratch.root));
  DiagnosticSink sink;

  DiskPageHeader header;
  std::vector<Edge> edges;

  check(!store.readPage(0u, 99u, header, edges, sink), "reading a page with no segment fails");
  check(sink.hasErrors(), "with a diagnostic rather than a crash");
}

void corruptMagicIsRejected()
{
  Scratch scratch;
  DiskStore store(smallSettings(scratch.root));
  DiagnosticSink sink;

  DiskPageHeader header;

  store.writePage(0u, 0u, header, {{1u, 1u}}, sink);

  {
    std::fstream file(store.segmentPath(0u, 0u), std::ios::binary | std::ios::in | std::ios::out);

    const uint32_t garbage = 0xdeadbeefu;

    file.seekp(0);
    file.write(reinterpret_cast<const char *>(&garbage), sizeof(garbage));
  }

  DiagnosticSink readSink;
  std::vector<Edge> edges;

  check(!store.readPage(0u, 0u, header, edges, readSink), "a segment with the wrong magic is rejected");
  check(readSink.hasErrors(), "with a diagnostic");
}

void geometryMismatchIsRejected()
{
  // A store written with a different page geometry cannot be addressed with this
  // one - the offsets would land in the middle of other pages and read garbage
  // that looks entirely plausible.
  Scratch scratch;
  DiagnosticSink sink;

  {
    DiskStore store(smallSettings(scratch.root));

    DiskPageHeader header;

    store.writePage(0u, 0u, header, {{1u, 1u}}, sink);
  }

  DiskStoreSettings other = smallSettings(scratch.root);

  other.edgesPerPage = 16u;

  DiskStore mismatched(other);

  DiskPageHeader header;
  std::vector<Edge> edges;
  DiagnosticSink readSink;

  check(!mismatched.readPage(0u, 0u, header, edges, readSink), "a geometry mismatch is rejected");
  check(readSink[0].message.find("pageEdges") != std::string::npos, "and the diagnostic names the geometry");
}

void oversizedPageIsRejected()
{
  Scratch scratch;
  DiskStore store(smallSettings(scratch.root));
  DiagnosticSink sink;

  DiskPageHeader header;

  check(!store.writePage(0u, 0u, header, makeEdges(9u, 4u), sink), "a page over capacity is rejected");
  check(sink.hasErrors(), "with a diagnostic");
}

void realGeometryRoundTrips()
{
  // The shipping geometry: 256 edges per page, 1024 pages per segment.
  Scratch scratch;

  DiskStoreSettings settings;

  settings.root = scratch.root;

  DiskStore store(settings);
  DiagnosticSink sink;

  const std::vector<Edge> edges = makeEdges(pageEdges, 11u);

  DiskPageHeader header;

  check(store.writePage(0u, 1023u, header, edges, sink), "the last slot of a segment writes");
  check(store.writePage(0u, 1024u, header, edges, sink), "the first slot of the next segment writes");

  DiskPageHeader readHeader;
  std::vector<Edge> readEdges;

  check(store.readPage(0u, 1023u, readHeader, readEdges, sink), "and reads back");
  check(readEdges == edges, "with a full 2 KiB page intact");

  checkEqual(store.segmentCount(0u), 2u, "two segments");

  const uint64_t expected = static_cast<uint64_t>(std::filesystem::file_size(store.segmentPath(0u, 0u)));

  SegmentLayout layout;

  layout.pagesPerSegment = settings.pagesPerSegment;
  layout.edgesPerPage = settings.edgesPerPage;

  checkEqual(expected, static_cast<uint64_t>(layout.totalSize()), "segment file is exactly the computed size");
}

} // namespace

int main()
{
  writeThenReadIsIdentical();
  emptyAndPartialPages();
  addressingSpansManySegments();
  partitionsAreSeparateDirectories();
  headerOnlyUpdateLeavesEdgesAlone();
  freedSlotsAreReusedBeforeGrowing();
  missingSegmentIsReported();
  corruptMagicIsRejected();
  geometryMismatchIsRejected();
  oversizedPageIsRejected();
  realGeometryRoundTrips();

  return report("GraphRewriteDiskStoreTests");
}
