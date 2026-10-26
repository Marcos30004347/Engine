#include "voxel/material/VoxelMaterial.hpp"
#include "voxel/material/VoxelMaterialAsset.hpp"
#include "voxel/material/VoxelMaterialFieldAtlas.hpp"
#include "voxel/material/VoxelBuiltInShading.hpp"
#include "voxel/material/VoxelTerrainMaterialPresets.hpp"
#include "voxel/material/VoxelMaterialContext.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelGeometryFieldCache.hpp"
#include "voxel/svraster/SparseVoxelTree64Material.hpp"
#include "voxel/svraster/PlanetVoxelChunk.hpp"

#include "procedural/world/MapExport.hpp"
#include "procedural/world/MapExportSampler.hpp"
#include "procedural/world/WorldBrief.hpp"

#undef NDEBUG
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{

using namespace voxel::material;
using namespace voxel::svraster;

void testContinuousLandformHeight()
{
  using namespace procedural::world;
  MapExportBundle bundle{};
  bundle.seed = 7u;
  bundle.bundleSignature = 0x20c7d1b59d4d7351ull;
  bundle.extentMetres = 4.0;
  bundle.resolution = 4u;
  bundle.pageSizePixels = 2u;
  bundle.gutterPixels = 1u;
  bundle.maximumPageLevel = 1u;
  MapExportField field{};
  field.name = "landform_id";
  field.available = true;
  field.format = MapExportFormat::R8Uint;
  field.filter = MapExportFilter::Nearest;
  for (uint32_t y = 0u; y < 2u; ++y)
    for (uint32_t x = 0u; x < 2u; ++x) {
      MapExportPage page{};
      page.address = {1u, x, y};
      page.width = page.height = 4u;
      for (int z = -1; z <= 2; ++z)
        for (int xx = -1; xx <= 2; ++xx)
          page.data.push_back(static_cast<uint8_t>(static_cast<int>(x)*2+xx < 2 ? CellType::Cliff : CellType::Hill));
      field.pages.push_back(std::move(page));
    }
  bundle.fields.push_back(std::move(field));
  MapExportFieldCursor cursor{};
  const auto height = [&](double x) {
    MapExportCategorySample mixture{};
    std::string error;
    assert(sampleMapExportCategoryAtWorld(bundle, "landform_id", cursor, {x, 2.0}, 1u, mixture, &error));
    double sum = 0.0, result = 0.0;
    for (uint32_t i = 0u; i < mixture.count; ++i) {
      assert(mixture.ids[i] == static_cast<uint8_t>(CellType::Cliff) || mixture.ids[i] == static_cast<uint8_t>(CellType::Hill));
      sum += mixture.weights[i];
      result += mixture.weights[i] * proceduralWorldMapTerrainDetail(
          {79375.0 + x - 2.0, 67333.33}, bundle, 128.0, 0.31, static_cast<CellType>(mixture.ids[i]), 1.0);
    }
    assert(std::abs(sum-1.0) < 1e-12);
    return result;
  };
  // This crosses BOTH a map page seam and the old nearest-label boundary.
  assert(std::abs(height(2.0-1e-4)-height(2.0+1e-4)) < 0.01);
  // Stable results after cursor reuse through the other page.
  const double first = height(1.75);
  height(2.75);
  assert(height(1.75) == first);
  for (const auto landform : {CellType::Cliff, CellType::Plateau}) {
    double previous = proceduralWorldMapTerrainDetail({0.0, 67333.33}, bundle, 600.0, 0.5, landform, 1.0);
    for (uint32_t i = 1u; i <= 32768u; ++i) {
      const double current = proceduralWorldMapTerrainDetail({i*0.125, 67333.33}, bundle, 600.0, 0.5, landform, 1.0);
      assert(std::abs(current-previous) < 0.25);
      previous = current;
    }
  }
}

void testWorldMapBiomeMixtureAndLandformDetail()
{
  procedural::world::MapExportBiomeSample mixture{};
  mixture.ids = {
    static_cast<uint8_t>(procedural::world::Biome::Grassland),
    static_cast<uint8_t>(procedural::world::Biome::Desert), 0u, 0u};
  mixture.weights = {0.25, 0.75, 0.0, 0.0};
  mixture.count = 2u;
  assert(mixture.selectBiome(0.0) == mixture.ids[0]);
  assert(mixture.selectBiome(0.249999) == mixture.ids[0]);
  assert(mixture.selectBiome(0.25) == mixture.ids[1]);
  assert(mixture.selectBiome(1.0) == mixture.ids[1]);

  procedural::world::MapExportBundle bundle{};
  bundle.seed = 7u;
  bundle.bundleSignature = 0x9e3779b97f4a7c15ull;
  uint32_t grassColumns = 0u;
  constexpr uint32_t side = 128u;
  for (uint32_t z = 0u; z < side; ++z)
    for (uint32_t x = 0u; x < side; ++x)
    {
      const procedural::Point2 position{
        static_cast<double>(x) + 0.5, static_cast<double>(z) + 0.5};
      const uint8_t first = worldMapBiomeForColumn(
          mixture, position, bundle, 1.0);
      const uint8_t repeated = worldMapBiomeForColumn(
          mixture, position, bundle, 1.0);
      assert(first == repeated);
      grassColumns += first == mixture.ids[0] ? 1u : 0u;
    }
  const double grassFraction =
      static_cast<double>(grassColumns) / (side * side);
  assert(grassFraction > 0.22 && grassFraction < 0.28);

  const auto detailStatistics = [&](const double relief,
                                    const procedural::world::CellType landform)
  {
    double sum = 0.0;
    double squared = 0.0;
    double first = 0.0;
    for (uint32_t sample = 0u; sample < 1024u; ++sample)
    {
      const procedural::Point2 position{
        static_cast<double>((sample * 137u) % 8192u),
        static_cast<double>((sample * 313u) % 8192u)};
      const double value = proceduralWorldMapTerrainDetail(
          position, bundle, relief, 0.5, landform, 1.0);
      if (sample == 0u)
        first = value;
      sum += value;
      squared += value * value;
    }
    return std::array<double, 3>{
      first, sum / 1024.0, std::sqrt(squared / 1024.0)};
  };

  const auto lowRelief = detailStatistics(
      30.0, procedural::world::CellType::Hill);
  const auto highRelief = detailStatistics(
      550.0, procedural::world::CellType::Hill);
  const double responseRatio = highRelief[2] / lowRelief[2];
  assert(responseRatio > 18.0 && responseRatio < 18.7);
  assert(proceduralWorldMapTerrainDetail(
      {123.0, 456.0}, bundle, 550.0, 0.5,
      procedural::world::CellType::Hill, 0.0) == 0.0);

  const auto mountain = detailStatistics(
      300.0, procedural::world::CellType::Mountain);
  const auto plateau = detailStatistics(
      300.0, procedural::world::CellType::Plateau);
  const auto plain = detailStatistics(
      300.0, procedural::world::CellType::Plain);
  const auto valley = detailStatistics(
      300.0, procedural::world::CellType::Valley);
  const auto cliff = detailStatistics(
      300.0, procedural::world::CellType::Cliff);
  const auto canyon = detailStatistics(
      300.0, procedural::world::CellType::Canyon);
  assert(mountain[2] > plain[2]);
  assert(valley[1] < plain[1]);
  assert(mountain[0] != plateau[0] && plateau[0] != plain[0] &&
      plain[0] != valley[0] && valley[0] != cliff[0] &&
      cliff[0] != canyon[0]);

  const procedural::Point2 canyonPosition{123.0, 456.0};
  const double canyonChannel = proceduralWorldMapTerrainDetail(
      canyonPosition, bundle, 300.0, 0.5,
      procedural::world::CellType::Canyon, 1.0, 0.0);
  const double canyonBank = proceduralWorldMapTerrainDetail(
      canyonPosition, bundle, 300.0, 0.5,
      procedural::world::CellType::Canyon, 1.0, 5000.0);
  assert(canyonChannel < canyonBank - 40.0);

  const double plainUnterraced = proceduralWorldMapTerrainHeight(
      canyonPosition, bundle, 337.0, 300.0, 0.5,
      procedural::world::CellType::Plain, 1.0, 0.0, 24.0, 0.16);
  const double plainTerraced = proceduralWorldMapTerrainHeight(
      canyonPosition, bundle, 337.0, 300.0, 0.5,
      procedural::world::CellType::Plain, 1.0, 1.0, 24.0, 0.16);
  assert(plainTerraced == plainUnterraced);

  uint32_t levelPlateauSamples = 0u;
  for (uint32_t sample = 0u; sample < 1024u; ++sample)
  {
    const procedural::Point2 position{
      static_cast<double>((sample * 137u) % 8192u),
      static_cast<double>((sample * 313u) % 8192u)};
    const double height = proceduralWorldMapTerrainHeight(
        position, bundle, 337.0, 300.0, 0.5,
        procedural::world::CellType::Plateau, 1.0, 1.0, 24.0, 0.16);
    const double nearestBand = std::round(height / 24.0) * 24.0;
    levelPlateauSamples += std::abs(height - nearestBand) < 2.0 ? 1u : 0u;
  }
  assert(levelPlateauSamples > 500u);

  const double infrastructureHeight = proceduralWorldMapTerrainHeight(
      canyonPosition, bundle, 337.0, 300.0, 0.5,
      procedural::world::CellType::Plateau, 1.0, 1.0, 24.0, 0.16,
      5000.0, 1.0);
  assert(infrastructureHeight == 337.0);

  double openDeviation = 0.0;
  double riverDeviation = 0.0;
  for (uint32_t sample = 0u; sample < 256u; ++sample)
  {
    const procedural::Point2 position{
      static_cast<double>((sample * 137u) % 8192u),
      static_cast<double>((sample * 313u) % 8192u)};
    openDeviation += std::abs(proceduralWorldMapTerrainHeight(
        position, bundle, 337.0, 300.0, 0.5,
        procedural::world::CellType::Hill, 1.0, 1.0, 24.0, 0.16,
        5000.0, 0.0) - 337.0);
    riverDeviation += std::abs(proceduralWorldMapTerrainHeight(
        position, bundle, 337.0, 300.0, 0.5,
        procedural::world::CellType::Hill, 1.0, 1.0, 24.0, 0.16,
        0.0, 0.0) - 337.0);
  }
  assert(riverDeviation < openDeviation * 0.35);

  WorldMapTerrainSignals channelSignals{};
  channelSignals.riverDischargeCubicMetresPerSecond = 100.0;
  channelSignals.erosionFlow = 0.8;
  const double incisedChannel = proceduralWorldMapTerrainHeight(
      canyonPosition, bundle, 337.0, 300.0, 0.5,
      procedural::world::CellType::Plain, 0.0, 0.0, 24.0, 0.16,
      0.0, 0.0, channelSignals);
  const double uncutBank = proceduralWorldMapTerrainHeight(
      canyonPosition, bundle, 337.0, 300.0, 0.5,
      procedural::world::CellType::Plain, 0.0, 0.0, 24.0, 0.16,
      200.0, 0.0, channelSignals);
  assert(incisedChannel < 329.0);
  assert(uncutBank == 337.0);

  WorldMapTerrainSignals baseSignals{};
  baseSignals.hasBaseElevation = true;
  baseSignals.baseElevationMetres = 300.0;
  const double conditionedLow = proceduralWorldMapTerrainHeight(
      canyonPosition, bundle, 337.0, 300.0, 0.5,
      procedural::world::CellType::Plateau, 1.0, 1.0, 24.0, 0.16,
      5000.0, 0.0, baseSignals);
  const double conditionedHigh = proceduralWorldMapTerrainHeight(
      canyonPosition, bundle, 347.0, 300.0, 0.5,
      procedural::world::CellType::Plateau, 1.0, 1.0, 24.0, 0.16,
      5000.0, 0.0, baseSignals);
  assert(std::abs((conditionedHigh - conditionedLow) - 10.0) < 1.0e-9);

  double neutralDetail = 0.0;
  double upliftDetail = 0.0;
  double erodedDetail = 0.0;
  WorldMapTerrainSignals upliftSignals{};
  upliftSignals.upliftMetresPerYear = 0.001;
  WorldMapTerrainSignals erodedSignals{};
  erodedSignals.erosionWear = 1.0;
  erodedSignals.erosionFlow = 1.0;
  erodedSignals.erosionDeposition = 1.0;
  for (uint32_t sample = 0u; sample < 256u; ++sample)
  {
    const procedural::Point2 position{
      static_cast<double>((sample * 137u) % 8192u),
      static_cast<double>((sample * 313u) % 8192u)};
    neutralDetail += std::abs(proceduralWorldMapTerrainHeight(
        position, bundle, 337.0, 300.0, 0.5,
        procedural::world::CellType::Mountain, 1.0, 0.0, 24.0, 0.16,
        5000.0, 0.0) - 337.0);
    upliftDetail += std::abs(proceduralWorldMapTerrainHeight(
        position, bundle, 337.0, 300.0, 0.5,
        procedural::world::CellType::Mountain, 1.0, 0.0, 24.0, 0.16,
        5000.0, 0.0, upliftSignals) - 337.0);
    erodedDetail += std::abs(proceduralWorldMapTerrainHeight(
        position, bundle, 337.0, 300.0, 0.5,
        procedural::world::CellType::Mountain, 1.0, 0.0, 24.0, 0.16,
        5000.0, 0.0, erodedSignals) - 337.0);
  }
  assert(upliftDetail > neutralDetail * 1.30);
  assert(erodedDetail < neutralDetail * 0.32);
}

SparseVoxelMicroMaterialGPU packedMaterial(const uint32_t seed = 17u, const SparseVoxelMicroShadingProfile shadingProfile = SparseVoxelMicroShadingProfile::Linear)
{
  SparseVoxelMicroDescriptor geometry{};
  geometry.subdivision = 5u;
  geometry.maximumCarveDepth = 2u;
  geometry.faceMask = 1u << static_cast<uint32_t>(SparseVoxelMicroFace::PositiveY);
  geometry.geometryProfile = SparseVoxelMicroGeometryProfile::HashColumns;
  geometry.materialSeed = seed;
  SparseVoxelMicroShadingDescriptor shading{};
  shading.profile = shadingProfile;
  shading.variationStrength = 31u;
  shading.lodHandoff = 128u;
  SparseVoxelMicroMaterialGPU packed{};
  std::string error;
  assert(packSparseVoxelMicroMaterial(geometry, shading, packed, &error));
  return packed;
}

VoxelShadingProgram slopeDepthProgram()
{
  return {{
    {VoxelShadingOpcode::LoadNormalizedDepth, 0u},
    {VoxelShadingOpcode::LoadTerrainSlope, 1u},
    {VoxelShadingOpcode::Multiply, 0u, 0u, 1u},
    {VoxelShadingOpcode::OutputLayerWeight, 0u, 0u},
  }};
}

void testShadingIRVerificationAndEvaluation()
{
  const VoxelShadingProgram program = slopeDepthProgram();
  std::string error;
  assert(verifyVoxelShadingProgram(program, &error));
  std::vector<VoxelShadingInstructionGPU> compiled;
  assert(compileVoxelShadingProgram(program, compiled, &error));
  assert(compiled.size() == program.instructions.size());
  assert((compiled[2].opcodeOperands & 0xffu) ==
      static_cast<uint32_t>(VoxelShadingOpcode::Multiply));
  assert(((compiled[2].opcodeOperands >> 8u) & 0xffu) == 0u);
  assert(((compiled[2].opcodeOperands >> 16u) & 0xffu) == 0u);
  assert((compiled[2].opcodeOperands >> 24u) == 1u);

  float weight = -1.0f;
  assert(evaluateVoxelShadingProgram(
      program, {0.75f, 0.25f, 1.0f}, weight, &error));
  assert(std::abs(weight - 0.75f) < 1.0e-6f);
  assert(evaluateVoxelShadingProgram(
      program, {0.75f, 0.25f, 0.0f}, weight, &error));
  assert(weight == 0.0f);
  assert(evaluateVoxelShadingProgram(
      program, {0.75f, 0.25f, 0.5f}, weight, &error));
  assert(std::abs(weight - 0.375f) < 1.0e-6f);

  const VoxelShadingProgram colourProgram{{
    {VoxelShadingOpcode::LoadNormalizedDepth, 0u},
    {VoxelShadingOpcode::LoadStableRandom, 1u},
    {VoxelShadingOpcode::ScaleBias, 2u, 1u, 0u, 0.2f, 0.9f},
    {VoxelShadingOpcode::LoadConstant, 3u, 0u, 0u, -0.25f},
    {VoxelShadingOpcode::LoadConstant, 4u, 0u, 0u, 0.35f},
    {VoxelShadingOpcode::LoadConstant, 5u, 0u, 0u, 1.5f},
    {VoxelShadingOpcode::OutputLayerWeight, 0u, 0u},
    {VoxelShadingOpcode::OutputHueVariation, 0u, 3u},
    {VoxelShadingOpcode::OutputBrightness, 0u, 2u},
    {VoxelShadingOpcode::OutputRoughness, 0u, 4u},
    {VoxelShadingOpcode::OutputMetallic, 0u, 5u},
  }};
  VoxelShadingResult result{};
  assert(evaluateVoxelShadingProgram(
      colourProgram, {0.6f, 0.75f, 0.0f}, result, &error));
  assert(std::abs(result.layerWeight - 0.6f) < 1.0e-6f);
  assert(std::abs(result.hueVariation + 0.25f) < 1.0e-6f);
  assert(std::abs(result.brightness - 1.05f) < 1.0e-6f);
  assert(std::abs(result.roughness - 0.35f) < 1.0e-6f);
  assert(result.metallic == 1.0f);

  const VoxelShadingProgram grassSoilProgram{{
    {VoxelShadingOpcode::LoadSideOrRiser, 0u},
    {VoxelShadingOpcode::LoadNormalizedDepth, 1u},
    {VoxelShadingOpcode::Smoothstep, 1u, 1u, 0u, 0.2f, 0.6f},
    {VoxelShadingOpcode::LoadTerrainSlope, 2u},
    {VoxelShadingOpcode::Smoothstep, 2u, 2u, 0u, 0.2f, 0.4f},
    {VoxelShadingOpcode::Maximum, 0u, 0u, 1u},
    {VoxelShadingOpcode::Maximum, 0u, 0u, 2u},
    {VoxelShadingOpcode::ScaleBias, 3u, 0u, 0u, 0.2f, 0.7f},
    {VoxelShadingOpcode::OutputLayerWeight, 0u, 0u},
    {VoxelShadingOpcode::OutputRoughness, 0u, 3u},
  }};
  assert(evaluateVoxelShadingProgram(
      grassSoilProgram, {0.1f, 0.0f, 0.0f, false}, result, &error));
  assert(result.layerWeight == 0.0f);
  assert(std::abs(result.roughness - 0.7f) < 1.0e-6f);
  assert(result.metallic < 0.0f);
  assert(evaluateVoxelShadingProgram(
      grassSoilProgram, {0.1f, 0.0f, 0.0f, true}, result, &error));
  assert(result.layerWeight == 1.0f);
  assert(std::abs(result.roughness - 0.9f) < 1.0e-6f);
  assert(evaluateVoxelShadingProgram(
      grassSoilProgram, {0.1f, 0.0f, 1.0f, false}, result, &error));
  assert(result.layerWeight == 1.0f);
  assert(evaluateVoxelShadingProgram(
      grassSoilProgram, {0.1f, 0.0f, 0.3f, false}, result, &error));
  assert(std::abs(result.layerWeight - 0.5f) < 1.0e-6f);
  assert(std::abs(result.roughness - 0.8f) < 1.0e-6f);
  assert(evaluateVoxelShadingProgram(
      grassSoilProgram, {0.4f, 0.0f, 0.0f, false}, result, &error));
  assert(std::abs(result.layerWeight - 0.5f) < 1.0e-6f);

  VoxelShadingProgram invalid{{
    {VoxelShadingOpcode::Add, 0u, 1u, 2u},
    {VoxelShadingOpcode::OutputLayerWeight, 0u, 0u},
  }};
  assert(!verifyVoxelShadingProgram(invalid, &error));
  invalid.instructions = {
    {VoxelShadingOpcode::LoadConstant, 0u, 0u, 0u, 1.0f},
    {VoxelShadingOpcode::OutputLayerWeight, 0u, 0u},
    {VoxelShadingOpcode::Clamp01, 0u, 0u},
  };
  assert(!verifyVoxelShadingProgram(invalid, &error));
  invalid.instructions = {
    {VoxelShadingOpcode::LoadConstant, 0u, 0u, 0u, 1.0f},
    {VoxelShadingOpcode::OutputLayerWeight, 0u, 0u},
    {VoxelShadingOpcode::OutputBrightness, 0u, 0u},
    {VoxelShadingOpcode::OutputBrightness, 0u, 0u},
  };
  assert(!verifyVoxelShadingProgram(invalid, &error));
}

void testStableShadingProgramSlots()
{
  VoxelMaterialManager manager;
  std::string error;
  VoxelMaterialDefinition definition{
    "terrain/programmed", "Programmed", packedMaterial(),
    {static_cast<uint32_t>(VoxelMaterialContextField::TerrainSlope)},
    VoxelMaterialSlotPolicy::UniqueEditable, slopeDepthProgram(),
  };
  VoxelMaterialHandle handle = 0u;
  assert(manager.registerMaterial(definition, handle, &error));
  assert(handle == 1u);
  assert(manager.shadingPrograms().size() == manager.descriptors().size());
  assert(manager.shadingInstructions().size() ==
      manager.descriptors().size() * kMaximumVoxelShadingInstructionCount);
  assert(manager.shadingPrograms()[handle].instructionOffset ==
      handle * kMaximumVoxelShadingInstructionCount);
  assert(manager.shadingPrograms()[handle].instructionCount == 4u);

  VoxelMaterialDefinition missingContext = definition;
  missingContext.assetId = "terrain/programmed-missing-context";
  missingContext.contextRequirements = {};
  VoxelMaterialHandle rejected = 0u;
  assert(!manager.registerMaterial(missingContext, rejected, &error));
  VoxelMaterialChange changes = VoxelMaterialChange::None;
  assert(!manager.updateMaterial(
      handle, manager.descriptor(handle), {}, changes, &error));

  const uint64_t revision = manager.shadingRevision();
  assert(manager.updateShadingProgram(handle, {}, changes, &error));
  assert(changes == VoxelMaterialChange::Shading);
  assert(manager.shadingRevision() == revision + 1u);
  assert(manager.shadingPrograms()[handle].instructionCount == 0u);
  assert(manager.shadingPrograms()[handle].instructionOffset ==
      handle * kMaximumVoxelShadingInstructionCount);
  assert(manager.updateShadingProgram(handle, {}, changes, &error));
  assert(changes == VoxelMaterialChange::None);
}

void testStableEditableSlots()
{
  VoxelMaterialManager manager;
  assert(manager.slotCount() == 1u);
  assert(manager.descriptors().size() == 1u);
  assert(manager.descriptor(kInvalidVoxelMaterialHandle).geometry == 0u);
  assert(manager.descriptor(kInvalidVoxelMaterialHandle).shading == 0u);

  const SparseVoxelMicroMaterialGPU packed = packedMaterial();
  VoxelMaterialDefinition grass{
    "terrain/grass",
    "Grass",
    packed,
    {static_cast<uint32_t>(VoxelMaterialContextField::BaseSurface) | static_cast<uint32_t>(VoxelMaterialContextField::TerrainSlope)},
    VoxelMaterialSlotPolicy::UniqueEditable,
  };
  VoxelMaterialHandle grassHandle = 0u;
  std::string error;
  assert(manager.registerMaterial(grass, grassHandle, &error));
  assert(grassHandle == 1u);
  assert(manager.handleForAsset("terrain/grass") == grassHandle);
  assert(manager.handleForSourceMaterial(3u) == kInvalidVoxelMaterialHandle);
  assert(manager.bindSourceMaterial(3u, grassHandle, &error));
  assert(manager.bindSourceMaterial(3u, grassHandle, &error));
  assert(manager.handleForSourceMaterial(3u) == grassHandle);
  assert(manager.descriptor(grassHandle).geometry == packed.geometry);
  assert(manager.descriptor(grassHandle).shading == packed.shading);
  assert(manager.slot(grassHandle).geometryRevision == 1u);
  assert(manager.slot(grassHandle).shadingRevision == 1u);
  assert(manager.slot(grassHandle).contextRevision == 1u);
  assert(!manager.slot(grassHandle).immutable);

  // Registration is idempotent for one asset identity and exact definition.
  VoxelMaterialHandle repeated = 0u;
  assert(manager.registerMaterial(grass, repeated, &error));
  assert(repeated == grassHandle);
  assert(manager.slotCount() == 2u);

  // Editable assets intentionally retain independent handles even when their
  // current descriptor bytes match.
  grass.assetId = "terrain/grass-variant";
  grass.name = "Grass variant";
  VoxelMaterialHandle variant = 0u;
  assert(manager.registerMaterial(grass, variant, &error));
  assert(variant != grassHandle);
  assert(manager.slotCount() == 3u);
  assert(!manager.bindSourceMaterial(3u, variant, &error));

  grass.assetId = "terrain/grass";
  grass.descriptor = packedMaterial(91u);
  assert(!manager.registerMaterial(grass, repeated, &error));
  assert(!error.empty());
}

void testImmutableSharing()
{
  VoxelMaterialManager manager;
  VoxelMaterialDefinition first{
    "import/stone-a",
    "Stone A",
    packedMaterial(44u),
    {},
    VoxelMaterialSlotPolicy::SharedImmutable,
  };
  VoxelMaterialDefinition second = first;
  second.assetId = "import/stone-b";
  second.name = "Stone B";
  VoxelMaterialHandle firstHandle = 0u;
  VoxelMaterialHandle secondHandle = 0u;
  std::string error;
  assert(manager.registerMaterial(first, firstHandle, &error));
  assert(manager.registerMaterial(second, secondHandle, &error));
  assert(firstHandle == secondHandle);
  assert(manager.slotCount() == 2u);
  assert(manager.handleForAsset(first.assetId) == firstHandle);
  assert(manager.handleForAsset(second.assetId) == firstHandle);
  assert(manager.slot(firstHandle).immutable);

  VoxelMaterialChange changes = VoxelMaterialChange::None;
  assert(!manager.updateShading(firstHandle, SparseVoxelMicroShadingDescriptor{}, changes, &error));
  assert(changes == VoxelMaterialChange::None);
}

void testIndependentRevisionsAndInvalidation()
{
  VoxelMaterialManager manager;
  VoxelMaterialDefinition definition{
    "terrain/editable",
    "Editable terrain",
    packedMaterial(),
    {static_cast<uint32_t>(VoxelMaterialContextField::BaseSurface)},
    VoxelMaterialSlotPolicy::UniqueEditable,
  };
  VoxelMaterialHandle handle = 0u;
  std::string error;
  assert(manager.registerMaterial(definition, handle, &error));
  const uint64_t initialContent = manager.contentRevision();
  const uint64_t initialGeometry = manager.geometryRevision();
  const uint64_t initialShading = manager.shadingRevision();
  const uint64_t initialContext = manager.contextRevision();

  SparseVoxelMicroShadingDescriptor shading = unpackSparseVoxelMicroShading(manager.descriptor(handle));
  shading.profile = SparseVoxelMicroShadingProfile::ExposedSides;
  shading.variationStrength = 83u;
  VoxelMaterialChange changes = VoxelMaterialChange::None;
  assert(manager.updateShading(handle, shading, changes, &error));
  assert(changes == VoxelMaterialChange::Shading);
  assert(manager.slot(handle).geometryRevision == 1u);
  assert(manager.slot(handle).shadingRevision == 2u);
  assert(manager.geometryRevision() == initialGeometry);
  assert(manager.shadingRevision() == initialShading + 1u);
  assert(manager.contextRevision() == initialContext);
  assert(manager.contentRevision() == initialContent + 1u);
  assert(!voxelMaterialChangeRequiresPageRebuild(changes));

  SparseVoxelMicroDescriptor geometry = unpackSparseVoxelMicroGeometry(manager.descriptor(handle));
  geometry.maximumCarveDepth = 3u;
  assert(manager.updateGeometry(handle, geometry, changes, &error));
  assert(changes == VoxelMaterialChange::Geometry);
  assert(manager.slot(handle).geometryRevision == 2u);
  assert(manager.slot(handle).shadingRevision == 2u);
  assert(manager.geometryRevision() == initialGeometry + 1u);
  assert(manager.shadingRevision() == initialShading + 1u);
  assert(!voxelMaterialChangeRequiresPageRebuild(changes));

  const VoxelMaterialContextRequirements expanded{
    static_cast<uint32_t>(VoxelMaterialContextField::BaseSurface) | static_cast<uint32_t>(VoxelMaterialContextField::TerrainSlope) | static_cast<uint32_t>(VoxelMaterialContextField::DepthBelowSurface),
  };
  assert(manager.updateMaterial(handle, manager.descriptor(handle), expanded, changes, &error));
  assert(changes == VoxelMaterialChange::ContextLayout);
  assert(voxelMaterialChangeRequiresPageRebuild(changes));
  assert(manager.slot(handle).contextRevision == 2u);
  assert(manager.contextRevision() == initialContext + 1u);

  const uint64_t unchangedContent = manager.contentRevision();
  assert(manager.updateMaterial(handle, manager.descriptor(handle), expanded, changes, &error));
  assert(changes == VoxelMaterialChange::None);
  assert(manager.contentRevision() == unchangedContent);
}

void testLegacyLibraryAdapterAndValidation()
{
  SparseVoxelMicroMaterialLibrary legacy;
  const SparseVoxelMicroMaterialGPU packed = packedMaterial(73u);
  std::string error;
  assert(legacy.setMaterial(29u, packed, &error));

  VoxelMaterialManager manager;
  VoxelMaterialHandle handle = 0u;
  assert(manager.importLegacyMaterial("legacy/material-29", "Legacy 29", 29u, legacy, handle, VoxelMaterialSlotPolicy::UniqueEditable, {static_cast<uint32_t>(VoxelMaterialContextField::BaseSurface)}, &error));
  assert(handle != kInvalidVoxelMaterialHandle);
  assert(manager.handleForSourceMaterial(29u) == handle);
  assert(manager.descriptor(handle).geometry == packed.geometry);
  assert(manager.descriptor(handle).shading == packed.shading);

  VoxelMaterialDefinition invalid{
    "invalid/descriptor",
    "Invalid",
    SparseVoxelMicroMaterialGPU{0u, 1u},
    {},
    VoxelMaterialSlotPolicy::UniqueEditable,
  };
  assert(!manager.registerMaterial(invalid, handle, &error));
  invalid.assetId = "invalid/context";
  invalid.descriptor = {};
  invalid.contextRequirements.fields = 1u << 31u;
  assert(!manager.registerMaterial(invalid, handle, &error));
  invalid.assetId.clear();
  invalid.contextRequirements = {};
  assert(!manager.registerMaterial(invalid, handle, &error));
}

void testBrickPaletteUsesStableMaterialHandles()
{
  VoxelMaterialManager manager;
  std::string error;
  const VoxelMaterialContextRequirements requirements{static_cast<uint32_t>(VoxelMaterialContextField::BaseSurface) | static_cast<uint32_t>(VoxelMaterialContextField::TerrainSlope)};

  VoxelMaterialHandle grass = 0u;
  VoxelMaterialHandle dirt = 0u;
  assert(manager.registerMaterial({"terrain/grass", "Grass", packedMaterial(101u), requirements, VoxelMaterialSlotPolicy::UniqueEditable}, grass, &error));
  assert(manager.registerMaterial({"terrain/dirt", "Dirt", packedMaterial(202u), requirements, VoxelMaterialSlotPolicy::UniqueEditable}, dirt, &error));
  assert(manager.bindSourceMaterial(10u, grass, &error));
  assert(manager.bindSourceMaterial(11u, dirt, &error));

  std::vector<SparseVoxelCenterLengthGPU> leaves(2u);
  leaves[0].center = {0.5f, 0.5f, 0.5f};
  leaves[1].center = {1.5f, 0.5f, 0.5f};
  leaves[0].length = leaves[1].length = 1.0f;
  std::vector<SparseVoxelMaterialGPU> materials(2u);
  materials[0].albedoRGBA8 = materials[1].albedoRGBA8 = 0xff446688u;
  materials[0].roughnessMetallicUNorm8 = materials[1].roughnessMetallicUNorm8 = 0x00004080u;
  materials[0].materialId = 10u;
  materials[1].materialId = 11u;
  materials[1].flags = 0xa0u;

  SparseVoxelBrickBuildSettings settings{};
  settings.voxelSize = 1.0f;
  settings.paletteSize = 4u;
  settings.voxelMaterials = &manager;
  SparseVoxelBrickHierarchy hierarchy;
  assert(buildSparseVoxelBrickHierarchy(leaves, materials, {0u, 1u}, settings, hierarchy, &error));

  assert(std::find(hierarchy.paletteDescriptorIds.begin(), hierarchy.paletteDescriptorIds.end(), grass) != hierarchy.paletteDescriptorIds.end());
  assert(std::find(hierarchy.paletteDescriptorIds.begin(), hierarchy.paletteDescriptorIds.end(), dirt) != hierarchy.paletteDescriptorIds.end());
  assert(std::find(hierarchy.microSurfaceData.begin(), hierarchy.microSurfaceData.end(), packSparseVoxelMicroContextData(0xa0u, 0u)) != hierarchy.microSurfaceData.end());
  assert(hierarchy.materialIndices.size() >= 2u);
  assert(hierarchy.materialIndices[0] != hierarchy.materialIndices[1]);
}

void testContextDoesNotSplitStableMaterialPalette()
{
  VoxelMaterialManager manager;
  std::string error;
  VoxelMaterialHandle handle = 0u;
  assert(manager.registerMaterial(
      {"terrain/shared-slope", "Shared slope", packedMaterial(303u),
       {static_cast<uint32_t>(VoxelMaterialContextField::BaseSurface) |
        static_cast<uint32_t>(VoxelMaterialContextField::TerrainSlope)},
       VoxelMaterialSlotPolicy::UniqueEditable},
      handle, &error));
  assert(manager.bindSourceMaterial(21u, handle, &error));

  std::vector<SparseVoxelCenterLengthGPU> leaves(2u);
  leaves[0].center = {0.5f, 0.5f, 0.5f};
  leaves[1].center = {1.5f, 0.5f, 0.5f};
  leaves[0].length = leaves[1].length = 1.0f;
  std::vector<SparseVoxelMaterialGPU> materials(2u);
  materials[0].albedoRGBA8 = materials[1].albedoRGBA8 = 0xff447744u;
  materials[0].roughnessMetallicUNorm8 =
      materials[1].roughnessMetallicUNorm8 = 0x000000c0u;
  materials[0].materialId = materials[1].materialId = 21u;
  materials[0].flags =
      static_cast<uint32_t>(SparseVoxelMicroContext::FlatSurface);
  const uint32_t slopeContext = sparseVoxelMicroSurfaceContext(5u, 2u);
  const uint64_t slopePatch = packSparseVoxelMicroSurfacePatch(
      {0.0, 0.25, 0.75, 1.0});
  materials[1].flags = slopeContext;
  const std::vector<uint64_t> terrainSurfacePatches{0u, slopePatch};

  SparseVoxelBrickBuildSettings settings{};
  settings.voxelSize = 1.0f;
  settings.paletteSize = 4u;
  settings.voxelMaterials = &manager;
  settings.terrainSurfacePatches = &terrainSurfacePatches;
  SparseVoxelBrickHierarchy hierarchy;
  assert(buildSparseVoxelBrickHierarchy(
      leaves, materials, {0u, 1u}, settings, hierarchy, &error));

  assert(hierarchy.materialIndices.size() >= 2u);
  assert(hierarchy.materialIndices[0] == hierarchy.materialIndices[1]);
  assert(std::count(
             hierarchy.paletteDescriptorIds.begin(),
             hierarchy.paletteDescriptorIds.end(), handle) == 1);
  assert(std::find(
             hierarchy.microSurfaceData.begin(),
             hierarchy.microSurfaceData.end(),
             packSparseVoxelMicroContextData(slopeContext, slopePatch)) !=
         hierarchy.microSurfaceData.end());

  const SparseVoxelMaterialSidecarMemory memory =
      sparseVoxelMaterialSidecarMemory(hierarchy);
  assert(memory.microSurfaceLogicalBytes ==
      hierarchy.microSurfaceData.size() * sizeof(uint64_t));
  assert(memory.paletteDescriptorLogicalBytes ==
      hierarchy.paletteDescriptorIds.size() * sizeof(uint16_t));
  assert(memory.logicalBytes() ==
      memory.microSurfaceLogicalBytes +
      memory.paletteDescriptorLogicalBytes);
  assert(memory.residentBytes() >= memory.logicalBytes());
}

float legacyProfileWeight(
    const SparseVoxelMicroShadingDescriptor &shading,
    const VoxelShadingInputs &inputs)
{
  const float variation = static_cast<float>(shading.variationStrength) /
      255.0f;
  switch (shading.profile)
  {
  case SparseVoxelMicroShadingProfile::Linear:
    return std::clamp(inputs.normalizedDepth, 0.0f, 1.0f);
  case SparseVoxelMicroShadingProfile::HardCrust:
  {
    const float parameter = static_cast<float>(shading.parameter0) / 255.0f;
    const float threshold = parameter > 0.0f ? parameter : 0.25f;
    return inputs.normalizedDepth > threshold ? 1.0f : 0.0f;
  }
  case SparseVoxelMicroShadingProfile::NoisyLinear:
    return std::clamp(
        inputs.normalizedDepth + (inputs.stableRandom * 2.0f - 1.0f) *
            variation,
        0.0f, 1.0f);
  case SparseVoxelMicroShadingProfile::ExposedSides:
  {
    if (inputs.sideOrRiser)
      return 1.0f;
    const float dirt = variation *
        (0.15f + inputs.stableRandom * 0.85f);
    return std::clamp(
        inputs.normalizedDepth +
            (inputs.terrainSlopeClassified ? 0.0f : dirt) *
                (1.0f - inputs.normalizedDepth),
        0.0f, 1.0f);
  }
  case SparseVoxelMicroShadingProfile::FoliageTint:
    return std::clamp(
        inputs.normalizedDepth * 0.65f + inputs.stableRandom * variation *
            0.35f,
        0.0f, 1.0f);
  }
  return 0.0f;
}

void testAllLegacyProfilesCompileToEquivalentBuiltIns()
{
  std::string error;
  for (uint32_t profile = 0u;
       profile <= static_cast<uint32_t>(
           SparseVoxelMicroShadingProfile::FoliageTint);
       ++profile)
  {
    SparseVoxelMicroShadingDescriptor shading{};
    shading.profile = static_cast<SparseVoxelMicroShadingProfile>(profile);
    shading.variationStrength = 83u;
    shading.parameter0 = 96u;
    const VoxelShadingProgram program =
        builtInVoxelShadingProgram(shading);
    assert(!program.instructions.empty());
    assert(program.instructions.size() <=
        kMaximumVoxelShadingInstructionCount);
    assert(verifyVoxelShadingProgram(program, &error));
    for (uint32_t depth = 0u; depth <= 16u; ++depth)
      for (uint32_t random = 0u; random <= 16u; ++random)
        for (uint32_t flags = 0u; flags < 4u; ++flags)
        {
          VoxelShadingInputs inputs{};
          inputs.normalizedDepth = static_cast<float>(depth) / 16.0f;
          inputs.stableRandom = static_cast<float>(random) / 16.0f;
          inputs.sideOrRiser = (flags & 1u) != 0u;
          inputs.terrainSlopeClassified = (flags & 2u) != 0u;
          VoxelShadingResult result{};
          assert(evaluateVoxelShadingProgram(
              program, inputs, result, &error));
          assert(std::abs(
              result.layerWeight - legacyProfileWeight(shading, inputs)) <
              2.0e-6f);
        }
  }
}

void testFirstClassAssetsAndAuthoredTerrainDefaults()
{
  const auto presets = defaultTerrainVoxelMaterialPresets(false);
  const auto volumePresets = defaultTerrainVoxelMaterialPresets(true);
  assert(presets.size() == kDefaultTerrainVoxelMaterialCount);
  assert(defaultTerrainVoxelMaterialNames().size() == presets.size());
  assert(presets[3u].geometry.geometryProfile ==
      SparseVoxelMicroGeometryProfile::Staircase);
  for (size_t index = 0u; index < presets.size(); ++index)
  {
    const bool water = index == 0u || index == 1u || index == 6u;
    assert(water || presets[index].geometry.geometryProfile ==
        SparseVoxelMicroGeometryProfile::Staircase);
  }
  assert(presets[3u].programmableGrassSoil);
  assert(volumePresets[3u].geometry.geometryProfile ==
      SparseVoxelMicroGeometryProfile::VolumeDensity);
  // Ordinary terrain keeps its authored exact microgeometry. Water alone is
  // flat in the surface preset so its sheet is not lowered away from banks.
  assert(presets[0u].geometry.maximumCarveDepth == 0u);
  assert(presets[1u].geometry.maximumCarveDepth == 0u);
  assert(presets[6u].geometry.maximumCarveDepth == 0u);
  assert(presets[3u].geometry.maximumCarveDepth > 0u);
  assert(presets[4u].geometry.maximumCarveDepth > 0u);
  assert(std::all_of(
      volumePresets.begin(), volumePresets.end(), [](const auto &preset)
      {
        return preset.geometry.maximumCarveDepth > 0u;
      }));

  VoxelMaterialAsset asset = terrainVoxelMaterialAsset(3u, presets[3u]);
  std::string error;
  assert(asset.validate(&error));
  assert(asset.assetId == "terrain/biome/3");
  assert(asset.contextRequirements.contains(
      VoxelMaterialContextField::TerrainSlope));
  const uint64_t stableHash = asset.contentHash();
  assert(stableHash == asset.contentHash());
  VoxelMaterialAsset edited = asset;
  edited.surface.roughness = 0.25f;
  assert(edited.contentHash() != stableHash);
  edited = asset;
  edited.version = kVoxelMaterialAssetVersion + 1u;
  assert(!edited.validate(&error));
  assert(error.find("version") != std::string::npos);

  VoxelMaterialDefinition definition{};
  assert(asset.definition(definition, &error));
  VoxelMaterialManager manager;
  VoxelMaterialHandle handle = 0u;
  assert(manager.registerMaterial(definition, handle, &error));
  assert(handle != 0u);
  assert((manager.shadingPrograms()[handle].flags &
      kVoxelShadingProgramEnabled) != 0u);
  assert((manager.shadingPrograms()[handle].flags &
      kVoxelShadingProgramMaterialFieldMask) ==
      (0x4u << kVoxelShadingProgramMaterialFieldShift));
  assert(asset.fieldBindings[0].semantic ==
      VoxelMaterialFieldSemantic::Grass);
  assert(asset.fieldBindings[1].semantic ==
      VoxelMaterialFieldSemantic::Dirt);
  assert(asset.fieldBindings[2].semantic ==
      VoxelMaterialFieldSemantic::Variation);
  assert(asset.shadingProgram.instructions.size() <=
      kMaximumVoxelShadingInstructionCount);

  VoxelShadingInputs flatTop{};
  flatTop.terrainSlopeClassified = true;
  flatTop.stableRandom = 0.25f;
  flatTop.fields = {0.0f, 1.0f, 0.5f, 0.0f};
  VoxelShadingResult flatTopResult{};
  assert(evaluateVoxelShadingProgram(
      asset.shadingProgram, flatTop, flatTopResult, &error));
  assert(flatTopResult.layerWeight == 0.0f);
  assert(std::abs(flatTopResult.hueVariation - 0.04f) < 1.0e-6f);
  assert(std::abs(flatTopResult.brightness - 0.98f) < 1.0e-6f);

  VoxelShadingInputs side = flatTop;
  side.sideOrRiser = true;
  VoxelShadingResult sideResult{};
  assert(evaluateVoxelShadingProgram(
      asset.shadingProgram, side, sideResult, &error));
  assert(sideResult.layerWeight == 1.0f);

  VoxelShadingInputs steep = flatTop;
  steep.terrainSlope = 1.0f;
  VoxelShadingResult steepResult{};
  assert(evaluateVoxelShadingProgram(
      asset.shadingProgram, steep, steepResult, &error));
  assert(steepResult.layerWeight == 1.0f);
}

void testInvalidLiveProgramPreservesResidentSlotAndFallsBackSafely()
{
  VoxelMaterialManager manager;
  std::string error;
  VoxelMaterialDefinition definition{
    "terrain/live", "Live", packedMaterial(),
    {static_cast<uint32_t>(VoxelMaterialContextField::TerrainSlope)},
    VoxelMaterialSlotPolicy::UniqueEditable,
    slopeDepthProgram(),
  };
  VoxelMaterialHandle handle = 0u;
  assert(manager.registerMaterial(definition, handle, &error));
  const VoxelShadingProgramGPU record = manager.shadingPrograms()[handle];
  const uint64_t revision = manager.shadingRevision();
  VoxelShadingProgram invalid{{
    {VoxelShadingOpcode::Add, 0u, 1u, 2u},
    {VoxelShadingOpcode::OutputLayerWeight, 0u, 0u},
  }};
  VoxelMaterialChange changes = VoxelMaterialChange::None;
  assert(!manager.updateShadingProgram(handle, invalid, changes, &error));
  assert(!error.empty());
  assert(changes == VoxelMaterialChange::None);
  assert(manager.shadingRevision() == revision);
  assert(manager.shadingPrograms()[handle].instructionOffset ==
      record.instructionOffset);
  assert(manager.shadingPrograms()[handle].instructionCount ==
      record.instructionCount);
  assert(manager.shadingPrograms()[handle].flags ==
      kVoxelShadingProgramEnabled);
}

void testOptionalResolvedShadingContextABI()
{
  const uint32_t flags =
      static_cast<uint32_t>(VoxelResolvedContextFlag::Valid) |
      static_cast<uint32_t>(VoxelResolvedContextFlag::Exact) |
      static_cast<uint32_t>(VoxelResolvedContextFlag::SideOrRiser);
  const uint32_t packed = packVoxelResolvedShadingContext(
      0.25f, 0.75f, 0.5f, flags);
  assert(std::abs(unpackVoxelResolvedContextChannel(
      packed, kVoxelResolvedContextDepthShift) - 0.25f) <= 0.5f / 255.0f);
  assert(std::abs(unpackVoxelResolvedContextChannel(
      packed, kVoxelResolvedContextTerrainBlendShift) - 0.75f) <=
      0.5f / 255.0f);
  assert(std::abs(unpackVoxelResolvedContextChannel(
      packed, kVoxelResolvedContextAuxiliaryShift) - 0.5f) <=
      0.5f / 255.0f + 1.0e-6f);
  assert(unpackVoxelResolvedContextFlags(packed) == flags);
  assert((packVoxelResolvedShadingContext(
      -1.0f, 2.0f, std::nanf(""), 0x1ffu) & 0x00ffffffu) ==
      0x0000ff00u);
}

void testPhaseZeroGrassShadingSignature()
{
  const auto presets = defaultTerrainVoxelMaterialPresets(false);
  const VoxelMaterialAsset asset =
      terrainVoxelMaterialAsset(3u, presets[3u]);
  const VoxelShadingProgram program = asset.shadingProgram;
  const auto fallbackFields = voxelMaterialFieldDefaultsFloat(
      compileVoxelMaterialFieldDefaults(asset.fieldBindings));
  uint64_t hash = 1469598103934665603ull;
  std::string error;
  for (uint32_t depth = 0u; depth <= 32u; ++depth)
    for (uint32_t random = 0u; random <= 16u; ++random)
      for (uint32_t slope = 0u; slope <= 18u; ++slope)
        for (uint32_t side = 0u; side < 2u; ++side)
        {
          VoxelShadingInputs inputs{};
          inputs.normalizedDepth = static_cast<float>(depth) / 32.0f;
          inputs.stableRandom = static_cast<float>(random) / 16.0f;
          inputs.terrainSlope = static_cast<float>(slope) / 18.0f;
          inputs.sideOrRiser = side != 0u;
          inputs.terrainSlopeClassified = true;
          inputs.fields = fallbackFields;
          VoxelShadingResult result{};
          assert(evaluateVoxelShadingProgram(program, inputs, result, &error));
          const std::array<uint32_t, 4> values{
            static_cast<uint32_t>(std::lround(result.layerWeight * 65535.0f)),
            static_cast<uint32_t>(std::lround(
                (result.hueVariation + 1.0f) * 32767.0f)),
            static_cast<uint32_t>(std::lround(result.brightness * 32767.0f)),
            static_cast<uint32_t>(std::lround(result.roughness * 65535.0f)),
          };
          for (const uint32_t value : values)
          {
            hash ^= value;
            hash *= 1099511628211ull;
          }
        }
  // Step 5 replaces per-microvoxel random grass colour with the authored
  // missing-region defaults. Generated region fields have their own seam/LOD
  // fixtures below.
  constexpr uint64_t expected = 0x23c61a8695e05ce7ull;
  if (hash != expected)
    std::cerr << "phase0 grass shading signature: " << std::hex << hash
              << std::dec << '\n';
  assert(hash == expected);
}

void testLiveShadingParametersUseStableSlots()
{
  const VoxelShadingProgram program{{
    {VoxelShadingOpcode::LoadParameter, 0u, 2u},
    {VoxelShadingOpcode::OutputLayerWeight, 0u, 0u},
  }};
  VoxelShadingInputs inputs{};
  inputs.parameters.values[2u] = 0.625f;
  VoxelShadingResult result{};
  std::string error;
  assert(evaluateVoxelShadingProgram(program, inputs, result, &error));
  assert(result.layerWeight == 0.625f);

  VoxelMaterialDefinition definition{
    "terrain/parameterized", "Parameterized", packedMaterial(), {},
    VoxelMaterialSlotPolicy::UniqueEditable, program,
  };
  definition.shadingParameters.values[2u] = 0.25f;
  VoxelMaterialManager manager;
  VoxelMaterialHandle handle = 0u;
  assert(manager.registerMaterial(definition, handle, &error));
  const uint32_t instructionOffset =
      manager.shadingPrograms()[handle].instructionOffset;
  const uint64_t geometryRevision = manager.geometryRevision();
  VoxelShadingParameterBlockGPU edited =
      manager.shadingParameters()[handle];
  edited.values[2u] = 0.75f;
  VoxelMaterialChange changes = VoxelMaterialChange::None;
  assert(manager.updateShadingParameters(handle, edited, changes, &error));
  assert(changes == VoxelMaterialChange::Shading);
  assert(manager.shadingParameters()[handle].values[2u] == 0.75f);
  assert(manager.shadingPrograms()[handle].instructionOffset ==
      instructionOffset);
  assert(manager.geometryRevision() == geometryRevision);
  edited.values[0u] = std::nanf("");
  assert(!manager.updateShadingParameters(handle, edited, changes, &error));
}

VoxelGeometryProgram terrainBumpProgram(const VoxelGeometryFixed strength)
{
  VoxelGeometryProgram program{};
  program.instructions = {
    {VoxelGeometryOpcode::LoadBaseSurface, 0u},
    {VoxelGeometryOpcode::LoadWorldNoise, 1u, 0u, 0u, 0x3141},
    {VoxelGeometryOpcode::LoadConstant, 2u, 0u, 0u,
     kVoxelGeometryFixedOne / 2},
    {VoxelGeometryOpcode::Subtract, 1u, 1u, 2u},
    {VoxelGeometryOpcode::LoadConstant, 3u, 0u, 0u, strength},
    {VoxelGeometryOpcode::Multiply, 1u, 1u, 3u},
    {VoxelGeometryOpcode::Add, 0u, 0u, 1u},
    {VoxelGeometryOpcode::Clamp01, 0u, 0u},
    {VoxelGeometryOpcode::Quantize, 0u, 0u, 0u,
     kVoxelGeometryFixedOne / 8},
    {VoxelGeometryOpcode::OutputRetainedHeight, 0u, 0u},
  };
  return program;
}

void testGeometryIRVerificationExecutionAndTerrainComposition()
{
  std::string error;
  const VoxelGeometryProgram program =
      terrainBumpProgram(kVoxelGeometryFixedOne / 4);
  assert(verifyVoxelGeometryProgram(program, &error));
  assert(voxelGeometryProgramUsesBaseSurface(program));

  VoxelGeometryCompilation compiled{};
  assert(compileVoxelGeometryProgram(program, 0u, compiled, &error));
  assert(compiled.execution == VoxelGeometryExecution::CachedHeightfield);
  assert(compiled.instructions.size() == program.instructions.size());
  assert((compiled.gpu.executionFlags & kVoxelGeometryProgramEnabled) != 0u);

  VoxelGeometryInputs inputs{};
  inputs.parentKey = {-11, 7, 19};
  inputs.face = static_cast<uint32_t>(SparseVoxelMicroFace::PositiveY);
  inputs.subdivision = 8u;
  inputs.materialSeed = 0x1234u;
  inputs.baseSurface = voxelGeometryFixedFromDouble(0.625);
  for (uint32_t v = 0u; v < inputs.subdivision; ++v)
    for (uint32_t u = 0u; u < inputs.subdivision; ++u)
    {
      inputs.faceCell = {u, v};
      VoxelGeometryResult cpu{};
      VoxelGeometryResult packed{};
      assert(evaluateVoxelGeometryProgram(program, inputs, cpu, &error));
      assert(evaluateCompiledVoxelGeometryProgram(
          compiled.gpu, compiled.instructions, inputs, packed, &error));
      assert(cpu.retainedHeight == packed.retainedHeight);
      assert(cpu.carveDepth == packed.carveDepth);
      assert(cpu.coverage == packed.coverage);
      assert(cpu.retainedHeight % (kVoxelGeometryFixedOne / 8) == 0);
    }

  VoxelGeometryHeightfield heightfield{};
  assert(materializeVoxelGeometryHeightfield(
      program, inputs, heightfield, &error));
  assert(heightfield.subdivision == 8u);
  assert(heightfield.retainedHeights.size() == 64u);

  VoxelGeometryProgram direct = program;
  direct.executionHint = VoxelGeometryExecutionHint::Direct;
  assert(compileVoxelGeometryProgram(direct, 0u, compiled, &error));
  assert(compiled.execution == VoxelGeometryExecution::Direct);

  VoxelGeometryProgram invalid{{}, {}};
  invalid.instructions = {
    {VoxelGeometryOpcode::Add, 0u, 1u, 2u},
    {VoxelGeometryOpcode::OutputRetainedHeight, 0u, 0u},
  };
  assert(!verifyVoxelGeometryProgram(invalid, &error));
  invalid.instructions = {
    {VoxelGeometryOpcode::LoadConstant, 0u, 0u, 0u,
     kVoxelGeometryFixedOne},
    {VoxelGeometryOpcode::Quantize, 0u, 0u, 0u, 0},
    {VoxelGeometryOpcode::OutputRetainedHeight, 0u, 0u},
  };
  assert(!verifyVoxelGeometryProgram(invalid, &error));

  VoxelGeometryProgram contextProgram{};
  contextProgram.instructions = {
    {VoxelGeometryOpcode::LoadContextMask, 0u,
     static_cast<uint32_t>(VoxelMaterialContextField::TerrainSlope), 0u,
     0x80},
    {VoxelGeometryOpcode::OutputCarveDepth, 0u, 0u},
  };
  VoxelMaterialDefinition missingContext{
    "terrain/missing-geometry-context", "Missing context", packedMaterial(),
    {}, VoxelMaterialSlotPolicy::UniqueEditable,
  };
  missingContext.geometryProgram = contextProgram;
  VoxelMaterialManager contextManager;
  VoxelMaterialHandle contextHandle = 0u;
  assert(!contextManager.registerMaterial(
      missingContext, contextHandle, &error));
  missingContext.contextRequirements.fields =
      static_cast<uint32_t>(VoxelMaterialContextField::TerrainSlope);
  assert(contextManager.registerMaterial(
      missingContext, contextHandle, &error));
}

void testGeometryIRWorldSeamsVolumeCacheAndStableCoverage()
{
  std::string error;
  VoxelGeometryProgram noiseHeight{};
  noiseHeight.instructions = {
    {VoxelGeometryOpcode::LoadWorldNoise, 0u, 0u, 0u, 0x55aa},
    {VoxelGeometryOpcode::OutputRetainedHeight, 0u, 0u},
  };
  VoxelGeometryInputs left{};
  left.parentKey = {4, -2, 9};
  left.face = static_cast<uint32_t>(SparseVoxelMicroFace::PositiveY);
  left.faceCell = {8u, 3u};
  left.subdivision = 8u;
  left.materialSeed = 77u;
  VoxelGeometryInputs right = left;
  ++right.parentKey[0];
  right.faceCell[0] = 0u;
  VoxelGeometryResult a{};
  VoxelGeometryResult b{};
  assert(evaluateVoxelGeometryProgram(noiseHeight, left, a, &error));
  assert(evaluateVoxelGeometryProgram(noiseHeight, right, b, &error));
  assert(a.retainedHeight == b.retainedHeight);

  VoxelGeometryProgram volume{};
  volume.domain = VoxelGeometryDomain::Volume;
  volume.instructions = {
    {VoxelGeometryOpcode::LoadWorldNoise, 0u, 0u, 0u, 0x6d2b},
    {VoxelGeometryOpcode::Quantize, 0u, 0u, 0u,
     kVoxelGeometryFixedOne},
    {VoxelGeometryOpcode::OutputCoverage, 0u, 0u},
  };
  VoxelGeometryCompilation compiled{};
  assert(compileVoxelGeometryProgram(volume, 0u, compiled, &error));
  assert(compiled.execution == VoxelGeometryExecution::CachedVolume);
  VoxelGeometryInputs inputs{};
  inputs.parentKey = {-3, 5, 12};
  inputs.subdivision = 6u;
  inputs.materialSeed = 912u;
  VoxelGeometryVolume cache{};
  assert(materializeVoxelGeometryVolume(volume, inputs, cache, &error));
  assert(cache.occupancy.size() == 216u);
  for (uint32_t z = 0u; z < 6u; ++z)
    for (uint32_t y = 0u; y < 6u; ++y)
      for (uint32_t x = 0u; x < 6u; ++x)
      {
        VoxelGeometryInputs sample = inputs;
        sample.face = 6u;
        sample.volumeCell = {x, y, z};
        VoxelGeometryResult cpu{};
        VoxelGeometryResult gpuRecord{};
        assert(evaluateVoxelGeometryProgram(volume, sample, cpu, &error));
        assert(evaluateCompiledVoxelGeometryProgram(
            compiled.gpu, compiled.instructions, sample, gpuRecord, &error));
        assert(cpu.coverage == gpuRecord.coverage);
        const size_t index = (z * 6u + y) * 6u + x;
        assert(cache.occupancy[index] == (cpu.coverage > 0 ? 1u : 0u));
      }

  const VoxelGeometryFixed coverageA =
      voxelGeometryExpectedCoverage(volume, inputs, 8u);
  const VoxelGeometryFixed coverageB =
      voxelGeometryExpectedCoverage(volume, inputs, 8u);
  assert(coverageA == coverageB);
  assert(coverageA >= 0 && coverageA <= kVoxelGeometryFixedOne);
}

void testGeometryProgramsRetainHandlesAndInvalidateTemporalHistory()
{
  VoxelMaterialDefinition definition{
    "terrain/live-geometry", "Live geometry", packedMaterial(),
    {static_cast<uint32_t>(VoxelMaterialContextField::BaseSurface)},
    VoxelMaterialSlotPolicy::UniqueEditable,
  };
  definition.geometryProgram = terrainBumpProgram(0);
  VoxelMaterialManager manager;
  VoxelMaterialHandle handle = 0u;
  std::string error;
  assert(manager.registerMaterial(definition, handle, &error));
  const uint32_t offset = manager.geometryPrograms()[handle].instructionOffset;
  const uint64_t firstIdentity = manager.geometryIdentity(handle);
  const size_t slotCount = manager.slotCount();

  VoxelGeometryProgram strong =
      terrainBumpProgram(kVoxelGeometryFixedOne / 3);
  VoxelMaterialChange changes{};
  assert(manager.updateGeometryProgram(handle, strong, changes, &error));
  assert(changes == VoxelMaterialChange::Geometry);
  assert(!voxelMaterialChangeRequiresPageRebuild(changes));
  assert(voxelMaterialChangeInvalidatesVisibilityHistory(changes));
  assert(manager.slotCount() == slotCount);
  assert(manager.handleForAsset("terrain/live-geometry") == handle);
  assert(manager.geometryPrograms()[handle].instructionOffset == offset);
  assert(manager.geometryIdentity(handle) != firstIdentity);

  VoxelGeometryParameterBlockGPU parameters{};
  parameters.values[0] = voxelGeometryFixedFromDouble(0.2);
  const uint64_t programIdentity = manager.geometryIdentity(handle);
  assert(manager.updateGeometryParameters(
      handle, parameters, changes, &error));
  assert(changes == VoxelMaterialChange::Geometry);
  assert(manager.geometryIdentity(handle) != programIdentity);
  assert(manager.geometryParameters()[handle] == parameters);

  for (uint32_t profile =
           static_cast<uint32_t>(SparseVoxelMicroGeometryProfile::Constant);
       profile <=
           static_cast<uint32_t>(SparseVoxelMicroGeometryProfile::Staircase);
       ++profile)
  {
    SparseVoxelMicroDescriptor descriptor{};
    descriptor.geometryProfile =
        static_cast<SparseVoxelMicroGeometryProfile>(profile);
    const VoxelGeometryCompilation builtIn =
        compileVoxelGeometryBuiltIn(descriptor);
    assert(builtIn.execution == VoxelGeometryExecution::BuiltIn);
    assert(builtIn.gpu.builtInProfile == profile);
    assert(builtIn.instructions.empty());
  }
}

void testPhase5ContextInputsAndVolumeCSG()
{
  VoxelGeometryProgram program{};
  program.domain = VoxelGeometryDomain::Volume;
  program.instructions = {
    {VoxelGeometryOpcode::LoadTerrainBlend, 0u},
    {VoxelGeometryOpcode::LoadAuxiliary, 1u},
    {VoxelGeometryOpcode::VolumeUnion, 2u, 0u, 1u},
    {VoxelGeometryOpcode::LoadDepthBelowSurface, 3u},
    {VoxelGeometryOpcode::VolumeSubtract, 2u, 2u, 3u},
    {VoxelGeometryOpcode::OutputCoverage, 0u, 2u},
  };
  std::string error;
  assert(verifyVoxelGeometryProgram(program, &error));
  const uint32_t expectedRequirements =
      static_cast<uint32_t>(VoxelMaterialContextField::DepthBelowSurface) |
      static_cast<uint32_t>(VoxelMaterialContextField::TerrainBlend) |
      static_cast<uint32_t>(VoxelMaterialContextField::Auxiliary);
  assert(voxelGeometryRequiredContextFields(program) == expectedRequirements);

  VoxelGeometryInputs inputs{};
  inputs.terrainBlend = voxelGeometryFixedFromDouble(0.4);
  inputs.auxiliary = voxelGeometryFixedFromDouble(0.8);
  inputs.depthBelowSurface = voxelGeometryFixedFromDouble(0.25);
  VoxelGeometryResult cpu{};
  assert(evaluateVoxelGeometryProgram(program, inputs, cpu, &error));
  assert(std::abs(voxelGeometryFixedToDouble(cpu.coverage) - 0.75) < 2.0e-5);

  VoxelGeometryCompilation compiled{};
  assert(compileVoxelGeometryProgram(program, 0u, compiled, &error));
  VoxelGeometryResult packed{};
  assert(evaluateCompiledVoxelGeometryProgram(
      compiled.gpu, compiled.instructions, inputs, packed, &error));
  assert(packed.coverage == cpu.coverage);

  VoxelGeometryProgram invalidHeightfield = program;
  invalidHeightfield.domain = VoxelGeometryDomain::Heightfield;
  assert(!verifyVoxelGeometryProgram(invalidHeightfield, &error));
  assert(error.find("volume CSG") != std::string::npos);
}

void testPhase5DepthSlopeAndReferenceFixtures()
{
  std::string error;
  const VoxelGeometryProgram snow = snowRetentionVoxelGeometryProgram();
  assert(verifyVoxelGeometryProgram(snow, &error));
  assert(voxelGeometryRequiredContextFields(snow) ==
      (static_cast<uint32_t>(VoxelMaterialContextField::BaseSurface) |
       static_cast<uint32_t>(VoxelMaterialContextField::TerrainSlope)));
  VoxelGeometryInputs flat{};
  flat.baseSurface = voxelGeometryFixedFromDouble(0.5);
  flat.terrainSlope = 0;
  flat.parameters.values[0] = voxelGeometryFixedFromDouble(0.25);
  VoxelGeometryInputs steep = flat;
  steep.terrainSlope = kVoxelGeometryFixedOne;
  VoxelGeometryResult flatSnow{};
  VoxelGeometryResult steepSnow{};
  assert(evaluateVoxelGeometryProgram(snow, flat, flatSnow, &error));
  assert(evaluateVoxelGeometryProgram(snow, steep, steepSnow, &error));
  assert(flatSnow.retainedHeight == voxelGeometryFixedFromDouble(0.75));
  assert(steepSnow.retainedHeight == flat.baseSurface);
  // Slope changes only the material modifier; both evaluations retain the
  // exact same terrain-owned base surface input.
  assert(flat.baseSurface == steep.baseSurface);

  VoxelGeometryProgram depth{};
  depth.domain = VoxelGeometryDomain::Volume;
  depth.instructions = {
    {VoxelGeometryOpcode::LoadDepthBelowSurface, 0u},
    {VoxelGeometryOpcode::LoadConstant, 1u, 0u, 0u,
     voxelGeometryFixedFromDouble(0.5)},
    {VoxelGeometryOpcode::Subtract, 0u, 0u, 1u},
    {VoxelGeometryOpcode::Clamp01, 0u, 0u},
    {VoxelGeometryOpcode::Quantize, 0u, 0u, 0u,
     kVoxelGeometryFixedOne / 2},
    {VoxelGeometryOpcode::OutputCoverage, 0u, 0u},
  };
  VoxelGeometryInputs topsoil{};
  topsoil.depthBelowSurface = voxelGeometryFixedFromDouble(0.2);
  VoxelGeometryInputs deep = topsoil;
  deep.depthBelowSurface = voxelGeometryFixedFromDouble(0.9);
  VoxelGeometryResult topsoilResult{};
  VoxelGeometryResult deepResult{};
  assert(evaluateVoxelGeometryProgram(depth, topsoil, topsoilResult, &error));
  assert(evaluateVoxelGeometryProgram(depth, deep, deepResult, &error));
  assert(topsoilResult.coverage == 0);
  assert(deepResult.coverage > 0);

  const VoxelGeometryProgram porous = porousRockVoxelGeometryProgram();
  const VoxelGeometryProgram foliage = foliageEnvelopeVoxelGeometryProgram();
  assert(verifyVoxelGeometryProgram(porous, &error));
  assert(verifyVoxelGeometryProgram(foliage, &error));
  assert(voxelGeometryRequiredContextFields(foliage) ==
      static_cast<uint32_t>(VoxelMaterialContextField::TerrainBlend));
  VoxelGeometryCompilation porousCompilation{};
  VoxelGeometryCompilation foliageCompilation{};
  assert(compileVoxelGeometryProgram(porous, 0u, porousCompilation, &error));
  assert(compileVoxelGeometryProgram(foliage, 0u, foliageCompilation, &error));
  assert(porousCompilation.execution == VoxelGeometryExecution::CachedVolume);
  assert(foliageCompilation.execution == VoxelGeometryExecution::CachedVolume);

  auto presets = defaultTerrainVoxelMaterialPresets(true);
  const VoxelMaterialAsset migrated = terrainVoxelMaterialAsset(3u, presets[3]);
  assert(migrated.geometry.geometryProfile ==
      SparseVoxelMicroGeometryProfile::VolumeDensity);
  assert(!migrated.geometryProgram.instructions.empty());
  assert(migrated.geometryProgram.domain == VoxelGeometryDomain::Volume);
}

void testPhase5VolumeTraversalMatchesBruteForceCells()
{
  const VoxelGeometryProgram program = foliageEnvelopeVoxelGeometryProgram();
  VoxelGeometryInputs inputs{};
  inputs.parentKey = {-9, 3, 14};
  inputs.subdivision = 8u;
  inputs.materialSeed = 0x7123u;
  inputs.terrainBlend = kVoxelGeometryFixedOne;
  inputs.parameters.values[0] = voxelGeometryFixedFromDouble(0.48);
  std::string error;
  VoxelGeometryVolume volume{};
  assert(materializeVoxelGeometryVolume(program, inputs, volume, &error));

  // Axis-aligned DDA through every Y/Z row. The traversal reads the cached
  // mask; the reference independently evaluates every microcell.
  for (uint32_t z = 0u; z < inputs.subdivision; ++z)
    for (uint32_t y = 0u; y < inputs.subdivision; ++y)
    {
      int32_t ddaHit = -1;
      int32_t referenceHit = -1;
      for (uint32_t x = 0u; x < inputs.subdivision; ++x)
      {
        const size_t index =
            (z * inputs.subdivision + y) * inputs.subdivision + x;
        if (ddaHit < 0 && volume.occupancy[index] != 0u)
          ddaHit = static_cast<int32_t>(x);
        bool occupied = false;
        assert(voxelGeometryCellOccupied(
            program, inputs, {x, y, z}, occupied, &error));
        if (referenceHit < 0 && occupied)
          referenceHit = static_cast<int32_t>(x);
      }
      assert(ddaHit == referenceHit);
    }
}

void testPhase5OptionalContextPoolAndSelectivePageRebuilds()
{
  VoxelAuxContextPool disabled;
  assert(!disabled.enabled());
  assert(disabled.memory().pooledBytes == 0u);
  assert(!voxelContextFieldsRequireAuxiliaryPool(
      static_cast<uint32_t>(VoxelMaterialContextField::BaseSurface) |
      static_cast<uint32_t>(VoxelMaterialContextField::MicroSlope)));
  assert(voxelContextFieldsRequireAuxiliaryPool(
      static_cast<uint32_t>(VoxelMaterialContextField::DepthBelowSurface)));

  VoxelAuxContextPool pool;
  pool.begin(128u);
  const VoxelAuxContext wet = makeVoxelAuxContext(0.25f, 0.5f, 0.75f, 1.0f);
  std::string error;
  for (uint32_t index = 0u; index < 128u; ++index)
    assert(pool.append((index & 1u) != 0u ? wet : VoxelAuxContext{}, &error));
  assert(pool.enabled());
  assert(pool.indices().size() == 128u);
  assert(pool.values().size() == 2u);
  assert(pool.context(1u) == wet);
  assert(pool.context(0u) == VoxelAuxContext{});
  VoxelGeometryInputs decoded{};
  applyPackedVoxelAuxContext(pool.values()[pool.indices()[1u]], decoded);
  assert(std::abs(voxelGeometryFixedToDouble(decoded.terrainBlend) - 0.75) <=
      1.0 / 255.0);
  assert(decoded.auxiliary == kVoxelGeometryFixedOne);
  const VoxelAuxContextMemory memory = pool.memory();
  assert(memory.pooledBytes == 128u * sizeof(uint16_t) + 2u * sizeof(uint32_t));
  assert(memory.directBytes == 128u * sizeof(uint32_t));
  assert(memory.withinDirectBudget());

  const std::vector<VoxelPageContextState> pages{
    {101u, static_cast<uint32_t>(VoxelMaterialContextField::BaseSurface),
     {7u, 8u}},
    {102u, static_cast<uint32_t>(VoxelMaterialContextField::BaseSurface) |
               static_cast<uint32_t>(VoxelMaterialContextField::TerrainBlend),
     {7u}},
    {103u, static_cast<uint32_t>(VoxelMaterialContextField::BaseSurface),
     {9u}},
  };
  const uint32_t before =
      static_cast<uint32_t>(VoxelMaterialContextField::BaseSurface);
  const uint32_t after = before |
      static_cast<uint32_t>(VoxelMaterialContextField::TerrainBlend);
  const std::vector<uint64_t> rebuilds = voxelPagesRequiringContextRebuild(
      pages, 7u, before, after);
  assert((rebuilds == std::vector<uint64_t>{101u}));
  assert(voxelPagesRequiringContextRebuild(
      pages, 7u, after, before).empty());
}

void testPhase5UnavailableContextDiagnostics()
{
  VoxelGeometryProgram program{};
  program.domain = VoxelGeometryDomain::Volume;
  program.instructions = {
    {VoxelGeometryOpcode::LoadDepthBelowSurface, 0u},
    {VoxelGeometryOpcode::LoadTerrainBlend, 1u},
    {VoxelGeometryOpcode::VolumeIntersection, 0u, 0u, 1u},
    {VoxelGeometryOpcode::OutputCoverage, 0u, 0u},
  };
  VoxelMaterialDefinition definition{
    "terrain/context-diagnostic", "Context diagnostic", packedMaterial(),
    {}, VoxelMaterialSlotPolicy::UniqueEditable,
  };
  definition.geometryProgram = program;
  VoxelMaterialManager manager;
  VoxelMaterialHandle handle = 0u;
  std::string error;
  assert(!manager.registerMaterial(definition, handle, &error));
  assert(error.find("depth below surface") != std::string::npos);
  assert(error.find("terrain blend") != std::string::npos);
}

void testPhase6AssetRoundTripCompilerCacheAndCompatibility()
{
  auto presets = defaultTerrainVoxelMaterialPresets(false);
  VoxelMaterialAsset original = terrainVoxelMaterialAsset(3u, presets[3u]);
  original.name = "Phase 6 grass / soil";
  original.surface.primaryAlbedoRGBA8 = 0xff3d8f43u;
  original.surface.secondaryAlbedoRGBA8 = 0xff5a3b25u;
  original.surface.roughness = 0.72f;
  original.shadingParameters.values[3] = 0.125f;

  std::string error;
  std::vector<uint8_t> encoded;
  assert(serializeVoxelMaterialAsset(original, encoded, &error));
  VoxelMaterialAsset decoded{};
  assert(deserializeVoxelMaterialAsset(encoded, decoded, &error));
  assert(decoded.contentHash() == original.contentHash());
  std::vector<uint8_t> reencoded;
  assert(serializeVoxelMaterialAsset(decoded, reencoded, &error));
  assert(reencoded == encoded);

  VoxelMaterialCompilerCache originalCache;
  VoxelMaterialCompilerCache decodedCache;
  const VoxelMaterialCompiledAsset *originalCompiled = nullptr;
  const VoxelMaterialCompiledAsset *decodedCompiled = nullptr;
  assert(originalCache.compile(original, originalCompiled, &error));
  assert(decodedCache.compile(decoded, decodedCompiled, &error));
  assert(originalCompiled->sourceHash == decodedCompiled->sourceHash);
  assert(std::memcmp(
             &originalCompiled->geometry.gpu,
             &decodedCompiled->geometry.gpu,
             sizeof(VoxelGeometryProgramGPU)) == 0);
  assert(originalCompiled->geometry.instructions.size() ==
      decodedCompiled->geometry.instructions.size());
  assert(originalCompiled->shadingInstructions.size() ==
      decodedCompiled->shadingInstructions.size());
  assert(originalCompiled->geometry.instructions.empty() ||
      std::memcmp(
          originalCompiled->geometry.instructions.data(),
          decodedCompiled->geometry.instructions.data(),
          originalCompiled->geometry.instructions.size() *
              sizeof(VoxelGeometryInstructionGPU)) == 0);
  assert(originalCompiled->shadingInstructions.empty() ||
      std::memcmp(
          originalCompiled->shadingInstructions.data(),
          decodedCompiled->shadingInstructions.data(),
          originalCompiled->shadingInstructions.size() *
              sizeof(VoxelShadingInstructionGPU)) == 0);

  const VoxelMaterialCompiledAsset *cached = nullptr;
  assert(originalCache.compile(original, cached, &error));
  assert(cached == originalCompiled);
  assert(originalCache.entryCount() == 1u);
  assert(originalCache.missCount() == 1u);
  assert(originalCache.hitCount() == 1u);

  std::vector<uint8_t> corrupt = encoded;
  corrupt.back() ^= 0x80u;
  assert(!deserializeVoxelMaterialAsset(corrupt, decoded, &error));
  assert(error.find("checksum") != std::string::npos);

  VoxelMaterialAsset legacy{};
  assert(adaptLegacyVoxelMaterialAsset(
      "legacy/phase6", "Legacy phase 6", packedMaterial(919u), legacy,
      &error));
  assert(legacy.shadingProgram.instructions.empty());
  assert(legacy.geometryProgram.instructions.empty());
  const VoxelMaterialCompiledAsset *legacyCompiled = nullptr;
  assert(originalCache.compile(legacy, legacyCompiled, &error));
  assert(legacyCompiled->geometry.execution == VoxelGeometryExecution::BuiltIn);
}

void testPhase6StableHotReloadAndGraphSelection()
{
  auto presets = defaultTerrainVoxelMaterialPresets(false);
  VoxelMaterialAsset asset = terrainVoxelMaterialAsset(3u, presets[3u]);
  VoxelMaterialManager manager;
  VoxelMaterialCompilerCache cache;
  VoxelMaterialHandle handle = 0u;
  std::string error;
  assert(loadVoxelMaterialAsset(manager, cache, asset, handle, &error));
  assert(handle != kInvalidVoxelMaterialHandle);
  assert(manager.assetId(handle) == asset.assetId);

  VoxelMaterialAsset edited = asset;
  edited.shadingParameters.values[0] += 0.05f;
  edited.geometryParameters.values[0] = voxelGeometryFixedFromDouble(0.3);
  VoxelMaterialChange changes = VoxelMaterialChange::None;
  assert(hotReloadVoxelMaterialAsset(
      manager, cache, handle, edited, changes, &error));
  assert(manager.handleForAsset(asset.assetId) == handle);
  assert(voxelMaterialChangeContains(changes, VoxelMaterialChange::Shading));
  assert(voxelMaterialChangeContains(changes, VoxelMaterialChange::Geometry));

  const uint64_t revision = manager.contentRevision();
  VoxelMaterialAsset invalid = edited;
  invalid.geometryProgram.instructions = {
    {VoxelGeometryOpcode::Add, 0u, 1u, 2u},
    {VoxelGeometryOpcode::OutputRetainedHeight, 0u, 0u},
  };
  assert(!hotReloadVoxelMaterialAsset(
      manager, cache, handle, invalid, changes, &error));
  assert(error.find("Geometry node 0") != std::string::npos);
  assert(manager.contentRevision() == revision);
  assert(manager.handleForAsset(asset.assetId) == handle);

  const std::array<VoxelMaterialGraphPreset, 5u> graphs{
    VoxelMaterialGraphPreset::LegacyCompatibility,
    VoxelMaterialGraphPreset::GrassOverSoil,
    VoxelMaterialGraphPreset::SnowRetention,
    VoxelMaterialGraphPreset::PorousRock,
    VoxelMaterialGraphPreset::FoliageEnvelope,
  };
  for (const VoxelMaterialGraphPreset graph : graphs)
  {
    TerrainVoxelMaterialPreset preset = presets[3u];
    preset.graphPreset = graph;
    VoxelMaterialAsset graphAsset = terrainVoxelMaterialAsset(3u, preset);
    assert(graphAsset.validate(&error));
    if (graph != VoxelMaterialGraphPreset::LegacyCompatibility &&
        graph != VoxelMaterialGraphPreset::GrassOverSoil)
      assert(!graphAsset.geometryProgram.instructions.empty());
  }
}

void testPhase6NodeDiagnosticsAndDebugABI()
{
  std::string error;
  VoxelShadingProgram invalidShading{{
    {VoxelShadingOpcode::Add, 0u, 1u, 2u},
    {VoxelShadingOpcode::OutputLayerWeight, 0u, 0u},
  }};
  assert(!verifyVoxelShadingProgram(invalidShading, &error));
  assert(error.find("Shading node 0") != std::string::npos);
  assert(error.find("constraint") != std::string::npos);

  VoxelGeometryProgram invalidGeometry{};
  invalidGeometry.instructions = {
    {VoxelGeometryOpcode::Add, 0u, 1u, 2u},
    {VoxelGeometryOpcode::OutputRetainedHeight, 0u, 0u},
  };
  assert(!verifyVoxelGeometryProgram(invalidGeometry, &error));
  assert(error.find("Geometry node 0") != std::string::npos);
  assert(error.find("constraint") != std::string::npos);

  assert(static_cast<uint32_t>(SparseVoxelDebugMode::MaterialHandle) == 21u);
  assert(static_cast<uint32_t>(SparseVoxelDebugMode::MaterialExactFallback) ==
      28u);
}

void testPhase7Tree64MaterialSidecarAndBoundedLookup()
{
  std::vector<SparseVoxelTree64MaterialCellGPU> source(3u);
  source[0].globalCell = {-7, 2, 19};
  source[0].materialAndFlags = 3u;
  source[0].surfaceData = 0x81010203u;
  source[0].auxContext = 0x44332211u;
  source[1].globalCell = {9, -4, 1};
  source[1].materialAndFlags = 4u;
  source[2] = source[0];

  std::vector<SparseVoxelTree64MaterialCellGPU> table;
  std::string error;
  assert(buildSparseVoxelTree64MaterialTable(source, table, &error));
  assert(!table.empty() && (table.size() & (table.size() - 1u)) == 0u);
  uint32_t probes = 0u;
  const auto *negative = findSparseVoxelTree64MaterialCell(
      table, {-7, 2, 19}, &probes);
  assert(negative != nullptr && negative->materialHandle() == 3u);
  assert(negative->surfaceData == 0x81010203u);
  assert(negative->auxContext == 0x44332211u);
  assert(probes > 0u && probes <= kSparseVoxelTree64MaterialMaximumProbes);
  assert(findSparseVoxelTree64MaterialCell(table, {0, 0, 0}) == nullptr);

  std::vector<SparseVoxelTree64MaterialCellGPU> emptyTable;
  assert(buildSparseVoxelTree64MaterialTable({}, emptyTable, &error));
  assert(emptyTable.size() == 2u);
  assert(!emptyTable.front().valid());

  auto conflicting = source;
  conflicting.back().materialAndFlags = 7u;
  assert(!buildSparseVoxelTree64MaterialTable(
      conflicting, table, &error));
  assert(table.empty());
  assert(error.find("conflicting definitions") != std::string::npos);

  auto unsupported = source.front();
  unsupported.reserved1 = 1u;
  assert(!buildSparseVoxelTree64MaterialTable(
      {unsupported}, table, &error));
  assert(error.find("unsupported flags or reserved fields") !=
      std::string::npos);
}

void testPhase7ExactBrickSidecarExtraction()
{
  SparseVoxelBrickHierarchy hierarchy{};
  SparseVoxelBrickNodeGPU node{};
  node.minimumVoxelSize = {-8.0f, 0.0f, 16.0f, 1.0f};
  node.canonicalGridOriginScale = {-8, 0, 16, 1};
  node.regionMappingId = 0x73a91bcdu;
  node.occupancyIndex = 0u;
  node.materialBase = 0u;
  node.paletteBase = 0u;
  hierarchy.nodes.push_back(node);
  SparseVoxelBrickOccupancyGPU occupancy{};
  occupancy.cells[0].bits = 1u;
  hierarchy.occupancy.push_back(occupancy);
  hierarchy.materialIndices.push_back(0u);
  const uint64_t highPrecisionSurface = packSparseVoxelMicroContextData(
      0x7fu, packSparseVoxelMicroSurfacePatch(
                  {3.0 / 63.0, 8.0 / 63.0, 16.0 / 63.0, 0.0}));
  hierarchy.microSurfaceData.push_back(highPrecisionSurface);
  hierarchy.palette.push_back({});
  hierarchy.paletteDescriptorIds.push_back(6u);

  std::vector<SparseVoxelTree64MaterialCellGPU> cells;
  SparseVoxelTree64MaterialBuildStats stats{};
  std::string error;
  assert(collectSparseVoxelTree64MaterialCells(
      hierarchy, {1.0f, 1.0f, 1.0f}, cells, &stats, &error));
  assert(cells.size() == 1u);
  assert((cells[0].globalCell == std::array<int32_t, 3>{-8, 0, 16}));
  assert(cells[0].materialHandle() == 6u);
  assert(cells[0].surfaceData == 0x7f010203u);
  assert(cells[0].regionMappingId == node.regionMappingId);
  assert(stats.finestParents == 1u && stats.occupiedCells == 1u &&
      stats.emittedCells == 1u);

  assert(collectSparseVoxelTree64MaterialCells(
      hierarchy, {2.0f, 2.0f, 2.0f}, cells, &stats, &error));
  assert(cells.empty() && stats.omittedCellSize == 1u);

  hierarchy.nodes[0].minimumVoxelSize[0] = -7.5f;
  assert(collectSparseVoxelTree64MaterialCells(
      hierarchy, {1.0f, 1.0f, 1.0f}, cells, &stats, &error));
  assert(cells.empty() && stats.omittedGridAlignment == 1u);

  hierarchy.nodes[0].minimumVoxelSize[0] = -8.0f;
  hierarchy.nodes[0].canonicalGridOriginScale[0] = -7;
  assert(collectSparseVoxelTree64MaterialCells(
      hierarchy, {1.0f, 1.0f, 1.0f}, cells, &stats, &error));
  assert(cells.empty() && stats.omittedCanonicalKey == 1u);

  hierarchy.nodes[0].canonicalGridOriginScale[0] = -8;
  auto conflictingNode = hierarchy.nodes[0];
  conflictingNode.occupancyIndex = 1u;
  conflictingNode.materialBase = 1u;
  conflictingNode.paletteBase = 1u;
  hierarchy.nodes.push_back(conflictingNode);
  hierarchy.occupancy.push_back(occupancy);
  hierarchy.materialIndices.push_back(0u);
  hierarchy.microSurfaceData.push_back(highPrecisionSurface);
  hierarchy.palette.push_back({});
  hierarchy.paletteDescriptorIds.push_back(9u);
  assert(collectSparseVoxelTree64MaterialCells(
      hierarchy, {1.0f, 1.0f, 1.0f}, cells, &stats, &error));
  assert(cells.empty() && stats.omittedAmbiguousCells == 1u);
}

void testPhase7RayConsumerParityContinuationAndMeasurements()
{
  VoxelGeometryProgram heightfield{};
  heightfield.instructions = {
    {VoxelGeometryOpcode::LoadConstant, 0u, 0u, 0u,
     voxelGeometryFixedFromDouble(0.5)},
    {VoxelGeometryOpcode::OutputRetainedHeight, 0u, 0u},
  };
  std::string error;
  assert(verifyVoxelGeometryProgram(heightfield, &error));

  SparseVoxelTree64MaterialCandidate carved{};
  carved.program = heightfield;
  carved.inputs.subdivision = 4u;
  carved.inputs.parentKey = {2, 3, 4};
  carved.inputs.face = 3u;
  carved.microCell = {1u, 3u, 1u};
  carved.deterministicIdentity = 100u;
  carved.distance = 2.0f;
  SparseVoxelTree64MaterialCandidate retained = carved;
  retained.microCell = {1u, 0u, 1u};
  retained.deterministicIdentity = 200u;
  retained.distance = 3.0f;
  const std::vector<SparseVoxelTree64MaterialCandidate> candidates{
    carved, retained};

  SparseVoxelTree64MaterialReferenceHit reference{};
  for (const SparseVoxelMaterialRayConsumer consumer : {
         SparseVoxelMaterialRayConsumer::Raster,
         SparseVoxelMaterialRayConsumer::Tree64Primary,
         SparseVoxelMaterialRayConsumer::Shadow,
         SparseVoxelMaterialRayConsumer::Probe})
  {
    SparseVoxelTree64MaterialMeasurements measurements{};
    SparseVoxelTree64MaterialReferenceHit hit{};
    assert(traceSparseVoxelTree64MaterialReference(
        consumer, candidates, hit, &measurements, &error));
    assert(hit.hit && hit.candidateIndex == 1u);
    assert(hit.deterministicIdentity == 200u && hit.distance == 3.0f);
    assert(measurements.candidateParents == 2u);
    assert(measurements.carvedParents == 1u);
    assert(measurements.interpreterInstructions == 4u);
    assert(measurements.instructionsPerCandidate() == 2.0);
    if (consumer == SparseVoxelMaterialRayConsumer::Raster)
      reference = hit;
    else
    {
      assert(hit.deterministicIdentity == reference.deterministicIdentity);
      assert(hit.distance == reference.distance);
    }
  }

  // Empty/disabled programs use the original solid-parent specialization:
  // no interpreter work, context load, or incremental material-pass bytes.
  SparseVoxelTree64MaterialCandidate disabled{};
  disabled.deterministicIdentity = 55u;
  SparseVoxelTree64MaterialMeasurements baseline{};
  assert(traceSparseVoxelTree64MaterialReference(
      SparseVoxelMaterialRayConsumer::Shadow, {disabled}, reference,
      &baseline, &error));
  assert(reference.hit && reference.deterministicIdentity == 55u);
  assert(baseline.interpreterInstructions == 0u);
  assert(baseline.contextLoads == 0u);
  assert(sparseVoxelMaterialPassBytesPerPixel(false) == 0u);
  assert(sparseVoxelMaterialPassBytesPerPixel(true) == 4u);
  assert(sparseVoxelMaterialSampleAddressBytesPerPixel() == 16u);
}

void testPhase8BoundedMaterialFieldABI()
{
  const VoxelMaterialSampleAddressGPU sampleAddress =
      voxelMaterialSampleAddress(
          {-2, 3, 4}, {1u, 2u, 7u}, 2u, 8u, 0x91a2b3c4u);
  assert((sampleAddress.canonicalMicroCoordinate ==
      std::array<int32_t, 3>{-14, 28, 46}));
  assert(sampleAddress.regionMappingId == 0x91a2b3c4u);

  VoxelMaterialFieldBindings bindings{};
  bindings[0] = {
    VoxelMaterialFieldSemantic::Grass,
    VoxelMaterialFieldSource::PlanetRegion,
    VoxelMaterialFieldMapping::PlanetSurface,
    VoxelMaterialFieldFilter::Linear,
    VoxelMaterialFieldAddressMode::Clamp,
    0.25f, 1.0f, 0.0f, {}};
  bindings[1] = {
    VoxelMaterialFieldSemantic::Variation,
    VoxelMaterialFieldSource::MaterialTexture,
    VoxelMaterialFieldMapping::CanonicalWorld,
    VoxelMaterialFieldFilter::Linear,
    VoxelMaterialFieldAddressMode::Repeat,
    0.5f, 0.75f, 0.1f, "textures/soil-variation"};
  std::string error;
  assert(validateVoxelMaterialFieldBindings(bindings, &error));

  VoxelMaterialFieldBindings invalidBindings = bindings;
  invalidBindings[0].defaultValue = 1.01f;
  assert(!validateVoxelMaterialFieldBindings(invalidBindings, &error));
  invalidBindings = bindings;
  invalidBindings[1].textureAssetId.clear();
  assert(!validateVoxelMaterialFieldBindings(invalidBindings, &error));
  invalidBindings = bindings;
  invalidBindings[2].source = VoxelMaterialFieldSource::PlanetRegion;
  assert(!validateVoxelMaterialFieldBindings(invalidBindings, &error));

  const VoxelShadingProgram shading{{
    {VoxelShadingOpcode::LoadField, 0u, 0u},
    {VoxelShadingOpcode::LoadField, 1u, 1u},
    {VoxelShadingOpcode::Multiply, 0u, 0u, 1u},
    {VoxelShadingOpcode::OutputLayerWeight, 0u, 0u},
  }};
  assert(verifyVoxelShadingProgram(shading, &error));
  assert(voxelShadingRequiredMaterialFields(shading) == 0x3u);
  std::vector<VoxelShadingInstructionGPU> shadingGPU;
  assert(compileVoxelShadingProgram(shading, shadingGPU, &error));
  assert((shadingGPU[0].opcodeOperands & 0xffu) ==
      static_cast<uint32_t>(VoxelShadingOpcode::LoadField));
  VoxelShadingInputs shadingInputs{};
  shadingInputs.fields[0] = 0.8f;
  shadingInputs.fields[1] = 0.25f;
  VoxelShadingResult shadingResult{};
  assert(evaluateVoxelShadingProgram(
      shading, shadingInputs, shadingResult, &error));
  assert(std::abs(shadingResult.layerWeight - 0.2f) < 1.0e-6f);

  VoxelShadingProgram invalidShading{{
    {VoxelShadingOpcode::LoadField, 0u, kVoxelMaterialFieldCount},
    {VoxelShadingOpcode::OutputLayerWeight, 0u, 0u},
  }};
  assert(!verifyVoxelShadingProgram(invalidShading, &error));
  invalidShading.instructions = {
    {VoxelShadingOpcode::LoadField, 0u, 0u},
    {VoxelShadingOpcode::LoadField, 1u, 1u},
    {VoxelShadingOpcode::LoadField, 2u, 2u},
    {VoxelShadingOpcode::OutputLayerWeight, 0u, 0u},
  };
  assert(!verifyVoxelShadingProgram(invalidShading, &error));

  VoxelGeometryProgram geometry{};
  geometry.instructions = {
    {VoxelGeometryOpcode::LoadField, 0u, 3u},
    {VoxelGeometryOpcode::OutputCarveDepth, 0u, 0u},
  };
  assert(verifyVoxelGeometryProgram(geometry, &error));
  assert(voxelGeometryRequiredMaterialFields(geometry) == 0x8u);
  VoxelGeometryInputs geometryInputs{};
  geometryInputs.fields[3] = voxelGeometryFixedFromDouble(0.25);
  VoxelGeometryResult geometryResult{};
  assert(evaluateVoxelGeometryProgram(
      geometry, geometryInputs, geometryResult, &error));
  assert(geometryResult.carveDepth == voxelGeometryFixedFromDouble(0.25));
  assert(geometryResult.retainedHeight == voxelGeometryFixedFromDouble(0.75));
  VoxelGeometryCompilation geometryGPU{};
  assert(compileVoxelGeometryProgram(
      geometry, 0u, geometryGPU, &error));
  assert(evaluateCompiledVoxelGeometryProgram(
      geometryGPU.gpu, geometryGPU.instructions, geometryInputs,
      geometryResult, &error));
  assert(geometryResult.carveDepth == voxelGeometryFixedFromDouble(0.25));
  geometry.instructions[0].source0 = kVoxelMaterialFieldCount;
  assert(!verifyVoxelGeometryProgram(geometry, &error));
  geometry.instructions[0].source0 = 3u;

  VoxelMaterialAsset asset{};
  asset.assetId = "phase8/bounded-fields";
  asset.name = "Bounded fields";
  const SparseVoxelMicroMaterialGPU packed = packedMaterial(808u);
  asset.geometry = unpackSparseVoxelMicroGeometry(packed);
  asset.legacyShading = unpackSparseVoxelMicroShading(packed);
  asset.fieldBindings = bindings;
  asset.shadingProgram = shading;
  assert(asset.validate(&error));
  const uint64_t originalHash = asset.contentHash();
  std::vector<uint8_t> bytes;
  assert(serializeVoxelMaterialAsset(asset, bytes, &error));
  VoxelMaterialAsset decoded{};
  assert(deserializeVoxelMaterialAsset(bytes, decoded, &error));
  assert(decoded.version == kVoxelMaterialAssetVersion);
  assert(decoded.fieldBindings == bindings);
  assert(decoded.contentHash() == originalHash);
  decoded.fieldBindings[0].defaultValue = 0.75f;
  assert(decoded.contentHash() != originalHash);

  VoxelMaterialManager manager;
  VoxelMaterialDefinition definition{};
  assert(asset.definition(definition, &error));
  VoxelMaterialHandle handle = kInvalidVoxelMaterialHandle;
  assert(manager.registerMaterial(definition, handle, &error));
  assert(manager.slot(handle).fieldBindings == bindings);
  assert(manager.materialFieldDefaults().size() == manager.slotCount());
  assert((manager.shadingPrograms()[handle].flags &
      kVoxelShadingProgramMaterialFieldMask) ==
      (0x3u << kVoxelShadingProgramMaterialFieldShift));
  assert(manager.materialFieldDefaults()[handle].values[0] == 16384u);
  assert(manager.materialFieldDefaults()[handle].values[1] == 32768u);
  const auto fixedDefaults = voxelMaterialFieldDefaultsFixed(
      manager.materialFieldDefaults()[handle]);
  const auto floatDefaults = voxelMaterialFieldDefaultsFloat(
      manager.materialFieldDefaults()[handle]);
  assert(fixedDefaults[0] == voxelGeometryFixedFromDouble(0.25));
  assert(std::abs(floatDefaults[1] - 0.5f) < 1.0e-6f);
  shadingInputs.fields = floatDefaults;
  assert(evaluateVoxelShadingProgram(
      shading, shadingInputs, shadingResult, &error));
  assert(std::abs(shadingResult.layerWeight - 0.125f) < 1.0e-6f);
  const uint64_t shadingRevision = manager.shadingRevision();
  VoxelMaterialChange changes = VoxelMaterialChange::None;
  bindings[0].defaultValue = 0.75f;
  assert(manager.updateFieldBindings(handle, bindings, changes, &error));
  assert(changes == VoxelMaterialChange::Shading);
  assert(manager.shadingRevision() == shadingRevision + 1u);
  assert(manager.slot(handle).fieldBindings[0].defaultValue == 0.75f);
  assert(manager.materialFieldDefaults()[handle].values[0] == 49152u);

  bindings[3] = {
    VoxelMaterialFieldSemantic::Carve,
    VoxelMaterialFieldSource::Constant,
    VoxelMaterialFieldMapping::ParentLocal,
    VoxelMaterialFieldFilter::Nearest,
    VoxelMaterialFieldAddressMode::Clamp,
    0.25f, 1.0f, 0.0f, {}};
  assert(manager.updateFieldBindings(handle, bindings, changes, &error));
  assert(changes == VoxelMaterialChange::None);
  assert(manager.updateGeometryProgram(handle, geometry, changes, &error));
  assert(changes == VoxelMaterialChange::Geometry);
  const uint64_t geometryRevision = manager.geometryRevision();
  bindings[3].defaultValue = 0.5f;
  assert(manager.updateFieldBindings(handle, bindings, changes, &error));
  assert(changes == VoxelMaterialChange::Geometry);
  assert(manager.geometryRevision() == geometryRevision + 1u);
  assert(manager.materialFieldDefaults()[handle].values[3] == 32768u);
  assert((manager.geometryPrograms()[handle].executionFlags &
      kVoxelGeometryProgramMaterialFieldMask) ==
      (0x8u << kVoxelGeometryProgramMaterialFieldShift));

  VoxelMaterialAsset undeclared = asset;
  undeclared.assetId = "phase8/undeclared-field";
  undeclared.fieldBindings[0] = {};
  assert(!undeclared.validate(&error));
  assert(error.find("undeclared material field") != std::string::npos);
}

void testPhase8SharedFieldAtlasAndBoundedRegionMapping()
{
  VoxelMaterialFieldBindings bindings{};
  bindings[0] = {
    VoxelMaterialFieldSemantic::Grass,
    VoxelMaterialFieldSource::PlanetRegion,
    VoxelMaterialFieldMapping::PlanetSurface,
    VoxelMaterialFieldFilter::Linear,
    VoxelMaterialFieldAddressMode::Clamp,
    0.25f, 1.0f, 0.0f, {}};
  bindings[1] = {
    VoxelMaterialFieldSemantic::Variation,
    VoxelMaterialFieldSource::MaterialTexture,
    VoxelMaterialFieldMapping::CanonicalWorld,
    VoxelMaterialFieldFilter::Nearest,
    VoxelMaterialFieldAddressMode::Repeat,
    0.5f, 0.75f, 0.1f, "textures/soil-variation"};
  const VoxelShadingProgram program{{
    {VoxelShadingOpcode::LoadField, 0u, 0u},
    {VoxelShadingOpcode::LoadField, 1u, 1u},
    {VoxelShadingOpcode::Multiply, 0u, 0u, 1u},
    {VoxelShadingOpcode::OutputLayerWeight, 0u, 0u},
  }};
  VoxelMaterialDefinition definition{};
  definition.assetId = "phase8/shared-atlas";
  definition.name = "Shared atlas";
  definition.descriptor = packedMaterial(812u);
  definition.shadingProgram = program;
  definition.fieldBindings = bindings;
  VoxelMaterialManager manager;
  VoxelMaterialHandle handle = kInvalidVoxelMaterialHandle;
  std::string error;
  assert(manager.registerMaterial(definition, handle, &error));

  VoxelMaterialFieldTile materialTile{};
  materialTile.values.resize(
      kVoxelMaterialFieldAtlasTileSide *
          kVoxelMaterialFieldAtlasTileSide);
  for (uint32_t y = 0u; y < kVoxelMaterialFieldAtlasTileSide; ++y)
    for (uint32_t x = 0u; x < kVoxelMaterialFieldAtlasTileSide; ++x)
      materialTile.values[y * kVoxelMaterialFieldAtlasTileSide + x] =
          static_cast<uint8_t>(x);
  VoxelMaterialFieldTile regionTile{};
  regionTile.canonicalOrigin = {-32, -32};
  regionTile.canonicalExtent = {64u, 64u};
  regionTile.values.resize(
      kVoxelMaterialFieldAtlasTileSide *
          kVoxelMaterialFieldAtlasTileSide);
  for (uint32_t y = 0u; y < kVoxelMaterialFieldAtlasTileSide; ++y)
    for (uint32_t x = 0u; x < kVoxelMaterialFieldAtlasTileSide; ++x)
      regionTile.values[y * kVoxelMaterialFieldAtlasTileSide + x] =
          static_cast<uint8_t>(x + y);
  constexpr uint32_t regionMappingId = 0x13579bdfu;
  VoxelMaterialFieldAtlas atlas;
  assert(atlas.setTextureAsset(
      "textures/soil-variation", materialTile, &error));
  assert(atlas.setPlanetRegionField(
      handle, regionMappingId, 0u, regionTile, &error));

  VoxelMaterialFieldAtlasSnapshot snapshot{};
  assert(atlas.build(manager, snapshot, &error));
  assert(snapshot.tileSide == kVoxelMaterialFieldAtlasTileSide);
  assert(snapshot.resolvers.size() == manager.slotCount());
  assert(snapshot.mappingTable.size() >= 4u);
  assert(snapshot.rgba8.size() ==
      static_cast<size_t>(snapshot.width) * snapshot.height * 4u);
  uint32_t probes = 0u;
  const auto *global = findVoxelMaterialFieldMapping(
      snapshot.mappingTable, handle, 0u, &probes);
  assert(global != nullptr && global->channelMask() == 0x2u);
  assert(probes <= kVoxelMaterialFieldMappingMaximumProbes);
  const auto *region = findVoxelMaterialFieldMapping(
      snapshot.mappingTable, handle, regionMappingId, &probes);
  assert(region != nullptr && region->channelMask() == 0x1u);
  assert(findVoxelMaterialFieldMapping(
      snapshot.mappingTable, handle, 0x2468ace0u) == nullptr);

  VoxelMaterialSampleAddressGPU address{};
  address.canonicalMicroCoordinate = {-1, 7, 8};
  address.regionMappingId = regionMappingId;
  const auto sampled = sampleVoxelMaterialFields(
      snapshot, manager.materialFieldDefaults()[handle], handle, 8u,
      address, voxelShadingRequiredMaterialFields(program));
  // Region linear filtering maps logical (31,40) into atlas (15.25,19.75).
  // The x+y ramp therefore evaluates to exactly 35. Material repeat maps the
  // canonical x=-1 address to texel 31.
  assert(std::abs(sampled[0] - 35.0f / 255.0f) < 1.0e-6f);
  assert(std::abs(sampled[1] -
      (31.0f / 255.0f * 0.75f + 0.1f)) < 1.0e-6f);
  VoxelShadingInputs inputs{};
  inputs.fields = sampled;
  VoxelShadingResult result{};
  assert(evaluateVoxelShadingProgram(program, inputs, result, &error));
  assert(std::abs(result.layerWeight - sampled[0] * sampled[1]) <
      1.0e-6f);

  address.regionMappingId = 0x2468ace0u;
  const auto missingRegion = sampleVoxelMaterialFields(
      snapshot, manager.materialFieldDefaults()[handle], handle, 8u,
      address, 0x1u);
  assert(std::abs(missingRegion[0] - 0.25f) < 1.0e-6f);
  // The required mask prevents an otherwise resident texture from being read.
  assert(std::abs(missingRegion[1] - 0.5f) < 1.0e-6f);

  VoxelMaterialFieldTile invalid = materialTile;
  invalid.width = 16u;
  assert(!atlas.setTextureAsset("textures/invalid", invalid, &error));
  assert(error.find("32x32") != std::string::npos);
  assert(!atlas.setPlanetRegionField(
      handle, 0u, 0u, regionTile, &error));
}

void testPhase8GeometryFieldCacheAndConservativeResidency()
{
  VoxelGeometryProgram geometry{};
  geometry.executionHint = VoxelGeometryExecutionHint::Direct;
  geometry.instructions = {
    {VoxelGeometryOpcode::LoadField, 0u, 0u},
    {VoxelGeometryOpcode::OutputCarveDepth, 0u, 0u},
  };
  VoxelGeometryCompilation compilation{};
  std::string error;
  assert(compileVoxelGeometryProgram(geometry, 0u, compilation, &error));
  assert(compilation.execution == VoxelGeometryExecution::CachedHeightfield);

  VoxelMaterialDefinition definition{};
  definition.assetId = "phase8/cached-carve";
  definition.name = "Cached carve";
  definition.descriptor = packedMaterial(913u);
  definition.geometryProgram = geometry;
  definition.fieldBindings[0] = {
    VoxelMaterialFieldSemantic::Carve,
    VoxelMaterialFieldSource::MaterialTexture,
    VoxelMaterialFieldMapping::ParentLocal,
    VoxelMaterialFieldFilter::Nearest,
    VoxelMaterialFieldAddressMode::Clamp,
    0.0f, 1.0f, 0.0f, "textures/carve-mask"};
  VoxelMaterialManager manager;
  VoxelMaterialHandle handle = kInvalidVoxelMaterialHandle;
  assert(manager.registerMaterial(definition, handle, &error));

  VoxelMaterialDefinition invalid = definition;
  invalid.assetId = "phase8/linear-carve";
  invalid.fieldBindings[0].filter = VoxelMaterialFieldFilter::Linear;
  VoxelMaterialHandle invalidHandle = kInvalidVoxelMaterialHandle;
  assert(!manager.registerMaterial(invalid, invalidHandle, &error));
  assert(error.find("fixed-mip nearest") != std::string::npos);

  VoxelMaterialFieldTile tile{};
  tile.values.resize(
      kVoxelMaterialFieldAtlasTileSide *
      kVoxelMaterialFieldAtlasTileSide);
  for (uint32_t y = 0u; y < kVoxelMaterialFieldAtlasTileSide; ++y)
    for (uint32_t x = 0u; x < kVoxelMaterialFieldAtlasTileSide; ++x)
      tile.values[y * kVoxelMaterialFieldAtlasTileSide + x] =
          x < 13u ? 0u : 255u;
  VoxelMaterialFieldAtlas atlas;
  assert(atlas.setTextureAsset("textures/carve-mask", tile, &error));
  VoxelMaterialFieldAtlasSnapshot atlasSnapshot{};
  assert(atlas.build(manager, atlasSnapshot, &error));

  VoxelMaterialSampleAddressGPU fixedAddress{};
  // Parent-local coordinates repeat at every parent even when the canonical
  // parent is far from the world origin. 42 == 8 * 5 + local cell 2.
  fixedAddress.canonicalMicroCoordinate = {42, 0, 0};
  const auto fixed = sampleVoxelMaterialFieldsFixed(
      atlasSnapshot, manager.materialFieldDefaults()[handle], handle, 5u,
      fixedAddress, 0x1u);
  assert(fixed.residentMask == 0x1u);
  assert(fixed.values[0] == kVoxelGeometryFixedOne);

  SparseVoxelBrickHierarchy hierarchy{};
  SparseVoxelBrickNodeGPU node{};
  node.minimumVoxelSize = {0.0f, 0.0f, 0.0f, 1.0f};
  node.canonicalGridOriginScale = {8, 0, 0, 1};
  node.occupancyIndex = 0u;
  node.materialBase = 0u;
  node.paletteBase = 0u;
  node.occupiedCount = 1u;
  hierarchy.nodes.push_back(node);
  SparseVoxelBrickOccupancyGPU occupancy{};
  occupancy.cells[0].bits = 1u;
  hierarchy.occupancy.push_back(occupancy);
  hierarchy.materialIndices.push_back(0u);
  hierarchy.microSurfaceData.push_back(packSparseVoxelMicroContextData(
      sparseVoxelMicroSurfaceContext(0u),
      packSparseVoxelMicroSurfacePatch({1.0, 1.0, 1.0, 1.0})));
  hierarchy.palette.push_back({});
  hierarchy.paletteDescriptorIds.push_back(handle);
  hierarchy.paletteStride = 1u;

  SparseVoxelGeometryFieldCacheSnapshot cache{};
  SparseVoxelGeometryFieldCacheSnapshot repeated{};
  assert(buildSparseVoxelGeometryFieldCache(
      hierarchy, manager, atlasSnapshot, cache, &error));
  assert(buildSparseVoxelGeometryFieldCache(
      hierarchy, manager, atlasSnapshot, repeated, &error));
  assert(cache.table == repeated.table);
  assert(cache.contentRevision == repeated.contentRevision);
  assert(cache.recordCount == 1u && cache.conservativeRecordCount == 0u);
  uint32_t probes = 0u;
  const auto *record = findSparseVoxelGeometryFieldCacheRecord(
      cache.table, {8, 0, 0}, handle, &probes);
  assert(record != nullptr && !record->conservative());
  assert(probes <= kSparseVoxelGeometryFieldCacheMaximumProbes);
  bool found = false;
  assert(sparseVoxelGeometryFieldCacheOccupied(
      cache.table, {8, 0, 0}, handle, {0u, 2u, 0u}, &found));
  assert(found);
  assert(!sparseVoxelGeometryFieldCacheOccupied(
      cache.table, {8, 0, 0}, handle, {3u, 2u, 0u}, &found));
  for (const SparseVoxelMaterialRayConsumer consumer : {
         SparseVoxelMaterialRayConsumer::Raster,
         SparseVoxelMaterialRayConsumer::Tree64Primary,
         SparseVoxelMaterialRayConsumer::Shadow,
         SparseVoxelMaterialRayConsumer::Probe})
  {
    (void)consumer;
    assert(!sparseVoxelGeometryFieldCacheOccupied(
        cache.table, {8, 0, 0}, handle, {3u, 2u, 0u}));
  }
  assert(sparseVoxelGeometryFieldCacheOccupied(
      cache.table, {99, 0, 0}, handle, {3u, 2u, 0u}, &found));
  assert(!found);

  tile.values.assign(tile.values.size(), 0u);
  assert(atlas.setTextureAsset("textures/carve-mask", tile, &error));
  VoxelMaterialFieldAtlasSnapshot changedAtlas{};
  assert(atlas.build(manager, changedAtlas, &error));
  SparseVoxelGeometryFieldCacheSnapshot changed{};
  assert(buildSparseVoxelGeometryFieldCache(
      hierarchy, manager, changedAtlas, changed, &error));
  assert(changed.contentRevision != cache.contentRevision);
  assert(sparseVoxelGeometryFieldCacheOccupied(
      changed.table, {8, 0, 0}, handle, {3u, 2u, 0u}));

  atlas.clearTextureAsset("textures/carve-mask");
  VoxelMaterialFieldAtlasSnapshot missingAtlas{};
  assert(atlas.build(manager, missingAtlas, &error));
  SparseVoxelGeometryFieldCacheSnapshot missing{};
  assert(buildSparseVoxelGeometryFieldCache(
      hierarchy, manager, missingAtlas, missing, &error));
  assert(missing.conservativeRecordCount == 1u);
  const auto *missingRecord = findSparseVoxelGeometryFieldCacheRecord(
      missing.table, {8, 0, 0}, handle);
  assert(missingRecord != nullptr && missingRecord->conservative());
  assert(sparseVoxelGeometryFieldCacheOccupied(
      missing.table, {8, 0, 0}, handle, {3u, 2u, 0u}));
}

void testPhase8BoundedFourColourPalette()
{
  VoxelShadingProgram program{{
    {VoxelShadingOpcode::LoadConstant, 0u, 0u, 0u, 0.25f},
    {VoxelShadingOpcode::LoadField, 1u, 0u},
    {VoxelShadingOpcode::LoadField, 2u, 1u},
    {VoxelShadingOpcode::OutputLayerWeight, 0u, 0u},
    {VoxelShadingOpcode::OutputPaletteWeight2, 0u, 1u},
    {VoxelShadingOpcode::OutputPaletteWeight3, 0u, 2u},
  }};
  std::string error;
  assert(verifyVoxelShadingProgram(program, &error));
  std::vector<VoxelShadingInstructionGPU> compiled;
  assert(compileVoxelShadingProgram(program, compiled, &error));
  assert((compiled[4].opcodeOperands & 0xffu) ==
      static_cast<uint32_t>(VoxelShadingOpcode::OutputPaletteWeight2));

  VoxelShadingResult result{};
  VoxelShadingInputs paletteInputs{};
  paletteInputs.fields = {0.5f, 1.0f, 0.0f, 0.0f};
  assert(evaluateVoxelShadingProgram(
      program, paletteInputs, result, &error));
  const float inverseSeven = 1.0f / 7.0f;
  assert(std::abs(result.paletteWeights[0]) < 1.0e-6f);
  assert(std::abs(result.paletteWeights[1] - inverseSeven) < 1.0e-6f);
  assert(std::abs(result.paletteWeights[2] - 2.0f * inverseSeven) < 1.0e-6f);
  assert(std::abs(result.paletteWeights[3] - 4.0f * inverseSeven) < 1.0e-6f);
  assert(std::abs(result.layerWeight - result.paletteWeights[1]) < 1.0e-6f);
  const auto colour = blendVoxelShadingPalette(
      {{{1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
        {1.0f, 1.0f, 1.0f}}},
      result);
  assert(std::abs(colour[0] - 4.0f * inverseSeven) < 1.0e-6f);
  assert(std::abs(colour[1] - 5.0f * inverseSeven) < 1.0e-6f);
  assert(std::abs(colour[2] - 6.0f * inverseSeven) < 1.0e-6f);

  const VoxelShadingProgram legacy{{
    {VoxelShadingOpcode::LoadConstant, 0u, 0u, 0u, 0.25f},
    {VoxelShadingOpcode::OutputLayerWeight, 0u, 0u},
  }};
  assert(evaluateVoxelShadingProgram(legacy, {}, result, &error));
  assert((result.paletteWeights ==
      std::array<float, 4>{0.75f, 0.25f, 0.0f, 0.0f}));

  VoxelShadingProgram duplicate = program;
  duplicate.instructions.push_back(
      {VoxelShadingOpcode::OutputPaletteWeight2, 0u, 1u});
  assert(!verifyVoxelShadingProgram(duplicate, &error));
  assert(error.find("duplicate palette output") != std::string::npos);

  auto presets = defaultTerrainVoxelMaterialPresets(false);
  VoxelMaterialAsset asset = terrainVoxelMaterialAsset(3u, presets[3u]);
  asset.surface.tertiaryAlbedoRGBA8 = 0xff8a8f95u;
  asset.surface.quaternaryAlbedoRGBA8 = 0xfff4f7f8u;
  std::vector<uint8_t> bytes;
  assert(serializeVoxelMaterialAsset(asset, bytes, &error));
  VoxelMaterialAsset decoded{};
  assert(deserializeVoxelMaterialAsset(bytes, decoded, &error));
  assert(decoded.surface == asset.surface);

  // Version-two files ended after the four field bindings. Removing the new
  // eight-byte palette tail produces a real legacy payload and verifies that
  // both auxiliary entries migrate to their white, unused defaults.
  std::vector<uint8_t> versionTwo = bytes;
  versionTwo.resize(versionTwo.size() - 2u * sizeof(uint32_t));
  const auto writeLittleU32 = [](std::vector<uint8_t> &target,
                                 const size_t offset,
                                 const uint32_t value)
  {
    for (uint32_t byte = 0u; byte < 4u; ++byte)
      target[offset + byte] = static_cast<uint8_t>(value >> (byte * 8u));
  };
  const auto writeLittleU64 = [](std::vector<uint8_t> &target,
                                 const size_t offset,
                                 const uint64_t value)
  {
    for (uint32_t byte = 0u; byte < 8u; ++byte)
      target[offset + byte] = static_cast<uint8_t>(value >> (byte * 8u));
  };
  writeLittleU32(versionTwo, 12u, 2u);
  writeLittleU64(versionTwo, 16u, versionTwo.size() - 32u);
  uint64_t checksum = 1469598103934665603ull;
  for (size_t index = 32u; index < versionTwo.size(); ++index)
  {
    checksum ^= versionTwo[index];
    checksum *= 1099511628211ull;
  }
  writeLittleU64(versionTwo, 24u, checksum);
  VoxelMaterialAsset migrated{};
  assert(deserializeVoxelMaterialAsset(versionTwo, migrated, &error));
  assert(migrated.surface.tertiaryAlbedoRGBA8 == 0xffffffffu);
  assert(migrated.surface.quaternaryAlbedoRGBA8 == 0xffffffffu);

  VoxelMaterialDefinition definition{};
  assert(decoded.definition(definition, &error));
  assert(definition.shadingPalette.additionalAlbedoRGBA8[0] ==
      asset.surface.tertiaryAlbedoRGBA8);
  assert(definition.shadingPalette.additionalAlbedoRGBA8[1] ==
      asset.surface.quaternaryAlbedoRGBA8);
  VoxelMaterialManager manager;
  VoxelMaterialHandle handle = kInvalidVoxelMaterialHandle;
  assert(manager.registerMaterial(definition, handle, &error));
  assert(manager.shadingPalettes()[handle] == definition.shadingPalette);
  VoxelMaterialChange changes = VoxelMaterialChange::None;
  assert(manager.updateShadingProgram(handle, program, changes, &error));
  assert((manager.shadingPrograms()[handle].flags &
      kVoxelShadingProgramPaletteMask) ==
      (0x3u << kVoxelShadingProgramPaletteShift));
  const uint64_t geometryRevision = manager.geometryRevision();
  const uint64_t shadingRevision = manager.shadingRevision();
  VoxelShadingPaletteGPU edited = definition.shadingPalette;
  edited.additionalAlbedoRGBA8[1] = 0xffdce8f2u;
  assert(manager.updateShadingPalette(handle, edited, changes, &error));
  assert(changes == VoxelMaterialChange::Shading);
  assert(manager.geometryRevision() == geometryRevision);
  assert(manager.shadingRevision() == shadingRevision + 1u);
}

} // namespace

int main()
{
  testContinuousLandformHeight();
  testWorldMapBiomeMixtureAndLandformDetail();
  testShadingIRVerificationAndEvaluation();
  testStableShadingProgramSlots();
  testStableEditableSlots();
  testImmutableSharing();
  testIndependentRevisionsAndInvalidation();
  testLegacyLibraryAdapterAndValidation();
  testBrickPaletteUsesStableMaterialHandles();
  testContextDoesNotSplitStableMaterialPalette();
  testAllLegacyProfilesCompileToEquivalentBuiltIns();
  testFirstClassAssetsAndAuthoredTerrainDefaults();
  testInvalidLiveProgramPreservesResidentSlotAndFallsBackSafely();
  testOptionalResolvedShadingContextABI();
  testPhaseZeroGrassShadingSignature();
  testLiveShadingParametersUseStableSlots();
  testGeometryIRVerificationExecutionAndTerrainComposition();
  testGeometryIRWorldSeamsVolumeCacheAndStableCoverage();
  testGeometryProgramsRetainHandlesAndInvalidateTemporalHistory();
  testPhase5ContextInputsAndVolumeCSG();
  testPhase5DepthSlopeAndReferenceFixtures();
  testPhase5VolumeTraversalMatchesBruteForceCells();
  testPhase5OptionalContextPoolAndSelectivePageRebuilds();
  testPhase5UnavailableContextDiagnostics();
  testPhase6AssetRoundTripCompilerCacheAndCompatibility();
  testPhase6StableHotReloadAndGraphSelection();
  testPhase6NodeDiagnosticsAndDebugABI();
  testPhase7Tree64MaterialSidecarAndBoundedLookup();
  testPhase7ExactBrickSidecarExtraction();
  testPhase7RayConsumerParityContinuationAndMeasurements();
  testPhase8BoundedMaterialFieldABI();
  testPhase8SharedFieldAtlasAndBoundedRegionMapping();
  testPhase8GeometryFieldCacheAndConservativeResidency();
  testPhase8BoundedFourColourPalette();
  std::cout << "Voxel material tests passed\n";
  return 0;
}
