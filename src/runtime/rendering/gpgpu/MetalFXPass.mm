#include "rendering/gpgpu/MetalFXPass.hpp"

#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include <algorithm>

namespace rendering::gpgpu
{
namespace
{
id<MTLTexture> exportedTexture(
    backend::vulkan::VulkanRHI &rhi, const TextureId textureId)
{
  if (textureId == TextureId::Invalid)
    return nil;
  return (__bridge id<MTLTexture>)rhi.exportMetalTexture(textureId);
}

bool fail(std::string *error, const char *message)
{
  if (error != nullptr)
    *error = message;
  return false;
}
} // namespace

struct MetalFXTemporalUpscalerPass::Impl
{
  backend::vulkan::VulkanRHI &rhi;
  Configuration configuration{};
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLFXTemporalScaler> scaler = nil;
  bool supported = false;

  Impl(
      backend::vulkan::VulkanRHI &inRhi,
      const Configuration &inConfiguration)
      : rhi(inRhi), configuration(inConfiguration)
  {
    device = (__bridge id<MTLDevice>)rhi.exportMetalDevice();
    queue = (__bridge id<MTLCommandQueue>)
        rhi.exportMetalCommandQueue(Queue::Graphics);
    if (device == nil || queue == nil)
      return;

    if (@available(macOS 13.0, *))
    {
      supported = [MTLFXTemporalScalerDescriptor supportsDevice:device];
      if (!supported)
        return;

      MTLFXTemporalScalerDescriptor *descriptor =
          [[MTLFXTemporalScalerDescriptor alloc] init];
      descriptor.colorTextureFormat = MTLPixelFormatRGBA16Float;
      descriptor.depthTextureFormat = MTLPixelFormatDepth32Float;
      descriptor.motionTextureFormat = MTLPixelFormatRG16Float;
      descriptor.outputTextureFormat = MTLPixelFormatRGBA16Float;
      descriptor.inputWidth = configuration.inputWidth;
      descriptor.inputHeight = configuration.inputHeight;
      descriptor.outputWidth = configuration.outputWidth;
      descriptor.outputHeight = configuration.outputHeight;
      descriptor.autoExposureEnabled = YES;
      descriptor.requiresSynchronousInitialization = NO;
      scaler = [descriptor newTemporalScalerWithDevice:device];
      supported = scaler != nil;
    }
  }

  bool encode(const Frame &frame, std::string *error)
  {
    if (scaler == nil)
      return fail(error, "MetalFX temporal upscaling is unavailable");

    id<MTLTexture> color = exportedTexture(rhi, frame.color);
    id<MTLTexture> depth = exportedTexture(rhi, frame.depth);
    id<MTLTexture> motion = exportedTexture(rhi, frame.motion);
    id<MTLTexture> output = exportedTexture(rhi, frame.output);
    if (color == nil || depth == nil || motion == nil || output == nil)
      return fail(error, "MoltenVK did not export a MetalFX upscaler texture");

    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    if (commandBuffer == nil)
      return fail(error, "MetalFX could not allocate an upscaler command buffer");

    scaler.colorTexture = color;
    scaler.depthTexture = depth;
    scaler.motionTexture = motion;
    scaler.outputTexture = output;
    scaler.inputContentWidth = configuration.inputWidth;
    scaler.inputContentHeight = configuration.inputHeight;
    scaler.jitterOffsetX = frame.jitterX;
    scaler.jitterOffsetY = frame.jitterY;
    // The engine stores current-UV minus previous-UV. MetalFX expects the
    // displacement back to the previous frame in input-pixel units.
    scaler.motionVectorScaleX = -static_cast<float>(configuration.inputWidth);
    scaler.motionVectorScaleY = -static_cast<float>(configuration.inputHeight);
    scaler.depthReversed = YES;
    scaler.reset = frame.historyValid ? NO : YES;
    [scaler encodeToCommandBuffer:commandBuffer];
    [commandBuffer commit];
    return true;
  }
};

MetalFXTemporalUpscalerPass::MetalFXTemporalUpscalerPass(
    backend::vulkan::VulkanRHI &rhi, const Configuration &configuration)
    : impl_(std::make_unique<Impl>(rhi, configuration))
{
}

MetalFXTemporalUpscalerPass::~MetalFXTemporalUpscalerPass() = default;

bool MetalFXTemporalUpscalerPass::available() const
{
  return impl_->supported;
}

bool MetalFXTemporalUpscalerPass::encode(
    const Frame &frame, std::string *error)
{
  return impl_->encode(frame, error);
}

void *MetalFXTemporalUpscalerPass::nativeScaler() const
{
  return (__bridge void *)impl_->scaler;
}

struct MetalFXFrameGenerationPass::Impl
{
  backend::vulkan::VulkanRHI &rhi;
  Configuration configuration{};
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLFXFrameInterpolator> interpolator = nil;
  bool supported = false;

  Impl(
      backend::vulkan::VulkanRHI &inRhi,
      const Configuration &inConfiguration,
      const MetalFXTemporalUpscalerPass *upscaler)
      : rhi(inRhi), configuration(inConfiguration)
  {
    device = (__bridge id<MTLDevice>)rhi.exportMetalDevice();
    queue = (__bridge id<MTLCommandQueue>)
        rhi.exportMetalCommandQueue(Queue::Graphics);
    if (device == nil || queue == nil)
      return;

    if (@available(macOS 26.0, *))
    {
      supported = [MTLFXFrameInterpolatorDescriptor supportsDevice:device];
      if (!supported)
        return;

      MTLFXFrameInterpolatorDescriptor *descriptor =
          [[MTLFXFrameInterpolatorDescriptor alloc] init];
      descriptor.colorTextureFormat = MTLPixelFormatRGBA16Float;
      descriptor.outputTextureFormat = MTLPixelFormatRGBA16Float;
      descriptor.depthTextureFormat = MTLPixelFormatDepth32Float;
      descriptor.motionTextureFormat = MTLPixelFormatRG16Float;
      descriptor.inputWidth = configuration.inputWidth;
      descriptor.inputHeight = configuration.inputHeight;
      descriptor.outputWidth = configuration.outputWidth;
      descriptor.outputHeight = configuration.outputHeight;
      if (upscaler != nullptr && upscaler->nativeScaler() != nullptr)
        descriptor.scaler =
            (__bridge id<MTLFXTemporalScaler>)upscaler->nativeScaler();
      interpolator = [descriptor newFrameInterpolatorWithDevice:device];
      supported = interpolator != nil;
    }
  }

  bool encode(const Frame &frame, std::string *error)
  {
    if (@available(macOS 26.0, *))
    {
      if (interpolator == nil)
        return fail(error, "MetalFX frame interpolation is unavailable");

      id<MTLTexture> current = exportedTexture(rhi, frame.currentColor);
      id<MTLTexture> previous = frame.historyValid
          ? exportedTexture(rhi, frame.previousColor)
          : current;
      id<MTLTexture> depth = exportedTexture(rhi, frame.depth);
      id<MTLTexture> motion = exportedTexture(rhi, frame.motion);
      id<MTLTexture> output = exportedTexture(rhi, frame.output);
      if (current == nil || previous == nil || depth == nil || motion == nil ||
          output == nil)
        return fail(
            error, "MoltenVK did not export a MetalFX interpolation texture");

      id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
      if (commandBuffer == nil)
        return fail(
            error, "MetalFX could not allocate an interpolation command buffer");

      interpolator.colorTexture = current;
      interpolator.prevColorTexture = previous;
      interpolator.depthTexture = depth;
      interpolator.motionTexture = motion;
      interpolator.outputTexture = output;
      interpolator.motionVectorScaleX =
          -static_cast<float>(configuration.inputWidth);
      interpolator.motionVectorScaleY =
          -static_cast<float>(configuration.inputHeight);
      interpolator.deltaTime = std::max(frame.deltaSeconds, 1.0e-4f);
      interpolator.nearPlane = std::max(frame.nearPlane, 1.0e-4f);
      interpolator.farPlane =
          std::max(frame.farPlane, frame.nearPlane + 1.0f);
      interpolator.fieldOfView = frame.verticalFovDegrees;
      interpolator.aspectRatio =
          static_cast<float>(configuration.outputWidth) /
          static_cast<float>(configuration.outputHeight);
      interpolator.jitterOffsetX = frame.jitterX;
      interpolator.jitterOffsetY = frame.jitterY;
      interpolator.depthReversed = YES;
      interpolator.shouldResetHistory = frame.historyValid ? NO : YES;
      [interpolator encodeToCommandBuffer:commandBuffer];
      [commandBuffer commit];
      return true;
    }
    return fail(error, "MetalFX frame interpolation requires macOS 26");
  }
};

MetalFXFrameGenerationPass::MetalFXFrameGenerationPass(
    backend::vulkan::VulkanRHI &rhi, const Configuration &configuration,
    const MetalFXTemporalUpscalerPass *upscaler)
    : impl_(std::make_unique<Impl>(rhi, configuration, upscaler))
{
}

MetalFXFrameGenerationPass::~MetalFXFrameGenerationPass() = default;

bool MetalFXFrameGenerationPass::available() const
{
  return impl_->supported;
}

bool MetalFXFrameGenerationPass::encode(
    const Frame &frame, std::string *error)
{
  return impl_->encode(frame, error);
}

} // namespace rendering::gpgpu
