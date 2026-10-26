#pragma once

// The durable home of a page.
//
// Memory is a cache; this is the owner. One directory per partition, holding
// fixed-size segment files.

#include "graphrewrite/frontend/Diagnostics.hpp"
#include "graphrewrite/runtime/SegmentFormat.hpp"

#include <string>
#include <vector>

namespace graphrewrite::runtime {

struct DiskStoreSettings
{
  std::string root;
  uint32_t pagesPerSegment = 1024u;
  uint32_t edgesPerPage = pageEdges;
};

class DiskStore
{
public:
  explicit DiskStore(DiskStoreSettings settings);

  const DiskStoreSettings &settings() const;

  bool readPage(uint32_t partition, PageId id, DiskPageHeader &header, std::vector<Edge> &edges, DiagnosticSink &sink);

  bool writePage(uint32_t partition, PageId id, const DiskPageHeader &header, const std::vector<Edge> &edges, DiagnosticSink &sink);

  // Updates only the 16-byte header record, leaving the edge block alone.
  bool writePageHeader(uint32_t partition, PageId id, const DiskPageHeader &header, DiagnosticSink &sink);

  bool freePage(uint32_t partition, PageId id, DiagnosticSink &sink);

  // Lowest PageId whose slot is free, appending a segment when none is.
  PageId allocatePage(uint32_t partition, DiagnosticSink &sink);

  uint32_t segmentCount(uint32_t partition) const;

  std::string segmentPath(uint32_t partition, uint32_t segment) const;

  uint64_t readCount() const;
  uint64_t writeCount() const;

private:
  bool ensureSegment(uint32_t partition, uint32_t segment, DiagnosticSink &sink);
  SegmentLayout layout() const;

  DiskStoreSettings storeSettings;
  uint64_t reads = 0u;
  uint64_t writes = 0u;
};

} // namespace graphrewrite::runtime
