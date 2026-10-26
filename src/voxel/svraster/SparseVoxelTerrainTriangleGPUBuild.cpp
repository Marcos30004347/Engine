#include "voxel/svraster/SparseVoxelTerrainTriangleGPUBuild.hpp"

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

bool statusReady(const SparseVoxelTerrainTriangleGPUPageBuildResult &build)
{
  return build.status == static_cast<uint32_t>(SparseVoxelTerrainTriangleGPUPageBuildStatus::Ready);
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

uint64_t sparseVoxelTerrainTriangleGPUPageRasterBytes(
    const SparseVoxelTerrainTriangleGPUPageBuildResult &build)
{
  return alignedBytes(static_cast<uint64_t>(build.triangleClusterCount) * sizeof(SparseVoxelTerrainTriangleCluster)) +
      alignedBytes(static_cast<uint64_t>(build.triangleVertexCount) * sizeof(SparseVoxelTerrainTriangleVertex)) +
      alignedBytes(static_cast<uint64_t>(build.triangleIndexCount) * sizeof(uint32_t));
}

uint64_t sparseVoxelTerrainTriangleGPUPageTraceBytes(
    const SparseVoxelTerrainTriangleGPUPageBuildResult &build)
{
  return alignedBytes(static_cast<uint64_t>(build.hierarchyNodeCount) * sizeof(SparseVoxelLODNodeGPU)) +
      alignedBytes(static_cast<uint64_t>(build.childIndexCount) * sizeof(uint32_t)) +
      alignedBytes(static_cast<uint64_t>(build.traceNodeCount) * sizeof(VoxelTraceNode64GPU)) +
      alignedBytes(static_cast<uint64_t>(build.traceErrorWordCount) * sizeof(uint32_t)) +
      alignedBytes(static_cast<uint64_t>(build.cullErrorWordCount) * sizeof(uint32_t));
}

bool validateSparseVoxelTerrainTriangleGPUPageBuild(
    const SparseVoxelTerrainTriangleGPUPageBuildResult &build,
    const SparseVoxelTerrainTriangleGPUPageBuildSettings &settings,
    SparseVoxelTerrainTriangleGPUPageBuildValidation &validation,
    std::string *error)
{
  validation = {};
  validation.rasterPayloadBytes = sparseVoxelTerrainTriangleGPUPageRasterBytes(build);
  validation.tracePayloadBytes = sparseVoxelTerrainTriangleGPUPageTraceBytes(build);
  validation.materialPayloadBytes =
      alignedBytes(static_cast<uint64_t>(build.materialCount) * sizeof(SparseVoxelMaterialGPU)) +
      alignedBytes(static_cast<uint64_t>(build.materialCellCount) * sizeof(SparseVoxelBrickMaterialGPU));
  validation.totalPayloadBytes =
      validation.rasterPayloadBytes +
      validation.tracePayloadBytes +
      validation.materialPayloadBytes;
  validation.fitsPackedPageSlot =
      settings.packedPageSlotBytes == 0u ||
      validation.totalPayloadBytes <= settings.packedPageSlotBytes;

  if (!build.page.valid() || build.requiredProducerRevision == 0u)
    return fail(error, "terrain triangle GPU page build identity is invalid");
  if (!statusReady(build) || build.validationErrors != 0u)
    return fail(error, "terrain triangle GPU page build is not ready");
  if ((build.stageFlags & kSparseVoxelTerrainTriangleGPUBuildRequiredStages) != kSparseVoxelTerrainTriangleGPUBuildRequiredStages)
    return fail(error, "terrain triangle GPU page build did not run all required stages");
  if (build.payloadRevision == 0u || build.geometryRevision == 0u)
    return fail(error, "terrain triangle GPU page build revision is invalid");
  if (build.materialRevision != 0u && build.materialRevision != build.geometryRevision)
    return fail(error, "terrain triangle GPU page build material revision does not match geometry revision");
  if (build.terrainGeneration == 0u || build.buildRevision == 0u)
    return fail(error, "terrain triangle GPU page build source generation is invalid");
  if (build.installRewriteCount == 0u)
    return fail(error, "terrain triangle GPU page build requires an install rewrite");
  if (build.uninstallRewriteCount == 0u)
    return fail(error, "terrain triangle GPU page build requires an uninstall rewrite");

  validation.completeRasterPayload =
      build.sourceVoxelCount != 0u &&
      build.exposedFaceCount <= build.sourceVoxelCount * 6u &&
      build.triangleClusterCount != 0u &&
      build.triangleVertexCount != 0u &&
      build.triangleIndexCount != 0u &&
      build.triangleIndexCount % 3u == 0u &&
      build.triangleClusterCount <= settings.maximumTriangleClusterCount &&
      build.triangleVertexCount <= settings.maximumTriangleVertexCount &&
      build.triangleIndexCount <= settings.maximumTriangleIndexCount &&
      build.greedyQuadCount != 0u &&
      build.exposedFaceCount != 0u &&
      build.greedyQuadCount <= build.exposedFaceCount &&
      (!settings.requireFaceLocalVoxelFaces ||
       (build.greedyQuadCount == build.exposedFaceCount &&
        build.triangleVertexCount == build.exposedFaceCount * 4u &&
        build.triangleIndexCount == build.exposedFaceCount * 6u));
  if (settings.requireTrianglePayload && !validation.completeRasterPayload)
    return fail(error, "terrain triangle GPU page build raster triangle payload is incomplete");

  validation.completeTracePayload =
      traceFormatKnownForBuild(build.traceFormat) &&
      build.hierarchyNodeCount != 0u &&
      build.traceNodeCount != 0u &&
      build.traceNodeCount <= settings.maximumTraceNodeCount &&
      (!settings.requireTraceErrorSidecar || build.traceErrorWordCount != 0u);
  if (settings.requireTracePayload && !validation.completeTracePayload)
    return fail(error, "terrain triangle GPU page build trace payload is incomplete");

  validation.completeDependencyPayload =
      !settings.requireDependencyMetadata ||
      build.rootPage ||
      (build.dependencyCount != 0u &&
       build.dependencyCount <= settings.maximumDependencyCount &&
       build.parentPage.valid() &&
       build.fallbackPage.valid());
  if (!validation.completeDependencyPayload)
    return fail(error, "terrain triangle GPU page build dependency metadata is incomplete");

  if (settings.requireMaterialPayload &&
      (build.materialCount == 0u || build.materialCellCount == 0u || build.materialRevision == 0u))
  {
    return fail(error, "terrain triangle GPU page build material payload is incomplete");
  }
  if (!validation.fitsPackedPageSlot)
    return fail(error, "terrain triangle GPU page build exceeds packed page slot budget");

  validation.ready = true;
  if (error != nullptr)
    error->clear();
  return true;
}

bool sparseVoxelProducedPagePayloadFromTerrainTriangleGPUBuild(
    const SparseVoxelTerrainTriangleGPUPageBuildResult &build,
    const SparseVoxelTerrainTriangleGPUPageBuildSettings &settings,
    SparseVoxelProducedPagePayload &payload,
    std::string *error)
{
  payload = {};
  SparseVoxelTerrainTriangleGPUPageBuildValidation validation{};
  if (!validateSparseVoxelTerrainTriangleGPUPageBuild(build, settings, validation, error))
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
  payload.requirements.hierarchyNodes.count = build.hierarchyNodeCount;
  payload.requirements.childIndices.count = build.childIndexCount;
  payload.requirements.triangleClusters.count = build.triangleClusterCount;
  payload.requirements.triangleVertices.count = build.triangleVertexCount;
  payload.requirements.triangleIndices.count = build.triangleIndexCount;
  payload.requirements.traceNodes.count = build.traceNodeCount;
  payload.requirements.traceErrorWords.count = build.traceErrorWordCount;
  payload.requirements.cullErrorWords.count = build.cullErrorWordCount;
  payload.requirements.materials.count = build.materialCount;
  payload.requirements.materialCells.count = build.materialCellCount;
  payload.installRewriteCount = build.installRewriteCount;
  payload.uninstallRewriteCount = build.uninstallRewriteCount;
  payload.payloadComplete = true;
  if (error != nullptr)
    error->clear();
  return true;
}

bool publishSparseVoxelTerrainTriangleGPUPageBuild(
    SparseVoxelSharedPageTable &table,
    const SparseVoxelTerrainTriangleGPUPageBuildResult &build,
    const SparseVoxelTerrainTriangleGPUPageBuildSettings &settings,
    const uint64_t copyFence,
    const std::vector<SparseVoxelPageInstallRewrite> &rewrites,
    SparseVoxelTerrainTriangleGPUPagePublication &publication,
    std::string *error)
{
  if (!prepareSparseVoxelTerrainTriangleGPUPageBuildPublication(
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

bool prepareSparseVoxelTerrainTriangleGPUPageBuildPublication(
    SparseVoxelSharedPageTable &table,
    const SparseVoxelTerrainTriangleGPUPageBuildResult &build,
    const SparseVoxelTerrainTriangleGPUPageBuildSettings &settings,
    const uint64_t copyFence,
    SparseVoxelTerrainTriangleGPUPagePublication &publication,
    std::string *error)
{
  publication = {};
  if (!sparseVoxelProducedPagePayloadFromTerrainTriangleGPUBuild(
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

} // namespace voxel::svraster
