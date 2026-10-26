#include "procedural/world/finalpatches/PatchAudit.hpp"

#include "procedural/world/finalpatches/PatchGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <sstream>
#include <iomanip>

namespace procedural::world::finalpatches {

using namespace ::procedural::world::rewrite;

namespace {

// Index of every edge by identity. Five of the eight checks want this, and
// building it where it is needed is cheaper to read than carrying one map
// through a hundred and eighty lines.
std::map<uint64_t, const SurfaceMapEdge *> edgeIndex(const WorldFinalPatches &p)
{
  std::map<uint64_t, const SurfaceMapEdge *> edges;

  for (const auto &e : p.map.edges)
  {
    edges.emplace(e.id, &e);
  }

  return edges;
}

// The cut lines of the tile grid, as the coordinate ladders they describe.
void tileLadders(const WorldFinalPatches &p, std::vector<double> &xs, std::vector<double> &zs)
{
  const auto &s = p.map.settings;

  xs.assign(1, s.minX);
  xs.insert(xs.end(), s.cutsX.begin(), s.cutsX.end());
  xs.push_back(s.maxX);

  zs.assign(1, s.minZ);
  zs.insert(zs.end(), s.cutsZ.begin(), s.cutsZ.end());
  zs.push_back(s.maxZ);
}

} // namespace

std::string TileCutsCheck::fault(const WorldFinalPatches &p) const
{
  const auto &settings = p.map.settings;

  const auto valid = [](const std::vector<double> &cuts, double lo, double hi) {
    if (!std::isfinite(lo) || !std::isfinite(hi) || !(hi > lo))
    {
      return false;
    }

    for (const double value : cuts)
    {
      if (!std::isfinite(value) || !(value > lo && value < hi))
      {
        return false;
      }

      lo = value;
    }

    return true;
  };

  if (!valid(settings.cutsX, settings.minX, settings.maxX) || !valid(settings.cutsZ, settings.minZ, settings.maxZ))
  {
    return "invalid final tile partition";
  }

  return {};
}

std::string TopologyCheck::fault(const WorldFinalPatches &p) const
{
  for (size_t i = 0; i < p.map.faces.size(); ++i)
  {
    const auto &face = p.map.faces[i];

    if (face.id != i + 1 || face.vertices.size() < 3 || face.layer > 2 || uint32_t(face.role) > uint32_t(SurfaceRole::Ditch) || !std::isfinite(face.plane.x) || !std::isfinite(face.plane.z) ||
        !std::isfinite(face.plane.constant))
    {
      return "invalid final face metadata";
    }

    for (const auto vertex : face.vertices)
    {
      if (vertex >= p.map.vertices.size())
      {
        return "invalid final face vertex";
      }
    }
  }

  std::set<uint64_t> seen;

  for (const auto &e : p.map.edges)
  {
    if (!e.id || e.leftFace > p.map.faces.size() || e.rightFace > p.map.faces.size() || e.a >= p.map.vertices.size() || e.b >= p.map.vertices.size() || !seen.insert(e.id).second)
    {
      return "invalid final edge identity";
    }
  }

  return {};
}

std::string FrozenVertexCheck::fault(const WorldFinalPatches &p) const
{
  if (p.mesh.patches.size() != p.map.faces.size())
  {
    return "missing final patch";
  }

  if (p.mesh.vertices.size() < p.map.vertices.size())
  {
    return "missing frozen mesh vertices";
  }

  for (size_t i = 0; i < p.map.vertices.size(); ++i)
  {
    if (!samePosition(p.mesh.vertices[i], p.map.vertices[i]))
    {
      return "published mesh moved frozen vertex";
    }
  }

  return {};
}

std::string PublishedTriangleCheck::fault(const WorldFinalPatches &p) const
{
  double groundArea = 0;

  for (const auto &t : p.mesh.triangles)
  {
    if (!t.face || t.face > p.map.faces.size())
    {
      return "invalid published triangle face";
    }

    const auto &f = p.map.faces[t.face - 1];

    if (t.material != f.material || t.layer != f.layer || t.role != f.role)
    {
      return "published material ownership mismatch";
    }

    for (int i = 0; i < 3; ++i)
    {
      if (t.vertices[i] >= p.mesh.vertices.size())
      {
        return "invalid published triangle vertex";
      }
    }

    const auto a = p.mesh.vertices[t.vertices[0]], b = p.mesh.vertices[t.vertices[1]], c = p.mesh.vertices[t.vertices[2]];
    const double area = ((b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z)) * .5;

    if (!std::isfinite(area) || area <= 0)
    {
      return "invalid published triangle area";
    }

    if (!t.layer)
    {
      groundArea += area;
    }
  }

  const auto &bounds = p.map.settings;
  const double expected = (bounds.maxX - bounds.minX) * (bounds.maxZ - bounds.minZ);

  if (!std::isfinite(expected) || expected <= 0 || std::abs(groundArea - expected) > expected * 1e-7)
  {
    return "published ground coverage mismatch";
  }

  return {};
}

std::string CanonicalEdgeCheck::fault(const WorldFinalPatches &p) const
{
  std::map<uint64_t, std::set<std::pair<uint32_t, uint32_t>>> published;

  for (const auto &t : p.mesh.triangles)
  {
    if (!t.face || t.face > p.map.faces.size())
    {
      return "invalid published triangle face";
    }

    for (int i = 0; i < 3; ++i)
    {
      published[t.face].insert(orderedPair(t.vertices[i], t.vertices[(i + 1) % 3]));
    }
  }

  for (const auto &e : p.map.edges)
  {
    for (const auto face : {e.leftFace, e.rightFace})
    {
      if (face && !published[face].count(orderedPair(e.a, e.b)))
      {
        return "published mesh lost canonical edge";
      }
    }
  }

  return {};
}

std::string ConstraintFidelityCheck::fault(const WorldFinalPatches &p) const
{
  const auto edges = edgeIndex(p);

  std::map<uint64_t, std::set<uint64_t>> preserved;
  std::set<uint64_t> patchIds;

  for (const auto &patch : p.mesh.patches)
  {
    if (!patch.patchId || patch.patchId > p.map.faces.size() || !patchIds.insert(patch.patchId).second)
    {
      return "invalid final patch identity";
    }

    std::set<std::pair<uint32_t, uint32_t>> triangleEdges;

    for (const auto &t : patch.triangles)
    {
      for (int i = 0; i < 3; ++i)
      {
        if (t[i] >= patch.vertices.size())
        {
          return "invalid final triangle";
        }

        triangleEdges.insert(orderedPair(t[i], t[(i + 1) % 3]));
      }
    }

    for (const auto &c : patch.constraints)
    {
      const auto it = edges.find(c.sourceEdge);

      if (it == edges.end() || c.a >= patch.vertices.size() || c.b >= patch.vertices.size() || !triangleEdges.count(orderedPair(c.a, c.b)))
      {
        return "CDT constraint missing from triangles";
      }

      const auto &e = *it->second;
      const auto a = patch.vertices[c.a].position, b = patch.vertices[c.b].position;
      const auto u = p.map.vertices[e.a], v = p.map.vertices[e.b];

      // Canonical edges are already split at every station, so each is one
      // CDT edge and must land on the canonical endpoints exactly.
      if (!((samePosition(a, u) && samePosition(b, v)) || (samePosition(a, v) && samePosition(b, u))))
      {
        return "CDT moved canonical boundary";
      }

      preserved[e.id].insert(patch.patchId);
    }
  }

  for (const auto &[id, e] : edges)
  {
    for (const auto face : {e->leftFace, e->rightFace})
    {
      if (face && !preserved[id].count(face))
      {
        return "canonical boundary lost by CDT";
      }
    }
  }

  return {};
}

std::string SourceChainCheck::fault(const WorldFinalPatches &p) const
{
  const auto edges = edgeIndex(p);

  std::set<std::tuple<uint64_t, uint64_t, uint32_t>> keys;

  for (const auto &source : p.sources)
  {
    if (!source.producer || !source.contour || !keys.emplace(source.producer, source.contour, source.segment).second)
    {
      return "duplicate final source identity";
    }

    std::vector<std::pair<double, double>> ranges;
    std::set<uint64_t> unique;

    for (const auto id : source.edges)
    {
      const auto it = edges.find(id);
      double lo = 0, hi = 0;

      if (it == edges.end() || !unique.insert(id).second)
      {
        return "invalid source chain edge";
      }

      const auto &e = *it->second;

      if (e.layer != source.layer || std::find(e.sourceContours.begin(), e.sourceContours.end(), source.canonicalContour) == e.sourceContours.end() ||
          !projectInterval(source.from, source.to, p.map.vertices[e.a], p.map.vertices[e.b], lo, hi))
      {
        return "source chain moved or lost provenance";
      }

      ranges.emplace_back(lo, hi);
    }

    std::sort(ranges.begin(), ranges.end());
    double end = 0;

    for (const auto [lo, hi] : ranges)
    {
      if (lo > end + kTolerance || lo < end - kTolerance)
      {
        std::ostringstream message;
        message << std::setprecision(17) << "source chain gap or overlap: producer=" << source.producer
                << " contour=" << source.contour << " segment=" << source.segment
                << " expected=" << end << " actual=" << lo << " next=" << hi
                << " from=(" << source.from.x << ',' << source.from.z << ") to=(" << source.to.x << ',' << source.to.z << ')';
        return message.str();
      }

      end = hi;
    }

    if (ranges.empty() || std::abs(end - std::hypot(source.to.x - source.from.x, source.to.z - source.from.z)) > kTolerance)
    {
      return "source boundary not fully preserved";
    }
  }

  return {};
}

std::string TilePartitionCheck::fault(const WorldFinalPatches &p) const
{
  std::vector<double> xs, zs;
  tileLadders(p, xs, zs);

  if (p.tiles.size() != (xs.size() - 1) * (zs.size() - 1))
  {
    return "missing final tile";
  }

  std::map<uint64_t, size_t> faceTiles;

  for (size_t i = 0; i < p.tiles.size(); ++i)
  {
    for (const auto face : p.tiles[i].faces)
    {
      if (!face || face > p.map.faces.size() || !faceTiles.emplace(face, i).second)
      {
        return "invalid tile face ownership";
      }
    }
  }

  if (faceTiles.size() != p.map.faces.size())
  {
    return "missing tile face";
  }

  // A tile's seam manifest is exactly the edges whose two faces fall in
  // different tiles. It is what lets a tile be rebuilt without its neighbours.
  for (size_t i = 0; i < p.tiles.size(); ++i)
  {
    std::set<uint64_t> expected;

    for (const auto &e : p.map.edges)
    {
      const auto left = e.leftFace ? faceTiles.at(e.leftFace) : SIZE_MAX;
      const auto right = e.rightFace ? faceTiles.at(e.rightFace) : SIZE_MAX;

      if (left != right && (left == i || right == i))
      {
        expected.insert(e.id);
      }
    }

    const auto &listed = p.tiles[i].boundaryEdges;

    if (expected != std::set<uint64_t>(listed.begin(), listed.end()) || expected.size() != listed.size())
    {
      return "tile seam manifest mismatch";
    }
  }

  std::vector<uint32_t> counts(p.mesh.triangles.size());
  std::set<std::pair<uint32_t, uint32_t>> tileKeys;

  for (const auto &tile : p.tiles)
  {
    if (uint64_t(tile.x) + 1 >= xs.size() || uint64_t(tile.z) + 1 >= zs.size() || !tileKeys.emplace(tile.x, tile.z).second)
    {
      return "invalid final tile";
    }

    for (const auto i : tile.triangles)
    {
      if (i >= counts.size() || ++counts[i] != 1)
      {
        return "duplicate tile triangle";
      }

      for (const auto v : p.mesh.triangles[i].vertices)
      {
        if (v >= p.mesh.vertices.size())
        {
          return "invalid tile vertex";
        }

        const auto a = p.mesh.vertices[v];

        if (a.x < xs[tile.x] - kTolerance || a.x > xs[tile.x + 1] + kTolerance || a.z < zs[tile.z] - kTolerance || a.z > zs[tile.z + 1] + kTolerance)
        {
          return "triangle crosses tile seam";
        }
      }
    }
  }

  if (std::find(counts.begin(), counts.end(), 0) != counts.end())
  {
    return "unassigned tile triangle";
  }

  return {};
}

const std::vector<CheckPtr> &worldPatchChecks()
{
  static const std::vector<CheckPtr> checks = {
    std::make_shared<TileCutsCheck>(),
    std::make_shared<TopologyCheck>(),
    std::make_shared<FrozenVertexCheck>(),
    std::make_shared<PublishedTriangleCheck>(),
    std::make_shared<CanonicalEdgeCheck>(),
    std::make_shared<ConstraintFidelityCheck>(),
    std::make_shared<SourceChainCheck>(),
    std::make_shared<TilePartitionCheck>(),
  };

  return checks;
}

} // namespace procedural::world::finalpatches
