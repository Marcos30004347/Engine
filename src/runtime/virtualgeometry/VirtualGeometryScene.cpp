#include "VirtualGeometryScene.hpp"
#include "os/Logger.hpp"
#include "rendering/animation/AnimationPlayer.hpp"
#include "virtualgeometry/VirtualMaterialFile.hpp"
#include <string>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <queue>
#include <unordered_map>
#include <stdexcept>

namespace virtualgeometry
{

namespace fs = std::filesystem;

static constexpr uint64_t kDefaultMeshPartTransformBufferSize = sizeof(math::Mat4f) * 1024ull * 64ull;

// Page-table and priority-slot capacity. Both are indexed by the culler, so
// this is also the ceiling on simultaneously registered pages.
static constexpr uint32_t kMaxScenePages = 65536u;

namespace
{
constexpr uint32_t kDescVertexCount = 8u;
constexpr uint32_t kDescTriangleCount = 9u;
constexpr uint32_t kDescBoneWeightsPerVertex = 32u;

bool setError(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

struct PageDecodedGeometryBudget
{
  uint32_t vertexCount = 0u;
  uint32_t indexCount = 0u;
  uint32_t boneWeightCount = 0u;
};

// Every instance write goes through here: the instance buffers are sized by
// Settings::instanceCapacity, and a write past the end would corrupt whatever
// the RHI placed after them.
static void ensureInstanceBufferFits(uint64_t endByte, uint64_t instanceBufferSize)
{
  if (endByte > instanceBufferSize)
    throw std::runtime_error(
        "VirtualGeometryScene: instance capacity exceeded (" + std::to_string(endByte) + " > " +
        std::to_string(instanceBufferSize) + " bytes); raise Settings::instanceCapacity");
}

uint64_t computeDecodedVertexCapacity(uint64_t pagesBufferSize)
{
  return std::max<uint64_t>(1u, (pagesBufferSize * 2u) / sizeof(DecodedVertexData));
}

uint64_t computeDecodedIndexCapacity(uint64_t pagesBufferSize)
{
  return std::max<uint64_t>(1u, pagesBufferSize / sizeof(uint32_t));
}

uint64_t computeDecodedBoneWeightCapacity(uint64_t pagesBufferSize)
{
  return std::max<uint64_t>(1u, pagesBufferSize / sizeof(DecodedBoneWeightData));
}

PageDecodedGeometryBudget computeDecodedGeometryBudget(
    const uint32_t *words,
    const size_t wordCount)
{
  PageDecodedGeometryBudget budget{};
  if (words == nullptr || wordCount < PAGE_HEADER_WORDS)
    return budget;

  const uint32_t meshletCount = words[0];
  if (wordCount < PAGE_HEADER_WORDS +
          static_cast<size_t>(meshletCount) * MESHLET_DESC_WORDS)
    return budget;

  uint64_t totalVertexCount = 0u;
  uint64_t totalIndexCount = 0u;
  uint64_t totalBoneWeightCount = 0u;
  for (uint32_t meshletIndex = 0u; meshletIndex < meshletCount; ++meshletIndex)
  {
    const uint32_t descBase = PAGE_HEADER_WORDS + meshletIndex * MESHLET_DESC_WORDS;
    const uint32_t vertexCount = words[descBase + kDescVertexCount];
    const uint32_t triangleCount = words[descBase + kDescTriangleCount];
    const uint32_t boneWeightsPerVertex = words[descBase + kDescBoneWeightsPerVertex];

    totalVertexCount += vertexCount;
    totalIndexCount += triangleCount * 3u;
    totalBoneWeightCount += static_cast<uint64_t>(vertexCount) * boneWeightsPerVertex;
  }

  budget.vertexCount = static_cast<uint32_t>(std::min<uint64_t>(totalVertexCount, std::numeric_limits<uint32_t>::max()));
  budget.indexCount = static_cast<uint32_t>(std::min<uint64_t>(totalIndexCount, std::numeric_limits<uint32_t>::max()));
  budget.boneWeightCount = static_cast<uint32_t>(std::min<uint64_t>(totalBoneWeightCount, std::numeric_limits<uint32_t>::max()));
  return budget;
}

PageDecodedGeometryBudget computeDecodedGeometryBudget(const VirtualGeometryStreamingManager::DecodedPagePayload &payload)
{
  if (!payload.isValid() || payload.bytes == nullptr || payload.dataSize < PAGE_HEADER_WORDS * sizeof(uint32_t))
    return {};

  const uint32_t wordCount = payload.dataSize / sizeof(uint32_t);
  return computeDecodedGeometryBudget(
      reinterpret_cast<const uint32_t *>(payload.bytes->data()),
      wordCount);
}

AABB makeNodeLocalBounds(const VirtualGeometryHierarchy &node)
{
  AABB bounds;
  bounds.minPoint[0] = node.min_x;
  bounds.minPoint[1] = node.min_y;
  bounds.minPoint[2] = node.min_z;
  bounds.maxPoint[0] = node.max_x;
  bounds.maxPoint[1] = node.max_y;
  bounds.maxPoint[2] = node.max_z;

  if (node.meshPartIndex != UINT32_MAX && node.max_radius > 0.0f)
  {
    const float parentMin[3] = {
      node.max_center_x - node.max_radius,
      node.max_center_y - node.max_radius,
      node.max_center_z - node.max_radius,
    };
    const float parentMax[3] = {
      node.max_center_x + node.max_radius,
      node.max_center_y + node.max_radius,
      node.max_center_z + node.max_radius,
    };
    bounds.expandBy(parentMin);
    bounds.expandBy(parentMax);
  }

  return bounds;
}

AABB computeHierarchyBounds(const std::vector<VirtualGeometryHierarchy> &hierarchy)
{
  AABB bounds;
  for (const VirtualGeometryHierarchy &node : hierarchy)
  {
    bounds.expandBy(makeNodeLocalBounds(node));
  }
  return bounds;
}

AABB transformAABB(const AABB &localBounds, const math::Mat4f &transform)
{
  AABB transformedBounds;
  for (uint32_t cornerIndex = 0u; cornerIndex < 8u; ++cornerIndex)
  {
    const math::Vec3f localCorner(
        (cornerIndex & 1u) ? localBounds.maxPoint[0] : localBounds.minPoint[0],
        (cornerIndex & 2u) ? localBounds.maxPoint[1] : localBounds.minPoint[1],
        (cornerIndex & 4u) ? localBounds.maxPoint[2] : localBounds.minPoint[2]);
    const math::Vec4f worldCorner = transform * math::Vec4f(localCorner[0], localCorner[1], localCorner[2], 1.0f);
    const float point[3] = {worldCorner[0], worldCorner[1], worldCorner[2]};
    transformedBounds.expandBy(point);
  }
  return transformedBounds;
}

bool isValidAABB(const AABB &bounds)
{
  return bounds.minPoint[0] <= bounds.maxPoint[0] &&
         bounds.minPoint[1] <= bounds.maxPoint[1] &&
         bounds.minPoint[2] <= bounds.maxPoint[2];
}

bool matricesApproximatelyEqual(const math::Mat4f &a, const math::Mat4f &b, float epsilon = 1.0e-5f)
{
  for (size_t index = 0u; index < 16u; ++index)
  {
    if (std::abs(a.data[index] - b.data[index]) > epsilon)
    {
      return false;
    }
  }
  return true;
}

bool palettesApproximatelyEqual(const std::vector<math::Mat4f> &a, const std::vector<math::Mat4f> &b, float epsilon = 1.0e-5f)
{
  if (a.size() != b.size())
  {
    return false;
  }

  for (size_t index = 0u; index < a.size(); ++index)
  {
    if (!matricesApproximatelyEqual(a[index], b[index], epsilon))
    {
      return false;
    }
  }

  return true;
}

AABB computeWorldBoundsForSceneInstances(
    const VirtualGeometryScene::InstanceManager &instanceManager,
    const std::vector<SceneInstanceId> &sceneInstanceIds,
    const AABB &localBounds)
{
  AABB worldBounds;
  for (SceneInstanceId sceneInstanceId : sceneInstanceIds)
  {
    const VirtualGeometryInstanceGPUData *instance = instanceManager.getInstance(sceneInstanceId);
    if (instance == nullptr)
    {
      continue;
    }

    worldBounds.expandBy(transformAABB(localBounds, instance->modelMatrix));
  }

  return worldBounds;
}

void notifyInvalidationIfNeeded(const std::function<void(const AABB &)> &callback, const AABB &bounds)
{
  if (callback && isValidAABB(bounds))
  {
    callback(bounds);
  }
}

} // namespace

static math::Mat4f buildModelMatrix(const math::Vec3f &position, const math::Quatf &rotation, const math::Vec3f &scale)
{
  return math::Mat4f::translate(position) * math::Mat4f::fromQuaternion(rotation) * math::Mat4f::scale(scale);
}

static PageTableEntry makePageTableEntry(const PageAllocation &alloc)
{
  PageTableEntry e{};
  e.bufferOffset = alloc.bufferOffset;
  e.size = alloc.size;
  e.clusterOffset = alloc.clusterOffset;
  e.clusterCount = alloc.clusterCount;
  e.isInstalled = alloc.isInstalled ? 1u : 0u;
  e.prioritySlot = alloc.prioritySlot;
  e.unitScaleBits = alloc.unitScaleBits;
  e.decodedVertexOffset = alloc.decodedVertexOffset;
  e.decodedIndexOffset = alloc.decodedIndexOffset;
  e.decodedBoneWeightOffset = alloc.decodedBoneWeightOffset;
  e._padding1 = alloc.ownerSparseInstance;
  return e;
}

static uint8_t fullEnabledMaskForNode(const VirtualGeometryHierarchy &node)
{
  if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u || node.child_count == 0u)
    return 0u;
  if (node.child_count >= 8u)
    return 0xFFu;
  return static_cast<uint8_t>((1u << node.child_count) - 1u);
}

static void setNodeClusterMasks(VirtualGeometryHierarchy &node, uint8_t streamingMask, uint8_t enabledMask)
{
  node.flags &= ~(HIERARCHY_STREAMING_MASK_BITS | HIERARCHY_ENABLED_MASK_BITS | STREAMING_LEAF_FLAG);
  node.flags |= (static_cast<uint32_t>(streamingMask) << HIERARCHY_STREAMING_MASK_SHIFT);
  node.flags |= (static_cast<uint32_t>(enabledMask) << HIERARCHY_ENABLED_MASK_SHIFT);

  // Keep the legacy aggregate flag for diagnostics/old debug tooling.
  if (streamingMask != 0u)
    node.flags |= STREAMING_LEAF_FLAG;
}

static void rebuildHierarchyFromInstalledPages(const VirtualGeometryFile *file, const std::vector<bool> &installedPages, uint32_t hierarchyNodeOffsetInFile, std::vector<VirtualGeometryHierarchy> &runtimeHierarchy)
{
  // 1) Reset leaf installation bit and clear per-cluster masks.
  for (auto &node : runtimeHierarchy)
  {
    if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
      continue;

    const uint32_t rawPage = node.pageIndex & ~PAGE_NOT_INSTALLED_BIT;
    const bool installed = rawPage < installedPages.size() ? installedPages[rawPage] : false;
    node.pageIndex = installed ? rawPage : (PAGE_NOT_INSTALLED_BIT | rawPage);
    setNodeClusterMasks(node, 0u, 0u);
  }

  // 2) For installed pages, apply latest per-node masks from rewrite lists.
  std::unordered_set<uint32_t> nodesWithUpdates;
  for (uint32_t pageId = 0; pageId < installedPages.size(); ++pageId)
  {
    const auto &updates = file->getPageInstallUpdates()[pageId].hierarchyUpdates;
    for (const auto &u : updates)
    {
      if (u.hierarchyNodeIndex < hierarchyNodeOffsetInFile)
        continue;
      const uint32_t localNodeIndex = u.hierarchyNodeIndex - hierarchyNodeOffsetInFile;
      if (localNodeIndex >= runtimeHierarchy.size())
        continue;
      nodesWithUpdates.insert(localNodeIndex);
    }

    if (!installedPages[pageId])
      continue;

    for (const auto &u : updates)
    {
      if (u.hierarchyNodeIndex < hierarchyNodeOffsetInFile)
        continue;
      const uint32_t localNodeIndex = u.hierarchyNodeIndex - hierarchyNodeOffsetInFile;
      if (localNodeIndex >= runtimeHierarchy.size())
        continue;
      setNodeClusterMasks(runtimeHierarchy[localNodeIndex], u.streamingLeafsBitset, u.enabledClustersBitset);
    }
  }

  // 3) Installed simple leaves that were never mentioned by updates become fully enabled.
  for (uint32_t ni = 0; ni < runtimeHierarchy.size(); ++ni)
  {
    auto &node = runtimeHierarchy[ni];
    if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
      continue;
    if (node.pageIndex & PAGE_NOT_INSTALLED_BIT)
      continue;
    if (nodesWithUpdates.find(ni) != nodesWithUpdates.end())
      continue;

    setNodeClusterMasks(node, 0u, fullEnabledMaskForNode(node));
  }
}

// ============================================================================
// VirtualGeometryStreamingManager
// ============================================================================

VirtualGeometryStreamingManager::VirtualGeometryStreamingManager(
    const VirtualGeometryFile *file,
    rendering::RenderGraph *renderGraph,
    rendering::BufferManager *pagesAllocator,
    rendering::BufferManager *clusterAllocator,
    rendering::BufferManager *decodedVertexAllocator,
    rendering::BufferManager *decodedIndexAllocator,
    rendering::BufferManager *decodedBoneWeightAllocator,
    rendering::Buffer pagesBuffer,
    rendering::Buffer hierarchyBuffer,
    rendering::Buffer pageTableBuffer,
    uint32_t pageTableOffset,
    std::function<void(uint32_t)> queuePageDecodeRequestCallback, bool compressedGeometry)
    : file_(file), renderGraph_(renderGraph), pagesAllocator_(pagesAllocator), pagesBuffer_(pagesBuffer), hierarchyBuffer_(hierarchyBuffer), pageTableBuffer_(pageTableBuffer), pageTableOffset_(pageTableOffset),
      clusterAllocator_(clusterAllocator), decodedVertexAllocator_(decodedVertexAllocator), decodedIndexAllocator_(decodedIndexAllocator), decodedBoneWeightAllocator_(decodedBoneWeightAllocator),
      compressedGeometry_(compressedGeometry), queuePageDecodeRequestCallback_(std::move(queuePageDecodeRequestCallback)), loadRequests_(), completedLoads_()
{
  const size_t pageCount = file_->getPageTable().size();
  installed_.assign(pageCount, false);
  allocations_.resize(pageCount);
  pendingLoads_.assign(pageCount, 0u);
  decodedPages_.resize(pageCount);
  for (size_t i = 0; i < pageCount; ++i)
    allocations_[i].pageId = static_cast<uint32_t>(i);
  loadWorker_ = os::Thread(
      [this]()
      {
        workerLoop();
      });
}

VirtualGeometryStreamingManager::~VirtualGeometryStreamingManager()
{
  stopWorker();
}

uint32_t VirtualGeometryStreamingManager::registerHierarchyUpload(uint64_t hierarchyAllocationOffset, uint32_t hierarchyNodeOffsetInFile, const std::vector<VirtualGeometryHierarchy> &hierarchyTemplate)
{
  HierarchyUpload upload;
  upload.id = nextHierarchyUploadId_++;
  upload.hierarchyAllocationOffset = hierarchyAllocationOffset;
  upload.hierarchyNodeOffsetInFile = hierarchyNodeOffsetInFile;
  upload.runtimeHierarchy = hierarchyTemplate;
  rebuildHierarchyFromInstalledPages(file_, installed_, upload.hierarchyNodeOffsetInFile, upload.runtimeHierarchy);

  if (!upload.runtimeHierarchy.empty())
  {
    renderGraph_->bufferWrite(hierarchyBuffer_, upload.hierarchyAllocationOffset, upload.runtimeHierarchy.size() * sizeof(VirtualGeometryHierarchy), upload.runtimeHierarchy.data());
  }

  hierarchyUploads_.push_back(std::move(upload));
  return hierarchyUploads_.back().id;
}

bool VirtualGeometryStreamingManager::unregisterHierarchyUpload(uint32_t hierarchyUploadId)
{
  for (size_t uploadIndex = 0; uploadIndex < hierarchyUploads_.size(); ++uploadIndex)
  {
    if (hierarchyUploads_[uploadIndex].id != hierarchyUploadId)
      continue;

    const size_t lastIndex = hierarchyUploads_.size() - 1u;
    if (uploadIndex != lastIndex)
      hierarchyUploads_[uploadIndex] = std::move(hierarchyUploads_[lastIndex]);
    hierarchyUploads_.pop_back();
    return true;
  }

  return false;
}

bool VirtualGeometryStreamingManager::isPageInstalled(uint32_t pageId) const
{
  return pageId < installed_.size() && installed_[pageId];
}

uint32_t VirtualGeometryStreamingManager::getPendingLoadCount() const
{
  uint32_t count = 0u;
  for (uint8_t pending : pendingLoads_)
  {
    if (pending != 0u)
      ++count;
  }
  return count;
}

uint32_t VirtualGeometryStreamingManager::getReadyPageCount() const
{
  return static_cast<uint32_t>(readyPageIds_.size());
}

bool VirtualGeometryStreamingManager::isPageLoadPending(uint32_t pageId) const
{
  return pageId < pendingLoads_.size() && pendingLoads_[pageId] != 0u;
}

bool VirtualGeometryStreamingManager::hasDecodedPageReady(uint32_t pageId) const
{
  return pageId < decodedPages_.size() && decodedPages_[pageId].isValid();
}

void VirtualGeometryStreamingManager::writePageTableEntry(uint32_t localPageId, const PageAllocation &alloc)
{
  PageTableEntry entry = makePageTableEntry(alloc);
  const uint32_t globalSlot = pageTableOffset_ + localPageId;
  renderGraph_->bufferWrite(pageTableBuffer_, globalSlot * sizeof(PageTableEntry), sizeof(PageTableEntry), &entry);
}

void VirtualGeometryStreamingManager::applyInstallUpdates(uint32_t pageId)
{
  (void)pageId;
  uploadHierarchies();
}

void VirtualGeometryStreamingManager::applyUninstallUpdates(uint32_t pageId)
{
  (void)pageId;
  uploadHierarchies();
}

void VirtualGeometryStreamingManager::uploadHierarchies()
{
  for (HierarchyUpload &upload : hierarchyUploads_)
  {
    rebuildHierarchyFromInstalledPages(file_, installed_, upload.hierarchyNodeOffsetInFile, upload.runtimeHierarchy);
    if (upload.runtimeHierarchy.empty())
      continue;

    renderGraph_->bufferWrite(hierarchyBuffer_, upload.hierarchyAllocationOffset, upload.runtimeHierarchy.size() * sizeof(VirtualGeometryHierarchy), upload.runtimeHierarchy.data());
  }
}

void VirtualGeometryStreamingManager::workerLoop()
{
  while (!stopWorkerRequested_.load(std::memory_order_acquire))
  {
    PageLoadRequest request;
    if (!loadRequests_.dequeue(request))
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    if (request.pageId >= allocations_.size())
      continue;

    PageLoadResult result;
    result.pageId = request.pageId;

    try
    {
      result.payload = readPagePayload(request.pageId);
      result.success = result.payload.isValid();
    }
    catch (const std::exception &e)
    {
      os::Logger::warningf("[StreamingManager] worker failed to load page %u: %s", request.pageId, e.what());
      result.success = false;
    }

    completedLoads_.enqueue(result);
  }
}

void VirtualGeometryStreamingManager::stopWorker()
{
  stopWorkerRequested_.store(true, std::memory_order_release);
  if (loadWorker_.isRunning())
    loadWorker_.join();
}

VirtualGeometryStreamingManager::DecodedPagePayload VirtualGeometryStreamingManager::readPagePayload(uint32_t pageId) const
{
  DecodedPagePayload payload;
  if (!file_ || pageId >= allocations_.size())
    return payload;

  const uint32_t maxPageSize = file_->getMaxPageSize();
  auto bytes = std::make_shared<std::vector<uint8_t>>(maxPageSize);

  VirtualGeometryStreamedPage streamedPage;
  if (!file_->streamPageRaw(pageId, bytes->data(), maxPageSize, streamedPage))
    throw std::runtime_error("Failed to stream page " + std::to_string(pageId));

  payload.bytes = std::move(bytes);
  payload.dataSize = streamedPage.getDataSizeInBytes();
  payload.meshletCount = streamedPage.getMeshletCount();
  return payload;
}

void VirtualGeometryStreamingManager::requestPageLoad(uint32_t pageId)
{
  if (pageId >= allocations_.size() || isPageInstalled(pageId) || isPageLoadPending(pageId) || hasDecodedPageReady(pageId))
    return;

  pendingLoads_[pageId] = 1u;
  loadRequests_.enqueue(PageLoadRequest{pageId});
  os::Logger::warningf("[StreamingManager] request queued: page=%u pending=%u ready=%u", pageId, getPendingLoadCount(), getReadyPageCount());
}

void VirtualGeometryStreamingManager::pumpCompletedLoads()
{
  PageLoadResult result;
  while (completedLoads_.dequeue(result))
  {
    if (result.pageId >= allocations_.size())
      continue;

    pendingLoads_[result.pageId] = 0u;
    if (!result.success || isPageInstalled(result.pageId))
    {
      os::Logger::warningf("[StreamingManager] load finished: page=%u success=%u installed=%u", result.pageId, result.success ? 1u : 0u, isPageInstalled(result.pageId) ? 1u : 0u);
      continue;
    }

    decodedPages_[result.pageId] = std::move(result.payload);
    readyPageIds_.push_back(result.pageId);
    os::Logger::warningf("[StreamingManager] decoded ready: page=%u ready=%u pending=%u", result.pageId, getReadyPageCount(), getPendingLoadCount());
  }
}

bool VirtualGeometryStreamingManager::canInstallPage(uint32_t pageId, const PageDependencyGraph &pageDeps) const
{
  if (pageId >= decodedPages_.size() || !decodedPages_[pageId].isValid() || isPageInstalled(pageId))
    return false;

  for (uint32_t parentId : pageDeps.parents[pageId])
  {
    if (!isPageInstalled(parentId))
      return false;
  }
  return true;
}

bool VirtualGeometryStreamingManager::installDecodedPage(uint32_t pageId, const DecodedPagePayload &payload)
{
  if (isPageInstalled(pageId) || !payload.isValid())
    return true;

  const uint32_t pageSize = payload.dataSize;
  const uint32_t meshletCount = payload.meshletCount;
  if (meshletCount == 0u || meshletCount > VirtualGeometryScene::MAX_CLUSTERS_PER_PAGE)
  {
    os::Logger::warningf(
        "[StreamingManager] rejected page=%u meshlets=%u exceeds decoded page capacity=%u",
        pageId,
        meshletCount,
        VirtualGeometryScene::MAX_CLUSTERS_PER_PAGE);
    return false;
  }
  const PageDecodedGeometryBudget decodedGeometryBudget = compressedGeometry_ ? PageDecodedGeometryBudget{} : computeDecodedGeometryBudget(payload);

  if (!pagesAllocator_->canAllocate(pageSize))
    return false;
  if (!clusterAllocator_ || !clusterAllocator_->canAllocate(meshletCount))
    return false;
  if ((decodedGeometryBudget.vertexCount != 0u && (!decodedVertexAllocator_ || !decodedVertexAllocator_->canAllocate(decodedGeometryBudget.vertexCount))) ||
      (decodedGeometryBudget.indexCount != 0u && (!decodedIndexAllocator_ || !decodedIndexAllocator_->canAllocate(decodedGeometryBudget.indexCount))) ||
      (decodedGeometryBudget.boneWeightCount != 0u && (!decodedBoneWeightAllocator_ || !decodedBoneWeightAllocator_->canAllocate(decodedGeometryBudget.boneWeightCount))))
    return false;

  rendering::Allocation pageAlloc = pagesAllocator_->allocate(pageSize);
  if (pageAlloc.offset == UINT64_MAX)
    return false;

  rendering::Allocation clusterAlloc = clusterAllocator_->allocate(meshletCount);
  if (clusterAlloc.offset == UINT64_MAX)
  {
    pagesAllocator_->deallocate(pageAlloc);
    return false;
  }

  rendering::Allocation decodedVertexAlloc{0u, 0u};
  if (decodedGeometryBudget.vertexCount != 0u)
  {
    decodedVertexAlloc = decodedVertexAllocator_->allocate(decodedGeometryBudget.vertexCount);
    if (decodedVertexAlloc.offset == UINT64_MAX)
    {
      clusterAllocator_->deallocate(clusterAlloc);
      pagesAllocator_->deallocate(pageAlloc);
      return false;
    }
  }

  rendering::Allocation decodedIndexAlloc{0u, 0u};
  if (decodedGeometryBudget.indexCount != 0u)
  {
    decodedIndexAlloc = decodedIndexAllocator_->allocate(decodedGeometryBudget.indexCount);
    if (decodedIndexAlloc.offset == UINT64_MAX)
    {
      if (decodedVertexAlloc.size != 0u)
        decodedVertexAllocator_->deallocate(decodedVertexAlloc);
      clusterAllocator_->deallocate(clusterAlloc);
      pagesAllocator_->deallocate(pageAlloc);
      return false;
    }
  }

  rendering::Allocation decodedBoneWeightAlloc{0u, 0u};
  if (decodedGeometryBudget.boneWeightCount != 0u)
  {
    decodedBoneWeightAlloc = decodedBoneWeightAllocator_->allocate(decodedGeometryBudget.boneWeightCount);
    if (decodedBoneWeightAlloc.offset == UINT64_MAX)
    {
      if (decodedIndexAlloc.size != 0u)
        decodedIndexAllocator_->deallocate(decodedIndexAlloc);
      if (decodedVertexAlloc.size != 0u)
        decodedVertexAllocator_->deallocate(decodedVertexAlloc);
      clusterAllocator_->deallocate(clusterAlloc);
      pagesAllocator_->deallocate(pageAlloc);
      return false;
    }
  }

  const uint32_t globalClusterBase = static_cast<uint32_t>(clusterAlloc.offset);
  renderGraph_->bufferWrite(pagesBuffer_, pageAlloc.offset, pageSize, payload.bytes->data());

  PageAllocation &alloc = allocations_[pageId];
  alloc.bufferOffset = static_cast<uint32_t>(pageAlloc.offset);
  alloc.size = pageSize;
  alloc.clusterOffset = globalClusterBase;
  alloc.clusterCount = meshletCount;
  alloc.unitScaleBits = file_->getMetadata().unit_scale_bits;
  alloc.decodedVertexOffset = static_cast<uint32_t>(decodedVertexAlloc.offset);
  alloc.decodedVertexCount = decodedGeometryBudget.vertexCount;
  alloc.decodedIndexOffset = static_cast<uint32_t>(decodedIndexAlloc.offset);
  alloc.decodedIndexCount = decodedGeometryBudget.indexCount;
  alloc.decodedBoneWeightOffset = static_cast<uint32_t>(decodedBoneWeightAlloc.offset);
  alloc.decodedBoneWeightCount = decodedGeometryBudget.boneWeightCount;
  alloc.isInstalled = true;

  installed_[pageId] = true;

  writePageTableEntry(pageId, alloc);
  if (queuePageDecodeRequestCallback_)
    queuePageDecodeRequestCallback_(pageTableOffset_ + pageId);

  applyInstallUpdates(pageId);
  os::Logger::warningf("[StreamingManager] installed page=%u size=%u meshlets=%u offset=%u", pageId, pageSize, meshletCount, alloc.bufferOffset);
  return true;
}

void VirtualGeometryStreamingManager::installPage(uint32_t pageId)
{
  if (isPageInstalled(pageId))
    return;

  if (!installDecodedPage(pageId, readPagePayload(pageId)))
    throw std::runtime_error("Failed to allocate page buffer space");
}

void VirtualGeometryStreamingManager::uninstallPage(uint32_t pageId)
{
  if (!isPageInstalled(pageId))
    return;

  installed_[pageId] = false;
  applyUninstallUpdates(pageId);

  PageAllocation &alloc = allocations_[pageId];
  pagesAllocator_->deallocate(rendering::Allocation{alloc.bufferOffset, alloc.size});
  if (alloc.clusterCount != 0u)
    clusterAllocator_->deallocate(rendering::Allocation{alloc.clusterOffset, alloc.clusterCount});
  if (alloc.decodedVertexCount != 0u)
    decodedVertexAllocator_->deallocate(rendering::Allocation{alloc.decodedVertexOffset, alloc.decodedVertexCount});
  if (alloc.decodedIndexCount != 0u)
    decodedIndexAllocator_->deallocate(rendering::Allocation{alloc.decodedIndexOffset, alloc.decodedIndexCount});
  if (alloc.decodedBoneWeightCount != 0u)
    decodedBoneWeightAllocator_->deallocate(rendering::Allocation{alloc.decodedBoneWeightOffset, alloc.decodedBoneWeightCount});

  alloc.bufferOffset = UINT32_MAX;
  alloc.size = 0;
  alloc.clusterOffset = 0;
  alloc.clusterCount = 0;
  alloc.unitScaleBits = 0u;
  alloc.decodedVertexOffset = 0u;
  alloc.decodedVertexCount = 0u;
  alloc.decodedIndexOffset = 0u;
  alloc.decodedIndexCount = 0u;
  alloc.decodedBoneWeightOffset = 0u;
  alloc.decodedBoneWeightCount = 0u;
  alloc.isInstalled = false;

  writePageTableEntry(pageId, alloc);
  os::Logger::warningf("[StreamingManager] uninstalled page=%u", pageId);
}

uint32_t VirtualGeometryStreamingManager::installReadyPages(const PageDependencyGraph &pageDeps, uint32_t maxPageInstalls, bool *blockedOnMemory)
{
  if (blockedOnMemory)
    *blockedOnMemory = false;

  uint32_t installedCount = 0u;
  while (installedCount < maxPageInstalls)
  {
    size_t bestReadyIndex = readyPageIds_.size();
    uint32_t bestPageId = UINT32_MAX;
    uint32_t bestDepth = std::numeric_limits<uint32_t>::max();

    for (size_t i = 0; i < readyPageIds_.size(); ++i)
    {
      const uint32_t pageId = readyPageIds_[i];
      if (!canInstallPage(pageId, pageDeps))
        continue;

      const uint32_t depth = pageId < pageDeps.pageDepth.size() ? pageDeps.pageDepth[pageId] : 0u;
      if (bestReadyIndex == readyPageIds_.size() || depth < bestDepth || (depth == bestDepth && pageId < bestPageId))
      {
        bestReadyIndex = i;
        bestPageId = pageId;
        bestDepth = depth;
      }
    }

    if (bestReadyIndex == readyPageIds_.size())
    {
      os::Logger::warningf("[StreamingManager] no ready page can be installed right now");
      break;
    }

    if (!installDecodedPage(bestPageId, decodedPages_[bestPageId]))
    {
      if (blockedOnMemory)
        *blockedOnMemory = true;
      os::Logger::warningf("[StreamingManager] install failed due to memory for page=%u", bestPageId);
      break;
    }

    decodedPages_[bestPageId] = DecodedPagePayload{};
    readyPageIds_[bestReadyIndex] = readyPageIds_.back();
    readyPageIds_.pop_back();
    ++installedCount;
  }

  return installedCount;
}

void VirtualGeometryStreamingManager::installPageRecursive(uint32_t pageId, const PageDependencyGraph &pageDeps)
{
  for (uint32_t parentId : pageDeps.parents[pageId])
    if (!isPageInstalled(parentId))
      installPageRecursive(parentId, pageDeps);
  if (!isPageInstalled(pageId))
    installPage(pageId);
}

void VirtualGeometryStreamingManager::uninstallPageRecursive(uint32_t pageId, const PageDependencyGraph &pageDeps)
{
  if (!isPageInstalled(pageId))
    return;
  for (uint32_t childId : pageDeps.children[pageId])
    if (isPageInstalled(childId))
      uninstallPageRecursive(childId, pageDeps);
  uninstallPage(pageId);
}

// ============================================================================
// InstanceManager
// ============================================================================

SceneInstanceId VirtualGeometryScene::InstanceManager::allocateInstance(
    const std::string &objectName,
    const math::Vec3f &position,
    const math::Quatf &rotation,
    float scale,
    uint32_t hierarchyOffset,
    uint32_t pageTableOffset,
    uint32_t materialIndex,
    uint32_t meshPartTransformsOffset,
    uint32_t previousMeshPartTransformsOffset,
    const QuantizationConfig &quantConfig)
{
  SceneInstanceId instanceId;
  if (!freeList.empty())
  {
    instanceId = freeList.back();
    freeList.pop_back();
  }
  else
  {
    instanceId = static_cast<SceneInstanceId>(sparseToDense.size());
    sparseToDense.push_back(INVALID_INSTANCE_ID);
  }

  const uint32_t denseIndex = static_cast<uint32_t>(packedInstances.size());

  VirtualGeometryInstanceGPUData gpuData{};
  gpuData.modelMatrix = buildModelMatrix(position, rotation, math::Vec3f(scale, scale, scale));
  gpuData.hierarchyStartOffset = hierarchyOffset;
  gpuData.quantization_factor = static_cast<uint32_t>(quantConfig.quantization_factor);
  std::memcpy(&gpuData.unit_scale_bits, &quantConfig.unit_scale, sizeof(uint32_t));
  gpuData.pageTableOffset = pageTableOffset;
  gpuData.materialIndex = materialIndex;
  gpuData.meshPartTransformsOffset = meshPartTransformsOffset;
  gpuData.previousModelMatrix = gpuData.modelMatrix;
  gpuData.previousMeshPartTransformsOffset = previousMeshPartTransformsOffset;
  gpuData._temporalFlags = 0u;
  gpuData._padding[0] = 0u;
  gpuData._padding[1] = 0u;
  gpuData._padding2[0] = 0u;
  gpuData._padding2[1] = 0u;

  packedInstances.push_back(gpuData);
  denseToSparse.push_back(instanceId);
  instanceToObjectName.push_back(objectName);

  sparseToDense[instanceId] = denseIndex;
  isDirty = true;
  return instanceId;
}

bool VirtualGeometryScene::InstanceManager::deallocateInstance(SceneInstanceId instanceId)
{
  if (instanceId >= sparseToDense.size() || sparseToDense[instanceId] == INVALID_INSTANCE_ID)
    return false;

  const uint32_t denseIndex = sparseToDense[instanceId];
  const uint32_t lastIndex = static_cast<uint32_t>(packedInstances.size() - 1);

  if (denseIndex != lastIndex)
  {
    packedInstances[denseIndex] = packedInstances[lastIndex];
    instanceToObjectName[denseIndex] = instanceToObjectName[lastIndex];

    const SceneInstanceId swapped = denseToSparse[lastIndex];
    denseToSparse[denseIndex] = swapped;
    sparseToDense[swapped] = denseIndex;
  }

  packedInstances.pop_back();
  denseToSparse.pop_back();
  instanceToObjectName.pop_back();

  sparseToDense[instanceId] = INVALID_INSTANCE_ID;
  freeList.push_back(instanceId);
  isDirty = true;
  return true;
}

bool VirtualGeometryScene::InstanceManager::updateInstance(SceneInstanceId instanceId, const math::Vec3f &position, const math::Quatf &rotation, math::Vec3f scale)
{
  if (instanceId >= sparseToDense.size() || sparseToDense[instanceId] == INVALID_INSTANCE_ID)
    return false;
  VirtualGeometryInstanceGPUData &instance = packedInstances[sparseToDense[instanceId]];
  const math::Mat4f updatedModelMatrix = buildModelMatrix(position, rotation, scale);
  if (matricesApproximatelyEqual(instance.modelMatrix, updatedModelMatrix))
  {
    return false;
  }
  instance.previousModelMatrix = instance.modelMatrix;
  instance.modelMatrix = updatedModelMatrix;
  isDirty = true;
  return true;
}

bool VirtualGeometryScene::InstanceManager::updateMaterial(SceneInstanceId instanceId, uint32_t materialIndex)
{
  if (instanceId >= sparseToDense.size() || sparseToDense[instanceId] == INVALID_INSTANCE_ID)
    return false;
  packedInstances[sparseToDense[instanceId]].materialIndex = materialIndex;
  isDirty = true;
  return true;
}

const VirtualGeometryInstanceGPUData *VirtualGeometryScene::InstanceManager::getInstance(SceneInstanceId instanceId) const
{
  if (instanceId >= sparseToDense.size() || sparseToDense[instanceId] == INVALID_INSTANCE_ID)
    return nullptr;
  return &packedInstances[sparseToDense[instanceId]];
}

const std::vector<VirtualGeometryInstanceGPUData> &VirtualGeometryScene::InstanceManager::getPackedData() const
{
  return packedInstances;
}
size_t VirtualGeometryScene::InstanceManager::getInstanceCount() const
{
  return packedInstances.size();
}
void VirtualGeometryScene::InstanceManager::clearDirtyFlag()
{
  isDirty = false;
}
VirtualGeometryScene::InstanceManager::InstanceManager() : isDirty(false)
{
}

// ============================================================================
// VirtualGeometryObjectRuntimeData
// ============================================================================

void VirtualGeometryScene::VirtualGeometryObjectRuntimeData::buildPageDependencyGraph()
{
  const auto &fileDeps = file.getPageDependencies();
  const size_t pageCount = file.getPageTable().size();

  pageDependencies.children.assign(pageCount, {});
  pageDependencies.parents.assign(pageCount, {});
  pageDependencies.pageDepth.resize(pageCount, 0);

  for (size_t pageId = 0; pageId < fileDeps.size() && pageId < pageCount; ++pageId)
  {
    auto &parents = pageDependencies.parents[pageId];
    parents.clear();
    parents.reserve(fileDeps[pageId].size());
    for (uint32_t parentId : fileDeps[pageId])
    {
      if (parentId < pageCount)
        parents.push_back(parentId);
    }
    std::sort(parents.begin(), parents.end());
    parents.erase(std::unique(parents.begin(), parents.end()), parents.end());

    for (uint32_t parentId : parents)
      pageDependencies.children[parentId].push_back(static_cast<uint32_t>(pageId));
  }

  for (auto &children : pageDependencies.children)
  {
    std::sort(children.begin(), children.end());
    children.erase(std::unique(children.begin(), children.end()), children.end());
  }

  std::vector<uint32_t> inDegree(pageCount, 0);
  for (uint32_t pageId = 0; pageId < pageCount; ++pageId)
    inDegree[pageId] = static_cast<uint32_t>(pageDependencies.parents[pageId].size());

  std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> ready;
  for (uint32_t pageId = 0; pageId < pageCount; ++pageId)
  {
    if (inDegree[pageId] == 0)
      ready.push(pageId);
  }

  pageDependencies.rootPageIds.clear();
  std::vector<uint8_t> rootPageSeen(pageCount, 0u);
  for (uint32_t instanceIndex = 0u; instanceIndex < file.getInstanceCount(); ++instanceIndex)
  {
    const uint32_t rootPageId = file.getRootPageForInstance(instanceIndex);
    if (rootPageId >= pageCount || rootPageSeen[rootPageId] != 0u)
      continue;
    rootPageSeen[rootPageId] = 1u;
    pageDependencies.rootPageIds.push_back(rootPageId);
  }

  if (pageDependencies.rootPageIds.empty() && pageCount > 0)
  {
    pageDependencies.rootPageIds.push_back(0u);
  }

  while (!ready.empty())
  {
    const uint32_t pageId = ready.top();
    ready.pop();

    const uint32_t depth = pageDependencies.pageDepth[pageId];
    for (uint32_t childId : pageDependencies.children[pageId])
    {
      pageDependencies.pageDepth[childId] = std::max(pageDependencies.pageDepth[childId], depth + 1);
      if (--inDegree[childId] == 0)
        ready.push(childId);
    }
  }
}

VirtualGeometryScene::VirtualGeometryObjectRuntimeData::VirtualGeometryObjectRuntimeData(std::string &filePath) : file(filePath, /*write=*/false), pageTableOffset(UINT32_MAX)
{
  if (!file.isOpen())
    throw std::runtime_error("VirtualGeometryObjectRuntimeData: cannot open file: " + filePath);

  VirtualGeometryEncodedData encoded;
  if (!file.readAll(encoded))
    throw std::runtime_error("VirtualGeometryObjectRuntimeData: readAll failed for: " + filePath);

  quantizationConfig = encoded.quantizationConfig;
  shapes = encoded.shapes;
  meshParts = encoded.meshParts;
  skeleton = encoded.skeleton;

  buildPageDependencyGraph();
}

// ============================================================================
// VirtualGeometryScene — constructor
// ============================================================================

VirtualGeometryScene::VirtualGeometryScene(
    rendering::RenderGraph *renderGraph,
    uint32_t hierarchyBufferSize,
    uint64_t pagesBufferSize)
    : VirtualGeometryScene(
          renderGraph,
          hierarchyBufferSize,
          pagesBufferSize,
          Settings{})
{
}

VirtualGeometryScene::VirtualGeometryScene(
    rendering::RenderGraph *renderGraph,
    uint32_t hierarchyBufferSize,
    uint64_t pagesBufferSize,
    Settings sceneSettings)
    : renderGraph(renderGraph), settings(sceneSettings), hierarchyBufferAllocator(hierarchyBufferSize), pagesBufferAllocator(pagesBufferSize), clusterAllocator(MAX_DECODED_CLUSTERS),
      decodedVertexAllocator(sceneSettings.compressedGeometry ? 1u : sceneSettings.decodedVertexCapacity != 0u ? sceneSettings.decodedVertexCapacity : computeDecodedVertexCapacity(pagesBufferSize)),
      decodedIndexAllocator(sceneSettings.compressedGeometry ? 1u : sceneSettings.decodedIndexCapacity != 0u ? sceneSettings.decodedIndexCapacity : computeDecodedIndexCapacity(pagesBufferSize)),
      decodedBoneWeightAllocator(sceneSettings.compressedGeometry ? 1u : sceneSettings.decodedBoneWeightCapacity != 0u ? sceneSettings.decodedBoneWeightCapacity : computeDecodedBoneWeightCapacity(pagesBufferSize)),
      meshPartTransformsAllocator(kDefaultMeshPartTransformBufferSize), materialManager(&virtualTextureSystem),
      hierarchyBufferSize(hierarchyBufferSize), pagesBufferSize(pagesBufferSize), meshPartTransformsBufferSize(kDefaultMeshPartTransformBufferSize),
      pageTableSlotAllocator(kMaxScenePages), prioritySlotAllocator(kMaxScenePages), nextPageTableSlot(0), nextPrioritySlot(0)
{
  hierarchyBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryHierarchyBuffer",
        .scratch = false,
        .size = hierarchyBufferSize,
        .usage = rendering::BufferUsage::BufferUsage_Storage | (settings.asyncRuntimeUploads ? rendering::BufferUsage::BufferUsage_CopyDst : rendering::BufferUsage::BufferUsage_Push) | rendering::BufferUsage::BufferUsage_CopySrc,
        .metalExportable = settings.metalExportableBuffers,
      });

  pagesBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryPagesBuffer",
        .scratch = false,
        .size = pagesBufferSize,
        .usage = rendering::BufferUsage::BufferUsage_Storage | (settings.asyncRuntimeUploads ? rendering::BufferUsage::BufferUsage_CopyDst : rendering::BufferUsage::BufferUsage_Push) | rendering::BufferUsage::BufferUsage_CopySrc,
        .metalExportable = settings.metalExportableBuffers,
      });

  decodedClusterCullBufferSize = static_cast<uint64_t>(MAX_DECODED_CLUSTERS) * sizeof(DecodedClusterCullData);
  decodedClusterCullBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryDecodedClusterCullBuffer",
        .scratch = false,
        .size = decodedClusterCullBufferSize,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_CopySrc,
        .metalExportable = settings.metalExportableBuffers,
      });

  clusterVisibilityBufferSize = static_cast<uint64_t>(
      (MAX_DECODED_CLUSTERS + CLUSTERS_PER_VISIBILITY_WORD - 1u) /
      CLUSTERS_PER_VISIBILITY_WORD) * sizeof(uint32_t);
  clusterVisibilityBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryClusterVisibilityBuffer",
        .scratch = false,
        .size = clusterVisibilityBufferSize,
        // Push so the initial zero-fill can be written from the host. Without
        // it the buffer is device-private, the write fails with
        // VK_ERROR_MEMORY_MAP_FAILED, and the first frame reads whatever the
        // allocation happened to contain. CopySrc so a readback pass can
        // snapshot the end-of-frame bits; a host read of the live buffer races
        // the next in-flight frame's advance pass.
        .usage = rendering::BufferUsage::BufferUsage_Storage |
            rendering::BufferUsage::BufferUsage_Push |
            rendering::BufferUsage::BufferUsage_CopySrc,
        .metalExportable = settings.metalExportableBuffers,
      });
  {
    // Nothing was visible before the first frame, so phase one draws nothing
    // and phase two draws the whole cut. The cut converges from there.
    std::vector<uint32_t> zeroed(
        static_cast<size_t>(clusterVisibilityBufferSize / sizeof(uint32_t)), 0u);
    // Diagnostic canary: the final word maps to clusters past every possible
    // allocation, so no shader ever touches it. If it reads back as anything
    // but all-ones, something outside the culling passes overwrote the buffer.
    if (!zeroed.empty())
      zeroed.back() = 0xFFFFFFFFu;
    writeSceneBuffer(
        clusterVisibilityBuffer, 0u, clusterVisibilityBufferSize, zeroed.data());
  }

  decodedClusterGeometryBufferSize = static_cast<uint64_t>(MAX_DECODED_CLUSTERS) * sizeof(DecodedClusterGeometryInfo);
  decodedClusterGeometryBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryDecodedClusterGeometryBuffer",
        .scratch = false,
        .size = decodedClusterGeometryBufferSize,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_CopySrc,
        .metalExportable = settings.metalExportableBuffers,
      });

  decodedVertexBufferSize = decodedVertexAllocator.getTotalSize() * sizeof(DecodedVertexData);
  decodedVertexBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryDecodedVertexBuffer",
        .scratch = false,
        .size = decodedVertexBufferSize,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_CopySrc,
        .metalExportable = settings.metalExportableBuffers,
      });

  decodedBoneWeightBufferSize = decodedBoneWeightAllocator.getTotalSize() * sizeof(DecodedBoneWeightData);
  decodedBoneWeightBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryDecodedBoneWeightBuffer",
        .scratch = false,
        .size = decodedBoneWeightBufferSize,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_CopySrc,
        .metalExportable = settings.metalExportableBuffers,
      });

  decodedIndexBufferSize = decodedIndexAllocator.getTotalSize() * sizeof(uint32_t);
  decodedIndexBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryDecodedIndexBuffer",
        .scratch = false,
        .size = decodedIndexBufferSize,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_CopySrc,
        .metalExportable = settings.metalExportableBuffers,
      });

  instanceBufferSize = sizeof(VirtualGeometryInstanceGPUData) *
      static_cast<uint64_t>(settings.instanceCapacity != 0u ? settings.instanceCapacity : 1024u * 1024u);
  instanceBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryInstanceBuffer",
        .size = instanceBufferSize,
        .scratch = false,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        .metalExportable = settings.metalExportableBuffers,
      });
  initializeFrameOverrideInstanceBuffers();

  // Stable sparse instance id -> current dense index, refreshed every
  // prepareFrame. Page table entries stamp the sparse id of their owning
  // object, and the software-raster resolve maps it through this table, so
  // dense compaction when other objects unregister can never leave a stale
  // owner in resolved pixels.
  instanceSparseToDenseBufferSize = sizeof(uint32_t) * 1024 * 1024;
  instanceSparseToDenseBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryInstanceSparseToDenseBuffer",
        .size = instanceSparseToDenseBufferSize,
        .scratch = false,
        .usage = rendering::BufferUsage::BufferUsage_Storage |
            rendering::BufferUsage::BufferUsage_Push,
        .metalExportable = settings.metalExportableBuffers,
      });

  maxPages = kMaxScenePages;
  pagePriorityBufferSize = maxPages * sizeof(uint32_t);
  pagePriorityBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryPagePriorityBuffer",
        .size = pagePriorityBufferSize,
        .scratch = false,
        .usage = rendering::BufferUsage::BufferUsage_Storage | (settings.asyncRuntimeUploads ? rendering::BufferUsage::BufferUsage_CopyDst : rendering::BufferUsage::BufferUsage_Push) | rendering::BufferUsage::BufferUsage_CopySrc,
        .metalExportable = settings.metalExportableBuffers,
      });

  pageInstallCandidateBufferSize = STREAMING_PAGE_SELECTION_COUNT * sizeof(StreamingPageCandidate);
  pageInstallCandidateBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryPageInstallCandidateBuffer",
        .size = pageInstallCandidateBufferSize,
        .scratch = false,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        .metalExportable = settings.metalExportableBuffers,
      });

  pageEvictCandidateBufferSize = STREAMING_PAGE_SELECTION_COUNT * sizeof(StreamingPageCandidate);
  pageEvictCandidateBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryPageEvictCandidateBuffer",
        .size = pageEvictCandidateBufferSize,
        .scratch = false,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        .metalExportable = settings.metalExportableBuffers,
      });

  pageDecodeUniformBufferSize = sizeof(PageDecodeUniformsGPU);
  pageDecodeUniformBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryPageDecodeUniformBuffer",
        .size = pageDecodeUniformBufferSize,
        .scratch = false,
        .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        .metalExportable = settings.metalExportableBuffers,
      });

  pageDecodeRequestBufferSize = static_cast<uint64_t>(maxPages) * sizeof(uint32_t);
  pageDecodeRequestBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryPageDecodeRequestBuffer",
        .size = pageDecodeRequestBufferSize,
        .scratch = false,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        .metalExportable = settings.metalExportableBuffers,
      });

  pageDecodeDispatchIndirectBufferSize = sizeof(DispatchIndirectArgsGPU);
  pageDecodeDispatchIndirectBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryPageDecodeDispatchIndirectBuffer",
        .size = pageDecodeDispatchIndirectBufferSize,
        .scratch = false,
        .usage = rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        .metalExportable = settings.metalExportableBuffers,
      });
  initializeFrameOverridePageDecodeBuffers();

  meshPartTransformsBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryMeshPartTransformsBuffer",
        .size = meshPartTransformsBufferSize,
        .scratch = false,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        .metalExportable = settings.metalExportableBuffers,
      });
  initializeFrameOverrideMeshPartTransformsBuffers();

  pagesTableBufferSize = maxPages * sizeof(PageTableEntry);
  pageTableBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryPageTableBuffer",
        .size = pagesTableBufferSize,
        .scratch = false,
        .usage = rendering::BufferUsage::BufferUsage_Storage | (settings.asyncRuntimeUploads ? rendering::BufferUsage::BufferUsage_CopyDst : rendering::BufferUsage::BufferUsage_Push) | rendering::BufferUsage::BufferUsage_CopySrc,
        .metalExportable = settings.metalExportableBuffers,
      });
  {
    std::vector<PageTableEntry> empty(maxPages);
    writeSceneBuffer(pageTableBuffer, 0, pagesTableBufferSize, empty.data());
  }

  {
    std::vector<uint32_t> emptyPriorities(maxPages, 0u);
    writeSceneBuffer(pagePriorityBuffer, 0, pagePriorityBufferSize, emptyPriorities.data());
  }

  {
    std::vector<StreamingPageCandidate> emptyCandidates(STREAMING_PAGE_SELECTION_COUNT);
    writeSceneBuffer(pageInstallCandidateBuffer, 0, pageInstallCandidateBufferSize, emptyCandidates.data());
    writeSceneBuffer(pageEvictCandidateBuffer, 0, pageEvictCandidateBufferSize, emptyCandidates.data());
  }

  {
    PageDecodeUniformsGPU uniforms{};
    DispatchIndirectArgsGPU dispatchArgs{};
    dispatchArgs.groupCountX = 0u;
    writeSceneBuffer(pageDecodeUniformBuffer, 0, sizeof(PageDecodeUniformsGPU), &uniforms);
    writeSceneBuffer(pageDecodeDispatchIndirectBuffer, 0, sizeof(DispatchIndirectArgsGPU), &dispatchArgs);
  }
}

VirtualGeometryScene::~VirtualGeometryScene()
{
  for (auto &frame : runtimeUploadFrames_)
    if (frame.staging != rendering::BufferId::Invalid)
      renderGraph->getRHI()->deleteBuffer(frame.staging);

  drainDeferredRuntimeReleases(true);
  for (const RuntimeObjectRecord &record : runtimeObjects_)
  {
    if (record.hierarchyAllocation.offset != UINT64_MAX &&
        record.hierarchyAllocation.size != 0u)
    {
      hierarchyBufferAllocator.deallocate(record.hierarchyAllocation);
    }
  }
  for (auto it = files.begin(); it != files.end(); ++it)
    delete it.value();
  for (auto it = cameras.begin(); it != cameras.end(); ++it)
    delete it.value();

  renderGraph->deleteBuffer(pageTableBuffer);
  renderGraph->deleteBuffer(hierarchyBuffer);
  renderGraph->deleteBuffer(pagesBuffer);
  renderGraph->deleteBuffer(clusterVisibilityBuffer);
  renderGraph->deleteBuffer(instanceSparseToDenseBuffer);
  renderGraph->deleteBuffer(decodedClusterCullBuffer);
  renderGraph->deleteBuffer(decodedClusterGeometryBuffer);
  renderGraph->deleteBuffer(decodedVertexBuffer);
  renderGraph->deleteBuffer(decodedBoneWeightBuffer);
  renderGraph->deleteBuffer(decodedIndexBuffer);
  renderGraph->deleteBuffer(instanceBuffer);
  renderGraph->deleteBuffer(pagePriorityBuffer);
  renderGraph->deleteBuffer(pageInstallCandidateBuffer);
  renderGraph->deleteBuffer(pageEvictCandidateBuffer);
  renderGraph->deleteBuffer(pageDecodeUniformBuffer);
  renderGraph->deleteBuffer(pageDecodeRequestBuffer);
  renderGraph->deleteBuffer(pageDecodeDispatchIndirectBuffer);
  renderGraph->deleteBuffer(meshPartTransformsBuffer);
  destroyFrameOverrideInstanceBuffers();
  destroyFrameOverrideMeshPartTransformsBuffers();
  destroyFrameOverridePageDecodeBuffers();
}

// ============================================================================
// Scene management
// ============================================================================

void VirtualGeometryScene::addCamera(std::string name, rendering::Camera camera)
{
  cameras.insert(name, new rendering::Camera(camera));
}

void VirtualGeometryScene::registerObjectForStreaming(std::string name, std::string filePath)
{
  auto *runtimeData = new VirtualGeometryObjectRuntimeData(filePath);

  uint32_t maxMaterialIndex = 0u;
  bool hasMaterialSlots = false;
  for (const VirtualGeometryShapeInfo &shape : runtimeData->shapes)
  {
    maxMaterialIndex = std::max(maxMaterialIndex, shape.materialIndex);
    hasMaterialSlots = true;
  }

  const fs::path materialBaseDirectory = fs::path(filePath).parent_path();
  const auto &materialFiles = runtimeData->file.getMaterialFiles();
  if (!materialFiles.empty())
  {
    maxMaterialIndex = std::max(maxMaterialIndex, static_cast<uint32_t>(materialFiles.size() - 1u));
    hasMaterialSlots = true;
  }

  if (hasMaterialSlots)
  {
    for (uint32_t materialIndex = 0u; materialIndex <= maxMaterialIndex; ++materialIndex)
    {
      if (!materialManager.hasMaterial(materialIndex))
        materialManager.ensureMaterial(materialIndex);
    }
  }

  for (uint32_t materialIndex = 0u; materialIndex < materialFiles.size(); ++materialIndex)
  {
    if (materialFiles[materialIndex].empty())
      continue;

    fs::path materialPath = materialFiles[materialIndex];
    if (materialPath.is_relative())
      materialPath = materialBaseDirectory / materialPath;
    if (!fs::exists(materialPath))
      continue;

    loadMaterialFile(materialIndex, materialPath.string());
  }

  materialManager.syncTextureIds();

  const size_t pageCount = runtimeData->file.getPageTable().size();

  const rendering::Allocation filePageTableSlots =
      pageTableSlotAllocator.allocate(pageCount);
  if (filePageTableSlots.offset == UINT64_MAX)
    throw std::runtime_error("registerObjectForStreaming: page table exhausted for " + name);
  const rendering::Allocation filePrioritySlots =
      prioritySlotAllocator.allocate(pageCount);
  if (filePrioritySlots.offset == UINT64_MAX)
  {
    pageTableSlotAllocator.deallocate(filePageTableSlots);
    throw std::runtime_error("registerObjectForStreaming: priority buffer exhausted for " + name);
  }

  runtimeData->pageTableOffset = static_cast<uint32_t>(filePageTableSlots.offset);
  const uint32_t filePrioritySlotOffset =
      static_cast<uint32_t>(filePrioritySlots.offset);
  nextPageTableSlot = std::max(
      nextPageTableSlot,
      runtimeData->pageTableOffset + static_cast<uint32_t>(pageCount));
  nextPrioritySlot = std::max(
      nextPrioritySlot,
      filePrioritySlotOffset + static_cast<uint32_t>(pageCount));
  runtimeData->localBounds = computeHierarchyBounds(runtimeData->file.getHierarchy());

  // Initialise streaming manager now that we know the page table offset
  runtimeData->streamingManager =
      std::make_unique<VirtualGeometryStreamingManager>(
          &runtimeData->file,
          renderGraph,
          &pagesBufferAllocator,
          &clusterAllocator,
          &decodedVertexAllocator,
          &decodedIndexAllocator,
          &decodedBoneWeightAllocator,
          pagesBuffer,
          hierarchyBuffer,
          pageTableBuffer,
          runtimeData->pageTableOffset,
          [this](uint32_t globalPageIndex)
          {
            enqueuePageDecodeRequest(globalPageIndex);
          }, settings.compressedGeometry);

  // Assign priority slots and write zeroed PageTableEntries
  // getPageAllocations() is const — we fill priority slots via the public allocations vector
  // by accessing it through the allocations_ member — cast to non-const through the ref trick
  // is not available; instead we expose a helper. For now assign priority slots directly:
  for (size_t i = 0; i < pageCount; ++i)
  {
    // We can't directly write to the streaming manager's allocations (const ref).
    // Slots come from the scene allocator and are written to the page table and
    // priority buffer here.
    const uint32_t prioritySlot =
        filePrioritySlotOffset + static_cast<uint32_t>(i);

    PageTableEntry entry{};
    entry.isInstalled = 0u;
    entry.prioritySlot = prioritySlot;
    const uint32_t globalSlot = runtimeData->pageTableOffset + static_cast<uint32_t>(i);
    writeSceneBuffer(pageTableBuffer, globalSlot * sizeof(PageTableEntry), sizeof(PageTableEntry), &entry);
  }

  // Zero out priority slots
  {
    std::vector<uint32_t> zeros(pageCount, 0u);
    writeSceneBuffer(pagePriorityBuffer, filePrioritySlotOffset * sizeof(uint32_t), pageCount * sizeof(uint32_t), zeros.data());
  }

  // Re-assign priority slots into the new manager's allocations
  {
    const uint32_t basePrioritySlot = filePrioritySlotOffset;
    // Access via const ref and const_cast — the priority slot is logically assigned at registration
    auto &allocsMut = const_cast<std::vector<PageAllocation> &>(runtimeData->streamingManager->getPageAllocations());
    for (size_t i = 0; i < pageCount; ++i)
      allocsMut[i].prioritySlot = basePrioritySlot + static_cast<uint32_t>(i);
  }

  // Install all root pages so coarse geometry is always present.
  for (uint32_t rootPageId : runtimeData->pageDependencies.rootPageIds)
  {
    os::Logger::warningf("[VirtualGeometryScene] Installing root page %u", rootPageId);
    runtimeData->streamingManager->installPage(rootPageId);
  }

  files.insert(name, runtimeData);
}

bool VirtualGeometryScene::loadMaterialFile(uint32_t materialIndex, const std::string &materialPath)
{
  return materialManager.loadMaterialFile(materialIndex, materialPath);
}

InstanceId VirtualGeometryScene::instantiateObjectInstance(const std::string &objectName, const math::Vec3f &position, const math::Quatf &rotation, float scale, uint32_t materialIndexOverride)
{
  auto runtimeData = files.find(objectName);
  if (runtimeData == files.end())
    throw std::runtime_error("Object not found: " + objectName);

  VirtualGeometryObjectRuntimeData *data = runtimeData.value();

  ObjectInstanceRecord record;
  record.objectName = objectName;
  const size_t sceneInstanceCount = data->file.getInstanceCount();
  record.sceneInstanceIds.reserve(sceneInstanceCount);
  record.hierarchySlices.reserve(sceneInstanceCount);

  const uint32_t boneCount = data->skeleton.getBoneCount();
  uint32_t meshPartTransformsOffset = 0u;
  uint32_t previousMeshPartTransformsOffset = 0u;
  if (boneCount > 0u)
  {
    record.meshPartTransformsAllocation = meshPartTransformsAllocator.allocate(static_cast<uint64_t>(boneCount) * sizeof(math::Mat4f) * 2u);
    if (record.meshPartTransformsAllocation.offset == UINT64_MAX)
      throw std::runtime_error("Failed to allocate mesh-part transform buffer space");

    meshPartTransformsOffset = static_cast<uint32_t>(record.meshPartTransformsAllocation.offset / sizeof(math::Mat4f));
    previousMeshPartTransformsOffset = meshPartTransformsOffset + boneCount;
    record.meshPartTransformsOffset = meshPartTransformsOffset;
    record.previousMeshPartTransformsOffset = previousMeshPartTransformsOffset;
    const std::vector<math::Mat4f> defaultPalette = data->skeleton.buildRestPoseSkinningPalette();
    record.currentMeshPartTransforms = defaultPalette;
    record.previousMeshPartTransforms = defaultPalette;
  }

  if (data->shapes.empty())
  {
    const auto instanceHierarchy = data->file.getHierarchyForInstance(0u);
    const uint64_t hierarchySize = static_cast<uint64_t>(instanceHierarchy.hierarchy.size()) * sizeof(VirtualGeometryHierarchy);
    ObjectInstanceRecord::HierarchySlice hierarchySlice;
    hierarchySlice.allocation = hierarchyBufferAllocator.allocate(hierarchySize);
    if (hierarchySlice.allocation.offset == UINT64_MAX)
      throw std::runtime_error("Failed to allocate hierarchy buffer space");
    hierarchySlice.uploadId = data->streamingManager->registerHierarchyUpload(hierarchySlice.allocation.offset, instanceHierarchy.hierarchyNodeOffsetInFile, instanceHierarchy.hierarchy);
    record.hierarchySlices.push_back(hierarchySlice);

    const uint32_t hierarchyBaseOffset = static_cast<uint32_t>(hierarchySlice.allocation.offset / sizeof(VirtualGeometryHierarchy));
    record.sceneInstanceIds.push_back(instanceManager.allocateInstance(
        objectName,
        position,
        rotation,
        scale,
        hierarchyBaseOffset,
        data->pageTableOffset,
        materialIndexOverride == UINT32_MAX ? 0u : materialIndexOverride,
        meshPartTransformsOffset,
        previousMeshPartTransformsOffset,
        data->quantizationConfig));
  }
  else
  {
    for (uint32_t instanceIndex = 0u; instanceIndex < data->shapes.size(); ++instanceIndex)
    {
      const auto instanceHierarchy = data->file.getHierarchyForInstance(instanceIndex);
      const uint64_t hierarchySize = static_cast<uint64_t>(instanceHierarchy.hierarchy.size()) * sizeof(VirtualGeometryHierarchy);
      ObjectInstanceRecord::HierarchySlice hierarchySlice;
      hierarchySlice.allocation = hierarchyBufferAllocator.allocate(hierarchySize);
      if (hierarchySlice.allocation.offset == UINT64_MAX)
      {
        throw std::runtime_error("Failed to allocate hierarchy buffer space");
      }
      hierarchySlice.uploadId = data->streamingManager->registerHierarchyUpload(hierarchySlice.allocation.offset, instanceHierarchy.hierarchyNodeOffsetInFile, instanceHierarchy.hierarchy);
      record.hierarchySlices.push_back(hierarchySlice);

      const uint32_t hierarchyBaseOffset = static_cast<uint32_t>(hierarchySlice.allocation.offset / sizeof(VirtualGeometryHierarchy));
      const uint32_t hierarchyOffset = hierarchyBaseOffset;
      const VirtualGeometryShapeInfo &shape = data->shapes[instanceIndex];
      record.sceneInstanceIds.push_back(instanceManager.allocateInstance(
          objectName,
          position,
          rotation,
          scale,
          hierarchyOffset,
          data->pageTableOffset,
          materialIndexOverride == UINT32_MAX ? shape.materialIndex : materialIndexOverride,
          meshPartTransformsOffset,
          previousMeshPartTransformsOffset,
          data->quantizationConfig));
    }
  }

  InstanceId instanceId;
  if (!objectFreeList.empty())
  {
    instanceId = objectFreeList.back();
    objectFreeList.pop_back();
  }
  else
  {
    instanceId = static_cast<InstanceId>(objectSparseToDense.size());
    objectSparseToDense.push_back(INVALID_INSTANCE_ID);
  }

  const uint32_t denseIndex = static_cast<uint32_t>(objectInstances.size());
  objectInstances.push_back(std::move(record));
  objectDenseToSparse.push_back(instanceId);
  objectSparseToDense[instanceId] = denseIndex;
  if (boneCount > 0u)
  {
    writeMeshPartTransformsRecordToCurrentBuffers(denseIndex);
    queueMeshPartTransformsRecordUpdateForAllFrameSlots(denseIndex);
  }
  return instanceId;
}

bool VirtualGeometryScene::destroyObjectInstance(InstanceId instanceId)
{
  if (instanceId >= objectSparseToDense.size() || objectSparseToDense[instanceId] == INVALID_INSTANCE_ID)
    return false;

  const uint32_t denseIndex = objectSparseToDense[instanceId];
  ObjectInstanceRecord &record = objectInstances[denseIndex];
  for (SceneInstanceId sceneInstanceId : record.sceneInstanceIds)
    instanceManager.deallocateInstance(sceneInstanceId);

  auto runtimeData = files.find(record.objectName);
  if (runtimeData != files.end())
  {
    VirtualGeometryObjectRuntimeData *data = runtimeData.value();
    for (const ObjectInstanceRecord::HierarchySlice &hierarchySlice : record.hierarchySlices)
      data->streamingManager->unregisterHierarchyUpload(hierarchySlice.uploadId);
  }
  for (const ObjectInstanceRecord::HierarchySlice &hierarchySlice : record.hierarchySlices)
  {
    if (hierarchySlice.allocation.offset != UINT64_MAX && hierarchySlice.allocation.size != 0u)
      hierarchyBufferAllocator.deallocate(hierarchySlice.allocation);
  }

  if (record.meshPartTransformsAllocation.offset != UINT64_MAX && record.meshPartTransformsAllocation.size != 0u)
    meshPartTransformsAllocator.deallocate(record.meshPartTransformsAllocation);

  const uint32_t lastIndex = static_cast<uint32_t>(objectInstances.size() - 1u);
  if (denseIndex != lastIndex)
  {
    objectInstances[denseIndex] = std::move(objectInstances[lastIndex]);
    const InstanceId swappedInstanceId = objectDenseToSparse[lastIndex];
    objectDenseToSparse[denseIndex] = swappedInstanceId;
    objectSparseToDense[swappedInstanceId] = denseIndex;
  }

  objectInstances.pop_back();
  objectDenseToSparse.pop_back();
  objectSparseToDense[instanceId] = INVALID_INSTANCE_ID;
  objectFreeList.push_back(instanceId);
  return true;
}

bool VirtualGeometryScene::updateObjectInstanceTransform(InstanceId instanceId, const math::Vec3f &position, const math::Quatf &rotation, float scale)
{
  if (instanceId >= objectSparseToDense.size() || objectSparseToDense[instanceId] == INVALID_INSTANCE_ID)
    return false;

  ObjectInstanceRecord &record = objectInstances[objectSparseToDense[instanceId]];
  AABB invalidationBounds;
  bool shouldInvalidate = false;

  if (streamingResidencyInvalidationCallback_)
  {
    auto runtimeData = files.find(record.objectName);
    if (runtimeData != files.end())
    {
      const VirtualGeometryObjectRuntimeData *data = runtimeData.value();
      if (isValidAABB(data->localBounds))
      {
        invalidationBounds = computeWorldBoundsForSceneInstances(instanceManager, record.sceneInstanceIds, data->localBounds);
        shouldInvalidate = isValidAABB(invalidationBounds);
      }
    }
  }

  bool updated = false;
  for (uint32_t shapeIndex = 0u; shapeIndex < record.sceneInstanceIds.size(); ++shapeIndex)
  {
    const SceneInstanceId sceneInstanceId = record.sceneInstanceIds[shapeIndex];
    const VirtualGeometryInstanceGPUData *previousInstance = instanceManager.getInstance(sceneInstanceId);
    const math::Mat4f previousTransform = previousInstance != nullptr ? previousInstance->modelMatrix : math::Mat4f::identity();
    const bool instanceUpdated = instanceManager.updateInstance(sceneInstanceId, position, rotation, math::Vec3f(scale, scale, scale));
    updated = instanceUpdated || updated;
    if (sceneInstanceId < instanceManager.sparseToDense.size())
    {
      const uint32_t denseIndex = instanceManager.sparseToDense[sceneInstanceId];
      if (denseIndex != INVALID_SCENE_INSTANCE_ID)
      {
        const VirtualGeometryInstanceGPUData &instanceData = instanceManager.packedInstances[denseIndex];
        writeInstanceRecordToCurrentBuffers(denseIndex, instanceData);
        queueInstanceRecordUpdateForAllFrameSlots(denseIndex);
        if (instanceUpdated)
        {
          SceneInstanceTransformUpdate transformUpdate{};
          transformUpdate.sequence = nextTransformUpdateSequence_++;
          transformUpdate.objectInstanceId = instanceId;
          transformUpdate.sceneInstanceId = sceneInstanceId;
          transformUpdate.shapeIndex = shapeIndex;
          transformUpdate.previousTransform = previousTransform;
          transformUpdate.transform = instanceData.modelMatrix;
          queueTransformUpdateForAllFrameSlots(transformUpdate);
        }
      }
    }
  }

  if (updated && streamingResidencyInvalidationCallback_)
  {
    auto runtimeData = files.find(record.objectName);
    if (runtimeData != files.end())
    {
      const VirtualGeometryObjectRuntimeData *data = runtimeData.value();
      if (isValidAABB(data->localBounds))
      {
        const AABB updatedBounds = computeWorldBoundsForSceneInstances(instanceManager, record.sceneInstanceIds, data->localBounds);
        invalidationBounds = shouldInvalidate ? AABB::Union(invalidationBounds, updatedBounds) : updatedBounds;
        shouldInvalidate = isValidAABB(invalidationBounds);
      }
    }
  }

  if (updated && shouldInvalidate)
  {
    notifyInvalidationIfNeeded(streamingResidencyInvalidationCallback_, invalidationBounds);
  }

  if (updated)
  {
    ++record.gpuSignedDistanceFields.contentRevision;
  }

  return updated;
}

bool VirtualGeometryScene::updateObjectInstanceMaterial(InstanceId instanceId, uint32_t materialIndex)
{
  if (instanceId >= objectSparseToDense.size() || objectSparseToDense[instanceId] == INVALID_INSTANCE_ID)
    return false;

  const ObjectInstanceRecord &record = objectInstances[objectSparseToDense[instanceId]];
  bool updated = false;
  for (SceneInstanceId sceneInstanceId : record.sceneInstanceIds)
  {
    updated = instanceManager.updateMaterial(sceneInstanceId, materialIndex) || updated;
    if (sceneInstanceId < instanceManager.sparseToDense.size())
    {
      const uint32_t denseIndex = instanceManager.sparseToDense[sceneInstanceId];
      if (denseIndex != INVALID_SCENE_INSTANCE_ID)
      {
        const VirtualGeometryInstanceGPUData &instanceData = instanceManager.packedInstances[denseIndex];
        writeInstanceRecordToCurrentBuffers(denseIndex, instanceData);
        queueInstanceRecordUpdateForAllFrameSlots(denseIndex);
      }
    }
  }
  return updated;
}

bool VirtualGeometryScene::uploadBoneTransforms(InstanceId instanceId, const std::vector<math::Mat4f> &boneTransforms)
{
  if (instanceId >= objectSparseToDense.size() || objectSparseToDense[instanceId] == INVALID_INSTANCE_ID)
    return false;

  const uint32_t denseIndex = objectSparseToDense[instanceId];
  ObjectInstanceRecord &record = objectInstances[denseIndex];
  if (record.meshPartTransformsAllocation.offset == UINT64_MAX || record.meshPartTransformsAllocation.size == 0u)
    return boneTransforms.empty();

  auto runtimeData = files.find(record.objectName);
  if (runtimeData == files.end())
    return false;

  const VirtualGeometryObjectRuntimeData *data = runtimeData.value();
  if (boneTransforms.size() != data->skeleton.getBoneCount())
    return false;

  if (palettesApproximatelyEqual(record.currentMeshPartTransforms, boneTransforms))
  {
    return true;
  }

  if (record.currentMeshPartTransforms.size() != boneTransforms.size())
  {
    record.currentMeshPartTransforms.assign(boneTransforms.begin(), boneTransforms.end());
    record.previousMeshPartTransforms.assign(boneTransforms.begin(), boneTransforms.end());
  }
  else
  {
    record.previousMeshPartTransforms = record.currentMeshPartTransforms;
    record.currentMeshPartTransforms.assign(boneTransforms.begin(), boneTransforms.end());
  }

  writeMeshPartTransformsRecordToCurrentBuffers(denseIndex);
  queueMeshPartTransformsRecordUpdateForAllFrameSlots(denseIndex);
  ++record.gpuSignedDistanceFields.contentRevision;
  return true;
}

bool VirtualGeometryScene::uploadAnimationPlayerPose(InstanceId instanceId, const rendering::animation::AnimationPlayer &animationPlayer)
{
  return uploadBoneTransforms(instanceId, animationPlayer.getBoneTransforms());
}

bool VirtualGeometryScene::applyAnimationFrame(InstanceId instanceId, rendering::animation::AnimationPlayer &animationPlayer, const std::string &animationName, float timeSeconds, bool looping)
{
  if (!animationPlayer.applyAnimationFrame(animationName, timeSeconds, looping))
    return false;
  return uploadAnimationPlayerPose(instanceId, animationPlayer);
}

bool VirtualGeometryScene::setObjectInstanceSdfDynamic(InstanceId instanceId, bool dynamic)
{
  if (instanceId >= objectSparseToDense.size() || objectSparseToDense[instanceId] == INVALID_INSTANCE_ID)
  {
    return false;
  }

  ObjectInstanceRecord &record = objectInstances[objectSparseToDense[instanceId]];
  if (record.gpuSignedDistanceFields.dynamic == dynamic)
  {
    return true;
  }

  record.gpuSignedDistanceFields.dynamic = dynamic;
  return true;
}

bool VirtualGeometryScene::isObjectInstanceSdfDynamic(InstanceId instanceId) const
{
  if (instanceId >= objectSparseToDense.size() || objectSparseToDense[instanceId] == INVALID_INSTANCE_ID)
  {
    return false;
  }

  return objectInstances[objectSparseToDense[instanceId]].gpuSignedDistanceFields.dynamic;
}

bool VirtualGeometryScene::requestObjectInstanceSdfRebuild(InstanceId instanceId)
{
  if (instanceId >= objectSparseToDense.size() || objectSparseToDense[instanceId] == INVALID_INSTANCE_ID)
  {
    return false;
  }

  ++objectInstances[objectSparseToDense[instanceId]].gpuSignedDistanceFields.rebuildRevision;
  return true;
}

void VirtualGeometryScene::updateInstanceBuffer(bool uploadAllFrameSlots)
{
  if (!instanceManager.isDirty)
    return;

  const auto &packedData = instanceManager.getPackedData();
  if (packedData.empty())
  {
    instanceManager.clearDirtyFlag();
    return;
  }

  // The "current" frame slot is the one the caller last recorded, which may
  // be executing on the GPU right now; under a release delay the slots are
  // brought up to date one at a time in prepareFrame instead.
  if (!uploadAllFrameSlots && deferringRuntimeReleases() &&
      !frameOverrideInstanceBufferIds_.empty())
  {
    publishInstanceLayoutChange();
    return;
  }

  writePackedInstanceDataToBuffers(packedData, uploadAllFrameSlots);
  instanceManager.clearDirtyFlag();
}

void VirtualGeometryScene::publishInstanceLayoutChange()
{
  if (!deferringRuntimeReleases() || frameOverrideInstanceBufferIds_.empty())
  {
    updateInstanceBuffer(true);
    return;
  }
  const auto &packedData = instanceManager.getPackedData();
  // The base buffer is never bound while frame overrides are in use, so it
  // can take the new layout immediately; the per-slot copies follow in
  // prepareFrame.
  if (!packedData.empty())
  {
    ensureInstanceBufferFits(packedData.size() * sizeof(VirtualGeometryInstanceGPUData), instanceBufferSize);
    writeSceneBuffer(
        instanceBuffer, 0,
        packedData.size() * sizeof(VirtualGeometryInstanceGPUData),
        const_cast<VirtualGeometryInstanceGPUData *>(packedData.data()));
  }
  instanceLayoutDirtyPerSlot_.assign(frameOverrideInstanceBufferIds_.size(), 1u);
  instanceManager.clearDirtyFlag();
}

void VirtualGeometryScene::appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
{
  const rendering::BufferId overrideId = getFrameOverrideInstanceBufferId(frameSlot);
  if (overrideId != rendering::BufferId::Invalid)
  {
    overrides.bufferOverrides.emplace(instanceBuffer.name, rendering::RenderGraphBufferOverride{.bufferId = overrideId});
  }

  const rendering::BufferId meshOverrideId = getFrameOverrideMeshPartTransformsBufferId(frameSlot);
  if (meshOverrideId != rendering::BufferId::Invalid)
  {
    overrides.bufferOverrides.emplace(meshPartTransformsBuffer.name, rendering::RenderGraphBufferOverride{.bufferId = meshOverrideId});
  }

  const rendering::BufferId pageDecodeUniformOverrideId = getFrameOverridePageDecodeUniformBufferId(frameSlot);
  if (pageDecodeUniformOverrideId != rendering::BufferId::Invalid)
  {
    overrides.bufferOverrides.emplace(pageDecodeUniformBuffer.name, rendering::RenderGraphBufferOverride{.bufferId = pageDecodeUniformOverrideId});
  }

  const rendering::BufferId pageDecodeRequestOverrideId = getFrameOverridePageDecodeRequestBufferId(frameSlot);
  if (pageDecodeRequestOverrideId != rendering::BufferId::Invalid)
  {
    overrides.bufferOverrides.emplace(pageDecodeRequestBuffer.name, rendering::RenderGraphBufferOverride{.bufferId = pageDecodeRequestOverrideId});
  }

  const rendering::BufferId pageDecodeDispatchOverrideId = getFrameOverridePageDecodeDispatchIndirectBufferId(frameSlot);
  if (pageDecodeDispatchOverrideId != rendering::BufferId::Invalid)
  {
    overrides.bufferOverrides.emplace(pageDecodeDispatchIndirectBuffer.name, rendering::RenderGraphBufferOverride{.bufferId = pageDecodeDispatchOverrideId});
  }
}

void VirtualGeometryScene::writeSceneBuffer(const rendering::Buffer &buffer,
    uint64_t offset, uint64_t size, void *data)
{
  const bool livePageBuffer = buffer.name == hierarchyBuffer.name ||
      buffer.name == pagesBuffer.name || buffer.name == pageTableBuffer.name ||
      buffer.name == pagePriorityBuffer.name;
  if (!settings.asyncRuntimeUploads || !livePageBuffer)
  {
    renderGraph->bufferWrite(buffer, offset, size, data);
    return;
  }
  if (size == 0u) return;
  auto &target = pendingRuntimeUploads_[buffer.name];
  target.buffer = buffer;
  auto &ranges = target.ranges;
  auto first = ranges.lower_bound(offset);
  if (first != ranges.begin())
  {
    auto previous = std::prev(first);
    if (previous->first + previous->second.size() > offset) first = previous;
  }
  if (first != ranges.end() && first->first <= offset &&
      first->first + first->second.size() >= offset + size)
  {
    std::memcpy(first->second.data() + offset - first->first, data, size);
    return;
  }
  uint64_t begin = offset, end = offset + size;
  auto last = first;
  while (last != ranges.end() && last->first < end)
  {
    begin = std::min(begin, last->first);
    end = std::max(end, last->first + last->second.size());
    ++last;
  }
  std::vector<uint8_t> combined(end - begin);
  for (auto it = first; it != last; ++it)
    std::memcpy(combined.data() + it->first - begin, it->second.data(), it->second.size());
  std::memcpy(combined.data() + offset - begin, data, size);
  ranges.erase(first, last);
  ranges.emplace(begin, std::move(combined));
}

void VirtualGeometryScene::prepareRuntimeUploads(uint32_t frameSlot)
{
  if (!settings.asyncRuntimeUploads) return;
  if (runtimeUploadFrames_.size() <= frameSlot) runtimeUploadFrames_.resize(frameSlot + 1u);
  auto &frame = runtimeUploadFrames_[frameSlot];
  frame.ranges.clear();
  uint64_t bytes = 0u;
  for (const auto &[name, target] : pendingRuntimeUploads_)
    for (const auto &[offset, data] : target.ranges) bytes += data.size();
  // Bootstrap uploads must not pin their peak staging allocation forever.
  // This slot's fence has completed, so replacing its staging buffer is safe.
  const uint64_t targetCapacity = std::max<uint64_t>(bytes, 8u << 20u);
  frame.smallUploadFrames = frame.capacity > targetCapacity * 4u
      ? frame.smallUploadFrames + 1u : 0u;
  if (frame.smallUploadFrames >= 8u)
  {
    renderGraph->getRHI()->deleteBuffer(frame.staging);
    frame.staging = rendering::BufferId::Invalid;
    frame.capacity = 0u;
    frame.smallUploadFrames = 0u;
  }
  if (bytes == 0u) return;
  if (bytes > frame.capacity)
  {
    if (frame.staging != rendering::BufferId::Invalid)
      renderGraph->getRHI()->deleteBuffer(frame.staging);
    frame.capacity = (bytes + (1u << 20u) - 1u) & ~uint64_t{(1u << 20u) - 1u};
    frame.staging = renderGraph->getRHI()->createBuffer(rendering::BufferInfo{
      .name = "VG.RuntimeUpload.frame" + std::to_string(frameSlot),
      .size = frame.capacity,
      .usage = rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
    });
  }
  std::vector<uint8_t> staging;
  staging.reserve(bytes);
  for (const auto &[name, target] : pendingRuntimeUploads_)
    for (const auto &[offset, data] : target.ranges)
    {
      frame.ranges.push_back({target.buffer, staging.size(), offset, data.size()});
      staging.insert(staging.end(), data.begin(), data.end());
    }
  renderGraph->getRHI()->bufferWrite(frame.staging, 0u, staging.size(), staging.data());
  pendingRuntimeUploads_.clear();
}

void VirtualGeometryScene::recordRuntimeUploads(rendering::CommandRunContext &context)
{
  const auto slot = renderGraph->getCurrentFrameIndex();
  if (slot >= runtimeUploadFrames_.size()) return;
  const auto &frame = runtimeUploadFrames_[slot];
  for (const auto &range : frame.ranges)
  {
    rendering::BufferView source{}, destination{};
    source.resourceId = frame.staging;
    source.offset = range.sourceOffset; source.size = range.bytes;
    destination.resourceId = renderGraph->resolveBufferId(range.destination);
    destination.offset = range.destinationOffset; destination.size = range.bytes;
    context.rhi->cmdCopyBuffer(context.commandBuffer, source, destination);
  }
}

void VirtualGeometryScene::prepareFrame(uint32_t frameSlot)
{
  drainDeferredRuntimeReleases(false);

  // A changed instance layout (an object registered or unregistered) reaches
  // each frame slot here, when that slot is about to be recorded, rather than
  // being written into every slot at once - one of those slots belongs to the
  // frame still executing on the GPU.
  if (frameSlot < instanceLayoutDirtyPerSlot_.size() &&
      instanceLayoutDirtyPerSlot_[frameSlot] != 0u)
  {
    const auto &packedData = instanceManager.getPackedData();
    const rendering::BufferId layoutOverrideId = getFrameOverrideInstanceBufferId(frameSlot);
    if (layoutOverrideId != rendering::BufferId::Invalid && !packedData.empty())
    {
      renderGraph->getRHI()->bufferWrite(
          layoutOverrideId,
          0,
          packedData.size() * sizeof(VirtualGeometryInstanceGPUData),
          const_cast<VirtualGeometryInstanceGPUData *>(packedData.data()));
    }
    clearPendingInstanceRecordUpdates(frameSlot);
    instanceLayoutDirtyPerSlot_[frameSlot] = 0u;
  }

  // Refresh the sparse->dense instance mapping the software-raster resolve
  // reads. It is a few kilobytes and dense compaction can move indices on
  // any unregister, so it is simplest to publish it every frame.
  if (instanceSparseToDenseBuffer.isValid() &&
      !instanceManager.sparseToDense.empty())
  {
    const uint64_t mappingBytes = std::min<uint64_t>(
        instanceManager.sparseToDense.size() * sizeof(uint32_t),
        instanceSparseToDenseBufferSize);
    writeSceneBuffer(
        instanceSparseToDenseBuffer, 0u, mappingBytes,
        instanceManager.sparseToDense.data());
  }
  const rendering::BufferId overrideId = getFrameOverrideInstanceBufferId(frameSlot);
  if (overrideId != rendering::BufferId::Invalid)
  {
    const uint32_t resolvedFrameSlot = frameSlot % frameOverrideInstanceBufferIds_.size();
    auto &pendingDenseIndices = pendingInstanceUpdateDenseIndicesPerFrame_[resolvedFrameSlot];
    auto &pendingMask = pendingInstanceUpdateMasksPerFrame_[resolvedFrameSlot];
    for (const uint32_t denseIndex : pendingDenseIndices)
    {
      if (denseIndex < instanceManager.packedInstances.size())
      {
        const VirtualGeometryInstanceGPUData &instanceData = instanceManager.packedInstances[denseIndex];
        const uint64_t offset = static_cast<uint64_t>(denseIndex) * sizeof(VirtualGeometryInstanceGPUData);
        renderGraph->getRHI()->bufferWrite(
            overrideId,
            offset,
            sizeof(VirtualGeometryInstanceGPUData),
            const_cast<VirtualGeometryInstanceGPUData *>(&instanceData));
      }

      if (denseIndex < pendingMask.size())
      {
        pendingMask[denseIndex] = 0u;
      }
    }
    pendingDenseIndices.clear();
  }

  const rendering::BufferId meshOverrideId = getFrameOverrideMeshPartTransformsBufferId(frameSlot);
  if (meshOverrideId != rendering::BufferId::Invalid)
  {
    const uint32_t resolvedFrameSlot = frameSlot % frameOverrideMeshPartTransformsBufferIds_.size();
    auto &pendingDenseIndices = pendingMeshPartTransformsUpdateDenseIndicesPerFrame_[resolvedFrameSlot];
    auto &pendingMask = pendingMeshPartTransformsUpdateMasksPerFrame_[resolvedFrameSlot];
    for (const uint32_t denseIndex : pendingDenseIndices)
    {
      if (denseIndex < objectInstances.size())
      {
        const ObjectInstanceRecord &record = objectInstances[denseIndex];
        if (!record.currentMeshPartTransforms.empty())
        {
          renderGraph->getRHI()->bufferWrite(
              meshOverrideId,
              static_cast<uint64_t>(record.meshPartTransformsOffset) * sizeof(math::Mat4f),
              static_cast<uint64_t>(record.currentMeshPartTransforms.size()) * sizeof(math::Mat4f),
              const_cast<math::Mat4f *>(record.currentMeshPartTransforms.data()));
          renderGraph->getRHI()->bufferWrite(
              meshOverrideId,
              static_cast<uint64_t>(record.previousMeshPartTransformsOffset) * sizeof(math::Mat4f),
              static_cast<uint64_t>(record.previousMeshPartTransforms.size()) * sizeof(math::Mat4f),
              const_cast<math::Mat4f *>(record.previousMeshPartTransforms.data()));
        }
      }

      if (denseIndex < pendingMask.size())
      {
        pendingMask[denseIndex] = 0u;
      }
    }
    pendingDenseIndices.clear();
  }

  prepareRuntimeUploads(frameSlot);
  updatePageDecodeBuffers(
      getFrameOverridePageDecodeUniformBufferId(frameSlot),
      getFrameOverridePageDecodeRequestBufferId(frameSlot),
      getFrameOverridePageDecodeDispatchIndirectBufferId(frameSlot),
      true);
}

void VirtualGeometryScene::initializeFrameOverrideInstanceBuffers()
{
  if (renderGraph == nullptr)
    return;

  const uint32_t frameCount = std::max(1u, renderGraph->getMaxFramesInFlight());
  pendingTransformUpdatesPerFrame_.assign(frameCount, {});
  if (frameCount <= 1u)
    return;

  frameOverrideInstanceBufferIds_.assign(frameCount, rendering::BufferId::Invalid);
  pendingInstanceUpdateDenseIndicesPerFrame_.assign(frameCount, {});
  pendingInstanceUpdateMasksPerFrame_.assign(frameCount, {});
  for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
  {
    frameOverrideInstanceBufferIds_[frameSlot] = renderGraph->getRHI()->createBuffer(
        rendering::BufferInfo{
            .name = "VirtualGeometryInstanceBuffer.override_frame" + std::to_string(frameSlot),
            .size = instanceBufferSize,
            .scratch = false,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
            .metalExportable = settings.metalExportableBuffers,
        });
  }
}

void VirtualGeometryScene::initializeFrameOverrideMeshPartTransformsBuffers()
{
  if (renderGraph == nullptr)
    return;

  const uint32_t frameCount = std::max(1u, renderGraph->getMaxFramesInFlight());
  if (frameCount <= 1u)
    return;

  frameOverrideMeshPartTransformsBufferIds_.assign(frameCount, rendering::BufferId::Invalid);
  pendingMeshPartTransformsUpdateDenseIndicesPerFrame_.assign(frameCount, {});
  pendingMeshPartTransformsUpdateMasksPerFrame_.assign(frameCount, {});
  for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
  {
    frameOverrideMeshPartTransformsBufferIds_[frameSlot] = renderGraph->getRHI()->createBuffer(
        rendering::BufferInfo{
            .name = "VirtualGeometryMeshPartTransformsBuffer.override_frame" + std::to_string(frameSlot),
            .size = meshPartTransformsBufferSize,
            .scratch = false,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
            .metalExportable = settings.metalExportableBuffers,
        });
  }
}

void VirtualGeometryScene::initializeFrameOverridePageDecodeBuffers()
{
  if (renderGraph == nullptr)
    return;

  const uint32_t frameCount = std::max(1u, renderGraph->getMaxFramesInFlight());
  if (frameCount <= 1u)
    return;

  frameOverridePageDecodeUniformBufferIds_.assign(frameCount, rendering::BufferId::Invalid);
  frameOverridePageDecodeRequestBufferIds_.assign(frameCount, rendering::BufferId::Invalid);
  frameOverridePageDecodeDispatchIndirectBufferIds_.assign(frameCount, rendering::BufferId::Invalid);
  for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
  {
    frameOverridePageDecodeUniformBufferIds_[frameSlot] = renderGraph->getRHI()->createBuffer(
        rendering::BufferInfo{
            .name = "VirtualGeometryPageDecodeUniformBuffer.override_frame" + std::to_string(frameSlot),
            .size = pageDecodeUniformBufferSize,
            .scratch = false,
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
            .metalExportable = settings.metalExportableBuffers,
        });
    frameOverridePageDecodeRequestBufferIds_[frameSlot] = renderGraph->getRHI()->createBuffer(
        rendering::BufferInfo{
            .name = "VirtualGeometryPageDecodeRequestBuffer.override_frame" + std::to_string(frameSlot),
            .size = pageDecodeRequestBufferSize,
            .scratch = false,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
            .metalExportable = settings.metalExportableBuffers,
        });
    frameOverridePageDecodeDispatchIndirectBufferIds_[frameSlot] = renderGraph->getRHI()->createBuffer(
        rendering::BufferInfo{
            .name = "VirtualGeometryPageDecodeDispatchIndirectBuffer.override_frame" + std::to_string(frameSlot),
            .size = pageDecodeDispatchIndirectBufferSize,
            .scratch = false,
            .usage = rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
            .metalExportable = settings.metalExportableBuffers,
        });
  }
}

void VirtualGeometryScene::destroyFrameOverrideInstanceBuffers()
{
  if (renderGraph == nullptr)
    return;

  for (const rendering::BufferId bufferId : frameOverrideInstanceBufferIds_)
  {
    if (bufferId != rendering::BufferId::Invalid)
    {
      renderGraph->getRHI()->deleteBuffer(bufferId);
    }
  }
  frameOverrideInstanceBufferIds_.clear();
  pendingInstanceUpdateDenseIndicesPerFrame_.clear();
  pendingInstanceUpdateMasksPerFrame_.clear();
  pendingTransformUpdatesPerFrame_.clear();
}

void VirtualGeometryScene::destroyFrameOverrideMeshPartTransformsBuffers()
{
  if (renderGraph == nullptr)
    return;

  for (const rendering::BufferId bufferId : frameOverrideMeshPartTransformsBufferIds_)
  {
    if (bufferId != rendering::BufferId::Invalid)
    {
      renderGraph->getRHI()->deleteBuffer(bufferId);
    }
  }
  frameOverrideMeshPartTransformsBufferIds_.clear();
  pendingMeshPartTransformsUpdateDenseIndicesPerFrame_.clear();
  pendingMeshPartTransformsUpdateMasksPerFrame_.clear();
}

void VirtualGeometryScene::destroyFrameOverridePageDecodeBuffers()
{
  if (renderGraph == nullptr)
    return;

  for (const rendering::BufferId bufferId : frameOverridePageDecodeUniformBufferIds_)
  {
    if (bufferId != rendering::BufferId::Invalid)
    {
      renderGraph->getRHI()->deleteBuffer(bufferId);
    }
  }
  for (const rendering::BufferId bufferId : frameOverridePageDecodeRequestBufferIds_)
  {
    if (bufferId != rendering::BufferId::Invalid)
    {
      renderGraph->getRHI()->deleteBuffer(bufferId);
    }
  }
  for (const rendering::BufferId bufferId : frameOverridePageDecodeDispatchIndirectBufferIds_)
  {
    if (bufferId != rendering::BufferId::Invalid)
    {
      renderGraph->getRHI()->deleteBuffer(bufferId);
    }
  }
  frameOverridePageDecodeUniformBufferIds_.clear();
  frameOverridePageDecodeRequestBufferIds_.clear();
  frameOverridePageDecodeDispatchIndirectBufferIds_.clear();
}

rendering::BufferId VirtualGeometryScene::getFrameOverrideInstanceBufferId(uint32_t frameSlot) const
{
  if (frameOverrideInstanceBufferIds_.empty())
    return rendering::BufferId::Invalid;

  return frameOverrideInstanceBufferIds_[frameSlot % frameOverrideInstanceBufferIds_.size()];
}

rendering::BufferId VirtualGeometryScene::getCurrentFrameOverrideInstanceBufferId() const
{
  return getFrameOverrideInstanceBufferId(renderGraph == nullptr ? 0u : renderGraph->getCurrentFrameIndex());
}

rendering::BufferId VirtualGeometryScene::getFrameOverrideMeshPartTransformsBufferId(uint32_t frameSlot) const
{
  if (frameOverrideMeshPartTransformsBufferIds_.empty())
    return rendering::BufferId::Invalid;

  return frameOverrideMeshPartTransformsBufferIds_[frameSlot % frameOverrideMeshPartTransformsBufferIds_.size()];
}

rendering::BufferId VirtualGeometryScene::getCurrentFrameOverrideMeshPartTransformsBufferId() const
{
  return getFrameOverrideMeshPartTransformsBufferId(renderGraph == nullptr ? 0u : renderGraph->getCurrentFrameIndex());
}

rendering::BufferId VirtualGeometryScene::getFrameOverridePageDecodeUniformBufferId(uint32_t frameSlot) const
{
  if (frameOverridePageDecodeUniformBufferIds_.empty())
    return rendering::BufferId::Invalid;

  return frameOverridePageDecodeUniformBufferIds_[frameSlot % frameOverridePageDecodeUniformBufferIds_.size()];
}

rendering::BufferId VirtualGeometryScene::getFrameOverridePageDecodeRequestBufferId(uint32_t frameSlot) const
{
  if (frameOverridePageDecodeRequestBufferIds_.empty())
    return rendering::BufferId::Invalid;

  return frameOverridePageDecodeRequestBufferIds_[frameSlot % frameOverridePageDecodeRequestBufferIds_.size()];
}

rendering::BufferId VirtualGeometryScene::getFrameOverridePageDecodeDispatchIndirectBufferId(uint32_t frameSlot) const
{
  if (frameOverridePageDecodeDispatchIndirectBufferIds_.empty())
    return rendering::BufferId::Invalid;

  return frameOverridePageDecodeDispatchIndirectBufferIds_[frameSlot % frameOverridePageDecodeDispatchIndirectBufferIds_.size()];
}

void VirtualGeometryScene::updatePageDecodeBuffers(
    rendering::BufferId uniformOverrideId,
    rendering::BufferId requestOverrideId,
    rendering::BufferId dispatchOverrideId,
    bool clearPendingRequests)
{
  const uint32_t requestCount = static_cast<uint32_t>(std::min<size_t>(pendingPageDecodeRequests_.size(), static_cast<size_t>(maxPages)));
  PageDecodeUniformsGPU uniforms{};
  uniforms.requestCount = requestCount;
  uniforms._padding0 = settings.compressedGeometry ? 1u : 0u;
  DispatchIndirectArgsGPU dispatchArgs{};
  dispatchArgs.groupCountX = requestCount;

  if (uniformOverrideId != rendering::BufferId::Invalid && requestOverrideId != rendering::BufferId::Invalid && dispatchOverrideId != rendering::BufferId::Invalid)
  {
    renderGraph->getRHI()->bufferWrite(uniformOverrideId, 0, sizeof(PageDecodeUniformsGPU), &uniforms);
    if (requestCount != 0u)
    {
      renderGraph->getRHI()->bufferWrite(requestOverrideId, 0, static_cast<uint64_t>(requestCount) * sizeof(uint32_t), pendingPageDecodeRequests_.data());
    }
    renderGraph->getRHI()->bufferWrite(dispatchOverrideId, 0, sizeof(DispatchIndirectArgsGPU), &dispatchArgs);
  }
  else
  {
    writeSceneBuffer(pageDecodeUniformBuffer, 0, sizeof(PageDecodeUniformsGPU), &uniforms);
    if (requestCount != 0u)
    {
      writeSceneBuffer(pageDecodeRequestBuffer, 0, static_cast<uint64_t>(requestCount) * sizeof(uint32_t), pendingPageDecodeRequests_.data());
    }
    writeSceneBuffer(pageDecodeDispatchIndirectBuffer, 0, sizeof(DispatchIndirectArgsGPU), &dispatchArgs);
  }

  if (clearPendingRequests)
  {
    pendingPageDecodeRequests_.clear();
  }
}

void VirtualGeometryScene::writeInstanceRecordToCurrentBuffers(uint32_t denseIndex, const VirtualGeometryInstanceGPUData &instanceData)
{
  const uint64_t offset = static_cast<uint64_t>(denseIndex) * sizeof(VirtualGeometryInstanceGPUData);
  ensureInstanceBufferFits(offset + sizeof(VirtualGeometryInstanceGPUData), instanceBufferSize);
  writeSceneBuffer(instanceBuffer, offset, sizeof(VirtualGeometryInstanceGPUData), const_cast<VirtualGeometryInstanceGPUData *>(&instanceData));
}

void VirtualGeometryScene::writeMeshPartTransformsRecordToCurrentBuffers(uint32_t denseIndex)
{
  if (denseIndex >= objectInstances.size())
    return;

  const ObjectInstanceRecord &record = objectInstances[denseIndex];
  if (record.currentMeshPartTransforms.empty())
    return;

  writeSceneBuffer(
      meshPartTransformsBuffer,
      static_cast<uint64_t>(record.meshPartTransformsOffset) * sizeof(math::Mat4f),
      static_cast<uint64_t>(record.currentMeshPartTransforms.size()) * sizeof(math::Mat4f),
      const_cast<math::Mat4f *>(record.currentMeshPartTransforms.data()));
  writeSceneBuffer(
      meshPartTransformsBuffer,
      static_cast<uint64_t>(record.previousMeshPartTransformsOffset) * sizeof(math::Mat4f),
      static_cast<uint64_t>(record.previousMeshPartTransforms.size()) * sizeof(math::Mat4f),
      const_cast<math::Mat4f *>(record.previousMeshPartTransforms.data()));
}

void VirtualGeometryScene::writePackedInstanceDataToBuffers(const std::vector<VirtualGeometryInstanceGPUData> &packedData, bool uploadAllFrameSlots)
{
  const uint64_t uploadSize = packedData.size() * sizeof(VirtualGeometryInstanceGPUData);
  ensureInstanceBufferFits(uploadSize, instanceBufferSize);
  writeSceneBuffer(instanceBuffer, 0, uploadSize, (void *)packedData.data());

  if (frameOverrideInstanceBufferIds_.empty())
    return;

  if (uploadAllFrameSlots)
  {
    for (const rendering::BufferId bufferId : frameOverrideInstanceBufferIds_)
    {
      if (bufferId != rendering::BufferId::Invalid)
      {
        renderGraph->getRHI()->bufferWrite(bufferId, 0, uploadSize, const_cast<VirtualGeometryInstanceGPUData *>(packedData.data()));
      }
    }
    for (uint32_t frameSlot = 0u; frameSlot < frameOverrideInstanceBufferIds_.size(); ++frameSlot)
    {
      clearPendingInstanceRecordUpdates(frameSlot);
    }
    return;
  }

  const rendering::BufferId overrideId = getCurrentFrameOverrideInstanceBufferId();
  if (overrideId != rendering::BufferId::Invalid)
  {
    renderGraph->getRHI()->bufferWrite(overrideId, 0, uploadSize, const_cast<VirtualGeometryInstanceGPUData *>(packedData.data()));
    clearPendingInstanceRecordUpdates(renderGraph->getCurrentFrameIndex());
  }
}

void VirtualGeometryScene::queueInstanceRecordUpdateForAllFrameSlots(uint32_t denseIndex)
{
  if (frameOverrideInstanceBufferIds_.empty())
    return;

  for (uint32_t frameSlot = 0u; frameSlot < pendingInstanceUpdateMasksPerFrame_.size(); ++frameSlot)
  {
    auto &pendingMask = pendingInstanceUpdateMasksPerFrame_[frameSlot];
    if (denseIndex >= pendingMask.size())
    {
      pendingMask.resize(std::max<size_t>(denseIndex + 1u, instanceManager.packedInstances.size()), 0u);
    }
    if (pendingMask[denseIndex] != 0u)
      continue;

    pendingMask[denseIndex] = 1u;
    pendingInstanceUpdateDenseIndicesPerFrame_[frameSlot].push_back(denseIndex);
  }
}

void VirtualGeometryScene::queueTransformUpdateForAllFrameSlots(const SceneInstanceTransformUpdate &update)
{
  if (pendingTransformUpdatesPerFrame_.empty())
  {
    const uint32_t frameCount = renderGraph == nullptr ? 1u : std::max(1u, renderGraph->getMaxFramesInFlight());
    pendingTransformUpdatesPerFrame_.assign(frameCount, {});
  }

  for (uint32_t frameSlot = 0u; frameSlot < pendingTransformUpdatesPerFrame_.size(); ++frameSlot)
  {
    SceneInstanceTransformUpdate frameUpdate = update;
    frameUpdate.frameSlot = frameSlot;
    pendingTransformUpdatesPerFrame_[frameSlot].push_back(frameUpdate);
  }
}

void VirtualGeometryScene::queueMeshPartTransformsRecordUpdateForAllFrameSlots(uint32_t denseIndex)
{
  if (frameOverrideMeshPartTransformsBufferIds_.empty())
    return;

  for (uint32_t frameSlot = 0u; frameSlot < pendingMeshPartTransformsUpdateMasksPerFrame_.size(); ++frameSlot)
  {
    auto &pendingMask = pendingMeshPartTransformsUpdateMasksPerFrame_[frameSlot];
    if (denseIndex >= pendingMask.size())
    {
      pendingMask.resize(std::max<size_t>(denseIndex + 1u, objectInstances.size()), 0u);
    }
    if (pendingMask[denseIndex] != 0u)
      continue;

    pendingMask[denseIndex] = 1u;
    pendingMeshPartTransformsUpdateDenseIndicesPerFrame_[frameSlot].push_back(denseIndex);
  }
}

const std::vector<VirtualGeometryScene::SceneInstanceTransformUpdate> &VirtualGeometryScene::getFrameTransformUpdates(uint32_t frameSlot) const
{
  static const std::vector<SceneInstanceTransformUpdate> emptyUpdates;
  if (pendingTransformUpdatesPerFrame_.empty())
    return emptyUpdates;

  return pendingTransformUpdatesPerFrame_[frameSlot % pendingTransformUpdatesPerFrame_.size()];
}

std::vector<VirtualGeometryScene::SceneInstanceTransformUpdate> VirtualGeometryScene::consumeFrameTransformUpdates(uint32_t frameSlot)
{
  if (pendingTransformUpdatesPerFrame_.empty())
    return {};

  const uint32_t resolvedFrameSlot = frameSlot % pendingTransformUpdatesPerFrame_.size();
  std::vector<SceneInstanceTransformUpdate> updates = std::move(pendingTransformUpdatesPerFrame_[resolvedFrameSlot]);
  pendingTransformUpdatesPerFrame_[resolvedFrameSlot].clear();
  return updates;
}

void VirtualGeometryScene::clearFrameTransformUpdates(uint32_t frameSlot)
{
  if (pendingTransformUpdatesPerFrame_.empty())
    return;

  pendingTransformUpdatesPerFrame_[frameSlot % pendingTransformUpdatesPerFrame_.size()].clear();
}

void VirtualGeometryScene::clearPendingInstanceRecordUpdates(uint32_t frameSlot)
{
  if (pendingInstanceUpdateDenseIndicesPerFrame_.empty())
    return;

  const uint32_t resolvedFrameSlot = frameSlot % pendingInstanceUpdateDenseIndicesPerFrame_.size();
  auto &pendingDenseIndices = pendingInstanceUpdateDenseIndicesPerFrame_[resolvedFrameSlot];
  auto &pendingMask = pendingInstanceUpdateMasksPerFrame_[resolvedFrameSlot];
  for (const uint32_t denseIndex : pendingDenseIndices)
  {
    if (denseIndex < pendingMask.size())
    {
      pendingMask[denseIndex] = 0u;
    }
  }
  pendingDenseIndices.clear();
}

void VirtualGeometryScene::clearPendingMeshPartTransformsRecordUpdates(uint32_t frameSlot)
{
  if (pendingMeshPartTransformsUpdateDenseIndicesPerFrame_.empty())
    return;

  const uint32_t resolvedFrameSlot = frameSlot % pendingMeshPartTransformsUpdateDenseIndicesPerFrame_.size();
  auto &pendingDenseIndices = pendingMeshPartTransformsUpdateDenseIndicesPerFrame_[resolvedFrameSlot];
  auto &pendingMask = pendingMeshPartTransformsUpdateMasksPerFrame_[resolvedFrameSlot];
  for (const uint32_t denseIndex : pendingDenseIndices)
  {
    if (denseIndex < pendingMask.size())
    {
      pendingMask[denseIndex] = 0u;
    }
  }
  pendingDenseIndices.clear();
}

// ============================================================================
// Page streaming
// ============================================================================

static void printHierarchy(const std::string &label, const std::vector<VirtualGeometryHierarchy> &hierarchy)
{
  os::Logger::warningf("[Hierarchy] ===== %s =====", label.c_str());
  for (uint32_t i = 0; i < static_cast<uint32_t>(hierarchy.size()); ++i)
  {
    const VirtualGeometryHierarchy &node = hierarchy[i];
    const bool isLeaf = (node.flags & 1u) != 0u;
    const bool notInstalled = (node.pageIndex & PAGE_NOT_INSTALLED_BIT) != 0u;
    const uint32_t rawPage = node.pageIndex & ~PAGE_NOT_INSTALLED_BIT;
    const bool noChildren = (node.child_start == UINT32_MAX);

    os::Logger::warningf(
        "[Hierarchy] node[%4u] %s  page=%u(%s)  child_start=%s  child_count=%u  min_lod_err=%.4f  max_parent_lod_err=%.4f",
        i,
        isLeaf ? "LEAF " : "GROUP",
        rawPage,
        notInstalled ? "NOT_INSTALLED" : "installed",
        noChildren ? "NONE" : std::to_string(node.child_start).c_str(),
        node.child_count,
        node.min_lod_error,
        node.max_parent_lod_error);
  }
  os::Logger::warningf("[Hierarchy] ===== end (%zu nodes) =====", hierarchy.size());
}

static std::vector<StreamingPageCandidate> readStreamingCandidates(rendering::RenderGraph *renderGraph, const rendering::Buffer &buffer, uint32_t candidateCount)
{
  std::vector<StreamingPageCandidate> candidates(candidateCount);
  renderGraph->bufferRead(
      buffer,
      0,
      static_cast<uint64_t>(candidateCount) * sizeof(StreamingPageCandidate),
      [&candidates](const void *gpuData)
      {
        std::memcpy(candidates.data(), gpuData, candidates.size() * sizeof(StreamingPageCandidate));
      });
  return candidates;
}

static std::vector<uint32_t> readPagePriorities(rendering::RenderGraph *renderGraph, const rendering::Buffer &buffer, uint64_t bufferSize)
{
  std::vector<uint32_t> priorities(static_cast<size_t>(bufferSize / sizeof(uint32_t)), 0u);
  renderGraph->bufferRead(
      buffer,
      0,
      bufferSize,
      [&priorities](const void *gpuData)
      {
        std::memcpy(priorities.data(), gpuData, priorities.size() * sizeof(uint32_t));
      });
  return priorities;
}

static bool hasInRangeCandidate(const std::vector<StreamingPageCandidate> &candidates, uint32_t objectPageBegin, uint32_t objectPageEnd)
{
  for (const StreamingPageCandidate &candidate : candidates)
  {
    if (candidate.priority == 0u || candidate.globalPageIndex == UINT32_MAX)
      continue;
    if (candidate.globalPageIndex >= objectPageBegin && candidate.globalPageIndex < objectPageEnd)
      return true;
  }
  return false;
}

static std::vector<StreamingPageCandidate>
buildCpuCandidates(const std::vector<PageAllocation> &allocations, const std::vector<uint32_t> &priorities, uint32_t objectPageBegin, bool installedOnly, bool descendingPriority)
{
  std::vector<StreamingPageCandidate> allCandidates;
  allCandidates.reserve(allocations.size());
  for (const PageAllocation &alloc : allocations)
  {
    if (alloc.pageId == UINT32_MAX)
      continue;
    if (alloc.isInstalled != installedOnly)
      continue;

    const uint32_t priority = alloc.prioritySlot < priorities.size() ? priorities[alloc.prioritySlot] : 0u;
    if (!installedOnly && priority == 0u)
      continue;

    allCandidates.push_back(
        StreamingPageCandidate{
          .globalPageIndex = objectPageBegin + alloc.pageId,
          .priority = priority,
        });
  }

  std::sort(
      allCandidates.begin(),
      allCandidates.end(),
      [descendingPriority](const StreamingPageCandidate &a, const StreamingPageCandidate &b)
      {
        if (a.priority != b.priority)
          return descendingPriority ? (a.priority > b.priority) : (a.priority < b.priority);
        return a.globalPageIndex < b.globalPageIndex;
      });

  if (allCandidates.size() > VirtualGeometryScene::STREAMING_PAGE_SELECTION_COUNT)
    allCandidates.resize(VirtualGeometryScene::STREAMING_PAGE_SELECTION_COUNT);

  std::vector<StreamingPageCandidate> output(VirtualGeometryScene::STREAMING_PAGE_SELECTION_COUNT);
  for (size_t i = 0; i < allCandidates.size(); ++i)
    output[i] = allCandidates[i];
  return output;
}

static void logStreamingCandidates(const std::string &objectName, const char *label, const std::vector<StreamingPageCandidate> &candidates, uint32_t objectPageBegin, uint32_t objectPageEnd, uint32_t maxLines = 8u)
{
  uint32_t logged = 0u;
  uint32_t inRange = 0u;
  for (const StreamingPageCandidate &candidate : candidates)
  {
    if (candidate.globalPageIndex == UINT32_MAX || candidate.priority == 0u)
      continue;
    if (candidate.globalPageIndex < objectPageBegin || candidate.globalPageIndex >= objectPageEnd)
      continue;

    ++inRange;
    if (logged >= maxLines)
      continue;

    // os::Logger::warningf("[Streaming][%s] %s candidate[%u]: global=%u local=%u priority=%u", objectName.c_str(), label, logged, candidate.globalPageIndex, candidate.globalPageIndex - objectPageBegin, candidate.priority);
    ++logged;
  }

  if (inRange == 0u)
  {
    // os::Logger::warningf("[Streaming][%s] %s candidates: none", objectName.c_str(), label);
    return;
  }

  if (inRange > logged)
  {
    // os::Logger::warningf("[Streaming][%s] %s candidates: logged %u of %u in-range entries", objectName.c_str(), label, logged, inRange);
  }
}

void VirtualGeometryScene::updatePageStreaming(const std::string &objectName, uint32_t maxPageInstallsPerUpdate)
{
  auto runtimeData = files.find(objectName);
  if (runtimeData == files.end())
    return;

  VirtualGeometryObjectRuntimeData *data = runtimeData.value();
  VirtualGeometryStreamingManager &streamingManager = *data->streamingManager;
  streamingManager.pumpCompletedLoads();

  const size_t pageCount = data->file.getPageTable().size();
  if (pageCount == 0u)
    return;

  auto installCandidates = readStreamingCandidates(renderGraph, pageInstallCandidateBuffer, STREAMING_PAGE_SELECTION_COUNT);
  auto evictCandidates = readStreamingCandidates(renderGraph, pageEvictCandidateBuffer, STREAMING_PAGE_SELECTION_COUNT);

  const uint32_t objectPageBegin = data->pageTableOffset;
  const uint32_t objectPageEnd = objectPageBegin + static_cast<uint32_t>(pageCount);
  const auto &allocations = streamingManager.getPageAllocations();
  if (!hasInRangeCandidate(installCandidates, objectPageBegin, objectPageEnd))
  {
    const auto allPriorities = readPagePriorities(renderGraph, pagePriorityBuffer, pagePriorityBufferSize);
    uint32_t nonZeroObjectPriorities = 0u;
    for (const PageAllocation &alloc : allocations)
    {
      if (alloc.prioritySlot < allPriorities.size() && allPriorities[alloc.prioritySlot] > 0u)
        ++nonZeroObjectPriorities;
    }

    os::Logger::warningf("[Streaming][%s] GPU install candidates empty, CPU fallback scanning raw priorities (nonZeroObjectPriorities=%u)", objectName.c_str(), nonZeroObjectPriorities);

    if (nonZeroObjectPriorities != 0u)
    {
      installCandidates = buildCpuCandidates(allocations, allPriorities, objectPageBegin, false, true);
      evictCandidates = buildCpuCandidates(allocations, allPriorities, objectPageBegin, true, false);
    }
  }

  logStreamingCandidates(objectName, "install", installCandidates, objectPageBegin, objectPageEnd);
  logStreamingCandidates(objectName, "evict", evictCandidates, objectPageBegin, objectPageEnd);
  os::Logger::warningf(
      "[Streaming][%s] state before update: pending=%u ready=%u maxInstalls=%u objectPages=[%u,%u)",
      objectName.c_str(),
      streamingManager.getPendingLoadCount(),
      streamingManager.getReadyPageCount(),
      maxPageInstallsPerUpdate,
      objectPageBegin,
      objectPageEnd);

  std::vector<std::pair<uint32_t, uint32_t>> pagesToRequest;
  pagesToRequest.reserve(installCandidates.size());
  for (const StreamingPageCandidate &candidate : installCandidates)
  {
    if (candidate.globalPageIndex == UINT32_MAX || candidate.priority == 0u)
      continue;
    if (candidate.globalPageIndex < objectPageBegin || candidate.globalPageIndex >= objectPageEnd)
      continue;

    const uint32_t localPageId = candidate.globalPageIndex - objectPageBegin;
    if (streamingManager.isPageInstalled(localPageId) || streamingManager.isPageLoadPending(localPageId) || streamingManager.hasDecodedPageReady(localPageId))
      continue;

    pagesToRequest.push_back({localPageId, candidate.priority});
  }

  // if (pagesToRequest.empty())
  // {
  //   os::Logger::warningf("[Streaming][%s] no install requests selected from GPU candidates", objectName.c_str());
  // }
  // else
  // {
  //   const uint32_t logCount = std::min<uint32_t>(static_cast<uint32_t>(pagesToRequest.size()), 8u);
  //   for (uint32_t i = 0u; i < logCount; ++i)
  //   {
  //     os::Logger::warningf("[Streaming][%s] request pick[%u]: local=%u priority=%u", objectName.c_str(), i, pagesToRequest[i].first, pagesToRequest[i].second);
  //   }
  //   if (pagesToRequest.size() > logCount)
  //     os::Logger::warningf("[Streaming][%s] request picks: logged %u of %zu", objectName.c_str(), logCount, pagesToRequest.size());
  // }

  std::vector<uint8_t> isRootPage(pageCount, 0u);
  for (uint32_t rootPageId : data->pageDependencies.rootPageIds)
    if (rootPageId < pageCount)
      isRootPage[rootPageId] = 1u;

  uint32_t installedPagesThisUpdate = 0u;
  size_t evictCandidateIndex = 0u;
  bool residentPagesChanged = false;
  while (installedPagesThisUpdate < maxPageInstallsPerUpdate)
  {
    bool blockedOnMemory = false;
    try
    {
      const uint32_t installedNow = streamingManager.installReadyPages(data->pageDependencies, maxPageInstallsPerUpdate - installedPagesThisUpdate, &blockedOnMemory);
      installedPagesThisUpdate += installedNow;
      residentPagesChanged = residentPagesChanged || (installedNow != 0u);
    }
    catch (const std::exception &e)
    {
      // os::Logger::warningf("[VirtualGeometryScene] installReadyPages failed for %s: %s", objectName.c_str(), e.what());
      break;
    }

    if (!blockedOnMemory)
      break;

    // os::Logger::warningf("[Streaming][%s] install blocked on memory, scanning eviction candidates", objectName.c_str());

    bool evictedPage = false;
    while (evictCandidateIndex < evictCandidates.size())
    {
      const StreamingPageCandidate &candidate = evictCandidates[evictCandidateIndex++];
      if (candidate.globalPageIndex == UINT32_MAX)
        continue;
      if (candidate.globalPageIndex < objectPageBegin || candidate.globalPageIndex >= objectPageEnd)
        continue;

      const uint32_t pageId = candidate.globalPageIndex - objectPageBegin;
      if (!streamingManager.isPageInstalled(pageId) || isRootPage[pageId])
        continue;

      bool hasInstalledChildren = false;
      for (uint32_t childId : data->pageDependencies.children[pageId])
      {
        if (!streamingManager.isPageInstalled(childId))
          continue;
        hasInstalledChildren = true;
        break;
      }

      if (hasInstalledChildren)
        continue;

      streamingManager.uninstallPage(pageId);
      residentPagesChanged = true;
      // os::Logger::warningf("[Streaming][%s] evicted local page %u due to install pressure", objectName.c_str(), pageId);
      evictedPage = true;
      break;
    }

    if (!evictedPage)
    {
      // os::Logger::warningf("[Streaming][%s] no evictable page found while blocked on memory", objectName.c_str());
      break;
    }
  }

  uint32_t requestsEmitted = 0u;
  std::vector<uint8_t> requestVisited(pageCount, 0u);
  std::function<void(uint32_t)> requestWithParents = [&](uint32_t pageId)
  {
    if (pageId >= pageCount || requestVisited[pageId] != 0u || requestsEmitted >= maxPageInstallsPerUpdate)
      return;
    requestVisited[pageId] = 1u;

    for (uint32_t parentId : data->pageDependencies.parents[pageId])
      requestWithParents(parentId);

    if (streamingManager.isPageInstalled(pageId) || streamingManager.isPageLoadPending(pageId) || streamingManager.hasDecodedPageReady(pageId))
      return;

    streamingManager.requestPageLoad(pageId);
    // os::Logger::warningf("[Streaming][%s] queued load request for local page %u", objectName.c_str(), pageId);
    ++requestsEmitted;
  };

  for (const auto &[pageId, _priority] : pagesToRequest)
  {
    if (requestsEmitted >= maxPageInstallsPerUpdate)
      break;
    requestWithParents(pageId);
  }

  if (residentPagesChanged && streamingResidencyInvalidationCallback_)
  {
    for (const ObjectInstanceRecord &record : objectInstances)
    {
      if (record.objectName != objectName)
      {
        continue;
      }
      if (!isValidAABB(data->localBounds))
      {
        continue;
      }

      AABB worldBounds;
      bool hasWorldBounds = false;
      for (SceneInstanceId sceneInstanceId : record.sceneInstanceIds)
      {
        const VirtualGeometryInstanceGPUData *instance = instanceManager.getInstance(sceneInstanceId);
        if (instance == nullptr)
        {
          continue;
        }

        const AABB instanceBounds = transformAABB(data->localBounds, instance->modelMatrix);
        if (!hasWorldBounds)
        {
          worldBounds = instanceBounds;
          hasWorldBounds = true;
        }
        else
        {
          worldBounds.expandBy(instanceBounds);
        }
      }

      if (hasWorldBounds)
      {
        streamingResidencyInvalidationCallback_(worldBounds);
      }
    }
  }

  os::Logger::warningf(
      "[Streaming][%s] update summary: installed=%u requested=%u pending=%u ready=%u",
      objectName.c_str(),
      installedPagesThisUpdate,
      requestsEmitted,
      streamingManager.getPendingLoadCount(),
      streamingManager.getReadyPageCount());

  // printHierarchy("AFTER  install/uninstall [" + objectName + "]", data->runtimeHierarchy);
}

void VirtualGeometryScene::setStreamingResidencyInvalidationCallback(std::function<void(const AABB &)> callback)
{
  streamingResidencyInvalidationCallback_ = std::move(callback);
}

bool VirtualGeometryScene::registerRuntimeObject(
    const RuntimeObjectCreateInfo &createInfo,
    RuntimeObjectId *outObjectId,
    std::string *error)
{
  if (outObjectId != nullptr)
    *outObjectId = INVALID_RUNTIME_OBJECT_ID;
  if (error != nullptr)
    error->clear();
  if (createInfo.name.empty())
    return setError(error, "runtime virtual-geometry object needs a name");
  if (createInfo.pageCount == 0u)
    return setError(error, "runtime virtual-geometry object needs at least one page");
  if (createInfo.hierarchy.empty())
    return setError(error, "runtime virtual-geometry object needs a hierarchy root");
  if (runtimeObjectByName_.find(createInfo.name) != runtimeObjectByName_.end())
    return setError(error, "runtime virtual-geometry object is already registered: " + createInfo.name);
  if (files.find(createInfo.name) != files.end())
    return setError(error, "runtime virtual-geometry object name collides with a file-backed object: " + createInfo.name);
  const rendering::Allocation pageTableSlots =
      pageTableSlotAllocator.allocate(createInfo.pageCount);
  if (pageTableSlots.offset == UINT64_MAX)
    return setError(error, "runtime virtual-geometry object page table exhausted: " + createInfo.name);
  const rendering::Allocation prioritySlots =
      prioritySlotAllocator.allocate(createInfo.pageCount);
  if (prioritySlots.offset == UINT64_MAX)
  {
    pageTableSlotAllocator.deallocate(pageTableSlots);
    return setError(error, "runtime virtual-geometry object priority table exhausted: " + createInfo.name);
  }

  const uint64_t hierarchyBytes =
      static_cast<uint64_t>(createInfo.hierarchy.size()) *
      sizeof(VirtualGeometryHierarchy);
  if (!hierarchyBufferAllocator.canAllocate(hierarchyBytes))
    evictLeastRecentlyUsedRuntimeObjects(hierarchyBytes);
  rendering::Allocation hierarchyAllocation =
      hierarchyBufferAllocator.allocate(hierarchyBytes);
  if (hierarchyAllocation.offset == UINT64_MAX)
  {
    pageTableSlotAllocator.deallocate(pageTableSlots);
    prioritySlotAllocator.deallocate(prioritySlots);
    return setError(
        error,
        "runtime virtual-geometry object hierarchy buffer exhausted: " + createInfo.name +
            (deferredHierarchyBytes_ != 0u
                 ? " (" + std::to_string(deferredHierarchyBytes_) +
                       " bytes await deferred release; retry next frame)"
                 : std::string()));
  }

  const uint32_t pageTableOffset = static_cast<uint32_t>(pageTableSlots.offset);
  const uint32_t prioritySlotOffset = static_cast<uint32_t>(prioritySlots.offset);
  // High-water marks only: the culler clears priorities over [0, nextPageTableSlot),
  // so they must cover every slot ever handed out, not just the live ones.
  nextPageTableSlot = std::max(nextPageTableSlot, pageTableOffset + createInfo.pageCount);
  nextPrioritySlot = std::max(nextPrioritySlot, prioritySlotOffset + createInfo.pageCount);

  writeSceneBuffer(
      hierarchyBuffer,
      hierarchyAllocation.offset,
      hierarchyBytes,
      const_cast<VirtualGeometryHierarchy *>(createInfo.hierarchy.data()));

  for (uint32_t pageIndex = 0u; pageIndex < createInfo.pageCount; ++pageIndex)
  {
    PageTableEntry entry{};
    entry.isInstalled = 0u;
    entry.prioritySlot = prioritySlotOffset + pageIndex;
    writeSceneBuffer(
        pageTableBuffer,
        static_cast<uint64_t>(pageTableOffset + pageIndex) * sizeof(PageTableEntry),
        sizeof(PageTableEntry),
        &entry);
  }

  std::vector<uint32_t> zeroPriorities(createInfo.pageCount, 0u);
  writeSceneBuffer(
      pagePriorityBuffer,
      static_cast<uint64_t>(prioritySlotOffset) * sizeof(uint32_t),
      static_cast<uint64_t>(zeroPriorities.size()) * sizeof(uint32_t),
      zeroPriorities.data());

  const uint32_t hierarchyStartOffset =
      static_cast<uint32_t>(hierarchyAllocation.offset / sizeof(VirtualGeometryHierarchy));
  const SceneInstanceId sceneInstanceId = instanceManager.allocateInstance(
      createInfo.name,
      createInfo.position,
      createInfo.rotation,
      createInfo.scale,
      hierarchyStartOffset,
      pageTableOffset,
      createInfo.materialIndex,
      0u,
      0u,
      createInfo.quantizationConfig);
  publishInstanceLayoutChange();

  // Reuse a tombstoned slot when one exists so a scene that rebuilds objects
  // continuously does not grow runtimeObjects_ without bound.
  RuntimeObjectId reusableId = INVALID_RUNTIME_OBJECT_ID;
  for (size_t index = 0u; index < runtimeObjects_.size(); ++index)
    if (!runtimeObjects_[index].registered)
    {
      reusableId = static_cast<RuntimeObjectId>(index);
      break;
    }

  RuntimeObjectRecord record{};
  record.objectId = reusableId != INVALID_RUNTIME_OBJECT_ID
      ? reusableId
      : static_cast<RuntimeObjectId>(runtimeObjects_.size());
  record.registered = true;
  record.pageTableSlots = pageTableSlots;
  record.prioritySlots = prioritySlots;
  record.name = createInfo.name;
  record.pageTableOffset = pageTableOffset;
  record.pageCount = createInfo.pageCount;
  record.prioritySlotOffset = prioritySlotOffset;
  record.hierarchyAllocation = hierarchyAllocation;
  record.hierarchyStartOffset = hierarchyStartOffset;
  record.sceneInstanceId = sceneInstanceId;
  record.quantizationConfig = createInfo.quantizationConfig;
  record.localBounds = createInfo.localBounds;
  record.hierarchy = createInfo.hierarchy;
  record.pages.resize(createInfo.pageCount);
  for (uint32_t pageIndex = 0u; pageIndex < createInfo.pageCount; ++pageIndex)
  {
    RuntimePageStatus &status = record.pages[pageIndex];
    status.localPageIndex = pageIndex;
    status.globalPageTableIndex = pageTableOffset + pageIndex;
    status.prioritySlot = prioritySlotOffset + pageIndex;
    status.pageTableEntry.prioritySlot = status.prioritySlot;
  }

  const RuntimeObjectId assignedId = record.objectId;
  runtimeObjectByName_[record.name] = assignedId;
  if (reusableId != INVALID_RUNTIME_OBJECT_ID)
    runtimeObjects_[reusableId] = std::move(record);
  else
    runtimeObjects_.push_back(std::move(record));
  if (outObjectId != nullptr)
    *outObjectId = assignedId;
  return true;
}

bool VirtualGeometryScene::installRuntimeEncodedObject(
    const RuntimeEncodedObjectInstallInfo &install,
    RuntimeEncodedObjectInstallStats *stats,
    std::string *error)
{
  if (stats != nullptr)
    *stats = {};
  if (error != nullptr)
    error->clear();
  if (install.name.empty())
    return setError(error, "runtime virtual-geometry encoded object needs a name");
  if (install.asset == nullptr)
    return setError(error, "runtime virtual-geometry encoded object needs an asset");
  const VirtualGeometryEncodedData &asset = *install.asset;
  if (asset.hierarchy.empty())
    return setError(error, "runtime virtual-geometry encoded object has no hierarchy");
  if (asset.pages.empty())
    return setError(error, "runtime virtual-geometry encoded object has no pages");
  if (asset.pages.size() > std::numeric_limits<uint32_t>::max())
    return setError(error, "runtime virtual-geometry encoded object has too many pages");
  if (asset.rootPageIndex >= asset.pages.size())
    return setError(error, "runtime virtual-geometry encoded object root page is out of range");
  if (install.basePageGeneration == 0u ||
      install.geometryRevision == 0u ||
      install.materialRevision == 0u)
  {
    return setError(error, "runtime virtual-geometry encoded object revisions must be non-zero");
  }

  AABB localBounds{};
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
    return setError(error, "runtime virtual-geometry encoded object has no finite bounds");

  std::vector<PageBuffer> encodedPages;
  encodedPages.reserve(asset.pages.size());
  for (uint32_t pageIndex = 0u; pageIndex < asset.pages.size(); ++pageIndex)
  {
    PageBuffer encoded = PageBuffer::encode(asset.pages[pageIndex]);
    if (encoded.data.empty())
      return setError(error, "runtime virtual-geometry encoded object produced an empty encoded page");
    encodedPages.push_back(std::move(encoded));
  }

  RuntimeObjectCreateInfo createInfo{};
  createInfo.name = install.name;
  createInfo.pageCount = static_cast<uint32_t>(asset.pages.size());
  createInfo.hierarchy = asset.hierarchy;
  createInfo.quantizationConfig = asset.quantizationConfig;
  createInfo.localBounds = localBounds;
  createInfo.position = install.position;
  createInfo.rotation = install.rotation;
  createInfo.scale = install.scale;
  createInfo.materialIndex = install.materialIndex;

  RuntimeObjectId objectId = INVALID_RUNTIME_OBJECT_ID;
  if (!registerRuntimeObject(createInfo, &objectId, error))
    return false;

  RuntimeEncodedObjectInstallStats localStats{};
  localStats.objectId = objectId;
  uint32_t unitScaleBits = 0u;
  std::memcpy(&unitScaleBits, &asset.quantizationConfig.unit_scale, sizeof(unitScaleBits));

  for (uint32_t pageIndex = 0u; pageIndex < asset.pages.size(); ++pageIndex)
  {
    RuntimeEncodedPageInstallInfo pageInstall{};
    pageInstall.localPageIndex = pageIndex;
    pageInstall.pageGeneration = install.basePageGeneration + pageIndex;
    pageInstall.geometryRevision = install.geometryRevision;
    pageInstall.materialRevision = install.materialRevision;
    pageInstall.unitScaleBits = unitScaleBits;
    pageInstall.rootPage = pageIndex <= asset.rootPageIndex;
    pageInstall.hasTracePayload = install.hasTracePayload;
    pageInstall.hasMaterialPayload = install.hasMaterialPayload;
    pageInstall.enqueuePageDecode = install.enqueuePageDecode;
    pageInstall.encodedPageWords = encodedPages[pageIndex].data;
    // The encoded asset carries the page's own hierarchy rewrites. Passing them
    // through is what gives the installed object a real LOD cut instead of
    // every level drawing at once.
    pageInstall.hierarchyClusterFlagUpdates =
        asset.pages[pageIndex].installUpdates.hierarchyUpdates;
    pageInstall.dependencies = asset.pages[pageIndex].dependencies;

    if (!installRuntimeObjectEncodedPage(install.name, pageInstall, error))
      return false;

    ++localStats.installedPageCount;
    localStats.queuedDecodePageCount += install.enqueuePageDecode ? 1u : 0u;
    localStats.encodedMeshletCount +=
        static_cast<uint32_t>(asset.pages[pageIndex].meshlets.size());
    localStats.encodedPageBytes +=
        static_cast<uint64_t>(encodedPages[pageIndex].data.size()) * sizeof(uint32_t);
  }

  if (stats != nullptr)
    *stats = localStats;
  return true;
}

bool VirtualGeometryScene::reserveRuntimeGeneratedPage(
    const RuntimeGeneratedPageReserveInfo &reserve,
    RuntimeGeneratedPageReservation &reservation,
    std::string *error)
{
  reservation = {};
  if (error != nullptr)
    error->clear();
  if (reserve.objectName.empty())
    return setError(error, "runtime generated virtual-geometry page needs an object name");
  const auto objectIt = runtimeObjectByName_.find(reserve.objectName);
  if (objectIt == runtimeObjectByName_.end())
    return setError(error, "runtime generated virtual-geometry object is not registered: " + reserve.objectName);
  RuntimeObjectRecord &record = runtimeObjects_[objectIt->second];
  if (reserve.localPageIndex >= record.pageCount)
    return setError(error, "runtime generated virtual-geometry page index is out of range: " + reserve.objectName);
  if (reserve.pageGeneration == 0u || reserve.geometryRevision == 0u ||
      reserve.materialRevision == 0u)
  {
    return setError(error, "runtime generated virtual-geometry page revisions must be non-zero");
  }
  if (reserve.clusterCount == 0u || reserve.clusterCount > MAX_CLUSTERS_PER_PAGE)
    return setError(error, "runtime generated virtual-geometry page cluster count is invalid");
  if (reserve.decodedVertexCount == 0u || reserve.decodedIndexCount == 0u)
    return setError(error, "runtime generated virtual-geometry page decoded geometry budget is invalid");
  if (reserve.encodedPageBytes > UINT32_MAX)
    return setError(error, "runtime generated virtual-geometry encoded page byte count overflows");
  if (static_cast<uint64_t>(reserve.encodedPageBytes) > pagesBufferSize)
    return setError(error, "runtime generated virtual-geometry encoded page exceeds scene page storage");

  RuntimePageStatus &status = record.pages[reserve.localPageIndex];
  if (status.installed)
  {
    if (reserve.pageGeneration < status.pageGeneration)
      return setError(error, "runtime generated virtual-geometry page reservation is stale");
    if (reserve.pageGeneration == status.pageGeneration &&
        reserve.geometryRevision != status.geometryRevision)
      return setError(error, "runtime generated virtual-geometry page generation aliases a different geometry revision");
  }

  const auto reservationShortfall = [&]() -> std::string
  {
    const auto describe = [](const char *name, const uint64_t want, const rendering::BufferManager &allocator)
    {
      return std::string(name) + " " + std::to_string(want) + " of " +
          std::to_string(allocator.getTotalSize() - allocator.getAllocatedSize()) +
          " free in " + std::to_string(allocator.getTotalSize());
    };
    if (reserve.encodedPageBytes != 0u &&
        !pagesBufferAllocator.canAllocate(reserve.encodedPageBytes))
      return describe("encoded bytes", reserve.encodedPageBytes, pagesBufferAllocator);
    if (!clusterAllocator.canAllocate(reserve.clusterCount))
      return describe("clusters", reserve.clusterCount, clusterAllocator);
    if (!decodedVertexAllocator.canAllocate(reserve.decodedVertexCount))
      return describe("decoded vertices", reserve.decodedVertexCount, decodedVertexAllocator);
    if (!decodedIndexAllocator.canAllocate(reserve.decodedIndexCount))
      return describe("decoded indices", reserve.decodedIndexCount, decodedIndexAllocator);
    if (reserve.decodedBoneWeightCount != 0u &&
        !decodedBoneWeightAllocator.canAllocate(reserve.decodedBoneWeightCount))
      return describe("bone weights", reserve.decodedBoneWeightCount, decodedBoneWeightAllocator);
    return {};
  }();
  if (!reservationShortfall.empty())
  {
    return setError(
        error,
        "runtime generated virtual-geometry scene buffers cannot fit the page reservation: " +
            reservationShortfall);
  }

  rendering::Allocation pageAlloc{UINT64_MAX, 0u};
  rendering::Allocation clusterAlloc{UINT64_MAX, 0u};
  rendering::Allocation decodedVertexAlloc{UINT64_MAX, 0u};
  rendering::Allocation decodedIndexAlloc{UINT64_MAX, 0u};
  rendering::Allocation decodedBoneWeightAlloc{UINT64_MAX, 0u};

  if (reserve.encodedPageBytes != 0u)
    pageAlloc = pagesBufferAllocator.allocate(reserve.encodedPageBytes);
  clusterAlloc = clusterAllocator.allocate(reserve.clusterCount);
  decodedVertexAlloc = decodedVertexAllocator.allocate(reserve.decodedVertexCount);
  decodedIndexAlloc = decodedIndexAllocator.allocate(reserve.decodedIndexCount);
  if (reserve.decodedBoneWeightCount != 0u)
    decodedBoneWeightAlloc =
        decodedBoneWeightAllocator.allocate(reserve.decodedBoneWeightCount);

  const auto rollback = [&]()
  {
    if (decodedBoneWeightAlloc.size != 0u && decodedBoneWeightAlloc.offset != UINT64_MAX)
      decodedBoneWeightAllocator.deallocate(decodedBoneWeightAlloc);
    if (decodedIndexAlloc.size != 0u && decodedIndexAlloc.offset != UINT64_MAX)
      decodedIndexAllocator.deallocate(decodedIndexAlloc);
    if (decodedVertexAlloc.size != 0u && decodedVertexAlloc.offset != UINT64_MAX)
      decodedVertexAllocator.deallocate(decodedVertexAlloc);
    if (clusterAlloc.size != 0u && clusterAlloc.offset != UINT64_MAX)
      clusterAllocator.deallocate(clusterAlloc);
    if (pageAlloc.size != 0u && pageAlloc.offset != UINT64_MAX)
      pagesBufferAllocator.deallocate(pageAlloc);
  };

  if ((reserve.encodedPageBytes != 0u && pageAlloc.offset == UINT64_MAX) ||
      clusterAlloc.offset == UINT64_MAX ||
      decodedVertexAlloc.offset == UINT64_MAX ||
      decodedIndexAlloc.offset == UINT64_MAX ||
      (reserve.decodedBoneWeightCount != 0u &&
       decodedBoneWeightAlloc.offset == UINT64_MAX))
  {
    rollback();
    return setError(error, "runtime generated virtual-geometry page reservation allocation failed");
  }

  PageAllocation allocation{};
  allocation.pageId = reserve.localPageIndex;
  allocation.bufferOffset =
      reserve.encodedPageBytes != 0u ? static_cast<uint32_t>(pageAlloc.offset) : UINT32_MAX;
  allocation.size = reserve.encodedPageBytes;
  allocation.clusterOffset = static_cast<uint32_t>(clusterAlloc.offset);
  allocation.clusterCount = reserve.clusterCount;
  allocation.unitScaleBits = reserve.unitScaleBits;
  allocation.decodedVertexOffset = static_cast<uint32_t>(decodedVertexAlloc.offset);
  allocation.decodedVertexCount = reserve.decodedVertexCount;
  allocation.decodedIndexOffset = static_cast<uint32_t>(decodedIndexAlloc.offset);
  allocation.decodedIndexCount = reserve.decodedIndexCount;
  allocation.decodedBoneWeightOffset =
      reserve.decodedBoneWeightCount != 0u
      ? static_cast<uint32_t>(decodedBoneWeightAlloc.offset)
      : 0u;
  allocation.decodedBoneWeightCount = reserve.decodedBoneWeightCount;
  allocation.prioritySlot = record.prioritySlotOffset + reserve.localPageIndex;
  allocation.ownerSparseInstance = record.sceneInstanceId;
  allocation.isInstalled = true;

  PageTableEntry pageTableEntry = makePageTableEntry(allocation);
  const uint32_t globalPageTableIndex =
      record.pageTableOffset + reserve.localPageIndex;
  pageTableEntry._padding0 = globalPageTableIndex;

  reservation.objectName = reserve.objectName;
  reservation.localPageIndex = reserve.localPageIndex;
  reservation.globalPageTableIndex = globalPageTableIndex;
  reservation.parentLocalPageIndex = reserve.parentLocalPageIndex;
  reservation.fallbackLocalPageIndex = reserve.fallbackLocalPageIndex;
  reservation.pageGeneration = reserve.pageGeneration;
  reservation.geometryRevision = reserve.geometryRevision;
  reservation.materialRevision = reserve.materialRevision;
  reservation.pageAllocation = allocation;
  reservation.pageTableEntry = pageTableEntry;
  reservation.overrideHierarchyLeafMetadata =
      reserve.overrideHierarchyLeafMetadata;
  reservation.hierarchyBoundsMin = reserve.hierarchyBoundsMin;
  reservation.hierarchyBoundsMax = reserve.hierarchyBoundsMax;
  reservation.hierarchySelfError = reserve.hierarchySelfError;
  reservation.hierarchyParentError = reserve.hierarchyParentError;
  reservation.hierarchyParentSphereValid = reserve.hierarchyParentSphereValid;
  reservation.hierarchyParentBoundsMin = reserve.hierarchyParentBoundsMin;
  reservation.hierarchyParentBoundsMax = reserve.hierarchyParentBoundsMax;
  reservation.decodedClusterFlags = reserve.decodedClusterFlags;
  reservation.rootPage = reserve.rootPage;
  reservation.hasTracePayload = reserve.hasTracePayload;
  reservation.hasMaterialPayload = reserve.hasMaterialPayload;
  reservation.requireProtectedParentOrFallback =
      reserve.requireProtectedParentOrFallback;
  return true;
}

bool VirtualGeometryScene::installRuntimeGeneratedPage(
    const RuntimeGeneratedPageInstallInfo &install,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  const RuntimeGeneratedPageReservation &reservation = install.reservation;
  if (!reservation.valid())
    return setError(error, "runtime generated virtual-geometry page reservation is invalid");
  if (!install.gpuWroteEncodedPageData &&
      !install.gpuWroteDecodedGeometry)
  {
    return setError(error, "runtime generated virtual-geometry page has no published GPU payload");
  }
  if (install.enqueuePageDecode && !install.gpuWroteEncodedPageData)
    return setError(error, "runtime generated virtual-geometry page decode requires encoded page data");

  const auto objectIt = runtimeObjectByName_.find(reservation.objectName);
  if (objectIt == runtimeObjectByName_.end())
    return setError(error, "runtime generated virtual-geometry object is not registered: " + reservation.objectName);
  RuntimeObjectRecord &record = runtimeObjects_[objectIt->second];
  if (reservation.localPageIndex >= record.pages.size())
    return setError(error, "runtime generated virtual-geometry page index is out of range: " + reservation.objectName);
  const RuntimePageStatus oldStatus = record.pages[reservation.localPageIndex];

  RuntimePageInstallInfo pageInstall{};
  pageInstall.localPageIndex = reservation.localPageIndex;
  pageInstall.parentLocalPageIndex = reservation.parentLocalPageIndex;
  pageInstall.fallbackLocalPageIndex = reservation.fallbackLocalPageIndex;
  pageInstall.pageGeneration = reservation.pageGeneration;
  pageInstall.geometryRevision = reservation.geometryRevision;
  pageInstall.materialRevision = reservation.materialRevision;
  pageInstall.pageAllocation = reservation.pageAllocation;
  pageInstall.pageTableEntry = reservation.pageTableEntry;
  pageInstall.rootPage = reservation.rootPage;
  pageInstall.hasTracePayload = reservation.hasTracePayload;
  pageInstall.hasMaterialPayload = reservation.hasMaterialPayload;
  pageInstall.requireProtectedParentOrFallback =
      reservation.requireProtectedParentOrFallback;
  pageInstall.writesSceneEncodedPageData = install.gpuWroteEncodedPageData;
  pageInstall.enqueuePageDecode =
      install.enqueuePageDecode && install.gpuWroteEncodedPageData;
  pageInstall.ownsSceneBufferAllocation = true;
  pageInstall.overrideHierarchyLeafMetadata =
      reservation.overrideHierarchyLeafMetadata;
  pageInstall.hierarchyBoundsMin = reservation.hierarchyBoundsMin;
  pageInstall.hierarchyBoundsMax = reservation.hierarchyBoundsMax;
  pageInstall.hierarchySelfError = reservation.hierarchySelfError;
  pageInstall.hierarchyParentError = reservation.hierarchyParentError;
  pageInstall.hierarchyParentSphereValid = reservation.hierarchyParentSphereValid;
  pageInstall.hierarchyParentBoundsMin = reservation.hierarchyParentBoundsMin;
  pageInstall.hierarchyParentBoundsMax = reservation.hierarchyParentBoundsMax;
  pageInstall.decodedClusterFlags = reservation.decodedClusterFlags;

  if (!installRuntimeObjectPage(reservation.objectName, pageInstall, error))
    return false;

  const auto sameAllocation = [](const PageAllocation &a, const PageAllocation &b)
  {
    return a.bufferOffset == b.bufferOffset &&
        a.size == b.size &&
        a.clusterOffset == b.clusterOffset &&
        a.clusterCount == b.clusterCount &&
        a.decodedVertexOffset == b.decodedVertexOffset &&
        a.decodedVertexCount == b.decodedVertexCount &&
        a.decodedIndexOffset == b.decodedIndexOffset &&
        a.decodedIndexCount == b.decodedIndexCount &&
        a.decodedBoneWeightOffset == b.decodedBoneWeightOffset &&
        a.decodedBoneWeightCount == b.decodedBoneWeightCount;
  };
  if (oldStatus.installed &&
      oldStatus.ownsSceneBufferAllocation &&
      !sameAllocation(oldStatus.pageAllocation, reservation.pageAllocation))
  {
    retireRuntimePageAllocation(
        oldStatus.pageAllocation, oldStatus.globalPageTableIndex,
        oldStatus.prioritySlot, /*clearPageTableEntry=*/false);
  }
  return true;
}

bool VirtualGeometryScene::cancelRuntimeGeneratedPageReservation(
    const RuntimeGeneratedPageReservation &reservation,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!reservation.valid())
    return setError(error, "runtime generated virtual-geometry page reservation is invalid");
  releaseRuntimePageAllocation(reservation.pageAllocation);
  return true;
}

namespace
{
void setHierarchyParentSphere(
    VirtualGeometryHierarchy &node,
    const std::array<float, 3u> &boundsMin,
    const std::array<float, 3u> &boundsMax)
{
  node.max_center_x = (boundsMin[0] + boundsMax[0]) * 0.5f;
  node.max_center_y = (boundsMin[1] + boundsMax[1]) * 0.5f;
  node.max_center_z = (boundsMin[2] + boundsMax[2]) * 0.5f;
  const float dx = boundsMax[0] - boundsMin[0];
  const float dy = boundsMax[1] - boundsMin[1];
  const float dz = boundsMax[2] - boundsMin[2];
  node.max_radius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;
}
} // namespace

namespace
{
// Rewrites one parked leaf in place; the caller owns the upload.
bool writeRuntimePageLeafNode(
    VirtualGeometryHierarchy &node,
    const uint32_t localPageIndex,
    const VirtualGeometryScene::RuntimePageLeafInfo &leaf)
{
  node.pageIndex = PAGE_NOT_INSTALLED_BIT | localPageIndex;
  node.min_x = leaf.boundsMin[0];
  node.min_y = leaf.boundsMin[1];
  node.min_z = leaf.boundsMin[2];
  node.max_x = leaf.boundsMax[0];
  node.max_y = leaf.boundsMax[1];
  node.max_z = leaf.boundsMax[2];
  setHierarchyParentSphere(node, leaf.parentSphereBoundsMin, leaf.parentSphereBoundsMax);
  node.min_lod_error = std::max(leaf.selfError, 0.0f);
  // A parked leaf projects zero parent error: the hierarchy stage rejects it
  // before it can request or enqueue anything. A root page projects an
  // infinite one: always traversed.
  node.max_parent_lod_error = !leaf.requestable ? 0.0f
      : leaf.rootPage ? std::numeric_limits<float>::max()
      : std::max(leaf.parentError, node.min_lod_error);
  node.child_start = 0u;
  node.child_count = 0u;
  node.flags = HIERARCHY_LEAF_FLAG |
      (leaf.decodedClusterFlags ? HIERARCHY_DECODED_CLUSTER_FLAGS_FLAG : 0u);
  return true;
}
} // namespace

bool VirtualGeometryScene::updateRuntimeObjectPageLeaf(
    const std::string &objectName,
    const uint32_t localPageIndex,
    const RuntimePageLeafInfo &leaf,
    std::string *error)
{
  return updateRuntimeObjectPageLeaves(objectName, {{localPageIndex, leaf}}, error);
}

bool VirtualGeometryScene::updateRuntimeObjectPageLeaves(
    const std::string &objectName,
    const std::vector<std::pair<uint32_t, RuntimePageLeafInfo>> &leaves,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (leaves.empty())
    return true;
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end())
    return setError(error, "runtime virtual-geometry object is not registered: " + objectName);
  RuntimeObjectRecord &record = runtimeObjects_[objectIt->second];
  // One pass over the hierarchy for the whole batch: the object's leaves are
  // keyed by page, so a map from page to node makes each update a lookup.
  std::unordered_map<uint32_t, uint32_t> nodeByPage;
  nodeByPage.reserve(record.hierarchy.size());
  for (uint32_t nodeIndex = 0u;
       nodeIndex < static_cast<uint32_t>(record.hierarchy.size());
       ++nodeIndex)
  {
    const VirtualGeometryHierarchy &node = record.hierarchy[nodeIndex];
    if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
      continue;
    nodeByPage.emplace(node.pageIndex & ~PAGE_NOT_INSTALLED_BIT, nodeIndex);
  }
  uint64_t firstNode = record.hierarchy.size();
  uint64_t lastNode = 0u;
  for (const auto &[localPageIndex, leaf] : leaves)
  {
    if (localPageIndex >= record.pageCount)
      return setError(error, "runtime virtual-geometry page index is out of range: " + objectName);
    if (record.pages[localPageIndex].installed)
      return setError(error, "runtime virtual-geometry page leaf update needs a page that is not installed");
    const auto found = nodeByPage.find(localPageIndex);
    if (found == nodeByPage.end())
      return setError(error, "runtime virtual-geometry page has no hierarchy leaf: " + objectName);
    writeRuntimePageLeafNode(record.hierarchy[found->second], localPageIndex, leaf);
    firstNode = std::min<uint64_t>(firstNode, found->second);
    lastNode = std::max<uint64_t>(lastNode, found->second);
  }
  // One upload over the touched span. Publishing a directory's worth of
  // leaves one write at a time cost seconds of scene update on a commit that
  // assigned hundreds of slots.
  writeSceneBuffer(
      hierarchyBuffer,
      record.hierarchyAllocation.offset +
          firstNode * sizeof(VirtualGeometryHierarchy),
      (lastNode - firstNode + 1u) * sizeof(VirtualGeometryHierarchy),
      record.hierarchy.data() + firstNode);
  return true;
}

bool VirtualGeometryScene::installRuntimeObjectPage(
    const std::string &objectName,
    const RuntimePageInstallInfo &install,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end())
    return setError(error, "runtime virtual-geometry object is not registered: " + objectName);
  RuntimeObjectRecord &record = runtimeObjects_[objectIt->second];
  if (install.localPageIndex >= record.pageCount)
    return setError(error, "runtime virtual-geometry page index is out of range: " + objectName);
  if (install.pageGeneration == 0u || install.geometryRevision == 0u)
    return setError(error, "runtime virtual-geometry page generation/revision is invalid");
  if (install.pageAllocation.pageId != install.localPageIndex)
    return setError(error, "runtime virtual-geometry page allocation uses the wrong local page id");
  const uint32_t expectedPrioritySlot =
      record.prioritySlotOffset + install.localPageIndex;
  if (install.pageAllocation.prioritySlot != expectedPrioritySlot ||
      install.pageTableEntry.prioritySlot != expectedPrioritySlot)
    return setError(error, "runtime virtual-geometry page priority slot does not match the registered object");
  if (install.pageTableEntry.isInstalled == 0u ||
      !install.pageAllocation.isInstalled)
    return setError(error, "runtime virtual-geometry page install must be marked resident");
  if (install.requireProtectedParentOrFallback && !install.rootPage)
  {
    const auto protectedInstalled = [&](const uint32_t localPageIndex)
    {
      return localPageIndex < record.pages.size() &&
          record.pages[localPageIndex].installed;
    };
    if (!protectedInstalled(install.parentLocalPageIndex) &&
        !protectedInstalled(install.fallbackLocalPageIndex))
    {
      return setError(error, "runtime virtual-geometry page requires an installed parent or fallback page");
    }
  }

  RuntimePageStatus &status = record.pages[install.localPageIndex];
  const bool replacingInstalledPage = status.installed;
  if (status.installed)
  {
    if (install.pageGeneration < status.pageGeneration)
      return setError(error, "runtime virtual-geometry page install is stale");
    if (install.pageGeneration == status.pageGeneration &&
        install.geometryRevision != status.geometryRevision)
      return setError(error, "runtime virtual-geometry page generation aliases a different geometry revision");
  }

  const uint64_t pageBytes = static_cast<uint64_t>(install.pageTableEntry.size);
  const uint64_t pageEnd =
      static_cast<uint64_t>(install.pageTableEntry.bufferOffset) + pageBytes;
  const bool hasProvidedEncodedPageWords = !install.encodedPageWords.empty();
  if (hasProvidedEncodedPageWords)
  {
    const uint64_t encodedBytes =
        static_cast<uint64_t>(install.encodedPageWords.size()) *
        sizeof(uint32_t);
    if (encodedBytes > pageBytes)
      return setError(error, "runtime virtual-geometry encoded page data exceeds the page table entry size");
    if (pageEnd > pagesBufferSize)
      return setError(error, "runtime virtual-geometry encoded page data exceeds the scene pages buffer");
    writeSceneBuffer(
        pagesBuffer,
        install.pageTableEntry.bufferOffset,
        encodedBytes,
        const_cast<uint32_t *>(install.encodedPageWords.data()));
  }
  else if (install.writesSceneEncodedPageData && pageEnd > pagesBufferSize)
  {
    return setError(error, "runtime virtual-geometry pre-written page data exceeds the scene pages buffer");
  }

  const uint32_t globalPageTableIndex =
      record.pageTableOffset + install.localPageIndex;
  PageTableEntry pageTableEntry = install.pageTableEntry;
  pageTableEntry._padding0 = globalPageTableIndex;
  writeSceneBuffer(
      pageTableBuffer,
      static_cast<uint64_t>(globalPageTableIndex) * sizeof(PageTableEntry),
      sizeof(PageTableEntry),
      &pageTableEntry);

  status.installed = true;
  status.rootPage = install.rootPage;
  status.hasTracePayload = install.hasTracePayload;
  status.hasMaterialPayload = install.hasMaterialPayload;
  status.sceneEncodedPageDataReady =
      install.writesSceneEncodedPageData || hasProvidedEncodedPageWords;
  status.ownsSceneBufferAllocation = install.ownsSceneBufferAllocation;
  status.localPageIndex = install.localPageIndex;
  status.globalPageTableIndex = globalPageTableIndex;
  status.prioritySlot = expectedPrioritySlot;
  status.parentLocalPageIndex = install.parentLocalPageIndex;
  status.fallbackLocalPageIndex = install.fallbackLocalPageIndex;
  status.pageGeneration = install.pageGeneration;
  status.geometryRevision = install.geometryRevision;
  status.materialRevision = install.materialRevision;
  status.pageAllocation = install.pageAllocation;
  status.pageTableEntry = pageTableEntry;
  // A freshly installed page is the most recently used one by definition, so
  // LRU eviction can never immediately retire what an install just made room for.
  status.lastUsedFrame = runtimeStreamingFrame_;
  status.installUpdates = install.hierarchyClusterFlagUpdates;
  status.hierarchyScanRanges = install.hierarchyScanRanges;
  // Reverse dependency counts: release what a previous install of this slot
  // held, then claim for the new one.
  // A page's own index can appear in its dependency list (the builder inserts
  // the page itself); a self-dependency must not count, or the page could
  // never be uninstalled. The walk this replaced skipped self explicitly.
  // Priming and eviction retain the dependency list for a later read, but
  // only installed pages own references. Releasing an uninstalled page's
  // list steals references from other dependents and permits their fallback
  // to be evicted while they are still resident.
  if (replacingInstalledPage)
    for (const uint32_t dependency : status.dependencies)
      if (dependency < record.pages.size() && dependency != install.localPageIndex &&
          record.pages[dependency].dependedOnBy != 0u)
        --record.pages[dependency].dependedOnBy;
  status.dependencies = install.dependencies;
  for (const uint32_t dependency : status.dependencies)
    if (dependency < record.pages.size() && dependency != install.localPageIndex)
      ++record.pages[dependency].dependedOnBy;
  record.latestGeometryRevision =
      std::max(record.latestGeometryRevision, install.geometryRevision);

  // Ranges to search for this page's leaf: the caller's hint, or everything.
  std::vector<RuntimePageInstallInfo::HierarchyScanRange> scanRanges =
      install.hierarchyScanRanges;
  if (scanRanges.empty())
    scanRanges.push_back({0u, static_cast<uint32_t>(record.hierarchy.size())});
  for (const auto &range : scanRanges)
  for (uint32_t nodeIndex = range.firstNode;
       nodeIndex < static_cast<uint32_t>(record.hierarchy.size()) &&
       nodeIndex < range.firstNode + range.nodeCount;
       ++nodeIndex)
  {
    VirtualGeometryHierarchy &node = record.hierarchy[nodeIndex];
    if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u ||
        node.pageIndex == UINT32_MAX)
    {
      continue;
    }
    const uint32_t rawPageIndex = node.pageIndex & ~PAGE_NOT_INSTALLED_BIT;
    if (rawPageIndex != install.localPageIndex)
      continue;

    node.pageIndex = install.localPageIndex;
    if (install.overrideHierarchyLeafMetadata)
    {
      node.min_x = install.hierarchyBoundsMin[0];
      node.min_y = install.hierarchyBoundsMin[1];
      node.min_z = install.hierarchyBoundsMin[2];
      node.max_x = install.hierarchyBoundsMax[0];
      node.max_y = install.hierarchyBoundsMax[1];
      node.max_z = install.hierarchyBoundsMax[2];
      node.max_center_x = (node.min_x + node.max_x) * 0.5f;
      node.max_center_y = (node.min_y + node.max_y) * 0.5f;
      node.max_center_z = (node.min_z + node.max_z) * 0.5f;
      const float dx = node.max_x - node.min_x;
      const float dy = node.max_y - node.min_y;
      const float dz = node.max_z - node.min_z;
      node.max_radius =
          std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;
      node.min_lod_error = std::max(install.hierarchySelfError, 0.0f);
      // A root page has nothing coarser to fall back to: its leaf must pass
      // the LOD test at every distance, as its clusters' parent test does.
      node.max_parent_lod_error = install.rootPage
          ? std::numeric_limits<float>::max()
          : std::max(install.hierarchyParentError, node.min_lod_error);
      if (install.hierarchyParentSphereValid)
        setHierarchyParentSphere(
            node, install.hierarchyParentBoundsMin, install.hierarchyParentBoundsMax);
    }
    if (install.decodedClusterFlags)
      node.flags |= HIERARCHY_DECODED_CLUSTER_FLAGS_FLAG;
    else
      node.flags &= ~HIERARCHY_DECODED_CLUSTER_FLAGS_FLAG;
    if (node.child_start == UINT32_MAX)
      node.child_start = 0u;
    // A page owns many hierarchy leaves - one per group, at most eight clusters
    // each - and each leaf's child_start/child_count already name its own slice
    // of the page. Writing the *page's* whole cluster count into every one of
    // them makes each leaf claim the entire page starting at its own offset:
    // the ranges overlap, so the culler emits the same cluster several times,
    // and the last ones run off the end of the page and emit clusters that do
    // not exist. Measured on a 7-page object: 300 visible-cluster entries for
    // 148 distinct slots, of which 104 were past the end of their page.
    //
    // Only a caller that owns the node layout may set it, and that is exactly
    // what overrideHierarchyLeafMetadata means - the generated-page path, where
    // one node covers one page and the counts arrive with the install.
    if (install.overrideHierarchyLeafMetadata)
    {
      const uint32_t advertisedClusterCount =
          install.pageTableEntry.clusterCount != 0u
          ? install.pageTableEntry.clusterCount
          : install.pageAllocation.clusterCount;
      if (advertisedClusterCount != 0u)
        node.child_count = advertisedClusterCount;
    }
    const uint8_t enabledMask = fullEnabledMaskForNode(node);
    // The streaming-leaf mark forces a cluster's self error to zero in the
    // culler and skips its parent test, so a cluster carrying it draws at every
    // distance. Marking the whole page that way removes the LOD cut outright:
    // every level draws simultaneously. It is only correct where the finer
    // geometry a cluster stands in for is absent, which is what the page's own
    // rewrites describe. Callers that supply them get a real cut; callers that
    // do not keep the previous always-resident behaviour.
    // A leaf whose clusters carry their own flags (generated terrain) gets no
    // mask: the cull data decides per cluster.
    const uint8_t streamingMask =
        install.decodedClusterFlags ? 0u
        : install.hierarchyClusterFlagUpdates.empty() ? enabledMask : 0u;
    setNodeClusterMasks(node, streamingMask, enabledMask);
    writeSceneBuffer(
        hierarchyBuffer,
        record.hierarchyAllocation.offset +
            static_cast<uint64_t>(nodeIndex) * sizeof(VirtualGeometryHierarchy),
        sizeof(VirtualGeometryHierarchy),
        &node);
  }

  // Apply this page's rewrites last: they may name coarser nodes owned by other
  // pages, disabling the clusters this page's finer geometry replaces and
  // marking the new residency border.
  for (const auto &update : install.hierarchyClusterFlagUpdates)
  {
    if (update.hierarchyNodeIndex >= record.hierarchy.size())
      continue;
    VirtualGeometryHierarchy &node = record.hierarchy[update.hierarchyNodeIndex];
    if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u)
      continue;
    setNodeClusterMasks(
        node, update.streamingLeafsBitset, update.enabledClustersBitset);
    writeSceneBuffer(
        hierarchyBuffer,
        record.hierarchyAllocation.offset +
            static_cast<uint64_t>(update.hierarchyNodeIndex) *
                sizeof(VirtualGeometryHierarchy),
        sizeof(VirtualGeometryHierarchy),
        &node);
  }

  if (install.enqueuePageDecode && status.sceneEncodedPageDataReady)
  {
    enqueuePageDecodeRequest(globalPageTableIndex);
    ++record.queuedDecodeRequestCount;
  }
  return true;
}

bool VirtualGeometryScene::writeRuntimeObjectHierarchyRange(
    const std::string &objectName,
    const uint32_t firstNode,
    const std::vector<VirtualGeometryHierarchy> &nodes,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end())
    return setError(error, "runtime virtual-geometry object is not registered: " + objectName);
  RuntimeObjectRecord &record = runtimeObjects_[objectIt->second];
  if (nodes.empty())
    return true;
  if (static_cast<uint64_t>(firstNode) + nodes.size() > record.hierarchy.size())
    return setError(
        error,
        "runtime virtual-geometry hierarchy range exceeds the object's allocation: " +
            objectName + " [" + std::to_string(firstNode) + "," +
            std::to_string(firstNode + nodes.size()) + ") of " +
            std::to_string(record.hierarchy.size()));
  std::copy(nodes.begin(), nodes.end(), record.hierarchy.begin() + firstNode);
  writeSceneBuffer(
      hierarchyBuffer,
      record.hierarchyAllocation.offset +
          static_cast<uint64_t>(firstNode) * sizeof(VirtualGeometryHierarchy),
      static_cast<uint64_t>(nodes.size()) * sizeof(VirtualGeometryHierarchy),
      const_cast<VirtualGeometryHierarchy *>(nodes.data()));
  return true;
}

bool VirtualGeometryScene::setRuntimeObjectHierarchyParentError(
    const std::string &objectName, const uint32_t nodeIndex,
    const float parentError, std::string *error)
{
  if (error != nullptr)
    error->clear();
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end())
    return setError(error, "runtime virtual-geometry object is not registered: " + objectName);
  auto &record = runtimeObjects_[objectIt->second];
  if (nodeIndex >= record.hierarchy.size())
    return setError(error, "runtime virtual-geometry hierarchy node is out of range");
  auto &node = record.hierarchy[nodeIndex];
  if (node.max_parent_lod_error == parentError)
    return true;
  node.max_parent_lod_error = parentError;
  writeSceneBuffer(
      hierarchyBuffer,
      record.hierarchyAllocation.offset +
          static_cast<uint64_t>(nodeIndex) * sizeof(VirtualGeometryHierarchy),
      sizeof(VirtualGeometryHierarchy), &node);
  return true;
}

bool VirtualGeometryScene::applyRuntimeObjectHierarchyClusterFlags(
    const std::string &objectName,
    const std::vector<HierarchyClusterFlagsUpdate> &updates,
    std::string *error,
    const bool persistIntoPageUpdates)
{
  if (error != nullptr)
    error->clear();
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end())
    return setError(error, "runtime virtual-geometry object is not registered: " + objectName);
  RuntimeObjectRecord &record = runtimeObjects_[objectIt->second];
  // Patch the mirror, then upload the span the updates touched in one write.
  // A chunk's rewrites are tens to hundreds of nodes inside one slice; one
  // write per node was thousands of tiny transfers per apply.
  uint32_t firstNode = UINT32_MAX;
  uint32_t lastNode = 0u;
  for (const auto &update : updates)
  {
    if (update.hierarchyNodeIndex >= record.hierarchy.size())
      continue;
    VirtualGeometryHierarchy &node = record.hierarchy[update.hierarchyNodeIndex];
    if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u)
      continue;
    setNodeClusterMasks(
        node, update.streamingLeafsBitset, update.enabledClustersBitset);
    firstNode = std::min(firstNode, update.hierarchyNodeIndex);
    lastNode = std::max(lastNode, update.hierarchyNodeIndex);
    // Record the mask against the page that owns the node, or it is not
    // durable: `rebuildRuntimeObjectHierarchyMasks` reconstructs the whole
    // object from `pages[].installUpdates` alone, so a mask written only into
    // the mirror is discarded the next time any page in the object uninstalls.
    // An asset like Sponza never notices, because every mask it has comes from
    // a page's own install rewrites; a caller maintaining a cross-page relation
    // by hand has to enter it the same way.
    if (!persistIntoPageUpdates)
      continue;
    const uint32_t owningPage = node.pageIndex & ~PAGE_NOT_INSTALLED_BIT;
    if (owningPage >= record.pages.size())
      continue;
    auto &pageUpdates = record.pages[owningPage].externalUpdates;
    const auto existing = std::find_if(
        pageUpdates.begin(), pageUpdates.end(),
        [&](const HierarchyClusterFlagsUpdate &entry)
        { return entry.hierarchyNodeIndex == update.hierarchyNodeIndex; });
    if (existing != pageUpdates.end())
      *existing = update;
    else
      pageUpdates.push_back(update);
  }
  if (firstNode != UINT32_MAX)
    writeSceneBuffer(
        hierarchyBuffer,
        record.hierarchyAllocation.offset +
            static_cast<uint64_t>(firstNode) * sizeof(VirtualGeometryHierarchy),
        static_cast<uint64_t>(lastNode - firstNode + 1u) * sizeof(VirtualGeometryHierarchy),
        record.hierarchy.data() + firstNode);
  return true;
}

std::vector<uint32_t> VirtualGeometryScene::debugRuntimePageDependers(
    const std::string &objectName, const uint32_t localPageIndex) const
{
  std::vector<uint32_t> dependers;
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end())
    return dependers;
  for (const auto &status : runtimeObjects_[objectIt->second].pages)
  {
    if (!status.installed || status.localPageIndex == localPageIndex)
      continue;
    for (const uint32_t dependency : status.dependencies)
      if (dependency == localPageIndex)
      {
        dependers.push_back(status.localPageIndex);
        break;
      }
  }
  return dependers;
}

uint32_t VirtualGeometryScene::debugRuntimePageDependedOnBy(
    const std::string &objectName, const uint32_t localPageIndex) const
{
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end())
    return 0u;
  const auto &pages = runtimeObjects_[objectIt->second].pages;
  return localPageIndex < pages.size() ? pages[localPageIndex].dependedOnBy : 0u;
}

bool VirtualGeometryScene::debugReadRuntimeObjectHierarchy(
    const std::string &objectName,
    std::vector<VirtualGeometryHierarchy> &out) const
{
  out.clear();
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end())
    return false;
  out = runtimeObjects_[objectIt->second].hierarchy;
  return true;
}

uint32_t VirtualGeometryScene::runtimeObjectHierarchyNodeCount(
    const std::string &objectName) const
{
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end())
    return 0u;
  return static_cast<uint32_t>(runtimeObjects_[objectIt->second].hierarchy.size());
}

bool VirtualGeometryScene::installRuntimeObjectEncodedPage(
    const std::string &objectName,
    const RuntimeEncodedPageInstallInfo &install,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end())
    return setError(error, "runtime virtual-geometry object is not registered: " + objectName);
  RuntimeObjectRecord &record = runtimeObjects_[objectIt->second];
  if (install.localPageIndex >= record.pageCount)
    return setError(error, "runtime virtual-geometry encoded page index is out of range: " + objectName);
  if (install.pageGeneration == 0u || install.geometryRevision == 0u)
    return setError(error, "runtime virtual-geometry encoded page generation/revision is invalid");
  if (install.encodedPageWords.empty() && !install.location.valid())
    return setError(error, "runtime virtual-geometry encoded page has neither data nor a page location");

  RuntimePageStatus &existingStatus = record.pages[install.localPageIndex];
  if (existingStatus.installed)
  {
    if (install.pageGeneration < existingStatus.pageGeneration)
      return setError(error, "runtime virtual-geometry encoded page install is stale");
    if (install.pageGeneration == existingStatus.pageGeneration &&
        install.geometryRevision != existingStatus.geometryRevision)
      return setError(error, "runtime virtual-geometry encoded page generation aliases a different geometry revision");
  }

  if (install.requireProtectedParentOrFallback && !install.rootPage)
  {
    const auto protectedInstalled = [&](const uint32_t localPageIndex)
    {
      return localPageIndex < record.pages.size() &&
          record.pages[localPageIndex].installed;
    };
    if (!protectedInstalled(install.parentLocalPageIndex) &&
        !protectedInstalled(install.fallbackLocalPageIndex))
    {
      return setError(error, "runtime virtual-geometry encoded page requires an installed parent or fallback page");
    }
  }

  // Phase 8g: no words given means the page is a location, and the scene
  // reads it. Reading after the generation and protection checks, so a
  // rejected install costs no read.
  std::vector<uint32_t> sourceWords;
  if (install.encodedPageWords.empty())
  {
    const auto readStart = std::chrono::steady_clock::now();
    std::string readError;
    const bool read = install.location.source->readEncodedPage(
        install.location, sourceWords, &readError);
    runtimePageSourceStats_.readMilliseconds +=
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - readStart)
            .count();
    if (!read || sourceWords.empty())
    {
      ++runtimePageSourceStats_.readFailures;
      return setError(
          error,
          "runtime virtual-geometry page source read failed: " +
              (readError.empty() ? std::string("empty page") : readError));
    }
    ++runtimePageSourceStats_.reads;
  }
  const std::vector<uint32_t> &words =
      install.encodedPageWords.empty() ? sourceWords : install.encodedPageWords;

  const uint64_t pageBytes =
      static_cast<uint64_t>(words.size()) * sizeof(uint32_t);
  if (pageBytes > pagesBufferSize || pageBytes > UINT32_MAX)
    return setError(error, "runtime virtual-geometry encoded page data exceeds the scene pages buffer");

  const uint32_t meshletCount = words[0];
  if (meshletCount == 0u)
    return setError(error, "runtime virtual-geometry encoded page has no meshlets");
  if (meshletCount > MAX_CLUSTERS_PER_PAGE)
    return setError(error, "runtime virtual-geometry encoded page exceeds the decoded page cluster capacity");
  PageDecodedGeometryBudget decodedGeometryBudget =
      computeDecodedGeometryBudget(words.data(), words.size());
  if (decodedGeometryBudget.vertexCount == 0u ||
      decodedGeometryBudget.indexCount == 0u)
  {
    return setError(error, "runtime virtual-geometry encoded page has no drawable decoded geometry");
  }

  if (settings.compressedGeometry) decodedGeometryBudget = {};

  // The page buffer is the one that fills up in steady state, because terrain
  // installs new pages continuously. Make room by retiring the pages nothing
  // has wanted for the longest, rather than failing the install.
  if (!pagesBufferAllocator.canAllocate(pageBytes))
    evictLeastRecentlyUsedRuntimePages(pageBytes);
  if (!pagesBufferAllocator.canAllocate(pageBytes) ||
      !clusterAllocator.canAllocate(meshletCount) ||
      (decodedGeometryBudget.vertexCount != 0u && !decodedVertexAllocator.canAllocate(decodedGeometryBudget.vertexCount)) ||
      (decodedGeometryBudget.indexCount != 0u && !decodedIndexAllocator.canAllocate(decodedGeometryBudget.indexCount)) ||
      (decodedGeometryBudget.boneWeightCount != 0u &&
       !decodedBoneWeightAllocator.canAllocate(decodedGeometryBudget.boneWeightCount)))
  {
    return setError(
        error,
        "runtime virtual-geometry scene buffers cannot fit the encoded page" +
            (deferredPagesBufferBytes_ != 0u
                 ? " (" + std::to_string(deferredPagesBufferBytes_) +
                       " bytes await deferred release; retry next frame)"
                 : std::string()));
  }

  rendering::Allocation pageAlloc = pagesBufferAllocator.allocate(pageBytes);
  rendering::Allocation clusterAlloc = clusterAllocator.allocate(meshletCount);
  rendering::Allocation decodedVertexAlloc =
      decodedGeometryBudget.vertexCount ? decodedVertexAllocator.allocate(decodedGeometryBudget.vertexCount) : rendering::Allocation{0u, 0u};
  rendering::Allocation decodedIndexAlloc =
      decodedGeometryBudget.indexCount ? decodedIndexAllocator.allocate(decodedGeometryBudget.indexCount) : rendering::Allocation{0u, 0u};
  rendering::Allocation decodedBoneWeightAlloc{0u, 0u};
  if (decodedGeometryBudget.boneWeightCount != 0u)
    decodedBoneWeightAlloc =
        decodedBoneWeightAllocator.allocate(decodedGeometryBudget.boneWeightCount);

  const auto rollbackNewAllocations = [&]()
  {
    if (decodedBoneWeightAlloc.size != 0u && decodedBoneWeightAlloc.offset != UINT64_MAX)
      decodedBoneWeightAllocator.deallocate(decodedBoneWeightAlloc);
    if (decodedIndexAlloc.size != 0u && decodedIndexAlloc.offset != UINT64_MAX)
      decodedIndexAllocator.deallocate(decodedIndexAlloc);
    if (decodedVertexAlloc.size != 0u && decodedVertexAlloc.offset != UINT64_MAX)
      decodedVertexAllocator.deallocate(decodedVertexAlloc);
    if (clusterAlloc.size != 0u && clusterAlloc.offset != UINT64_MAX)
      clusterAllocator.deallocate(clusterAlloc);
    if (pageAlloc.size != 0u && pageAlloc.offset != UINT64_MAX)
      pagesBufferAllocator.deallocate(pageAlloc);
  };

  if (pageAlloc.offset == UINT64_MAX || clusterAlloc.offset == UINT64_MAX ||
      decodedVertexAlloc.offset == UINT64_MAX ||
      decodedIndexAlloc.offset == UINT64_MAX ||
      (decodedGeometryBudget.boneWeightCount != 0u &&
       decodedBoneWeightAlloc.offset == UINT64_MAX))
  {
    rollbackNewAllocations();
    return setError(error, "runtime virtual-geometry scene buffer allocation failed for encoded page");
  }

  const RuntimePageStatus oldStatus = existingStatus;

  RuntimePageInstallInfo runtimeInstall{};
  runtimeInstall.localPageIndex = install.localPageIndex;
  runtimeInstall.parentLocalPageIndex = install.parentLocalPageIndex;
  runtimeInstall.fallbackLocalPageIndex = install.fallbackLocalPageIndex;
  runtimeInstall.pageGeneration = install.pageGeneration;
  runtimeInstall.geometryRevision = install.geometryRevision;
  runtimeInstall.materialRevision = install.materialRevision;
  runtimeInstall.pageAllocation.pageId = install.localPageIndex;
  runtimeInstall.pageAllocation.bufferOffset = static_cast<uint32_t>(pageAlloc.offset);
  runtimeInstall.pageAllocation.size = static_cast<uint32_t>(pageBytes);
  runtimeInstall.pageAllocation.clusterOffset = static_cast<uint32_t>(clusterAlloc.offset);
  runtimeInstall.pageAllocation.clusterCount = meshletCount;
  runtimeInstall.pageAllocation.unitScaleBits = install.unitScaleBits;
  runtimeInstall.pageAllocation.decodedVertexOffset = static_cast<uint32_t>(decodedVertexAlloc.offset);
  runtimeInstall.pageAllocation.decodedVertexCount = decodedGeometryBudget.vertexCount;
  runtimeInstall.pageAllocation.decodedIndexOffset = static_cast<uint32_t>(decodedIndexAlloc.offset);
  runtimeInstall.pageAllocation.decodedIndexCount = decodedGeometryBudget.indexCount;
  runtimeInstall.pageAllocation.decodedBoneWeightOffset = static_cast<uint32_t>(decodedBoneWeightAlloc.offset);
  runtimeInstall.pageAllocation.decodedBoneWeightCount = decodedGeometryBudget.boneWeightCount;
  runtimeInstall.pageAllocation.prioritySlot =
      record.prioritySlotOffset + install.localPageIndex;
  runtimeInstall.pageAllocation.ownerSparseInstance = record.sceneInstanceId;
  runtimeInstall.pageAllocation.isInstalled = true;
  runtimeInstall.pageTableEntry = makePageTableEntry(runtimeInstall.pageAllocation);
  runtimeInstall.rootPage = install.rootPage;
  runtimeInstall.hasTracePayload = install.hasTracePayload;
  runtimeInstall.hasMaterialPayload = install.hasMaterialPayload;
  runtimeInstall.hierarchyClusterFlagUpdates = install.hierarchyClusterFlagUpdates;
  runtimeInstall.hierarchyScanRanges = install.hierarchyScanRanges;
  runtimeInstall.dependencies = install.dependencies;
  runtimeInstall.requireProtectedParentOrFallback =
      install.requireProtectedParentOrFallback;
  runtimeInstall.enqueuePageDecode = install.enqueuePageDecode;
  runtimeInstall.ownsSceneBufferAllocation = true;
  if (install.encodedPageWords.empty())
    runtimeInstall.encodedPageWords = std::move(sourceWords);
  else
    runtimeInstall.encodedPageWords = install.encodedPageWords;

  if (!installRuntimeObjectPage(objectName, runtimeInstall, error))
  {
    rollbackNewAllocations();
    return false;
  }

  // Phase 8g: keep where the words came from, so an eviction of this page is
  // reversible by the scene alone. An install from words with no location has
  // nothing to come back from, and says so.
  {
    RuntimePageStatus &installedStatus = record.pages[install.localPageIndex];
    if (install.location.valid())
      installedStatus.reinstall = makeRuntimePageReinstall(install);
    else
      installedStatus.reinstall = {};
  }

  // The page table entry now names the new allocation; a frame still in
  // flight may be drawing from the old one, so it is retired, not freed.
  if (oldStatus.installed && oldStatus.ownsSceneBufferAllocation)
    retireRuntimePageAllocation(
        oldStatus.pageAllocation, oldStatus.globalPageTableIndex,
        oldStatus.prioritySlot, /*clearPageTableEntry=*/false);
  return true;
}

bool VirtualGeometryScene::uninstallRuntimeObjectPage(
    const std::string &objectName,
    const uint32_t localPageIndex,
    const uint64_t requiredPageGeneration,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end())
    return setError(error, "runtime virtual-geometry object is not registered: " + objectName);
  RuntimeObjectRecord &record = runtimeObjects_[objectIt->second];
  if (localPageIndex >= record.pageCount)
    return setError(error, "runtime virtual-geometry page index is out of range: " + objectName);
  RuntimePageStatus &status = record.pages[localPageIndex];
  if (!status.installed)
    return true;
  if (requiredPageGeneration != 0u &&
      status.pageGeneration != requiredPageGeneration)
    return setError(error, "runtime virtual-geometry page uninstall generation does not match the resident page");
  for (const RuntimePageStatus &candidate : record.pages)
  {
    if (!candidate.installed || candidate.localPageIndex == localPageIndex)
      continue;
    if (candidate.parentLocalPageIndex == localPageIndex ||
        candidate.fallbackLocalPageIndex == localPageIndex)
    {
      return setError(error, "runtime virtual-geometry page still protects an installed child page");
    }
  }
  if (runtimePageIsDependedOn(record, localPageIndex))
    return setError(error, "runtime virtual-geometry page is a dependency of an installed page");
  static const bool traceUninstall = std::getenv("VG_TRACE_UNINSTALL") != nullptr;
  const auto tProtect = std::chrono::steady_clock::now();

  const uint32_t globalPageTableIndex = status.globalPageTableIndex;
  const uint32_t prioritySlot = status.prioritySlot;
  // The GPU-visible page table entry and the page's buffers go together: a
  // frame in flight that still lists this page's clusters keeps drawing them
  // until both are retired, which happens now or after the release delay.
  const auto tRetire = std::chrono::steady_clock::now();
  retireRuntimePageAllocation(
      status.ownsSceneBufferAllocation ? status.pageAllocation : PageAllocation{},
      globalPageTableIndex, prioritySlot, /*clearPageTableEntry=*/true);
  const auto tRetired = std::chrono::steady_clock::now();
  std::vector<HierarchyClusterFlagsUpdate> installUpdates = std::move(status.installUpdates);
  std::vector<uint32_t> dependencies = std::move(status.dependencies);
  RuntimePageStatus::Reinstall reinstall = std::move(status.reinstall);
  const std::vector<RuntimePageInstallInfo::HierarchyScanRange> rebuildScope =
      std::move(status.hierarchyScanRanges);
  for (const uint32_t dependency : dependencies)
    if (dependency < record.pages.size() && dependency != localPageIndex &&
        record.pages[dependency].dependedOnBy != 0u)
      --record.pages[dependency].dependedOnBy;
  const uint32_t dependedOnBy = status.dependedOnBy;
  status = {};
  status.dependedOnBy = dependedOnBy;
  status.localPageIndex = localPageIndex;
  status.globalPageTableIndex = globalPageTableIndex;
  status.prioritySlot = prioritySlot;
  status.pageTableEntry.prioritySlot = prioritySlot;
  status.installUpdates = std::move(installUpdates);
  status.dependencies = std::move(dependencies);
  status.reinstall = std::move(reinstall);

  // New frames must not walk into the retired page: its leaves are marked
  // not installed and the clusters it replaced are enabled again, by replaying
  // the install rewrites of the pages that remain. Without this the hierarchy
  // still pointed at a page the table said was gone, and the culler drew
  // nothing there.
  //
  // A page whose clusters carry their own flags (GPU-generated terrain) has
  // no masks to replay and one leaf to mark; rewriting the object's whole
  // hierarchy for it raced the frame in flight, which read every page as
  // uninstalled for one frame.
  bool decodedFlagLeaf = false;
  // Only where this page's leaf can be: the scope the install recorded, or
  // the whole hierarchy for a one-asset object. On the shared terrain object
  // the unscoped walk was 857 ms per page removed - the whole retire cost.
  std::vector<RuntimePageInstallInfo::HierarchyScanRange> leafScope = rebuildScope;
  if (leafScope.empty())
    leafScope.push_back({0u, static_cast<uint32_t>(record.hierarchy.size())});
  for (const auto &leafRange : leafScope)
  for (uint32_t nodeIndex = leafRange.firstNode;
       nodeIndex < static_cast<uint32_t>(record.hierarchy.size()) &&
       nodeIndex < leafRange.firstNode + leafRange.nodeCount;
       ++nodeIndex)
  {
    VirtualGeometryHierarchy &node = record.hierarchy[nodeIndex];
    if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX ||
        (node.pageIndex & ~PAGE_NOT_INSTALLED_BIT) != localPageIndex ||
        (node.flags & HIERARCHY_DECODED_CLUSTER_FLAGS_FLAG) == 0u)
      continue;
    decodedFlagLeaf = true;
    node.pageIndex = PAGE_NOT_INSTALLED_BIT | localPageIndex;
    node.child_start = 0u;
    node.child_count = 0u;
    writeSceneBuffer(
        hierarchyBuffer,
        record.hierarchyAllocation.offset +
            static_cast<uint64_t>(nodeIndex) * sizeof(VirtualGeometryHierarchy),
        sizeof(VirtualGeometryHierarchy),
        &node);
  }
  const auto tRebuild = std::chrono::steady_clock::now();
  if (!decodedFlagLeaf)
  {
    if (rebuildScope.empty())
      rebuildRuntimeObjectHierarchyMasks(record);
    else
      rebuildRuntimeObjectHierarchyMasksScoped(record, rebuildScope);
  }
  if (traceUninstall)
  {
    const auto ms = [](const auto a, const auto b)
    { return std::chrono::duration<double, std::milli>(b - a).count(); };
    const auto now = std::chrono::steady_clock::now();
    os::Logger::logf(
        "VG uninstall %s page %u: preRetire=%.2fms retire=%.2fms leafWalk=%.2fms rebuild=%.2fms scoped=%d",
        objectName.c_str(), localPageIndex, ms(tProtect, tRetire), ms(tRetire, tRetired),
        ms(tRetired, tRebuild), ms(tRebuild, now), rebuildScope.empty() ? 0 : 1);
  }
  return true;
}

VirtualGeometryScene::RuntimePageStatus::Reinstall
VirtualGeometryScene::makeRuntimePageReinstall(const RuntimeEncodedPageInstallInfo &install)
{
  RuntimePageStatus::Reinstall reinstall{};
  reinstall.location = install.location;
  reinstall.pageGeneration = install.pageGeneration;
  reinstall.geometryRevision = install.geometryRevision;
  reinstall.materialRevision = install.materialRevision;
  reinstall.unitScaleBits = install.unitScaleBits;
  reinstall.parentLocalPageIndex = install.parentLocalPageIndex;
  reinstall.fallbackLocalPageIndex = install.fallbackLocalPageIndex;
  reinstall.rootPage = install.rootPage;
  reinstall.hasTracePayload = install.hasTracePayload;
  reinstall.hasMaterialPayload = install.hasMaterialPayload;
  reinstall.requireProtectedParentOrFallback = install.requireProtectedParentOrFallback;
  reinstall.enqueuePageDecode = install.enqueuePageDecode;
  reinstall.hierarchyScanRanges = install.hierarchyScanRanges;
  return reinstall;
}

bool VirtualGeometryScene::primeRuntimeObjectPageSource(
    const std::string &objectName,
    const RuntimeEncodedPageInstallInfo &install,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end())
    return setError(error, "runtime virtual-geometry object is not registered: " + objectName);
  RuntimeObjectRecord &record = runtimeObjects_[objectIt->second];
  if (install.localPageIndex >= record.pageCount)
    return setError(error, "runtime virtual-geometry page index is out of range: " + objectName);
  if (!install.location.valid())
    return setError(error, "runtime virtual-geometry page prime carries no page location");
  if (install.pageGeneration == 0u || install.geometryRevision == 0u)
    return setError(error, "runtime virtual-geometry page prime generation/revision is invalid");
  RuntimePageStatus &status = record.pages[install.localPageIndex];
  if (status.installed)
    return setError(error, "runtime virtual-geometry page is installed; its install records its location");
  // What an uninstall would have left behind, so the reinstall path reads
  // the same fields whether the page was ever installed or not. Dependency
  // counts belong to installed pages only, exactly as after an uninstall.
  status.installUpdates = install.hierarchyClusterFlagUpdates;
  status.dependencies = install.dependencies;
  status.reinstall = makeRuntimePageReinstall(install);
  return true;
}

bool VirtualGeometryScene::reinstallRuntimePageFromSourceRecursive(
    RuntimeObjectRecord &record,
    const uint32_t localPageIndex,
    const uint32_t depth,
    const uint32_t budget,
    uint32_t &installedCount,
    std::string *error)
{
  if (localPageIndex >= record.pages.size())
    return setError(error, "runtime virtual-geometry page index is out of range: " + record.name);
  if (record.pages[localPageIndex].installed)
    return true;
  if (!record.pages[localPageIndex].reinstall.valid())
    return setError(
        error,
        "runtime virtual-geometry page " + std::to_string(localPageIndex) +
            " has no recorded page location");
  // A dependency chain is at most the page count long; deeper is a cycle in
  // the recorded dependencies, and following it would never end.
  if (depth > record.pages.size())
    return setError(error, "runtime virtual-geometry page dependencies form a cycle");
  if (installedCount >= budget)
    return setError(error, "runtime virtual-geometry page reinstall budget is exhausted");

  // Dependencies first. The builder emits them so a page is installable only
  // once they are resident, and the eviction that took this page may have
  // taken them too. A copy: the installs below rewrite status fields.
  const std::vector<uint32_t> dependencies = record.pages[localPageIndex].dependencies;
  for (const uint32_t dependency : dependencies)
  {
    if (dependency == localPageIndex || dependency >= record.pages.size())
      continue;
    if (record.pages[dependency].installed)
      continue;
    if (!reinstallRuntimePageFromSourceRecursive(
            record, dependency, depth + 1u, budget, installedCount, error))
      return false;
  }

  const RuntimePageStatus &status = record.pages[localPageIndex];
  const RuntimePageStatus::Reinstall &reinstall = status.reinstall;
  RuntimeEncodedPageInstallInfo install{};
  install.localPageIndex = localPageIndex;
  install.parentLocalPageIndex = reinstall.parentLocalPageIndex;
  install.fallbackLocalPageIndex = reinstall.fallbackLocalPageIndex;
  install.pageGeneration = reinstall.pageGeneration;
  install.geometryRevision = reinstall.geometryRevision;
  install.materialRevision = reinstall.materialRevision;
  install.unitScaleBits = reinstall.unitScaleBits;
  install.rootPage = reinstall.rootPage;
  install.hasTracePayload = reinstall.hasTracePayload;
  install.hasMaterialPayload = reinstall.hasMaterialPayload;
  install.requireProtectedParentOrFallback = reinstall.requireProtectedParentOrFallback;
  install.enqueuePageDecode = reinstall.enqueuePageDecode;
  install.location = reinstall.location;
  install.dependencies = status.dependencies;
  install.hierarchyClusterFlagUpdates = status.installUpdates;
  install.hierarchyScanRanges = reinstall.hierarchyScanRanges;
  if (settings.asyncRuntimeUploads)
  {
    bool pending = false;
    if (!install.location.source->tryReadEncodedPage(install.location,
            install.encodedPageWords, pending, error))
    {
      if (pending) return setError(error, "runtime page source read is pending");
      return false;
    }
  }
  // `record.name` by value: the install looks the object up again by name.
  const std::string objectName = record.name;
  if (!installRuntimeObjectEncodedPage(objectName, install, error))
  {
    ++runtimePageSourceStats_.reinstallFailures;
    return false;
  }
  ++runtimePageSourceStats_.reinstalls;
  ++installedCount;
  return true;
}

bool VirtualGeometryScene::reinstallRuntimeObjectPageFromSource(
    const std::string &objectName,
    const uint32_t localPageIndex,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end())
    return setError(error, "runtime virtual-geometry object is not registered: " + objectName);
  RuntimeObjectRecord &record = runtimeObjects_[objectIt->second];
  if (localPageIndex >= record.pageCount)
    return setError(error, "runtime virtual-geometry page index is out of range: " + objectName);
  if (record.pages[localPageIndex].installed)
    return setError(error, "runtime virtual-geometry page is installed already");
  uint32_t installedCount = 0u;
  return reinstallRuntimePageFromSourceRecursive(
      record, localPageIndex, 0u, UINT32_MAX, installedCount, error);
}

uint32_t VirtualGeometryScene::reinstallRequestedRuntimePagesFromSource(
    const std::string &objectName,
    const std::vector<RuntimeObjectPageRequest> &requests,
    const uint32_t budget,
    uint32_t *failures)
{
  if (failures != nullptr)
    *failures = 0u;
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end() || budget == 0u)
    return 0u;
  RuntimeObjectRecord &record = runtimeObjects_[objectIt->second];
  uint32_t installedCount = 0u;
  for (const RuntimeObjectPageRequest &request : requests)
  {
    if (installedCount >= budget)
      break;
    if (request.localPageIndex >= record.pageCount)
      continue;
    const RuntimePageStatus &status = record.pages[request.localPageIndex];
    if (status.installed || !status.reinstall.valid())
      continue;
    std::string error;
    if (reinstallRuntimePageFromSourceRecursive(
            record, request.localPageIndex, 0u, budget, installedCount, &error))
      continue;
    if (error.find("budget is exhausted") != std::string::npos)
      break;
    if (error == "runtime page source read is pending")
      continue;
    if (failures != nullptr)
      ++*failures;
    os::Logger::warningf("VG source page install: object=%s page=%u: %s",
        objectName.c_str(), request.localPageIndex, error.c_str());
    // A pool that cannot take this page cannot take the next one; the
    // caller's headroom pass is what makes room.
    if (error.find("cannot fit") != std::string::npos ||
        error.find("allocation failed") != std::string::npos)
      break;
  }
  return installedCount;
}

void VirtualGeometryScene::forgetRuntimeObjectPageSource(
    const std::string &objectName,
    const uint32_t localPageIndex)
{
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end())
    return;
  RuntimeObjectRecord &record = runtimeObjects_[objectIt->second];
  if (localPageIndex < record.pages.size())
    record.pages[localPageIndex].reinstall = {};
}

void VirtualGeometryScene::forgetRuntimePageSource(const VirtualGeometryPageSource *source)
{
  if (source == nullptr)
    return;
  for (RuntimeObjectRecord &record : runtimeObjects_)
  {
    if (!record.registered)
      continue;
    for (RuntimePageStatus &status : record.pages)
      if (status.reinstall.location.source == source)
        status.reinstall = {};
  }
}

uint32_t VirtualGeometryScene::countReinstallableRuntimePages(const std::string &objectName) const
{
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end())
    return 0u;
  const RuntimeObjectRecord &record = runtimeObjects_[objectIt->second];
  uint32_t count = 0u;
  for (const RuntimePageStatus &status : record.pages)
    if (!status.installed && status.reinstall.valid())
      ++count;
  return count;
}

bool VirtualGeometryScene::unregisterRuntimeObject(
    const std::string &objectName,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end())
    return setError(error, "runtime virtual-geometry object is not registered: " + objectName);
  RuntimeObjectRecord &record = runtimeObjects_[objectIt->second];

  // Everything the GPU can still be reading - page table entries, page and
  // decoded-cluster ranges, the hierarchy range, the slot ranges - is gathered
  // into one release and applied now or after the release delay. Only the
  // instance disappears immediately, and it does so per frame slot, so a
  // frame in flight keeps a consistent view of the object it was given.
  DeferredRuntimeRelease release{};
  release.releaseFrame = runtimeStreamingFrame_ + runtimeReleaseDelayFrames_;
  for (uint32_t offset = record.pageCount; offset != 0u; --offset)
  {
    const uint32_t localPageIndex = offset - 1u;
    RuntimePageStatus &status = record.pages[localPageIndex];
    if (!status.installed)
      continue;
    release.pageTableEntriesToClear.emplace_back(
        status.globalPageTableIndex, status.prioritySlot);
    if (status.ownsSceneBufferAllocation)
      release.pageAllocations.push_back(status.pageAllocation);
    status = {};
    status.localPageIndex = localPageIndex;
  }
  release.hierarchyAllocation = record.hierarchyAllocation;
  release.pageTableSlots = record.pageTableSlots;
  release.prioritySlots = record.prioritySlots;
  if (deferringRuntimeReleases())
  {
    for (const PageAllocation &allocation : release.pageAllocations)
    {
      deferredPagesBufferBytes_ += allocation.size;
      deferredClusterCount_ += allocation.clusterCount;
      deferredDecodedVertexCount_ += allocation.decodedVertexCount;
      deferredDecodedIndexCount_ += allocation.decodedIndexCount;
    }
    if (release.hierarchyAllocation.offset != UINT64_MAX)
      deferredHierarchyBytes_ += release.hierarchyAllocation.size;
    deferredRuntimeReleases_.push_back(std::move(release));
  }
  else
  {
    applyDeferredRuntimeRelease(release);
  }
  if (record.sceneInstanceId != INVALID_SCENE_INSTANCE_ID)
  {
    instanceManager.deallocateInstance(record.sceneInstanceId);
    publishInstanceLayoutChange();
  }

  runtimeObjectByName_.erase(objectIt);
  const RuntimeObjectId objectId = record.objectId;
  record = {};
  record.objectId = objectId;
  record.registered = false;
  return true;
}

uint32_t VirtualGeometryScene::touchRuntimePagesFromGPUPriorities(const rendering::Buffer *completedReadback)
{
  // GPU-only pools must be observed through a completed-frame Pull copy.
  if (settings.asyncRuntimeUploads && completedReadback == nullptr) return 0u;
  if (nextPrioritySlot == 0u)
    return 0u;
  std::vector<uint32_t> priorities(nextPrioritySlot, 0u);
  renderGraph->bufferRead(
      completedReadback != nullptr ? *completedReadback : pagePriorityBuffer,
      0u,
      static_cast<uint64_t>(priorities.size()) * sizeof(uint32_t),
      [&](const void *data)
      {
        std::memcpy(
            priorities.data(), data,
            static_cast<uint64_t>(priorities.size()) * sizeof(uint32_t));
      });

  uint32_t touched = 0u;
  for (RuntimeObjectRecord &record : runtimeObjects_)
  {
    if (!record.registered)
      continue;
    for (RuntimePageStatus &status : record.pages)
    {
      if (!status.installed || status.prioritySlot >= priorities.size())
        continue;
      if (priorities[status.prioritySlot] == 0u)
        continue;
      status.lastUsedFrame = runtimeStreamingFrame_;
      ++touched;
    }
  }
  return touched;
}

uint32_t VirtualGeometryScene::evictLeastRecentlyUsedRuntimeObjects(
    const uint64_t hierarchyBytes,
    const uint32_t maximumEvictions)
{
  if (hierarchyBytes == 0u || maximumEvictions == 0u)
    return 0u;
  uint32_t evicted = 0u;
  // Under a release delay an eviction frees nothing until the delay elapses,
  // so the loop is bounded by what is already awaiting release instead.
  while (evicted < maximumEvictions &&
         !hierarchyBufferAllocator.canAllocate(hierarchyBytes) &&
         (!deferringRuntimeReleases() || deferredHierarchyBytes_ < hierarchyBytes))
  {
    const RuntimeObjectRecord *victim = nullptr;
    uint64_t victimFrame = 0u;
    for (const RuntimeObjectRecord &record : runtimeObjects_)
    {
      if (!record.registered || record.name.empty())
        continue;
      uint64_t lastUsed = 0u;
      for (const RuntimePageStatus &status : record.pages)
        if (status.installed)
          lastUsed = std::max(lastUsed, status.lastUsedFrame);
      if (victim != nullptr && lastUsed >= victimFrame)
        continue;
      victim = &record;
      victimFrame = lastUsed;
    }
    if (victim == nullptr)
      break;

    const std::string name = victim->name;
    std::string unregisterError;
    if (!unregisterRuntimeObject(name, &unregisterError))
    {
      os::Logger::warningf(
          "[VirtualGeometry] hierarchy eviction could not retire %s: %s",
          name.c_str(), unregisterError.c_str());
      break;
    }
    ++evicted;
  }
  return evicted;
}

bool VirtualGeometryScene::isRuntimeObjectRegistered(
    const std::string &objectName) const
{
  return runtimeObjectByName_.find(objectName) != runtimeObjectByName_.end();
}

void VirtualGeometryScene::setRuntimeStreamingFrame(const uint64_t frame)
{
  runtimeStreamingFrame_ = std::max(runtimeStreamingFrame_, frame);
  drainDeferredRuntimeReleases(false);
}

bool VirtualGeometryScene::findLeastRecentlyUsedRuntimePage(RuntimeObjectId &objectId, uint32_t &localPageIndex) const
{
  bool found = false;
  uint64_t bestLastUsed = 0u;
  for (const RuntimeObjectRecord &record : runtimeObjects_)
  {
    if (!record.registered)
      continue;
    for (const RuntimePageStatus &status : record.pages)
    {
      if (!status.installed || status.rootPage || !status.ownsSceneBufferAllocation)
        continue;
      if (status.pageAllocation.size == 0u)
        continue;
      // Never retire something this frame already installed or used. Without
      // this, installing a whole object in one frame could evict the pages it
      // depends on.
      if (status.lastUsedFrame >= runtimeStreamingFrame_)
        continue;
      bool protectsChild = false;
      for (const RuntimePageStatus &child : record.pages)
      {
        if (!child.installed || child.localPageIndex == status.localPageIndex)
          continue;
        if (child.parentLocalPageIndex == status.localPageIndex ||
            child.fallbackLocalPageIndex == status.localPageIndex)
        {
          protectsChild = true;
          break;
        }
      }
      if (protectsChild || runtimePageIsDependedOn(record, status.localPageIndex))
        continue;
      if (found && status.lastUsedFrame >= bestLastUsed)
        continue;
      objectId = record.objectId;
      localPageIndex = status.localPageIndex;
      bestLastUsed = status.lastUsedFrame;
      found = true;
    }
  }
  return found;
}

uint32_t VirtualGeometryScene::evictLeastRecentlyUsedRuntimePagesWhile(
    const std::function<bool()> &needsMore, const uint32_t maximumEvictions)
{
  uint32_t evicted = 0u;
  while (evicted < maximumEvictions && needsMore())
  {
    RuntimeObjectId objectId = INVALID_RUNTIME_OBJECT_ID;
    uint32_t localPageIndex = 0u;
    if (!findLeastRecentlyUsedRuntimePage(objectId, localPageIndex))
      break;
    const std::string name = runtimeObjects_[objectId].name;
    std::string uninstallError;
    if (!uninstallRuntimeObjectPage(name, localPageIndex, 0u, &uninstallError))
    {
      os::Logger::warningf(
          "[VirtualGeometry] LRU eviction could not retire %s page %u: %s",
          name.c_str(), localPageIndex, uninstallError.c_str());
      break;
    }
    ++evicted;
  }
  return evicted;
}

uint32_t VirtualGeometryScene::evictLeastRecentlyUsedRuntimePages(const uint64_t bytes, const uint32_t maximumEvictions)
{
  if (bytes == 0u || maximumEvictions == 0u)
    return 0u;
  return evictLeastRecentlyUsedRuntimePagesWhile(
      [&]()
      {
        return !pagesBufferAllocator.canAllocate(bytes) &&
            (!deferringRuntimeReleases() || deferredPagesBufferBytes_ < bytes);
      },
      maximumEvictions);
}

VirtualGeometryScene::RuntimePoolUsage VirtualGeometryScene::getRuntimePoolUsage() const
{
  RuntimePoolUsage usage{};
  usage.pagesUsedBytes = pagesBufferAllocator.getAllocatedSize();
  usage.pagesTotalBytes = pagesBufferAllocator.getTotalSize();
  usage.pagesDeferredBytes = deferredPagesBufferBytes_;
  usage.clustersUsed = clusterAllocator.getAllocatedSize();
  usage.clustersTotal = clusterAllocator.getTotalSize();
  usage.clustersDeferred = deferredClusterCount_;
  usage.decodedVerticesUsed = decodedVertexAllocator.getAllocatedSize();
  usage.decodedVerticesTotal = decodedVertexAllocator.getTotalSize();
  usage.decodedVerticesDeferred = deferredDecodedVertexCount_;
  usage.decodedIndicesUsed = decodedIndexAllocator.getAllocatedSize();
  usage.decodedIndicesTotal = decodedIndexAllocator.getTotalSize();
  usage.decodedIndicesDeferred = deferredDecodedIndexCount_;
  return usage;
}

uint32_t VirtualGeometryScene::evictLeastRecentlyUsedRuntimePagesForHeadroom(const double freeFraction, const uint32_t maximumEvictions)
{
  if (freeFraction <= 0.0 || maximumEvictions == 0u)
    return 0u;
  // A pool is short when its free space, plus what deferred releases return
  // within a few frames, is under the requested fraction of its capacity.
  const auto poolShort = [&](const rendering::BufferManager &pool, const uint64_t deferred)
  {
    const uint64_t total = pool.getTotalSize();
    const uint64_t used = pool.getAllocatedSize();
    const uint64_t free = total > used ? total - used : 0u;
    return static_cast<double>(free + deferred) < freeFraction * static_cast<double>(total);
  };
  return evictLeastRecentlyUsedRuntimePagesWhile(
      [&]()
      {
        return poolShort(pagesBufferAllocator, deferredPagesBufferBytes_) ||
            poolShort(clusterAllocator, deferredClusterCount_) ||
            poolShort(decodedVertexAllocator, deferredDecodedVertexCount_) ||
            poolShort(decodedIndexAllocator, deferredDecodedIndexCount_);
      },
      maximumEvictions);
}

bool VirtualGeometryScene::getRuntimeObjectDiagnostics(
    const std::string &objectName,
    RuntimeObjectDiagnostics &diagnostics) const
{
  diagnostics = {};
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end())
    return false;
  const RuntimeObjectRecord &record = runtimeObjects_[objectIt->second];
  diagnostics.registered = true;
  diagnostics.usesCPUCulling = false;
  diagnostics.objectId = record.objectId;
  diagnostics.sceneInstanceId = record.sceneInstanceId;
  diagnostics.pageTableOffset = record.pageTableOffset;
  diagnostics.pageCount = record.pageCount;
  diagnostics.prioritySlotOffset = record.prioritySlotOffset;
  diagnostics.hierarchyStartOffset = record.hierarchyStartOffset;
  diagnostics.hierarchyNodeCount =
      static_cast<uint32_t>(record.hierarchyAllocation.size / sizeof(VirtualGeometryHierarchy));
  diagnostics.queuedDecodeRequestCount = record.queuedDecodeRequestCount;
  diagnostics.latestGeometryRevision = record.latestGeometryRevision;
  for (const RuntimePageStatus &status : record.pages)
  {
    if (!status.installed)
      continue;
    ++diagnostics.installedPageCount;
    if (status.hasTracePayload)
      ++diagnostics.traceablePageCount;
    if (status.hasMaterialPayload)
      ++diagnostics.materialPageCount;
    if (status.sceneEncodedPageDataReady)
      ++diagnostics.sceneEncodedPageCount;
  }
  return true;
}

const VirtualGeometryScene::RuntimePageStatus *
VirtualGeometryScene::getRuntimeObjectPageStatus(
    const std::string &objectName,
    const uint32_t localPageIndex) const
{
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end())
    return nullptr;
  const RuntimeObjectRecord &record = runtimeObjects_[objectIt->second];
  if (localPageIndex >= record.pages.size())
    return nullptr;
  return &record.pages[localPageIndex];
}

std::vector<uint32_t> VirtualGeometryScene::readRuntimeObjectPagePriorities(
    const std::string &objectName, const rendering::Buffer &priorityReadback,
    const bool touchResidentPages)
{
  const auto it = runtimeObjectByName_.find(objectName);
  if (it == runtimeObjectByName_.end() || !priorityReadback.isValid())
    return {};
  const auto &record = runtimeObjects_[it->second];
  if (record.pageCount == 0u || record.prioritySlotOffset == UINT32_MAX)
    return {};
  std::vector<uint32_t> priorities(record.pageCount);
  renderGraph->bufferRead(priorityReadback,
      uint64_t{record.prioritySlotOffset} * sizeof(uint32_t),
      priorities.size() * sizeof(uint32_t), [&](const void *data) {
        std::memcpy(priorities.data(), data, priorities.size() * sizeof(uint32_t));
      });
  if (touchResidentPages)
    for (auto &page : runtimeObjects_[it->second].pages)
      if (page.installed && page.localPageIndex < priorities.size() && priorities[page.localPageIndex] != 0u)
        page.lastUsedFrame = runtimeStreamingFrame_;
  return priorities;
}

std::vector<VirtualGeometryScene::RuntimeObjectPageRequest>
VirtualGeometryScene::collectRuntimeObjectPageRequests(
    const std::string &objectName,
    const uint32_t maxRequests,
    const bool includeInstalled,
    const rendering::Buffer *priorityReadback,
    const uint32_t minPriority,
    const std::vector<uint32_t> *prioritySnapshot)
{
  std::vector<RuntimeObjectPageRequest> requests;
  if (maxRequests == 0u)
    return requests;
  const auto objectIt = runtimeObjectByName_.find(objectName);
  if (objectIt == runtimeObjectByName_.end())
    return requests;
  const RuntimeObjectRecord &record = runtimeObjects_[objectIt->second];
  if (record.pageCount == 0u || record.prioritySlotOffset == UINT32_MAX)
    return requests;

  std::vector<uint32_t> readback;
  if (prioritySnapshot == nullptr)
  {
    readback = readRuntimeObjectPagePriorities(objectName,
        priorityReadback != nullptr && priorityReadback->isValid()
            ? *priorityReadback : pagePriorityBuffer);
    prioritySnapshot = &readback;
  }
  const auto &priorities = *prioritySnapshot;
  if (priorities.size() != record.pageCount)
    return requests;

  requests.reserve(std::min<uint32_t>(record.pageCount, maxRequests));
  for (uint32_t localPageIndex = 0u; localPageIndex < record.pageCount;
       ++localPageIndex)
  {
    const uint32_t priority = priorities[localPageIndex];
    if (priority == 0u || priority < minPriority)
      continue;
    const RuntimePageStatus &status = record.pages[localPageIndex];
    if (!includeInstalled && status.installed)
      continue;
    RuntimeObjectPageRequest request{};
    request.localPageIndex = localPageIndex;
    request.globalPageTableIndex = record.pageTableOffset + localPageIndex;
    request.prioritySlot = record.prioritySlotOffset + localPageIndex;
    request.priority = priority;
    requests.push_back(request);
  }
  const auto selectedEnd = requests.begin() + std::min<size_t>(requests.size(), maxRequests);
  std::partial_sort(
      requests.begin(), selectedEnd, requests.end(),
      [](const RuntimeObjectPageRequest &left,
         const RuntimeObjectPageRequest &right)
      {
        if (left.priority != right.priority)
          return left.priority > right.priority;
        return left.localPageIndex < right.localPageIndex;
      });
  if (requests.size() > maxRequests)
    requests.resize(maxRequests);
  return requests;
}

void VirtualGeometryScene::enqueuePageDecodeRequest(uint32_t globalPageIndex)
{
  pendingPageDecodeRequests_.push_back(globalPageIndex);
}

std::vector<uint32_t> VirtualGeometryScene::consumePendingPageDecodeRequests()
{
  std::vector<uint32_t> requests;
  requests.swap(pendingPageDecodeRequests_);
  return requests;
}

void VirtualGeometryScene::releaseRuntimePageAllocation(
    const PageAllocation &allocation)
{
  if (allocation.size != 0u && allocation.bufferOffset != UINT32_MAX)
    pagesBufferAllocator.deallocate(
        rendering::Allocation{allocation.bufferOffset, allocation.size});
  if (allocation.clusterCount != 0u)
    clusterAllocator.deallocate(
        rendering::Allocation{allocation.clusterOffset, allocation.clusterCount});
  if (allocation.decodedVertexCount != 0u)
    decodedVertexAllocator.deallocate(
        rendering::Allocation{allocation.decodedVertexOffset, allocation.decodedVertexCount});
  if (allocation.decodedIndexCount != 0u)
    decodedIndexAllocator.deallocate(
        rendering::Allocation{allocation.decodedIndexOffset, allocation.decodedIndexCount});
  if (allocation.decodedBoneWeightCount != 0u)
    decodedBoneWeightAllocator.deallocate(
        rendering::Allocation{allocation.decodedBoneWeightOffset, allocation.decodedBoneWeightCount});
}

bool VirtualGeometryScene::debugReadFrameOverrideInstances(
    const uint32_t frameSlot,
    std::vector<VirtualGeometryInstanceGPUData> &out) const
{
  out.clear();
  const rendering::BufferId overrideId = getFrameOverrideInstanceBufferId(frameSlot);
  if (overrideId == rendering::BufferId::Invalid)
    return false;
  const size_t count = instanceManager.getPackedData().size();
  if (count == 0u)
    return true;
  out.resize(count);
  renderGraph->getRHI()->bufferRead(
      overrideId, 0, count * sizeof(VirtualGeometryInstanceGPUData),
      [&](const void *data) { std::memcpy(out.data(), data, count * sizeof(VirtualGeometryInstanceGPUData)); });
  return true;
}

void VirtualGeometryScene::setRuntimeReleaseDelayFrames(const uint32_t frames)
{
  runtimeReleaseDelayFrames_ = frames;
  if (frames == 0u)
    drainDeferredRuntimeReleases(true);
}

void VirtualGeometryScene::writeClearedPageTableEntry(
    const uint32_t globalPageTableIndex,
    const uint32_t prioritySlot)
{
  PageTableEntry entry{};
  entry.isInstalled = 0u;
  entry.prioritySlot = prioritySlot;
  writeSceneBuffer(
      pageTableBuffer,
      static_cast<uint64_t>(globalPageTableIndex) * sizeof(PageTableEntry),
      sizeof(PageTableEntry),
      &entry);
}

void VirtualGeometryScene::applyDeferredRuntimeRelease(const DeferredRuntimeRelease &release)
{
  for (const auto &[globalPageTableIndex, prioritySlot] : release.pageTableEntriesToClear)
    writeClearedPageTableEntry(globalPageTableIndex, prioritySlot);
  for (const PageAllocation &allocation : release.pageAllocations)
    releaseRuntimePageAllocation(allocation);
  if (release.hierarchyAllocation.offset != UINT64_MAX && release.hierarchyAllocation.size != 0u)
    hierarchyBufferAllocator.deallocate(release.hierarchyAllocation);
  if (release.pageTableSlots.offset != UINT64_MAX && release.pageTableSlots.size != 0u)
    pageTableSlotAllocator.deallocate(release.pageTableSlots);
  if (release.prioritySlots.offset != UINT64_MAX && release.prioritySlots.size != 0u)
    prioritySlotAllocator.deallocate(release.prioritySlots);
}

uint32_t VirtualGeometryScene::drainDeferredRuntimeReleases(const bool force)
{
  uint32_t released = 0u;
  for (size_t index = 0u; index < deferredRuntimeReleases_.size();)
  {
    const DeferredRuntimeRelease &release = deferredRuntimeReleases_[index];
    if (!force && release.releaseFrame > runtimeStreamingFrame_)
    {
      ++index;
      continue;
    }
    for (const PageAllocation &allocation : release.pageAllocations)
    {
      deferredPagesBufferBytes_ -= std::min<uint64_t>(deferredPagesBufferBytes_, allocation.size);
      deferredClusterCount_ -= std::min<uint64_t>(deferredClusterCount_, allocation.clusterCount);
      deferredDecodedVertexCount_ -=
          std::min<uint64_t>(deferredDecodedVertexCount_, allocation.decodedVertexCount);
      deferredDecodedIndexCount_ -=
          std::min<uint64_t>(deferredDecodedIndexCount_, allocation.decodedIndexCount);
    }
    if (release.hierarchyAllocation.offset != UINT64_MAX)
      deferredHierarchyBytes_ -=
          std::min<uint64_t>(deferredHierarchyBytes_, release.hierarchyAllocation.size);
    applyDeferredRuntimeRelease(release);
    deferredRuntimeReleases_.erase(deferredRuntimeReleases_.begin() + static_cast<std::ptrdiff_t>(index));
    ++released;
  }
  return released;
}

void VirtualGeometryScene::retireRuntimePageAllocation(
    const PageAllocation &allocation,
    const uint32_t globalPageTableIndex,
    const uint32_t prioritySlot,
    const bool clearPageTableEntry)
{
  DeferredRuntimeRelease release{};
  release.releaseFrame = runtimeStreamingFrame_ + runtimeReleaseDelayFrames_;
  if (allocation.size != 0u || allocation.clusterCount != 0u)
    release.pageAllocations.push_back(allocation);
  if (clearPageTableEntry && globalPageTableIndex != UINT32_MAX)
    release.pageTableEntriesToClear.emplace_back(globalPageTableIndex, prioritySlot);
  if (release.pageAllocations.empty() && release.pageTableEntriesToClear.empty())
    return;
  if (!deferringRuntimeReleases())
  {
    applyDeferredRuntimeRelease(release);
    return;
  }
  deferredPagesBufferBytes_ += allocation.size;
  deferredRuntimeReleases_.push_back(std::move(release));
}

bool VirtualGeometryScene::runtimePageIsDependedOn(
    const RuntimeObjectRecord &record,
    const uint32_t localPageIndex) const
{
  return localPageIndex < record.pages.size() &&
      record.pages[localPageIndex].dependedOnBy != 0u;
}

// Rebuilds the object's leaf install bits and per-cluster masks from the set
// of installed pages, replaying each installed page's own leaves and install
// rewrites in ascending page order - the same sequence installRuntimeObjectPage
// performs incrementally, so a full install and an install-then-uninstall
// history land on identical nodes. The whole object's hierarchy is then
// written to the GPU.
void VirtualGeometryScene::rebuildRuntimeObjectHierarchyMasks(RuntimeObjectRecord &record)
{
  if (record.hierarchy.empty() || record.hierarchyAllocation.offset == UINT64_MAX)
    return;

  for (VirtualGeometryHierarchy &node : record.hierarchy)
  {
    if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
      continue;
    const uint32_t rawPage = node.pageIndex & ~PAGE_NOT_INSTALLED_BIT;
    const bool installed = rawPage < record.pages.size() && record.pages[rawPage].installed;
    node.pageIndex = installed ? rawPage : (PAGE_NOT_INSTALLED_BIT | rawPage);
    setNodeClusterMasks(node, 0u, 0u);
  }

  for (const RuntimePageStatus &status : record.pages)
  {
    if (!status.installed)
      continue;
    for (VirtualGeometryHierarchy &node : record.hierarchy)
    {
      if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex != status.localPageIndex)
        continue;
      if (node.child_start == UINT32_MAX)
        node.child_start = 0u;
      const uint8_t enabledMask = fullEnabledMaskForNode(node);
      const uint8_t streamingMask =
          (node.flags & HIERARCHY_DECODED_CLUSTER_FLAGS_FLAG) != 0u ? 0u
          : status.installUpdates.empty() ? enabledMask : 0u;
      setNodeClusterMasks(node, streamingMask, enabledMask);
    }
    for (const HierarchyClusterFlagsUpdate &update : status.installUpdates)
    {
      if (update.hierarchyNodeIndex >= record.hierarchy.size())
        continue;
      VirtualGeometryHierarchy &node = record.hierarchy[update.hierarchyNodeIndex];
      if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u)
        continue;
      setNodeClusterMasks(node, update.streamingLeafsBitset, update.enabledClustersBitset);
    }
  }

  // External masks last: they encode a relation between pages that the builder
  // could not put in either one's own rewrites, so they have to survive the
  // replay of both.
  for (const RuntimePageStatus &status : record.pages)
  {
    if (!status.installed)
      continue;
    for (const HierarchyClusterFlagsUpdate &update : status.externalUpdates)
    {
      if (update.hierarchyNodeIndex >= record.hierarchy.size())
        continue;
      VirtualGeometryHierarchy &node = record.hierarchy[update.hierarchyNodeIndex];
      if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u)
        continue;
      setNodeClusterMasks(node, update.streamingLeafsBitset, update.enabledClustersBitset);
    }
  }

  writeSceneBuffer(
      hierarchyBuffer,
      record.hierarchyAllocation.offset,
      static_cast<uint64_t>(record.hierarchy.size()) * sizeof(VirtualGeometryHierarchy),
      record.hierarchy.data());
}

void VirtualGeometryScene::rebuildRuntimeObjectHierarchyMasksScoped(
    RuntimeObjectRecord &record,
    const std::vector<RuntimePageInstallInfo::HierarchyScanRange> &scope)
{
  if (record.hierarchy.empty() || record.hierarchyAllocation.offset == UINT64_MAX)
    return;
  const auto inScope = [&](const uint32_t nodeIndex)
  {
    for (const auto &range : scope)
      if (nodeIndex >= range.firstNode && nodeIndex < range.firstNode + range.nodeCount)
        return true;
    return false;
  };
  const auto forEachScopedNode = [&](auto &&visit)
  {
    for (const auto &range : scope)
    {
      const uint32_t end = static_cast<uint32_t>(std::min<uint64_t>(
          record.hierarchy.size(), uint64_t{range.firstNode} + range.nodeCount));
      for (uint32_t nodeIndex = range.firstNode; nodeIndex < end; ++nodeIndex)
        visit(nodeIndex, record.hierarchy[nodeIndex]);
    }
  };

  // Reset every leaf in scope to "not installed, no clusters".
  forEachScopedNode([&](const uint32_t, VirtualGeometryHierarchy &node)
  {
    if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
      return;
    const uint32_t rawPage = node.pageIndex & ~PAGE_NOT_INSTALLED_BIT;
    const bool installed = rawPage < record.pages.size() && record.pages[rawPage].installed;
    node.pageIndex = installed ? rawPage : (PAGE_NOT_INSTALLED_BIT | rawPage);
    setNodeClusterMasks(node, 0u, 0u);
  });

  // Replay only the pages that share this scope - the same chunk.
  for (const RuntimePageStatus &status : record.pages)
  {
    if (!status.installed || status.hierarchyScanRanges.size() != scope.size())
      continue;
    bool sameScope = true;
    for (size_t index = 0u; sameScope && index < scope.size(); ++index)
      sameScope = status.hierarchyScanRanges[index].firstNode == scope[index].firstNode &&
          status.hierarchyScanRanges[index].nodeCount == scope[index].nodeCount;
    if (!sameScope)
      continue;
    forEachScopedNode([&](const uint32_t, VirtualGeometryHierarchy &node)
    {
      if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex != status.localPageIndex)
        return;
      if (node.child_start == UINT32_MAX)
        node.child_start = 0u;
      const uint8_t enabledMask = fullEnabledMaskForNode(node);
      const uint8_t streamingMask =
          (node.flags & HIERARCHY_DECODED_CLUSTER_FLAGS_FLAG) != 0u ? 0u
          : status.installUpdates.empty() ? enabledMask : 0u;
      setNodeClusterMasks(node, streamingMask, enabledMask);
    });
    for (const HierarchyClusterFlagsUpdate &update : status.installUpdates)
    {
      if (update.hierarchyNodeIndex >= record.hierarchy.size() || !inScope(update.hierarchyNodeIndex))
        continue;
      VirtualGeometryHierarchy &node = record.hierarchy[update.hierarchyNodeIndex];
      if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u)
        continue;
      setNodeClusterMasks(node, update.streamingLeafsBitset, update.enabledClustersBitset);
    }
  }

  for (const auto &range : scope)
  {
    const uint64_t end = std::min<uint64_t>(
        record.hierarchy.size(), uint64_t{range.firstNode} + range.nodeCount);
    if (end <= range.firstNode)
      continue;
    writeSceneBuffer(
        hierarchyBuffer,
        record.hierarchyAllocation.offset +
            static_cast<uint64_t>(range.firstNode) * sizeof(VirtualGeometryHierarchy),
        (end - range.firstNode) * sizeof(VirtualGeometryHierarchy),
        record.hierarchy.data() + range.firstNode);
  }
}

// ============================================================================
// Static helpers
// ============================================================================

uint32_t VirtualGeometryScene::getGlobalClusterIndex(const PageTableEntry &pageEntry, uint32_t pageLocalClusterIndex)
{
  return pageEntry.clusterOffset + pageLocalClusterIndex;
}

std::pair<uint32_t, uint32_t> VirtualGeometryScene::getPageClusterRange(const PageTableEntry &pageEntry)
{
  return {pageEntry.clusterOffset, pageEntry.clusterOffset + pageEntry.clusterCount};
}

size_t VirtualGeometryScene::getInstanceCount() const
{
  return instanceManager.getInstanceCount();
}

} // namespace virtualgeometry
