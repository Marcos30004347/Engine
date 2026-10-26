#include "procedural/geometry/TerrainHydraulicErosion.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace procedural::geometry
{
HydraulicErosionReport erodeMountainHydraulics(uint32_t side, double cellMetres,
    uint64_t seed, const std::vector<double> &mask, std::vector<double> &height,
    const HydraulicErosionSettings &settings)
{
  HydraulicErosionReport report;
  if (!settings.enabled || side < 4 || cellMetres <= 0 || mask.size() != size_t(side) * side || height.size() != mask.size()) return report;
  const auto original = height;
  std::vector<uint32_t> starts;
  for (uint32_t y = 1; y + 2 < side; ++y)
    for (uint32_t x = 1; x + 2 < side; ++x)
      if (mask[size_t(y) * side + x] > 0.1) starts.push_back(y * side + x);
  if (starts.empty()) return report;
  const auto random = [&]()
  {
    seed += 0x9e3779b97f4a7c15ull;
    uint64_t n = seed; n = (n ^ (n >> 30)) * 0xbf58476d1ce4e5b9ull;
    n = (n ^ (n >> 27)) * 0x94d049bb133111ebull;
    return double((n ^ (n >> 31)) >> 11) / double(uint64_t(1) << 53);
  };
  struct Sample { double height, gx, gy, mask; std::array<size_t, 4> ids; std::array<double, 4> weights; };
  const auto sample = [&](double x, double y)
  {
    const auto ix = uint32_t(x), iy = uint32_t(y);
    const double tx = x - ix, ty = y - iy;
    Sample s{};
    s.ids = {size_t(iy) * side + ix, size_t(iy) * side + ix + 1,
        size_t(iy + 1) * side + ix, size_t(iy + 1) * side + ix + 1};
    s.weights = {(1-tx)*(1-ty), tx*(1-ty), (1-tx)*ty, tx*ty};
    for (size_t i = 0; i < 4; ++i) { s.height += height[s.ids[i]] * s.weights[i]; s.mask += mask[s.ids[i]] * s.weights[i]; }
    s.gx = ((height[s.ids[1]] - height[s.ids[0]]) * (1-ty) + (height[s.ids[3]] - height[s.ids[2]]) * ty) / cellMetres;
    s.gy = ((height[s.ids[2]] - height[s.ids[0]]) * (1-tx) + (height[s.ids[3]] - height[s.ids[1]]) * tx) / cellMetres;
    return s;
  };
  // Return the amount actually exchanged, in equivalent texel-height metres.
  const auto deposit = [&](const Sample &s, double amount)
  {
    double done = 0;
    for (size_t i = 0; i < 4; ++i)
    {
      const double localMask = std::clamp(mask[s.ids[i]], 0.0, 1.0);
      const double added = std::min(amount * s.weights[i] * localMask,
          std::max(0.0, original[s.ids[i]] + settings.maximumDepositMetres * localMask - height[s.ids[i]]));
      height[s.ids[i]] += added; done += added;
    }
    report.depositedCubicMetres += done * cellMetres * cellMetres;
    return done;
  };
  const uint64_t drops = std::max(uint64_t(1), uint64_t(starts.size() * std::max(0.0, settings.dropletsPerCell)));
  for (uint64_t drop = 0; drop < drops; ++drop)
  {
    const auto start = starts[std::min(starts.size() - 1, size_t(random() * starts.size()))];
    double x = start % side + random(), y = start / side + random();
    double dx = 0, dy = 0, water = 1, speed = 1, sediment = 0;
    ++report.droplets;
    for (uint32_t step = 0; step < settings.maximumSteps; ++step)
    {
      const auto old = sample(x, y);
      if (old.mask < 0.01) break;
      dx = dx * settings.inertia - old.gx * (1-settings.inertia);
      dy = dy * settings.inertia - old.gy * (1-settings.inertia);
      const double length = std::hypot(dx, dy);
      if (length < 1e-10) { sediment -= deposit(old, sediment); break; }
      dx /= length; dy /= length;
      const double nx = x + dx, ny = y + dy;
      if (nx < 0 || ny < 0 || nx >= side-1 || ny >= side-1) break;
      const auto next = sample(nx, ny);
      const double fall = old.height - next.height;
      const double capacity = std::max(0.01, fall) * speed * water * 0.025;
      if (fall < 0 || sediment > capacity)
      {
        const double amount = fall < 0 ? std::min(sediment, -fall) : (sediment-capacity) * settings.depositionRate;
        sediment -= deposit(old, amount);
      }
      else
      {
        const double amount = std::min({(capacity-sediment) * settings.erosionRate, std::max(0.0, fall) * 0.25, 3.0});
        // A small radial brush avoids drilling a single grid vertex. Bound
        // removal by the local downhill drop to keep a particle from digging
        // an uphill pit underneath itself.
        const int ix = int(x), iy = int(y);
        double totalWeight = 0;
        for (int oy = -1; oy <= 1; ++oy) for (int ox = -1; ox <= 1; ++ox)
          if (ix+ox >= 0 && iy+oy >= 0 && ix+ox < int(side) && iy+oy < int(side))
            totalWeight += mask[size_t(iy+oy)*side+ix+ox] / (1 + ox*ox + oy*oy);
        if (totalWeight > 0)
          for (int oy = -1; oy <= 1; ++oy) for (int ox = -1; ox <= 1; ++ox)
          {
            if (ix+ox < 0 || iy+oy < 0 || ix+ox >= int(side) || iy+oy >= int(side)) continue;
            const size_t at = size_t(iy+oy)*side+ix+ox;
            const double removed = std::min(amount * mask[at] / ((1 + ox*ox + oy*oy) * totalWeight),
                std::max(0.0, height[at] - original[at] + settings.maximumCutMetres * mask[at]));
            height[at] -= removed; sediment += removed;
            report.erodedCubicMetres += removed * cellMetres * cellMetres;
          }
      }
      speed = std::sqrt(std::max(0.05, speed*speed + 9.81 * fall));
      water *= 1-settings.evaporation;
      x = nx; y = ny;
      if (water < 0.05 || step + 1 == settings.maximumSteps)
      { sediment -= deposit(sample(x, y), sediment); break; }
    }
    report.exportedCubicMetres += sediment * cellMetres * cellMetres;
  }
  for (size_t i = 0; i < height.size(); ++i)
    report.maximumCutMetres = std::max(report.maximumCutMetres, original[i] - height[i]);
  return report;
}
}
