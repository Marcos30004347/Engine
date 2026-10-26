// Scalar-only adapter for the existing deterministic particle erosion solver.
#include "json.hpp"
#include "procedural/geometry/TerrainHydraulicErosion.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
using nlohmann::json;
int main(int argc, char **argv)
{
  try
  {
    if (argc != 3)
      throw std::runtime_error("usage: WorldTerrainErosion input.json output.json");
    std::ifstream input(argv[1]);
    json j;
    input >> j;
    const auto side = j.at("side").get<uint32_t>();
    const auto cell = j.at("cellMetres").get<double>();
    auto h = j.at("height").get<std::vector<double>>();
    const auto mask = j.at("mask").get<std::vector<double>>();
    if (side < 4 || side > 2049 || !std::isfinite(cell) || cell <= 0 || h.size() != size_t(side) * side || mask.size() != h.size())
      throw std::runtime_error("invalid erosion dimensions");
    for (size_t i = 0; i < h.size(); ++i)
      if (!std::isfinite(h[i]) || !std::isfinite(mask[i]) || mask[i] < 0 || mask[i] > 1)
        throw std::runtime_error("invalid erosion scalar input");
    procedural::geometry::HydraulicErosionSettings settings;
    settings.dropletsPerCell = 3;
    settings.maximumSteps = 70;
    settings.maximumCutMetres = 7;
    settings.maximumDepositMetres = 2;
    if (j.contains("erosionSettings"))
    {
      const auto &s = j.at("erosionSettings");
      settings.dropletsPerCell = s.value("dropletsPerCell", 3.0);
      settings.maximumSteps = s.value("maximumSteps", 70u);
      settings.maximumCutMetres = s.value("maximumCutMetres", 7.0);
      settings.maximumDepositMetres = s.value("maximumDepositMetres", 2.0);
      if (!std::isfinite(settings.dropletsPerCell) || settings.dropletsPerCell <= 0 || settings.dropletsPerCell > 32 ||
          settings.maximumSteps < 1 || settings.maximumSteps > 512 ||
          !std::isfinite(settings.maximumCutMetres) || settings.maximumCutMetres <= 0 || settings.maximumCutMetres > 200 ||
          !std::isfinite(settings.maximumDepositMetres) || settings.maximumDepositMetres < 0 || settings.maximumDepositMetres > 50)
        throw std::runtime_error("invalid regional erosion budget");
    }
    const auto before = h;
    const auto r = procedural::geometry::erodeMountainHydraulics(side, cell, j.at("seed"), mask, h, settings);
    double change = 0;
    for (size_t i = 0; i < h.size(); ++i)
      change += (before[i] - h[i]) * cell * cell;
    const double residual = r.erodedCubicMetres - r.depositedCubicMetres - r.exportedCubicMetres;
    if (!std::isfinite(change) || std::abs(residual) > 1e-7 * std::max(1., r.erodedCubicMetres) || std::abs(change - r.exportedCubicMetres) > 1e-7 * std::max(1., r.erodedCubicMetres))
      throw std::runtime_error("erosion mass balance failed");
    json out = {
      {"height", h},
      {"ledger",
       {{"droplets", r.droplets},
        {"erodedCubicMetres", r.erodedCubicMetres},
        {"depositedCubicMetres", r.depositedCubicMetres},
        {"exportedCubicMetres", r.exportedCubicMetres},
        {"maximumCutMetres", r.maximumCutMetres},
        {"massResidualCubicMetres", residual},
        {"heightVolumeLossCubicMetres", change}}}};
    std::ofstream output(argv[2]);
    output << out.dump() << '\n';
    if (!output)
      throw std::runtime_error("cannot write erosion output");
    std::cout << "PASS hydraulic erosion: " << r.droplets << " droplets, " << r.erodedCubicMetres << " m3 eroded, " << r.depositedCubicMetres << " m3 deposited; residual=" << residual << " m3\n";
    return 0;
  }
  catch (const std::exception &e)
  {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
