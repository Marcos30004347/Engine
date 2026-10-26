#include "voxel/svraster/SparseVoxelTree64Material.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace voxel::svraster
{
namespace
{

uint32_t mix(uint32_t value)
{
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  return value ^ (value >> 16u);
}

uint32_t populationCount(uint32_t value)
{
  value -= (value >> 1u) & 0x55555555u;
  value = (value & 0x33333333u) + ((value >> 2u) & 0x33333333u);
  value = (value + (value >> 4u)) & 0x0f0f0f0fu;
  return (value * 0x01010101u) >> 24u;
}

bool sameCell(
    const std::array<int32_t, 3> &left,
    const std::array<int32_t, 3> &right)
{
  return left == right;
}

bool samePayload(
    const SparseVoxelTree64MaterialCellGPU &left,
    const SparseVoxelTree64MaterialCellGPU &right)
{
  return (left.materialAndFlags & ~kSparseVoxelTree64MaterialCellValid) ==
          (right.materialAndFlags & ~kSparseVoxelTree64MaterialCellValid) &&
      left.surfaceData == right.surfaceData &&
      left.auxContext == right.auxContext &&
      left.regionMappingId == right.regionMappingId &&
      left.reserved1 == right.reserved1;
}

bool populate(
    const std::vector<SparseVoxelTree64MaterialCellGPU> &cells,
    std::vector<SparseVoxelTree64MaterialCellGPU> &table,
    bool &conflictingDefinition)
{
  conflictingDefinition = false;
  for (SparseVoxelTree64MaterialCellGPU record : cells)
  {
    record.materialAndFlags |= kSparseVoxelTree64MaterialCellValid;
    const uint32_t start =
        sparseVoxelTree64MaterialHash(record.globalCell) % table.size();
    bool inserted = false;
    for (uint32_t probe = 0u;
         probe < kSparseVoxelTree64MaterialMaximumProbes; ++probe)
    {
      SparseVoxelTree64MaterialCellGPU &slot =
          table[(start + probe) % table.size()];
      if (!slot.valid())
      {
        slot = record;
        inserted = true;
        break;
      }
      if (sameCell(slot.globalCell, record.globalCell))
      {
        if (!samePayload(slot, record))
        {
          conflictingDefinition = true;
          return false;
        }
        inserted = true;
        break;
      }
    }
    if (!inserted)
      return false;
  }
  return true;
}

} // namespace

bool collectSparseVoxelTree64MaterialCells(
    const SparseVoxelBrickHierarchy &hierarchy,
    const std::array<float, 3> &treeCellSize,
    std::vector<SparseVoxelTree64MaterialCellGPU> &cells,
    SparseVoxelTree64MaterialBuildStats *stats,
    std::string *error)
{
  cells.clear();
  SparseVoxelTree64MaterialBuildStats measured{};
  if (error != nullptr)
    error->clear();
  if (std::any_of(
          treeCellSize.begin(), treeCellSize.end(),
          [](const float value)
          {
            return !std::isfinite(value) || !(value > 0.0f);
          }))
  {
    if (error != nullptr)
      *error = "Tree64 material extraction needs finite positive cell sizes";
    return false;
  }
  // A route that does not pack micro-surface words into the arena
  // (WorldVoxelChunkStreamingSettings::packMicroSurface) extracts materials
  // without per-voxel micro detail: the sidecar is either complete or absent.
  if ((!hierarchy.microSurfaceData.empty() &&
       hierarchy.materialIndices.size() != hierarchy.microSurfaceData.size()) ||
      hierarchy.palette.size() != hierarchy.paletteDescriptorIds.size())
  {
    if (error != nullptr)
      *error = "Tree64 material extraction found inconsistent brick sidecars";
    return false;
  }

  std::map<std::array<int32_t, 3>, SparseVoxelTree64MaterialCellGPU>
      uniqueCells;
  std::set<std::array<int32_t, 3>> ambiguousCells;

  for (const SparseVoxelBrickNodeGPU &node : hierarchy.nodes)
  {
    if (node.canonicalGridOriginScale[3] != 1)
      continue;
    ++measured.finestParents;
    if (node.occupancyIndex >= hierarchy.occupancy.size())
    {
      cells.clear();
      if (error != nullptr)
        *error = "Tree64 material extraction found an invalid occupancy index";
      return false;
    }
    const float voxelSize = node.minimumVoxelSize[3];
    bool matchingSize = std::isfinite(voxelSize) && voxelSize > 0.0f;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const double tolerance =
          std::max(1.0e-6, std::abs(static_cast<double>(treeCellSize[axis])) *
              1.0e-5);
      matchingSize = matchingSize &&
          std::abs(static_cast<double>(voxelSize) -
                   static_cast<double>(treeCellSize[axis])) <= tolerance;
    }
    const SparseVoxelBrickOccupancyGPU &occupancy =
        hierarchy.occupancy[node.occupancyIndex];
    for (uint32_t linearCell = 0u;
         linearCell < kSparseVoxelBrickSide * kSparseVoxelBrickSide *
                 kSparseVoxelBrickSide;
         ++linearCell)
    {
      const uint32_t word = linearCell / 32u;
      const uint32_t bit = linearCell % 32u;
      const uint32_t bits = occupancy.cells[word].bits;
      if ((bits & (uint32_t{1u} << bit)) == 0u)
        continue;
      ++measured.occupiedCells;
      if (!matchingSize)
      {
        ++measured.omittedCellSize;
        continue;
      }
      const std::array<uint32_t, 3> localCell{
        linearCell % kSparseVoxelBrickSide,
        (linearCell / kSparseVoxelBrickSide) % kSparseVoxelBrickSide,
        linearCell / (kSparseVoxelBrickSide * kSparseVoxelBrickSide),
      };
      std::array<int32_t, 3> globalCell{};
      bool aligned = true;
      bool canonical = true;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        const double worldMinimum =
            static_cast<double>(node.minimumVoxelSize[axis]) +
            static_cast<double>(localCell[axis]) * voxelSize;
        const double coordinate =
            worldMinimum / static_cast<double>(treeCellSize[axis]);
        const double rounded = std::round(coordinate);
        const double tolerance =
            std::max(1.0e-6, std::abs(coordinate) * 1.0e-6);
        if (std::abs(coordinate - rounded) > tolerance ||
            rounded < std::numeric_limits<int32_t>::min() ||
            rounded > std::numeric_limits<int32_t>::max())
        {
          aligned = false;
          break;
        }
        globalCell[axis] = static_cast<int32_t>(rounded);
        const int64_t canonicalCell =
            static_cast<int64_t>(node.canonicalGridOriginScale[axis]) +
            static_cast<int64_t>(localCell[axis]);
        canonical = canonical &&
            canonicalCell == static_cast<int64_t>(globalCell[axis]);
      }
      if (!aligned)
      {
        ++measured.omittedGridAlignment;
        continue;
      }
      if (!canonical)
      {
        ++measured.omittedCanonicalKey;
        continue;
      }
      const uint32_t precedingMask =
          bit == 0u ? 0u : ((uint32_t{1u} << bit) - 1u);
      const uint32_t materialOffset = occupancy.cells[word].prefix +
          populationCount(bits & precedingMask);
      const uint64_t materialAddress =
          static_cast<uint64_t>(node.materialBase) + materialOffset;
      if (materialAddress >= hierarchy.materialIndices.size())
      {
        cells.clear();
        if (error != nullptr)
          *error = "Tree64 material extraction found an invalid material address";
        return false;
      }
      const uint64_t paletteAddress =
          static_cast<uint64_t>(node.paletteBase) +
          hierarchy.materialIndices[materialAddress];
      if (paletteAddress >= hierarchy.paletteDescriptorIds.size())
      {
        cells.clear();
        if (error != nullptr)
          *error = "Tree64 material extraction found an invalid palette address";
        return false;
      }
      const uint16_t materialHandle =
          hierarchy.paletteDescriptorIds[paletteAddress];
      if (materialHandle == 0u)
      {
        ++measured.omittedDisabledMaterial;
        continue;
      }
      SparseVoxelTree64MaterialCellGPU record{};
      record.globalCell = globalCell;
      record.materialAndFlags = materialHandle;
      record.surfaceData = packSparseVoxelMicroContextDataLegacy(
          hierarchy.microSurfaceData.empty() ? uint64_t{0} : hierarchy.microSurfaceData[materialAddress]);
      record.regionMappingId = node.regionMappingId;
      if (ambiguousCells.find(globalCell) != ambiguousCells.end())
        continue;
      const auto existing = uniqueCells.find(globalCell);
      if (existing == uniqueCells.end())
      {
        uniqueCells.emplace(globalCell, record);
      }
      else if (!samePayload(existing->second, record))
      {
        uniqueCells.erase(existing);
        ambiguousCells.insert(globalCell);
        ++measured.omittedAmbiguousCells;
      }
    }
  }
  cells.reserve(uniqueCells.size());
  for (const auto &[cell, record] : uniqueCells)
  {
    (void)cell;
    cells.push_back(record);
  }
  measured.emittedCells = cells.size();
  if (stats != nullptr)
    *stats = measured;
  return true;
}

uint32_t sparseVoxelTree64MaterialHash(
    const std::array<int32_t, 3> &cell)
{
  uint32_t value = static_cast<uint32_t>(cell[0]) * 0x8da6b343u;
  value = mix(value ^ static_cast<uint32_t>(cell[1]) * 0xd8163841u);
  return mix(value ^ static_cast<uint32_t>(cell[2]) * 0xcb1ab31fu);
}

bool buildSparseVoxelTree64MaterialTable(
    const std::vector<SparseVoxelTree64MaterialCellGPU> &cells,
    std::vector<SparseVoxelTree64MaterialCellGPU> &table,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  constexpr uint32_t knownFlags =
      kSparseVoxelTree64MaterialCellValid |
      kSparseVoxelTree64MaterialHandleMask;
  for (const SparseVoxelTree64MaterialCellGPU &record : cells)
  {
    if ((record.materialAndFlags & ~knownFlags) != 0u ||
        record.reserved1 != 0u)
    {
      table.clear();
      if (error != nullptr)
        *error = "Tree64 material sidecar record uses unsupported flags or reserved fields";
      return false;
    }
  }
  if (cells.size() > std::numeric_limits<size_t>::max() / 2u)
  {
    table.clear();
    if (error != nullptr)
      *error = "Tree64 material sidecar capacity overflows addressable memory";
    return false;
  }
  size_t capacity = 1u;
  while (capacity < std::max<size_t>(2u, cells.size() * 2u))
  {
    if (capacity > std::numeric_limits<size_t>::max() / 2u)
    {
      table.clear();
      if (error != nullptr)
        *error = "Tree64 material sidecar capacity overflows addressable memory";
      return false;
    }
    capacity <<= 1u;
  }
  for (uint32_t attempt = 0u; attempt < 8u; ++attempt)
  {
    table.assign(capacity, {});
    bool conflictingDefinition = false;
    if (populate(cells, table, conflictingDefinition))
      return true;
    if (conflictingDefinition)
    {
      table.clear();
      if (error != nullptr)
        *error = "Tree64 material sidecar contains conflicting definitions for one exact cell";
      return false;
    }
    if (capacity > std::numeric_limits<size_t>::max() / 2u)
      break;
    capacity <<= 1u;
  }
  table.clear();
  if (error != nullptr)
    *error = "Tree64 material sidecar exceeded its bounded probe budget";
  return false;
}

const SparseVoxelTree64MaterialCellGPU *findSparseVoxelTree64MaterialCell(
    const std::vector<SparseVoxelTree64MaterialCellGPU> &table,
    const std::array<int32_t, 3> &cell, uint32_t *probes)
{
  if (probes != nullptr)
    *probes = 0u;
  if (table.empty())
    return nullptr;
  const uint32_t start = sparseVoxelTree64MaterialHash(cell) % table.size();
  for (uint32_t probe = 0u;
       probe < kSparseVoxelTree64MaterialMaximumProbes; ++probe)
  {
    if (probes != nullptr)
      ++*probes;
    const SparseVoxelTree64MaterialCellGPU &record =
        table[(start + probe) % table.size()];
    if (!record.valid())
      return nullptr;
    if (sameCell(record.globalCell, cell))
      return &record;
  }
  return nullptr;
}

double SparseVoxelTree64MaterialMeasurements::instructionsPerCandidate() const
{
  return candidateParents == 0u
      ? 0.0
      : static_cast<double>(interpreterInstructions) /
          static_cast<double>(candidateParents);
}

double SparseVoxelTree64MaterialMeasurements::contextLoadsPerCandidate() const
{
  return candidateParents == 0u
      ? 0.0
      : static_cast<double>(contextLoads) /
          static_cast<double>(candidateParents);
}

bool traceSparseVoxelTree64MaterialReference(
    const SparseVoxelMaterialRayConsumer consumer,
    const std::vector<SparseVoxelTree64MaterialCandidate> &candidates,
    SparseVoxelTree64MaterialReferenceHit &hit,
    SparseVoxelTree64MaterialMeasurements *measurements,
    std::string *error)
{
  (void)consumer;
  hit = {};
  if (error != nullptr)
    error->clear();
  uint64_t previousInstructionCount = std::numeric_limits<uint64_t>::max();
  for (uint32_t index = 0u; index < candidates.size(); ++index)
  {
    const SparseVoxelTree64MaterialCandidate &candidate = candidates[index];
    if (measurements != nullptr)
    {
      ++measurements->candidateParents;
      ++measurements->microCellEvaluations;
      measurements->interpreterInstructions +=
          candidate.program.instructions.size();
      if (previousInstructionCount != std::numeric_limits<uint64_t>::max() &&
          previousInstructionCount != candidate.program.instructions.size())
        ++measurements->instructionDivergence;
      if (material::voxelGeometryProgramUsesContext(candidate.program) ||
          material::voxelGeometryRequiredContextFields(candidate.program) != 0u)
        ++measurements->contextLoads;
    }
    previousInstructionCount = candidate.program.instructions.size();
    bool occupied = true;
    if (!candidate.program.instructions.empty() &&
        !material::voxelGeometryCellOccupied(
            candidate.program, candidate.inputs, candidate.microCell,
            occupied, error))
      return false;
    if (!occupied)
    {
      if (measurements != nullptr)
        ++measurements->carvedParents;
      continue;
    }
    hit.hit = true;
    hit.candidateIndex = index;
    hit.deterministicIdentity = candidate.deterministicIdentity;
    hit.distance = candidate.distance;
    return true;
  }
  return true;
}

} // namespace voxel::svraster
