#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace voxel::svraster::rendering
{

// R9 H0. Counts the screen-tile candidate list every visible brick would
// contribute to a tiled compute raster, and changes nothing else.
//
// The number this exists to produce is `maximumCandidatesPerTile`. The compute
// path has to resolve nearest-wins per pixel with a packed atomic, and on this
// platform that atomic is 32-bit - MoltenVK reports
// shaderBufferInt64Atomics = false on an Apple M4, so a single 64-bit atomicMin
// over (depth | payload) is unavailable. A 32-bit key can carry roughly 20 bits
// of ordering distance and 12 bits of payload, and 12 bits is only enough if no
// tile ever needs more than 4096 candidates. This census answers that before
// the tile kernel is written.
inline constexpr uint32_t kSparseVoxelBrickTileSize = 16u;
inline constexpr uint32_t kSparseVoxelBrickTileSlotBits = 12u;
inline constexpr uint32_t kSparseVoxelBrickTileSlotCapacity =
    1u << kSparseVoxelBrickTileSlotBits;
inline constexpr uint32_t kSparseVoxelBrickTileHistogramBuckets = 13u;

struct alignas(16) SparseVoxelBrickTileBinStatsGPU
{
  uint32_t binnedBricks = 0u;
  uint32_t tileReferences = 0u;
  uint32_t maximumTilesPerBrick = 0u;
  uint32_t maximumCandidatesPerTile = 0u;
  uint32_t nonEmptyTiles = 0u;
  uint32_t overflowTiles = 0u;
  std::array<uint32_t, kSparseVoxelBrickTileHistogramBuckets> histogram{};
  uint32_t nearPlaneBricks = 0u;
  uint32_t offscreenBricks = 0u;
  uint32_t scatterOverflow = 0u;
  uint32_t scatterHighWater = 0u;
};

inline uint32_t sparseVoxelBrickTileGridExtent(const uint32_t viewportExtent)
{
  return (std::max(viewportExtent, 1u) + kSparseVoxelBrickTileSize - 1u) /
      kSparseVoxelBrickTileSize;
}

class SparseVoxelBrickTileBinPass final : public ::rendering::Pass
{
public:
  enum class Kernel
  {
    Reset,
    Count,
    Reduce,
    Scan,
    Scatter,
  };

  struct Buffers
  {
    ::rendering::Buffer nodes;
    ::rendering::Buffer visibleBricks;
    ::rendering::Buffer counters;
    ::rendering::Buffer tileCounts;
    ::rendering::Buffer stats;
    ::rendering::Buffer tileOffsets;
    ::rendering::Buffer tileCandidates;
    // Phase one's visible list and counters. The hardware draws both phases
    // into one depth buffer, so parity needs both binned.
    ::rendering::Buffer visibleBricks2;
    ::rendering::Buffer counters2;
  };

  struct Settings
  {
    SparseVoxelBrickCullUniformsGPU uniforms{};
    uint32_t tileCount = 1u;
    // Capacity of the scatter buffer, in candidate entries. Sized from the H0
    // census with headroom; overflow is reported rather than silently dropped.
    uint32_t candidateCapacity = 1u;
  };

  SparseVoxelBrickTileBinPass(
      const Kernel kernel, const Buffers buffers, const Settings settings)
      : kernel_(kernel), buffers_(buffers), settings_(settings)
  {
  }

  ~SparseVoxelBrickTileBinPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
  }

  void updateUniforms(const SparseVoxelBrickCullUniformsGPU &uniforms,
      const uint32_t tileCount)
  {
    settings_.uniforms = uniforms;
    settings_.tileCount = std::max(tileCount, 1u);
    if (uniformBuffer_.isValid())
      renderGraph->bufferWrite(
          uniformBuffer_, 0u, sizeof(settings_.uniforms), &settings_.uniforms);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(
        uniformBuffer_, 0u, sizeof(settings_.uniforms), &settings_.uniforms);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    // Reset covers whichever is larger, the tile grid or the single-thread
    // header clear; Count is per visible brick and bounds itself on the GPU
    // against the live count, so it dispatches over the capacity.
    if (kernel_ == Kernel::Scan)
    {
      // One workgroup by construction: the scan walks the whole tile grid in
      // chunks so it needs a single barrier domain.
      commandBuffer.cmdDispatch(1u, 1u, 1u);
      return;
    }
    const uint32_t threads =
        (kernel_ == Kernel::Count || kernel_ == Kernel::Scatter)
        // Both phases concatenated; the kernel bounds itself on the live sum.
        ? std::max(settings_.uniforms.visibleCapacity * 2u, 1u)
        : settings_.tileCount;
    commandBuffer.cmdDispatch(std::max(1u, (threads + 63u) / 64u), 1u, 1u);
  }

private:
  const char *entryPoint() const
  {
    switch (kernel_)
    {
    case Kernel::Reset:
      return "resetBrickTileBins";
    case Kernel::Count:
      return "countBrickTileBins";
    case Kernel::Reduce:
      return "reduceBrickTileBins";
    case Kernel::Scan:
      return "scanBrickTileBins";
    case Kernel::Scatter:
      return "scatterBrickTileBins";
    }
    return "";
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
        {.name = passName + "_Uniforms",
         .size = sizeof(SparseVoxelBrickCullUniformsGPU),
         .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
             ::rendering::BufferUsage::BufferUsage_Push,
         .frameLocal = true});
    const auto storage = [](const uint32_t binding, const char *name)
    {
      return ::rendering::BindingGroupLayoutBufferEntry{
        .name = name,
        .binding = binding,
        .isDynamic = false,
        .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
        .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute};
    };
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "uniforms",
              .binding = 0u,
              .isDynamic = false,
              .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             storage(1u, "nodes"),
             storage(2u, "visibleBricks"),
             storage(3u, "counters"),
             storage(4u, "tileCounts"),
             storage(5u, "stats"),
             storage(6u, "tileOffsets"),
             storage(7u, "tileCandidates"),
             storage(8u, "visibleBricks2"),
             storage(9u, "counters2"),
           }}}});
    const auto view = [](const ::rendering::Buffer buffer, const uint64_t size,
                          const ::rendering::AccessPattern access)
    {
      return ::rendering::BufferView{
        .buffer = buffer, .offset = 0u, .size = size, .access = access};
    };
    const ::rendering::AccessPattern readWrite =
        ::rendering::AccessPattern::SHADER_READ |
        ::rendering::AccessPattern::SHADER_WRITE;
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u,
              .bufferView = view(uniformBuffer_,
                  sizeof(SparseVoxelBrickCullUniformsGPU),
                  ::rendering::AccessPattern::UNIFORM_READ)},
             {.binding = 1u,
              .bufferView = view(buffers_.nodes,
                  capacityBytes(settings_.uniforms.nodeCount,
                      sizeof(SparseVoxelBrickNodeGPU)),
                  ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 2u,
              .bufferView = view(buffers_.visibleBricks,
                  capacityBytes(
                      settings_.uniforms.visibleCapacity, sizeof(uint32_t)),
                  ::rendering::AccessPattern::SHADER_READ)},
             // The cull counters declare atomic members, so the binding has to
             // be read-write even though this pass only loads from it.
             {.binding = 3u,
              .bufferView =
                  view(buffers_.counters, sizeof(uint32_t) * 12u, readWrite)},
             {.binding = 4u,
              .bufferView = view(buffers_.tileCounts,
                  capacityBytes(settings_.tileCount, sizeof(uint32_t)),
                  readWrite)},
             {.binding = 5u,
              .bufferView = view(buffers_.stats,
                  sizeof(SparseVoxelBrickTileBinStatsGPU), readWrite)},
             {.binding = 6u,
              .bufferView = view(buffers_.tileOffsets,
                  capacityBytes(settings_.tileCount, sizeof(uint32_t)),
                  readWrite)},
             {.binding = 7u,
              .bufferView = view(buffers_.tileCandidates,
                  capacityBytes(settings_.candidateCapacity, sizeof(uint32_t)),
                  readWrite)},
             {.binding = 8u,
              .bufferView = view(buffers_.visibleBricks2,
                  capacityBytes(
                      settings_.uniforms.visibleCapacity, sizeof(uint32_t)),
                  ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 9u,
              .bufferView =
                  view(buffers_.counters2, sizeof(uint32_t) * 12u, readWrite)},
           }}}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader",
         .layout = layout_,
         .src = os::io::readRelativeFile(
             "assets/shaders/spirv/svraster/sparse-voxel-brick-tile-bin.spirv"),
         .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline({.entry = entryPoint(),
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_});
  }

  Kernel kernel_;
  Buffers buffers_{};
  Settings settings_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
