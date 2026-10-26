#pragma once
#include <cstdint>
#include <vector>
namespace procedural::geometry
{
struct HydraulicErosionSettings
{
  bool enabled = true;
  double dropletsPerCell = 3.0;
  uint32_t maximumSteps = 60;
  double inertia = 0.12;
  double evaporation = 0.025;
  double erosionRate = 0.25;
  double depositionRate = 0.3;
  // Prevent concentrated particle traffic from drilling raster pits or piling
  // sediment cones while retaining a measurable transport budget.
  double maximumCutMetres = 180.0;
  double maximumDepositMetres = 20.0;
};
struct HydraulicErosionReport
{
  uint64_t droplets = 0;
  double erodedCubicMetres = 0, depositedCubicMetres = 0, exportedCubicMetres = 0;
  double maximumCutMetres = 0;
};
// Deterministic water particles carry sediment downhill. Mask zero freezes a
// texel (plains and water); sediment leaving that domain is recorded as export.
HydraulicErosionReport erodeMountainHydraulics(uint32_t side, double cellMetres,
    uint64_t seed, const std::vector<double> &mask, std::vector<double> &height,
    const HydraulicErosionSettings &settings = {});
}
