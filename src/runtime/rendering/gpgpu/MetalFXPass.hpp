#pragma once

#include "rendering/gpu/Types.hpp"

#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace rendering::backend::vulkan
{
class VulkanRHI;
}

namespace rendering::gpgpu
{

#if defined(__APPLE__)

// Diagnostic-only readback of the actual native output, after MetalFX runs.
// Never call in a throughput benchmark: this deliberately waits for a copy.
bool readMetalFXOutput(backend::vulkan::VulkanRHI &, TextureId,
    std::vector<float> &rgba, std::string *error = nullptr);

// Optional GPU-only handoff for a native render-graph segment. The caller keeps
// the shared event alive through command completion. No drawable is involved.
struct MetalFXSynchronization
{
  void *sharedEvent = nullptr;
  uint64_t readyValue = 0;
  uint64_t doneValue = 0;
};

// Native encoders write caller-owned, exportable private textures. Use
// MetalFXReconstructionPass to expose these dependencies to the render graph.
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
    MetalFXSynchronization synchronization{};
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

// Denoises the noisy diffuse ReSTIR image and performs temporal scaling in one
// operation. The caller supplies linear shaded colour and world-space normals.
class MetalFXTemporalDenoisedScalerPass
{
public:
  using Configuration = MetalFXTemporalUpscalerPass::Configuration;
  struct Frame : MetalFXTemporalUpscalerPass::Frame
  {
    TextureId diffuseAlbedo = TextureId::Invalid;
    TextureId normal = TextureId::Invalid;
    std::array<float,16> worldToView{}, viewToClip{};
  };
  MetalFXTemporalDenoisedScalerPass(backend::vulkan::VulkanRHI &, const Configuration &);
  ~MetalFXTemporalDenoisedScalerPass();
  bool available() const;
  bool encode(const Frame &, std::string *error = nullptr);
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
    MetalFXSynchronization synchronization{};
  };

  MetalFXFrameGenerationPass(
      backend::vulkan::VulkanRHI &rhi,
      const Configuration &configuration,
      const MetalFXTemporalUpscalerPass *upscaler = nullptr,
      const MetalFXTemporalDenoisedScalerPass *denoiser = nullptr);
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
