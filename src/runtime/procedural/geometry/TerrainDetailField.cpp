#include "procedural/geometry/TerrainDetailField.hpp"
#include <algorithm>
#include <cmath>

namespace procedural::geometry
{
namespace
{
uint64_t mix(uint64_t n)
{
  n ^= n >> 30; n *= 0xbf58476d1ce4e5b9ull; n ^= n >> 27;
  n *= 0x94d049bb133111ebull; return n ^ (n >> 31);
}
double smooth(double t) { t = std::clamp(t, 0.0, 1.0); return t * t * t * (t * (t * 6 - 15) + 10); }
// Value noise on a lattice. Cheap, and the shape wanted here is patches rather
// than the ridged detail a gradient noise would give.
double lattice(const double x, const double y, const uint64_t seed)
{
  const auto ix = int64_t(std::floor(x)), iy = int64_t(std::floor(y));
  const double tx = smooth(x - double(ix)), ty = smooth(y - double(iy));
  const auto corner = [&](const int64_t a, const int64_t b)
  {
    const uint64_t h = mix(seed ^ uint64_t(a) * 0x9E3779B97F4A7C15ull ^ (uint64_t(b) << 21u));
    return double(h >> 11) / double(1ull << 53);
  };
  const double a = corner(ix, iy), b = corner(ix + 1, iy);
  const double c = corner(ix, iy + 1), d = corner(ix + 1, iy + 1);
  return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
}
}

void TerrainDetailField::build(const double extentMetres, const DetailSettings &settings)
{
  settings_ = settings;
  extent_ = extentMetres > 0 ? extentMetres : 1.0;
}

double TerrainDetailField::at(const Point2 point) const
{
  if (!settings_.enabled || !(settings_.wavelengthMetres > 0)) return .5;
  double total = 0, amplitude = 1, sum = 0, wavelength = settings_.wavelengthMetres;
  for (uint32_t octave = 0; octave < std::max(1u, settings_.octaves); ++octave)
  {
    total += amplitude * lattice(point.x / wavelength, point.y / wavelength,
        mix(settings_.seed + octave * 7919u));
    sum += amplitude;
    amplitude *= settings_.gain;
    wavelength *= .5;
  }
  return std::clamp(sum > 0 ? total / sum : .5, 0.0, 1.0);
}

double TerrainDetailField::toleranceScale(const Point2 point) const
{
  if (!settings_.enabled) return 1.0;
  // Centred on 1 so the field redistributes the budget rather than inflating
  // it: a demanding patch refines harder only because a dull one refines less.
  const double demand = at(point) * 2.0 - 1.0;
  return std::clamp(1.0 - settings_.strength * demand, .25, 2.0);
}

TerrainDetailField::Chunk TerrainDetailField::bake(const Point2 origin, const double sizeMetres,
    const uint32_t side) const
{
  Chunk chunk;
  chunk.origin = origin;
  chunk.sizeMetres = sizeMetres;
  chunk.side = std::max(1u, side);
  chunk.values.resize(size_t(chunk.side) * chunk.side);
  const double step = sizeMetres / chunk.side;
  for (uint32_t y = 0; y < chunk.side; ++y)
    for (uint32_t x = 0; x < chunk.side; ++x)
      chunk.values[size_t(y) * chunk.side + x] = uint8_t(std::clamp(
          at({origin.x + (x + .5) * step, origin.y + (y + .5) * step}) * 255.0, 0.0, 255.0));
  return chunk;
}
}
