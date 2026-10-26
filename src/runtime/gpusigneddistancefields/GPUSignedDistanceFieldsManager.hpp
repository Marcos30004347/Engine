#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "math/math.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace gpusigneddistancefields
{

class GPUSignedDistanceFieldsManager
{
public:
  static constexpr uint32_t CASCADE_RESOLUTION = 64u;
  static constexpr uint32_t CASCADE_VOXEL_COUNT = CASCADE_RESOLUTION * CASCADE_RESOLUTION * CASCADE_RESOLUTION;
  static constexpr uint32_t BRICK_RESOLUTION = 8u;
  static constexpr uint32_t BRICK_CELL_COUNT = BRICK_RESOLUTION * BRICK_RESOLUTION * BRICK_RESOLUTION;
  static constexpr uint32_t INVALID_ID = 0x00ffffffu;
  static constexpr uint32_t UNINITIALIZED_ID = 0xffffffffu;
  static constexpr uint32_t INVALID_BOTTOM_AABB_NODE = 0x00007fffu;
  static constexpr uint32_t VOXELIZER_GROUP_SIZE = 256u;
  static constexpr uint32_t SCAN_GROUP_SIZE = 256u;
  static constexpr uint32_t COMPACT_REFERENCES_GROUP_SIZE = 64u;
  static constexpr uint32_t EMIT_SDF_GROUP_SIZE = 32u;
  static constexpr uint32_t EMIT_SDF_REFS_PER_GROUP = EMIT_SDF_GROUP_SIZE;
  static constexpr uint32_t INDIRECT_DISPATCH_STRIDE32 = 4u;
  static constexpr uint32_t INDIRECT_DISPATCH_STRIDE = 16u;
  static constexpr uint32_t BUILD_TREE_BOTTOM_NODE_COUNT = 16u * 16u * 16u;
  static constexpr uint32_t BUILD_TREE_MID_NODE_COUNT = 4u * 4u * 4u;
  static constexpr uint32_t BUILD_TREE_TOP_NODE_COUNT = 1u;
  static constexpr uint32_t BUILD_TREE_NODE_COUNT = BUILD_TREE_BOTTOM_NODE_COUNT + BUILD_TREE_MID_NODE_COUNT + BUILD_TREE_TOP_NODE_COUNT;
  static constexpr uint32_t BUILD_TREE_FLOAT4_COUNT = BUILD_TREE_NODE_COUNT * 2u;
  static constexpr uint32_t SURFACE_CACHE_FACE_COUNT = 6u;

  enum class Path : uint32_t
  {
    Static = 0u,
    Skinned = 1u,
    Combined = 2u,
  };

  enum MeshFlags : uint32_t
  {
    MeshFlag_None = 0u,
    MeshFlag_Skinned = 1u << 0u,
  };

  enum InstanceFlags : uint32_t
  {
    InstanceFlag_None = 0u,
    InstanceFlag_Skinned = 1u << 0u,
  };

  enum JobFlags : uint32_t
  {
    JobFlag_None = 0u,
    JobFlag_Invalidate = 1u << 0u,
  };

  enum ContextCounter : uint32_t
  {
    ContextCounter_BrickCount = 0u,
    ContextCounter_DirtyBricks = 1u,
    ContextCounter_FreeBricks = 2u,
    ContextCounter_ClearBricks = 3u,
    ContextCounter_MergeBricks = 4u,
    ContextCounter_NumDebugAabbs = 5u,
    ContextCounter_Count = 6u,
  };

  enum ScratchCounter : uint32_t
  {
    ScratchCounter_Triangles = 0u,
    ScratchCounter_MaxTriangles = 1u,
    ScratchCounter_References = 2u,
    ScratchCounter_MaxReferences = 3u,
    ScratchCounter_GroupIndex = 4u,
    ScratchCounter_CompressionBricks = 5u,
    ScratchCounter_StorageOffset = 6u,
    ScratchCounter_StorageSize = 7u,
    ScratchCounter_NumBricksAllocated = 8u,
    ScratchCounter_ClearBricks = 9u,
    ScratchCounter_Count = 10u,
  };

  enum IndirectOffset : uint32_t
  {
    IndirectOffset_InitializeCascade = 0u,
    IndirectOffset_CompactReferences = 16u,
    IndirectOffset_EmitSdf = 32u,
    IndirectOffset_Voxelize = 48u,
    IndirectOffset_CompressBricks = 64u,
    IndirectOffset_Eikonal = 80u,
    IndirectOffset_ClearBricks = 96u,
    IndirectOffset_MergeBricks = 112u,
    IndirectOffset_ScrollCascade = 128u,
    IndirectOffset_ClearRefCounters = 144u,
    IndirectOffset_ClearJobCounter = 160u,
    IndirectOffset_InvalidateJobAreas = 176u,
    IndirectOffset_CoarseCulling = 192u,
    IndirectOffset_ScanJobs = 208u,
    IndirectOffset_FinalizeJobScan = 224u,
    IndirectOffset_ScanReferences = 240u,
    IndirectOffset_FinalizeReferenceScan = 256u,
    IndirectOffset_Count = 272u,
  };

  struct Settings
  {
    uint32_t cascadeCount = 4u;
    float firstCascadeVoxelSize = 0.5f;
    float cascadeVoxelSizeScale = 2.0f;
    uint32_t atlasBrickCapacityPerAxis = 32u;
    uint32_t maxBricks = 32768u;
    uint32_t maxMeshes = 4096u;
    uint32_t maxVertices = 1u << 20u;
    uint32_t maxIndices = 1u << 21u;
    uint32_t maxBoneWeights = 1u << 18u;
    uint32_t maxBoneMatrices = 1u << 17u;
    uint32_t maxInstancesPerPath = 2048u;
    uint32_t maxJobsPerCascade = 2048u;
    uint32_t maxReferences = 1u << 18u;
    uint32_t maxBricksPerBake = 4096u;
    uint32_t maxTriangleSwapWords = 0u;
    uint32_t surfaceCachePageResolution = 4u;
    bool surfaceCacheEnableAlbedo = true;
    bool surfaceCacheEnableNormals = true;
    bool surfaceCacheEnableEmissive = false;
    float surfaceCacheDistanceThreshold = 0.75f;
  };

  struct SurfaceCacheAtlasLayout
  {
    uint32_t pageResolution = 0u;
    uint32_t pageCount = 0u;
    uint32_t pagesPerRow = 0u;
    uint32_t atlasWidth = 0u;
    uint32_t atlasHeight = 0u;
    uint64_t texelCount = 0u;
  };

  struct VertexGPU
  {
    math::Vec3f position;
    uint32_t boneWeightOffset = 0u;
    std::array<float, 4> uvMaterial{0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> surfaceBaseColorOpacity{0.85f, 0.85f, 0.85f, 1.0f};
    std::array<float, 4> surfaceNormalRoughness{0.0f, 0.0f, 1.0f, 1.0f};
    std::array<float, 4> surfaceEmissiveMetallic{0.0f, 0.0f, 0.0f, 0.0f};
    std::array<uint32_t, 4> surfaceFlags{0u, 0u, 0u, 0u};
  };
  static_assert(sizeof(VertexGPU) == 96u, "VertexGPU must stay tightly packed");

  struct BoneWeightGPU
  {
    float weight = 0.0f;
    uint32_t boneIndex = 0u;
    uint32_t _padding0 = 0u;
    uint32_t _padding1 = 0u;
  };
  static_assert(sizeof(BoneWeightGPU) == 16u, "BoneWeightGPU must stay tightly packed");

  struct MeshTableEntryGPU
  {
    uint32_t vertexOffset = 0u;
    uint32_t vertexCount = 0u;
    uint32_t indexOffset = 0u;
    uint32_t triangleCount = 0u;
    uint32_t boneWeightsPerVertex = 0u;
    uint32_t flags = 0u;
    uint32_t surfaceMaterialIndex = 0u;
    uint32_t _padding1 = 0u;
  };
  static_assert(sizeof(MeshTableEntryGPU) == 32u, "MeshTableEntryGPU must stay tightly packed");

  struct SurfaceMaterialGPU
  {
    std::array<float, 4> baseColorOpacity{0.85f, 0.85f, 0.85f, 1.0f};
    std::array<float, 4> normalRoughness{0.0f, 0.0f, 1.0f, 1.0f};
    std::array<float, 4> emissiveMetallic{0.0f, 0.0f, 0.0f, 0.0f};
    std::array<uint32_t, 4> flags{0u, 0u, 0u, 0u};
  };
  static_assert(sizeof(SurfaceMaterialGPU) == 64u, "SurfaceMaterialGPU must stay tightly packed");

  struct InstanceGPU
  {
    math::Mat4f modelMatrix = math::Mat4f::identity();
    math::Mat4f previousModelMatrix = math::Mat4f::identity();
    math::Vec3f aabbMin = math::Vec3f(0.0f, 0.0f, 0.0f);
    uint32_t meshIndex = 0u;
    math::Vec3f aabbMax = math::Vec3f(0.0f, 0.0f, 0.0f);
    uint32_t flags = 0u;
    uint32_t boneMatrixOffset = 0u;
    uint32_t previousBoneMatrixOffset = 0u;
    uint32_t boneCount = 0u;
    uint32_t _padding0 = 0u;
  };
  static_assert(sizeof(InstanceGPU) == 176u, "InstanceGPU must stay tightly packed");

  struct JobGPU
  {
    uint32_t aabbMin[3] = {0u, 0u, 0u};
    uint32_t instanceIndex = 0u;
    uint32_t aabbMax[3] = {0u, 0u, 0u};
    uint32_t flags = 0u;
  };
  static_assert(sizeof(JobGPU) == 32u, "JobGPU must stay tightly packed");

  struct RawTriangleReferenceGPU
  {
    uint32_t voxelIndex = 0u;
    uint32_t triangleId = 0u;
    uint32_t localReferenceIndex = 0u;
    uint32_t _padding0 = 0u;
  };
  static_assert(sizeof(RawTriangleReferenceGPU) == 16u, "RawTriangleReferenceGPU must stay tightly packed");

  struct CascadeInfoGPU
  {
    math::Vec3f gridMin = math::Vec3f(0.0f, 0.0f, 0.0f);
    float voxelSize = 1.0f;
    math::Vec3f gridMax = math::Vec3f(0.0f, 0.0f, 0.0f);
    uint32_t flags = 0u;
    uint32_t clipmapOffset[3] = {0u, 0u, 0u};
    uint32_t _padding0 = 0u;
    int32_t clipmapInvalidationOffset[3] = {0, 0, 0};
    uint32_t _padding1 = 0u;
    int32_t ioffset[3] = {0, 0, 0};
    uint32_t index = 0u;
    math::Vec3f gridMid = math::Vec3f(0.0f, 0.0f, 0.0f);
    uint32_t isEnabled = 1u;
    uint32_t relGridMinFP16[2] = {0u, 0u};
    uint32_t relGridMaxFP16[2] = {0u, 0u};
    uint32_t _padding2 = 0u;
    uint32_t _padding3 = 0u;
    float iVoxelSize = 1.0f;
    uint32_t isInitialized = 0u;
  };
  static_assert(sizeof(CascadeInfoGPU) == 128u, "CascadeInfoGPU must stay tightly packed");

  struct CascadeBuildInfoGPU
  {
    uint32_t numJobs = 0u;
    uint32_t numJobVoxels = 0u;
    uint32_t voxelizeDispatchX = 0u;
    uint32_t compactReferencesDispatchX = 0u;
    uint32_t clearBrickStorageDispatchX = 0u;
    uint32_t emitSdfDispatchX = 0u;
    uint32_t compressBricksDispatchX = 0u;
    uint32_t _padding0 = 0u;
  };
  static_assert(sizeof(CascadeBuildInfoGPU) == 32u, "CascadeBuildInfoGPU must stay tightly packed");

  struct ScratchResources
  {
    rendering::Buffer counters;
    uint64_t countersSize = 0u;
    rendering::Buffer jobCounters;
    uint64_t jobCountersSize = 0u;
    rendering::Buffer indexSwap;
    uint64_t indexSwapSize = 0u;
    rendering::Buffer rawReferences;
    uint64_t rawReferencesSize = 0u;
    rendering::Buffer compactedReferences;
    uint64_t compactedReferencesSize = 0u;
    rendering::Buffer referenceCounters;
    uint64_t referenceCountersSize = 0u;
    rendering::Buffer voxelAllocationFailCounters;
    uint64_t voxelAllocationFailCountersSize = 0u;
    rendering::Buffer jobCountersScan;
    uint64_t jobCountersScanSize = 0u;
    rendering::Buffer jobGlobalCountersScan;
    uint64_t jobGlobalCountersScanSize = 0u;
    rendering::Buffer referenceCounterScan;
    uint64_t referenceCounterScanSize = 0u;
    rendering::Buffer referenceGlobalScan;
    uint64_t referenceGlobalScanSize = 0u;
    rendering::Buffer stampScan;
    uint64_t stampScanSize = 0u;
    rendering::Buffer stampGlobalScan;
    uint64_t stampGlobalScanSize = 0u;
    rendering::Buffer brickStorage;
    uint64_t brickStorageSize = 0u;
    rendering::Buffer brickStorageOffsets;
    uint64_t brickStorageOffsetsSize = 0u;
    rendering::Buffer brickCompressionList;
    uint64_t brickCompressionListSize = 0u;
    rendering::Buffer brickClearList;
    uint64_t brickClearListSize = 0u;
    // Compatibility alias during the all-cascade migration.
    rendering::Buffer indirectArgs;
    uint64_t indirectArgsSize = 0u;
  };

  struct CascadeResources
  {
    rendering::Buffer brickMap;
    uint64_t brickMapSize = 0u;
    uint64_t brickMapOffset = 0u;
    rendering::Buffer aabbTree;
    uint64_t aabbTreeSize = 0u;
    uint64_t aabbTreeOffset = 0u;
    rendering::Buffer jobBuffer;
    uint64_t jobBufferSize = 0u;
    uint64_t jobBufferOffset = 0u;
    rendering::Buffer jobSubvoxelOffsetBuffer;
    uint64_t jobSubvoxelOffsetBufferSize = 0u;
    uint64_t jobSubvoxelOffsetBufferOffset = 0u;
    uint32_t jobCount = 0u;
    uint32_t jobVoxelCount = 0u;
  };

  struct PathResources
  {
    rendering::Buffer instances;
    uint64_t instancesSize = 0u;
    uint32_t instanceCount = 0u;
    uint32_t activeInstanceCount = 0u;
    ScratchResources scratch;
    rendering::Buffer cascadeBuildInfos;
    uint64_t cascadeBuildInfosSize = 0u;
    rendering::Buffer stageIndirectArgs;
    uint64_t stageIndirectArgsSize = 0u;
    rendering::Buffer brickMaps;
    uint64_t brickMapsSize = 0u;
    rendering::Buffer aabbTrees;
    uint64_t aabbTreesSize = 0u;
    rendering::Buffer jobBuffers;
    uint64_t jobBuffersSize = 0u;
    rendering::Buffer jobSubvoxelOffsetBuffers;
    uint64_t jobSubvoxelOffsetBuffersSize = 0u;
    std::vector<CascadeResources> cascades;
  };

  struct SharedResources
  {
    rendering::Buffer meshTable;
    uint64_t meshTableSize = 0u;
    rendering::Buffer surfaceMaterials;
    uint64_t surfaceMaterialsSize = 0u;
    rendering::Buffer vertices;
    uint64_t verticesSize = 0u;
    rendering::Buffer indices;
    uint64_t indicesSize = 0u;
    rendering::Buffer boneWeights;
    uint64_t boneWeightsSize = 0u;
    rendering::Buffer boneMatrices;
    uint64_t boneMatricesSize = 0u;

    rendering::Buffer brickVoxelMap;
    uint64_t brickVoxelMapSize = 0u;
    rendering::Buffer brickAABB;
    uint64_t brickAABBSize = 0u;
    rendering::Buffer brickFreeList;
    uint64_t brickFreeListSize = 0u;
    rendering::Buffer brickClearList;
    uint64_t brickClearListSize = 0u;
    rendering::Buffer brickDirtyList;
    uint64_t brickDirtyListSize = 0u;
    rendering::Buffer brickMergeList;
    uint64_t brickMergeListSize = 0u;
    rendering::Buffer brickEikonalCounters;
    uint64_t brickEikonalCountersSize = 0u;
    rendering::Buffer contextCounters;
    uint64_t contextCountersSize = 0u;
    rendering::Buffer contextIndirectArgs;
    uint64_t contextIndirectArgsSize = 0u;
    rendering::Buffer cascadeInfos;
    uint64_t cascadeInfosSize = 0u;

    rendering::Texture sdfAtlas;

    SurfaceCacheAtlasLayout surfaceCacheLayout;
    rendering::Texture surfaceCacheMaterialReferenceAtlas;
    uint64_t surfaceCacheMaterialReferenceAtlasSize = 0u;
    rendering::Texture surfaceCacheMaterialReferenceBasisAtlas;
    uint64_t surfaceCacheMaterialReferenceBasisAtlasSize = 0u;
    rendering::Buffer surfaceCacheMaterialReferenceScores;
    uint64_t surfaceCacheMaterialReferenceScoresSize = 0u;
  };

  GPUSignedDistanceFieldsManager(rendering::RenderGraph *renderGraph);
  GPUSignedDistanceFieldsManager(rendering::RenderGraph *renderGraph, Settings settings);
  ~GPUSignedDistanceFieldsManager();

  static bool isSurfaceCachePageResolutionValid(uint32_t pageResolution);
  static bool isSurfaceCacheEnabled(const Settings &settings);
  static SurfaceCacheAtlasLayout makeSurfaceCacheAtlasLayout(const Settings &settings);
  static uint64_t surfaceCacheResidentByteSize(const Settings &settings, uint32_t bytesPerTexel);
  void updateClipmaps(const math::Vec3f &center);
  void markCascadeInitialized(uint32_t cascadeIndex);
  void markAllCascadesUninitialized();
  void markContextInitialized();

  void uploadMeshTable(const std::vector<MeshTableEntryGPU> &meshTable);
  void uploadSurfaceMaterials(const std::vector<SurfaceMaterialGPU> &materials);
  void uploadVertices(const std::vector<VertexGPU> &vertices);
  void uploadIndices(const std::vector<uint32_t> &indices);
  void uploadBoneWeights(const std::vector<BoneWeightGPU> &boneWeights);
  void uploadBoneMatrices(const std::vector<math::Mat4f> &boneMatrices);
  void uploadInstances(Path path, const std::vector<InstanceGPU> &instances);
  void uploadInstanceSlice(Path path, uint32_t index, const InstanceGPU &instance);
  void uploadBoneMatricesSlice(uint32_t startIndex, uint32_t count, const math::Mat4f *matrices);
  void setInstanceCount(Path path, uint32_t count);
  void setActiveInstanceCount(Path path, uint32_t count);
  void uploadJobs(Path path, uint32_t cascadeIndex, const std::vector<JobGPU> &jobs);
  void syncFrameLocalInputs();
  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const;

  const Settings &getSettings() const;
  const SharedResources &getSharedResources() const;
  const PathResources &getPathResources(Path path) const;
  const std::vector<MeshTableEntryGPU> &getUploadedMeshTable() const;
  const std::vector<SurfaceMaterialGPU> &getUploadedSurfaceMaterials() const;
  const std::vector<VertexGPU> &getUploadedVertices() const;
  const std::vector<uint32_t> &getUploadedIndices() const;
  const std::vector<BoneWeightGPU> &getUploadedBoneWeights() const;
  const std::vector<math::Mat4f> &getUploadedBoneMatrices() const;
  const std::vector<InstanceGPU> &getUploadedInstances(Path path) const;
  const std::vector<std::vector<JobGPU>> &getUploadedJobs(Path path) const;
  const CascadeInfoGPU &getCascadeInfo(uint32_t cascadeIndex) const;
  uint64_t getCascadeBuildInfoOffset(uint32_t cascadeIndex) const;
  uint64_t getCascadeInfoOffset(uint32_t cascadeIndex) const;
  uint32_t getCascadeCount() const;
  bool needsContextInitialization() const;
  bool isPathBuildable(Path path) const;

private:
  struct CascadeRuntime
  {
    bool hasPreviousQuantizedMin = false;
    bool isInitialized = false;
    int32_t quantizedGridMin[3] = {0, 0, 0};
    uint32_t clipmapOffset[3] = {0u, 0u, 0u};
  };

  void initializeSharedResources();
  void initializePathResources();
  void initializePathResource(Path path);
  void initializeBufferContents();
  rendering::Buffer createManagedBuffer(rendering::BufferInfo info, bool frameLocal);
  void initializeFrameOverrideBuffer(const rendering::Buffer &buffer, uint64_t size, rendering::BufferUsage usage);
  rendering::BufferId getFrameOverrideBufferId(const rendering::Buffer &buffer, uint32_t frameSlot) const;
  void writeManagedBuffer(const rendering::Buffer &buffer, uint64_t offset, uint64_t size, const void *data) const;
  void syncPathFrameLocalInputs(Path path);
  void uploadCascadeInfos();
  void uploadCascadeBuildInfo(Path path, uint32_t cascadeIndex);
  void uploadDynamicStageIndirectArgs(Path path);
  void uploadWholeBuffer(const rendering::Buffer &buffer, uint64_t bufferSize, const void *data, uint64_t dataSize);
  void uploadBufferPrefix(const rendering::Buffer &buffer, uint64_t bufferOffset, const void *data, uint64_t dataSize);
  void uploadUIntBufferFill(const rendering::Buffer &buffer, uint64_t bufferSize, uint32_t value);
  static uint32_t positiveModulo(int32_t value, uint32_t modulus);
  static const char *pathName(Path path);

  rendering::RenderGraph *renderGraph_ = nullptr;
  Settings settings_{};
  SharedResources sharedResources_{};
  PathResources staticResources_{};
  PathResources skinnedResources_{};
  PathResources combinedResources_{};
  std::vector<MeshTableEntryGPU> uploadedMeshTable_{};
  std::vector<SurfaceMaterialGPU> uploadedSurfaceMaterials_{};
  std::vector<VertexGPU> uploadedVertices_{};
  std::vector<uint32_t> uploadedIndices_{};
  std::vector<BoneWeightGPU> uploadedBoneWeights_{};
  std::vector<math::Mat4f> uploadedBoneMatrices_{};
  std::vector<InstanceGPU> uploadedStaticInstances_{};
  std::vector<InstanceGPU> uploadedSkinnedInstances_{};
  std::vector<std::vector<JobGPU>> uploadedStaticJobs_{};
  std::vector<std::vector<JobGPU>> uploadedSkinnedJobs_{};
  std::unordered_map<std::string, std::vector<rendering::BufferId>> frameOverrideBufferIds_{};
  std::vector<CascadeInfoGPU> cascadeInfos_{};
  std::vector<CascadeRuntime> cascadeRuntime_{};
  bool needsContextInitialization_ = true;
};

} // namespace gpusigneddistancefields
