#include <mutex>
#include <future>
// Low-poly terrain generator.
//
// Generates a world with the shared map pipeline, then turns it into an
// explicit faceted triangle mesh whose edges are the map's own semantic lines:
// the coast, the elevation contours, and the region borders. The look is
// N64-era on purpose - flat shaded, terraced, hard silhouettes - because that
// is what a constrained triangulation is naturally good at. It spends triangles
// on lines you can see and almost none on open ground.
//
// Generation itself lives in procedural::world::generateWorld so this sample
// and the world generator cannot drift into producing different worlds from
// the same seed.

#include "LowPolyTerrainView.hpp"

#include "os/Logger.hpp"
#include "procedural/PolygonTerrain.hpp"
#include "procedural/geometry/TerrainMaterial.hpp"
#include "procedural/geometry/TerrainRasterSource.hpp"
#include "procedural/geometry/TerrainMapSource.hpp"
#include "procedural/geometry/TerrainMeshBuild.hpp"
#include "procedural/world/WorldPipeline.hpp"
#include "procedural/world/WorldRuntimeArchive.hpp"
#include "procedural/world/WorldRuntimeDisplay.hpp"
#include "procedural/world/WorldRuntimeCollision.hpp"
#include "procedural/world/WorldRuntimeCover.hpp"
#include "window/sdl3/SDL3Window.hpp"

#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <atomic>
#include "json.hpp"
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace procedural::world;
using namespace procedural::geometry;
using procedural::Point2;
using procedural::PolygonTerrainGenerator;
using procedural::PolygonTerrainMaps;
using procedural::PolygonTerrainRasterSettings;
using procedural::PolygonTerrainSettings;
using procedural::PolygonTerrainStatistics;

double secondsSince(const std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
      .count();
}

struct Options
{
  std::string rewriteArchive, rewriteReloadArchive;
  uint32_t rewriteFrames = 0;
  uint64_t seed = 7ull;
  double extentKilometres = 128.0;
  double reliefMetres = 6000.0;
  double cellSpacingKilometres = 0.7;
  uint32_t hierarchyLevels = 2u;
  double tileSizeMetres = 4000.0;
  double toleranceMetres = 4.0;
  uint32_t refinementRounds = 7u;
  double chainSpacingMetres = 0.0;
  double borderSpacingMetres = 0.0;
  double terraceStrength = 0.0;
  double minimumAngleDegrees = 0.0;
  // The blocks used to read as terraces because the ground under them was the
  // analytic map eroded on a 250 m grid. It is a 125 m grid now and the
  // formations dissect, so the mesas have relief to sit on.
  bool plateaus = true;
  bool carveRivers = true;
  bool stylize = false;
  bool erode = true;
  uint32_t erodeIterations = 60u;
  double erodeFlatten = 0.0;
  bool beautify = false;
  double stylizeGain = 0.55;
  double plateauCoverage = 0.16;
  uint32_t plateauRungs = 3u;
  double terraceIntervalMetres = 0.0;
  double contourIntervalMetres = 0.0;
  double poissonSpacingMetres = 0.0;
  uint32_t poissonLayers = 3u;
  bool relocateSamples = false;
  uint32_t workers = 0u;
  std::function<void(const std::string &, const TerrainMeshProgress &)> progress;
  std::string plyPath;
  // Default: consume the same world-map export as the voxel viewer. Legacy
  // "raster" regenerates a polygon heightmap; "map" generates an analytic graph.
  bool rasterSource = true;
  bool exportSource = true;
  std::string worldMapPath = LOW_POLY_TERRAIN_DEFAULT_MAP_PATH;
  uint32_t rasterResolution = 2048u;
  double rasterSiteRadiusMetres = 0.0;
  bool headless = false;
  std::string fieldPath;
  double worldScale = 2.0;
  // The CFL limit scales with cell size, so the 2048 grid needs about three
  // times the steps 1024 did to cover the same model time. At 1800 it eroded
  // for 28 kyr instead of 42 kyr and the landforms came out under-dissected.
  uint32_t upliftSteps = 5400;
  // The stream-power grid is what dissects the landforms, and a rock face is
  // 80 to 300 m across. At 1024 over a 256 km map a cell is 250 m, so erosion
  // could not cut into a formation at all and they read as pasted on. The
  // solver is banded across cores now, so the finer grid costs wall clock it
  // did not used to have to spend.
  uint32_t erosionResolution = 2048;
  // Land allocation: settlements, roads, and the parcel layer.
  bool allocateLand = true;
  // Zero means "whatever the parcel layer's own default is". Hardcoding a
  // number here silently overrode every library default, so retuning the field
  // scale in ParcelSettings did nothing at all through this sample.
  double parcelMetres = 0.0;
  // Opacity written into the river surface mesh, 0-255.
  uint8_t riverAlpha = 165u;
  bool variedBiomes = true;
  // Off: the spline rock formations raise a plateau and paste an analytic face
  // profile onto the finished surface, after the erosion solver has run, so
  // nothing can erode them. Contour scarps replace them - they cut the existing
  // ground before the solver, so drainage and landslide act on them. Pass
  // --rock-formations 1 to get the old generator back.
  bool rockFormations = false;
  bool scarps = true;
  uint32_t landformCount = 32;
  uint32_t cliffLevels = 3;
  bool guidedGeometry = true, simplifyGeometry = true;
  std::string landformSplines;
  std::string tileDirectory;
  uint64_t triangleBudget = 300000000;
  bool productionQuality = false;
};

// Flat-shaded palette. Deliberately readable rather than photographic: this is
// a stylized mesh and the colour is the only material channel it has until the
// substrate and cover maps arrive.
std::array<float, 3> colourOf(const CellType type)
{
  switch (type)
  {
  case CellType::Ocean: return {0.09f, 0.20f, 0.38f};
  case CellType::Lake: return {0.16f, 0.36f, 0.55f};
  case CellType::Coast: return {0.76f, 0.70f, 0.50f};
  case CellType::Plain: return {0.42f, 0.55f, 0.28f};
  case CellType::Hill: return {0.36f, 0.47f, 0.24f};
  case CellType::Mountain: return {0.48f, 0.46f, 0.44f};
  case CellType::Plateau: return {0.52f, 0.48f, 0.36f};
  case CellType::Valley: return {0.34f, 0.50f, 0.30f};
  case CellType::Wood: return {0.20f, 0.36f, 0.19f};
  case CellType::Swamp: return {0.28f, 0.36f, 0.25f};
  case CellType::Desert: return {0.78f, 0.69f, 0.44f};
  case CellType::Rocks: return {0.44f, 0.42f, 0.40f};
  case CellType::Cliff: return {0.55f, 0.48f, 0.42f};
  case CellType::Canyon: return {0.60f, 0.42f, 0.32f};
  default: return {0.5f, 0.5f, 0.5f};
  }
}

WorldPipelineSettings pipelineSettings(const Options &options)
{
  WorldPipelineSettings settings{};
  settings.brief.seed = options.seed;
  settings.brief.extentMetres = options.extentKilometres * 1000.0;
  settings.brief.seaLevelMetres = 0.0;
  settings.brief.reliefMetres = options.reliefMetres;
  settings.brief.tags = {Tag::Coast};
  settings.cellSpacingMetres = options.cellSpacingKilometres * 1000.0;
  settings.hierarchyLevels = options.hierarchyLevels;
  settings.grownLandmasses = true;
  settings.landFraction = 0.75;
  settings.continentCount = 1u;
  settings.continentSpanMetres = options.extentKilometres * 1000.0 * 0.8;
  settings.landmassCores = 6u;
  settings.cratonSpread = 0.55;
  settings.fluvialErosion = options.erode;
  settings.fluvial.iterations = options.erodeIterations;
  settings.fluvial.flattenBeforeErosion = options.erodeFlatten;
  settings.stylizeRanges = options.stylize;
  settings.stylize.gain = options.stylizeGain;
  settings.plateaus = options.plateaus;
  settings.plateau.coverageFraction = options.plateauCoverage;
  settings.plateau.maximumRungs = options.plateauRungs;
  settings.plateau.minimumRungs = std::min(1u, options.plateauRungs);
  return settings;
}

std::function<void(const TerrainMeshProgress &)> meshProgress(const std::string &name, const std::function<void(const std::string &, const TerrainMeshProgress &)> &observer = {})
{
  return [name,observer](const TerrainMeshProgress &p)
  {
    if(observer)observer(name,p);
    std::ostringstream out;
    out<<name<<" / "<<p.stage<<"  "<<p.completedTiles<<"/"<<p.totalTiles<<" tiles ("
       <<std::fixed<<std::setprecision(1)<<(p.totalTiles?100.0*p.completedTiles/p.totalTiles:0)<<"%), "
       <<p.activeWorkers<<"/"<<p.workerCount<<" workers, "<<p.triangles<<" triangles, "<<p.elapsedSeconds<<"s";
    if(p.completedTiles&&p.completedTiles<p.totalTiles)
      out<<", ETA ~"<<p.elapsedSeconds*(p.totalTiles-p.completedTiles)/p.completedTiles<<"s";
    std::cout<<out.str()<<std::endl;
  };
}

bool generateWithProgress(const Options &options,
    const std::function<bool(const Options &)> &generate,std::string &error)
{
  // SDL stays on the main thread; generation and its tile pool run in the background.
  if(!SDL_InitSubSystem(SDL_INIT_VIDEO))return generate(options);
  SDL_Window *window=SDL_CreateWindow("Generating terrain",880,200,0);
  SDL_Renderer *renderer=window?SDL_CreateRenderer(window,nullptr):nullptr;
  if(!renderer){if(window)SDL_DestroyWindow(window);return generate(options);}
  std::mutex mutex;std::atomic<bool> cancel{false};
  TerrainMeshProgress current;std::string phase="Loading planet maps";
  Options observed=options;
  observed.progress=[&](const std::string &name,const TerrainMeshProgress &p)
  {
    if(cancel)throw std::runtime_error("terrain generation cancelled");
    if(options.progress)options.progress(name,p);
    std::lock_guard<std::mutex> lock(mutex);current=p;phase=name+" / "+p.stage;
  };
  const auto start=std::chrono::steady_clock::now();
  auto work=std::async(std::launch::async,[&]{return generate(observed);});
  while(work.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)
  {
    SDL_Event event;while(SDL_PollEvent(&event))if(event.type==SDL_EVENT_QUIT||event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED)cancel=true;
    TerrainMeshProgress p;std::string label;
    {std::lock_guard<std::mutex> lock(mutex);p=current;label=phase;}
    if(cancel)label="Cancelling generation...";
    SDL_SetRenderDrawColor(renderer,24,30,37,255);SDL_RenderClear(renderer);
    SDL_SetRenderDrawColor(renderer,233,239,243,255);SDL_RenderDebugText(renderer,24,24,label.c_str());
    const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::ostringstream detail;detail<<p.completedTiles<<" / "<<p.totalTiles<<" tiles    "<<p.activeWorkers<<" / "<<p.workerCount<<" workers    "<<p.triangles<<" triangles";
    if(p.totalTiles)SDL_RenderDebugText(renderer,24,54,detail.str().c_str());
    SDL_FRect track{24,88,832,18};SDL_SetRenderDrawColor(renderer,50,61,72,255);SDL_RenderFillRect(renderer,&track);
    SDL_FRect fill=track;
    if(p.totalTiles)fill.w*=float(p.completedTiles)/p.totalTiles;
    else{fill.w=100;fill.x+=float(std::fmod(elapsed*.25,1.0))*732;}
    SDL_SetRenderDrawColor(renderer,93,181,158,255);SDL_RenderFillRect(renderer,&fill);
    std::ostringstream time;time<<std::fixed<<std::setprecision(0)<<elapsed<<"s elapsed";
    if(p.completedTiles&&p.completedTiles<p.totalTiles)time<<"    ETA ~"<<p.elapsedSeconds*(p.totalTiles-p.completedTiles)/p.completedTiles<<"s";
    SDL_SetRenderDrawColor(renderer,189,203,213,255);SDL_RenderDebugText(renderer,24,132,time.str().c_str());
    SDL_RenderPresent(renderer);SDL_Delay(16);
  }
  bool okay=false;try{okay=work.get()&&!cancel;}catch(const std::exception &e){error=e.what();}
  SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);
  if(cancel&&error.empty())error="terrain generation cancelled";
  return okay;
}

TerrainMeshSettings meshSettings(
    const Options &options, const GeneratedWorld &world)
{
  TerrainMeshSettings settings{};
  settings.tileSizeMetres = options.tileSizeMetres;
  settings.verticalToleranceMetres = options.toleranceMetres;
  settings.maximumRefinementRounds = options.refinementRounds;
  settings.minimumEdgeMetres = std::max(2.0, options.tileSizeMetres / 400.0);
  settings.chainSpacingMetres = options.chainSpacingMetres > 0.0
      ? options.chainSpacingMetres
      : options.tileSizeMetres / 16.0;
  settings.borderSpacingMetres = options.borderSpacingMetres > 0.0
      ? options.borderSpacingMetres
      : options.tileSizeMetres / 8.0;
  settings.carveRivers = options.carveRivers;
  settings.beautifyEdges = options.beautify;
  settings.minimumAngleDegrees = options.minimumAngleDegrees;
  settings.terraceStrength = options.terraceStrength;
  // One ladder. The contours, the plateau rungs and the terrace shelves are
  // the same set of elevations, which is what lets a plateau's brink *be* a
  // contour line instead of merely resembling one.
  settings.terraceIntervalMetres = options.terraceIntervalMetres > 0.0
      ? options.terraceIntervalMetres
      : (world.plateaus.ladderIntervalMetres > 0.0
                ? world.plateaus.ladderIntervalMetres
                : std::max(1.0, world.contours.intervalMetres));
  settings.workerCount = options.workers;
  settings.progress = meshProgress("terrain",options.progress);
  return settings;
}

// Binary little-endian PLY, with a per-vertex colour so the faceting is visible
// in any viewer without materials.
// `alpha` writes a fourth colour channel and forces every vertex to it. The
// river surface is the one mesh that wants it: water is not an opaque lid, and
// a translucent sheet over the carved bed is the cheapest thing that reads as a
// river. Omitted (0) keeps the three-channel layout every other consumer
// expects, so the terrain format is unchanged.
bool writePly(
    const std::string &path, const TerrainMesh &mesh, std::string *error,
    const TerrainMapSource *mapSource = nullptr, uint8_t alpha = 0u)
{
  std::ofstream out(path, std::ios::binary);
  if (!out)
  {
    if (error != nullptr)
      *error = "could not open " + path;
    return false;
  }
  uint64_t vertexCount = 0u;
  uint64_t faceCount = 0u;
  for (const MeshTile &tile : mesh.tiles)
  {
    vertexCount += tile.vertices.size();
    faceCount += tile.triangleCount();
  }
  // One vertex per corner, not one per position. A vertex on a parcel boundary
  // belongs to two parcels and can only carry one colour, so shared vertices
  // make the rasteriser interpolate across every triangle - the colour has to
  // be constant over a face for the material to read at all. The mesh is flat
  // shaded from screen-space derivatives, so unsharing costs nothing but size.
  vertexCount = faceCount * 3u;
  out << "ply\nformat binary_little_endian 1.0\n"
      << "comment low-poly terrain, world metres, y up\n"
      << "element vertex " << vertexCount << "\n"
      << "property float x\nproperty float y\nproperty float z\n"
      << "property float nx\nproperty float ny\nproperty float nz\n"
      << "property uchar red\nproperty uchar green\nproperty uchar blue\n"
      << (alpha ? "property uchar alpha\n" : "")
      << "element face " << faceCount << "\n"
      << "property list uchar uint vertex_indices\n"
      << "property uchar material\n"
      << "end_header\n";

  // Smooth normals, accumulated by world position across tiles. Hard colour and
  // smooth shading are not in tension: a field is one material whatever the
  // light does across it, and a flat normal on a mesh of millions of small
  // triangles reports every sliver as a facet.
  std::map<std::pair<int64_t, int64_t>, std::array<double, 3>> smoothNormals;
  const auto positionKey = [](const double x, const double z)
  { return std::make_pair(int64_t(std::llround(x * 1000.0)), int64_t(std::llround(z * 1000.0))); };
  for (const MeshTile &tile : mesh.tiles)
    for (size_t face = 0u; face < tile.materials.size(); ++face)
    {
      const MeshVertex &a = tile.vertices[tile.indices[face * 3u]];
      const MeshVertex &b = tile.vertices[tile.indices[face * 3u + 1u]];
      const MeshVertex &c = tile.vertices[tile.indices[face * 3u + 2u]];
      double nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
      double ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
      double nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
      if (ny < 0) { nx = -nx; ny = -ny; nz = -nz; }
      for (const MeshVertex *vertex : {&a, &b, &c})
      {
        auto &at = smoothNormals[positionKey(vertex->x, vertex->z)];
        at[0] += nx; at[1] += ny; at[2] += nz;
      }
    }
  for (const MeshTile &tile : mesh.tiles)
  {
    for (size_t face = 0u; face < tile.materials.size(); ++face)
    {
      const MeshVertex &a = tile.vertices[tile.indices[face * 3u]];
      const MeshVertex &b = tile.vertices[tile.indices[face * 3u + 1u]];
      const MeshVertex &c = tile.vertices[tile.indices[face * 3u + 2u]];
      // Sampled at the centroid, which lies unambiguously inside one parcel.
      // A vertex sits on the boundary between two, so sampling there picks
      // whichever side the lookup happened to test first.
      const Point2 middle((a.x + b.x + c.x) / 3.0, (a.z + b.z + c.z) / 3.0);
      const uint8_t material = tile.materials[face];
      const std::array<float, 3> colour =
          mapSource ? mapSource->surfaceColour(middle, mesh.palette.entries()[material]) :
          mesh.palette.entries().empty()
          ? colourOf(static_cast<CellType>(material))
          : materialColour(
                mesh.palette.entries()[std::min<size_t>(material,
                    mesh.palette.entries().size() - 1u)]);
      const uint8_t rgb[3] = {
          static_cast<uint8_t>(std::clamp(colour[0], 0.0f, 1.0f) * 255.0f),
          static_cast<uint8_t>(std::clamp(colour[1], 0.0f, 1.0f) * 255.0f),
          static_cast<uint8_t>(std::clamp(colour[2], 0.0f, 1.0f) * 255.0f)};
      for (const MeshVertex *vertex : {&a, &b, &c})
      {
        const float position[3] = {static_cast<float>(vertex->x),
            static_cast<float>(vertex->y), static_cast<float>(vertex->z)};
        out.write(reinterpret_cast<const char *>(position), sizeof(position));
        const auto &sum = smoothNormals[positionKey(vertex->x, vertex->z)];
        const double length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
        const float normal[3] = {
            static_cast<float>(length > 1e-12 ? sum[0] / length : 0.0),
            static_cast<float>(length > 1e-12 ? sum[1] / length : 1.0),
            static_cast<float>(length > 1e-12 ? sum[2] / length : 0.0)};
        out.write(reinterpret_cast<const char *>(normal), sizeof(normal));
        out.write(reinterpret_cast<const char *>(rgb), sizeof(rgb));
        if (alpha) out.write(reinterpret_cast<const char *>(&alpha), 1);
      }
    }
  }
  // Vertices were written one per corner, in face order, so the indices are
  // simply consecutive.
  uint32_t corner = 0u;
  for (const MeshTile &tile : mesh.tiles)
    for (size_t face = 0u; face < tile.materials.size(); ++face)
    {
      const uint8_t corners = 3u;
      out.write(reinterpret_cast<const char *>(&corners), 1);
      for (uint32_t k = 0; k < 3u; ++k, ++corner)
        out.write(reinterpret_cast<const char *>(&corner), sizeof(corner));
      out.write(reinterpret_cast<const char *>(&tile.materials[face]), 1);
    }
  return out.good();
}

void reportRasterMesh(const TerrainMesh &mesh)
{
  const TerrainMeshStatistics &statistics = mesh.statistics;
  const double areaSquareKilometres =
      mesh.extentMetres * mesh.extentMetres * 1.0e-6;
  std::cout << std::fixed << std::setprecision(3)
            << "mesh signature   " << std::hex << mesh.signature() << std::dec
            << "\n"
            << "tiles            " << statistics.tileCount << " ("
            << mesh.tilesPerAxis << " x " << mesh.tilesPerAxis << " of "
            << mesh.tileSizeMetres * 1.0e-3 << " km)\n"
            << "vertices         " << statistics.vertexCount << "\n"
            << "triangles        " << statistics.triangleCount << "\n"
            << "triangles/km^2   "
            << static_cast<double>(statistics.triangleCount) /
            std::max(1.0, areaSquareKilometres)
            << "\n"
            << "slivers          " << statistics.sliverTriangleCount << " ("
            << 100.0 * statistics.sliverAreaSquareMetres /
            std::max(1.0, statistics.totalAreaSquareMetres)
            << "% of area)\n"
            << "build            " << statistics.buildSeconds << " s (peak tile "
            << statistics.peakTileSeconds << " s)\n"
            << "failed tiles     " << statistics.failedTiles << "\n"
            << "constraints      " << statistics.constraintsIncluded << "/"
            << statistics.constraintCount << " inserted, "
            << statistics.relaxedTiles << " tiles relaxed\n"
            << "materials        " << mesh.palette.size() << " palette entries"
            << (mesh.palette.saturated() ? " (SATURATED)" : "") << "\n"
            << "step faces       " << statistics.stepFaceTriangles
            << " triangles\n";
  const double megabytes =
      (static_cast<double>(statistics.vertexCount) * 12.0 +
          static_cast<double>(statistics.triangleCount) * 8.0) /
      (1024.0 * 1024.0);
  std::cout << "packed size      " << megabytes << " MiB\n";
}

void reportMesh(const TerrainMesh &mesh, const GeneratedWorld &world)
{
  const TerrainMeshStatistics &statistics = mesh.statistics;
  const double areaSquareKilometres =
      mesh.extentMetres * mesh.extentMetres * 1.0e-6;
  std::cout << std::fixed << std::setprecision(3)
            << "mesh signature   " << std::hex << mesh.signature() << std::dec
            << "\n"
            << "tiles            " << statistics.tileCount << " ("
            << mesh.tilesPerAxis << " x " << mesh.tilesPerAxis << " of "
            << mesh.tileSizeMetres * 1.0e-3 << " km)\n"
            << "vertices         " << statistics.vertexCount << "\n"
            << "triangles        " << statistics.triangleCount << "\n"
            << "triangles/km^2   "
            << static_cast<double>(statistics.triangleCount) /
            std::max(1.0, areaSquareKilometres)
            << "\n"
            << "smallest angle   " << statistics.minimumTriangleAngleDegrees
            << " deg, " << statistics.sliverTriangleCount << " under 5 deg ("
            << 100.0 * static_cast<double>(statistics.sliverTriangleCount) /
            std::max<double>(1.0, static_cast<double>(statistics.triangleCount))
            << "% of faces, "
            << 100.0 * statistics.sliverAreaSquareMetres /
            std::max(1.0, statistics.totalAreaSquareMetres)
            << "% of area)\n"
            << "build            " << statistics.buildSeconds << " s (peak tile "
            << statistics.peakTileSeconds << " s)\n"
            << "failed tiles     " << statistics.failedTiles << "\n"
            << "constraints      " << statistics.constraintsIncluded << "/"
            << statistics.constraintCount << " inserted, "
            << statistics.relaxedTiles << " tiles relaxed\n"
            << "contours         " << world.contours.lines.size() << " lines, "
            << world.contours.pointCount() << " points (interval "
            << world.contours.intervalMetres << " m)\n"
            << "coast runs       " << world.coastline.size() << "\n"
            << "plateaus         " << world.plateaus.blocks.size()
            << " blocks, " << world.plateaus.flattenedCells << " cells, ladder "
            << world.plateaus.ladderIntervalMetres << " m, mean riser "
            << world.plateaus.meanRiserMetres << " m\n"
            << "channels         " << statistics.strokeCourses << " reaches, "
            << statistics.strokeJunctions << " confluences, "
            << statistics.strokeDuplicates << " cells carved twice\n"
            << "materials        " << mesh.palette.size() << " palette entries"
            << (mesh.palette.saturated() ? " (SATURATED)" : "") << "\n"
            << "step faces       " << statistics.stepFaceTriangles
            << " triangles from " << statistics.stepCount << " quads\n"
            << "terraces         " << mesh.terraces.edges.size() << " ramps ("
            << [&] {
                 size_t stacked = 0u;
                 for (const auto &edge : mesh.terraces.edges)
                   stacked += edge.from != kBaseTerrace ? 1u : 0u;
                 return stacked;
               }()
            << " block-to-block), "
            << mesh.terraces.unreachableBlocks.size() << " unreachable of "
            << mesh.terraces.blockCount << ", steepest ramp "
            << mesh.terraces.steepestRampGradient << "\n"
            << "hydrology        " << world.rivers.riverCells << " river cells, "
            << world.rivers.mouths << " mouths, " << world.lakeCells
            << " lake cells, " << world.lakesFilled << " basin-filled, "
            << world.waterfalls << " waterfalls\n"
            << "fluvial          " << world.fluvial.iterations << " steps, "
            << world.fluvial.drainageRebuilds << " drainage rebuilds, relief "
            << world.fluvial.reliefBefore << " -> " << world.fluvial.reliefAfter
            << " m, mean erosion " << world.fluvial.meanErosionMetres
            << " m, " << world.fluvial.inversionsClamped << " inversions\n"
            << "                 uplift/height correlation "
            << world.fluvial.upliftCorrelationBefore << " -> "
            << world.fluvial.upliftCorrelationAfter << "\n"
            << "stylization      relief " << world.stylization.reliefBefore
            << " -> " << world.stylization.reliefAfter << " m, gain "
            << world.stylization.gainApplied << " of max "
            << world.stylization.maximumGain << ", "
            << world.ridges.size() << " ridge edges, " << world.peaks.size()
            << " peaks\n"
            << "elevation        " << world.deepestMetres << " to "
            << world.highestMetres << " m, land cells " << world.landCells
            << "\n";

  // Bytes, as the storage format would hold them: 12 per vertex and 8 per
  // triangle. The plan's budget is stated in these units.
  const double megabytes =
      (static_cast<double>(statistics.vertexCount) * 12.0 +
          static_cast<double>(statistics.triangleCount) * 8.0) /
      (1024.0 * 1024.0);
  std::cout << "packed size      " << megabytes << " MiB\n";
}

// The raster path: generate with the polygon terrain generator, trace its own
// coast, contours and ridges, resolve its drainage, then mesh.
struct RasterWorld
{
  PolygonTerrainMaps maps;
  PolygonTerrainStatistics statistics{};
  RasterTerrain terrain{};
  RasterChains chains{};
  ChannelNetwork channels{};
  double generateSeconds = 0.0;
  HeightFunction height;
};

double settingsSiteRadius(const Options &options)
{
  const double width = options.extentKilometres * 1000.0;
  return options.rasterSiteRadiusMetres > 0.0 ? options.rasterSiteRadiusMetres
                                              : std::clamp(width / 80.0, 120.0, 800.0);
}

bool generateRasterWorld(
    const Options &options, RasterWorld &world, std::string *error)
{
  const auto started = std::chrono::steady_clock::now();
  PolygonTerrainSettings settings{};
  settings.widthMetres = options.extentKilometres * 1000.0;
  settings.heightMetres = settings.widthMetres;
  // Site radius sets how many polygon regions the generator draws before it
  // rasterises. Derived from the world so the region count stays sane at any
  // extent rather than exploding on a large one.
  // Region spacing, matched to the planet map generator sample: 204k regions
  // over a 100 km-radius sphere is about 785 m between region centres, and that
  // is the density its whole pipeline - rivers, ridges, climate - is tuned for.
  // A planar world wants the same spacing, not the same count.
  settings.siteRadiusMetres = options.rasterSiteRadiusMetres > 0.0
      ? options.rasterSiteRadiusMetres
      : std::clamp(settings.widthMetres / 80.0, 120.0, 800.0);
  settings.seed = options.seed;
  settings.oceanMarginFraction = 0.06;
  settings.continuousElevation = true;
  PolygonTerrainRasterSettings raster{};
  raster.width = options.rasterResolution;
  raster.height = options.rasterResolution;
  raster.workerCount = 0u;

  PolygonTerrainGenerator generator;
  if (!generator.generate(
          settings, raster, world.maps, &world.statistics, error))
    return false;
  world.generateSeconds = secondsSince(started);

  world.terrain.maps = &world.maps;
  world.terrain.extentMetres = settings.widthMetres;
  world.terrain.reliefMetres = options.reliefMetres;
  world.terrain.seaLevelMetres = 0.0;
  return true;
}

bool buildRasterWorldMesh(const Options &options, RasterWorld &world,
    TerrainMesh &mesh, std::string &error)
{
  std::cout << "generating raster world, seed " << options.seed << " at "
            << options.extentKilometres << " km, " << options.rasterResolution
            << "^2\n";
  if (!generateRasterWorld(options, world, &error))
  {
    std::cerr << "raster generation failed: " << error << "\n";
    return false;
  }
  {
    // "Complete islands" is a measurable property, not a look: the land has to
    // close inside the map rather than run off its edge. The generator's
    // coast-distance elevation puts ocean at the border by construction, so a
    // border that is not water means the world is too small for its own
    // landmasses and the coastline is really the frame.
    const auto &maps = world.maps;
    uint64_t land = 0u;
    uint64_t borderLand = 0u;
    uint64_t border = 0u;
    for (uint32_t y = 0u; y < maps.height; ++y)
      for (uint32_t x = 0u; x < maps.width; ++x)
      {
        const bool isLand =
            maps.heightmap[static_cast<size_t>(y) * maps.width + x] != 0u;
        land += isLand ? 1u : 0u;
        if (x == 0u || y == 0u || x + 1u == maps.width || y + 1u == maps.height)
        {
          ++border;
          borderLand += isLand ? 1u : 0u;
        }
      }
    const double cell = world.terrain.cellMetres();
    std::cout << "source           " << world.statistics.siteCount
              << " sites (" << settingsSiteRadius(options) << " m spacing), "
              << world.statistics.riverEdgeCount << " river edges, in "
              << world.generateSeconds << " s\n"
              << "raster           " << maps.width << "^2 at " << cell
              << " m/texel, oversampling the region graph "
              << settingsSiteRadius(options) / cell << "x\n"
              << "land             "
              << 100.0 * static_cast<double>(land) /
            static_cast<double>(maps.heightmap.size())
              << "%, border land "
              << 100.0 * static_cast<double>(borderLand) /
            std::max<double>(1.0, static_cast<double>(border))
              << "% (islands are complete when this is near zero)\n";
  }

  {
    std::array<uint64_t, 21> materials{};
    std::array<uint64_t, 6> forms{};
    for (const auto material : world.maps.materials)
      if (material < materials.size()) ++materials[material];
    for (const auto form : world.maps.terrainForms)
      if (form < forms.size()) ++forms[form];
    const auto maximum = *std::max_element(world.maps.heightmap.begin(), world.maps.heightmap.end());
    std::cout << "relief           0 to " << maximum / 65535.0 * options.reliefMetres << " m\n"
              << "terrain texels   " << forms[2] << " valley, " << forms[4] << " mountain, " << forms[5] << " peak\n"
              << "biome texels     " << materials[14] + materials[20] << " desert, "
              << materials[5] + materials[8] << " snow/ice\n";
  }

  RasterSamplingSettings sampling{};
  sampling.toleranceMetres = options.toleranceMetres;
  sampling.chainSpacingMetres = options.chainSpacingMetres;
  sampling.seed = options.seed;
  sampling.contourIntervalMetres = options.contourIntervalMetres;
  world.chains = extractRasterChains(world.terrain, sampling);
  std::cout << "traced           " << world.chains.coastRuns << " coast runs, "
            << world.chains.contourLines << " contour lines (interval "
            << world.chains.contourIntervalMetres << " m), "
            << world.chains.ridgeRuns << " ridge runs, "
            << world.chains.biomeRuns << " biome borders, "
            << world.chains.regionEdges << " source region edges\n";

  StrokeSettings strokes{};
  strokes.seed = options.seed ^ 0x2545f4914f6cdd1dull;
  strokes.spacingMetres = std::max(30.0, world.terrain.cellMetres() * 1.5);
  std::vector<Chain> chains = world.chains.chains;
  std::vector<StepQuad> steps;
  world.height = rasterHeightFunction(world.terrain);
  if (options.carveRivers)
  {
    world.channels = resolveRasterChannels(world.terrain, sampling, strokes);
    StrokeGround ground{};
    ground.height = rasterHeightFunction(world.terrain);
    const RasterTerrain terrain = world.terrain;
    ground.cover = [terrain](const Point2 point)
    { return polygonMaterialToBiome(terrain.material(point)); };
    const StrokeGeometry water =
        buildStrokeGeometry(world.channels, ground, strokes);
    chains.insert(chains.end(), water.chains.begin(), water.chains.end());
    world.height = rasterRiverHeightFunction(world.terrain, world.channels);
    // The raster path carves one continuous surface. Overlaying bank quads on
    // the original heightfield would leave two intersecting terrain surfaces.
    for (Chain &chain : chains)
    {
      if (!chain.hasElevation) continue;
      std::vector<double> elevations;
      for (size_t index = 0; index < chain.points.size(); ++index)
        elevations.push_back(std::min(chain.elevationAt(index), world.height(chain.points[index])));
      chain.elevations = std::move(elevations);
    }
    std::cout << "channels         " << water.courseCount << " reaches, "
              << water.junctionCount << " confluences, "
              << world.channels.duplicateCells << " cells carved twice\n";
  }

  TerrainMeshSettings meshSettings = ::meshSettings(options, GeneratedWorld{});
  meshSettings.terraceIntervalMetres = world.chains.contourIntervalMetres;
  meshSettings.poissonSpacingMetres = options.poissonSpacingMetres > 0.0
      ? options.poissonSpacingMetres : std::max(world.terrain.cellMetres() * 2.0, 100.0);
  meshSettings.poissonLayers = options.poissonLayers;
  meshSettings.poissonSeed = options.seed;
  if (options.relocateSamples)
  meshSettings.poissonPlacement = [terrain = world.terrain](Point2 point, double radius)
  {
    const uint8_t form = terrain.terrainForm(point);
    if (form != 2 && form < 4) return point;
    // A bounded relocation toward the ridge/valley. The mesh builder rechecks
    // spacing and constraint clearance afterwards; semantic vertices never move.
    const double step = std::min(radius * 0.25, terrain.cellMetres());
    Point2 best = point;
    const double sign = form == 2 ? -1.0 : 1.0;
    double score = terrain.height(point) * sign;
    for (int y = -1; y <= 1; ++y)
      for (int x = -1; x <= 1; ++x)
      {
        const Point2 probe(point.x + x * step, point.y + y * step);
        if (terrain.water(probe)) continue;
        const double candidate = terrain.height(probe) * sign;
        if (candidate > score) { score = candidate; best = probe; }
      }
    return best;
  };
  meshSettings.preserveChainVertices = true;
  meshSettings.toleranceScale = rasterToleranceScale(world.terrain, sampling);
  const bool built = buildTerrainMesh(chains, steps,
      world.height,
      rasterMaterialFunction(world.terrain, mesh.palette),
      world.terrain.extentMetres, meshSettings, mesh, &error);
  flattenRasterOcean(world.terrain, mesh);
  reportRasterMesh(mesh);
  if (!built)
  {
    std::cerr << "mesh build failed: " << error << "\n";
    return false;
  }
  if (!options.plyPath.empty())
  {
    if (!writePly(options.plyPath, mesh, &error))
    {
      std::cerr << "ply export failed: " << error << "\n";
      return false;
    }
    std::cout << "wrote            " << options.plyPath << "\n";
  }
  return mesh.statistics.failedTiles == 0u;
}

void reportMapFidelity(const TerrainMapSource &source, const TerrainMesh &mesh)
{
  double maximumVertexError = 0.0, squaredError = 0.0, landArea = 0.0;
  double gentleSource = 0.0, gentleMesh = 0.0, maximumOceanError = 0.0;
  const double delta = source.cellMetres() * 0.25;
  for (const auto &tile : mesh.tiles)
  {
    for (size_t index = 0; index < tile.indices.size(); index += 3)
    {
      const auto &a = tile.vertices[tile.indices[index]];
      const auto &b = tile.vertices[tile.indices[index + 1]];
      const auto &c = tile.vertices[tile.indices[index + 2]];
      const Point2 point((a.x + b.x + c.x) / 3.0, (a.z + b.z + c.z) / 3.0);
      if (source.waterType(point))
      {
        if (source.waterType(point) == 1)
          for (const auto *vertex : {&a, &b, &c})
            maximumOceanError = std::max(maximumOceanError, std::abs(vertex->y - source.seaLevelMetres));
        continue;
      }
      for (const auto *vertex : {&a, &b, &c})
        maximumVertexError = std::max(maximumVertexError,
            std::abs(vertex->y - source.height({vertex->x, vertex->z})));
      const double determinant = (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
      const double area = std::abs(determinant) * 0.5;
      if (area < 1e-8) continue;
      const double gx = ((b.y - a.y) * (c.z - a.z) - (c.y - a.y) * (b.z - a.z)) / determinant;
      const double gz = ((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) / determinant;
      const double sx = (source.height({point.x + delta, point.y}) - source.height({point.x - delta, point.y})) / (2 * delta);
      const double sz = (source.height({point.x, point.y + delta}) - source.height({point.x, point.y - delta})) / (2 * delta);
      const double error = (a.y + b.y + c.y) / 3.0 - source.height(point);
      squaredError += error * error * area; landArea += area;
      gentleSource += std::hypot(sx, sz) < std::tan(5.0 * 3.141592653589793 / 180.0) ? area : 0.0;
      gentleMesh += std::hypot(gx, gz) < std::tan(5.0 * 3.141592653589793 / 180.0) ? area : 0.0;
    }
  }
  std::cout << "field fidelity   max vertex error " << maximumVertexError << " m, area-weighted interior RMS "
            << std::sqrt(squaredError / std::max(1.0, landArea)) << " m\n"
            << "gentle land      processed field " << 100 * gentleSource / std::max(1.0, landArea)
            << "%, mesh " << 100 * gentleMesh / std::max(1.0, landArea) << "% below 5 degrees\n"
            << "ocean error      " << maximumOceanError << " m from sea datum\n";
}

nlohmann::json landformDefinitions(const TerrainMapSource &source)
{
  auto output=nlohmann::json::array();
  for(const auto &f:source.rockFormations.definitions())
  {
    auto controls=nlohmann::json::array();for(const auto p:f.controls)controls.push_back({p.x,p.y});
    output.push_back({{"controls",controls},{"topMetres",f.topMetres},{"faceWidthMetres",f.faceWidthMetres},
        {"talusWidthMetres",f.talusWidthMetres},{"stratumMetres",f.stratumMetres},
        {"rampWidthMetres",f.rampWidthMetres},{"rampAzimuth",f.rampAzimuth},
        {"lithology",uint32_t(f.lithology)},{"seed",f.seed},{"parent",f.parent},
        {"capReliefMetres",f.capReliefMetres},{"capRoundness",f.capRoundness},{"capResistance",f.capResistance},{"cutCap",f.cutCap},{"upliftMetresPerYear",f.upliftMetresPerYear}});
  }
  return {{"formations",output}};
}

bool buildExportWorldMesh(const Options &options, TerrainMapSource &source, TerrainMesh &mesh, std::string &error, TerrainMesh *riverOutput = nullptr)
{
  std::cout << "loading world map " << options.worldMapPath << "\n";
  source.horizontalScale = options.worldScale;
  source.variedBiomes = options.variedBiomes;
  source.landforms.enabled = options.rockFormations;
  source.scarps.enabled = options.scarps;
  source.landforms.maximumCount = options.landformCount;
  source.landforms.cliffLevels = options.cliffLevels;
  source.guidedGeometry = options.guidedGeometry;
  if(!options.landformSplines.empty())
  {
    try
    {
      std::ifstream file(options.landformSplines);if(!file)throw std::runtime_error("cannot open spline file");
      nlohmann::json json;file>>json;
      const auto &formations=json.at("formations");
      if(!formations.is_array()||formations.size()>4096)throw std::runtime_error("formations must be an array of at most 4096 entries");
      for(const auto &entry:formations)
      {
        RockFormation f;f.topMetres=entry.at("topMetres").get<double>();
        const auto &controls=entry.at("controls");
        if(!controls.is_array()||controls.size()<3||controls.size()>256)throw std::runtime_error("each spline needs 3 to 256 controls");
        for(const auto &p:controls)
        {if(!p.is_array()||p.size()!=2)throw std::runtime_error("control must be [x,z]");f.controls.push_back({p[0].get<double>(),p[1].get<double>()});}
        f.faceWidthMetres=entry.value("faceWidthMetres",180.0);f.talusWidthMetres=entry.value("talusWidthMetres",450.0);
        f.stratumMetres=entry.value("stratumMetres",60.0);f.rampWidthMetres=entry.value("rampWidthMetres",0.0);
        f.rampAzimuth=entry.value("rampAzimuth",0.0);f.seed=entry.value("seed",uint64_t(0));
        f.parent=entry.value("parent",UINT32_MAX);
        f.capReliefMetres=entry.value("capReliefMetres",.5);f.capRoundness=entry.value("capRoundness",0.0);
        f.capResistance=entry.value("capResistance",40.0);
        f.cutCap=entry.value("cutCap",false);
        f.upliftMetresPerYear=entry.value("upliftMetresPerYear",0.0);
        const auto lith=entry.value("lithology",3u);if(lith>=procedural::kPlanetLithologyCount)throw std::runtime_error("invalid lithology");
        f.lithology=static_cast<procedural::PlanetLithology>(lith);source.landforms.authored.push_back(std::move(f));
      }
      source.landforms.maximumCount=0;
    }
    catch(const std::exception &e){error=std::string("landform splines: ")+e.what();return false;}
  }
  source.settlements.enabled = options.allocateLand;
  source.parcels.enabled = options.allocateLand;
  if (options.parcelMetres > 0)
    source.parcels.targetAreaSquareMetres = options.parcelMetres * options.parcelMetres;
  source.streamPower.enabled = options.erode;
  source.streamPower.iterations = options.upliftSteps;
  source.streamPower.maximumResolution = options.erosionResolution;
  source.streamPower.progress = [&options](uint32_t step,double years)
  { if(options.progress)options.progress("erosion step "+std::to_string(step)+" / "+std::to_string(options.upliftSteps),{"evolving landscape"});
    std::cout << "stream power     step " << step << ", model years " << years << std::endl; };
  source.hydraulics.enabled = false;
  source.hydraulics.maximumSteps = options.erodeIterations;
  if (!source.load(options.worldMapPath, &error))
  {
    std::cerr << "world-map load failed: " << error << "\n"
              << "Use --world-map <directory> to select an export, or --source raster to generate another map.\n";
    return false;
  }
  std::cout << "source field     " << source.elevationField << ", " << source.minimumElevation << " to "
            << source.maximumElevation << " m (original physical units)\n"
            << "construction     " << source.constructionField << " + graph uplift + stream power\n"
            << "source map       " << source.extentMetres * .001 << " km, " << source.resolution << "^2, "
            << source.contourCount() << " original contours, " << source.riverCount() << " river edges\n"
            << "geometry         " << (source.hasVectorRegions() ? "vector regions" : "raster fallback")
            << ", shore transition, river carving, initial ridge relief up to " << source.maximumMountainDetailMetres << " m\n"
            << "ridge skeleton   " << source.mountains.rangeCount << " ranges, " << source.mountains.edges.size()
            << " edges, " << source.mountains.passes.size() << " passes\n";
  {
    uint32_t cities=0,towns=0,villages=0;
    for(const auto &site:source.settlementNetwork.sites)
    { cities+=site.tier==SettlementTier::City; towns+=site.tier==SettlementTier::Town;
      villages+=site.tier==SettlementTier::Village; }
    uint32_t hedges=0,walls=0,lanes=0,treelines=0;
    for(const auto &edge:source.parcelLayer.edges)
    { hedges+=edge.type==ParcelEdgeType::Hedgerow; walls+=edge.type==ParcelEdgeType::Wall;
      lanes+=edge.type==ParcelEdgeType::Lane; treelines+=edge.type==ParcelEdgeType::Treeline; }
    std::cout << "settlements      " << cities << " cities, " << towns << " towns, " << villages
              << " villages, " << source.settlementNetwork.roads.size() << " roads\n";
    std::cout << "parcels          " << source.parcelLayer.cells.size() << " cells, "
              << source.parcelLayer.edges.size() << " edges (" << hedges << " hedgerow, " << walls
              << " wall, " << treelines << " treeline, " << lanes << " lane)\n";
  }
  // PARCEL_DUMP=<path> writes the allocation for inspection: the layer is
  // judged on whether the pattern reads as land, which no counter shows.
  if(const char *dump=std::getenv("PARCEL_DUMP"))
  {
    std::ofstream out(dump);
    for(const auto &site:source.settlementNetwork.sites)
      out<<"S "<<uint32_t(site.tier)<<' '<<site.position.x<<' '<<site.position.y<<'\n';
    for(const auto &road:source.settlementNetwork.roads)
    { out<<"R "<<uint32_t(road.kind)<<' '<<road.widthMetres<<' '<<road.path.size();
      for(const auto &q:road.path)out<<' '<<q.x<<' '<<q.y; out<<'\n'; }
    for(const auto &plan:source.settlementNetwork.plans)
    {
      out<<"P "<<plan.site<<' '<<plan.radiusMetres<<' '<<plan.outline.size();
      for(const auto &q:plan.outline)out<<' '<<q.x<<' '<<q.y; out<<'\n';
      if(!plan.wall.empty())
      { out<<"W "<<plan.wall.size(); for(const auto &q:plan.wall)out<<' '<<q.x<<' '<<q.y; out<<'\n'; }
      for(const auto &g:plan.gates)out<<"G "<<g.x<<' '<<g.y<<'\n';
      for(const auto &street:plan.streets)
      { out<<"T "<<street.size(); for(const auto &q:street)out<<' '<<q.x<<' '<<q.y; out<<'\n'; }
      for(const auto &d:plan.districts)
      { out<<"D "<<uint32_t(d.kind)<<' '<<d.boundary.size();
        for(const auto &q:d.boundary)out<<' '<<q.x<<' '<<q.y; out<<'\n'; }
    }
    for(const auto &cell:source.parcelLayer.cells)
    { out<<"C "<<uint32_t(cell.kind)<<' '<<uint32_t(cell.tone)<<' '<<cell.boundary.size();
      for(const auto &q:cell.boundary)out<<' '<<q.x<<' '<<q.y; out<<'\n'; }
    for(const auto &edge:source.parcelLayer.edges)
    { if(edge.type==ParcelEdgeType::None)continue;
      out<<"E "<<uint32_t(edge.type)<<' '<<edge.path.size();
      for(const auto &q:edge.path)out<<' '<<q.x<<' '<<q.y; out<<'\n'; }
    // The water, so the 2D map can be judged without building a mesh: every
    // reach with the width and depth it was given, and the shoreline as the
    // contour extraction sees it.
    // Reaches: one record per continuous river, with the half-width at each
    // vertex, so a consumer can stroke it rather than reassembling segments.
    for(const auto &reach:source.riverReaches)
    {
      if(reach.path.size()<2u)continue;
      out<<"X "<<reach.dischargeCumecs<<' '<<(reach.reachesWater?1:0)<<' '<<reach.path.size();
      for(size_t k=0;k<reach.path.size();++k)
        out<<' '<<reach.path[k].x<<' '<<reach.path[k].y<<' '<<reach.width[k];
      out<<'\n';
    }
    for(const auto &section:source.rivers.sections())
      out<<"V "<<section.a.x<<' '<<section.a.y<<' '<<section.b.x<<' '<<section.b.y<<' '
         <<section.widthA<<' '<<section.widthB<<' '<<section.depthA<<' '<<section.depthB<<'\n';
    for(const auto &chain:source.constraintChains())
      if(chain.kind==ChainKind::Coast&&chain.points.size()>1u)
      { out<<"K "<<chain.points.size();
        for(const auto &q:chain.points)out<<' '<<q.x<<' '<<q.y; out<<'\n'; }
    std::cout<<"wrote            "<<dump<<std::endl;
  }
  std::cout << "rock formations  " << source.rockFormations.size() << " spline footprints, " << source.rockFormations.chains().size() << " constrained rings and slope ribs\n";
  std::cout << "stream result    " << source.streamPowerReport.iterations << " steps, " << source.streamPowerReport.years
            << " years, max cut " << source.streamPowerReport.maximumCutMetres << " m, pits "
            << source.streamPowerReport.initialPits << " -> " << source.streamPowerReport.finalPits << std::endl;
  auto settings = meshSettings(options, GeneratedWorld{});
  if (!options.fieldPath.empty())
  {
    std::ofstream definitions(options.fieldPath+".landforms.json");definitions<<landformDefinitions(source).dump(2);definitions.close();
    std::ofstream field(options.fieldPath, std::ios::binary);
    const uint32_t side = source.resolution;
    std::ofstream colors(options.fieldPath + ".ppm", std::ios::binary);
    colors << "P6\n" << side << " " << side << "\n255\n";
    MaterialPalette diagnosticPalette;
    const auto diagnosticMaterial = source.materialFunction(diagnosticPalette);
    field.write(reinterpret_cast<const char *>(&side), sizeof(side));
    for (uint32_t y = 0; y < side; ++y) for (uint32_t x = 0; x < side; ++x)
    {
      const Point2 point((x+.5)*source.cellMetres(),(y+.5)*source.cellMetres());
      const float h = float(source.height(point));
      field.write(reinterpret_cast<const char *>(&h), sizeof(h));
      const auto color = source.surfaceColour(point, diagnosticPalette.entries()[diagnosticMaterial(point)]);
      const uint8_t rgb[] = {uint8_t(color[0]*255),uint8_t(color[1]*255),uint8_t(color[2]*255)};
      colors.write(reinterpret_cast<const char *>(rgb),sizeof(rgb));
    }
    if (!field || !colors) { error = "failed writing diagnostic field"; return false; }
    return true;
  }
  settings.simplificationRounds = options.simplifyGeometry?6:0;
  settings.simplificationToleranceMetres = std::min(1.0,settings.verticalToleranceMetres*.25);
  // False lets `emitRun` walk each chain by arc length and emit a point every
  // `chainSpacing`, which merges as readily as it splits. True keeps every
  // source vertex, and two chains a few metres apart each carrying their own
  // dense sampling is the ladder of slivers TerrainMeshBuild warns about.
  settings.preserveChainVertices = false;
  settings.maximumConstraintSegmentMetres = source.cellMetres();
  settings.borderSpacingMetres = options.borderSpacingMetres > 0 ? options.borderSpacingMetres : source.cellMetres();
  settings.poissonSpacingMetres = options.poissonSpacingMetres > 0 ? options.poissonSpacingMetres : source.cellMetres() * 2.0;
  settings.poissonLayers = options.poissonLayers;
  settings.poissonSeed = source.seed;
  settings.toleranceScale = [&source](Point2 p) { return source.toleranceScale(p); };
  settings.maximumTriangleArea = [&source](Point2 p) { return source.maximumTriangleArea(p); };
  settings.terraceStrength = 0.0;
  settings.beautifyEdges = false;
  // Original map plus continuous geometry profiles. Every newly split
  // constraint samples this same final field, without stale contour heights.
  settings.maximumTriangleCount=options.triangleBudget;
  settings.uniformSubdivisionRounds=options.productionQuality?2:0;
  std::vector<nlohmann::json> tileRecords;
  if(!options.tileDirectory.empty())
  {
    const std::filesystem::path directory(options.tileDirectory);
    if(std::filesystem::exists(directory) && !std::filesystem::is_empty(directory))
    { error="tile output directory must be empty"; std::cerr<<error<<"\n"; return false; }
    std::filesystem::create_directories(directory);
    const uint32_t side=uint32_t(std::ceil(source.extentMetres/settings.tileSizeMetres));
    tileRecords.resize(size_t(side)*side);
    settings.retainTiles=false;
    settings.tileSink=[&,side,directory](MeshTile &tile,std::string &message)
    {
      const size_t id=size_t(tile.tileZ)*side+tile.tileX;
      const std::string name="tile-"+std::to_string(tile.tileX)+"-"+std::to_string(tile.tileZ)+".ply";
      TerrainMesh part; part.palette=mesh.palette;part.tiles.push_back(std::move(tile));
      source.flattenOcean(part);
      const bool ok=writePly((directory/name).string(),part,&message,&source);
      tile=std::move(part.tiles.front());
      if(!ok)return false;
      tileRecords[id]={{"x",tile.tileX},{"z",tile.tileZ},{"file",name},
          {"vertices",tile.vertices.size()},{"triangles",tile.triangleCount()},
          {"constraints",tile.constraintCount},{"constraintsIncluded",tile.constraintsIncluded}};
      return true;
    };
  }
  if(options.progress)options.progress("geometry",{"extracting constraints"});
  std::cout<<"geometry         extracting CDT constraints"<<std::endl;
  const auto chains = source.constraintChains();
  std::cout << "meshing          " << chains.size() << " source chains, beautification off\n";
  if (!buildTerrainMesh(chains, [&source](Point2 p) { return source.height(p); },
          source.materialFunction(mesh.palette), source.extentMetres, settings, mesh, &error))
  {
    std::cerr << "mesh build failed: " << error << "\n";
    return false;
  }
  TerrainMesh riverMesh;
  auto riverSettings=settings;riverSettings.tileSink={};
  riverSettings.progress=meshProgress("rivers",options.progress);
  nlohmann::json riverRecords=nlohmann::json::array();
  std::mutex riverRecordMutex;
  if(!options.tileDirectory.empty())
  {
    const auto directory=std::filesystem::path(options.tileDirectory);
    riverSettings.tileSink=[&](MeshTile &tile,std::string &message)
    {
      if(tile.indices.empty())return true;
      const std::string name="river-"+std::to_string(tile.tileX)+"-"+std::to_string(tile.tileZ)+".ply";
      TerrainMesh part;part.palette=riverMesh.palette;part.tiles.push_back(std::move(tile));
      const bool okay=writePly((std::filesystem::path(options.tileDirectory)/name).string(),part,&message);
      tile=std::move(part.tiles.front());if(!okay)return false;
      std::lock_guard<std::mutex> lock(riverRecordMutex);
      riverRecords.push_back({{"x",tile.tileX},{"z",tile.tileZ},{"file",name},{"vertices",tile.vertices.size()},{"triangles",tile.triangleCount()}});
      return true;
    };
  }
  std::vector<Chain> riverShores;
  for(const auto &chain:chains)if(any(chain.kind,ChainKind::Coast))riverShores.push_back(chain);
  if(!source.rivers.buildMesh(riverSettings,riverMesh,&error,riverShores,[&source](Point2 p){return !source.waterType(p);}))return false;
  std::cout<<"river surface    "<<source.rivers.sections().size()<<" reaches, "<<riverMesh.statistics.triangleCount<<" separate water triangles\n";
  if(!options.plyPath.empty())
  {
    const auto path=std::filesystem::path(options.plyPath);
    if(!writePly((path.parent_path()/(path.stem().string()+"-rivers.ply")).string(),riverMesh,&error,
        nullptr,options.riverAlpha))return false;
  }
  if(!options.tileDirectory.empty())
  {
    nlohmann::json manifest={{"format","engine.terrain-tiles"},{"version",1},
        {"extentMetres",source.extentMetres},{"tileSizeMetres",settings.tileSizeMetres},
        {"triangles",mesh.statistics.triangleCount},{"vertices",mesh.statistics.vertexCount},
        {"seed",source.seed},{"tiles",tileRecords}};
    std::sort(riverRecords.begin(),riverRecords.end(),[](const auto &a,const auto &b){return std::pair{a["z"].template get<uint32_t>(),a["x"].template get<uint32_t>()}<std::pair{b["z"].template get<uint32_t>(),b["x"].template get<uint32_t>()};});
    manifest["riverMesh"]={{"tiles",riverRecords},{"triangles",riverMesh.statistics.triangleCount},{"vertices",riverMesh.statistics.vertexCount},{"material",{{"role",uint32_t(SurfaceRole::Water)},{"cover",uint32_t(procedural::world::Biome::Lake)}}}};
    manifest["rockFormations"]=landformDefinitions(source);
    manifest["guidedGeometry"]=options.guidedGeometry;
    manifest["simplifiedVertices"]=mesh.statistics.simplifiedVertices;
    manifest["triangleBudget"]=options.triangleBudget;
    manifest["subdivisionRounds"]=settings.uniformSubdivisionRounds;
    manifest["erosion"]={{"method","Schott et al. stream power"},{"iterations",source.streamPowerReport.iterations},
        {"modelYears",source.streamPowerReport.years},{"flowExponent",4},{"slopeExponent",1},{"areaExponent",.5}};
    manifest["materials"]=nlohmann::json::array();
    for(const auto &m:mesh.palette.entries())manifest["materials"].push_back({{"cover",uint32_t(m.cover)},
        {"role",uint32_t(m.role)},{"lithology",uint32_t(m.lithology)},{"weathering",m.weathering}});
    const auto path=std::filesystem::path(options.tileDirectory)/"manifest.json";
    std::ofstream out(path.string()+".tmp");out<<manifest.dump(2);out.close();
    if(!out){error="failed writing tile manifest";return false;}
    std::filesystem::rename(path.string()+".tmp",path);
    std::cout<<"archive complete "<<mesh.statistics.triangleCount<<" triangles, "
             <<mesh.statistics.constraintsIncluded<<" / "<<mesh.statistics.constraintCount
             <<" constraints, "<<mesh.statistics.buildSeconds<<" s"<<std::endl;
    return true;
  }
  if(riverOutput)*riverOutput=std::move(riverMesh);
  source.flattenOcean(mesh);
  reportRasterMesh(mesh);
  reportMapFidelity(source, mesh);
  if (!options.plyPath.empty() && !writePly(options.plyPath, mesh, &error, &source)) return false;
  return true;
}

int runExportHeadless(const Options &options)
{
  TerrainMapSource source;
  TerrainMesh mesh;
  std::string error;
  if(buildExportWorldMesh(options, source, mesh, error))return 0;
  std::cerr << "terrain generation failed: " << error << "\n";
  return 1;
}

int runRasterHeadless(const Options &options)
{
  RasterWorld world;
  TerrainMesh mesh;
  std::string error;
  return buildRasterWorldMesh(options, world, mesh, error) ? 0 : 1;
}

int runHeadless(const Options &options)
{
  std::cout << "generating seed " << options.seed << " at "
            << options.extentKilometres << " km\n";
  const auto started = std::chrono::steady_clock::now();
  GeneratedWorld world;
  std::string error;
  WorldPipelineCallbacks callbacks{};
  callbacks.setPhase = [](const char *phase, const double progress)
  {
    std::cout << "  [" << std::setw(3) << static_cast<int>(progress * 100.0)
              << "%] " << phase << "\n";
  };
  if (!generateWorld(pipelineSettings(options), world, callbacks, &error))
  {
    std::cerr << "generation failed: " << error << "\n";
    return 1;
  }
  std::cout << "map              " << world.graph.nodeCount() << " cells in "
            << secondsSince(started) << " s\n"
            << "map signature    " << std::hex << world.graph.signature()
            << std::dec << "\n";

  TerrainMesh mesh;
  const bool meshBuilt = buildTerrainMesh(world, meshSettings(options, world), mesh, &error);
  reportMesh(mesh, world);
  if (!meshBuilt)
    std::cerr << "mesh build failed: " << error << "\n";

  if (!options.plyPath.empty())
  {
    const auto plyStart = std::chrono::steady_clock::now();
    if (!writePly(options.plyPath, mesh, &error))
    {
      std::cerr << "ply export failed: " << error << "\n";
      return 1;
    }
    std::cout << "wrote            " << options.plyPath << " in "
              << secondsSince(plyStart) << " s\n";
  }
  return mesh.statistics.failedTiles == 0u ? 0 : 2;
}

const MaterialPalette *gPalette = nullptr;
const TerrainMapSource *gMapSource = nullptr;

uint32_t packedColourOf(const uint8_t material, double x, double z)
{
  std::array<float, 3> colour{0.5f, 0.5f, 0.5f};
  if (gPalette != nullptr && material < gPalette->entries().size())
    colour = gMapSource ? gMapSource->surfaceColour({x,z}, gPalette->entries()[material])
                        : materialColour(gPalette->entries()[material]);
  const auto channel = [](const float value)
  {
    return static_cast<uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
  };
  return channel(colour[0]) | (channel(colour[1]) << 8u) |
      (channel(colour[2]) << 16u) | 0xff000000u;
}

namespace rewrite = procedural::world::rewrite;
bool rewriteMesh(const rewrite::RuntimeSnapshot &snapshot, TerrainMesh &out, std::string &error)
{
  rewrite::RuntimeDisplayMesh display;
  if (!rewrite::buildRuntimeDisplayMesh(snapshot, 1000000, display, &error)) return false;
  TerrainMesh result;
  const auto &bounds = snapshot.assets->levels[0].world.map.settings;
  result.extentMetres = std::max(bounds.maxX-bounds.minX, bounds.maxZ-bounds.minZ);
  MeshTile tile;
  for (const auto &v : display.vertices) tile.vertices.push_back({v.x,v.y,v.z});
  for (size_t i = 0; i < display.triangles.size(); ++i)
  {
    if (display.materials[i] > 255) { error = "rewrite viewer requires byte material IDs"; return false; }
    for (auto id : display.triangles[i]) tile.indices.push_back(id);
    tile.materials.push_back(uint8_t(display.materials[i]));
  }
  result.tiles.push_back(std::move(tile));
  out = std::move(result);
  return true;
}
bool loadRewrite(const Options &options, rewrite::RuntimeSnapshot &snapshot, TerrainMesh &mesh, std::string &error)
{
  std::ifstream file(options.rewriteArchive, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() < 0 || file.tellg() > 64*1024*1024) { error = "rewrite archive missing or exceeds 64 MiB"; return false; }
  std::string encoded(size_t(file.tellg()), '\0');
  file.seekg(0);
  if (!file.read(encoded.data(), encoded.size())) { error = "rewrite archive read failed"; return false; }
  return rewrite::loadRuntimeArchive(encoded, snapshot, &error) && rewriteMesh(snapshot, mesh, error);
}
uint32_t rewriteColour(uint8_t material, double, double)
{
  static const std::array<uint32_t,14> colours{0xff71936fu,0xffa2b9cbu,0xff8b9eaeu,0xff6c8599u,0xff718797u,0xff6292b7u,
    0xff80977eu,0xff9bafadu,0xff8aa2b9u,0xffba8a42u,0xff728592u,0xff99a6b5u,0xff7d909du,0xff557a9du};
  return colours[material < colours.size() ? material : 5];
}

int runWindowed(const Options &options)
{
  os::Logger::setIdleSleep(lib::time::TimeSpan::fromMilliseconds(10));
  os::Logger::start();

  GeneratedWorld world;
  RasterWorld rasterWorld;
  TerrainMapSource mapSource;
  TerrainMesh mesh,riverMesh;
  std::string error;
  HeightFunction groundHeight;
  double highestMetres = 0.0;
  rewrite::RuntimeSnapshot rewriteSnapshot;
  rewrite::WorldRuntimeCollision rewriteCollision;
  if (!options.rewriteArchive.empty())
  {
    if (!loadRewrite(options, rewriteSnapshot, mesh, error))
    { std::cerr << error << "\n"; os::Logger::shutdown(); return 1; }
    if (!rewriteCollision.build(rewriteSnapshot,1000000,&error))
    { std::cerr << error << "\n"; os::Logger::shutdown(); return 1; }
    for (const auto &tile : mesh.tiles) for (const auto &v : tile.vertices) highestMetres = std::max(highestMetres,v.y);
    groundHeight = [&rewriteSnapshot](Point2 p)
    {
      rewrite::RuntimeSurfaceHit hit;
      return rewrite::queryRuntimeSurface(rewriteSnapshot,p.x,p.y,0,hit) ? hit.height : 0.;
    };
  }
  else if (options.exportSource)
  {
    if (!generateWithProgress(options,[&](const Options &observed){return buildExportWorldMesh(observed, mapSource, mesh, error, &riverMesh);},error))
    {
      std::cerr << "terrain generation failed: " << error << "\n";
      os::Logger::shutdown();
      return 1;
    }
    groundHeight = [&mapSource](Point2 p) { return mapSource.height(p); };
    highestMetres = mapSource.maximumElevation;
  }
  else if (options.rasterSource)
  {
    if (!buildRasterWorldMesh(options, rasterWorld, mesh, error))
    {
      os::Logger::shutdown();
      return 1;
    }
    groundHeight = rasterWorld.height;
    highestMetres = *std::max_element(rasterWorld.maps.heightmap.begin(),
        rasterWorld.maps.heightmap.end()) / 65535.0 * options.reliefMetres;
  }
  else
  {
    WorldPipelineCallbacks callbacks{};
    callbacks.setPhase = [](const char *phase, double)
    { std::cout << "  " << phase << "\n"; };
    if (!generateWorld(pipelineSettings(options), world, callbacks, &error) ||
        !buildTerrainMesh(world, meshSettings(options, world), mesh, &error))
    {
      std::cerr << "generation failed: " << error << "\n";
      os::Logger::shutdown();
      return 1;
    }
    reportMesh(mesh, world);
    groundHeight = [&world](const Point2 point)
    { return world.surface.sample(point).elevationMetres; };
    highestMetres = world.highestMetres;
  }

  auto window = std::make_unique<window::sdl3::SDL3Window>(
      window::WindowSurface_Vulkan, "Low-poly terrain", 1600, 1000);
  if (!window->isRunning || window->sdlWindow == nullptr)
  {
    std::cerr << "could not create the window: " << SDL_GetError() << "\n";
    os::Logger::shutdown();
    return 1;
  }

  namespace gpu = rendering;
  namespace vkbackend = rendering::backend::vulkan;
  const gpu::DeviceRequiredLimits limits{
    .minimumMemory = 0u,
    .minimumComputeSharedMemory = 0u,
    .minimumComputeWorkGroupInvocations = 64u,
  };
  lowpoly::TerrainRHI rhi(vkbackend::Vulkan_1_2, limits,
      gpu::DeviceFeatures_Compute, std::vector<std::string>{});
  std::vector<VkSurfaceKHR> surfaces{window->getVulkanSurface(rhi.getInstance())};
  rhi.init(surfaces);
  rhi.setSwapChainVSyncEnabled(true);
  gpu::SwapChain swapChain =
      rhi.createSwapChain(0u, window->getWidth(), window->getHeight());

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  if (!ImGui_ImplSDL3_InitForVulkan(window->sdlWindow))
    throw std::runtime_error("Failed to initialize the ImGui SDL3 backend");

  lowpoly::FramePass framePass(rhi);
  framePass.initialize(swapChain);
  lowpoly::TerrainMeshPass meshPass(rhi, LOW_POLY_TERRAIN_SHADER_PATH);
  meshPass.initialize(framePass.renderPass());
  lowpoly::TerrainMeshPass riverPass(rhi, LOW_POLY_TERRAIN_SHADER_PATH);
  riverPass.initialize(framePass.renderPass());
  riverPass.setMaterialColour([](uint8_t,double,double){return 0xffb87b43u;});
  gPalette = &mesh.palette;
  gMapSource = options.exportSource ? &mapSource : nullptr;
  auto coverColour = [](const rewrite::RuntimeSnapshot &snapshot, uint8_t material, double x, double z)
  {
    rewrite::CoverSample sample;
    // Current vertex-shading path uses the largest selected tile footprint.
    const auto lod = *std::max_element(snapshot.tileLevels.begin(),snapshot.tileLevels.end());
    if (!rewrite::sampleRuntimeCover(*snapshot.assets,x,z,material,std::ldexp(snapshot.assets->coverPages[0].page.footprintMetres,int(lod)),sample)) return rewriteColour(material,x,z);
    double rgb[3]{};
    for (size_t i=0;i<sample.weights.size();++i)
    {
      const auto representative=snapshot.assets->coverDefinitions[i].materials[0];
      const auto colour=rewriteColour(uint8_t(representative<256?representative:5),x,z);
      for (int c=0;c<3;++c) rgb[c]+=double((colour>>(8*c))&255)*sample.weights[i];
    }
    const double variation=.9+.2*sample.variation;
    uint32_t packed=0xff000000u;
    for (int c=0;c<3;++c) packed|=uint32_t(std::clamp(rgb[c]*variation,0.,255.)+.5)<<(8*c);
    return packed;
  };
  if (rewriteSnapshot.assets) meshPass.setMaterialColour([&](uint8_t m,double x,double z){return coverColour(rewriteSnapshot,m,x,z);});
  else meshPass.setMaterialColour(packedColourOf);
  meshPass.setFlatShading(!options.rewriteArchive.empty());

  ImGui_ImplVulkan_InitInfo imguiInfo{};
  imguiInfo.ApiVersion = VK_API_VERSION_1_2;
  imguiInfo.Instance = rhi.getInstance();
  imguiInfo.PhysicalDevice = rhi.getPhysicalDevice();
  imguiInfo.Device = rhi.getDevice();
  imguiInfo.QueueFamily = rhi.getGraphicsQueueFamilyIndex();
  imguiInfo.Queue = rhi.getGraphicsQueue();
  imguiInfo.DescriptorPoolSize = 64u;
  imguiInfo.MinImageCount = std::max(2u, rhi.getSwapChainImagesCount(swapChain));
  imguiInfo.ImageCount = imguiInfo.MinImageCount;
  imguiInfo.PipelineInfoMain.RenderPass = framePass.renderPass();
  imguiInfo.PipelineInfoMain.Subpass = 0u;
  imguiInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  imguiInfo.CheckVkResultFn = lowpoly::FramePass::check;
  if (!ImGui_ImplVulkan_Init(&imguiInfo))
    throw std::runtime_error("Failed to initialize the ImGui Vulkan backend");

  if ((!riverMesh.tiles.empty() && !riverPass.upload(riverMesh, &error)) || !meshPass.upload(mesh, &error))
  {
    std::cerr << "mesh upload failed: " << error << "\n";
    return 1;
  }

  lowpoly::TerrainCamera camera{};
  const auto extent = static_cast<float>(mesh.extentMetres);
  camera.x = extent * 0.5f;
  camera.z = extent * 0.1f;
  camera.y = static_cast<float>(highestMetres) + extent * 0.06f;
  camera.pitchRadians = -0.35f;
  bool wireframe = false;
  bool mouseLook = false;
  auto lastFrame = std::chrono::steady_clock::now();

  uint32_t rewriteFrame = 0;
  if (rewriteSnapshot.assets)
  {
    const auto &b = rewriteSnapshot.assets->levels[0].world.map.settings;
    camera.x = float((b.minX+b.maxX)*.5); camera.z = float(b.minZ-extent*.35);
    camera.y = float(highestMetres)+extent*.6f; camera.pitchRadians = -.65f;
    camera.speedMetresPerSecond = extent*.1f;
  }
  bool running = true;
  while (running)
  {
    SDL_Event event{};
    while (SDL_PollEvent(&event))
    {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT ||
          (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
              event.window.windowID == SDL_GetWindowID(window->sdlWindow)))
        running = false;
      if (event.type == SDL_EVENT_WINDOW_RESIZED &&
          event.window.windowID == SDL_GetWindowID(window->sdlWindow))
      {
        window->width = static_cast<uint32_t>(std::max(1, event.window.data1));
        window->height = static_cast<uint32_t>(std::max(1, event.window.data2));
        rhi.waitIdle();
        framePass.destroyFramebuffers();
        rhi.destroySwapChain(swapChain);
        swapChain =
            rhi.createSwapChain(0u, window->getWidth(), window->getHeight());
        ImGui_ImplVulkan_SetMinImageCount(
            std::max(2u, rhi.getSwapChainImagesCount(swapChain)));
        framePass.onSwapChainRecreated(swapChain);
      }
      // Log-scaled speed on the wheel: walking pace to hundreds of km/s on the
      // same control, because a linear one is unusable at both ends.
      if (event.type == SDL_EVENT_MOUSE_WHEEL && !io.WantCaptureMouse)
        camera.speedMetresPerSecond = std::clamp(
            camera.speedMetresPerSecond * std::pow(1.25f, event.wheel.y),
            1.0f, 200000.0f);
      if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
          event.button.button == SDL_BUTTON_RIGHT && !io.WantCaptureMouse)
        mouseLook = true;
      if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
          event.button.button == SDL_BUTTON_RIGHT)
        mouseLook = false;
      if (event.type == SDL_EVENT_MOUSE_MOTION && mouseLook)
      {
        camera.yawRadians -= event.motion.xrel * 0.0032f;
        camera.pitchRadians = std::clamp(
            camera.pitchRadians - event.motion.yrel * 0.0032f, -1.53f, 1.53f);
      }
      if (event.type == SDL_EVENT_KEY_DOWN &&
          event.key.key == SDLK_F && !io.WantCaptureKeyboard)
        wireframe = !wireframe;
    }
    if (SDL_GetWindowFlags(window->sdlWindow) & SDL_WINDOW_MINIMIZED)
    {
      SDL_Delay(10);
      continue;
    }

    const auto now = std::chrono::steady_clock::now();
    const float delta = std::min(0.1f,
        std::chrono::duration<float>(now - lastFrame).count());
    lastFrame = now;

    const bool *keys = SDL_GetKeyboardState(nullptr);
    if (!io.WantCaptureKeyboard)
    {
      const std::array<float, 3> forward = camera.forward();
      const float right[3] = {forward[2], 0.0f, -forward[0]};
      const float rightLength =
          std::max(1.0e-6f, std::sqrt(right[0] * right[0] + right[2] * right[2]));
      const float step = camera.effectiveSpeed() * delta *
          (keys[SDL_SCANCODE_LSHIFT] ? 6.0f : 1.0f);
      const auto move = [&](const float dx, const float dy, const float dz)
      {
        camera.x += dx * step;
        camera.y += dy * step;
        camera.z += dz * step;
      };
      if (keys[SDL_SCANCODE_W]) move(forward[0], forward[1], forward[2]);
      if (keys[SDL_SCANCODE_S]) move(-forward[0], -forward[1], -forward[2]);
      if (keys[SDL_SCANCODE_D]) move(right[0] / rightLength, 0.0f, right[2] / rightLength);
      if (keys[SDL_SCANCODE_A]) move(-right[0] / rightLength, 0.0f, -right[2] / rightLength);
      if (keys[SDL_SCANCODE_E]) move(0.0f, 1.0f, 0.0f);
      if (keys[SDL_SCANCODE_Q]) move(0.0f, -1.0f, 0.0f);
    }
    camera.groundHeightMetres = static_cast<float>(
        groundHeight(Point2(camera.x, camera.z)));

    if (rewriteSnapshot.assets && options.rewriteFrames && rewriteFrame % 10 == 0)
    {
      auto next = rewriteSnapshot;
      if (rewriteFrame == 30 && !options.rewriteReloadArchive.empty())
      {
        Options reload = options; reload.rewriteArchive = options.rewriteReloadArchive;
        TerrainMesh checked;
        if (!loadRewrite(reload,next,checked,error) || next.assets->levels[0].world.map.revision <= rewriteSnapshot.assets->levels[0].world.map.revision)
        { std::cerr << "FAIL rewrite edit reload: " << error << "\n"; return 1; }
      }
      const auto lod = (rewriteFrame / 10) % next.assets->levels.size();
      for (size_t i = 0; i < next.tileLevels.size(); ++i) next.tileLevels[i] = uint32_t(rewriteFrame<30 ? lod : (lod+i)%next.assets->levels.size());
      TerrainMesh replacement;
      if (!rewriteMesh(next, replacement, error)) { std::cerr << error << "\n"; return 1; }
      rewrite::WorldRuntimeCollision nextCollision;
      const bool newAssets = next.assets != rewriteSnapshot.assets;
      if (newAssets && !nextCollision.build(next,1000000,&error)) { std::cerr << error << "\n"; return 1; }
      const auto oldTriangles = meshPass.triangleCount();
      meshPass.setMaterialColour([&](uint8_t m,double x,double z){return coverColour(next,m,x,z);});
      meshPass.setUploadBudget(1);
      const bool rejected = !meshPass.upload(replacement, &error) && meshPass.ready() && meshPass.triangleCount() == oldTriangles;
      meshPass.setUploadBudget(256ull*1024*1024);
      if (!rejected || !meshPass.upload(replacement, &error)) { std::cerr << "FAIL rewrite GPU transaction: " << error << "\n"; return 1; }
      // Upload returned only after writes and retirement succeeded. CPU readers
      // switch at this same frame boundary, and retain the same asset revision.
      rewriteSnapshot = std::move(next); mesh = std::move(replacement);
      meshPass.setMaterialColour([&](uint8_t m,double x,double z){return coverColour(rewriteSnapshot,m,x,z);});
      if (newAssets) rewriteCollision = std::move(nextCollision);
      if (rewriteCollision.revision() != rewriteSnapshot.assets->levels[0].world.map.revision) { std::cerr << "FAIL collision revision mismatch\n"; return 1; }
      camera.x += extent*.025f;
      std::cout << "PASS rewrite GPU frame=" << rewriteFrame << " revision=" << rewriteSnapshot.assets->levels[0].world.map.revision
                << " triangles=" << meshPass.triangleCount() << " budget-rejection-preserved-parent=1 collision-revision=" << rewriteCollision.revision() << " cover-levels=" << rewriteSnapshot.assets->coverMips.size()+1 << " cover-footprint=" << std::ldexp(rewriteSnapshot.assets->coverPages[0].page.footprintMetres,int(*std::max_element(rewriteSnapshot.tileLevels.begin(),rewriteSnapshot.tileLevels.end()))) << "\n";
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Once);
    ImGui::Begin("Low-poly terrain");
    ImGui::Text("%llu triangles, %llu vertices",
        static_cast<unsigned long long>(meshPass.triangleCount()),
        static_cast<unsigned long long>(meshPass.vertexCount()));
    ImGui::Text("%.1f fps", static_cast<double>(io.Framerate));
    ImGui::Separator();
    ImGui::Text("position %.0f, %.0f, %.0f m", camera.x, camera.y, camera.z);
    ImGui::Text("ground %.0f m, altitude %.0f m", camera.groundHeightMetres,
        camera.y - camera.groundHeightMetres);
    ImGui::Text("speed %.1f m/s (effective %.1f)", camera.speedMetresPerSecond,
        camera.effectiveSpeed());
    ImGui::Checkbox("Altitude-relative speed", &camera.altitudeRelativeSpeed);
    ImGui::Checkbox("Wireframe (F)", &wireframe);
    if (rewriteSnapshot.assets)
    {
      const auto forward = camera.forward();
      rewrite::RuntimeCollisionHit pick;
      const rewrite::Position from{camera.x,camera.y,camera.z}, to{camera.x+forward[0]*extent*2,camera.y+forward[1]*extent*2,camera.z+forward[2]*extent*2};
      if (rewriteCollision.segment(from,to,rewrite::CollisionSolid,pick))
        ImGui::Text("Solid pick %.2f m / material %u / revision %llu",pick.distanceMetres,pick.material,static_cast<unsigned long long>(pick.revision));
      else ImGui::TextUnformatted("Solid pick: no intersection");
    }

    ImGui::Separator();
    ImGui::TextWrapped(
        "Right-drag to look, WASD to move, Q/E down and up, shift to sprint, "
        "wheel to change speed.");
    ImGui::End();
    ImGui::Render();

    // This integration path uses a single uniform allocation: finish preceding
    // GPU readers before writing the next frame. Per-frame rings are a future optimization.
    if (rewriteSnapshot.assets) lowpoly::FramePass::check(vkDeviceWaitIdle(rhi.getDevice()));
    framePass.render(swapChain, rhi.getCurrentSwapChainTextureView(swapChain),
        ImGui::GetDrawData(),
        [&](VkCommandBuffer commandBuffer, VkExtent2D frameExtent)
        { meshPass.render(commandBuffer, frameExtent, camera, wireframe);
          if(riverPass.ready())riverPass.render(commandBuffer, frameExtent, camera, wireframe); });
    if (options.rewriteFrames && ++rewriteFrame >= options.rewriteFrames) running = false;
  }

  rhi.waitIdle();
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  os::Logger::shutdown();
  return 0;
}

void printUsage()
{
  std::cout
      << "Low-poly terrain generator\n\n"
      << "  (no flag)                 open the 3D view\n"
      << "  --world-scale <factor>    horizontal map expansion, default 2 (256 km for world-data)\n"
      << "  --erosion-resolution <n>  simulation grid side, default 1024, capped by source resolution\n"
      << "  --uplift-steps <n>        paper stream-power iterations, default 1800\n"
      << "  --varied-biomes <0|1>     climate and landform variety, default 1\n"
      << "  --rock-formations <0|1>   legacy spline cliffs and buttes, default 0\n"
      << "  --scarps <0|1>            contour scarps cut before erosion, default 1\n"
      << "  --landform-splines <json> authored closed footprints in world metres\n"
      << "  --guided-geometry <0|1>  final-surface ridge, river and slope guides, default 1\n"
      << "  --simplify <0|1>         simplify planar interiors, preserving constraints, default 1\n"
      << "  --cliff-levels <1..4>     maximum nested tiers per root, default 3\n"
      << "  --landform-count <n>      maximum automatic roots, default 32\n"
      << "  --tile-output <dir>       stream independent PLY tiles and a final manifest\n"
      << "  --triangle-budget <n>     hard output ceiling, default 300000000\n"
      << "  --quality <low|high|production> production requires tile output; 2 subdivision rounds\n"
      << "  --field <path>            dump uint32 side + float32 heights, skip mesh\n"
      << "  --rewrite-world <path>     audited compiled rewrite archive (bypasses legacy generation)\n"
      << "  --rewrite-reload <path>    replace with a newer archived world at playback frame 30\n"
      << "  --rewrite-frames <n>       bounded live GPU LOD playback and replacement rejection test\n"
      << "  --headless                generate, build the mesh, and report\n"
      << "  --ply <path>              write the mesh as binary PLY\n"
      << "  --seed <n>\n"
      << "  --extent <km>             regenerated sources only, default 128\n"
      << "  --relief <m>              regenerated sources only, default 6000\n"
      << "  --spacing <km>            map cell spacing, default 0.7\n"
      << "  --levels <1..4>           map hierarchy levels\n"
      << "  --tile <m>                build tile size, default 4000\n"
      << "  --tolerance <m>           vertical error budget, default 4\n"
      << "  --rounds <n>              refinement rounds, default 7\n"
      << "  --chain-spacing <m>       constraint resampling\n"
      << "  --border-spacing <m>      tile border sampling\n"
      << "  --beautify <0|1>          legacy cliff detail; off for world-map imports\n"
      << "  --source <world-map|raster|map>  default world-map uses the voxel map export\n"
      << "  --world-map <directory>   existing map export, preserving its physical scale\n"
      << "  --raster-resolution <n>   source heightmap side, default 2048\n"
      << "  --site-radius <m>         polygon region size\n"
      << "  --contour-interval <m>    0 derives the elevation ladder\n"
      << "  --poisson-spacing <m>     base layer spacing, 0 derives from raster\n"
      << "  --poisson-layers <0..6>   density layers, default 3\n"
      << "  --relocate <0|1>          attract free samples to ridges/valleys\n"
      << "  --erode <0|1>             stream-power relief against uplift\n"
      << "  --erode-iterations <n>\n"
      << "  --erode-flatten <0..1>    discard analytic relief, build it from uplift\n"
      << "  --stylize <0|1>           expand the upper relief band\n"
      << "  --stylize-gain <0..1>     how much, bounded by monotonicity\n"
      << "  --rivers <0|1>            carve the drainage network\n"
      << "  --plateaus <0|1>          flatten blocks and cut cliffs\n"
      << "  --plateau-coverage <0..1> share of land made into mesa\n"
      << "  --plateau-rungs <n>       cliff height in ladder rungs\n"
      << "  --min-angle <deg>         Delaunay quality bound, 0 disables\n"
      << "  --terrace <0..1>          contour terracing strength\n"
      << "  --terrace-interval <m>    terrace ladder, default the map ladder\n"
      << "  --land <0|1>              settlements, roads and the parcel layer\n"
      << "  --parcel-size <m>         target field side, default from the parcel layer\n"
      << "  --workers <n>             0 selects hardware concurrency\n";
}

} // namespace

int main(const int argc, char **argv)
{
  std::cout << std::unitbuf;
  Options options{};
  for (int argument = 1; argument < argc; ++argument)
  {
    const std::string option = argv[argument];
    if (option == "--help")
    {
      printUsage();
      return 0;
    }
    if (option == "--headless")
    {
      options.headless = true;
      continue;
    }
    if (argument + 1 >= argc)
    {
      printUsage();
      return 1;
    }
    const std::string value = argv[++argument];
    if (option == "--rewrite-world") options.rewriteArchive = value;
    else if (option == "--rewrite-reload") options.rewriteReloadArchive = value;
    else if (option == "--rewrite-frames") options.rewriteFrames = uint32_t(std::clamp(std::atoi(value.c_str()), 1, 10000));
    else if (option == "--tile-output") { options.tileDirectory=value; options.headless=true; }
    else if (option == "--triangle-budget") options.triangleBudget=std::strtoull(value.c_str(),nullptr,10);
    else if (option == "--world-scale") options.worldScale = std::atof(value.c_str());
    else if (option == "--erosion-resolution") options.erosionResolution = uint32_t(std::max(4,std::atoi(value.c_str())));
    else if (option == "--land") options.allocateLand = std::atoi(value.c_str()) != 0;
    else if (option == "--parcel-size") options.parcelMetres = std::max(30.0, std::atof(value.c_str()));
    else if (option == "--uplift-steps") options.upliftSteps = uint32_t(std::max(0,std::atoi(value.c_str())));
    else if (option == "--rock-formations") options.rockFormations = std::atoi(value.c_str()) != 0;
    else if (option == "--scarps") options.scarps = std::atoi(value.c_str()) != 0;
    else if (option == "--landform-splines") options.landformSplines = value;
    else if (option == "--guided-geometry") options.guidedGeometry = std::atoi(value.c_str()) != 0;
    else if (option == "--simplify") options.simplifyGeometry = std::atoi(value.c_str()) != 0;
    else if (option == "--cliff-levels") options.cliffLevels = uint32_t(std::clamp(std::atoi(value.c_str()),1,4));
    else if (option == "--landform-count") options.landformCount = uint32_t(std::max(0,std::atoi(value.c_str())));
    else if (option == "--varied-biomes") options.variedBiomes = std::atoi(value.c_str()) != 0;
    else if (option == "--quality")
    {
      if (value != "high" && value != "low" && value != "production") { std::cerr << "quality must be high, low or production\n"; return 1; }
      options.productionQuality = value == "production";
      options.poissonSpacingMetres = options.productionQuality ? 0 : value == "high" ? 62.5 : 125;
      options.poissonLayers = options.productionQuality ? 3 : value == "high" ? 4 : 3;
      options.toleranceMetres = value == "high" ? 1 : 4;
    }
    else if (option == "--field")
    { options.fieldPath = value; options.headless = true; }
    else if (option == "--ply")
      options.plyPath = value;
    else if (option == "--seed")
      options.seed = std::strtoull(value.c_str(), nullptr, 10);
    else if (option == "--extent")
      options.extentKilometres = std::atof(value.c_str());
    else if (option == "--relief")
      options.reliefMetres = std::atof(value.c_str());
    else if (option == "--spacing")
      options.cellSpacingKilometres = std::atof(value.c_str());
    else if (option == "--levels")
      options.hierarchyLevels = static_cast<uint32_t>(
          std::clamp(std::atoi(value.c_str()), 1, 4));
    else if (option == "--tile")
      options.tileSizeMetres = std::atof(value.c_str());
    else if (option == "--tolerance")
      options.toleranceMetres = std::atof(value.c_str());
    else if (option == "--rounds")
      options.refinementRounds = static_cast<uint32_t>(
          std::clamp(std::atoi(value.c_str()), 0, 24));
    else if (option == "--chain-spacing")
      options.chainSpacingMetres = std::atof(value.c_str());
    else if (option == "--border-spacing")
      options.borderSpacingMetres = std::atof(value.c_str());
    else if (option == "--beautify")
      options.beautify = std::atoi(value.c_str()) != 0;
    else if (option == "--world-map")
    {
      options.worldMapPath = value;
      options.exportSource = true;
    }
    else if (option == "--source")
    {
      if (value != "raster" && value != "map" && value != "world-map")
      {
        std::cerr << "source must be world-map, raster or map\n";
        return 1;
      }
      options.exportSource = value == "world-map";
      options.rasterSource = value == "raster";
    }
    else if (option == "--raster-resolution")
      options.rasterResolution = static_cast<uint32_t>(
          std::clamp(std::atoi(value.c_str()), 64, 8192));
    else if (option == "--site-radius")
      options.rasterSiteRadiusMetres = std::atof(value.c_str());
    else if (option == "--contour-interval")
      options.contourIntervalMetres = std::atof(value.c_str());
    else if (option == "--poisson-spacing")
      options.poissonSpacingMetres = std::atof(value.c_str());
    else if (option == "--poisson-layers")
      options.poissonLayers = static_cast<uint32_t>(std::clamp(std::atoi(value.c_str()), 0, 6));
    else if (option == "--relocate")
      options.relocateSamples = std::atoi(value.c_str()) != 0;
    else if (option == "--erode")
      options.erode = std::atoi(value.c_str()) != 0;
    else if (option == "--erode-iterations")
      options.erodeIterations = static_cast<uint32_t>(
          std::clamp(std::atoi(value.c_str()), 1, 400));
    else if (option == "--erode-flatten")
      options.erodeFlatten = std::atof(value.c_str());
    else if (option == "--stylize")
      options.stylize = std::atoi(value.c_str()) != 0;
    else if (option == "--stylize-gain")
      options.stylizeGain = std::atof(value.c_str());
    else if (option == "--rivers")
      options.carveRivers = std::atoi(value.c_str()) != 0;
    else if (option == "--plateaus")
      options.plateaus = std::atoi(value.c_str()) != 0;
    else if (option == "--plateau-coverage")
      options.plateauCoverage = std::atof(value.c_str());
    else if (option == "--plateau-rungs")
      options.plateauRungs = static_cast<uint32_t>(
          std::clamp(std::atoi(value.c_str()), 1, 12));
    else if (option == "--min-angle")
      options.minimumAngleDegrees = std::atof(value.c_str());
    else if (option == "--terrace")
      options.terraceStrength = std::atof(value.c_str());
    else if (option == "--terrace-interval")
      options.terraceIntervalMetres = std::atof(value.c_str());
    else if (option == "--workers")
      options.workers = static_cast<uint32_t>(std::max(0, std::atoi(value.c_str())));
    else
    {
      printUsage();
      return 1;
    }
  }

  if(options.productionQuality && options.tileDirectory.empty())
  { std::cerr<<"production quality requires --tile-output <empty directory> for bounded memory\n"; return 1; }
  if(!options.tileDirectory.empty() && (!options.exportSource || !options.plyPath.empty()))
  { std::cerr<<"tile output requires world-map source and cannot be combined with --ply\n"; return 1; }
  if (options.headless && !options.rewriteArchive.empty())
  {
    rewrite::RuntimeSnapshot snapshot; TerrainMesh mesh; std::string error;
    if (!loadRewrite(options,snapshot,mesh,error)) { std::cerr << error << "\n"; return 1; }
    std::cout << "PASS rewrite archive viewer input revision=" << snapshot.assets->levels[0].world.map.revision << " triangles=" << mesh.triangleCount() << "\n";
    return 0;
  }
  if (options.headless)
    return options.exportSource ? runExportHeadless(options) : options.rasterSource ? runRasterHeadless(options) : runHeadless(options);
  return runWindowed(options);
}
