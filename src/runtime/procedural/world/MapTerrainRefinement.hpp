#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world
{

enum class GroundSubstrate : uint8_t
{
  None = 0u,
  Bedrock,
  Scree,
  Regolith,
  Alluvium,
  Sand,
  ClayLoam,
  Peat,
  SaltCrust,
  GlacialTill,
  Count,
};

struct TerrainRefinementSettings
{
  double maximumIncisionMetres = 85.0;
  double maximumDepositionMetres = 28.0;
  double drainageExponent = 0.55;
  double slopeExponent = 0.80;
};

struct TerrainRefinementInput
{
  uint32_t side = 0u;
  double extentMetres = 0.0;
  double seaLevelMetres = 0.0;
  std::vector<double> elevationMetres;
  std::vector<float> moisture;
  std::vector<uint8_t> biome;
  // 0 land, 1 ocean, 2 lake.
  std::vector<uint8_t> waterType;
};

struct TerrainRefinementMaps
{
  uint32_t side = 0u;
  std::vector<double> elevationMetres;
  std::vector<float> wear;
  std::vector<float> flow;
  std::vector<float> deposition;
  std::vector<float> soilDepthMetres;
  std::vector<uint8_t> substrate;
  // Bedrock, regolith, alluvium and organic fractions; every land texel sums
  // to 255 so the runtime can blend layers without another normalization map.
  std::vector<std::array<uint8_t, 4>> soilLayers;
};

struct TerrainRefinementReport
{
  double erodedVolumeCubicMetres = 0.0;
  double depositedVolumeCubicMetres = 0.0;
  double sedimentExportedVolumeCubicMetres = 0.0;
  double massBalanceRelativeError = 0.0;
  double maximumFlowCells = 0.0;
};

// Deterministic raster refinement used for map-export guidance and as the
// parent condition for future high-resolution regional simulation. Priority
// flooding guarantees a receiver for every land texel; stream-power incision,
// capacity-limited deposition and substrate classification then share that
// drainage graph. It is intentionally bounded and does not replace metre-scale
// erosion inside a loaded terrain node.
bool refineTerrainRaster(
    const TerrainRefinementInput &input,
    const TerrainRefinementSettings &settings,
    TerrainRefinementMaps &maps,
    TerrainRefinementReport *report = nullptr,
    std::string *error = nullptr);

} // namespace procedural::world
