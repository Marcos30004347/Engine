#pragma once

#include <algorithm>
#include <memory>

#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualgeometry/rendering/VirtualGeometryCullingMultipleDispatchesPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryDepthPrePassDrawPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryDepthPyramidPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryHardwareDrawPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryPageDecodePass.hpp"
#include "virtualgeometry/rendering/MaterialPass.hpp"
#include "virtualgeometry/rendering/ScreenSpaceRayTracePass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryStreamingPageSelectionPass.hpp"

namespace virtualgeometry
{
namespace gpgpu
{

class VirtualGeometryRendererPass : public rendering::Pass
{
public:
  struct Settings
  {
    VirtualGeometryCullingMultipleDispatchesPass::Settings prepassCullingSettings;
    VirtualGeometryDepthPrePassDrawPass::Settings depthPrePassSettings;
    VirtualGeometryDepthPyramidPass::Settings depthPyramidSettings;
    VirtualGeometryCullingMultipleDispatchesPass::Settings finalCullingSettings;
    VirtualGeometryHardwareDrawPass::Settings hardwareDrawSettings;
    MaterialPass::Settings materialPassSettings;
    ScreenSpaceRayTracePass::Settings screenSpaceRayTraceSettings;
    bool registerScreenSpaceRayTracePass = true;
    bool registerMaterialPass = true;
  };

  VirtualGeometryRendererPass(
      VirtualGeometryScene &scene,
      VirtualGeometryHardwareDrawPass::FrameTarget frameTarget,
      VirtualGeometryDepthPrePassDrawPass::FrameTarget prePassTarget,
      rendering::Texture shadowLightingTexture,
      rendering::Texture previousDepthHZBTexture,
      float clearDepth,
      Settings settings)
      : scene_(scene), frameTarget_(frameTarget), prePassTarget_(prePassTarget), shadowLightingTexture_(shadowLightingTexture), previousDepthHZBTexture_(previousDepthHZBTexture), clearDepth_(clearDepth),
        settings_(settings)
  {
  }

  ~VirtualGeometryRendererPass() override
  {
    delete materialDispatchPass_;
    materialDispatchPass_ = nullptr;
    delete materialPreparePass_;
    materialPreparePass_ = nullptr;
    delete materialSetupPass_;
    materialSetupPass_ = nullptr;
    materialPass_.reset();
    delete hardwareDrawPass_;
    hardwareDrawPass_ = nullptr;
    delete screenSpaceRayTracePass_;
    screenSpaceRayTracePass_ = nullptr;
    delete pageDecodePass_;
    pageDecodePass_ = nullptr;
    delete streamingPageSelectionPass_;
    streamingPageSelectionPass_ = nullptr;
    delete finalCullingPass_;
    finalCullingPass_ = nullptr;
    delete depthPyramidPass_;
    depthPyramidPass_ = nullptr;
    delete depthPrePass_;
    depthPrePass_ = nullptr;
    delete preCullingPass_;
    preCullingPass_ = nullptr;
  }

  void recordCommandBuffer(rendering::CommandRecorder &) override
  {
    uint32_t childIndex = getIndex() + 1u;
    auto prepassCullingSettings = settings_.prepassCullingSettings;
    auto finalCullingSettings = settings_.finalCullingSettings;
    prepassCullingSettings.enableStreamingPriorityUpdates = false;
    finalCullingSettings.enableStreamingPriorityUpdates = true;

    pageDecodePass_ = renderGraph->registerPass<VirtualGeometryPageDecodePass>(passName + ".pageDecode", childIndex++, prepassCommandBuffer_, scene_);

    preCullingPass_ = renderGraph->registerPass<VirtualGeometryCullingMultipleDispatchesPass>(passName + ".preCulling", childIndex++, prepassCommandBuffer_, scene_, prepassCullingSettings, previousDepthHZBTexture_);

    depthPrePass_ = renderGraph->registerPass<VirtualGeometryDepthPrePassDrawPass>(
        passName + ".depthPrePass",
        childIndex++,
        prepassCommandBuffer_,
        scene_,
        preCullingPass_->getHWDrawIndirectBuffer(),
        preCullingPass_->getHWVisibleClusterInfosBuffer(),
        preCullingPass_->getCullingStatisticsBuffer(),
        prePassTarget_,
        rendering::LoadOp::LoadOp_Clear,
        clearDepth_,
        settings_.depthPrePassSettings);

    depthPyramidPass_ =
        renderGraph->registerPass<VirtualGeometryDepthPyramidPass>(passName + ".depthPyramid", childIndex++, prepassCommandBuffer_, frameTarget_.depthTexture, previousDepthHZBTexture_, settings_.depthPyramidSettings);

    finalCullingPass_ = renderGraph->registerPass<VirtualGeometryCullingMultipleDispatchesPass>(passName + ".finalCulling", childIndex++, mainCommandBuffer_, scene_, finalCullingSettings, previousDepthHZBTexture_);

    streamingPageSelectionPass_ = renderGraph->registerPass<VirtualGeometryStreamingPageSelectionPass>(passName + ".streamingSelection", childIndex++, mainCommandBuffer_, scene_);

    hardwareDrawPass_ = renderGraph->registerPass<VirtualGeometryHardwareDrawPass>(
        passName + ".hardwareDraw",
        childIndex++,
        mainCommandBuffer_,
        scene_,
        finalCullingPass_->getHWDrawIndirectBuffer(),
        finalCullingPass_->getHWVisibleClusterInfosBuffer(),
        finalCullingPass_->getCullingStatisticsBuffer(),
        frameTarget_,
        rendering::LoadOp::LoadOp_Clear,
        clearDepth_,
        settings_.hardwareDrawSettings);

    if (settings_.registerScreenSpaceRayTracePass)
    {
      auto screenSpaceRayTraceSettings = settings_.screenSpaceRayTraceSettings;
      if (screenSpaceRayTraceSettings.width == 0u)
        screenSpaceRayTraceSettings.width = settings_.hardwareDrawSettings.viewPortWidth;
      if (screenSpaceRayTraceSettings.height == 0u)
        screenSpaceRayTraceSettings.height = settings_.hardwareDrawSettings.viewPortHeight;
      if (screenSpaceRayTraceSettings.hzbMipCount == 0u)
        screenSpaceRayTraceSettings.hzbMipCount = settings_.depthPyramidSettings.mipCount;
      screenSpaceRayTracePass_ = renderGraph->registerPass<ScreenSpaceRayTracePass>(
          passName + ".screenSpaceRayTrace",
          childIndex++,
          mainCommandBuffer_,
          frameTarget_.depthTexture,
          frameTarget_.geometryNormalTexture,
          previousDepthHZBTexture_,
          screenSpaceRayTraceSettings);
    }

    if (settings_.registerMaterialPass)
    {
      materialPass_ = std::make_unique<MaterialPass>(scene_, frameTarget_, shadowLightingTexture_, settings_.materialPassSettings);
      materialPass_->initialize(*renderGraph, passName + ".material");
      materialSetupPass_ = renderGraph->registerPass<MaterialSetupPass>(passName + ".material.setup", childIndex++, mainCommandBuffer_, *materialPass_);
      materialPreparePass_ = renderGraph->registerPass<MaterialPreparePass>(passName + ".material.prepare", childIndex++, mainCommandBuffer_, *materialPass_);
      materialDispatchPass_ = renderGraph->registerPass<MaterialDispatchPass>(passName + ".material.dispatch", childIndex++, mainCommandBuffer_, *materialPass_);
    }

  }

  VirtualGeometryCullingMultipleDispatchesPass *getPreCullingPass() const
  {
    return preCullingPass_;
  }
  VirtualGeometryDepthPrePassDrawPass *getDepthPrePass() const
  {
    return depthPrePass_;
  }
  VirtualGeometryDepthPyramidPass *getDepthPyramidPass() const
  {
    return depthPyramidPass_;
  }
  VirtualGeometryCullingMultipleDispatchesPass *getFinalCullingPass() const
  {
    return finalCullingPass_;
  }
  VirtualGeometryHardwareDrawPass *getHardwareDrawPass() const
  {
    return hardwareDrawPass_;
  }
  MaterialPass *getMaterialPass() const
  {
    return materialPass_.get();
  }
  ScreenSpaceRayTracePass *getScreenSpaceRayTracePass() const
  {
    return screenSpaceRayTracePass_;
  }
  VirtualGeometryStreamingPageSelectionPass *getStreamingPageSelectionPass() const
  {
    return streamingPageSelectionPass_;
  }
  const rendering::Texture &getMaterialIdTexture() const
  {
    return frameTarget_.materialIdView.texture;
  }
  const rendering::Texture &getColorTexture() const
  {
    return frameTarget_.colorView.texture;
  }
  const rendering::Texture &getBaseColorTexture() const
  {
    return frameTarget_.baseColorTexture;
  }
  const rendering::Texture &getGeometryNormalTexture() const
  {
    return frameTarget_.geometryNormalTexture;
  }
  const rendering::Texture &getNormalTexture() const
  {
    return frameTarget_.normalTexture;
  }
  const rendering::Texture &getSceneDepthTexture() const
  {
    return frameTarget_.depthTexture;
  }
  const rendering::Texture &getMotionVectorTexture() const
  {
    return frameTarget_.motionVectorTexture;
  }
  const rendering::Texture &getPrepassDepthHZBTexture() const
  {
    return previousDepthHZBTexture_;
  }
  const rendering::Texture &getScreenSpaceRayTraceStepTexture() const
  {
    if (screenSpaceRayTracePass_ != nullptr)
      return screenSpaceRayTracePass_->getStepCountTexture();
    return frameTarget_.colorTexture;
  }
private:

  VirtualGeometryScene &scene_;
  VirtualGeometryHardwareDrawPass::FrameTarget frameTarget_;
  VirtualGeometryDepthPrePassDrawPass::FrameTarget prePassTarget_;
  rendering::Texture shadowLightingTexture_;
  rendering::Texture previousDepthHZBTexture_;
  float clearDepth_ = 0.0f;
  Settings settings_;

  rendering::CommandRecorder prepassCommandBuffer_;
  rendering::CommandRecorder mainCommandBuffer_;

  VirtualGeometryCullingMultipleDispatchesPass *preCullingPass_ = nullptr;
  VirtualGeometryPageDecodePass *pageDecodePass_ = nullptr;
  VirtualGeometryDepthPrePassDrawPass *depthPrePass_ = nullptr;
  VirtualGeometryDepthPyramidPass *depthPyramidPass_ = nullptr;
  VirtualGeometryCullingMultipleDispatchesPass *finalCullingPass_ = nullptr;
  VirtualGeometryStreamingPageSelectionPass *streamingPageSelectionPass_ = nullptr;
  VirtualGeometryHardwareDrawPass *hardwareDrawPass_ = nullptr;
  ScreenSpaceRayTracePass *screenSpaceRayTracePass_ = nullptr;
  MaterialSetupPass *materialSetupPass_ = nullptr;
  MaterialPreparePass *materialPreparePass_ = nullptr;
  MaterialDispatchPass *materialDispatchPass_ = nullptr;
  std::unique_ptr<MaterialPass> materialPass_;
};

} // namespace gpgpu
} // namespace virtualgeometry
