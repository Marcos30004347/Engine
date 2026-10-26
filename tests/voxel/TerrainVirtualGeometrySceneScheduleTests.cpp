// Terrain ground footprint helpers: quadtree containment, overlap and
// coverage. The coverage-aware CPU scheduler these once drove is gone -
// residency is the streamer's decision and LOD is the culler's.
#include "editor/virtualgeometry/TerrainVirtualGeometrySceneSchedule.hpp"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

using namespace voxel::svraster;

namespace
{

int failures = 0;

#define CHECK(cond)                                                                          \
  do                                                                                         \
  {                                                                                          \
    if (!(cond))                                                                             \
    {                                                                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
      ++failures;                                                                            \
    }                                                                                        \
  } while (false)

TerrainVGGroundFootprint fp(const double x, const double z, const double edge, const uint32_t lod)
{
  TerrainVGGroundFootprint footprint{};
  footprint.centerX = x;
  footprint.centerZ = z;
  footprint.edge = edge;
  footprint.lod = lod;
  return footprint;
}

// Four quadtree children of a footprint.
std::vector<TerrainVGGroundFootprint> childrenOf(const TerrainVGGroundFootprint &parent)
{
  const double q = parent.edge * 0.25;
  return {
      fp(parent.centerX - q, parent.centerZ - q, parent.edge * 0.5, parent.lod - 1u),
      fp(parent.centerX + q, parent.centerZ - q, parent.edge * 0.5, parent.lod - 1u),
      fp(parent.centerX - q, parent.centerZ + q, parent.edge * 0.5, parent.lod - 1u),
      fp(parent.centerX + q, parent.centerZ + q, parent.edge * 0.5, parent.lod - 1u),
  };
}

void testGeometryHelpers()
{
  const auto parent = fp(0.0, 0.0, 64.0, 2u);
  const auto children = childrenOf(parent);
  std::vector<const TerrainVGGroundFootprint *> covers;
  for (const auto &child : children)
    covers.push_back(&child);
  CHECK(terrainVGFootprintCoveredBy(parent, covers));
  covers.pop_back();
  CHECK(!terrainVGFootprintCoveredBy(parent, covers));
  // Mixed levels: three children plus the four grandchildren of the fourth.
  const auto grandchildren = childrenOf(children[3]);
  for (const auto &grandchild : grandchildren)
    covers.push_back(&grandchild);
  CHECK(terrainVGFootprintCoveredBy(parent, covers));
  covers.pop_back();
  CHECK(!terrainVGFootprintCoveredBy(parent, covers));
  // A coarser cover contains outright.
  const auto grand = fp(0.0, 0.0, 128.0, 3u);
  const TerrainVGGroundFootprint *one[] = {&grand};
  CHECK(terrainVGFootprintCoveredBy(parent, {one[0]}));
  CHECK(terrainVGFootprintsOverlap(parent, children[0]));
  CHECK(!terrainVGFootprintsOverlap(children[0], children[3]));
  CHECK(!terrainVGFootprintsOverlap(children[0], children[1]));
  CHECK(terrainVGFootprintContains(parent, 31.99, -31.99));
  CHECK(!terrainVGFootprintContains(parent, 32.5, 0.0));
}

} // namespace

int main()
{
  testGeometryHelpers();
  if (failures != 0)
  {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("TerrainVirtualGeometrySceneSchedule: all checks passed\n");
  return EXIT_SUCCESS;
}
