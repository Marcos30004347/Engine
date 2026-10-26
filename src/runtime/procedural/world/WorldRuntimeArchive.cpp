#include "WorldRuntimeArchive.hpp"
#include "json.hpp"
#include <cmath>
namespace procedural::world::rewrite
{
using nlohmann::json;
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Position, x, y, z)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BoundaryVertex, owner, index, position)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PatchConstraint, sourceEdge, a, b)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PatchMesh, patchId, vertices, triangles, constraints, domainArea, triangleArea)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SurfacePlane, x, z, constant)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SurfaceMapSettings, minX, minZ, maxX, maxZ, seamToleranceMetres, maximumFaces, cutsX, cutsZ)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SurfaceMapFace, id, sourceContour, material, layer, role, terrainSurface, plane, vertices)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SurfaceMapEdge, id, a, b, layer, leftFace, rightFace, sourceContours, materialBoundary, surfaceBreak)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SurfaceArrangement, revision, vertices, faces, edges, settings)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SurfaceTriangle, vertices, face, material, layer, role)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SurfaceArrangementMesh, vertices, triangles, patches, groundArea)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WorldSourceChain, producer, contour, canonicalContour, segment, layer, from, to, edges)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WorldPatchTile, x, z, faces, boundaryEdges, triangles)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WorldFinalPatches, map, mesh, sources, tiles)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RefinedWorldSurfaces, world, triangleHeightErrors, triangleNormalErrors, maximumHeightError, maximumNormalError, addedVertices, passes)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RuntimeDependencies, road, discharge, spline, settlement, cover)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CoverSample, weights, owner, hard, variation, cropRows)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CoverPage, originX, originZ, width, height, gutter, spacingMetres, footprintMetres, samples)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RuntimeCoverPage, revision, dependencies, page)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SplineFaceMesh, vertices, triangles, expectedNormals)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CoverDefinition, id, materials, hard, crop, rowSpacingMetres, rowAngleRadians)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(RuntimeWorldAssets, dependencies, levels, coverPages, coverDefinitions, coverMips, structures, structuralMaterials)
namespace
{
constexpr size_t maximumBytes = 64 * 1024 * 1024;
bool fail(std::string *error, const char *message)
{
  if (error)
    *error = message;
  return false;
}
// Validate scalar types before narrowing numeric fields into C++ integers.
// Unknown enum values and negative/fractional indices must never wrap into IDs.
bool scalarTypes(const json &j)
{
  if (j.is_object())
  {
    for (auto it = j.begin(); it != j.end(); ++it)
    {
      const auto &key = it.key();
      const auto &v = it.value();
      if (key == "role" && (!v.is_number_unsigned() || v.get<uint64_t>() > uint64_t(SurfaceRole::Ditch)))
        return false;
      if (!scalarTypes(v))
        return false;
    }
  }
  else if (j.is_array())
  {
    for (const auto &v : j)
      if (!scalarTypes(v))
        return false;
  }
  else if (j.is_number_float() && !std::isfinite(j.get<double>()))
    return false;
  return true;
}
bool safeIndices(const RuntimeWorldAssets &assets)
{
  if (assets.levels.empty() || assets.levels.size() > 4)
    return false;
  uint64_t totalTriangles = 0;
  for (const auto &level : assets.levels)
  {
    const auto &w = level.world;
    if (w.map.faces.empty() || w.map.faces.size() > 20000 || w.mesh.patches.size() != w.map.faces.size() || w.map.vertices.size() > 1000000 || w.mesh.vertices.size() > 2000000 ||
        (totalTriangles += w.mesh.triangles.size()) > 2000000)
      return false;
    for (size_t i = 0; i < w.map.faces.size(); ++i)
    {
      const auto &f = w.map.faces[i];
      if (f.id != i + 1 || f.vertices.size() < 3 || f.layer > 2 || w.mesh.patches[i].patchId != f.id)
        return false;
      for (auto v : f.vertices)
        if (v >= w.map.vertices.size())
          return false;
    }
    std::vector<json> globalTriangles(w.map.faces.size()), patchTriangles(w.map.faces.size());
    for (const auto &t : w.mesh.triangles)
    {
      if (!t.face || t.face > w.map.faces.size())
        return false;
      json points = json::array();
      for (auto v : t.vertices)
      {
        if (v >= w.mesh.vertices.size())
          return false;
        const auto p = w.mesh.vertices[v];
        points.push_back({p.x, p.y, p.z});
      }
      std::sort(points.begin(), points.end());
      globalTriangles[t.face - 1].push_back(points);
    }
    for (size_t i = 0; i < w.mesh.patches.size(); ++i)
    {
      const auto &patch = w.mesh.patches[i];
      if (!std::isfinite(patch.domainArea) || !std::isfinite(patch.triangleArea) || patch.domainArea <= 0 || patch.triangleArea <= 0)
        return false;
      for (const auto &t : patch.triangles)
      {
        json points = json::array();
        for (auto v : t)
        {
          if (v >= patch.vertices.size())
            return false;
          const auto p = patch.vertices[v].position;
          points.push_back({p.x, p.y, p.z});
        }
        std::sort(points.begin(), points.end());
        patchTriangles[i].push_back(points);
      }
      std::sort(globalTriangles[i].begin(), globalTriangles[i].end());
      std::sort(patchTriangles[i].begin(), patchTriangles[i].end());
      if (globalTriangles[i] != patchTriangles[i])
        return false;
    }
    for (const auto &e : w.map.edges)
      if (e.a >= w.map.vertices.size() || e.b >= w.map.vertices.size() || e.leftFace > w.map.faces.size() || e.rightFace > w.map.faces.size())
        return false;
    for (const auto &t : w.mesh.triangles)
    {
      if (!t.face || t.face > w.map.faces.size())
        return false;
      for (auto v : t.vertices)
        if (v >= w.mesh.vertices.size())
          return false;
    }
  }
  return true;
}
} // namespace
bool saveRuntimeArchive(const RuntimeSnapshot &snapshot, std::string &out, std::string *error)
{
  try
  {
    std::string selection;
    if (!snapshot.assets || !safeIndices(*snapshot.assets) || !saveRuntimeSelection(snapshot, selection, error))
      return false;
    json j = {{"schema", 1}, {"kind", "terrain-rewrite-compiled-world"}, {"assets", *snapshot.assets}, {"selection", json::parse(selection)}};
    auto encoded = j.dump();
    if (encoded.size() > maximumBytes)
      return fail(error, "runtime archive exceeds 64 MiB budget");
    out = std::move(encoded);
    return true;
  }
  catch (const std::exception &)
  {
    return fail(error, "runtime archive encoding failed");
  }
}
bool loadRuntimeArchive(const std::string &encoded, RuntimeSnapshot &out, std::string *error)
{
  if (encoded.empty() || encoded.size() > maximumBytes)
    return fail(error, "runtime archive exceeds 64 MiB budget");
  // Bound nesting before the recursive DOM/parser/serializer sees the input.
  size_t depth = 0;
  bool quoted = false, escaped = false;
  for (char c : encoded)
  {
    if (quoted)
    {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        quoted = false;
      continue;
    }
    if (c == '"')
      quoted = true;
    else if (c == '[' || c == '{')
    {
      if (++depth > 32)
        return fail(error, "runtime archive nesting exceeds budget");
    }
    else if (c == ']' || c == '}')
    {
      if (!depth)
        return fail(error, "runtime archive malformed nesting");
      --depth;
    }
  }
  try
  {
    const auto j = json::parse(encoded);
    if (j.at("schema") != 1 || j.at("kind") != "terrain-rewrite-compiled-world" || !scalarTypes(j))
      return fail(error, "unsupported runtime archive schema or scalar types");
    auto encodedAssets = j.at("assets");
    if (!encodedAssets.contains("coverDefinitions"))
      encodedAssets["coverDefinitions"] = json::array();
    if (!encodedAssets.contains("coverMips"))
      encodedAssets["coverMips"] = json::array();
    RuntimeWorldAssets assets = encodedAssets.get<RuntimeWorldAssets>();
    if (json(assets) != encodedAssets)
      return fail(error, "runtime archive contains lossy numeric conversions or unknown fields");
    if (!safeIndices(assets) || !validateRuntimeAssets(assets, error))
      return fail(error, "invalid runtime archive geometry or dependencies");
    std::vector<uint32_t> levels;
    if (!loadRuntimeSelection(j.at("selection").dump(), assets, levels, error))
      return false;
    RuntimeSnapshot result;
    result.assets = std::make_shared<const RuntimeWorldAssets>(std::move(assets));
    result.tileLevels = std::move(levels);
    // Tickets are process-local; reloaded data must enter a new publication transaction.
    out = std::move(result);
    return true;
  }
  catch (const std::exception &)
  {
    return fail(error, "runtime archive decoding failed");
  }
}
} // namespace procedural::world::rewrite
