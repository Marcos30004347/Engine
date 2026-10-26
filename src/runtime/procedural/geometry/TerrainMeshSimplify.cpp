#include "procedural/geometry/TerrainMeshSimplify.hpp"
#include "procedural/ConstrainedDelaunay.hpp"
#include "procedural/Geometry.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
namespace procedural::geometry
{
namespace
{
Point2 xy(const MeshVertex &v)
{
  return {v.x, v.z};
}
double area(Point2 a, Point2 b, Point2 c)
{
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}
std::array<double, 3> bary(Point2 p, Point2 a, Point2 b, Point2 c)
{
  const double d = area(a, b, c);
  if (std::abs(d) < 1e-15)
    return {-1, -1, -1};
  return {area(p, b, c) / d, area(a, p, c) / d, area(a, b, p) / d};
}

} // namespace
void simplifyTerrainTile(MeshTile &tile, std::vector<uint8_t> locked, const TerrainMeshSettings &settings, const HeightFunction &height, const MaterialFunction &material)
{
  const auto budget = [&](Point2 p)
  {
    return std::min(settings.simplificationToleranceMetres, settings.verticalToleranceMetres * .5 * (settings.toleranceScale ? std::max(.05, settings.toleranceScale(p)) : 1.0));
  };
  if (!settings.simplificationRounds || settings.simplificationToleranceMetres <= 0)
    return;
  for (uint32_t round = 0; round < settings.simplificationRounds; ++round)
  {
    const auto &v = tile.vertices;
    const auto &tri = tile.indices;
    // The surface field is the cost of this pass, not the local retriangulation:
    // one material() on rock resolves five height() calls, and each of those
    // walks the region, coast, river and landform buckets. A vertex is examined
    // once per incident face of each of its neighbours' stars - about a dozen
    // times a round - so memoise both field queries per vertex. Pure functions
    // of position, so the mesh is unchanged; only the call count is.
    std::vector<uint8_t> materialAtVertex(v.size()), materialResolved(v.size(), 0u);
    std::vector<double> budgetAtVertex(v.size());
    std::vector<uint8_t> budgetResolved(v.size(), 0u);
    const auto vertexMaterial = [&](const uint32_t n)
    {
      if (!materialResolved[n]) { materialAtVertex[n] = material(xy(v[n])); materialResolved[n] = 1u; }
      return materialAtVertex[n];
    };
    const auto vertexBudget = [&](const uint32_t n)
    {
      if (!budgetResolved[n]) { budgetAtVertex[n] = budget(xy(v[n])); budgetResolved[n] = 1u; }
      return budgetAtVertex[n];
    };
    std::vector<std::vector<uint32_t>> stars(v.size());
    // Only the boundary edges are wanted - the ones a single face uses - so a
    // sorted run scan answers it without a tree node per edge of the tile.
    std::vector<std::pair<uint32_t, uint32_t>> edges;
    edges.reserve(tri.size());
    for (uint32_t i = 0; i < tri.size(); i += 3)
    {
      for (int j = 0; j < 3; ++j)
      {
        const uint32_t from = tri[i + j], to = tri[i + (j + 1) % 3];
        stars[from].push_back(i);
        edges.emplace_back(std::min(from, to), std::max(from, to));
      }
    }
    std::sort(edges.begin(), edges.end());
    for (size_t i = 0; i < edges.size();)
    {
      size_t next = i;
      while (next < edges.size() && edges[next] == edges[i])
        ++next;
      if (next - i == 1)
      {
        locked[edges[i].first] = 1;
        locked[edges[i].second] = 1;
      }
      i = next;
    }
    std::vector<uint8_t> blocked = locked, remove(v.size()), removeFace(tri.size() / 3);
    std::vector<uint32_t> replacements;
    std::vector<uint8_t> replacementMaterials;
    uint32_t removed = 0;
    for (uint32_t id = 0; id < v.size(); ++id)
    {
      if (blocked[id] || stars[id].empty())
        continue;
      const auto first = stars[id].front();
      const auto a = v[tri[first]], b = v[tri[first + 1]], c = v[tri[first + 2]];
      if (std::abs(area(xy(a), xy(b), xy(c))) < 1e-10)
        continue;
      bool planar = true;
      std::vector<uint32_t> neighbors;
      const auto mat = vertexMaterial(id);
      for (const auto face : stars[id])
        planar &= tile.materials[face / 3] == mat;
      // The face-material test is a table lookup and the star scan is not, so
      // settle the cheap one before paying for a single field query.
      if (!planar)
        continue;
      for (const auto face : stars[id])
      {
        for (int j = 0; j < 3; ++j)
        {
          const auto n = tri[face + j];
          const auto w = bary(xy(v[n]), xy(a), xy(b), xy(c));
          const double expected = w[0] * a.y + w[1] * b.y + w[2] * c.y;
          neighbors.push_back(n);
          planar &= std::abs(v[n].y - expected) <= vertexBudget(n) * .5 && vertexMaterial(n) == mat;
          // A star is rejected on its first non-planar neighbour; the rest of
          // the scan cannot revive it. `neighbors` is only read on the path
          // where `planar` still holds, so leaving it short here is safe.
          if (!planar)
            break;
        }
        if (!planar)
          break;
      }
      if (!planar)
        continue;
      // Validate the small replacement cavity before batching removals. One
      // borderline curved star must not veto valid removals on distant plains.
      auto ring = neighbors;
      std::sort(ring.begin(), ring.end());
      ring.erase(std::unique(ring.begin(), ring.end()), ring.end());
      ring.erase(std::remove(ring.begin(), ring.end(), id), ring.end());
      std::vector<Point2> local;
      for (auto n : ring)
        local.push_back(xy(v[n]));
      std::vector<Constraint> boundary;
      for (auto face : stars[id])
      {
        std::vector<uint32_t> edge;
        for (int j = 0; j < 3; ++j)
          if (tri[face + j] != id)
            edge.push_back(uint32_t(std::lower_bound(ring.begin(), ring.end(), tri[face + j]) - ring.begin()));
        if (edge.size() == 2)
          boundary.emplace_back(edge[0], edge[1]);
      }
      ConstrainedDelaunayTriangulator localCdt;
      if (!localCdt.triangulate(local, boundary))
        continue;
      for (size_t k = 0; k < boundary.size(); ++k)
        planar &= localCdt.constraintIsIncluded(k);
      if (!planar)
        continue;
      std::vector<uint32_t> cavity;
      double oldArea = 0, newArea = 0;
      for (auto face : stars[id])
        oldArea += std::abs(area(xy(v[tri[face]]), xy(v[tri[face + 1]]), xy(v[tri[face + 2]])));
      const auto faces = localCdt.collectTriangles();
      bool found = false;
      for (size_t k = 0; k + 2 < faces.size() && planar; k += 3)
      {
        if (faces[k] < 0 || faces[k + 1] < 0 || faces[k + 2] < 0 || faces[k] >= int64_t(ring.size()) || faces[k + 1] >= int64_t(ring.size()) || faces[k + 2] >= int64_t(ring.size()))
          continue;
        const auto va = v[ring[faces[k]]], vb = v[ring[faces[k + 1]]], vc = v[ring[faces[k + 2]]];
        const Point2 mid{(va.x + vb.x + vc.x) / 3, (va.z + vb.z + vc.z) / 3};
        bool inside = false;
        for (auto face : stars[id])
        {
          const auto w = bary(mid, xy(v[tri[face]]), xy(v[tri[face + 1]]), xy(v[tri[face + 2]]));
          inside |= *std::min_element(w.begin(), w.end()) >= -1e-8;
        }
        if (!inside)
          continue;
        uint32_t ia = ring[faces[k]], ib = ring[faces[k + 1]], ic = ring[faces[k + 2]];
        const double signedArea = area(xy(va), xy(vb), xy(vc));
        if (std::abs(signedArea) < 1e-10)
        {
          planar = false;
          break;
        }
        newArea += std::abs(signedArea);
        if (signedArea < 0)
          std::swap(ib, ic);
        cavity.insert(cavity.end(), {ia, ib, ic});
        const auto at = bary(xy(v[id]), xy(va), xy(vb), xy(vc));
        if (*std::min_element(at.begin(), at.end()) >= -1e-8)
        {
          found = true;
          const double z = at[0] * va.y + at[1] * vb.y + at[2] * vc.y;
          const double allowed = vertexBudget(id);
          planar &= std::abs(z - v[id].y) <= allowed && std::abs(z - height(xy(v[id]))) <= allowed;
        }
        for (const auto w : std::array<std::array<double, 3>, 7>{{{1. / 3, 1. / 3, 1. / 3}, {.5, .5, 0}, {.5, 0, .5}, {0, .5, .5}, {.6, .2, .2}, {.2, .6, .2}, {.2, .2, .6}}})
        {
          const Point2 p{w[0] * va.x + w[1] * vb.x + w[2] * vc.x, w[0] * va.z + w[1] * vb.z + w[2] * vc.z};
          planar &= std::abs(w[0] * va.y + w[1] * vb.y + w[2] * vc.y - height(p)) <= budget(p) && material(p) == mat;
          // Seven probes a face, three field queries a probe. The face loop
          // already stops on `planar`; stop inside it too.
          if (!planar)
            break;
        }
      }
      if (!planar || !found || std::abs(newArea - oldArea) > std::max(1e-6, oldArea * 1e-8))
        continue;
      for (auto face : stars[id])
        removeFace[face / 3] = 1;
      replacements.insert(replacements.end(), cavity.begin(), cavity.end());
      replacementMaterials.insert(replacementMaterials.end(), cavity.size() / 3, mat);
      remove[id] = 1;
      ++removed;
      for (auto n : neighbors)
        blocked[n] = 1;
    }
    if (!removed)
      break;
    // Independent stars are replaced locally. All other triangles, feature
    // edges and cavity boundaries survive without a global diagonal flip.
    std::vector<uint32_t> remap(v.size(), UINT32_MAX);
    std::vector<MeshVertex> vertices;
    std::vector<uint8_t> nextLocked;
    for (uint32_t i = 0; i < v.size(); ++i)
      if (!remove[i])
      {
        remap[i] = uint32_t(vertices.size());
        vertices.push_back(v[i]);
        nextLocked.push_back(locked[i]);
      }
    std::vector<uint32_t> indices;
    std::vector<uint8_t> materials;
    for (size_t i = 0; i < tri.size(); i += 3)
      if (!removeFace[i / 3])
      {
        for (int j = 0; j < 3; ++j)
          indices.push_back(remap[tri[i + j]]);
        materials.push_back(tile.materials[i / 3]);
      }
    for (auto id : replacements)
      indices.push_back(remap[id]);
    materials.insert(materials.end(), replacementMaterials.begin(), replacementMaterials.end());
    tile.vertices.swap(vertices);
    tile.indices.swap(indices);
    tile.materials.swap(materials);
    locked.swap(nextLocked);
    tile.simplifiedVertices += removed;
  }
}
} // namespace procedural::geometry
