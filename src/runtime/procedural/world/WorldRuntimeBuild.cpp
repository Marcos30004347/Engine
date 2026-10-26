#include "WorldRuntimeBuild.hpp"
#include "WorldRuntimeCover.hpp"
#include "json.hpp"
#include <algorithm>
#include <cmath>
namespace procedural::world::rewrite
{
namespace
{
using nlohmann::json;
json tilePayload(const RuntimeWorldAssets &assets, size_t index)
{
  json payload;
  for (const auto &level : assets.levels)
  {
    json triangles = json::array();
    const auto &world = level.world;
    for (auto ti : world.tiles[index].triangles)
    {
      const auto &t = world.mesh.triangles[ti];
      json xyz = json::array();
      for (auto id : t.vertices)
      {
        auto v = world.mesh.vertices[id];
        xyz.push_back({v.x, v.y, v.z});
      }
      triangles.push_back({t.material, t.layer, int(t.role), xyz, level.triangleHeightErrors[ti], level.triangleNormalErrors[ti]});
    }
    std::sort(triangles.begin(), triangles.end());
    payload["levels"].push_back(std::move(triangles));
  }
  const auto &p = assets.coverPages[index].page;
  payload["page"] = {p.originX, p.originZ, p.width, p.height, p.gutter, p.spacingMetres, p.footprintMetres};
  for (const auto &s : p.samples)
    payload["samples"].push_back({s.owner, s.hard, s.weights, s.variation, s.cropRows});
  for (const auto &d : assets.coverDefinitions)
    payload["definitions"].push_back({d.id, d.materials, d.hard, d.crop, d.rowSpacingMetres, d.rowAngleRadians});
  for (const auto &mip : assets.coverMips)
  {
    const auto &p = mip[index].page;
    json layer = {{"lattice", {p.originX, p.originZ, p.width, p.height, p.gutter, p.spacingMetres, p.footprintMetres}}};
    for (const auto &s : p.samples)
      layer["samples"].push_back({s.owner, s.hard, s.weights, s.variation, s.cropRows});
    payload["mips"].push_back(std::move(layer));
  }
  return payload;
}
json structures(const RuntimeWorldAssets &assets)
{
  json j;
  for (auto v : assets.structures.vertices)
    j["positions"].push_back({v.x, v.y, v.z});
  for (auto n : assets.structures.expectedNormals)
    j["normals"].push_back({n.x, n.y, n.z});
  j["triangles"] = assets.structures.triangles;
  j["materials"] = assets.structuralMaterials;
  return j;
}
} // namespace
bool buildRuntimeWorld(const RuntimeBuildInput &input, const RuntimeWorldAssets *previous, RuntimeBuildResult &out, std::string *error)
{
  auto fail = [&](const char *message)
  {
    if (error)
      *error = message;
    return false;
  };
  if (!input.samplingSeed || !input.terrain || !std::isfinite(input.coverSpacingMetres) || input.coverSpacingMetres <= 0 || input.errors.empty())
    return fail("invalid runtime world build settings");
  if (previous && !validateRuntimeAssets(*previous, error))
    return false;
  WorldFinalPatches frozen;
  SurfaceRefinement initial = input.initial;
  initial.samplingSeed = input.samplingSeed;
  if (!freezeWorldPatches(input.producers, input.map, initial, input.terrain, input.revision, frozen, error))
    return false;
  WorldCoverDetail detail;
  if (!detail.build(frozen, input.covers, input.cover, error))
    return false;
  std::vector<double> xs{input.map.minX}, zs{input.map.minZ};
  xs.insert(xs.end(), frozen.map.settings.cutsX.begin(), frozen.map.settings.cutsX.end());
  xs.push_back(input.map.maxX);
  zs.insert(zs.end(), frozen.map.settings.cutsZ.begin(), frozen.map.settings.cutsZ.end());
  zs.push_back(input.map.maxZ);
  std::vector<RuntimeCoverPage> pages;
  for (const auto &tile : frozen.tiles)
  {
    const double x0 = std::floor(xs[tile.x] / input.coverSpacingMetres), x1 = std::ceil(xs[tile.x + 1] / input.coverSpacingMetres);
    const double z0 = std::floor(zs[tile.z] / input.coverSpacingMetres), z1 = std::ceil(zs[tile.z + 1] / input.coverSpacingMetres);
    if (std::abs(x0) > 1e9 || std::abs(z0) > 1e9 || x1 - x0 > 2047 || z1 - z0 > 2047)
      return fail("runtime cover lattice exceeds budget");
    CoverPage p;
    if (!detail.page(int64_t(x0), int64_t(z0), uint32_t(x1 - x0 + 1), uint32_t(z1 - z0 + 1), 1, input.coverSpacingMetres, input.coverFootprintMetres, p, error))
      return false;
    pages.push_back({input.revision, input.dependencies, std::move(p)});
  }
  PatchSurface surface = input.surface;
  if (!surface)
    surface = [&](const SurfaceMapFace &face, double x, double z)
    {
      return face.terrainSurface ? input.terrain(x, z) : face.plane.height(x, z);
    };
  RuntimeBuildResult result;
  auto densities = input.densities;
  for (auto &density : densities)
    density.samplingSeed = input.samplingSeed;
  if (!buildRuntimeLods(frozen, surface, input.features, input.errors, densities, input.dependencies, std::move(pages), result.assets, error))
    return false;
  if (!buildRuntimeCoverMips(result.assets, detail, input.covers, uint32_t(input.errors.size()), error))
    return false;
  result.assets.structures = input.structures;
  result.assets.structuralMaterials = input.structuralMaterials;
  if (!validateRuntimeAssets(result.assets, error))
    return false;
  const bool sameTiles = previous && previous->levels[0].world.tiles.size() == frozen.tiles.size();
  for (size_t i = 0; i < frozen.tiles.size(); ++i)
    if (!sameTiles || tilePayload(*previous, i) != tilePayload(result.assets, i))
      result.changedTiles.push_back(uint32_t(i));
  result.structuresChanged = !previous || structures(*previous) != structures(result.assets);
  out = std::move(result);
  return true;
}
} // namespace procedural::world::rewrite
