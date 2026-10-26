#include "voxel/svraster/SparseVoxelGeometryFieldCache.hpp"

#include "voxel/material/VoxelMaterial.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelMicroGeometry.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <tuple>

namespace voxel::svraster
{
namespace
{

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

uint32_t mix(uint32_t value)
{
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  return value ^ (value >> 16u);
}

using CacheKey =
    std::tuple<std::array<int32_t, 3>, material::VoxelMaterialHandle>;

struct ParentSource
{
  uint32_t regionMappingId = 0u;
  uint64_t surfaceData = 0u;
  bool ambiguous = false;
};

bool sameSource(const ParentSource &left, const ParentSource &right)
{
  return left.regionMappingId == right.regionMappingId &&
      left.surfaceData == right.surfaceData;
}

uint32_t firstGeometryFace(
    const SparseVoxelMicroDescriptor &descriptor, const uint32_t context)
{
  if (descriptor.geometryProfile ==
          SparseVoxelMicroGeometryProfile::Staircase &&
      sparseVoxelMicroContextIsSurface(context))
    return static_cast<uint32_t>(SparseVoxelMicroFace::PositiveY);
  for (uint32_t face = 0u; face < 6u; ++face)
    if ((descriptor.faceMask & (1u << face)) != 0u)
      return face;
  return static_cast<uint32_t>(SparseVoxelMicroFace::PositiveY);
}

void hash64(uint64_t &hash, const uint32_t value)
{
  for (uint32_t byte = 0u; byte < 4u; ++byte)
  {
    hash ^= static_cast<uint8_t>(value >> (byte * 8u));
    hash *= 1099511628211ull;
  }
}

} // namespace

uint32_t sparseVoxelGeometryFieldCacheHash(
    const std::array<int32_t, 3> &canonicalParent,
    const material::VoxelMaterialHandle materialHandle)
{
  uint32_t value = mix(materialHandle ^ 0x85ebca6bu);
  value = mix(value ^ static_cast<uint32_t>(canonicalParent[0]) *
      0x8da6b343u);
  value = mix(value ^ static_cast<uint32_t>(canonicalParent[1]) *
      0xd8163841u);
  return mix(value ^ static_cast<uint32_t>(canonicalParent[2]) *
      0xcb1ab31fu);
}

const SparseVoxelGeometryFieldCacheRecordGPU *
findSparseVoxelGeometryFieldCacheRecord(
    const std::vector<SparseVoxelGeometryFieldCacheRecordGPU> &table,
    const std::array<int32_t, 3> &canonicalParent,
    const material::VoxelMaterialHandle materialHandle,
    uint32_t *probes)
{
  if (probes != nullptr)
    *probes = 0u;
  if (table.empty())
    return nullptr;
  const uint32_t start = sparseVoxelGeometryFieldCacheHash(
      canonicalParent, materialHandle) % static_cast<uint32_t>(table.size());
  for (uint32_t probe = 0u;
       probe < kSparseVoxelGeometryFieldCacheMaximumProbes; ++probe)
  {
    if (probes != nullptr)
      ++*probes;
    const auto &record = table[(start + probe) % table.size()];
    if (!record.valid())
      return nullptr;
    if (record.materialHandle() == materialHandle &&
        record.canonicalParent == canonicalParent)
      return &record;
  }
  return nullptr;
}

bool buildSparseVoxelGeometryFieldCache(
    const SparseVoxelBrickHierarchy &hierarchy,
    const material::VoxelMaterialManager &materials,
    const material::VoxelMaterialFieldAtlasSnapshot &atlas,
    SparseVoxelGeometryFieldCacheSnapshot &snapshot, std::string *error)
{
  snapshot = {};
  snapshot.sourceAtlasRevision = atlas.contentRevision;
  if (error != nullptr)
    error->clear();
  // The micro-surface sidecar is complete or absent (a route that does not
  // pack it into the arena builds the field without per-voxel micro detail).
  if ((!hierarchy.microSurfaceData.empty() &&
       hierarchy.materialIndices.size() != hierarchy.microSurfaceData.size()) ||
      hierarchy.palette.size() != hierarchy.paletteDescriptorIds.size())
    return fail(error, "Geometry field cache found inconsistent brick sidecars");

  bool hasFieldGeometry = false;
  for (uint32_t handle = 1u; handle < materials.slotCount(); ++handle)
    hasFieldGeometry = hasFieldGeometry ||
        material::voxelGeometryRequiredMaterialFields(
            materials.slot(static_cast<material::VoxelMaterialHandle>(handle))
                .geometryProgram) != 0u;
  if (!hasFieldGeometry)
  {
    snapshot.table.assign(2u, {});
    uint64_t revision = 1469598103934665603ull;
    hash64(revision, static_cast<uint32_t>(materials.geometryRevision()));
    hash64(revision, static_cast<uint32_t>(materials.geometryRevision() >> 32u));
    snapshot.contentRevision = revision == 0u ? 1u : revision;
    return true;
  }

  std::map<CacheKey, ParentSource> sources;
  for (const auto &node : hierarchy.nodes)
  {
    if (node.canonicalGridOriginScale[3] != 1)
      continue;
    if (node.occupancyIndex >= hierarchy.occupancy.size())
      return fail(error, "Geometry field cache found an invalid occupancy index");
    const auto &occupancy = hierarchy.occupancy[node.occupancyIndex];
    for (uint32_t linearCell = 0u;
         linearCell < kSparseVoxelBrickVoxelCount; ++linearCell)
    {
      const uint32_t word = linearCell / 32u;
      const uint32_t bit = linearCell % 32u;
      const uint32_t bits = occupancy.cells[word].bits;
      if ((bits & (1u << bit)) == 0u)
        continue;
      const uint32_t before = bit == 0u ? 0u : ((1u << bit) - 1u);
      const uint64_t materialAddress =
          static_cast<uint64_t>(node.materialBase) +
          occupancy.cells[word].prefix +
          static_cast<uint32_t>(__builtin_popcount(bits & before));
      if (materialAddress >= hierarchy.materialIndices.size())
        return fail(error, "Geometry field cache found an invalid material address");
      const uint64_t paletteAddress =
          static_cast<uint64_t>(node.paletteBase) +
          hierarchy.materialIndices[materialAddress];
      if (paletteAddress >= hierarchy.paletteDescriptorIds.size())
        return fail(error, "Geometry field cache found an invalid palette address");
      const auto handle = static_cast<material::VoxelMaterialHandle>(
          hierarchy.paletteDescriptorIds[paletteAddress]);
      if (handle == material::kInvalidVoxelMaterialHandle ||
          handle >= materials.slotCount() ||
          material::voxelGeometryRequiredMaterialFields(
              materials.slot(handle).geometryProgram) == 0u)
        continue;
      const std::array<uint32_t, 3> local{
        linearCell % kSparseVoxelBrickSide,
        (linearCell / kSparseVoxelBrickSide) % kSparseVoxelBrickSide,
        linearCell / (kSparseVoxelBrickSide * kSparseVoxelBrickSide)};
      std::array<int32_t, 3> parent{};
      for (uint32_t axis = 0u; axis < 3u; ++axis)
        parent[axis] = node.canonicalGridOriginScale[axis] +
            static_cast<int32_t>(local[axis]);
      ParentSource source{
        node.regionMappingId,
        hierarchy.microSurfaceData.empty() ? uint64_t{0} : hierarchy.microSurfaceData[materialAddress], false};
      auto [iterator, inserted] = sources.emplace(
          CacheKey{parent, handle}, source);
      if (!inserted && !sameSource(iterator->second, source))
        iterator->second.ambiguous = true;
    }
  }
  if (sources.size() > kMaximumSparseVoxelGeometryFieldCacheRecords)
    return fail(error, "Geometry field cache exceeds its bounded record capacity");

  std::vector<SparseVoxelGeometryFieldCacheRecordGPU> records;
  records.reserve(sources.size());
  for (const auto &[key, source] : sources)
  {
    const auto &[parent, handle] = key;
    const auto &slot = materials.slot(handle);
    const auto descriptor = unpackSparseVoxelMicroGeometry(slot.descriptor);
    const uint32_t subdivision = descriptor.subdivision;
    if (subdivision < 2u ||
        subdivision > kSparseVoxelMicroMaximumSubdivision)
      continue;
    SparseVoxelGeometryFieldCacheRecordGPU record{};
    record.canonicalParent = parent;
    record.materialAndFlags = kSparseVoxelGeometryFieldCacheValid |
        handle |
        (subdivision << kSparseVoxelGeometryFieldCacheSubdivisionShift);
    bool conservative = source.ambiguous;
    if (!conservative)
    {
      const uint32_t required = material::voxelGeometryRequiredMaterialFields(
          slot.geometryProgram);
      const uint32_t context = unpackSparseVoxelMicroContextValue(
          source.surfaceData);
      const uint32_t face = firstGeometryFace(descriptor, context);
      for (uint32_t z = 0u; z < subdivision && !conservative; ++z)
        for (uint32_t y = 0u; y < subdivision && !conservative; ++y)
          for (uint32_t x = 0u; x < subdivision; ++x)
          {
            const std::array<uint32_t, 3> cell{x, y, z};
            const auto address = voxelMaterialSampleAddress(
                parent, cell, 1u, subdivision, source.regionMappingId);
            const auto fields = material::sampleVoxelMaterialFieldsFixed(
                atlas, materials.materialFieldDefaults()[handle], handle,
                subdivision, address, required);
            if ((fields.residentMask & required) != required)
            {
              conservative = true;
              break;
            }
            material::VoxelGeometryInputs inputs{};
            inputs.parentKey = parent;
            inputs.volumeCell = cell;
            inputs.face = slot.geometryProgram.domain ==
                    material::VoxelGeometryDomain::Volume
                ? 6u : face;
            inputs.faceCell = sparseVoxelMicroFaceCell(
                cell, subdivision, static_cast<SparseVoxelMicroFace>(face));
            inputs.subdivision = subdivision;
            inputs.materialSeed = descriptor.materialSeed;
            inputs.context = context;
            inputs.fields = fields.values;
            inputs.parameters = slot.geometryParameters;
            inputs.terrainSlope = material::voxelGeometryFixedFromDouble(
                sparseVoxelMicroContextTerrainSlope(context));
            if (inputs.face == static_cast<uint32_t>(
                    SparseVoxelMicroFace::PositiveY) &&
                sparseVoxelMicroContextIsSurface(context))
            {
              const uint32_t depth = sparseVoxelMicroSurfaceColumnDepth(
                  unpackSparseVoxelMicroContextSurfacePatch(
                      source.surfaceData),
                  subdivision, x, z);
              inputs.baseSurface = static_cast<int32_t>(
                  (static_cast<uint64_t>(subdivision - depth) *
                   material::kVoxelGeometryFixedOne) /
                  subdivision);
            }
            bool occupied = true;
            if (!material::voxelGeometryCellOccupied(
                    slot.geometryProgram, inputs, cell, occupied, error))
              return false;
            if (occupied)
            {
              const uint32_t linear =
                  (z * subdivision + y) * subdivision + x;
              record.occupancy[linear / 32u] |= 1u << (linear % 32u);
            }
          }
    }
    if (conservative)
    {
      record.materialAndFlags |=
          kSparseVoxelGeometryFieldCacheConservative;
      record.occupancy.fill(0xffffffffu);
      ++snapshot.conservativeRecordCount;
    }
    records.push_back(record);
  }

  snapshot.recordCount = static_cast<uint32_t>(records.size());
  uint32_t tableCapacity = 2u;
  while (tableCapacity < std::max<size_t>(2u, records.size() * 4u))
    tableCapacity <<= 1u;
  snapshot.table.assign(tableCapacity, {});
  for (const auto &record : records)
  {
    const uint32_t start = sparseVoxelGeometryFieldCacheHash(
        record.canonicalParent, record.materialHandle()) % tableCapacity;
    bool inserted = false;
    for (uint32_t probe = 0u;
         probe < kSparseVoxelGeometryFieldCacheMaximumProbes; ++probe)
    {
      auto &destination = snapshot.table[(start + probe) % tableCapacity];
      if (!destination.valid())
      {
        destination = record;
        inserted = true;
        break;
      }
    }
    if (!inserted)
      return fail(error, "Geometry field cache exceeded its bounded probe budget");
  }

  uint64_t revision = 1469598103934665603ull;
  hash64(revision, static_cast<uint32_t>(atlas.contentRevision));
  hash64(revision, static_cast<uint32_t>(atlas.contentRevision >> 32u));
  hash64(revision, static_cast<uint32_t>(materials.geometryRevision()));
  hash64(revision, static_cast<uint32_t>(materials.geometryRevision() >> 32u));
  for (const auto &record : snapshot.table)
  {
    for (const int32_t coordinate : record.canonicalParent)
      hash64(revision, static_cast<uint32_t>(coordinate));
    hash64(revision, record.materialAndFlags);
    for (const uint32_t word : record.occupancy)
      hash64(revision, word);
  }
  snapshot.contentRevision = revision == 0u ? 1u : revision;
  return true;
}

bool sparseVoxelGeometryFieldCacheOccupied(
    const std::vector<SparseVoxelGeometryFieldCacheRecordGPU> &table,
    const std::array<int32_t, 3> &canonicalParent,
    const material::VoxelMaterialHandle materialHandle,
    const std::array<uint32_t, 3> &microCell, bool *found)
{
  const auto *record = findSparseVoxelGeometryFieldCacheRecord(
      table, canonicalParent, materialHandle);
  if (found != nullptr)
    *found = record != nullptr;
  if (record == nullptr || record->conservative())
    return true;
  const uint32_t subdivision = record->subdivision();
  if (subdivision == 0u ||
      std::any_of(
          microCell.begin(), microCell.end(),
          [subdivision](const uint32_t value)
          { return value >= subdivision; }))
    return true;
  const uint32_t linear =
      (microCell[2] * subdivision + microCell[1]) * subdivision +
      microCell[0];
  return (record->occupancy[linear / 32u] &
          (1u << (linear % 32u))) != 0u;
}

} // namespace voxel::svraster
