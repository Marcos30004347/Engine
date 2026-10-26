#pragma once
#include <cstdint>
#include <functional>
#include <vector>
namespace procedural::geometry
{
// Schott et al., Large-scale terrain authoring through interactive erosion
// simulation, equations 2, 4 and 5. Physical units: metres and model years.
struct StreamPowerSettings
{
  bool enabled = true;
  uint32_t iterations = 1800;
  uint32_t maximumResolution = 1024;
  double erodibility = 0.0001; // n=1, m=1/2
  double diffusion = 1.0; // m^2 / year
  double maximumTimeStep = 250.0;
  double durationYears = 150000.0;
  // Landslide erosion, after McDonald and Cordonnier 2026 eq. 5: ground steeper
  // than the material's angle of repose is unstable and sheds downslope. Their
  // E_l removes mass and hands it to a debris field; with no debris field here
  // the same excess is moved straight to the neighbours below, which conserves
  // mass and is what makes an escarpment retreat and leave scree at its foot.
  // 0 disables. 37 degrees is the paper's default for rock.
  double reposeAngleDegrees = 37.0;
  // Fraction of the unstable excess relaxed per iteration. Above 0.5 the
  // Jacobi update can overshoot and ring.
  double landslideRate = 0.35;
  // Row bands the per-cell passes are split across. 0 selects the hardware
  // concurrency. Bands are fixed and their partials folded in band order,
  // so this changes wall clock and not the eroded field.
  uint32_t workerCount = 0u;
  std::function<void(uint32_t,double)> progress;
};
struct StreamPowerReport
{
  uint32_t iterations = 0, initialPits = 0, finalPits = 0;
  double years = 0, maximumCutMetres = 0, maximumRiseMetres = 0;
  double erodedCubicMetres = 0, upliftCubicMetres = 0;
};
// Exposed separately so the Jacobi drainage approximation can be checked
// against exact upstream accumulation. Outlets absorb flow and never emit it.
void streamPowerDrainageStep(uint32_t side, double cellMetres,
    const std::vector<double> &height, const std::vector<uint8_t> &outlet,
    const std::vector<double> &rain, const std::vector<double> &previous,
    std::vector<double> &next);
StreamPowerReport erodeStreamPower(uint32_t side, double cellMetres,
    const std::vector<double> &uplift, const std::vector<double> &rain,
    const std::vector<double> &hardness, const std::vector<uint8_t> &outlet,
    std::vector<double> &height, const StreamPowerSettings &settings = {});
}
