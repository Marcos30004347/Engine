#pragma once

#include "voxel/material/VoxelMaterial.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace voxel::material
{

inline constexpr uint32_t kVoxelMaterialFieldAtlasTileSide = 32u;
inline constexpr uint32_t kMaximumVoxelMaterialFieldAtlasTiles = 2048u;
inline constexpr uint32_t kMaximumVoxelMaterialFieldMappings = 4096u;
inline constexpr uint32_t kVoxelMaterialFieldMappingTableEntries = 16384u;
inline constexpr uint32_t kVoxelMaterialFieldAtlasMaximumSide = 1472u;
inline constexpr uint32_t kVoxelMaterialFieldMappingMaximumProbes = 8u;
inline constexpr uint32_t kVoxelMaterialFieldMappingValid = 1u << 31u;
inline constexpr uint32_t kVoxelMaterialFieldMappingChannelShift = 16u;
inline constexpr uint32_t kVoxelMaterialFieldMappingChannelMask = 0x0fu;

struct VoxelMaterialFieldTile
{
  uint32_t width = kVoxelMaterialFieldAtlasTileSide;
  uint32_t height = kVoxelMaterialFieldAtlasTileSide;
  // R8 scalar values in row-major order.
  std::vector<uint8_t> values;
  // Canonical XZ range represented by the tile. Material-owned repeating
  // textures normally leave these at {0,0}/{tile side,tile side}.
  std::array<int32_t, 2> canonicalOrigin{};
  std::array<uint32_t, 2> canonicalExtent{
    kVoxelMaterialFieldAtlasTileSide,
    kVoxelMaterialFieldAtlasTileSide};

  bool operator==(const VoxelMaterialFieldTile &other) const;
};

// One record per stable material handle. Sampling controls are packed per
// logical field; scale and bias stay scalar so authored values round-trip.
struct alignas(16) VoxelMaterialFieldResolverGPU
{
  std::array<uint32_t, kVoxelMaterialFieldCount> controls{};
  std::array<float, kVoxelMaterialFieldCount> scale{};
  std::array<float, kVoxelMaterialFieldCount> bias{};
};
static_assert(sizeof(VoxelMaterialFieldResolverGPU) == 48u);

// Open-addressed lookup keyed by (stable material handle, logical region ID).
// Atlas coordinates are physical, but remain behind this table and never enter
// resolved surfaces or persistent material identities.
struct alignas(16) VoxelMaterialFieldMappingGPU
{
  uint32_t materialAndFlags = 0u;
  uint32_t regionMappingId = 0u;
  std::array<uint32_t, 2> atlasOrigin{};
  std::array<uint32_t, 2> atlasExtent{};
  std::array<int32_t, 2> canonicalOrigin{};
  std::array<uint32_t, 2> canonicalExtent{};
  uint32_t reserved0 = 0u;
  uint32_t reserved1 = 0u;

  bool valid() const
  {
    return (materialAndFlags & kVoxelMaterialFieldMappingValid) != 0u;
  }
  VoxelMaterialHandle materialHandle() const
  {
    return static_cast<VoxelMaterialHandle>(materialAndFlags & 0xffffu);
  }
  uint32_t channelMask() const
  {
    return (materialAndFlags >> kVoxelMaterialFieldMappingChannelShift) &
        kVoxelMaterialFieldMappingChannelMask;
  }
};
static_assert(sizeof(VoxelMaterialFieldMappingGPU) == 48u);

struct VoxelMaterialFieldAtlasSnapshot
{
  uint32_t width = 1u;
  uint32_t height = 1u;
  uint32_t tileSide = kVoxelMaterialFieldAtlasTileSide;
  // RGBA8Uint: one channel for each logical material-field slot.
  std::vector<uint8_t> rgba8{0u, 0u, 0u, 0u};
  std::vector<VoxelMaterialFieldResolverGPU> resolvers;
  std::vector<VoxelMaterialFieldMappingGPU> mappingTable;
  // Deterministic identity of the resolved atlas, mappings, and sampling
  // controls. Geometry materialization folds this into its own revision so a
  // changed carve source invalidates cached visibility without exposing a
  // physical atlas allocation as persistent identity.
  uint64_t contentRevision = 0u;
};

struct VoxelMaterialFieldFixedSample
{
  std::array<VoxelGeometryFixed, kVoxelMaterialFieldCount> values{};
  // Required constant fields and resident texture channels set their bit.
  // Geometry materialization treats any missing required bit as no-carve.
  uint32_t residentMask = 0u;
};

class VoxelMaterialFieldAtlas
{
public:
  bool setTextureAsset(
      const std::string &assetId, const VoxelMaterialFieldTile &tile,
      std::string *error = nullptr);
  bool setPlanetRegionField(
      VoxelMaterialHandle materialHandle, uint32_t regionMappingId,
      uint32_t fieldSlot, const VoxelMaterialFieldTile &tile,
      std::string *error = nullptr);
  void clearTextureAsset(const std::string &assetId);
  void clearPlanetRegion(uint32_t regionMappingId);
  void clearPlanetRegions();
  void clear();

  bool build(
      const VoxelMaterialManager &materials,
      VoxelMaterialFieldAtlasSnapshot &snapshot,
      std::string *error = nullptr) const;

private:
  using RegionFieldKey = std::tuple<VoxelMaterialHandle, uint32_t, uint32_t>;
  std::map<std::string, VoxelMaterialFieldTile> textureAssets_;
  std::map<RegionFieldKey, VoxelMaterialFieldTile> regionFields_;
};

uint32_t packVoxelMaterialFieldControl(
    const VoxelMaterialFieldBinding &binding);
uint32_t voxelMaterialFieldMappingHash(
    VoxelMaterialHandle materialHandle, uint32_t regionMappingId);
const VoxelMaterialFieldMappingGPU *findVoxelMaterialFieldMapping(
    const std::vector<VoxelMaterialFieldMappingGPU> &table,
    VoxelMaterialHandle materialHandle, uint32_t regionMappingId,
    uint32_t *probes = nullptr);

// CPU reference for the pre-IR resolver used by tests and deterministic tools.
std::array<float, kVoxelMaterialFieldCount> sampleVoxelMaterialFields(
    const VoxelMaterialFieldAtlasSnapshot &snapshot,
    const VoxelMaterialFieldDefaultsGPU &defaults,
    VoxelMaterialHandle materialHandle, uint32_t subdivision,
    const svraster::VoxelMaterialSampleAddressGPU &address,
    uint32_t requiredMask = (1u << kVoxelMaterialFieldCount) - 1u);

// Geometry resolver: mip zero, nearest texel, and Q16.16 arithmetic only.
// Authored linear filtering remains available to shading but is deliberately
// ignored here; manager validation rejects it for geometry-consumed slots.
VoxelMaterialFieldFixedSample sampleVoxelMaterialFieldsFixed(
    const VoxelMaterialFieldAtlasSnapshot &snapshot,
    const VoxelMaterialFieldDefaultsGPU &defaults,
    VoxelMaterialHandle materialHandle, uint32_t subdivision,
    const svraster::VoxelMaterialSampleAddressGPU &address,
    uint32_t requiredMask);

} // namespace voxel::material
