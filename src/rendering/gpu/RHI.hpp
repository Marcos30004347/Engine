#pragma once

#include "Types.hpp"

namespace rendering
{

enum class SwapChain : uint32_t
{
};

class RHI
{
protected:
  DeviceFeatures features;
  DeviceProperties properties;
  DeviceRequiredLimits requiredLimits;

public:
  virtual ~RHI() = default;

//   virtual const Buffer getBuffer(const std::string &name) = 0;
//   virtual const Texture getTexture(const std::string &name) = 0;

  virtual void bufferMapRead(const Buffer &buffer, const uint64_t offset, const uint64_t size, void **ptr) = 0;
  virtual void bufferUnmap(const Buffer &buffer) = 0;

  virtual void bufferWrite(const Buffer &buffer, const uint64_t offset, const uint64_t size, void *data) = 0;

  virtual const SwapChain createSwapChain(uint32_t surfaceIndex, uint32_t width, uint32_t height) = 0;
  virtual void destroySwapChain(SwapChain) = 0;

  virtual const TextureView getCurrentSwapChainTextureView(SwapChain swapChainHandle) = 0;
};

} // namespace rendering