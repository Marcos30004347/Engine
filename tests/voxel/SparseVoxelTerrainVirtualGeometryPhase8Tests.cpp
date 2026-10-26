#include "voxel/svraster/SparseVoxelTerrainVirtualGeometryGPUBuild.hpp"

#include "virtualgeometry/VirtualGeometryData.hpp"

#include <cstdint>
#include <iostream>
#include <string>

namespace svr = voxel::svraster;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

svr::SparseVoxelTerrainVGPUPageBuildSettings buildSettings()
{
  svr::SparseVoxelTerrainVGPUPageBuildSettings settings{};
  settings.requireVGRasterPayload = true;
  settings.requireTracePayload = true;
  settings.requireTraceErrorSidecar = true;
  settings.requireDependencyMetadata = true;
  settings.requireMaterialPayload = false;
  return settings;
}

svr::SparseVoxelTerrainVGPUPageBuildResult readyBuild(const bool rootPage = false)
{
  const uint64_t producer = 0x5445525241494e56ull;
  svr::SparseVoxelTerrainVGPUPageBuildResult result{};
  result.page = {producer, rootPage ? 0u : 1u};
  result.parentPage = rootPage ? svr::SparseVoxelPageId{} : svr::SparseVoxelPageId{producer, 0u};
  result.fallbackPage = result.parentPage;
  result.expectedPageGeneration = 7u;
  result.requiredProducerRevision = 23u;
  result.payloadRevision = 0x8001u;
  result.geometryRevision = 0x8001u;
  result.materialRevision = 0u;
  result.productionFence = 9u;
  result.terrainGeneration = 44u;
  result.buildRevision = 23u;
  result.status = static_cast<uint32_t>(svr::SparseVoxelTerrainVGPUPageBuildStatus::Ready);
  result.stageFlags = svr::kSparseVoxelTerrainVGPURequiredStages;
  result.sourceVoxelCount = 256u;
  result.exposedFaceCount = 192u;
  result.greedyQuadCount = 192u;
  result.vgMeshletCount = 4u;
  result.vgGroupCount = 2u;
  result.vgHierarchyNodeCount = 3u;
  result.vgInstallRewriteCount = 2u;
  result.vgUninstallRewriteCount = 2u;
  result.vgVertexCount = 768u;
  result.vgTriangleCount = 384u;
  result.vgMaxVerticesPerMeshlet = 192u;
  result.vgMaxTrianglesPerMeshlet = 96u;
  result.vgPositionWordCount = 768u;
  result.vgNormalWordCount = 768u;
  result.vgUVWordCount = 1536u;
  result.vgIndexByteCount = 1152u;
  result.traceFormat = svr::kVoxelTraceFormatCubic64;
  result.traceNodeCount = 16u;
  result.traceErrorWordCount = 8u;
  result.cullErrorWordCount = 8u;
  result.materialCount = 4u;
  result.materialCellCount = 4u;
  result.dependencyCount = rootPage ? 0u : 1u;
  result.rootPage = rootPage;
  return result;
}

void testReadyJointPayloadValidates()
{
  std::cout << "ready GPU-produced terrain page validates as VG raster + trace payload\n";
  const auto build = readyBuild();
  svr::SparseVoxelTerrainVGPUPageBuildValidation validation{};
  std::string error;

  check(
      svr::validateSparseVoxelTerrainVGPUPageBuild(
          build,
          buildSettings(),
          validation,
          &error),
      "complete joint build validates: " + error);
  check(validation.ready, "validation marks the page ready");
  check(validation.completeVGRasterPayload, "validation marks virtual-geometry raster payload complete");
  check(validation.completeTracePayload, "validation marks compute trace payload complete");
  check(validation.completeDependencyPayload, "validation marks dependency metadata complete");
  check(validation.fitsPackedPageSlot, "validation keeps the page inside the packed slot budget");
  check(validation.vgRasterPayloadBytes > 0u && validation.tracePayloadBytes > 0u, "validation reports non-empty raster and trace byte ranges");
  check(
      validation.totalPayloadBytes ==
          validation.vgRasterPayloadBytes +
          validation.tracePayloadBytes +
          validation.materialPayloadBytes,
      "validation reports a consistent total byte budget");
}

void testReadyJointPayloadCarriesExactInstallRequirements()
{
  std::cout << "ready GPU-produced terrain page exports exact install/page payload requirements\n";
  const auto build = readyBuild();
  const auto settings = buildSettings();
  svr::SparseVoxelProducedPagePayload payload{};
  std::string error;

  check(
      svr::sparseVoxelProducedPagePayloadFromTerrainVGPUPageBuild(
          build,
          settings,
          payload,
          &error),
      "GPU build converts to installable payload: " + error);
  check(payload.valid(), "installable payload is valid");
  check(payload.page == build.page, "payload preserves page identity");
  check(payload.expectedPageGeneration == build.expectedPageGeneration, "payload preserves expected page generation");
  check(payload.requiredProducerRevision == build.requiredProducerRevision, "payload preserves producer revision");
  check(payload.payloadRevision == build.payloadRevision, "payload preserves payload revision");
  check(payload.geometryRevision == build.geometryRevision, "payload preserves geometry revision");
  check(payload.productionFence == build.productionFence, "payload preserves production fence");
  check(payload.payloadComplete, "payload is marked complete only after validation");
  check(
      payload.requirements.virtualGeometryMeshlets.count ==
          build.vgMeshletCount,
      "payload requests exact VG meshlet range");
  check(
      payload.requirements.virtualGeometryHierarchyNodes.count ==
          build.vgHierarchyNodeCount,
      "payload requests exact VG hierarchy range");
  check(
      payload.requirements.virtualGeometryGroups.count == build.vgGroupCount,
      "payload requests exact VG group range");
  check(
      payload.requirements.virtualGeometryPositionWords.count ==
          build.vgPositionWordCount,
      "payload requests exact VG position range");
  check(
      payload.requirements.virtualGeometryNormalWords.count ==
          build.vgNormalWordCount,
      "payload requests exact VG normal range");
  check(
      payload.requirements.virtualGeometryUVWords.count ==
          build.vgUVWordCount,
      "payload requests exact VG uv range");
  check(
      payload.requirements.virtualGeometryIndexBytes.count ==
          build.vgIndexByteCount,
      "payload requests exact VG index-byte range");
  check(
      payload.requirements.traceNodes.count == build.traceNodeCount &&
          payload.requirements.traceErrorWords.count ==
              build.traceErrorWordCount &&
          payload.requirements.cullErrorWords.count ==
              build.cullErrorWordCount,
      "payload requests exact trace and error sidecar ranges");
  check(
      payload.installRewriteCount == build.vgInstallRewriteCount &&
          payload.uninstallRewriteCount == build.vgUninstallRewriteCount,
      "payload preserves install/uninstall rewrite counts");
}

void testMissingRasterPayloadRejected()
{
  std::cout << "missing virtual-geometry raster payload rejects publication\n";
  auto build = readyBuild();
  build.vgMeshletCount = 0u;

  svr::SparseVoxelTerrainVGPUPageBuildValidation validation{};
  std::string error;
  check(
      !svr::validateSparseVoxelTerrainVGPUPageBuild(
          build,
          buildSettings(),
          validation,
          &error),
      "build without VG meshlets is rejected");
  check(!validation.completeVGRasterPayload, "validation reports raster incomplete");
  check(error.find("virtual-geometry raster payload") != std::string::npos, "error names the raster payload");
}

void testDefaultBuildRejectsMergedVoxelFaces()
{
  std::cout << "default VG GPU build rejects merged voxel-face payloads\n";
  auto build = readyBuild();
  build.greedyQuadCount = build.exposedFaceCount / 2u;

  svr::SparseVoxelTerrainVGPUPageBuildValidation validation{};
  std::string error;
  check(
      !svr::validateSparseVoxelTerrainVGPUPageBuild(
          build,
          buildSettings(),
          validation,
          &error),
      "default settings reject a merged/greedy voxel-face payload");
  check(!validation.completeVGRasterPayload, "merged faces mark the VG raster payload incomplete");

  auto settings = buildSettings();
  settings.requireFaceLocalVoxelFaces = false;
  error.clear();
  check(
      svr::validateSparseVoxelTerrainVGPUPageBuild(
          build,
          settings,
          validation,
          &error),
      "explicit compression mode can still validate greedy benchmark payloads: " + error);
}

void testMissingTracePayloadRejected()
{
  std::cout << "missing trace payload rejects publication\n";
  auto build = readyBuild();
  build.traceNodeCount = 0u;

  svr::SparseVoxelTerrainVGPUPageBuildValidation validation{};
  std::string error;
  check(
      !svr::validateSparseVoxelTerrainVGPUPageBuild(
          build,
          buildSettings(),
          validation,
          &error),
      "build without trace nodes is rejected");
  check(validation.completeVGRasterPayload && !validation.completeTracePayload, "validation keeps raster ready but trace incomplete");
  check(error.find("trace payload") != std::string::npos, "error names the trace payload");
}

void testDependencyContract()
{
  std::cout << "dependency metadata keeps non-root publication safe while root pages can bootstrap\n";
  auto child = readyBuild();
  child.dependencyCount = 0u;

  svr::SparseVoxelTerrainVGPUPageBuildValidation validation{};
  std::string error;
  check(
      !svr::validateSparseVoxelTerrainVGPUPageBuild(
          child,
          buildSettings(),
          validation,
          &error),
      "non-root page without dependency metadata is rejected");
  check(!validation.completeDependencyPayload, "validation reports missing dependency metadata");

  auto root = readyBuild(true);
  error.clear();
  check(
      svr::validateSparseVoxelTerrainVGPUPageBuild(
          root,
          buildSettings(),
          validation,
          &error),
      "root page can validate without parent/fallback dependencies: " + error);
  check(validation.completeDependencyPayload, "root validation marks dependency metadata complete");
}

void testPackedSlotOverflowRejected()
{
  std::cout << "packed page slot budget rejects oversized generated pages\n";
  auto build = readyBuild();
  auto settings = buildSettings();
  settings.packedPageSlotBytes = 256u;

  svr::SparseVoxelTerrainVGPUPageBuildValidation validation{};
  std::string error;
  check(
      !svr::validateSparseVoxelTerrainVGPUPageBuild(
          build,
          settings,
          validation,
          &error),
      "oversized page is rejected");
  check(!validation.fitsPackedPageSlot, "validation reports the packed slot overflow");
  check(error.find("packed page slot") != std::string::npos, "error names the packed slot budget");
}

void testMeshletBudgetsRejected()
{
  std::cout << "virtual-geometry meshlet budgets are enforced at the terrain page boundary\n";
  auto build = readyBuild();
  build.vgMaxTrianglesPerMeshlet = virtualgeometry::ClusterSize + 1u;

  svr::SparseVoxelTerrainVGPUPageBuildValidation validation{};
  std::string error;
  check(
      !svr::validateSparseVoxelTerrainVGPUPageBuild(
          build,
          buildSettings(),
          validation,
          &error),
      "meshlet triangle overflow is rejected");
  check(!validation.completeVGRasterPayload, "validation reports VG raster incomplete for over-budget meshlets");

  build = readyBuild();
  build.vgMaxVerticesPerMeshlet = 256u;
  error.clear();
  check(
      !svr::validateSparseVoxelTerrainVGPUPageBuild(
          build,
          buildSettings(),
          validation,
          &error),
      "meshlet vertex overflow is rejected");
}

void testRequiredStagesRejected()
{
  std::cout << "all generation/build/publication stages are required before a page can publish\n";
  auto build = readyBuild();
  build.stageFlags &= ~svr::kSparseVoxelTerrainVGPUStageTraceBuilt;

  svr::SparseVoxelTerrainVGPUPageBuildValidation validation{};
  std::string error;
  check(
      !svr::validateSparseVoxelTerrainVGPUPageBuild(
          build,
          buildSettings(),
          validation,
          &error),
      "missing trace-build stage rejects the page");
  check(error.find("required stages") != std::string::npos, "error names the missing stage contract");
}

} // namespace

int main()
{
  testReadyJointPayloadValidates();
  testReadyJointPayloadCarriesExactInstallRequirements();
  testMissingRasterPayloadRejected();
  testDefaultBuildRejectsMergedVoxelFaces();
  testMissingTracePayloadRejected();
  testDependencyContract();
  testPackedSlotOverflowRejected();
  testMeshletBudgetsRejected();
  testRequiredStagesRejected();

  if (failures != 0)
  {
    std::cerr << failures << " terrain VG Phase 8 GPU build contract checks failed\n";
    return 1;
  }

  std::cout << "Sparse voxel terrain virtual-geometry Phase 8 GPU build contract tests passed\n";
  return 0;
}
