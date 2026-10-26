#include "voxel/material/VoxelMaterialFieldAtlas.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace voxel::material
{
namespace
{

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool validTile(const VoxelMaterialFieldTile &tile, std::string *error)
{
  if (tile.width != kVoxelMaterialFieldAtlasTileSide ||
      tile.height != kVoxelMaterialFieldAtlasTileSide ||
      tile.values.size() != static_cast<size_t>(tile.width) * tile.height)
    return fail(error, "Voxel material field tile must be one bounded 32x32 R8 tile");
  if (tile.canonicalExtent[0] == 0u || tile.canonicalExtent[1] == 0u)
    return fail(error, "Voxel material field tile canonical extent is zero");
  if (tile.canonicalExtent[0] >
          static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
      tile.canonicalExtent[1] >
          static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
    return fail(error, "Voxel material field tile canonical extent exceeds signed addressing");
  for (uint32_t axis = 0u; axis < 2u; ++axis)
    if (static_cast<int64_t>(tile.canonicalOrigin[axis]) +
            tile.canonicalExtent[axis] - 1ll >
        std::numeric_limits<int32_t>::max())
      return fail(error, "Voxel material field tile canonical range exceeds signed addressing");
  return true;
}

uint32_t mix(uint32_t value)
{
  value ^= value >> 16u;
  value *= 0x7feb352du;
  value ^= value >> 15u;
  value *= 0x846ca68bu;
  return value ^ (value >> 16u);
}

struct ComposedTile
{
  VoxelMaterialHandle materialHandle = 0u;
  uint32_t regionMappingId = 0u;
  uint32_t channelMask = 0u;
  std::array<int32_t, 2> canonicalOrigin{};
  std::array<uint32_t, 2> canonicalExtent{
    kVoxelMaterialFieldAtlasTileSide,
    kVoxelMaterialFieldAtlasTileSide};
  std::vector<uint8_t> rgba8 = std::vector<uint8_t>(
      kVoxelMaterialFieldAtlasTileSide *
          kVoxelMaterialFieldAtlasTileSide * 4u,
      0u);
};

bool addChannel(
    ComposedTile &destination, const uint32_t slot,
    const VoxelMaterialFieldTile &source, const bool requireSameMapping,
    std::string *error)
{
  if (requireSameMapping && destination.channelMask != 0u &&
      (destination.canonicalOrigin != source.canonicalOrigin ||
       destination.canonicalExtent != source.canonicalExtent))
    return fail(error, "Voxel material region channels use incompatible canonical mappings");
  if (destination.channelMask == 0u)
  {
    destination.canonicalOrigin = source.canonicalOrigin;
    destination.canonicalExtent = source.canonicalExtent;
  }
  for (size_t texel = 0u; texel < source.values.size(); ++texel)
    destination.rgba8[texel * 4u + slot] = source.values[texel];
  destination.channelMask |= 1u << slot;
  return true;
}

int64_t positiveModulo(const int64_t value, const int64_t divisor)
{
  const int64_t remainder = value % divisor;
  return remainder < 0 ? remainder + divisor : remainder;
}

void hashRevision(uint64_t &hash, const uint32_t value)
{
  for (uint32_t byte = 0u; byte < 4u; ++byte)
  {
    hash ^= static_cast<uint8_t>(value >> (byte * 8u));
    hash *= 1099511628211ull;
  }
}

void hashRevision(uint64_t &hash, const int32_t value)
{
  hashRevision(hash, static_cast<uint32_t>(value));
}

void hashRevision(uint64_t &hash, const float value)
{
  uint32_t bits = 0u;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  hashRevision(hash, bits);
}

} // namespace

bool VoxelMaterialFieldTile::operator==(
    const VoxelMaterialFieldTile &other) const
{
  return width == other.width && height == other.height &&
      values == other.values && canonicalOrigin == other.canonicalOrigin &&
      canonicalExtent == other.canonicalExtent;
}

uint32_t packVoxelMaterialFieldControl(
    const VoxelMaterialFieldBinding &binding)
{
  return (static_cast<uint32_t>(binding.source) & 0x3u) |
      ((static_cast<uint32_t>(binding.mapping) & 0x3u) << 2u) |
      ((static_cast<uint32_t>(binding.filter) & 0x1u) << 4u) |
      ((static_cast<uint32_t>(binding.addressMode) & 0x1u) << 5u);
}

uint32_t voxelMaterialFieldMappingHash(
    const VoxelMaterialHandle materialHandle, const uint32_t regionMappingId)
{
  return mix(static_cast<uint32_t>(materialHandle) * 0x9e3779b9u ^
      mix(regionMappingId ^ 0x85ebca6bu));
}

const VoxelMaterialFieldMappingGPU *findVoxelMaterialFieldMapping(
    const std::vector<VoxelMaterialFieldMappingGPU> &table,
    const VoxelMaterialHandle materialHandle, const uint32_t regionMappingId,
    uint32_t *probes)
{
  if (probes != nullptr)
    *probes = 0u;
  if (table.empty())
    return nullptr;
  const uint32_t start = voxelMaterialFieldMappingHash(
      materialHandle, regionMappingId) % static_cast<uint32_t>(table.size());
  for (uint32_t probe = 0u;
       probe < kVoxelMaterialFieldMappingMaximumProbes; ++probe)
  {
    if (probes != nullptr)
      ++*probes;
    const auto &record = table[(start + probe) % table.size()];
    if (!record.valid())
      return nullptr;
    if (record.materialHandle() == materialHandle &&
        record.regionMappingId == regionMappingId)
      return &record;
  }
  return nullptr;
}

bool VoxelMaterialFieldAtlas::setTextureAsset(
    const std::string &assetId, const VoxelMaterialFieldTile &tile,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (assetId.empty())
    return fail(error, "Voxel material field texture asset ID is empty");
  if (!validTile(tile, error))
    return false;
  textureAssets_[assetId] = tile;
  return true;
}

bool VoxelMaterialFieldAtlas::setPlanetRegionField(
    const VoxelMaterialHandle materialHandle, const uint32_t regionMappingId,
    const uint32_t fieldSlot, const VoxelMaterialFieldTile &tile,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (materialHandle == kInvalidVoxelMaterialHandle || regionMappingId == 0u ||
      fieldSlot >= kVoxelMaterialFieldCount)
    return fail(error, "Voxel material planet-region field key is invalid");
  if (!validTile(tile, error))
    return false;
  regionFields_[{materialHandle, regionMappingId, fieldSlot}] = tile;
  return true;
}

void VoxelMaterialFieldAtlas::clearTextureAsset(const std::string &assetId)
{
  textureAssets_.erase(assetId);
}

void VoxelMaterialFieldAtlas::clearPlanetRegion(const uint32_t regionMappingId)
{
  for (auto iterator = regionFields_.begin(); iterator != regionFields_.end();)
  {
    if (std::get<1>(iterator->first) == regionMappingId)
      iterator = regionFields_.erase(iterator);
    else
      ++iterator;
  }
}

void VoxelMaterialFieldAtlas::clearPlanetRegions()
{
  regionFields_.clear();
}

void VoxelMaterialFieldAtlas::clear()
{
  textureAssets_.clear();
  regionFields_.clear();
}

bool VoxelMaterialFieldAtlas::build(
    const VoxelMaterialManager &materials,
    VoxelMaterialFieldAtlasSnapshot &snapshot, std::string *error) const
{
  if (error != nullptr)
    error->clear();
  snapshot = {};
  snapshot.resolvers.resize(materials.slotCount());
  std::map<std::pair<VoxelMaterialHandle, uint32_t>, ComposedTile> composed;
  for (uint32_t handle = 0u; handle < materials.slotCount(); ++handle)
  {
    const auto materialHandle = static_cast<VoxelMaterialHandle>(handle);
    const auto &bindings = materials.slot(materialHandle).fieldBindings;
    auto &resolver = snapshot.resolvers[handle];
    for (uint32_t slot = 0u; slot < kVoxelMaterialFieldCount; ++slot)
    {
      const auto &binding = bindings[slot];
      resolver.controls[slot] = packVoxelMaterialFieldControl(binding);
      resolver.scale[slot] = binding.scale;
      resolver.bias[slot] = binding.bias;
      if (binding.source != VoxelMaterialFieldSource::MaterialTexture)
        continue;
      const auto source = textureAssets_.find(binding.textureAssetId);
      if (source == textureAssets_.end())
        continue;
      auto &tile = composed[{materialHandle, 0u}];
      tile.materialHandle = materialHandle;
      if (!addChannel(tile, slot, source->second, true, error))
        return false;
    }
  }
  for (const auto &[key, source] : regionFields_)
  {
    const auto [materialHandle, regionMappingId, slot] = key;
    if (materialHandle >= materials.slotCount())
      return fail(error, "Voxel material region field references an invalid material handle");
    const auto &binding = materials.slot(materialHandle).fieldBindings[slot];
    if (binding.source != VoxelMaterialFieldSource::PlanetRegion)
      return fail(error, "Voxel material region data targets a non-region field slot");
    auto &tile = composed[{materialHandle, regionMappingId}];
    tile.materialHandle = materialHandle;
    tile.regionMappingId = regionMappingId;
    if (!addChannel(tile, slot, source, true, error))
      return false;
  }
  if (composed.size() > kMaximumVoxelMaterialFieldMappings)
    return fail(error, "Voxel material field atlas exceeds its bounded mapping capacity");

  // Multiple materials commonly bind the same generated planet channels for
  // one region. Mapping records remain material-specific, but their immutable
  // RGBA payload is stored once.
  std::vector<const ComposedTile *> physicalTiles;
  std::vector<uint32_t> physicalIndices;
  std::map<uint64_t, std::vector<uint32_t>> physicalByHash;
  physicalIndices.reserve(composed.size());
  for (const auto &[key, tile] : composed)
  {
    (void)key;
    uint64_t hash = 1469598103934665603ull;
    for (const uint8_t byte : tile.rgba8)
    {
      hash ^= byte;
      hash *= 1099511628211ull;
    }
    uint32_t physicalIndex = std::numeric_limits<uint32_t>::max();
    for (const uint32_t candidate : physicalByHash[hash])
      if (physicalTiles[candidate]->rgba8 == tile.rgba8)
      {
        physicalIndex = candidate;
        break;
      }
    if (physicalIndex == std::numeric_limits<uint32_t>::max())
    {
      physicalIndex = static_cast<uint32_t>(physicalTiles.size());
      physicalTiles.push_back(&tile);
      physicalByHash[hash].push_back(physicalIndex);
    }
    physicalIndices.push_back(physicalIndex);
  }
  if (physicalTiles.size() > kMaximumVoxelMaterialFieldAtlasTiles)
    return fail(error, "Voxel material field atlas exceeds its bounded tile capacity");

  const uint32_t tileCount = static_cast<uint32_t>(physicalTiles.size());
  const uint32_t tilesPerRow = tileCount == 0u ? 0u :
      static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(tileCount))));
  const uint32_t rows = tileCount == 0u ? 0u :
      (tileCount + tilesPerRow - 1u) / tilesPerRow;
  snapshot.width = tileCount == 0u ? 1u : tilesPerRow * snapshot.tileSide;
  snapshot.height = tileCount == 0u ? 1u : rows * snapshot.tileSide;
  snapshot.rgba8.assign(
      static_cast<size_t>(snapshot.width) * snapshot.height * 4u, 0u);

  for (uint32_t tileIndex = 0u; tileIndex < tileCount; ++tileIndex)
  {
    const auto &tile = *physicalTiles[tileIndex];
    const uint32_t tileX = tileIndex % tilesPerRow;
    const uint32_t tileY = tileIndex / tilesPerRow;
    const uint32_t originX = tileX * snapshot.tileSide;
    const uint32_t originY = tileY * snapshot.tileSide;
    for (uint32_t y = 0u; y < snapshot.tileSide; ++y)
      for (uint32_t x = 0u; x < snapshot.tileSide; ++x)
      {
        const size_t source = (static_cast<size_t>(y) * snapshot.tileSide + x) * 4u;
        const size_t destination =
            (static_cast<size_t>(originY + y) * snapshot.width + originX + x) * 4u;
        std::copy_n(tile.rgba8.begin() + source, 4u,
            snapshot.rgba8.begin() + destination);
      }
  }

  std::vector<VoxelMaterialFieldMappingGPU> records;
  records.reserve(composed.size());
  uint32_t mappingIndex = 0u;
  for (const auto &[key, tile] : composed)
  {
    (void)key;
    const uint32_t tileIndex = physicalIndices[mappingIndex++];
    VoxelMaterialFieldMappingGPU record{};
    record.materialAndFlags = kVoxelMaterialFieldMappingValid |
        tile.materialHandle |
        (tile.channelMask << kVoxelMaterialFieldMappingChannelShift);
    record.regionMappingId = tile.regionMappingId;
    record.atlasOrigin = {
      (tileIndex % tilesPerRow) * snapshot.tileSide,
      (tileIndex / tilesPerRow) * snapshot.tileSide};
    record.atlasExtent = {snapshot.tileSide, snapshot.tileSide};
    record.canonicalOrigin = tile.canonicalOrigin;
    record.canonicalExtent = tile.canonicalExtent;
    records.push_back(record);
  }

  const size_t tableCapacity = records.empty()
      ? 2u : kVoxelMaterialFieldMappingTableEntries;
  snapshot.mappingTable.assign(tableCapacity, {});
  for (const auto &record : records)
  {
    const uint32_t start = voxelMaterialFieldMappingHash(
        record.materialHandle(), record.regionMappingId) %
        static_cast<uint32_t>(tableCapacity);
    bool inserted = false;
    for (uint32_t probe = 0u;
         probe < kVoxelMaterialFieldMappingMaximumProbes; ++probe)
    {
      auto &destination = snapshot.mappingTable[(start + probe) % tableCapacity];
      if (!destination.valid())
      {
        destination = record;
        inserted = true;
        break;
      }
    }
    if (!inserted)
    {
      snapshot = {};
      return fail(error, "Voxel material field mapping table exceeded its bounded probe budget");
    }
  }
  uint64_t revision = 1469598103934665603ull;
  hashRevision(revision, snapshot.width);
  hashRevision(revision, snapshot.height);
  for (const uint8_t value : snapshot.rgba8)
  {
    revision ^= value;
    revision *= 1099511628211ull;
  }
  for (const auto &resolver : snapshot.resolvers)
    for (uint32_t slot = 0u; slot < kVoxelMaterialFieldCount; ++slot)
    {
      hashRevision(revision, resolver.controls[slot]);
      hashRevision(revision, resolver.scale[slot]);
      hashRevision(revision, resolver.bias[slot]);
    }
  for (const auto &mapping : snapshot.mappingTable)
  {
    hashRevision(revision, mapping.materialAndFlags);
    hashRevision(revision, mapping.regionMappingId);
    for (uint32_t axis = 0u; axis < 2u; ++axis)
    {
      hashRevision(revision, mapping.atlasOrigin[axis]);
      hashRevision(revision, mapping.atlasExtent[axis]);
      hashRevision(revision, mapping.canonicalOrigin[axis]);
      hashRevision(revision, mapping.canonicalExtent[axis]);
    }
  }
  snapshot.contentRevision = revision == 0u ? 1u : revision;
  return true;
}

std::array<float, kVoxelMaterialFieldCount> sampleVoxelMaterialFields(
    const VoxelMaterialFieldAtlasSnapshot &snapshot,
    const VoxelMaterialFieldDefaultsGPU &defaults,
    const VoxelMaterialHandle materialHandle, const uint32_t subdivision,
    const svraster::VoxelMaterialSampleAddressGPU &address,
    const uint32_t requiredMask)
{
  auto result = voxelMaterialFieldDefaultsFloat(defaults);
  if (materialHandle >= snapshot.resolvers.size() || snapshot.width == 0u ||
      snapshot.height == 0u || snapshot.rgba8.size() !=
          static_cast<size_t>(snapshot.width) * snapshot.height * 4u)
    return result;
  const auto &resolver = snapshot.resolvers[materialHandle];
  for (uint32_t slot = 0u; slot < kVoxelMaterialFieldCount; ++slot)
  {
    if ((requiredMask & (1u << slot)) == 0u)
      continue;
    const uint32_t control = resolver.controls[slot];
    const auto source = static_cast<VoxelMaterialFieldSource>(control & 0x3u);
    if (source == VoxelMaterialFieldSource::Constant)
      continue;
    const uint32_t region = source == VoxelMaterialFieldSource::PlanetRegion
        ? address.regionMappingId : 0u;
    const auto *mapping = findVoxelMaterialFieldMapping(
        snapshot.mappingTable, materialHandle, region);
    if (mapping == nullptr || (mapping->channelMask() & (1u << slot)) == 0u)
      continue;
    const auto mappingMode = static_cast<VoxelMaterialFieldMapping>(
        (control >> 2u) & 0x3u);
    const bool linear = ((control >> 4u) & 1u) != 0u;
    const bool repeat = ((control >> 5u) & 1u) != 0u;
    std::array<int64_t, 2> coordinate{
      address.canonicalMicroCoordinate[0],
      address.canonicalMicroCoordinate[2]};
    std::array<int64_t, 2> logicalOrigin{
      mapping->canonicalOrigin[0], mapping->canonicalOrigin[1]};
    std::array<uint32_t, 2> logicalExtent = mapping->canonicalExtent;
    if (mappingMode == VoxelMaterialFieldMapping::ParentLocal)
    {
      logicalOrigin = {0, 0};
      logicalExtent = {std::max(subdivision, 1u), std::max(subdivision, 1u)};
    }
    std::array<double, 2> texel{};
    for (uint32_t axis = 0u; axis < 2u; ++axis)
    {
      int64_t local = mappingMode == VoxelMaterialFieldMapping::ParentLocal
          ? positiveModulo(coordinate[axis], logicalExtent[axis])
          : coordinate[axis] - logicalOrigin[axis];
      if (mappingMode != VoxelMaterialFieldMapping::ParentLocal)
        local = repeat
            ? positiveModulo(local, logicalExtent[axis])
            : std::clamp<int64_t>(local, 0, logicalExtent[axis] - 1u);
      texel[axis] =
          (static_cast<double>(local) + 0.5) * mapping->atlasExtent[axis] /
              logicalExtent[axis] -
          0.5;
    }
    const auto load = [&](int64_t x, int64_t y)
    {
      const auto addressAxis = [&](const int64_t value, const uint32_t extent)
      {
        return repeat ? positiveModulo(value, extent)
                      : std::clamp<int64_t>(value, 0, extent - 1u);
      };
      x = addressAxis(x, mapping->atlasExtent[0]) + mapping->atlasOrigin[0];
      y = addressAxis(y, mapping->atlasExtent[1]) + mapping->atlasOrigin[1];
      return static_cast<float>(snapshot.rgba8[
          (static_cast<size_t>(y) * snapshot.width + x) * 4u + slot]) /
          255.0f;
    };
    float value = 0.0f;
    if (!linear)
      value = load(static_cast<int64_t>(std::floor(texel[0] + 0.5)),
          static_cast<int64_t>(std::floor(texel[1] + 0.5)));
    else
    {
      const int64_t x0 = static_cast<int64_t>(std::floor(texel[0]));
      const int64_t y0 = static_cast<int64_t>(std::floor(texel[1]));
      const float fx = static_cast<float>(texel[0] - std::floor(texel[0]));
      const float fy = static_cast<float>(texel[1] - std::floor(texel[1]));
      const auto lerp = [](const float from, const float to, const float weight)
      {
        return from + (to - from) * weight;
      };
      value = lerp(
          lerp(load(x0, y0), load(x0 + 1, y0), fx),
          lerp(load(x0, y0 + 1), load(x0 + 1, y0 + 1), fx), fy);
    }
    result[slot] = std::clamp(
        value * resolver.scale[slot] + resolver.bias[slot], 0.0f, 1.0f);
  }
  return result;
}

VoxelMaterialFieldFixedSample sampleVoxelMaterialFieldsFixed(
    const VoxelMaterialFieldAtlasSnapshot &snapshot,
    const VoxelMaterialFieldDefaultsGPU &defaults,
    const VoxelMaterialHandle materialHandle, const uint32_t subdivision,
    const svraster::VoxelMaterialSampleAddressGPU &address,
    const uint32_t requiredMask)
{
  VoxelMaterialFieldFixedSample result{};
  result.values = voxelMaterialFieldDefaultsFixed(defaults);
  if (materialHandle >= snapshot.resolvers.size() || snapshot.width == 0u ||
      snapshot.height == 0u || snapshot.rgba8.size() !=
          static_cast<size_t>(snapshot.width) * snapshot.height * 4u)
    return result;
  const auto &resolver = snapshot.resolvers[materialHandle];
  for (uint32_t slot = 0u; slot < kVoxelMaterialFieldCount; ++slot)
  {
    const uint32_t slotMask = 1u << slot;
    if ((requiredMask & slotMask) == 0u)
      continue;
    const uint32_t control = resolver.controls[slot];
    const auto source = static_cast<VoxelMaterialFieldSource>(control & 0x3u);
    if (source == VoxelMaterialFieldSource::Constant)
    {
      result.residentMask |= slotMask;
      continue;
    }
    const uint32_t region = source == VoxelMaterialFieldSource::PlanetRegion
        ? address.regionMappingId : 0u;
    const auto *mapping = findVoxelMaterialFieldMapping(
        snapshot.mappingTable, materialHandle, region);
    if (mapping == nullptr || (mapping->channelMask() & slotMask) == 0u)
      continue;
    const auto mappingMode = static_cast<VoxelMaterialFieldMapping>(
        (control >> 2u) & 0x3u);
    const bool repeat = ((control >> 5u) & 1u) != 0u;
    std::array<int64_t, 2> logicalOrigin{
      mapping->canonicalOrigin[0], mapping->canonicalOrigin[1]};
    std::array<uint32_t, 2> logicalExtent = mapping->canonicalExtent;
    if (mappingMode == VoxelMaterialFieldMapping::ParentLocal)
    {
      logicalOrigin = {0, 0};
      logicalExtent = {std::max(subdivision, 1u),
                       std::max(subdivision, 1u)};
    }
    if (logicalExtent[0] == 0u || logicalExtent[1] == 0u ||
        mapping->atlasExtent[0] == 0u || mapping->atlasExtent[1] == 0u)
      continue;
    const std::array<int64_t, 2> coordinate{
      address.canonicalMicroCoordinate[0],
      address.canonicalMicroCoordinate[2]};
    std::array<uint32_t, 2> atlasCoordinate{};
    bool valid = true;
    for (uint32_t axis = 0u; axis < 2u; ++axis)
    {
      int64_t local = mappingMode == VoxelMaterialFieldMapping::ParentLocal
          ? positiveModulo(coordinate[axis], logicalExtent[axis])
          : coordinate[axis] - logicalOrigin[axis];
      if (mappingMode != VoxelMaterialFieldMapping::ParentLocal)
        local = repeat
            ? positiveModulo(local, logicalExtent[axis])
            : std::clamp<int64_t>(local, 0, logicalExtent[axis] - 1u);
      // nearest(round(((local + .5) * atlas / logical) - .5))
      // simplifies to floor((2*local+1)*atlas/(2*logical)).
      const uint64_t numerator =
          static_cast<uint64_t>(local * 2 + 1) *
          mapping->atlasExtent[axis];
      int64_t texel = static_cast<int64_t>(
          numerator / (uint64_t{2u} * logicalExtent[axis]));
      texel = repeat
          ? positiveModulo(texel, mapping->atlasExtent[axis])
          : std::clamp<int64_t>(
                texel, 0, mapping->atlasExtent[axis] - 1u);
      const uint64_t atlas =
          static_cast<uint64_t>(mapping->atlasOrigin[axis]) + texel;
      if (atlas >= (axis == 0u ? snapshot.width : snapshot.height))
      {
        valid = false;
        break;
      }
      atlasCoordinate[axis] = static_cast<uint32_t>(atlas);
    }
    if (!valid)
      continue;
    const uint8_t unorm = snapshot.rgba8[
        (static_cast<size_t>(atlasCoordinate[1]) * snapshot.width +
         atlasCoordinate[0]) * 4u + slot];
    const int32_t sampled = static_cast<int32_t>(
        (static_cast<uint32_t>(unorm) * kVoxelGeometryFixedOne + 127u) /
        255u);
    const int32_t scale = voxelGeometryFixedFromDouble(resolver.scale[slot]);
    const int32_t bias = voxelGeometryFixedFromDouble(resolver.bias[slot]);
    result.values[slot] = static_cast<int32_t>(std::clamp<int64_t>(
        (static_cast<int64_t>(sampled) * scale) /
                kVoxelGeometryFixedOne +
            bias,
        0, kVoxelGeometryFixedOne));
    result.residentMask |= slotMask;
  }
  return result;
}

} // namespace voxel::material
