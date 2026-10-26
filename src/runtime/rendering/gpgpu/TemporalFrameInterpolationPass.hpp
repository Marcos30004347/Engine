#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace rendering::gpgpu
{

// Engine-native frame generation with the same major data flow as FidelityFX
// Frame Interpolation: ping-ponged luminance pyramids, coarse-to-fine optical
// flow, game-motion and color-inpainting pyramids, game/flow arbitration,
// two-sided validity, and late UI composition by the presenter. It is
// intentionally not branded as AMD SDK code; the shaders are WGSL
// implementations owned here.
class TemporalFrameInterpolationPass final : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t outputWidth = 1u;
    uint32_t outputHeight = 1u;
    uint32_t motionWidth = 1u;
    uint32_t motionHeight = 1u;
    bool historyValid = false;
    float interpolationPoint = 0.5f;
    float depthThreshold = 0.02f;
    float maximumMotionPixels = 96.0f;
    float clampGamma = 1.25f;
    uint32_t debugMode = 0u;
  };

  TemporalFrameInterpolationPass(
      rendering::Texture previousColor,
      rendering::Texture currentColor,
      rendering::Texture previousDepth,
      rendering::Texture currentDepth,
      rendering::Texture currentMotion,
      rendering::Texture disocclusion,
      rendering::Texture output,
      Settings settings)
      : previousColor_(previousColor), currentColor_(currentColor),
        previousDepth_(previousDepth), currentDepth_(currentDepth),
        currentMotion_(currentMotion), disocclusion_(disocclusion),
        output_(output), settings_(settings)
  {
  }

  ~TemporalFrameInterpolationPass() override { destroy(); }

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    if (resourcesCreated_)
      writeDynamicUniforms();
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    createResources();

    for (uint32_t level = 0u; level < kPyramidLevels; ++level)
    {
      dispatch(commandBuffer, lumaPipeline_, previousLumaGroups_[level],
               pyramidWidths_[level], pyramidHeights_[level]);
      dispatch(commandBuffer, lumaPipeline_, currentLumaGroups_[level],
               pyramidWidths_[level], pyramidHeights_[level]);
      dispatch(commandBuffer, motionPyramidPipeline_,
               motionPyramidGroups_[level], pyramidWidths_[level],
               pyramidHeights_[level]);
      computeBarrier(commandBuffer);
    }

    // Flow textures are stored finest-to-coarsest, so dispatch in reverse.
    for (uint32_t reverseLevel = 0u; reverseLevel < kFlowLevels;
         ++reverseLevel)
    {
      const uint32_t level = kFlowLevels - 1u - reverseLevel;
      dispatch(commandBuffer, flowPipeline_, flowGroups_[level],
               flowWidths_[level], flowHeights_[level]);
      computeBarrier(commandBuffer);
    }

    dispatch(commandBuffer, interpolationPipeline_, interpolationGroups_,
             settings_.outputWidth, settings_.outputHeight);
    computeBarrier(commandBuffer);

    for (uint32_t level = 0u; level < kPyramidLevels; ++level)
    {
      dispatch(commandBuffer, inpaintPyramidPipeline_,
               inpaintPyramidGroups_[level], pyramidWidths_[level],
               pyramidHeights_[level]);
      computeBarrier(commandBuffer);
    }

    dispatch(commandBuffer, inpaintPipeline_, inpaintGroups_,
             settings_.outputWidth, settings_.outputHeight);
  }

private:
  static constexpr uint32_t kPyramidLevels = 4u;
  static constexpr uint32_t kFlowLevels = 3u;

  struct PyramidUniforms
  {
    std::array<uint32_t, 4u> extent{};
    std::array<uint32_t, 4u> flags{};
  };
  static_assert(sizeof(PyramidUniforms) == 32u);

  struct FlowUniforms
  {
    std::array<uint32_t, 4u> extent{};
    std::array<float, 4u> controls{};
    std::array<uint32_t, 4u> flags{};
  };
  static_assert(sizeof(FlowUniforms) == 48u);

  struct MotionPyramidUniforms
  {
    std::array<uint32_t, 4u> extent{};
    std::array<float, 4u> controls{};
    std::array<uint32_t, 4u> flags{};
  };
  static_assert(sizeof(MotionPyramidUniforms) == 48u);

  struct InterpolationUniforms
  {
    std::array<uint32_t, 4u> outputViewport{};
    std::array<float, 4u> controls{};
    std::array<uint32_t, 4u> flags{};
  };
  static_assert(sizeof(InterpolationUniforms) == 48u);

  struct InpaintUniforms
  {
    std::array<uint32_t, 4u> outputExtent{};
    std::array<uint32_t, 4u> flags{};
  };
  static_assert(sizeof(InpaintUniforms) == 32u);

  rendering::Texture createInternalTexture(
      const std::string &suffix, const rendering::Format format,
      const uint32_t width, const uint32_t height)
  {
    rendering::Texture texture = createFrameLocalTexture({
      .name = passName + "." + suffix,
      .format = format,
      .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
      .usage = rendering::ImageUsage::ImageUsage_Storage |
          rendering::ImageUsage::ImageUsage_Sampled,
      .width = std::max(width, 1u),
      .height = std::max(height, 1u),
      .depth = 1u,
      .mipLevels = 1u,
    });
    internalTextures_.push_back(texture);
    return texture;
  }

  template <typename T>
  rendering::Buffer createUniformBuffer(const std::string &suffix)
  {
    rendering::Buffer buffer = createFrameLocalBuffer({
      .name = passName + "." + suffix + ".uniforms",
      .size = sizeof(T),
      .usage = rendering::BufferUsage::BufferUsage_Uniform |
          rendering::BufferUsage::BufferUsage_Push,
    });
    uniformBuffers_.push_back(buffer);
    return buffer;
  }

  template <typename T>
  void writeUniform(const rendering::Buffer &buffer, const T &value)
  {
    renderGraph->bufferWrite(
        buffer, 0u, sizeof(T), const_cast<T *>(&value));
  }

  static rendering::BufferView uniformView(
      const rendering::Buffer &buffer, const uint64_t size)
  {
    return {
      .buffer = buffer,
      .offset = 0u,
      .size = size,
      .access = rendering::AccessPattern::SHADER_READ,
    };
  }

  static rendering::TextureView sampled(
      const rendering::Texture &texture,
      const bool externallyProduced = false)
  {
    return {
      .texture = texture,
      .access = rendering::AccessPattern::SHADER_READ,
      .layout = externallyProduced
          ? rendering::ResourceLayout::SHADER_READ_ONLY
          : rendering::ResourceLayout::GENERAL,
      .index = 0u,
      .flags = rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u,
    };
  }

  static rendering::TextureView storage(const rendering::Texture &texture)
  {
    return {
      .texture = texture,
      .access = rendering::AccessPattern::SHADER_WRITE,
      .layout = rendering::ResourceLayout::GENERAL,
      .index = 0u,
      .flags = rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u,
    };
  }

  rendering::BindingsLayout createLayout(
      rendering::BindingsLayoutInfo info)
  {
    rendering::BindingsLayout layout = renderGraph->createBindingsLayout(info);
    layouts_.push_back(layout);
    return layout;
  }

  rendering::Shader createShader(
      const rendering::BindingsLayout layout, const std::string &name,
      const std::string &path)
  {
    rendering::Shader shader = renderGraph->createShader({
      .name = passName + "." + name + ".shader",
      .layout = layout,
      .src = os::io::readRelativeFile(path),
      .type = rendering::ShaderType::SpirV,
    });
    shaders_.push_back(shader);
    return shader;
  }

  rendering::ComputePipeline createPipeline(
      const rendering::BindingsLayout layout,
      const rendering::Shader shader, const std::string &name)
  {
    rendering::ComputePipeline pipeline = renderGraph->createComputePipeline({
      .entry = "main",
      .layout = layout,
      .name = passName + "." + name + ".pipeline",
      .shader = shader,
    });
    pipelines_.push_back(pipeline);
    return pipeline;
  }

  rendering::BindingGroups createGroups(
      const rendering::BindingsLayout layout,
      rendering::BindingGroupsInfo info)
  {
    info.layout = layout;
    rendering::BindingGroups groups = renderGraph->createBindingGroups(info);
    allBindingGroups_.push_back(groups);
    return groups;
  }

  void createPyramidResources()
  {
    uint32_t width = std::max(settings_.outputWidth, 1u);
    uint32_t height = std::max(settings_.outputHeight, 1u);
    for (uint32_t level = 0u; level < kPyramidLevels; ++level)
    {
      width = std::max((width + 1u) / 2u, 1u);
      height = std::max((height + 1u) / 2u, 1u);
      pyramidWidths_[level] = width;
      pyramidHeights_[level] = height;
      previousLuma_[level] = createInternalTexture(
          "previousLuma" + std::to_string(level),
          rendering::Format::Format_R32Float, width, height);
      currentLuma_[level] = createInternalTexture(
          "currentLuma" + std::to_string(level),
          rendering::Format::Format_R32Float, width, height);
      gameMotionPyramid_[level] = createInternalTexture(
          "gameMotion" + std::to_string(level),
          rendering::Format::Format_RGBA16Float, width, height);
      inpaintPyramid_[level] = createInternalTexture(
          "inpaint" + std::to_string(level),
          rendering::Format::Format_RGBA16Float, width, height);
    }

    // quarter, eighth, and sixteenth resolution fields.
    for (uint32_t level = 0u; level < kFlowLevels; ++level)
    {
      const uint32_t pyramidLevel = level + 1u;
      flowWidths_[level] = pyramidWidths_[pyramidLevel];
      flowHeights_[level] = pyramidHeights_[pyramidLevel];
      opticalFlow_[level] = createInternalTexture(
          "opticalFlow" + std::to_string(level),
          rendering::Format::Format_RGBA16Float,
          flowWidths_[level], flowHeights_[level]);
    }
    interpolatedRaw_ = createInternalTexture(
        "interpolatedRaw", rendering::Format::Format_RGBA16Float,
        settings_.outputWidth, settings_.outputHeight);
  }

  void createMotionPyramidStages()
  {
    motionPyramidLayout_ = createLayout({
      .name = passName + ".motionPyramid.layout",
      .groups = {rendering::BindingGroupLayout{
        .buffers = {{
          .name = "uniforms", .binding = 0u, .isDynamic = false,
          .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer,
          .visibility = rendering::BindingVisibility::BindingVisibility_Compute,
        }},
        .textures = {
          {.name = "input", .binding = 1u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "disocclusion", .binding = 2u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
        },
        .storageTextures = {{
          .name = "output", .binding = 3u,
          .visibility = rendering::BindingVisibility::BindingVisibility_Compute,
        }},
      }},
    });
    const rendering::Shader shader = createShader(
        motionPyramidLayout_, "motionPyramid",
        "assets/shaders/spirv/frame-generation-motion-pyramid-cs.spirv");
    motionPyramidPipeline_ = createPipeline(
        motionPyramidLayout_, shader, "motionPyramid");

    for (uint32_t level = 0u; level < kPyramidLevels; ++level)
    {
      motionPyramidUniforms_[level] =
          createUniformBuffer<MotionPyramidUniforms>(
              "motionPyramid" + std::to_string(level));
      const rendering::Texture input = level == 0u
          ? currentMotion_ : gameMotionPyramid_[level - 1u];
      motionPyramidGroups_[level] = createGroups(motionPyramidLayout_, {
        .name = passName + ".motionPyramid" + std::to_string(level) + ".groups",
        .groups = {rendering::GroupInfo{
          .name = "group0",
          .buffers = {{.binding = 0u, .bufferView = uniformView(
              motionPyramidUniforms_[level], sizeof(MotionPyramidUniforms))}},
          .textures = {
            {.binding = 1u, .textureView = sampled(input, level == 0u)},
            {.binding = 2u, .textureView = sampled(disocclusion_, true)},
          },
          .storageTextures = {{.binding = 3u, .textureView = storage(
              gameMotionPyramid_[level])}},
        }},
      });
    }
  }

  void createLumaStages()
  {
    lumaLayout_ = createLayout({
      .name = passName + ".luma.layout",
      .groups = {rendering::BindingGroupLayout{
        .buffers = {{
          .name = "uniforms", .binding = 0u, .isDynamic = false,
          .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer,
          .visibility = rendering::BindingVisibility::BindingVisibility_Compute,
        }},
        .textures = {{
          .name = "input", .binding = 1u,
          .visibility = rendering::BindingVisibility::BindingVisibility_Compute,
        }},
        .storageTextures = {{
          .name = "output", .binding = 2u,
          .visibility = rendering::BindingVisibility::BindingVisibility_Compute,
        }},
      }},
    });
    const rendering::Shader shader = createShader(
        lumaLayout_, "luma",
        "assets/shaders/spirv/frame-generation-luma-pyramid-cs.spirv");
    lumaPipeline_ = createPipeline(lumaLayout_, shader, "luma");

    for (uint32_t level = 0u; level < kPyramidLevels; ++level)
    {
      const uint32_t sourceWidth = level == 0u
          ? settings_.outputWidth : pyramidWidths_[level - 1u];
      const uint32_t sourceHeight = level == 0u
          ? settings_.outputHeight : pyramidHeights_[level - 1u];
      const PyramidUniforms values{
        {sourceWidth, sourceHeight,
         pyramidWidths_[level], pyramidHeights_[level]},
        {level == 0u ? 0u : 1u, 0u, 0u, 0u},
      };
      previousLumaUniforms_[level] =
          createUniformBuffer<PyramidUniforms>(
              "previousLuma" + std::to_string(level));
      currentLumaUniforms_[level] =
          createUniformBuffer<PyramidUniforms>(
              "currentLuma" + std::to_string(level));
      writeUniform(previousLumaUniforms_[level], values);
      writeUniform(currentLumaUniforms_[level], values);
      const rendering::Texture previousInput = level == 0u
          ? previousColor_ : previousLuma_[level - 1u];
      const rendering::Texture currentInput = level == 0u
          ? currentColor_ : currentLuma_[level - 1u];
      previousLumaGroups_[level] = createGroups(lumaLayout_, {
        .name = passName + ".previousLuma" + std::to_string(level) + ".groups",
        .groups = {rendering::GroupInfo{
          .name = "group0",
          .buffers = {{.binding = 0u, .bufferView = uniformView(
              previousLumaUniforms_[level], sizeof(PyramidUniforms))}},
          .textures = {{.binding = 1u, .textureView = sampled(
              previousInput, level == 0u)}},
          .storageTextures = {{.binding = 2u, .textureView = storage(
              previousLuma_[level])}},
        }},
      });
      currentLumaGroups_[level] = createGroups(lumaLayout_, {
        .name = passName + ".currentLuma" + std::to_string(level) + ".groups",
        .groups = {rendering::GroupInfo{
          .name = "group0",
          .buffers = {{.binding = 0u, .bufferView = uniformView(
              currentLumaUniforms_[level], sizeof(PyramidUniforms))}},
          .textures = {{.binding = 1u, .textureView = sampled(
              currentInput, level == 0u)}},
          .storageTextures = {{.binding = 2u, .textureView = storage(
              currentLuma_[level])}},
        }},
      });
    }
  }

  void createFlowStages()
  {
    flowLayout_ = createLayout({
      .name = passName + ".flow.layout",
      .groups = {rendering::BindingGroupLayout{
        .buffers = {{
          .name = "uniforms", .binding = 0u, .isDynamic = false,
          .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer,
          .visibility = rendering::BindingVisibility::BindingVisibility_Compute,
        }},
        .textures = {
          {.name = "previousLuma", .binding = 1u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "currentLuma", .binding = 2u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "previousFlow", .binding = 3u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
        },
        .storageTextures = {{
          .name = "outputFlow", .binding = 4u,
          .visibility = rendering::BindingVisibility::BindingVisibility_Compute,
        }},
      }},
    });
    const rendering::Shader shader = createShader(
        flowLayout_, "flow",
        "assets/shaders/spirv/frame-generation-optical-flow-cs.spirv");
    flowPipeline_ = createPipeline(flowLayout_, shader, "flow");

    for (uint32_t level = 0u; level < kFlowLevels; ++level)
    {
      flowUniforms_[level] = createUniformBuffer<FlowUniforms>(
          "flow" + std::to_string(level));
      const uint32_t pyramidLevel = level + 1u;
      const rendering::Texture previousFlow = level + 1u < kFlowLevels
          ? opticalFlow_[level + 1u] : currentLuma_[pyramidLevel];
      flowGroups_[level] = createGroups(flowLayout_, {
        .name = passName + ".flow" + std::to_string(level) + ".groups",
        .groups = {rendering::GroupInfo{
          .name = "group0",
          .buffers = {{.binding = 0u, .bufferView = uniformView(
              flowUniforms_[level], sizeof(FlowUniforms))}},
          .textures = {
            {.binding = 1u, .textureView = sampled(previousLuma_[pyramidLevel])},
            {.binding = 2u, .textureView = sampled(currentLuma_[pyramidLevel])},
            {.binding = 3u, .textureView = sampled(previousFlow)},
          },
          .storageTextures = {{.binding = 4u, .textureView = storage(
              opticalFlow_[level])}},
        }},
      });
    }
  }

  void createInterpolationStage()
  {
    interpolationUniform_ =
        createUniformBuffer<InterpolationUniforms>("interpolation");
    interpolationLayout_ = createLayout({
      .name = passName + ".interpolation.layout",
      .groups = {rendering::BindingGroupLayout{
        .buffers = {{
          .name = "uniforms", .binding = 0u, .isDynamic = false,
          .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer,
          .visibility = rendering::BindingVisibility::BindingVisibility_Compute,
        }},
        .textures = {
          {.name = "previousColor", .binding = 1u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "currentColor", .binding = 2u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "previousDepth", .binding = 3u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "currentDepth", .binding = 4u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "currentMotion", .binding = 5u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "disocclusion", .binding = 6u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "gameMotion0", .binding = 7u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "gameMotion1", .binding = 8u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "gameMotion2", .binding = 9u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "gameMotion3", .binding = 10u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "opticalFlow", .binding = 11u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
        },
        .storageTextures = {{
          .name = "output", .binding = 12u,
          .visibility = rendering::BindingVisibility::BindingVisibility_Compute,
        }},
      }},
    });
    const rendering::Shader shader = createShader(
        interpolationLayout_, "interpolation",
        "assets/shaders/spirv/frame-generation-interpolate-cs.spirv");
    interpolationPipeline_ = createPipeline(
        interpolationLayout_, shader, "interpolation");
    interpolationGroups_ = createGroups(interpolationLayout_, {
      .name = passName + ".interpolation.groups",
      .groups = {rendering::GroupInfo{
        .name = "group0",
        .buffers = {{.binding = 0u, .bufferView = uniformView(
            interpolationUniform_, sizeof(InterpolationUniforms))}},
        .textures = {
          {.binding = 1u, .textureView = sampled(previousColor_, true)},
          {.binding = 2u, .textureView = sampled(currentColor_, true)},
          {.binding = 3u, .textureView = sampled(previousDepth_, true)},
          {.binding = 4u, .textureView = sampled(currentDepth_, true)},
          {.binding = 5u, .textureView = sampled(currentMotion_, true)},
          {.binding = 6u, .textureView = sampled(disocclusion_, true)},
          {.binding = 7u, .textureView = sampled(gameMotionPyramid_[0u])},
          {.binding = 8u, .textureView = sampled(gameMotionPyramid_[1u])},
          {.binding = 9u, .textureView = sampled(gameMotionPyramid_[2u])},
          {.binding = 10u, .textureView = sampled(gameMotionPyramid_[3u])},
          {.binding = 11u, .textureView = sampled(opticalFlow_[0u])},
        },
        .storageTextures = {{.binding = 12u, .textureView = storage(
            interpolatedRaw_)}},
      }},
    });
  }

  void createInpaintStages()
  {
    inpaintPyramidLayout_ = createLayout({
      .name = passName + ".inpaintPyramid.layout",
      .groups = {rendering::BindingGroupLayout{
        .buffers = {{
          .name = "uniforms", .binding = 0u, .isDynamic = false,
          .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer,
          .visibility = rendering::BindingVisibility::BindingVisibility_Compute,
        }},
        .textures = {{.name = "input", .binding = 1u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute}},
        .storageTextures = {{.name = "output", .binding = 2u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute}},
      }},
    });
    const rendering::Shader pyramidShader = createShader(
        inpaintPyramidLayout_, "inpaintPyramid",
        "assets/shaders/spirv/frame-generation-inpaint-pyramid-cs.spirv");
    inpaintPyramidPipeline_ = createPipeline(
        inpaintPyramidLayout_, pyramidShader, "inpaintPyramid");
    for (uint32_t level = 0u; level < kPyramidLevels; ++level)
    {
      const uint32_t sourceWidth = level == 0u
          ? settings_.outputWidth : pyramidWidths_[level - 1u];
      const uint32_t sourceHeight = level == 0u
          ? settings_.outputHeight : pyramidHeights_[level - 1u];
      const PyramidUniforms values{
        {sourceWidth, sourceHeight,
         pyramidWidths_[level], pyramidHeights_[level]},
        {0u, 0u, 0u, 0u},
      };
      inpaintPyramidUniforms_[level] =
          createUniformBuffer<PyramidUniforms>(
              "inpaintPyramid" + std::to_string(level));
      writeUniform(inpaintPyramidUniforms_[level], values);
      const rendering::Texture input = level == 0u
          ? interpolatedRaw_ : inpaintPyramid_[level - 1u];
      inpaintPyramidGroups_[level] = createGroups(inpaintPyramidLayout_, {
        .name = passName + ".inpaintPyramid" + std::to_string(level) + ".groups",
        .groups = {rendering::GroupInfo{
          .name = "group0",
          .buffers = {{.binding = 0u, .bufferView = uniformView(
              inpaintPyramidUniforms_[level], sizeof(PyramidUniforms))}},
          .textures = {{.binding = 1u, .textureView = sampled(input)}},
          .storageTextures = {{.binding = 2u, .textureView = storage(
              inpaintPyramid_[level])}},
        }},
      });
    }

    inpaintUniform_ = createUniformBuffer<InpaintUniforms>("inpaint");
    inpaintLayout_ = createLayout({
      .name = passName + ".inpaint.layout",
      .groups = {rendering::BindingGroupLayout{
        .buffers = {{
          .name = "uniforms", .binding = 0u, .isDynamic = false,
          .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer,
          .visibility = rendering::BindingVisibility::BindingVisibility_Compute,
        }},
        .textures = {
          {.name = "raw", .binding = 1u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "pyramid0", .binding = 2u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "pyramid1", .binding = 3u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "pyramid2", .binding = 4u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
          {.name = "pyramid3", .binding = 5u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
        },
        .storageTextures = {{.name = "output", .binding = 6u, .visibility = rendering::BindingVisibility::BindingVisibility_Compute}},
      }},
    });
    const rendering::Shader shader = createShader(
        inpaintLayout_, "inpaint",
        "assets/shaders/spirv/frame-generation-inpaint-cs.spirv");
    inpaintPipeline_ = createPipeline(inpaintLayout_, shader, "inpaint");
    inpaintGroups_ = createGroups(inpaintLayout_, {
      .name = passName + ".inpaint.groups",
      .groups = {rendering::GroupInfo{
        .name = "group0",
        .buffers = {{.binding = 0u, .bufferView = uniformView(
            inpaintUniform_, sizeof(InpaintUniforms))}},
        .textures = {
          {.binding = 1u, .textureView = sampled(interpolatedRaw_)},
          {.binding = 2u, .textureView = sampled(inpaintPyramid_[0u])},
          {.binding = 3u, .textureView = sampled(inpaintPyramid_[1u])},
          {.binding = 4u, .textureView = sampled(inpaintPyramid_[2u])},
          {.binding = 5u, .textureView = sampled(inpaintPyramid_[3u])},
        },
        .storageTextures = {{.binding = 6u, .textureView = storage(output_)}},
      }},
    });
  }

  void writeDynamicUniforms()
  {
    for (uint32_t level = 0u; level < kPyramidLevels; ++level)
    {
      const uint32_t sourceWidth = level == 0u
          ? settings_.motionWidth : pyramidWidths_[level - 1u];
      const uint32_t sourceHeight = level == 0u
          ? settings_.motionHeight : pyramidHeights_[level - 1u];
      const MotionPyramidUniforms values{
        {sourceWidth, sourceHeight,
         pyramidWidths_[level], pyramidHeights_[level]},
        {std::max(settings_.maximumMotionPixels, 1.0f),
         static_cast<float>(std::max(settings_.outputWidth, 1u)),
         static_cast<float>(std::max(settings_.outputHeight, 1u)), 0.0f},
        {level == 0u ? 0u : 1u, 0u, 0u, 0u},
      };
      writeUniform(motionPyramidUniforms_[level], values);
    }
    for (uint32_t level = 0u; level < kFlowLevels; ++level)
    {
      const uint32_t pyramidLevel = level + 1u;
      const FlowUniforms values{
        {pyramidWidths_[pyramidLevel], pyramidHeights_[pyramidLevel],
         flowWidths_[level], flowHeights_[level]},
        {level + 1u == kFlowLevels ? 4.0f : 2.0f, 5.0f, 0.0f, 0.0f},
        {settings_.historyValid ? 1u : 0u,
         level + 1u == kFlowLevels ? 1u : 0u, 0u, 0u},
      };
      writeUniform(flowUniforms_[level], values);
    }
    const InterpolationUniforms interpolation{
      {std::max(settings_.outputWidth, 1u),
       std::max(settings_.outputHeight, 1u),
       std::max(settings_.motionWidth, 1u),
       std::max(settings_.motionHeight, 1u)},
      {std::clamp(settings_.interpolationPoint, 0.0f, 1.0f),
       std::max(settings_.depthThreshold, 1.0e-5f),
       std::max(settings_.maximumMotionPixels, 1.0f),
       std::max(settings_.clampGamma, 0.0f)},
      {settings_.historyValid ? 1u : 0u, settings_.debugMode, 0u, 0u},
    };
    writeUniform(interpolationUniform_, interpolation);
    const InpaintUniforms inpaint{
      {std::max(settings_.outputWidth, 1u),
       std::max(settings_.outputHeight, 1u), 0u, 0u},
      {settings_.debugMode, 0u, 0u, 0u},
    };
    writeUniform(inpaintUniform_, inpaint);
  }

  void createResources()
  {
    if (resourcesCreated_)
      return;
    createPyramidResources();
    createLumaStages();
    createMotionPyramidStages();
    createFlowStages();
    createInterpolationStage();
    createInpaintStages();
    resourcesCreated_ = true;
    writeDynamicUniforms();
  }

  static void dispatch(
      rendering::CommandRecorder &commandBuffer,
      const rendering::ComputePipeline pipeline,
      const rendering::BindingGroups groups,
      const uint32_t width, const uint32_t height)
  {
    commandBuffer.cmdBindComputePipeline(pipeline);
    commandBuffer.cmdBindBindingGroups(groups, nullptr, 0u);
    commandBuffer.cmdDispatch(
        (std::max(width, 1u) + 7u) / 8u,
        (std::max(height, 1u) + 7u) / 8u, 1u);
  }

  static void computeBarrier(rendering::CommandRecorder &commandBuffer)
  {
    commandBuffer.cmdMemoryBarrier(
        rendering::PipelineStage::COMPUTE_SHADER,
        rendering::PipelineStage::COMPUTE_SHADER,
        rendering::AccessPattern::SHADER_WRITE,
        rendering::AccessPattern::SHADER_READ_WRITE);
  }

  void destroy()
  {
    for (const rendering::ComputePipeline pipeline : pipelines_)
      if (pipeline.isValid()) renderGraph->deleteComputePipeline(pipeline);
    for (const rendering::Shader shader : shaders_)
      if (shader.isValid()) renderGraph->deleteShader(shader);
    for (const rendering::BindingGroups groups : allBindingGroups_)
      if (groups.isValid()) renderGraph->deleteBindingGroups(groups);
    for (const rendering::BindingsLayout layout : layouts_)
      if (layout.isValid()) renderGraph->deleteBindingsLayout(layout);
    for (const rendering::Buffer buffer : uniformBuffers_)
      if (buffer.isValid()) renderGraph->deleteBuffer(buffer);
    for (const rendering::Texture texture : internalTextures_)
      if (texture.isValid()) renderGraph->deleteTexture(texture);
  }

  rendering::Texture previousColor_{};
  rendering::Texture currentColor_{};
  rendering::Texture previousDepth_{};
  rendering::Texture currentDepth_{};
  rendering::Texture currentMotion_{};
  rendering::Texture disocclusion_{};
  rendering::Texture output_{};
  Settings settings_{};
  bool resourcesCreated_ = false;

  std::array<uint32_t, kPyramidLevels> pyramidWidths_{};
  std::array<uint32_t, kPyramidLevels> pyramidHeights_{};
  std::array<uint32_t, kFlowLevels> flowWidths_{};
  std::array<uint32_t, kFlowLevels> flowHeights_{};
  std::array<rendering::Texture, kPyramidLevels> previousLuma_{};
  std::array<rendering::Texture, kPyramidLevels> currentLuma_{};
  std::array<rendering::Texture, kPyramidLevels> gameMotionPyramid_{};
  std::array<rendering::Texture, kPyramidLevels> inpaintPyramid_{};
  std::array<rendering::Texture, kFlowLevels> opticalFlow_{};
  rendering::Texture interpolatedRaw_{};

  std::array<rendering::Buffer, kPyramidLevels> previousLumaUniforms_{};
  std::array<rendering::Buffer, kPyramidLevels> currentLumaUniforms_{};
  std::array<rendering::Buffer, kPyramidLevels> motionPyramidUniforms_{};
  std::array<rendering::Buffer, kPyramidLevels> inpaintPyramidUniforms_{};
  std::array<rendering::Buffer, kFlowLevels> flowUniforms_{};
  rendering::Buffer interpolationUniform_{};
  rendering::Buffer inpaintUniform_{};

  rendering::BindingsLayout lumaLayout_{};
  rendering::BindingsLayout motionPyramidLayout_{};
  rendering::BindingsLayout flowLayout_{};
  rendering::BindingsLayout interpolationLayout_{};
  rendering::BindingsLayout inpaintPyramidLayout_{};
  rendering::BindingsLayout inpaintLayout_{};
  rendering::ComputePipeline lumaPipeline_{};
  rendering::ComputePipeline motionPyramidPipeline_{};
  rendering::ComputePipeline flowPipeline_{};
  rendering::ComputePipeline interpolationPipeline_{};
  rendering::ComputePipeline inpaintPyramidPipeline_{};
  rendering::ComputePipeline inpaintPipeline_{};
  std::array<rendering::BindingGroups, kPyramidLevels> previousLumaGroups_{};
  std::array<rendering::BindingGroups, kPyramidLevels> currentLumaGroups_{};
  std::array<rendering::BindingGroups, kPyramidLevels> motionPyramidGroups_{};
  std::array<rendering::BindingGroups, kPyramidLevels> inpaintPyramidGroups_{};
  std::array<rendering::BindingGroups, kFlowLevels> flowGroups_{};
  rendering::BindingGroups interpolationGroups_{};
  rendering::BindingGroups inpaintGroups_{};

  std::vector<rendering::Texture> internalTextures_;
  std::vector<rendering::Buffer> uniformBuffers_;
  std::vector<rendering::BindingsLayout> layouts_;
  std::vector<rendering::BindingGroups> allBindingGroups_;
  std::vector<rendering::Shader> shaders_;
  std::vector<rendering::ComputePipeline> pipelines_;
};

} // namespace rendering::gpgpu
