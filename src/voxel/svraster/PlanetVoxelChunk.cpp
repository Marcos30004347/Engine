#include "voxel/svraster/PlanetVoxelChunk.hpp"
#include "os/hqos.hpp"

#include "voxel/material/VoxelMaterial.hpp"
#include "voxel/material/VoxelMaterialContext.hpp"
#include "voxel/svraster/TerrainGPUColumnBuild.hpp"

#include "os/Logger.hpp"
#include "procedural/PlanetCubeMapping.hpp"
#include "procedural/PolygonTerrain.hpp"
#include "procedural/world/MapExportSampler.hpp"
#include "procedural/world/MapTerrainRefinement.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace voxel::svraster
{
namespace
{

constexpr int32_t kTerrainSlopeContextHalo = 3;
constexpr double kHalfPi = 1.57079632679489661923;

uint32_t terrainSurfaceContextForGradient(const double gradientMagnitude)
{
  const double normalizedSlopeAngle =
      std::atan(std::max(gradientMagnitude, 0.0)) / kHalfPi;
  return normalizedSlopeAngle > 1.0e-6
      ? sparseVoxelMicroTerrainSlopeContext(normalizedSlopeAngle)
      : static_cast<uint32_t>(SparseVoxelMicroContext::FlatSurface);
}

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool cancellationRequested(const std::atomic_bool *cancellation)
{
  return cancellation != nullptr && cancellation->load(std::memory_order_relaxed);
}

class TerrainChunkBuildThreadPool
{
public:
  TerrainChunkBuildThreadPool() = default;

  ~TerrainChunkBuildThreadPool()
  {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    workAvailable_.notify_all();
    for (auto &worker : workers_)
      if (worker.joinable())
        worker.join();
  }

  TerrainChunkBuildThreadPool(const TerrainChunkBuildThreadPool &) = delete;
  TerrainChunkBuildThreadPool &operator=(
      const TerrainChunkBuildThreadPool &) = delete;

  template <typename Function>
  void parallelFor(
      const uint32_t requestedWorkers,
      const uint32_t jobCount,
      Function &&function)
  {
    if (jobCount == 0u)
      return;
    const uint32_t workerCount = std::max(requestedWorkers, 1u);
    ensureWorkers(workerCount);
    auto batch = std::make_shared<Batch>();
    batch->remaining = jobCount;
    batch->function = std::forward<Function>(function);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (uint32_t job = 0u; job < jobCount; ++job)
        pending_.push_back({batch, job});
    }
    workAvailable_.notify_all();

    // The calling thread runs items too instead of parking. That is what makes
    // a nested parallelFor safe: page generation runs on pool threads and
    // parallelises its own column sampling, so a pool whose every thread
    // blocked on a nested batch would deadlock with work still queued. It also
    // stops the submitting thread from idling through the batch it asked for.
    for (;;)
    {
      {
        std::lock_guard<std::mutex> lock(batch->mutex);
        if (batch->remaining == 0u)
          break;
      }
      WorkItem item;
      bool claimed = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_.empty())
        {
          item = std::move(pending_.front());
          pending_.pop_front();
          claimed = true;
        }
      }
      if (claimed)
      {
        runItem(item);
        continue;
      }
      // Nothing left to claim: every remaining item of this batch is in flight
      // on another thread. Waiting on the batch is correct, and the timeout
      // only bounds how long a lost notification could stall this thread.
      std::unique_lock<std::mutex> lock(batch->mutex);
      batch->complete.wait_for(
          lock, std::chrono::milliseconds(2),
          [&] { return batch->remaining == 0u; });
    }
    if (batch->failure)
      std::rethrow_exception(batch->failure);
  }

private:
  struct Batch
  {
    std::mutex mutex;
    std::condition_variable complete;
    std::function<void(uint32_t)> function;
    std::exception_ptr failure;
    uint32_t remaining = 0u;
  };

  struct WorkItem
  {
    std::shared_ptr<Batch> batch;
    uint32_t index = 0u;
  };

  void ensureWorkers(const uint32_t workerCount)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!stopping_ && workers_.size() < workerCount)
      workers_.emplace_back(
          [this]
          {
            os::hqos::setWorkerQos();
            workerMain();
          });
  }

  void workerMain()
  {
    for (;;)
    {
      WorkItem item;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        workAvailable_.wait(
            lock, [&] { return stopping_ || !pending_.empty(); });
        if (stopping_ && pending_.empty())
          return;
        item = std::move(pending_.front());
        pending_.pop_front();
      }

      runItem(item);
    }
  }

  void runItem(WorkItem &item)
  {
    std::exception_ptr failure;
    try
    {
      item.batch->function(item.index);
    }
    catch (...)
    {
      failure = std::current_exception();
    }

    {
      std::lock_guard<std::mutex> lock(item.batch->mutex);
      if (failure && !item.batch->failure)
        item.batch->failure = failure;
      if (item.batch->remaining != 0u)
        --item.batch->remaining;
    }
    item.batch->complete.notify_all();
  }

  std::mutex mutex_;
  std::condition_variable workAvailable_;
  std::deque<WorkItem> pending_;
  std::vector<std::thread> workers_;
  bool stopping_ = false;
};

TerrainChunkBuildThreadPool &terrainChunkBuildThreadPool()
{
  static TerrainChunkBuildThreadPool pool;
  return pool;
}

uint32_t lowestSetBitIndex(const uint32_t mask)
{
  uint32_t index = 0u;
  while (index < 31u && (mask & (uint32_t{1u} << index)) == 0u)
    ++index;
  return index;
}

uint32_t highestSetBitIndex(const uint32_t mask)
{
  uint32_t index = 31u;
  while (index != 0u && (mask & (uint32_t{1u} << index)) == 0u)
    --index;
  return index;
}

bool cancelled(std::string *error)
{
  return fail(error, "Terrain work cancelled");
}

void report(const PlanetVoxelChunkProgress &progress, const char *phase, const double fraction)
{
  if (progress)
    progress(phase, fraction);
}

uint16_t readU16(const std::vector<uint8_t> &bytes, const size_t index)
{
  const size_t offset = index * 2u;
  return static_cast<uint16_t>(bytes[offset]) | static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset + 1u]) << 8u);
}

uint32_t rgba(const uint8_t red, const uint8_t green, const uint8_t blue)
{
  return static_cast<uint32_t>(red) | (static_cast<uint32_t>(green) << 8u) | (static_cast<uint32_t>(blue) << 16u) | 0xff000000u;
}

SparseVoxelMaterialGPU makePlanetVoxelBaseMaterial(const uint8_t material)
{
  using Material = procedural::PolygonTerrainMaterial;
  uint32_t albedo = rgba(112u, 103u, 84u);
  uint8_t roughness = 210u;
  switch (static_cast<Material>(material))
  {
  case Material::Ocean:
    albedo = rgba(20u, 67u, 112u);
    roughness = 90u;
    break;
  case Material::Lake:
    albedo = rgba(33u, 104u, 143u);
    roughness = 80u;
    break;
  case Material::Beach:
    albedo = rgba(205u, 187u, 129u);
    break;
  case Material::Grass:
  case Material::Grassland:
    albedo = rgba(91u, 132u, 63u);
    break;
  case Material::Rock:
  case Material::Bare:
  case Material::Scorched:
    albedo = rgba(104u, 99u, 91u);
    roughness = 225u;
    break;
  case Material::Snow:
  case Material::Ice:
    albedo = rgba(218u, 230u, 233u);
    roughness = 155u;
    break;
  case Material::River:
    albedo = rgba(38u, 103u, 142u);
    roughness = 85u;
    break;
  case Material::Marsh:
    albedo = rgba(75u, 105u, 74u);
    break;
  case Material::Tundra:
    albedo = rgba(130u, 139u, 117u);
    break;
  case Material::Taiga:
    albedo = rgba(48u, 91u, 67u);
    break;
  case Material::Shrubland:
    albedo = rgba(116u, 126u, 75u);
    break;
  case Material::TemperateDesert:
    albedo = rgba(177u, 151u, 104u);
    break;
  case Material::SubtropicalDesert:
    albedo = rgba(202u, 169u, 102u);
    break;
  case Material::TemperateRainForest:
  case Material::TropicalRainForest:
    albedo = rgba(35u, 105u, 58u);
    break;
  case Material::TemperateDeciduousForest:
  case Material::TropicalSeasonalForest:
    albedo = rgba(50u, 119u, 57u);
    break;
  }
  SparseVoxelMaterialGPU result{};
  result.albedoRGBA8 = albedo;
  result.roughnessMetallicUNorm8 = roughness;
  result.materialId = material;
  return result;
}

double bilinearHeight(const std::vector<uint8_t> &height, const uint32_t storedSide, const uint16_t gutter, const uint32_t interiorSide, const double u, const double v, const double scale, const double bias)
{
  const double x = std::clamp(static_cast<double>(gutter) + u * interiorSide - 0.5, 0.0, static_cast<double>(storedSide - 1u));
  const double y = std::clamp(static_cast<double>(gutter) + v * interiorSide - 0.5, 0.0, static_cast<double>(storedSide - 1u));
  const uint32_t x0 = static_cast<uint32_t>(std::floor(x));
  const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
  const uint32_t x1 = std::min(x0 + 1u, storedSide - 1u);
  const uint32_t y1 = std::min(y0 + 1u, storedSide - 1u);
  const double tx = x - x0;
  const double ty = y - y0;
  const double top = readU16(height, static_cast<size_t>(y0) * storedSide + x0) * (1.0 - tx) + readU16(height, static_cast<size_t>(y0) * storedSide + x1) * tx;
  const double bottom = readU16(height, static_cast<size_t>(y1) * storedSide + x0) * (1.0 - tx) + readU16(height, static_cast<size_t>(y1) * storedSide + x1) * tx;
  return bias + (top * (1.0 - ty) + bottom * ty) * scale;
}

uint8_t nearestByte(const std::vector<uint8_t> &values, const uint32_t storedSide, const uint16_t gutter, const uint32_t interiorSide, const double u, const double v)
{
  const uint32_t x = std::min(gutter + static_cast<uint32_t>(std::floor(std::clamp(u, 0.0, 1.0) * interiorSide)), static_cast<uint32_t>(gutter) + interiorSide - 1u);
  const uint32_t y = std::min(gutter + static_cast<uint32_t>(std::floor(std::clamp(v, 0.0, 1.0) * interiorSide)), static_cast<uint32_t>(gutter) + interiorSide - 1u);
  return values[static_cast<size_t>(y) * storedSide + x];
}

double bilinearByte(const std::vector<uint8_t> &values, const uint32_t storedSide, const uint16_t gutter, const uint32_t interiorSide, const double u, const double v)
{
  const double x = std::clamp(static_cast<double>(gutter) + u * interiorSide - 0.5, 0.0, static_cast<double>(storedSide - 1u));
  const double y = std::clamp(static_cast<double>(gutter) + v * interiorSide - 0.5, 0.0, static_cast<double>(storedSide - 1u));
  const uint32_t x0 = static_cast<uint32_t>(std::floor(x));
  const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
  const uint32_t x1 = std::min(x0 + 1u, storedSide - 1u);
  const uint32_t y1 = std::min(y0 + 1u, storedSide - 1u);
  const double tx = x - x0;
  const double ty = y - y0;
  const double top = values[static_cast<size_t>(y0) * storedSide + x0] * (1.0 - tx) + values[static_cast<size_t>(y0) * storedSide + x1] * tx;
  const double bottom = values[static_cast<size_t>(y1) * storedSide + x0] * (1.0 - tx) + values[static_cast<size_t>(y1) * storedSide + x1] * tx;
  return (top * (1.0 - ty) + bottom * ty) / 255.0;
}

uint64_t mixNoise(uint64_t value)
{
  value ^= value >> 30u;
  value *= 0xbf58476d1ce4e5b9ull;
  value ^= value >> 27u;
  value *= 0x94d049bb133111ebull;
  return value ^ (value >> 31u);
}

double latticeNoise(const int64_t x, const int64_t y, const int64_t z, const uint64_t seed)
{
  uint64_t value = mixNoise(seed ^ static_cast<uint64_t>(x));
  value = mixNoise(value ^ (static_cast<uint64_t>(y) + 0x9e3779b97f4a7c15ull));
  value = mixNoise(value ^ (static_cast<uint64_t>(z) + 0xd1b54a32d192ed03ull));
  return static_cast<double>(value >> 11u) * (1.0 / 9007199254740992.0) * 2.0 - 1.0;
}

double smoothNoise3D(const procedural::SpherePoint &direction, const double planetRadiusMetres, const double wavelengthMetres, const uint64_t seed)
{
  const std::array<double, 3> position{direction.x * planetRadiusMetres / wavelengthMetres, direction.y * planetRadiusMetres / wavelengthMetres, direction.z * planetRadiusMetres / wavelengthMetres};
  std::array<int64_t, 3> base{};
  std::array<double, 3> blend{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    base[axis] = static_cast<int64_t>(std::floor(position[axis]));
    const double fraction = position[axis] - static_cast<double>(base[axis]);
    blend[axis] = fraction * fraction * (3.0 - 2.0 * fraction);
  }
  double result = 0.0;
  for (uint32_t corner = 0u; corner < 8u; ++corner)
  {
    const uint32_t ox = corner & 1u;
    const uint32_t oy = (corner >> 1u) & 1u;
    const uint32_t oz = (corner >> 2u) & 1u;
    const double weightX = ox != 0u ? blend[0] : 1.0 - blend[0];
    const double weightY = oy != 0u ? blend[1] : 1.0 - blend[1];
    const double weightZ = oz != 0u ? blend[2] : 1.0 - blend[2];
    result += latticeNoise(base[0] + ox, base[1] + oy, base[2] + oz, seed) * weightX * weightY * weightZ;
  }
  return result;
}

double smoothNoise2D(
    const procedural::Point2 &position,
    const double wavelengthMetres,
    const uint64_t seed)
{
  const double px = position.x / wavelengthMetres;
  const double py = position.y / wavelengthMetres;
  const int64_t x0 = static_cast<int64_t>(std::floor(px));
  const int64_t y0 = static_cast<int64_t>(std::floor(py));
  const double fractionX = px - static_cast<double>(x0);
  const double fractionY = py - static_cast<double>(y0);
  const double blendX = fractionX * fractionX * (3.0 - 2.0 * fractionX);
  const double blendY = fractionY * fractionY * (3.0 - 2.0 * fractionY);
  const double top =
      latticeNoise(x0, y0, 0, seed) * (1.0 - blendX) +
      latticeNoise(x0 + 1, y0, 0, seed) * blendX;
  const double bottom =
      latticeNoise(x0, y0 + 1, 0, seed) * (1.0 - blendX) +
      latticeNoise(x0 + 1, y0 + 1, 0, seed) * blendX;
  return top * (1.0 - blendY) + bottom * blendY;
}

// Map exports intentionally remain compact, so their elevation texels can be
// kilometres apart. Reconstruct deterministic terrain below that sampling
// scale in global map coordinates; evaluating this at canonical page corners
// keeps adjacent pages and spatial LODs continuous.
uint8_t colourChannel(const uint32_t colour, const uint32_t shift)
{
  return static_cast<uint8_t>((colour >> shift) & 0xffu);
}

uint8_t blendChannel(const uint8_t first, const uint8_t second, const double amount)
{
  return static_cast<uint8_t>(std::lround(static_cast<double>(first) + (static_cast<double>(second) - first) * std::clamp(amount, 0.0, 1.0)));
}

// The terrain package stores a compact biome ID, not a colour texture. Build a
// small deterministic atlas from that ID at voxelization time: broad patches
// vary the vegetation tone, fine patches break up repetition, and exposed
// slopes reveal soil. Values are quantized deliberately so a page retains a
// compact exact palette instead of manufacturing a unique material per voxel.
SparseVoxelMaterialGPU makePlanetVoxelSurfaceMaterial(const uint8_t material, const procedural::SpherePoint &direction, const procedural::PlanetPackageMetadata &metadata, const double slope, const double mountain)
{
  using Material = procedural::PolygonTerrainMaterial;
  SparseVoxelMaterialGPU result = makePlanetVoxelBaseMaterial(material);
  const uint64_t seed = metadata.contentHash ^ 0x510e527fade682d1ull;
  const double regional = smoothNoise3D(direction, metadata.planetRadiusMetres, 1800.0, seed);
  const double broad = smoothNoise3D(direction, metadata.planetRadiusMetres, 240.0, mixNoise(seed + 0x243f6a8885a308d3ull));
  const double fine = smoothNoise3D(direction, metadata.planetRadiusMetres, 43.0, mixNoise(seed + 0x9e3779b97f4a7c15ull));
  const double rawTone = std::clamp(0.5 + regional * 0.20 + broad * 0.25 + fine * 0.16, 0.0, 1.0);
  const double tone = std::round(rawTone * 11.0) / 11.0;
  const double rawHue = std::clamp(0.5 + regional * 0.29 - broad * 0.24 + fine * 0.08, 0.0, 1.0);
  const double hue = std::round(rawHue * 9.0) / 9.0;

  const auto biome = static_cast<Material>(material);
  const bool grass = biome == Material::Grass || biome == Material::Grassland;
  const bool forest =
      biome == Material::Taiga || biome == Material::TemperateRainForest || biome == Material::TemperateDeciduousForest || biome == Material::TropicalRainForest || biome == Material::TropicalSeasonalForest;
  const bool scrub = biome == Material::Marsh || biome == Material::Shrubland || biome == Material::Tundra;
  const bool vegetation = grass || forest || scrub;
  const bool desert = biome == Material::TemperateDesert || biome == Material::SubtropicalDesert;
  const bool geological = biome == Material::Rock || biome == Material::Bare || biome == Material::Scorched;
  const bool beach = biome == Material::Beach;
  const bool snow = biome == Material::Snow;
  const bool ice = biome == Material::Ice;
  const bool water = biome == Material::Ocean || biome == Material::Lake || biome == Material::River;

  uint8_t red = colourChannel(result.albedoRGBA8, 0u);
  uint8_t green = colourChannel(result.albedoRGBA8, 8u);
  uint8_t blue = colourChannel(result.albedoRGBA8, 16u);
  if (vegetation)
  {
    // Keep every vegetation biome in the same green/brown family while
    // retaining a recognizable climate signature. This is also the palette
    // used by the distant surface atlas, so a forest does not turn into a
    // generic grass patch when the voxel representation hands off.
    std::array<uint8_t, 3> cool{48u, 105u, 42u};
    std::array<uint8_t, 3> warm{119u, 147u, 58u};
    if (biome == Material::Taiga)
    {
      cool = {31u, 72u, 58u};
      warm = {74u, 108u, 68u};
    }
    else if (biome == Material::TropicalRainForest)
    {
      cool = {18u, 69u, 35u};
      warm = {54u, 125u, 53u};
    }
    else if (biome == Material::TemperateRainForest)
    {
      cool = {27u, 78u, 47u};
      warm = {67u, 124u, 60u};
    }
    else if (biome == Material::TemperateDeciduousForest)
    {
      cool = {45u, 83u, 43u};
      warm = {112u, 137u, 58u};
    }
    else if (biome == Material::TropicalSeasonalForest)
    {
      cool = {42u, 88u, 39u};
      warm = {136u, 137u, 52u};
    }
    else if (biome == Material::Marsh)
    {
      cool = {48u, 78u, 55u};
      warm = {104u, 119u, 62u};
    }
    else if (biome == Material::Tundra)
    {
      cool = {91u, 106u, 82u};
      warm = {157u, 149u, 96u};
    }
    else if (biome == Material::Shrubland)
    {
      cool = {72u, 96u, 49u};
      warm = {148u, 134u, 67u};
    }
    red = blendChannel(cool[0], warm[0], hue);
    green = blendChannel(cool[1], warm[1], hue);
    blue = blendChannel(cool[2], warm[2], hue);
    const double brightness = 0.82 + tone * 0.34;
    red = static_cast<uint8_t>(std::clamp(std::lround(red * brightness), 0l, 255l));
    green = static_cast<uint8_t>(std::clamp(std::lround(green * brightness), 0l, 255l));
    blue = static_cast<uint8_t>(std::clamp(std::lround(blue * brightness), 0l, 255l));

    const double patch = std::clamp((fine - 0.34) * 1.35 + (broad - 0.58) * 0.55, 0.0, 1.0);
    const double exposed = std::clamp((slope - 0.34) * 0.82 + patch * 0.72 + mountain * 0.12, 0.0, 1.0);
    const double soil = std::round(exposed * 4.0) / 4.0;
    std::array<uint8_t, 3> dirt = hue < 0.5 ? std::array<uint8_t, 3>{86u, 64u, 43u} : std::array<uint8_t, 3>{119u, 87u, 52u};
    if (forest)
      dirt = hue < 0.5 ? std::array<uint8_t, 3>{61u, 50u, 36u} : std::array<uint8_t, 3>{96u, 69u, 42u};
    else if (biome == Material::Marsh)
      dirt = {59u, 57u, 39u};
    else if (biome == Material::Tundra)
      dirt = {102u, 91u, 70u};
    // Keep vegetation green as the authored top layer. The existing material
    // words carry an opaque dirt bottom layer, and the compact brick palette
    // turns that into an eight-bit secondary index. Only upward/downward
    // (Y-normal) faces expose the green layer and blend toward dirt in recessed
    // micro cells. Side faces resolve directly to dirt.
    result.reserved0 = rgba(dirt[0], dirt[1], dirt[2]);
    result.reserved1 = kSparseVoxelMaterialBlendTopBottomFaces;
    result.roughnessMetallicUNorm8 = static_cast<uint32_t>(205u + static_cast<uint32_t>(soil * 25.0));
  }
  else if (desert)
  {
    // Sand changes in broad depositional provinces rather than noisy speckles.
    // The two climates retain distinct palettes and dune faces get just enough
    // value separation to make their slopes readable from orbit and on foot.
    const bool hot = biome == Material::SubtropicalDesert;
    const std::array<uint8_t, 3> cool = hot ? std::array<uint8_t, 3>{174u, 132u, 78u} : std::array<uint8_t, 3>{142u, 126u, 94u};
    const std::array<uint8_t, 3> warm = hot ? std::array<uint8_t, 3>{226u, 190u, 116u} : std::array<uint8_t, 3>{204u, 179u, 120u};
    const double sandTone = std::round(std::clamp(0.5 + regional * 0.28 + broad * 0.34, 0.0, 1.0) * 8.0) / 8.0;
    red = blendChannel(cool[0], warm[0], sandTone);
    green = blendChannel(cool[1], warm[1], sandTone);
    blue = blendChannel(cool[2], warm[2], sandTone);
    result.roughnessMetallicUNorm8 = 232u;
  }
  else if (geological)
  {
    const std::array<uint8_t, 3> cool = biome == Material::Scorched ? std::array<uint8_t, 3>{48u, 43u, 42u} : biome == Material::Bare ? std::array<uint8_t, 3>{83u, 76u, 66u} : std::array<uint8_t, 3>{70u, 76u, 79u};
    const std::array<uint8_t, 3> warm = biome == Material::Scorched ? std::array<uint8_t, 3>{103u, 70u, 54u} : biome == Material::Bare ? std::array<uint8_t, 3>{146u, 119u, 87u} : std::array<uint8_t, 3>{137u, 111u, 86u};
    const double strata = std::round(std::clamp(0.5 + regional * 0.31 - broad * 0.17 + mountain * 0.08, 0.0, 1.0) * 7.0) / 7.0;
    red = blendChannel(cool[0], warm[0], strata);
    green = blendChannel(cool[1], warm[1], strata);
    blue = blendChannel(cool[2], warm[2], strata);
    const double brightness = 0.86 + tone * 0.24;
    red = static_cast<uint8_t>(std::clamp(std::lround(red * brightness), 0l, 255l));
    green = static_cast<uint8_t>(std::clamp(std::lround(green * brightness), 0l, 255l));
    blue = static_cast<uint8_t>(std::clamp(std::lround(blue * brightness), 0l, 255l));
    result.roughnessMetallicUNorm8 = static_cast<uint32_t>(std::clamp(std::lround(218.0 + (1.0 - tone) * 24.0), 0l, 255l));
  }
  else if (beach)
  {
    const double shellTone = std::round(std::clamp(0.5 + regional * 0.22 + broad * 0.34 + fine * 0.08, 0.0, 1.0) * 8.0) / 8.0;
    red = blendChannel(171u, 224u, shellTone);
    green = blendChannel(151u, 207u, shellTone);
    blue = blendChannel(105u, 151u, shellTone);
    const std::array<uint8_t, 3> wetSand{111u, 94u, 68u};
    result.reserved0 = rgba(wetSand[0], wetSand[1], wetSand[2]);
    result.reserved1 = kSparseVoxelMaterialBlendTopBottomFaces;
    result.roughnessMetallicUNorm8 = 224u;
  }
  else if (snow)
  {
    const double snowTone = std::round(std::clamp(0.64 + regional * 0.12 + broad * 0.15 + fine * 0.05, 0.0, 1.0) * 7.0) / 7.0;
    red = blendChannel(178u, 235u, snowTone);
    green = blendChannel(192u, 241u, snowTone);
    blue = blendChannel(202u, 244u, snowTone);
    const std::array<uint8_t, 3> exposedRock = hue < 0.5 ? std::array<uint8_t, 3>{91u, 96u, 98u} : std::array<uint8_t, 3>{124u, 112u, 101u};
    result.reserved0 = rgba(exposedRock[0], exposedRock[1], exposedRock[2]);
    result.reserved1 = kSparseVoxelMaterialBlendTopBottomFaces;
    result.roughnessMetallicUNorm8 = 174u;
  }
  else if (ice)
  {
    const double iceTone = std::round(std::clamp(0.56 + regional * 0.17 + broad * 0.13, 0.0, 1.0) * 6.0) / 6.0;
    red = blendChannel(162u, 219u, iceTone);
    green = blendChannel(195u, 235u, iceTone);
    blue = blendChannel(211u, 243u, iceTone);
    result.roughnessMetallicUNorm8 = 132u;
  }
  else if (water)
  {
    // Water receives only broad value variation. Fine chroma noise on a
    // specular surface reads as compression artefacts from orbit.
    const double brightness = 0.91 + std::round(std::clamp(0.5 + regional * 0.34, 0.0, 1.0) * 5.0) / 5.0 * 0.16;
    red = static_cast<uint8_t>(std::clamp(std::lround(red * brightness), 0l, 255l));
    green = static_cast<uint8_t>(std::clamp(std::lround(green * brightness), 0l, 255l));
    blue = static_cast<uint8_t>(std::clamp(std::lround(blue * brightness), 0l, 255l));
  }
  else
  {
    // Stone, sand, snow, shore, and water retain their authored identity while
    // acquiring an independent warm/cool hue axis as well as value breakup.
    const double warmth = (hue - 0.5) * 2.0;
    const double brightness = 0.82 + tone * 0.31;
    red = static_cast<uint8_t>(std::clamp(std::lround(red * brightness * (1.0 + warmth * 0.10)), 0l, 255l));
    green = static_cast<uint8_t>(std::clamp(std::lround(green * brightness * (1.0 + warmth * 0.015)), 0l, 255l));
    blue = static_cast<uint8_t>(std::clamp(std::lround(blue * brightness * (1.0 - warmth * 0.10)), 0l, 255l));
  }
  result.albedoRGBA8 = rgba(red, green, blue);
  return result;
}

// Runtime terrain detail, evaluated in planet space so adjacent pages and cube
// faces agree without storing a dense global height field.
//
// The bake resolves 307 m cells. Everything a person actually stands next to —
// ledges, boulders, scree, gully walls — is below that, so it has to be
// generated here or it does not exist. What the bake supplies is *which* rock is
// exposed, and this function turns that into landforms: layered rock terraces,
// resistant rock stands up as tors and boulder fields, weak rock rounds off.
//
// Lithology drives it rather than surface cover. The previous version keyed its
// rock behaviour off the Rock, Bare, Scorched and Snow cover classes, and the
// climate classifier emits none of them on any planet we generate, so that path
// had never once run.
double proceduralTerrainDetail(
    const procedural::SpherePoint &direction,
    const procedural::PlanetPackageMetadata &metadata,
    const double mountain,
    const double tectonics,
    const uint8_t material,
    const uint8_t lithology,
    const double strength)
{
  using Material = procedural::PolygonTerrainMaterial;
  if (!(strength > 0.0) || material == static_cast<uint8_t>(Material::Ocean) || material == static_cast<uint8_t>(Material::Lake) || material == static_cast<uint8_t>(Material::River))
    return 0.0;

  const procedural::PlanetLithologyProperties &rock = procedural::planetLithologyProperties(lithology);
  // Resistance is the inverse of erodibility, normalized so the table's
  // geometric mean sits near the middle of the range. Strong rock holds a face;
  // weak rock cannot, and that difference is the whole vocabulary here.
  const double resistance = std::clamp(1.0 / std::max(static_cast<double>(rock.erodibility), 0.05), 0.0, 5.0) / 5.0;
  const bool layered = lithology == static_cast<uint8_t>(procedural::PlanetLithology::Shale) || lithology == static_cast<uint8_t>(procedural::PlanetLithology::Sandstone) ||
                       lithology == static_cast<uint8_t>(procedural::PlanetLithology::Limestone) || lithology == static_cast<uint8_t>(procedural::PlanetLithology::Sedimentary);
  const bool desert = material == static_cast<uint8_t>(Material::TemperateDesert) || material == static_cast<uint8_t>(Material::SubtropicalDesert);

  const double mountainWeight = std::clamp(std::max(mountain, tectonics * 0.72) + resistance * 0.30, 0.0, 1.0);
  const double rollingWeight = 0.18 + mountainWeight * 0.82;
  const uint64_t seed = metadata.contentHash ^ 0x6a09e667f3bcc909ull;
  const auto noise = [&](const double wavelength, const uint64_t octave)
  {
    return smoothNoise3D(direction, metadata.planetRadiusMetres, wavelength, mixNoise(seed + octave * 0x9e3779b97f4a7c15ull));
  };

  if (desert)
  {
    // Broad, coherent dunes replace the generic ground/fine octaves. Using a
    // tangent-like pair of world-space phases gives smooth windward slopes and
    // slightly sharper lee faces while remaining seamless across cube pages.
    constexpr double twoPi = 4.0 * kHalfPi;
    const double eastMetres = (direction.x * 0.79 + direction.z * 0.61) * metadata.planetRadiusMetres;
    const double crossMetres = (direction.x * -0.61 + direction.z * 0.79) * metadata.planetRadiusMetres;
    const double phase = twoPi * eastMetres / 58.0 + noise(190.0, 7u) * 2.1;
    const double crossPhase = twoPi * crossMetres / 31.0 + noise(120.0, 8u);
    const double envelope = std::clamp(0.72 + noise(760.0, 9u) * 0.28, 0.35, 1.0);
    const double dune = (std::sin(phase) * 1.65 + std::sin(phase * 2.0) * 0.44 + std::sin(crossPhase) * 0.28) * envelope;
    const double basin = noise(820.0, 10u) * 3.2 + noise(260.0, 11u) * 0.85;
    return strength * (basin + dune);
  }

  // Mountain-scale shape. Ridges sharpen with rock resistance: a granite range
  // keeps an edge where a shale one is already a rounded hill.
  const double broadRelief = noise(2048.0, 0u) * 95.0 * mountainWeight * mountainWeight;
  const double ridgeSharpness = 2.35 + resistance * 1.60;
  const double ridgeCrest = (std::pow(std::clamp(1.0 - std::abs(noise(760.0, 5u)), 0.0, 1.0), ridgeSharpness) - 0.32) * 58.0 * mountainWeight * mountainWeight;
  const double branchingRidge = (std::pow(std::clamp(1.0 - std::abs(noise(285.0, 6u)), 0.0, 1.0), 3.0 + resistance) - 0.25) * 11.0 * mountainWeight;
  const double hillRelief = noise(512.0, 1u) * 22.0 * rollingWeight;
  const double shoulderRelief = noise(160.0, 2u) * 6.0 * rollingWeight;

  // Gullies: a narrow incision along the ridge network's inverse, which cuts
  // the V-shaped side valleys that the 307 m bake cannot resolve. Weak rock
  // gullies more deeply, so this scales against resistance rather than with it.
  const double gullyRidge = std::pow(std::clamp(1.0 - std::abs(noise(96.0, 12u)), 0.0, 1.0), 6.0);
  const double gully = -gullyRidge * (9.0 + 14.0 * (1.0 - resistance)) * rollingWeight;

  double detail = broadRelief + ridgeCrest + branchingRidge + hillRelief + shoulderRelief + gully;

  if (layered)
  {
    // Bedding terraces. Quantizing the accumulated relief into steps is what
    // turns a smooth slope into a stack of ledges, and it is the runtime echo
    // of the stratigraphy the solver eroded differentially at bake scale.
    //
    // The step height is a real bed thickness, not a noise wavelength, and the
    // soft edge keeps the riser from being a single-voxel cliff everywhere.
    // Bed thickness is a property of the sequence, not of how hard one bed is:
    // a terrace forms from the *contrast* between a resistant bed and the weak
    // one under it, which is why sandstone and limestone hold ledges despite
    // sitting mid-table on absolute erodibility.
    const double bedMetres = 14.0 + resistance * 22.0;
    const double warp = noise(220.0, 13u) * bedMetres * 0.8;
    const double section = (detail + warp) / bedMetres;
    const double bed = std::floor(section);
    const double within = section - bed;
    // A riser occupying the top fifth of each bed, eased so the tread is flat.
    const double riser = std::clamp((within - 0.80) / 0.20, 0.0, 1.0);
    const double terraced = (bed + riser * riser * (3.0 - 2.0 * riser)) * bedMetres - warp;
    const double terraceStrength = 0.85 * std::clamp(0.35 + mountainWeight, 0.0, 1.0);
    detail = detail + (terraced - detail) * terraceStrength;
  }

  // Tors and boulder fields. Sparse, tall, isolated: thresholding the noise and
  // easing the remainder gives standing rocks with ground between them, where
  // plain noise of the same amplitude would give an even lumpiness that reads as
  // texture rather than as objects.
  //
  // Only resistant rock does this. A boulder is what is left when everything
  // softer around it has gone, so shale has none and granite has many.
  const double boulderPresence = std::clamp(resistance * 1.35 - 0.28, 0.0, 1.0);
  if (boulderPresence > 0.0)
  {
    const double field = noise(38.0, 14u) * 0.5 + 0.5;
    // A high threshold is what makes these objects rather than texture: at 0.68
    // roughly a third of the ground was raised and the result read as generic
    // lumpiness. Past 0.82 only isolated patches survive, with open ground
    // between them, which is what a boulder field looks like.
    const double threshold = 0.86 - boulderPresence * 0.06;
    const double mass = std::clamp((field - threshold) / std::max(1.0 - threshold, 1.0e-3), 0.0, 1.0);
    // Height ranges from a waist-high block to a house-sized tor.
    const double boulder = std::pow(mass, 0.55) * (4.0 + 16.0 * boulderPresence);
    // Individual blocks on top of the mass, so a tor is not one smooth dome.
    const double blockiness = noise(9.0, 15u) * 1.1 * mass;
    detail = detail + (boulder + blockiness) * (0.45 + rollingWeight * 0.55);
  }

  // Scree below the standing rock. Weak rock supplies more debris, and the
  // fill is smooth and low-frequency because a talus slope is graded.
  const double screeSupply = std::clamp(1.0 - resistance, 0.0, 1.0) * rollingWeight;
  const double scree = std::max(noise(64.0, 16u), 0.0) * 3.4 * screeSupply;

  const double groundRelief = noise(48.0, 3u) * 1.4 * (0.35 + rollingWeight * 0.65);
  const double fineRelief = noise(14.0, 4u) * 0.32 * (0.45 + rollingWeight * 0.55);
  return strength * (detail + scree + groundRelief + fineRelief);
}

bool writeTranslatedHierarchySlot(
    SparseVoxelBrickHierarchy &destination,
    const SparseVoxelBrickHierarchy &source,
    const std::array<float, 3> &translation,
    const std::array<int32_t, 3> &canonicalTranslation,
    const uint32_t canonicalScale,
    const std::array<double, 4> *canonicalWorldGrid,
    const uint32_t regionMappingId,
    const std::array<float, 4> &finerClipBounds,
    const uint32_t clipmapLOD,
    const bool forceLeafRaster,
    const uint32_t pageSlot,
    const uint32_t nodeBase,
    const uint32_t nodeCapacity,
    const uint32_t childBase,
    const uint32_t childCapacity,
    const uint32_t rootBase,
    const uint32_t rootCapacity,
    const uint32_t materialBase,
    const uint32_t materialCapacity,
    const uint32_t paletteBase,
    const uint32_t paletteCapacity,
    uint64_t *nodeRevision,
    std::string *error)
{
  if (source.empty())
    return fail(error, "Cannot write an empty planet brick hierarchy slot");
  if (source.nodes.size() > nodeCapacity || source.occupancy.size() > nodeCapacity || source.childIndices.size() > childCapacity || source.rootIndices.size() > rootCapacity ||
      source.materialIndices.size() > materialCapacity || (!source.microSurfaceData.empty() && source.microSurfaceData.size() != source.materialIndices.size()) || source.palette.size() > paletteCapacity ||
      source.paletteDescriptorIds.size() != source.palette.size() || static_cast<uint64_t>(nodeBase) + nodeCapacity > destination.nodes.size() ||
      static_cast<uint64_t>(nodeBase) + nodeCapacity > destination.occupancy.size() || static_cast<uint64_t>(childBase) + childCapacity > destination.childIndices.size() ||
      static_cast<uint64_t>(rootBase) + rootCapacity > destination.rootIndices.size() || static_cast<uint64_t>(materialBase) + materialCapacity > destination.materialIndices.size() ||
      (!destination.microSurfaceData.empty() && destination.microSurfaceData.size() != destination.materialIndices.size()) || static_cast<uint64_t>(paletteBase) + paletteCapacity > destination.palette.size() ||
      destination.paletteDescriptorIds.size() != destination.palette.size())
    return fail(error, "Planet brick hierarchy exceeds its physical clipmap slot");
  if (destination.paletteStride != 0u && destination.paletteStride != source.paletteStride)
    return fail(error, "Planet chunk brick hierarchies use incompatible layouts");
  if (destination.paletteStride == 0u)
  {
    destination.paletteStride = source.paletteStride;
    destination.finestVoxelSize = source.finestVoxelSize;
  }
  else
  {
    destination.finestVoxelSize = std::min(destination.finestVoxelSize, source.finestVoxelSize);
  }

  if (canonicalScale == 0u)
    return fail(error, "Planet brick canonical grid scale is zero");
  if (canonicalWorldGrid != nullptr &&
      (!std::isfinite((*canonicalWorldGrid)[0]) ||
       !std::isfinite((*canonicalWorldGrid)[1]) ||
       !std::isfinite((*canonicalWorldGrid)[2]) ||
       !std::isfinite((*canonicalWorldGrid)[3]) ||
       !((*canonicalWorldGrid)[3] > 0.0)))
    return fail(error, "Planet brick canonical world grid is invalid");
  for (const SparseVoxelBrickNodeGPU &node : source.nodes)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const int64_t value = static_cast<int64_t>(node.canonicalGridOriginScale[axis]) * canonicalScale + canonicalTranslation[axis];
      if (value < std::numeric_limits<int32_t>::min() || value > std::numeric_limits<int32_t>::max())
        return fail(error, "Planet brick canonical grid origin exceeds 32 bits");
    }
    const int64_t scale = static_cast<int64_t>(node.canonicalGridOriginScale[3]) * canonicalScale;
    if (scale <= 0 || scale > std::numeric_limits<int32_t>::max())
      return fail(error, "Planet brick canonical grid scale exceeds 32 bits");
  }

  uint64_t packedNodeRevision = 0xcbf29ce484222325ull;
  const auto hashBytes = [&](const void *data, const size_t byteCount)
  {
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t byte = 0u; byte < byteCount; ++byte)
    {
      packedNodeRevision ^= bytes[byte];
      packedNodeRevision *= 0x100000001b3ull;
    }
  };
  for (size_t index = 0u; index < source.nodes.size(); ++index)
  {
    auto node = source.nodes[index];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      node.canonicalGridOriginScale[axis] = static_cast<int32_t>(static_cast<int64_t>(node.canonicalGridOriginScale[axis]) * canonicalScale + canonicalTranslation[axis]);
      // Generated terrain is addressed by one integer lattice spanning every
      // streamed page. Reconstruct its float position from that lattice in one
      // double-precision operation. Adding a rounded page centre to a rounded
      // local brick origin made the two sides of a page boundary land on
      // different floats, which becomes a visible gap or overlap when the eye
      // is centimetres from terrain (especially kilometres from world zero).
      node.minimumVoxelSize[axis] = canonicalWorldGrid != nullptr
          ? static_cast<float>((*canonicalWorldGrid)[axis] +
                static_cast<double>(node.canonicalGridOriginScale[axis]) *
                    (*canonicalWorldGrid)[3])
          : node.minimumVoxelSize[axis] + translation[axis];
    }
    node.canonicalGridOriginScale[3] = static_cast<int32_t>(static_cast<int64_t>(node.canonicalGridOriginScale[3]) * canonicalScale);
    if (canonicalWorldGrid != nullptr)
      node.minimumVoxelSize[3] = static_cast<float>(
          static_cast<double>(node.canonicalGridOriginScale[3]) *
          (*canonicalWorldGrid)[3]);
    node.regionMappingId = regionMappingId;
    // Low eight bits retain the hierarchy-local brick level. The upper byte
    // identifies the terrain clipmap ring, letting visibility disable exact
    // microgeometry whenever either representation is coarser than canonical.
    node.level = (node.level & 0xffu) | ((clipmapLOD & 0xffu) << 8u) |
        (forceLeafRaster ? kSparseVoxelBrickForceLeafRaster : 0u);
    // Progressive terrain uses this as the stable GPU refinement-request ID.
    // The legacy ring path below replaces it with a tagged overlap rectangle.
    node.instanceIndex = pageSlot;
    node.occupancyIndex += nodeBase;
    node.materialBase += materialBase;
    if (node.parentIndex != kSparseVoxelBrickInvalidIndex)
      node.parentIndex += nodeBase;
    if (node.childCount != 0u)
      node.firstChild += childBase;
    node.paletteBase += paletteBase;
    if (finerClipBounds[0] < finerClipBounds[2] && finerClipBounds[1] < finerClipBounds[3])
    {
      const float voxelSize = node.minimumVoxelSize[3];
      const float brickExtent = voxelSize * static_cast<float>(kSparseVoxelBrickSide);
      const float minimumX = std::max(node.minimumVoxelSize[0], finerClipBounds[0]);
      const float minimumZ = std::max(node.minimumVoxelSize[2], finerClipBounds[1]);
      const float maximumX = std::min(node.minimumVoxelSize[0] + brickExtent, finerClipBounds[2]);
      const float maximumZ = std::min(node.minimumVoxelSize[2] + brickExtent, finerClipBounds[3]);
      if (minimumX < maximumX && minimumZ < maximumZ)
      {
        const auto quantizeMinimum = [&](const float value, const uint32_t axis)
        {
          const float local = (value - node.minimumVoxelSize[axis]) / voxelSize;
          return static_cast<uint32_t>(std::clamp(std::ceil(local * kSparseVoxelBrickTerrainClipQuantization), 0.0f, static_cast<float>(kSparseVoxelBrickSide * kSparseVoxelBrickTerrainClipQuantization)));
        };
        const auto quantizeMaximum = [&](const float value, const uint32_t axis)
        {
          const float local = (value - node.minimumVoxelSize[axis]) / voxelSize;
          return static_cast<uint32_t>(std::clamp(std::floor(local * kSparseVoxelBrickTerrainClipQuantization), 0.0f, static_cast<float>(kSparseVoxelBrickSide * kSparseVoxelBrickTerrainClipQuantization)));
        };
        // The packed rectangle is an exclusion region: rounding it outward
        // deletes coarse terrain which the finer cascade may not cover. Round
        // both edges inward instead. A sub-step overlap can depth-test safely;
        // a sub-step hole is the visible square crack this ownership mask must
        // never create, especially after world-space floats lose low bits.
        const uint32_t packedMinimumX = quantizeMinimum(minimumX, 0u);
        const uint32_t packedMinimumZ = quantizeMinimum(minimumZ, 2u);
        const uint32_t packedMaximumX = quantizeMaximum(maximumX, 0u);
        const uint32_t packedMaximumZ = quantizeMaximum(maximumZ, 2u);
        if (packedMinimumX < packedMaximumX && packedMinimumZ < packedMaximumZ)
          node.instanceIndex = kSparseVoxelBrickTerrainClipFlag | packedMinimumX | (packedMinimumZ << 7u) | (packedMaximumX << 14u) | (packedMaximumZ << 21u);
      }
    }
    destination.nodes[nodeBase + index] = node;
    hashBytes(node.minimumVoxelSize.data(), node.minimumVoxelSize.size() * sizeof(node.minimumVoxelSize[0]));
    hashBytes(node.canonicalGridOriginScale.data(), node.canonicalGridOriginScale.size() * sizeof(node.canonicalGridOriginScale[0]));
    const std::array<uint32_t, 13u> packedFields{
      node.packedOccupiedBounds,
      node.coarseOccupancyLow,
      node.coarseOccupancyHigh,
      node.occupancyIndex,
      node.materialBase,
      node.parentIndex,
      node.childCount,
      node.level,
      node.occupiedCount,
      node.firstChild,
      node.instanceIndex,
      node.paletteBase,
      node.regionMappingId,
    };
    hashBytes(packedFields.data(), packedFields.size() * sizeof(packedFields[0]));
  }
  hashBytes(&nodeBase, sizeof(nodeBase));
  hashBytes(&nodeCapacity, sizeof(nodeCapacity));
  if (nodeRevision != nullptr)
    *nodeRevision = packedNodeRevision;
  std::copy(source.occupancy.begin(), source.occupancy.end(), destination.occupancy.begin() + nodeBase);
  std::copy(source.materialIndices.begin(), source.materialIndices.end(), destination.materialIndices.begin() + materialBase);
  if (!destination.microSurfaceData.empty())
    std::copy(source.microSurfaceData.begin(), source.microSurfaceData.end(), destination.microSurfaceData.begin() + materialBase);
  std::copy(source.palette.begin(), source.palette.end(), destination.palette.begin() + paletteBase);
  std::copy(source.paletteDescriptorIds.begin(), source.paletteDescriptorIds.end(), destination.paletteDescriptorIds.begin() + paletteBase);
  for (size_t index = 0u; index < source.childIndices.size(); ++index)
    destination.childIndices[childBase + index] = source.childIndices[index] + nodeBase;
  for (size_t index = 0u; index < source.rootIndices.size(); ++index)
    destination.rootIndices[rootBase + index] = source.rootIndices[index] + nodeBase;
  // Every page range is defined over its whole capacity, not just the part the
  // payload fills: the upload publishes the full reservation, so a tail left
  // undefined would reach the GPU. Clearing tails here is what lets the arena
  // mirror itself be recycled between transactions instead of reallocated and
  // zeroed - the tails are page sized, the arena is gigabytes.
  const auto clearTail = [](auto &vector, const size_t base, const size_t used,
                            const size_t capacity)
  {
    if (used >= capacity)
      return;
    using Value = typename std::decay_t<decltype(vector)>::value_type;
    std::fill(
        vector.begin() + static_cast<ptrdiff_t>(base + used),
        vector.begin() + static_cast<ptrdiff_t>(base + capacity), Value{});
  };
  clearTail(destination.nodes, nodeBase, source.nodes.size(), nodeCapacity);
  clearTail(
      destination.occupancy, nodeBase, source.occupancy.size(), nodeCapacity);
  clearTail(
      destination.childIndices, childBase, source.childIndices.size(),
      childCapacity);
  clearTail(
      destination.rootIndices, rootBase, source.rootIndices.size(),
      rootCapacity);
  clearTail(
      destination.materialIndices, materialBase,
      source.materialIndices.size(), materialCapacity);
  if (!destination.microSurfaceData.empty())
    clearTail(
        destination.microSurfaceData, materialBase,
        source.microSurfaceData.size(), materialCapacity);
  clearTail(
      destination.palette, paletteBase, source.palette.size(),
      paletteCapacity);
  clearTail(
      destination.paletteDescriptorIds, paletteBase,
      source.paletteDescriptorIds.size(), paletteCapacity);
  destination.levelCount = std::max(destination.levelCount, source.levelCount);
  destination.paletteInstanceCount += source.paletteInstanceCount;
  destination.paletteExactInstances += source.paletteExactInstances;
  const uint64_t previousApproximationCount = destination.paletteEmittedApproximated;
  destination.paletteEmittedVoxels += source.paletteEmittedVoxels;
  destination.paletteEmittedApproximated += source.paletteEmittedApproximated;
  if (destination.paletteEmittedApproximated != 0u)
    destination.paletteEmittedMeanError = (destination.paletteEmittedMeanError * previousApproximationCount + source.paletteEmittedMeanError * source.paletteEmittedApproximated) / destination.paletteEmittedApproximated;
  destination.paletteEmittedMaximumError = std::max(destination.paletteEmittedMaximumError, source.paletteEmittedMaximumError);
  destination.paletteMaximumError = std::max(destination.paletteMaximumError, source.paletteMaximumError);
  destination.paletteStraddlingBricks += source.paletteStraddlingBricks;
  destination.paletteStraddlingVoxels += source.paletteStraddlingVoxels;
  return true;
}

// Rewrites just a page's root index range. Root indices are packed by
// publication order, so a page whose payload and every other range are
// unchanged still moves here, and rebasing four indices is much cheaper than
// re-translating the page to move them.
bool rewriteTranslatedHierarchyRoots(
    SparseVoxelBrickHierarchy &destination,
    const SparseVoxelBrickHierarchy &source,
    const uint32_t nodeBase,
    const uint32_t rootBase,
    const uint32_t rootCapacity,
    std::string *error)
{
  if (source.rootIndices.size() > rootCapacity ||
      uint64_t{rootBase} + rootCapacity > destination.rootIndices.size())
    return fail(error, "terrain page root range does not fit its reservation");
  for (size_t index = 0u; index < source.rootIndices.size(); ++index)
    destination.rootIndices[rootBase + index] =
        source.rootIndices[index] + nodeBase;
  std::fill(
      destination.rootIndices.begin() + static_cast<ptrdiff_t>(rootBase) +
          static_cast<ptrdiff_t>(source.rootIndices.size()),
      destination.rootIndices.begin() + static_cast<ptrdiff_t>(rootBase) +
          static_cast<ptrdiff_t>(rootCapacity),
      0u);
  return true;
}

bool samePageSettings(const PlanetVoxelChunkSettings &left, const PlanetVoxelChunkSettings &right)
{
  return left.horizontalVoxelCount == right.horizontalVoxelCount && left.patchEdgeMetres == right.patchEdgeMetres && left.foundationDepthVoxels == right.foundationDepthVoxels &&
         left.surfaceShellDepthVoxels == right.surfaceShellDepthVoxels && left.maximumVerticalVoxels == right.maximumVerticalVoxels && left.maximumBricks == right.maximumBricks &&
         left.proceduralDetailStrength == right.proceduralDetailStrength && left.proceduralTerraceStrength == right.proceduralTerraceStrength &&
         left.proceduralTerraceHeightMetres == right.proceduralTerraceHeightMetres && left.proceduralTerraceTransitionFraction == right.proceduralTerraceTransitionFraction &&
         left.microMaterialRevision == right.microMaterialRevision;
}

bool sameSettings(const PlanetVoxelChunkSettings &left, const PlanetVoxelChunkSettings &right)
{
  return samePageSettings(left, right) && left.clipmapLevelCount == right.clipmapLevelCount && left.requestedLODMask == right.requestedLODMask && left.clipmapInnerHoleRadius == right.clipmapInnerHoleRadius &&
         left.clipmapResolutionDropInterval == right.clipmapResolutionDropInterval && left.clipmapMinimumHorizontalVoxelCount == right.clipmapMinimumHorizontalVoxelCount &&
         left.maximumResidentChunks == right.maximumResidentChunks && left.strategy == right.strategy;
}

// Raw brick-node records are part of the transient payload. Bump the magic
// whenever their ABI changes so an old temporary page is rebuilt rather than
// interpreted with a new stride.
// Bumped when the per-column surface height raster joined the payload: an
// older page restores without it and a mesh proxy would read a flat chunk.
constexpr uint64_t kPlanetVoxelTransientPageMagic = 0x4250474e4c505656ull;

template <typename T> void appendValue(std::vector<uint8_t> &bytes, const T &value)
{
  static_assert(std::is_trivially_copyable<T>::value);
  const size_t offset = bytes.size();
  bytes.resize(offset + sizeof(T));
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

template <typename T> void appendValues(std::vector<uint8_t> &bytes, const std::vector<T> &values)
{
  static_assert(std::is_trivially_copyable<T>::value);
  appendValue(bytes, static_cast<uint64_t>(values.size()));
  const size_t valueBytes = values.size() * sizeof(T);
  const size_t offset = bytes.size();
  bytes.resize(offset + valueBytes);
  if (valueBytes != 0u)
    std::memcpy(bytes.data() + offset, values.data(), valueBytes);
}

struct PlanetVoxelPageReader
{
  const std::vector<uint8_t> &bytes;
  size_t offset = 0u;

  template <typename T> bool read(T &value)
  {
    static_assert(std::is_trivially_copyable<T>::value);
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
      return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
  }

  template <typename T> bool readValues(std::vector<T> &values)
  {
    static_assert(std::is_trivially_copyable<T>::value);
    uint64_t count = 0u;
    if (!read(count) || count > (bytes.size() - offset) / sizeof(T))
      return false;
    values.resize(static_cast<size_t>(count));
    const size_t valueBytes = values.size() * sizeof(T);
    if (valueBytes != 0u)
      std::memcpy(values.data(), bytes.data() + offset, valueBytes);
    offset += valueBytes;
    return true;
  }
};

void appendTile(std::vector<uint8_t> &bytes, const procedural::PlanetTileAddress &tile)
{
  appendValue(bytes, tile.face);
  appendValue(bytes, tile.level);
  appendValue(bytes, tile.x);
  appendValue(bytes, tile.y);
}

void appendString(std::vector<uint8_t> &bytes, const std::string &value)
{
  appendValue(bytes, static_cast<uint64_t>(value.size()));
  const size_t offset = bytes.size();
  bytes.resize(offset + value.size());
  if (!value.empty())
    std::memcpy(bytes.data() + offset, value.data(), value.size());
}

bool readString(PlanetVoxelPageReader &reader, std::string &value)
{
  uint64_t size = 0u;
  if (!reader.read(size) || size > 1u << 20u || reader.offset > reader.bytes.size() || size > reader.bytes.size() - reader.offset)
    return false;
  value.assign(reinterpret_cast<const char *>(reader.bytes.data() + reader.offset), static_cast<size_t>(size));
  reader.offset += static_cast<size_t>(size);
  return true;
}

bool readTile(PlanetVoxelPageReader &reader, procedural::PlanetTileAddress &tile)
{
  return reader.read(tile.face) && reader.read(tile.level) && reader.read(tile.x) && reader.read(tile.y) && procedural::validPlanetTileAddress(tile);
}

std::vector<uint8_t> serializePlanetVoxelTransientPageInternal(const PlanetVoxelChunk &chunk)
{
  std::vector<uint8_t> bytes;
  appendValue(bytes, kPlanetVoxelTransientPageMagic);
  appendTile(bytes, chunk.requestedTile);
  appendTile(bytes, chunk.sourceTile);
  appendValue(bytes, chunk.edgeMetres);
  appendValue(bytes, chunk.voxelSizeMetres);
  appendValue(bytes, chunk.elevationOriginMetres);
  appendValue(bytes, chunk.minimumSurfaceMetres);
  appendValue(bytes, chunk.maximumSurfaceMetres);
  appendValue(bytes, chunk.centerSurfaceMetres);
  appendValue(bytes, chunk.centerMaterial);
  appendValue(bytes, chunk.leafVoxelCount);
  appendValue(bytes, chunk.surfaceHeightSide);
  appendValue(bytes, chunk.surfaceHeightHalo);
  appendValues(bytes, chunk.surfaceHeights);
  appendValues(bytes, chunk.surfaceEnvelope);
  appendValues(bytes, chunk.surfaceMaterials);
  appendValue(bytes, static_cast<uint64_t>(chunk.materialReferences.size()));
  for (const auto &reference : chunk.materialReferences)
  {
    appendValue(bytes, reference.handle);
    appendValue(bytes, reference.contextFields);
    appendValue(bytes, reference.assetHash);
    appendString(bytes, reference.assetId);
  }
  appendValue(bytes, chunk.bakedContextFields);
  appendValues(bytes, chunk.auxiliaryContextIndices);
  appendValues(bytes, chunk.auxiliaryContextPool);
  const auto &bricks = chunk.bricks;
  appendValue(bytes, bricks.paletteStride);
  appendValue(bytes, bricks.paletteInstanceCount);
  appendValue(bytes, bricks.paletteExactInstances);
  appendValue(bytes, bricks.paletteMeanError);
  appendValue(bytes, bricks.paletteMaximumError);
  appendValue(bytes, bricks.paletteEmittedVoxels);
  appendValue(bytes, bricks.paletteEmittedApproximated);
  appendValue(bytes, bricks.paletteEmittedMeanError);
  appendValue(bytes, bricks.paletteEmittedMaximumError);
  appendValue(bytes, bricks.paletteStraddlingBricks);
  appendValue(bytes, bricks.paletteStraddlingVoxels);
  appendValue(bytes, bricks.rootCount);
  appendValue(bytes, bricks.levelCount);
  appendValue(bytes, bricks.finestVoxelSize);
  appendValues(bytes, bricks.nodes);
  appendValues(bytes, bricks.occupancy);
  appendValues(bytes, bricks.materialIndices);
  appendValues(bytes, bricks.microSurfaceData);
  appendValues(bytes, bricks.palette);
  appendValues(bytes, bricks.paletteDescriptorIds);
  appendValues(bytes, bricks.childIndices);
  appendValues(bytes, bricks.rootIndices);
  return bytes;
}

bool deserializePlanetVoxelTransientPageInternal(const std::vector<uint8_t> &bytes, PlanetVoxelChunk &chunk, std::string *error)
{
  if (error != nullptr)
    error->clear();
  PlanetVoxelChunk result{};
  PlanetVoxelPageReader reader{bytes};
  uint64_t magic = 0u;
  uint64_t materialReferenceCount = 0u;
  auto &bricks = result.bricks;
  if (!reader.read(magic))
  {
    if (error != nullptr)
      *error = "Planet voxel transient page header is truncated";
    return false;
  }
  if (magic != kPlanetVoxelTransientPageMagic)
  {
    if (error != nullptr)
      *error = "Planet voxel transient page version is unsupported";
    return false;
  }
  if (!readTile(reader, result.requestedTile) || !readTile(reader, result.sourceTile) || !reader.read(result.edgeMetres) || !reader.read(result.voxelSizeMetres) || !reader.read(result.elevationOriginMetres) ||
      !reader.read(result.minimumSurfaceMetres) || !reader.read(result.maximumSurfaceMetres) || !reader.read(result.centerSurfaceMetres) || !reader.read(result.centerMaterial) || !reader.read(result.leafVoxelCount) ||
      !reader.read(result.surfaceHeightSide) || !reader.read(result.surfaceHeightHalo) || !reader.readValues(result.surfaceHeights) || !reader.readValues(result.surfaceEnvelope) ||
      !reader.readValues(result.surfaceMaterials) || !reader.read(materialReferenceCount) || materialReferenceCount > 65535u)
  {
    if (error != nullptr)
      *error = "Planet voxel transient page payload is corrupt";
    return false;
  }
  result.materialReferences.resize(static_cast<size_t>(materialReferenceCount));
  for (auto &reference : result.materialReferences)
    if (!reader.read(reference.handle) || !reader.read(reference.contextFields) || !reader.read(reference.assetHash) || !readString(reader, reference.assetId))
    {
      if (error != nullptr)
        *error = "Planet voxel transient material references are corrupt";
      return false;
    }
  if (!reader.read(result.bakedContextFields) || !reader.readValues(result.auxiliaryContextIndices) || !reader.readValues(result.auxiliaryContextPool) || !reader.read(bricks.paletteStride) ||
      !reader.read(bricks.paletteInstanceCount) || !reader.read(bricks.paletteExactInstances) || !reader.read(bricks.paletteMeanError) || !reader.read(bricks.paletteMaximumError) ||
      !reader.read(bricks.paletteEmittedVoxels) || !reader.read(bricks.paletteEmittedApproximated) || !reader.read(bricks.paletteEmittedMeanError) || !reader.read(bricks.paletteEmittedMaximumError) ||
      !reader.read(bricks.paletteStraddlingBricks) || !reader.read(bricks.paletteStraddlingVoxels) || !reader.read(bricks.rootCount) || !reader.read(bricks.levelCount) || !reader.read(bricks.finestVoxelSize) ||
      !reader.readValues(bricks.nodes) || !reader.readValues(bricks.occupancy) || !reader.readValues(bricks.materialIndices) || !reader.readValues(bricks.microSurfaceData) || !reader.readValues(bricks.palette) ||
      !reader.readValues(bricks.paletteDescriptorIds) || !reader.readValues(bricks.childIndices) || !reader.readValues(bricks.rootIndices) || reader.offset != bytes.size() || bricks.empty() ||
      bricks.nodes.size() != bricks.occupancy.size() || (!bricks.microSurfaceData.empty() && bricks.microSurfaceData.size() != bricks.materialIndices.size()) || bricks.paletteDescriptorIds.size() != bricks.palette.size() ||
      bricks.rootCount != bricks.rootIndices.size() || (!result.auxiliaryContextIndices.empty() && result.auxiliaryContextIndices.size() != bricks.materialIndices.size()) ||
      (result.auxiliaryContextIndices.empty() != result.auxiliaryContextPool.empty()) || !(result.edgeMetres > 0.0) || !(result.voxelSizeMetres > 0.0))
  {
    if (error != nullptr)
      *error = "Planet voxel transient page payload is corrupt";
    return false;
  }
  for (const uint16_t index : result.auxiliaryContextIndices)
    if (index >= result.auxiliaryContextPool.size())
      return fail(error, "Planet voxel transient context index is corrupt");
  chunk = std::move(result);
  return true;
}

constexpr uint64_t kWorldVoxelTransientPageMagic = 0x4157504756565334ull;

void appendFieldRegion(
    std::vector<uint8_t> &bytes, const PlanetVoxelMaterialFieldRegion &region)
{
  appendTile(bytes, region.tile);
  appendValue(bytes, region.regionMappingId);
  appendValue(bytes, region.canonicalParentOrigin);
  appendValue(bytes, region.canonicalParentExtent);
  for (const auto &values : region.values)
    appendValues(bytes, values);
}

bool readFieldRegion(
    PlanetVoxelPageReader &reader, PlanetVoxelMaterialFieldRegion &region)
{
  if (!readTile(reader, region.tile) || !reader.read(region.regionMappingId) ||
      !reader.read(region.canonicalParentOrigin) ||
      !reader.read(region.canonicalParentExtent))
    return false;
  for (auto &values : region.values)
    if (!reader.readValues(values))
      return false;
  return true;
}

// One generated-world page payload, in the form the page source caches it:
// local brick arrays plus local ranges, before any physical arena placement is
// assigned. Publication-time identity is deliberately excluded - a restored
// page is installed into whatever slot/range is available, exactly as a freshly
// generated one is.
void appendTerrainVirtualPageRecord(
    std::vector<uint8_t> &bytes, const TerrainVirtualPageRecord &record)
{
  appendTile(bytes, record.requestedTile);
  appendTile(bytes, record.sourceTile);
  appendValue(bytes, record.tileOffsetX);
  appendValue(bytes, record.tileOffsetY);
  appendValue(bytes, record.clipmapLOD);
  appendValue(bytes, record.regionMappingId);
  appendValue(bytes, record.measuredErrorMetres);
  appendValue(bytes, record.worldCenterXMetres);
  appendValue(bytes, record.worldCenterZMetres);
  appendValue(bytes, record.edgeMetres);
  appendValue(bytes, record.voxelSizeMetres);
  appendValue(bytes, record.localBrickNodeOffset);
  appendValue(bytes, record.localBrickNodeCount);
  appendValue(bytes, record.localBrickChildOffset);
  appendValue(bytes, record.localBrickChildCount);
  appendValue(bytes, record.localBrickMaterialOffset);
  appendValue(bytes, record.localBrickMaterialCount);
  appendValue(bytes, record.localBrickPaletteOffset);
  appendValue(bytes, record.localBrickPaletteCount);
  appendValue(bytes, record.brickPayloadRevision);
  appendValue(bytes, record.brickNodeRevision);
  appendValue(bytes, record.tree64Revision);
  appendValue(bytes, record.localFirstBrickRoot);
  appendValue(bytes, record.localBrickRootCount);
  appendValue(bytes, record.elevationOriginMetres);
  appendValue(bytes, record.minimumSurfaceMetres);
  appendValue(bytes, record.maximumSurfaceMetres);
  appendValue(bytes, record.centerSurfaceMetres);
  appendValue(bytes, record.centerMaterial);
  appendValue(bytes, record.leafVoxelCount);
  appendValue(bytes, record.brickCount);
}

bool readTerrainVirtualPageRecord(
    PlanetVoxelPageReader &reader, TerrainVirtualPageRecord &record)
{
  return readTile(reader, record.requestedTile) &&
      readTile(reader, record.sourceTile) &&
      reader.read(record.tileOffsetX) && reader.read(record.tileOffsetY) &&
      reader.read(record.clipmapLOD) &&
      reader.read(record.regionMappingId) &&
      reader.read(record.measuredErrorMetres) &&
      reader.read(record.worldCenterXMetres) &&
      reader.read(record.worldCenterZMetres) &&
      reader.read(record.edgeMetres) && reader.read(record.voxelSizeMetres) &&
      reader.read(record.localBrickNodeOffset) &&
      reader.read(record.localBrickNodeCount) &&
      reader.read(record.localBrickChildOffset) &&
      reader.read(record.localBrickChildCount) &&
      reader.read(record.localBrickMaterialOffset) &&
      reader.read(record.localBrickMaterialCount) &&
      reader.read(record.localBrickPaletteOffset) &&
      reader.read(record.localBrickPaletteCount) &&
      reader.read(record.brickPayloadRevision) &&
      reader.read(record.brickNodeRevision) &&
      reader.read(record.tree64Revision) &&
      reader.read(record.localFirstBrickRoot) &&
      reader.read(record.localBrickRootCount) &&
      reader.read(record.elevationOriginMetres) &&
      reader.read(record.minimumSurfaceMetres) &&
      reader.read(record.maximumSurfaceMetres) &&
      reader.read(record.centerSurfaceMetres) &&
      reader.read(record.centerMaterial) &&
      reader.read(record.leafVoxelCount) && reader.read(record.brickCount);
}

std::vector<uint8_t> serializeTerrainVirtualPagePayloadInternal(
    const TerrainVirtualPagePayload &page)
{
  std::vector<uint8_t> bytes;
  appendValue(bytes, kWorldVoxelTransientPageMagic);
  appendTile(bytes, page.centerTile);
  appendValue(bytes, static_cast<uint64_t>(page.records.size()));
  for (const auto &record : page.records)
    appendTerrainVirtualPageRecord(bytes, record);
  appendValue(bytes, page.edgeMetres);
  appendValue(bytes, page.voxelSizeMetres);
  appendValue(bytes, page.elevationOriginMetres);
  appendValue(bytes, page.centerSurfaceMetres);
  appendValue(bytes, page.centerMaterial);
  appendValue(bytes, page.leafVoxelCount);
  appendValue(bytes, page.payloadRevision);
  appendValue(bytes, page.surfaceHeightSide);
  appendValue(bytes, page.surfaceHeightHalo);
  appendValues(bytes, page.surfaceHeights);
  appendValues(bytes, page.surfaceEnvelope);
  appendValue(bytes, page.surfaceMaterialSide);
  appendValues(bytes, page.surfaceMaterials);
  appendValue(bytes, static_cast<uint64_t>(page.materialFieldRegions.size()));
  for (const auto &region : page.materialFieldRegions)
    appendFieldRegion(bytes, region);
  appendValue(bytes, page.minimumWorldMetres);
  appendValue(bytes, page.maximumWorldMetres);
  const auto &bricks = page.bricks;
  appendValue(bytes, bricks.paletteStride);
  appendValue(bytes, bricks.paletteInstanceCount);
  appendValue(bytes, bricks.paletteExactInstances);
  appendValue(bytes, bricks.paletteMeanError);
  appendValue(bytes, bricks.paletteMaximumError);
  appendValue(bytes, bricks.paletteEmittedVoxels);
  appendValue(bytes, bricks.paletteEmittedApproximated);
  appendValue(bytes, bricks.paletteEmittedMeanError);
  appendValue(bytes, bricks.paletteEmittedMaximumError);
  appendValue(bytes, bricks.paletteStraddlingBricks);
  appendValue(bytes, bricks.paletteStraddlingVoxels);
  appendValue(bytes, bricks.rootCount);
  appendValue(bytes, bricks.levelCount);
  appendValue(bytes, bricks.finestVoxelSize);
  appendValues(bytes, bricks.nodes);
  appendValues(bytes, bricks.occupancy);
  appendValues(bytes, bricks.materialIndices);
  appendValues(bytes, bricks.microSurfaceData);
  appendValues(bytes, bricks.palette);
  appendValues(bytes, bricks.paletteDescriptorIds);
  appendValues(bytes, bricks.childIndices);
  appendValues(bytes, bricks.rootIndices);
  return bytes;
}

bool deserializeTerrainVirtualPagePayloadInternal(
    const std::vector<uint8_t> &bytes,
    TerrainVirtualPagePayload &page,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  TerrainVirtualPagePayload result{};
  PlanetVoxelPageReader reader{bytes};
  uint64_t magic = 0u;
  if (!reader.read(magic))
    return fail(error, "Generated-world transient page header is truncated");
  if (magic != kWorldVoxelTransientPageMagic)
    return fail(error, "Generated-world transient page version is unsupported");
  uint64_t fieldRegionCount = 0u;
  uint64_t pageRecordCount = 0u;
  auto &bricks = result.bricks;
  if (!readTile(reader, result.centerTile) ||
      !reader.read(pageRecordCount) || pageRecordCount > 65535u)
    return fail(error, "Generated-world transient page payload is corrupt");
  result.records.resize(static_cast<size_t>(pageRecordCount));
  for (auto &record : result.records)
    if (!readTerrainVirtualPageRecord(reader, record))
      return fail(error, "Generated-world transient page records are corrupt");
  if (!reader.read(result.edgeMetres) ||
      !reader.read(result.voxelSizeMetres) ||
      !reader.read(result.elevationOriginMetres) ||
      !reader.read(result.centerSurfaceMetres) ||
      !reader.read(result.centerMaterial) ||
      !reader.read(result.leafVoxelCount) ||
      !reader.read(result.payloadRevision) ||
      !reader.read(result.surfaceHeightSide) ||
      !reader.read(result.surfaceHeightHalo) ||
      !reader.readValues(result.surfaceHeights) ||
      !reader.readValues(result.surfaceEnvelope) ||
      !reader.read(result.surfaceMaterialSide) ||
      !reader.readValues(result.surfaceMaterials) ||
      !reader.read(fieldRegionCount) || fieldRegionCount > 65535u)
    return fail(error, "Generated-world transient page payload is corrupt");
  result.materialFieldRegions.resize(static_cast<size_t>(fieldRegionCount));
  for (auto &region : result.materialFieldRegions)
    if (!readFieldRegion(reader, region))
      return fail(
          error, "Generated-world transient material fields are corrupt");
  if (!reader.read(result.minimumWorldMetres) ||
      !reader.read(result.maximumWorldMetres) ||
      !reader.read(bricks.paletteStride) ||
      !reader.read(bricks.paletteInstanceCount) ||
      !reader.read(bricks.paletteExactInstances) ||
      !reader.read(bricks.paletteMeanError) ||
      !reader.read(bricks.paletteMaximumError) ||
      !reader.read(bricks.paletteEmittedVoxels) ||
      !reader.read(bricks.paletteEmittedApproximated) ||
      !reader.read(bricks.paletteEmittedMeanError) ||
      !reader.read(bricks.paletteEmittedMaximumError) ||
      !reader.read(bricks.paletteStraddlingBricks) ||
      !reader.read(bricks.paletteStraddlingVoxels) ||
      !reader.read(bricks.rootCount) || !reader.read(bricks.levelCount) ||
      !reader.read(bricks.finestVoxelSize) ||
      !reader.readValues(bricks.nodes) ||
      !reader.readValues(bricks.occupancy) ||
      !reader.readValues(bricks.materialIndices) ||
      !reader.readValues(bricks.microSurfaceData) ||
      !reader.readValues(bricks.palette) ||
      !reader.readValues(bricks.paletteDescriptorIds) ||
      !reader.readValues(bricks.childIndices) ||
      !reader.readValues(bricks.rootIndices) || reader.offset != bytes.size() ||
      bricks.empty() || bricks.nodes.size() != bricks.occupancy.size() ||
      (!bricks.microSurfaceData.empty() && bricks.microSurfaceData.size() != bricks.materialIndices.size()) ||
      bricks.paletteDescriptorIds.size() != bricks.palette.size() ||
      bricks.rootCount != bricks.rootIndices.size() ||
      !(result.edgeMetres > 0.0) || !(result.voxelSizeMetres > 0.0))
    return fail(error, "Generated-world transient page payload is corrupt");
  const auto rangeWithin = [](const uint32_t offset,
                              const uint32_t count,
                              const size_t size)
  {
    return static_cast<uint64_t>(offset) <= size &&
        static_cast<uint64_t>(count) <=
            size - static_cast<uint64_t>(offset);
  };
  for (const auto &record : result.records)
  {
    if (!rangeWithin(
            record.localBrickNodeOffset, record.localBrickNodeCount,
            bricks.nodes.size()) ||
        !rangeWithin(
            record.localBrickChildOffset, record.localBrickChildCount,
            bricks.childIndices.size()) ||
        !rangeWithin(
            record.localBrickMaterialOffset, record.localBrickMaterialCount,
            bricks.materialIndices.size()) ||
        !rangeWithin(
            record.localBrickPaletteOffset, record.localBrickPaletteCount,
            bricks.palette.size()) ||
        !rangeWithin(
            record.localFirstBrickRoot, record.localBrickRootCount,
            bricks.rootIndices.size()) ||
        !(record.edgeMetres > 0.0) || !(record.voxelSizeMetres > 0.0))
      return fail(
          error, "Generated-world transient page record ranges are corrupt");
  }
  page = std::move(result);
  return true;
}

void hashBytes(uint64_t &hash, const void *data, const size_t size)
{
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t index = 0u; index < size; ++index)
  {
    hash ^= bytes[index];
    hash *= 1099511628211ull;
  }
}

template <typename T> void hashValue(uint64_t &hash, const T &value)
{
  static_assert(std::is_trivially_copyable<T>::value);
  hashBytes(hash, &value, sizeof(value));
}

TransientSparseVoxelPageKey planetTransientPageKey(const procedural::PlanetPackageMetadata &metadata, const PlanetVoxelChunkSettings &settings, const procedural::PlanetTileAddress &tile)
{
  uint64_t source = 1469598103934665603ull;
  hashValue(source, metadata.contentHash);
  hashValue(source, metadata.buildHash);
  hashValue(source, metadata.planetRadiusMetres);
  hashValue(source, metadata.nominalRenderPatchEdgeMetres);
  hashValue(source, settings.horizontalVoxelCount);
  hashValue(source, settings.patchEdgeMetres);
  hashValue(source, settings.foundationDepthVoxels);
  hashValue(source, settings.surfaceShellDepthVoxels);
  hashValue(source, settings.maximumVerticalVoxels);
  hashValue(source, settings.maximumBricks);
  hashValue(source, settings.proceduralDetailStrength);
  hashValue(source, settings.proceduralTerraceStrength);
  hashValue(source, settings.proceduralTerraceHeightMetres);
  hashValue(source, settings.proceduralTerraceTransitionFraction);
  hashValue(source, settings.microMaterialRevision);
  uint64_t page = 1469598103934665603ull;
  hashValue(page, tile.face);
  hashValue(page, tile.level);
  hashValue(page, tile.x);
  hashValue(page, tile.y);
  return {source, page};
}


} // namespace

uint8_t worldMapBiomeForColumn(
    const procedural::world::MapExportBiomeSample &biome,
    const procedural::Point2 &position,
    const procedural::world::MapExportBundle &bundle,
    const double finestVoxelSizeMetres)
{
  const double grid = finestVoxelSizeMetres > 0.0 &&
          std::isfinite(finestVoxelSizeMetres)
      ? finestVoxelSizeMetres
      : 1.0;
  const int64_t columnX = static_cast<int64_t>(std::floor(position.x / grid));
  const int64_t columnZ = static_cast<int64_t>(std::floor(position.y / grid));
  const uint64_t seed = mixNoise(
      bundle.seed ^ bundle.bundleSignature ^ 0xbb67ae8584caa73bull);
  const double unitSample =
      latticeNoise(columnX, columnZ, 0, seed) * 0.5 + 0.5;
  return biome.selectBiome(unitSample);
}

bool selectWorldMapTerrainSpawnView(
    const procedural::world::MapExportBundle &bundle,
    const uint8_t requestedLevel,
    const double preferredU,
    const double preferredV,
    const double maximumViewDistanceMetres,
    WorldMapTerrainSpawnView &view,
    std::string *error,
    const bool keepPreferredSpawn)
{
  using procedural::world::CellType;
  using procedural::world::MapExportFieldSample;
  view = {};
  view.spawnU = std::clamp(preferredU, 0.0, 1.0);
  view.spawnV = std::clamp(preferredV, 0.0, 1.0);
  view.targetU = view.spawnU;
  view.targetV = view.spawnV;
  if (!(bundle.extentMetres > 0.0) ||
      !(maximumViewDistanceMetres > 0.0) ||
      requestedLevel > bundle.maximumPageLevel)
    return fail(error, "World-map startup view settings are invalid");

  const auto *refined = bundle.field("refined_elevation");
  const char *elevationName = refined != nullptr && refined->available
      ? "refined_elevation"
      : "base_elevation";
  const auto *elevation = bundle.field(elevationName);
  const auto *waterField = bundle.field("water_type");
  if (elevation == nullptr || !elevation->available ||
      waterField == nullptr || !waterField->available)
    return fail(error, "World-map startup view needs elevation and water_type");

  const uint32_t side = keepPreferredSpawn
      ? 65u
      : std::clamp(
          static_cast<uint32_t>(std::ceil(
              bundle.extentMetres /
              std::max(maximumViewDistanceMetres * 0.28, 1.0))),
          96u, 384u);
  const double localNormalizedRadius =
      maximumViewDistanceMetres / bundle.extentMetres;
  const auto sampleU = [&](const uint32_t x)
  {
    return keepPreferredSpawn
        ? std::clamp(
            preferredU - localNormalizedRadius +
                2.0 * localNormalizedRadius * x /
                    static_cast<double>(side - 1u),
            0.0, 1.0)
        : (static_cast<double>(x) + 0.5) / side;
  };
  const auto sampleV = [&](const uint32_t z)
  {
    return keepPreferredSpawn
        ? std::clamp(
            preferredV - localNormalizedRadius +
                2.0 * localNormalizedRadius * z /
                    static_cast<double>(side - 1u),
            0.0, 1.0)
        : (static_cast<double>(z) + 0.5) / side;
  };
  struct Sample
  {
    double elevation = 0.0;
    double relief = 0.0;
    double roughness = 0.0;
    double uplift = 0.0;
    double riverDistance = -1.0;
    double groundcover = 0.0;
    uint8_t water = 0u;
    CellType landform = CellType::Hill;
  };
  std::vector<Sample> samples(static_cast<size_t>(side) * side);
  const auto *reliefField = bundle.field("relief_roughness");
  const auto *upliftField = bundle.field("uplift");
  const auto *riverField = bundle.field("river");
  const auto *placementField = bundle.field("placement_potential");
  const auto *landformField = bundle.field("landform_id");
  const auto sampleOptional = [&](const char *name,
                                  const procedural::world::MapExportField *field,
                                  const double u, const double v,
                                  MapExportFieldSample &sample)
  {
    return field == nullptr || !field->available ||
        procedural::world::sampleMapExportField(
            bundle, name, u, v, requestedLevel, sample, error);
  };
  for (uint32_t z = 0u; z < side; ++z)
    for (uint32_t x = 0u; x < side; ++x)
    {
      const double u = sampleU(x);
      const double v = sampleV(z);
      MapExportFieldSample elevationSample{};
      MapExportFieldSample water{};
      MapExportFieldSample relief{};
      MapExportFieldSample uplift{};
      MapExportFieldSample river{};
      MapExportFieldSample placement{};
      MapExportFieldSample landform{};
      if (!procedural::world::sampleMapExportField(
              bundle, elevationName, u, v, requestedLevel,
              elevationSample, error) ||
          !procedural::world::sampleMapExportField(
              bundle, "water_type", u, v, requestedLevel, water, error) ||
          !sampleOptional("relief_roughness", reliefField, u, v, relief) ||
          !sampleOptional("uplift", upliftField, u, v, uplift) ||
          !sampleOptional("river", riverField, u, v, river) ||
          !sampleOptional("placement_potential", placementField,
              u, v, placement) ||
          !sampleOptional("landform_id", landformField, u, v, landform))
        return false;
      Sample &sample = samples[static_cast<size_t>(z) * side + x];
      sample.elevation = elevationSample.values[0];
      sample.water = static_cast<uint8_t>(std::clamp(
          std::llround(water.values[0]), 0ll, 2ll));
      sample.relief = relief.values[0];
      sample.roughness = relief.values[1];
      sample.uplift = uplift.values[0];
      sample.riverDistance = riverField != nullptr && riverField->available
          ? river.values[0]
          : -1.0;
      sample.groundcover = placement.values[2];
      if (landformField != nullptr && landformField->available)
      {
        const auto value = static_cast<uint8_t>(std::clamp(
            std::llround(landform.values[0]), 0ll,
            static_cast<long long>(CellType::Count) - 1ll));
        sample.landform = static_cast<CellType>(value);
      }
    }

  const double spacing = keepPreferredSpawn
      ? maximumViewDistanceMetres * 2.0 /
          static_cast<double>(side - 1u)
      : bundle.extentMetres / side;
  const auto at = [&](const uint32_t x, const uint32_t z) -> const Sample &
  {
    return samples[static_cast<size_t>(z) * side + x];
  };
  const auto localRelief = [&](const uint32_t x, const uint32_t z)
  {
    const uint32_t x0 = x == 0u ? x : x - 1u;
    const uint32_t x1 = std::min(x + 1u, side - 1u);
    const uint32_t z0 = z == 0u ? z : z - 1u;
    const uint32_t z1 = std::min(z + 1u, side - 1u);
    const double minimum = std::min({at(x, z).elevation,
        at(x0, z).elevation, at(x1, z).elevation,
        at(x, z0).elevation, at(x, z1).elevation});
    const double maximum = std::max({at(x, z).elevation,
        at(x0, z).elevation, at(x1, z).elevation,
        at(x, z0).elevation, at(x, z1).elevation});
    return maximum - minimum;
  };

  std::vector<uint32_t> targets;
  targets.reserve(side * side);
  double minimumLandElevation = std::numeric_limits<double>::max();
  double maximumLandElevation = std::numeric_limits<double>::lowest();
  for (const Sample &sample : samples)
    if (sample.water == 0u)
    {
      minimumLandElevation = std::min(
          minimumLandElevation, sample.elevation);
      maximumLandElevation = std::max(
          maximumLandElevation, sample.elevation);
    }
  const double highLandThreshold = std::isfinite(minimumLandElevation) &&
          std::isfinite(maximumLandElevation)
      ? minimumLandElevation +
          (maximumLandElevation - minimumLandElevation) * 0.68
      : std::numeric_limits<double>::max();
  for (uint32_t z = 0u; z < side; ++z)
    for (uint32_t x = 0u; x < side; ++x)
    {
      const Sample &sample = at(x, z);
      const double relief = std::max(localRelief(x, z), sample.relief);
      const bool mountain = sample.landform == CellType::Mountain ||
          sample.landform == CellType::Cliff ||
          sample.landform == CellType::Plateau;
      if (sample.water == 0u &&
          (mountain || relief >= 90.0 ||
           sample.elevation >= highLandThreshold))
        targets.push_back(z * side + x);
    }
  std::sort(targets.begin(), targets.end(), [&](const uint32_t a,
                                                const uint32_t b)
  {
    const uint32_t ax = a % side;
    const uint32_t az = a / side;
    const uint32_t bx = b % side;
    const uint32_t bz = b / side;
    const Sample &as = at(ax, az);
    const Sample &bs = at(bx, bz);
    const double aScore = as.elevation + std::max(as.relief,
        localRelief(ax, az)) * 2.0 + std::abs(as.uplift) * 400000.0;
    const double bScore = bs.elevation + std::max(bs.relief,
        localRelief(bx, bz)) * 2.0 + std::abs(bs.uplift) * 400000.0;
    return aScore > bScore;
  });
  if (targets.size() > 256u)
    targets.resize(256u);

  const double minimumViewDistance = std::max(spacing * 1.5,
      maximumViewDistanceMetres * 0.22);
  const double maximumDistance = maximumViewDistanceMetres * 0.92;
  double bestScore = std::numeric_limits<double>::lowest();
  const uint32_t preferredX = keepPreferredSpawn
      ? side / 2u
      : static_cast<uint32_t>(std::clamp(
          std::floor(std::clamp(preferredU, 0.0, 1.0) * side),
          0.0, static_cast<double>(side - 1u)));
  const uint32_t preferredZ = keepPreferredSpawn
      ? side / 2u
      : static_cast<uint32_t>(std::clamp(
          std::floor(std::clamp(preferredV, 0.0, 1.0) * side),
          0.0, static_cast<double>(side - 1u)));
  const uint32_t spawnZBegin = keepPreferredSpawn ? preferredZ : 0u;
  const uint32_t spawnZEnd = keepPreferredSpawn ? preferredZ + 1u : side;
  const uint32_t spawnXBegin = keepPreferredSpawn ? preferredX : 0u;
  const uint32_t spawnXEnd = keepPreferredSpawn ? preferredX + 1u : side;
  for (uint32_t sz = spawnZBegin; sz < spawnZEnd; ++sz)
    for (uint32_t sx = spawnXBegin; sx < spawnXEnd; ++sx)
    {
      const Sample &spawn = at(sx, sz);
      const double spawnRelief = localRelief(sx, sz);
      const double spawnSlope = spawnRelief / std::max(spacing, 1.0);
      if (!keepPreferredSpawn &&
          (spawn.water != 0u || spawnSlope > 0.22 ||
           (spawn.riverDistance >= 0.0 && spawn.riverDistance < 80.0)))
        continue;
      const double spawnU = keepPreferredSpawn
          ? std::clamp(preferredU, 0.0, 1.0)
          : sampleU(sx);
      const double spawnV = keepPreferredSpawn
          ? std::clamp(preferredV, 0.0, 1.0)
          : sampleV(sz);
      for (const uint32_t targetIndex : targets)
      {
        const uint32_t tx = targetIndex % side;
        const uint32_t tz = targetIndex / side;
        const Sample &target = at(tx, tz);
        const double targetU = sampleU(tx);
        const double targetV = sampleV(tz);
        const double dx = static_cast<double>(tx) - sx;
        const double dz = static_cast<double>(tz) - sz;
        const double distance = std::hypot(
            targetU - spawnU, targetV - spawnV) * bundle.extentMetres;
        const double rise = target.elevation - spawn.elevation;
        if (distance < minimumViewDistance || distance > maximumDistance ||
            rise < 80.0)
          continue;
        const double targetAngle = std::atan2(rise, distance);
        double blockingAngle = -1.5707963267948966;
        const uint32_t raySteps = std::max(2u, static_cast<uint32_t>(
            std::ceil(std::hypot(dx, dz))));
        for (uint32_t step = 1u; step < raySteps; ++step)
        {
          const double t = static_cast<double>(step) / raySteps;
          const uint32_t rx = static_cast<uint32_t>(std::clamp(
              std::llround(static_cast<double>(sx) + dx * t),
              0ll, static_cast<long long>(side - 1u)));
          const uint32_t rz = static_cast<uint32_t>(std::clamp(
              std::llround(static_cast<double>(sz) + dz * t),
              0ll, static_cast<long long>(side - 1u)));
          const double rayDistance = distance * t;
          blockingAngle = std::max(blockingAngle, std::atan2(
              at(rx, rz).elevation - spawn.elevation,
              std::max(rayDistance, 1.0)));
        }
        const double skylineMargin = targetAngle - blockingAngle;
        if (skylineMargin < 0.0025)
          continue;
        const double centerDistance = std::hypot(
            spawnU - std::clamp(preferredU, 0.0, 1.0),
            spawnV - std::clamp(preferredV, 0.0, 1.0));
        const bool mountain = target.landform == CellType::Mountain ||
            target.landform == CellType::Cliff;
        const double score = rise * 2.0 +
            std::max(target.relief, localRelief(tx, tz)) * 1.5 +
            targetAngle * 2500.0 + skylineMargin * 5000.0 +
            (mountain ? 350.0 : 0.0) +
            std::clamp(spawn.groundcover, 0.0, 1.0) * 80.0 -
            spawnRelief * 1.5 - centerDistance * 180.0;
        if (score <= bestScore)
          continue;
        bestScore = score;
        view.found = true;
        view.spawnU = spawnU;
        view.spawnV = spawnV;
        view.targetU = targetU;
        view.targetV = targetV;
        view.spawnElevationMetres = spawn.elevation;
        view.targetElevationMetres = target.elevation;
        view.score = score;
      }
    }
  return true;
}

double proceduralWorldMapTerrainDetail(
    const procedural::Point2 &position,
    const procedural::world::MapExportBundle &bundle,
    const double authoredReliefMetres,
    const double authoredRoughness,
    const procedural::world::CellType landform,
    const double strength,
    const double riverDistanceMetres)
{
  if (!(strength > 0.0))
    return 0.0;
  const double roughness = std::clamp(authoredRoughness, 0.0, 1.0);
  // Every octave shares this unsaturated authored amplitude. The 20 m floor
  // prevents perfectly flat exports from becoming numerically featureless,
  // while 30 m and 550 m cells retain their authored 18.3x contrast.
  const double reliefAmplitude =
      std::max(std::abs(authoredReliefMetres), 20.0) / 600.0;
  const double amplitude = reliefAmplitude * (0.8 + 0.4 * roughness);
  const uint64_t seed = mixNoise(
      bundle.seed ^ bundle.bundleSignature ^ 0x6a09e667f3bcc909ull);
  const auto noise = [&](const double wavelength, const uint64_t octave)
  {
    return smoothNoise2D(
        position, wavelength,
        mixNoise(seed + octave * 0x9e3779b97f4a7c15ull));
  };
  const auto ridged = [&](const double value, const double power)
  {
    return std::pow(
               std::clamp(1.0 - std::abs(value), 0.0, 1.0), power) -
        0.28;
  };

  const double broadNoise = noise(2400.0, 0u);
  const double provinceNoise = noise(1500.0, 1u);
  const double middleNoise = noise(1050.0, 2u);
  const double hillNoise = noise(720.0, 3u);
  const double ridgeNoise = noise(420.0, 4u);
  const double shoulderNoise = noise(220.0, 5u);
  double broad = broadNoise * 24.0;
  double middle = provinceNoise * 10.0 + middleNoise * 5.0;
  double hill = hillNoise * 11.0;
  double ridge = ridged(ridgeNoise, 3.5) * 8.0;
  double shoulder = shoulderNoise * 4.0;

  const auto smoothShelf = [](const double value) {
    const double lower = std::floor(value);
    const double t = value - lower;
    return lower + t*t*(3.0-2.0*t);
  };
  using procedural::world::CellType;
  switch (landform)
  {
  case CellType::Mountain:
    broad *= 1.15;
    middle = ridged(provinceNoise, 2.2) * 24.0 +
        ridged(middleNoise, 2.8) * 13.0;
    ridge = ridged(ridgeNoise, 2.5) * 13.0;
    break;
  case CellType::Plateau:
  {
    const double terraceSource =
        provinceNoise * 0.65 + hillNoise * 0.35;
    middle = smoothShelf(terraceSource * 5.0) * (18.0 / 5.0);
    hill *= 0.35;
    ridge *= 0.30;
    shoulder *= 0.65;
    break;
  }
  case CellType::Cliff:
  {
    const double shelfSource =
        provinceNoise * 0.70 + middleNoise * 0.30;
    broad *= 0.85;
    middle = smoothShelf(shelfSource * 4.0) * (40.0 / 4.0);
    hill *= 0.22;
    ridge = ridged(ridgeNoise, 2.4) * 11.0;
    shoulder *= 0.45;
    break;
  }
  case CellType::Plain:
    broad = broadNoise * 14.0;
    middle = provinceNoise * 5.0 + middleNoise * 2.0;
    hill *= 0.18;
    ridge = 0.0;
    shoulder *= 0.15;
    break;
  case CellType::Valley:
  {
    const double channelAxis = std::clamp(
        provinceNoise + middleNoise * 0.35, -1.0, 1.0);
    const double channel = -std::pow(
        std::clamp(1.0 - std::abs(channelAxis), 0.0, 1.0), 3.0) * 26.0;
    broad *= 0.75;
    middle = provinceNoise * 6.0 + channel;
    hill *= 0.30;
    ridge *= 0.20;
    break;
  }
  case CellType::Canyon:
  {
    double channelMask = 0.0;
    if (riverDistanceMetres >= 0.0)
    {
      const double halfWidth = std::clamp(
          std::max(std::abs(authoredReliefMetres), 20.0) * 1.5,
          180.0, 1200.0);
      const double edge = std::clamp(
          (halfWidth - riverDistanceMetres) / (halfWidth * 0.22),
          0.0, 1.0);
      channelMask = edge * edge * (3.0 - 2.0 * edge);
    }
    else
    {
      const double channelAxis = std::clamp(
          provinceNoise + middleNoise * 0.35, -1.0, 1.0);
      channelMask = std::pow(
          std::clamp(1.0 - std::abs(channelAxis), 0.0, 1.0), 6.0);
    }
    broad *= 0.65;
    middle = provinceNoise * 5.0 - channelMask * 90.0;
    hill *= 0.18;
    ridge *= 0.30;
    shoulder *= 0.35;
    break;
  }
  case CellType::Hill:
    middle = provinceNoise * 12.0 + middleNoise * 7.0;
    hill *= 1.25;
    break;
  default:
    break;
  }

  const double ground = noise(48.0, 6u) * 1.4;
  const double fine = noise(14.0, 7u) * 0.32;
  return strength * amplitude *
      (broad + middle + hill + ridge + shoulder + ground + fine);
}

double proceduralWorldMapTerrainHeight(
    const procedural::Point2 &position,
    const procedural::world::MapExportBundle &bundle,
    const double authoredElevationMetres,
    const double authoredReliefMetres,
    const double authoredRoughness,
    const procedural::world::CellType landform,
    const double detailStrength,
    const double terraceStrength,
    const double terraceHeightMetres,
    const double terraceTransitionFraction,
    const double riverDistanceMetres,
    const double infrastructureSmoothingMask,
    const WorldMapTerrainSignals &signals)
{
  const double flow = std::clamp(signals.erosionFlow, 0.0, 1.0);
  const double wear = std::clamp(signals.erosionWear, 0.0, 1.0);
  const double deposition = std::clamp(
      signals.erosionDeposition, 0.0, 1.0);
  const double uplift = std::clamp(
      std::abs(signals.upliftMetresPerYear) / 0.001, 0.0, 1.0);
  double channelIncision = 0.0;
  if (riverDistanceMetres >= 0.0 &&
      signals.riverDischargeCubicMetresPerSecond > 0.0)
  {
    const double discharge =
        signals.riverDischargeCubicMetresPerSecond;
    const double halfWidth = std::clamp(
        5.0 + std::sqrt(discharge) * 8.0, 5.0, 120.0);
    const double t = std::clamp(
        1.0 - riverDistanceMetres / halfWidth, 0.0, 1.0);
    const double channelMask = t * t * (3.0 - 2.0 * t);
    channelIncision = channelMask * std::clamp(
        std::log1p(discharge) * 1.8 + flow * 4.0, 0.0, 12.0);
  }
  const double authoredSurface = authoredElevationMetres - channelIncision;
  if (!(detailStrength > 0.0))
    return authoredSurface;
  const double coastMask = signals.coastDistanceMetres < 0.0
      ? 0.0
      : std::clamp(
          1.0 - std::abs(signals.coastDistanceMetres) / 320.0,
          0.0, 1.0);
  const double signalDetailStrength = detailStrength *
      (1.0 + uplift * 0.35) *
      (1.0 - wear * 0.15) *
      (1.0 - flow * 0.45) *
      (1.0 - deposition * 0.35) *
      (1.0 - coastMask * 0.65);
  const double rawDetail = proceduralWorldMapTerrainDetail(position, bundle,
          authoredReliefMetres, authoredRoughness, landform,
          signalDetailStrength, riverDistanceMetres);
  const double riverShoulder = std::max(180.0, terraceHeightMetres * 16.0);
  const double riverT = riverDistanceMetres < 0.0
      ? 0.0
      : std::clamp(1.0 - riverDistanceMetres / riverShoulder, 0.0, 1.0);
  const double riverSmoothingMask =
      riverT * riverT * (3.0 - 2.0 * riverT);
  const double infrastructureMask =
      std::clamp(infrastructureSmoothingMask, 0.0, 1.0);
  const double corridorMask = std::max(
      infrastructureMask, riverSmoothingMask);
  const double detailSuppression = std::max(
      infrastructureMask, riverSmoothingMask * 0.90);
  const double detailed = authoredSurface +
      rawDetail * (1.0 - detailSuppression);
  if (!(terraceStrength > 0.0) || !(terraceHeightMetres > 0.0))
    return detailed;

  const uint64_t seed = mixNoise(
      bundle.seed ^ bundle.bundleSignature ^ 0x510e527fade682d1ull);
  const double warpWavelength = std::max(4800.0, terraceHeightMetres * 220.0);
  const double warpAmplitude = std::max(700.0, terraceHeightMetres * 42.0);
  const procedural::Point2 warped{
    position.x + smoothNoise2D(position, warpWavelength,
                     mixNoise(seed ^ 0x1f83d9abfb41bd6bull)) *
        warpAmplitude,
    position.y + smoothNoise2D(position, warpWavelength,
                     mixNoise(seed ^ 0x5be0cd19137e2179ull)) *
        warpAmplitude};
  const double bandOffset = smoothNoise2D(
      warped, warpWavelength * 0.55,
      mixNoise(seed ^ 0xa54ff53a5f1d36f1ull)) *
      terraceHeightMetres * 0.48;
  const double benchNoise = smoothNoise2D(
      warped, warpWavelength * 1.35,
      mixNoise(seed ^ 0x9b05688c2b3e6c1full));
  const double benchT = std::clamp((benchNoise + 0.15) / 0.55, 0.0, 1.0);
  const double benchMask = benchT * benchT * (3.0 - 2.0 * benchT);

  using procedural::world::CellType;
  double landformWeight = 0.0;
  switch (landform)
  {
  case CellType::Plateau:
  case CellType::Cliff:
    landformWeight = 1.0;
    break;
  case CellType::Canyon:
    landformWeight = 0.78 + benchMask * 0.14;
    break;
  case CellType::Hill:
    landformWeight = 0.50 + benchMask * 0.28;
    break;
  case CellType::Mountain:
    landformWeight = 0.32 + benchMask * 0.24;
    break;
  case CellType::Valley:
    landformWeight = 0.10;
    break;
  default:
    break;
  }
  const double blend = std::clamp(
      terraceStrength * landformWeight * (1.0 - corridorMask), 0.0, 1.0);
  if (!(blend > 0.0))
    return detailed;

  // Only the broad authored surface chooses a height band. Feeding metre-scale
  // detail into the quantizer lets a 14 m octave cross a complete terrace in a
  // tiny runtime page, creating a false wall and exhausting its vertical
  // budget. Fine relief is restored as a restrained residual below.
  const double terraceDatum = signals.hasBaseElevation
      ? signals.baseElevationMetres
      : authoredElevationMetres;
  const double conditioningResidual = authoredSurface - terraceDatum;
  const double shifted = terraceDatum + bandOffset;
  const double scaled = shifted / terraceHeightMetres;
  const double lower = std::floor(scaled);
  const double phase = scaled - lower;
  const double transition = std::clamp(
      terraceTransitionFraction, 0.0, 0.49);
  double riser = 0.0;
  if (transition > 0.0)
  {
    const double t = std::clamp(
        (phase - (1.0 - transition * 2.0)) / (transition * 2.0),
        0.0, 1.0);
    riser = t * t * (3.0 - 2.0 * t);
  }
  double residualDetail = 0.70;
  switch (landform)
  {
  case CellType::Plateau:
    residualDetail = 0.12;
    break;
  case CellType::Cliff:
    residualDetail = 0.18;
    break;
  case CellType::Canyon:
    residualDetail = 0.22;
    break;
  case CellType::Hill:
    residualDetail = 0.35;
    break;
  case CellType::Mountain:
    residualDetail = 0.50;
    break;
  default:
    break;
  }
  const double terraced =
      (lower + riser) * terraceHeightMetres +
      conditioningResidual +
      (detailed - authoredSurface) * residualDetail;
  return detailed + (terraced - detailed) * blend;
}

TerrainVirtualPagePayload terrainVirtualPagePayloadFromChunkSet(
    const PlanetVoxelChunkSet &page)
{
  TerrainVirtualPagePayload payload{};
  payload.centerTile = page.centerTile;
  payload.edgeMetres = page.edgeMetres;
  payload.voxelSizeMetres = page.voxelSizeMetres;
  payload.elevationOriginMetres = page.elevationOriginMetres;
  payload.centerSurfaceMetres = page.centerSurfaceMetres;
  payload.centerMaterial = page.centerMaterial;
  payload.leafVoxelCount = page.leafVoxelCount;
  payload.payloadRevision = page.clipmapLayoutRevision;
  payload.surfaceHeightSide = page.surfaceHeightSide;
  payload.surfaceHeightHalo = page.surfaceHeightHalo;
  payload.surfaceHeights = page.surfaceHeights;
  payload.surfaceEnvelope = page.surfaceEnvelope;
  payload.surfaceMaterialSide = page.surfaceMaterialSide;
  payload.surfaceMaterials = page.surfaceMaterials;
  payload.materialFieldRegions = page.materialFieldRegions;
  payload.minimumWorldMetres = page.minimumWorldMetres;
  payload.maximumWorldMetres = page.maximumWorldMetres;
  payload.bricks = page.bricks;
  payload.records.reserve(page.chunks.size());

  const bool singlePagePayload = page.chunks.size() == 1u;
  for (const auto &record : page.chunks)
  {
    TerrainVirtualPageRecord out{};
    out.requestedTile = record.requestedTile;
    out.sourceTile = record.sourceTile;
    out.tileOffsetX = record.tileOffsetX;
    out.tileOffsetY = record.tileOffsetY;
    out.clipmapLOD = record.clipmapLOD;
    out.regionMappingId = record.regionMappingId;
    out.measuredErrorMetres = record.measuredErrorMetres;
    out.worldCenterXMetres = record.worldCenterXMetres;
    out.worldCenterZMetres = record.worldCenterZMetres;
    out.edgeMetres = record.edgeMetres;
    out.voxelSizeMetres = record.voxelSizeMetres;
    if (singlePagePayload)
    {
      out.localBrickNodeOffset = 0u;
      out.localBrickNodeCount =
          static_cast<uint32_t>(payload.bricks.nodes.size());
      out.localBrickChildOffset = 0u;
      out.localBrickChildCount =
          static_cast<uint32_t>(payload.bricks.childIndices.size());
      out.localBrickMaterialOffset = 0u;
      out.localBrickMaterialCount =
          static_cast<uint32_t>(payload.bricks.materialIndices.size());
      out.localBrickPaletteOffset = 0u;
      out.localBrickPaletteCount =
          static_cast<uint32_t>(payload.bricks.palette.size());
      out.localFirstBrickRoot = 0u;
      out.localBrickRootCount =
          static_cast<uint32_t>(payload.bricks.rootIndices.size());
    }
    else
    {
      out.localBrickNodeOffset = record.brickNodeOffset;
      out.localBrickNodeCount = record.brickNodeCapacity;
      out.localBrickChildOffset = record.brickChildOffset;
      out.localBrickChildCount = record.brickChildCapacity;
      out.localBrickMaterialOffset = record.brickMaterialOffset;
      out.localBrickMaterialCount = record.brickMaterialCapacity;
      out.localBrickPaletteOffset = record.brickPaletteOffset;
      out.localBrickPaletteCount = record.brickPaletteCapacity;
      out.localFirstBrickRoot = record.firstBrickRoot;
      out.localBrickRootCount = record.brickRootCount;
    }
    out.brickPayloadRevision = record.brickPayloadRevision;
    out.brickNodeRevision = record.brickNodeRevision;
    out.tree64Revision = record.tree64Revision;
    out.elevationOriginMetres = record.elevationOriginMetres;
    out.minimumSurfaceMetres = record.minimumSurfaceMetres;
    out.maximumSurfaceMetres = record.maximumSurfaceMetres;
    out.centerSurfaceMetres = record.centerSurfaceMetres;
    out.centerMaterial = record.centerMaterial;
    out.leafVoxelCount = record.leafVoxelCount;
    out.brickCount = record.brickCount;
    payload.records.push_back(out);
  }

  return payload;
}

PlanetVoxelChunkSet terrainVirtualPagePayloadToChunkSet(
    const TerrainVirtualPagePayload &payload)
{
  PlanetVoxelChunkSet page{};
  page.centerTile = payload.centerTile;
  page.bricks = payload.bricks;
  page.edgeMetres = payload.edgeMetres;
  page.voxelSizeMetres = payload.voxelSizeMetres;
  page.elevationOriginMetres = payload.elevationOriginMetres;
  page.centerSurfaceMetres = payload.centerSurfaceMetres;
  page.centerMaterial = payload.centerMaterial;
  page.leafVoxelCount = payload.leafVoxelCount;
  page.clipmapLayoutRevision = payload.payloadRevision;
  page.surfaceHeightSide = payload.surfaceHeightSide;
  page.surfaceHeightHalo = payload.surfaceHeightHalo;
  page.surfaceHeights = payload.surfaceHeights;
  page.surfaceEnvelope = payload.surfaceEnvelope;
  page.surfaceMaterialSide = payload.surfaceMaterialSide;
  page.surfaceMaterials = payload.surfaceMaterials;
  page.materialFieldRegions = payload.materialFieldRegions;
  page.minimumWorldMetres = payload.minimumWorldMetres;
  page.maximumWorldMetres = payload.maximumWorldMetres;
  page.chunks.reserve(payload.records.size());

  for (const auto &record : payload.records)
  {
    PlanetVoxelChunkRecord out{};
    out.requestedTile = record.requestedTile;
    out.sourceTile = record.sourceTile;
    out.tileOffsetX = record.tileOffsetX;
    out.tileOffsetY = record.tileOffsetY;
    out.clipmapLOD = record.clipmapLOD;
    out.regionMappingId = record.regionMappingId;
    out.measuredErrorMetres = record.measuredErrorMetres;
    out.worldCenterXMetres = record.worldCenterXMetres;
    out.worldCenterZMetres = record.worldCenterZMetres;
    out.edgeMetres = record.edgeMetres;
    out.voxelSizeMetres = record.voxelSizeMetres;
    out.brickNodeOffset = record.localBrickNodeOffset;
    out.brickNodeCapacity = record.localBrickNodeCount;
    out.brickChildOffset = record.localBrickChildOffset;
    out.brickChildCapacity = record.localBrickChildCount;
    out.brickMaterialOffset = record.localBrickMaterialOffset;
    out.brickMaterialCapacity = record.localBrickMaterialCount;
    out.brickPaletteOffset = record.localBrickPaletteOffset;
    out.brickPaletteCapacity = record.localBrickPaletteCount;
    out.brickPayloadRevision = record.brickPayloadRevision;
    out.brickNodeRevision = record.brickNodeRevision;
    out.tree64Revision = record.tree64Revision;
    out.firstBrickRoot = record.localFirstBrickRoot;
    out.brickRootCapacity = record.localBrickRootCount;
    out.brickRootCount = record.localBrickRootCount;
    out.elevationOriginMetres = record.elevationOriginMetres;
    out.minimumSurfaceMetres = record.minimumSurfaceMetres;
    out.maximumSurfaceMetres = record.maximumSurfaceMetres;
    out.centerSurfaceMetres = record.centerSurfaceMetres;
    out.centerMaterial = record.centerMaterial;
    out.leafVoxelCount = record.leafVoxelCount;
    out.brickCount = record.brickCount;
    page.chunks.push_back(out);
  }

  return page;
}

std::vector<uint8_t> serializeTerrainVirtualPagePayload(
    const TerrainVirtualPagePayload &payload)
{
  return serializeTerrainVirtualPagePayloadInternal(payload);
}

bool deserializeTerrainVirtualPagePayload(
    const std::vector<uint8_t> &bytes,
    TerrainVirtualPagePayload &payload,
    std::string *error)
{
  return deserializeTerrainVirtualPagePayloadInternal(bytes, payload, error);
}

// A generated page is a pure function of the source bundle, the generator
// settings that shape voxelization, and the page address. Anything that would
// change the produced bytes has to be folded in here, or a stale page is
// restored under a key that no longer describes it.
TransientSparseVoxelPageKey worldTransientPageKey(
    const uint64_t bundleIdentity,
    const WorldVoxelChunkStreamingSettings &settings,
    const WorldVoxelChunkAddress &address,
    const uint32_t internalLOD)
{
  uint64_t source = 1469598103934665603ull;
  hashValue(source, bundleIdentity);
  hashValue(source, settings.generatorRevision);
  hashValue(source, settings.chunkEdgeMetres);
  hashValue(source, settings.finestVoxelSizeMetres);
  hashValue(source, settings.finestHorizontalVoxelCount);
  hashValue(source, settings.lodCount);
  hashValue(source, settings.voxel.foundationDepthVoxels);
  hashValue(source, settings.voxel.surfaceShellDepthVoxels);
  hashValue(source, settings.voxel.maximumVerticalVoxels);
  hashValue(source, settings.voxel.surfaceEnvelopeRadius);
  hashValue(source, settings.voxel.maximumBricks);
  hashValue(source, settings.voxel.proceduralDetailStrength);
  hashValue(source, settings.voxel.proceduralTerraceStrength);
  hashValue(source, settings.voxel.proceduralTerraceHeightMetres);
  hashValue(source, settings.voxel.proceduralTerraceTransitionFraction);
  hashValue(source, settings.voxel.microGeometry);
  hashValue(source, settings.voxel.microMaterialRevision);
  uint64_t page = 1469598103934665603ull;
  hashValue(page, address.x);
  hashValue(page, address.z);
  hashValue(page, address.spatialLOD);
  hashValue(page, internalLOD);
  return {source, page};
}

uint32_t recommendedPlanetVoxelWorkerCount(const uint32_t hardwareThreadCount)
{
  const uint32_t available = std::max(1u, hardwareThreadCount);
  const uint32_t buildThreads = available > 2u ? available - 2u : available;
  return std::clamp(buildThreads, 1u, 8u);
}

bool samplePlanetVoxelSurfaceTop(
    const PlanetVoxelChunkSet &chunks,
    const double worldXMetres,
    const double worldZMetres,
    double &localTopMetres)
{
  localTopMetres = 0.0;
  if (!std::isfinite(worldXMetres) || !std::isfinite(worldZMetres) ||
      chunks.surfaceHeightSide == 0u ||
      chunks.surfaceHeightSide <= chunks.surfaceHeightHalo * 2u)
    return false;
  const size_t rasterSide = chunks.surfaceHeightSide;
  if (rasterSide > std::numeric_limits<size_t>::max() / rasterSide)
    return false;
  const size_t samplesPerChunk = rasterSide * rasterSide;
  if (chunks.chunks.size() >
          std::numeric_limits<size_t>::max() / samplesPerChunk ||
      chunks.surfaceHeights.size() <
          chunks.chunks.size() * samplesPerChunk)
    return false;

  const uint32_t columnSide = chunks.surfaceHeightSide -
      chunks.surfaceHeightHalo * 2u;
  bool found = false;
  double highestTop = std::numeric_limits<double>::lowest();
  for (size_t chunkIndex = 0u;
       chunkIndex < chunks.chunks.size(); ++chunkIndex)
  {
    const auto &record = chunks.chunks[chunkIndex];
    if (!(record.edgeMetres > 0.0) ||
        !(record.voxelSizeMetres > 0.0) ||
        !std::isfinite(record.edgeMetres) ||
        !std::isfinite(record.voxelSizeMetres))
      continue;
    const double halfEdge = record.edgeMetres * 0.5;
    const double minimumX = record.worldCenterXMetres - halfEdge;
    const double minimumZ = record.worldCenterZMetres - halfEdge;
    const double tolerance = std::max(record.edgeMetres * 1.0e-9, 1.0e-7);
    if (worldXMetres < minimumX - tolerance ||
        worldXMetres > minimumX + record.edgeMetres + tolerance ||
        worldZMetres < minimumZ - tolerance ||
        worldZMetres > minimumZ + record.edgeMetres + tolerance)
      continue;

    const double localX = std::clamp(
        (worldXMetres - minimumX) / record.edgeMetres,
        0.0, std::nextafter(1.0, 0.0));
    const double localZ = std::clamp(
        (worldZMetres - minimumZ) / record.edgeMetres,
        0.0, std::nextafter(1.0, 0.0));
    const uint32_t columnX = std::min(
        static_cast<uint32_t>(std::floor(localX * columnSide)),
        columnSide - 1u);
    const uint32_t columnZ = std::min(
        static_cast<uint32_t>(std::floor(localZ * columnSide)),
        columnSide - 1u);
    const uint32_t sampleX = columnX + chunks.surfaceHeightHalo;
    const uint32_t sampleZ = columnZ + chunks.surfaceHeightHalo;
    const size_t sampleIndex = chunkIndex * samplesPerChunk +
        static_cast<size_t>(sampleZ) * rasterSide + sampleX;
    const double height = chunks.surfaceHeights[sampleIndex];
    if (!std::isfinite(height))
      continue;
    const double absoluteTop = record.elevationOriginMetres +
        (std::floor(
             (height - record.elevationOriginMetres) /
             record.voxelSizeMetres) +
         1.0) *
            record.voxelSizeMetres;
    highestTop = std::max(
        highestTop, absoluteTop - chunks.elevationOriginMetres);
    found = true;
  }
  if (found)
    localTopMetres = highestTop;
  return found;
}

uint32_t planetVoxelClipmapLevelCountForDistance(const double finePageEdgeMetres, const uint32_t chunkRadius, const double requestedDistanceMetres)
{
  if (!(finePageEdgeMetres > 0.0) || !std::isfinite(finePageEdgeMetres) || !(requestedDistanceMetres > 0.0) || !std::isfinite(requestedDistanceMetres))
    return 0u;
  if (chunkRadius == 0u)
    return 1u;
  for (uint32_t level = 0u; level < kPlanetVoxelMaximumClipmapLevels; ++level)
  {
    // The camera may sit at either edge of the ancestor tile that centres a
    // coarse ring. Only the complete pages beyond that ancestor are guaranteed
    // in every alignment, hence radius * pageEdge rather than radius + 0.5.
    const double halfExtent = static_cast<double>(chunkRadius) * std::ldexp(finePageEdgeMetres, static_cast<int>(level));
    if (halfExtent >= requestedDistanceMetres)
      return level + 1u;
  }
  return kPlanetVoxelMaximumClipmapLevels;
}

SparseVoxelMaterialGPU planetVoxelBaseMaterial(const uint8_t material)
{
  return makePlanetVoxelBaseMaterial(material);
}

SparseVoxelMaterialGPU planetVoxelSurfaceMaterial(const uint8_t material, const procedural::SpherePoint &direction, const procedural::PlanetPackageMetadata &metadata, const double slope, const double mountain)
{
  return makePlanetVoxelSurfaceMaterial(material, direction, metadata, slope, mountain);
}

double planetVoxelTerrainDetail(
    const procedural::SpherePoint &direction,
    const procedural::PlanetPackageMetadata &metadata,
    const double mountain,
    const double tectonics,
    const uint8_t material,
    const double strength,
    const uint8_t lithology)
{
  return proceduralTerrainDetail(direction, metadata, mountain, tectonics, material, lithology, strength);
}

uint32_t planetVoxelMaterialRegionMappingId(const procedural::PlanetTileAddress &tile)
{
  if (!procedural::validPlanetTileAddress(tile))
    return 0u;
  uint32_t value = 0x811c9dc5u;
  const auto append = [&](const uint32_t component)
  {
    value ^= component;
    value *= 0x01000193u;
    value ^= value >> 16u;
  };
  append(tile.face);
  append(tile.level);
  append(tile.x);
  append(tile.y);
  // Zero is reserved for material-global mappings.
  return value == 0u ? 1u : value;
}

bool generatePlanetVoxelMaterialFieldRegion(
    const procedural::PlanetPackageMetadata &metadata,
    const procedural::PlanetTileAddress &tile,
    const std::array<int32_t, 2> &canonicalParentOrigin,
    const std::array<uint32_t, 2> &canonicalParentExtent,
    PlanetVoxelMaterialFieldRegion &region,
    std::string *error)
{
  region = {};
  if (error != nullptr)
    error->clear();
  if (!procedural::validPlanetTileAddress(tile) || !(metadata.planetRadiusMetres > 0.0) || !std::isfinite(metadata.planetRadiusMetres) || canonicalParentExtent[0] == 0u || canonicalParentExtent[1] == 0u)
    return fail(error, "Planet material-field region inputs are invalid");

  region.tile = tile;
  region.regionMappingId = planetVoxelMaterialRegionMappingId(tile);
  region.canonicalParentOrigin = canonicalParentOrigin;
  region.canonicalParentExtent = canonicalParentExtent;
  constexpr uint32_t side = voxel::material::kVoxelMaterialFieldAtlasTileSide;
  for (auto &values : region.values)
    values.resize(static_cast<size_t>(side) * side);
  const uint64_t grassSeed = metadata.contentHash ^ 0x3c6ef372fe94f82bull;
  const uint64_t variationSeed = metadata.contentHash ^ 0xa54ff53a5f1d36f1ull;
  const double dimension = std::ldexp(1.0, tile.level);
  const auto smooth01 = [](const double value)
  {
    const double t = std::clamp(value, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
  };
  for (uint32_t y = 0u; y < side; ++y)
    for (uint32_t x = 0u; x < side; ++x)
    {
      // End-point sampling makes both texel borders exact shared samples.
      // Because noise is evaluated in planet space, a parent and any child
      // also agree wherever their sample positions coincide.
      const double localU = static_cast<double>(x) / (side - 1u);
      const double localV = static_cast<double>(y) / (side - 1u);
      const procedural::PlanetCubeCoordinate coordinate{static_cast<procedural::PlanetCubeFace>(tile.face), (static_cast<double>(tile.x) + localU) / dimension, (static_cast<double>(tile.y) + localV) / dimension};
      procedural::SpherePoint direction{};
      if (!procedural::planetCubeDirection(coordinate, direction))
        return fail(error, "Could not map a material-field texel onto the planet");
      const double grassBroad = smoothNoise3D(direction, metadata.planetRadiusMetres, 110.0, grassSeed);
      const double grassFine = smoothNoise3D(direction, metadata.planetRadiusMetres, 27.0, mixNoise(grassSeed + 0x9e3779b97f4a7c15ull));
      const double grass = smooth01(0.72 + grassBroad * 0.24 + grassFine * 0.16);
      const double variationBroad = smoothNoise3D(direction, metadata.planetRadiusMetres, 61.0, variationSeed);
      const double variationFine = smoothNoise3D(direction, metadata.planetRadiusMetres, 13.0, mixNoise(variationSeed + 0xd1b54a32d192ed03ull));
      const double variation = std::clamp(0.58 + variationBroad * 0.27 + variationFine * 0.15, 0.0, 1.0);
      const size_t texel = static_cast<size_t>(y) * side + x;
      region.values[static_cast<size_t>(PlanetVoxelGeneratedMaterialField::Grass)][texel] = static_cast<uint8_t>(std::lround(grass * 255.0));
      region.values[static_cast<size_t>(PlanetVoxelGeneratedMaterialField::Dirt)][texel] = static_cast<uint8_t>(std::lround((1.0 - grass) * 255.0));
      region.values[static_cast<size_t>(PlanetVoxelGeneratedMaterialField::Variation)][texel] = static_cast<uint8_t>(std::lround(variation * 255.0));
    }
  return true;
}

bool populatePlanetVoxelMaterialFieldAtlas(const PlanetVoxelChunkSet &chunks, const voxel::material::VoxelMaterialManager &materials, voxel::material::VoxelMaterialFieldAtlas &atlas, std::string *error)
{
  if (error != nullptr)
    error->clear();
  atlas.clearPlanetRegions();
  std::map<uint32_t, procedural::PlanetTileAddress> regionIdentities;
  for (const auto &region : chunks.materialFieldRegions)
  {
    const auto [identity, inserted] = regionIdentities.emplace(region.regionMappingId, region.tile);
    if (!inserted && !(identity->second == region.tile))
      return fail(error, "Planet material-field region ID collision");
    for (uint32_t handle = 1u; handle < materials.slotCount(); ++handle)
    {
      const auto materialHandle = static_cast<voxel::material::VoxelMaterialHandle>(handle);
      const auto &slot = materials.slot(materialHandle);
      if (!slot.occupied)
        continue;
      const uint32_t subdivision = std::max(unpackSparseVoxelMicroGeometry(slot.descriptor).subdivision, 1u);
      std::array<int32_t, 2> microOrigin{};
      std::array<uint32_t, 2> microExtent{};
      for (uint32_t axis = 0u; axis < 2u; ++axis)
      {
        const int64_t origin = static_cast<int64_t>(region.canonicalParentOrigin[axis]) * subdivision;
        const uint64_t extent = static_cast<uint64_t>(region.canonicalParentExtent[axis]) * subdivision;
        if (origin < std::numeric_limits<int32_t>::min() || origin > std::numeric_limits<int32_t>::max() || extent == 0u || extent > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
          return fail(error, "Planet material-field canonical range exceeds 32 bits");
        microOrigin[axis] = static_cast<int32_t>(origin);
        microExtent[axis] = static_cast<uint32_t>(extent);
      }
      for (uint32_t fieldSlot = 0u; fieldSlot < voxel::material::kVoxelMaterialFieldCount; ++fieldSlot)
      {
        const auto &binding = slot.fieldBindings[fieldSlot];
        if (binding.source != voxel::material::VoxelMaterialFieldSource::PlanetRegion)
          continue;
        size_t generated = 0u;
        switch (binding.semantic)
        {
        case voxel::material::VoxelMaterialFieldSemantic::Grass:
          generated = static_cast<size_t>(PlanetVoxelGeneratedMaterialField::Grass);
          break;
        case voxel::material::VoxelMaterialFieldSemantic::Dirt:
          generated = static_cast<size_t>(PlanetVoxelGeneratedMaterialField::Dirt);
          break;
        case voxel::material::VoxelMaterialFieldSemantic::Variation:
          generated = static_cast<size_t>(PlanetVoxelGeneratedMaterialField::Variation);
          break;
        default:
          continue;
        }
        voxel::material::VoxelMaterialFieldTile tile{};
        tile.values = region.values[generated];
        tile.canonicalOrigin = microOrigin;
        tile.canonicalExtent = microExtent;
        if (!atlas.setPlanetRegionField(materialHandle, region.regionMappingId, fieldSlot, tile, error))
          return false;
      }
    }
  }
  return true;
}

std::vector<uint8_t> serializePlanetVoxelTransientPage(const PlanetVoxelChunk &chunk)
{
  return serializePlanetVoxelTransientPageInternal(chunk);
}

std::vector<uint8_t> serializeWorldVoxelTransientPage(
    const PlanetVoxelChunkSet &page)
{
  return serializeTerrainVirtualPagePayload(
      terrainVirtualPagePayloadFromChunkSet(page));
}

bool deserializeWorldVoxelTransientPage(
    const std::vector<uint8_t> &bytes,
    PlanetVoxelChunkSet &page,
    std::string *error)
{
  TerrainVirtualPagePayload payload{};
  if (!deserializeTerrainVirtualPagePayload(bytes, payload, error))
    return false;
  page = terrainVirtualPagePayloadToChunkSet(payload);
  return true;
}

bool deserializePlanetVoxelTransientPage(const std::vector<uint8_t> &bytes, PlanetVoxelChunk &chunk, std::string *error)
{
  return deserializePlanetVoxelTransientPageInternal(bytes, chunk, error);
}

bool findPlanetLandSpawnTile(const procedural::PlanetPackageReader &package, const uint8_t targetLevel, procedural::PlanetTileAddress &tile, double *surfaceMetres, uint8_t *material, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!package.isOpen() || targetLevel > 31u)
    return fail(error, "Land spawn selection needs an open package and valid target level");
  const auto &metadata = package.metadata();
  const uint32_t heightChannel = static_cast<uint32_t>(procedural::PlanetRasterChannel::Height);
  const uint32_t materialChannel = static_cast<uint32_t>(procedural::PlanetRasterChannel::Material);
  const uint32_t mountainChannel = static_cast<uint32_t>(procedural::PlanetRasterChannel::Mountain);
  const uint32_t tectonicsChannel = static_cast<uint32_t>(procedural::PlanetRasterChannel::Tectonics);
  const auto *heightDescriptor = package.findChannel(procedural::PlanetPackageDomain::Raster, heightChannel);
  if (heightDescriptor == nullptr || heightDescriptor->format != procedural::PlanetPackageFormat::R16UInt)
    return fail(error, "Planet package has no unsigned height raster for land spawn selection");

  uint8_t coarsestLevel = 0xffu;
  for (const auto &record : package.blocks())
    if (record.key.domain == procedural::PlanetPackageDomain::Raster && record.key.channelId == heightChannel)
      coarsestLevel = std::min(coarsestLevel, record.key.tile.level);
  if (coarsestLevel == 0xffu)
    return fail(error, "Planet package contains no height tiles");

  const uint32_t storedSide = metadata.tileSizePixels + 2u * metadata.gutterPixels;
  const size_t storedTexels = static_cast<size_t>(storedSide) * storedSide;
  bool found = false;
  bool foundBroadLand = false;
  int64_t selectedScore = std::numeric_limits<int64_t>::lowest();
  uint16_t selectedHeight = 0u;
  uint8_t selectedMaterial = 0u;
  procedural::SpherePoint selectedDirection{};
  double selectedPredictedHorizonRise = 0.0;
  double selectedPredictedDetailRise = 0.0;
  struct SpawnCandidate
  {
    int64_t mapScore = 0;
    uint16_t height = 0u;
    uint8_t material = 0u;
    double mountain = 0.0;
    double tectonics = 0.0;
    int64_t reliefPotential = 0;
    procedural::SpherePoint direction{};
  };
  const auto lowerMapScore = [](const SpawnCandidate &left, const SpawnCandidate &right)
  {
    return left.mapScore > right.mapScore;
  };
  std::priority_queue<SpawnCandidate, std::vector<SpawnCandidate>, decltype(lowerMapScore)> broadCandidates(lowerMapScore);
  const auto lowerReliefPotential = [](const SpawnCandidate &left, const SpawnCandidate &right)
  {
    return left.reliefPotential > right.reliefPotential;
  };
  std::priority_queue<SpawnCandidate, std::vector<SpawnCandidate>, decltype(lowerReliefPotential)> reliefCandidates(lowerReliefPotential);
  constexpr size_t kMountainViewCandidateCount = 512u;
  const auto spawnBiomePreference = [](const uint8_t value)
  {
    using Material = procedural::PolygonTerrainMaterial;
    switch (static_cast<Material>(value))
    {
    case Material::Grass:
    case Material::Grassland:
      return int64_t{4'000'000};
    case Material::TemperateRainForest:
    case Material::TemperateDeciduousForest:
    case Material::TropicalRainForest:
    case Material::TropicalSeasonalForest:
    case Material::Taiga:
      return int64_t{3'000'000};
    case Material::Marsh:
    case Material::Shrubland:
      return int64_t{2'000'000};
    case Material::Tundra:
      return int64_t{500'000};
    case Material::Bare:
    case Material::Scorched:
    case Material::Rock:
    case Material::Snow:
    case Material::Ice:
      return int64_t{-2'000'000};
    default:
      return int64_t{0};
    }
  };
  for (const auto &heightRecord : package.blocks())
  {
    if (heightRecord.key.domain != procedural::PlanetPackageDomain::Raster || heightRecord.key.channelId != heightChannel || heightRecord.key.tile.level != coarsestLevel)
      continue;
    const auto *materialRecord = package.findBlock(procedural::planetRasterBlockKey(materialChannel, heightRecord.key.tile));
    if (materialRecord == nullptr)
      continue;
    const auto *mountainRecord = package.findBlock(procedural::planetRasterBlockKey(mountainChannel, heightRecord.key.tile));
    const auto *tectonicsRecord = package.findBlock(procedural::planetRasterBlockKey(tectonicsChannel, heightRecord.key.tile));
    std::vector<uint8_t> heights;
    std::vector<uint8_t> materials;
    std::vector<uint8_t> mountains;
    std::vector<uint8_t> tectonics;
    if (!package.readBlock(heightRecord, heights, error) || !package.readBlock(*materialRecord, materials, error) || (mountainRecord != nullptr && !package.readBlock(*mountainRecord, mountains, error)) ||
        (tectonicsRecord != nullptr && !package.readBlock(*tectonicsRecord, tectonics, error)))
      return false;
    if (heights.size() != storedTexels * sizeof(uint16_t) || materials.size() != storedTexels || (!mountains.empty() && mountains.size() != storedTexels) || (!tectonics.empty() && tectonics.size() != storedTexels))
      return fail(error, "Planet package spawn maps do not match their tile dimensions");

    const auto isDry = [&](const uint32_t x, const uint32_t y)
    {
      const size_t index = static_cast<size_t>(y + metadata.gutterPixels) * storedSide + x + metadata.gutterPixels;
      const auto candidate = static_cast<procedural::PolygonTerrainMaterial>(materials[index]);
      return candidate != procedural::PolygonTerrainMaterial::Ocean && candidate != procedural::PolygonTerrainMaterial::Lake;
    };
    for (uint32_t y = 0u; y < metadata.tileSizePixels; ++y)
      for (uint32_t x = 0u; x < metadata.tileSizePixels; ++x)
      {
        if (!isDry(x, y))
          continue;
        const size_t index = static_cast<size_t>(y + metadata.gutterPixels) * storedSide + x + metadata.gutterPixels;
        const uint16_t candidateHeight = readU16(heights, index);
        bool broadLand = x >= 2u && y >= 2u && x + 2u < metadata.tileSizePixels && y + 2u < metadata.tileSizePixels;
        uint16_t neighbourhoodFloor = candidateHeight;
        uint16_t neighbourhoodCeiling = candidateHeight;
        for (int32_t offsetY = -2; offsetY <= 2 && broadLand; ++offsetY)
          for (int32_t offsetX = -2; offsetX <= 2; ++offsetX)
          {
            const uint32_t sampleX = static_cast<uint32_t>(static_cast<int32_t>(x) + offsetX);
            const uint32_t sampleY = static_cast<uint32_t>(static_cast<int32_t>(y) + offsetY);
            if (!isDry(sampleX, sampleY))
            {
              broadLand = false;
              break;
            }
            const size_t sample = static_cast<size_t>(sampleY + metadata.gutterPixels) * storedSide + sampleX + metadata.gutterPixels;
            neighbourhoodFloor = std::min(neighbourhoodFloor, readU16(heights, sample));
            neighbourhoodCeiling = std::max(neighbourhoodCeiling, readU16(heights, sample));
          }
        const int64_t elevationScore = broadLand ? static_cast<uint32_t>(candidateHeight) + neighbourhoodFloor : candidateHeight;
        int64_t candidateScore = elevationScore + spawnBiomePreference(materials[index]);
        int64_t localReliefPotential = 0;
        if (!mountains.empty() || !tectonics.empty())
        {
          // Compose a mountain *view*, not a mountain spawn. Search a wide
          // annulus for uplift and reward the outer part of it, while strongly
          // penalizing uplift and roughness under the player's feet. The
          // runtime clipmap later chooses the precise visible peak and camera
          // bearing from generated page bounds.
          const int32_t maximumRadius = std::max<int32_t>(2, std::min<int32_t>(10, metadata.tileSizePixels / 3u));
          const int32_t minimumRadius = std::max(2, maximumRadius / 2);
          int64_t distantReliefScore = 0;
          for (int32_t offsetY = -maximumRadius; offsetY <= maximumRadius; ++offsetY)
            for (int32_t offsetX = -maximumRadius; offsetX <= maximumRadius; ++offsetX)
            {
              const int32_t distanceSquared = offsetX * offsetX + offsetY * offsetY;
              if (distanceSquared < minimumRadius * minimumRadius || distanceSquared > maximumRadius * maximumRadius)
                continue;
              const int32_t sampleX = static_cast<int32_t>(x) + offsetX;
              const int32_t sampleY = static_cast<int32_t>(y) + offsetY;
              if (sampleX < 0 || sampleY < 0 || sampleX >= static_cast<int32_t>(metadata.tileSizePixels) || sampleY >= static_cast<int32_t>(metadata.tileSizePixels))
                continue;
              const size_t sample = static_cast<size_t>(sampleY + metadata.gutterPixels) * storedSide + static_cast<uint32_t>(sampleX) + metadata.gutterPixels;
              const int64_t signal = (mountains.empty() ? 0 : mountains[sample] * 3) + (tectonics.empty() ? 0 : tectonics[sample]);
              const int64_t heightGain = std::max<int64_t>(0, static_cast<int64_t>(readU16(heights, sample)) - candidateHeight);
              distantReliefScore = std::max(distantReliefScore, signal * 2048 + heightGain * 8 + distanceSquared * 64);
            }
          const int64_t localSignal = (mountains.empty() ? 0 : mountains[index] * 3) + (tectonics.empty() ? 0 : tectonics[index]);
          const int64_t localRoughness = static_cast<int64_t>(neighbourhoodCeiling) - neighbourhoodFloor;
          localReliefPotential = localSignal * 2048 - localRoughness * 4;
          candidateScore = distantReliefScore - localSignal * 1536 - localRoughness * 4 + elevationScore / 8;
        }
        procedural::SpherePoint direction{};
        if (!procedural::planetCubeTileTexelDirection(heightRecord.key.tile, metadata.tileSizePixels, metadata.gutterPixels, x + metadata.gutterPixels, y + metadata.gutterPixels, direction))
          continue;
        if (broadLand)
        {
          const SpawnCandidate viewCandidate{
            candidateScore,
            candidateHeight,
            materials[index],
            mountains.empty() ? 0.0 : mountains[index] / 255.0,
            tectonics.empty() ? 0.0 : tectonics[index] / 255.0,
            localReliefPotential,
            direction,
          };
          if (broadCandidates.size() < kMountainViewCandidateCount)
            broadCandidates.push(viewCandidate);
          else if (candidateScore > broadCandidates.top().mapScore)
          {
            broadCandidates.pop();
            broadCandidates.push(viewCandidate);
          }
          if (reliefCandidates.size() < kMountainViewCandidateCount)
            reliefCandidates.push(viewCandidate);
          else if (viewCandidate.reliefPotential > reliefCandidates.top().reliefPotential)
          {
            reliefCandidates.pop();
            reliefCandidates.push(viewCandidate);
          }
        }
        if ((!found || (broadLand && !foundBroadLand) || (broadLand == foundBroadLand && candidateScore > selectedScore)))
        {
          found = true;
          foundBroadLand = broadLand;
          selectedScore = candidateScore;
          selectedHeight = candidateHeight;
          selectedMaterial = materials[index];
          selectedDirection = direction;
        }
      }
  }
  if (!broadCandidates.empty() || !reliefCandidates.empty())
  {
    // The atlas annulus is the authoritative scenic signal: it distinguishes a
    // gentle foreground beside tectonic uplift from an arbitrary flat biome.
    // Use procedural detail only as a tie-breaker. The previous weighting did
    // the opposite and then added the multi-million-point grass preference a
    // second time, so almost every world selected a green but featureless
    // plain even when a mountain belt was available nearby.
    double bestViewScore = std::numeric_limits<double>::lowest();
    // Resolve the shortlist against the real height atlas at distances that
    // occupy the outer half of the initial 2.25 km resident footprint. This is
    // the terrain the player will actually see on the horizon; mountain and
    // tectonic masks only guide us toward promising candidates.
    const std::array<double, 3> viewDistances{metadata.nominalRenderPatchEdgeMetres * 0.55, metadata.nominalRenderPatchEdgeMetres * 0.80, metadata.nominalRenderPatchEdgeMetres * 1.05};
    std::map<procedural::PlanetPackageBlockKey, std::vector<uint8_t>> heightTileCache;
    bool heightSamplingFailed = false;
    const auto sampledSurface = [&](const procedural::SpherePoint &direction) -> std::optional<double>
    {
      procedural::PlanetTileAddress requested{};
      if (!procedural::planetCubeTile(direction, targetLevel, requested))
        return std::nullopt;
      const auto *record = package.findBestRasterBlock(heightChannel, requested);
      if (record == nullptr)
        return std::nullopt;
      auto [entry, inserted] = heightTileCache.try_emplace(record->key);
      if (inserted && !package.readBlock(*record, entry->second, error))
      {
        heightSamplingFailed = true;
        return std::nullopt;
      }
      if (entry->second.size() != storedTexels * sizeof(uint16_t))
      {
        heightSamplingFailed = true;
        return std::nullopt;
      }
      procedural::PlanetTileAddress sampledTile{};
      double tileU = 0.0;
      double tileV = 0.0;
      if (!procedural::planetCubeTile(direction, record->key.tile.level, sampledTile, &tileU, &tileV))
        return std::nullopt;
      return bilinearHeight(entry->second, storedSide, metadata.gutterPixels, metadata.tileSizePixels, tileU, tileV, heightDescriptor->scale, heightDescriptor->bias);
    };
    std::vector<SpawnCandidate> viewCandidates;
    viewCandidates.reserve(broadCandidates.size() + reliefCandidates.size());
    while (!broadCandidates.empty())
    {
      viewCandidates.push_back(broadCandidates.top());
      broadCandidates.pop();
    }
    while (!reliefCandidates.empty())
    {
      viewCandidates.push_back(reliefCandidates.top());
      reliefCandidates.pop();
    }
    for (const SpawnCandidate &candidate : viewCandidates)
    {
      const procedural::SpherePoint up = candidate.direction;
      const procedural::SpherePoint reference = std::abs(up.y) < 0.9 ? procedural::SpherePoint{0.0, 1.0, 0.0} : procedural::SpherePoint{1.0, 0.0, 0.0};
      const auto normalized = [](const procedural::SpherePoint value)
      {
        const double inverseLength = 1.0 / std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
        return procedural::SpherePoint{value.x * inverseLength, value.y * inverseLength, value.z * inverseLength};
      };
      const procedural::SpherePoint tangentX = normalized({
        reference.y * up.z - reference.z * up.y,
        reference.z * up.x - reference.x * up.z,
        reference.x * up.y - reference.y * up.x,
      });
      const procedural::SpherePoint tangentY{
        up.y * tangentX.z - up.z * tangentX.y,
        up.z * tangentX.x - up.x * tangentX.z,
        up.x * tangentX.y - up.y * tangentX.x,
      };
      const double centerRelief = proceduralTerrainDetail(up, metadata, candidate.mountain, candidate.tectonics, candidate.material, static_cast<uint8_t>(procedural::PlanetLithology::Sedimentary), 1.0);
      const std::optional<double> centerSurface = sampledSurface(up);
      double bestDistantRelief = std::numeric_limits<double>::lowest();
      double bestAtlasRise = std::numeric_limits<double>::lowest();
      constexpr uint32_t directionCount = 16u;
      for (uint32_t index = 0u; index < directionCount; ++index)
      {
        const double angle = static_cast<double>(index) * 6.28318530717958647692 / directionCount;
        for (const double viewDistance : viewDistances)
        {
          const double offset = viewDistance / metadata.planetRadiusMetres;
          const procedural::SpherePoint sample = normalized({
            up.x + offset * (std::cos(angle) * tangentX.x + std::sin(angle) * tangentY.x),
            up.y + offset * (std::cos(angle) * tangentX.y + std::sin(angle) * tangentY.y),
            up.z + offset * (std::cos(angle) * tangentX.z + std::sin(angle) * tangentY.z),
          });
          bestDistantRelief =
              std::max(bestDistantRelief, proceduralTerrainDetail(sample, metadata, candidate.mountain, candidate.tectonics, candidate.material, static_cast<uint8_t>(procedural::PlanetLithology::Sedimentary), 1.0));
          const std::optional<double> surface = sampledSurface(sample);
          if (centerSurface.has_value() && surface.has_value())
            bestAtlasRise = std::max(bestAtlasRise, *surface - *centerSurface);
        }
      }
      const double fineReliefGain = std::max(0.0, bestDistantRelief - centerRelief);
      const double visibleRise = std::max(0.0, bestAtlasRise);
      const double viewScore = visibleRise * 1'000'000.0 + fineReliefGain * 1'000'000.0 + static_cast<double>(candidate.reliefPotential) * 0.50 + static_cast<double>(candidate.mapScore) * 0.10 + bestDistantRelief * 8.0 +
                               static_cast<double>(spawnBiomePreference(candidate.material)) * 0.02;
      if (viewScore > bestViewScore)
      {
        bestViewScore = viewScore;
        found = true;
        foundBroadLand = true;
        selectedScore = candidate.mapScore;
        selectedHeight = candidate.height;
        selectedMaterial = candidate.material;
        selectedDirection = candidate.direction;
        selectedPredictedHorizonRise = visibleRise;
        selectedPredictedDetailRise = fineReliefGain;
      }
    }
    if (heightSamplingFailed)
      return fail(error, "Could not sample height atlas during scenic spawn selection");
  }
  if (!found || !procedural::planetCubeTile(selectedDirection, targetLevel, tile))
    return fail(error, "Planet package contains no dry-land spawn sample");
  os::Logger::logf(
      "Planet scenic spawn selection: sourceLevel=%u targetLevel=%u "
      "atlasRise=%.2fm detailRise=%.2fm material=%u",
      static_cast<uint32_t>(coarsestLevel),
      static_cast<uint32_t>(targetLevel),
      selectedPredictedHorizonRise,
      selectedPredictedDetailRise,
      static_cast<uint32_t>(selectedMaterial));
  if (surfaceMetres != nullptr)
    *surfaceMetres = heightDescriptor->bias + selectedHeight * heightDescriptor->scale;
  if (material != nullptr)
    *material = selectedMaterial;
  return true;
}

bool buildPlanetVoxelChunk(
    const procedural::PlanetPackageReader &package,
    const procedural::PlanetTileAddress &tile,
    const PlanetVoxelChunkSettings &settings,
    PlanetVoxelChunk &chunk,
    std::string *error,
    const PlanetVoxelChunkProgress &progress,
    const std::atomic_bool *cancellation)
{
  chunk = {};
  if (error != nullptr)
    error->clear();
  if (cancellationRequested(cancellation))
    return cancelled(error);
  if (!package.isOpen() || !procedural::validPlanetTileAddress(tile))
    return fail(error, "Planet voxel chunk needs an open package and valid tile address");
  if (settings.horizontalVoxelCount == 0u || settings.maximumVerticalVoxels == 0u || settings.maximumBricks == 0u || !std::isfinite(settings.proceduralDetailStrength) || settings.proceduralDetailStrength < 0.0 ||
      !std::isfinite(settings.proceduralTerraceStrength) || settings.proceduralTerraceStrength < 0.0 || !std::isfinite(settings.proceduralTerraceHeightMetres) || !(settings.proceduralTerraceHeightMetres > 0.0) ||
      !std::isfinite(settings.proceduralTerraceTransitionFraction) || settings.proceduralTerraceTransitionFraction < 0.0 || settings.proceduralTerraceTransitionFraction >= 0.5)
    return fail(error, "Planet voxel chunk settings are invalid");
  const auto &metadata = package.metadata();
  if (metadata.tileSizePixels == 0u || !(metadata.nominalRenderPatchEdgeMetres > 0.0) || !std::isfinite(metadata.nominalRenderPatchEdgeMetres))
    return fail(error, "Planet package has invalid chunk metadata");

  const uint32_t heightChannel = static_cast<uint32_t>(procedural::PlanetRasterChannel::Height);
  const uint32_t materialChannel = static_cast<uint32_t>(procedural::PlanetRasterChannel::Material);
  const uint32_t mountainChannel = static_cast<uint32_t>(procedural::PlanetRasterChannel::Mountain);
  const uint32_t tectonicsChannel = static_cast<uint32_t>(procedural::PlanetRasterChannel::Tectonics);
  const uint32_t lithologyChannel = static_cast<uint32_t>(procedural::PlanetRasterChannel::Lithology);
  const auto *heightRecord = package.findBestRasterBlock(heightChannel, tile);
  if (heightRecord == nullptr)
    return fail(error, "Planet package has no height tile for the requested patch");
  const auto *materialRecord = package.findBlock(procedural::planetRasterBlockKey(materialChannel, heightRecord->key.tile));
  if (materialRecord == nullptr)
    return fail(error, "Planet package height tile has no matching material tile");
  const auto *heightDescriptor = package.findChannel(procedural::PlanetPackageDomain::Raster, heightChannel);
  if (heightDescriptor == nullptr || !std::isfinite(heightDescriptor->scale) || !std::isfinite(heightDescriptor->bias))
    return fail(error, "Planet package height channel is invalid");

  std::vector<uint8_t> heightBytes;
  std::vector<uint8_t> materialBytes;
  std::vector<uint8_t> mountainBytes;
  std::vector<uint8_t> tectonicsBytes;
  std::vector<uint8_t> lithologyBytes;
  report(progress, "Loading compressed height and material maps", 0.10);
  if (!package.readBlock(*heightRecord, heightBytes, error) || !package.readBlock(*materialRecord, materialBytes, error))
    return false;
  const auto loadOptionalMap = [&](const uint32_t channel, std::vector<uint8_t> &bytes)
  {
    const auto *record = package.findBlock(procedural::planetRasterBlockKey(channel, heightRecord->key.tile));
    return record == nullptr || package.readBlock(*record, bytes, error);
  };
  if (!loadOptionalMap(mountainChannel, mountainBytes) || !loadOptionalMap(tectonicsChannel, tectonicsBytes) || !loadOptionalMap(lithologyChannel, lithologyBytes))
    return false;
  if (cancellationRequested(cancellation))
    return cancelled(error);
  report(progress, "Sampling the requested terrain patch", 0.25);
  const uint64_t storedSide64 = static_cast<uint64_t>(metadata.tileSizePixels) + 2u * metadata.gutterPixels;
  if (storedSide64 > std::numeric_limits<uint32_t>::max())
    return fail(error, "Planet package tile dimensions are too large");
  const uint32_t storedSide = static_cast<uint32_t>(storedSide64);
  const size_t storedTexels = static_cast<size_t>(storedSide) * storedSide;
  if (heightBytes.size() != storedTexels * sizeof(uint16_t) || materialBytes.size() != storedTexels)
    return fail(error, "Planet package tile payload size does not match its metadata");
  if ((!mountainBytes.empty() && mountainBytes.size() != storedTexels) || (!tectonicsBytes.empty() && tectonicsBytes.size() != storedTexels))
    return fail(error, "Planet package relief maps do not match their tile dimensions");

  const auto sourceTile = heightRecord->key.tile;
  const uint32_t levelDelta = tile.level - sourceTile.level;
  const uint64_t subdivision = uint64_t{1u} << levelDelta;
  const uint64_t sourceBaseX = static_cast<uint64_t>(sourceTile.x) * subdivision;
  const uint64_t sourceBaseY = static_cast<uint64_t>(sourceTile.y) * subdivision;
  if (tile.x < sourceBaseX || tile.y < sourceBaseY || tile.x >= sourceBaseX + subdivision || tile.y >= sourceBaseY + subdivision)
    return fail(error, "Planet package ancestor does not contain the requested patch");

  const uint32_t side = settings.horizontalVoxelCount;
  if (side > static_cast<uint32_t>(std::numeric_limits<int32_t>::max() - 2 * kTerrainSlopeContextHalo))
    return fail(error, "Planet voxel patch dimensions are too large");
  const size_t columnCount = static_cast<size_t>(side) * side;
  std::vector<double> heights(columnCount);
  std::vector<uint32_t> columnMaterials(columnCount * 2u);
  std::vector<uint8_t> materials(columnCount);
  std::vector<procedural::SpherePoint> directions(columnCount);
  std::vector<double> mountainWeights(columnCount, 0.0);
  const int32_t halo = kTerrainSlopeContextHalo;
  const uint32_t haloSide = side + static_cast<uint32_t>(2 * halo);
  std::vector<double> haloHeights(static_cast<size_t>(haloSide) * haloSide);

  struct RasterSampleBlock
  {
    procedural::PlanetTileAddress sourceTile{};
    std::vector<uint8_t> heights;
    std::vector<uint8_t> materials;
    std::vector<uint8_t> mountains;
    std::vector<uint8_t> tectonics;
    // Optional: packages written before the geology channel existed have none,
    // and every consumer falls back to the default rock rather than failing.
    std::vector<uint8_t> lithology;
  };
  std::vector<RasterSampleBlock> rasterBlocks;
  rasterBlocks.reserve(9u);
  rasterBlocks.push_back({
    sourceTile,
    std::move(heightBytes),
    std::move(materialBytes),
    std::move(mountainBytes),
    std::move(tectonicsBytes),
    std::move(lithologyBytes),
  });
  struct RasterTileMapping
  {
    procedural::PlanetTileAddress requestedTile{};
    procedural::PlanetTileAddress sourceTile{};
  };
  std::vector<RasterTileMapping> rasterTileMappings;
  rasterTileMappings.reserve(9u);
  rasterTileMappings.push_back({tile, sourceTile});
  const auto rasterBlockFor = [&](const procedural::PlanetTileAddress &requestedTile, const RasterSampleBlock *&result)
  {
    const auto mapped = std::find_if(
        rasterTileMappings.begin(),
        rasterTileMappings.end(),
        [&](const RasterTileMapping &candidate)
        {
          return candidate.requestedTile == requestedTile;
        });
    const procedural::PlanetPackageBlockRecord *requestedHeight = nullptr;
    procedural::PlanetTileAddress requestedSource{};
    if (mapped != rasterTileMappings.end())
    {
      requestedSource = mapped->sourceTile;
    }
    else
    {
      requestedHeight = package.findBestRasterBlock(heightChannel, requestedTile);
      if (requestedHeight == nullptr)
        return fail(error, "Planet package has no height tile for a terrain halo sample");
      requestedSource = requestedHeight->key.tile;
      rasterTileMappings.push_back({requestedTile, requestedSource});
    }
    const auto found = std::find_if(
        rasterBlocks.begin(),
        rasterBlocks.end(),
        [&](const RasterSampleBlock &candidate)
        {
          return candidate.sourceTile == requestedSource;
        });
    if (found != rasterBlocks.end())
    {
      result = &*found;
      return true;
    }

    const auto *requestedMaterial = package.findBlock(procedural::planetRasterBlockKey(materialChannel, requestedSource));
    if (requestedMaterial == nullptr)
      return fail(error, "Planet package height tile has no matching material tile for a terrain halo sample");
    RasterSampleBlock block{};
    block.sourceTile = requestedSource;
    if (requestedHeight == nullptr)
      requestedHeight = package.findBlock(procedural::planetRasterBlockKey(heightChannel, requestedSource));
    if (requestedHeight == nullptr)
      return fail(error, "Planet package terrain halo height tile disappeared");
    if (!package.readBlock(*requestedHeight, block.heights, error) || !package.readBlock(*requestedMaterial, block.materials, error))
      return false;
    const auto loadOptionalHaloMap = [&](const uint32_t channel, std::vector<uint8_t> &bytes)
    {
      const auto *record = package.findBlock(procedural::planetRasterBlockKey(channel, requestedSource));
      return record == nullptr || package.readBlock(*record, bytes, error);
    };
    if (!loadOptionalHaloMap(mountainChannel, block.mountains) || !loadOptionalHaloMap(tectonicsChannel, block.tectonics) || !loadOptionalHaloMap(lithologyChannel, block.lithology))
      return false;
    if (block.heights.size() != storedTexels * sizeof(uint16_t) || block.materials.size() != storedTexels || (!block.mountains.empty() && block.mountains.size() != storedTexels) ||
        (!block.tectonics.empty() && block.tectonics.size() != storedTexels) || (!block.lithology.empty() && block.lithology.size() != storedTexels))
      return fail(error, "Planet package terrain halo maps do not match their tile dimensions");
    rasterBlocks.push_back(std::move(block));
    result = &rasterBlocks.back();
    return true;
  };

  double minimumHeight = std::numeric_limits<double>::max();
  double maximumHeight = std::numeric_limits<double>::lowest();
  const double dimension = std::ldexp(1.0, tile.level);
  for (int32_t z = -halo; z < static_cast<int32_t>(side) + halo; ++z)
  {
    if (cancellationRequested(cancellation))
      return cancelled(error);
    for (int32_t x = -halo; x < static_cast<int32_t>(side) + halo; ++x)
    {
      const double localU = (static_cast<double>(x) + 0.5) / side;
      const double localV = (static_cast<double>(z) + 0.5) / side;
      const procedural::PlanetCubeCoordinate coordinate{static_cast<procedural::PlanetCubeFace>(tile.face), (static_cast<double>(tile.x) + localU) / dimension, (static_cast<double>(tile.y) + localV) / dimension};
      procedural::SpherePoint direction{};
      procedural::PlanetTileAddress sampleTile{};
      double tileU = 0.0;
      double tileV = 0.0;
      if (!procedural::planetCubeDirection(coordinate, direction))
        return fail(error, "Could not map a terrain material sample onto the planet");
      const bool interiorSample = x >= 0 && z >= 0 && x < static_cast<int32_t>(side) && z < static_cast<int32_t>(side);
      if (interiorSample)
      {
        sampleTile = tile;
        tileU = localU;
        tileV = localV;
      }
      else if (!procedural::planetCubeTile(direction, tile.level, sampleTile, &tileU, &tileV))
      {
        return fail(error, "Could not resolve a terrain halo sample onto a canonical tile");
      }
      const RasterSampleBlock *rasterBlock = nullptr;
      if (!rasterBlockFor(sampleTile, rasterBlock))
        return false;
      const uint32_t sampleLevelDelta = sampleTile.level - rasterBlock->sourceTile.level;
      const uint64_t sampleSubdivision = uint64_t{1u} << sampleLevelDelta;
      const uint64_t sampleSourceBaseX = static_cast<uint64_t>(rasterBlock->sourceTile.x) * sampleSubdivision;
      const uint64_t sampleSourceBaseY = static_cast<uint64_t>(rasterBlock->sourceTile.y) * sampleSubdivision;
      if (sampleTile.x < sampleSourceBaseX || sampleTile.y < sampleSourceBaseY || sampleTile.x >= sampleSourceBaseX + sampleSubdivision || sampleTile.y >= sampleSourceBaseY + sampleSubdivision)
        return fail(error, "Planet package ancestor does not contain a terrain halo sample");
      const double sourceU = (static_cast<double>(sampleTile.x - sampleSourceBaseX) + tileU) / sampleSubdivision;
      const double sourceV = (static_cast<double>(sampleTile.y - sampleSourceBaseY) + tileV) / sampleSubdivision;
      double height = bilinearHeight(rasterBlock->heights, storedSide, metadata.gutterPixels, metadata.tileSizePixels, sourceU, sourceV, heightDescriptor->scale, heightDescriptor->bias);
      const uint8_t material = nearestByte(rasterBlock->materials, storedSide, metadata.gutterPixels, metadata.tileSizePixels, sourceU, sourceV);
      const double mountain = rasterBlock->mountains.empty() ? 0.0 : bilinearByte(rasterBlock->mountains, storedSide, metadata.gutterPixels, metadata.tileSizePixels, sourceU, sourceV);
      const double tectonics = rasterBlock->tectonics.empty() ? 0.0 : bilinearByte(rasterBlock->tectonics, storedSide, metadata.gutterPixels, metadata.tileSizePixels, sourceU, sourceV);
      // Rock type is a class index: nearest, never interpolated, or the
      // landform generator is handed rock types that do not exist and every
      // contact becomes a blend of two behaviours instead of a boundary.
      const uint8_t lithology = rasterBlock->lithology.empty() ? static_cast<uint8_t>(procedural::PlanetLithology::Sedimentary)
                                                               : nearestByte(rasterBlock->lithology, storedSide, metadata.gutterPixels, metadata.tileSizePixels, sourceU, sourceV);
      if (settings.proceduralDetailStrength > 0.0)
        height += proceduralTerrainDetail(direction, metadata, mountain, tectonics, material, lithology, settings.proceduralDetailStrength);
      const size_t haloColumn = static_cast<size_t>(z + halo) * haloSide + static_cast<uint32_t>(x + halo);
      haloHeights[haloColumn] = height;
      if (interiorSample)
      {
        const size_t column = static_cast<size_t>(z) * side + static_cast<uint32_t>(x);
        heights[column] = height;
        materials[column] = material;
        directions[column] = direction;
        mountainWeights[column] = std::max(mountain, tectonics * 0.72);
        minimumHeight = std::min(minimumHeight, height);
        maximumHeight = std::max(maximumHeight, height);
      }
    }
    const uint32_t haloRow = static_cast<uint32_t>(z + halo + 1);
    if ((haloRow & 7u) == 0u || haloRow == haloSide)
      report(progress, "Sampling the requested terrain patch", 0.25 + 0.25 * static_cast<double>(haloRow) / haloSide);
  }

  const double edge = settings.patchEdgeMetres > 0.0 ? settings.patchEdgeMetres : metadata.nominalRenderPatchEdgeMetres;
  if (!(edge > 0.0) || !std::isfinite(edge))
    return fail(error, "Planet voxel patch extent is invalid");
  const double voxelSize = edge / side;
  const double elevationOrigin = std::floor(minimumHeight / voxelSize) * voxelSize - static_cast<double>(settings.foundationDepthVoxels) * voxelSize;
  std::vector<uint32_t> haloSurfaceTops(static_cast<size_t>(haloSide) * haloSide);
  for (uint32_t z = 0u; z < haloSide; ++z)
    for (uint32_t x = 0u; x < haloSide; ++x)
    {
      const size_t column = static_cast<size_t>(z) * haloSide + x;
      haloSurfaceTops[column] = std::min(static_cast<uint32_t>(std::max(0.0, std::floor((haloHeights[column] - elevationOrigin) / voxelSize))), settings.maximumVerticalVoxels - 1u);
    }
  const auto haloColumn = [&](const int32_t x, const int32_t z)
  {
    return static_cast<size_t>(z + halo) * haloSide + static_cast<uint32_t>(x + halo);
  };
  const auto sampleSurfaceTop = [&](const int32_t x, const int32_t z)
  {
    return haloSurfaceTops[haloColumn(x, z)];
  };
  std::vector<SparseVoxelCenterLengthGPU> leaves;
  std::vector<SparseVoxelMaterialGPU> leafMaterials;
  std::vector<uint32_t> leafIndices;
  const uint64_t maximumLeaves = static_cast<uint64_t>(columnCount) * settings.maximumVerticalVoxels;
  leaves.reserve(static_cast<size_t>(std::min<uint64_t>(maximumLeaves, 1u << 20u)));
  leafMaterials.reserve(leaves.capacity());
  leafIndices.reserve(leaves.capacity());
  const auto sampleHeight = [&](const int32_t x, const int32_t z)
  {
    return haloHeights[haloColumn(x, z)];
  };
  const auto surfaceGradient = [&](const uint32_t x, const uint32_t z)
  {
    const auto sample = [&](const int32_t offsetX, const int32_t offsetZ)
    {
      return sampleHeight(static_cast<int32_t>(x) + offsetX, static_cast<int32_t>(z) + offsetZ);
    };
    // A normalized 3x3 Scharr gradient is more rotationally symmetric than
    // two independent central differences. Its small neighbourhood keeps the
    // material slope classification stable along curved contours.
    const double dx = (3.0 * (sample(1, -1) - sample(-1, -1)) + 10.0 * (sample(1, 0) - sample(-1, 0)) + 3.0 * (sample(1, 1) - sample(-1, 1))) / (32.0 * voxelSize);
    const double dz = (3.0 * (sample(-1, 1) - sample(-1, -1)) + 10.0 * (sample(0, 1) - sample(0, -1)) + 3.0 * (sample(1, 1) - sample(1, -1))) / (32.0 * voxelSize);
    return std::array<double, 3>{dx, dz, std::sqrt(dx * dx + dz * dz)};
  };
  for (uint32_t z = 0u; z < side; ++z)
  {
    for (uint32_t x = 0u; x < side; ++x)
    {
      const size_t column = static_cast<size_t>(z) * side + x;
      const uint32_t top = sampleSurfaceTop(static_cast<int32_t>(x), static_cast<int32_t>(z));
      const std::array<double, 3> gradient = surfaceGradient(x, z);
      SparseVoxelMaterialGPU surface = makePlanetVoxelSurfaceMaterial(materials[column], directions[column], metadata, gradient[2], mountainWeights[column]);
      // Terrain remains a full-voxel height field. Keep the compact context
      // byte for material/normal classification, but do not attach an exact
      // micro coverage patch; those patches quantize slopes into visible
      // triangle bands and hard contour lines.
      surface.flags = terrainSurfaceContextForGradient(gradient[2]);
      // The appearance the column's top voxel is about to be given, published
      // for consumers that reconstruct this surface from the height field
      // instead of from the brick hierarchy. Taken here rather than from the
      // palette, because the palette's secondary index addresses a per-brick
      // table a height-field consumer does not have.
      {
        const SparseVoxelBrickMaterialGPU packed = packSparseVoxelBrickMaterial(surface);
        columnMaterials[column * 2u] = packed.packed[0];
        columnMaterials[column * 2u + 1u] = packed.packed[1] | ((surface.flags & 0xffu) << 20u);
      }
      uint32_t firstSolidY = 0u;
      if (settings.surfaceShellDepthVoxels != 0u && top > settings.surfaceShellDepthVoxels)
      {
        const uint32_t shellFloor = top - settings.surfaceShellDepthVoxels;
        uint32_t lowestNeighbour = top;
        lowestNeighbour = std::min(lowestNeighbour, sampleSurfaceTop(static_cast<int32_t>(x) - 1, static_cast<int32_t>(z)));
        lowestNeighbour = std::min(lowestNeighbour, sampleSurfaceTop(static_cast<int32_t>(x) + 1, static_cast<int32_t>(z)));
        lowestNeighbour = std::min(lowestNeighbour, sampleSurfaceTop(static_cast<int32_t>(x), static_cast<int32_t>(z) - 1));
        lowestNeighbour = std::min(lowestNeighbour, sampleSurfaceTop(static_cast<int32_t>(x), static_cast<int32_t>(z) + 1));
        // Include every cell above the lower neighbour. This closes an exposed
        // cliff while omitting geology that no above-ground primary or sun ray
        // can reach. The one-cell overlap at the bottom prevents a diagonal
        // crack after hierarchy reduction.
        const uint32_t wallFloor = lowestNeighbour < top ? lowestNeighbour : top;
        firstSolidY = std::min(shellFloor, wallFloor);
      }
      for (uint32_t y = firstSolidY; y <= top; ++y)
      {
        SparseVoxelCenterLengthGPU leaf{};
        leaf.center = {
          static_cast<float>((static_cast<double>(x) + 0.5) * voxelSize - edge * 0.5),
          static_cast<float>((static_cast<double>(y) + 0.5) * voxelSize),
          static_cast<float>((static_cast<double>(z) + 0.5) * voxelSize - edge * 0.5),
        };
        leaf.length = static_cast<float>(voxelSize);
        leafIndices.push_back(static_cast<uint32_t>(leaves.size()));
        leaves.push_back(leaf);
        // These columns are a closed representation of a height field, not a
        // geological volume. Brick LODs OR-reduce 2x2x2 cells and retain one
        // contributing material. If the hidden column interior is gray rock,
        // progressively coarser cells select that rock instead of the visible
        // biome and produce gray trails as their LOD cut moves. Carrying the
        // column's surface material through its fill preserves the appearance
        // of the height field at every hierarchy level, including its compact
        // green-top/dirt-bottom palette relationship.
        SparseVoxelMaterialGPU voxelMaterial = surface;
        if (y != top)
        {
          SparseVoxelMicroContext voxelContext = SparseVoxelMicroContext::Interior;
          uint32_t lowestNeighbour = top;
          const auto considerWall = [&](const int32_t neighbourX, const int32_t neighbourZ, const SparseVoxelMicroContext wallContext)
          {
            const uint32_t neighbourTop = sampleSurfaceTop(neighbourX, neighbourZ);
            if (neighbourTop < y && neighbourTop < lowestNeighbour)
            {
              lowestNeighbour = neighbourTop;
              voxelContext = wallContext;
            }
          };
          considerWall(static_cast<int32_t>(x) - 1, static_cast<int32_t>(z), SparseVoxelMicroContext::WallNegativeX);
          considerWall(static_cast<int32_t>(x) + 1, static_cast<int32_t>(z), SparseVoxelMicroContext::WallPositiveX);
          considerWall(static_cast<int32_t>(x), static_cast<int32_t>(z) - 1, SparseVoxelMicroContext::WallNegativeZ);
          considerWall(static_cast<int32_t>(x), static_cast<int32_t>(z) + 1, SparseVoxelMicroContext::WallPositiveZ);
          voxelMaterial.flags = static_cast<uint32_t>(voxelContext);
        }
        leafMaterials.push_back(voxelMaterial);
      }
    }
    if ((z & 7u) == 7u || z + 1u == side)
      report(progress, "Voxelizing solid terrain columns", 0.50 + 0.32 * static_cast<double>(z + 1u) / side);
  }

  SparseVoxelBrickBuildSettings brickSettings{};
  brickSettings.voxelSize = static_cast<float>(voxelSize);
  brickSettings.maximumBricks = settings.maximumBricks;
  brickSettings.buildMicroSurface = settings.buildMicroSurface;
  brickSettings.microMaterials = nullptr;
  brickSettings.voxelMaterials = nullptr;
  report(progress, "Building sparse voxel bricks", 0.85);
  if (cancellationRequested(cancellation))
    return cancelled(error);
  if (!buildSparseVoxelBrickHierarchy(leaves, leafMaterials, leafIndices, brickSettings, chunk.bricks, error))
    return false;
  chunk.requestedTile = tile;
  chunk.sourceTile = sourceTile;
  chunk.edgeMetres = edge;
  chunk.voxelSizeMetres = voxelSize;
  chunk.elevationOriginMetres = elevationOrigin;
  chunk.minimumSurfaceMetres = minimumHeight;
  chunk.maximumSurfaceMetres = maximumHeight;
  const size_t centerColumn = static_cast<size_t>(side / 2u) * side + side / 2u;
  chunk.centerSurfaceMetres = heights[centerColumn];
  chunk.centerMaterial = materials[centerColumn];
  chunk.leafVoxelCount = static_cast<uint32_t>(leaves.size());
  // Publish the same height field the columns above were quantized from. It is
  // the only surface both a mesh proxy and the voxels can agree on: the
  // overview atlas the background mesh displaces from is metres coarser and
  // carries none of the procedural detail added here.
  chunk.surfaceHeightSide = haloSide;
  chunk.surfaceHeightHalo = static_cast<uint32_t>(halo);
  chunk.surfaceHeights.resize(haloHeights.size());
  std::transform(
      haloHeights.begin(),
      haloHeights.end(),
      chunk.surfaceHeights.begin(),
      [](const double height)
      {
        return static_cast<float>(height);
      });
  chunk.surfaceMaterials = std::move(columnMaterials);
  // The envelope is built from haloSurfaceTops rather than recomputed from the
  // heights, so it is quantized on exactly the cells the voxelizer emitted.
  // Page origins are all integer multiples of the voxel size, so two pages at
  // the same clipmap level agree on a shared column's cell top; pages at
  // different levels quantize on different grids and meet with a step, which
  // is the same LOD boundary the brick path already has.
  chunk.surfaceEnvelope.resize(haloHeights.size());
  for (int32_t z = 0; z < static_cast<int32_t>(haloSide); ++z)
    for (int32_t x = 0; x < static_cast<int32_t>(haloSide); ++x)
    {
      double maximum = std::numeric_limits<double>::lowest();
      const int32_t reach = static_cast<int32_t>(std::max(settings.surfaceEnvelopeRadius, 1u));
      for (int32_t offsetZ = -reach; offsetZ <= reach; ++offsetZ)
        for (int32_t offsetX = -reach; offsetX <= reach; ++offsetX)
        {
          const int32_t sampleX = std::clamp(x + offsetX, 0, static_cast<int32_t>(haloSide) - 1);
          const int32_t sampleZ = std::clamp(z + offsetZ, 0, static_cast<int32_t>(haloSide) - 1);
          const size_t neighbour = static_cast<size_t>(sampleZ) * haloSide + sampleX;
          maximum = std::max(maximum, elevationOrigin + static_cast<double>(haloSurfaceTops[neighbour] + 1u) * voxelSize);
        }
      chunk.surfaceEnvelope[static_cast<size_t>(z) * haloSide + x] = static_cast<float>(maximum);
    }
  if (settings.voxelMaterials != nullptr)
  {
    std::vector<uint16_t> handles = chunk.bricks.paletteDescriptorIds;
    std::sort(handles.begin(), handles.end());
    handles.erase(std::unique(handles.begin(), handles.end()), handles.end());
    for (const uint16_t handle : handles)
    {
      if (handle == voxel::material::kInvalidVoxelMaterialHandle)
        continue;
      const auto &slot = settings.voxelMaterials->slot(handle);
      const std::string assetId = settings.voxelMaterials->assetId(handle);
      uint64_t assetHash = 1469598103934665603ull;
      for (const char character : assetId)
      {
        assetHash ^= static_cast<uint8_t>(character);
        assetHash *= 1099511628211ull;
      }
      chunk.materialReferences.push_back({handle, slot.contextRequirements.fields, assetHash, assetId});
      chunk.bakedContextFields |= slot.contextRequirements.fields;
    }

    if (voxel::material::voxelContextFieldsRequireAuxiliaryPool(chunk.bakedContextFields))
    {
      voxel::material::VoxelAuxContextPool pool;
      pool.begin(static_cast<uint32_t>(chunk.bricks.materialIndices.size()));
      std::vector<voxel::material::VoxelAuxContext> contexts(chunk.bricks.materialIndices.size());
      for (const SparseVoxelBrickNodeGPU &node : chunk.bricks.nodes)
        for (uint32_t local = 0u; local < node.occupiedCount; ++local)
        {
          const size_t materialAddress = static_cast<size_t>(node.materialBase) + local;
          if (materialAddress >= chunk.bricks.microSurfaceData.size())
            continue;
          const uint64_t packedSurface =
              chunk.bricks.microSurfaceData[materialAddress];
          const uint32_t context = unpackSparseVoxelMicroContextValue(packedSurface);
          const bool surface = sparseVoxelMicroContextIsSurface(context);
          const float slope = surface ? static_cast<float>(sparseVoxelMicroContextTerrainSlope(context)) : 0.0f;
          // Interior terrain is canonically deep; visible surface parents are
          // depth zero. Blend/auxiliary remain canonical zero until supplied
          // by a terrain package channel.
          contexts[materialAddress] = voxel::material::makeVoxelAuxContext(slope, surface ? 0.0f : 1.0f, 0.0f, 0.0f);
        }
      for (const auto context : contexts)
        if (!pool.append(context, error))
          return false;
      chunk.auxiliaryContextIndices = pool.indices();
      chunk.auxiliaryContextPool = pool.values();
    }
  }
  if (cancellationRequested(cancellation))
    return cancelled(error);
  report(progress, "Voxel terrain ready", 1.0);
  return true;
}

int64_t quantizeWorldMapTerrainHeight(
    const double heightMetres, const double heightUnitMetres)
{
  if (!std::isfinite(heightMetres) || !std::isfinite(heightUnitMetres) ||
      !(heightUnitMetres > 0.0))
    return 0;
  const double scaled = heightMetres / heightUnitMetres;
  if (!std::isfinite(scaled) ||
      scaled <= static_cast<double>(std::numeric_limits<int64_t>::min()))
    return std::numeric_limits<int64_t>::min();
  if (scaled >= std::nextafter(
          static_cast<double>(std::numeric_limits<int64_t>::max()), 0.0))
    return std::numeric_limits<int64_t>::max();
  return static_cast<int64_t>(std::llround(scaled));
}

bool validateWorldMapTerrainColumns(
    const WorldMapTerrainColumnPage &page, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (page.abiVersion != kWorldMapTerrainColumnABIVersion ||
      page.semanticRevision != kWorldMapTerrainColumnSemanticRevision ||
      page.columnStride != sizeof(WorldMapTerrainColumnGPU) ||
      page.wallStride != sizeof(WorldMapTerrainColumnWallGPU))
    return fail(error, "World-map terrain column ABI is unsupported");
  if (page.side == 0u || page.maximumVerticalVoxels == 0u ||
      !std::isfinite(page.heightUnitMetres) || !(page.heightUnitMetres > 0.0) ||
      !std::isfinite(page.voxelSizeMetres) || !(page.voxelSizeMetres > 0.0) ||
      !std::isfinite(page.edgeMetres) || !(page.edgeMetres > 0.0) ||
      !std::isfinite(page.elevationOriginMetres))
    return fail(error, "World-map terrain column header is invalid");
  const uint64_t columnCount = uint64_t{page.side} * page.side;
  const uint64_t cornerSide = uint64_t{page.side} + 1u;
  if (columnCount > std::numeric_limits<size_t>::max() ||
      columnCount > std::numeric_limits<size_t>::max() / 2u ||
      cornerSide > std::numeric_limits<uint64_t>::max() / cornerSide ||
      page.columns.size() != columnCount || page.walls.size() != columnCount ||
      page.canonicalCornerHeights.size() != cornerSide * cornerSide ||
      page.canonicalBoundaryMinimumHeights.size() != uint64_t{4u} * page.side ||
      page.canonicalBoundaryMaximumHeights.size() != uint64_t{4u} * page.side)
    return fail(error, "World-map terrain column sidecar sizes are invalid");
  for (size_t index = 0u; index < page.columns.size(); ++index)
  {
    const auto &column = page.columns[index];
    const auto &wall = page.walls[index];
    if (column.topY >= page.maximumVerticalVoxels ||
        column.firstSolidY > column.topY ||
        wall.firstSolidYWithoutBoundaryStitch < column.firstSolidY ||
        wall.firstSolidYWithoutBoundaryStitch > column.topY ||
        wall.lowestNeighbourY >= page.maximumVerticalVoxels)
      return fail(error, "World-map terrain column vertical range is invalid at index " +
          std::to_string(index));
    if (worldMapTerrainColumnMaterial(column) >
        static_cast<uint8_t>(procedural::PolygonTerrainMaterial::SubtropicalDesert))
      return fail(error, "World-map terrain column material is invalid at index " +
          std::to_string(index));
    const auto validContext = [](const uint32_t context)
    {
      return context == static_cast<uint32_t>(SparseVoxelMicroContext::Interior) ||
          context == static_cast<uint32_t>(SparseVoxelMicroContext::FlatSurface) ||
          context == static_cast<uint32_t>(SparseVoxelMicroContext::WallNegativeX) ||
          context == static_cast<uint32_t>(SparseVoxelMicroContext::WallPositiveX) ||
          context == static_cast<uint32_t>(SparseVoxelMicroContext::WallNegativeZ) ||
          context == static_cast<uint32_t>(SparseVoxelMicroContext::WallPositiveZ);
    };
    if (!validContext(worldMapTerrainColumnSurfaceContext(column)) ||
        !validContext(worldMapTerrainColumnWallContext(wall)) ||
        !validContext(worldMapTerrainColumnBoundaryContext(wall)))
      return fail(error, "World-map terrain column context is invalid at index " +
          std::to_string(index));
  }
  return true;
}

bool buildWorldMapTerrainColumnLeafPayload(
    const WorldMapTerrainColumnPage &page,
    WorldMapTerrainColumnLeafPayload &payload, std::string *error)
{
  payload = {};
  if (!validateWorldMapTerrainColumns(page, error))
    return false;
  uint64_t leafCount = 0u;
  for (const auto &column : page.columns)
    leafCount += uint64_t{column.topY} - column.firstSolidY + 1u;
  if (leafCount > std::numeric_limits<uint32_t>::max() ||
      leafCount > std::numeric_limits<size_t>::max())
    return fail(error, "World-map terrain column leaf count is not representable");
  payload.leaves.reserve(static_cast<size_t>(leafCount));
  payload.materials.reserve(static_cast<size_t>(leafCount));
  payload.indices.reserve(static_cast<size_t>(leafCount));
  payload.surfaceMaterials.resize(page.columns.size() * 2u);
  for (uint32_t z = 0u; z < page.side; ++z)
    for (uint32_t x = 0u; x < page.side; ++x)
    {
      const size_t index = static_cast<size_t>(z) * page.side + x;
      const auto &column = page.columns[index];
      const auto &wall = page.walls[index];
      SparseVoxelMaterialGPU surface =
          planetVoxelBaseMaterial(worldMapTerrainColumnMaterial(column));
      surface.flags = worldMapTerrainColumnSurfaceContext(column);
      const SparseVoxelBrickMaterialGPU packed =
          packSparseVoxelBrickMaterial(surface);
      payload.surfaceMaterials[index * 2u] = packed.packed[0];
      payload.surfaceMaterials[index * 2u + 1u] = packed.packed[1];
      for (uint32_t y = column.firstSolidY; y <= column.topY; ++y)
      {
        payload.leaves.push_back({{
            static_cast<float>((x + 0.5) * page.voxelSizeMetres -
                page.edgeMetres * 0.5),
            static_cast<float>((y + 0.5) * page.voxelSizeMetres),
            static_cast<float>((z + 0.5) * page.voxelSizeMetres -
                page.edgeMetres * 0.5)},
            static_cast<float>(page.voxelSizeMetres)});
        SparseVoxelMaterialGPU material = surface;
        if (y != column.topY)
        {
          uint32_t context =
              static_cast<uint32_t>(SparseVoxelMicroContext::Interior);
          if (y < wall.firstSolidYWithoutBoundaryStitch)
            context = worldMapTerrainColumnBoundaryContext(wall);
          if (wall.lowestNeighbourY < y)
            context = worldMapTerrainColumnWallContext(wall);
          material.flags = context;
        }
        payload.materials.push_back(material);
        payload.indices.push_back(
            static_cast<uint32_t>(payload.indices.size()));
      }
    }
  return true;
}

// Field and page resolutions held for one batch of map samples.
//
// Every sample used to re-resolve its field by a linear scan over the bundle
// comparing names, then its page by a linear scan over that field's pages, once
// per level `bestPage` walks up. A page of terrain samples about ten thousand
// positions from twenty fields and they all land in the same export page, so
// that resolution is identical every time and costs more than the filter it
// precedes. Holding it per batch is what makes generation cost the filter.
//
// Indexed rather than keyed so a lookup is an array subscript. Not thread safe:
// each parallel row block owns one.
enum class WorldMapCursorField : uint32_t
{
  Elevation,
  WaterType,
  WaterLevel,
  River,
  BaseElevation,
  Uplift,
  ReliefRoughness,
  LandformId,
  Roads,
  SettlementActivity,
  ObjectExclusion,
  ErosionWearFlow,
  ErosionDeposition,
  CoastDistance,
  SubstrateId,
  SoilDepth,
  Climate,
  PlacementPotential,
  SoilLayers,
  Count,
};

struct WorldMapSampleCursors
{
  std::array<
      procedural::world::MapExportFieldCursor,
      static_cast<size_t>(WorldMapCursorField::Count)>
      fields{};
  procedural::world::MapExportBiomeCursor biome{};

  procedural::world::MapExportFieldCursor &operator[](
      const WorldMapCursorField field)
  {
    return fields[static_cast<size_t>(field)];
  }
};

bool buildWorldMapVoxelTerrain(
    const procedural::world::MapExportBundle &bundle,
    const WorldMapVoxelTerrainSettings &settings,
    PlanetVoxelChunkSet &chunks,
    std::string *error,
    const PlanetVoxelChunkProgress &progress,
    const std::atomic_bool *cancellation,
    WorldMapVoxelGenerationMetrics *metrics,
    WorldMapTerrainColumnPage *columnPage)
{
  using procedural::PolygonTerrainMaterial;
  using procedural::world::Biome;
  using procedural::world::GroundSubstrate;
  using procedural::world::MapExportBiomeSample;
  using procedural::world::MapExportFieldSample;

  chunks = {};
  if (columnPage != nullptr)
    *columnPage = {};
  WorldMapVoxelGenerationMetrics localMetrics{};
  localMetrics.internalLOD = settings.internalLOD;
  const auto millisecondsSince = [](const auto begin, const auto end)
  {
    return std::chrono::duration<double, std::milli>(end - begin).count();
  };
  if (metrics != nullptr)
    *metrics = {};
  if (error != nullptr)
    error->clear();
  if (cancellationRequested(cancellation))
    return cancelled(error);
  PlanetVoxelChunkSettings voxel = settings.voxel;
  const uint32_t finestHorizontalVoxelCount =
      settings.finestHorizontalVoxelCount != 0u
      ? settings.finestHorizontalVoxelCount
      : voxel.horizontalVoxelCount;
  if (settings.internalLOD >= 32u ||
      finestHorizontalVoxelCount == 0u ||
      finestHorizontalVoxelCount % (uint32_t{1u} << settings.internalLOD) != 0u)
    return fail(error, "World-map internal LOD does not divide the finest resolution");
  voxel.horizontalVoxelCount =
      finestHorizontalVoxelCount / (uint32_t{1u} << settings.internalLOD);
  if (settings.finestHorizontalVoxelCount != 0u &&
      (voxel.horizontalVoxelCount < kSparseVoxelBrickSide ||
       voxel.horizontalVoxelCount % kSparseVoxelBrickSide != 0u))
    return fail(error, "World-map internal LOD is not brick aligned");
  if (!(bundle.extentMetres > 0.0) || !std::isfinite(bundle.extentMetres) || settings.requestedLevel > bundle.maximumPageLevel || voxel.horizontalVoxelCount == 0u || voxel.maximumVerticalVoxels == 0u ||
      voxel.maximumBricks == 0u || !std::isfinite(voxel.proceduralDetailStrength) || voxel.proceduralDetailStrength < 0.0 || !std::isfinite(voxel.proceduralTerraceStrength) || voxel.proceduralTerraceStrength < 0.0 ||
      !std::isfinite(voxel.proceduralTerraceHeightMetres) || !(voxel.proceduralTerraceHeightMetres > 0.0) || !std::isfinite(voxel.proceduralTerraceTransitionFraction) ||
      voxel.proceduralTerraceTransitionFraction < 0.0 || voxel.proceduralTerraceTransitionFraction >= 0.5)
    return fail(error, "World-map voxel terrain settings are invalid");
  if (settings.boundaryStitchSubdivisions == 0u ||
      settings.boundaryStitchSubdivisions > 8u)
    return fail(error, "World-map boundary stitch subdivision is invalid");
  for (const double coordinate : settings.normalizedBounds)
    if (!std::isfinite(coordinate))
      return fail(error, "World-map voxel bounds are not finite");
  const double minimumU = std::clamp(std::min(settings.normalizedBounds[0], settings.normalizedBounds[2]), 0.0, 1.0);
  const double minimumV = std::clamp(std::min(settings.normalizedBounds[1], settings.normalizedBounds[3]), 0.0, 1.0);
  const double maximumU = std::clamp(std::max(settings.normalizedBounds[0], settings.normalizedBounds[2]), 0.0, 1.0);
  const double maximumV = std::clamp(std::max(settings.normalizedBounds[1], settings.normalizedBounds[3]), 0.0, 1.0);
  const double edgeX = (maximumU - minimumU) * bundle.extentMetres;
  const double edgeZ = (maximumV - minimumV) * bundle.extentMetres;
  if (!(edgeX > 0.0) || !(edgeZ > 0.0) || std::abs(edgeX - edgeZ) > std::max(edgeX, edgeZ) * 1.0e-9)
    return fail(error, "World-map voxel terrain currently requires square bounds");
  const double edge = edgeX;
  const uint32_t side = voxel.horizontalVoxelCount;
  const double voxelSize = edge / side;
  if (!(voxelSize > 0.0) || !std::isfinite(voxelSize))
    return fail(error, "World-map voxel size is invalid");
  const double finestVoxelSize = settings.finestVoxelSizeMetres > 0.0
      ? settings.finestVoxelSizeMetres
      : edge / finestHorizontalVoxelCount;
  if (!(finestVoxelSize > 0.0) || !std::isfinite(finestVoxelSize) ||
      std::abs(finestVoxelSize * finestHorizontalVoxelCount - edge) >
          std::max(edge, 1.0) * 1.0e-9)
    return fail(error, "World-map finest voxel grid does not match the fixed footprint");
  localMetrics.horizontalVoxelCount = side;
  localMetrics.verticalVoxelCapacity = voxel.maximumVerticalVoxels;
  const auto samplingStart = std::chrono::steady_clock::now();

  const auto *refined = bundle.field("refined_elevation");
  const std::string elevationField = refined != nullptr && refined->available ? "refined_elevation" : "base_elevation";
  const std::array<const char *, 7u> requiredFields{"water_type", "water_level", "river", "substrate_id", "soil_depth", "biome_mix_ids", "biome_mix_weights"};
  const auto *elevation = bundle.field(elevationField);
  const auto *baseElevationField = bundle.field("base_elevation");
  const auto *upliftField = bundle.field("uplift");
  const auto *reliefRoughness = bundle.field("relief_roughness");
  const auto *landformField = bundle.field("landform_id");
  const auto *erosionWearFlowField = bundle.field("erosion_wear_flow");
  const auto *erosionDepositionField = bundle.field("erosion_deposition");
  const auto *coastDistanceField = bundle.field("coast_distance");
  const auto *climateField = bundle.field("climate");
  const auto *placementPotentialField = bundle.field("placement_potential");
  const auto *soilLayersField = bundle.field("soil_layers");
  const auto *roadsField = bundle.field("roads");
  const auto *settlementField = bundle.field("settlement_activity");
  const auto *objectExclusionField = bundle.field("object_exclusion");
  if (elevation == nullptr || !elevation->available)
    return fail(error, "World-map voxel terrain has no available elevation field");
  for (const char *name : requiredFields)
  {
    const auto *field = bundle.field(name);
    if (field == nullptr || !field->available)
      return fail(error, std::string("World-map voxel terrain needs field: ") + name);
  }

  const auto biomeMaterial = [](const Biome biome)
  {
    switch (biome)
    {
    case Biome::Ocean:
      return PolygonTerrainMaterial::Ocean;
    case Biome::Lake:
      return PolygonTerrainMaterial::Lake;
    case Biome::Shore:
      return PolygonTerrainMaterial::Beach;
    case Biome::Glacier:
      return PolygonTerrainMaterial::Ice;
    case Biome::Tundra:
    case Biome::ColdDesert:
    case Biome::AlpineMeadow:
      return PolygonTerrainMaterial::Tundra;
    case Biome::Taiga:
    case Biome::MontaneForest:
      return PolygonTerrainMaterial::Taiga;
    case Biome::TemperateForest:
      return PolygonTerrainMaterial::TemperateDeciduousForest;
    case Biome::TemperateRainforest:
      return PolygonTerrainMaterial::TemperateRainForest;
    case Biome::TropicalRainforest:
    case Biome::SubtropicalForest:
      return PolygonTerrainMaterial::TropicalRainForest;
    case Biome::TropicalSeasonalForest:
      return PolygonTerrainMaterial::TropicalSeasonalForest;
    case Biome::Desert:
    case Biome::SandDesert:
    case Biome::SaltFlat:
      return PolygonTerrainMaterial::SubtropicalDesert;
    case Biome::Mediterranean:
    case Biome::Shrubland:
    case Biome::Steppe:
    case Biome::Savanna:
      return PolygonTerrainMaterial::Shrubland;
    case Biome::Wetland:
    case Biome::Swamp:
    case Biome::Mangrove:
      return PolygonTerrainMaterial::Marsh;
    case Biome::BareRock:
      return PolygonTerrainMaterial::Rock;
    case Biome::Grassland:
      return PolygonTerrainMaterial::Grassland;
    case Biome::Count:
      break;
    }
    return PolygonTerrainMaterial::Grass;
  };

  const int32_t halo = kTerrainSlopeContextHalo;
  const uint32_t haloSide = side + static_cast<uint32_t>(halo * 2);
  std::vector<double> haloHeights(static_cast<size_t>(haloSide) * haloSide);
  std::vector<double> heights(static_cast<size_t>(side) * side);
  std::vector<uint8_t> materials(heights.size());
  double minimumHeight = std::numeric_limits<double>::max();
  double maximumHeight = std::numeric_limits<double>::lowest();
  // The error sink is a parameter, not the enclosing one: column blocks are
  // sampled in parallel and each needs somewhere private to fail.
  const auto sampleWorldMapSurface =
      [&](const procedural::Point2 &position,
          double &surfaceHeight,
          uint8_t &waterType,
          MapExportFieldSample *sampledRiver,
          std::string *error,
          WorldMapSampleCursors &cursors)
  {
    MapExportFieldSample elevationSample{};
    MapExportFieldSample baseElevation{};
    MapExportFieldSample uplift{};
    MapExportFieldSample water{};
    MapExportFieldSample waterLevel{};
    MapExportFieldSample relief{};
    procedural::world::MapExportCategorySample landforms{};
    landforms.count = 1u;
    landforms.ids[0] = static_cast<uint8_t>(procedural::world::CellType::Hill);
    landforms.weights[0] = 1.0;
    MapExportFieldSample riverSurface{};
    MapExportFieldSample erosionWearFlow{};
    MapExportFieldSample erosionDeposition{};
    MapExportFieldSample coastDistance{};
    MapExportFieldSample roads{};
    MapExportFieldSample settlement{};
    MapExportFieldSample objectExclusion{};
    ++localMetrics.mapSampleCount;
    if (!procedural::world::sampleMapExportFieldAtWorld(
            bundle, elevationField, cursors[WorldMapCursorField::Elevation],
            position, settings.requestedLevel, elevationSample, error) ||
        !procedural::world::sampleMapExportFieldAtWorld(
            bundle, "water_type", cursors[WorldMapCursorField::WaterType],
            position, settings.requestedLevel, water, error) ||
        !procedural::world::sampleMapExportFieldAtWorld(
            bundle, "water_level", cursors[WorldMapCursorField::WaterLevel],
            position, settings.requestedLevel, waterLevel, error) ||
        !procedural::world::sampleMapExportFieldAtWorld(
            bundle, "river", cursors[WorldMapCursorField::River],
            position, settings.requestedLevel, riverSurface, error))
      return false;
    const auto sampleOptional = [&](const char *name,
                                    const WorldMapCursorField cursorField,
                                    const procedural::world::MapExportField *field,
                                    MapExportFieldSample &sample)
    {
      return field == nullptr || !field->available ||
          procedural::world::sampleMapExportFieldAtWorld(
              bundle, name, cursors[cursorField], position,
              settings.requestedLevel, sample, error);
    };
    if (!sampleOptional("base_elevation", WorldMapCursorField::BaseElevation,
            baseElevationField, baseElevation) ||
        !sampleOptional("uplift", WorldMapCursorField::Uplift, upliftField,
            uplift) ||
        !sampleOptional("erosion_wear_flow",
            WorldMapCursorField::ErosionWearFlow, erosionWearFlowField,
            erosionWearFlow) ||
        !sampleOptional("erosion_deposition",
            WorldMapCursorField::ErosionDeposition, erosionDepositionField,
            erosionDeposition) ||
        !sampleOptional("coast_distance", WorldMapCursorField::CoastDistance,
            coastDistanceField, coastDistance))
      return false;
    if (reliefRoughness != nullptr && reliefRoughness->available &&
        !procedural::world::sampleMapExportFieldAtWorld(
            bundle, "relief_roughness",
            cursors[WorldMapCursorField::ReliefRoughness], position,
            settings.requestedLevel, relief, error))
      return false;
    if (landformField != nullptr && landformField->available &&
        !procedural::world::sampleMapExportCategoryAtWorld(bundle, "landform_id",
            cursors[WorldMapCursorField::LandformId], position,
            settings.requestedLevel, landforms, error))
      return false;
    double infrastructureSmoothingMask = 0.0;
    if (roadsField != nullptr && roadsField->available)
    {
      if (!procedural::world::sampleMapExportFieldAtWorld(
              bundle, "roads", cursors[WorldMapCursorField::Roads], position,
              settings.requestedLevel, roads, error))
        return false;
      infrastructureSmoothingMask = roads.values[0] > 0.0 ? 1.0 : 0.0;
    }
    if (settlementField != nullptr && settlementField->available)
    {
      if (!procedural::world::sampleMapExportFieldAtWorld(
              bundle, "settlement_activity",
              cursors[WorldMapCursorField::SettlementActivity], position,
              settings.requestedLevel, settlement, error))
        return false;
      const double activity = settlement.values[0] > 1.0
          ? settlement.values[0] / 255.0
          : settlement.values[0];
      infrastructureSmoothingMask = std::max(
          infrastructureSmoothingMask, std::clamp(activity, 0.0, 1.0));
    }
    if (objectExclusionField != nullptr && objectExclusionField->available)
    {
      if (!procedural::world::sampleMapExportFieldAtWorld(
              bundle, "object_exclusion",
              cursors[WorldMapCursorField::ObjectExclusion], position,
              settings.requestedLevel, objectExclusion, error))
        return false;
      if (objectExclusion.values[0] > 0.0)
        infrastructureSmoothingMask = 1.0;
    }
    if (sampledRiver != nullptr)
      *sampledRiver = riverSurface;
    waterType = static_cast<uint8_t>(std::clamp(
        std::llround(water.values[0]), 0ll, 2ll));
    WorldMapTerrainSignals terrainSignals{};
    terrainSignals.hasBaseElevation =
        baseElevationField != nullptr && baseElevationField->available;
    terrainSignals.baseElevationMetres = baseElevation.values[0];
    terrainSignals.upliftMetresPerYear = uplift.values[0];
    terrainSignals.erosionWear = erosionWearFlow.values[0];
    terrainSignals.erosionFlow = erosionWearFlow.values[1];
    terrainSignals.erosionDeposition = erosionDeposition.values[0];
    terrainSignals.riverDischargeCubicMetresPerSecond =
        riverSurface.values[1];
    terrainSignals.coastDistanceMetres =
        coastDistanceField != nullptr && coastDistanceField->available
        ? coastDistance.values[0]
        : -1.0;
    surfaceHeight = waterLevel.values[0];
    if (waterType == 0u)
    {
      surfaceHeight = 0.0;
      for (uint32_t index = 0u; index < landforms.count; ++index)
      {
        const auto landform = static_cast<procedural::world::CellType>(std::min<uint8_t>(
            landforms.ids[index], static_cast<uint8_t>(procedural::world::CellType::Count) - 1u));
        surfaceHeight += landforms.weights[index] * proceduralWorldMapTerrainHeight(position, bundle,
              elevationSample.values[0], relief.values[0], relief.values[1],
              landform, voxel.proceduralDetailStrength,
              voxel.proceduralTerraceStrength,
              voxel.proceduralTerraceHeightMetres,
              voxel.proceduralTerraceTransitionFraction,
              riverSurface.values[0], infrastructureSmoothingMask,
              terrainSignals);
      }
    }
    return true;
  };
  report(progress, "Sampling generated world maps", 0.05);
  // Column samples are independent, so the grid is sampled in row blocks. A
  // transaction with four pages would otherwise leave most of the machine idle
  // while each page sampled 166 thousand columns on one thread.
  const uint32_t columnSamplingWorkers =
      std::max(1u, settings.columnSamplingWorkerCount);
  const uint32_t columnRowBlocks = std::min<uint32_t>(
      haloSide, std::max(1u, columnSamplingWorkers * 4u));
  std::atomic_bool columnSamplingFailed{false};
  std::mutex columnSamplingFailureMutex;
  std::string columnSamplingFailure;
  std::vector<double> columnBlockMinimum(
      columnRowBlocks, std::numeric_limits<double>::max());
  std::vector<double> columnBlockMaximum(
      columnRowBlocks, std::numeric_limits<double>::lowest());
  // Including the halo, which the elevation anchor grid needs and the page's
  // own minimum deliberately excludes.
  std::vector<double> columnBlockHaloMinimum(
      columnRowBlocks, std::numeric_limits<double>::max());
  const auto sampleColumnRowBlock = [&](const uint32_t block)
  {
    std::string blockErrorStorage;
    std::string *const error = &blockErrorStorage;
    WorldMapSampleCursors cursors{};
    const auto failBlock = [&]
    {
      if (!columnSamplingFailed.exchange(true))
      {
        std::lock_guard<std::mutex> lock(columnSamplingFailureMutex);
        columnSamplingFailure = blockErrorStorage.empty()
            ? std::string("World-map column sampling failed")
            : blockErrorStorage;
      }
    };
    const uint32_t rowBegin =
        static_cast<uint32_t>(uint64_t{block} * haloSide / columnRowBlocks);
    const uint32_t rowEnd = static_cast<uint32_t>(
        uint64_t{block + 1u} * haloSide / columnRowBlocks);
    double blockMinimum = std::numeric_limits<double>::max();
    double blockMaximum = std::numeric_limits<double>::lowest();
    double blockHaloMinimum = std::numeric_limits<double>::max();
    for (uint32_t row = rowBegin; row < rowEnd; ++row)
    {
      if (cancellationRequested(cancellation) ||
          columnSamplingFailed.load(std::memory_order_relaxed))
        return;
      const int32_t z = static_cast<int32_t>(row) - halo;
    for (int32_t x = -halo; x < static_cast<int32_t>(side) + halo; ++x)
    {
      const double localU = (static_cast<double>(x) + 0.5) / side;
      const double localV = (static_cast<double>(z) + 0.5) / side;
      const procedural::Point2 position{(minimumU + (maximumU - minimumU) * localU) * bundle.extentMetres, (minimumV + (maximumV - minimumV) * localV) * bundle.extentMetres};
      MapExportFieldSample river{};
      MapExportFieldSample substrate{};
      MapExportFieldSample soil{};
      MapExportFieldSample climate{};
      MapExportFieldSample placementPotential{};
      MapExportFieldSample soilLayers{};
      MapExportBiomeSample biome{};
      double height = 0.0;
      uint8_t waterType = 0u;
      if (!sampleWorldMapSurface(
              position, height, waterType, &river, error, cursors))
      {
        failBlock();
        return;
      }
      const size_t haloColumnIndex = static_cast<size_t>(z + halo) * haloSide +
          static_cast<uint32_t>(x + halo);
      haloHeights[haloColumnIndex] = height;
      blockHaloMinimum = std::min(blockHaloMinimum, height);
      // A halo column contributes slope context and the anchor minimum only.
      // Sampling its substrate, soil, biome and climate fields and then
      // discarding them was six field lookups per column for nothing.
      if (x < 0 || z < 0 || x >= static_cast<int32_t>(side) ||
          z >= static_cast<int32_t>(side))
        continue;
      if (!procedural::world::sampleMapExportFieldAtWorld(
              bundle, "substrate_id", cursors[WorldMapCursorField::SubstrateId],
              position, settings.requestedLevel, substrate, error) ||
          !procedural::world::sampleMapExportFieldAtWorld(
              bundle, "soil_depth", cursors[WorldMapCursorField::SoilDepth],
              position, settings.requestedLevel, soil, error) ||
          !procedural::world::sampleMapExportBiomeAtWorld(
              bundle, cursors.biome, position, settings.requestedLevel, biome,
              error))
      {
        failBlock();
        return;
      }
      const auto sampleMaterialField = [&](const char *name,
                                           const WorldMapCursorField cursorField,
                                           const procedural::world::MapExportField *field,
                                           MapExportFieldSample &sample)
      {
        return field == nullptr || !field->available ||
            procedural::world::sampleMapExportFieldAtWorld(
                bundle, name, cursors[cursorField], position,
                settings.requestedLevel, sample, error);
      };
      if (!sampleMaterialField("climate", WorldMapCursorField::Climate,
              climateField, climate) ||
          !sampleMaterialField("placement_potential",
              WorldMapCursorField::PlacementPotential,
              placementPotentialField, placementPotential) ||
          !sampleMaterialField("soil_layers", WorldMapCursorField::SoilLayers,
              soilLayersField, soilLayers))
      {
        failBlock();
        return;
      }
      const size_t column = static_cast<size_t>(z) * side + static_cast<uint32_t>(x);
      heights[column] = height;
      PolygonTerrainMaterial material = waterType == 1u
          ? PolygonTerrainMaterial::Ocean
          : waterType == 2u
              ? PolygonTerrainMaterial::Lake
              : biomeMaterial(static_cast<Biome>(worldMapBiomeForColumn(
                    biome, position, bundle, finestVoxelSize)));
      const auto ground = static_cast<GroundSubstrate>(static_cast<uint8_t>(std::clamp(std::llround(substrate.values[0]), 0ll, static_cast<long long>(GroundSubstrate::Count) - 1ll)));
      const bool exposedRock =
          (placementPotentialField != nullptr &&
              placementPotentialField->available &&
              placementPotential.values[3] >= 0.58 &&
              soil.values[0] < 0.5) ||
          (soilLayersField != nullptr && soilLayersField->available &&
              soilLayers.values[0] >= 0.62);
      if (waterType == 0u && (soil.values[0] < 0.15 ||
          ground == GroundSubstrate::Bedrock ||
          ground == GroundSubstrate::Scree || exposedRock))
        material = PolygonTerrainMaterial::Rock;
      if (waterType == 0u && climateField != nullptr &&
          climateField->available && climate.values[0] <= -2.0 &&
          climate.values[1] >= 0.12)
        material = PolygonTerrainMaterial::Snow;
      const double channelHalfWidth = river.values[1] > 0.0
          ? std::clamp(5.0 + std::sqrt(river.values[1]) * 8.0,
                std::max(2.0, voxelSize * 0.75), 120.0)
          : 0.0;
      const bool alluvialBank = soilLayersField != nullptr &&
          soilLayersField->available &&
          soilLayers.values[2] + soilLayers.values[3] >= 0.48;
      if (waterType == 0u && alluvialBank &&
          climate.values[1] >= 0.55 && river.values[0] <=
              std::max(channelHalfWidth * 3.0, 30.0))
        material = PolygonTerrainMaterial::Marsh;
      if (waterType == 0u && river.values[1] > 0.0 &&
          river.values[0] <= channelHalfWidth)
        material = PolygonTerrainMaterial::River;
      materials[column] = static_cast<uint8_t>(material);
      blockMinimum = std::min(blockMinimum, height);
      blockMaximum = std::max(blockMaximum, height);
    }
      if (block == 0u && ((row & 7u) == 0u || row + 1u == rowEnd))
        report(
            progress, "Sampling generated world maps",
            0.05 + 0.35 * static_cast<double>(row - rowBegin + 1u) /
                std::max(1u, rowEnd - rowBegin));
    }
    columnBlockMinimum[block] = blockMinimum;
    columnBlockMaximum[block] = blockMaximum;
    columnBlockHaloMinimum[block] = blockHaloMinimum;
  };
  if (columnRowBlocks > 1u && columnSamplingWorkers > 1u)
    terrainChunkBuildThreadPool().parallelFor(
        columnSamplingWorkers, columnRowBlocks, sampleColumnRowBlock);
  else
    for (uint32_t block = 0u; block < columnRowBlocks; ++block)
      sampleColumnRowBlock(block);
  if (cancellationRequested(cancellation))
    return cancelled(error);
  if (columnSamplingFailed.load(std::memory_order_relaxed))
    return fail(error, columnSamplingFailure);
  double haloInclusiveMinimum = std::numeric_limits<double>::max();
  for (uint32_t block = 0u; block < columnRowBlocks; ++block)
  {
    minimumHeight = std::min(minimumHeight, columnBlockMinimum[block]);
    maximumHeight = std::max(maximumHeight, columnBlockMaximum[block]);
    haloInclusiveMinimum =
        std::min(haloInclusiveMinimum, columnBlockHaloMinimum[block]);
  }
  if (!std::isfinite(minimumHeight) || !std::isfinite(maximumHeight))
    return fail(error, "World-map voxel terrain sampled no finite surface");

  // Surface patches are defined at voxel corners, not column centres. Deriving
  // a corner by averaging four centre samples makes that value depend on the
  // cascade's sample spacing: fine and coarse pages then disagree at the same
  // world coordinate and expose a tall voxel side at their ownership edge.
  // Sample the canonical world-space corners directly so page seams and every
  // spatial LOD share bit-identical source elevations.
  const uint32_t cornerSide = haloSide + 1u;
  std::vector<double> cornerHeights(
      static_cast<size_t>(cornerSide) * cornerSide);
  const auto sampleCanonicalSurface = [&](const double localU,
                                           const double localV,
                                           double &surfaceHeight,
                                           WorldMapSampleCursors &cursors)
  {
    const procedural::Point2 position{
      (minimumU + (maximumU - minimumU) * localU) * bundle.extentMetres,
      (minimumV + (maximumV - minimumV) * localV) * bundle.extentMetres};
    uint8_t waterType = 0u;
    return sampleWorldMapSurface(
        position, surfaceHeight, waterType, nullptr, error, cursors);
  };
  // Corner elevations are the same independent per-sample work as the column
  // grid and are sampled the same way.
  const uint32_t cornerRowBlocks = std::min<uint32_t>(
      cornerSide, std::max(1u, columnSamplingWorkers * 4u));
  std::atomic_bool cornerSamplingFailed{false};
  std::mutex cornerSamplingFailureMutex;
  std::string cornerSamplingFailure;
  std::vector<double> cornerBlockMinimum(
      cornerRowBlocks, std::numeric_limits<double>::max());
  std::vector<double> cornerBlockMaximum(
      cornerRowBlocks, std::numeric_limits<double>::lowest());
  std::vector<double> cornerBlockHaloMinimum(
      cornerRowBlocks, std::numeric_limits<double>::max());
  const auto sampleCornerRowBlock = [&](const uint32_t block)
  {
    std::string blockErrorStorage;
    WorldMapSampleCursors cursors{};
    const uint32_t rowBegin =
        static_cast<uint32_t>(uint64_t{block} * cornerSide / cornerRowBlocks);
    const uint32_t rowEnd = static_cast<uint32_t>(
        uint64_t{block + 1u} * cornerSide / cornerRowBlocks);
    double blockMinimum = std::numeric_limits<double>::max();
    double blockMaximum = std::numeric_limits<double>::lowest();
    double blockHaloMinimum = std::numeric_limits<double>::max();
    for (uint32_t row = rowBegin; row < rowEnd; ++row)
    {
      if (cancellationRequested(cancellation) ||
          cornerSamplingFailed.load(std::memory_order_relaxed))
        return;
      const int32_t z = static_cast<int32_t>(row) - halo;
      for (int32_t x = -halo;
           x <= static_cast<int32_t>(side) + halo; ++x)
      {
        const double localU = static_cast<double>(x) / side;
        const double localV = static_cast<double>(z) / side;
        double cornerSurfaceHeight = 0.0;
        const procedural::Point2 position{
          (minimumU + (maximumU - minimumU) * localU) * bundle.extentMetres,
          (minimumV + (maximumV - minimumV) * localV) * bundle.extentMetres};
        uint8_t cornerWaterType = 0u;
        if (!sampleWorldMapSurface(
                position, cornerSurfaceHeight, cornerWaterType, nullptr,
                &blockErrorStorage, cursors))
        {
          if (!cornerSamplingFailed.exchange(true))
          {
            std::lock_guard<std::mutex> lock(cornerSamplingFailureMutex);
            cornerSamplingFailure = blockErrorStorage.empty()
                ? std::string("World-map corner sampling failed")
                : blockErrorStorage;
          }
          return;
        }
        cornerHeights[
            static_cast<size_t>(z + halo) * cornerSide +
            static_cast<uint32_t>(x + halo)] = cornerSurfaceHeight;
        blockHaloMinimum = std::min(blockHaloMinimum, cornerSurfaceHeight);
        if (settings.finestHorizontalVoxelCount == 0u ||
            (x >= 0 && z >= 0 && x <= static_cast<int32_t>(side) &&
             z <= static_cast<int32_t>(side)))
        {
          blockMinimum = std::min(blockMinimum, cornerSurfaceHeight);
          blockMaximum = std::max(blockMaximum, cornerSurfaceHeight);
        }
      }
    }
    cornerBlockMinimum[block] = blockMinimum;
    cornerBlockMaximum[block] = blockMaximum;
    cornerBlockHaloMinimum[block] = blockHaloMinimum;
  };
  if (cornerRowBlocks > 1u && columnSamplingWorkers > 1u)
    terrainChunkBuildThreadPool().parallelFor(
        columnSamplingWorkers, cornerRowBlocks, sampleCornerRowBlock);
  else
    for (uint32_t block = 0u; block < cornerRowBlocks; ++block)
      sampleCornerRowBlock(block);
  if (cancellationRequested(cancellation))
    return cancelled(error);
  if (cornerSamplingFailed.load(std::memory_order_relaxed))
    return fail(error, cornerSamplingFailure);
  for (uint32_t block = 0u; block < cornerRowBlocks; ++block)
  {
    minimumHeight = std::min(minimumHeight, cornerBlockMinimum[block]);
    maximumHeight = std::max(maximumHeight, cornerBlockMaximum[block]);
    haloInclusiveMinimum =
        std::min(haloInclusiveMinimum, cornerBlockHaloMinimum[block]);
  }

  // A coarse edge column can meet two fine columns. Its canonical endpoints
  // are shared, but their maximum does not describe a valley at the fine
  // midpoint. Record the minimum surface along every boundary segment so the
  // coarse owner emits a vertical stitch wall down to all adjacent fine tops.
  constexpr uint32_t kBoundaryFaceCount = 4u;
  // The boundary stitch walk is serial, so one cursor set covers it.
  WorldMapSampleCursors boundaryCursors{};
  std::vector<double> boundaryMinimumHeights(
      static_cast<size_t>(kBoundaryFaceCount) * side,
      std::numeric_limits<double>::max());
  std::vector<double> boundaryMaximumHeights(
      static_cast<size_t>(kBoundaryFaceCount) * side,
      std::numeric_limits<double>::lowest());
  const auto boundaryMinimum = [&](const uint32_t face,
                                   const uint32_t segment) -> double &
  {
    return boundaryMinimumHeights[static_cast<size_t>(face) * side + segment];
  };
  const auto boundaryMaximum = [&](const uint32_t face,
                                   const uint32_t segment) -> double &
  {
    return boundaryMaximumHeights[static_cast<size_t>(face) * side + segment];
  };
  for (uint32_t segment = 0u; segment < side; ++segment)
  {
    const std::array<std::array<double, 2u>, kBoundaryFaceCount> endpoints{{
      {{cornerHeights[static_cast<size_t>(segment + halo) * cornerSide + halo],
        cornerHeights[static_cast<size_t>(segment + halo + 1u) * cornerSide + halo]}},
      {{cornerHeights[static_cast<size_t>(segment + halo) * cornerSide + side + halo],
        cornerHeights[static_cast<size_t>(segment + halo + 1u) * cornerSide + side + halo]}},
      {{cornerHeights[static_cast<size_t>(halo) * cornerSide + segment + halo],
        cornerHeights[static_cast<size_t>(halo) * cornerSide + segment + halo + 1u]}},
      {{cornerHeights[static_cast<size_t>(side + halo) * cornerSide + segment + halo],
        cornerHeights[static_cast<size_t>(side + halo) * cornerSide + segment + halo + 1u]}},
    }};
    for (uint32_t face = 0u; face < kBoundaryFaceCount; ++face)
    {
      boundaryMinimum(face, segment) = std::min(
          endpoints[face][0], endpoints[face][1]);
      boundaryMaximum(face, segment) = std::max(
          endpoints[face][0], endpoints[face][1]);
    }
    for (uint32_t subdivision = 1u;
         subdivision < settings.boundaryStitchSubdivisions;
         ++subdivision)
    {
      const double along =
          (static_cast<double>(segment) +
           static_cast<double>(subdivision) /
               settings.boundaryStitchSubdivisions) / side;
      std::array<double, kBoundaryFaceCount> samples{};
      if (!sampleCanonicalSurface(0.0, along, samples[0], boundaryCursors) ||
          !sampleCanonicalSurface(1.0, along, samples[1], boundaryCursors) ||
          !sampleCanonicalSurface(along, 0.0, samples[2], boundaryCursors) ||
          !sampleCanonicalSurface(along, 1.0, samples[3], boundaryCursors))
        return false;
      for (uint32_t face = 0u; face < kBoundaryFaceCount; ++face)
      {
        boundaryMinimum(face, segment) = std::min(
            boundaryMinimum(face, segment), samples[face]);
        boundaryMaximum(face, segment) = std::max(
            boundaryMaximum(face, segment), samples[face]);
        minimumHeight = std::min(minimumHeight, samples[face]);
        maximumHeight = std::max(maximumHeight, samples[face]);
      }
    }
  }

  double elevationOrigin =
      std::floor(minimumHeight / voxelSize) * voxelSize -
      static_cast<double>(voxel.foundationDepthVoxels) * voxelSize;
  if (settings.finestHorizontalVoxelCount != 0u)
  {
    const uint32_t anchorLOD = settings.elevationAnchorLOD == 0xffffffffu
        ? settings.internalLOD
        : settings.elevationAnchorLOD;
    if (anchorLOD >= 32u ||
        finestHorizontalVoxelCount % (uint32_t{1u} << anchorLOD) != 0u)
      return fail(error, "World-map elevation anchor LOD is invalid");
    const uint32_t anchorSide =
        finestHorizontalVoxelCount / (uint32_t{1u} << anchorLOD);
    if (anchorSide < kSparseVoxelBrickSide ||
        anchorSide % kSparseVoxelBrickSide != 0u)
      return fail(error, "World-map elevation anchor grid is not brick aligned");
    double anchorMinimum = std::numeric_limits<double>::max();
    const auto sampleAnchorHeight = [&](const double localU,
                                        const double localV,
                                        double &height,
                                        std::string *sampleError,
                                        WorldMapSampleCursors &cursors)
    {
      const procedural::Point2 position{
        (minimumU + (maximumU - minimumU) * localU) * bundle.extentMetres,
        (minimumV + (maximumV - minimumV) * localV) * bundle.extentMetres};
      uint8_t waterType = 0u;
      return sampleWorldMapSurface(
          position, height, waterType, nullptr, sampleError, cursors);
    };
    // Two more independent grids: the anchor centres and the anchor corners.
    // They reduce to one minimum, so a row block only has to report its own.
    const auto reduceAnchorGrid =
        [&](const uint32_t rowCount, const bool corners) -> bool
    {
      const uint32_t blocks = std::min<uint32_t>(
          rowCount, std::max(1u, columnSamplingWorkers * 4u));
      std::atomic_bool failed{false};
      std::mutex failureMutex;
      std::string failure;
      std::vector<double> blockMinimum(
          blocks, std::numeric_limits<double>::max());
      const auto sampleBlock = [&](const uint32_t block)
      {
        std::string blockErrorStorage;
        WorldMapSampleCursors cursors{};
        const uint32_t rowBegin =
            static_cast<uint32_t>(uint64_t{block} * rowCount / blocks);
        const uint32_t rowEnd =
            static_cast<uint32_t>(uint64_t{block + 1u} * rowCount / blocks);
        double minimum = std::numeric_limits<double>::max();
        for (uint32_t row = rowBegin; row < rowEnd; ++row)
        {
          if (cancellationRequested(cancellation) ||
              failed.load(std::memory_order_relaxed))
            return;
          const int32_t z = static_cast<int32_t>(row) - halo;
          const int32_t xEnd = corners
              ? static_cast<int32_t>(anchorSide) + halo
              : static_cast<int32_t>(anchorSide) + halo - 1;
          for (int32_t x = -halo; x <= xEnd; ++x)
          {
            double height = 0.0;
            const double offset = corners ? 0.0 : 0.5;
            if (!sampleAnchorHeight(
                    (static_cast<double>(x) + offset) / anchorSide,
                    (static_cast<double>(z) + offset) / anchorSide, height,
                    &blockErrorStorage, cursors))
            {
              if (!failed.exchange(true))
              {
                std::lock_guard<std::mutex> lock(failureMutex);
                failure = blockErrorStorage.empty()
                    ? std::string("World-map anchor sampling failed")
                    : blockErrorStorage;
              }
              return;
            }
            minimum = std::min(minimum, height);
          }
        }
        blockMinimum[block] = minimum;
      };
      if (blocks > 1u && columnSamplingWorkers > 1u)
        terrainChunkBuildThreadPool().parallelFor(
            columnSamplingWorkers, blocks, sampleBlock);
      else
        for (uint32_t block = 0u; block < blocks; ++block)
          sampleBlock(block);
      if (cancellationRequested(cancellation))
        return cancelled(error);
      if (failed.load(std::memory_order_relaxed))
        return fail(error, failure);
      for (const double value : blockMinimum)
        anchorMinimum = std::min(anchorMinimum, value);
      return true;
    };
    // When the anchor LOD is this page's own LOD the two anchor grids are the
    // column grid and the corner grid, position for position, so their minimum
    // is already known. Sampling them again was half of all map sampling.
    if (anchorSide == side)
      anchorMinimum = std::min(anchorMinimum, haloInclusiveMinimum);
    else
    {
      if (!reduceAnchorGrid(
              anchorSide + static_cast<uint32_t>(halo * 2), false))
        return false;
      anchorMinimum = std::min(anchorMinimum, minimumHeight);
      if (!reduceAnchorGrid(
              anchorSide + static_cast<uint32_t>(halo * 2) + 1u, true))
        return false;
    }
    anchorMinimum = std::min(anchorMinimum, minimumHeight);
    const double anchorVoxelSize = edge / anchorSide;
    elevationOrigin =
        std::floor(anchorMinimum / finestVoxelSize) * finestVoxelSize -
        static_cast<double>(voxel.foundationDepthVoxels) * anchorVoxelSize;
    if (minimumHeight < elevationOrigin)
      return fail(
          error,
          "The common elevation anchor does not conservatively cover this LOD");
  }
  localMetrics.mapSamplingMilliseconds = millisecondsSince(
      samplingStart, std::chrono::steady_clock::now());
  const auto shellStart = std::chrono::steady_clock::now();
  const double canonicalHeightUnit = finestVoxelSize /
      static_cast<double>(kWorldMapTerrainHeightUnitsPerVoxel);
  if (!std::isfinite(canonicalHeightUnit) || !(canonicalHeightUnit > 0.0))
    return fail(error, "World-map terrain canonical height unit is invalid");
  const auto relativeOwnershipUnits = [&](const double height)
  {
    const double scaled = (height - elevationOrigin) / voxelSize *
        static_cast<double>(kWorldMapTerrainHeightUnitsPerVoxel);
    if (!std::isfinite(scaled) ||
        scaled <= static_cast<double>(std::numeric_limits<int64_t>::min()) ||
        scaled >= std::nextafter(
            static_cast<double>(std::numeric_limits<int64_t>::max()), 0.0))
      return int64_t{0};
    return static_cast<int64_t>(std::llround(scaled));
  };
  const auto ownedTopFromHeight = [&](const double height)
  {
    const int64_t relative = std::max<int64_t>(
        relativeOwnershipUnits(height), 0);
    if (relative == 0)
      return uint64_t{0u};
    return (static_cast<uint64_t>(relative) +
               kWorldMapTerrainHeightUnitsPerVoxel - 1u) /
        kWorldMapTerrainHeightUnitsPerVoxel - 1u;
  };
  const auto floorFromHeight = [&](const double height)
  {
    const int64_t relative = std::max<int64_t>(
        relativeOwnershipUnits(height), 0);
    return static_cast<uint64_t>(relative) /
        kWorldMapTerrainHeightUnitsPerVoxel;
  };
  const auto haloColumn = [&](const int32_t x, const int32_t z)
  {
    return static_cast<size_t>(z + halo) * haloSide + static_cast<uint32_t>(x + halo);
  };
  const auto sampleHeight = [&](const int32_t x, const int32_t z)
  {
    const int32_t minimum = -halo;
    const int32_t maximum = static_cast<int32_t>(side) + halo - 1;
    return haloHeights[haloColumn(
        std::clamp(x, minimum, maximum),
        std::clamp(z, minimum, maximum))];
  };
  const auto cornerHeight = [&](const int32_t x, const int32_t z)
  {
    return cornerHeights[
        static_cast<size_t>(z + halo) * cornerSide +
        static_cast<uint32_t>(x + halo)];
  };
  std::vector<uint32_t> surfaceTops(haloHeights.size());
  for (int32_t z = -halo;
       z < static_cast<int32_t>(side) + halo; ++z)
    for (int32_t x = -halo;
         x < static_cast<int32_t>(side) + halo; ++x)
    {
      // Occupancy and the exact height patch must describe the same bilinear
      // cell. Choosing this slice from the centre sample allowed all four
      // reconstructed corners to lie below it at local extrema, leaving an
      // empty top parent that appeared as a random hole or pillar.
      double topHeight = std::max({
          cornerHeight(x, z), cornerHeight(x + 1, z),
          cornerHeight(x, z + 1), cornerHeight(x + 1, z + 1)});
      if (settings.boundaryStitchSubdivisions > 1u &&
          x >= 0 && z >= 0 &&
          x < static_cast<int32_t>(side) &&
          z < static_cast<int32_t>(side))
      {
        if (x == 0)
          topHeight = std::max(topHeight,
              boundaryMaximum(0u, static_cast<uint32_t>(z)));
        if (x + 1 == static_cast<int32_t>(side))
          topHeight = std::max(topHeight,
              boundaryMaximum(1u, static_cast<uint32_t>(z)));
        if (z == 0)
          topHeight = std::max(topHeight,
              boundaryMaximum(2u, static_cast<uint32_t>(x)));
        if (z + 1 == static_cast<int32_t>(side))
          topHeight = std::max(topHeight,
              boundaryMaximum(3u, static_cast<uint32_t>(x)));
      }
      // Occupancy is the half-open interval below the surface. Quantize in the
      // canonical signed integer domain first, then select ceil(height)-1.
      // This is independent of floating-point contraction and dispatch order.
      const uint64_t topCell = ownedTopFromHeight(topHeight);
      if (settings.finestHorizontalVoxelCount != 0u &&
          topCell >= voxel.maximumVerticalVoxels)
        return fail(
            error,
            "World-map fixed chunk exceeds its vertical voxel capacity");
      surfaceTops[haloColumn(x, z)] = std::min(
          static_cast<uint32_t>(topCell),
          voxel.maximumVerticalVoxels - 1u);
    }
  const auto surfaceTop = [&](const int32_t x, const int32_t z)
  {
    return surfaceTops[haloColumn(x, z)];
  };
  WorldMapTerrainColumnPage canonicalColumns{};
  canonicalColumns.side = side;
  canonicalColumns.halo = static_cast<uint32_t>(halo);
  canonicalColumns.maximumVerticalVoxels = voxel.maximumVerticalVoxels;
  canonicalColumns.internalLOD = settings.internalLOD;
  canonicalColumns.flags = settings.exteriorShellOnly
      ? WorldMapTerrainColumnExteriorShell : 0u;
  canonicalColumns.heightUnitMetres = canonicalHeightUnit;
  canonicalColumns.voxelSizeMetres = voxelSize;
  canonicalColumns.edgeMetres = edge;
  canonicalColumns.elevationOriginMetres = elevationOrigin;
  canonicalColumns.columns.resize(heights.size());
  canonicalColumns.walls.resize(heights.size());
  canonicalColumns.canonicalCornerHeights.resize(
      static_cast<size_t>(side + 1u) * (side + 1u));
  for (uint32_t z = 0u; z <= side; ++z)
    for (uint32_t x = 0u; x <= side; ++x)
      canonicalColumns.canonicalCornerHeights[
          static_cast<size_t>(z) * (side + 1u) + x] =
          quantizeWorldMapTerrainHeight(
              cornerHeight(static_cast<int32_t>(x), static_cast<int32_t>(z)),
              canonicalHeightUnit);
  canonicalColumns.canonicalBoundaryMinimumHeights.resize(
      boundaryMinimumHeights.size());
  canonicalColumns.canonicalBoundaryMaximumHeights.resize(
      boundaryMaximumHeights.size());
  for (size_t index = 0u; index < boundaryMinimumHeights.size(); ++index)
  {
    canonicalColumns.canonicalBoundaryMinimumHeights[index] =
        quantizeWorldMapTerrainHeight(
            boundaryMinimumHeights[index], canonicalHeightUnit);
    canonicalColumns.canonicalBoundaryMaximumHeights[index] =
        quantizeWorldMapTerrainHeight(
            boundaryMaximumHeights[index], canonicalHeightUnit);
  }
  for (uint32_t z = 0u; z < side; ++z)
  {
    for (uint32_t x = 0u; x < side; ++x)
    {
      const size_t column = static_cast<size_t>(z) * side + x;
      const uint32_t top = surfaceTop(x, z);
      const std::array<double, 4> cornerSurfaceCells{
        (cornerHeight(static_cast<int32_t>(x),
             static_cast<int32_t>(z)) - elevationOrigin) / voxelSize,
        (cornerHeight(static_cast<int32_t>(x + 1u),
             static_cast<int32_t>(z)) - elevationOrigin) / voxelSize,
        (cornerHeight(static_cast<int32_t>(x),
             static_cast<int32_t>(z + 1u)) - elevationOrigin) / voxelSize,
        (cornerHeight(static_cast<int32_t>(x + 1u),
             static_cast<int32_t>(z + 1u)) - elevationOrigin) / voxelSize,
      };
      uint32_t firstSolidY = 0u;
      const uint32_t lowestNeighbour = std::min({
        top,
        surfaceTop(static_cast<int32_t>(x) - 1, z),
        surfaceTop(static_cast<int32_t>(x) + 1, z),
        surfaceTop(x, static_cast<int32_t>(z) - 1),
        surfaceTop(x, static_cast<int32_t>(z) + 1)});
      uint32_t firstSolidYWithoutBoundaryStitch = 0u;
      if (settings.exteriorShellOnly)
      {
        // Keep flat terrain exactly one cell thick. Closure belongs only
        // below an exposed side face: that one-cell overlap prevents a
        // diagonal opening when a coarse boundary column meets two finer
        // columns without restoring an underground top-shell layer.
        const uint32_t closure = voxel.surfaceShellDepthVoxels;
        const uint32_t firstExposedSide = lowestNeighbour < top
            ? lowestNeighbour + 1u
            : top;
        const uint32_t cliffClosureFloor = firstExposedSide > closure
            ? firstExposedSide - closure
            : 0u;
        firstSolidY = lowestNeighbour < top
            ? cliffClosureFloor
            : top;
        const double minimumCornerHeight = elevationOrigin +
            *std::min_element(
                cornerSurfaceCells.begin(), cornerSurfaceCells.end()) *
                voxelSize;
        const uint32_t surfaceFloor = std::min(
            static_cast<uint32_t>(floorFromHeight(minimumCornerHeight)),
            voxel.maximumVerticalVoxels - 1u);
        // A steep source cell can cross several vertical voxels. Emit the
        // intervening square cells so the stepped surface remains closed
        // instead of exposing an opening below the top voxel.
        firstSolidY = std::min(firstSolidY, surfaceFloor);
        firstSolidYWithoutBoundaryStitch = firstSolidY;

        double boundaryHeight = std::numeric_limits<double>::max();
        if (settings.boundaryStitchSubdivisions > 1u && x == 0u)
          boundaryHeight = std::min(boundaryHeight,
              boundaryMinimum(0u, z));
        if (settings.boundaryStitchSubdivisions > 1u && x + 1u == side)
          boundaryHeight = std::min(boundaryHeight,
              boundaryMinimum(1u, z));
        if (settings.boundaryStitchSubdivisions > 1u && z == 0u)
          boundaryHeight = std::min(boundaryHeight,
              boundaryMinimum(2u, x));
        if (settings.boundaryStitchSubdivisions > 1u && z + 1u == side)
          boundaryHeight = std::min(boundaryHeight,
              boundaryMinimum(3u, x));
        if (std::isfinite(boundaryHeight))
        {
          const uint32_t boundaryTop = std::min(
              static_cast<uint32_t>(floorFromHeight(boundaryHeight)),
              voxel.maximumVerticalVoxels - 1u);
          const uint32_t stitchFloor = boundaryTop > closure
              ? boundaryTop - closure
              : 0u;
          if (stitchFloor < firstSolidY)
          {
            localMetrics.boundaryStitchVoxelCount +=
                firstSolidY - stitchFloor;
            firstSolidY = stitchFloor;
          }
        }
      }
      else if (voxel.surfaceShellDepthVoxels != 0u &&
               top > voxel.surfaceShellDepthVoxels)
      {
        const uint32_t shellFloor = top - voxel.surfaceShellDepthVoxels;
        firstSolidY = std::min(shellFloor, lowestNeighbour);
      }
      if (!settings.exteriorShellOnly)
        firstSolidYWithoutBoundaryStitch = firstSolidY;
      localMetrics.omittedInteriorVoxelCount += firstSolidY;
      const auto boundaryContext = [&]()
      {
        if (x == 0u)
          return SparseVoxelMicroContext::WallNegativeX;
        if (x + 1u == side)
          return SparseVoxelMicroContext::WallPositiveX;
        if (z == 0u)
          return SparseVoxelMicroContext::WallNegativeZ;
        if (z + 1u == side)
          return SparseVoxelMicroContext::WallPositiveZ;
        return SparseVoxelMicroContext::Interior;
      }();
      uint32_t wallThreshold = top;
      SparseVoxelMicroContext wallContext = SparseVoxelMicroContext::Interior;
      const auto considerWall =
          [&](const int32_t neighbourX, const int32_t neighbourZ,
              const SparseVoxelMicroContext candidate)
      {
        const uint32_t neighbourTop = surfaceTop(neighbourX, neighbourZ);
        // Strict comparison is the canonical tie-break: -X, +X, -Z, +Z are
        // visited in that order and the first lowest neighbour retains
        // ownership when two faces expose the same vertical run.
        if (neighbourTop < wallThreshold)
        {
          wallThreshold = neighbourTop;
          wallContext = candidate;
        }
      };
      considerWall(static_cast<int32_t>(x) - 1, static_cast<int32_t>(z),
          SparseVoxelMicroContext::WallNegativeX);
      considerWall(static_cast<int32_t>(x) + 1, static_cast<int32_t>(z),
          SparseVoxelMicroContext::WallPositiveX);
      considerWall(static_cast<int32_t>(x), static_cast<int32_t>(z) - 1,
          SparseVoxelMicroContext::WallNegativeZ);
      considerWall(static_cast<int32_t>(x), static_cast<int32_t>(z) + 1,
          SparseVoxelMicroContext::WallPositiveZ);
      auto &canonicalColumn = canonicalColumns.columns[column];
      canonicalColumn.topY = top;
      canonicalColumn.firstSolidY = firstSolidY;
      canonicalColumn.packedMaterialContext =
          static_cast<uint32_t>(materials[column]) |
          (static_cast<uint32_t>(SparseVoxelMicroContext::FlatSurface) << 8u);
      canonicalColumn.packedFlags = settings.exteriorShellOnly
          ? WorldMapTerrainColumnExteriorShell : 0u;
      if (firstSolidY < firstSolidYWithoutBoundaryStitch)
        canonicalColumn.packedFlags |= WorldMapTerrainColumnBoundaryStitched;
      auto &canonicalWall = canonicalColumns.walls[column];
      canonicalWall.firstSolidYWithoutBoundaryStitch =
          firstSolidYWithoutBoundaryStitch;
      canonicalWall.lowestNeighbourY = wallThreshold;
      canonicalWall.packedContexts = static_cast<uint32_t>(wallContext) |
          (static_cast<uint32_t>(boundaryContext) << 8u);
      localMetrics.emittedShellVoxelCount +=
          uint64_t{top} - firstSolidY + 1u;
      for (uint32_t y = firstSolidYWithoutBoundaryStitch;
           settings.exteriorShellOnly && y < top && y <= lowestNeighbour; ++y)
      {
        const uint32_t firstExposedSide = lowestNeighbour < top
            ? lowestNeighbour + 1u : top;
        const uint32_t closureDepth =
            std::min(top - y, firstExposedSide - y);
        localMetrics.maximumUnexposedDepthVoxels = std::max(
            localMetrics.maximumUnexposedDepthVoxels, closureDepth);
      }
    }
    if ((z & 7u) == 7u || z + 1u == side)
      report(progress, "Voxelizing generated world terrain", 0.40 + 0.35 * static_cast<double>(z + 1u) / side);
  }

  PlanetVoxelChunk chunk{};
  localMetrics.shellExtractionMilliseconds = millisecondsSince(
      shellStart, std::chrono::steady_clock::now());
  const auto hierarchyStart = std::chrono::steady_clock::now();
  report(progress, "Building generated world voxel bricks", 0.78);
  WorldMapTerrainColumnHierarchyPayload columnHierarchy{};
  if (!buildWorldMapTerrainColumnHierarchy(canonicalColumns,
          voxel.maximumBricks, columnHierarchy, error))
    return false;
  chunk.bricks = std::move(columnHierarchy.hierarchy);
  std::vector<uint32_t> surfaceMaterials =
      std::move(columnHierarchy.surfaceMaterials);
  localMetrics.hierarchyBuildMilliseconds = millisecondsSince(
      hierarchyStart, std::chrono::steady_clock::now());
  chunk.requestedTile = {0u, 0u, 0u, 0u};
  chunk.sourceTile = chunk.requestedTile;
  chunk.edgeMetres = edge;
  chunk.voxelSizeMetres = voxelSize;
  chunk.elevationOriginMetres = elevationOrigin;
  chunk.minimumSurfaceMetres = minimumHeight;
  chunk.maximumSurfaceMetres = maximumHeight;
  const size_t centerColumn = static_cast<size_t>(side / 2u) * side + side / 2u;
  chunk.centerSurfaceMetres = heights[centerColumn];
  chunk.centerMaterial = materials[centerColumn];
  if (columnHierarchy.occupiedFineVoxelCount >
      std::numeric_limits<uint32_t>::max())
    return fail(error,
        "Generated world terrain leaf count is not representable");
  chunk.leafVoxelCount = static_cast<uint32_t>(
      columnHierarchy.occupiedFineVoxelCount);
  chunk.surfaceHeightSide = haloSide;
  chunk.surfaceHeightHalo = static_cast<uint32_t>(halo);
  chunk.surfaceHeights.resize(haloHeights.size());
  std::transform(
      haloHeights.begin(),
      haloHeights.end(),
      chunk.surfaceHeights.begin(),
      [](const double value)
      {
        return static_cast<float>(value);
      });
  chunk.surfaceMaterials = std::move(surfaceMaterials);
  chunk.surfaceEnvelope.resize(haloHeights.size());
  const int32_t envelopeReach = static_cast<int32_t>(std::max(voxel.surfaceEnvelopeRadius, 1u));
  for (int32_t z = 0; z < static_cast<int32_t>(haloSide); ++z)
    for (int32_t x = 0; x < static_cast<int32_t>(haloSide); ++x)
    {
      double maximum = std::numeric_limits<double>::lowest();
      for (int32_t oz = -envelopeReach; oz <= envelopeReach; ++oz)
        for (int32_t ox = -envelopeReach; ox <= envelopeReach; ++ox)
        {
          const int32_t sx = std::clamp(x + ox, 0, static_cast<int32_t>(haloSide) - 1);
          const int32_t sz = std::clamp(z + oz, 0, static_cast<int32_t>(haloSide) - 1);
          maximum = std::max(maximum, elevationOrigin + static_cast<double>(surfaceTops[static_cast<size_t>(sz) * haloSide + sx] + 1u) * voxelSize);
        }
      chunk.surfaceEnvelope[static_cast<size_t>(z) * haloSide + x] = static_cast<float>(maximum);
    }

  const double centerX = ((minimumU + maximumU) * 0.5 - 0.5) * bundle.extentMetres;
  const double centerZ = ((minimumV + maximumV) * 0.5 - 0.5) * bundle.extentMetres;
  chunks.centerTile = chunk.requestedTile;
  chunks.edgeMetres = edge;
  chunks.voxelSizeMetres = voxelSize;
  chunks.elevationOriginMetres = elevationOrigin;
  chunks.centerSurfaceMetres = chunk.centerSurfaceMetres;
  chunks.centerMaterial = chunk.centerMaterial;
  chunks.surfaceHeightSide = haloSide;
  chunks.surfaceHeightHalo = static_cast<uint32_t>(halo);
  chunks.surfaceMaterialSide = side;
  chunks.surfaceHeights = chunk.surfaceHeights;
  chunks.surfaceEnvelope = chunk.surfaceEnvelope;
  chunks.surfaceMaterials = chunk.surfaceMaterials;
  chunks.leafVoxelCount = chunk.leafVoxelCount;
  chunks.minimumWorldMetres = {centerX - edge * 0.5, 0.0, centerZ - edge * 0.5};
  chunks.maximumWorldMetres = {centerX + edge * 0.5, maximumHeight - elevationOrigin + voxelSize, centerZ + edge * 0.5};

  const uint32_t nodeCount = static_cast<uint32_t>(chunk.bricks.nodes.size());
  const uint32_t childCount = static_cast<uint32_t>(chunk.bricks.childIndices.size());
  const uint32_t rootCount = static_cast<uint32_t>(chunk.bricks.rootIndices.size());
  const uint32_t materialCount = static_cast<uint32_t>(chunk.bricks.materialIndices.size());
  const uint32_t paletteCount = static_cast<uint32_t>(chunk.bricks.palette.size());
  chunks.bricks.nodes.resize(nodeCount);
  chunks.bricks.occupancy.resize(nodeCount);
  chunks.bricks.childIndices.resize(childCount);
  chunks.bricks.rootIndices.resize(rootCount);
  chunks.bricks.materialIndices.resize(materialCount);
  chunks.bricks.microSurfaceData.resize(chunk.bricks.microSurfaceData.empty() ? 0u : materialCount);
  chunks.bricks.palette.resize(paletteCount);
  chunks.bricks.paletteDescriptorIds.resize(paletteCount);
  uint64_t nodeRevision = 0u;
  const std::array<float, 3> translation{static_cast<float>(centerX), 0.0f, static_cast<float>(centerZ)};
  // Canonical terrain coordinates are relative to the exported map minimum,
  // not renderer world zero. World zero is the map centre and is not generally
  // divisible by every spatial-cascade voxel size. Rounding centreX/voxelSize
  // independently therefore shifted coarse pages by a finest-grid cell at
  // some LODs. Map-space page centres are exact half-page coordinates, so this
  // address remains integer and nested after the aggregate scales it back to
  // the finest lattice.
  const double mapCenterX = (minimumU + maximumU) * 0.5 * bundle.extentMetres;
  const double mapCenterZ = (minimumV + maximumV) * 0.5 * bundle.extentMetres;
  const std::array<int32_t, 3> canonicalTranslation{
    static_cast<int32_t>(std::llround(mapCenterX / voxelSize)),
    0,
    static_cast<int32_t>(std::llround(mapCenterZ / voxelSize))};
  const std::array<double, 4> canonicalWorldGrid{
    -bundle.extentMetres * 0.5, 0.0, -bundle.extentMetres * 0.5,
    voxelSize};
  std::transform(
      canonicalWorldGrid.begin(), canonicalWorldGrid.end(),
      chunks.canonicalWorldGrid.begin(),
      [](const double value) { return static_cast<float>(value); });
  if (!writeTranslatedHierarchySlot(
          chunks.bricks,
          chunk.bricks,
          translation,
          canonicalTranslation,
          1u,
          &canonicalWorldGrid,
          0u,
          {},
          0u,
          true,
          kSparseVoxelBrickInvalidIndex,
          0u,
          nodeCount,
          0u,
          childCount,
          0u,
          rootCount,
          0u,
          materialCount,
          0u,
          paletteCount,
          &nodeRevision,
          error))
    return false;
  chunks.bricks.rootCount = rootCount;
  localMetrics.brickLevelCount = chunks.bricks.levelCount;
  localMetrics.brickNodeCount = nodeCount;
  localMetrics.brickRootCount = rootCount;
  localMetrics.brickPayloadBytes =
      uint64_t{chunks.bricks.nodes.size()} *
          sizeof(chunks.bricks.nodes.front()) +
      uint64_t{chunks.bricks.occupancy.size()} *
          sizeof(chunks.bricks.occupancy.front()) +
      uint64_t{chunks.bricks.childIndices.size()} *
          sizeof(chunks.bricks.childIndices.front()) +
      uint64_t{chunks.bricks.rootIndices.size()} *
          sizeof(chunks.bricks.rootIndices.front()) +
      uint64_t{chunks.bricks.materialIndices.size()} *
          sizeof(chunks.bricks.materialIndices.front()) +
      uint64_t{chunks.bricks.microSurfaceData.size()} *
          sizeof(chunks.bricks.microSurfaceData.front()) +
      uint64_t{chunks.bricks.palette.size()} *
          sizeof(chunks.bricks.palette.front()) +
      uint64_t{chunks.bricks.paletteDescriptorIds.size()} *
          sizeof(chunks.bricks.paletteDescriptorIds.front());
  localMetrics.surfacePayloadBytes =
      uint64_t{chunks.surfaceHeights.size()} *
          sizeof(chunks.surfaceHeights.front()) +
      uint64_t{chunks.surfaceEnvelope.size()} *
          sizeof(chunks.surfaceEnvelope.front()) +
      uint64_t{chunks.surfaceMaterials.size()} *
          sizeof(chunks.surfaceMaterials.front());
  uint64_t sourceRevision = 0xcbf29ce484222325ull;
  const auto hashBytes = [&](const void *data, const size_t byteCount)
  {
    const auto *bytes = static_cast<const uint8_t *>(data);
    for (size_t byte = 0u; byte < byteCount; ++byte)
    {
      sourceRevision ^= bytes[byte];
      sourceRevision *= 0x100000001b3ull;
    }
  };
  const auto hashValue = [&](const auto &value)
  {
    hashBytes(&value, sizeof(value));
  };
  const auto hashVector = [&](const auto &values)
  {
    hashValue(values.size());
    if (!values.empty())
      hashBytes(values.data(), values.size() * sizeof(values.front()));
  };
  const uint64_t bundleRevision = bundle.bundleSignature != 0u
      ? bundle.bundleSignature
      : (bundle.seed != 0u ? bundle.seed : 1u);
  hashValue(bundleRevision);
  hashValue(settings.generatorRevision);
  hashValue(canonicalColumns.abiVersion);
  hashValue(canonicalColumns.semanticRevision);
  hashValue(settings.internalLOD);
  hashValue(finestHorizontalVoxelCount);
  hashValue(finestVoxelSize);
  hashBytes(settings.normalizedBounds.data(),
      settings.normalizedBounds.size() * sizeof(settings.normalizedBounds[0]));
  hashValue(voxel.foundationDepthVoxels);
  hashValue(voxel.surfaceShellDepthVoxels);
  hashValue(settings.exteriorShellOnly);
  hashValue(settings.boundaryStitchSubdivisions);
  hashVector(canonicalColumns.columns);
  hashVector(canonicalColumns.walls);
  hashVector(canonicalColumns.canonicalCornerHeights);
  hashVector(canonicalColumns.canonicalBoundaryMinimumHeights);
  hashVector(canonicalColumns.canonicalBoundaryMaximumHeights);
  hashVector(chunks.bricks.nodes);
  hashVector(chunks.bricks.occupancy);
  hashVector(chunks.bricks.childIndices);
  hashVector(chunks.bricks.rootIndices);
  hashVector(chunks.bricks.materialIndices);
  hashVector(chunks.bricks.microSurfaceData);
  hashVector(chunks.bricks.palette);
  hashVector(chunks.bricks.paletteDescriptorIds);
  hashVector(chunks.surfaceHeights);
  hashVector(chunks.surfaceEnvelope);
  hashVector(chunks.surfaceMaterials);
  if (sourceRevision == 0u)
    sourceRevision = 1u;
  localMetrics.contentRevision = sourceRevision;
  PlanetVoxelChunkRecord record{};
  record.requestedTile = chunk.requestedTile;
  record.sourceTile = chunk.sourceTile;
  record.clipmapSlot = 0u;
  record.regionMappingId = 0u;
  record.worldCenterXMetres = centerX;
  record.worldCenterZMetres = centerZ;
  record.edgeMetres = edge;
  record.voxelSizeMetres = voxelSize;
  record.brickNodeCapacity = nodeCount;
  record.brickChildCapacity = childCount;
  record.brickMaterialCapacity = materialCount;
  record.brickPaletteCapacity = paletteCount;
  // Monotonicity makes the roots' maximum the whole page's deviation.
  float measuredError = 0.0f;
  for (const uint32_t rootIndex : chunks.bricks.rootIndices)
    if (rootIndex < chunks.bricks.nodes.size())
      measuredError =
          std::max(measuredError, chunks.bricks.nodes[rootIndex].selfError);
  record.measuredErrorMetres = measuredError;
  record.brickPayloadRevision = sourceRevision;
  record.brickNodeRevision = nodeRevision;
  record.tree64Revision = nodeRevision != 0u ? nodeRevision : sourceRevision;
  record.brickRootCapacity = rootCount;
  record.brickRootCount = rootCount;
  record.elevationOriginMetres = elevationOrigin;
  record.minimumSurfaceMetres = minimumHeight;
  record.maximumSurfaceMetres = maximumHeight;
  record.centerSurfaceMetres = chunk.centerSurfaceMetres;
  record.centerMaterial = chunk.centerMaterial;
  record.leafVoxelCount = chunk.leafVoxelCount;
  record.brickCount = static_cast<uint32_t>(chunk.bricks.occupancy.size());
  chunks.chunks.push_back(record);
  chunks.clipmapLayoutRevision = sourceRevision;
  if (cancellationRequested(cancellation))
    return cancelled(error);
  if (columnPage != nullptr)
    *columnPage = std::move(canonicalColumns);
  if (metrics != nullptr)
    *metrics = localMetrics;
  report(progress, "Generated world voxel terrain ready", 1.0);
  return !chunks.bricks.empty();
}

bool operator==(
    const WorldVoxelChunkAddress &left,
    const WorldVoxelChunkAddress &right)
{
  return left.x == right.x && left.z == right.z &&
      left.spatialLOD == right.spatialLOD;
}

namespace
{

// Coarsest first, then x, then z. A linear walk of entries sorted this way
// visits every parent before any of its children, which is exactly the
// dependency order an install has to follow.
bool worldVoxelPageOrderBefore(
    const WorldVoxelChunkAddress &left, const WorldVoxelChunkAddress &right)
{
  if (left.spatialLOD != right.spatialLOD)
    return left.spatialLOD > right.spatialLOD;
  if (left.x != right.x)
    return left.x < right.x;
  return left.z < right.z;
}

// Rounds toward negative infinity, so a page west or north of the world origin
// lands on the parent that contains it rather than the one next to it.
int32_t worldVoxelPageHalf(const int32_t value)
{
  return value >= 0 ? value / 2 : -((-value + 1) / 2);
}

WorldVoxelChunkAddress worldVoxelPageParent(
    const WorldVoxelChunkAddress &address)
{
  return {
    worldVoxelPageHalf(address.x), worldVoxelPageHalf(address.z),
    address.spatialLOD + 1u};
}

uint32_t worldVoxelPageRootLOD(
    const double baseEdge, const WorldVoxelChunkStreamingSettings &settings)
{
  const uint32_t maximumLOD = std::min(
      settings.maximumSpatialLOD, kPlanetVoxelMaximumClipmapLevels - 1u);
  uint32_t rootLOD = 0u;
  while (rootLOD < maximumLOD &&
         std::ldexp(baseEdge, static_cast<int>(rootLOD)) <
             settings.renderDistanceMetres)
    ++rootLOD;
  return rootLOD;
}

bool worldVoxelPageInsideWorld(
    const WorldVoxelChunkAddress &address,
    const double baseEdge,
    const double worldExtentMetres)
{
  const double edge = std::ldexp(baseEdge, static_cast<int>(address.spatialLOD));
  const double minimumX = static_cast<double>(address.x) * edge;
  const double minimumZ = static_cast<double>(address.z) * edge;
  return minimumX + edge > 0.0 && minimumZ + edge > 0.0 &&
      minimumX < worldExtentMetres && minimumZ < worldExtentMetres;
}

} // namespace

uint32_t WorldVoxelPageDirectory::find(
    const WorldVoxelChunkAddress &address) const
{
  const auto found = std::lower_bound(
      entries.begin(), entries.end(), address,
      [](const WorldVoxelPageDirectoryEntry &entry,
         const WorldVoxelChunkAddress &key)
      {
        return worldVoxelPageOrderBefore(entry.address, key);
      });
  if (found == entries.end() || !(found->address == address))
    return kWorldVoxelInvalidPage;
  return static_cast<uint32_t>(found - entries.begin());
}

namespace
{

float worldVoxelPageMeasuredError(const PlanetVoxelChunkSet &page)
{
  return page.chunks.empty() ? 0.0f : page.chunks.front().measuredErrorMetres;
}

} // namespace

std::vector<WorldVoxelPagePayloadRecord> collectWorldVoxelPagePayloadRecords(
    const WorldVoxelChunkCache &cache)
{
  std::vector<WorldVoxelPagePayloadRecord> records;
  records.reserve(
      cache.payloadCache.size() + cache.stagedSpatialEntries.size() +
      cache.entries.size());
  const auto makeKey = [&](const uint64_t bundleSignature,
                           const WorldVoxelChunkAddress &address,
                           const uint32_t lod)
  {
    return worldTransientPageKey(
        bundleSignature != 0u ? bundleSignature : cache.bundleSignature,
        cache.settings, address, lod);
  };
  for (const auto &payload : cache.payloadCache)
  {
    records.push_back({
      payload.address,
      payload.lod,
      makeKey(payload.bundleSignature, payload.address, payload.lod),
      payload.chunk->clipmapLayoutRevision,
      worldVoxelPageMeasuredError(*payload.chunk),
      WorldVoxelPagePayloadLocation::FrontCache});
  }
  for (const auto &staged : cache.stagedSpatialEntries)
  {
    records.push_back({
      staged.address,
      staged.residentLOD,
      makeKey(cache.bundleSignature, staged.address, staged.residentLOD),
      staged.chunk->clipmapLayoutRevision,
      worldVoxelPageMeasuredError(*staged.chunk),
      WorldVoxelPagePayloadLocation::Installing});
  }
  for (const auto &resident : cache.entries)
  {
    records.push_back({
      resident.address,
      resident.residentLOD,
      makeKey(cache.bundleSignature, resident.address, resident.residentLOD),
      resident.chunk->clipmapLayoutRevision,
      worldVoxelPageMeasuredError(*resident.chunk),
      WorldVoxelPagePayloadLocation::Resident});
  }
  return records;
}

std::vector<WorldVoxelPageResidencyRecord>
collectWorldVoxelPageResidencyRecords(const WorldVoxelChunkCache &cache)
{
  std::vector<WorldVoxelPageResidencyRecord> records;
  records.reserve(cache.stagedSpatialEntries.size() + cache.entries.size());
  for (const auto &staged : cache.stagedSpatialEntries)
  {
    records.push_back({
      staged.address,
      staged.slot,
      staged.residentLOD,
      staged.generation,
      staged.allocation.id,
      WorldVoxelPageResidencyLocation::Installing});
  }
  for (const auto &resident : cache.entries)
  {
    records.push_back({
      resident.address,
      resident.slot,
      resident.residentLOD,
      resident.generation,
      resident.allocation.id,
      WorldVoxelPageResidencyLocation::Resident});
  }
  return records;
}

bool buildWorldVoxelPageDirectory(
    const double worldExtentMetres,
    const WorldVoxelChunkStreamingSettings &settings,
    const std::vector<WorldVoxelChunkAddress> &covered,
    WorldVoxelPageDirectory &directory,
    std::string *error)
{
  directory = {};
  const double baseEdge = settings.chunkEdgeMetres > 0.0
      ? settings.chunkEdgeMetres
      : settings.finestVoxelSizeMetres *
            static_cast<double>(settings.finestHorizontalVoxelCount);
  if (!(worldExtentMetres > 0.0) || !std::isfinite(worldExtentMetres) ||
      !(baseEdge > 0.0) || !std::isfinite(baseEdge) ||
      !(settings.finestVoxelSizeMetres > 0.0) ||
      !std::isfinite(settings.finestVoxelSizeMetres) ||
      !(settings.renderDistanceMetres > 0.0) ||
      !std::isfinite(settings.renderDistanceMetres) ||
      settings.maximumSpatialLOD >= kPlanetVoxelMaximumClipmapLevels)
    return fail(error, "Generated-world page directory settings are invalid");

  const uint32_t rootLOD = worldVoxelPageRootLOD(baseEdge, settings);
  std::vector<WorldVoxelChunkAddress> addresses;
  std::vector<WorldVoxelChunkAddress> coveredAddresses;
  std::vector<WorldVoxelChunkAddress> frontierAddresses;
  addresses.reserve(covered.size() * 6u + 16u);
  const auto addAddress = [&](const WorldVoxelChunkAddress &address)
  {
    if (address.spatialLOD > rootLOD ||
        !worldVoxelPageInsideWorld(address, baseEdge, worldExtentMetres))
      return;
    addresses.push_back(address);
  };

  uint32_t coveredCount = 0u;
  uint32_t frontierCount = 0u;
  for (const auto &address : covered)
  {
    if (address.spatialLOD > rootLOD)
      return fail(
          error,
          "A generated-world page is coarser than the directory root level: "
          "pageLOD=" + std::to_string(address.spatialLOD) +
              " rootLOD=" + std::to_string(rootLOD) +
              " page=(" + std::to_string(address.x) + "," +
              std::to_string(address.z) + ")");
    if (!worldVoxelPageInsideWorld(address, baseEdge, worldExtentMetres))
      continue;
    addAddress(address);
    coveredAddresses.push_back(address);
    ++coveredCount;
    // Ancestors, so the entry is reachable from a root and an install can be
    // ordered against everything above it.
    WorldVoxelChunkAddress ancestor = address;
    while (ancestor.spatialLOD < rootLOD)
    {
      ancestor = worldVoxelPageParent(ancestor);
      addAddress(ancestor);
    }
    // One level of refinement frontier.
    if (address.spatialLOD == 0u)
      continue;
    const uint32_t childLOD = address.spatialLOD - 1u;
    for (int32_t childZ = 0; childZ != 2; ++childZ)
      for (int32_t childX = 0; childX != 2; ++childX)
      {
        const WorldVoxelChunkAddress child{
          address.x * 2 + childX, address.z * 2 + childZ, childLOD};
        if (!worldVoxelPageInsideWorld(child, baseEdge, worldExtentMetres))
          continue;
        addAddress(child);
        frontierAddresses.push_back(child);
        ++frontierCount;
      }
  }
  if (addresses.empty())
  {
    // A directory with no covered pages is still well formed: it is what a
    // cold start looks like before the first cut exists.
    for (int32_t z = 0;; ++z)
    {
      bool addedRow = false;
      for (int32_t x = 0;; ++x)
      {
        const WorldVoxelChunkAddress root{x, z, rootLOD};
        if (!worldVoxelPageInsideWorld(root, baseEdge, worldExtentMetres))
          break;
        addresses.push_back(root);
        addedRow = true;
      }
      if (!addedRow)
        break;
    }
  }

  std::sort(
      addresses.begin(), addresses.end(),
      [](const WorldVoxelChunkAddress &left,
         const WorldVoxelChunkAddress &right)
      {
        return worldVoxelPageOrderBefore(left, right);
      });
  addresses.erase(
      std::unique(addresses.begin(), addresses.end()), addresses.end());

  directory.entries.reserve(addresses.size());
  for (const auto &address : addresses)
  {
    WorldVoxelPageDirectoryEntry entry{};
    entry.address = address;
    entry.edgeMetres =
        std::ldexp(baseEdge, static_cast<int>(address.spatialLOD));
    entry.minimumXMetres = static_cast<double>(address.x) * entry.edgeMetres;
    entry.minimumZMetres = static_cast<double>(address.z) * entry.edgeMetres;
    entry.errorEnvelopeMetres = std::ldexp(
        settings.finestVoxelSizeMetres, static_cast<int>(address.spatialLOD));
    directory.entries.push_back(entry);
  }

  for (uint32_t index = 0u;
       index < static_cast<uint32_t>(directory.entries.size()); ++index)
  {
    auto &entry = directory.entries[index];
    if (entry.address.spatialLOD >= rootLOD)
    {
      directory.roots.push_back(index);
      continue;
    }
    const uint32_t parent =
        directory.find(worldVoxelPageParent(entry.address));
    if (parent == kWorldVoxelInvalidPage)
      return fail(
          error,
          "A generated-world page directory entry has no reachable parent");
    entry.parent = parent;
    auto &parentEntry = directory.entries[parent];
    if (parentEntry.childCount >= parentEntry.children.size())
      return fail(
          error, "A generated-world page directory parent has five children");
    parentEntry.children[parentEntry.childCount++] = index;
  }
  for (const auto &address : coveredAddresses)
  {
    const uint32_t index = directory.find(address);
    if (index != kWorldVoxelInvalidPage)
      directory.entries[index].covered = true;
  }
  for (const auto &address : frontierAddresses)
  {
    const uint32_t index = directory.find(address);
    if (index != kWorldVoxelInvalidPage)
      directory.entries[index].frontier = true;
  }

  directory.coveredCount = coveredCount;
  directory.frontierCount = frontierCount;
  return true;
}

void annotateWorldVoxelPageDirectory(
    const WorldVoxelChunkCache &cache, WorldVoxelPageDirectory &directory)
{
  const std::vector<WorldVoxelPagePayloadRecord> payloadRecords =
      collectWorldVoxelPagePayloadRecords(cache);
  const std::vector<WorldVoxelPageResidencyRecord> residencyRecords =
      collectWorldVoxelPageResidencyRecords(cache);
  for (auto &entry : directory.entries)
  {
    entry.state = WorldVoxelPageState::Absent;
    entry.residentSlot = kWorldVoxelInvalidPage;
    entry.residentLOD = 0u;
    entry.residencyGeneration = 0u;
    entry.streamingLeaf = false;
    entry.childRangeEnabled = false;
    entry.residentFirstChildPage = kWorldVoxelInvalidPage;
    entry.residentChildCount = 0u;
  }
  for (const auto &payload : payloadRecords)
  {
    const uint32_t index = directory.find(payload.address);
    if (index == kWorldVoxelInvalidPage)
      continue;
    auto &entry = directory.entries[index];
    entry.state = WorldVoxelPageState::Cached;
    entry.payloadLOD = payload.lod;
    entry.payloadKey = payload.payloadKey;
    entry.payloadRevision = payload.payloadRevision;
    entry.measuredErrorMetres = payload.measuredErrorMetres;
  }
  for (const auto &resident : residencyRecords)
  {
    const uint32_t index = directory.find(resident.address);
    if (index == kWorldVoxelInvalidPage)
      continue;
    auto &entry = directory.entries[index];
    entry.state =
        resident.location == WorldVoxelPageResidencyLocation::Installing
        ? WorldVoxelPageState::Installing
        : WorldVoxelPageState::Resident;
    entry.residentSlot = resident.residentSlot;
    entry.residentLOD = resident.residentLOD;
    entry.residencyGeneration = resident.residencyGeneration;
    entry.streamingLeaf =
        resident.location == WorldVoxelPageResidencyLocation::Resident;
  }
}

void assignWorldVoxelPageDirectoryPayloadKeys(
    const uint64_t bundleIdentity,
    const WorldVoxelChunkStreamingSettings &settings,
    const uint32_t internalLOD,
    WorldVoxelPageDirectory &directory)
{
  for (auto &entry : directory.entries)
  {
    entry.payloadLOD = internalLOD;
    entry.payloadKey =
        worldTransientPageKey(bundleIdentity, settings, entry.address, internalLOD);
  }
}

void preserveWorldVoxelPageDirectoryState(
    const WorldVoxelPageDirectory &previous,
    WorldVoxelPageDirectory &directory)
{
  for (auto &entry : directory.entries)
  {
    const uint32_t previousIndex = previous.find(entry.address);
    if (previousIndex == kWorldVoxelInvalidPage)
      continue;
    const auto &old = previous.entries[previousIndex];
    entry.measuredErrorMetres = old.measuredErrorMetres;
    entry.payloadLOD = old.payloadLOD;
    entry.payloadKey = old.payloadKey;
    entry.payloadRevision = old.payloadRevision;
    entry.state = old.state;
    entry.residentSlot = old.residentSlot;
    entry.residentLOD = old.residentLOD;
    entry.residencyGeneration = old.residencyGeneration;
    entry.streamingLeaf = old.streamingLeaf;
    entry.childRangeEnabled = old.childRangeEnabled;
    entry.residentFirstChildPage = old.residentFirstChildPage;
    entry.residentChildCount = old.residentChildCount;
  }
}

namespace
{

bool worldVoxelPageIndexValid(
    const WorldVoxelPageDirectory &directory,
    const uint32_t index)
{
  return index < directory.entries.size();
}

bool worldVoxelPageInstalled(const WorldVoxelPageDirectoryEntry &entry)
{
  return entry.state == WorldVoxelPageState::Resident;
}

bool worldVoxelPageAllChildrenResident(
    const WorldVoxelPageDirectory &directory,
    const WorldVoxelPageDirectoryEntry &entry)
{
  if (entry.childCount == 0u ||
      entry.children[0] == kWorldVoxelInvalidPage)
    return false;
  for (uint32_t offset = 0u; offset < entry.childCount; ++offset)
  {
    const uint32_t childIndex = entry.children[offset];
    if (!worldVoxelPageIndexValid(directory, childIndex))
      return false;
    if (!worldVoxelPageInstalled(directory.entries[childIndex]))
      return false;
  }
  return true;
}

bool worldVoxelPageChildrenResident(
    const WorldVoxelPageDirectory &directory,
    const WorldVoxelPageDirectoryEntry &entry)
{
  if (!entry.childRangeEnabled || entry.residentChildCount == 0u ||
      entry.residentFirstChildPage == kWorldVoxelInvalidPage)
    return false;
  if (entry.residentChildCount != entry.childCount ||
      entry.residentFirstChildPage != entry.children[0])
    return false;
  for (uint32_t offset = 0u; offset < entry.residentChildCount; ++offset)
  {
    const uint32_t childIndex = entry.children[offset];
    if (!worldVoxelPageIndexValid(directory, childIndex))
      return false;
    if (!worldVoxelPageInstalled(directory.entries[childIndex]))
      return false;
  }
  return true;
}

void refreshWorldVoxelPageRuntimeFlags(WorldVoxelPageDirectory &directory)
{
  for (auto &entry : directory.entries)
  {
    if (!worldVoxelPageInstalled(entry))
    {
      entry.streamingLeaf = false;
      entry.childRangeEnabled = false;
      entry.residentFirstChildPage = kWorldVoxelInvalidPage;
      entry.residentChildCount = 0u;
      continue;
    }
    if (entry.childRangeEnabled &&
        (entry.residentFirstChildPage == kWorldVoxelInvalidPage ||
         entry.residentChildCount == 0u))
      entry.childRangeEnabled = false;
  }
  for (auto &entry : directory.entries)
  {
    if (!worldVoxelPageInstalled(entry))
      continue;
    entry.streamingLeaf =
        !worldVoxelPageChildrenResident(directory, entry);
  }
}

bool applyWorldVoxelTerrainPagePatch(
    WorldVoxelPageDirectory &directory,
    const TerrainPagePatch &patch,
    const uint32_t residencyGeneration,
    std::string *error)
{
  if (!worldVoxelPageIndexValid(directory, patch.pageIndex))
    return fail(error, "Generated-world terrain page patch has an invalid page");
  auto &entry = directory.entries[patch.pageIndex];
  switch (patch.kind)
  {
  case TerrainPagePatchKind::SetChildRange:
    if (!worldVoxelPageInstalled(entry))
      return fail(error, "Generated-world terrain page patch targets a non-resident parent");
    if (patch.childCount == 0u || patch.childCount != entry.childCount ||
        patch.firstChild == kWorldVoxelInvalidPage ||
        patch.firstChild != entry.children[0])
      return fail(error, "Generated-world terrain page patch has an invalid child range");
    if (!worldVoxelPageAllChildrenResident(directory, entry))
      return true;
    entry.childRangeEnabled = true;
    entry.residentFirstChildPage = patch.firstChild;
    entry.residentChildCount = patch.childCount;
    return true;
  case TerrainPagePatchKind::ClearChildRange:
    entry.childRangeEnabled = false;
    entry.residentFirstChildPage = kWorldVoxelInvalidPage;
    entry.residentChildCount = 0u;
    return true;
  case TerrainPagePatchKind::SetStreamingLeaf:
    if (worldVoxelPageInstalled(entry))
      entry.streamingLeaf = true;
    return true;
  case TerrainPagePatchKind::ClearStreamingLeaf:
    entry.streamingLeaf = false;
    return true;
  case TerrainPagePatchKind::SetPageInstalled:
    entry.state = WorldVoxelPageState::Resident;
    entry.residencyGeneration = residencyGeneration;
    if (entry.residentSlot == kWorldVoxelInvalidPage)
      entry.residentSlot = patch.pageIndex;
    return true;
  case TerrainPagePatchKind::ClearPageInstalled:
    entry.state = entry.payloadKey.source != 0u || entry.payloadKey.page != 0u ||
            entry.payloadRevision != 0u
        ? WorldVoxelPageState::Cached
        : WorldVoxelPageState::Absent;
    entry.residentSlot = kWorldVoxelInvalidPage;
    entry.residencyGeneration = 0u;
    entry.streamingLeaf = false;
    entry.childRangeEnabled = false;
    entry.residentFirstChildPage = kWorldVoxelInvalidPage;
    entry.residentChildCount = 0u;
    return true;
  }
  return fail(error, "Generated-world terrain page patch kind is invalid");
}

bool sameWorldVoxelTerrainPagePatch(
    const TerrainPagePatch &left,
    const TerrainPagePatch &right)
{
  return left.kind == right.kind && left.pageIndex == right.pageIndex &&
      left.firstChild == right.firstChild &&
      left.childCount == right.childCount && left.flags == right.flags;
}

bool worldVoxelPageHasResidentChild(
    const WorldVoxelPageDirectory &directory,
    const uint32_t pageIndex)
{
  if (!worldVoxelPageIndexValid(directory, pageIndex))
    return false;
  const auto &entry = directory.entries[pageIndex];
  for (uint32_t child = 0u; child < entry.childCount; ++child)
  {
    const uint32_t childIndex = entry.children[child];
    if (!worldVoxelPageIndexValid(directory, childIndex))
      continue;
    if (worldVoxelPageInstalled(directory.entries[childIndex]) ||
        worldVoxelPageHasResidentChild(directory, childIndex))
      return true;
  }
  return false;
}

void appendWorldVoxelPageCut(
    const WorldVoxelPageDirectory &directory,
    const uint32_t pageIndex,
    std::vector<uint32_t> &cut)
{
  if (!worldVoxelPageIndexValid(directory, pageIndex))
    return;
  const auto &entry = directory.entries[pageIndex];
  if (!worldVoxelPageInstalled(entry))
    return;
  if (worldVoxelPageChildrenResident(directory, entry))
  {
    for (uint32_t child = 0u; child < entry.childCount; ++child)
      appendWorldVoxelPageCut(directory, entry.children[child], cut);
    return;
  }
  cut.push_back(pageIndex);
}

} // namespace

bool buildWorldVoxelPageDirectoryPatchLists(
    WorldVoxelPageDirectory &directory,
    std::string *error)
{
  directory.installPatches.clear();
  directory.uninstallPatches.clear();
  directory.patchGeneration = directory.patchGeneration == 0u
      ? 1u
      : directory.patchGeneration + 1u;
  for (uint32_t index = 0u;
       index < static_cast<uint32_t>(directory.entries.size()); ++index)
  {
    auto &entry = directory.entries[index];
    entry.installPatchOffset =
        static_cast<uint32_t>(directory.installPatches.size());
    directory.installPatches.push_back({
      TerrainPagePatchKind::SetPageInstalled,
      index,
      kWorldVoxelInvalidPage,
      0u,
      0u});
    directory.installPatches.push_back({
      TerrainPagePatchKind::SetStreamingLeaf,
      index,
      kWorldVoxelInvalidPage,
      0u,
      0u});
    if (entry.parent != kWorldVoxelInvalidPage)
    {
      const auto &parent = directory.entries[entry.parent];
      if (parent.childCount == 0u ||
          parent.children[0] == kWorldVoxelInvalidPage)
        return fail(error, "Generated-world terrain page parent has no child range");
      directory.installPatches.push_back({
        TerrainPagePatchKind::SetChildRange,
        entry.parent,
        parent.children[0],
        parent.childCount,
        0u});
    }
    entry.installPatchCount =
        static_cast<uint32_t>(directory.installPatches.size()) -
        entry.installPatchOffset;

    entry.uninstallPatchOffset =
        static_cast<uint32_t>(directory.uninstallPatches.size());
    if (entry.parent != kWorldVoxelInvalidPage)
    {
      directory.uninstallPatches.push_back({
        TerrainPagePatchKind::ClearChildRange,
        entry.parent,
        kWorldVoxelInvalidPage,
        0u,
        0u});
      directory.uninstallPatches.push_back({
        TerrainPagePatchKind::SetStreamingLeaf,
        entry.parent,
        kWorldVoxelInvalidPage,
        0u,
        0u});
    }
    directory.uninstallPatches.push_back({
      TerrainPagePatchKind::ClearStreamingLeaf,
      index,
      kWorldVoxelInvalidPage,
      0u,
      0u});
    directory.uninstallPatches.push_back({
      TerrainPagePatchKind::ClearPageInstalled,
      index,
      kWorldVoxelInvalidPage,
      0u,
      0u});
    entry.uninstallPatchCount =
        static_cast<uint32_t>(directory.uninstallPatches.size()) -
        entry.uninstallPatchOffset;
  }
  if (error != nullptr)
    error->clear();
  return true;
}

bool prepareWorldVoxelPageInstall(
    const WorldVoxelPageDirectory &directory,
    const uint32_t pageIndex,
    const uint32_t residencyGeneration,
    TerrainPagePatchBatch &batch,
    std::string *error)
{
  if (!worldVoxelPageIndexValid(directory, pageIndex))
    return fail(error, "Generated-world terrain page install index is invalid");
  const auto &entry = directory.entries[pageIndex];
  if (directory.patchGeneration == 0u || entry.installPatchCount == 0u ||
      entry.installPatchOffset > directory.installPatches.size() ||
      entry.installPatchCount >
          directory.installPatches.size() - entry.installPatchOffset)
    return fail(error, "Generated-world terrain page install patches are missing");
  if (worldVoxelPageInstalled(entry) ||
      entry.state == WorldVoxelPageState::Generating)
    return fail(error, "Generated-world terrain page is not installable");
  if (entry.state != WorldVoxelPageState::Cached &&
      entry.state != WorldVoxelPageState::Installing)
    return fail(error, "Generated-world terrain page payload is not cached");
  if (entry.parent != kWorldVoxelInvalidPage &&
      !worldVoxelPageInstalled(directory.entries[entry.parent]))
    return fail(error, "Generated-world terrain page parent is not resident");

  batch = {};
  batch.directoryGeneration = directory.patchGeneration;
  batch.pageIndex = pageIndex;
  batch.residencyGeneration = residencyGeneration;
  batch.patches.insert(
      batch.patches.end(),
      directory.installPatches.begin() + entry.installPatchOffset,
      directory.installPatches.begin() + entry.installPatchOffset +
          entry.installPatchCount);
  if (error != nullptr)
    error->clear();
  return true;
}

bool commitWorldVoxelPageInstall(
    WorldVoxelPageDirectory &directory,
    const TerrainPagePatchBatch &batch,
    std::string *error)
{
  if (batch.directoryGeneration != directory.patchGeneration ||
      !worldVoxelPageIndexValid(directory, batch.pageIndex))
    return fail(error, "Generated-world terrain page install batch is stale");
  const auto &entry = directory.entries[batch.pageIndex];
  if (entry.installPatchCount != batch.patches.size())
    return fail(error, "Generated-world terrain page install batch size is invalid");
  for (uint32_t offset = 0u; offset < entry.installPatchCount; ++offset)
    if (!sameWorldVoxelTerrainPagePatch(
            batch.patches[offset],
            directory.installPatches[entry.installPatchOffset + offset]))
      return fail(
          error,
          "Generated-world terrain page install batch patches are stale");
  for (const auto &patch : batch.patches)
    if (!applyWorldVoxelTerrainPagePatch(
            directory, patch, batch.residencyGeneration, error))
      return false;
  refreshWorldVoxelPageRuntimeFlags(directory);
  return validateWorldVoxelPageResidencyOracle(directory, error);
}

bool prepareWorldVoxelPageUninstall(
    const WorldVoxelPageDirectory &directory,
    const uint32_t pageIndex,
    TerrainPagePatchBatch &batch,
    std::string *error)
{
  if (!worldVoxelPageIndexValid(directory, pageIndex))
    return fail(error, "Generated-world terrain page uninstall index is invalid");
  const auto &entry = directory.entries[pageIndex];
  if (directory.patchGeneration == 0u || entry.uninstallPatchCount == 0u ||
      entry.uninstallPatchOffset > directory.uninstallPatches.size() ||
      entry.uninstallPatchCount >
          directory.uninstallPatches.size() - entry.uninstallPatchOffset)
    return fail(error, "Generated-world terrain page uninstall patches are missing");
  if (!worldVoxelPageInstalled(entry))
    return fail(error, "Generated-world terrain page is not resident");
  if (worldVoxelPageHasResidentChild(directory, pageIndex))
    return fail(
        error,
        "Generated-world terrain page must uninstall resident children first");

  batch = {};
  batch.directoryGeneration = directory.patchGeneration;
  batch.pageIndex = pageIndex;
  batch.patches.insert(
      batch.patches.end(),
      directory.uninstallPatches.begin() + entry.uninstallPatchOffset,
      directory.uninstallPatches.begin() + entry.uninstallPatchOffset +
          entry.uninstallPatchCount);
  if (error != nullptr)
    error->clear();
  return true;
}

bool commitWorldVoxelPageUninstall(
    WorldVoxelPageDirectory &directory,
    const TerrainPagePatchBatch &batch,
    std::string *error)
{
  if (batch.directoryGeneration != directory.patchGeneration ||
      !worldVoxelPageIndexValid(directory, batch.pageIndex))
    return fail(error, "Generated-world terrain page uninstall batch is stale");
  const auto &entry = directory.entries[batch.pageIndex];
  if (entry.uninstallPatchCount != batch.patches.size())
    return fail(error, "Generated-world terrain page uninstall batch size is invalid");
  for (uint32_t offset = 0u; offset < entry.uninstallPatchCount; ++offset)
    if (!sameWorldVoxelTerrainPagePatch(
            batch.patches[offset],
            directory.uninstallPatches[entry.uninstallPatchOffset + offset]))
      return fail(
          error,
          "Generated-world terrain page uninstall batch patches are stale");
  for (const auto &patch : batch.patches)
    if (!applyWorldVoxelTerrainPagePatch(directory, patch, 0u, error))
      return false;
  refreshWorldVoxelPageRuntimeFlags(directory);
  return validateWorldVoxelPageResidencyOracle(directory, error);
}

std::vector<uint32_t> selectedWorldVoxelPageCut(
    const WorldVoxelPageDirectory &directory)
{
  std::vector<uint32_t> cut;
  for (const uint32_t root : directory.roots)
    appendWorldVoxelPageCut(directory, root, cut);
  return cut;
}

bool validateWorldVoxelPageResidencyOracle(
    const WorldVoxelPageDirectory &directory,
    std::string *error)
{
  for (uint32_t index = 0u;
       index < static_cast<uint32_t>(directory.entries.size()); ++index)
  {
    const auto &entry = directory.entries[index];
    if (!worldVoxelPageInstalled(entry))
      continue;
    if (entry.parent != kWorldVoxelInvalidPage &&
        !worldVoxelPageInstalled(directory.entries[entry.parent]))
      return fail(
          error,
          "Generated-world terrain page is resident without its parent");
    if (entry.childRangeEnabled)
    {
      if (entry.residentFirstChildPage == kWorldVoxelInvalidPage ||
          entry.residentChildCount == 0u ||
          entry.residentChildCount != entry.childCount ||
          entry.residentFirstChildPage != entry.children[0])
        return fail(
            error,
            "Generated-world terrain page has an invalid resident child range");
      if (!worldVoxelPageAllChildrenResident(directory, entry))
        return fail(
            error,
            "Generated-world terrain page child range exposes a non-resident child");
      for (uint32_t offset = 0u; offset < entry.residentChildCount; ++offset)
      {
        const uint32_t childIndex = entry.children[offset];
        if (!worldVoxelPageIndexValid(directory, childIndex))
          return fail(
              error,
              "Generated-world terrain page child range leaves the directory");
        if (directory.entries[childIndex].parent != index)
          return fail(
              error,
              "Generated-world terrain page child range is not owned by parent");
      }
    }
  }

  const std::vector<uint32_t> cut = selectedWorldVoxelPageCut(directory);
  for (const uint32_t pageIndex : cut)
  {
    if (!worldVoxelPageIndexValid(directory, pageIndex) ||
        !worldVoxelPageInstalled(directory.entries[pageIndex]) ||
        !directory.entries[pageIndex].streamingLeaf)
      return fail(
          error,
          "Generated-world selected terrain cut contains a disabled leaf");
  }
  for (size_t left = 0u; left < cut.size(); ++left)
    for (size_t right = left + 1u; right < cut.size(); ++right)
    {
      uint32_t walk = cut[right];
      while (worldVoxelPageIndexValid(directory, walk) &&
             directory.entries[walk].parent != kWorldVoxelInvalidPage)
      {
        walk = directory.entries[walk].parent;
        if (walk == cut[left])
          return fail(
              error,
              "Generated-world selected terrain cut contains parent and child");
      }
      walk = cut[left];
      while (worldVoxelPageIndexValid(directory, walk) &&
             directory.entries[walk].parent != kWorldVoxelInvalidPage)
      {
        walk = directory.entries[walk].parent;
        if (walk == cut[right])
          return fail(
              error,
              "Generated-world selected terrain cut contains child and parent");
      }
    }
  if (error != nullptr)
    error->clear();
  return true;
}

std::vector<uint32_t> planWorldVoxelPageInstalls(
    const WorldVoxelPageDirectory &directory,
    const std::vector<WorldVoxelChunkAddress> &targetCut)
{
  std::vector<uint8_t> wanted(directory.entries.size(), 0u);
  for (const auto &address : targetCut)
  {
    uint32_t walk = directory.find(address);
    while (worldVoxelPageIndexValid(directory, walk) && wanted[walk] == 0u)
    {
      wanted[walk] = 1u;
      const uint32_t parent = directory.entries[walk].parent;
      if (parent == kWorldVoxelInvalidPage)
        break;
      walk = parent;
    }
  }
  std::vector<uint32_t> result;
  for (uint32_t index = 0u;
       index < static_cast<uint32_t>(directory.entries.size()); ++index)
    if (wanted[index] != 0u && !worldVoxelPageInstalled(directory.entries[index]))
      result.push_back(index);
  return result;
}

std::vector<uint32_t> planWorldVoxelPageUninstalls(
    const WorldVoxelPageDirectory &directory,
    const std::vector<WorldVoxelChunkAddress> &targetCut)
{
  std::vector<uint8_t> protectedClosure(directory.entries.size(), 0u);
  for (const auto &address : targetCut)
  {
    uint32_t walk = directory.find(address);
    while (worldVoxelPageIndexValid(directory, walk))
    {
      protectedClosure[walk] = 1u;
      const uint32_t parent = directory.entries[walk].parent;
      if (parent == kWorldVoxelInvalidPage)
        break;
      walk = parent;
    }
  }
  std::vector<uint32_t> result;
  for (uint32_t index = static_cast<uint32_t>(directory.entries.size());
       index-- > 0u;)
    if (worldVoxelPageInstalled(directory.entries[index]) &&
        protectedClosure[index] == 0u)
      result.push_back(index);
  return result;
}

WorldVoxelPageStreamingPlan planWorldVoxelPageStreaming(
    const WorldVoxelPageDirectory &directory,
    const std::vector<WorldVoxelChunkAddress> &targetCut)
{
  WorldVoxelPageStreamingPlan plan{};
  const std::vector<uint32_t> installClosure =
      planWorldVoxelPageInstalls(directory, targetCut);
  plan.payloadRequests.reserve(installClosure.size());
  plan.readyInstalls.reserve(installClosure.size());
  plan.blockedInstalls.reserve(installClosure.size());

  for (const uint32_t pageIndex : installClosure)
  {
    if (!worldVoxelPageIndexValid(directory, pageIndex))
      continue;
    const auto &entry = directory.entries[pageIndex];
    const bool parentResident =
        entry.parent == kWorldVoxelInvalidPage ||
        worldVoxelPageInstalled(directory.entries[entry.parent]);
    switch (entry.state)
    {
    case WorldVoxelPageState::Cached:
    case WorldVoxelPageState::Installing:
      if (parentResident)
        plan.readyInstalls.push_back(pageIndex);
      else
        plan.blockedInstalls.push_back(pageIndex);
      break;
    case WorldVoxelPageState::Absent:
      plan.payloadRequests.push_back(pageIndex);
      plan.blockedInstalls.push_back(pageIndex);
      break;
    case WorldVoxelPageState::Generating:
      plan.blockedInstalls.push_back(pageIndex);
      break;
    case WorldVoxelPageState::Resident:
      break;
    }
  }

  plan.uninstalls = planWorldVoxelPageUninstalls(directory, targetCut);
  return plan;
}

namespace
{

bool worldVoxelPageHasPayloadForRuntime(
    const WorldVoxelPageDirectoryEntry &entry)
{
  return entry.state == WorldVoxelPageState::Installing ||
      entry.state == WorldVoxelPageState::Resident;
}

uint32_t worldVoxelRuntimePageFlags(const WorldVoxelPageDirectoryEntry &entry)
{
  uint32_t flags = kWorldVoxelTerrainRuntimePageRequestable;
  if (worldVoxelPageInstalled(entry))
    flags |= kWorldVoxelTerrainRuntimePageResident;
  if (worldVoxelPageHasPayloadForRuntime(entry))
    flags |= kWorldVoxelTerrainRuntimePagePayloadInstalled;
  if (entry.childRangeEnabled)
    flags |= kWorldVoxelTerrainRuntimePageChildRangeEnabled;
  if (entry.streamingLeaf)
    flags |= kWorldVoxelTerrainRuntimePageStreamingLeaf;
  return flags;
}

bool worldVoxelRuntimePageVisible(
    const WorldVoxelTerrainRuntimePageGPU &page)
{
  // Installing owns a physical payload before it joins the resident cut. The
  // payload and resident flags deliberately have different lifetimes; only a
  // resident streaming leaf is eligible for the compact raster root set.
  constexpr uint32_t required =
      kWorldVoxelTerrainRuntimePageResident |
      kWorldVoxelTerrainRuntimePagePayloadInstalled |
      kWorldVoxelTerrainRuntimePageStreamingLeaf;
  return (page.flags & required) == required;
}

uint32_t appendWorldVoxelRuntimeNodePlaceholder(
    WorldVoxelTerrainRuntimePageTableGPU &table)
{
  table.nodes.push_back({});
  return static_cast<uint32_t>(table.nodes.size() - 1u);
}

void writeWorldVoxelRuntimeBrickNode(
    WorldVoxelTerrainRuntimePageTableGPU &table,
    const SparseVoxelBrickHierarchy &hierarchy,
    const WorldVoxelChunkEntry &chunk,
    const uint32_t pageIndex,
    const uint32_t runtimeNodeIndex,
    const uint32_t localNodeIndex,
    const bool pageResident)
{
  if (localNodeIndex >= hierarchy.nodes.size() ||
      runtimeNodeIndex >= table.nodes.size())
    return;

  const auto &source = hierarchy.nodes[localNodeIndex];
  std::vector<uint32_t> childLocalIndices;
  if (source.childCount != 0u &&
      source.firstChild <= hierarchy.childIndices.size() &&
      source.childCount <= hierarchy.childIndices.size() - source.firstChild)
  {
    childLocalIndices.reserve(source.childCount);
    for (uint32_t child = 0u; child < source.childCount; ++child)
    {
      const uint32_t localChild =
          hierarchy.childIndices[source.firstChild + child];
      if (localChild < hierarchy.nodes.size())
        childLocalIndices.push_back(localChild);
    }
  }

  uint32_t firstChildNode = kWorldVoxelInvalidPage;
  if (!childLocalIndices.empty())
  {
    firstChildNode = static_cast<uint32_t>(table.nodes.size());
    for (uint32_t child = 0u; child < childLocalIndices.size(); ++child)
      appendWorldVoxelRuntimeNodePlaceholder(table);
  }

  WorldVoxelTerrainRuntimeNodeGPU runtime{};
  const float voxelSize = source.minimumVoxelSize[3];
  const float extent = voxelSize * static_cast<float>(kSparseVoxelBrickSide);
  runtime.worldBoundsMinError = {
    source.minimumVoxelSize[0],
    source.minimumVoxelSize[1],
    source.minimumVoxelSize[2],
    source.selfError > 0.0f ? source.selfError : voxelSize};
  runtime.worldBoundsMaxError = {
    source.minimumVoxelSize[0] + extent,
    source.minimumVoxelSize[1] + extent,
    source.minimumVoxelSize[2] + extent,
    source.parentError};
  runtime.owningPage = pageIndex;
  runtime.firstChildNode = firstChildNode;
  runtime.childCount = static_cast<uint32_t>(childLocalIndices.size());
  runtime.flags = kWorldVoxelTerrainRuntimeNodeDrawFallback;
  if (pageResident)
    runtime.flags |= kWorldVoxelTerrainRuntimeNodeResident;
  runtime.payloadNodeOffset = chunk.allocation.nodes.offset + localNodeIndex;
  runtime.payloadNodeCount = 1u;
  table.nodes[runtimeNodeIndex] = runtime;

  for (uint32_t child = 0u; child < childLocalIndices.size(); ++child)
    writeWorldVoxelRuntimeBrickNode(
        table, hierarchy, chunk, pageIndex, firstChildNode + child,
        childLocalIndices[child], pageResident);
}

void attachWorldVoxelRuntimePageNodes(
    WorldVoxelTerrainRuntimePageTableGPU &table,
    const WorldVoxelPageDirectory &directory,
    const WorldVoxelChunkEntry &chunk)
{
  const uint32_t pageIndex = directory.find(chunk.address);
  if (!worldVoxelPageIndexValid(directory, pageIndex) ||
      pageIndex >= table.pages.size())
    return;
  const auto &entry = directory.entries[pageIndex];
  if (chunk.chunk->gpuOwnedGeneratedPayloads)
  {
    if (!worldVoxelPageHasPayloadForRuntime(entry) ||
        table.pages[pageIndex].nodeCount != 0u ||
        chunk.chunk->chunks.empty())
      return;
    const auto &record = chunk.chunk->chunks.front();
    if (record.brickRootCount == 0u ||
        chunk.allocation.roots.capacity < record.brickRootCount)
      return;

    const uint32_t rootNode =
        appendWorldVoxelRuntimeNodePlaceholder(table);
    WorldVoxelTerrainRuntimeNodeGPU syntheticRoot{};
    const float minimumY = std::isfinite(chunk.chunk->minimumWorldMetres[1])
        ? static_cast<float>(chunk.chunk->minimumWorldMetres[1]) : 0.0f;
    const float maximumY = std::isfinite(chunk.chunk->maximumWorldMetres[1])
        ? static_cast<float>(chunk.chunk->maximumWorldMetres[1]) : minimumY;
    syntheticRoot.worldBoundsMinError = {
      static_cast<float>(entry.minimumXMetres), minimumY,
      static_cast<float>(entry.minimumZMetres),
      entry.measuredErrorMetres > 0.0f
          ? entry.measuredErrorMetres
          : static_cast<float>(entry.errorEnvelopeMetres)};
    syntheticRoot.worldBoundsMaxError = {
      static_cast<float>(entry.minimumXMetres + entry.edgeMetres), maximumY,
      static_cast<float>(entry.minimumZMetres + entry.edgeMetres),
      static_cast<float>(entry.errorEnvelopeMetres)};
    syntheticRoot.owningPage = pageIndex;
    syntheticRoot.flags = kWorldVoxelTerrainRuntimeNodeDrawFallback;
    if (worldVoxelPageInstalled(entry))
      syntheticRoot.flags |= kWorldVoxelTerrainRuntimeNodeResident;
    if (entry.streamingLeaf)
      syntheticRoot.flags |= kWorldVoxelTerrainRuntimeNodeStreamingLeaf;
    // Unlike the CPU hierarchy, the GPU builder writes roots directly into
    // their stable physical arena range. Do not compact or renumber it.
    syntheticRoot.fallbackBrickRoot = chunk.allocation.roots.offset;
    syntheticRoot.fallbackBrickRootCount = record.brickRootCount;
    syntheticRoot.payloadNodeOffset = chunk.allocation.nodes.offset;
    syntheticRoot.payloadNodeCount = chunk.allocation.nodes.capacity;
    table.nodes[rootNode] = syntheticRoot;
    auto &page = table.pages[pageIndex];
    page.firstNode = rootNode;
    page.rootNode = rootNode;
    page.nodeCount = 1u;
    return;
  }
  if (!worldVoxelPageHasPayloadForRuntime(entry) ||
      table.pages[pageIndex].nodeCount != 0u ||
      chunk.chunk->bricks.nodes.empty() ||
      chunk.chunk->bricks.rootIndices.empty())
    return;

  const bool pageResident = worldVoxelPageInstalled(entry);
  const uint32_t firstNode = static_cast<uint32_t>(table.nodes.size());
  const uint32_t rootNode = appendWorldVoxelRuntimeNodePlaceholder(table);

  std::vector<uint32_t> rootLocalIndices;
  rootLocalIndices.reserve(chunk.chunk->bricks.rootIndices.size());
  for (const uint32_t localRoot : chunk.chunk->bricks.rootIndices)
    if (localRoot < chunk.chunk->bricks.nodes.size())
      rootLocalIndices.push_back(localRoot);

  uint32_t firstChildNode = kWorldVoxelInvalidPage;
  if (!rootLocalIndices.empty())
  {
    firstChildNode = static_cast<uint32_t>(table.nodes.size());
    for (uint32_t root = 0u; root < rootLocalIndices.size(); ++root)
      appendWorldVoxelRuntimeNodePlaceholder(table);
  }

  WorldVoxelTerrainRuntimeNodeGPU syntheticRoot{};
  const float minimumY =
      std::isfinite(chunk.chunk->minimumWorldMetres[1])
      ? static_cast<float>(chunk.chunk->minimumWorldMetres[1])
      : 0.0f;
  const float maximumY =
      std::isfinite(chunk.chunk->maximumWorldMetres[1])
      ? static_cast<float>(chunk.chunk->maximumWorldMetres[1])
      : minimumY;
  syntheticRoot.worldBoundsMinError = {
    static_cast<float>(entry.minimumXMetres),
    minimumY,
    static_cast<float>(entry.minimumZMetres),
    entry.measuredErrorMetres > 0.0f
        ? entry.measuredErrorMetres
        : static_cast<float>(entry.errorEnvelopeMetres)};
  syntheticRoot.worldBoundsMaxError = {
    static_cast<float>(entry.minimumXMetres + entry.edgeMetres),
    maximumY,
    static_cast<float>(entry.minimumZMetres + entry.edgeMetres),
    static_cast<float>(entry.errorEnvelopeMetres)};
  syntheticRoot.owningPage = pageIndex;
  syntheticRoot.firstChildNode = firstChildNode;
  syntheticRoot.childCount = static_cast<uint32_t>(rootLocalIndices.size());
  syntheticRoot.flags = kWorldVoxelTerrainRuntimeNodeDrawFallback;
  if (pageResident)
    syntheticRoot.flags |= kWorldVoxelTerrainRuntimeNodeResident;
  if (entry.streamingLeaf)
    syntheticRoot.flags |= kWorldVoxelTerrainRuntimeNodeStreamingLeaf;
  syntheticRoot.fallbackBrickRoot = chunk.allocation.roots.offset;
  syntheticRoot.fallbackBrickRootCount =
      static_cast<uint32_t>(rootLocalIndices.size());
  syntheticRoot.payloadNodeOffset = chunk.allocation.nodes.offset;
  syntheticRoot.payloadNodeCount = chunk.allocation.nodes.capacity;
  table.nodes[rootNode] = syntheticRoot;

  for (uint32_t root = 0u; root < rootLocalIndices.size(); ++root)
    writeWorldVoxelRuntimeBrickNode(
        table, chunk.chunk->bricks, chunk, pageIndex, firstChildNode + root,
        rootLocalIndices[root], pageResident);

  auto &page = table.pages[pageIndex];
  page.firstNode = firstNode;
  page.rootNode = rootNode;
  page.nodeCount = static_cast<uint32_t>(table.nodes.size()) - firstNode;
}

} // namespace

WorldVoxelTerrainRuntimePageTableGPU buildWorldVoxelTerrainRuntimePageTable(
    const WorldVoxelPageDirectory &directory)
{
  WorldVoxelTerrainRuntimePageTableGPU table{};
  table.pages.reserve(directory.entries.size());
  table.rootPages = directory.roots;
  uint32_t childReferenceCount = 0u;
  for (const auto &entry : directory.entries)
    childReferenceCount += entry.childCount;
  table.childPageIndices.reserve(childReferenceCount);

  for (const auto &entry : directory.entries)
  {
    WorldVoxelTerrainRuntimePageGPU page{};
    page.parentPage = entry.parent;
    if (entry.childCount != 0u)
    {
      page.firstChildPage =
          static_cast<uint32_t>(table.childPageIndices.size());
      page.childCount = entry.childCount;
      for (uint32_t child = 0u; child < entry.childCount; ++child)
        table.childPageIndices.push_back(entry.children[child]);
    }
    page.flags = worldVoxelRuntimePageFlags(entry);
    page.payloadSlot = worldVoxelPageHasPayloadForRuntime(entry)
        ? entry.residentSlot
        : kWorldVoxelInvalidPage;
    page.residencyGeneration = worldVoxelPageHasPayloadForRuntime(entry)
        ? entry.residencyGeneration
        : 0u;
    page.payloadRevisionLo = static_cast<uint32_t>(entry.payloadRevision);
    page.payloadRevisionHi =
        static_cast<uint32_t>(entry.payloadRevision >> 32u);
    table.pages.push_back(page);
  }

  return table;
}

WorldVoxelTerrainRuntimePageTableGPU buildWorldVoxelTerrainRuntimePageTable(
    const WorldVoxelPageDirectory &directory,
    const WorldVoxelChunkCache &cache)
{
  WorldVoxelTerrainRuntimePageTableGPU table =
      buildWorldVoxelTerrainRuntimePageTable(directory);
  for (const auto &chunk : cache.stagedSpatialEntries)
    attachWorldVoxelRuntimePageNodes(table, directory, chunk);
  for (const auto &chunk : cache.entries)
    attachWorldVoxelRuntimePageNodes(table, directory, chunk);
  return table;
}

bool validateWorldVoxelTerrainRuntimePageTable(
    const WorldVoxelPageDirectory &directory,
    const WorldVoxelTerrainRuntimePageTableGPU &table,
    std::string *error)
{
  if (table.pages.size() != directory.entries.size())
    return fail(error, "Generated-world runtime page table size is invalid");
  if (table.rootPages != directory.roots)
    return fail(error, "Generated-world runtime page roots are stale");
  for (const uint32_t root : table.rootPages)
    if (!worldVoxelPageIndexValid(directory, root) ||
        table.pages[root].parentPage != kWorldVoxelInvalidPage)
      return fail(error, "Generated-world runtime page root is invalid");

  for (uint32_t index = 0u;
       index < static_cast<uint32_t>(directory.entries.size()); ++index)
  {
    const auto &entry = directory.entries[index];
    const auto &page = table.pages[index];
    if (page.parentPage != entry.parent)
      return fail(error, "Generated-world runtime page parent is stale");
    if (page.flags != worldVoxelRuntimePageFlags(entry))
      return fail(error, "Generated-world runtime page flags are stale");
    if (page.nodeCount == 0u)
    {
      if (page.firstNode != kWorldVoxelInvalidPage ||
          page.rootNode != kWorldVoxelInvalidPage)
        return fail(error, "Generated-world runtime page has a stray node span");
    }
    else
    {
      if (page.firstNode == kWorldVoxelInvalidPage ||
          page.rootNode == kWorldVoxelInvalidPage ||
          page.firstNode > table.nodes.size() ||
          page.nodeCount > table.nodes.size() - page.firstNode ||
          page.rootNode < page.firstNode ||
          page.rootNode >= page.firstNode + page.nodeCount)
        return fail(error, "Generated-world runtime page node span is invalid");
      if (table.nodes[page.rootNode].owningPage != index)
        return fail(error, "Generated-world runtime page root owner is stale");
    }
    if (page.childCount != entry.childCount)
      return fail(error, "Generated-world runtime page child count is stale");
    if (entry.childCount == 0u)
    {
      if (page.firstChildPage != kWorldVoxelInvalidPage)
        return fail(
            error,
            "Generated-world runtime leaf page has child references");
    }
    else
    {
      if (page.firstChildPage == kWorldVoxelInvalidPage ||
          page.firstChildPage > table.childPageIndices.size() ||
          page.childCount >
              table.childPageIndices.size() - page.firstChildPage)
        return fail(
            error,
            "Generated-world runtime page child references are invalid");
      for (uint32_t child = 0u; child < entry.childCount; ++child)
      {
        const uint32_t childIndex =
            table.childPageIndices[page.firstChildPage + child];
        if (childIndex != entry.children[child] ||
            !worldVoxelPageIndexValid(directory, childIndex) ||
            directory.entries[childIndex].parent != index)
          return fail(
              error,
              "Generated-world runtime page child reference is stale");
      }
    }

    const bool payloadInstalled = (page.flags &
        kWorldVoxelTerrainRuntimePagePayloadInstalled) != 0u;
    if (payloadInstalled != worldVoxelPageHasPayloadForRuntime(entry))
      return fail(
          error,
          "Generated-world runtime page payload flag is stale");
    if (payloadInstalled)
    {
      if (page.payloadSlot != entry.residentSlot ||
          page.residencyGeneration != entry.residencyGeneration)
        return fail(
            error,
            "Generated-world runtime page residency payload is stale");
    }
    else if (
        page.payloadSlot != kWorldVoxelInvalidPage ||
        page.residencyGeneration != 0u)
      return fail(
          error,
          "Generated-world runtime page has a stray residency payload");

    const uint64_t payloadRevision =
        uint64_t{page.payloadRevisionLo} |
        (uint64_t{page.payloadRevisionHi} << 32u);
    if (payloadRevision != entry.payloadRevision)
      return fail(error, "Generated-world runtime page revision is stale");
  }
  for (uint32_t nodeIndex = 0u;
       nodeIndex < static_cast<uint32_t>(table.nodes.size()); ++nodeIndex)
  {
    const auto &node = table.nodes[nodeIndex];
    if (!worldVoxelPageIndexValid(directory, node.owningPage))
      return fail(error, "Generated-world runtime node owner is invalid");
    const auto &page = table.pages[node.owningPage];
    if (nodeIndex < page.firstNode || nodeIndex >= page.firstNode + page.nodeCount)
      return fail(error, "Generated-world runtime node leaves its page span");
    if (node.childCount == 0u)
    {
      if (node.firstChildNode != kWorldVoxelInvalidPage)
        return fail(error, "Generated-world runtime leaf node has children");
    }
    else
    {
      if (node.firstChildNode == kWorldVoxelInvalidPage ||
          node.firstChildNode < page.firstNode ||
          node.firstChildNode > table.nodes.size() ||
          node.childCount > table.nodes.size() - node.firstChildNode)
        return fail(error, "Generated-world runtime node children are invalid");
      for (uint32_t child = 0u; child < node.childCount; ++child)
      {
        const uint32_t childIndex = node.firstChildNode + child;
        if (childIndex >= page.firstNode + page.nodeCount ||
            table.nodes[childIndex].owningPage != node.owningPage)
          return fail(error, "Generated-world runtime node child owner is stale");
      }
    }
    if (node.payloadNodeCount != 0u &&
        node.payloadNodeOffset == kWorldVoxelInvalidPage)
      return fail(error, "Generated-world runtime node payload range is invalid");
  }
  if (error != nullptr)
    error->clear();
  return true;
}

bool buildWorldVoxelTerrainRuntimeVisibleBrickRoots(
    const WorldVoxelTerrainRuntimePageTableGPU &table,
    const SparseVoxelBrickHierarchy &bricks,
    std::vector<uint32_t> &roots,
    std::string *error)
{
  struct VisibleRootRange
  {
    uint32_t page = kWorldVoxelInvalidPage;
    uint32_t offset = 0u;
    uint32_t count = 0u;
  };

  roots.clear();
  std::vector<VisibleRootRange> ranges;
  ranges.reserve(table.pages.size());
  for (uint32_t pageIndex = 0u;
       pageIndex < static_cast<uint32_t>(table.pages.size()); ++pageIndex)
  {
    const auto &page = table.pages[pageIndex];
    if (!worldVoxelRuntimePageVisible(page))
      continue;
    if (page.rootNode == kWorldVoxelInvalidPage ||
        page.rootNode >= table.nodes.size())
      return fail(
          error,
          "Generated-world runtime visible page has no root node");
    const auto &rootNode = table.nodes[page.rootNode];
    if (rootNode.owningPage != pageIndex)
      return fail(
          error,
          "Generated-world runtime visible root owner is stale");
    if ((rootNode.flags & kWorldVoxelTerrainRuntimeNodeStreamingLeaf) == 0u)
      return fail(
          error,
          "Generated-world runtime visible root is not a streaming leaf");
    if (rootNode.fallbackBrickRoot == kWorldVoxelInvalidPage ||
        rootNode.fallbackBrickRootCount == 0u)
      return fail(
          error,
          "Generated-world runtime visible root has no brick fallback");
    if (rootNode.fallbackBrickRoot > bricks.rootIndices.size() ||
        rootNode.fallbackBrickRootCount >
            bricks.rootIndices.size() - rootNode.fallbackBrickRoot)
      return fail(
          error,
          "Generated-world runtime visible brick root range is invalid");
    ranges.push_back(
        {pageIndex, rootNode.fallbackBrickRoot,
         rootNode.fallbackBrickRootCount});
  }

  std::sort(
      ranges.begin(), ranges.end(),
      [](const VisibleRootRange &left, const VisibleRootRange &right)
      {
        if (left.offset != right.offset)
          return left.offset < right.offset;
        return left.page < right.page;
      });
  uint32_t previousEnd = 0u;
  bool hasPrevious = false;
  for (const auto &range : ranges)
  {
    if (hasPrevious && range.offset < previousEnd)
      return fail(
          error,
          "Generated-world runtime visible brick root ranges overlap");
    hasPrevious = true;
    previousEnd = range.offset + range.count;
    for (uint32_t index = 0u; index < range.count; ++index)
    {
      const uint32_t root = bricks.rootIndices[range.offset + index];
      if (root >= bricks.nodes.size())
        return fail(
            error,
            "Generated-world runtime visible brick root node is invalid");
      roots.push_back(root);
    }
  }

  if (error != nullptr)
    error->clear();
  return true;
}

bool compactWorldVoxelTerrainRuntimeVisibleBrickRoots(
    WorldVoxelTerrainRuntimePageTableGPU &table,
    std::vector<PlanetVoxelChunkRecord> &records,
    SparseVoxelBrickHierarchy &bricks,
    std::string *error)
{
  struct VisiblePageRoots
  {
    uint32_t pageIndex = kWorldVoxelInvalidPage;
    uint32_t recordIndex = kWorldVoxelInvalidPage;
    uint32_t physicalOffset = 0u;
    uint32_t compactOffset = 0u;
    uint32_t count = 0u;
  };

  std::vector<uint32_t> compactRoots;
  std::vector<VisiblePageRoots> visiblePages;
  visiblePages.reserve(table.pages.size());
  for (uint32_t pageIndex = 0u;
       pageIndex < static_cast<uint32_t>(table.pages.size()); ++pageIndex)
  {
    auto &page = table.pages[pageIndex];
    if (!worldVoxelRuntimePageVisible(page))
      continue;
    if (page.payloadSlot == kWorldVoxelInvalidPage)
      return fail(
          error,
          "Generated-world runtime visible page has no payload slot");
    const auto record = std::find_if(
        records.begin(), records.end(),
        [&](const PlanetVoxelChunkRecord &candidate)
        {
          return candidate.clipmapSlot == page.payloadSlot &&
              candidate.residencyGeneration == page.residencyGeneration;
        });
    if (record == records.end())
      return fail(
          error,
          "Generated-world runtime visible page has no compact root record");
    if (page.rootNode == kWorldVoxelInvalidPage ||
        page.rootNode >= table.nodes.size())
      return fail(
          error,
          "Generated-world runtime visible page has no patchable root node");
    if (record->brickRootCount == 0u)
      return fail(
          error,
          "Generated-world runtime compact root record is empty");
    if (record->firstBrickRoot > bricks.rootIndices.size() ||
        record->brickRootCount >
            bricks.rootIndices.size() - record->firstBrickRoot)
      return fail(
          error,
          "Generated-world runtime visible page root range exceeds arena");
    visiblePages.push_back({
      pageIndex,
      static_cast<uint32_t>(record - records.begin()),
      record->firstBrickRoot,
      0u,
      record->brickRootCount});
  }
  std::sort(
      visiblePages.begin(), visiblePages.end(),
      [](const VisiblePageRoots &left, const VisiblePageRoots &right)
      {
        if (left.physicalOffset != right.physicalOffset)
          return left.physicalOffset < right.physicalOffset;
        return left.pageIndex < right.pageIndex;
      });
  for (auto &visible : visiblePages)
  {
    visible.compactOffset = static_cast<uint32_t>(compactRoots.size());
    for (uint32_t root = 0u; root < visible.count; ++root)
    {
      const uint32_t node =
          bricks.rootIndices[visible.physicalOffset + root];
      if (node >= bricks.nodes.size())
        return fail(
            error,
            "Generated-world runtime visible page root node is invalid");
      compactRoots.push_back(node);
    }
  }
  if (compactRoots.size() > bricks.rootIndices.size())
    return fail(
        error,
        "Generated-world progressive compact root prefix exceeds arena");
  std::copy(
      compactRoots.begin(), compactRoots.end(), bricks.rootIndices.begin());
  bricks.rootCount = static_cast<uint32_t>(compactRoots.size());
  for (const auto &visible : visiblePages)
  {
    auto &rootNode = table.nodes[table.pages[visible.pageIndex].rootNode];
    rootNode.fallbackBrickRoot = visible.compactOffset;
    rootNode.fallbackBrickRootCount = visible.count;
    auto &record = records[visible.recordIndex];
    record.firstBrickRoot = visible.compactOffset;
    record.brickRootCount = visible.count;
  }
  if (error != nullptr)
    error->clear();
  return true;
}

namespace
{

bool worldVoxelPageAncestorOf(
    const WorldVoxelChunkAddress &ancestor,
    const WorldVoxelChunkAddress &descendant)
{
  if (ancestor.spatialLOD <= descendant.spatialLOD)
    return false;
  const uint32_t levels = ancestor.spatialLOD - descendant.spatialLOD;
  if (levels >= 31u)
    return false;
  WorldVoxelChunkAddress walk = descendant;
  for (uint32_t level = 0u; level < levels; ++level)
    walk = worldVoxelPageParent(walk);
  return walk.x == ancestor.x && walk.z == ancestor.z;
}

struct WorldVoxelPageBox
{
  double minimumX = 0.0;
  double minimumZ = 0.0;
  double edge = 0.0;
};

WorldVoxelPageBox worldVoxelPageBox(
    const WorldVoxelChunkAddress &address, const double baseEdge)
{
  const double edge = std::ldexp(baseEdge, static_cast<int>(address.spatialLOD));
  return {
    static_cast<double>(address.x) * edge,
    static_cast<double>(address.z) * edge, edge};
}

bool worldVoxelPagesOverlap(
    const WorldVoxelChunkAddress &left,
    const WorldVoxelChunkAddress &right,
    const double baseEdge)
{
  const WorldVoxelPageBox a = worldVoxelPageBox(left, baseEdge);
  const WorldVoxelPageBox b = worldVoxelPageBox(right, baseEdge);
  const double tolerance = std::max(a.edge, b.edge) * 1.0e-9;
  return a.minimumX < b.minimumX + b.edge - tolerance &&
      b.minimumX < a.minimumX + a.edge - tolerance &&
      a.minimumZ < b.minimumZ + b.edge - tolerance &&
      b.minimumZ < a.minimumZ + a.edge - tolerance;
}

bool worldVoxelCutCoversPage(
    const std::vector<WorldVoxelChunkAddress> &cut,
    const WorldVoxelChunkAddress &page)
{
  for (const auto &candidate : cut)
    if (candidate == page || worldVoxelPageAncestorOf(candidate, page))
      return true;

  bool hasDescendant = false;
  for (const auto &candidate : cut)
    if (worldVoxelPageAncestorOf(page, candidate))
    {
      hasDescendant = true;
      break;
    }
  if (!hasDescendant || page.spatialLOD == 0u)
    return false;

  for (uint32_t childZ = 0u; childZ < 2u; ++childZ)
    for (uint32_t childX = 0u; childX < 2u; ++childX)
    {
      const WorldVoxelChunkAddress child{
        page.x * 2 + static_cast<int32_t>(childX),
        page.z * 2 + static_cast<int32_t>(childZ),
        page.spatialLOD - 1u};
      if (!worldVoxelCutCoversPage(cut, child))
        return false;
    }
  return true;
}

bool worldVoxelCutCoversPublishedOwners(
    const std::vector<WorldVoxelChunkAddress> &selected,
    const std::vector<WorldVoxelChunkAddress> &published)
{
  for (const auto &page : published)
    if (!worldVoxelCutCoversPage(selected, page))
      return false;
  return true;
}

// Face adjacency, matching the balance rule the requested cut is built with:
// the boxes touch along an edge rather than only at a corner.
bool worldVoxelPagesFaceAdjacent(
    const WorldVoxelChunkAddress &left,
    const WorldVoxelChunkAddress &right,
    const double baseEdge)
{
  const WorldVoxelPageBox a = worldVoxelPageBox(left, baseEdge);
  const WorldVoxelPageBox b = worldVoxelPageBox(right, baseEdge);
  const double tolerance = std::max(a.edge, b.edge) * 1.0e-9;
  const bool overlapX = a.minimumX < b.minimumX + b.edge - tolerance &&
      b.minimumX < a.minimumX + a.edge - tolerance;
  const bool overlapZ = a.minimumZ < b.minimumZ + b.edge - tolerance &&
      b.minimumZ < a.minimumZ + a.edge - tolerance;
  const bool touchX =
      std::abs(a.minimumX + a.edge - b.minimumX) <= tolerance ||
      std::abs(b.minimumX + b.edge - a.minimumX) <= tolerance;
  const bool touchZ =
      std::abs(a.minimumZ + a.edge - b.minimumZ) <= tolerance ||
      std::abs(b.minimumZ + b.edge - a.minimumZ) <= tolerance;
  return (touchX && overlapZ) || (touchZ && overlapX);
}

} // namespace

// Diagnostic: how much ground inside `radius` of the camera no page of `cut`
// contains. A spatial leaf cut must be a partition of its roots, so any
// uncovered sample is ground the renderer has no owner for. Enabled by
// SVR_TRACE_TERRAIN_CUT so it costs nothing in a shipping run.
double worldVoxelCutUncoveredFraction(
    const std::vector<WorldVoxelChunkAddress> &cut,
    const double centerXMetres,
    const double centerZMetres,
    const double radiusMetres,
    const double baseEdgeMetres)
{
  if (cut.empty() || !(radiusMetres > 0.0) || !(baseEdgeMetres > 0.0))
    return 0.0;
  constexpr uint32_t kSamplesPerAxis = 96u;
  uint32_t maximumLOD = 0u;
  for (const auto &address : cut)
    maximumLOD = std::max(maximumLOD, address.spatialLOD);
  uint32_t uncovered = 0u;
  uint32_t sampled = 0u;
  for (uint32_t sampleZ = 0u; sampleZ < kSamplesPerAxis; ++sampleZ)
    for (uint32_t sampleX = 0u; sampleX < kSamplesPerAxis; ++sampleX)
    {
      const double unitX =
          (static_cast<double>(sampleX) + 0.5) / kSamplesPerAxis * 2.0 - 1.0;
      const double unitZ =
          (static_cast<double>(sampleZ) + 0.5) / kSamplesPerAxis * 2.0 - 1.0;
      if (std::hypot(unitX, unitZ) > 1.0)
        continue;
      ++sampled;
      const double worldX = centerXMetres + unitX * radiusMetres;
      const double worldZ = centerZMetres + unitZ * radiusMetres;
      bool covered = false;
      for (uint32_t lod = 0u; lod <= maximumLOD && !covered; ++lod)
      {
        const double edge = std::ldexp(baseEdgeMetres, static_cast<int>(lod));
        const WorldVoxelChunkAddress probe{
          static_cast<int32_t>(std::floor(worldX / edge)),
          static_cast<int32_t>(std::floor(worldZ / edge)), lod};
        covered = std::find(cut.begin(), cut.end(), probe) != cut.end();
      }
      if (!covered)
        ++uncovered;
    }
  return sampled != 0u
      ? static_cast<double>(uncovered) / static_cast<double>(sampled) : 0.0;
}

std::vector<WorldVoxelChunkAddress> clampWorldVoxelCutToRefinementLevels(
    const std::vector<WorldVoxelChunkAddress> &published,
    const std::vector<WorldVoxelChunkAddress> &requested,
    const uint32_t maximumRefinementLevels)
{
  if (published.empty() || requested.empty() || maximumRefinementLevels == 0u)
    return requested;
  const auto buildClamped = [&](const uint32_t refinementLevels)
  {
    std::vector<WorldVoxelChunkAddress> clamped;
    clamped.reserve(requested.size());
    for (const auto &page : requested)
    {
      WorldVoxelChunkAddress emitted = page;
      for (const auto &owner : published)
      {
        if (!worldVoxelPageAncestorOf(owner, page))
          continue;
        // Only a refinement is clamped. A coarsening replaces several
        // published pages with one already-cheap page.
        const uint32_t depth = owner.spatialLOD - page.spatialLOD;
        if (depth > refinementLevels)
        {
          const uint32_t levels = depth - refinementLevels;
          for (uint32_t level = 0u; level < levels; ++level)
            emitted = worldVoxelPageParent(emitted);
        }
        break;
      }
      if (std::find(clamped.begin(), clamped.end(), emitted) == clamped.end())
        clamped.push_back(emitted);
    }
    return clamped;
  };
  auto clamped = buildClamped(maximumRefinementLevels);
  // Raising a page to an ancestor grows its footprint, and the requested cut it
  // came from is only a partition at its own depths. Where the raised page
  // swallows a sibling that was not raised - a request spanning owners at
  // different levels - the clamped cut is no longer a partition. A multi-level
  // advance can also expose a face transition steeper than the requested cut's
  // 2:1 balance. Fall back to the established one-level advance in either case.
  const double baseEdge = 1.0;
  bool invalid = false;
  for (size_t left = 0u; left < clamped.size(); ++left)
    for (size_t right = left + 1u; right < clamped.size(); ++right)
    {
      const uint32_t delta = clamped[left].spatialLOD >
              clamped[right].spatialLOD
          ? clamped[left].spatialLOD - clamped[right].spatialLOD
          : clamped[right].spatialLOD - clamped[left].spatialLOD;
      invalid |= worldVoxelPagesOverlap(
          clamped[left], clamped[right], baseEdge);
      invalid |= delta > 1u && worldVoxelPagesFaceAdjacent(
          clamped[left], clamped[right], baseEdge);
    }
  if (invalid && maximumRefinementLevels > 1u)
    clamped = buildClamped(1u);
  for (size_t left = 0u; left < clamped.size(); ++left)
    for (size_t right = left + 1u; right < clamped.size(); ++right)
      if (worldVoxelPagesOverlap(clamped[left], clamped[right], baseEdge))
        return requested;
  return clamped;
}

std::vector<WorldVoxelChunkAddress> clampWorldVoxelCutToOneLevel(
    const std::vector<WorldVoxelChunkAddress> &published,
    const std::vector<WorldVoxelChunkAddress> &requested)
{
  return clampWorldVoxelCutToRefinementLevels(published, requested, 1u);
}

std::vector<WorldVoxelChunkAddress> limitWorldVoxelCutToCapacity(
    const std::vector<WorldVoxelChunkAddress> &published,
    const std::vector<WorldVoxelChunkAddress> &requested,
    const std::vector<WorldVoxelChunkAddress> &staged,
    const double baseEdgeMetres,
    const uint32_t slotCapacity,
    uint32_t *admittedGroupCount,
    uint32_t *deferredGroupCount)
{
  if (admittedGroupCount != nullptr)
    *admittedGroupCount = 0u;
  if (deferredGroupCount != nullptr)
    *deferredGroupCount = 0u;
  const auto contains = [](const auto &pages, const auto &page)
  { return std::find(pages.begin(), pages.end(), page) != pages.end(); };
  // What holds a slot while this transaction runs. The allocator marks every
  // published and staged owner's slot occupied - a held-back group keeps
  // drawing from its old owner, and a staged one is already built - so a page
  // this cut retires does not give its slot back until publication. Peak
  // coexistence is therefore `held` plus every page the transaction adds, not
  // the size of the cut it produces.
  std::vector<WorldVoxelChunkAddress> held = published;
  for (const auto &page : staged)
    if (!contains(held, page))
      held.push_back(page);

  // One group per ownership boundary, in the order `requested` lists them,
  // which is the demand order the caller ranked. A group is whole: the pages
  // that replace one owner are admitted together or not at all, because half
  // a replacement is either a hole or a double-draw.
  std::vector<WorldVoxelChunkAddress> owners;
  for (const auto &page : requested)
  {
    if (contains(published, page))
      continue;
    // Refinement: the published page this one is splitting out of. Coarsening
    // and fresh area have no published ancestor, and own themselves.
    WorldVoxelChunkAddress owner = page;
    for (const auto &parent : published)
      if (worldVoxelPageAncestorOf(parent, page))
      {
        owner = parent;
        break;
      }
    if (!contains(owners, owner))
      owners.push_back(owner);
  }

  std::vector<WorldVoxelChunkAddress> admittedAdds;
  std::vector<WorldVoxelChunkAddress> retired;
  bool capacityDeferred = false;
  size_t slotsTaken = held.size();
  for (const auto &owner : owners)
  {
    // Everything this group puts into the cut, and the subset of it that needs
    // a slot it does not already hold: a staged child is built and owns one.
    std::vector<WorldVoxelChunkAddress> adds;
    size_t slotCost = 0u;
    for (const auto &candidate : requested)
    {
      if (!(candidate == owner) && !worldVoxelPageAncestorOf(owner, candidate))
        continue;
      if (contains(published, candidate) || contains(adds, candidate))
        continue;
      adds.push_back(candidate);
      if (!contains(held, candidate))
        ++slotCost;
    }
    if (adds.empty())
      continue;
    if (slotsTaken + slotCost > slotCapacity)
    {
      capacityDeferred = true;
      // Deferred, not rejected. Its owner keeps drawing, the demand that asked
      // for it is unchanged, and a later transaction - after a merge or an
      // eviction frees slots - admits it. Rejecting the whole batch here is
      // what froze the cut: the same eight splits were rebuilt and refused
      // every frame with nothing in the world changing in between.
      if (deferredGroupCount != nullptr)
        ++*deferredGroupCount;
      continue;
    }
    slotsTaken += slotCost;
    if (admittedGroupCount != nullptr)
      ++*admittedGroupCount;
    admittedAdds.insert(admittedAdds.end(), adds.begin(), adds.end());
    // Refinement retires the owner; coarsening retires the published pages
    // underneath the new coarse page. `published` is a partition, so exactly
    // one of the two applies to any given owner.
    for (const auto &page : published)
      if ((page == owner || worldVoxelPageAncestorOf(owner, page)) &&
          !contains(retired, page))
        retired.push_back(page);
  }

  std::vector<WorldVoxelChunkAddress> result;
  std::vector<WorldVoxelChunkAddress> retainedPublished;
  result.reserve(published.size() + admittedAdds.size());
  for (const auto &page : published)
  {
    // A published page the requested cut no longer covers at all - it left the
    // render distance - follows the request out. Its slot is not reusable
    // until this transaction publishes, so dropping it does not fund an
    // admission here; it funds the next one.
    bool covered = false;
    for (const auto &candidate : requested)
      if (candidate == page || worldVoxelPageAncestorOf(candidate, page) ||
          worldVoxelPageAncestorOf(page, candidate))
      {
        covered = true;
        break;
      }
    if (covered)
    {
      retainedPublished.push_back(page);
      if (!contains(retired, page))
        result.push_back(page);
    }
  }
  result.insert(result.end(), admittedAdds.begin(), admittedAdds.end());
  if (!capacityDeferred)
    return result;
  // Feedback can request an intermediate cut that has not yet converged its
  // neighbours. Admission must not turn that existing imbalance into a new
  // reason to reject otherwise affordable measured-error refinement.
  const auto isBalanced = [&](const auto &cut)
  {
    for (size_t left = 0u; left < cut.size(); ++left)
      for (size_t right = left + 1u; right < cut.size(); ++right)
      {
        const uint32_t delta = cut[left].spatialLOD > cut[right].spatialLOD
            ? cut[left].spatialLOD - cut[right].spatialLOD
            : cut[right].spatialLOD - cut[left].spatialLOD;
        if (delta > 1u && worldVoxelPagesFaceAdjacent(
                cut[left], cut[right], baseEdgeMetres))
          return false;
      }
    return true;
  };
  if (!isBalanced(published) || !isBalanced(requested))
    return result;
  // Capacity can defer a neighbour needed to balance an admitted split. Apply
  // the same ownership closure as publication before spending generation work.
  // This only retracts additions, so peak slot usage cannot increase.
  if (retainedPublished.empty())
    return result;
  uint32_t advanced = 0u, heldBack = 0u;
  auto balanced = selectPublishableWorldVoxelCut(
      retainedPublished, result, result, baseEdgeMetres, &advanced, &heldBack);
  if (admittedGroupCount != nullptr)
    *admittedGroupCount = advanced;
  if (deferredGroupCount != nullptr)
    *deferredGroupCount += heldBack;
  // Ownership closure walks the old cut. Restore demand order so the next
  // generation budget still advances the highest-priority admitted group.
  std::stable_sort(balanced.begin(), balanced.end(),
      [&](const WorldVoxelChunkAddress &left, const WorldVoxelChunkAddress &right)
      {
        return std::find(requested.begin(), requested.end(), left) <
            std::find(requested.begin(), requested.end(), right);
      });
  return balanced;
}

std::vector<WorldVoxelChunkAddress> selectPublishableWorldVoxelCut(
    const std::vector<WorldVoxelChunkAddress> &published,
    const std::vector<WorldVoxelChunkAddress> &requested,
    const std::vector<WorldVoxelChunkAddress> &staged,
    const double baseEdgeMetres,
    uint32_t *advancedGroupCount,
    uint32_t *heldGroupCount)
{
  if (advancedGroupCount != nullptr)
    *advancedGroupCount = 0u;
  if (heldGroupCount != nullptr)
    *heldGroupCount = 0u;
  if (published.empty() || !(baseEdgeMetres > 0.0) ||
      !std::isfinite(baseEdgeMetres))
    return published;

  const auto contains = [](const std::vector<WorldVoxelChunkAddress> &set,
                           const WorldVoxelChunkAddress &address)
  {
    return std::find(set.begin(), set.end(), address) != set.end();
  };

  // Group key: the coarsest page involved in this ownership change. For a
  // refinement that is the published page; for a coarsening it is the requested
  // page absorbing several published ones.
  struct OwnershipGroup
  {
    WorldVoxelChunkAddress key{};
    std::vector<WorldVoxelChunkAddress> publishedMembers;
    std::vector<WorldVoxelChunkAddress> requestedMembers;
  };
  std::vector<OwnershipGroup> groups;
  const auto groupFor = [&](const WorldVoxelChunkAddress &key)
      -> OwnershipGroup &
  {
    for (auto &group : groups)
      if (group.key == key)
        return group;
    groups.push_back({key, {}, {}});
    return groups.back();
  };

  for (const auto &page : published)
  {
    WorldVoxelChunkAddress key = page;
    if (!contains(requested, page))
      for (const auto &candidate : requested)
        if (worldVoxelPageAncestorOf(candidate, page))
        {
          key = candidate;
          break;
        }
    groupFor(key).publishedMembers.push_back(page);
  }
  // Every requested page joins exactly one group. Assigning by scanning groups
  // per page instead would put a page into both a group keyed on one of its
  // ancestors and a group keyed on itself, and the resolved cut would then
  // contain that page twice - which reads downstream as a second lookup of an
  // entry the first one already moved out, reported as a chunk with no record.
  const size_t ownershipGroupCount = groups.size();
  for (const auto &page : requested)
  {
    size_t owningGroup = ownershipGroupCount;
    for (size_t index = 0u; index < ownershipGroupCount; ++index)
      if (page == groups[index].key ||
          worldVoxelPageAncestorOf(groups[index].key, page))
      {
        owningGroup = index;
        break;
      }
    if (owningGroup != ownershipGroupCount)
    {
      groups[owningGroup].requestedMembers.push_back(page);
      continue;
    }
    // Disjoint from everything published: new coverage rather than an
    // ownership change, so it gets a group of its own instead of being dropped
    // for having no published counterpart to replace.
    groupFor(page).requestedMembers.push_back(page);
  }

  std::vector<uint8_t> committable(groups.size(), 0u);
  for (size_t index = 0u; index < groups.size(); ++index)
  {
    const auto &group = groups[index];
    if (group.requestedMembers.empty())
      continue;
    // An unchanged group is trivially committable and costs nothing to take.
    const bool unchanged =
        group.publishedMembers.size() == group.requestedMembers.size() &&
        std::all_of(
            group.publishedMembers.begin(), group.publishedMembers.end(),
            [&](const WorldVoxelChunkAddress &page)
            { return contains(group.requestedMembers, page); });
    committable[index] = unchanged ||
            std::all_of(
                group.requestedMembers.begin(), group.requestedMembers.end(),
                [&](const WorldVoxelChunkAddress &page)
                { return contains(staged, page); })
        ? 1u
        : 0u;
  }

  const auto resolve = [&]
  {
    std::vector<WorldVoxelChunkAddress> cut;
    for (size_t index = 0u; index < groups.size(); ++index)
    {
      const auto &members = committable[index] != 0u
          ? groups[index].requestedMembers
          : groups[index].publishedMembers;
      // A duplicate here is not cosmetic: the caller looks each address up once
      // and moves the entry out, so a second occurrence resolves to a moved-from
      // entry with no chunk records at all.
      for (const auto &member : members)
        if (!contains(cut, member))
          cut.push_back(member);
    }
    return cut;
  };

  // Bounding how many groups may advance here was tried, to shorten the
  // invisible window of one ownership transaction, and it is a bad trade as
  // written: with a 512-page generation budget it cut first paint from 166
  // frames to 28 but collapsed converged detail from 1429 to 136 frames at
  // spatial LOD <= 2. `groups` is in published-cut order, not screen-error
  // order, so a blind cap keeps advancing whatever it happens to reach first
  // and starves the pages actually in front of the camera. A cap here needs the
  // camera to sort groups by projected error before it is worth having.
  //
  // Hold back any commit that would leave a face neighbour more than one level
  // away. Reverting only ever moves a group back toward the published cut,
  // which is already balanced, so the loop terminates.
  std::vector<uint32_t> owner;
  for (;;)
  {
    std::vector<WorldVoxelChunkAddress> cut;
    owner.clear();
    for (size_t index = 0u; index < groups.size(); ++index)
    {
      const auto &members = committable[index] != 0u
          ? groups[index].requestedMembers
          : groups[index].publishedMembers;
      for (const auto &member : members)
      {
        cut.push_back(member);
        owner.push_back(static_cast<uint32_t>(index));
      }
    }
    // Collect every offender in one sweep rather than reverting one group and
    // rescanning. Reverting more than strictly necessary is always safe - the
    // published cut it falls back to is balanced - and it keeps this closure
    // near O(n^2) instead of O(n^3) on a cut of a thousand pages.
    bool reverted = false;
    for (size_t left = 0u; left < cut.size(); ++left)
      for (size_t right = left + 1u; right < cut.size(); ++right)
      {
        // Disjointness first, and enforced rather than argued. A requested cut
        // spanning several published owners can resolve to pages that contain
        // one another once some groups advance and others do not, and a page
        // published together with its own descendants is drawn twice. Reverting
        // a group always moves it back to a published owner that was already
        // disjoint, so this terminates.
        const bool overlapping =
            worldVoxelPagesOverlap(cut[left], cut[right], baseEdgeMetres);
        const uint32_t delta = cut[left].spatialLOD > cut[right].spatialLOD
            ? cut[left].spatialLOD - cut[right].spatialLOD
            : cut[right].spatialLOD - cut[left].spatialLOD;
        const bool unbalanced = delta > 1u &&
            worldVoxelPagesFaceAdjacent(cut[left], cut[right], baseEdgeMetres);
        if (!overlapping && !unbalanced)
          continue;
        // Revert whichever side moved. If both moved, revert the finer one.
        const size_t leftGroup = owner[left];
        const size_t rightGroup = owner[right];
        if (committable[leftGroup] != 0u &&
            (committable[rightGroup] == 0u ||
             cut[left].spatialLOD < cut[right].spatialLOD))
        {
          committable[leftGroup] = 0u;
          reverted = true;
        }
        else if (committable[rightGroup] != 0u)
        {
          committable[rightGroup] = 0u;
          reverted = true;
        }
      }
    if (!reverted)
      break;
  }

  uint32_t advanced = 0u;
  uint32_t held = 0u;
  for (size_t index = 0u; index < groups.size(); ++index)
  {
    const bool unchanged =
        groups[index].publishedMembers.size() ==
            groups[index].requestedMembers.size() &&
        std::all_of(
            groups[index].publishedMembers.begin(),
            groups[index].publishedMembers.end(),
            [&](const WorldVoxelChunkAddress &page)
            { return contains(groups[index].requestedMembers, page); });
    if (unchanged)
      continue;
    if (committable[index] != 0u)
      ++advanced;
    else
      ++held;
  }
  // Last gate. Everything above reasons about ownership groups being disjoint,
  // and the published cut it falls back to is a partition by induction - but a
  // cut that is wrong here is a page drawn together with its own descendants,
  // which is a visible defect rather than a slow path. Verify the result and
  // keep the previous cut whole if it does not hold, so incremental publication
  // can only ever be an improvement on the barrier it replaces.
  std::vector<WorldVoxelChunkAddress> selected = resolve();
  for (size_t left = 0u; left < selected.size(); ++left)
    for (size_t right = left + 1u; right < selected.size(); ++right)
      if (worldVoxelPagesOverlap(
              selected[left], selected[right], baseEdgeMetres))
      {
        if (advancedGroupCount != nullptr)
          *advancedGroupCount = 0u;
        if (heldGroupCount != nullptr)
          *heldGroupCount = static_cast<uint32_t>(groups.size());
        return published;
      }
  if (!worldVoxelCutCoversPublishedOwners(selected, published))
  {
    if (advancedGroupCount != nullptr)
      *advancedGroupCount = 0u;
    if (heldGroupCount != nullptr)
      *heldGroupCount = static_cast<uint32_t>(groups.size());
    return published;
  }
  if (advancedGroupCount != nullptr)
    *advancedGroupCount = advanced;
  if (heldGroupCount != nullptr)
    *heldGroupCount = held;
  return selected;
}

float worldVoxelMeasuredPageError(
    const WorldVoxelMeasuredPageErrors &measured,
    const WorldVoxelChunkAddress &address)
{
  for (const auto &entry : measured)
    if (entry.first == address)
      return entry.second;
  return 0.0f;
}

std::vector<WorldVoxelChunkAddress> selectWorldVoxelCutForHeadroom(
    const std::vector<WorldVoxelChunkAddress> &published,
    const std::vector<WorldVoxelChunkAddress> &residentParents,
    const double baseEdge, const double cameraX, const double cameraZ)
{
  if (!(baseEdge > 0.0) || !std::isfinite(cameraX) || !std::isfinite(cameraZ))
    return published;
  struct Candidate { WorldVoxelChunkAddress parent; double error; };
  std::vector<Candidate> candidates;
  for (const auto &parent : residentParents)
  {
    if (parent.spatialLOD == 0u || parent.spatialLOD > 31u)
      continue;
    const double edge = std::ldexp(baseEdge, int(parent.spatialLOD));
    const double minX = double(parent.x) * edge, minZ = double(parent.z) * edge;
    if (cameraX >= minX && cameraX < minX + edge &&
        cameraZ >= minZ && cameraZ < minZ + edge)
      continue;
    const auto children = std::count_if(published.begin(), published.end(),
        [&](const auto &leaf) { return worldVoxelPageParent(leaf) == parent; });
    if (children != 4)
      continue;
    const double dx = std::max({minX - cameraX, cameraX - minX - edge, 0.0});
    const double dz = std::max({minZ - cameraZ, cameraZ - minZ - edge, 0.0});
    candidates.push_back({parent, edge / std::max(std::hypot(dx, dz), baseEdge)});
  }
  std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
    if (a.error != b.error) return a.error < b.error;
    return std::tie(a.parent.spatialLOD, a.parent.x, a.parent.z) <
        std::tie(b.parent.spatialLOD, b.parent.x, b.parent.z);
  });
  for (const auto &candidate : candidates)
  {
    std::vector<WorldVoxelChunkAddress> cut;
    for (const auto &leaf : published)
      if (!(worldVoxelPageParent(leaf) == candidate.parent)) cut.push_back(leaf);
    cut.push_back(candidate.parent);
    auto balanced = balanceWorldVoxelCut(cut, baseEdge);
    // The balancing operation only refines. Reject a merge that would need
    // replacement geometry: recovery must run with no free page allocation.
    if (balanced.size() == cut.size()) return cut;
  }
  return published;
}

std::vector<WorldVoxelChunkAddress> balanceWorldVoxelCut(
    std::vector<WorldVoxelChunkAddress> leaves, const double baseEdge)
{
  for (;;)
  {
    std::vector<uint8_t> refine(leaves.size(), 0u);
    bool anyRefined = false;
    for (size_t leftIndex = 0u; leftIndex < leaves.size(); ++leftIndex)
      for (size_t rightIndex = leftIndex + 1u; rightIndex < leaves.size();
           ++rightIndex)
      {
        const auto &left = leaves[leftIndex];
        const auto &right = leaves[rightIndex];
        const uint32_t lodDelta = left.spatialLOD > right.spatialLOD
            ? left.spatialLOD - right.spatialLOD
            : right.spatialLOD - left.spatialLOD;
        if (lodDelta <= 1u)
          continue;
        const size_t coarseIndex =
            left.spatialLOD > right.spatialLOD ? leftIndex : rightIndex;
        if (refine[coarseIndex] != 0u)
          continue;
        const double leftEdge = std::ldexp(baseEdge, left.spatialLOD);
        const double rightEdge = std::ldexp(baseEdge, right.spatialLOD);
        const double leftMinX = static_cast<double>(left.x) * leftEdge;
        const double leftMinZ = static_cast<double>(left.z) * leftEdge;
        const double rightMinX = static_cast<double>(right.x) * rightEdge;
        const double rightMinZ = static_cast<double>(right.z) * rightEdge;
        const double tolerance = std::max(leftEdge, rightEdge) * 1.0e-10;
        const bool overlapX =
            std::min(leftMinX + leftEdge, rightMinX + rightEdge) -
                std::max(leftMinX, rightMinX) > tolerance;
        const bool overlapZ =
            std::min(leftMinZ + leftEdge, rightMinZ + rightEdge) -
                std::max(leftMinZ, rightMinZ) > tolerance;
        const bool touchesX =
            std::abs(leftMinX + leftEdge - rightMinX) <= tolerance ||
            std::abs(rightMinX + rightEdge - leftMinX) <= tolerance;
        const bool touchesZ =
            std::abs(leftMinZ + leftEdge - rightMinZ) <= tolerance ||
            std::abs(rightMinZ + rightEdge - leftMinZ) <= tolerance;
        if ((touchesX && overlapZ) || (touchesZ && overlapX))
        {
          refine[coarseIndex] = 1u;
          anyRefined = true;
        }
      }
    if (!anyRefined)
      break;
    std::vector<WorldVoxelChunkAddress> balanced;
    balanced.reserve(leaves.size() + 4u);
    for (size_t index = 0u; index < leaves.size(); ++index)
    {
      if (refine[index] == 0u)
      {
        balanced.push_back(leaves[index]);
        continue;
      }
      const WorldVoxelChunkAddress coarse = leaves[index];
      const uint32_t childLOD = coarse.spatialLOD - 1u;
      for (int32_t childZ = 0; childZ != 2; ++childZ)
        for (int32_t childX = 0; childX != 2; ++childX)
          balanced.push_back({
            coarse.x * 2 + childX, coarse.z * 2 + childZ, childLOD});
    }
    leaves = std::move(balanced);
  }
  return leaves;
}

std::vector<WorldVoxelChunkAddress> buildWorldVoxelChunkLeafCut(
    const double worldExtentMetres,
    const double cameraWorldXMetres,
    const double cameraWorldZMetres,
    const WorldVoxelChunkStreamingSettings &settings,
    const WorldVoxelMeasuredPageErrors *measuredErrors,
    const std::vector<WorldVoxelChunkAddress> *stableCut)
{
  std::vector<WorldVoxelChunkAddress> leaves;
  const double baseEdge = settings.chunkEdgeMetres > 0.0
      ? settings.chunkEdgeMetres
      : settings.finestVoxelSizeMetres *
            static_cast<double>(settings.finestHorizontalVoxelCount);
  if (!(worldExtentMetres > 0.0) || !std::isfinite(worldExtentMetres) ||
      !(baseEdge > 0.0) || !std::isfinite(baseEdge) ||
      !(settings.finestVoxelSizeMetres > 0.0) ||
      !std::isfinite(settings.finestVoxelSizeMetres) ||
      !(settings.renderDistanceMetres > 0.0) ||
      !std::isfinite(settings.renderDistanceMetres) ||
      !(settings.projectionScalePixels > 0.0) ||
      !std::isfinite(settings.projectionScalePixels) ||
      !(settings.splitErrorPixels > 0.0f) ||
      !std::isfinite(settings.splitErrorPixels))
    return leaves;

  const double mapX = std::clamp(
      cameraWorldXMetres + worldExtentMetres * 0.5,
      0.0, std::nextafter(worldExtentMetres, 0.0));
  const double mapZ = std::clamp(
      cameraWorldZMetres + worldExtentMetres * 0.5,
      0.0, std::nextafter(worldExtentMetres, 0.0));
  uint32_t rootLOD = 0u;
  const uint32_t maximumLOD = std::min(
      settings.maximumSpatialLOD,
      kPlanetVoxelMaximumClipmapLevels - 1u);
  while (rootLOD < maximumLOD &&
         std::ldexp(baseEdge, static_cast<int>(rootLOD)) <
             settings.renderDistanceMetres)
    ++rootLOD;

  const auto distanceSquaredToPage = [&](const WorldVoxelChunkAddress &page)
  {
    const double edge = std::ldexp(baseEdge, page.spatialLOD);
    const double minimumX = static_cast<double>(page.x) * edge;
    const double minimumZ = static_cast<double>(page.z) * edge;
    const double maximumX = minimumX + edge;
    const double maximumZ = minimumZ + edge;
    const double deltaX = mapX < minimumX
        ? minimumX - mapX
        : mapX > maximumX ? mapX - maximumX : 0.0;
    const double deltaZ = mapZ < minimumZ
        ? minimumZ - mapZ
        : mapZ > maximumZ ? mapZ - maximumZ : 0.0;
    return deltaX * deltaX + deltaZ * deltaZ;
  };
  const double renderDistanceSquared =
      settings.renderDistanceMetres * settings.renderDistanceMetres;
  const auto visit = [&](const auto &self,
                         const WorldVoxelChunkAddress &page) -> void
  {
    const double edge = std::ldexp(baseEdge, page.spatialLOD);
    const double minimumX = static_cast<double>(page.x) * edge;
    const double minimumZ = static_cast<double>(page.z) * edge;
    if (minimumX + edge <= 0.0 || minimumZ + edge <= 0.0 ||
        minimumX >= worldExtentMetres || minimumZ >= worldExtentMetres ||
        distanceSquaredToPage(page) > renderDistanceSquared)
      return;

    const bool completelyInsideWorld = minimumX >= 0.0 &&
        minimumZ >= 0.0 &&
        minimumX + edge <= worldExtentMetres + 1.0e-6 &&
        minimumZ + edge <= worldExtentMetres + 1.0e-6;
    if (!completelyInsideWorld)
    {
      if (page.spatialLOD == 0u)
        return;
      const uint32_t childLOD = page.spatialLOD - 1u;
      for (int32_t childZ = 0; childZ != 2; ++childZ)
        for (int32_t childX = 0; childX != 2; ++childX)
          self(self, {
            page.x * 2 + childX,
            page.z * 2 + childZ,
            childLOD});
      return;
    }

    const double nearestDistance = std::sqrt(distanceSquaredToPage(page));
    // The page voxel is only an estimate of what this page would lose by not
    // being subdivided. Where the streamer has already built the page it knows
    // the answer, and a flat plain measures far below its voxel while a cliff
    // measures near it.
    const double geometricError = std::ldexp(
        settings.finestVoxelSizeMetres,
        static_cast<int>(page.spatialLOD));
    const double measured = measuredErrors != nullptr
        ? static_cast<double>(
              worldVoxelMeasuredPageError(*measuredErrors, page))
        : 0.0;
    const double voxelSize = measured > 0.0 ? measured : geometricError;
    // A page containing the camera has zero horizontal distance.  Clamping
    // that to half the *page* extent made the projected-error test depend on
    // page resolution instead of proximity: with the shipping 128-cell page
    // and the 0.25-voxel measured-error floor, every spatial LOD evaluated to
    // roughly focalPixels / 256 (1.95 px at a 500 px focal length).  That is
    // just below the 2 px split threshold, so a many-metre voxel could remain
    // directly under a ground-level camera and expose its page wall beside an
    // already-refined neighbour.
    //
    // One represented voxel is the finite near-field distance for this
    // screen-error estimate.  It keeps the division bounded while ensuring a
    // page intersecting the eye continues to refine down to spatial LOD 0.
    const double metricDistance = std::max(nearestDistance, geometricError);
    const double distanceKilometres = metricDistance * 0.001;
    const double distanceMultiplier = 1.0 +
        std::max(0.0f, settings.distanceErrorGrowthPerKilometre) *
            std::pow(
                distanceKilometres,
                std::clamp(
                    static_cast<double>(settings.distanceErrorExponent),
                    0.125, 8.0));
    const double projectedError =
        settings.projectionScalePixels * voxelSize /
        metricDistance / distanceMultiplier;
    // Preserve the previous spatial topology inside the split/merge band.
    // Without this, crossing a finest-page camera cell rebuilt the target cut
    // at one threshold and pages near it alternated between parent and children.
    const bool wasRefined = stableCut != nullptr && std::any_of(
        stableCut->begin(), stableCut->end(),
        [&](const WorldVoxelChunkAddress &leaf)
        {
          return worldVoxelPageAncestorOf(page, leaf);
        });
    const double splitThreshold = wasRefined &&
            settings.mergeErrorPixels > 0.0f &&
            std::isfinite(settings.mergeErrorPixels)
        ? settings.mergeErrorPixels
        : settings.splitErrorPixels;
    if (page.spatialLOD != 0u &&
        projectedError > splitThreshold)
    {
      const uint32_t childLOD = page.spatialLOD - 1u;
      for (int32_t childZ = 0; childZ != 2; ++childZ)
        for (int32_t childX = 0; childX != 2; ++childX)
          self(self, {
            page.x * 2 + childX,
            page.z * 2 + childZ,
            childLOD});
      return;
    }
    leaves.push_back(page);
  };

  const double rootEdge = std::ldexp(baseEdge, rootLOD);
  const int32_t minimumRootX = static_cast<int32_t>(std::floor(
      (mapX - settings.renderDistanceMetres) / rootEdge));
  const int32_t maximumRootX = static_cast<int32_t>(std::floor(
      (mapX + settings.renderDistanceMetres) / rootEdge));
  const int32_t minimumRootZ = static_cast<int32_t>(std::floor(
      (mapZ - settings.renderDistanceMetres) / rootEdge));
  const int32_t maximumRootZ = static_cast<int32_t>(std::floor(
      (mapZ + settings.renderDistanceMetres) / rootEdge));
  for (int32_t z = minimumRootZ; z <= maximumRootZ; ++z)
    for (int32_t x = minimumRootX; x <= maximumRootX; ++x)
      visit(visit, {x, z, rootLOD});

  return balanceWorldVoxelCut(std::move(leaves), baseEdge);
}

uint32_t worldVoxelChunkRadiusForProjection(
    const uint32_t viewportHeight,
    const float verticalFovRadians,
    const uint32_t horizontalVoxelCount,
    const float transitionPixelThreshold,
    const uint32_t maximumRadius)
{
  if (viewportHeight == 0u || horizontalVoxelCount == 0u ||
      !(verticalFovRadians > 0.0f) ||
      !(verticalFovRadians < 3.14159265358979323846f) ||
      !std::isfinite(verticalFovRadians) ||
      !(transitionPixelThreshold > 0.0f) ||
      !std::isfinite(transitionPixelThreshold) || maximumRadius == 0u)
    return 1u;
  const double focalPixels =
      0.5 * static_cast<double>(viewportHeight) /
      std::tan(static_cast<double>(verticalFovRadians) * 0.5);
  const double required = std::ceil(
      2.0 * focalPixels /
              (static_cast<double>(transitionPixelThreshold) *
               horizontalVoxelCount) -
          0.5);
  return static_cast<uint32_t>(
      std::clamp(required, 1.0, static_cast<double>(maximumRadius)));
}

std::vector<WorldVoxelChunkRequest> coalesceWorldVoxelChunkRequests(
    const WorldVoxelChunkCache &cache,
    const std::vector<WorldVoxelChunkRequest> &requests)
{
  std::vector<WorldVoxelChunkRequest> result;
  struct TraceCounts
  {
    uint32_t staleSlot = 0u;
    uint32_t sameLOD = 0u;
    uint32_t finest = 0u;
    uint32_t belowStableLOD = 0u;
    uint32_t spatialMerge = 0u;
    uint32_t partialSpatialMerge = 0u;
    uint32_t beyondMaximumLOD = 0u;
    uint32_t duplicate = 0u;
    uint32_t requestedRefine = 0u;
    uint32_t requestedCoarsen = 0u;
    uint32_t acceptedRefine = 0u;
    uint32_t acceptedCoarsen = 0u;
  } traceCounts;
  const bool spatialFeedback = cache.settings.renderDistanceMetres > 0.0;
  const double baseEdge = cache.settings.chunkEdgeMetres > 0.0
      ? cache.settings.chunkEdgeMetres
      : cache.settings.finestVoxelSizeMetres *
            static_cast<double>(cache.settings.finestHorizontalVoxelCount);
  const uint32_t spatialRootLOD = spatialFeedback
      ? worldVoxelPageRootLOD(baseEdge, cache.settings)
      : 0u;
  const bool traceTerrain = std::getenv("SVR_TRACE_TERRAIN") != nullptr;
  std::vector<WorldVoxelChunkRequest> traceStaleSlot;
  std::vector<WorldVoxelChunkRequest> traceSameLOD;
  std::vector<WorldVoxelChunkRequest> traceFinest;
  std::vector<WorldVoxelChunkRequest> traceSpatialMerge;
  std::vector<WorldVoxelChunkRequest> tracePartialSpatialMerge;
  std::vector<WorldVoxelChunkRequest> traceBeyondMaximumLOD;
  const auto rememberTrace = [&](std::vector<WorldVoxelChunkRequest> &bucket,
                                 const WorldVoxelChunkRequest &request)
  {
    if (traceTerrain && bucket.size() < 8u)
      bucket.push_back(request);
  };
  for (const auto &request : requests)
  {
    const auto resident = std::find_if(
        cache.entries.begin(), cache.entries.end(),
        [&](const WorldVoxelChunkEntry &entry)
        {
          return entry.slot == request.slot &&
              entry.generation == request.expectedGeneration;
        });
    if (resident == cache.entries.end())
    {
      ++traceCounts.staleSlot;
      rememberTrace(traceStaleSlot, request);
      continue;
    }
    const uint32_t currentLOD = spatialFeedback
        ? std::min(resident->address.spatialLOD + resident->residentLOD, 31u)
        : resident->residentLOD;
    if (request.desiredLOD == currentLOD)
    {
      ++traceCounts.sameLOD;
      rememberTrace(traceSameLOD, request);
      continue;
    }
    const bool refinement = request.desiredLOD < currentLOD;
    if (refinement)
      ++traceCounts.requestedRefine;
    else
      ++traceCounts.requestedCoarsen;
    if (refinement && currentLOD == 0u)
    {
      ++traceCounts.finest;
      rememberTrace(traceFinest, request);
      continue;
    }
    if (!refinement)
    {
      if (spatialFeedback)
      {
        if (resident->address.spatialLOD >= spatialRootLOD)
        {
          ++traceCounts.beyondMaximumLOD;
          rememberTrace(traceBeyondMaximumLOD, request);
          continue;
        }
        const WorldVoxelChunkAddress parent =
            worldVoxelPageParent(resident->address);
        bool completeSiblingMerge = true;
        uint32_t siblingCount = 0u;
        for (const auto &sibling : cache.entries)
        {
          if (sibling.address.spatialLOD >= parent.spatialLOD ||
              !worldVoxelPagesOverlap(sibling.address, parent, baseEdge))
            continue;
          ++siblingCount;
          const uint32_t siblingLOD =
              std::min(sibling.address.spatialLOD + sibling.residentLOD, 31u);
          const bool siblingRequested = std::any_of(
              requests.begin(), requests.end(),
              [&](const WorldVoxelChunkRequest &candidate)
              {
                return candidate.slot == sibling.slot &&
                    candidate.expectedGeneration == sibling.generation &&
                    candidate.desiredLOD > siblingLOD;
              });
          if (!siblingRequested)
          {
            completeSiblingMerge = false;
            break;
          }
        }
        if (!completeSiblingMerge || siblingCount == 0u)
        {
          ++traceCounts.partialSpatialMerge;
          rememberTrace(tracePartialSpatialMerge, request);
          continue;
        }
        ++traceCounts.spatialMerge;
        rememberTrace(traceSpatialMerge, request);
      }
      else if (resident->residentLOD + 1u >= cache.settings.lodCount)
      {
        ++traceCounts.beyondMaximumLOD;
        rememberTrace(traceBeyondMaximumLOD, request);
        continue;
      }
    }
    auto existing = std::find_if(
        result.begin(), result.end(),
        [&](const WorldVoxelChunkRequest &entry)
        {
          return entry.slot == request.slot &&
              entry.expectedGeneration == request.expectedGeneration;
        });
    if (existing == result.end())
    {
      auto accepted = request;
      accepted.currentLOD = currentLOD;
      accepted.desiredLOD =
          refinement ? currentLOD - 1u : currentLOD + 1u;
      result.push_back(accepted);
      if (refinement)
        ++traceCounts.acceptedRefine;
      else
        ++traceCounts.acceptedCoarsen;
    }
    else
    {
      ++traceCounts.duplicate;
      // A simultaneous split always wins over a merge for the same owner.
      // Otherwise retain the oldest/highest-priority observation.
      existing->currentLOD = currentLOD;
      if (refinement)
        existing->desiredLOD = currentLOD - 1u;
      else if (existing->desiredLOD >= existing->currentLOD)
        existing->desiredLOD = std::max(existing->desiredLOD, currentLOD + 1u);
      existing->priority = std::max(existing->priority, request.priority);
    }
  }
  std::sort(
      result.begin(), result.end(),
      [](const WorldVoxelChunkRequest &left,
         const WorldVoxelChunkRequest &right)
      {
        if (left.priority != right.priority)
          return left.priority > right.priority;
        const bool leftRefines = left.desiredLOD < left.currentLOD;
        const bool rightRefines = right.desiredLOD < right.currentLOD;
        if (leftRefines != rightRefines)
          // An aged merge releases a fine allocation for the approaching
          // split. Grace has already filtered transient candidates, so memory
          // reclamation wins this local ordering.
          return !leftRefines;
        if (left.desiredLOD != right.desiredLOD)
          return leftRefines
              ? left.desiredLOD < right.desiredLOD
              : left.desiredLOD > right.desiredLOD;
        return left.slot < right.slot;
      });
  if (traceTerrain && !requests.empty())
  {
    static std::mutex traceMutex;
    static std::array<uint32_t, 15u> previousSignature{};
    static bool hasPreviousSignature = false;
    const std::array<uint32_t, 15u> signature{
      static_cast<uint32_t>(
          std::min<size_t>(
              requests.size(), std::numeric_limits<uint32_t>::max())),
      static_cast<uint32_t>(
          std::min<size_t>(
              result.size(), std::numeric_limits<uint32_t>::max())),
      traceCounts.staleSlot,
      traceCounts.sameLOD,
      traceCounts.finest,
      traceCounts.belowStableLOD,
      traceCounts.spatialMerge,
      traceCounts.partialSpatialMerge,
      traceCounts.beyondMaximumLOD,
      traceCounts.duplicate,
      static_cast<uint32_t>(cache.entries.size()),
      traceCounts.requestedRefine,
      traceCounts.requestedCoarsen,
      traceCounts.acceptedRefine,
      traceCounts.acceptedCoarsen,
    };
    std::lock_guard<std::mutex> lock(traceMutex);
    if (!hasPreviousSignature || previousSignature != signature)
    {
      os::Logger::logf(
          "SVR terrain coalesce: input=%u accepted=%u stale=%u sameLOD=%u "
          "finest=%u belowStable=%u spatialMerge=%u "
          "partialSpatialMerge=%u beyondLOD=%u "
          "duplicates=%u residents=%u refine=%u coarsen=%u "
          "acceptedRefine=%u acceptedCoarsen=%u spatial=%u rootLOD=%u",
          signature[0],
          signature[1],
          traceCounts.staleSlot,
          traceCounts.sameLOD,
          traceCounts.finest,
          traceCounts.belowStableLOD,
          traceCounts.spatialMerge,
          traceCounts.partialSpatialMerge,
          traceCounts.beyondMaximumLOD,
          traceCounts.duplicate,
          static_cast<uint32_t>(
              std::min<size_t>(
                  cache.entries.size(),
                  std::numeric_limits<uint32_t>::max())),
          traceCounts.requestedRefine,
          traceCounts.requestedCoarsen,
          traceCounts.acceptedRefine,
          traceCounts.acceptedCoarsen,
          spatialFeedback ? 1u : 0u,
          spatialRootLOD);
      const auto logRequestBucket =
          [&](const char *label,
              const std::vector<WorldVoxelChunkRequest> &bucket)
      {
        const size_t count = std::min<size_t>(bucket.size(), 8u);
        for (size_t index = 0u; index < count; ++index)
        {
          const auto &request = bucket[index];
          const auto resident = std::find_if(
              cache.entries.begin(), cache.entries.end(),
              [&](const WorldVoxelChunkEntry &entry)
              {
                return entry.slot == request.slot &&
                    entry.generation == request.expectedGeneration;
              });
          const int32_t pageX =
              resident != cache.entries.end() ? resident->address.x : 0;
          const int32_t pageZ =
              resident != cache.entries.end() ? resident->address.z : 0;
          const uint32_t pageLOD = resident != cache.entries.end()
              ? resident->address.spatialLOD
              : 0xffffffffu;
          const uint32_t residentLOD = resident != cache.entries.end()
              ? resident->residentLOD
              : 0xffffffffu;
          os::Logger::logf(
              "SVR terrain coalesce %s[%zu]: slot=%u expectedGen=%u "
              "currentLOD=%u desiredLOD=%u priority=%u "
              "page=(%d,%d,lod%u) residentLOD=%u",
              label,
              index,
              request.slot,
              request.expectedGeneration,
              request.currentLOD,
              request.desiredLOD,
              request.priority,
              pageX,
              pageZ,
              pageLOD,
              residentLOD);
        }
      };
      logRequestBucket("accepted", result);
      logRequestBucket("stale", traceStaleSlot);
      logRequestBucket("sameLOD", traceSameLOD);
      logRequestBucket("finest", traceFinest);
      logRequestBucket("spatialMerge", traceSpatialMerge);
      logRequestBucket("partialSpatialMerge", tracePartialSpatialMerge);
      logRequestBucket("beyondLOD", traceBeyondMaximumLOD);
      previousSignature = signature;
      hasPreviousSignature = true;
    }
  }
  return result;
}

struct WorldVoxelChunkRequestQueue::Impl
{
  struct RankedRequest
  {
    WorldVoxelChunkRequest request{};
    uint64_t revision = 0u;
    bool cameraOwner = false;
    uint32_t visibilityTier = 0u;
    double distanceSquared = 0.0;
    double viewAlignment = 0.0;
    bool criticalCameraOwner = false;
  };

  struct LowerPriority
  {
    bool operator()(const RankedRequest &left,
                    const RankedRequest &right) const
    {
      const bool leftRefines =
          left.request.desiredLOD < left.request.currentLOD;
      const bool rightRefines =
          right.request.desiredLOD < right.request.currentLOD;
      if (leftRefines != rightRefines)
        return !leftRefines;
      if (left.criticalCameraOwner != right.criticalCameraOwner)
        return !left.criticalCameraOwner;
      // Refinement priority is the GPU-measured maximum projected error in
      // 16.16 pixels. It is the most direct estimate of visible deficit and
      // must not be hidden behind a synthetic camera-owner request or a CPU
      // page-centre heuristic. The coarsening path stores its aged-LRU score
      // in the same field, so it is compared only after refinement/coarsening
      // have been separated above.
      if (left.request.priority != right.request.priority)
        return left.request.priority < right.request.priority;
      if (left.visibilityTier != right.visibilityTier)
        return left.visibilityTier > right.visibilityTier;
      if (left.distanceSquared != right.distanceSquared)
        return left.distanceSquared > right.distanceSquared;
      if (leftRefines && left.cameraOwner != right.cameraOwner)
        return !left.cameraOwner;
      if (leftRefines && left.cameraOwner && right.cameraOwner &&
          left.request.currentLOD != right.request.currentLOD)
        return left.request.currentLOD > right.request.currentLOD;
      if (left.viewAlignment != right.viewAlignment)
        return left.viewAlignment < right.viewAlignment;
      if (left.request.slot != right.request.slot)
        return left.request.slot > right.request.slot;
      return left.revision < right.revision;
    }
  };

  static uint64_t key(const WorldVoxelChunkRequest &request)
  {
    return (static_cast<uint64_t>(request.expectedGeneration) << 32u) |
        request.slot;
  }

  void compact()
  {
    decltype(heap) replacement;
    for (const auto &[key, request] : active)
    {
      (void)key;
      replacement.push(request);
    }
    heap = std::move(replacement);
  }

  std::priority_queue<
      RankedRequest, std::vector<RankedRequest>, LowerPriority> heap;
  std::unordered_map<uint64_t, RankedRequest> active;
  std::vector<WorldVoxelChunkRequest> observations;
  uint64_t nextRevision = 1u;
};

WorldVoxelChunkRequestQueue::WorldVoxelChunkRequestQueue()
    : impl_(std::make_unique<Impl>())
{
}

WorldVoxelChunkRequestQueue::~WorldVoxelChunkRequestQueue() = default;

WorldVoxelChunkRequestQueue::WorldVoxelChunkRequestQueue(
    WorldVoxelChunkRequestQueue &&) noexcept = default;

WorldVoxelChunkRequestQueue &WorldVoxelChunkRequestQueue::operator=(
    WorldVoxelChunkRequestQueue &&) noexcept = default;

void WorldVoxelChunkRequestQueue::clear()
{
  impl_->active.clear();
  impl_->observations.clear();
  impl_->heap = {};
}

bool WorldVoxelChunkRequestQueue::empty() const
{
  return impl_->active.empty() && impl_->observations.empty();
}

size_t WorldVoxelChunkRequestQueue::size() const
{
  return impl_->active.size() + impl_->observations.size();
}

void WorldVoxelChunkRequestQueue::enqueue(
    const std::vector<WorldVoxelChunkRequest> &observations)
{
  impl_->observations.insert(
      impl_->observations.end(), observations.begin(), observations.end());
}

void WorldVoxelChunkRequestQueue::replaceFrame(
    const std::vector<WorldVoxelChunkRequest> &observations)
{
  // The heap contains lazy superseded entries, so replacing only `active`
  // would retain old revisions until they happened to reach the top. A frame
  // replacement is an exact demand boundary: rebuild from this observation
  // set on the next update().
  impl_->active.clear();
  impl_->heap = {};
  impl_->observations = observations;
  static const bool traceQueue = std::getenv("SVR_TRACE_TERRAIN") != nullptr;
  if (traceQueue)
    os::Logger::logf(
        "SVR request queue replaceFrame: count=%zu", observations.size());
}

void WorldVoxelChunkRequestQueue::update(
    const WorldVoxelChunkCache &cache,
    const double cameraMapXMetres,
    const double cameraMapZMetres,
    const WorldVoxelChunkStreamingSettings &settings,
    const std::vector<WorldVoxelChunkRequest> &observations)
{
  std::vector<WorldVoxelChunkRequest> pending;
  pending.reserve(
      impl_->active.size() + impl_->observations.size() +
      observations.size());
  for (const auto &[key, ranked] : impl_->active)
  {
    (void)key;
    pending.push_back(ranked.request);
  }
  const size_t storedObservations = impl_->observations.size();
  pending.insert(
      pending.end(), impl_->observations.begin(), impl_->observations.end());
  pending.insert(pending.end(), observations.begin(), observations.end());
  impl_->observations.clear();
  static const bool traceQueue = std::getenv("SVR_TRACE_TERRAIN") != nullptr;
  if (traceQueue)
    os::Logger::logf(
        "SVR request queue update: active=%zu stored=%zu extra=%zu pending=%zu",
        impl_->active.size(), storedObservations, observations.size(), pending.size());

  const auto accepted = coalesceWorldVoxelChunkRequests(cache, pending);
  std::unordered_map<uint64_t, Impl::RankedRequest> nextActive;
  nextActive.reserve(accepted.size());

  const double chunkEdge = settings.chunkEdgeMetres > 0.0
      ? settings.chunkEdgeMetres
      : settings.finestVoxelSizeMetres *
            static_cast<double>(settings.finestHorizontalVoxelCount);
  const double viewLength = std::hypot(
      settings.priorityViewDirectionX,
      settings.priorityViewDirectionZ);
  const double viewX = viewLength > 1.0e-12
      ? settings.priorityViewDirectionX / viewLength
      : 0.0;
  const double viewZ = viewLength > 1.0e-12
      ? settings.priorityViewDirectionZ / viewLength
      : 0.0;
  const double visibilityCosine = std::clamp(
      settings.priorityViewCosine, -1.0, 1.0);

  for (const auto &request : accepted)
  {
    const auto resident = std::find_if(
        cache.entries.begin(), cache.entries.end(),
        [&](const WorldVoxelChunkEntry &entry)
        {
          return entry.slot == request.slot &&
              entry.generation == request.expectedGeneration;
        });
    if (resident == cache.entries.end())
      continue;

    const double edge = std::ldexp(
        chunkEdge, static_cast<int>(resident->address.spatialLOD));
    const double minimumX = static_cast<double>(resident->address.x) * edge;
    const double minimumZ = static_cast<double>(resident->address.z) * edge;
    const double centerX = minimumX + edge * 0.5;
    const double centerZ = minimumZ + edge * 0.5;
    const double viewDeltaX = centerX - cameraMapXMetres;
    const double viewDeltaZ = centerZ - cameraMapZMetres;
    const double deltaX = cameraMapXMetres < minimumX
        ? minimumX - cameraMapXMetres
        : cameraMapXMetres > minimumX + edge
            ? cameraMapXMetres - (minimumX + edge)
            : 0.0;
    const double deltaZ = cameraMapZMetres < minimumZ
        ? minimumZ - cameraMapZMetres
        : cameraMapZMetres > minimumZ + edge
            ? cameraMapZMetres - (minimumZ + edge)
            : 0.0;
    const double distanceSquared = deltaX * deltaX + deltaZ * deltaZ;
    const double distance = std::sqrt(distanceSquared);
    const double viewDistance = std::hypot(viewDeltaX, viewDeltaZ);
    const double alignment = viewLength > 1.0e-12 && viewDistance > 1.0e-12
        ? (viewDeltaX * viewX + viewDeltaZ * viewZ) / viewDistance
        : 1.0;
    const uint32_t ring = chunkEdge > 0.0
        ? static_cast<uint32_t>(std::floor(distance / chunkEdge))
        : 0u;
    // Screen visibility breaks equal-error ties before proximity. Previously
    // every page in the near ring was tier zero, including pages behind the
    // camera, so a close off-screen owner could repeatedly beat the visible
    // cut while turning.
    const bool inView = alignment >= visibilityCosine;
    const uint32_t visibilityTier = inView
        ? (ring <= settings.priorityNearChunkRadius ? 0u : 1u)
        : 2u;

    Impl::RankedRequest ranked{
      request,
      impl_->nextRevision++,
      distanceSquared <= 1.0e-18,
      visibilityTier,
      distanceSquared,
      alignment};
    ranked.criticalCameraOwner = settings.priorityCameraOwnerRefinement &&
        request.desiredLOD < request.currentLOD &&
        cameraMapXMetres >= minimumX && cameraMapXMetres < minimumX + edge &&
        cameraMapZMetres >= minimumZ && cameraMapZMetres < minimumZ + edge;
    if (impl_->nextRevision == 0u)
      impl_->nextRevision = 1u;
    nextActive.insert_or_assign(Impl::key(request), ranked);
    impl_->heap.push(std::move(ranked));
  }
  impl_->active = std::move(nextActive);

  const size_t compactThreshold =
      std::max<size_t>(impl_->active.size() * 4u, 64u);
  if (impl_->heap.size() > compactThreshold)
    impl_->compact();
}

std::vector<WorldVoxelChunkRequest> WorldVoxelChunkRequestQueue::take(
    const WorldVoxelChunkCache &cache,
    const double cameraMapXMetres,
    const double cameraMapZMetres,
    const WorldVoxelChunkStreamingSettings &settings,
    const size_t maximumCount)
{
  update(
      cache, cameraMapXMetres, cameraMapZMetres, settings, {});
  const size_t count = maximumCount == 0u
      ? impl_->active.size()
      : std::min(maximumCount, impl_->active.size());
  std::vector<WorldVoxelChunkRequest> result;
  result.reserve(count);
  while (result.size() < count && !impl_->heap.empty())
  {
    Impl::RankedRequest ranked = impl_->heap.top();
    impl_->heap.pop();
    const uint64_t key = Impl::key(ranked.request);
    const auto active = impl_->active.find(key);
    if (active == impl_->active.end() ||
        active->second.revision != ranked.revision)
      continue;
    result.push_back(ranked.request);
    impl_->active.erase(active);

    if (settings.renderDistanceMetres > 0.0 &&
        ranked.request.desiredLOD > ranked.request.currentLOD)
    {
      const auto resident = std::find_if(
          cache.entries.begin(), cache.entries.end(),
          [&](const WorldVoxelChunkEntry &entry)
          {
            return entry.slot == ranked.request.slot &&
                entry.generation == ranked.request.expectedGeneration;
          });
      if (resident == cache.entries.end())
        continue;
      const WorldVoxelChunkAddress parent =
          worldVoxelPageParent(resident->address);
      for (auto sibling = impl_->active.begin();
           sibling != impl_->active.end();)
      {
        const auto &candidate = sibling->second.request;
        if (candidate.desiredLOD <= candidate.currentLOD)
        {
          ++sibling;
          continue;
        }
        const auto siblingResident = std::find_if(
            cache.entries.begin(), cache.entries.end(),
            [&](const WorldVoxelChunkEntry &entry)
            {
              return entry.slot == candidate.slot &&
                  entry.generation == candidate.expectedGeneration;
            });
        if (siblingResident == cache.entries.end() ||
            !(worldVoxelPageParent(siblingResident->address) == parent))
        {
          ++sibling;
          continue;
        }
        result.push_back(candidate);
        sibling = impl_->active.erase(sibling);
      }
    }
  }
  return result;
}

namespace
{

using WorldPhysicalRange = WorldVoxelChunkEntry::PhysicalRange;
using WorldPhysicalAllocation = WorldVoxelChunkEntry::PhysicalAllocation;

template <typename T>
uint64_t worldVectorBytes(const std::vector<T> &values)
{
  return static_cast<uint64_t>(values.size()) * sizeof(T);
}

uint64_t worldVoxelPayloadBytes(const PlanetVoxelChunkSet &page)
{
  uint64_t bytes = sizeof(page);
  const auto add = [&](const uint64_t value)
  {
    bytes = value > std::numeric_limits<uint64_t>::max() - bytes
        ? std::numeric_limits<uint64_t>::max()
        : bytes + value;
  };
  add(worldVectorBytes(page.chunks));
  add(worldVectorBytes(page.bricks.nodes));
  add(worldVectorBytes(page.bricks.occupancy));
  add(worldVectorBytes(page.bricks.childIndices));
  add(worldVectorBytes(page.bricks.rootIndices));
  add(worldVectorBytes(page.bricks.materialIndices));
  add(worldVectorBytes(page.bricks.microSurfaceData));
  add(worldVectorBytes(page.bricks.palette));
  add(worldVectorBytes(page.bricks.paletteDescriptorIds));
  add(worldVectorBytes(page.surfaceHeights));
  add(worldVectorBytes(page.surfaceEnvelope));
  add(worldVectorBytes(page.surfaceMaterials));
  add(worldVectorBytes(page.materialFieldRegions));
  return bytes;
}

uint64_t worldVoxelCPUCacheBytes(const WorldVoxelChunkCache &cache)
{
  uint64_t bytes = 0u;
  for (const auto &payload : cache.payloadCache)
  {
    const uint64_t payloadBytes = worldVoxelPayloadBytes(*payload.chunk);
    if (payloadBytes > std::numeric_limits<uint64_t>::max() - bytes)
      return std::numeric_limits<uint64_t>::max();
    bytes += payloadBytes;
  }
  return bytes;
}

uint64_t worldVoxelGPUPageBytes(const WorldVoxelChunkCache &cache)
{
  uint64_t bytes = 0u;
  const auto add = [&](const uint64_t count, const uint64_t stride)
  {
    if (count > (std::numeric_limits<uint64_t>::max() - bytes) / stride)
      bytes = std::numeric_limits<uint64_t>::max();
    else
      bytes += count * stride;
  };
  add(cache.arenaNodeCapacity, sizeof(SparseVoxelBrickNodeGPU));
  add(cache.arenaNodeCapacity, sizeof(SparseVoxelBrickOccupancyGPU));
  add(cache.arenaChildCapacity, sizeof(uint32_t));
  add(cache.arenaRootCapacity, sizeof(uint32_t));
  add((uint64_t{cache.arenaMaterialCapacity} + 3u) / 4u,
      sizeof(uint32_t));
  add(cache.arenaMicroSurface ? cache.arenaMaterialCapacity : 0u, sizeof(uint64_t));
  add(cache.arenaPaletteCapacity, sizeof(SparseVoxelBrickMaterialGPU));
  add((uint64_t{cache.arenaPaletteCapacity} + 1u) / 2u,
      sizeof(uint32_t));
  // Current, phase-two and previous-frame visibility ownership arrays.
  add(cache.arenaNodeCapacity, 3u * sizeof(uint32_t));
  return bytes;
}

enum class WorldVoxelTerrainPageSourceKind : uint8_t
{
  FrontCache,
  TransientMemory,
  TransientDisk,
  Generated,
};

struct WorldVoxelTerrainPageLoad
{
  WorldVoxelTerrainPageSourceKind source =
      WorldVoxelTerrainPageSourceKind::Generated;
  WorldMapVoxelGenerationMetrics metrics{};
  bool transientStored = false;
};

class WorldVoxelTerrainPageSource
{
public:
  WorldVoxelTerrainPageSource(
      const procedural::world::MapExportBundle &bundle,
      const uint64_t bundleIdentity,
      const double chunkEdgeMetres,
      const WorldVoxelChunkStreamingSettings &settings,
      WorldVoxelChunkCache &cache,
      WorldVoxelChunkCacheUpdate *update,
      const bool transientStoreActive)
      : bundle_(bundle),
        bundleIdentity_(bundleIdentity),
        chunkEdgeMetres_(chunkEdgeMetres),
        settings_(settings),
        cache_(cache),
        update_(update),
        transientStoreActive_(transientStoreActive)
  {
  }

  bool loadCached(
      const WorldVoxelChunkAddress &address,
      const uint32_t internalLOD,
      PlanetVoxelChunkSet &page,
      WorldVoxelTerrainPageSourceKind &source)
  {
    if (restoreFrontCache(address, internalLOD, page))
    {
      source = WorldVoxelTerrainPageSourceKind::FrontCache;
      markDirectoryCached(address, internalLOD, page);
      return true;
    }
    bool fromDisk = false;
    if (restoreTransient(address, internalLOD, page, fromDisk))
    {
      source = fromDisk
          ? WorldVoxelTerrainPageSourceKind::TransientDisk
          : WorldVoxelTerrainPageSourceKind::TransientMemory;
      markDirectoryCached(address, internalLOD, page);
      return true;
    }
    return false;
  }

  bool load(
      const WorldVoxelChunkAddress &address,
      const uint32_t internalLOD,
      const uint32_t elevationAnchorLOD,
      PlanetVoxelChunkSet &page,
      WorldVoxelTerrainPageLoad &load,
      std::string *error,
      const PlanetVoxelChunkProgress &progress = {},
      const std::atomic_bool *cancellation = nullptr)
  {
    if (loadCached(address, internalLOD, page, load.source))
      return true;
    markDirectoryGenerating(address, internalLOD);
    if (!generate(
            address, internalLOD, elevationAnchorLOD, page, load.metrics,
            error, progress, cancellation))
      return false;
    load.source = WorldVoxelTerrainPageSourceKind::Generated;
    load.transientStored = storeTransient(address, internalLOD, page);
    return true;
  }

  bool generate(
      const WorldVoxelChunkAddress &address,
      const uint32_t internalLOD,
      const uint32_t elevationAnchorLOD,
      PlanetVoxelChunkSet &page,
      WorldMapVoxelGenerationMetrics &metrics,
      std::string *error,
      const PlanetVoxelChunkProgress &progress = {},
      const std::atomic_bool *cancellation = nullptr,
      const uint32_t columnSamplingWorkerCount = 0u) const
  {
    WorldMapVoxelTerrainSettings pageSettings{};
    if (!makePageSettings(address, internalLOD, elevationAnchorLOD, pageSettings))
      return fail(error, "Generated-world page source resolved an invalid page");
    pageSettings.columnSamplingWorkerCount = columnSamplingWorkerCount;
    pageSettings.voxel.buildMicroSurface = settings_.packMicroSurface;
    if (settings_.gpuOwnedPayloads)
      return planGPUOwnedPage(address, pageSettings, page, metrics, error);
    return buildWorldMapVoxelTerrain(
        bundle_, pageSettings, page, error, progress, cancellation, &metrics);
  }

  bool storeTransient(
      const WorldVoxelChunkAddress &address,
      const uint32_t internalLOD,
      const PlanetVoxelChunkSet &page)
  {
    if (!transientStoreActive_ || !cache_.transientPages ||
        page.bricks.empty())
      return false;
    std::string transientError;
    const bool stored = cache_.transientPages->put(
        worldTransientPageKey(bundleIdentity_, settings_, address, internalLOD),
        serializeWorldVoxelTransientPage(page),
        &transientError);
    if (stored)
      markDirectoryCached(address, internalLOD, page);
    return stored;
  }

private:
  bool planGPUOwnedPage(
      const WorldVoxelChunkAddress &address,
      const WorldMapVoxelTerrainSettings &pageSettings,
      PlanetVoxelChunkSet &page,
      WorldMapVoxelGenerationMetrics &metrics,
      std::string *error) const
  {
    page = {};
    metrics = {};
    const uint32_t side = pageSettings.finestHorizontalVoxelCount /
        (uint32_t{1u} << pageSettings.internalLOD);
    const uint32_t maximumVertical =
        pageSettings.voxel.maximumVerticalVoxels;
    if (side < kSparseVoxelBrickSide ||
        side % kSparseVoxelBrickSide != 0u || maximumVertical == 0u)
      return fail(error, "GPU-owned terrain page dimensions are invalid");

    // The inactive build lane uses a deterministic page class. It is sized
    // from page dimensions only, so planning never samples a map or expands a
    // terrain column on the CPU. Material storage covers the horizontal shell
    // plus a bounded margin for vertical cliff closures. The old exact
    // horizontal-shell count left no closure allowance at all: a real fine
    // page produced 32,769 solid cells for a 32,768-cell class and permanently
    // stalled refinement one cell over capacity.
    const auto defaultNodeCapacity = [&]()
    {
      uint64_t count = 0u;
      uint32_t x = side / kSparseVoxelBrickSide;
      uint32_t y = (maximumVertical + kSparseVoxelBrickSide - 1u) /
          kSparseVoxelBrickSide;
      while (true)
      {
        count += uint64_t{x} * y * x;
        if (x <= 2u && y == 1u)
          break;
        x = std::max((x + 1u) / 2u, 1u);
        y = std::max((y + 1u) / 2u, 1u);
      }
      return static_cast<uint32_t>(std::min<uint64_t>(
          count, std::numeric_limits<uint32_t>::max()));
    }();
    const uint32_t productionNodeClass = std::min(
        defaultNodeCapacity, 8192u);
    const uint32_t nodeCapacity = settings_.gpuPageNodeCapacity != 0u
        ? settings_.gpuPageNodeCapacity : productionNodeClass;
    const uint32_t childCapacity = settings_.gpuPageChildCapacity != 0u
        ? settings_.gpuPageChildCapacity : nodeCapacity;
    const uint32_t rootCapacity = settings_.gpuPageRootCapacity != 0u
        ? settings_.gpuPageRootCapacity : nodeCapacity;
    const uint64_t horizontalShellMaterials = uint64_t{side} * side *
        std::max(1u, std::min(
            maximumVertical,
            pageSettings.voxel.surfaceShellDepthVoxels + 1u));
    const uint64_t cliffClosureMargin =
        std::max<uint64_t>(uint64_t{side} * side / 4u, side);
    // Coarser brick levels retain conservative occupancy for culling and
    // Tree64 parity and therefore also carry compact material indices. A 2-D
    // shell contributes a geometric 1/4 + 1/16 + ... overhead (one third of
    // the leaf level); two leaf levels plus the closure margin is a bounded,
    // conservative class without allocating the full volume.
    const uint64_t defaultMaterials64 =
        horizontalShellMaterials * 2u + cliffClosureMargin;
    const uint32_t materialCapacity = settings_.gpuPageMaterialCapacity != 0u
        ? settings_.gpuPageMaterialCapacity
        : static_cast<uint32_t>(std::min<uint64_t>(
              defaultMaterials64, std::numeric_limits<uint32_t>::max()));
    const uint32_t paletteCapacity = settings_.gpuPagePaletteCapacity != 0u
        ? settings_.gpuPagePaletteCapacity : 256u;
    if (nodeCapacity == 0u || childCapacity == 0u ||
        rootCapacity == 0u || materialCapacity == 0u ||
        paletteCapacity == 0u || paletteCapacity > 256u)
      return fail(error, "GPU-owned terrain page class is invalid");

    const double minimumU = std::clamp(std::min(
        pageSettings.normalizedBounds[0],
        pageSettings.normalizedBounds[2]), 0.0, 1.0);
    const double minimumV = std::clamp(std::min(
        pageSettings.normalizedBounds[1],
        pageSettings.normalizedBounds[3]), 0.0, 1.0);
    const double maximumU = std::clamp(std::max(
        pageSettings.normalizedBounds[0],
        pageSettings.normalizedBounds[2]), 0.0, 1.0);
    const double maximumV = std::clamp(std::max(
        pageSettings.normalizedBounds[1],
        pageSettings.normalizedBounds[3]), 0.0, 1.0);
    const double edge = (maximumU - minimumU) * bundle_.extentMetres;
    const double voxelSize = edge / side;
    const double centerX =
        ((minimumU + maximumU) * 0.5 - 0.5) * bundle_.extentMetres;
    const double centerZ =
        ((minimumV + maximumV) * 0.5 - 0.5) * bundle_.extentMetres;
    if (!(edge > 0.0) || !(voxelSize > 0.0) ||
        !std::isfinite(centerX) || !std::isfinite(centerZ))
      return fail(error, "GPU-owned terrain page placement is invalid");

    // Capacity is allocator metadata, not a request for a CPU payload. The
    // immutable material lookup is uploaded once by the GPU build lane and is
    // likewise not duplicated per logical page.
    page.gpuArenaNodeCapacity = nodeCapacity;
    page.gpuArenaChildCapacity = childCapacity;
    page.gpuArenaRootCapacity = rootCapacity;
    page.gpuArenaMaterialCapacity = materialCapacity;
    page.gpuArenaPaletteCapacity = paletteCapacity;
    // The shipping GPU path publishes solid leaf bricks directly. Every
    // occupied leaf can therefore be a draw root; reserving only the former
    // four hierarchy roots rejects an otherwise valid leaf-only page during
    // the atomic relocation. `brickRootCount` remains the metadata placeholder
    // until the GPU result publishes the exact compact root count.
    constexpr uint32_t generatedRootCount = 1u;
    page.bricks.rootCount = generatedRootCount;

    // The GPU column result owns the actual per-page datum. Zero here is only
    // a stable metadata placeholder and is never uploaded as geometry.
    const double elevationOrigin = 0.0;
    uint64_t revision = 0xcbf29ce484222325ull;
    const auto mix = [&](const uint64_t value)
    {
      revision ^= value;
      revision *= 0x100000001b3ull;
    };
    mix(bundleIdentity_);
    mix(settings_.generatorRevision);
    mix(static_cast<uint32_t>(address.x));
    mix(static_cast<uint32_t>(address.z));
    mix(address.spatialLOD);
    mix(pageSettings.internalLOD);
    if (revision == 0u)
      revision = 1u;

    PlanetVoxelChunkRecord record{};
    record.requestedTile = {0u, 0u, 0u, 0u};
    record.sourceTile = record.requestedTile;
    record.worldCenterXMetres = centerX;
    record.worldCenterZMetres = centerZ;
    record.edgeMetres = edge;
    record.voxelSizeMetres = voxelSize;
    record.brickNodeCapacity = nodeCapacity;
    record.brickChildCapacity = childCapacity;
    record.brickMaterialCapacity = materialCapacity;
    record.brickPaletteCapacity = paletteCapacity;
    record.brickRootCapacity = rootCapacity;
    record.brickRootCount = generatedRootCount;
    record.elevationOriginMetres = elevationOrigin;
    record.minimumSurfaceMetres = elevationOrigin;
    record.maximumSurfaceMetres =
        elevationOrigin + maximumVertical * voxelSize;
    record.centerSurfaceMetres = elevationOrigin;
    record.leafVoxelCount = side * side;
    record.brickCount = nodeCapacity;
    record.brickPayloadRevision = revision;
    record.brickNodeRevision = revision;
    record.tree64Revision = kTerrainGPUColumnGeneratorRevision;
    page.chunks.push_back(record);
    page.edgeMetres = edge;
    page.voxelSizeMetres = voxelSize;
    page.elevationOriginMetres = elevationOrigin;
    page.centerSurfaceMetres = elevationOrigin;
    page.leafVoxelCount = side * side;
    page.clipmapLayoutRevision = revision;
    page.gpuOwnedGeneratedPayloads = true;
    page.minimumWorldMetres = {
      centerX - edge * 0.5, 0.0, centerZ - edge * 0.5};
    page.maximumWorldMetres = {
      centerX + edge * 0.5, maximumVertical * voxelSize,
      centerZ + edge * 0.5};
    metrics.internalLOD = pageSettings.internalLOD;
    metrics.horizontalVoxelCount = side;
    metrics.verticalVoxelCapacity = maximumVertical;
    metrics.brickNodeCount = nodeCapacity;
    metrics.brickRootCount = generatedRootCount;
    return true;
  }

  bool makePageSettings(
      const WorldVoxelChunkAddress &address,
      const uint32_t internalLOD,
      const uint32_t elevationAnchorLOD,
      WorldMapVoxelTerrainSettings &pageSettings) const
  {
    const double spatialScale = std::ldexp(1.0, address.spatialLOD);
    const double pageEdge = chunkEdgeMetres_ * spatialScale;
    const double minimumX = static_cast<double>(address.x) * pageEdge;
    const double minimumZ = static_cast<double>(address.z) * pageEdge;
    if (!(bundle_.extentMetres > 0.0) || !(pageEdge > 0.0))
      return false;
    pageSettings = {};
    pageSettings.normalizedBounds = {
      minimumX / bundle_.extentMetres,
      minimumZ / bundle_.extentMetres,
      (minimumX + pageEdge) / bundle_.extentMetres,
      (minimumZ + pageEdge) / bundle_.extentMetres};
    pageSettings.requestedLevel = bundle_.maximumPageLevel;
    pageSettings.voxel = settings_.voxel;
    // Spatially coarse pages sample each edge voxel at two sub-intervals when
    // deriving their vertical closure, so their edge wall also reaches the
    // midpoint an adjacent 2:1 finer page represents. The GPU raster lane
    // has always done this; the CPU lane left it at one and opened seams
    // between LOD levels on slopes.
    // Measured on the slope test (Phase 29 of the cluster plan): seams between
    // pages are at most 22 of 230k pixels with or without stitching, so the
    // CPU lane keeps the single interval. SVR_TERRAIN_STITCH=<n> overrides it
    // for experiments.
    pageSettings.boundaryStitchSubdivisions = 1u;
    if (const char *forced = std::getenv("SVR_TERRAIN_STITCH"))
      pageSettings.boundaryStitchSubdivisions =
          std::clamp<uint32_t>(static_cast<uint32_t>(std::atoi(forced)), 1u, 8u);
    // Terrain pages keep slope context metadata but no longer emit exact
    // microgeometry. Descriptor-driven terrain coverage patches produced hard
    // stair bands and visible contour lines on organic slopes.
    pageSettings.voxel.microGeometry = false;
    pageSettings.voxel.horizontalVoxelCount =
        settings_.finestHorizontalVoxelCount;
    pageSettings.voxel.patchEdgeMetres = pageEdge;
    // Generated terrain is a height-field surface. Keep at least one closure
    // cell below exposed side walls, but let the world-map sample request a
    // deeper shell for coarse spatial pages where a single top voxel can
    // under-cover grazing terrain.
    pageSettings.voxel.surfaceShellDepthVoxels =
        std::max<uint32_t>(settings_.voxel.surfaceShellDepthVoxels, 1u);
    pageSettings.voxel.clipmapLevelCount = 1u;
    pageSettings.voxel.requestedLODMask = 1u;
    pageSettings.internalLOD = internalLOD;
    pageSettings.finestHorizontalVoxelCount =
        settings_.finestHorizontalVoxelCount;
    pageSettings.finestVoxelSizeMetres =
        settings_.finestVoxelSizeMetres * spatialScale;
    pageSettings.elevationAnchorLOD = elevationAnchorLOD;
    pageSettings.generatorRevision = settings_.generatorRevision;
    pageSettings.exteriorShellOnly = true;
    pageSettings.boundaryStitchSubdivisions =
        address.spatialLOD + internalLOD > 0u ? 2u : 1u;
    return true;
  }

  bool restoreFrontCache(
      const WorldVoxelChunkAddress &address,
      const uint32_t internalLOD,
      PlanetVoxelChunkSet &page)
  {
    const auto cachedPayload = std::find_if(
        cache_.payloadCache.begin(), cache_.payloadCache.end(),
        [&](const WorldVoxelChunkCachedPayload &entry)
        {
          return entry.bundleSignature == bundleIdentity_ &&
              entry.address == address && entry.lod == internalLOD &&
              entry.generatorRevision == settings_.generatorRevision;
        });
    if (cachedPayload == cache_.payloadCache.end())
      return false;
    if (update_ != nullptr)
      update_->removedPayloadRollbackEntries.push_back(*cachedPayload);
    page = *cachedPayload->chunk;
    cache_.payloadCache.erase(cachedPayload);
    return true;
  }

  bool restoreTransient(
      const WorldVoxelChunkAddress &address,
      const uint32_t internalLOD,
      PlanetVoxelChunkSet &page,
      bool &fromDisk)
  {
    fromDisk = false;
    if (!transientStoreActive_ || !cache_.transientPages)
      return false;
    const auto key = worldTransientPageKey(
        bundleIdentity_, settings_, address, internalLOD);
    std::vector<uint8_t> payload;
    std::string transientError;
    const auto source =
        cache_.transientPages->get(key, payload, &transientError);
    if (source == TransientSparseVoxelPageSource::Missing)
      return false;
    if (!deserializeWorldVoxelTransientPage(payload, page, &transientError))
    {
      cache_.transientPages->erase(key);
      return false;
    }
    fromDisk = source == TransientSparseVoxelPageSource::Disk;
    return true;
  }

  void markDirectoryGenerating(
      const WorldVoxelChunkAddress &address,
      const uint32_t internalLOD)
  {
    const uint32_t index = cache_.pageDirectory.find(address);
    if (index == kWorldVoxelInvalidPage)
      return;
    auto &entry = cache_.pageDirectory.entries[index];
    if (entry.state == WorldVoxelPageState::Resident ||
        entry.state == WorldVoxelPageState::Installing)
      return;
    entry.state = WorldVoxelPageState::Generating;
    entry.payloadLOD = internalLOD;
    entry.payloadKey = worldTransientPageKey(
        bundleIdentity_, settings_, address, internalLOD);
  }

  void markDirectoryCached(
      const WorldVoxelChunkAddress &address,
      const uint32_t internalLOD,
      const PlanetVoxelChunkSet &page)
  {
    const uint32_t index = cache_.pageDirectory.find(address);
    if (index == kWorldVoxelInvalidPage)
      return;
    auto &entry = cache_.pageDirectory.entries[index];
    if (entry.state != WorldVoxelPageState::Resident &&
        entry.state != WorldVoxelPageState::Installing)
      entry.state = WorldVoxelPageState::Cached;
    entry.payloadLOD = internalLOD;
    entry.payloadKey =
        worldTransientPageKey(bundleIdentity_, settings_, address, internalLOD);
    entry.payloadRevision = page.clipmapLayoutRevision;
    if (!page.chunks.empty())
      entry.measuredErrorMetres = page.chunks.front().measuredErrorMetres;
  }

  const procedural::world::MapExportBundle &bundle_;
  uint64_t bundleIdentity_ = 0u;
  double chunkEdgeMetres_ = 0.0;
  const WorldVoxelChunkStreamingSettings &settings_;
  WorldVoxelChunkCache &cache_;
  WorldVoxelChunkCacheUpdate *update_ = nullptr;
  bool transientStoreActive_ = false;
};

bool allocateWorldPhysicalRange(
    std::vector<WorldVoxelPhysicalFreeRange> &freeRanges,
    const uint32_t required,
    const uint32_t alignment,
    WorldPhysicalRange &allocation)
{
  allocation = {};
  if (required == 0u)
    return true;
  const uint64_t alignedRequired64 = alignment > 1u
      ? (static_cast<uint64_t>(required) + alignment - 1u) &
          ~(static_cast<uint64_t>(alignment) - 1u)
      : required;
  if (alignedRequired64 > std::numeric_limits<uint32_t>::max())
    return false;
  const uint32_t alignedRequired = static_cast<uint32_t>(alignedRequired64);
  for (size_t index = 0u; index < freeRanges.size(); ++index)
  {
    auto &range = freeRanges[index];
    const uint64_t alignedOffset64 = alignment > 1u
        ? (static_cast<uint64_t>(range.offset) + alignment - 1u) &
            ~(static_cast<uint64_t>(alignment) - 1u)
        : range.offset;
    const uint64_t padding = alignedOffset64 - range.offset;
    if (padding + alignedRequired > range.capacity)
      continue;
    const uint32_t alignedOffset = static_cast<uint32_t>(alignedOffset64);
    const uint32_t suffixOffset = alignedOffset + alignedRequired;
    const uint32_t suffixCapacity =
        range.capacity - static_cast<uint32_t>(padding) - alignedRequired;
    if (padding == 0u)
    {
      if (suffixCapacity == 0u)
        freeRanges.erase(freeRanges.begin() + index);
      else
        range = {suffixOffset, suffixCapacity};
    }
    else
    {
      range.capacity = static_cast<uint32_t>(padding);
      if (suffixCapacity != 0u)
        freeRanges.insert(
            freeRanges.begin() + index + 1u,
            {suffixOffset, suffixCapacity});
    }
    allocation = {alignedOffset, alignedRequired};
    return true;
  }
  return false;
}

void releaseWorldPhysicalRange(
    std::vector<WorldVoxelPhysicalFreeRange> &freeRanges,
    const WorldPhysicalRange allocation)
{
  if (allocation.capacity == 0u)
    return;
  freeRanges.push_back({allocation.offset, allocation.capacity});
  std::sort(
      freeRanges.begin(), freeRanges.end(),
      [](const auto &left, const auto &right)
      {
        return left.offset < right.offset;
      });
  size_t write = 0u;
  for (size_t read = 0u; read < freeRanges.size(); ++read)
  {
    if (write != 0u &&
        static_cast<uint64_t>(freeRanges[write - 1u].offset) +
                freeRanges[write - 1u].capacity ==
            freeRanges[read].offset)
    {
      freeRanges[write - 1u].capacity += freeRanges[read].capacity;
      continue;
    }
    freeRanges[write++] = freeRanges[read];
  }
  freeRanges.resize(write);
}

void releaseWorldPhysicalAllocation(
    WorldVoxelChunkCache &cache,
    const WorldPhysicalAllocation &allocation)
{
  releaseWorldPhysicalRange(cache.freeNodeRanges, allocation.nodes);
  releaseWorldPhysicalRange(cache.freeChildRanges, allocation.children);
  releaseWorldPhysicalRange(cache.freeRootRanges, allocation.roots);
  releaseWorldPhysicalRange(cache.freeMaterialRanges, allocation.materials);
  releaseWorldPhysicalRange(cache.freePaletteRanges, allocation.palette);
}

bool allocateWorldPhysicalAllocation(
    WorldVoxelChunkCache &cache,
    const PlanetVoxelChunkSet &page,
    WorldPhysicalAllocation &allocation)
{
  const auto capacity = [&](const uint32_t gpuCapacity, const size_t cpuSize)
  {
    return page.gpuOwnedGeneratedPayloads
        ? gpuCapacity
        : static_cast<uint32_t>(cpuSize);
  };
  const uint32_t nodeCount = capacity(
      page.gpuArenaNodeCapacity, page.bricks.nodes.size());
  const uint32_t childCount = capacity(
      page.gpuArenaChildCapacity, page.bricks.childIndices.size());
  const uint32_t rootCount = capacity(
      page.gpuArenaRootCapacity, page.bricks.rootIndices.size());
  const uint32_t materialCount = capacity(
      page.gpuArenaMaterialCapacity, page.bricks.materialIndices.size());
  const uint32_t paletteCount = capacity(
      page.gpuArenaPaletteCapacity, page.bricks.palette.size());
  const auto nodeRanges = cache.freeNodeRanges;
  const auto childRanges = cache.freeChildRanges;
  const auto rootRanges = cache.freeRootRanges;
  const auto materialRanges = cache.freeMaterialRanges;
  const auto paletteRanges = cache.freePaletteRanges;
  allocation = {};
  allocation.id = cache.nextAllocationId++;
  if (allocation.id == 0u)
    allocation.id = cache.nextAllocationId++;
  const bool allocated =
      allocateWorldPhysicalRange(
          cache.freeNodeRanges,
          nodeCount, 1u,
          allocation.nodes) &&
      allocateWorldPhysicalRange(
          cache.freeChildRanges,
          childCount, 1u,
          allocation.children) &&
      allocateWorldPhysicalRange(
          cache.freeRootRanges,
          rootCount, 1u,
          allocation.roots) &&
      allocateWorldPhysicalRange(
          cache.freeMaterialRanges,
          materialCount, 4u,
          allocation.materials) &&
      allocateWorldPhysicalRange(
          cache.freePaletteRanges,
          paletteCount, 2u,
          allocation.palette);
  if (allocated)
    return true;
  cache.freeNodeRanges = nodeRanges;
  cache.freeChildRanges = childRanges;
  cache.freeRootRanges = rootRanges;
  cache.freeMaterialRanges = materialRanges;
  cache.freePaletteRanges = paletteRanges;
  --cache.nextAllocationId;
  allocation = {};
  return false;
}

void reclaimWorldPhysicalAllocations(
    WorldVoxelChunkCache &cache,
    const uint64_t readerFrame)
{
  auto retained = cache.retiredAllocations.begin();
  for (auto entry = cache.retiredAllocations.begin();
       entry != cache.retiredAllocations.end(); ++entry)
  {
    if (entry->releaseFrame <= readerFrame)
    {
      releaseWorldPhysicalAllocation(cache, entry->allocation);
      continue;
    }
    if (retained != entry)
      *retained = std::move(*entry);
    ++retained;
  }
  cache.retiredAllocations.erase(retained, cache.retiredAllocations.end());
}

void retireWorldPhysicalAllocation(
    WorldVoxelChunkCache &cache,
    const WorldPhysicalAllocation &allocation,
    const WorldVoxelChunkStreamingSettings &settings)
{
  if (allocation.id == 0u)
    return;
  cache.retiredAllocations.push_back({
    allocation,
    settings.readerFrame,
    std::numeric_limits<uint64_t>::max()});
}

} // namespace

bool validateWorldVoxelChunkPhysicalPages(
    const WorldVoxelChunkCache &cache,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!cache.virtualArenaInitialized)
    return true;
  const auto validateArena = [&]<typename SelectAllocation,
                                 typename SelectFree>(
      const uint32_t arenaCapacity,
      SelectAllocation selectAllocation,
      SelectFree selectFree,
      const char *name)
  {
    struct Span
    {
      uint32_t offset = 0u;
      uint32_t capacity = 0u;
    };
    std::vector<Span> spans;
    for (const auto &entry : cache.entries)
    {
      const auto range = selectAllocation(entry.allocation);
      if (range.capacity != 0u)
        spans.push_back({range.offset, range.capacity});
    }
    for (const auto &entry : cache.stagedSpatialEntries)
    {
      const auto range = selectAllocation(entry.allocation);
      if (range.capacity != 0u)
        spans.push_back({range.offset, range.capacity});
    }
    for (const auto &retired : cache.retiredAllocations)
    {
      const auto range = selectAllocation(retired.allocation);
      if (range.capacity != 0u)
        spans.push_back({range.offset, range.capacity});
    }
    for (const auto &range : selectFree(cache))
      if (range.capacity != 0u)
        spans.push_back({range.offset, range.capacity});
    std::sort(
        spans.begin(), spans.end(),
        [](const Span &left, const Span &right)
        {
          return left.offset < right.offset;
        });
    uint64_t end = 0u;
    for (const auto &span : spans)
    {
      if (span.offset < end ||
          static_cast<uint64_t>(span.offset) + span.capacity > arenaCapacity)
      {
        if (error != nullptr)
          *error = std::string("Generated-world ") + name +
              " physical pages overlap or exceed their arena";
        return false;
      }
      end = static_cast<uint64_t>(span.offset) + span.capacity;
    }
    if (end != arenaCapacity)
    {
      if (error != nullptr)
        *error = std::string("Generated-world ") + name +
            " physical arena has an untracked gap";
      return false;
    }
    return true;
  };
  return validateArena(
             cache.arenaNodeCapacity,
             [](const WorldPhysicalAllocation &value) { return value.nodes; },
             [](const WorldVoxelChunkCache &value) -> const auto &
             { return value.freeNodeRanges; }, "node") &&
      validateArena(
          cache.arenaChildCapacity,
          [](const WorldPhysicalAllocation &value) { return value.children; },
          [](const WorldVoxelChunkCache &value) -> const auto &
          { return value.freeChildRanges; }, "child") &&
      validateArena(
          cache.arenaRootCapacity,
          [](const WorldPhysicalAllocation &value) { return value.roots; },
          [](const WorldVoxelChunkCache &value) -> const auto &
          { return value.freeRootRanges; }, "root") &&
      validateArena(
          cache.arenaMaterialCapacity,
          [](const WorldPhysicalAllocation &value) { return value.materials; },
          [](const WorldVoxelChunkCache &value) -> const auto &
          { return value.freeMaterialRanges; }, "material") &&
      validateArena(
          cache.arenaPaletteCapacity,
          [](const WorldPhysicalAllocation &value) { return value.palette; },
          [](const WorldVoxelChunkCache &value) -> const auto &
          { return value.freePaletteRanges; }, "palette");
}

void markWorldVoxelChunkPhysicalPagesPublished(
    WorldVoxelChunkCache &cache,
    const uint64_t readerFrame,
    const uint32_t deferredReleaseFrames)
{
  const uint64_t releaseFrame =
      readerFrame > std::numeric_limits<uint64_t>::max() -
              deferredReleaseFrames
      ? std::numeric_limits<uint64_t>::max()
      : readerFrame + deferredReleaseFrames;
  for (auto &retired : cache.retiredAllocations)
    if (retired.releaseFrame == std::numeric_limits<uint64_t>::max())
    {
      retired.lastReaderFrame = readerFrame;
      retired.releaseFrame = releaseFrame;
    }
}

void rollbackWorldVoxelChunkCacheUpdate(
    WorldVoxelChunkCache &cache,
    WorldVoxelChunkCacheUpdate &update)
{
  if (update.refinementRollbackEntries.empty() &&
      update.evictedRollbackEntries.empty() &&
      !update.physicalAllocatorSnapshot)
    return;
  cache.entries.erase(
      std::remove_if(
          cache.entries.begin(), cache.entries.end(),
          [&](const WorldVoxelChunkEntry &entry)
          {
            return std::find(
                       update.newAllocationIds.begin(),
                       update.newAllocationIds.end(),
                       entry.allocation.id) !=
                update.newAllocationIds.end();
          }),
      cache.entries.end());
  update.refinementRollbackEntries.insert(
      update.refinementRollbackEntries.end(),
      std::make_move_iterator(update.evictedRollbackEntries.begin()),
      std::make_move_iterator(update.evictedRollbackEntries.end()));
  update.evictedRollbackEntries.clear();
  cache.payloadCache.erase(
      std::remove_if(
          cache.payloadCache.begin(), cache.payloadCache.end(),
          [&](const WorldVoxelChunkCachedPayload &payload)
          {
            return std::find(
                       update.addedPayloadLastUses.begin(),
                       update.addedPayloadLastUses.end(), payload.lastUse) !=
                update.addedPayloadLastUses.end();
          }),
      cache.payloadCache.end());
  cache.payloadCache.insert(
      cache.payloadCache.end(),
      std::make_move_iterator(
          update.removedPayloadRollbackEntries.begin()),
      std::make_move_iterator(
          update.removedPayloadRollbackEntries.end()));
  update.removedPayloadRollbackEntries.clear();
  update.addedPayloadLastUses.clear();
  for (auto &previous : update.refinementRollbackEntries)
  {
    previous.requestedLOD = previous.residentLOD;
    previous.state = previous.residentLOD == cache.settings.lodCount - 1u
        ? WorldVoxelChunkState::CoarseResident
        : WorldVoxelChunkState::Resident;
    const auto current = std::find_if(
        cache.entries.begin(), cache.entries.end(),
        [&](const WorldVoxelChunkEntry &entry)
        {
          return entry.slot == previous.slot;
        });
    if (current != cache.entries.end())
      *current = std::move(previous);
    else
      cache.entries.push_back(std::move(previous));
  }
  std::sort(
      cache.entries.begin(), cache.entries.end(),
      [](const WorldVoxelChunkEntry &left,
         const WorldVoxelChunkEntry &right)
      {
        return left.slot < right.slot;
      });
  if (update.physicalAllocatorSnapshot)
  {
    cache.freeNodeRanges = std::move(update.previousFreeNodeRanges);
    cache.freeChildRanges = std::move(update.previousFreeChildRanges);
    cache.freeRootRanges = std::move(update.previousFreeRootRanges);
    cache.freeMaterialRanges = std::move(update.previousFreeMaterialRanges);
    cache.freePaletteRanges = std::move(update.previousFreePaletteRanges);
    cache.retiredAllocations =
        std::move(update.previousRetiredAllocations);
    cache.nextAllocationId = update.previousNextAllocationId;
    cache.payloadCacheClock = update.previousPayloadCacheClock;
    cache.hasElevationOrigin = update.previousHasElevationOrigin;
    cache.elevationOriginMetres = update.previousElevationOriginMetres;
    cache.centerChunkX = update.previousCenterChunkX;
    cache.centerChunkZ = update.previousCenterChunkZ;
    cache.pendingChunkCount = update.previousPendingChunkCount;
    cache.settings = update.previousSettings;
    cache.spatialCutMetricConvergencePending =
        update.previousSpatialCutMetricConvergencePending;
    cache.stagedSpatialEntries =
        std::move(update.previousStagedSpatialEntries);
  }
  if (!update.previousSlotGenerations.empty())
  {
    cache.slotGenerations = std::move(update.previousSlotGenerations);
    cache.slotNodeCapacity = std::move(update.previousSlotNodeCapacity);
    cache.slotChildCapacity = std::move(update.previousSlotChildCapacity);
    cache.slotRootCapacity = std::move(update.previousSlotRootCapacity);
    cache.slotMaterialCapacity = std::move(update.previousSlotMaterialCapacity);
    cache.slotPaletteCapacity = std::move(update.previousSlotPaletteCapacity);
    cache.layoutRevision = update.previousLayoutRevision;
  }
  cache.payloadCacheBytes = worldVoxelCPUCacheBytes(cache);
  update.refinementRollbackEntries.clear();
  update.previousStagedSpatialEntries.clear();
  update.newAllocationIds.clear();
  update.addedPayloadLastUses.clear();
  update.physicalAllocatorSnapshot = false;
}

bool updateWorldVoxelChunkCache(
    const procedural::world::MapExportBundle &bundle,
    const double cameraWorldXMetres,
    const double cameraWorldZMetres,
    const WorldVoxelChunkStreamingSettings &settings,
    WorldVoxelChunkCache &cache,
    PlanetVoxelChunkSet &chunks,
    WorldVoxelChunkCacheUpdate *update,
    std::string *error,
    const PlanetVoxelChunkProgress &progress,
    const std::atomic_bool *cancellation,
    const std::vector<WorldVoxelChunkRequest> *refinementRequests,
    const std::vector<WorldVoxelChunkAddress> *forcedSpatialCut,
    const std::vector<PlanetVoxelChunkRecord> *arenaResidentRecords)
{
  // The dense arena mirror runs to gigabytes once the terrain budget does, and
  // allocating plus zeroing a fresh one per transaction measured 600-1200 ms -
  // more than every other stage of this update combined. The caller hands back
  // a retired arena to write over. Reusing it is sound because every resident
  // page's payload is rewritten into its own range below and every range is
  // defined over its whole capacity, so nothing the GPU can address survives
  // from the previous tenant.
  SparseVoxelBrickHierarchy recycledArena;
  recycledArena.nodes = std::move(chunks.bricks.nodes);
  recycledArena.occupancy = std::move(chunks.bricks.occupancy);
  recycledArena.childIndices = std::move(chunks.bricks.childIndices);
  recycledArena.rootIndices = std::move(chunks.bricks.rootIndices);
  recycledArena.materialIndices = std::move(chunks.bricks.materialIndices);
  recycledArena.microSurfaceData = std::move(chunks.bricks.microSurfaceData);
  recycledArena.palette = std::move(chunks.bricks.palette);
  recycledArena.paletteDescriptorIds =
      std::move(chunks.bricks.paletteDescriptorIds);
  chunks = {};
  chunks.bricks.nodes = std::move(recycledArena.nodes);
  chunks.bricks.occupancy = std::move(recycledArena.occupancy);
  chunks.bricks.childIndices = std::move(recycledArena.childIndices);
  chunks.bricks.rootIndices = std::move(recycledArena.rootIndices);
  chunks.bricks.materialIndices = std::move(recycledArena.materialIndices);
  chunks.bricks.microSurfaceData = std::move(recycledArena.microSurfaceData);
  chunks.bricks.palette = std::move(recycledArena.palette);
  chunks.bricks.paletteDescriptorIds =
      std::move(recycledArena.paletteDescriptorIds);
  if (update != nullptr)
    *update = {};
  if (error != nullptr)
    error->clear();
  if (cancellationRequested(cancellation))
    return cancelled(error);

  // Stage checkpoints for SVR_TRACE_TERRAIN. The cut update is one long
  // function and the page workgroup turned out to be a small part of it.
  const bool traceStages = std::getenv("SVR_TRACE_TERRAIN") != nullptr;
  auto stageMark = std::chrono::steady_clock::now();
  const auto stage = [&](const char *name)
  {
    if (!traceStages)
      return;
    const auto now = std::chrono::steady_clock::now();
    os::Logger::logf(
        "World terrain stage: %s=%.1fms", name,
        std::chrono::duration<double, std::milli>(now - stageMark).count());
    stageMark = now;
  };
  const double derivedEdge = settings.finestVoxelSizeMetres *
      static_cast<double>(settings.finestHorizontalVoxelCount);
  const double chunkEdge = settings.chunkEdgeMetres > 0.0
      ? settings.chunkEdgeMetres
      : derivedEdge;
  const bool spatialLeafCut = settings.renderDistanceMetres > 0.0;
  if (!(bundle.extentMetres > 0.0) || !std::isfinite(bundle.extentMetres) ||
      !std::isfinite(cameraWorldXMetres) ||
      !std::isfinite(cameraWorldZMetres) ||
      !(settings.finestVoxelSizeMetres > 0.0) ||
      !std::isfinite(settings.finestVoxelSizeMetres) ||
      !(chunkEdge > 0.0) || !std::isfinite(chunkEdge) ||
      settings.finestHorizontalVoxelCount < kSparseVoxelBrickSide ||
      settings.finestHorizontalVoxelCount % kSparseVoxelBrickSide != 0u ||
      (!spatialLeafCut &&
       settings.residentRadius > kWorldVoxelMaximumResidentRadius) ||
      (spatialLeafCut &&
       (!(settings.projectionScalePixels > 0.0) ||
        !std::isfinite(settings.projectionScalePixels) ||
        settings.maximumSpatialLOD >= kPlanetVoxelMaximumClipmapLevels ||
        settings.maximumResidentChunks == 0u)) ||
      settings.lodCount == 0u ||
      settings.lodCount > kPlanetVoxelMaximumLODLevels ||
      !(settings.splitErrorPixels > settings.mergeErrorPixels) ||
      !(settings.mergeErrorPixels > 0.0f) ||
      !std::isfinite(settings.splitErrorPixels) ||
      !std::isfinite(settings.mergeErrorPixels) ||
      (spatialLeafCut && settings.feedbackDrivenPageStreaming &&
       settings.splitErrorPixels + 1.0e-6f <
           settings.mergeErrorPixels * 2.0f) ||
      !(settings.distanceErrorGrowthPerKilometre >= 0.0f) ||
      !std::isfinite(settings.distanceErrorGrowthPerKilometre) ||
      !(settings.distanceErrorExponent > 0.0f) ||
      !std::isfinite(settings.distanceErrorExponent) ||
      !(settings.virtualArenaScale >= 1.0f) ||
      !std::isfinite(settings.virtualArenaScale) ||
      chunkEdge > bundle.extentMetres)
    return fail(error, "Generated-world progressive chunk settings are invalid");
  const double edgeTolerance = std::max(1.0, chunkEdge) * 1.0e-9;
  if (std::abs(chunkEdge - derivedEdge) > edgeTolerance)
    return fail(
        error,
        "The fixed chunk edge must equal the finest voxel size times the "
        "finest horizontal resolution");
  // Spatial leaves already encode their projected error in their footprint.
  // A second internal coarse LOD would compound the error and create the
  // terraced strips seen in the previous fixed-window implementation.
  const uint32_t coarsestLOD = spatialLeafCut ? 0u : settings.lodCount - 1u;
  const uint32_t coarseScale = uint32_t{1u} << coarsestLOD;
  if (settings.finestHorizontalVoxelCount % coarseScale != 0u)
    return fail(error, "The coarsest generated-world LOD does not divide the finest resolution");
  const uint32_t coarseResolution =
      settings.finestHorizontalVoxelCount / coarseScale;
  if (coarseResolution < kSparseVoxelBrickSide ||
      coarseResolution % kSparseVoxelBrickSide != 0u)
    return fail(error, "The coarsest generated-world LOD is not brick aligned");

  const uint32_t gridSide = settings.residentRadius * 2u + 1u;
  const uint32_t fixedGridChunkCount = gridSide * gridSide;
  if (!spatialLeafCut && settings.maximumResidentChunks != 0u &&
      fixedGridChunkCount > settings.maximumResidentChunks)
    return fail(error, "The fixed generated-world grid exceeds its resident chunk budget");

  const uint64_t bundleIdentity = bundle.bundleSignature != 0u
      ? bundle.bundleSignature
      : (bundle.seed != 0u ? bundle.seed : 1u);
  const bool spatialCutMetricChanged = spatialLeafCut &&
      cache.bundleSignature == bundleIdentity && !cache.entries.empty() &&
      (cache.settings.splitErrorPixels != settings.splitErrorPixels ||
       cache.settings.mergeErrorPixels != settings.mergeErrorPixels ||
       cache.settings.distanceErrorGrowthPerKilometre !=
           settings.distanceErrorGrowthPerKilometre ||
       cache.settings.distanceErrorExponent !=
           settings.distanceErrorExponent);
  const bool spatialCutMetricConvergence =
      spatialCutMetricChanged || cache.spatialCutMetricConvergencePending;
  const auto compatibleSettings = [](const WorldVoxelChunkStreamingSettings &left,
                                      const WorldVoxelChunkStreamingSettings &right)
  {
    return left.chunkEdgeMetres == right.chunkEdgeMetres &&
        left.finestVoxelSizeMetres == right.finestVoxelSizeMetres &&
        left.finestHorizontalVoxelCount == right.finestHorizontalVoxelCount &&
        left.residentRadius == right.residentRadius &&
        left.lodCount == right.lodCount &&
        left.renderDistanceMetres == right.renderDistanceMetres &&
        left.projectionScalePixels == right.projectionScalePixels &&
        left.maximumSpatialLOD == right.maximumSpatialLOD &&
        left.maximumResidentChunks == right.maximumResidentChunks &&
        left.feedbackDrivenPageStreaming ==
            right.feedbackDrivenPageStreaming &&
        left.feedbackDrivenFallbackConvergence ==
            right.feedbackDrivenFallbackConvergence &&
        left.rasterSafeCoarseTerrain == right.rasterSafeCoarseTerrain &&
        left.virtualArenaScale == right.virtualArenaScale &&
        left.generatorRevision == right.generatorRevision &&
        left.voxel.foundationDepthVoxels == right.voxel.foundationDepthVoxels &&
        left.voxel.surfaceShellDepthVoxels == right.voxel.surfaceShellDepthVoxels &&
        left.voxel.maximumVerticalVoxels == right.voxel.maximumVerticalVoxels &&
        left.voxel.surfaceEnvelopeRadius == right.voxel.surfaceEnvelopeRadius &&
        left.voxel.maximumBricks == right.voxel.maximumBricks &&
        left.voxel.proceduralDetailStrength == right.voxel.proceduralDetailStrength &&
        left.voxel.proceduralTerraceStrength == right.voxel.proceduralTerraceStrength &&
        left.voxel.proceduralTerraceHeightMetres == right.voxel.proceduralTerraceHeightMetres &&
        left.voxel.proceduralTerraceTransitionFraction == right.voxel.proceduralTerraceTransitionFraction &&
        left.voxel.microGeometry == right.voxel.microGeometry &&
        left.voxel.microMaterialRevision == right.voxel.microMaterialRevision;
  };
  if (cache.bundleSignature != bundleIdentity ||
      !compatibleSettings(cache.settings, settings))
  {
    // The compressed store survives a logical reset. Its keys already fold in
    // the bundle and every generator setting, so pages built under the old
    // configuration are unreachable rather than wrong - and pages that are
    // still reachable would otherwise be regenerated for nothing. Resetting it
    // here would also delete and recreate the owned temporary directory.
    auto retainedTransientPages = std::move(cache.transientPages);
    cache = {};
    cache.transientPages = std::move(retainedTransientPages);
  }
  const auto transactionSlotGenerations = cache.slotGenerations;
  if (update != nullptr && cache.virtualArenaInitialized)
  {
    update->physicalAllocatorSnapshot = true;
    update->previousFreeNodeRanges = cache.freeNodeRanges;
    update->previousFreeChildRanges = cache.freeChildRanges;
    update->previousFreeRootRanges = cache.freeRootRanges;
    update->previousFreeMaterialRanges = cache.freeMaterialRanges;
    update->previousFreePaletteRanges = cache.freePaletteRanges;
    update->previousRetiredAllocations = cache.retiredAllocations;
    update->previousNextAllocationId = cache.nextAllocationId;
    update->previousPayloadCacheClock = cache.payloadCacheClock;
    update->previousHasElevationOrigin = cache.hasElevationOrigin;
    update->previousElevationOriginMetres = cache.elevationOriginMetres;
    update->previousCenterChunkX = cache.centerChunkX;
    update->previousCenterChunkZ = cache.centerChunkZ;
    update->previousPendingChunkCount = cache.pendingChunkCount;
    update->previousSettings = cache.settings;
    update->previousSpatialCutMetricConvergencePending =
        cache.spatialCutMetricConvergencePending;
    update->previousStagedSpatialEntries = cache.stagedSpatialEntries;
    update->previousSlotGenerations = cache.slotGenerations;
    update->previousSlotNodeCapacity = cache.slotNodeCapacity;
    update->previousSlotChildCapacity = cache.slotChildCapacity;
    update->previousSlotRootCapacity = cache.slotRootCapacity;
    update->previousSlotMaterialCapacity = cache.slotMaterialCapacity;
    update->previousSlotPaletteCapacity = cache.slotPaletteCapacity;
    update->previousLayoutRevision = cache.layoutRevision;
  }
  reclaimWorldPhysicalAllocations(cache, settings.readerFrame);
  if (cache.virtualArenaInitialized && settings.maximumGPUPageBytes != 0u &&
      worldVoxelGPUPageBytes(cache) > settings.maximumGPUPageBytes)
    return fail(error, "Generated-world GPU page arena exceeds its budget");

  // The compressed store is a cache, so a failure to create it must not fail
  // the transaction: the generator is still the source of truth and every miss
  // is simply a page that gets built again.
  const bool transientStoreRequested =
      settings.maximumTransientMemoryBytes != 0u ||
      settings.maximumTransientDiskBytes != 0u;
  if (transientStoreRequested && !cache.transientPages)
    cache.transientPages = std::make_shared<TransientSparseVoxelPageStore>();
  if (transientStoreRequested && !cache.transientPages->initialized())
  {
    TransientSparseVoxelPageStoreSettings transientSettings{};
    transientSettings.maximumMemoryBytes = settings.maximumTransientMemoryBytes;
    transientSettings.maximumDiskBytes = settings.maximumTransientDiskBytes;
    std::string transientInitialisationError;
    if (!cache.transientPages->initialize(
            transientSettings, &transientInitialisationError))
      cache.transientPages.reset();
  }
  const bool transientStoreActive =
      cache.transientPages && cache.transientPages->initialized();
  uint32_t restoredTransientMemoryCount = 0u;
  uint32_t restoredTransientDiskCount = 0u;
  uint32_t spilledTransientCount = 0u;
  uint32_t storedTransientCount = 0u;

  // Cache entries are deliberately moved into the next cut and rollback
  // storage later in this transaction. Snapshot the small immutable identity
  // fields before any such move so expected-side publication rewrites never
  // observe a moved-from PlanetVoxelChunkSet.
  struct PreviousPageIdentitySnapshot
  {
    WorldVoxelChunkAddress address{};
    uint32_t slot = 0xffffffffu;
    uint32_t generation = 0u;
    uint32_t residentLOD = 0u;
    uint64_t rasterAllocation = 0u;
    uint64_t sourceTree64Revision = 0u;
  };
  std::vector<PreviousPageIdentitySnapshot> previousPageIdentities;
  previousPageIdentities.reserve(cache.entries.size());
  for (const auto &entry : cache.entries)
    previousPageIdentities.push_back({
      entry.address, entry.slot, entry.generation,
      std::min(entry.address.spatialLOD + entry.residentLOD, 31u),
      entry.allocation.id,
      entry.chunk->chunks.empty()
          ? 0u : entry.chunk->chunks.front().tree64Revision});
  // GPU-owned staged pages reserve only metadata and arena ranges. Remember
  // them so that, once a complete group advances, its pages still emit GPU
  // build packets even though their allocations came from an earlier update.
  std::vector<uint64_t> previouslyStagedAllocationIds;
  previouslyStagedAllocationIds.reserve(cache.stagedSpatialEntries.size());
  for (const auto &entry : cache.stagedSpatialEntries)
    if (entry.allocation.id != 0u)
      previouslyStagedAllocationIds.push_back(entry.allocation.id);
  const std::vector<WorldVoxelChunkRequest> acceptedRequests =
      refinementRequests != nullptr
      ? coalesceWorldVoxelChunkRequests(cache, *refinementRequests)
      : std::vector<WorldVoxelChunkRequest>{};

  const double mapX = std::clamp(
      cameraWorldXMetres + bundle.extentMetres * 0.5,
      0.0, std::nextafter(bundle.extentMetres, 0.0));
  const double mapZ = std::clamp(
      cameraWorldZMetres + bundle.extentMetres * 0.5,
      0.0, std::nextafter(bundle.extentMetres, 0.0));
  const int32_t requestedCenterChunkX =
      static_cast<int32_t>(std::floor(mapX / chunkEdge));
  const int32_t requestedCenterChunkZ =
      static_cast<int32_t>(std::floor(mapZ / chunkEdge));
  const bool streamingCenterChanged = !cache.entries.empty() &&
      (requestedCenterChunkX != cache.centerChunkX ||
       requestedCenterChunkZ != cache.centerChunkZ);
  cache.centerChunkX = requestedCenterChunkX;
  cache.centerChunkZ = requestedCenterChunkZ;

  const bool bootstrapSpatialCut = spatialLeafCut && cache.entries.empty() &&
      cache.stagedSpatialEntries.empty();
  const bool feedbackDrivenSpatialCut =
      spatialLeafCut && settings.feedbackDrivenPageStreaming;
  // Bootstrap, camera motion, and an authored error-curve change already have
  // a deterministic CPU target cut. Camera motion must rebuild that target
  // even in feedback mode: once the camera leaves the published footprint no
  // visible page can emit feedback for the missing area. The existing staged
  // ownership transition keeps the old complete cut visible until entering
  // pages are ready.
  // An explicit measured-error request is the highest-priority source of a
  // feedback-driven cut update. The pending count may still describe a much
  // broader CPU target left by bootstrap or camera motion; allowing that count
  // to override acceptedRequests caused nearby visible owners to remain coarse
  // while unrelated terrain was generated first.
  const bool explicitFeedbackRequest =
      feedbackDrivenSpatialCut && !acceptedRequests.empty();
  bool cameraNeedsNewCoverage = streamingCenterChanged;
  if (streamingCenterChanged && explicitFeedbackRequest)
  {
    // Crossing a fine-grid cell does not necessarily change world coverage.
    // Keep measured near-field refinement ahead of a broad metric recut while
    // the published partition covers every root needed at the new position.
    // Once new ground enters the render radius, the root coverage update wins.
    auto coverageSettings = settings;
    coverageSettings.splitErrorPixels = std::numeric_limits<float>::max();
    const auto requiredRoots = buildWorldVoxelChunkLeafCut(
        bundle.extentMetres, cameraWorldXMetres, cameraWorldZMetres, coverageSettings);
    std::vector<WorldVoxelChunkAddress> publishedCoverage;
    for (const auto &entry : cache.entries) publishedCoverage.push_back(entry.address);
    cameraNeedsNewCoverage = std::any_of(requiredRoots.begin(), requiredRoots.end(),
        [&](const auto &root) { return !worldVoxelCutCoversPage(publishedCoverage, root); });
  }
  const bool cpuDrivenSpatialConvergence = spatialLeafCut &&
      (!feedbackDrivenSpatialCut || bootstrapSpatialCut ||
       cameraNeedsNewCoverage ||
       (!explicitFeedbackRequest &&
        (spatialCutMetricConvergence ||
         (settings.feedbackDrivenFallbackConvergence && cache.pendingChunkCount != 0u))));
  const bool requestDrivenFeedbackCut = feedbackDrivenSpatialCut &&
      !cpuDrivenSpatialConvergence && !cache.entries.empty();
  std::vector<WorldVoxelChunkAddress> desired;
  std::vector<WorldVoxelChunkAddress> requestedAfterBootstrap;
  std::vector<WorldVoxelChunkAddress> stableSpatialCut;
  if (spatialLeafCut && !bootstrapSpatialCut &&
      !spatialCutMetricConvergence)
  {
    stableSpatialCut.reserve(cache.entries.size());
    for (const auto &entry : cache.entries)
      stableSpatialCut.push_back(entry.address);
  }
  const uint32_t spatialAdmissionCapacity = settings.maximumActiveSpatialPages == 0u
      ? settings.maximumResidentChunks
      : std::min(settings.maximumResidentChunks, settings.maximumActiveSpatialPages);
  const bool useForcedSpatialCut = spatialLeafCut &&
      forcedSpatialCut != nullptr && !forcedSpatialCut->empty();
  if (useForcedSpatialCut)
  {
    // The caller already holds a valid partition and wants it reproduced
    // exactly - a render-graph rebuild re-emitting the resident cut into fresh
    // arenas. Deriving a cut from the camera here would silently replace it
    // with the coarse bootstrap cut.
    desired.reserve(forcedSpatialCut->size());
    for (const auto &address : *forcedSpatialCut)
      if (worldVoxelPageInsideWorld(address, chunkEdge, bundle.extentMetres))
        desired.push_back(address);
  }
  else if (spatialLeafCut)
  {
    if (requestDrivenFeedbackCut)
    {
      desired.reserve(
          cache.entries.size() + cache.stagedSpatialEntries.size() +
          acceptedRequests.size() * 3u);
      for (const auto &entry : cache.entries)
        desired.push_back(entry.address);
      const auto removeOverlapping =
          [&](const WorldVoxelChunkAddress &address)
      {
        desired.erase(
            std::remove_if(
                desired.begin(), desired.end(),
                [&](const WorldVoxelChunkAddress &candidate)
                {
                  return worldVoxelPagesOverlap(
                      candidate, address, chunkEdge);
                }),
            desired.end());
      };
      const auto addUnique = [&](const WorldVoxelChunkAddress &address)
      {
        if (!worldVoxelPageInsideWorld(
                address, chunkEdge, bundle.extentMetres))
          return;
        if (std::find(desired.begin(), desired.end(), address) ==
            desired.end())
          desired.push_back(address);
      };
      // A staged page owns a built payload but is only a *candidate* owner:
      // the four-child group it belongs to may still be generating. Removing
      // every incumbent that overlaps it and adding only that one page deleted
      // the parent's other three quarters from the cut, and that ground then
      // had no owner at all - a page-shaped hole that persists until the
      // siblings finish. Apply a staged refinement only once the whole group
      // can replace the parent in one step; a staged page that merely absorbs
      // finer incumbents always covers what it removes and is safe.
      for (const auto &staged : cache.stagedSpatialEntries)
      {
        const auto address = staged.address;
        const auto incumbent = std::find_if(
            desired.begin(), desired.end(),
            [&](const WorldVoxelChunkAddress &candidate)
            { return worldVoxelPageAncestorOf(candidate, address); });
        if (incumbent == desired.end())
        {
          // Nothing coarser owns this ground: the staged page either replaces
          // finer incumbents or is new coverage. Either way it covers what it
          // removes.
          removeOverlapping(address);
          addUnique(address);
          continue;
        }
        // A staged page is one quarter of an ownership group. Retiring its
        // parent and adding only the staged quarter left the other three
        // quarters with no owner at all - the page-shaped hole. Replace the
        // parent with its complete four-child group instead, so the cut stays
        // a partition and the siblings that are still missing keep being
        // requested. Publication is what waits for them: the availability
        // clamp below collapses the group back onto this parent until all four
        // can actually be drawn.
        const WorldVoxelChunkAddress parent = worldVoxelPageParent(address);
        // Only a single-level advance can be completed by one sibling group; a
        // deeper incumbent waits for the intermediate levels to be requested.
        if (!(*incumbent == parent))
          continue;
        removeOverlapping(parent);
        for (int32_t childZ = 0; childZ != 2; ++childZ)
          for (int32_t childX = 0; childX != 2; ++childX)
            addUnique({
              parent.x * 2 + childX, parent.z * 2 + childZ,
              address.spatialLOD});
      }
      for (const auto &request : acceptedRequests)
      {
        const auto resident = std::find_if(
            cache.entries.begin(), cache.entries.end(),
            [&](const WorldVoxelChunkEntry &entry)
            {
              return entry.slot == request.slot &&
                  entry.generation == request.expectedGeneration;
            });
        if (resident == cache.entries.end())
          continue;
        const uint32_t currentLOD =
            std::min(resident->address.spatialLOD + resident->residentLOD, 31u);
        if (request.desiredLOD < currentLOD)
        {
          if (resident->address.spatialLOD == 0u)
            continue;
          removeOverlapping(resident->address);
          const uint32_t childLOD = resident->address.spatialLOD - 1u;
          for (int32_t childZ = 0; childZ != 2; ++childZ)
            for (int32_t childX = 0; childX != 2; ++childX)
            {
              const int64_t x =
                  static_cast<int64_t>(resident->address.x) * 2 + childX;
              const int64_t z =
                  static_cast<int64_t>(resident->address.z) * 2 + childZ;
              if (x < std::numeric_limits<int32_t>::min() ||
                  x > std::numeric_limits<int32_t>::max() ||
                  z < std::numeric_limits<int32_t>::min() ||
                  z > std::numeric_limits<int32_t>::max())
                continue;
              addUnique({
                static_cast<int32_t>(x),
                static_cast<int32_t>(z),
                childLOD});
            }
        }
        else if (request.desiredLOD > currentLOD)
        {
          const uint32_t spatialRootLOD =
              worldVoxelPageRootLOD(chunkEdge, settings);
          if (resident->address.spatialLOD >= spatialRootLOD)
            continue;
          const WorldVoxelChunkAddress parent =
              worldVoxelPageParent(resident->address);
          removeOverlapping(parent);
          addUnique(parent);
        }
      }
    }
    else
    {
      // Never expose a priority-ordered prefix of the requested leaf cut. On a
      // fresh cache, synchronously establish the small complete root cut first;
      // later calls refine it through the existing atomic staged transition.
      // This is the geometric fallback that guarantees every point in render
      // distance retains an owner while asynchronous generation is bounded.
      WorldVoxelChunkStreamingSettings cutSettings = settings;
      if (bootstrapSpatialCut)
        cutSettings.splitErrorPixels =
            std::numeric_limits<float>::max();
      // Measured page error is recorded but deliberately not fed to the cut
      // here; see the Stage 4b note in the brick-cluster LOD plan. On generated
      // relief it moved the resident cut by three percent, and paying for that
      // with a cut that depends on which pages happen to have been built -
      // rather than on camera and settings alone - is a bad trade.
      desired = buildWorldVoxelChunkLeafCut(
          bundle.extentMetres, cameraWorldXMetres, cameraWorldZMetres,
          cutSettings, nullptr,
          stableSpatialCut.empty() ? nullptr : &stableSpatialCut);
      // The screen-error cut is not bounded by the resident budget: a low
      // enough error threshold asks for more pages than there are slots. That
      // used to fail the whole transaction - "the generated-world spatial leaf
      // cut exceeds its resident chunk budget" - which rejects it again on
      // every subsequent update from the same viewpoint, so the cut can never
      // advance again. The budget is a property of the machine and the error
      // threshold is the knob that trades detail for pages, so coarsen the
      // threshold until the cut fits rather than refusing to publish one.
      // Rebuilding is cheap next to generating a page, and every rebuild is a
      // balanced partition by construction, which a local merge would not be.
      if (settings.maximumResidentChunks != 0u &&
          desired.size() > settings.maximumResidentChunks)
      {
        const float requestedSplitErrorPixels = cutSettings.splitErrorPixels;
        for (uint32_t attempt = 0u;
             attempt < 16u && desired.size() > settings.maximumResidentChunks;
             ++attempt)
        {
          if (!std::isfinite(cutSettings.splitErrorPixels))
            break;
          cutSettings.splitErrorPixels *= 1.5f;
          cutSettings.mergeErrorPixels =
              std::max(cutSettings.mergeErrorPixels,
                  cutSettings.splitErrorPixels);
          desired = buildWorldVoxelChunkLeafCut(
              bundle.extentMetres, cameraWorldXMetres, cameraWorldZMetres,
              cutSettings, nullptr,
              stableSpatialCut.empty() ? nullptr : &stableSpatialCut);
        }
        os::Logger::logf(
            "Generated-world cut coarsened to fit its resident budget: "
            "splitError=%.2fpx -> %.2fpx pages=%zu budget=%u",
            static_cast<double>(requestedSplitErrorPixels),
            static_cast<double>(cutSettings.splitErrorPixels),
            desired.size(), settings.maximumResidentChunks);
      }
      if (bootstrapSpatialCut)
        requestedAfterBootstrap = buildWorldVoxelChunkLeafCut(
            bundle.extentMetres, cameraWorldXMetres, cameraWorldZMetres,
            settings);
    }
  }
  else
  {
    desired.reserve(fixedGridChunkCount);
    const int32_t radius = static_cast<int32_t>(settings.residentRadius);
    for (int32_t offsetZ = -radius; offsetZ <= radius; ++offsetZ)
      for (int32_t offsetX = -radius; offsetX <= radius; ++offsetX)
      {
        const WorldVoxelChunkAddress address{
          cache.centerChunkX + offsetX,
          cache.centerChunkZ + offsetZ};
        if (address.x < 0 || address.z < 0)
          continue;
        const double minimumX = static_cast<double>(address.x) * chunkEdge;
        const double minimumZ = static_cast<double>(address.z) * chunkEdge;
        if (minimumX + chunkEdge > bundle.extentMetres + 1.0e-6 ||
            minimumZ + chunkEdge > bundle.extentMetres + 1.0e-6)
          continue;
        desired.push_back(address);
      }
  }
  // Publication enforces 2:1 neighbours. Feedback must request that same
  // closure, including offscreen neighbours that cannot emit raster demand;
  // otherwise ready children remain staged indefinitely behind their parent.
  if (requestDrivenFeedbackCut && !useForcedSpatialCut)
    desired = balanceWorldVoxelCut(std::move(desired), chunkEdge);
  if (desired.empty())
    return fail(error, "Generated-world progressive cache resolved no chunks");
  if (std::getenv("SVR_TRACE_TERRAIN") != nullptr)
    os::Logger::logf(
        "SVR cut source: desired=%zu requestDriven=%u cpuConvergence=%u "
        "bootstrap=%u centerChanged=%u explicitRequest=%u accepted=%zu "
        "pending=%u metricConvergence=%u staged=%zu entries=%zu",
        desired.size(), requestDrivenFeedbackCut ? 1u : 0u,
        cpuDrivenSpatialConvergence ? 1u : 0u, bootstrapSpatialCut ? 1u : 0u,
        streamingCenterChanged ? 1u : 0u, explicitFeedbackRequest ? 1u : 0u,
        acceptedRequests.size(), cache.pendingChunkCount,
        spatialCutMetricConvergence ? 1u : 0u,
        cache.stagedSpatialEntries.size(), cache.entries.size());
  const bool traceCutCoverage =
      spatialLeafCut && std::getenv("SVR_TRACE_TERRAIN_CUT") != nullptr;
  const double cutTraceRadius = settings.renderDistanceMetres * 0.85;
  const double builtCutGap = traceCutCoverage
      ? worldVoxelCutUncoveredFraction(
            desired, mapX, mapZ, cutTraceRadius,
            chunkEdge)
      : 0.0;
  // Advance one level per convergence transaction. Each published page then
  // owns one independently generated four-child replacement group. This keeps
  // generation inside the page budget and allows completed groups to become
  // visible without waiting for a 4^N descendant batch.
  //
  // This deliberately sits after the leaf-cut/fixed-grid choice above rather
  // than between its branches: an `if` placed there captures the fixed-grid
  // `else`, and the grid is then appended to a leaf cut that already covers the
  // world.
  std::vector<WorldVoxelChunkAddress> fullyRequested = desired;
  // GPU feedback intentionally advances only the observed owner.  Keep the
  // complete screen-error cut alongside that one-owner transaction, however,
  // so the scheduler can resume metadata-driven convergence if feedback goes
  // quiet.  A visible owner can temporarily fail to report (near-plane clip,
  // occlusion history, publication age, or a recycled feedback generation),
  // and treating that silence as "the cut is complete" permanently stranded
  // coarse pages after the last queued request was consumed.
  //
  // This does not displace fresh feedback: accepted requests still select the
  // requestDrivenFeedbackCut above, and the next job takes them before looking
  // at pendingChunkCount.  It is only the empty-queue fallback target.
  std::vector<WorldVoxelChunkAddress> feedbackFallbackRequested;
  if (requestDrivenFeedbackCut &&
      settings.feedbackDrivenFallbackConvergence)
    feedbackFallbackRequested = buildWorldVoxelChunkLeafCut(
        bundle.extentMetres, cameraWorldXMetres, cameraWorldZMetres,
        settings, nullptr,
        stableSpatialCut.empty() ? nullptr : &stableSpatialCut);
  if (spatialLeafCut && !bootstrapSpatialCut && !cache.entries.empty())
  {
    std::vector<WorldVoxelChunkAddress> publishedAddresses;
    publishedAddresses.reserve(cache.entries.size());
    for (const auto &entry : cache.entries)
      publishedAddresses.push_back(entry.address);
    // One spatial level per publication. Refining a page several levels at once
    // was measured at three budgets and only pays when the budget can plan the
    // entire cut, which is not a setting the atomic publication path can carry
    // (see the note on terrainPagesPerUpdate). Frames resident at spatial
    // LOD <= 2 over 1800 frames of travel with rotation:
    //
    //   budget  16: one level 482, two levels 226
    //   budget  64: one level 1107, unbounded levels 183
    //   budget 512: one level 817(*), unbounded levels 1440
    //
    //   (*) 512 with one level was measured at the 32-page transaction shape.
    //
    // The reason is budget, not depth: a two-level advance replaces one parent
    // with sixteen grandchildren, so below saturation it spends the whole
    // budget improving a single chunk instead of advancing four of them.
    // Throughput per published page is what the cut converges on.
    constexpr uint32_t kSpatialLevelsPerPublication = 1u;
    desired = clampWorldVoxelCutToRefinementLevels(
        publishedAddresses, desired, kSpatialLevelsPerPublication);
  }
  const double clampedCutGap = traceCutCoverage
      ? worldVoxelCutUncoveredFraction(
            desired, mapX, mapZ, cutTraceRadius,
            chunkEdge)
      : 0.0;
  const uint32_t slotCapacity = spatialLeafCut
      ? settings.maximumResidentChunks
      : fixedGridChunkCount;
  const double viewLength = std::hypot(
      settings.priorityViewDirectionX,
      settings.priorityViewDirectionZ);
  const double viewX = viewLength > 1.0e-12
      ? settings.priorityViewDirectionX / viewLength : 0.0;
  const double viewZ = viewLength > 1.0e-12
      ? settings.priorityViewDirectionZ / viewLength : 0.0;
  const double visibilityCosine = std::clamp(
      settings.priorityViewCosine, -1.0, 1.0);
  const auto addressDistanceSquared = [&](const WorldVoxelChunkAddress &address)
  {
    const double edge = std::ldexp(chunkEdge, address.spatialLOD);
    const double minimumX = static_cast<double>(address.x) * edge;
    const double minimumZ = static_cast<double>(address.z) * edge;
    const double deltaX = mapX < minimumX ? minimumX - mapX
        : mapX > minimumX + edge ? mapX - (minimumX + edge) : 0.0;
    const double deltaZ = mapZ < minimumZ ? minimumZ - mapZ
        : mapZ > minimumZ + edge ? mapZ - (minimumZ + edge) : 0.0;
    return deltaX * deltaX + deltaZ * deltaZ;
  };
  const auto addressViewAlignment = [&](const WorldVoxelChunkAddress &address)
  {
    if (!(viewLength > 1.0e-12))
      return -1.0;
    const double edge = std::ldexp(chunkEdge, address.spatialLOD);
    const double centerX = (static_cast<double>(address.x) + 0.5) * edge;
    const double centerZ = (static_cast<double>(address.z) + 0.5) * edge;
    const double deltaX = centerX - mapX;
    const double deltaZ = centerZ - mapZ;
    const double distance = std::hypot(deltaX, deltaZ);
    return distance > 1.0e-12
        ? (deltaX * viewX + deltaZ * viewZ) / distance
        : 1.0;
  };
  const auto addressPriorityTier = [&](const WorldVoxelChunkAddress &address)
  {
    const uint32_t ring = static_cast<uint32_t>(std::floor(
        std::sqrt(addressDistanceSquared(address)) / chunkEdge));
    if (ring <= settings.priorityNearChunkRadius)
      return 0u;
    return addressViewAlignment(address) >= visibilityCosine ? 1u : 2u;
  };
  const auto addressPriorityLess = [&](const WorldVoxelChunkAddress &left,
                                       const WorldVoxelChunkAddress &right)
  {
    const uint32_t leftTier = addressPriorityTier(left);
    const uint32_t rightTier = addressPriorityTier(right);
    if (leftTier != rightTier)
      return leftTier < rightTier;
    const double leftDistance = addressDistanceSquared(left);
    const double rightDistance = addressDistanceSquared(right);
    if (leftDistance != rightDistance)
      return leftDistance < rightDistance;
    const double leftAlignment = addressViewAlignment(left);
    const double rightAlignment = addressViewAlignment(right);
    if (leftAlignment != rightAlignment)
      return leftAlignment > rightAlignment;
    return std::tie(left.spatialLOD, left.z, left.x) <
        std::tie(right.spatialLOD, right.z, right.x);
  };
  // Streaming order is the visual deficit each page is carrying: the projected
  // error of the owner drawn there now, minus the error the cut is asking for.
  //
  // Distance alone cannot express that. It ranks a near page that is already
  // almost right above a page whose installed owner is four levels too coarse,
  // so the budget is spent where it changes the image least. The deficit is
  // zero for a page that is already correct, largest for the coarsest owner
  // relative to its request, and unbounded where nothing is drawn at all.
  const auto refinementErrorDeficit = [&](
      const WorldVoxelChunkAddress &owner,
      const WorldVoxelChunkAddress &requested)
  {
    // Project the reduction in geometric error at the requested page. Using
    // each level's half-edge as its own distance made owner and child errors
    // identical for a camera inside both pages, so a nearby refinement often
    // sorted behind a distant one. A common finest-cell floor is finite while
    // preserving the expected inverse-distance screen-error ordering.
    const double metricDistance = std::max(
        std::sqrt(addressDistanceSquared(requested)),
        settings.finestVoxelSizeMetres * 0.5);
    const double ownerVoxelSize = std::ldexp(
        settings.finestVoxelSizeMetres, static_cast<int>(owner.spatialLOD));
    const double requestedVoxelSize = std::ldexp(
        settings.finestVoxelSizeMetres,
        static_cast<int>(requested.spatialLOD));
    const double errorReduction =
        std::max(ownerVoxelSize - requestedVoxelSize, 0.0);
    const double multiplier = 1.0 +
        std::max(0.0f, settings.distanceErrorGrowthPerKilometre) *
            std::pow(
                metricDistance * 0.001,
                std::clamp(
                    static_cast<double>(settings.distanceErrorExponent), 0.125,
                    8.0));
    const double scale = settings.projectionScalePixels > 0.0
        ? settings.projectionScalePixels
        : 1.0;
    return scale * errorReduction / metricDistance / multiplier;
  };
  // Only the spatial cut has an error delta to rank by. A fixed grid is one
  // level throughout, so the deficit there collapses to "resident or not",
  // which is not a detail ordering and would displace the camera-near
  // admission rank that mode documents.
  std::vector<double> desiredErrorDeficits(desired.size(), 0.0);
  std::vector<WorldVoxelChunkAddress> desiredOwnershipGroups(desired.size());
  for (size_t index = 0u; spatialLeafCut && index < desired.size(); ++index)
  {
    const auto &address = desired[index];
    desiredOwnershipGroups[index] = address;
    const bool alreadyOwned = std::any_of(
        cache.entries.begin(), cache.entries.end(),
        [&](const WorldVoxelChunkEntry &entry)
        { return entry.address == address; });
    if (alreadyOwned)
      continue;
    const auto owner = std::find_if(
        cache.entries.begin(), cache.entries.end(),
        [&](const WorldVoxelChunkEntry &entry)
        { return worldVoxelPageAncestorOf(entry.address, address); });
    if (owner != cache.entries.end())
      desiredOwnershipGroups[index] = owner->address;
    desiredErrorDeficits[index] = owner != cache.entries.end()
        ? refinementErrorDeficit(owner->address, address)
        : std::numeric_limits<double>::max();
  }
  {
    // Sort a permutation rather than the addresses, so the deficit is computed
    // once per page instead of on every comparison.
    std::vector<size_t> order(desired.size());
    for (size_t index = 0u; index < order.size(); ++index)
      order[index] = index;
    std::stable_sort(
        order.begin(), order.end(),
        [&](const size_t left, const size_t right)
        {
          if (desiredErrorDeficits[left] != desiredErrorDeficits[right])
            return desiredErrorDeficits[left] > desiredErrorDeficits[right];
          return addressPriorityLess(desired[left], desired[right]);
        });
    // The GPU publication lane switches one complete ownership group at a
    // time. Keep the four children of a refined parent contiguous after
    // ranking the group by its most important child. Otherwise a four-page
    // budget can stage one child from four different parents and publish no
    // visible progress at all.
    struct AddressLess
    {
      bool operator()(
          const WorldVoxelChunkAddress &left,
          const WorldVoxelChunkAddress &right) const
      {
        return std::tie(left.spatialLOD, left.z, left.x) <
            std::tie(right.spatialLOD, right.z, right.x);
      }
    };
    std::map<WorldVoxelChunkAddress, size_t, AddressLess> groupIndices;
    std::vector<std::vector<WorldVoxelChunkAddress>> groups;
    for (const size_t index : order)
    {
      const auto [group, inserted] = groupIndices.emplace(
          desiredOwnershipGroups[index], groups.size());
      if (inserted)
        groups.emplace_back();
      groups[group->second].push_back(desired[index]);
    }
    std::vector<WorldVoxelChunkAddress> ordered;
    ordered.reserve(desired.size());
    for (auto &group : groups)
      ordered.insert(
          ordered.end(),
          std::make_move_iterator(group.begin()),
          std::make_move_iterator(group.end()));
    desired = std::move(ordered);
  }

  if (cpuDrivenSpatialConvergence && !useForcedSpatialCut)
  {
    // Staging is speculative. A camera move can make a staged refinement
    // obsolete while its published parent still covers the ground. Release
    // those candidates before capacity admission, otherwise old incomplete
    // groups can occupy every transition slot and strand the new near field.
    for (auto staged = cache.stagedSpatialEntries.begin();
         staged != cache.stagedSpatialEntries.end();)
    {
      const bool stillRequested = std::any_of(
          fullyRequested.begin(), fullyRequested.end(),
          [&](const WorldVoxelChunkAddress &target)
          {
            return staged->address == target ||
                worldVoxelPageAncestorOf(staged->address, target);
          });
      if (stillRequested)
        ++staged;
      else
      {
        retireWorldPhysicalAllocation(cache, staged->allocation, settings);
        staged = cache.stagedSpatialEntries.erase(staged);
      }
    }
  }
  // Admission must see the same priority order as generation. The leaf-cut
  // builder emits spatial traversal order; admitting that order at a full
  // pool can exclude the camera's owner before error ranking ever sees it.
  if (spatialLeafCut && !cache.entries.empty())
  {
    std::vector<WorldVoxelChunkAddress> published, staged;
    published.reserve(cache.entries.size());
    staged.reserve(cache.stagedSpatialEntries.size());
    for (const auto &entry : cache.entries)
      published.push_back(entry.address);
    for (const auto &entry : cache.stagedSpatialEntries)
      staged.push_back(entry.address);
    uint32_t admittedGroups = 0u;
    uint32_t deferredGroups = 0u;
    desired = limitWorldVoxelCutToCapacity(
        published, desired, staged, chunkEdge,
        useForcedSpatialCut ? slotCapacity :
            std::max<uint32_t>(published.size(), spatialAdmissionCapacity),
        &admittedGroups, &deferredGroups);
    if (update != nullptr)
    {
      update->admittedCapacityGroupCount = admittedGroups;
      update->deferredCapacityGroupCount = deferredGroups;
    }
  }
  if (desired.size() > slotCapacity)
    return fail(
        error,
        "The generated-world spatial leaf cut exceeds its resident chunk "
        "budget: pages=" + std::to_string(desired.size()) + " budget=" +
        std::to_string(slotCapacity) +
        (spatialLeafCut
             ? "; the cut was already coarsened as far as the split threshold "
               "allows, so this budget is below the coarsest cut this render "
               "distance can produce and has to be raised at configuration "
               "time"
             : ""));
  if (spatialLeafCut)
  {
    for (auto staged = cache.stagedSpatialEntries.begin();
         staged != cache.stagedSpatialEntries.end();)
    {
      if (std::find(desired.begin(), desired.end(), staged->address) !=
          desired.end())
      {
        ++staged;
        continue;
      }
      // A staged page whose parent still owns the cut is a refinement that has
      // not been applied yet - either its sibling group is incomplete, or the
      // per-publication group cap deferred it. Retiring it because it is absent
      // from `desired` destroys the payload the group is waiting on, and the
      // group can then never complete: refinement livelocks at whatever LOD the
      // cut had reached. Keep it; it is dropped only once the cut stops asking
      // for that ground at all.
      const WorldVoxelChunkAddress stagedParent =
          worldVoxelPageParent(staged->address);
      if (std::find(desired.begin(), desired.end(), stagedParent) !=
          desired.end())
      {
        ++staged;
        continue;
      }
      retireWorldPhysicalAllocation(cache, staged->allocation, settings);
      staged = cache.stagedSpatialEntries.erase(staged);
    }
  }

  if (spatialLeafCut)
  {
    const std::vector<WorldVoxelChunkAddress> directoryCover =
        !requestedAfterBootstrap.empty()
        ? requestedAfterBootstrap
        : fullyRequested;
    WorldVoxelPageDirectory nextDirectory{};
    std::string directoryError;
    if (!buildWorldVoxelPageDirectory(
            bundle.extentMetres, settings, directoryCover, nextDirectory,
            &directoryError))
      return fail(error, directoryError);
    preserveWorldVoxelPageDirectoryState(cache.pageDirectory, nextDirectory);
    assignWorldVoxelPageDirectoryPayloadKeys(
        bundleIdentity, settings, coarsestLOD, nextDirectory);
    annotateWorldVoxelPageDirectory(cache, nextDirectory);
    if (!buildWorldVoxelPageDirectoryPatchLists(
            nextDirectory, &directoryError))
      return fail(error, directoryError);
    cache.pageDirectory = std::move(nextDirectory);
  }
  else
    cache.pageDirectory = {};

  WorldVoxelTerrainPageSource pageSource{
    bundle, bundleIdentity, chunkEdge, settings, cache, update,
    transientStoreActive};

  if (cache.slotCount == 0u)
  {
    cache.slotCount = slotCapacity;
    cache.slotGenerations.assign(cache.slotCount, 0u);
    cache.slotNodeCapacity.assign(cache.slotCount, 0u);
    cache.slotChildCapacity.assign(cache.slotCount, 0u);
    cache.slotRootCapacity.assign(cache.slotCount, 0u);
    cache.slotMaterialCapacity.assign(cache.slotCount, 0u);
    cache.slotPaletteCapacity.assign(cache.slotCount, 0u);
  }
  if (cache.slotCount != slotCapacity ||
      cache.slotGenerations.size() != cache.slotCount ||
      cache.slotNodeCapacity.size() != cache.slotCount ||
      cache.slotChildCapacity.size() != cache.slotCount ||
      cache.slotRootCapacity.size() != cache.slotCount ||
      cache.slotMaterialCapacity.size() != cache.slotCount ||
      cache.slotPaletteCapacity.size() != cache.slotCount)
    return fail(error, "Generated-world progressive slot table is invalid");

  const auto positiveModulo = [](const int32_t value, const uint32_t divisor)
  {
    const int64_t remainder = static_cast<int64_t>(value) % divisor;
    return static_cast<uint32_t>(remainder < 0 ? remainder + divisor : remainder);
  };
  // Slot assignment, resolved once for the whole cut.
  //
  // This used to be recomputed per page: rebuild an occupancy bitmap, then walk
  // every earlier page in priority order and, for each, scan the resident and
  // staged sets to decide whether it consumes a slot. That is O(missing x cut x
  // (resident + staged + slots)) - around 890 million operations on a converged
  // 16 km world, and the reason a late transaction took tens of seconds.
  //
  // The walk only ever reproduces "hand out free slots in priority order", so a
  // single forward pass with a cursor gives the identical answer: no slot below
  // the cursor can become free while the pass runs. Retained and staged owners
  // keep the slot they already hold, which is what makes the assignment stable
  // across the several transactions an ownership group may take to complete.
  struct AddressHash
  {
    size_t operator()(const WorldVoxelChunkAddress &address) const
    {
      uint64_t hash = 1469598103934665603ull;
      const auto mix = [&hash](const uint32_t value)
      {
        hash ^= value;
        hash *= 1099511628211ull;
      };
      mix(static_cast<uint32_t>(address.x));
      mix(static_cast<uint32_t>(address.z));
      mix(address.spatialLOD);
      return static_cast<size_t>(hash);
    }
  };
  std::unordered_map<WorldVoxelChunkAddress, uint32_t, AddressHash>
      slotByAddress;
  if (spatialLeafCut)
  {
    slotByAddress.reserve(desired.size() * 2u);
    std::unordered_map<WorldVoxelChunkAddress, uint32_t, AddressHash> heldSlots;
    heldSlots.reserve(cache.entries.size() + cache.stagedSpatialEntries.size());
    std::vector<bool> occupied(cache.slotCount, false);
    // Every published owner holds its slot, including one this cut intends to
    // replace: a group held back keeps its old owner visible, and that owner is
    // still being drawn from the slot. Staged owners hold theirs for the same
    // reason. The resident budget is sized for both cuts to coexist.
    for (const auto &entry : cache.entries)
    {
      heldSlots.emplace(entry.address, entry.slot);
      if (entry.slot < occupied.size())
        occupied[entry.slot] = true;
    }
    for (const auto &entry : cache.stagedSpatialEntries)
    {
      heldSlots.emplace(entry.address, entry.slot);
      if (entry.slot < occupied.size())
        occupied[entry.slot] = true;
    }
    uint32_t cursor = 0u;
    for (const auto &address : desired)
    {
      const auto held = heldSlots.find(address);
      if (held != heldSlots.end())
      {
        slotByAddress.emplace(address, held->second);
        continue;
      }
      while (cursor < cache.slotCount && occupied[cursor])
        ++cursor;
      if (cursor >= cache.slotCount)
      {
        slotByAddress.emplace(address, 0xffffffffu);
        continue;
      }
      occupied[cursor] = true;
      slotByAddress.emplace(address, cursor);
      ++cursor;
    }
  }
  const auto slotForAddress = [&](const WorldVoxelChunkAddress &address)
  {
    if (spatialLeafCut)
    {
      const auto assigned = slotByAddress.find(address);
      return assigned != slotByAddress.end() ? assigned->second : 0xffffffffu;
    }
    return positiveModulo(address.z, gridSide) * gridSide +
        positiveModulo(address.x, gridSide);
  };
  const auto findCached = [&](const WorldVoxelChunkAddress &address)
      -> WorldVoxelChunkEntry *
  {
    const auto found = std::find_if(
        cache.entries.begin(), cache.entries.end(),
        [&](const WorldVoxelChunkEntry &entry)
        {
          return entry.address == address;
        });
    return found == cache.entries.end() ? nullptr : &*found;
  };
  const auto findStaged = [&](const WorldVoxelChunkAddress &address)
      -> WorldVoxelChunkEntry *
  {
    const auto found = std::find_if(
        cache.stagedSpatialEntries.begin(),
        cache.stagedSpatialEntries.end(),
        [&](const WorldVoxelChunkEntry &entry)
        {
          return entry.address == address;
        });
    return found == cache.stagedSpatialEntries.end() ? nullptr : &*found;
  };
  uint32_t reusedCount = 0u;
  uint32_t missingCount = 0u;
  for (const auto &address : desired)
  {
    if (findCached(address) != nullptr)
      ++reusedCount;
    else if (findStaged(address) == nullptr)
      ++missingCount;
  }
  uint32_t effectiveGenerationBudget =
      settings.maximumGeneratedChunksPerUpdate;
  if (settings.gpuOwnedPayloads &&
      settings.maximumGPUOwnedPagesPerTransaction != 0u)
    effectiveGenerationBudget = effectiveGenerationBudget == 0u
        ? settings.maximumGPUOwnedPagesPerTransaction
        : std::min(
              effectiveGenerationBudget,
              settings.maximumGPUOwnedPagesPerTransaction);
  const uint32_t coarseGenerationBudget =
      bootstrapSpatialCut || effectiveGenerationBudget == 0u
      ? missingCount
      : std::min(missingCount, effectiveGenerationBudget);

  if (update != nullptr && refinementRequests != nullptr)
    update->rejectedStaleRequestCount = static_cast<uint32_t>(std::count_if(
        refinementRequests->begin(), refinementRequests->end(),
        [&](const WorldVoxelChunkRequest &request)
        {
          const auto resident = std::find_if(
              cache.entries.begin(), cache.entries.end(),
              [&](const WorldVoxelChunkEntry &entry)
              {
                return entry.slot == request.slot &&
                    entry.generation == request.expectedGeneration;
              });
          const uint32_t currentLOD = resident == cache.entries.end()
              ? 0u
              : (spatialLeafCut
                    ? std::min(
                          resident->address.spatialLOD + resident->residentLOD,
                          31u)
                    : resident->residentLOD);
          return resident == cache.entries.end() ||
              request.desiredLOD == currentLOD ||
              (spatialLeafCut
                    ? request.desiredLOD >
                          worldVoxelPageRootLOD(chunkEdge, settings)
                    : request.desiredLOD >= settings.lodCount);
        }));

  std::vector<WorldVoxelChunkEntry> built;
  built.reserve(coarseGenerationBudget);
  uint32_t restoredCachedCount = 0u;
  double samplingMilliseconds = 0.0;
  double shellMilliseconds = 0.0;
  double hierarchyMilliseconds = 0.0;
  uint64_t emittedShellVoxels = 0u;
  uint64_t omittedInteriorVoxels = 0u;
  uint32_t maximumUnexposedDepthVoxels = 0u;
  struct GeneratedWorldPageJob
  {
    WorldVoxelChunkAddress address{};
    uint32_t slot = 0u;
    uint32_t generation = 0u;
    PlanetVoxelChunkSet page{};
    WorldMapVoxelGenerationMetrics metrics{};
    std::string error;
    bool success = false;
  };
  std::vector<GeneratedWorldPageJob> generationJobs;
  generationJobs.reserve(coarseGenerationBudget);
  for (const auto &address : desired)
  {
    if (findCached(address) != nullptr || findStaged(address) != nullptr ||
        built.size() + generationJobs.size() >= coarseGenerationBudget)
      continue;
    if (cancellationRequested(cancellation))
      return cancelled(error);
    const uint32_t slot = slotForAddress(address);
    if (slot >= cache.slotCount)
      return fail(error, "Generated-world spatial leaf cut exhausted its slots");
    const uint32_t generation = std::max(
        cache.slotGenerations[slot] + 1u, 1u);
    PlanetVoxelChunkSet cachedPage{};
    WorldVoxelTerrainPageSourceKind cachedSource{};
    if (pageSource.loadCached(address, coarsestLOD, cachedPage, cachedSource))
    {
      cache.slotGenerations[slot] = generation;
      built.push_back({
        address, slot, generation, coarsestLOD, coarsestLOD,
        WorldVoxelChunkState::CoarseResident, std::make_shared<const PlanetVoxelChunkSet>(std::move(cachedPage))});
      ++restoredCachedCount;
      if (cachedSource == WorldVoxelTerrainPageSourceKind::TransientDisk)
        ++restoredTransientDiskCount;
      else if (cachedSource == WorldVoxelTerrainPageSourceKind::TransientMemory)
        ++restoredTransientMemoryCount;
      continue;
    }
    generationJobs.push_back({address, slot, generation});
  }

  const uint32_t requestedWorldWorkers = settings.voxel.workerCount == 0u
      ? recommendedPlanetVoxelWorkerCount(
            std::max(1u, std::thread::hardware_concurrency()))
      : std::clamp(settings.voxel.workerCount, 1u, 64u);
  const uint32_t worldWorkerCount = std::min<uint32_t>(
      requestedWorldWorkers,
      static_cast<uint32_t>(generationJobs.size()));
  // Fewer pages than workers leaves cores idle, and a four-page transaction is
  // the common case. Each page samples its own columns across the remainder.
  const uint32_t columnSamplingWorkerCount = worldWorkerCount == 0u
      ? 1u
      : std::max(1u, requestedWorldWorkers / worldWorkerCount);
  std::mutex worldProgressMutex;
  const auto generatePage = [&](const uint32_t jobIndex)
  {
    if (jobIndex >= generationJobs.size() ||
        cancellationRequested(cancellation))
      return;
    auto &job = generationJobs[jobIndex];
    const std::string label = "World chunk " +
        std::to_string(jobIndex + 1u) + "/" +
        std::to_string(generationJobs.size());
    job.success = pageSource.generate(
        job.address, coarsestLOD, coarsestLOD, job.page, job.metrics,
        &job.error,
        [&](const char *phase, const double fraction)
        {
          if (!progress)
            return;
          const std::string labelled = label + ": " +
              (phase != nullptr ? phase : "");
          std::lock_guard<std::mutex> lock(worldProgressMutex);
          progress(
              labelled.c_str(),
              (static_cast<double>(jobIndex) + fraction) /
                  std::max<size_t>(generationJobs.size(), 1u) * 0.92);
        },
        cancellation,
        columnSamplingWorkerCount);
  };
  // Everything above is cut derivation: the desired address set, the page
  // directory walk, cache and transient lookups, request validation. It runs
  // before a single page is generated and had never been timed.
  stage("cutPlan");
  const auto generateStart = std::chrono::steady_clock::now();
  if (worldWorkerCount != 0u)
  {
    try
    {
      terrainChunkBuildThreadPool().parallelFor(
          worldWorkerCount,
          static_cast<uint32_t>(generationJobs.size()),
          generatePage);
    }
    catch (const std::exception &exception)
    {
      return fail(
          error,
          std::string("Generated-world page workgroup failed: ") +
              exception.what());
    }
    catch (...)
    {
      return fail(
          error,
          "Generated-world page workgroup failed with an unknown exception");
    }
  }
  if (cancellationRequested(cancellation))
    return cancelled(error);
  for (auto &job : generationJobs)
  {
    if (!job.success)
      return fail(
          error,
          "Generated-world page workgroup failed at (" +
              std::to_string(job.address.x) + "," +
              std::to_string(job.address.z) + ", spatial LOD " +
              std::to_string(job.address.spatialLOD) + ", internal LOD " +
              std::to_string(coarsestLOD) + "): " + job.error);
    samplingMilliseconds += job.metrics.mapSamplingMilliseconds;
    shellMilliseconds += job.metrics.shellExtractionMilliseconds;
    hierarchyMilliseconds += job.metrics.hierarchyBuildMilliseconds;
    emittedShellVoxels += job.metrics.emittedShellVoxelCount;
    omittedInteriorVoxels += job.metrics.omittedInteriorVoxelCount;
    maximumUnexposedDepthVoxels = std::max(
        maximumUnexposedDepthVoxels,
        job.metrics.maximumUnexposedDepthVoxels);
    if (pageSource.storeTransient(job.address, coarsestLOD, job.page))
      ++storedTransientCount;
    cache.slotGenerations[job.slot] = job.generation;
    built.push_back({
      job.address, job.slot, job.generation, coarsestLOD, coarsestLOD,
      WorldVoxelChunkState::CoarseResident, std::make_shared<const PlanetVoxelChunkSet>(std::move(job.page))});
  }

  if (std::getenv("SVR_TRACE_TERRAIN") != nullptr)
    os::Logger::logf(
        "World terrain pages: jobs=%zu pageWorkers=%u columnWorkers=%u "
        "generate=%.1fms sampleSum=%.1fms hierarchySum=%.1fms",
        generationJobs.size(),
        worldWorkerCount,
        columnSamplingWorkerCount,
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - generateStart).count(),
        samplingMilliseconds,
        hierarchyMilliseconds);
  stage("generate");
  const uint32_t generatedThisUpdate = static_cast<uint32_t>(built.size());
  uint32_t pendingCoarseCount =
      missingCount - std::min(missingCount, generatedThisUpdate);
  uint32_t advancedGroupCount = 0u;
  uint32_t heldGroupCount = 0u;

  const bool ownershipChanged = spatialLeafCut &&
      (cache.entries.size() != desired.size() ||
       std::any_of(
           cache.entries.begin(), cache.entries.end(),
           [&](const WorldVoxelChunkEntry &entry)
           {
             return std::find(desired.begin(), desired.end(), entry.address) ==
                 desired.end();
           }));
  const bool stagedLeafCutTransition = ownershipChanged &&
      !cache.entries.empty();
  if (stagedLeafCutTransition)
  {
    cache.stagedSpatialEntries.insert(
        cache.stagedSpatialEntries.end(),
        std::make_move_iterator(built.begin()),
        std::make_move_iterator(built.end()));
    built.clear();
    if (pendingCoarseCount == 0u)
    {
      built = std::move(cache.stagedSpatialEntries);
      cache.stagedSpatialEntries.clear();
    }
    else
    {
      // Advance every ownership group whose replacement pages are all staged,
      // and keep publishing the previous owners for the rest. A group is one
      // published page and the requested pages that tile it, or one requested
      // page and the published pages it absorbs, so the cut stays a partition
      // at every step: no parent disappears before its replacements exist, and
      // no subset of them becomes visible on its own.
      //
      // Holding the entire replacement cut instead - which is what this did -
      // left the coarse bootstrap visible until the last page of a 16 km world
      // was built, and read as terrain that sits on one LOD and then jumps.
      cache.slotGenerations = transactionSlotGenerations;
      std::vector<WorldVoxelChunkAddress> publishedAddresses;
      publishedAddresses.reserve(cache.entries.size());
      for (const auto &entry : cache.entries)
        publishedAddresses.push_back(entry.address);
      std::vector<WorldVoxelChunkAddress> stagedAddresses;
      stagedAddresses.reserve(cache.stagedSpatialEntries.size());
      for (const auto &entry : cache.stagedSpatialEntries)
        stagedAddresses.push_back(entry.address);
      desired = selectPublishableWorldVoxelCut(
          publishedAddresses, desired, stagedAddresses, chunkEdge,
          &advancedGroupCount, &heldGroupCount);
      // Take the staged payloads the advanced cut now owns. The rest stay
      // staged, still invisible, and are retried next transaction.
      for (auto staged = cache.stagedSpatialEntries.begin();
           staged != cache.stagedSpatialEntries.end();)
      {
        if (std::find(desired.begin(), desired.end(), staged->address) !=
            desired.end())
        {
          built.push_back(std::move(*staged));
          staged = cache.stagedSpatialEntries.erase(staged);
        }
        else
          ++staged;
      }
    }
  }

  if (traceCutCoverage)
  {
    const double selectedCutGap = worldVoxelCutUncoveredFraction(
        desired, mapX, mapZ, cutTraceRadius,
        chunkEdge);
    uint32_t minimumCutLOD = 32u;
    uint32_t maximumCutLOD = 0u;
    for (const auto &address : desired)
    {
      minimumCutLOD = std::min(minimumCutLOD, address.spatialLOD);
      maximumCutLOD = std::max(maximumCutLOD, address.spatialLOD);
    }
    uint32_t requestedMinimumLOD = 32u;
    for (const auto &address : fullyRequested)
      requestedMinimumLOD = std::min(requestedMinimumLOD, address.spatialLOD);
    os::Logger::logf(
        "SVR terrain cut coverage: leafCut=%.4f afterClamp=%.4f "
        "afterSelect=%.4f pages=%zu published=%zu staged=%zu "
        "transition=%u pendingCoarse=%u lod=[%u,%u] requestedMin=%u "
        "feedbackCut=%u",
        builtCutGap, clampedCutGap, selectedCutGap, desired.size(),
        cache.entries.size(), cache.stagedSpatialEntries.size(),
        stagedLeafCutTransition ? 1u : 0u, pendingCoarseCount,
        minimumCutLOD, maximumCutLOD, requestedMinimumLOD,
        requestDrivenFeedbackCut ? 1u : 0u);
  }

  for (const auto &entry : built)
    if (entry.slot < cache.slotGenerations.size())
      cache.slotGenerations[entry.slot] = entry.generation;

  // Entering coarse owners always win the generation budget. LOD replacement
  // starts once the requested resident window is complete and follows the
  // separately bounded refinement batch.
  struct LODReplacementCandidate
  {
    WorldVoxelChunkEntry *resident = nullptr;
    uint32_t targetLOD = 0u;
    uint32_t priority = 0u;
  };
  std::vector<LODReplacementCandidate> refinementCandidates;
  if (missingCount == built.size())
    for (auto &entry : cache.entries)
    {
      if (std::find(desired.begin(), desired.end(), entry.address) ==
          desired.end())
        continue;
      const auto request = std::find_if(
          acceptedRequests.begin(), acceptedRequests.end(),
          [&](const WorldVoxelChunkRequest &candidate)
          {
            return candidate.slot == entry.slot &&
                candidate.expectedGeneration == entry.generation;
          });
      // Spatial feedback has already rewritten `desired` to child/parent
      // page ownership above. Treating its combined spatial LOD as an
      // internal page LOD here asks a 128-cell page for internal LOD 10 or 11
      // and fails generation. Internal replacement candidates exist only for
      // the fixed-grid mode.
      if (!spatialLeafCut && request != acceptedRequests.end())
        refinementCandidates.push_back(
            {&entry, request->desiredLOD, request->priority});
      else if (entry.requestedLOD < entry.residentLOD)
        refinementCandidates.push_back(
            {&entry, entry.residentLOD - 1u, 0u});
    }
  std::sort(
      refinementCandidates.begin(), refinementCandidates.end(),
      [&](const LODReplacementCandidate &left,
          const LODReplacementCandidate &right)
      {
        const uint32_t leftTier = addressPriorityTier(left.resident->address);
        const uint32_t rightTier = addressPriorityTier(right.resident->address);
        if (leftTier != rightTier)
          return leftTier < rightTier;
        if (left.priority != right.priority)
          return left.priority > right.priority;
        const double leftDistance =
            addressDistanceSquared(left.resident->address);
        const double rightDistance =
            addressDistanceSquared(right.resident->address);
        if (leftDistance != rightDistance)
          return leftDistance < rightDistance;
        return left.resident->slot < right.resident->slot;
      });
  const uint32_t totalBudgetRemaining =
      effectiveGenerationBudget == 0u
      ? static_cast<uint32_t>(refinementCandidates.size())
      : effectiveGenerationBudget > built.size()
          ? effectiveGenerationBudget -
                static_cast<uint32_t>(built.size())
          : 0u;
  const uint32_t refinementSpecificBudget =
      settings.maximumRefinedChunksPerUpdate == 0u
      ? static_cast<uint32_t>(refinementCandidates.size())
      : settings.maximumRefinedChunksPerUpdate;
  const uint32_t refinementBudget = std::min(
      {static_cast<uint32_t>(refinementCandidates.size()),
       totalBudgetRemaining, refinementSpecificBudget});
  std::vector<WorldVoxelChunkEntry> refined;
  refined.reserve(refinementBudget);
  uint32_t refinedCount = 0u;
  uint32_t coarsenedCount = 0u;
  for (uint32_t refinementIndex = 0u;
       refinementIndex < refinementBudget; ++refinementIndex)
  {
    if (cancellationRequested(cancellation))
      return cancelled(error);
    const auto &candidate = refinementCandidates[refinementIndex];
    const auto &resident = *candidate.resident;
    const uint32_t targetLOD = candidate.targetLOD;
    const bool coarsening = targetLOD > resident.residentLOD;
    PlanetVoxelChunkSet page;
    const std::string label = std::string(
        coarsening ? "Coarsening world chunk slot "
                   : "Refining world chunk slot ") +
        std::to_string(resident.slot) + " to LOD " +
        std::to_string(targetLOD);
    WorldVoxelTerrainPageLoad pageLoad{};
    if (!pageSource.load(
            resident.address, targetLOD, coarsestLOD, page, pageLoad, error,
            [&](const char *phase, const double fraction)
            {
              if (!progress)
                return;
              const std::string labelled = label + ": " +
                  (phase != nullptr ? phase : "");
              progress(
                  labelled.c_str(),
                  0.05 +
                      (static_cast<double>(refinementIndex) + fraction) /
                          std::max(refinementBudget, 1u) * 0.87);
            },
            cancellation))
      return false;
    if (pageLoad.source == WorldVoxelTerrainPageSourceKind::Generated)
    {
      samplingMilliseconds += pageLoad.metrics.mapSamplingMilliseconds;
      shellMilliseconds += pageLoad.metrics.shellExtractionMilliseconds;
      hierarchyMilliseconds += pageLoad.metrics.hierarchyBuildMilliseconds;
      emittedShellVoxels += pageLoad.metrics.emittedShellVoxelCount;
      omittedInteriorVoxels += pageLoad.metrics.omittedInteriorVoxelCount;
      maximumUnexposedDepthVoxels = std::max(
          maximumUnexposedDepthVoxels,
          pageLoad.metrics.maximumUnexposedDepthVoxels);
      if (pageLoad.transientStored)
        ++storedTransientCount;
    }
    else
    {
      ++restoredCachedCount;
      if (pageLoad.source == WorldVoxelTerrainPageSourceKind::TransientDisk)
        ++restoredTransientDiskCount;
      else if (
          pageLoad.source == WorldVoxelTerrainPageSourceKind::TransientMemory)
        ++restoredTransientMemoryCount;
    }
    uint32_t generation = resident.generation + 1u;
    if (generation == 0u)
      generation = 1u;
    cache.slotGenerations[resident.slot] = generation;
    refined.push_back({
      resident.address, resident.slot, generation, targetLOD, targetLOD,
      targetLOD == coarsestLOD
          ? WorldVoxelChunkState::CoarseResident
          : WorldVoxelChunkState::Resident,
      std::make_shared<const PlanetVoxelChunkSet>(std::move(page))});
    if (coarsening)
      ++coarsenedCount;
    else
      ++refinedCount;
  }
  const auto findBuilt = [&](const WorldVoxelChunkAddress &address)
      -> WorldVoxelChunkEntry *
  {
    const auto found = std::find_if(
        built.begin(), built.end(),
        [&](const WorldVoxelChunkEntry &entry)
        {
          return entry.address == address;
        });
    return found == built.end() ? nullptr : &*found;
  };
  const auto findRefined = [&](const WorldVoxelChunkAddress &address)
      -> WorldVoxelChunkEntry *
  {
    const auto found = std::find_if(
        refined.begin(), refined.end(),
        [&](const WorldVoxelChunkEntry &entry)
        {
          return entry.address == address;
        });
    return found == refined.end() ? nullptr : &*found;
  };

  // Existing allocations remain live while replacements are generated. New
  // coarse/refined payloads therefore reserve genuinely unreferenced ranges;
  // the old ranges are retired only after the complete CPU cut exists.
  stage("refinementPlan");
  std::vector<uint64_t> newAllocationIdsThisTransaction;
  if (cache.virtualArenaInitialized)
  {
    const auto freeNodeSnapshot = cache.freeNodeRanges;
    const auto freeChildSnapshot = cache.freeChildRanges;
    const auto freeRootSnapshot = cache.freeRootRanges;
    const auto freeMaterialSnapshot = cache.freeMaterialRanges;
    const auto freePaletteSnapshot = cache.freePaletteRanges;
    const uint64_t nextAllocationSnapshot = cache.nextAllocationId;
    std::string allocationFailure;
    const auto allocateEntries = [&](auto &entries)
    {
      for (auto &entry : entries)
      {
        if (entry.allocation.id != 0u)
          continue;
        if (!allocateWorldPhysicalAllocation(
                cache, *entry.chunk, entry.allocation))
        {
          const auto largest = [](const auto &ranges)
          {
            uint32_t result = 0u;
            for (const auto &range : ranges)
              result = std::max(result, range.capacity);
            return result;
          };
          allocationFailure =
              " slot=" + std::to_string(entry.slot) +
              " lod=" + std::to_string(entry.residentLOD) +
              " required(nodes=" +
              std::to_string(entry.chunk->bricks.nodes.size()) +
              ",children=" +
              std::to_string(entry.chunk->bricks.childIndices.size()) +
              ",roots=" +
              std::to_string(entry.chunk->bricks.rootIndices.size()) +
              ",materials=" +
              std::to_string(entry.chunk->bricks.materialIndices.size()) +
              ",palette=" +
              std::to_string(entry.chunk->bricks.palette.size()) +
              ") largestFree(nodes=" +
              std::to_string(largest(cache.freeNodeRanges)) +
              ",children=" +
              std::to_string(largest(cache.freeChildRanges)) +
              ",roots=" +
              std::to_string(largest(cache.freeRootRanges)) +
              ",materials=" +
              std::to_string(largest(cache.freeMaterialRanges)) +
              ",palette=" +
              std::to_string(largest(cache.freePaletteRanges)) + ")";
          return false;
        }
        newAllocationIdsThisTransaction.push_back(entry.allocation.id);
        if (update != nullptr)
          update->newAllocationIds.push_back(entry.allocation.id);
      }
      return true;
    };
    if (!allocateEntries(built) || !allocateEntries(refined) ||
        !allocateEntries(cache.stagedSpatialEntries))
    {
      cache.freeNodeRanges = freeNodeSnapshot;
      cache.freeChildRanges = freeChildSnapshot;
      cache.freeRootRanges = freeRootSnapshot;
      cache.freeMaterialRanges = freeMaterialSnapshot;
      cache.freePaletteRanges = freePaletteSnapshot;
      cache.nextAllocationId = nextAllocationSnapshot;
      cache.slotGenerations = transactionSlotGenerations;
      if (update != nullptr)
        rollbackWorldVoxelChunkCacheUpdate(cache, *update);
      return fail(
          error,
          "Generated-world virtual page allocation failed; the previous "
          "complete hierarchy remains resident;" + allocationFailure);
    }
  }

  // A published cut has to be a *complete* leaf cut over pages that actually
  // own a payload. `desired` is the screen-error cut, and generation is
  // budget-limited, so it routinely names children no source can supply this
  // update. Publishing it verbatim dropped the parent (it is no longer in
  // `desired`, so the eviction loop below retires it) without the children
  // arriving to replace it, and that missing ground is the page-shaped hole.
  //
  // This is the streaming rule virtual geometry already uses: a region may be
  // selected only when its own data is resident, otherwise the parent stays
  // resident and keeps being selected. Collapse any group whose members are
  // not all available back onto the parent, repeatedly, until every published
  // address can be sourced. Generation still targets the unclamped `desired`,
  // so refinement keeps converging; only publication waits for the payload.
  uint32_t cutCollapsedCount = 0u;
  if (spatialLeafCut)
  {
    const auto available = [&](const WorldVoxelChunkAddress &address)
    {
      return findRefined(address) != nullptr ||
          findCached(address) != nullptr || findBuilt(address) != nullptr;
    };
    const auto parentOf = [](const WorldVoxelChunkAddress &address)
    {
      return WorldVoxelChunkAddress{
        address.x >= 0 ? address.x / 2 : -((-address.x + 1) / 2),
        address.z >= 0 ? address.z / 2 : -((-address.z + 1) / 2),
        address.spatialLOD + 1u};
    };
    const uint32_t rootLOD = worldVoxelPageRootLOD(chunkEdge, settings);
    const size_t desiredCount = desired.size();
    for (uint32_t pass = 0u; pass <= rootLOD; ++pass)
    {
      std::vector<WorldVoxelChunkAddress> collapsedParents;
      for (const auto &address : desired)
      {
        if (available(address) || address.spatialLOD >= rootLOD)
          continue;
        const auto parent = parentOf(address);
        if (std::find(
                collapsedParents.begin(), collapsedParents.end(), parent) ==
            collapsedParents.end())
          collapsedParents.push_back(parent);
      }
      if (collapsedParents.empty())
        break;
      std::vector<WorldVoxelChunkAddress> clamped;
      clamped.reserve(desired.size());
      for (const auto &address : desired)
      {
        WorldVoxelChunkAddress candidate = address;
        if (address.spatialLOD < rootLOD)
        {
          const auto parent = parentOf(address);
          if (std::find(
                  collapsedParents.begin(), collapsedParents.end(), parent) !=
              collapsedParents.end())
            candidate = parent;
        }
        if (std::find(clamped.begin(), clamped.end(), candidate) ==
            clamped.end())
          clamped.push_back(candidate);
      }
      desired = std::move(clamped);
    }
    // Anything still unsourceable has no resident ancestor to fall back on.
    // It cannot be drawn either way; dropping it keeps the eviction loop and
    // the entry lookup below consistent.
    desired.erase(
        std::remove_if(
            desired.begin(), desired.end(),
            [&](const WorldVoxelChunkAddress &address)
            { return !available(address); }),
        desired.end());
    cutCollapsedCount = static_cast<uint32_t>(
        desiredCount > desired.size() ? desiredCount - desired.size() : 0u);
  }

  stage("allocate");
  uint32_t retainedDesiredCount = 0u;
  for (const auto &entry : cache.entries)
    if (std::find(desired.begin(), desired.end(), entry.address) != desired.end())
      ++retainedDesiredCount;
  const uint32_t evictedCount = static_cast<uint32_t>(cache.entries.size()) -
      retainedDesiredCount;
  if (settings.maximumCachedPayloads != 0u)
  {
    for (auto &entry : cache.entries)
    {
      if (std::find(desired.begin(), desired.end(), entry.address) !=
          desired.end())
        continue;
      retireWorldPhysicalAllocation(cache, entry.allocation, settings);
      if (update != nullptr)
        update->evictedRollbackEntries.push_back(entry);
      const auto duplicate = std::find_if(
          cache.payloadCache.begin(), cache.payloadCache.end(),
          [&](const WorldVoxelChunkCachedPayload &payload)
          {
            return payload.bundleSignature == bundleIdentity &&
                payload.address == entry.address &&
                payload.lod == entry.residentLOD &&
                payload.generatorRevision == settings.generatorRevision;
          });
      if (duplicate != cache.payloadCache.end())
      {
        if (update != nullptr)
          update->removedPayloadRollbackEntries.push_back(
              std::move(*duplicate));
        cache.payloadCache.erase(duplicate);
      }
      const uint64_t payloadLastUse = ++cache.payloadCacheClock;
      cache.payloadCache.push_back({
        bundleIdentity, entry.address, entry.residentLOD,
        settings.generatorRevision, payloadLastUse,
        std::move(entry.chunk)});
      if (update != nullptr)
        update->addedPayloadLastUses.push_back(payloadLastUse);
    }
    cache.payloadCacheBytes = worldVoxelCPUCacheBytes(cache);
    while (cache.payloadCache.size() > settings.maximumCachedPayloads ||
           (settings.maximumCPUCacheBytes != 0u &&
            cache.payloadCacheBytes > settings.maximumCPUCacheBytes))
    {
      const auto oldest = std::min_element(
          cache.payloadCache.begin(), cache.payloadCache.end(),
          [](const WorldVoxelChunkCachedPayload &left,
             const WorldVoxelChunkCachedPayload &right)
          {
            return left.lastUse < right.lastUse;
          });
      // Spill before the entry is moved out. The store is content-addressed and
      // outside the transaction, so a later rollback that restores this payload
      // to the uncompressed cache leaves a duplicate under the same key rather
      // than an inconsistency.
      if (pageSource.storeTransient(
              oldest->address, oldest->lod, *oldest->chunk))
        ++spilledTransientCount;
      if (update != nullptr &&
          std::find(
              update->addedPayloadLastUses.begin(),
              update->addedPayloadLastUses.end(), oldest->lastUse) ==
              update->addedPayloadLastUses.end())
        update->removedPayloadRollbackEntries.push_back(
            std::move(*oldest));
      cache.payloadCache.erase(oldest);
      cache.payloadCacheBytes = worldVoxelCPUCacheBytes(cache);
    }
  }
  else
  {
    for (const auto &entry : cache.entries)
      if (std::find(desired.begin(), desired.end(), entry.address) ==
          desired.end())
      {
        retireWorldPhysicalAllocation(cache, entry.allocation, settings);
        if (update != nullptr)
          update->evictedRollbackEntries.push_back(entry);
      }
    if (update != nullptr)
      update->removedPayloadRollbackEntries.insert(
          update->removedPayloadRollbackEntries.end(),
          std::make_move_iterator(cache.payloadCache.begin()),
          std::make_move_iterator(cache.payloadCache.end()));
    cache.payloadCache.clear();
    cache.payloadCacheBytes = 0u;
  }
  stage("evict");
  std::vector<WorldVoxelChunkEntry> nextEntries;
  nextEntries.reserve(desired.size());
  for (const auto &address : desired)
  {
    if (auto *entry = findRefined(address))
    {
      if (auto *previous = findCached(address))
        retireWorldPhysicalAllocation(cache, previous->allocation, settings);
      if (update != nullptr)
      {
        if (auto *previous = findCached(address))
          update->refinementRollbackEntries.push_back(std::move(*previous));
      }
      nextEntries.push_back(std::move(*entry));
    }
    else if (auto *entry = findCached(address))
      nextEntries.push_back(std::move(*entry));
    else if (auto *entry = findBuilt(address))
      nextEntries.push_back(std::move(*entry));
  }
  std::sort(
      nextEntries.begin(), nextEntries.end(),
      [](const WorldVoxelChunkEntry &left, const WorldVoxelChunkEntry &right)
      {
        return left.slot < right.slot;
      });
  if (nextEntries.empty())
    return fail(error, "Generated-world progressive cache has no resident chunks");

  if (!cache.virtualArenaInitialized)
  {
    const auto deriveArenaCapacity = [&](const auto sizeFor,
                                         const auto gpuSizeFor,
                                         const uint32_t alignment)
    {
      uint64_t resident = 0u;
      uint64_t maximumScratch = 0u;
      uint64_t maximumObserved = 0u;
      for (const auto &entry : nextEntries)
      {
        uint64_t required = entry.chunk->gpuOwnedGeneratedPayloads
            ? gpuSizeFor(*entry.chunk)
            : sizeFor(entry.chunk->bricks);
        if (alignment > 1u)
          required = (required + alignment - 1u) &
              ~(static_cast<uint64_t>(alignment) - 1u);
        resident += required;
        maximumObserved = std::max(maximumObserved, required);
        const uint32_t shift = std::min(entry.residentLOD * 3u, 31u);
        const uint64_t scale = uint64_t{1u} << shift;
        if (required > std::numeric_limits<uint64_t>::max() / scale)
          return std::numeric_limits<uint64_t>::max();
        maximumScratch = std::max(maximumScratch, required * scale);
      }
      // If startup generation was deliberately bounded, reserve the unseen
      // coarse slots from the largest observed owner. The arena becomes
      // immutable once initialized.
      const uint64_t observedSlots = nextEntries.size();
      resident += maximumObserved *
          (cache.slotCount > observedSlots
               ? cache.slotCount - observedSlots
               : 0u);
      if (maximumObserved != 0u &&
          cache.slotCount >
              std::numeric_limits<uint64_t>::max() / maximumObserved)
        return std::numeric_limits<uint64_t>::max();
      // `slotCount` already includes the published owners and one update's
      // entering ownership groups. Complete groups publish incrementally, so
      // reserving another full `maximumObserved * slotCount` physical cut here
      // duplicates the logical transition headroom and can exceed the GPU
      // budget before the first page is shown. Keep only the bounded fine-page
      // scratch below; retired allocations remain protected by their reader
      // completion tokens and are reclaimed before a range is reused.
      const uint64_t concurrentRefinements = std::max<uint64_t>(
          1u, std::min<uint64_t>(
                  effectiveGenerationBudget == 0u
                      ? (settings.maximumRefinedChunksPerUpdate == 0u
                            ? cache.slotCount
                            : settings.maximumRefinedChunksPerUpdate)
                      : std::min(
                            effectiveGenerationBudget,
                            settings.maximumRefinedChunksPerUpdate == 0u
                                ? effectiveGenerationBudget
                                : settings.maximumRefinedChunksPerUpdate),
                  cache.slotCount));
      if (maximumScratch != 0u &&
          concurrentRefinements >
              std::numeric_limits<uint64_t>::max() / maximumScratch)
        return std::numeric_limits<uint64_t>::max();
      const uint64_t refinementScratch =
          maximumScratch * concurrentRefinements;
      if (refinementScratch >
          std::numeric_limits<uint64_t>::max() - resident)
        return std::numeric_limits<uint64_t>::max();
      const long double scaled =
          static_cast<long double>(
              resident + refinementScratch) *
          settings.virtualArenaScale;
      if (scaled > std::numeric_limits<uint64_t>::max())
        return std::numeric_limits<uint64_t>::max();
      uint64_t total = static_cast<uint64_t>(std::ceil(scaled));
      if (alignment > 1u)
        total = (total + alignment - 1u) &
            ~(static_cast<uint64_t>(alignment) - 1u);
      return total;
    };
    const uint64_t nodeCapacity = deriveArenaCapacity(
        [](const auto &bricks) { return bricks.nodes.size(); },
        [](const auto &page) { return page.gpuArenaNodeCapacity; }, 1u);
    const uint64_t observedChildCapacity = deriveArenaCapacity(
        [](const auto &bricks) { return bricks.childIndices.size(); },
        [](const auto &page) { return page.gpuArenaChildCapacity; }, 1u);
    // A cut made entirely of leaf bricks legitimately has no child indices,
    // but its first finer replacement does.  Deriving an immutable arena from
    // that zero observation makes every refinement impossible.  Each child
    // index names one resident hierarchy node, so the already bounded node
    // arena is also a conservative child-index bound.
    const uint64_t childCapacity =
        std::max(observedChildCapacity, nodeCapacity);
    const uint64_t rootCapacity = deriveArenaCapacity(
        [](const auto &bricks) { return bricks.rootIndices.size(); },
        [](const auto &page) { return page.gpuArenaRootCapacity; }, 1u);
    // Material indices are the arena that runs short first: a refined cliff
    // chunk exposes far more surface voxels (one material byte each) than the
    // coarse bootstrap chunks the derivation observes, so the observed-average
    // estimate undershoots badly on steep ground. A single chunk that cannot
    // fit rolls back the whole frame's transaction - every frame it stays in
    // the desired cut - so the cut can never settle and the ground it owns
    // flickers as a hole. Materials are one byte each, so extra headroom here
    // is the cheapest arena to over-provision. 4x covers the measured cliff
    // shortfall (required 144381 vs 120864 free) with margin; the GPU page
    // budget check below still fails loudly if the total is genuinely too big.
    const uint64_t materialCapacity = 4u * deriveArenaCapacity(
        [](const auto &bricks) { return bricks.materialIndices.size(); },
        [](const auto &page) { return page.gpuArenaMaterialCapacity; }, 4u);
    const uint64_t paletteCapacity = deriveArenaCapacity(
        [](const auto &bricks) { return bricks.palette.size(); },
        [](const auto &page) { return page.gpuArenaPaletteCapacity; }, 2u);
    if (nodeCapacity > std::numeric_limits<uint32_t>::max() ||
        childCapacity > std::numeric_limits<uint32_t>::max() ||
        rootCapacity > std::numeric_limits<uint32_t>::max() ||
        materialCapacity > std::numeric_limits<uint32_t>::max() ||
        paletteCapacity > std::numeric_limits<uint32_t>::max())
      return fail(error, "Generated-world virtual page arena exceeds 32 bits");
    cache.arenaNodeCapacity = static_cast<uint32_t>(nodeCapacity);
    cache.arenaChildCapacity = static_cast<uint32_t>(childCapacity);
    cache.arenaRootCapacity = static_cast<uint32_t>(rootCapacity);
    cache.arenaMaterialCapacity = static_cast<uint32_t>(materialCapacity);
    cache.arenaPaletteCapacity = static_cast<uint32_t>(paletteCapacity);
    cache.arenaMicroSurface = settings.packMicroSurface;
    if (std::getenv("SVR_TRACE_TERRAIN") != nullptr)
      os::Logger::logf(
          "World terrain arena capacity: slots=%u bootstrapEntries=%zu nodes=%llu children=%llu "
          "roots=%llu materials=%llu microSurface=%s (%.2fGiB if packed) palette=%llu",
          cache.slotCount, nextEntries.size(),
          static_cast<unsigned long long>(nodeCapacity), static_cast<unsigned long long>(childCapacity),
          static_cast<unsigned long long>(rootCapacity), static_cast<unsigned long long>(materialCapacity),
          cache.arenaMicroSurface ? "packed" : "not packed",
          static_cast<double>(materialCapacity) * 8.0 / (1024.0 * 1024.0 * 1024.0),
          static_cast<unsigned long long>(paletteCapacity));
    if (settings.maximumGPUPageBytes != 0u &&
        worldVoxelGPUPageBytes(cache) > settings.maximumGPUPageBytes)
      return fail(error, "Generated-world GPU page arena exceeds its budget");
    cache.freeNodeRanges = {{0u, cache.arenaNodeCapacity}};
    cache.freeChildRanges = {{0u, cache.arenaChildCapacity}};
    cache.freeRootRanges = {{0u, cache.arenaRootCapacity}};
    cache.freeMaterialRanges = {{0u, cache.arenaMaterialCapacity}};
    cache.freePaletteRanges = {{0u, cache.arenaPaletteCapacity}};
    cache.virtualArenaInitialized = true;
    cache.layoutRevision = std::max<uint64_t>(cache.layoutRevision + 1u, 1u);
    for (auto &entry : nextEntries)
      if (!allocateWorldPhysicalAllocation(
              cache, *entry.chunk, entry.allocation))
        return fail(error, "Could not bootstrap generated-world virtual pages");
      else
      {
        newAllocationIdsThisTransaction.push_back(entry.allocation.id);
        if (update != nullptr)
          update->newAllocationIds.push_back(entry.allocation.id);
      }
  }

  for (const auto &entry : nextEntries)
  {
    cache.slotNodeCapacity[entry.slot] = entry.allocation.nodes.capacity;
    cache.slotChildCapacity[entry.slot] = entry.allocation.children.capacity;
    cache.slotRootCapacity[entry.slot] = entry.allocation.roots.capacity;
    cache.slotMaterialCapacity[entry.slot] = entry.allocation.materials.capacity;
    cache.slotPaletteCapacity[entry.slot] = entry.allocation.palette.capacity;
  }
  stage("arena");
  chunks.edgeMetres = chunkEdge;
  chunks.voxelSizeMetres = settings.finestVoxelSizeMetres;
  // GPU-owned pages intentionally leave the hierarchy payload vectors empty,
  // but this scalar remains live renderer metadata.  The material G-buffer
  // uses it to address the world-stable per-voxel colour lattice.  Leaving it
  // at the default zero made the renderer clamp the lattice to 1e-6 metres
  // and consequently disable all terrain colour variation.
  chunks.bricks.finestVoxelSize =
      static_cast<float>(settings.finestVoxelSizeMetres);
  chunks.progressiveGeneratedWorld = true;
  chunks.gpuOwnedGeneratedPayloads = settings.gpuOwnedPayloads;
  chunks.progressiveVirtualPages = true;
  chunks.progressiveSlotCapacity = cache.slotCount;
  chunks.progressiveWindowExtentMetres = spatialLeafCut
      ? settings.renderDistanceMetres * 2.0
      : chunkEdge * gridSide;
  chunks.progressiveSplitErrorPixels = settings.splitErrorPixels;
  chunks.progressiveMergeErrorPixels = settings.mergeErrorPixels;
  chunks.progressiveDistanceErrorGrowthPerKilometre =
      settings.distanceErrorGrowthPerKilometre;
  chunks.progressiveDistanceErrorExponent = settings.distanceErrorExponent;
  chunks.progressiveMinimumResidentFrames = settings.minimumResidentFrames;
  chunks.progressiveCoarseningGraceFrames =
      settings.coarseningGraceFrames;
  double packedElevationOrigin = cache.hasElevationOrigin
      ? cache.elevationOriginMetres
      : std::numeric_limits<double>::max();
  chunks.minimumWorldMetres = {
    std::numeric_limits<double>::max(),
    std::numeric_limits<double>::max(),
    std::numeric_limits<double>::max()};
  chunks.maximumWorldMetres = {
    std::numeric_limits<double>::lowest(),
    std::numeric_limits<double>::lowest(),
    std::numeric_limits<double>::lowest()};
  if (!cache.hasElevationOrigin)
    for (const auto &entry : nextEntries)
      packedElevationOrigin = std::min(
          packedElevationOrigin, entry.chunk->elevationOriginMetres);
  if (!std::isfinite(packedElevationOrigin))
    return fail(error, "Generated-world elevation origin is invalid");
  chunks.elevationOriginMetres = packedElevationOrigin;
  const std::array<double, 4> canonicalWorldGrid{
    -bundle.extentMetres * 0.5, 0.0, -bundle.extentMetres * 0.5,
    settings.finestVoxelSizeMetres};
  std::transform(
      canonicalWorldGrid.begin(), canonicalWorldGrid.end(),
      chunks.canonicalWorldGrid.begin(),
      [](const double value) { return static_cast<float>(value); });
  chunks.gpuArenaNodeCapacity = cache.arenaNodeCapacity;
  chunks.gpuArenaChildCapacity = cache.arenaChildCapacity;
  chunks.gpuArenaRootCapacity = cache.arenaRootCapacity;
  chunks.gpuArenaMaterialCapacity = cache.arenaMaterialCapacity;
  chunks.gpuArenaPaletteCapacity = cache.arenaPaletteCapacity;
  if (!settings.gpuOwnedPayloads)
  {
    // A recycled arena of the right shape is left exactly as it is: no
    // allocation and no zero fill. Only a capacity change pays for either.
    // Recycling the allocation is what avoids the first-touch page faults, and
    // zeroing it in parallel is what keeps the old semantics exactly: the
    // arena a transaction publishes into is fully zero outside the page ranges
    // it writes. Anything else has to reason about which bytes a two
    // generations old buffer still holds, and the terrain G-buffer's motion
    // vectors caught that being wrong.
    // The arena mirror is recycled and never wholesale zeroed. Zeroing 1.9 GiB
    // per transaction cost 600 ms and bought nothing: no consumer reads a byte
    // outside a published page's range - every read goes through a record - and
    // inside a range the bytes are either rewritten by this transaction or
    // already correct from whichever transaction wrote them. Each written range
    // still has its tail cleared to its full reservation, because the upload
    // publishes the whole reservation.
    uint64_t reshapedElements = 0u;
    const auto shapeArena = [&](auto &vector, const size_t size)
    {
      if (vector.size() == size)
        return;
      using Value = typename std::decay_t<decltype(vector)>::value_type;
      reshapedElements += size;
      vector.assign(size, Value{});
    };
    shapeArena(chunks.bricks.nodes, cache.arenaNodeCapacity);
    shapeArena(chunks.bricks.occupancy, cache.arenaNodeCapacity);
    shapeArena(chunks.bricks.childIndices, cache.arenaChildCapacity);
    shapeArena(chunks.bricks.rootIndices, cache.arenaRootCapacity);
    shapeArena(chunks.bricks.materialIndices, cache.arenaMaterialCapacity);
    shapeArena(chunks.bricks.microSurfaceData, cache.arenaMicroSurface ? cache.arenaMaterialCapacity : 0u);
    shapeArena(chunks.bricks.palette, cache.arenaPaletteCapacity);
    shapeArena(chunks.bricks.paletteDescriptorIds, cache.arenaPaletteCapacity);
    if (traceStages && reshapedElements != 0u)
      os::Logger::logf(
          "World terrain arena reshaped: elements=%llu",
          static_cast<unsigned long long>(reshapedElements));
    stage("arenaShape");
  }

  const auto mixRevision = [](uint64_t revision, const uint64_t value)
  {
    revision ^= value;
    revision *= 0x100000001b3ull;
    return revision;
  };
  const auto mappingIdFor = [&](const WorldVoxelChunkAddress &address)
  {
    uint64_t hash = 0xcbf29ce484222325ull;
    hash = mixRevision(hash, static_cast<uint32_t>(address.x));
    hash = mixRevision(hash, static_cast<uint32_t>(address.z));
    hash = mixRevision(hash, address.spatialLOD);
    uint32_t result = static_cast<uint32_t>(hash ^ (hash >> 32u));
    return result != 0u ? result : 1u;
  };
  const auto isNewAllocationThisTransaction = [&](const uint64_t allocationId)
  {
    return allocationId != 0u &&
        std::find(
            newAllocationIdsThisTransaction.begin(),
            newAllocationIdsThisTransaction.end(), allocationId) !=
        newAllocationIdsThisTransaction.end();
  };
  const auto rangeBytes = [](const uint32_t count, const size_t elementSize)
  {
    return static_cast<uint64_t>(count) * elementSize;
  };
  const auto appendTerrainPageUpload =
      [&](const WorldVoxelChunkEntry &entry,
          const PlanetVoxelChunkRecord &record,
          const uint64_t nodeRevision,
          const bool visible)
  {
    const bool previouslyStaged = std::find(
        previouslyStagedAllocationIds.begin(),
        previouslyStagedAllocationIds.end(), entry.allocation.id) !=
        previouslyStagedAllocationIds.end();
    // GPU-v2 has one scratch lane, so an invisible payload cannot be retained
    // safely across other builds yet. Generate it when its complete ownership
    // group becomes visible; the parent remains authoritative until the final
    // atomic publication. CPU-owned pages keep their hidden-upload path.
    if ((settings.gpuOwnedPayloads && !visible) ||
        (!isNewAllocationThisTransaction(entry.allocation.id) &&
         !(settings.gpuOwnedPayloads && visible && previouslyStaged)))
      return;
    PlanetVoxelTerrainPageUpload upload{};
    upload.identity.slot = entry.slot;
    upload.identity.generation = entry.generation;
    upload.identity.internalLOD = record.clipmapLOD;
    upload.identity.rasterAllocation = entry.allocation.id;
    upload.identity.geometryRevision = record.tree64Revision;
    upload.pageX = entry.address.x;
    upload.pageZ = entry.address.z;
    upload.spatialLOD = entry.address.spatialLOD;
    upload.residentLOD = entry.residentLOD;
    upload.payloadRevision = record.brickPayloadRevision;
    upload.nodeRevision = nodeRevision;
    upload.visible = visible;
    upload.nodes = {record.brickNodeOffset, record.brickNodeCapacity};
    upload.occupancy = {record.brickNodeOffset, record.brickNodeCapacity};
    upload.childIndices = {
      record.brickChildOffset, record.brickChildCapacity};
    upload.roots = {record.firstBrickRoot, record.brickRootCapacity};
    upload.materialIndices = {
      record.brickMaterialOffset, record.brickMaterialCapacity};
    if (!chunks.bricks.microSurfaceData.empty())
      upload.microSurfaceData = {
        record.brickMaterialOffset, record.brickMaterialCapacity};
    upload.palette = {record.brickPaletteOffset, record.brickPaletteCapacity};
    upload.paletteDescriptorIds = {
      record.brickPaletteOffset, record.brickPaletteCapacity};
    upload.payloadBytes =
        rangeBytes(upload.occupancy.count, sizeof(chunks.bricks.occupancy[0])) +
        rangeBytes(upload.childIndices.count,
                   sizeof(chunks.bricks.childIndices[0])) +
        rangeBytes(upload.roots.count, sizeof(chunks.bricks.rootIndices[0])) +
        rangeBytes(upload.materialIndices.count,
                   sizeof(chunks.bricks.materialIndices[0])) +
        rangeBytes(upload.microSurfaceData.count,
                   sizeof(chunks.bricks.microSurfaceData[0])) +
        rangeBytes(upload.palette.count, sizeof(chunks.bricks.palette[0])) +
        rangeBytes(upload.paletteDescriptorIds.count,
                   sizeof(chunks.bricks.paletteDescriptorIds[0]));
    upload.publicationBytes =
        rangeBytes(upload.nodes.count, sizeof(chunks.bricks.nodes[0]));
    if (settings.gpuOwnedPayloads)
    {
      // These ranges are GPU build destinations, not CPU staging payloads.
      // Directory patches below remain compact CPU-authored metadata.
      upload.payloadBytes = 0u;
      upload.publicationBytes = 0u;
    }
    const uint32_t pageIndex = cache.pageDirectory.find(entry.address);
    if (pageIndex != kWorldVoxelInvalidPage)
    {
      const auto &directoryEntry = cache.pageDirectory.entries[pageIndex];
      const uint64_t patchEnd =
          static_cast<uint64_t>(directoryEntry.installPatchOffset) +
          directoryEntry.installPatchCount;
      if (patchEnd <= cache.pageDirectory.installPatches.size())
      {
        upload.installPatches.insert(
            upload.installPatches.end(),
            cache.pageDirectory.installPatches.begin() +
                directoryEntry.installPatchOffset,
            cache.pageDirectory.installPatches.begin() + patchEnd);
        upload.publicationBytes +=
            rangeBytes(
                static_cast<uint32_t>(upload.installPatches.size()),
                sizeof(TerrainPagePatch));
      }
    }
    chunks.terrainPageUploads.push_back(std::move(upload));
  };
  stage("pageHelpers");
  double closestCenterDistance = std::numeric_limits<double>::max();
  std::vector<bool> publishedSlots(cache.slotCount, false);
  uint32_t skippedArenaWrites = 0u;
  std::vector<uint32_t> publishedMappingIds;
  publishedMappingIds.reserve(nextEntries.size());
  for (const auto &entry : nextEntries)
  {
    if (entry.chunk->chunks.empty())
      return fail(error, "Generated-world progressive chunk has no record");
    const auto &sourceRecord = entry.chunk->chunks.front();
    const uint32_t slot = entry.slot;
    if (slot >= publishedSlots.size() || publishedSlots[slot])
      return fail(error, "Generated-world progressive chunks do not have unique toroidal slots");
    publishedSlots[slot] = true;
    const double elevationDelta =
        entry.chunk->elevationOriginMetres - chunks.elevationOriginMetres;
    const std::array<float, 3> translation{
      0.0f, static_cast<float>(elevationDelta), 0.0f};
    const std::array<int32_t, 3> canonicalTranslation{
      0,
      static_cast<int32_t>(std::llround(
          elevationDelta / settings.finestVoxelSizeMetres)),
      0};
    const uint32_t mappingId = mappingIdFor(entry.address);
    if (std::find(
            publishedMappingIds.begin(), publishedMappingIds.end(),
            mappingId) != publishedMappingIds.end())
      return fail(error, "Generated-world progressive ownership identity collided");
    publishedMappingIds.push_back(mappingId);
    uint64_t nodeRevision = 0u;
    const uint32_t combinedLOD = std::min(
        entry.address.spatialLOD + entry.residentLOD, 31u);
    const uint32_t residentScale = uint32_t{1u} << combinedLOD;
    // A page already written at these exact offsets, from this exact payload,
    // is left alone: the arena is not zeroed between transactions, so its bytes
    // are still there. Only the root index range is rewritten, because roots
    // are packed by publication order and move for almost every page.
    const PlanetVoxelChunkRecord *resident = nullptr;
    if (!settings.gpuOwnedPayloads && arenaResidentRecords != nullptr)
    {
      const uint64_t payloadRevision =
          mixRevision(sourceRecord.brickPayloadRevision, mappingId);
      for (const auto &candidate : *arenaResidentRecords)
        if (candidate.regionMappingId == mappingId &&
            candidate.brickPayloadRevision == payloadRevision &&
            candidate.brickNodeOffset == entry.allocation.nodes.offset &&
            candidate.brickNodeCapacity == entry.allocation.nodes.capacity &&
            candidate.brickChildOffset == entry.allocation.children.offset &&
            candidate.brickChildCapacity == entry.allocation.children.capacity &&
            candidate.brickMaterialOffset == entry.allocation.materials.offset &&
            candidate.brickMaterialCapacity ==
                entry.allocation.materials.capacity &&
            candidate.brickPaletteOffset == entry.allocation.palette.offset &&
            candidate.brickPaletteCapacity == entry.allocation.palette.capacity)
        {
          resident = &candidate;
          break;
        }
    }
    if (settings.gpuOwnedPayloads)
      nodeRevision = sourceRecord.brickNodeRevision;
    else if (resident != nullptr)
    {
      nodeRevision = resident->brickNodeRevision;
      if (!rewriteTranslatedHierarchyRoots(
              chunks.bricks, entry.chunk->bricks,
              entry.allocation.nodes.offset, entry.allocation.roots.offset,
              entry.allocation.roots.capacity, error))
        return false;
      chunks.bricks.levelCount = std::max(
          chunks.bricks.levelCount, entry.chunk->bricks.levelCount);
      chunks.bricks.paletteInstanceCount +=
          entry.chunk->bricks.paletteInstanceCount;
      chunks.bricks.paletteExactInstances +=
          entry.chunk->bricks.paletteExactInstances;
      chunks.bricks.paletteEmittedVoxels +=
          entry.chunk->bricks.paletteEmittedVoxels;
      chunks.bricks.paletteEmittedApproximated +=
          entry.chunk->bricks.paletteEmittedApproximated;
      chunks.bricks.paletteMaximumError = std::max(
          chunks.bricks.paletteMaximumError,
          entry.chunk->bricks.paletteMaximumError);
      chunks.bricks.paletteEmittedMaximumError = std::max(
          chunks.bricks.paletteEmittedMaximumError,
          entry.chunk->bricks.paletteEmittedMaximumError);
      chunks.bricks.paletteStraddlingBricks +=
          entry.chunk->bricks.paletteStraddlingBricks;
      chunks.bricks.paletteStraddlingVoxels +=
          entry.chunk->bricks.paletteStraddlingVoxels;
      ++skippedArenaWrites;
    }
    else if (!writeTranslatedHierarchySlot(
                 chunks.bricks, entry.chunk->bricks, translation,
                 canonicalTranslation, residentScale,
                 &canonicalWorldGrid, mappingId, {}, combinedLOD,
                 spatialLeafCut && !settings.rasterSafeCoarseTerrain, slot,
                 entry.allocation.nodes.offset,
                 entry.allocation.nodes.capacity,
                 entry.allocation.children.offset,
                 entry.allocation.children.capacity,
                 entry.allocation.roots.offset,
                 entry.allocation.roots.capacity,
                 entry.allocation.materials.offset,
                 entry.allocation.materials.capacity,
                 entry.allocation.palette.offset,
                 entry.allocation.palette.capacity,
                 &nodeRevision, error))
      return false;
    PlanetVoxelChunkRecord record = sourceRecord;
    record.requestedTile = {
      0u, static_cast<uint8_t>(combinedLOD),
      static_cast<uint32_t>(entry.address.x),
      static_cast<uint32_t>(entry.address.z)};
    record.sourceTile = record.requestedTile;
    record.tileOffsetX = entry.address.x;
    record.tileOffsetY = entry.address.z;
    record.clipmapLOD = combinedLOD;
    record.clipmapSlot = slot;
    record.residencyGeneration = entry.generation;
    const auto priorityPosition = std::find(
        desired.begin(), desired.end(), entry.address);
    record.streamingPriority = priorityPosition != desired.end()
        ? static_cast<uint32_t>(desired.end() - priorityPosition)
        : 0u;
    record.regionMappingId = mappingId;
    record.brickNodeOffset = entry.allocation.nodes.offset;
    record.brickNodeCapacity = entry.allocation.nodes.capacity;
    record.brickChildOffset = entry.allocation.children.offset;
    record.brickChildCapacity = entry.allocation.children.capacity;
    record.firstBrickRoot = entry.allocation.roots.offset;
    record.brickRootCapacity = entry.allocation.roots.capacity;
    record.brickRootCount = entry.chunk->gpuOwnedGeneratedPayloads
        ? sourceRecord.brickRootCount
        : static_cast<uint32_t>(entry.chunk->bricks.rootIndices.size());
    record.brickMaterialOffset = entry.allocation.materials.offset;
    record.brickMaterialCapacity = entry.allocation.materials.capacity;
    record.brickPaletteOffset = entry.allocation.palette.offset;
    record.brickPaletteCapacity = entry.allocation.palette.capacity;
    record.brickNodeRevision = nodeRevision;
    // Tree64 identity follows authored geometry and stable logical ownership,
    // never the physical brick offsets baked into translated raster nodes.
    // A virtual-page relocation can therefore retain its BLAS payload.
    record.tree64Revision = mixRevision(
        mixRevision(sourceRecord.tree64Revision, mappingId),
        combinedLOD);
    record.brickPayloadRevision = mixRevision(
        sourceRecord.brickPayloadRevision, mappingId);
    appendTerrainPageUpload(entry, record, nodeRevision, true);
    chunks.chunks.push_back(record);
    chunks.leafVoxelCount += entry.chunk->leafVoxelCount;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const double offset = axis == 1u ? elevationDelta : 0.0;
      chunks.minimumWorldMetres[axis] = std::min(
          chunks.minimumWorldMetres[axis],
          entry.chunk->minimumWorldMetres[axis] + offset);
      chunks.maximumWorldMetres[axis] = std::max(
          chunks.maximumWorldMetres[axis],
          entry.chunk->maximumWorldMetres[axis] + offset);
    }
    if (chunks.surfaceHeightSide == 0u)
    {
      chunks.surfaceHeightSide = entry.chunk->surfaceHeightSide;
      chunks.surfaceHeightHalo = entry.chunk->surfaceHeightHalo;
      chunks.surfaceMaterialSide = entry.chunk->surfaceMaterialSide;
    }
    if (chunks.surfaceHeightSide == entry.chunk->surfaceHeightSide &&
        chunks.surfaceHeightHalo == entry.chunk->surfaceHeightHalo &&
        chunks.surfaceMaterialSide == entry.chunk->surfaceMaterialSide)
    {
      chunks.surfaceHeights.insert(
          chunks.surfaceHeights.end(), entry.chunk->surfaceHeights.begin(),
          entry.chunk->surfaceHeights.end());
      chunks.surfaceEnvelope.insert(
          chunks.surfaceEnvelope.end(), entry.chunk->surfaceEnvelope.begin(),
          entry.chunk->surfaceEnvelope.end());
      chunks.surfaceMaterials.insert(
          chunks.surfaceMaterials.end(), entry.chunk->surfaceMaterials.begin(),
          entry.chunk->surfaceMaterials.end());
    }
    const double distance = std::hypot(
        record.worldCenterXMetres - cameraWorldXMetres,
        record.worldCenterZMetres - cameraWorldZMetres);
    if (distance < closestCenterDistance)
    {
      closestCenterDistance = distance;
      chunks.centerTile = record.requestedTile;
      chunks.centerSurfaceMetres = record.centerSurfaceMetres;
      chunks.centerMaterial = record.centerMaterial;
    }
  }
  for (const auto &entry : cache.stagedSpatialEntries)
  {
    if (!isNewAllocationThisTransaction(entry.allocation.id))
      continue;
    if (entry.chunk->chunks.empty())
      return fail(error, "Generated-world staged chunk has no record");
    const auto &sourceRecord = entry.chunk->chunks.front();
    const double elevationDelta =
        entry.chunk->elevationOriginMetres - chunks.elevationOriginMetres;
    const std::array<float, 3> translation{
      0.0f, static_cast<float>(elevationDelta), 0.0f};
    const std::array<int32_t, 3> canonicalTranslation{
      0,
      static_cast<int32_t>(std::llround(
          elevationDelta / settings.finestVoxelSizeMetres)),
      0};
    const uint32_t mappingId = mappingIdFor(entry.address);
    uint64_t nodeRevision = 0u;
    const uint32_t combinedLOD = std::min(
        entry.address.spatialLOD + entry.residentLOD, 31u);
    const uint32_t residentScale = uint32_t{1u} << combinedLOD;
    const uint32_t levelCount = chunks.bricks.levelCount;
    const uint32_t paletteInstanceCount = chunks.bricks.paletteInstanceCount;
    const uint64_t paletteEmittedVoxels = chunks.bricks.paletteEmittedVoxels;
    const uint64_t paletteEmittedApproximated =
        chunks.bricks.paletteEmittedApproximated;
    const uint64_t paletteExactInstances =
        chunks.bricks.paletteExactInstances;
    if (settings.gpuOwnedPayloads)
      nodeRevision = sourceRecord.brickNodeRevision;
    else if (!writeTranslatedHierarchySlot(
                 chunks.bricks, entry.chunk->bricks, translation,
                 canonicalTranslation, residentScale,
                 &canonicalWorldGrid, mappingId, {}, combinedLOD,
                 spatialLeafCut && !settings.rasterSafeCoarseTerrain,
                 entry.slot, entry.allocation.nodes.offset,
                 entry.allocation.nodes.capacity,
                 entry.allocation.children.offset,
                 entry.allocation.children.capacity,
                 entry.allocation.roots.offset,
                 entry.allocation.roots.capacity,
                 entry.allocation.materials.offset,
                 entry.allocation.materials.capacity,
                 entry.allocation.palette.offset,
                 entry.allocation.palette.capacity,
                 &nodeRevision, error))
      return false;
    chunks.bricks.levelCount = levelCount;
    chunks.bricks.paletteInstanceCount = paletteInstanceCount;
    chunks.bricks.paletteEmittedVoxels = paletteEmittedVoxels;
    chunks.bricks.paletteEmittedApproximated = paletteEmittedApproximated;
    chunks.bricks.paletteExactInstances = paletteExactInstances;
    PlanetVoxelChunkRecord record = sourceRecord;
    record.requestedTile = {
      0u, static_cast<uint8_t>(combinedLOD),
      static_cast<uint32_t>(entry.address.x),
      static_cast<uint32_t>(entry.address.z)};
    record.sourceTile = record.requestedTile;
    record.tileOffsetX = entry.address.x;
    record.tileOffsetY = entry.address.z;
    record.clipmapLOD = combinedLOD;
    record.clipmapSlot = entry.slot;
    record.residencyGeneration = entry.generation;
    record.streamingPriority = 0u;
    record.regionMappingId = mappingId;
    record.brickNodeOffset = entry.allocation.nodes.offset;
    record.brickNodeCapacity = entry.allocation.nodes.capacity;
    record.brickChildOffset = entry.allocation.children.offset;
    record.brickChildCapacity = entry.allocation.children.capacity;
    record.firstBrickRoot = entry.allocation.roots.offset;
    record.brickRootCapacity = entry.allocation.roots.capacity;
    record.brickRootCount = entry.chunk->gpuOwnedGeneratedPayloads
        ? sourceRecord.brickRootCount
        : static_cast<uint32_t>(entry.chunk->bricks.rootIndices.size());
    record.brickMaterialOffset = entry.allocation.materials.offset;
    record.brickMaterialCapacity = entry.allocation.materials.capacity;
    record.brickPaletteOffset = entry.allocation.palette.offset;
    record.brickPaletteCapacity = entry.allocation.palette.capacity;
    record.brickNodeRevision = nodeRevision;
    record.tree64Revision = mixRevision(
        mixRevision(sourceRecord.tree64Revision, mappingId),
        combinedLOD);
    record.brickPayloadRevision = mixRevision(
        sourceRecord.brickPayloadRevision, mappingId);
    appendTerrainPageUpload(entry, record, nodeRevision, false);
  }
  if (traceStages)
    os::Logger::logf(
        "World terrain publish: pages=%zu translated=%zu reused=%u",
        chunks.chunks.size(),
        chunks.chunks.size() - skippedArenaWrites, skippedArenaWrites);
  stage("publishEntries");
  chunks.clipmapLayoutRevision = cache.layoutRevision;

  // Materialize the stable-slot page transition once, before either renderer
  // sees the transaction. Raster and Tree64 have different physical page
  // handles, but they must compare against and publish the same logical
  // generation/LOD/geometry identity. This is the generated-world equivalent
  // of virtual geometry's install/uninstall rewrite list.
  const auto replacementPageIdentity = [&](
      const WorldVoxelChunkEntry *entry, const uint32_t slot)
  {
    PlanetVoxelHierarchyPageIdentity identity{};
    identity.slot = slot;
    if (entry == nullptr || entry->chunk->chunks.empty() ||
        entry->allocation.id == 0u)
      return identity;
    const uint32_t mappingId = mappingIdFor(entry->address);
    const auto &sourceRecord = entry->chunk->chunks.front();
    identity.generation = entry->generation;
    const uint32_t combinedLOD = std::min(
        entry->address.spatialLOD + entry->residentLOD, 31u);
    identity.internalLOD = combinedLOD;
    identity.rasterAllocation = entry->allocation.id;
    identity.geometryRevision = mixRevision(
        mixRevision(sourceRecord.tree64Revision, mappingId),
        combinedLOD);
    return identity;
  };
  const auto expectedPageIdentity = [&](
      const PreviousPageIdentitySnapshot *entry, const uint32_t slot)
  {
    PlanetVoxelHierarchyPageIdentity identity{};
    identity.slot = slot;
    if (entry == nullptr || entry->rasterAllocation == 0u ||
        entry->sourceTree64Revision == 0u)
      return identity;
    const uint32_t mappingId = mappingIdFor(entry->address);
    identity.generation = entry->generation;
    identity.internalLOD = entry->residentLOD;
    identity.rasterAllocation = entry->rasterAllocation;
    identity.geometryRevision = mixRevision(
        mixRevision(entry->sourceTree64Revision, mappingId),
        entry->residentLOD);
    return identity;
  };
  const auto sameIdentity = [](const PlanetVoxelHierarchyPageIdentity &left,
                               const PlanetVoxelHierarchyPageIdentity &right)
  {
    return left.slot == right.slot &&
        left.generation == right.generation &&
        left.internalLOD == right.internalLOD &&
        left.rasterAllocation == right.rasterAllocation &&
        left.geometryRevision == right.geometryRevision;
  };
  for (uint32_t slot = 0u; slot < cache.slotCount; ++slot)
  {
    const auto previous = std::find_if(
        previousPageIdentities.begin(), previousPageIdentities.end(),
        [&](const PreviousPageIdentitySnapshot &entry)
        {
          return entry.slot == slot;
        });
    const auto replacement = std::find_if(
        nextEntries.begin(), nextEntries.end(),
        [&](const WorldVoxelChunkEntry &entry) { return entry.slot == slot; });
    const auto *previousEntry = previous != previousPageIdentities.end()
        ? &*previous : nullptr;
    const auto *replacementEntry = replacement != nextEntries.end()
        ? &*replacement : nullptr;
    const auto expectedIdentity = expectedPageIdentity(previousEntry, slot);
    const auto replacementIdentity = replacementPageIdentity(
        replacementEntry, slot);
    if (sameIdentity(expectedIdentity, replacementIdentity))
      continue;
    PlanetVoxelHierarchyPageRewrite rewrite{};
    rewrite.expected = expectedIdentity;
    rewrite.replacement = replacementIdentity;
    if (previousEntry == nullptr)
      rewrite.kind = PlanetVoxelHierarchyRewriteKind::InstallRoot;
    else if (replacementEntry == nullptr)
      rewrite.kind = PlanetVoxelHierarchyRewriteKind::RemoveOwner;
    else if (!(previousEntry->address == replacementEntry->address))
      rewrite.kind = PlanetVoxelHierarchyRewriteKind::ReplaceOwner;
    else if (replacementEntry->residentLOD < previousEntry->residentLOD)
      rewrite.kind = PlanetVoxelHierarchyRewriteKind::Refine;
    else if (replacementEntry->residentLOD > previousEntry->residentLOD)
      rewrite.kind = PlanetVoxelHierarchyRewriteKind::Coarsen;
    else
      rewrite.kind = PlanetVoxelHierarchyRewriteKind::ReplaceOwner;
    chunks.hierarchyPageRewrites.push_back(rewrite);
  }

  stage("pageIdentity");
  const bool residentCutChanged =
      cache.entries.size() != nextEntries.size() ||
      std::any_of(
          nextEntries.begin(), nextEntries.end(),
          [&](const WorldVoxelChunkEntry &next)
          {
            const auto previous = std::find_if(
                cache.entries.begin(), cache.entries.end(),
                [&](const WorldVoxelChunkEntry &entry)
                {
                  return entry.slot == next.slot;
                });
            return previous == cache.entries.end() ||
                !(previous->address == next.address) ||
                previous->generation != next.generation ||
                previous->residentLOD != next.residentLOD ||
                previous->allocation.id != next.allocation.id;
          });

  // Spatial page refinement replaces one owner with descendant pages instead
  // of changing a fixed-footprint entry's internal LOD. Account for that path
  // explicitly so refinement telemetry describes the renderer-visible cut,
  // not only the legacy replacement mechanism above.
  uint32_t spatialRefinedCount = 0u;
  uint32_t spatialCoarsenedCount = 0u;
  if (spatialLeafCut)
    for (const auto &next : nextEntries)
    {
      const bool retained = std::any_of(
          cache.entries.begin(), cache.entries.end(),
          [&](const WorldVoxelChunkEntry &previous)
          { return previous.address == next.address; });
      if (retained)
        continue;
      if (std::any_of(
              cache.entries.begin(), cache.entries.end(),
              [&](const WorldVoxelChunkEntry &previous)
              {
                return worldVoxelPageAncestorOf(
                    previous.address, next.address);
              }))
        ++spatialRefinedCount;
      else if (std::any_of(
                   cache.entries.begin(), cache.entries.end(),
                   [&](const WorldVoxelChunkEntry &previous)
                   {
                     return worldVoxelPageAncestorOf(
                         next.address, previous.address);
                   }))
        ++spatialCoarsenedCount;
    }

  stage("spatialCut");
  cache.bundleSignature = bundleIdentity;
  cache.settings = settings;
  cache.hasElevationOrigin = true;
  cache.elevationOriginMetres = packedElevationOrigin;
  cache.entries = std::move(nextEntries);
  cache.payloadCacheBytes = worldVoxelCPUCacheBytes(cache);
  std::string physicalPageError;
  if (!validateWorldVoxelChunkPhysicalPages(cache, &physicalPageError))
    return fail(error, physicalPageError);
  stage("cacheCommit");
  const uint32_t pendingRefinementCount = static_cast<uint32_t>(
      std::count_if(
          cache.entries.begin(), cache.entries.end(),
          [](const WorldVoxelChunkEntry &entry)
          {
            return entry.requestedLOD < entry.residentLOD;
          }));
  // The initial transaction deliberately publishes only a complete coarse
  // fallback. Keep the real requested cut pending so callers immediately
  // schedule the next bounded batch without requiring camera movement or GPU
  // feedback. Reporting zero here left the bootstrap visible forever.
  const uint32_t pendingBootstrapDetailCount =
      bootstrapSpatialCut
      ? static_cast<uint32_t>(std::count_if(
            requestedAfterBootstrap.begin(), requestedAfterBootstrap.end(),
            [&](const WorldVoxelChunkAddress &address)
            {
              return std::none_of(
                  cache.entries.begin(), cache.entries.end(),
                  [&](const WorldVoxelChunkEntry &entry)
                  {
                    return entry.address == address;
                  });
            }))
      : 0u;
  // A cut clamped to one level per transaction reaches its clamped target with
  // nothing coarse pending, while the metric still asks for more. Report that
  // remaining depth, or the caller stops scheduling transactions the moment the
  // first subdivision lands and the terrain stalls one level in.
  const auto &deepeningTarget = feedbackFallbackRequested.empty()
      ? fullyRequested : feedbackFallbackRequested;
  const uint32_t pendingDeepeningCount = static_cast<uint32_t>(
      std::count_if(
          deepeningTarget.begin(), deepeningTarget.end(),
          [&](const WorldVoxelChunkAddress &address)
          {
            // A complete non-overlapping cut either has this target leaf, has
            // one coarser ancestor covering it, or already covers it with
            // finer descendants. Only the ancestor case needs deepening.
            // Exact-address comparison incorrectly called an intentionally
            // over-refined feedback page pending and made it coarsen again.
            return std::any_of(
                cache.entries.begin(), cache.entries.end(),
                [&](const WorldVoxelChunkEntry &entry)
                {
                  return !(entry.address == address) &&
                      worldVoxelPageAncestorOf(entry.address, address);
                });
          }));
  const bool metricCutReached =
      !spatialCutMetricConvergence ||
      (cache.entries.size() == fullyRequested.size() &&
       std::all_of(
           fullyRequested.begin(), fullyRequested.end(),
           [&](const WorldVoxelChunkAddress &address)
           {
             return std::any_of(
                 cache.entries.begin(), cache.entries.end(),
                 [&](const WorldVoxelChunkEntry &entry)
                 { return entry.address == address; });
           }));
  const uint32_t pendingMetricCutCount = metricCutReached ? 0u : 1u;
  cache.pendingChunkCount = pendingCoarseCount + pendingRefinementCount +
      pendingBootstrapDetailCount + pendingDeepeningCount +
      pendingMetricCutCount;
  // Track unfinished bootstrap/motion work explicitly. A deferred GPU request
  // also contributes to pendingChunkCount, but is not permission to replace
  // a fine measured cut with the unrelated coarse CPU target on an idle frame.
  const bool cpuTargetPending = cpuDrivenSpatialConvergence &&
      (pendingBootstrapDetailCount != 0u || pendingDeepeningCount != 0u ||
       pendingCoarseCount != 0u);
  cache.spatialCutMetricConvergencePending =
      explicitFeedbackRequest && !settings.feedbackDrivenFallbackConvergence
          ? false : (!metricCutReached || cpuTargetPending);

  if (spatialLeafCut)
    annotateWorldVoxelPageDirectory(cache, cache.pageDirectory);
  else
    cache.pageDirectory = {};
  if (spatialLeafCut)
  {
    chunks.terrainRuntimePageTable =
        buildWorldVoxelTerrainRuntimePageTable(cache.pageDirectory, cache);
    std::string runtimePageError;
    if (!settings.gpuOwnedPayloads &&
        !compactWorldVoxelTerrainRuntimeVisibleBrickRoots(
            chunks.terrainRuntimePageTable, chunks.chunks, chunks.bricks,
            &runtimePageError))
      return fail(error, runtimePageError);
    if (!validateWorldVoxelTerrainRuntimePageTable(
            cache.pageDirectory, chunks.terrainRuntimePageTable,
            &runtimePageError))
      return fail(
          error,
          runtimePageError.empty()
              ? "Generated-world GPU runtime page table validation failed"
              : runtimePageError);
  }

  stage("pendingCounts");
  const WorldVoxelPageStreamingPlan finalDirectoryStreamingPlan =
      spatialLeafCut
      ? planWorldVoxelPageStreaming(
            cache.pageDirectory,
            !requestedAfterBootstrap.empty()
                ? requestedAfterBootstrap
                : fullyRequested)
      : WorldVoxelPageStreamingPlan{};

  if (update != nullptr)
  {
    stage("publish");
    update->residentCutChanged = residentCutChanged;
    update->reusedChunkCount = reusedCount;
    update->builtChunkCount = generatedThisUpdate +
        static_cast<uint32_t>(refined.size());
    update->refinedChunkCount = refinedCount + spatialRefinedCount;
    update->coarsenedChunkCount = coarsenedCount + spatialCoarsenedCount;
    update->evictedChunkCount = evictedCount;
    update->collapsedUnavailableChunkCount = cutCollapsedCount;
    update->pendingChunkCount = cache.pendingChunkCount;
    update->coarsestResidentLOD = coarsestLOD;
    update->restoredCachedChunkCount = restoredCachedCount;
    update->restoredTransientMemoryChunkCount = restoredTransientMemoryCount;
    update->restoredTransientDiskChunkCount = restoredTransientDiskCount;
    update->spilledTransientChunkCount = spilledTransientCount;
    update->storedTransientChunkCount = storedTransientCount;
    update->directoryEntryCount =
        static_cast<uint32_t>(cache.pageDirectory.entries.size());
    update->directoryCoveredCount = cache.pageDirectory.coveredCount;
    update->directoryFrontierCount = cache.pageDirectory.frontierCount;
    update->advancedOwnershipGroupCount = advancedGroupCount;
    update->heldOwnershipGroupCount = heldGroupCount;
    update->directoryAbsentCount = static_cast<uint32_t>(std::count_if(
        cache.pageDirectory.entries.begin(),
        cache.pageDirectory.entries.end(),
        [](const WorldVoxelPageDirectoryEntry &entry)
        { return entry.state == WorldVoxelPageState::Absent; }));
    update->directoryPayloadRequestCount = static_cast<uint32_t>(
        finalDirectoryStreamingPlan.payloadRequests.size());
    update->directoryReadyInstallCount = static_cast<uint32_t>(
        finalDirectoryStreamingPlan.readyInstalls.size());
    update->directoryBlockedInstallCount = static_cast<uint32_t>(
        finalDirectoryStreamingPlan.blockedInstalls.size());
    update->directoryPlannedUninstallCount = static_cast<uint32_t>(
        finalDirectoryStreamingPlan.uninstalls.size());
    update->mapSamplingMilliseconds = samplingMilliseconds;
    update->shellExtractionMilliseconds = shellMilliseconds;
    update->hierarchyBuildMilliseconds = hierarchyMilliseconds;
    update->emittedShellVoxelCount = emittedShellVoxels;
    update->omittedInteriorVoxelCount = omittedInteriorVoxels;
    update->maximumUnexposedDepthVoxels = maximumUnexposedDepthVoxels;
    update->cpuCacheBytes = cache.payloadCacheBytes;
    update->gpuPageBytes = worldVoxelGPUPageBytes(cache);
    update->tree64BudgetBytes = settings.maximumTree64Bytes;
    update->physicalUploadPageCount =
        static_cast<uint32_t>(chunks.terrainPageUploads.size());
    update->hiddenPhysicalUploadPageCount =
        static_cast<uint32_t>(std::count_if(
            chunks.terrainPageUploads.begin(),
            chunks.terrainPageUploads.end(),
            [](const PlanetVoxelTerrainPageUpload &upload)
            {
              return !upload.visible;
            }));
    for (const auto &upload : chunks.terrainPageUploads)
      update->physicalUploadBytes +=
          upload.payloadBytes + upload.publicationBytes;
  }
  report(progress, "Generated-world progressive chunk transaction ready", 1.0);
  // A full-GPU transaction is deliberately metadata-only on the CPU. Its
  // arena capacities and page-build requests are the payload; an empty CPU
  // hierarchy is therefore success, not a failed terrain build.
  return settings.gpuOwnedPayloads || !chunks.bricks.empty();
}

bool planetVoxelTileAtOffset(const procedural::PlanetTileAddress &anchorTile, const int32_t tileOffsetX, const int32_t tileOffsetY, procedural::PlanetTileAddress &tile)
{
  if (!procedural::validPlanetTileAddress(anchorTile))
    return false;
  const double dimension = std::ldexp(1.0, anchorTile.level);
  const procedural::PlanetCubeCoordinate coordinate{
    static_cast<procedural::PlanetCubeFace>(anchorTile.face), (static_cast<double>(anchorTile.x) + tileOffsetX + 0.5) / dimension, (static_cast<double>(anchorTile.y) + tileOffsetY + 0.5) / dimension};
  procedural::SpherePoint direction{};
  return procedural::planetCubeDirection(coordinate, direction) && procedural::planetCubeTile(direction, anchorTile.level, tile);
}

bool updatePlanetVoxelChunkCache(
    const procedural::PlanetPackageReader &package,
    const procedural::PlanetTileAddress &anchorTile,
    const int32_t centerOffsetX,
    const int32_t centerOffsetY,
    const uint32_t chunkRadius,
    const PlanetVoxelChunkSettings &settings,
    PlanetVoxelChunkCache &cache,
    PlanetVoxelChunkSet &chunks,
    PlanetVoxelChunkCacheUpdate *update,
    std::string *error,
    const PlanetVoxelChunkProgress &progress,
    const std::atomic_bool *cancellation,
    const bool allowPhysicalLayoutGrowth,
    const std::vector<PlanetVoxelChunkRefinementRequest> *refinementRequests)
{
  chunks = {};
  if (update != nullptr)
    *update = {};
  if (error != nullptr)
    error->clear();
  if (cancellationRequested(cancellation))
    return cancelled(error);
  const uint32_t supportedLODMask = settings.clipmapLevelCount >= 32u ? std::numeric_limits<uint32_t>::max() : (uint32_t{1u} << settings.clipmapLevelCount) - 1u;
  const uint32_t maximumLevelCount = settings.strategy == PlanetVoxelChunkStrategy::ClipmapRings ? kPlanetVoxelMaximumClipmapLevels : kPlanetVoxelMaximumLODLevels;
  if (!package.isOpen() || !procedural::validPlanetTileAddress(anchorTile) || chunkRadius > 8u || settings.clipmapLevelCount == 0u || settings.clipmapLevelCount > maximumLevelCount ||
      settings.clipmapLevelCount > static_cast<uint32_t>(anchorTile.level) + 1u || settings.requestedLODMask == 0u || (settings.requestedLODMask & ~supportedLODMask) != 0u ||
      (settings.strategy == PlanetVoxelChunkStrategy::ClipmapRings && settings.clipmapLevelCount > 1u && (chunkRadius == 0u || settings.clipmapInnerHoleRadius >= chunkRadius)))
    return fail(error, "Planet chunk cache settings are invalid");
  if (settings.horizontalVoxelCount < kSparseVoxelBrickSide || settings.horizontalVoxelCount % kSparseVoxelBrickSide != 0u)
    return fail(error, "A planet chunk cache needs a brick-aligned horizontal voxel count");
  if (settings.strategy == PlanetVoxelChunkStrategy::ClipmapRings && settings.clipmapResolutionDropInterval != 0u &&
      (settings.clipmapMinimumHorizontalVoxelCount < kSparseVoxelBrickSide || settings.clipmapMinimumHorizontalVoxelCount > settings.horizontalVoxelCount ||
       settings.clipmapMinimumHorizontalVoxelCount % kSparseVoxelBrickSide != 0u))
    return fail(
        error,
        "A clipmap resolution floor must be brick aligned and no larger "
        "than the finest page resolution");
  if (!cache.transientPages)
  {
    cache.transientPages = std::make_shared<TransientSparseVoxelPageStore>();
    if (!cache.transientPages->initialize({}, error))
      return false;
  }
  else if (!cache.transientPages->initialized())
  {
    if (!cache.transientPages->initialize({}, error))
      return false;
  }
  const auto transientStatisticsBefore = cache.transientPages->statistics();

  struct DesiredChunk
  {
    procedural::PlanetTileAddress tile{};
    int32_t offsetX = 0;
    int32_t offsetY = 0;
    uint32_t clipmapLOD = 0u;
    uint32_t physicalSlot = std::numeric_limits<uint32_t>::max();
    double worldCenterXMetres = 0.0;
    double worldCenterZMetres = 0.0;
    PlanetVoxelChunkSettings settings{};
  };
  std::vector<DesiredChunk> targetDesired;
  procedural::PlanetTileAddress centerTile{};
  if (!planetVoxelTileAtOffset(anchorTile, centerOffsetX, centerOffsetY, centerTile))
    return fail(error, "Could not resolve the planet cache center tile");
  const double fineEdgeMetres = settings.patchEdgeMetres > 0.0 ? settings.patchEdgeMetres : package.metadata().nominalRenderPatchEdgeMetres;
  const bool physicalCacheCompatible = !cache.entries.empty() && sameSettings(cache.settings, settings);
  const bool pageCacheCompatible = !cache.entries.empty() && samePageSettings(cache.settings, settings);
  const bool logicalCacheCompatible = pageCacheCompatible && cache.anchorTile == anchorTile;
  uint32_t progressivePendingChunkCount = 0u;
  if (settings.strategy == PlanetVoxelChunkStrategy::ProgressiveLOD)
  {
    const uint32_t gridSide = chunkRadius * 2u + 1u;
    const uint32_t gridChunkCount = gridSide * gridSide;
    if (settings.maximumResidentChunks != 0u && gridChunkCount > settings.maximumResidentChunks)
      return fail(error, "The fixed terrain chunk grid exceeds its resident instance budget");
    const uint32_t coarsestAdmittedLOD = highestSetBitIndex(settings.requestedLODMask);
    const auto resolutionForLOD = [&](const uint32_t lod, uint32_t &resolution)
    {
      if (lod >= 32u)
        return false;
      const uint32_t scale = uint32_t{1u} << lod;
      if (settings.horizontalVoxelCount % scale != 0u)
        return false;
      resolution = settings.horizontalVoxelCount / scale;
      return resolution >= kSparseVoxelBrickSide && resolution % kSparseVoxelBrickSide == 0u;
    };
    for (uint32_t lod = 0u; lod < settings.clipmapLevelCount; ++lod)
      if ((settings.requestedLODMask & (uint32_t{1u} << lod)) != 0u)
      {
        uint32_t ignoredResolution = 0u;
        if (!resolutionForLOD(lod, ignoredResolution))
          return fail(
              error,
              "A requested terrain LOD is not representable by the fixed "
              "brick-aligned chunk resolution");
      }
    const auto positiveModulo = [](const int64_t value, const uint32_t divisor)
    {
      const int64_t signedDivisor = static_cast<int64_t>(divisor);
      const int64_t remainder = value % signedDivisor;
      return static_cast<uint32_t>(remainder < 0 ? remainder + signedDivisor : remainder);
    };
    const auto physicalSlotFor = [&](const int32_t offsetX, const int32_t offsetY)
    {
      const uint32_t x = positiveModulo(static_cast<int64_t>(centerOffsetX) + offsetX, gridSide);
      const uint32_t y = positiveModulo(static_cast<int64_t>(centerOffsetY) + offsetY, gridSide);
      return y * gridSide + x;
    };
    const auto pageSettingsForLOD = [&](const uint32_t lod)
    {
      PlanetVoxelChunkSettings pageSettings = settings;
      uint32_t resolution = settings.horizontalVoxelCount;
      (void)resolutionForLOD(lod, resolution);
      pageSettings.horizontalVoxelCount = resolution;
      // Every LOD owns the same world-space chunk footprint. Only the internal
      // heightmap/voxel resolution changes when the slot is rewritten.
      pageSettings.patchEdgeMetres = fineEdgeMetres;
      pageSettings.clipmapLevelCount = 1u;
      pageSettings.requestedLODMask = 1u;
      return pageSettings;
    };

    targetDesired.reserve(gridChunkCount);
    for (int32_t offsetY = -static_cast<int32_t>(chunkRadius); offsetY <= static_cast<int32_t>(chunkRadius); ++offsetY)
      for (int32_t offsetX = -static_cast<int32_t>(chunkRadius); offsetX <= static_cast<int32_t>(chunkRadius); ++offsetX)
      {
        procedural::PlanetTileAddress tile{};
        if (!planetVoxelTileAtOffset(centerTile, offsetX, offsetY, tile))
          return fail(error, "Could not resolve a toroidal planet terrain chunk");
        uint32_t lod = coarsestAdmittedLOD;
        if (logicalCacheCompatible)
        {
          const auto retained = std::find_if(
              cache.entries.begin(),
              cache.entries.end(),
              [&](const PlanetVoxelChunkCacheEntry &entry)
              {
                return entry.tile == tile;
              });
          if (retained != cache.entries.end())
            lod = retained->clipmapLOD;
        }
        targetDesired.push_back(
            {tile,
             offsetX,
             offsetY,
             lod,
             physicalSlotFor(offsetX, offsetY),
             static_cast<double>(centerOffsetX + offsetX) * fineEdgeMetres,
             static_cast<double>(centerOffsetY + offsetY) * fineEdgeMetres,
             pageSettingsForLOD(lod)});
      }

    struct RefinementCandidate
    {
      size_t desiredIndex = 0u;
      uint32_t nextLOD = 0u;
      uint32_t priority = 0u;
      double distanceSquared = 0.0;
    };
    std::vector<RefinementCandidate> candidates;
    if (logicalCacheCompatible && refinementRequests != nullptr)
    {
      const double cameraX = static_cast<double>(centerOffsetX) * fineEdgeMetres;
      const double cameraZ = static_cast<double>(centerOffsetY) * fineEdgeMetres;
      for (const auto &request : *refinementRequests)
      {
        const auto found = std::find_if(
            cache.entries.begin(),
            cache.entries.end(),
            [&](const PlanetVoxelChunkCacheEntry &entry)
            {
              return entry.clipmapSlot == request.clipmapSlot;
            });
        if (found == cache.entries.end())
          continue;
        const auto desired = std::find_if(
            targetDesired.begin(),
            targetDesired.end(),
            [&](const DesiredChunk &entry)
            {
              return entry.tile == found->tile;
            });
        if (desired == targetDesired.end())
          continue;
        uint32_t nextLOD = found->clipmapLOD;
        bool hasFinerLOD = false;
        while (nextLOD != 0u)
        {
          --nextLOD;
          if ((settings.requestedLODMask & (uint32_t{1u} << nextLOD)) != 0u)
          {
            hasFinerLOD = true;
            break;
          }
        }
        if (!hasFinerLOD)
          continue;
        const size_t desiredIndex = static_cast<size_t>(std::distance(targetDesired.begin(), desired));
        const double deltaX = found->worldCenterXMetres - cameraX;
        const double deltaZ = found->worldCenterZMetres - cameraZ;
        const auto duplicate = std::find_if(
            candidates.begin(),
            candidates.end(),
            [&](const RefinementCandidate &candidate)
            {
              return candidate.desiredIndex == desiredIndex;
            });
        if (duplicate == candidates.end())
          candidates.push_back({desiredIndex, nextLOD, request.priority, deltaX * deltaX + deltaZ * deltaZ});
        else
          duplicate->priority = std::max(duplicate->priority, request.priority);
      }
    }
    std::sort(
        candidates.begin(),
        candidates.end(),
        [&](const RefinementCandidate &left, const RefinementCandidate &right)
        {
          const uint32_t leftLOD = cache.entries[left.desiredIndex].clipmapLOD;
          const uint32_t rightLOD = cache.entries[right.desiredIndex].clipmapLOD;
          // A child cannot be requested until its parent is resident, so LOD
          // dependencies are already satisfied. Within that frontier the GPU
          // importance wins, then camera distance, then stable page order.
          if (left.priority != right.priority)
            return left.priority > right.priority;
          if (left.distanceSquared != right.distanceSquared)
            return left.distanceSquared < right.distanceSquared;
          if (leftLOD != rightLOD)
            return leftLOD > rightLOD;
          return left.desiredIndex < right.desiredIndex;
        });
    const uint32_t parentBudget = settings.maximumGeneratedChunksPerUpdate == 0u ? static_cast<uint32_t>(candidates.size()) : settings.maximumGeneratedChunksPerUpdate;
    const uint32_t selectedParentCount = std::min(parentBudget, static_cast<uint32_t>(candidates.size()));
    progressivePendingChunkCount = static_cast<uint32_t>(candidates.size()) - selectedParentCount;
    for (uint32_t candidateIndex = 0u; candidateIndex < selectedParentCount; ++candidateIndex)
    {
      auto &selected = targetDesired[candidates[candidateIndex].desiredIndex];
      selected.clipmapLOD = candidates[candidateIndex].nextLOD;
      selected.settings = pageSettingsForLOD(selected.clipmapLOD);
    }
  }
  else
  {
    uint32_t firstLOD = 0u;
    uint32_t endLOD = settings.clipmapLevelCount;
    if (settings.strategy == PlanetVoxelChunkStrategy::ProgressiveLOD)
    {
      firstLOD = highestSetBitIndex(settings.requestedLODMask);
      endLOD = firstLOD + 1u;
    }
    for (uint32_t clipmapLOD = firstLOD; clipmapLOD < endLOD; ++clipmapLOD)
    {
      if ((settings.requestedLODMask & (uint32_t{1u} << clipmapLOD)) == 0u)
        continue;
      if (cancellationRequested(cancellation))
        return cancelled(error);
      const uint32_t scale = 1u << clipmapLOD;
      procedural::PlanetTileAddress levelCenter = centerTile;
      levelCenter.level = static_cast<uint8_t>(centerTile.level - clipmapLOD);
      levelCenter.x >>= clipmapLOD;
      levelCenter.y >>= clipmapLOD;
      const double childCenterX = static_cast<double>(centerTile.x % scale) + 0.5;
      const double childCenterY = static_cast<double>(centerTile.y % scale) + 0.5;
      const double ancestorCenter = static_cast<double>(scale) * 0.5;
      const double levelCenterWorldX = (static_cast<double>(centerOffsetX) + ancestorCenter - childCenterX) * fineEdgeMetres;
      const double levelCenterWorldZ = (static_cast<double>(centerOffsetY) + ancestorCenter - childCenterY) * fineEdgeMetres;
      PlanetVoxelChunkSettings levelSettings = settings;
      levelSettings.patchEdgeMetres = fineEdgeMetres * scale;
      if (settings.clipmapResolutionDropInterval != 0u)
      {
        const uint32_t reductions = clipmapLOD / settings.clipmapResolutionDropInterval;
        uint32_t resolution = settings.horizontalVoxelCount;
        for (uint32_t reduction = 0u; reduction < reductions && resolution > settings.clipmapMinimumHorizontalVoxelCount; ++reduction)
          resolution = std::max(resolution / 2u, settings.clipmapMinimumHorizontalVoxelCount);
        levelSettings.horizontalVoxelCount = resolution;
      }
      levelSettings.clipmapLevelCount = 1u;
      levelSettings.requestedLODMask = 1u;
      const double levelEdgeMetres = levelSettings.patchEdgeMetres;
      for (int32_t offsetY = -static_cast<int32_t>(chunkRadius); offsetY <= static_cast<int32_t>(chunkRadius); ++offsetY)
        for (int32_t offsetX = -static_cast<int32_t>(chunkRadius); offsetX <= static_cast<int32_t>(chunkRadius); ++offsetX)
        {
          if (settings.strategy == PlanetVoxelChunkStrategy::ClipmapRings && clipmapLOD != 0u && std::abs(offsetX) <= static_cast<int32_t>(settings.clipmapInnerHoleRadius) &&
              std::abs(offsetY) <= static_cast<int32_t>(settings.clipmapInnerHoleRadius))
            continue;
          procedural::PlanetTileAddress tile{};
          if (!planetVoxelTileAtOffset(levelCenter, offsetX, offsetY, tile))
            return fail(error, "Could not resolve a planet terrain page");
          targetDesired.push_back({tile, offsetX, offsetY, clipmapLOD, std::numeric_limits<uint32_t>::max(), levelCenterWorldX + offsetX * levelEdgeMetres, levelCenterWorldZ + offsetY * levelEdgeMetres, levelSettings});
        }
    }
  }
  const auto findCachedIn = [&](const DesiredChunk &request) -> PlanetVoxelChunkCacheEntry *
  {
    if (!logicalCacheCompatible)
      return nullptr;
    const auto found = std::find_if(
        cache.entries.begin(),
        cache.entries.end(),
        [&](const PlanetVoxelChunkCacheEntry &entry)
        {
          return entry.clipmapLOD == request.clipmapLOD && entry.tile == request.tile;
        });
    return found != cache.entries.end() ? &*found : nullptr;
  };

  // Progressive slots form a non-overlapping fixed-footprint grid. Internal
  // LOD rewrites preserve that ownership, so no fragment ownership mask or
  // transition blend exists.
  std::vector<DesiredChunk> desired = targetDesired;
  uint32_t pendingChunkCount = progressivePendingChunkCount;
  uint32_t publishedLOD = 0u;
  for (const auto &request : targetDesired)
    publishedLOD = std::max(publishedLOD, request.clipmapLOD);

  const auto findCached = [&](const DesiredChunk &request) -> PlanetVoxelChunkCacheEntry *
  {
    return findCachedIn(request);
  };

  std::vector<std::optional<PlanetVoxelChunkCacheEntry>> producedEntries(desired.size());
  struct VoxelJob
  {
    size_t index = 0u;
    uint32_t lod = 0u;
    double distanceSquared = 0.0;
  };
  struct LowerVoxelPriority
  {
    bool operator()(const VoxelJob &left, const VoxelJob &right) const
    {
      if (left.lod != right.lod)
        return left.lod < right.lod;
      if (left.distanceSquared != right.distanceSquared)
        return left.distanceSquared > right.distanceSquared;
      return left.index > right.index;
    }
  };
  std::priority_queue<VoxelJob, std::vector<VoxelJob>, LowerVoxelPriority> voxelJobs;
  std::vector<size_t> voxelJobIndices;
  voxelJobIndices.reserve(desired.size());
  std::vector<double> pageProgress(desired.size(), 0.0);
  std::mutex progressMutex;
  double lastAggregateProgress = 0.0;
  const auto updatePageProgress = [&](const size_t index, const char *phase, const double fraction)
  {
    if (!progress)
      return;
    std::lock_guard<std::mutex> lock(progressMutex);
    pageProgress[index] = std::max(pageProgress[index], std::clamp(fraction, 0.0, 1.0));
    double completed = 0.0;
    for (const double value : pageProgress)
      completed += value;
    const double aggregate = std::max(lastAggregateProgress, 0.9 * completed / desired.size());
    lastAggregateProgress = aggregate;
    const std::string labelledPhase = "Chunk " + std::to_string(index + 1u) + "/" + std::to_string(desired.size()) + ": " + (phase != nullptr ? phase : "");
    progress(labelledPhase.c_str(), aggregate);
  };
  uint32_t reusedCount = 0u;
  uint32_t restoredMemoryCount = 0u;
  uint32_t restoredDiskCount = 0u;
  for (size_t index = 0u; index < desired.size(); ++index)
  {
    if (cancellationRequested(cancellation))
      return cancelled(error);
    const auto &request = desired[index];
    if (findCached(request) != nullptr)
    {
      ++reusedCount;
      updatePageProgress(index, "Reusing resident terrain page", 1.0);
      continue;
    }
    PlanetVoxelChunk chunk{};
    std::vector<uint8_t> transientPayload;
    std::string transientError;
    const auto transientSource = cache.transientPages->get(planetTransientPageKey(package.metadata(), request.settings, request.tile), transientPayload, &transientError);
    if (transientSource != TransientSparseVoxelPageSource::Missing)
    {
      if (deserializePlanetVoxelTransientPage(transientPayload, chunk, &transientError) && chunk.requestedTile == request.tile)
      {
        if (transientSource == TransientSparseVoxelPageSource::Memory)
          ++restoredMemoryCount;
        else
          ++restoredDiskCount;
        producedEntries[index].emplace(PlanetVoxelChunkCacheEntry{request.offsetX, request.offsetY, request.clipmapLOD, 0u, request.worldCenterXMetres, request.worldCenterZMetres, request.tile, std::move(chunk)});
        updatePageProgress(index, transientSource == TransientSparseVoxelPageSource::Memory ? "Restoring compressed terrain page from memory" : "Restoring compressed terrain page from transient disk", 1.0);
        continue;
      }
      cache.transientPages->erase(planetTransientPageKey(package.metadata(), request.settings, request.tile));
    }
    const double cameraX = static_cast<double>(centerOffsetX) * fineEdgeMetres;
    const double cameraZ = static_cast<double>(centerOffsetY) * fineEdgeMetres;
    const double deltaX = request.worldCenterXMetres - cameraX;
    const double deltaZ = request.worldCenterZMetres - cameraZ;
    voxelJobs.push({index, request.clipmapLOD, deltaX * deltaX + deltaZ * deltaZ});
    voxelJobIndices.push_back(index);
  }

  const uint32_t availableWorkers = std::max(1u, std::thread::hardware_concurrency());
  const uint32_t requestedWorkers = settings.workerCount == 0u ? recommendedPlanetVoxelWorkerCount(availableWorkers) : std::clamp(settings.workerCount, 1u, 64u);
  const uint32_t workerCount = std::min<uint32_t>(requestedWorkers, static_cast<uint32_t>(voxelJobs.size()));
  std::vector<std::string> voxelErrors(desired.size());
  std::mutex voxelJobMutex;
  const auto runVoxelWorker = [&]
  {
    for (;;)
    {
      if (cancellationRequested(cancellation))
        return;
      size_t index = 0u;
      {
        std::lock_guard<std::mutex> lock(voxelJobMutex);
        if (voxelJobs.empty())
          return;
        index = voxelJobs.top().index;
        voxelJobs.pop();
      }
      const auto &request = desired[index];
      PlanetVoxelChunk chunk{};
      const auto chunkProgress = [&](const char *phase, const double fraction)
      {
        updatePageProgress(index, phase, fraction);
      };
      if (buildPlanetVoxelChunk(package, request.tile, request.settings, chunk, &voxelErrors[index], chunkProgress, cancellation))
      {
        producedEntries[index].emplace(PlanetVoxelChunkCacheEntry{request.offsetX, request.offsetY, request.clipmapLOD, 0u, request.worldCenterXMetres, request.worldCenterZMetres, request.tile, std::move(chunk)});
      }
    }
  };
  std::vector<std::thread> voxelWorkers;
  if (workerCount > 1u)
  {
    voxelWorkers.reserve(workerCount - 1u);
    for (uint32_t worker = 1u; worker < workerCount; ++worker)
      voxelWorkers.emplace_back(runVoxelWorker);
  }
  if (workerCount != 0u)
    runVoxelWorker();
  for (auto &worker : voxelWorkers)
    worker.join();
  if (cancellationRequested(cancellation))
    return cancelled(error);
  for (const size_t index : voxelJobIndices)
    if (!producedEntries[index])
      return fail(error, "Terrain voxel workgroup failed for page " + std::to_string(index + 1u) + ": " + voxelErrors[index]);

  // Only publish the replacement window after every missing page either
  // restores or generates successfully. At that point pages leaving RAM may
  // safely enter the disposable compressed tier.
  if (pageCacheCompatible)
  {
    for (const auto &entry : cache.entries)
    {
      const bool remainsResident = std::any_of(
          desired.begin(),
          desired.end(),
          [&](const DesiredChunk &request)
          {
            return entry.clipmapLOD == request.clipmapLOD && entry.tile == request.tile;
          });
      if (remainsResident)
        continue;
      const std::vector<uint8_t> payload = serializePlanetVoxelTransientPage(entry.chunk);
      PlanetVoxelChunkSettings entrySettings = cache.settings;
      entrySettings.patchEdgeMetres = entry.chunk.edgeMetres;
      // Fixed-footprint progressive slots vary their internal resolution.
      // Transient identity must use the resolution that produced this page,
      // otherwise a toroidally evicted coarse chunk cannot be found when the
      // camera walks back into it.
      if (settings.strategy == PlanetVoxelChunkStrategy::ProgressiveLOD && entry.chunk.voxelSizeMetres > 0.0)
        entrySettings.horizontalVoxelCount = static_cast<uint32_t>(std::llround(entry.chunk.edgeMetres / entry.chunk.voxelSizeMetres));
      entrySettings.clipmapLevelCount = 1u;
      entrySettings.requestedLODMask = 1u;
      std::string ignoredCacheError;
      cache.transientPages->put(planetTransientPageKey(package.metadata(), entrySettings, entry.tile), payload, &ignoredCacheError);
    }
  }

  std::array<std::vector<uint32_t>, kPlanetVoxelMaximumClipmapLevels> availableSlots;
  const bool reusePhysicalSlots = physicalCacheCompatible && desired.size() == cache.entries.size();
  if (reusePhysicalSlots && settings.strategy != PlanetVoxelChunkStrategy::ProgressiveLOD)
  {
    for (const auto &entry : cache.entries)
    {
      const bool remainsResident = logicalCacheCompatible && std::any_of(
                                                                 desired.begin(),
                                                                 desired.end(),
                                                                 [&](const DesiredChunk &request)
                                                                 {
                                                                   return entry.clipmapLOD == request.clipmapLOD && entry.tile == request.tile;
                                                                 });
      if (!remainsResident)
        availableSlots[entry.clipmapLOD].push_back(entry.clipmapSlot);
    }
  }

  std::vector<PlanetVoxelChunkCacheEntry> nextEntries;
  nextEntries.reserve(desired.size());
  uint32_t nextInitialSlot = 0u;
  const bool reusableSlotCapacities = reusePhysicalSlots && cache.clipmapSlotCount == cache.entries.size() && cache.clipmapSlotNodeCapacity.size() == cache.clipmapSlotCount &&
                                      cache.clipmapSlotChildCapacity.size() == cache.clipmapSlotCount && cache.clipmapSlotRootCapacity.size() == cache.clipmapSlotCount &&
                                      cache.clipmapSlotMaterialCapacity.size() == cache.clipmapSlotCount && cache.clipmapSlotPaletteCapacity.size() == cache.clipmapSlotCount;
  for (size_t index = 0u; index < desired.size(); ++index)
  {
    const auto &request = desired[index];
    if (auto *entry = findCached(request); entry != nullptr)
    {
      entry->tileOffsetX = request.offsetX;
      entry->tileOffsetY = request.offsetY;
      entry->worldCenterXMetres = request.worldCenterXMetres;
      entry->worldCenterZMetres = request.worldCenterZMetres;
      if (settings.strategy == PlanetVoxelChunkStrategy::ProgressiveLOD)
        entry->clipmapSlot = request.physicalSlot;
      else if (!reusePhysicalSlots)
        entry->clipmapSlot = nextInitialSlot++;
      nextEntries.push_back(std::move(*entry));
      continue;
    }
    if (!producedEntries[index])
      return fail(error, "A built planet cache chunk could not be published");
    if (settings.strategy == PlanetVoxelChunkStrategy::ProgressiveLOD)
    {
      if (request.physicalSlot >= desired.size())
        return fail(error, "A toroidal terrain slot is outside its fixed grid");
      producedEntries[index]->clipmapSlot = request.physicalSlot;
    }
    else if (reusePhysicalSlots)
    {
      auto &lodSlots = availableSlots[producedEntries[index]->clipmapLOD];
      if (lodSlots.empty())
        return fail(error, "Planet clipmap has no physical slot for an entering page");
      auto selected = lodSlots.end();
      if (reusableSlotCapacities)
      {
        const auto &bricks = producedEntries[index]->chunk.bricks;
        selected = std::min_element(
            lodSlots.begin(),
            lodSlots.end(),
            [&](const uint32_t left, const uint32_t right)
            {
              const auto fits = [&](const uint32_t slot)
              {
                return bricks.nodes.size() <= cache.clipmapSlotNodeCapacity[slot] && bricks.childIndices.size() <= cache.clipmapSlotChildCapacity[slot] &&
                       bricks.rootIndices.size() <= cache.clipmapSlotRootCapacity[slot] && bricks.materialIndices.size() <= cache.clipmapSlotMaterialCapacity[slot] &&
                       bricks.palette.size() <= cache.clipmapSlotPaletteCapacity[slot];
              };
              const bool leftFits = fits(left);
              const bool rightFits = fits(right);
              if (leftFits != rightFits)
                return leftFits;
              const uint64_t leftCapacity = static_cast<uint64_t>(cache.clipmapSlotNodeCapacity[left]) + cache.clipmapSlotChildCapacity[left] + cache.clipmapSlotRootCapacity[left] +
                                            cache.clipmapSlotMaterialCapacity[left] + cache.clipmapSlotPaletteCapacity[left];
              const uint64_t rightCapacity = static_cast<uint64_t>(cache.clipmapSlotNodeCapacity[right]) + cache.clipmapSlotChildCapacity[right] + cache.clipmapSlotRootCapacity[right] +
                                             cache.clipmapSlotMaterialCapacity[right] + cache.clipmapSlotPaletteCapacity[right];
              return leftCapacity < rightCapacity;
            });
      }
      if (selected == lodSlots.end())
        selected = std::prev(lodSlots.end());
      producedEntries[index]->clipmapSlot = *selected;
      lodSlots.erase(selected);
    }
    else
    {
      producedEntries[index]->clipmapSlot = nextInitialSlot++;
    }
    nextEntries.push_back(std::move(*producedEntries[index]));
  }
  if (reusePhysicalSlots && settings.strategy != PlanetVoxelChunkStrategy::ProgressiveLOD)
    for (const auto &lodSlots : availableSlots)
      if (!lodSlots.empty())
        return fail(error, "Planet clipmap physical slot count changed unexpectedly");

  const auto guardedPayloadCapacity = [](const uint32_t required)
  {
    // Toroidal slots must accommodate terrain that was not resident when the
    // graph was created. Reserve one complete current page of growth instead
    // of the old 25% allowance; mountain pages routinely differ by more than
    // that from plains and otherwise force a full arena remap while moving.
    const uint64_t guarded = static_cast<uint64_t>(required) + std::max<uint64_t>(required, 16u);
    return static_cast<uint32_t>(std::min<uint64_t>(guarded, std::numeric_limits<uint32_t>::max()));
  };
  const auto guardedTopologyCapacity = [](const uint32_t required)
  {
    // Nodes, child links, and roots are much smaller than the per-voxel
    // material/micro-surface payload. Keep three observed pages in each
    // topology slot. A two-page reserve was only 16 nodes short when moving
    // from plains to the stress-test mountain region, which needlessly forced
    // the complete graph-owned arena through its synchronous remap path.
    const uint64_t guarded = static_cast<uint64_t>(required) + std::max<uint64_t>(static_cast<uint64_t>(required) * 2u, 64u);
    return static_cast<uint32_t>(std::min<uint64_t>(guarded, std::numeric_limits<uint32_t>::max()));
  };
  const auto alignedPageCapacity = [&](const uint32_t required, const uint32_t alignment)
  {
    const uint64_t guarded = guardedPayloadCapacity(required);
    const uint64_t aligned = (guarded + alignment - 1u) & ~(static_cast<uint64_t>(alignment) - 1u);
    const uint64_t maximumAligned = std::numeric_limits<uint32_t>::max() & ~(static_cast<uint64_t>(alignment) - 1u);
    return static_cast<uint32_t>(std::min(aligned, maximumAligned));
  };
  const uint32_t slotCount = static_cast<uint32_t>(nextEntries.size());
  bool layoutChanged = !reusableSlotCapacities || cache.clipmapSlotCount != slotCount;
  if (layoutChanged)
  {
    cache.clipmapSlotNodeCapacity.assign(slotCount, 0u);
    cache.clipmapSlotChildCapacity.assign(slotCount, 0u);
    cache.clipmapSlotRootCapacity.assign(slotCount, 0u);
    cache.clipmapSlotMaterialCapacity.assign(slotCount, 0u);
    cache.clipmapSlotPaletteCapacity.assign(slotCount, 0u);

    if (settings.strategy == PlanetVoxelChunkStrategy::ProgressiveLOD)
    {
      const uint32_t finestAdmittedLOD = lowestSetBitIndex(settings.requestedLODMask);
      const uint32_t finestResolution = settings.horizontalVoxelCount >> finestAdmittedLOD;
      const auto ceilDivide = [](const uint64_t value, const uint64_t divisor)
      {
        return (value + divisor - 1u) / divisor;
      };
      // Derive the arena from the complete representable heightfield volume,
      // not from the content of the first coarse page. A mountainous fine page
      // can occupy far more vertical cells than its coarse sample, so scaling
      // observed counts is not a capacity contract. At hierarchy level L all
      // occupied cells lie inside the full-resolution X/Y/Z box divided by
      // 2^L. The extra cell/brick on each axis accounts for the centred signed
      // X/Z grid crossing an alignment boundary. Summing through the last
      // possible brick level bounds nodes and material entries for every
      // heightmap the generator can emit at this resolution.
      uint64_t maximumNodes = 0u;
      uint64_t maximumMaterials = 0u;
      uint64_t scale = 1u;
      for (uint32_t level = 0u; level < 32u; ++level)
      {
        const uint64_t dividedXZ = ceilDivide(finestResolution, scale);
        const uint64_t dividedY = ceilDivide(settings.maximumVerticalVoxels, scale);
        const uint64_t cellsXZ = dividedXZ + 1u;
        const uint64_t cellsY = dividedY + 1u;
        maximumMaterials += cellsXZ * cellsXZ * cellsY;
        const uint64_t bricksXZ = ceilDivide(cellsXZ, kSparseVoxelBrickSide) + 1u;
        const uint64_t bricksY = ceilDivide(cellsY, kSparseVoxelBrickSide) + 1u;
        maximumNodes += bricksXZ * bricksXZ * bricksY;
        if (dividedXZ <= 1u && dividedY <= 1u)
          break;
        scale <<= 1u;
      }
      const uint64_t maximumChildren = maximumNodes * 8u;
      const uint64_t maximumRoots = maximumNodes;
      const auto boundedCapacity = [&](const uint64_t required, const uint32_t alignment)
      {
        const uint64_t aligned = alignment == 1u ? required : (required + alignment - 1u) & ~(static_cast<uint64_t>(alignment) - 1u);
        const uint64_t maximumAligned = alignment == 1u ? std::numeric_limits<uint32_t>::max() : std::numeric_limits<uint32_t>::max() & ~(static_cast<uint64_t>(alignment) - 1u);
        return static_cast<uint32_t>(std::min(aligned, maximumAligned));
      };
      for (const auto &entry : nextEntries)
      {
        const uint32_t slot = entry.clipmapSlot;
        const auto &bricks = entry.chunk.bricks;
        cache.clipmapSlotNodeCapacity[slot] = boundedCapacity(std::max<uint64_t>(maximumNodes, bricks.nodes.size()), 1u);
        cache.clipmapSlotChildCapacity[slot] = boundedCapacity(std::max<uint64_t>(maximumChildren, bricks.childIndices.size()), 1u);
        cache.clipmapSlotRootCapacity[slot] = boundedCapacity(std::max<uint64_t>(maximumRoots, bricks.rootIndices.size()), 1u);
        cache.clipmapSlotMaterialCapacity[slot] = boundedCapacity(std::max<uint64_t>(maximumMaterials, bricks.materialIndices.size()), 4u);
        cache.clipmapSlotPaletteCapacity[slot] = boundedCapacity(std::max<uint64_t>(std::max<uint32_t>(bricks.paletteStride, 1u), bricks.palette.size()), 2u);
      }
    }
    else
    {

      // Every toroidal slot is exchanged only with pages from its own clipmap
      // LOD. Size all slots in that ring from the densest initially observed
      // page, with the growth reserve above, so movement does not depend on
      // which particular plain/mountain happened to occupy a physical slot at
      // startup.
      struct LODMaximums
      {
        uint32_t nodes = 0u;
        uint32_t children = 0u;
        uint32_t roots = 0u;
        uint32_t materials = 0u;
        uint32_t palette = 0u;
      };
      std::array<LODMaximums, kPlanetVoxelMaximumClipmapLevels> maximums{};
      for (const auto &entry : nextEntries)
      {
        auto &maximum = maximums[std::min<uint32_t>(entry.clipmapLOD, kPlanetVoxelMaximumClipmapLevels - 1u)];
        maximum.nodes = std::max<uint32_t>(maximum.nodes, entry.chunk.bricks.nodes.size());
        maximum.children = std::max<uint32_t>(maximum.children, entry.chunk.bricks.childIndices.size());
        maximum.roots = std::max<uint32_t>(maximum.roots, entry.chunk.bricks.rootIndices.size());
        maximum.materials = std::max<uint32_t>(maximum.materials, entry.chunk.bricks.materialIndices.size());
        maximum.palette = std::max<uint32_t>(maximum.palette, entry.chunk.bricks.palette.size());
      }
      for (const auto &entry : nextEntries)
      {
        const uint32_t slot = entry.clipmapSlot;
        const auto &maximum = maximums[std::min<uint32_t>(entry.clipmapLOD, kPlanetVoxelMaximumClipmapLevels - 1u)];
        cache.clipmapSlotNodeCapacity[slot] = guardedTopologyCapacity(maximum.nodes);
        cache.clipmapSlotChildCapacity[slot] = guardedTopologyCapacity(maximum.children);
        cache.clipmapSlotRootCapacity[slot] = guardedTopologyCapacity(maximum.roots);
        cache.clipmapSlotMaterialCapacity[slot] = alignedPageCapacity(maximum.materials, 4u);
        cache.clipmapSlotPaletteCapacity[slot] = alignedPageCapacity(maximum.palette, 2u);
      }
    }
  }
  cache.clipmapSlotCount = slotCount;
  bool physicalLayoutCapacityExceeded = false;
  std::string physicalLayoutCapacityError;
  for (const auto &entry : nextEntries)
  {
    if (entry.clipmapSlot >= slotCount)
      return fail(error, "Planet clipmap physical slot is outside its arena");
    const uint32_t slot = entry.clipmapSlot;
    const auto grow = [&](const char *resource, std::vector<uint32_t> &capacities, const uint32_t required, const bool topology, const uint32_t alignment = 1u)
    {
      if (required <= capacities[slot])
        return;
      if (!allowPhysicalLayoutGrowth)
      {
        physicalLayoutCapacityExceeded = true;
        if (physicalLayoutCapacityError.empty())
          physicalLayoutCapacityError = "Planet terrain slot " + std::to_string(slot) + " LOD " + std::to_string(entry.clipmapLOD) + " needs " + std::to_string(required) + " " + resource +
                                        " entries but its fixed capacity is " + std::to_string(capacities[slot]);
        return;
      }
      capacities[slot] = alignment == 1u ? topology ? guardedTopologyCapacity(required) : guardedPayloadCapacity(required) : alignedPageCapacity(required, alignment);
      layoutChanged = true;
    };
    grow("node", cache.clipmapSlotNodeCapacity, static_cast<uint32_t>(entry.chunk.bricks.nodes.size()), true);
    grow("child-index", cache.clipmapSlotChildCapacity, static_cast<uint32_t>(entry.chunk.bricks.childIndices.size()), true);
    grow("root-index", cache.clipmapSlotRootCapacity, static_cast<uint32_t>(entry.chunk.bricks.rootIndices.size()), true);
    grow("material", cache.clipmapSlotMaterialCapacity, static_cast<uint32_t>(entry.chunk.bricks.materialIndices.size()), false, 4u);
    grow("palette", cache.clipmapSlotPaletteCapacity, static_cast<uint32_t>(entry.chunk.bricks.palette.size()), false, 2u);
  }
  if (physicalLayoutCapacityExceeded)
    return fail(error, physicalLayoutCapacityError);
  if (layoutChanged)
    ++cache.clipmapLayoutRevision;
  std::sort(
      nextEntries.begin(),
      nextEntries.end(),
      [](const PlanetVoxelChunkCacheEntry &left, const PlanetVoxelChunkCacheEntry &right)
      {
        return left.clipmapSlot < right.clipmapSlot;
      });

  const uint32_t previousCount = static_cast<uint32_t>(cache.entries.size());
  const bool keepWorldOrigin = logicalCacheCompatible && cache.hasWorldElevationOrigin;
  const double worldOrigin = cache.worldElevationOriginMetres;
  cache.anchorTile = anchorTile;
  cache.centerOffsetX = centerOffsetX;
  cache.centerOffsetY = centerOffsetY;
  cache.chunkRadius = chunkRadius;
  cache.settings = settings;
  cache.entries = std::move(nextEntries);
  cache.pendingChunkCount = pendingChunkCount;
  cache.requestedChunkCount = static_cast<uint32_t>(targetDesired.size());
  const auto cachedCenterEntry = std::find_if(
      cache.entries.begin(),
      cache.entries.end(),
      [&](const PlanetVoxelChunkCacheEntry &entry)
      {
        return entry.clipmapLOD == 0u && entry.tile == centerTile;
      });
  auto referenceCenterEntry = cachedCenterEntry;
  if (referenceCenterEntry == cache.entries.end())
  {
    const double cameraX = static_cast<double>(centerOffsetX) * fineEdgeMetres;
    const double cameraZ = static_cast<double>(centerOffsetY) * fineEdgeMetres;
    referenceCenterEntry = std::min_element(
        cache.entries.begin(),
        cache.entries.end(),
        [&](const PlanetVoxelChunkCacheEntry &left, const PlanetVoxelChunkCacheEntry &right)
        {
          if (left.clipmapLOD != right.clipmapLOD)
            return left.clipmapLOD < right.clipmapLOD;
          const auto distanceSquared = [&](const PlanetVoxelChunkCacheEntry &entry)
          {
            const double x = entry.worldCenterXMetres - cameraX;
            const double z = entry.worldCenterZMetres - cameraZ;
            return x * x + z * z;
          };
          return distanceSquared(left) < distanceSquared(right);
        });
  }
  if (referenceCenterEntry == cache.entries.end())
    return fail(error, "Planet cache has no terrain page after publication");
  if (keepWorldOrigin)
  {
    cache.worldElevationOriginMetres = worldOrigin;
    cache.hasWorldElevationOrigin = true;
  }
  else
  {
    cache.worldElevationOriginMetres = referenceCenterEntry->chunk.elevationOriginMetres;
    cache.hasWorldElevationOrigin = true;
  }
  if (update != nullptr)
  {
    update->workerCount = workerCount;
    update->reusedChunkCount = reusedCount;
    update->restoredMemoryChunkCount = restoredMemoryCount;
    update->restoredDiskChunkCount = restoredDiskCount;
    update->builtChunkCount = static_cast<uint32_t>(desired.size()) - reusedCount - restoredMemoryCount - restoredDiskCount;
    update->evictedChunkCount = previousCount > reusedCount ? previousCount - reusedCount : 0u;
    const auto transientStatisticsAfter = cache.transientPages->statistics();
    update->spilledChunkCount = static_cast<uint32_t>(transientStatisticsAfter.diskSpills - transientStatisticsBefore.diskSpills);
    update->queuedChunkCount = static_cast<uint32_t>(desired.size()) - reusedCount;
    update->pendingChunkCount = pendingChunkCount;
    update->publishedLOD = publishedLOD;
    update->refinementComplete = pendingChunkCount == 0u;
  }

  report(progress, "Packing resident terrain chunks", 0.92);
  if (cancellationRequested(cancellation))
    return cancelled(error);
  chunks.centerTile = centerTile;
  chunks.elevationOriginMetres = cache.worldElevationOriginMetres;
  const auto centerEntry = referenceCenterEntry;
  chunks.edgeMetres = fineEdgeMetres;
  chunks.voxelSizeMetres = fineEdgeMetres / static_cast<double>(settings.horizontalVoxelCount);
  chunks.centerSurfaceMetres = centerEntry->chunk.centerSurfaceMetres;
  chunks.centerMaterial = centerEntry->chunk.centerMaterial;
  chunks.minimumWorldMetres = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
  chunks.maximumWorldMetres = {std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
  uint64_t packedNodeCount = 0u;
  uint64_t packedChildCount = 0u;
  uint64_t packedRootCount = 0u;
  uint64_t packedMaterialCount = 0u;
  uint64_t packedPaletteCount = 0u;
  for (const auto &entry : cache.entries)
  {
    if (cancellationRequested(cancellation))
      return cancelled(error);
    packedNodeCount += cache.clipmapSlotNodeCapacity[entry.clipmapSlot];
    packedChildCount += cache.clipmapSlotChildCapacity[entry.clipmapSlot];
    packedRootCount += cache.clipmapSlotRootCapacity[entry.clipmapSlot];
    packedMaterialCount += cache.clipmapSlotMaterialCapacity[entry.clipmapSlot];
    packedPaletteCount += cache.clipmapSlotPaletteCapacity[entry.clipmapSlot];
  }
  if (packedNodeCount > std::numeric_limits<uint32_t>::max() || packedChildCount > std::numeric_limits<uint32_t>::max() || packedRootCount > std::numeric_limits<uint32_t>::max() ||
      packedMaterialCount > std::numeric_limits<uint32_t>::max() || packedPaletteCount > std::numeric_limits<uint32_t>::max())
    return fail(error, "Planet clipmap physical layout exceeds 32-bit indexing");
  chunks.clipmapLayoutRevision = cache.clipmapLayoutRevision;
  chunks.bricks.nodes.resize(static_cast<size_t>(packedNodeCount));
  chunks.bricks.occupancy.resize(static_cast<size_t>(packedNodeCount));
  chunks.bricks.childIndices.resize(static_cast<size_t>(packedChildCount));
  chunks.bricks.rootIndices.resize(static_cast<size_t>(packedRootCount));
  chunks.bricks.materialIndices.resize(static_cast<size_t>(packedMaterialCount));
  chunks.bricks.microSurfaceData.resize(static_cast<size_t>(packedMaterialCount));
  chunks.bricks.palette.resize(static_cast<size_t>(packedPaletteCount));
  chunks.bricks.paletteDescriptorIds.resize(static_cast<size_t>(packedPaletteCount));
  chunks.chunks.reserve(cache.entries.size());
  chunks.materialFieldRegions.reserve(cache.entries.size());
  // One shared side across resident pages is what lets the packed rasters be
  // addressed as a plain slice per chunk. Every page is built from the same
  // horizontalVoxelCount and halo, so a disagreement means a stale entry from
  // an older settings generation and the whole array is left empty rather than
  // published with mismatched strides.
  chunks.surfaceHeightSide = 0u;
  chunks.surfaceHeightHalo = 0u;
  chunks.surfaceHeights.clear();
  chunks.surfaceEnvelope.clear();
  chunks.surfaceMaterialSide = 0u;
  chunks.surfaceMaterials.clear();
  bool surfaceHeightsConsistent = !cache.entries.empty();
  for (const auto &entry : cache.entries)
  {
    const auto &chunk = entry.chunk;
    const uint32_t materialSide = chunk.surfaceHeightSide - 2u * chunk.surfaceHeightHalo;
    if (chunk.surfaceHeightSide == 0u || chunk.surfaceHeights.size() != static_cast<size_t>(chunk.surfaceHeightSide) * chunk.surfaceHeightSide || chunk.surfaceEnvelope.size() != chunk.surfaceHeights.size() ||
        chunk.surfaceMaterials.size() != static_cast<size_t>(materialSide) * materialSide * 2u)
    {
      surfaceHeightsConsistent = false;
      break;
    }
    if (chunks.surfaceHeightSide == 0u)
    {
      chunks.surfaceHeightSide = chunk.surfaceHeightSide;
      chunks.surfaceHeightHalo = chunk.surfaceHeightHalo;
      chunks.surfaceMaterialSide = materialSide;
    }
    else if (chunks.surfaceHeightSide != chunk.surfaceHeightSide || chunks.surfaceHeightHalo != chunk.surfaceHeightHalo)
    {
      surfaceHeightsConsistent = false;
      break;
    }
  }
  if (surfaceHeightsConsistent)
  {
    chunks.surfaceHeights.reserve(cache.entries.size() * static_cast<size_t>(chunks.surfaceHeightSide) * chunks.surfaceHeightSide);
    chunks.surfaceMaterials.reserve(cache.entries.size() * static_cast<size_t>(chunks.surfaceMaterialSide) * chunks.surfaceMaterialSide * 2u);
  }
  else
  {
    chunks.surfaceHeightSide = 0u;
    chunks.surfaceHeightHalo = 0u;
    chunks.surfaceMaterialSide = 0u;
  }
  uint32_t nextNodeBase = 0u;
  uint32_t nextChildBase = 0u;
  uint32_t nextRootBase = 0u;
  uint32_t nextMaterialBase = 0u;
  uint32_t nextPaletteBase = 0u;
  std::array<std::array<float, 4>, kPlanetVoxelMaximumClipmapLevels> finerClipBounds{};
  if (settings.strategy == PlanetVoxelChunkStrategy::ClipmapRings)
    for (uint32_t lod = 1u; lod < settings.clipmapLevelCount; ++lod)
    {
      double minimumX = std::numeric_limits<double>::max();
      double minimumZ = std::numeric_limits<double>::max();
      double maximumX = std::numeric_limits<double>::lowest();
      double maximumZ = std::numeric_limits<double>::lowest();
      for (const auto &entry : cache.entries)
      {
        if (entry.clipmapLOD >= lod)
          continue;
        const double halfEdge = entry.chunk.edgeMetres * 0.5;
        minimumX = std::min(minimumX, entry.worldCenterXMetres - halfEdge);
        minimumZ = std::min(minimumZ, entry.worldCenterZMetres - halfEdge);
        maximumX = std::max(maximumX, entry.worldCenterXMetres + halfEdge);
        maximumZ = std::max(maximumZ, entry.worldCenterZMetres + halfEdge);
      }
      if (minimumX < maximumX && minimumZ < maximumZ)
        finerClipBounds[lod] = {static_cast<float>(minimumX), static_cast<float>(minimumZ), static_cast<float>(maximumX), static_cast<float>(maximumZ)};
    }
  const auto mixPageRevision = [](uint64_t revision, const uint64_t value)
  {
    revision ^= value + 0x9e3779b97f4a7c15ull + (revision << 6u) + (revision >> 2u);
    return revision;
  };
  for (const auto &entry : cache.entries)
  {
    const auto &chunk = entry.chunk;
    const std::array<float, 3> translation{static_cast<float>(entry.worldCenterXMetres), static_cast<float>(chunk.elevationOriginMetres - chunks.elevationOriginMetres), static_cast<float>(entry.worldCenterZMetres)};
    const uint32_t spatialScale = 1u << entry.clipmapLOD;
    const double nodeScaleValue = entry.chunk.voxelSizeMetres / chunks.voxelSizeMetres;
    const uint32_t canonicalScale = static_cast<uint32_t>(std::llround(nodeScaleValue));
    if (canonicalScale == 0u || std::abs(nodeScaleValue - static_cast<double>(canonicalScale)) > 1.0e-6)
      return fail(error, "Planet clipmap voxel scale is not canonical");
    const auto canonicalCell = [&](const double metres)
    {
      return static_cast<int32_t>(std::llround(metres / chunks.voxelSizeMetres));
    };
    const std::array<int32_t, 3> canonicalTranslation{
      canonicalCell(entry.worldCenterXMetres),
      canonicalCell(chunk.elevationOriginMetres - chunks.elevationOriginMetres),
      canonicalCell(entry.worldCenterZMetres),
    };
    const uint32_t regionMappingId = planetVoxelMaterialRegionMappingId(chunk.requestedTile);
    // Progressive chunks keep one fixed world footprint while their local
    // resolution changes. Multiplying their local coordinates by
    // canonicalScale already returns them to the finest grid, so the region
    // extent itself remains one full-resolution chunk (not a clipmap-sized
    // parent page).
    const bool fixedFootprint = settings.strategy == PlanetVoxelChunkStrategy::ProgressiveLOD;
    const int64_t canonicalHalfSide = static_cast<int64_t>(settings.horizontalVoxelCount / 2u) * (fixedFootprint ? 1u : spatialScale);
    const uint64_t canonicalSide = static_cast<uint64_t>(settings.horizontalVoxelCount) * (fixedFootprint ? 1u : spatialScale);
    if (canonicalSide == 0u || canonicalSide > std::numeric_limits<uint32_t>::max() || static_cast<int64_t>(canonicalTranslation[0]) - canonicalHalfSide < std::numeric_limits<int32_t>::min() ||
        static_cast<int64_t>(canonicalTranslation[0]) + canonicalHalfSide > std::numeric_limits<int32_t>::max() ||
        static_cast<int64_t>(canonicalTranslation[2]) - canonicalHalfSide < std::numeric_limits<int32_t>::min() || static_cast<int64_t>(canonicalTranslation[2]) + canonicalHalfSide > std::numeric_limits<int32_t>::max())
      return fail(error, "Planet material-field canonical page exceeds 32 bits");
    PlanetVoxelMaterialFieldRegion materialFieldRegion{};
    if (!generatePlanetVoxelMaterialFieldRegion(
            package.metadata(),
            chunk.requestedTile,
            {static_cast<int32_t>(static_cast<int64_t>(canonicalTranslation[0]) - canonicalHalfSide), static_cast<int32_t>(static_cast<int64_t>(canonicalTranslation[2]) - canonicalHalfSide)},
            {static_cast<uint32_t>(canonicalSide), static_cast<uint32_t>(canonicalSide)},
            materialFieldRegion,
            error))
      return false;
    if (materialFieldRegion.regionMappingId != regionMappingId)
      return fail(error, "Planet material-field region identity changed during packing");
    chunks.materialFieldRegions.push_back(std::move(materialFieldRegion));
    const uint32_t brickCount = static_cast<uint32_t>(chunk.bricks.occupancy.size());
    const uint32_t nodeCapacity = cache.clipmapSlotNodeCapacity[entry.clipmapSlot];
    const uint32_t materialCapacity = cache.clipmapSlotMaterialCapacity[entry.clipmapSlot];
    const uint32_t childCapacity = cache.clipmapSlotChildCapacity[entry.clipmapSlot];
    const uint32_t rootCapacity = cache.clipmapSlotRootCapacity[entry.clipmapSlot];
    const uint32_t paletteCapacity = cache.clipmapSlotPaletteCapacity[entry.clipmapSlot];
    const uint32_t nodeBase = nextNodeBase;
    const uint32_t childBase = nextChildBase;
    const uint32_t rootBase = nextRootBase;
    const uint32_t materialBase = nextMaterialBase;
    const uint32_t paletteBase = nextPaletteBase;
    nextNodeBase += nodeCapacity;
    nextChildBase += childCapacity;
    nextRootBase += rootCapacity;
    nextMaterialBase += materialCapacity;
    nextPaletteBase += paletteCapacity;
    uint64_t nodeRevision = 0u;
    const uint32_t refinementPageSlot = fixedFootprint ? entry.clipmapSlot : kSparseVoxelBrickInvalidIndex;
    if (!writeTranslatedHierarchySlot(
            chunks.bricks,
            chunk.bricks,
            translation,
            canonicalTranslation,
            canonicalScale,
            nullptr,
            regionMappingId,
            finerClipBounds[entry.clipmapLOD],
            entry.clipmapLOD,
            !fixedFootprint,
            refinementPageSlot,
            nodeBase,
            nodeCapacity,
            childBase,
            childCapacity,
            rootBase,
            rootCapacity,
            materialBase,
            materialCapacity,
            paletteBase,
            paletteCapacity,
            &nodeRevision,
            error))
      return false;
    chunks.bricks.rootCount +=
        static_cast<uint32_t>(chunk.bricks.rootIndices.size());
    uint64_t payloadRevision = 0xcbf29ce484222325ull;
    payloadRevision = mixPageRevision(payloadRevision, chunk.requestedTile.face);
    payloadRevision = mixPageRevision(payloadRevision, chunk.requestedTile.level);
    payloadRevision = mixPageRevision(payloadRevision, chunk.requestedTile.x);
    payloadRevision = mixPageRevision(payloadRevision, chunk.requestedTile.y);
    payloadRevision = mixPageRevision(payloadRevision, chunk.sourceTile.face);
    payloadRevision = mixPageRevision(payloadRevision, chunk.sourceTile.level);
    payloadRevision = mixPageRevision(payloadRevision, chunk.sourceTile.x);
    payloadRevision = mixPageRevision(payloadRevision, chunk.sourceTile.y);
    payloadRevision = mixPageRevision(payloadRevision, chunk.bricks.nodes.size());
    payloadRevision = mixPageRevision(payloadRevision, chunk.bricks.childIndices.size());
    payloadRevision = mixPageRevision(payloadRevision, chunk.bricks.rootIndices.size());
    payloadRevision = mixPageRevision(payloadRevision, chunk.bricks.materialIndices.size());
    for (const uint64_t surfaceData : chunk.bricks.microSurfaceData)
      payloadRevision = mixPageRevision(payloadRevision, surfaceData);
    payloadRevision = mixPageRevision(payloadRevision, chunk.bricks.palette.size());
    for (const uint16_t descriptorId : chunk.bricks.paletteDescriptorIds)
      payloadRevision = mixPageRevision(payloadRevision, descriptorId);

    // Tree64 consumes occupancy and spatial ownership, never physical brick
    // addresses or material payloads. Hash that semantic source before it is
    // translated into the current arena. A layout growth may move every
    // brickNodeOffset, but retained pages then keep this revision and reuse
    // their already-built Tree64 model.
    uint64_t tree64Revision = 0xcbf29ce484222325ull;
    const auto hashTreeBytes = [&](const void *data, const size_t byteCount)
    {
      const auto *bytes = static_cast<const uint8_t *>(data);
      for (size_t byte = 0u; byte < byteCount; ++byte)
      {
        tree64Revision ^= bytes[byte];
        tree64Revision *= 0x100000001b3ull;
      }
    };
    for (const auto &node : chunk.bricks.nodes)
    {
      hashTreeBytes(node.minimumVoxelSize.data(), node.minimumVoxelSize.size() * sizeof(node.minimumVoxelSize[0]));
      const std::array<uint32_t, 8u> geometryFields{
        node.packedOccupiedBounds,
        node.coarseOccupancyLow,
        node.coarseOccupancyHigh,
        node.parentIndex,
        node.childCount,
        node.level,
        node.occupiedCount,
        node.firstChild,
      };
      hashTreeBytes(geometryFields.data(), geometryFields.size() * sizeof(geometryFields[0]));
    }
    if (!chunk.bricks.occupancy.empty())
      hashTreeBytes(chunk.bricks.occupancy.data(), chunk.bricks.occupancy.size() * sizeof(chunk.bricks.occupancy[0]));
    hashTreeBytes(translation.data(), translation.size() * sizeof(float));
    hashTreeBytes(finerClipBounds[entry.clipmapLOD].data(), finerClipBounds[entry.clipmapLOD].size() * sizeof(float));
    hashTreeBytes(&entry.clipmapLOD, sizeof(entry.clipmapLOD));
    if (tree64Revision == 0u)
      tree64Revision = 1u;
    const uint32_t brickRootCount = static_cast<uint32_t>(chunk.bricks.rootIndices.size());
    chunks.chunks.push_back(
        {chunk.requestedTile,
         chunk.sourceTile,
         entry.tileOffsetX,
         entry.tileOffsetY,
         entry.clipmapLOD,
         entry.clipmapSlot,
         regionMappingId,
         0.0f,
         entry.worldCenterXMetres,
         entry.worldCenterZMetres,
         chunk.edgeMetres,
         chunk.voxelSizeMetres,
         nodeBase,
         nodeCapacity,
         childBase,
         childCapacity,
         materialBase,
         materialCapacity,
         paletteBase,
         paletteCapacity,
         payloadRevision,
         nodeRevision,
         tree64Revision,
         rootBase,
         rootCapacity,
         brickRootCount,
         chunk.elevationOriginMetres,
         chunk.minimumSurfaceMetres,
         chunk.maximumSurfaceMetres,
         chunk.centerSurfaceMetres,
         chunk.centerMaterial,
         {},
         chunk.leafVoxelCount,
         brickCount});
    if (surfaceHeightsConsistent)
    {
      chunks.surfaceHeights.insert(chunks.surfaceHeights.end(), chunk.surfaceHeights.begin(), chunk.surfaceHeights.end());
      chunks.surfaceEnvelope.insert(chunks.surfaceEnvelope.end(), chunk.surfaceEnvelope.begin(), chunk.surfaceEnvelope.end());
      chunks.surfaceMaterials.insert(chunks.surfaceMaterials.end(), chunk.surfaceMaterials.begin(), chunk.surfaceMaterials.end());
    }
    chunks.leafVoxelCount += chunk.leafVoxelCount;
    const double centerX = entry.worldCenterXMetres;
    const double centerZ = entry.worldCenterZMetres;
    chunks.minimumWorldMetres[0] = std::min(chunks.minimumWorldMetres[0], centerX - chunk.edgeMetres * 0.5);
    chunks.maximumWorldMetres[0] = std::max(chunks.maximumWorldMetres[0], centerX + chunk.edgeMetres * 0.5);
    chunks.minimumWorldMetres[1] = std::min(chunks.minimumWorldMetres[1], chunk.elevationOriginMetres - chunks.elevationOriginMetres);
    chunks.maximumWorldMetres[1] = std::max(chunks.maximumWorldMetres[1], chunk.maximumSurfaceMetres - chunks.elevationOriginMetres + chunk.voxelSizeMetres);
    chunks.minimumWorldMetres[2] = std::min(chunks.minimumWorldMetres[2], centerZ - chunk.edgeMetres * 0.5);
    chunks.maximumWorldMetres[2] = std::max(chunks.maximumWorldMetres[2], centerZ + chunk.edgeMetres * 0.5);
  }
  if (cancellationRequested(cancellation))
    return cancelled(error);
  report(progress, "Terrain chunk cache ready", 1.0);
  return !chunks.bricks.empty();
}

bool buildPlanetVoxelChunkGrid(
    const procedural::PlanetPackageReader &package,
    const procedural::PlanetTileAddress &centerTile,
    const uint32_t chunkRadius,
    const PlanetVoxelChunkSettings &settings,
    PlanetVoxelChunkSet &chunks,
    std::string *error,
    const PlanetVoxelChunkProgress &progress,
    const std::atomic_bool *cancellation)
{
  PlanetVoxelChunkCache cache{};
  return updatePlanetVoxelChunkCache(package, centerTile, 0, 0, chunkRadius, settings, cache, chunks, nullptr, error, progress, cancellation);
}

} // namespace voxel::svraster
