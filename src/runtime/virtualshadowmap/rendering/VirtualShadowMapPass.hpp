#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#include "time/TimeSpan.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualshadowmap/VirtualShadowMapManager.hpp"
#include "virtualshadowmap/rendering/VirtualShadowMapBookkeepingPass.hpp"
#include "virtualshadowmap/rendering/VirtualShadowMapDebugPass.hpp"
#include "virtualshadowmap/rendering/VirtualShadowMapDrawPass.hpp"
#include "virtualshadowmap/rendering/VirtualShadowMapRayTraceDepthPass.hpp"
#if defined(__APPLE__) && defined(ENGINE_VG_HAS_METAL_RAYTRACE)
#include "virtualgeometry/raytracing/metal/MetalPageBlasGate.hpp"
#endif
#include "virtualshadowmap/rendering/VirtualShadowMapScreenSpaceSoftShadowPass.hpp"

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
#if defined(__APPLE__) && defined(ENGINE_VG_HAS_METAL_RAYTRACE)
    // The caller supplies a separate complete cut using the install gate's backend.
    std::shared_ptr<raytracing::VirtualGeometryMetalRayTraceBackend> hardwareDepthBackend;
    rendering::Buffer hardwareDepthInstances, hardwareDepthInstanceCount;
    VirtualShadowMapRayTraceDepthPass::Settings hardwareDepthSettings;
#endif
    bool enableTableDebugPass = false;
    VirtualShadowMapTableDebugPass::Settings tableDebugSettings;
    bool enableAtlasDebugPass = false;
    VirtualShadowMapAtlasDebugPass::Settings atlasDebugSettings;
    bool enableScreenSpaceShadowPass = true;
    VirtualShadowMapScreenSpaceShadowPass::Settings screenSpaceShadowSettings;
    SoftShadowMethod softShadowMethod = SoftShadowMethod::Hard;
    VirtualShadowMapScreenSpaceSoftShadowPass::Settings screenSpaceSoftShadowSettings;
    bool enableShadowPcfPass = true;
    VirtualShadowMapShadowPcfPass::Settings shadowPcfSettings;
  };

  VirtualShadowMapPass(
      VirtualGeometryScene &scene,
      VirtualShadowMapManager &manager,
      rendering::Texture depthTexture,
      rendering::Texture normalTexture,
      rendering::Texture pagesDebugTexture,
      rendering::Texture tableDebugTexture,
      rendering::Texture atlasDebugTexture,
      rendering::Texture screenSpaceShadowTexture,
      rendering::Texture screenSpaceSoftShadowDebugTexture,
      rendering::Texture shadowLightingTexture)
      : VirtualShadowMapPass(scene, manager, depthTexture, normalTexture, pagesDebugTexture, tableDebugTexture, atlasDebugTexture, screenSpaceShadowTexture, screenSpaceSoftShadowDebugTexture, shadowLightingTexture, Settings{})
  {
  }

  ~VirtualShadowMapPass() override
  {
    delete shadowPcfPass_;
    shadowPcfPass_ = nullptr;
    delete screenSpaceShadowPass_;
    screenSpaceShadowPass_ = nullptr;
    delete screenSpaceSoftShadowPass_;
    screenSpaceSoftShadowPass_ = nullptr;
    delete tableDebugPass_;
    tableDebugPass_ = nullptr;
    delete atlasDebugPass_;
    atlasDebugPass_ = nullptr;
#if defined(__APPLE__) && defined(ENGINE_VG_HAS_METAL_RAYTRACE)
    delete rayTraceDepthPass_;
#endif
    delete drawPass_;
    drawPass_ = nullptr;
    delete bookkeepingPass_;
    bookkeepingPass_ = nullptr;
  }

  VirtualShadowMapPass(
      VirtualGeometryScene &scene,
      VirtualShadowMapManager &manager,
      rendering::Texture depthTexture,
      rendering::Texture normalTexture,
      rendering::Texture pagesDebugTexture,
      rendering::Texture tableDebugTexture,
      rendering::Texture atlasDebugTexture,
      rendering::Texture screenSpaceShadowTexture,
      rendering::Texture screenSpaceSoftShadowDebugTexture,
      rendering::Texture shadowLightingTexture,
      Settings settings)
      : scene_(scene), manager_(manager), depthTexture_(depthTexture), normalTexture_(normalTexture), pagesDebugTexture_(pagesDebugTexture), tableDebugTexture_(tableDebugTexture), atlasDebugTexture_(atlasDebugTexture), screenSpaceShadowTexture_(screenSpaceShadowTexture),
        screenSpaceSoftShadowDebugTexture_(screenSpaceSoftShadowDebugTexture),
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
#if defined(__APPLE__) && defined(ENGINE_VG_HAS_METAL_RAYTRACE)
    if (settings_.hardwareDepthBackend) {
      const auto gate=std::dynamic_pointer_cast<raytracing::MetalPageBlasGate>(scene_.settings.pageBlasGate);
      if (!gate || gate->backend()!=settings_.hardwareDepthBackend)
        throw std::invalid_argument("Hardware VSM pages require the scene installation backend");
      rayTraceDepthPass_=renderGraph->registerPass<VirtualShadowMapRayTraceDepthPass>(
          passName+".rayTraceDepth",getIndex()+2u,manager_,settings_.hardwareDepthBackend,
          settings_.hardwareDepthInstances,settings_.hardwareDepthInstanceCount,settings_.hardwareDepthSettings);
    } else
#endif
    drawPass_ = renderGraph->registerPass<VirtualShadowMapDrawPass>(passName + ".draw", getIndex() + 2u, scene_, manager_, settings_.drawSettings);
    uint32_t producerExtraPasses=0;
#if defined(__APPLE__) && defined(ENGINE_VG_HAS_METAL_RAYTRACE)
    producerExtraPasses=settings_.hardwareDepthBackend ? 5u : 0u;
#endif
    if (settings_.enableTableDebugPass)
    {
      os::Logger::logf("Registering VirtualShadowMapTableDebugPass");
      tableDebugPass_ = renderGraph->registerPass<VirtualShadowMapTableDebugPass>(passName + ".tableDebug", getIndex() + producerExtraPasses + 3u, manager_, depthTexture_, tableDebugTexture_, settings_.tableDebugSettings);
    }
    if (settings_.enableAtlasDebugPass)
    {
      os::Logger::logf("Registering VirtualShadowMapAtlasDebugPass");
      atlasDebugPass_ = renderGraph->registerPass<VirtualShadowMapAtlasDebugPass>(passName + ".atlasDebug", getIndex() + producerExtraPasses + 4u, manager_, depthTexture_, atlasDebugTexture_, settings_.atlasDebugSettings);
    }
    if (settings_.enableScreenSpaceShadowPass)
    {
      os::Logger::logf("Registering VirtualShadowMapScreenSpaceShadowPass");
      screenSpaceShadowPass_ = renderGraph->registerPass<VirtualShadowMapScreenSpaceShadowPass>(
          passName + ".screenSpaceShadow", getIndex() + producerExtraPasses + 5u, manager_, depthTexture_, screenSpaceShadowTexture_, settings_.screenSpaceShadowSettings);
    }
    if (settings_.softShadowMethod == SoftShadowMethod::LayeredScreenSpaceSoftShadows)
    {
      os::Logger::logf("Registering VirtualShadowMapScreenSpaceSoftShadowPass");
      screenSpaceSoftShadowPass_ = renderGraph->registerPass<VirtualShadowMapScreenSpaceSoftShadowPass>(
          passName + ".screenSpaceSoftShadow", getIndex() + producerExtraPasses + 6u, manager_, depthTexture_, normalTexture_, screenSpaceSoftShadowDebugTexture_, settings_.screenSpaceSoftShadowSettings);
    }
    if (settings_.enableShadowPcfPass)
    {
      os::Logger::logf("Registering VirtualShadowMapShadowPcfPass");
      shadowPcfPass_ = renderGraph->registerPass<VirtualShadowMapShadowPcfPass>(
          passName + ".shadowLighting", getIndex() + producerExtraPasses + 7u, manager_, depthTexture_, screenSpaceShadowTexture_, screenSpaceSoftShadowPass_, normalTexture_, shadowLightingTexture_, settings_.shadowPcfSettings, bookkeepingPass_->getLightTilesBuffer());
    }
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

  VirtualShadowMapAtlasDebugPass *getAtlasDebugPass() const
  {
    return atlasDebugPass_;
  }

  VirtualShadowMapScreenSpaceShadowPass *getScreenSpaceShadowPass() const
  {
    return screenSpaceShadowPass_;
  }

  VirtualShadowMapScreenSpaceSoftShadowPass *getScreenSpaceSoftShadowPass() const
  {
    return screenSpaceSoftShadowPass_;
  }

  VirtualShadowMapShadowPcfPass *getShadowPcfPass() const
  {
    return shadowPcfPass_;
  }

private:
  VirtualGeometryScene &scene_;
  VirtualShadowMapManager &manager_;
  rendering::Texture depthTexture_;
  rendering::Texture normalTexture_;
  rendering::Texture pagesDebugTexture_;
  rendering::Texture tableDebugTexture_;
  rendering::Texture atlasDebugTexture_;
  rendering::Texture screenSpaceShadowTexture_;
  rendering::Texture screenSpaceSoftShadowDebugTexture_;
  rendering::Texture shadowLightingTexture_;
  Settings settings_;
  VirtualShadowMapBookkeepingPass *bookkeepingPass_ = nullptr;
  VirtualShadowMapDrawPass *drawPass_ = nullptr;
#if defined(__APPLE__) && defined(ENGINE_VG_HAS_METAL_RAYTRACE)
  VirtualShadowMapRayTraceDepthPass *rayTraceDepthPass_ = nullptr;
#endif
  VirtualShadowMapTableDebugPass *tableDebugPass_ = nullptr;
  VirtualShadowMapAtlasDebugPass *atlasDebugPass_ = nullptr;
  VirtualShadowMapScreenSpaceShadowPass *screenSpaceShadowPass_ = nullptr;
  VirtualShadowMapScreenSpaceSoftShadowPass *screenSpaceSoftShadowPass_ = nullptr;
  VirtualShadowMapShadowPcfPass *shadowPcfPass_ = nullptr;
};

} // namespace gpgpu
} // namespace virtualgeometry
