// A voxel object instance as one welded, seamless triangle mesh, and the
// virtual geometry DAG the stock builder makes from it.
//
// The mesh contract is checked on shapes whose answer is known by hand (a
// cube, a ring, cells touching only along an edge or at a corner, a hollow
// box) and on random cell sets, then on one real dwelling. The DAG contract
// is checked on the dwelling: every cut the culler can choose is closed (a
// border edge is a crack), stays near the voxel surface, and the chain
// reaches a small root.
//
// VOXEL_SURFACE_TEST_OUT=<dir> also writes the dwelling's LOD0 glTF and a
// preview sheet of its cuts; VOXEL_SURFACE_CRACK_DEBUG=1 names the clusters
// owning any open edge.

#include "editor/virtualgeometry/CityVoxelDwellingCompiler.hpp"
#include "editor/virtualgeometry/VirtualGeometryBuilder.hpp"
#include "editor/virtualgeometry/VirtualGeometryDecoder.hpp"
#include "editor/virtualgeometry/VirtualGeometryHierarchyBuilder.hpp"
#include "editor/virtualgeometry/VoxelSurfaceAtlas.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"
#include "editor/virtualgeometry/VoxelSurfaceTriangleMesh.hpp"
#include "procedural/geom2/Polygon.hpp"
#include "procedural/world/hierarchy/CellDwellingGenerator.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <string>
#include <unordered_map>

using namespace virtualgeometry;

namespace
{

int failures = 0;
void check(bool condition, const std::string &message)
{
  if (!condition)
  {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

using Cells = std::vector<VoxelSurfaceCell>;

Cells box(int x0, int y0, int z0, int x1, int y1, int z1, uint32_t rgba = 0xff808080u)
{
  Cells cells;
  for (int x = x0; x < x1; ++x)
    for (int y = y0; y < y1; ++y)
      for (int z = z0; z < z1; ++z)
        cells.push_back({{x, y, z}, rgba});
  return cells;
}

Cells join(Cells a, const Cells &b)
{
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

Cells without(const Cells &a, const Cells &b)
{
  std::set<std::array<int32_t, 3>> drop;
  for (const auto &cell : b)
    drop.insert(cell.position);
  Cells out;
  for (const auto &cell : a)
    if (!drop.count(cell.position))
      out.push_back(cell);
  return out;
}

uint64_t exposedFaces(const Cells &cells)
{
  std::set<std::array<int32_t, 3>> solid;
  for (const auto &cell : cells)
    solid.insert(cell.position);
  uint64_t faces = 0;
  static constexpr int d[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  for (const auto &p : solid)
    for (const auto &s : d)
      faces += !solid.count({p[0] + s[0], p[1] + s[1], p[2] + s[2]});
  return faces;
}

bool distinctPositions(const VoxelSurfaceTriangleMesh &mesh)
{
  std::set<std::array<float, 3>> seen;
  for (const auto &v : mesh.vertices)
    if (!seen.insert({v.pos[0], v.pos[1], v.pos[2]}).second)
      return false;
  return true;
}

// The facts every mesh of `cells` must satisfy; `euler` < 0 skips that one.
VoxelSurfaceTriangleMesh expectMesh(const std::string &name, const Cells &cells, int64_t euler, int64_t vertices = -1,
                                    int64_t split = -1)
{
  VoxelSurfaceTriangleMeshSettings settings;
  settings.pitchMetres = 0.1;
  VoxelSurfaceTriangleMesh mesh;
  std::string error;
  check(buildVoxelSurfaceTriangleMesh(cells, settings, mesh, &error), name + ": meshes (" + error + ")");
  const VoxelSurfaceTopology t = measureVoxelSurfaceTopology(mesh);
  std::set<std::array<int32_t, 3>> unique;
  for (const auto &cell : cells)
    unique.insert(cell.position);
  const double pitch = settings.pitchMetres;
  check(mesh.faceCount == exposedFaces(cells), name + ": one quad per exposed face");
  check(mesh.splitEdges > 0u || mesh.indices.size() == mesh.faceCount * 6u, name + ": two triangles per face");
  check(t.closedManifold(), name + ": closed, consistently wound 2-manifold (" + std::to_string(t.borderEdges) + " border, " +
                                std::to_string(t.nonManifoldEdges) + " non-manifold, " + std::to_string(t.misorientedEdges) + " misoriented)");
  check(t.degenerateTriangles == 0u && t.zeroAreaTriangles == 0u, name + ": no degenerate triangles");
  check(distinctPositions(mesh), name + ": no two vertices share a position (no seams)");
  check(t.vertices == mesh.vertices.size(), name + ": every vertex is used");
  // Split vertices move a thousandth of a voxel; allow for that and no more.
  const double moved = 1e-2 * double(mesh.splitVertices + 2u * mesh.splitEdges);
  check(std::fabs(t.area - double(mesh.faceCount) * pitch * pitch) <= (moved + 1e-5 * double(mesh.faceCount)) * pitch * pitch,
        name + ": area is the exposed faces");
  const double volume = double(unique.size()) * pitch * pitch * pitch;
  check(std::fabs(t.signedVolume - volume) <= (moved + 1e-5 * double(unique.size())) * pitch * pitch * pitch,
        name + ": outward winding encloses the cells' volume");
  if (euler >= 0)
    check(t.eulerCharacteristic() == euler, name + ": Euler characteristic " + std::to_string(t.eulerCharacteristic()) +
                                                " (expected " + std::to_string(euler) + ")");
  if (vertices >= 0)
    check(int64_t(mesh.vertices.size()) == vertices, name + ": " + std::to_string(mesh.vertices.size()) + " vertices (expected " +
                                                         std::to_string(vertices) + ")");
  if (split >= 0)
    check(int64_t(mesh.splitVertices) == split, name + ": " + std::to_string(mesh.splitVertices) + " split vertices (expected " +
                                                    std::to_string(split) + ")");
  for (const auto &v : mesh.vertices)
    check(std::fabs(v.norm[0] * v.norm[0] + v.norm[1] * v.norm[1] + v.norm[2] * v.norm[2] - 1.f) < 1e-4f, name + ": unit vertex normals");

  // The lossless merge: the same closed surface, area and volume, fewer triangles.
  VoxelSurfaceTriangleMesh merged = mesh;
  mergeVoxelSurfaceCoplanar(merged, settings);
  const VoxelSurfaceTopology m = measureVoxelSurfaceTopology(merged);
  check(m.closedManifold() && m.degenerateTriangles == 0u, name + ": merged LOD0 is still closed and manifold");
  check(m.eulerCharacteristic() == t.eulerCharacteristic(), name + ": merged LOD0 keeps its topology");
  // Collapses next to a split vertex inherit its thousandth-of-a-voxel offset.
  const double areaSlack = (moved + 1e-5 * double(mesh.faceCount)) * pitch * pitch;
  const double volumeSlack = (moved + 1e-5 * double(unique.size())) * pitch * pitch * pitch;
  check(std::fabs(m.area - t.area) <= areaSlack && std::fabs(m.signedVolume - t.signedVolume) <= volumeSlack,
        name + ": merged LOD0 is the same surface (area " + std::to_string(m.area - t.area) + ", volume " +
            std::to_string(m.signedVolume - t.signedVolume) + ")");
  check(merged.indices.size() <= mesh.indices.size(), name + ": merging never adds triangles");
  return mesh;
}

void knownShapes()
{
  expectMesh("cube", box(0, 0, 0, 1, 1, 1), 2, 8, 0);
  {
    VoxelSurfaceTriangleMesh slab = expectMesh("slab", box(0, 0, 0, 5, 1, 3), 2);
    mergeVoxelSurfaceCoplanar(slab, {});
    check(slab.indices.size() == 36u && slab.vertices.size() == 8u, "slab: merges to a 12-triangle box (" +
                                                                        std::to_string(slab.indices.size() / 3u) + " triangles)");
  }
  // A ring: genus one.
  expectMesh("ring", without(box(0, 0, 0, 3, 1, 3), box(1, 0, 1, 2, 1, 2)), 0);
  // Two cells on one edge: two closed cubes, the shared edge's two points
  // split, nothing welded across.
  expectMesh("edge contact", join(box(0, 0, 0, 1, 1, 1), box(1, 1, 0, 2, 2, 1)), 4, 16, 2);
  // At one corner only: one point split.
  expectMesh("corner contact", join(box(0, 0, 0, 1, 1, 1), box(1, 1, 1, 2, 2, 2)), 4, 16, 1);
  // A diagonal staircase of edge contacts, the case stepped roofs produce.
  {
    Cells stair;
    for (int i = 0; i < 6; ++i)
      stair = join(stair, box(i, i, 0, i + 1, i + 1, 4));
    expectMesh("edge-contact staircase", stair, 12);
  }
  // Two diagonal cells joined round both ends of the edge they share - above
  // and below - so the surface passes that edge twice: a ring through a
  // pinch, which only an edge split keeps manifold.
  {
    Cells pinched = join(box(0, 0, 0, 1, 1, 1), box(1, 1, 0, 2, 2, 1));
    for (int z : {-1, 1})
      pinched = join(pinched, join(join(box(0, 0, z, 1, 1, z + 1), box(0, 1, z, 1, 2, z + 1)), box(1, 1, z, 2, 2, z + 1)));
    const auto mesh = expectMesh("pinched edge", pinched, 0);
    check(mesh.splitEdges == 1u, "pinched edge: the shared edge is split once (" + std::to_string(mesh.splitEdges) + ")");
  }
  // A hollow box: an outer surface and an inward-facing inner one.
  expectMesh("hollow box", without(box(0, 0, 0, 5, 5, 5), box(1, 1, 1, 4, 4, 4)), 4);
  // An L, a concave corner and a step: one sphere.
  expectMesh("L step", join(join(box(0, 0, 0, 4, 1, 4), box(0, 1, 0, 1, 3, 4)), box(1, 1, 0, 2, 2, 1)), 2);

  // Colour never splits a vertex: the mesh is one per instance, not one per
  // material, and colour is baked later.
  {
    Cells plain = box(0, 0, 0, 4, 3, 2), painted = plain;
    for (size_t i = 0; i < painted.size(); ++i)
      painted[i].rgba = 0xff000000u | uint32_t(i * 2654435761u);
    const auto a = expectMesh("plain block", plain, 2), b = expectMesh("painted block", painted, 2);
    check(a.vertices.size() == b.vertices.size() && a.indices == b.indices, "colour does not change the mesh");
  }
}

void randomCellSets()
{
  std::mt19937 random(20261016u);
  for (int trial = 0; trial < 40; ++trial)
  {
    const int n = 3 + int(random() % 6u);
    const float fill = 0.25f + 0.5f * float(random() % 100u) / 100.f;
    Cells cells;
    for (int x = 0; x < n; ++x)
      for (int y = 0; y < n; ++y)
        for (int z = 0; z < n; ++z)
          if (float(random() % 1000u) / 1000.f < fill)
            cells.push_back({{x - 2, y + 7, z - 5}, 0xff000000u | random()});
    if (cells.empty())
      continue;
    const std::string name = "random " + std::to_string(trial);
    const auto mesh = expectMesh(name, cells, -1);

    // Euler characteristic of a closed orientable surface is even.
    check(measureVoxelSurfaceTopology(mesh).eulerCharacteristic() % 2 == 0, name + ": even Euler characteristic");

    // Input order is irrelevant: the cells are sorted before anything is numbered.
    Cells shuffled = cells;
    std::shuffle(shuffled.begin(), shuffled.end(), random);
    VoxelSurfaceTriangleMesh again;
    buildVoxelSurfaceTriangleMesh(shuffled, {}, again, nullptr);
    bool same = again.indices == mesh.indices && again.vertices.size() == mesh.vertices.size();
    for (size_t v = 0; same && v < mesh.vertices.size(); ++v)
      same = std::equal(mesh.vertices[v].pos, mesh.vertices[v].pos + 3, again.vertices[v].pos);
    check(same, name + ": independent of input order");
  }
}

// --- One real dwelling -------------------------------------------------------

procedural::geom2::Polygon rectangleLot(double width, double depth)
{
  using namespace procedural::geom2;
  Polygon lot;
  for (const auto &[x, z] : std::vector<std::pair<double, double>>{{0, 0}, {width, 0}, {width, depth}, {0, depth}})
    lot.outer.push_back(Point{toUnit(x), toUnit(z)});
  if (!counterClockwise(lot.outer))
    reverse(lot.outer);
  return lot;
}

bool dwellingCells(Cells &cells, double &pitchMetres)
{
  namespace hier = procedural::world::hierarchy;
  hier::CellDwellings generator;
  hier::RegionTree tree;
  hier::Context context;
  context.worldSeed = 20260918;
  const hier::Region region = hier::makeRegion(hier::RegionId(3), hier::kNoRegion, rectangleLot(18, 14));
  std::vector<hier::Object> objects;
  procedural::world::generator::Ledger ledger;
  std::string error;
  if (!generator.populate(hier::Request{region, tree, context}, objects, ledger, &error) || objects.empty() ||
      !objects.front().voxelAssembly || !objects.front().skeleton)
  {
    std::cerr << "dwelling: not generated " << error << '\n';
    return false;
  }
  std::vector<voxel::svraster::CityVoxelCell> resolved;
  uint32_t pitch = 0;
  CityVoxelDwellingCompileReport report;
  if (!resolveCityVoxelDwellingCells(*objects.front().voxelAssembly, *objects.front().skeleton, resolved, pitch, report, &error))
  {
    std::cerr << "dwelling: cells not resolved " << error << '\n';
    return false;
  }
  pitchMetres = pitch * 1e-6;
  cells.clear();
  for (const auto &cell : resolved)
    cells.push_back({cell.position, cell.rgba});
  return !cells.empty();
}

// Distance from p to the nearest LOD0 triangle, searched within `reach`;
// reach + 1 when nothing is that near.
struct SurfaceIndex
{
  float bucket = 0.25f;
  std::vector<std::array<float, 3>> corners;
  std::unordered_map<uint64_t, std::vector<uint32_t>> buckets;

  static uint64_t keyOf(int x, int y, int z)
  {
    return (uint64_t(uint32_t(x + (1 << 20))) << 42) | (uint64_t(uint32_t(y + (1 << 20))) << 21) | uint64_t(uint32_t(z + (1 << 20)));
  }
  explicit SurfaceIndex(const std::vector<std::array<float, 3>> &triangleCorners) : corners(triangleCorners)
  {
    for (uint32_t t = 0; t * 3u < corners.size(); ++t)
    {
      int lo[3], hi[3];
      for (int a = 0; a < 3; ++a)
      {
        const float mn = std::min({corners[t * 3][a], corners[t * 3 + 1][a], corners[t * 3 + 2][a]});
        const float mx = std::max({corners[t * 3][a], corners[t * 3 + 1][a], corners[t * 3 + 2][a]});
        lo[a] = int(std::floor(mn / bucket));
        hi[a] = int(std::floor(mx / bucket));
      }
      for (int x = lo[0]; x <= hi[0]; ++x)
        for (int y = lo[1]; y <= hi[1]; ++y)
          for (int z = lo[2]; z <= hi[2]; ++z)
            buckets[keyOf(x, y, z)].push_back(t);
    }
  }

  static float pointTriangle(const std::array<float, 3> &p, const std::array<float, 3> &a, const std::array<float, 3> &b,
                             const std::array<float, 3> &c)
  {
    // Ericson, Real-Time Collision Detection 5.1.5.
    auto sub = [](auto u, auto v) { return std::array<float, 3>{u[0] - v[0], u[1] - v[1], u[2] - v[2]}; };
    auto dot = [](auto u, auto v) { return u[0] * v[0] + u[1] * v[1] + u[2] * v[2]; };
    auto at = [&](float s, auto u, float t, auto v) { return std::array<float, 3>{a[0] + s * u[0] + t * v[0], a[1] + s * u[1] + t * v[1], a[2] + s * u[2] + t * v[2]}; };
    const auto ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
    std::array<float, 3> q;
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    const auto bp = sub(p, b), cp = sub(p, c);
    const float d3 = dot(ab, bp), d4 = dot(ac, bp), d5 = dot(ab, cp), d6 = dot(ac, cp);
    const float vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
    if (d1 <= 0 && d2 <= 0) q = a;
    else if (d3 >= 0 && d4 <= d3) q = b;
    else if (vc <= 0 && d1 >= 0 && d3 <= 0) q = at(d1 / (d1 - d3), ab, 0, ac);
    else if (d6 >= 0 && d5 <= d6) q = c;
    else if (vb <= 0 && d2 >= 0 && d6 <= 0) q = at(0, ab, d2 / (d2 - d6), ac);
    else if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
    {
      const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
      q = {b[0] + w * (c[0] - b[0]), b[1] + w * (c[1] - b[1]), b[2] + w * (c[2] - b[2])};
    }
    else
    {
      const float denom = 1.f / (va + vb + vc);
      q = at(vb * denom, ab, vc * denom, ac);
    }
    const auto d = sub(p, q);
    return std::sqrt(dot(d, d));
  }

  float distance(const std::array<float, 3> &p, float reach) const
  {
    float best = reach + 1.f;
    const int r = int(std::ceil(reach / bucket));
    const int cx = int(std::floor(p[0] / bucket)), cy = int(std::floor(p[1] / bucket)), cz = int(std::floor(p[2] / bucket));
    for (int x = cx - r; x <= cx + r; ++x)
      for (int y = cy - r; y <= cy + r; ++y)
        for (int z = cz - r; z <= cz + r; ++z)
        {
          const auto it = buckets.find(keyOf(x, y, z));
          if (it == buckets.end())
            continue;
          for (uint32_t t : it->second)
            best = std::min(best, pointTriangle(p, corners[t * 3], corners[t * 3 + 1], corners[t * 3 + 2]));
        }
    return best;
  }
};

double meanDifference(const VoxelSurfacePreviewImage &a, const VoxelSurfacePreviewImage &b)
{
  double sum = 0.0;
  uint64_t pixels = 0u;
  for (size_t p = 0; p < a.rgba.size(); ++p)
  {
    if (a.rgba[p] == 0xff302820u && b.rgba[p] == 0xff302820u)
      continue;
    ++pixels;
    for (int k = 0; k < 3; ++k)
      sum += std::abs(int((a.rgba[p] >> (8 * k)) & 255u) - int((b.rgba[p] >> (8 * k)) & 255u));
  }
  return pixels ? sum / (3.0 * double(pixels)) : 0.0;
}

// Bakes the dwelling's DAG and checks the atlas: every cluster in budget,
// charts disjoint, no two triangles of one cluster on one texel, the
// textured cuts matching the voxels, and the written file reading back to
// the same cuts.
void dwellingAtlas(VirtualGeometryBuildData build, const Cells &cells, const VoxelSurfaceTriangleMeshSettings &settings,
                   const VoxelSurfacePreviewView &view, const std::vector<float> &cuts, const char *outDir)
{
  using Clock = std::chrono::steady_clock;
  VoxelSurfaceAtlas atlas;
  std::string error;
  const auto t0 = Clock::now();
  check(bakeVoxelSurfaceAtlas(build, cells, settings, {}, atlas, &error), "atlas: bakes (" + error + ")");
  const double bakeMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  std::printf("    atlas: %ux%u, %zu charts (%u shared view regions, %u peeled layers), triangles %llu shared / %llu own, %u vertex copies, "
              "max %u cluster vertices, %u folded clusters, %llu texels covered, %llu missed (%.2f%%), %.0f ms\n",
              atlas.width, atlas.height, atlas.charts.size(), atlas.sharedRegions, atlas.peeledCharts, (unsigned long long)atlas.sharedTriangles,
              (unsigned long long)atlas.ownTriangles, atlas.addedVertices, atlas.maxClusterVertices,
              atlas.mergedClusters, (unsigned long long)atlas.coveredTexels, (unsigned long long)atlas.missedTexels,
              atlas.coveredTexels ? 100.0 * double(atlas.missedTexels) / double(atlas.coveredTexels) : 0.0, bakeMs);
  check(atlas.maxClusterVertices <= MaxVertices, "atlas: every cluster within MaxVertices after vertex copies");
  check(atlas.missedTexels * 50u <= atlas.coveredTexels, "atlas: under 2% of texels find no voxel");
  check(atlas.sharedTriangles * 10u >= (atlas.sharedTriangles + atlas.ownTriangles) * 9u,
        "atlas: nine in ten triangles draw from the shared view regions");

  // Charts are disjoint rectangles inside the atlas.
  {
    std::vector<uint8_t> owner(size_t(atlas.width) * atlas.height, 0u);
    uint64_t overlap = 0u, outside = 0u;
    for (const auto &chart : atlas.charts)
    {
      if (chart.x + chart.width > atlas.width || chart.y + chart.height > atlas.height)
      {
        ++outside;
        continue;
      }
      for (uint32_t y = chart.y; y < chart.y + chart.height; ++y)
        for (uint32_t x = chart.x; x < chart.x + chart.width; ++x)
          overlap += owner[size_t(y) * atlas.width + x]++ != 0u;
    }
    check(outside == 0u && overlap == 0u, "atlas: charts are disjoint and inside the atlas");
    uint64_t rectArea = 0u, innerArea = 0u;
    for (const auto &chart : atlas.charts)
    {
      rectArea += uint64_t(chart.width) * chart.height;
      innerArea += uint64_t(chart.width - 2u) * (chart.height - 2u);
    }
    std::printf("    atlas: chart rectangles %.1f%% of the atlas, inside their gutters %.1f%%, triangles cover %.1f%%\n",
                100.0 * double(rectArea) / (double(atlas.width) * atlas.height), 100.0 * double(innerArea) / (double(atlas.width) * atlas.height),
                100.0 * double(atlas.coveredTexels) / (double(atlas.width) * atlas.height));
  }

  // Within a cluster no texel centre lies in two triangles: the peeling. A
  // folded cluster is allowed overlaps; it is counted above.
  {
    uint64_t shared = 0u, texels = 0u;
    for (const auto &cluster : build.clusters)
    {
      std::unordered_map<uint64_t, uint32_t> seen;
      for (size_t t = 0; t + 2 < cluster.indices.size(); t += 3)
      {
        float uv[3][2];
        for (int k = 0; k < 3; ++k)
        {
          uv[k][0] = cluster.vertices[cluster.indices[t + k]].uv[0] * float(atlas.width);
          uv[k][1] = cluster.vertices[cluster.indices[t + k]].uv[1] * float(atlas.height);
        }
        const float area = (uv[1][0] - uv[0][0]) * (uv[2][1] - uv[0][1]) - (uv[2][0] - uv[0][0]) * (uv[1][1] - uv[0][1]);
        if (std::fabs(area) < 1e-9f)
          continue;
        const int x0 = int(std::floor(std::min({uv[0][0], uv[1][0], uv[2][0]}))), x1 = int(std::ceil(std::max({uv[0][0], uv[1][0], uv[2][0]})));
        const int y0 = int(std::floor(std::min({uv[0][1], uv[1][1], uv[2][1]}))), y1 = int(std::ceil(std::max({uv[0][1], uv[1][1], uv[2][1]})));
        for (int y = y0; y <= y1; ++y)
          for (int x = x0; x <= x1; ++x)
          {
            const float px = x + 0.5f, py = y + 0.5f;
            const float w0 = ((uv[1][0] - px) * (uv[2][1] - py) - (uv[2][0] - px) * (uv[1][1] - py)) / area;
            const float w1 = ((uv[2][0] - px) * (uv[0][1] - py) - (uv[0][0] - px) * (uv[2][1] - py)) / area;
            if (w0 <= 1e-4f || w1 <= 1e-4f || 1.f - w0 - w1 <= 1e-4f)
              continue;
            ++texels;
            shared += seen[(uint64_t(uint32_t(x)) << 32) | uint32_t(y)]++ != 0u;
          }
      }
    }
    std::printf("    atlas: %llu of %llu covered texels shared by two triangles of one cluster\n", (unsigned long long)shared,
                (unsigned long long)texels);
    check(shared * 1000u <= texels, "atlas: triangles of a cluster do not share texels (peeled layers)");
  }

  // Textured cuts against the voxel truth, and the written file read back.
  const std::string base = (outDir ? std::filesystem::path(outDir) : std::filesystem::temp_directory_path()) / "dwelling-baked";
  check(writeVoxelSurfaceAsset(build, atlas, base, &error), "atlas: writes .png, .vmat and .vg (" + error + ")");
  VirtualGeometryEncodedData loaded;
  {
    VirtualGeometryFile file(base + ".vg", false);
    check(file.isOpen() && file.readAll(loaded), "atlas: the .vg reads back");
  }
  check(loaded.materialFiles.size() == 1u, "atlas: the .vg names its material");
  const double pitch = settings.pitchMetres;
  std::vector<VoxelSurfacePreviewImage> panels;
  for (const float cut : cuts)
  {
    std::vector<std::array<float, 2>> uvs;
    const auto corners = voxelSurfaceCutTriangles(build, cut, &uvs);
    const auto textured = renderVoxelSurfaceTexturedPreview(corners, uvs, atlas.rgba, atlas.width, atlas.height, view);
    const auto truth = renderVoxelSurfacePreview(corners, cells, settings, view, std::max(2.f * float(pitch), cut > 1e29f ? 2.f : 2.f * cut));

    std::vector<std::array<float, 3>> fileCorners;
    std::vector<std::array<float, 2>> fileUVs;
    for (uint32_t page = 0; page < loaded.pages.size(); ++page)
      for (uint32_t m = 0; m < loaded.pages[page].meshlets.size(); ++m)
      {
        const auto &meshlet = loaded.pages[page].meshlets[m];
        if (!(meshlet.self.error <= cut && cut < meshlet.parent.error))
          continue;
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        VirtualGeometryDecoder::decodeMeshlet(loaded, page, m, vertices, indices);
        for (uint32_t index : indices)
        {
          fileCorners.push_back({vertices[index].pos[0], vertices[index].pos[1], vertices[index].pos[2]});
          fileUVs.push_back({vertices[index].uv[0], vertices[index].uv[1]});
        }
      }
    const auto fromFile = renderVoxelSurfaceTexturedPreview(fileCorners, fileUVs, atlas.rgba, atlas.width, atlas.height, view);
    const double vsTruth = meanDifference(textured, truth), vsFile = meanDifference(textured, fromFile);
    char label[32];
    std::snprintf(label, sizeof(label), cut > 1e29f ? "root" : "%.3f m", cut);
    std::printf("    textured cut %-8s mean |rgb| vs voxels %.2f, file vs build %.2f (%zu vs %zu triangles)\n", label, vsTruth, vsFile,
                fileCorners.size() / 3u, corners.size() / 3u);
    check(fileCorners.size() == corners.size(), std::string("atlas cut ") + label + ": the file draws the same triangles");
    check(vsFile < 1.0, std::string("atlas cut ") + label + ": the file renders as the build does");
    if (cut == 0.f)
      check(vsTruth < 0.5, "atlas: LOD0 textured is the voxels");
    panels.push_back(truth);
    panels.push_back(textured);
  }
  if (outDir)
  {
    check(writeVoxelSurfacePreviewSheet(panels, std::string(outDir) + "/dwelling-textured.png", &error), "atlas: writes preview");
    std::printf("    wrote %s.{png,vmat,vg} and %s/dwelling-textured.png\n", base.c_str(), outDir);
  }
  else
  {
    for (const char *extension : {".png", ".vmat", ".vg"})
      std::filesystem::remove(base + extension);
  }
}

// --- Terrain chunks ----------------------------------------------------------

// A stepped heightfield: rolling hills, a cliff, a flat plateau, by column.
int32_t terrainHeight(int32_t x, int32_t z)
{
  double h = 24.0 + 7.0 * std::sin(x / 13.0) + 5.0 * std::cos(z / 9.0) + 2.5 * std::sin((x + 2 * z) / 5.0);
  if (x > 70 && z < 50)
    h = 30.0; // Plateau, as city ground is.
  if (x + z > 150)
    h += 14.0; // Cliff.
  return int32_t(std::floor(h));
}

uint32_t terrainColour(int32_t x, int32_t y, int32_t z, int32_t top)
{
  const uint32_t grain = uint32_t((x * 73856093) ^ (y * 19349663) ^ (z * 83492791)) % 24u;
  if (y == top)
    return 0xff000000u | ((40u + grain) << 16) | ((120u + grain) << 8) | (60u + grain); // Grass.
  if (top - y < 4)
    return 0xff000000u | ((40u + grain) << 16) | ((80u + grain) << 8) | (110u + grain); // Earth.
  return 0xff000000u | ((110u + grain) << 16) | ((110u + grain) << 8) | (115u + grain); // Rock.
}

constexpr int32_t kChunk = 64, kFloor = 0;

// One chunk's cells: its columns plus a one-column halo, and a solid layer
// under the floor so the underside is never a face.
Cells terrainChunkCells(int32_t cx, int32_t cz)
{
  Cells cells;
  for (int32_t x = cx * kChunk - 1; x <= cx * kChunk + kChunk; ++x)
    for (int32_t z = cz * kChunk - 1; z <= cz * kChunk + kChunk; ++z)
    {
      const int32_t top = terrainHeight(x, z);
      for (int32_t y = kFloor - 1; y <= top; ++y)
        cells.push_back({{x, y, z}, terrainColour(x, y, z, top)});
    }
  return cells;
}

VoxelSurfaceTriangleMeshSettings terrainChunkSettings(int32_t cx, int32_t cz)
{
  VoxelSurfaceTriangleMeshSettings settings;
  settings.hasDomain = true;
  settings.domainMin = {cx * kChunk, kFloor, cz * kChunk};
  settings.domainMax = {cx * kChunk + kChunk, 1 << 16, cz * kChunk + kChunk};
  return settings;
}

// Open edges not on any of `boxes`' outer faces (lattice units, pitch 0.1).
uint64_t interiorOpenEdges(const std::vector<std::array<float, 3>> &corners, const std::array<int32_t, 3> &lo, const std::array<int32_t, 3> &hi)
{
  std::map<std::pair<std::array<float, 3>, std::array<float, 3>>, int> count;
  for (size_t i = 0; i + 2 < corners.size(); i += 3)
    for (int e = 0; e < 3; ++e)
    {
      auto a = corners[i + e], b = corners[i + (e + 1) % 3];
      if (a == b)
        continue;
      ++count[{std::min(a, b), std::max(a, b)}];
    }
  const auto onBoundary = [&](const std::array<float, 3> &p)
  {
    for (int a : {0, 2})
      if (std::fabs(p[a] / 0.1f - float(lo[a])) < 1e-2f || std::fabs(p[a] / 0.1f - float(hi[a])) < 1e-2f)
        return true;
    return std::fabs(p[1] / 0.1f - float(lo[1])) < 1e-2f;
  };
  uint64_t interior = 0u;
  for (const auto &[edge, n] : count)
    if (n == 1 && !(onBoundary(edge.first) && onBoundary(edge.second)))
      ++interior;
  return interior;
}

void terrainChunks(const char *outDir)
{
  using Clock = std::chrono::steady_clock;
  const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
  struct Chunk
  {
    Cells cells;
    VoxelSurfaceTriangleMeshSettings settings;
    VoxelSurfaceTriangleMesh mesh;
    VirtualGeometryBuildData build;
    VoxelSurfaceAtlas atlas;
  };
  std::vector<Chunk> chunks(4);
  double tMesh = 0, tMerge = 0, tBuild = 0, tAtlas = 0;
  uint64_t rawTriangles = 0, lod0Triangles = 0;
  for (int i = 0; i < 4; ++i)
  {
    Chunk &chunk = chunks[i];
    const int32_t cx = i % 2, cz = i / 2;
    chunk.cells = terrainChunkCells(cx, cz);
    chunk.settings = terrainChunkSettings(cx, cz);
    std::string error;
    const auto t0 = Clock::now();
    check(buildVoxelSurfaceTriangleMesh(chunk.cells, chunk.settings, chunk.mesh, &error), "terrain: chunk meshes (" + error + ")");
    const auto t1 = Clock::now();
    const VoxelSurfaceTopology raw = measureVoxelSurfaceTopology(chunk.mesh);
    rawTriangles += chunk.mesh.indices.size() / 3u;
    check(raw.nonManifoldEdges == 0u && raw.misorientedEdges == 0u, "terrain: chunk LOD0 is manifold and consistently wound");
    std::vector<std::array<float, 3>> corners;
    for (uint32_t index : chunk.mesh.indices)
      corners.push_back({chunk.mesh.vertices[index].pos[0], chunk.mesh.vertices[index].pos[1], chunk.mesh.vertices[index].pos[2]});
    const std::array<int32_t, 3> lo{cx * kChunk, kFloor, cz * kChunk}, hi{cx * kChunk + kChunk, 0, cz * kChunk + kChunk};
    check(interiorOpenEdges(corners, lo, hi) == 0u, "terrain: a chunk's only open edges are on its boundary");
    mergeVoxelSurfaceCoplanar(chunk.mesh, chunk.settings);
    const auto t2 = Clock::now();
    lod0Triangles += chunk.mesh.indices.size() / 3u;
    std::vector<Vertex> vertices = chunk.mesh.vertices;
    std::vector<Shape> shapes(1);
    shapes[0].indices = chunk.mesh.indices;
    chunk.build = VirtualGeometryBuilder::build(vertices, shapes, voxelSurfaceBuildSettings());
    const auto t3 = Clock::now();
    check(bakeVoxelSurfaceAtlas(chunk.build, chunk.cells, chunk.settings, {}, chunk.atlas, &error), "terrain: chunk atlas bakes (" + error + ")");
    const auto t4 = Clock::now();
    tMesh += ms(t0, t1);
    tMerge += ms(t1, t2);
    tBuild += ms(t2, t3);
    tAtlas += ms(t3, t4);
  }
  std::printf("    terrain: 4 chunks of %dx%d columns, %llu raw -> %llu LOD0 triangles; per chunk mesh %.1f, merge %.1f, VG build %.1f, atlas %.1f ms; "
              "atlas %ux%u\n",
              kChunk, kChunk, (unsigned long long)rawTriangles, (unsigned long long)lod0Triangles, tMesh / 4, tMerge / 4, tBuild / 4, tAtlas / 4,
              chunks[0].atlas.width, chunks[0].atlas.height);

  // Every chunk at its own cut, all combinations of a few: neighbours meet.
  const float cuts[] = {0.f, 0.05f, 0.3f, 1e30f};
  uint64_t worst = 0u, combinations = 0u;
  for (int combination = 0; combination < 256; combination += 7)
  {
    std::vector<std::array<float, 3>> corners;
    for (int i = 0; i < 4; ++i)
    {
      const auto c = voxelSurfaceCutTriangles(chunks[i].build, cuts[(combination >> (2 * i)) & 3]);
      corners.insert(corners.end(), c.begin(), c.end());
    }
    worst = std::max(worst, interiorOpenEdges(corners, {0, kFloor, 0}, {2 * kChunk, 0, 2 * kChunk}));
    ++combinations;
  }
  std::printf("    terrain: %llu cut combinations, worst %llu open edges inside the 2x2 chunks\n", (unsigned long long)combinations,
              (unsigned long long)worst);
  check(worst == 0u, "terrain: chunks at different cuts meet without cracks");
  for (const float cut : {0.05f, 0.3f, 1e30f})
  {
    uint64_t triangles = 0u;
    for (const auto &chunk : chunks)
      triangles += voxelSurfaceCutTriangles(chunk.build, cut).size() / 3u;
    std::printf("    terrain cut %-6s %llu triangles over 4 chunks\n", cut > 1e29f ? "root" : std::to_string(cut).substr(0, 4).c_str(),
                (unsigned long long)triangles);
  }

  if (outDir)
  {
    VoxelSurfacePreviewView view;
    view.size = 512u;
    view.boundsMin = {0.f, 0.f, 0.f};
    view.boundsMax = {12.8f, 6.0f, 12.8f};
    std::vector<VoxelSurfacePreviewImage> panels;
    for (const float cut : {0.f, 0.05f, 0.3f, 1e30f})
    {
      VoxelSurfacePreviewImage merged;
      std::vector<std::array<float, 3>> allCorners;
      for (const auto &chunk : chunks)
      {
        const auto c = voxelSurfaceCutTriangles(chunk.build, cut);
        allCorners.insert(allCorners.end(), c.begin(), c.end());
      }
      // Each chunk textured from its own atlas, composited by depth: render
      // per chunk and keep covered pixels (chunks do not overlap in view much).
      VoxelSurfacePreviewImage image;
      for (const auto &chunk : chunks)
      {
        std::vector<std::array<float, 2>> uvs;
        auto c = voxelSurfaceCutTriangles(chunk.build, cut, &uvs);
        auto part = renderVoxelSurfaceTexturedPreview(c, uvs, chunk.atlas.rgba, chunk.atlas.width, chunk.atlas.height, view);
        if (image.rgba.empty())
          image = part;
        else
          for (size_t p = 0; p < part.rgba.size(); ++p)
            if (part.rgba[p] != 0xff302820u && image.rgba[p] == 0xff302820u)
              image.rgba[p] = part.rgba[p];
      }
      panels.push_back(image);
    }
    std::string error;
    check(writeVoxelSurfacePreviewSheet(panels, std::string(outDir) + "/terrain-cuts.png", &error), "terrain: writes preview");
  }
}

void dwelling()
{
  using Clock = std::chrono::steady_clock;
  const auto ms = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
  const auto t0 = Clock::now();
  Cells cells;
  double pitch = 0.0;
  if (!dwellingCells(cells, pitch))
  {
    check(false, "dwelling: one dwelling generates and resolves to cells");
    return;
  }
  const auto t1 = Clock::now();

  VoxelSurfaceTriangleMeshSettings settings;
  settings.pitchMetres = pitch;
  VoxelSurfaceTriangleMesh mesh;
  std::string error;
  check(buildVoxelSurfaceTriangleMesh(cells, settings, mesh, &error), "dwelling: meshes (" + error + ")");
  const auto t2 = Clock::now();
  const VoxelSurfaceTopology lod0 = measureVoxelSurfaceTopology(mesh);
  const double volume = double(cells.size()) * pitch * pitch * pitch;
  std::printf("    dwelling: %zu cells at %.0f mm, %llu faces, %zu vertices (%u split), %zu triangles, euler %lld; "
              "generate %.0f ms, mesh %.0f ms\n",
              cells.size(), pitch * 1e3, (unsigned long long)mesh.faceCount, mesh.vertices.size(), mesh.splitVertices,
              mesh.indices.size() / 3u, (long long)lod0.eulerCharacteristic(), ms(t0, t1), ms(t1, t2));
  check(mesh.faceCount == exposedFaces(cells), "dwelling: one quad per exposed face");
  check(lod0.closedManifold(), "dwelling: LOD0 is a closed, consistently wound 2-manifold");
  check(distinctPositions(mesh), "dwelling: LOD0 has no seams (no shared positions)");
  check(std::fabs(lod0.signedVolume - volume) < 1e-4 * volume, "dwelling: LOD0 encloses the cells' volume");

  // From here LOD0 is the losslessly merged surface.
  const size_t rawTriangles = mesh.indices.size() / 3u;
  const auto m0 = Clock::now();
  mergeVoxelSurfaceCoplanar(mesh, settings);
  const VoxelSurfaceTopology merged = measureVoxelSurfaceTopology(mesh);
  std::printf("    dwelling: lossless LOD0 %zu triangles (%.1f%% of %zu), %zu vertices, %.0f ms\n", mesh.indices.size() / 3u,
              100.0 * double(mesh.indices.size() / 3u) / double(rawTriangles), rawTriangles, mesh.vertices.size(), ms(m0, Clock::now()));
  check(merged.closedManifold() && std::fabs(merged.signedVolume - volume) < 1e-4 * volume, "dwelling: merged LOD0 is closed and encloses the cells");
  check(mesh.indices.size() / 3u * 4u <= rawTriangles, "dwelling: the lossless merge removes at least three quarters of the triangles");

  std::vector<Vertex> vertices = mesh.vertices;
  std::vector<Shape> shapes(1);
  shapes[0].indices = mesh.indices;
  const auto t3 = Clock::now();
  const VirtualGeometryBuildData build = VirtualGeometryBuilder::build(vertices, shapes, voxelSurfaceBuildSettings());
  const auto t4 = Clock::now();
  std::printf("    dwelling: VG build %.0f ms (lod0 %.0f, partition %.0f, simplify %.0f, group %.0f cpu ms), %zu clusters, %zu groups, %zu pages\n",
              ms(t3, t4), build.timings.lod0Clusters, build.timings.partition, build.timings.simplify, build.timings.group,
              build.clusters.size(), build.groupInfos.size(), build.pages.size());

  // Cluster budgets and error monotonicity.
  uint32_t overBudget = 0, nonMonotonic = 0;
  uint64_t rootTriangles = 0;
  float maxFiniteError = 0.f;
  for (const auto &cluster : build.clusters)
  {
    overBudget += cluster.vertices.size() > MaxVertices || cluster.indices.size() / 3u > MaxTriangles;
    nonMonotonic += !(cluster.self.error <= cluster.parent.error);
    if (cluster.parent.error == std::numeric_limits<float>::max())
      rootTriangles += cluster.indices.size() / 3u;
    else
      maxFiniteError = std::max(maxFiniteError, cluster.parent.error);
  }
  check(overBudget == 0u, "dwelling: every cluster within MaxVertices and MaxTriangles");
  check(nonMonotonic == 0u, "dwelling: self error never exceeds parent error");
  std::printf("    dwelling: root %llu triangles, coarsest finite error %.3f m\n", (unsigned long long)rootTriangles, maxFiniteError);
  check(rootTriangles <= 4u * ClusterSize, "dwelling: the chain simplifies to a small root (" + std::to_string(rootTriangles) + " triangles)");

  // Every cut: closed (no cracks between clusters of different levels), and
  // near the voxel surface.
  std::vector<std::array<float, 3>> lod0Corners;
  for (uint32_t index : mesh.indices)
    lod0Corners.push_back({mesh.vertices[index].pos[0], mesh.vertices[index].pos[1], mesh.vertices[index].pos[2]});
  const SurfaceIndex surface(lod0Corners);

  const char *outDir = std::getenv("VOXEL_SURFACE_TEST_OUT");
  VoxelSurfacePreviewView view;
  view.size = 512u;
  for (int a = 0; a < 3; ++a)
  {
    view.boundsMin[a] = FLT_MAX;
    view.boundsMax[a] = -FLT_MAX;
  }
  for (const auto &v : mesh.vertices)
    for (int a = 0; a < 3; ++a)
    {
      view.boundsMin[a] = std::min(view.boundsMin[a], v.pos[a]);
      view.boundsMax[a] = std::max(view.boundsMax[a], v.pos[a]);
    }
  std::vector<VoxelSurfacePreviewImage> panels;

  const std::vector<float> cuts = {0.f, float(0.25 * pitch), float(pitch), float(3 * pitch), float(10 * pitch), 1e30f};
  uint64_t previous = UINT64_MAX;
  for (const float cut : cuts)
  {
    const auto corners = voxelSurfaceCutTriangles(build, cut);
    const VoxelSurfaceTopology t = measureVoxelSurfaceTopology(corners);
    // Samples: every vertex and the centroid of every triangle. The maximum
    // is reported, not judged: covering a window recess or a doorway in a flat
    // wall costs a quadric nothing, so the farthest sample is a closed hole,
    // which the bake paints, not a misplaced surface.
    std::vector<float> distances;
    const float bound = cut > 1e29f ? maxFiniteError * 2.f + float(pitch) : 2.f * cut + 1e-4f;
    for (size_t i = 0; i + 2 < corners.size(); i += 3)
    {
      const std::array<float, 3> centroid{(corners[i][0] + corners[i + 1][0] + corners[i + 2][0]) / 3.f,
                                          (corners[i][1] + corners[i + 1][1] + corners[i + 2][1]) / 3.f,
                                          (corners[i][2] + corners[i + 1][2] + corners[i + 2][2]) / 3.f};
      for (const auto &p : {corners[i], centroid})
        distances.push_back(surface.distance(p, 4.f * bound));
    }
    std::sort(distances.begin(), distances.end());
    double sum = 0.0;
    for (float d : distances)
      sum += d;
    const float p99 = distances.empty() ? 0.f : distances[distances.size() * 99u / 100u];
    const float worst = distances.empty() ? 0.f : distances.back();
    char label[32];
    std::snprintf(label, sizeof(label), cut > 1e29f ? "root" : "%.3f m", cut);
    std::printf("    cut %-8s %8llu triangles (%llu zero-area), %llu border, %llu non-manifold, %llu misoriented edges; distance to LOD0 mean %.4f p99 %.4f max %s%.4f m\n",
                label, (unsigned long long)t.triangles, (unsigned long long)t.zeroAreaTriangles, (unsigned long long)t.borderEdges,
                (unsigned long long)t.nonManifoldEdges, (unsigned long long)t.misorientedEdges, distances.empty() ? 0.0 : sum / distances.size(),
                p99, worst > 4.f * bound ? ">" : "", std::min(worst, 4.f * bound));
    check(t.borderEdges == 0u, std::string("dwelling cut ") + label + ": no cracks (border edges)");
    check(p99 <= bound, std::string("dwelling cut ") + label + ": 99% of the surface within twice the cut error of LOD0");
    check(t.triangles <= previous, std::string("dwelling cut ") + label + ": coarser cut, no more triangles");
    if (cut == 0.f)
      check(t.triangles == mesh.indices.size() / 3u, "dwelling: the zero cut is LOD0");
    // With errors summed through the levels (each is measured against the
    // level below it), a one-voxel cut keeps 43% of the merged LOD0.
    if (cut == float(pitch))
      check(t.triangles * 2u <= mesh.indices.size() / 3u, "dwelling: a one-voxel cut keeps under half of LOD0");
    previous = t.triangles;
    if (std::getenv("VOXEL_SURFACE_CRACK_DEBUG") && t.borderEdges)
    {
      // Which clusters own the open edges, and who else has that edge.
      std::map<std::pair<std::array<float, 3>, std::array<float, 3>>, std::vector<size_t>> owners;
      for (size_t c = 0; c < build.clusters.size(); ++c)
      {
        const auto &cl = build.clusters[c];
        for (size_t i = 0; i + 2 < cl.indices.size(); i += 3)
          for (int e = 0; e < 3; ++e)
          {
            const Vertex &a = cl.vertices[cl.indices[i + e]], &b = cl.vertices[cl.indices[i + (e + 1) % 3]];
            std::array<float, 3> pa{a.pos[0], a.pos[1], a.pos[2]}, pb{b.pos[0], b.pos[1], b.pos[2]};
            owners[{std::min(pa, pb), std::max(pa, pb)}].push_back(c);
          }
      }
      std::map<std::pair<std::array<float, 3>, std::array<float, 3>>, int> count;
      for (size_t i = 0; i + 2 < corners.size(); i += 3)
        for (int e = 0; e < 3; ++e)
        {
          auto a = corners[i + e], b = corners[i + (e + 1) % 3];
          ++count[{std::min(a, b), std::max(a, b)}];
        }
      for (const auto &[edge, n] : count)
        if (n == 1)
        {
          std::printf("      open edge (%.3f %.3f %.3f)-(%.3f %.3f %.3f):", edge.first[0], edge.first[1], edge.first[2], edge.second[0], edge.second[1], edge.second[2]);
          for (size_t c : owners[edge])
            std::printf(" [c%zu g%u self %.4f parent %.4g]", c, build.clusters[c].groupId, build.clusters[c].self.error, build.clusters[c].parent.error);
          std::printf("\n");
        }
    }
    if (outDir)
      panels.push_back(renderVoxelSurfacePreview(corners, cells, settings, view, std::max(2.f * float(pitch), cut > 1e29f ? 2.f * maxFiniteError : 2.f * cut)));
  }

  // Deterministic: the same instance builds the same DAG.
  {
    std::vector<Vertex> again = mesh.vertices;
    const VirtualGeometryBuildData rebuilt = VirtualGeometryBuilder::build(again, shapes, voxelSurfaceBuildSettings());
    bool same = rebuilt.clusters.size() == build.clusters.size();
    for (size_t c = 0; same && c < build.clusters.size(); ++c)
      same = rebuilt.clusters[c].indices == build.clusters[c].indices && rebuilt.clusters[c].self.error == build.clusters[c].self.error;
    check(same, "dwelling: the DAG build is deterministic");
  }

  dwellingAtlas(build, cells, settings, view, cuts, outDir);

  if (outDir)
  {
    const std::string base = std::string(outDir) + "/dwelling";
    check(writeVoxelSurfaceTriangleMeshGLTF(mesh, base, &error), "dwelling: writes glTF (" + error + ")");
    check(writeVoxelSurfacePreviewSheet(panels, base + "-cuts.png", &error), "dwelling: writes preview (" + error + ")");
    std::printf("    wrote %s.gltf and %s-cuts.png\n", base.c_str(), base.c_str());
  }
}

} // namespace

int main()
{
  setVirtualGeometryBuildLogging(false);
  std::cout << "known shapes\n";
  knownShapes();
  std::cout << "random cell sets\n";
  randomCellSets();
  std::cout << "one dwelling\n";
  dwelling();
  std::cout << "terrain chunks\n";
  terrainChunks(std::getenv("VOXEL_SURFACE_TEST_OUT"));
  if (failures)
  {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "VoxelSurfaceMeshTests passed\n";
  return 0;
}
