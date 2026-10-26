#include "graphrewrite/runtime/Manifest.hpp"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <filesystem>
#include <fstream>

namespace graphrewrite::runtime {
namespace {

void putU32(std::vector<uint8_t> &out, const uint32_t value)
{
  out.push_back(static_cast<uint8_t>(value & 0xffu));
  out.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
  out.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
  out.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
}

bool takeU32(const std::vector<uint8_t> &data, size_t &head, uint32_t &out)
{
  if (head + 4u > data.size())
  {
    return false;
  }

  out = static_cast<uint32_t>(data[head]) | (static_cast<uint32_t>(data[head + 1u]) << 8u) |
        (static_cast<uint32_t>(data[head + 2u]) << 16u) | (static_cast<uint32_t>(data[head + 3u]) << 24u);

  head += 4u;

  return true;
}

} // namespace

std::string manifestPath(const DiskStore &disk)
{
  return disk.settings().root + "/manifest.gre";
}

bool commitStore(const EdgePageStore &store, DiskStore &disk, const uint32_t generation, DiagnosticSink &sink)
{
  std::vector<uint8_t> blob;

  putU32(blob, manifestMagic);
  putU32(blob, manifestVersion);
  putU32(blob, generation);
  putU32(blob, disk.settings().edgesPerPage);
  putU32(blob, disk.settings().pagesPerSegment);

  putU32(blob, store.partitionCount());

  for (uint32_t index = 0u; index < store.partitionCount(); ++index)
  {
    const Partition &partition = store.partitionAt(index);

    putU32(blob, partition.edgeType());
    putU32(blob, partition.order() == KeyOrder::ByOrigin ? 0u : 1u);
    putU32(blob, partition.pageCount());

    for (uint32_t position = 0u; position < partition.pageCount(); ++position)
    {
      const PageId id = partition.pageIdAt(position);

      putU32(blob, id);
      putU32(blob, partition.minKeyAt(position));
      putU32(blob, partition.maxKeyAt(position));

      DiskPageHeader header;

      header.pageId = id;
      header.minKey = partition.minKeyAt(position);
      header.maxKey = partition.maxKeyAt(position);

      if (!disk.writePage(index, id, header, partition.pageAt(position), sink))
      {
        return false;
      }
    }
  }

  // Write to a temporary file, then rename over the old manifest. The rename is
  // the commit point: before it the previous generation is still what loads.
  const std::string finalPath = manifestPath(disk);
  const std::string tempPath = finalPath + ".tmp";

  {
    std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);

    if (!file)
    {
      SourceSpan span;

      sink.error(span, "cannot write manifest: " + tempPath);

      return false;
    }

    file.write(reinterpret_cast<const char *>(blob.data()), static_cast<std::streamsize>(blob.size()));
    file.flush();
  }

  std::error_code error;

  std::filesystem::rename(tempPath, finalPath, error);

  if (error)
  {
    SourceSpan span;

    sink.error(span, "cannot commit manifest: " + error.message());

    return false;
  }

  return true;
}

bool openStore(DiskStore &disk, EdgePageStore &out, DiagnosticSink &sink, uint32_t *generation)
{
  const std::string path = manifestPath(disk);

  std::ifstream file(path, std::ios::binary);

  if (!file)
  {
    SourceSpan span;

    sink.error(span, "no manifest at " + path);

    return false;
  }

  const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  size_t head = 0u;

  uint32_t magic = 0u;
  uint32_t version = 0u;
  uint32_t storedGeneration = 0u;
  uint32_t edgesPerPage = 0u;
  uint32_t pagesPerSegment = 0u;

  if (!takeU32(blob, head, magic) || !takeU32(blob, head, version) || !takeU32(blob, head, storedGeneration) ||
      !takeU32(blob, head, edgesPerPage) || !takeU32(blob, head, pagesPerSegment))
  {
    SourceSpan span;

    sink.error(span, "manifest is truncated in its header");

    return false;
  }

  if (magic != manifestMagic)
  {
    SourceSpan span;

    sink.error(span, "not a graph-rewrite manifest: " + path);

    return false;
  }

  if (version != manifestVersion)
  {
    SourceSpan span;

    sink.error(span, "manifest is version " + std::to_string(version) + ", expected " + std::to_string(manifestVersion));

    return false;
  }

  // A store written with a different page geometry cannot be addressed with this
  // one: the offsets would land inside other pages and read plausible garbage.
  if (edgesPerPage != disk.settings().edgesPerPage || pagesPerSegment != disk.settings().pagesPerSegment)
  {
    SourceSpan span;

    sink.error(span, "manifest was written with pageEdges=" + std::to_string(edgesPerPage) + " pagesPerSegment=" +
                         std::to_string(pagesPerSegment) + ", this store uses " + std::to_string(disk.settings().edgesPerPage) + " and " +
                         std::to_string(disk.settings().pagesPerSegment));

    return false;
  }

  uint32_t partitionCount = 0u;

  if (!takeU32(blob, head, partitionCount))
  {
    return false;
  }

  compiler::EdgeTypeTable types;
  std::vector<std::pair<EdgeTypeId, uint32_t>> layout;

  std::vector<PartitionDirectory> directories;
  std::vector<std::vector<std::vector<Edge>>> pages;

  for (uint32_t index = 0u; index < partitionCount; ++index)
  {
    uint32_t edgeType = 0u;
    uint32_t order = 0u;
    uint32_t pageCount = 0u;

    if (!takeU32(blob, head, edgeType) || !takeU32(blob, head, order) || !takeU32(blob, head, pageCount))
    {
      return false;
    }

    layout.emplace_back(edgeType, order);

    PartitionDirectory directory;
    std::vector<std::vector<Edge>> partitionPages;

    for (uint32_t position = 0u; position < pageCount; ++position)
    {
      uint32_t id = 0u;
      uint32_t minKey = 0u;
      uint32_t maxKey = 0u;

      if (!takeU32(blob, head, id) || !takeU32(blob, head, minKey) || !takeU32(blob, head, maxKey))
      {
        return false;
      }

      DiskPageHeader header;
      std::vector<Edge> edges;

      if (!disk.readPage(index, id, header, edges, sink))
      {
        return false;
      }

      if (partitionPages.size() <= id)
      {
        partitionPages.resize(id + 1u);
      }

      partitionPages[id] = std::move(edges);

      directory.order.push_back(id);
      directory.minKey.push_back(minKey);
      directory.maxKey.push_back(maxKey);
    }

    directories.push_back(std::move(directory));
    pages.push_back(std::move(partitionPages));
  }

  // Rebuild the type table from what the manifest says each partition indexes.
  uint32_t maxType = 0u;

  for (const auto &entry : layout)
  {
    maxType = std::max(maxType, entry.first + 1u);
  }

  for (uint32_t type = 0u; type < maxType; ++type)
  {
    types.intern("restored." + std::to_string(type), 2u);
  }

  for (const auto &entry : layout)
  {
    if (entry.second == 0u)
    {
      types.mutableType(entry.first).usedForward = true;
    }
    else
    {
      types.mutableType(entry.first).usedBackward = true;
    }
  }

  out.configure(types);

  for (uint32_t index = 0u; index < partitionCount && index < out.partitionCount(); ++index)
  {
    out.partitionAt(index).restore(std::move(pages[index]), std::move(directories[index]));
  }

  if (generation != nullptr)
  {
    *generation = storedGeneration;
  }

  return true;
}

} // namespace graphrewrite::runtime
