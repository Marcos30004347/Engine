#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

namespace virtualgeometry
{
namespace gpgpu
{

class VirtualGeometryStreamingPageSelectionPass : public rendering::Pass
{
public:
  explicit VirtualGeometryStreamingPageSelectionPass(VirtualGeometryScene &scene) : scene_(scene)
  {
  }

  ~VirtualGeometryStreamingPageSelectionPass() override
  {
    renderGraph->deleteComputePipeline(pipeline_);
    renderGraph->deleteShader(shader_);
    renderGraph->deleteBindingGroups(rawToScratchABindingGroups_);
    renderGraph->deleteBindingGroups(rawToInstallBindingGroups_);
    renderGraph->deleteBindingGroups(rawToEvictBindingGroups_);
    renderGraph->deleteBindingGroups(scratchAToScratchBBindingGroups_);
    renderGraph->deleteBindingGroups(scratchBToScratchABindingGroups_);
    renderGraph->deleteBindingGroups(scratchAToInstallBindingGroups_);
    renderGraph->deleteBindingGroups(scratchBToInstallBindingGroups_);
    renderGraph->deleteBindingGroups(scratchAToEvictBindingGroups_);
    renderGraph->deleteBindingGroups(scratchBToEvictBindingGroups_);
    renderGraph->deleteBindingsLayout(layout_);
    renderGraph->deleteBuffer(uniformBuffer_);
    renderGraph->deleteBuffer(scratchCandidatesA_);
    renderGraph->deleteBuffer(scratchCandidatesB_);
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    const uint32_t uniformAlignment = renderGraph->getRHI()->GetProperties().uniformBufferAlignment;
    const uint32_t maxReductionPasses = reductionPassCount(std::max(1u, scene_.getMaxPagesInScene()));
    const uint32_t alignedUniformSize = alignUp(sizeof(Uniforms), uniformAlignment);
    const uint32_t totalUniformBufferSize = alignedUniformSize * maxReductionPasses * 2u;
    const uint32_t maxScratchCandidates = std::max(1u, divCeil(scene_.getMaxPagesInScene(), kItemsPerGroup) * kTopN);

    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName + "_Uniforms.buffer",
          .size = totalUniformBufferSize,
          .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });

    scratchCandidatesA_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName + "_ScratchA.buffer",
          .size = static_cast<uint64_t>(maxScratchCandidates) * sizeof(StreamingPageCandidate),
          .usage = rendering::BufferUsage::BufferUsage_Storage,
        });

    scratchCandidatesB_ = createFrameLocalBuffer(
        rendering::BufferInfo{
          .name = passName + "_ScratchB.buffer",
          .size = static_cast<uint64_t>(maxScratchCandidates) * sizeof(StreamingPageCandidate),
          .usage = rendering::BufferUsage::BufferUsage_Storage,
        });

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
          .name = passName + "_Layout",
          .groups = {rendering::BindingGroupLayout{
            .buffers =
                {
                  {.name = "uniforms", .binding = 0, .isDynamic = true, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                  {.name = "pageTable", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                  {.name = "pagePriorities",
                   .binding = 2,
                   .isDynamic = false,
                   .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                   .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                  {.name = "inputCandidates",
                   .binding = 3,
                   .isDynamic = false,
                   .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                   .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                  {.name = "outputCandidates",
                   .binding = 4,
                   .isDynamic = false,
                   .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                   .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
          }},
        });

    rawToScratchABindingGroups_ = createBindingGroups(passName + "_RawToScratchA", scratchCandidatesA_, scratchCandidatesA_, maxScratchCandidates);
    rawToInstallBindingGroups_ = createBindingGroups(passName + "_RawToInstall", scratchCandidatesA_, scene_.pageInstallCandidateBuffer, kTopN);
    rawToEvictBindingGroups_ = createBindingGroups(passName + "_RawToEvict", scratchCandidatesA_, scene_.pageEvictCandidateBuffer, kTopN);
    scratchAToScratchBBindingGroups_ = createBindingGroups(passName + "_ScratchAToScratchB", scratchCandidatesA_, scratchCandidatesB_, maxScratchCandidates);
    scratchBToScratchABindingGroups_ = createBindingGroups(passName + "_ScratchBToScratchA", scratchCandidatesB_, scratchCandidatesA_, maxScratchCandidates);
    scratchAToInstallBindingGroups_ = createBindingGroups(passName + "_ScratchAToInstall", scratchCandidatesA_, scene_.pageInstallCandidateBuffer, kTopN);
    scratchBToInstallBindingGroups_ = createBindingGroups(passName + "_ScratchBToInstall", scratchCandidatesB_, scene_.pageInstallCandidateBuffer, kTopN);
    scratchAToEvictBindingGroups_ = createBindingGroups(passName + "_ScratchAToEvict", scratchCandidatesA_, scene_.pageEvictCandidateBuffer, kTopN);
    scratchBToEvictBindingGroups_ = createBindingGroups(passName + "_ScratchBToEvict", scratchCandidatesB_, scene_.pageEvictCandidateBuffer, kTopN);

    const auto shaderSrc = os::io::readRelativeFile("assets/shaders/spirv/streaming-page-selection-cs.spirv");
    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
          .name = passName + "_Shader",
          .layout = layout_,
          .src = shaderSrc,
          .type = rendering::ShaderType::SpirV,
        });

    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
          .entry = "reduceStreamingCandidates",
          .layout = layout_,
          .name = passName + "_Pipeline",
          .shader = shader_,
        });

    commandBuffer.cmdBindComputePipeline(pipeline_);

    uint32_t uniformIndex = 0u;
    auto dispatchReduction = [&](rendering::BindingGroups bindingGroups, const Uniforms &uniforms, uint32_t workgroupCount)
    {
      const uint32_t offset = uniformIndex * alignedUniformSize;
      Uniforms dispatchUniforms = uniforms;
      renderGraph->bufferWrite(uniformBuffer_, offset, sizeof(Uniforms), &dispatchUniforms);
      uint32_t dynamicOffset = offset;
      commandBuffer.cmdBindBindingGroups(bindingGroups, &dynamicOffset, 1);
      commandBuffer.cmdDispatch(workgroupCount, 1, 1);
      uniformIndex++;
    };

    auto runSelection = [&](bool selectInstalled)
    {
      const uint32_t rawInputCount = scene_.nextPageTableSlot;
      const uint32_t selectInstalledFlag = selectInstalled ? 1u : 0u;
      const uint32_t selectLowestPriorityFlag = selectInstalled ? 0u : 1u;
      uint32_t inputCount = rawInputCount;
      uint32_t workgroupCount = std::max(1u, divCeil(inputCount, kItemsPerGroup));

      if (workgroupCount == 1u)
      {
        dispatchReduction(
            selectInstalled ? rawToInstallBindingGroups_ : rawToEvictBindingGroups_,
            Uniforms{
                .inputCount = inputCount,
                .useRawInput = 1u,
                .selectInstalled = selectInstalledFlag,
                .selectLowestPriority = selectLowestPriorityFlag,
            },
            1u);
        return;
      }

      dispatchReduction(
          rawToScratchABindingGroups_,
          Uniforms{
              .inputCount = inputCount,
              .useRawInput = 1u,
              .selectInstalled = selectInstalledFlag,
              .selectLowestPriority = selectLowestPriorityFlag,
          },
          workgroupCount);

      uint32_t reducedCount = workgroupCount * kTopN;
      bool currentBufferIsA = true;
      while (reducedCount > kItemsPerGroup)
      {
        workgroupCount = divCeil(reducedCount, kItemsPerGroup);
        dispatchReduction(
            currentBufferIsA ? scratchAToScratchBBindingGroups_ : scratchBToScratchABindingGroups_,
            Uniforms{
                .inputCount = reducedCount,
                .useRawInput = 0u,
                .selectInstalled = selectInstalledFlag,
                .selectLowestPriority = selectLowestPriorityFlag,
            },
            workgroupCount);
        reducedCount = workgroupCount * kTopN;
        currentBufferIsA = !currentBufferIsA;
      }

      dispatchReduction(
          selectInstalled ? (currentBufferIsA ? scratchAToInstallBindingGroups_ : scratchBToInstallBindingGroups_)
                          : (currentBufferIsA ? scratchAToEvictBindingGroups_ : scratchBToEvictBindingGroups_),
          Uniforms{
              .inputCount = reducedCount,
              .useRawInput = 0u,
              .selectInstalled = selectInstalledFlag,
              .selectLowestPriority = selectLowestPriorityFlag,
          },
          1u);
    };

    runSelection(false);
    runSelection(true);
  }

private:
  static constexpr uint32_t kWorkgroupSize = 128u;
  static constexpr uint32_t kItemsPerGroup = kWorkgroupSize * 2u;
  static constexpr uint32_t kTopN = VirtualGeometryScene::STREAMING_PAGE_SELECTION_COUNT;

  struct Uniforms
  {
    uint32_t inputCount = 0u;
    uint32_t useRawInput = 0u;
    uint32_t selectInstalled = 0u;
    uint32_t selectLowestPriority = 0u;
  };

  static constexpr uint32_t alignUp(uint32_t value, uint32_t alignment)
  {
    return ((value + alignment - 1u) / alignment) * alignment;
  }

  static constexpr uint32_t divCeil(uint32_t value, uint32_t divisor)
  {
    return (value + divisor - 1u) / divisor;
  }

  static constexpr uint32_t reductionPassCount(uint32_t inputCount)
  {
    uint32_t passes = 1u;
    uint32_t currentCount = inputCount;
    while (currentCount > kItemsPerGroup)
    {
      currentCount = divCeil(currentCount, kItemsPerGroup) * kTopN;
      passes++;
    }
    return passes;
  }

  rendering::BindingGroups createBindingGroups(const std::string &name, const rendering::Buffer &inputBuffer, const rendering::Buffer &outputBuffer, uint32_t outputCandidateCapacity)
  {
    return renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = name,
            .groups =
                {rendering::GroupInfo{
                    .name = "group0",
                    .buffers =
                        {
                            {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = sizeof(Uniforms)}},
                            {.binding = 1, .bufferView = {.buffer = scene_.pageTableBuffer, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = scene_.pagesTableBufferSize}},
                            {.binding = 2, .bufferView = {.buffer = scene_.pagePriorityBuffer, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = scene_.pagePriorityBufferSize}},
                            {.binding = 3,
                             .bufferView = {.buffer = inputBuffer,
                                            .access = rendering::AccessPattern::SHADER_READ,
                                            .offset = 0,
                                            .size = std::max<uint64_t>(sizeof(StreamingPageCandidate), static_cast<uint64_t>(maxScratchCandidateCount()) * sizeof(StreamingPageCandidate))}},
                            {.binding = 4,
                             .bufferView = {.buffer = outputBuffer,
                                            .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
                                            .offset = 0,
                                            .size = std::max<uint64_t>(sizeof(StreamingPageCandidate), static_cast<uint64_t>(outputCandidateCapacity) * sizeof(StreamingPageCandidate))}},
                        },
                }},
        });
  }

  uint32_t maxScratchCandidateCount() const
  {
    return std::max(1u, divCeil(scene_.getMaxPagesInScene(), kItemsPerGroup) * kTopN);
  }

  VirtualGeometryScene &scene_;

  rendering::BindingsLayout layout_;
  rendering::BindingGroups rawToScratchABindingGroups_;
  rendering::BindingGroups rawToInstallBindingGroups_;
  rendering::BindingGroups rawToEvictBindingGroups_;
  rendering::BindingGroups scratchAToScratchBBindingGroups_;
  rendering::BindingGroups scratchBToScratchABindingGroups_;
  rendering::BindingGroups scratchAToInstallBindingGroups_;
  rendering::BindingGroups scratchBToInstallBindingGroups_;
  rendering::BindingGroups scratchAToEvictBindingGroups_;
  rendering::BindingGroups scratchBToEvictBindingGroups_;
  rendering::Shader shader_;
  rendering::ComputePipeline pipeline_;
  rendering::Buffer uniformBuffer_;
  rendering::Buffer scratchCandidatesA_;
  rendering::Buffer scratchCandidatesB_;
};

} // namespace gpgpu
} // namespace virtualgeometry
