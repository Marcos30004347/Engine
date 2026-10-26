#pragma once

#include <cstdint>
#include <vector>

namespace voxel::svraster
{

// Coverage-aware scheduling of terrain chunk objects in a live scene.
//
// A terrain chunk reaches the screen as one runtime object per chunk, built
// from the page cache. The cache is keyed by slot and converges a bounded
// number of slots per frame, while the terrain cut moves as a whole: a
// transaction refines a parent into four children that arrive over several
// frames, coarsens four children into one parent, or simply moves a chunk to
// another slot. Keying the scene objects by slot, and installing whatever a
// slot holds the moment it is rebuilt, turned every one of those into a hole
// or a double draw for the frames between the first and the last slot
// involved - measured as a 12% coverage dip on every refinement.
//
// This scheduler keys objects by the ground they cover (world centre and
// LOD), treats a slot move as a no-op, and only ever swaps coverage
// atomically:
//
//   - a finer entry under a registered coarser object is held back until every
//     live chunk inside the coarser object's footprint is covered by finer
//     entries; then all of them install and the coarser object retires in the
//     same step. Ground inside that footprint with no live chunk at all has
//     left residency and is not waited for;
//   - a coarser entry over registered finer objects installs at once and the
//     finer objects it covers retire in the same step;
//   - an object whose ground left the cut retires only once other objects
//     cover it, or once nothing in the cut overlaps it at all (out of range).
//
// The function is pure so the sequence can be tested without a GPU.
struct TerrainVGGroundFootprint
{
  double centerX = 0.0;
  double centerZ = 0.0;
  double edge = 0.0;
  uint32_t lod = 0u;
};

struct TerrainVGGroundKey
{
  int64_t centerX = 0; // millimetres
  int64_t centerZ = 0;
  uint32_t lod = 0u;

  bool operator==(const TerrainVGGroundKey &other) const
  {
    return centerX == other.centerX && centerZ == other.centerZ && lod == other.lod;
  }
  bool operator!=(const TerrainVGGroundKey &other) const { return !(*this == other); }
};

struct TerrainVGGroundKeyHash
{
  size_t operator()(const TerrainVGGroundKey &key) const noexcept
  {
    uint64_t hash = 1469598103934665603ull;
    for (const uint64_t part :
         {static_cast<uint64_t>(key.centerX), static_cast<uint64_t>(key.centerZ), static_cast<uint64_t>(key.lod)})
      hash = (hash ^ part) * 1099511628211ull;
    return static_cast<size_t>(hash);
  }
};

TerrainVGGroundKey terrainVGGroundKey(const TerrainVGGroundFootprint &footprint);

// A page cache entry that could be installed.
struct TerrainVGScheduleEntry
{
  TerrainVGGroundFootprint footprint{};
  // Payload identity; a registered object with a different version for the
  // same ground is replaced in place.
  uint64_t version = 0u;
  // Rebuild order. When two cache slots hold the same ground, the most
  // recently rebuilt one is the candidate.
  uint64_t recency = 0u;
  // Caller-defined handle, echoed back in the result.
  uint32_t index = 0u;
};

// A runtime object currently registered in the scene.
// `live` is every chunk in the terrain's current cut, whether or not the
// cache has a page for it yet. `pending` is ground the cut has asked for but
// does not hold yet (requested, generating, staged): a coarser object keeps
// standing in for it until the page arrives, so refinement never opens a hole
// where a child is still on its way. Ground in neither list has left
// residency and holds nothing back.
// True when `point` lies inside the footprint's XZ square.
bool terrainVGFootprintContains(const TerrainVGGroundFootprint &footprint, double x, double z);

// True when the two XZ squares overlap with positive area.
bool terrainVGFootprintsOverlap(const TerrainVGGroundFootprint &a, const TerrainVGGroundFootprint &b);

// True when the union of `covers` contains every point of `area`. Exact for
// quadtree-aligned tilings down to `maxDepth` subdivisions of `area`;
// conservative (may answer false) otherwise.
bool terrainVGFootprintCoveredBy(
    const TerrainVGGroundFootprint &area,
    const std::vector<const TerrainVGGroundFootprint *> &covers,
    uint32_t maxDepth = 10u);

} // namespace voxel::svraster
