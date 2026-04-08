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
constexpr uint32_t MAX_PACKED_PHYSICAL_PAGE_COORD = 0xFFu;

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

inline math::Vec3f makeAABBCorner(const AABB &region, uint32_t cornerIndex)
{
  return math::Vec3f(
      (cornerIndex & 1u) ? region.maxPoint[0] : region.minPoint[0],
      (cornerIndex & 2u) ? region.maxPoint[1] : region.minPoint[1],
      (cornerIndex & 4u) ? region.maxPoint[2] : region.minPoint[2]);
}

} // namespace

VirtualShadowMapManager::VirtualShadowMapManager(rendering::RenderGraph *renderGraph, Settings settings) : renderGraph_(renderGraph), settings_(settings)
{
  if (renderGraph_ == nullptr)
  {
    throw std::runtime_error("VirtualShadowMapManager requires a valid RenderGraph");
  }
  if (settings_.virtualPageTableResolution != DEFAULT_PAGE_TABLE_RESOLUTION)
  {
    throw std::runtime_error("VirtualShadowMapManager currently expects a 32x32 virtual page table resolution");
  }
  if (settings_.physicalPageSize == 0u || settings_.physicalAtlasResolution == 0u || settings_.cascadeCount == 0u || settings_.maxDirectionalLights == 0u)
  {
    throw std::runtime_error("VirtualShadowMapManager settings must all be non-zero");
  }
  if (!std::isfinite(settings_.pageWorldScale) || settings_.pageWorldScale <= 0.0f)
  {
    throw std::runtime_error("VirtualShadowMapManager pageWorldScale must be finite and greater than zero");
  }

  physicalPageTableResolution_ = settings_.physicalAtlasResolution / settings_.physicalPageSize;
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

VirtualShadowMapManager::LightId VirtualShadowMapManager::createDirectionalLight(const math::Vec3f &direction, const math::Vec3f &color)
{
  if (directionalLights_.size() >= settings_.maxDirectionalLights)
  {
    throw std::runtime_error("VirtualShadowMapManager exceeded maxDirectionalLights");
  }

  DirectionalLightRuntime lightRuntime;
  lightRuntime.direction = direction.normalize();
  lightRuntime.color = color;
  lightRuntime.cascades.resize(settings_.cascadeCount);
  directionalLights_.push_back(lightRuntime);
  directionalLightsCPU_.resize(std::max<uint32_t>(1u, settings_.maxDirectionalLights));

  const uint32_t lightIndex = static_cast<uint32_t>(directionalLights_.size() - 1u);
  return lightIndex;
}

void VirtualShadowMapManager::setDirectionalLightDirection(LightId lightId, const math::Vec3f &direction)
{
  if (lightId >= directionalLights_.size())
  {
    return;
  }

  const math::Vec3f normalizedDirection = direction.normalize();
  const math::Vec3f previousDirection = directionalLights_[lightId].direction;
  directionalLights_[lightId].direction = normalizedDirection;

  if ((normalizedDirection - previousDirection).length() > 1e-5f)
  {
    queueInvalidateAllPages();
  }
}

void VirtualShadowMapManager::setDirectionalLightColor(LightId lightId, const math::Vec3f &color)
{
  if (lightId >= directionalLights_.size())
  {
    return;
  }

  directionalLights_[lightId].color = color;
}

void VirtualShadowMapManager::update(const rendering::Camera &camera)
{
  const math::Mat4f inverseView = camera.getInverseViewMatrix();
  const math::Mat4f inverseProjection = camera.getInverseProjectionMatrix();
  const math::Mat4f viewProjection = camera.getProjectionMatrix() * camera.getViewMatrix();
  const math::Mat4f inverseViewProjection = (camera.getProjectionMatrix() * camera.getViewMatrix()).inverse();
  std::memcpy(cameraStateCPU_.inverseView, inverseView.data, sizeof(cameraStateCPU_.inverseView));
  std::memcpy(cameraStateCPU_.inverseProjection, inverseProjection.data, sizeof(cameraStateCPU_.inverseProjection));
  std::memcpy(cameraStateCPU_.inverseViewProjection, inverseViewProjection.data, sizeof(cameraStateCPU_.inverseViewProjection));
  std::memcpy(cameraStateCPU_.viewProjection, viewProjection.data, sizeof(cameraStateCPU_.viewProjection));
  cameraStateCPU_.cameraPosition[0] = camera.getPosition()[0];
  cameraStateCPU_.cameraPosition[1] = camera.getPosition()[1];
  cameraStateCPU_.cameraPosition[2] = camera.getPosition()[2];
  cameraStateCPU_.cameraPosition[3] = 1.0f;
  cameraStateCPU_.reverseZ = camera.isReverseZ() ? 1u : 0u;

  for (uint32_t lightIndex = 0u; lightIndex < directionalLights_.size(); ++lightIndex)
  {
    for (uint32_t cascadeIndex = 0u; cascadeIndex < settings_.cascadeCount; ++cascadeIndex)
    {
      computeCascadeState(lightIndex, cascadeIndex, camera);
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
  renderGraph_->getRHI()->bufferWrite(getCurrentFrameOverrideResources().cameraStateBufferId, 0, cameraStateBufferSize_, &cameraStateCPU_);
}

void VirtualShadowMapManager::invalidateRegion(const AABB &region)
{
  for (uint32_t lightIndex = 0u; lightIndex < directionalLights_.size(); ++lightIndex)
  {
    for (uint32_t cascadeIndex = 0u; cascadeIndex < settings_.cascadeCount; ++cascadeIndex)
    {
      const uint32_t layerIndex = lightIndex * settings_.cascadeCount + cascadeIndex;
      markRegionInvalidatedForLayer(layerIndex, region);
    }
  }

  uploadInvalidationMasks();
}

void VirtualShadowMapManager::queueInvalidateRegion(const AABB &region)
{
  queuedInvalidationRegions_.push_back(region);
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

uint32_t VirtualShadowMapManager::getActiveDirectionalLightCount() const
{
  return static_cast<uint32_t>(directionalLights_.size());
}

uint32_t VirtualShadowMapManager::getActiveLayerCount() const
{
  return getActiveDirectionalLightCount() * settings_.cascadeCount;
}

uint32_t VirtualShadowMapManager::getMaxLayerCount() const
{
  return settings_.maxDirectionalLights * settings_.cascadeCount;
}

float VirtualShadowMapManager::getFirstCascadeWorldExtent() const
{
  return settings_.firstCascadeWorldExtent * settings_.pageWorldScale;
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
  directionalLightsBufferSize_ = std::max<uint64_t>(1u, settings_.maxDirectionalLights) * sizeof(DirectionalLightGPU);
  allocatorCountersBufferSize_ = 8u * sizeof(uint32_t);
  const uint64_t totalVirtualPagesAcrossMipChain = maxLayers * vptMipChainEntriesPerLayer;
  allocationRequestsBufferSize_ = std::max<uint64_t>(settings_.maxAllocationRequestsPerFrame, totalVirtualPagesAcrossMipChain) * packedCoordSize;
  futureAllocationRequestsBufferSize_ = std::max<uint64_t>(settings_.maxFutureAllocationRequests, totalVirtualPagesAcrossMipChain) * packedCoordSize;
  unallocatedPhysicalPagesBufferSize_ = pptEntries * packedCoordSize;
  reclaimablePhysicalPagesBufferSize_ = pptEntries * packedCoordSize;

  virtualPageTableBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "VirtualShadowMap_VPT.buffer",
          .size = virtualPageTableBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  physicalPageTableBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "VirtualShadowMap_PPT.buffer",
          .size = physicalPageTableBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
      });
  virtualPageStateBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "VirtualShadowMap_PageState.buffer",
          .size = virtualPageStateBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          .frameLocal = true,
      });
  invalidationMaskBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "VirtualShadowMap_Invalidations.buffer",
          .size = invalidationMaskBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          .frameLocal = true,
      });
  cascadeStatesBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "VirtualShadowMap_CascadeStates.buffer",
          .size = cascadeStatesBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          .frameLocal = true,
      });
  cascadeMatricesBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "VirtualShadowMap_CascadeMatrices.buffer",
          .size = cascadeMatricesBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          .frameLocal = true,
      });
  cameraStateBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "VirtualShadowMap_CameraState.buffer",
          .size = cameraStateBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          .frameLocal = true,
      });
  directionalLightsBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "VirtualShadowMap_DirectionalLights.buffer",
          .size = directionalLightsBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          .frameLocal = true,
      });
  allocatorCountersBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "VirtualShadowMap_AllocatorCounters.buffer",
          .size = allocatorCountersBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  allocationRequestsBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "VirtualShadowMap_AllocationRequests.buffer",
          .size = allocationRequestsBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage,
          .frameLocal = true,
      });
  futureAllocationRequestsBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "VirtualShadowMap_FutureAllocationRequests.buffer",
          .size = futureAllocationRequestsBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage,
          .frameLocal = true,
      });
  unallocatedPhysicalPagesBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "VirtualShadowMap_UnallocatedPhysicalPages.buffer",
          .size = unallocatedPhysicalPagesBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
      });
  reclaimablePhysicalPagesBuffer_ = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "VirtualShadowMap_ReclaimablePhysicalPages.buffer",
          .size = reclaimablePhysicalPagesBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
      });

  hierarchicalPageBoundsMipCount_ = computeMipCount(settings_.virtualPageTableResolution);
  shadowAtlasTexture_ = renderGraph_->createTexture(
      rendering::TextureInfo{
          .name = "VirtualShadowMap_Atlas.texture",
          .format = rendering::Format::Format_R32Float,
          .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
          .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled | rendering::ImageUsage::ImageUsage_ColorAttachment,
          .width = settings_.physicalAtlasResolution,
          .height = settings_.physicalAtlasResolution,
          .depth = 1u,
          .mipLevels = 1u,
      });
  hierarchicalPageBoundsTexture_ = renderGraph_->createTexture(
      rendering::TextureInfo{
          .name = "VirtualShadowMap_HPB.texture",
          .format = rendering::Format::Format_R32Float,
          .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
          .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          .width = settings_.virtualPageTableResolution,
          .height = settings_.virtualPageTableResolution,
          .depth = 1u,
          .arrayLayers = getMaxLayerCount(),
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
  directionalLightsCPU_.assign(std::max<uint32_t>(1u, settings_.maxDirectionalLights), {});

  initializeFrameOverrideResources();
  initializeFrameLocalResources();
}

void VirtualShadowMapManager::initializeFrameOverrideResources()
{
  const uint32_t frameCount = std::max(1u, renderGraph_->getMaxFramesInFlight());
  frameOverrideResources_.assign(frameCount, {});

  rendering::RHI *const rhi = renderGraph_->getRHI();
  for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
  {
    FrameOverrideResources &frameResources = frameOverrideResources_[frameSlot];
    const std::string suffix = "_frame" + std::to_string(frameSlot);

    frameResources.virtualPageStateBufferId = rhi->createBuffer(
        rendering::BufferInfo{
            .name = "VirtualShadowMap_PageState.override" + suffix,
            .size = virtualPageStateBufferSize_,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    frameResources.invalidationMaskBufferId = rhi->createBuffer(
        rendering::BufferInfo{
            .name = "VirtualShadowMap_Invalidations.override" + suffix,
            .size = invalidationMaskBufferSize_,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    frameResources.cascadeStatesBufferId = rhi->createBuffer(
        rendering::BufferInfo{
            .name = "VirtualShadowMap_CascadeStates.override" + suffix,
            .size = cascadeStatesBufferSize_,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    frameResources.cascadeMatricesBufferId = rhi->createBuffer(
        rendering::BufferInfo{
            .name = "VirtualShadowMap_CascadeMatrices.override" + suffix,
            .size = cascadeMatricesBufferSize_,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    frameResources.cameraStateBufferId = rhi->createBuffer(
        rendering::BufferInfo{
            .name = "VirtualShadowMap_CameraState.override" + suffix,
            .size = cameraStateBufferSize_,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    frameResources.directionalLightsBufferId = rhi->createBuffer(
        rendering::BufferInfo{
            .name = "VirtualShadowMap_DirectionalLights.override" + suffix,
            .size = directionalLightsBufferSize_,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    frameResources.allocationRequestsBufferId = rhi->createBuffer(
        rendering::BufferInfo{
            .name = "VirtualShadowMap_AllocationRequests.override" + suffix,
            .size = allocationRequestsBufferSize_,
            .usage = rendering::BufferUsage::BufferUsage_Storage,
        });
    frameResources.futureAllocationRequestsBufferId = rhi->createBuffer(
        rendering::BufferInfo{
            .name = "VirtualShadowMap_FutureAllocationRequests.override" + suffix,
            .size = futureAllocationRequestsBufferSize_,
            .usage = rendering::BufferUsage::BufferUsage_Storage,
        });
    frameResources.hierarchicalPageBoundsTextureId = rhi->createTexture(
        rendering::TextureInfo{
            .name = "VirtualShadowMap_HPB.override" + suffix,
            .format = rendering::Format::Format_R32Float,
            .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
            .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
            .width = settings_.virtualPageTableResolution,
            .height = settings_.virtualPageTableResolution,
            .depth = 1u,
            .arrayLayers = getMaxLayerCount(),
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
    const math::Vec3f direction = isActive ? directionalLights_[lightIndex].direction.normalize() : math::Vec3f(0.0f, -1.0f, 0.0f);
    const math::Vec3f color = isActive ? directionalLights_[lightIndex].color : math::Vec3f(0.0f, 0.0f, 0.0f);
    lightGPU.direction[0] = direction[0];
    lightGPU.direction[1] = direction[1];
    lightGPU.direction[2] = direction[2];
    lightGPU.direction[3] = 0.0f;
    lightGPU.color[0] = color[0];
    lightGPU.color[1] = color[1];
    lightGPU.color[2] = color[2];
    lightGPU.color[3] = 1.0f;
  }

  renderGraph_->getRHI()->bufferWrite(getCurrentFrameOverrideResources().directionalLightsBufferId, 0, directionalLightsBufferSize_, directionalLightsCPU_.data());
}

float VirtualShadowMapManager::computeCascadeWorldExtent(uint32_t cascadeIndex) const
{
  const float cascadeScale = std::pow(settings_.cascadeWorldExtentScale, static_cast<float>(cascadeIndex));
  return getFirstCascadeWorldExtent() * cascadeScale;
}

void VirtualShadowMapManager::computeCascadeState(uint32_t lightIndex, uint32_t cascadeIndex, const rendering::Camera &camera)
{
  DirectionalLightRuntime &lightRuntime = directionalLights_[lightIndex];
  CascadeRuntime &cascadeRuntime = lightRuntime.cascades[cascadeIndex];

  const math::Vec3f direction = lightRuntime.direction.normalize();
  const math::Vec3f upSeed = chooseStableUp(direction);
  // Keep the quantized page grid in the exact same basis used by the light
  // view matrix. Using the opposite cross-product order here makes the page
  // ring advance in the opposite X direction from projected cascade UVs.
  const math::Vec3f right = direction.cross(upSeed).normalize();
  const math::Vec3f lightUp = right.cross(direction).normalize();
  const math::Vec3f cameraPosition = camera.getPosition();

  cascadeRuntime.worldExtent = computeCascadeWorldExtent(cascadeIndex);
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

  const uint32_t layerIndex = lightIndex * settings_.cascadeCount + cascadeIndex;
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
  cascadeMatrix.cascadeIndex = cascadeIndex;
}

void VirtualShadowMapManager::markRegionInvalidatedForLayer(uint32_t layerIndex, const AABB &region)
{
  if (layerIndex >= getActiveLayerCount())
  {
    return;
  }

  const CascadeMatrixGPU &cascadeMatrix = cascadeMatricesCPU_[layerIndex];
  math::Mat4f viewProj{};
  std::memcpy(viewProj.data, cascadeMatrix.viewProj, sizeof(cascadeMatrix.viewProj));

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
    return;
  }
  if (maxZ < 0.0f || minZ > 1.0f)
  {
    return;
  }

  const float clampedMinU = std::clamp(minX * 0.5f + 0.5f, 0.0f, 1.0f);
  const float clampedMaxU = std::clamp(maxX * 0.5f + 0.5f, 0.0f, 1.0f);
  const float clampedMinV = std::clamp(minY * 0.5f + 0.5f, 0.0f, 1.0f);
  const float clampedMaxV = std::clamp(maxY * 0.5f + 0.5f, 0.0f, 1.0f);

  const int32_t pageMinX = std::clamp(static_cast<int32_t>(std::floor(clampedMinU * settings_.virtualPageTableResolution)), 0, static_cast<int32_t>(settings_.virtualPageTableResolution) - 1);
  const int32_t pageMaxX = std::clamp(static_cast<int32_t>(std::ceil(clampedMaxU * settings_.virtualPageTableResolution)) - 1, 0, static_cast<int32_t>(settings_.virtualPageTableResolution) - 1);
  const int32_t pageMinY = std::clamp(static_cast<int32_t>(std::floor(clampedMinV * settings_.virtualPageTableResolution)), 0, static_cast<int32_t>(settings_.virtualPageTableResolution) - 1);
  const int32_t pageMaxY = std::clamp(static_cast<int32_t>(std::ceil(clampedMaxV * settings_.virtualPageTableResolution)) - 1, 0, static_cast<int32_t>(settings_.virtualPageTableResolution) - 1);

  std::vector<uint32_t> &invalidationMasksCPU = getCurrentInvalidationMasksCPU();
  for (int32_t y = pageMinY; y <= pageMaxY; ++y)
  {
    const uint32_t wordIndex = layerIndex * INVALIDATION_MASK_WORDS_PER_LAYER + static_cast<uint32_t>(y);
    invalidationMasksCPU[wordIndex] |= (1u << static_cast<uint32_t>(pageMinX));
    for (int32_t x = pageMinX + 1; x <= pageMaxX; ++x)
    {
      invalidationMasksCPU[wordIndex] |= (1u << static_cast<uint32_t>(x));
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
    return;
  }

  if (queuedInvalidationRegions_.empty())
  {
    return;
  }

  for (const AABB &region : queuedInvalidationRegions_)
  {
    for (uint32_t lightIndex = 0u; lightIndex < directionalLights_.size(); ++lightIndex)
    {
      for (uint32_t cascadeIndex = 0u; cascadeIndex < settings_.cascadeCount; ++cascadeIndex)
      {
        const uint32_t layerIndex = lightIndex * settings_.cascadeCount + cascadeIndex;
        markRegionInvalidatedForLayer(layerIndex, region);
      }
    }
  }

  queuedInvalidationRegions_.clear();
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
