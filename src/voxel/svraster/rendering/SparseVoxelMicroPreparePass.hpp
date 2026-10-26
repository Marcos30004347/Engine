#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/material/VoxelGeometryIR.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelMicroMaterial.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace voxel::svraster::rendering
{

// Readback ABI for the opt-in Phase0 unique-parent sizing diagnostic. This is
// separate from SparseVoxelMicroCountersGPU: those counters describe repeated
// fragment encounters, while every value here is accumulated once per
// (visible brick, occupied parent voxel).
struct alignas(16) SparseVoxelMicroPrepareCountersGPU
{
  uint32_t visibleBricks = 0u;
  uint32_t occupiedParents = 0u;
  uint32_t microParents = 0u;
  uint32_t validDescriptors = 0u;
  uint32_t invalidDescriptors = 0u;
  uint32_t contextInterior = 0u;
  uint32_t contextFlat = 0u;
  uint32_t contextSlope = 0u;
  uint32_t contextWall = 0u;
  uint32_t contextOther = 0u;
  uint32_t projectedLtHalfPixel = 0u;
  uint32_t projectedHalfToOnePixel = 0u;
  uint32_t projectedOneToTwoPixels = 0u;
  uint32_t projectedGeTwoPixels = 0u;
  // Potentially enters the exact/fallback handoff using maximum AABB projected
  // size; includes the guaranteed set and stochastic transition band.
  uint32_t handoffEligible = 0u;
  // Guaranteed exact for every possible AABB entry, using minimum AABB
  // projected size and the optional distance cutoff.
  uint32_t exactNearEligible = 0u;
  // Number of four-word compact parent records selected by the source scan.
  // Geometry remains procedural.
  uint32_t heightfieldRecords = 0u;
  uint32_t heightfieldMicrocells = 0u;
  uint32_t nonHeightfieldExact = 0u;
  uint32_t heightfieldQuadsLow = 0u;
  uint32_t heightfieldQuadsHigh = 0u;
  uint32_t heightfieldBytesLow = 0u;
  uint32_t heightfieldBytesHigh = 0u;
  uint32_t reserved0 = 0u;
};
static_assert(sizeof(SparseVoxelMicroPrepareCountersGPU) == 96u);

inline constexpr uint32_t kSparseVoxelMicroPrepareRecordBytes = 32u;
inline constexpr uint32_t kSparseVoxelMicroRecordParentMask = 0x1ffu;
inline constexpr uint32_t kSparseVoxelMicroRecordDescriptorShift = 9u;
inline constexpr uint32_t kSparseVoxelMicroRecordDescriptorMask = 0xffffu;
inline constexpr uint32_t kSparseVoxelMicroRecordGeometryIRShift = 25u;
inline constexpr uint32_t kSparseVoxelMicroRecordNeighbourShift = 26u;
inline constexpr uint32_t kSparseVoxelMicroRecordNeighbourMask = 0x3fu;

constexpr uint32_t sparseVoxelMicroRecordParentDescriptorFlags(
    const uint32_t parentIndex,
    const uint32_t descriptorId,
    const bool geometryIREnabled,
    const uint32_t occupiedNeighbourMask)
{
  return (parentIndex & kSparseVoxelMicroRecordParentMask) |
      ((descriptorId & kSparseVoxelMicroRecordDescriptorMask) <<
       kSparseVoxelMicroRecordDescriptorShift) |
      (static_cast<uint32_t>(geometryIREnabled) <<
       kSparseVoxelMicroRecordGeometryIRShift) |
      ((occupiedNeighbourMask & kSparseVoxelMicroRecordNeighbourMask) <<
       kSparseVoxelMicroRecordNeighbourShift);
}

inline constexpr uint32_t kSparseVoxelMicroRecordAllFields =
    sparseVoxelMicroRecordParentDescriptorFlags(
        511u, 65535u, true, 63u);
static_assert(kSparseVoxelMicroRecordAllFields == 0xffffffffu);
static_assert(
    (kSparseVoxelMicroRecordAllFields & kSparseVoxelMicroRecordParentMask) ==
    511u);
static_assert(
    ((kSparseVoxelMicroRecordAllFields >>
      kSparseVoxelMicroRecordDescriptorShift) &
     kSparseVoxelMicroRecordDescriptorMask) == 65535u);
static_assert(
    ((kSparseVoxelMicroRecordAllFields >>
      kSparseVoxelMicroRecordGeometryIRShift) & 1u) == 1u);
static_assert(
    ((kSparseVoxelMicroRecordAllFields >>
      kSparseVoxelMicroRecordNeighbourShift) &
     kSparseVoxelMicroRecordNeighbourMask) == 63u);

// Compact parent records consumed by the prepared-parent raster. The descriptor
// and geometry-program bit share the otherwise unused high bits of the 9-bit
// parent index; the final six bits carry source-neighbour occupancy for exact
// residual/prepared boundary ownership. MicroPrepare uses the resolved material
// slot once to stage the context/surface patch, so the R2 fragment performs no
// occupancy-prefix, packed-material-index, descriptor-table, or context-buffer
// lookup.
struct alignas(16) SparseVoxelMicroBrickRecordGPU
{
  uint32_t nodeIndex = 0u;
  uint32_t parentDescriptorFlags = 0u;
  uint32_t paletteAddress = 0u;
  uint32_t microContextData = 0u;
  // Plan stage R3. Four bits per surface column, so the default subdivision of
  // 5 stores all 25 in 100 bits; bit 31 of the last word marks the cache
  // usable. Generated once per extracted parent, replacing a per-column
  // bilinear evaluation, quantization and staircase classification in every
  // fragment that visits the parent. The raster path falls back to live
  // evaluation whenever the valid bit is clear, so a coarser subdivision or a
  // geometry-IR parent simply keeps the incumbent behaviour.
  uint32_t columns[4]{};
};
static_assert(sizeof(SparseVoxelMicroBrickRecordGPU) == 32u);

// Fine occupancy remaining in the legacy brick after extracted parents have
// been removed, followed by the rebuilt 4^3 (logical 64-bit) coarse mask.
// Prefixes deliberately stay in the immutable source occupancy: clearing a bit
// must not renumber any surviving material.
// `passthrough` is nonzero when the brick extracted nothing, which after
// camera-facing eligibility is the overwhelmingly common case: a recent terrain
// capture extracted 61 parents out of 2,890,365. The residual mask of such a
// brick is byte-for-byte its source occupancy, so `bits`, `coarseLow`, and
// `coarseHigh` are left unwritten and the draw reads the immutable source
// instead. Writing them cost about 19 MB of store traffic per frame to restate
// data the draw already had.
struct SparseVoxelMicroResidualGPU
{
  uint32_t bits[16]{};
  uint32_t coarseLow = 0u;
  uint32_t coarseHigh = 0u;
  uint32_t passthrough = 0u;
  uint32_t padding = 0u;
};
static_assert(sizeof(SparseVoxelMicroResidualGPU) == 80u);

struct alignas(16) SparseVoxelMicroPrepareStateGPU
{
  uint32_t recordCount = 0u;
  uint32_t overflowBricks = 0u;
  uint32_t extractedParents = 0u;
  uint32_t residualParents = 0u;
  uint32_t outputBytesLow = 0u;
  uint32_t outputBytesHigh = 0u;
  // R3: extracted parents that received a compact column cache.
  uint32_t cachedRecords = 0u;
  uint32_t reserved1 = 0u;
};
static_assert(sizeof(SparseVoxelMicroPrepareStateGPU) == 32u);

struct SparseVoxelMicroPrepareDispatchGPU
{
  uint32_t x = 0u;
  uint32_t y = 1u;
  uint32_t z = 1u;
};
static_assert(sizeof(SparseVoxelMicroPrepareDispatchGPU) == 12u);

constexpr uint32_t sparseVoxelMicroPrepareQuadUpperBound(
    const uint32_t subdivision)
{
  return 3u * subdivision * subdivision + 2u * subdivision;
}
static_assert(sparseVoxelMicroPrepareQuadUpperBound(2u) == 16u);
static_assert(sparseVoxelMicroPrepareQuadUpperBound(8u) == 208u);

class SparseVoxelMicroPreparePass final : public ::rendering::Pass
{
public:
  enum class Kernel
  {
    Reset,
    Scan,
    Transform,
  };

  struct Buffers
  {
    ::rendering::Buffer visibleBricks;
    ::rendering::Buffer drawArgs;
    ::rendering::Buffer nodes;
    ::rendering::Buffer occupancy;
    ::rendering::Buffer materials;
    ::rendering::Buffer microSurfaceData;
    ::rendering::Buffer palette;
    ::rendering::Buffer paletteDescriptorIds;
    ::rendering::Buffer microMaterials;
    ::rendering::Buffer geometryPrograms;
    ::rendering::Buffer counters;
    ::rendering::Buffer records;
    ::rendering::Buffer residuals;
    ::rendering::Buffer state;
    ::rendering::Buffer dispatchArgs;
    ::rendering::Buffer microDrawArgs;
  };

  struct Capacities
  {
    uint32_t visibleCount = 1u;
    uint32_t nodeCount = 1u;
    uint32_t occupancyCount = 1u;
    uint32_t materialWordCount = 1u;
    uint32_t microSurfaceCount = 1u;
    uint32_t paletteCount = 1u;
    uint32_t paletteDescriptorWordCount = 1u;
    uint32_t microMaterialCount = 1u;
    uint32_t geometryProgramCount = 1u;
    uint32_t recordCount = 1u;
  };

  SparseVoxelMicroPreparePass(
      const Kernel kernel,
      const Buffers buffers,
      const Capacities capacities,
      const SparseVoxelBrickDrawUniformsGPU uniforms,
      const bool diagnostics)
      : kernel_(kernel), buffers_(buffers), capacities_(capacities),
        uniforms_(uniforms), diagnostics_(diagnostics)
  {
  }

  ~SparseVoxelMicroPreparePass() override
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

  void updateUniforms(const SparseVoxelBrickDrawUniformsGPU &uniforms)
  {
    uniforms_ = uniforms;
    if (uniformBuffer_.isValid())
      renderGraph->bufferWrite(
          uniformBuffer_, 0u, sizeof(uniforms_), &uniforms_);
  }

  void recordCommandBuffer(
      ::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    if (uniformBuffer_.isValid())
      renderGraph->bufferWrite(
          uniformBuffer_, 0u, sizeof(uniforms_), &uniforms_);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    if (kernel_ == Kernel::Transform)
      commandBuffer.cmdDispatchIndirect(buffers_.dispatchArgs, 0u);
    else
      commandBuffer.cmdDispatch(
          kernel_ == Kernel::Scan
              ? std::max(1u, (capacities_.visibleCount + 63u) / 64u)
              : 1u,
          1u, 1u);
  }

private:
  static uint64_t capacityBytes(
      const uint32_t count, const uint64_t stride)
  {
    return std::max<uint64_t>(1u, count) * stride;
  }

  const char *entryPoint() const
  {
    if (kernel_ == Kernel::Reset)
      return "resetMicroPrepareCounters";
    if (kernel_ == Kernel::Transform)
      return "transformMicroPrepareParents";
    return "scanMicroPrepareParents";
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    if (kernel_ != Kernel::Reset)
      uniformBuffer_ = renderGraph->createBuffer(
          {.name = passName + "_Uniforms",
           .size = sizeof(SparseVoxelBrickDrawUniformsGPU),
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
    std::vector<::rendering::BindingGroupLayoutBufferEntry> layoutBuffers{
             {.name = "uniforms", .binding = 0u, .isDynamic = false,
              .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
             storage(1u, "visibleBricks"),
             storage(2u, "drawArgs"),
             storage(3u, "nodes"),
             storage(4u, "occupancy"),
             storage(5u, "brickMaterialIndices"),
             storage(6u, "brickMicroSurfaceData"),
             storage(7u, "brickPalette"),
             storage(8u, "brickPaletteDescriptorIds"),
             storage(9u, "microMaterials"),
             storage(10u, "geometryPrograms"),
             storage(12u, "records"),
             storage(13u, "residuals"),
             storage(15u, "dispatchArgs"),
             storage(16u, "microDrawArgs"),
    };
    if (diagnostics_)
    {
      layoutBuffers.push_back(storage(11u, "output"));
      layoutBuffers.push_back(storage(14u, "state"));
    }
    const auto usesBinding = [this](const uint32_t binding)
    {
      if (kernel_ == Kernel::Reset)
        return binding == 1u || binding == 2u || binding == 15u ||
            binding == 16u ||
            (diagnostics_ && (binding == 11u || binding == 14u));
      if (kernel_ == Kernel::Scan)
        return diagnostics_ && binding <= 11u;
      return binding <= 10u || binding == 12u || binding == 13u ||
          binding == 16u || (diagnostics_ && binding == 14u);
    };
    layoutBuffers.erase(
        std::remove_if(
            layoutBuffers.begin(), layoutBuffers.end(),
            [&usesBinding](const auto &entry)
            {
              return !usesBinding(entry.binding);
            }),
        layoutBuffers.end());
    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = std::move(layoutBuffers)}}});
    const auto view = [](const ::rendering::Buffer buffer,
                          const uint64_t size,
                          const ::rendering::AccessPattern access)
    {
      return ::rendering::BufferView{
        .buffer = buffer, .offset = 0u, .size = size, .access = access};
    };
    std::vector<::rendering::BindingBuffer> groupBuffers{
             {.binding = 0u, .bufferView = view(
                  uniformBuffer_, sizeof(SparseVoxelBrickDrawUniformsGPU),
                  ::rendering::AccessPattern::UNIFORM_READ)},
             {.binding = 1u, .bufferView = view(
                  buffers_.visibleBricks,
                  capacityBytes(capacities_.visibleCount, sizeof(uint32_t)),
                  ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 2u, .bufferView = view(
                  buffers_.drawArgs, sizeof(SparseVoxelBrickDrawIndirectGPU),
                  ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 3u, .bufferView = view(
                  buffers_.nodes,
                  capacityBytes(capacities_.nodeCount,
                      sizeof(SparseVoxelBrickNodeGPU)),
                  ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 4u, .bufferView = view(
                  buffers_.occupancy,
                  capacityBytes(capacities_.occupancyCount,
                      sizeof(SparseVoxelBrickOccupancyGPU)),
                  ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 5u, .bufferView = view(
                  buffers_.materials,
                  capacityBytes(capacities_.materialWordCount,
                      sizeof(uint32_t)),
                  ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 6u, .bufferView = view(
                  buffers_.microSurfaceData,
                  capacityBytes(capacities_.microSurfaceCount,
                      sizeof(uint64_t)),
                  ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 7u, .bufferView = view(
                  buffers_.palette,
                  capacityBytes(capacities_.paletteCount,
                      sizeof(SparseVoxelBrickMaterialGPU)),
                  ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 8u, .bufferView = view(
                  buffers_.paletteDescriptorIds,
                  capacityBytes(capacities_.paletteDescriptorWordCount,
                      sizeof(uint32_t)),
                  ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 9u, .bufferView = view(
                  buffers_.microMaterials,
                  capacityBytes(capacities_.microMaterialCount,
                      sizeof(SparseVoxelMicroMaterialGPU)),
                  ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 10u, .bufferView = view(
                 buffers_.geometryPrograms,
                 capacityBytes(capacities_.geometryProgramCount,
                      sizeof(::voxel::material::VoxelGeometryProgramGPU)),
                  ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 12u, .bufferView = view(
                  buffers_.records,
                  capacityBytes(capacities_.recordCount,
                      sizeof(SparseVoxelMicroBrickRecordGPU)),
                  ::rendering::AccessPattern::SHADER_WRITE)},
             {.binding = 13u, .bufferView = view(
                  buffers_.residuals,
                  capacityBytes(capacities_.visibleCount,
                      sizeof(SparseVoxelMicroResidualGPU)),
                  ::rendering::AccessPattern::SHADER_WRITE)},
             {.binding = 15u, .bufferView = view(
                  buffers_.dispatchArgs,
                  sizeof(SparseVoxelMicroPrepareDispatchGPU),
                  ::rendering::AccessPattern::SHADER_WRITE)},
             {.binding = 16u, .bufferView = view(
                  buffers_.microDrawArgs,
                  sizeof(SparseVoxelBrickDrawIndirectGPU),
                  ::rendering::AccessPattern::SHADER_READ |
                      ::rendering::AccessPattern::SHADER_WRITE)},
    };
    if (diagnostics_)
    {
      groupBuffers.push_back(
          {.binding = 11u,
           .bufferView = view(
               buffers_.counters,
               sizeof(SparseVoxelMicroPrepareCountersGPU),
               ::rendering::AccessPattern::SHADER_READ |
                   ::rendering::AccessPattern::SHADER_WRITE)});
      groupBuffers.push_back(
          {.binding = 14u,
           .bufferView = view(
               buffers_.state,
               sizeof(SparseVoxelMicroPrepareStateGPU),
               ::rendering::AccessPattern::SHADER_READ |
                   ::rendering::AccessPattern::SHADER_WRITE)});
    }
    groupBuffers.erase(
        std::remove_if(
            groupBuffers.begin(), groupBuffers.end(),
            [&usesBinding](const auto &entry)
            {
              return !usesBinding(entry.binding);
            }),
        groupBuffers.end());
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0", .buffers = std::move(groupBuffers)}}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader",
         .layout = layout_,
         .src = os::io::readRelativeFile(
             std::string("assets/shaders/spirv/svraster/") +
             (diagnostics_
                 ? "sparse-voxel-micro-prepare-diagnostics.spirv"
                 : "sparse-voxel-micro-prepare.spirv")),
         .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline(
        {.entry = entryPoint(), .layout = layout_,
         .name = passName + "_Pipeline", .shader = shader_});
  }

  Kernel kernel_;
  Buffers buffers_{};
  Capacities capacities_{};
  SparseVoxelBrickDrawUniformsGPU uniforms_{};
  bool diagnostics_ = false;
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
