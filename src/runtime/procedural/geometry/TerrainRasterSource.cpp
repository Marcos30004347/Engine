#include "procedural/geometry/TerrainRasterSource.hpp"

#include "procedural/Geometry.hpp"
#include "procedural/geometry/TerrainMaterial.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <numeric>

namespace procedural::geometry
{

namespace
{

constexpr double kPi = 3.14159265358979323846;

double niceInterval(const double raw)
{
  if (!(raw > 0.0) || !std::isfinite(raw))
    return 50.0;
  const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
  const double normalised = raw / magnitude;
  double step = 10.0;
  for (const double candidate : {1.0, 2.0, 2.5, 5.0})
    if (normalised <= candidate)
    {
      step = candidate;
      break;
    }
  return std::max(0.5, step * magnitude);
}

// Joins loose segments into runs by shared endpoint, on a quantised lattice so
// two segments that meet actually meet.
std::vector<std::vector<Point2>> joinSegments(
    std::vector<std::pair<Point2, Point2>> segments, const double quantum)
{
  const auto key = [quantum](const Point2 &p)
  {
    return std::pair<int64_t, int64_t>(
        static_cast<int64_t>(std::llround(p.x / quantum)),
        static_cast<int64_t>(std::llround(p.y / quantum)));
  };
  std::multimap<std::pair<int64_t, int64_t>, size_t> index;
  for (size_t at = 0u; at < segments.size(); ++at)
  {
    index.emplace(key(segments[at].first), at);
    index.emplace(key(segments[at].second), at);
  }
  std::vector<uint8_t> used(segments.size(), 0u);
  std::vector<std::vector<Point2>> runs;
  for (size_t at = 0u; at < segments.size(); ++at)
  {
    if (used[at] != 0u)
      continue;
    used[at] = 1u;
    std::vector<Point2> run{segments[at].first, segments[at].second};
    for (bool grew = true; grew;)
    {
      grew = false;
      const auto endpoint = key(run.back());
      // Stop at a junction. Walking *through* a point where three or more
      // segments meet lets a run leave one contour and continue along another,
      // and the resampling that follows then cuts a straight line across
      // everything between them - which is how a coastline vertex ended up
      // pinned to sea level in the middle of 664 m of high ground.
      if (index.count(endpoint) != 2u)
        break;
      const auto range = index.equal_range(endpoint);
      for (auto entry = range.first; entry != range.second; ++entry)
      {
        if (used[entry->second] != 0u)
          continue;
        const auto &segment = segments[entry->second];
        const bool forward = key(segment.first) == endpoint;
        used[entry->second] = 1u;
        run.push_back(forward ? segment.second : segment.first);
        grew = true;
        break;
      }
    }
    if (run.size() >= 2u)
      runs.push_back(std::move(run));
  }
  return runs;
}

// Marching squares over a scalar sampled at cell corners.
template <typename Sample, typename Skip>
std::vector<std::pair<Point2, Point2>> marchingSquares(const uint32_t side,
    const double cell, const double level, const Sample &sample, const Skip &skip)
{
  std::vector<std::pair<Point2, Point2>> segments;
  const auto position = [cell](const double x, const double y)
  { return Point2(x * cell, y * cell); };
  for (uint32_t y = 0u; y + 1u < side; ++y)
    for (uint32_t x = 0u; x + 1u < side; ++x)
    {
      if (skip(x, y))
        continue;
      const double v00 = sample(x, y);
      const double v10 = sample(x + 1u, y);
      const double v11 = sample(x + 1u, y + 1u);
      const double v01 = sample(x, y + 1u);
      const uint32_t code = (v00 >= level ? 1u : 0u) | (v10 >= level ? 2u : 0u) |
          (v11 >= level ? 4u : 0u) | (v01 >= level ? 8u : 0u);
      if (code == 0u || code == 15u)
        continue;
      const auto lerp = [level](const double a, const double b)
      {
        const double denominator = b - a;
        return std::abs(denominator) < 1.0e-12
            ? 0.5
            : std::clamp((level - a) / denominator, 0.0, 1.0);
      };
      const Point2 bottom = position(x + lerp(v00, v10), y);
      const Point2 right = position(x + 1u, y + lerp(v10, v11));
      const Point2 top = position(x + lerp(v01, v11), y + 1u);
      const Point2 left = position(x, y + lerp(v00, v01));
      const auto push = [&segments](const Point2 &a, const Point2 &b)
      { segments.emplace_back(a, b); };
      switch (code)
      {
      case 1: case 14: push(left, bottom); break;
      case 2: case 13: push(bottom, right); break;
      case 3: case 12: push(left, right); break;
      case 4: case 11: push(right, top); break;
      case 6: case 9: push(bottom, top); break;
      case 7: case 8: push(left, top); break;
      // The ambiguous saddles. Both diagonals are emitted rather than a guess;
      // an extra edge is harmless to a constrained triangulation, a wrong one
      // closes the wrong region.
      case 5: push(left, bottom); push(right, top); break;
      case 10: push(bottom, right); push(left, top); break;
      default: break;
      }
    }
  return segments;
}

// Douglas-Peucker: drop a point only when leaving it out moves the line by less
// than the tolerance.
//
// Uniform arc-length resampling is wrong for a *traced* line. It walks a fixed
// step and cuts whatever corner lies between two samples, and on steep ground a
// small planar shortcut is a large vertical error - measured, a contour pinned
// to 350 m sitting on 505 m of hillside. Simplification keeps every corner and
// removes only what carries no shape.
void simplifyInto(const std::vector<Point2> &run, const size_t first,
    const size_t last, const double tolerance, std::vector<uint8_t> &keep)
{
  if (last <= first + 1u)
    return;
  const Point2 &a = run[first];
  const Point2 &b = run[last];
  const double dx = b.x - a.x;
  const double dz = b.y - a.y;
  const double lengthSquared = dx * dx + dz * dz;
  double worst = -1.0;
  size_t at = first;
  for (size_t index = first + 1u; index < last; ++index)
  {
    const Point2 &p = run[index];
    double deviation = 0.0;
    if (lengthSquared <= 0.0)
      deviation = distance(p, a);
    else
    {
      const double t =
          std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dz) / lengthSquared, 0.0, 1.0);
      deviation = distance(p, Point2(a.x + t * dx, a.y + t * dz));
    }
    if (deviation > worst)
    {
      worst = deviation;
      at = index;
    }
  }
  if (worst <= tolerance)
    return;
  keep[at] = 1u;
  simplifyInto(run, first, at, tolerance, keep);
  simplifyInto(run, at, last, tolerance, keep);
}

std::vector<Point2> simplifyRun(
    const std::vector<Point2> &run, const double tolerance)
{
  if (run.size() < 3u)
    return run;
  std::vector<uint8_t> keep(run.size(), 0u);
  keep.front() = 1u;
  keep.back() = 1u;
  simplifyInto(run, 0u, run.size() - 1u, tolerance, keep);
  std::vector<Point2> output;
  output.reserve(run.size());
  for (size_t at = 0u; at < run.size(); ++at)
    if (keep[at] != 0u)
      output.push_back(run[at]);
  return output;
}

constexpr auto kNeverSkip = [](uint32_t, uint32_t) { return false; };

} // namespace

bool RasterTerrain::valid() const
{
  return maps != nullptr && maps->width != 0u && maps->width == maps->height &&
      maps->heightmap.size() ==
          static_cast<size_t>(maps->width) * maps->height &&
      extentMetres > 0.0;
}

uint32_t RasterTerrain::side() const
{
  return maps != nullptr ? maps->width : 0u;
}

double RasterTerrain::cellMetres() const
{
  return side() > 1u ? extentMetres / static_cast<double>(side() - 1u) : extentMetres;
}

double RasterTerrain::height(const Point2 position) const
{
  if (!valid())
    return seaLevelMetres;
  const double cell = cellMetres();
  const double fx = std::clamp(position.x / cell, 0.0, static_cast<double>(side() - 1u));
  const double fy = std::clamp(position.y / cell, 0.0, static_cast<double>(side() - 1u));
  const auto x0 = static_cast<uint32_t>(fx);
  const auto y0 = static_cast<uint32_t>(fy);
  const uint32_t x1 = std::min(side() - 1u, x0 + 1u);
  const uint32_t y1 = std::min(side() - 1u, y0 + 1u);
  const double tx = fx - static_cast<double>(x0);
  const double ty = fy - static_cast<double>(y0);
  const auto at = [this](const uint32_t x, const uint32_t y)
  {
    return static_cast<double>(
               maps->heightmap[static_cast<size_t>(y) * maps->width + x]) /
        65535.0;
  };
  const double top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
  const double bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
  const double normalised = top + (bottom - top) * ty;
  return seaLevelMetres + normalised * reliefMetres;
}

namespace
{
size_t rasterIndex(const RasterTerrain &terrain, const Point2 position)
{
  const double cell = terrain.cellMetres();
  const auto x = static_cast<uint32_t>(std::clamp(
      std::llround(position.x / cell), 0ll,
      static_cast<long long>(terrain.side() - 1u)));
  const auto y = static_cast<uint32_t>(std::clamp(
      std::llround(position.y / cell), 0ll,
      static_cast<long long>(terrain.side() - 1u)));
  return static_cast<size_t>(y) * terrain.side() + x;
}
} // namespace

PolygonTerrainMaterial RasterTerrain::material(const Point2 position) const
{
  if (!valid() || maps->materials.empty())
    return PolygonTerrainMaterial::Grass;
  return static_cast<PolygonTerrainMaterial>(
      maps->materials[rasterIndex(*this, position)]);
}

uint8_t RasterTerrain::terrainForm(const Point2 position) const
{
  if (!valid() || maps->terrainForms.empty())
    return 1u;
  return maps->terrainForms[rasterIndex(*this, position)];
}

bool RasterTerrain::water(const Point2 position) const
{
  if (!valid())
    return true;
  return maps->heightmap[rasterIndex(*this, position)] == 0u;
}

uint8_t RasterTerrain::riverFlow(const Point2 position) const
{
  if (!valid() || maps->rivers.empty())
    return 0u;
  return maps->rivers[rasterIndex(*this, position)];
}

HeightFunction rasterHeightFunction(const RasterTerrain &terrain)
{
  return [terrain](const Point2 position) { return terrain.height(position); };
}

void flattenRasterOcean(const RasterTerrain &terrain, TerrainMesh &mesh)
{
  uint64_t vertices = 0;
  for (auto &tile : mesh.tiles)
  {
    std::map<uint32_t, uint32_t> waterVertices;
    for (size_t face = 0; face < tile.materials.size(); ++face)
    {
      const auto material = tile.materials[face];
      if (material >= mesh.palette.entries().size() ||
          mesh.palette.entries()[material].cover != world::Biome::Ocean) continue;
      for (size_t corner = 0; corner < 3; ++corner)
      {
        auto &index = tile.indices[face * 3 + corner];
        if (tile.vertices[index].y == terrain.seaLevelMetres) continue;
        const auto [it, inserted] = waterVertices.emplace(index, static_cast<uint32_t>(tile.vertices.size()));
        if (inserted)
        {
          auto vertex = tile.vertices[index];
          vertex.y = terrain.seaLevelMetres;
          tile.vertices.push_back(vertex);
        }
        index = it->second;
      }
    }
    vertices += tile.vertices.size();
  }
  mesh.statistics.vertexCount = vertices;
}

HeightFunction rasterRiverHeightFunction(
    const RasterTerrain &terrain, const ChannelNetwork &channels)
{
  struct Segment { ChannelVertex a, b; };
  struct Corridors
  {
    double cell = 1.0;
    std::vector<Segment> segments;
    std::map<std::pair<int64_t, int64_t>, std::vector<size_t>> buckets;
  };
  auto data = std::make_shared<Corridors>();
  data->cell = std::max(100.0, terrain.cellMetres() * 4.0);
  for (const auto &reach : channels.reaches)
    for (size_t index = 1; index < reach.vertices.size(); ++index)
    {
      const Segment segment{reach.vertices[index - 1], reach.vertices[index]};
      const double radius = std::max(segment.a.widthMetres, segment.b.widthMetres) * 1.5;
      const size_t id = data->segments.size();
      data->segments.push_back(segment);
      const auto cell = [&](double value) { return static_cast<int64_t>(std::floor(value / data->cell)); };
      for (int64_t y = cell(std::min(segment.a.position.y, segment.b.position.y) - radius);
           y <= cell(std::max(segment.a.position.y, segment.b.position.y) + radius); ++y)
        for (int64_t x = cell(std::min(segment.a.position.x, segment.b.position.x) - radius);
             x <= cell(std::max(segment.a.position.x, segment.b.position.x) + radius); ++x)
          data->buckets[{x, y}].push_back(id);
    }
  return [terrain, data](Point2 point)
  {
    const double ground = terrain.height(point);
    double carved = ground;
    const auto found = data->buckets.find({static_cast<int64_t>(std::floor(point.x / data->cell)),
        static_cast<int64_t>(std::floor(point.y / data->cell))});
    if (found == data->buckets.end()) return ground;
    for (const size_t id : found->second)
    {
      const auto &segment = data->segments[id];
      const double dx = segment.b.position.x - segment.a.position.x;
      const double dy = segment.b.position.y - segment.a.position.y;
      const double t = std::clamp(((point.x - segment.a.position.x) * dx +
          (point.y - segment.a.position.y) * dy) / std::max(1e-20, dx * dx + dy * dy), 0.0, 1.0);
      const double d = distance(point, Point2(segment.a.position.x + t * dx, segment.a.position.y + t * dy));
      const auto lerp = [t](double a, double b) { return a + (b - a) * t; };
      const double half = lerp(segment.a.widthMetres, segment.b.widthMetres) * 0.5;
      if (half <= 0.0 || d >= half * 3.0) continue;
      const double bed = lerp(segment.a.bedMetres, segment.b.bedMetres);
      const double surface = lerp(segment.a.surfaceMetres, segment.b.surfaceMetres);
      double height = 0.0;
      if (d <= half)
        height = bed + (surface - bed) * (d / half) * (d / half);
      else
      {
        const double u = (d - half) / (2.0 * half);
        height = surface + (ground - surface) * u * u * (3.0 - 2.0 * u);
      }
      carved = std::min(carved, height);
    }
    return carved;
  };
}

MaterialFunction rasterMaterialFunction(
    const RasterTerrain &terrain, MaterialPalette &palette)
{
  // Freeze the lookup before tile workers start: palette insertion is mutable
  // and must never happen inside the parallel material callback.
  std::array<std::array<uint8_t, 21>, 2> lookup{};
  for (size_t water = 0; water < lookup.size(); ++water)
    for (size_t material = 0; material < lookup[water].size(); ++material)
    {
      MaterialEntry entry{};
      entry.role = water ? SurfaceRole::Water : SurfaceRole::Ground;
      entry.cover = polygonMaterialToBiome(static_cast<PolygonTerrainMaterial>(material));
      entry.lithology = PlanetLithology::Sedimentary;
      lookup[water][material] = palette.add(entry);
    }
  return [terrain, lookup](const Point2 position) -> uint8_t
  {
    const size_t material = std::min<size_t>(20, static_cast<uint8_t>(terrain.material(position)));
    return lookup[terrain.water(position) ? 1 : 0][material];
  };
}

std::function<double(Point2)> rasterToleranceScale(
    const RasterTerrain &terrain, const RasterSamplingSettings &settings)
{
  // Distance to the nearest channel, in cells, by a two-pass chamfer over the
  // raster's own river field. Cheap, and it only has to be approximate: it
  // decides how hard to refine, not where anything goes.
  const uint32_t side = terrain.side();
  const size_t count = static_cast<size_t>(side) * side;
  auto distance = std::make_shared<std::vector<float>>(count, 1.0e9f);
  if (!terrain.maps->rivers.empty())
  {
    for (size_t at = 0u; at < count; ++at)
      if (terrain.maps->rivers[at] != 0u)
        (*distance)[at] = 0.0f;
    const auto relax = [&](const size_t at, const size_t from, const float cost)
    {
      const float candidate = (*distance)[from] + cost;
      if (candidate < (*distance)[at])
        (*distance)[at] = candidate;
    };
    for (uint32_t y = 0u; y < side; ++y)
      for (uint32_t x = 0u; x < side; ++x)
      {
        const size_t at = static_cast<size_t>(y) * side + x;
        if (x > 0u)
          relax(at, at - 1u, 1.0f);
        if (y > 0u)
          relax(at, at - side, 1.0f);
        if (x > 0u && y > 0u)
          relax(at, at - side - 1u, 1.41421356f);
      }
    for (uint32_t y = side; y-- > 0u;)
      for (uint32_t x = side; x-- > 0u;)
      {
        const size_t at = static_cast<size_t>(y) * side + x;
        if (x + 1u < side)
          relax(at, at + 1u, 1.0f);
        if (y + 1u < side)
          relax(at, at + side, 1.0f);
        if (x + 1u < side && y + 1u < side)
          relax(at, at + side + 1u, 1.41421356f);
      }
  }

  const double influence = settings.riverInfluenceMetres > 0.0
      ? settings.riverInfluenceMetres / std::max(1.0, terrain.cellMetres())
      : settings.riverInfluenceCells;
  return [terrain, settings, distance, side, influence](
             const Point2 position) -> double
  {
    double scale = settings.plainsTolerance;
    switch (terrain.terrainForm(position))
    {
    case 1u: scale = settings.plainsTolerance; break;
    case 2u: scale = settings.valleyTolerance; break;
    case 3u: scale = settings.hillTolerance; break;
    case 4u: scale = settings.mountainTolerance; break;
    case 5u: scale = settings.peakTolerance; break;
    default: break;
    }
    if (!distance->empty() && influence > 0.0)
    {
      const size_t at = rasterIndex(terrain, position);
      const double cells = (*distance)[at];
      if (cells < influence)
      {
        // Full strength on the channel, fading out over the influence band, so
        // the transition does not put a density step across the valley.
        const double t = 1.0 - cells / influence;
        scale = scale + (settings.riverToleranceScale - scale) * t * t;
      }
    }
    return scale;
  };
}

RasterChains extractRasterChains(
    const RasterTerrain &terrain, const RasterSamplingSettings &settings)
{
  RasterChains output{};
  if (!terrain.valid())
    return output;
  const uint32_t side = terrain.side();
  const double cell = terrain.cellMetres();
  const double weld = cell * 1e-7;
  // Keep all semantic runs. Removing short loops can erase an island, a biome
  // pocket, or a peak; collinear samples may be merged without moving an edge.
  const double simplifyTolerance = 0.0;

  // World border.
  {
    Chain border{};
    border.kind = ChainKind::WorldBorder;
    border.closed = true;
    const double extent = terrain.extentMetres;
    border.points = {Point2(0.0, 0.0), Point2(extent, 0.0), Point2(extent, extent),
        Point2(0.0, extent)};
    output.chains.push_back(std::move(border));
  }

  const auto heightAt = [&terrain](const uint32_t x, const uint32_t y)
  {
    return static_cast<double>(
        terrain.maps->heightmap[static_cast<size_t>(y) * terrain.side() + x]);
  };

  // Coastline: the raster reserves zero for water, so the shore is the
  // boundary of the positive region and needs no threshold of its own.
  for (const std::vector<Point2> &run :
      joinSegments(marchingSquares(side, cell, 0.5, heightAt, kNeverSkip), weld))
  {
    const std::vector<Point2> points = simplifyRun(run, simplifyTolerance);
    if (points.size() < 2u)
      continue;
    Chain chain{};
    chain.kind = ChainKind::Coast;
    chain.points = points;
    chain.elevationMetres = terrain.seaLevelMetres;
    chain.hasElevation = true;
    output.chains.push_back(std::move(chain));
    ++output.coastRuns;
  }

  // Contours, at an interval derived from the relief that exists.
  double highest = 0.0;
  for (const uint16_t sample : terrain.maps->heightmap)
    highest = std::max(highest, static_cast<double>(sample));
  const double reliefMetres = highest / 65535.0 * terrain.reliefMetres;
  const double interval = settings.contourIntervalMetres > 0.0
      ? settings.contourIntervalMetres
      : niceInterval(reliefMetres / std::max(1u, settings.contourBandTarget));
  output.contourIntervalMetres = interval;
  for (double level = interval; level < reliefMetres; level += interval)
  {
    const double raw = level / terrain.reliefMetres * 65535.0;
    for (const std::vector<Point2> &run :
        joinSegments(marchingSquares(side, cell, raw, heightAt, kNeverSkip), weld))
    {
      const std::vector<Point2> points = simplifyRun(run, simplifyTolerance);
      if (points.size() < 2u)
        continue;
      Chain chain{};
      chain.kind = ChainKind::Contour;
      chain.points = points;
      chain.elevationMetres = terrain.seaLevelMetres + level;
      chain.hasElevation = true;
      output.chains.push_back(std::move(chain));
      ++output.contourLines;
    }
  }

  if (settings.extractRegionBorders)
    for (const auto &edge : terrain.maps->regionEdges)
    {
      Chain chain{};
      chain.kind = ChainKind::RegionBorder;
      chain.points = {Point2(edge.ax, edge.ay), Point2(edge.bx, edge.by)};
      output.chains.push_back(std::move(chain));
      ++output.regionEdges;
    }

  // Cover boundaries, walked along the cell edges where the class changes.
  // Exact by construction - no thresholding a class field, which would invent
  // a boundary that is not where the generator put one.
  if (settings.extractBiomeBorders && !terrain.maps->materials.empty())
  {
    std::vector<std::pair<Point2, Point2>> borders;
    const auto classAt = [&terrain](const uint32_t x, const uint32_t y)
    { return terrain.maps->materials[static_cast<size_t>(y) * terrain.side() + x]; };
    for (uint32_t y = 0u; y < side; ++y)
      for (uint32_t x = 0u; x < side; ++x)
      {
        const uint8_t here = classAt(x, y);
        // The edge between this cell and the one to its right, and the one
        // below, each drawn once.
        // The shared edge between two cells, in cell-corner coordinates. Kept
        // non-negative: the half-offsets below zero fall outside the world and
        // the tile clip silently drops them.
        if (x + 1u < side && classAt(x + 1u, y) != here)
          borders.emplace_back(Point2((x + 0.5) * cell, std::max(0.0, (y - 0.5) * cell)),
              Point2(std::min(terrain.extentMetres, (x + 0.5) * cell),
                  std::min(terrain.extentMetres, (y + 0.5) * cell)));
        if (y + 1u < side && classAt(x, y + 1u) != here)
          borders.emplace_back(Point2(std::max(0.0, (x - 0.5) * cell), (y + 0.5) * cell),
              Point2(std::min(terrain.extentMetres, (x + 0.5) * cell),
                  std::min(terrain.extentMetres, (y + 0.5) * cell)));
      }
    for (const std::vector<Point2> &run : joinSegments(borders, weld))
    {
      // A cover boundary is a network of short pieces meeting at junctions,
      // not a long smooth line, so the run-length rule that suppresses
      // contour noise would erase it entirely.
      const std::vector<Point2> points = simplifyRun(run, simplifyTolerance);
      if (points.size() < 2u)
        continue;
      Chain chain{};
      chain.kind = ChainKind::RegionBorder;
      chain.points = points;
      output.chains.push_back(std::move(chain));
      ++output.biomeRuns;
    }
  }

  // Ridges: the generator already classifies peaks and mountains, and a crest
  // is where that class forms a thin connected line. Tracing the boundary of
  // the peak class gives the mesh an edge along the crest instead of triangles
  // straddling it and rounding the silhouette off.
  if (settings.extractRidges && !terrain.maps->terrainForms.empty())
  {
    const auto peakAt = [&terrain](const uint32_t x, const uint32_t y)
    {
      const uint8_t form =
          terrain.maps->terrainForms[static_cast<size_t>(y) * terrain.side() + x];
      return form >= 5u ? 1.0 : 0.0;
    };
    for (const std::vector<Point2> &run :
        joinSegments(marchingSquares(side, cell, 0.5, peakAt, kNeverSkip), weld))
    {
      const std::vector<Point2> points = simplifyRun(run, simplifyTolerance);
      if (points.size() < 2u)
        continue;
      Chain chain{};
      chain.kind = ChainKind::Ridge;
      chain.points = points;
      output.chains.push_back(std::move(chain));
      ++output.ridgeRuns;
    }
  }
  return output;
}

namespace
{
ChannelNetwork sourceChannels(const RasterTerrain &terrain, const StrokeSettings &strokes)
{
  ChannelNetwork result;
  const auto &edges = terrain.maps->riverEdges;
  std::map<uint32_t, size_t> outgoing;
  std::map<uint32_t, uint32_t> inflow;
  std::map<uint32_t, Point2> positions;
  std::map<uint32_t, double> heights;
  for (size_t index = 0; index < edges.size(); ++index)
  {
    const auto &edge = edges[index];
    outgoing[edge.from] = index;
    ++inflow[edge.to];
    positions[edge.from] = Point2(edge.fromX, edge.fromY);
    positions[edge.to] = Point2(edge.toX, edge.toY);
  }
  for (const auto &[id, point] : positions)
    heights[id] = terrain.height(point);
  // Topological propagation makes the sampled bed descend through every
  // confluence, even where raster interpolation rounded off the source cut.
  auto remaining = inflow;
  std::vector<uint32_t> ready;
  for (const auto &[id, point] : positions)
    if (remaining[id] == 0) ready.push_back(id);
  for (size_t cursor = 0; cursor < ready.size(); ++cursor)
  {
    const uint32_t id = ready[cursor];
    const auto next = outgoing.find(id);
    if (next == outgoing.end()) continue;
    const uint32_t to = edges[next->second].to;
    heights[to] = std::min(heights[to], heights[id]);
    if (--remaining[to] == 0) ready.push_back(to);
  }
  std::map<uint32_t, uint32_t> junctions;
  for (const auto &[id, count] : inflow)
    if (count > 1)
    {
      junctions[id] = static_cast<uint32_t>(result.nodes.size());
      ChannelNode node{};
      node.position = positions[id];
      node.surfaceMetres = heights[id];
      node.widthMetres = strokes.minimumWidthMetres;
      node.bedMetres = heights[id] - strokes.minimumDepthMetres;
      result.nodes.push_back(node);
    }
  const auto nodeFor = [&](uint32_t id)
  {
    const auto found = junctions.find(id);
    return found == junctions.end() ? kInvalidChannelNode : found->second;
  };
  for (const auto &edge : edges)
  {
    if (inflow[edge.from] == 1) continue;
    ChannelReach reach{};
    reach.upstreamNode = nodeFor(edge.from);
    uint32_t id = edge.from;
    double flow = edge.flow;
    for (size_t walked = 0; walked <= edges.size(); ++walked)
    {
      const auto next = outgoing.find(id);
      if (next != outgoing.end()) flow = edges[next->second].flow;
      ChannelVertex vertex{};
      vertex.position = positions[id];
      // The source stores contributing headwaters, not calibrated discharge.
      // Keep that topology/relative flow and use 1 m^3/s per source for the
      // preview's hydraulic width/depth law.
      vertex.dischargeCumecs = std::max(1.0, flow);
      vertex.widthMetres = std::max(strokes.minimumWidthMetres, strokes.widthMetres(vertex.dischargeCumecs));
      vertex.depthMetres = std::max(strokes.minimumDepthMetres, strokes.depthMetres(vertex.dischargeCumecs));
      vertex.surfaceMetres = heights[id];
      vertex.bedMetres = vertex.surfaceMetres - vertex.depthMetres;
      reach.vertices.push_back(vertex);
      if (next == outgoing.end() || (id != edge.from && inflow[id] > 1)) break;
      id = edges[next->second].to;
      ++result.carvedCells;
    }
    reach.downstreamNode = nodeFor(id);
    reach.mapCellCount = static_cast<uint32_t>(reach.vertices.size());
    const uint32_t index = static_cast<uint32_t>(result.reaches.size());
    if (reach.upstreamNode != kInvalidChannelNode)
    {
      auto &node = result.nodes[reach.upstreamNode];
      node.outgoing = index;
      node.widthMetres = reach.vertices.front().widthMetres;
      node.bedMetres = reach.vertices.front().bedMetres;
    }
    if (reach.downstreamNode != kInvalidChannelNode)
      result.nodes[reach.downstreamNode].incoming.push_back(index);
    result.reaches.push_back(std::move(reach));
  }
  return result;
}
} // namespace

ChannelNetwork resolveRasterChannels(
    const RasterTerrain &terrain,
    const RasterSamplingSettings &settings,
    const StrokeSettings &strokes)
{
  ChannelNetwork network{};
  if (!terrain.valid())
    return network;
  if (!terrain.maps->riverEdges.empty())
    return sourceChannels(terrain, strokes);
  const uint32_t side = terrain.side();
  const size_t count = static_cast<size_t>(side) * side;
  const double cell = terrain.cellMetres();

  // D8 receivers over the raster. Water is an outlet, so the descent always
  // terminates and no depression filling is needed for the channels we keep.
  std::vector<int64_t> receiver(count, -1);
  std::vector<double> elevation(count, 0.0);
  for (size_t at = 0u; at < count; ++at)
    elevation[at] = static_cast<double>(terrain.maps->heightmap[at]);

  for (uint32_t y = 0u; y < side; ++y)
    for (uint32_t x = 0u; x < side; ++x)
    {
      const size_t at = static_cast<size_t>(y) * side + x;
      if (elevation[at] <= 0.0)
        continue;
      double steepest = 0.0;
      int64_t best = -1;
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
        {
          if (dx == 0 && dy == 0)
            continue;
          const int64_t nx = static_cast<int64_t>(x) + dx;
          const int64_t ny = static_cast<int64_t>(y) + dy;
          if (nx < 0 || ny < 0 || nx >= side || ny >= side)
            continue;
          const size_t neighbour = static_cast<size_t>(ny) * side + static_cast<size_t>(nx);
          const double drop = (elevation[at] - elevation[neighbour]) /
              std::sqrt(static_cast<double>(dx * dx + dy * dy));
          if (drop > steepest)
          {
            steepest = drop;
            best = static_cast<int64_t>(neighbour);
          }
        }
      receiver[at] = best;
    }

  // Accumulation, in cells, by processing from high to low.
  std::vector<uint32_t> order(count);
  std::iota(order.begin(), order.end(), 0u);
  std::sort(order.begin(), order.end(),
      [&elevation](const uint32_t left, const uint32_t right)
      {
        if (elevation[left] != elevation[right])
          return elevation[left] > elevation[right];
        return left < right;
      });
  std::vector<double> accumulation(count, 1.0);
  std::vector<uint32_t> inflow(count, 0u);
  for (const uint32_t at : order)
  {
    if (elevation[at] <= 0.0 || receiver[at] < 0)
      continue;
    accumulation[static_cast<size_t>(receiver[at])] += accumulation[at];
  }
  for (size_t at = 0u; at < count; ++at)
    if (elevation[at] > 0.0 && receiver[at] >= 0 &&
        accumulation[at] >= settings.channelThresholdCells)
      ++inflow[static_cast<size_t>(receiver[at])];

  const auto isChannel = [&](const size_t at)
  {
    return elevation[at] > 0.0 && accumulation[at] >= settings.channelThresholdCells;
  };
  const auto positionOf = [cell, side](const size_t at)
  {
    return Point2(static_cast<double>(at % side) * cell,
        static_cast<double>(at / side) * cell);
  };
  // Discharge from accumulated cells: catchment area times a runoff depth, so
  // the width and depth laws are fed something with physical units rather than
  // a cell count.
  const double cellArea = cell * cell;
  const auto dischargeOf = [&](const size_t at)
  {
    const double areaSquareMetres = accumulation[at] * cellArea;
    return areaSquareMetres * 0.3 / (365.25 * 24.0 * 3600.0);
  };

  std::vector<uint32_t> nodeOfCell(count, kInvalidChannelNode);
  for (size_t at = 0u; at < count; ++at)
  {
    if (!isChannel(at) || inflow[at] < 2u)
      continue;
    ChannelNode node{};
    node.position = positionOf(at);
    node.surfaceMetres = terrain.height(node.position);
    const double flow = dischargeOf(at);
    node.widthMetres =
        std::max(strokes.minimumWidthMetres, strokes.widthMetres(flow));
    node.bedMetres = node.surfaceMetres -
        std::max(strokes.minimumDepthMetres, strokes.depthMetres(flow));
    nodeOfCell[at] = static_cast<uint32_t>(network.nodes.size());
    network.nodes.push_back(std::move(node));
  }

  std::vector<uint8_t> visited(count, 0u);
  for (size_t start = 0u; start < count; ++start)
  {
    if (!isChannel(start) || inflow[start] == 1u)
      continue;
    std::vector<size_t> course;
    for (size_t at = start;;)
    {
      course.push_back(at);
      if (receiver[at] < 0)
        break;
      const auto next = static_cast<size_t>(receiver[at]);
      if (!isChannel(next))
      {
        course.push_back(next);
        break;
      }
      if (inflow[next] >= 2u)
      {
        course.push_back(next);
        break;
      }
      at = next;
      if (course.size() > count)
        break;
    }
    if (course.size() < 2u)
      continue;

    ChannelReach reach{};
    reach.upstreamNode = nodeOfCell[course.front()];
    reach.downstreamNode = nodeOfCell[course.back()];
    reach.mapCellCount = static_cast<uint32_t>(course.size());
    for (const size_t at : course)
    {
      ChannelVertex vertex{};
      vertex.position = positionOf(at);
      vertex.dischargeCumecs = dischargeOf(at);
      vertex.surfaceMetres = terrain.height(vertex.position);
      reach.vertices.push_back(vertex);
    }
    for (size_t at = 1u; at < reach.vertices.size(); ++at)
      reach.vertices[at].surfaceMetres = std::min(
          reach.vertices[at].surfaceMetres, reach.vertices[at - 1u].surfaceMetres);
    if (strokes.reachIntervalMetres > 0.0)
      for (ChannelVertex &vertex : reach.vertices)
        vertex.surfaceMetres =
            std::floor(vertex.surfaceMetres / strokes.reachIntervalMetres) *
            strokes.reachIntervalMetres;
    for (ChannelVertex &vertex : reach.vertices)
    {
      vertex.widthMetres = std::max(
          strokes.minimumWidthMetres, strokes.widthMetres(vertex.dischargeCumecs));
      vertex.depthMetres = std::max(
          strokes.minimumDepthMetres, strokes.depthMetres(vertex.dischargeCumecs));
      vertex.bedMetres = vertex.surfaceMetres - vertex.depthMetres;
    }

    const auto reachIndex = static_cast<uint32_t>(network.reaches.size());
    if (reach.downstreamNode != kInvalidChannelNode)
      network.nodes[reach.downstreamNode].incoming.push_back(reachIndex);
    if (reach.upstreamNode != kInvalidChannelNode)
      network.nodes[reach.upstreamNode].outgoing = reachIndex;
    for (const size_t at : course)
    {
      if (!isChannel(at))
        continue;
      ++network.carvedCells;
      if (visited[at] != 0u && inflow[at] < 2u)
        ++network.duplicateCells;
      visited[at] = 1u;
    }
    network.reaches.push_back(std::move(reach));
  }

  // Junction surfaces from what arrives, never an independent sample.
  for (ChannelNode &node : network.nodes)
  {
    double surface = node.surfaceMetres;
    for (const uint32_t incoming : node.incoming)
      if (incoming < network.reaches.size() &&
          !network.reaches[incoming].vertices.empty())
        surface = std::min(
            surface, network.reaches[incoming].vertices.back().surfaceMetres);
    if (strokes.reachIntervalMetres > 0.0)
      surface = std::floor(surface / strokes.reachIntervalMetres) *
          strokes.reachIntervalMetres;
    node.surfaceMetres = surface;
    node.bedMetres = surface - strokes.minimumDepthMetres;
    for (const uint32_t incoming : node.incoming)
      if (incoming < network.reaches.size() &&
          !network.reaches[incoming].vertices.empty())
        network.reaches[incoming].vertices.back().surfaceMetres = surface;
    if (node.outgoing != kInvalidChannelNode &&
        node.outgoing < network.reaches.size())
    {
      ChannelReach &out = network.reaches[node.outgoing];
      if (!out.vertices.empty())
      {
        out.vertices.front().surfaceMetres = surface;
        for (size_t at = 1u; at < out.vertices.size(); ++at)
          out.vertices[at].surfaceMetres = std::min(
              out.vertices[at].surfaceMetres, out.vertices[at - 1u].surfaceMetres);
      }
    }
  }
  for (ChannelReach &reach : network.reaches)
    for (ChannelVertex &vertex : reach.vertices)
      vertex.bedMetres = vertex.surfaceMetres - vertex.depthMetres;
  return network;
}

} // namespace procedural::geometry
