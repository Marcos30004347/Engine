#include "WorldMapPatchAdapter.hpp"
#include <cmath>

namespace procedural::world::rewrite {
bool importWorldPatches(const procedural::world::MapGraph &graph, EditablePatchSet &out, std::string *error)
{
  if (!graph.validate(error))
  {
    return false;
  }
  EditablePatchSet result;
  std::map<std::pair<int64_t, int64_t>, uint64_t> vertices;
  for (auto &node : graph.nodes())
  {
    EditablePatch patch;
    patch.id = uint64_t(node.id) + 1;
    patch.parent = node.parent == procedural::world::kInvalidNode ? 0 : uint64_t(node.parent) + 1;
    patch.owner = node.region == procedural::world::kInvalidNode ? 0 : uint64_t(node.region) + 1;
    patch.biome = node.biome;
    patch.use = node.land ? PatchLandUse::Unassigned : PatchLandUse::Protected;
    for (auto p : node.area)
    {
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || std::abs(p.x) > 1e7 || std::abs(p.y) > 1e7)
      {
        if (error)
        {
          *error = "world patch coordinate exceeds editable range";
        }
        return false;
      }
      auto key = std::make_pair(int64_t(std::llround(p.x * 1e9)), int64_t(std::llround(p.y * 1e9)));
      auto it = vertices.find(key);
      if (it == vertices.end())
      {
        auto id = uint64_t(vertices.size()) + 1;
        it = vertices.emplace(key, id).first;
        result.vertices.emplace(id, Position{double(key.first) / 1e9, 0, double(key.second) / 1e9});
      }
      if (patch.vertices.empty() || patch.vertices.back() != it->second)
      {
        patch.vertices.push_back(it->second);
      }
    }
    if (patch.vertices.size() > 1 && patch.vertices.front() == patch.vertices.back())
    {
      patch.vertices.pop_back();
    }
    result.patches.emplace(patch.id, std::move(patch));
  }
  if (!validateEditablePatches(result, error))
  {
    return false;
  }
  out = std::move(result);
  return true;
}
} // namespace procedural::world::rewrite
