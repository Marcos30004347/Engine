#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace voxel::svraster::rendering
{

// R9 H1. Rasterizes the binned brick candidates in compute and diffs the result
// against the hardware raster's resolved coverage and linear depth.
//
// It never presents. H1 exists to establish that the compute path reproduces
// the hardware one before H3 routes any real work to it, so its output is a
// comparison buffer and its product is a parity report.
//
// One 16x16 workgroup owns one screen tile and one thread owns one pixel, so a
// thread keeps its nearest hit in a register and the inner loop needs no atomic
// at all. The packed depth|payload key the H0 census sized is only required by
// a primitive-parallel variant, which pixel ownership makes unnecessary.
struct alignas(16) SparseVoxelBrickTileParityGPU
{
  uint32_t comparedPixels = 0u;
  uint32_t bothCovered = 0u;
  // The two numbers H1 has to drive to zero.
  uint32_t hardwareOnly = 0u;
  uint32_t computeOnly = 0u;
  uint32_t distanceMatches = 0u;
  uint32_t maximumDeltaMicrometres = 0u;
  uint32_t computeNearer = 0u;
  uint32_t hardwareNearer = 0u;
  uint32_t worstPixel = 0u;
  uint32_t worstHardwareBits = 0u;
  uint32_t worstComputeBits = 0u;
  uint32_t worstCandidatesSpanning = 0u;
  uint32_t worstCandidateCount = 0u;
  uint32_t worstComputeNode = 0u;
  // Sizes the last open H1 question against the fragment's
  // microCounters.eligibleParentHits: how much of the fragment's micro work is
  // overdraw redundancy that a per-pixel compute walk would not repeat. Only
  // populated when exact-micro counters are collected.
  uint32_t occupiedCellHits = 0u;
  uint32_t microEligibleCellHits = 0u;
  // Micro record parity over pixels where both paths agree on the surface.
  uint32_t microComparedPixels = 0u;
  uint32_t microPayloadMismatches = 0u;
  uint32_t microAddressMismatches = 0u;
  uint32_t microIdentityMismatches = 0u;
  uint32_t microPayloadHardwareOnly = 0u;
  uint32_t microPayloadComputeOnly = 0u;
  uint32_t microPayloadBothDiffer = 0u;
  uint32_t computeExactTraces = 0u;
};

// This struct is read back verbatim from the buffer the tile kernel writes, so
// it has to match `SparseVoxelBrickTileParity` in
// sparse_voxel_brick_tile_raster.wgsl field for field. Nothing else enforces
// that: a field added on one side only would silently shift every counter after
// it and the parity report would read plausible but wrong numbers rather than
// fail. Assert the offsets of the boundaries between the three groups the WGSL
// struct is documented in, plus the total size.
// 23 counters, and alignas(16) rounds the 92 bytes up to 96. Asserting the last
// field's offset rather than the size is what actually pins the field count -
// the padding would otherwise absorb a 24th field without complaint.
static_assert(
    offsetof(SparseVoxelBrickTileParityGPU, computeExactTraces) ==
        23u * sizeof(uint32_t),
    "Parity struct must stay in step with the WGSL declaration");
static_assert(sizeof(SparseVoxelBrickTileParityGPU) == 96u,
    "Parity struct is 24 u32, already 16-byte aligned");
static_assert(offsetof(SparseVoxelBrickTileParityGPU, worstPixel) ==
        8u * sizeof(uint32_t),
    "worstPixel must follow the eight aggregate counters");
static_assert(offsetof(SparseVoxelBrickTileParityGPU, occupiedCellHits) ==
        14u * sizeof(uint32_t),
    "Micro sizing counters must follow the worst-pixel capture block");
static_assert(offsetof(SparseVoxelBrickTileParityGPU, microComparedPixels) ==
        16u * sizeof(uint32_t),
    "Micro record parity counters must follow the micro sizing counters");
static_assert(
    offsetof(SparseVoxelBrickTileParityGPU, microPayloadHardwareOnly) ==
        20u * sizeof(uint32_t),
    "Payload direction counters must come last");

class SparseVoxelBrickTileRasterPass final : public ::rendering::Pass
{
public:
  enum class Kernel
  {
    ResetParity,
    Raster,
    Compare,
    Capture,
    // H3. Writes into the shared resolved-surface/coverage buffers, so unlike
    // the others this one changes what is rendered.
    Merge,
  };

  struct Buffers
  {
    ::rendering::Buffer nodes;
    ::rendering::Buffer occupancy;
    ::rendering::Buffer tileCounts;
    ::rendering::Buffer tileOffsets;
    ::rendering::Buffer tileCandidates;
    ::rendering::Buffer computeDistance;
    ::rendering::Buffer computeCoverage;
    ::rendering::Buffer hardwareCoverage;
    ::rendering::Buffer hardwareDistance;
    ::rendering::Buffer parity;
    ::rendering::Buffer computeNode;
    ::rendering::Buffer computeCell;
    ::rendering::Buffer brickMaterialIndices;
    ::rendering::Buffer brickPalette;
    ::rendering::Buffer resolvedSurfaces;
    ::rendering::Buffer computeAxis;
    ::rendering::Buffer brickMicroSurfaceData;
    // H1 exact microgeometry in compute.
    ::rendering::Buffer brickPaletteDescriptorIds;
    ::rendering::Buffer microMaterials;
    ::rendering::Buffer computeMicroPayload;
    ::rendering::Buffer computeMicroAddress;
    ::rendering::Buffer computeIdentity;
    ::rendering::Buffer computeContext;
    // The resolver's shared micro products, which a merged pixel publishes into.
    ::rendering::Buffer microPayloads;
    ::rendering::Buffer materialSampleAddresses;
  };

  struct Capacities
  {
    uint32_t nodeCount = 1u;
    uint32_t occupancyCount = 1u;
    uint32_t tileCount = 1u;
    uint32_t candidateCapacity = 1u;
    uint32_t pixelCount = 1u;
    uint32_t materialWordCount = 1u;
    uint32_t paletteCount = 1u;
    uint32_t microSurfaceCount = 1u;
    uint32_t paletteDescriptorWordCount = 1u;
    uint32_t microMaterialCount = 1u;
  };

  struct Settings
  {
    SparseVoxelBrickDrawUniformsGPU uniforms{};
  };

  SparseVoxelBrickTileRasterPass(const Kernel kernel, const Buffers buffers,
      const Capacities capacities, const Settings settings)
      : kernel_(kernel), buffers_(buffers), capacities_(capacities),
        settings_(settings)
  {
  }

  ~SparseVoxelBrickTileRasterPass() override
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
    settings_.uniforms = uniforms;
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
    switch (kernel_)
    {
    case Kernel::ResetParity:
      commandBuffer.cmdDispatch(1u, 1u, 1u);
      break;
    case Kernel::Raster:
      // One workgroup per tile. Empty tiles cost a workgroup that exits at its
      // first bounds test, which is cheaper than an indirect compaction here.
      commandBuffer.cmdDispatch(std::max(capacities_.tileCount, 1u), 1u, 1u);
      break;
    case Kernel::Compare:
    case Kernel::Capture:
    case Kernel::Merge:
      commandBuffer.cmdDispatch(
          std::max(1u, (capacities_.pixelCount + 63u) / 64u), 1u, 1u);
      break;
    }
  }

private:
  const char *entryPoint() const
  {
    switch (kernel_)
    {
    case Kernel::ResetParity:
      return "resetBrickTileParity";
    case Kernel::Raster:
      return "rasterBrickTile";
    case Kernel::Compare:
      return "compareBrickTileRaster";
    case Kernel::Capture:
      return "captureBrickTileMismatch";
    case Kernel::Merge:
      return "mergeBrickTileRaster";
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
             storage(2u, "occupancy"),
             storage(3u, "tileCounts"),
             storage(4u, "tileOffsets"),
             storage(5u, "tileCandidates"),
             storage(6u, "computeDistance"),
             storage(7u, "computeCoverage"),
             storage(8u, "hardwareCoverage"),
             storage(9u, "hardwareDistance"),
             storage(10u, "parity"),
             storage(11u, "computeNode"),
             storage(12u, "computeCell"),
             storage(13u, "brickMaterialIndices"),
             storage(14u, "brickPalette"),
             storage(15u, "resolvedSurfaces"),
             storage(16u, "computeAxis"),
             storage(17u, "brickMicroSurfaceData"),
             storage(18u, "brickPaletteDescriptorIds"),
             storage(19u, "microMaterials"),
             storage(20u, "computeMicroPayload"),
             storage(21u, "computeMicroAddress"),
             storage(22u, "computeIdentity"),
             storage(23u, "computeContext"),
             storage(24u, "microPayloads"),
             storage(25u, "materialSampleAddresses"),
           }}}});
    const auto view = [](const ::rendering::Buffer buffer, const uint64_t size,
                          const ::rendering::AccessPattern access)
    {
      return ::rendering::BufferView{
        .buffer = buffer, .offset = 0u, .size = size, .access = access};
    };
    const ::rendering::AccessPattern read =
        ::rendering::AccessPattern::SHADER_READ;
    const ::rendering::AccessPattern readWrite =
        read | ::rendering::AccessPattern::SHADER_WRITE;
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u,
              .bufferView = view(uniformBuffer_,
                  sizeof(SparseVoxelBrickDrawUniformsGPU),
                  ::rendering::AccessPattern::UNIFORM_READ)},
             {.binding = 1u,
              .bufferView = view(buffers_.nodes,
                  capacityBytes(
                      capacities_.nodeCount, sizeof(SparseVoxelBrickNodeGPU)),
                  read)},
             {.binding = 2u,
              .bufferView = view(buffers_.occupancy,
                  capacityBytes(capacities_.occupancyCount,
                      sizeof(SparseVoxelBrickOccupancyGPU)),
                  read)},
             {.binding = 3u,
              .bufferView = view(buffers_.tileCounts,
                  capacityBytes(capacities_.tileCount, sizeof(uint32_t)),
                  read)},
             {.binding = 4u,
              .bufferView = view(buffers_.tileOffsets,
                  capacityBytes(capacities_.tileCount, sizeof(uint32_t)),
                  read)},
             {.binding = 5u,
              .bufferView = view(buffers_.tileCandidates,
                  capacityBytes(
                      capacities_.candidateCapacity, sizeof(uint32_t)),
                  read)},
             {.binding = 6u,
              .bufferView = view(buffers_.computeDistance,
                  capacityBytes(capacities_.pixelCount, sizeof(float)),
                  readWrite)},
             {.binding = 7u,
              .bufferView = view(buffers_.computeCoverage,
                  capacityBytes(capacities_.pixelCount, sizeof(uint32_t)),
                  readWrite)},
             // Read-write: the merge publishes into the resolver's shared
             // coverage and linear depth, which is what downstream reads.
             {.binding = 8u,
              .bufferView = view(buffers_.hardwareCoverage,
                  capacityBytes(capacities_.pixelCount, sizeof(uint32_t)),
                  readWrite)},
             {.binding = 9u,
              .bufferView = view(buffers_.hardwareDistance,
                  capacityBytes(capacities_.pixelCount, sizeof(float)),
                  readWrite)},
             {.binding = 10u,
              .bufferView = view(buffers_.parity,
                  sizeof(SparseVoxelBrickTileParityGPU), readWrite)},
             {.binding = 11u,
              .bufferView = view(buffers_.computeNode,
                  capacityBytes(capacities_.pixelCount, sizeof(uint32_t)),
                  readWrite)},
             {.binding = 12u,
              .bufferView = view(buffers_.computeCell,
                  capacityBytes(capacities_.pixelCount, sizeof(uint32_t)),
                  readWrite)},
             {.binding = 13u,
              .bufferView = view(buffers_.brickMaterialIndices,
                  capacityBytes(
                      capacities_.materialWordCount, sizeof(uint32_t)),
                  read)},
             {.binding = 14u,
              .bufferView = view(buffers_.brickPalette,
                  capacityBytes(capacities_.paletteCount,
                      sizeof(SparseVoxelBrickMaterialGPU)),
                  read)},
             {.binding = 15u,
              .bufferView = view(buffers_.resolvedSurfaces,
                  capacityBytes(capacities_.pixelCount,
                      sizeof(ResolvedVoxelSurfaceGPU)),
                  readWrite)},
             {.binding = 16u,
              .bufferView = view(buffers_.computeAxis,
                  capacityBytes(capacities_.pixelCount, sizeof(uint32_t)),
                  readWrite)},
             {.binding = 17u,
              .bufferView = view(buffers_.brickMicroSurfaceData,
                  capacityBytes(
                      capacities_.microSurfaceCount, sizeof(uint64_t)),
                  read)},
             {.binding = 18u,
              .bufferView = view(buffers_.brickPaletteDescriptorIds,
                  capacityBytes(capacities_.paletteDescriptorWordCount,
                      sizeof(uint32_t)),
                  read)},
             {.binding = 19u,
              .bufferView = view(buffers_.microMaterials,
                  capacityBytes(capacities_.microMaterialCount,
                      sizeof(SparseVoxelMicroMaterialGPU)),
                  read)},
             {.binding = 20u,
              .bufferView = view(buffers_.computeMicroPayload,
                  capacityBytes(capacities_.pixelCount, sizeof(uint32_t)),
                  readWrite)},
             {.binding = 21u,
              .bufferView = view(buffers_.computeMicroAddress,
                  capacityBytes(capacities_.pixelCount, sizeof(uint32_t) * 4u),
                  readWrite)},
             {.binding = 22u,
              .bufferView = view(buffers_.computeIdentity,
                  capacityBytes(capacities_.pixelCount, sizeof(uint32_t)),
                  readWrite)},
             {.binding = 23u,
              .bufferView = view(buffers_.computeContext,
                  capacityBytes(capacities_.pixelCount, sizeof(uint32_t)),
                  readWrite)},
             {.binding = 24u,
              .bufferView = view(buffers_.microPayloads,
                  capacityBytes(capacities_.pixelCount, sizeof(uint32_t)),
                  readWrite)},
             {.binding = 25u,
              .bufferView = view(buffers_.materialSampleAddresses,
                  capacityBytes(capacities_.pixelCount, sizeof(uint32_t) * 4u),
                  readWrite)},
           }}}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader",
         .layout = layout_,
         .src = os::io::readRelativeFile(
             "assets/shaders/spirv/svraster/"
             "sparse-voxel-brick-tile-raster.spirv"),
         .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline({.entry = entryPoint(),
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_});
  }

  Kernel kernel_;
  Buffers buffers_{};
  Capacities capacities_{};
  Settings settings_{};
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
