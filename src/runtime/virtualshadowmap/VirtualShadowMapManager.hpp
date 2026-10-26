#pragma once

#include "rendering/core/Camera.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualshadowmap/SoftShadowLayers.hpp"
#include "virtualgeometry/VirtualGeometryBounds.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace virtualgeometry
{

class VirtualShadowMapManager
{
public:
  static constexpr uint32_t DEFAULT_PAGE_TABLE_RESOLUTION = 32u;
  static constexpr uint32_t INVALIDATION_MASK_WORDS_PER_LAYER = 32u;

  struct Settings
  {
    uint32_t virtualPageTableResolution = DEFAULT_PAGE_TABLE_RESOLUTION;
    uint32_t physicalAtlasResolution = 16384u;
    uint32_t rsmAtlasResolution = 4096u;
    uint32_t physicalPageSize = 128u; // Physical page resolution in texels.
    uint32_t cascadeCount = 4u;
    uint32_t maxDirectionalLights = 1u;
    uint32_t maxSpotLights = 0u;
    uint32_t maxPointLights = 0u;
    uint32_t maxSoftShadowLayers = 8u;
    uint32_t maxAllocationRequestsPerFrame = 64u;
    uint32_t maxFutureAllocationRequests = 4096u;
    uint32_t fallbackCascadeOffset = 2u;
    float firstCascadeWorldExtent = 128.0f;
    float cascadeWorldExtentScale = 2.0f;
    float pageWorldScale = 1.0f;
    float lightDistance = 2048.0f;
    bool reverseZ = true;
    bool rsmPayloadEnabled = false;
    enum class ShadowMapType : uint32_t
    {
      VirtualShadowMap = 0u,
      ReflexiveShadowMap = 1u,
    } shadowMapType = ShadowMapType::VirtualShadowMap;
    std::string resourceNamePrefix = "VirtualShadowMap";
  };

  struct CascadeStateGPU
  {
    int32_t pageOffset[2] = {0, 0};
    int32_t pageShift[2] = {0, 0};
    uint32_t _padding[4] = {0u, 0u, 0u, 0u};
  };
  static_assert(sizeof(CascadeStateGPU) == 32u, "CascadeStateGPU must stay tightly packed");

  struct CascadeMatrixGPU
  {
    float view[16]{};
    float proj[16]{};
    float viewProj[16]{};
    float worldExtent = 0.0f;
    float pageWorldSize = 0.0f;
    uint32_t lightIndex = 0u;
    uint32_t cascadeIndex = 0u;
    uint32_t pageTableResolution = 0u;
    uint32_t lightType = 0u;
    uint32_t faceIndex = 0u;
    uint32_t mipLevel = 0u;
  };
  static_assert(sizeof(CascadeMatrixGPU) == 16u * sizeof(float) * 3u + 8u * sizeof(float), "CascadeMatrixGPU size mismatch");

  struct CameraStateGPU
  {
    float inverseView[16]{};
    float inverseProjection[16]{};
    float inverseViewProjection[16]{};
    float viewProjection[16]{};
    float cameraPosition[4]{};
    uint32_t reverseZ = 0u;
    uint32_t forceVisiblePagesDirtyLight = 0u;
    uint32_t _padding[2] = {0u, 0u};
  };
  static_assert(sizeof(CameraStateGPU) == 288u, "CameraStateGPU size mismatch");

  struct DirectionalLightGPU
  {
    float position[4]{};
    float direction[4]{};
    float color[4]{};
    float params[4]{};
    uint32_t metadata[4] = {0u, 0u, 0u, 0u};
  };
  static_assert(sizeof(DirectionalLightGPU) == 20u * sizeof(float), "DirectionalLightGPU size mismatch");

  struct SoftShadowLightGPU
  {
    uint32_t layer = 0u;
    uint32_t flags = 0u;
    float lightRadiusWorld = 0.0f;
    float penumbraScale = 1.0f;
  };
  static_assert(sizeof(SoftShadowLightGPU) == 16u, "SoftShadowLightGPU size mismatch");

  enum class LightType : uint32_t
  {
    Directional = 0u,
    Spot = 1u,
    Point = 2u,
  };

  static constexpr uint32_t POINT_LIGHT_FACE_COUNT = 6u;

  using LightId = uint32_t;
  static constexpr LightId INVALID_LIGHT_ID = UINT32_MAX;

  explicit VirtualShadowMapManager(rendering::RenderGraph *renderGraph) : VirtualShadowMapManager(renderGraph, Settings{})
  {
  }
  VirtualShadowMapManager(rendering::RenderGraph *renderGraph, Settings settings);
  ~VirtualShadowMapManager();

  LightId createDirectionalLight(const math::Vec3f &direction, const math::Vec3f &color = math::Vec3f(1.0f, 1.0f, 1.0f), float intensity = 1.0f);
  LightId createSpotLight(
      const math::Vec3f &position,
      const math::Vec3f &direction,
      float outerAngleRadians,
      float range,
      const math::Vec3f &color = math::Vec3f(1.0f, 1.0f, 1.0f),
      float intensity = 1.0f);
  LightId createPointLight(
      const math::Vec3f &position,
      float range,
      const math::Vec3f &color = math::Vec3f(1.0f, 1.0f, 1.0f),
      float intensity = 1.0f);
  void setDirectionalLightDirection(LightId lightId, const math::Vec3f &direction);
  void setLightColor(LightId lightId, const math::Vec3f &color);
  void setDirectionalLightColor(LightId lightId, const math::Vec3f &color);
  void setLightIntensity(LightId lightId, float intensity);
  void setPointLightPosition(LightId lightId, const math::Vec3f &position);
  void setPointLightRange(LightId lightId, float range);
  void setMaxSoftShadowLayers(uint32_t maxSoftShadowLayers);

  void update(const rendering::Camera &camera, const math::Mat4f *depthProjectionOverride = nullptr);
  void invalidateRegion(const AABB &region);
  void queueInvalidateRegion(const AABB &region);
  void invalidateAllPages();
  void queueInvalidateAllPages();
  void queueInvalidateLightPages(LightId lightId);
  void queueInvalidateVisiblePages();
  void resetInvalidations();

  const Settings &getSettings() const;
  uint32_t getVirtualPageTableResolution() const;
  uint32_t getPhysicalPageResolution() const;
  uint32_t getPhysicalPageTableResolution() const;
  uint32_t getCascadeCount() const;
  uint32_t getActiveLightCount() const;
  uint32_t getActiveDirectionalLightCount() const;
  uint32_t getActiveLayerCount() const;
  uint32_t getActiveSoftShadowLayerCount() const;
  uint32_t getMaxSoftShadowLayerCount() const;
  uint32_t getSoftShadowLayerForLight(LightId lightId) const;
  bool isSoftShadowFallbackLight(LightId lightId) const;
  uint32_t getSoftShadowMaxGraphDegree() const;
  uint32_t getSoftShadowOverflowCount() const;
  uint32_t getMaxLayerCount() const;
  uint32_t getLightFirstLayer(LightId lightId) const;
  uint32_t getLightLayerCount(LightId lightId) const;
  float getFirstCascadeWorldExtent() const;
  math::Vec3f getDirectionalLightDirection(LightId lightId) const;

  const rendering::Buffer &getVirtualPageTableBuffer() const;
  const rendering::Buffer &getPhysicalPageTableBuffer() const;
  const rendering::Buffer &getVirtualPageStateBuffer() const;
  const rendering::Buffer &getInvalidationMaskBuffer() const;
  const rendering::Buffer &getCascadeStatesBuffer() const;
  const rendering::Buffer &getCascadeMatricesBuffer() const;
  const rendering::Buffer &getCameraStateBuffer() const;
  const rendering::Buffer &getDirectionalLightsBuffer() const;
  const rendering::Buffer &getSoftShadowLightsBuffer() const;
  const rendering::Buffer &getAllocatorCountersBuffer() const;
  const rendering::Buffer &getAllocationRequestsBuffer() const;
  const rendering::Buffer &getFutureAllocationRequestsBuffer() const;
  const rendering::Buffer &getUnallocatedPhysicalPagesBuffer() const;
  const rendering::Buffer &getReclaimablePhysicalPagesBuffer() const;
  const rendering::Texture &getShadowAtlasTexture() const;
  bool usesRsmPayload() const;
  bool usesReflexiveShadowMap() const;
  uint32_t getRsmAtlasResolution() const;
  const rendering::Texture &getRsmFluxAtlasTexture() const;
  const rendering::Texture &getRsmNormalAtlasTexture() const;
  const rendering::Texture &getRsmPositionAtlasTexture() const;
  const rendering::Texture &getHierarchicalPageBoundsTexture() const;

  uint64_t getVirtualPageTableBufferSize() const;
  uint64_t getPhysicalPageTableBufferSize() const;
  uint64_t getVirtualPageStateBufferSize() const;
  uint64_t getInvalidationMaskBufferSize() const;
  uint64_t getCascadeStatesBufferSize() const;
  uint64_t getCascadeMatricesBufferSize() const;
  uint64_t getCameraStateBufferSize() const;
  uint64_t getDirectionalLightsBufferSize() const;
  uint64_t getSoftShadowLightsBufferSize() const;
  uint64_t getAllocatorCountersBufferSize() const;
  uint64_t getAllocationRequestsBufferSize() const;
  uint64_t getFutureAllocationRequestsBufferSize() const;
  uint64_t getUnallocatedPhysicalPagesBufferSize() const;
  uint64_t getReclaimablePhysicalPagesBufferSize() const;
  uint32_t getHierarchicalPageBoundsMipCount() const;
  uint32_t getAllocationRequestCapacity() const;
  uint32_t getFutureAllocationRequestCapacity() const;
  uint32_t getFallbackCascadeOffset() const;
  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const;

private:
  struct FrameOverrideResources
  {
    rendering::BufferId virtualPageStateBufferId = rendering::BufferId::Invalid;
    rendering::BufferId invalidationMaskBufferId = rendering::BufferId::Invalid;
    rendering::BufferId cascadeStatesBufferId = rendering::BufferId::Invalid;
    rendering::BufferId cascadeMatricesBufferId = rendering::BufferId::Invalid;
    rendering::BufferId cameraStateBufferId = rendering::BufferId::Invalid;
    rendering::BufferId directionalLightsBufferId = rendering::BufferId::Invalid;
    rendering::BufferId softShadowLightsBufferId = rendering::BufferId::Invalid;
    rendering::BufferId allocationRequestsBufferId = rendering::BufferId::Invalid;
    rendering::BufferId futureAllocationRequestsBufferId = rendering::BufferId::Invalid;
    rendering::TextureId hierarchicalPageBoundsTextureId = rendering::TextureId::Invalid;
  };

  struct CascadeRuntime
  {
    std::array<int32_t, 2> pageOffset = {0, 0};
    std::array<int32_t, 2> pageShift = {0, 0};
    std::array<int32_t, 2> quantizedCenter = {0, 0};
    bool hasPreviousQuantizedCenter = false;
    float worldExtent = 0.0f;
    float pageWorldSize = 0.0f;
    math::Mat4f view = math::Mat4f::identity();
    math::Mat4f proj = math::Mat4f::identity();
    math::Mat4f viewProj = math::Mat4f::identity();
  };

  struct DirectionalLightRuntime
  {
    LightType type = LightType::Directional;
    math::Vec3f position = math::Vec3f(0.0f, 0.0f, 0.0f);
    math::Vec3f direction = math::Vec3f(0.0f, -1.0f, 0.0f);
    math::Vec3f color = math::Vec3f(1.0f, 1.0f, 1.0f);
    float intensity = 1.0f;
    float range = 0.0f;
    float outerAngleRadians = 0.0f;
    uint32_t firstLayer = 0u;
    uint32_t softShadowLayer = 0u;
    bool softShadowFallback = false;
    float softShadowLightRadiusWorld = 0.5f;
    float softShadowPenumbraScale = 1.0f;
    std::vector<CascadeRuntime> cascades;
  };

  struct PendingLightInvalidation
  {
    LightId lightId = INVALID_LIGHT_ID;
  };

  struct PendingRegionInvalidation
  {
    AABB region;
  };

  static constexpr uint32_t VPT_DIRTY_BIT = 1u << 0u;
  static constexpr uint32_t VPT_ALLOCATED_BIT = 1u << 2u;
  static constexpr uint32_t VPT_PAGE_X_SHIFT = 8u;
  static constexpr uint32_t VPT_PAGE_Y_SHIFT = 16u;

  static constexpr uint32_t PPT_VISIBLE_BIT = 1u << 0u;
  static constexpr uint32_t PPT_ALLOCATED_BIT = 1u << 1u;
  static constexpr uint32_t PPT_VPT_X_SHIFT = 8u;
  static constexpr uint32_t PPT_VPT_Y_SHIFT = 16u;
  static constexpr uint32_t PPT_LAYER_SHIFT = 24u;

  static uint32_t packVPTEntry(bool dirty, bool visible, bool allocated, uint32_t pageX, uint32_t pageY);
  static uint32_t packPPTEntry(bool visible, bool allocated, uint32_t vptX, uint32_t vptY, uint32_t layer);

  void initializeBuffers();
  void initializeFrameOverrideResources();
  void initializeFrameLocalResources();
  void initializeFrameLocalResourcesForSlot(uint32_t frameSlot);
  void uploadPageTables();
  void uploadPageStates();
  void uploadInvalidationMasks();
  void uploadCascadeState();
  void uploadCascadeMatrices();
  void uploadDirectionalLights();
  void uploadSoftShadowLights();
  void rebuildSoftShadowLayers();
  void computeCascadeState(uint32_t lightIndex, uint32_t localLayerIndex, const rendering::Camera &camera);
  bool computeInvalidatedPageRectForLayer(
      uint32_t layerIndex,
      const AABB &region,
      uint32_t *pageMinX,
      uint32_t *pageMaxX,
      uint32_t *pageMinY,
      uint32_t *pageMaxY) const;
  void markRegionInvalidatedForLayer(uint32_t layerIndex, const AABB &region);
  void flushQueuedInvalidations();
  float computeCascadeWorldExtent(uint32_t cascadeIndex) const;
  uint32_t getLightLayerCount(const DirectionalLightRuntime &lightRuntime) const;
  uint32_t getTotalConfiguredLightCount() const;
  uint32_t getCurrentFrameSlot() const;
  std::vector<uint32_t> &getCurrentPageStatesCPU();
  const std::vector<uint32_t> &getCurrentPageStatesCPU() const;
  void clearCurrentPageStatesCPU();
  std::vector<uint32_t> &getCurrentInvalidationMasksCPU();
  const std::vector<uint32_t> &getCurrentInvalidationMasksCPU() const;
  void clearCurrentInvalidationMasksCPU();
  const FrameOverrideResources &getFrameOverrideResources(uint32_t frameSlot) const;
  const FrameOverrideResources &getCurrentFrameOverrideResources() const;

  static uint32_t positiveModulo(int32_t value, uint32_t modulus);
  static math::Vec3f chooseStableUp(const math::Vec3f &direction);

  rendering::RenderGraph *renderGraph_ = nullptr;
  Settings settings_{};

  uint32_t physicalPageTableResolution_ = 0u;
  uint32_t perspectiveLightMipCount_ = 1u;

  rendering::Buffer virtualPageTableBuffer_;
  rendering::Buffer physicalPageTableBuffer_;
  rendering::Buffer virtualPageStateBuffer_;
  rendering::Buffer invalidationMaskBuffer_;
  rendering::Buffer cascadeStatesBuffer_;
  rendering::Buffer cascadeMatricesBuffer_;
  rendering::Buffer cameraStateBuffer_;
  rendering::Buffer directionalLightsBuffer_;
  rendering::Buffer softShadowLightsBuffer_;
  rendering::Buffer allocatorCountersBuffer_;
  rendering::Buffer allocationRequestsBuffer_;
  rendering::Buffer futureAllocationRequestsBuffer_;
  rendering::Buffer unallocatedPhysicalPagesBuffer_;
  rendering::Buffer reclaimablePhysicalPagesBuffer_;
  rendering::Texture shadowAtlasTexture_;
  rendering::Texture rsmFluxAtlasTexture_;
  rendering::Texture rsmNormalAtlasTexture_;
  rendering::Texture rsmPositionAtlasTexture_;
  rendering::Texture hierarchicalPageBoundsTexture_;

  uint64_t virtualPageTableBufferSize_ = 0u;
  uint64_t physicalPageTableBufferSize_ = 0u;
  uint64_t virtualPageStateBufferSize_ = 0u;
  uint64_t invalidationMaskBufferSize_ = 0u;
  uint64_t cascadeStatesBufferSize_ = 0u;
  uint64_t cascadeMatricesBufferSize_ = 0u;
  uint64_t cameraStateBufferSize_ = 0u;
  uint64_t directionalLightsBufferSize_ = 0u;
  uint64_t softShadowLightsBufferSize_ = 0u;
  uint64_t allocatorCountersBufferSize_ = 0u;
  uint64_t allocationRequestsBufferSize_ = 0u;
  uint64_t futureAllocationRequestsBufferSize_ = 0u;
  uint64_t unallocatedPhysicalPagesBufferSize_ = 0u;
  uint64_t reclaimablePhysicalPagesBufferSize_ = 0u;
  uint32_t hierarchicalPageBoundsMipCount_ = 0u;

  std::vector<uint32_t> virtualPageTableCPU_;
  std::vector<uint32_t> physicalPageTableCPU_;
  std::vector<std::vector<uint32_t>> virtualPageStatesPerFrameCPU_;
  std::vector<std::vector<uint32_t>> invalidationMasksPerFrameCPU_;
  std::vector<FrameOverrideResources> frameOverrideResources_;
  std::vector<CascadeStateGPU> cascadeStatesCPU_;
  std::vector<CascadeMatrixGPU> cascadeMatricesCPU_;
  CameraStateGPU cameraStateCPU_{};
  std::vector<DirectionalLightGPU> directionalLightsCPU_;
  std::vector<SoftShadowLightGPU> softShadowLightsCPU_;
  std::vector<DirectionalLightRuntime> directionalLights_;
  uint32_t activeSoftShadowLayerCount_ = 0u;
  uint32_t softShadowMaxGraphDegree_ = 0u;
  uint32_t softShadowOverflowCount_ = 0u;
  std::vector<PendingRegionInvalidation> queuedInvalidationRegions_;
  std::vector<PendingLightInvalidation> queuedLightInvalidations_;
  bool queuedFullInvalidation_ = false;
  bool queuedVisiblePageInvalidation_ = false;
  LightId queuedVisibleInvalidationLight_ = INVALID_LIGHT_ID;
};

} // namespace virtualgeometry
