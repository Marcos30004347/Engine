#include "voxel/svraster/SparseVoxelTerrainVirtualGeometryGPUBuild.hpp"

#include "virtualgeometry/VirtualGeometryData.hpp"

#include <algorithm>

namespace voxel::svraster
{
namespace
{

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool statusReady(const SparseVoxelTerrainVGPUPageBuildResult &build)
{
  return build.status == static_cast<uint32_t>(SparseVoxelTerrainVGPUPageBuildStatus::Ready);
}

bool traceFormatKnownForBuild(const uint32_t format)
{
  return format == kVoxelTraceFormatCubic64 ||
      format == kVoxelTraceFormatBounds8 ||
      format == kVoxelTraceFormatBounds16;
}

uint64_t alignedBytes(const uint64_t bytes)
{
  if (bytes == 0u)
    return 0u;
  return ((bytes + 15u) / 16u) * 16u;
}

} // namespace

uint64_t sparseVoxelTerrainVGPUPageRasterBytes(
    const SparseVoxelTerrainVGPUPageBuildResult &build)
{
  return alignedBytes(static_cast<uint64_t>(build.vgMeshletCount) * sizeof(virtualgeometry::Meshlet)) +
      alignedBytes(static_cast<uint64_t>(build.vgHierarchyNodeCount) * sizeof(virtualgeometry::VirtualGeometryHierarchy)) +
      alignedBytes(static_cast<uint64_t>(build.vgGroupCount) * sizeof(uint32_t)) +
      alignedBytes(static_cast<uint64_t>(build.vgPositionWordCount) * sizeof(uint32_t)) +
      alignedBytes(static_cast<uint64_t>(build.vgNormalWordCount) * sizeof(uint32_t)) +
      alignedBytes(static_cast<uint64_t>(build.vgUVWordCount) * sizeof(uint32_t)) +
      alignedBytes(static_cast<uint64_t>(build.vgIndexByteCount)) +
      alignedBytes(static_cast<uint64_t>(build.vgBoneWeightWordCount) * sizeof(uint32_t));
}

uint64_t sparseVoxelTerrainVGPUPageTraceBytes(
    const SparseVoxelTerrainVGPUPageBuildResult &build)
{
  return alignedBytes(static_cast<uint64_t>(build.traceNodeCount) * sizeof(VoxelTraceNode64GPU)) +
      alignedBytes(static_cast<uint64_t>(build.traceErrorWordCount) * sizeof(uint32_t)) +
      alignedBytes(static_cast<uint64_t>(build.cullErrorWordCount) * sizeof(uint32_t));
}

bool validateSparseVoxelTerrainVGPUPageBuild(
    const SparseVoxelTerrainVGPUPageBuildResult &build,
    const SparseVoxelTerrainVGPUPageBuildSettings &settings,
    SparseVoxelTerrainVGPUPageBuildValidation &validation,
    std::string *error)
{
  validation = {};
  validation.vgRasterPayloadBytes = sparseVoxelTerrainVGPUPageRasterBytes(build);
  validation.tracePayloadBytes = sparseVoxelTerrainVGPUPageTraceBytes(build);
  validation.materialPayloadBytes =
      alignedBytes(static_cast<uint64_t>(build.materialCount) * sizeof(SparseVoxelMaterialGPU)) +
      alignedBytes(static_cast<uint64_t>(build.materialCellCount) * sizeof(SparseVoxelBrickMaterialGPU));
  validation.totalPayloadBytes =
      validation.vgRasterPayloadBytes +
      validation.tracePayloadBytes +
      validation.materialPayloadBytes;
  validation.fitsPackedPageSlot =
      settings.packedPageSlotBytes == 0u ||
      validation.totalPayloadBytes <= settings.packedPageSlotBytes;

  if (!build.page.valid() || build.requiredProducerRevision == 0u)
    return fail(error, "terrain VG GPU page build identity is invalid");
  if (!statusReady(build) || build.validationErrors != 0u)
    return fail(error, "terrain VG GPU page build is not ready");
  if ((build.stageFlags & kSparseVoxelTerrainVGPURequiredStages) != kSparseVoxelTerrainVGPURequiredStages)
    return fail(error, "terrain VG GPU page build did not run all required stages");
  if (build.payloadRevision == 0u || build.geometryRevision == 0u)
    return fail(error, "terrain VG GPU page build revision is invalid");
  if (build.materialRevision != 0u && build.materialRevision != build.geometryRevision)
    return fail(error, "terrain VG GPU page build material revision does not match geometry revision");
  if (build.terrainGeneration == 0u || build.buildRevision == 0u)
    return fail(error, "terrain VG GPU page build source generation is invalid");
  if (build.vgInstallRewriteCount == 0u || build.vgUninstallRewriteCount == 0u)
    return fail(error, "terrain VG GPU page build requires virtual-geometry rewrite metadata");

  validation.completeVGRasterPayload =
      build.vgMeshletCount != 0u &&
      build.vgGroupCount != 0u &&
      build.vgHierarchyNodeCount != 0u &&
      build.vgVertexCount != 0u &&
      build.vgTriangleCount != 0u &&
      build.vgMaxVerticesPerMeshlet != 0u &&
      build.vgMaxVerticesPerMeshlet <= 255u &&
      build.vgMaxTrianglesPerMeshlet != 0u &&
      build.vgMaxTrianglesPerMeshlet <= virtualgeometry::ClusterSize &&
      build.vgMeshletCount <= settings.maximumVGMeshletCount &&
      build.vgGroupCount <= settings.maximumVGGroupCount &&
      build.vgHierarchyNodeCount <= settings.maximumVGHierarchyNodeCount &&
      build.vgVertexCount <= settings.maximumVGVertexCount &&
      build.vgTriangleCount <= settings.maximumVGTriangleCount &&
      build.vgPositionWordCount != 0u &&
      build.vgNormalWordCount != 0u &&
      build.vgUVWordCount != 0u &&
      build.vgIndexByteCount >= build.vgTriangleCount * 3u &&
      build.greedyQuadCount != 0u &&
      build.exposedFaceCount != 0u &&
      build.greedyQuadCount <= build.exposedFaceCount &&
      (!settings.requireFaceLocalVoxelFaces ||
       (build.greedyQuadCount == build.exposedFaceCount &&
        build.vgVertexCount >= build.exposedFaceCount * 4u &&
        build.vgTriangleCount >= build.exposedFaceCount * 2u));
  if (settings.requireVGRasterPayload && !validation.completeVGRasterPayload)
    return fail(error, "terrain VG GPU page build virtual-geometry raster payload is incomplete");

  validation.completeTracePayload =
      traceFormatKnownForBuild(build.traceFormat) &&
      build.traceNodeCount != 0u &&
      build.traceNodeCount <= settings.maximumTraceNodeCount &&
      (!settings.requireTraceErrorSidecar || build.traceErrorWordCount != 0u);
  if (settings.requireTracePayload && !validation.completeTracePayload)
    return fail(error, "terrain VG GPU page build trace payload is incomplete");

  validation.completeDependencyPayload =
      !settings.requireDependencyMetadata ||
      build.rootPage ||
      (build.dependencyCount != 0u &&
       build.dependencyCount <= settings.maximumDependencyCount &&
       build.parentPage.valid() &&
       build.fallbackPage.valid());
  if (!validation.completeDependencyPayload)
    return fail(error, "terrain VG GPU page build dependency metadata is incomplete");

  if (settings.requireMaterialPayload &&
      (build.materialCount == 0u || build.materialCellCount == 0u || build.materialRevision == 0u))
  {
    return fail(error, "terrain VG GPU page build material payload is incomplete");
  }
  if (!validation.fitsPackedPageSlot)
    return fail(error, "terrain VG GPU page build exceeds packed page slot budget");

  validation.ready = true;
  if (error != nullptr)
    error->clear();
  return true;
}

bool sparseVoxelProducedPagePayloadFromTerrainVGPUPageBuild(
    const SparseVoxelTerrainVGPUPageBuildResult &build,
    const SparseVoxelTerrainVGPUPageBuildSettings &settings,
    SparseVoxelProducedPagePayload &payload,
    std::string *error)
{
  payload = {};
  SparseVoxelTerrainVGPUPageBuildValidation validation{};
  if (!validateSparseVoxelTerrainVGPUPageBuild(build, settings, validation, error))
    return false;

  payload.page = build.page;
  payload.expectedPageGeneration = build.expectedPageGeneration;
  payload.requiredProducerRevision = build.requiredProducerRevision;
  payload.payloadRevision = build.payloadRevision;
  payload.geometryRevision = build.geometryRevision;
  payload.materialRevision =
      build.materialRevision != 0u
          ? build.materialRevision
          : (build.materialCount != 0u || build.materialCellCount != 0u ? build.geometryRevision : 0u);
  payload.productionFence = build.productionFence;
  payload.requirements.page = build.page;
  payload.requirements.geometryRevision = build.geometryRevision;
  payload.requirements.materialRevision = payload.materialRevision;
  payload.requirements.traceFormat = build.traceFormat;

  // Sparse voxel scene rewrites still target the shared scene hierarchy.
  // Virtual-geometry raster hierarchy/meshlets live in the explicit VG ranges
  // below and are installed into VirtualGeometryScene in the next phase.
  payload.requirements.hierarchyNodes.count = build.vgInstallRewriteCount;

  payload.requirements.virtualGeometryMeshlets.count = build.vgMeshletCount;
  payload.requirements.virtualGeometryHierarchyNodes.count = build.vgHierarchyNodeCount;
  payload.requirements.virtualGeometryGroups.count = build.vgGroupCount;
  payload.requirements.virtualGeometryPositionWords.count = build.vgPositionWordCount;
  payload.requirements.virtualGeometryNormalWords.count = build.vgNormalWordCount;
  payload.requirements.virtualGeometryUVWords.count = build.vgUVWordCount;
  payload.requirements.virtualGeometryIndexBytes.count = build.vgIndexByteCount;
  payload.requirements.virtualGeometryBoneWeightWords.count = build.vgBoneWeightWordCount;

  payload.requirements.traceNodes.count = build.traceNodeCount;
  payload.requirements.traceErrorWords.count = build.traceErrorWordCount;
  payload.requirements.cullErrorWords.count = build.cullErrorWordCount;
  payload.requirements.materials.count = build.materialCount;
  payload.requirements.materialCells.count = build.materialCellCount;
  payload.installRewriteCount = build.vgInstallRewriteCount;
  payload.uninstallRewriteCount = build.vgUninstallRewriteCount;
  payload.payloadComplete = true;
  if (error != nullptr)
    error->clear();
  return true;
}

bool prepareSparseVoxelTerrainVGPUPageBuildPublication(
    SparseVoxelSharedPageTable &table,
    const SparseVoxelTerrainVGPUPageBuildResult &build,
    const SparseVoxelTerrainVGPUPageBuildSettings &settings,
    const uint64_t copyFence,
    SparseVoxelTerrainVGPUPagePublication &publication,
    std::string *error)
{
  publication = {};
  if (!sparseVoxelProducedPagePayloadFromTerrainVGPUPageBuild(
          build,
          settings,
          publication.produced,
          error))
  {
    return false;
  }
  publication.validated = true;
  if (!table.reserveProducedPagePayload(publication.produced, publication.reservation, error))
    return false;
  publication.reserved = true;
  if (!table.markPayloadCopied(publication.reservation, copyFence, error))
    return false;
  publication.copied = true;
  if (error != nullptr)
    error->clear();
  return true;
}

bool publishSparseVoxelTerrainVGPUPageBuild(
    SparseVoxelSharedPageTable &table,
    const SparseVoxelTerrainVGPUPageBuildResult &build,
    const SparseVoxelTerrainVGPUPageBuildSettings &settings,
    const uint64_t copyFence,
    const std::vector<SparseVoxelPageInstallRewrite> &rewrites,
    SparseVoxelTerrainVGPUPagePublication &publication,
    std::string *error)
{
  if (!prepareSparseVoxelTerrainVGPUPageBuildPublication(
          table,
          build,
          settings,
          copyFence,
          publication,
          error))
  {
    return false;
  }
  if (!table.installCopiedPagePayload(publication.reservation, rewrites, error))
    return false;
  publication.committed = true;
  if (error != nullptr)
    error->clear();
  return true;
}

} // namespace voxel::svraster
