// Per-geometry VG statistics of a .cvscene: pages, hierarchy nodes, encoded
// bytes and clusters, sampled evenly over the geometry table. Sizes the
// city kit object (slots x nodes x pages) against what the bake produced.
#include "virtualgeometry/VirtualGeometryFile.hpp"
#include "editor/virtualgeometry/VirtualGeometryBuilder.hpp"
#include "editor/virtualgeometry/CityVoxelSurfaceVirtualGeometry.hpp"
#include "editor/virtualgeometry/VoxelSurfaceAtlas.hpp"
#include "editor/virtualgeometry/VoxelSurfaceTriangleMesh.hpp"
#include "voxel/svraster/CityVoxelAssetCompiler.hpp"
#include "editor/virtualgeometry/VirtualGeometryCompressor.hpp"
#include <chrono>
#include <cstring>
#include "voxel/svraster/CityVoxelSceneFormat.hpp"
#include "voxel/svraster/CityVoxelVGPage.hpp"
#include "virtualgeometry/CityVoxelStaticScene.hpp"
#include "voxel/svraster/CityGroundOverlay.hpp"
#include <unordered_map>
#include <cmath>
#include "editor/virtualgeometry/CityVoxelVirtualGeometryCompiler.hpp"
#include "editor/virtualgeometry/VirtualGeometryDecoder.hpp"
#include <fstream>
#include <iterator>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

// Writes one synthetic geometry's VG to a file and to memory and compares.
static int vgWriteCheck() {
  std::vector<voxel::svraster::CityVoxelCell> cells;
  for (int x = 0; x < 12; ++x) for (int y = 0; y < 9; ++y) for (int z = 0; z < 3; ++z)
    if (x == 0 || x == 11 || y == 0 || y == 8 || z == 0 || (x * 7 + y * 3) % 5 == 0)
      cells.push_back({{x, y, z}, 0xff8090a0u + uint32_t(x), 1u});
  voxel::svraster::CityVoxelAssetCompiler assets;
  voxel::svraster::CityVoxelCompiledPlacement placement;
  std::string error;
  if (!assets.submit(7, 100000, cells, placement, &error)) { std::printf("submit: %s\n", error.c_str()); return 1; }
  voxel::svraster::CityVoxelVGAsset asset;
  if (!voxel::svraster::buildCityVoxelVirtualGeometry(assets.geometry().front(), {}, asset, &error)) {
    std::printf("vg: %s\n", error.c_str()); return 1;
  }
  const std::string path = "/tmp/city-vg-write-check.vg";
  { virtualgeometry::VirtualGeometryFile file(path, true);
    file.write(asset.encoded, asset.buildPages, virtualgeometry::MESHLET_LZ4, nullptr, 1u); }
  std::ifstream in(path, std::ios::binary);
  std::vector<uint8_t> disk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<uint8_t> memory;
  virtualgeometry::VirtualGeometryFile::writeToMemory(asset.encoded, asset.buildPages, virtualgeometry::MESHLET_LZ4, memory, 1u);
  size_t first = 0;
  while (first < std::min(disk.size(), memory.size()) && disk[first] == memory[first]) ++first;
  std::printf("disk %zu bytes, memory %zu bytes, first difference at %zu\n", disk.size(), memory.size(), first);
  return disk == memory ? 0 : 2;
}

// Placements of one house should sit together; a block far from its
// house's median position was placed through the wrong frame.
static int ownerSpread(const char *path) {
  voxel::svraster::CityVoxelSceneFileReader reader;
  std::string error;
  virtualgeometry::CityVoxelStaticScene scene;
  if (!reader.open(path, {}, &error) || !scene.loadSceneIndex(reader.index(), &error)) {
    std::fprintf(stderr, "%s\n", error.c_str()); return 2;
  }
  const auto &index = reader.index();
  std::unordered_map<uint64_t, std::vector<std::array<double, 3>>> byOwner;
  for (size_t i = 0; i < index.instances.size(); ++i) {
    const auto p = scene.worldPosition(scene.placements()[i]);
    byOwner[index.instances[i].ownerId].push_back({p.x(), p.y(), p.z()});
  }
  uint64_t far = 0, farOwners = 0, total = 0;
  std::vector<double> spread;
  int printed = 0;
  for (auto &[owner, points] : byOwner) {
    std::array<double, 3> median{};
    for (unsigned a = 0; a < 3; ++a) {
      std::vector<double> v; for (auto &p : points) v.push_back(p[a]);
      std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); median[a] = v[v.size() / 2];
    }
    uint64_t ownerFar = 0;
    for (auto &p : points) {
      const double d = std::sqrt((p[0]-median[0])*(p[0]-median[0]) + (p[1]-median[1])*(p[1]-median[1]) + (p[2]-median[2])*(p[2]-median[2]));
      spread.push_back(d);
      if (d > 40.0) { ++ownerFar; if (printed < 8) { ++printed;
        std::printf("owner %llu: block at (%.1f %.1f %.1f) is %.1f m from median (%.1f %.1f %.1f), %zu blocks\n",
          (unsigned long long)owner, p[0], p[1], p[2], d, median[0], median[1], median[2], points.size()); } }
    }
    far += ownerFar; farOwners += ownerFar != 0; total += points.size();
  }
  std::sort(spread.begin(), spread.end());
  std::printf("%zu owners, %llu placements; %llu placements in %llu owners are over 40 m from their median; "
              "distance p50 %.1f p99 %.1f max %.1f\n", byOwner.size(), (unsigned long long)total,
              (unsigned long long)far, (unsigned long long)farOwners, spread[spread.size() / 2],
              spread[size_t(.99 * (spread.size() - 1))], spread.back());
  return 0;
}

// Overlay ground minus each house's lowest block, at the house's centre: a
// positive gap is a house standing in the ground.
static int groundCheck(const char *scenePath, const char *groundPath) {
  voxel::svraster::CityVoxelSceneFileReader reader;
  virtualgeometry::CityVoxelStaticScene scene;
  voxel::svraster::CityGroundOverlayData data;
  voxel::svraster::CityGroundOverlay overlay;
  std::string error;
  if (!reader.open(scenePath, {}, &error) || !scene.loadSceneIndex(reader.index(), &error) ||
      !voxel::svraster::readCityGroundOverlay(groundPath, data, &error) || !overlay.load(std::move(data), &error)) {
    std::fprintf(stderr, "%s\n", error.c_str()); return 2;
  }
  struct Owner { double x = 0, z = 0, low = 1e30; uint32_t n = 0; };
  std::unordered_map<uint64_t, Owner> owners;
  for (size_t i = 0; i < reader.index().instances.size(); ++i) {
    const auto p = scene.worldPosition(scene.placements()[i]);
    auto &o = owners[reader.index().instances[i].ownerId];
    o.x += p.x(); o.z += p.z(); o.low = std::min(o.low, double(p.y())); ++o.n;
  }
  std::vector<double> gaps;
  for (auto &[id, o] : owners) {
    const auto column = overlay.sample(o.x / o.n, o.z / o.n);
    if (column.weight <= 0) continue;
    gaps.push_back(column.heightMetres - o.low);
  }
  std::sort(gaps.begin(), gaps.end());
  const auto at = [&](double q) { return gaps[size_t(q * (gaps.size() - 1))]; };
  size_t buried = 0; for (double g : gaps) buried += g > 0.5;
  std::printf("%zu houses: ground - lowest block p1 %.2f p50 %.2f p90 %.2f p99 %.2f max %.2f m; %zu over 0.5 m\n",
              gaps.size(), at(.01), at(.5), at(.9), at(.99), gaps.back(), buried);
  return 0;
}

// One house's blocks decoded on the CPU and placed through the renderer's
// transforms, drawn as top and side height maps: whether the file holds
// whole houses, independent of the GPU path.
static int ownerView(const char *scenePath, uint64_t owner, const char *outPath, bool fromVG = false, uint32_t level = 0) {
  voxel::svraster::CityVoxelSceneFileReader reader;
  virtualgeometry::CityVoxelStaticScene scene;
  std::string error;
  if (!reader.open(scenePath, {}, &error) || !scene.loadSceneIndex(reader.index(), &error)) {
    std::fprintf(stderr, "%s\n", error.c_str()); return 2;
  }
  const auto &index = reader.index();
  if (!owner) {  // The house with the most blocks.
    std::unordered_map<uint64_t, uint32_t> counts;
    for (const auto &instance : index.instances) ++counts[instance.ownerId];
    uint32_t best = 0; for (auto &[id, n] : counts) if (n > best && n < 400) { best = n; owner = id; }
  }
  std::vector<std::array<double, 3>> points;
  for (size_t i = 0; i < index.instances.size(); ++i) {
    if (index.instances[i].ownerId != owner) continue;
    const auto &placement = scene.placements()[i];
    const auto &record = index.geometry[placement.geometryIndex];
    std::vector<uint8_t> bytes;
    if (!reader.readPage(record.firstPage, bytes, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 3; }
    voxel::svraster::CityVoxelCompiledGeometry asset;
    asset.id = record.id; asset.contentHash = record.contentHash; asset.addressHash = record.voxelAddressHash;
    asset.pitchMicrometres = record.pitchMicrometres;
    for (unsigned a = 0; a < 3; ++a) asset.extent[a] = int32_t(int64_t(record.maximumCell[a]) - record.minimumCell[a]);
    uint64_t occupied = 0;
    const uint32_t bricks = uint32_t(bytes[24] | bytes[25] << 8 | bytes[26] << 16 | uint32_t(bytes[27]) << 24);
    for (uint32_t b = 0; b < bricks; ++b) for (uint32_t w = 0; w < 64; ++w)
      occupied += __builtin_popcount(bytes[28 + b * 76 + 12 + w]);
    asset.occupiedCount = uint32_t(occupied); asset.bytes = bytes;
    const auto origin = scene.worldPosition(placement);
    const auto rotation = scene.worldRotation(placement);
    const double pitch = record.pitchMicrometres * 1e-6;
    if (fromVG) {
      // LOD0 triangles of the block's VG, sampled densely.
      uint32_t vgPageId = UINT32_MAX;
      for (const auto &page : index.pages)
        if (page.kind == voxel::svraster::CityVoxelPageKind::VirtualGeometry && page.assetId == record.id) { vgPageId = page.id; break; }
      std::vector<uint8_t> vgBytes;
      voxel::svraster::CityVoxelVGPageView view;
      virtualgeometry::VirtualGeometryEncodedData data;
      if (vgPageId == UINT32_MAX || !reader.readPage(vgPageId, vgBytes, &error) ||
          !voxel::svraster::decodeCityVoxelVGPage(vgBytes.data(), vgBytes.size(), record.id, record.contentHash, view, &error) ||
          !virtualgeometry::VirtualGeometryFile::readFromMemory(view.vgFileBytes, view.vgFileSize, data, &error)) {
        std::fprintf(stderr, "vg: %s\n", error.c_str()); return 3;
      }
      size_t cluster = 0;
      for (uint32_t pg = 0; pg < data.pages.size(); ++pg)
        for (uint32_t m = 0; m < data.pages[pg].meshlets.size(); ++m, ++cluster) {
          if (cluster >= view.clusterSources.size() || view.clusterSources[cluster].level != level) continue;
          std::vector<virtualgeometry::Vertex> vertices; std::vector<uint32_t> indices;
          virtualgeometry::VirtualGeometryDecoder::decodeMeshlet(data, pg, m, vertices, indices);
          for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            const auto &A = vertices[indices[t]].pos, &B = vertices[indices[t + 1]].pos, &C = vertices[indices[t + 2]].pos;
            for (double u = 0; u <= 1.0; u += 0.05) for (double v = 0; u + v <= 1.0; v += 0.05) {
              const math::Vec3f local(float(A[0] + (B[0] - A[0]) * u + (C[0] - A[0]) * v),
                                      float(A[1] + (B[1] - A[1]) * u + (C[1] - A[1]) * v),
                                      float(A[2] + (B[2] - A[2]) * u + (C[2] - A[2]) * v));
              const math::Vec3f w = rotation * local;
              points.push_back({origin.x() + w.x(), origin.y() + w.y(), origin.z() + w.z()});
            }
          }
        }
      continue;
    }
    std::vector<std::array<int32_t, 3>> cells;
    if (!voxel::svraster::decodeCityVoxelGeometry(asset, cells, nullptr, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 3; }
    for (const auto &c : cells) {
      const math::Vec3f local(float((c[0] + .5) * pitch), float((c[1] + .5) * pitch), float((c[2] + .5) * pitch));
      const math::Vec3f w = rotation * local;
      points.push_back({origin.x() + w.x(), origin.y() + w.y(), origin.z() + w.z()});
    }
  }
  if (points.empty()) { std::fprintf(stderr, "owner has no cells\n"); return 4; }
  std::array<double, 3> lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
  for (auto &p : points) for (unsigned a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], p[a]); hi[a] = std::max(hi[a], p[a]); }
  const double scale = 10.0; // pixels per metre
  const int W = int((hi[0] - lo[0]) * scale) + 2, D = int((hi[2] - lo[2]) * scale) + 2, H = int((hi[1] - lo[1]) * scale) + 2;
  const int width = W + 4 + W, height = std::max(D, H);
  std::vector<double> top(size_t(W) * D, -1e30), side(size_t(W) * H, 1e30);
  for (auto &p : points) {
    const int x = int(std::lround((p[0] - lo[0]) * scale)), z = int(std::lround((p[2] - lo[2]) * scale)), y = int(std::lround((hi[1] - p[1]) * scale));
    top[size_t(z) * W + x] = std::max(top[size_t(z) * W + x], p[1]);
    side[size_t(y) * W + x] = std::min(side[size_t(y) * W + x], p[2]);
  }
  std::FILE *f = std::fopen(outPath, "wb");
  std::fprintf(f, "P6 %d %d 255\n", width, height);
  for (int r = 0; r < height; ++r) for (int c = 0; c < width; ++c) {
    unsigned char rgb[3] = {30, 30, 40};
    if (c < W && r < D && top[size_t(r) * W + c] > -1e29) {
      const double t = (top[size_t(r) * W + c] - lo[1]) / std::max(1e-6, hi[1] - lo[1]);
      rgb[0] = rgb[1] = rgb[2] = (unsigned char)(60 + 195 * t);
    } else if (c >= W + 4 && r < H && side[size_t(r) * W + (c - W - 4)] < 1e29) {
      const double t = (side[size_t(r) * W + (c - W - 4)] - lo[2]) / std::max(1e-6, hi[2] - lo[2]);
      rgb[0] = (unsigned char)(80 + 150 * (1 - t)); rgb[1] = (unsigned char)(120 + 100 * (1 - t)); rgb[2] = 90;
    }
    std::fwrite(rgb, 1, 3, f);
  }
  std::fclose(f);
  std::printf("owner %llu: %zu cells, %.1f x %.1f x %.1f m, city min (%.2f %.2f %.2f) max (%.2f %.2f %.2f)\n",
              (unsigned long long)owner, points.size(), hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2],
              lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
  return 0;
}



// One house as one welded triangle mesh: its blocks' exact cells in the
// house's own lattice, meshed by VoxelSurfaceTriangleMesh, written as glTF,
// and run through the stock virtual geometry builder with meshoptimizer
// clustering and positions-only simplification.
static int ownerMesh(const char *scenePath, uint64_t owner, const char *outBase) {
  voxel::svraster::CityVoxelSceneFileReader reader;
  virtualgeometry::CityVoxelStaticScene scene;
  std::string error;
  if (!reader.open(scenePath, {}, &error) || !scene.loadSceneIndex(reader.index(), &error)) {
    std::fprintf(stderr, "%s\n", error.c_str()); return 2;
  }
  const auto &index = reader.index();
  if (!owner) {
    std::unordered_map<uint64_t, uint32_t> counts;
    for (const auto &instance : index.instances) ++counts[instance.ownerId];
    uint32_t best = 0; for (auto &[id, n] : counts) if (n > best && n < 400) { best = n; owner = id; }
  }
  const auto start = std::chrono::steady_clock::now();
  std::vector<virtualgeometry::VoxelSurfaceCell> cells;
  bool haveFrame = false;
  math::Vec3f frameOrigin;
  math::Quatf frameInverse;
  double pitch = 0.1;
  for (size_t i = 0; i < index.instances.size(); ++i) {
    if (index.instances[i].ownerId != owner) continue;
    const auto &placement = scene.placements()[i];
    const auto &record = index.geometry[placement.geometryIndex];
    voxel::svraster::CityVoxelCompiledGeometry geometry;
    geometry.id = record.id; geometry.contentHash = record.contentHash; geometry.addressHash = record.voxelAddressHash;
    geometry.pitchMicrometres = record.pitchMicrometres;
    for (unsigned a = 0; a < 3; ++a) geometry.extent[a] = int32_t(int64_t(record.maximumCell[a]) - record.minimumCell[a]);
    if (!reader.readPage(record.firstPage, geometry.bytes, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 3; }
    const uint32_t bricks = uint32_t(geometry.bytes[24] | geometry.bytes[25] << 8 | geometry.bytes[26] << 16 | uint32_t(geometry.bytes[27]) << 24);
    uint64_t occupied = 0;
    for (uint32_t b = 0; b < bricks; ++b) for (uint32_t w = 0; w < 64; ++w) occupied += __builtin_popcount(geometry.bytes[28 + b * 76 + 12 + w]);
    geometry.occupiedCount = uint32_t(occupied);
    const auto &appearanceRecord = index.appearances[placement.appearanceIndex];
    voxel::svraster::CityVoxelCompiledAppearance appearance;
    appearance.id = appearanceRecord.id; appearance.contentHash = appearanceRecord.contentHash;
    appearance.geometryAddressHash = appearanceRecord.geometryAddressHash; appearance.geometryId = appearanceRecord.geometryId;
    appearance.codec = appearanceRecord.codec; appearance.occupiedCount = geometry.occupiedCount;
    if (!reader.readPage(appearanceRecord.firstPage, appearance.bytes, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 3; }
    std::vector<std::array<int32_t, 3>> positions;
    std::vector<voxel::svraster::CityVoxelCell> values;
    if (!voxel::svraster::decodeCityVoxelGeometry(geometry, positions, nullptr, &error) ||
        !voxel::svraster::decodeCityVoxelAppearance(appearance, values, &error) || values.size() != positions.size()) {
      std::fprintf(stderr, "decode: %s (%zu values, %zu cells)\n", error.c_str(), values.size(), positions.size()); return 3;
    }
    pitch = record.pitchMicrometres * 1e-6;
    const auto origin = scene.worldPosition(placement);
    const auto rotation = scene.worldRotation(placement);
    if (!haveFrame) {
      haveFrame = true; frameOrigin = origin;
      frameInverse = math::Quatf(-rotation.x, -rotation.y, -rotation.z, rotation.w);
    }
    for (size_t c = 0; c < positions.size(); ++c) {
      const auto &p = positions[c];
      const math::Vec3f local(float((p[0] + .5) * pitch), float((p[1] + .5) * pitch), float((p[2] + .5) * pitch));
      const math::Vec3f house = frameInverse * (origin + rotation * local - frameOrigin);
      cells.push_back({{int32_t(std::floor(house.x() / pitch)), int32_t(std::floor(house.y() / pitch)),
                        int32_t(std::floor(house.z() / pitch))}, values[c].rgba});
    }
  }
  if (cells.empty()) { std::fprintf(stderr, "owner has no cells\n"); return 4; }
  // Blocks are disjoint, but keep the first value of any duplicate lattice cell.
  std::sort(cells.begin(), cells.end(), [](const auto &l, const auto &r) { return l.position < r.position; });
  cells.erase(std::unique(cells.begin(), cells.end(), [](const auto &l, const auto &r) { return l.position == r.position; }), cells.end());
  const auto gathered = std::chrono::steady_clock::now();

  virtualgeometry::VoxelSurfaceTriangleMeshSettings settings;
  settings.pitchMetres = pitch;
  virtualgeometry::VoxelSurfaceTriangleMesh mesh;
  if (!virtualgeometry::buildVoxelSurfaceTriangleMesh(cells, settings, mesh, &error)) { std::fprintf(stderr, "mesh: %s\n", error.c_str()); return 5; }
  const size_t rawTriangles = mesh.indices.size() / 3u;
  virtualgeometry::mergeVoxelSurfaceCoplanar(mesh, settings);
  std::printf("lossless LOD0: %zu of %zu triangles\n", mesh.indices.size() / 3u, rawTriangles);
  const auto meshed = std::chrono::steady_clock::now();
  if (!virtualgeometry::writeVoxelSurfaceTriangleMeshGLTF(mesh, outBase, &error)) { std::fprintf(stderr, "gltf: %s\n", error.c_str()); return 6; }

  std::vector<virtualgeometry::Vertex> vertices = mesh.vertices;
  std::vector<virtualgeometry::Shape> shapes(1);
  shapes[0].indices = mesh.indices;
  const auto buildStart = std::chrono::steady_clock::now();
  auto build = virtualgeometry::VirtualGeometryBuilder::build(vertices, shapes, virtualgeometry::voxelSurfaceBuildSettings());
  const auto built = std::chrono::steady_clock::now();
  const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };

  const auto topology = virtualgeometry::measureVoxelSurfaceTopology(mesh);
  std::printf("owner %llu: %zu cells, %llu faces, %zu vertices (%u split), %zu triangles; closed %d, euler %lld\n",
              (unsigned long long)owner, cells.size(), (unsigned long long)mesh.faceCount, mesh.vertices.size(), mesh.splitVertices,
              mesh.indices.size() / 3, int(topology.closedManifold()), (long long)topology.eulerCharacteristic());
  std::printf("time: gather %.0f ms, mesh %.0f ms, vg build %.0f ms; %zu clusters, %zu pages, %zu groups\n",
              ms(start, gathered), ms(gathered, meshed), ms(buildStart, built), build.clusters.size(), build.pages.size(), build.groupInfos.size());
  const auto &t = build.timings;
  std::printf("builder: lod0 %.0f partition %.0f simplify %.0f group %.0f hierarchy %.0f merge %.0f pack %.0f ms, %u front threads\n",
              t.lod0Clusters, t.partition, t.simplify, t.group, t.hierarchy, t.merge, t.packPages, t.frontThreads);

  virtualgeometry::VoxelSurfacePreviewView view;
  view.size = 640u;
  for (unsigned k = 0; k < 3; ++k) { view.boundsMin[k] = 1e30f; view.boundsMax[k] = -1e30f; }
  for (const auto &vertex : mesh.vertices)
    for (unsigned k = 0; k < 3; ++k) { view.boundsMin[k] = std::min(view.boundsMin[k], vertex.pos[k]); view.boundsMax[k] = std::max(view.boundsMax[k], vertex.pos[k]); }
  // Bake the atlas, write the asset, and render each cut twice: the voxels
  // marched from the surface (truth), and the atlas at the baked UVs.
  virtualgeometry::VoxelSurfaceAtlas atlas;
  const auto bakeStart = std::chrono::steady_clock::now();
  if (!virtualgeometry::bakeVoxelSurfaceAtlas(build, cells, settings, {}, atlas, &error)) { std::fprintf(stderr, "bake: %s\n", error.c_str()); return 7; }
  const auto baked = std::chrono::steady_clock::now();
  if (!virtualgeometry::writeVoxelSurfaceAsset(build, atlas, outBase, &error)) { std::fprintf(stderr, "asset: %s\n", error.c_str()); return 8; }
  std::printf("atlas: %ux%u, %zu charts (%u peeled), %u vertex copies, max %u cluster vertices, %u folded, %llu texels, %llu missed; bake %.0f ms, write %.0f ms\n",
              atlas.width, atlas.height, atlas.charts.size(), atlas.peeledCharts, atlas.addedVertices, atlas.maxClusterVertices, atlas.mergedClusters,
              (unsigned long long)atlas.coveredTexels, (unsigned long long)atlas.missedTexels, ms(bakeStart, baked), ms(baked, std::chrono::steady_clock::now()));
  const float cuts[] = {0.f, 0.01f, 0.03f, 0.1f, 0.3f, 1.0f};
  std::vector<virtualgeometry::VoxelSurfacePreviewImage> panels, texturedPanels;
  for (const float cut : cuts) {
    std::vector<std::array<float, 2>> uvs;
    const auto triangles = virtualgeometry::voxelSurfaceCutTriangles(build, cut, &uvs);
    const auto cutTopology = virtualgeometry::measureVoxelSurfaceTopology(triangles);
    panels.push_back(virtualgeometry::renderVoxelSurfacePreview(triangles, cells, settings, view, std::max(2.f * float(pitch), 2.f * cut)));
    texturedPanels.push_back(virtualgeometry::renderVoxelSurfaceTexturedPreview(triangles, uvs, atlas.rgba, atlas.width, atlas.height, view));
    const auto difference = [](const auto &image, const auto &reference) {
      double diff = 0; uint64_t pixels = 0;
      for (size_t p = 0; p < image.rgba.size(); ++p) {
        if (image.rgba[p] == 0xff302820u && reference.rgba[p] == 0xff302820u) continue;
        ++pixels;
        for (int k = 0; k < 3; ++k) diff += std::abs(int((image.rgba[p] >> (8 * k)) & 255u) - int((reference.rgba[p] >> (8 * k)) & 255u));
      }
      return pixels ? diff / (3.0 * pixels) : 0.0;
    };
    std::printf("  cut %.2f m: %8llu triangles, %llu border / %llu non-manifold edges; mean |rgb|: voxels vs LOD0 %.2f, textured vs voxels %.2f, textured vs LOD0 %.2f\n",
                cut, (unsigned long long)cutTopology.triangles, (unsigned long long)cutTopology.borderEdges, (unsigned long long)cutTopology.nonManifoldEdges,
                difference(panels.back(), panels.front()), difference(texturedPanels.back(), panels.back()), difference(texturedPanels.back(), panels.front()));
  }
  panels.insert(panels.end(), texturedPanels.begin(), texturedPanels.end());
  const std::string sheetPath = std::string(outBase) + "-cuts.png";
  if (!virtualgeometry::writeVoxelSurfacePreviewSheet(panels, sheetPath, &error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 7; }
  std::printf("wrote %s.{gltf,bin,png,vmat,vg} and %s (top-left six: voxels, then textured)\n", outBase, sheetPath.c_str());
  return 0;
}

// The scene with every VG page rebuilt through the welded-surface pipeline.
static int rebuildVG(const char *input, const char *output, uint32_t threads) {
  virtualgeometry::CityVoxelSurfaceRebuildStats stats;
  std::string error;
  const bool ok = virtualgeometry::rebuildCityVoxelSceneSurfaceVG(input, output, {}, threads, &stats, &error);
  std::printf("rebuild-vg: %u geometries rebuilt, %u kept their octree page; %llu raw -> %llu LOD0 triangles, %llu clusters; "
              "VG bytes %.1f -> %.1f MiB; %.1f s\n",
              stats.geometries, stats.failed, (unsigned long long)stats.rawTriangles, (unsigned long long)stats.lod0Triangles,
              (unsigned long long)stats.clusters, stats.oldVGBytes / 1048576.0, stats.newVGBytes / 1048576.0, stats.buildSeconds);
  for (const auto &[reason, count] : stats.failureReasons)
    std::printf("  kept octree: %u x %s\n", count, reason.c_str());
  if (!ok) { std::fprintf(stderr, "rebuild-vg: %s\n", error.c_str()); return 2; }
  return 0;
}

// One mesh per house, with surface VG pages.
static int houseScene(const char *input, const char *output, uint32_t threads) {
  virtualgeometry::CityVoxelHouseSceneStats stats;
  std::string error;
  const bool ok = virtualgeometry::buildCityVoxelHouseScene(input, output, {}, threads, &stats, &error);
  std::printf("house-scene: %u houses (%u merged, %u kept their blocks), %u -> %u placements, %u geometries, %llu cells, %.1f s\n",
              stats.houses, stats.mergedHouses, stats.splitHouses, stats.sourcePlacements, stats.placements, stats.geometries,
              (unsigned long long)stats.cells, stats.seconds);
  if (!ok) { std::fprintf(stderr, "house-scene: %s\n", error.c_str()); return 2; }
  return 0;
}

// Far proxies shaped from the detailed scene's 10 cm cells.
static int farFromDetail(const char *detail, const char *far, const char *output, double errorMetres, uint32_t threads) {
  virtualgeometry::CityVoxelFarSceneStats stats;
  std::string error;
  const bool ok = virtualgeometry::rebuildCityVoxelFarSceneFromDetail(detail, far, output, {}, threads, errorMetres, &stats, &error);
  std::printf("far-from-detail: %u rebuilt, %u kept; %llu triangles; VG %.1f -> %.1f MiB; %.1f s\n", stats.rebuilt, stats.kept,
              (unsigned long long)stats.triangles, stats.oldVGBytes / 1048576.0, stats.newVGBytes / 1048576.0, stats.seconds);
  for (const auto &[reason, count] : stats.keptReasons)
    std::printf("  kept: %u x %s\n", count, reason.c_str());
  if (!ok) { std::fprintf(stderr, "far-from-detail: %s\n", error.c_str()); return 2; }
  return 0;
}

int main(int argc, char **argv) {
  if (argc >= 5 && std::string(argv[1]) == "--far-from-detail")
    return farFromDetail(argv[2], argv[3], argv[4], argc >= 6 ? std::stod(argv[5]) : 0.1, argc >= 7 ? uint32_t(std::stoul(argv[6])) : 4u);
  if (argc >= 4 && std::string(argv[1]) == "--house-scene")
    return houseScene(argv[2], argv[3], argc >= 5 ? uint32_t(std::stoul(argv[4])) : 4u);
  if (argc >= 4 && std::string(argv[1]) == "--rebuild-vg")
    return rebuildVG(argv[2], argv[3], argc >= 5 ? uint32_t(std::stoul(argv[4])) : 4u);
  if (argc == 5 && std::string(argv[1]) == "--owner-mesh") return ownerMesh(argv[2], std::stoull(argv[3]), argv[4]);
  if (argc == 5 && std::string(argv[1]) == "--owner-view") return ownerView(argv[2], std::stoull(argv[3]), argv[4]);
  if (argc >= 5 && std::string(argv[1]) == "--owner-vg-view")
    return ownerView(argv[2], std::stoull(argv[3]), argv[4], true, argc > 5 ? uint32_t(std::stoul(argv[5])) : 0u);
  if (argc == 5 && std::string(argv[1]) == "--ground-at") {
    voxel::svraster::CityGroundOverlayData data;
    voxel::svraster::CityGroundOverlay overlay;
    std::string error;
    if (!voxel::svraster::readCityGroundOverlay(argv[2], data, &error) || !overlay.load(std::move(data), &error)) return 2;
    const auto c = overlay.sample(std::stod(argv[3]), std::stod(argv[4]));
    std::printf("site-local (%s, %s): height %.2f weight %.2f material %u paved %d; site origin mm (%lld, %lld)\n",
                argv[3], argv[4], c.heightMetres, c.weight, unsigned(c.material), int(c.paved),
                (long long)overlay.data().siteOriginMillimetres[0], (long long)overlay.data().siteOriginMillimetres[1]);
    return 0;
  }
  if (argc == 4 && std::string(argv[1]) == "--ground-check") return groundCheck(argv[2], argv[3]);
  if (argc == 2 && std::string(argv[1]) == "--vg-write-check") return vgWriteCheck();
  if (argc == 3 && std::string(argv[1]) == "--owner-spread") return ownerSpread(argv[2]);
  if (argc < 2) {
    std::fprintf(stderr, "usage: CityVoxelSceneStats scene.cvscene [samples]\n");
    return 1;
  }
  const uint32_t samples = argc > 2 ? uint32_t(std::stoul(argv[2])) : 2000u;
  voxel::svraster::CityVoxelSceneFileReader reader;
  std::string error;
  if (!reader.open(argv[1], {}, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 2;
  }
  const auto &index = reader.index();
  std::map<uint64_t, uint32_t> vgPage;
  for (const auto &page : index.pages)
    if (page.kind == voxel::svraster::CityVoxelPageKind::VirtualGeometry) vgPage[page.assetId] = page.id;
  std::vector<uint32_t> pages, nodes, clusters, bytes, triangles, vertices;
  const uint32_t step = std::max<uint32_t>(1u, uint32_t(index.geometry.size()) / std::max(1u, samples));
  for (uint32_t g = 0; g < index.geometry.size(); g += step) {
    const auto &geometry = index.geometry[g];
    std::vector<uint8_t> payload;
    voxel::svraster::CityVoxelVGPageView view;
    virtualgeometry::VirtualGeometryEncodedData data;
    if (!vgPage.count(geometry.id) || !reader.readPage(vgPage[geometry.id], payload, &error) ||
        !voxel::svraster::decodeCityVoxelVGPage(payload.data(), payload.size(), geometry.id,
                                                geometry.contentHash, view, &error) ||
        !virtualgeometry::VirtualGeometryFile::readFromMemory(view.vgFileBytes, view.vgFileSize, data, &error)) {
      std::fprintf(stderr, "geometry %u: %s\n", g, error.c_str());
      return 3;
    }
    if (const char *dump = std::getenv("CITY_STATS_DUMP_VG"); dump && pages.empty()) {
      std::FILE *f = std::fopen(dump, "wb");
      if (f) { std::fwrite(view.vgFileBytes, 1, view.vgFileSize, f); std::fclose(f); }
    }
    if (std::getenv("CITY_STATS_NODES") && pages.empty())
      for (size_t n = 0; n < data.hierarchy.size(); ++n)
        std::printf("node %zu meshPart %u flags 0x%x childStart %u childCount %u page %u selfErr %g parentErr %g\n", n,
                    data.hierarchy[n].meshPartIndex, data.hierarchy[n].flags, data.hierarchy[n].child_start,
                    data.hierarchy[n].child_count, data.hierarchy[n].pageIndex, data.hierarchy[n].min_lod_error,
                    data.hierarchy[n].max_parent_lod_error);
    uint32_t c = 0, t = 0, v = 0;
    for (const auto &page : data.pages)
      for (const auto &meshlet : page.meshlets) { ++c; t += meshlet.triangle_count; v += meshlet.vertex_count; }
    triangles.push_back(t);
    vertices.push_back(v);
    pages.push_back(uint32_t(data.pages.size()));
    nodes.push_back(uint32_t(data.hierarchy.size()));
    clusters.push_back(c);
    bytes.push_back(uint32_t(payload.size()));
  }
  const auto report = [](const char *name, std::vector<uint32_t> values) {
    std::sort(values.begin(), values.end());
    const auto at = [&](double q) { return values[size_t(q * double(values.size() - 1))]; };
    std::printf("%-10s p50 %6u  p90 %6u  p99 %6u  max %6u\n", name, at(.5), at(.9), at(.99), values.back());
  };
  std::printf("%zu geometries, %zu appearances, %zu instances, %zu cells; sampled %zu\n",
              index.geometry.size(), index.appearances.size(), index.instances.size(),
              index.spatialCells.size(), pages.size());
  report("pages", pages);
  report("nodes", nodes);
  report("clusters", clusters);
  report("vgBytes", bytes);
  report("triangles", triangles);
  report("vertices", vertices);
  const char *kinds[] = {"?", "geometry", "appearance", "trace", "fallback", "vg", "dictionary"};
  uint64_t stored[7] = {}, decoded[7] = {};
  uint32_t compressed[7] = {}, count[7] = {};
  for (const auto &page : index.pages) {
    const size_t k = std::min<size_t>(size_t(page.kind), 6u);
    stored[k] += page.storedBytes;
    decoded[k] += page.decodedBytes;
    compressed[k] += page.codec == voxel::svraster::CityVoxelCodec::LZ4;
    ++count[k];
  }
  for (size_t k = 1; k < 7; ++k)
    if (count[k])
      std::printf("%-10s %8u pages (%u LZ4)  stored %8.1f MB  decoded %8.1f MB  ratio %.3f\n", kinds[k], count[k],
                  compressed[k], stored[k] / 1.0e6, decoded[k] / 1.0e6, decoded[k] ? double(stored[k]) / decoded[k] : 1.0);
  return 0;
}
