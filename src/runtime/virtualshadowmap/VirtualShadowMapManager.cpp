#include "virtualshadowmap/VirtualShadowMapManager.hpp"

#include "rendering/core/LightCamera.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace virtualgeometry
{

namespace
{

constexpr float WORLD_UP_DOT_THRESHOLD = 0.95f;
constexpr float PI_F = 3.14159265358979323846f;
constexpr float LOCAL_LIGHT_NEAR_PLANE = 0.1f;
constexpr uint32_t MAX_PACKED_PHYSICAL_PAGE_COORD = 0xFFu;
constexpr uint32_t FORCE_VISIBLE_PAGES_DIRTY_ALL_LIGHTS = 0xFFFFFFFFu;

uint32_t computeMipCount(uint32_t dimension)
{
  uint32_t mipCount = 1u;
  while (dimension > 1u)
  {
    dimension >>= 1u;
    ++mipCount;
  }
  return mipCount;
}

uint64_t computeMipChainPageCount(uint32_t resolution)
{
  uint64_t total = 0u;
  uint32_t currentResolution = std::max(1u, resolution);
  while (true)
  {
    total += static_cast<uint64_t>(currentResolution) * currentResolution;
    if (currentResolution == 1u)
    {
      break;
    }
    currentResolution = std::max(1u, currentResolution >> 1u);
  }
  return total;
}

inline uint32_t computeAtlasTilesPerRow(uint32_t itemCount)
{
  uint32_t tilesPerRow = 1u;
  while (tilesPerRow * tilesPerRow < std::max(1u, itemCount))
  {
    ++tilesPerRow;
  }
  return tilesPerRow;
}

inline math::Vec3f makeAABBCorner(const AABB &region, uint32_t cornerIndex)
{
  return math::Vec3f(
      (cornerIndex & 1u) ? region.maxPoint[0] : region.minPoint[0],
      (cornerIndex & 2u) ? region.maxPoint[1] : region.minPoint[1],
      (cornerIndex & 4u) ? region.maxPoint[2] : region.minPoint[2]);
}

inline math::Vec3f computeAABBCenter(const AABB &region)
{
  return math::Vec3f(
      (region.minPoint[0] + region.maxPoint[0]) * 0.5f,
      (region.minPoint[1] + region.maxPoint[1]) * 0.5f,
      (region.minPoint[2] + region.maxPoint[2]) * 0.5f);
}

inline float computeAABBSphereRadius(const AABB &region)
{
  const math::Vec3f center = computeAABBCenter(region);
  const math::Vec3f maxCorner(region.maxPoint[0], region.maxPoint[1], region.maxPoint[2]);
  return (maxCorner - center).length();
}

inline float clampAbsMin(float value, float minAbsValue)
{
  return value >= 0.0f ? std::max(value, minAbsValue) : std::min(value, -minAbsValue);
}

inline bool perspectiveFrustumCullAABB(const AABB &region, const math::Mat4f &view, const math::Mat4f &proj)
{
  const float invProjX = 1.0f / std::max(std::abs(proj.at(0, 0)), 1e-8f);
  const float invProjY = 1.0f / std::max(std::abs(proj.at(1, 1)), 1e-8f);
  bool allOutsideLeft = true;
  bool allOutsideRight = true;
  bool allOutsideBottom = true;
  bool allOutsideTop = true;
  bool allOutsideNear = true;

  for (uint32_t cornerIndex = 0u; cornerIndex < 8u; ++cornerIndex)
  {
    const math::Vec3f worldCorner = makeAABBCorner(region, cornerIndex);
    const math::Vec4f viewCorner = view * math::Vec4f(worldCorner[0], worldCorner[1], worldCorner[2], 1.0f);
    const float leftPlane = viewCorner[2] * invProjX;
    const float rightPlane = -viewCorner[2] * invProjX;
    const float bottomPlane = viewCorner[2] * invProjY;
    const float topPlane = -viewCorner[2] * invProjY;

    if (viewCorner[0] >= leftPlane)
    {
      allOutsideLeft = false;
    }
    if (viewCorner[0] <= rightPlane)
    {
      allOutsideRight = false;
    }
    if (viewCorner[1] >= bottomPlane)
    {
      allOutsideBottom = false;
    }
    if (viewCorner[1] <= topPlane)
    {
      allOutsideTop = false;
    }
    if (viewCorner[2] <= -LOCAL_LIGHT_NEAR_PLANE)
    {
      allOutsideNear = false;
    }
  }

  return !(allOutsideLeft || allOutsideRight || allOutsideBottom || allOutsideTop || allOutsideNear);
}

inline bool projectAABBToPerspectiveUvBounds(
    const AABB &region,
    const math::Mat4f &view,
    const math::Mat4f &proj,
    float *minU,
    float *maxU,
    float *minV,
    float *maxV)
{
  if (!perspectiveFrustumCullAABB(region, view, proj))
  {
    return false;
  }

  const math::Vec3f centerWorld = computeAABBCenter(region);
  const float radius = computeAABBSphereRadius(region);
  const math::Vec4f centerView4 = view * math::Vec4f(centerWorld[0], centerWorld[1], centerWorld[2], 1.0f);
  const float centerX = centerView4[0];
  const float centerY = centerView4[1];
  const float centerZ = -centerView4[2];
  if (centerZ < radius + LOCAL_LIGHT_NEAR_PLANE)
  {
    *minU = 0.0f;
    *maxU = 1.0f;
    *minV = 0.0f;
    *maxV = 1.0f;
    return true;
  }

  const float centerRadiusX = centerX * radius;
  const float centerRadiusY = centerY * radius;
  const float centerRadiusZ = centerZ * radius;
  const float centerDepthRadiusSq = std::max(centerZ * centerZ - radius * radius, 0.0f);
  const float vx = std::sqrt(centerX * centerX + centerDepthRadiusSq);
  const float vy = std::sqrt(centerY * centerY + centerDepthRadiusSq);
  const float minX = (vx * centerX - centerRadiusZ) / clampAbsMin(vx * centerZ + centerRadiusX, 1e-6f);
  const float maxX = (vx * centerX + centerRadiusZ) / clampAbsMin(vx * centerZ - centerRadiusX, 1e-6f);
  const float minY = (vy * centerY - centerRadiusZ) / clampAbsMin(vy * centerZ + centerRadiusY, 1e-6f);
  const float maxY = (vy * centerY + centerRadiusZ) / clampAbsMin(vy * centerZ - centerRadiusY, 1e-6f);
  const float p00 = std::abs(proj.at(0, 0));
  const float p11 = std::abs(proj.at(1, 1));

  *minU = std::clamp(minX * p00 * 0.5f + 0.5f, 0.0f, 1.0f);
  *maxU = std::clamp(maxX * p00 * 0.5f + 0.5f, 0.0f, 1.0f);
  *minV = std::clamp(minY * p11 * 0.5f + 0.5f, 0.0f, 1.0f);
  *maxV = std::clamp(maxY * p11 * 0.5f + 0.5f, 0.0f, 1.0f);
  return true;
}

inline math::Vec3f getPointLightFaceDirection(uint32_t faceIndex)
{
  switch (faceIndex)
  {
  case 0u:
    return math::Vec3f(1.0f, 0.0f, 0.0f);
  case 1u:
    return math::Vec3f(-1.0f, 0.0f, 0.0f);
  case 2u:
    return math::Vec3f(0.0f, 1.0f, 0.0f);
  case 3u:
    return math::Vec3f(0.0f, -1.0f, 0.0f);
  case 4u:
    return math::Vec3f(0.0f, 0.0f, 1.0f);
  default:
    return math::Vec3f(0.0f, 0.0f, -1.0f);
  }
}

inline math::Vec3f getPointLightFaceUp(uint32_t faceIndex)
{
  switch (faceIndex)
  {
  case 0u:
  case 1u:
  case 4u:
  case 5u:
    return math::Vec3f(0.0f, 1.0f, 0.0f);
  case 2u:
    return math::Vec3f(0.0f, 0.0f, -1.0f);
  default:
    return math::Vec3f(0.0f, 0.0f, 1.0f);
  }
}

} // namespace

VirtualShadowMapManager::VirtualShadowMapManager(rendering::RenderGraph *renderGraph, Settings settings) : renderGraph_(renderGraph), settings_(settings)
{
  if (renderGraph_ == nullptr)
  {
    throw std::runtime_error("VirtualShadowMapManager requires a valid RenderGraph");
  }
  if (settings_.virtualPageTableResolution == 0u || settings_.virtualPageTableResolution > DEFAULT_PAGE_TABLE_RESOLUTION ||
      (settings_.virtualPageTableResolution & (settings_.virtualPageTableResolution - 1u)) != 0u)
  {
    throw std::runtime_error("VirtualShadowMapManager requires a power-of-two virtual page table resolution in [1, 32]");
  }
  if (settings_.physicalPageSize == 0u || settings_.physicalAtlasResolution == 0u || settings_.cascadeCount == 0u)
  {
    throw std::runtime_error("VirtualShadowMapManager settings must all be non-zero");
  }
  const auto isSupportedRsmAtlasResolution = [](uint32_t resolution) -> bool
  {
    return resolution == 1024u || resolution == 2048u || resolution == 4096u || resolution == 8192u || resolution == 16384u;
  };
  if ((settings_.rsmPayloadEnabled || settings_.shadowMapType == Settings::ShadowMapType::ReflexiveShadowMap) && !isSupportedRsmAtlasResolution(settings_.rsmAtlasResolution))
  {
    throw std::runtime_error("VirtualShadowMapManager RSM atlas resolution must be one of 1024, 2048, 4096, 8192 or 16384");
  }
  if ((settings_.maxDirectionalLights + settings_.maxSpotLights + settings_.maxPointLights) == 0u)
  {
    throw std::runtime_error("VirtualShadowMapManager requires at least one configured light slot");
  }
  if (!std::isfinite(settings_.pageWorldScale) || settings_.pageWorldScale <= 0.0f)
  {
    throw std::runtime_error("VirtualShadowMapManager pageWorldScale must be finite and greater than zero");
  }

  physicalPageTableResolution_ = settings_.physicalAtlasResolution / settings_.physicalPageSize;
  perspectiveLightMipCount_ = computeMipCount(settings_.virtualPageTableResolution);
  if (physicalPageTableResolution_ == 0u || (settings_.physicalAtlasResolution % settings_.physicalPageSize) != 0u)
  {
    throw std::runtime_error("VirtualShadowMapManager requires an atlas resolution divisible by the page size");
  }
  if (physicalPageTableResolution_ > (MAX_PACKED_PHYSICAL_PAGE_COORD + 1u))
  {
    throw std::runtime_error("VirtualShadowMapManager physical page resolution creates more than 256 atlas pages per axis, which exceeds packed VPT coordinates");
  }
  const uint64_t maxVirtualPages =
      static_cast<uint64_t>(getMaxLayerCount()) * settings_.virtualPageTableResolution * settings_.virtualPageTableResolution;
  const uint64_t physicalPages =
      static_cast<uint64_t>(physicalPageTableResolution_) * physicalPageTableResolution_;

  initializeBuffers();
}

VirtualShadowMapManager::~VirtualShadowMapManager()
{
  if (renderGraph_ == nullptr)
  {
    return;
  }

  rendering::RHI *const rhi = renderGraph_->getRHI();
  for (const FrameOverrideResources &frameResources : frameOverrideResources_)
  {
    if (frameResources.virtualPageStateBufferId != rendering::BufferId::Invalid)
    {
      rhi->deleteBuffer(frameResources.virtualPageStateBufferId);
    }
    if (frameResources.invalidationMaskBufferId != rendering::BufferId::Invalid)
    {
      rhi->deleteBuffer(frameResources.invalidationMaskBufferId);
    }
    if (frameResources.cascadeStatesBufferId != rendering::BufferId::Invalid)
    {
      rhi->deleteBuffer(frameResources.cascadeStatesBufferId);
    }
    if (frameResources.cascadeMatricesBufferId != rendering::BufferId::Invalid)
    {
      rhi->deleteBuffer(frameResources.cascadeMatricesBufferId);
    }
    if (frameResources.cameraStateBufferId != rendering::BufferId::Invalid)
    {
      rhi->deleteBuffer(frameResources.cameraStateBufferId);
    }
    if (frameResources.directionalLightsBufferId != rendering::BufferId::Invalid)
    {
      rhi->deleteBuffer(frameResources.directionalLightsBufferId);
    }
    if (frameResources.softShadowLightsBufferId != rendering::BufferId::Invalid)
    {
      rhi->deleteBuffer(frameResources.softShadowLightsBufferId);
    }
    if (frameResources.allocationRequestsBufferId != rendering::BufferId::Invalid)
    {
      rhi->deleteBuffer(frameResources.allocationRequestsBufferId);
    }
    if (frameResources.futureAllocationRequestsBufferId != rendering::BufferId::Invalid)
    {
      rhi->deleteBuffer(frameResources.futureAllocationRequestsBufferId);
    }
    if (frameResources.hierarchicalPageBoundsTextureId != rendering::TextureId::Invalid)
    {
      rhi->deleteTexture(frameResources.hierarchicalPageBoundsTextureId);
    }
  }
  frameOverrideResources_.clear();

  const auto deleteBufferIfValid = [&](const rendering::Buffer &buffer)
  {
    if (buffer.isValid())
    {
      renderGraph_->deleteBuffer(buffer);
    }
  };
  const auto deleteTextureIfValid = [&](const rendering::Texture &texture)
  {
    if (texture.isValid())
    {
      renderGraph_->deleteTexture(texture);
    }
  };

  deleteBufferIfValid(virtualPageTableBuffer_);
  deleteBufferIfValid(physicalPageTableBuffer_);
  deleteBufferIfValid(virtualPageStateBuffer_);
  deleteBufferIfValid(invalidationMaskBuffer_);
  deleteBufferIfValid(cascadeStatesBuffer_);
  deleteBufferIfValid(cascadeMatricesBuffer_);
  deleteBufferIfValid(cameraStateBuffer_);
  deleteBufferIfValid(directionalLightsBuffer_);
  deleteBufferIfValid(softShadowLightsBuffer_);
  deleteBufferIfValid(allocatorCountersBuffer_);
  deleteBufferIfValid(allocationRequestsBuffer_);
  deleteBufferIfValid(futureAllocationRequestsBuffer_);
  deleteBufferIfValid(unallocatedPhysicalPagesBuffer_);
  deleteBufferIfValid(reclaimablePhysicalPagesBuffer_);
  deleteTextureIfValid(shadowAtlasTexture_);
  deleteTextureIfValid(rsmFluxAtlasTexture_);
  deleteTextureIfValid(rsmNormalAtlasTexture_);
  deleteTextureIfValid(rsmPositionAtlasTexture_);
  deleteTextureIfValid(hierarchicalPageBoundsTexture_);
}

VirtualShadowMapManager::LightId VirtualShadowMapManager::createDirectionalLight(const math::Vec3f &direction, const math::Vec3f &color, float intensity)
{
  uint32_t activeDirectionalLightCount = 0u;
  for (const DirectionalLightRuntime &lightRuntime : directionalLights_)
  {
    if (lightRuntime.type == LightType::Directional)
    {
      ++activeDirectionalLightCount;
    }
  }
  if (activeDirectionalLightCount >= settings_.maxDirectionalLights)
  {
    throw std::runtime_error("VirtualShadowMapManager exceeded maxDirectionalLights");
  }

  DirectionalLightRuntime lightRuntime;
  lightRuntime.type = LightType::Directional;
  lightRuntime.direction = direction.normalize();
  lightRuntime.color = color;
  lightRuntime.intensity = std::max(intensity, 0.0f);
  lightRuntime.firstLayer = getActiveLayerCount();
  lightRuntime.cascades.resize(settings_.cascadeCount);
  directionalLights_.push_back(lightRuntime);
  directionalLightsCPU_.resize(std::max<uint32_t>(1u, getTotalConfiguredLightCount()));
  softShadowLightsCPU_.resize(std::max<uint32_t>(1u, getTotalConfiguredLightCount()));
  rebuildSoftShadowLayers();

  const uint32_t lightIndex = static_cast<uint32_t>(directionalLights_.size() - 1u);
  return lightIndex;
}

VirtualShadowMapManager::LightId VirtualShadowMapManager::createSpotLight(
    const math::Vec3f &position,
    const math::Vec3f &direction,
    float outerAngleRadians,
    float range,
    const math::Vec3f &color,
    float intensity)
{
  uint32_t activeSpotLightCount = 0u;
  for (const DirectionalLightRuntime &lightRuntime : directionalLights_)
  {
    if (lightRuntime.type == LightType::Spot)
    {
      ++activeSpotLightCount;
    }
  }
  if (activeSpotLightCount >= settings_.maxSpotLights)
  {
    throw std::runtime_error("VirtualShadowMapManager exceeded maxSpotLights");
  }

  DirectionalLightRuntime lightRuntime;
  lightRuntime.type = LightType::Spot;
  lightRuntime.position = position;
  lightRuntime.direction = direction.normalize();
  lightRuntime.color = color;
  lightRuntime.intensity = std::max(intensity, 0.0f);
  lightRuntime.range = std::max(range, 0.1f);
  lightRuntime.outerAngleRadians = std::clamp(outerAngleRadians, 0.05f, PI_F - 0.05f);
  lightRuntime.firstLayer = getActiveLayerCount();
  lightRuntime.cascades.resize(perspectiveLightMipCount_);
  directionalLights_.push_back(lightRuntime);
  directionalLightsCPU_.resize(std::max<uint32_t>(1u, getTotalConfiguredLightCount()));
  softShadowLightsCPU_.resize(std::max<uint32_t>(1u, getTotalConfiguredLightCount()));
  rebuildSoftShadowLayers();

  return static_cast<uint32_t>(directionalLights_.size() - 1u);
}

VirtualShadowMapManager::LightId VirtualShadowMapManager::createPointLight(
    const math::Vec3f &position,
    float range,
    const math::Vec3f &color,
    float intensity)
{
  uint32_t activePointLightCount = 0u;
  for (const DirectionalLightRuntime &lightRuntime : directionalLights_)
  {
    if (lightRuntime.type == LightType::Point)
    {
      ++activePointLightCount;
    }
  }
  if (activePointLightCount >= settings_.maxPointLights)
  {
    throw std::runtime_error("VirtualShadowMapManager exceeded maxPointLights");
  }

  DirectionalLightRuntime lightRuntime;
  lightRuntime.type = LightType::Point;
  lightRuntime.position = position;
  lightRuntime.color = color;
  lightRuntime.intensity = std::max(intensity, 0.0f);
  lightRuntime.range = std::max(range, 0.1f);
  lightRuntime.outerAngleRadians = PI_F * 0.5f;
  lightRuntime.firstLayer = getActiveLayerCount();
  lightRuntime.cascades.resize(POINT_LIGHT_FACE_COUNT * perspectiveLightMipCount_);
  directionalLights_.push_back(lightRuntime);
  directionalLightsCPU_.resize(std::max<uint32_t>(1u, getTotalConfiguredLightCount()));
  softShadowLightsCPU_.resize(std::max<uint32_t>(1u, getTotalConfiguredLightCount()));
  rebuildSoftShadowLayers();

  return static_cast<uint32_t>(directionalLights_.size() - 1u);
}

void VirtualShadowMapManager::setDirectionalLightDirection(LightId lightId, const math::Vec3f &direction)
{
  if (lightId >= directionalLights_.size() || directionalLights_[lightId].type != LightType::Directional)
  {
    return;
  }

  const math::Vec3f normalizedDirection = direction.normalize();
  const math::Vec3f previousDirection = directionalLights_[lightId].direction;
  directionalLights_[lightId].direction = normalizedDirection;

  if ((normalizedDirection - previousDirection).length() > 1e-5f)
  {
    queueInvalidateLightPages(lightId);
  }
}

void VirtualShadowMapManager::setLightColor(LightId lightId, const math::Vec3f &color)
{
  if (lightId >= directionalLights_.size())
  {
    return;
  }

  directionalLights_[lightId].color = color;
}

void VirtualShadowMapManager::setDirectionalLightColor(LightId lightId, const math::Vec3f &color)
{
  setLightColor(lightId, color);
}

void VirtualShadowMapManager::setLightIntensity(LightId lightId, float intensity)
{
  if (lightId >= directionalLights_.size())
  {
    return;
  }

  directionalLights_[lightId].intensity = std::max(intensity, 0.0f);
  rebuildSoftShadowLayers();
}

void VirtualShadowMapManager::setPointLightPosition(LightId lightId, const math::Vec3f &position)
{
  if (lightId >= directionalLights_.size() || directionalLights_[lightId].type != LightType::Point)
  {
    return;
  }

  if ((directionalLights_[lightId].position - position).length() <= 1e-5f)
  {
    return;
  }

  directionalLights_[lightId].position = position;
  rebuildSoftShadowLayers();
  queueInvalidateLightPages(lightId);
}

void VirtualShadowMapManager::setPointLightRange(LightId lightId, float range)
{
  if (lightId >= directionalLights_.size() || directionalLights_[lightId].type != LightType::Point)
  {
    return;
  }

  const float clampedRange = std::max(range, 0.1f);
  if (std::abs(directionalLights_[lightId].range - clampedRange) <= 1e-5f)
  {
    return;
  }

  directionalLights_[lightId].range = clampedRange;
  rebuildSoftShadowLayers();
  queueInvalidateLightPages(lightId);
}

void VirtualShadowMapManager::setMaxSoftShadowLayers(uint32_t maxSoftShadowLayers)
{
  const uint32_t clampedLayerCount = std::max(maxSoftShadowLayers, 1u);
  if (settings_.maxSoftShadowLayers == clampedLayerCount)
  {
    return;
  }
  settings_.maxSoftShadowLayers = clampedLayerCount;
  rebuildSoftShadowLayers();
}

void VirtualShadowMapManager::update(const rendering::Camera &camera, const math::Mat4f *depthProjectionOverride)
{
  const math::Mat4f inverseView = camera.getInverseViewMatrix();
  const math::Mat4f &depthProjection = depthProjectionOverride != nullptr ? *depthProjectionOverride : camera.getProjectionMatrix();
  const math::Mat4f inverseProjection = depthProjection.inverse();
  const math::Mat4f viewProjection = depthProjection * camera.getViewMatrix();
  const math::Mat4f inverseViewProjection = viewProjection.inverse();
  std::memcpy(cameraStateCPU_.inverseView, inverseView.data, sizeof(cameraStateCPU_.inverseView));
  std::memcpy(cameraStateCPU_.inverseProjection, inverseProjection.data, sizeof(cameraStateCPU_.inverseProjection));
  std::memcpy(cameraStateCPU_.inverseViewProjection, inverseViewProjection.data, sizeof(cameraStateCPU_.inverseViewProjection));
  std::memcpy(cameraStateCPU_.viewProjection, viewProjection.data, sizeof(cameraStateCPU_.viewProjection));
  cameraStateCPU_.cameraPosition[0] = camera.getPosition()[0];
  cameraStateCPU_.cameraPosition[1] = camera.getPosition()[1];
  cameraStateCPU_.cameraPosition[2] = camera.getPosition()[2];
  cameraStateCPU_.cameraPosition[3] = 1.0f;
  cameraStateCPU_.reverseZ = camera.isReverseZ() ? 1u : 0u;
  cameraStateCPU_.forceVisiblePagesDirtyLight = queuedVisiblePageInvalidation_
                                                    ? (queuedVisibleInvalidationLight_ == INVALID_LIGHT_ID ? FORCE_VISIBLE_PAGES_DIRTY_ALL_LIGHTS : queuedVisibleInvalidationLight_ + 1u)
                                                    : 0u;
  cameraStateCPU_._padding[0] = 0u;
  cameraStateCPU_._padding[1] = 0u;
  queuedVisiblePageInvalidation_ = false;
  queuedVisibleInvalidationLight_ = INVALID_LIGHT_ID;

  for (uint32_t lightIndex = 0u; lightIndex < directionalLights_.size(); ++lightIndex)
  {
    for (uint32_t localLayerIndex = 0u; localLayerIndex < getLightLayerCount(directionalLights_[lightIndex]); ++localLayerIndex)
    {
      computeCascadeState(lightIndex, localLayerIndex, camera);
    }
  }

  clearCurrentPageStatesCPU();
  uploadPageStates();
  clearCurrentInvalidationMasksCPU();
  flushQueuedInvalidations();
  uploadInvalidationMasks();
  uploadCascadeState();
  uploadCascadeMatrices();
  uploadDirectionalLights();
  uploadSoftShadowLights();
  renderGraph_->getRHI()->bufferWrite(getCurrentFrameOverrideResources().cameraStateBufferId, 0, cameraStateBufferSize_, &cameraStateCPU_);
}

void VirtualShadowMapManager::invalidateRegion(const AABB &region)
{
  for (uint32_t lightIndex = 0u; lightIndex < directionalLights_.size(); ++lightIndex)
  {
    const DirectionalLightRuntime &lightRuntime = directionalLights_[lightIndex];
    for (uint32_t localLayerIndex = 0u; localLayerIndex < getLightLayerCount(lightRuntime); ++localLayerIndex)
    {
      const uint32_t layerIndex = lightRuntime.firstLayer + localLayerIndex;
      markRegionInvalidatedForLayer(layerIndex, region);
    }
  }

  uploadInvalidationMasks();
}

void VirtualShadowMapManager::queueInvalidateRegion(const AABB &region)
{
  PendingRegionInvalidation pending{};
  pending.region = region;
  queuedInvalidationRegions_.push_back(pending);
  queuedVisiblePageInvalidation_ = true;
  queuedVisibleInvalidationLight_ = INVALID_LIGHT_ID;
}

void VirtualShadowMapManager::invalidateAllPages()
{
  std::vector<uint32_t> &invalidationMasksCPU = getCurrentInvalidationMasksCPU();
  std::fill(invalidationMasksCPU.begin(), invalidationMasksCPU.end(), 0xFFFFFFFFu);
  uploadInvalidationMasks();
}

void VirtualShadowMapManager::queueInvalidateAllPages()
{
  queuedFullInvalidation_ = true;
  queuedVisiblePageInvalidation_ = true;
  queuedVisibleInvalidationLight_ = INVALID_LIGHT_ID;
}

void VirtualShadowMapManager::queueInvalidateLightPages(LightId lightId)
{
  if (lightId >= directionalLights_.size())
  {
    return;
  }

  queuedVisiblePageInvalidation_ = true;
  queuedVisibleInvalidationLight_ = lightId;

  for (PendingLightInvalidation &pending : queuedLightInvalidations_)
  {
    if (pending.lightId == lightId)
    {
      return;
    }
  }

  queuedLightInvalidations_.push_back(PendingLightInvalidation{.lightId = lightId});
}

void VirtualShadowMapManager::queueInvalidateVisiblePages()
{
  queuedVisiblePageInvalidation_ = true;
  queuedVisibleInvalidationLight_ = INVALID_LIGHT_ID;
}

void VirtualShadowMapManager::resetInvalidations()
{
  clearCurrentInvalidationMasksCPU();
  uploadInvalidationMasks();
}

const VirtualShadowMapManager::Settings &VirtualShadowMapManager::getSettings() const
{
  return settings_;
}

uint32_t VirtualShadowMapManager::getVirtualPageTableResolution() const
{
  return settings_.virtualPageTableResolution;
}

uint32_t VirtualShadowMapManager::getPhysicalPageResolution() const
{
  return settings_.physicalPageSize;
}

uint32_t VirtualShadowMapManager::getPhysicalPageTableResolution() const
{
  return physicalPageTableResolution_;
}

uint32_t VirtualShadowMapManager::getCascadeCount() const
{
  return settings_.cascadeCount;
}

uint32_t VirtualShadowMapManager::getActiveLightCount() const
{
  return static_cast<uint32_t>(directionalLights_.size());
}

uint32_t VirtualShadowMapManager::getActiveDirectionalLightCount() const
{
  uint32_t activeDirectionalLightCount = 0u;
  for (const DirectionalLightRuntime &lightRuntime : directionalLights_)
  {
    if (lightRuntime.type == LightType::Directional)
    {
      ++activeDirectionalLightCount;
    }
  }
  return activeDirectionalLightCount;
}

uint32_t VirtualShadowMapManager::getActiveLayerCount() const
{
  uint32_t layerCount = 0u;
  for (const DirectionalLightRuntime &lightRuntime : directionalLights_)
  {
    layerCount += getLightLayerCount(lightRuntime);
  }
  return layerCount;
}

uint32_t VirtualShadowMapManager::getActiveSoftShadowLayerCount() const
{
  return activeSoftShadowLayerCount_;
}

uint32_t VirtualShadowMapManager::getMaxSoftShadowLayerCount() const
{
  return std::max(settings_.maxSoftShadowLayers, 1u);
}

uint32_t VirtualShadowMapManager::getSoftShadowLayerForLight(LightId lightId) const
{
  if (lightId >= directionalLights_.size())
  {
    return 0u;
  }
  return directionalLights_[lightId].softShadowLayer;
}

bool VirtualShadowMapManager::isSoftShadowFallbackLight(LightId lightId) const
{
  if (lightId >= directionalLights_.size())
  {
    return true;
  }
  return directionalLights_[lightId].softShadowFallback;
}

uint32_t VirtualShadowMapManager::getSoftShadowMaxGraphDegree() const
{
  return softShadowMaxGraphDegree_;
}

uint32_t VirtualShadowMapManager::getSoftShadowOverflowCount() const
{
  return softShadowOverflowCount_;
}

uint32_t VirtualShadowMapManager::getMaxLayerCount() const
{
  return settings_.maxDirectionalLights * settings_.cascadeCount +
         settings_.maxSpotLights * perspectiveLightMipCount_ +
         settings_.maxPointLights * POINT_LIGHT_FACE_COUNT * perspectiveLightMipCount_;
}

uint32_t VirtualShadowMapManager::getLightFirstLayer(LightId lightId) const
{
  if (lightId >= directionalLights_.size())
  {
    return 0u;
  }

  return directionalLights_[lightId].firstLayer;
}

uint32_t VirtualShadowMapManager::getLightLayerCount(LightId lightId) const
{
  if (lightId >= directionalLights_.size())
  {
    return 0u;
  }

  return getLightLayerCount(directionalLights_[lightId]);
}

float VirtualShadowMapManager::getFirstCascadeWorldExtent() const
{
  return settings_.firstCascadeWorldExtent * settings_.pageWorldScale;
}

math::Vec3f VirtualShadowMapManager::getDirectionalLightDirection(LightId lightId) const
{
  if (lightId >= directionalLights_.size() || directionalLights_[lightId].type != LightType::Directional)
  {
    return math::Vec3f(0.0f, -1.0f, 0.0f);
  }
  return directionalLights_[lightId].direction;
}

const rendering::Buffer &VirtualShadowMapManager::getVirtualPageTableBuffer() const
{
  return virtualPageTableBuffer_;
}

const rendering::Buffer &VirtualShadowMapManager::getPhysicalPageTableBuffer() const
{
  return physicalPageTableBuffer_;
}

const rendering::Buffer &VirtualShadowMapManager::getVirtualPageStateBuffer() const
{
  return virtualPageStateBuffer_;
}

const rendering::Buffer &VirtualShadowMapManager::getInvalidationMaskBuffer() const
{
  return invalidationMaskBuffer_;
}

const rendering::Buffer &VirtualShadowMapManager::getCascadeStatesBuffer() const
{
  return cascadeStatesBuffer_;
}

const rendering::Buffer &VirtualShadowMapManager::getCascadeMatricesBuffer() const
{
  return cascadeMatricesBuffer_;
}

const rendering::Buffer &VirtualShadowMapManager::getCameraStateBuffer() const
{
  return cameraStateBuffer_;
}

const rendering::Buffer &VirtualShadowMapManager::getDirectionalLightsBuffer() const
{
  return directionalLightsBuffer_;
}

const rendering::Buffer &VirtualShadowMapManager::getSoftShadowLightsBuffer() const
{
  return softShadowLightsBuffer_;
}

const rendering::Buffer &VirtualShadowMapManager::getAllocatorCountersBuffer() const
{
  return allocatorCountersBuffer_;
}

const rendering::Buffer &VirtualShadowMapManager::getAllocationRequestsBuffer() const
{
  return allocationRequestsBuffer_;
}

const rendering::Buffer &VirtualShadowMapManager::getFutureAllocationRequestsBuffer() const
{
  return futureAllocationRequestsBuffer_;
}

const rendering::Buffer &VirtualShadowMapManager::getUnallocatedPhysicalPagesBuffer() const
{
  return unallocatedPhysicalPagesBuffer_;
}

const rendering::Buffer &VirtualShadowMapManager::getReclaimablePhysicalPagesBuffer() const
{
  return reclaimablePhysicalPagesBuffer_;
}

const rendering::Texture &VirtualShadowMapManager::getShadowAtlasTexture() const
{
  return shadowAtlasTexture_;
}

bool VirtualShadowMapManager::usesReflexiveShadowMap() const
{
  return usesRsmPayload();
}

bool VirtualShadowMapManager::usesRsmPayload() const
{
  return settings_.rsmPayloadEnabled || settings_.shadowMapType == Settings::ShadowMapType::ReflexiveShadowMap;
}

uint32_t VirtualShadowMapManager::getRsmAtlasResolution() const
{
  return usesRsmPayload() ? settings_.rsmAtlasResolution : settings_.physicalAtlasResolution;
}

const rendering::Texture &VirtualShadowMapManager::getRsmFluxAtlasTexture() const
{
  return rsmFluxAtlasTexture_;
}

const rendering::Texture &VirtualShadowMapManager::getRsmNormalAtlasTexture() const
{
  return rsmNormalAtlasTexture_;
}

const rendering::Texture &VirtualShadowMapManager::getRsmPositionAtlasTexture() const
{
  return rsmPositionAtlasTexture_;
}

const rendering::Texture &VirtualShadowMapManager::getHierarchicalPageBoundsTexture() const
{
  return hierarchicalPageBoundsTexture_;
}

uint64_t VirtualShadowMapManager::getVirtualPageTableBufferSize() const
{
  return virtualPageTableBufferSize_;
}

uint64_t VirtualShadowMapManager::getPhysicalPageTableBufferSize() const
{
  return physicalPageTableBufferSize_;
}

uint64_t VirtualShadowMapManager::getVirtualPageStateBufferSize() const
{
  return virtualPageStateBufferSize_;
}

uint64_t VirtualShadowMapManager::getInvalidationMaskBufferSize() const
{
  return invalidationMaskBufferSize_;
}

uint64_t VirtualShadowMapManager::getCascadeStatesBufferSize() const
{
  return cascadeStatesBufferSize_;
}

uint64_t VirtualShadowMapManager::getCascadeMatricesBufferSize() const
{
  return cascadeMatricesBufferSize_;
}

uint64_t VirtualShadowMapManager::getCameraStateBufferSize() const
{
  return cameraStateBufferSize_;
}

uint64_t VirtualShadowMapManager::getDirectionalLightsBufferSize() const
{
  return directionalLightsBufferSize_;
}

uint64_t VirtualShadowMapManager::getSoftShadowLightsBufferSize() const
{
  return softShadowLightsBufferSize_;
}

uint64_t VirtualShadowMapManager::getAllocatorCountersBufferSize() const
{
  return allocatorCountersBufferSize_;
}

uint64_t VirtualShadowMapManager::getAllocationRequestsBufferSize() const
{
  return allocationRequestsBufferSize_;
}

uint64_t VirtualShadowMapManager::getFutureAllocationRequestsBufferSize() const
{
  return futureAllocationRequestsBufferSize_;
}

uint64_t VirtualShadowMapManager::getUnallocatedPhysicalPagesBufferSize() const
{
  return unallocatedPhysicalPagesBufferSize_;
}

uint64_t VirtualShadowMapManager::getReclaimablePhysicalPagesBufferSize() const
{
  return reclaimablePhysicalPagesBufferSize_;
}

uint32_t VirtualShadowMapManager::getHierarchicalPageBoundsMipCount() const
{
  return hierarchicalPageBoundsMipCount_;
}

uint32_t VirtualShadowMapManager::getAllocationRequestCapacity() const
{
  return settings_.maxAllocationRequestsPerFrame;
}

uint32_t VirtualShadowMapManager::getFutureAllocationRequestCapacity() const
{
  return static_cast<uint32_t>(futureAllocationRequestsBufferSize_ / sizeof(uint32_t));
}

uint32_t VirtualShadowMapManager::getFallbackCascadeOffset() const
{
  return settings_.fallbackCascadeOffset;
}

void VirtualShadowMapManager::appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
{
  const FrameOverrideResources &frameResources = getFrameOverrideResources(frameSlot);
  overrides.bufferOverrides.emplace(virtualPageStateBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = frameResources.virtualPageStateBufferId});
  overrides.bufferOverrides.emplace(invalidationMaskBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = frameResources.invalidationMaskBufferId});
  overrides.bufferOverrides.emplace(cascadeStatesBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = frameResources.cascadeStatesBufferId});
  overrides.bufferOverrides.emplace(cascadeMatricesBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = frameResources.cascadeMatricesBufferId});
  overrides.bufferOverrides.emplace(cameraStateBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = frameResources.cameraStateBufferId});
  overrides.bufferOverrides.emplace(directionalLightsBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = frameResources.directionalLightsBufferId});
  overrides.bufferOverrides.emplace(softShadowLightsBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = frameResources.softShadowLightsBufferId});
  overrides.bufferOverrides.emplace(allocationRequestsBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = frameResources.allocationRequestsBufferId});
  overrides.bufferOverrides.emplace(futureAllocationRequestsBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = frameResources.futureAllocationRequestsBufferId});
  overrides.textureOverrides.emplace(
      hierarchicalPageBoundsTexture_.name,
      rendering::RenderGraphTextureOverride{
          .textureId = frameResources.hierarchicalPageBoundsTextureId,
          .layout = rendering::ResourceLayout::UNDEFINED,
      });
}

uint32_t VirtualShadowMapManager::packVPTEntry(bool dirty, bool visible, bool allocated, uint32_t pageX, uint32_t pageY)
{
  (void)visible;
  return (dirty ? VPT_DIRTY_BIT : 0u) | (allocated ? VPT_ALLOCATED_BIT : 0u) | (pageX << VPT_PAGE_X_SHIFT) | (pageY << VPT_PAGE_Y_SHIFT);
}

uint32_t VirtualShadowMapManager::packPPTEntry(bool visible, bool allocated, uint32_t vptX, uint32_t vptY, uint32_t layer)
{
  return (visible ? PPT_VISIBLE_BIT : 0u) | (allocated ? PPT_ALLOCATED_BIT : 0u) | (vptX << PPT_VPT_X_SHIFT) | (vptY << PPT_VPT_Y_SHIFT) | (layer << PPT_LAYER_SHIFT);
}

void VirtualShadowMapManager::initializeBuffers()
{
  const auto resourceName = [this](const char *suffix) -> std::string
  {
    return settings_.resourceNamePrefix + suffix;
  };
  const uint64_t maxLayers = getMaxLayerCount();
  const uint64_t vptEntriesPerLayer = static_cast<uint64_t>(settings_.virtualPageTableResolution) * settings_.virtualPageTableResolution;
  const uint64_t vptMipChainEntriesPerLayer = computeMipChainPageCount(settings_.virtualPageTableResolution);
  const uint64_t pptEntries = static_cast<uint64_t>(physicalPageTableResolution_) * physicalPageTableResolution_;
  const uint64_t packedCoordSize = sizeof(uint32_t);

  virtualPageTableBufferSize_ = maxLayers * vptEntriesPerLayer * sizeof(uint32_t);
  physicalPageTableBufferSize_ = pptEntries * sizeof(uint32_t);
  virtualPageStateBufferSize_ = virtualPageTableBufferSize_;
  invalidationMaskBufferSize_ = maxLayers * INVALIDATION_MASK_WORDS_PER_LAYER * sizeof(uint32_t);
  cascadeStatesBufferSize_ = maxLayers * sizeof(CascadeStateGPU);
  cascadeMatricesBufferSize_ = maxLayers * sizeof(CascadeMatrixGPU);
  cameraStateBufferSize_ = sizeof(CameraStateGPU);
  directionalLightsBufferSize_ = std::max<uint64_t>(1u, getTotalConfiguredLightCount()) * sizeof(DirectionalLightGPU);
  softShadowLightsBufferSize_ = std::max<uint64_t>(1u, getTotalConfiguredLightCount()) * sizeof(SoftShadowLightGPU);
  allocatorCountersBufferSize_ = 8u * sizeof(uint32_t);
  const uint64_t totalVirtualPagesAcrossMipChain = maxLayers * vptMipChainEntriesPerLayer;
  allocationRequestsBufferSize_ = std::max<uint64_t>(settings_.maxAllocationRequestsPerFrame, totalVirtualPagesAcrossMipChain) * packedCoordSize;
  futureAllocationRequestsBufferSize_ = std::max<uint64_t>(settings_.maxFutureAllocationRequests, totalVirtualPagesAcrossMipChain) * packedCoordSize;
  unallocatedPhysicalPagesBufferSize_ = pptEntries * packedCoordSize;
  reclaimablePhysicalPagesBufferSize_ = pptEntries * packedCoordSize;

  virtualPageTableBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = resourceName("_VPT.buffer"),
          .size = virtualPageTableBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  physicalPageTableBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = resourceName("_PPT.buffer"),
          .size = physicalPageTableBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
      });
  virtualPageStateBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = resourceName("_PageState.buffer"),
          .size = virtualPageStateBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          .frameLocal = true,
      });
  invalidationMaskBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = resourceName("_Invalidations.buffer"),
          .size = invalidationMaskBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          .frameLocal = true,
      });
  cascadeStatesBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = resourceName("_CascadeStates.buffer"),
          .size = cascadeStatesBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          .frameLocal = true,
      });
  cascadeMatricesBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = resourceName("_CascadeMatrices.buffer"),
          .size = cascadeMatricesBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          .frameLocal = true,
      });
  cameraStateBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = resourceName("_CameraState.buffer"),
          .size = cameraStateBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          .frameLocal = true,
      });
  directionalLightsBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = resourceName("_DirectionalLights.buffer"),
          .size = directionalLightsBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          .frameLocal = true,
      });
  softShadowLightsBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = resourceName("_SoftShadowLights.buffer"),
          .size = softShadowLightsBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          .frameLocal = true,
      });
  allocatorCountersBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = resourceName("_AllocatorCounters.buffer"),
          .size = allocatorCountersBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  allocationRequestsBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = resourceName("_AllocationRequests.buffer"),
          .size = allocationRequestsBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage,
          .frameLocal = true,
      });
  futureAllocationRequestsBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = resourceName("_FutureAllocationRequests.buffer"),
          .size = futureAllocationRequestsBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage,
          .frameLocal = true,
      });
  unallocatedPhysicalPagesBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = resourceName("_UnallocatedPhysicalPages.buffer"),
          .size = unallocatedPhysicalPagesBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
      });
  reclaimablePhysicalPagesBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = resourceName("_ReclaimablePhysicalPages.buffer"),
          .size = reclaimablePhysicalPagesBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
      });
  hierarchicalPageBoundsMipCount_ = computeMipCount(settings_.virtualPageTableResolution);
  shadowAtlasTexture_ = renderGraph_->createTexture(
      rendering::TextureInfo{
          .name = resourceName("_Atlas.texture"),
          .format = rendering::Format::Format_Depth32Float,
          .memoryProperties = rendering::BufferUsage::BufferUsage_None,
          .usage = rendering::ImageUsage::ImageUsage_Sampled | rendering::ImageUsage::ImageUsage_DepthStencilAttachment,
          .width = settings_.physicalAtlasResolution,
          .height = settings_.physicalAtlasResolution,
          .depth = 1u,
          .mipLevels = 1u,
      });
  if (usesRsmPayload())
  {
    const auto createRsmAtlasTexture = [&](const char *suffix, rendering::Format format) -> rendering::Texture
    {
      return renderGraph_->createTexture(
          rendering::TextureInfo{
              .name = resourceName(suffix),
              .format = format,
              .memoryProperties = rendering::BufferUsage::BufferUsage_None,
              .usage = rendering::ImageUsage::ImageUsage_Sampled | rendering::ImageUsage::ImageUsage_Storage,
              .width = settings_.rsmAtlasResolution,
              .height = settings_.rsmAtlasResolution,
              .depth = 1u,
              .mipLevels = 1u,
          });
    };
    rsmFluxAtlasTexture_ = createRsmAtlasTexture("_RSMFluxAtlas.texture", rendering::Format::Format_RGBA16Float);
    rsmNormalAtlasTexture_ = createRsmAtlasTexture("_RSMNormalAtlas.texture", rendering::Format::Format_RGBA8Unorm);
    rsmPositionAtlasTexture_ = createRsmAtlasTexture("_RSMPositionAtlas.texture", rendering::Format::Format_RGBA16Float);
  }
  const uint32_t hpbTilesPerRow = computeAtlasTilesPerRow(getMaxLayerCount());
  const uint32_t hpbTilesPerColumn = std::max(1u, (getMaxLayerCount() + hpbTilesPerRow - 1u) / hpbTilesPerRow);
  hierarchicalPageBoundsTexture_ = renderGraph_->createTexture(
      rendering::TextureInfo{
          .name = resourceName("_HPB.texture"),
          .format = rendering::Format::Format_R32Float,
          .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
          .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          .width = settings_.virtualPageTableResolution * hpbTilesPerRow,
          .height = settings_.virtualPageTableResolution * hpbTilesPerColumn,
          .depth = 1u,
          .mipLevels = hierarchicalPageBoundsMipCount_,
          .frameLocal = true,
      });

  virtualPageTableCPU_.assign(virtualPageTableBufferSize_ / sizeof(uint32_t), 0u);
  physicalPageTableCPU_.assign(physicalPageTableBufferSize_ / sizeof(uint32_t), 0u);
  virtualPageStatesPerFrameCPU_.assign(
      std::max(1u, renderGraph_->getMaxFramesInFlight()),
      std::vector<uint32_t>(virtualPageStateBufferSize_ / sizeof(uint32_t), 0u));
  invalidationMasksPerFrameCPU_.assign(
      std::max(1u, renderGraph_->getMaxFramesInFlight()),
      std::vector<uint32_t>(invalidationMaskBufferSize_ / sizeof(uint32_t), 0u));
  cascadeStatesCPU_.assign(getMaxLayerCount(), {});
  cascadeMatricesCPU_.assign(getMaxLayerCount(), {});
  directionalLightsCPU_.assign(std::max<uint32_t>(1u, getTotalConfiguredLightCount()), {});
  softShadowLightsCPU_.assign(std::max<uint32_t>(1u, getTotalConfiguredLightCount()), {});
  rebuildSoftShadowLayers();

  initializeFrameOverrideResources();
  initializeFrameLocalResources();
}

void VirtualShadowMapManager::initializeFrameOverrideResources()
{
  const auto resourceName = [this](const std::string &suffix) -> std::string
  {
    return settings_.resourceNamePrefix + suffix;
  };
  const uint32_t frameCount = std::max(1u, renderGraph_->getMaxFramesInFlight());
  frameOverrideResources_.assign(frameCount, {});

  rendering::RHI *const rhi = renderGraph_->getRHI();
  for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
  {
    FrameOverrideResources &frameResources = frameOverrideResources_[frameSlot];
    const std::string suffix = "_frame" + std::to_string(frameSlot);
    const uint32_t hpbTilesPerRow = computeAtlasTilesPerRow(getMaxLayerCount());
    const uint32_t hpbTilesPerColumn = std::max(1u, (getMaxLayerCount() + hpbTilesPerRow - 1u) / hpbTilesPerRow);

    frameResources.virtualPageStateBufferId = rhi->createBuffer(
        rendering::BufferInfo{
            .name = resourceName("_PageState.override" + suffix),
            .size = virtualPageStateBufferSize_,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    frameResources.invalidationMaskBufferId = rhi->createBuffer(
        rendering::BufferInfo{
            .name = resourceName("_Invalidations.override" + suffix),
            .size = invalidationMaskBufferSize_,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    frameResources.cascadeStatesBufferId = rhi->createBuffer(
        rendering::BufferInfo{
            .name = resourceName("_CascadeStates.override" + suffix),
            .size = cascadeStatesBufferSize_,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    frameResources.cascadeMatricesBufferId = rhi->createBuffer(
        rendering::BufferInfo{
            .name = resourceName("_CascadeMatrices.override" + suffix),
            .size = cascadeMatricesBufferSize_,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    frameResources.cameraStateBufferId = rhi->createBuffer(
        rendering::BufferInfo{
            .name = resourceName("_CameraState.override" + suffix),
            .size = cameraStateBufferSize_,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    frameResources.directionalLightsBufferId = rhi->createBuffer(
        rendering::BufferInfo{
            .name = resourceName("_DirectionalLights.override" + suffix),
            .size = directionalLightsBufferSize_,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    frameResources.softShadowLightsBufferId = rhi->createBuffer(
        rendering::BufferInfo{
            .name = resourceName("_SoftShadowLights.override" + suffix),
            .size = softShadowLightsBufferSize_,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    frameResources.allocationRequestsBufferId = rhi->createBuffer(
        rendering::BufferInfo{
            .name = resourceName("_AllocationRequests.override" + suffix),
            .size = allocationRequestsBufferSize_,
            .usage = rendering::BufferUsage::BufferUsage_Storage,
        });
    frameResources.futureAllocationRequestsBufferId = rhi->createBuffer(
        rendering::BufferInfo{
            .name = resourceName("_FutureAllocationRequests.override" + suffix),
            .size = futureAllocationRequestsBufferSize_,
            .usage = rendering::BufferUsage::BufferUsage_Storage,
        });
    frameResources.hierarchicalPageBoundsTextureId = rhi->createTexture(
        rendering::TextureInfo{
            .name = resourceName("_HPB.override" + suffix),
            .format = rendering::Format::Format_R32Float,
            .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
            .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
            .width = settings_.virtualPageTableResolution * hpbTilesPerRow,
            .height = settings_.virtualPageTableResolution * hpbTilesPerColumn,
            .depth = 1u,
            .mipLevels = hierarchicalPageBoundsMipCount_,
        });
  }
}

void VirtualShadowMapManager::initializeFrameLocalResources()
{
  const uint32_t previousFrameSlot = renderGraph_->getCurrentFrameIndex();

  uploadPageTables();

  std::vector<uint32_t> initialFreePages(unallocatedPhysicalPagesBufferSize_ / sizeof(uint32_t), 0u);
  for (uint32_t y = 0u; y < physicalPageTableResolution_; ++y)
  {
    for (uint32_t x = 0u; x < physicalPageTableResolution_; ++x)
    {
      initialFreePages[y * physicalPageTableResolution_ + x] = (x & 0xFFFFu) | ((y & 0xFFFFu) << 16u);
    }
  }
  renderGraph_->bufferWrite(unallocatedPhysicalPagesBuffer_, 0, unallocatedPhysicalPagesBufferSize_, initialFreePages.data());

  std::vector<uint32_t> emptyReclaimable(reclaimablePhysicalPagesBufferSize_ / sizeof(uint32_t), 0u);
  renderGraph_->bufferWrite(reclaimablePhysicalPagesBuffer_, 0, reclaimablePhysicalPagesBufferSize_, emptyReclaimable.data());

  std::vector<uint32_t> initialCounters(allocatorCountersBufferSize_ / sizeof(uint32_t), 0u);
  initialCounters[2] = static_cast<uint32_t>(initialFreePages.size());
  renderGraph_->bufferWrite(allocatorCountersBuffer_, 0, allocatorCountersBufferSize_, initialCounters.data());

  const uint32_t maxFramesInFlight = std::max(1u, renderGraph_->getMaxFramesInFlight());
  for (uint32_t frameSlot = 0u; frameSlot < maxFramesInFlight; ++frameSlot)
  {
    initializeFrameLocalResourcesForSlot(frameSlot);
  }
  renderGraph_->setCurrentFrameIndex(previousFrameSlot);
}

void VirtualShadowMapManager::initializeFrameLocalResourcesForSlot(uint32_t frameSlot)
{
  const FrameOverrideResources &frameResources = getFrameOverrideResources(frameSlot);
  rendering::RHI *const rhi = renderGraph_->getRHI();

  std::vector<uint32_t> emptyPageStates(virtualPageStateBufferSize_ / sizeof(uint32_t), 0u);
  rhi->bufferWrite(frameResources.virtualPageStateBufferId, 0, virtualPageStateBufferSize_, emptyPageStates.data());

  std::vector<uint32_t> emptyInvalidations(invalidationMaskBufferSize_ / sizeof(uint32_t), 0u);
  rhi->bufferWrite(frameResources.invalidationMaskBufferId, 0, invalidationMaskBufferSize_, emptyInvalidations.data());

  rhi->bufferWrite(frameResources.cascadeStatesBufferId, 0, cascadeStatesBufferSize_, cascadeStatesCPU_.data());
  rhi->bufferWrite(frameResources.cascadeMatricesBufferId, 0, cascadeMatricesBufferSize_, cascadeMatricesCPU_.data());
  rhi->bufferWrite(frameResources.cameraStateBufferId, 0, cameraStateBufferSize_, &cameraStateCPU_);
  rhi->bufferWrite(frameResources.directionalLightsBufferId, 0, directionalLightsBufferSize_, directionalLightsCPU_.data());
  rhi->bufferWrite(frameResources.softShadowLightsBufferId, 0, softShadowLightsBufferSize_, softShadowLightsCPU_.data());
}

void VirtualShadowMapManager::uploadPageTables()
{
  renderGraph_->bufferWrite(virtualPageTableBuffer_, 0, virtualPageTableBufferSize_, virtualPageTableCPU_.data());
  renderGraph_->bufferWrite(physicalPageTableBuffer_, 0, physicalPageTableBufferSize_, physicalPageTableCPU_.data());
}

void VirtualShadowMapManager::uploadPageStates()
{
  std::vector<uint32_t> &pageStatesCPU = getCurrentPageStatesCPU();
  renderGraph_->getRHI()->bufferWrite(getCurrentFrameOverrideResources().virtualPageStateBufferId, 0, virtualPageStateBufferSize_, pageStatesCPU.data());
}

void VirtualShadowMapManager::uploadInvalidationMasks()
{
  std::vector<uint32_t> &invalidationMasksCPU = getCurrentInvalidationMasksCPU();
  renderGraph_->getRHI()->bufferWrite(getCurrentFrameOverrideResources().invalidationMaskBufferId, 0, invalidationMaskBufferSize_, invalidationMasksCPU.data());
}

void VirtualShadowMapManager::uploadCascadeState()
{
  renderGraph_->getRHI()->bufferWrite(getCurrentFrameOverrideResources().cascadeStatesBufferId, 0, cascadeStatesBufferSize_, cascadeStatesCPU_.data());
}

void VirtualShadowMapManager::uploadCascadeMatrices()
{
  renderGraph_->getRHI()->bufferWrite(getCurrentFrameOverrideResources().cascadeMatricesBufferId, 0, cascadeMatricesBufferSize_, cascadeMatricesCPU_.data());
}

void VirtualShadowMapManager::uploadDirectionalLights()
{
  for (size_t lightIndex = 0; lightIndex < directionalLightsCPU_.size(); ++lightIndex)
  {
    DirectionalLightGPU &lightGPU = directionalLightsCPU_[lightIndex];
    const bool isActive = lightIndex < directionalLights_.size();
    const DirectionalLightRuntime &lightRuntime = isActive ? directionalLights_[lightIndex] : DirectionalLightRuntime{};
    const math::Vec3f position = isActive ? lightRuntime.position : math::Vec3f(0.0f, 0.0f, 0.0f);
    const math::Vec3f direction = isActive ? lightRuntime.direction.normalize() : math::Vec3f(0.0f, -1.0f, 0.0f);
    const math::Vec3f color = isActive ? lightRuntime.color : math::Vec3f(0.0f, 0.0f, 0.0f);
    lightGPU.position[0] = position[0];
    lightGPU.position[1] = position[1];
    lightGPU.position[2] = position[2];
    lightGPU.position[3] = 1.0f;
    lightGPU.direction[0] = direction[0];
    lightGPU.direction[1] = direction[1];
    lightGPU.direction[2] = direction[2];
    lightGPU.direction[3] = 0.0f;
    lightGPU.color[0] = color[0];
    lightGPU.color[1] = color[1];
    lightGPU.color[2] = color[2];
    lightGPU.color[3] = isActive ? std::max(lightRuntime.intensity, 0.0f) : 0.0f;
    lightGPU.params[0] = isActive ? lightRuntime.range : 0.0f;
    lightGPU.params[1] = isActive ? lightRuntime.outerAngleRadians : 0.0f;
    lightGPU.params[2] = 0.0f;
    lightGPU.params[3] = 0.0f;
    lightGPU.metadata[0] = isActive ? static_cast<uint32_t>(lightRuntime.type) : static_cast<uint32_t>(LightType::Directional);
    lightGPU.metadata[1] = isActive ? lightRuntime.firstLayer : 0u;
    lightGPU.metadata[2] = isActive ? getLightLayerCount(lightRuntime) : 0u;
    lightGPU.metadata[3] = isActive && lightRuntime.type != LightType::Directional ? perspectiveLightMipCount_ : 1u;
  }

  renderGraph_->getRHI()->bufferWrite(getCurrentFrameOverrideResources().directionalLightsBufferId, 0, directionalLightsBufferSize_, directionalLightsCPU_.data());
}

void VirtualShadowMapManager::uploadSoftShadowLights()
{
  for (size_t lightIndex = 0; lightIndex < softShadowLightsCPU_.size(); ++lightIndex)
  {
    SoftShadowLightGPU &lightGPU = softShadowLightsCPU_[lightIndex];
    const bool isActive = lightIndex < directionalLights_.size();
    const DirectionalLightRuntime &lightRuntime = isActive ? directionalLights_[lightIndex] : DirectionalLightRuntime{};
    lightGPU.layer = isActive ? lightRuntime.softShadowLayer : 0u;
    lightGPU.flags = isActive && lightRuntime.softShadowFallback ? 1u : 0u;
    lightGPU.lightRadiusWorld = isActive ? std::max(lightRuntime.softShadowLightRadiusWorld, 0.0f) : 0.0f;
    lightGPU.penumbraScale = isActive ? std::max(lightRuntime.softShadowPenumbraScale, 0.0f) : 1.0f;
  }

  renderGraph_->getRHI()->bufferWrite(getCurrentFrameOverrideResources().softShadowLightsBufferId, 0, softShadowLightsBufferSize_, softShadowLightsCPU_.data());
}

void VirtualShadowMapManager::rebuildSoftShadowLayers()
{
  std::vector<SoftShadowLightVolume> volumes;
  volumes.reserve(directionalLights_.size());
  for (const DirectionalLightRuntime &lightRuntime : directionalLights_)
  {
    SoftShadowLightVolume volume{};
    volume.type = static_cast<SoftShadowLightType>(static_cast<uint32_t>(lightRuntime.type));
    volume.position = lightRuntime.position;
    volume.direction = lightRuntime.direction;
    volume.range = lightRuntime.type == LightType::Directional ? std::numeric_limits<float>::infinity() : lightRuntime.range;
    volume.outerAngleRadians = lightRuntime.outerAngleRadians;
    volume.shadowCasting = lightRuntime.intensity > 0.0f;
    volumes.push_back(volume);
  }

  const SoftShadowLayerBuildResult assignment = buildSoftShadowLayerAssignment(volumes, getMaxSoftShadowLayerCount());
  activeSoftShadowLayerCount_ = assignment.activeLayerCount;
  softShadowMaxGraphDegree_ = assignment.maxGraphDegree;
  softShadowOverflowCount_ = assignment.overflowCount;

  for (uint32_t lightIndex = 0u; lightIndex < directionalLights_.size(); ++lightIndex)
  {
    DirectionalLightRuntime &lightRuntime = directionalLights_[lightIndex];
    if (lightIndex < assignment.assignments.size())
    {
      lightRuntime.softShadowLayer = assignment.assignments[lightIndex].layer;
      lightRuntime.softShadowFallback = assignment.assignments[lightIndex].fallback;
    }
    else
    {
      lightRuntime.softShadowLayer = 0u;
      lightRuntime.softShadowFallback = true;
    }
  }
}

float VirtualShadowMapManager::computeCascadeWorldExtent(uint32_t cascadeIndex) const
{
  const float cascadeScale = std::pow(settings_.cascadeWorldExtentScale, static_cast<float>(cascadeIndex));
  return getFirstCascadeWorldExtent() * cascadeScale;
}

void VirtualShadowMapManager::computeCascadeState(uint32_t lightIndex, uint32_t localLayerIndex, const rendering::Camera &camera)
{
  DirectionalLightRuntime &lightRuntime = directionalLights_[lightIndex];
  CascadeRuntime &cascadeRuntime = lightRuntime.cascades[localLayerIndex];
  const bool isPointLight = lightRuntime.type == LightType::Point;
  const bool isSpotLight = lightRuntime.type == LightType::Spot;
  const uint32_t faceIndex = isPointLight ? (localLayerIndex / perspectiveLightMipCount_) : 0u;
  const uint32_t mipLevel = isPointLight ? (localLayerIndex % perspectiveLightMipCount_) : (isSpotLight ? localLayerIndex : 0u);

  if (lightRuntime.type == LightType::Directional)
  {
    const math::Vec3f direction = lightRuntime.direction.normalize();
    const math::Vec3f upSeed = chooseStableUp(direction);
    // Keep the quantized page grid in the exact same basis used by the light
    // view matrix. Using the opposite cross-product order here makes the page
    // ring advance in the opposite X direction from projected cascade UVs.
    const math::Vec3f right = direction.cross(upSeed).normalize();
    const math::Vec3f lightUp = right.cross(direction).normalize();
    const math::Vec3f cameraPosition = camera.getPosition();

    cascadeRuntime.worldExtent = computeCascadeWorldExtent(localLayerIndex);
    cascadeRuntime.pageWorldSize = cascadeRuntime.worldExtent / static_cast<float>(settings_.virtualPageTableResolution);

    const int32_t quantizedX = static_cast<int32_t>(std::floor(cameraPosition.dot(right) / cascadeRuntime.pageWorldSize));
    const int32_t quantizedY = static_cast<int32_t>(std::floor(cameraPosition.dot(lightUp) / cascadeRuntime.pageWorldSize));

    cascadeRuntime.pageShift = {0, 0};
    if (cascadeRuntime.hasPreviousQuantizedCenter)
    {
      cascadeRuntime.pageShift = {
        quantizedX - cascadeRuntime.quantizedCenter[0],
        quantizedY - cascadeRuntime.quantizedCenter[1],
      };
    }
    cascadeRuntime.quantizedCenter = {quantizedX, quantizedY};
    cascadeRuntime.hasPreviousQuantizedCenter = true;
    cascadeRuntime.pageOffset = {
      static_cast<int32_t>(positiveModulo(quantizedX, settings_.virtualPageTableResolution)),
      static_cast<int32_t>(positiveModulo(quantizedY, settings_.virtualPageTableResolution)),
    };

    const float snappedCenterX = static_cast<float>(quantizedX) * cascadeRuntime.pageWorldSize;
    const float snappedCenterY = static_cast<float>(quantizedY) * cascadeRuntime.pageWorldSize;
    // Keep the shadow depth reference stable in light space. The page ring already
    // tracks camera motion in X/Y via pageOffset/pageShift, so moving the light
    // frustum along the light direction makes previously rendered pages represent
    // different world-space depths.
    const math::Vec3f snappedTarget = right * snappedCenterX + lightUp * snappedCenterY;
    const math::Vec3f lightPosition = snappedTarget - direction * settings_.lightDistance;

    const float halfExtent = cascadeRuntime.worldExtent * 0.5f;
    rendering::LightCamera lightCamera = rendering::LightCamera::orthographic(
        lightPosition,
        snappedTarget,
        rendering::LightCamera::OrthographicDesc{
            .left = -halfExtent,
            .right = halfExtent,
            .bottom = -halfExtent,
            .top = halfExtent,
            .nearPlane = 0.1f,
            .farPlane = settings_.lightDistance * 2.0f,
            .reverseZ = settings_.reverseZ,
        },
        lightUp);

    cascadeRuntime.view = lightCamera.getViewMatrix();
    cascadeRuntime.proj = lightCamera.getProjectionMatrix();
    cascadeRuntime.viewProj = cascadeRuntime.proj * cascadeRuntime.view;
    cascadeRuntime.worldExtent = computeCascadeWorldExtent(localLayerIndex);
  }
  else
  {
    const uint32_t effectivePageTableResolution = std::max(1u, settings_.virtualPageTableResolution >> mipLevel);
    const math::Vec3f direction = isPointLight ? getPointLightFaceDirection(faceIndex) : lightRuntime.direction.normalize();
    const math::Vec3f lightUp = isPointLight ? getPointLightFaceUp(faceIndex) : chooseStableUp(direction);
    const float lightRange = std::max(lightRuntime.range, 0.1f);
    const float fovY = isPointLight ? (PI_F * 0.5f) : lightRuntime.outerAngleRadians;
    const float halfFovTan = std::tan(fovY * 0.5f);
    // Keep perspective-light pages responsive to the global page world scale too,
    // so point/spot allocations grow in world space the same way directional pages do.
    cascadeRuntime.worldExtent = lightRange * 2.0f * halfFovTan * settings_.pageWorldScale;
    cascadeRuntime.pageWorldSize = std::max(cascadeRuntime.worldExtent / static_cast<float>(effectivePageTableResolution), 1e-4f);
    cascadeRuntime.pageShift = {0, 0};
    cascadeRuntime.pageOffset = {0, 0};
    cascadeRuntime.quantizedCenter = {0, 0};
    cascadeRuntime.hasPreviousQuantizedCenter = false;

    rendering::LightCamera lightCamera = rendering::LightCamera::perspective(
        lightRuntime.position,
        lightRuntime.position + direction,
        rendering::LightCamera::PerspectiveDesc{
            .fovY = fovY,
            .aspect = 1.0f,
            .nearPlane = 0.1f,
            .farPlane = lightRange,
            .reverseZ = settings_.reverseZ,
        },
        lightUp);

    cascadeRuntime.view = lightCamera.getViewMatrix();
    cascadeRuntime.proj = lightCamera.getProjectionMatrix();
    cascadeRuntime.viewProj = cascadeRuntime.proj * cascadeRuntime.view;
  }
  const uint32_t layerIndex = lightRuntime.firstLayer + localLayerIndex;
  CascadeStateGPU &cascadeState = cascadeStatesCPU_[layerIndex];
  cascadeState.pageOffset[0] = cascadeRuntime.pageOffset[0];
  cascadeState.pageOffset[1] = cascadeRuntime.pageOffset[1];
  cascadeState.pageShift[0] = cascadeRuntime.pageShift[0];
  cascadeState.pageShift[1] = cascadeRuntime.pageShift[1];

  CascadeMatrixGPU &cascadeMatrix = cascadeMatricesCPU_[layerIndex];
  std::memcpy(cascadeMatrix.view, cascadeRuntime.view.data, sizeof(cascadeMatrix.view));
  std::memcpy(cascadeMatrix.proj, cascadeRuntime.proj.data, sizeof(cascadeMatrix.proj));
  std::memcpy(cascadeMatrix.viewProj, cascadeRuntime.viewProj.data, sizeof(cascadeMatrix.viewProj));
  cascadeMatrix.worldExtent = cascadeRuntime.worldExtent;
  cascadeMatrix.pageWorldSize = cascadeRuntime.pageWorldSize;
  cascadeMatrix.lightIndex = lightIndex;
  cascadeMatrix.cascadeIndex = localLayerIndex;
  cascadeMatrix.pageTableResolution = lightRuntime.type == LightType::Directional
                                          ? settings_.virtualPageTableResolution
                                          : std::max(1u, settings_.virtualPageTableResolution >> mipLevel);
  cascadeMatrix.lightType = static_cast<uint32_t>(lightRuntime.type);
  cascadeMatrix.faceIndex = isPointLight ? faceIndex : 0u;
  cascadeMatrix.mipLevel = mipLevel;
}

bool VirtualShadowMapManager::computeInvalidatedPageRectForLayer(
    uint32_t layerIndex,
    const AABB &region,
    uint32_t *pageMinX,
    uint32_t *pageMaxX,
    uint32_t *pageMinY,
    uint32_t *pageMaxY) const
{
  if (layerIndex >= getActiveLayerCount())
  {
    return false;
  }

  const CascadeMatrixGPU &cascadeMatrix = cascadeMatricesCPU_[layerIndex];
  math::Mat4f viewProj{};
  std::memcpy(viewProj.data, cascadeMatrix.viewProj, sizeof(cascadeMatrix.viewProj));
  float clampedMinU = 0.0f;
  float clampedMaxU = 1.0f;
  float clampedMinV = 0.0f;
  float clampedMaxV = 1.0f;
  const LightType lightType = static_cast<LightType>(cascadeMatrix.lightType);
  if (lightType == LightType::Directional)
  {
    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float minZ = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();
    float maxZ = std::numeric_limits<float>::lowest();

    for (uint32_t cornerIndex = 0u; cornerIndex < 8u; ++cornerIndex)
    {
      const math::Vec3f worldCorner = makeAABBCorner(region, cornerIndex);
      const math::Vec4f clip = viewProj * math::Vec4f(worldCorner[0], worldCorner[1], worldCorner[2], 1.0f);
      const float invW = std::abs(clip[3]) > 1e-6f ? 1.0f / clip[3] : 1.0f;
      const float ndcX = clip[0] * invW;
      const float ndcY = clip[1] * invW;
      const float ndcZ = clip[2] * invW;

      minX = std::min(minX, ndcX);
      minY = std::min(minY, ndcY);
      minZ = std::min(minZ, ndcZ);
      maxX = std::max(maxX, ndcX);
      maxY = std::max(maxY, ndcY);
      maxZ = std::max(maxZ, ndcZ);
    }

    if (maxX < -1.0f || minX > 1.0f || maxY < -1.0f || minY > 1.0f)
    {
      return false;
    }
    if (maxZ < 0.0f || minZ > 1.0f)
    {
      return false;
    }

    clampedMinU = std::clamp(minX * 0.5f + 0.5f, 0.0f, 1.0f);
    clampedMaxU = std::clamp(maxX * 0.5f + 0.5f, 0.0f, 1.0f);
    clampedMinV = std::clamp(minY * 0.5f + 0.5f, 0.0f, 1.0f);
    clampedMaxV = std::clamp(maxY * 0.5f + 0.5f, 0.0f, 1.0f);
  }
  else
  {
    math::Mat4f view{};
    math::Mat4f proj{};
    std::memcpy(view.data, cascadeMatrix.view, sizeof(cascadeMatrix.view));
    std::memcpy(proj.data, cascadeMatrix.proj, sizeof(cascadeMatrix.proj));
    if (!projectAABBToPerspectiveUvBounds(region, view, proj, &clampedMinU, &clampedMaxU, &clampedMinV, &clampedMaxV))
    {
      return false;
    }
  }

  const uint32_t layerPageTableResolution = std::max(1u, cascadeMatrix.pageTableResolution);
  const int32_t rectPageMinX = std::clamp(static_cast<int32_t>(std::floor(clampedMinU * layerPageTableResolution)), 0, static_cast<int32_t>(layerPageTableResolution) - 1);
  const int32_t rectPageMaxX = std::clamp(static_cast<int32_t>(std::ceil(clampedMaxU * layerPageTableResolution)) - 1, 0, static_cast<int32_t>(layerPageTableResolution) - 1);
  const int32_t rectPageMinY = std::clamp(static_cast<int32_t>(std::floor(clampedMinV * layerPageTableResolution)), 0, static_cast<int32_t>(layerPageTableResolution) - 1);
  const int32_t rectPageMaxY = std::clamp(static_cast<int32_t>(std::ceil(clampedMaxV * layerPageTableResolution)) - 1, 0, static_cast<int32_t>(layerPageTableResolution) - 1);

  if (rectPageMinX > rectPageMaxX || rectPageMinY > rectPageMaxY)
  {
    return false;
  }

  *pageMinX = static_cast<uint32_t>(rectPageMinX);
  *pageMaxX = static_cast<uint32_t>(rectPageMaxX);
  *pageMinY = static_cast<uint32_t>(rectPageMinY);
  *pageMaxY = static_cast<uint32_t>(rectPageMaxY);
  return true;
}

void VirtualShadowMapManager::markRegionInvalidatedForLayer(uint32_t layerIndex, const AABB &region)
{
  uint32_t pageMinX = 0u;
  uint32_t pageMaxX = 0u;
  uint32_t pageMinY = 0u;
  uint32_t pageMaxY = 0u;
  if (!computeInvalidatedPageRectForLayer(layerIndex, region, &pageMinX, &pageMaxX, &pageMinY, &pageMaxY))
  {
    return;
  }

  std::vector<uint32_t> &invalidationMasksCPU = getCurrentInvalidationMasksCPU();
  for (uint32_t y = pageMinY; y <= pageMaxY; ++y)
  {
    const uint32_t wordIndex = layerIndex * INVALIDATION_MASK_WORDS_PER_LAYER + y;
    for (uint32_t x = pageMinX; x <= pageMaxX; ++x)
    {
      invalidationMasksCPU[wordIndex] |= (1u << x);
    }
  }
}

void VirtualShadowMapManager::flushQueuedInvalidations()
{
  std::vector<uint32_t> &invalidationMasksCPU = getCurrentInvalidationMasksCPU();

  if (queuedFullInvalidation_)
  {
    std::fill(invalidationMasksCPU.begin(), invalidationMasksCPU.end(), 0xFFFFFFFFu);
    queuedFullInvalidation_ = false;
    queuedInvalidationRegions_.clear();
    queuedLightInvalidations_.clear();
    return;
  }

  for (const PendingLightInvalidation &pending : queuedLightInvalidations_)
  {
    if (pending.lightId >= directionalLights_.size())
    {
      continue;
    }

    const DirectionalLightRuntime &lightRuntime = directionalLights_[pending.lightId];
    const uint32_t lightLayerCount = getLightLayerCount(lightRuntime);
    for (uint32_t localLayerIndex = 0u; localLayerIndex < lightLayerCount; ++localLayerIndex)
    {
      const uint32_t layerIndex = lightRuntime.firstLayer + localLayerIndex;
      if (layerIndex >= getActiveLayerCount())
      {
        continue;
      }

      const uint32_t layerPageTableResolution = std::min<uint32_t>(
          std::max(1u, cascadeMatricesCPU_[layerIndex].pageTableResolution),
          INVALIDATION_MASK_WORDS_PER_LAYER);
      const uint32_t rowMask = layerPageTableResolution >= 32u ? 0xFFFFFFFFu : ((1u << layerPageTableResolution) - 1u);
      for (uint32_t y = 0u; y < layerPageTableResolution; ++y)
      {
        invalidationMasksCPU[layerIndex * INVALIDATION_MASK_WORDS_PER_LAYER + y] |= rowMask;
      }
    }
  }
  queuedLightInvalidations_.clear();

  for (const PendingRegionInvalidation &pending : queuedInvalidationRegions_)
  {
    for (uint32_t lightIndex = 0u; lightIndex < directionalLights_.size(); ++lightIndex)
    {
      const DirectionalLightRuntime &lightRuntime = directionalLights_[lightIndex];
      const uint32_t lightLayerCount = getLightLayerCount(lightRuntime);
      for (uint32_t localLayerIndex = 0u; localLayerIndex < lightLayerCount; ++localLayerIndex)
      {
        markRegionInvalidatedForLayer(lightRuntime.firstLayer + localLayerIndex, pending.region);
      }
    }
  }
  queuedInvalidationRegions_.clear();
}

uint32_t VirtualShadowMapManager::getLightLayerCount(const DirectionalLightRuntime &lightRuntime) const
{
  switch (lightRuntime.type)
  {
  case LightType::Directional:
    return settings_.cascadeCount;
  case LightType::Spot:
    return perspectiveLightMipCount_;
  case LightType::Point:
    return POINT_LIGHT_FACE_COUNT * perspectiveLightMipCount_;
  }

  return 0u;
}

uint32_t VirtualShadowMapManager::getTotalConfiguredLightCount() const
{
  return settings_.maxDirectionalLights + settings_.maxSpotLights + settings_.maxPointLights;
}

uint32_t VirtualShadowMapManager::getCurrentFrameSlot() const
{
  if (renderGraph_ == nullptr)
  {
    return 0u;
  }

  const uint32_t maxFramesInFlight = std::max(1u, renderGraph_->getMaxFramesInFlight());
  return renderGraph_->getCurrentFrameIndex() % maxFramesInFlight;
}

std::vector<uint32_t> &VirtualShadowMapManager::getCurrentPageStatesCPU()
{
  return virtualPageStatesPerFrameCPU_[getCurrentFrameSlot()];
}

const std::vector<uint32_t> &VirtualShadowMapManager::getCurrentPageStatesCPU() const
{
  return virtualPageStatesPerFrameCPU_[getCurrentFrameSlot()];
}

void VirtualShadowMapManager::clearCurrentPageStatesCPU()
{
  std::vector<uint32_t> &pageStatesCPU = getCurrentPageStatesCPU();
  std::fill(pageStatesCPU.begin(), pageStatesCPU.end(), 0u);
}

std::vector<uint32_t> &VirtualShadowMapManager::getCurrentInvalidationMasksCPU()
{
  return invalidationMasksPerFrameCPU_[getCurrentFrameSlot()];
}

const std::vector<uint32_t> &VirtualShadowMapManager::getCurrentInvalidationMasksCPU() const
{
  return invalidationMasksPerFrameCPU_[getCurrentFrameSlot()];
}

const VirtualShadowMapManager::FrameOverrideResources &VirtualShadowMapManager::getFrameOverrideResources(uint32_t frameSlot) const
{
  const uint32_t frameCount = static_cast<uint32_t>(frameOverrideResources_.size());
  return frameOverrideResources_[frameCount == 0u ? 0u : (frameSlot % frameCount)];
}

const VirtualShadowMapManager::FrameOverrideResources &VirtualShadowMapManager::getCurrentFrameOverrideResources() const
{
  return getFrameOverrideResources(getCurrentFrameSlot());
}

void VirtualShadowMapManager::clearCurrentInvalidationMasksCPU()
{
  std::vector<uint32_t> &invalidationMasksCPU = getCurrentInvalidationMasksCPU();
  std::fill(invalidationMasksCPU.begin(), invalidationMasksCPU.end(), 0u);
}

uint32_t VirtualShadowMapManager::positiveModulo(int32_t value, uint32_t modulus)
{
  const int32_t signedModulus = static_cast<int32_t>(modulus);
  const int32_t result = value % signedModulus;
  return static_cast<uint32_t>(result < 0 ? (result + signedModulus) : result);
}

math::Vec3f VirtualShadowMapManager::chooseStableUp(const math::Vec3f &direction)
{
  const math::Vec3f worldUp(0.0f, 1.0f, 0.0f);
  if (std::abs(direction.normalize().dot(worldUp)) > WORLD_UP_DOT_THRESHOLD)
  {
    return math::Vec3f(1.0f, 0.0f, 0.0f);
  }
  return worldUp;
}

} // namespace virtualgeometry
