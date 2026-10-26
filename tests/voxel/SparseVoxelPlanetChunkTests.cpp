#include "procedural/PlanetPackage.hpp"
#include "procedural/PolygonTerrain.hpp"
#include "voxel/material/VoxelTerrainMaterialPresets.hpp"
#include "voxel/svraster/PlanetVoxelChunk.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(const bool condition, const char *message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

void appendU16(std::vector<uint8_t> &bytes, const uint16_t value)
{
  bytes.push_back(static_cast<uint8_t>(value & 0xffu));
  bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
}

} // namespace

int main()
{
  std::cout << "planet package to sparse voxel bricks\n";
  check(
      voxel::svraster::recommendedPlanetVoxelWorkerCount(0u) == 1u &&
          voxel::svraster::recommendedPlanetVoxelWorkerCount(1u) == 1u &&
          voxel::svraster::recommendedPlanetVoxelWorkerCount(2u) == 2u &&
          voxel::svraster::recommendedPlanetVoxelWorkerCount(4u) == 2u &&
          voxel::svraster::recommendedPlanetVoxelWorkerCount(10u) == 8u &&
          voxel::svraster::recommendedPlanetVoxelWorkerCount(128u) == 8u,
      "automatic terrain workers reserve render threads and remain bounded");
  {
    voxel::svraster::PlanetVoxelChunkSet chunks{};
    chunks.elevationOriginMetres = 90.0;
    chunks.surfaceHeightSide = 6u;
    chunks.surfaceHeightHalo = 1u;
    voxel::svraster::PlanetVoxelChunkRecord left{};
    left.worldCenterXMetres = 0.0;
    left.worldCenterZMetres = 0.0;
    left.edgeMetres = 4.0;
    left.voxelSizeMetres = 1.0;
    left.elevationOriginMetres = 100.0;
    voxel::svraster::PlanetVoxelChunkRecord right = left;
    right.worldCenterXMetres = 4.0;
    chunks.chunks = {left, right};
    chunks.surfaceHeights.insert(
        chunks.surfaceHeights.end(), 36u, 100.2f);
    chunks.surfaceHeights.insert(
        chunks.surfaceHeights.end(), 36u, 105.2f);
    double leftTop = 0.0;
    double boundaryTop = 0.0;
    double missingTop = 1.0;
    check(
        voxel::svraster::samplePlanetVoxelSurfaceTop(
            chunks, -1.0, 0.0, leftTop) &&
            std::abs(leftTop - 11.0) < 1.0e-9 &&
        voxel::svraster::samplePlanetVoxelSurfaceTop(
            chunks, 2.0, 0.0, boundaryTop) &&
            std::abs(boundaryTop - 16.0) < 1.0e-9 &&
        !voxel::svraster::samplePlanetVoxelSurfaceTop(
            chunks, 20.0, 0.0, missingTop) &&
            missingTop == 0.0,
        "spawn height samples the rendered voxel column and chooses the "
        "higher owner at page boundaries");
  }
  {
    const voxel::svraster::TransientSparseVoxelPageStoreSettings defaults{};
    check(
        defaults.maximumMemoryBytes == 4ull * 1024u * 1024u * 1024u &&
            defaults.maximumDiskBytes == 4ull * 1024u * 1024u * 1024u,
        "transient terrain cache defaults provide multi-gigabyte headroom");
  }
  {
    voxel::svraster::TransientSparseVoxelPageStore memoryStore;
    voxel::svraster::TransientSparseVoxelPageStoreSettings memorySettings{};
    memorySettings.maximumMemoryBytes = 1024u * 1024u;
    memorySettings.maximumDiskBytes = 0u;
    std::string memoryError;
    const std::vector<uint8_t> input{1u, 3u, 3u, 7u, 9u, 42u};
    std::vector<uint8_t> output;
    check(
        memoryStore.initialize(memorySettings, &memoryError) &&
            memoryStore.put({11u, 29u}, input, &memoryError) &&
            memoryStore.get({11u, 29u}, output, &memoryError) ==
                voxel::svraster::TransientSparseVoxelPageSource::Memory &&
            output == input,
        "an LZ4 procedural page round-trips from the memory tier");
  }
  procedural::PlanetPackageMetadata metadata{};
  metadata.planetRadiusMetres = 6371000.0;
  metadata.heightScaleMetres = 1.0;
  metadata.nominalRenderPatchEdgeMetres = 1000.0;
  metadata.tileSizePixels = 4u;
  metadata.gutterPixels = 1u;
  metadata.maximumTileLevel = 0u;
  metadata.contentHash = 0x8f3d4721a6c95be0ull;
  {
    using Material = procedural::PolygonTerrainMaterial;
    const std::array<procedural::SpherePoint, 6u> directions{{
      {1.0, 0.0, 0.0}, {-1.0, 0.0, 0.0},
      {0.0, 1.0, 0.0}, {0.0, -1.0, 0.0},
      {0.0, 0.0, 1.0}, {0.0, 0.0, -1.0}}};
    std::array<uint32_t, directions.size()> colours{};
    for (size_t index = 0u; index < directions.size(); ++index)
      colours[index] = voxel::svraster::planetVoxelSurfaceMaterial(
          static_cast<uint8_t>(Material::Grass), directions[index], metadata)
                           .albedoRGBA8;
    auto uniqueColours = colours;
    std::sort(uniqueColours.begin(), uniqueColours.end());
    const auto uniqueEnd = std::unique(
        uniqueColours.begin(), uniqueColours.end());
    check(
        std::distance(uniqueColours.begin(), uniqueEnd) >= 3 &&
            voxel::svraster::planetVoxelSurfaceMaterial(
                static_cast<uint8_t>(Material::Grass), directions[0], metadata)
                    .albedoRGBA8 == colours[0],
        "planet-space terrain colours are varied and deterministic");

    const std::array<Material, 8u> representativeBiomes{{
      Material::Grassland, Material::Taiga, Material::TropicalRainForest,
      Material::Tundra, Material::SubtropicalDesert, Material::Rock,
      Material::Snow, Material::Beach}};
    std::array<uint32_t, representativeBiomes.size()> biomeColours{};
    for (size_t index = 0u; index < representativeBiomes.size(); ++index)
      biomeColours[index] = voxel::svraster::planetVoxelSurfaceMaterial(
          static_cast<uint8_t>(representativeBiomes[index]), directions[0],
          metadata, 0.75, 0.5)
                                .albedoRGBA8;
    auto uniqueBiomes = biomeColours;
    std::sort(uniqueBiomes.begin(), uniqueBiomes.end());
    const auto uniqueBiomeEnd = std::unique(
        uniqueBiomes.begin(), uniqueBiomes.end());
    const auto snow = voxel::svraster::planetVoxelSurfaceMaterial(
        static_cast<uint8_t>(Material::Snow), directions[0], metadata,
        0.75, 0.5);
    const auto beach = voxel::svraster::planetVoxelSurfaceMaterial(
        static_cast<uint8_t>(Material::Beach), directions[0], metadata,
        0.15, 0.0);
    check(
        uniqueBiomeEnd == uniqueBiomes.end() &&
            snow.reserved0 != 0u && beach.reserved0 != 0u &&
            (snow.reserved1 &
                 voxel::svraster::kSparseVoxelMaterialBlendTopBottomFaces) !=
                0u &&
            (beach.reserved1 &
                 voxel::svraster::kSparseVoxelMaterialBlendTopBottomFaces) !=
                0u,
        "terrain types retain distinct brick palettes and exposed secondary layers");
  }
  const procedural::PlanetTileAddress root{0u, 0u, 0u, 0u};
  const uint32_t storedSide = metadata.tileSizePixels + metadata.gutterPixels * 2u;
  std::vector<uint8_t> heights;
  std::vector<uint8_t> materials(static_cast<size_t>(storedSide) * storedSide, 3u);
  for (uint32_t y = 0u; y < storedSide; ++y)
    for (uint32_t x = 0u; x < storedSide; ++x)
      appendU16(heights, static_cast<uint16_t>(100u + x * 20u + y * 10u));

  const uint32_t heightChannel = static_cast<uint32_t>(procedural::PlanetRasterChannel::Height);
  const uint32_t materialChannel = static_cast<uint32_t>(procedural::PlanetRasterChannel::Material);
  const std::vector<procedural::PlanetPackageChannel> channels{
    {procedural::PlanetPackageDomain::Raster, heightChannel, procedural::PlanetPackageFormat::R16UInt, procedural::PlanetPackageMipPolicy::Average, 0u, 1.0, 0.0, "height"},
    {procedural::PlanetPackageDomain::Raster, materialChannel, procedural::PlanetPackageFormat::R8UInt, procedural::PlanetPackageMipPolicy::Mode, 0u, 1.0, 0.0, "material"},
  };
  std::vector<procedural::PlanetPackageBlockInput> blocks;
  for (uint8_t face = 0u; face < procedural::kPlanetPackageCubeFaceCount;
       ++face)
  {
    const procedural::PlanetTileAddress faceRoot{face, 0u, 0u, 0u};
    blocks.push_back(
        {procedural::planetRasterBlockKey(heightChannel, faceRoot), heights});
    blocks.push_back(
        {procedural::planetRasterBlockKey(materialChannel, faceRoot), materials});
  }
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "planet-voxel-chunk-test.planetpkg";
  std::string error;
  check(procedural::writePlanetPackage(path, metadata, channels, blocks, {}, nullptr, &error), "test planet package is written");
  procedural::PlanetPackageReader reader;
  check(reader.open(path, &error), "test planet package opens without decoding its maps");

  voxel::svraster::PlanetVoxelMaterialFieldRegion fieldA{};
  voxel::svraster::PlanetVoxelMaterialFieldRegion fieldARepeat{};
  voxel::svraster::PlanetVoxelMaterialFieldRegion fieldB{};
  voxel::svraster::PlanetVoxelMaterialFieldRegion fieldParent{};
  voxel::svraster::PlanetVoxelMaterialFieldRegion fieldChild{};
  const procedural::PlanetTileAddress fieldTileA{0u, 2u, 1u, 1u};
  const procedural::PlanetTileAddress fieldTileB{0u, 2u, 2u, 1u};
  const procedural::PlanetTileAddress fieldParentTile{0u, 1u, 0u, 0u};
  const procedural::PlanetTileAddress fieldChildTile{0u, 2u, 1u, 0u};
  const bool generatedFields =
      voxel::svraster::generatePlanetVoxelMaterialFieldRegion(
          metadata, fieldTileA, {0, 0}, {64u, 64u}, fieldA, &error) &&
      voxel::svraster::generatePlanetVoxelMaterialFieldRegion(
          metadata, fieldTileA, {0, 0}, {64u, 64u}, fieldARepeat,
          &error) &&
      voxel::svraster::generatePlanetVoxelMaterialFieldRegion(
          metadata, fieldTileB, {64, 0}, {64u, 64u}, fieldB, &error);
  const bool generatedLodFields =
      voxel::svraster::generatePlanetVoxelMaterialFieldRegion(
          metadata, fieldParentTile, {0, 0}, {128u, 128u}, fieldParent,
          &error) &&
      voxel::svraster::generatePlanetVoxelMaterialFieldRegion(
          metadata, fieldChildTile, {64, 0}, {64u, 64u}, fieldChild,
          &error);
  bool exactFieldSeam = generatedFields;
  bool exactFieldLodBoundary = generatedLodFields;
  bool complementaryGrassAndDirt = generatedFields;
  constexpr uint32_t fieldSide =
      voxel::material::kVoxelMaterialFieldAtlasTileSide;
  if (generatedFields)
  {
    exactFieldSeam &= fieldA.values == fieldARepeat.values;
    for (uint32_t row = 0u; row < fieldSide; ++row)
      for (size_t channel = 0u;
           channel < static_cast<size_t>(
               voxel::svraster::PlanetVoxelGeneratedMaterialField::Count);
           ++channel)
        exactFieldSeam &=
            fieldA.values[channel][row * fieldSide + fieldSide - 1u] ==
            fieldB.values[channel][row * fieldSide];
    const auto &grass = fieldA.values[static_cast<size_t>(
        voxel::svraster::PlanetVoxelGeneratedMaterialField::Grass)];
    const auto &dirt = fieldA.values[static_cast<size_t>(
        voxel::svraster::PlanetVoxelGeneratedMaterialField::Dirt)];
    for (size_t texel = 0u; texel < grass.size(); ++texel)
      complementaryGrassAndDirt &=
          static_cast<uint32_t>(grass[texel]) + dirt[texel] == 255u;
  }
  if (generatedLodFields)
    for (size_t channel = 0u;
         channel < static_cast<size_t>(
             voxel::svraster::PlanetVoxelGeneratedMaterialField::Count);
         ++channel)
      // Child (level 2, x=1) ends at its level-1 parent's right edge.
      // Both tiles therefore sample the same planet-space corner here.
      exactFieldLodBoundary &=
          fieldParent.values[channel][fieldSide - 1u] ==
          fieldChild.values[channel][fieldSide - 1u];
  check(
      generatedFields && exactFieldSeam,
      "planet material fields regenerate deterministically with exact shared borders");
  check(
      exactFieldLodBoundary,
      "planet material fields agree at coincident parent-child LOD samples");
  check(
      complementaryGrassAndDirt,
      "generated grass and dirt amounts are complementary bounded masks");

  voxel::svraster::PlanetVoxelChunkSettings settings{};
  settings.horizontalVoxelCount = 8u;
  settings.foundationDepthVoxels = 2u;
  settings.maximumVerticalVoxels = 16u;
  settings.proceduralDetailStrength = 0.0;
  // The cache movement checks below exercise the retained toroidal window.
  // Progressive residency has its own GPU-feedback tests further down.
  settings.strategy =
      voxel::svraster::PlanetVoxelChunkStrategy::ClipmapRings;
  voxel::svraster::PlanetVoxelChunk chunk{};
  const procedural::PlanetTileAddress requested{0u, 2u, 3u, 1u};
  std::vector<double> progressValues;
  check(
      voxel::svraster::buildPlanetVoxelChunk(
          reader, requested, settings, chunk, &error,
          [&progressValues](const char *, const double progress)
          {
            progressValues.push_back(progress);
          }),
      "a finer 1 km render patch builds directly into bricks");
  check(chunk.requestedTile == requested && chunk.sourceTile == root, "chunk records requested and ancestor source addresses");
  check(chunk.edgeMetres == 1000.0 && chunk.voxelSizeMetres == 125.0, "chunk dimensions follow the 1 km package contract");
  check(chunk.leafVoxelCount >= 8u * 8u * 3u, "terrain columns include a bounded solid foundation");
  check(!chunk.bricks.empty() && chunk.bricks.occupancy.size() == chunk.bricks.nodes.size(), "voxel columns produce the existing 8-cubed brick hierarchy");
  check(!chunk.bricks.materialIndices.empty() && !chunk.bricks.palette.empty(), "planet materials survive brick palette construction");
  std::array<bool, 256u> usedTerrainPaletteEntries{};
  for (const uint8_t index : chunk.bricks.materialIndices)
    usedTerrainPaletteEntries[index] = true;
  check(
      std::count(
          usedTerrainPaletteEntries.begin(),
          usedTerrainPaletteEntries.end(), true) >= 3,
      "surface terrain uses multiple deterministic biome and soil tones");
  bool foundGreenDirtLayers = false;
  for (uint32_t entry = 0u;
       entry < chunk.bricks.paletteStride &&
       entry < chunk.bricks.palette.size(); ++entry)
  {
    const auto &top = chunk.bricks.palette[entry];
    if ((top.packed[1] &
         voxel::svraster::kSparseVoxelBrickSecondaryEnabled) == 0u)
      continue;
    const uint32_t bottomIndex =
        (top.packed[1] >>
         voxel::svraster::kSparseVoxelBrickSecondaryPaletteShift) &
        voxel::svraster::kSparseVoxelBrickSecondaryPaletteMask;
    if (bottomIndex >= chunk.bricks.palette.size())
      continue;
    const uint32_t topRGB = top.packed[0] & 0x00ffffffu;
    const uint32_t bottomRGB =
        chunk.bricks.palette[bottomIndex].packed[0] & 0x00ffffffu;
    const uint32_t modes = top.packed[1] &
        (voxel::svraster::kSparseVoxelBrickBlendTopBottomFaces |
         voxel::svraster::kSparseVoxelBrickBlendSideFaces);
    foundGreenDirtLayers = topRGB != bottomRGB &&
        modes == voxel::svraster::kSparseVoxelBrickBlendTopBottomFaces;
    if (foundGreenDirtLayers)
      break;
  }
  check(
      foundGreenDirtLayers,
      "vegetation uses green/dirt layers on top and bottom faces with dirt-only sides");
  const uint32_t hiddenRockAlbedo = 104u | (99u << 8u) | (91u << 16u);
  bool coarseTerrainRetainsSurfaceMaterial = true;
  for (const auto &node : chunk.bricks.nodes)
  {
    if (node.level == 0u)
      continue;
    for (uint32_t cell = 0u; cell < node.occupiedCount; ++cell)
    {
      const size_t material = static_cast<size_t>(node.materialBase) + cell;
      if (material >= chunk.bricks.materialIndices.size())
      {
        coarseTerrainRetainsSurfaceMaterial = false;
        break;
      }
      const size_t palette = static_cast<size_t>(node.paletteBase) +
          chunk.bricks.materialIndices[material];
      if (palette >= chunk.bricks.palette.size() ||
          (chunk.bricks.palette[palette].packed[0] & 0x00ffffffu) ==
              hiddenRockAlbedo)
      {
        coarseTerrainRetainsSurfaceMaterial = false;
        break;
      }
    }
  }
  check(
      coarseTerrainRetainsSurfaceMaterial,
      "coarse terrain LODs retain surface colours instead of hidden gray fill");
  check(
      !progressValues.empty() && progressValues.back() == 1.0 &&
          std::is_sorted(progressValues.begin(), progressValues.end()),
      "package-to-voxel construction reports monotonic progress");
  {
    voxel::svraster::PlanetVoxelChunk persisted = chunk;
    persisted.materialReferences = {
        {7u, 0x13u, 0x6c8e9cf570932bd1ull, "terrain/grass-soil"}};
    persisted.bakedContextFields = 0x13u;
    persisted.auxiliaryContextPool = {0u, 0x80402010u};
    persisted.auxiliaryContextIndices.assign(
        persisted.bricks.materialIndices.size(), 1u);
    const std::vector<uint8_t> payload =
        voxel::svraster::serializePlanetVoxelTransientPage(persisted);
    voxel::svraster::PlanetVoxelChunk restored{};
    std::string persistenceError;
    const bool roundTrip =
        voxel::svraster::deserializePlanetVoxelTransientPage(
            payload, restored, &persistenceError);
    check(
        roundTrip &&
            restored.bricks.microSurfaceData ==
                persisted.bricks.microSurfaceData &&
            restored.bricks.paletteDescriptorIds ==
                persisted.bricks.paletteDescriptorIds &&
            restored.materialReferences == persisted.materialReferences &&
            restored.bakedContextFields == persisted.bakedContextFields &&
            restored.auxiliaryContextIndices ==
                persisted.auxiliaryContextIndices &&
            restored.auxiliaryContextPool == persisted.auxiliaryContextPool,
        "transient pages preserve material identity, context, and handles");
    std::vector<uint8_t> oldVersion = payload;
    if (!oldVersion.empty())
      oldVersion[0u] ^= 0xffu;
    persistenceError.clear();
    check(
        !voxel::svraster::deserializePlanetVoxelTransientPage(
            oldVersion, restored, &persistenceError) &&
            persistenceError.find("version") != std::string::npos,
        "old transient-page versions fail with a clear diagnostic");
    const auto sidecar =
        voxel::svraster::sparseVoxelMaterialSidecarMemory(chunk.bricks);
    check(
        sidecar.logicalBytes() ==
            chunk.bricks.microSurfaceData.size() * sizeof(uint64_t) +
            chunk.bricks.paletteDescriptorIds.size() * sizeof(uint16_t),
        "terrain sidecar memory has explicit phase-zero accounting");
  }

  voxel::svraster::SparseVoxelMicroMaterialLibrary microMaterials;
  voxel::svraster::SparseVoxelMicroDescriptor microGeometry{};
  microGeometry.subdivision = 5u;
  microGeometry.maximumCarveDepth = 5u;
  microGeometry.faceMask = 1u << static_cast<uint32_t>(
      voxel::svraster::SparseVoxelMicroFace::PositiveY);
  microGeometry.geometryProfile =
      voxel::svraster::SparseVoxelMicroGeometryProfile::Staircase;
  microGeometry.materialSeed = 0x5du;
  check(
      microMaterials.setMaterial(3u, microGeometry, {}, &error),
      "terrain micro descriptor registers for the grass material");
  std::vector<uint8_t> transitionHeights;
  for (uint32_t y = 0u; y < storedSide; ++y)
    for (uint32_t x = 0u; x < storedSide; ++x)
      appendU16(
          transitionHeights,
          static_cast<uint16_t>(
              100u + (x / 2u) * 150u + (y / 2u) * 150u));
  std::vector<procedural::PlanetPackageBlockInput> transitionBlocks;
  for (uint8_t face = 0u; face < procedural::kPlanetPackageCubeFaceCount;
       ++face)
  {
    const procedural::PlanetTileAddress faceRoot{face, 0u, 0u, 0u};
    transitionBlocks.push_back(
        {procedural::planetRasterBlockKey(heightChannel, faceRoot),
         transitionHeights});
    transitionBlocks.push_back(
        {procedural::planetRasterBlockKey(materialChannel, faceRoot),
         materials});
  }
  const std::filesystem::path transitionPath =
      std::filesystem::temp_directory_path() /
      "planet-voxel-transition-test.planetpkg";
  procedural::PlanetPackageReader transitionReader;
  check(
      procedural::writePlanetPackage(
          transitionPath, metadata, channels, transitionBlocks, {}, nullptr,
          &error) &&
          transitionReader.open(transitionPath, &error),
      "height-level transition test package opens");
  voxel::svraster::PlanetVoxelChunkSettings microSettings = settings;
  microSettings.microMaterials = &microMaterials;
  microSettings.microGeometry = true;
  microSettings.microMaterialRevision = 1u;
  std::vector<uint8_t> subVoxelHeights;
  for (uint32_t y = 0u; y < storedSide; ++y)
    for (uint32_t x = 0u; x < storedSide; ++x)
      appendU16(
          subVoxelHeights,
          static_cast<uint16_t>(100u + x * 2u + y));
  std::vector<procedural::PlanetPackageBlockInput> subVoxelBlocks;
  for (uint8_t face = 0u;
       face < procedural::kPlanetPackageCubeFaceCount; ++face)
  {
    const procedural::PlanetTileAddress faceRoot{face, 0u, 0u, 0u};
    subVoxelBlocks.push_back(
        {procedural::planetRasterBlockKey(heightChannel, faceRoot),
         subVoxelHeights});
    subVoxelBlocks.push_back(
        {procedural::planetRasterBlockKey(materialChannel, faceRoot),
         materials});
  }
  const std::filesystem::path subVoxelPath =
      std::filesystem::temp_directory_path() /
      "planet-voxel-subvoxel-slope-test.planetpkg";
  procedural::PlanetPackageReader subVoxelReader;
  voxel::svraster::PlanetVoxelChunk subVoxelChunk{};
  const bool builtSubVoxelSlope =
      procedural::writePlanetPackage(
          subVoxelPath, metadata, channels, subVoxelBlocks, {}, nullptr,
          &error) &&
      subVoxelReader.open(subVoxelPath, &error) &&
      voxel::svraster::buildPlanetVoxelChunk(
          subVoxelReader, root, microSettings, subVoxelChunk, &error);
  bool subVoxelSlopeHasNoCoveragePatch = builtSubVoxelSlope;
  for (const uint64_t packedContext : subVoxelChunk.bricks.microSurfaceData)
  {
    subVoxelSlopeHasNoCoveragePatch &=
        voxel::svraster::unpackSparseVoxelMicroContextSurfacePatch(
            packedContext) == 0u;
  }
  check(
      subVoxelSlopeHasNoCoveragePatch,
      "sub-voxel grass slopes do not emit staircase coverage patches");
  voxel::svraster::PlanetVoxelChunk microChunk{};
  check(
      voxel::svraster::buildPlanetVoxelChunk(
          transitionReader, root, microSettings, microChunk, &error),
      "planet chunk construction keeps terrain context without micro descriptors");
  bool foundMicroPaletteEntry = false;
  bool foundContextualVoxel = false;
  bool foundContinuousSurfaceContext = false;
  bool foundQuantitativeSlopeContext = false;
  bool allSlopeContextsNormalized = true;
  bool foundHeightLevelTransition = false;
  for (size_t entry = 0u; entry < microChunk.bricks.palette.size(); ++entry)
  {
    const bool enabled =
        (microChunk.bricks.palette[entry].packed[1] &
         voxel::svraster::kSparseVoxelBrickMicroEnabled) != 0u;
    const uint16_t descriptorId =
        microChunk.bricks.paletteDescriptorIds[entry];
    foundMicroPaletteEntry |= enabled &&
        descriptorId == microMaterials.descriptorId(3u);
  }
  for (const uint64_t packedContext : microChunk.bricks.microSurfaceData)
  {
    const uint32_t context =
        voxel::svraster::unpackSparseVoxelMicroContextValue(packedContext);
    foundContextualVoxel |= context != 0u;
    const bool slope =
        voxel::svraster::sparseVoxelMicroContextIsSurface(context);
    foundContinuousSurfaceContext |= slope;
    if (slope)
    {
      const double magnitude =
          voxel::svraster::sparseVoxelMicroContextTerrainSlope(context);
      foundQuantitativeSlopeContext |=
          (context & voxel::svraster::kSparseVoxelMicroTerrainSlopeMask) != 0u;
      allSlopeContextsNormalized &= magnitude >= 0.0 && magnitude <= 1.0;
    }
    foundHeightLevelTransition |= slope;
  }
  check(
      !foundMicroPaletteEntry,
      "terrain grass palette entries do not enable exact microgeometry");
  check(
      foundContextualVoxel,
      "terrain height-level classification reaches the per-voxel context word");
  check(
      foundContinuousSurfaceContext,
      "sloped terrain retains normalized terrain-surface context");
  check(
      foundHeightLevelTransition,
      "quantized heightmap crossings produce slope context");
  check(
      foundQuantitativeSlopeContext,
      "slope context carries a quantitative terrain slope angle");
  check(
      allSlopeContextsNormalized,
      "terrain slope contexts decode to normalized angles");
  bool foundCoveragePatch = false;
  for (const uint64_t packedContext : microChunk.bricks.microSurfaceData)
  {
    const uint64_t packedPatch =
        voxel::svraster::unpackSparseVoxelMicroContextSurfacePatch(
            packedContext);
    if (packedPatch == 0u)
      continue;
    foundCoveragePatch = true;
  }
  check(
      microChunk.bricks.microSurfaceData.size() ==
              microChunk.bricks.materialIndices.size() &&
          !foundCoveragePatch,
      "terrain voxels no longer carry per-voxel coverage patches");

  // A terrain page must classify its last three parent columns from the real
  // neighbouring page, not from a clamped copy of its own edge. Keep the
  // requested page byte-identical in both packages and change only its +X
  // neighbour; the requested page's terrain context must react while its
  // reported interior height remains unchanged.
  procedural::PlanetPackageMetadata seamMetadata = metadata;
  seamMetadata.maximumTileLevel = 2u;
  const procedural::PlanetTileAddress seamTile{0u, 2u, 1u, 1u};
  const procedural::PlanetTileAddress seamNeighbour{0u, 2u, 2u, 1u};
  const auto uniformHeightPayload = [&](const uint16_t value)
  {
    std::vector<uint8_t> payload;
    payload.reserve(static_cast<size_t>(storedSide) * storedSide * 2u);
    for (uint32_t texel = 0u; texel < storedSide * storedSide; ++texel)
      appendU16(payload, value);
    return payload;
  };
  const auto writeSeamPackage = [&] (
      const std::filesystem::path &seamPath,
      const uint16_t neighbourHeight)
  {
    std::vector<procedural::PlanetPackageBlockInput> seamBlocks;
    const std::vector<uint8_t> flatHeight = uniformHeightPayload(1000u);
    for (uint8_t face = 0u;
         face < procedural::kPlanetPackageCubeFaceCount; ++face)
    {
      const procedural::PlanetTileAddress faceRoot{face, 0u, 0u, 0u};
      seamBlocks.push_back(
          {procedural::planetRasterBlockKey(heightChannel, faceRoot),
           flatHeight});
      seamBlocks.push_back(
          {procedural::planetRasterBlockKey(materialChannel, faceRoot),
           materials});
    }
    seamBlocks.push_back(
        {procedural::planetRasterBlockKey(heightChannel, seamTile),
         flatHeight});
    seamBlocks.push_back(
        {procedural::planetRasterBlockKey(materialChannel, seamTile),
         materials});
    seamBlocks.push_back(
        {procedural::planetRasterBlockKey(heightChannel, seamNeighbour),
         uniformHeightPayload(neighbourHeight)});
    seamBlocks.push_back(
        {procedural::planetRasterBlockKey(materialChannel, seamNeighbour),
         materials});
    return procedural::writePlanetPackage(
        seamPath, seamMetadata, channels, seamBlocks, {}, nullptr, &error);
  };
  const std::filesystem::path flatSeamPath =
      std::filesystem::temp_directory_path() /
      "planet-voxel-flat-seam-test.planetpkg";
  const std::filesystem::path fallingSeamPath =
      std::filesystem::temp_directory_path() /
      "planet-voxel-falling-seam-test.planetpkg";
  procedural::PlanetPackageReader flatSeamReader;
  procedural::PlanetPackageReader fallingSeamReader;
  check(
      writeSeamPackage(flatSeamPath, 1000u) &&
          flatSeamReader.open(flatSeamPath, &error) &&
          writeSeamPackage(fallingSeamPath, 850u) &&
          fallingSeamReader.open(fallingSeamPath, &error),
      "terrain page seam regression packages open");
  voxel::svraster::PlanetVoxelChunkSettings seamSettings = microSettings;
  seamSettings.patchEdgeMetres = 8.0;
  voxel::svraster::PlanetVoxelChunk flatSeamChunk{};
  voxel::svraster::PlanetVoxelChunk fallingSeamChunk{};
  const bool seamChunksBuilt = voxel::svraster::buildPlanetVoxelChunk(
      flatSeamReader, seamTile, seamSettings, flatSeamChunk, &error) &&
      voxel::svraster::buildPlanetVoxelChunk(
          fallingSeamReader, seamTile, seamSettings,
          fallingSeamChunk, &error);
  check(
      seamChunksBuilt,
      "terrain pages sample a canonical three-parent source-height halo");
  check(
      seamChunksBuilt &&
          flatSeamChunk.centerSurfaceMetres ==
              fallingSeamChunk.centerSurfaceMetres &&
          flatSeamChunk.minimumSurfaceMetres ==
              fallingSeamChunk.minimumSurfaceMetres &&
          flatSeamChunk.maximumSurfaceMetres ==
              fallingSeamChunk.maximumSurfaceMetres &&
          flatSeamChunk.bricks.microSurfaceData !=
              fallingSeamChunk.bricks.microSurfaceData,
      "neighbouring terrain changes slope context at the seam without changing the emitted page interior");

  voxel::svraster::PlanetVoxelChunkSettings detailedSettings = settings;
  detailedSettings.proceduralDetailStrength = 1.0;
  voxel::svraster::PlanetVoxelChunk detailedChunkA{};
  voxel::svraster::PlanetVoxelChunk detailedChunkB{};
  check(
      voxel::svraster::buildPlanetVoxelChunk(
          reader, requested, detailedSettings, detailedChunkA, &error) &&
          voxel::svraster::buildPlanetVoxelChunk(
              reader, requested, detailedSettings, detailedChunkB, &error) &&
          detailedChunkA.centerSurfaceMetres ==
              detailedChunkB.centerSurfaceMetres &&
          detailedChunkA.minimumSurfaceMetres ==
              detailedChunkB.minimumSurfaceMetres &&
          detailedChunkA.maximumSurfaceMetres ==
              detailedChunkB.maximumSurfaceMetres,
      "procedural sub-texel relief regenerates deterministically");
  check(
      detailedChunkA.centerSurfaceMetres != chunk.centerSurfaceMetres,
      "procedural detail adds relief below the stored raster resolution");

  voxel::svraster::PlanetVoxelChunkSettings finePageSettings = settings;
  finePageSettings.horizontalVoxelCount = 24u;
  finePageSettings.patchEdgeMetres = 4.8;
  finePageSettings.maximumVerticalVoxels = 32u;
  voxel::svraster::PlanetVoxelChunk finePage{};
  check(
      voxel::svraster::buildPlanetVoxelChunk(
          reader, requested, finePageSettings, finePage, &error),
      "a non-power-of-two brick-aligned runtime page builds");
  check(
      std::abs(finePage.edgeMetres - 4.8) < 1e-9 &&
          std::abs(finePage.voxelSizeMetres - 0.2) < 1e-9,
      "runtime page extent produces 20 cm leaf voxels");
  voxel::svraster::PlanetVoxelChunkSettings solidFinePageSettings =
      finePageSettings;
  solidFinePageSettings.surfaceShellDepthVoxels = 0u;
  voxel::svraster::PlanetVoxelChunk solidFinePage{};
  check(
      voxel::svraster::buildPlanetVoxelChunk(
          reader, requested, solidFinePageSettings, solidFinePage, &error) &&
          finePage.leafVoxelCount < solidFinePage.leafVoxelCount,
      "surface-shell terrain omits hidden geology retained by solid columns");

  procedural::PlanetTileAddress landSpawn{};
  double landSurface = 0.0;
  uint8_t landMaterial = 0u;
  check(
      voxel::svraster::findPlanetLandSpawnTile(
          reader, 2u, landSpawn, &landSurface, &landMaterial, &error),
      "a dry-land spawn is selected from coarse package maps");
  check(
      landSurface > 0.0 &&
          landMaterial == static_cast<uint8_t>(procedural::PolygonTerrainMaterial::Grass),
      "the selected spawn rejects ocean materials");

  voxel::svraster::PlanetVoxelChunkSet chunkGrid{};
  std::vector<double> gridProgress;
  const procedural::PlanetTileAddress gridCenter{0u, 2u, 2u, 1u};
  check(
      voxel::svraster::buildPlanetVoxelChunkGrid(
          reader, gridCenter, 1u, settings, chunkGrid, &error,
          [&gridProgress](const char *, const double progress)
          {
            gridProgress.push_back(progress);
          }),
      "a three-by-three window builds from independently loaded chunks");
  check(
      chunkGrid.chunks.size() == 9u &&
          chunkGrid.minimumWorldMetres[0] == -1500.0 &&
          chunkGrid.maximumWorldMetres[0] == 1500.0 &&
          chunkGrid.minimumWorldMetres[2] == -1500.0 &&
          chunkGrid.maximumWorldMetres[2] == 1500.0,
      "neighbor chunks occupy a shared three-kilometre tangent-space grid");
  check(
      chunkGrid.bricks.nodes.size() > chunk.bricks.nodes.size() &&
          chunkGrid.bricks.rootCount > chunk.bricks.rootCount &&
          chunkGrid.leafVoxelCount >= chunk.leafVoxelCount * 9u,
      "chunk brick hierarchies combine into one multi-root resident hierarchy");
  check(
      !gridProgress.empty() && gridProgress.back() == 1.0 &&
          std::is_sorted(gridProgress.begin(), gridProgress.end()),
      "multi-chunk loading reports monotonic aggregate progress");

  voxel::svraster::PlanetVoxelChunkSettings clipmapSettings =
      finePageSettings;
  clipmapSettings.strategy =
      voxel::svraster::PlanetVoxelChunkStrategy::ClipmapRings;
  clipmapSettings.clipmapLevelCount = 4u;
  clipmapSettings.requestedLODMask = 0x0fu;
  clipmapSettings.clipmapInnerHoleRadius = 1u;
  voxel::svraster::PlanetVoxelChunkCache clipmapCache{};
  voxel::svraster::PlanetVoxelChunkSet clipmapChunks{};
  voxel::svraster::PlanetVoxelChunkCacheUpdate clipmapUpdate{};
  // Keep enough cube-quadtree address space at the coarsest level for the
  // five-page ring to contain unique pages. A tiny level-five anchor can wrap
  // this deliberately oversized test window onto the same cube tile twice,
  // which is not representative of the level-19 runtime terrain.
  const procedural::PlanetTileAddress clipmapCenter{0u, 8u, 128u, 64u};
  const bool clipmapBuilt = voxel::svraster::updatePlanetVoxelChunkCache(
      reader, clipmapCenter, 0, 0, 2u, clipmapSettings, clipmapCache,
      clipmapChunks, &clipmapUpdate, &error);
  if (!clipmapBuilt)
    std::cout << "  clipmap error: " << error << '\n';
  check(
      clipmapBuilt,
      "a long-range four-level terrain clipmap builds mixed-resolution page rings");
  check(
      clipmapBuilt &&
          clipmapChunks.materialFieldRegions.size() ==
              clipmapChunks.chunks.size(),
      "every resident planet region carries one generated material-field tile set");
  voxel::material::VoxelMaterialManager fieldMaterials;
  const auto fieldPresets =
      voxel::material::defaultTerrainVoxelMaterialPresets(false);
  const std::array<size_t, 3u> rockyMaterials{
    static_cast<size_t>(procedural::PolygonTerrainMaterial::Rock),
    static_cast<size_t>(procedural::PolygonTerrainMaterial::Bare),
    static_cast<size_t>(procedural::PolygonTerrainMaterial::Scorched),
  };
  bool rockyMaterialsUseOnlyTerrainSlopes = true;
  for (const size_t materialIndex : rockyMaterials)
  {
    const auto asset = voxel::material::terrainVoxelMaterialAsset(
        materialIndex, fieldPresets[materialIndex]);
    const bool hasHueOutput = std::any_of(
        asset.shadingProgram.instructions.begin(),
        asset.shadingProgram.instructions.end(),
        [](const auto &instruction)
        {
          return instruction.opcode ==
              voxel::material::VoxelShadingOpcode::OutputHueVariation;
        });
    const bool hasBrightnessOutput = std::any_of(
        asset.shadingProgram.instructions.begin(),
        asset.shadingProgram.instructions.end(),
        [](const auto &instruction)
        {
          return instruction.opcode ==
              voxel::material::VoxelShadingOpcode::OutputBrightness;
        });
    const auto &geometry = fieldPresets[materialIndex].geometry;
    const double flatCoverage =
        voxel::svraster::sparseVoxelMicroExpectedCoverage(
            geometry,
            static_cast<uint32_t>(
                voxel::svraster::SparseVoxelMicroContext::FlatSurface));
    const double slopeCoverage =
        voxel::svraster::sparseVoxelMicroExpectedCoverage(
            geometry,
            voxel::svraster::sparseVoxelMicroTerrainSlopeContext(0.5));
    rockyMaterialsUseOnlyTerrainSlopes &=
        geometry.geometryProfile ==
            voxel::svraster::SparseVoxelMicroGeometryProfile::Staircase &&
        asset.geometryProgram.instructions.empty() && hasHueOutput &&
        hasBrightnessOutput && std::abs(flatCoverage - 1.0) < 1.0e-12 &&
        slopeCoverage < 1.0;
  }
  check(
      rockyMaterialsUseOnlyTerrainSlopes,
      "rock, bare, and scorched terrain keep authored slopes without random microgeometry");
  bool registeredFieldMaterials = true;
  for (size_t index = 0u; index < fieldPresets.size(); ++index)
  {
    voxel::material::VoxelMaterialDefinition definition{};
    voxel::material::VoxelMaterialHandle handle =
        voxel::material::kInvalidVoxelMaterialHandle;
    const auto asset = voxel::material::terrainVoxelMaterialAsset(
        index, fieldPresets[index]);
    registeredFieldMaterials &= asset.definition(definition, &error) &&
        fieldMaterials.registerMaterial(definition, handle, &error) &&
        fieldMaterials.bindSourceMaterial(
            static_cast<uint32_t>(index), handle, &error);
  }
  voxel::material::VoxelMaterialFieldAtlas fieldAtlas;
  voxel::material::VoxelMaterialFieldAtlasSnapshot fieldSnapshot{};
  const bool populatedFieldAtlas = registeredFieldMaterials && clipmapBuilt &&
      voxel::svraster::populatePlanetVoxelMaterialFieldAtlas(
          clipmapChunks, fieldMaterials, fieldAtlas, &error) &&
      fieldAtlas.build(fieldMaterials, fieldSnapshot, &error);
  bool fieldSlopeComposition = populatedFieldAtlas &&
      !clipmapChunks.materialFieldRegions.empty();
  if (fieldSlopeComposition)
  {
    const auto grassMaterial = static_cast<size_t>(
        procedural::PolygonTerrainMaterial::Grass);
    const float hueAmplitude =
        fieldPresets[grassMaterial].programmableHueVariation;
    const float brightnessAmplitude =
        fieldPresets[grassMaterial].programmableBrightnessVariation;
    const auto handle = fieldMaterials.handleForSourceMaterial(
        static_cast<uint32_t>(
            procedural::PolygonTerrainMaterial::Grass));
    const auto &region = clipmapChunks.materialFieldRegions.front();
    const auto *mapping = voxel::material::findVoxelMaterialFieldMapping(
        fieldSnapshot.mappingTable, handle, region.regionMappingId);
    fieldSlopeComposition &= mapping != nullptr &&
        mapping->channelMask() == 0x7u;
    const auto descriptor = voxel::svraster::unpackSparseVoxelMicroGeometry(
        fieldMaterials.descriptor(handle));
    voxel::svraster::VoxelMaterialSampleAddressGPU address{};
    address.canonicalMicroCoordinate = {
      region.canonicalParentOrigin[0] *
          static_cast<int32_t>(descriptor.subdivision),
      0,
      region.canonicalParentOrigin[1] *
          static_cast<int32_t>(descriptor.subdivision)};
    address.regionMappingId = region.regionMappingId;
    const auto sampled = voxel::material::sampleVoxelMaterialFields(
        fieldSnapshot, fieldMaterials.materialFieldDefaults()[handle],
        handle, descriptor.subdivision, address,
        voxel::material::voxelShadingRequiredMaterialFields(
            fieldMaterials.slot(handle).shadingProgram));
    voxel::material::VoxelShadingInputs flat{};
    flat.fields = sampled;
    flat.terrainSlopeClassified = true;
    flat.stableRandom = 0.25f;
    voxel::material::VoxelShadingInputs steep = flat;
    steep.terrainSlope = 1.0f;
    voxel::material::VoxelShadingResult flatResult{};
    voxel::material::VoxelShadingResult steepResult{};
    fieldSlopeComposition &=
        voxel::material::evaluateVoxelShadingProgram(
            fieldMaterials.slot(handle).shadingProgram,
            flat, flatResult, &error) &&
        voxel::material::evaluateVoxelShadingProgram(
            fieldMaterials.slot(handle).shadingProgram,
            steep, steepResult, &error) &&
        flatResult.layerWeight == 0.0f &&
        steepResult.layerWeight == 1.0f &&
        std::abs(
            flatResult.hueVariation - sampled[2] * hueAmplitude) < 1.0e-6f &&
        std::abs(
            flatResult.brightness -
            (1.0f - brightnessAmplitude +
                2.0f * brightnessAmplitude * flat.stableRandom)) < 1.0e-6f;
  }
  check(
      fieldSlopeComposition,
      "flat terrain uses field-driven hue and brightness while steep terrain retains soil blending");
  const auto lodPageCount = [&](const uint32_t lod)
  {
    return static_cast<uint32_t>(std::count_if(
        clipmapCache.entries.begin(), clipmapCache.entries.end(),
        [&](const voxel::svraster::PlanetVoxelChunkCacheEntry &entry)
        {
          return entry.clipmapLOD == lod;
        }));
  };
  bool clipmapLeafScalesArePerPage = true;
  bool clipmapLevelsAreEncoded = true;
  bool clipmapCanonicalGridIsExact = true;
  bool clipmapRegionMappingsAreStable = true;
  for (const auto &record : clipmapChunks.chunks)
  {
    const uint32_t expectedRegionMapping =
        voxel::svraster::planetVoxelMaterialRegionMappingId(
            record.requestedTile);
    clipmapRegionMappingsAreStable &= expectedRegionMapping != 0u;
    const float expectedVoxelSize = std::ldexp(0.2f, record.clipmapLOD);
    bool foundLeafBrick = false;
    for (uint32_t index = 0u; index < record.brickCount; ++index)
    {
      const auto &node = clipmapChunks.bricks.nodes[
          record.brickNodeOffset + index];
      clipmapLevelsAreEncoded &= ((node.level >> 8u) & 0xffu) ==
          record.clipmapLOD;
      clipmapRegionMappingsAreStable &=
          node.regionMappingId == expectedRegionMapping;
      const uint32_t localLevel = node.level & 0xffu;
      const int32_t expectedScale = static_cast<int32_t>(
          1u << (localLevel + record.clipmapLOD));
      clipmapCanonicalGridIsExact &=
          node.canonicalGridOriginScale[3] == expectedScale;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        const double canonicalMetres =
            static_cast<double>(node.canonicalGridOriginScale[axis]) *
            clipmapChunks.voxelSizeMetres;
        clipmapCanonicalGridIsExact &= std::abs(
            canonicalMetres - node.minimumVoxelSize[axis]) < 2.0e-4;
      }
      clipmapCanonicalGridIsExact &= std::abs(
          static_cast<double>(node.canonicalGridOriginScale[3]) *
              clipmapChunks.voxelSizeMetres -
          node.minimumVoxelSize[3]) < 2.0e-6;
      if ((node.level & 0xffu) == 0u &&
          std::abs(node.minimumVoxelSize[3] - expectedVoxelSize) < 1e-6f)
      {
        foundLeafBrick = true;
        break;
      }
    }
    clipmapLeafScalesArePerPage &= foundLeafBrick;
  }
  check(
      clipmapChunks.chunks.size() == 73u && lodPageCount(0u) == 25u &&
          lodPageCount(1u) == 16u && lodPageCount(2u) == 16u &&
          lodPageCount(3u) == 16u &&
          std::abs(
              clipmapChunks.maximumWorldMetres[0] -
                  clipmapChunks.minimumWorldMetres[0] -
              192.0) < 1e-6,
      "long-range clipmap keeps the full near square and omits covered coarse centers");
  check(
      clipmapRegionMappingsAreStable,
      "planet brick nodes carry stable logical region mappings independent of physical clipmap slots");
  check(
      std::abs(clipmapChunks.bricks.finestVoxelSize - 0.2f) < 1e-6f &&
          clipmapCache.clipmapSlotCount == 73u &&
          clipmapLeafScalesArePerPage &&
          clipmapLevelsAreEncoded &&
          clipmapCanonicalGridIsExact &&
          clipmapChunks.bricks.nodes.size() ==
              std::accumulate(
                  clipmapCache.entries.begin(), clipmapCache.entries.end(),
                  size_t{0u},
                  [&](const size_t count,
                      const voxel::svraster::PlanetVoxelChunkCacheEntry &entry)
                  {
                    return count +
                        clipmapCache.clipmapSlotNodeCapacity[
                            entry.clipmapSlot];
                  }),
      "spatial terrain LODs retain an exact canonical grid and bounded 2x scale progression");
  {
    voxel::svraster::PlanetVoxelChunkSettings distantSettings =
        finePageSettings;
    distantSettings.patchEdgeMetres = 31.25;
    distantSettings.strategy =
        voxel::svraster::PlanetVoxelChunkStrategy::ClipmapRings;
    distantSettings.clipmapLevelCount = 11u;
    distantSettings.requestedLODMask = (1u << 11u) - 1u;
    distantSettings.clipmapInnerHoleRadius = 0u;
    // Production long-distance terrain keeps one source resolution. The
    // spatial page footprint supplies coverage while projected brick LOD picks
    // the visible cut; an additional page-resolution drop would compound this
    // 2x sequence into a 4x discontinuity.
    distantSettings.clipmapResolutionDropInterval = 0u;
    distantSettings.clipmapMinimumHorizontalVoxelCount =
        distantSettings.horizontalVoxelCount;
    voxel::svraster::PlanetVoxelChunkCache distantCache{};
    voxel::svraster::PlanetVoxelChunkSet distantChunks{};
    voxel::svraster::PlanetVoxelChunkCacheUpdate distantUpdate{};
    const procedural::PlanetTileAddress distantCenter{
        0u, 16u, 32768u, 16384u};
    const bool distantBuilt = voxel::svraster::updatePlanetVoxelChunkCache(
        reader, distantCenter, 0, 0, 1u, distantSettings, distantCache,
        distantChunks, &distantUpdate, &error);
    const size_t outerPageCount = std::count_if(
        distantChunks.chunks.begin(), distantChunks.chunks.end(),
        [](const auto &record)
        {
          return record.clipmapLOD ==
              10u;
        });
    const auto outerRecord = std::find_if(
        distantChunks.chunks.begin(), distantChunks.chunks.end(),
        [](const auto &record) { return record.clipmapLOD == 10u; });
    bool outerCanonicalScale = outerRecord != distantChunks.chunks.end();
    bool outerRingCannotRequestProgressiveRewrite = outerCanonicalScale;
    bool foundOuterLeaf = false;
    if (outerCanonicalScale)
      for (uint32_t index = 0u; index < outerRecord->brickCount; ++index)
      {
        const auto &node = distantChunks.bricks.nodes[
            outerRecord->brickNodeOffset + index];
        if ((node.level & 0xffu) == 0u)
        {
          foundOuterLeaf = true;
          outerCanonicalScale &=
              std::abs(node.minimumVoxelSize[3] -
                  static_cast<float>(
                      32000.0 / distantSettings.horizontalVoxelCount)) <
                  1.0e-3f &&
              node.canonicalGridOriginScale[3] == 1024;
        }
        if (!voxel::svraster::sparseVoxelBrickHasFinerTerrainClip(node))
          outerRingCannotRequestProgressiveRewrite &=
              node.instanceIndex ==
                  voxel::svraster::kSparseVoxelBrickInvalidIndex;
      }
    check(
        distantBuilt && distantChunks.chunks.size() == 89u &&
            distantCache.clipmapSlotCount == 89u && outerPageCount == 8u &&
            outerRecord != distantChunks.chunks.end() &&
            std::abs(outerRecord->edgeMetres - 32000.0) < 1.0e-4 &&
            std::abs(outerRecord->voxelSizeMetres -
                32000.0 / distantSettings.horizontalVoxelCount) < 1.0e-4 &&
            outerRecord->leafVoxelCount > 0u &&
            outerCanonicalScale && foundOuterLeaf &&
            outerRingCannotRequestProgressiveRewrite &&
            distantChunks.maximumWorldMetres[0] -
                    distantChunks.minimumWorldMetres[0] >
                25000.0,
        "eleven spatial LODs cover 25 km without compounding page and spatial voxel scales");
    check(
        voxel::svraster::planetVoxelClipmapLevelCountForDistance(
            31.25, 1u, 25'000.0) == 11u &&
            voxel::svraster::planetVoxelClipmapLevelCountForDistance(
                31.25, 3u, 25'000.0) == 10u,
        "automatic spatial clipmap count conservatively reaches the requested distance");
  }
  {
    voxel::svraster::PlanetVoxelChunkSettings requestedSettings =
        finePageSettings;
    requestedSettings.horizontalVoxelCount = 64u;
    requestedSettings.patchEdgeMetres = 12.8;
    requestedSettings.strategy =
        voxel::svraster::PlanetVoxelChunkStrategy::ProgressiveLOD;
    requestedSettings.clipmapLevelCount = 4u;
    requestedSettings.requestedLODMask = 1u << 3u;
    voxel::svraster::PlanetVoxelChunkCache requestedCache{};
    voxel::svraster::PlanetVoxelChunkSet requestedChunks{};
    voxel::svraster::PlanetVoxelChunkCacheUpdate requestedUpdate{};
    const bool requestedBuilt =
        voxel::svraster::updatePlanetVoxelChunkCache(
            reader, clipmapCenter, 0, 0, 1u, requestedSettings,
            requestedCache, requestedChunks, &requestedUpdate, &error);
    std::array<size_t, 4u> requestedCounts{};
    for (const auto &record : requestedChunks.chunks)
      if (record.clipmapLOD < requestedCounts.size())
        ++requestedCounts[record.clipmapLOD];
    std::vector<voxel::svraster::PlanetVoxelChunkRefinementRequest>
        rejectedRequests;
    if (!requestedCache.entries.empty())
      rejectedRequests.push_back(
          {requestedCache.entries.front().clipmapSlot, 100u});
    const bool rejectedRefinement =
        voxel::svraster::updatePlanetVoxelChunkCache(
            reader, clipmapCenter, 0, 0, 1u, requestedSettings,
            requestedCache, requestedChunks, &requestedUpdate, &error,
            {}, nullptr, true, &rejectedRequests);
    check(
        requestedBuilt && rejectedRefinement &&
            requestedChunks.chunks.size() == 9u &&
            requestedCache.requestedChunkCount == 9u &&
            requestedCounts == std::array<size_t, 4u>{0u, 0u, 0u, 9u} &&
            requestedUpdate.builtChunkCount == 0u &&
            requestedChunks.edgeMetres == requestedSettings.patchEdgeMetres &&
            requestedChunks.voxelSizeMetres ==
                requestedSettings.patchEdgeMetres /
                    requestedSettings.horizontalVoxelCount,
        "progressive terrain never refines below the finest admitted LOD");
  }
  {
    voxel::svraster::PlanetVoxelChunkSettings progressiveSettings =
        finePageSettings;
    progressiveSettings.horizontalVoxelCount = 64u;
    progressiveSettings.patchEdgeMetres = 12.8;
    progressiveSettings.strategy =
        voxel::svraster::PlanetVoxelChunkStrategy::ProgressiveLOD;
    progressiveSettings.clipmapLevelCount = 4u;
    progressiveSettings.requestedLODMask = 0x0fu;
    progressiveSettings.maximumGeneratedChunksPerUpdate = 1u;
    progressiveSettings.maximumResidentChunks = 9u;
    progressiveSettings.workerCount = 1u;
    voxel::svraster::PlanetVoxelChunkCache progressiveCache{};
    voxel::svraster::PlanetVoxelChunkSet progressiveChunks{};
    voxel::svraster::PlanetVoxelChunkCacheUpdate progressiveUpdate{};
    const procedural::PlanetTileAddress progressiveCenter{
        0u, 8u, 127u, 64u};
    const bool progressiveBuilt =
        voxel::svraster::updatePlanetVoxelChunkCache(
            reader, progressiveCenter, 0, 0, 1u, progressiveSettings,
            progressiveCache, progressiveChunks, &progressiveUpdate, &error);
    bool coarseCut = progressiveBuilt &&
        progressiveChunks.chunks.size() == 9u &&
        progressiveCache.clipmapSlotCount == 9u &&
        progressiveUpdate.builtChunkCount == 9u;
    for (const auto &record : progressiveChunks.chunks)
      coarseCut &= record.clipmapLOD == 3u;
    check(
        coarseCut,
        "toroidal terrain starts with nine coarse independent chunk instances");

    const uint64_t fixedLayoutRevision =
        progressiveChunks.clipmapLayoutRevision;

    const auto distanceSquared = [](const auto &entry)
    {
      return entry.worldCenterXMetres * entry.worldCenterXMetres +
          entry.worldCenterZMetres * entry.worldCenterZMetres;
    };
    const auto nearest = std::min_element(
        progressiveCache.entries.begin(), progressiveCache.entries.end(),
        [&](const auto &left, const auto &right)
        {
          return distanceSquared(left) < distanceSquared(right);
        });
    const auto farthest = std::max_element(
        progressiveCache.entries.begin(), progressiveCache.entries.end(),
        [&](const auto &left, const auto &right)
        {
          return distanceSquared(left) < distanceSquared(right);
        });
    const auto refinedTile = nearest->tile;
    const auto deferredTile = farthest->tile;
    const uint32_t refinedSlot = nearest->clipmapSlot;
    const std::vector<voxel::svraster::PlanetVoxelChunkRefinementRequest>
        rootRequests{{farthest->clipmapSlot, 10u},
                     {nearest->clipmapSlot, 10u}};
    const bool rootRefined = voxel::svraster::updatePlanetVoxelChunkCache(
        reader, progressiveCenter, 0, 0, 1u, progressiveSettings,
        progressiveCache, progressiveChunks, &progressiveUpdate, &error,
        {}, nullptr, true, &rootRequests);
    const auto hasPage = [&](const procedural::PlanetTileAddress &tile,
                             const uint32_t lod)
    {
      return std::any_of(
          progressiveCache.entries.begin(), progressiveCache.entries.end(),
          [&](const auto &entry)
          {
            return entry.clipmapLOD == lod && entry.tile == tile;
          });
    };
    const auto refinedEntry = std::find_if(
        progressiveCache.entries.begin(), progressiveCache.entries.end(),
        [&](const auto &entry) { return entry.tile == refinedTile; });
    const bool exactRewrite = rootRefined &&
        !hasPage(refinedTile, 3u) &&
        hasPage(refinedTile, 2u) &&
        hasPage(deferredTile, 3u) &&
        refinedEntry != progressiveCache.entries.end() &&
        refinedEntry->clipmapSlot == refinedSlot &&
        progressiveCache.entries.size() == 9u &&
        progressiveChunks.clipmapLayoutRevision == fixedLayoutRevision &&
        progressiveUpdate.builtChunkCount == 1u &&
        progressiveUpdate.reusedChunkCount == 8u &&
        progressiveUpdate.pendingChunkCount == 1u;
    check(
        exactRewrite,
        "equal GPU requests rewrite the nearer chunk first without changing its physical slot");

    auto requestedChild = std::find_if(
        progressiveCache.entries.begin(), progressiveCache.entries.end(),
        [](const auto &entry) { return entry.clipmapLOD == 2u; });
    const std::vector<voxel::svraster::PlanetVoxelChunkRefinementRequest>
        childRequests{{requestedChild->clipmapSlot, 100u}};
    const bool childRefined = voxel::svraster::updatePlanetVoxelChunkCache(
        reader, progressiveCenter, 0, 0, 1u, progressiveSettings,
        progressiveCache, progressiveChunks, &progressiveUpdate, &error,
        {}, nullptr, true, &childRequests);
    const std::array<size_t, 4u> cutCounts{
      static_cast<size_t>(std::count_if(
          progressiveCache.entries.begin(), progressiveCache.entries.end(),
          [](const auto &entry) { return entry.clipmapLOD == 0u; })),
      static_cast<size_t>(std::count_if(
          progressiveCache.entries.begin(), progressiveCache.entries.end(),
          [](const auto &entry) { return entry.clipmapLOD == 1u; })),
      static_cast<size_t>(std::count_if(
          progressiveCache.entries.begin(), progressiveCache.entries.end(),
          [](const auto &entry) { return entry.clipmapLOD == 2u; })),
      static_cast<size_t>(std::count_if(
          progressiveCache.entries.begin(), progressiveCache.entries.end(),
          [](const auto &entry) { return entry.clipmapLOD == 3u; })),
    };
    bool noProgressiveClipTags = childRefined &&
        cutCounts == std::array<size_t, 4u>{0u, 1u, 0u, 8u} &&
        progressiveCache.entries.size() == 9u &&
        progressiveChunks.clipmapLayoutRevision == fixedLayoutRevision;
    for (const auto &node : progressiveChunks.bricks.nodes)
      noProgressiveClipTags &=
          !voxel::svraster::sparseVoxelBrickHasFinerTerrainClip(node);
    check(
        noProgressiveClipTags,
        "GPU feedback advances only the requested chunk's internal LOD");

    std::vector<std::pair<procedural::PlanetTileAddress, uint32_t>>
        retainedSlots;
    for (const auto &entry : progressiveCache.entries)
      retainedSlots.push_back({entry.tile, entry.clipmapSlot});
    const bool walked = voxel::svraster::updatePlanetVoxelChunkCache(
        reader, progressiveCenter, 1, 0, 1u, progressiveSettings,
        progressiveCache, progressiveChunks, &progressiveUpdate, &error,
        {}, nullptr, false, nullptr);
    size_t stableRetainedSlots = 0u;
    for (const auto &retainedSlot : retainedSlots)
    {
      const auto tile = retainedSlot.first;
      const uint32_t slot = retainedSlot.second;
      const auto retained = std::find_if(
          progressiveCache.entries.begin(), progressiveCache.entries.end(),
          [&](const auto &entry) { return entry.tile == tile; });
      if (retained != progressiveCache.entries.end() &&
          retained->clipmapSlot == slot)
        ++stableRetainedSlots;
    }
    check(
        walked && progressiveCache.entries.size() == 9u &&
            progressiveCache.clipmapSlotCount == 9u &&
            progressiveChunks.clipmapLayoutRevision == fixedLayoutRevision &&
            progressiveUpdate.builtChunkCount == 3u &&
            progressiveUpdate.reusedChunkCount == 6u &&
            stableRetainedSlots == 6u,
        "walking remaps only the entering column through stable toroidal slots");
    const bool walkedBack = voxel::svraster::updatePlanetVoxelChunkCache(
        reader, progressiveCenter, 0, 0, 1u, progressiveSettings,
        progressiveCache, progressiveChunks, &progressiveUpdate, &error,
        {}, nullptr, false, nullptr);
    check(
        walkedBack && progressiveCache.entries.size() == 9u &&
            progressiveUpdate.reusedChunkCount == 6u &&
            progressiveUpdate.restoredMemoryChunkCount == 3u &&
            progressiveUpdate.builtChunkCount == 0u,
        "walking back restores the evicted toroidal column at its original LOD");
  }
  bool cascadeOwnershipIsExplicit = true;
  bool cascadeVerticalGridsAreUnbiased = true;
  for (const auto &record : clipmapChunks.chunks)
  {
    std::array<float, 4> expectedClip{};
    if (record.clipmapLOD != 0u)
    {
      double minimumX = std::numeric_limits<double>::max();
      double minimumZ = std::numeric_limits<double>::max();
      double maximumX = std::numeric_limits<double>::lowest();
      double maximumZ = std::numeric_limits<double>::lowest();
      for (const auto &entry : clipmapCache.entries)
      {
        if (entry.clipmapLOD >= record.clipmapLOD)
          continue;
        const double halfEdge = entry.chunk.edgeMetres * 0.5;
        minimumX = std::min(minimumX, entry.worldCenterXMetres - halfEdge);
        minimumZ = std::min(minimumZ, entry.worldCenterZMetres - halfEdge);
        maximumX = std::max(maximumX, entry.worldCenterXMetres + halfEdge);
        maximumZ = std::max(maximumZ, entry.worldCenterZMetres + halfEdge);
      }
      expectedClip = {
        static_cast<float>(minimumX), static_cast<float>(minimumZ),
        static_cast<float>(maximumX), static_cast<float>(maximumZ)};
    }
    for (uint32_t index = 0u; index < record.brickCount; ++index)
    {
      const auto &node = clipmapChunks.bricks.nodes[
          record.brickNodeOffset + index];
      const float brickExtent = node.minimumVoxelSize[3] *
          voxel::svraster::kSparseVoxelBrickSide;
      const std::array<float, 4> expectedIntersection{
        std::max(node.minimumVoxelSize[0], expectedClip[0]),
        std::max(node.minimumVoxelSize[2], expectedClip[1]),
        std::min(node.minimumVoxelSize[0] + brickExtent, expectedClip[2]),
        std::min(node.minimumVoxelSize[2] + brickExtent, expectedClip[3]),
      };
      const bool intersects = record.clipmapLOD != 0u &&
          expectedIntersection[0] < expectedIntersection[2] &&
          expectedIntersection[1] < expectedIntersection[3];
      const bool tagged =
          voxel::svraster::sparseVoxelBrickHasFinerTerrainClip(node);
      cascadeOwnershipIsExplicit &= tagged == intersects;
      if (tagged)
      {
        const auto actual =
            voxel::svraster::sparseVoxelBrickFinerTerrainClipBounds(node);
        const float tolerance = node.minimumVoxelSize[3] /
            voxel::svraster::kSparseVoxelBrickTerrainClipQuantization + 1e-5f;
        for (uint32_t coordinate = 0u; coordinate < 4u; ++coordinate)
          cascadeOwnershipIsExplicit &= std::abs(
              actual[coordinate] - expectedIntersection[coordinate]) <=
              tolerance;
        // This is an exclusion rectangle. Quantization may make it smaller,
        // but it must never extend outside the finer cascade and punch a hole
        // in the coarse fallback.
        cascadeOwnershipIsExplicit &=
            actual[0] + 1e-5f >= expectedIntersection[0] &&
            actual[1] + 1e-5f >= expectedIntersection[1] &&
            actual[2] <= expectedIntersection[2] + 1e-5f &&
            actual[3] <= expectedIntersection[3] + 1e-5f;
      }
    }
    const auto cacheEntry = std::find_if(
        clipmapCache.entries.begin(), clipmapCache.entries.end(),
        [&](const voxel::svraster::PlanetVoxelChunkCacheEntry &entry)
        {
          return entry.clipmapSlot == record.clipmapSlot;
        });
    if (cacheEntry != clipmapCache.entries.end() && record.brickCount != 0u)
    {
      const float expectedY =
          cacheEntry->chunk.bricks.nodes.front().minimumVoxelSize[1] +
          static_cast<float>(cacheEntry->chunk.elevationOriginMetres -
                             clipmapChunks.elevationOriginMetres);
      cascadeVerticalGridsAreUnbiased &= std::abs(
          clipmapChunks.bricks.nodes[record.brickNodeOffset]
                  .minimumVoxelSize[1] -
              expectedY) < 1e-5f;
    }
    else
    {
      cascadeVerticalGridsAreUnbiased = false;
    }
  }
  check(
      cascadeOwnershipIsExplicit,
      "coarse terrain nodes clip the rectangle owned by finer cascades");
  check(
      cascadeVerticalGridsAreUnbiased,
      "coarse cascade packing does not introduce a vertical terrain step");
  std::vector<bool> clipmapSlots(73u, false);
  bool validChunkAccelerationRanges = clipmapChunks.chunks.size() == 73u &&
      clipmapCache.clipmapSlotNodeCapacity.size() == 73u &&
      clipmapCache.clipmapSlotChildCapacity.size() == 73u &&
      clipmapCache.clipmapSlotRootCapacity.size() == 73u &&
      clipmapCache.clipmapSlotMaterialCapacity.size() == 73u &&
      clipmapCache.clipmapSlotPaletteCapacity.size() == 73u;
  for (const auto &record : clipmapChunks.chunks)
  {
    const bool validSlot = record.clipmapSlot < clipmapSlots.size() &&
        record.clipmapSlot < clipmapCache.clipmapSlotNodeCapacity.size() &&
        record.clipmapSlot < clipmapCache.clipmapSlotChildCapacity.size() &&
        record.clipmapSlot < clipmapCache.clipmapSlotRootCapacity.size() &&
        record.clipmapSlot < clipmapCache.clipmapSlotMaterialCapacity.size() &&
        record.clipmapSlot < clipmapCache.clipmapSlotPaletteCapacity.size();
    validChunkAccelerationRanges &=
        validSlot &&
        !clipmapSlots[record.clipmapSlot] &&
        record.brickNodeCapacity ==
            clipmapCache.clipmapSlotNodeCapacity[record.clipmapSlot] &&
        record.brickChildCapacity ==
            clipmapCache.clipmapSlotChildCapacity[record.clipmapSlot] &&
        record.brickRootCapacity ==
            clipmapCache.clipmapSlotRootCapacity[record.clipmapSlot] &&
        record.brickMaterialCapacity ==
            clipmapCache.clipmapSlotMaterialCapacity[record.clipmapSlot] &&
        record.brickPaletteCapacity ==
            clipmapCache.clipmapSlotPaletteCapacity[record.clipmapSlot] &&
        record.brickNodeOffset + record.brickNodeCapacity <=
            clipmapChunks.bricks.nodes.size() &&
        record.brickChildOffset + record.brickChildCapacity <=
            clipmapChunks.bricks.childIndices.size() &&
        record.brickMaterialOffset % 4u == 0u &&
        record.brickMaterialCapacity % 4u == 0u &&
        record.brickMaterialOffset + record.brickMaterialCapacity <=
            clipmapChunks.bricks.materialIndices.size() &&
        record.brickPaletteOffset + record.brickPaletteCapacity <=
            clipmapChunks.bricks.palette.size() &&
        record.brickPayloadRevision != 0u &&
        record.brickNodeRevision != 0u &&
        record.firstBrickRoot + record.brickRootCapacity <=
            clipmapChunks.bricks.rootIndices.size() &&
        record.brickRootCount <= record.brickRootCapacity &&
        record.brickRootCount != 0u && record.edgeMetres > 0.0;
    if (validSlot)
      clipmapSlots[record.clipmapSlot] = true;
  }
  check(
      validChunkAccelerationRanges &&
          std::all_of(
              clipmapSlots.begin(), clipmapSlots.end(),
              [](const bool occupied) { return occupied; }),
      "every terrain page exposes one independently sized stable acceleration-structure slot");
  bool clipmapSlotsHaveMountainGrowthReserve = clipmapBuilt;
  for (const auto &entry : clipmapCache.entries)
  {
    const uint32_t slot = entry.clipmapSlot;
    clipmapSlotsHaveMountainGrowthReserve &=
        uint64_t{clipmapCache.clipmapSlotNodeCapacity[slot]} >=
            uint64_t{entry.chunk.bricks.nodes.size()} * 3u &&
        uint64_t{clipmapCache.clipmapSlotChildCapacity[slot]} >=
            uint64_t{entry.chunk.bricks.childIndices.size()} * 3u &&
        uint64_t{clipmapCache.clipmapSlotRootCapacity[slot]} >=
            uint64_t{entry.chunk.bricks.rootIndices.size()} * 3u &&
        uint64_t{clipmapCache.clipmapSlotMaterialCapacity[slot]} >=
            uint64_t{entry.chunk.bricks.materialIndices.size()} * 2u &&
        uint64_t{clipmapCache.clipmapSlotPaletteCapacity[slot]} >=
            uint64_t{entry.chunk.bricks.palette.size()} * 2u;
  }
  check(
      clipmapSlotsHaveMountainGrowthReserve,
      "toroidal terrain slots reserve mountain topology without tripling voxel payloads");
  const uint64_t clipmapLayoutRevision =
      clipmapCache.clipmapLayoutRevision;
  const auto initialClipmapRecords = clipmapChunks.chunks;
  const bool clipmapMoved = voxel::svraster::updatePlanetVoxelChunkCache(
          reader, clipmapCenter, 1, 0, 2u, clipmapSettings, clipmapCache,
          clipmapChunks, &clipmapUpdate, &error);
  if (!clipmapMoved || clipmapUpdate.reusedChunkCount != 68u ||
      clipmapUpdate.builtChunkCount != 5u ||
      clipmapCache.clipmapLayoutRevision != clipmapLayoutRevision)
    std::cout << "  clipmap move: ok=" << clipmapMoved
              << " reused=" << clipmapUpdate.reusedChunkCount
              << " built=" << clipmapUpdate.builtChunkCount
              << " layout=" << clipmapCache.clipmapLayoutRevision
              << " expectedLayout=" << clipmapLayoutRevision
              << " error=" << error << '\n';
  check(
      clipmapMoved &&
          clipmapUpdate.reusedChunkCount == 68u &&
          clipmapUpdate.builtChunkCount == 5u &&
          clipmapCache.clipmapLayoutRevision == clipmapLayoutRevision,
      "one fine-page move rewrites only the entering clipmap column");
  uint32_t replacedBrickPayloadPages = 0u;
  for (const auto &record : clipmapChunks.chunks)
  {
    const auto previous = std::find_if(
        initialClipmapRecords.begin(), initialClipmapRecords.end(),
        [&](const auto &candidate)
        {
          return candidate.clipmapSlot == record.clipmapSlot;
        });
    if (previous != initialClipmapRecords.end() &&
        previous->brickPayloadRevision != record.brickPayloadRevision)
      ++replacedBrickPayloadPages;
  }
  check(
      replacedBrickPayloadPages == 5u,
      "brick page revisions identify only the replaced fine-page column");
  const bool clipmapMiddleMoved = voxel::svraster::updatePlanetVoxelChunkCache(
      reader, clipmapCenter, 2, 0, 2u, clipmapSettings, clipmapCache,
      clipmapChunks, &clipmapUpdate, &error);
  check(
      clipmapMiddleMoved && clipmapUpdate.builtChunkCount > 5u &&
          clipmapUpdate.reusedChunkCount + clipmapUpdate.builtChunkCount ==
              73u &&
          clipmapCache.clipmapLayoutRevision == clipmapLayoutRevision,
      "crossing a coarse boundary updates its ring without changing slots");
  const uint32_t teleportSlotCount = clipmapCache.clipmapSlotCount;
  const uint64_t teleportLayoutRevision = clipmapCache.clipmapLayoutRevision;
  const auto teleportNodeCapacities = clipmapCache.clipmapSlotNodeCapacity;
  const procedural::PlanetTileAddress teleportCenter{0u, 8u, 200u, 200u};
  const bool clipmapTeleported = voxel::svraster::updatePlanetVoxelChunkCache(
      reader, teleportCenter, 0, 0, 2u, clipmapSettings, clipmapCache,
      clipmapChunks, &clipmapUpdate, &error);
  check(
      clipmapTeleported && clipmapUpdate.reusedChunkCount == 0u &&
          clipmapCache.clipmapSlotCount == teleportSlotCount &&
          clipmapCache.clipmapSlotNodeCapacity == teleportNodeCapacities &&
          clipmapCache.clipmapLayoutRevision == teleportLayoutRevision,
      "a distant anchor replaces logical pages while reusing fixed toroidal slots");

  const size_t residentPagesBeforeCancellation = clipmapCache.entries.size();
  std::atomic_bool cancelledTerrainWork{true};
  voxel::svraster::PlanetVoxelChunkSet cancelledChunks{};
  check(
      !voxel::svraster::updatePlanetVoxelChunkCache(
          reader, clipmapCenter, 0, 0, 2u, clipmapSettings, clipmapCache,
          cancelledChunks, nullptr, &error, {}, &cancelledTerrainWork) &&
          error == "Terrain work cancelled" &&
          cancelledChunks.bricks.empty() &&
          clipmapCache.entries.size() == residentPagesBeforeCancellation,
      "cancellation rejects publication and preserves the resident cache");

  const auto fixedContractAnchor = clipmapCache.anchorTile;
  const uint64_t fixedContractRevision = clipmapCache.clipmapLayoutRevision;
  std::fill(
      clipmapCache.clipmapSlotNodeCapacity.begin(),
      clipmapCache.clipmapSlotNodeCapacity.end(), 1u);
  check(
      !voxel::svraster::updatePlanetVoxelChunkCache(
          reader, clipmapCenter, 0, 0, 2u, clipmapSettings, clipmapCache,
          cancelledChunks, nullptr, &error, {}, nullptr, false) &&
          error.rfind("Planet terrain slot ", 0u) == 0u &&
          error.find("node entries but its fixed capacity is 1") !=
              std::string::npos &&
          clipmapCache.anchorTile == fixedContractAnchor &&
          clipmapCache.clipmapLayoutRevision == fixedContractRevision,
      "a navigation update rejects slot growth before publishing its anchor");

  voxel::svraster::PlanetVoxelChunkCache cache{};
  settings.workerCount = 3u;
  cache.transientPages =
      std::make_shared<voxel::svraster::TransientSparseVoxelPageStore>();
  voxel::svraster::TransientSparseVoxelPageStoreSettings transientSettings{};
  transientSettings.maximumMemoryBytes = 1u;
  transientSettings.maximumDiskBytes = 64u * 1024u * 1024u;
  check(
      cache.transientPages->initialize(transientSettings, &error),
      "a disposable procedural page store is initialized");
  const std::filesystem::path transientDirectory =
      cache.transientPages->spillDirectory();
  voxel::svraster::PlanetVoxelChunkCacheUpdate cacheUpdate{};
  voxel::svraster::PlanetVoxelChunkSet cachedGrid{};
  check(
      voxel::svraster::updatePlanetVoxelChunkCache(
          reader, gridCenter, 0, 0, 1u, settings, cache, cachedGrid,
          &cacheUpdate, &error),
      "a reusable terrain cache builds its initial resident window");
  check(
      cacheUpdate.builtChunkCount == 9u &&
          cacheUpdate.reusedChunkCount == 0u &&
          cacheUpdate.evictedChunkCount == 0u &&
          cacheUpdate.workerCount == 3u,
      "the initial cache update voxelizes new pages with a bounded workgroup");
  check(
      voxel::svraster::updatePlanetVoxelChunkCache(
          reader, gridCenter, -1, 0, 1u, settings, cache, cachedGrid,
          &cacheUpdate, &error),
      "moving one tile republishes the resident terrain window");
  check(
      cacheUpdate.builtChunkCount == 3u &&
          cacheUpdate.reusedChunkCount == 6u &&
          cacheUpdate.evictedChunkCount == 3u &&
          cacheUpdate.spilledChunkCount == 3u,
      "a one-tile move reuses six chunks and spills the trailing row");
  check(
      cachedGrid.minimumWorldMetres[0] == -2500.0 &&
          cachedGrid.maximumWorldMetres[0] == 500.0,
      "cached chunks retain a stable anchor-relative world frame");
  check(
      voxel::svraster::updatePlanetVoxelChunkCache(
          reader, gridCenter, 0, 0, 1u, settings, cache, cachedGrid,
          &cacheUpdate, &error),
      "returning to a previous window reloads procedural pages");
  check(
      cacheUpdate.reusedChunkCount == 6u &&
          cacheUpdate.restoredDiskChunkCount == 3u &&
          cacheUpdate.restoredMemoryChunkCount == 0u &&
          cacheUpdate.builtChunkCount == 0u &&
          cacheUpdate.evictedChunkCount == 3u,
      "disk-spilled pages avoid procedural regeneration");
  check(
      cache.transientPages->statistics().diskHits >= 3u,
      "the transient store reports disk-backed page hits");

  voxel::svraster::PlanetVoxelChunkCache failedCache{};
  voxel::svraster::PlanetVoxelChunkSet failedGrid{};
  voxel::svraster::PlanetVoxelChunkSettings failingSettings = settings;
  failingSettings.horizontalVoxelCount = 16u;
  failingSettings.maximumBricks = 1u;
  check(
      !voxel::svraster::updatePlanetVoxelChunkCache(
          reader, gridCenter, 0, 0, 1u, failingSettings, failedCache,
          failedGrid, nullptr, &error) && failedCache.entries.empty(),
      "a workgroup page failure leaves the resident cache unpublished");

  procedural::PlanetTileAddress acrossFace{};
  check(
      voxel::svraster::planetVoxelTileAtOffset(
          {0u, 2u, 3u, 1u}, 1, 0, acrossFace) &&
          acrossFace.face != 0u && acrossFace.level == 2u,
      "logical cache offsets resolve across cube-face boundaries");

  std::error_code filesystemError;
  std::filesystem::remove(path, filesystemError);
  std::filesystem::remove(transitionPath, filesystemError);
  std::filesystem::remove(flatSeamPath, filesystemError);
  std::filesystem::remove(fallingSeamPath, filesystemError);
  cache.transientPages.reset();
  check(
      !std::filesystem::exists(transientDirectory),
      "the transient page directory is removed without an explicit save");
  return failures == 0 ? 0 : 1;
}
