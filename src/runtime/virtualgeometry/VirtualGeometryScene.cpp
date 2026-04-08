#include "VirtualGeometryScene.hpp"
#include "os/Logger.hpp"
#include "rendering/animation/AnimationPlayer.hpp"
#include "virtualgeometry/VirtualMaterialFile.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <queue>
#include <stdexcept>

namespace virtualgeometry
{

namespace fs = std::filesystem;

static constexpr uint64_t kDefaultMeshPartTransformBufferSize = sizeof(math::Mat4f) * 1024ull * 64ull;

namespace
{

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
    rendering::Buffer pagesBuffer,
    rendering::Buffer hierarchyBuffer,
    rendering::Buffer pageTableBuffer,
    uint32_t pageTableOffset,
    uint32_t *nextClusterOffset)
    : file_(file), renderGraph_(renderGraph), pagesAllocator_(pagesAllocator), pagesBuffer_(pagesBuffer), hierarchyBuffer_(hierarchyBuffer), pageTableBuffer_(pageTableBuffer), pageTableOffset_(pageTableOffset),
      nextClusterOffset_(nextClusterOffset), loadRequests_(), completedLoads_()
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

  if (!pagesAllocator_->canAllocate(pageSize))
    return false;

  rendering::Allocation pageAlloc = pagesAllocator_->allocate(pageSize);
  if (pageAlloc.offset == UINT64_MAX)
    return false;
  const uint32_t globalClusterBase = *nextClusterOffset_;
  *nextClusterOffset_ += meshletCount;
  renderGraph_->bufferWrite(pagesBuffer_, pageAlloc.offset, pageSize, payload.bytes->data());

  PageAllocation &alloc = allocations_[pageId];
  alloc.bufferOffset = static_cast<uint32_t>(pageAlloc.offset);
  alloc.size = pageSize;
  alloc.clusterOffset = globalClusterBase;
  alloc.clusterCount = meshletCount;
  alloc.isInstalled = true;

  installed_[pageId] = true;

  writePageTableEntry(pageId, alloc);

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

  alloc.bufferOffset = UINT32_MAX;
  alloc.size = 0;
  alloc.clusterOffset = 0;
  alloc.clusterCount = 0;
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
  gpuData._padding[0] = 0u;
  gpuData._padding[1] = 0u;

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
  packedInstances[sparseToDense[instanceId]].modelMatrix = buildModelMatrix(position, rotation, scale);
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

VirtualGeometryScene::VirtualGeometryScene(rendering::RenderGraph *renderGraph, uint32_t hierarchyBufferSize, uint64_t pagesBufferSize)
    : renderGraph(renderGraph), hierarchyBufferAllocator(hierarchyBufferSize), pagesBufferAllocator(pagesBufferSize), meshPartTransformsAllocator(kDefaultMeshPartTransformBufferSize), nextClusterOffset(0),
      hierarchyBufferSize(hierarchyBufferSize), pagesBufferSize(pagesBufferSize), meshPartTransformsBufferSize(kDefaultMeshPartTransformBufferSize), nextPageTableSlot(0), nextPrioritySlot(0)
{
  hierarchyBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryHierarchyBuffer",
        .scratch = false,
        .size = hierarchyBufferSize,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });

  pagesBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryPagesBuffer",
        .scratch = false,
        .size = pagesBufferSize,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });

  instanceBufferSize = sizeof(VirtualGeometryInstanceGPUData) * 1024 * 1024;
  instanceBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryInstanceBuffer",
        .size = instanceBufferSize,
        .scratch = false,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  initializeFrameOverrideInstanceBuffers();

  maxPages = 65536;
  pagePriorityBufferSize = maxPages * sizeof(uint32_t);
  pagePriorityBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryPagePriorityBuffer",
        .size = pagePriorityBufferSize,
        .scratch = false,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });

  pageInstallCandidateBufferSize = STREAMING_PAGE_SELECTION_COUNT * sizeof(StreamingPageCandidate);
  pageInstallCandidateBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryPageInstallCandidateBuffer",
        .size = pageInstallCandidateBufferSize,
        .scratch = false,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });

  pageEvictCandidateBufferSize = STREAMING_PAGE_SELECTION_COUNT * sizeof(StreamingPageCandidate);
  pageEvictCandidateBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryPageEvictCandidateBuffer",
        .size = pageEvictCandidateBufferSize,
        .scratch = false,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });

  meshPartTransformsBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryMeshPartTransformsBuffer",
        .size = meshPartTransformsBufferSize,
        .scratch = false,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });

  pagesTableBufferSize = maxPages * sizeof(PageTableEntry);
  pageTableBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryPageTableBuffer",
        .size = pagesTableBufferSize,
        .scratch = false,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  {
    std::vector<PageTableEntry> empty(maxPages);
    renderGraph->bufferWrite(pageTableBuffer, 0, pagesTableBufferSize, empty.data());
  }

  {
    std::vector<uint32_t> emptyPriorities(maxPages, 0u);
    renderGraph->bufferWrite(pagePriorityBuffer, 0, pagePriorityBufferSize, emptyPriorities.data());
  }

  {
    std::vector<StreamingPageCandidate> emptyCandidates(STREAMING_PAGE_SELECTION_COUNT);
    renderGraph->bufferWrite(pageInstallCandidateBuffer, 0, pageInstallCandidateBufferSize, emptyCandidates.data());
    renderGraph->bufferWrite(pageEvictCandidateBuffer, 0, pageEvictCandidateBufferSize, emptyCandidates.data());
  }
}

VirtualGeometryScene::~VirtualGeometryScene()
{
  for (auto it = files.begin(); it != files.end(); ++it)
    delete it.value();
  for (auto it = cameras.begin(); it != cameras.end(); ++it)
    delete it.value();

  renderGraph->deleteBuffer(pageTableBuffer);
  renderGraph->deleteBuffer(hierarchyBuffer);
  renderGraph->deleteBuffer(pagesBuffer);
  renderGraph->deleteBuffer(instanceBuffer);
  renderGraph->deleteBuffer(pagePriorityBuffer);
  renderGraph->deleteBuffer(pageInstallCandidateBuffer);
  renderGraph->deleteBuffer(pageEvictCandidateBuffer);
  renderGraph->deleteBuffer(meshPartTransformsBuffer);
  destroyFrameOverrideInstanceBuffers();
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
    VirtualTextureSystem::PreparedMaterial defaultMaterial;
    for (uint32_t materialIndex = 0u; materialIndex <= maxMaterialIndex; ++materialIndex)
    {
      if (!virtualTextureSystem.hasMaterial(materialIndex))
        virtualTextureSystem.registerPreparedMaterial(materialIndex, defaultMaterial);
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

  const size_t pageCount = runtimeData->file.getPageTable().size();

  if (nextPageTableSlot + pageCount > maxPages)
    throw std::runtime_error("registerObjectForStreaming: page table exhausted for " + name);
  if (nextPrioritySlot + pageCount > maxPages)
    throw std::runtime_error("registerObjectForStreaming: priority buffer exhausted for " + name);

  runtimeData->pageTableOffset = nextPageTableSlot;
  nextPageTableSlot += static_cast<uint32_t>(pageCount);
  runtimeData->localBounds = computeHierarchyBounds(runtimeData->file.getHierarchy());

  // Initialise streaming manager now that we know the page table offset
  runtimeData->streamingManager =
      std::make_unique<VirtualGeometryStreamingManager>(&runtimeData->file, renderGraph, &pagesBufferAllocator, pagesBuffer, hierarchyBuffer, pageTableBuffer, runtimeData->pageTableOffset, &nextClusterOffset);

  // Assign priority slots and write zeroed PageTableEntries
  // getPageAllocations() is const — we fill priority slots via the public allocations vector
  // by accessing it through the allocations_ member — cast to non-const through the ref trick
  // is not available; instead we expose a helper. For now assign priority slots directly:
  for (size_t i = 0; i < pageCount; ++i)
  {
    // We can't directly write to the streaming manager's allocations (const ref).
    // Use the scene-level nextPrioritySlot and write to page table + priority buffer.
    const uint32_t prioritySlot = nextPrioritySlot++;

    PageTableEntry entry{};
    entry.isInstalled = 0u;
    entry.prioritySlot = prioritySlot;
    const uint32_t globalSlot = runtimeData->pageTableOffset + static_cast<uint32_t>(i);
    renderGraph->bufferWrite(pageTableBuffer, globalSlot * sizeof(PageTableEntry), sizeof(PageTableEntry), &entry);
  }

  // Zero out priority slots
  {
    const uint32_t firstSlot = runtimeData->pageTableOffset; // reuse as proxy since slots align
    std::vector<uint32_t> zeros(pageCount, 0u);
    renderGraph->bufferWrite(pagePriorityBuffer, (nextPrioritySlot - static_cast<uint32_t>(pageCount)) * sizeof(uint32_t), pageCount * sizeof(uint32_t), zeros.data());
  }

  // Re-assign priority slots into the new manager's allocations
  {
    const uint32_t basePrioritySlot = nextPrioritySlot - static_cast<uint32_t>(pageCount);
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
  VirtualTextureSystem::PreparedMaterial material;
  if (!VirtualMaterialFile::load(materialPath, material))
    return false;

  return virtualTextureSystem.registerPreparedMaterial(materialIndex, material);
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
  if (boneCount > 0u)
  {
    record.meshPartTransformsAllocation = meshPartTransformsAllocator.allocate(static_cast<uint64_t>(boneCount) * sizeof(math::Mat4f));
    if (record.meshPartTransformsAllocation.offset == UINT64_MAX)
      throw std::runtime_error("Failed to allocate mesh-part transform buffer space");

    meshPartTransformsOffset = static_cast<uint32_t>(record.meshPartTransformsAllocation.offset / sizeof(math::Mat4f));
    const std::vector<math::Mat4f> defaultPalette = data->skeleton.buildRestPoseSkinningPalette();
    renderGraph->bufferWrite(meshPartTransformsBuffer, record.meshPartTransformsAllocation.offset, static_cast<uint64_t>(defaultPalette.size()) * sizeof(math::Mat4f), const_cast<math::Mat4f *>(defaultPalette.data()));
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
        objectName, position, rotation, scale, hierarchyBaseOffset, data->pageTableOffset, materialIndexOverride == UINT32_MAX ? 0u : materialIndexOverride, meshPartTransformsOffset, data->quantizationConfig));
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

  const ObjectInstanceRecord &record = objectInstances[objectSparseToDense[instanceId]];
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
  for (SceneInstanceId sceneInstanceId : record.sceneInstanceIds)
  {
    updated = instanceManager.updateInstance(sceneInstanceId, position, rotation, math::Vec3f(scale, scale, scale)) || updated;
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

  const ObjectInstanceRecord &record = objectInstances[objectSparseToDense[instanceId]];
  if (record.meshPartTransformsAllocation.offset == UINT64_MAX || record.meshPartTransformsAllocation.size == 0u)
    return boneTransforms.empty();

  auto runtimeData = files.find(record.objectName);
  if (runtimeData == files.end())
    return false;

  const VirtualGeometryObjectRuntimeData *data = runtimeData.value();
  if (boneTransforms.size() != data->skeleton.getBoneCount())
    return false;

  renderGraph->bufferWrite(meshPartTransformsBuffer, record.meshPartTransformsAllocation.offset, static_cast<uint64_t>(boneTransforms.size()) * sizeof(math::Mat4f), const_cast<math::Mat4f *>(boneTransforms.data()));
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

  writePackedInstanceDataToBuffers(packedData, uploadAllFrameSlots);
  instanceManager.clearDirtyFlag();
}

void VirtualGeometryScene::appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
{
  const rendering::BufferId overrideId = getFrameOverrideInstanceBufferId(frameSlot);
  if (overrideId == rendering::BufferId::Invalid)
    return;

  overrides.bufferOverrides.emplace(instanceBuffer.name, rendering::RenderGraphBufferOverride{.bufferId = overrideId});
}

void VirtualGeometryScene::prepareFrame(uint32_t frameSlot)
{
  const rendering::BufferId overrideId = getFrameOverrideInstanceBufferId(frameSlot);
  if (overrideId == rendering::BufferId::Invalid)
    return;

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

void VirtualGeometryScene::initializeFrameOverrideInstanceBuffers()
{
  if (renderGraph == nullptr)
    return;

  const uint32_t frameCount = std::max(1u, renderGraph->getMaxFramesInFlight());
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

void VirtualGeometryScene::writeInstanceRecordToCurrentBuffers(uint32_t denseIndex, const VirtualGeometryInstanceGPUData &instanceData)
{
  const uint64_t offset = static_cast<uint64_t>(denseIndex) * sizeof(VirtualGeometryInstanceGPUData);
  renderGraph->bufferWrite(instanceBuffer, offset, sizeof(VirtualGeometryInstanceGPUData), const_cast<VirtualGeometryInstanceGPUData *>(&instanceData));
}

void VirtualGeometryScene::writePackedInstanceDataToBuffers(const std::vector<VirtualGeometryInstanceGPUData> &packedData, bool uploadAllFrameSlots)
{
  const uint64_t uploadSize = packedData.size() * sizeof(VirtualGeometryInstanceGPUData);
  renderGraph->bufferWrite(instanceBuffer, 0, uploadSize, (void *)packedData.data());

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
