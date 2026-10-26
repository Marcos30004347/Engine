#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "gpusigneddistancefields/GPUSignedDistanceFieldsManager.hpp"
#include "math/math.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/Material.hpp"
#include "virtualgeometry/MaterialTextureResolver.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsTraceDebugPass : public rendering::Pass
{
public:
  static constexpr uint32_t kMaxDebugCascades = 8u;

  enum class Mode : uint32_t
  {
    RaymarchSurface = 0u,
    StepCount = 1u,
    SurfaceMaterialId = 2u,
    SurfaceMaterialUV = 3u,
    SurfaceMaterialUVWindow = 4u,
    SurfaceMaterialAlbedo = 5u,
    LocalInstanceRaymarch = 6u,
    LocalInstanceNormal = 7u,
    LocalInstanceAlbedo = 8u,
    LocalInstanceCascades = 9u,
    LocalInstanceCascadeInstances = 10u,
    LocalInstanceBlinn = 11u,
    LocalInstanceStepCount = 12u,
	  };

  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    Mode mode = Mode::LocalInstanceRaymarch;
    uint32_t maxSteps = 256u;
    float maxTraceDistance = 10000.0f;
    float hitThresholdScale = 1.0f;
    float emptySpaceStepScale = 1.0f;
    int32_t cascadeOverride = -1;
    math::Vec3f stepCountColor = math::Vec3f(0.92f, 0.26f, 0.12f);
    math::Vec4f traceHeatmapLowColor = math::Vec4f(0.02f, 0.03f, 0.08f, 1.0f);
    math::Vec4f traceHeatmapMidColor = math::Vec4f(0.10f, 0.72f, 0.50f, 1.0f);
    math::Vec4f traceHeatmapHighColor = math::Vec4f(0.92f, 0.26f, 0.12f, 1.0f);
    bool surfaceCacheMaterialsEnabled = true;
    uint32_t feedbackRequestCapacity = 16384u;
    uint32_t feedbackUploadBudget = 0u;
    bool localTraceStatsEnabled = false;
  };

  struct LocalTraceStats
  {
    uint32_t totalPixels = 0u;
    uint32_t disabledPixels = 0u;
    uint32_t localModePixels = 0u;
    uint32_t noCachePixels = 0u;
    uint32_t cachePixels = 0u;
    uint32_t cascadeTests = 0u;
    uint32_t cascadeHits = 0u;
    uint32_t cascadeInstanceRefs = 0u;
    uint32_t instanceTests = 0u;
    uint32_t instanceAabbHits = 0u;
    uint32_t sdfSamples = 0u;
    uint32_t sdfHits = 0u;
    uint32_t noHitPixels = 0u;
    uint32_t hitPixels = 0u;
    uint32_t albedoSamples = 0u;
    uint32_t cascadeDebugPixels = 0u;
    uint32_t cascadeDebugHits = 0u;
    uint32_t _padding0 = 0u;
  };
  static_assert(sizeof(LocalTraceStats) == 72u, "LocalTraceStats must match WGSL counter layout");

	  GPUSignedDistanceFieldsTraceDebugPass(
	      GPUSignedDistanceFieldsManager &manager,
	      rendering::Texture outputTexture,
	      Settings settings,
	      virtualgeometry::VirtualGeometryScene *scene = nullptr)
	      : manager_(manager), scene_(scene), outputTexture_(outputTexture), settings_(settings)
  {
    if (manager_.getCascadeCount() > kMaxDebugCascades)
    {
      throw std::runtime_error("GPUSignedDistanceFieldsTraceDebugPass supports up to 8 cascades");
    }
  }

  ~GPUSignedDistanceFieldsTraceDebugPass() override
  {
    destroyFrameOverrideBuffers(frameOverrideUniformBufferIds_);
    destroyFrameOverrideBuffers(frameOverrideCascadeInfoBufferIds_);
    destroyFrameOverrideBuffers(frameOverrideMaterialRuntimeBufferIds_);
    destroyFrameOverrideBuffers(frameOverrideAtlasInfoBufferIds_);
    destroyFrameOverrideBuffers(frameOverrideTextureEntriesBufferIds_);
    destroyFrameOverrideBuffers(frameOverridePageTableBufferIds_);
    destroyFrameOverrideBuffers(frameOverridePhysicalPagesBufferIds_);
    destroyFrameOverrideBuffers(frameOverrideFeedbackBufferIds_);
    destroyFrameOverrideBuffers(frameOverrideFeedbackReadbackBufferIds_);
    destroyFrameOverrideBuffers(frameOverrideLocalTraceStatsBufferIds_);
    destroyFrameOverrideBuffers(frameOverrideLocalTraceStatsReadbackBufferIds_);
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (bindingGroups_.isValid())
      renderGraph->deleteBindingGroups(bindingGroups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
    if (cascadeInfoBuffer_.isValid())
      renderGraph->deleteBuffer(cascadeInfoBuffer_);
    if (fallbackBrickMapBuffer_.isValid())
      renderGraph->deleteBuffer(fallbackBrickMapBuffer_);
    if (fallbackAabbTreeBuffer_.isValid())
      renderGraph->deleteBuffer(fallbackAabbTreeBuffer_);
	    if (fallbackSurfaceCacheMaterialReferenceAtlas_.isValid())
	      renderGraph->deleteTexture(fallbackSurfaceCacheMaterialReferenceAtlas_);
	    if (fallbackSurfaceCacheMaterialReferenceBasisAtlas_.isValid())
	      renderGraph->deleteTexture(fallbackSurfaceCacheMaterialReferenceBasisAtlas_);
	    if (materialRuntimeBuffer_.isValid())
	      renderGraph->deleteBuffer(materialRuntimeBuffer_);
	    if (atlasInfoBuffer_.isValid())
	      renderGraph->deleteBuffer(atlasInfoBuffer_);
	    if (textureEntriesBuffer_.isValid())
	      renderGraph->deleteBuffer(textureEntriesBuffer_);
	    if (vtPageTableBuffer_.isValid())
	      renderGraph->deleteBuffer(vtPageTableBuffer_);
	    if (vtPhysicalPagesBuffer_.isValid())
	      renderGraph->deleteBuffer(vtPhysicalPagesBuffer_);
	    if (feedbackBuffer_.isValid())
	      renderGraph->deleteBuffer(feedbackBuffer_);
	    if (feedbackReadbackBuffer_.isValid())
	      renderGraph->deleteBuffer(feedbackReadbackBuffer_);
    if (localTraceStatsBuffer_.isValid())
      renderGraph->deleteBuffer(localTraceStatsBuffer_);
    if (localTraceStatsReadbackBuffer_.isValid())
      renderGraph->deleteBuffer(localTraceStatsReadbackBuffer_);
    if (vsdfRuntimeInfoBuffer_.isValid())
      renderGraph->deleteBuffer(vsdfRuntimeInfoBuffer_);
    if (vsdfInstanceBuffer_.isValid())
      renderGraph->deleteBuffer(vsdfInstanceBuffer_);
    if (vsdfCardBuffer_.isValid())
      renderGraph->deleteBuffer(vsdfCardBuffer_);
    if (vsdfPageBuffer_.isValid())
      renderGraph->deleteBuffer(vsdfPageBuffer_);
    if (vsdfAlbedoBuffer_.isValid())
      renderGraph->deleteBuffer(vsdfAlbedoBuffer_);
    if (vsdfNormalBuffer_.isValid())
      renderGraph->deleteBuffer(vsdfNormalBuffer_);
    if (vsdfCascadeBuffer_.isValid())
      renderGraph->deleteBuffer(vsdfCascadeBuffer_);
    if (vsdfCascadeInstanceBuffer_.isValid())
      renderGraph->deleteBuffer(vsdfCascadeInstanceBuffer_);
    if (vsdfSdfChunkBuffer_.isValid())
      renderGraph->deleteBuffer(vsdfSdfChunkBuffer_);
    if (vsdfSdfWordBuffer_.isValid())
      renderGraph->deleteBuffer(vsdfSdfWordBuffer_);
    if (vsdfSparseTLASInfoBuffer_.isValid())
      renderGraph->deleteBuffer(vsdfSparseTLASInfoBuffer_);
    if (vsdfSparseTLASPageTableBuffer_.isValid())
      renderGraph->deleteBuffer(vsdfSparseTLASPageTableBuffer_);
    if (vsdfSparseTLASCellBuffer_.isValid())
      renderGraph->deleteBuffer(vsdfSparseTLASCellBuffer_);
    if (vsdfSparseTLASInstanceRefBuffer_.isValid())
      renderGraph->deleteBuffer(vsdfSparseTLASInstanceRefBuffer_);
    if (sdfAtlasSampler_.isValid())
      renderGraph->deleteSampler(sdfAtlasSampler_);
  }

  void setEnabled(bool enabled)
  {
    enabled_ = enabled;
    if (uniformBuffer_.isValid())
    {
      writeUniforms();
    }
  }

	  void setSettings(const Settings &settings)
	  {
	    settings_ = settings;
	    if (uniformBuffer_.isValid())
	    {
	      writeUniformsAllFrames();
	    }
	  }

  void updateRuntimeState(const math::Mat4f &inverseViewProjection, const math::Vec3f &cameraPosition, bool reverseZ)
  {
    inverseViewProjection_ = inverseViewProjection;
    viewProjection_ = inverseViewProjection.inverse();
    cameraPosition_ = cameraPosition;
    reverseZ_ = reverseZ;
    if (uniformBuffer_.isValid())
    {
      writeUniforms();
    }
    if (cascadeInfoBuffer_.isValid())
    {
      writeCascadeInfos();
    }
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    const_cast<GPUSignedDistanceFieldsTraceDebugPass *>(this)->ensureResources();
    const rendering::BufferId uniformOverride = getFrameOverrideBufferId(frameOverrideUniformBufferIds_, frameSlot);
    if (uniformOverride != rendering::BufferId::Invalid)
    {
      overrides.bufferOverrides.emplace(uniformBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = uniformOverride});
    }

    const rendering::BufferId cascadeInfoOverride = getFrameOverrideBufferId(frameOverrideCascadeInfoBufferIds_, frameSlot);
    if (cascadeInfoOverride != rendering::BufferId::Invalid)
    {
      overrides.bufferOverrides.emplace(cascadeInfoBuffer_.name, rendering::RenderGraphBufferOverride{.bufferId = cascadeInfoOverride});
    }
    appendFrameOverride(overrides, materialRuntimeBuffer_, frameOverrideMaterialRuntimeBufferIds_, frameSlot);
    appendFrameOverride(overrides, atlasInfoBuffer_, frameOverrideAtlasInfoBufferIds_, frameSlot);
    appendFrameOverride(overrides, textureEntriesBuffer_, frameOverrideTextureEntriesBufferIds_, frameSlot);
    appendFrameOverride(overrides, vtPageTableBuffer_, frameOverridePageTableBufferIds_, frameSlot);
    appendFrameOverride(overrides, vtPhysicalPagesBuffer_, frameOverridePhysicalPagesBufferIds_, frameSlot);
    appendFrameOverride(overrides, feedbackBuffer_, frameOverrideFeedbackBufferIds_, frameSlot);
    appendFrameOverride(overrides, feedbackReadbackBuffer_, frameOverrideFeedbackReadbackBufferIds_, frameSlot);
    appendFrameOverride(overrides, localTraceStatsBuffer_, frameOverrideLocalTraceStatsBufferIds_, frameSlot);
    appendFrameOverride(overrides, localTraceStatsReadbackBuffer_, frameOverrideLocalTraceStatsReadbackBufferIds_, frameSlot);
  }

	  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
	  {
    ++recordCallCount_;
    if (!enabled_)
      ++recordSkippedDisabledCount_;

	    ensureResources();
	    syncMaterialStateAllFrames(false);
	    clearFeedback();
    clearLocalTraceStats();
    ++recordDispatchCount_;

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(
        (settings_.width + kWorkgroupSizeX - 1u) / kWorkgroupSizeX,
        (settings_.height + kWorkgroupSizeY - 1u) / kWorkgroupSizeY,
        1u);

	    commandBuffer.cmdCopyBuffer(
	        rendering::BufferView{
	            .buffer = feedbackBuffer_,
	            .offset = 0u,
	            .size = feedbackBufferSize_,
	            .access = rendering::AccessPattern::TRANSFER_READ,
	        },
	        rendering::BufferView{
	            .buffer = feedbackReadbackBuffer_,
	            .offset = 0u,
	            .size = feedbackBufferSize_,
	            .access = rendering::AccessPattern::TRANSFER_WRITE,
	        });

    commandBuffer.cmdCopyBuffer(
        rendering::BufferView{
            .buffer = localTraceStatsBuffer_,
            .offset = 0u,
            .size = sizeof(LocalTraceStats),
            .access = rendering::AccessPattern::TRANSFER_READ,
        },
        rendering::BufferView{
            .buffer = localTraceStatsReadbackBuffer_,
            .offset = 0u,
            .size = sizeof(LocalTraceStats),
            .access = rendering::AccessPattern::TRANSFER_WRITE,
        });
  }

  const LocalTraceStats &getLastLocalTraceStats() const
  {
    return lastLocalTraceStats_;
  }

  bool isEnabled() const
  {
    return enabled_;
  }

  bool hasLocalTraceStatsResources() const
  {
    return localTraceStatsBuffer_.isValid() && localTraceStatsReadbackBuffer_.isValid();
  }

  uint64_t getRecordCallCount() const
  {
    return recordCallCount_;
  }

  uint64_t getRecordDispatchCount() const
  {
    return recordDispatchCount_;
  }

  uint64_t getRecordSkippedDisabledCount() const
  {
    return recordSkippedDisabledCount_;
  }

  LocalTraceStats processLocalTraceStats()
  {
    if (!enabled_ || !localTraceStatsReadbackBuffer_.isValid())
      return lastLocalTraceStats_;

    LocalTraceStats stats{};
    readFrameLocalBuffer(
        localTraceStatsReadbackBuffer_,
        frameOverrideLocalTraceStatsReadbackBufferIds_,
        0u,
        sizeof(stats),
        [&](const void *data)
        {
          std::memcpy(&stats, data, sizeof(stats));
        });
    lastLocalTraceStats_ = stats;
    return lastLocalTraceStats_;
  }

	  bool processFeedback(uint32_t uploadBudget = 0u)
	  {
	    if (!enabled_)
	      return false;
	    if (scene_ == nullptr || !feedbackReadbackBuffer_.isValid() || feedbackBufferSize_ == 0u)
	      return false;

	    virtualgeometry::VirtualTextureSystem::FeedbackHeaderGPU header{};
		    readFrameLocalBuffer(
		        feedbackReadbackBuffer_,
		        frameOverrideFeedbackReadbackBufferIds_,
		        0u,
		        sizeof(header),
	        [&](const void *data)
	        {
	          std::memcpy(&header, data, sizeof(header));
	        });

	    lastFeedbackStats_ = {};
	    lastFeedbackStats_.requested = header.requestCount;
	    lastFeedbackStats_.overflowed = header.overflowCount;

		    const uint32_t capacity = std::max(1u, settings_.feedbackRequestCapacity);
		    const uint32_t requestCount = std::min(header.requestCount, capacity);
		    if (requestCount == 0u)
		    {
		      syncMaterialStateAllFrames(false);
		      clearFeedbackAllFrames();
		      return false;
		    }

		    std::vector<virtualgeometry::VirtualTextureSystem::FeedbackRequestGPU> requests(requestCount);
		    readFrameLocalBuffer(
		        feedbackReadbackBuffer_,
		        frameOverrideFeedbackReadbackBufferIds_,
		        sizeof(virtualgeometry::VirtualTextureSystem::FeedbackHeaderGPU),
	        static_cast<uint64_t>(requestCount) * sizeof(virtualgeometry::VirtualTextureSystem::FeedbackRequestGPU),
	        [&](const void *data)
	        {
	          std::memcpy(requests.data(), data, static_cast<size_t>(requestCount) * sizeof(virtualgeometry::VirtualTextureSystem::FeedbackRequestGPU));
	        });

	    uint32_t resolvedUploadBudget = uploadBudget != 0u ? uploadBudget : settings_.feedbackUploadBudget;
	    if (resolvedUploadBudget == 0u)
	    {
	      const uint32_t baseBudget = std::max(1u, scene_->getVirtualTextureSystem().getSettings().maxUploadsPerFrame);
	      const uint32_t pressureBudget = baseBudget + requestCount / 256u + header.overflowCount / 64u;
	      resolvedUploadBudget = std::min(baseBudget * 4u, std::max(baseBudget, pressureBudget));
	    }

		    const bool changed = scene_->getVirtualTextureSystem().processFeedbackRequests(requests, resolvedUploadBudget, &lastFeedbackStats_);
		    syncMaterialStateAllFrames(false);
		    clearFeedbackAllFrames();
		    return changed;
		  }

private:
  static constexpr uint32_t kWorkgroupSizeX = 8u;
  static constexpr uint32_t kWorkgroupSizeY = 8u;

  struct Uniforms
  {
    math::Mat4f inverseViewProjection = math::Mat4f::identity(); // 64
    math::Vec3f cameraPosition = math::Vec3f(0.0f, 0.0f, 0.0f); // 12
    float maxTraceDistance = 10000.0f;                           //  4 -> total 80
    uint32_t outputWidth = 1u;                                   //  4
    uint32_t outputHeight = 1u;                                  //  4
    uint32_t mode = 0u;                                          //  4
    uint32_t maxSteps = 64u;                                     //  4
    uint32_t atlasBricksPerAxis = 0u;                            //  4
    uint32_t cascadeCount = 0u;                                  //  4
    int32_t cascadeOverride = -1;                                //  4
    uint32_t reverseZ = 1u;                                      //  4
    float hitThresholdScale = 1.0f;                              //  4
    float emptySpaceStepScale = 1.0f;                            //  4
    uint32_t enabled = 1u;                                       //  4
    uint32_t _padding0 = 0u;                                     //  4 -> align following vec3 to WGSL 16-byte boundary
    math::Vec3f stepCountColor = math::Vec3f(0.92f, 0.26f, 0.12f); // 12
    uint32_t surfaceCachePageResolution = 0u;                    //  4
    uint32_t surfaceCachePagesPerRow = 0u;                       //  4
    uint32_t feedbackRequestCapacity = 0u;                       //  4
    uint32_t localTraceStatsEnabled = 0u;                        //  4
    uint32_t _padding3 = 0u;                                     //  4 -> align following mat4 to WGSL 16-byte boundary
    math::Vec4f traceHeatmapLowColor = math::Vec4f(0.02f, 0.03f, 0.08f, 1.0f); // 16
    math::Vec4f traceHeatmapMidColor = math::Vec4f(0.10f, 0.72f, 0.50f, 1.0f); // 16
    math::Vec4f traceHeatmapHighColor = math::Vec4f(0.92f, 0.26f, 0.12f, 1.0f); // 16
    math::Mat4f viewProjection = math::Mat4f::identity();        // 64 -> total 272 = 17x16
  };
  static_assert(sizeof(Uniforms) == 272u, "Trace debug uniforms must be 272 bytes to match WGSL layout");
  static_assert(sizeof(Uniforms) % 16u == 0u, "Trace debug uniforms must be 16-byte aligned");
  static_assert(offsetof(Uniforms, stepCountColor) == 128u, "Trace debug stepCountColor offset must match WGSL layout");
  static_assert(offsetof(Uniforms, surfaceCachePageResolution) == 140u, "Trace debug surface cache page resolution offset must match WGSL layout");
  static_assert(offsetof(Uniforms, surfaceCachePagesPerRow) == 144u, "Trace debug surface cache pages-per-row offset must match WGSL layout");
  static_assert(offsetof(Uniforms, traceHeatmapLowColor) == 160u, "Trace debug heatmap low color offset must match WGSL layout");
  static_assert(offsetof(Uniforms, traceHeatmapMidColor) == 176u, "Trace debug heatmap mid color offset must match WGSL layout");
  static_assert(offsetof(Uniforms, traceHeatmapHighColor) == 192u, "Trace debug heatmap high color offset must match WGSL layout");
	  static_assert(offsetof(Uniforms, viewProjection) == 208u, "Trace debug viewProjection offset must match WGSL layout");

	  struct MaterialRuntimeGPU
	  {
	    uint32_t materialId = 0u;
	    uint32_t tileListBase = 0u;
	    uint32_t baseColorTextureId = virtualgeometry::Material::InvalidTextureId;
	    uint32_t normalTextureId = virtualgeometry::Material::InvalidTextureId;
	    uint32_t hasNormalTexture = 0u;
	    uint32_t _padding0 = 0u;
	    uint32_t _padding1 = 0u;
	    uint32_t _padding2 = 0u;
	    std::array<float, 4> baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
	    std::array<float, 4> pbrFactors{1.0f, 0.0f, 1.0f, 0.0f};
	    std::array<float, 4> emissiveFactorAndStrength{0.0f, 0.0f, 0.0f, 0.0f};
	  };
	  static_assert(sizeof(MaterialRuntimeGPU) == 80u, "MaterialRuntimeGPU must match WGSL layout");

  static rendering::TextureView makeSampledColor3DView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ,
        .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0,
        .flags = rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  static rendering::TextureView makeSampledColor2DView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ,
        .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0,
        .flags = rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  static rendering::TextureView makeStorageColorView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
        .layout = rendering::ResourceLayout::GENERAL,
        .index = 0,
        .flags = rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  void ensureResources()
  {
    if (uniformBuffer_.isValid())
    {
      const auto materialStateSizes = materialStateBufferSizes();
      const uint64_t desiredFeedbackBufferSize =
          sizeof(virtualgeometry::VirtualTextureSystem::FeedbackHeaderGPU) +
          static_cast<uint64_t>(std::max(1u, settings_.feedbackRequestCapacity)) * sizeof(virtualgeometry::VirtualTextureSystem::FeedbackRequestGPU);
      const bool materialBuffersTooSmall =
          renderGraph->getBufferSize(materialRuntimeBuffer_) < materialStateSizes.materialRuntimeSize ||
          renderGraph->getBufferSize(textureEntriesBuffer_) < materialStateSizes.textureEntriesSize ||
          renderGraph->getBufferSize(vtPageTableBuffer_) < materialStateSizes.pageTableSize ||
          renderGraph->getBufferSize(vtPhysicalPagesBuffer_) < materialStateSizes.physicalPagesSize ||
          renderGraph->getBufferSize(feedbackBuffer_) < desiredFeedbackBufferSize ||
          renderGraph->getBufferSize(feedbackReadbackBuffer_) < desiredFeedbackBufferSize;
      if (!materialBuffersTooSmall && !forceResourceRebuild_)
      {
        return;
      }
      forceResourceRebuild_ = false;

      if (pipeline_.isValid())
      {
        renderGraph->deleteComputePipeline(pipeline_);
        pipeline_ = {};
      }
      if (shader_.isValid())
      {
        renderGraph->deleteShader(shader_);
        shader_ = {};
      }
      if (bindingGroups_.isValid())
      {
        renderGraph->deleteBindingGroups(bindingGroups_);
        bindingGroups_ = {};
      }
      if (layout_.isValid())
      {
        renderGraph->deleteBindingsLayout(layout_);
        layout_ = {};
      }
      if (uniformBuffer_.isValid())
      {
        renderGraph->deleteBuffer(uniformBuffer_);
        uniformBuffer_ = {};
      }
      if (cascadeInfoBuffer_.isValid())
      {
        renderGraph->deleteBuffer(cascadeInfoBuffer_);
        cascadeInfoBuffer_ = {};
      }
      if (fallbackBrickMapBuffer_.isValid())
      {
        renderGraph->deleteBuffer(fallbackBrickMapBuffer_);
        fallbackBrickMapBuffer_ = {};
      }
      if (fallbackAabbTreeBuffer_.isValid())
      {
        renderGraph->deleteBuffer(fallbackAabbTreeBuffer_);
        fallbackAabbTreeBuffer_ = {};
      }
      if (fallbackSurfaceCacheMaterialReferenceAtlas_.isValid())
      {
        renderGraph->deleteTexture(fallbackSurfaceCacheMaterialReferenceAtlas_);
        fallbackSurfaceCacheMaterialReferenceAtlas_ = {};
      }
      if (fallbackSurfaceCacheMaterialReferenceBasisAtlas_.isValid())
      {
        renderGraph->deleteTexture(fallbackSurfaceCacheMaterialReferenceBasisAtlas_);
        fallbackSurfaceCacheMaterialReferenceBasisAtlas_ = {};
      }
      if (materialRuntimeBuffer_.isValid())
      {
        renderGraph->deleteBuffer(materialRuntimeBuffer_);
        materialRuntimeBuffer_ = {};
      }
      if (atlasInfoBuffer_.isValid())
      {
        renderGraph->deleteBuffer(atlasInfoBuffer_);
        atlasInfoBuffer_ = {};
      }
      if (textureEntriesBuffer_.isValid())
      {
        renderGraph->deleteBuffer(textureEntriesBuffer_);
        textureEntriesBuffer_ = {};
      }
      if (vtPageTableBuffer_.isValid())
      {
        renderGraph->deleteBuffer(vtPageTableBuffer_);
        vtPageTableBuffer_ = {};
      }
      if (vtPhysicalPagesBuffer_.isValid())
      {
        renderGraph->deleteBuffer(vtPhysicalPagesBuffer_);
        vtPhysicalPagesBuffer_ = {};
      }
      if (feedbackBuffer_.isValid())
      {
        renderGraph->deleteBuffer(feedbackBuffer_);
        feedbackBuffer_ = {};
      }
      if (feedbackReadbackBuffer_.isValid())
      {
        renderGraph->deleteBuffer(feedbackReadbackBuffer_);
	      feedbackReadbackBuffer_ = {};
      }
      deleteBuffer(vsdfRuntimeInfoBuffer_);
      deleteBuffer(vsdfInstanceBuffer_);
      deleteBuffer(vsdfCardBuffer_);
      deleteBuffer(vsdfPageBuffer_);
      deleteBuffer(vsdfAlbedoBuffer_);
      deleteBuffer(vsdfNormalBuffer_);
      deleteBuffer(vsdfCascadeBuffer_);
      deleteBuffer(vsdfCascadeInstanceBuffer_);
      deleteBuffer(vsdfSdfChunkBuffer_);
      deleteBuffer(vsdfSdfWordBuffer_);
      deleteBuffer(vsdfSparseTLASInfoBuffer_);
      deleteBuffer(vsdfSparseTLASPageTableBuffer_);
      deleteBuffer(vsdfSparseTLASCellBuffer_);
      deleteBuffer(vsdfSparseTLASInstanceRefBuffer_);
      deleteBuffer(localTraceStatsBuffer_);
      deleteBuffer(localTraceStatsReadbackBuffer_);
      if (sdfAtlasSampler_.isValid())
      {
        renderGraph->deleteSampler(sdfAtlasSampler_);
        sdfAtlasSampler_ = {};
      }
      destroyFrameOverrideBuffers(frameOverrideMaterialRuntimeBufferIds_);
      destroyFrameOverrideBuffers(frameOverrideUniformBufferIds_);
      destroyFrameOverrideBuffers(frameOverrideCascadeInfoBufferIds_);
      destroyFrameOverrideBuffers(frameOverrideAtlasInfoBufferIds_);
      destroyFrameOverrideBuffers(frameOverrideTextureEntriesBufferIds_);
      destroyFrameOverrideBuffers(frameOverridePageTableBufferIds_);
      destroyFrameOverrideBuffers(frameOverridePhysicalPagesBufferIds_);
      destroyFrameOverrideBuffers(frameOverrideFeedbackBufferIds_);
      destroyFrameOverrideBuffers(frameOverrideFeedbackReadbackBufferIds_);
      destroyFrameOverrideBuffers(frameOverrideLocalTraceStatsBufferIds_);
      destroyFrameOverrideBuffers(frameOverrideLocalTraceStatsReadbackBufferIds_);
      frameOverrideUniformBufferIds_.clear();
      frameOverrideCascadeInfoBufferIds_.clear();
      frameOverrideMaterialRuntimeBufferIds_.clear();
      frameOverrideAtlasInfoBufferIds_.clear();
      frameOverrideTextureEntriesBufferIds_.clear();
      frameOverridePageTableBufferIds_.clear();
      frameOverridePhysicalPagesBufferIds_.clear();
      frameOverrideFeedbackBufferIds_.clear();
      frameOverrideFeedbackReadbackBufferIds_.clear();
      frameOverrideLocalTraceStatsBufferIds_.clear();
      frameOverrideLocalTraceStatsReadbackBufferIds_.clear();
    }

    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(Uniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    initializeFrameOverrideBuffers(
        uniformBuffer_,
        rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        frameOverrideUniformBufferIds_);
    cascadeInfoBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_CascadeInfos.buffer",
            .size = sizeof(GPUSignedDistanceFieldsManager::CascadeInfoGPU) * kMaxDebugCascades,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    initializeFrameOverrideBuffers(
        cascadeInfoBuffer_,
        rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        frameOverrideCascadeInfoBufferIds_);
    fallbackBrickMapBuffer_ = renderGraph->createBuffer(
        rendering::BufferInfo{
            .name = passName + "_FallbackBrickMap.buffer",
            .size = sizeof(uint32_t),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    fallbackAabbTreeBuffer_ = renderGraph->createBuffer(
        rendering::BufferInfo{
            .name = passName + "_FallbackAabbTree.buffer",
            .size = GPUSignedDistanceFieldsManager::BUILD_TREE_FLOAT4_COUNT * sizeof(float) * 4u,
            .usage = rendering::BufferUsage::BufferUsage_Storage,
        });
	    fallbackSurfaceCacheMaterialReferenceAtlas_ = renderGraph->createTexture(
	        rendering::TextureInfo{
	            .name = passName + "_FallbackSurfaceCacheMaterialReferenceAtlas.texture",
	            .format = rendering::Format::Format_RGBA32Uint,
	            .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
	            .usage = rendering::ImageUsage::ImageUsage_Sampled,
	            .width = 1u,
	            .height = 1u,
	            .depth = 1u,
	            .mipLevels = 1u,
	        });
	    fallbackSurfaceCacheMaterialReferenceBasisAtlas_ = renderGraph->createTexture(
	        rendering::TextureInfo{
	            .name = passName + "_FallbackSurfaceCacheMaterialReferenceBasisAtlas.texture",
	            .format = rendering::Format::Format_RGBA32Uint,
	            .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
	            .usage = rendering::ImageUsage::ImageUsage_Sampled,
	            .width = 1u,
	            .height = 1u,
	            .depth = 1u,
	            .mipLevels = 1u,
	        });
	    const auto materialStateSizes = materialStateBufferSizes();
	    materialRuntimeBuffer_ = createFrameLocalBuffer(
	        rendering::BufferInfo{
	            .name = passName + "_MaterialRuntime.buffer",
	            .size = materialStateSizes.materialRuntimeSize,
	            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
	        });
	    atlasInfoBuffer_ = createFrameLocalBuffer(
	        rendering::BufferInfo{
	            .name = passName + "_AtlasInfo.buffer",
	            .size = sizeof(virtualgeometry::VirtualTextureSystem::AtlasInfoGPU),
	            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
	        });
	    textureEntriesBuffer_ = createFrameLocalBuffer(
	        rendering::BufferInfo{
	            .name = passName + "_TextureEntries.buffer",
	            .size = materialStateSizes.textureEntriesSize,
	            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
	        });
	    vtPageTableBuffer_ = createFrameLocalBuffer(
	        rendering::BufferInfo{
	            .name = passName + "_VirtualTexturePageTable.buffer",
	            .size = materialStateSizes.pageTableSize,
	            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
	        });
	    vtPhysicalPagesBuffer_ = createFrameLocalBuffer(
	        rendering::BufferInfo{
	            .name = passName + "_VirtualTexturePhysicalPages.buffer",
	            .size = materialStateSizes.physicalPagesSize,
	            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
	        });
	    feedbackBufferSize_ = sizeof(virtualgeometry::VirtualTextureSystem::FeedbackHeaderGPU) +
	                          static_cast<uint64_t>(std::max(1u, settings_.feedbackRequestCapacity)) * sizeof(virtualgeometry::VirtualTextureSystem::FeedbackRequestGPU);
	    feedbackBuffer_ = createFrameLocalBuffer(
	        rendering::BufferInfo{
	            .name = passName + "_VirtualTextureFeedback.buffer",
	            .size = feedbackBufferSize_,
	            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
	        });
	    feedbackReadbackBuffer_ = createFrameLocalBuffer(
		        rendering::BufferInfo{
		            .name = passName + "_VirtualTextureFeedback.readback",
		            .size = feedbackBufferSize_,
		            .usage = rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull,
		        });
    localTraceStatsBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_LocalTraceStats.buffer",
            .size = sizeof(LocalTraceStats),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    localTraceStatsReadbackBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_LocalTraceStats.readback",
            .size = sizeof(LocalTraceStats),
            .usage = rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull,
        });
		    initializeFrameOverrideBuffers(materialRuntimeBuffer_, rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push, frameOverrideMaterialRuntimeBufferIds_);
		    initializeFrameOverrideBuffers(atlasInfoBuffer_, rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push, frameOverrideAtlasInfoBufferIds_);
		    initializeFrameOverrideBuffers(textureEntriesBuffer_, rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push, frameOverrideTextureEntriesBufferIds_);
		    initializeFrameOverrideBuffers(vtPageTableBuffer_, rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push, frameOverridePageTableBufferIds_);
		    initializeFrameOverrideBuffers(vtPhysicalPagesBuffer_, rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push, frameOverridePhysicalPagesBufferIds_);
		    initializeFrameOverrideBuffers(feedbackBuffer_, rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc, frameOverrideFeedbackBufferIds_);
		    initializeFrameOverrideBuffers(feedbackReadbackBuffer_, rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull, frameOverrideFeedbackReadbackBufferIds_);
    initializeFrameOverrideBuffers(localTraceStatsBuffer_, rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc, frameOverrideLocalTraceStatsBufferIds_);
    initializeFrameOverrideBuffers(localTraceStatsReadbackBuffer_, rendering::BufferUsage::BufferUsage_CopyDst | rendering::BufferUsage::BufferUsage_Pull, frameOverrideLocalTraceStatsReadbackBufferIds_);
	    uint32_t invalidBrick = GPUSignedDistanceFieldsManager::INVALID_ID;
	    renderGraph->bufferWrite(fallbackBrickMapBuffer_, 0u, sizeof(uint32_t), &invalidBrick);
	    writeUniformsAllFrames();
	    writeCascadeInfosAllFrames();
    syncMaterialStateAllFrames(true);
    clearFeedbackAllFrames();
    clearLocalTraceStatsAllFrames();
    createVSDFRuntimeResources();
    forceResourceRebuild_ = false;

	    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {
                rendering::BindingGroupLayout{
                    .buffers = {
                        {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeInfos", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedBrickMap0", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedBrickMap1", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedBrickMap2", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedBrickMap3", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedBrickMap4", .binding = 8, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedBrickMap5", .binding = 9, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedBrickMap6", .binding = 10, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedBrickMap7", .binding = 11, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "contextBrickAabb", .binding = 14, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedAabbTree0", .binding = 15, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedAabbTree1", .binding = 16, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedAabbTree2", .binding = 17, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedAabbTree3", .binding = 18, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedAabbTree4", .binding = 19, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedAabbTree5", .binding = 20, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "combinedAabbTree6", .binding = 21, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
	                        {.name = "combinedAabbTree7", .binding = 22, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
	                        {.name = "materialRuntime", .binding = 26, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
	                        {.name = "atlasInfo", .binding = 27, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
	                        {.name = "textureEntries", .binding = 28, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
	                        {.name = "vtPageTable", .binding = 29, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
	                        {.name = "vtPhysicalPages", .binding = 30, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
	                        {.name = "feedbackBuffer", .binding = 32, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "vsdfRuntimeInfo", .binding = 33, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "vsdfInstances", .binding = 34, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "vsdfCards", .binding = 35, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "vsdfPages", .binding = 36, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "vsdfAlbedoPages", .binding = 37, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "vsdfNormalPages", .binding = 38, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "vsdfCascades", .binding = 39, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "vsdfCascadeInstanceIndices", .binding = 40, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "vsdfSdfChunks", .binding = 41, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "vsdfSdfWords", .binding = 42, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "localTraceStats", .binding = 43, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "vsdfSparseTLASInfo", .binding = 44, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "vsdfSparseTLASPageTable", .binding = 45, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "vsdfSparseTLASCells", .binding = 46, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "vsdfSparseTLASInstanceRefs", .binding = 47, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
	                    },
                    .samplers = {
                        // binding=13: WGSL separate sampler for the SDF atlas (bilinear, clamp-to-edge)
                        {.name = "sdfAtlasSampler", .binding = 13, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                    .textures = {
                        {.name = "sdfAtlasTexture", .binding = 3, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
	                        {.name = "surfaceCacheMaterialReferenceAtlas", .binding = 25, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
	                        {.name = "surfaceCacheMaterialReferenceBasisAtlas", .binding = 31, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
	                    },
                    .storageTextures = {
                        {.name = "outputTexture", .binding = 12, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                },
            },
        });

    const auto &shared = manager_.getSharedResources();
    const auto &combined = manager_.getPathResources(GPUSignedDistanceFieldsManager::Path::Combined);
	    const rendering::Texture surfaceCacheMaterialReferenceAtlas =
	        shared.surfaceCacheMaterialReferenceAtlas.isValid() ? shared.surfaceCacheMaterialReferenceAtlas : fallbackSurfaceCacheMaterialReferenceAtlas_;
	    const rendering::Texture surfaceCacheMaterialReferenceBasisAtlas =
	        shared.surfaceCacheMaterialReferenceBasisAtlas.isValid() ? shared.surfaceCacheMaterialReferenceBasisAtlas : fallbackSurfaceCacheMaterialReferenceBasisAtlas_;
    auto brickMapView = [&](uint32_t cascadeIndex) -> rendering::BufferView
    {
      if (cascadeIndex < combined.cascades.size())
      {
        const auto &cascade = combined.cascades[cascadeIndex];
        return rendering::BufferView{
            .buffer = cascade.brickMap,
            .offset = cascade.brickMapOffset,
            .size = cascade.brickMapSize,
            .access = rendering::AccessPattern::SHADER_READ,
        };
      }

      return rendering::BufferView{
          .buffer = fallbackBrickMapBuffer_,
          .offset = 0u,
          .size = sizeof(uint32_t),
          .access = rendering::AccessPattern::SHADER_READ,
      };
    };
    auto aabbTreeView = [&](uint32_t cascadeIndex) -> rendering::BufferView
    {
      if (cascadeIndex < combined.cascades.size())
      {
        const auto &cascade = combined.cascades[cascadeIndex];
        return rendering::BufferView{
            .buffer = cascade.aabbTree,
            .offset = cascade.aabbTreeOffset,
            .size = cascade.aabbTreeSize,
            .access = rendering::AccessPattern::SHADER_READ,
        };
      }

      return rendering::BufferView{
          .buffer = fallbackAabbTreeBuffer_,
          .offset = 0u,
          .size = GPUSignedDistanceFieldsManager::BUILD_TREE_FLOAT4_COUNT * sizeof(float) * 4u,
          .access = rendering::AccessPattern::SHADER_READ,
      };
    };

    // Create a linear clamp sampler for the SDF atlas — matches Brixelizer's bilinear atlas sampling
    sdfAtlasSampler_ = renderGraph->createSampler(
        rendering::SamplerInfo{
            .name = passName + "_SdfAtlasSampler",
            .minFilter = rendering::Filter::Linear,
            .magFilter = rendering::Filter::Linear,
            .addressModeU = rendering::SamplerAddressMode::ClampToEdge,
            .addressModeV = rendering::SamplerAddressMode::ClampToEdge,
            .addressModeW = rendering::SamplerAddressMode::ClampToEdge,
            .anisotropyEnable = false,
            .maxAnisotropy = 1.0f,
            .maxLod = 1.0f,
        });

    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_BindingGroups",
            .groups = {
                rendering::GroupInfo{
                    .name = "group0",
                    .buffers = {
                        {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .offset = 0u, .size = sizeof(Uniforms), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 1, .bufferView = {.buffer = cascadeInfoBuffer_, .offset = 0u, .size = sizeof(GPUSignedDistanceFieldsManager::CascadeInfoGPU) * kMaxDebugCascades, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 4, .bufferView = brickMapView(0u)},
                        {.binding = 5, .bufferView = brickMapView(1u)},
                        {.binding = 6, .bufferView = brickMapView(2u)},
                        {.binding = 7, .bufferView = brickMapView(3u)},
                        {.binding = 8, .bufferView = brickMapView(4u)},
                        {.binding = 9, .bufferView = brickMapView(5u)},
                        {.binding = 10, .bufferView = brickMapView(6u)},
                        {.binding = 11, .bufferView = brickMapView(7u)},
                        {.binding = 14, .bufferView = {.buffer = shared.brickAABB, .offset = 0u, .size = shared.brickAABBSize, .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 15, .bufferView = aabbTreeView(0u)},
                        {.binding = 16, .bufferView = aabbTreeView(1u)},
                        {.binding = 17, .bufferView = aabbTreeView(2u)},
                        {.binding = 18, .bufferView = aabbTreeView(3u)},
                        {.binding = 19, .bufferView = aabbTreeView(4u)},
                        {.binding = 20, .bufferView = aabbTreeView(5u)},
                        {.binding = 21, .bufferView = aabbTreeView(6u)},
	                        {.binding = 22, .bufferView = aabbTreeView(7u)},
	                        {.binding = 26, .bufferView = {.buffer = materialRuntimeBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(materialRuntimeBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
	                        {.binding = 27, .bufferView = {.buffer = atlasInfoBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(atlasInfoBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
	                        {.binding = 28, .bufferView = {.buffer = textureEntriesBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(textureEntriesBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
	                        {.binding = 29, .bufferView = {.buffer = vtPageTableBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(vtPageTableBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
	                        {.binding = 30, .bufferView = {.buffer = vtPhysicalPagesBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(vtPhysicalPagesBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
	                        {.binding = 32, .bufferView = {.buffer = feedbackBuffer_, .offset = 0u, .size = feedbackBufferSize_, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 33, .bufferView = {.buffer = vsdfRuntimeInfoBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(vsdfRuntimeInfoBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 34, .bufferView = {.buffer = vsdfInstanceBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(vsdfInstanceBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 35, .bufferView = {.buffer = vsdfCardBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(vsdfCardBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 36, .bufferView = {.buffer = vsdfPageBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(vsdfPageBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 37, .bufferView = {.buffer = vsdfAlbedoBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(vsdfAlbedoBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 38, .bufferView = {.buffer = vsdfNormalBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(vsdfNormalBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 39, .bufferView = {.buffer = vsdfCascadeBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(vsdfCascadeBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 40, .bufferView = {.buffer = vsdfCascadeInstanceBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(vsdfCascadeInstanceBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 41, .bufferView = {.buffer = vsdfSdfChunkBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(vsdfSdfChunkBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 42, .bufferView = {.buffer = vsdfSdfWordBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(vsdfSdfWordBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 43, .bufferView = {.buffer = localTraceStatsBuffer_, .offset = 0u, .size = sizeof(LocalTraceStats), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 44, .bufferView = {.buffer = vsdfSparseTLASInfoBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(vsdfSparseTLASInfoBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 45, .bufferView = {.buffer = vsdfSparseTLASPageTableBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(vsdfSparseTLASPageTableBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 46, .bufferView = {.buffer = vsdfSparseTLASCellBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(vsdfSparseTLASCellBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 47, .bufferView = {.buffer = vsdfSparseTLASInstanceRefBuffer_, .offset = 0u, .size = renderGraph->getBufferSize(vsdfSparseTLASInstanceRefBuffer_), .access = rendering::AccessPattern::SHADER_READ}},
	                    },
                    .samplers = {
                        // binding=13: WGSL separate sampler for the SDF atlas (bilinear, clamp-to-edge)
                        {.sampler = sdfAtlasSampler_, .view = makeSampledColor3DView(shared.sdfAtlas), .binding = 13},
                    },
                    .textures = {
	                        {.binding = 3, .textureView = makeSampledColor3DView(shared.sdfAtlas)},
	                        {.binding = 25, .textureView = makeSampledColor2DView(surfaceCacheMaterialReferenceAtlas)},
	                        {.binding = 31, .textureView = makeSampledColor2DView(surfaceCacheMaterialReferenceBasisAtlas)},
	                    },
                    .storageTextures = {
                        {.binding = 12, .textureView = makeStorageColorView(outputTexture_)},
                    },
                },
            },
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/gpusigneddistancefields-debug-trace.spirv"),
            .type = rendering::ShaderType::SpirV,
        });
    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = "cs_main",
            .layout = layout_,
            .name = passName + "_Pipeline",
            .shader = shader_,
        });
  }

	  void writeUniforms()
  {
    Uniforms uniforms{};
    uniforms.inverseViewProjection = inverseViewProjection_;
    uniforms.cameraPosition = cameraPosition_;
    uniforms.maxTraceDistance = settings_.maxTraceDistance;
    uniforms.outputWidth = settings_.width;
    uniforms.outputHeight = settings_.height;
    uniforms.mode = static_cast<uint32_t>(settings_.mode);
    uniforms.maxSteps = settings_.maxSteps;
    uniforms.atlasBricksPerAxis = manager_.getSettings().atlasBrickCapacityPerAxis;
    uniforms.cascadeCount = std::min<uint32_t>(manager_.getCascadeCount(), kMaxDebugCascades);
    uniforms.cascadeOverride = settings_.cascadeOverride;
    uniforms.reverseZ = reverseZ_ ? 1u : 0u;
    uniforms.hitThresholdScale = settings_.hitThresholdScale;
    uniforms.emptySpaceStepScale = settings_.emptySpaceStepScale;
    uniforms.enabled = enabled_ ? 1u : 0u;
    uniforms.stepCountColor = settings_.stepCountColor;
    const auto &shared = manager_.getSharedResources();
    const bool canSampleSurfaceCache =
        settings_.surfaceCacheMaterialsEnabled && shared.surfaceCacheMaterialReferenceAtlas.isValid();
    uniforms.surfaceCachePageResolution = canSampleSurfaceCache ? manager_.getSettings().surfaceCachePageResolution : 0u;
    uniforms.surfaceCachePagesPerRow = canSampleSurfaceCache ? shared.surfaceCacheLayout.pagesPerRow : 0u;
    uniforms.feedbackRequestCapacity = settings_.feedbackRequestCapacity;
    uniforms.localTraceStatsEnabled = settings_.localTraceStatsEnabled ? 1u : 0u;
    uniforms.traceHeatmapLowColor = settings_.traceHeatmapLowColor;
    uniforms.traceHeatmapMidColor = settings_.traceHeatmapMidColor;
    uniforms.traceHeatmapHighColor = settings_.traceHeatmapHighColor;
    uniforms.viewProjection = viewProjection_;
	    writeFrameLocalBuffer(frameOverrideUniformBufferIds_, uniformBuffer_, 0u, sizeof(Uniforms), &uniforms);
	  }

	  void clearFeedback()
	  {
		    if (!feedbackBuffer_.isValid())
		      return;
		    virtualgeometry::VirtualTextureSystem::FeedbackHeaderGPU zeroHeader{};
		    writeFrameLocalBuffer(frameOverrideFeedbackBufferIds_, feedbackBuffer_, 0u, sizeof(zeroHeader), &zeroHeader);
		  }

  void clearLocalTraceStats()
  {
    if (!localTraceStatsBuffer_.isValid())
      return;
    LocalTraceStats zeroStats{};
    writeFrameLocalBuffer(frameOverrideLocalTraceStatsBufferIds_, localTraceStatsBuffer_, 0u, sizeof(zeroStats), &zeroStats);
  }

	  struct MaterialStateBufferSizes
	  {
	    uint64_t materialRuntimeSize = sizeof(MaterialRuntimeGPU);
	    uint64_t textureEntriesSize = sizeof(virtualgeometry::VirtualTextureSystem::TextureEntryGPU);
	    uint64_t pageTableSize = sizeof(virtualgeometry::VirtualTextureSystem::PageTableEntryGPU);
	    uint64_t physicalPagesSize = sizeof(uint32_t);
	  };

	  MaterialStateBufferSizes materialStateBufferSizes() const
	  {
	    MaterialStateBufferSizes sizes{};
	    if (scene_ == nullptr)
	      return sizes;
	    const auto &virtualTextures = scene_->getVirtualTextureSystem();
	    sizes.materialRuntimeSize = std::max<uint64_t>(1u, virtualTextures.getMaterialEntries().size()) * sizeof(MaterialRuntimeGPU);
	    sizes.textureEntriesSize = std::max<uint64_t>(1u, virtualTextures.getTextureEntries().size()) * sizeof(virtualgeometry::VirtualTextureSystem::TextureEntryGPU);
	    sizes.pageTableSize = std::max<uint64_t>(1u, virtualTextures.getPageTableEntries().size()) * sizeof(virtualgeometry::VirtualTextureSystem::PageTableEntryGPU);
	    sizes.physicalPagesSize = std::max<uint64_t>(1u, virtualTextures.getPhysicalPagePixels().size()) * sizeof(uint32_t);
	    return sizes;
	  }

	  void writeMaterialMetadata()
	  {
	    MaterialRuntimeGPU emptyMaterial{};
	    virtualgeometry::VirtualTextureSystem::TextureEntryGPU emptyTexture{};
	    virtualgeometry::VirtualTextureSystem::PageTableEntryGPU emptyPage{};
	    virtualgeometry::VirtualTextureSystem::AtlasInfoGPU emptyAtlas{};
		    if (scene_ == nullptr)
		    {
		      writeFrameLocalBuffer(frameOverrideMaterialRuntimeBufferIds_, materialRuntimeBuffer_, 0u, sizeof(emptyMaterial), &emptyMaterial);
		      writeFrameLocalBuffer(frameOverrideAtlasInfoBufferIds_, atlasInfoBuffer_, 0u, sizeof(emptyAtlas), &emptyAtlas);
		      writeFrameLocalBuffer(frameOverrideTextureEntriesBufferIds_, textureEntriesBuffer_, 0u, sizeof(emptyTexture), &emptyTexture);
		      writeFrameLocalBuffer(frameOverridePageTableBufferIds_, vtPageTableBuffer_, 0u, sizeof(emptyPage), &emptyPage);
		      return;
		    }

	    const auto &virtualTextures = scene_->getVirtualTextureSystem();
	    const auto &materialEntries = virtualTextures.getMaterialEntries();
	    std::vector<MaterialRuntimeGPU> runtime(std::max<size_t>(1u, materialEntries.size()));
	    for (uint32_t materialIndex = 0u; materialIndex < runtime.size(); ++materialIndex)
	    {
	      MaterialRuntimeGPU &material = runtime[materialIndex];
	      material.materialId = materialIndex;
	      const virtualgeometry::Material *definition = scene_->getMaterialManager().getMaterial(materialIndex);
	      if (materialIndex < materialEntries.size())
	        material.baseColorTextureId = virtualgeometry::resolveBaseColorMaterialTextureId(definition, materialEntries[materialIndex]);
	      if (definition != nullptr)
	      {
	        const float *baseColorFactor = definition->getBaseColorFactor();
	        std::copy(baseColorFactor, baseColorFactor + 4, material.baseColorFactor.begin());
	        material.pbrFactors[0] = definition->getRoughnessFactor();
	        material.pbrFactors[1] = definition->getMetallicFactor();
	        material.pbrFactors[2] = definition->getOpacity();
	        const float *emissiveFactor = definition->getEmissiveFactor();
	        material.emissiveFactorAndStrength[0] = emissiveFactor[0];
	        material.emissiveFactorAndStrength[1] = emissiveFactor[1];
	        material.emissiveFactorAndStrength[2] = emissiveFactor[2];
	        material.emissiveFactorAndStrength[3] = definition->getEmissiveStrength();
	      }
	    }

	    const auto &textureEntries = virtualTextures.getTextureEntries();
	    const auto &pageTable = virtualTextures.getPageTableEntries();
		    writeFrameLocalBuffer(frameOverrideMaterialRuntimeBufferIds_, materialRuntimeBuffer_, 0u, runtime.size() * sizeof(MaterialRuntimeGPU), runtime.data());
		    const auto &atlasInfo = virtualTextures.getAtlasInfo();
		    writeFrameLocalBuffer(frameOverrideAtlasInfoBufferIds_, atlasInfoBuffer_, 0u, sizeof(atlasInfo), &atlasInfo);
		    writeFrameLocalBuffer(
		        frameOverrideTextureEntriesBufferIds_,
		        textureEntriesBuffer_,
		        0u,
		        std::max<size_t>(1u, textureEntries.size()) * sizeof(emptyTexture),
		        textureEntries.empty() ? &emptyTexture : textureEntries.data());
		    writeFrameLocalBuffer(
		        frameOverridePageTableBufferIds_,
		        vtPageTableBuffer_,
		        0u,
		        std::max<size_t>(1u, pageTable.size()) * sizeof(emptyPage),
		        pageTable.empty() ? &emptyPage : pageTable.data());
		  }

	  void writePhysicalPages(const std::vector<uint32_t> *dirtyPhysicalPages, bool fullUpload)
	  {
	    const uint32_t emptyPixel = 0xffffffffu;
	    if (scene_ == nullptr)
	    {
	      if (fullUpload)
	        writeFrameLocalBuffer(frameOverridePhysicalPagesBufferIds_, vtPhysicalPagesBuffer_, 0u, sizeof(emptyPixel), &emptyPixel);
	      return;
	    }

	    const auto &virtualTextures = scene_->getVirtualTextureSystem();
	    const auto &physicalPages = virtualTextures.getPhysicalPagePixels();
	    if (physicalPages.empty())
	    {
	      if (fullUpload)
	        writeFrameLocalBuffer(frameOverridePhysicalPagesBufferIds_, vtPhysicalPagesBuffer_, 0u, sizeof(emptyPixel), &emptyPixel);
	      return;
	    }

	    const uint64_t pageSize = std::max<uint32_t>(1u, virtualTextures.getSettings().pageSize);
	    const uint64_t pageTexelCount = pageSize * pageSize;
	    if (fullUpload || dirtyPhysicalPages == nullptr)
	    {
	      writeFrameLocalBuffer(
	          frameOverridePhysicalPagesBufferIds_,
	          vtPhysicalPagesBuffer_,
	          0u,
	          physicalPages.size() * sizeof(uint32_t),
	          physicalPages.data());
	      return;
	    }

	    const uint64_t totalTexels = physicalPages.size();
	    for (const uint32_t physicalPage : *dirtyPhysicalPages)
	    {
	      const uint64_t texelOffset = static_cast<uint64_t>(physicalPage) * pageTexelCount;
	      if (texelOffset >= totalTexels)
	        continue;
	      const uint64_t texelCount = std::min<uint64_t>(pageTexelCount, totalTexels - texelOffset);
	      writeFrameLocalBuffer(
	          frameOverridePhysicalPagesBufferIds_,
	          vtPhysicalPagesBuffer_,
	          texelOffset * sizeof(uint32_t),
	          texelCount * sizeof(uint32_t),
	          physicalPages.data() + texelOffset);
	    }
	  }

	  void writeMaterialState()
	  {
	    writeMaterialMetadata();
	    writePhysicalPages(nullptr, true);
	  }

	  void syncMaterialStateAllFrames(bool forceFullUpload)
	  {
	    bool metadataChanged = forceFullUpload;
	    bool physicalPagesChanged = forceFullUpload;
	    bool fullPhysicalUpload = forceFullUpload;
	    const std::vector<uint32_t> *dirtyPages = nullptr;

	    if (scene_ != nullptr)
	    {
	      const auto &virtualTextures = scene_->getVirtualTextureSystem();
	      const uint64_t pageTableVersion = virtualTextures.getPageTableVersion();
	      const uint64_t physicalPageVersion = virtualTextures.getPhysicalPageVersion();
	      metadataChanged = forceFullUpload || pageTableVersion != syncedPageTableVersion_;
	      physicalPagesChanged = forceFullUpload || physicalPageVersion != syncedPhysicalPageVersion_;
	      if (!metadataChanged && !physicalPagesChanged)
	        return;

	      pendingPhysicalPageUploads_.clear();
	      if (physicalPagesChanged && !fullPhysicalUpload)
	        fullPhysicalUpload = !virtualTextures.collectDirtyPhysicalPagesSince(syncedPhysicalPageVersion_, pendingPhysicalPageUploads_);
	      dirtyPages = fullPhysicalUpload ? nullptr : &pendingPhysicalPageUploads_;
	    }
	    else if (!forceFullUpload)
	    {
	      return;
	    }

	    const uint32_t frameCount = std::max(1u, getMaxFramesInFlight());
	    const uint32_t originalFrameIndex = getCurrentFrameIndex();
	    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
	    {
	      renderGraph->setCurrentFrameIndex(frameSlot);
	      if (metadataChanged)
	        writeMaterialMetadata();
	      if (physicalPagesChanged)
	        writePhysicalPages(dirtyPages, fullPhysicalUpload);
	    }
	    renderGraph->setCurrentFrameIndex(originalFrameIndex);

	    if (scene_ != nullptr)
	    {
	      const auto &virtualTextures = scene_->getVirtualTextureSystem();
	      syncedPageTableVersion_ = virtualTextures.getPageTableVersion();
	      syncedPhysicalPageVersion_ = virtualTextures.getPhysicalPageVersion();
	    }
	  }

  void writeCascadeInfos()
  {
    std::array<GPUSignedDistanceFieldsManager::CascadeInfoGPU, kMaxDebugCascades> cascadeInfos{};
    const uint32_t count = std::min<uint32_t>(manager_.getCascadeCount(), kMaxDebugCascades);
    for (uint32_t cascadeIndex = 0u; cascadeIndex < count; ++cascadeIndex)
    {
      cascadeInfos[cascadeIndex] = manager_.getCascadeInfo(cascadeIndex);
    }
    writeFrameLocalBuffer(frameOverrideCascadeInfoBufferIds_, cascadeInfoBuffer_, 0u, sizeof(cascadeInfos), cascadeInfos.data());
  }

  void initializeFrameOverrideBuffers(
      const rendering::Buffer &buffer,
      rendering::BufferUsage usage,
      std::vector<rendering::BufferId> &bufferIds)
  {
    if (renderGraph == nullptr)
    {
      return;
    }

    const uint32_t frameCount = std::max(1u, renderGraph->getMaxFramesInFlight());
    if (frameCount <= 1u || !bufferIds.empty())
    {
      return;
    }

    bufferIds.assign(frameCount, rendering::BufferId::Invalid);
    const uint64_t bufferSize = renderGraph->getBufferSize(buffer);
    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
    {
      bufferIds[frameSlot] = renderGraph->getRHI()->createBuffer(
          rendering::BufferInfo{
              .name = buffer.name + ".override_frame" + std::to_string(frameSlot),
              .size = bufferSize,
              .usage = usage,
          });
    }
  }

  void destroyFrameOverrideBuffers(const std::vector<rendering::BufferId> &bufferIds) const
  {
    if (renderGraph == nullptr)
    {
      return;
    }

    for (const rendering::BufferId bufferId : bufferIds)
    {
      if (bufferId != rendering::BufferId::Invalid)
      {
        renderGraph->getRHI()->deleteBuffer(bufferId);
      }
    }
  }

  rendering::BufferId getFrameOverrideBufferId(const std::vector<rendering::BufferId> &bufferIds, uint32_t frameSlot) const
  {
    if (bufferIds.empty())
    {
      return rendering::BufferId::Invalid;
    }

    return bufferIds[frameSlot % bufferIds.size()];
  }

	  rendering::BufferId getCurrentFrameOverrideBufferId(const std::vector<rendering::BufferId> &bufferIds) const
	  {
	    return getFrameOverrideBufferId(bufferIds, getCurrentFrameIndex());
	  }

	  void appendFrameOverride(
	      rendering::RenderGraph::Overrides &overrides,
	      const rendering::Buffer &buffer,
	      const std::vector<rendering::BufferId> &bufferIds,
	      uint32_t frameSlot) const
	  {
	    if (!buffer.isValid() || bufferIds.empty())
	      return;

	    const rendering::BufferId bufferId = getFrameOverrideBufferId(bufferIds, frameSlot);
	    if (bufferId != rendering::BufferId::Invalid)
	      overrides.bufferOverrides.emplace(buffer.name, rendering::RenderGraphBufferOverride{.bufferId = bufferId});
	  }

	  void writeFrameLocalBuffer(
	      const std::vector<rendering::BufferId> &bufferIds,
	      const rendering::Buffer &buffer,
      uint64_t offset,
      uint64_t size,
      const void *data) const
  {
    const rendering::BufferId overrideBufferId = getCurrentFrameOverrideBufferId(bufferIds);
    if (overrideBufferId != rendering::BufferId::Invalid)
    {
      renderGraph->getRHI()->bufferWrite(overrideBufferId, offset, size, const_cast<void *>(data));
      return;
    }

	    renderGraph->bufferWrite(buffer, offset, size, const_cast<void *>(data));
	  }

  void deleteBuffer(rendering::Buffer &buffer) const
  {
    if (buffer.isValid())
    {
      renderGraph->deleteBuffer(buffer);
      buffer = {};
    }
  }

  rendering::Buffer createStorageBufferWithData(const std::string &name, uint64_t size, const void *data) const
  {
    rendering::Buffer buffer = renderGraph->createBuffer(
        rendering::BufferInfo{
            .name = name,
            .size = std::max<uint64_t>(size, sizeof(uint32_t)),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
        });
    if (data != nullptr && size != 0u)
      renderGraph->bufferWrite(buffer, 0u, size, const_cast<void *>(data));
    else
    {
      uint32_t zero = 0u;
      renderGraph->bufferWrite(buffer, 0u, sizeof(zero), &zero);
    }
    return buffer;
  }

  void createVSDFRuntimeResources()
  {
    const uint32_t zero = 0u;
    const std::array<uint32_t, 8u> emptyInfo{};

    vsdfRuntimeInfoBuffer_ = renderGraph->createBuffer(
        rendering::BufferInfo{
            .name = passName + "_VSDFRuntimeInfo.buffer",
            .size = sizeof(emptyInfo),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    renderGraph->bufferWrite(vsdfRuntimeInfoBuffer_, 0u, sizeof(emptyInfo), const_cast<uint32_t *>(emptyInfo.data()));

    vsdfInstanceBuffer_ = createStorageBufferWithData(passName + "_VSDFInstances.buffer", sizeof(zero), &zero);
    vsdfCardBuffer_ = createStorageBufferWithData(passName + "_VSDFCards.buffer", sizeof(zero), &zero);
    vsdfPageBuffer_ = createStorageBufferWithData(passName + "_VSDFPages.buffer", sizeof(zero), &zero);
    vsdfAlbedoBuffer_ = createStorageBufferWithData(passName + "_VSDFAlbedoPages.buffer", sizeof(zero), &zero);
    vsdfNormalBuffer_ = createStorageBufferWithData(passName + "_VSDFNormalPages.buffer", sizeof(zero), &zero);
    vsdfCascadeBuffer_ = createStorageBufferWithData(passName + "_VSDFCascades.buffer", sizeof(zero), &zero);
    vsdfCascadeInstanceBuffer_ = createStorageBufferWithData(passName + "_VSDFCascadeInstanceIndices.buffer", sizeof(zero), &zero);
    vsdfSdfChunkBuffer_ = createStorageBufferWithData(passName + "_VSDFSdfChunks.buffer", sizeof(zero), &zero);
    vsdfSdfWordBuffer_ = createStorageBufferWithData(passName + "_VSDFSdfWords.buffer", sizeof(zero), &zero);

    const std::array<uint32_t, 16u> sparseTLASInfo{};
    vsdfSparseTLASInfoBuffer_ = renderGraph->createBuffer(
        rendering::BufferInfo{
            .name = passName + "_VSDFSparseTLASInfo.buffer",
            .size = sizeof(sparseTLASInfo),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    renderGraph->bufferWrite(vsdfSparseTLASInfoBuffer_, 0u, sizeof(sparseTLASInfo), const_cast<uint32_t *>(sparseTLASInfo.data()));
    vsdfSparseTLASPageTableBuffer_ = createStorageBufferWithData(passName + "_VSDFSparseTLASPageTable.buffer", sizeof(zero), &zero);
    vsdfSparseTLASCellBuffer_ = createStorageBufferWithData(passName + "_VSDFSparseTLASCells.buffer", sizeof(zero), &zero);
    vsdfSparseTLASInstanceRefBuffer_ = createStorageBufferWithData(passName + "_VSDFSparseTLASInstanceRefs.buffer", sizeof(zero), &zero);
  }

	  void readFrameLocalBuffer(
	      const rendering::Buffer &buffer,
	      const std::vector<rendering::BufferId> &bufferIds,
	      uint64_t offset,
	      uint64_t size,
	      std::function<void(const void *)> callback) const
	  {
	    const rendering::BufferId overrideBufferId = getCurrentFrameOverrideBufferId(bufferIds);
	    if (overrideBufferId != rendering::BufferId::Invalid)
	    {
	      renderGraph->getRHI()->bufferRead(overrideBufferId, offset, size, std::move(callback));
	      return;
	    }

	    renderGraph->bufferRead(buffer, offset, size, std::move(callback));
	  }

	  void writeUniformsAllFrames()
	  {
	    const uint32_t frameCount = std::max(1u, getMaxFramesInFlight());
	    const uint32_t originalFrameIndex = getCurrentFrameIndex();
	    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
	    {
	      renderGraph->setCurrentFrameIndex(frameSlot);
	      writeUniforms();
	    }
	    renderGraph->setCurrentFrameIndex(originalFrameIndex);
	  }

	  void writeCascadeInfosAllFrames()
	  {
	    const uint32_t frameCount = std::max(1u, getMaxFramesInFlight());
	    const uint32_t originalFrameIndex = getCurrentFrameIndex();
	    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
	    {
	      renderGraph->setCurrentFrameIndex(frameSlot);
	      writeCascadeInfos();
	    }
	    renderGraph->setCurrentFrameIndex(originalFrameIndex);
	  }

	  void clearFeedbackAllFrames()
	  {
	    const uint32_t frameCount = std::max(1u, getMaxFramesInFlight());
	    const uint32_t originalFrameIndex = getCurrentFrameIndex();
	    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
	    {
	      renderGraph->setCurrentFrameIndex(frameSlot);
	      clearFeedback();
	    }
	    renderGraph->setCurrentFrameIndex(originalFrameIndex);
	  }

  void clearLocalTraceStatsAllFrames()
  {
    const uint32_t frameCount = std::max(1u, getMaxFramesInFlight());
    const uint32_t originalFrameIndex = getCurrentFrameIndex();
    for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
    {
      renderGraph->setCurrentFrameIndex(frameSlot);
      clearLocalTraceStats();
    }
    renderGraph->setCurrentFrameIndex(originalFrameIndex);
  }

		  GPUSignedDistanceFieldsManager &manager_;
	  virtualgeometry::VirtualGeometryScene *scene_ = nullptr;
	  rendering::Texture outputTexture_;
  Settings settings_{};
  bool enabled_ = false;
  uint64_t recordCallCount_ = 0u;
  uint64_t recordDispatchCount_ = 0u;
  uint64_t recordSkippedDisabledCount_ = 0u;
  bool reverseZ_ = true;
  bool forceResourceRebuild_ = false;
  math::Mat4f inverseViewProjection_ = math::Mat4f::identity();
  math::Mat4f viewProjection_ = math::Mat4f::identity();
  math::Vec3f cameraPosition_ = math::Vec3f(0.0f, 0.0f, 0.0f);

  rendering::BindingsLayout layout_;
  rendering::BindingGroups bindingGroups_;
  rendering::Shader shader_;
  rendering::ComputePipeline pipeline_;
  rendering::Buffer uniformBuffer_;
  rendering::Buffer cascadeInfoBuffer_;
  rendering::Buffer fallbackBrickMapBuffer_;
  rendering::Buffer fallbackAabbTreeBuffer_;
	  rendering::Texture fallbackSurfaceCacheMaterialReferenceAtlas_;
	  rendering::Texture fallbackSurfaceCacheMaterialReferenceBasisAtlas_;
	  rendering::Buffer materialRuntimeBuffer_;
	  rendering::Buffer atlasInfoBuffer_;
	  rendering::Buffer textureEntriesBuffer_;
	  rendering::Buffer vtPageTableBuffer_;
	  rendering::Buffer vtPhysicalPagesBuffer_;
	  rendering::Buffer feedbackBuffer_;
	  rendering::Buffer feedbackReadbackBuffer_;
  rendering::Buffer localTraceStatsBuffer_;
  rendering::Buffer localTraceStatsReadbackBuffer_;
  rendering::Buffer vsdfRuntimeInfoBuffer_;
  rendering::Buffer vsdfInstanceBuffer_;
  rendering::Buffer vsdfCardBuffer_;
  rendering::Buffer vsdfPageBuffer_;
  rendering::Buffer vsdfAlbedoBuffer_;
  rendering::Buffer vsdfNormalBuffer_;
  rendering::Buffer vsdfCascadeBuffer_;
  rendering::Buffer vsdfCascadeInstanceBuffer_;
  rendering::Buffer vsdfSdfChunkBuffer_;
  rendering::Buffer vsdfSdfWordBuffer_;
  rendering::Buffer vsdfSparseTLASInfoBuffer_;
  rendering::Buffer vsdfSparseTLASPageTableBuffer_;
  rendering::Buffer vsdfSparseTLASCellBuffer_;
  rendering::Buffer vsdfSparseTLASInstanceRefBuffer_;
  rendering::Sampler sdfAtlasSampler_;
	  std::vector<rendering::BufferId> frameOverrideUniformBufferIds_{};
	  std::vector<rendering::BufferId> frameOverrideCascadeInfoBufferIds_{};
	  std::vector<rendering::BufferId> frameOverrideMaterialRuntimeBufferIds_{};
	  std::vector<rendering::BufferId> frameOverrideAtlasInfoBufferIds_{};
	  std::vector<rendering::BufferId> frameOverrideTextureEntriesBufferIds_{};
	  std::vector<rendering::BufferId> frameOverridePageTableBufferIds_{};
	  std::vector<rendering::BufferId> frameOverridePhysicalPagesBufferIds_{};
	  std::vector<rendering::BufferId> frameOverrideFeedbackBufferIds_{};
	  std::vector<rendering::BufferId> frameOverrideFeedbackReadbackBufferIds_{};
  std::vector<rendering::BufferId> frameOverrideLocalTraceStatsBufferIds_{};
  std::vector<rendering::BufferId> frameOverrideLocalTraceStatsReadbackBufferIds_{};
		  uint64_t feedbackBufferSize_ = 0u;
	  uint64_t syncedPageTableVersion_ = 0u;
	  uint64_t syncedPhysicalPageVersion_ = 0u;
	  virtualgeometry::VirtualTextureSystem::FeedbackStats lastFeedbackStats_{};
  LocalTraceStats lastLocalTraceStats_{};
	  std::vector<uint32_t> pendingPhysicalPageUploads_{};
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
