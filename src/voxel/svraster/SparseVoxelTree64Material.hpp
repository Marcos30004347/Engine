#pragma once

#include "voxel/material/VoxelGeometryIR.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

struct SparseVoxelBrickHierarchy;

inline constexpr uint32_t kSparseVoxelTree64MaterialCellValid = 1u << 31u;
inline constexpr uint32_t kSparseVoxelTree64MaterialHandleMask = 0xffffu;
inline constexpr uint32_t kSparseVoxelTree64MaterialMaximumProbes = 8u;

// Sparse, open-addressed sidecar for an occupied Tree64 base cell. Tree64
// nodes remain representation-only; this record supplies the stable material
// handle and normalized contexts required to refine an exact leaf hit. Producers
// must omit cells that combine multiple source materials or contexts: a missing
// record remains conservatively occupied rather than inventing a refinement.
struct alignas(16) SparseVoxelTree64MaterialCellGPU
{
  std::array<int32_t, 3> globalCell{};
  uint32_t materialAndFlags = 0u;
  // Low 24 bits are the shared four-corner base-surface patch; the high byte
  // is the common normalized context used by brick visibility.
  uint32_t surfaceData = 0u;
  uint32_t auxContext = 0u;
  // Stable logical field-mapping identity copied from the source brick.
  uint32_t regionMappingId = 0u;
  uint32_t reserved1 = 0u;

  bool valid() const
  {
    return (materialAndFlags & kSparseVoxelTree64MaterialCellValid) != 0u;
  }
  uint16_t materialHandle() const
  {
    return static_cast<uint16_t>(
        materialAndFlags & kSparseVoxelTree64MaterialHandleMask);
  }
};
static_assert(sizeof(SparseVoxelTree64MaterialCellGPU) == 32u);

struct alignas(16) SparseVoxelTree64MaterialCountersGPU
{
  uint32_t candidateParents = 0u;
  uint32_t sidecarProbes = 0u;
  uint32_t contextLoads = 0u;
  uint32_t interpreterInstructions = 0u;
  uint32_t microCellSteps = 0u;
  uint32_t carvedParents = 0u;
  uint32_t acceptedParents = 0u;
  uint32_t reserved0 = 0u;
};
static_assert(sizeof(SparseVoxelTree64MaterialCountersGPU) == 32u);

struct SparseVoxelTree64MaterialBuildStats
{
  uint64_t finestParents = 0u;
  uint64_t occupiedCells = 0u;
  uint64_t emittedCells = 0u;
  uint64_t omittedCellSize = 0u;
  uint64_t omittedGridAlignment = 0u;
  uint64_t omittedCanonicalKey = 0u;
  uint64_t omittedDisabledMaterial = 0u;
  uint64_t omittedAmbiguousCells = 0u;
};

// Extracts only finest brick parents that are exactly the same world-space
// cells addressed by Tree64. This is intentionally strict: a different cell
// size, a translated grid, or a different canonical procedural key omits the
// source and leaves Tree64 conservatively occupied.
bool collectSparseVoxelTree64MaterialCells(
    const SparseVoxelBrickHierarchy &hierarchy,
    const std::array<float, 3> &treeCellSize,
    std::vector<SparseVoxelTree64MaterialCellGPU> &cells,
    SparseVoxelTree64MaterialBuildStats *stats = nullptr,
    std::string *error = nullptr);

uint32_t sparseVoxelTree64MaterialHash(const std::array<int32_t, 3> &cell);

// Produces a power-of-two hash table with bounded lookup. Duplicate definitions
// are accepted only when their payloads are identical; conflicting definitions
// are ambiguous coarse-cell mappings and fail instead of selecting one by
// insertion order. An empty input still returns a non-empty invalid table so it
// can be bound as a legal buffer.
bool buildSparseVoxelTree64MaterialTable(
    const std::vector<SparseVoxelTree64MaterialCellGPU> &cells,
    std::vector<SparseVoxelTree64MaterialCellGPU> &table,
    std::string *error = nullptr);

const SparseVoxelTree64MaterialCellGPU *findSparseVoxelTree64MaterialCell(
    const std::vector<SparseVoxelTree64MaterialCellGPU> &table,
    const std::array<int32_t, 3> &cell, uint32_t *probes = nullptr);

enum class SparseVoxelMaterialRayConsumer : uint32_t
{
  Raster,
  Tree64Primary,
  Shadow,
  Probe,
};

struct SparseVoxelTree64MaterialCandidate
{
  material::VoxelGeometryProgram program{};
  material::VoxelGeometryInputs inputs{};
  std::array<uint32_t, 3> microCell{};
  uint64_t deterministicIdentity = 0u;
  float distance = 0.0f;
};

struct SparseVoxelTree64MaterialReferenceHit
{
  bool hit = false;
  uint32_t candidateIndex = 0u;
  uint64_t deterministicIdentity = 0u;
  float distance = 0.0f;
};

struct SparseVoxelTree64MaterialMeasurements
{
  uint64_t candidateParents = 0u;
  uint64_t contextLoads = 0u;
  uint64_t interpreterInstructions = 0u;
  uint64_t instructionDivergence = 0u;
  uint64_t microCellEvaluations = 0u;
  uint64_t carvedParents = 0u;
  uint64_t materialPassBytes = 0u;

  double instructionsPerCandidate() const;
  double contextLoadsPerCandidate() const;
};

// Incremental resolved-material bandwidth from Phase 4/5. The common context
// stays in the existing packed word; only auxiliary context adds one u32.
inline constexpr uint32_t sparseVoxelMaterialPassBytesPerPixel(
    const bool auxiliaryContext)
{
  return auxiliaryContext ? sizeof(uint32_t) : 0u;
}

// Phase 8's stable resolved sample address is one signed XYZ plus one logical
// region ID. It is representation-independent and does not include transient
// residency identifiers.
inline constexpr uint32_t sparseVoxelMaterialSampleAddressBytesPerPixel()
{
  return sizeof(VoxelMaterialSampleAddressGPU);
}

// Consumer is deliberately not part of the occupancy decision. Keeping it in
// this API makes parity tests cover the real primary/shadow/probe call sites
// while enforcing one deterministic GeometryIR strategy for all of them.
bool traceSparseVoxelTree64MaterialReference(
    SparseVoxelMaterialRayConsumer consumer,
    const std::vector<SparseVoxelTree64MaterialCandidate> &candidates,
    SparseVoxelTree64MaterialReferenceHit &hit,
    SparseVoxelTree64MaterialMeasurements *measurements = nullptr,
    std::string *error = nullptr);

} // namespace voxel::svraster
