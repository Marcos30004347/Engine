#include "voxel/svraster/SparseVoxelTerrainVirtualGeometrySceneBridge.hpp"

#include <algorithm>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

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

bool checkedAddU32(const uint32_t a, const uint32_t b, uint32_t &out)
{
  const uint64_t value = static_cast<uint64_t>(a) + static_cast<uint64_t>(b);
  if (value > std::numeric_limits<uint32_t>::max())
    return false;
  out = static_cast<uint32_t>(value);
  return true;
}

bool checkedMulAddU32(
    const uint32_t a,
    const uint32_t b,
    const uint32_t c,
    uint32_t &out)
{
  const uint64_t value =
      static_cast<uint64_t>(a) * static_cast<uint64_t>(b) +
      static_cast<uint64_t>(c);
  if (value > std::numeric_limits<uint32_t>::max())
    return false;
  out = static_cast<uint32_t>(value);
  return true;
}

bool rangesMatchBuild(
    const SparseVoxelPhysicalPageAllocation &allocation,
    const SparseVoxelTerrainVGPUPageBuildResult &build)
{
  return allocation.virtualGeometryMeshlets.count == build.vgMeshletCount &&
      allocation.virtualGeometryHierarchyNodes.count == build.vgHierarchyNodeCount &&
      allocation.virtualGeometryGroups.count == build.vgGroupCount &&
      allocation.virtualGeometryPositionWords.count == build.vgPositionWordCount &&
      allocation.virtualGeometryNormalWords.count == build.vgNormalWordCount &&
      allocation.virtualGeometryUVWords.count == build.vgUVWordCount &&
      allocation.virtualGeometryIndexBytes.count == build.vgIndexByteCount &&
      allocation.virtualGeometryBoneWeightWords.count == build.vgBoneWeightWordCount &&
      allocation.traceNodes.count == build.traceNodeCount &&
      allocation.traceErrorWords.count == build.traceErrorWordCount &&
      allocation.cullErrorWords.count == build.cullErrorWordCount &&
      allocation.materials.count == build.materialCount &&
      allocation.materialCells.count == build.materialCellCount;
}

} // namespace

bool buildSparseVoxelTerrainVGScenePageInstall(
    const SparseVoxelSharedPageTable &table,
    const SparseVoxelTerrainVGPUPageBuildResult &build,
    const SparseVoxelTerrainVGSceneInstallSettings &settings,
    SparseVoxelTerrainVGScenePageInstall &install,
    std::string *error)
{
  install = {};
  if (!build.page.valid() || build.payloadRevision == 0u || build.geometryRevision == 0u)
    return fail(error, "terrain VG scene install source build identity is invalid");
  if (settings.localPageIndex == UINT32_MAX || settings.prioritySlot == UINT32_MAX)
    return fail(error, "terrain VG scene install page table identity is invalid");

  const auto *entry = table.entry(build.page);
  if (entry == nullptr)
    return fail(error, "terrain VG scene install page is missing from the shared table");
  if (!entry->resident())
    return fail(error, "terrain VG scene install page is not resident");
  if (entry->generation != build.payloadRevision ||
      entry->geometryRevision != build.geometryRevision ||
      entry->allocation.geometryRevision != build.geometryRevision)
  {
    return fail(error, "terrain VG scene install page generation/revision does not match the GPU build");
  }
  if (entry->producer.producerRevision != build.requiredProducerRevision)
    return fail(error, "terrain VG scene install producer revision is stale");
  if (!entry->allocation.hasVirtualGeometryRasterPayload())
    return fail(error, "terrain VG scene install page has no virtual-geometry raster payload");
  if (settings.requireTracePayload && !entry->traceable())
    return fail(error, "terrain VG scene install page has no matching trace payload");
  if (settings.requireMaterialPayload && !entry->materialTraceable())
    return fail(error, "terrain VG scene install page has no matching material payload");
  if (!rangesMatchBuild(entry->allocation, build))
    return fail(error, "terrain VG scene install allocation ranges do not match the GPU build");

  uint32_t globalPageTableIndex = UINT32_MAX;
  if (!checkedAddU32(settings.pageTableOffset, settings.localPageIndex, globalPageTableIndex))
    return fail(error, "terrain VG scene install page table index overflows");

  uint32_t pageBufferOffset = 0u;
  if (entry->allocation.packedSlot != kSparseVoxelInvalidPackedPageSlot)
  {
    if (!checkedMulAddU32(
            entry->allocation.packedSlot,
            settings.packedSlotStrideBytes,
            entry->allocation.packedSlotOffsetBytes,
            pageBufferOffset))
    {
      return fail(error, "terrain VG scene install packed page offset overflows");
    }
  }

  const uint64_t decodedIndexCount64 =
      static_cast<uint64_t>(build.vgTriangleCount) * 3ull;
  if (decodedIndexCount64 > std::numeric_limits<uint32_t>::max())
    return fail(error, "terrain VG scene install decoded index count overflows");

  install.page = build.page;
  install.pageGeneration = entry->generation;
  install.geometryRevision = entry->geometryRevision;
  install.materialRevision = entry->materialRevision;
  install.traceFormat = entry->traceFormat;
  install.localPageIndex = settings.localPageIndex;
  install.globalPageTableIndex = globalPageTableIndex;
  install.prioritySlot = settings.prioritySlot;
  install.pageSlot = entry->allocation.packedSlot;
  install.pagelet = entry->allocation.packedPagelet;
  install.readyForSceneInstall = true;
  install.hasTracePayload = entry->traceable();
  install.hasMaterialPayload = entry->materialTraceable();
  install.sharedAllocation = entry->allocation;

  auto &pageAllocation = install.virtualPageAllocation;
  pageAllocation.pageId = settings.localPageIndex;
  pageAllocation.bufferOffset = pageBufferOffset;
  pageAllocation.size = entry->allocation.packedSlotBytes;
  pageAllocation.clusterOffset = entry->allocation.virtualGeometryMeshlets.first;
  pageAllocation.clusterCount = entry->allocation.virtualGeometryMeshlets.count;
  pageAllocation.unitScaleBits = settings.unitScaleBits;
  pageAllocation.decodedVertexOffset = entry->allocation.virtualGeometryPositionWords.first;
  pageAllocation.decodedVertexCount = build.vgVertexCount;
  pageAllocation.decodedIndexOffset = entry->allocation.virtualGeometryIndexBytes.first;
  pageAllocation.decodedIndexCount = static_cast<uint32_t>(decodedIndexCount64);
  pageAllocation.decodedBoneWeightOffset = entry->allocation.virtualGeometryBoneWeightWords.first;
  pageAllocation.decodedBoneWeightCount = entry->allocation.virtualGeometryBoneWeightWords.count;
  pageAllocation.prioritySlot = settings.prioritySlot;
  pageAllocation.isInstalled = true;

  install.pageTableEntry.bufferOffset = pageAllocation.bufferOffset;
  install.pageTableEntry.size = pageAllocation.size;
  install.pageTableEntry.clusterOffset = pageAllocation.clusterOffset;
  install.pageTableEntry.clusterCount = pageAllocation.clusterCount;
  install.pageTableEntry.isInstalled = 1u;
  install.pageTableEntry.prioritySlot = settings.prioritySlot;
  install.pageTableEntry.unitScaleBits = settings.unitScaleBits;
  install.pageTableEntry.decodedVertexOffset = pageAllocation.decodedVertexOffset;
  install.pageTableEntry.decodedIndexOffset = pageAllocation.decodedIndexOffset;
  install.pageTableEntry.decodedBoneWeightOffset = pageAllocation.decodedBoneWeightOffset;
  install.pageTableEntry._padding0 = globalPageTableIndex;
  install.pageTableEntry._padding1 = 0u;

  if (error != nullptr)
    error->clear();
  return true;
}

bool registerSparseVoxelTerrainVGRuntimeObject(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const uint32_t pageCount,
    const std::vector<virtualgeometry::VirtualGeometryHierarchy> &hierarchy,
    const virtualgeometry::QuantizationConfig &quantizationConfig,
    virtualgeometry::VirtualGeometryScene::RuntimeObjectId *outObjectId,
    std::string *error)
{
  return registerSparseVoxelTerrainVGRuntimeObjectWithPlacement(
      scene,
      objectName,
      pageCount,
      hierarchy,
      quantizationConfig,
      {},
      math::Vec3f(0.0f, 0.0f, 0.0f),
      outObjectId,
      error);
}

bool registerSparseVoxelTerrainVGRuntimeObjectWithPlacement(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const uint32_t pageCount,
    const std::vector<virtualgeometry::VirtualGeometryHierarchy> &hierarchy,
    const virtualgeometry::QuantizationConfig &quantizationConfig,
    const virtualgeometry::AABB &localBounds,
    const math::Vec3f &position,
    virtualgeometry::VirtualGeometryScene::RuntimeObjectId *outObjectId,
    std::string *error)
{
  virtualgeometry::VirtualGeometryScene::RuntimeObjectCreateInfo createInfo{};
  createInfo.name = objectName;
  createInfo.pageCount = pageCount;
  createInfo.hierarchy = hierarchy;
  createInfo.quantizationConfig = quantizationConfig;
  createInfo.localBounds = localBounds;
  createInfo.position = position;
  return scene.registerRuntimeObject(createInfo, outObjectId, error);
}

bool buildSparseVoxelTerrainVGGeneratedPayloadRequest(
    const SparseVoxelTerrainVGPUPageBuildResult &build,
    const std::string &objectName,
    const uint32_t localPageIndex,
    const uint32_t parentLocalPageIndex,
    const uint32_t fallbackLocalPageIndex,
    const uint32_t unitScaleBits,
    const uint32_t priority,
    virtualgeometry::VirtualGeometryGeneratedPayloadManager::Request &request,
    std::string *error)
{
  request = {};
  if (objectName.empty())
    return fail(error, "terrain VG generated payload request needs an object name");
  if (localPageIndex == UINT32_MAX)
    return fail(error, "terrain VG generated payload request needs a local page index");
  if (build.status != static_cast<uint32_t>(SparseVoxelTerrainVGPUPageBuildStatus::Ready) ||
      build.validationErrors != 0u)
    return fail(error, "terrain VG generated payload request source build is not ready");
  if (build.payloadRevision == 0u || build.geometryRevision == 0u)
    return fail(error, "terrain VG generated payload request source revisions are invalid");
  if (build.vgMeshletCount == 0u || build.vgVertexCount == 0u ||
      build.vgTriangleCount == 0u)
    return fail(error, "terrain VG generated payload request has no decoded raster payload");
  const uint64_t decodedIndexCount =
      static_cast<uint64_t>(build.vgTriangleCount) * 3ull;
  if (decodedIndexCount > std::numeric_limits<uint32_t>::max())
    return fail(error, "terrain VG generated payload request index budget overflows");

  request.objectName = objectName;
  request.localPageIndex = localPageIndex;
  request.parentLocalPageIndex = parentLocalPageIndex;
  request.fallbackLocalPageIndex = fallbackLocalPageIndex;
  request.pageGeneration = build.payloadRevision;
  request.geometryRevision = build.geometryRevision;
  request.materialRevision =
      build.materialRevision != 0u ? build.materialRevision : build.geometryRevision;
  request.unitScaleBits = unitScaleBits;
  // The first real runtime path writes decoded scene-visible buffers directly.
  // Encoded page bytes stay zero until the terrain GPU encoder is plugged into
  // the same reservation descriptor.
  request.encodedPageBytes = 0u;
  request.clusterCount = build.vgMeshletCount;
  request.decodedVertexCount = build.vgVertexCount;
  request.decodedIndexCount = static_cast<uint32_t>(decodedIndexCount);
  request.decodedBoneWeightCount = build.vgBoneWeightWordCount;
  request.priority = priority;
  request.rootPage = build.rootPage;
  request.hasTracePayload = build.traceNodeCount != 0u;
  request.hasMaterialPayload = build.materialCount != 0u || build.materialCellCount != 0u;
  request.requireProtectedParentOrFallback = !build.rootPage;

  if (error != nullptr)
    error->clear();
  return true;
}

bool reserveSparseVoxelTerrainVGGeneratedPayload(
    virtualgeometry::VirtualGeometryGeneratedPayloadManager &manager,
    const SparseVoxelTerrainVGPUPageBuildResult &build,
    const std::string &objectName,
    const uint32_t localPageIndex,
    const uint32_t parentLocalPageIndex,
    const uint32_t fallbackLocalPageIndex,
    const uint32_t unitScaleBits,
    const uint32_t priority,
    virtualgeometry::VirtualGeometryGeneratedPayloadManager::GpuWriteDescriptor &descriptor,
    std::string *error)
{
  descriptor = {};
  virtualgeometry::VirtualGeometryGeneratedPayloadManager::Request request{};
  if (!buildSparseVoxelTerrainVGGeneratedPayloadRequest(
          build,
          objectName,
          localPageIndex,
          parentLocalPageIndex,
          fallbackLocalPageIndex,
          unitScaleBits,
          priority,
          request,
          error))
  {
    return false;
  }
  if (!manager.enqueueRequest(request, error))
    return false;
  std::vector<virtualgeometry::VirtualGeometryGeneratedPayloadManager::GpuWriteDescriptor> descriptors;
  std::vector<std::string> errors;
  const uint32_t reserved = manager.reserveReady(1u, &descriptors, &errors);
  if (reserved == 0u || descriptors.empty())
  {
    if (error != nullptr)
    {
      *error = errors.empty()
          ? "terrain VG generated payload manager did not reserve a page"
          : errors.front();
    }
    return false;
  }
  descriptor = descriptors.front();
  if (error != nullptr)
    error->clear();
  return true;
}

bool publishSparseVoxelTerrainVGGeneratedDecodedPayload(
    virtualgeometry::VirtualGeometryGeneratedPayloadManager &manager,
    const virtualgeometry::VirtualGeometryGeneratedPayloadManager::TicketId ticketId,
    std::string *error)
{
  virtualgeometry::VirtualGeometryGeneratedPayloadManager::PublishInfo publish{};
  publish.ticketId = ticketId;
  publish.gpuWroteEncodedPageData = false;
  publish.gpuWroteDecodedGeometry = true;
  publish.enqueuePageDecode = false;
  return manager.publishCompleted(publish, error);
}

bool installSparseVoxelTerrainVGScenePage(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const SparseVoxelTerrainVGScenePageInstall &install,
    const bool rootPage,
    const uint32_t parentLocalPageIndex,
    const uint32_t fallbackLocalPageIndex,
    std::string *error)
{
  if (!install.readyForSceneInstall)
    return fail(error, "terrain VG scene install descriptor is not ready");

  if (install.sceneVisibleRasterPayloadReady || !install.encodedPageWords.empty())
  {
    if (install.encodedPageWords.empty())
      return fail(error, "terrain VG scene install claims drawable payload without encoded VG page words");

    virtualgeometry::VirtualGeometryScene::RuntimeEncodedPageInstallInfo encodedInstall{};
    encodedInstall.localPageIndex = install.localPageIndex;
    encodedInstall.parentLocalPageIndex = parentLocalPageIndex;
    encodedInstall.fallbackLocalPageIndex = fallbackLocalPageIndex;
    encodedInstall.pageGeneration = install.pageGeneration;
    encodedInstall.geometryRevision = install.geometryRevision;
    encodedInstall.materialRevision = install.materialRevision;
    encodedInstall.unitScaleBits = install.virtualPageAllocation.unitScaleBits;
    encodedInstall.rootPage = rootPage;
    encodedInstall.hasTracePayload = install.hasTracePayload;
    encodedInstall.hasMaterialPayload = install.hasMaterialPayload;
    encodedInstall.requireProtectedParentOrFallback = !rootPage;
    encodedInstall.enqueuePageDecode = install.enqueueScenePageDecode;
    encodedInstall.encodedPageWords = install.encodedPageWords;
    return scene.installRuntimeObjectEncodedPage(
        objectName,
        encodedInstall,
        error);
  }

  virtualgeometry::VirtualGeometryScene::RuntimePageInstallInfo runtimeInstall{};
  runtimeInstall.localPageIndex = install.localPageIndex;
  runtimeInstall.parentLocalPageIndex = parentLocalPageIndex;
  runtimeInstall.fallbackLocalPageIndex = fallbackLocalPageIndex;
  runtimeInstall.pageGeneration = install.pageGeneration;
  runtimeInstall.geometryRevision = install.geometryRevision;
  runtimeInstall.materialRevision = install.materialRevision;
  runtimeInstall.pageAllocation = install.virtualPageAllocation;
  runtimeInstall.pageTableEntry = install.pageTableEntry;
  runtimeInstall.rootPage = rootPage;
  runtimeInstall.hasTracePayload = install.hasTracePayload;
  runtimeInstall.hasMaterialPayload = install.hasMaterialPayload;
  runtimeInstall.requireProtectedParentOrFallback = !rootPage;

  // Terrain GPU pages already publish their payload into the shared terrain
  // page storage. Until the publisher gives VirtualGeometryScene an encoded VG
  // page payload or a decoded-buffer alias, this install updates only the scene
  // page-table/residency contract and deliberately skips decode requests.
  runtimeInstall.writesSceneEncodedPageData = false;
  runtimeInstall.enqueuePageDecode = false;

  return scene.installRuntimeObjectPage(objectName, runtimeInstall, error);
}

bool attachSparseVoxelTerrainVGSceneEncodedPage(
    SparseVoxelTerrainVGScenePageInstall &install,
    const virtualgeometry::VirtualGeometryPage &page,
    std::string *error)
{
  if (!install.readyForSceneInstall)
    return fail(error, "terrain VG scene encoded payload needs a ready install descriptor");
  if (page.meshlets.empty())
    return fail(error, "terrain VG scene encoded payload has no meshlets");

  const virtualgeometry::PageBuffer encoded =
      virtualgeometry::PageBuffer::encode(page);
  if (encoded.data.empty())
    return fail(error, "terrain VG scene encoded payload failed to encode");

  uint64_t vertexCount = 0u;
  uint64_t indexCount = 0u;
  uint64_t boneWeightCount = 0u;
  for (const auto &meshlet : page.meshlets)
  {
    vertexCount += meshlet.vertex_count;
    indexCount += static_cast<uint64_t>(meshlet.triangle_count) * 3u;
    boneWeightCount +=
        static_cast<uint64_t>(meshlet.vertex_count) *
        static_cast<uint64_t>(meshlet.boneWeightsPerVertex);
  }
  if (vertexCount == 0u || indexCount == 0u ||
      vertexCount > std::numeric_limits<uint32_t>::max() ||
      indexCount > std::numeric_limits<uint32_t>::max() ||
      boneWeightCount > std::numeric_limits<uint32_t>::max())
  {
    return fail(error, "terrain VG scene encoded payload decoded geometry budget is invalid");
  }

  install.encodedPageWords = encoded.data;
  install.sceneVisibleRasterPayloadReady = true;
  install.virtualPageAllocation.size =
      std::max<uint32_t>(
          install.virtualPageAllocation.size,
          static_cast<uint32_t>(encoded.data.size() * sizeof(uint32_t)));
  install.virtualPageAllocation.clusterCount =
      static_cast<uint32_t>(page.meshlets.size());
  install.virtualPageAllocation.decodedVertexCount =
      static_cast<uint32_t>(vertexCount);
  install.virtualPageAllocation.decodedIndexCount =
      static_cast<uint32_t>(indexCount);
  install.virtualPageAllocation.decodedBoneWeightCount =
      static_cast<uint32_t>(boneWeightCount);
  install.pageTableEntry.size = install.virtualPageAllocation.size;
  install.pageTableEntry.clusterCount = install.virtualPageAllocation.clusterCount;
  install.pageTableEntry.decodedVertexOffset =
      install.virtualPageAllocation.decodedVertexOffset;
  install.pageTableEntry.decodedIndexOffset =
      install.virtualPageAllocation.decodedIndexOffset;
  install.pageTableEntry.decodedBoneWeightOffset =
      install.virtualPageAllocation.decodedBoneWeightOffset;

  if (error != nullptr)
    error->clear();
  return true;
}

bool installSparseVoxelTerrainVGEncodedAssetRuntimeObject(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const virtualgeometry::VirtualGeometryEncodedData &asset,
    const SparseVoxelTerrainVGEncodedAssetRuntimeInstallSettings &settings,
    SparseVoxelTerrainVGEncodedAssetRuntimeInstallStats *stats,
    std::string *error)
{
  if (stats != nullptr)
    *stats = {};
  if (error != nullptr)
    error->clear();
  if (objectName.empty())
    return fail(error, "terrain virtual-geometry encoded asset needs a runtime object name");
  if (asset.hierarchy.empty())
    return fail(error, "terrain virtual-geometry encoded asset has no hierarchy");
  if (asset.pages.empty())
    return fail(error, "terrain virtual-geometry encoded asset has no pages");
  if (asset.pages.size() > std::numeric_limits<uint32_t>::max())
    return fail(error, "terrain virtual-geometry encoded asset has too many pages");
  if (asset.rootPageIndex >= asset.pages.size())
    return fail(error, "terrain virtual-geometry encoded asset root page is out of range");
  if (settings.basePageGeneration == 0u ||
      settings.geometryRevision == 0u ||
      settings.materialRevision == 0u)
  {
    return fail(error, "terrain virtual-geometry encoded asset revisions must be non-zero");
  }

  virtualgeometry::AABB localBounds{};
  bool anyFiniteBounds = false;
  for (const auto &node : asset.hierarchy)
  {
    const float minPoint[3] = {node.min_x, node.min_y, node.min_z};
    const float maxPoint[3] = {node.max_x, node.max_y, node.max_z};
    const bool valid =
        std::isfinite(minPoint[0]) && std::isfinite(minPoint[1]) &&
        std::isfinite(minPoint[2]) && std::isfinite(maxPoint[0]) &&
        std::isfinite(maxPoint[1]) && std::isfinite(maxPoint[2]) &&
        minPoint[0] <= maxPoint[0] && minPoint[1] <= maxPoint[1] &&
        minPoint[2] <= maxPoint[2];
    if (!valid)
      continue;
    localBounds.expandBy(minPoint);
    localBounds.expandBy(maxPoint);
    anyFiniteBounds = true;
  }
  if (!anyFiniteBounds)
    return fail(error, "terrain virtual-geometry encoded asset has no finite bounds");

  std::vector<virtualgeometry::PageBuffer> encodedPages;
  encodedPages.reserve(asset.pages.size());
  for (uint32_t pageIndex = 0u; pageIndex < asset.pages.size(); ++pageIndex)
  {
    virtualgeometry::PageBuffer encoded =
        virtualgeometry::PageBuffer::encode(asset.pages[pageIndex]);
    if (encoded.data.empty())
      return fail(error, "terrain virtual-geometry encoded asset produced an empty encoded page");
    encodedPages.push_back(std::move(encoded));
  }

  virtualgeometry::VirtualGeometryScene::RuntimeObjectCreateInfo createInfo{};
  createInfo.name = objectName;
  createInfo.pageCount = static_cast<uint32_t>(asset.pages.size());
  createInfo.hierarchy = asset.hierarchy;
  createInfo.quantizationConfig = asset.quantizationConfig;
  createInfo.localBounds = localBounds;
  createInfo.position = settings.position;
  createInfo.materialIndex = settings.materialIndex;

  virtualgeometry::VirtualGeometryScene::RuntimeObjectId objectId =
      virtualgeometry::VirtualGeometryScene::INVALID_RUNTIME_OBJECT_ID;
  if (!scene.registerRuntimeObject(createInfo, &objectId, error))
    return false;

  SparseVoxelTerrainVGEncodedAssetRuntimeInstallStats localStats{};
  localStats.objectId = objectId;
  uint32_t unitScaleBits = 0u;
  std::memcpy(&unitScaleBits, &asset.quantizationConfig.unit_scale, sizeof(unitScaleBits));

  for (uint32_t pageIndex = 0u; pageIndex < asset.pages.size(); ++pageIndex)
  {
    virtualgeometry::VirtualGeometryScene::RuntimeEncodedPageInstallInfo install{};
    install.localPageIndex = pageIndex;
    install.pageGeneration = settings.basePageGeneration + pageIndex;
    install.geometryRevision = settings.geometryRevision;
    install.materialRevision = settings.materialRevision;
    install.unitScaleBits = unitScaleBits;
    install.rootPage = pageIndex <= asset.rootPageIndex;
    install.hasTracePayload = settings.hasTracePayload;
    install.hasMaterialPayload = settings.hasMaterialPayload;
    install.enqueuePageDecode = settings.enqueueScenePageDecode;
    install.encodedPageWords = encodedPages[pageIndex].data;

    if (!scene.installRuntimeObjectEncodedPage(objectName, install, error))
      return false;

    ++localStats.installedPageCount;
    localStats.queuedDecodePageCount += settings.enqueueScenePageDecode ? 1u : 0u;
    localStats.encodedMeshletCount +=
        static_cast<uint32_t>(asset.pages[pageIndex].meshlets.size());
    localStats.encodedPageBytes +=
        static_cast<uint64_t>(encodedPages[pageIndex].data.size()) * sizeof(uint32_t);
  }

  if (stats != nullptr)
    *stats = localStats;
  return true;
}

bool sparseVoxelTerrainVGSceneInstallHasDrawablePayload(
    const SparseVoxelTerrainVGScenePageInstall &install)
{
  return install.readyForSceneInstall &&
      install.sceneVisibleRasterPayloadReady &&
      !install.encodedPageWords.empty();
}

} // namespace voxel::svraster

namespace voxel::svraster
{
namespace
{

virtualgeometry::VirtualGeometryHierarchy makeInertChunkRoot(
    const virtualgeometry::AABB &bounds)
{
  // Degenerate bounds at the object's minimum corner, no children and no page:
  // the culler visits it, finds nothing to expand and nothing to draw. This is
  // what an unoccupied slot looks like, and what a parked one returns to.
  virtualgeometry::VirtualGeometryHierarchy node{};
  node.min_x = bounds.minPoint[0];
  node.min_y = bounds.minPoint[1];
  node.min_z = bounds.minPoint[2];
  node.max_x = bounds.minPoint[0];
  node.max_y = bounds.minPoint[1];
  node.max_z = bounds.minPoint[2];
  node.max_center_x = bounds.minPoint[0];
  node.max_center_y = bounds.minPoint[1];
  node.max_center_z = bounds.minPoint[2];
  node.max_radius = 0.0f;
  node.min_lod_error = 0.0f;
  node.max_parent_lod_error = 0.0f;
  node.child_start = 0u;
  node.child_count = 0u;
  node.pageIndex = UINT32_MAX;
  node.meshPartIndex = UINT32_MAX;
  node.flags = 0u;
  return node;
}

} // namespace

bool buildSparseVoxelTerrainVGSharedObjectHierarchy(
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    const virtualgeometry::AABB &bounds,
    std::vector<virtualgeometry::VirtualGeometryHierarchy> &hierarchy,
    std::string *error)
{
  hierarchy.clear();
  if (error != nullptr)
    error->clear();
  if (!layout.valid())
    return fail(error, "terrain shared virtual-geometry layout is invalid");

  virtualgeometry::VirtualGeometryHierarchy root{};
  root.min_x = bounds.minPoint[0];
  root.min_y = bounds.minPoint[1];
  root.min_z = bounds.minPoint[2];
  root.max_x = bounds.maxPoint[0];
  root.max_y = bounds.maxPoint[1];
  root.max_z = bounds.maxPoint[2];
  root.max_center_x = (root.min_x + root.max_x) * 0.5f;
  root.max_center_y = (root.min_y + root.max_y) * 0.5f;
  root.max_center_z = (root.min_z + root.max_z) * 0.5f;
  const float dx = root.max_x - root.min_x;
  const float dy = root.max_y - root.min_y;
  const float dz = root.max_z - root.min_z;
  root.max_radius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;
  root.min_lod_error = 0.0f;
  // The root must never be selected in place of its children.
  root.max_parent_lod_error = std::numeric_limits<float>::max();
  root.child_start = 1u;
  root.child_count = layout.capacity;
  root.pageIndex = UINT32_MAX;
  root.meshPartIndex = UINT32_MAX;
  root.flags = virtualgeometry::HIERARCHY_FORCE_TRAVERSAL_FLAG;

  hierarchy.assign(layout.hierarchyNodeCount(), makeInertChunkRoot(bounds));
  hierarchy[0] = root;
  return true;
}

bool remapSparseVoxelTerrainVGChunkHierarchy(
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    const uint32_t slot,
    const std::vector<virtualgeometry::VirtualGeometryHierarchy> &chunkHierarchy,
    const std::vector<uint32_t> &pageSlots,
    std::vector<virtualgeometry::VirtualGeometryHierarchy> &chunkRoot,
    std::vector<virtualgeometry::VirtualGeometryHierarchy> &slice,
    std::string *error)
{
  chunkRoot.clear();
  slice.clear();
  if (error != nullptr)
    error->clear();
  if (!layout.valid() || slot >= layout.capacity)
    return fail(error, "terrain shared virtual-geometry slot is out of range");
  if (chunkHierarchy.empty())
    return fail(error, "terrain chunk hierarchy is empty");
  if (chunkHierarchy.size() > layout.maxChunkNodes)
    return fail(
        error,
        "terrain chunk hierarchy exceeds the per-slot reservation: " +
            std::to_string(chunkHierarchy.size()) + " > " +
            std::to_string(layout.maxChunkNodes));

  auto remapped = chunkHierarchy;
  for (size_t index = 0u; index < remapped.size(); ++index)
  {
    auto &node = remapped[index];
    // Only an interior node's child run names hierarchy nodes. A leaf's
    // child_start is a page-relative *cluster* index, and the page keeps its
    // own slot, so shifting it would point the leaf at another chunk's
    // geometry. Running the unshifted version is what caught this: real assets
    // have leaves whose cluster run starts at 0 and others that run past the
    // node count.
    const bool interior =
        (node.flags & virtualgeometry::HIERARCHY_LEAF_FLAG) == 0u;
    if (interior && node.child_count != 0u)
    {
      // An interior node's children are always at index >= 1, so a contiguous
      // run in chunk space stays contiguous in the slice. A run beginning at
      // the chunk root would not, and cannot occur in a tree rooted at 0.
      if (node.child_start == 0u)
        return fail(
            error, "terrain chunk hierarchy names its own root as a child");
      const uint64_t last =
          static_cast<uint64_t>(node.child_start) + node.child_count - 1u;
      if (last >= chunkHierarchy.size())
        return fail(error, "terrain chunk hierarchy child run is out of range");
      node.child_start = layout.nodeIndex(slot, node.child_start);
    }
    if (node.pageIndex != UINT32_MAX)
    {
      const uint32_t assetPage =
          node.pageIndex & ~virtualgeometry::PAGE_NOT_INSTALLED_BIT;
      if (assetPage >= pageSlots.size())
        return fail(
            error, "terrain chunk hierarchy names a page the slot does not own");
      const uint32_t installedBit =
          node.pageIndex & virtualgeometry::PAGE_NOT_INSTALLED_BIT;
      node.pageIndex = installedBit | pageSlots[assetPage];
    }
  }

  chunkRoot.assign(1u, remapped[0]);
  slice.assign(remapped.begin() + 1, remapped.end());
  return true;
}

std::vector<virtualgeometry::HierarchyClusterFlagsUpdate>
remapSparseVoxelTerrainVGChunkRewrites(
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    const uint32_t slot,
    const std::vector<virtualgeometry::HierarchyClusterFlagsUpdate> &updates)
{
  std::vector<virtualgeometry::HierarchyClusterFlagsUpdate> remapped;
  remapped.reserve(updates.size());
  for (const auto &update : updates)
  {
    if (update.hierarchyNodeIndex >= layout.maxChunkNodes)
      continue;
    auto copy = update;
    copy.hierarchyNodeIndex = layout.nodeIndex(slot, update.hierarchyNodeIndex);
    remapped.push_back(copy);
  }
  return remapped;
}

bool parkSparseVoxelTerrainVGChunkSlot(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    const uint32_t slot,
    const virtualgeometry::AABB &bounds,
    std::string *error)
{
  if (!layout.valid() || slot >= layout.capacity)
    return fail(error, "terrain shared virtual-geometry slot is out of range");
  // Only the chunk root has to go inert. The slice behind it is unreachable
  // once nothing points at it, and leaving it costs nothing but keeps the
  // uninstall to one node write.
  const std::vector<virtualgeometry::VirtualGeometryHierarchy> inert{
    makeInertChunkRoot(bounds)};
  return scene.writeRuntimeObjectHierarchyRange(
      objectName, layout.nodeIndex(slot, 0u), inert, error);
}

bool setSparseVoxelTerrainVGChunkRootParentError(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    const uint32_t slot,
    virtualgeometry::VirtualGeometryHierarchy &chunkRoot,
    const float parentError,
    std::string *error)
{
  if (!layout.valid() || slot >= layout.capacity)
    return fail(error, "terrain shared virtual-geometry slot is out of range");
  if (!scene.setRuntimeObjectHierarchyParentError(
          objectName, layout.nodeIndex(slot, 0u), parentError, error))
    return false;
  chunkRoot.max_parent_lod_error = parentError;
  return true;
}

namespace
{

// The install descriptor for one page of a chunk, however its words arrive.
bool buildSparseVoxelTerrainVGChunkPageInstall(
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    const uint32_t slot,
    const std::vector<uint32_t> &pageSlots,
    const SparseVoxelTerrainVGChunkPageBytes &page,
    const uint32_t pageIndex,
    const uint32_t rootPageIndex,
    const uint32_t unitScaleBits,
    const SparseVoxelTerrainVGEncodedAssetRuntimeInstallSettings &settings,
    virtualgeometry::VirtualGeometryScene::RuntimeEncodedPageInstallInfo &install,
    std::string *error)
{
  if (pageIndex >= pageSlots.size())
    return fail(error, "terrain chunk page index is out of range");
  const bool haveWords = page.encodedWords != nullptr && !page.encodedWords->empty();
  if (!haveWords && !page.location.valid())
    return fail(error, "terrain chunk page has neither encoded words nor a page location");

  install = {};
  install.localPageIndex = pageSlots[pageIndex];
  install.parentLocalPageIndex = UINT32_MAX;
  install.fallbackLocalPageIndex = UINT32_MAX;
  install.pageGeneration = settings.basePageGeneration + pageIndex;
  install.geometryRevision = settings.geometryRevision;
  install.materialRevision = settings.materialRevision;
  install.rootPage = pageIndex <= rootPageIndex;
  install.hasTracePayload = settings.hasTracePayload;
  install.hasMaterialPayload = settings.hasMaterialPayload;
  install.enqueuePageDecode = settings.enqueueScenePageDecode;
  install.unitScaleBits = unitScaleBits;
  if (haveWords)
    install.encodedPageWords = *page.encodedWords;
  install.location = page.location;
  if (page.dependencies != nullptr)
  {
    install.dependencies.reserve(page.dependencies->size());
    for (const uint32_t dependency : *page.dependencies)
      if (dependency < pageSlots.size())
        install.dependencies.push_back(pageSlots[dependency]);
  }
  // Without these every cluster on the page is treated as a streaming leaf,
  // its self error is forced to zero, and every LOD level draws at once.
  if (page.installUpdates != nullptr)
    install.hierarchyClusterFlagUpdates =
        remapSparseVoxelTerrainVGChunkRewrites(layout, slot, *page.installUpdates);
  // This page's leaf can only be in this chunk's own nodes: the chunk root
  // stub and the slice behind it.
  install.hierarchyScanRanges = {
    {layout.nodeIndex(slot, 0u), 1u},
    {layout.nodeIndex(slot, 1u), layout.maxChunkNodes - 1u}};
  return true;
}

} // namespace

bool installSparseVoxelTerrainVGChunkPageBytes(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    const uint32_t slot,
    const std::vector<uint32_t> &pageSlots,
    const SparseVoxelTerrainVGChunkPageBytes &page,
    const uint32_t pageIndex,
    const uint32_t rootPageIndex,
    const uint32_t unitScaleBits,
    const SparseVoxelTerrainVGEncodedAssetRuntimeInstallSettings &settings,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  virtualgeometry::VirtualGeometryScene::RuntimeEncodedPageInstallInfo install{};
  if (!buildSparseVoxelTerrainVGChunkPageInstall(
          layout, slot, pageSlots, page, pageIndex, rootPageIndex, unitScaleBits,
          settings, install, error))
    return false;
  // Ancestor closure, checked before the page lands rather than repaired
  // after it - and checked as one predecessor test, not a walk.
  //
  // A page's dependencies are the pages holding the DAG parents of its
  // clusters, and the builder emits them so that a page is only ever
  // installable once they are resident: the streaming-leaf marks on those
  // parents are what the culler descends through to reach this page's nodes,
  // and the rewrites this install carries clear them. Installing out of order
  // leaves hierarchy nodes the culler can reach with no geometry behind them.
  //
  // The scene reference-counts dependencies so eviction cannot break a chain,
  // but it does not check them at install, so nothing stopped a caller
  // installing a page whose parents were missing. `installTerrainVGRequestedPages`
  // then found chunks in that state and repaired them from the root down -
  // 20 repairs per 300-frame walk, and the repair is a safety net rather than
  // the invariant.
  //
  // Why the predecessor alone is the whole check: the packer's page order is a
  // topological order of the page DAG. It assigns groups by expanding the
  // cluster cut from the roots, so a cluster's DAG parents always land on an
  // earlier page, and it chains every page to its predecessor - page i's
  // dependencies lie in [0, i) and i-1 is among them
  // (`VirtualGeometryPackerDeterminism::testPageOrderIsTopological` asserts
  // both, on both routes). Pages therefore install as a prefix, and a prefix
  // is intact exactly when its last member is: i-1 being resident means i-2
  // was resident when i-1 installed, and the dependency count has protected it
  // ever since. One lookup replaces one per dependency.
  if (pageIndex != 0u)
  {
    const uint32_t predecessor = pageSlots[pageIndex - 1u];
    const auto *status = scene.getRuntimeObjectPageStatus(objectName, predecessor);
    if (status == nullptr || !status->installed)
      return fail(
          error,
          "terrain chunk page " + std::to_string(pageIndex) +
              " follows page slot " + std::to_string(predecessor) +
              ", which is not installed; install a chunk's pages in order");
  }
  return scene.installRuntimeObjectEncodedPage(objectName, install, error);
}

bool primeSparseVoxelTerrainVGChunkPageSource(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    const uint32_t slot,
    const std::vector<uint32_t> &pageSlots,
    const SparseVoxelTerrainVGChunkPageBytes &page,
    const uint32_t pageIndex,
    const uint32_t rootPageIndex,
    const uint32_t unitScaleBits,
    const SparseVoxelTerrainVGEncodedAssetRuntimeInstallSettings &settings,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!page.location.valid())
    return fail(error, "terrain chunk page has no page location to prime");
  virtualgeometry::VirtualGeometryScene::RuntimeEncodedPageInstallInfo install{};
  if (!buildSparseVoxelTerrainVGChunkPageInstall(
          layout, slot, pageSlots, page, pageIndex, rootPageIndex, unitScaleBits,
          settings, install, error))
    return false;
  // The words are not needed to prime, and copying them would be the CPU-side
  // page the location exists to remove.
  install.encodedPageWords.clear();
  return scene.primeRuntimeObjectPageSource(objectName, install, error);
}

bool installSparseVoxelTerrainVGChunkPage(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    const uint32_t slot,
    const std::vector<uint32_t> &pageSlots,
    const virtualgeometry::VirtualGeometryEncodedData &asset,
    const uint32_t pageIndex,
    const SparseVoxelTerrainVGEncodedAssetRuntimeInstallSettings &settings,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (pageIndex >= asset.pages.size() || pageSlots.size() != asset.pages.size())
    return fail(error, "terrain chunk page index is out of range");
  // The decoder reconstructs vertex positions with this scale (see the slot
  // install).
  uint32_t chunkUnitScaleBits = 0u;
  std::memcpy(
      &chunkUnitScaleBits, &asset.quantizationConfig.unit_scale,
      sizeof(chunkUnitScaleBits));
  const auto &sourcePage = asset.pages[pageIndex];
  const virtualgeometry::PageBuffer encoded =
      virtualgeometry::PageBuffer::encode(sourcePage);
  SparseVoxelTerrainVGChunkPageBytes page{};
  page.encodedWords = &encoded.data;
  page.dependencies = &sourcePage.dependencies;
  page.installUpdates = &sourcePage.installUpdates.hierarchyUpdates;
  return installSparseVoxelTerrainVGChunkPageBytes(
      scene, objectName, layout, slot, pageSlots, page, pageIndex,
      asset.rootPageIndex, chunkUnitScaleBits, settings, error);
}

bool installSparseVoxelTerrainVGChunkIntoSlotFromBytes(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    const uint32_t slot,
    const std::vector<uint32_t> &pageSlots,
    const std::vector<virtualgeometry::VirtualGeometryHierarchy> &chunkHierarchy,
    const uint32_t pageCount,
    const uint32_t rootPageIndex,
    const uint32_t unitScaleBits,
    const std::function<bool(uint32_t, SparseVoxelTerrainVGChunkPageBytes &, std::string *)> &fetchPage,
    const SparseVoxelTerrainVGEncodedAssetRuntimeInstallSettings &settings,
    std::string *error,
    virtualgeometry::VirtualGeometryHierarchy *chunkRootOut)
{
  if (error != nullptr)
    error->clear();
  if (pageCount == 0u)
    return fail(error, "terrain chunk asset has no pages");
  if (pageSlots.size() != static_cast<size_t>(pageCount))
    return fail(
        error,
        "terrain chunk owns " + std::to_string(pageSlots.size()) +
            " page slots for " + std::to_string(pageCount) + " pages");

  std::vector<virtualgeometry::VirtualGeometryHierarchy> chunkRoot;
  std::vector<virtualgeometry::VirtualGeometryHierarchy> slice;
  if (!remapSparseVoxelTerrainVGChunkHierarchy(
          layout, slot, chunkHierarchy, pageSlots, chunkRoot, slice, error))
    return false;

  // Hierarchy first, then the pages. The runtime clears a leaf's
  // PAGE_NOT_INSTALLED_BIT *at page install* by scanning the object's hierarchy
  // for the leaf that names that page; a page installed before its leaf exists
  // finds nothing to clear, and the leaf written afterwards keeps the bit the
  // asset encoded it with. The culler admits a leaf's clusters only when that
  // bit is clear and never consults the page table, so the original order -
  // pages first - installed 429 pages cleanly and drew zero pixels.
  //
  // Publishing the slice before its pages is safe precisely because of that
  // bit: until each page lands, its leaf reads as not installed and the culler
  // stops there.
  if (!slice.empty() &&
      !scene.writeRuntimeObjectHierarchyRange(
          objectName, layout.nodeIndex(slot, 1u), slice, error))
    return false;
  chunkRoot[0].max_parent_lod_error = settings.rootParentError;
  if (chunkRootOut != nullptr)
    *chunkRootOut = chunkRoot[0];
  if (!scene.writeRuntimeObjectHierarchyRange(
          objectName, layout.nodeIndex(slot, 0u), chunkRoot, error))
    return false;

  // A chunk install is all or nothing.
  //
  // Stopping half-way used to leave the pages it had already installed behind.
  // The page set then does not call the slot installed while the scene reports
  // its pages installed, and the recovery for that disagreement - "slot N has
  // installed pages outside its record" - retires them, after the chunk they
  // belonged to has been dropped. That is the path that leaves ground bare:
  // measured at `--terrain-vg-memory-mb 160`, 469 install failures and 469
  // chunks with nothing drawing on them, every failure the transient
  // "buffers cannot fit the encoded page ... retry next frame".
  std::vector<uint32_t> installedPages;
  installedPages.reserve(pageCount);
  const auto rollback = [&]()
  {
    for (auto it = installedPages.rbegin(); it != installedPages.rend(); ++it)
    {
      const uint32_t installed = *it;
      std::string uninstallError;
      // Nothing to do if the rollback itself fails: the caller is already on
      // its error path, and the recovery in the apply loop still covers it.
      (void)scene.uninstallRuntimeObjectPage(
          objectName, pageSlots[installed], 0u, &uninstallError);
    }
    // A slot that is given back must not keep locations the next chunk in it
    // could be served from.
    for (const uint32_t pageSlot : pageSlots)
      scene.forgetRuntimeObjectPageSource(objectName, pageSlot);
  };
  // Up to and including the root page, not the root page alone.
  //
  // A page's dependencies are the pages holding the DAG parents of its
  // clusters, and the packer places those earlier - so a page is installable
  // only once its whole prefix is. `rootPageIndex` is not always 0, and
  // installing it by itself left its dependencies missing: 2,278 refusals per
  // 300-frame walk once the install started checking, and before that the same
  // number of chunks reaching a state `installTerrainVGRequestedPages` had to
  // repair from the root down.
  const uint32_t lastRequiredPage =
      settings.installAllPages ? pageCount - 1u : rootPageIndex;
  for (uint32_t pageIndex = 0u; pageIndex <= lastRequiredPage; ++pageIndex)
  {
    SparseVoxelTerrainVGChunkPageBytes page{};
    if (!fetchPage(pageIndex, page, error))
    {
      rollback();
      return false;
    }
    if (!installSparseVoxelTerrainVGChunkPageBytes(
            scene, objectName, layout, slot, pageSlots, page, pageIndex,
            rootPageIndex, unitScaleBits, settings, error))
    {
      rollback();
      return false;
    }
    installedPages.push_back(pageIndex);
  }
  // Phase 8g: the pages left for the culler are primed with their locations,
  // so the scene installs them itself when they are requested, in dependency
  // order, and brings them back after evicting them. A fetch that yields no
  // location (an asset-backed chunk) primes nothing, and a prime that fails
  // costs only the caller-served path it falls back to.
  for (uint32_t pageIndex = lastRequiredPage + 1u; pageIndex < pageCount; ++pageIndex)
  {
    SparseVoxelTerrainVGChunkPageBytes page{};
    std::string primeError;
    if (!fetchPage(pageIndex, page, &primeError) || !page.location.valid())
      continue;
    (void)primeSparseVoxelTerrainVGChunkPageSource(
        scene, objectName, layout, slot, pageSlots, page, pageIndex,
        rootPageIndex, unitScaleBits, settings, &primeError);
  }
  return true;
}

bool installSparseVoxelTerrainVGChunkIntoSlot(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    const uint32_t slot,
    const std::vector<uint32_t> &pageSlots,
    const virtualgeometry::VirtualGeometryEncodedData &asset,
    const SparseVoxelTerrainVGEncodedAssetRuntimeInstallSettings &settings,
    std::string *error,
    virtualgeometry::VirtualGeometryHierarchy *chunkRootOut)
{
  // The decoder reconstructs vertex positions with this scale. Leaving it at
  // its default zero collapses every vertex to the origin: the object installs
  // cleanly, reports all its pages traceable, and draws nothing at all.
  uint32_t chunkUnitScaleBits = 0u;
  std::memcpy(
      &chunkUnitScaleBits, &asset.quantizationConfig.unit_scale,
      sizeof(chunkUnitScaleBits));
  // The encoded words have to outlive the install call that reads them.
  virtualgeometry::PageBuffer encoded;
  const auto fetchPage =
      [&](const uint32_t pageIndex, SparseVoxelTerrainVGChunkPageBytes &page,
          std::string *fetchError) -> bool
  {
    if (pageIndex >= asset.pages.size())
    {
      if (fetchError != nullptr)
        *fetchError = "terrain chunk page index is out of range";
      return false;
    }
    const auto &sourcePage = asset.pages[pageIndex];
    encoded = virtualgeometry::PageBuffer::encode(sourcePage);
    page.encodedWords = &encoded.data;
    page.dependencies = &sourcePage.dependencies;
    page.installUpdates = &sourcePage.installUpdates.hierarchyUpdates;
    return true;
  };
  return installSparseVoxelTerrainVGChunkIntoSlotFromBytes(
      scene, objectName, layout, slot, pageSlots, asset.hierarchy,
      static_cast<uint32_t>(asset.pages.size()), asset.rootPageIndex,
      chunkUnitScaleBits, fetchPage, settings, error, chunkRootOut);
}

} // namespace voxel::svraster


namespace voxel::svraster
{

std::vector<uint8_t> sparseVoxelTerrainVGChunkNodeQuadrants(
    const std::vector<virtualgeometry::VirtualGeometryHierarchy> &chunkHierarchy,
    const float centerX,
    const float centerZ)
{
  std::vector<uint8_t> quadrants(chunkHierarchy.size(), 0u);
  for (size_t index = 0u; index < chunkHierarchy.size(); ++index)
  {
    const auto &node = chunkHierarchy[index];
    // A node's box can straddle the centre, so test both halves on each axis
    // and take every quadrant it reaches.
    const bool negativeX = node.min_x < centerX;
    const bool positiveX = node.max_x >= centerX;
    const bool negativeZ = node.min_z < centerZ;
    const bool positiveZ = node.max_z >= centerZ;
    uint8_t mask = 0u;
    if (negativeX && negativeZ)
      mask |= 1u << 0u;
    if (positiveX && negativeZ)
      mask |= 1u << 1u;
    if (negativeX && positiveZ)
      mask |= 1u << 2u;
    if (positiveX && positiveZ)
      mask |= 1u << 3u;
    quadrants[index] = mask;
  }
  return quadrants;
}

SparseVoxelTerrainVGChunkClusterQuadrants
sparseVoxelTerrainVGChunkClusterQuadrantsFromBlocks(
    const std::vector<virtualgeometry::VirtualGeometryHierarchy> &chunkHierarchy,
    const std::vector<std::array<float, 4>> &blocks,
    const float centerX,
    const float centerZ,
    const std::vector<SparseVoxelTerrainVGClusterSurface> *surfaces)
{
  SparseVoxelTerrainVGChunkClusterQuadrants result;
  result.byNode.assign(
      chunkHierarchy.size(), std::array<SparseVoxelTerrainVGClusterBlock, 8>{});
  for (size_t node = 0u; node < chunkHierarchy.size(); ++node)
  {
    const auto &entry = chunkHierarchy[node];
    if ((entry.flags & virtualgeometry::HIERARCHY_LEAF_FLAG) == 0u)
      continue;
    const uint32_t count = std::min<uint32_t>(entry.child_count, 8u);
    for (uint32_t cluster = 0u; cluster < count; ++cluster)
    {
      const size_t index = static_cast<size_t>(entry.child_start) + cluster;
      if (index >= blocks.size())
        continue;
      const float extent = blocks[index][3];
      if (!(extent > 0.0f))
        continue;
      // One box answers both halves. The octree block cube is the fallback;
      // the cluster's own geometry is what it actually covers.
      const bool haveSurface = surfaces != nullptr && index < surfaces->size() &&
          (*surfaces)[index].valid;
      float minimumX = blocks[index][0];
      float minimumZ = blocks[index][2];
      float maximumX = minimumX + extent;
      float maximumZ = minimumZ + extent;
      if (haveSurface)
      {
        const auto &surface = (*surfaces)[index];
        minimumX = surface.minimum[0];
        minimumZ = surface.minimum[2];
        maximumX = surface.maximum[0];
        maximumZ = surface.maximum[2];
      }
      // Same conservative straddle rule as the node form.
      const bool negativeX = minimumX < centerX;
      const bool positiveX = maximumX >= centerX;
      const bool negativeZ = minimumZ < centerZ;
      const bool positiveZ = maximumZ >= centerZ;
      uint8_t mask = 0u;
      if (negativeX && negativeZ)
        mask |= 1u << 0u;
      if (positiveX && negativeZ)
        mask |= 1u << 1u;
      if (negativeX && positiveZ)
        mask |= 1u << 2u;
      if (positiveX && positiveZ)
        mask |= 1u << 3u;
      auto &block = result.byNode[node][cluster];
      block.quadrants = mask;
    }
  }
  return result;
}

std::vector<virtualgeometry::HierarchyClusterFlagsUpdate>
sparseVoxelTerrainVGChunkRewritesForResidentChildrenPerCluster(
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    const uint32_t slot,
    const std::vector<virtualgeometry::HierarchyClusterFlagsUpdate> &baseUpdates,
    const SparseVoxelTerrainVGChunkClusterQuadrants &clusterQuadrants,
    const std::array<SparseVoxelTerrainVGChildCoverage, 4> &children,
    SparseVoxelTerrainVGYieldBlockStats *stats,
    const bool replaceCoveredClusters)
{
  std::vector<virtualgeometry::HierarchyClusterFlagsUpdate> result;
  result.reserve(baseUpdates.size());
  for (const auto &update : baseUpdates)
  {
    if (update.hierarchyNodeIndex >= layout.maxChunkNodes)
      continue;
    auto adjusted = update;
    if (update.hierarchyNodeIndex < clusterQuadrants.byNode.size())
    {
      const auto &blocks = clusterQuadrants.byNode[update.hierarchyNodeIndex];
      uint8_t cleared = adjusted.streamingLeafsBitset;
      for (uint32_t cluster = 0u; cluster < 8u; ++cluster)
      {
        const auto &block = blocks[cluster];
        // A cluster with no recorded block keeps its bit: never clear a
        // boundary whose coverage is unknown.
        const bool offered = ((adjusted.streamingLeafsBitset >> cluster) & 1u) != 0u;
        if (block.quadrants == 0u)
        {
          if (stats != nullptr && offered)
          {
            ++stats->offered;
            ++stats->blockedUnknownBlock;
          }
          continue;
        }
        if (stats != nullptr && offered)
          ++stats->offered;
        // Residency, and nothing else.
        //
        // There was a vertical containment test here - the child had to reach
        // every height the parent's block did. It was answering a question
        // that cannot arise: a parent brick *is* the OR reduction of its eight
        // children and the mesher emits all six faces of every brick, so a
        // parent's cluster and the child bricks under it cover exactly the
        // same volume. The four child chunks tile the parent's volume the same
        // way. There is no height a parent reaches that its children do not.
        //
        // It cost 96% of the yields whose children were all resident, and six
        // separate corrections to how its two boxes were derived moved that by
        // 5.9% - which is what tuning a test that should not exist looks like.
        bool covered = true;
        for (uint32_t quadrant = 0u; quadrant < 4u && covered; ++quadrant)
        {
          if ((block.quadrants & (1u << quadrant)) == 0u)
            continue;
          covered = children[quadrant].installed;
        }
        if (covered)
        {
          cleared = static_cast<uint8_t>(cleared & ~(1u << cluster));
          if (replaceCoveredClusters)
            adjusted.enabledClustersBitset = static_cast<uint8_t>(
                adjusted.enabledClustersBitset & ~(1u << cluster));
          if (stats != nullptr && offered)
            ++stats->cleared;
        }
        else if (stats != nullptr && offered)
          ++stats->blockedNotInstalled;
      }
      adjusted.streamingLeafsBitset = cleared;
    }
    adjusted.hierarchyNodeIndex = layout.nodeIndex(slot, update.hierarchyNodeIndex);
    result.push_back(adjusted);
  }
  return result;
}

std::vector<virtualgeometry::HierarchyClusterFlagsUpdate>
sparseVoxelTerrainVGChunkRewritesForResidentChildren(
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    const uint32_t slot,
    const std::vector<virtualgeometry::HierarchyClusterFlagsUpdate> &baseUpdates,
    const std::vector<uint8_t> &nodeQuadrants,
    const uint32_t residentChildMask,
    const bool replaceCoveredClusters)
{
  std::vector<virtualgeometry::HierarchyClusterFlagsUpdate> result;
  result.reserve(baseUpdates.size());
  for (const auto &update : baseUpdates)
  {
    if (update.hierarchyNodeIndex >= layout.maxChunkNodes)
      continue;
    auto adjusted = update;
    const uint8_t touched = update.hierarchyNodeIndex < nodeQuadrants.size()
        ? nodeQuadrants[update.hierarchyNodeIndex]
        : 0u;
    // Every quadrant this node reaches is resident, so nothing it holds is a
    // streaming boundary any more. `enabledClustersBitset` is untouched: the
    // parent's clusters stay in the cut and the culler's error test decides
    // whether a pixel gets them or the child's.
    if (touched != 0u && (touched & ~static_cast<uint8_t>(residentChildMask)) == 0u)
    {
      adjusted.streamingLeafsBitset = 0u;
      if (replaceCoveredClusters)
        adjusted.enabledClustersBitset = 0u;
    }
    adjusted.hierarchyNodeIndex = layout.nodeIndex(slot, update.hierarchyNodeIndex);
    result.push_back(adjusted);
  }
  return result;
}

} // namespace voxel::svraster
