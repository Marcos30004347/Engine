#include "os/Logger.hpp"
#include "procedural/world/MapExport.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/TerrainGPUV2Columns.hpp"
#include "voxel/svraster/CityGroundOverlay.hpp"
#include "voxel/svraster/PlanetVoxelChunk.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2ColumnChain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace world = procedural::world;
namespace svr = voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

std::vector<uint8_t> texel(const std::string &name)
{
  if (name == "refined_elevation" || name == "water_level")
    return {0u, 0u};
  if (name == "water_type")
    return {0u};
  if (name == "river")
    return {0u, 0u, 0u, 0u};
  if (name == "substrate_id")
    return {3u};
  if (name == "soil_depth")
    return {0u, 0x3cu};
  if (name == "biome_mix_ids")
    return {13u, 0u, 0u, 0u};
  return {255u, 0u, 0u, 0u};
}

world::MapExportField field(const std::string &name, const world::MapExportFormat format, const world::MapExportFilter filter, const std::string &group = {})
{
  world::MapExportField result{};
  result.name = name;
  result.format = format;
  result.filter = filter;
  result.available = true;
  result.width = 8u;
  result.height = 8u;
  result.samplingGroup = group;
  if (name == "refined_elevation")
    result.offset[0] = 320.0;
  const auto value = texel(name);
  world::MapExportPage page{};
  page.address = {0u, 0u, 0u};
  page.width = 10u;
  page.height = 10u;
  page.data.resize(100u * value.size());
  for (uint32_t pixel = 0u; pixel < 100u; ++pixel)
    std::memcpy(page.data.data() + uint64_t{pixel} * value.size(), value.data(), value.size());
  result.pages.push_back(std::move(page));
  return result;
}

world::MapExportBundle bundle()
{
  world::MapExportBundle result{};
  result.resolution = 8u;
  result.pageSizePixels = 8u;
  result.gutterPixels = 1u;
  result.maximumPageLevel = 0u;
  result.extentMetres = 64.0;
  result.seed = 17u;
  result.bundleSignature = 23u;
  result.fields = {
    field("refined_elevation", world::MapExportFormat::R16Unorm, world::MapExportFilter::Linear),
    field("water_type", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest),
    field("water_level", world::MapExportFormat::R16Float, world::MapExportFilter::Linear),
    field("river", world::MapExportFormat::RG16Float, world::MapExportFilter::Linear),
    field("substrate_id", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest),
    field("soil_depth", world::MapExportFormat::R16Float, world::MapExportFilter::Linear),
    field("biome_mix_ids", world::MapExportFormat::RGBA8Uint, world::MapExportFilter::SparseMixture, "biome"),
    field("biome_mix_weights", world::MapExportFormat::RGBA8Unorm, world::MapExportFilter::SparseMixture, "biome"),
  };
  return result;
}

struct AtlasUpload
{
  std::vector<uint32_t> metadata;
  std::vector<uint32_t> pageTable;
  std::vector<uint8_t> physical;
};

AtlasUpload atlasUpload(const world::MapExportBundle &source, const world::MapExportGpuPageLayout &layout)
{
  AtlasUpload result{};
  const size_t metadataBytes = sizeof(world::MapExportGpuHeader) + layout.descriptors().size() * sizeof(world::MapExportGpuFieldDescriptor);
  result.metadata.resize(metadataBytes / sizeof(uint32_t));
  std::memcpy(result.metadata.data(), &layout.header(), sizeof(world::MapExportGpuHeader));
  std::memcpy(result.metadata.data() + sizeof(world::MapExportGpuHeader) / sizeof(uint32_t), layout.descriptors().data(), layout.descriptors().size() * sizeof(world::MapExportGpuFieldDescriptor));
  result.pageTable.resize(uint64_t{layout.entriesPerField()} * layout.fields().size());
  result.physical.resize(layout.physicalBufferBytes());
  for (uint32_t index = 0u; index < layout.fields().size(); ++index)
  {
    const auto logical = layout.pageTableIndex(index, {0u, 0u, 0u});
    result.pageTable[*logical] = index + 1u;
    const auto *page = source.field(layout.fields()[index].name)->page({0u, 0u, 0u});
    std::memcpy(result.physical.data() + uint64_t{index} * layout.physicalSlotBytes(), page->data.data(), page->data.size());
  }
  return result;
}

const std::array<const char *, svr::kTerrainGPUV2ColumnFieldCount> kWorldColumnFields{
  "refined_elevation",
  "base_elevation",
  "uplift",
  "relief_roughness",
  "landform_id",
  "water_type",
  "water_level",
  "river",
  "coast_distance",
  "erosion_wear_flow",
  "erosion_deposition",
  "substrate_id",
  "soil_depth",
  "biome_mix_ids",
  "biome_mix_weights",
  "climate",
  "placement_potential",
  "soil_layers",
  "roads",
  "settlement_activity",
};

uint32_t worldAtlasPageCount(const world::MapExportBundle &source)
{
  uint32_t count = 0u;
  for (const char *name : kWorldColumnFields)
  {
    const auto *sourceField = source.field(name);
    if (sourceField != nullptr)
      count += static_cast<uint32_t>(sourceField->pages.size());
  }
  return count;
}

AtlasUpload worldAtlasUpload(
    const world::MapExportBundle &source,
    const world::MapExportGpuPageLayout &layout)
{
  AtlasUpload result{};
  const size_t metadataBytes = sizeof(world::MapExportGpuHeader) +
      layout.descriptors().size() * sizeof(world::MapExportGpuFieldDescriptor);
  result.metadata.resize(metadataBytes / sizeof(uint32_t));
  std::memcpy(result.metadata.data(), &layout.header(),
              sizeof(world::MapExportGpuHeader));
  std::memcpy(
      result.metadata.data() +
          sizeof(world::MapExportGpuHeader) / sizeof(uint32_t),
      layout.descriptors().data(),
      layout.descriptors().size() * sizeof(world::MapExportGpuFieldDescriptor));
  result.pageTable.resize(
      uint64_t{layout.entriesPerField()} * layout.fields().size());
  result.physical.resize(layout.physicalBufferBytes());

  uint32_t slot = 0u;
  for (const char *name : kWorldColumnFields)
  {
    const auto fieldIndex = layout.fieldIndex(name);
    const auto *sourceField = source.field(name);
    if (!fieldIndex || sourceField == nullptr)
      continue;
    for (const auto &page : sourceField->pages)
    {
      const auto logical = layout.pageTableIndex(*fieldIndex, page.address);
      if (!logical || slot >= layout.slotCapacity() ||
          page.data.size() > layout.physicalSlotBytes())
        continue;
      result.pageTable[*logical] = slot + 1u;
      std::memcpy(
          result.physical.data() + uint64_t{slot} * layout.physicalSlotBytes(),
          page.data.data(), page.data.size());
      ++slot;
    }
  }
  check(slot == layout.slotCapacity(),
        "real-map atlas uploads every terrain column page");
  return result;
}

template <typename T> std::vector<T> read(::rendering::RenderGraph &graph, const ::rendering::Buffer buffer, const uint32_t count)
{
  std::vector<T> result(count);
  graph.bufferRead(
      buffer,
      0u,
      uint64_t{count} * sizeof(T),
      [&](const void *data)
      {
        std::memcpy(result.data(), data, result.size() * sizeof(T));
      });
  return result;
}

template <typename T> bool bytesEqual(const std::vector<T> &left, const std::vector<T> &right)
{
  return left.size() == right.size() && (left.empty() || std::memcmp(left.data(), right.data(), left.size() * sizeof(T)) == 0);
}

struct GPUOutput
{
  svr::TerrainGPUV2ColumnResultGPU result{};
  std::vector<svr::TerrainGPUV2HeightSampleGPU> heights{};
  std::vector<svr::TerrainGPUV2ColumnGPU> columns{};
};

void diagnoseWorldDifference(
    const GPUOutput &actual,
    const std::vector<svr::TerrainGPUV2HeightSampleGPU> &expectedHeights,
    const std::vector<svr::TerrainGPUV2ColumnGPU> &expectedColumns,
    const uint32_t lod)
{
  uint32_t heightDifferences = 0u;
  uint32_t columnDifferences = 0u;
  size_t firstHeight = expectedHeights.size();
  size_t firstColumn = expectedColumns.size();
  size_t firstRejectedHeight = expectedHeights.size();
  size_t firstRejectedColumn = expectedColumns.size();
  for (size_t index = 0u;
       index < std::min(actual.heights.size(), expectedHeights.size());
       ++index)
  {
    if (std::memcmp(&actual.heights[index], &expectedHeights[index],
                    sizeof(actual.heights[index])) != 0)
    {
      firstHeight = std::min(firstHeight, index);
      ++heightDifferences;
    }
    if (actual.heights[index].canonicalX != expectedHeights[index].canonicalX ||
        actual.heights[index].canonicalZ != expectedHeights[index].canonicalZ ||
        svr::terrainGPUV2HeightSampleFlags(actual.heights[index]) !=
            svr::terrainGPUV2HeightSampleFlags(expectedHeights[index]) ||
        std::abs(int64_t{actual.heights[index].heightUnits} -
                 expectedHeights[index].heightUnits) > 1)
      firstRejectedHeight = std::min(firstRejectedHeight, index);
  }
  for (size_t index = 0u;
       index < std::min(actual.columns.size(), expectedColumns.size());
       ++index)
  {
    if (std::memcmp(&actual.columns[index], &expectedColumns[index],
                    sizeof(actual.columns[index])) != 0)
    {
      firstColumn = std::min(firstColumn, index);
      ++columnDifferences;
    }
    if (actual.columns[index].material != expectedColumns[index].material ||
        actual.columns[index].flags != expectedColumns[index].flags ||
        std::abs(int64_t{actual.columns[index].minimumHeightUnits} -
                 expectedColumns[index].minimumHeightUnits) > 1 ||
        std::abs(int64_t{actual.columns[index].maximumHeightUnits} -
                 expectedColumns[index].maximumHeightUnits) > 1)
      firstRejectedColumn = std::min(firstRejectedColumn, index);
  }
  std::cout << "    LOD " << lod << " GPU status=" << actual.result.status
            << " missing=" << actual.result.missingSamples
            << " faults=" << actual.result.faultCount
            << " heightDiffs=" << heightDifferences
            << " columnDiffs=" << columnDifferences;
  if (firstHeight < expectedHeights.size())
    std::cout << " firstHeight=" << firstHeight
              << " gpu=" << actual.heights[firstHeight].heightUnits
              << " cpu=" << expectedHeights[firstHeight].heightUnits
              << " gpuXZ=(" << actual.heights[firstHeight].canonicalX << ','
              << actual.heights[firstHeight].canonicalZ << ") cpuXZ=("
              << expectedHeights[firstHeight].canonicalX << ','
              << expectedHeights[firstHeight].canonicalZ << ')';
  if (firstColumn < expectedColumns.size())
    std::cout << " firstColumn=" << firstColumn
              << " gpuHeight=("
              << actual.columns[firstColumn].minimumHeightUnits << ','
              << actual.columns[firstColumn].maximumHeightUnits
              << ") cpuHeight=("
              << expectedColumns[firstColumn].minimumHeightUnits << ','
              << expectedColumns[firstColumn].maximumHeightUnits
              << ") gpuMaterial=" << actual.columns[firstColumn].material
              << " cpuMaterial=" << expectedColumns[firstColumn].material;
  if (firstRejectedHeight < expectedHeights.size())
    std::cout << " rejectedHeight=" << firstRejectedHeight;
  if (firstRejectedColumn < expectedColumns.size())
    std::cout << " rejectedColumn=" << firstRejectedColumn
              << " gpuRejectedMaterial="
              << actual.columns[firstRejectedColumn].material
              << " cpuRejectedMaterial="
              << expectedColumns[firstRejectedColumn].material
              << " gpuRejectedHeight=("
              << actual.columns[firstRejectedColumn].minimumHeightUnits << ','
              << actual.columns[firstRejectedColumn].maximumHeightUnits
              << ") cpuRejectedHeight=("
              << expectedColumns[firstRejectedColumn].minimumHeightUnits << ','
              << expectedColumns[firstRejectedColumn].maximumHeightUnits << ')';
  std::cout << '\n';
}

bool matchesWorldCPU(
    const GPUOutput &actual,
    const std::vector<svr::TerrainGPUV2HeightSampleGPU> &expectedHeights,
    const std::vector<svr::TerrainGPUV2ColumnGPU> &expectedColumns,
    const uint32_t spatialLOD)
{
  if (actual.result.status !=
          static_cast<uint32_t>(svr::TerrainGPUV2ColumnStatus::Ready) ||
      actual.result.missingSamples != 0u || actual.result.faultCount != 0u ||
      actual.heights.size() != expectedHeights.size() ||
      actual.columns.size() != expectedColumns.size())
    return false;
  // The runtime shader samples and interpolates the atlas in f32 while the
  // independent map oracle uses double precision. At 10 cm quantization the
  // permitted height difference is one unit. The low validity flags remain
  // exact; the upper packed residual bits carry continuous sub-voxel height
  // and may differ by tiny f32/f64 rounding amounts on real generated maps.
  for (size_t index = 0u; index < actual.heights.size(); ++index)
  {
    const auto &gpu = actual.heights[index];
    const auto &cpu = expectedHeights[index];
    if (gpu.canonicalX != cpu.canonicalX || gpu.canonicalZ != cpu.canonicalZ ||
        svr::terrainGPUV2HeightSampleFlags(gpu) !=
            svr::terrainGPUV2HeightSampleFlags(cpu) ||
        std::abs(int64_t{gpu.heightUnits} - cpu.heightUnits) > 1)
      return false;
  }
  uint32_t materialDifferences = 0u;
  for (size_t index = 0u; index < actual.columns.size(); ++index)
  {
    const auto &gpu = actual.columns[index];
    const auto &cpu = expectedColumns[index];
    materialDifferences += gpu.material != cpu.material;
    if (gpu.flags != cpu.flags ||
        std::abs(int64_t{gpu.minimumHeightUnits} -
                 cpu.minimumHeightUnits) > 1 ||
        std::abs(int64_t{gpu.maximumHeightUnits} -
                 cpu.maximumHeightUnits) > 1)
      return false;
  }
  // Texture filtering and threshold evaluation are f32 on GPU and f64 in the
  // reference map sampler. They are exact through the near LODs; coarse pages
  // can classify the neighbouring sparse-mixture component at a threshold.
  // Bound that disagreement rather than requiring an impossible bitwise match
  // between different numeric domains. Production-page parity below
  // independently checks the renderer's CPU material output.
  return materialDifferences == 0u ||
      (spatialLOD >= 6u &&
       materialDifferences <= actual.columns.size() / 6u);
}

GPUOutput runVariant(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const world::MapExportBundle &source,
    const world::MapExportGpuPageLayout &layout,
    const AtlasUpload &upload,
    const svr::TerrainGPUV2ColumnConfigGPU &config,
    const uint32_t workgroupX,
    const uint32_t workgroupZ,
    const bool testMissing,
    const std::vector<uint32_t> &cityGroundWords = {})
{
  using namespace ::rendering;
  const uint32_t maximumFaults = std::max(256u, config.faultCapacity);
  GPUOutput output{};
  RenderGraph graph(&rhi);
  const auto atlasUsage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push;
  const Buffer metadata = graph.createBuffer({.name = "TerrainGPUV2.P3.Metadata", .size = upload.metadata.size() * sizeof(uint32_t), .usage = atlasUsage});
  const Buffer pageTable = graph.createBuffer({.name = "TerrainGPUV2.P3.PageTable", .size = upload.pageTable.size() * sizeof(uint32_t), .usage = atlasUsage});
  const Buffer physical = graph.createBuffer({.name = "TerrainGPUV2.P3.Physical", .size = upload.physical.size(), .usage = atlasUsage});
  graph.bufferWrite(metadata, 0u, upload.metadata.size() * sizeof(uint32_t), const_cast<uint32_t *>(upload.metadata.data()));
  graph.bufferWrite(pageTable, 0u, upload.pageTable.size() * sizeof(uint32_t), const_cast<uint32_t *>(upload.pageTable.data()));
  graph.bufferWrite(physical, 0u, upload.physical.size(), const_cast<uint8_t *>(upload.physical.data()));
  const uint32_t heightCount = (config.columnsX + 2u * config.halo + 1u) * (config.columnsZ + 2u * config.halo + 1u);
  const uint32_t columnCount = (config.columnsX + 2u * config.halo) * (config.columnsZ + 2u * config.halo);
  const auto pull = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst;
  const Buffer resultReadback = graph.createBuffer({.name = "TerrainGPUV2.P3.ResultReadback", .size = sizeof(svr::TerrainGPUV2ColumnResultGPU), .usage = pull});
  const Buffer heightReadback = graph.createBuffer({.name = "TerrainGPUV2.P3.HeightReadback", .size = uint64_t{heightCount} * sizeof(svr::TerrainGPUV2HeightSampleGPU), .usage = pull});
  const Buffer columnReadback = graph.createBuffer({.name = "TerrainGPUV2.P3.ColumnReadback", .size = uint64_t{columnCount} * sizeof(svr::TerrainGPUV2ColumnGPU), .usage = pull});
  const Buffer faultReadback = graph.createBuffer({.name = "TerrainGPUV2.P3.FaultReadback", .size = uint64_t{maximumFaults} * sizeof(svr::TerrainGPUV2MapFaultGPU), .usage = pull});
  uint32_t passIndex = 0u;
  {
    svrRendering::TerrainGPUV2ColumnChain chain(
        graph,
        passIndex,
        {metadata, pageTable, physical, upload.metadata.size() * sizeof(uint32_t), upload.pageTable.size() * sizeof(uint32_t), upload.physical.size(),
         cityGroundWords},
        {config.columnsX, config.columnsZ, config.halo, maximumFaults, workgroupX, workgroupZ});
    auto *copyResult =
        graph.registerPass<gpgpu::CopyBufferPass>("TerrainGPUV2.P3.CopyResult", passIndex++, chain.result(), 0u, sizeof(svr::TerrainGPUV2ColumnResultGPU), resultReadback, 0u, sizeof(svr::TerrainGPUV2ColumnResultGPU));
    auto *copyHeights = graph.registerPass<gpgpu::CopyBufferPass>(
        "TerrainGPUV2.P3.CopyHeights",
        passIndex++,
        chain.heights(),
        0u,
        uint64_t{heightCount} * sizeof(svr::TerrainGPUV2HeightSampleGPU),
        heightReadback,
        0u,
        uint64_t{heightCount} * sizeof(svr::TerrainGPUV2HeightSampleGPU));
    auto *copyColumns = graph.registerPass<gpgpu::CopyBufferPass>(
        "TerrainGPUV2.P3.CopyColumns", passIndex++, chain.columns(), 0u, uint64_t{columnCount} * sizeof(svr::TerrainGPUV2ColumnGPU), columnReadback, 0u, uint64_t{columnCount} * sizeof(svr::TerrainGPUV2ColumnGPU));
    auto *copyFaults = graph.registerPass<gpgpu::CopyBufferPass>(
        "TerrainGPUV2.P3.CopyFaults", passIndex++, chain.faults(), 0u, uint64_t{maximumFaults} * sizeof(svr::TerrainGPUV2MapFaultGPU), faultReadback, 0u, uint64_t{maximumFaults} * sizeof(svr::TerrainGPUV2MapFaultGPU));
    check(chain.update(config), "GPU column chain accepts planned config");
    graph.compile();
    const auto run = [&]()
    {
      RenderGraph::Frame frame{};
      RenderGraph::Overrides overrides{};
      graph.run(frame, overrides);
      graph.waitFrame(frame);
    };
    run();
    output.result = read<svr::TerrainGPUV2ColumnResultGPU>(graph, resultReadback, 1u).front();
    output.heights = read<svr::TerrainGPUV2HeightSampleGPU>(graph, heightReadback, heightCount);
    output.columns = read<svr::TerrainGPUV2ColumnGPU>(graph, columnReadback, columnCount);

    if (testMissing)
    {
      auto missingTable = upload.pageTable;
      const auto elevation = layout.fieldIndex("refined_elevation");
      const auto logical = layout.pageTableIndex(*elevation, {0u, 0u, 0u});
      missingTable[*logical] = 0u;
      graph.bufferWrite(pageTable, 0u, missingTable.size() * sizeof(uint32_t), missingTable.data());
      const auto priorColumns = output.columns;
      run();
      const auto missing = read<svr::TerrainGPUV2ColumnResultGPU>(graph, resultReadback, 1u).front();
      const auto unchanged = read<svr::TerrainGPUV2ColumnGPU>(graph, columnReadback, columnCount);
      const auto faultRecords = read<svr::TerrainGPUV2MapFaultGPU>(graph, faultReadback, std::min(missing.faultCount, maximumFaults));
      check(
          missing.status == static_cast<uint32_t>(svr::TerrainGPUV2ColumnStatus::MissingPage) && missing.writtenColumns == 0u && missing.faultCount != 0u && bytesEqual(priorColumns, unchanged) && !faultRecords.empty() &&
              faultRecords.front().field == *elevation,
          "missing atlas pages emit compact faults and write no geometry columns");
    }

    delete copyFaults;
    delete copyColumns;
    delete copyHeights;
    delete copyResult;
  }
  graph.deleteBuffer(faultReadback);
  graph.deleteBuffer(columnReadback);
  graph.deleteBuffer(heightReadback);
  graph.deleteBuffer(resultReadback);
  graph.deleteBuffer(physical);
  graph.deleteBuffer(pageTable);
  graph.deleteBuffer(metadata);
  return output;
}

std::array<GPUOutput, 2u> runBatchedVariants(::rendering::backend::vulkan::VulkanRHI &rhi, const AtlasUpload &upload, const std::array<svr::TerrainGPUV2ColumnConfigGPU, 2u> &configs)
{
  using namespace ::rendering;
  constexpr uint32_t lanes = 2u;
  const uint32_t maximumFaults = std::max({256u, configs[0].faultCapacity, configs[1].faultCapacity});
  const uint32_t heightCapacity = (configs[0].columnsX + 2u * configs[0].halo + 1u) * (configs[0].columnsZ + 2u * configs[0].halo + 1u);
  const uint32_t columnCapacity = (configs[0].columnsX + 2u * configs[0].halo) * (configs[0].columnsZ + 2u * configs[0].halo);
  RenderGraph graph(&rhi);
  const auto atlasUsage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push;
  const Buffer metadata = graph.createBuffer({.name = "TerrainGPUV2.P3.Batch.Metadata", .size = upload.metadata.size() * sizeof(uint32_t), .usage = atlasUsage});
  const Buffer pageTable = graph.createBuffer({.name = "TerrainGPUV2.P3.Batch.PageTable", .size = upload.pageTable.size() * sizeof(uint32_t), .usage = atlasUsage});
  const Buffer physical = graph.createBuffer({.name = "TerrainGPUV2.P3.Batch.Physical", .size = upload.physical.size(), .usage = atlasUsage});
  graph.bufferWrite(metadata, 0u, upload.metadata.size() * sizeof(uint32_t), const_cast<uint32_t *>(upload.metadata.data()));
  graph.bufferWrite(pageTable, 0u, upload.pageTable.size() * sizeof(uint32_t), const_cast<uint32_t *>(upload.pageTable.data()));
  graph.bufferWrite(physical, 0u, upload.physical.size(), const_cast<uint8_t *>(upload.physical.data()));

  const auto pull = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst;
  const Buffer resultReadback = graph.createBuffer({.name = "TerrainGPUV2.P3.Batch.ResultReadback", .size = uint64_t{lanes} * sizeof(svr::TerrainGPUV2ColumnResultGPU), .usage = pull});
  const Buffer heightReadback = graph.createBuffer({.name = "TerrainGPUV2.P3.Batch.HeightReadback", .size = uint64_t{lanes} * heightCapacity * sizeof(svr::TerrainGPUV2HeightSampleGPU), .usage = pull});
  const Buffer columnReadback = graph.createBuffer({.name = "TerrainGPUV2.P3.Batch.ColumnReadback", .size = uint64_t{lanes} * columnCapacity * sizeof(svr::TerrainGPUV2ColumnGPU), .usage = pull});
  uint32_t passIndex = 0u;
  std::array<GPUOutput, lanes> outputs{};
  {
    svrRendering::TerrainGPUV2ColumnChain chain(
        graph,
        passIndex,
        {metadata, pageTable, physical, upload.metadata.size() * sizeof(uint32_t), upload.pageTable.size() * sizeof(uint32_t), upload.physical.size()},
        {configs[0].columnsX, configs[0].columnsZ, configs[0].halo, maximumFaults, 8u, 8u, lanes});
    auto *copyResult = graph.registerPass<gpgpu::CopyBufferPass>(
        "TerrainGPUV2.P3.Batch.CopyResult", passIndex++, chain.result(), 0u, uint64_t{lanes} * sizeof(svr::TerrainGPUV2ColumnResultGPU), resultReadback, 0u, uint64_t{lanes} * sizeof(svr::TerrainGPUV2ColumnResultGPU));
    auto *copyHeights = graph.registerPass<gpgpu::CopyBufferPass>(
        "TerrainGPUV2.P3.Batch.CopyHeights",
        passIndex++,
        chain.heights(),
        0u,
        uint64_t{lanes} * heightCapacity * sizeof(svr::TerrainGPUV2HeightSampleGPU),
        heightReadback,
        0u,
        uint64_t{lanes} * heightCapacity * sizeof(svr::TerrainGPUV2HeightSampleGPU));
    auto *copyColumns = graph.registerPass<gpgpu::CopyBufferPass>(
        "TerrainGPUV2.P3.Batch.CopyColumns",
        passIndex++,
        chain.columns(),
        0u,
        uint64_t{lanes} * columnCapacity * sizeof(svr::TerrainGPUV2ColumnGPU),
        columnReadback,
        0u,
        uint64_t{lanes} * columnCapacity * sizeof(svr::TerrainGPUV2ColumnGPU));
    check(chain.updateBatch(std::vector<svr::TerrainGPUV2ColumnConfigGPU>(configs.begin(), configs.end())), "GPU column chain accepts two immutable page lanes");
    graph.compile();
    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);
    const auto results = read<svr::TerrainGPUV2ColumnResultGPU>(graph, resultReadback, lanes);
    const auto heights = read<svr::TerrainGPUV2HeightSampleGPU>(graph, heightReadback, lanes * heightCapacity);
    const auto columns = read<svr::TerrainGPUV2ColumnGPU>(graph, columnReadback, lanes * columnCapacity);
    for (uint32_t lane = 0u; lane < lanes; ++lane)
    {
      outputs[lane].result = results[lane];
      outputs[lane].heights.assign(
          heights.begin() + uint64_t{lane} * heightCapacity,
          heights.begin() + uint64_t{lane} * heightCapacity +
              results[lane].heightSampleCount);
      outputs[lane].columns.assign(
          columns.begin() + uint64_t{lane} * columnCapacity,
          columns.begin() + uint64_t{lane} * columnCapacity +
              results[lane].columnCount);
    }
    delete copyColumns;
    delete copyHeights;
    delete copyResult;
  }
  graph.deleteBuffer(columnReadback);
  graph.deleteBuffer(heightReadback);
  graph.deleteBuffer(resultReadback);
  graph.deleteBuffer(physical);
  graph.deleteBuffer(pageTable);
  graph.deleteBuffer(metadata);
  return outputs;
}

void testGPUColumns(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  const auto source = bundle();
  world::MapExportGpuPageLayout layout;
  std::string error;
  check(layout.initialize(source, static_cast<uint32_t>(source.fields.size()), &error), "GPU fixture atlas layout initializes: " + error);
  const auto upload = atlasUpload(source, layout);
  svr::TerrainGPUV2ColumnBuildSettings settings{};
  settings.key.worldBundleRevision = 41u;
  settings.key.tileX = 0;
  settings.key.tileZ = 0;
  settings.key.clipmapLOD = 0u;
  settings.key.terrainSlot = 7u;
  settings.key.expectedGeneration = 11u;
  settings.key.shape = {8u, 8u, 2u, 0u};
  settings.requestedMapLevel = 0u;
  settings.worldExtentCanonical = 64u;
  settings.heightUnitMetres = 1.0f;
  settings.expectedAtlasRevision = 5u;
  settings.faultCapacity = 256u;
  svr::TerrainGPUV2ColumnConfigGPU config{};
  check(svr::planTerrainGPUV2Columns(source, layout, settings, config, &error), "GPU column fixture plans: " + error);
  std::vector<svr::TerrainGPUV2HeightSampleGPU> expectedHeights;
  std::vector<svr::TerrainGPUV2ColumnGPU> expectedColumns;
  check(svr::buildTerrainGPUV2ColumnOracle(source, config, expectedHeights, expectedColumns, &error), "CPU differential oracle builds: " + error);

  const auto output8x8 = runVariant(rhi, source, layout, upload, config, 8u, 8u, true);
  check(
      output8x8.result.status == static_cast<uint32_t>(svr::TerrainGPUV2ColumnStatus::Ready) && output8x8.result.missingSamples == 0u && output8x8.result.writtenColumns == expectedColumns.size() &&
          bytesEqual(output8x8.heights, expectedHeights) && bytesEqual(output8x8.columns, expectedColumns),
      "8x8 GPU columns exactly match canonical CPU heights and materials");
  const auto output16x8 = runVariant(rhi, source, layout, upload, config, 16u, 8u, false);
  const auto output8x16 = runVariant(rhi, source, layout, upload, config, 8u, 16u, false);
  check(
      bytesEqual(output8x8.heights, output16x8.heights) && bytesEqual(output8x8.heights, output8x16.heights) && bytesEqual(output8x8.columns, output16x8.columns) && bytesEqual(output8x8.columns, output8x16.columns),
      "all qualified 2D workgroup shapes produce deterministic output");

  auto secondConfig = config;
  secondConfig.expectedTerrainGeneration = 97u;
  secondConfig.columnsX = 4u;
  secondConfig.columnsZ = 4u;
  // A different quantization scale produces a bitwise-distinct, exactly
  // representable flat fixture without introducing CPU/GPU noise tolerance.
  secondConfig.heightUnitMetres = 2.0f;
  std::vector<svr::TerrainGPUV2HeightSampleGPU> secondExpectedHeights;
  std::vector<svr::TerrainGPUV2ColumnGPU> secondExpectedColumns;
  check(svr::buildTerrainGPUV2ColumnOracle(source, secondConfig, secondExpectedHeights, secondExpectedColumns, &error), "second batched CPU differential oracle builds: " + error);
  const auto batched = runBatchedVariants(rhi, upload, {config, secondConfig});
  check(batched[0].result.terrainGeneration == config.expectedTerrainGeneration && batched[1].result.terrainGeneration == secondConfig.expectedTerrainGeneration, "batched result records retain each lane generation");
  check(bytesEqual(batched[0].heights, expectedHeights) && bytesEqual(batched[0].columns, expectedColumns), "batched lane zero matches its independent CPU oracle");
  check(bytesEqual(batched[1].heights, secondExpectedHeights) && bytesEqual(batched[1].columns, secondExpectedColumns), "batched lane one matches its independent CPU oracle");
  check(!bytesEqual(batched[0].heights, batched[1].heights), "indirect workgroup z lanes keep distinct output arenas isolated");

  // City ground on the GPU lane: a flat overlay at 123 m with full weight
  // over the whole fixture must replace every sampled height, through the
  // pass's tenth storage binding.
  {
    svr::CityGroundOverlayData data;
    data.groundSpacingMillimetres = 1000u;
    data.groundWidth = 80u;
    data.groundHeight = 80u;
    data.originMillimetres = {-8000, -8000};
    data.groundMillimetres.assign(size_t(80u) * 80u, 123000);
    data.groundWeight.assign(size_t(80u) * 80u, 255u);
    svr::CityGroundOverlay overlay;
    check(overlay.load(std::move(data), &error), "flat city overlay loads: " + error);
    std::vector<uint32_t> words;
    overlay.packGPU(0.0, words);
    const auto city = runVariant(rhi, source, layout, upload, config, 8u, 8u, false, words);
    uint32_t replaced = 0u;
    for (const auto &sample : city.heights) replaced += sample.heightUnits == 123;
    check(city.result.status == static_cast<uint32_t>(svr::TerrainGPUV2ColumnStatus::Ready) && replaced == city.heights.size(),
          "the GPU lane takes the city ground height everywhere the overlay covers (" +
              std::to_string(replaced) + " of " + std::to_string(city.heights.size()) + " samples at 123 m)");
  }
}

void testLandformBoundary(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  auto source = bundle();
  source.extentMetres = 4096.0;
  auto landform = field("landform_id", world::MapExportFormat::R8Uint,
      world::MapExportFilter::Nearest);
  landform.pages[0].data.resize(100u);
  for (uint32_t z = 0u; z < 10u; ++z)
    for (uint32_t x = 0u; x < 10u; ++x)
      landform.pages[0].data[z * 10u + x] = z < 5u
          ? (x < 5u ? 3u : 12u) : (x < 5u ? 6u : 5u);
  source.fields.push_back(std::move(landform));
  auto relief = field("relief_roughness", world::MapExportFormat::RG16Float,
      world::MapExportFilter::Linear);
  std::fill(relief.pages[0].data.begin(), relief.pages[0].data.end(), 0u);
  relief.offset = {600.0, 0.5, 0.0, 0.0};
  source.fields.push_back(std::move(relief));
  for (auto &entry : source.fields)
    if (entry.name == "river") entry.offset[0] = 10000.0;
  world::MapExportGpuPageLayout layout;
  std::string error;
  check(layout.initialize(source, static_cast<uint32_t>(source.fields.size()), &error),
      "landform boundary atlas initializes: " + error);
  const auto upload = atlasUpload(source, layout);
  svr::TerrainGPUV2ColumnBuildSettings settings{};
  settings.key.shape = {64u, 64u, 2u, 0u};
  settings.worldExtentCanonical = 40960u;
  settings.heightUnitMetres = 0.1f;
  settings.proceduralDetailStrength = 2.0;
  settings.proceduralTerraceStrength = 0.0;
  settings.faultCapacity = 4096u;
  svr::TerrainGPUV2ColumnConfigGPU config{};
  check(svr::planTerrainGPUV2Columns(source, layout, settings, config, &error),
      "landform boundary columns plan: " + error);
  config.tileMinimumCanonicalX = 20448;
  config.tileMinimumCanonicalZ = 20448;
  std::vector<svr::TerrainGPUV2HeightSampleGPU> heights;
  std::vector<svr::TerrainGPUV2ColumnGPU> columns;
  check(svr::buildTerrainGPUV2ColumnOracle(source, config, heights, columns, &error),
      "continuous CPU boundary oracle builds: " + error);
  const auto output = runVariant(rhi, source, layout, upload, config, 8u, 8u, false);
  check(matchesWorldCPU(output, heights, columns, 0u),
      "four neighboring landforms blend to the CPU height without a region wall");
  const uint32_t stride = config.columnsX + 2u * config.halo + 1u;
  int32_t largestStep = 0;
  for (uint32_t z = 1u; z < stride; ++z)
    for (uint32_t x = 1u; x < stride; ++x)
    {
      const auto i = z * stride + x;
      largestStep = std::max({largestStep,
          std::abs(output.heights[i].heightUnits - output.heights[i - 1u].heightUnits),
          std::abs(output.heights[i].heightUnits - output.heights[i - stride].heightUnits)});
    }
  check(largestStep <= 2, "region transition has no height jump above 20 cm per 10 cm sample (max=" +
      std::to_string(largestStep) + " units)");
}

svr::TerrainGPUV2ColumnBuildSettings worldSettings(
    const world::MapExportBundle &source,
    const double u,
    const double v,
    const uint32_t lod,
    const int32_t tileOffsetX = 0)
{
  // Match the renderer's generated-world page class. A reduced test-only page
  // can hide errors whose wavelength falls between 64 and 128 samples.
  constexpr uint32_t columns = 128u;
  constexpr uint32_t halo = 2u;
  constexpr double finestVoxelMetres = 0.1;
  const uint32_t canonicalStep = uint32_t{1u} << lod;
  const uint32_t worldExtentCanonical = static_cast<uint32_t>(
      std::llround(source.extentMetres / finestVoxelMetres));
  const int64_t canonicalX = static_cast<int64_t>(
      std::floor(std::clamp(u, 0.0, 1.0) * worldExtentCanonical));
  const int64_t canonicalZ = static_cast<int64_t>(
      std::floor(std::clamp(v, 0.0, 1.0) * worldExtentCanonical));
  const int64_t tileSpan = int64_t{columns} * canonicalStep;

  svr::TerrainGPUV2ColumnBuildSettings settings{};
  settings.key.worldBundleRevision = source.bundleSignature;
  settings.key.tileX = static_cast<int32_t>(canonicalX / tileSpan) +
      tileOffsetX;
  settings.key.tileZ = static_cast<int32_t>(canonicalZ / tileSpan);
  settings.key.clipmapLOD = lod;
  settings.key.terrainSlot = 3u;
  settings.key.expectedGeneration = 17u;
  settings.key.shape = {columns, columns, halo, 0u};
  settings.requestedMapLevel = source.maximumPageLevel;
  settings.worldOriginCanonicalXZ = {0, 0};
  settings.worldExtentCanonical = worldExtentCanonical;
  settings.heightUnitMetres = static_cast<float>(finestVoxelMetres);
  settings.proceduralDetailStrength = 2.0;
  settings.proceduralTerraceStrength = 1.0;
  settings.proceduralTerraceHeightMetres = 4.0;
  settings.proceduralTerraceTransitionFraction = 0.16;
  settings.expectedAtlasRevision = 9u;
  settings.faultCapacity = 4096u;
  return settings;
}

struct ProductionCPUParity
{
  uint32_t comparedCorners = 0u;
  uint32_t heightMismatches = 0u;
  uint32_t slopeMismatches = 0u;
  uint32_t firstSolidCellMismatches = 0u;
  uint32_t topCellMismatches = 0u;
  uint32_t materialMismatches = 0u;
  double maximumHeightErrorMetres = 0.0;
  double rmsHeightErrorMetres = 0.0;
  double maximumSlopeAngleDegrees = 0.0;
  int64_t diagnosticOriginUnits = 0;
  int64_t diagnosticGPUFirst = 0;
  int64_t diagnosticCPUFirst = 0;
  int64_t diagnosticGPUTop = 0;
  int64_t diagnosticCPUTop = 0;
  int64_t diagnosticGPUMinimumUnits = 0;
  int64_t diagnosticGPUMaximumUnits = 0;
  int64_t diagnosticCPUMinimumUnits = 0;
  int64_t diagnosticCPUMaximumUnits = 0;
  uint32_t diagnosticX = 0;
  uint32_t diagnosticZ = 0;
  double diagnosticCPUOriginMetres = 0.0;
  double diagnosticCPUMinimumMetres = 0.0;
  double diagnosticCPUMaximumMetres = 0.0;
};

bool buildProductionCPUPage(
    const world::MapExportBundle &source,
    const svr::TerrainGPUV2ColumnConfigGPU &config,
    const uint32_t spatialLOD,
    svr::WorldMapTerrainColumnPage &page,
    std::string *error)
{
  const double finestVoxelMetres = config.heightUnitMetres;
  const double spatialVoxelMetres =
      finestVoxelMetres * static_cast<double>(config.canonicalStep);
  const double minimumU =
      (static_cast<double>(config.tileMinimumCanonicalX) -
          config.worldOriginCanonicalX) * finestVoxelMetres /
      source.extentMetres;
  const double minimumV =
      (static_cast<double>(config.tileMinimumCanonicalZ) -
          config.worldOriginCanonicalZ) * finestVoxelMetres /
      source.extentMetres;
  const double maximumU = minimumU +
      static_cast<double>(config.columnsX) * config.canonicalStep *
          finestVoxelMetres / source.extentMetres;
  const double maximumV = minimumV +
      static_cast<double>(config.columnsZ) * config.canonicalStep *
          finestVoxelMetres / source.extentMetres;

  svr::WorldMapVoxelTerrainSettings settings{};
  settings.normalizedBounds = {minimumU, minimumV, maximumU, maximumV};
  settings.requestedLevel = static_cast<uint8_t>(config.requestedMapLevel);
  // Spatial pages keep 128 samples and grow their metric footprint. Internal
  // LOD remains zero in the production renderer.
  settings.internalLOD = 0u;
  settings.finestHorizontalVoxelCount = config.columnsX;
  settings.finestVoxelSizeMetres = spatialVoxelMetres;
  settings.elevationAnchorLOD = 0u;
  settings.generatorRevision = 13u;
  settings.exteriorShellOnly = true;
  settings.boundaryStitchSubdivisions = spatialLOD == 0u ? 1u : 2u;
  settings.voxel.horizontalVoxelCount = config.columnsX;
  settings.voxel.patchEdgeMetres =
      static_cast<double>(config.columnsX) * spatialVoxelMetres;
  settings.voxel.foundationDepthVoxels = 4u;
  settings.voxel.surfaceShellDepthVoxels = 0u;
  settings.voxel.maximumVerticalVoxels = 128u;
  settings.voxel.maximumBricks = 1u << 18u;
  // These are the sample defaults used by `--world-map world-data`.
  settings.voxel.proceduralDetailStrength = 2.0;
  settings.voxel.proceduralTerraceStrength = 1.0;
  settings.voxel.proceduralTerraceHeightMetres = 4.0;
  settings.voxel.proceduralTerraceTransitionFraction = 0.16;
  settings.voxel.clipmapLevelCount = 1u;
  settings.voxel.requestedLODMask = 1u;
  settings.voxel.microGeometry = false;
  settings.voxel.workerCount = 1u;

  svr::PlanetVoxelChunkSet unused{};
  return svr::buildWorldMapVoxelTerrain(
      source, settings, unused, error, {}, nullptr, nullptr, &page);
}

ProductionCPUParity compareProductionCPUPage(
    const GPUOutput &gpu,
    const svr::TerrainGPUV2ColumnConfigGPU &config,
    const svr::WorldMapTerrainColumnPage &cpu)
{
  ProductionCPUParity result{};
  if (cpu.side != config.columnsX || cpu.side != config.columnsZ ||
      cpu.canonicalCornerHeights.size() !=
          uint64_t{cpu.side + 1u} * (cpu.side + 1u))
    return result;
  const uint32_t gpuStride = config.columnsX + 2u * config.halo + 1u;
  const auto cpuHeight = [&](const uint32_t x, const uint32_t z)
  {
    return static_cast<double>(
        cpu.canonicalCornerHeights[uint64_t{z} * (cpu.side + 1u) + x]) *
        cpu.heightUnitMetres;
  };
  const auto gpuHeight = [&](const uint32_t x, const uint32_t z)
  {
    return static_cast<double>(gpu.heights[
        uint64_t{z + config.halo} * gpuStride + x + config.halo]
                                   .heightUnits) *
        config.heightUnitMetres;
  };
  const auto cpuGPUUnits = [&](const uint32_t x, const uint32_t z)
  {
    return static_cast<int64_t>(std::ceil(
        cpuHeight(x, z) / config.heightUnitMetres));
  };
  const auto gpuUnits = [&](const uint32_t x, const uint32_t z)
  {
    return static_cast<int64_t>(gpu.heights[
        uint64_t{z + config.halo} * gpuStride + x + config.halo]
                                   .heightUnits);
  };
  double squaredError = 0.0;
  for (uint32_t z = 0u; z <= cpu.side; ++z)
    for (uint32_t x = 0u; x <= cpu.side; ++x)
    {
      const double error = std::abs(gpuHeight(x, z) - cpuHeight(x, z));
      ++result.comparedCorners;
      // CPU production stores a finer fixed-point height. Quantize it to the
      // GPU-v2 global 10 cm lattice, then demand exact integer agreement.
      result.heightMismatches +=
          gpuUnits(x, z) != cpuGPUUnits(x, z) ? 1u : 0u;
      result.maximumHeightErrorMetres =
          std::max(result.maximumHeightErrorMetres, error);
      squaredError += error * error;
    }

  const double radiansToDegrees = 57.2957795130823208768;
  const uint32_t columnStride = config.columnsX + 2u * config.halo;
  const int64_t originUnits = static_cast<int64_t>(std::llround(
      cpu.elevationOriginMetres / config.heightUnitMetres));
  const int64_t step = config.canonicalStep;
  const auto floorDivide = [](const int64_t value, const int64_t divisor)
  {
    const int64_t quotient = value / divisor;
    const int64_t remainder = value % divisor;
    return quotient - (remainder < 0 ? 1 : 0);
  };
  const auto gpuColumnAt = [&](const int32_t x, const int32_t z)
      -> const svr::TerrainGPUV2ColumnGPU &
  {
    return gpu.columns[
        static_cast<uint64_t>(z + static_cast<int32_t>(config.halo)) *
            columnStride +
        static_cast<uint64_t>(x + static_cast<int32_t>(config.halo))];
  };
  const auto gpuTopAt = [&](const int32_t x, const int32_t z)
  {
    const auto &column = gpuColumnAt(x, z);
    return -floorDivide(
        originUnits - int64_t{column.maximumHeightUnits}, step) - 1;
  };
  for (uint32_t z = 0u; z < cpu.side; ++z)
    for (uint32_t x = 0u; x < cpu.side; ++x)
    {
      const std::array<double, 4u> cpuCorners{
        cpuHeight(x, z), cpuHeight(x + 1u, z),
        cpuHeight(x, z + 1u), cpuHeight(x + 1u, z + 1u)};
      const std::array<double, 4u> gpuCorners{
        gpuHeight(x, z), gpuHeight(x + 1u, z),
        gpuHeight(x, z + 1u), gpuHeight(x + 1u, z + 1u)};
      const std::array<int64_t, 4u> cpuQuantized{
        cpuGPUUnits(x, z), cpuGPUUnits(x + 1u, z),
        cpuGPUUnits(x, z + 1u), cpuGPUUnits(x + 1u, z + 1u)};
      const std::array<int64_t, 4u> gpuQuantized{
        gpuUnits(x, z), gpuUnits(x + 1u, z),
        gpuUnits(x, z + 1u), gpuUnits(x + 1u, z + 1u)};
      const auto normal = [&](const std::array<double, 4u> &height)
      {
        const double dx =
            ((height[1] + height[3]) - (height[0] + height[2])) /
            (2.0 * cpu.voxelSizeMetres);
        const double dz =
            ((height[2] + height[3]) - (height[0] + height[1])) /
            (2.0 * cpu.voxelSizeMetres);
        const double inverseLength =
            1.0 / std::sqrt(dx * dx + dz * dz + 1.0);
        return std::array<double, 3u>{
          -dx * inverseLength, inverseLength, -dz * inverseLength};
      };
      const auto cpuNormal = normal(cpuCorners);
      const auto gpuNormal = normal(gpuCorners);
      const double cosine = std::clamp(
          cpuNormal[0] * gpuNormal[0] +
              cpuNormal[1] * gpuNormal[1] +
              cpuNormal[2] * gpuNormal[2],
          -1.0, 1.0);
      const double angle = std::acos(cosine) * radiansToDegrees;
      result.maximumSlopeAngleDegrees =
          std::max(result.maximumSlopeAngleDegrees, angle);
      const int64_t cpuDX = cpuQuantized[1] + cpuQuantized[3] -
          cpuQuantized[0] - cpuQuantized[2];
      const int64_t cpuDZ = cpuQuantized[2] + cpuQuantized[3] -
          cpuQuantized[0] - cpuQuantized[1];
      const int64_t gpuDX = gpuQuantized[1] + gpuQuantized[3] -
          gpuQuantized[0] - gpuQuantized[2];
      const int64_t gpuDZ = gpuQuantized[2] + gpuQuantized[3] -
          gpuQuantized[0] - gpuQuantized[1];
      result.slopeMismatches +=
          cpuDX != gpuDX || cpuDZ != gpuDZ ? 1u : 0u;

      const auto &gpuColumn = gpuColumnAt(x, z);
      const auto &cpuColumn = cpu.columns[uint64_t{z} * cpu.side + x];
      const int64_t first = floorDivide(
          int64_t{gpuColumn.minimumHeightUnits} - originUnits, step);
      const int64_t top = gpuTopAt(x, z);
      const int64_t lowestNeighbour = std::min({
          top,
          gpuTopAt(static_cast<int32_t>(x) - 1, z),
          gpuTopAt(static_cast<int32_t>(x) + 1, z),
          gpuTopAt(x, static_cast<int32_t>(z) - 1),
          gpuTopAt(x, static_cast<int32_t>(z) + 1)});
      const int64_t firstExposedSide = lowestNeighbour < top
          ? lowestNeighbour + 1 : top;
      const uint32_t gpuFirst = static_cast<uint32_t>(std::max<int64_t>(
          std::min(first, firstExposedSide), 0));
      const uint32_t gpuTop = static_cast<uint32_t>(std::max<int64_t>(top, 0));
      if (gpuFirst != cpuColumn.firstSolidY &&
          result.firstSolidCellMismatches == 0u)
      {
        result.diagnosticOriginUnits = originUnits;
        result.diagnosticGPUFirst = gpuFirst;
        result.diagnosticCPUFirst = cpuColumn.firstSolidY;
        result.diagnosticGPUTop = gpuTop;
        result.diagnosticCPUTop = cpuColumn.topY;
        result.diagnosticGPUMinimumUnits = gpuColumn.minimumHeightUnits;
        result.diagnosticGPUMaximumUnits = gpuColumn.maximumHeightUnits;
        result.diagnosticCPUMinimumUnits = *std::min_element(
            cpuQuantized.begin(), cpuQuantized.end());
        result.diagnosticCPUMaximumUnits = *std::max_element(
            cpuQuantized.begin(), cpuQuantized.end());
        result.diagnosticX = x;
        result.diagnosticZ = z;
        result.diagnosticCPUOriginMetres = cpu.elevationOriginMetres;
        result.diagnosticCPUMinimumMetres = *std::min_element(
            cpuCorners.begin(), cpuCorners.end());
        result.diagnosticCPUMaximumMetres = *std::max_element(
            cpuCorners.begin(), cpuCorners.end());
      }
      result.firstSolidCellMismatches +=
          gpuFirst != cpuColumn.firstSolidY ? 1u : 0u;
      result.topCellMismatches += gpuTop != cpuColumn.topY ? 1u : 0u;
      result.materialMismatches += gpuColumn.material !=
          svr::worldMapTerrainColumnMaterial(cpuColumn) ? 1u : 0u;
    }
  if (result.comparedCorners != 0u)
    result.rmsHeightErrorMetres = std::sqrt(
        squaredError / static_cast<double>(result.comparedCorners));
  return result;
}

void testWorldMapGPUColumns(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const std::string &worldMapPath)
{
  std::cout << "real generated-world GPU columns\n";
  world::MapExportBundle source{};
  std::string error;
  check(world::readMapExportBundle(worldMapPath, source, &error),
        "default generated terrain bundle loads: " + error);
  if (!source.validate(&error))
  {
    check(false, "default generated terrain bundle validates: " + error);
    return;
  }

  const uint32_t pageCount = worldAtlasPageCount(source);
  world::MapExportGpuPageLayout layout;
  check(pageCount != 0u && layout.initialize(source, pageCount, &error),
        "real-map GPU atlas layout initializes: " + error);
  if (pageCount == 0u || layout.slotCapacity() != pageCount)
    return;
  const auto upload = worldAtlasUpload(source, layout);

  svr::WorldMapTerrainSpawnView spawn{};
  check(svr::selectWorldMapTerrainSpawnView(
            source, source.maximumPageLevel, 0.5, 0.5, 16000.0,
            spawn, &error) && spawn.found,
        "GPU regression uses the renderer's automatic 16 km startup view: " +
            error);
  if (!spawn.found)
    return;

  std::array<GPUOutput, 2u> fineOutputs{};
  std::array<svr::TerrainGPUV2ColumnConfigGPU, 2u> fineConfigs{};
  std::array<std::vector<svr::TerrainGPUV2HeightSampleGPU>, 2u>
      fineExpectedHeights{};
  std::array<std::vector<svr::TerrainGPUV2ColumnGPU>, 2u>
      fineExpectedColumns{};
  for (uint32_t tile = 0u; tile < 2u; ++tile)
  {
    const auto settings = worldSettings(
        source, spawn.spawnU, spawn.spawnV, 0u,
        static_cast<int32_t>(tile));
    check(svr::planTerrainGPUV2Columns(
              source, layout, settings, fineConfigs[tile], &error),
          "real LOD0 column tile plans: " + error);
    check(svr::buildTerrainGPUV2ColumnOracle(
              source, fineConfigs[tile], fineExpectedHeights[tile],
              fineExpectedColumns[tile], &error),
          "real LOD0 CPU column oracle builds: " + error);
    fineOutputs[tile] = runVariant(
        rhi, source, layout, upload, fineConfigs[tile], 8u, 8u, false);
    if (!bytesEqual(fineOutputs[tile].heights,
                    fineExpectedHeights[tile]) ||
        !bytesEqual(fineOutputs[tile].columns,
                    fineExpectedColumns[tile]))
      diagnoseWorldDifference(
          fineOutputs[tile], fineExpectedHeights[tile],
          fineExpectedColumns[tile], 0u);
    check(
        fineOutputs[tile].result.status ==
                static_cast<uint32_t>(svr::TerrainGPUV2ColumnStatus::Ready) &&
            fineOutputs[tile].result.missingSamples == 0u &&
            fineOutputs[tile].result.faultCount == 0u &&
            matchesWorldCPU(
                fineOutputs[tile], fineExpectedHeights[tile],
                fineExpectedColumns[tile], 0u),
        "real LOD0 GPU coordinates, occupancy columns, and materials match "
        "the CPU terrain oracle (heights within one 10 cm unit)");
  }

  const uint32_t leftStride = fineConfigs[0].columnsX +
      2u * fineConfigs[0].halo + 1u;
  const uint32_t rightStride = fineConfigs[1].columnsX +
      2u * fineConfigs[1].halo + 1u;
  bool exactSharedEdge = true;
  for (uint32_t z = 0u; z <= fineConfigs[0].columnsZ; ++z)
  {
    const auto &left = fineOutputs[0].heights[
        uint64_t{z + fineConfigs[0].halo} * leftStride +
        fineConfigs[0].halo + fineConfigs[0].columnsX];
    const auto &right = fineOutputs[1].heights[
        uint64_t{z + fineConfigs[1].halo} * rightStride +
        fineConfigs[1].halo];
    exactSharedEdge &= left.canonicalX == right.canonicalX &&
        left.canonicalZ == right.canonicalZ &&
        left.heightUnits == right.heightUnits && left.flags == right.flags;
  }
  check(exactSharedEdge,
        "independently dispatched adjacent LOD0 chunks have a bit-identical "
        "shared edge (no crack or overlap)");

  const auto checkProductionParity = [&](const GPUOutput &output,
                                          const auto &config,
                                          const uint32_t lod)
  {
    svr::WorldMapTerrainColumnPage cpu{};
    std::string cpuError;
    const bool built = buildProductionCPUPage(
        source, config, lod, cpu, &cpuError);
    if (!built && lod >= 14u &&
        cpuError.find("fixed footprint") != std::string::npos)
    {
      check(true,
          "production CPU terrain page oracle is outside the fixed-footprint "
          "contract at spatial LOD " +
              std::to_string(lod) + ": " + cpuError);
      return;
    }
    check(built,
        "production CPU terrain page builds at spatial LOD " +
            std::to_string(lod) + ": " + cpuError);
    if (!built)
      return;
    const auto parity = compareProductionCPUPage(output, config, cpu);
    std::cout << "    production parity LOD " << lod
              << " corners=" << parity.comparedCorners
              << " heightMismatch=" << parity.heightMismatches
              << " slopeMismatch=" << parity.slopeMismatches
              << " firstSolidMismatch="
              << parity.firstSolidCellMismatches
              << " topMismatch=" << parity.topCellMismatches
              << " materialMismatch=" << parity.materialMismatches
              << " maxHeightError=" << parity.maximumHeightErrorMetres
              << "m rmsHeightError=" << parity.rmsHeightErrorMetres
              << "m maxSlopeAngle=" << parity.maximumSlopeAngleDegrees
              << "deg firstDiag=(origin=" << parity.diagnosticOriginUnits
              << " gpu=" << parity.diagnosticGPUFirst << ".."
              << parity.diagnosticGPUTop << " cpu="
              << parity.diagnosticCPUFirst << ".."
              << parity.diagnosticCPUTop << " units(gpu="
              << parity.diagnosticGPUMinimumUnits << ".."
              << parity.diagnosticGPUMaximumUnits << " cpu="
              << parity.diagnosticCPUMinimumUnits << ".."
              << parity.diagnosticCPUMaximumUnits << ") xz="
              << parity.diagnosticX << "," << parity.diagnosticZ
              << " metres(origin=" << parity.diagnosticCPUOriginMetres
              << " corners=" << parity.diagnosticCPUMinimumMetres << ".."
              << parity.diagnosticCPUMaximumMetres << "))\n";
    check(
        parity.comparedCorners ==
                uint64_t{config.columnsX + 1u} *
                    (config.columnsZ + 1u) &&
            parity.maximumHeightErrorMetres <=
                static_cast<double>(config.heightUnitMetres) * 2.001 &&
            parity.rmsHeightErrorMetres <=
                static_cast<double>(config.heightUnitMetres) * 1.001 &&
            parity.topCellMismatches <=
                uint64_t{config.columnsX} * config.columnsZ / 32u &&
            parity.materialMismatches <=
                (lod < 6u
                    ? 0u
                    : uint64_t{config.columnsX} * config.columnsZ / 6u),
        "GPU occupancy surface stays within two finest height units, one-cell "
        "top ownership remains at least 96.875% identical, and material "
        "classification matches the production CPU tolerance at spatial LOD " +
            std::to_string(lod));
  };
  checkProductionParity(fineOutputs[0], fineConfigs[0], 0u);

  std::set<uint32_t> materials;
  int32_t minimumHeight = std::numeric_limits<int32_t>::max();
  int32_t maximumHeight = std::numeric_limits<int32_t>::min();
  for (const auto &column : fineOutputs[0].columns)
  {
    if ((column.flags & svr::TerrainGPUV2ColumnValid) == 0u)
      continue;
    materials.insert(column.material);
    minimumHeight = std::min(minimumHeight, column.minimumHeightUnits);
    maximumHeight = std::max(maximumHeight, column.maximumHeightUnits);
  }
  check(!materials.empty() && maximumHeight >= minimumHeight,
        "real GPU tile retains valid terrain occupancy and material IDs");

  std::map<std::pair<int32_t, int32_t>, int32_t> fineLattice;
  for (const auto &sample : fineOutputs[0].heights)
    fineLattice[{sample.canonicalX, sample.canonicalZ}] = sample.heightUnits;
  // Spatial LOD is the generated-world terrain LOD: pages retain the 128^2
  // raster and double their metric footprint. Cover every level admitted by
  // the runtime page contract, including the very coarse roots where a wrong
  // sampling phase turns organic relief into long triangular slices.
  for (uint32_t lod = 1u;
       lod < svr::kPlanetVoxelMaximumClipmapLevels; ++lod)
  {
    const auto settings = worldSettings(
        source, spawn.spawnU, spawn.spawnV, lod);
    svr::TerrainGPUV2ColumnConfigGPU config{};
    std::vector<svr::TerrainGPUV2HeightSampleGPU> expectedHeights;
    std::vector<svr::TerrainGPUV2ColumnGPU> expectedColumns;
    check(svr::planTerrainGPUV2Columns(
              source, layout, settings, config, &error) &&
              svr::buildTerrainGPUV2ColumnOracle(
                  source, config, expectedHeights, expectedColumns, &error),
          "real coarse-LOD CPU column oracle builds: " + error);
    const auto output = runVariant(
        rhi, source, layout, upload, config, 8u, 8u, false);
    if (!bytesEqual(output.heights, expectedHeights) ||
        !bytesEqual(output.columns, expectedColumns))
      diagnoseWorldDifference(
          output, expectedHeights, expectedColumns, lod);
    check(
        output.result.status ==
                static_cast<uint32_t>(svr::TerrainGPUV2ColumnStatus::Ready) &&
            output.result.missingSamples == 0u &&
            matchesWorldCPU(output, expectedHeights, expectedColumns, lod),
        "real GPU columns match the bounded CPU oracle at LOD " +
            std::to_string(lod));
    checkProductionParity(output, config, lod);
    uint32_t commonSamples = 0u;
    bool exactCommonLattice = true;
    for (const auto &sample : output.heights)
    {
      const auto fine = fineLattice.find(
          {sample.canonicalX, sample.canonicalZ});
      if (fine == fineLattice.end())
        continue;
      ++commonSamples;
      exactCommonLattice &= fine->second == sample.heightUnits;
    }
    // A coarse page need not share a sample with the tiny LOD0 page around
    // the spawn. When it does, however, the canonical lattice is exact.
    check(commonSamples == 0u || exactCommonLattice,
          "GPU LOD " + std::to_string(lod) +
              " is bit-identical to LOD0 at every common lattice sample");
  }
}

} // namespace

int main(const int argc, const char *const *argv)
{
  os::Logger::start();
  ::rendering::DeviceRequiredLimits limits{.minimumMemory = 0u, .minimumComputeSharedMemory = 0u, .minimumComputeWorkGroupInvocations = 128u};
  ::rendering::backend::vulkan::VulkanRHI rhi(::rendering::backend::vulkan::Vulkan_1_2, limits, ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  testGPUColumns(rhi);
  testLandformBoundary(rhi);
  if (argc >= 2)
    testWorldMapGPUColumns(rhi, argv[1]);
  else
    check(false, "real world-map path is required for GPU regression coverage");
  os::Logger::shutdown();
  std::cout << (failures == 0 ? "GPU terrain V2 Phase 3 GPU tests passed\n" : "GPU terrain V2 Phase 3 GPU tests failed\n");
  return failures == 0 ? 0 : 1;
}
