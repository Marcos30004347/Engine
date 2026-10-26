#include "procedural/world/MapExportGpuCache.hpp"
#include "voxel/svraster/TerrainGPUV2Columns.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#undef NDEBUG
#include <cassert>

namespace
{
namespace world = procedural::world;
namespace svr = voxel::svraster;

std::vector<uint8_t> u16(const uint16_t value)
{
  return {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8u)};
}

std::vector<uint8_t> halfPair(const uint16_t x, const uint16_t y)
{
  return {static_cast<uint8_t>(x), static_cast<uint8_t>(x >> 8u), static_cast<uint8_t>(y), static_cast<uint8_t>(y >> 8u)};
}

std::vector<uint8_t> texel(const std::string &name)
{
  if (name == "refined_elevation" || name == "base_elevation")
    return u16(0u);
  if (name == "water_level" || name == "uplift" || name == "coast_distance" || name == "erosion_deposition")
    return u16(0u);
  if (name == "river" || name == "relief_roughness" || name == "erosion_wear_flow")
    return halfPair(0u, 0u);
  if (name == "water_type" || name == "roads")
    return {0u};
  if (name == "landform_id")
    return {2u};
  if (name == "substrate_id")
    return {3u};
  if (name == "soil_depth")
    return u16(0x3c00u);
  if (name == "biome_mix_ids")
    return {13u, 0u, 0u, 0u};
  if (name == "biome_mix_weights")
    return {255u, 0u, 0u, 0u};
  if (name == "climate")
    return halfPair(0u, 0x3800u);
  if (name == "placement_potential")
    return {48u, 64u, 96u, 32u};
  if (name == "soil_layers")
    return {32u, 128u, 64u, 31u};
  if (name == "region_id")
    return {9u, 0u, 0u, 0u};
  if (name == "rock_potential")
    return {64u, 128u, 32u, 16u};
  if (name == "exposed_bedrock")
    return u16(0x3800u);
  if (name == "vegetation_potential")
    return {10u, 20u, 30u, 40u};
  if (name == "canopy_density")
    return u16(0x3400u);
  if (name == "ground_density")
    return u16(0x3800u);
  if (name == "riparian_mask")
    return u16(0x3c00u);
  return {};
}

world::MapExportField field(const std::string &name, const world::MapExportFormat format, const world::MapExportFilter filter, const bool available = true, const std::string &group = {})
{
  world::MapExportField result{};
  result.name = name;
  result.format = format;
  result.filter = filter;
  result.available = available;
  result.width = 8u;
  result.height = 8u;
  result.samplingGroup = group;
  if (!available)
    return result;
  if (name == "refined_elevation")
    result.offset[0] = 320.0;
  const auto value = texel(name);
  assert(value.size() == world::mapExportTexelBytes(format));
  constexpr uint32_t storedSide = 6u;
  world::MapExportPage page{};
  page.address = {0u, 0u, 0u};
  page.width = storedSide;
  page.height = storedSide;
  page.data.resize(uint64_t{storedSide} * storedSide * value.size());
  for (uint32_t pixel = 0u; pixel < storedSide * storedSide; ++pixel)
    std::memcpy(page.data.data() + uint64_t{pixel} * value.size(), value.data(), value.size());
  result.pages.push_back(std::move(page));
  return result;
}

world::MapExportBundle bundle()
{
  world::MapExportBundle result{};
  result.resolution = 8u;
  result.pageSizePixels = 4u;
  result.gutterPixels = 1u;
  result.maximumPageLevel = 1u;
  result.extentMetres = 64.0;
  result.seed = 8u;
  result.bundleSignature = 88u;
  result.fields = {
    field("refined_elevation", world::MapExportFormat::R16Unorm, world::MapExportFilter::Linear),
    field("base_elevation", world::MapExportFormat::R16Unorm, world::MapExportFilter::Linear),
    field("uplift", world::MapExportFormat::R16Float, world::MapExportFilter::Linear),
    field("relief_roughness", world::MapExportFormat::RG16Float, world::MapExportFilter::Linear),
    field("landform_id", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest),
    field("water_type", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest),
    field("water_level", world::MapExportFormat::R16Float, world::MapExportFilter::Linear),
    field("river", world::MapExportFormat::RG16Float, world::MapExportFilter::Linear),
    field("coast_distance", world::MapExportFormat::R16Float, world::MapExportFilter::Linear),
    field("erosion_wear_flow", world::MapExportFormat::RG16Float, world::MapExportFilter::Linear),
    field("erosion_deposition", world::MapExportFormat::R16Float, world::MapExportFilter::Linear),
    field("substrate_id", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest),
    field("soil_depth", world::MapExportFormat::R16Float, world::MapExportFilter::Linear),
    field("biome_mix_ids", world::MapExportFormat::RGBA8Uint, world::MapExportFilter::SparseMixture, true, "biome"),
    field("biome_mix_weights", world::MapExportFormat::RGBA8Unorm, world::MapExportFilter::SparseMixture, true, "biome"),
    field("climate", world::MapExportFormat::RG16Float, world::MapExportFilter::Linear),
    field("placement_potential", world::MapExportFormat::RGBA8Unorm, world::MapExportFilter::Linear),
    field("soil_layers", world::MapExportFormat::RGBA8Unorm, world::MapExportFilter::Linear),
    field("roads", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest),
    field("region_id", world::MapExportFormat::R32Uint, world::MapExportFilter::Nearest),
    field("rock_potential", world::MapExportFormat::RGBA8Unorm, world::MapExportFilter::Linear),
    field("rock_type", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest, false),
    field("exposed_bedrock", world::MapExportFormat::R16Float, world::MapExportFilter::Linear),
    field("vegetation_potential", world::MapExportFormat::RGBA8Unorm, world::MapExportFilter::Linear),
    field("vegetation_type", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest, false),
    field("canopy_density", world::MapExportFormat::R16Float, world::MapExportFilter::Linear),
    field("ground_density", world::MapExportFormat::R16Float, world::MapExportFilter::Linear),
    field("riparian_mask", world::MapExportFormat::R16Float, world::MapExportFilter::Linear),
  };
  return result;
}

const svr::TerrainGPUV2SemanticFieldSample *sampleByName(const svr::TerrainGPUV2SemanticSampleReport &report, const std::string &name)
{
  const auto found = std::find_if(
      report.fields.begin(),
      report.fields.end(),
      [&](const auto &sample)
      {
        return sample.fieldName == name;
      });
  return found == report.fields.end() ? nullptr : &*found;
}

void testPhase8FieldsBindAndSampleWhenAvailable()
{
  const auto source = bundle();
  world::MapExportGpuPageLayout layout;
  std::string error;
  assert(layout.initialize(source, 64u, &error));
  assert(layout.fieldIndex("rock_potential").has_value());
  assert(layout.fieldIndex("vegetation_potential").has_value());
  assert(layout.fieldIndex("exposed_bedrock").has_value());
  assert(layout.fieldIndex("canopy_density").has_value());
  assert(layout.fieldIndex("ground_density").has_value());
  assert(layout.fieldIndex("riparian_mask").has_value());
  assert(!layout.fieldIndex("rock_type").has_value());
  assert(!layout.fieldIndex("vegetation_type").has_value());

  svr::TerrainGPUV2ColumnBuildSettings settings{};
  settings.key.worldBundleRevision = 88u;
  settings.key.shape = {8u, 8u, 1u, 0u};
  settings.requestedMapLevel = 1u;
  settings.worldExtentCanonical = 64u;
  settings.heightUnitMetres = 1.0f;
  settings.faultCapacity = 64u;
  svr::TerrainGPUV2ColumnConfigGPU config{};
  assert(svr::planTerrainGPUV2Columns(source, layout, settings, config, &error));

  svr::TerrainGPUV2SemanticSampleReport report{};
  std::vector<svr::TerrainGPUV2MapFaultGPU> faults;
  assert(svr::sampleTerrainGPUV2SemanticInputs(source, config, 0.55, 0.25, report, &faults, &error));
  assert(faults.empty());
  const auto *rock = sampleByName(report, "rock_potential");
  const auto *vegetation = sampleByName(report, "vegetation_potential");
  const auto *exposed = sampleByName(report, "exposed_bedrock");
  const auto *canopy = sampleByName(report, "canopy_density");
  const auto *ground = sampleByName(report, "ground_density");
  const auto *riparian = sampleByName(report, "riparian_mask");
  assert(rock != nullptr && vegetation != nullptr && exposed != nullptr && canopy != nullptr && ground != nullptr && riparian != nullptr);
  assert(rock->fallback == svr::TerrainGPUV2SemanticFallbackKind::AncestorPage);
  assert(vegetation->fallback == svr::TerrainGPUV2SemanticFallbackKind::AncestorPage);
  assert(rock->componentCount == 4u && rock->values[0] == 64.0 / 255.0);
  assert(vegetation->componentCount == 4u && vegetation->values[3] == 40.0 / 255.0);
  assert(exposed->componentCount == 1u && exposed->values[0] == 0.5);
  assert(canopy->values[0] == 0.25);
  assert(ground->values[0] == 0.5);
  assert(riparian->values[0] == 1.0);
  assert(sampleByName(report, "rock_type")->fallback == svr::TerrainGPUV2SemanticFallbackKind::UnavailableField);
  assert(sampleByName(report, "vegetation_type")->fallback == svr::TerrainGPUV2SemanticFallbackKind::UnavailableField);
}

} // namespace

int main()
{
  testPhase8FieldsBindAndSampleWhenAvailable();
  std::cout << "Sparse voxel terrain convergence Phase 8 tests passed\n";
  return 0;
}
