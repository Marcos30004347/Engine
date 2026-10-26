#include "WorldSurfaceRefinement.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace procedural::world::rewrite {
namespace {
bool fail(std::string *e, const char *s)
{
  if (e)
  {
    *e = s;
  }
  return false;
}

Position normal(Position a, Position b, Position c)
{
  Position u{b.x - a.x, b.y - a.y, b.z - a.z}, v{c.x - a.x, c.y - a.y, c.z - a.z};
  Position n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
  double length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
  if (length == 0 || !std::isfinite(length))
  {
    return {NAN, NAN, NAN};
  }
  return {n.x / length, n.y / length, n.z / length};
}

double angle(Position a, Position b)
{
  return std::acos(std::clamp(a.x * b.x + a.y * b.y + a.z * b.z, -1., 1.)) * 180 / 3.141592653589793;
}

bool finite(Position p)
{
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
} // namespace

bool refineWorldSurfaces(
    const WorldFinalPatches &input,
    const PatchSurface &surface,
    const std::vector<SurfaceFeatureSample> &features,
    const SurfaceErrorSettings &settings,
    RefinedWorldSurfaces &out,
    std::string *error)
{
  if (!surface || !std::isfinite(settings.heightMetres) || settings.heightMetres <= 0 || !std::isfinite(settings.normalDegrees) || settings.normalDegrees <= 0 || settings.normalDegrees > 180 ||
      !std::isfinite(settings.derivativeStepMetres) || settings.derivativeStepMetres <= 0 || settings.maximumPasses == 0 || settings.maximumPasses > 64 || settings.maximumAddedVertices > 100000 ||
      features.size() > 100000)
  {
    return fail(error, "invalid surface refinement settings");
  }
  if (!auditWorldFinalPatches(input, error))
  {
    return false;
  }
  RefinedWorldSurfaces result;
  result.world = input;
  BoundaryRegistry registry;
  std::vector<BoundaryInput> edges;
  std::map<std::pair<uint32_t, uint32_t>, uint64_t> edgeIds;
  for (auto e : input.map.edges)
  {
    edges.push_back({e.id, {e.a + 1, input.map.vertices[e.a]}, {e.b + 1, input.map.vertices[e.b]}, {}});
    edgeIds[std::minmax(e.a, e.b)] = e.id;
  }
  if (!registry.commit(edges, error))
  {
    return false;
  }
  std::map<uint64_t, std::vector<Position>> mandatory;
  for (auto f : features)
  {
    if (!f.face || f.face > input.map.faces.size() || !finite(f.position))
    {
      return fail(error, "invalid mandatory feature");
    }
    mandatory[f.face].push_back(f.position);
  }
  SurfaceArrangementMesh mesh;
  mesh.vertices = input.map.vertices;
  for (const auto &face : input.map.faces)
  {
    auto height = [&](double x, double z) {
      return surface(face, x, z);
    };
    PatchDomain domain;
    domain.id = face.id;
    for (size_t i = 0; i < face.vertices.size(); ++i)
    {
      auto a = face.vertices[i], b = face.vertices[(i + 1) % face.vertices.size()];
      domain.outer.push_back({edgeIds.at(std::minmax(a, b)), a > b});
    }
    auto patch = input.mesh.patches[face.id - 1];
    std::vector<Position> samples;
    std::set<std::pair<double, double>> present;
    for (auto v : patch.vertices)
    {
      present.emplace(v.position.x, v.position.z);
      if (v.owner == UINT64_MAX)
      {
        samples.push_back(v.position);
      }
    }
    bool dirty = false;
    auto add = [&](Position p) {
      if (!present.emplace(p.x, p.z).second)
      {
        return false;
      }
      samples.push_back(p);
      ++result.addedVertices;
      return true;
    };
    auto required = mandatory[face.id];
    std::sort(required.begin(), required.end(), [](auto a, auto b) {
      return std::tie(a.x, a.z) < std::tie(b.x, b.z);
    });
    for (auto p : required)
    {
      dirty |= add(p);
    }
    std::vector<double> heightErrors, normalErrors;
    bool accepted = false;
    for (uint32_t pass = 0; pass < settings.maximumPasses; ++pass)
    {
      result.passes = std::max(result.passes, pass + 1);
      if (result.addedVertices > settings.maximumAddedVertices)
      {
        return fail(error, "surface refinement vertex budget exhausted");
      }
      // Re-evaluate the source even on the first pass: reject stale boundary heights.
      for (const auto &c : patch.constraints)
      {
        auto a = patch.vertices[c.a].position, b = patch.vertices[c.b].position;
        for (double t : {0., .25, .5, .75, 1.})
        {
          double h = height(a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t);
          if (!std::isfinite(h))
          {
            return fail(error, "nonfinite boundary source surface");
          }
          if (std::abs(h - (a.y + (b.y - a.y) * t)) > settings.heightMetres)
          {
            std::ostringstream detail;
            detail << "frozen boundary needs upstream stations: face " << face.id << " material " << face.material << " role " << int(face.role) << (face.terrainSurface ? " terrain" : " plane") << " edge (" << a.x << ","
                   << a.z << ")-(" << b.x << "," << b.z << ") t=" << t << " surface " << h << " vs edge " << a.y + (b.y - a.y) * t;
            return fail(error, detail.str().c_str());
          }
        }
      }
      if (dirty || pass == 0)
      {
        if (!compilePatchMesh(domain, registry, samples, height, patch, error))
        {
          return false;
        }
      }
      dirty = false;
      heightErrors.clear();
      normalErrors.clear();
      for (auto t : patch.triangles)
      {
        auto a = patch.vertices[t[0]].position, b = patch.vertices[t[1]].position, c = patch.vertices[t[2]].position;
        auto n = normal(a, b, c);
        if (!finite(n))
        {
          return fail(error, "invalid refinement triangle normal");
        }
        if (n.y < 0)
        {
          n.x = -n.x;
          n.y = -n.y;
          n.z = -n.z;
        }
        double maxH = 0, maxN = 0;
        Position worst{};
        double worstScore = -1;
        // Edge quarter/midpoints, centroid and interior barycentric probes.
        const std::array<std::array<double, 3>, 13> weights = {
          {{1. / 3, 1. / 3, 1. / 3}, {.6, .2, .2}, {.2, .6, .2}, {.2, .2, .6}, {.5, .5, 0}, {0, .5, .5}, {.5, 0, .5}, {.25, .75, 0}, {.75, .25, 0}, {0, .25, .75}, {0, .75, .25}, {.25, 0, .75}, {.75, 0, .25}}};
        for (auto w : weights)
        {
          Position p{a.x * w[0] + b.x * w[1] + c.x * w[2], a.y * w[0] + b.y * w[1] + c.y * w[2], a.z * w[0] + b.z * w[1] + c.z * w[2]};
          double h = height(p.x, p.z), dh = std::abs(h - p.y), dn = 0;
          if (!std::isfinite(h))
          {
            return fail(error, "nonfinite refinement source surface");
          }
          // Derivatives are only sampled inside the triangle to avoid querying across ownership creases.
          if (w[0] > 0 && w[1] > 0 && w[2] > 0 && settings.normalDegrees < 180)
          {
            double step = settings.derivativeStepMetres;
            for (auto edge : std::array<std::pair<Position, Position>, 3>{{{a, b}, {b, c}, {c, a}}})
            {
              auto u = edge.first, v = edge.second;
              double distance = std::abs((v.x - u.x) * (p.z - u.z) - (v.z - u.z) * (p.x - u.x)) / std::hypot(v.x - u.x, v.z - u.z);
              step = std::min(step, distance * .25);
            }
            if (!(step > 0))
            {
              return fail(error, "normal derivative probe has no interior clearance");
            }
            double hx = (height(p.x + step, p.z) - height(p.x - step, p.z)) / (2 * step), hz = (height(p.x, p.z + step) - height(p.x, p.z - step)) / (2 * step);
            double length = std::sqrt(1 + hx * hx + hz * hz);
            Position expected{-hx / length, 1 / length, -hz / length};
            if (!finite(expected))
            {
              return fail(error, "nonfinite source normal");
            }
            dn = angle(n, expected);
          }
          maxH = std::max(maxH, dh);
          maxN = std::max(maxN, dn);
          double score = std::max(dh / settings.heightMetres, dn / settings.normalDegrees);
          // A previous edge probe can already be present in a degenerate CDT neighborhood.
          // Prefer the highest-error new probe; retain every constraint and existing sample.
          if (score > worstScore && !present.count({p.x, p.z}))
          {
            worstScore = score;
            worst = p;
          }
        }
        heightErrors.push_back(maxH);
        normalErrors.push_back(maxN);
        if (worstScore > 1)
        {
          // Never insert an interior point on a frozen constraint.
          bool boundary = false;
          for (auto e : patch.constraints)
          {
            auto u = patch.vertices[e.a].position, v = patch.vertices[e.b].position;
            double dx = v.x - u.x, dz = v.z - u.z;
            double length = std::hypot(dx, dz), dot = (worst.x - u.x) * dx + (worst.z - u.z) * dz;
            if (std::abs(dx * (worst.z - u.z) - dz * (worst.x - u.x)) <= 1e-8 * length && dot >= 0 && dot <= length * length)
            {
              boundary = true;
              break;
            }
          }
          if (boundary)
          {
            return fail(error, "error probe requires frozen boundary refinement");
          }
          dirty |= add(worst);
        }
      }
      auto maxH = *std::max_element(heightErrors.begin(), heightErrors.end()), maxN = *std::max_element(normalErrors.begin(), normalErrors.end());
      if (maxH <= settings.heightMetres && maxN <= settings.normalDegrees)
      {
        accepted = true;
        break;
      }
      if (!dirty)
      {
        if (error)
        {
          *error = "surface refinement stalled above error budget face=" + std::to_string(face.id) + " height=" + std::to_string(maxH) + " normal=" + std::to_string(maxN);
        }
        return false;
      }
    }
    if (!accepted)
    {
      return fail(error, "surface refinement pass budget exhausted");
    }
    result.triangleHeightErrors.insert(result.triangleHeightErrors.end(), heightErrors.begin(), heightErrors.end());
    result.triangleNormalErrors.insert(result.triangleNormalErrors.end(), normalErrors.begin(), normalErrors.end());
    std::vector<uint32_t> indices;
    for (auto v : patch.vertices)
    {
      if (v.owner == 0 && v.index && v.index <= input.map.vertices.size())
      {
        indices.push_back(v.index - 1);
      }
      else
      {
        indices.push_back(mesh.vertices.size());
        mesh.vertices.push_back(v.position);
      }
    }
    for (auto t : patch.triangles)
    {
      std::array<uint32_t, 3> ids{indices[t[0]], indices[t[1]], indices[t[2]]};
      if (normal(mesh.vertices[ids[0]], mesh.vertices[ids[1]], mesh.vertices[ids[2]]).y < 0)
      {
        std::swap(ids[1], ids[2]);
      }
      mesh.triangles.push_back({ids, face.id, face.material, face.layer, face.role});
    }
    if (!face.layer)
    {
      mesh.groundArea += patch.triangleArea;
    }
    mesh.patches.push_back(std::move(patch));
  }
  result.world.mesh = std::move(mesh);
  std::map<uint64_t, size_t> tile;
  for (size_t i = 0; i < result.world.tiles.size(); ++i)
  {
    auto &t = result.world.tiles[i];
    t.triangles.clear();
    for (auto f : t.faces)
    {
      tile[f] = i;
    }
  }
  for (uint32_t i = 0; i < result.world.mesh.triangles.size(); ++i)
  {
    result.world.tiles[tile.at(result.world.mesh.triangles[i].face)].triangles.push_back(i);
  }
  if (!auditWorldFinalPatches(result.world, error))
  {
    return false;
  }
  result.maximumHeightError = *std::max_element(result.triangleHeightErrors.begin(), result.triangleHeightErrors.end());
  result.maximumNormalError = *std::max_element(result.triangleNormalErrors.begin(), result.triangleNormalErrors.end());
  out = std::move(result);
  return true;
}

bool assembleSurfaceMesh(const RefinedWorldSurfaces &input, const SplineFaceMesh &structures, uint32_t material, bool allowOpen, AssembledSurfaceMesh &out, std::string *error)
{
  if (!auditWorldFinalPatches(input.world, error))
  {
    return false;
  }
  if (structures.expectedNormals.size() != structures.triangles.size())
  {
    return fail(error, "structural faces need explicit normals");
  }
  AssembledSurfaceMesh result;
  using Key = std::tuple<double, double, double>;
  using Edge = std::pair<Key, Key>;
  std::map<Edge, std::pair<uint32_t, int>> incidence;
  std::set<Edge> planarEdges, permittedPlanarRims;
  for (auto e : input.world.map.edges)
  {
    if (!e.leftFace || !e.rightFace)
    {
      auto a = input.world.map.vertices[e.a], b = input.world.map.vertices[e.b];
      permittedPlanarRims.insert(std::minmax(Key{a.x, a.y, a.z}, Key{b.x, b.y, b.z}));
    }
  }
  auto append = [&](std::array<Position, 3> p, uint32_t mat, Position expected) {
    auto n = normal(p[0], p[1], p[2]);
    if (!finite(n) || !finite(expected) || n.x * expected.x + n.y * expected.y + n.z * expected.z <= 0)
    {
      return false;
    }
    uint32_t base = result.geometry.vertices.size();
    for (auto v : p)
    {
      if (!finite(v))
      {
        return false;
      }
      result.geometry.vertices.push_back(v);
    }
    result.geometry.triangles.push_back({base, base + 1, base + 2});
    result.geometry.expectedNormals.push_back(n);
    result.materials.push_back(mat);
    for (int i = 0; i < 3; ++i)
    {
      auto a = p[i], b = p[(i + 1) % 3];
      Key u{a.x, a.y, a.z}, v{b.x, b.y, b.z};
      auto &edge = incidence[std::minmax(u, v)];
      ++edge.first;
      edge.second += u < v ? 1 : -1;
    }
    return true;
  };
  for (auto t : input.world.mesh.triangles)
  {
    if (!append({input.world.mesh.vertices[t.vertices[0]], input.world.mesh.vertices[t.vertices[1]], input.world.mesh.vertices[t.vertices[2]]}, t.material, {0, 1, 0}))
    {
      return fail(error, "invalid ground assembly triangle");
    }
  }
  for (const auto &[key, value] : incidence)
  {
    planarEdges.insert(key);
  }
  for (size_t i = 0; i < structures.triangles.size(); ++i)
  {
    auto t = structures.triangles[i];
    for (auto v : t)
    {
      if (v >= structures.vertices.size())
      {
        return fail(error, "invalid structural index");
      }
    }
    if (!append({structures.vertices[t[0]], structures.vertices[t[1]], structures.vertices[t[2]]}, material, structures.expectedNormals[i]))
    {
      return fail(error, "invalid structural normal or triangle");
    }
  }
  for (auto [key, e] : incidence)
  {
    if (e.first > 2 || (e.first == 2 && e.second != 0))
    {
      return fail(error, "nonmanifold or duplicate assembly edge");
    }
    if (e.first == 1)
    {
      if (planarEdges.count(key) && !permittedPlanarRims.count(key))
      {
        return fail(error, "unclassified internal ground crack");
      }
      ++result.openEdges;
    }
    else
    {
      ++result.sharedEdges;
    }
  }
  if (!allowOpen && result.openEdges)
  {
    return fail(error, "closed assembly has open boundaries");
  }
  out = std::move(result);
  return true;
}
} // namespace procedural::world::rewrite
