#include "procedural/world/finalpatches/FreezeStage.hpp"

#include "procedural/world/finalpatches/PatchGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace procedural::world::finalpatches {

using namespace ::procedural::world::rewrite;

namespace {

bool stationFail(std::string *error, const char *message)
{
  if (error != nullptr)
  {
    *error = message;
  }

  return false;
}

constexpr double tolerance = kTolerance;

} // namespace

bool stationArrangementEdges(SurfaceArrangement &map, const std::function<double(double, double)> &terrain, const SurfaceRefinement &refinement, std::string *error)
{
  if (!(refinement.stationSpacingMetres > 0) || !std::isfinite(refinement.stationSpacingMetres) || !std::isfinite(refinement.stationToleranceMetres) || refinement.stationToleranceMetres <= 0)
  {
    return true;
  }
  auto terrainFace = [&](uint64_t id) {
    return id && id <= map.faces.size() && map.faces[id - 1].terrainSurface;
  };
  auto insertIntoRing = [&](uint64_t faceId, uint32_t a, uint32_t b, uint32_t m) {
    if (!faceId || faceId > map.faces.size())
    {
      return true;
    }
    auto &ring = map.faces[faceId - 1].vertices;
    for (size_t i = 0; i < ring.size(); ++i)
    {
      uint32_t u = ring[i], v = ring[(i + 1) % ring.size()];
      if ((u == a && v == b) || (u == b && v == a))
      {
        ring.insert(ring.begin() + i + 1, m);
        return true;
      }
    }
    return false;
  };
  uint64_t nextEdgeId = 0;
  for (const auto &e : map.edges)
  {
    nextEdgeId = std::max(nextEdgeId, e.id);
  }
  uint32_t inserted = 0;
  std::vector<size_t> work;
  for (size_t i = 0; i < map.edges.size(); ++i)
  {
    work.push_back(i);
  }
  while (!work.empty())
  {
    size_t index = work.back();
    work.pop_back();
    SurfaceMapEdge edge = map.edges[index];
    if (!terrainFace(edge.leftFace) && !terrainFace(edge.rightFace))
    {
      continue;
    }
    if (edge.a >= map.vertices.size() || edge.b >= map.vertices.size() || edge.a >= edge.b)
    {
      return stationFail(error, "frozen edge is not canonical");
    }
    Position a = map.vertices[edge.a], b = map.vertices[edge.b];
    double length = std::hypot(b.x - a.x, b.z - a.z);
    if (length <= refinement.stationSpacingMetres)
    {
      continue;
    }
    double ya = terrain(a.x, a.z), yb = terrain(b.x, b.z);
    if (!std::isfinite(ya) || !std::isfinite(yb))
    {
      return stationFail(error, "nonfinite terrain along a frozen edge");
    }
    bool split = false;
    int probes = std::max(4, int(std::ceil(length / refinement.stationSpacingMetres)) * 2);
    for (int k = 1; k < probes && !split; ++k)
    {
      double t = double(k) / probes;
      double y = terrain(a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t);
      split = std::abs(y - (ya + (yb - ya) * t)) > refinement.stationToleranceMetres;
    }
    if (!split)
    {
      continue;
    }
    if (++inserted > refinement.maximumStations)
    {
      return stationFail(error, "frozen station budget exceeded");
    }
    // Frozen vertices carry their height; a terrain face beside the edge
    // owns it, matching the arrangement's own assignment rule.
    Position mid{(a.x + b.x) * .5, 0, (a.z + b.z) * .5};
    mid.y = terrain(mid.x, mid.z);
    if (!std::isfinite(mid.y))
    {
      return stationFail(error, "nonfinite terrain at a frozen station");
    }
    uint32_t m = uint32_t(map.vertices.size());
    map.vertices.push_back(mid);
    // Edges are canonical with a < b; the new vertex has the largest index,
    // so the second piece is stored reversed with its faces swapped.
    SurfaceMapEdge second = edge;
    second.id = ++nextEdgeId;
    second.a = edge.b;
    second.b = m;
    std::swap(second.leftFace, second.rightFace);
    map.edges[index].b = m;
    if (!insertIntoRing(edge.leftFace, edge.a, edge.b, m) || !insertIntoRing(edge.rightFace, edge.a, edge.b, m))
    {
      return stationFail(error, "frozen station could not join its face rings");
    }
    map.edges.push_back(second);
    work.push_back(index);
    work.push_back(map.edges.size() - 1);
  }
  return true;
}

bool OrderBatchesStage::run(FreezeBuild &build, std::string *error)
{
  if (build.batches.size() > 8192)
  {
    return stationFail(error, "final producer budget exceeded");
  }

  std::set<uint64_t> producers;

  for (const auto &b : build.batches)
  {
    if (!b.producer || !producers.insert(b.producer).second)
    {
      return stationFail(error, "invalid final producer identity");
    }
  }

  // Priority is ownership; producer breaks the tie so the result does not
  // depend on which worker finished first.
  std::sort(build.batches.begin(), build.batches.end(), [](const auto &a, const auto &b) {
    return std::tie(a.priority, a.producer) < std::tie(b.priority, b.producer);
  });

  return true;
}

bool NormaliseCutsStage::run(FreezeBuild &build, std::string *error)
{
  for (auto *cuts : {&build.settings.cutsX, &build.settings.cutsZ})
  {
    for (const double v : *cuts)
    {
      if (!std::isfinite(v))
      {
        return stationFail(error, "nonfinite tile cut");
      }
    }

    std::sort(cuts->begin(), cuts->end());
    cuts->erase(std::unique(cuts->begin(), cuts->end()), cuts->end());
  }

  return true;
}

bool CollectContoursStage::run(FreezeBuild &build, std::string *error)
{
  for (auto &batch : build.batches)
  {
    std::set<uint64_t> local;

    for (auto c : batch.contours)
    {
      if (!c.id || !local.insert(c.id).second)
      {
        return stationFail(error, "duplicate local contour identity");
      }

      if (c.loop.size() < 3 || c.loop.size() > 256)
      {
        return stationFail(error, "final contour vertex budget exceeded");
      }

      // The contour keeps its producer-local identity in the source chain and
      // takes a global one for the arrangement, so two producers numbering
      // their contours from one cannot collide.
      const auto original = c.id;
      c.id = build.contours.size() + 1;

      for (uint32_t i = 0; i < c.loop.size(); ++i)
      {
        build.result.sources.push_back({batch.producer, original, c.id, i, c.layer, c.loop[i], c.loop[(i + 1) % c.loop.size()], {}});
      }

      build.contours.push_back(std::move(c));

      if (build.contours.size() > 8192)
      {
        return stationFail(error, "final contour budget exceeded");
      }
    }
  }

  return true;
}

bool ArrangeAndMeshStage::run(FreezeBuild &build, std::string *error)
{
  if (!arrangeSurfaceContours(build.contours, build.settings, build.terrain, build.revision, build.result.map, error))
  {
    return false;
  }

  if (!stationArrangementEdges(build.result.map, build.terrain, build.refinement, error))
  {
    return false;
  }

  return meshSurfaceArrangement(build.result.map, build.terrain, build.refinement, build.result.mesh, error);
}

bool LinkProvenanceStage::run(FreezeBuild &build, std::string *)
{
  std::map<uint64_t, std::vector<const SurfaceMapEdge *>> bySource;

  for (const auto &e : build.result.map.edges)
  {
    for (const auto source : e.sourceContours)
    {
      bySource[source].push_back(&e);
    }
  }

  for (auto &source : build.result.sources)
  {
    std::vector<std::pair<double, uint64_t>> ordered;

    for (const auto *e : bySource[source.canonicalContour])
    {
      double lo = 0, hi = 0;

      if (projectInterval(source.from, source.to, build.result.map.vertices[e->a], build.result.map.vertices[e->b], lo, hi))
      {
        ordered.emplace_back(lo, e->id);
      }
    }

    // Ordered along the segment, so the chain reads from one end to the other
    // and a gap in it is a gap in coverage rather than an artefact of order.
    std::sort(ordered.begin(), ordered.end());

    for (const auto [station, id] : ordered)
    {
      source.edges.push_back(id);
    }
  }

  return true;
}

bool TileStage::run(FreezeBuild &build, std::string *)
{
  auto &result = build.result;
  const auto &settings = build.settings;

  const auto nx = settings.cutsX.size() + 1, nz = settings.cutsZ.size() + 1;

  for (uint32_t z = 0; z < nz; ++z)
  {
    for (uint32_t x = 0; x < nx; ++x)
    {
      result.tiles.push_back({x, z, {}, {}, {}});
    }
  }

  // A face goes whole to the tile its centre falls in. Splitting it would
  // create geometry no producer authored.
  std::vector<size_t> faceTile(result.map.faces.size() + 1);

  for (const auto &f : result.map.faces)
  {
    Position center{};

    for (const auto v : f.vertices)
    {
      center.x += result.map.vertices[v].x / f.vertices.size();
      center.z += result.map.vertices[v].z / f.vertices.size();
    }

    const auto x = std::upper_bound(settings.cutsX.begin(), settings.cutsX.end(), center.x) - settings.cutsX.begin();
    const auto z = std::upper_bound(settings.cutsZ.begin(), settings.cutsZ.end(), center.z) - settings.cutsZ.begin();

    faceTile[f.id] = z * nx + x;
    result.tiles[z * nx + x].faces.push_back(f.id);
  }

  // The seam manifest: edges whose two faces landed in different tiles. This
  // is what lets a tile be rebuilt without its neighbours.
  for (const auto &e : result.map.edges)
  {
    const auto left = e.leftFace ? faceTile[e.leftFace] : SIZE_MAX;
    const auto right = e.rightFace ? faceTile[e.rightFace] : SIZE_MAX;

    if (left != right)
    {
      if (left != SIZE_MAX)
      {
        result.tiles[left].boundaryEdges.push_back(e.id);
      }

      if (right != SIZE_MAX)
      {
        result.tiles[right].boundaryEdges.push_back(e.id);
      }
    }
  }

  for (uint32_t i = 0; i < result.mesh.triangles.size(); ++i)
  {
    result.tiles[faceTile[result.mesh.triangles[i].face]].triangles.push_back(i);
  }

  return true;
}

const std::vector<FreezeStagePtr> &freezePipeline()
{
  static const std::vector<FreezeStagePtr> stages = {
    std::make_shared<OrderBatchesStage>(),
    std::make_shared<NormaliseCutsStage>(),
    std::make_shared<CollectContoursStage>(),
    std::make_shared<ArrangeAndMeshStage>(),
    std::make_shared<LinkProvenanceStage>(),
    std::make_shared<TileStage>(),
  };

  return stages;
}

} // namespace procedural::world::finalpatches
