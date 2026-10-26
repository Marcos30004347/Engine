#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "math/math.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "sdf/SDFSceneFile.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

namespace sdf
{

struct SDFBounds
{
  math::Vec3f min{0.0f};
  math::Vec3f max{0.0f};
};

struct SDFRay
{
  math::Vec3f origin{0.0f};
  math::Vec3f direction{0.0f, 0.0f, 1.0f};
  float tMin = 0.0f;
  float tMax = 1.0e30f;
};

struct SDFSceneHit
{
  bool hit = false;
  uint32_t instanceIndex = UINT32_MAX;
  uint32_t instanceId = UINT32_MAX;
  uint32_t blasIndex = UINT32_MAX;
  uint32_t sdfNode = UINT32_MAX;
  uint32_t brickIndex = UINT32_MAX;
  uint32_t sdfLevel = 0u;
  uint32_t steps = 0u;
  float t = 0.0f;
  math::Vec3f worldPosition{0.0f};
  math::Vec3f worldNormal{0.0f, 0.0f, 1.0f};
  math::Vec3f localPosition{0.0f};
};

struct alignas(16) SDFSceneInfoGPU
{
  std::array<float, 4> worldBoundsMinAndMaxDepth{};
  std::array<float, 4> worldBoundsMaxAndRootNode{};
  std::array<uint32_t, 4> counts{};
  std::array<uint32_t, 4> settings{};
};

struct alignas(16) SDFInstanceGPU
{
  std::array<float, 16> localToWorld{};
  std::array<float, 16> worldToLocal{};
  std::array<float, 4> worldBoundsMin{};
  std::array<float, 4> worldBoundsMaxAndConservativeScale{};
  std::array<uint32_t, 4> idsAndBlas{};
  std::array<uint32_t, 4> materialAndFlags{};
};

struct alignas(16) SDFBlasGPU
{
  std::array<float, 4> localBoundsMin{};
  std::array<float, 4> localBoundsMaxAndRootVoxelSize{};
  std::array<uint32_t, 4> offsetsAndFormat{};
  std::array<uint32_t, 4> countsAndNarrowBand{};
};

struct alignas(16) SDFTlasNodeGPU
{
  std::array<float, 4> boundsMin{};
  std::array<float, 4> boundsMax{};
  std::array<uint32_t, 4> childAndRefs{};
  std::array<uint32_t, 4> depthFlags{};
  std::array<uint32_t, 4> childIndices0{};
  std::array<uint32_t, 4> childIndices1{};
};

struct alignas(16) SDFSceneGridInfoGPU
{
  std::array<float, 4> worldBoundsMin{};
  std::array<float, 4> worldBoundsMaxAndResolution{};
  std::array<uint32_t, 4> counts{};
  std::array<uint32_t, 4> settings{};
};

struct alignas(16) SDFSceneGridCellGPU
{
  std::array<uint32_t, 4> offsetCountAndFlags{};
};

struct alignas(16) SDFSceneGridRefGPU
{
  std::array<float, 4> boundsMin{};
  std::array<float, 4> boundsMax{};
  std::array<uint32_t, 4> kindInstanceAndPage{};
};

struct SDFGPUResources
{
  rendering::Buffer sceneInfo;
  rendering::Buffer instances;
  rendering::Buffer blases;
  rendering::Buffer blasNodes;
  rendering::Buffer blasNodeChildWordPrefixes;
  rendering::Buffer blasBricks;
  rendering::Buffer distanceWords;
  rendering::Buffer tracePageTables;
  rendering::Buffer tracePages;
  rendering::Buffer tracePageMasks;
  rendering::Buffer traceBrickMaps;
  rendering::Buffer tlasNodes;
  rendering::Buffer tlasInstanceRefs;
  rendering::Buffer sceneGridInfo;
  rendering::Buffer sceneGridCells;
  rendering::Buffer sceneGridRefs;
  rendering::Buffer sceneGridGlobalRefs;
  rendering::Texture distanceAtlas;
  uint32_t distanceAtlasBricksPerAxis = 0u;

  bool isValid() const
  {
    return sceneInfo.isValid() && instances.isValid() && blases.isValid() && blasNodes.isValid() &&
           blasNodeChildWordPrefixes.isValid() && blasBricks.isValid() && distanceWords.isValid() &&
           tracePageTables.isValid() && tracePages.isValid() && tracePageMasks.isValid() && traceBrickMaps.isValid() && tlasNodes.isValid() && tlasInstanceRefs.isValid() &&
           sceneGridInfo.isValid() && sceneGridCells.isValid() && sceneGridRefs.isValid() && sceneGridGlobalRefs.isValid() &&
           distanceAtlas.isValid() && distanceAtlasBricksPerAxis > 0u;
  }
};

class SDFSceneManager
{
public:
  struct Settings
  {
    uint32_t tlasMaxDepth = 16u;
    uint32_t tlasLeafTargetInstanceCount = 4u;
    uint32_t maxInstanceTraceSteps = 256u;
    float hitThresholdScale = 1.0f;
    float stepScale = 0.95f;
    float minStepScale = 0.25f;
    bool enableMailbox = true;
    bool enableSceneGrid = true;
    bool enableSceneGridPageRefs = true;
    uint32_t sceneGridResolution = 32u;
    uint32_t sceneGridCellCapacity = 16u;
    uint32_t sceneGridHugeInstanceCellThreshold = 64u;
    uint32_t sceneGridMaxCellsPerRef = 256u;
    uint32_t sceneGridGlobalRefCapacity = 8192u;
  };

  explicit SDFSceneManager(rendering::RenderGraph *renderGraph = nullptr);
  SDFSceneManager(rendering::RenderGraph *renderGraph, Settings settings);
  ~SDFSceneManager();

  bool loadVSDF(const std::string &path, std::string *error = nullptr);
  void clear();

  void setRenderGraph(rendering::RenderGraph *renderGraph);
  void uploadGPUResources();
  void destroyGPUResources();
  bool syncInstanceTransforms(
      const virtualgeometry::VirtualGeometryScene &scene,
      virtualgeometry::InstanceId objectInstanceId);
  bool syncInstanceTransforms(
      const virtualgeometry::VirtualGeometryScene::InstanceManager &instanceManager,
      const std::vector<virtualgeometry::SceneInstanceId> &sceneInstanceIds);

  std::vector<uint32_t> queryCandidates(const SDFRay &ray) const;
  SDFSceneHit traceCPU(const SDFRay &ray) const;

  const Settings &settings() const { return settings_; }
  const SDFBounds &worldBounds() const { return worldBounds_; }
  const SDFGPUResources &gpuResources() const { return gpuResources_; }
  const std::vector<SDFInstanceGPU> &gpuInstances() const { return gpuInstances_; }
  const std::vector<SDFBlasGPU> &gpuBlases() const { return gpuBlases_; }
  const std::vector<SDFTlasNodeGPU> &gpuTlasNodes() const { return gpuTlasNodes_; }
  const std::vector<uint32_t> &tlasInstanceRefs() const { return tlasInstanceRefs_; }
  uint64_t gpuResourceRevision() const { return gpuResourceRevision_; }
  uint32_t instanceCount() const { return static_cast<uint32_t>(instances_.size()); }
  uint32_t blasCount() const { return static_cast<uint32_t>(blases_.size()); }
  uint32_t tlasNodeCount() const { return static_cast<uint32_t>(tlasNodes_.size()); }

private:
  struct RuntimeBlas
  {
    VSDFDecodedInstance payload;
    uint32_t nodeOffset = 0u;
    uint32_t brickOffset = 0u;
    uint32_t distanceByteOffset = 0u;
    uint32_t distanceWordOffset = 0u;
  };

  struct RuntimeInstance
  {
    VSDFInstanceDirectoryEntry directory;
    VSDFInstancePayloadHeader header;
    SDFBounds worldBounds;
    SDFBounds localBounds;
    uint32_t blasIndex = 0u;
  };

  struct TlasNode
  {
    SDFBounds bounds;
    uint32_t childMask = 0u;
    uint32_t firstChild = UINT32_MAX;
    uint32_t firstInstanceRef = 0u;
    uint32_t instanceRefCount = 0u;
    uint32_t depth = 0u;
    uint32_t flags = 0u;
    std::array<uint32_t, 8u> childIndices{};
  };

  struct SampleResult
  {
    float distance = 1.0e30f;
    uint32_t nodeIndex = UINT32_MAX;
    uint32_t brickIndex = UINT32_MAX;
    uint32_t level = 0u;
    float voxelSize = 1.0f;
    bool validSurfaceCell = false;
    bool traceSurfaceCell = false;
    SDFBounds cellBounds{};
  };

  bool buildFromFile(const SDFSceneFile &file, std::string *error);
  void buildTLAS();
  uint32_t buildTlasNode(const SDFBounds &bounds, const std::vector<uint32_t> &instanceIndices, uint32_t depth);
  void flattenTLAS(uint32_t nodeIndex);
  void rebuildGPUArrays();
  void rebuildGPUInstanceTlasArrays();
  void rebuildGPUSceneGridArrays();
  void uploadDynamicGPUResources();

  SampleResult sampleLocal(uint32_t instanceIndex, const math::Vec3f &localPoint, uint32_t debugMaxLevel = UINT32_MAX) const;
  SDFSceneHit traceInstanceCPU(uint32_t instanceIndex, const SDFRay &ray, SDFSceneHit bestHit) const;

  rendering::Buffer createUploadedBuffer(const std::string &name, uint64_t size, const void *data, rendering::BufferUsage extraUsage) const;

  Settings settings_{};
  rendering::RenderGraph *renderGraph_ = nullptr;
  std::vector<RuntimeInstance> instances_;
  std::vector<RuntimeBlas> blases_;
  std::vector<TlasNode> tlasNodes_;
  std::vector<uint32_t> tlasInstanceRefs_;
  SDFBounds worldBounds_{};
  uint32_t rootTlasNode_ = UINT32_MAX;

  std::vector<SDFInstanceGPU> gpuInstances_;
  std::vector<SDFBlasGPU> gpuBlases_;
  std::vector<VSDFNodeDisk> gpuBlasNodes_;
  std::vector<uint32_t> gpuBlasNodeChildWordPrefixes_;
  std::vector<VSDFBrickDisk> gpuBlasBricks_;
  std::vector<uint32_t> gpuDistanceWords_;
  std::vector<uint32_t> gpuTracePageTables_;
  std::vector<VSDFTracePageDisk> gpuTracePages_;
  std::vector<uint32_t> gpuTracePageMasks_;
  std::vector<uint32_t> gpuTraceBrickMaps_;
  std::vector<uint8_t> gpuDistanceAtlasBytes_;
  uint32_t gpuDistanceAtlasBricksPerAxis_ = 1u;
  std::vector<SDFTlasNodeGPU> gpuTlasNodes_;
  SDFSceneGridInfoGPU gpuSceneGridInfo_{};
  std::vector<SDFSceneGridCellGPU> gpuSceneGridCells_;
  std::vector<SDFSceneGridRefGPU> gpuSceneGridRefs_;
  std::vector<SDFSceneGridRefGPU> gpuSceneGridGlobalRefs_;
  SDFSceneInfoGPU gpuSceneInfo_{};
  SDFGPUResources gpuResources_{};
  uint64_t gpuResourceRevision_ = 0u;
};

} // namespace sdf
