#pragma once

#include <algorithm>
#include <vector>

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

namespace virtualgeometry
{
namespace gpgpu
{

class VirtualGeometryPageDecodePass : public rendering::Pass
{
public:
  explicit VirtualGeometryPageDecodePass(VirtualGeometryScene &scene) : scene_(scene)
  {
  }

  ~VirtualGeometryPageDecodePass() override
  {
    renderGraph->deleteComputePipeline(pipeline_);
    renderGraph->deleteShader(shader_);
    renderGraph->deleteBindingGroups(bindingGroups_);
    renderGraph->deleteBindingsLayout(layout_);
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
          .name = passName + "_Layout",
          .groups = {rendering::BindingGroupLayout{
              .buffers =
                  {
                      {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "pageDecodeRequests", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "pageTable", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "pagesBuffer", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "decodedClusterCullBuffer", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "decodedClusterGeometryBuffer", .binding = 5, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "decodedVertexBuffer", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "decodedIndexBuffer", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                      {.name = "decodedBoneWeightBuffer", .binding = 8, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                  },
          }},
        });

    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_BindingGroups",
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers =
                    {
                        {.binding = 0, .bufferView = {.buffer = scene_.pageDecodeUniformBuffer, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = scene_.pageDecodeUniformBufferSize}},
                        {.binding = 1, .bufferView = {.buffer = scene_.pageDecodeRequestBuffer, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = scene_.pageDecodeRequestBufferSize}},
                        {.binding = 2, .bufferView = {.buffer = scene_.pageTableBuffer, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = scene_.pagesTableBufferSize}},
                        {.binding = 3, .bufferView = {.buffer = scene_.pagesBuffer, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = scene_.pagesBufferSize}},
                        {.binding = 4, .bufferView = {.buffer = scene_.decodedClusterCullBuffer, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE, .offset = 0, .size = scene_.decodedClusterCullBufferSize}},
                        {.binding = 5, .bufferView = {.buffer = scene_.decodedClusterGeometryBuffer, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE, .offset = 0, .size = scene_.decodedClusterGeometryBufferSize}},
                        {.binding = 6, .bufferView = {.buffer = scene_.decodedVertexBuffer, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE, .offset = 0, .size = scene_.decodedVertexBufferSize}},
                        {.binding = 7, .bufferView = {.buffer = scene_.decodedIndexBuffer, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE, .offset = 0, .size = scene_.decodedIndexBufferSize}},
                        {.binding = 8, .bufferView = {.buffer = scene_.decodedBoneWeightBuffer, .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE, .offset = 0, .size = scene_.decodedBoneWeightBufferSize}},
                    },
            }},
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
          .name = passName + "_Shader",
          .layout = layout_,
          .src = os::io::readRelativeFile("assets/shaders/spirv/virtualgeometry-page-decode-cs.spirv"),
          .type = rendering::ShaderType::SpirV,
        });

    pipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
          .entry = "decodePages",
          .layout = layout_,
          .name = passName + "_Pipeline",
          .shader = shader_,
        });

    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatchIndirect(scene_.pageDecodeDispatchIndirectBuffer, 0u);
  }

private:
  VirtualGeometryScene &scene_;
  rendering::BindingsLayout layout_;
  rendering::BindingGroups bindingGroups_;
  rendering::Shader shader_;
  rendering::ComputePipeline pipeline_;
};

} // namespace gpgpu
} // namespace virtualgeometry
