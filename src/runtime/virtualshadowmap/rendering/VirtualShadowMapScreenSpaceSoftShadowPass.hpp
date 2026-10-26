#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualshadowmap/SoftShadowSettings.hpp"
#include "virtualshadowmap/VirtualShadowMapManager.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace virtualgeometry
{
namespace gpgpu
{

class VirtualShadowMapScreenSpaceSoftShadowPass : public rendering::Pass
{
public:
  struct Uniforms
  {
    uint32_t viewport[2] = {1u, 1u};
    uint32_t inputViewport[2] = {1u, 1u};
    uint32_t layerCount = 1u;
    uint32_t activeLights = 0u;
    uint32_t resolutionScale = 1u;
    uint32_t kernelSize = 11u;
    uint32_t blockerSearchTaps = 3u;
    uint32_t useMinFilter = 0u;
    uint32_t translucentShadowsEnabled = 0u;
    float defaultLightRadiusWorld = 0.5f;
    float penumbraScale = 1.0f;
    float grazingThreshold = 0.25f;
    float depthRejectThreshold = 0.03f;
    uint32_t debugMode = 4u;
    uint32_t debugLayer = 0u;
    float debugValueScale = 1.0f;
    uint32_t _padding[2] = {0u, 0u};
    uint32_t pageTableResolution = 0u;
    uint32_t activeLayers = 0u;
    uint32_t cascadeCount = 0u;
    uint32_t physicalPageSize = 0u;
    uint32_t reverseZ = 0u;
    uint32_t fallbackCascadeOffset = 0u;
    float firstCascadeWorldExtent = 0.0f;
    float localLightMipTransitionScale = 2.0f;
    float localLightMipBias = 0.0f;
    float localLightPageWorldScale = 1.0f;
    float directionalShadowBias = 0.0015f;
    float spotShadowBias = 0.0015f;
    float pointShadowBias = 0.0000005f;
    uint32_t _vsmPadding[3] = {0u, 0u, 0u};
  };
  static_assert(sizeof(Uniforms) == 144u, "SSSS uniforms must stay aligned to 16 bytes");

  struct Settings : ScreenSpaceSoftShadowSettings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    float directionalShadowBias = 0.0015f;
    float spotShadowBias = 0.0015f;
    float pointShadowBias = 0.0000005f;
    float localLightMipTransitionScale = 2.0f;
    float localLightMipBias = 0.0f;
  };

  VirtualShadowMapScreenSpaceSoftShadowPass(VirtualShadowMapManager &manager, rendering::Texture depthTexture, rendering::Texture normalTexture, rendering::Texture debugTexture, Settings settings)
      : manager_(manager), depthTexture_(depthTexture), normalTexture_(normalTexture), debugTexture_(debugTexture), settings_(settings)
  {
  }

  ~VirtualShadowMapScreenSpaceSoftShadowPass() override
  {
    destroy();
  }

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    if (uniformBuffer_.isValid() && outputWidth_ > 0u && outputHeight_ > 0u && arrayLayerCount_ > 0u)
    {
      writeUniforms();
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    createResources();
    createSharedResources();
    createBindings();
    createPipelines();
    writeUniforms();

    const uint32_t fullDispatchX = std::max(1u, (fullWidth_ + 7u) / 8u);
    const uint32_t fullDispatchY = std::max(1u, (fullHeight_ + 7u) / 8u);
    const uint32_t dispatchX = std::max(1u, (outputWidth_ + 7u) / 8u);
    const uint32_t dispatchY = std::max(1u, (outputHeight_ + 7u) / 8u);
    const uint32_t dispatchZ = std::max(1u, arrayLayerCount_);

    commandBuffer.cmdBindComputePipeline(penumbraPipeline_);
    commandBuffer.cmdBindBindingGroups(penumbraBindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(fullDispatchX, fullDispatchY, dispatchZ);

    commandBuffer.cmdBindComputePipeline(horizontalBlurPipeline_);
    commandBuffer.cmdBindBindingGroups(horizontalBlurBindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(dispatchX, dispatchY, dispatchZ);

    commandBuffer.cmdBindComputePipeline(verticalBlurPipeline_);
    commandBuffer.cmdBindBindingGroups(verticalBlurBindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(dispatchX, dispatchY, dispatchZ);

    commandBuffer.cmdBindComputePipeline(debugPipeline_);
    commandBuffer.cmdBindBindingGroups(debugBindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(fullDispatchX, fullDispatchY, 1u);
  }

  const rendering::Texture &getHardShadowTexture() const
  {
    return hardShadowTexture_;
  }

  const rendering::Texture &getPenumbraTexture() const
  {
    return penumbraTexture_;
  }

  const rendering::Texture &getTranslucencyTexture() const
  {
    return translucencyTexture_;
  }

  const rendering::Texture &getBlurPingTexture() const
  {
    return blurPingTexture_;
  }

  const rendering::Texture &getBlurredVisibilityTexture() const
  {
    return blurredVisibilityTexture_;
  }

  const rendering::Texture &getDebugTexture() const
  {
    return debugTexture_;
  }

  uint32_t getOutputWidth() const
  {
    return outputWidth_;
  }

  uint32_t getOutputHeight() const
  {
    return outputHeight_;
  }

  uint32_t getArrayLayerCount() const
  {
    return arrayLayerCount_;
  }

private:
  static const std::string &getCachedPenumbraSpirv()
  {
    static const std::string spirv = os::io::readRelativeFile("assets/shaders/spirv/vsm-ssss-penumbra.spirv");
    return spirv;
  }

  static const std::string &getCachedHorizontalBlurSpirv()
  {
    static const std::string spirv = os::io::readRelativeFile("assets/shaders/spirv/vsm-ssss-blur-horizontal.spirv");
    return spirv;
  }

  static const std::string &getCachedVerticalBlurSpirv()
  {
    static const std::string spirv = os::io::readRelativeFile("assets/shaders/spirv/vsm-ssss-blur-vertical.spirv");
    return spirv;
  }

  static const std::string &getCachedDebugSpirv()
  {
    static const std::string spirv = os::io::readRelativeFile("assets/shaders/spirv/vsm-ssss-debug.spirv");
    return spirv;
  }

  static rendering::TextureView makeSampledDepthView(rendering::Texture texture)
  {
    return rendering::TextureView{
      .texture = texture,
      .access = rendering::AccessPattern::SHADER_READ,
      .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
      .index = 0,
      .flags = rendering::ImageAspectFlags::Depth,
      .baseArrayLayer = 0,
      .baseMipLevel = 0,
      .layerCount = 1,
      .levelCount = 1,
    };
  }

  static rendering::TextureView makeSampledColorView(rendering::Texture texture, uint32_t layerCount = 1u, bool forceArrayView = false)
  {
    return rendering::TextureView{
      .texture = texture,
      .access = rendering::AccessPattern::SHADER_READ,
      .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
      .index = 0,
      .flags = rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0,
      .baseMipLevel = 0,
      .layerCount = layerCount,
      .levelCount = 1,
      .forceArrayView = forceArrayView,
    };
  }

  static rendering::TextureView makeStorageColorView(rendering::Texture texture, uint32_t layerCount = 1u)
  {
    return rendering::TextureView{
      .texture = texture,
      .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
      .layout = rendering::ResourceLayout::GENERAL,
      .index = 0,
      .flags = rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0,
      .baseMipLevel = 0,
      .layerCount = layerCount,
      .levelCount = 1,
      .forceArrayView = true,
    };
  }

  static rendering::TextureView makeStorageColor2DView(rendering::Texture texture)
  {
    return rendering::TextureView{
      .texture = texture,
      .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
      .layout = rendering::ResourceLayout::GENERAL,
      .index = 0,
      .flags = rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0,
      .baseMipLevel = 0,
      .layerCount = 1,
      .levelCount = 1,
      .forceArrayView = false,
    };
  }

  void destroy()
  {
    if (renderGraph == nullptr)
    {
      return;
    }
    auto deleteBindingGroupsIfValid =
        [&](const rendering::BindingGroups &bindingGroups)
    {
      if (bindingGroups.isValid())
      {
        renderGraph->deleteBindingGroups(bindingGroups);
      }
    };
    auto deleteComputePipelineIfValid =
        [&](const rendering::ComputePipeline &pipeline)
    {
      if (pipeline.isValid())
      {
        renderGraph->deleteComputePipeline(pipeline);
      }
    };
    auto deleteShaderIfValid =
        [&](const rendering::Shader &shader)
    {
      if (shader.isValid())
      {
        renderGraph->deleteShader(shader);
      }
    };
    auto deleteBindingsLayoutIfValid =
        [&](const rendering::BindingsLayout &layout)
    {
      if (layout.isValid())
      {
        renderGraph->deleteBindingsLayout(layout);
      }
    };
    auto deleteBufferIfValid =
        [&](const rendering::Buffer &buffer)
    {
      if (buffer.isValid())
      {
        renderGraph->deleteBuffer(buffer);
      }
    };

    deleteBindingGroupsIfValid(penumbraBindingGroups_);
    deleteBindingGroupsIfValid(horizontalBlurBindingGroups_);
    deleteBindingGroupsIfValid(verticalBlurBindingGroups_);
    deleteBindingGroupsIfValid(debugBindingGroups_);
    penumbraBindingGroups_ = {};
    horizontalBlurBindingGroups_ = {};
    verticalBlurBindingGroups_ = {};
    debugBindingGroups_ = {};
    destroyResources();
    deleteComputePipelineIfValid(penumbraPipeline_);
    deleteComputePipelineIfValid(horizontalBlurPipeline_);
    deleteComputePipelineIfValid(verticalBlurPipeline_);
    deleteComputePipelineIfValid(debugPipeline_);
    deleteShaderIfValid(penumbraShader_);
    deleteShaderIfValid(horizontalBlurShader_);
    deleteShaderIfValid(verticalBlurShader_);
    deleteShaderIfValid(debugShader_);
    deleteBindingsLayoutIfValid(penumbraLayout_);
    deleteBindingsLayoutIfValid(blurLayout_);
    deleteBindingsLayoutIfValid(debugLayout_);
    deleteBufferIfValid(uniformBuffer_);
    penumbraPipeline_ = {};
    horizontalBlurPipeline_ = {};
    verticalBlurPipeline_ = {};
    debugPipeline_ = {};
    penumbraShader_ = {};
    horizontalBlurShader_ = {};
    verticalBlurShader_ = {};
    debugShader_ = {};
    penumbraBindingGroups_ = {};
    horizontalBlurBindingGroups_ = {};
    verticalBlurBindingGroups_ = {};
    debugBindingGroups_ = {};
    penumbraLayout_ = {};
    blurLayout_ = {};
    debugLayout_ = {};
    uniformBuffer_ = {};
  }

  void destroyResources()
  {
    if (renderGraph == nullptr)
    {
      return;
    }
    auto deleteTextureIfValid =
        [&](const rendering::Texture &texture)
    {
      if (texture.isValid())
      {
        renderGraph->deleteTexture(texture);
      }
    };
    deleteTextureIfValid(hardShadowTexture_);
    deleteTextureIfValid(penumbraTexture_);
    deleteTextureIfValid(translucencyTexture_);
    deleteTextureIfValid(blurPingTexture_);
    deleteTextureIfValid(blurredVisibilityTexture_);
    hardShadowTexture_ = {};
    penumbraTexture_ = {};
    translucencyTexture_ = {};
    blurPingTexture_ = {};
    blurredVisibilityTexture_ = {};
    fullWidth_ = 0u;
    fullHeight_ = 0u;
    outputWidth_ = 0u;
    outputHeight_ = 0u;
    arrayLayerCount_ = 0u;
  }

  rendering::Texture createLayeredTexture(const char *suffix, rendering::Format format, uint32_t width, uint32_t height) const
  {
    return renderGraph->createTexture(
        rendering::TextureInfo{
            .name = passName + suffix,
            .format = format,
            .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
            .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
            .width = width,
            .height = height,
            .depth = 1u,
            .arrayLayers = arrayLayerCount_,
            .mipLevels = 1u,
        });
  }

  void createResources()
  {
    const uint32_t nextFullWidth = std::max(settings_.width, 1u);
    const uint32_t nextFullHeight = std::max(settings_.height, 1u);
    const uint32_t nextWidth = scaledScreenSpaceSoftShadowExtent(settings_.width, settings_.resolutionScale);
    const uint32_t nextHeight = scaledScreenSpaceSoftShadowExtent(settings_.height, settings_.resolutionScale);
    const uint32_t nextLayerCount = std::max(1u, manager_.getMaxSoftShadowLayerCount());
    if (hardShadowTexture_.isValid() && fullWidth_ == nextFullWidth && fullHeight_ == nextFullHeight && outputWidth_ == nextWidth && outputHeight_ == nextHeight &&
        arrayLayerCount_ == nextLayerCount)
    {
      return;
    }

    destroyResources();
    fullWidth_ = nextFullWidth;
    fullHeight_ = nextFullHeight;
    outputWidth_ = nextWidth;
    outputHeight_ = nextHeight;
    arrayLayerCount_ = nextLayerCount;
    hardShadowTexture_ = createLayeredTexture(".HardShadow.texture", rendering::Format::Format_R16Float, fullWidth_, fullHeight_);
    penumbraTexture_ = createLayeredTexture(".Penumbra.texture", rendering::Format::Format_R16Float, fullWidth_, fullHeight_);
    translucencyTexture_ = createLayeredTexture(".Translucency.texture", rendering::Format::Format_RGBA16Float, fullWidth_, fullHeight_);
    blurPingTexture_ = createLayeredTexture(".BlurPing.texture", rendering::Format::Format_RGBA16Float, outputWidth_, outputHeight_);
    blurredVisibilityTexture_ = createLayeredTexture(".BlurredVisibility.texture", rendering::Format::Format_RGBA16Float, outputWidth_, outputHeight_);
  }

  void createSharedResources()
  {
    if (!uniformBuffer_.isValid())
    {
      uniformBuffer_ = createFrameLocalBuffer(
          rendering::BufferInfo{
              .name = passName + "_Uniforms.buffer",
              .size = sizeof(Uniforms),
              .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
          });
    }
    if (!penumbraLayout_.isValid())
    {
      penumbraLayout_ = renderGraph->createBindingsLayout(
          rendering::BindingsLayoutInfo{
              .name = passName + "_Penumbra.layout",
              .groups = {rendering::BindingGroupLayout{
                  .buffers =
                      {
                          {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "softShadowLights", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "cascadeStates", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "cascadeMatrices", .binding = 8, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "cameraState", .binding = 9, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "directionalLights", .binding = 10, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "virtualPageTable", .binding = 11, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      },
                  .textures =
                      {
                          {.name = "depthTexture", .binding = 1, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "normalTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "shadowAtlasTexture", .binding = 12, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      },
                  .storageTextures =
                      {
                          {.name = "hardShadowTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "penumbraTexture", .binding = 5, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "translucencyTexture", .binding = 6, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      },
              }},
          });
    }
    if (!blurLayout_.isValid())
    {
      blurLayout_ = renderGraph->createBindingsLayout(
          rendering::BindingsLayoutInfo{
              .name = passName + "_Blur.layout",
              .groups = {rendering::BindingGroupLayout{
                  .buffers =
                      {
                          {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "cameraState", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      },
                  .textures =
                      {
                          {.name = "depthTexture", .binding = 1, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "normalTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "sourceTexture", .binding = 3, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "penumbraTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      },
                  .storageTextures =
                      {
                          {.name = "outputTexture", .binding = 5, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      },
              }},
          });
    }
    if (!debugLayout_.isValid())
    {
      debugLayout_ = renderGraph->createBindingsLayout(
          rendering::BindingsLayoutInfo{
              .name = passName + "_Debug.layout",
              .groups = {rendering::BindingGroupLayout{
                  .buffers =
                      {
                          {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      },
                  .textures =
                      {
                          {.name = "hardShadowTexture", .binding = 1, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "penumbraTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "translucencyTexture", .binding = 3, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "horizontalBlurTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                          {.name = "blurredVisibilityTexture", .binding = 5, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      },
                  .storageTextures =
                      {
                          {.name = "outputTexture", .binding = 6, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      },
              }},
          });
    }
  }

  void createBindings()
  {
    if (!penumbraBindingGroups_.isValid())
    {
      penumbraBindingGroups_ = renderGraph->createBindingGroups(
          rendering::BindingGroupsInfo{
              .layout = penumbraLayout_,
              .name = passName + "_Penumbra.bindings",
              .groups = {rendering::GroupInfo{
                  .name = "group0",
                  .buffers =
                      {
                          {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .offset = 0, .size = sizeof(Uniforms), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 3, .bufferView = {.buffer = manager_.getSoftShadowLightsBuffer(), .offset = 0, .size = manager_.getSoftShadowLightsBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 7, .bufferView = {.buffer = manager_.getCascadeStatesBuffer(), .offset = 0, .size = manager_.getCascadeStatesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 8, .bufferView = {.buffer = manager_.getCascadeMatricesBuffer(), .offset = 0, .size = manager_.getCascadeMatricesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 9, .bufferView = {.buffer = manager_.getCameraStateBuffer(), .offset = 0, .size = manager_.getCameraStateBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 10, .bufferView = {.buffer = manager_.getDirectionalLightsBuffer(), .offset = 0, .size = manager_.getDirectionalLightsBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                          {.binding = 11, .bufferView = {.buffer = manager_.getVirtualPageTableBuffer(), .offset = 0, .size = manager_.getVirtualPageTableBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                      },
                  .textures =
                      {
                          {.binding = 1, .textureView = makeSampledDepthView(depthTexture_)},
                          {.binding = 2, .textureView = makeSampledColorView(normalTexture_)},
                          {.binding = 12, .textureView = makeSampledDepthView(manager_.getShadowAtlasTexture())},
                      },
                  .storageTextures =
                      {
                          {.binding = 4, .textureView = makeStorageColorView(hardShadowTexture_, arrayLayerCount_)},
                          {.binding = 5, .textureView = makeStorageColorView(penumbraTexture_, arrayLayerCount_)},
                          {.binding = 6, .textureView = makeStorageColorView(translucencyTexture_, arrayLayerCount_)},
                      },
              }},
          });
    }
    if (!horizontalBlurBindingGroups_.isValid())
    {
      horizontalBlurBindingGroups_ = createBlurBindingGroups("_HorizontalBlur.bindings", hardShadowTexture_, blurPingTexture_);
    }
    if (!verticalBlurBindingGroups_.isValid())
    {
      verticalBlurBindingGroups_ = createBlurBindingGroups("_VerticalBlur.bindings", blurPingTexture_, blurredVisibilityTexture_);
    }
    if (!debugBindingGroups_.isValid())
    {
      debugBindingGroups_ = renderGraph->createBindingGroups(
          rendering::BindingGroupsInfo{
              .layout = debugLayout_,
              .name = passName + "_Debug.bindings",
              .groups = {rendering::GroupInfo{
                  .name = "group0",
                  .buffers =
                      {
                          {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .offset = 0, .size = sizeof(Uniforms), .access = rendering::AccessPattern::SHADER_READ}},
                      },
                  .textures =
                      {
                          {.binding = 1, .textureView = makeSampledColorView(hardShadowTexture_, arrayLayerCount_, true)},
                          {.binding = 2, .textureView = makeSampledColorView(penumbraTexture_, arrayLayerCount_, true)},
                          {.binding = 3, .textureView = makeSampledColorView(translucencyTexture_, arrayLayerCount_, true)},
                          {.binding = 4, .textureView = makeSampledColorView(blurPingTexture_, arrayLayerCount_, true)},
                          {.binding = 5, .textureView = makeSampledColorView(blurredVisibilityTexture_, arrayLayerCount_, true)},
                      },
                  .storageTextures =
                      {
                          {.binding = 6, .textureView = makeStorageColor2DView(debugTexture_)},
                      },
              }},
          });
    }
  }

  rendering::BindingGroups createBlurBindingGroups(const char *nameSuffix, rendering::Texture sourceTexture, rendering::Texture outputTexture)
  {
    return renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = blurLayout_,
            .name = passName + nameSuffix,
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers =
                    {
                        {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .offset = 0, .size = sizeof(Uniforms), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 6, .bufferView = {.buffer = manager_.getCameraStateBuffer(), .offset = 0, .size = manager_.getCameraStateBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                    },
                .textures =
                    {
                        {.binding = 1, .textureView = makeSampledDepthView(depthTexture_)},
                        {.binding = 2, .textureView = makeSampledColorView(normalTexture_)},
                        {.binding = 3, .textureView = makeSampledColorView(sourceTexture, arrayLayerCount_, true)},
                        {.binding = 4, .textureView = makeSampledColorView(penumbraTexture_, arrayLayerCount_, true)},
                    },
                .storageTextures =
                    {
                        {.binding = 5, .textureView = makeStorageColorView(outputTexture, arrayLayerCount_)},
                    },
            }},
        });
  }

  void createPipelines()
  {
    if (!penumbraShader_.isValid())
    {
      penumbraShader_ = renderGraph->createShader(rendering::ShaderInfo{.name = passName + "_Penumbra.shader", .layout = penumbraLayout_, .src = getCachedPenumbraSpirv(), .type = rendering::ShaderType::SpirV});
      horizontalBlurShader_ = renderGraph->createShader(rendering::ShaderInfo{.name = passName + "_HorizontalBlur.shader", .layout = blurLayout_, .src = getCachedHorizontalBlurSpirv(), .type = rendering::ShaderType::SpirV});
      verticalBlurShader_ = renderGraph->createShader(rendering::ShaderInfo{.name = passName + "_VerticalBlur.shader", .layout = blurLayout_, .src = getCachedVerticalBlurSpirv(), .type = rendering::ShaderType::SpirV});
      debugShader_ = renderGraph->createShader(rendering::ShaderInfo{.name = passName + "_Debug.shader", .layout = debugLayout_, .src = getCachedDebugSpirv(), .type = rendering::ShaderType::SpirV});
      penumbraPipeline_ = renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry = "main", .layout = penumbraLayout_, .name = passName + "_Penumbra.pipeline", .shader = penumbraShader_});
      horizontalBlurPipeline_ = renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry = "main", .layout = blurLayout_, .name = passName + "_HorizontalBlur.pipeline", .shader = horizontalBlurShader_});
      verticalBlurPipeline_ = renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry = "main", .layout = blurLayout_, .name = passName + "_VerticalBlur.pipeline", .shader = verticalBlurShader_});
      debugPipeline_ = renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry = "main", .layout = debugLayout_, .name = passName + "_Debug.pipeline", .shader = debugShader_});
    }
  }

  void writeUniforms()
  {
    Uniforms uniforms{};
    uniforms.viewport[0] = outputWidth_;
    uniforms.viewport[1] = outputHeight_;
    uniforms.inputViewport[0] = settings_.width;
    uniforms.inputViewport[1] = settings_.height;
    uniforms.layerCount = arrayLayerCount_;
    uniforms.activeLights = manager_.getActiveLightCount();
    uniforms.resolutionScale = sanitizeScreenSpaceSoftShadowResolutionScale(settings_.resolutionScale);
    uniforms.kernelSize = sanitizeScreenSpaceSoftShadowKernelSize(settings_.kernelSize);
    uniforms.blockerSearchTaps = sanitizeScreenSpaceSoftShadowBlockerSearchTaps(settings_.blockerSearchTaps);
    uniforms.useMinFilter = settings_.useMinFilter ? 1u : 0u;
    uniforms.translucentShadowsEnabled = settings_.translucentShadowsEnabled ? 1u : 0u;
    uniforms.defaultLightRadiusWorld = std::max(settings_.defaultLightRadiusWorld, 0.0f);
    uniforms.penumbraScale = std::max(settings_.penumbraScale, 0.0f);
    uniforms.grazingThreshold = std::clamp(settings_.grazingThreshold, 0.0f, 1.0f);
    uniforms.depthRejectThreshold = std::max(settings_.depthRejectThreshold, 0.0f);
    uniforms.debugMode = static_cast<uint32_t>(settings_.debugMode);
    uniforms.debugLayer = std::min(settings_.debugLayer, std::max(arrayLayerCount_, 1u) - 1u);
    uniforms.debugValueScale = std::max(settings_.debugValueScale, 0.0f);
    uniforms.pageTableResolution = manager_.getVirtualPageTableResolution();
    uniforms.activeLayers = manager_.getActiveLayerCount();
    uniforms.cascadeCount = manager_.getCascadeCount();
    uniforms.physicalPageSize = manager_.getPhysicalPageResolution();
    uniforms.reverseZ = manager_.getSettings().reverseZ ? 1u : 0u;
    uniforms.fallbackCascadeOffset = manager_.getFallbackCascadeOffset();
    uniforms.firstCascadeWorldExtent = manager_.getFirstCascadeWorldExtent();
    uniforms.localLightMipTransitionScale = std::max(settings_.localLightMipTransitionScale, 1e-4f);
    uniforms.localLightMipBias = settings_.localLightMipBias;
    uniforms.localLightPageWorldScale = std::max(manager_.getSettings().pageWorldScale, 1e-4f);
    uniforms.directionalShadowBias = settings_.directionalShadowBias;
    uniforms.spotShadowBias = settings_.spotShadowBias;
    uniforms.pointShadowBias = settings_.pointShadowBias;
    renderGraph->bufferWrite(uniformBuffer_, 0, sizeof(uniforms), &uniforms);
  }

  VirtualShadowMapManager &manager_;
  rendering::Texture depthTexture_;
  rendering::Texture normalTexture_;
  Settings settings_;
  rendering::Texture hardShadowTexture_;
  rendering::Texture penumbraTexture_;
  rendering::Texture translucencyTexture_;
  rendering::Texture blurPingTexture_;
  rendering::Texture blurredVisibilityTexture_;
  rendering::Texture debugTexture_;
  rendering::Buffer uniformBuffer_;
  rendering::BindingsLayout penumbraLayout_;
  rendering::BindingsLayout blurLayout_;
  rendering::BindingsLayout debugLayout_;
  rendering::BindingGroups penumbraBindingGroups_;
  rendering::BindingGroups horizontalBlurBindingGroups_;
  rendering::BindingGroups verticalBlurBindingGroups_;
  rendering::BindingGroups debugBindingGroups_;
  rendering::Shader penumbraShader_;
  rendering::Shader horizontalBlurShader_;
  rendering::Shader verticalBlurShader_;
  rendering::Shader debugShader_;
  rendering::ComputePipeline penumbraPipeline_;
  rendering::ComputePipeline horizontalBlurPipeline_;
  rendering::ComputePipeline verticalBlurPipeline_;
  rendering::ComputePipeline debugPipeline_;
  uint32_t fullWidth_ = 0u;
  uint32_t fullHeight_ = 0u;
  uint32_t outputWidth_ = 0u;
  uint32_t outputHeight_ = 0u;
  uint32_t arrayLayerCount_ = 0u;
};

} // namespace gpgpu
} // namespace virtualgeometry
