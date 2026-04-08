#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#include "time/TimeSpan.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualshadowmap/VirtualShadowMapManager.hpp"
#include "virtualshadowmap/rendering/VirtualShadowMapBookkeepingPass.hpp"
#include "virtualshadowmap/rendering/VirtualShadowMapDebugPass.hpp"
#include "virtualshadowmap/rendering/VirtualShadowMapDrawPass.hpp"

namespace virtualgeometry
{
namespace gpgpu
{

class VirtualShadowMapPass : public rendering::Pass
{
public:
  struct Settings
  {
    VirtualShadowMapBookkeepingPass::Settings bookkeepingSettings;
    VirtualShadowMapDrawPass::Settings drawSettings;
    bool enableTableDebugPass = false;
    VirtualShadowMapTableDebugPass::Settings tableDebugSettings;
    VirtualShadowMapScreenSpaceShadowPass::Settings screenSpaceShadowSettings;
    VirtualShadowMapShadowPcfPass::Settings shadowPcfSettings;
  };

  VirtualShadowMapPass(
      VirtualGeometryScene &scene,
      VirtualShadowMapManager &manager,
      rendering::Texture depthTexture,
      rendering::Texture pagesDebugTexture,
      rendering::Texture tableDebugTexture,
      rendering::Texture screenSpaceShadowTexture,
      rendering::Texture shadowLightingTexture)
      : VirtualShadowMapPass(scene, manager, depthTexture, pagesDebugTexture, tableDebugTexture, screenSpaceShadowTexture, shadowLightingTexture, Settings{})
  {
  }

  VirtualShadowMapPass(
      VirtualGeometryScene &scene,
      VirtualShadowMapManager &manager,
      rendering::Texture depthTexture,
      rendering::Texture pagesDebugTexture,
      rendering::Texture tableDebugTexture,
      rendering::Texture screenSpaceShadowTexture,
      rendering::Texture shadowLightingTexture,
      Settings settings)
      : scene_(scene), manager_(manager), depthTexture_(depthTexture), pagesDebugTexture_(pagesDebugTexture), tableDebugTexture_(tableDebugTexture), screenSpaceShadowTexture_(screenSpaceShadowTexture),
        shadowLightingTexture_(shadowLightingTexture),
        settings_(settings)
  {
  }

  void recordCommandBuffer(rendering::CommandRecorder &) override
  {
    const auto setupStart = lib::time::TimeSpan::now();
    os::Logger::logf("Registering VirtualShadowMapBookkeepingPass");
    bookkeepingPass_ = renderGraph->registerPass<VirtualShadowMapBookkeepingPass>(passName + ".bookkeeping", getIndex() + 1u, manager_, depthTexture_, pagesDebugTexture_, settings_.bookkeepingSettings);
    os::Logger::logf("Registering VirtualShadowMapDrawPass");
    drawPass_ = renderGraph->registerPass<VirtualShadowMapDrawPass>(passName + ".draw", getIndex() + 2u, scene_, manager_, settings_.drawSettings);
    if (settings_.enableTableDebugPass)
    {
      os::Logger::logf("Registering VirtualShadowMapTableDebugPass");
      tableDebugPass_ = renderGraph->registerPass<VirtualShadowMapTableDebugPass>(passName + ".tableDebug", getIndex() + 3u, manager_, depthTexture_, tableDebugTexture_, settings_.tableDebugSettings);
    }
    os::Logger::logf("Registering VirtualShadowMapScreenSpaceShadowPass");
    screenSpaceShadowPass_ = renderGraph->registerPass<VirtualShadowMapScreenSpaceShadowPass>(
        passName + ".screenSpaceShadow", getIndex() + 4u, depthTexture_, screenSpaceShadowTexture_, settings_.screenSpaceShadowSettings);
    os::Logger::logf("Registering VirtualShadowMapShadowPcfPass");
    shadowPcfPass_ = renderGraph->registerPass<VirtualShadowMapShadowPcfPass>(
        passName + ".shadowLighting", getIndex() + 5u, manager_, depthTexture_, screenSpaceShadowTexture_, shadowLightingTexture_, settings_.shadowPcfSettings);
    os::Logger::logf("Registered %s in %.2f ms", passName.c_str(), (lib::time::TimeSpan::now() - setupStart).milliseconds());
  }

  VirtualShadowMapBookkeepingPass *getBookkeepingPass() const
  {
    return bookkeepingPass_;
  }

  VirtualShadowMapDrawPass *getDrawPass() const
  {
    return drawPass_;
  }

  VirtualShadowMapTableDebugPass *getTableDebugPass() const
  {
    return tableDebugPass_;
  }

  VirtualShadowMapScreenSpaceShadowPass *getScreenSpaceShadowPass() const
  {
    return screenSpaceShadowPass_;
  }

  VirtualShadowMapShadowPcfPass *getShadowPcfPass() const
  {
    return shadowPcfPass_;
  }

private:
  VirtualGeometryScene &scene_;
  VirtualShadowMapManager &manager_;
  rendering::Texture depthTexture_;
  rendering::Texture pagesDebugTexture_;
  rendering::Texture tableDebugTexture_;
  rendering::Texture screenSpaceShadowTexture_;
  rendering::Texture shadowLightingTexture_;
  Settings settings_;
  VirtualShadowMapBookkeepingPass *bookkeepingPass_ = nullptr;
  VirtualShadowMapDrawPass *drawPass_ = nullptr;
  VirtualShadowMapTableDebugPass *tableDebugPass_ = nullptr;
  VirtualShadowMapScreenSpaceShadowPass *screenSpaceShadowPass_ = nullptr;
  VirtualShadowMapShadowPcfPass *shadowPcfPass_ = nullptr;
};

} // namespace gpgpu
} // namespace virtualgeometry
