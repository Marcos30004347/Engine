#include "rendering/gpgpu/MetalFXPass.hpp"

#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include <algorithm>
#include <cstring>

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

id<MTLTexture> unitExposure(id<MTLDevice> device)
{
  auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Float
      width:1 height:1 mipmapped:NO];
  descriptor.storageMode = MTLStorageModeShared;
  descriptor.usage = MTLTextureUsageShaderRead;
  auto texture = [device newTextureWithDescriptor:descriptor];
  const uint16_t one = 0x3c00; // IEEE half 1.0: scene-linear radiance stays unchanged.
  [texture replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&one bytesPerRow:2];
  return texture;
}

bool fail(std::string *error, const char *message)
{
  if (error != nullptr)
    *error = message;
  return false;
}
} // namespace

bool readMetalFXOutput(backend::vulkan::VulkanRHI &rhi,TextureId textureId,
    std::vector<float> &rgba,std::string *error)
{
  auto texture=exportedTexture(rhi,textureId);
  if (!texture || texture.pixelFormat!=MTLPixelFormatRGBA16Float)
    return fail(error,"Native output readback requires an RGBA16Float texture");
  auto queue=(__bridge id<MTLCommandQueue>)rhi.exportMetalCommandQueue(Queue::Graphics);
  const NSUInteger stride=(texture.width*8+255)&~NSUInteger(255);
  auto buffer=[texture.device newBufferWithLength:stride*texture.height options:MTLResourceStorageModeShared];
  auto command=[queue commandBuffer]; auto blit=[command blitCommandEncoder];
  [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
      sourceSize:MTLSizeMake(texture.width,texture.height,1) toBuffer:buffer destinationOffset:0
      destinationBytesPerRow:stride destinationBytesPerImage:stride*texture.height];
  [blit endEncoding]; [command commit]; [command waitUntilCompleted];
  if (command.status!=MTLCommandBufferStatusCompleted)
    return fail(error,command.error.localizedDescription.UTF8String);
  rgba.resize(texture.width*texture.height*4);
  for (NSUInteger y=0;y<texture.height;++y) {
    auto row=reinterpret_cast<const _Float16 *>(static_cast<const char *>(buffer.contents)+y*stride);
    for (NSUInteger x=0;x<texture.width*4;++x) rgba[y*texture.width*4+x]=float(row[x]);
  }
  return true;
}

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
      descriptor.autoExposureEnabled = NO;
      descriptor.requiresSynchronousInitialization = NO;
      scaler = [descriptor newTemporalScalerWithDevice:device];
      supported = scaler != nil;
      scaler.exposureTexture = unitExposure(device);
      scaler.preExposure = 1.0f;
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

    if (frame.synchronization.sharedEvent)
      [commandBuffer encodeWaitForEvent:(__bridge id<MTLEvent>)frame.synchronization.sharedEvent value:frame.synchronization.readyValue];

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
    if (frame.synchronization.sharedEvent)
      [commandBuffer encodeSignalEvent:(__bridge id<MTLEvent>)frame.synchronization.sharedEvent value:frame.synchronization.doneValue];
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

struct MetalFXTemporalDenoisedScalerPass::Impl
{
  backend::vulkan::VulkanRHI &rhi;
  Configuration configuration;
  id<MTLCommandQueue> queue = nil;
  id<MTLFXTemporalDenoisedScaler> scaler = nil;
  id<MTLComputePipelineState> prepare = nil;
  id<MTLTexture> specular = nil, roughness = nil, normals = nil, mask = nil;
  id<MTLCommandBuffer> lastCommand = nil;

  Impl(backend::vulkan::VulkanRHI &inRhi, const Configuration &c)
      : rhi(inRhi), configuration(c)
  {
    if (@available(macOS 26.0, *))
    {
      auto device = (__bridge id<MTLDevice>)rhi.exportMetalDevice();
      if (!device || ![MTLFXTemporalDenoisedScalerDescriptor supportsDevice:device]) return;
      const float scale = float(c.outputWidth) / std::max(c.inputWidth,1u);
      if (scale < [MTLFXTemporalDenoisedScalerDescriptor supportedInputContentMinScaleForDevice:device] ||
          scale > [MTLFXTemporalDenoisedScalerDescriptor supportedInputContentMaxScaleForDevice:device]) return;
      queue = (__bridge id<MTLCommandQueue>)rhi.exportMetalCommandQueue(Queue::Graphics);
      if (!queue) return;
      auto descriptor = [[MTLFXTemporalDenoisedScalerDescriptor alloc] init];
      descriptor.inputWidth=c.inputWidth; descriptor.inputHeight=c.inputHeight;
      descriptor.outputWidth=c.outputWidth; descriptor.outputHeight=c.outputHeight;
      descriptor.colorTextureFormat=MTLPixelFormatRGBA16Float;
      descriptor.outputTextureFormat=MTLPixelFormatRGBA16Float;
      descriptor.depthTextureFormat=MTLPixelFormatDepth32Float;
      descriptor.motionTextureFormat=MTLPixelFormatRG16Float;
      descriptor.diffuseAlbedoTextureFormat=MTLPixelFormatRGBA16Float;
      descriptor.specularAlbedoTextureFormat=MTLPixelFormatRGBA16Float;
      descriptor.normalTextureFormat=MTLPixelFormatRGBA16Float;
      descriptor.roughnessTextureFormat=MTLPixelFormatR16Float;
      descriptor.denoiseStrengthMaskTextureFormat=MTLPixelFormatR16Float;
      descriptor.denoiseStrengthMaskTextureEnabled=YES;
      // ReSTIR currently supplies diffuse direct light, not reflected rays.
      descriptor.specularHitDistanceTextureEnabled=NO;
      descriptor.autoExposureEnabled=NO;
      descriptor.requiresSynchronousInitialization=YES;
      scaler=[descriptor newTemporalDenoisedScalerWithDevice:device];
      if (!scaler) return;
      // Exposure belongs to scene composition, not to reconstruction. The
      // bright sun disk entering the image must not rescale terrain lighting.
      scaler.exposureTexture = unitExposure(device);
      scaler.preExposure = 1.0f;
      const auto texture = [&](MTLPixelFormat format, MTLTextureUsage usage) {
        auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:c.inputWidth height:c.inputHeight mipmapped:NO];
        d.storageMode=MTLStorageModePrivate; d.usage=usage|MTLTextureUsageShaderWrite;
        return [device newTextureWithDescriptor:d];
      };
      specular=texture(MTLPixelFormatRGBA16Float,scaler.specularAlbedoTextureUsage);
      roughness=texture(MTLPixelFormatR16Float,scaler.roughnessTextureUsage);
      normals=texture(MTLPixelFormatRGBA16Float,scaler.normalTextureUsage);
      mask=texture(MTLPixelFormatR16Float,scaler.denoiseStrengthMaskTextureUsage);
      // The current direct-lighting BRDF is diffuse. Supply zero specular and
      // unit roughness, normalized world normals, and bypass clean sky pixels.
      NSString *source=@R"(
        #include <metal_stdlib>
        using namespace metal;
        kernel void prepare(texture2d<float,access::read> n [[texture(0)]],
          depth2d<float,access::read> d [[texture(1)]],
          texture2d<float,access::write> ns [[texture(2)]],
          texture2d<float,access::write> s [[texture(3)]],
          texture2d<float,access::write> r [[texture(4)]],
          texture2d<float,access::write> m [[texture(5)]],uint2 p [[thread_position_in_grid]]) {
          if (p.x>=ns.get_width() || p.y>=ns.get_height()) return;
          float3 v=n.read(p).xyz;
          bool valid=all(isfinite(v)) && dot(v,v)>1e-12 && d.read(p)>0;
          ns.write(float4(valid ? normalize(v) : float3(0,0,1),1),p);
          s.write(float4(0),p); r.write(float4(1),p);
          m.write(float4(valid ? 0.0f : 1.0f),p);
        })";
      NSError *error=nil;
      auto library=[device newLibraryWithSource:source options:nil error:&error];
      if (library) prepare=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"prepare"] error:&error];
      if (!prepare || !specular || !roughness || !normals || !mask) scaler=nil;
    }
  }
  ~Impl() { if (lastCommand) [lastCommand waitUntilCompleted]; }
  bool encode(const Frame &f, std::string *error)
  {
    if (@available(macOS 26.0, *))
    {
      if (!scaler) return fail(error,"MetalFX temporal denoising is unavailable");
      if (lastCommand && lastCommand.status==MTLCommandBufferStatusError)
        return fail(error,lastCommand.error.localizedDescription.UTF8String);
      auto color=exportedTexture(rhi,f.color), depth=exportedTexture(rhi,f.depth);
      auto motion=exportedTexture(rhi,f.motion), output=exportedTexture(rhi,f.output);
      auto albedo=exportedTexture(rhi,f.diffuseAlbedo), normal=exportedTexture(rhi,f.normal);
      if (!color || !depth || !motion || !output || !albedo || !normal)
        return fail(error,"Could not export MetalFX denoising inputs");
      const auto compatible=[&](const char *name,id<MTLTexture> t, MTLPixelFormat format, MTLTextureUsage usage) {
        if (t.pixelFormat==format && (t.usage&usage)==usage) return true;
        if (error) *error=std::string("MetalFX ")+name+" format="+std::to_string(t.pixelFormat)+
            " expected="+std::to_string(format)+" usage="+std::to_string(t.usage)+" required="+std::to_string(usage);
        return false;
      };
      if (!compatible("color",color,scaler.colorTextureFormat,scaler.colorTextureUsage) ||
          !compatible("depth",depth,scaler.depthTextureFormat,scaler.depthTextureUsage) ||
          !compatible("motion",motion,scaler.motionTextureFormat,scaler.motionTextureUsage) ||
          !compatible("albedo",albedo,scaler.diffuseAlbedoTextureFormat,scaler.diffuseAlbedoTextureUsage) ||
          !compatible("output",output,scaler.outputTextureFormat,scaler.outputTextureUsage)) return false;
      if (output.storageMode!=MTLStorageModePrivate)
        return fail(error,"MetalFX denoising requires a private output texture");
      auto command=[queue commandBuffer];
      if (!command) return fail(error,"MetalFX could not allocate a denoiser command buffer");
      command.label=@"ReSTIR MetalFX Temporal Denoised Scaler";
      if (f.synchronization.sharedEvent)
        [command encodeWaitForEvent:(__bridge id<MTLEvent>)f.synchronization.sharedEvent value:f.synchronization.readyValue];
      auto encoder=[command computeCommandEncoder];
      [encoder setComputePipelineState:prepare];
      id<MTLTexture> textures[]={normal,depth,normals,specular,roughness,mask};
      for (uint32_t i=0;i<6;++i) [encoder setTexture:textures[i] atIndex:i];
      [encoder dispatchThreads:MTLSizeMake(configuration.inputWidth,configuration.inputHeight,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];
      [encoder endEncoding];
      scaler.colorTexture=color; scaler.depthTexture=depth; scaler.motionTexture=motion;
      scaler.outputTexture=output; scaler.diffuseAlbedoTexture=albedo;
      scaler.specularAlbedoTexture=specular; scaler.normalTexture=normals;
      scaler.roughnessTexture=roughness; scaler.denoiseStrengthMaskTexture=mask;
      scaler.jitterOffsetX=f.jitterX; scaler.jitterOffsetY=f.jitterY;
      scaler.motionVectorScaleX=-float(configuration.inputWidth);
      scaler.motionVectorScaleY=-float(configuration.inputHeight);
      scaler.depthReversed=YES; scaler.shouldResetHistory=!f.historyValid;
      simd_float4x4 view, projection;
      std::memcpy(&view,f.worldToView.data(),sizeof(view));
      std::memcpy(&projection,f.viewToClip.data(),sizeof(projection));
      scaler.worldToViewMatrix=view; scaler.viewToClipMatrix=projection;
      [scaler encodeToCommandBuffer:command];
      if (f.synchronization.sharedEvent)
        [command encodeSignalEvent:(__bridge id<MTLEvent>)f.synchronization.sharedEvent value:f.synchronization.doneValue];
      [command commit]; lastCommand=command;
      return true;
    }
    return fail(error,"MetalFX temporal denoising requires macOS 26");
  }
};
MetalFXTemporalDenoisedScalerPass::MetalFXTemporalDenoisedScalerPass(backend::vulkan::VulkanRHI &rhi,const Configuration &c)
    :impl_(std::make_unique<Impl>(rhi,c)) {}
MetalFXTemporalDenoisedScalerPass::~MetalFXTemporalDenoisedScalerPass()=default;
bool MetalFXTemporalDenoisedScalerPass::available() const { return impl_->scaler!=nil; }
bool MetalFXTemporalDenoisedScalerPass::encode(const Frame &f,std::string *error) { return impl_->encode(f,error); }
void *MetalFXTemporalDenoisedScalerPass::nativeScaler() const { return (__bridge void *)impl_->scaler; }

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
      const MetalFXTemporalUpscalerPass *upscaler,
      const MetalFXTemporalDenoisedScalerPass *denoiser)
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
      if (denoiser != nullptr && denoiser->nativeScaler() != nullptr)
        descriptor.scaler=(__bridge id<MTLFXTemporalDenoisedScaler>)denoiser->nativeScaler();
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

      if (frame.synchronization.sharedEvent)
        [commandBuffer encodeWaitForEvent:(__bridge id<MTLEvent>)frame.synchronization.sharedEvent value:frame.synchronization.readyValue];
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
      if (frame.synchronization.sharedEvent)
        [commandBuffer encodeSignalEvent:(__bridge id<MTLEvent>)frame.synchronization.sharedEvent value:frame.synchronization.doneValue];
      [commandBuffer commit];
      return true;
    }
    return fail(error, "MetalFX frame interpolation requires macOS 26");
  }
};

MetalFXFrameGenerationPass::MetalFXFrameGenerationPass(
    backend::vulkan::VulkanRHI &rhi, const Configuration &configuration,
    const MetalFXTemporalUpscalerPass *upscaler,
    const MetalFXTemporalDenoisedScalerPass *denoiser)
    : impl_(std::make_unique<Impl>(rhi, configuration, upscaler, denoiser))
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
