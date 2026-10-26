#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace voxel::svraster::rendering
{

class SparseVoxelResolvePass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer instances;
    ::rendering::Buffer centersLengths;
    ::rendering::Buffer projectedVoxels;
    ::rendering::Buffer candidates;
    ::rendering::Buffer tileRanges;
    ::rendering::Buffer resolvedSurfaces;
    ::rendering::Buffer coverage;
    ::rendering::Buffer linearDepth;
    // Slim bucket entries. When unset the pass falls back to reading full
    // TileCandidate records out of `candidates`, which is what the legacy
    // emit/sort chain produces.
    ::rendering::Buffer candidateIndices;
    // Zeroed for ordinary scene voxels, which have no material-field region.
    ::rendering::Buffer materialSampleAddresses;
  };

  struct Settings
  {
    SparseVoxelRasterUniformsGPU uniforms{};
    uint32_t instanceCapacity = 1u;
    uint32_t voxelCapacity = 1u;
    uint32_t projectedCapacity = 1u;
    uint32_t candidateCapacity = 1u;
    // Minimum projected pixel area for a candidate to be considered. Only the
    // pre-phase sets this; see the note in the shader.
    uint32_t minimumOccluderArea = 0u;
  };

  SparseVoxelResolvePass(const Buffers buffers, const Settings settings) : buffers_(buffers), settings_(settings)
  {
  }

  void updateUniforms(const SparseVoxelRasterUniformsGPU &uniforms)
  {
    settings_.uniforms = uniforms;
    if (uniformBuffer_.isValid())
      renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(settings_.uniforms), &settings_.uniforms);
    writeControls();
  }

  ~SparseVoxelResolvePass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (bindingGroups_.isValid())
      renderGraph->deleteBindingGroups(bindingGroups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
    if (controlsBuffer_.isValid())
      renderGraph->deleteBuffer(controlsBuffer_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(settings_.uniforms), &settings_.uniforms);
    writeControls();
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    // One 8x8 workgroup resolves multiple fine candidate buckets.
    constexpr uint32_t kResolveWorkgroupPixels = 8u;
    static_assert(
        kResolveWorkgroupPixels % kSparseVoxelTileSize == 0u,
        "resolve workgroup must contain an integer number of tile buckets");
    constexpr uint32_t kBucketsPerWorkgroupAxis =
        kResolveWorkgroupPixels / kSparseVoxelTileSize;
    commandBuffer.cmdDispatch(
        std::max(
            1u, (settings_.uniforms.viewportTiles[2] +
                 kBucketsPerWorkgroupAxis - 1u) /
                    kBucketsPerWorkgroupAxis),
        std::max(
            1u, (settings_.uniforms.viewportTiles[3] +
                 kBucketsPerWorkgroupAxis - 1u) /
                    kBucketsPerWorkgroupAxis),
        1u);
  }

private:
  void writeControls()
  {
    if (!controlsBuffer_.isValid())
      return;
    const std::array<uint32_t, 4> controls{
      settings_.minimumOccluderArea, buffers_.candidateIndices.isValid() ? 1u : 0u, 0u, 0u};
    renderGraph->bufferWrite(controlsBuffer_, 0u, sizeof(controls), const_cast<uint32_t *>(controls.data()));
  }

  static uint64_t capacityBytes(const uint32_t count, const uint64_t stride)
  {
    return std::max<uint64_t>(1u, count) * stride;
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniformBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Uniforms.buffer", .size = sizeof(SparseVoxelRasterUniformsGPU), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push, .frameLocal = true});
    controlsBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Controls.buffer", .size = sizeof(uint32_t) * 4u, .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push, .frameLocal = true});
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "uniforms", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "instances", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "centersLengths", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "projectedVoxels", .binding = 3u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "candidates", .binding = 4u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "tileRanges", .binding = 5u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "resolvedSurfaces", .binding = 6u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "coverage", .binding = 7u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "linearDepth", .binding = 8u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "resolveControls", .binding = 9u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "candidateIndices", .binding = 10u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             {.name = "materialSampleAddresses", .binding = 11u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
           }}}});

    const uint32_t pixelCapacity = std::max(1u, settings_.uniforms.viewportTiles[0] * settings_.uniforms.viewportTiles[1]);
    const auto view = [](const ::rendering::Buffer buffer, const uint64_t size, const ::rendering::AccessPattern access)
    {
      return ::rendering::BufferView{.buffer = buffer, .offset = 0u, .size = size, .access = access};
    };
    bindingGroups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u, .bufferView = view(uniformBuffer_, sizeof(SparseVoxelRasterUniformsGPU), ::rendering::AccessPattern::UNIFORM_READ)},
             {.binding = 1u, .bufferView = view(buffers_.instances, capacityBytes(settings_.instanceCapacity, sizeof(SparseVoxelInstanceGPU)), ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 2u, .bufferView = view(buffers_.centersLengths, capacityBytes(settings_.voxelCapacity, sizeof(SparseVoxelCenterLengthGPU)), ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 3u, .bufferView = view(buffers_.projectedVoxels, capacityBytes(settings_.projectedCapacity, sizeof(ProjectedVoxelGPU)), ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 4u, .bufferView = view(buffers_.candidates, capacityBytes(settings_.candidateCapacity, sizeof(TileCandidateGPU)), ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 5u, .bufferView = view(buffers_.tileRanges, capacityBytes(settings_.uniforms.tileRangeCapacity, sizeof(SparseVoxelTileRangeGPU)), ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 6u, .bufferView = view(buffers_.resolvedSurfaces, capacityBytes(pixelCapacity, sizeof(ResolvedVoxelSurfaceGPU)), ::rendering::AccessPattern::SHADER_WRITE)},
             {.binding = 7u, .bufferView = view(buffers_.coverage, capacityBytes(pixelCapacity, sizeof(uint32_t)), ::rendering::AccessPattern::SHADER_WRITE)},
             {.binding = 8u, .bufferView = view(buffers_.linearDepth, capacityBytes(pixelCapacity, sizeof(float)), ::rendering::AccessPattern::SHADER_WRITE)},
             {.binding = 9u, .bufferView = view(controlsBuffer_, sizeof(uint32_t) * 4u, ::rendering::AccessPattern::UNIFORM_READ)},
             // The legacy record path leaves this unset. The binding still has
             // to resolve to a real allocation, so it aliases the candidate
             // records the shader reads instead; resolveControls.y keeps the
             // shader off it.
             {.binding = 10u,
              .bufferView = view(
                  buffers_.candidateIndices.isValid() ? buffers_.candidateIndices : buffers_.candidates,
                  capacityBytes(settings_.candidateCapacity, sizeof(uint32_t)),
                  ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 11u,
              .bufferView = view(
                  buffers_.materialSampleAddresses,
                  capacityBytes(
                      pixelCapacity, sizeof(VoxelMaterialSampleAddressGPU)),
                  ::rendering::AccessPattern::SHADER_WRITE)},
           }}}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader", .layout = layout_, .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-resolve.spirv"), .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline({.entry = "resolveSolidVoxels", .layout = layout_, .name = passName + "_Pipeline", .shader = shader_});
  }

  Buffers buffers_{};
  Settings settings_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::Buffer controlsBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups bindingGroups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
