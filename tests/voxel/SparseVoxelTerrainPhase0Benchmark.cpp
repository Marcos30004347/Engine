#include "TerrainGenerationPhase0Utils.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace svr = voxel::svraster;

namespace
{
struct PageRecord
{
  std::string fixture;
  uint32_t lod = 0u;
  svr::WorldMapVoxelGenerationMetrics generation{};
  svr::SparseVoxelTerrainPageTree64 tree64{};
  uint64_t semanticHash = 0u;
  uint64_t emittedRuns = 0u;
  uint64_t payloadBytes = 0u;
};

struct ChurnRecord
{
  std::string event;
  double cameraX = 0.0;
  double cameraZ = 0.0;
  double requestToVisibleMilliseconds = 0.0;
  double updateMilliseconds = 0.0;
  uint32_t updates = 0u;
  uint32_t builtPages = 0u;
  uint32_t reusedPages = 0u;
  uint32_t evictedPages = 0u;
  uint32_t pendingPages = 0u;
  uint64_t uploadBytes = 0u;
  bool visible = false;
};

double millisecondsSince(const std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

bool cacheCovers(const svr::WorldVoxelChunkCache &cache, const svr::WorldVoxelChunkStreamingSettings &settings, const procedural::world::MapExportBundle &bundle, const double worldX, const double worldZ)
{
  const double mapX = worldX + bundle.extentMetres * 0.5;
  const double mapZ = worldZ + bundle.extentMetres * 0.5;
  for (const auto &entry : cache.entries)
  {
    const double edge = std::ldexp(settings.chunkEdgeMetres, static_cast<int>(entry.address.spatialLOD));
    const double minimumX = static_cast<double>(entry.address.x) * edge;
    const double minimumZ = static_cast<double>(entry.address.z) * edge;
    if (mapX >= minimumX && mapX < minimumX + edge && mapZ >= minimumZ && mapZ < minimumZ + edge)
      return true;
  }
  return false;
}

std::string jsonString(const std::string &value)
{
  std::string result = "\"";
  for (const char character : value)
  {
    if (character == '\\' || character == '"')
      result.push_back('\\');
    result.push_back(character);
  }
  result.push_back('"');
  return result;
}

std::string reportJSON(const std::vector<PageRecord> &pages, const std::vector<ChurnRecord> &churn, const bool quick)
{
  std::ostringstream out;
  out << std::fixed << std::setprecision(6);
  out << "{\n  \"schema\": \"gpu-terrain-generation-phase0-v1\",\n";
  out << "  \"mode\": " << jsonString(quick ? "quick" : "full") << ",\n";
  out << "  \"gpu_upload_timing\": " << jsonString("captured by the existing TerrainUpload render-graph timer") << ",\n";
  out << "  \"pages\": [\n";
  for (size_t index = 0u; index < pages.size(); ++index)
  {
    const auto &page = pages[index];
    out << "    {\"fixture\": " << jsonString(page.fixture) << ", \"lod\": " << page.lod << ", \"map_sampling_ms\": " << page.generation.mapSamplingMilliseconds
        << ", \"shell_extraction_ms\": " << page.generation.shellExtractionMilliseconds << ", \"brick_hierarchy_ms\": " << page.generation.hierarchyBuildMilliseconds
        << ", \"tree64_ms\": " << page.tree64.buildMilliseconds << ", \"map_samples\": " << page.generation.mapSampleCount << ", \"shell_voxels\": " << page.generation.emittedShellVoxelCount
        << ", \"emitted_runs\": " << page.emittedRuns << ", \"horizontal_voxels\": " << page.generation.horizontalVoxelCount << ", \"vertical_voxel_capacity\": " << page.generation.verticalVoxelCapacity
        << ", \"brick_levels\": " << page.generation.brickLevelCount << ", \"brick_nodes\": " << page.generation.brickNodeCount << ", \"tree64_levels\": " << page.tree64.levelCount
        << ", \"tree64_nodes\": " << page.tree64.tree.nodes.size() << ", \"payload_bytes\": " << page.payloadBytes << ", \"semantic_hash\": " << page.semanticHash << "}";
    out << (index + 1u == pages.size() ? "\n" : ",\n");
  }
  out << "  ],\n  \"streaming_churn\": [\n";
  for (size_t index = 0u; index < churn.size(); ++index)
  {
    const auto &event = churn[index];
    out << "    {\"event\": " << jsonString(event.event) << ", \"camera_x\": " << event.cameraX << ", \"camera_z\": " << event.cameraZ << ", \"request_to_visible_ms\": " << event.requestToVisibleMilliseconds
        << ", \"streaming_update_ms\": " << event.updateMilliseconds << ", \"updates\": " << event.updates << ", \"built_pages\": " << event.builtPages << ", \"reused_pages\": " << event.reusedPages
        << ", \"evicted_pages\": " << event.evictedPages << ", \"pending_pages\": " << event.pendingPages << ", \"upload_bytes\": " << event.uploadBytes << ", \"visible\": " << (event.visible ? "true" : "false") << "}";
    out << (index + 1u == churn.size() ? "\n" : ",\n");
  }
  out << "  ]\n}\n";
  return out.str();
}

} // namespace

int main(int argc, char **argv)
{
  std::filesystem::path output;
  bool quick = false;
  for (int index = 1; index < argc; ++index)
  {
    const std::string argument = argv[index];
    if (argument == "--quick")
      quick = true;
    else if (argument == "--output" && index + 1 < argc)
      output = argv[++index];
    else
    {
      std::cerr << "Usage: SparseVoxelTerrainPhase0Benchmark "
                   "[--quick] [--output report.json]\n";
      return 2;
    }
  }

  std::vector<PageRecord> pages;
  std::string error;
  for (const auto kind : terrain_phase0::kFixtureKinds)
  {
    if (quick && kind != terrain_phase0::FixtureKind::Flat && kind != terrain_phase0::FixtureKind::Cliff && kind != terrain_phase0::FixtureKind::CrossLOD)
      continue;
    const uint32_t lodCount = quick ? 1u : 3u;
    for (uint32_t lod = 0u; lod < lodCount; ++lod)
    {
      const auto bundle = terrain_phase0::makeFixture(kind);
      const auto settings = terrain_phase0::fixtureSettings(kind, lod);
      svr::PlanetVoxelChunkSet page{};
      PageRecord record{};
      record.fixture = terrain_phase0::fixtureName(kind);
      record.lod = lod;
      if (!svr::buildWorldMapVoxelTerrain(bundle, settings, page, &error, {}, nullptr, &record.generation))
      {
        std::cerr << "Could not build " << record.fixture << " LOD " << lod << ": " << error << '\n';
        return 1;
      }
      terrain_phase0::SemanticTerrainPage semantic{};
      if (!terrain_phase0::decodeSemanticTerrainPage(page, 0u, semantic, &error) || !terrain_phase0::compareRasterAndTree64Occupancy(page, 0u, &record.tree64, &error))
      {
        std::cerr << "Could not decode " << record.fixture << " LOD " << lod << ": " << error << '\n';
        return 1;
      }
      record.semanticHash = terrain_phase0::semanticTerrainPageHash(semantic);
      record.emittedRuns = terrain_phase0::semanticTerrainRunCount(semantic);
      record.payloadBytes = terrain_phase0::terrainPayloadBytes(page) + record.tree64.payloadBytes;
      pages.push_back(std::move(record));
    }
  }

  const auto churnBundle = terrain_phase0::makeFixture(terrain_phase0::FixtureKind::CrossLOD);
  svr::WorldVoxelChunkStreamingSettings streaming{};
  streaming.chunkEdgeMetres = 32.0;
  streaming.finestVoxelSizeMetres = 1.0;
  streaming.finestHorizontalVoxelCount = 32u;
  streaming.renderDistanceMetres = 48.0;
  streaming.projectionScalePixels = 160.0;
  streaming.splitErrorPixels = 4.0f;
  streaming.mergeErrorPixels = 2.0f;
  streaming.maximumSpatialLOD = 3u;
  streaming.lodCount = 3u;
  streaming.maximumResidentChunks = 64u;
  streaming.maximumGeneratedChunksPerUpdate = quick ? 4u : 2u;
  streaming.maximumRefinedChunksPerUpdate = 1u;
  streaming.maximumCachedPayloads = 64u;
  streaming.maximumTransientMemoryBytes = 16ull << 20u;
  streaming.feedbackDrivenPageStreaming = true;
  streaming.voxel = terrain_phase0::fixtureSettings(terrain_phase0::FixtureKind::CrossLOD, 0u).voxel;

  struct CameraEvent
  {
    const char *name;
    double x;
    double z;
  };
  const std::vector<CameraEvent> cameraEvents =
      quick ? std::vector<CameraEvent>{{"bootstrap", 0.0, 0.0}, {"teleport", 80.0, 80.0}, {"return", 0.0, 0.0}}
            : std::vector<CameraEvent>{{"bootstrap", 0.0, 0.0}, {"walk-1", 16.0, 0.0}, {"walk-2", 32.0, 8.0}, {"walk-3", 48.0, 16.0}, {"teleport", 80.0, 80.0}, {"teleport-opposite", -80.0, -80.0}, {"return", 0.0, 0.0}};
  svr::WorldVoxelChunkCache cache{};
  std::vector<ChurnRecord> churn;
  for (const auto &camera : cameraEvents)
  {
    ChurnRecord record{};
    record.event = camera.name;
    record.cameraX = camera.x;
    record.cameraZ = camera.z;
    const auto requestStart = std::chrono::steady_clock::now();
    constexpr uint32_t kMaximumUpdates = 32u;
    for (uint32_t updateIndex = 0u; updateIndex < kMaximumUpdates; ++updateIndex)
    {
      svr::PlanetVoxelChunkSet chunks{};
      svr::WorldVoxelChunkCacheUpdate update{};
      const auto updateStart = std::chrono::steady_clock::now();
      if (!svr::updateWorldVoxelChunkCache(churnBundle, camera.x, camera.z, streaming, cache, chunks, &update, &error))
      {
        std::cerr << "Streaming event " << record.event << " failed: " << error << '\n';
        return 1;
      }
      record.updateMilliseconds += millisecondsSince(updateStart);
      ++record.updates;
      record.builtPages += update.builtChunkCount;
      record.reusedPages += update.reusedChunkCount;
      record.evictedPages += update.evictedChunkCount;
      record.pendingPages = update.pendingChunkCount;
      record.uploadBytes += update.physicalUploadBytes;
      record.visible = cacheCovers(cache, streaming, churnBundle, camera.x, camera.z);
      if (record.visible && cache.pendingChunkCount == 0u)
        break;
    }
    record.requestToVisibleMilliseconds = millisecondsSince(requestStart);
    churn.push_back(record);
  }

  const std::string json = reportJSON(pages, churn, quick);
  if (output.empty())
    std::cout << json;
  else
  {
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    if (!file || !(file << json))
    {
      std::cerr << "Could not write " << output << '\n';
      return 1;
    }
    std::cout << output << '\n';
  }
  return 0;
}
