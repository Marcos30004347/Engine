#pragma once
#include "datastructure/ConcurrentHashMap.hpp"
#include "datastructure/ConcurrentQueue.hpp"
#include "os/Thread.hpp"
#include "rendering/core/BufferManager.hpp"
#include "rendering/core/Camera.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/MaterialManager.hpp"
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
// Layout (176 bytes):
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
//   previousModelMatrix  — mat4x4<f32>  (64 bytes)
//   previousMeshPartTransformsOffset — u32 (4 bytes) previous bone palette base
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
  math::Mat4f previousModelMatrix;
  uint32_t previousMeshPartTransformsOffset;
  uint32_t _temporalFlags;
  uint32_t _padding2[2];
};
static_assert(sizeof(VirtualGeometryInstanceGPUData) == 176, "VirtualGeometryInstanceGPUData size mismatch");

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
  uint32_t unitScaleBits = 0u;
  uint32_t decodedVertexOffset = 0u;
  uint32_t decodedVertexCount = 0u;
  uint32_t decodedIndexOffset = 0u;
  uint32_t decodedIndexCount = 0u;
  uint32_t decodedBoneWeightOffset = 0u;
  uint32_t decodedBoneWeightCount = 0u;
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
  uint32_t unitScaleBits;
  uint32_t decodedVertexOffset;
  uint32_t decodedIndexOffset;
  uint32_t decodedBoneWeightOffset;
  uint32_t _padding0;
  uint32_t _padding1;
};
static_assert(sizeof(PageTableEntry) == 48, "PageTableEntry must be 48 bytes");

struct DecodedClusterCullData
{
  float localMinX;
  float localMinY;
  float localMinZ;
  uint32_t triangleCount;

  float localMaxX;
  float localMaxY;
  float localMaxZ;
  uint32_t _padding0;

  float selfCenterX;
  float selfCenterY;
  float selfCenterZ;
  float selfRadius;

  float parentCenterX;
  float parentCenterY;
  float parentCenterZ;
  float parentRadius;

  float coneAxisX;
  float coneAxisY;
  float coneAxisZ;
  float coneCutoff;

  float selfError;
  float parentError;
  uint32_t _padding1;
  uint32_t _padding2;
};
static_assert(sizeof(DecodedClusterCullData) == 96, "DecodedClusterCullData must be 96 bytes");

struct DecodedClusterGeometryInfo
{
  uint32_t vertexOffset;
  uint32_t indexOffset;
  uint32_t vertexCount;
  uint32_t triangleCount;
  uint32_t pageLocalGroupIndex;
  uint32_t clusterIndexInGroup;
  uint32_t _padding0;
  uint32_t _padding1;
};
static_assert(sizeof(DecodedClusterGeometryInfo) == 32, "DecodedClusterGeometryInfo must be 32 bytes");

struct DecodedVertexData
{
  float positionX;
  float positionY;
  float positionZ;
  uint32_t boneWeightOffset;

  float normalX;
  float normalY;
  float normalZ;
  uint32_t boneWeightCount;

  float uvX;
  float uvY;
  uint32_t _padding0;
  uint32_t _padding1;
};
static_assert(sizeof(DecodedVertexData) == 48, "DecodedVertexData must be 48 bytes");

struct DecodedBoneWeightData
{
  float weight;
  uint32_t boneIndex;
};
static_assert(sizeof(DecodedBoneWeightData) == 8, "DecodedBoneWeightData must be 8 bytes");

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

struct PageDecodeUniformsGPU
{
  uint32_t requestCount = 0u;
  uint32_t _padding0 = 0u;
  uint32_t _padding1 = 0u;
  uint32_t _padding2 = 0u;
};
static_assert(sizeof(PageDecodeUniformsGPU) == 16, "PageDecodeUniformsGPU must be 16 bytes");

struct DispatchIndirectArgsGPU
{
  uint32_t groupCountX = 1u;
  uint32_t groupCountY = 1u;
  uint32_t groupCountZ = 1u;
};
static_assert(sizeof(DispatchIndirectArgsGPU) == 12, "DispatchIndirectArgsGPU must be 12 bytes");

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
      rendering::BufferManager *clusterAllocator,
      rendering::BufferManager *decodedVertexAllocator,
      rendering::BufferManager *decodedIndexAllocator,
      rendering::BufferManager *decodedBoneWeightAllocator,
      rendering::Buffer pagesBuffer,
      rendering::Buffer hierarchyBuffer,
      rendering::Buffer pageTableBuffer,
      uint32_t pageTableOffset,
      std::function<void(uint32_t)> queuePageDecodeRequestCallback);
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
  rendering::BufferManager *clusterAllocator_ = nullptr;
  rendering::BufferManager *decodedVertexAllocator_ = nullptr;
  rendering::BufferManager *decodedIndexAllocator_ = nullptr;
  rendering::BufferManager *decodedBoneWeightAllocator_ = nullptr;
  rendering::Buffer pagesBuffer_;
  rendering::Buffer hierarchyBuffer_;
  rendering::Buffer pageTableBuffer_;
  uint32_t pageTableOffset_ = 0;
  uint32_t nextHierarchyUploadId_ = 0u;
  std::function<void(uint32_t)> queuePageDecodeRequestCallback_;

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
  struct SceneInstanceTransformUpdate
  {
    uint64_t sequence = 0u;
    uint32_t frameSlot = 0u;
    InstanceId objectInstanceId = INVALID_INSTANCE_ID;
    SceneInstanceId sceneInstanceId = INVALID_SCENE_INSTANCE_ID;
    uint32_t shapeIndex = UINT32_MAX;
    math::Mat4f previousTransform = math::Mat4f::identity();
    math::Mat4f transform = math::Mat4f::identity();
  };

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
        uint32_t previousMeshPartTransformsOffset,
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
    struct GPUSignedDistanceFieldsState
    {
      bool dynamic = false;
      uint64_t contentRevision = 1u;
      uint64_t rebuildRevision = 0u;
    };

    struct HierarchySlice
    {
      rendering::Allocation allocation{UINT64_MAX, 0u};
      uint32_t uploadId = UINT32_MAX;
    };

    std::string objectName;
    std::vector<SceneInstanceId> sceneInstanceIds;
    std::vector<HierarchySlice> hierarchySlices;
    rendering::Allocation meshPartTransformsAllocation{UINT64_MAX, 0u};
    uint32_t meshPartTransformsOffset = 0u;
    uint32_t previousMeshPartTransformsOffset = 0u;
    std::vector<math::Mat4f> currentMeshPartTransforms;
    std::vector<math::Mat4f> previousMeshPartTransforms;
    GPUSignedDistanceFieldsState gpuSignedDistanceFields;
  };

  lib::ConcurrentHashMap<std::string, rendering::Camera *> cameras;
  lib::ConcurrentHashMap<std::string, VirtualGeometryObjectRuntimeData *> files;

  rendering::RenderGraph *renderGraph;
  rendering::BufferManager hierarchyBufferAllocator;
  rendering::BufferManager pagesBufferAllocator;
  rendering::BufferManager clusterAllocator;
  rendering::BufferManager decodedVertexAllocator;
  rendering::BufferManager decodedIndexAllocator;
  rendering::BufferManager decodedBoneWeightAllocator;
  rendering::BufferManager meshPartTransformsAllocator;
  VirtualTextureSystem virtualTextureSystem;
  MaterialManager materialManager;

  InstanceManager instanceManager;
  std::vector<ObjectInstanceRecord> objectInstances;
  std::vector<uint32_t> objectSparseToDense;
  std::vector<InstanceId> objectDenseToSparse;
  std::vector<InstanceId> objectFreeList;
  uint64_t instanceBufferSize;
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
  rendering::Buffer decodedClusterCullBuffer;
  rendering::Buffer decodedClusterGeometryBuffer;
  rendering::Buffer decodedVertexBuffer;
  rendering::Buffer decodedBoneWeightBuffer;
  rendering::Buffer decodedIndexBuffer;
  rendering::Buffer instanceBuffer;
  rendering::Buffer pagePriorityBuffer;
  rendering::Buffer pageInstallCandidateBuffer;
  rendering::Buffer pageEvictCandidateBuffer;
  rendering::Buffer meshPartTransformsBuffer;
  rendering::Buffer pageDecodeUniformBuffer;
  rendering::Buffer pageDecodeRequestBuffer;
  rendering::Buffer pageDecodeDispatchIndirectBuffer;

  uint64_t hierarchyBufferSize;
  uint64_t pagesBufferSize;
  uint64_t decodedClusterCullBufferSize;
  uint64_t decodedClusterGeometryBufferSize;
  uint64_t decodedVertexBufferSize;
  uint64_t decodedBoneWeightBufferSize;
  uint64_t decodedIndexBufferSize;
  uint64_t pagesTableBufferSize;
  uint64_t pagePriorityBufferSize;
  uint64_t pageInstallCandidateBufferSize;
  uint64_t pageEvictCandidateBufferSize;
  uint64_t meshPartTransformsBufferSize;
  uint64_t pageDecodeUniformBufferSize;
  uint64_t pageDecodeRequestBufferSize;
  uint64_t pageDecodeDispatchIndirectBufferSize;

  static constexpr uint32_t STREAMING_PAGE_SELECTION_COUNT = 64u;
  static constexpr uint32_t MAX_VISIBLE_CLUSTERS = 1024u * 256u;
  static constexpr uint32_t MAX_DECODED_CLUSTERS = MAX_VISIBLE_CLUSTERS * 2u;
  static constexpr uint32_t MAX_CLUSTER_VERTICES = static_cast<uint32_t>(MaxVertices);
  static constexpr uint32_t MAX_CLUSTER_TRIANGLES = ClusterSize;
  static constexpr uint32_t MAX_CLUSTER_INDICES = MAX_CLUSTER_TRIANGLES * 3u;
  static constexpr uint32_t MAX_CLUSTER_BONE_WEIGHTS_PER_VERTEX = 4u;
  static constexpr uint32_t MAX_CLUSTERS_PER_PAGE = MAX_GROUPS_PER_PAGE * GroupSize;
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
  bool setObjectInstanceSdfDynamic(InstanceId instanceId, bool dynamic);
  bool isObjectInstanceSdfDynamic(InstanceId instanceId) const;
  bool requestObjectInstanceSdfRebuild(InstanceId instanceId);

  void updateInstanceBuffer(bool uploadAllFrameSlots = false);
  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const;
  void prepareFrame(uint32_t frameSlot);
  void updatePageStreaming(const std::string &objectName, uint32_t maxPageInstallsPerUpdate = UINT32_MAX);
  void setStreamingResidencyInvalidationCallback(std::function<void(const AABB &)> callback);
  const std::vector<SceneInstanceTransformUpdate> &getFrameTransformUpdates(uint32_t frameSlot) const;
  std::vector<SceneInstanceTransformUpdate> consumeFrameTransformUpdates(uint32_t frameSlot);
  void clearFrameTransformUpdates(uint32_t frameSlot);

  size_t getInstanceCount() const;
  VirtualTextureSystem &getVirtualTextureSystem()
  {
    return virtualTextureSystem;
  }
  const VirtualTextureSystem &getVirtualTextureSystem() const
  {
    return virtualTextureSystem;
  }
  MaterialManager &getMaterialManager()
  {
    return materialManager;
  }
  const MaterialManager &getMaterialManager() const
  {
    return materialManager;
  }

  static uint32_t getGlobalClusterIndex(const PageTableEntry &pageEntry, uint32_t pageLocalClusterIndex);
  static std::pair<uint32_t, uint32_t> getPageClusterRange(const PageTableEntry &pageEntry);
  void enqueuePageDecodeRequest(uint32_t globalPageIndex);
  std::vector<uint32_t> consumePendingPageDecodeRequests();

private:
  void initializeFrameOverrideInstanceBuffers();
  void destroyFrameOverrideInstanceBuffers();
  void initializeFrameOverrideMeshPartTransformsBuffers();
  void destroyFrameOverrideMeshPartTransformsBuffers();
  void initializeFrameOverridePageDecodeBuffers();
  void destroyFrameOverridePageDecodeBuffers();
  void updatePageDecodeBuffers(
      rendering::BufferId uniformOverrideId,
      rendering::BufferId requestOverrideId,
      rendering::BufferId dispatchOverrideId,
      bool clearPendingRequests);
  rendering::BufferId getFrameOverrideInstanceBufferId(uint32_t frameSlot) const;
  rendering::BufferId getCurrentFrameOverrideInstanceBufferId() const;
  rendering::BufferId getFrameOverrideMeshPartTransformsBufferId(uint32_t frameSlot) const;
  rendering::BufferId getCurrentFrameOverrideMeshPartTransformsBufferId() const;
  rendering::BufferId getFrameOverridePageDecodeUniformBufferId(uint32_t frameSlot) const;
  rendering::BufferId getFrameOverridePageDecodeRequestBufferId(uint32_t frameSlot) const;
  rendering::BufferId getFrameOverridePageDecodeDispatchIndirectBufferId(uint32_t frameSlot) const;
  void writeInstanceRecordToCurrentBuffers(uint32_t denseIndex, const VirtualGeometryInstanceGPUData &instanceData);
  void writePackedInstanceDataToBuffers(const std::vector<VirtualGeometryInstanceGPUData> &packedData, bool uploadAllFrameSlots);
  void queueInstanceRecordUpdateForAllFrameSlots(uint32_t denseIndex);
  void queueTransformUpdateForAllFrameSlots(const SceneInstanceTransformUpdate &update);
  void clearPendingInstanceRecordUpdates(uint32_t frameSlot);
  void writeMeshPartTransformsRecordToCurrentBuffers(uint32_t denseIndex);
  void queueMeshPartTransformsRecordUpdateForAllFrameSlots(uint32_t denseIndex);
  void clearPendingMeshPartTransformsRecordUpdates(uint32_t frameSlot);

  std::function<void(const AABB &)> streamingResidencyInvalidationCallback_;
  std::vector<rendering::BufferId> frameOverrideInstanceBufferIds_;
  std::vector<std::vector<uint32_t>> pendingInstanceUpdateDenseIndicesPerFrame_;
  std::vector<std::vector<uint8_t>> pendingInstanceUpdateMasksPerFrame_;
  std::vector<std::vector<SceneInstanceTransformUpdate>> pendingTransformUpdatesPerFrame_;
  uint64_t nextTransformUpdateSequence_ = 1u;
  std::vector<rendering::BufferId> frameOverrideMeshPartTransformsBufferIds_;
  std::vector<std::vector<uint32_t>> pendingMeshPartTransformsUpdateDenseIndicesPerFrame_;
  std::vector<std::vector<uint8_t>> pendingMeshPartTransformsUpdateMasksPerFrame_;
  std::vector<rendering::BufferId> frameOverridePageDecodeUniformBufferIds_;
  std::vector<rendering::BufferId> frameOverridePageDecodeRequestBufferIds_;
  std::vector<rendering::BufferId> frameOverridePageDecodeDispatchIndirectBufferIds_;
  std::vector<uint32_t> pendingPageDecodeRequests_;
};

} // namespace virtualgeometry
