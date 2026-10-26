#pragma once

#include <cstdint>

#include "rendering/gpu/RenderGraph.hpp"

namespace rendering
{
namespace gpgpu
{

class TextureCopyPass : public rendering::Pass
{
public:
  enum class Aspect : uint32_t
  {
    Color,
    Depth,
  };

  TextureCopyPass(rendering::Texture sourceTexture, rendering::Texture destinationTexture, Aspect aspect)
      : sourceTexture_(sourceTexture), destinationTexture_(destinationTexture), aspect_(aspect)
  {
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    commandBuffer.cmdCopyImage(makeTransferSrcView(sourceTexture_, aspect_), makeTransferDstView(destinationTexture_, aspect_));
  }

private:
  static rendering::TextureView makeTransferSrcView(rendering::Texture texture, Aspect aspect)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::TRANSFER_READ,
        .layout = rendering::ResourceLayout::TRANSFER_SRC,
        .index = 0u,
        .flags = aspect == Aspect::Depth ? rendering::ImageAspectFlags::Depth : rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  static rendering::TextureView makeTransferDstView(rendering::Texture texture, Aspect aspect)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::TRANSFER_WRITE,
        .layout = rendering::ResourceLayout::TRANSFER_DST,
        .index = 0u,
        .flags = aspect == Aspect::Depth ? rendering::ImageAspectFlags::Depth : rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  rendering::Texture sourceTexture_{};
  rendering::Texture destinationTexture_{};
  Aspect aspect_ = Aspect::Color;
};

} // namespace gpgpu
} // namespace rendering
