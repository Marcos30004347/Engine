#include "WorldRuntimeDisplay.hpp"
namespace procedural::world::rewrite
{
bool buildRuntimeDisplayMesh(const RuntimeSnapshot &snapshot, uint64_t maximumTriangles, RuntimeDisplayMesh &out, std::string *error)
{
  const auto fail = [&](const char *message)
  {
    if (error)
      *error = message;
    return false;
  };
  if (!snapshot.assets || !validateRuntimeAssets(*snapshot.assets, error))
    return false;
  const auto &assets = *snapshot.assets;
  if (snapshot.tileLevels.size() != assets.levels[0].world.tiles.size())
    return fail("display selection does not cover the world");
  uint64_t count = assets.structures.triangles.size();
  for (size_t i = 0; i < snapshot.tileLevels.size(); ++i)
  {
    const auto lod = snapshot.tileLevels[i];
    if (lod >= assets.levels.size())
      return fail("display selection has invalid LOD");
    count += assets.levels[lod].world.tiles[i].triangles.size();
  }
  if (!count || count > maximumTriangles || count > UINT32_MAX / 3)
    return fail("display triangle budget exceeded");
  RuntimeDisplayMesh result;
  result.vertices.reserve(count * 3);
  result.triangles.reserve(count);
  result.materials.reserve(count);
  auto append = [&](const std::array<uint32_t, 3> &ids, const std::vector<Position> &vertices, uint32_t material)
  {
    const auto first = uint32_t(result.vertices.size());
    for (auto id : ids)
      result.vertices.push_back(vertices[id]);
    result.triangles.push_back({first, first + 1, first + 2});
    result.materials.push_back(material);
  };
  for (size_t i = 0; i < snapshot.tileLevels.size(); ++i)
  {
    const auto &world = assets.levels[snapshot.tileLevels[i]].world;
    for (auto index : world.tiles[i].triangles)
    {
      const auto &t = world.mesh.triangles[index];
      append(t.vertices, world.mesh.vertices, t.material);
    }
  }
  for (size_t i = 0; i < assets.structures.triangles.size(); ++i)
    append(assets.structures.triangles[i], assets.structures.vertices, assets.structuralMaterials[i]);
  out = std::move(result);
  return true;
}
} // namespace procedural::world::rewrite
