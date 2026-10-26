// Phase 8a of docs/terrain-vg-unified-hierarchy-plan.md: terrain chunks live in
// a scratch file as ordinary virtual-geometry pages.
//
// The gate for the write path is that a page read back is byte-identical to the
// page the install path would have taken from the in-memory asset - the words
// `PageBuffer::encode` produces, the dependency list, and the install rewrites.
// A single dropped or reordered word here is a chunk that installs cleanly and
// draws nothing, or worse, draws another chunk's geometry.

#include "editor/virtualgeometry/TerrainVirtualGeometryScratchFile.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace svr = voxel::svraster;
namespace vg = virtualgeometry;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &what)
{
  if (condition)
  {
    std::cout << "  PASSED " << what << "\n";
    return;
  }
  std::cout << "  FAILED " << what << "\n";
  ++failures;
}

std::string scratchPath(const std::string &name)
{
  const char *tmp = std::getenv("TMPDIR");
  std::string directory = tmp != nullptr ? tmp : "/tmp/";
  if (directory.back() != '/')
    directory.push_back('/');
  return directory + "terrain-vg-scratch-test-" + name + ".vgcache";
}

// A meshlet with enough of every block populated that the encoder writes
// positions, normals, uvs and indices - an all-zero page would round-trip
// through a broken cursor just as well as a correct one.
vg::Meshlet makeMeshlet(const uint32_t seed, const uint32_t vertexCount, const uint32_t triangleCount)
{
  vg::Meshlet meshlet{};
  meshlet.vertex_count = static_cast<uint8_t>(vertexCount);
  meshlet.triangle_count = static_cast<uint8_t>(triangleCount);
  meshlet.start_vertex_position_bit = seed * 64u;
  meshlet.start_vertex_attribute_id = seed * vertexCount;
  meshlet.start_index_id = seed * 4u;
  meshlet.quantized_position_span_x = 255u;
  meshlet.quantized_position_span_y = 127u;
  meshlet.quantized_position_span_z = 63u;
  meshlet.vertex_position_quantization_factor = 6u;
  meshlet.min_vertex_position_channel_x = -static_cast<int32_t>(seed);
  meshlet.min_vertex_position_channel_y = 2 * static_cast<int32_t>(seed);
  meshlet.min_vertex_position_channel_z = -3 * static_cast<int32_t>(seed);
  meshlet.self.center[0] = static_cast<float>(seed);
  meshlet.self.center[1] = 1.5f * static_cast<float>(seed);
  meshlet.self.center[2] = -2.0f * static_cast<float>(seed);
  meshlet.self.radius = 3.0f + static_cast<float>(seed);
  meshlet.self.error = 0.125f * static_cast<float>(seed + 1u);
  meshlet.parent.center[0] = 0.5f;
  meshlet.parent.radius = 8.0f;
  meshlet.parent.error = 4.0f;
  meshlet.cone.axis[0] = 0.0f;
  meshlet.cone.axis[1] = 1.0f;
  meshlet.cone.axis[2] = 0.0f;
  meshlet.cone.cutoff = -0.25f;
  meshlet.pageLocalGroupIndex = static_cast<uint8_t>(seed % 8u);
  meshlet.clusterIndexInGroup = static_cast<uint8_t>(seed % 4u);
  return meshlet;
}

vg::VirtualGeometryEncodedData makeAsset(const uint32_t pageCount)
{
  vg::VirtualGeometryEncodedData asset;
  asset.quantizationConfig.unit_scale = 512.0f;
  asset.quantizationConfig.quantization_factor = 6u;
  asset.quantizationConfig.padMeshlets = false;
  asset.rootPageIndex = 0u;
  asset.root_node_index = 0u;

  asset.hierarchy.resize(3u);
  for (uint32_t index = 0u; index < asset.hierarchy.size(); ++index)
  {
    auto &node = asset.hierarchy[index];
    node.min_x = -1.0f * static_cast<float>(index + 1u);
    node.min_y = -2.0f;
    node.min_z = -3.0f;
    node.max_x = 4.0f * static_cast<float>(index + 1u);
    node.max_y = 5.0f;
    node.max_z = 6.0f;
    node.max_center_x = 0.5f;
    node.max_center_y = 0.25f;
    node.max_center_z = 0.125f;
    node.max_radius = 7.0f;
    node.min_lod_error = 0.5f * static_cast<float>(index);
    node.max_parent_lod_error = 100.0f;
    node.child_start = index + 1u;
    node.child_count = index == 0u ? 2u : 0u;
    node.pageIndex = index;
    node.meshPartIndex = 0u;
    node.flags = index;
  }

  asset.pages.resize(pageCount);
  for (uint32_t pageIndex = 0u; pageIndex < pageCount; ++pageIndex)
  {
    auto &page = asset.pages[pageIndex];
    const uint32_t meshletCount = 1u + pageIndex;
    page.groupCount = 1u;
    page.groupMeshletOffsets = {0u};
    for (uint32_t meshlet = 0u; meshlet < meshletCount; ++meshlet)
      page.meshlets.push_back(makeMeshlet(pageIndex * 8u + meshlet, 8u, 4u));

    const size_t vertexCount = page.meshlets.size() * 8u;
    const size_t indexCount = page.meshlets.size() * 12u;
    // Positions are a bit-packed uint32 stream, normals oct-encoded uint32,
    // uvs plain floats and indices one byte each: the encoder packs each block
    // differently, so a fixture that filled them all the same way would not
    // exercise the framing.
    page.vertexBuffers.positions.resize(vertexCount * 2u);
    page.vertexBuffers.normals.resize(vertexCount);
    page.vertexBuffers.uvs.resize(vertexCount * 2u);
    page.vertexBuffers.indices.resize(indexCount);
    for (size_t index = 0u; index < page.vertexBuffers.positions.size(); ++index)
      page.vertexBuffers.positions[index] = 0x9e3779b9u * static_cast<uint32_t>(index + pageIndex);
    for (size_t index = 0u; index < page.vertexBuffers.normals.size(); ++index)
      page.vertexBuffers.normals[index] = 0x85ebca6bu ^ static_cast<uint32_t>(index);
    for (size_t index = 0u; index < page.vertexBuffers.uvs.size(); ++index)
      page.vertexBuffers.uvs[index] = 0.5f * static_cast<float>(index % 4u);
    for (size_t index = 0u; index < indexCount; ++index)
      page.vertexBuffers.indices[index] = static_cast<uint8_t>(index % 8u);

    // Pages depend on their predecessor by construction, which is what makes a
    // requested page's missing prefix well defined.
    if (pageIndex != 0u)
      page.dependencies.push_back(pageIndex - 1u);

    vg::HierarchyClusterFlagsUpdate install{};
    install.hierarchyNodeIndex = pageIndex;
    install.streamingLeafsBitset = static_cast<uint8_t>(0x3u << (pageIndex % 4u));
    install.enabledClustersBitset = static_cast<uint8_t>(0xf0u >> (pageIndex % 4u));
    page.installUpdates.hierarchyUpdates.push_back(install);
    vg::HierarchyClusterFlagsUpdate uninstall = install;
    uninstall.streamingLeafsBitset = 0xffu;
    page.uninstallUpdates.hierarchyUpdates.push_back(uninstall);
  }
  return asset;
}

bool sameUpdates(
    const std::vector<vg::HierarchyClusterFlagsUpdate> &left,
    const std::vector<vg::HierarchyClusterFlagsUpdate> &right)
{
  if (left.size() != right.size())
    return false;
  for (size_t index = 0u; index < left.size(); ++index)
  {
    if (left[index].hierarchyNodeIndex != right[index].hierarchyNodeIndex ||
        left[index].streamingLeafsBitset != right[index].streamingLeafsBitset ||
        left[index].enabledClustersBitset != right[index].enabledClustersBitset)
      return false;
  }
  return true;
}

void testPageRoundTrip()
{
  std::cout << "testPageRoundTrip\n";
  const std::string path = scratchPath("roundtrip");
  std::remove(path.c_str());

  svr::TerrainVirtualGeometryScratchFile file;
  std::string error;
  check(file.open(path, 0u, &error), "opens a fresh scratch file: " + error);

  const vg::VirtualGeometryEncodedData assetA = makeAsset(4u);
  const vg::VirtualGeometryEncodedData assetB = makeAsset(2u);
  uint32_t handleA = svr::TerrainVirtualGeometryScratchFile::kInvalidChunk;
  uint32_t handleB = svr::TerrainVirtualGeometryScratchFile::kInvalidChunk;
  check(file.appendChunk(assetA, handleA, &error), "appends the first chunk: " + error);
  check(file.appendChunk(assetB, handleB, &error), "appends the second chunk: " + error);
  check(handleA != handleB, "each chunk gets its own handle");

  // Both chunks, interleaved: a read must resolve through its own directory
  // entry, not through wherever the write cursor happens to be.
  const std::pair<const vg::VirtualGeometryEncodedData *, uint32_t> assets[]{
    {&assetA, handleA}, {&assetB, handleB}};
  for (const auto &entry : assets)
  {
    const auto &asset = *entry.first;
    const uint32_t handle = entry.second;

    svr::TerrainVirtualGeometryScratchChunk chunk;
    check(file.readChunk(handle, chunk, &error), "reads the chunk header: " + error);
    check(chunk.pageCount == asset.pages.size(), "page count survives the round trip");
    check(chunk.rootPageIndex == asset.rootPageIndex, "root page index survives");
    check(
        chunk.quantization.unit_scale == asset.quantizationConfig.unit_scale,
        "unit scale survives - a zero here collapses every vertex to the origin");
    check(
        chunk.quantization.quantization_factor == asset.quantizationConfig.quantization_factor,
        "quantization factor survives");
    check(chunk.hierarchy.size() == asset.hierarchy.size(), "hierarchy node count survives");
    bool hierarchyIdentical = chunk.hierarchy.size() == asset.hierarchy.size();
    for (size_t index = 0u; hierarchyIdentical && index < chunk.hierarchy.size(); ++index)
      hierarchyIdentical = std::memcmp(
                               &chunk.hierarchy[index], &asset.hierarchy[index],
                               sizeof(vg::VirtualGeometryHierarchy)) == 0;
    check(hierarchyIdentical, "every hierarchy node is byte-identical");
    std::vector<vg::HierarchyClusterFlagsUpdate> expectedUpdates;
    for (const auto &page : asset.pages)
      expectedUpdates.insert(
          expectedUpdates.end(), page.installUpdates.hierarchyUpdates.begin(),
          page.installUpdates.hierarchyUpdates.end());
    check(
        sameUpdates(chunk.installUpdates, expectedUpdates),
        "the header carries every page's install rewrites, in page order");
    check(file.pageCountOf(handle) == asset.pages.size(), "pageCountOf agrees without a read");

    bool everyPageIdentical = true;
    bool everyDependencyIdentical = true;
    bool everyUpdateIdentical = true;
    for (uint32_t pageIndex = 0u; pageIndex < asset.pages.size(); ++pageIndex)
    {
      svr::TerrainVirtualGeometryScratchPage page;
      if (!file.readPage(handle, pageIndex, page, &error))
      {
        check(false, "reads page " + std::to_string(pageIndex) + ": " + error);
        everyPageIdentical = false;
        continue;
      }
      const vg::PageBuffer encoded = vg::PageBuffer::encode(asset.pages[pageIndex]);
      everyPageIdentical = everyPageIdentical && page.encodedWords == encoded.data;
      everyDependencyIdentical =
          everyDependencyIdentical && page.dependencies == asset.pages[pageIndex].dependencies;
      everyUpdateIdentical = everyUpdateIdentical &&
          sameUpdates(page.installUpdates, asset.pages[pageIndex].installUpdates.hierarchyUpdates) &&
          sameUpdates(page.uninstallUpdates, asset.pages[pageIndex].uninstallUpdates.hierarchyUpdates);
    }
    check(everyPageIdentical, "every page's encoded words are byte-identical to PageBuffer::encode");
    check(everyDependencyIdentical, "every page's dependency list survives");
    check(everyUpdateIdentical, "every page's install and uninstall rewrites survive");
  }

  const auto stats = file.stats();
  check(stats.chunksWritten == 2u, "two chunks written");
  check(stats.pagesWritten == assetA.pages.size() + assetB.pages.size(), "six pages written");
  check(stats.readFailures == 0u, "no read failed");
  // The whole point of the file is that it is smaller than the arena it
  // replaces; LZ4 on this fixture is not the measurement, but a file larger
  // than the plain bytes means the framing is wrong.
  check(stats.fileBytes != 0u && stats.bytesWritten == stats.fileBytes, "file bytes account for every block");
  file.close();
  std::remove(path.c_str());
}

void testReleaseAndBadHandles()
{
  std::cout << "testReleaseAndBadHandles\n";
  const std::string path = scratchPath("release");
  std::remove(path.c_str());

  svr::TerrainVirtualGeometryScratchFile file;
  std::string error;
  check(file.open(path, 0u, &error), "opens: " + error);
  const vg::VirtualGeometryEncodedData asset = makeAsset(3u);
  uint32_t handle = svr::TerrainVirtualGeometryScratchFile::kInvalidChunk;
  check(file.appendChunk(asset, handle, &error), "appends: " + error);
  check(file.isLive(handle), "the handle is live");

  svr::TerrainVirtualGeometryScratchPage page;
  check(!file.readPage(handle, 99u, page, &error), "a page index past the chunk fails");
  check(!file.readPage(handle + 1000u, 0u, page, &error), "an unknown handle fails");

  check(file.releaseChunk(handle), "releases the chunk");
  check(!file.isLive(handle), "the handle is no longer live");
  // A retired handle must fail rather than resolve to whatever now occupies
  // those bytes: a stale handle that reads is a chunk drawing another chunk's
  // geometry, which looks like a culler bug for days.
  check(!file.readPage(handle, 0u, page, &error), "a released handle no longer reads");
  svr::TerrainVirtualGeometryScratchChunk chunk;
  check(!file.readChunk(handle, chunk, &error), "a released handle has no header");
  check(!file.releaseChunk(handle), "releasing twice is refused");
  check(file.stats().chunksLive == 0u, "no chunk is live");
  file.close();
  std::remove(path.c_str());
}

void testOpenRefusesExistingPath()
{
  std::cout << "testOpenRefusesExistingPath\n";
  const std::string path = scratchPath("existing");
  std::remove(path.c_str());
  if (FILE *existing = std::fopen(path.c_str(), "wb"))
  {
    const char payload[] = "not a scratch file";
    std::fwrite(payload, 1u, sizeof(payload), existing);
    std::fclose(existing);
  }
  svr::TerrainVirtualGeometryScratchFile file;
  std::string error;
  check(!file.open(path, 0u, &error), "refuses to truncate a path that already exists");
  check(!error.empty(), "and says why");
  check(!file.isOpen(), "and stays closed");
  std::remove(path.c_str());
}

void testRegionReuse()
{
  std::cout << "testRegionReuse\n";
  const std::string path = scratchPath("reuse");
  std::remove(path.c_str());

  svr::TerrainVirtualGeometryScratchFile file;
  std::string error;
  check(file.open(path, 0u, &error), "opens: " + error);

  const vg::VirtualGeometryEncodedData asset = makeAsset(3u);
  uint32_t first = 0u;
  uint32_t second = 0u;
  uint32_t third = 0u;
  check(file.appendChunk(asset, first, &error), "appends the first chunk: " + error);
  check(file.appendChunk(asset, second, &error), "appends the second chunk: " + error);
  const uint64_t highWater = file.stats().fileBytes;
  check(highWater != 0u, "the file has a high-water mark");

  // Free the *first* chunk, so the hole is in the middle and reuse cannot be
  // confused with the cursor simply moving back.
  check(file.releaseChunk(first), "releases the first chunk");
  check(file.stats().bytesFree != 0u, "its bytes are on the free list");
  check(file.appendChunk(asset, third, &error), "appends a third chunk: " + error);
  check(
      file.stats().fileBytes == highWater,
      "the third chunk reused the hole instead of extending the file");
  check(file.stats().appendsReusingRegion == 1u, "exactly one append reused a region");

  // The reused region must serve the new chunk, and the retired handle must
  // still be dead rather than resolving into it.
  svr::TerrainVirtualGeometryScratchPage page;
  bool reusedReadsBack = true;
  for (uint32_t pageIndex = 0u; pageIndex < asset.pages.size(); ++pageIndex)
  {
    if (!file.readPage(third, pageIndex, page, &error))
    {
      reusedReadsBack = false;
      break;
    }
    reusedReadsBack = reusedReadsBack &&
        page.encodedWords == vg::PageBuffer::encode(asset.pages[pageIndex]).data;
  }
  check(reusedReadsBack, "every page of the chunk in the reused region reads back intact");
  check(!file.readPage(first, 0u, page, &error), "the retired handle stays dead");

  // Releasing everything must give the high-water mark back, or a long walk
  // holds the peak of the whole run forever.
  check(file.releaseChunk(second), "releases the second chunk");
  check(file.releaseChunk(third), "releases the third chunk");
  check(file.stats().fileBytes == 0u, "an empty file has no high-water mark left");
  check(file.stats().bytesFree == 0u, "and nothing left on the free list");
  file.close();
  std::remove(path.c_str());
}

void testSizeBudget()
{
  std::cout << "testSizeBudget\n";
  const std::string path = scratchPath("budget");
  std::remove(path.c_str());

  svr::TerrainVirtualGeometryScratchFile file;
  std::string error;
  const vg::VirtualGeometryEncodedData asset = makeAsset(3u);
  // Size the budget so exactly one chunk fits.
  svr::TerrainVirtualGeometryScratchBlob blob;
  check(svr::TerrainVirtualGeometryScratchFile::encodeChunk(asset, blob, &error), "encodes: " + error);
  const uint64_t chunkBytes = blob.bytes.size();
  check(file.open(path, chunkBytes, &error), "opens with a one-chunk budget: " + error);

  uint32_t first = 0u;
  uint32_t second = 0u;
  check(file.appendChunk(asset, first, &error), "the first chunk fits: " + error);
  // Refused, not fatal: the caller keeps the chunk's asset and the file stays
  // consistent.
  check(!file.appendChunk(asset, second, &error), "the second is refused by the budget");
  check(file.stats().appendsRefusedByBudget == 1u, "and is counted as a budget refusal");
  check(file.stats().chunksLive == 1u, "the chunk that fit is still live");
  svr::TerrainVirtualGeometryScratchPage page;
  check(file.readPage(first, 0u, page, &error), "and still reads: " + error);

  // Freeing the resident chunk must make room for the next one.
  check(file.releaseChunk(first), "releases it");
  check(file.appendChunk(asset, second, &error), "the next chunk now fits: " + error);
  file.close();
  std::remove(path.c_str());
}

// Phase 8g: a page is a (file, offset, size) the scene reads itself. The
// scratch file hands out locations and reads them back through the
// page-source interface, refusing anything that is not exactly a live chunk's
// own range - a location into a reused region would be another chunk's
// geometry - and the chunk header carries every page's dependencies so a page
// install touches no page block at all.
void testPageLocationsReadThroughTheSource()
{
  std::cout << "testPageLocationsReadThroughTheSource\n";
  const std::string path = scratchPath("locations");
  std::remove(path.c_str());

  svr::TerrainVirtualGeometryScratchFile file;
  std::string error;
  check(file.open(path, 0u, &error), "opens a fresh scratch file: " + error);
  const vg::VirtualGeometryEncodedData asset = makeAsset(3u);
  uint32_t handle = svr::TerrainVirtualGeometryScratchFile::kInvalidChunk;
  check(file.appendChunk(asset, handle, &error), "appends a chunk: " + error);

  svr::TerrainVirtualGeometryScratchChunk chunk;
  check(file.readChunk(handle, chunk, &error), "reads the chunk header: " + error);
  check(chunk.dependencyCounts.size() == asset.pages.size(), "the header carries one dependency count per page");
  {
    bool same = chunk.dependencyCounts.size() == asset.pages.size();
    size_t cursor = 0u;
    for (size_t page = 0u; same && page < asset.pages.size(); ++page)
    {
      const auto &expected = asset.pages[page].dependencies;
      same = chunk.dependencyCounts[page] == expected.size() &&
          cursor + expected.size() <= chunk.dependencies.size() &&
          std::equal(expected.begin(), expected.end(), chunk.dependencies.begin() + static_cast<long>(cursor));
      cursor += expected.size();
    }
    check(same && cursor == chunk.dependencies.size(), "the header's dependencies are the pages' own, in page order");
  }

  const vg::VirtualGeometryPageSource &source = file;
  for (uint32_t page = 0u; page < asset.pages.size(); ++page)
  {
    const auto location = file.pageLocation(handle, page);
    check(
        location.valid() && location.source == &file && location.key == handle &&
            location.index == page,
        "page " + std::to_string(page) + " has a location naming this file and chunk");
    std::vector<uint32_t> words;
    check(source.readEncodedPage(location, words, &error), "reads through the page source: " + error);
    check(words == vg::PageBuffer::encode(asset.pages[page]).data, "the words are PageBuffer::encode's");
    svr::TerrainVirtualGeometryScratchPage full;
    check(file.readPage(handle, page, full, &error) && full.encodedWords == words, "and what readPage returns");
  }

  std::vector<uint32_t> words;
  auto tampered = file.pageLocation(handle, 0u);
  tampered.offset += 1u;
  check(!source.readEncodedPage(tampered, words, &error), "a location whose range disagrees with the record is refused");
  check(!file.pageLocation(handle, 99u).valid(), "a page past the chunk has no location");

  // A released chunk's locations die with it, even once its region is reused.
  const auto kept = file.pageLocation(handle, 1u);
  check(file.releaseChunk(handle), "releases the chunk");
  uint32_t next = svr::TerrainVirtualGeometryScratchFile::kInvalidChunk;
  check(file.appendChunk(makeAsset(2u), next, &error), "another chunk takes its region: " + error);
  check(!source.readEncodedPage(kept, words, &error), "the released chunk's location no longer reads");
  check(!file.pageLocation(handle, 1u).valid(), "and no new location is issued for it");
  check(file.pageLocation(next, 1u).valid(), "while the new chunk's pages have theirs");

  const auto stats = file.stats();
  check(stats.encodedPageReads == asset.pages.size(), "stats count the source reads");
  check(stats.staleLocationReads == 2u, "and the refused locations");
  file.close();
  std::remove(path.c_str());
}

// The same interface on an ordinary `.vg` file: its page table already is the
// (offset, size) per page, so any file-backed instance can be served the way
// terrain is.
void testVirtualGeometryFileIsAPageSource()
{
  std::cout << "testVirtualGeometryFileIsAPageSource\n";
  const std::string path = scratchPath("vgfile") + ".vg";
  std::remove(path.c_str());
  const vg::VirtualGeometryEncodedData asset = makeAsset(3u);
  {
    vg::VirtualGeometryFile writer(path, true);
    check(writer.isOpen(), "creates a .vg file");
    check(writer.write(asset, {}, vg::MESHLET_LZ4), "writes the pages");
  }
  std::string error;
  vg::VirtualGeometryFile reader(path);
  check(reader.isOpen(), "opens it for reading");
  check(reader.getPageTable().size() == asset.pages.size(), "the page table has every page");
  const vg::VirtualGeometryPageSource &source = reader;
  for (uint32_t page = 0u; page < reader.getPageTable().size(); ++page)
  {
    const auto &descriptor = reader.getPageTable()[page];
    const auto location = reader.pageLocation(page);
    check(
        location.valid() && location.source == &reader && location.index == page &&
            location.offset == descriptor.file_offset &&
            location.storedBytes == descriptor.compressed_size &&
            location.uncompressedBytes == descriptor.uncompressed_size,
        "page " + std::to_string(page) + "'s location is its page table entry");
    std::vector<uint32_t> words;
    check(source.readEncodedPage(location, words, &error), "reads through the page source: " + error);
    // The file may pad pages to its maximum page size; the encoded words are
    // the prefix either way.
    const auto encoded = vg::PageBuffer::encode(asset.pages[page]).data;
    check(
        words.size() >= encoded.size() &&
            std::equal(encoded.begin(), encoded.end(), words.begin()),
        "the words are PageBuffer::encode's");
  }
  std::vector<uint32_t> words;
  auto tampered = reader.pageLocation(0u);
  tampered.storedBytes += 1u;
  check(!source.readEncodedPage(tampered, words, &error), "a location that is not the page table entry is refused");
  check(!reader.pageLocation(99u).valid(), "a page past the table has no location");
  std::remove(path.c_str());
}

} // namespace

void testAsyncPageReads()
{
  const std::string path = "/tmp/terrain-vg-async-source-test.bin";
  std::remove(path.c_str());
  svr::TerrainVirtualGeometryScratchFile file;
  std::string error;
  check(file.open(path, 64u * 1024u * 1024u, &error), "async source opens");
  auto asset = makeAsset(4u);
  uint32_t handle = svr::TerrainVirtualGeometryScratchFile::kInvalidChunk;
  check(file.appendChunk(asset, handle, &error), "async source appends a chunk");
  const auto location = file.pageLocation(handle, 0u);
  std::vector<uint32_t> expected, words;
  check(file.readEncodedPage(location, expected, &error), "reference page reads");
  bool pending = false;
  check(!file.tryReadEncodedPage(location, words, pending, &error) && pending,
      "first asynchronous read queues work without waiting");
  bool read = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  do
  {
    read = file.tryReadEncodedPage(location, words, pending, &error);
    if (!pending) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < deadline);
  check(read && words == expected, "background read returns identical encoded words");
  const auto stale = file.pageLocation(handle, 1u);
  file.tryReadEncodedPage(stale, words, pending, &error);
  check(file.releaseChunk(handle), "retire a chunk with an outstanding read");
  bool rejected = false;
  do
  {
    const bool success = file.tryReadEncodedPage(stale, words, pending, &error);
    if (!pending) { rejected = !success; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < deadline);
  check(rejected, "a completed or pending read cannot revive a retired chunk");
  file.close();
}

int main()
{
  std::cout << "SparseVoxelTerrainVGScratchFileTests\n";
  testPageRoundTrip();
  testReleaseAndBadHandles();
  testOpenRefusesExistingPath();
  testRegionReuse();
  testSizeBudget();
  testPageLocationsReadThroughTheSource();
  testVirtualGeometryFileIsAPageSource();
  testAsyncPageReads();
  if (failures != 0)
  {
    std::cout << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
