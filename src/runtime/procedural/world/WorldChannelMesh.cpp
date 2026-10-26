#include "WorldChannelMesh.hpp"
#include "WorldPatchMesh.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace procedural::world::rewrite {
bool buildChannelMesh(const WorldRiverChannels &channels, const WorldRiverChannels::BaseSurface &base, const ChannelMeshSettings &s, ChannelMeshProduct &out, std::string *error)
{
  auto fail = [&](const char *t) {
    if (error)
    {
      *error = t;
    }
    return false;
  };
  for (double v : {s.minX, s.maxX, s.minZ, s.maxZ, s.spacingMetres})
  {
    if (!std::isfinite(v) || std::abs(v) > 1e8)
    {
      return fail("Invalid channel mesh bounds");
    }
  }
  if (s.maxX <= s.minX || s.maxZ <= s.minZ || s.spacingMetres <= 0 || s.spacingMetres > channels.settings().minimumBottomWidthMetres * .5)
  {
    return fail("Grid must resolve the narrowest channel");
  }
  double nxD = std::ceil((s.maxX - s.minX) / s.spacingMetres), nzD = std::ceil((s.maxZ - s.minZ) / s.spacingMetres);
  if (nxD * nzD > 20000)
  {
    return fail("Channel acceptance mesh budget exceeded");
  }
  int nx = int(nxD), nz = int(nzD);
  std::vector<Position> points;
  std::vector<ChannelSample> values;
  auto add = [&](double x, double z) {
    ChannelSample value;
    if (!channels.sample(x, z, base, value))
    {
      return uint32_t(UINT32_MAX);
    }
    uint32_t id = points.size();
    points.push_back({x, value.terrainHeight, z});
    values.push_back(value);
    return id;
  };
  for (int z = 0; z <= nz; ++z)
  {
    for (int x = 0; x <= nx; ++x)
    {
      if (add(s.minX + (s.maxX - s.minX) * x / nx, s.minZ + (s.maxZ - s.minZ) * z / nz) == UINT32_MAX)
      {
        return fail("Unsupported mesh sample");
      }
    }
  }
  using Edge = std::pair<uint32_t, uint32_t>;
  std::map<Edge, uint32_t> crossings;
  auto crossing = [&](uint32_t a, uint32_t b) {
    Edge key = std::minmax(a, b);
    auto found = crossings.find(key);
    if (found != crossings.end())
    {
      return found->second;
    }
    if (std::abs(values[a].signedWaterDepth) < 1e-10)
    {
      return a;
    }
    if (std::abs(values[b].signedWaterDepth) < 1e-10)
    {
      return b;
    }
    auto pa = points[a], pb = points[b];
    double lo = 0, hi = 1;
    bool positive = values[a].signedWaterDepth > 0;
    for (int k = 0; k < 48; ++k)
    {
      double mid = (lo + hi) / 2;
      ChannelSample v;
      if (!channels.sample(pa.x + mid * (pb.x - pa.x), pa.z + mid * (pb.z - pa.z), base, v))
      {
        return uint32_t(UINT32_MAX);
      }
      if ((v.signedWaterDepth > 0) == positive)
      {
        lo = mid;
      }
      else
      {
        hi = mid;
      }
    }
    double t = (lo + hi) / 2;
    auto id = add(pa.x + t * (pb.x - pa.x), pa.z + t * (pb.z - pa.z));
    crossings[key] = id;
    return id;
  };

  struct Polygon
  {
    std::vector<uint32_t> ids;
    bool wet;
  };

  std::vector<Polygon> polygons;
  for (int z = 0; z < nz; ++z)
  {
    for (int x = 0; x < nx; ++x)
    {
      uint32_t a = z * (nx + 1) + x, b = a + 1, c = a + nx + 1, d = c + 1;
      for (auto tri : std::vector<std::array<uint32_t, 3>>{{a, b, c}, {b, d, c}})
      {
        for (bool wet : {false, true})
        {
          std::vector<uint32_t> ids;
          for (int j = 0; j < 3; ++j)
          {
            auto a = tri[j], b = tri[(j + 1) % 3];
            bool ia = wet ? values[a].signedWaterDepth > 0 : values[a].signedWaterDepth <= 0, ib = wet ? values[b].signedWaterDepth > 0 : values[b].signedWaterDepth <= 0;
            if (ia)
            {
              ids.push_back(a);
            }
            if (ia != ib)
            {
              auto id = crossing(a, b);
              if (id == UINT32_MAX)
              {
                return fail("Unsupported bank root");
              }
              ids.push_back(id);
            }
          }
          ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
          if (ids.size() > 1 && ids.front() == ids.back())
          {
            ids.pop_back();
          }
          if (ids.size() >= 3)
          {
            polygons.push_back({ids, wet});
          }
        }
      }
    }
  }
  std::map<Edge, uint64_t> edges;
  std::vector<BoundaryInput> bedInputs, waterInputs;
  for (auto poly : polygons)
  {
    for (size_t j = 0; j < poly.ids.size(); ++j)
    {
      uint32_t a = poly.ids[j], b = poly.ids[(j + 1) % poly.ids.size()];
      Edge key = std::minmax(a, b);
      if (edges.count(key))
      {
        continue;
      }
      uint64_t id = edges.size() + 1;
      edges[key] = id;
      auto pa = points[key.first], pb = points[key.second];
      bedInputs.push_back({id, {key.first + 1, pa}, {key.second + 1, pb}, {}});
      pa.y = values[key.first].waterHeight;
      pb.y = values[key.second].waterHeight;
      waterInputs.push_back({id, {key.first + 1, pa}, {key.second + 1, pb}, {}});
    }
  }
  BoundaryRegistry bedRegistry, waterRegistry;
  if (!bedRegistry.commit(bedInputs, error) || !waterRegistry.commit(waterInputs, error))
  {
    return false;
  }
  ChannelMeshProduct result;
  std::map<std::pair<uint8_t, uint64_t>, uint32_t> published;
  uint64_t patchId = 1;
  for (auto poly : polygons)
  {
    PatchDomain domain;
    domain.id = patchId++;
    for (size_t j = 0; j < poly.ids.size(); ++j)
    {
      auto a = poly.ids[j], b = poly.ids[(j + 1) % poly.ids.size()];
      domain.outer.push_back({edges[std::minmax(a, b)], a > b});
    }
    for (uint8_t role = 0; role <= (poly.wet ? 1 : 0); ++role)
    {
      PatchMesh mesh;
      auto &registry = role ? waterRegistry : bedRegistry;
      if (!compilePatchMesh(
              domain,
              registry,
              {},
              [](double, double) {
                return 0.;
              },
              mesh,
              error))
      {
        return false;
      }
      std::vector<uint32_t> indices;
      for (auto vertex : mesh.vertices)
      {
        auto key = std::make_pair(role, vertex.index);
        if (!published.count(key))
        {
          published[key] = result.vertices.size();
          result.vertices.push_back(vertex.position);
        }
        indices.push_back(published[key]);
      }
      for (auto t : mesh.triangles)
      {
        result.triangles.push_back({indices[t[0]], indices[t[1]], indices[t[2]]});
        result.roles.push_back(role);
      }
      if (role)
      {
        result.waterAreaMetres2 += mesh.triangleArea;
      }
      else
      {
        result.terrainAreaMetres2 += mesh.triangleArea;
      }
    }
  }
  result.shorelineSegments = crossings.size();
  double expected = (s.maxX - s.minX) * (s.maxZ - s.minZ);
  if (std::abs(result.terrainAreaMetres2 - expected) > expected * 1e-8)
  {
    return fail("Channel bed/land partition lost area");
  }
  out = std::move(result);
  return true;
}
} // namespace procedural::world::rewrite
