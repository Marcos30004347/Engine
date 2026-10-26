#include "WorldReliefSurface.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace procedural::world::rewrite {
namespace {
bool fail(std::string *error, const char *s)
{
  if (error)
  {
    *error = s;
  }
  return false;
}

Position project(Position p, Position a, Position b)
{
  double dx = b.x - a.x, dz = b.z - a.z, l2 = dx * dx + dz * dz, t = l2 ? std::clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / l2, 0.0, 1.0) : 0;
  return {a.x + t * dx, a.y + t * (b.y - a.y), a.z + t * dz};
}

double length(Position a, Position b)
{
  return std::hypot(a.x - b.x, a.z - b.z);
}

double noise(double x, double z, uint64_t seed)
{
  double fx = std::floor(x), fz = std::floor(z), u = x - fx, v = z - fz;
  u = u * u * (3 - 2 * u);
  v = v * v * (3 - 2 * v);
  auto at = [&](int dx, int dz) {
    return 2 * double(featureSeed(seed, 1, uint64_t(int64_t(fx) + dx), 0x4e4f4953, uint64_t(int64_t(fz) + dz)) >> 11) / 9007199254740992.0 - 1;
  };
  return (at(0, 0) * (1 - u) + at(1, 0) * u) * (1 - v) + (at(0, 1) * (1 - u) + at(1, 1) * u) * v;
}
} // namespace

double CompactTerrainField::weight(double d, double radius)
{
  if (!std::isfinite(d) || !std::isfinite(radius) || radius <= 0 || d < 0 || d >= radius)
  {
    return 0;
  }
  double u = d / radius;
  return (1 - u * u) * (1 - u * u);
}

bool CompactTerrainField::build(std::vector<TerrainPrimitive> primitives, std::string *error)
{
  if (primitives.empty() || primitives.size() > 65536)
  {
    return fail(error, "Invalid terrain primitive budget");
  }
  std::sort(primitives.begin(), primitives.end(), [](auto &a, auto &b) {
    return a.id < b.id;
  });
  for (size_t i = 0; i < primitives.size(); ++i)
  {
    auto p = primitives[i];
    if ((i && p.id == primitives[i - 1].id) || !std::isfinite(p.centre.x) || !std::isfinite(p.centre.y) || !std::isfinite(p.centre.z) || std::abs(p.centre.x) > 1e12 || std::abs(p.centre.z) > 1e12 ||
        !std::isfinite(p.radiusMetres) || p.radiusMetres <= 0 || p.radiusMetres > 1e12 || !std::isfinite(p.noiseAmplitudeMetres) || p.noiseAmplitudeMetres < 0 || !std::isfinite(p.noiseWavelengthMetres) ||
        p.noiseWavelengthMetres < 1e-3)
    {
      return fail(error, "Invalid or duplicate terrain primitive");
    }
  }
  CompactTerrainField next;
  next.primitives_ = std::move(primitives);
  size_t leaves = 1;
  while (leaves < next.primitives_.size())
  {
    leaves *= 2;
  }
  next.tree_.reserve(2 * leaves);
  std::function<size_t(size_t, size_t)> make = [&](size_t begin, size_t end) {
    size_t index = next.tree_.size();
    next.tree_.push_back({});
    Node n;
    n.begin = begin;
    n.end = end;
    n.minX = n.minZ = std::numeric_limits<double>::infinity();
    n.maxX = n.maxZ = -n.minX;
    if (end - begin == 1)
    {
      if (begin < next.primitives_.size())
      {
        auto p = next.primitives_[begin];
        n.minX = p.centre.x - p.radiusMetres;
        n.maxX = p.centre.x + p.radiusMetres;
        n.minZ = p.centre.z - p.radiusMetres;
        n.maxZ = p.centre.z + p.radiusMetres;
      }
    }
    else
    {
      n.left = make(begin, (begin + end) / 2);
      n.right = make((begin + end) / 2, end);
      for (auto child : {n.left, n.right})
      {
        auto &c = next.tree_[child];
        n.minX = std::min(n.minX, c.minX);
        n.minZ = std::min(n.minZ, c.minZ);
        n.maxX = std::max(n.maxX, c.maxX);
        n.maxZ = std::max(n.maxZ, c.maxZ);
      }
    }
    next.tree_[index] = n;
    return index;
  };
  make(0, leaves);
  *this = std::move(next);
  if (error)
  {
    error->clear();
  }
  return true;
}

std::pair<double, double> CompactTerrainField::evaluate(size_t index, double x, double z, bool accelerated) const
{
  const auto &n = tree_[index];
  if (accelerated && (x < n.minX || x > n.maxX || z < n.minZ || z > n.maxZ))
  {
    return {0, 0};
  }
  if (n.end - n.begin == 1)
  {
    if (n.begin >= primitives_.size())
    {
      return {0, 0};
    }
    auto p = primitives_[n.begin];
    double w = weight(std::hypot(x - p.centre.x, z - p.centre.z), p.radiusMetres);
    if (w == 0)
    {
      return {0, 0};
    }
    return {p.centre.y + p.noiseAmplitudeMetres * noise(x / p.noiseWavelengthMetres, z / p.noiseWavelengthMetres, p.noiseSeed), w};
  }
  auto a = evaluate(n.left, x, z, accelerated), b = evaluate(n.right, x, z, accelerated);
  double w = a.second + b.second;
  if (w == 0)
  {
    return {0, 0};
  }
  return {(a.first * a.second + b.first * b.second) / w, w * .5};
}

bool CompactTerrainField::sample(double x, double z, double &height, bool accelerated) const
{
  if (tree_.empty() || !std::isfinite(x) || !std::isfinite(z) || std::abs(x) > 1e12 || std::abs(z) > 1e12)
  {
    return false;
  }
  auto result = evaluate(0, x, z, accelerated);
  if (result.second <= 0 || !std::isfinite(result.first))
  {
    return false;
  }
  height = result.first;
  return true;
}

bool WorldReliefSurface::build(const LandProduct &land, const RiverGrowthProduct &rivers, const WatershedProduct &watersheds, const ReliefSurfaceSettings &settings, std::string *error)
{
  for (double v : {settings.sampleSpacingMetres, settings.supportScale, settings.roughnessFraction, settings.shoreTransitionMetres, settings.maximumCoastalSlope})
  {
    if (!std::isfinite(v))
    {
      return fail(error, "Nonfinite relief setting");
    }
  }
  if (settings.maximumCoastalSlope < 0 || settings.poissonSamplesPerCell > 256 || settings.sampleSpacingMetres < 0.01 || settings.supportScale <= 1 || settings.supportScale > 4 || settings.roughnessFraction < 0 ||
      settings.roughnessFraction > .25 || settings.shoreTransitionMetres <= 0 || rivers.nodes.empty() || watersheds.cells.size() != rivers.nodes.size())
  {
    return fail(error, "Invalid relief settings or watershed input");
  }
  if (!land.spec.validate(error))
  {
    return false;
  }
  for (size_t i = 0; i < rivers.nodes.size(); ++i)
  {
    auto n = rivers.nodes[i];
    if (!std::isfinite(n.position.x) || !std::isfinite(n.position.y) || !std::isfinite(n.position.z) || (n.receiver != UINT32_MAX && n.receiver >= i))
    {
      return fail(error, "Invalid relief river references");
    }
  }
  for (auto vertex : watersheds.vertices)
  {
    if (!std::isfinite(vertex.position.x) || !std::isfinite(vertex.position.y) || !std::isfinite(vertex.position.z))
    {
      return fail(error, "Nonfinite crest vertex");
    }
  }
  WorldReliefSurface next;
  next.shoreWidth_ = settings.shoreTransitionMetres;
  next.seaLevel_ = land.spec.seaLevelMetres;
  for (auto &loop : land.coasts)
  {
    for (auto use : loop.edges)
    {
      auto e = land.boundaries.find(use.edgeId);
      if (!e)
      {
        return fail(error, "Missing coast boundary");
      }
      for (size_t i = 1; i < e->vertices.size(); ++i)
      {
        next.coast_.push_back({e->vertices[i - 1].position, e->vertices[i].position});
      }
    }
  }
  if (next.coast_.empty())
  {
    return fail(error, "Missing coast supports");
  }
  for (auto e : watersheds.edges)
  {
    if (e.a >= watersheds.vertices.size() || e.b >= watersheds.vertices.size() || e.leftCell >= watersheds.cells.size() || e.rightCell >= watersheds.cells.size())
    {
      return fail(error, "Invalid crest references");
    }
  }
  std::vector<TerrainPrimitive> primitives;
  for (auto &cell : watersheds.cells)
  {
    const size_t cellBegin = primitives.size();
    if (cell.riverNode >= rivers.nodes.size())
    {
      return fail(error, "Invalid hydrological cell owner");
    }
    for (auto &fragment : cell.fragments)
    {
      if (fragment.size() < 3)
      {
        return fail(error, "Degenerate hydrological fragment");
      }
      for (size_t i = 0; i < fragment.size(); ++i)
      {
        auto a = fragment[i], b = fragment[(i + 1) % fragment.size()], c = fragment[(i + 2) % fragment.size()];
        if (!std::isfinite(a.x) || !std::isfinite(a.z) || (b.x - a.x) * (c.z - b.z) - (b.z - a.z) * (c.x - b.x) < -1e-7)
        {
          return fail(error, "Invalid convex support fragment");
        }
      }
      next.domain_.push_back(fragment);
      std::vector<std::array<Position, 3>> pending;
      for (size_t j = 1; j + 1 < fragment.size(); ++j)
      {
        pending.push_back({fragment[0], fragment[j], fragment[j + 1]});
      }
      while (!pending.empty())
      {
        auto t = pending.back();
        pending.pop_back();
        double extent = std::max({length(t[0], t[1]), length(t[1], t[2]), length(t[2], t[0])});
        double area = std::abs((t[1].x - t[0].x) * (t[2].z - t[0].z) - (t[1].z - t[0].z) * (t[2].x - t[0].x));
        if (area == 0)
        {
          continue;
        }
        if (primitives.size() + pending.size() > 65530)
        {
          return fail(error, "Terrain support refinement exceeds budget");
        }
        if (extent > settings.sampleSpacingMetres)
        {
          auto midpoint = [](Position a, Position b) {
            return Position{(a.x + b.x) * .5, 0, (a.z + b.z) * .5};
          };
          auto a = midpoint(t[0], t[1]), b = midpoint(t[1], t[2]), c = midpoint(t[2], t[0]);
          pending.push_back({t[0], a, c});
          pending.push_back({a, t[1], b});
          pending.push_back({c, b, t[2]});
          pending.push_back({a, b, c});
          continue;
        }
        Position centre{(t[0].x + t[1].x + t[2].x) / 3, 0, (t[0].z + t[1].z + t[2].z) / 3};
        double dr = std::numeric_limits<double>::infinity(), dc = dr;
        Position river{}, crest{};
        for (size_t n = 0; n < rivers.nodes.size(); ++n)
        {
          auto node = rivers.nodes[n];
          auto q = node.receiver == UINT32_MAX ? node.position : project(centre, node.position, rivers.nodes[node.receiver].position);
          double d = length(centre, q);
          if (d < dr)
          {
            dr = d;
            river = q;
          }
        }
        for (auto edge : watersheds.edges)
        {
          if (!edge.riverCrossing && (edge.leftCell == cell.riverNode || edge.rightCell == cell.riverNode))
          {
            auto q = project(centre, watersheds.vertices[edge.a].position, watersheds.vertices[edge.b].position);
            double d = length(centre, q);
            if (d < dc)
            {
              dc = d;
              crest = q;
            }
          }
        }
        // Coast compatibility is continuous. Switching to the nearest sea-level
        // crest abruptly would create an artificial inland escarpment.
        double distanceToCoast = std::numeric_limits<double>::infinity();
        Position nearestCoast;
        for (auto s : next.coast_)
        {
          auto q = project(centre, s.first, s.second);
          const double d = length(centre, q);
          if (d < distanceToCoast)
          {
            distanceToCoast = d;
            nearestCoast = q;
          }
        }
        if (!std::isfinite(dc))
        {
          dc = distanceToCoast;
          crest = nearestCoast;
          crest.y = next.seaLevel_;
        }
        centre.y = dr + dc > 0 ? (river.y * dc + crest.y * dr) / (dr + dc) : river.y;
        if (settings.maximumCoastalSlope > 0)
        {
          centre.y = std::min(centre.y, next.seaLevel_ + settings.maximumCoastalSlope * distanceToCoast);
        }
        double radius = std::max({length(centre, t[0]), length(centre, t[1]), length(centre, t[2])}) * settings.supportScale;
        if (radius <= 0 || !std::isfinite(centre.y))
        {
          return fail(error, "Invalid relief support construction");
        }
        uint64_t id = primitives.size() + 1;
        double amplitude = settings.roughnessFraction * std::max(0.0, crest.y - river.y) * (dr / (dr + dc + 1e-12));
        primitives.push_back({id, featureSeed(land.spec.seed, land.spec.generatorVersion, id, 0x54455252), centre, radius, amplitude, settings.sampleSpacingMetres});
      }
    }
    if (settings.poissonSamplesPerCell && primitives.size() > cellBegin)
    {
      // Deterministic dart throwing over the refined candidate set. Expand each
      // retained support to contain assigned old supports: coverage is preserved.
      std::vector<TerrainPrimitive> candidates(primitives.begin() + cellBegin, primitives.end()), selected;
      std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
        return a.noiseSeed < b.noiseSeed;
      });
      const double separation = .6 * std::sqrt(cell.areaMetres2 / settings.poissonSamplesPerCell);
      for (auto p : candidates)
      {
        bool clear = true;
        for (const auto &q : selected)
        {
          clear &= length(p.centre, q.centre) >= separation;
        }
        if (clear)
        {
          selected.push_back(p);
        }
        if (selected.size() >= settings.poissonSamplesPerCell)
        {
          break;
        }
      }
      for (const auto &p : candidates)
      {
        auto nearest = std::min_element(selected.begin(), selected.end(), [&](const auto &a, const auto &b) {
          return length(a.centre, p.centre) < length(b.centre, p.centre);
        });
        nearest->radiusMetres = std::max(nearest->radiusMetres, length(nearest->centre, p.centre) + p.radiusMetres);
      }
      primitives.resize(cellBegin);
      for (auto p : selected)
      {
        p.id = primitives.size() + 1;
        primitives.push_back(p);
      }
    }
  }
  if (!next.field_.build(std::move(primitives), error))
  {
    return false;
  }
  *this = std::move(next);
  if (error)
  {
    error->clear();
  }
  return true;
}

bool WorldReliefSurface::sample(double x, double z, double &height, bool accelerated) const
{
  if (!std::isfinite(x) || !std::isfinite(z))
  {
    return false;
  }
  bool inside = false;
  for (auto &r : domain_)
  {
    bool contained = true;
    for (size_t i = 0; i < r.size(); ++i)
    {
      auto a = r[i], b = r[(i + 1) % r.size()];
      if ((b.x - a.x) * (z - a.z) - (b.z - a.z) * (x - a.x) < -1e-8)
      {
        contained = false;
        break;
      }
    }
    if (contained)
    {
      inside = true;
      break;
    }
  }
  if (!inside)
  {
    return false;
  }
  double h;
  if (!field_.sample(x, z, h, accelerated))
  {
    return false;
  }
  Position p{x, 0, z};
  double d = std::numeric_limits<double>::infinity();
  for (auto s : coast_)
  {
    d = std::min(d, length(p, project(p, s.first, s.second)));
  }
  double u = std::clamp(d / shoreWidth_, 0.0, 1.0);
  u = u * u * (3 - 2 * u);
  height = seaLevel_ + (h - seaLevel_) * u;
  return std::isfinite(height);
}
} // namespace procedural::world::rewrite
