#pragma once

// Durability: a checkpoint, not a journal.
//
// Committing a step flushes every page, writes the manifest to a temporary file
// and renames it over the old one. `rename` is atomic, so a crash leaves either
// the previous consistent generation or the new one, never a torn mix. Losing
// the last uncommitted step is acceptable; losing the graph is not.
//
// The directory lives in the manifest rather than in the segment files because
// it is small, it changes on every split, and it has to be consistent with the
// generation being committed.

#include "graphrewrite/runtime/CpuPageCache.hpp"
#include "graphrewrite/runtime/EdgePageStore.hpp"

#include <string>

namespace graphrewrite::runtime {

constexpr uint32_t manifestMagic = 0x4e414d47u; // "GMAN"
constexpr uint32_t manifestVersion = 1u;

struct ManifestPartition
{
  EdgeTypeId edgeType = 0u;
  uint32_t order = 0u; // KeyOrder
  uint32_t pageCount = 0u;
};

struct Manifest
{
  uint32_t version = manifestVersion;
  uint32_t generation = 0u;
  uint32_t pageEdges = runtime::pageEdges;
  uint32_t pagesPerSegment = 1024u;

  std::vector<ManifestPartition> partitions;
  std::vector<PageId> directoryOrder; // concatenated, partition by partition
  std::vector<uint32_t> minKey;
  std::vector<uint32_t> maxKey;
};

// Writes every page of the store to disk, then the manifest, atomically.
bool commitStore(const EdgePageStore &store, DiskStore &disk, uint32_t generation, DiagnosticSink &sink);

// Reads the manifest and rebuilds the store from the segment files.
bool openStore(DiskStore &disk, EdgePageStore &out, DiagnosticSink &sink, uint32_t *generation = nullptr);

std::string manifestPath(const DiskStore &disk);

} // namespace graphrewrite::runtime
