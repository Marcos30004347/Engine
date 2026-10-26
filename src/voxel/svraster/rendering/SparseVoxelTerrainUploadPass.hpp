#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace voxel::svraster::rendering
{

class SparseVoxelTerrainUploadPass final : public ::rendering::Pass
{
public:
  static constexpr uint32_t kBufferDestinationCount = 25u;
  static constexpr uint32_t kDestinationCount = 26u;
  // Default terrain movement changes many independently allocated page ranges.
  // 1024 records still keep SettingsGPU below the portable 64 KiB uniform
  // limit, while the shader resolves them with a logarithmic search.
  static constexpr uint32_t kMaximumSections = 1024u;

  enum class Destination : uint32_t
  {
    BrickNodes = 0u,
    BrickOccupancy = 1u,
    BrickMaterials = 2u,
    BrickPalette = 3u,
    BrickPreviouslyVisible = 4u,
    Tree64Memberships = 5u,
    Tree64References = 6u,
    Tree64Instances = 7u,
    Tree64Models = 8u,
    Tree64Nodes = 9u,
    Tree64NodesPadded = 10u,
    BrickPaletteDescriptors = 11u,
    BrickMicroSurfaces = 12u,
    MaterialFieldResolvers = 13u,
    MaterialFieldMappings = 14u,
    GeometryFieldCache = 15u,
    Tree64MaterialCells = 16u,
    TerrainPageStates = 17u,
    // Immutable physical-page pools. These destinations deliberately are not
    // consumed by the compatibility trace. A later descriptor publication is
    // the only operation allowed to make one of these ranges reachable.
    Tree64PageNodes = 18u,
    Tree64PageNodesPadded = 19u,
    Tree64PageMaterialCells = 20u,
    Tree64NodePageGenerations = 21u,
    Tree64MaterialPageGenerations = 22u,
    BrickChildIndices = 23u,
    BrickRootIndices = 24u,
    MaterialFieldAtlas = 25u,
  };

  struct alignas(16) SectionGPU
  {
    uint32_t stagingWordOffset = 0u;
    uint32_t destinationWordOffset = 0u;
    uint32_t wordCount = 0u;
    uint32_t destination = 0u;
  };

  struct alignas(16) SettingsGPU
  {
    uint32_t uploadWords = 0u;
    uint32_t sectionCount = 0u;
    uint32_t generation = 0u;
    uint32_t atlasWidth = 1u;
    uint32_t publicationCount = 0u;
    uint32_t freezePublication = 0u;
    uint32_t nodesPerPage = 0u;
    uint32_t materialCellsPerPage = 0u;
    uint32_t reserved0 = 0u;
    uint32_t reserved1 = 0u;
    uint32_t reserved2 = 0u;
    uint32_t reserved3 = 0u;
    std::array<SectionGPU, kMaximumSections> sections{};
  };

  struct Buffers
  {
    ::rendering::Buffer staging;
    std::array<::rendering::Buffer, kBufferDestinationCount> destinations{};
    ::rendering::Texture materialFieldAtlas;
    ::rendering::Buffer terrainTree64Descriptors;
    // Phase 5 may generate publication records entirely on the GPU after
    // validating and relocating matching raster/Tree64 build results. An
    // invalid handle retains the CPU-uploaded compatibility path.
    ::rendering::Buffer terrainPublicationRecords;
  };

  struct Capacities
  {
    uint64_t stagingBytes = 0u;
    std::array<uint64_t, kBufferDestinationCount> destinationBytes{};
    uint64_t terrainTree64DescriptorBytes = 0u;
    uint32_t publicationRecordCount = 1u;
  };

  SparseVoxelTerrainUploadPass(
      const Buffers buffers, const Capacities capacities)
      : buffers_(buffers), capacities_(capacities)
  {
    if (!buffers_.staging.isValid() || capacities_.stagingBytes < sizeof(uint32_t))
      throw std::invalid_argument("Terrain upload staging buffer is invalid");
    for (uint32_t index = 0u; index < kBufferDestinationCount; ++index)
      if (!buffers_.destinations[index].isValid() ||
          capacities_.destinationBytes[index] < sizeof(uint32_t))
        throw std::invalid_argument("Terrain upload destination is invalid");
    if (!buffers_.materialFieldAtlas.isValid())
      throw std::invalid_argument("Terrain material-field atlas is invalid");
    if (buffers_.terrainTree64Descriptors.isValid() !=
        (capacities_.terrainTree64DescriptorBytes != 0u))
      throw std::invalid_argument(
          "Terrain Tree64 publication descriptor capacity is invalid");
    if (capacities_.publicationRecordCount == 0u)
      throw std::invalid_argument(
          "Terrain publication record capacity is invalid");
  }

  ~SparseVoxelTerrainUploadPass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (validationPipeline_.isValid())
      renderGraph->deleteComputePipeline(validationPipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (indirectArgs_.isValid()) renderGraph->deleteBuffer(indirectArgs_);
    if (uniforms_.isValid()) renderGraph->deleteBuffer(uniforms_);
    if (publicationRecords_.isValid())
      renderGraph->deleteBuffer(publicationRecords_);
    if (publicationStatus_.isValid())
      renderGraph->deleteBuffer(publicationStatus_);
    if (publicationStatusReadback_.isValid())
      renderGraph->deleteBuffer(publicationStatusReadback_);
    if (terrainTree64DescriptorsFallback_.isValid())
      renderGraph->deleteBuffer(terrainTree64DescriptorsFallback_);
  }

  bool update(
      const SettingsGPU &settings,
      const SparseVoxelTerrainPublicationRecordGPU *publicationRecords =
          nullptr)
  {
    if (settings.sectionCount > kMaximumSections ||
        settings.publicationCount > capacities_.publicationRecordCount ||
        (settings.publicationCount != 0u && publicationRecords == nullptr &&
         !buffers_.terrainPublicationRecords.isValid()) ||
        uint64_t{settings.uploadWords} * sizeof(uint32_t) >
            capacities_.stagingBytes)
      return false;
    ensureResources();
    settings_ = settings;
    publicationRecordsCPU_.clear();
    if (settings.publicationCount != 0u && publicationRecords != nullptr)
      publicationRecordsCPU_.assign(
          publicationRecords,
          publicationRecords + settings.publicationCount);
    const uint64_t groups = (uint64_t{settings.uploadWords} + 63u) / 64u;
    const uint64_t publicationGroups =
        (uint64_t{settings.publicationCount} + 63u) / 64u;
    const uint64_t workGroups = std::max(groups, publicationGroups);
    constexpr uint32_t kMaximumDispatchDimension = 65535u;
    const uint32_t x = static_cast<uint32_t>(
        std::min<uint64_t>(workGroups, kMaximumDispatchDimension));
    const uint32_t y = x == 0u
        ? 0u
        : static_cast<uint32_t>((workGroups + x - 1u) / x);
    if (y > kMaximumDispatchDimension)
      return false;
    const IndirectDispatchArgsGPU args{x, y, 1u, 0u};
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(settings_), &settings_);
    renderGraph->bufferWrite(
        indirectArgs_, 0u, sizeof(args),
        const_cast<IndirectDispatchArgsGPU *>(&args));
    if (settings.publicationCount != 0u && publicationRecords != nullptr)
      renderGraph->bufferWrite(
          activePublicationRecords(), 0u,
          uint64_t{settings.publicationCount} *
              sizeof(SparseVoxelTerrainPublicationRecordGPU),
          publicationRecordsCPU_.data());
    return true;
  }

  bool readPublicationStatus(
      SparseVoxelTerrainPublicationStatusGPU &status)
  {
    if (!publicationStatus_.isValid())
      return false;
    renderGraph->bufferRead(
        publicationStatusReadback_, 0u, sizeof(status),
        [&](const void *data) { std::memcpy(&status, data, sizeof(status)); });
    return true;
  }

  ::rendering::Buffer publicationStatus() const
  {
    return publicationStatus_;
  }

  ::rendering::Buffer publicationRecords() const
  {
    return buffers_.terrainPublicationRecords.isValid()
        ? buffers_.terrainPublicationRecords : publicationRecords_;
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    commandBuffer.cmdBindComputePipeline(validationPipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatch(1u, 1u, 1u);
    commandBuffer.cmdMemoryBarrier();
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatchIndirect(indirectArgs_, 0u);
    commandBuffer.cmdMemoryBarrier(
        ::rendering::PipelineStage::COMPUTE_SHADER,
        ::rendering::PipelineStage::TRANSFER,
        ::rendering::AccessPattern::SHADER_WRITE,
        ::rendering::AccessPattern::TRANSFER_READ);
    commandBuffer.cmdCopyBuffer(
        view(
            publicationStatus_,
            sizeof(SparseVoxelTerrainPublicationStatusGPU),
            ::rendering::AccessPattern::TRANSFER_READ),
        view(
            publicationStatusReadback_,
            sizeof(SparseVoxelTerrainPublicationStatusGPU),
            ::rendering::AccessPattern::TRANSFER_WRITE));
  }

private:
  static ::rendering::BufferView view(
      const ::rendering::Buffer buffer, const uint64_t size,
      const ::rendering::AccessPattern access)
  {
    return {
      .buffer = buffer,
      .offset = 0u,
      .size = std::max<uint64_t>(size, sizeof(uint32_t)),
      .access = access,
    };
  }

  static ::rendering::TextureView atlasView(
      const ::rendering::Texture texture)
  {
    return {
      .texture = texture,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Color,
      .baseMipLevel = 0u,
      .levelCount = 1u,
      .baseArrayLayer = 0u,
      .layerCount = 1u,
      .access = ::rendering::AccessPattern::SHADER_WRITE,
      .layout = ::rendering::ResourceLayout::GENERAL,
    };
  }

  void ensureResources()
  {
    if (layout_.isValid()) return;
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(SettingsGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
               ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    indirectArgs_ = renderGraph->createBuffer({
      .name = passName + "_IndirectArgs",
      .size = sizeof(IndirectDispatchArgsGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Indirect |
               ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    if (!buffers_.terrainPublicationRecords.isValid())
      publicationRecords_ = renderGraph->createBuffer({
        .name = passName + "_PublicationRecords",
        .size = uint64_t{capacities_.publicationRecordCount} *
            sizeof(SparseVoxelTerrainPublicationRecordGPU),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
                 ::rendering::BufferUsage::BufferUsage_Push,
      });
    publicationStatus_ = renderGraph->createBuffer({
      .name = passName + "_PublicationStatus",
      .size = sizeof(SparseVoxelTerrainPublicationStatusGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage |
               ::rendering::BufferUsage::BufferUsage_Push |
               ::rendering::BufferUsage::BufferUsage_CopySrc,
    });
    publicationStatusReadback_ = renderGraph->createBuffer({
      .name = passName + "_PublicationStatusReadback",
      .size = sizeof(SparseVoxelTerrainPublicationStatusGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Pull |
               ::rendering::BufferUsage::BufferUsage_CopyDst,
    });
    if (!buffers_.terrainTree64Descriptors.isValid())
    {
      terrainTree64DescriptorsFallback_ = renderGraph->createBuffer({
        .name = passName + "_EmptyTerrainTree64Descriptors",
        .size = sizeof(SparseVoxelTerrainTree64DescriptorGPU),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
                 ::rendering::BufferUsage::BufferUsage_Push,
      });
      const SparseVoxelTerrainTree64DescriptorGPU empty{};
      renderGraph->bufferWrite(
          terrainTree64DescriptorsFallback_, 0u, sizeof(empty),
          const_cast<SparseVoxelTerrainTree64DescriptorGPU *>(&empty));
    }
    const ::rendering::Buffer terrainTree64Descriptors =
        buffers_.terrainTree64Descriptors.isValid()
        ? buffers_.terrainTree64Descriptors
        : terrainTree64DescriptorsFallback_;
    const uint64_t terrainTree64DescriptorBytes =
        buffers_.terrainTree64Descriptors.isValid()
        ? capacities_.terrainTree64DescriptorBytes
        : sizeof(SparseVoxelTerrainTree64DescriptorGPU);
    std::vector<::rendering::BindingGroupLayoutBufferEntry> bindings;
    bindings.reserve(kBufferDestinationCount + 5u);
    for (uint32_t binding = 0u; binding < 12u; ++binding)
      bindings.push_back({
        .name = binding == 0u ? "staging" : "destination",
        .binding = binding,
        .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
        .isDynamic = false,
      });
    bindings.push_back({
      .name = "brickPaletteDescriptors",
      .binding = 13u,
      .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
      .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
      .isDynamic = false,
    });
    bindings.push_back({
      .name = "brickMicroSurfaces",
      .binding = 14u,
      .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
      .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
      .isDynamic = false,
    });
    for (uint32_t binding = 15u;
         binding <= kBufferDestinationCount + 1u; ++binding)
      bindings.push_back({
        .name = "terrainPublicationState",
        .binding = binding,
        .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
        .isDynamic = false,
      });
    bindings.push_back({
      .name = "settings",
      .binding = 12u,
      .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
      .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
      .isDynamic = false,
    });
    for (const auto &[name, binding] : std::array{
           std::pair{"terrainTree64Descriptors", 28u},
           std::pair{"terrainPublicationRecords", 29u},
           std::pair{"terrainPublicationStatus", 30u},
         })
      bindings.push_back({
        .name = name,
        .binding = binding,
        .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
        .isDynamic = false,
      });
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers = bindings,
        .storageTextures = {{
          .name = "materialFieldAtlas",
          .binding = kBufferDestinationCount + 2u,
          .visibility =
              ::rendering::BindingVisibility::BindingVisibility_Compute,
        }},
      }},
    });
    std::vector<::rendering::BindingBuffer> groupBuffers;
    groupBuffers.reserve(kBufferDestinationCount + 4u);
    groupBuffers.push_back({
      .bufferView = view(
          buffers_.staging, capacities_.stagingBytes,
          ::rendering::AccessPattern::SHADER_READ),
      .binding = 0u,
    });
    for (uint32_t index = 0u; index < 11u; ++index)
      groupBuffers.push_back({
        .bufferView = view(
            buffers_.destinations[index],
            capacities_.destinationBytes[index],
            ::rendering::AccessPattern::SHADER_WRITE),
        .binding = index + 1u,
      });
    groupBuffers.push_back({
      .bufferView = view(
          buffers_.destinations[11u], capacities_.destinationBytes[11u],
          ::rendering::AccessPattern::SHADER_WRITE),
      .binding = 13u,
    });
    groupBuffers.push_back({
      .bufferView = view(
          buffers_.destinations[12u], capacities_.destinationBytes[12u],
          ::rendering::AccessPattern::SHADER_WRITE),
      .binding = 14u,
    });
    for (uint32_t index = 13u; index < kBufferDestinationCount; ++index)
      groupBuffers.push_back({
        .bufferView = view(
            buffers_.destinations[index],
            capacities_.destinationBytes[index],
            index == static_cast<uint32_t>(Destination::TerrainPageStates) ||
                    index == static_cast<uint32_t>(
                        Destination::Tree64NodePageGenerations) ||
                    index == static_cast<uint32_t>(
                        Destination::Tree64MaterialPageGenerations)
                ? ::rendering::AccessPattern::SHADER_READ_WRITE
                : ::rendering::AccessPattern::SHADER_WRITE),
        .binding = index + 2u,
      });
    groupBuffers.push_back({
      .bufferView = view(
          uniforms_, sizeof(SettingsGPU),
          ::rendering::AccessPattern::UNIFORM_READ),
      .binding = 12u,
    });
    groupBuffers.push_back({
      .bufferView = view(
          terrainTree64Descriptors, terrainTree64DescriptorBytes,
          ::rendering::AccessPattern::SHADER_READ_WRITE),
      .binding = 28u,
    });
    groupBuffers.push_back({
      .bufferView = view(
          activePublicationRecords(),
          uint64_t{capacities_.publicationRecordCount} *
              sizeof(SparseVoxelTerrainPublicationRecordGPU),
          ::rendering::AccessPattern::SHADER_READ),
      .binding = 29u,
    });
    groupBuffers.push_back({
      .bufferView = view(
          publicationStatus_,
          sizeof(SparseVoxelTerrainPublicationStatusGPU),
          ::rendering::AccessPattern::SHADER_READ_WRITE),
      .binding = 30u,
    });
    groups_ = renderGraph->createBindingGroups({
      .name = passName + "_Groups",
      .layout = layout_,
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = groupBuffers,
        .storageTextures = {{
          .textureView = atlasView(buffers_.materialFieldAtlas),
          .binding = kBufferDestinationCount + 2u,
        }},
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/sparse-voxel-terrain-upload.spirv"),
      .type = ::rendering::ShaderType::SpirV,
      .layout = layout_,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .name = passName + "_Pipeline",
      .shader = shader_,
      .entry = "uploadTerrainPages",
      .layout = layout_,
    });
    validationPipeline_ = renderGraph->createComputePipeline({
      .name = passName + "_ValidationPipeline",
      .shader = shader_,
      .entry = "validateTerrainPublication",
      .layout = layout_,
    });
  }

  Buffers buffers_{};
  Capacities capacities_{};
  SettingsGPU settings_{};
  std::vector<SparseVoxelTerrainPublicationRecordGPU>
      publicationRecordsCPU_;
  ::rendering::Buffer uniforms_{};
  ::rendering::Buffer indirectArgs_{};
  ::rendering::Buffer publicationRecords_{};
  ::rendering::Buffer publicationStatus_{};
  ::rendering::Buffer publicationStatusReadback_{};
  ::rendering::Buffer terrainTree64DescriptorsFallback_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
  ::rendering::ComputePipeline validationPipeline_{};

  ::rendering::Buffer activePublicationRecords() const
  {
    return buffers_.terrainPublicationRecords.isValid()
        ? buffers_.terrainPublicationRecords : publicationRecords_;
  }
};

static_assert(sizeof(SparseVoxelTerrainUploadPass::SectionGPU) == 16u);
static_assert(sizeof(SparseVoxelTerrainUploadPass::SettingsGPU) == 16432u);

} // namespace voxel::svraster::rendering
