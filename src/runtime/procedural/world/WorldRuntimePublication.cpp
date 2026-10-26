#include "WorldRuntimePublication.hpp"
#include "WorldRuntimeCover.hpp"
#include "json.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

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

using nlohmann::json;

json dependencies(RuntimeDependencies d)
{
  return {d.road, d.discharge, d.spline, d.settlement, d.cover};
}

json interfaces(const WorldFinalPatches &p)
{
  json j;
  for (auto v : p.map.vertices)
  {
    j["vertices"].push_back({v.x, v.y, v.z});
  }
  for (auto e : p.map.edges)
  {
    j["edges"].push_back({e.id, e.a, e.b, e.layer, e.leftFace, e.rightFace, e.sourceContours});
  }
  for (auto f : p.map.faces)
  {
    j["faces"].push_back({f.id, f.sourceContour, f.material, f.layer, int(f.role), f.terrainSurface, f.plane.x, f.plane.z, f.plane.constant, f.vertices});
  }
  for (auto s : p.sources)
  {
    j["sources"].push_back({s.producer, s.contour, s.canonicalContour, s.segment, s.layer, s.from.x, s.from.y, s.from.z, s.to.x, s.to.y, s.to.z, s.edges});
  }
  for (auto t : p.tiles)
  {
    j["tiles"].push_back({t.x, t.z, t.faces, t.boundaryEdges});
  }
  const auto &s = p.map.settings;
  j["bounds"] = {s.minX, s.minZ, s.maxX, s.maxZ, s.cutsX, s.cutsZ};
  return j;
}

// Conservative XZ bounds deliberately include touching faces: extra retained
// geometry is preferable to a water/ground occlusion change at a tile interface.
std::vector<bool> protectedFaces(const SurfaceArrangement &map)
{
  struct Bounds
  {
    double x0 = INFINITY, z0 = INFINITY, x1 = -INFINITY, z1 = -INFINITY;
  };

  std::vector<Bounds> bounds(map.faces.size() + 1);
  std::vector<bool> protectedFace(map.faces.size() + 1, false);
  for (const auto &f : map.faces)
  {
    auto &b = bounds[f.id];
    for (auto id : f.vertices)
    {
      const auto &v = map.vertices[id];
      b.x0 = std::min(b.x0, v.x);
      b.x1 = std::max(b.x1, v.x);
      b.z0 = std::min(b.z0, v.z);
      b.z1 = std::max(b.z1, v.z);
    }
    protectedFace[f.id] = f.role != SurfaceRole::Ground;
  }
  for (const auto &water : map.faces)
  {
    if (water.role == SurfaceRole::Water)
    {
      for (const auto &ground : map.faces)
      {
        if (ground.layer == 0)
        {
          const auto &a = bounds[water.id], &b = bounds[ground.id];
          if (a.x0 <= b.x1 && b.x0 <= a.x1 && a.z0 <= b.z1 && b.z0 <= a.z1)
          {
            protectedFace[ground.id] = true;
          }
        }
      }
    }
  }
  return protectedFace;
}

json protectedPayload(const RefinedWorldSurfaces &level, const std::vector<bool> &mask)
{
  json result = json::array();
  const auto &mesh = level.world.mesh;
  for (size_t i = 0; i < mesh.triangles.size(); ++i)
  {
    const auto &t = mesh.triangles[i];
    if (!mask[t.face])
    {
      continue;
    }
    json points = json::array();
    for (auto id : t.vertices)
    {
      const auto &v = mesh.vertices[id];
      points.push_back({v.x, v.y, v.z});
    }
    result.push_back({t.face, t.material, t.layer, int(t.role), points, level.triangleHeightErrors[i], level.triangleNormalErrors[i]});
  }
  return result;
}

void retainPhysicalFeatures(const RefinedWorldSurfaces &fine, RefinedWorldSurfaces &coarse)
{
  const auto mask = protectedFaces(fine.world.map);
  SurfaceArrangementMesh mesh;
  mesh.vertices = fine.world.map.vertices;
  mesh.groundArea = coarse.world.mesh.groundArea;
  std::vector<double> heights, normals;
  std::vector<std::vector<size_t>> fineTriangles(mask.size()), coarseTriangles(mask.size());
  for (size_t i = 0; i < fine.world.mesh.triangles.size(); ++i)
  {
    fineTriangles[fine.world.mesh.triangles[i].face].push_back(i);
  }
  for (size_t i = 0; i < coarse.world.mesh.triangles.size(); ++i)
  {
    coarseTriangles[coarse.world.mesh.triangles[i].face].push_back(i);
  }
  // Face-local indexing keeps canonical vertices shared and copies each chosen
  // patch's interior unchanged, including its measured error certificates.
  for (const auto &face : fine.world.map.faces)
  {
    const auto &source = mask[face.id] ? fine : coarse;
    mesh.patches.push_back(source.world.mesh.patches[face.id - 1]);
    std::map<uint32_t, uint32_t> remap;
    const auto &indices = mask[face.id] ? fineTriangles[face.id] : coarseTriangles[face.id];
    for (size_t i : indices)
    {
      auto t = source.world.mesh.triangles[i];
      for (auto &id : t.vertices)
      {
        if (id >= fine.world.map.vertices.size())
        {
          auto [it, added] = remap.emplace(id, uint32_t(mesh.vertices.size()));
          if (added)
          {
            mesh.vertices.push_back(source.world.mesh.vertices[id]);
          }
          id = it->second;
        }
      }
      mesh.triangles.push_back(t);
      heights.push_back(source.triangleHeightErrors[i]);
      normals.push_back(source.triangleNormalErrors[i]);
    }
  }
  coarse.world.mesh = std::move(mesh);
  coarse.triangleHeightErrors = std::move(heights);
  coarse.triangleNormalErrors = std::move(normals);
  coarse.maximumHeightError = *std::max_element(coarse.triangleHeightErrors.begin(), coarse.triangleHeightErrors.end());
  coarse.maximumNormalError = *std::max_element(coarse.triangleNormalErrors.begin(), coarse.triangleNormalErrors.end());
  std::vector<size_t> tileForFace(mask.size());
  for (size_t i = 0; i < coarse.world.tiles.size(); ++i)
  {
    auto &tile = coarse.world.tiles[i];
    tile.triangles.clear();
    for (auto f : tile.faces)
    {
      tileForFace[f] = i;
    }
  }
  for (uint32_t i = 0; i < coarse.world.mesh.triangles.size(); ++i)
  {
    coarse.world.tiles[tileForFace[coarse.world.mesh.triangles[i].face]].triangles.push_back(i);
  }
}

uint64_t resident(const RuntimeWorldAssets &a)
{
  uint64_t count = a.structures.triangles.size();
  for (const auto &l : a.levels)
  {
    count += l.world.mesh.triangles.size();
  }
  return count;
}
} // namespace

bool validateRuntimeAssets(const RuntimeWorldAssets &assets, std::string *error)
{
  if (assets.levels.empty() || assets.levels.size() > 4)
  {
    return fail(error, "runtime supports one to four fixed-interface LODs");
  }
  auto d = assets.dependencies;
  if (!d.road || !d.discharge || !d.spline || !d.settlement || !d.cover)
  {
    return fail(error, "runtime requires explicit dependency versions");
  }
  const auto &base = assets.levels[0].world;
  auto revision = base.map.revision;
  if (base.tiles.empty() || base.tiles.size() > 4096 || assets.coverPages.size() != base.tiles.size())
  {
    return fail(error, "incomplete runtime tile pages");
  }
  json canonical = interfaces(base);
  for (const auto &l : assets.levels)
  {
    if (l.world.map.revision != revision || !auditWorldFinalPatches(l.world, error))
    {
      return fail(error, "invalid runtime LOD geometry");
    }
    for (auto v : l.world.mesh.vertices)
    {
      if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
      {
        return fail(error, "nonfinite runtime vertex");
      }
    }
    if (interfaces(l.world) != canonical)
    {
      return fail(error, "runtime LOD changed a source interface");
    }
    if (!std::isfinite(l.maximumHeightError) || !std::isfinite(l.maximumNormalError) || l.maximumHeightError < 0 || l.maximumNormalError < 0 || l.triangleHeightErrors.size() != l.world.mesh.triangles.size() ||
        l.triangleNormalErrors.size() != l.world.mesh.triangles.size())
    {
      return fail(error, "missing runtime error measurements");
    }
    double maxHeight = 0, maxNormal = 0;
    for (double v : l.triangleHeightErrors)
    {
      if (!std::isfinite(v) || v < 0)
      {
        return fail(error, "invalid runtime height error");
      }
      maxHeight = std::max(maxHeight, v);
    }
    for (double v : l.triangleNormalErrors)
    {
      if (!std::isfinite(v) || v < 0 || v > 180)
      {
        return fail(error, "invalid runtime normal error");
      }
      maxNormal = std::max(maxNormal, v);
    }
    if (maxHeight != l.maximumHeightError || maxNormal != l.maximumNormalError)
    {
      return fail(error, "runtime error summary mismatch");
    }
  }
  const auto mask = protectedFaces(base.map);
  const auto physical = protectedPayload(assets.levels[0], mask);
  for (size_t i = 1; i < assets.levels.size(); ++i)
  {
    if (protectedPayload(assets.levels[i], mask) != physical)
    {
      return fail(error, "runtime LOD changed protected physical geometry");
    }
  }
  const auto &structure = assets.structures;
  if (structure.triangles.size() != assets.structuralMaterials.size() || structure.triangles.size() != structure.expectedNormals.size())
  {
    return fail(error, "incomplete runtime structural metadata");
  }
  for (auto v : structure.vertices)
  {
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
    {
      return fail(error, "nonfinite runtime structure");
    }
  }
  for (size_t i = 0; i < structure.triangles.size(); ++i)
  {
    const auto t = structure.triangles[i];
    for (auto v : t)
    {
      if (v >= structure.vertices.size())
      {
        return fail(error, "invalid runtime structure index");
      }
    }
    const auto a = structure.vertices[t[0]], b = structure.vertices[t[1]], c = structure.vertices[t[2]], n = structure.expectedNormals[i];
    const Position cross{(b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y), (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z), (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)};
    if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z) || !(cross.x * n.x + cross.y * n.y + cross.z * n.z > 1e-12))
    {
      return fail(error, "degenerate or inverted runtime structure");
    }
  }
  uint64_t totalSamples = 0;
  std::map<std::pair<int64_t, int64_t>, const CoverSample *> sharedSamples;
  std::vector<double> xs{base.map.settings.minX}, zs{base.map.settings.minZ};
  xs.insert(xs.end(), base.map.settings.cutsX.begin(), base.map.settings.cutsX.end());
  xs.push_back(base.map.settings.maxX);
  zs.insert(zs.end(), base.map.settings.cutsZ.begin(), base.map.settings.cutsZ.end());
  zs.push_back(base.map.settings.maxZ);
  for (size_t index = 0; index < assets.coverPages.size(); ++index)
  {
    const auto &versioned = assets.coverPages[index];
    if (versioned.revision != revision || !(versioned.dependencies == assets.dependencies))
    {
      return fail(error, "runtime cover page has stale dependencies");
    }
    const auto &page = versioned.page;
    if (!page.width || !page.height || page.width > 2048 || page.height > 2048 || page.gutter > 16 || !std::isfinite(page.spacingMetres) || page.spacingMetres <= 0 || !std::isfinite(page.footprintMetres) ||
        page.footprintMetres < 0 || page.samples.size() != uint64_t(page.width + page.gutter * 2) * (page.height + page.gutter * 2))
    {
      return fail(error, "invalid runtime cover page");
    }
    if ((totalSamples += page.samples.size()) > 2000000)
    {
      return fail(error, "runtime cover sample budget exceeded");
    }
    const auto &tile = base.tiles[index];
    if (tile.x + 1 >= xs.size() || tile.z + 1 >= zs.size() || std::abs(double(page.originX)) > 1e9 || std::abs(double(page.originZ)) > 1e9 || page.originX * page.spacingMetres > xs[tile.x] + 1e-8 ||
        (page.originX + double(page.width) - 1) * page.spacingMetres < xs[tile.x + 1] - 1e-8 || page.originZ * page.spacingMetres > zs[tile.z] + 1e-8 ||
        (page.originZ + double(page.height) - 1) * page.spacingMetres < zs[tile.z + 1] - 1e-8)
    {
      return fail(error, "runtime cover page misses its tile");
    }
    if (page.spacingMetres != assets.coverPages[0].page.spacingMetres || page.footprintMetres != assets.coverPages[0].page.footprintMetres)
    {
      return fail(error, "runtime page lattices or footprints disagree");
    }
    size_t sampleIndex = 0;
    for (const auto &s : page.samples)
    {
      int64_t px = page.originX + int64_t(sampleIndex % (page.width + 2 * page.gutter)) - page.gutter;
      int64_t pz = page.originZ + int64_t(sampleIndex / (page.width + 2 * page.gutter)) - page.gutter;
      ++sampleIndex;
      auto [it, inserted] = sharedSamples.emplace(std::make_pair(px, pz), &s);
      if (!inserted)
      {
        const auto &other = *it->second;
        if (s.weights != other.weights || s.owner != other.owner || s.hard != other.hard || s.variation != other.variation || s.cropRows != other.cropRows)
        {
          return fail(error, "runtime page seam mismatch");
        }
      }
      double sum = 0;
      if (s.weights.empty() || s.weights.size() > 16 || !std::isfinite(s.variation) || !std::isfinite(s.cropRows))
      {
        return fail(error, "invalid runtime cover sample");
      }
      for (auto w : s.weights)
      {
        if (!std::isfinite(w) || w < 0 || w > 1)
        {
          return fail(error, "invalid runtime cover weight");
        }
        sum += w;
      }
      if (std::abs(sum - 1) > 1e-10)
      {
        return fail(error, "unnormalized runtime cover weights");
      }
    }
  }
  return validateRuntimeCoverMips(assets, error);
}

bool buildRuntimeLods(
    const WorldFinalPatches &source,
    const PatchSurface &surface,
    const std::vector<SurfaceFeatureSample> &features,
    const std::vector<SurfaceErrorSettings> &errors,
    const std::vector<SurfaceRefinement> &densities,
    RuntimeDependencies deps,
    std::vector<RuntimeCoverPage> pages,
    RuntimeWorldAssets &out,
    std::string *error)
{
  if (errors.empty() || errors.size() > 4 || errors.size() != densities.size() || !surface)
  {
    return fail(error, "invalid runtime LOD settings");
  }
  if (!auditWorldFinalPatches(source, error))
  {
    return false;
  }
  RuntimeWorldAssets result;
  result.dependencies = deps;
  result.coverPages = std::move(pages);
  for (size_t i = 0; i < errors.size(); ++i)
  {
    if (i && (errors[i].heightMetres < errors[i - 1].heightMetres || errors[i].normalDegrees < errors[i - 1].normalDegrees))
    {
      return fail(error, "coarse LOD error budget must not decrease");
    }
    if (i && (densities[i].groundSpacingMetres < densities[i - 1].groundSpacingMetres || densities[i].modifiedSpacingMetres < densities[i - 1].modifiedSpacingMetres))
    {
      return fail(error, "coarse LOD spacing must not decrease");
    }
    WorldFinalPatches candidate = source;
    // Seed CDT in XZ only. The refinement pass evaluates every retained interior
    // vertex against its actual owning face; never invent a face for the callback.
    auto terrain = [](double, double) {
      return 0.;
    };
    if (!meshSurfaceArrangement(source.map, terrain, densities[i], candidate.mesh, error))
    {
      return false;
    }
    std::vector<size_t> tiles(source.map.faces.size() + 1);
    for (size_t t = 0; t < candidate.tiles.size(); ++t)
    {
      candidate.tiles[t].triangles.clear();
      for (auto f : candidate.tiles[t].faces)
      {
        tiles[f] = t;
      }
    }
    for (uint32_t t = 0; t < candidate.mesh.triangles.size(); ++t)
    {
      candidate.tiles[tiles[candidate.mesh.triangles[t].face]].triangles.push_back(t);
    }
    RefinedWorldSurfaces level;
    if (!refineWorldSurfaces(candidate, surface, features, errors[i], level, error))
    {
      if (error)
      {
        *error += " lod=" + std::to_string(i);
      }
      return false;
    }
    if (i)
    {
      retainPhysicalFeatures(result.levels[0], level);
    }
    result.levels.push_back(std::move(level));
  }
  if (!validateRuntimeAssets(result, error))
  {
    return false;
  }
  out = std::move(result);
  return true;
}

bool queryRuntimeSurface(const RuntimeSnapshot &snapshot, double x, double z, uint32_t layer, RuntimeSurfaceHit &out, std::string *error)
{
  if (!snapshot.assets || snapshot.assets->levels.empty() || !std::isfinite(x) || !std::isfinite(z) || layer > 2)
  {
    return fail(error, "invalid runtime surface query");
  }
  const auto &world = snapshot.assets->levels[0].world;
  for (const auto &t : world.mesh.triangles)
  {
    if (t.layer == layer)
    {
      auto a = world.mesh.vertices[t.vertices[0]], b = world.mesh.vertices[t.vertices[1]], c = world.mesh.vertices[t.vertices[2]];
      double den = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
      double u = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / den, v = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / den, w = 1 - u - v;
      if (u >= -1e-10 && v >= -1e-10 && w >= -1e-10)
      {
        out = {u * a.y + v * b.y + w * c.y, world.map.revision, t.face, t.material};
        return true;
      }
    }
  }
  return fail(error, "runtime query outside requested surface layer");
}

bool WorldRuntimePublication::begin(const RuntimeWorldAssets &assets, std::vector<uint32_t> levels, uint64_t capacity, uint64_t &ticket, std::string *error)
{
  if (!validateRuntimeAssets(assets, error))
  {
    return false;
  }
  if (levels.size() != assets.levels[0].world.tiles.size())
  {
    return fail(error, "runtime selection must cover every tile");
  }
  for (auto l : levels)
  {
    if (l >= assets.levels.size())
    {
      return fail(error, "unsupported runtime LOD");
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t needed = resident(assets) + (visible_ ? resident(*visible_->assets) : 0);
  if (needed > capacity)
  {
    return fail(error, "runtime resident triangle capacity exhausted");
  }
  if (next_ == UINT64_MAX)
  {
    return fail(error, "runtime ticket space exhausted");
  }
  auto owned = std::make_shared<const RuntimeWorldAssets>(assets);
  auto pending = std::make_shared<RuntimeSnapshot>();
  pending->ticket = ++next_;
  pending->assets = std::move(owned);
  pending->tileLevels = std::move(levels);
  pending_ = std::move(pending);
  ready_.assign(pending_->tileLevels.size(), false);
  ticket = pending_->ticket;
  return true;
}

bool WorldRuntimePublication::complete(uint64_t ticket, uint32_t tile, RuntimeDependencies deps, std::string *error)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (!pending_ || pending_->ticket != ticket)
  {
    return fail(error, "stale or cancelled runtime completion");
  }
  if (!(deps == pending_->assets->dependencies))
  {
    return fail(error, "runtime completion has stale dependencies");
  }
  if (tile >= ready_.size())
  {
    return fail(error, "invalid runtime completion tile");
  }
  ready_[tile] = true;
  if (std::all_of(ready_.begin(), ready_.end(), [](bool v) {
        return v;
      }))
  {
    visible_ = pending_;
    pending_.reset();
    ready_.clear();
  }
  return true;
}

bool WorldRuntimePublication::cancel(uint64_t ticket)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (!pending_ || pending_->ticket != ticket)
  {
    return false;
  }
  pending_.reset();
  ready_.clear();
  return true;
}

std::shared_ptr<const RuntimeSnapshot> WorldRuntimePublication::acquire() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return visible_;
}

bool saveRuntimeSelection(const RuntimeSnapshot &s, std::string &out, std::string *error)
{
  if (!s.assets || !validateRuntimeAssets(*s.assets, error))
  {
    return false;
  }
  if (s.tileLevels.size() != s.assets->levels[0].world.tiles.size())
  {
    return fail(error, "invalid saved tile selection");
  }
  for (auto l : s.tileLevels)
  {
    if (l >= s.assets->levels.size())
    {
      return fail(error, "invalid saved LOD");
    }
  }
  json j = {{"schema", 1}, {"revision", s.assets->levels[0].world.map.revision}, {"dependencies", dependencies(s.assets->dependencies)}, {"levels", s.tileLevels}};
  out = j.dump();
  return true;
}

bool loadRuntimeSelection(const std::string &text, const RuntimeWorldAssets &assets, std::vector<uint32_t> &out, std::string *error)
{
  if (text.size() > 65536 || !validateRuntimeAssets(assets, error))
  {
    return fail(error, "invalid runtime selection input");
  }
  try
  {
    auto j = json::parse(text);
    if (j.at("schema") != 1 || j.at("revision") != assets.levels[0].world.map.revision || j.at("dependencies") != dependencies(assets.dependencies))
    {
      return fail(error, "stale runtime selection");
    }
    auto list = j.at("levels");
    if (!list.is_array() || list.size() != assets.levels[0].world.tiles.size())
    {
      return fail(error, "invalid saved tile count");
    }
    std::vector<uint32_t> result;
    for (auto l : list)
    {
      if (!l.is_number_unsigned() || l.get<uint64_t>() >= assets.levels.size())
      {
        return fail(error, "invalid saved LOD");
      }
      result.push_back(l.get<uint32_t>());
    }
    out = std::move(result);
    return true;
  }
  catch (const json::exception &)
  {
    return fail(error, "malformed runtime selection JSON");
  }
}
} // namespace procedural::world::rewrite
