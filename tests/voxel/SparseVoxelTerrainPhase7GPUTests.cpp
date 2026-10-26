#include "os/Logger.hpp"
#include "procedural/world/MapExport.hpp"
#include "procedural/world/MapExportGpuCache.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/TerrainGPUColumnBuild.hpp"
#include "voxel/svraster/rendering/TerrainGPUColumnBuildChain.hpp"
#include "voxel/svraster/rendering/TerrainGPUPageBuildChain.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#undef NDEBUG
#include <cassert>

namespace
{
using namespace ::rendering;
namespace world = procedural::world;
namespace svr = voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;

world::MapExportField field(const char *name, const world::MapExportFormat format, const world::MapExportFilter filter, const char *group = "")
{
  world::MapExportField result{};
  result.name = name;
  result.format = format;
  result.filter = filter;
  result.available = true;
  result.width = 4u;
  result.height = 4u;
  result.samplingGroup = group;
  return result;
}

world::MapExportBundle bundle()
{
  world::MapExportBundle result{};
  result.resolution = 2u;
  result.pageSizePixels = 2u;
  result.gutterPixels = 1u;
  result.maximumPageLevel = 0u;
  result.extentMetres = 16.0;
  result.seed = 7u;
  result.bundleSignature = 11u;
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
  result.field("refined_elevation")->offset[0] = 320.0;
  return result;
}

struct AtlasUpload
{
  std::vector<uint32_t> metadata;
  std::vector<uint32_t> pageTable;
  std::vector<uint8_t> physical;
};

AtlasUpload atlasUpload(const world::MapExportGpuPageLayout &layout)
{
  AtlasUpload result{};
  const size_t metadataBytes = sizeof(world::MapExportGpuHeader) + layout.descriptors().size() * sizeof(world::MapExportGpuFieldDescriptor);
  result.metadata.resize(metadataBytes / sizeof(uint32_t));
  std::memcpy(result.metadata.data(), &layout.header(), sizeof(world::MapExportGpuHeader));
  std::memcpy(result.metadata.data() + sizeof(world::MapExportGpuHeader) / sizeof(uint32_t), layout.descriptors().data(), layout.descriptors().size() * sizeof(world::MapExportGpuFieldDescriptor));
  result.pageTable.resize(static_cast<size_t>(layout.entriesPerField()) * layout.fields().size());
  result.physical.resize(layout.physicalBufferBytes());
  for (uint32_t index = 0u; index < layout.fields().size(); ++index)
  {
    result.pageTable[layout.fields()[index].pageTableOffset] = index + 1u;
    uint8_t *slot = result.physical.data() + static_cast<size_t>(index) * layout.physicalSlotBytes();
    const auto &field = layout.fields()[index];
    for (uint32_t texel = 0u; texel < 16u; ++texel)
    {
      uint8_t *destination = slot + static_cast<size_t>(texel) * field.texelBytes;
      if (field.name == "substrate_id")
        destination[0] = 3u;
      if (field.name == "soil_depth")
      {
        destination[0] = 0u;
        destination[1] = 0x3cu;
      }
      if (field.name == "biome_mix_ids")
        destination[0] = 13u;
      if (field.name == "biome_mix_weights")
        destination[0] = 255u;
    }
  }
  return result;
}

template <typename T> std::vector<T> read(RenderGraph &graph, const Buffer buffer, const size_t count)
{
  std::vector<T> result(count);
  graph.bufferRead(
      buffer,
      0u,
      result.size() * sizeof(T),
      [&](const void *data)
      {
        std::memcpy(result.data(), data, result.size() * sizeof(T));
      });
  return result;
}

void testAtlasToColumns(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  const auto source = bundle();
  world::MapExportGpuPageLayout layout;
  std::string error;
  assert(layout.initialize(source, static_cast<uint32_t>(source.fields.size()), &error));
  auto upload = atlasUpload(layout);
  svr::TerrainGPUColumnBuildSettings settings{};
  settings.side = 8u;
  settings.halo = 2u;
  settings.maximumVerticalVoxels = 4096u;
  settings.finestVoxelSizeMetres = 2.0;
  settings.elevationOriginMetres = -100.0;
  settings.proceduralDetailStrength = 0.0;
  settings.proceduralTerraceStrength = 0.0;
  settings.expectedTerrainGeneration = 17u;
  settings.expectedAtlasRevision = 23u;
  svr::TerrainGPUColumnConfigGPU config{};
  assert(svr::planTerrainGPUColumnBuild(source, layout, settings, config, &error));

  RenderGraph graph(&rhi);
  const auto storage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push;
  const Buffer metadata = graph.createBuffer({
    .name = "Phase7.Atlas.Metadata",
    .size = upload.metadata.size() * sizeof(uint32_t),
    .usage = storage,
  });
  const Buffer pageTable = graph.createBuffer({
    .name = "Phase7.Atlas.PageTable",
    .size = upload.pageTable.size() * sizeof(uint32_t),
    .usage = storage,
  });
  const Buffer physical = graph.createBuffer({
    .name = "Phase7.Atlas.Physical",
    .size = upload.physical.size(),
    .usage = storage,
  });
  graph.bufferWrite(metadata, 0u, upload.metadata.size() * sizeof(uint32_t), upload.metadata.data());
  graph.bufferWrite(pageTable, 0u, upload.pageTable.size() * sizeof(uint32_t), upload.pageTable.data());
  graph.bufferWrite(physical, 0u, upload.physical.size(), upload.physical.data());
  const auto pull = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst;
  const Buffer resultReadback = graph.createBuffer({
    .name = "Phase7.Result.Readback",
    .size = sizeof(svr::TerrainGPUColumnResultGPU),
    .usage = pull,
  });
  const Buffer columnReadback = graph.createBuffer({
    .name = "Phase7.Columns.Readback",
    .size = 64u * sizeof(svr::WorldMapTerrainColumnGPU),
    .usage = pull,
  });
  uint32_t passIndex = 0u;
  {
    svrRendering::TerrainGPUColumnBuildChain chain(
        graph, passIndex, {metadata, pageTable, physical, upload.metadata.size() * sizeof(uint32_t), upload.pageTable.size() * sizeof(uint32_t), upload.physical.size()}, 8u, 2u);
    auto *copyResult =
        graph.registerPass<::rendering::gpgpu::CopyBufferPass>("Phase7.Copy.Result", passIndex++, chain.result(), 0u, sizeof(svr::TerrainGPUColumnResultGPU), resultReadback, 0u, sizeof(svr::TerrainGPUColumnResultGPU));
    auto *copyColumns = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Phase7.Copy.Columns", passIndex++, chain.columns(), 0u, 64u * sizeof(svr::WorldMapTerrainColumnGPU), columnReadback, 0u, 64u * sizeof(svr::WorldMapTerrainColumnGPU));
    graph.compile();
    assert(chain.update(config));
    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);
    const auto result = read<svr::TerrainGPUColumnResultGPU>(graph, resultReadback, 1u).front();
    assert(result.status == static_cast<uint32_t>(svr::TerrainGPUColumnStatus::Ready));
    assert(result.missingSamples == 0u && result.verticalOverflows == 0u && result.columnCount == 64u);
    const auto columns = read<svr::WorldMapTerrainColumnGPU>(graph, columnReadback, 64u);
    for (const auto &column : columns)
    {
      assert(column.topY == 209u && column.firstSolidY == 209u);
      assert(svr::worldMapTerrainColumnMaterial(column) == 13u);
    }
    delete copyColumns;
    delete copyResult;
  }
  graph.deleteBuffer(columnReadback);
  graph.deleteBuffer(resultReadback);
  graph.deleteBuffer(physical);
  graph.deleteBuffer(pageTable);
  graph.deleteBuffer(metadata);
}

void testAtlasToPublishedPage(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  const auto source = bundle();
  world::MapExportGpuPageLayout layout;
  std::string error;
  assert(layout.initialize(
      source, static_cast<uint32_t>(source.fields.size()), &error));
  auto upload = atlasUpload(layout);

  constexpr uint32_t side = 8u;
  constexpr uint32_t maximumVerticalVoxels = 4096u;
  constexpr uint32_t terrainGeneration = 17u;
  constexpr uint64_t geometryRevision =
      svr::kTerrainGPUColumnGeneratorRevision;
  const svr::TerrainGPURasterCapacity rasterCapacity{
    8192u, 2048u, 4096u, 8192u, 64u};
  const svr::TerrainGPUTree64Capacity treeCapacity{8192u, 2048u};

  svr::TerrainGPUColumnBuildSettings columnSettings{};
  columnSettings.side = side;
  columnSettings.halo = 2u;
  columnSettings.maximumVerticalVoxels = maximumVerticalVoxels;
  columnSettings.finestVoxelSizeMetres = 2.0;
  columnSettings.elevationOriginMetres = -100.0;
  columnSettings.proceduralDetailStrength = 0.0;
  columnSettings.proceduralTerraceStrength = 0.0;
  columnSettings.expectedTerrainGeneration = terrainGeneration;
  columnSettings.expectedAtlasRevision = 23u;
  svr::TerrainGPUColumnConfigGPU columnConfig{};
  assert(svr::planTerrainGPUColumnBuild(
      source, layout, columnSettings, columnConfig, &error));

  svr::TerrainGPURasterConfigGPU rasterConfig{};
  const svr::TerrainGPURasterPageDescription rasterPage{
    side, maximumVerticalVoxels, columnConfig.voxelSizeMetres,
    columnConfig.edgeMetres};
  assert(svr::planTerrainGPURasterBuild(
      rasterPage, rasterCapacity, terrainGeneration, geometryRevision,
      1u, rasterConfig, &error));
  svr::TerrainGPUTree64ConfigGPU treeConfig{};
  const svr::TerrainGPUTree64PageDescription treePage{
    side, maximumVerticalVoxels, 0u, columnConfig.voxelSizeMetres,
    columnConfig.edgeMetres};
  assert(svr::planTerrainGPUTree64Build(
      treePage, treeCapacity, terrainGeneration, geometryRevision, 0u, 0u,
      1u, treeConfig, &error));

  std::array<svr::TerrainGPURasterMaterialGPU,
      svr::kTerrainGPURasterMaterialTableSize> materialTable{};
  for (auto &entry : materialTable) entry.paletteEntry = 0xffffffffu;
  const auto packed = svr::packSparseVoxelBrickMaterial(
      svr::planetVoxelBaseMaterial(13u));
  materialTable[13u] = {0u, packed.packed[0], packed.packed[1], 0u};

  const svr::TerrainGPUJointPublicationCapacities publicationCapacity{
    .requests = 1u,
    .rasterResults = 1u,
    .rasterNodes = rasterCapacity.nodes,
    .rasterChildren = rasterCapacity.children,
    .rasterMaterials = rasterCapacity.materials,
    .rasterRoots = rasterCapacity.roots,
    .tree64Results = 1u,
    .tree64Nodes = treeCapacity.nodes,
    .destinationNodes = rasterCapacity.nodes,
    .destinationChildren = rasterCapacity.children,
    .destinationMaterials = rasterCapacity.materials,
    .destinationRoots = rasterCapacity.roots,
    .destinationTree64Nodes = treeCapacity.nodes,
    .destinationTree64Instances = 1u,
  };

  RenderGraph graph(&rhi);
  const auto input = BufferUsage::BufferUsage_Storage |
      BufferUsage::BufferUsage_Push;
  const auto output = BufferUsage::BufferUsage_Storage |
      BufferUsage::BufferUsage_CopySrc;
  const auto makeBuffer = [&](const char *name, const uint64_t bytes,
                              const BufferUsage usage)
  {
    return graph.createBuffer({.name = name, .size = bytes, .usage = usage});
  };
  const Buffer metadata = makeBuffer(
      "Phase7.Full.Atlas.Metadata",
      upload.metadata.size() * sizeof(uint32_t), input);
  const Buffer pageTable = makeBuffer(
      "Phase7.Full.Atlas.PageTable",
      upload.pageTable.size() * sizeof(uint32_t), input);
  const Buffer physical = makeBuffer(
      "Phase7.Full.Atlas.Physical", upload.physical.size(), input);
  graph.bufferWrite(metadata, 0u,
      upload.metadata.size() * sizeof(uint32_t), upload.metadata.data());
  graph.bufferWrite(pageTable, 0u,
      upload.pageTable.size() * sizeof(uint32_t), upload.pageTable.data());
  graph.bufferWrite(
      physical, 0u, upload.physical.size(), upload.physical.data());

  const Buffer nodes = makeBuffer("Phase7.Full.Output.Nodes",
      uint64_t{rasterCapacity.nodes} *
          sizeof(svr::SparseVoxelBrickNodeGPU), output);
  const Buffer occupancy = makeBuffer("Phase7.Full.Output.Occupancy",
      uint64_t{rasterCapacity.nodes} *
          sizeof(svr::SparseVoxelBrickOccupancyGPU), output);
  const Buffer materials = makeBuffer("Phase7.Full.Output.Materials",
      ((uint64_t{rasterCapacity.materials} + 3u) / 4u) *
          sizeof(uint32_t), output);
  const Buffer micro = makeBuffer("Phase7.Full.Output.Micro",
      uint64_t{rasterCapacity.materials} * sizeof(uint64_t), output);
  const Buffer children = makeBuffer("Phase7.Full.Output.Children",
      uint64_t{rasterCapacity.children} * sizeof(uint32_t), output);
  const Buffer roots = makeBuffer("Phase7.Full.Output.Roots",
      uint64_t{rasterCapacity.roots} * sizeof(uint32_t), output);
  const Buffer treeNodes = makeBuffer("Phase7.Full.Output.Tree64",
      uint64_t{treeCapacity.nodes} *
          sizeof(svr::SparseVoxelTree64NodeGPU), output);
  const Buffer treeNodesPadded = makeBuffer(
      "Phase7.Full.Output.Tree64Padded",
      uint64_t{treeCapacity.nodes} *
          sizeof(svr::SparseVoxelTree64NodePaddedGPU), output);
  const Buffer treeInstances = makeBuffer(
      "Phase7.Full.Output.Tree64Instances",
      sizeof(svr::SparseVoxelTree64DynamicInstanceGPU), output);
  const Buffer lodGroups = makeBuffer("Phase7.Full.Output.LODGroups",
      uint64_t{rasterCapacity.nodes} *
          sizeof(svr::SparseVoxelBrickLODGroupGPU), output);
  const Buffer nodeLODGroupIndices = makeBuffer(
      "Phase7.Full.Output.NodeLODGroupIndices",
      uint64_t{rasterCapacity.nodes} * sizeof(uint32_t), output);
  const Buffer representativeLODGroupIndices = makeBuffer(
      "Phase7.Full.Output.RepresentativeLODGroupIndices",
      uint64_t{rasterCapacity.nodes} * sizeof(uint32_t), output);
  const auto pull = BufferUsage::BufferUsage_Pull |
      BufferUsage::BufferUsage_CopyDst;
  const Buffer resultReadback = makeBuffer("Phase7.Full.Result.Readback",
      sizeof(svr::TerrainGPUJointPublicationResultGPU), pull);
  const Buffer recordReadback = makeBuffer("Phase7.Full.Record.Readback",
      sizeof(svr::SparseVoxelTerrainPublicationRecordGPU), pull);
  const Buffer columnResultReadback = makeBuffer(
      "Phase7.Full.ColumnResult.Readback",
      sizeof(svr::TerrainGPUColumnResultGPU), pull);
  const Buffer rasterResultReadback = makeBuffer(
      "Phase7.Full.RasterResult.Readback",
      sizeof(svr::TerrainGPURasterResultGPU), pull);
  const Buffer treeResultReadback = makeBuffer(
      "Phase7.Full.TreeResult.Readback",
      sizeof(svr::TerrainGPUTree64ResultGPU), pull);
  const Buffer treeInstanceReadback = makeBuffer(
      "Phase7.Full.TreeInstance.Readback",
      sizeof(svr::SparseVoxelTree64DynamicInstanceGPU), pull);
  const Buffer nodeReadback = makeBuffer(
      "Phase7.Full.Node.Readback",
      uint64_t{rasterCapacity.nodes} *
          sizeof(svr::SparseVoxelBrickNodeGPU), pull);
  const Buffer lodGroupReadback = makeBuffer(
      "Phase7.Full.LODGroup.Readback",
      uint64_t{rasterCapacity.nodes} *
          sizeof(svr::SparseVoxelBrickLODGroupGPU), pull);
  const Buffer microReadback = makeBuffer(
      "Phase7.Full.Micro.Readback", sizeof(uint64_t), pull);

  uint32_t passIndex = 0u;
  {
    svrRendering::TerrainGPUPageBuildChain chain(
        graph, passIndex,
        {metadata, pageTable, physical,
         upload.metadata.size() * sizeof(uint32_t),
         upload.pageTable.size() * sizeof(uint32_t), upload.physical.size()},
        {side, 2u, rasterCapacity, treeCapacity, publicationCapacity},
        {nodes, occupancy, materials, micro, children, roots, treeNodes,
         treeNodesPadded, treeInstances, lodGroups, nodeLODGroupIndices,
         representativeLODGroupIndices});
    auto *copyResult = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Phase7.Full.Copy.Result", passIndex++, chain.publicationResults(),
        0u, sizeof(svr::TerrainGPUJointPublicationResultGPU), resultReadback,
        0u, sizeof(svr::TerrainGPUJointPublicationResultGPU));
    auto *copyRecord = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Phase7.Full.Copy.Record", passIndex++, chain.publicationRecords(),
        0u, sizeof(svr::SparseVoxelTerrainPublicationRecordGPU),
        recordReadback, 0u,
        sizeof(svr::SparseVoxelTerrainPublicationRecordGPU));
    auto *copyColumn = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Phase7.Full.Copy.Column", passIndex++, chain.columnResults(), 0u,
        sizeof(svr::TerrainGPUColumnResultGPU), columnResultReadback, 0u,
        sizeof(svr::TerrainGPUColumnResultGPU));
    auto *copyRaster = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Phase7.Full.Copy.Raster", passIndex++, chain.rasterResults(), 0u,
        sizeof(svr::TerrainGPURasterResultGPU), rasterResultReadback, 0u,
        sizeof(svr::TerrainGPURasterResultGPU));
    auto *copyTree = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Phase7.Full.Copy.Tree", passIndex++, chain.tree64Results(), 0u,
        sizeof(svr::TerrainGPUTree64ResultGPU), treeResultReadback, 0u,
        sizeof(svr::TerrainGPUTree64ResultGPU));
    auto *copyTreeInstance =
        graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
            "Phase7.Full.Copy.TreeInstance", passIndex++, treeInstances, 0u,
            sizeof(svr::SparseVoxelTree64DynamicInstanceGPU),
            treeInstanceReadback, 0u,
            sizeof(svr::SparseVoxelTree64DynamicInstanceGPU));
    auto *copyNode = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Phase7.Full.Copy.Node", passIndex++, nodes, 0u,
        uint64_t{rasterCapacity.nodes} *
            sizeof(svr::SparseVoxelBrickNodeGPU),
        nodeReadback, 0u,
        uint64_t{rasterCapacity.nodes} *
            sizeof(svr::SparseVoxelBrickNodeGPU));
    auto *copyLODGroup =
        graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
            "Phase7.Full.Copy.LODGroup", passIndex++, lodGroups, 0u,
            uint64_t{rasterCapacity.nodes} *
                sizeof(svr::SparseVoxelBrickLODGroupGPU),
            lodGroupReadback, 0u,
            uint64_t{rasterCapacity.nodes} *
                sizeof(svr::SparseVoxelBrickLODGroupGPU));
    auto *copyMicro = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        "Phase7.Full.Copy.Micro", passIndex++, micro, 0u,
        sizeof(uint64_t), microReadback, 0u, sizeof(uint64_t));

    svr::TerrainGPUJointPublicationRequestGPU publication{};
    publication.terrainSlot = 0u;
    publication.publishedFrame = 31u;
    publication.nodeCapacity = rasterCapacity.nodes;
    publication.childCapacity = rasterCapacity.children;
    publication.rootCapacity = rasterCapacity.roots;
    publication.materialCapacity = rasterCapacity.materials;
    publication.paletteCapacity = 1u;
    publication.tree64NodesPerPage = treeCapacity.nodes;
    publication.nodePageGeneration = 1u;
    publication.materialPageGeneration = 1u;
    static_assert(sizeof(float) == sizeof(uint32_t));
    std::memcpy(
        &publication.receiverCellSizeBits,
        &columnConfig.voxelSizeMetres,
        sizeof(publication.receiverCellSizeBits));
    publication.translation[3] = columnConfig.edgeMetres;
    publication.pageSide = side;
    publication.finerClipPacked =
        (side << 16u) | (side << 24u);
    publication.forceLeafRaster =
        svr::kTerrainGPUJointSourceWorldSpace |
        svr::kTerrainGPUJointForceLeafRaster;

    svrRendering::TerrainGPUPageBuildChain::Update update{};
    update.columns = columnConfig;
    update.raster = rasterConfig;
    update.tree64 = treeConfig;
    update.materialTable = materialTable;
    update.publications = {publication};
    update.stages.publish = true;
    assert(update.stages.publish && update.publications.size() == 1u);
    assert(chain.update(update, &error));
    graph.compile();
    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);

    const auto result =
        read<svr::TerrainGPUJointPublicationResultGPU>(
            graph, resultReadback, 1u).front();
    const auto record = read<svr::SparseVoxelTerrainPublicationRecordGPU>(
        graph, recordReadback, 1u).front();
    const auto columnResult = read<svr::TerrainGPUColumnResultGPU>(
        graph, columnResultReadback, 1u).front();
    const auto rasterResult = read<svr::TerrainGPURasterResultGPU>(
        graph, rasterResultReadback, 1u).front();
    const auto treeResult = read<svr::TerrainGPUTree64ResultGPU>(
        graph, treeResultReadback, 1u).front();
    const auto treeInstance =
        read<svr::SparseVoxelTree64DynamicInstanceGPU>(
            graph, treeInstanceReadback, 1u).front();
    const auto publishedNodes = read<svr::SparseVoxelBrickNodeGPU>(
        graph, nodeReadback, rasterCapacity.nodes);
    const auto publishedLODGroups =
        read<svr::SparseVoxelBrickLODGroupGPU>(
            graph, lodGroupReadback, rasterCapacity.nodes);
    const auto &firstNode = publishedNodes.front();
    const auto firstMicro = read<uint64_t>(
        graph, microReadback, 1u).front();
    if (result.status != static_cast<uint32_t>(
            svr::TerrainGPUJointPublicationStatus::Approved))
      std::cerr << "full GPU publication rejected: reason="
                << result.rejectionReason << " generation="
                << result.terrainGeneration << " rasterNodes="
                << result.rasterNodeCount << " treeNodes="
                << result.tree64NodeCount << " occupied="
                << result.occupiedVoxelCount << '\n';
    if (result.status == 0u)
      std::cerr << "stage status: columns=" << columnResult.status
                << " raster=" << rasterResult.status
                << " rasterOverflow=" << rasterResult.overflowStage
                << " tree=" << treeResult.status
                << " treeOverflow=" << treeResult.overflowStage << '\n';
    assert(result.status == static_cast<uint32_t>(
        svr::TerrainGPUJointPublicationStatus::Approved));
    assert(result.terrainGeneration == terrainGeneration);
    assert(result.rasterNodeCount != 0u && result.tree64NodeCount != 0u);
    assert(result.occupiedVoxelCount == side * side);
    assert(record.flags ==
        (svr::kSparseVoxelTerrainPublicationValid |
         svr::kSparseVoxelTerrainPublicationDynamicInstance));
    assert(record.replacementPageState[0] == terrainGeneration);
    assert(record.replacementTree64.terrainGeneration == terrainGeneration);
    assert(record.replacementTree64.nodeCount == result.tree64NodeCount);
    assert((treeInstance.flags & svr::kSparseVoxelTree64DynamicValid) != 0u);
    assert((treeInstance.flags &
            svr::kSparseVoxelTree64DynamicSkipReceiverCell) != 0u);
    assert((treeInstance.flags &
            svr::kSparseVoxelTree64DynamicTranslationOnly) != 0u);
    assert(result.rasterNodeCount <= publishedNodes.size());
    bool foundCertifiedSurfaceRepresentative = false;
    bool certifiedPageEdgeRepresentative = false;
    assert(std::all_of(
        publishedNodes.begin(),
        publishedNodes.begin() + result.rasterNodeCount,
        [](const svr::SparseVoxelBrickNodeGPU &node)
        {
          return (node.level & svr::kSparseVoxelBrickForceLeafRaster) != 0u;
        }));
    for (uint32_t nodeIndex = 0u; nodeIndex < result.rasterNodeCount;
         ++nodeIndex)
    {
      const auto &node = publishedNodes[nodeIndex];
      const auto &group = publishedLODGroups[nodeIndex];
      if (node.childCount == 8u &&
          group.representativeNode == nodeIndex &&
          group.nodeCount == 8u &&
          (group.flags &
           svr::kSparseVoxelBrickLODGroupSurfaceRepresentativeCertified) != 0u)
      {
        foundCertifiedSurfaceRepresentative = true;
        certifiedPageEdgeRepresentative =
            certifiedPageEdgeRepresentative ||
            (group.flags & svr::kSparseVoxelBrickLODGroupBoundaryMask) != 0u;
      }
    }
    assert(foundCertifiedSurfaceRepresentative);
    assert(!certifiedPageEdgeRepresentative);
    assert(treeInstance.terrainGeneration == terrainGeneration);
    assert(treeInstance.receiverCellSizeBits ==
        publication.receiverCellSizeBits);
    assert(svr::sparseVoxelBrickHasFinerTerrainClip(firstNode));
    assert(static_cast<uint32_t>(firstMicro >> 56u) == 1u);
    const auto finerBounds =
        svr::sparseVoxelBrickFinerTerrainClipBounds(firstNode);
    assert(finerBounds[0] < finerBounds[2] &&
        finerBounds[1] < finerBounds[3]);

    // Runtime streaming builds an entire spatial ownership group into hidden
    // arenas, then installs every child and removes its parent in one metadata
    // dispatch. The build publication must therefore be an exact metadata
    // no-op even though raster and Tree64 payload relocation still ran.
    publication.forceLeafRaster =
        svr::kTerrainGPUJointForceLeafRaster |
        svr::kTerrainGPUJointDeferVisibility;
    publication.expectedPageState = {91u, 3u, 17u,
        svr::kSparseVoxelTerrainPageStateProgressiveGenerated};
    publication.expectedTree64 = record.replacementTree64;
    publication.expectedTree64.terrainGeneration = 91u;
    publication.expectedTree64.internalLOD = 3u;
    update.publications = {publication};
    assert(chain.update(update, &error));
    RenderGraph::Frame deferredFrame{};
    graph.run(deferredFrame, overrides);
    graph.waitFrame(deferredFrame);
    const auto deferredRecord =
        read<svr::SparseVoxelTerrainPublicationRecordGPU>(
            graph, recordReadback, 1u).front();
    assert(deferredRecord.flags ==
        svr::kSparseVoxelTerrainPublicationValid);
    assert(deferredRecord.replacementPageState ==
        publication.expectedPageState);
    assert(std::memcmp(
        &deferredRecord.replacementTree64,
        &publication.expectedTree64,
        sizeof(publication.expectedTree64)) == 0);
    const auto hiddenTreeInstance =
        read<svr::SparseVoxelTree64DynamicInstanceGPU>(
            graph, treeInstanceReadback, 1u).front();
    assert(std::memcmp(
        &hiddenTreeInstance, &treeInstance, sizeof(treeInstance)) == 0);
    const auto &deferredReplacement =
        deferredRecord.replacementDynamicInstance;
    assert((deferredReplacement.flags &
        svr::kSparseVoxelTree64DynamicValid) != 0u);
    assert(deferredReplacement.terrainGeneration == terrainGeneration);
    delete copyMicro;
    delete copyLODGroup;
    delete copyNode;
    delete copyTreeInstance;
    delete copyTree;
    delete copyRaster;
    delete copyColumn;
    delete copyRecord;
    delete copyResult;
  }

  for (const Buffer buffer : {microReadback, lodGroupReadback, nodeReadback,
           treeInstanceReadback,
           treeResultReadback,
           rasterResultReadback,
           columnResultReadback, recordReadback, resultReadback, treeNodes,
           roots, children, micro, materials, occupancy, nodes, physical,
           representativeLODGroupIndices, nodeLODGroupIndices, lodGroups,
           treeInstances, treeNodesPadded, pageTable, metadata})
    graph.deleteBuffer(buffer);
}
} // namespace

int main()
{
  os::Logger::start();
  ::rendering::DeviceRequiredLimits limits{
    .minimumMemory = 0u,
    .minimumComputeSharedMemory = 0u,
    .minimumComputeWorkGroupInvocations = 1u,
  };
  ::rendering::backend::vulkan::VulkanRHI rhi(::rendering::backend::vulkan::Vulkan_1_2, limits, ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  testAtlasToColumns(rhi);
  testAtlasToPublishedPage(rhi);
  std::cout << "GPU terrain Phase 7 atlas and full ownership tests passed\n";
  os::Logger::shutdown();
  return 0;
}
