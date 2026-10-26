#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelMicroGeometry.hpp"
#include "voxel/svraster/SparseVoxelMicroMaterial.hpp"

#include <algorithm>
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{

using namespace voxel::svraster;

uint32_t nextRandom(uint32_t &state)
{
  state = state * 1664525u + 1013904223u;
  return state;
}

double randomUnit(uint32_t &state)
{
  return static_cast<double>(nextRandom(state) >> 8u) / static_cast<double>(0x00ffffffu);
}

bool intersectBounds(
    const SparseVoxelMicroRay &ray,
    const std::array<double, 3> &minimum,
    const std::array<double, 3> &maximum,
    double &entry)
{
  entry = ray.minimumDistance;
  double exit = ray.maximumDistance;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (ray.direction[axis] == 0.0)
    {
      if (ray.origin[axis] < minimum[axis] || ray.origin[axis] > maximum[axis])
        return false;
      continue;
    }
    double nearDistance = (minimum[axis] - ray.origin[axis]) / ray.direction[axis];
    double farDistance = (maximum[axis] - ray.origin[axis]) / ray.direction[axis];
    if (nearDistance > farDistance)
      std::swap(nearDistance, farDistance);
    entry = std::max(entry, nearDistance);
    exit = std::min(exit, farDistance);
    if (exit + 1.0e-12 < entry)
      return false;
  }
  return exit >= ray.minimumDistance;
}

SparseVoxelMicroTraceResult bruteForceTrace(const SparseVoxelMicroDescriptor &descriptor, const std::array<int32_t, 3> &parentKey, const SparseVoxelMicroRay &ray, const uint32_t context = 0u)
{
  SparseVoxelMicroTraceResult result{};
  if (!sparseVoxelMicroDescriptorIsValid(descriptor))
    return result;
  const uint32_t subdivision = std::max(descriptor.subdivision, 1u);
  double closest = std::numeric_limits<double>::infinity();
  for (uint32_t z = 0u; z < subdivision; ++z)
  {
    for (uint32_t y = 0u; y < subdivision; ++y)
    {
      for (uint32_t x = 0u; x < subdivision; ++x)
      {
        const std::array<uint32_t, 3> cell{x, y, z};
        std::array<double, 3> minimum{};
        std::array<double, 3> maximum{};
        if (!sparseVoxelMicroCellBounds(
                descriptor, parentKey, cell, minimum, maximum, context))
          continue;
        double entry = 0.0;
        if (intersectBounds(ray, minimum, maximum, entry) && entry < closest)
        {
          closest = entry;
          result.cell = cell;
        }
      }
    }
  }
  if (std::isfinite(closest))
  {
    result.status = SparseVoxelMicroTraceStatus::Hit;
    result.distance = closest;
  }
  else
  {
    result.status = SparseVoxelMicroTraceStatus::Miss;
  }
  return result;
}

void assertEquivalent(const SparseVoxelMicroTraceResult &reference, const SparseVoxelMicroTraceResult &actual)
{
  assert(reference.status != SparseVoxelMicroTraceStatus::Invalid);
  assert(actual.status != SparseVoxelMicroTraceStatus::Invalid);
  assert(reference.hit() == actual.hit());
  if (reference.hit())
    assert(std::abs(reference.distance - actual.distance) < 1.0e-9);
}

void assertExactTraceEquivalent(
    const SparseVoxelMicroTraceResult &reference,
    const SparseVoxelMicroTraceResult &actual)
{
  assert(reference.status == actual.status);
  assert(reference.steps == actual.steps);
  if (!reference.hit())
    return;
  assert(std::abs(reference.distance - actual.distance) < 1.0e-12);
  assert(reference.cell == actual.cell);
  assert(reference.face == actual.face);
}

SparseVoxelMicroDescriptor descriptor(const uint32_t subdivision, const uint32_t maximumDepth, const uint32_t faceMask, const SparseVoxelMicroGeometryProfile profile, const uint32_t seed = 0x12345678u)
{
  return {subdivision, maximumDepth, faceMask, profile, seed};
}

void testDescriptorValidationAndFaceOrientation()
{
  assert(sparseVoxelMicroDescriptorIsValid({}));
  assert(!sparseVoxelMicroDescriptorIsEnabled({}));
  assert(!sparseVoxelMicroDescriptorIsValid(descriptor(1u, 1u, 1u, SparseVoxelMicroGeometryProfile::Constant)));
  assert(!sparseVoxelMicroDescriptorIsValid(descriptor(9u, 1u, 1u, SparseVoxelMicroGeometryProfile::Constant)));
  assert(!sparseVoxelMicroDescriptorIsValid(descriptor(5u, 6u, 1u, SparseVoxelMicroGeometryProfile::Constant)));
  assert(!sparseVoxelMicroDescriptorIsValid(descriptor(5u, 1u, 1u << 6u, SparseVoxelMicroGeometryProfile::Constant)));
  auto invalidFraction = descriptor(
      5u, 2u, 1u, SparseVoxelMicroGeometryProfile::Constant);
  invalidFraction.carveDepthFraction = 16u;
  assert(!sparseVoxelMicroDescriptorIsValid(invalidFraction));
  invalidFraction.maximumCarveDepth = 5u;
  invalidFraction.carveDepthFraction = 1u;
  assert(!sparseVoxelMicroDescriptorIsValid(invalidFraction));

  const std::array<uint32_t, 3> cell{1u, 2u, 3u};
  assert(sparseVoxelMicroFaceCell(cell, 5u, SparseVoxelMicroFace::NegativeX) == (std::array<uint32_t, 2>{3u, 2u}));
  assert(sparseVoxelMicroFaceCell(cell, 5u, SparseVoxelMicroFace::PositiveX) == (std::array<uint32_t, 2>{1u, 2u}));
  assert(sparseVoxelMicroFaceCell(cell, 5u, SparseVoxelMicroFace::NegativeY) == (std::array<uint32_t, 2>{1u, 3u}));
  assert(sparseVoxelMicroFaceCell(cell, 5u, SparseVoxelMicroFace::PositiveY) == (std::array<uint32_t, 2>{1u, 1u}));
  assert(sparseVoxelMicroFaceCell(cell, 5u, SparseVoxelMicroFace::NegativeZ) == (std::array<uint32_t, 2>{3u, 2u}));
  assert(sparseVoxelMicroFaceCell(cell, 5u, SparseVoxelMicroFace::PositiveZ) == (std::array<uint32_t, 2>{1u, 2u}));
}

void testPackedMaterialABIAndDeduplication()
{
  static_assert(sizeof(SparseVoxelMicroMaterialGPU) == 8u);
  SparseVoxelMicroDescriptor geometry = descriptor(5u, 3u, 1u << static_cast<uint32_t>(SparseVoxelMicroFace::PositiveY), SparseVoxelMicroGeometryProfile::HashColumns, 0x1abu);
  geometry.carveDepthFraction = 8u;
  SparseVoxelMicroShadingDescriptor shading{};
  shading.profile = SparseVoxelMicroShadingProfile::ExposedSides;
  shading.variationStrength = 17u;
  shading.parameter0 = 29u;
  shading.parameter1 = 43u;
  shading.lodHandoff = 71u;
  SparseVoxelMicroMaterialGPU packed{};
  std::string error;
  assert(packSparseVoxelMicroMaterial(geometry, shading, packed, &error));
  assert(sparseVoxelMicroMaterialIsValid(packed));
  const SparseVoxelMicroDescriptor unpackedGeometry = unpackSparseVoxelMicroGeometry(packed);
  assert(unpackedGeometry.subdivision == 5u);
  assert(unpackedGeometry.maximumCarveDepth == 3u);
  assert(unpackedGeometry.carveDepthFraction == 8u);
  assert(unpackedGeometry.faceMask == geometry.faceMask);
  assert(unpackedGeometry.geometryProfile == geometry.geometryProfile);
  assert(unpackedGeometry.materialSeed == 0xabu);
  for (uint32_t u = 0u; u < geometry.subdivision; ++u)
    for (uint32_t v = 0u; v < geometry.subdivision; ++v)
      assert(sparseVoxelMicroCarveDepth(
                 geometry, {-19, 4, 81},
                 SparseVoxelMicroFace::PositiveY, {u, v}) ==
          sparseVoxelMicroCarveDepth(
                 unpackedGeometry, {-19, 4, 81},
                 SparseVoxelMicroFace::PositiveY, {u, v}));
  const SparseVoxelMicroShadingDescriptor unpackedShading = unpackSparseVoxelMicroShading(packed);
  assert(unpackedShading.profile == shading.profile);
  assert(unpackedShading.variationStrength == 17u);
  assert(unpackedShading.parameter0 == 29u);
  assert(unpackedShading.parameter1 == 43u);
  assert(unpackedShading.lodHandoff == 71u);

  SparseVoxelMicroMaterialLibrary library;
  assert(library.descriptors().size() == 1u);
  assert(library.setMaterial(7u, packed, &error));
  assert(library.setMaterial(19u, packed, &error));
  assert(library.descriptorId(7u) != 0u);
  assert(library.descriptorId(7u) == library.descriptorId(19u));
  assert(library.descriptors().size() == 2u);
  assert(library.descriptorId(999u) == 0u);
  assert(!library.setMaterial(2u, SparseVoxelMicroMaterialGPU{0u, 1u}, &error));
}

void testMicroPayloadPacking()
{
  const std::array<uint32_t, 3> cell{1u, 2u, 3u};
  const uint32_t payload = packSparseVoxelMicroPayload(
      0x4a2bu, cell, 5u, SparseVoxelMicroFace::PositiveY);
  assert((payload & kSparseVoxelMicroPayloadDescriptorMask) == 0x4a2bu);
  assert(((payload >> kSparseVoxelMicroPayloadUShift) &
          kSparseVoxelMicroPayloadCoordinateMask) == 1u);
  assert(((payload >> kSparseVoxelMicroPayloadVShift) &
          kSparseVoxelMicroPayloadCoordinateMask) == 1u);
  assert(((payload >> kSparseVoxelMicroPayloadDepthShift) &
          kSparseVoxelMicroPayloadCoordinateMask) == 2u);
  assert((payload & kSparseVoxelMicroPayloadValid) != 0u);
  assert((payload & kSparseVoxelMicroPayloadHeightfield) != 0u);
  assert((payload & kSparseVoxelMicroPayloadTerrainSlope) == 0u);
  // Flat staircase tops must use the same +Y heightfield frame as sloped
  // terrain. Packing a top microcell relative to a lateral face turns its X/Z
  // coordinate into false material depth and produces periodic soil bands.
  const uint32_t flatTopPayload = packSparseVoxelMicroPayload(
      0x4a2bu, {2u, 4u, 1u}, 5u,
      SparseVoxelMicroFace::PositiveY, true, false);
  assert(((flatTopPayload >> kSparseVoxelMicroPayloadDepthShift) &
          kSparseVoxelMicroPayloadCoordinateMask) == 0u);
  const uint32_t terrainSlopePayload = packSparseVoxelMicroPayload(
      0x4a2bu, cell, 5u, SparseVoxelMicroFace::PositiveY, true, true);
  assert((terrainSlopePayload & kSparseVoxelMicroPayloadTerrainSlope) != 0u);
  const uint32_t volumePayload = packSparseVoxelMicroPayload(
      0x4a2bu, cell, 5u, SparseVoxelMicroFace::PositiveY, false);
  assert(((volumePayload >> kSparseVoxelMicroPayloadUShift) &
          kSparseVoxelMicroPayloadCoordinateMask) == cell[0]);
  assert(((volumePayload >> kSparseVoxelMicroPayloadVShift) &
          kSparseVoxelMicroPayloadCoordinateMask) == cell[1]);
  assert(((volumePayload >> kSparseVoxelMicroPayloadDepthShift) &
          kSparseVoxelMicroPayloadCoordinateMask) == cell[2]);
  assert((volumePayload & kSparseVoxelMicroPayloadValid) != 0u);
  assert((volumePayload & kSparseVoxelMicroPayloadHeightfield) == 0u);
  const uint32_t fallbackPayload =
      packSparseVoxelMicroFallbackPayload(0x4a2bu);
  assert((fallbackPayload & kSparseVoxelMicroPayloadDescriptorMask) ==
         0x4a2bu);
  assert((fallbackPayload & kSparseVoxelMicroPayloadValid) != 0u);
  assert((fallbackPayload & kSparseVoxelMicroPayloadFallback) != 0u);
  assert((fallbackPayload & kSparseVoxelMicroPayloadHeightfield) == 0u);
  assert((fallbackPayload & kSparseVoxelMicroPayloadTerrainSlope) == 0u);
  const uint32_t terrainSlopeFallbackPayload =
      packSparseVoxelMicroFallbackPayload(0x4a2bu, true);
  assert((terrainSlopeFallbackPayload &
          kSparseVoxelMicroPayloadTerrainSlope) != 0u);
  assert(packSparseVoxelMicroFallbackPayload(0u) == 0u);
  assert(packSparseVoxelMicroFallbackPayload(0u, true) == 0u);
  assert(packSparseVoxelMicroPayload(
             0u, cell, 5u, SparseVoxelMicroFace::PositiveY) == 0u);
  assert(packSparseVoxelMicroPayload(
             1u, {5u, 0u, 0u}, 5u,
             SparseVoxelMicroFace::NegativeX) == 0u);
}

void testQuantitativeTerrainSlopeContext()
{
  const uint32_t flatSlope = sparseVoxelMicroTerrainSlopeContext(0.0);
  const uint32_t halfSlope = sparseVoxelMicroTerrainSlopeContext(0.5);
  const uint32_t verticalSlope = sparseVoxelMicroTerrainSlopeContext(1.0);
  assert(flatSlope == 0x81u);
  assert(verticalSlope == 0xffu);
  assert(sparseVoxelMicroContextIsSurface(flatSlope));
  assert(sparseVoxelMicroContextTerrainSlope(
             static_cast<uint32_t>(SparseVoxelMicroContext::FlatSurface)) ==
      0.0);
  assert(sparseVoxelMicroContextTerrainSlope(flatSlope) == 0.0);
  assert(std::abs(sparseVoxelMicroContextTerrainSlope(halfSlope) - 0.5) <=
      0.5 / static_cast<double>(kSparseVoxelMicroTerrainSlopeQuantization));
  assert(sparseVoxelMicroContextTerrainSlope(verticalSlope) == 1.0);
  // Code zero was emitted by revision 19 terrain pages and retains its
  // original binary-slope interpretation.
  assert(sparseVoxelMicroContextTerrainSlope(
             kSparseVoxelMicroContextSurfaceFlag) == 1.0);
  assert(sparseVoxelMicroTerrainSlopeContext(
             std::numeric_limits<double>::quiet_NaN()) == flatSlope);
}

void testCanonicalParentKeys()
{
  const std::array<int32_t, 4> brick{-8000, 120, 16'000, 1};
  std::vector<std::array<int32_t, 3>> keys;
  for (uint32_t z = 0u; z < 8u; ++z)
    for (uint32_t y = 0u; y < 8u; ++y)
      for (uint32_t x = 0u; x < 8u; ++x)
        keys.push_back(sparseVoxelMicroCanonicalParentKey(brick, {x, y, z}));
  std::sort(keys.begin(), keys.end());
  assert(std::unique(keys.begin(), keys.end()) == keys.end());
  assert(sparseVoxelMicroCanonicalParentKey(brick, {3u, 4u, 5u}) ==
      (std::array<int32_t, 3>{-7997, 124, 16'005}));

  // Two adjacent bricks address the shared integer grid without consulting
  // their float world minima.
  const std::array<int32_t, 4> rightBrick{-7992, 120, 16'000, 1};
  assert(sparseVoxelMicroCanonicalParentKey(brick, {7u, 2u, 3u})[0] + 1 ==
      sparseVoxelMicroCanonicalParentKey(rightBrick, {0u, 2u, 3u})[0]);

  const std::array<int32_t, 4> coarseBrick{-8000, 120, 16'000, 2};
  assert(sparseVoxelMicroCanonicalParentKey(coarseBrick, {3u, 4u, 5u}) ==
      (std::array<int32_t, 3>{-7994, 128, 16'010}));

  // Canonical face orientation maps both sides of one physical boundary to
  // the same lattice column even though +X reverses its face-relative U axis.
  const auto leftBoundary = sparseVoxelMicroCanonicalFaceColumn(
      {0, 0, 0}, 5u, SparseVoxelMicroFace::PositiveX, {3u, 3u});
  const auto rightBoundary = sparseVoxelMicroCanonicalFaceColumn(
      {1, 0, 0}, 5u, SparseVoxelMicroFace::NegativeX, {1u, 3u});
  assert(leftBoundary == rightBoundary);
  assert(leftBoundary == (std::array<int64_t, 3>{5, 3, 1}));
}

void testGeometricLodHandoff()
{
  assert(sparseVoxelMicroHandoffWeight(0.5) == 0.0);
  assert(sparseVoxelMicroHandoffWeight(1.0) == 1.0);
  assert(sparseVoxelMicroHandoffWeight(0.75) > 0.0);
  assert(sparseVoxelMicroHandoffWeight(0.75) < 1.0);
  assert(std::abs(
      sparseVoxelMicroHandoffWeight(0.9) -
      sparseVoxelMicroHandoffWeight(0.9, 128u)) < 1.0e-12);
  assert(sparseVoxelMicroHandoffWeight(0.6, 64u) >
         sparseVoxelMicroHandoffWeight(0.6));

  const auto constantTop = descriptor(
      5u, 2u,
      1u << static_cast<uint32_t>(SparseVoxelMicroFace::PositiveY),
      SparseVoxelMicroGeometryProfile::Constant);
  assert(std::abs(
      sparseVoxelMicroExpectedCoverage(constantTop) - 0.6) < 1.0e-12);
  const auto constantShell = descriptor(
      5u, 2u, 0x3fu, SparseVoxelMicroGeometryProfile::Constant);
  assert(std::abs(
      sparseVoxelMicroExpectedCoverage(constantShell) - 0.008) < 1.0e-12);
  const auto hashTop = descriptor(
      5u, 2u,
      1u << static_cast<uint32_t>(SparseVoxelMicroFace::PositiveY),
      SparseVoxelMicroGeometryProfile::HashColumns);
  assert(std::abs(
      sparseVoxelMicroExpectedCoverage(hashTop) - 0.8) < 1.0e-12);
  const auto hashShell = descriptor(
      5u, 2u, 0x3fu, SparseVoxelMicroGeometryProfile::HashColumns);
  assert(std::abs(
      sparseVoxelMicroExpectedCoverage(hashShell) -
      std::pow(0.8, 6.0)) < 1.0e-12);
  const auto volume = descriptor(
      5u, 2u, 0x3fu, SparseVoxelMicroGeometryProfile::VolumeDensity);
  assert(std::abs(
      sparseVoxelMicroExpectedCoverage(volume) - 0.6) < 1.0e-12);
  assert(sparseVoxelMicroExpectedCoverage({}) == 1.0);
}

void testBrickPalettePreservesDescriptorIdentity()
{
  std::vector<SparseVoxelCenterLengthGPU> leaves(2u);
  leaves[0].center = {0.5f, 0.5f, 0.5f};
  leaves[1].center = {1.5f, 0.5f, 0.5f};
  leaves[0].length = leaves[1].length = 1.0f;
  std::vector<SparseVoxelMaterialGPU> materials(2u);
  materials[0].albedoRGBA8 = materials[1].albedoRGBA8 = 0xff336699u;
  materials[0].roughnessMetallicUNorm8 = materials[1].roughnessMetallicUNorm8 = 0x00004080u;
  materials[0].materialId = 10u;
  materials[1].materialId = 11u;
  const std::vector<uint32_t> leafIndices{0u, 1u};

  SparseVoxelMicroMaterialLibrary library;
  std::string error;
  assert(library.setMaterial(10u, descriptor(5u, 2u, 1u << static_cast<uint32_t>(SparseVoxelMicroFace::PositiveY), SparseVoxelMicroGeometryProfile::Constant), {}, &error));
  assert(library.setMaterial(11u, descriptor(5u, 4u, 1u << static_cast<uint32_t>(SparseVoxelMicroFace::PositiveY), SparseVoxelMicroGeometryProfile::HashColumns), {}, &error));

  SparseVoxelBrickBuildSettings settings{};
  settings.voxelSize = 1.0f;
  settings.paletteSize = 4u;
  settings.microMaterials = &library;
  SparseVoxelBrickHierarchy micro;
  assert(buildSparseVoxelBrickHierarchy(leaves, materials, leafIndices, settings, micro, &error));
  assert(micro.paletteDescriptorIds.size() == micro.palette.size());
  uint32_t enabledEntries = 0u;
  for (size_t index = 0u; index < micro.palette.size(); ++index)
  {
    const bool enabled = micro.paletteDescriptorIds[index] != 0u;
    assert(((micro.palette[index].packed[1] & kSparseVoxelBrickMicroEnabled) != 0u) == enabled);
    enabledEntries += enabled ? 1u : 0u;
  }
  assert(enabledEntries == 2u);
  assert(micro.materialIndices.size() >= 2u);
  assert(micro.materialIndices[0] != micro.materialIndices[1]);

  SparseVoxelBrickBuildSettings legacySettings = settings;
  legacySettings.microMaterials = nullptr;
  SparseVoxelBrickHierarchy legacy;
  SparseVoxelBrickHierarchy emptyLibrary;
  assert(buildSparseVoxelBrickHierarchy(leaves, materials, leafIndices, legacySettings, legacy, &error));
  SparseVoxelMicroMaterialLibrary disabledLibrary;
  legacySettings.microMaterials = &disabledLibrary;
  assert(buildSparseVoxelBrickHierarchy(leaves, materials, leafIndices, legacySettings, emptyLibrary, &error));
  assert(legacy.materialIndices == emptyLibrary.materialIndices);
  assert(legacy.palette.size() == emptyLibrary.palette.size());
  assert(std::memcmp(legacy.palette.data(), emptyLibrary.palette.data(), legacy.palette.size() * sizeof(legacy.palette[0])) == 0);
  assert(
      std::all_of(
          legacy.paletteDescriptorIds.begin(),
          legacy.paletteDescriptorIds.end(),
          [](const uint16_t id)
          {
            return id == 0u;
          }));
}

void testCarveDepthAndSubtractiveOccupancy()
{
  const std::array<int32_t, 3> key{-17, 29, 5};
  const SparseVoxelMicroDescriptor constant =
      descriptor(5u, 2u, (1u << static_cast<uint32_t>(SparseVoxelMicroFace::NegativeX)) | (1u << static_cast<uint32_t>(SparseVoxelMicroFace::PositiveX)), SparseVoxelMicroGeometryProfile::Constant);
  assert(!sparseVoxelMicroCellOccupied(constant, key, {0u, 2u, 2u}));
  assert(!sparseVoxelMicroCellOccupied(constant, key, {1u, 2u, 2u}));
  assert(sparseVoxelMicroCellOccupied(constant, key, {2u, 2u, 2u}));
  assert(!sparseVoxelMicroCellOccupied(constant, key, {3u, 2u, 2u}));
  assert(!sparseVoxelMicroCellOccupied(constant, key, {4u, 2u, 2u}));

  const SparseVoxelMicroDescriptor hashed = descriptor(8u, 7u, 1u << static_cast<uint32_t>(SparseVoxelMicroFace::PositiveY), SparseVoxelMicroGeometryProfile::HashColumns, 91u);
  for (uint32_t v = 0u; v < 8u; ++v)
  {
    for (uint32_t u = 0u; u < 8u; ++u)
    {
      const double first = sparseVoxelMicroCarveDepth(hashed, key, SparseVoxelMicroFace::PositiveY, {u, v});
      const double second = sparseVoxelMicroCarveDepth(hashed, key, SparseVoxelMicroFace::PositiveY, {u, v});
      assert(first == second);
      assert(first <= sparseVoxelMicroMaximumCarveDepth(hashed));
    }
  }


  const SparseVoxelMicroDescriptor clumped = descriptor(
      5u, 4u,
      1u << static_cast<uint32_t>(SparseVoxelMicroFace::PositiveY),
      SparseVoxelMicroGeometryProfile::ClumpedColumns, 91u);
  // These columns straddle a negative-coordinate parent boundary but remain
  // inside one floor-divided 4-cell cluster. Fine jitter can move either by
  // only one layer from their shared cluster depth.
  const double leftDepth = sparseVoxelMicroCarveDepth(
      clumped, {-2, 0, 0}, SparseVoxelMicroFace::PositiveY, {4u, 3u});
  const double rightDepth = sparseVoxelMicroCarveDepth(
      clumped, {-1, 0, 0}, SparseVoxelMicroFace::PositiveY, {0u, 3u});
  assert(leftDepth <= sparseVoxelMicroMaximumCarveDepth(clumped));
  assert(rightDepth <= sparseVoxelMicroMaximumCarveDepth(clumped));
  assert(std::abs(leftDepth - rightDepth) <= 2.0);
}

SparseVoxelMicroRay randomRay(uint32_t &random, const uint32_t outsideAxis)
{
  SparseVoxelMicroRay ray{};
  const bool positiveSide = (nextRandom(random) & 1u) != 0u;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    ray.origin[axis] = randomUnit(random) * 1.5 - 0.25;
  ray.origin[outsideAxis] = positiveSide ? 2.0 : -1.0;
  std::array<double, 3> target{};
  for (double &coordinate : target)
    coordinate = 0.05 + randomUnit(random) * 0.9;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    ray.direction[axis] = target[axis] - ray.origin[axis];
  return ray;
}

void testSolidStaircaseInteriorShortcutEquivalence()
{
  uint32_t random = 0x6a09e667u;
  const std::array<int32_t, 3> parentKey{-37, 11, 4093};
  const uint32_t interior =
      static_cast<uint32_t>(SparseVoxelMicroContext::Interior);

  for (uint32_t subdivision = 2u;
       subdivision <= kSparseVoxelMicroMaximumSubdivision; ++subdivision)
  {
    const SparseVoxelMicroDescriptor stairs = descriptor(
        subdivision, subdivision, kSparseVoxelStaircaseFaceMask,
        SparseVoxelMicroGeometryProfile::Staircase,
        0x9e3779b9u ^ subdivision);
    for (uint32_t rayIndex = 0u; rayIndex < 192u; ++rayIndex)
    {
      const SparseVoxelMicroRay ray = randomRay(random, rayIndex % 3u);
      const SparseVoxelMicroTraceResult interiorReference =
          traceSparseVoxelMicroVolume(stairs, parentKey, ray, interior);
      const SparseVoxelMicroTraceResult interiorShortcut =
          traceSparseVoxelMicroSolidStaircaseInterior(stairs, ray, interior);
      assertExactTraceEquivalent(interiorReference, interiorShortcut);

      // Downstream payload and identity inputs are deliberately the incumbent
      // raw cell and visible face. Interior remains a volume payload.
      assert(packSparseVoxelMicroPayload(
                 19u, interiorReference.cell, subdivision,
                 SparseVoxelMicroFace::NegativeX, false, false) ==
          packSparseVoxelMicroPayload(
              19u, interiorShortcut.cell, subdivision,
              SparseVoxelMicroFace::NegativeX, false, false));
    }

    // Exercise exact parent corner/edge ties, axis-parallel rays, internal
    // microplanes, and a near-plane-clipped start strictly inside the parent.
    const double boundary =
        static_cast<double>(subdivision / 2u) / subdivision;
    const std::array<SparseVoxelMicroRay, 8u> boundaries{{
      {{{-1.0, -1.0, -1.0}}, {{1.0, 1.0, 1.0}}, 0.0,
       std::numeric_limits<double>::infinity()},
      {{{2.0, 2.0, 2.0}}, {{-1.0, -1.0, -1.0}}, 0.0,
       std::numeric_limits<double>::infinity()},
      {{{-1.0, boundary, boundary}}, {{1.0, 0.0, 0.0}}, 0.0,
       std::numeric_limits<double>::infinity()},
      {{{2.0, boundary, boundary}}, {{-1.0, 0.0, 0.0}}, 0.0,
       std::numeric_limits<double>::infinity()},
      {{{boundary, -1.0, boundary}}, {{0.0, 1.0, 0.0}}, 0.0,
       std::numeric_limits<double>::infinity()},
      {{{boundary, 2.0, boundary}}, {{0.0, -1.0, 0.0}}, 0.0,
       std::numeric_limits<double>::infinity()},
      {{{-1.0, 0.21, 0.63}}, {{1.0, 0.17, -0.09}}, 1.2, 4.0},
      {{{-1.0, 0.25, 0.75}}, {{1.0, 0.0, 0.0}}, 1.0, 1.0},
    }};
    for (const SparseVoxelMicroRay &ray : boundaries)
    {
      assertExactTraceEquivalent(
          traceSparseVoxelMicroVolume(stairs, parentKey, ray, interior),
          traceSparseVoxelMicroSolidStaircaseInterior(
              stairs, ray, interior));
    }
  }

  SparseVoxelMicroDescriptor nonStaircase = descriptor(
      5u, 1u, 1u, SparseVoxelMicroGeometryProfile::Constant);
  const SparseVoxelMicroRay ray{{-1.0, 0.5, 0.5}, {1.0, 0.0, 0.0}};
  assert(traceSparseVoxelMicroSolidStaircaseInterior(
             nonStaircase, ray, interior).status ==
      SparseVoxelMicroTraceStatus::Invalid);
  const SparseVoxelMicroDescriptor stairs = descriptor(
      5u, 5u, kSparseVoxelStaircaseFaceMask,
      SparseVoxelMicroGeometryProfile::Staircase);
  assert(traceSparseVoxelMicroSolidStaircaseInterior(
             stairs, ray,
             static_cast<uint32_t>(
                 SparseVoxelMicroContext::WallPositiveX)).status ==
      SparseVoxelMicroTraceStatus::Invalid);
  assert(traceSparseVoxelMicroSolidStaircaseInterior(
             stairs, ray,
             static_cast<uint32_t>(
                 SparseVoxelMicroContext::FlatSurface)).status ==
      SparseVoxelMicroTraceStatus::Invalid);
  assert(traceSparseVoxelMicroSolidStaircaseInterior(
             stairs, ray, sparseVoxelMicroSurfaceContext(4u)).status ==
      SparseVoxelMicroTraceStatus::Invalid);
}

void testSingleFaceTraversalAgainstBruteForce()
{
  uint32_t random = 0x31415926u;
  const std::array<int32_t, 3> parentKey{-101, 7, 4096};
  for (uint32_t subdivision = 2u;
       subdivision <= kSparseVoxelMicroMaximumSubdivision; ++subdivision)
  {
    for (uint32_t face = 0u; face < 6u; ++face)
    {
      for (const SparseVoxelMicroGeometryProfile profile : {
             SparseVoxelMicroGeometryProfile::Constant,
             SparseVoxelMicroGeometryProfile::HashColumns,
             SparseVoxelMicroGeometryProfile::ClumpedColumns})
      {
        const SparseVoxelMicroDescriptor value = descriptor(subdivision, subdivision, 1u << face, profile, 0x9e3779b9u + subdivision * 17u + face);
        for (uint32_t rayIndex = 0u; rayIndex < 96u; ++rayIndex)
        {
          const SparseVoxelMicroRay ray = randomRay(random, rayIndex % 3u);
          const SparseVoxelMicroTraceResult brute = bruteForceTrace(value, parentKey, ray);
          const SparseVoxelMicroTraceResult heightfield = traceSparseVoxelMicroOneFace(value, parentKey, ray);
          const SparseVoxelMicroTraceResult volume = traceSparseVoxelMicroVolume(value, parentKey, ray);
          assertEquivalent(brute, heightfield);
          assertEquivalent(brute, volume);
          assert(heightfield.steps <= 2u * subdivision + 2u);
          assert(volume.steps <= 3u * subdivision + 3u);
        }
      }
    }
  }
}

void testMultiFaceVolumeTraversalAgainstBruteForce()
{
  uint32_t random = 0x27182818u;
  const std::array<int32_t, 3> parentKey{3, -11, 47};
  for (uint32_t subdivision = 2u;
       subdivision <= kSparseVoxelMicroMaximumSubdivision; ++subdivision)
  {
    for (const SparseVoxelMicroGeometryProfile profile : {
           SparseVoxelMicroGeometryProfile::HashColumns,
           SparseVoxelMicroGeometryProfile::ClumpedColumns,
           SparseVoxelMicroGeometryProfile::VolumeDensity})
    {
      const SparseVoxelMicroDescriptor value = descriptor(
          subdivision, std::max(1u, subdivision / 2u), 0x3fu,
          profile, 0xabcdef01u + subdivision);
      for (uint32_t rayIndex = 0u; rayIndex < 256u; ++rayIndex)
      {
        const SparseVoxelMicroRay ray = randomRay(random, rayIndex % 3u);
        const SparseVoxelMicroTraceResult brute =
            bruteForceTrace(value, parentKey, ray);
        assertEquivalent(
            brute, traceSparseVoxelMicroVolume(value, parentKey, ray));
        if (profile == SparseVoxelMicroGeometryProfile::VolumeDensity)
          assertEquivalent(
              brute, traceSparseVoxelMicroOneFace(value, parentKey, ray));
      }
    }
  }
}

void testVolumeDensityUsesCanonicalWorldCells()
{
  const SparseVoxelMicroDescriptor volume = descriptor(
      5u, 2u, 0x3fu,
      SparseVoxelMicroGeometryProfile::VolumeDensity, 0x5du);
  const std::array<int32_t, 3> leftParent{-21, 7, 103};
  const std::array<int32_t, 3> rightParent{-20, 7, 103};

  for (uint32_t y = 0u; y < volume.subdivision; ++y)
    for (uint32_t z = 0u; z < volume.subdivision; ++z)
    {
      const std::array<uint32_t, 3> leftCell{
        volume.subdivision - 1u, y, z};
      const std::array<uint32_t, 3> rightCell{0u, y, z};
      const auto leftGlobal = sparseVoxelMicroCanonicalCell(
          leftParent, volume.subdivision, leftCell);
      const auto rightGlobal = sparseVoxelMicroCanonicalCell(
          rightParent, volume.subdivision, rightCell);
      assert(leftGlobal[0] + 1 == rightGlobal[0]);
      assert(leftGlobal[1] == rightGlobal[1]);
      assert(leftGlobal[2] == rightGlobal[2]);
    }

  SparseVoxelMicroDescriptor empty = volume;
  empty.maximumCarveDepth = empty.subdivision;
  for (uint32_t x = 0u; x < empty.subdivision; ++x)
    for (uint32_t y = 0u; y < empty.subdivision; ++y)
      for (uint32_t z = 0u; z < empty.subdivision; ++z)
        assert(!sparseVoxelMicroCellOccupied(
            empty, leftParent, {x, y, z}));
}

void testDisabledProfileMatchesSolidParent()
{
  const SparseVoxelMicroDescriptor disabled{};
  const std::array<int32_t, 3> key{};
  const SparseVoxelMicroRay ray{{-1.0, 0.25, 0.75}, {1.0, 0.0, 0.0}};
  const SparseVoxelMicroTraceResult hit = traceSparseVoxelMicroVolume(disabled, key, ray);
  assert(hit.hit());
  assert(std::abs(hit.distance - 1.0) < 1.0e-12);
  assert(hit.face == SparseVoxelMicroFace::NegativeX);
}

void testExactRecessDepthAndFace()
{
  const std::array<int32_t, 3> key{};
  const SparseVoxelMicroDescriptor top = descriptor(5u, 2u, 1u << static_cast<uint32_t>(SparseVoxelMicroFace::PositiveY), SparseVoxelMicroGeometryProfile::Constant);
  const SparseVoxelMicroRay downward{{0.5, 2.0, 0.5}, {0.0, -1.0, 0.0}};
  for (const SparseVoxelMicroTraceResult hit : {
         traceSparseVoxelMicroOneFace(top, key, downward),
         traceSparseVoxelMicroVolume(top, key, downward),
       })
  {
    assert(hit.hit());
    assert(std::abs(hit.distance - 1.4) < 1.0e-12);
    assert(hit.face == SparseVoxelMicroFace::PositiveY);
    assert(hit.cell == (std::array<uint32_t, 3>{2u, 2u, 2u}));
  }

  const SparseVoxelMicroDescriptor side = descriptor(4u, 1u, 1u << static_cast<uint32_t>(SparseVoxelMicroFace::NegativeX), SparseVoxelMicroGeometryProfile::Constant);
  const SparseVoxelMicroRay rightward{{-1.0, 0.5, 0.5}, {1.0, 0.0, 0.0}};
  const SparseVoxelMicroTraceResult hit = traceSparseVoxelMicroOneFace(side, key, rightward);
  assert(hit.hit());
  assert(std::abs(hit.distance - 1.25) < 1.0e-12);
  assert(hit.face == SparseVoxelMicroFace::NegativeX);
  assert(hit.cell == (std::array<uint32_t, 3>{1u, 2u, 2u}));
}

void testFractionalDepthAndMultiFaceClipping()
{
  const std::array<int32_t, 3> key{};
  SparseVoxelMicroDescriptor top = descriptor(
      5u, 2u,
      1u << static_cast<uint32_t>(SparseVoxelMicroFace::PositiveY),
      SparseVoxelMicroGeometryProfile::Constant);
  top.carveDepthFraction = 8u;
  assert(std::abs(sparseVoxelMicroMaximumCarveDepth(top) - 2.5) < 1.0e-12);
  const SparseVoxelMicroRay downward{
    {0.5, 2.0, 0.5}, {0.0, -1.0, 0.0}};
  for (const auto &hit : {
         traceSparseVoxelMicroOneFace(top, key, downward),
         traceSparseVoxelMicroVolume(top, key, downward),
       })
  {
    assert(hit.hit());
    assert(std::abs(hit.distance - 1.5) < 1.0e-12);
  }

  SparseVoxelMicroDescriptor opposing = descriptor(
      5u, 1u,
      (1u << static_cast<uint32_t>(SparseVoxelMicroFace::NegativeX)) |
          (1u << static_cast<uint32_t>(SparseVoxelMicroFace::PositiveX)),
      SparseVoxelMicroGeometryProfile::Constant);
  opposing.carveDepthFraction = 4u; // 1.25 cells from both sides.
  const SparseVoxelMicroRay rightward{
    {-1.0, 0.5, 0.5}, {1.0, 0.0, 0.0}};
  const SparseVoxelMicroRay leftward{
    {2.0, 0.5, 0.5}, {-1.0, 0.0, 0.0}};
  const auto leftHit = traceSparseVoxelMicroVolume(
      opposing, key, rightward);
  const auto rightHit = traceSparseVoxelMicroVolume(
      opposing, key, leftward);
  assert(leftHit.hit() && rightHit.hit());
  assert(std::abs(leftHit.distance - 1.25) < 1.0e-12);
  assert(std::abs(rightHit.distance - 1.25) < 1.0e-12);

  opposing.maximumCarveDepth = 2u;
  opposing.carveDepthFraction = 8u;
  assert(!traceSparseVoxelMicroVolume(
              opposing, key, rightward).hit());
}

void testSlopeContextAndStaircaseProfile()
{
  SparseVoxelMicroDescriptor stairs = descriptor(
      5u, 5u,
      1u << static_cast<uint32_t>(SparseVoxelMicroFace::PositiveY),
      SparseVoxelMicroGeometryProfile::Staircase);
  const std::array<int32_t, 3> key{3, -2, 7};
  const uint32_t positiveX = sparseVoxelMicroSurfaceContext(0u, 4u);
  const uint32_t negativeX = sparseVoxelMicroSurfaceContext(8u, 4u);
  const uint32_t diagonal = sparseVoxelMicroSurfaceContext(14u, 4u);
  assert(sparseVoxelMicroContextIsTerrainSurface(
      static_cast<uint32_t>(SparseVoxelMicroContext::FlatSurface)));
  assert(sparseVoxelMicroContextIsTerrainSurface(positiveX));
  assert(!sparseVoxelMicroContextIsTerrainSurface(
      static_cast<uint32_t>(SparseVoxelMicroContext::Interior)));
  assert(!sparseVoxelMicroContextIsTerrainSurface(
      static_cast<uint32_t>(SparseVoxelMicroContext::WallPositiveX)));
  const auto depth = [&](const uint32_t context,
                         const uint32_t u, const uint32_t v)
  {
    return sparseVoxelMicroCarveDepth(
        stairs, key, SparseVoxelMicroFace::PositiveY, {u, v},
        context);
  };
  assert(depth(
             static_cast<uint32_t>(SparseVoxelMicroContext::Interior),
             4u, 0u) == 0.0);
  assert(depth(
             static_cast<uint32_t>(SparseVoxelMicroContext::FlatSurface),
             4u, 0u) == 0.0);
  for (uint32_t coordinate = 0u; coordinate < stairs.subdivision;
       ++coordinate)
  {
    const double positive = depth(positiveX, coordinate, 2u);
    const double negative = depth(
        negativeX, stairs.subdivision - 1u - coordinate, 2u);
    assert(positive == std::floor(positive));
    assert(negative == std::floor(negative));
    assert(positive >= 0.0 && positive <= stairs.subdivision);
    assert(negative >= 0.0 && negative <= stairs.subdivision);
  }
  for (uint32_t v = 0u; v < stairs.subdivision; ++v)
    for (uint32_t u = 0u; u < stairs.subdivision; ++u)
    {
      const double aliased = depth(diagonal, u, v);
      assert(aliased == std::floor(aliased));
      assert(aliased >= 0.0 && aliased <= stairs.subdivision);
    }
  // The heightmap phase is the plane intercept. It must shift the complete
  // profile without consulting the parent key; random per-parent offsets are
  // precisely what makes the outer voxel boundaries visible.
  const uint32_t lowSurface = sparseVoxelMicroSurfaceContext(0u, 0u);
  const uint32_t highSurface = sparseVoxelMicroSurfaceContext(0u, 7u);
  assert(depth(lowSurface, 2u, 2u) > depth(highSurface, 2u, 2u));
  assert(depth(lowSurface, 4u, 2u) == stairs.subdivision);
  for (int32_t parentX = -16; parentX <= 16; ++parentX)
    for (int32_t parentZ = -16; parentZ <= 16; ++parentZ)
      assert(sparseVoxelMicroCarveDepth(
                 stairs, {parentX, -2, parentZ},
                 SparseVoxelMicroFace::PositiveY, {2u, 2u}, positiveX) ==
          depth(positiveX, 2u, 2u));

  // Non-cardinal sectors vary over both axes, while exact cardinal terrain is
  // allowed to remain straight instead of receiving synthetic noise.
  for (uint32_t angleBin = 0u; angleBin < 16u; ++angleBin)
  {
    const uint32_t angularContext =
        sparseVoxelMicroSurfaceContext(angleBin, 4u);
    bool changesX = false;
    bool changesZ = false;
    for (uint32_t v = 0u; v < stairs.subdivision; ++v)
      for (uint32_t u = 0u; u < stairs.subdivision; ++u)
      {
        const double value = depth(angularContext, u, v);
        if (u + 1u < stairs.subdivision)
          changesX |= value != depth(angularContext, u + 1u, v);
        if (v + 1u < stairs.subdivision)
          changesZ |= value != depth(angularContext, u, v + 1u);
      }
    assert(changesX == (angleBin != 4u && angleBin != 12u));
    assert(changesZ == (angleBin != 0u && angleBin != 8u));
  }

  stairs.faceMask |=
      (1u << static_cast<uint32_t>(SparseVoxelMicroFace::NegativeX)) |
      (1u << static_cast<uint32_t>(SparseVoxelMicroFace::PositiveX)) |
      (1u << static_cast<uint32_t>(SparseVoxelMicroFace::NegativeZ)) |
      (1u << static_cast<uint32_t>(SparseVoxelMicroFace::PositiveZ));
  for (uint32_t coordinate = 0u; coordinate < stairs.subdivision;
       ++coordinate)
    assert(sparseVoxelMicroCarveDepth(
               stairs, key, SparseVoxelMicroFace::PositiveX,
               {2u, coordinate},
               static_cast<uint32_t>(
                   SparseVoxelMicroContext::WallPositiveX)) ==
        static_cast<double>(coordinate));
  assert(sparseVoxelMicroCarveDepth(
             stairs, key, SparseVoxelMicroFace::NegativeX, {2u, 4u},
             static_cast<uint32_t>(
                 SparseVoxelMicroContext::WallPositiveX)) == 0.0);
  assert(sparseVoxelMicroCarveDepth(
             stairs, key, SparseVoxelMicroFace::PositiveX, {2u, 4u},
             static_cast<uint32_t>(diagonal)) == 0.0);
  assert(sparseVoxelMicroCarveDepth(
             stairs, key, SparseVoxelMicroFace::NegativeZ, {2u, 4u},
             static_cast<uint32_t>(diagonal)) == 0.0);

  SparseVoxelMicroDescriptor differentlyAuthored = stairs;
  differentlyAuthored.maximumCarveDepth = 4u;
  differentlyAuthored.carveDepthFraction = 11u;
  differentlyAuthored.faceMask = 0u;
  differentlyAuthored.materialSeed ^= 0xffu;
  assert(sparseVoxelMicroCarveDepth(
             differentlyAuthored, key,
             SparseVoxelMicroFace::PositiveY, {2u, 2u},
             positiveX) ==
      sparseVoxelMicroCarveDepth(
          stairs, key, SparseVoxelMicroFace::PositiveY, {2u, 2u},
          positiveX));
  assert(sparseVoxelMicroCarveDepth(
             differentlyAuthored, key,
             SparseVoxelMicroFace::PositiveX, {2u, 4u},
             static_cast<uint32_t>(
                 SparseVoxelMicroContext::WallPositiveX)) == 4.0);
  SparseVoxelMicroMaterialGPU packedStairs{};
  assert(packSparseVoxelMicroMaterial(
      differentlyAuthored, {}, packedStairs));
  const SparseVoxelMicroDescriptor canonicalStairs =
      unpackSparseVoxelMicroGeometry(packedStairs);
  assert(canonicalStairs.maximumCarveDepth == 5u);
  assert(canonicalStairs.carveDepthFraction == 0u);
  assert(canonicalStairs.faceMask == kSparseVoxelStaircaseFaceMask);

  const SparseVoxelMicroRay downward{
    {0.7, 2.0, 0.5}, {0.0, -1.0, 0.0}};
  const uint32_t context = positiveX;
  const auto volume = traceSparseVoxelMicroVolume(
      stairs, key, downward, context);
  const auto brute = bruteForceTrace(stairs, key, downward, context);
  assertEquivalent(brute, volume);
  assert(volume.hit());
  assert(sparseVoxelMicroExpectedCoverage(
             stairs, static_cast<uint32_t>(
                         SparseVoxelMicroContext::Interior)) == 1.0);

  SparseVoxelMaterialGPU source{};
  source.flags = diagonal;
  const SparseVoxelBrickMaterialGPU packed =
      packSparseVoxelBrickMaterial(source);
  assert((packed.packed[1] & kSparseVoxelBrickTransientMask) == 0u);

  std::vector<SparseVoxelCenterLengthGPU> leaves(3u);
  leaves[0].center = {0.5f, 0.5f, 0.5f};
  leaves[1].center = {0.5f, 1.5f, 0.5f};
  leaves[2].center = {8.5f, 0.5f, 0.5f};
  for (auto &leaf : leaves)
    leaf.length = 1.0f;
  std::vector<SparseVoxelMaterialGPU> materials(3u);
  materials[1].flags = diagonal;
  SparseVoxelBrickHierarchy hierarchy{};
  SparseVoxelBrickBuildSettings settings{};
  settings.voxelSize = 1.0f;
  std::string error;
  assert(buildSparseVoxelBrickHierarchy(
      leaves, materials, {0u, 1u, 2u}, settings, hierarchy, &error));
  bool coarseContextRetained = false;
  for (const auto &node : hierarchy.nodes)
  {
    if (node.level == 0u)
      continue;
    for (uint32_t cell = 0u; cell < node.occupiedCount; ++cell)
    {
      const size_t materialSlot =
          static_cast<size_t>(node.materialBase) + cell;
      coarseContextRetained |=
          unpackSparseVoxelMicroContextValue(
              hierarchy.microSurfaceData[materialSlot]) == diagonal;
    }
  }
  assert(coarseContextRetained);
}

void testContinuousSurfaceCoveragePatch()
{
  const uint32_t subdivision = 5u;
  const uint64_t contextPatch = packSparseVoxelMicroSurfacePatch(
      {0.0, 0.25, 0.75, 1.0});
  const uint64_t packedContext = packSparseVoxelMicroContextData(
      sparseVoxelMicroSurfaceContext(9u, 3u), contextPatch);
  assert(unpackSparseVoxelMicroContextValue(packedContext) ==
      sparseVoxelMicroSurfaceContext(9u, 3u));
  assert(unpackSparseVoxelMicroContextSurfacePatch(packedContext) ==
      contextPatch);
  // The old UNorm6 sidecar flattened changes below 1/63 of a voxel. Real
  // kilometre-scale height maps routinely produce smaller adjacent deltas.
  const std::array<double, 4> subtleSource{
      0.5, 0.5002, 0.5, 0.5002};
  const auto subtleDecoded = unpackSparseVoxelMicroSurfacePatch(
      packSparseVoxelMicroSurfacePatch(subtleSource));
  assert(subtleDecoded[1] > subtleDecoded[0]);
  assert(std::abs(
             (subtleDecoded[1] - subtleDecoded[0]) -
             (subtleSource[1] - subtleSource[0])) <=
      1.0 / static_cast<double>(kSparseVoxelMicroSurfaceCornerMask));
  ResolvedVoxelSurfaceGPU resolved{};
  resolved.hitFlags = kSparseVoxelSurfacePackedMaterial;
  resolved.reserved2 = packSparseVoxelResolvedBottomAndContext(
      0x00abcdefu, sparseVoxelMicroSurfaceContext(9u, 3u));
  assert((resolved.reserved2 & kSparseVoxelResolvedBottomAlbedoMask) ==
      0x00abcdefu);
  assert(sparseVoxelResolvedContext(resolved) ==
      sparseVoxelMicroSurfaceContext(9u, 3u));
  resolved.hitFlags = 0u;
  assert(sparseVoxelResolvedContext(resolved) == 0u);
  for (uint32_t angleStep = 0u; angleStep < 64u; ++angleStep)
  {
    const double angle = static_cast<double>(angleStep) *
        6.2831853071795864769 / 64.0;
    const double normalX = std::cos(angle);
    const double normalZ = std::sin(angle);
    std::array<double, 4> sourceCorners{};
    for (uint32_t z = 0u; z < 2u; ++z)
      for (uint32_t x = 0u; x < 2u; ++x)
      {
        const double localX = static_cast<double>(x) - 0.5;
        const double localZ = static_cast<double>(z) - 0.5;
        sourceCorners[z * 2u + x] = std::clamp(
            0.5 + 0.5 * (normalX * localX + normalZ * localZ),
            0.0, 1.0);
      }
    const uint64_t packed =
        packSparseVoxelMicroSurfacePatch(sourceCorners);
    const std::array<double, 4> decoded =
        unpackSparseVoxelMicroSurfacePatch(packed);
    for (uint32_t corner = 0u; corner < decoded.size(); ++corner)
      assert(std::abs(decoded[corner] - sourceCorners[corner]) <=
          0.5 / static_cast<double>(kSparseVoxelMicroSurfaceCornerMask) +
          1.0e-12);
    assert(std::abs(
               sparseVoxelMicroSurfaceExpectedCoverage(packed) - 0.5) <=
        1.0 / static_cast<double>(kSparseVoxelMicroSurfaceCornerMask));

    bool changesX = false;
    bool changesZ = false;
    for (uint32_t z = 0u; z < subdivision; ++z)
      for (uint32_t x = 0u; x < subdivision; ++x)
      {
        const uint32_t depth = sparseVoxelMicroSurfaceColumnDepth(
            packed, subdivision, x, z);
        assert(depth <= subdivision);
        if (x + 1u < subdivision)
        {
          const uint32_t next = sparseVoxelMicroSurfaceColumnDepth(
              packed, subdivision, x + 1u, z);
          assert(std::max(depth, next) - std::min(depth, next) <= 1u);
          changesX |= depth != next;
          if (normalX > 1.0e-12)
            assert(depth >= next);
          else if (normalX < -1.0e-12)
            assert(depth <= next);
        }
        if (z + 1u < subdivision)
        {
          const uint32_t next = sparseVoxelMicroSurfaceColumnDepth(
              packed, subdivision, x, z + 1u);
          assert(std::max(depth, next) - std::min(depth, next) <= 1u);
          changesZ |= depth != next;
          if (normalZ > 1.0e-12)
            assert(depth >= next);
          else if (normalZ < -1.0e-12)
            assert(depth <= next);
        }
      }
    // A whole-cell digital plane cannot express a small minor-axis movement
    // inside every individual 5x5 parent. It must, however, advance along the
    // dominant axis; the shared phase carries minor-axis steps into adjacent
    // parents instead of introducing fractional-height cells.
    if (std::abs(normalX) >= std::abs(normalZ))
      assert(changesX);
    if (std::abs(normalZ) >= std::abs(normalX))
      assert(changesZ);
  }
  const uint64_t uncarvedPatch = packSparseVoxelMicroSurfacePatch(
      {1.0, 1.0, 1.0, 1.0});
  const uint64_t fullyCarvedPatch = packSparseVoxelMicroSurfacePatch(
      {0.0, 0.0, 0.0, 0.0});
  for (uint32_t z = 0u; z < subdivision; ++z)
    for (uint32_t x = 0u; x < subdivision; ++x)
    {
      assert(sparseVoxelMicroSurfaceColumnDepth(
                 uncarvedPatch, subdivision, x, z) == 0u);
      assert(sparseVoxelMicroSurfaceColumnDepth(
                 fullyCarvedPatch, subdivision, x, z) == subdivision);
    }
  assert(packSparseVoxelMicroSurfacePatch(
             {std::numeric_limits<double>::quiet_NaN(), -1.0, 2.0, 0.5}) ==
      ((static_cast<uint64_t>(kSparseVoxelMicroSurfaceCornerMask) << 28u) |
       (uint64_t{8192u} << 42u)));
  assert(sparseVoxelMicroSurfaceColumnDepth(0u, 0u, 0u, 0u) == 0u);
}

uint32_t shaderReferenceSurfaceColumnDepth(
    const uint64_t packedPatch, const uint32_t subdivision,
    const uint32_t x, const uint32_t z)
{
  if (subdivision == 0u || x >= subdivision || z >= subdivision)
    return 0u;
  const auto corner = [packedPatch](const uint32_t index)
  {
    return static_cast<float>(
        (packedPatch >> (index * 14u)) & 0x3fffu) / 16383.0f;
  };
  const float u = (static_cast<float>(x) + 0.5f) /
      static_cast<float>(subdivision);
  const float v = (static_cast<float>(z) + 0.5f) /
      static_cast<float>(subdivision);
  const float negativeZ = corner(0u) + (corner(1u) - corner(0u)) * u;
  const float positiveZ = corner(2u) + (corner(3u) - corner(2u)) * u;
  const float retained = std::clamp(
      negativeZ + (positiveZ - negativeZ) * v, 0.0f, 1.0f);
  const float depth =
      (1.0f - retained) * static_cast<float>(subdivision);
  if (depth >= static_cast<float>(subdivision) - 0.5f)
    return subdivision;
  return static_cast<uint32_t>(std::clamp(
      std::floor(depth + 1.0e-5f), 0.0f,
      static_cast<float>(subdivision)));
}

void testSurfaceCoverageCPUShaderAgreementAllSubdivisions()
{
  // Independent f32 mirror of sparseVoxelExactMicroCarveDepth. Every packed
  // corner combination sampled here is sent to the same WGSL function by the
  // brick exact path; this locks its quantization contract for N=2..8.
  uint32_t state = 0x7a1c93e5u;
  for (uint32_t subdivision = 2u;
       subdivision <= kSparseVoxelMicroMaximumSubdivision; ++subdivision)
  {
    for (uint32_t fixture = 0u; fixture < 4096u; ++fixture)
    {
      const uint64_t packed =
          ((static_cast<uint64_t>(nextRandom(state)) << 32u) |
           nextRandom(state)) & kSparseVoxelMicroSurfacePatchMask;
      for (uint32_t z = 0u; z < subdivision; ++z)
        for (uint32_t x = 0u; x < subdivision; ++x)
          assert(sparseVoxelMicroSurfaceColumnDepth(
                     packed, subdivision, x, z) ==
                 shaderReferenceSurfaceColumnDepth(
                     packed, subdivision, x, z));
    }
  }
}

uint64_t microGeometrySignature(
    const SparseVoxelMicroGeometryProfile profile,
    const uint32_t context = 0u)
{
  const uint32_t faceMask = profile == SparseVoxelMicroGeometryProfile::Staircase
      ? kSparseVoxelStaircaseFaceMask : 0x3fu;
  const auto material = descriptor(5u, 3u, faceMask, profile, 0x5du);
  uint64_t hash = 1469598103934665603ull;
  const std::array<std::array<int32_t, 3>, 4> parents{
    std::array<int32_t, 3>{0, 0, 0},
    std::array<int32_t, 3>{-17, 3, 91},
    std::array<int32_t, 3>{2048, -8, -4097},
    std::array<int32_t, 3>{11, 13, 17},
  };
  for (const auto &parent : parents)
    for (uint32_t z = 0u; z < material.subdivision; ++z)
      for (uint32_t y = 0u; y < material.subdivision; ++y)
        for (uint32_t x = 0u; x < material.subdivision; ++x)
        {
          const uint64_t value = sparseVoxelMicroCellOccupied(
              material, parent, {x, y, z}, context) ? 1u : 0u;
          hash ^= value;
          hash *= 1099511628211ull;
        }
  return hash;
}

void testPhaseZeroDeterministicGeometrySignatures()
{
  const uint64_t slopePatch = packSparseVoxelMicroSurfacePatch(
      {0.0, 0.25, 0.75, 1.0});
  const uint32_t slopeContext = packSparseVoxelMicroContextDataLegacy(
      packSparseVoxelMicroContextData(
          sparseVoxelMicroTerrainSlopeContext(0.5), slopePatch));
  const std::array<uint64_t, 4> signatures{
    microGeometrySignature(SparseVoxelMicroGeometryProfile::HashColumns),
    microGeometrySignature(SparseVoxelMicroGeometryProfile::ClumpedColumns),
    microGeometrySignature(SparseVoxelMicroGeometryProfile::VolumeDensity),
    microGeometrySignature(
        SparseVoxelMicroGeometryProfile::Staircase, slopeContext),
  };
  // Values are populated from the deterministic reference evaluator and are
  // intentionally architecture-independent (only boolean occupancy is mixed).
  const std::array<uint64_t, 4> expected{
    0x1a6f766ebda33013ull,
    0x9f29a5f35bc32f9eull,
    0x781086ea7135f066ull,
    0xc105f266c7064607ull,
  };
  if (signatures != expected)
  {
    std::cerr << "phase0 geometry signatures:";
    for (const uint64_t signature : signatures)
      std::cerr << ' ' << std::hex << signature;
    std::cerr << std::dec << '\n';
  }
  assert(signatures == expected);
}

void testParentContinuation()
{
  SparseVoxelMicroParentGrid grid{};
  grid.dimensions = {3u, 1u, 1u};
  grid.occupied = {1u, 1u, 0u};
  grid.descriptors.resize(3u);
  grid.descriptors[0] = descriptor(5u, 5u, 1u << static_cast<uint32_t>(SparseVoxelMicroFace::NegativeX), SparseVoxelMicroGeometryProfile::Constant);
  const SparseVoxelMicroRay ray{{-1.0, 0.5, 0.5}, {1.0, 0.0, 0.0}};
  const SparseVoxelMicroParentTraceResult continued = traceSparseVoxelMicroParents(grid, ray);
  assert(continued.hit());
  assert(!continued.microHit);
  assert(continued.parentCell == (std::array<uint32_t, 3>{1u, 0u, 0u}));
  assert(std::abs(continued.distance - 2.0) < 1.0e-12);
  assert(continued.parentContinuations == 1u);

  grid.occupied = {1u, 0u, 0u};
  const SparseVoxelMicroParentTraceResult miss = traceSparseVoxelMicroParents(grid, ray);
  assert(!miss.hit());
  assert(miss.status == SparseVoxelMicroTraceStatus::Miss);
  assert(miss.parentContinuations == 1u);

  grid.descriptors[0] = descriptor(4u, 2u, 1u << static_cast<uint32_t>(SparseVoxelMicroFace::NegativeX), SparseVoxelMicroGeometryProfile::Constant);
  const SparseVoxelMicroParentTraceResult recessed = traceSparseVoxelMicroParents(grid, ray);
  assert(recessed.hit());
  assert(recessed.microHit);
  assert(std::abs(recessed.distance - 1.5) < 1.0e-12);
  assert(recessed.microCell == (std::array<uint32_t, 3>{2u, 2u, 2u}));
}

void testBrickExactMicroDistanceShaderControl()
{
  assert(!sparseVoxelBrickExactMicroDistanceEnabled(0.0f));
  assert(!sparseVoxelBrickExactMicroDistanceEnabled(-1.0f));
  assert(!sparseVoxelBrickExactMicroDistanceEnabled(
      std::numeric_limits<float>::infinity()));
  assert(!sparseVoxelBrickExactMicroDistanceEnabled(
      std::numeric_limits<float>::quiet_NaN()));
  assert(sparseVoxelBrickExactMicroDistanceEnabled(1250.5f));

  assert(sparseVoxelBrickExactMicroDistanceBits(0.0f) == 0u);
  assert(sparseVoxelBrickExactMicroDistanceBits(-1.0f) == 0u);
  uint32_t expectedBits = 0u;
  const float cutoff = 1250.5f;
  std::memcpy(&expectedBits, &cutoff, sizeof(expectedBits));
  assert(sparseVoxelBrickExactMicroDistanceBits(cutoff) == expectedBits);
}

} // namespace

int main()
{
  testDescriptorValidationAndFaceOrientation();
  testPackedMaterialABIAndDeduplication();
  testMicroPayloadPacking();
  testQuantitativeTerrainSlopeContext();
  testCanonicalParentKeys();
  testGeometricLodHandoff();
  testBrickPalettePreservesDescriptorIdentity();
  testCarveDepthAndSubtractiveOccupancy();
  testSingleFaceTraversalAgainstBruteForce();
  testSolidStaircaseInteriorShortcutEquivalence();
  testMultiFaceVolumeTraversalAgainstBruteForce();
  testVolumeDensityUsesCanonicalWorldCells();
  testDisabledProfileMatchesSolidParent();
  testExactRecessDepthAndFace();
  testFractionalDepthAndMultiFaceClipping();
  testSlopeContextAndStaircaseProfile();
  testContinuousSurfaceCoveragePatch();
  testSurfaceCoverageCPUShaderAgreementAllSubdivisions();
  testPhaseZeroDeterministicGeometrySignatures();
  testParentContinuation();
  testBrickExactMicroDistanceShaderControl();
  std::cout << "Sparse voxel microgeometry tests passed\n";
  return 0;
}
