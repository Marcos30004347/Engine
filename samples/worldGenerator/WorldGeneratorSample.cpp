#include "WorldGeneratorViews.hpp"

#include "os/Logger.hpp"
#include "procedural/world/MapClimate.hpp"
#include "procedural/world/MapCoast.hpp"
#include "procedural/world/MapContours.hpp"
#include "procedural/world/MapElevation.hpp"
#include "procedural/world/MapExport.hpp"
#include "procedural/world/MapGraph.hpp"
#include "procedural/world/MapHierarchy.hpp"
#include "procedural/world/MapLandmass.hpp"
#include "procedural/world/MapPartition.hpp"
#include "procedural/world/MapRegions.hpp"
#include "procedural/world/MapRivers.hpp"
#include "procedural/world/MapTerrain.hpp"
#include "procedural/world/Raisers.hpp"
#include "procedural/world/WorldBrief.hpp"
#include "procedural/world/WorldPipelineDirector.hpp"
#include "procedural/world/land/Blend.hpp"
#include "time/TimeSpan.hpp"
#include "window/sdl3/SDL3Window.hpp"

#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{

namespace gpu = rendering;
namespace vkbackend = rendering::backend::vulkan;

using namespace procedural::world;
using procedural::Point2;

constexpr double kMaximumMountainElevationMetres = 4500.0;

// The result of one generation, as a single unit. Everything downstream of the
// graph is derived, so they are produced together and swapped in together: a
// half-updated set - a new graph against the previous mesh - would draw a map
// that never existed.
// The generated world plus this sample's picture of it.
//
// Generation lives in procedural::world::generateWorld so that this sample and
// the low-poly terrain sample cannot drift apart. What is added here is the
// cartographic mesh, which is a *view*: the same world is drawn completely
// differently by the other sample, and the runtime has no business owning
// either picture.
struct WorldResult : GeneratedWorld
{
  worldgen::MapMesh mesh;
  double meshSeconds = 0.0;
};

struct GenerationJob
{
  std::thread worker;
  std::mutex mutex;
  bool running = false;
  bool finished = false;
  bool success = false;
  bool cancelRequested = false;
  double progress = 0.0;
  std::string phase;
  std::string error;
  double elapsedSeconds = 0.0;
  WorldResult result;
};

struct SaveJob
{
  std::thread worker;
  std::mutex mutex;
  bool running = false;
  bool finished = false;
  bool success = false;
  double progress = 0.0;
  std::string phase;
  std::string error;
  std::string summary;
};

struct ApplicationState
{
  // Brief, as the UI edits it.
  std::array<bool, static_cast<size_t>(Tag::Count)> tagSelected{};
  uint64_t seed = 7ull;
  // A game-scale country: large enough for several terrain provinces while a
  // player can still cross it in hours once terrain and route detours count.
  double extentKilometres = 128.0;
  double seaLevelMetres = 0.0;
  double reliefMetres = kMaximumMountainElevationMetres;

  double landFraction = 0.75;
  // 0.5 is the RaiseSettings default and is enough noise to warp the island
  // archetype's disc into an unrecognisable blob - the coast displaces by about
  // 15% of the world extent at that setting. 0.25 keeps the archetype legible
  // while still breaking the analytic outline.
  double coastComplexity = 0.25;
  double coastDetail = 0.55;
  // Also the craton count per continent, and therefore how many seams there are
  // to put ranges on.
  uint32_t landmassCores = 9u;
  // Roughly 180 sites per axis preserves enough drainage depth for rivers and
  // biome variety at this extent while still generating inside a second.
  double cellSpacingKilometres = 0.7;
  uint32_t hierarchyLevels = 2u;
  // Growth is the default now: the archetypes make blobs, and a blob is what
  // every render of this generator was until the coarse partition existed to
  // grow over.
  bool grownLandmasses = true;
  // A continent is a size, not a share of the world. The span is per
  // continent, so two continents at 1,250 km do not split one country's area.
  double continentSpanKilometres = 105.0;
  uint32_t continentCount = 1u;
  double cratonSpread = 0.34;
  double rescaledExtent = 0.0;

  BlendSettings blend{};
  ElevationSettings elevation = []
  {
    ElevationSettings settings{};
    settings.maximumRangesPerLandmass = 3u;
    settings.rangeStrength = 0.68;
    settings.broadReliefFraction = 2200.0 /
        kMaximumMountainElevationMetres;
    return settings;
  }();
  TerrainConditioningSettings terrain{};
  RiverSettings rivers{};
  LakeSettings lakes{};
  RegionSettings regions{};
  ClimateSettings climate = []
  {
    ClimateSettings settings{};
    // A temperate-to-boreal window gives a game-scale country warm lowlands
    // and a real alpine snowline without turning every upland into ice.
    settings.southLatitudeDegrees = 38.0;
    settings.latitudeSpanDegrees = 16.0;
    settings.rainShadowHalfHeightMetres = 1800.0;
    return settings;
  }();
  worldgen::MapShading shading{};
  worldgen::MapOverlayStyle overlay{};
  bool useGpuFill = true;
  bool useGpuLines = true;

  worldgen::MapView view{};
  bool viewInitialized = false;
  bool dragging = false;
  ImVec2 mapOrigin{};
  ImVec2 mapSize{};

  bool blendRebuildNeeded = false;
  bool gpuUploadNeeded = false;
  double gpuUploadSeconds = 0.0;
  size_t gpuVertexCount = 0u;
  size_t gpuIndexCount = 0u;
  size_t gpuLineInstanceCount = 0u;
  size_t gpuMaterialPageCount = 0u;
  size_t gpuMaterialPageBytes = 0u;
  uint32_t selectedNode = kInvalidNode;

  GenerationJob generation;
  SaveJob save;
  WorldResult world;
  bool generated = false;
  char saveImagePath[512] = "world-map.png";
  char saveExportDirectory[512] = "world-data";
  int saveImageResolution = 1024;
  int saveExportResolution = 1024;
  std::string status = "Pick tags and press Generate.";
};

// Several settings are lengths and areas, and their shipped defaults were tuned
// against a 361 km world. On a France-sized one they are absurd: a 22 km range
// width is a hairline on a 950 km continent, and a 2,500 km^2 region gives two
// hundred labels. Scaled from the extent whenever the extent changes, so the
// dials keep meaning the same thing at any world size.
void rescaleForExtent(ApplicationState &state);

WorldBrief briefFromState(const ApplicationState &state)
{
  WorldBrief brief{};
  brief.seed = state.seed;
  brief.extentMetres = state.extentKilometres * 1000.0;
  brief.seaLevelMetres = state.seaLevelMetres;
  brief.reliefMetres = state.reliefMetres;
  for (size_t tag = 0u; tag < state.tagSelected.size(); ++tag)
    if (state.tagSelected[tag])
      brief.tags.push_back(static_cast<Tag>(tag));
  resolveTags(brief.tags);
  return brief;
}

// measuredShape() is a four-way classifier over the border land fraction, the
// one the reference generator uses. It cannot name archipelago, bay, fjord or
// headland, so comparing it directly against the requested tag reports a
// mismatch for four of the eight archetypes that are working correctly. Compare
// families instead.
Tag expectedMeasuredShape(const Tag requested)
{
  switch (requested)
  {
  case Tag::Island:
  case Tag::Archipelago:
  case Tag::Headland:
    // Land in the middle, water at the border.
    return Tag::Island;
  case Tag::Coast:
  case Tag::Bay:
  case Tag::Fjord:
    // Land meeting water across the world, so the border is mixed.
    return Tag::Coast;
  default:
    return requested;
  }
}

void rescaleForExtent(ApplicationState &state)
{
  const double scale = state.extentKilometres / 361.0;
  // Narrower than the 22 km that suited a compact 361 km island. Ranges now sit
  // on the seams between the blocks a continent grew from, and a seam is a
  // line: an 85 km flank turns a 250 km seam into a dome rather than a chain.
  state.elevation.rangeWidthMetres =
      std::max(8000.0, 9000.0 * std::sqrt(scale));
  state.elevation.coastalPlainMetres = 60000.0 * scale;
  state.elevation.shelfWidthMetres = 50000.0 * scale;
  state.elevation.detailWavelengthMetres = 22000.0 * scale;
  // Landform provinces are geographic features, not a fixed fraction of the
  // viewport. Scale them sublinearly so a larger continent gets more terrain
  // variety without turning the province diagram into cell-scale patchwork.
  const double landformScale = std::sqrt(scale);
  state.elevation.landformSpacingMetres = 140000.0 * landformScale;
  state.elevation.landformEdgeMetres = 18000.0 * landformScale;
  state.terrain.floodplainWidthMetres = 26000.0 * landformScale;
  // How far maritime moisture reaches is a length like any other. Left at its
  // 1,400 km default on a small world, nothing is far enough from the sea for
  // the gradient to mean anything and the orographic term wins - the interior
  // comes out wetter than the coast.
  state.climate.coastalMoistureRangeMetres = 240000.0 * scale;
  state.elevation.blockSpacingMetres = 200000.0 * scale;
  state.elevation.blockEdgeMetres = 5000.0 * scale;
  // Regions are provinces: France has thirteen at about 42,000 km^2 and
  // ninety-six departments at 5,700. Twenty thousand sits between them and
  // gives a map you can read.
  state.regions.targetAreaSquareMetres = 2500.0e6 * scale * scale;
  state.rescaledExtent = state.extentKilometres;
}

double secondsSince(const std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// Runs the map stage on a worker, then builds the cartographic mesh from it.
//
// Cancellation and progress are the caller's, not the pipeline's, so this owns
// the mutex and hands the pipeline two closures.
void runGeneration(
    GenerationJob &job,
    const WorldPipelineSettings &settings,
    const worldgen::MapShading &shading)
{
  const auto started = std::chrono::steady_clock::now();

  WorldPipelineCallbacks callbacks{};
  callbacks.setPhase = [&job](const char *phase, const double progress)
  {
    std::lock_guard<std::mutex> lock(job.mutex);
    job.phase = phase;
    job.progress = progress;
  };
  callbacks.cancelled = [&job]
  {
    std::lock_guard<std::mutex> lock(job.mutex);
    return job.cancelRequested;
  };

  WorldResult result{};
  std::string error;
  auto inhabitedSettings=settings;inhabitedSettings.historyYears=settings.historyYears?settings.historyYears:200;
  if (!procedural::world::generateWorldDirected(inhabitedSettings, result, callbacks, &error))
  {
    std::lock_guard<std::mutex> lock(job.mutex);
    job.error = error;
    job.success = false;
    job.running = false;
    job.finished = true;
    job.elapsedSeconds = secondsSince(started);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(job.mutex);
    job.phase = "Blending";
    job.progress = 0.9;
  }
  const auto meshStart = std::chrono::steady_clock::now();
  worldgen::MapShading meshShading = shading;
  meshShading.seaLevelMetres = settings.brief.seaLevelMetres;
  meshShading.reliefMetres = settings.brief.reliefMetres;
  meshShading.peakUpliftMetresPerYear =
      settings.elevation.upliftRateMetresPerYear;
  result.mesh = worldgen::buildMapMesh(result.graph, result.coastline,
      result.rivers, result.regions, result.contours, result.surface,
      result.index, settings.blend, meshShading);
  result.meshSeconds = secondsSince(meshStart);

  std::lock_guard<std::mutex> lock(job.mutex);
  job.result = std::move(result);
  job.success = true;
  job.running = false;
  job.finished = true;
  job.progress = 1.0;
  job.phase = "Done";
  job.elapsedSeconds = secondsSince(started);
}

bool generationRunning(ApplicationState &state)
{
  std::lock_guard<std::mutex> lock(state.generation.mutex);
  return state.generation.running;
}

bool saveRunning(ApplicationState &state)
{
  std::lock_guard<std::mutex> lock(state.save.mutex);
  return state.save.running;
}

// The UI's state, as the pipeline's settings. Both the windowed and the
// headless path go through here so they cannot configure different worlds.
WorldPipelineSettings pipelineSettingsFromState(const ApplicationState &state)
{
  WorldPipelineSettings settings{};
  settings.brief = briefFromState(state);
  settings.grownLandmasses = state.grownLandmasses;
  settings.landFraction = state.landFraction;
  settings.coastComplexity = state.coastComplexity;
  settings.coastDetail = state.coastDetail;
  settings.landmassCores = state.landmassCores;
  settings.continentCount = state.continentCount;
  settings.continentSpanMetres = state.continentSpanKilometres * 1000.0;
  settings.cratonSpread = state.cratonSpread;
  settings.cellSpacingMetres = state.cellSpacingKilometres * 1000.0;
  settings.hierarchyLevels = state.hierarchyLevels;
  settings.blend = state.blend;
  settings.elevation = state.elevation;
  settings.terrain = state.terrain;
  settings.rivers = state.rivers;
  settings.lakes = state.lakes;
  settings.regions = state.regions;
  settings.climate = state.climate;
  return settings;
}

void startGeneration(ApplicationState &state)
{
  if (generationRunning(state) || saveRunning(state))
    return;
  if (state.generation.worker.joinable())
    state.generation.worker.join();

  if (state.rescaledExtent != state.extentKilometres)
    rescaleForExtent(state);
  const WorldPipelineSettings settings = pipelineSettingsFromState(state);
  const worldgen::MapShading shading = state.shading;
  {
    std::lock_guard<std::mutex> lock(state.generation.mutex);
    state.generation.running = true;
    state.generation.finished = false;
    state.generation.success = false;
    state.generation.cancelRequested = false;
    state.generation.progress = 0.0;
    state.generation.phase = "Starting";
    state.generation.error.clear();
  }
  state.status = "Generating...";
  state.generation.worker = std::thread(
      [&state, settings, shading]
      { runGeneration(state.generation, settings, shading); });
}

void requestCancel(ApplicationState &state)
{
  std::lock_guard<std::mutex> lock(state.generation.mutex);
  if (state.generation.running)
  {
    state.generation.cancelRequested = true;
    state.status = "Cancelling...";
  }
}

void pollGeneration(ApplicationState &state)
{
  {
    std::lock_guard<std::mutex> lock(state.generation.mutex);
    if (!state.generation.finished)
      return;
  }
  if (state.generation.worker.joinable())
    state.generation.worker.join();

  bool success = false;
  std::string error;
  double elapsed = 0.0;
  {
    std::lock_guard<std::mutex> lock(state.generation.mutex);
    success = state.generation.success;
    error = state.generation.error;
    elapsed = state.generation.elapsedSeconds;
    if (success)
      state.world = std::move(state.generation.result);
    state.generation.finished = false;
  }
  if (!success)
  {
    // Deliberately does not clear state.generated: a failed run must leave the
    // previous map on screen. Gating the view on the generation flag is the
    // mistake that once took the whole planet off screen in the terrain sample.
    state.status = "Generation failed: " + error;
    std::cout << state.status << "\n";
    return;
  }
  state.generated = true;
  state.gpuUploadNeeded = true;
  state.selectedNode = kInvalidNode;
  state.viewInitialized = false;
  state.shading.seaLevelMetres = state.seaLevelMetres;
  state.shading.reliefMetres = state.world.reliefMetres;
  state.shading.peakUpliftMetresPerYear = state.elevation.upliftRateMetresPerYear;

  std::ostringstream summary;
  summary.setf(std::ios::fixed);
  summary.precision(2);
  summary << "Generated " << state.world.graph.nodeCount() << " cells in " << elapsed << " s. Requested " << tagName(state.world.requestedShape) << ", measured " << tagName(state.world.measuredShapeTag) << ".";
  state.status = summary.str();
  std::cout << state.status << "\n";
}

void startSave(ApplicationState &state)
{
  if (!state.generated || generationRunning(state) || saveRunning(state))
    return;
  if (state.save.worker.joinable())
    state.save.worker.join();

  const std::string imagePath = state.saveImagePath;
  const std::string exportDirectory = state.saveExportDirectory;
  const uint32_t imageResolution = static_cast<uint32_t>(
      std::clamp(state.saveImageResolution, 64, 8192));
  const uint32_t exportResolution = static_cast<uint32_t>(
      std::clamp(state.saveExportResolution, 2, 4096));
  const BlendSettings blend = state.blend;
  worldgen::MapShading shading = state.shading;
  shading.seaLevelMetres = state.seaLevelMetres;
  shading.reliefMetres = state.world.reliefMetres;
  shading.peakUpliftMetresPerYear =
      state.elevation.upliftRateMetresPerYear;
  const worldgen::MapOverlayStyle overlay = state.overlay;
  {
    std::lock_guard<std::mutex> lock(state.save.mutex);
    state.save.running = true;
    state.save.finished = false;
    state.save.success = false;
    state.save.progress = 0.0;
    state.save.phase = "Rendering PNG";
    state.save.error.clear();
    state.save.summary.clear();
  }
  state.status = "Saving the current map and world-data bundle...";
  state.save.worker = std::thread(
      [&state, imagePath, exportDirectory, imageResolution, exportResolution,
       blend, shading, overlay]
      {
        const auto started = std::chrono::steady_clock::now();
        const auto phase = [&state](const char *name, const double progress)
        {
          std::lock_guard<std::mutex> lock(state.save.mutex);
          state.save.phase = name;
          state.save.progress = progress;
        };
        const auto finish = [&state](const bool success,
                                const std::string &message)
        {
          std::lock_guard<std::mutex> lock(state.save.mutex);
          state.save.success = success;
          if (success)
            state.save.summary = message;
          else
            state.save.error = message;
          state.save.progress = 1.0;
          state.save.phase = success ? "Saved" : "Failed";
          state.save.running = false;
          state.save.finished = true;
        };

        const WorldResult &world = state.world;
        std::string error;
        const std::vector<uint8_t> rgba = worldgen::renderMapImage(
            world.graph, world.coastline, world.rivers, world.mesh,
            world.surface, world.index, blend, shading, imageResolution,
            imageResolution, world.extentMetres, overlay);
        if (!worldgen::writePng(
                imagePath, imageResolution, imageResolution, rgba, error))
        {
          finish(false, error);
          return;
        }

        phase("Building world data", 0.35);
        MapExportSettings exportSettings{};
        exportSettings.resolution = exportResolution;
        MapExportBundle bundle;
        if (!buildMapExportBundle(world.graph, world.index, world.surface,
                world.climate, world.rivers, world.coastline, world.contours,
                world.regions, blend, shading.heightBlend, exportSettings,
                bundle, &error))
        {
          finish(false, "World-data export failed: " + error);
          return;
        }

        phase("Writing world data", 0.75);
        if (!writeMapExportBundle(bundle, exportDirectory, &error))
        {
          finish(false, "World-data export failed: " + error);
          return;
        }

        phase("Verifying world data", 0.92);
        MapExportBundle verified;
        MapGraph verifiedGraph;
        MapExportFeatures verifiedFeatures;
        if (!readMapExportBundle(exportDirectory, verified, &error) ||
            verified.bundleSignature != bundle.bundleSignature ||
            !decodeMapExportGraph(verified, verifiedGraph, &error) ||
            !decodeMapExportFeatures(verified, verifiedFeatures, &error) ||
            verifiedGraph.signature() != world.graph.signature() ||
            verifiedFeatures.contours.lines.size() !=
                world.contours.lines.size())
        {
          finish(false, "World-data verification failed: " + error);
          return;
        }

        std::ostringstream summary;
        summary.setf(std::ios::fixed);
        summary.precision(2);
        summary << "Saved " << imagePath << " and " << exportDirectory
                << " in " << secondsSince(started) << " s.";
        finish(true, summary.str());
      });
}

void pollSave(ApplicationState &state)
{
  {
    std::lock_guard<std::mutex> lock(state.save.mutex);
    if (!state.save.finished)
      return;
  }
  if (state.save.worker.joinable())
    state.save.worker.join();

  bool success = false;
  std::string message;
  {
    std::lock_guard<std::mutex> lock(state.save.mutex);
    success = state.save.success;
    message = success ? state.save.summary : state.save.error;
    state.save.finished = false;
  }
  state.status = success ? message : "Save failed: " + message;
  std::cout << state.status << "\n";
}

// --------------------------------------------------------------------------
// UI
// --------------------------------------------------------------------------

void drawTagSelector(ApplicationState &state)
{
  ImGui::TextUnformatted("Shape");
  ImGui::Separator();
  for (uint8_t tag = 0u; tag < static_cast<uint8_t>(Tag::Count); ++tag)
  {
    const Tag value = static_cast<Tag>(tag);
    if (!tagIsShape(value))
      continue;
    ImGui::Checkbox(tagName(value), &state.tagSelected[tag]);
    if ((tag % 2u) == 0u)
      ImGui::SameLine(160.0f * ImGui::GetIO().FontGlobalScale);
  }
  ImGui::NewLine();
  // Ticking two shape tags is a UI mistake, not a fatal one: it resolves. Show
  // what it resolved to rather than refusing to run.
  const WorldBrief brief = briefFromState(state);
  ImGui::TextDisabled("resolves to: %s", tagName(shapeTag(brief.tags)));

  ImGui::Spacing();
  ImGui::TextUnformatted("Character");
  ImGui::Separator();
  int column = 0;
  for (uint8_t tag = 0u; tag < static_cast<uint8_t>(Tag::Count); ++tag)
  {
    const Tag value = static_cast<Tag>(tag);
    if (tagIsShape(value))
      continue;
    ImGui::Checkbox(tagName(value), &state.tagSelected[tag]);
    if ((column++ % 2) == 0)
      ImGui::SameLine(160.0f * ImGui::GetIO().FontGlobalScale);
  }
  ImGui::NewLine();
  ImGui::TextDisabled("Character tags modulate the generators, not the land mask.");
}

void drawControls(ApplicationState &state, const float panelWidth)
{
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(panelWidth, ImGui::GetIO().DisplaySize.y));
  ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

  ImGui::TextUnformatted("BRIEF");
  ImGui::Separator();
  drawTagSelector(state);

  ImGui::Spacing();
  int seed = static_cast<int>(state.seed & 0x7fffffffull);
  if (ImGui::InputInt("seed", &seed))
    state.seed = static_cast<uint64_t>(std::max(0, seed));
  ImGui::SameLine();
  if (ImGui::Button("Randomise"))
    state.seed = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count() & 0x7fffffffll);

  // The brief is in metres and kilometres; ImGui's sliders are float. Kept
  // narrow rather than templated, since every one of these is a UI-range value
  // where float is exact enough and the stored double is what generates.
  {
    float extent = static_cast<float>(state.extentKilometres);
    if (ImGui::SliderFloat("extent km", &extent, 100.0f, 4000.0f, "%.0f"))
    {
      // A continent is a physical size, so a bigger world leaves it looking
      // smaller - correct, and not what anyone means by the slider. Scale the
      // span with the world so widening the view widens the country too; set
      // the span explicitly afterwards to pin it.
      const double ratio = extent / std::max(1.0, state.extentKilometres);
      state.extentKilometres = extent;
      state.continentSpanKilometres *= ratio;
    }

    float spacing = static_cast<float>(state.cellSpacingKilometres);
    if (ImGui::SliderFloat("cell spacing km", &spacing, 1.0f, 60.0f, "%.1f"))
      state.cellSpacingKilometres = spacing;
    const double cells = std::pow(state.extentKilometres / std::max(0.1, state.cellSpacingKilometres), 2.0);
    ImGui::TextDisabled("~%.0f cells", cells);

    // Land fraction and continent span both decide how much land there is, and
    // only one of them can be in charge. The span wins when it is set, and
    // saying so beats a slider that silently does nothing - which is exactly
    // how it read: raising the land fraction changed nothing at all.
    const bool bySpan = state.grownLandmasses && state.continentSpanKilometres > 0.0;
    ImGui::BeginDisabled(bySpan);
    float land = static_cast<float>(state.landFraction);
    if (ImGui::SliderFloat("land fraction", &land, 0.02f, 0.95f, "%.2f"))
      state.landFraction = land;
    ImGui::EndDisabled();
    if (bySpan)
      ImGui::TextDisabled("ignored: continent km is deciding the size");

    float complexity = static_cast<float>(state.coastComplexity);
    if (ImGui::SliderFloat("coast complexity", &complexity, 0.0f, 1.0f, "%.2f"))
      state.coastComplexity = complexity;

    float detail = static_cast<float>(state.coastDetail);
    if (ImGui::SliderFloat("coast detail", &detail, 0.0f, 1.0f, "%.2f"))
      state.coastDetail = detail;

    int cores = static_cast<int>(state.landmassCores);
    if (ImGui::SliderInt("landmass cores", &cores, 1, 12))
      state.landmassCores = static_cast<uint32_t>(cores);

    int levels = static_cast<int>(state.hierarchyLevels);
    if (ImGui::SliderInt("hierarchy levels", &levels, 1, 4))
      state.hierarchyLevels = static_cast<uint32_t>(levels);
    ImGui::TextDisabled("coarse cells are realms, fine cells are terrain");
    ImGui::Checkbox("grow landmasses", &state.grownLandmasses);
    ImGui::TextDisabled("off falls back to the analytic archetypes");
    float span = static_cast<float>(state.continentSpanKilometres);
    if (ImGui::SliderFloat("continent km", &span, 40.0f, 3000.0f, "%.0f"))
      state.continentSpanKilometres = span;
    ImGui::TextDisabled("France is ~950, Italy ~700; zero hands it back to");
    ImGui::TextDisabled("the land fraction");
    int continents = static_cast<int>(state.continentCount);
    if (ImGui::SliderInt("continents", &continents, 1, 6))
      state.continentCount = static_cast<uint32_t>(continents);

    int ranges = static_cast<int>(state.elevation.maximumRangesPerLandmass);
    if (ImGui::SliderInt("ranges per landmass", &ranges, 0, 6))
      state.elevation.maximumRangesPerLandmass = static_cast<uint32_t>(ranges);

    float relief = static_cast<float>(state.reliefMetres);
    if (ImGui::SliderFloat("relief m", &relief, 200.0f,
            static_cast<float>(kMaximumMountainElevationMetres), "%.0f"))
      state.reliefMetres = relief;
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("TERRAIN");
  ImGui::Separator();
  {
    float rangeStrength = static_cast<float>(state.elevation.rangeStrength);
    if (ImGui::SliderFloat("range strength", &rangeStrength, 0.0f, 1.0f, "%.2f"))
      state.elevation.rangeStrength = rangeStrength;
    float rangeWidth = static_cast<float>(state.elevation.rangeWidthMetres / 1000.0);
    if (ImGui::SliderFloat("range width km", &rangeWidth, 5.0f, 150.0f, "%.0f"))
      state.elevation.rangeWidthMetres = rangeWidth * 1000.0;
    float plain = static_cast<float>(state.elevation.coastalPlainMetres / 1000.0);
    if (ImGui::SliderFloat("coastal plain km", &plain, 5.0f, 200.0f, "%.0f"))
      state.elevation.coastalPlainMetres = plain * 1000.0;
    float landform =
        static_cast<float>(state.elevation.landformSpacingMetres / 1000.0);
    if (ImGui::SliderFloat(
            "landform province km", &landform, 20.0f, 500.0f, "%.0f"))
      state.elevation.landformSpacingMetres = landform * 1000.0;
    float landformContrast =
        static_cast<float>(state.elevation.landformContrast);
    if (ImGui::SliderFloat(
            "landform contrast", &landformContrast, 0.0f, 1.0f, "%.2f"))
      state.elevation.landformContrast = landformContrast;
    ImGui::TextDisabled("mixes plains, rolling country, plateaus and highlands");
    float cliffGradient =
        static_cast<float>(state.elevation.cliffMinimumGradient);
    if (ImGui::SliderFloat(
            "cliff gradient", &cliffGradient, 0.01f, 0.40f, "%.2f"))
      state.elevation.cliffMinimumGradient = cliffGradient;
    float planar = static_cast<float>(state.terrain.planarStrength);
    if (ImGui::SliderFloat(
            "planarization", &planar, 0.0f, 0.9f, "%.2f"))
      state.terrain.planarStrength = planar;
    float floodplain =
        static_cast<float>(state.terrain.floodplainWidthMetres / 1000.0);
    if (ImGui::SliderFloat(
            "floodplain km", &floodplain, 2.0f, 100.0f, "%.0f"))
      state.terrain.floodplainWidthMetres = floodplain * 1000.0;
    float floodplainStrength =
        static_cast<float>(state.terrain.floodplainStrength);
    if (ImGui::SliderFloat(
            "floodplain grading", &floodplainStrength, 0.0f, 1.0f, "%.2f"))
      state.terrain.floodplainStrength = floodplainStrength;
    ImGui::TextDisabled("major rivers grade broad, buildable valley floors");
    float uplift = static_cast<float>(state.elevation.upliftRateMetresPerYear * 1000.0);
    if (ImGui::SliderFloat("peak uplift mm/yr", &uplift, 0.0f, 10.0f, "%.2f"))
      state.elevation.upliftRateMetresPerYear = uplift / 1000.0;

    float runoff = static_cast<float>(state.rivers.runoffMetresPerYear);
    if (ImGui::SliderFloat("runoff m/yr", &runoff, 0.02f, 2.0f, "%.2f"))
      state.rivers.runoffMetresPerYear = runoff;
    float threshold = static_cast<float>(state.rivers.riverThresholdCells);
    if (ImGui::SliderFloat("channel threshold", &threshold, 1.0f, 40.0f, "%.1f"))
      state.rivers.riverThresholdCells = threshold;
    ImGui::TextDisabled("cells of catchment before a channel is drawn");
    float canyonGradient =
        static_cast<float>(state.rivers.canyonMinimumBankGradient);
    if (ImGui::SliderFloat(
            "canyon bank gradient", &canyonGradient, 0.01f, 0.30f, "%.2f"))
      state.rivers.canyonMinimumBankGradient = canyonGradient;
    float lake = static_cast<float>(state.lakes.minimumDepthMetres);
    if (ImGui::SliderFloat("lake depth m", &lake, 1.0f, 80.0f, "%.0f"))
      state.lakes.minimumDepthMetres = lake;
    float lakeArea = static_cast<float>(state.lakes.maximumAreaSquareMetres * 1.0e-6);
    if (ImGui::SliderFloat("max lake km^2", &lakeArea, 50.0f, 2000.0f, "%.0f"))
      state.lakes.maximumAreaSquareMetres = lakeArea * 1.0e6;
    ImGui::TextDisabled("wet basin cores are capped by area and catchment");

    float regionArea = static_cast<float>(state.regions.targetAreaSquareMetres * 1e-6);
    if (ImGui::SliderFloat("region km^2", &regionArea, 100.0f, 8000.0f, "%.0f"))
      state.regions.targetAreaSquareMetres = regionArea * 1e6;
    float ridge = static_cast<float>(state.regions.ridgeResistance);
    if (ImGui::SliderFloat("ridge resistance", &ridge, 0.0f, 12.0f, "%.1f"))
      state.regions.ridgeResistance = ridge;
    float river = static_cast<float>(state.regions.riverResistance);
    if (ImGui::SliderFloat("river resistance", &river, 0.0f, 8.0f, "%.1f"))
      state.regions.riverResistance = river;
    float hierarchy = static_cast<float>(state.regions.hierarchyCohesion);
    if (ImGui::SliderFloat("region hierarchy", &hierarchy, 0.0f, 1.0f, "%.2f"))
      state.regions.hierarchyCohesion = hierarchy;
    ImGui::TextDisabled("larger parent cells guide region silhouettes");
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("CLIMATE");
  ImGui::Separator();
  {
    float latitude = static_cast<float>(state.climate.southLatitudeDegrees);
    if (ImGui::SliderFloat("south latitude", &latitude, -60.0f, 70.0f, "%.0f"))
      state.climate.southLatitudeDegrees = latitude;
    float span = static_cast<float>(state.climate.latitudeSpanDegrees);
    if (ImGui::SliderFloat("latitude span", &span, 1.0f, 40.0f, "%.0f"))
      state.climate.latitudeSpanDegrees = span;
    float wind = static_cast<float>(state.climate.prevailingWindBearingDegrees);
    if (ImGui::SliderFloat("wind from", &wind, 0.0f, 360.0f, "%.0f"))
      state.climate.prevailingWindBearingDegrees = wind;
    float shadow = static_cast<float>(state.climate.rainShadowStrength);
    if (ImGui::SliderFloat("rain shadow", &shadow, 0.0f, 1.0f, "%.2f"))
      state.climate.rainShadowStrength = shadow;
    float reach = static_cast<float>(state.climate.coastalMoistureRangeMetres / 1000.0);
    if (ImGui::SliderFloat("moisture reach km", &reach, 20.0f, 900.0f, "%.0f"))
      state.climate.coastalMoistureRangeMetres = reach * 1000.0;
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("GENERATION");
  ImGui::Separator();
  double progress = 0.0;
  std::string phase;
  bool running = false;
  double saveProgress = 0.0;
  std::string savePhase;
  bool saving = false;
  {
    std::lock_guard<std::mutex> lock(state.generation.mutex);
    running = state.generation.running;
    progress = state.generation.progress;
    phase = state.generation.phase;
  }
  {
    std::lock_guard<std::mutex> lock(state.save.mutex);
    saving = state.save.running;
    saveProgress = state.save.progress;
    savePhase = state.save.phase;
  }
  if (running)
  {
    if (ImGui::Button("Cancel", ImVec2(-1.0f, 0.0f)))
      requestCancel(state);
    ImGui::ProgressBar(static_cast<float>(progress), ImVec2(-1.0f, 0.0f), phase.c_str());
  }
  else
  {
    ImGui::BeginDisabled(saving);
    if (ImGui::Button("Generate", ImVec2(-1.0f, 0.0f)))
      startGeneration(state);
    ImGui::EndDisabled();
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("SAVE");
  ImGui::Separator();
  ImGui::InputText(
      "PNG path", state.saveImagePath, sizeof(state.saveImagePath));
  ImGui::InputText("world-data directory", state.saveExportDirectory,
      sizeof(state.saveExportDirectory));
  if (ImGui::InputInt("PNG resolution", &state.saveImageResolution))
    state.saveImageResolution =
        std::clamp(state.saveImageResolution, 64, 8192);
  int exportResolutionIndex = 2;
  switch (state.saveExportResolution)
  {
  case 256: exportResolutionIndex = 0; break;
  case 512: exportResolutionIndex = 1; break;
  case 2048: exportResolutionIndex = 3; break;
  case 4096: exportResolutionIndex = 4; break;
  default: break;
  }
  if (ImGui::Combo("data resolution", &exportResolutionIndex,
          "256\0" "512\0" "1024\0" "2048\0" "4096\0"))
    state.saveExportResolution = 256 << exportResolutionIndex;
  const bool saveUnavailable = !state.generated || running || saving ||
      state.blendRebuildNeeded || state.saveImagePath[0] == '\0' ||
      state.saveExportDirectory[0] == '\0';
  ImGui::BeginDisabled(saveUnavailable);
  if (ImGui::Button("Save PNG + world data", ImVec2(-1.0f, 0.0f)))
    startSave(state);
  ImGui::EndDisabled();
  if (saving)
    ImGui::ProgressBar(static_cast<float>(saveProgress), ImVec2(-1.0f, 0.0f),
        savePhase.c_str());
  else if (state.blendRebuildNeeded)
    ImGui::TextDisabled("rebuild map geometry before saving");

  ImGui::Spacing();
  ImGui::TextUnformatted("DISPLAY");
  ImGui::Separator();
  ImGui::Checkbox("GPU fill", &state.useGpuFill);
  ImGui::SameLine();
  ImGui::BeginDisabled(!state.useGpuFill);
  ImGui::Checkbox("GPU lines", &state.useGpuLines);
  ImGui::EndDisabled();
  int residentLevel = static_cast<int>(
      state.shading.materialResidentLevel);
  if (ImGui::SliderInt("GPU material resident LOD", &residentLevel, 0, 2))
    state.shading.materialResidentLevel =
        static_cast<uint32_t>(residentLevel);
  ImGui::TextDisabled("0 forces root fallback; 2 exposes all 1024 pages");
  float sharpness = static_cast<float>(state.blend.sharpnessScale);
  const bool sharpnessChanged = ImGui::SliderFloat("region influence sharpness", &sharpness, 0.2f, 4.0f, "%.2f");
  if (sharpnessChanged)
  {
    state.blend.sharpnessScale = sharpness;
    state.blendRebuildNeeded = true;
  }
  int neighbours = static_cast<int>(state.blend.neighbourCount);
  const bool neighboursChanged = ImGui::SliderInt("blend neighbours", &neighbours, 1, 8);
  if (neighboursChanged)
  {
    state.blend.neighbourCount = static_cast<uint32_t>(neighbours);
    state.blendRebuildNeeded = true;
  }
  // Categorical blends are sampled into the mesh's material channels, so a
  // change to their weights needs the mesh rebuilt. Layer and relief controls
  // are shader-side and do not take this path.
  int layer = static_cast<int>(state.shading.layer);
  if (ImGui::Combo(
          "layer",
          &layer,
          "content\0landform\0elevation\0uplift\0biome\0heightmap\0"
          "region\0"))
    state.shading.layer = static_cast<worldgen::MapLayer>(layer);
  bool terrainBlend = !state.shading.flatCells;
  if (ImGui::Checkbox("terrain threshold field", &terrainBlend))
  {
    state.shading.flatCells = !terrainBlend;
    state.blendRebuildNeeded = true;
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("shore and shelf", &state.shading.shadeShoreAndShelf))
    state.blendRebuildNeeded = true;
  if (terrainBlend)
  {
    float cellWeight = static_cast<float>(
        state.shading.heightBlend.cellWeightStrength);
    if (ImGui::SliderFloat(
            "biome/region cell weight", &cellWeight, 0.0f, 1.5f, "%.2f"))
    {
      state.shading.heightBlend.cellWeightStrength = cellWeight;
      state.blendRebuildNeeded = true;
    }
    float heightInfluence = static_cast<float>(state.shading.heightBlend.influence);
    if (ImGui::SliderFloat("height threshold influence", &heightInfluence, 0.0f, 1.0f, "%.2f"))
    {
      state.shading.heightBlend.influence = heightInfluence;
      state.blendRebuildNeeded = true;
    }
    float heightThreshold = static_cast<float>(state.shading.heightBlend.highVariationFraction * 100.0);
    if (ImGui::SliderFloat("height threshold (% relief)", &heightThreshold, 2.0f, 20.0f, "%.1f%%"))
    {
      state.shading.heightBlend.highVariationFraction = heightThreshold * 0.01;
      state.shading.heightBlend.lowVariationFraction =
          state.shading.heightBlend.highVariationFraction * 0.125;
      state.blendRebuildNeeded = true;
    }
  }
  float boundaryNoise = static_cast<float>(state.shading.boundaryNoiseScale);
  if (ImGui::SliderFloat("land boundary noise", &boundaryNoise, 0.0f, 0.75f, "%.2f"))
  {
    state.shading.boundaryNoiseScale = boundaryNoise;
    state.blendRebuildNeeded = true;
  }
  if (state.blendRebuildNeeded)
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "map presentation changed - rebuild geometry");
  if (state.generated && !running && !saving &&
      ImGui::Button("Rebuild map geometry", ImVec2(-1.0f, 0.0f)))
  {
    const auto started = std::chrono::steady_clock::now();
    state.world.mesh = worldgen::buildMapMesh(state.world.graph, state.world.coastline, state.world.rivers, state.world.regions, state.world.contours, state.world.surface, state.world.index, state.blend, state.shading);
    state.world.meshSeconds = secondsSince(started);
    state.blendRebuildNeeded = false;
    state.gpuUploadNeeded = true;
  }
  ImGui::Checkbox("coastline", &state.overlay.showCoastline);
  ImGui::SameLine();
  ImGui::Checkbox("rivers", &state.overlay.showRivers);
  if (state.shading.layer == worldgen::MapLayer::Elevation || state.shading.layer == worldgen::MapLayer::Heightmap)
  {
    float displayRelief = static_cast<float>(state.shading.reliefMetres);
    if (ImGui::SliderFloat("display relief m", &displayRelief, 200.0f,
            static_cast<float>(kMaximumMountainElevationMetres), "%.0f"))
      state.shading.reliefMetres = displayRelief;
  }
  if (state.shading.layer == worldgen::MapLayer::Heightmap)
  {
    float bearing = static_cast<float>(state.shading.sunBearingDegrees);
    if (ImGui::SliderFloat("sun bearing", &bearing, 0.0f, 360.0f, "%.0f"))
      state.shading.sunBearingDegrees = bearing;
    float altitude = static_cast<float>(state.shading.sunAltitudeDegrees);
    if (ImGui::SliderFloat("sun altitude", &altitude, 5.0f, 85.0f, "%.0f"))
      state.shading.sunAltitudeDegrees = altitude;
    float exaggeration = static_cast<float>(state.shading.verticalExaggeration);
    if (ImGui::SliderFloat("relief scale", &exaggeration, 0.5f, 10.0f, "%.1f"))
      state.shading.verticalExaggeration = exaggeration;
  }
  ImGui::Checkbox("labels", &state.overlay.showLabels);
  ImGui::SameLine();
  ImGui::Checkbox("contours", &state.overlay.showContours);
  ImGui::Checkbox("cell edges", &state.overlay.showCellEdges);
  ImGui::SameLine();
  ImGui::Checkbox("world border", &state.overlay.showWorldBorder);
  ImGui::SliderFloat("river width", &state.overlay.riverWidthScale, 1.0f, 14.0f, "%.1f");
  ImGui::SliderFloat("contour width", &state.overlay.contourThickness, 0.25f, 3.0f, "%.2f");
  ImGui::SliderFloat("index contour width", &state.overlay.majorContourThickness, 0.5f, 5.0f, "%.2f");
  if (state.overlay.showCoastline)
    ImGui::SliderFloat("coast width", &state.overlay.coastlineThickness, 0.5f, 6.0f, "%.2f");
  if (ImGui::Button("Fit view", ImVec2(-1.0f, 0.0f)))
    state.viewInitialized = false;

  ImGui::Spacing();
  ImGui::TextUnformatted("STATS");
  ImGui::Separator();
  if (state.generated)
  {
    const WorldResult &world = state.world;
    ImGui::Text("cells        %zu (%u leaves, %u levels)", world.graph.nodeCount(), world.leafCells, world.hierarchyLevels);
    ImGui::Text("land area    %.0f km^2", world.landAreaSquareMetres * 1e-6);
    ImGui::Text("land         %u (%.1f%%)", world.landCells, 100.0 * static_cast<double>(world.landCells) / std::max<size_t>(1u, world.graph.nodeCount()));
    ImGui::Text("border land  %.3f", world.borderLand);
    ImGui::Text("lakes        %u cells, %u filled", world.lakeCells, world.lakesFilled);
    ImGui::Text("coast runs   %zu", world.coastline.size());
    ImGui::Text("contours     %zu (%zu points)", world.contours.lines.size(), world.contours.pointCount());
    ImGui::Text("relief       %.0f to %.0f m", world.deepestMetres, world.highestMetres);
    ImGui::Text("rivers       %u cells, %u mouths", world.rivers.riverCells, world.rivers.mouths);
    ImGui::Text("largest      %.1f cumecs, order %u", world.rivers.largestDischargeCumecs, world.rivers.maximumOrder);
    ImGui::Text("filled pits  %u", world.rivers.filledCells);
    ImGui::Text("waterfalls   %u", world.waterfalls);
    ImGui::Text("landforms    %u mountains, %u cliffs, %u canyons",
        world.mountainCells, world.cliffCells, world.canyonCells);
    ImGui::Text("snow cover  %u cells", world.glacierCells);
    ImGui::Text("conditioned  %u planar, %u floodplain",
        world.terrainConditioning.planarCells,
        world.terrainConditioning.floodplainCells);
    ImGui::Text("bad borders  %u", world.incompatibleBorders);
    ImGui::Text("regions      %zu, named rivers %zu", world.regions.regions.size(), world.regions.rivers.size());
    ImGui::Text("triangles    %zu", world.mesh.triangleCount);
    ImGui::Separator();
    ImGui::Text("requested    %s", tagName(world.requestedShape));
    ImGui::Text("measured     %s", tagName(world.measuredShapeTag));
    if (expectedMeasuredShape(world.requestedShape) != world.measuredShapeTag)
      ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "the map does not read as the family asked for");
    ImGui::TextDisabled(
        "the classifier reads border water; it names only island,\n"
        "coast, lake and landlocked");
    ImGui::Separator();
    ImGui::Text("partition    %.3f s", world.partitionSeconds);
    ImGui::Text("raise        %.3f s", world.raiseSeconds);
    ImGui::Text("coast        %.3f s", world.coastSeconds);
    ImGui::Text("elevation    %.3f s", world.elevationSeconds);
    ImGui::Text("rivers       %.3f s", world.riverSeconds);
    ImGui::Text("regions      %.3f s", world.regionSeconds);
    ImGui::Text("blend + mesh %.3f s", world.meshSeconds);
    ImGui::Text("GPU upload   %.3f ms (%zu vertices, %zu indices, %zu lines)", state.gpuUploadSeconds * 1000.0, state.gpuVertexCount, state.gpuIndexCount, state.gpuLineInstanceCount);
    ImGui::Text("GPU pages    %zu (%0.1f MiB, resident LOD %u)",
        state.gpuMaterialPageCount,
        state.gpuMaterialPageBytes / (1024.0 * 1024.0),
        state.shading.materialResidentLevel);
  }
  else
    ImGui::TextDisabled("nothing generated yet");

  ImGui::End();
}

void drawInspector(ApplicationState &state)
{
  if (!state.generated || state.selectedNode == kInvalidNode || state.selectedNode >= state.world.graph.nodeCount())
  {
    ImGui::TextDisabled("Click the map to select a cell.");
    return;
  }
  const MapNode &node = state.world.graph.node(state.selectedNode);
  ImGui::Text("id %u   level %u   %s / %s", node.id, static_cast<uint32_t>(node.level), cellTypeName(node.type), cellTypeName(node.content));
  if (node.region != kInvalidNode && node.region < state.world.regions.regions.size())
  {
    const MapRegion &region = state.world.regions.regions[node.region];
    ImGui::Text("region  %s", region.name.c_str());
    ImGui::Text("        %zu cells, %.0f km^2, relief %.0f m", region.cells.size(), region.areaSquareMetres * 1e-6, region.reliefMetres);
  }
  ImGui::SameLine();
  ImGui::Text("  %s%s%s", node.land ? "land" : "water", node.coastal ? " coastal" : "", node.shallow ? " shallow" : "");
  ImGui::Text("site   %.0f, %.0f m", node.site.x, node.site.y);
  ImGui::Text("corners %zu   neighbours %zu", node.area.size(), node.neighbours.size());
  ImGui::Text(
      "intent  elevation %.0f m  uplift %.3f mm/yr  relief %.0f m  "
      "roughness %.2f",
      node.intent.elevationMetres,
      node.intent.upliftRate * 1000.0,
      node.intent.reliefAmplitude,
      node.intent.roughness);
  if (state.selectedNode < state.world.rivers.nodes.size())
  {
    const RiverNode &river = state.world.rivers.nodes[state.selectedNode];
    ImGui::Text("drainage %.1f km^2   discharge %.2f cumecs", river.drainageAreaSquareMetres * 1e-6, river.dischargeCumecs);
    if (river.river)
      ImGui::Text("channel  order %u, drains into cell %u", river.strahlerOrder, river.downstream);
    else
      ImGui::TextDisabled("not a channel at the current threshold");
    if (river.filledElevationMetres > node.intent.elevationMetres + 1e-9)
      ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "sits in a filled depression, water surface %.0f m", river.filledElevationMetres);
  }
  // Refine and Regenerate are phase 6; the fields they act on are shown now
  // because they are what says whether the layout stage did anything.
  ImGui::TextDisabled("Refine / regenerate arrive with the hierarchy (phase 6).");
}

void drawMapPanel(ApplicationState &state, const float panelWidth, const bool gpuFill, const bool gpuLines)
{
  const ImGuiIO &io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(panelWidth, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x - panelWidth, io.DisplaySize.y));
  ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
  if (gpuFill)
    windowFlags |= ImGuiWindowFlags_NoBackground;
  ImGui::Begin("Map", nullptr, windowFlags);

  const float inspectorHeight = 190.0f * io.FontGlobalScale;
  const ImVec2 available = ImGui::GetContentRegionAvail();
  const ImVec2 mapSize(available.x, std::max(64.0f, available.y - inspectorHeight));
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  state.mapOrigin = origin;
  state.mapSize = mapSize;

  ImGui::InvisibleButton("mapSurface", mapSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hovered = ImGui::IsItemHovered();

  if (state.generated && !state.viewInitialized)
  {
    state.view = worldgen::fitView(state.world.extentMetres, mapSize.x, mapSize.y);
    state.viewInitialized = true;
  }

  if (hovered && io.MouseWheel != 0.0f)
  {
    // Zoom about the cursor, so the feature under the pointer stays under it.
    const Point2 before = worldgen::screenToWorld(state.view, origin, mapSize, io.MousePos);
    state.view.metresPerPixel = std::clamp(state.view.metresPerPixel * std::pow(1.15, -io.MouseWheel), 1.0, std::max(2.0, state.world.extentMetres));
    const Point2 after = worldgen::screenToWorld(state.view, origin, mapSize, io.MousePos);
    state.view.centreX += before.x - after.x;
    state.view.centreY += before.y - after.y;
  }
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right))
  {
    const ImVec2 delta = io.MouseDelta;
    state.view.centreX -= delta.x * state.view.metresPerPixel;
    state.view.centreY += delta.y * state.view.metresPerPixel;
  }
  if (state.generated && ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 16.0f)
  {
    const Point2 world = worldgen::screenToWorld(state.view, origin, mapSize, io.MousePos);
    state.selectedNode = state.world.graph.nodeAt(world);
  }

  ImDrawList *drawList = ImGui::GetWindowDrawList();
  drawList->PushClipRect(origin, ImVec2(origin.x + mapSize.x, origin.y + mapSize.y), true);
  if (state.generated)
  {
    worldgen::drawMapMesh(drawList, state.world.mesh, state.view, origin, mapSize, state.shading, state.overlay, state.world.extentMetres, !gpuFill, gpuLines);
    if (state.selectedNode != kInvalidNode && state.selectedNode < state.world.graph.nodeCount())
    {
      const MapNode &node = state.world.graph.node(state.selectedNode);
      std::vector<ImVec2> outline;
      outline.reserve(node.area.size());
      for (const Point2 &point : node.area)
        outline.push_back(ImVec2(
            origin.x + mapSize.x * 0.5f + static_cast<float>((point.x - state.view.centreX) / state.view.metresPerPixel),
            origin.y + mapSize.y * 0.5f - static_cast<float>((point.y - state.view.centreY) / state.view.metresPerPixel)));
      if (outline.size() >= 2u)
        drawList->AddPolyline(outline.data(), static_cast<int>(outline.size()), IM_COL32(255, 220, 120, 255), ImDrawFlags_Closed, 2.0f);
    }
  }
  else
    drawList->AddText(ImVec2(origin.x + 24.0f, origin.y + 24.0f), IM_COL32(180, 180, 190, 255), "Press Generate.");
  drawList->PopClipRect();

  ImGui::Separator();
  ImGui::BeginChild("inspector", ImVec2(0.0f, 0.0f));
  ImGui::TextUnformatted("NODE INSPECTOR");
  ImGui::Separator();
  drawInspector(state);
  ImGui::Separator();
  ImGui::TextWrapped("%s", state.status.c_str());
  ImGui::TextDisabled("right drag pans, wheel zooms, left click selects a cell");
  ImGui::EndChild();

  ImGui::End();
}

// --------------------------------------------------------------------------
// Headless
// --------------------------------------------------------------------------

bool parseTag(const std::string &name, Tag &tag)
{
  std::string lowered;
  lowered.reserve(name.size());
  for (const char character : name)
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  for (uint8_t candidate = 0u; candidate < static_cast<uint8_t>(Tag::Count); ++candidate)
  {
    std::string canonical = tagName(static_cast<Tag>(candidate));
    for (char &character : canonical)
      character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    if (canonical == lowered)
    {
      tag = static_cast<Tag>(candidate);
      return true;
    }
  }
  return false;
}

void printUsage()
{
  std::cout << "World generator\n"
               "  --headless                generate and write a PNG, no window\n"
               "  --output <path.png>       headless image path\n"
               "  --resolution <pixels>     headless image side, default 1024\n"
               "  --export <directory>      write the registered world-data bundle\n"
               "  --export-resolution <n>  bundle field side, default 1024\n"
               "  --tags a,b,c              brief tags, e.g. island,verdant\n"
               "  --seed <n>\n"
               "  --traverse-hours <h>     derive game extent from travel time\n"
               "  --traverse-speed <km/h>  default 6\n"
               "  --traverse-detour <x>    default 5\n"
               "  --extent <km>\n"
               "  --spacing <km>            cell spacing\n"
               "  --land <fraction>\n"
               "  --coast-complexity <0..1>\n"
               "  --coast-detail <0..1>     fine coastal structure\n"
               "  --cores <n>               landmass cores\n"
               "  --levels <1..4>           hierarchy levels\n"
               "  --grow <0|1>              grow landmasses over the coarse cells\n"
               "  --continent-span <km>     target continent width (France ~950)\n"
               "  --continents <n>          how many\n"
               "  --latitude <deg>          southern edge of the world\n"
               "  --latitude-span <deg>\n"
               "  --wind <bearing>          where the weather comes from\n"
               "  --ranges <n>              mountain ranges per landmass\n"
               "  --sharpness <scale>       blend sharpness\n"
               "  --shore-shading <0|1>     tint coastal land and shallow water\n"
               "  --layer <content|landform|elevation|uplift|biome|heightmap|"
               "region>\n"
               "  --rivers <0|1>            draw the river overlay\n"
               "  --contours <0|1>          draw elevation contours\n"
               "  --channel-threshold <n>   cells of catchment before a channel\n"
               "  --lake-depth <m>          minimum permanent-water depth\n"
               "  --lake-area <km^2>        maximum generated lake area\n"
               "  --range-strength <0..1>\n"
               "  --range-width <km>\n"
               "  --relief <m>              datum to highest summit\n";
}

int runHeadless(
    ApplicationState &state,
    const std::string &outputPath,
    const uint32_t resolution,
    const std::string &exportDirectory,
    const uint32_t exportResolution)
{
  GenerationJob job;
  runGeneration(job, pipelineSettingsFromState(state), state.shading);
  if (!job.success)
  {
    std::cerr << "Generation failed: " << job.error << "\n";
    return 1;
  }
  const WorldResult &world = job.result;
  const size_t closedContours = static_cast<size_t>(std::count_if(
      world.contours.lines.begin(),
      world.contours.lines.end(),
      [](const ContourPolyline &line)
      {
        return line.closed;
      }));
  std::cout << std::hex << "signature   " << world.graph.signature() << std::dec
            << "\n"
            << "cells       " << world.graph.nodeCount() << "\n"
            << "land        " << world.landCells << "\n"
            << "border land " << world.borderLand << "\n"
            << "coast runs  " << world.coastline.size() << "\n"
            << "contours    " << world.contours.lines.size() << " lines, " << closedContours << " closed, " << world.contours.pointCount() << " points\n"
            << "requested   " << tagName(world.requestedShape) << "\n"
            << "measured    " << tagName(world.measuredShapeTag) << " (expected " << tagName(expectedMeasuredShape(world.requestedShape)) << ")\n"
            << "land area   " << world.landAreaSquareMetres * 1e-6 << " km^2\n"
            << "lakes       " << world.lakeCells << " cells, " << world.lakesFilled << " basin-filled\n"
            << "relief      " << world.deepestMetres << " to " << world.highestMetres << " m\n"
            << "rivers      " << world.rivers.riverCells << " cells, " << world.rivers.mouths << " mouths, max order " << world.rivers.maximumOrder << "\n"
            << "largest     " << world.rivers.largestDischargeCumecs << " cumecs\n"
            << "filled pits " << world.rivers.filledCells << "\n"
            << "waterfalls  " << world.waterfalls << "\n"
            << "landforms   " << world.mountainCells << " mountains, "
            << world.cliffCells << " cliffs, " << world.canyonCells
            << " canyons\n"
            << "snow cover  " << world.glacierCells << " cells\n"
            << "conditioned " << world.terrainConditioning.planarCells
            << " planar, " << world.terrainConditioning.floodplainCells
            << " floodplain\n"
            << "bad borders " << world.incompatibleBorders << "\n"
            << "elapsed     " << job.elapsedSeconds << " s\n";

  std::array<uint32_t, static_cast<size_t>(Biome::Count)> biomeCounts{};
  for (const MapNode &node : world.graph.nodes())
    if (!node.refined())
      ++biomeCounts[static_cast<size_t>(biomeOf(node))];
  std::cout << "biomes      ";
  bool firstBiome = true;
  for (size_t biome = 0u; biome < biomeCounts.size(); ++biome)
  {
    if (biomeCounts[biome] == 0u)
      continue;
    if (!firstBiome)
      std::cout << ", ";
    std::cout << biomeName(static_cast<Biome>(biome)) << " "
              << biomeCounts[biome];
    firstBiome = false;
  }
  std::cout << "\n";

  for (const IncompatiblePair &pair : incompatibleBorderPairs(world.graph))
    std::cout << "  bad pair  " << biomeName(pair.first) << " | " << biomeName(pair.second) << "  x" << pair.count << "\n";

  const auto started = std::chrono::steady_clock::now();
  worldgen::MapShading shading = state.shading;
  shading.seaLevelMetres = state.seaLevelMetres;
  shading.reliefMetres = state.reliefMetres;
  shading.peakUpliftMetresPerYear = state.elevation.upliftRateMetresPerYear;
  const std::vector<uint8_t> rgba = worldgen::renderMapImage(world.graph, world.coastline, world.rivers, world.mesh, world.surface, world.index, state.blend, shading, resolution, resolution, world.extentMetres, state.overlay);
  std::string error;
  if (!worldgen::writePng(outputPath, resolution, resolution, rgba, error))
  {
    std::cerr << error << "\n";
    return 1;
  }
  std::cout << "wrote " << outputPath << " in " << secondsSince(started) << " s\n";

  if (!exportDirectory.empty())
  {
    const auto exportStarted = std::chrono::steady_clock::now();
    MapExportSettings exportSettings{};
    exportSettings.resolution = exportResolution;
    MapExportBundle bundle;
    if (!buildMapExportBundle(world.graph, world.index, world.surface,
            world.climate, world.rivers, world.coastline, world.contours,
            world.regions, state.blend, shading.heightBlend, exportSettings,
            bundle, &error))
    {
      std::cerr << "Export failed: " << error << "\n";
      return 1;
    }
    if (!writeMapExportBundle(bundle, exportDirectory, &error))
    {
      std::cerr << "Export failed: " << error << "\n";
      return 1;
    }
    MapExportBundle verified;
    if (!readMapExportBundle(exportDirectory, verified, &error) ||
        verified.bundleSignature != bundle.bundleSignature)
    {
      std::cerr << "Export verification failed: " << error << "\n";
      return 1;
    }
    MapGraph verifiedGraph;
    MapExportFeatures verifiedFeatures;
    if (!decodeMapExportGraph(verified, verifiedGraph, &error) ||
        !decodeMapExportFeatures(verified, verifiedFeatures, &error) ||
        verifiedGraph.signature() != world.graph.signature() ||
        verifiedFeatures.contours.lines.size() != world.contours.lines.size())
    {
      std::cerr << "Export semantic verification failed: " << error << "\n";
      return 1;
    }
    const size_t available = static_cast<size_t>(std::count_if(
        bundle.fields.begin(), bundle.fields.end(),
        [](const MapExportField &field) { return field.available; }));
    size_t pageCount = 0u;
    size_t compressed = 0u;
    size_t rawBytes = bundle.graphData.size() + bundle.featureData.size();
    size_t pagedBytes = bundle.graphData.size() + bundle.featureData.size();
    size_t storedBytes = rawBytes + bundle.manifestJson.size();
    for (const MapExportField &field : bundle.fields)
      if (field.available)
      {
        rawBytes += field.data.size();
        for (const MapExportPage &page : field.pages)
        {
          ++pageCount;
          compressed +=
              page.compression == MapExportCompression::Lz4 ? 1u : 0u;
          pagedBytes += page.data.size();
          storedBytes += page.compression == MapExportCompression::None
              ? page.data.size()
              : page.storedData.size();
        }
      }
    std::cout << "exported    " << exportDirectory << " (" << available
              << " available, " << bundle.fields.size() - available
              << " reserved fields, " << pageCount << " pages/"
              << compressed << " LZ4, "
              << std::fixed << std::setprecision(1)
              << static_cast<double>(storedBytes) / (1024.0 * 1024.0)
              << " MiB stored from "
              << static_cast<double>(pagedBytes) / (1024.0 * 1024.0)
              << " MiB paged/"
              << static_cast<double>(rawBytes) / (1024.0 * 1024.0)
              << " MiB full-res, verified signature " << std::hex
              << bundle.bundleSignature << std::dec << ") in "
              << secondsSince(exportStarted) << " s\n";
  }
  return expectedMeasuredShape(world.requestedShape) == world.measuredShapeTag ? 0 : 2;
}

} // namespace

int main(const int argc, char **argv)
{
  ApplicationState state{};
  state.tagSelected[static_cast<size_t>(Tag::Coast)] = true;
  state.tagSelected[static_cast<size_t>(Tag::Highland)] = true;

  bool headless = false;
  bool traverseSpecified = false;
  bool extentSpecified = false;
  bool spacingSpecified = false;
  double traverseHours = 0.0;
  double traverseSpeedKilometresPerHour = 6.0;
  double traverseDetourFactor = 5.0;
  std::string outputPath = "world-map.png";
  std::string exportDirectory;
  uint32_t resolution = 1024u;
  uint32_t exportResolution = 1024u;
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
      headless = true;
      continue;
    }
    if (argument + 1 >= argc)
    {
      printUsage();
      return 1;
    }
    const std::string value = argv[++argument];
    if (option == "--output")
      outputPath = value;
    else if (option == "--resolution")
      resolution = static_cast<uint32_t>(std::clamp(std::atoi(value.c_str()), 16, 8192));
    else if (option == "--export")
      exportDirectory = value;
    else if (option == "--export-resolution")
      exportResolution = static_cast<uint32_t>(std::clamp(std::atoi(value.c_str()), 2, 4096));
    else if (option == "--seed")
      state.seed = std::strtoull(value.c_str(), nullptr, 10);
    else if (option == "--traverse-hours")
    {
      traverseHours = std::atof(value.c_str());
      traverseSpecified = true;
    }
    else if (option == "--traverse-speed")
      traverseSpeedKilometresPerHour = std::atof(value.c_str());
    else if (option == "--traverse-detour")
      traverseDetourFactor = std::atof(value.c_str());
    else if (option == "--extent")
    {
      state.extentKilometres = std::atof(value.c_str());
      extentSpecified = true;
    }
    else if (option == "--spacing")
    {
      state.cellSpacingKilometres = std::atof(value.c_str());
      spacingSpecified = true;
    }
    else if (option == "--land")
      state.landFraction = std::atof(value.c_str());
    else if (option == "--coast-complexity")
      state.coastComplexity = std::atof(value.c_str());
    else if (option == "--coast-detail")
      state.coastDetail = std::atof(value.c_str());
    else if (option == "--craton-spread")
      state.cratonSpread = std::atof(value.c_str());
    else if (option == "--continents")
      state.continentCount = static_cast<uint32_t>(std::clamp(std::atoi(value.c_str()), 1, 8));
    else if (option == "--continent-span")
      state.continentSpanKilometres = std::atof(value.c_str());
    else if (option == "--latitude")
      state.climate.southLatitudeDegrees = std::atof(value.c_str());
    else if (option == "--latitude-span")
      state.climate.latitudeSpanDegrees = std::atof(value.c_str());
    else if (option == "--wind")
      state.climate.prevailingWindBearingDegrees = std::atof(value.c_str());
    else if (option == "--grow")
      state.grownLandmasses = std::atoi(value.c_str()) != 0;
    else if (option == "--ranges")
      state.elevation.maximumRangesPerLandmass = static_cast<uint32_t>(std::clamp(std::atoi(value.c_str()), 0, 8));
    else if (option == "--levels")
      state.hierarchyLevels = static_cast<uint32_t>(std::clamp(std::atoi(value.c_str()), 1, 4));
    else if (option == "--cores")
      state.landmassCores = static_cast<uint32_t>(std::max(1, std::atoi(value.c_str())));
    else if (option == "--sharpness")
      state.blend.sharpnessScale = std::atof(value.c_str());
    else if (option == "--shore-shading")
      state.shading.shadeShoreAndShelf = std::atoi(value.c_str()) != 0;
    else if (option == "--layer")
    {
      if (value == "content")
        state.shading.layer = worldgen::MapLayer::Content;
      else if (value == "landform")
        state.shading.layer = worldgen::MapLayer::Landform;
      else if (value == "region")
        state.shading.layer = worldgen::MapLayer::Region;
      else if (value == "heightmap")
        state.shading.layer = worldgen::MapLayer::Heightmap;
      else if (value == "biome" || value == "landtype")
        state.shading.layer = worldgen::MapLayer::Biome;
      else if (value == "elevation")
        state.shading.layer = worldgen::MapLayer::Elevation;
      else if (value == "uplift")
        state.shading.layer = worldgen::MapLayer::Uplift;
      else
      {
        std::cerr << "unknown layer: " << value << "\n";
        return 1;
      }
    }
    else if (option == "--rivers")
      state.overlay.showRivers = std::atoi(value.c_str()) != 0;
    else if (option == "--contours")
      state.overlay.showContours = std::atoi(value.c_str()) != 0;
    else if (option == "--lake-depth")
      state.lakes.minimumDepthMetres = std::atof(value.c_str());
    else if (option == "--lake-area")
      state.lakes.maximumAreaSquareMetres = std::atof(value.c_str()) * 1.0e6;
    else if (option == "--channel-threshold")
      state.rivers.riverThresholdCells = std::atof(value.c_str());
    else if (option == "--range-strength")
      state.elevation.rangeStrength = std::atof(value.c_str());
    else if (option == "--range-width")
      state.elevation.rangeWidthMetres = std::atof(value.c_str()) * 1000.0;
    else if (option == "--relief")
      state.reliefMetres = std::clamp(std::atof(value.c_str()), 200.0,
          kMaximumMountainElevationMetres);
    else if (option == "--tags")
    {
      state.tagSelected.fill(false);
      std::stringstream stream(value);
      std::string name;
      while (std::getline(stream, name, ','))
      {
        Tag tag = Tag::Island;
        if (!parseTag(name, tag))
        {
          std::cerr << "unknown tag: " << name << "\n";
          return 1;
        }
        state.tagSelected[static_cast<size_t>(tag)] = true;
      }
    }
    else
    {
      printUsage();
      return 1;
    }
  }

  if (traverseSpecified)
  {
    if (!(traverseHours > 0.0) || !std::isfinite(traverseHours) ||
        !(traverseSpeedKilometresPerHour > 0.0) ||
        !std::isfinite(traverseSpeedKilometresPerHour) ||
        !(traverseDetourFactor > 0.0) ||
        !std::isfinite(traverseDetourFactor))
    {
      std::cerr << "traverse hours, speed and detour must be finite and positive\n";
      return 1;
    }
    const double derivedExtent = traverseHours *
        traverseSpeedKilometresPerHour * traverseDetourFactor;
    if (!extentSpecified)
      state.extentKilometres = derivedExtent;
    if (!spacingSpecified)
      state.cellSpacingKilometres = state.extentKilometres / 180.0;
  }

  if (state.rescaledExtent != state.extentKilometres)
    rescaleForExtent(state);

  if (!exportDirectory.empty())
    headless = true;
  if (headless)
    return runHeadless(state, outputPath, resolution, exportDirectory,
        exportResolution);

  os::Logger::setIdleSleep(lib::time::TimeSpan::fromMilliseconds(10));
  os::Logger::start();

  auto window = std::make_unique<window::sdl3::SDL3Window>(window::WindowSurface_Vulkan, "World Generator", 1720, 1080);
  if (!window->isRunning || window->sdlWindow == nullptr)
  {
    std::cerr << "Could not create the world generator window: " << SDL_GetError() << "\n";
    os::Logger::shutdown();
    return 1;
  }

  const gpu::DeviceRequiredLimits limits{
    .minimumMemory = 0u,
    .minimumComputeSharedMemory = 0u,
    .minimumComputeWorkGroupInvocations = 64u,
  };
  worldgen::WorldMapRHI rhi(vkbackend::Vulkan_1_2, limits, gpu::DeviceFeatures_Compute, std::vector<std::string>{});
  std::vector<VkSurfaceKHR> surfaces{window->getVulkanSurface(rhi.getInstance())};
  rhi.init(surfaces);
  rhi.setSwapChainVSyncEnabled(true);
  gpu::SwapChain swapChain = rhi.createSwapChain(0u, window->getWidth(), window->getHeight());

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.FontGlobalScale = 1.35f;
  ImGui::GetStyle().ScaleAllSizes(1.20f);
  if (!ImGui_ImplSDL3_InitForVulkan(window->sdlWindow))
    throw std::runtime_error("Failed to initialize the ImGui SDL3 backend");

  worldgen::WorldMapOverlayPass overlayPass(rhi);
  overlayPass.initialize(swapChain);
  worldgen::WorldMapGpuPass gpuPass(rhi, WORLD_GENERATOR_MAP_SHADER_PATH);
  gpuPass.initialize(overlayPass.renderPass());

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
  imguiInfo.PipelineInfoMain.RenderPass = overlayPass.renderPass();
  imguiInfo.PipelineInfoMain.Subpass = 0u;
  imguiInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  imguiInfo.CheckVkResultFn = worldgen::WorldMapOverlayPass::checkVkResult;
  if (!ImGui_ImplVulkan_Init(&imguiInfo))
    throw std::runtime_error("Failed to initialize the ImGui Vulkan backend");

  startGeneration(state);

  bool running = true;
  while (running)
  {
    SDL_Event event{};
    while (SDL_PollEvent(&event))
    {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window->sdlWindow)))
        running = false;
      if (event.type == SDL_EVENT_WINDOW_RESIZED && event.window.windowID == SDL_GetWindowID(window->sdlWindow))
      {
        window->width = static_cast<uint32_t>(std::max(1, event.window.data1));
        window->height = static_cast<uint32_t>(std::max(1, event.window.data2));
        rhi.waitIdle();
        overlayPass.destroyFramebuffers();
        rhi.destroySwapChain(swapChain);
        swapChain = rhi.createSwapChain(0u, window->getWidth(), window->getHeight());
        ImGui_ImplVulkan_SetMinImageCount(std::max(2u, rhi.getSwapChainImagesCount(swapChain)));
        overlayPass.onSwapChainRecreated(swapChain);
      }
    }
    if (SDL_GetWindowFlags(window->sdlWindow) & SDL_WINDOW_MINIMIZED)
    {
      SDL_Delay(10);
      continue;
    }

    pollGeneration(state);
    pollSave(state);
    if (state.generated && state.gpuUploadNeeded)
    {
      const auto started = std::chrono::steady_clock::now();
      std::string error;
      if (!gpuPass.upload(state.world.mesh, &error))
        state.status = "GPU map upload failed; using CPU renderer: " + error;
      else
      {
        state.gpuUploadSeconds = secondsSince(started);
        state.gpuVertexCount = gpuPass.vertexCount();
        state.gpuIndexCount = gpuPass.indexCount();
        state.gpuLineInstanceCount = gpuPass.lineInstanceCount();
        state.gpuMaterialPageCount = gpuPass.materialPageCount();
        state.gpuMaterialPageBytes = gpuPass.materialPageBytes();
        std::cout << "Uploaded GPU map: " << state.gpuVertexCount << " vertices, " << state.gpuIndexCount << " indices, " << state.gpuLineInstanceCount << " line instances, " << state.gpuMaterialPageCount << " material pages (" << state.gpuMaterialPageBytes / (1024.0 * 1024.0) << " MiB) in " << state.gpuUploadSeconds * 1000.0 << " ms\n";
      }
      state.gpuUploadNeeded = false;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const float panelWidth = std::min(460.0f * io.FontGlobalScale, io.DisplaySize.x * 0.42f);
    drawControls(state, panelWidth);
    const bool gpuFill = state.useGpuFill && gpuPass.ready();
    const bool gpuLines = gpuFill && state.useGpuLines && gpuPass.linesReady();
    drawMapPanel(state, panelWidth, gpuFill, gpuLines);
    ImGui::Render();

    overlayPass.render(
        swapChain,
        rhi.getCurrentSwapChainTextureView(swapChain),
        ImGui::GetDrawData(),
        [&gpuPass, &state, gpuFill, gpuLines](const VkCommandBuffer commandBuffer, const VkExtent2D extent)
        {
          if (state.generated && (gpuFill || gpuLines))
            gpuPass.render(commandBuffer, extent, state.view, state.mapOrigin, state.mapSize, state.shading, state.overlay, gpuFill, gpuLines);
        });
  }

  requestCancel(state);
  if (state.generation.worker.joinable())
    state.generation.worker.join();
  if (state.save.worker.joinable())
    state.save.worker.join();
  rhi.waitIdle();
  gpuPass.destroy();
  overlayPass.destroy();
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  rhi.destroySwapChain(swapChain);
  os::Logger::shutdown();
  return 0;
}
