#pragma once

#include "globalIllumination/raytracing/ScreenRayTracePass.hpp"
#include "globalIllumination/raytracing/WorldRayTracePass.hpp"

namespace globalillumination::raytracing
{

struct ScreenWorldRayTraceChain
{
  ScreenRayTracePass *screenTrace = nullptr;
  WorldRayTracePass *worldTrace = nullptr;

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    if (screenTrace != nullptr)
      screenTrace->appendFrameOverrides(overrides, frameSlot);
    if (worldTrace != nullptr)
      worldTrace->appendFrameOverrides(overrides, frameSlot);
  }

  void updateScreenUniforms(const math::Mat4f &viewProjection) const
  {
    if (screenTrace != nullptr)
      screenTrace->updateUniforms(viewProjection);
  }

  void setSettings(const ScreenRayTracePass::Settings &screenSettings, const WorldRayTracePass::Settings &worldSettings) const
  {
    if (screenTrace != nullptr)
      screenTrace->setSettings(screenSettings);
    if (worldTrace != nullptr)
      worldTrace->setSettings(worldSettings);
  }
};

} // namespace globalillumination::raytracing
