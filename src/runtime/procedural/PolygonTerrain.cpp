#include "procedural/PolygonTerrain.hpp"

#include "procedural/ConstrainedDelaunay.hpp"
#include "procedural/PoissonDiskSampler.hpp"
#include "procedural/Predicates.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace procedural
{
namespace
{

constexpr double kPi = 3.14159265358979323846264338327950288;

struct Site
{
  Point2 position;
  std::vector<uint32_t> neighbours;
  std::vector<uint32_t> corners;
  bool border = false;
  bool water = false;
  bool ocean = false;
  bool coast = false;
  double elevation = 0.0;
  double moisture = 0.0;
  double temperature = 0.0;
  double precipitation = 0.0;
  double windMoisture = 0.0;
  double windExposure = 0.0;
  double mountain = 0.0;
  double plain = 0.0;
  double valley = 0.0;
  uint32_t river = 0u;
};

struct Corner
{
  Point2 position;
  std::vector<uint32_t> neighbours;
  std::vector<uint32_t> touches;
  bool border = false;
  bool water = false;
  bool ocean = false;
  bool coast = false;
  double elevation = 0.0;
  double moisture = 0.0;
  uint32_t river = 0u;
  int32_t downhill = -1;
};

struct Triangle
{
  std::array<uint32_t, 3> sites{};
  uint32_t corner = 0u;
};

struct EdgeRecord
{
  std::vector<uint32_t> triangles;
};

struct MountainRange
{
  Point2 begin;
  Point2 end;
  double halfWidth = 1.0;
  double strength = 0.0;
};

struct LakeBasin
{
  Point2 centre;
  double radiusX = 1.0;
  double radiusY = 1.0;
  double angle = 0.0;
  double strength = 0.0;
};

struct IslandProfile
{
  Point2 centre;
  double radiusX = 1.0;
  double radiusY = 1.0;
  double angle = 0.0;
  double peakRadius = 0.24;
  double peakStrength = 0.65;
  double shelfWidth = 0.18;
};

struct MacroRegion
{
  bool continental = false;
  double baseElevation = 0.0;
  double moisture = 0.0;
};

struct MacroCell
{
  uint32_t region = 0u;
  double continent = 0.0;
  double moisture = 0.0;
  double uplift = 0.0;
};

struct MacroWorld
{
  uint32_t columns = 0u;
  uint32_t rows = 0u;
  double cellWidth = 1.0;
  double cellHeight = 1.0;
  std::vector<MacroRegion> regions;
  std::vector<MacroCell> cells;
};

struct TectonicPlate
{
  Point2 centre;
  double velocityX = 0.0;
  double velocityY = 0.0;
  double baseElevation = 0.0;
  bool continental = false;
};

struct TerrainFeatures
{
  IslandProfile island;
  MacroWorld macro;
  std::vector<TectonicPlate> plates;
  std::vector<MountainRange> mountains;
  std::vector<LakeBasin> lakeBasins;
};

struct GridIndex
{
  double cellSize = 1.0;
  uint32_t columns = 0u;
  uint32_t rows = 0u;
  double width = 0.0;
  double height = 0.0;
  std::vector<std::vector<uint32_t>> cells;
};

uint64_t splitmix64(uint64_t value)
{
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
  return value ^ (value >> 31u);
}

double hashUnit(const uint64_t value)
{
  return static_cast<double>(splitmix64(value) >> 11u) * (1.0 / 9007199254740992.0);
}

double smoothstep(const double value)
{
  return value * value * (3.0 - 2.0 * value);
}

double valueNoise(const double x, const double y, const uint64_t seed)
{
  const int64_t x0 = static_cast<int64_t>(std::floor(x));
  const int64_t y0 = static_cast<int64_t>(std::floor(y));
  const double tx = smoothstep(x - static_cast<double>(x0));
  const double ty = smoothstep(y - static_cast<double>(y0));
  const auto corner = [seed](const int64_t ix, const int64_t iy)
  {
    return hashUnit(seed ^ (static_cast<uint64_t>(ix) * 0x9e3779b97f4a7c15ull) ^
                   (static_cast<uint64_t>(iy) * 0xbf58476d1ce4e5b9ull));
  };
  const double a = corner(x0, y0);
  const double b = corner(x0 + 1, y0);
  const double c = corner(x0, y0 + 1);
  const double d = corner(x0 + 1, y0 + 1);
  return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

double fractalNoise(
    const Point2 &point, const PolygonTerrainSettings &settings, const double baseFrequency,
    const uint64_t seed)
{
  const double nx = point.x / settings.widthMetres;
  const double ny = point.y / settings.heightMetres;
  double amplitude = 0.5;
  double frequency = baseFrequency;
  double value = 0.0;
  double totalAmplitude = 0.0;
  for (uint32_t octave = 0u; octave < 4u; ++octave)
  {
    value += valueNoise(nx * frequency, ny * frequency, seed + octave * 0x9e3779b97f4a7c15ull) * amplitude;
    totalAmplitude += amplitude;
    frequency *= 2.03;
    amplitude *= 0.5;
  }
  return totalAmplitude > 0.0 ? value / totalAmplitude : 0.0;
}

double metricFractalNoise(
    const Point2 &point, const double baseWavelengthMetres, const uint64_t seed)
{
  const double wavelength = std::max(1.0, baseWavelengthMetres);
  double amplitude = 0.5;
  double frequency = 1.0 / wavelength;
  double value = 0.0;
  double totalAmplitude = 0.0;
  for (uint32_t octave = 0u; octave < 4u; ++octave)
  {
    value += valueNoise(point.x * frequency, point.y * frequency, seed + octave * 0x517cc1b727220a95ull) * amplitude;
    totalAmplitude += amplitude;
    frequency *= 2.07;
    amplitude *= 0.5;
  }
  return totalAmplitude > 0.0 ? value / totalAmplitude : 0.0;
}

double ridgedMetricNoise(
    const Point2 &point, const double baseWavelengthMetres, const uint64_t seed)
{
  const double wavelength = std::max(1.0, baseWavelengthMetres);
  double amplitude = 0.5;
  double frequency = 1.0 / wavelength;
  double value = 0.0;
  double totalAmplitude = 0.0;
  for (uint32_t octave = 0u; octave < 5u; ++octave)
  {
    const double raw = valueNoise(
        point.x * frequency, point.y * frequency, seed + octave * 0x94d049bb133111ebull);
    const double ridge = 1.0 - std::abs(raw * 2.0 - 1.0);
    value += ridge * ridge * amplitude;
    totalAmplitude += amplitude;
    frequency *= 2.11;
    amplitude *= 0.48;
  }
  return totalAmplitude > 0.0 ? value / totalAmplitude : 0.0;
}

double billowMetricNoise(
    const Point2 &point, const double baseWavelengthMetres, const uint64_t seed)
{
  const double wavelength = std::max(1.0, baseWavelengthMetres);
  double amplitude = 0.5;
  double frequency = 1.0 / wavelength;
  double value = 0.0;
  double totalAmplitude = 0.0;
  for (uint32_t octave = 0u; octave < 4u; ++octave)
  {
    const double raw = valueNoise(
        point.x * frequency, point.y * frequency, seed + octave * 0xbb67ae8584caa73bull);
    value += std::abs(raw * 2.0 - 1.0) * amplitude;
    totalAmplitude += amplitude;
    frequency *= 2.03;
    amplitude *= 0.52;
  }
  return totalAmplitude > 0.0 ? value / totalAmplitude : 0.0;
}

double domainWarpedMetricNoise(
    const Point2 &point, const double baseWavelengthMetres, const double warpMetres,
    const uint64_t seed)
{
  const double wx =
      (metricFractalNoise(point, baseWavelengthMetres * 1.7, seed ^ 0x243f6a8885a308d3ull) - 0.5) *
      warpMetres;
  const double wy =
      (metricFractalNoise(point, baseWavelengthMetres * 1.7, seed ^ 0x13198a2e03707344ull) - 0.5) *
      warpMetres;
  return metricFractalNoise(Point2(point.x + wx, point.y + wy), baseWavelengthMetres, seed);
}

double distanceToSegment(const Point2 &point, const Point2 &begin, const Point2 &end)
{
  const double vx = end.x - begin.x;
  const double vy = end.y - begin.y;
  const double wx = point.x - begin.x;
  const double wy = point.y - begin.y;
  const double lengthSquared = vx * vx + vy * vy;
  const double t = lengthSquared > 0.0 ? std::clamp((wx * vx + wy * vy) / lengthSquared, 0.0, 1.0) : 0.0;
  const double dx = point.x - (begin.x + vx * t);
  const double dy = point.y - (begin.y + vy * t);
  return std::sqrt(dx * dx + dy * dy);
}

double islandDistance(const Point2 &point, const IslandProfile &island)
{
  const double dx = point.x - island.centre.x;
  const double dy = point.y - island.centre.y;
  const double ca = std::cos(island.angle);
  const double sa = std::sin(island.angle);
  const double localX = (dx * ca + dy * sa) / std::max(1.0, island.radiusX);
  const double localY = (-dx * sa + dy * ca) / std::max(1.0, island.radiusY);
  return std::sqrt(localX * localX + localY * localY);
}

double centralPeakInfluence(const Point2 &point, const TerrainFeatures &features)
{
  const double distance = islandDistance(point, features.island);
  const double radius = std::max(0.05, features.island.peakRadius);
  const double peak = std::exp(-std::pow(distance / radius, 2.0));
  const double highlandShoulder = std::exp(-std::pow(distance / (radius * 2.35), 2.0)) * 0.36;
  return std::clamp((peak + highlandShoulder) * features.island.peakStrength, 0.0, 1.0);
}

uint32_t macroCellIndex(const MacroWorld &macro, const uint32_t x, const uint32_t y)
{
  return y * macro.columns + x;
}

double sampleMacroContinent(const Point2 &point, const TerrainFeatures &features, const PolygonTerrainSettings &settings)
{
  const MacroWorld &macro = features.macro;
  if (macro.cells.empty() || macro.columns == 0u || macro.rows == 0u) return 0.0;
  const double gx = std::clamp(point.x / std::max(1.0, settings.widthMetres) * macro.columns - 0.5, 0.0, static_cast<double>(macro.columns - 1u));
  const double gy = std::clamp(point.y / std::max(1.0, settings.heightMetres) * macro.rows - 0.5, 0.0, static_cast<double>(macro.rows - 1u));
  const uint32_t x0 = static_cast<uint32_t>(std::floor(gx));
  const uint32_t y0 = static_cast<uint32_t>(std::floor(gy));
  const uint32_t x1 = std::min(x0 + 1u, macro.columns - 1u);
  const uint32_t y1 = std::min(y0 + 1u, macro.rows - 1u);
  const double tx = smoothstep(gx - x0);
  const double ty = smoothstep(gy - y0);
  const auto value = [&macro](const uint32_t x, const uint32_t y)
  {
    const MacroCell &cell = macro.cells[macroCellIndex(macro, x, y)];
    return cell.continent + cell.uplift * 0.28;
  };
  const double a = value(x0, y0);
  const double b = value(x1, y0);
  const double c = value(x0, y1);
  const double d = value(x1, y1);
  return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

struct TectonicSample
{
  double baseElevation = 0.0;
  double boundary = 0.0;
  double convergence = 0.0;
  double rift = 0.0;
  bool continental = false;
};

TectonicSample sampleTectonics(
    const Point2 &point, const TerrainFeatures &features, const PolygonTerrainSettings &settings)
{
  if (features.plates.empty()) return {};
  uint32_t nearest = 0u;
  uint32_t second = 0u;
  double nearestDistance = std::numeric_limits<double>::max();
  double secondDistance = std::numeric_limits<double>::max();
  for (uint32_t plate = 0u; plate < features.plates.size(); ++plate)
  {
    const double dx = (point.x - features.plates[plate].centre.x) / settings.widthMetres;
    const double dy = (point.y - features.plates[plate].centre.y) / settings.heightMetres;
    const double distance = dx * dx + dy * dy;
    if (distance < nearestDistance)
    {
      secondDistance = nearestDistance;
      second = nearest;
      nearestDistance = distance;
      nearest = plate;
    }
    else if (distance < secondDistance)
    {
      secondDistance = distance;
      second = plate;
    }
  }

  const TectonicPlate &a = features.plates[nearest];
  const TectonicPlate &b = features.plates[second];
  const double nearestRoot = std::sqrt(std::max(0.0, nearestDistance));
  const double secondRoot = std::sqrt(std::max(0.0, secondDistance));
  const double boundary = 1.0 - std::clamp((secondRoot - nearestRoot) / 0.085, 0.0, 1.0);
  double nx = b.centre.x - a.centre.x;
  double ny = b.centre.y - a.centre.y;
  const double normalLength = std::sqrt(nx * nx + ny * ny);
  if (normalLength > 0.0)
  {
    nx /= normalLength;
    ny /= normalLength;
  }
  const double relativeVelocity = (a.velocityX - b.velocityX) * nx + (a.velocityY - b.velocityY) * ny;
  TectonicSample sample{};
  sample.baseElevation = a.baseElevation;
  sample.boundary = smoothstep(boundary);
  sample.convergence = std::max(0.0, relativeVelocity) * sample.boundary;
  sample.rift = std::max(0.0, -relativeVelocity) * sample.boundary;
  sample.continental = a.continental;
  return sample;
}

double mountainInfluence(
    const Point2 &point, const TerrainFeatures &features, const PolygonTerrainSettings &settings)
{
  double influence = centralPeakInfluence(point, features);
  for (const MountainRange &range : features.mountains)
  {
    const double distance = distanceToSegment(point, range.begin, range.end);
    const double ridge = 1.0 - std::clamp(distance / range.halfWidth, 0.0, 1.0);
    const double foothill = 1.0 - std::clamp(distance / (range.halfWidth * 3.0), 0.0, 1.0);
    if (ridge <= 0.0 && foothill <= 0.0) continue;
    const double roughness = 0.72 + fractalNoise(point, settings, 28.0, settings.seed ^ 0xf1357aea2e62a9c5ull) * 0.44;
    const double core = ridge > 0.0 ? smoothstep(ridge) * range.strength * roughness : 0.0;
    const double shoulder = foothill > 0.0 ? smoothstep(foothill) * range.strength * 0.28 : 0.0;
    influence = std::max(influence, core + shoulder);
  }
  return std::clamp(influence, 0.0, 1.0);
}

double lakeBasinInfluence(
    const Point2 &point, const TerrainFeatures &features, const PolygonTerrainSettings &settings)
{
  double influence = 0.0;
  for (const LakeBasin &basin : features.lakeBasins)
  {
    const double dx = point.x - basin.centre.x;
    const double dy = point.y - basin.centre.y;
    const double ca = std::cos(basin.angle);
    const double sa = std::sin(basin.angle);
    const double localX = (dx * ca + dy * sa) / basin.radiusX;
    const double localY = (-dx * sa + dy * ca) / basin.radiusY;
    const double distance = std::sqrt(localX * localX + localY * localY);
    const double basinShape = 1.0 - std::clamp(distance, 0.0, 1.0);
    if (basinShape <= 0.0) continue;
    const double shorelineNoise =
        (fractalNoise(point, settings, 22.0, settings.seed ^ 0x4f1bbcdc9f45d4b5ull) - 0.5) * 0.20;
    influence = std::max(influence, smoothstep(basinShape) * basin.strength + shorelineNoise);
  }
  return std::clamp(influence, 0.0, 1.0);
}

double plainInfluence(const Point2 &point, const PolygonTerrainSettings &settings)
{
  const double lowFrequency = fractalNoise(point, settings, 2.0, settings.seed ^ 0x8b8b1f55d43536a1ull);
  const double mask = std::clamp((lowFrequency - 0.46) / 0.28, 0.0, 1.0);
  return smoothstep(mask);
}

double climateLatitude(const Point2 &point, const PolygonTerrainSettings &settings)
{
  const double y = settings.heightMetres <= 0.0 ? 0.5 : point.y / settings.heightMetres;
  const double tilt =
      (hashUnit(settings.seed ^ 0x9b05688c2b3e6c1full) - 0.5) * 0.18;
  return std::clamp(std::abs((y + tilt) * 2.0 - 1.0), 0.0, 1.0);
}

double prevailingWindAngle(const PolygonTerrainSettings &settings)
{
  return hashUnit(settings.seed ^ 0x510e527fade682d1ull) * 2.0 * kPi;
}

double rainShadow(
    const Point2 &point, const TerrainFeatures &features, const PolygonTerrainSettings &settings)
{
  const double windAngle = prevailingWindAngle(settings);
  const double upwindX = -std::cos(windAngle);
  const double upwindY = -std::sin(windAngle);
  const double extent = std::max(settings.widthMetres, settings.heightMetres);
  const double step = std::max(settings.siteRadiusMetres * 2.0, extent * 0.035);
  double shadow = 0.0;
  for (uint32_t sample = 1u; sample <= 7u; ++sample)
  {
    const Point2 probe(point.x + upwindX * step * sample, point.y + upwindY * step * sample);
    if (probe.x < 0.0 || probe.y < 0.0 || probe.x > settings.widthMetres || probe.y > settings.heightMetres)
      break;
    const double mountain = mountainInfluence(probe, features, settings);
    const double weight = 1.0 - static_cast<double>(sample - 1u) / 7.0;
    shadow += mountain * weight;
  }
  return std::clamp(shadow * 0.38, 0.0, 0.85);
}

double climateTemperature(
    const Site &site, const TerrainFeatures &features, const PolygonTerrainSettings &settings)
{
  if (site.ocean) return 0.45;
  const double latitude = climateLatitude(site.position, settings);
  const double base = 1.0 - std::pow(latitude, 1.55);
  const double lapse = site.elevation * 0.58 + site.mountain * 0.18;
  const double continentality =
      (metricFractalNoise(site.position, 3600.0, settings.seed ^ 0x1f83d9abfb41bd6bull) - 0.5) * 0.16;
  const double local = (metricFractalNoise(site.position, 900.0, settings.seed ^ 0x5be0cd19137e2179ull) - 0.5) * 0.07;
  const double coastalModeration = site.coast || site.water ? 0.08 * (0.55 - base) : 0.0;
  return std::clamp(base - lapse + continentality + local + coastalModeration, 0.0, 1.0);
}

double climatePrecipitation(
    const Site &site, const TerrainFeatures &features, const PolygonTerrainSettings &settings)
{
  const double latitude = climateLatitude(site.position, settings);
  const double wetLatitude =
      0.25 +
      0.45 * std::exp(-std::pow(latitude / 0.28, 2.0)) +
      0.18 * std::exp(-std::pow((latitude - 0.58) / 0.18, 2.0));
  const double aridBelt =
      0.34 * std::exp(-std::pow((latitude - 0.36) / 0.13, 2.0));
  const double orographicLift = site.mountain * 0.42;
  const double shadow = rainShadow(site.position, features, settings);
  const double stormNoise =
      (metricFractalNoise(site.position, 2400.0, settings.seed ^ 0xcbbb9d5dc1059ed8ull) - 0.5) * 0.20;
  const double waterSource = site.moisture * 0.48 + (site.coast ? 0.20 : 0.0) + (site.water ? 0.28 : 0.0);
  return std::clamp(waterSource + wetLatitude + orographicLift + stormNoise - aridBelt - shadow, 0.0, 1.0);
}

uint8_t quantizeUnit(const double value)
{
  return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

uint8_t terrainFormForSite(const Site &site)
{
  if (site.water || site.ocean) return 0u;
  if (site.elevation > 0.86 || (site.mountain > 0.76 && site.elevation > 0.70)) return 5u;
  if (site.mountain > 0.52 || site.elevation > 0.72) return 4u;
  if (site.river > 0u || site.valley > 0.22) return 2u;
  if (site.plain > 0.50 && site.elevation < 0.55) return 1u;
  if (site.elevation < 0.38 && site.mountain < 0.18) return 1u;
  return 3u;
}

double forestSuitabilityForSite(const Site &site)
{
  if (site.water || site.ocean || site.coast) return 0.0;
  if (site.elevation > 0.80 || site.mountain > 0.68) return 0.0;
  const double wetness = smoothstep(std::clamp((site.precipitation - 0.28) / 0.46, 0.0, 1.0));
  const double tooWet = smoothstep(std::clamp((site.precipitation - 0.86) / 0.14, 0.0, 1.0));
  const double warmth =
      smoothstep(std::clamp((site.temperature - 0.20) / 0.24, 0.0, 1.0)) *
      (1.0 - smoothstep(std::clamp((site.temperature - 0.88) / 0.12, 0.0, 1.0)));
  const double alpinePenalty = smoothstep(std::clamp((site.elevation - 0.58) / 0.24, 0.0, 1.0));
  const double plainsBreakup = 1.0 - site.plain * 0.30;
  return std::clamp(wetness * warmth * plainsBreakup * (1.0 - tooWet * 0.35) * (1.0 - alpinePenalty), 0.0, 1.0);
}

double settlementSuitabilityForSite(const Site &site)
{
  if (site.water || site.ocean || site.coast) return 0.0;
  if (site.elevation < 0.035 || site.elevation > 0.52) return 0.0;
  const double flatness = std::clamp(site.plain * 0.82 + (1.0 - site.mountain) * 0.18, 0.0, 1.0);
  const double elevationBand =
      smoothstep(std::clamp((site.elevation - 0.05) / 0.14, 0.0, 1.0)) *
      (1.0 - smoothstep(std::clamp((site.elevation - 0.44) / 0.14, 0.0, 1.0)));
  const double temperate =
      smoothstep(std::clamp((site.temperature - 0.24) / 0.18, 0.0, 1.0)) *
      (1.0 - smoothstep(std::clamp((site.temperature - 0.78) / 0.16, 0.0, 1.0)));
  const double rainComfort =
      smoothstep(std::clamp((site.precipitation - 0.16) / 0.22, 0.0, 1.0)) *
      (1.0 - smoothstep(std::clamp((site.precipitation - 0.82) / 0.16, 0.0, 1.0)));
  const double waterAccess =
      std::clamp((site.river > 0u ? 0.62 : 0.0) + site.moisture * 0.34 + (site.valley > 0.0 ? 0.20 : 0.0), 0.18, 1.0);
  const double terrainCore = std::sqrt(std::max(0.0, flatness * elevationBand));
  const double mountainPenalty = site.mountain * 0.34;
  const double forestPenalty = forestSuitabilityForSite(site) * 0.12;
  const double blended =
      flatness * 0.30 +
      elevationBand * 0.22 +
      temperate * 0.16 +
      rainComfort * 0.12 +
      waterAccess * 0.20;
  return std::clamp(blended * (0.42 + terrainCore * 0.58) - mountainPenalty - forestPenalty, 0.0, 1.0);
}

void simulateWindClimate(
    std::vector<Site> &sites, const TerrainFeatures &features, const PolygonTerrainSettings &settings)
{
  const double windAngle = prevailingWindAngle(settings);
  const double windX = std::cos(windAngle);
  const double windY = std::sin(windAngle);
  const double extent = std::max(settings.widthMetres, settings.heightMetres);
  std::vector<uint32_t> order(sites.size());
  std::iota(order.begin(), order.end(), 0u);
  std::sort(
      order.begin(), order.end(),
      [&sites, windX, windY](const uint32_t a, const uint32_t b)
      {
        const double projectionA = sites[a].position.x * windX + sites[a].position.y * windY;
        const double projectionB = sites[b].position.x * windX + sites[b].position.y * windY;
        if (projectionA != projectionB) return projectionA < projectionB;
        return a < b;
      });

  for (Site &site : sites)
  {
    site.temperature = climateTemperature(site, features, settings);
    site.precipitation = 0.0;
    site.windMoisture = 0.0;
    site.windExposure = 0.0;
  }

  for (const uint32_t siteIndex : order)
  {
    Site &site = sites[siteIndex];
    const double projection = site.position.x * windX + site.position.y * windY;
    double incomingMoisture = 0.0;
    double incomingElevation = 0.0;
    double incomingWeight = 0.0;
    double exposure = 0.0;
    for (const uint32_t neighbourIndex : site.neighbours)
    {
      const Site &neighbour = sites[neighbourIndex];
      const double neighbourProjection = neighbour.position.x * windX + neighbour.position.y * windY;
      if (neighbourProjection > projection) continue;
      const double dx = site.position.x - neighbour.position.x;
      const double dy = site.position.y - neighbour.position.y;
      const double distance = std::max(1.0, std::sqrt(dx * dx + dy * dy));
      const double alignment = std::max(0.0, ((dx / distance) * windX + (dy / distance) * windY));
      const double weight = (0.20 + alignment) / distance;
      incomingMoisture += neighbour.windMoisture * weight;
      incomingElevation += neighbour.elevation * weight;
      incomingWeight += weight;
      exposure = std::max(exposure, alignment);
    }

    if (incomingWeight > 0.0)
    {
      incomingMoisture /= incomingWeight;
      incomingElevation /= incomingWeight;
    }
    else
    {
      const double upwindEdgeDistance = std::min({
          windX > 0.0 ? site.position.x : settings.widthMetres - site.position.x,
          windY > 0.0 ? site.position.y : settings.heightMetres - site.position.y,
          extent * 0.35});
      const double edgeSource = 1.0 - std::clamp(upwindEdgeDistance / std::max(1.0, extent * 0.35), 0.0, 1.0);
      incomingMoisture = 0.18 + edgeSource * 0.44;
      incomingElevation = site.elevation;
      exposure = edgeSource;
    }

    const double latitude = climateLatitude(site.position, settings);
    const double wetLatitude =
        0.22 +
        0.36 * std::exp(-std::pow(latitude / 0.28, 2.0)) +
        0.14 * std::exp(-std::pow((latitude - 0.58) / 0.18, 2.0));
    const double aridBelt = 0.30 * std::exp(-std::pow((latitude - 0.36) / 0.13, 2.0));
    const double terrainRise = std::max(0.0, site.elevation - incomingElevation);
    const double lift = terrainRise * 0.70 + site.mountain * 0.55;
    const double localStorms =
        (metricFractalNoise(site.position, 2300.0, settings.seed ^ 0xcbbb9d5dc1059ed8ull) - 0.5) * 0.16;
    const double waterSource =
        (site.ocean ? 0.78 : 0.0) +
        (site.water && !site.ocean ? 0.40 : 0.0) +
        (site.coast ? 0.22 : 0.0) +
        site.moisture * 0.18 +
        std::min(0.18, static_cast<double>(site.river) / 900.0);
    const double rainfall =
        std::clamp(incomingMoisture * (0.12 + lift) + wetLatitude * 0.38 + waterSource + localStorms - aridBelt, 0.0, 1.0);
    const double baseline = climatePrecipitation(site, features, settings);
    site.precipitation = std::clamp(rainfall * 0.72 + baseline * 0.28, 0.0, 1.0);
    site.windMoisture = std::clamp(
        incomingMoisture * 0.94 + waterSource * 0.55 + wetLatitude * 0.12 - rainfall * 0.58 - lift * 0.12,
        0.0, 1.0);
    site.windExposure = std::clamp(
        exposure * 0.45 + incomingMoisture * 0.25 + lift * 0.30 +
            (site.coast || site.ocean ? 0.22 : 0.0),
        0.0, 1.0);
  }
}

double continentField(
    const Point2 &point, const PolygonTerrainSettings &settings, const TerrainFeatures &features)
{
  const double nx = point.x / settings.widthMetres;
  const double ny = point.y / settings.heightMetres;
  const double centredX = nx * 2.0 - 1.0;
  const double centredY = ny * 2.0 - 1.0;
  const double extent = std::max(settings.widthMetres, settings.heightMetres);
  const double island = islandDistance(point, features.island);
  const double edgeDistanceMetres = std::min(
      std::min(point.x, settings.widthMetres - point.x),
      std::min(point.y, settings.heightMetres - point.y));
  const double shelf = smoothstep(std::clamp(edgeDistanceMetres / std::max(260.0, extent * 0.08), 0.0, 1.0));
  const uint64_t shapeSeed = settings.seed ^ 0xd1b54a32d192ed03ull;
  const double angle = std::atan2(centredY, centredX);
  const double radialDistance =
      std::sqrt((centredX * 0.92) * (centredX * 0.92) + (centredY * 1.06) * (centredY * 1.06));
  const double islandWeight =
      1.0 - smoothstep(std::clamp((extent - 6000.0) / 8000.0, 0.0, 1.0));
  const double angularCoast =
      0.69 +
      0.13 * std::sin(angle * 2.0 + hashUnit(shapeSeed) * 2.0 * kPi) +
      0.09 * std::sin(angle * 5.0 + hashUnit(shapeSeed ^ 0x94d049bb133111ebull) * 2.0 * kPi) +
      0.05 * std::sin(angle * 9.0 + hashUnit(shapeSeed ^ 0xbf58476d1ce4e5b9ull) * 2.0 * kPi);
  const double broadLand = angularCoast - radialDistance;
  const TectonicSample tectonics = sampleTectonics(point, features, settings);
  const double plateUplift =
      tectonics.baseElevation +
      tectonics.convergence * (tectonics.continental ? 0.72 : 0.38) -
      tectonics.rift * 0.58;
  const double warpedIsland =
      (domainWarpedMetricNoise(point, extent * 0.34, extent * 0.055, settings.seed ^ 0x1f83d9abfb41bd6bull) - 0.5) *
      0.34;
  const double scallopedCoast =
      (metricFractalNoise(point, extent * 0.09, settings.seed ^ 0x5be0cd19137e2179ull) - 0.5) *
      0.16;
  const double islandBody = (1.0 - island) * 0.82 + warpedIsland + scallopedCoast;
  const double centralUplift = centralPeakInfluence(point, features) * 0.42;
  const double macroContinent = sampleMacroContinent(point, features, settings);
  const double continental =
      (metricFractalNoise(point, 4200.0, settings.seed ^ 0x6a09e667f3bcc909ull) - 0.5) * 0.50;
  const double regional =
      (metricFractalNoise(point, 1450.0, settings.seed ^ 0x94d049bb133111ebull) - 0.5) * 0.28;
  const double coast =
      (metricFractalNoise(point, 520.0, settings.seed ^ 0xbb67ae8584caa73bull) - 0.5) * 0.12;
  const double outsidePenalty = island > 1.0 ? (island - 1.0) * (1.15 + features.island.shelfWidth) : 0.0;
  const double islandField = islandBody + centralUplift - outsidePenalty;
  const double continentField = macroContinent * 0.98 + plateUplift * 0.46 + broadLand * 0.08;
  return islandField * islandWeight + continentField * (1.0 - islandWeight) +
      plateUplift * (0.20 + islandWeight * 0.26) + shelf * 0.10 +
      continental * 0.30 + regional * 0.24 + coast - (1.0 - shelf) * 0.28;
}

bool isTerrainWater(
    const Point2 &point, const PolygonTerrainSettings &settings, const TerrainFeatures &features)
{
  const double land = continentField(point, settings, features);
  if (land < 0.08) return true;
  const double mountain = mountainInfluence(point, features, settings);
  if (mountain < 0.35 && land > 0.28 && lakeBasinInfluence(point, features, settings) > 0.34)
    return true;
  return false;
}

bool isBoundaryPoint(const Point2 &point, const PolygonTerrainSettings &settings)
{
  constexpr double epsilon = 1.0e-7;
  return point.x <= epsilon || point.y <= epsilon ||
      point.x >= settings.widthMetres - epsilon || point.y >= settings.heightMetres - epsilon;
}

uint64_t edgeKey(const uint32_t a, const uint32_t b)
{
  const uint32_t first = std::min(a, b);
  const uint32_t second = std::max(a, b);
  return (static_cast<uint64_t>(first) << 32u) | second;
}

Point2 circumcentre(const Point2 &a, const Point2 &b, const Point2 &c)
{
  const double ax = a.x, ay = a.y;
  const double bx = b.x, by = b.y;
  const double cx = c.x, cy = c.y;
  const double denominator = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
  if (std::abs(denominator) < 1.0e-12)
    return Point2((ax + bx + cx) / 3.0, (ay + by + cy) / 3.0);
  const double a2 = ax * ax + ay * ay;
  const double b2 = bx * bx + by * by;
  const double c2 = cx * cx + cy * cy;
  return Point2(
      (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / denominator,
      (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / denominator);
}

Point2 clampToMap(const Point2 &point, const PolygonTerrainSettings &settings)
{
  return Point2(
      std::clamp(point.x, 0.0, settings.widthMetres),
      std::clamp(point.y, 0.0, settings.heightMetres));
}

TerrainFeatures buildTerrainFeatures(const PolygonTerrainSettings &settings)
{
  TerrainFeatures features;
  const double extent = std::max(settings.widthMetres, settings.heightMetres);
  const double worldWeight = smoothstep(std::clamp((extent - 6000.0) / 8000.0, 0.0, 1.0));
  const uint64_t islandSeed = settings.seed ^ 0x8b8b1f55d43536a1ull;
  features.island = IslandProfile{
      Point2(
          (0.46 + hashUnit(islandSeed) * 0.08) * settings.widthMetres,
          (0.46 + hashUnit(islandSeed ^ 0x6a09e667f3bcc909ull) * 0.08) * settings.heightMetres),
      (0.38 + hashUnit(islandSeed ^ 0xbb67ae8584caa73bull) * 0.11) * settings.widthMetres,
      (0.38 + hashUnit(islandSeed ^ 0xa4093822299f31d0ull) * 0.11) * settings.heightMetres,
      hashUnit(islandSeed ^ 0x94d049bb133111ebull) * 2.0 * kPi,
      0.18 + hashUnit(islandSeed ^ 0x510e527fade682d1ull) * 0.13,
      (0.58 + hashUnit(islandSeed ^ 0x3c6ef372fe94f82bull) * 0.30) * (1.0 - worldWeight * 0.82),
      0.16 + hashUnit(islandSeed ^ 0x1f83d9abfb41bd6bull) * 0.10};

  MacroWorld &macro = features.macro;
  const double targetCellSize = std::clamp(extent / 44.0, 700.0, 3200.0);
  macro.columns = std::clamp(static_cast<uint32_t>(std::ceil(settings.widthMetres / targetCellSize)), 12u, 96u);
  macro.rows = std::clamp(static_cast<uint32_t>(std::ceil(settings.heightMetres / targetCellSize)), 12u, 96u);
  macro.cellWidth = settings.widthMetres / macro.columns;
  macro.cellHeight = settings.heightMetres / macro.rows;
  macro.cells.assign(static_cast<size_t>(macro.columns) * macro.rows, {});
  const uint32_t macroRegionCount = std::clamp(
      static_cast<uint32_t>(std::lround(extent / 3200.0)) + 5u, 5u, 28u);
  macro.regions.resize(macroRegionCount);
  std::vector<uint32_t> regionSeeds;
  regionSeeds.reserve(macroRegionCount);
  const uint32_t macroCellCount = static_cast<uint32_t>(macro.cells.size());
  for (uint32_t region = 0u; region < macroRegionCount; ++region)
  {
    const uint64_t seed = settings.seed ^ (0xc6bc279692b5cc83ull + region * 0x9e3779b97f4a7c15ull);
    uint32_t cell = static_cast<uint32_t>(splitmix64(seed) % macroCellCount);
    for (uint32_t attempt = 0u; attempt < macroCellCount && std::find(regionSeeds.begin(), regionSeeds.end(), cell) != regionSeeds.end(); ++attempt)
      cell = (cell + 1u + static_cast<uint32_t>(splitmix64(seed + attempt) % 17u)) % macroCellCount;
    regionSeeds.push_back(cell);
    const bool continental = region == 0u ? true :
        region == 1u ? false :
        hashUnit(seed ^ 0x94d049bb133111ebull) > 0.46;
    macro.regions[region] = MacroRegion{
        continental,
        continental ?
            0.46 + hashUnit(seed ^ 0xbb67ae8584caa73bull) * 0.32 :
            -0.72 + hashUnit(seed ^ 0xa4093822299f31d0ull) * 0.22,
        0.20 + hashUnit(seed ^ 0x13198a2e03707344ull) * 0.66};
  }

  std::vector<int32_t> assigned(macro.cells.size(), -1);
  std::vector<uint32_t> queue;
  queue.reserve(macro.cells.size());
  for (uint32_t region = 0u; region < regionSeeds.size(); ++region)
  {
    assigned[regionSeeds[region]] = static_cast<int32_t>(region);
    queue.push_back(regionSeeds[region]);
  }
  for (uint32_t queueOut = 0u; queueOut < queue.size(); ++queueOut)
  {
    const uint64_t seed = settings.seed ^ (0x452821e638d01377ull + queueOut * 0xbf58476d1ce4e5b9ull);
    const uint32_t swapIndex = queueOut + static_cast<uint32_t>(splitmix64(seed) % (queue.size() - queueOut));
    std::swap(queue[queueOut], queue[swapIndex]);
    const uint32_t cell = queue[queueOut];
    const uint32_t x = cell % macro.columns;
    const uint32_t y = cell / macro.columns;
    const std::array<std::pair<int32_t, int32_t>, 8> offsets{{
        {-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}}};
    for (const auto &offset : offsets)
    {
      const int32_t nx = static_cast<int32_t>(x) + offset.first;
      const int32_t ny = static_cast<int32_t>(y) + offset.second;
      if (nx < 0 || ny < 0 || nx >= static_cast<int32_t>(macro.columns) || ny >= static_cast<int32_t>(macro.rows)) continue;
      const uint32_t neighbour = macroCellIndex(macro, static_cast<uint32_t>(nx), static_cast<uint32_t>(ny));
      if (assigned[neighbour] != -1) continue;
      assigned[neighbour] = assigned[cell];
      queue.push_back(neighbour);
    }
  }
  for (uint32_t cell = 0u; cell < macroCellCount; ++cell)
  {
    if (assigned[cell] < 0) assigned[cell] = 0;
    MacroCell &macroCell = macro.cells[cell];
    macroCell.region = static_cast<uint32_t>(assigned[cell]);
    const MacroRegion &region = macro.regions[macroCell.region];
    const uint32_t x = cell % macro.columns;
    const uint32_t y = cell / macro.columns;
    const Point2 centre(
        (static_cast<double>(x) + 0.5) * macro.cellWidth,
        (static_cast<double>(y) + 0.5) * macro.cellHeight);
    const double regionalNoise =
        (metricFractalNoise(centre, extent * 0.18, settings.seed ^ 0x243f6a8885a308d3ull) - 0.5) * 0.28;
    macroCell.continent = region.baseElevation + regionalNoise;
    macroCell.moisture = region.moisture;
  }
  for (uint32_t y = 0u; y < macro.rows; ++y)
    for (uint32_t x = 0u; x < macro.columns; ++x)
    {
      MacroCell &cell = macro.cells[macroCellIndex(macro, x, y)];
      const bool continental = macro.regions[cell.region].continental;
      uint32_t opposite = 0u;
      uint32_t differentRegion = 0u;
      for (int32_t oy = -1; oy <= 1; ++oy)
        for (int32_t ox = -1; ox <= 1; ++ox)
        {
          if (ox == 0 && oy == 0) continue;
          const int32_t nx = static_cast<int32_t>(x) + ox;
          const int32_t ny = static_cast<int32_t>(y) + oy;
          if (nx < 0 || ny < 0 || nx >= static_cast<int32_t>(macro.columns) || ny >= static_cast<int32_t>(macro.rows)) continue;
          const MacroCell &neighbour = macro.cells[macroCellIndex(macro, static_cast<uint32_t>(nx), static_cast<uint32_t>(ny))];
          if (neighbour.region != cell.region) ++differentRegion;
          if (macro.regions[neighbour.region].continental != continental) ++opposite;
        }
      const double boundary = static_cast<double>(differentRegion) / 8.0;
      const double coast = static_cast<double>(opposite) / 8.0;
      cell.uplift = boundary * (continental ? 0.32 : 0.12) + coast * 0.18;
      cell.continent += continental ? -coast * 0.22 + boundary * 0.12 : coast * 0.28 - boundary * 0.08;
    }

  const uint32_t plateCount = worldWeight > 0.35 ?
      std::clamp(static_cast<uint32_t>(std::lround(extent / 2600.0)) + 8u, 8u, 36u) :
      std::clamp(static_cast<uint32_t>(std::lround(extent / 1700.0)) + 4u, 5u, 16u);
  features.plates.reserve(plateCount);
  for (uint32_t plate = 0u; plate < plateCount; ++plate)
  {
    const uint64_t seed = settings.seed ^ (0x2f39a15d31b53f75ull + plate * 0x9e3779b97f4a7c15ull);
    if (worldWeight > 0.35)
    {
      const uint32_t cell = static_cast<uint32_t>(splitmix64(seed ^ 0x510e527fade682d1ull) % macroCellCount);
      const uint32_t cellX = cell % macro.columns;
      const uint32_t cellY = cell / macro.columns;
      const MacroCell &macroCell = macro.cells[cell];
      const bool continental = macro.regions[macroCell.region].continental;
      const Point2 centre(
          std::clamp((static_cast<double>(cellX) + 0.18 + hashUnit(seed) * 0.64) * macro.cellWidth, 0.0, settings.widthMetres),
          std::clamp((static_cast<double>(cellY) + 0.18 + hashUnit(seed ^ 0x6a09e667f3bcc909ull) * 0.64) * macro.cellHeight, 0.0, settings.heightMetres));
      const double angle = hashUnit(seed ^ 0x94d049bb133111ebull) * 2.0 * kPi;
      const double speed = 0.40 + hashUnit(seed ^ 0xbf58476d1ce4e5b9ull) * 0.92;
      features.plates.push_back(TectonicPlate{
          centre,
          std::cos(angle) * speed,
          std::sin(angle) * speed,
          continental ?
              0.08 + std::max(0.0, macroCell.continent) * 0.26 :
              -0.34 + hashUnit(seed ^ 0xa4093822299f31d0ull) * 0.16,
          continental});
      continue;
    }
    const double ringAngle =
        (static_cast<double>(plate) / std::max(1u, plateCount - 1u)) * 2.0 * kPi +
        (hashUnit(seed ^ 0x94d049bb133111ebull) - 0.5) * 0.54;
    const double ringRadius = (0.34 + hashUnit(seed ^ 0xbf58476d1ce4e5b9ull) * 0.36) * extent;
    Point2 centre = features.island.centre;
    double velocityX = 0.0;
    double velocityY = 0.0;
    double baseElevation = 0.34 + hashUnit(seed ^ 0xbb67ae8584caa73bull) * 0.16;
    bool continental = true;
    if (plate != 0u)
    {
      centre = clampToMap(Point2(
          features.island.centre.x + std::cos(ringAngle) * ringRadius,
          features.island.centre.y + std::sin(ringAngle) * ringRadius),
          settings);
      double inwardX = features.island.centre.x - centre.x;
      double inwardY = features.island.centre.y - centre.y;
      const double inwardLength = std::sqrt(inwardX * inwardX + inwardY * inwardY);
      if (inwardLength > 0.0)
      {
        inwardX /= inwardLength;
        inwardY /= inwardLength;
      }
      const double speed = 0.55 + hashUnit(seed ^ 0xd1b54a32d192ed03ull) * 0.72;
      const double shearAngle = (hashUnit(seed ^ 0xa4093822299f31d0ull) - 0.5) * 0.82;
      velocityX = inwardX * std::cos(shearAngle) - inwardY * std::sin(shearAngle);
      velocityY = inwardX * std::sin(shearAngle) + inwardY * std::cos(shearAngle);
      velocityX *= speed;
      velocityY *= speed;
      continental = hashUnit(seed ^ 0x13198a2e03707344ull) > 0.62;
      baseElevation = continental ?
          0.08 + hashUnit(seed ^ 0xbb67ae8584caa73bull) * 0.18 :
          -0.34 + hashUnit(seed ^ 0x243f6a8885a308d3ull) * 0.18;
    }
    features.plates.push_back(TectonicPlate{
        centre,
        velocityX,
        velocityY,
        baseElevation,
        continental});
  }

  const auto addMountainChain = [&](const Point2 &centre, const double angle, const double length,
                                    const double halfWidth, const double strength, const uint64_t seed)
  {
    const double normalAngle = angle + kPi * 0.5;
    const double bend = (hashUnit(seed ^ 0x243f6a8885a308d3ull) - 0.5) * 0.24 * length;
    const uint32_t segmentCount = 4u + static_cast<uint32_t>(splitmix64(seed ^ 0xa4093822299f31d0ull) % 4u);
    std::vector<Point2> chain;
    chain.reserve(segmentCount + 1u);
    for (uint32_t point = 0u; point <= segmentCount; ++point)
    {
      const double t = segmentCount == 0u ? 0.0 : static_cast<double>(point) / segmentCount;
      const double along = (t - 0.5) * length;
      const double sinBend = std::sin((t - 0.5) * kPi);
      const double randomBend =
          (valueNoise(t * 3.0, static_cast<double>(seed & 0xffffu) * 0.001, seed) - 0.5) * length * 0.11;
      const double offset = sinBend * bend + randomBend;
      chain.push_back(clampToMap(Point2(
          centre.x + std::cos(angle) * along + std::cos(normalAngle) * offset,
          centre.y + std::sin(angle) * along + std::sin(normalAngle) * offset),
          settings));
    }
    for (uint32_t segment = 0u; segment + 1u < chain.size(); ++segment)
    {
      const double t = static_cast<double>(segment) / std::max(1u, segmentCount - 1u);
      const double taper = 0.58 + std::sin(t * kPi) * 0.42;
      features.mountains.push_back(MountainRange{
          chain[segment],
          chain[segment + 1u],
          std::max(settings.siteRadiusMetres * 2.5, halfWidth * (0.75 + taper * 0.55)),
          strength * taper});
    }
  };

  features.mountains.reserve(plateCount * 5u);
  for (uint32_t a = 0u; a < features.plates.size(); ++a)
    for (uint32_t b = a + 1u; b < features.plates.size(); ++b)
    {
      const TectonicPlate &first = features.plates[a];
      const TectonicPlate &second = features.plates[b];
      double nx = second.centre.x - first.centre.x;
      double ny = second.centre.y - first.centre.y;
      const double distance = std::sqrt(nx * nx + ny * ny);
      if (distance <= 0.0 || distance > extent * 0.62) continue;
      nx /= distance;
      ny /= distance;
      const double relativeVelocity = (first.velocityX - second.velocityX) * nx + (first.velocityY - second.velocityY) * ny;
      const double convergence = std::max(0.0, relativeVelocity);
      if (convergence < 0.28) continue;
      const Point2 centre(
          (first.centre.x + second.centre.x) * 0.5,
          (first.centre.y + second.centre.y) * 0.5);
      const double angle = std::atan2(ny, nx) + kPi * 0.5;
      const double continentalBonus = first.continental && second.continental ? 1.25 : 0.82;
      const uint64_t seed = settings.seed ^ (static_cast<uint64_t>(a) * 0x517cc1b727220a95ull) ^
          (static_cast<uint64_t>(b) * 0xbf58476d1ce4e5b9ull);
      addMountainChain(
          centre,
          angle,
          std::clamp(distance * (0.72 + hashUnit(seed) * 0.38), extent * 0.22, extent * 0.72),
          (0.018 + hashUnit(seed ^ 0x6a09e667f3bcc909ull) * 0.026) * extent,
          std::clamp((0.42 + convergence * 0.36) * continentalBonus, 0.30, 0.95),
          seed);
      if (features.mountains.size() > plateCount * 6u) break;
    }

  const double islandMountainWeight = 1.0 - worldWeight;
  const uint32_t islandRangeCount = islandMountainWeight <= 0.12 ? 0u :
      2u + static_cast<uint32_t>(splitmix64(settings.seed ^ 0x5be0cd19137e2179ull) % 2u);
  for (uint32_t range = 0u; range < islandRangeCount; ++range)
  {
    const uint64_t seed = settings.seed ^ (0x9b05688c2b3e6c1full + range * 0x94d049bb133111ebull);
    const double angle = features.island.angle +
        (static_cast<double>(range) / std::max(1u, islandRangeCount)) * kPi +
        (hashUnit(seed) - 0.5) * 0.72;
    const double length = (0.42 + hashUnit(seed ^ 0x6a09e667f3bcc909ull) * 0.22) *
        std::max(features.island.radiusX, features.island.radiusY);
    const Point2 centre(
        features.island.centre.x +
            (hashUnit(seed ^ 0xbb67ae8584caa73bull) - 0.5) * features.island.radiusX * 0.30,
        features.island.centre.y +
            (hashUnit(seed ^ 0xa4093822299f31d0ull) - 0.5) * features.island.radiusY * 0.30);
    addMountainChain(
        clampToMap(centre, settings),
        angle,
        length,
        (0.030 + hashUnit(seed ^ 0x13198a2e03707344ull) * 0.020) * extent,
        (0.54 + hashUnit(seed ^ 0x243f6a8885a308d3ull) * 0.28) * islandMountainWeight,
        seed);
  }

  const uint32_t arcCount = islandMountainWeight <= 0.12 ? 0u :
      2u + static_cast<uint32_t>(splitmix64(settings.seed ^ 0xd1b54a32d192ed03ull) % 3u);
  for (uint32_t arc = 0u; arc < arcCount; ++arc)
  {
    const uint64_t seed = settings.seed ^ (0xcbbb9d5dc1059ed8ull + arc * 0xbf58476d1ce4e5b9ull);
    const double angle = hashUnit(seed) * 2.0 * kPi;
    const double ring = (0.38 + hashUnit(seed ^ 0x94d049bb133111ebull) * 0.30);
    const Point2 centre = clampToMap(Point2(
        features.island.centre.x + std::cos(angle) * features.island.radiusX * ring,
        features.island.centre.y + std::sin(angle) * features.island.radiusY * ring),
        settings);
    addMountainChain(
        centre,
        angle + kPi * 0.5 + (hashUnit(seed ^ 0x6a09e667f3bcc909ull) - 0.5) * 0.55,
        (0.22 + hashUnit(seed ^ 0xbb67ae8584caa73bull) * 0.20) *
            std::max(features.island.radiusX, features.island.radiusY),
        (0.018 + hashUnit(seed ^ 0xa4093822299f31d0ull) * 0.018) * extent,
        (0.34 + hashUnit(seed ^ 0x13198a2e03707344ull) * 0.24) * islandMountainWeight,
        seed);
  }

  const uint32_t supplementalRangeCount = std::clamp(
      static_cast<uint32_t>(std::lround(extent / 3600.0)), 1u, 4u);
  for (uint32_t range = 0u; range < supplementalRangeCount; ++range)
  {
    const uint64_t seed = settings.seed ^ (0x517cc1b727220a95ull + range * 0x9e3779b97f4a7c15ull);
    addMountainChain(
        Point2(
            (0.12 + hashUnit(seed) * 0.76) * settings.widthMetres,
            (0.12 + hashUnit(seed ^ 0xbf58476d1ce4e5b9ull) * 0.76) * settings.heightMetres),
        hashUnit(seed ^ 0x94d049bb133111ebull) * 2.0 * kPi,
        (0.32 + hashUnit(seed ^ 0xd1b54a32d192ed03ull) * 0.28) * extent,
        (0.018 + hashUnit(seed ^ 0x6a09e667f3bcc909ull) * 0.025) * extent,
        0.34 + hashUnit(seed ^ 0xbb67ae8584caa73bull) * 0.22,
        seed);
  }

  const uint32_t basinCount = std::clamp(
      static_cast<uint32_t>(std::lround(extent / 1100.0)), 1u, 9u);
  features.lakeBasins.reserve(basinCount);
  for (uint32_t basin = 0u; basin < basinCount; ++basin)
  {
    const uint64_t seed = settings.seed ^ (0x452821e638d01377ull + basin * 0xbf58476d1ce4e5b9ull);
    const Point2 centre(
        (0.18 + hashUnit(seed) * 0.64) * settings.widthMetres,
        (0.18 + hashUnit(seed ^ 0x94d049bb133111ebull) * 0.64) * settings.heightMetres);
    const double mountain = mountainInfluence(centre, features, settings);
    const double radius = (0.035 + hashUnit(seed ^ 0xd1b54a32d192ed03ull) * 0.055) * extent;
    features.lakeBasins.push_back(LakeBasin{
        centre,
        std::max(settings.siteRadiusMetres * 2.0, radius * (0.85 + hashUnit(seed ^ 0x6a09e667f3bcc909ull) * 0.75)),
        std::max(settings.siteRadiusMetres * 1.5, radius * (0.45 + hashUnit(seed ^ 0xbb67ae8584caa73bull) * 0.45)),
        hashUnit(seed ^ 0xa4093822299f31d0ull) * 2.0 * kPi,
        (mountain > 0.32 ? 0.50 : 0.66) + hashUnit(seed ^ 0x13198a2e03707344ull) * 0.28});
  }
  return features;
}

void uniqueSort(std::vector<uint32_t> &values)
{
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

bool fail(std::string *error, const char *message)
{
  if (error != nullptr) *error = message;
  return false;
}

bool reportProgress(
    const PolygonTerrainProgress *progress, std::mutex &mutex, const char *phase, const double fraction)
{
  if (progress == nullptr || !progress->callback) return true;
  std::lock_guard<std::mutex> lock(mutex);
  return progress->callback(phase, std::clamp(fraction, 0.0, 1.0));
}

GridIndex buildGrid(const std::vector<Site> &sites, const PolygonTerrainSettings &settings)
{
  GridIndex grid{};
  grid.cellSize = settings.siteRadiusMetres;
  grid.width = settings.widthMetres;
  grid.height = settings.heightMetres;
  grid.columns = static_cast<uint32_t>(std::ceil(grid.width / grid.cellSize));
  grid.rows = static_cast<uint32_t>(std::ceil(grid.height / grid.cellSize));
  grid.cells.resize(static_cast<size_t>(grid.columns) * grid.rows);
  for (uint32_t site = 0u; site < sites.size(); ++site)
  {
    const uint32_t x = std::min(
        grid.columns - 1u, static_cast<uint32_t>(sites[site].position.x / grid.cellSize));
    const uint32_t y = std::min(
        grid.rows - 1u, static_cast<uint32_t>(sites[site].position.y / grid.cellSize));
    grid.cells[static_cast<size_t>(y) * grid.columns + x].push_back(site);
  }
  return grid;
}

double squaredDistanceToRectangleBoundary(
    const Point2 &point, const double left, const double bottom, const double right, const double top)
{
  const double distance = std::min(
      std::min(point.x - left, right - point.x), std::min(point.y - bottom, top - point.y));
  return distance * distance;
}

uint32_t nearestSite(const GridIndex &grid, const std::vector<Site> &sites, const Point2 &point)
{
  const int32_t cellX = std::clamp(
      static_cast<int32_t>(point.x / grid.cellSize), 0, static_cast<int32_t>(grid.columns) - 1);
  const int32_t cellY = std::clamp(
      static_cast<int32_t>(point.y / grid.cellSize), 0, static_cast<int32_t>(grid.rows) - 1);
  uint32_t bestSite = 0u;
  double bestDistance = std::numeric_limits<double>::max();
  const int32_t maximumRadius = std::max(grid.columns, grid.rows);
  for (int32_t radius = 0; radius <= maximumRadius; ++radius)
  {
    const int32_t firstX = std::max(0, cellX - radius);
    const int32_t lastX = std::min(static_cast<int32_t>(grid.columns) - 1, cellX + radius);
    const int32_t firstY = std::max(0, cellY - radius);
    const int32_t lastY = std::min(static_cast<int32_t>(grid.rows) - 1, cellY + radius);
    for (int32_t y = firstY; y <= lastY; ++y)
      for (int32_t x = firstX; x <= lastX; ++x)
      {
        if (radius != 0 && x != firstX && x != lastX && y != firstY && y != lastY) continue;
        for (const uint32_t site : grid.cells[static_cast<size_t>(y) * grid.columns + x])
        {
          const double dx = sites[site].position.x - point.x;
          const double dy = sites[site].position.y - point.y;
          const double distance = dx * dx + dy * dy;
          if (distance < bestDistance || (distance == bestDistance && site < bestSite))
          {
            bestDistance = distance;
            bestSite = site;
          }
        }
      }
    const double left = static_cast<double>(firstX) * grid.cellSize;
    const double right = std::min(grid.width, static_cast<double>(lastX + 1) * grid.cellSize);
    const double bottom = static_cast<double>(firstY) * grid.cellSize;
    const double top = std::min(grid.height, static_cast<double>(lastY + 1) * grid.cellSize);
    if (bestDistance <= squaredDistanceToRectangleBoundary(point, left, bottom, right, top)) break;
  }
  return bestSite;
}

uint8_t regionColour(const uint32_t site, const bool water)
{
  if (water) return 0u;
  return static_cast<uint8_t>(48u + (splitmix64(site) % 208u));
}

PolygonTerrainMaterial materialForSite(const Site &site)
{
  if (site.ocean) return PolygonTerrainMaterial::Ocean;
  if (site.water)
  {
    return site.temperature < 0.16 ? PolygonTerrainMaterial::Ice : PolygonTerrainMaterial::Lake;
  }
  if (site.coast) return PolygonTerrainMaterial::Beach;
  const double temperature = site.temperature;
  const double precipitation = site.precipitation;
  if (temperature < 0.10)
    return precipitation > 0.32 ? PolygonTerrainMaterial::Snow : PolygonTerrainMaterial::Ice;
  if (temperature < 0.20)
    return precipitation > 0.48 ? PolygonTerrainMaterial::Snow :
        precipitation > 0.22 ? PolygonTerrainMaterial::Tundra : PolygonTerrainMaterial::Bare;
  if (site.elevation > 0.82)
  {
    if (temperature < 0.34 || precipitation > 0.54) return PolygonTerrainMaterial::Snow;
    if (precipitation > 0.32) return PolygonTerrainMaterial::Tundra;
    if (precipitation > 0.16) return PolygonTerrainMaterial::Bare;
    return PolygonTerrainMaterial::Scorched;
  }
  if (temperature > 0.72 && precipitation < 0.24) return PolygonTerrainMaterial::SubtropicalDesert;
  if (temperature > 0.50 && precipitation < 0.18) return PolygonTerrainMaterial::TemperateDesert;
  if (precipitation < 0.12)
    return temperature < 0.34 ? PolygonTerrainMaterial::Bare : PolygonTerrainMaterial::TemperateDesert;
  if (site.elevation > 0.60)
  {
    if (temperature < 0.34) return PolygonTerrainMaterial::Tundra;
    if (precipitation > 0.64) return PolygonTerrainMaterial::Taiga;
    if (precipitation > 0.30) return PolygonTerrainMaterial::Shrubland;
    return PolygonTerrainMaterial::TemperateDesert;
  }
  if (temperature < 0.34)
    return precipitation > 0.45 ? PolygonTerrainMaterial::Taiga : PolygonTerrainMaterial::Tundra;
  if (temperature < 0.58)
  {
    if (precipitation > 0.78) return PolygonTerrainMaterial::TemperateRainForest;
    if (precipitation > 0.48) return PolygonTerrainMaterial::TemperateDeciduousForest;
    if (precipitation > 0.22) return PolygonTerrainMaterial::Grassland;
    return PolygonTerrainMaterial::TemperateDesert;
  }
  if (precipitation > 0.72) return PolygonTerrainMaterial::TropicalRainForest;
  if (precipitation > 0.42) return PolygonTerrainMaterial::TropicalSeasonalForest;
  if (precipitation > 0.22) return PolygonTerrainMaterial::Grassland;
  return PolygonTerrainMaterial::SubtropicalDesert;
}

double rasterElevation(
    const uint32_t siteIndex, const Point2 &point, const std::vector<Site> &sites,
    const PolygonTerrainSettings &settings)
{
  const Site &site = sites[siteIndex];
  if (site.water || site.ocean) return 0.0;

  double weightedElevation = 0.0;
  double totalWeight = 0.0;
  const double smoothingRadius = settings.siteRadiusMetres * 0.55;
  const double smoothingRadiusSquared = smoothingRadius * smoothingRadius;
  const auto accumulate = [&](const uint32_t candidateIndex)
  {
    const Site &candidate = sites[candidateIndex];
    if (candidate.water || candidate.ocean) return;
    const double dx = candidate.position.x - point.x;
    const double dy = candidate.position.y - point.y;
    const double weight = 1.0 / (dx * dx + dy * dy + smoothingRadiusSquared);
    weightedElevation += candidate.elevation * weight;
    totalWeight += weight;
  };

  accumulate(siteIndex);
  for (const uint32_t neighbour : site.neighbours) accumulate(neighbour);
  double elevation = totalWeight > 0.0 ? weightedElevation / totalWeight : site.elevation;
  const double detailScale =
      (0.014 + site.mountain * 0.050 + (1.0 - site.plain) * 0.010) *
      (1.0 - site.plain * 0.62) * (1.0 - site.valley * 0.62);
  const double detailWavelength = std::max(45.0, settings.siteRadiusMetres * 4.5);
  const double detail =
      (metricFractalNoise(point, detailWavelength, settings.seed ^ 0xa4093822299f31d0ull) - 0.5) *
      detailScale;
  elevation = elevation + detail;
  return std::clamp(elevation, 0.0, 1.0);
}

void drawRiverLine(
    const Point2 &from, const Point2 &to, const uint32_t flow, const PolygonTerrainSettings &settings,
    PolygonTerrainMaps &maps)
{
  const auto toX = [&](const double x)
  {
    return static_cast<int32_t>(std::lround(x / settings.widthMetres * (maps.width - 1u)));
  };
  const auto toY = [&](const double y)
  {
    return static_cast<int32_t>(std::lround(y / settings.heightMetres * (maps.height - 1u)));
  };
  int32_t x0 = toX(from.x), y0 = toY(from.y), x1 = toX(to.x), y1 = toY(to.y);
  const int32_t dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
  const int32_t stepX = x0 < x1 ? 1 : -1;
  const int32_t stepY = y0 < y1 ? 1 : -1;
  int32_t error = dx + dy;
  const int32_t radius = flow >= 4u ? 2 : flow >= 2u ? 1 : 0;
  const uint8_t value = static_cast<uint8_t>(std::min(255u, 96u + flow * 24u));
  for (;;)
  {
    for (int32_t oy = -radius; oy <= radius; ++oy)
      for (int32_t ox = -radius; ox <= radius; ++ox)
      {
        const int32_t x = x0 + ox;
        const int32_t y = y0 + oy;
        if (x < 0 || y < 0 || x >= static_cast<int32_t>(maps.width) || y >= static_cast<int32_t>(maps.height)) continue;
        uint8_t &pixel = maps.rivers[static_cast<size_t>(y) * maps.width + x];
        pixel = std::max(pixel, value);
      }
    if (x0 == x1 && y0 == y1) break;
    const int32_t twiceError = error * 2;
    if (twiceError >= dy)
    {
      error += dy;
      x0 += stepX;
    }
    if (twiceError <= dx)
    {
      error += dx;
      y0 += stepY;
    }
  }
}

uint32_t automaticRiverSourceCount(
    const PolygonTerrainSettings &settings, const size_t landCornerCount, const size_t candidateCount,
    const size_t cornerCount)
{
  if (candidateCount == 0u || cornerCount == 0u) return 0u;
  const double landFraction = std::clamp(static_cast<double>(landCornerCount) / static_cast<double>(cornerCount), 0.05, 1.0);
  const double landAreaKm2 = settings.widthMetres * settings.heightMetres * landFraction / 1000000.0;
  const double spacingFactor = std::clamp(std::sqrt(18.0 / std::max(1.0, settings.siteRadiusMetres)), 0.75, 1.80);
  const double scaleDriven = (std::sqrt(std::max(0.0, landAreaKm2)) * 12.0 + landAreaKm2 * 0.80) * spacingFactor;
  const uint32_t desired = static_cast<uint32_t>(std::lround(std::clamp(scaleDriven, 3.0, 4096.0)));
  const uint32_t graphLimit = static_cast<uint32_t>(std::max<size_t>(1u, landCornerCount / 10u));
  return std::max(1u, std::min<uint32_t>(
      desired, std::min<uint32_t>(static_cast<uint32_t>(candidateCount), graphLimit)));
}

} // namespace

bool PolygonTerrainGenerator::generate(
    const PolygonTerrainSettings &settings, const PolygonTerrainRasterSettings &rasterSettings,
    PolygonTerrainMaps &maps, PolygonTerrainStatistics *statistics, std::string *error,
    const PolygonTerrainProgress *progress)
{
  if (settings.widthMetres <= 0.0 || settings.heightMetres <= 0.0 || settings.siteRadiusMetres <= 0.0)
    return fail(error, "Terrain dimensions and site radius must be positive");
  if (rasterSettings.width == 0u || rasterSettings.height == 0u)
    return fail(error, "Terrain raster dimensions must be positive");

  std::mutex progressMutex;
  if (!reportProgress(progress, progressMutex, "Sampling sites", 0.02))
    return fail(error, "Terrain generation cancelled");

  initialisePredicates();
  PoissonDiskSampler sampler;
  PoissonDiskSampler::Settings poissonSettings{};
  poissonSettings.width = settings.widthMetres;
  poissonSettings.height = settings.heightMetres;
  poissonSettings.radius = settings.siteRadiusMetres;
  poissonSettings.seed = settings.seed;
  poissonSettings.method = PoissonDiskSampler::Method::RandomThrows;
  std::vector<Point2> points = sampler.generate(poissonSettings);
  if (!reportProgress(progress, progressMutex, "Triangulating sites", 0.12))
    return fail(error, "Terrain generation cancelled");
  points.emplace_back(0.0, 0.0);
  points.emplace_back(settings.widthMetres, 0.0);
  points.emplace_back(settings.widthMetres, settings.heightMetres);
  points.emplace_back(0.0, settings.heightMetres);
  if (points.size() < 7u) return fail(error, "Poisson sampler produced too few points");

  const uint32_t inputPointCount = static_cast<uint32_t>(points.size());
  const TerrainFeatures terrainFeatures = buildTerrainFeatures(settings);
  ConstrainedDelaunayTriangulator triangulator;
  // The four added rectangle corners are the convex hull because Poisson sites
  // lie strictly within the requested domain. That gives us a rectangular map
  // boundary without inserting constraints through the triangulator's current
  // fixed-pass constraint path; its own documentation notes arrangements that
  // can stall when constraint insertion and Delaunay restoration conflict.
  if (!triangulator.triangulate(points, {}))
    return fail(error, "Delaunay triangulation did not converge");
  if (!reportProgress(progress, progressMutex, "Building Voronoi graph", 0.32))
    return fail(error, "Terrain generation cancelled");

  std::vector<Site> sites(inputPointCount);
  for (uint32_t site = 0u; site < inputPointCount; ++site)
  {
    sites[site].position = points[site];
    sites[site].border = isBoundaryPoint(points[site], settings);
    sites[site].water = sites[site].border;
  }

  std::vector<Triangle> triangles;
  std::vector<Corner> corners;
  std::map<uint64_t, EdgeRecord> edges;
  const auto rawTriangles = triangulator.collectTriangles();
  for (size_t raw = 0u; raw + 2u < rawTriangles.size(); raw += 3u)
  {
    const int64_t a = rawTriangles[raw];
    const int64_t b = rawTriangles[raw + 1u];
    const int64_t c = rawTriangles[raw + 2u];
    if (a < 0 || b < 0 || c < 0 || a >= inputPointCount || b >= inputPointCount || c >= inputPointCount) continue;
    Triangle triangle{};
    triangle.sites = {static_cast<uint32_t>(a), static_cast<uint32_t>(b), static_cast<uint32_t>(c)};
    triangle.corner = static_cast<uint32_t>(corners.size());
    Corner corner{};
    corner.position = clampToMap(circumcentre(points[a], points[b], points[c]), settings);
    corner.border = isBoundaryPoint(corner.position, settings);
    corner.touches.assign(triangle.sites.begin(), triangle.sites.end());
    corners.push_back(corner);
    triangles.push_back(triangle);
    for (uint32_t edge = 0u; edge < 3u; ++edge)
    {
      const uint32_t first = triangle.sites[edge];
      const uint32_t second = triangle.sites[(edge + 1u) % 3u];
      sites[first].neighbours.push_back(second);
      sites[second].neighbours.push_back(first);
      edges[edgeKey(first, second)].triangles.push_back(triangle.corner);
    }
    for (const uint32_t site : triangle.sites) sites[site].corners.push_back(triangle.corner);
  }
  if (triangles.empty()) return fail(error, "Triangulation contains no finite map triangles");

  for (Site &site : sites)
  {
    uniqueSort(site.neighbours);
    uniqueSort(site.corners);
  }
  for (const auto &entry : edges)
    if (entry.second.triangles.size() == 2u)
    {
      const uint32_t first = entry.second.triangles[0];
      const uint32_t second = entry.second.triangles[1];
      corners[first].neighbours.push_back(second);
      corners[second].neighbours.push_back(first);
    }
  for (Corner &corner : corners) uniqueSort(corner.neighbours);
  if (!reportProgress(progress, progressMutex, "Classifying water", 0.42))
    return fail(error, "Terrain generation cancelled");

  std::deque<uint32_t> elevationQueue;
  for (uint32_t cornerIndex = 0u; cornerIndex < corners.size(); ++cornerIndex)
  {
    Corner &corner = corners[cornerIndex];
    corner.water = corner.border || isTerrainWater(corner.position, settings, terrainFeatures);
    corner.elevation = corner.border ? 0.0 : std::numeric_limits<double>::infinity();
    if (corner.border) elevationQueue.push_back(cornerIndex);
  }
  if (elevationQueue.empty())
  {
    for (uint32_t cornerIndex = 0u; cornerIndex < corners.size(); ++cornerIndex)
      if (corners[cornerIndex].water)
      {
        corners[cornerIndex].elevation = 0.0;
        elevationQueue.push_back(cornerIndex);
      }
  }
  while (!elevationQueue.empty())
  {
    const uint32_t cornerIndex = elevationQueue.front();
    elevationQueue.pop_front();
    for (const uint32_t neighbour : corners[cornerIndex].neighbours)
    {
      double newElevation = corners[cornerIndex].elevation + 0.01;
      if (!corners[cornerIndex].water && !corners[neighbour].water) newElevation += 1.0;
      if (newElevation < corners[neighbour].elevation)
      {
        corners[neighbour].elevation = newElevation;
        elevationQueue.push_back(neighbour);
      }
    }
  }

  constexpr double lakeThreshold = 0.3;
  std::deque<uint32_t> oceanQueue;
  for (uint32_t siteIndex = 0u; siteIndex < sites.size(); ++siteIndex)
  {
    Site &site = sites[siteIndex];
    uint32_t waterCorners = 0u;
    for (const uint32_t corner : site.corners)
    {
      if (corners[corner].border)
      {
        site.border = true;
        site.ocean = true;
        corners[corner].water = true;
      }
      if (corners[corner].water) ++waterCorners;
    }
    site.water = site.ocean || site.border ||
        (!site.corners.empty() && static_cast<double>(waterCorners) >= static_cast<double>(site.corners.size()) * lakeThreshold);
    if (site.ocean)
      oceanQueue.push_back(siteIndex);
  }
  while (!oceanQueue.empty())
  {
    const uint32_t siteIndex = oceanQueue.front();
    oceanQueue.pop_front();
    for (const uint32_t neighbour : sites[siteIndex].neighbours)
      if (sites[neighbour].water && !sites[neighbour].ocean)
      {
        sites[neighbour].ocean = true;
        oceanQueue.push_back(neighbour);
      }
  }
  if (!reportProgress(progress, progressMutex, "Solving elevation", 0.54))
    return fail(error, "Terrain generation cancelled");

  for (Site &site : sites)
  {
    uint32_t oceanNeighbours = 0u;
    uint32_t landNeighbours = 0u;
    for (const uint32_t neighbour : site.neighbours)
    {
      if (sites[neighbour].ocean) ++oceanNeighbours;
      if (!sites[neighbour].water) ++landNeighbours;
    }
    site.coast = oceanNeighbours > 0u && landNeighbours > 0u;
  }
  for (Corner &corner : corners)
  {
    uint32_t oceanTouches = 0u;
    uint32_t landTouches = 0u;
    for (const uint32_t site : corner.touches)
    {
      if (sites[site].ocean) ++oceanTouches;
      if (!sites[site].water) ++landTouches;
    }
    corner.ocean = !corner.touches.empty() && oceanTouches == corner.touches.size();
    corner.coast = oceanTouches > 0u && landTouches > 0u;
    corner.water = corner.border || ((landTouches != corner.touches.size()) && !corner.coast);
  }

  std::vector<uint32_t> landCorners;
  for (uint32_t cornerIndex = 0u; cornerIndex < corners.size(); ++cornerIndex)
    if (!corners[cornerIndex].ocean && !corners[cornerIndex].coast && std::isfinite(corners[cornerIndex].elevation))
      landCorners.push_back(cornerIndex);
  std::sort(
      landCorners.begin(), landCorners.end(),
      [&corners](const uint32_t a, const uint32_t b)
      {
        if (corners[a].elevation != corners[b].elevation) return corners[a].elevation < corners[b].elevation;
        return a < b;
      });
  constexpr double elevationScaleFactor = 1.1;
  for (uint32_t rank = 0u; rank < landCorners.size(); ++rank)
  {
    const double y = landCorners.size() <= 1u ? 1.0 : static_cast<double>(rank) / (landCorners.size() - 1u);
    corners[landCorners[rank]].elevation =
        std::min(1.0, std::sqrt(elevationScaleFactor) - std::sqrt(elevationScaleFactor * (1.0 - y)));
  }
  for (const uint32_t cornerIndex : landCorners)
  {
    Corner &corner = corners[cornerIndex];
    const TectonicSample tectonics = sampleTectonics(corner.position, terrainFeatures, settings);
    const double mountain = mountainInfluence(corner.position, terrainFeatures, settings);
    const double plain = plainInfluence(corner.position, settings) * (1.0 - mountain);
    const double detail =
        (metricFractalNoise(corner.position, 820.0, settings.seed ^ 0x243f6a8885a308d3ull) - 0.5) * 0.08;
    const double ridges =
        (ridgedMetricNoise(corner.position, 1150.0, settings.seed ^ 0x3c6ef372fe94f82bull) - 0.44) *
        mountain * 0.18;
    const double hillBillows =
        (billowMetricNoise(corner.position, 680.0, settings.seed ^ 0xa54ff53a5f1d36f1ull) - 0.48) *
        (1.0 - plain) * (1.0 - mountain * 0.35) * 0.075;
    const double warpedLowland =
        (domainWarpedMetricNoise(
             corner.position, 1450.0, std::max(80.0, settings.siteRadiusMetres * 5.0),
             settings.seed ^ 0x510e527fade682d1ull) - 0.5) *
        plain * 0.055;
    double elevation = std::clamp(corner.elevation + detail, 0.0, 1.0);

    const double plainTarget = 0.16 + valueNoise(
        corner.position.x / 1300.0,
        corner.position.y / 1300.0,
        settings.seed ^ 0x13198a2e03707344ull) * 0.14;
    if (elevation < 0.58)
      elevation = elevation * (1.0 - plain * 0.82) + plainTarget * (plain * 0.82);

    elevation += tectonics.convergence * (tectonics.continental ? 0.26 : 0.12);
    elevation -= tectonics.rift * 0.18;
    elevation += ridges + hillBillows + warpedLowland;
    elevation = std::max(elevation, elevation + mountain * (1.0 - elevation) * 0.88);
    corner.elevation = std::clamp(elevation, 0.0, 1.0);
  }
  for (Corner &corner : corners)
    if (corner.ocean || corner.coast)
      corner.elevation = 0.0;

  for (uint32_t cornerIndex = 0u; cornerIndex < corners.size(); ++cornerIndex)
  {
    Corner &corner = corners[cornerIndex];
    corner.downhill = static_cast<int32_t>(cornerIndex);
    for (const uint32_t neighbour : corner.neighbours)
    {
      const uint32_t currentDownhill = static_cast<uint32_t>(corner.downhill);
      const double neighbourElevation = corners[neighbour].elevation;
      const double currentElevation = corners[currentDownhill].elevation;
      if (neighbourElevation < currentElevation - 1.0e-9 ||
          (std::abs(neighbourElevation - currentElevation) <= 1.0e-9 && neighbour < currentDownhill))
        corner.downhill = static_cast<int32_t>(neighbour);
    }
  }

  for (Site &site : sites)
  {
    site.elevation = 0.0;
    if (site.corners.empty()) continue;
    for (const uint32_t corner : site.corners) site.elevation += corners[corner].elevation;
    site.elevation /= site.corners.size();
    if (site.water || site.ocean || site.coast) site.elevation = 0.0;
    site.mountain = site.water ? 0.0 : mountainInfluence(site.position, terrainFeatures, settings);
    site.plain = site.water ? 0.0 : plainInfluence(site.position, settings) * (1.0 - site.mountain);
  }
  if (!reportProgress(progress, progressMutex, "Routing rivers", 0.66))
    return fail(error, "Terrain generation cancelled");

  struct RiverSourceCandidate
  {
    uint32_t corner = 0u;
    double score = 0.0;
    uint64_t tie = 0u;
  };
  std::vector<RiverSourceCandidate> riverSources;
  for (const uint32_t corner : landCorners)
    if (!corners[corner].ocean && !corners[corner].coast &&
        corners[corner].elevation >= 0.3 && corners[corner].elevation <= 0.9)
      {
        const uint64_t tie = splitmix64(settings.seed ^ (static_cast<uint64_t>(corner) * 0x9e3779b97f4a7c15ull));
        const double jitter = static_cast<double>(tie >> 11u) * (1.0 / 9007199254740992.0);
        const double mountain = mountainInfluence(corners[corner].position, terrainFeatures, settings);
        const double sourceBand = 1.0 - std::abs(corners[corner].elevation - 0.64) / 0.34;
        riverSources.push_back(RiverSourceCandidate{
            corner,
            std::clamp(sourceBand, 0.0, 1.0) * 0.44 + corners[corner].elevation * 0.32 +
                mountain * 0.18 + jitter * 0.06,
            tie});
      }
  const auto betterRiverSource = [](const RiverSourceCandidate &a, const RiverSourceCandidate &b)
  {
    if (a.score != b.score) return a.score > b.score;
    if (a.tie != b.tie) return a.tie < b.tie;
    return a.corner < b.corner;
  };
  const uint32_t resolvedRiverSourceCount = settings.riverSourceCount == 0u ?
      automaticRiverSourceCount(settings, landCorners.size(), riverSources.size(), corners.size()) :
      std::min<uint32_t>(settings.riverSourceCount, static_cast<uint32_t>(riverSources.size()));
  if (riverSources.size() > resolvedRiverSourceCount)
  {
    std::nth_element(
        riverSources.begin(), riverSources.begin() + resolvedRiverSourceCount, riverSources.end(),
        betterRiverSource);
    riverSources.resize(resolvedRiverSourceCount);
  }
  std::sort(riverSources.begin(), riverSources.end(), betterRiverSource);

  std::vector<uint32_t> riverFlow(corners.size(), 0u);
  for (const RiverSourceCandidate &source : riverSources)
    ++riverFlow[source.corner];

  std::vector<uint32_t> flowOrder = landCorners;
  std::sort(
      flowOrder.begin(), flowOrder.end(),
      [&corners](const uint32_t a, const uint32_t b)
      {
        if (corners[a].elevation != corners[b].elevation) return corners[a].elevation > corners[b].elevation;
        return a > b;
      });
  std::unordered_map<uint64_t, uint32_t> riverEdges;
  riverEdges.reserve(riverSources.size() * 16u + 32u);
  for (const uint32_t corner : flowOrder)
  {
    const uint32_t flow = riverFlow[corner];
    if (flow == 0u) continue;
    corners[corner].river += flow;
    if (corners[corner].coast) continue;
    const int32_t downhill = corners[corner].downhill;
    if (downhill < 0 || static_cast<uint32_t>(downhill) == corner) continue;
    const uint32_t downhillCorner = static_cast<uint32_t>(downhill);
    if (corners[downhillCorner].elevation > corners[corner].elevation + 1.0e-9) continue;
    riverEdges[edgeKey(corner, downhillCorner)] += flow;
    if (riverFlow[downhillCorner] <= std::numeric_limits<uint32_t>::max() - flow)
      riverFlow[downhillCorner] += flow;
    else
      riverFlow[downhillCorner] = std::numeric_limits<uint32_t>::max();
  }
  for (uint32_t cornerIndex = 0u; cornerIndex < corners.size(); ++cornerIndex)
  {
    const uint32_t flow = riverFlow[cornerIndex];
    if (flow == 0u || !corners[cornerIndex].coast) continue;
    if (corners[cornerIndex].river <= std::numeric_limits<uint32_t>::max() - flow)
      corners[cornerIndex].river += flow;
    else
      corners[cornerIndex].river = std::numeric_limits<uint32_t>::max();
  }

  for (uint32_t cornerIndex = 0u; cornerIndex < corners.size(); ++cornerIndex)
  {
    Corner &corner = corners[cornerIndex];
    if (corner.river == 0u || corner.ocean || corner.coast) continue;
    const double carve = std::min(0.42, 0.12 + std::log2(static_cast<double>(corner.river) + 1.0) * 0.07);
    corner.elevation = std::max(0.0, corner.elevation * (1.0 - carve));
    for (const uint32_t neighbour : corner.neighbours)
      if (!corners[neighbour].ocean && !corners[neighbour].coast)
        corners[neighbour].elevation = std::max(0.0, corners[neighbour].elevation * (1.0 - carve * 0.45));
  }

  for (Site &site : sites)
  {
    site.elevation = 0.0;
    site.river = 0u;
    if (site.corners.empty()) continue;
    for (const uint32_t corner : site.corners)
    {
      site.elevation += corners[corner].elevation;
      site.river += corners[corner].river;
    }
    site.elevation /= site.corners.size();
    if (site.water || site.ocean || site.coast) site.elevation = 0.0;
    site.valley = site.river == 0u ? 0.0 :
        std::min(1.0, 0.25 + std::log2(static_cast<double>(site.river) + 1.0) * 0.18);
  }
  if (!reportProgress(progress, progressMutex, "Solving moisture", 0.74))
    return fail(error, "Terrain generation cancelled");

  std::deque<uint32_t> moistureQueue;
  for (uint32_t cornerIndex = 0u; cornerIndex < corners.size(); ++cornerIndex)
  {
    Corner &corner = corners[cornerIndex];
    if ((corner.water || corner.river > 0u) && !corner.ocean)
    {
      corner.moisture = corner.river > 0u ? std::min(3.0, 0.2 * static_cast<double>(corner.river)) : 1.0;
      moistureQueue.push_back(cornerIndex);
    }
    else
      corner.moisture = 0.0;
  }
  while (!moistureQueue.empty())
  {
    const uint32_t cornerIndex = moistureQueue.front();
    moistureQueue.pop_front();
    for (const uint32_t neighbour : corners[cornerIndex].neighbours)
    {
      const double newMoisture = corners[cornerIndex].moisture * 0.9;
      if (newMoisture > corners[neighbour].moisture)
      {
        corners[neighbour].moisture = newMoisture;
        moistureQueue.push_back(neighbour);
      }
    }
  }
  for (Corner &corner : corners)
    if (corner.ocean || corner.coast)
      corner.moisture = 1.0;
  std::vector<uint32_t> moistureCorners = landCorners;
  std::sort(
      moistureCorners.begin(), moistureCorners.end(),
      [&corners](const uint32_t a, const uint32_t b)
      {
        if (corners[a].moisture != corners[b].moisture) return corners[a].moisture < corners[b].moisture;
        return a < b;
      });
  for (uint32_t rank = 0u; rank < moistureCorners.size(); ++rank)
    corners[moistureCorners[rank]].moisture =
        moistureCorners.size() <= 1u ? 1.0 : static_cast<double>(rank) / (moistureCorners.size() - 1u);

  for (Site &site : sites)
  {
    site.moisture = 0.0;
    if (site.corners.empty()) continue;
    for (const uint32_t corner : site.corners) site.moisture += std::min(1.0, corners[corner].moisture);
    site.moisture /= site.corners.size();
  }
  if (!reportProgress(progress, progressMutex, "Simulating wind + climate", 0.78))
    return fail(error, "Terrain generation cancelled");
  simulateWindClimate(sites, terrainFeatures, settings);
  if (!reportProgress(progress, progressMutex, "Preparing raster maps", 0.80))
    return fail(error, "Terrain generation cancelled");

  maps = {};
  maps.width = rasterSettings.width;
  maps.height = rasterSettings.height;
  const size_t pixelCount = static_cast<size_t>(maps.width) * maps.height;
  maps.heightmap.assign(pixelCount, 0u);
  maps.sourceRegions.assign(pixelCount, 0u);
  maps.rivers.assign(pixelCount, 0u);
  maps.regions.assign(pixelCount, 0u);
  maps.materials.assign(pixelCount, static_cast<uint8_t>(PolygonTerrainMaterial::Ocean));
  maps.terrainForms.assign(pixelCount, 0u);
  maps.temperature.assign(pixelCount, 0u);
  maps.precipitation.assign(pixelCount, 0u);
  maps.wind.assign(pixelCount, 0u);
  maps.forests.assign(pixelCount, 0u);
  maps.settlements.assign(pixelCount, 0u);
  maps.continents.assign(pixelCount, 0u);
  maps.tectonics.assign(pixelCount, 0u);
  for (const auto &river : riverEdges)
  {
    const uint32_t first = static_cast<uint32_t>(river.first >> 32u);
    const uint32_t second = static_cast<uint32_t>(river.first & 0xffffffffu);
    drawRiverLine(corners[first].position, corners[second].position, river.second, settings, maps);
  }
  const GridIndex grid = buildGrid(sites, settings);
  constexpr uint32_t defaultRasterWorkers = 64u;
  const uint32_t requestedWorkers = rasterSettings.workerCount == 0u ?
      defaultRasterWorkers : rasterSettings.workerCount;
  const uint32_t workerCount = std::max(1u, std::min(requestedWorkers, maps.height));
  std::vector<std::thread> workers;
  workers.reserve(workerCount);
  std::atomic<uint32_t> completedRasterRows{0u};
  std::atomic<bool> cancelled{false};
  const auto rasterRows = [&](const uint32_t firstRow, const uint32_t endRow)
  {
    for (uint32_t y = firstRow; y < endRow; ++y)
    {
      if (cancelled.load(std::memory_order_relaxed)) return;
      for (uint32_t x = 0u; x < maps.width; ++x)
      {
        const Point2 point(
            (static_cast<double>(x) + 0.5) / maps.width * settings.widthMetres,
            (static_cast<double>(y) + 0.5) / maps.height * settings.heightMetres);
        const uint32_t site = nearestSite(grid, sites, point);
        const size_t pixel = static_cast<size_t>(y) * maps.width + x;
        double elevation = rasterElevation(site, point, sites, settings);
        if (maps.rivers[pixel] != 0u && !sites[site].ocean)
          elevation *= std::max(0.0, 0.78 - static_cast<double>(maps.rivers[pixel]) / 1024.0);
        maps.heightmap[pixel] = static_cast<uint16_t>(std::lround(std::clamp(elevation, 0.0, 1.0) * 65535.0));
        maps.sourceRegions[pixel] = site;
        maps.regions[pixel] = regionColour(site, sites[site].water);
        maps.terrainForms[pixel] = terrainFormForSite(sites[site]);
        maps.temperature[pixel] = quantizeUnit(sites[site].temperature);
        maps.precipitation[pixel] = quantizeUnit(sites[site].precipitation);
        maps.wind[pixel] = quantizeUnit(sites[site].windExposure);
        maps.forests[pixel] = quantizeUnit(forestSuitabilityForSite(sites[site]));
        maps.settlements[pixel] = quantizeUnit(settlementSuitabilityForSite(sites[site]));
        maps.continents[pixel] = quantizeUnit(sampleMacroContinent(point, terrainFeatures, settings) * 0.5 + 0.5);
        const TectonicSample tectonics = sampleTectonics(point, terrainFeatures, settings);
        maps.tectonics[pixel] = quantizeUnit(std::clamp(
            mountainInfluence(point, terrainFeatures, settings) * 0.62 +
                tectonics.convergence * 0.30 + tectonics.rift * 0.22,
            0.0, 1.0));
        if (maps.rivers[pixel] != 0u && !sites[site].ocean)
          maps.materials[pixel] = static_cast<uint8_t>(PolygonTerrainMaterial::River);
        else
          maps.materials[pixel] = static_cast<uint8_t>(materialForSite(sites[site]));
      }
      const uint32_t rows = completedRasterRows.fetch_add(1u, std::memory_order_relaxed) + 1u;
      if ((rows & 15u) == 0u || rows == maps.height)
      {
        const double fraction = 0.80 + 0.18 * (static_cast<double>(rows) / maps.height);
        if (!reportProgress(progress, progressMutex, "Rasterizing maps", fraction))
          cancelled.store(true, std::memory_order_relaxed);
      }
    }
  };
  for (uint32_t worker = 0u; worker < workerCount; ++worker)
  {
    const uint32_t firstRow = static_cast<uint32_t>(
        (static_cast<uint64_t>(maps.height) * worker) / workerCount);
    const uint32_t endRow = static_cast<uint32_t>(
        (static_cast<uint64_t>(maps.height) * (worker + 1u)) / workerCount);
    workers.emplace_back(rasterRows, firstRow, endRow);
  }
  for (std::thread &worker : workers) worker.join();
  if (cancelled.load(std::memory_order_relaxed))
    return fail(error, "Terrain generation cancelled");
  if (!reportProgress(progress, progressMutex, "Finalizing terrain", 0.99))
    return fail(error, "Terrain generation cancelled");

  if (statistics != nullptr)
  {
    *statistics = {};
    statistics->siteCount = static_cast<uint32_t>(sites.size());
    statistics->triangleCount = static_cast<uint32_t>(triangles.size());
    statistics->voronoiCornerCount = static_cast<uint32_t>(corners.size());
    statistics->riverEdgeCount = static_cast<uint32_t>(riverEdges.size());
    statistics->riverSourceCount = static_cast<uint32_t>(riverSources.size());
    for (const Site &site : sites)
      if (site.ocean)
        ++statistics->oceanSiteCount;
      else if (site.water)
        ++statistics->lakeSiteCount;
  }
  reportProgress(progress, progressMutex, "Complete", 1.0);
  return true;
}

} // namespace procedural
