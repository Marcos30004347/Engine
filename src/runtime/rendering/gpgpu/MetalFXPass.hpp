#pragma once

#include "rendering/gpu/Types.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace rendering::backend::vulkan
{
class VulkanRHI;
}

namespace rendering::gpgpu
{

#if defined(__APPLE__)

// Native MetalFX work deliberately sits outside RenderGraph command recording:
// MoltenVK owns the Vulkan resources, while MetalFX encodes on the Metal queue
// exported by that same RHI. Keeping this boundary in renderer passes makes it
// reusable without leaking Objective-C or Metal types into callers.
class MetalFXTemporalUpscalerPass
{
public:
  struct Configuration
  {
    uint32_t inputWidth = 1u;
    uint32_t inputHeight = 1u;
    uint32_t outputWidth = 1u;
    uint32_t outputHeight = 1u;
  };

  struct Frame
  {
    TextureId color = TextureId::Invalid;
    TextureId depth = TextureId::Invalid;
    TextureId motion = TextureId::Invalid;
    TextureId output = TextureId::Invalid;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    bool historyValid = false;
  };

  MetalFXTemporalUpscalerPass(
      backend::vulkan::VulkanRHI &rhi,
      const Configuration &configuration);
  ~MetalFXTemporalUpscalerPass();

  MetalFXTemporalUpscalerPass(const MetalFXTemporalUpscalerPass &) = delete;
  MetalFXTemporalUpscalerPass &operator=(
      const MetalFXTemporalUpscalerPass &) = delete;

  bool available() const;
  bool encode(const Frame &frame, std::string *error = nullptr);

private:
  friend class MetalFXFrameGenerationPass;
  void *nativeScaler() const;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class MetalFXFrameGenerationPass
{
public:
  struct Configuration
  {
    // Depth and motion-vector domain. The color inputs may already be at the
    // output size when a temporal upscaler is connected.
    uint32_t inputWidth = 1u;
    uint32_t inputHeight = 1u;
    uint32_t outputWidth = 1u;
    uint32_t outputHeight = 1u;
  };

  struct Frame
  {
    TextureId currentColor = TextureId::Invalid;
    TextureId previousColor = TextureId::Invalid;
    TextureId depth = TextureId::Invalid;
    TextureId motion = TextureId::Invalid;
    TextureId output = TextureId::Invalid;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float deltaSeconds = 1.0f / 60.0f;
    float nearPlane = 0.01f;
    float farPlane = 1000.0f;
    float verticalFovDegrees = 60.0f;
    bool historyValid = false;
  };

  MetalFXFrameGenerationPass(
      backend::vulkan::VulkanRHI &rhi,
      const Configuration &configuration,
      const MetalFXTemporalUpscalerPass *upscaler = nullptr);
  ~MetalFXFrameGenerationPass();

  MetalFXFrameGenerationPass(const MetalFXFrameGenerationPass &) = delete;
  MetalFXFrameGenerationPass &operator=(
      const MetalFXFrameGenerationPass &) = delete;

  bool available() const;
  bool encode(const Frame &frame, std::string *error = nullptr);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

#endif // defined(__APPLE__)

} // namespace rendering::gpgpu
