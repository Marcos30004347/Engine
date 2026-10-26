#pragma once
#include "WorldSurfaceArrangement.hpp"
namespace procedural::world::rewrite
{
// Priority increases ownership precedence. Within a producer, preserve contour order.
// Producer identity breaks equal-priority ties independently of worker completion order.
struct WorldContourBatch
{
  uint64_t producer = 0;
  int32_t priority = 0;
  std::vector<SurfaceContour> contours;
};
struct WorldSourceChain
{
  uint64_t producer = 0, contour = 0, canonicalContour = 0;
  uint32_t segment = 0, layer = 0;
  Position from, to;
  std::vector<uint64_t> edges;
};
struct WorldPatchTile
{
  uint32_t x = 0, z = 0;
  // Indices into the frozen mesh. Patches and boundary stations are never rebuilt per tile.
  std::vector<uint64_t> faces, boundaryEdges;
  std::vector<uint32_t> triangles;
};
struct WorldFinalPatches
{
  SurfaceArrangement map;
  SurfaceArrangementMesh mesh;
  std::vector<WorldSourceChain> sources;
  std::vector<WorldPatchTile> tiles;
};
// Validate source -> arrangement -> actual CDT edge coverage, including coincident sources.
// Also usable before publishing a deserialized artifact. Does not mutate the product.
bool auditWorldFinalPatches(const WorldFinalPatches &, std::string *error = nullptr);
// Bounded global arrangement; convex ownership masks support holes through decomposition.
// Cuts are sorted/deduplicated globally. Output is published only after the audit passes.
bool freezeWorldPatches(
    std::vector<WorldContourBatch>,
    SurfaceMapSettings,
    const SurfaceRefinement &,
    const std::function<double(double, double)> &terrain,
    uint64_t revision,
    WorldFinalPatches &,
    std::string *error = nullptr);
} // namespace procedural::world::rewrite
