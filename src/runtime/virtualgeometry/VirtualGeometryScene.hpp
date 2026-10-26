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
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <map>
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
  // Sparse scene-instance id of the owning object; INVALID_SCENE_INSTANCE_ID
  // when the object draws through several instances. The software-raster
  // resolve maps it through instanceSparseToDenseBuffer per frame.
  uint32_t ownerSparseInstance = UINT32_MAX;
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
  // DECODED_CLUSTER_FLAG_* for pages whose leaf carries
  // HIERARCHY_DECODED_CLUSTER_FLAGS_FLAG; zero for decoded encoded pages.
  uint32_t flags;
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
      std::function<void(uint32_t)> queuePageDecodeRequestCallback, bool compressedGeometry = false);
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
  bool compressedGeometry_ = false;
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

  using RuntimeObjectId = uint32_t;
  static constexpr RuntimeObjectId INVALID_RUNTIME_OBJECT_ID = UINT32_MAX;

  struct RuntimeObjectCreateInfo
  {
    std::string name;
    uint32_t pageCount = 0u;
    std::vector<VirtualGeometryHierarchy> hierarchy;
    QuantizationConfig quantizationConfig{};
    AABB localBounds{};
    math::Vec3f position{0.0f, 0.0f, 0.0f};
    math::Quatf rotation = math::Quatf::identity();
    float scale = 1.0f;
    uint32_t materialIndex = 0u;
  };

  struct RuntimePageInstallInfo
  {
    uint32_t localPageIndex = UINT32_MAX;
    uint32_t parentLocalPageIndex = UINT32_MAX;
    uint32_t fallbackLocalPageIndex = UINT32_MAX;
    uint64_t pageGeneration = 0u;
    uint64_t geometryRevision = 0u;
    uint64_t materialRevision = 0u;
    PageAllocation pageAllocation{};
    PageTableEntry pageTableEntry{};
    bool rootPage = false;
    bool hasTracePayload = false;
    bool hasMaterialPayload = false;
    bool requireProtectedParentOrFallback = false;
    bool writesSceneEncodedPageData = false;
    bool enqueuePageDecode = true;
    bool ownsSceneBufferAllocation = false;
    bool overrideHierarchyLeafMetadata = false;
    std::array<float, 3u> hierarchyBoundsMin{};
    std::array<float, 3u> hierarchyBoundsMax{};
    float hierarchySelfError = 0.0f;
    float hierarchyParentError = 0.0f;
    // With overrideHierarchyLeafMetadata: the sphere the leaf's parent test
    // projects (max_center/max_radius) is the parent page's bounds rather than
    // the page's own, so a child page and its parent's clusters evaluate the
    // identical sphere and error and the cut never draws both or neither.
    bool hierarchyParentSphereValid = false;
    std::array<float, 3u> hierarchyParentBoundsMin{};
    std::array<float, 3u> hierarchyParentBoundsMax{};
    // The page's clusters carry their streaming-leaf state in the decoded
    // cull data (HIERARCHY_DECODED_CLUSTER_FLAGS_FLAG on the leaf).
    bool decodedClusterFlags = false;
    std::vector<uint32_t> encodedPageWords;
    // Local page indices this page needs resident (the builder's page
    // dependencies). A page is never evicted while an installed page depends
    // on it, which is what keeps the replayed install rewrites consistent.
    std::vector<uint32_t> dependencies;
    // Per-page hierarchy rewrites, as produced by the offline page builder.
    //
    // These are what make a partially resident DAG renderable: a cluster is a
    // streaming leaf only where the finer geometry it stands in for is not
    // resident yet. Leaving them empty falls back to marking every cluster on
    // the page a streaming leaf, which forces its self error to zero in the
    // culler - the cluster then draws at every distance and the parent test is
    // skipped entirely, so every LOD level draws at once and the cut stops
    // existing. Supply them whenever the asset has them.
    std::vector<HierarchyClusterFlagsUpdate> hierarchyClusterFlagUpdates;
    // Where to look for the leaf that names this page. The install marks that
    // leaf installed by scanning the object's hierarchy for it; on an object
    // built from one asset that is the whole hierarchy and cheap. A shared
    // terrain object holds hundreds of chunks - 884K nodes - and a page's leaf
    // can only be inside its own chunk's slice, so scanning everything cost
    // 444-654 ms per apply. Empty means the whole hierarchy.
    struct HierarchyScanRange
    {
      uint32_t firstNode = 0u;
      uint32_t nodeCount = 0u;
    };
    std::vector<HierarchyScanRange> hierarchyScanRanges;
  };

  struct RuntimeEncodedPageInstallInfo
  {
    uint32_t localPageIndex = UINT32_MAX;
    uint32_t parentLocalPageIndex = UINT32_MAX;
    uint32_t fallbackLocalPageIndex = UINT32_MAX;
    uint64_t pageGeneration = 0u;
    uint64_t geometryRevision = 0u;
    uint64_t materialRevision = 0u;
    uint32_t unitScaleBits = 0u;
    bool rootPage = false;
    bool hasTracePayload = false;
    bool hasMaterialPayload = false;
    bool requireProtectedParentOrFallback = false;
    bool enqueuePageDecode = true;
    // The page's words, or where to read them (phase 8g). With `location`
    // valid and `encodedPageWords` empty the scene reads the page itself; with
    // both, the words are used and the location is kept. Either way a valid
    // location is recorded on the page and survives an uninstall, so an
    // eviction is reversible by the scene alone.
    std::vector<uint32_t> encodedPageWords;
    VirtualGeometryPageLocation location{};
    std::vector<uint32_t> dependencies;
    std::vector<HierarchyClusterFlagsUpdate> hierarchyClusterFlagUpdates;
    std::vector<RuntimePageInstallInfo::HierarchyScanRange> hierarchyScanRanges;
  };

  struct RuntimeEncodedObjectInstallInfo
  {
    std::string name;
    const VirtualGeometryEncodedData *asset = nullptr;
    uint64_t basePageGeneration = 1u;
    uint64_t geometryRevision = 1u;
    uint64_t materialRevision = 1u;
    math::Vec3f position{0.0f, 0.0f, 0.0f};
    math::Quatf rotation = math::Quatf::identity();
    float scale = 1.0f;
    uint32_t materialIndex = 0u;
    bool hasTracePayload = false;
    bool hasMaterialPayload = false;
    bool enqueuePageDecode = true;
  };

  struct RuntimeEncodedObjectInstallStats
  {
    RuntimeObjectId objectId = INVALID_RUNTIME_OBJECT_ID;
    uint32_t installedPageCount = 0u;
    uint32_t queuedDecodePageCount = 0u;
    uint32_t encodedMeshletCount = 0u;
    uint64_t encodedPageBytes = 0u;
  };

  struct RuntimeGeneratedPageReserveInfo
  {
    std::string objectName;
    uint32_t localPageIndex = UINT32_MAX;
    uint32_t parentLocalPageIndex = UINT32_MAX;
    uint32_t fallbackLocalPageIndex = UINT32_MAX;
    uint64_t pageGeneration = 0u;
    uint64_t geometryRevision = 0u;
    uint64_t materialRevision = 0u;
    uint32_t unitScaleBits = 0u;
    uint32_t encodedPageBytes = 0u;
    uint32_t clusterCount = 0u;
    uint32_t decodedVertexCount = 0u;
    uint32_t decodedIndexCount = 0u;
    uint32_t decodedBoneWeightCount = 0u;
    bool overrideHierarchyLeafMetadata = false;
    std::array<float, 3u> hierarchyBoundsMin{};
    std::array<float, 3u> hierarchyBoundsMax{};
    float hierarchySelfError = 0.0f;
    float hierarchyParentError = 0.0f;
    // With overrideHierarchyLeafMetadata: the sphere the leaf's parent test
    // projects (max_center/max_radius) is the parent page's bounds rather than
    // the page's own, so a child page and its parent's clusters evaluate the
    // identical sphere and error and the cut never draws both or neither.
    bool hierarchyParentSphereValid = false;
    std::array<float, 3u> hierarchyParentBoundsMin{};
    std::array<float, 3u> hierarchyParentBoundsMax{};
    // The page's clusters carry their streaming-leaf state in the decoded
    // cull data (HIERARCHY_DECODED_CLUSTER_FLAGS_FLAG on the leaf).
    bool decodedClusterFlags = false;
    bool rootPage = false;
    bool hasTracePayload = false;
    bool hasMaterialPayload = false;
    bool requireProtectedParentOrFallback = false;
  };

  struct RuntimeGeneratedPageReservation
  {
    std::string objectName;
    uint32_t localPageIndex = UINT32_MAX;
    uint32_t globalPageTableIndex = UINT32_MAX;
    uint32_t parentLocalPageIndex = UINT32_MAX;
    uint32_t fallbackLocalPageIndex = UINT32_MAX;
    uint64_t pageGeneration = 0u;
    uint64_t geometryRevision = 0u;
    uint64_t materialRevision = 0u;
    PageAllocation pageAllocation{};
    PageTableEntry pageTableEntry{};
    bool overrideHierarchyLeafMetadata = false;
    std::array<float, 3u> hierarchyBoundsMin{};
    std::array<float, 3u> hierarchyBoundsMax{};
    float hierarchySelfError = 0.0f;
    float hierarchyParentError = 0.0f;
    // With overrideHierarchyLeafMetadata: the sphere the leaf's parent test
    // projects (max_center/max_radius) is the parent page's bounds rather than
    // the page's own, so a child page and its parent's clusters evaluate the
    // identical sphere and error and the cut never draws both or neither.
    bool hierarchyParentSphereValid = false;
    std::array<float, 3u> hierarchyParentBoundsMin{};
    std::array<float, 3u> hierarchyParentBoundsMax{};
    // The page's clusters carry their streaming-leaf state in the decoded
    // cull data (HIERARCHY_DECODED_CLUSTER_FLAGS_FLAG on the leaf).
    bool decodedClusterFlags = false;
    bool rootPage = false;
    bool hasTracePayload = false;
    bool hasMaterialPayload = false;
    bool requireProtectedParentOrFallback = false;

    bool valid() const
    {
      return !objectName.empty() &&
          localPageIndex != UINT32_MAX &&
          globalPageTableIndex != UINT32_MAX &&
          pageGeneration != 0u &&
          geometryRevision != 0u &&
          pageAllocation.pageId == localPageIndex &&
          pageAllocation.isInstalled &&
          pageTableEntry.isInstalled != 0u;
    }
  };

  struct RuntimeGeneratedPageInstallInfo
  {
    RuntimeGeneratedPageReservation reservation{};
    bool gpuWroteEncodedPageData = true;
    bool gpuWroteDecodedGeometry = false;
    bool enqueuePageDecode = true;
  };

  struct RuntimePageStatus
  {
    bool installed = false;
    // How many installed pages list this one as a dependency. Kept so the
    // "may this page be uninstalled" test is a counter read instead of a walk
    // over every page of the object - 8192 of them on the shared terrain
    // object, per page, per retire.
    uint32_t dependedOnBy = 0u;
    // The hierarchy ranges this page's leaf can live in, kept from the install
    // so an uninstall can rebuild masks over just that scope. Empty means the
    // page belongs to a one-asset object and the whole hierarchy is its scope.
    std::vector<RuntimePageInstallInfo::HierarchyScanRange> hierarchyScanRanges;
    // Masks a caller applied to this page's nodes after the install, kept
    // separate from `installUpdates` so that list keeps meaning "what the
    // builder emitted for this page" - the replay and the install path both
    // read `installUpdates.empty()` to decide a node's *default* mask, so
    // folding external masks into it silently flips that default. Replayed
    // after the page's own updates.
    std::vector<HierarchyClusterFlagsUpdate> externalUpdates;
    bool rootPage = false;
    bool hasTracePayload = false;
    bool hasMaterialPayload = false;
    bool sceneEncodedPageDataReady = false;
    bool ownsSceneBufferAllocation = false;
    uint32_t localPageIndex = UINT32_MAX;
    uint32_t globalPageTableIndex = UINT32_MAX;
    uint32_t prioritySlot = UINT32_MAX;
    uint32_t parentLocalPageIndex = UINT32_MAX;
    uint32_t fallbackLocalPageIndex = UINT32_MAX;
    uint64_t pageGeneration = 0u;
    uint64_t geometryRevision = 0u;
    uint64_t materialRevision = 0u;
    PageAllocation pageAllocation{};
    PageTableEntry pageTableEntry{};
    // Frame this page was last installed, or last seen wanted by the culler.
    // LRU eviction takes the smallest value first.
    uint64_t lastUsedFrame = 0u;
    // Kept across an uninstall: rebuilding the object's hierarchy masks after
    // a page leaves replays the install rewrites of the pages that remain.
    std::vector<HierarchyClusterFlagsUpdate> installUpdates;
    std::vector<uint32_t> dependencies;
    // Phase 8g: everything a re-read of this page needs that the status does
    // not already keep. Set by an install that carries a location (or by
    // `primeRuntimeObjectPageSource` for a page not installed yet), kept across
    // an uninstall like `installUpdates`, cleared by an install that supplies
    // words with no location and by `forgetRuntimeObjectPageSource`.
    struct Reinstall
    {
      VirtualGeometryPageLocation location{};
      uint64_t pageGeneration = 0u;
      uint64_t geometryRevision = 0u;
      uint64_t materialRevision = 0u;
      uint32_t unitScaleBits = 0u;
      uint32_t parentLocalPageIndex = UINT32_MAX;
      uint32_t fallbackLocalPageIndex = UINT32_MAX;
      bool rootPage = false;
      bool hasTracePayload = false;
      bool hasMaterialPayload = false;
      bool requireProtectedParentOrFallback = false;
      bool enqueuePageDecode = true;
      std::vector<RuntimePageInstallInfo::HierarchyScanRange> hierarchyScanRanges;

      bool valid() const { return location.valid(); }
    };
    Reinstall reinstall;
  };

  struct RuntimeObjectDiagnostics
  {
    bool registered = false;
    bool usesCPUCulling = false;
    RuntimeObjectId objectId = INVALID_RUNTIME_OBJECT_ID;
    SceneInstanceId sceneInstanceId = INVALID_SCENE_INSTANCE_ID;
    uint32_t pageTableOffset = UINT32_MAX;
    uint32_t pageCount = 0u;
    uint32_t prioritySlotOffset = UINT32_MAX;
    uint32_t hierarchyStartOffset = UINT32_MAX;
    uint32_t hierarchyNodeCount = 0u;
    uint32_t installedPageCount = 0u;
    uint32_t traceablePageCount = 0u;
    uint32_t materialPageCount = 0u;
    uint32_t sceneEncodedPageCount = 0u;
    uint32_t queuedDecodeRequestCount = 0u;
    uint64_t latestGeometryRevision = 0u;
  };

  struct RuntimeObjectPageRequest
  {
    uint32_t localPageIndex = UINT32_MAX;
    uint32_t globalPageTableIndex = UINT32_MAX;
    uint32_t prioritySlot = UINT32_MAX;
    uint32_t priority = 0u;
  };

  lib::ConcurrentHashMap<std::string, rendering::Camera *> cameras;
  lib::ConcurrentHashMap<std::string, VirtualGeometryObjectRuntimeData *> files;

  struct Settings
  {
    bool metalExportableBuffers = false;
    // Decoded geometry capacity, in elements. Zero derives it from the pages
    // buffer, which is right for encoded assets: their decoded size follows
    // the bytes they stream. A producer that writes decoded geometry directly
    // (GPU-generated terrain) streams no encoded bytes at all, so its pages
    // buffer is dead weight and its decoded need is unrelated to it.
    uint64_t decodedVertexCapacity = 0u;
    uint64_t decodedIndexCapacity = 0u;
    // Bone weights are only decoded for skinned clusters. Zero derives the
    // capacity from the pages buffer (466 MiB of pages made a 466 MiB bone
    // pool); a scene without skinning passes a small non-zero value.
    uint64_t decodedBoneWeightCapacity = 0u;
    // Instances the scene can hold (0 = 1M, the historical default: 176 MiB
    // held once per frame slot besides the main buffer).
    uint32_t instanceCapacity = 0u;
    // Stage live page/hierarchy changes before GPU decode and use
    // nonblocking page-source reads when the source supports them.
    bool asyncRuntimeUploads = false;
    // Keep encoded page geometry resident; installation expands only culling metadata.
    bool compressedGeometry = false;
  };

  rendering::RenderGraph *renderGraph;
  Settings settings{};
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
  //
  // Slot ranges are recycled: a runtime object whose geometry is rebuilt is
  // unregistered and registered again, and terrain does that continuously as
  // its pages refine. Monotonic counters exhausted the table within a couple
  // of minutes and forced a whole render-graph recompile to reclaim it, which
  // is the stall this allocator removes. The counters remain as high-water
  // marks - the culler clears priorities over [0, nextPageTableSlot).
  // -------------------------------------------------------------------------
  rendering::BufferManager pageTableSlotAllocator;
  rendering::BufferManager prioritySlotAllocator;
  uint32_t nextPageTableSlot; // high-water mark in pageTable[]
  uint32_t nextPrioritySlot;  // high-water mark in pagePriorities[]

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
  // Two bits per decoded cluster slot, persistent across frames:
  //   bit 0 - the cluster was drawn last frame. Phase one of the two-pass
  //           occlusion cut draws exactly this set with the current camera.
  //   bit 1 - the cluster has been drawn this frame. Phase two skips it.
  // A per-frame kernel shifts bit 1 into bit 0 and clears bit 1, so a stale bit
  // can only ever cause an extra phase-one draw (self-correcting), never a
  // phase-two skip of something that was not drawn (which would be a hole).
  rendering::Buffer clusterVisibilityBuffer;
  // Sparse instance id -> dense index, refreshed each prepareFrame; see the
  // creation-site note in VirtualGeometryScene.cpp.
  rendering::Buffer instanceSparseToDenseBuffer;
  uint64_t instanceSparseToDenseBufferSize = 0u;
  uint64_t clusterVisibilityBufferSize = 0u;
  static constexpr uint32_t CLUSTERS_PER_VISIBILITY_WORD = 16u;
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
  static_assert(
      MAX_DECODED_CLUSTERS <= VisibilityPayloadClusterMask + 1u,
      "the visibility payload cannot address every decoded cluster slot");
  static constexpr uint32_t MAX_CLUSTER_VERTICES = static_cast<uint32_t>(MaxVertices);
  static constexpr uint32_t MAX_CLUSTER_TRIANGLES = ClusterSize;
  static constexpr uint32_t MAX_CLUSTER_INDICES = MAX_CLUSTER_TRIANGLES * 3u;
  static constexpr uint32_t MAX_CLUSTER_BONE_WEIGHTS_PER_VERTEX = 4u;
  static constexpr uint32_t MAX_CLUSTERS_PER_PAGE = MAX_GROUPS_PER_PAGE * GroupSize;
  static constexpr uint32_t MAX_VISIBLE_SHADOW_JOBS = MAX_VISIBLE_CLUSTERS;

  VirtualGeometryScene(
      rendering::RenderGraph *renderGraph,
      uint32_t hierarchyBufferSize,
      uint64_t pagesBufferSize);
  VirtualGeometryScene(
      rendering::RenderGraph *renderGraph,
      uint32_t hierarchyBufferSize,
      uint64_t pagesBufferSize,
      Settings settings);
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
  void recordRuntimeUploads(rendering::CommandRunContext &context);

  void updatePageStreaming(const std::string &objectName, uint32_t maxPageInstallsPerUpdate = UINT32_MAX);
  void setStreamingResidencyInvalidationCallback(std::function<void(const AABB &)> callback);
  bool registerRuntimeObject(const RuntimeObjectCreateInfo &createInfo, RuntimeObjectId *outObjectId = nullptr, std::string *error = nullptr);
  bool installRuntimeEncodedObject(const RuntimeEncodedObjectInstallInfo &install, RuntimeEncodedObjectInstallStats *stats = nullptr, std::string *error = nullptr);
  bool reserveRuntimeGeneratedPage(const RuntimeGeneratedPageReserveInfo &reserve, RuntimeGeneratedPageReservation &reservation, std::string *error = nullptr);
  bool installRuntimeGeneratedPage(const RuntimeGeneratedPageInstallInfo &install, std::string *error = nullptr);
  bool cancelRuntimeGeneratedPageReservation(const RuntimeGeneratedPageReservation &reservation, std::string *error = nullptr);
  bool installRuntimeObjectPage(const std::string &objectName, const RuntimePageInstallInfo &install, std::string *error = nullptr);
  bool installRuntimeObjectEncodedPage(const std::string &objectName, const RuntimeEncodedPageInstallInfo &install, std::string *error = nullptr);
  bool uninstallRuntimeObjectPage(const std::string &objectName, uint32_t localPageIndex, uint64_t requiredPageGeneration = 0u, std::string *error = nullptr);

  // Phase 8g: a runtime page as a location the scene reads itself.
  //
  // Records where a page not yet installed can be read from, with the install
  // parameters a later install needs (`install.encodedPageWords` is ignored;
  // `install.location` must be valid). The page stays uninstalled; a request
  // for it is then served by `reinstallRequestedRuntimePagesFromSource` with no
  // caller involvement. Refused for an installed page - its install records
  // its own location.
  bool primeRuntimeObjectPageSource(const std::string &objectName, const RuntimeEncodedPageInstallInfo &install, std::string *error = nullptr);
  // Installs a page that is not installed from the location recorded for it,
  // reading the words through the source. Dependencies that are not installed
  // are installed first, from their own locations, in order - an eviction may
  // have taken them too. Fails, with the other pages untouched, when the page
  // is installed, has no recorded location, or the source refuses the read.
  bool reinstallRuntimeObjectPageFromSource(const std::string &objectName, uint32_t localPageIndex, std::string *error = nullptr);
  // The same over the culler's page requests for one object, in the order
  // given, at most `budget` page installs counting dependencies. Requests for
  // installed pages and for pages with no recorded location are skipped, so a
  // caller that also serves pages another way runs this first and takes what
  // is left. Stops at the first pool-capacity failure: the next page would
  // fail the same way. Returns the pages installed; `failures` counts the
  // requests that could not be served.
  uint32_t reinstallRequestedRuntimePagesFromSource(const std::string &objectName, const std::vector<RuntimeObjectPageRequest> &requests, uint32_t budget, uint32_t *failures = nullptr);
  // Drops the recorded location of one page, or of every page that reads from
  // `source`. A retired chunk's page slots and a source about to close must be
  // forgotten, or a later request could read a location that is no longer
  // that page.
  void forgetRuntimeObjectPageSource(const std::string &objectName, uint32_t localPageIndex);
  void forgetRuntimePageSource(const VirtualGeometryPageSource *source);
  // Pages of the object that are not installed and carry a location.
  uint32_t countReinstallableRuntimePages(const std::string &objectName) const;
  struct RuntimePageSourceStats
  {
    uint64_t reads = 0u;
    uint64_t readFailures = 0u;
    double readMilliseconds = 0.0;
    uint64_t reinstalls = 0u;
    uint64_t reinstallFailures = 0u;
  };
  RuntimePageSourceStats getRuntimePageSourceStats() const { return runtimePageSourceStats_; }
  // Rewrites the hierarchy leaf of a page that is not installed, so the culler
  // can request it: bounds (frustum), the sphere and error of its parent test,
  // and the flags. `requestable == false` parks the leaf (parent error zero:
  // never traversed, never requested). Used by GPU-generated terrain, whose
  // page set changes at runtime while the object stays registered.
  struct RuntimePageLeafInfo
  {
    std::array<float, 3u> boundsMin{};
    std::array<float, 3u> boundsMax{};
    std::array<float, 3u> parentSphereBoundsMin{};
    std::array<float, 3u> parentSphereBoundsMax{};
    float selfError = 0.0f;
    float parentError = 0.0f;
    bool rootPage = false;
    bool requestable = true;
    bool decodedClusterFlags = true;
  };
  bool updateRuntimeObjectPageLeaf(const std::string &objectName, uint32_t localPageIndex, const RuntimePageLeafInfo &leaf, std::string *error = nullptr);
  // The same rewrite for many pages under one buffer upload. A page set that
  // maps a streaming directory onto slots republishes hundreds of leaves in
  // one commit, and one upload each cost seconds of scene update.
  bool updateRuntimeObjectPageLeaves(
      const std::string &objectName,
      const std::vector<std::pair<uint32_t, RuntimePageLeafInfo>> &leaves,
      std::string *error = nullptr);
  // Releases every resource the object holds - installed pages, hierarchy
  // nodes, page-table and priority slots, and its scene instance - so the name
  // can be registered again with different geometry. This is what lets a
  // rebuilt terrain chunk reach the screen without recompiling the graph.
  // Writes a contiguous run of hierarchy nodes into an object's hierarchy
  // allocation, updating the CPU mirror the cluster-flag rewrites patch and
  // uploading only that run.
  //
  // Registration sizes an object's hierarchy once and uploads it whole; every
  // later change is a single node patched by a rewrite. Terrain needs a third
  // shape: one object holds many chunks, and a chunk arrives with its own DAG
  // of tens of nodes that has to land in the slice reserved for it. Indices
  // inside `nodes` (child_start) are the caller's responsibility - they are
  // object-local, so the caller shifts them by `firstNode` before calling.
  bool writeRuntimeObjectHierarchyRange(
      const std::string &objectName,
      uint32_t firstNode,
      const std::vector<VirtualGeometryHierarchy> &nodes,
      std::string *error = nullptr);
  // Node count the object's hierarchy allocation holds.
  uint32_t runtimeObjectHierarchyNodeCount(const std::string &objectName) const;
  // Change a traversal gate without overwriting page-install patches in the
  // live node (cluster offsets, residency and enabled masks).
  bool setRuntimeObjectHierarchyParentError(
      const std::string &objectName, uint32_t nodeIndex, float parentError,
      std::string *error = nullptr);
  // Diagnostic: the CPU mirror of an object's hierarchy, which is what every
  // install and rewrite patches and what is uploaded to the GPU.
  // Diagnostic: how many installed pages list this one as a dependency.
  uint32_t debugRuntimePageDependedOnBy(const std::string &objectName, uint32_t localPageIndex) const;
  // Diagnostic: installed pages whose dependency list names this one.
  std::vector<uint32_t> debugRuntimePageDependers(const std::string &objectName, uint32_t localPageIndex) const;
  bool debugReadRuntimeObjectHierarchy(
      const std::string &objectName,
      std::vector<VirtualGeometryHierarchy> &out) const;
  // Applies cluster-flag rewrites to an object outside a page install.
  //
  // A page install carries the rewrites its own asset produced. Terrain needs
  // to restate a *different* chunk's masks when one of its children arrives or
  // leaves - the coarse chunk stops, or resumes, being the streaming boundary
  // over that quadrant - and that has no page install to ride along with.
  // Page-buffer bytes freed but not yet reusable: they belong to pages this
  // frame uninstalled, and the allocator can only hand them out once the
  // frame's GPU work retires. An install attempted against them fails with
  // "retry next frame", which is harmless for a caller that is adding
  // geometry and destructive for one that has already thrown geometry away in
  // order to replace it.
  uint64_t deferredRuntimePageReleaseBytes() const
  {
    return deferredPagesBufferBytes_;
  }

  // Whether all four pools could take a whole asset right now, counting only
  // memory that is actually reusable this frame.
  //
  // A caller about to destroy geometry in order to replace it must ask first.
  // An install that fails here fails with "buffers cannot fit the encoded page
  // (N bytes await deferred release; retry next frame)" - the bytes it needs
  // are the ones the caller just freed, which do not come back until the
  // frame's GPU work retires - and by then the ground being replaced is gone.
  //
  // All four pools, because the page buffer is rarely the one that fills: the
  // install also takes a cluster slot per meshlet and decoded vertex and index
  // capacity, and asking about page bytes alone answers yes when the decoded
  // pools are full. That is why the first version of this gate never fired.
  bool canFitRuntimeAsset(
      const uint64_t encodedPageBytes,
      const uint32_t meshletCount,
      const uint64_t decodedVertexCount,
      const uint64_t decodedIndexCount) const
  {
    return pagesBufferAllocator.canAllocate(encodedPageBytes) &&
        clusterAllocator.canAllocate(meshletCount) &&
        decodedVertexAllocator.canAllocate(decodedVertexCount) &&
        decodedIndexAllocator.canAllocate(decodedIndexCount);
  }

  bool applyRuntimeObjectHierarchyClusterFlags(
      const std::string &objectName,
      const std::vector<HierarchyClusterFlagsUpdate> &updates,
      std::string *error = nullptr,
      // Also record each mask against the page that owns its node, so it
      // survives `rebuildRuntimeObjectHierarchyMasks` - which rebuilds the
      // object from `pages[].installUpdates` and drops anything written only
      // into the mirror. Required for any mask maintaining a relation the
      // builder could not express in a page's own rewrites.
      bool persistIntoPageUpdates = false);

  bool unregisterRuntimeObject(const std::string &objectName, std::string *error = nullptr);
  bool isRuntimeObjectRegistered(const std::string &objectName) const;
  // Frame-safe replacement. Every scene buffer is host-written in place, so a
  // frame still executing on the GPU sees a page-table entry cleared, a
  // hierarchy range overwritten, or a cluster slot reused the moment the CPU
  // retires an object - the replaced chunk is missing for that frame. With a
  // delay of N frames, unregistering and uninstalling keep the GPU-visible
  // resources intact and release them N streaming frames later, instance
  // layout changes reach each frame slot through prepareFrame, and new objects
  // are allocated in fresh ranges. Zero (the default) releases immediately.
  // Set it to the render graph's frames in flight.
  void setRuntimeReleaseDelayFrames(uint32_t frames);
  uint32_t runtimeReleaseDelayFrames() const { return runtimeReleaseDelayFrames_; }
  // Releases every deferred resource whose frame has come; `force` releases
  // all of them. Called by setRuntimeStreamingFrame and prepareFrame.
  uint32_t drainDeferredRuntimeReleases(bool force = false);
  size_t pendingDeferredRuntimeReleaseCount() const { return deferredRuntimeReleases_.size(); }
  // Diagnostic: reads back the instance records a frame slot's override
  // buffer currently holds (the buffer the culler binds for that slot).
  bool debugReadFrameOverrideInstances(uint32_t frameSlot, std::vector<VirtualGeometryInstanceGPUData> &out) const;
  // Advances the clock LRU eviction order is measured against. Call once per
  // rendered frame.
  void setRuntimeStreamingFrame(uint64_t frame);
  uint64_t runtimeStreamingFrame() const { return runtimeStreamingFrame_; }
  // Uninstalls runtime pages, least recently used first, until the page buffer
  // can satisfy `bytes`. Root pages and pages protecting an installed child
  // are never evicted. Returns how many were uninstalled.
  uint32_t evictLeastRecentlyUsedRuntimePages(uint64_t bytes, uint32_t maximumEvictions = UINT32_MAX);
  // Uninstalls runtime pages, least recently used first, until every scene
  // pool (encoded pages, clusters, decoded vertices, decoded indices) has at
  // least `freeFraction` of its capacity free, counting releases still
  // deferred as free. Same protections as evictLeastRecentlyUsedRuntimePages.
  // Exhausted pools used to freeze the cut silently: installs failed with a
  // warning and the culler kept asking for the same pages.
  uint32_t evictLeastRecentlyUsedRuntimePagesForHeadroom(double freeFraction, uint32_t maximumEvictions = UINT32_MAX);
  struct RuntimePoolUsage
  {
    uint64_t pagesUsedBytes = 0u, pagesTotalBytes = 0u, pagesDeferredBytes = 0u;
    uint64_t clustersUsed = 0u, clustersTotal = 0u, clustersDeferred = 0u;
    uint64_t decodedVerticesUsed = 0u, decodedVerticesTotal = 0u, decodedVerticesDeferred = 0u;
    uint64_t decodedIndicesUsed = 0u, decodedIndicesTotal = 0u, decodedIndicesDeferred = 0u;
  };
  RuntimePoolUsage getRuntimePoolUsage() const;
  // Stamps the current streaming frame on every installed runtime page the
  // culler asked for this frame. This is what makes eviction order "least
  // recently *used*" rather than "least recently installed". Returns how many
  // pages were touched.
  uint32_t touchRuntimePagesFromGPUPriorities(const rendering::Buffer *completedReadback = nullptr);
  // Hierarchy nodes are allocated per object, not per page, so relieving
  // pressure there means retiring whole objects. Order is the most recent frame
  // any of an object's pages was used, ascending. Returns how many were
  // unregistered.
  uint32_t evictLeastRecentlyUsedRuntimeObjects(uint64_t hierarchyBytes, uint32_t maximumEvictions = UINT32_MAX);
  bool getRuntimeObjectDiagnostics(const std::string &objectName, RuntimeObjectDiagnostics &diagnostics) const;
  const RuntimePageStatus *getRuntimeObjectPageStatus(const std::string &objectName, uint32_t localPageIndex) const;
  // priorityReadback, when valid, is a frame-local Pull copy of
  // pagePriorityBuffer written by a CopyBufferPass after final culling.
  // Reading the live buffer instead races the GPU's per-frame reset and
  // re-accumulation, so half the reads land mid-reset and see zeros.
  // minPriority filters the collected pages; the culler publishes explicit
  // geometry demand in a high band (PAGE_PRIORITY_GEOMETRY_DEMAND in
  // virtualgeometrydata.wgsl), so a caller interested only in that demand
  // passes the band as the floor and the visit-driven noise on installed
  // pages stays out of the result.
  std::vector<RuntimeObjectPageRequest> collectRuntimeObjectPageRequests(
      const std::string &objectName,
      uint32_t maxRequests = STREAMING_PAGE_SELECTION_COUNT,
      bool includeInstalled = false,
      const rendering::Buffer *priorityReadback = nullptr,
      uint32_t minPriority = 0u,
      const std::vector<uint32_t> *prioritySnapshot = nullptr);
  // Read once after the frame fence, then reuse for install, demand and LOD
  // queries. Residency is filtered at query time, after any intervening installs.
  std::vector<uint32_t> readRuntimeObjectPagePriorities(
      const std::string &objectName, const rendering::Buffer &priorityReadback,
      bool touchResidentPages = false);
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
  struct RuntimeObjectRecord
  {
    RuntimeObjectId objectId = INVALID_RUNTIME_OBJECT_ID;
    std::string name;
    uint32_t pageTableOffset = UINT32_MAX;
    uint32_t pageCount = 0u;
    uint32_t prioritySlotOffset = UINT32_MAX;
    rendering::Allocation hierarchyAllocation{UINT64_MAX, 0u};
    uint32_t hierarchyStartOffset = UINT32_MAX;
    SceneInstanceId sceneInstanceId = INVALID_SCENE_INSTANCE_ID;
    QuantizationConfig quantizationConfig{};
    AABB localBounds{};
    std::vector<VirtualGeometryHierarchy> hierarchy;
    std::vector<RuntimePageStatus> pages;
    uint32_t queuedDecodeRequestCount = 0u;
    uint64_t latestGeometryRevision = 0u;
    // Cleared by unregisterRuntimeObject. The slot stays in runtimeObjects_ so
    // every live RuntimeObjectId keeps its meaning; the next registration
    // reuses it.
    bool registered = true;
    rendering::Allocation pageTableSlots{UINT64_MAX, 0u};
    rendering::Allocation prioritySlots{UINT64_MAX, 0u};
  };

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
  void releaseRuntimePageAllocation(const PageAllocation &allocation);

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
  uint64_t runtimeStreamingFrame_ = 1u;
  std::vector<RuntimeObjectRecord> runtimeObjects_;
  std::unordered_map<std::string, RuntimeObjectId> runtimeObjectByName_;

  struct DeferredRuntimeRelease
  {
    uint64_t releaseFrame = 0u;
    rendering::Allocation hierarchyAllocation{UINT64_MAX, 0u};
    rendering::Allocation pageTableSlots{UINT64_MAX, 0u};
    rendering::Allocation prioritySlots{UINT64_MAX, 0u};
    std::vector<PageAllocation> pageAllocations;
    // (global page table index, priority slot) entries to clear on release.
    std::vector<std::pair<uint32_t, uint32_t>> pageTableEntriesToClear;
  };
  struct RuntimeUploadRange
  {
    rendering::Buffer destination;
    uint64_t sourceOffset = 0u, destinationOffset = 0u, bytes = 0u;
  };
  struct RuntimeUploadFrame
  {
    rendering::BufferId staging = rendering::BufferId::Invalid;
    uint64_t capacity = 0u;
    uint32_t smallUploadFrames = 0u;
    std::vector<RuntimeUploadRange> ranges;
  };
  struct RuntimeUploadTarget
  {
    rendering::Buffer buffer;
    std::map<uint64_t, std::vector<uint8_t>> ranges;
  };
  std::map<std::string, RuntimeUploadTarget> pendingRuntimeUploads_;
  std::vector<RuntimeUploadFrame> runtimeUploadFrames_;
  void writeSceneBuffer(const rendering::Buffer &, uint64_t offset, uint64_t size, void *data);
  void prepareRuntimeUploads(uint32_t frameSlot);
  uint32_t runtimeReleaseDelayFrames_ = 0u;
  std::vector<DeferredRuntimeRelease> deferredRuntimeReleases_;
  std::vector<uint8_t> instanceLayoutDirtyPerSlot_;
  uint64_t deferredPagesBufferBytes_ = 0u;
  uint64_t deferredHierarchyBytes_ = 0u;
  uint64_t deferredClusterCount_ = 0u;
  uint64_t deferredDecodedVertexCount_ = 0u;
  uint64_t deferredDecodedIndexCount_ = 0u;
  bool findLeastRecentlyUsedRuntimePage(RuntimeObjectId &objectId, uint32_t &localPageIndex) const;
  uint32_t evictLeastRecentlyUsedRuntimePagesWhile(const std::function<bool()> &needsMore, uint32_t maximumEvictions);
  // Phase 8g. `budget` bounds the installs including dependencies; `depth`
  // bounds the dependency recursion at the page count.
  bool reinstallRuntimePageFromSourceRecursive(RuntimeObjectRecord &record, uint32_t localPageIndex, uint32_t depth, uint32_t budget, uint32_t &installedCount, std::string *error);
  static RuntimePageStatus::Reinstall makeRuntimePageReinstall(const RuntimeEncodedPageInstallInfo &install);
  RuntimePageSourceStats runtimePageSourceStats_{};

  bool deferringRuntimeReleases() const { return runtimeReleaseDelayFrames_ != 0u; }
  void publishInstanceLayoutChange();
  void applyDeferredRuntimeRelease(const DeferredRuntimeRelease &release);
  void retireRuntimePageAllocation(const PageAllocation &allocation, uint32_t globalPageTableIndex, uint32_t prioritySlot, bool clearPageTableEntry);
  void writeClearedPageTableEntry(uint32_t globalPageTableIndex, uint32_t prioritySlot);
  void rebuildRuntimeObjectHierarchyMasks(RuntimeObjectRecord &record);
  // The same rebuild restricted to `scope`: only nodes inside those ranges are
  // reset and re-marked, only installed pages whose own scope is identical are
  // replayed, and only those ranges are uploaded. On a shared terrain object a
  // page's scope is its chunk's slice, which is what makes an uninstall cost
  // the chunk rather than the world - the unscoped version walked 884K nodes
  // per installed page and uploaded 56 MB, per page removed, about 600 ms.
  void rebuildRuntimeObjectHierarchyMasksScoped(
      RuntimeObjectRecord &record,
      const std::vector<RuntimePageInstallInfo::HierarchyScanRange> &scope);
  bool runtimePageIsDependedOn(const RuntimeObjectRecord &record, uint32_t localPageIndex) const;
};

} // namespace virtualgeometry
