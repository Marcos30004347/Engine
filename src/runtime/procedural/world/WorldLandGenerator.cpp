#include "WorldLandGenerator.hpp"
#include "procedural/PoissonDiskSampler.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
namespace procedural::world::rewrite
{
bool generateLand(const WorldSpec &spec, double spacing, const std::vector<Position> &cores, double fraction, LandProduct &output, std::string *error, double oceanMarginMetres)
{
  auto fail = [&](const char *s)
  {
    if (error)
      *error = s;
    return false;
  };
  if (!spec.validate(error))
    return false;
  if (!std::isfinite(spacing) || spacing <= 0 || spacing > spec.extentMetres / 2 || spec.extentMetres / spacing > 250)
    return fail("Land spacing exceeds supported partition budget");
  if (cores.empty() || !std::isfinite(fraction) || fraction <= 0 || fraction >= 1)
    return fail("Invalid land cores or fraction");
  for (auto p : cores)
    if (!std::isfinite(p.x) || !std::isfinite(p.z) || p.x < 0 || p.z < 0 || p.x > spec.extentMetres || p.z > spec.extentMetres)
      return fail("Land core outside world");
  PoissonDiskSampler sampler;
  PoissonDiskSampler::Settings sampling;
  sampling.width = sampling.height = spec.extentMetres;
  sampling.radius = spacing;
  sampling.seed = featureSeed(spec.seed, spec.generatorVersion, 0, 0x50415254);
  sampling.method = PoissonDiskSampler::Method::RandomThrows;
  auto sites = sampler.generate(sampling);
  std::vector<VoronoiCell> cells;
  double e = spec.extentMetres;
  if (!buildVoronoiCells(sites, {{0, 0}, {e, 0}, {e, e}, {0, e}}, 1e-7, cells, error))
    return false;
  // Clipping can emit the same boundary corner twice. Normalize construction
  // roundoff here; the published-product validator still rejects collapsed rings.
  for (auto &cell : cells)
  {
    for (auto &p : cell.area)
    {
      p.x = std::round(p.x * 1e6) / 1e6;
      p.y = std::round(p.y * 1e6) / 1e6;
    }
    auto same = [](auto a, auto b)
    {
      return a.x == b.x && a.y == b.y;
    };
    cell.area.erase(std::unique(cell.area.begin(), cell.area.end(), same), cell.area.end());
    if (cell.area.size() > 1 && same(cell.area.front(), cell.area.back()))
      cell.area.pop_back();
  }
  LandSettings land;
  land.landFraction = fraction;
  land.oceanMarginMetres = oceanMarginMetres;
  std::set<uint32_t> selected;
  for (auto p : cores)
  {
    double best = std::numeric_limits<double>::infinity();
    uint32_t nearest = 0;
    for (uint32_t c = 0; c < cells.size(); ++c)
    {
      double d = std::hypot(p.x - cells[c].site.x, p.z - cells[c].site.y);
      if (d < best)
      {
        best = d;
        nearest = c;
      }
    }
    if (!selected.insert(nearest).second)
      return fail("Land cores resolve to the same planning cell");
    land.cores.push_back(nearest);
  }
  return buildLandProduct(spec, cells, land, output, error);
}
} // namespace procedural::world::rewrite
