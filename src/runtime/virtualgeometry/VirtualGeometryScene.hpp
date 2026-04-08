#pragma once
#include "datastructure/ConcurrentHashMap.hpp"
#include "datastructure/ConcurrentQueue.hpp"
#include "os/Thread.hpp"
#include "rendering/core/BufferManager.hpp"
#include "rendering/core/Camera.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/VirtualGeometryBounds.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"
#include "virtualgeometry/VirtualTextureSystem.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rendering
{
namespace animation
{
class AnimationPlayer;
}
} // namespace rendering

namespace virtualgeometry
{

namespace animation = rendering::animation;

// GPU instance data — must match InstanceData in virtualgeometrydata.wgsl.
// Layout (96 bytes):
//   modelMatrix          — mat4x4<f32>  (64 bytes)
//   hierarchyStartOffset — u32          ( 4 bytes)  index of root node in hierarchy[]
//   quantization_factor  — u32          ( 4 bytes)  raw qFactor for dequant
//   unit_scale_bits      — u32          ( 4 bytes)  f32 unit_scale bitcast as u32
//   pageTableOffset      — u32          ( 4 bytes)  index of this object's first
//                                                   PageTableEntry in the global
//                                                   pageTable[].  Shader computes:
//                                                     globalPageIdx =
//                                                       pageTableOffset + (node.pageIndex & ~PAGE_NOT_INSTALLED_BIT)
//   materialIndex        — u32          ( 4 bytes)  per-shape material slot
//   meshPartTransformsOffset — u32      ( 4 bytes)  first bone matrix in meshPartTransforms[]
struct VirtualGeometryInstanceGPUData
{
  math::Mat4f modelMatrix;
  uint32_t hierarchyStartOffset;
  uint32_t quantization_factor;
  uint32_t unit_scale_bits;
  uint32_t pageTableOffset; // was _padding — now carries the global page table base
  uint32_t materialIndex;
  uint32_t meshPartTransformsOffset;
  uint32_t _padding[2];
};
static_assert(sizeof(VirtualGeometryInstanceGPUData) == 96, "VirtualGeometryInstanceGPUData size mismatch");

using InstanceId = uint32_t;
static constexpr InstanceId INVALID_INSTANCE_ID = UINT32_MAX;
using SceneInstanceId = uint32_t;
static constexpr SceneInstanceId INVALID_SCENE_INSTANCE_ID = UINT32_MAX;

// Bit 31 of VirtualGeometryHierarchy::pageIndex is the "not-yet-installed" flag.
// The CPU sets this bit for every hierarchy node whose page is not currently resident:
//   node.pageIndex = PAGE_NOT_INSTALLED_BIT | localPageIndex   (not installed)
//   node.pageIndex = localPageIndex                            (installed)
// The shader tests (pageIndex & PAGE_NOT_INSTALLED_BIT) to skip non-resident nodes
// and masks it off before indexing the page table.
static constexpr uint32_t PAGE_NOT_INSTALLED_BIT = (1u << 31);

struct PageAllocation
{
  uint32_t pageId = UINT32_MAX;
  uint32_t bufferOffset = UINT32_MAX;
  uint32_t size = 0u;
  uint32_t clusterOffset = 0u;
  uint32_t clusterCount = 0u;
  // Globally unique index into pagePriorityBuffer[].
  // Assigned ONCE at registerObjectForStreaming time (one slot per page,
  // regardless of residency).  Never recycled while the object exists, so the
  // shader can accumulate priority demand for uninstalled pages too.
  uint32_t prioritySlot = UINT32_MAX;
  bool isInstalled = false;
};

struct PageTableEntry
{
  uint32_t bufferOffset;  // byte offset in pagesBuffer     (0 when not installed)
  uint32_t size;          // page size in bytes             (0 when not installed)
  uint32_t clusterOffset; // global cluster base index      (0 when not installed)
  uint32_t clusterCount;  // number of meshlets             (0 when not installed)
  uint32_t isInstalled;   // 0 = not resident, 1 = resident
  // Permanently assigned at registration; valid regardless of isInstalled.
  // The shader writes pagePriorities[entry.prioritySlot] so streaming demand
  // can be accumulated for pages that are not yet loaded.
  uint32_t prioritySlot;
  uint32_t _padding[2]; // pad to 32 bytes
};
static_assert(sizeof(PageTableEntry) == 32, "PageTableEntry must be 32 bytes");

struct VisibleClusterInfo
{
  uint32_t pageIndex;
  uint32_t pageLocalClusterIndex;
  uint32_t instanceIndex;
  uint32_t _padding;
};
static_assert(sizeof(VisibleClusterInfo) == 16, "VisibleClusterInfo must be 16 bytes");

struct StreamingPageCandidate
{
  uint32_t globalPageIndex = UINT32_MAX;
  uint32_t priority = 0u;
};
static_assert(sizeof(StreamingPageCandidate) == 8, "StreamingPageCandidate must be 8 bytes");

struct PageDependencyGraph
{
  std::vector<std::vector<uint32_t>> children;
  std::vector<std::vector<uint32_t>> parents;
  std::vector<uint32_t> pageDepth;
  std::vector<uint32_t> rootPageIds;
};

// ─────────────────────────────────────────────────────────────────────────────
// VirtualGeometryStreamingManager
//
// Manages streaming of pages from a VirtualGeometryFile into GPU VRAM for a
// single object.  Handles:
//   • uploading raw page data to the pages buffer
//   • toggling bit 31 on hierarchy nodes when pages are installed/uninstalled
//   • applying per-page HierarchyClusterFlagsUpdate rewrites (8-bit enabled +
//     8-bit streaming masks per leaf node) from file metadata
// ─────────────────────────────────────────────────────────────────────────────
class VirtualGeometryStreamingManager
{
public:
  struct DecodedPagePayload
  {
    std::shared_ptr<std::vector<uint8_t>> bytes;
    uint32_t dataSize = 0u;
    uint32_t meshletCount = 0u;

    bool isValid() const
    {
      return bytes && dataSize != 0u;
    }
  };

  struct HierarchyUpload
  {
    uint32_t id = UINT32_MAX;
    uint64_t hierarchyAllocationOffset = 0u;
    uint32_t hierarchyNodeOffsetInFile = 0u;
    std::vector<VirtualGeometryHierarchy> runtimeHierarchy;
  };

  VirtualGeometryStreamingManager() = delete;
  VirtualGeometryStreamingManager(
      const VirtualGeometryFile *file,
      rendering::RenderGraph *renderGraph,
      rendering::BufferManager *pagesAllocator,
      rendering::Buffer pagesBuffer,
      rendering::Buffer hierarchyBuffer,
      rendering::Buffer pageTableBuffer,
      uint32_t pageTableOffset,
      uint32_t *nextClusterOffset);
  ~VirtualGeometryStreamingManager();

  VirtualGeometryStreamingManager(const VirtualGeometryStreamingManager &) = delete;
  VirtualGeometryStreamingManager &operator=(const VirtualGeometryStreamingManager &) = delete;

  uint32_t registerHierarchyUpload(uint64_t hierarchyAllocationOffset, uint32_t hierarchyNodeOffsetInFile, const std::vector<VirtualGeometryHierarchy> &hierarchyTemplate);
  bool unregisterHierarchyUpload(uint32_t hierarchyUploadId);

  bool isPageInstalled(uint32_t pageId) const;
  bool isPageLoadPending(uint32_t pageId) const;
  bool hasDecodedPageReady(uint32_t pageId) const;
  void requestPageLoad(uint32_t pageId);
  void pumpCompletedLoads();
  void installPage(uint32_t pageId);
  void uninstallPage(uint32_t pageId);
  uint32_t installReadyPages(const PageDependencyGraph &pageDeps, uint32_t maxPageInstalls, bool *blockedOnMemory = nullptr);
  void installPageRecursive(uint32_t pageId, const PageDependencyGraph &pageDeps);
  void uninstallPageRecursive(uint32_t pageId, const PageDependencyGraph &pageDeps);

  const std::vector<PageAllocation> &getPageAllocations() const
  {
    return allocations_;
  }
  uint32_t getPendingLoadCount() const;
  uint32_t getReadyPageCount() const;

private:
  struct PageLoadRequest
  {
    uint32_t pageId = UINT32_MAX;
  };

  struct PageLoadResult
  {
    uint32_t pageId = UINT32_MAX;
    bool success = false;
    DecodedPagePayload payload;
  };

  // Rebuild hierarchy masks and install bits after installing page P.
  void applyInstallUpdates(uint32_t pageId);
  // Rebuild hierarchy masks and install bits after uninstalling page P.
  void applyUninstallUpdates(uint32_t pageId);
  void uploadHierarchies();
  void workerLoop();
  void stopWorker();
  bool canInstallPage(uint32_t pageId, const PageDependencyGraph &pageDeps) const;
  DecodedPagePayload readPagePayload(uint32_t pageId) const;
  bool installDecodedPage(uint32_t pageId, const DecodedPagePayload &payload);

  // Write the PageTableEntry for localPageId into its global slot
  void writePageTableEntry(uint32_t localPageId, const PageAllocation &alloc);

  const VirtualGeometryFile *file_ = nullptr;
  rendering::RenderGraph *renderGraph_ = nullptr;
  rendering::BufferManager *pagesAllocator_ = nullptr;
  rendering::Buffer pagesBuffer_;
  rendering::Buffer hierarchyBuffer_;
  rendering::Buffer pageTableBuffer_;
  uint32_t pageTableOffset_ = 0;
  uint32_t *nextClusterOffset_ = nullptr;
  uint32_t nextHierarchyUploadId_ = 0u;

  std::vector<bool> installed_;
  std::vector<PageAllocation> allocations_;
  std::vector<HierarchyUpload> hierarchyUploads_;
  std::vector<uint8_t> pendingLoads_;
  std::vector<DecodedPagePayload> decodedPages_;
  std::vector<uint32_t> readyPageIds_;
  std::atomic<bool> stopWorkerRequested_{false};
  os::Thread loadWorker_;
  lib::ConcurrentQueue<PageLoadRequest> loadRequests_;
  lib::ConcurrentQueue<PageLoadResult> completedLoads_;
};

class VirtualGeometryScene
{
public:
  struct VirtualGeometryObjectRuntimeData
  {
    VirtualGeometryFile file;
    std::vector<VirtualGeometryShapeInfo> shapes;
    std::vector<MeshPartInfo> meshParts;
    rendering::animation::Skeleton skeleton;
    QuantizationConfig quantizationConfig;
    PageDependencyGraph pageDependencies;

    // Streaming manager: owns page allocations and applies all install/uninstall updates
    std::unique_ptr<VirtualGeometryStreamingManager> streamingManager;

    // Index of this object's first PageTableEntry in the scene-wide pageTable[].
    uint32_t pageTableOffset;
    AABB localBounds;

    VirtualGeometryObjectRuntimeData()
    {
    }
    VirtualGeometryObjectRuntimeData(std::string &filePath);
    void buildPageDependencyGraph();
  };

  struct InstanceManager
  {
    std::vector<VirtualGeometryInstanceGPUData> packedInstances;
    std::vector<uint32_t> sparseToDense;
    std::vector<SceneInstanceId> denseToSparse;
    std::vector<SceneInstanceId> freeList;
    std::vector<std::string> instanceToObjectName;
    bool isDirty;

    InstanceManager();
    SceneInstanceId allocateInstance(
        const std::string &objectName,
        const math::Vec3f &position,
        const math::Quatf &rotation,
        float scale,
        uint32_t hierarchyOffset,
        uint32_t pageTableOffset,
        uint32_t materialIndex,
        uint32_t meshPartTransformsOffset,
        const QuantizationConfig &quantConfig);
    bool deallocateInstance(SceneInstanceId instanceId);
    bool updateInstance(SceneInstanceId instanceId, const math::Vec3f &position, const math::Quatf &rotation, math::Vec3f scale);
    bool updateMaterial(SceneInstanceId instanceId, uint32_t materialIndex);
    const VirtualGeometryInstanceGPUData *getInstance(SceneInstanceId instanceId) const;
    const std::vector<VirtualGeometryInstanceGPUData> &getPackedData() const;
    size_t getInstanceCount() const;
    void clearDirtyFlag();
  };

  struct ObjectInstanceRecord
  {
    struct HierarchySlice
    {
      rendering::Allocation allocation{UINT64_MAX, 0u};
      uint32_t uploadId = UINT32_MAX;
    };

    std::string objectName;
    std::vector<SceneInstanceId> sceneInstanceIds;
    std::vector<HierarchySlice> hierarchySlices;
    rendering::Allocation meshPartTransformsAllocation{UINT64_MAX, 0u};
  };

  lib::ConcurrentHashMap<std::string, rendering::Camera *> cameras;
  lib::ConcurrentHashMap<std::string, VirtualGeometryObjectRuntimeData *> files;

  rendering::RenderGraph *renderGraph;
  rendering::BufferManager hierarchyBufferAllocator;
  rendering::BufferManager pagesBufferAllocator;
  rendering::BufferManager meshPartTransformsAllocator;
  VirtualTextureSystem virtualTextureSystem;

  InstanceManager instanceManager;
  std::vector<ObjectInstanceRecord> objectInstances;
  std::vector<uint32_t> objectSparseToDense;
  std::vector<InstanceId> objectDenseToSparse;
  std::vector<InstanceId> objectFreeList;
  uint64_t instanceBufferSize;
  uint32_t nextClusterOffset;
  uint32_t maxPages;

  // -------------------------------------------------------------------------
  // Global allocators for the page table and priority buffer slices.
  // Both counters advance monotonically; slots are never recycled while the
  // scene is alive (objects are long-lived and pages hold slots for their
  // object's lifetime).
  // -------------------------------------------------------------------------
  uint32_t nextPageTableSlot; // next free index in pageTable[]
  uint32_t nextPrioritySlot;  // next free index in pagePriorities[]

  // -------------------------------------------------------------------------
  // GPU buffers — public so render passes can bind them directly
  // -------------------------------------------------------------------------
  inline uint32_t getMaxPagesInScene()
  {
    return maxPages;
  }

  rendering::Buffer pageTableBuffer;
  rendering::Buffer hierarchyBuffer;
  rendering::Buffer pagesBuffer;
  rendering::Buffer instanceBuffer;
  rendering::Buffer pagePriorityBuffer;
  rendering::Buffer pageInstallCandidateBuffer;
  rendering::Buffer pageEvictCandidateBuffer;
  rendering::Buffer meshPartTransformsBuffer;

  uint64_t hierarchyBufferSize;
  uint64_t pagesBufferSize;
  uint64_t pagesTableBufferSize;
  uint64_t pagePriorityBufferSize;
  uint64_t pageInstallCandidateBufferSize;
  uint64_t pageEvictCandidateBufferSize;
  uint64_t meshPartTransformsBufferSize;

  static constexpr uint32_t STREAMING_PAGE_SELECTION_COUNT = 64u;
  static constexpr uint32_t MAX_VISIBLE_CLUSTERS = 1024u * 256u;
  static constexpr uint32_t MAX_VISIBLE_SHADOW_JOBS = MAX_VISIBLE_CLUSTERS;

  VirtualGeometryScene(rendering::RenderGraph *renderGraph, uint32_t hierarchyBufferSize, uint64_t pagesBufferSize);
  ~VirtualGeometryScene();

  void addCamera(std::string name, rendering::Camera camera);
  void registerObjectForStreaming(std::string name, std::string file);
  bool loadMaterialFile(uint32_t materialIndex, const std::string &materialPath);

  InstanceId instantiateObjectInstance(const std::string &objectName, const math::Vec3f &position, const math::Quatf &rotation, float scale = 1.0f, uint32_t materialIndexOverride = UINT32_MAX);
  bool destroyObjectInstance(InstanceId instanceId);
  bool updateObjectInstanceTransform(InstanceId instanceId, const math::Vec3f &position, const math::Quatf &rotation, float scale);
  bool updateObjectInstanceMaterial(InstanceId instanceId, uint32_t materialIndex);
  bool uploadBoneTransforms(InstanceId instanceId, const std::vector<math::Mat4f> &boneTransforms);
  bool uploadAnimationPlayerPose(InstanceId instanceId, const animation::AnimationPlayer &animationPlayer);
  bool applyAnimationFrame(InstanceId instanceId, animation::AnimationPlayer &animationPlayer, const std::string &animationName, float timeSeconds, bool looping = true);

  void updateInstanceBuffer(bool uploadAllFrameSlots = false);
  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const;
  void prepareFrame(uint32_t frameSlot);
  void updatePageStreaming(const std::string &objectName, uint32_t maxPageInstallsPerUpdate = UINT32_MAX);
  void setStreamingResidencyInvalidationCallback(std::function<void(const AABB &)> callback);

  size_t getInstanceCount() const;
  VirtualTextureSystem &getVirtualTextureSystem()
  {
    return virtualTextureSystem;
  }
  const VirtualTextureSystem &getVirtualTextureSystem() const
  {
    return virtualTextureSystem;
  }

  static uint32_t getGlobalClusterIndex(const PageTableEntry &pageEntry, uint32_t pageLocalClusterIndex);
  static std::pair<uint32_t, uint32_t> getPageClusterRange(const PageTableEntry &pageEntry);

private:
  void initializeFrameOverrideInstanceBuffers();
  void destroyFrameOverrideInstanceBuffers();
  rendering::BufferId getFrameOverrideInstanceBufferId(uint32_t frameSlot) const;
  rendering::BufferId getCurrentFrameOverrideInstanceBufferId() const;
  void writeInstanceRecordToCurrentBuffers(uint32_t denseIndex, const VirtualGeometryInstanceGPUData &instanceData);
  void writePackedInstanceDataToBuffers(const std::vector<VirtualGeometryInstanceGPUData> &packedData, bool uploadAllFrameSlots);
  void queueInstanceRecordUpdateForAllFrameSlots(uint32_t denseIndex);
  void clearPendingInstanceRecordUpdates(uint32_t frameSlot);

  std::function<void(const AABB &)> streamingResidencyInvalidationCallback_;
  std::vector<rendering::BufferId> frameOverrideInstanceBufferIds_;
  std::vector<std::vector<uint32_t>> pendingInstanceUpdateDenseIndicesPerFrame_;
  std::vector<std::vector<uint8_t>> pendingInstanceUpdateMasksPerFrame_;
};

} // namespace virtualgeometry
