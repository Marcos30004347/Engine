#include "graphrewrite/runtime/DiskStore.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace graphrewrite::runtime {
namespace {

std::string partitionDirectory(const std::string &root, const uint32_t partition)
{
  char name[32];

  std::snprintf(name, sizeof(name), "part%04u", partition);

  return root + "/" + name;
}

void reportIo(DiagnosticSink &sink, const std::string &what, const std::string &path)
{
  SourceSpan span;

  sink.error(span, what + ": " + path);
}

} // namespace

DiskStore::DiskStore(DiskStoreSettings settings) : storeSettings(std::move(settings))
{
  std::error_code error;

  std::filesystem::create_directories(storeSettings.root, error);
}

const DiskStoreSettings &DiskStore::settings() const
{
  return storeSettings;
}

SegmentLayout DiskStore::layout() const
{
  SegmentLayout out;

  out.pagesPerSegment = storeSettings.pagesPerSegment;
  out.edgesPerPage = storeSettings.edgesPerPage;

  return out;
}

std::string DiskStore::segmentPath(const uint32_t partition, const uint32_t segment) const
{
  char name[32];

  std::snprintf(name, sizeof(name), "/seg%04u.gre", segment);

  return partitionDirectory(storeSettings.root, partition) + name;
}

uint32_t DiskStore::segmentCount(const uint32_t partition) const
{
  uint32_t count = 0u;

  while (std::filesystem::exists(segmentPath(partition, count)))
  {
    ++count;
  }

  return count;
}

bool DiskStore::ensureSegment(const uint32_t partition, const uint32_t segment, DiagnosticSink &sink)
{
  const std::string path = segmentPath(partition, segment);

  if (std::filesystem::exists(path))
  {
    return true;
  }

  std::error_code error;

  std::filesystem::create_directories(partitionDirectory(storeSettings.root, partition), error);

  std::ofstream file(path, std::ios::binary | std::ios::trunc);

  if (!file)
  {
    reportIo(sink, "cannot create segment", path);

    return false;
  }

  const SegmentLayout segmentLayout = layout();

  // Zero-fill, then stamp the header and mark every slot free.
  const std::vector<char> zeros(segmentLayout.totalSize(), 0);

  file.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));

  SegmentHeader header;

  header.partition = partition;
  header.segmentIndex = segment;
  header.pagesPerSegment = storeSettings.pagesPerSegment;
  header.pageEdges = storeSettings.edgesPerPage;

  file.seekp(0);
  file.write(reinterpret_cast<const char *>(&header), sizeof(header));

  DiskPageHeader freeHeader;

  for (uint32_t slot = 0u; slot < storeSettings.pagesPerSegment; ++slot)
  {
    file.seekp(segmentLayout.headerOffset(slot));
    file.write(reinterpret_cast<const char *>(&freeHeader), sizeof(freeHeader));
  }

  return true;
}

bool DiskStore::writePage(const uint32_t partition, const PageId id, const DiskPageHeader &header, const std::vector<Edge> &edges,
                          DiagnosticSink &sink)
{
  const uint32_t segment = id / storeSettings.pagesPerSegment;
  const uint32_t slot = id % storeSettings.pagesPerSegment;

  if (edges.size() > storeSettings.edgesPerPage)
  {
    SourceSpan span;

    sink.error(span, "page " + std::to_string(id) + " holds " + std::to_string(edges.size()) + " edges, over the page capacity");

    return false;
  }

  if (!ensureSegment(partition, segment, sink))
  {
    return false;
  }

  const std::string path = segmentPath(partition, segment);

  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);

  if (!file)
  {
    reportIo(sink, "cannot open segment for writing", path);

    return false;
  }

  const SegmentLayout segmentLayout = layout();

  DiskPageHeader stored = header;

  stored.pageId = id;
  stored.count = static_cast<uint32_t>(edges.size());

  file.seekp(segmentLayout.headerOffset(slot));
  file.write(reinterpret_cast<const char *>(&stored), sizeof(stored));

  file.seekp(segmentLayout.edgesOffset(slot));

  if (!edges.empty())
  {
    file.write(reinterpret_cast<const char *>(edges.data()), static_cast<std::streamsize>(sizeof(Edge) * edges.size()));
  }

  ++writes;

  return file.good();
}

bool DiskStore::writePageHeader(const uint32_t partition, const PageId id, const DiskPageHeader &header, DiagnosticSink &sink)
{
  const uint32_t segment = id / storeSettings.pagesPerSegment;
  const uint32_t slot = id % storeSettings.pagesPerSegment;

  if (!ensureSegment(partition, segment, sink))
  {
    return false;
  }

  std::fstream file(segmentPath(partition, segment), std::ios::binary | std::ios::in | std::ios::out);

  if (!file)
  {
    reportIo(sink, "cannot open segment for writing", segmentPath(partition, segment));

    return false;
  }

  DiskPageHeader stored = header;

  stored.pageId = id;

  file.seekp(layout().headerOffset(slot));
  file.write(reinterpret_cast<const char *>(&stored), sizeof(stored));

  ++writes;

  return file.good();
}

bool DiskStore::readPage(const uint32_t partition, const PageId id, DiskPageHeader &header, std::vector<Edge> &edges, DiagnosticSink &sink)
{
  const uint32_t segment = id / storeSettings.pagesPerSegment;
  const uint32_t slot = id % storeSettings.pagesPerSegment;

  const std::string path = segmentPath(partition, segment);

  std::ifstream file(path, std::ios::binary);

  if (!file)
  {
    reportIo(sink, "no segment file for page " + std::to_string(id), path);

    return false;
  }

  SegmentHeader segmentHeader;

  file.read(reinterpret_cast<char *>(&segmentHeader), sizeof(segmentHeader));

  if (!file || segmentHeader.magic != segmentMagic)
  {
    reportIo(sink, "not a graph-rewrite segment file", path);

    return false;
  }

  if (segmentHeader.version != segmentVersion)
  {
    SourceSpan span;

    sink.error(span, "segment " + path + " is version " + std::to_string(segmentHeader.version) + ", expected " +
                         std::to_string(segmentVersion));

    return false;
  }

  // A store written with a different page geometry cannot be addressed with
  // this one: the offsets would silently land in the middle of other pages.
  if (segmentHeader.pagesPerSegment != storeSettings.pagesPerSegment || segmentHeader.pageEdges != storeSettings.edgesPerPage)
  {
    SourceSpan span;

    sink.error(span, "segment " + path + " was written with pagesPerSegment=" + std::to_string(segmentHeader.pagesPerSegment) +
                         " pageEdges=" + std::to_string(segmentHeader.pageEdges) + ", this store uses " +
                         std::to_string(storeSettings.pagesPerSegment) + " and " + std::to_string(storeSettings.edgesPerPage));

    return false;
  }

  const SegmentLayout segmentLayout = layout();

  file.seekg(segmentLayout.headerOffset(slot));
  file.read(reinterpret_cast<char *>(&header), sizeof(header));

  if (!file)
  {
    reportIo(sink, "segment is truncated at the page header", path);

    return false;
  }

  if (header.pageId == freePageSlot)
  {
    SourceSpan span;

    sink.error(span, "page " + std::to_string(id) + " is a free slot");

    return false;
  }

  if (header.count > storeSettings.edgesPerPage)
  {
    SourceSpan span;

    sink.error(span, "page " + std::to_string(id) + " claims " + std::to_string(header.count) + " edges");

    return false;
  }

  edges.resize(header.count);

  if (header.count != 0u)
  {
    file.seekg(segmentLayout.edgesOffset(slot));
    file.read(reinterpret_cast<char *>(edges.data()), static_cast<std::streamsize>(sizeof(Edge) * header.count));

    if (!file)
    {
      reportIo(sink, "segment is truncated in the edge block", path);

      return false;
    }
  }

  ++reads;

  return true;
}

bool DiskStore::freePage(const uint32_t partition, const PageId id, DiagnosticSink &sink)
{
  DiskPageHeader header;

  header.pageId = freePageSlot;

  const uint32_t segment = id / storeSettings.pagesPerSegment;
  const uint32_t slot = id % storeSettings.pagesPerSegment;

  if (!ensureSegment(partition, segment, sink))
  {
    return false;
  }

  std::fstream file(segmentPath(partition, segment), std::ios::binary | std::ios::in | std::ios::out);

  if (!file)
  {
    return false;
  }

  file.seekp(layout().headerOffset(slot));
  file.write(reinterpret_cast<const char *>(&header), sizeof(header));

  ++writes;

  return file.good();
}

PageId DiskStore::allocatePage(const uint32_t partition, DiagnosticSink &sink)
{
  const uint32_t segments = std::max(segmentCount(partition), 1u);

  // Fill holes before appending: a page freed by a merge leaves a slot that
  // costs nothing to reuse.
  for (uint32_t segment = 0u; segment < segments; ++segment)
  {
    if (!ensureSegment(partition, segment, sink))
    {
      return invalidPage;
    }

    std::ifstream file(segmentPath(partition, segment), std::ios::binary);

    if (!file)
    {
      return invalidPage;
    }

    const SegmentLayout segmentLayout = layout();

    for (uint32_t slot = 0u; slot < storeSettings.pagesPerSegment; ++slot)
    {
      DiskPageHeader header;

      file.seekg(segmentLayout.headerOffset(slot));
      file.read(reinterpret_cast<char *>(&header), sizeof(header));

      if (file && header.pageId == freePageSlot)
      {
        return segment * storeSettings.pagesPerSegment + slot;
      }
    }
  }

  if (!ensureSegment(partition, segments, sink))
  {
    return invalidPage;
  }

  return segments * storeSettings.pagesPerSegment;
}

uint64_t DiskStore::readCount() const
{
  return reads;
}

uint64_t DiskStore::writeCount() const
{
  return writes;
}

} // namespace graphrewrite::runtime
