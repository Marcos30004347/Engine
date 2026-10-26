#include "voxel/svraster/SparseVoxelTerrainTriangleRaster.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

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

const SparseVoxelTerrainTriangleRasterPageView *findTrianglePageView(
    const std::vector<SparseVoxelTerrainTriangleRasterPageView> &views,
    const SparseVoxelPageId page,
    const uint64_t geometryRevision)
{
  const auto found = std::find_if(
      views.begin(),
      views.end(),
      [page, geometryRevision](const SparseVoxelTerrainTriangleRasterPageView &view)
      {
        return view.page == page &&
            view.geometryRevision == geometryRevision &&
            view.sidecar != nullptr;
      });
  return found == views.end() ? nullptr : &*found;
}

uint64_t sidecarGeometryRevision(const SparseVoxelTerrainTrianglePageSidecar &sidecar)
{
  return sidecar.tree64Revision != 0u
      ? sidecar.tree64Revision
      : sidecar.brickPayloadRevision;
}

bool appendTriangleDraws(
    const SparseVoxelSharedPageTableEntry &entry,
    const SparseVoxelTerrainTrianglePageSidecar &sidecar,
    const uint32_t instanceIndex,
    SparseVoxelTerrainRasterSelection &selection,
    SparseVoxelTerrainTriangleRasterDrawList &result,
    std::string *error)
{
  if (!entry.allocation.hasTriangleRasterPayload())
    return fail(error, "triangle raster draw path needs resident triangle ranges");
  if (sidecar.clusters.size() != entry.allocation.triangleClusters.count ||
      sidecar.vertices.size() != entry.allocation.triangleVertices.count ||
      sidecar.indices.size() != entry.allocation.triangleIndices.count)
  {
    return fail(error, "triangle raster sidecar counts do not match resident ranges");
  }
  if (entry.allocation.triangleVertices.first >
      static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
  {
    return fail(error, "triangle raster vertex range exceeds indexed draw base-vertex range");
  }

  selection.firstDraw = static_cast<uint32_t>(result.triangleDraws.size());
  for (uint32_t clusterIndex = 0u;
       clusterIndex < sidecar.clusters.size();
       ++clusterIndex)
  {
    const auto &cluster = sidecar.clusters[clusterIndex];
    if (uint64_t{entry.allocation.triangleIndices.first} + cluster.firstIndex >
        std::numeric_limits<uint32_t>::max())
    {
      return fail(error, "triangle raster index range exceeds indexed draw range");
    }

    SparseVoxelTerrainTriangleDrawRecord draw{};
    draw.page = entry.page;
    draw.instanceIndex = instanceIndex;
    draw.clusterIndex = clusterIndex;
    draw.lodGroup = cluster.lodGroup;
    draw.boundsMin = cluster.boundsMin;
    draw.boundsMax = cluster.boundsMax;
    draw.indirect.indexCount = cluster.indexCount;
    draw.indirect.instanceCount = 1u;
    draw.indirect.firstIndex =
        entry.allocation.triangleIndices.first + cluster.firstIndex;
    draw.indirect.vertexOffset =
        static_cast<int32_t>(entry.allocation.triangleVertices.first);
    draw.indirect.firstInstance =
        static_cast<uint32_t>(result.triangleDraws.size());
    result.triangleDraws.push_back(draw);
  }
  selection.drawCount =
      static_cast<uint32_t>(result.triangleDraws.size()) - selection.firstDraw;
  return true;
}

bool finiteBounds(const SparseVoxelTerrainTriangleDrawRecord &draw)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (!std::isfinite(draw.boundsMin[axis]) ||
        !std::isfinite(draw.boundsMax[axis]) ||
        draw.boundsMin[axis] > draw.boundsMax[axis])
    {
      return false;
    }
  }
  return true;
}

bool flagAt(const std::vector<uint8_t> &flags, const uint32_t index)
{
  return index < flags.size() && flags[index] != 0u;
}

void appendRejectedDraw(
    const uint32_t drawIndex,
    const SparseVoxelTerrainTriangleDrawRecord &draw,
    SparseVoxelTerrainTriangleCullPlan &result)
{
  result.rejectedDrawIndices.push_back(drawIndex);
  ++result.rejectedDrawCount;
  result.rejectedIndexCount += draw.indirect.indexCount;
  result.rejectedTriangleCount += draw.indirect.indexCount / 3u;
}

void appendEarlyDraw(
    const uint32_t drawIndex,
    const SparseVoxelTerrainTriangleDrawRecord &draw,
    SparseVoxelTerrainTriangleCullPlan &result)
{
  result.earlyDrawIndices.push_back(drawIndex);
  result.earlyIndirect.push_back(draw.indirect);
  ++result.submittedDrawCount;
  result.submittedIndexCount += draw.indirect.indexCount;
  result.submittedTriangleCount += draw.indirect.indexCount / 3u;
}

void appendLateDraw(
    const uint32_t drawIndex,
    const SparseVoxelTerrainTriangleDrawRecord &draw,
    SparseVoxelTerrainTriangleCullPlan &result)
{
  result.lateDrawIndices.push_back(drawIndex);
  result.lateIndirect.push_back(draw.indirect);
  ++result.submittedDrawCount;
  result.submittedIndexCount += draw.indirect.indexCount;
  result.submittedTriangleCount += draw.indirect.indexCount / 3u;
}

} // namespace

bool buildSparseVoxelTerrainTriangleRasterDrawList(
    const SparseVoxelSharedPageTable &table,
    const std::vector<SparseVoxelTraversalCandidate> &candidates,
    const std::vector<SparseVoxelTerrainTriangleRasterPageView> &trianglePages,
    const SparseVoxelTerrainTriangleMeshSettings &settings,
    SparseVoxelTerrainTriangleRasterDrawList &result,
    std::string *error)
{
  result = {};
  if (error != nullptr)
    error->clear();

  for (const auto &candidate : candidates)
  {
    SparseVoxelInstanceLODSelection lod{};
    if (!selectSparseVoxelStreamedInstanceLOD(
            table,
            candidate.instance,
            candidate.desiredPage,
            lod,
            error))
    {
      return false;
    }

    SparseVoxelTerrainRasterSelection selection{};
    selection.instanceIndex = lod.instanceIndex;
    selection.desiredPage = lod.desiredPage;
    selection.selectedPage = lod.selectedPage;
    selection.selectedFallback = lod.selectedFallback;
    selection.requestedDesiredPage = lod.requestedDesiredPage;

    const auto *entry = table.entry(lod.selectedPage);
    if (entry != nullptr && entry->resident() && entry->allocation.hasTriangleRasterPayload())
    {
      const auto *view = findTrianglePageView(
          trianglePages,
          lod.selectedPage,
          entry->geometryRevision);
      if (view != nullptr &&
          sidecarGeometryRevision(*view->sidecar) == entry->geometryRevision &&
          validateSparseVoxelTerrainTrianglePageSidecar(*view->sidecar, settings, error))
      {
        selection.path = SparseVoxelTerrainRasterPath::TriangleClusters;
        if (!appendTriangleDraws(
                *entry,
                *view->sidecar,
                lod.instanceIndex,
                selection,
                result,
                error))
        {
          return false;
        }
        ++result.triangleSelectionCount;
        result.selections.push_back(selection);
        continue;
      }
      ++result.staleSidecarCount;
    }

    if (entry != nullptr && entry->resident() && entry->allocation.hasBrickRasterPayload())
    {
      selection.path = SparseVoxelTerrainRasterPath::BrickFallback;
      ++result.brickFallbackCount;
      result.selections.push_back(selection);
      continue;
    }

    const auto *root = table.entry(candidate.instance.rootPage);
    if (root != nullptr && root->resident() && root->allocation.hasBrickRasterPayload())
    {
      selection.selectedPage = candidate.instance.rootPage;
      selection.selectedFallback = true;
      selection.requestedDesiredPage = !(selection.desiredPage == selection.selectedPage);
      selection.path = SparseVoxelTerrainRasterPath::BrickFallback;
      ++result.brickFallbackCount;
      result.selections.push_back(selection);
      continue;
    }

    selection.path = SparseVoxelTerrainRasterPath::None;
    ++result.missingRasterCount;
    result.selections.push_back(selection);
  }

  return true;
}

bool buildSparseVoxelTerrainTriangleCullPlan(
    const SparseVoxelTerrainTriangleRasterDrawList &drawList,
    const SparseVoxelTerrainTriangleCullSettings &settings,
    SparseVoxelTerrainTriangleCullPlan &result,
    std::string *error)
{
  result = {};
  if (error != nullptr)
    error->clear();
  result.inputDrawCount =
      static_cast<uint32_t>(drawList.triangleDraws.size());
  const auto drawCount = drawList.triangleDraws.size();
  result.earlyDrawIndices.reserve(drawCount);
  result.lateDrawIndices.reserve(drawCount);
  result.rejectedDrawIndices.reserve(drawCount);
  result.earlyIndirect.reserve(drawCount);
  result.lateIndirect.reserve(drawCount);

  const uint32_t planeCount =
      settings.enableFrustum
          ? std::min<uint32_t>(settings.frustumPlaneCount, settings.frustumPlanes.size())
          : 0u;
  const bool frustumActive = planeCount != 0u;
  const bool allowPreviousFrameVisibility = !settings.cameraCut;
  const bool hasPreviousVisibility =
      allowPreviousFrameVisibility && !settings.previouslyVisible.empty();
  const bool hasHZBRejects =
      allowPreviousFrameVisibility && settings.enableHZB && !settings.hzbOccluded.empty();
  const bool hasOccluderSeed =
      allowPreviousFrameVisibility && settings.occluderStride != 0u;
  uint32_t nextOccluderSeed = 0u;

  for (uint32_t drawIndex = 0u;
       drawIndex < drawCount;
       ++drawIndex)
  {
    const auto &draw = drawList.triangleDraws[drawIndex];
    if (!finiteBounds(draw))
      return fail(error, "triangle raster cull plan found invalid draw bounds");
    if (draw.indirect.indexCount == 0u || draw.indirect.instanceCount == 0u)
      return fail(error, "triangle raster cull plan found an empty draw command");

    bool inFrustum = true;
    if (frustumActive)
    {
      for (uint32_t planeIndex = 0u; planeIndex < planeCount; ++planeIndex)
      {
        const auto &plane = settings.frustumPlanes[planeIndex];
        const std::array<float, 3> positive{
            plane.normal[0] >= 0.0f ? draw.boundsMax[0] : draw.boundsMin[0],
            plane.normal[1] >= 0.0f ? draw.boundsMax[1] : draw.boundsMin[1],
            plane.normal[2] >= 0.0f ? draw.boundsMax[2] : draw.boundsMin[2],
        };
        const float signedDistance =
            plane.normal[0] * positive[0] +
            plane.normal[1] * positive[1] +
            plane.normal[2] * positive[2] +
            plane.distance;
        if (signedDistance < 0.0f)
        {
          inFrustum = false;
          break;
        }
      }
    }

    if (!inFrustum)
    {
      appendRejectedDraw(drawIndex, draw, result);
      ++result.frustumRejectedCount;
      continue;
    }

    const bool previousVisible =
        hasPreviousVisibility && flagAt(settings.previouslyVisible, drawIndex);
    while (hasOccluderSeed && nextOccluderSeed < drawIndex)
      nextOccluderSeed += settings.occluderStride;
    const bool occluderSeed = hasOccluderSeed && drawIndex == nextOccluderSeed;
    if (previousVisible || occluderSeed)
    {
      appendEarlyDraw(drawIndex, draw, result);
      result.previousVisibleEarlyCount += previousVisible ? 1u : 0u;
      result.occluderEarlyCount += (!previousVisible && occluderSeed) ? 1u : 0u;
      continue;
    }

    const bool hzbReject = hasHZBRejects && flagAt(settings.hzbOccluded, drawIndex);
    if (hzbReject)
    {
      appendRejectedDraw(drawIndex, draw, result);
      ++result.hzbRejectedCount;
      continue;
    }

    appendLateDraw(drawIndex, draw, result);
  }

  return true;
}

} // namespace voxel::svraster
