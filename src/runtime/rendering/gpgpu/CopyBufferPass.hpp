#pragma once
#include "rendering/gpu/RenderGraph.hpp"

namespace rendering
{
namespace gpgpu
{

class CopyBufferPass : public Pass
{
  rendering::BufferView bufferViewA;
  rendering::BufferView bufferViewB;

public:


  CopyBufferPass(const Buffer &bufferA, uint64_t offsetA, uint64_t sizeA, const Buffer &bufferB, uint64_t offsetB, uint64_t sizeB)
  {
    bufferViewA = BufferView{
      .buffer = bufferA,
      .offset = offsetA,
      .size = sizeA,
      .access = AccessPattern::TRANSFER_READ,
    };
    bufferViewB = BufferView{
      .buffer = bufferB,
      .offset = offsetB,
      .size = sizeB,
      .access = AccessPattern::TRANSFER_WRITE,
    };
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    commandBuffer.cmdCopyBuffer(bufferViewA, bufferViewB);
  }
};

} // namespace gpgpu
} // namespace rendering
