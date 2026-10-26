#include "editor/virtualgeometry/CityVoxelSurfaceVirtualGeometry.hpp"

#include "editor/virtualgeometry/CityVoxelScenePackager.hpp"
#include "editor/virtualgeometry/VirtualGeometryBuilder.hpp"
#include "editor/virtualgeometry/VirtualGeometryHierarchyBuilder.hpp"
#include "editor/virtualgeometry/VirtualGeometryPacker.hpp"
#include "editor/virtualgeometry/VoxelSurfaceTriangleMesh.hpp"
#include "virtualgeometry/CityVoxelStaticScene.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"

#include "meshoptimizer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <chrono>
#include <thread>
#include <unordered_map>

namespace virtualgeometry
{
namespace
{

bool fail(std::string *error, const std::string &message)
{
  if (error)
    *error = message;
  return false;
}

// The compiled geometry a scene's geometry page holds (as CityVoxelSceneRuntime
// reconstructs it).
bool compiledGeometry(const voxel::svraster::CityVoxelGeometryAsset &record, std::vector<uint8_t> bytes,
                      voxel::svraster::CityVoxelCompiledGeometry &geometry, std::string *error)
{
  if (bytes.size() < 28u)
    return fail(error, "city geometry page is truncated");
  const auto read32 = [&](uint64_t at) { return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 | uint32_t(bytes[at + 2]) << 16 | uint32_t(bytes[at + 3]) << 24; };
  const uint32_t bricks = read32(24u);
  if (28ull + uint64_t(bricks) * 76ull > bytes.size())
    return fail(error, "city geometry page bricks are truncated");
  uint64_t occupied = 0u;
  for (uint32_t brick = 0; brick < bricks; ++brick)
    for (uint32_t word = 0; word < 16u; ++word)
      occupied += uint32_t(__builtin_popcount(read32(28ull + uint64_t(brick) * 76ull + 12ull + word * 4ull)));
  geometry.id = record.id;
  geometry.contentHash = record.contentHash;
  geometry.addressHash = record.voxelAddressHash;
  geometry.pitchMicrometres = record.pitchMicrometres;
  for (unsigned axis = 0; axis < 3; ++axis)
    geometry.extent[axis] = int32_t(int64_t(record.maximumCell[axis]) - record.minimumCell[axis]);
  geometry.occupiedCount = uint32_t(occupied);
  geometry.bytes = std::move(bytes);
  return occupied != 0u || fail(error, "city geometry page has no occupied cells");
}

// Decoded cells of a scene's instances, in each instance's owner frame.
class CitySceneCells
{
public:
  explicit CitySceneCells(const voxel::svraster::CityVoxelSceneFileReader &reader) : reader_(reader), index_(reader.index())
  {
    for (uint32_t g = 0; g < index_.geometry.size(); ++g)
      geometryById_.emplace(index_.geometry[g].id, g);
    for (uint32_t a = 0; a < index_.appearances.size(); ++a)
      appearanceById_.emplace(index_.appearances[a].id, a);
  }

  bool instanceCells(const voxel::svraster::CityVoxelInstance &instance, std::vector<voxel::svraster::CityVoxelCell> &out,
                     std::string *cellError) const
  {
    using namespace voxel::svraster;
    const auto &index = index_;
    const auto &reader = reader_;
    const auto &geometryById = geometryById_;
    const auto &appearanceById = appearanceById_;
    const auto g = geometryById.find(instance.geometryId);
    const auto a = appearanceById.find(instance.appearanceId);
    if (g == geometryById.end() || a == appearanceById.end())
      return fail(cellError, "instance asset is missing");
    const auto &geometryRecord = index.geometry[g->second];
    const auto &appearanceRecord = index.appearances[a->second];
    if (geometryRecord.pageCount != 1u || appearanceRecord.pageCount != 1u || appearanceRecord.baseAppearanceId != 0u)
      return fail(cellError, "instance asset spans pages or has a base appearance");
    std::vector<uint8_t> bytes;
    CityVoxelCompiledGeometry geometry;
    if (!reader.readPage(geometryRecord.firstPage, bytes, cellError) || !compiledGeometry(geometryRecord, std::move(bytes), geometry, cellError))
      return false;
    CityVoxelCompiledAppearance appearance;
    appearance.id = appearanceRecord.id;
    appearance.contentHash = appearanceRecord.contentHash;
    appearance.geometryAddressHash = appearanceRecord.geometryAddressHash;
    appearance.geometryId = appearanceRecord.geometryId;
    appearance.codec = appearanceRecord.codec;
    appearance.occupiedCount = geometry.occupiedCount;
    if (!reader.readPage(appearanceRecord.firstPage, appearance.bytes, cellError))
      return false;
    std::vector<std::array<int32_t, 3>> positions;
    std::vector<CityVoxelCell> values;
    if (!decodeCityVoxelGeometry(geometry, positions, nullptr, cellError) || !decodeCityVoxelAppearance(appearance, values, cellError) ||
        values.size() != positions.size())
      return fail(cellError, "instance cells do not decode");
    const bool localFrame = (instance.flags & kCityVoxelInstanceLocalFrame) != 0u;
    for (size_t i = 0; i < positions.size(); ++i)
    {
      std::array<int64_t, 3> p{positions[i][0], positions[i][1], positions[i][2]};
      if (!localFrame)
      {
        // The cell's centre turned with the instance, as worldPosition turns it.
        const int64_t x2 = 2 * p[0] + 1, z2 = 2 * p[2] + 1;
        int64_t rx2 = x2, rz2 = z2;
        switch (instance.quarterTurn)
        {
        case 1: rx2 = -z2; rz2 = x2; break;
        case 2: rx2 = -x2; rz2 = -z2; break;
        case 3: rx2 = z2; rz2 = -x2; break;
        default: break;
        }
        p[0] = (rx2 - 1) / 2;
        p[2] = (rz2 - 1) / 2;
      }
      CityVoxelCell cell = values[i];
      for (int axis = 0; axis < 3; ++axis)
        cell.position[axis] = int32_t(int64_t(instance.gridPosition[axis]) + p[axis]);
      out.push_back(cell);
    }
    return true;
  }

private:
  const voxel::svraster::CityVoxelSceneFileReader &reader_;
  const voxel::svraster::CityVoxelSceneIndex &index_;
  std::unordered_map<uint64_t, uint32_t> geometryById_, appearanceById_;
};

} // namespace

namespace
{

// 1/80 m: unit_scale counts steps per metre at factor 0 and the decoder
// multiplies by 2^factor, so 10 x 2^3. The lattice (10 cm) is 8 steps.
QuantizationConfig cityQuantization()
{
  QuantizationConfig config;
  config.unit_scale = 10.f;
  config.quantization_factor = 3;
  return config;
}

// The CVGV package for one geometry from its cells: meshed at `meshPitch`,
// optionally simplified as a whole to `presimplifyError` first (a far proxy's
// finest level), built into a DAG, and given lattice UVs in `uvPitch` cells -
// the pitch of the geometry asset the resolve looks colours up in.
bool surfacePackageFromCells(const std::vector<VoxelSurfaceCell> &cells, double meshPitch, double uvPitch, double presimplifyError,
                             uint64_t geometryId, uint64_t contentHash, uint64_t maximumBytes, std::vector<uint8_t> &package,
                             CityVoxelSurfaceVGStats *stats, std::string *error)
{
  VoxelSurfaceTriangleMeshSettings settings;
  settings.pitchMetres = meshPitch;
  VoxelSurfaceTriangleMesh mesh;
  if (!buildVoxelSurfaceTriangleMesh(cells, settings, mesh, error))
    return false;
  const uint64_t rawTriangles = mesh.indices.size() / 3u;
  mergeVoxelSurfaceCoplanar(mesh, settings);
  float finestError = 0.f;
  if (presimplifyError > 0.0)
  {
    std::vector<uint32_t> simplified(mesh.indices.size());
    float achieved = 0.f;
    simplified.resize(meshopt_simplifyWithAttributes(simplified.data(), mesh.indices.data(), mesh.indices.size(), &mesh.vertices[0].pos[0],
                                                     mesh.vertices.size(), sizeof(Vertex), nullptr, 0, nullptr, 0, nullptr, 0,
                                                     float(presimplifyError), meshopt_SimplifyErrorAbsolute, &achieved));
    if (!simplified.empty())
    {
      mesh.indices = std::move(simplified);
      finestError = achieved;
    }
  }
  // Positions are stored on a 12.5 mm grid (`cityQuantization`): the 10 cm
  // lattice is exact on it, and the simplifier snaps what it moves. A
  // presimplified finest level moved vertices too; snap them here and count
  // the half-diagonal of a grid cell in its error.
  const QuantizationConfig quantization = cityQuantization();
  const float grid = 1.f / (float(1u << quantization.quantization_factor) * quantization.unit_scale);
  if (finestError > 0.f)
  {
    for (Vertex &vertex : mesh.vertices)
      for (int a = 0; a < 3; ++a)
        vertex.pos[a] = std::round(vertex.pos[a] / grid) * grid;
    finestError += 0.5f * std::sqrt(3.f) * grid;
  }

  // The resolve takes each pixel's voxel from its position over the colour
  // asset's pitch (`uvPitch`) and the face normal of its triangle, so the
  // mesh stores neither UVs nor normals: no per-view vertex copies, constant
  // attributes that compress away.
  (void)uvPitch;
  std::vector<Vertex> vertices = mesh.vertices;
  for (Vertex &vertex : vertices)
  {
    vertex.norm[0] = 0.f;
    vertex.norm[1] = 1.f;
    vertex.norm[2] = 0.f;
    vertex.uv[0] = vertex.uv[1] = 0.f;
  }
  std::vector<Shape> shapes(1);
  shapes[0].indices = mesh.indices;
  VirtualGeometryBuildSettings buildSettings = voxelSurfaceBuildSettings();
  buildSettings.clusterMaxVertices = 0u;
  // A presimplified finest level is not exact: say so, or the cut draws it
  // as LOD0 at any distance (far proxies sat at 0.25 m error with error 0).
  buildSettings.finestClusterError = finestError;
  buildSettings.positionGrid = grid;
  VirtualGeometryBuildData build = VirtualGeometryBuilder::build(vertices, shapes, buildSettings);
  if (build.clusters.empty() || build.pages.empty())
    return fail(error, "city surface VG build emitted nothing");
  uint32_t largest = 0u;
  for (const auto &cluster : build.clusters)
    largest = std::max(largest, uint32_t(cluster.vertices.size()));

  const VirtualGeometryEncodedData encoded = VirtualGeometryPacker::encode(build, quantization, nullptr, 1u);
  std::vector<uint8_t> blob;
  if (!VirtualGeometryFile::writeToMemory(encoded, build.pages, MESHLET_LZ4, blob, 1u))
    return fail(error, "cannot encode city surface VG");
  uint64_t clusterCount = 0u;
  for (const auto &page : encoded.pages)
    clusterCount += page.meshlets.size();
  const uint64_t packageBytes = 32ull + clusterCount * 20ull + blob.size();
  if (packageBytes > maximumBytes || blob.size() > UINT32_MAX)
    return fail(error, "city surface VG package exceeds the page limit");

  package.clear();
  package.reserve(size_t(packageBytes));
  const auto put = [&](uint64_t value, unsigned width)
  {
    for (unsigned i = 0; i < width; ++i)
      package.push_back(uint8_t(value >> (i * 8)));
  };
  put(0x47565643u, 4);
  put(1u, 4);
  put(geometryId, 8);
  put(contentHash, 8);
  put(clusterCount, 4);
  put(blob.size(), 4);
  for (uint64_t c = 0; c < clusterCount; ++c)
  {
    put(0u, 4);
    put(0u, 4);
    put(0u, 4);
    put(kCityVoxelLatticeClusterScale, 4);
    put(0u, 4); // Level.
  }
  package.insert(package.end(), blob.begin(), blob.end());
  if (stats)
  {
    stats->rawTriangles = rawTriangles;
    stats->lod0Triangles = mesh.indices.size() / 3u;
    stats->clusters = clusterCount;
    stats->maxClusterVertices = largest;
    stats->packageBytes = package.size();
  }
  return true;
}

} // namespace

bool buildCityVoxelSurfaceVGPackage(const voxel::svraster::CityVoxelCompiledGeometry &geometry, uint64_t maximumBytes,
                                    std::vector<uint8_t> &package, CityVoxelSurfaceVGStats *stats, std::string *error)
{
  std::vector<std::array<int32_t, 3>> positions;
  if (!voxel::svraster::decodeCityVoxelGeometry(geometry, positions, nullptr, error))
    return false;
  std::vector<VoxelSurfaceCell> cells(positions.size());
  for (size_t i = 0; i < positions.size(); ++i)
    cells[i].position = positions[i];
  const double pitch = double(geometry.pitchMicrometres) * 1e-6;
  return surfacePackageFromCells(cells, pitch, pitch, 0.0, geometry.id, geometry.contentHash, maximumBytes, package, stats, error);
}

bool rebuildCityVoxelSceneSurfaceVG(const std::string &inputPath, const std::string &outputPath,
                                    const voxel::svraster::CityVoxelSceneLimits &limits, uint32_t threads,
                                    CityVoxelSurfaceRebuildStats *stats, std::string *error)
{
  using namespace voxel::svraster;
  if (inputPath == outputPath)
    return fail(error, "rebuild needs a different output path");
  CityVoxelSceneFileReader reader;
  if (!reader.open(inputPath, limits, error))
    return false;
  CityVoxelSceneIndex index = reader.index();
  // The builder prints every level of every DAG; over a city that was 300 MB
  // of output and most of an 88-minute rebuild.
  setVirtualGeometryBuildLogging(false);

  // Pages come back decoded, so every page is written raw and the writer
  // compresses again with fresh dictionaries; the old dictionaries go.
  while (!index.pages.empty() && index.pages.back().kind == CityVoxelPageKind::Dictionary)
    index.pages.pop_back();
  for (const auto &page : index.pages)
    if (page.kind == CityVoxelPageKind::Dictionary)
      return fail(error, "dictionary pages are not at the end of the scene");
  for (auto &page : index.pages)
  {
    page.fileOffset = 0u;
    page.storedBytes = 0u;
    page.checksum = 0u;
    page.codec = CityVoxelCodec::Raw;
    page.decodedBytes = 0u;
  }

  // The writer takes `index` by value; the producer reads this copy.
  const CityVoxelSceneIndex source = index;
  std::unordered_map<uint64_t, uint32_t> geometryById;
  for (uint32_t g = 0; g < index.geometry.size(); ++g)
    geometryById.emplace(index.geometry[g].id, g);
  std::vector<uint32_t> vgPages;
  for (const auto &page : index.pages)
    if (page.kind == CityVoxelPageKind::VirtualGeometry)
      vgPages.push_back(page.id);

  CityVoxelSurfaceRebuildStats local;
  std::atomic<uint64_t> rawTriangles{0u}, lod0Triangles{0u}, clusters{0u}, newBytes{0u}, oldBytes{0u};
  std::atomic<uint32_t> failed{0u}, built{0u};
  std::mutex reasonsMutex;
  std::map<std::string, uint32_t> reasons;
  threads = std::max(1u, threads);
  const auto start = std::chrono::steady_clock::now();

  // VG pages in batches built on every thread, ahead of the writer.
  constexpr size_t kBatch = 512u;
  std::unordered_map<uint32_t, std::vector<uint8_t>> ready;
  size_t nextBatch = 0u;
  const auto buildBatch = [&](size_t first)
  {
    const size_t count = std::min(kBatch, vgPages.size() - first);
    std::vector<std::vector<uint8_t>> results(count);
    std::atomic<size_t> next{0u};
    std::vector<std::thread> workers;
    for (uint32_t t = 0; t < threads; ++t)
      workers.emplace_back([&]
                           {
                             for (size_t i; (i = next.fetch_add(1)) < count;)
                             {
                               const uint32_t pageId = vgPages[first + i];
                               std::vector<uint8_t> old;
                               std::string pageError;
                               if (!reader.readPage(pageId, old, &pageError))
                                 continue; // Left empty: the producer reports it.
                               oldBytes += old.size();
                               const auto found = geometryById.find(source.pages[pageId].assetId);
                               std::vector<uint8_t> geometryBytes;
                               CityVoxelCompiledGeometry geometry;
                               CityVoxelSurfaceVGStats one;
                               if (found != geometryById.end() && source.geometry[found->second].pageCount == 1u &&
                                   reader.readPage(source.geometry[found->second].firstPage, geometryBytes, &pageError) &&
                                   compiledGeometry(source.geometry[found->second], std::move(geometryBytes), geometry, &pageError) &&
                                   buildCityVoxelSurfaceVGPackage(geometry, limits.maximumStoredPageBytes, results[i], &one, &pageError))
                               {
                                 rawTriangles += one.rawTriangles;
                                 lod0Triangles += one.lod0Triangles;
                                 clusters += one.clusters;
                                 ++built;
                               }
                               else
                               {
                                 results[i] = std::move(old);
                                 ++failed;
                                 if (found == geometryById.end())
                                   pageError = "no geometry record";
                                 else if (source.geometry[found->second].pageCount != 1u)
                                   pageError = "geometry spans " + std::to_string(source.geometry[found->second].pageCount) + " pages";
                                 const std::lock_guard<std::mutex> lock(reasonsMutex);
                                 ++reasons[pageError];
                               }
                               newBytes += results[i].size();
                             }
                           });
    for (auto &worker : workers)
      worker.join();
    for (size_t i = 0; i < count; ++i)
      ready.emplace(vgPages[first + i], std::move(results[i]));
  };

  const CityVoxelPageProducer producer = [&](uint32_t pageId, std::vector<uint8_t> &payload, std::string *pageError)
  {
    if (source.pages[pageId].kind != CityVoxelPageKind::VirtualGeometry)
      return reader.readPage(pageId, payload, pageError);
    auto it = ready.find(pageId);
    while (it == ready.end() && nextBatch < vgPages.size())
    {
      buildBatch(nextBatch);
      nextBatch += kBatch;
      it = ready.find(pageId);
    }
    if (it == ready.end() || it->second.empty())
      return fail(pageError, "city VG page could not be read");
    payload = std::move(it->second);
    ready.erase(it);
    return true;
  };

  CityVoxelSceneCompression compression;
  compression.enabled = true;
  compression.workers = threads;
  const bool ok = writeCityVoxelSceneFile(outputPath, std::move(index), limits, producer, error, compression);
  local.geometries = built.load();
  local.failed = failed.load();
  local.rawTriangles = rawTriangles.load();
  local.lod0Triangles = lod0Triangles.load();
  local.clusters = clusters.load();
  local.oldVGBytes = oldBytes.load();
  local.newVGBytes = newBytes.load();
  local.buildSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  local.failureReasons.assign(reasons.begin(), reasons.end());
  std::sort(local.failureReasons.begin(), local.failureReasons.end(), [](const auto &l, const auto &r) { return l.second > r.second; });
  if (stats)
    *stats = local;
  return ok;
}

bool buildCityVoxelHouseScene(const std::string &inputPath, const std::string &outputPath,
                              const voxel::svraster::CityVoxelSceneLimits &limits, uint32_t threads,
                              CityVoxelHouseSceneStats *stats, std::string *error)
{
  using namespace voxel::svraster;
  const auto start = std::chrono::steady_clock::now();
  if (inputPath == outputPath)
    return fail(error, "house scene needs a different output path");
  CityVoxelSceneFileReader reader;
  if (!reader.open(inputPath, limits, error))
    return false;
  const CityVoxelSceneIndex &index = reader.index();
  if (index.cityOriginMillimetres != std::array<int64_t, 3>{})
    return fail(error, "house scene: a non-zero city origin is not carried by the scene writer");
  CityVoxelStaticScene source;
  if (!source.loadSceneIndex(index, error))
    return false;
  setVirtualGeometryBuildLogging(false);

  CitySceneCells sceneCells(reader);
  const auto instanceCells = [&](const CityVoxelInstance &instance, std::vector<CityVoxelCell> &out, std::string *cellError)
  { return sceneCells.instanceCells(instance, out, cellError); };

  CityVoxelCompilerLimits compilerLimits;
  compilerLimits.maximumPartCells = 8'000'000u;
  compilerLimits.maximumBricks = 1'000'000u;
  compilerLimits.maximumScratchBytes = 2048ull * 1024ull * 1024ull;
  CityVoxelAssetCompiler compiler(compilerLimits);

  struct NewPlacement
  {
    uint64_t stableId, ownerId, geometryId, appearanceId;
    uint32_t building;
    std::array<int32_t, 3> origin;
  };
  std::vector<NewPlacement> newPlacements;
  std::vector<CityVoxelStaticCell> cells;
  std::vector<CityVoxelStaticBuilding> buildings(4u);
  for (uint32_t turn = 0; turn < 4u; ++turn)
    buildings[turn].quarterTurns = turn;
  CityVoxelHouseSceneStats local;
  local.sourcePlacements = uint32_t(index.instances.size());

  // Houses in scene order: per spatial cell, its owners in first-seen order.
  struct House
  {
    uint32_t cellIndex;
    uint64_t owner;
    std::vector<uint32_t> members;
    uint32_t building;
  };
  std::vector<House> houses;
  std::vector<std::pair<uint32_t, uint32_t>> housesOfCell(index.spatialCells.size(), {0u, 0u}); // first, count
  for (uint32_t cellIndex = 0; cellIndex < index.spatialCells.size(); ++cellIndex)
  {
    const auto &spatial = index.spatialCells[cellIndex];
    housesOfCell[cellIndex].first = uint32_t(houses.size());
    if (!spatial.instanceCount)
      continue;
    const auto &first = index.instances[spatial.firstInstance];
    uint32_t building = 0u;
    if ((first.flags & kCityVoxelInstanceLocalFrame) != 0u)
    {
      CityVoxelStaticBuilding frame;
      frame.localFrame = true;
      frame.originMillimetres = spatial.originMillimetres;
      frame.yawCode = first.frameId;
      building = uint32_t(buildings.size());
      buildings.push_back(frame);
    }
    std::unordered_map<uint64_t, uint32_t> houseOfOwner;
    for (uint32_t i = spatial.firstInstance; i < spatial.firstInstance + spatial.instanceCount; ++i)
    {
      const uint64_t owner = index.instances[i].ownerId;
      auto [it, inserted] = houseOfOwner.emplace(owner, uint32_t(houses.size()));
      if (inserted)
        houses.push_back({cellIndex, owner, {}, building});
      houses[it->second].members.push_back(i);
    }
    housesOfCell[cellIndex].second = uint32_t(houses.size()) - housesOfCell[cellIndex].first;
  }

  // Gather and merge a batch of houses on every thread (reading and decoding
  // is most of the work), then submit them to the compiler in scene order.
  threads = std::max(1u, threads);
  std::vector<std::pair<uint32_t, uint32_t>> placementsOfHouse(houses.size(), {0u, 0u});
  constexpr size_t kBatch = 256u;
  for (size_t batchFirst = 0; batchFirst < houses.size(); batchFirst += kBatch)
  {
    const size_t count = std::min(kBatch, houses.size() - batchFirst);
    std::vector<std::vector<CityVoxelCell>> merged(count);
    std::vector<std::string> gatherErrors(count);
    std::atomic<size_t> next{0u};
    std::vector<std::thread> workers;
    for (uint32_t t = 0; t < threads; ++t)
      workers.emplace_back([&]
                           {
                             for (size_t i; (i = next.fetch_add(1)) < count;)
                             {
                               auto &cells = merged[i];
                               for (const uint32_t member : houses[batchFirst + i].members)
                                 if (!instanceCells(index.instances[member], cells, &gatherErrors[i]))
                                   break;
                               // Dedupe positions, keeping the first value.
                               std::stable_sort(cells.begin(), cells.end(), [](const CityVoxelCell &l, const CityVoxelCell &r) { return l.position < r.position; });
                               cells.erase(std::unique(cells.begin(), cells.end(), [](const CityVoxelCell &l, const CityVoxelCell &r) { return l.position == r.position; }),
                                           cells.end());
                             }
                           });
    for (auto &worker : workers)
      worker.join();
    for (size_t i = 0; i < count; ++i)
    {
      const House &house = houses[batchFirst + i];
      if (!gatherErrors[i].empty())
        return fail(error, "house scene: " + gatherErrors[i]);
      ++local.houses;
      local.cells += merged[i].size();
      placementsOfHouse[batchFirst + i].first = uint32_t(newPlacements.size());
      const uint64_t stableId = (house.owner * 0x9E3779B97F4A7C15ull) ^ (uint64_t(house.cellIndex) + 1u);
      CityVoxelCompiledPlacement compiled;
      std::string submitError;
      if (compiler.submit(stableId ? stableId : 1u, index.nominalPitchMicrometres, merged[i], compiled, &submitError))
      {
        ++local.mergedHouses;
        newPlacements.push_back({stableId ? stableId : 1u, house.owner, compiled.geometryId, compiled.appearanceId, house.building,
                                 compiled.originCell});
      }
      else
      {
        // Too big for one asset: its blocks, each submitted as it was placed.
        ++local.splitHouses;
        for (const uint32_t member : house.members)
        {
          std::vector<CityVoxelCell> part;
          std::string houseError;
          if (!instanceCells(index.instances[member], part, &houseError))
            return fail(error, "house scene: " + houseError);
          CityVoxelCompiledPlacement partPlacement;
          if (!compiler.submit(index.instances[member].stableId, index.nominalPitchMicrometres, part, partPlacement, &houseError))
            return fail(error, "house scene: a block does not compile: " + houseError);
          newPlacements.push_back({index.instances[member].stableId, house.owner, partPlacement.geometryId, partPlacement.appearanceId,
                                   house.building, partPlacement.originCell});
        }
      }
      placementsOfHouse[batchFirst + i].second = uint32_t(newPlacements.size()) - placementsOfHouse[batchFirst + i].first;
      merged[i] = {};
    }
  }
  for (uint32_t cellIndex = 0; cellIndex < index.spatialCells.size(); ++cellIndex)
  {
    CityVoxelStaticCell cell = source.cells()[cellIndex];
    const auto [firstHouse, houseCount] = housesOfCell[cellIndex];
    cell.firstPlacement = houseCount ? placementsOfHouse[firstHouse].first : uint32_t(newPlacements.size());
    cell.placementCount = 0u;
    for (uint32_t h = firstHouse; h < firstHouse + houseCount; ++h)
      cell.placementCount += placementsOfHouse[h].second;
    cells.push_back(cell);
  }

  // Tables in the compiler's order, placements pointing into them.
  std::vector<CityVoxelStaticModel> models;
  std::unordered_map<uint64_t, uint32_t> modelIndex, appearanceIndex;
  for (const auto &geometry : compiler.geometry())
  {
    modelIndex.emplace(geometry.id, uint32_t(models.size()));
    models.push_back({geometry.id, "city-house-" + std::to_string(geometry.id), 0u});
  }
  std::vector<uint64_t> appearanceIds;
  for (const auto &appearance : compiler.appearances())
  {
    appearanceIndex.emplace(appearance.id, uint32_t(appearanceIds.size()));
    appearanceIds.push_back(appearance.id);
  }
  std::vector<CityVoxelStaticPlacement> placements;
  std::unordered_map<uint64_t, uint64_t> ownerByPlacement;
  for (const auto &p : newPlacements)
  {
    CityVoxelStaticPlacement placement;
    placement.stableId = p.stableId;
    placement.geometryIndex = modelIndex.at(p.geometryId);
    placement.appearanceIndex = appearanceIndex.at(p.appearanceId);
    placement.buildingIndex = p.building;
    placement.localOriginCell = p.origin;
    placements.push_back(placement);
    if (!ownerByPlacement.emplace(p.stableId, p.ownerId).second)
      return fail(error, "house scene: placement ids collide");
  }
  local.placements = uint32_t(placements.size());
  local.geometries = uint32_t(models.size());

  CityVoxelStaticScene city;
  if (!city.load(std::move(models), std::move(appearanceIds), std::move(buildings), std::move(cells), std::move(placements),
                 index.nominalPitchMicrometres, error))
    return false;
  CityVoxelScenePackOptions options;
  options.tracePages = (index.requiredFeatures & kCityVoxelFeatureTracePages) != 0u;
  options.workers = std::max(1u, threads);
  options.surfaceVirtualGeometry = true;
  const bool ok = writeCityVoxelCompiledScene(outputPath, city, compiler.geometry(), compiler.appearances(), index.materials,
                                              index.sourceHash, index.settingsHash ^ 0x5355524641434521ull, limits, error,
                                              &ownerByPlacement, options);
  local.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  if (stats)
    *stats = local;
  return ok;
}

bool rebuildCityVoxelFarSceneFromDetail(const std::string &detailPath, const std::string &farPath, const std::string &outputPath,
                                        const voxel::svraster::CityVoxelSceneLimits &limits, uint32_t threads, double farErrorMetres,
                                        CityVoxelFarSceneStats *stats, std::string *error)
{
  using namespace voxel::svraster;
  const auto start = std::chrono::steady_clock::now();
  if (farPath == outputPath || detailPath == outputPath)
    return fail(error, "far rebuild needs a different output path");
  CityVoxelSceneFileReader detail, far;
  if (!detail.open(detailPath, limits, error) || !far.open(farPath, limits, error))
    return false;
  setVirtualGeometryBuildLogging(false);
  const CitySceneCells detailCells(detail);
  const auto &detailIndex = detail.index();
  std::unordered_map<uint64_t, std::vector<uint32_t>> detailByOwner;
  for (uint32_t i = 0; i < detailIndex.instances.size(); ++i)
    detailByOwner[detailIndex.instances[i].ownerId].push_back(i);

  CityVoxelSceneIndex index = far.index();
  const CityVoxelSceneIndex source = index;
  const uint32_t finePitch = detailIndex.nominalPitchMicrometres, coarsePitch = source.nominalPitchMicrometres;
  if (!finePitch || coarsePitch % finePitch != 0u)
    return fail(error, "far rebuild needs a coarse pitch that is a whole multiple of the detailed pitch");
  const int32_t factor = int32_t(coarsePitch / finePitch);

  while (!index.pages.empty() && index.pages.back().kind == CityVoxelPageKind::Dictionary)
    index.pages.pop_back();
  for (auto &page : index.pages)
  {
    page.fileOffset = 0u;
    page.storedBytes = 0u;
    page.checksum = 0u;
    page.codec = CityVoxelCodec::Raw;
    page.decodedBytes = 0u;
  }
  std::unordered_map<uint64_t, uint32_t> firstInstance; // far geometry id -> far instance
  for (uint32_t i = 0; i < source.instances.size(); ++i)
    firstInstance.emplace(source.instances[i].geometryId, i);
  std::unordered_map<uint64_t, uint64_t> contentHash;
  for (const auto &g : source.geometry)
    contentHash.emplace(g.id, g.contentHash);
  std::vector<uint32_t> vgPages;
  for (const auto &page : index.pages)
    if (page.kind == CityVoxelPageKind::VirtualGeometry)
      vgPages.push_back(page.id);

  std::atomic<uint32_t> rebuilt{0u}, kept{0u};
  std::atomic<uint64_t> triangles{0u}, oldBytes{0u}, newBytes{0u};
  std::mutex reasonsMutex;
  std::map<std::string, uint32_t> reasons;
  threads = std::max(1u, threads);
  constexpr size_t kBatch = 256u;
  std::unordered_map<uint32_t, std::vector<uint8_t>> ready;
  size_t nextBatch = 0u;
  const auto buildOne = [&](uint32_t pageId, std::vector<uint8_t> &out, std::string *why)
  {
    const uint64_t geometryId = source.pages[pageId].assetId;
    const auto first = firstInstance.find(geometryId);
    if (first == firstInstance.end())
      return fail(why, "far geometry has no instance");
    const auto &instance = source.instances[first->second];
    if (!(instance.flags & kCityVoxelInstanceLocalFrame))
      return fail(why, "far instance is not in an owner frame");
    const auto owner = detailByOwner.find(instance.ownerId);
    if (owner == detailByOwner.end())
      return fail(why, "owner is not in the detailed scene");
    std::vector<CityVoxelCell> fine;
    for (const uint32_t i : owner->second)
    {
      // Same owner origin; the far frame may be turned by whole quarter
      // turns (it is: 16384 yaw units less, measured on every house).
      if (!(detailIndex.instances[i].flags & kCityVoxelInstanceLocalFrame) || (uint16_t(detailIndex.instances[i].frameId - instance.frameId) & 0x3fffu) != 0u)
        return fail(why, "detailed owner frame differs from the far one by more than quarter turns");
      if (!detailCells.instanceCells(detailIndex.instances[i], fine, why))
        return false;
    }
    // Into the far asset's own frame, in fine cells: turned by the frames'
    // yaw difference (whole quarter turns, cell for cell: a turn by +90 deg
    // takes the cell at (x, z) to (-z - 1, x), as worldPosition turns its
    // centre), then offset by the far placement.
    const uint32_t turns = (uint16_t(detailIndex.instances[owner->second.front()].frameId - instance.frameId) >> 14) & 3u;
    std::vector<VoxelSurfaceCell> cells;
    cells.reserve(fine.size());
    for (const auto &cell : fine)
    {
      int32_t x = cell.position[0], z = cell.position[2];
      for (uint32_t t = 0; t < turns; ++t)
      {
        const int32_t nx = -z - 1, nz = x;
        x = nx;
        z = nz;
      }
      VoxelSurfaceCell local;
      local.position = {x - factor * instance.gridPosition[0], cell.position[1] - factor * instance.gridPosition[1],
                        z - factor * instance.gridPosition[2]};
      cells.push_back(local);
    }
    CityVoxelSurfaceVGStats one;
    if (!surfacePackageFromCells(cells, double(finePitch) * 1e-6, double(coarsePitch) * 1e-6, farErrorMetres, geometryId,
                                 contentHash[geometryId], limits.maximumStoredPageBytes, out, &one, why))
      return false;
    triangles += one.lod0Triangles;
    return true;
  };
  const auto buildBatch = [&](size_t firstPage)
  {
    const size_t count = std::min(kBatch, vgPages.size() - firstPage);
    std::vector<std::vector<uint8_t>> results(count);
    std::atomic<size_t> next{0u};
    std::vector<std::thread> workers;
    for (uint32_t t = 0; t < threads; ++t)
      workers.emplace_back([&]
                           {
                             for (size_t i; (i = next.fetch_add(1)) < count;)
                             {
                               const uint32_t pageId = vgPages[firstPage + i];
                               std::vector<uint8_t> old;
                               std::string why;
                               if (!far.readPage(pageId, old, &why))
                                 continue;
                               oldBytes += old.size();
                               if (buildOne(pageId, results[i], &why))
                                 ++rebuilt;
                               else
                               {
                                 results[i] = std::move(old);
                                 ++kept;
                                 const std::lock_guard<std::mutex> lock(reasonsMutex);
                                 ++reasons[why];
                               }
                               newBytes += results[i].size();
                             }
                           });
    for (auto &worker : workers)
      worker.join();
    for (size_t i = 0; i < count; ++i)
      ready.emplace(vgPages[firstPage + i], std::move(results[i]));
  };
  const CityVoxelPageProducer producer = [&](uint32_t pageId, std::vector<uint8_t> &payload, std::string *pageError)
  {
    if (source.pages[pageId].kind != CityVoxelPageKind::VirtualGeometry)
      return far.readPage(pageId, payload, pageError);
    auto it = ready.find(pageId);
    while (it == ready.end() && nextBatch < vgPages.size())
    {
      buildBatch(nextBatch);
      nextBatch += kBatch;
      it = ready.find(pageId);
    }
    if (it == ready.end() || it->second.empty())
      return fail(pageError, "far VG page could not be read");
    payload = std::move(it->second);
    ready.erase(it);
    return true;
  };
  CityVoxelSceneCompression compression;
  compression.enabled = true;
  compression.workers = threads;
  const bool ok = writeCityVoxelSceneFile(outputPath, std::move(index), limits, producer, error, compression);
  if (stats)
  {
    stats->rebuilt = rebuilt.load();
    stats->kept = kept.load();
    stats->triangles = triangles.load();
    stats->oldVGBytes = oldBytes.load();
    stats->newVGBytes = newBytes.load();
    stats->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    stats->keptReasons.assign(reasons.begin(), reasons.end());
  }
  return ok;
}

} // namespace virtualgeometry
