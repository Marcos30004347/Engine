#include "voxel/svraster/SparseVoxelTerrainVirtualGeometrySceneBridge.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace svr = voxel::svraster;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

svr::SparseVoxelPageProducer producer()
{
  svr::SparseVoxelPageProducer result{};
  result.producerId = 0x5445525241494e58ull;
  result.kind = svr::SparseVoxelPageProducerKind::GPUGeneratedTerrain;
  result.producerRevision = 41u;
  result.producesOnGPU = true;
  result.debugName = "terrain-vg-phase10";
  return result;
}

svr::SparseVoxelSharedPageTableSettings tableSettings()
{
  svr::SparseVoxelSharedPageTableSettings settings{};
  settings.hierarchyNodeCapacity = 64u;
  settings.materialCapacity = 64u;
  settings.materialCellCapacity = 64u;
  settings.traceNodeCapacity = 128u;
  settings.traceErrorWordCapacity = 128u;
  settings.cullErrorWordCapacity = 128u;
  settings.virtualGeometryMeshletCapacity = 128u;
  settings.virtualGeometryHierarchyNodeCapacity = 128u;
  settings.virtualGeometryGroupCapacity = 128u;
  settings.virtualGeometryPositionWordCapacity = 4096u;
  settings.virtualGeometryNormalWordCapacity = 4096u;
  settings.virtualGeometryUVWordCapacity = 8192u;
  settings.virtualGeometryIndexByteCapacity = 8192u;
  settings.virtualGeometryBoneWeightWordCapacity = 1024u;
  return settings;
}

svr::SparseVoxelTerrainVGPUPageBuildSettings buildSettings(
    const bool requireTrace = true,
    const bool requireMaterial = false)
{
  svr::SparseVoxelTerrainVGPUPageBuildSettings settings{};
  settings.requireVGRasterPayload = true;
  settings.requireTracePayload = requireTrace;
  settings.requireTraceErrorSidecar = requireTrace;
  settings.requireDependencyMetadata = true;
  settings.requireMaterialPayload = requireMaterial;
  return settings;
}

svr::SparseVoxelTerrainVGSceneInstallSettings sceneSettings(
    const uint32_t localPageIndex = 3u,
    const bool requireTrace = true,
    const bool requireMaterial = false)
{
  svr::SparseVoxelTerrainVGSceneInstallSettings settings{};
  settings.localPageIndex = localPageIndex;
  settings.pageTableOffset = 100u;
  settings.prioritySlot = 200u + localPageIndex;
  settings.unitScaleBits = 0x3f800000u;
  settings.requireTracePayload = requireTrace;
  settings.requireMaterialPayload = requireMaterial;
  return settings;
}

svr::SparseVoxelTerrainVGPUPageBuildResult readyBuild(
    const svr::SparseVoxelPageId page,
    const uint64_t expectedGeneration,
    const uint32_t producerRevision,
    const uint64_t revision)
{
  svr::SparseVoxelTerrainVGPUPageBuildResult result{};
  result.page = page;
  result.expectedPageGeneration = expectedGeneration;
  result.requiredProducerRevision = producerRevision;
  result.payloadRevision = revision;
  result.geometryRevision = revision;
  result.productionFence = 25u;
  result.terrainGeneration = 65u;
  result.buildRevision = producerRevision;
  result.status = static_cast<uint32_t>(svr::SparseVoxelTerrainVGPUPageBuildStatus::Ready);
  result.stageFlags = svr::kSparseVoxelTerrainVGPURequiredStages;
  result.sourceVoxelCount = 512u;
  result.exposedFaceCount = 384u;
  result.greedyQuadCount = 384u;
  result.vgMeshletCount = 8u;
  result.vgGroupCount = 3u;
  result.vgHierarchyNodeCount = 5u;
  result.vgInstallRewriteCount = 2u;
  result.vgUninstallRewriteCount = 2u;
  result.vgVertexCount = 1536u;
  result.vgTriangleCount = 768u;
  result.vgMaxVerticesPerMeshlet = 192u;
  result.vgMaxTrianglesPerMeshlet = 96u;
  result.vgPositionWordCount = 1536u;
  result.vgNormalWordCount = 1536u;
  result.vgUVWordCount = 3072u;
  result.vgIndexByteCount = 2304u;
  result.traceFormat = svr::kVoxelTraceFormatCubic64;
  result.traceNodeCount = 24u;
  result.traceErrorWordCount = 12u;
  result.cullErrorWordCount = 12u;
  result.materialCount = 4u;
  result.materialCellCount = 4u;
  result.dependencyCount = 0u;
  result.rootPage = true;
  return result;
}

std::vector<svr::SparseVoxelPageInstallRewrite> installRewrites(
    const svr::SparseVoxelReservedPagePayload &reservation)
{
  std::vector<svr::SparseVoxelPageInstallRewrite> rewrites;
  rewrites.reserve(reservation.produced.installRewriteCount);
  for (uint32_t index = 0u; index < reservation.produced.installRewriteCount; ++index)
  {
    svr::SparseVoxelPageInstallRewrite rewrite{};
    rewrite.page = reservation.produced.page;
    rewrite.allocation = reservation.allocation;
    rewrite.requiredProducerRevision = reservation.produced.requiredProducerRevision;
    rewrite.rewrite.nodeIndex = reservation.allocation.hierarchyNodes.first + index;
    rewrite.rewrite.flags = 1u;
    rewrites.push_back(rewrite);
  }
  return rewrites;
}

bool publishBuild(
    svr::SparseVoxelSharedPageTable &table,
    const svr::SparseVoxelTerrainVGPUPageBuildResult &build,
    const svr::SparseVoxelTerrainVGPUPageBuildSettings &settings,
    std::string &error)
{
  svr::SparseVoxelTerrainVGPUPagePublication publication{};
  if (!svr::prepareSparseVoxelTerrainVGPUPageBuildPublication(
          table,
          build,
          settings,
          build.productionFence,
          publication,
          &error))
  {
    return false;
  }
  return table.installCopiedPagePayload(
      publication.reservation,
      installRewrites(publication.reservation),
      &error);
}

void testCommittedPageBuildsVGSceneInstallDescriptor()
{
  std::cout << "committed terrain VG page builds a VirtualGeometryScene install descriptor\n";
  const auto source = producer();
  const svr::SparseVoxelPageId page{source.producerId, 0u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure(tableSettings(), &error), "scene table configures: " + error);
  check(table.addPage(page, source, {}, true, &error), "scene page registers: " + error);
  auto build = readyBuild(page, table.entry(page)->generation, source.producerRevision, 0xa001u);
  check(publishBuild(table, build, buildSettings(), error), "scene page publishes before descriptor build: " + error);

  svr::SparseVoxelTerrainVGScenePageInstall install{};
  const auto settings = sceneSettings(5u);
  check(
      svr::buildSparseVoxelTerrainVGScenePageInstall(
          table,
          build,
          settings,
          install,
          &error),
      "scene descriptor builds from committed page: " + error);
  check(install.readyForSceneInstall, "descriptor is marked ready for scene install");
  check(install.hasTracePayload && install.hasMaterialPayload, "descriptor observes matching trace and material views");
  check(install.globalPageTableIndex == settings.pageTableOffset + settings.localPageIndex, "descriptor computes the global page-table index");
  check(install.virtualPageAllocation.pageId == settings.localPageIndex, "VG page allocation uses the terrain-local VG page index");
  check(install.virtualPageAllocation.clusterCount == build.vgMeshletCount, "VG page allocation exposes meshlet count as cluster count");
  check(install.virtualPageAllocation.decodedVertexCount == build.vgVertexCount, "VG page allocation preserves decoded vertex count");
  check(install.virtualPageAllocation.decodedIndexCount == build.vgTriangleCount * 3u, "VG page allocation preserves decoded index count");
  check(install.pageTableEntry.isInstalled == 1u, "VG page table entry is resident");
  check(install.pageTableEntry.prioritySlot == settings.prioritySlot, "VG page table entry preserves priority slot");
  check(install.pageTableEntry.clusterOffset == install.virtualPageAllocation.clusterOffset, "VG page table entry points at the same cluster base");
}

void testSceneDescriptorRejectsNonResidentPage()
{
  std::cout << "non-resident terrain pages cannot produce VG scene descriptors\n";
  const auto source = producer();
  const svr::SparseVoxelPageId page{source.producerId, 10u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure(tableSettings(), &error), "nonresident table configures: " + error);
  check(table.addPage(page, source, {}, true, &error), "nonresident page registers: " + error);
  auto build = readyBuild(page, table.entry(page)->generation, source.producerRevision, 0xa101u);

  svr::SparseVoxelTerrainVGScenePageInstall install{};
  check(
      !svr::buildSparseVoxelTerrainVGScenePageInstall(
          table,
          build,
          sceneSettings(),
          install,
          &error),
      "non-resident page is rejected");
  check(error.find("not resident") != std::string::npos, "error names residency");
}

void testSceneDescriptorRejectsStaleBuildMetadata()
{
  std::cout << "stale build metadata cannot alias a committed VG scene page\n";
  const auto source = producer();
  const svr::SparseVoxelPageId page{source.producerId, 20u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure(tableSettings(), &error), "stale table configures: " + error);
  check(table.addPage(page, source, {}, true, &error), "stale page registers: " + error);
  auto build = readyBuild(page, table.entry(page)->generation, source.producerRevision, 0xa201u);
  check(publishBuild(table, build, buildSettings(), error), "stale source page publishes: " + error);

  auto stale = build;
  stale.payloadRevision += 1u;
  svr::SparseVoxelTerrainVGScenePageInstall install{};
  check(
      !svr::buildSparseVoxelTerrainVGScenePageInstall(
          table,
          stale,
          sceneSettings(),
          install,
          &error),
      "stale build payload revision is rejected");
  check(error.find("generation/revision") != std::string::npos, "error names the stale generation/revision");
}

void testSceneDescriptorRequiresTraceAndOptionalMaterial()
{
  std::cout << "scene descriptor respects trace and material requirements\n";
  const auto source = producer();
  const svr::SparseVoxelPageId page{source.producerId, 30u};
  std::string error;

  svr::SparseVoxelSharedPageTable table{};
  check(table.configure(tableSettings(), &error), "trace table configures: " + error);
  check(table.addPage(page, source, {}, true, &error), "trace page registers: " + error);
  auto build = readyBuild(page, table.entry(page)->generation, source.producerRevision, 0xa301u);
  build.traceFormat = svr::kVoxelTraceFormatInvalid;
  build.traceNodeCount = 0u;
  build.traceErrorWordCount = 0u;
  build.cullErrorWordCount = 0u;
  build.materialCount = 0u;
  build.materialCellCount = 0u;
  check(publishBuild(table, build, buildSettings(false, false), error), "raster-only VG page can publish for fallback diagnostics: " + error);

  svr::SparseVoxelTerrainVGScenePageInstall install{};
  check(
      !svr::buildSparseVoxelTerrainVGScenePageInstall(
          table,
          build,
          sceneSettings(3u, true, false),
          install,
          &error),
      "trace-required scene descriptor rejects raster-only page");
  check(error.find("trace payload") != std::string::npos, "error names missing trace payload");

  error.clear();
  check(
      svr::buildSparseVoxelTerrainVGScenePageInstall(
          table,
          build,
          sceneSettings(3u, false, false),
          install,
          &error),
      "trace-optional scene descriptor can be built for diagnostics: " + error);
  check(!install.hasTracePayload && install.readyForSceneInstall, "trace-optional descriptor records that trace is absent");

  error.clear();
  check(
      !svr::buildSparseVoxelTerrainVGScenePageInstall(
          table,
          build,
          sceneSettings(3u, false, true),
          install,
          &error),
      "material-required scene descriptor rejects pages without material payload");
  check(error.find("material payload") != std::string::npos, "error names missing material payload");
}

} // namespace

int main()
{
  testCommittedPageBuildsVGSceneInstallDescriptor();
  testSceneDescriptorRejectsNonResidentPage();
  testSceneDescriptorRejectsStaleBuildMetadata();
  testSceneDescriptorRequiresTraceAndOptionalMaterial();

  if (failures != 0)
  {
    std::cerr << failures << " terrain VG Phase 10 scene bridge checks failed\n";
    return 1;
  }

  std::cout << "Sparse voxel terrain virtual-geometry Phase 10 scene bridge tests passed\n";
  return 0;
}
