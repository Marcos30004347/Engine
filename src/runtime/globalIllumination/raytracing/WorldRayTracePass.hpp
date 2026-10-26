#pragma once

#include <algorithm>
#include <cstdint>

#include "rendering/gpu/RenderGraph.hpp"
#include "softwarebvh/SoftwareBLASManager.hpp"
#include "softwarebvh/SoftwareTLASManager.hpp"
#include "softwarebvh/rendering/SoftwareRayTracePass.hpp"

namespace globalillumination::raytracing
{

class WorldRayTracePass : public softwarebvh::rendering::SoftwareRayTracePass
{
public:
  using Settings = softwarebvh::rendering::SoftwareRayTracePass::Settings;
  using TraceRayGPU = softwarebvh::rendering::SoftwareRayTracePass::TraceRayGPU;
  using HitOutputGPU = softwarebvh::rendering::SoftwareRayTracePass::HitOutputGPU;

  WorldRayTracePass(
      softwarebvh::SoftwareTLASManager &tlasManager,
      softwarebvh::SoftwareBLASManager &blasManager,
      rendering::Buffer rayInputBuffer,
      rendering::Buffer hitOutputBuffer,
      Settings settings)
      : softwarebvh::rendering::SoftwareRayTracePass(tlasManager, blasManager, sanitizeSettings(settings))
  {
    const Settings sanitized = sanitizeSettings(settings);
    setExternalRayBuffers(rayInputBuffer, rayInputBufferSize(sanitized.rayCount), hitOutputBuffer, hitOutputBufferSize(sanitized.rayCount));
  }

  static constexpr uint64_t rayInputBufferSize(uint32_t rayCount)
  {
    return static_cast<uint64_t>(rayCount == 0u ? 1u : rayCount) * sizeof(TraceRayGPU);
  }

  static constexpr uint64_t hitOutputBufferSize(uint32_t rayCount)
  {
    return static_cast<uint64_t>(rayCount == 0u ? 1u : rayCount) * sizeof(HitOutputGPU);
  }

  void setSettings(const Settings &settings)
  {
    softwarebvh::rendering::SoftwareRayTracePass::setSettings(sanitizeSettings(settings));
  }

private:
  static Settings sanitizeSettings(Settings settings)
  {
    settings.generateCameraRays = false;
    settings.width = 1u;
    settings.height = 1u;
    settings.rayCount = std::max(1u, settings.rayCount);
    return settings;
  }
};

} // namespace globalillumination::raytracing
