#include "procedural/SphericalTerrain.hpp"

#include "procedural/PlanetGeology.hpp"
#include "procedural/PolygonTerrain.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace procedural
{
namespace
{

constexpr double kPi = 3.14159265358979323846264338327950288;

// Stages are timed by reading the clock between them rather than by a running
// total, so a deleted stage takes its probe with it instead of silently
// folding its cost into a neighbour.
using ProfileClock = std::chrono::steady_clock;

double elapsedMilliseconds(const ProfileClock::time_point start)
{
  return std::chrono::duration<double, std::milli>(ProfileClock::now() - start).count();
}

struct Vec3
{
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

struct Plate
{
  uint32_t seedCell = 0u;
  bool continental = false;
  double baseElevation = 0.0;
  double moisture = 0.0;
  Vec3 angularVelocity;
};

struct Cell
{
  uint32_t plate = 0u;
  double elevation = 0.0;
  double uplift = 0.0;
  double rift = 0.0;
  double mountain = 0.0;
  double precipitation = 0.0;
  double temperature = 0.0;
  double windExposure = 0.0;
  double windEast = 0.0;
  double windNorth = 0.0;
  bool lake = false;
  double lakeSurface = 0.0;
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
  const double t = std::clamp(value, 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

double gradientNoise3D(const double x, const double y, const double z, const uint64_t seed)
{
  const int64_t x0 = static_cast<int64_t>(std::floor(x));
  const int64_t y0 = static_cast<int64_t>(std::floor(y));
  const int64_t z0 = static_cast<int64_t>(std::floor(z));
  const double fx = x - static_cast<double>(x0);
  const double fy = y - static_cast<double>(y0);
  const double fz = z - static_cast<double>(z0);
  const auto fade = [](const double t)
  {
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
  };
  const double tx = fade(fx);
  const double ty = fade(fy);
  const double tz = fade(fz);
  const auto corner = [seed](const int64_t ix, const int64_t iy, const int64_t iz, const double dx, const double dy, const double dz)
  {
    const uint64_t hash = splitmix64(seed ^ (static_cast<uint64_t>(ix) * 0x9e3779b97f4a7c15ull) ^ (static_cast<uint64_t>(iy) * 0xbf58476d1ce4e5b9ull) ^ (static_cast<uint64_t>(iz) * 0x94d049bb133111ebull));
    constexpr std::array<std::array<double, 3>, 12> gradients{{
      {{1, 1, 0}},
      {{-1, 1, 0}},
      {{1, -1, 0}},
      {{-1, -1, 0}},
      {{1, 0, 1}},
      {{-1, 0, 1}},
      {{1, 0, -1}},
      {{-1, 0, -1}},
      {{0, 1, 1}},
      {{0, -1, 1}},
      {{0, 1, -1}},
      {{0, -1, -1}},
    }};
    const auto &gradient = gradients[hash % gradients.size()];
    return (gradient[0] * dx + gradient[1] * dy + gradient[2] * dz) * 0.7071067811865475;
  };
  const auto lerp = [](const double a, const double b, const double t)
  {
    return a + (b - a) * t;
  };
  const double n000 = corner(x0, y0, z0, fx, fy, fz);
  const double n100 = corner(x0 + 1, y0, z0, fx - 1.0, fy, fz);
  const double n010 = corner(x0, y0 + 1, z0, fx, fy - 1.0, fz);
  const double n110 = corner(x0 + 1, y0 + 1, z0, fx - 1.0, fy - 1.0, fz);
  const double n001 = corner(x0, y0, z0 + 1, fx, fy, fz - 1.0);
  const double n101 = corner(x0 + 1, y0, z0 + 1, fx - 1.0, fy, fz - 1.0);
  const double n011 = corner(x0, y0 + 1, z0 + 1, fx, fy - 1.0, fz - 1.0);
  const double n111 = corner(x0 + 1, y0 + 1, z0 + 1, fx - 1.0, fy - 1.0, fz - 1.0);
  return lerp(lerp(lerp(n000, n100, tx), lerp(n010, n110, tx), ty), lerp(lerp(n001, n101, tx), lerp(n011, n111, tx), ty), tz);
}

double sphereFbm(const Vec3 &point, const uint64_t seed, double frequency, const uint32_t octaves, const double lacunarity = 2.03, const double gain = 0.51)
{
  double amplitude = 1.0;
  double sum = 0.0;
  double total = 0.0;
  for (uint32_t octave = 0u; octave < octaves; ++octave)
  {
    sum += gradientNoise3D(point.x * frequency, point.y * frequency, point.z * frequency, seed + octave * 0x517cc1b727220a95ull) * amplitude;
    total += amplitude;
    amplitude *= gain;
    frequency *= lacunarity;
  }
  return total > 0.0 ? sum / total : 0.0;
}

double encodedLandElevation(const double elevation)
{
  if (elevation <= 0.0)
    return 0.0;
  // Preserve high-end detail without flattening every strong collision into
  // 65535. The exponential shoulder leaves room for exceptional summits.
  return std::clamp(0.025 + 0.955 * (1.0 - std::exp(-1.55 * elevation)), 0.0, 0.995);
}

double encodedOceanFloorDepth(const double elevation)
{
  if (elevation >= 0.0)
    return 0.0;
  // Keep the shelf continuous with the coast while retaining headroom for
  // exceptional trenches. The package exposes this unsigned magnitude using
  // a negative scale, so consumers receive an elevation below the sea datum.
  return std::clamp(0.955 * (1.0 - std::exp(1.85 * elevation)), 0.0, 0.995);
}

double decodedLandElevation(const double encoded)
{
  if (encoded <= 0.0)
    return 0.0;
  const double shoulder = std::clamp((encoded - 0.025) / 0.955, 0.0, 0.999999);
  return -std::log(1.0 - shoulder) / 1.55;
}

// Rock suite at a point, from data the planet already has.
//
// The geology follows the plate model rather than a standalone noise field:
// oceanic crust is basaltic, continental basement is granite and metamorphic
// raised along convergent belts, volcanic provinces track the most active
// boundaries, and everything quiet enough to accumulate sediment becomes a
// layered platform.
//
// This returns the *suite*, not the rock. Which bed of the suite is exposed
// depends on the surface height, which erosion is still changing.
uint8_t classifyRockSuite(
    const bool continental,
    const double tectonics,
    const Vec3 &position,
    const uint64_t seed)
{
  using Suite = PlanetRockSuite;
  // Province noise breaks the planet into basins and massifs at a scale far
  // larger than the bedding, so provinces read as geology rather than dither.
  const double province = sphereFbm(position, seed ^ 0x51a2f8d94c37b16dull, 1.35, 4u);

  if (!continental)
    return static_cast<uint8_t>(tectonics > 0.52 ? Suite::Volcanic : Suite::Oceanic);
  if (tectonics > 0.38)
    return static_cast<uint8_t>(Suite::Basement);
  if (tectonics > 0.24 && province > 0.16)
    return static_cast<uint8_t>(Suite::Volcanic);
  return static_cast<uint8_t>(Suite::Platform);
}

// Bedding offset in metres, wrapped into one stratum thickness. Warping the
// phase tilts and folds the sequence, so bedding planes are not one set of
// horizontal surfaces spanning the planet.
double beddingPhaseMetres(const Vec3 &position, const uint64_t seed)
{
  const double warp =
      sphereFbm(position, seed ^ 0x9d2c5680b3f7a41cull, 2.1, 4u) * 0.62 +
      sphereFbm(position, seed ^ 0x3f84d5b5b5470917ull, 7.3, 3u) * 0.24;
  const double phase = warp * kPlanetStratumThicknessMetres * 2.0;
  const double wrapped = std::fmod(phase, static_cast<double>(kPlanetStratumThicknessMetres));
  return wrapped < 0.0 ? wrapped + kPlanetStratumThicknessMetres : wrapped;
}

// Whittaker classification against the planet's own climate distribution.
//
// rainRank and temperatureRank are the cell's percentile within the land it
// belongs to, not raw values. The thresholds below are absolute, but the
// climate model's precipitation is a normalized index with no calibration
// against them: on the reference planet 55% of land fell under the two driest
// branches and five cover classes were never emitted at all. Ranking makes the
// boundaries land where the climate actually varies, so every band is populated
// on any planet, however wet or dry its absolute numbers happen to be.
//
// Negative ranks fall back to the raw values, which is what the graph pass uses
// before any distribution is known.
uint8_t classifyGraphMaterial(
    const Cell &cell,
    const uint8_t river,
    const bool coast,
    const Vec3 &position,
    const uint64_t seed,
    const double variation,
    const double rainRank = -1.0,
    const double temperatureRank = -1.0,
    const double slope = 0.0)
{
  using Material = PolygonTerrainMaterial;
  if (cell.elevation <= 0.0)
    return static_cast<uint8_t>(Material::Ocean);
  if (cell.lake)
    return static_cast<uint8_t>(Material::Lake);

  const double patchStrength = std::clamp(variation, 0.0, 3.0);
  const double moisturePatch = sphereFbm(position, seed ^ 0xa54ff53a5f1d36f1ull, 3.4, 4u) * 0.32 * patchStrength;
  const double temperaturePatch = sphereFbm(position, seed ^ 0x510e527fade682d1ull, 2.7, 3u) * 0.14 * patchStrength;
  const double rain = rainRank >= 0.0
      ? std::clamp(rainRank + moisturePatch * 0.5, 0.0, 1.0)
      : std::clamp(cell.precipitation + moisturePatch, 0.0, 1.0);
  const double temperature = temperatureRank >= 0.0
      ? std::clamp(temperatureRank + temperaturePatch * 0.5, 0.0, 1.0)
      : std::clamp(cell.temperature + temperaturePatch, 0.0, 1.0);

  if (river != 0u)
    return cell.elevation < 0.055 && rain > 0.66 ? static_cast<uint8_t>(Material::Marsh) : static_cast<uint8_t>(Material::River);
  if (coast && cell.elevation < 0.055)
    return static_cast<uint8_t>(Material::Beach);
  if (temperature < 0.10)
    return static_cast<uint8_t>(Material::Ice);
  // Steep ground shows rock whatever its altitude. Slope is the physical reason
  // a surface is bare — soil does not stay on it — and erosion now supplies a
  // real slope field, so this no longer waits for the rare cell that is both
  // high and mountainous.
  // slope is a true gradient in metres per metre. 0.55 is about 29 degrees,
  // roughly where soil stops holding on a hillside.
  if (slope > 0.55 || cell.mountain > 0.66 || cell.elevation > 0.58)
  {
    if (temperature < 0.20)
      return static_cast<uint8_t>(Material::Snow);
    if (rain < 0.12 && temperature > 0.72)
      return static_cast<uint8_t>(Material::Scorched);
    if (rain < 0.34)
      return static_cast<uint8_t>(Material::Bare);
    return static_cast<uint8_t>(Material::Rock);
  }
  if (temperature < 0.24)
    return rain > 0.42 ? static_cast<uint8_t>(Material::Taiga) : static_cast<uint8_t>(Material::Tundra);
  if (rain < 0.14)
    return temperature > 0.66 ? static_cast<uint8_t>(Material::SubtropicalDesert) : static_cast<uint8_t>(Material::TemperateDesert);
  if (rain < 0.28)
    return static_cast<uint8_t>(Material::Shrubland);
  if (temperature > 0.70)
  {
    if (rain > 0.72)
      return static_cast<uint8_t>(Material::TropicalRainForest);
    if (rain > 0.44)
      return static_cast<uint8_t>(Material::TropicalSeasonalForest);
    return static_cast<uint8_t>(Material::Grassland);
  }
  if (rain > 0.76)
    return static_cast<uint8_t>(Material::TemperateRainForest);
  if (rain > 0.50)
    return static_cast<uint8_t>(Material::TemperateDeciduousForest);
  if (temperature < 0.42 && rain > 0.42)
    return static_cast<uint8_t>(Material::Taiga);
  // Grass was unreachable: every path that meant "grass" returned Grassland, so
  // one of the twenty-one cover classes could never appear on any planet.
  // Lowland meadow is the distinction, against Grassland's drier steppe.
  if (cell.elevation < 0.28 && rain > 0.38)
    return static_cast<uint8_t>(Material::Grass);
  return static_cast<uint8_t>(Material::Grassland);
}

double visibleElevation(const Cell &cell)
{
  return cell.lake ? std::max(cell.elevation, cell.lakeSurface) : cell.elevation;
}

double periodicValueNoise(const double longitude01, const double latitude01, const uint32_t longitudePeriod, const uint32_t latitudePeriod, const uint64_t seed)
{
  const double x = longitude01 * static_cast<double>(longitudePeriod);
  const double y = latitude01 * static_cast<double>(latitudePeriod);
  const int64_t x0 = static_cast<int64_t>(std::floor(x));
  const int64_t y0 = static_cast<int64_t>(std::floor(y));
  const double tx = smoothstep(x - static_cast<double>(x0));
  const double ty = smoothstep(y - static_cast<double>(y0));
  const auto wrappedLongitude = [longitudePeriod](int64_t longitude)
  {
    const int64_t period = static_cast<int64_t>(longitudePeriod);
    longitude %= period;
    return longitude < 0 ? longitude + period : longitude;
  };
  const auto corner = [seed, &wrappedLongitude](const int64_t ix, const int64_t iy)
  {
    return hashUnit(seed ^ (static_cast<uint64_t>(wrappedLongitude(ix)) * 0x9e3779b97f4a7c15ull) ^ (static_cast<uint64_t>(iy) * 0xbf58476d1ce4e5b9ull));
  };
  const double a = corner(x0, y0);
  const double b = corner(x0 + 1, y0);
  const double c = corner(x0, y0 + 1);
  const double d = corner(x0 + 1, y0 + 1);
  return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

// Equirectangular previews need periodic longitude, but longitude must also lose
// influence as a sample approaches either pole. This keeps every longitude at a
// pole describing the same physical point when the atlas is projected to a mesh.
double planetNoise(const double longitude01, const double latitude01, const uint64_t seed, const uint32_t basePeriod)
{
  double amplitude = 0.5;
  double value = 0.0;
  double total = 0.0;
  const double poleWeight = std::pow(std::max(0.0, std::sin(std::clamp(latitude01, 0.0, 1.0) * kPi)), 0.65);
  for (uint32_t octave = 0u; octave < 5u; ++octave)
  {
    const uint32_t longitudePeriod = std::max(2u, basePeriod << octave);
    const uint32_t latitudePeriod = std::max(1u, longitudePeriod / 2u);
    const uint64_t octaveSeed = seed + octave * 0x517cc1b727220a95ull;
    const double aroundSphere = periodicValueNoise(longitude01, latitude01, longitudePeriod, latitudePeriod, octaveSeed);
    const double atPole = periodicValueNoise(0.0, latitude01, longitudePeriod, latitudePeriod, octaveSeed);
    value += (atPole + (aroundSphere - atPole) * poleWeight) * amplitude;
    total += amplitude;
    amplitude *= 0.5;
  }
  return total > 0.0 ? value / total : 0.0;
}

Vec3 add(const Vec3 &a, const Vec3 &b)
{
  return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 subtract(const Vec3 &a, const Vec3 &b)
{
  return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 multiply(const Vec3 &value, const double scale)
{
  return Vec3{value.x * scale, value.y * scale, value.z * scale};
}

double dot(const Vec3 &a, const Vec3 &b)
{
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(const Vec3 &a, const Vec3 &b)
{
  return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 normalize(const Vec3 &value)
{
  const double length = std::sqrt(dot(value, value));
  if (length <= 0.0)
    return Vec3{0.0, 0.0, 1.0};
  return multiply(value, 1.0 / length);
}

Vec3 spherePoint(const uint32_t x, const uint32_t y, const uint32_t width, const uint32_t height)
{
  const double longitude = ((static_cast<double>(x) + 0.5) / width) * 2.0 * kPi - kPi;
  const double latitude = kPi * 0.5 - ((static_cast<double>(y) + 0.5) / height) * kPi;
  const double clat = std::cos(latitude);
  return Vec3{clat * std::cos(longitude), std::sin(latitude), clat * std::sin(longitude)};
}

uint8_t quantizeUnit(const double value)
{
  return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

uint8_t quantizeSignedUnit(const double value)
{
  return quantizeUnit(value * 0.5 + 0.5);
}

uint32_t cellIndex(const uint32_t x, const uint32_t y, const uint32_t width)
{
  return y * width + x;
}

uint32_t wrappedX(const int32_t x, const uint32_t width)
{
  const int32_t wrapped = x < 0 ? x + static_cast<int32_t>(width) : x >= static_cast<int32_t>(width) ? x - static_cast<int32_t>(width) : x;
  return static_cast<uint32_t>(wrapped);
}

std::vector<uint32_t> neighbours(const uint32_t cell, const uint32_t width, const uint32_t height)
{
  const uint32_t x = cell % width;
  const uint32_t y = cell / width;
  std::vector<uint32_t> out;
  out.reserve(6u);
  out.push_back(cellIndex(wrappedX(static_cast<int32_t>(x) - 1, width), y, width));
  out.push_back(cellIndex(wrappedX(static_cast<int32_t>(x) + 1, width), y, width));
  if (y > 0u)
  {
    out.push_back(cellIndex(x, y - 1u, width));
    out.push_back(cellIndex(wrappedX(static_cast<int32_t>(x) + (y & 1u ? 1 : -1), width), y - 1u, width));
  }
  if (y + 1u < height)
  {
    out.push_back(cellIndex(x, y + 1u, width));
    out.push_back(cellIndex(wrappedX(static_cast<int32_t>(x) + (y & 1u ? 1 : -1), width), y + 1u, width));
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

uint32_t sphericalRasterCell(int32_t x, int32_t y, const uint32_t width, const uint32_t height)
{
  // Crossing an equirectangular pole reflects latitude and rotates longitude
  // by half a turn. Clamping Y would turn each pole into a horizontal wall.
  if (y < 0)
  {
    y = -y - 1;
    x += static_cast<int32_t>(width / 2u);
  }
  else if (y >= static_cast<int32_t>(height))
  {
    y = static_cast<int32_t>(height) * 2 - y - 1;
    x += static_cast<int32_t>(width / 2u);
  }
  y = std::clamp(y, 0, static_cast<int32_t>(height) - 1);
  x %= static_cast<int32_t>(width);
  if (x < 0)
    x += static_cast<int32_t>(width);
  return cellIndex(static_cast<uint32_t>(x), static_cast<uint32_t>(y), width);
}

std::array<uint32_t, 8> sphericalRasterNeighbours8(const uint32_t cell, const uint32_t width, const uint32_t height)
{
  const int32_t x = static_cast<int32_t>(cell % width);
  const int32_t y = static_cast<int32_t>(cell / width);
  return {
    sphericalRasterCell(x - 1, y - 1, width, height),
    sphericalRasterCell(x, y - 1, width, height),
    sphericalRasterCell(x + 1, y - 1, width, height),
    sphericalRasterCell(x - 1, y, width, height),
    sphericalRasterCell(x + 1, y, width, height),
    sphericalRasterCell(x - 1, y + 1, width, height),
    sphericalRasterCell(x, y + 1, width, height),
    sphericalRasterCell(x + 1, y + 1, width, height),
  };
}

std::vector<uint16_t> distanceFromOcean(const std::vector<Cell> &cells, const uint32_t width, const uint32_t height)
{
  const size_t cellCount = static_cast<size_t>(width) * height;
  constexpr uint16_t unknown = std::numeric_limits<uint16_t>::max();
  std::vector<uint16_t> distance(cellCount, unknown);
  std::vector<uint32_t> queue;
  queue.reserve(cellCount / 3u);
  for (uint32_t cell = 0u; cell < cellCount; ++cell)
  {
    if (cells[cell].elevation <= 0.0)
    {
      distance[cell] = 0u;
      continue;
    }
    for (const uint32_t neighbour : sphericalRasterNeighbours8(cell, width, height))
      if (cells[neighbour].elevation <= 0.0)
      {
        distance[cell] = 1u;
        queue.push_back(cell);
        break;
      }
  }
  for (size_t head = 0u; head < queue.size(); ++head)
  {
    const uint32_t cell = queue[head];
    const uint16_t nextDistance = distance[cell] == unknown - 1u ? unknown : static_cast<uint16_t>(distance[cell] + 1u);
    for (const uint32_t neighbour : sphericalRasterNeighbours8(cell, width, height))
      if (cells[neighbour].elevation > 0.0 && distance[neighbour] > nextDistance)
      {
        distance[neighbour] = nextDistance;
        queue.push_back(neighbour);
      }
  }
  return distance;
}

template <typename Function> void parallelRows(uint32_t height, Function &&function);
template <typename Function> void parallelFor(uint32_t count, uint32_t requestedWorkers, Function &&function);

void routeRainfallRunoff(
    const uint32_t width,
    const uint32_t height,
    const uint64_t seed,
    const std::vector<uint16_t> &heightmap,
    const std::vector<uint8_t> &precipitation,
    std::vector<uint8_t> &rivers,
    uint32_t &sourceCount)
{
  const size_t cellCount = static_cast<size_t>(width) * height;
  rivers.assign(cellCount, 0u);
  sourceCount = 0u;
  if (width == 0u || height == 0u || heightmap.size() != cellCount || precipitation.size() != cellCount)
    return;

  constexpr uint32_t noDownstream = std::numeric_limits<uint32_t>::max();
  using FloodNode = std::pair<double, uint32_t>;
  std::priority_queue<FloodNode, std::vector<FloodNode>, std::greater<FloodNode>> flood;
  std::vector<uint32_t> downstream(cellCount, noDownstream);
  std::vector<double> drainageSurface(cellCount, 0.0);
  std::vector<uint8_t> visited(cellCount, 0u);
  std::vector<uint32_t> heightOrder;
  heightOrder.reserve(cellCount);
  size_t landCount = 0u;
  for (uint32_t cell = 0u; cell < cellCount; ++cell)
  {
    drainageSurface[cell] = static_cast<double>(heightmap[cell]);
    if (heightmap[cell] == 0u)
    {
      visited[cell] = 1u;
      flood.emplace(0.0, cell);
    }
    else
      ++landCount;
  }
  if (landCount == 0u)
    return;
  if (flood.empty())
    return;

  // Barnes-style priority flood produces a monotonic drainage surface and an
  // outlet for every land cell. A tiny deterministic priority perturbation
  // avoids ruler-straight paths across broad, quantized flats without changing
  // the stored heightmap.
  while (!flood.empty())
  {
    const auto [surface, cell] = flood.top();
    flood.pop();
    if (surface > drainageSurface[cell] + 1.0e-9)
      continue;
    for (const uint32_t neighbour : sphericalRasterNeighbours8(cell, width, height))
    {
      if (visited[neighbour] != 0u)
        continue;
      visited[neighbour] = 1u;
      downstream[neighbour] = cell;
      const double epsilon = 1.0e-4 + hashUnit(0x6a09e667f3bcc909ull ^ static_cast<uint64_t>(neighbour)) * 2.0e-4;
      drainageSurface[neighbour] = std::max(static_cast<double>(heightmap[neighbour]), surface + epsilon);
      flood.emplace(drainageSurface[neighbour], neighbour);
      if (heightmap[neighbour] != 0u)
        heightOrder.push_back(neighbour);
    }
  }

  // The flood parent is an outlet guarantee, not the final river direction.
  // Rebuild steepest descent on the depression-free surface; following the
  // flood tree directly creates long parallel 45-degree paths on a raster.
  // Low-amplitude seamless 3D routing noise only breaks equal/near-equal
  // gradients and gives channels a natural meander without changing relief.
  std::vector<double> routingNoise(cellCount, 0.0);
  parallelRows(
      height,
      [&](const uint32_t y)
      {
        for (uint32_t x = 0u; x < width; ++x)
        {
          const uint32_t cell = cellIndex(x, y, width);
          routingNoise[cell] = sphereFbm(spherePoint(x, y, width, height), seed ^ 0x243f6a8885a308d3ull, 38.0, 3u) * 18.0;
        }
      });
  for (const uint32_t cell : heightOrder)
  {
    const uint32_t x = cell % width;
    const uint32_t y = cell / width;
    const double latitude = (0.5 - (static_cast<double>(y) + 0.5) / height) * kPi;
    const double longitudeMetric = std::max(0.20, std::abs(std::cos(latitude)));
    double bestSlope = -std::numeric_limits<double>::infinity();
    uint32_t best = noDownstream;
    for (const uint32_t neighbour : sphericalRasterNeighbours8(cell, width, height))
    {
      if (drainageSurface[neighbour] >= drainageSurface[cell] || neighbour == cell)
        continue;
      const int32_t nx = static_cast<int32_t>(neighbour % width);
      const int32_t ny = static_cast<int32_t>(neighbour / width);
      int32_t dx = std::abs(nx - static_cast<int32_t>(x));
      dx = std::min(dx, static_cast<int32_t>(width) - dx);
      const int32_t dy = std::abs(ny - static_cast<int32_t>(y));
      const double distance = std::sqrt(static_cast<double>(dx * dx) * longitudeMetric * longitudeMetric + static_cast<double>(dy * dy));
      const double slope = (drainageSurface[cell] + routingNoise[cell] - drainageSurface[neighbour] - routingNoise[neighbour]) / std::max(0.05, distance);
      if (slope > bestSlope)
      {
        bestSlope = slope;
        best = neighbour;
      }
    }
    if (best != noDownstream)
      downstream[cell] = best;
  }
  std::sort(
      heightOrder.begin(),
      heightOrder.end(),
      [&drainageSurface](const uint32_t a, const uint32_t b)
      {
        if (drainageSurface[a] != drainageSurface[b])
          return drainageSurface[a] < drainageSurface[b];
        return a < b;
      });

  std::vector<double> discharge(cellCount, 0.0);
  for (uint32_t y = 0u; y < height; ++y)
  {
    const double areaWeight = std::max(0.015, std::sin((static_cast<double>(y) + 0.5) / height * kPi));
    for (uint32_t x = 0u; x < width; ++x)
    {
      const uint32_t cell = cellIndex(x, y, width);
      if (heightmap[cell] != 0u)
      {
        const double rain = 0.08 + static_cast<double>(precipitation[cell]) / 255.0;
        discharge[cell] = rain * areaWeight;
      }
    }
  }
  // Reverse drainage-surface order accumulates rainfall from headwaters to the
  // sea without local sink loss.
  for (auto iterator = heightOrder.rbegin(); iterator != heightOrder.rend(); ++iterator)
  {
    const uint32_t cell = *iterator;
    if (downstream[cell] != noDownstream)
      discharge[downstream[cell]] += discharge[cell];
  }

  std::vector<double> routed;
  routed.reserve(landCount);
  for (const uint32_t cell : heightOrder)
    if (downstream[cell] != noDownstream && discharge[cell] > 0.0)
      routed.push_back(discharge[cell]);
  if (routed.empty())
    return;
  const auto percentile = [&routed](const double p)
  {
    const size_t index = std::min(routed.size() - 1u, static_cast<size_t>(p * static_cast<double>(routed.size() - 1u)));
    std::nth_element(routed.begin(), routed.begin() + static_cast<std::ptrdiff_t>(index), routed.end());
    return routed[index];
  };
  const double threshold = std::max(1.0, percentile(0.955));
  const double highFlow = std::max(threshold * 1.01, percentile(0.997));
  const double logRange = std::max(1.0e-6, std::log1p(highFlow / threshold));
  for (const uint32_t cell : heightOrder)
  {
    if (downstream[cell] == noDownstream || discharge[cell] < threshold)
      continue;
    const double strength = std::clamp(std::log1p(discharge[cell] / threshold) / logRange, 0.0, 1.0);
    rivers[cell] = static_cast<uint8_t>(std::lround(42.0 + strength * 213.0));
  }
  for (const uint32_t cell : heightOrder)
  {
    if (rivers[cell] == 0u)
      continue;
    bool hasRiverInput = false;
    for (const uint32_t neighbour : sphericalRasterNeighbours8(cell, width, height))
      if (downstream[neighbour] == cell && rivers[neighbour] != 0u)
      {
        hasRiverInput = true;
        break;
      }
    sourceCount += hasRiverInput ? 0u : 1u;
  }
}

uint32_t automaticPlateCount(const SphericalTerrainSettings &settings)
{
  (void)settings;
  // Plate topology is world data, not an image-resolution detail. Keeping this
  // stable makes a seed describe the same macro planet at every bake size.
  return 80u;
}

double sampleMacroU8(const std::vector<uint8_t> &field, const uint32_t width, const uint32_t height, const double longitude01, const double latitude01)
{
  if (field.size() != static_cast<size_t>(width) * height || width == 0u || height == 0u)
    return 0.0;
  const double x = longitude01 * width - 0.5;
  const double y = std::clamp(latitude01 * height - 0.5, 0.0, static_cast<double>(height - 1u));
  const int64_t x0Unwrapped = static_cast<int64_t>(std::floor(x));
  const uint32_t x0 = static_cast<uint32_t>((x0Unwrapped % static_cast<int64_t>(width) + width) % width);
  const uint32_t x1 = (x0 + 1u) % width;
  const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
  const uint32_t y1 = std::min(y0 + 1u, height - 1u);
  const double tx = x - std::floor(x);
  const double ty = y - y0;
  const double a = static_cast<double>(field[static_cast<size_t>(y0) * width + x0]) / 255.0;
  const double b = static_cast<double>(field[static_cast<size_t>(y0) * width + x1]) / 255.0;
  const double c = static_cast<double>(field[static_cast<size_t>(y1) * width + x0]) / 255.0;
  const double d = static_cast<double>(field[static_cast<size_t>(y1) * width + x1]) / 255.0;
  return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

double sampleMacroU16(const std::vector<uint16_t> &field, const uint32_t width, const uint32_t height, const double longitude01, const double latitude01)
{
  if (field.size() != static_cast<size_t>(width) * height || width == 0u || height == 0u)
    return 0.0;
  const double x = longitude01 * width - 0.5;
  const double y = std::clamp(latitude01 * height - 0.5, 0.0, static_cast<double>(height - 1u));
  const int64_t x0Unwrapped = static_cast<int64_t>(std::floor(x));
  const uint32_t x0 = static_cast<uint32_t>((x0Unwrapped % static_cast<int64_t>(width) + width) % width);
  const uint32_t x1 = (x0 + 1u) % width;
  const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
  const uint32_t y1 = std::min(y0 + 1u, height - 1u);
  const double tx = x - std::floor(x);
  const double ty = y - y0;
  const double a = static_cast<double>(field[static_cast<size_t>(y0) * width + x0]) / 65535.0;
  const double b = static_cast<double>(field[static_cast<size_t>(y0) * width + x1]) / 65535.0;
  const double c = static_cast<double>(field[static_cast<size_t>(y1) * width + x0]) / 65535.0;
  const double d = static_cast<double>(field[static_cast<size_t>(y1) * width + x1]) / 65535.0;
  return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

uint32_t sampleMacroPlate(const SphericalTerrainMaps &macro, const double longitude01, const double latitude01)
{
  if (macro.plates.size() != static_cast<size_t>(macro.width) * macro.height || macro.width == 0u || macro.height == 0u)
    return 0u;
  const uint32_t x = static_cast<uint32_t>(std::floor(longitude01 * macro.width)) % macro.width;
  const uint32_t y = std::min(static_cast<uint32_t>(std::floor(std::clamp(latitude01, 0.0, 1.0) * macro.height)), macro.height - 1u);
  return macro.plates[static_cast<size_t>(y) * macro.width + x];
}

uint32_t sampleMacroRegion(const SphericalTerrainMaps &macro, const double longitude01, const double latitude01)
{
  if (macro.sourceRegions.size() != static_cast<size_t>(macro.width) * macro.height || macro.width == 0u || macro.height == 0u)
    return 0u;
  const double wrappedLongitude = longitude01 - std::floor(longitude01);
  const uint32_t x = static_cast<uint32_t>(std::floor(wrappedLongitude * macro.width)) % macro.width;
  const uint32_t y = std::min(static_cast<uint32_t>(std::floor(std::clamp(latitude01, 0.0, 1.0) * macro.height)), macro.height - 1u);
  return macro.sourceRegions[static_cast<size_t>(y) * macro.width + x];
}

template <typename Function> void parallelRows(uint32_t height, Function &&function);

double graphEdgeDistance(const SpherePoint &a, const SpherePoint &b);

std::vector<double> diffuseGraphSignal(const SphericalVoronoiMesh &mesh, const std::vector<double> &source, const uint32_t iterations, const double decay, const uint32_t workerCount)
{
  if (source.size() != mesh.regions.size() || iterations == 0u)
    return source;
  std::vector<double> current = source;
  std::vector<double> next(source.size(), 0.0);
  for (uint32_t iteration = 0u; iteration < iterations; ++iteration)
  {
    parallelFor(
        static_cast<uint32_t>(source.size()),
        workerCount,
        [&](const uint32_t region)
        {
          double sum = current[region] * 2.0;
          double weight = 2.0;
          for (const uint32_t neighbour : mesh.regionNeighbours[region])
          {
            sum += current[neighbour];
            weight += 1.0;
          }
          const double spread = weight > 0.0 ? sum / weight * decay : 0.0;
          next[region] = std::max(source[region], spread);
        });
    current.swap(next);
  }
  return current;
}

std::vector<double> smoothGraphField(const SphericalVoronoiMesh &mesh, const std::vector<double> &source, const uint32_t iterations, const uint32_t workerCount)
{
  if (source.size() != mesh.regions.size() || iterations == 0u)
    return source;
  std::vector<double> current = source;
  std::vector<double> next(source.size(), 0.0);
  for (uint32_t iteration = 0u; iteration < iterations; ++iteration)
  {
    parallelFor(
        static_cast<uint32_t>(source.size()),
        workerCount,
        [&](const uint32_t region)
        {
          double sum = current[region] * 3.0;
          double weight = 3.0;
          for (const uint32_t neighbour : mesh.regionNeighbours[region])
          {
            sum += current[neighbour];
            weight += 1.0;
          }
          next[region] = sum / weight;
        });
    current.swap(next);
  }
  return current;
}

std::vector<double> graphDistanceToSignal(const SphericalVoronoiMesh &mesh, const std::vector<double> &source, const double threshold)
{
  const uint32_t count = static_cast<uint32_t>(mesh.regions.size());
  std::vector<double> distance(count, std::numeric_limits<double>::infinity());
  using Node = std::pair<double, uint32_t>;
  std::priority_queue<Node, std::vector<Node>, std::greater<Node>> queue;
  for (uint32_t region = 0u; region < count; ++region)
    if (source[region] > threshold)
    {
      distance[region] = 0.0;
      queue.emplace(0.0, region);
    }
  while (!queue.empty())
  {
    const auto [currentDistance, region] = queue.top();
    queue.pop();
    if (currentDistance > distance[region] + 1.0e-12)
      continue;
    for (const uint32_t neighbour : mesh.regionNeighbours[region])
    {
      const double candidate = currentDistance + graphEdgeDistance(mesh.regions[region], mesh.regions[neighbour]);
      if (candidate >= distance[neighbour])
        continue;
      distance[neighbour] = candidate;
      queue.emplace(candidate, neighbour);
    }
  }
  return distance;
}

template <typename Value> std::vector<Value> blurMacroField(const std::vector<Value> &source, const uint32_t width, const uint32_t height, const uint32_t radius, const uint32_t passes)
{
  if (source.size() != static_cast<size_t>(width) * height || radius == 0u || passes == 0u)
    return source;
  std::vector<Value> current = source;
  std::vector<Value> temporary(source.size());
  const uint64_t diameter = radius * 2u + 1u;
  for (uint32_t pass = 0u; pass < passes; ++pass)
  {
    parallelRows(
        height,
        [&](const uint32_t y)
        {
          for (uint32_t x = 0u; x < width; ++x)
          {
            uint64_t sum = 0u;
            for (int32_t offset = -static_cast<int32_t>(radius); offset <= static_cast<int32_t>(radius); ++offset)
            {
              const uint32_t sx = wrappedX(static_cast<int32_t>(x) + offset, width);
              sum += current[static_cast<size_t>(y) * width + sx];
            }
            temporary[static_cast<size_t>(y) * width + x] = static_cast<Value>((sum + diameter / 2u) / diameter);
          }
        });
    parallelRows(
        height,
        [&](const uint32_t y)
        {
          for (uint32_t x = 0u; x < width; ++x)
          {
            uint64_t sum = 0u;
            for (int32_t offset = -static_cast<int32_t>(radius); offset <= static_cast<int32_t>(radius); ++offset)
              sum += temporary[sphericalRasterCell(static_cast<int32_t>(x), static_cast<int32_t>(y) + offset, width, height)];
            current[static_cast<size_t>(y) * width + x] = static_cast<Value>((sum + diameter / 2u) / diameter);
          }
        });
  }
  return current;
}

template <typename Function> void parallelRows(const uint32_t height, Function &&function)
{
  parallelFor(height, 0u, std::forward<Function>(function));
}

template <typename Function> void parallelFor(const uint32_t count, const uint32_t requestedWorkers, Function &&function)
{
  if (count == 0u)
    return;
  const uint32_t available = std::max(1u, std::thread::hardware_concurrency());
  const uint32_t automaticWorkers = std::min(64u, available * 2u);
  const uint32_t workerCount = std::min(count, requestedWorkers == 0u ? automaticWorkers : std::clamp(requestedWorkers, 1u, 64u));
  if (workerCount <= 1u || count < 128u)
  {
    for (uint32_t index = 0u; index < count; ++index)
      function(index);
    return;
  }
  std::atomic<uint32_t> nextIndex{0u};
  std::vector<std::thread> workers;
  workers.reserve(workerCount);
  for (uint32_t worker = 0u; worker < workerCount; ++worker)
    workers.emplace_back(
        [&]
        {
          for (;;)
          {
            const uint32_t index = nextIndex.fetch_add(1u, std::memory_order_relaxed);
            if (index >= count)
              break;
            function(index);
          }
        });
  for (std::thread &worker : workers)
    worker.join();
}

void simulateWindAndRainShadows(std::vector<Cell> &cells, const uint32_t width, const uint32_t height)
{
  if (cells.size() != static_cast<size_t>(width) * height || width == 0u || height == 0u)
    return;
  const double cellDistanceScale = 512.0 / std::max(1.0, static_cast<double>(width));
  const double gradientScale = std::max(1.0, static_cast<double>(width) / 512.0);
  parallelRows(
      height,
      [&](const uint32_t y)
      {
        const double latitude01 = (static_cast<double>(y) + 0.5) / height;
        const double signedLatitude = 1.0 - latitude01 * 2.0;
        const double absoluteLatitude = std::abs(signedLatitude);
        const double hemisphere = signedLatitude >= 0.0 ? 1.0 : -1.0;
        const double tropicalToMidlatitude = smoothstep((absoluteLatitude - 0.24) / 0.15);
        const double midlatitudeToPolar = smoothstep((absoluteLatitude - 0.61) / 0.18);
        double eastWind = -1.0 + tropicalToMidlatitude * 2.0;
        eastWind += (-0.72 - eastWind) * midlatitudeToPolar;
        double northWind = -hemisphere * 0.18 + tropicalToMidlatitude * hemisphere * 0.30;
        northWind += (-hemisphere * 0.08 - northWind) * midlatitudeToPolar;
        const double windSpeed = 0.25 + std::abs(eastWind) * 0.75;
        const int32_t direction = eastWind >= 0.0 ? 1 : -1;
        uint32_t startX = direction > 0 ? 0u : width - 1u;
        for (uint32_t probe = 0u; probe < width; ++probe)
        {
          const uint32_t x = direction > 0 ? probe : width - 1u - probe;
          if (cells[static_cast<size_t>(y) * width + x].elevation <= 0.0)
          {
            startX = x;
            break;
          }
        }

        const double wetLatitude = 0.22 + 0.44 * std::exp(-std::pow(absoluteLatitude / 0.30, 2.0)) + 0.14 * std::exp(-std::pow((absoluteLatitude - 0.58) / 0.18, 2.0));
        double atmosphericMoisture = std::clamp(0.50 + wetLatitude * 0.42, 0.0, 1.0);
        double previousElevation = cells[static_cast<size_t>(y) * width + startX].elevation;
        double previousUplift = cells[static_cast<size_t>(y) * width + startX].uplift;
        for (uint32_t step = 0u; step < width * 2u; ++step)
        {
          const int64_t unwrapped = static_cast<int64_t>(startX) + static_cast<int64_t>(direction) * step;
          const uint32_t x = static_cast<uint32_t>((unwrapped % static_cast<int64_t>(width) + width) % width);
          Cell &cell = cells[static_cast<size_t>(y) * width + x];
          cell.windEast = eastWind;
          cell.windNorth = northWind;
          if (cell.elevation <= 0.0)
          {
            atmosphericMoisture = std::max(atmosphericMoisture, std::clamp(0.78 + wetLatitude * 0.20, 0.0, 1.0));
            if (step >= width)
            {
              cell.precipitation = std::clamp(0.68 + wetLatitude * 0.24, 0.0, 1.0);
              cell.windExposure = 0.08;
            }
          }
          else
          {
            const double elevationRise = std::max(0.0, cell.elevation - previousElevation) * gradientScale;
            const double upliftRise = std::max(0.0, cell.uplift - previousUplift);
            const double lift = (elevationRise * 2.8 + upliftRise * 0.34 + cell.uplift * 0.012) * windSpeed;
            const double backgroundCondensation = (0.0045 + wetLatitude * 0.0065) * cellDistanceScale;
            const double rainFraction = std::clamp(backgroundCondensation + lift, 0.002, 0.72);
            const double rainfall = atmosphericMoisture * rainFraction;
            const double descendingShadow = std::max(0.0, previousElevation - cell.elevation) * gradientScale * 0.42 + std::max(0.0, previousUplift - cell.uplift) * 0.10;
            if (step >= width)
            {
              const double transportedRain = rainfall * 5.2 + wetLatitude * 0.22 + cell.precipitation * 0.18 - descendingShadow * 0.55;
              cell.precipitation = std::clamp(transportedRain, 0.0, 1.0);
              cell.windExposure = std::clamp(lift * 1.8 + std::abs(eastWind) * 0.10 + std::abs(northWind) * 0.08, 0.0, 1.0);
            }
            atmosphericMoisture = std::clamp(atmosphericMoisture * std::pow(0.998, cellDistanceScale) - rainfall * 0.90, 0.0, 1.0);
          }
          previousElevation = cell.elevation;
          previousUplift = cell.uplift;
        }
      });
}

Vec3 toVec3(const SpherePoint &point)
{
  return Vec3{point.x, point.y, point.z};
}

SpherePoint toSpherePoint(const Vec3 &point)
{
  return SpherePoint{point.x, point.y, point.z};
}

uint64_t graphEdgeKey(const uint32_t a, const uint32_t b)
{
  const uint32_t lo = std::min(a, b);
  const uint32_t hi = std::max(a, b);
  return (static_cast<uint64_t>(lo) << 32u) | hi;
}

double graphEdgeDistance(const SpherePoint &a, const SpherePoint &b)
{
  return std::max(1.0e-6, std::acos(std::clamp(a.x * b.x + a.y * b.y + a.z * b.z, -1.0, 1.0)));
}

struct GraphDrainage
{
  std::vector<double> surface;
  std::vector<double> flow;
  std::vector<int32_t> downflow;
  std::vector<uint32_t> descending;
};

GraphDrainage resolveGraphDrainage(const SphericalVoronoiMesh &mesh, const std::vector<Cell> &regions)
{
  const uint32_t count = static_cast<uint32_t>(regions.size());
  GraphDrainage drainage{};
  drainage.surface.resize(count);
  drainage.flow.assign(count, 0.0);
  drainage.downflow.assign(count, -1);
  std::vector<uint8_t> visited(count, 0u);
  using Node = std::pair<double, uint32_t>;
  std::priority_queue<Node, std::vector<Node>, std::greater<Node>> queue;

  for (uint32_t region = 0u; region < count; ++region)
  {
    drainage.surface[region] = regions[region].elevation;
    if (regions[region].elevation > 0.0)
      continue;
    visited[region] = 1u;
    double lowest = regions[region].elevation;
    for (const uint32_t neighbour : mesh.regionNeighbours[region])
      if (regions[neighbour].elevation < lowest)
      {
        lowest = regions[neighbour].elevation;
        drainage.downflow[region] = static_cast<int32_t>(neighbour);
      }
    queue.emplace(drainage.surface[region], region);
  }

  // Barnes-style priority flood on the sphere. The epsilon imposes a strict
  // drainage gradient through flats without changing visible elevation.
  while (!queue.empty())
  {
    const auto [surface, current] = queue.top();
    queue.pop();
    if (surface > drainage.surface[current] + 1.0e-12)
      continue;
    for (const uint32_t neighbour : mesh.regionNeighbours[current])
    {
      if (visited[neighbour] != 0u)
        continue;
      visited[neighbour] = 1u;
      drainage.downflow[neighbour] = static_cast<int32_t>(current);
      drainage.surface[neighbour] = std::max(regions[neighbour].elevation, surface + 1.0e-9);
      queue.emplace(drainage.surface[neighbour], neighbour);
    }
  }

  drainage.descending.resize(count);
  std::iota(drainage.descending.begin(), drainage.descending.end(), 0u);
  std::stable_sort(
      drainage.descending.begin(),
      drainage.descending.end(),
      [&drainage](const uint32_t a, const uint32_t b)
      {
        return drainage.surface[a] > drainage.surface[b];
      });
  for (uint32_t region = 0u; region < count; ++region)
    if (regions[region].elevation > 0.0)
    {
      const double rain = std::clamp(regions[region].precipitation, 0.02, 1.0);
      drainage.flow[region] = 0.10 + rain * rain;
    }
  for (const uint32_t region : drainage.descending)
    if (drainage.downflow[region] >= 0)
      drainage.flow[static_cast<uint32_t>(drainage.downflow[region])] += drainage.flow[region];
  return drainage;
}

uint32_t formGraphLakes(const SphericalVoronoiMesh &mesh, const std::vector<double> &distanceToOcean, const double typicalEdge, const double abundance, std::vector<Cell> &regions)
{
  const double amount = std::clamp(abundance, 0.0, 3.0);
  if (amount <= 0.0 || regions.empty())
    return 0u;

  const GraphDrainage drainage = resolveGraphDrainage(mesh, regions);
  struct LakeCandidate
  {
    double score = 0.0;
    uint32_t region = 0u;
  };
  std::vector<LakeCandidate> candidates;
  candidates.reserve(regions.size() / 80u);
  const double minimumDepth = typicalEdge * (0.085 / std::sqrt(std::max(0.25, amount)));
  const double minimumCoastDistance = std::max(typicalEdge * 3.0, 0.030);
  for (uint32_t region = 0u; region < regions.size(); ++region)
  {
    if (regions[region].elevation <= 0.0 || distanceToOcean[region] < minimumCoastDistance)
      continue;
    const double depth = drainage.surface[region] - regions[region].elevation;
    if (depth < minimumDepth || drainage.flow[region] < 0.65 || regions[region].precipitation < 0.16)
      continue;
    const double score = (depth / typicalEdge) * std::log1p(drainage.flow[region]) * (0.45 + regions[region].precipitation);
    bool localMaximum = true;
    for (const uint32_t neighbour : mesh.regionNeighbours[region])
    {
      const double neighbourDepth = drainage.surface[neighbour] - regions[neighbour].elevation;
      if (neighbourDepth > depth && drainage.flow[neighbour] >= drainage.flow[region] * 0.55)
      {
        localMaximum = false;
        break;
      }
    }
    if (localMaximum)
      candidates.push_back(LakeCandidate{score, region});
  }
  std::sort(
      candidates.begin(),
      candidates.end(),
      [](const LakeCandidate &a, const LakeCandidate &b)
      {
        return a.score > b.score;
      });

  const uint32_t targetLakes = std::clamp(static_cast<uint32_t>(std::lround(std::sqrt(static_cast<double>(regions.size())) * 0.080 * amount)), 1u, 128u);
  const double maximumRadius = 0.018 + 0.010 * std::sqrt(amount);
  const double minimumDot = std::cos(maximumRadius);
  const uint32_t maximumLakeNodes = std::clamp(static_cast<uint32_t>(std::lround(3.0 + amount * 9.0)), 3u, 36u);
  const uint32_t minimumLakeNodes = regions.size() >= 50000u ? 2u : 1u;
  std::vector<uint8_t> claimed(regions.size(), 0u);
  std::vector<uint32_t> visitStamp(regions.size(), 0u);
  uint32_t currentStamp = 0u;
  uint32_t lakeCount = 0u;
  uint32_t lakeNodes = 0u;
  for (const LakeCandidate &candidate : candidates)
  {
    if (lakeCount >= targetLakes || claimed[candidate.region] != 0u)
      continue;
    const uint32_t seed = candidate.region;
    const double waterLevel = drainage.surface[seed] - minimumDepth * 0.08;
    const double surfaceTolerance = std::max(minimumDepth * 2.5, typicalEdge * 0.12);
    std::vector<uint32_t> basin;
    std::vector<uint32_t> queue{seed};
    ++currentStamp;
    visitStamp[seed] = currentStamp;
    for (size_t head = 0u; head < queue.size() && basin.size() < maximumLakeNodes; ++head)
    {
      const uint32_t region = queue[head];
      const SpherePoint &point = mesh.regions[region];
      const SpherePoint &seedPoint = mesh.regions[seed];
      const double positionDot = point.x * seedPoint.x + point.y * seedPoint.y + point.z * seedPoint.z;
      if (regions[region].elevation <= 0.0 || distanceToOcean[region] < minimumCoastDistance || positionDot < minimumDot || regions[region].elevation > waterLevel - minimumDepth * 0.10 ||
          std::abs(drainage.surface[region] - drainage.surface[seed]) > surfaceTolerance)
        continue;
      basin.push_back(region);
      for (const uint32_t neighbour : mesh.regionNeighbours[region])
        if (visitStamp[neighbour] != currentStamp && claimed[neighbour] == 0u)
        {
          visitStamp[neighbour] = currentStamp;
          queue.push_back(neighbour);
        }
    }
    if (basin.size() < minimumLakeNodes)
      continue;
    for (const uint32_t region : basin)
    {
      claimed[region] = 1u;
      regions[region].lake = true;
      regions[region].lakeSurface = waterLevel;
      regions[region].mountain = 0.0;
      regions[region].precipitation = std::max(regions[region].precipitation, 0.85);
    }
    lakeNodes += static_cast<uint32_t>(basin.size());
    ++lakeCount;
  }
  return lakeNodes;
}

void simulateGraphClimate(const SphericalVoronoiMesh &mesh, const std::vector<Plate> &plates, std::vector<Cell> &regions, const uint64_t seed, const uint32_t workerCount, const double wetness)
{
  const uint32_t count = static_cast<uint32_t>(regions.size());
  std::vector<Vec3> winds(count);
  std::vector<double> humidity(count, 0.0);
  std::vector<double> nextHumidity(count, 0.0);
  std::vector<double> rain(count, 0.0);

  parallelFor(
      count,
      workerCount,
      [&](const uint32_t region)
      {
        const Vec3 position = toVec3(mesh.regions[region]);
        Vec3 east = cross(Vec3{0.0, 1.0, 0.0}, position);
        if (dot(east, east) < 1.0e-12)
          east = Vec3{1.0, 0.0, 0.0};
        else
          east = normalize(east);
        const Vec3 north = normalize(cross(position, east));
        const double latitude = std::asin(std::clamp(position.y, -1.0, 1.0));
        const double absoluteLatitude = std::abs(latitude) / (kPi * 0.5);
        const double zonal = absoluteLatitude < 0.34 ? -1.0 : absoluteLatitude < 0.72 ? 1.0 : -0.65;
        const double equatorward = latitude >= 0.0 ? -1.0 : 1.0;
        winds[region] = normalize(add(multiply(east, zonal), multiply(north, equatorward * 0.22)));
        regions[region].windEast = zonal;
        regions[region].windNorth = equatorward * 0.22;
        humidity[region] = regions[region].elevation <= 0.0 ? 1.0 : 0.16 + plates[regions[region].plate].moisture * 0.16;

        const double latitude01 = std::abs(latitude) / (kPi * 0.5);
        const double height = encodedLandElevation(regions[region].elevation);
        regions[region].temperature = std::clamp(1.0 - std::pow(latitude01, 1.45) - height * 0.48 + sphereFbm(position, seed ^ 0x1f83d9abfb41bd6bull, 2.2, 3u) * 0.055, 0.0, 1.0);
      });

  // Iterative graph advection handles cyclic prevailing winds without a
  // longitude seam. Only genuinely upwind neighbours contribute moisture.
  for (uint32_t iteration = 0u; iteration < 28u; ++iteration)
  {
    std::fill(rain.begin(), rain.end(), 0.0);
    parallelFor(
        count,
        workerCount,
        [&](const uint32_t region)
        {
          if (regions[region].elevation <= 0.0)
          {
            nextHumidity[region] = 1.0;
            return;
          }
          const Vec3 position = toVec3(mesh.regions[region]);
          double incoming = 0.0;
          double weight = 0.0;
          double lift = 0.0;
          for (const uint32_t neighbour : mesh.regionNeighbours[region])
          {
            const Vec3 neighbourPosition = toVec3(mesh.regions[neighbour]);
            const Vec3 direction = normalize(subtract(position, multiply(neighbourPosition, dot(position, neighbourPosition))));
            const double alignment = std::max(0.0, dot(winds[neighbour], direction));
            if (alignment <= 0.02)
              continue;
            const double edge = graphEdgeDistance(mesh.regions[region], mesh.regions[neighbour]);
            const double rise = std::max(0.0, regions[region].elevation - regions[neighbour].elevation) / edge;
            incoming += humidity[neighbour] * alignment;
            weight += alignment;
            lift += rise * alignment;
          }
          const double latitude = std::abs(std::asin(std::clamp(position.y, -1.0, 1.0))) / (kPi * 0.5);
          const double latitudeRain = 0.055 + 0.135 * std::exp(-std::pow(latitude / 0.28, 2.0)) + 0.075 * std::exp(-std::pow((latitude - 0.62) / 0.18, 2.0));
          const double transported = weight > 0.0 ? incoming / weight : humidity[region] * 0.82;
          const double orographic = std::clamp(lift / std::max(0.5, weight) * 0.075, 0.0, 0.42);
          rain[region] = std::clamp(latitudeRain + transported * 0.16 + orographic, 0.0, 0.90);
          nextHumidity[region] = std::clamp(transported * 0.94 + latitudeRain * 0.30 - rain[region] * 0.72, 0.015, 1.0);
        });
    humidity.swap(nextHumidity);
    parallelFor(
        count,
        workerCount,
        [&](const uint32_t region)
        {
          regions[region].precipitation = regions[region].elevation <= 0.0 ? 1.0 : regions[region].precipitation * 0.72 + rain[region] * 0.28;
        });
  }
  parallelFor(
      count,
      workerCount,
      [&](const uint32_t region)
      {
        regions[region].precipitation = std::clamp(regions[region].precipitation * 1.80 * std::clamp(wetness, 0.25, 2.0), 0.0, 1.0);
        regions[region].windExposure = std::clamp(rain[region] * 1.8, 0.0, 1.0);
      });
}

void erodeSphericalGraph(const SphericalVoronoiMesh &mesh, const SphericalTerrainSettings &settings, std::vector<Cell> &regions)
{
  const uint32_t count = static_cast<uint32_t>(regions.size());
  const std::vector<uint8_t> ocean = [&regions]
  {
    std::vector<uint8_t> mask(regions.size(), 0u);
    for (uint32_t region = 0u; region < regions.size(); ++region)
      mask[region] = regions[region].elevation <= 0.0 ? 1u : 0u;
    return mask;
  }();
  const double erosionK = std::clamp(settings.hydraulicErosion, 0.0, 0.01);

  for (uint32_t iteration = 0u; iteration < settings.hydraulicIterations; ++iteration)
  {
    const GraphDrainage drainage = resolveGraphDrainage(mesh, regions);
    std::vector<double> next(count, 0.0);
    for (uint32_t region = 0u; region < count; ++region)
      next[region] = regions[region].elevation;
    for (auto order = drainage.descending.rbegin(); order != drainage.descending.rend(); ++order)
    {
      const uint32_t region = *order;
      if (ocean[region] != 0u || drainage.downflow[region] < 0)
        continue;
      const uint32_t receiver = static_cast<uint32_t>(drainage.downflow[region]);
      const double receiverHeight = ocean[receiver] != 0u ? 0.0 : std::max(0.0, next[receiver]);
      const double distance = graphEdgeDistance(mesh.regions[region], mesh.regions[receiver]);
      const double factor = erosionK * std::sqrt(std::max(0.0, drainage.flow[region])) / distance;
      const double eroded = (regions[region].elevation + factor * receiverHeight) / (1.0 + factor);
      next[region] = std::max(1.0e-5, eroded);
    }
    for (uint32_t region = 0u; region < count; ++region)
      if (ocean[region] == 0u)
        regions[region].elevation = next[region];
  }

  const double thermalStrength = std::clamp(settings.thermalErosion, 0.0, 0.5);
  const double talus = std::clamp(settings.talusSlope, 0.05, 4.0);
  for (uint32_t iteration = 0u; iteration < settings.thermalIterations; ++iteration)
  {
    std::vector<double> delta(count, 0.0);
    for (uint32_t region = 0u; region < count; ++region)
    {
      if (ocean[region] != 0u)
        continue;
      for (const uint32_t neighbour : mesh.regionNeighbours[region])
      {
        if (ocean[neighbour] != 0u || region >= neighbour || regions[region].elevation == regions[neighbour].elevation)
          continue;
        const uint32_t high = regions[region].elevation > regions[neighbour].elevation ? region : neighbour;
        const uint32_t low = high == region ? neighbour : region;
        const double distance = graphEdgeDistance(mesh.regions[high], mesh.regions[low]);
        const double excess = regions[high].elevation - regions[low].elevation - talus * distance;
        if (excess <= 0.0)
          continue;
        const double transfer = std::min(excess * thermalStrength * 0.25, regions[high].elevation * 0.04);
        delta[high] -= transfer;
        delta[low] += transfer;
      }
    }
    for (uint32_t region = 0u; region < count; ++region)
      if (ocean[region] == 0u)
        regions[region].elevation = std::max(1.0e-5, regions[region].elevation + delta[region]);
  }

  // Orogen's default ridge pass exaggerates local maxima and weakly deepens
  // valleys before soil creep rounds the resulting hillslopes.
  const std::vector<double> preSharpenElevation = [&regions]
  {
    std::vector<double> elevation(regions.size());
    for (uint32_t region = 0u; region < regions.size(); ++region)
      elevation[region] = regions[region].elevation;
    return elevation;
  }();
  const double ridgeStrength = std::clamp(settings.ridgeSharpening, 0.0, 0.25);
  for (uint32_t iteration = 0u; iteration < settings.ridgeSharpeningIterations; ++iteration)
  {
    std::vector<double> next(count, 0.0);
    for (uint32_t region = 0u; region < count; ++region)
    {
      next[region] = regions[region].elevation;
      if (ocean[region] != 0u || mesh.regionNeighbours[region].empty())
        continue;
      double average = 0.0;
      for (const uint32_t neighbour : mesh.regionNeighbours[region])
        average += regions[neighbour].elevation;
      average /= mesh.regionNeighbours[region].size();
      const double height = regions[region].elevation;
      if (height > average)
        next[region] = std::min(height + (height - average) * ridgeStrength, preSharpenElevation[region] * 2.0);
      else if (height < average)
        next[region] = std::max({1.0e-3, preSharpenElevation[region] * 0.5, height - (average - height) * ridgeStrength * 0.5});
    }
    for (uint32_t region = 0u; region < count; ++region)
      if (ocean[region] == 0u)
        regions[region].elevation = next[region];
  }

  const double creepStrength = std::clamp(settings.soilCreep, 0.0, 0.5);
  std::vector<uint32_t> interiorLand;
  interiorLand.reserve(count);
  for (uint32_t region = 0u; region < count; ++region)
  {
    if (ocean[region] != 0u)
      continue;
    const bool coast = std::any_of(
        mesh.regionNeighbours[region].begin(),
        mesh.regionNeighbours[region].end(),
        [&ocean](const uint32_t neighbour)
        {
          return ocean[neighbour] != 0u;
        });
    if (!coast)
      interiorLand.push_back(region);
  }
  for (uint32_t iteration = 0u; iteration < settings.soilCreepIterations; ++iteration)
  {
    std::vector<double> next(count, 0.0);
    for (const uint32_t region : interiorLand)
    {
      double average = 0.0;
      uint32_t neighbours = 0u;
      for (const uint32_t neighbour : mesh.regionNeighbours[region])
        if (ocean[neighbour] == 0u)
        {
          average += regions[neighbour].elevation;
          ++neighbours;
        }
      next[region] = neighbours == 0u ? regions[region].elevation : regions[region].elevation + (average / neighbours - regions[region].elevation) * creepStrength;
    }
    for (const uint32_t region : interiorLand)
      regions[region].elevation = std::max(1.0e-5, next[region]);
  }
}

uint32_t automaticRegionCount(const SphericalTerrainSettings &settings)
{
  // Plate topology is generated once on a fixed reference graph so a seed does
  // not change when the output texture resolution changes. Twenty thousand
  // regions are still cheap at load time but give stress propagation enough
  // room to form broad, irregular orogenic belts.
  return std::clamp(settings.graphRegionCount, 4096u, 256000u);
}

uint32_t nearestRegion(const Vec3 &position, const SphericalVoronoiMesh &mesh, const std::vector<std::vector<uint32_t>> &bins, const uint32_t binWidth, const uint32_t binHeight)
{
  const double longitude = std::atan2(position.z, position.x);
  const double latitude = std::asin(std::clamp(position.y, -1.0, 1.0));
  const uint32_t bx = static_cast<uint32_t>(std::clamp(static_cast<int32_t>(std::floor(((longitude + kPi) / (2.0 * kPi)) * binWidth)), 0, static_cast<int32_t>(binWidth) - 1));
  const uint32_t by = static_cast<uint32_t>(std::clamp(static_cast<int32_t>(std::floor((0.5 - latitude / kPi) * binHeight)), 0, static_cast<int32_t>(binHeight) - 1));
  uint32_t best = 0u;
  double bestDot = -std::numeric_limits<double>::infinity();
  bool found = false;
  for (int32_t radius = 0; radius <= 3 && !found; ++radius)
  {
    for (int32_t oy = -radius; oy <= radius; ++oy)
    {
      const int32_t yy = static_cast<int32_t>(by) + oy;
      if (yy < 0 || yy >= static_cast<int32_t>(binHeight))
        continue;
      for (int32_t ox = -radius; ox <= radius; ++ox)
      {
        const int32_t xx = (static_cast<int32_t>(bx) + ox + static_cast<int32_t>(binWidth)) % static_cast<int32_t>(binWidth);
        const std::vector<uint32_t> &candidates = bins[static_cast<uint32_t>(yy) * binWidth + static_cast<uint32_t>(xx)];
        for (const uint32_t region : candidates)
        {
          const SpherePoint &point = mesh.regions[region];
          const double score = position.x * point.x + position.y * point.y + position.z * point.z;
          if (!found || score > bestDot)
          {
            found = true;
            bestDot = score;
            best = region;
          }
        }
      }
    }
  }
  if (found)
    return best;
  for (uint32_t region = 0u; region < mesh.regions.size(); ++region)
  {
    const SpherePoint &point = mesh.regions[region];
    const double score = position.x * point.x + position.y * point.y + position.z * point.z;
    if (score > bestDot)
    {
      bestDot = score;
      best = region;
    }
  }
  return best;
}

bool generateVoronoiTerrain(const SphericalTerrainSettings &settings, SphericalTerrainMaps &maps, SphericalTerrainStatistics *statistics, SphericalTerrainProfile *profile = nullptr)
{
  const ProfileClock::time_point graphStart = ProfileClock::now();
  ProfileClock::time_point stageStart = graphStart;
  const uint32_t width = settings.width;
  const uint32_t height = settings.height;
  const uint32_t cellCount = width * height;
  const double jitter = std::clamp(settings.jitter, 0.0, 1.0);
  const double collisionThreshold = std::clamp(settings.collisionThreshold, 0.02, 2.0);

  SphericalVoronoiMeshSettings meshSettings{};
  meshSettings.seed = settings.seed ^ 0x1843bead5eed5eedull;
  meshSettings.regionCount = automaticRegionCount(settings);
  meshSettings.jitter = std::clamp(0.35 + jitter * 0.55, 0.0, 1.0);
  meshSettings.workerCount = settings.graphWorkerCount;
  SphericalVoronoiMesh mesh{};
  SphericalVoronoiMeshGenerator meshGenerator;
  if (!meshGenerator.generate(meshSettings, mesh))
    return false;
  if (profile != nullptr)
    profile->graphMeshMs = elapsedMilliseconds(stageStart);
  stageStart = ProfileClock::now();

  const uint32_t regionCount = static_cast<uint32_t>(mesh.regions.size());
  const uint32_t plateCount = settings.plateCount == 0u ? automaticPlateCount(settings) : std::clamp(settings.plateCount, 2u, std::min(255u, regionCount));
  const double oceanicFraction = std::clamp(settings.oceanicPlateFraction, 0.0, 1.0);

  std::vector<Plate> plates(plateCount);
  std::vector<Cell> regions(regionCount);
  std::vector<int32_t> assigned(regionCount, -1);
  std::vector<uint32_t> queue;
  queue.reserve(regionCount);
  for (uint32_t plate = 0u; plate < plateCount; ++plate)
  {
    const uint64_t seed = settings.seed ^ (0x2f39a15d31b53f75ull + plate * 0x9e3779b97f4a7c15ull);
    uint32_t region = static_cast<uint32_t>(splitmix64(seed) % regionCount);
    for (uint32_t attempt = 0u; attempt < regionCount && assigned[region] != -1; ++attempt)
      region = (region + 1u + static_cast<uint32_t>(splitmix64(seed + attempt) % 37u)) % regionCount;
    const bool continental = hashUnit(seed ^ 0x94d049bb133111ebull) > oceanicFraction;
    const Vec3 axis = normalize(Vec3{hashUnit(seed ^ 0x6a09e667f3bcc909ull) * 2.0 - 1.0, hashUnit(seed ^ 0xbb67ae8584caa73bull) * 2.0 - 1.0, hashUnit(seed ^ 0xa4093822299f31d0ull) * 2.0 - 1.0});
    const double speed = 0.35 + hashUnit(seed ^ 0x13198a2e03707344ull) * 1.15;
    plates[plate] = Plate{
      region,
      continental,
      continental ? 0.30 + hashUnit(seed ^ 0x243f6a8885a308d3ull) * 0.42 : -0.62 + hashUnit(seed ^ 0x510e527fade682d1ull) * 0.20,
      0.18 + hashUnit(seed ^ 0x9b05688c2b3e6c1full) * 0.70,
      multiply(axis, speed)};
    assigned[region] = static_cast<int32_t>(plate);
    queue.push_back(region);
  }
  for (uint32_t queueOut = 0u; queueOut < queue.size(); ++queueOut)
  {
    const uint64_t seed = settings.seed ^ (0x452821e638d01377ull + queueOut * 0xbf58476d1ce4e5b9ull);
    const uint32_t remaining = static_cast<uint32_t>(queue.size() - queueOut);
    const uint32_t randomWindow = std::max(1u, static_cast<uint32_t>(std::lround(static_cast<double>(remaining) * jitter)));
    const uint32_t swapIndex = queueOut + static_cast<uint32_t>(splitmix64(seed) % randomWindow);
    std::swap(queue[queueOut], queue[swapIndex]);
    const uint32_t region = queue[queueOut];
    for (const uint32_t neighbour : mesh.regionNeighbours[region])
      if (assigned[neighbour] == -1)
      {
        assigned[neighbour] = assigned[region];
        queue.push_back(neighbour);
      }
  }
  for (uint32_t region = 0u; region < regionCount; ++region)
    regions[region].plate = static_cast<uint32_t>(std::max(0, assigned[region]));

  parallelFor(
      regionCount,
      settings.graphWorkerCount,
      [&](const uint32_t region)
      {
        const Vec3 position = toVec3(mesh.regions[region]);
        const Plate &plate = plates[regions[region].plate];
        const Vec3 velocity = cross(plate.angularVelocity, position);
        for (const uint32_t neighbour : mesh.regionNeighbours[region])
        {
          if (regions[neighbour].plate == regions[region].plate)
            continue;
          const Vec3 neighbourPosition = toVec3(mesh.regions[neighbour]);
          const Plate &otherPlate = plates[regions[neighbour].plate];
          const Vec3 otherVelocity = cross(otherPlate.angularVelocity, neighbourPosition);
          const Vec3 tangent = normalize(subtract(neighbourPosition, multiply(position, dot(position, neighbourPosition))));
          const double closing = dot(subtract(velocity, otherVelocity), tangent);
          const bool landLand = plate.continental && otherPlate.continental;
          const bool mixed = plate.continental != otherPlate.continental;
          if (closing > collisionThreshold * 0.84)
            regions[region].uplift = std::max(regions[region].uplift, closing * (landLand ? 0.68 : mixed ? 0.50 : 0.14));
          else if (closing < -collisionThreshold * 0.84)
            regions[region].rift = std::max(regions[region].rift, -closing * (landLand ? 0.44 : 0.22));
        }
      });

  std::vector<double> rawUplift(regionCount, 0.0);
  std::vector<double> rawRift(regionCount, 0.0);
  parallelFor(
      regionCount,
      settings.graphWorkerCount,
      [&](const uint32_t region)
      {
        rawUplift[region] = regions[region].uplift;
        rawRift[region] = regions[region].rift;
      });
  // Keep tectonic belt width stable in kilometres as graph detail changes.
  // Graph edge length scales with 1/sqrt(N), so propagation radius must scale
  // with sqrt(N) instead of using a fixed number of cells.
  const double detailScale = std::sqrt(static_cast<double>(regionCount) / 20000.0) * std::clamp(settings.mountainBeltWidth, 0.25, 3.0);
  const uint32_t upliftIterations = std::clamp(static_cast<uint32_t>(std::lround(10.0 * detailScale)), 6u, 48u);
  const uint32_t riftIterations = std::clamp(static_cast<uint32_t>(std::lround(7.0 * detailScale)), 4u, 36u);
  const std::vector<double> broadUplift = diffuseGraphSignal(mesh, rawUplift, upliftIterations, 0.93, settings.graphWorkerCount);
  const std::vector<double> broadRift = diffuseGraphSignal(mesh, rawRift, riftIterations, 0.90, settings.graphWorkerCount);
  const std::vector<double> convergenceDistance = graphDistanceToSignal(mesh, rawUplift, 1.0e-6);
  const double ridgeWavelength = std::max(0.0105, std::sqrt(4.0 * kPi / static_cast<double>(regionCount)) * 1.25) * std::clamp(settings.mountainRidgeSpacing, 0.35, 3.0);
  std::vector<double> rawPlateElevation(regionCount, 0.0);
  parallelFor(
      regionCount,
      settings.graphWorkerCount,
      [&](const uint32_t region)
      {
        rawPlateElevation[region] = plates[regions[region].plate].baseElevation;
      });
  // Continental crust does not end as a vertical step at a plate edge. A
  // short signed diffusion creates a shelf/margin where seamless noise can
  // move the actual shoreline away from the underlying Voronoi boundary.
  const uint32_t crustBlendIterations = std::clamp(static_cast<uint32_t>(std::lround(4.2 * std::sqrt(static_cast<double>(regionCount) / 20000.0))), 3u, 18u);
  const std::vector<double> blendedPlateElevation = smoothGraphField(mesh, rawPlateElevation, crustBlendIterations, settings.graphWorkerCount);

  SphericalTerrainStatistics stats{};
  stats.plateCount = plateCount;
  for (const Plate &plate : plates)
    if (plate.continental)
      ++stats.continentalPlateCount;
    else
      ++stats.oceanicPlateCount;

  parallelFor(
      regionCount,
      settings.graphWorkerCount,
      [&](const uint32_t region)
      {
        const SpherePoint &point = mesh.regions[region];
        const Vec3 spherePosition = toVec3(point);
        const Plate &plate = plates[regions[region].plate];
        const double latitude01 = 0.5 - std::asin(std::clamp(point.y, -1.0, 1.0)) / kPi;
        // Plate convergence and divergence are the sole macro relief sources. Noise
        // represents heterogeneous crustal strength; it no longer constructs a
        // separate mountain curve or multiplies a boundary-distance field.
        const double crustVariation = sphereFbm(spherePosition, settings.seed ^ 0x5be0cd19137e2179ull, 1.10, 6u);
        const double crustStrength = std::clamp(0.82 + sphereFbm(spherePosition, settings.seed ^ 0xd1b54a32d192ed03ull, 2.3, 4u) * 0.32, 0.55, 1.15);
        const double reliefAmount = std::clamp(settings.interiorRelief, 0.0, 3.0);
        const double shorelineAmount = std::clamp(settings.shorelineDetail, 0.0, 3.0);
        const double continentalWeight = smoothstep((blendedPlateElevation[region] + 0.30) / 0.72);
        const double plateProvince = sphereFbm(spherePosition, settings.seed ^ 0x6f067aa72176fba7ull, 0.82, 6u);
        double elevation = blendedPlateElevation[region] * 0.45;
        elevation += plateProvince * (0.11 + reliefAmount * 0.045) * continentalWeight;
        elevation += crustVariation * (0.14 + jitter * 0.055);
        elevation += broadUplift[region] * crustStrength * (plate.continental ? 0.62 : 0.15) * std::clamp(settings.mountainUplift, 0.0, 3.0);
        elevation -= broadRift[region] * (plate.continental ? 0.24 : 0.14);
        if (std::isfinite(convergenceDistance[region]) && broadUplift[region] > 1.0e-5)
        {
          // A warped multi-scale distance phasor turns smooth convergence uplift
          // into parallel, branching ridge/valley structure. The scalar distance
          // field stays continuous where nearest boundary sources change, avoiding
          // the hard angular seams produced by per-cell direction choices.
          const double distanceWarp = sphereFbm(spherePosition, settings.seed ^ 0x8cb92baa37d6f51dull, 7.0, 4u) * ridgeWavelength * 0.52;
          const double fineWarp = sphereFbm(spherePosition, settings.seed ^ 0x9e3779b185ebca87ull, 23.0, 3u) * ridgeWavelength * 0.16;
          const double warpedDistance = convergenceDistance[region] + distanceWarp + fineWarp;
          const double primary = std::pow(0.5 + 0.5 * std::cos((2.0 * kPi * warpedDistance) / ridgeWavelength), 3.4);
          const double secondary = std::pow(0.5 + 0.5 * std::cos((2.0 * kPi * warpedDistance) / (ridgeWavelength * 0.53) + 1.7), 4.2);
          const double ridgeTexture = primary * 0.72 + secondary * 0.28;
          const double activity = smoothstep(broadUplift[region] * 4.0) * (plate.continental ? 1.0 : 0.28);
          const double ridgeHeight =
              std::clamp(settings.mountainRidgeHeight, 0.0, 3.0);
          elevation += (ridgeTexture - 0.22) * activity * crustStrength *
              0.17 * ridgeHeight;
          // Only the strongest convergent spines receive the extra height.
          // This creates recognisable summits and saddles instead of raising an
          // entire mountain belt into one uniformly noisy wall.
          const double crest = smoothstep((ridgeTexture - 0.38) / 0.46);
          const double peakVariation = std::clamp(
              0.72 + sphereFbm(spherePosition,
                         settings.seed ^ 0x94d049bb133111ebull, 12.0, 3u) *
                  0.28,
              0.42, 1.0);
          elevation += crest * crest * activity * crustStrength *
              peakVariation * 0.052 * ridgeHeight;
        }
        const double shorelineBand = 1.0 - smoothstep(std::abs(elevation) / 0.20);
        const double shorelineNoise = sphereFbm(spherePosition, settings.seed ^ 0x71d67fffeda60031ull, 3.1, 5u) * 0.78 + sphereFbm(spherePosition, settings.seed ^ 0xff51afd7ed558ccdull, 10.5, 3u) * 0.22;
        elevation += shorelineNoise * 0.15 * shorelineAmount * shorelineBand;
        regions[region].uplift = broadUplift[region];
        regions[region].rift = broadRift[region];
        regions[region].elevation = elevation;
        const bool land = elevation > 0.0;
        regions[region].mountain = land ? std::clamp(broadUplift[region] * 0.72 + std::max(0.0, elevation - 0.24) * 0.58, 0.0, 1.0) : 0.0;

        const double latitude = std::abs(latitude01 * 2.0 - 1.0);
        const double normalizedHeight = encodedLandElevation(elevation);
        regions[region].temperature = std::clamp(1.0 - std::pow(latitude, 1.55) - normalizedHeight * 0.42 + sphereFbm(spherePosition, settings.seed ^ 0x1f83d9abfb41bd6bull, 2.2, 4u) * 0.10, 0.0, 1.0);
        const double wetLatitude = 0.28 + 0.42 * std::exp(-std::pow(latitude / 0.30, 2.0)) + 0.16 * std::exp(-std::pow((latitude - 0.58) / 0.18, 2.0));
        regions[region].precipitation = std::clamp(
            (land ? plate.moisture * 0.42 : 0.92) + wetLatitude + broadUplift[region] * 0.10 - broadRift[region] * 0.10 + sphereFbm(spherePosition, settings.seed ^ 0xcbbb9d5dc1059ed8ull, 2.0, 4u) * 0.16, 0.0, 1.0);
      });

  // Keep the ocean/land decision tectonic, then shape the continental margin
  // from geodesic distance to that stable coastline. Previously only the
  // mountain *colour mask* was reduced near water; the uplifted elevation was
  // left intact, producing beaches directly beneath steep ranges.
  std::vector<double> oceanSignal(regionCount, 0.0);
  parallelFor(
      regionCount,
      settings.graphWorkerCount,
      [&](const uint32_t region)
      {
        oceanSignal[region] = regions[region].elevation <= 0.0 ? 1.0 : 0.0;
      });
  const std::vector<double> distanceToOcean = graphDistanceToSignal(mesh, oceanSignal, 0.5);
  const double typicalEdge = std::sqrt(4.0 * kPi / static_cast<double>(regionCount));
  const double coastalWidth = 0.028 * std::clamp(settings.coastalPlainWidth, 0.25, 3.0);
  const double interiorStrength = std::clamp(settings.interiorRelief, 0.0, 3.0) * (0.40 + std::clamp(settings.roughness, 0.0, 1.0) * 1.50);
  parallelFor(
      regionCount,
      settings.graphWorkerCount,
      [&](const uint32_t region)
      {
        if (regions[region].elevation <= 0.0)
          return;
        const Vec3 position = toVec3(mesh.regions[region]);
        const double distance = distanceToOcean[region];
        const double interior = smoothstep((distance - typicalEdge * 0.55) / std::max(1.0e-6, coastalWidth));

        // Broad province noise chooses where a continent stays as a plain and
        // where rolling hills/old uplands appear. Higher-frequency ridged noise is
        // deliberately much weaker than active tectonic mountain relief.
        const double province = std::clamp(0.5 + sphereFbm(position, settings.seed ^ 0x3c6ef372fe94f82bull, 1.65, 4u), 0.0, 1.0);
        const double broadRelief = sphereFbm(position, settings.seed ^ 0x243f6a8885a308d3ull, 1.05, 6u);
        const double rolling = sphereFbm(position, settings.seed ^ 0xbb67ae8584caa73bull, 4.6, 6u);
        const double folded = std::pow(std::clamp(1.0 - std::abs(sphereFbm(position, settings.seed ^ 0xa4093822299f31d0ull, 9.5, 4u)) * 2.8, 0.0, 1.0), 3.0) - 0.46;
        const double basin = sphereFbm(
            position, settings.seed ^ 0x13198a2e03707344ull, 0.62, 5u);
        const double plateau = smoothstep(
            (sphereFbm(position, settings.seed ^ 0x082efa98ec4e6c89ull,
                 2.45, 4u) +
                0.24) /
            0.48) - 0.5;
        const double relief =
            (broadRelief * (0.055 + province * 0.065) +
             rolling * (0.035 + province * province * 0.075) +
             folded * 0.026 + basin * 0.042 + plateau * 0.032) *
            interiorStrength * interior;
        regions[region].elevation = std::max(1.0e-5, regions[region].elevation + relief);

        // A low cap near the water creates beaches and coastal plains. It blends
        // out smoothly rather than carving a constant-width ring into continents.
        const double coastalCap = 0.014 + distance * 1.35;
        const double flattened = std::min(regions[region].elevation, coastalCap);
        regions[region].elevation = flattened + (regions[region].elevation - flattened) * interior;
        regions[region].uplift *= 0.06 + interior * 0.94;
        regions[region].mountain *= 0.04 + interior * 0.96;
      });

  if (profile != nullptr)
    profile->graphFieldsMs = elapsedMilliseconds(stageStart);
  stageStart = ProfileClock::now();

  // From here on the graph is the terrain. Erosion changes region elevation,
  // then climate observes those final slopes, and drainage observes the final
  // climate. No raster result is fed back into the globe preview.
  erodeSphericalGraph(mesh, settings, regions);
  // Ridge sharpening intentionally exaggerates local maxima and can therefore
  // partially undo the coastal cap. Reapply the same continuous continental
  // margin after erosion so beaches cannot acquire a one-cell cliff while
  // inland ranges retain the eroded profile.
  parallelFor(
      regionCount,
      settings.graphWorkerCount,
      [&](const uint32_t region)
      {
        if (regions[region].elevation <= 0.0)
          return;
        const double distance = distanceToOcean[region];
        const double interior = smoothstep((distance - typicalEdge * 0.55) / std::max(1.0e-6, coastalWidth));
        const double coastalCap = 0.014 + distance * 1.35;
        const double flattened = std::min(regions[region].elevation, coastalCap);
        regions[region].elevation = flattened + (regions[region].elevation - flattened) * interior;
      });
  if (profile != nullptr)
    profile->graphErosionMs = elapsedMilliseconds(stageStart);
  stageStart = ProfileClock::now();

  simulateGraphClimate(mesh, plates, regions, settings.seed, settings.graphWorkerCount, settings.climateWetness);
  // Distance from moisture sources and the descending branches of the global
  // circulation create broad continental drylands. Seamless province noise
  // breaks those belts into deserts, steppes, and greener refuges instead of
  // producing latitude-coloured stripes.
  const double desertAmount = std::clamp(settings.desertStrength, 0.0, 3.0);
  parallelFor(
      regionCount,
      settings.graphWorkerCount,
      [&](const uint32_t region)
      {
        if (regions[region].elevation <= 0.0)
          return;
        const Vec3 position = toVec3(mesh.regions[region]);
        const double latitude = std::abs(std::asin(std::clamp(position.y, -1.0, 1.0))) / (kPi * 0.5);
        const double subtropical = std::exp(-std::pow((latitude - 0.32) / 0.15, 2.0));
        const double continentality = smoothstep((distanceToOcean[region] - std::max(0.035, coastalWidth)) / 0.18);
        const double dryProvince = std::clamp(0.5 + sphereFbm(position, settings.seed ^ 0x106aa07032bbd1b8ull, 1.9, 4u), 0.0, 1.0);
        const double rainShadow = 1.0 - smoothstep(regions[region].windExposure * 1.45);
        const double drying = desertAmount * continentality * (0.08 + subtropical * 0.42) * (0.58 + dryProvince * 0.32 + rainShadow * 0.18);
        regions[region].precipitation *= std::clamp(1.0 - drying, 0.16, 1.0);

        // Desert terrain is shaped by coherent wind bands after climate has
        // identified dry provinces. It deliberately contains no high-frequency
        // noise: the smaller voxel pages inherit smooth, traversable slopes.
        const double dryMask =
            smoothstep((0.27 - regions[region].precipitation) / 0.16) *
            smoothstep((regions[region].temperature - 0.34) / 0.28) *
            (1.0 - smoothstep((regions[region].mountain - 0.28) / 0.42)) *
            continentality;
        const double dunePhase =
            2.0 * kPi *
                (position.x * 0.79 + position.z * 0.61) * 65.0 +
            sphereFbm(position, settings.seed ^ 0x3bd39e10cb0ef593ull,
                7.0, 3u) *
                1.7;
        const double duneEnvelope = std::clamp(
            0.72 + sphereFbm(position,
                       settings.seed ^ 0xc0acf169b5f18a8cull, 2.2, 4u) *
                    0.28,
            0.32, 1.0);
        const double duneShape =
            std::sin(dunePhase) + 0.27 * std::sin(dunePhase * 2.0);
        regions[region].elevation = std::max(
            1.0e-5, regions[region].elevation +
                duneShape * duneEnvelope * dryMask * 0.0085 *
                    std::min(1.5, desertAmount));
      });
  stats.lakePixelCount = formGraphLakes(mesh, distanceToOcean, typicalEdge, settings.lakeAbundance, regions);
  stats.landPixelCount = 0u;
  stats.oceanPixelCount = 0u;
  stats.mountainPixelCount = 0u;
  for (uint32_t region = 0u; region < regionCount; ++region)
  {
    if (regions[region].elevation <= 0.0)
    {
      regions[region].mountain = 0.0;
      ++stats.oceanPixelCount;
      continue;
    }
    ++stats.landPixelCount;
    if (regions[region].lake)
    {
      regions[region].mountain = 0.0;
      continue;
    }
    double neighbourMean = 0.0;
    double maximumSlope = 0.0;
    uint32_t landNeighbours = 0u;
    for (const uint32_t neighbour : mesh.regionNeighbours[region])
    {
      if (regions[neighbour].elevation <= 0.0)
        continue;
      const double edge = graphEdgeDistance(mesh.regions[region], mesh.regions[neighbour]);
      neighbourMean += regions[neighbour].elevation;
      maximumSlope = std::max(maximumSlope, std::abs(regions[region].elevation - regions[neighbour].elevation) / edge);
      ++landNeighbours;
    }
    neighbourMean /= std::max(1u, landNeighbours);
    const double ridge = std::max(0.0, regions[region].elevation - neighbourMean);
    double mountain = smoothstep((maximumSlope - 0.16) / 1.20) * 0.58 + smoothstep(ridge / 0.045) * 0.34 + smoothstep((regions[region].elevation - 0.30) / 0.42) * 0.20;
    mountain *= 0.36 + smoothstep((regions[region].uplift - 0.015) / 0.30) * 0.64;
    const double coastInterior = smoothstep((distanceToOcean[region] - typicalEdge * 0.55) / std::max(1.0e-6, coastalWidth));
    mountain *= 0.04 + coastInterior * 0.96;
    regions[region].mountain = std::clamp(mountain, 0.0, 1.0);
    if (regions[region].mountain > 0.48)
      ++stats.mountainPixelCount;
  }

  if (profile != nullptr)
    profile->graphClimateMs = elapsedMilliseconds(stageStart);
  stageStart = ProfileClock::now();

  // The reference implementation routes rivers on Delaunay triangles, not on
  // the Voronoi regions or an equirectangular grid. Build that same dual graph.
  struct TriangleEdgeUse
  {
    uint64_t key = 0u;
    uint32_t firstTriangle = 0u;
    uint32_t secondTriangle = 0u;
  };
  const uint32_t triangleCount = static_cast<uint32_t>(mesh.triangles.size());
  std::vector<std::pair<uint64_t, uint32_t>> triangleEdgeReferences;
  triangleEdgeReferences.reserve(static_cast<size_t>(triangleCount) * 3u);
  for (uint32_t triangleIndex = 0u; triangleIndex < mesh.triangles.size(); ++triangleIndex)
  {
    const auto &triangle = mesh.triangles[triangleIndex];
    for (uint32_t edge = 0u; edge < 3u; ++edge)
      triangleEdgeReferences.emplace_back(graphEdgeKey(triangle[edge], triangle[(edge + 1u) % 3u]), triangleIndex * 3u + edge);
  }
  std::sort(
      triangleEdgeReferences.begin(),
      triangleEdgeReferences.end(),
      [](const auto &left, const auto &right)
      {
        return left.first < right.first;
      });
  std::vector<TriangleEdgeUse> triangleEdges;
  triangleEdges.reserve(triangleEdgeReferences.size() / 2u);
  std::vector<uint32_t> triangleEdgeOpposite(static_cast<size_t>(triangleCount) * 3u, std::numeric_limits<uint32_t>::max());
  std::vector<std::vector<uint32_t>> triangleNeighbours(triangleCount);
  for (size_t begin = 0u; begin < triangleEdgeReferences.size();)
  {
    size_t end = begin + 1u;
    while (end < triangleEdgeReferences.size() && triangleEdgeReferences[end].first == triangleEdgeReferences[begin].first)
      ++end;
    if (end - begin == 2u)
    {
      const uint32_t firstReference = triangleEdgeReferences[begin].second;
      const uint32_t secondReference = triangleEdgeReferences[begin + 1u].second;
      const uint32_t firstTriangle = firstReference / 3u;
      const uint32_t secondTriangle = secondReference / 3u;
      triangleEdges.push_back(TriangleEdgeUse{triangleEdgeReferences[begin].first, firstTriangle, secondTriangle});
      triangleEdgeOpposite[firstReference] = secondTriangle;
      triangleEdgeOpposite[secondReference] = firstTriangle;
      triangleNeighbours[firstTriangle].push_back(secondTriangle);
      triangleNeighbours[secondTriangle].push_back(firstTriangle);
    }
    begin = end;
  }
  triangleEdgeReferences.clear();
  triangleEdgeReferences.shrink_to_fit();
  std::vector<double> triangleElevation(triangleCount, 0.0);
  std::vector<double> triangleMoisture(triangleCount, 0.0);
  parallelFor(
      triangleCount,
      settings.graphWorkerCount,
      [&mesh, &regions, &triangleElevation, &triangleMoisture](const uint32_t triangle)
      {
        const auto &corners = mesh.triangles[triangle];
        triangleElevation[triangle] = (visibleElevation(regions[corners[0]]) + visibleElevation(regions[corners[1]]) + visibleElevation(regions[corners[2]])) / 3.0;
        triangleMoisture[triangle] = (regions[corners[0]].precipitation + regions[corners[1]].precipitation + regions[corners[2]].precipitation) / 3.0;
      });

  using TriangleDrainageNode = std::pair<double, uint32_t>;
  std::priority_queue<TriangleDrainageNode, std::vector<TriangleDrainageNode>, std::greater<TriangleDrainageNode>> triangleQueue;
  std::vector<uint8_t> triangleVisited(triangleCount, 0u);
  std::vector<double> triangleDrainageSurface = triangleElevation;
  std::vector<int32_t> triangleDownflow(triangleCount, -1);
  std::vector<uint32_t> triangleOrder;
  triangleOrder.reserve(triangleCount);
  for (uint32_t triangle = 0u; triangle < triangleCount; ++triangle)
  {
    if (triangleElevation[triangle] >= 0.0)
      continue;
    triangleVisited[triangle] = 1u;
    double bestElevation = triangleElevation[triangle];
    for (const uint32_t neighbour : triangleNeighbours[triangle])
      if (triangleElevation[neighbour] < bestElevation)
      {
        bestElevation = triangleElevation[neighbour];
        triangleDownflow[triangle] = static_cast<int32_t>(neighbour);
      }
    triangleQueue.emplace(triangleDrainageSurface[triangle], triangle);
    triangleOrder.push_back(triangle);
  }
  while (!triangleQueue.empty())
  {
    const auto [surfaceElevation, current] = triangleQueue.top();
    triangleQueue.pop();
    if (surfaceElevation > triangleDrainageSurface[current] + 1.0e-12)
      continue;
    for (const uint32_t neighbour : triangleNeighbours[current])
    {
      if (triangleVisited[neighbour] != 0u || triangleElevation[neighbour] < 0.0)
        continue;
      triangleVisited[neighbour] = 1u;
      triangleDownflow[neighbour] = static_cast<int32_t>(current);
      triangleDrainageSurface[neighbour] = std::max(triangleElevation[neighbour], surfaceElevation + 1.0e-9);
      triangleQueue.emplace(triangleDrainageSurface[neighbour], neighbour);
      triangleOrder.push_back(neighbour);
    }
  }

  std::vector<double> triangleFlow(triangleCount, 0.0);
  for (uint32_t triangle = 0u; triangle < triangleCount; ++triangle)
    if (triangleElevation[triangle] >= 0.0)
      triangleFlow[triangle] = 0.5 * triangleMoisture[triangle] * triangleMoisture[triangle];
  for (auto order = triangleOrder.rbegin(); order != triangleOrder.rend(); ++order)
  {
    const uint32_t triangle = *order;
    if (triangleElevation[triangle] < 0.0 || triangleDownflow[triangle] < 0)
      continue;
    const uint32_t downflow = static_cast<uint32_t>(triangleDownflow[triangle]);
    triangleFlow[downflow] += triangleFlow[triangle];
    if (triangleElevation[downflow] > triangleElevation[triangle])
      triangleElevation[downflow] = triangleElevation[triangle];
  }
  // Catchment area, rather than a requested river count, determines the
  // network. This threshold exposes tributaries while accumulated flow still
  // controls their visual width.
  constexpr double riverThreshold = 0.42;
  std::vector<uint8_t> triangleRivers(triangleCount, 0u);
  std::vector<uint32_t> riverInputs(triangleCount, 0u);
  for (uint32_t triangle = 0u; triangle < triangleCount; ++triangle)
  {
    if (triangleElevation[triangle] < 0.0 || triangleDownflow[triangle] < 0 || triangleFlow[triangle] <= riverThreshold)
      continue;
    const double alpha = std::clamp(0.10 * std::sqrt(triangleFlow[triangle]), 0.0, 1.0);
    triangleRivers[triangle] = static_cast<uint8_t>(std::lround((0.18 + alpha * 0.82) * 255.0));
    const uint32_t downflow = static_cast<uint32_t>(triangleDownflow[triangle]);
    if (triangleRivers[downflow] != 0u || triangleFlow[downflow] > riverThreshold)
      ++riverInputs[downflow];
  }
  stats.riverSourceCount = 0u;
  for (uint32_t triangle = 0u; triangle < triangleCount; ++triangle)
    if (triangleRivers[triangle] != 0u && riverInputs[triangle] == 0u)
      ++stats.riverSourceCount;

  maps = {};
  maps.width = width;
  maps.height = height;
  if (settings.rasterizeMaps)
  {
    maps.heightmap.assign(cellCount, 0u);
    maps.oceanFloorHeightmap.assign(cellCount, 0u);
    maps.waterHeightmap.assign(cellCount, 0u);
    maps.oceanMask.assign(cellCount, 0u);
    maps.lakeMask.assign(cellCount, 0u);
    maps.sourceRegions.assign(cellCount, 0u);
    maps.plates.assign(cellCount, 0u);
    maps.continents.assign(cellCount, 0u);
    maps.tectonics.assign(cellCount, 0u);
    maps.mountains.assign(cellCount, 0u);
    maps.rivers.assign(cellCount, 0u);
    maps.temperature.assign(cellCount, 0u);
    maps.precipitation.assign(cellCount, 0u);
    maps.wind.assign(cellCount, 0u);
    maps.windEast.assign(cellCount, 127u);
    maps.windNorth.assign(cellCount, 127u);
    maps.plateMotionEast.assign(cellCount, 127u);
    maps.plateMotionNorth.assign(cellCount, 127u);
    maps.materials.assign(cellCount, 0u);
  }
  maps.regionPositions = mesh.regions;
  maps.regionElevations.assign(regionCount, 0u);
  maps.regionRivers.assign(regionCount, 0u);

  for (uint32_t region = 0u; region < regionCount; ++region)
  {
    const bool land = regions[region].elevation > 0.0;
    maps.regionElevations[region] = quantizeUnit(encodedLandElevation(visibleElevation(regions[region])));
    for (const uint32_t triangle : mesh.regionTriangles[region])
      maps.regionRivers[region] = std::max(maps.regionRivers[region], triangleRivers[triangle]);
  }
  std::vector<uint8_t> regionMaterials(regionCount, 0u);
  parallelFor(
      regionCount,
      settings.graphWorkerCount,
      [&](const uint32_t region)
      {
        const bool coast = distanceToOcean[region] <= typicalEdge * 1.75;
        regionMaterials[region] = classifyGraphMaterial(regions[region], maps.regionRivers[region], coast, toVec3(mesh.regions[region]), settings.seed, settings.biomeVariation);
      });
  const GraphDrainage finalRegionDrainage = resolveGraphDrainage(mesh, regions);
  maps.regionNodes.resize(regionCount);
  maps.regionNeighbourOffsets.resize(static_cast<size_t>(regionCount) + 1u, 0u);
  parallelFor(
      regionCount,
      settings.graphWorkerCount,
      [&](const uint32_t region)
      {
        const Cell &cell = regions[region];
        const Plate &plate = plates[cell.plate];
        const bool land = cell.elevation > 0.0;
        const Vec3 position = toVec3(mesh.regions[region]);
        const double longitude = std::atan2(position.z, position.x);
        const Vec3 east = normalize(Vec3{-std::sin(longitude), 0.0, std::cos(longitude)});
        const Vec3 north = normalize(cross(east, position));
        const Vec3 plateVelocity = cross(plate.angularVelocity, position);
        maps.regionNodes[region] = SphericalTerrainNode{
          .position = mesh.regions[region],
          .elevation = static_cast<uint16_t>(std::lround(encodedLandElevation(visibleElevation(cell)) * 65535.0)),
          .plate = static_cast<uint8_t>(cell.plate % 255u),
          .continent = quantizeUnit(land ? std::clamp(0.54 + cell.elevation * 0.36 + (plate.continental ? 0.10 : -0.04), 0.0, 1.0) : std::clamp(0.42 + cell.elevation * 0.30, 0.0, 0.50)),
          .tectonics = quantizeUnit(std::clamp(cell.uplift * 1.2 + cell.rift * 0.8, 0.0, 1.0)),
          .mountain = quantizeUnit(cell.mountain),
          .river = maps.regionRivers[region],
          .temperature = quantizeUnit(cell.temperature),
          .precipitation = quantizeUnit(cell.precipitation),
          .windExposure = quantizeUnit(cell.windExposure),
          .windEast = quantizeSignedUnit(cell.windEast),
          .windNorth = quantizeSignedUnit(cell.windNorth),
          .plateMotionEast = quantizeSignedUnit(std::clamp(dot(plateVelocity, east) * 0.72, -1.0, 1.0)),
          .plateMotionNorth = quantizeSignedUnit(std::clamp(dot(plateVelocity, north) * 0.72, -1.0, 1.0)),
          .material = regionMaterials[region],
          .terrainElevation = static_cast<uint16_t>(std::lround(encodedLandElevation(cell.elevation) * 65535.0)),
          .waterElevation = static_cast<uint16_t>(cell.lake ? std::lround(encodedLandElevation(cell.lakeSurface) * 65535.0) : 0),
          .downstreamRegion = finalRegionDrainage.downflow[region] >= 0 ? static_cast<uint32_t>(finalRegionDrainage.downflow[region]) : SphericalTerrainNode::noRegion,
          .riverDischarge = static_cast<float>(finalRegionDrainage.flow[region]),
          .flags = static_cast<uint8_t>((cell.elevation <= 0.0 ? SphericalTerrainNode::oceanFlag : 0u) | (cell.lake ? SphericalTerrainNode::lakeFlag : 0u)),
        };
      });
  for (uint32_t region = 0u; region < regionCount; ++region)
    maps.regionNeighbourOffsets[region + 1u] = maps.regionNeighbourOffsets[region] + static_cast<uint32_t>(mesh.regionNeighbours[region].size());
  maps.regionNeighbourIndices.resize(maps.regionNeighbourOffsets.back());
  parallelFor(
      regionCount,
      settings.graphWorkerCount,
      [&](const uint32_t region)
      {
        std::copy(mesh.regionNeighbours[region].begin(), mesh.regionNeighbours[region].end(), maps.regionNeighbourIndices.begin() + maps.regionNeighbourOffsets[region]);
      });

  if (profile != nullptr)
    profile->graphRiversMs = elapsedMilliseconds(stageStart);
  stageStart = ProfileClock::now();

  // Reproduce QuadGeometry.setMap from the reference implementation. Each
  // Delaunay half-edge becomes one triangle. Coasts and flowing edges fold
  // towards the two triangle centres (a valley); all other edges fold across
  // the two region sites (a ridge). Derivative shading can then reveal relief
  // without displacing or UV-mapping the sphere.
  maps.surfacePositions = mesh.regions;
  maps.surfacePositions.insert(maps.surfacePositions.end(), mesh.triangleCenters.begin(), mesh.triangleCenters.end());
  maps.surfaceElevations.reserve(maps.surfacePositions.size());
  for (const Cell &region : regions)
    maps.surfaceElevations.push_back(static_cast<uint16_t>(std::lround(encodedLandElevation(visibleElevation(region)) * 65535.0)));
  for (const double elevation : triangleElevation)
    maps.surfaceElevations.push_back(static_cast<uint16_t>(std::lround(encodedLandElevation(elevation) * 65535.0)));
  const auto appendSurfaceAttribute = [&mesh, &regions, regionCount](std::vector<uint8_t> &attribute, const auto &regionValue)
  {
    attribute.reserve(regionCount + mesh.triangles.size());
    for (uint32_t region = 0u; region < regionCount; ++region)
      attribute.push_back(regionValue(region));
    for (const auto &triangle : mesh.triangles)
    {
      const uint32_t value = static_cast<uint32_t>(regionValue(triangle[0])) + regionValue(triangle[1]) + regionValue(triangle[2]);
      attribute.push_back(static_cast<uint8_t>((value + 1u) / 3u));
    }
  };
  appendSurfaceAttribute(
      maps.surfaceContinents,
      [&regions, &plates](const uint32_t region)
      {
        const Cell &cell = regions[region];
        const bool land = cell.elevation > 0.0;
        return quantizeUnit(land ? std::clamp(0.54 + cell.elevation * 0.36 + (plates[cell.plate].continental ? 0.10 : -0.04), 0.0, 1.0) : std::clamp(0.42 + cell.elevation * 0.30, 0.0, 0.50));
      });
  appendSurfaceAttribute(
      maps.surfaceTectonics,
      [&regions](const uint32_t region)
      {
        return quantizeUnit(std::clamp(regions[region].uplift * 1.2 + regions[region].rift * 0.8, 0.0, 1.0));
      });
  appendSurfaceAttribute(
      maps.surfaceMountains,
      [&regions](const uint32_t region)
      {
        return quantizeUnit(regions[region].mountain);
      });
  appendSurfaceAttribute(
      maps.surfaceTemperature,
      [&regions](const uint32_t region)
      {
        return quantizeUnit(regions[region].temperature);
      });
  appendSurfaceAttribute(
      maps.surfacePrecipitation,
      [&regions](const uint32_t region)
      {
        return quantizeUnit(regions[region].precipitation);
      });
  appendSurfaceAttribute(
      maps.surfacePlates,
      [&regions](const uint32_t region)
      {
        return static_cast<uint8_t>(regions[region].plate % 255u);
      });
  maps.surfaceMaterials.reserve(maps.surfacePositions.size());
  maps.surfaceMaterials.insert(maps.surfaceMaterials.end(), regionMaterials.begin(), regionMaterials.end());
  for (const auto &triangle : mesh.triangles)
  {
    const uint8_t a = regionMaterials[triangle[0]];
    const uint8_t b = regionMaterials[triangle[1]];
    const uint8_t c = regionMaterials[triangle[2]];
    maps.surfaceMaterials.push_back(a == b || a == c ? a : b == c ? b : a);
  }
  maps.surfaceRivers.reserve(maps.surfacePositions.size());
  maps.surfaceRivers.insert(maps.surfaceRivers.end(), maps.regionRivers.begin(), maps.regionRivers.end());
  maps.surfaceRivers.insert(maps.surfaceRivers.end(), triangleRivers.begin(), triangleRivers.end());
  maps.surfaceIndices.reserve(mesh.triangles.size() * 9u);
  const auto appendSurfaceTriangle = [&maps](uint32_t a, uint32_t b, uint32_t c)
  {
    const Vec3 pa = toVec3(maps.surfacePositions[a]);
    const Vec3 pb = toVec3(maps.surfacePositions[b]);
    const Vec3 pc = toVec3(maps.surfacePositions[c]);
    if (dot(cross(subtract(pb, pa), subtract(pc, pa)), add(add(pa, pb), pc)) < 0.0)
      std::swap(b, c);
    maps.surfaceIndices.push_back(a);
    maps.surfaceIndices.push_back(b);
    maps.surfaceIndices.push_back(c);
  };
  for (uint32_t triangle = 0u; triangle < triangleCount; ++triangle)
    for (uint32_t edge = 0u; edge < 3u; ++edge)
    {
      const uint32_t r1 = mesh.triangles[triangle][edge];
      const uint32_t r2 = mesh.triangles[triangle][(edge + 1u) % 3u];
      const uint32_t otherTriangle = triangleEdgeOpposite[triangle * 3u + edge];
      if (otherTriangle == std::numeric_limits<uint32_t>::max())
        continue;
      const bool coast = regions[r1].elevation < 0.0 || regions[r2].elevation < 0.0;
      const bool flowingEdge = (triangleDownflow[triangle] == static_cast<int32_t>(otherTriangle) && triangleRivers[triangle] != 0u) ||
                               (triangleDownflow[otherTriangle] == static_cast<int32_t>(triangle) && triangleRivers[otherTriangle] != 0u);
      if (coast || flowingEdge)
        appendSurfaceTriangle(r1, regionCount + otherTriangle, regionCount + triangle);
      else
        appendSurfaceTriangle(r1, r2, regionCount + triangle);
    }

  maps.sphereLines.reserve(triangleEdges.size() + regionCount / 3u);
  for (const TriangleEdgeUse &edge : triangleEdges)
  {
    const uint32_t a = static_cast<uint32_t>(edge.key >> 32u);
    const uint32_t b = static_cast<uint32_t>(edge.key & 0xffffffffu);
    const bool plateBoundary = regions[a].plate != regions[b].plate;
    if (!plateBoundary)
      continue;
    maps.sphereLines.push_back(SphericalTerrainLine{mesh.triangleCenters[edge.firstTriangle], mesh.triangleCenters[edge.secondTriangle], {0.98f, 0.46f, 0.10f, 0.82f}, 0.0024f});
  }
  for (uint32_t triangle = 0u; triangle < triangleCount; ++triangle)
  {
    if (triangleRivers[triangle] == 0u || triangleDownflow[triangle] < 0)
      continue;
    const float alpha = static_cast<float>(triangleRivers[triangle]) / 255.0f;
    maps.sphereLines.push_back(SphericalTerrainLine{mesh.triangleCenters[triangle], mesh.triangleCenters[static_cast<uint32_t>(triangleDownflow[triangle])], {0.16f, 0.52f, 0.78f, alpha}, 0.00035f + alpha * 0.00055f});
  }

  if (profile != nullptr)
    profile->graphSurfaceMs = elapsedMilliseconds(stageStart);
  stageStart = ProfileClock::now();

  if (!settings.rasterizeMaps)
  {
    stats.riverPixelCount = static_cast<uint32_t>(std::count_if(
        maps.regionRivers.begin(),
        maps.regionRivers.end(),
        [](const uint8_t value)
        {
          return value != 0u;
        }));
    if (statistics != nullptr)
      *statistics = stats;
    if (profile != nullptr)
      profile->graphTotalMs = elapsedMilliseconds(graphStart);
    return true;
  }

  const uint32_t binHeight = std::clamp(static_cast<uint32_t>(std::lround(std::sqrt(static_cast<double>(regionCount)) * 0.55)), 16u, 96u);
  const uint32_t binWidth = binHeight * 2u;
  std::vector<std::vector<uint32_t>> bins(static_cast<size_t>(binWidth) * binHeight);
  for (uint32_t region = 0u; region < regionCount; ++region)
  {
    const SpherePoint &point = mesh.regions[region];
    const double longitude = std::atan2(point.z, point.x);
    const double latitude = std::asin(std::clamp(point.y, -1.0, 1.0));
    const uint32_t bx = static_cast<uint32_t>(std::clamp(static_cast<int32_t>(std::floor(((longitude + kPi) / (2.0 * kPi)) * binWidth)), 0, static_cast<int32_t>(binWidth) - 1));
    const uint32_t by = static_cast<uint32_t>(std::clamp(static_cast<int32_t>(std::floor((0.5 - latitude / kPi) * binHeight)), 0, static_cast<int32_t>(binHeight) - 1));
    bins[by * binWidth + bx].push_back(region);
  }

  parallelFor(
      height,
      settings.graphWorkerCount,
      [&](const uint32_t y)
      {
        for (uint32_t x = 0u; x < width; ++x)
        {
          const uint32_t pixel = cellIndex(x, y, width);
          const uint32_t region = nearestRegion(spherePoint(x, y, width, height), mesh, bins, binWidth, binHeight);
          const Plate &plate = plates[regions[region].plate];
          const bool land = regions[region].elevation > 0.0;
          const double normalizedHeight = encodedLandElevation(regions[region].elevation);
          const double normalizedOceanFloorDepth =
              encodedOceanFloorDepth(regions[region].elevation);
          const double tectonics = std::clamp(regions[region].uplift * 1.2 + regions[region].rift * 0.8, 0.0, 1.0);
          const double continent = land ? std::clamp(0.54 + regions[region].elevation * 0.36 + (plate.continental ? 0.10 : -0.04), 0.0, 1.0) : std::clamp(0.42 + regions[region].elevation * 0.30, 0.0, 0.50);
          maps.heightmap[pixel] = static_cast<uint16_t>(std::lround(normalizedHeight * 65535.0));
          maps.oceanFloorHeightmap[pixel] = land ? 0u :
              static_cast<uint16_t>(std::lround(
                  normalizedOceanFloorDepth * 65535.0));
          maps.waterHeightmap[pixel] = regions[region].lake ?
              static_cast<uint16_t>(std::lround(
                  encodedLandElevation(regions[region].lakeSurface) * 65535.0)) :
              0u;
          maps.oceanMask[pixel] = land ? 0u : 255u;
          maps.lakeMask[pixel] = regions[region].lake ? 255u : 0u;
          maps.sourceRegions[pixel] = region;
          maps.plates[pixel] = static_cast<uint8_t>(regions[region].plate % 255u);
          maps.continents[pixel] = quantizeUnit(continent);
          maps.tectonics[pixel] = quantizeUnit(tectonics);
          maps.mountains[pixel] = quantizeUnit(regions[region].mountain);
          maps.rivers[pixel] = maps.regionRivers[region];
          maps.temperature[pixel] = quantizeUnit(regions[region].temperature);
          maps.precipitation[pixel] = quantizeUnit(regions[region].precipitation);
          maps.materials[pixel] = regionMaterials[region];
          const Vec3 position = spherePoint(x, y, width, height);
          const double longitude = ((static_cast<double>(x) + 0.5) / width) * 2.0 * kPi - kPi;
          const Vec3 east = normalize(Vec3{-std::sin(longitude), 0.0, std::cos(longitude)});
          const Vec3 north = normalize(cross(east, position));
          const Vec3 velocity = cross(plate.angularVelocity, position);
          maps.plateMotionEast[pixel] = quantizeSignedUnit(std::clamp(dot(velocity, east) * 0.72, -1.0, 1.0));
          maps.plateMotionNorth[pixel] = quantizeSignedUnit(std::clamp(dot(velocity, north) * 0.72, -1.0, 1.0));
        }
      });

  std::vector<uint8_t> widenedRivers = maps.rivers;
  for (uint32_t pixel = 0u; pixel < cellCount; ++pixel)
  {
    const uint8_t river = maps.rivers[pixel];
    if (river < 80u)
      continue;
    const uint8_t shoulder = static_cast<uint8_t>(std::max(42, static_cast<int>(std::lround(static_cast<double>(river) * 0.46))));
    for (const uint32_t neighbour : neighbours(pixel, width, height))
      if (maps.heightmap[neighbour] > 0u && widenedRivers[neighbour] < shoulder)
        widenedRivers[neighbour] = shoulder;
  }
  maps.rivers = std::move(widenedRivers);
  stats.riverPixelCount = static_cast<uint32_t>(std::count_if(
      maps.rivers.begin(),
      maps.rivers.end(),
      [](const uint8_t value)
      {
        return value > 0u;
      }));
  if (statistics != nullptr)
    *statistics = stats;
  if (profile != nullptr)
  {
    profile->graphRasterizeMs = elapsedMilliseconds(stageStart);
    profile->graphTotalMs = elapsedMilliseconds(graphStart);
  }
  return true;
}

} // namespace

bool SphericalTerrainGenerator::generate(const SphericalTerrainSettings &settings, SphericalTerrainMaps &maps, SphericalTerrainStatistics *statistics, SphericalTerrainProfile *profile)
{
  if (profile != nullptr)
    *profile = {};
  const ProfileClock::time_point generateStart = ProfileClock::now();
  if (settings.width < 8u || settings.height < 4u)
    return false;
  if (!settings.rasterizeMaps)
  {
    const bool generated = generateVoronoiTerrain(settings, maps, statistics, profile);
    if (profile != nullptr)
      profile->totalMs = elapsedMilliseconds(generateStart);
    return generated;
  }
  const uint32_t width = settings.width;
  const uint32_t height = settings.height;
  const uint32_t cellCount = width * height;
  const double oceanicFraction = std::clamp(settings.oceanicPlateFraction, 0.0, 1.0);
  const double jitter = std::clamp(settings.jitter, 0.0, 1.0);
  const double terrainWarp = std::clamp(settings.terrainWarp, 0.0, 1.0);
  const double roughness = std::clamp(settings.roughness, 0.0, 1.0);

  // The graph is deliberately generated at a bounded macro resolution. It is
  // the authority for plates and large-scale fields, while the requested
  // raster resolution is reserved for coast, relief, climate, and drainage
  // detail. This prevents Voronoi cells from becoming visible terrain pixels.
  SphericalTerrainSettings macroSettings = settings;
  macroSettings.width = std::min(width, 512u);
  macroSettings.height = std::min(height, std::max(4u, static_cast<uint32_t>(std::lround(static_cast<double>(macroSettings.width) * height / width))));
  SphericalTerrainMaps macroMaps{};
  SphericalTerrainStatistics macroStatistics{};
  if (!generateVoronoiTerrain(macroSettings, macroMaps, &macroStatistics, profile))
    return false;
  ProfileClock::time_point stageStart = ProfileClock::now();
  const double pixelsPerRegion = static_cast<double>(macroMaps.width) * macroMaps.height / std::max<size_t>(1u, macroMaps.regionPositions.size());
  const uint32_t regionFootprint = std::clamp(static_cast<uint32_t>(std::ceil(std::sqrt(pixelsPerRegion))), 2u, 10u);
  const uint32_t broadReconstruction = std::min(12u, regionFootprint + regionFootprint / 2u);
  macroMaps.heightmap = blurMacroField(macroMaps.heightmap, macroMaps.width, macroMaps.height, broadReconstruction, 2u);
  macroMaps.continents = blurMacroField(macroMaps.continents, macroMaps.width, macroMaps.height, broadReconstruction, 2u);
  macroMaps.tectonics = blurMacroField(macroMaps.tectonics, macroMaps.width, macroMaps.height, regionFootprint, 2u);
  macroMaps.mountains = blurMacroField(macroMaps.mountains, macroMaps.width, macroMaps.height, regionFootprint, 2u);
  macroMaps.temperature = blurMacroField(macroMaps.temperature, macroMaps.width, macroMaps.height, broadReconstruction, 2u);
  macroMaps.precipitation = blurMacroField(macroMaps.precipitation, macroMaps.width, macroMaps.height, broadReconstruction, 2u);
  if (profile != nullptr)
    profile->macroBlurMs = elapsedMilliseconds(stageStart);
  stageStart = ProfileClock::now();
  const uint32_t plateCount = macroStatistics.plateCount;

  std::vector<Plate> plates(plateCount);
  for (uint32_t plate = 0u; plate < plateCount; ++plate)
  {
    const uint64_t seed = settings.seed ^ (0x2f39a15d31b53f75ull + plate * 0x9e3779b97f4a7c15ull);
    const bool continental = hashUnit(seed ^ 0x94d049bb133111ebull) > oceanicFraction;
    const Vec3 axis = normalize(Vec3{hashUnit(seed ^ 0x6a09e667f3bcc909ull) * 2.0 - 1.0, hashUnit(seed ^ 0xbb67ae8584caa73bull) * 2.0 - 1.0, hashUnit(seed ^ 0xa4093822299f31d0ull) * 2.0 - 1.0});
    const double speed = 0.35 + hashUnit(seed ^ 0x13198a2e03707344ull) * 1.15;
    plates[plate] = Plate{
      cellCount,
      continental,
      continental ? 0.30 + hashUnit(seed ^ 0x243f6a8885a308d3ull) * 0.42 : -0.62 + hashUnit(seed ^ 0x510e527fade682d1ull) * 0.20,
      0.18 + hashUnit(seed ^ 0x9b05688c2b3e6c1full) * 0.70,
      multiply(axis, speed)};
  }

  std::vector<Cell> cells(cellCount);
  for (uint32_t cell = 0u; cell < cellCount; ++cell)
  {
    const uint32_t x = cell % width;
    const uint32_t y = cell / width;
    const double longitude01 = (static_cast<double>(x) + 0.5) / width;
    const double latitude01 = (static_cast<double>(y) + 0.5) / height;
    const uint32_t plate = std::min(sampleMacroPlate(macroMaps, longitude01, latitude01), plateCount - 1u);
    cells[cell].plate = plate;
    if (plates[plate].seedCell >= cellCount)
      plates[plate].seedCell = cell;
  }
  for (uint32_t plate = 0u; plate < plateCount; ++plate)
    if (plates[plate].seedCell >= cellCount)
      plates[plate].seedCell = static_cast<uint32_t>(splitmix64(settings.seed ^ (0x8f3f73b5cf1c9adeull + plate * 0x9e3779b97f4a7c15ull)) % cellCount);

  maps = {};
  maps.width = width;
  maps.height = height;
  maps.heightmap.assign(cellCount, 0u);
  maps.oceanFloorHeightmap.assign(cellCount, 0u);
  maps.waterHeightmap.assign(cellCount, 0u);
  maps.oceanMask.assign(cellCount, 0u);
  maps.lakeMask.assign(cellCount, 0u);
  maps.sourceRegions.assign(cellCount, 0u);
  maps.plates.assign(cellCount, 0u);
  maps.continents.assign(cellCount, 0u);
  maps.tectonics.assign(cellCount, 0u);
  maps.mountains.assign(cellCount, 0u);
  maps.rivers.assign(cellCount, 0u);
  maps.temperature.assign(cellCount, 0u);
  maps.precipitation.assign(cellCount, 0u);
  maps.wind.assign(cellCount, 0u);
  maps.windEast.assign(cellCount, 127u);
  maps.windNorth.assign(cellCount, 127u);
  maps.plateMotionEast.assign(cellCount, 127u);
  maps.plateMotionNorth.assign(cellCount, 127u);
  maps.materials.assign(cellCount, 0u);
  maps.rockSuite.assign(cellCount, static_cast<uint8_t>(PlanetRockSuite::Platform));
  maps.bedPhase.assign(cellCount, 0u);
  maps.lithology.assign(cellCount, static_cast<uint8_t>(PlanetLithology::Sedimentary));

  SphericalTerrainStatistics stats{};
  stats.plateCount = plateCount;
  stats.lakePixelCount = macroStatistics.lakePixelCount;
  for (const Plate &plate : plates)
    if (plate.continental)
      ++stats.continentalPlateCount;
    else
      ++stats.oceanicPlateCount;

  parallelRows(
      height,
      [&](const uint32_t y)
      {
        for (uint32_t x = 0u; x < width; ++x)
        {
          const uint32_t cell = cellIndex(x, y, width);
          const Plate &plate = plates[cells[cell].plate];
          const double longitude01 = (static_cast<double>(x) + 0.5) / width;
          const double latitude01 = (static_cast<double>(y) + 0.5) / height;
          const Vec3 terrainPoint = spherePoint(x, y, width, height);
          Vec3 east = cross(Vec3{0.0, 1.0, 0.0}, terrainPoint);
          if (dot(east, east) < 1.0e-12)
            east = Vec3{1.0, 0.0, 0.0};
          else
            east = normalize(east);
          const Vec3 north = normalize(cross(terrainPoint, east));
          const double warpAmplitude = 0.12 * terrainWarp;
          const double warpEast = sphereFbm(terrainPoint, settings.seed ^ 0x6a09e667f3bcc909ull, 6.0, 4u) * warpAmplitude;
          const double warpNorth = sphereFbm(terrainPoint, settings.seed ^ 0xbb67ae8584caa73bull, 6.0, 4u) * warpAmplitude;
          const Vec3 warpedTerrainPoint = normalize(add(terrainPoint, add(multiply(east, warpEast), multiply(north, warpNorth))));
          const double warpedLongitude01 = std::fmod(std::atan2(warpedTerrainPoint.z, warpedTerrainPoint.x) / (2.0 * kPi) + 1.5, 1.0);
          const double warpedLatitude01 = 0.5 - std::asin(std::clamp(warpedTerrainPoint.y, -1.0, 1.0)) / kPi;
          // Warp the complete structural field, not merely the detail noise.
          // This bends coastlines, trenches, and mountain belts coherently.
          const double macroHeight = sampleMacroU16(macroMaps.heightmap, macroMaps.width, macroMaps.height, warpedLongitude01, warpedLatitude01);
          const double macroContinent = sampleMacroU8(macroMaps.continents, macroMaps.width, macroMaps.height, warpedLongitude01, warpedLatitude01);
          const double macroTectonics = sampleMacroU8(macroMaps.tectonics, macroMaps.width, macroMaps.height, warpedLongitude01, warpedLatitude01);
          const uint32_t macroRegion = sampleMacroRegion(macroMaps, warpedLongitude01, warpedLatitude01);
          const double macroTemperature = sampleMacroU8(macroMaps.temperature, macroMaps.width, macroMaps.height, longitude01, latitude01);
          const double macroPrecipitation = sampleMacroU8(macroMaps.precipitation, macroMaps.width, macroMaps.height, longitude01, latitude01);
          cells[cell].uplift = macroTectonics;
          maps.sourceRegions[cell] = macroRegion;
          // The graph chooses the continental mass, but the raster owns the coast.
          // Perturbing the threshold (instead of the final height) adds capes, bays,
          // and island chains without changing which tectonic plates are continental.
          const double coastDetail = sphereFbm(warpedTerrainPoint, settings.seed ^ 0x71d67fffeda60031ull, 1.7, 6u) * 0.22 + sphereFbm(warpedTerrainPoint, settings.seed ^ 0xff51afd7ed558ccdull, 7.5, 4u) * 0.070;
          const double coastBlend = smoothstep((macroContinent + coastDetail * std::clamp(settings.shorelineDetail, 0.0, 3.0) - 0.47) / 0.11);
          const double graphLandElevation = std::max(0.0, (macroHeight - 0.06) / 0.94);
          const double graphOceanElevation = -0.035 - std::max(0.0, 0.52 - macroContinent) * 0.38;
          const double graphElevation = graphOceanElevation + (graphLandElevation - graphOceanElevation) * coastBlend;
          const double rollingRelief = sphereFbm(warpedTerrainPoint, settings.seed ^ 0x5be0cd19137e2179ull, 3.4, 6u);
          const double fineRelief = sphereFbm(warpedTerrainPoint, settings.seed ^ 0x4cf5ad432745937full, 18.0, 5u);
          const double crustHardness = std::clamp(0.72 + sphereFbm(terrainPoint, settings.seed ^ 0xd1b54a32d192ed03ull, 2.1, 4u) * 0.35, 0.45, 1.10);
          const double landDetailWeight = smoothstep(std::clamp((coastBlend - 0.05) / 0.80, 0.0, 1.0));
          const double dryTerrainMask =
              smoothstep((0.28 - macroPrecipitation) / 0.17) *
              smoothstep((macroTemperature - 0.32) / 0.30) *
              (1.0 - smoothstep((macroTectonics - 0.28) / 0.45));
          const double rasterDunePhase =
              2.0 * kPi *
                  (warpedTerrainPoint.x * 0.79 +
                   warpedTerrainPoint.z * 0.61) *
                  82.0 +
              sphereFbm(warpedTerrainPoint,
                  settings.seed ^ 0x3bd39e10cb0ef593ull, 9.0, 3u) *
                  1.4;
          const double rasterDunes =
              (std::sin(rasterDunePhase) +
                  0.25 * std::sin(rasterDunePhase * 2.0)) *
              dryTerrainMask * 0.0045;
          // Macro elevation already contains simulated plate uplift/subsidence.
          // These small seamless perturbations only model heterogeneous crust;
          // there is no independent mountain generator in the raster stage.
          double elevation = graphElevation;
          const double rasterRelief = std::clamp(settings.interiorRelief, 0.0, 3.0);
          elevation += landDetailWeight * crustHardness *
              (rollingRelief *
                      (0.025 + roughness * 0.090 + macroTectonics * 0.022) +
                  fineRelief * (0.006 + roughness * 0.032) *
                      (1.0 - dryTerrainMask * 0.86)) *
              (0.45 + rasterRelief * 0.55);
          elevation += landDetailWeight * rasterDunes;
          // Bathymetry is derived from the same tectonic field as the visible
          // continents. Broad abyssal undulation and deeper active-boundary
          // trenches add useful underwater relief, but fade to zero at the
          // shoreline so the solid surface remains continuous.
          const double oceanDetailWeight = smoothstep(
              (-graphElevation - 0.004) / 0.085);
          const double abyssalRelief = sphereFbm(warpedTerrainPoint,
              settings.seed ^ 0x94d049bb133111ebull, 5.1, 5u);
          const double trenchShape = smoothstep(
              (macroTectonics - 0.24) / 0.55);
          elevation += oceanDetailWeight *
              (abyssalRelief * 0.020 + fineRelief * 0.004);
          elevation -= oceanDetailWeight * trenchShape *
              (0.012 + std::max(0.0, -abyssalRelief) * 0.026);
          cells[cell].elevation = elevation;
          const bool land = elevation > 0.0;
          const double latitude = std::abs(latitude01 * 2.0 - 1.0);
          const double normalizedHeight = encodedLandElevation(elevation);
          const double localTemperature = std::clamp(1.0 - std::pow(latitude, 1.55) - normalizedHeight * 0.42 + (planetNoise(longitude01, latitude01, settings.seed ^ 0x1f83d9abfb41bd6bull, 3u) - 0.5) * 0.10, 0.0, 1.0);
          const double temperature = std::clamp(macroTemperature * 0.62 + localTemperature * 0.38, 0.0, 1.0);
          const double wetLatitude = 0.25 + 0.42 * std::exp(-std::pow(latitude / 0.30, 2.0)) + 0.16 * std::exp(-std::pow((latitude - 0.58) / 0.18, 2.0));
          const double localPrecipitation =
              std::clamp((land ? plate.moisture * 0.38 : 0.80) + wetLatitude + cells[cell].uplift * 0.11 + (planetNoise(longitude01, latitude01, settings.seed ^ 0xcbbb9d5dc1059ed8ull, 4u) - 0.5) * 0.18, 0.0, 1.0);
          const double precipitation = std::clamp(macroPrecipitation * 0.64 + localPrecipitation * 0.36, 0.0, 1.0);
          cells[cell].temperature = temperature;
          cells[cell].precipitation = precipitation;
        }
      });
  stats.landPixelCount = static_cast<uint32_t>(std::count_if(
      cells.begin(),
      cells.end(),
      [](const Cell &cell)
      {
        return cell.elevation > 0.0;
      }));
  stats.oceanPixelCount = cellCount - stats.landPixelCount;
  if (profile != nullptr)
    profile->rasterStructureMs = elapsedMilliseconds(stageStart);
  stageStart = ProfileClock::now();

  const std::vector<uint16_t> coastDistance = distanceFromOcean(cells, width, height);
  const uint16_t coastBand = static_cast<uint16_t>(std::clamp(height / 24u, 10u, 36u));
  if (profile != nullptr)
    profile->rasterCoastMs = elapsedMilliseconds(stageStart);
  stageStart = ProfileClock::now();

  // Classify visible mountain terrain only after the elevation field is
  // complete. This mirrors Red Blob's derivative-based rendering: tectonics
  // says where ranges may form, while actual slope and ridge curvature decide
  // which pixels read as mountains.
  parallelRows(
      height,
      [&](const uint32_t y)
      {
        for (uint32_t x = 0u; x < width; ++x)
        {
          const uint32_t cell = cellIndex(x, y, width);
          if (cells[cell].elevation <= 0.0)
          {
            cells[cell].mountain = 0.0;
            continue;
          }
          double neighbourMean = 0.0;
          double maximumSlope = 0.0;
          size_t landNeighbourCount = 0u;
          bool touchesOcean = false;
          for (const uint32_t neighbour : sphericalRasterNeighbours8(cell, width, height))
          {
            if (cells[neighbour].elevation <= 0.0)
            {
              touchesOcean = true;
              continue;
            }
            neighbourMean += cells[neighbour].elevation;
            maximumSlope = std::max(maximumSlope, std::abs(cells[cell].elevation - cells[neighbour].elevation));
            ++landNeighbourCount;
          }
          neighbourMean /= std::max<size_t>(1u, landNeighbourCount);
          const double ridgeCurvature = std::max(0.0, cells[cell].elevation - neighbourMean);
          const double highland = smoothstep((cells[cell].elevation - 0.24) / 0.42);
          const double tectonicWeight = smoothstep((cells[cell].uplift - 0.035) / 0.36);
          const double erodedShape = maximumSlope * 4.2 + ridgeCurvature * 12.0;
          double mountain = std::clamp(erodedShape * (0.12 + tectonicWeight * 0.88) + highland * (0.16 + tectonicWeight * 0.16), 0.0, 1.0);
          // A sea/land discontinuity is a coast, not a mountain range. Keeping
          // it out of the slope classifier prevents plate-shaped white rims.
          if (touchesOcean)
            mountain *= 0.28;
          const double coastInterior = smoothstep((static_cast<double>(coastDistance[cell]) - 1.0) / std::max(1.0, static_cast<double>(coastBand) - 1.0));
          mountain *= coastInterior;
          cells[cell].mountain = mountain;
        }
      });
  stats.mountainPixelCount = static_cast<uint32_t>(std::count_if(
      cells.begin(),
      cells.end(),
      [](const Cell &cell)
      {
        return cell.mountain > 0.48;
      }));
  if (profile != nullptr)
    profile->rasterMountainMs = elapsedMilliseconds(stageStart);
  stageStart = ProfileClock::now();

  simulateWindAndRainShadows(cells, width, height);
  for (uint32_t cell = 0u; cell < cellCount; ++cell)
  {
    const Plate &plate = plates[cells[cell].plate];
    const uint32_t x = cell % width;
    const uint32_t y = cell / width;
    const double longitude01 = (static_cast<double>(x) + 0.5) / width;
    const double latitude01 = (static_cast<double>(y) + 0.5) / height;
    const double macroContinent = sampleMacroU8(macroMaps.continents, macroMaps.width, macroMaps.height, longitude01, latitude01);
    const double macroTectonics = sampleMacroU8(macroMaps.tectonics, macroMaps.width, macroMaps.height, longitude01, latitude01);
    const bool land = cells[cell].elevation > 0.0;
    if (land)
    {
      const double latitude = std::abs(latitude01 * 2.0 - 1.0);
      const double angularCoastDistance = static_cast<double>(coastDistance[cell]) * kPi / std::max(1u, height);
      const double continentality = smoothstep((angularCoastDistance - 0.035) / 0.18);
      const double subtropical = std::exp(-std::pow((latitude - 0.32) / 0.15, 2.0));
      const Vec3 climatePoint = spherePoint(x, y, width, height);
      const double dryProvince = std::clamp(0.5 + sphereFbm(climatePoint, settings.seed ^ 0x106aa07032bbd1b8ull, 1.9, 4u), 0.0, 1.0);
      const double rainShadow = 1.0 - smoothstep(cells[cell].windExposure * 1.45);
      const double drying = std::clamp(settings.desertStrength, 0.0, 3.0) * continentality * (0.08 + subtropical * 0.42) * (0.58 + dryProvince * 0.32 + rainShadow * 0.18);
      cells[cell].precipitation *= std::clamp(1.0 - drying, 0.16, 1.0);
    }
    const double normalizedHeight = encodedLandElevation(cells[cell].elevation);
    const double normalizedOceanFloorDepth =
        encodedOceanFloorDepth(cells[cell].elevation);
    const double tectonics = macroTectonics;
    const double localContinent = land ? std::clamp(0.54 + cells[cell].elevation * 0.36 + (plate.continental ? 0.10 : -0.04), 0.0, 1.0) : std::clamp(0.42 + cells[cell].elevation * 0.30, 0.0, 0.50);
    const double continent = std::clamp(macroContinent * 0.74 + localContinent * 0.26, 0.0, 1.0);
    maps.heightmap[cell] = static_cast<uint16_t>(std::lround(normalizedHeight * 65535.0));
    maps.oceanFloorHeightmap[cell] = land ? 0u :
        static_cast<uint16_t>(std::lround(
            normalizedOceanFloorDepth * 65535.0));
    maps.plates[cell] = static_cast<uint8_t>(cells[cell].plate % 255u);
    maps.continents[cell] = quantizeUnit(continent);
    maps.tectonics[cell] = quantizeUnit(tectonics);
    maps.mountains[cell] = quantizeUnit(cells[cell].mountain);
    maps.temperature[cell] = quantizeUnit(cells[cell].temperature);
    maps.precipitation[cell] = quantizeUnit(cells[cell].precipitation);
    maps.wind[cell] = quantizeUnit(cells[cell].windExposure);
    maps.windEast[cell] = quantizeSignedUnit(cells[cell].windEast);
    maps.windNorth[cell] = quantizeSignedUnit(cells[cell].windNorth);
    const Vec3 position = spherePoint(x, y, width, height);
    // Geology, generated before erosion because erosion reads it: the whole
    // point of lithology is that k varies with rock type.
    maps.rockSuite[cell] = classifyRockSuite(plate.continental, tectonics, position, settings.seed);
    maps.bedPhase[cell] = static_cast<uint8_t>(std::lround(
        beddingPhaseMetres(position, settings.seed) / kPlanetStratumThicknessMetres * 255.0));
    const double longitude = ((static_cast<double>(x) + 0.5) / width) * 2.0 * kPi - kPi;
    const Vec3 east = normalize(Vec3{-std::sin(longitude), 0.0, std::cos(longitude)});
    const Vec3 north = normalize(cross(east, position));
    const Vec3 velocity = cross(plate.angularVelocity, position);
    maps.plateMotionEast[cell] = quantizeSignedUnit(std::clamp(dot(velocity, east) * 0.72, -1.0, 1.0));
    maps.plateMotionNorth[cell] = quantizeSignedUnit(std::clamp(dot(velocity, north) * 0.72, -1.0, 1.0));
  }

  if (profile != nullptr)
    profile->rasterClimateMs = elapsedMilliseconds(stageStart);
  stageStart = ProfileClock::now();

  maps.regionPositions = macroMaps.regionPositions;
  maps.regionElevations = macroMaps.regionElevations;
  maps.regionRivers = macroMaps.regionRivers;
  maps.regionNodes = std::move(macroMaps.regionNodes);
  maps.regionNeighbourOffsets = std::move(macroMaps.regionNeighbourOffsets);
  maps.regionNeighbourIndices = std::move(macroMaps.regionNeighbourIndices);
  maps.surfacePositions = macroMaps.surfacePositions;
  maps.surfaceElevations = macroMaps.surfaceElevations;
  maps.surfaceContinents = macroMaps.surfaceContinents;
  maps.surfaceTectonics = macroMaps.surfaceTectonics;
  maps.surfaceMountains = macroMaps.surfaceMountains;
  maps.surfaceTemperature = macroMaps.surfaceTemperature;
  maps.surfacePrecipitation = macroMaps.surfacePrecipitation;
  maps.surfacePlates = macroMaps.surfacePlates;
  maps.surfaceRivers = macroMaps.surfaceRivers;
  maps.surfaceMaterials = macroMaps.surfaceMaterials;
  maps.surfaceIndices = macroMaps.surfaceIndices;
  maps.sphereLines = macroMaps.sphereLines;
  // Route once on the structural raster so the river mask material
  // classification consumes exists, let the fluvial solver carve the relief,
  // then route again so the exported river map follows the final terrain
  // rather than the pre-erosion surface.
  //
  // The solver's drainage area is a strictly better discharge proxy than this
  // re-routed mask, so both of these routing calls become redundant once the
  // solver exports its accumulation field.
  stageStart = ProfileClock::now();
  routeRainfallRunoff(width, height, settings.seed, maps.heightmap, maps.precipitation, maps.rivers, stats.riverSourceCount);
  if (profile != nullptr)
    profile->rasterRoutingMs += elapsedMilliseconds(stageStart);
  stageStart = ProfileClock::now();

  if (settings.fluvialSolver != nullptr)
  {
    // maps.oceanMask is not written until material classification, which runs
    // after this point and needs the eroded surface. The height field already
    // carries the distinction the solver needs: it holds solid terrain only, so
    // a zero texel is sea rather than land at sea level.
    std::vector<uint8_t> solverOceanMask(cellCount, 0u);
    for (uint32_t cell = 0u; cell < cellCount; ++cell)
      solverOceanMask[cell] = maps.heightmap[cell] == 0u ? 255u : 0u;
    SphericalTerrainFluvialSolver::Request request{};
    request.width = width;
    request.height = height;
    request.planetRadiusMetres = settings.planetRadiusMetres;
    request.reliefMetres = settings.reliefMetres;
    request.heightmap = &maps.heightmap;
    request.oceanMask = &solverOceanMask;
    request.tectonics = &maps.tectonics;
    request.rockSuite = &maps.rockSuite;
    request.bedPhase = &maps.bedPhase;
    request.exposedLithology = &maps.lithology;
    std::string solverError;
    if (!settings.fluvialSolver->solve(request, &solverError))
      return false;
  }
  if (profile != nullptr)
    profile->rasterErosionMs = elapsedMilliseconds(stageStart);
  stageStart = ProfileClock::now();

  parallelRows(
      height,
      [&](const uint32_t y)
      {
        for (uint32_t x = 0u; x < width; ++x)
        {
          const uint32_t cell = cellIndex(x, y, width);
          cells[cell].elevation = decodedLandElevation(
              static_cast<double>(maps.heightmap[cell]) / 65535.0);
          if (maps.heightmap[cell] == 0u)
          {
            cells[cell].mountain = 0.0;
            maps.mountains[cell] = 0u;
            continue;
          }
          double neighbourMean = 0.0;
          double maximumSlope = 0.0;
          size_t landNeighbourCount = 0u;
          bool touchesOcean = false;
          for (const uint32_t neighbour :
               sphericalRasterNeighbours8(cell, width, height))
          {
            if (maps.heightmap[neighbour] == 0u)
            {
              touchesOcean = true;
              continue;
            }
            const double neighbourElevation = decodedLandElevation(
                static_cast<double>(maps.heightmap[neighbour]) / 65535.0);
            neighbourMean += neighbourElevation;
            maximumSlope = std::max(maximumSlope,
                std::abs(cells[cell].elevation - neighbourElevation));
            ++landNeighbourCount;
          }
          neighbourMean /= std::max<size_t>(1u, landNeighbourCount);
          const double ridgeCurvature =
              std::max(0.0, cells[cell].elevation - neighbourMean);
          const double highland =
              smoothstep((cells[cell].elevation - 0.24) / 0.42);
          const double tectonicWeight =
              smoothstep((cells[cell].uplift - 0.035) / 0.36);
          const double erodedShape =
              maximumSlope * 4.2 + ridgeCurvature * 12.0;
          double mountain = std::clamp(
              erodedShape * (0.12 + tectonicWeight * 0.88) +
                  highland * (0.16 + tectonicWeight * 0.16),
              0.0, 1.0);
          if (touchesOcean)
            mountain *= 0.28;
          const double coastInterior = smoothstep(
              (static_cast<double>(coastDistance[cell]) - 1.0) /
              std::max(1.0, static_cast<double>(coastBand) - 1.0));
          cells[cell].mountain = mountain * coastInterior;
          maps.mountains[cell] = quantizeUnit(cells[cell].mountain);
        }
      });
  stats.mountainPixelCount = static_cast<uint32_t>(std::count_if(
      maps.mountains.begin(), maps.mountains.end(),
      [](const uint8_t value) { return value > 122u; }));
  if (profile != nullptr)
    profile->rasterMountainMs += elapsedMilliseconds(stageStart);
  stageStart = ProfileClock::now();

  routeRainfallRunoff(width, height, settings.seed, maps.heightmap,
      maps.precipitation, maps.rivers, stats.riverSourceCount);
  if (profile != nullptr)
    profile->rasterRoutingMs += elapsedMilliseconds(stageStart);
  stageStart = ProfileClock::now();

  // Percentile tables for the climate fields over land, so biome bands can be
  // placed where this planet's climate actually varies. Absolute thresholds
  // against a normalized index collapsed 55% of land onto two cover classes and
  // left five classes unreachable on every planet.
  std::array<uint32_t, 256> rainHistogram{};
  std::array<uint32_t, 256> temperatureHistogram{};
  uint64_t climateLandCells = 0u;
  for (uint32_t cell = 0u; cell < cellCount; ++cell)
  {
    if (maps.heightmap[cell] == 0u)
      continue;
    ++rainHistogram[maps.precipitation[cell]];
    ++temperatureHistogram[maps.temperature[cell]];
    ++climateLandCells;
  }
  // Steepest neighbour gradient per cell, computed before classification.
  //
  // It cannot be computed inside the classification loop: that loop zeroes
  // maps.heightmap for every cell it decides is ocean, while neighbouring rows
  // are being classified in parallel and would read those heights. The result
  // was a genuine data race whose output changed run to run.
  std::vector<float> surfaceGradient(cellCount, 0.0f);
  {
    const double cellMetres = std::max(
        2.0 * kPi * settings.planetRadiusMetres / std::max(width, 1u), 1.0);
    const double gradientScale = settings.reliefMetres / (cellMetres * 65535.0);
    parallelRows(
        height,
        [&](const uint32_t y)
        {
          for (uint32_t x = 0u; x < width; ++x)
          {
            const uint32_t cell = cellIndex(x, y, width);
            if (maps.heightmap[cell] == 0u)
              continue;
            double steepest = 0.0;
            for (const uint32_t neighbour : sphericalRasterNeighbours8(cell, width, height))
              if (maps.heightmap[neighbour] != 0u)
                steepest = std::max(steepest,
                    std::abs(static_cast<double>(maps.heightmap[cell]) -
                        static_cast<double>(maps.heightmap[neighbour])));
            surfaceGradient[cell] = static_cast<float>(steepest * gradientScale);
          }
        });
  }

  std::array<double, 256> rainRank{};
  std::array<double, 256> temperatureRank{};
  {
    const double total = static_cast<double>(std::max<uint64_t>(climateLandCells, 1u));
    uint64_t rainRunning = 0u;
    uint64_t temperatureRunning = 0u;
    for (uint32_t bucket = 0u; bucket < 256u; ++bucket)
    {
      // Mid-bucket rank, so a value sitting entirely in one bucket does not
      // read as the very bottom or top of the distribution.
      rainRank[bucket] = (rainRunning + rainHistogram[bucket] * 0.5) / total;
      temperatureRank[bucket] = (temperatureRunning + temperatureHistogram[bucket] * 0.5) / total;
      rainRunning += rainHistogram[bucket];
      temperatureRunning += temperatureHistogram[bucket];
    }
  }

  parallelRows(
      height,
      [&](const uint32_t y)
      {
        for (uint32_t x = 0u; x < width; ++x)
        {
          const uint32_t cell = cellIndex(x, y, width);
          const uint32_t sourceRegion = maps.sourceRegions[cell];
          const bool graphLake = sourceRegion < maps.regionNodes.size() && maps.regionNodes[sourceRegion].material == static_cast<uint8_t>(PolygonTerrainMaterial::Lake);
          if (graphLake)
          {
            maps.materials[cell] = static_cast<uint8_t>(PolygonTerrainMaterial::Lake);
            maps.waterHeightmap[cell] =
                maps.regionNodes[sourceRegion].waterElevation;
            maps.heightmap[cell] = std::min(maps.heightmap[cell],
                maps.waterHeightmap[cell]);
            maps.oceanFloorHeightmap[cell] = 0u;
            maps.lakeMask[cell] = 255u;
            continue;
          }
          maps.materials[cell] = classifyGraphMaterial(
              cells[cell], maps.rivers[cell], coastDistance[cell] <= 2u,
              spherePoint(x, y, width, height), settings.seed,
              settings.biomeVariation,
              rainRank[maps.precipitation[cell]],
              temperatureRank[maps.temperature[cell]],
              surfaceGradient[cell]);
          if (maps.materials[cell] ==
              static_cast<uint8_t>(PolygonTerrainMaterial::Ocean))
          {
            // Ocean is a separate zero-elevation water surface. Never let
            // filtered coastal terrain leak into the solid height field.
            maps.heightmap[cell] = 0u;
            maps.oceanMask[cell] = 255u;
          }
          else
          {
            maps.oceanFloorHeightmap[cell] = 0u;
          }
        }
      });
  stats.riverPixelCount = static_cast<uint32_t>(std::count_if(
      maps.rivers.begin(),
      maps.rivers.end(),
      [](const uint8_t value)
      {
        return value > 0u;
      }));
  if (profile != nullptr)
  {
    profile->rasterClassifyMs = elapsedMilliseconds(stageStart);
    profile->totalMs = elapsedMilliseconds(generateStart);
  }

  if (statistics != nullptr)
    *statistics = stats;
  return true;
}

} // namespace procedural
