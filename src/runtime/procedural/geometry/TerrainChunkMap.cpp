#include "procedural/geometry/TerrainChunkMap.hpp"
#include "procedural/geometry/TerrainMapSource.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace procedural::geometry
{
namespace
{
uint64_t mix(uint64_t n)
{
  n ^= n >> 30; n *= 0xbf58476d1ce4e5b9ull; n ^= n >> 27;
  n *= 0x94d049bb133111ebull; return n ^ (n >> 31);
}
// Value noise on a lattice, hashed from the integer cell so neighbouring chunks
// generate identical detail wherever they overlap the same ground. A chunk map
// that did not agree with its neighbour would show a seam at every boundary.
double valueNoise(const double x, const double y, const uint64_t salt)
{
  const auto hash = [salt](const int64_t a, const int64_t b)
  {
    const uint64_t n = mix(uint64_t(a) * 0x9E3779B97F4A7C15ull ^
        uint64_t(b) * 0xC2B2AE3D27D4EB4Full ^ salt);
    return double(n >> 11) / double(1ull << 53);
  };
  const double fx = std::floor(x), fy = std::floor(y);
  const auto ix = int64_t(fx), iy = int64_t(fy);
  const double tx = x - fx, ty = y - fy;
  const double sx = tx * tx * (3 - 2 * tx), sy = ty * ty * (3 - 2 * ty);
  const double a = hash(ix, iy), b = hash(ix + 1, iy);
  const double c = hash(ix, iy + 1), d = hash(ix + 1, iy + 1);
  return (a + (b - a) * sx) * (1 - sy) + (c + (d - c) * sx) * sy;
}
double ridged(const double x, const double y, const uint64_t salt, const uint32_t octaves)
{
  double sum = 0, amplitude = 1, frequency = 1, total = 0;
  for (uint32_t k = 0; k < octaves; ++k)
  {
    // Ridged rather than plain fBm: the absolute value folds the field, which
    // makes creases and spurs instead of dunes. Plain fBm on a hillside reads
    // as noise; this reads as small-scale relief.
    const double v = 1.0 - std::abs(valueNoise(x * frequency, y * frequency, salt + k) * 2.0 - 1.0);
    sum += (v * v - .5) * amplitude;
    total += amplitude;
    amplitude *= .5; frequency *= 2.03;
  }
  return total > 0 ? sum / total : 0.0;
}
}

ChunkMap buildChunkMap(const TerrainMapSource &source, const Point2 origin, const double sizeMetres,
    const ChunkMapSettings &settings)
{
  if (!(sizeMetres > 0) || !(settings.metresPerPixel > 0))
    throw std::invalid_argument("invalid chunk map extent");
  ChunkMap map;
  map.origin = origin;
  map.sizeMetres = sizeMetres;
  map.metresPerPixel = settings.metresPerPixel;
  map.side = uint32_t(std::llround(sizeMetres / settings.metresPerPixel));
  if (map.side < 2u) throw std::invalid_argument("chunk map is smaller than two pixels");
  const size_t count = size_t(map.side) * map.side;
  map.elevation.assign(count, 0.f);
  map.flow.assign(count, 0.f);
  map.water.assign(count, 0u);
  map.biome.assign(count, 0u);
  map.feature.assign(count, uint8_t(ChunkFeature::None));

  // --- the coarse signal, and what it cannot say -----------------------------
  // The source is sampled directly rather than interpolated from a cached
  // block: `height` already composes the eroded field, the rock formations, the
  // river carving and the shore transition, and all of those are continuous
  // functions. Asking it per pixel is what makes the chunk map a *refinement*
  // of the world rather than a separate one.
  const double pixel = settings.metresPerPixel;
  for (uint32_t y = 0; y < map.side; ++y)
    for (uint32_t x = 0; x < map.side; ++x)
    {
      const size_t id = size_t(y) * map.side + x;
      const Point2 p = map.positionOf(x, y);
      map.water[id] = source.waterType(p);
      map.biome[id] = source.biome(p);
      map.elevation[id] = float(map.water[id] ? source.waterLevel(p) : source.height(p));
    }

  // --- detail ---------------------------------------------------------------
  // Below 125 m the coarse map has nothing to say, so an upsample of it is
  // smooth by construction: more pixels carrying no more information. Detail is
  // added in proportion to how rough the ground already is - a mountainside
  // gets it, a floodplain barely does, and open water none at all.
  if (settings.detailAmplitudeMetres > 0 && settings.detailOctaves > 0)
  {
    std::vector<float> detailed = map.elevation;
    for (uint32_t y = 1; y + 1 < map.side; ++y)
      for (uint32_t x = 1; x + 1 < map.side; ++x)
      {
        const size_t id = size_t(y) * map.side + x;
        if (map.water[id]) continue;
        const double dx = (map.elevation[id + 1] - map.elevation[id - 1]) / (2 * pixel);
        const double dy = (map.elevation[id + map.side] - map.elevation[id - map.side]) / (2 * pixel);
        const double slope = std::hypot(dx, dy);
        const double gain = std::clamp(slope * settings.detailSlopeGain, .12, 1.0);
        const Point2 p = map.positionOf(x, y);
        detailed[id] = float(map.elevation[id] + settings.detailAmplitudeMetres * gain *
            ridged(p.x / settings.detailScaleMetres, p.y / settings.detailScaleMetres,
                0x6465746169ull, settings.detailOctaves));
      }
    map.elevation.swap(detailed);
  }

  // --- features, at their true width ----------------------------------------
  // This is the whole point of the chunk map. A road is seven metres wide and a
  // river channel thirty; at a metre a pixel both are dozens of pixels across,
  // and can be carved with a cross-section instead of being a line that tints
  // whatever cell it happens to fall in.
  if (settings.carveFeatures)
  {
    for (uint32_t y = 0; y < map.side; ++y)
      for (uint32_t x = 0; x < map.side; ++x)
      {
        const size_t id = size_t(y) * map.side + x;
        const Point2 p = map.positionOf(x, y);
        if (map.water[id]) continue;
        const auto wet = source.rivers.sample(p);
        if (wet.inside)
        {
          // The bed sits below the water surface, deepest at the thread of the
          // channel and rising to the banks - the cross-section the coarse
          // field could never hold.
          const double across = std::clamp(wet.lateral, -1.0, 1.0);
          map.elevation[id] = float(wet.level - wet.depth * (1.0 - across * across));
          map.feature[id] = uint8_t(ChunkFeature::River);
          map.flow[id] = 1.f;
          continue;
        }
        const double coast = source.coastDistance(p);
        if (coast < 60.0) map.feature[id] = uint8_t(ChunkFeature::Shore);
      }

    // Roads are carved as a graded ribbon: a flat running surface with a slight
    // camber, and a verge either side that meets the ground it crosses.
    for (const auto &road : source.settlementNetwork.roads)
    {
      const double half = std::max(1.5, road.widthMetres * .5);
      const double verge = half * 2.4;
      for (size_t k = 1; k < road.path.size(); ++k)
      {
        const Point2 a = road.path[k - 1], b = road.path[k];
        const double lowX = std::min(a.x, b.x) - verge, highX = std::max(a.x, b.x) + verge;
        const double lowY = std::min(a.y, b.y) - verge, highY = std::max(a.y, b.y) + verge;
        if (highX < origin.x || lowX > origin.x + sizeMetres ||
            highY < origin.y || lowY > origin.y + sizeMetres) continue;
        const auto x0 = uint32_t(std::clamp((lowX - origin.x) / pixel, 0.0, double(map.side - 1)));
        const auto x1 = uint32_t(std::clamp((highX - origin.x) / pixel, 0.0, double(map.side - 1)));
        const auto y0 = uint32_t(std::clamp((lowY - origin.y) / pixel, 0.0, double(map.side - 1)));
        const auto y1 = uint32_t(std::clamp((highY - origin.y) / pixel, 0.0, double(map.side - 1)));
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double span = dx * dx + dy * dy;
        if (span < 1e-9) continue;
        for (uint32_t y = y0; y <= y1; ++y)
          for (uint32_t x = x0; x <= x1; ++x)
          {
            const size_t id = size_t(y) * map.side + x;
            if (map.water[id] || map.feature[id] == uint8_t(ChunkFeature::River)) continue;
            const Point2 p = map.positionOf(x, y);
            const double t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / span, 0.0, 1.0);
            const Point2 on(a.x + t * dx, a.y + t * dy);
            const double distance = std::hypot(p.x - on.x, p.y - on.y);
            if (distance > verge) continue;
            // The running surface is level across the carriageway; outside it
            // the verge eases back to the ground in a smoothstep, so a road on
            // a slope has a cut on one side and a fill on the other.
            const double surface = source.height(on);
            if (distance <= half)
            {
              map.elevation[id] = float(surface - .15 * (distance / half) * (distance / half));
              map.feature[id] = uint8_t(ChunkFeature::Road);
            }
            else
            {
              const double u = (distance - half) / (verge - half);
              const double blend = u * u * (3.0 - 2.0 * u);
              map.elevation[id] = float(surface * (1.0 - blend) + map.elevation[id] * blend);
            }
          }
      }
    }
  }

  // --- thermal erosion at chunk resolution -----------------------------------
  // The stream-power solver runs at 250 m, where a cliff is a fraction of a
  // cell; measured, cutting a scarp before it changed the slope distribution
  // not at all. At a metre a pixel the angle of repose is meaningful again:
  // anything standing steeper than its material can hold sheds downslope and
  // piles at the foot, which is what puts talus under a cliff and rounds the
  // brink. Roads and river beds are held, since they are made ground.
  if (settings.thermalRounds > 0)
  {
    const double repose = std::tan(std::clamp(settings.reposeDegrees, 5.0, 60.0) * 3.14159265358979 / 180.0);
    const double limit = repose * pixel;
    std::vector<float> delta(count, 0.f);
    for (uint32_t round = 0; round < settings.thermalRounds; ++round)
    {
      std::fill(delta.begin(), delta.end(), 0.f);
      for (uint32_t y = 1; y + 1 < map.side; ++y)
        for (uint32_t x = 1; x + 1 < map.side; ++x)
        {
          const size_t id = size_t(y) * map.side + x;
          if (map.water[id]) continue;
          if (map.feature[id] == uint8_t(ChunkFeature::Road) ||
              map.feature[id] == uint8_t(ChunkFeature::River)) continue;
          const double here = map.elevation[id];
          const size_t around[4] = {id - 1, id + 1, id - map.side, id + map.side};
          double excess = 0, share[4] = {0, 0, 0, 0};
          for (int k = 0; k < 4; ++k)
          {
            const double fall = here - map.elevation[around[k]];
            if (fall > limit) { share[k] = fall - limit; excess += share[k]; }
          }
          if (!(excess > 0)) continue;
          const double move = excess * settings.thermalRate / 4.0;
          delta[id] -= float(move);
          for (int k = 0; k < 4; ++k)
            if (share[k] > 0) delta[around[k]] += float(move * (share[k] / excess));
        }
      for (size_t id = 0; id < count; ++id) map.elevation[id] += delta[id];
    }
  }

  // --- flow, for wetness and material ---------------------------------------
  // Not a drainage solve: a single steepest-descent accumulation pass over the
  // chunk, which is enough to say where water collects and therefore where the
  // ground is wet, gullied or bare. The real network is the coarse map's; this
  // only refines it inside the chunk.
  {
    std::vector<uint32_t> order(count);
    for (uint32_t i = 0; i < count; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](const uint32_t l, const uint32_t r)
    { return map.elevation[l] > map.elevation[r]; });
    std::vector<float> gathered(count, 1.f);
    for (const uint32_t id : order)
    {
      const uint32_t x = id % map.side, y = id / map.side;
      if (!x || !y || x + 1 >= map.side || y + 1 >= map.side) continue;
      uint32_t lowest = id; float best = map.elevation[id];
      for (const int32_t step : {-1, 1, -int32_t(map.side), int32_t(map.side)})
      {
        const auto next = uint32_t(int32_t(id) + step);
        if (map.elevation[next] < best) { best = map.elevation[next]; lowest = next; }
      }
      if (lowest != id) gathered[lowest] += gathered[id];
    }
    float peak = 1.f;
    for (const float v : gathered) peak = std::max(peak, v);
    const float inverse = 1.f / std::log2(peak + 1.f);
    for (size_t id = 0; id < count; ++id)
      map.flow[id] = std::max(map.flow[id], std::log2(gathered[id] + 1.f) * inverse);
  }
  return map;
}
}
