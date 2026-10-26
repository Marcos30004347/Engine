// Phase 3.13: durability and restart.

#include "TestHarness.hpp"
#include "graphrewrite/runtime/Manifest.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
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
    root = (std::filesystem::temp_directory_path() / ("graphrewrite-dur-" + std::to_string(::getpid()) + "-" + std::to_string(counter++)))
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

DiskStoreSettings settingsFor(const std::string &root)
{
  DiskStoreSettings settings;

  settings.root = root;
  settings.pagesPerSegment = 8u;
  settings.edgesPerPage = pageEdges;

  return settings;
}

compiler::EdgeTypeTable typesFor()
{
  compiler::EdgeTypeTable types;

  const compiler::EdgeTypeId a = types.intern("? a ?", 2u);
  const compiler::EdgeTypeId b = types.intern("? b ?", 2u);

  types.mutableType(a).usedForward = true;
  types.mutableType(b).usedForward = true;
  types.mutableType(b).usedBackward = true;

  return types;
}

std::vector<std::tuple<uint32_t, EdgeTypeId, uint32_t>> snapshotOf(const EdgePageStore &store)
{
  std::vector<std::tuple<uint32_t, EdgeTypeId, uint32_t>> out;

  for (uint32_t index = 0u; index < store.partitionCount(); ++index)
  {
    const Partition &partition = store.partitionAt(index);

    if (partition.order() != KeyOrder::ByOrigin)
    {
      continue;
    }

    for (const Edge &edge : partition.snapshot())
    {
      out.emplace_back(edge.origin, partition.edgeType(), edge.target);
    }
  }

  std::sort(out.begin(), out.end());

  return out;
}

void roundTripThroughDisk()
{
  Scratch scratch;

  const compiler::EdgeTypeTable types = typesFor();

  EdgePageStore original;

  original.configure(types);

  std::mt19937 rng(4242u);

  for (uint32_t i = 0u; i < 2000u; ++i)
  {
    original.addEdge(static_cast<EdgeTypeId>(rng() % 2u), Edge{rng() % 300u, rng() % 300u});
  }

  const auto before = snapshotOf(original);

  DiskStore disk(settingsFor(scratch.root));
  DiagnosticSink sink;

  check(commitStore(original, disk, 3u, sink), "the store commits");
  checkEqual(sink.count(), 0u, "without diagnostics");
  check(std::filesystem::exists(manifestPath(disk)), "and a manifest exists");
  check(!std::filesystem::exists(manifestPath(disk) + ".tmp"), "with no temporary left behind");

  // Reopen through a completely fresh store and disk handle.
  DiskStore reopened(settingsFor(scratch.root));
  EdgePageStore restored;
  DiagnosticSink readSink;

  uint32_t generation = 0u;

  check(openStore(reopened, restored, readSink, &generation), "the store reopens");
  checkEqual(readSink.count(), 0u, "without diagnostics");
  checkEqual(generation, 3u, "and remembers its generation");

  const auto after = snapshotOf(restored);

  checkEqual(after.size(), before.size(), "the same number of edges came back");
  check(after == before, "and exactly the same edges");

  checkEqual(restored.partitionCount(), original.partitionCount(), "with the same partitions");

  std::string problem;

  for (uint32_t index = 0u; index < restored.partitionCount(); ++index)
  {
    check(restored.partitionAt(index).checkInvariants(problem), "restored partition " + std::to_string(index) + " is well formed: " + problem);
    checkEqual(restored.partitionAt(index).pageCount(), original.partitionAt(index).pageCount(),
               "partition " + std::to_string(index) + " has the same page count");
  }
}

void restoredStoreIsStillUsable()
{
  Scratch scratch;

  EdgePageStore original;

  original.configure(typesFor());

  for (uint32_t i = 0u; i < 500u; ++i)
  {
    original.addEdge(0u, Edge{i % 50u, i});
  }

  DiskStore disk(settingsFor(scratch.root));
  DiagnosticSink sink;

  commitStore(original, disk, 1u, sink);

  DiskStore reopened(settingsFor(scratch.root));
  EdgePageStore restored;

  openStore(reopened, restored, sink, nullptr);

  // Searching, inserting and removing must all still work on the restored
  // directory - not just reading it back.
  const PageRun run = restored.partitionAt(0u).rangeFor(7u);

  check(!run.empty(), "a key search works on the restored directory");

  restored.addEdge(0u, Edge{7u, 9999u});

  check(restored.partitionAt(0u).rangeFor(7u).size() >= run.size(), "an insert still lands");

  check(restored.removeEdge(0u, Edge{7u, 9999u}), "and can be removed again");

  std::string problem;

  check(restored.partitionAt(0u).checkInvariants(problem), "invariants hold after that: " + problem);
}

void aTruncatedManifestLeavesThePreviousGeneration()
{
  Scratch scratch;

  EdgePageStore first;

  first.configure(typesFor());

  for (uint32_t i = 0u; i < 100u; ++i)
  {
    first.addEdge(0u, Edge{i, i + 1u});
  }

  DiskStore disk(settingsFor(scratch.root));
  DiagnosticSink sink;

  commitStore(first, disk, 1u, sink);

  const auto committed = snapshotOf(first);

  // Simulate a crash mid-write: a temporary file exists but the rename never
  // happened. The previous generation must still be exactly what loads.
  {
    std::ofstream partial(manifestPath(disk) + ".tmp", std::ios::binary);

    partial << "garbage";
  }

  EdgePageStore restored;
  DiagnosticSink readSink;

  uint32_t generation = 0u;

  check(openStore(disk, restored, readSink, &generation), "the previous generation still opens");
  checkEqual(generation, 1u, "and it is the committed one");
  check(snapshotOf(restored) == committed, "with its edges intact");
}

void aCorruptManifestIsRejected()
{
  Scratch scratch;

  EdgePageStore store;

  store.configure(typesFor());
  store.addEdge(0u, Edge{1u, 2u});

  DiskStore disk(settingsFor(scratch.root));
  DiagnosticSink sink;

  commitStore(store, disk, 1u, sink);

  {
    std::fstream file(manifestPath(disk), std::ios::binary | std::ios::in | std::ios::out);

    const uint32_t garbage = 0xdeadbeefu;

    file.seekp(0);
    file.write(reinterpret_cast<const char *>(&garbage), sizeof(garbage));
  }

  EdgePageStore restored;
  DiagnosticSink readSink;

  check(!openStore(disk, restored, readSink, nullptr), "a manifest with the wrong magic is rejected");
  check(readSink.hasErrors(), "with a diagnostic rather than plausible garbage");
}

void aGeometryMismatchIsRejected()
{
  Scratch scratch;

  EdgePageStore store;

  store.configure(typesFor());
  store.addEdge(0u, Edge{1u, 2u});

  DiskStore disk(settingsFor(scratch.root));
  DiagnosticSink sink;

  commitStore(store, disk, 1u, sink);

  DiskStoreSettings other = settingsFor(scratch.root);

  other.pagesPerSegment = 16u; // a different geometry entirely

  DiskStore mismatched(other);
  EdgePageStore restored;
  DiagnosticSink readSink;

  check(!openStore(mismatched, restored, readSink, nullptr), "a geometry mismatch is rejected");
  check(readSink[0].message.find("pagesPerSegment") != std::string::npos, "naming the geometry that disagrees");
}

void missingManifestIsReported()
{
  Scratch scratch;

  DiskStore disk(settingsFor(scratch.root));
  EdgePageStore restored;
  DiagnosticSink sink;

  check(!openStore(disk, restored, sink, nullptr), "opening a store with no manifest fails");
  check(sink.hasErrors(), "with a diagnostic rather than an empty graph");
}

void successiveGenerationsOverwriteCleanly()
{
  Scratch scratch;

  DiskStore disk(settingsFor(scratch.root));
  DiagnosticSink sink;

  EdgePageStore store;

  store.configure(typesFor());

  for (uint32_t generation = 1u; generation <= 4u; ++generation)
  {
    store.addEdge(0u, Edge{generation, generation * 10u});

    check(commitStore(store, disk, generation, sink), "generation " + std::to_string(generation) + " commits");

    EdgePageStore restored;
    DiagnosticSink readSink;

    uint32_t restoredGeneration = 0u;

    check(openStore(disk, restored, readSink, &restoredGeneration), "and reopens");
    checkEqual(restoredGeneration, generation, "at the right generation");
    check(snapshotOf(restored) == snapshotOf(store), "with the edges of that generation");
  }
}

} // namespace

int main()
{
  roundTripThroughDisk();
  restoredStoreIsStillUsable();
  aTruncatedManifestLeavesThePreviousGeneration();
  aCorruptManifestIsRejected();
  aGeometryMismatchIsRejected();
  missingManifestIsReported();
  successiveGenerationsOverwriteCleanly();

  return report("GraphRewriteDurabilityTests");
}
