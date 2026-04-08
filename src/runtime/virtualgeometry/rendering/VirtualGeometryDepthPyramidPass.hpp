#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace virtualgeometry
{
namespace gpgpu
{

class VirtualGeometryDepthPyramidPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t outputWidth = 0;  // 0 => use width
    uint32_t outputHeight = 0; // 0 => use height
    uint32_t mipCount = 0;        // 0 => auto
    uint32_t workgroupSizeX = 16; // SPD shader is fixed to 16x16
    uint32_t workgroupSizeY = 16; // kept for compatibility with existing call sites
    rendering::Format outputFormat = rendering::Format::Format_R32Float;
  };

private:
  struct ReductionConstants
  {
    uint32_t srcSize[2] = {0u, 0u};
    uint32_t dstSize[2] = {0u, 0u};
  };

  struct SPDConstants
  {
    uint32_t readSize[2] = {0u, 0u};
    uint32_t dispatchSize[2] = {0u, 0u};
    uint32_t levelsToWrite = 1u;
    uint32_t copySourceToFirstMip = 0u;
    uint32_t numWorkgroups = 0u;
  };

  struct DispatchPlan
  {
    uint32_t srcMipLevel = 0u;
    uint32_t dstBaseMip = 0u;
    uint32_t srcWidth = 1u;
    uint32_t srcHeight = 1u;
    uint32_t dispatchWidth = 1u;
    uint32_t dispatchHeight = 1u;
    uint32_t levelsToWrite = 1u;
    uint32_t workgroupsX = 1u;
    uint32_t workgroupsY = 1u;
  };

  rendering::BindingsLayout reductionLayout;
  rendering::BindingGroups reductionBindingGroup;
  rendering::Shader reductionShader;
  rendering::ComputePipeline reductionPipeline;

  rendering::BindingsLayout depthPyramidLayout;
  rendering::BindingGroups sourceDepthPyramidBindingGroup;
  std::vector<rendering::BindingGroups> depthPyramidBindingGroups;
  rendering::Shader sourceDepthPyramidShader;
  rendering::Shader depthPyramidShader;
  rendering::ComputePipeline sourceDepthPyramidPipeline;
  rendering::ComputePipeline depthPyramidPipeline;

  rendering::Buffer reductionConstantsBuffer;
  rendering::Buffer constantsBuffer;
  rendering::Buffer counterBuffer;

  rendering::Texture inputDepthTexture;
  rendering::Texture providedOutputHZBTexture;
  rendering::Texture outputHZBTexture;
  bool ownsOutputHZBTexture = true;

  Settings settings;
  uint32_t resolvedMipCount = 1u;
  static constexpr uint32_t kSpdTileSize = 64u;
  static constexpr uint32_t kReductionWorkgroupSizeX = 8u;
  static constexpr uint32_t kReductionWorkgroupSizeY = 8u;
  static constexpr uint32_t kMinMipLevelsPerPass = 4u;
  static constexpr uint32_t kMaxMipLevelsPerPass = 8u;
  static constexpr uint32_t kMaxMipLevelsPerPassUpperBound = 8u;
  // SPD local reduction levels: the maximum mip levels one dispatch can produce
  // using only workgroup-local LDS, with no cross-workgroup global sync.
  // Removing the last-workgroup serial loop means this is the hard cap per pass;
  // overflow levels fall through to subsequent dispatchPlans automatically.
  static constexpr uint32_t kSpdLocalReductionLevels = 6u;

  static uint32_t alignUp(uint32_t value, uint32_t alignment)
  {
    if (alignment == 0u)
    {
      return value;
    }
    return (value + alignment - 1u) & ~(alignment - 1u);
  }

  static uint32_t ceilDiv(uint32_t value, uint32_t divisor)
  {
    return (value + divisor - 1u) / divisor;
  }

  static uint32_t computeMipCount(uint32_t width, uint32_t height)
  {
    uint32_t maxDim = std::max(width, height);
    uint32_t levels = 1u;
    while (maxDim > 1u)
    {
      maxDim >>= 1u;
      ++levels;
    }
    return levels;
  }

  static uint32_t mipDimension(uint32_t baseDimension, uint32_t mipLevel)
  {
    return std::max(1u, baseDimension >> mipLevel);
  }

public:
  VirtualGeometryDepthPyramidPass(rendering::Texture depthTexture, Settings settings) : inputDepthTexture(depthTexture), settings(settings)
  {
  }

  VirtualGeometryDepthPyramidPass(rendering::Texture depthTexture, rendering::Texture outputHZBTexture, Settings settings) : inputDepthTexture(depthTexture), providedOutputHZBTexture(outputHZBTexture), settings(settings)
  {
  }

  ~VirtualGeometryDepthPyramidPass() override
  {
    renderGraph->deleteComputePipeline(reductionPipeline);
    renderGraph->deleteBindingGroups(reductionBindingGroup);
    renderGraph->deleteBindingsLayout(reductionLayout);
    renderGraph->deleteShader(reductionShader);
    renderGraph->deleteBuffer(reductionConstantsBuffer);
    renderGraph->deleteComputePipeline(sourceDepthPyramidPipeline);
    renderGraph->deleteComputePipeline(depthPyramidPipeline);
    renderGraph->deleteBindingGroups(sourceDepthPyramidBindingGroup);
    for (const auto &group : depthPyramidBindingGroups)
    {
      renderGraph->deleteBindingGroups(group);
    }
    renderGraph->deleteBindingsLayout(depthPyramidLayout);
    renderGraph->deleteShader(sourceDepthPyramidShader);
    renderGraph->deleteShader(depthPyramidShader);
    renderGraph->deleteBuffer(constantsBuffer);
    renderGraph->deleteBuffer(counterBuffer);
    if (ownsOutputHZBTexture)
    {
      renderGraph->deleteTexture(outputHZBTexture);
    }
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (settings.width == 0u || settings.height == 0u)
    {
      throw std::runtime_error("VirtualGeometryDepthPyramidPass requires non-zero width and height");
    }
    static_assert(kMaxMipLevelsPerPass >= kMinMipLevelsPerPass && kMaxMipLevelsPerPass <= kMaxMipLevelsPerPassUpperBound, "kMaxMipLevelsPerPass must be in [4, 8]");

    const uint32_t outputWidth = (settings.outputWidth == 0u) ? settings.width : settings.outputWidth;
    const uint32_t outputHeight = (settings.outputHeight == 0u) ? settings.height : settings.outputHeight;
    const uint32_t maxMipCount = computeMipCount(outputWidth, outputHeight);
    resolvedMipCount = (settings.mipCount == 0u) ? maxMipCount : std::min(settings.mipCount, maxMipCount);
    resolvedMipCount = std::max(1u, resolvedMipCount);

    if (!providedOutputHZBTexture.name.empty())
    {
      outputHZBTexture = providedOutputHZBTexture;
      ownsOutputHZBTexture = false;
    }
    else
    {
      outputHZBTexture = createFrameLocalTexture(
          rendering::TextureInfo{
            .name = passName + "_HZB.texture",
            .format = settings.outputFormat,
            .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
            .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
            .width = outputWidth,
            .height = outputHeight,
            .depth = 1,
            .mipLevels = resolvedMipCount,
          });
      ownsOutputHZBTexture = true;
    }

    const bool skipMip0 = settings.width == outputWidth && settings.height == outputHeight && resolvedMipCount > 1u;

    DispatchPlan sourceDepthPlan{};
    bool hasSourceDepthPlan = false;

    std::vector<DispatchPlan> dispatchPlans;
    dispatchPlans.reserve(ceilDiv(std::max(1u, resolvedMipCount - 1u), kSpdLocalReductionLevels));

    uint32_t producedMipLevels = 1u;
    if (skipMip0)
    {
      // Keep mip 0 unused and start the HZB at the first reduced level so the
      // full-resolution copy path can drop out of the normal frame.
      sourceDepthPlan.dstBaseMip = 1u;
      sourceDepthPlan.levelsToWrite = std::min({kMaxMipLevelsPerPass, kSpdLocalReductionLevels, resolvedMipCount - 1u});
      sourceDepthPlan.srcWidth = outputWidth;
      sourceDepthPlan.srcHeight = outputHeight;
      sourceDepthPlan.dispatchWidth = outputWidth;
      sourceDepthPlan.dispatchHeight = outputHeight;
      sourceDepthPlan.workgroupsX = ceilDiv(sourceDepthPlan.dispatchWidth, kSpdTileSize);
      sourceDepthPlan.workgroupsY = ceilDiv(sourceDepthPlan.dispatchHeight, kSpdTileSize);
      hasSourceDepthPlan = sourceDepthPlan.levelsToWrite != 0u;
      producedMipLevels = sourceDepthPlan.dstBaseMip + sourceDepthPlan.levelsToWrite;
    }

    while (producedMipLevels < resolvedMipCount)
    {
      DispatchPlan plan{};
      plan.dstBaseMip = producedMipLevels;
      plan.levelsToWrite = std::min({kMaxMipLevelsPerPass, kSpdLocalReductionLevels, resolvedMipCount - producedMipLevels});
      plan.srcMipLevel = producedMipLevels - 1u;
      plan.srcWidth = mipDimension(outputWidth, plan.srcMipLevel);
      plan.srcHeight = mipDimension(outputHeight, plan.srcMipLevel);
      plan.dispatchWidth = plan.srcWidth;
      plan.dispatchHeight = plan.srcHeight;
      plan.workgroupsX = ceilDiv(plan.dispatchWidth, kSpdTileSize);
      plan.workgroupsY = ceilDiv(plan.dispatchHeight, kSpdTileSize);
      dispatchPlans.push_back(plan);
      producedMipLevels += plan.levelsToWrite;
    }

    if (!hasSourceDepthPlan)
    {
      reductionConstantsBuffer = createFrameLocalBuffer(
          rendering::BufferInfo{
            .name = passName + "_ReductionConstants.buffer",
            .size = sizeof(ReductionConstants),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
          });

      ReductionConstants reductionConstants{};
      reductionConstants.srcSize[0] = settings.width;
      reductionConstants.srcSize[1] = settings.height;
      reductionConstants.dstSize[0] = outputWidth;
      reductionConstants.dstSize[1] = outputHeight;
      renderGraph->bufferWrite(reductionConstantsBuffer, 0, sizeof(ReductionConstants), &reductionConstants);

      rendering::BindingGroupLayout reductionGroupLayout{};
      reductionGroupLayout.buffers = {
        {
          .name = "constants",
          .binding = 0,
          .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer,
          .visibility = rendering::BindingVisibility::BindingVisibility_Compute,
          .isDynamic = false,
        },
      };
      reductionGroupLayout.textures = {
        {
          .name = "srcDepth",
          .binding = 1,
          .visibility = rendering::BindingVisibility::BindingVisibility_Compute,
        },
      };
      reductionGroupLayout.storageTextures = {
        {
          .name = "dstMip0",
          .binding = 2,
          .visibility = rendering::BindingVisibility::BindingVisibility_Compute,
        },
      };

      reductionLayout = renderGraph->createBindingsLayout(
          rendering::BindingsLayoutInfo{
            .name = passName + "_reductionLayout.layout",
            .groups = {reductionGroupLayout},
          });

      auto reductionShaderSrc = os::io::readRelativeFile("assets/shaders/spirv/hzb-depth-reduce-pot.spirv");
      reductionShader = renderGraph->createShader(
          rendering::ShaderInfo{
            .name = passName + "_DepthReduction.shader",
            .layout = reductionLayout,
            .src = reductionShaderSrc,
            .type = rendering::ShaderType::SpirV,
          });

      reductionPipeline = renderGraph->createComputePipeline(
          rendering::ComputePipelineInfo{
            .entry = "depth_reduce_to_pot",
            .layout = reductionLayout,
            .name = passName + "_DepthReduceToPot.pipeline",
            .shader = reductionShader,
          });

      reductionBindingGroup = renderGraph->createBindingGroups(
          rendering::BindingGroupsInfo{
            .layout = reductionLayout,
            .name = passName + "_DepthReductionBindingGroup",
            .groups =
                {
                  rendering::GroupInfo{
                    .name = "Group0",
                    .buffers =
                        {
                          {
                            .bufferView =
                                {
                                  .buffer = reductionConstantsBuffer,
                                  .offset = 0,
                                  .size = sizeof(ReductionConstants),
                                  .access = rendering::AccessPattern::SHADER_READ,
                                },
                            .binding = 0,
                          },
                        },
                    .textures =
                        {
                          {
                            .textureView =
                                {
                                  .texture = inputDepthTexture,
                                  .index = 0,
                                  .flags = rendering::ImageAspectFlags::Depth,
                                  .baseMipLevel = 0,
                                  .levelCount = 1,
                                  .baseArrayLayer = 0,
                                  .layerCount = 1,
                                  .access = rendering::AccessPattern::SHADER_READ,
                                  .layout = rendering::ResourceLayout::DEPTH_STENCIL_READ_ONLY,
                                },
                            .binding = 1,
                          },
                        },
                    .storageTextures =
                        {
                          {
                            .textureView =
                                {
                                  .texture = outputHZBTexture,
                                  .index = 0,
                                  .flags = rendering::ImageAspectFlags::Color,
                                  .baseMipLevel = 0,
                                  .levelCount = 1,
                                  .baseArrayLayer = 0,
                                  .layerCount = 1,
                                  .access = rendering::AccessPattern::SHADER_WRITE,
                                  .layout = rendering::ResourceLayout::GENERAL,
                                },
                            .binding = 2,
                          },
                        },
                  },
                },
          });
    }

    const size_t totalPlanCount = dispatchPlans.size() + (hasSourceDepthPlan ? 1u : 0u);
    if (totalPlanCount > 0u)
    {
      const uint32_t uniformAlignment = static_cast<uint32_t>(std::max<size_t>(1u, renderGraph->getRHI()->GetProperties().uniformBufferAlignment));
      const uint32_t alignedConstantsSize = alignUp(sizeof(SPDConstants), uniformAlignment);
      const uint64_t totalConstantsBytes = static_cast<uint64_t>(alignedConstantsSize) * totalPlanCount;

      constantsBuffer = createFrameLocalBuffer(
          rendering::BufferInfo{
            .name = passName + "_Constants.buffer",
            .size = totalConstantsBytes,
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
          });

      counterBuffer = createFrameLocalBuffer(
          rendering::BufferInfo{
            .name = passName + "_Counter.buffer",
            .size = sizeof(uint32_t),
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
          });

      std::vector<uint8_t> constantsData(totalConstantsBytes, 0u);
      auto writeConstants = [&](size_t planIndex, const DispatchPlan &plan) {
        SPDConstants *constants = reinterpret_cast<SPDConstants *>(constantsData.data() + planIndex * alignedConstantsSize);
        constants->readSize[0] = plan.srcWidth;
        constants->readSize[1] = plan.srcHeight;
        constants->dispatchSize[0] = plan.dispatchWidth;
        constants->dispatchSize[1] = plan.dispatchHeight;
        constants->levelsToWrite = plan.levelsToWrite;
        constants->copySourceToFirstMip = 0u;
        constants->numWorkgroups = plan.workgroupsX * plan.workgroupsY;
      };

      size_t planIndex = 0u;
      if (hasSourceDepthPlan)
      {
        writeConstants(planIndex++, sourceDepthPlan);
      }
      for (const DispatchPlan &plan : dispatchPlans)
      {
        writeConstants(planIndex++, plan);
      }

      uint32_t counterReset = 0u;
      renderGraph->bufferWrite(constantsBuffer, 0, totalConstantsBytes, constantsData.data());
      renderGraph->bufferWrite(counterBuffer, 0, sizeof(uint32_t), &counterReset);

      rendering::BindingGroupLayout groupLayout{};
      groupLayout.buffers = {
        {
          .name = "constants",
          .binding = 0,
          .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer,
          .visibility = rendering::BindingVisibility::BindingVisibility_Compute,
          .isDynamic = false,
        },
        {
          .name = "globalCounter",
          .binding = 1,
          .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
          .visibility = rendering::BindingVisibility::BindingVisibility_Compute,
          .isDynamic = false,
        },
      };
      groupLayout.textures = {
        {
          .name = "srcTexture",
          .binding = 2,
          .visibility = rendering::BindingVisibility::BindingVisibility_Compute,
        },
      };
      groupLayout.storageTextures.reserve(kMaxMipLevelsPerPass);
      for (uint32_t mip = 0u; mip < kMaxMipLevelsPerPass; ++mip)
      {
        groupLayout.storageTextures.push_back({
          .name = "mipOut" + std::to_string(mip),
          .binding = 3u + mip,
          .visibility = rendering::BindingVisibility::BindingVisibility_Compute,
        });
      }

      depthPyramidLayout = renderGraph->createBindingsLayout(
          rendering::BindingsLayoutInfo{
            .name = passName + "_depthPyramidLayout.layout",
            .groups = {groupLayout},
          });

      const uint32_t fallbackMip = resolvedMipCount - 1u;
      if (hasSourceDepthPlan)
      {
        rendering::GroupInfo groupInfo{
          .name = "Group0",
          .buffers =
              {
                {
                  .bufferView =
                      {
                        .buffer = constantsBuffer,
                        .offset = 0,
                        .size = alignedConstantsSize,
                        .access = rendering::AccessPattern::SHADER_READ,
                      },
                  .binding = 0,
                },
                {
                  .bufferView =
                      {
                        .buffer = counterBuffer,
                        .offset = 0,
                        .size = sizeof(uint32_t),
                        .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
                      },
                  .binding = 1,
                },
              },
          .textures =
              {
                {
                  .textureView =
                      {
                        .texture = inputDepthTexture,
                        .index = 0,
                        .flags = rendering::ImageAspectFlags::Depth,
                        .baseMipLevel = 0,
                        .levelCount = 1,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                        .access = rendering::AccessPattern::SHADER_READ,
                        .layout = rendering::ResourceLayout::DEPTH_STENCIL_READ_ONLY,
                      },
                  .binding = 2,
                },
              },
        };

        groupInfo.storageTextures.reserve(kMaxMipLevelsPerPass);
        for (uint32_t mip = 0u; mip < kMaxMipLevelsPerPass; ++mip)
        {
          const uint32_t boundMip = std::min(sourceDepthPlan.dstBaseMip + mip, fallbackMip);
          groupInfo.storageTextures.push_back({
            .binding = 3u + mip,
            .textureView =
                {
                  .texture = outputHZBTexture,
                  .index = 0,
                  .flags = rendering::ImageAspectFlags::Color,
                  .baseMipLevel = boundMip,
                  .levelCount = 1,
                  .baseArrayLayer = 0,
                  .layerCount = 1,
                  .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
                  .layout = rendering::ResourceLayout::GENERAL,
                },
          });
        }

        sourceDepthPyramidBindingGroup = renderGraph->createBindingGroups(
            rendering::BindingGroupsInfo{
              .layout = depthPyramidLayout,
              .name = passName + "_SourceDepthPyramidBindingGroup",
              .groups = {groupInfo},
            });

        const std::string sourceDepthShaderPath = "assets/shaders/spirv/hzb-depth-pyramid-source-depth-mips" + std::to_string(kMaxMipLevelsPerPass) + ".spirv";
        auto sourceDepthShaderSrc = os::io::readRelativeFile(sourceDepthShaderPath);
        sourceDepthPyramidShader = renderGraph->createShader(
            rendering::ShaderInfo{
              .name = passName + "_SourceDepthPyramid.shader",
              .layout = depthPyramidLayout,
              .src = sourceDepthShaderSrc,
              .type = rendering::ShaderType::SpirV,
            });

        sourceDepthPyramidPipeline = renderGraph->createComputePipeline(
            rendering::ComputePipelineInfo{
              .entry = "depth_pyramid_spd_chunked_source_depth",
              .layout = depthPyramidLayout,
              .name = passName + "_SourceDepthPyramidSPD.pipeline",
              .shader = sourceDepthPyramidShader,
            });
      }

      if (!dispatchPlans.empty())
      {
        const std::string shaderPath = "assets/shaders/spirv/hzb-depth-pyramid-mips" + std::to_string(kMaxMipLevelsPerPass) + ".spirv";
        auto shaderSrc = os::io::readRelativeFile(shaderPath);
        depthPyramidShader = renderGraph->createShader(
            rendering::ShaderInfo{
              .name = passName + "_DepthPyramid.shader",
              .layout = depthPyramidLayout,
              .src = shaderSrc,
              .type = rendering::ShaderType::SpirV,
            });

        depthPyramidPipeline = renderGraph->createComputePipeline(
            rendering::ComputePipelineInfo{
              .entry = "depth_pyramid_spd_chunked",
              .layout = depthPyramidLayout,
              .name = passName + "_DepthPyramidSPD.pipeline",
              .shader = depthPyramidShader,
            });

        depthPyramidBindingGroups.clear();
        depthPyramidBindingGroups.reserve(dispatchPlans.size());
        const size_t firstRegularPlanIndex = hasSourceDepthPlan ? 1u : 0u;
        for (size_t i = 0; i < dispatchPlans.size(); ++i)
        {
          const DispatchPlan &plan = dispatchPlans[i];
          rendering::TextureView srcTextureView{
            .texture = outputHZBTexture,
            .index = 0,
            .flags = rendering::ImageAspectFlags::Color,
            .baseMipLevel = plan.srcMipLevel,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
            .access = rendering::AccessPattern::SHADER_READ,
            .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
          };

          rendering::GroupInfo groupInfo{
            .name = "Group0",
            .buffers =
                {
                  {
                    .bufferView =
                        {
                          .buffer = constantsBuffer,
                          .offset = static_cast<uint64_t>(firstRegularPlanIndex + i) * alignedConstantsSize,
                          .size = alignedConstantsSize,
                          .access = rendering::AccessPattern::SHADER_READ,
                        },
                    .binding = 0,
                  },
                  {
                    .bufferView =
                        {
                          .buffer = counterBuffer,
                          .offset = 0,
                          .size = sizeof(uint32_t),
                          .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
                        },
                    .binding = 1,
                  },
                },
            .textures =
                {
                  {
                    .textureView = srcTextureView,
                    .binding = 2,
                  },
                },
          };

          groupInfo.storageTextures.reserve(kMaxMipLevelsPerPass);
          for (uint32_t mip = 0u; mip < kMaxMipLevelsPerPass; ++mip)
          {
            const uint32_t boundMip = std::min(plan.dstBaseMip + mip, fallbackMip);
            rendering::TextureView dstMipView{
              .texture = outputHZBTexture,
              .index = 0,
              .flags = rendering::ImageAspectFlags::Color,
              .baseMipLevel = boundMip,
              .levelCount = 1,
              .baseArrayLayer = 0,
              .layerCount = 1,
              .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
              .layout = rendering::ResourceLayout::GENERAL,
            };
            groupInfo.storageTextures.push_back({
              .textureView = dstMipView,
              .binding = 3u + mip,
            });
          }

          depthPyramidBindingGroups.push_back(renderGraph->createBindingGroups(
              rendering::BindingGroupsInfo{
                .layout = depthPyramidLayout,
                .name = passName + "_DepthPyramidBindingGroup_" + std::to_string(i),
                .groups = {groupInfo},
              }));
        }
      }
    }

    if (!hasSourceDepthPlan)
    {
      commandBuffer.cmdBindComputePipeline(reductionPipeline);
      commandBuffer.cmdBindBindingGroups(reductionBindingGroup, nullptr, 0);
      commandBuffer.cmdDispatch(
          ceilDiv(outputWidth, kReductionWorkgroupSizeX),
          ceilDiv(outputHeight, kReductionWorkgroupSizeY),
          1u);
    }

    if (hasSourceDepthPlan)
    {
      commandBuffer.cmdBindComputePipeline(sourceDepthPyramidPipeline);
      commandBuffer.cmdBindBindingGroups(sourceDepthPyramidBindingGroup, nullptr, 0);
      commandBuffer.cmdDispatch(sourceDepthPlan.workgroupsX, sourceDepthPlan.workgroupsY, 1u);
    }

    for (size_t i = 0; i < dispatchPlans.size(); ++i)
    {
      commandBuffer.cmdBindComputePipeline(depthPyramidPipeline);
      commandBuffer.cmdBindBindingGroups(depthPyramidBindingGroups[i], nullptr, 0);
      commandBuffer.cmdDispatch(dispatchPlans[i].workgroupsX, dispatchPlans[i].workgroupsY, 1u);
    }
  }

  const rendering::Texture &getHZBTexture() const
  {
    return outputHZBTexture;
  }

  uint32_t getMipCount() const
  {
    return resolvedMipCount;
  }
};

} // namespace gpgpu
} // namespace virtualgeometry
