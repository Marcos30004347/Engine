// Terrain chunks through the welded-surface pipeline (SVR_TERRAIN_SURFACE_VG),
// against the octree route on the same generated chunks.
//
// For each fixture's chunks: the asset has one hierarchy root (the runtime
// installs a chunk through one - four shapes drew a quarter of each chunk),
// covers the same ground as the octree asset, its finest cut is closed, its
// finest clusters carry the octree route's leaf error (the voxel size, so a
// resident finer chunk replaces them), every cluster has a source with valid
// bounds, and it draws no more triangles than the octree at its finest (the
// quadrant skirts aside).

#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "TerrainGenerationPhase0Utils.hpp"
#include "editor/virtualgeometry/TerrainTriangleVirtualGeometryBridge.hpp"
#include "editor/virtualgeometry/VirtualGeometryDecoder.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"
#include "voxel/svraster/SparseVoxelGenerator.hpp"

using namespace virtualgeometry;
namespace svr = voxel::svraster;

namespace
{

int failures = 0;
void check(bool condition, const std::string &message)
{
  if (!condition)
  {
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    ++failures;
  }
}

struct Cut
{
  uint64_t triangles = 0u;
  uint64_t openEdges = 0u;
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
};

// The finest clusters (self error at most `finest`), decoded.
Cut finestCut(const VirtualGeometryEncodedData &encoded, float finest)
{
  Cut cut;
  std::map<std::pair<std::array<float, 3>, std::array<float, 3>>, int> edges;
  for (uint32_t page = 0; page < encoded.pages.size(); ++page)
    for (uint32_t m = 0; m < encoded.pages[page].meshlets.size(); ++m)
    {
      const auto &meshlet = encoded.pages[page].meshlets[m];
      if (meshlet.self.error > finest)
        continue;
      std::vector<Vertex> vertices;
      std::vector<uint32_t> indices;
      VirtualGeometryDecoder::decodeMeshlet(encoded, page, m, vertices, indices);
      for (size_t t = 0; t + 2 < indices.size(); t += 3)
      {
        std::array<float, 3> p[3];
        for (int k = 0; k < 3; ++k)
        {
          const auto &v = vertices[indices[t + k]];
          p[k] = {v.pos[0], v.pos[1], v.pos[2]};
          for (int a = 0; a < 3; ++a)
          {
            cut.lo[a] = std::min(cut.lo[a], v.pos[a]);
            cut.hi[a] = std::max(cut.hi[a], v.pos[a]);
          }
        }
        if (p[0] == p[1] || p[1] == p[2] || p[0] == p[2])
          continue;
        ++cut.triangles;
        for (int e = 0; e < 3; ++e)
        {
          auto a = p[e], b = p[(e + 1) % 3];
          ++edges[{std::min(a, b), std::max(a, b)}];
        }
      }
    }
  for (const auto &[edge, count] : edges)
    cut.openEdges += count == 1;
  return cut;
}

void fixture(terrain_phase0::FixtureKind kind, const char *name)
{
  std::string error;
  const auto bundle = terrain_phase0::makeFixture(kind);
  const auto settings = terrain_phase0::fixtureSettings(kind, 0u);
  svr::PlanetVoxelChunkSet chunks;
  svr::WorldMapVoxelGenerationMetrics metrics{};
  if (!svr::buildWorldMapVoxelTerrain(bundle, settings, chunks, &error, {}, nullptr, &metrics) || chunks.chunks.empty())
  {
    check(false, std::string(name) + ": terrain generates (" + error + ")");
    return;
  }
  for (uint32_t chunk = 0; chunk < chunks.chunks.size(); ++chunk)
  {
    const float voxel = float(chunks.chunks[chunk].voxelSizeMetres);
    svr::SparseVoxelOctreeVirtualGeometrySettings octree{};
    octree.leafSelfError = voxel; // As the shared terrain object sets it.
    VirtualGeometryBuildSettings build{};
    QuantizationConfig quantization{};
    VirtualGeometryEncodedData octreeAsset, surfaceAsset;
    svr::SparseVoxelTerrainOctreeVirtualGeometryBuildStats octreeStats, surfaceStats;
    std::array<double, 3> octreeOrigin{}, surfaceOrigin{};
    const bool octreeOk = svr::buildSparseVoxelTerrainOctreeVirtualGeometryAsset(chunks, chunk, octree, build, quantization, octreeAsset,
                                                                                 &octreeOrigin, &octreeStats, &error);
    const auto t0 = std::chrono::steady_clock::now();
    const bool surfaceOk = svr::buildSparseVoxelTerrainSurfaceVirtualGeometryAsset(chunks, chunk, octree, build, quantization, surfaceAsset,
                                                                                   &surfaceOrigin, &surfaceStats, &error);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const std::string label = std::string(name) + " chunk " + std::to_string(chunk);
    check(octreeOk, label + ": octree asset builds");
    check(surfaceOk, label + ": surface asset builds (" + error + ")");
    if (!octreeOk || !surfaceOk || octreeAsset.pages.empty())
      continue;
    check(surfaceAsset.shapes.size() == 1u, label + ": one hierarchy root");
    check(surfaceOrigin == octreeOrigin, label + ": same page origin");

    uint64_t clusters = 0u;
    float finest = 1e30f;
    for (const auto &page : surfaceAsset.pages)
      for (const auto &meshlet : page.meshlets)
      {
        ++clusters;
        finest = std::min(finest, meshlet.self.error);
      }
    check(std::fabs(finest - voxel) <= 1e-6f * voxel, label + ": finest clusters carry the voxel size as error");
    check(surfaceStats.build.clusterSources.size() == clusters, label + ": one cluster source per cluster");
    bool sourcesValid = true;
    for (const auto &source : surfaceStats.build.clusterSources)
      sourcesValid = sourcesValid && source.surfaceValid && source.blockExtent > 0.f;
    check(sourcesValid, label + ": every cluster source has bounds");

    const Cut surfaceCut = finestCut(surfaceAsset, voxel * 1.0001f);
    const Cut octreeCut = finestCut(octreeAsset, voxel * 1.0001f);
    std::printf("    %s: surface %llu triangles (%llu open edges) vs octree %llu; %.1f ms\n", label.c_str(),
                (unsigned long long)surfaceCut.triangles, (unsigned long long)surfaceCut.openEdges, (unsigned long long)octreeCut.triangles, ms);
    check(surfaceCut.openEdges == 0u, label + ": the finest cut is closed");
    // The four quadrant meshes' skirts cost a few triangles that a flat chunk
    // cannot win back (flat: 392 against 384); on relief the surface route is
    // far smaller (mountain 2470 against 6864).
    check(surfaceCut.triangles <= octreeCut.triangles + octreeCut.triangles / 10u + 64u,
          label + ": no more triangles than the octree at its finest, skirts aside");
    bool sameGround = true;
    for (int a : {0, 2})
      sameGround = sameGround && std::fabs(surfaceCut.lo[a] - octreeCut.lo[a]) <= 1.01f * voxel &&
                   std::fabs(surfaceCut.hi[a] - octreeCut.hi[a]) <= 1.01f * voxel;
    check(sameGround, label + ": covers the same ground as the octree asset");
  }
}

} // namespace

int main()
{
  fixture(terrain_phase0::FixtureKind::Flat, "flat");
  fixture(terrain_phase0::FixtureKind::Mountain, "mountain");
  fixture(terrain_phase0::FixtureKind::Cliff, "cliff");
  if (failures)
  {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("SparseVoxelTerrainSurfaceVGTests passed\n");
  return 0;
}
