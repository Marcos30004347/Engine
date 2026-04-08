#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualgeometry/rendering/VirtualGeometryCullingMultipleDispatchesPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryDepthPrePassDrawPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryDepthPyramidPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryHardwareDrawPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryMaterialPass.hpp"
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
    VirtualGeometryMaterialPass::Settings materialPassSettings;
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

  void recordCommandBuffer(rendering::CommandRecorder &) override
  {
    uint32_t childIndex = getIndex() + 1u;
    auto prepassCullingSettings = settings_.prepassCullingSettings;
    auto finalCullingSettings = settings_.finalCullingSettings;
    prepassCullingSettings.enableStreamingPriorityUpdates = false;
    finalCullingSettings.enableStreamingPriorityUpdates = true;

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

    if (settings_.registerMaterialPass)
    {
      materialPass_ = renderGraph->registerPass<VirtualGeometryMaterialPass>(passName + ".material", childIndex++, mainCommandBuffer_, scene_, frameTarget_, shadowLightingTexture_, settings_.materialPassSettings);
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
  VirtualGeometryMaterialPass *getMaterialPass() const
  {
    return materialPass_;
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
  const rendering::Texture &getSceneDepthTexture() const
  {
    return frameTarget_.depthTexture;
  }
  const rendering::Texture &getPrepassDepthHZBTexture() const
  {
    return previousDepthHZBTexture_;
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
  VirtualGeometryDepthPrePassDrawPass *depthPrePass_ = nullptr;
  VirtualGeometryDepthPyramidPass *depthPyramidPass_ = nullptr;
  VirtualGeometryCullingMultipleDispatchesPass *finalCullingPass_ = nullptr;
  VirtualGeometryStreamingPageSelectionPass *streamingPageSelectionPass_ = nullptr;
  VirtualGeometryHardwareDrawPass *hardwareDrawPass_ = nullptr;
  VirtualGeometryMaterialPass *materialPass_ = nullptr;
};

} // namespace gpgpu
} // namespace virtualgeometry
