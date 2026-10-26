#include "procedural/world/MapExport.hpp"
#include "voxel/svraster/TerrainGPUBuildScheduler.hpp"
#include "voxel/svraster/TerrainGPUWorldMapAtlas.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#undef NDEBUG
#include <cassert>

namespace
{
namespace world = procedural::world;
namespace svr = voxel::svraster;

world::MapExportField field(const std::string &name, const world::MapExportFormat format, const world::MapExportFilter filter, const std::string &group = {})
{
  world::MapExportField result{};
  result.name = name;
  result.format = format;
  result.filter = filter;
  result.available = true;
  result.width = 4u;
  result.height = 4u;
  result.samplingGroup = group;
  const uint32_t texelBytes = world::mapExportTexelBytes(format);
  for (uint8_t level = 0u; level <= 1u; ++level)
  {
    const uint32_t dimension = 1u << level;
    for (uint32_t y = 0u; y < dimension; ++y)
      for (uint32_t x = 0u; x < dimension; ++x)
      {
        world::MapExportPage page{};
        page.address = {level, x, y};
        page.width = 4u;
        page.height = 4u;
        page.data.resize(16u * texelBytes);
        for (size_t byte = 0u; byte < page.data.size(); ++byte)
          page.data[byte] = static_cast<uint8_t>(name.size() * 17u + level * 31u + x * 7u + y * 11u + byte);
        result.pages.push_back(std::move(page));
      }
  }
  return result;
}

world::MapExportBundle atlasBundle()
{
  world::MapExportBundle bundle{};
  bundle.resolution = 4u;
  bundle.pageSizePixels = 2u;
  bundle.gutterPixels = 1u;
  bundle.maximumPageLevel = 1u;
  bundle.fields = {
    field("height", world::MapExportFormat::R16Unorm, world::MapExportFilter::Linear),
    field("category", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest),
  };
  return bundle;
}

svr::TerrainGPUBuildBudget budget()
{
  svr::TerrainGPUBuildBudget result{};
  result.maximumScratchBytes = 4096u;
  result.maximumInFlightJobs = 4u;
  return result;
}

svr::TerrainGPUBuildRequest buildRequest()
{
  svr::TerrainGPUBuildRequest result{};
  result.key = {0x61746c6173ull, 0, 0, 1u, 3u, 7u, 1u};
  result.jobClass = svr::TerrainGPUJobClass::VisibleMissing;
  result.projectedError = 4.0f;
  result.distance = 8.0f;
  result.scratchBytes = 512u;
  result.semanticRevision = 1u;
  result.mapsReady = false;
  return result;
}

world::MapExportStreamRequest mapRequest()
{
  world::MapExportStreamRequest result{};
  result.normalizedBounds = {0.0, 0.0, 0.49, 0.49};
  result.level = 1u;
  result.byteBudget = std::numeric_limits<size_t>::max();
  result.fields = {"height", "category"};
  return result;
}

void testNonblockingReadinessAndPinning()
{
  const auto bundle = atlasBundle();
  svr::TerrainGPUWorldMapAtlasConfig config{};
  config.pageCache.slotCapacity = 6u;
  config.pinnedRootFields = {"height", "category"};
  svr::TerrainGPUWorldMapAtlas atlas;
  std::string error;
  assert(atlas.initialize(bundle, config, &error));
  assert(atlas.layout().fields().size() == 2u);

  svr::TerrainGPUBuildScheduler scheduler(budget());
  scheduler.setBundleSignature(0x61746c6173ull);
  scheduler.setTerrainGeneration(7u, 1u);
  uint64_t job = 0u;
  assert(scheduler.request(buildRequest(), job, &error));
  assert(atlas.request(job, mapRequest(), &error));
  assert(!atlas.ready(job));
  assert(scheduler.job(job)->stage == svr::TerrainGPUBuildStage::WaitingForMaps);
  assert(atlas.waitForPreparation(std::chrono::seconds(2), &error));

  world::MapExportPagePublication partial{};
  assert(atlas.publish(16u, partial, &error));
  assert(partial.uploads.size() == 1u);
  assert(partial.deferredUnits != 0u);
  world::MapExportPagePublication duplicatePublish{};
  assert(!atlas.publish(16u, duplicatePublish, &error));
  auto corruptCommit = partial;
  corruptCommit.pageTable.clear();
  assert(!atlas.commit(corruptCommit, scheduler, &error));
  // This call stands after MapExportGpuPageCache::apply in the render path.
  assert(atlas.commit(partial, scheduler, &error));
  assert(!atlas.ready(job));
  assert(scheduler.job(job)->stage == svr::TerrainGPUBuildStage::WaitingForMaps);

  world::MapExportPagePublication complete{};
  assert(atlas.publish(std::numeric_limits<size_t>::max(), complete, &error));
  assert(atlas.commit(complete, scheduler, &error));
  assert(atlas.ready(job));
  assert(scheduler.job(job)->stage == svr::TerrainGPUBuildStage::UploadColumns);
  const auto resident = atlas.diagnostics();
  assert(resident.readyJobs == 1u && resident.pinnedPages == 4u);

  assert(scheduler.cancel(job));
  assert(atlas.release(job, &error));
  world::MapExportPagePublication evicted{};
  assert(atlas.publish(0u, evicted, &error));
  assert(evicted.evictions.size() == 2u);
  assert(evicted.pageTable.size() == 2u);
  assert(atlas.commit(evicted, scheduler, &error));
  assert(atlas.diagnostics().pinnedPages == 2u);
}

void testCapacityAndRequiredFieldsFailClosed()
{
  const auto bundle = atlasBundle();
  svr::TerrainGPUWorldMapAtlasConfig config{};
  config.pageCache.slotCapacity = 3u;
  config.pinnedRootFields = {"height", "category"};
  svr::TerrainGPUWorldMapAtlas atlas;
  std::string error;
  assert(atlas.initialize(bundle, config, &error));
  assert(!atlas.request(1u, mapRequest(), &error));

  world::MapExportBundle terrain{};
  terrain.resolution = 2u;
  terrain.pageSizePixels = 2u;
  terrain.gutterPixels = 1u;
  terrain.fields = {
    field("refined_elevation", world::MapExportFormat::R16Unorm, world::MapExportFilter::Linear),
    field("water_type", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest),
    field("water_level", world::MapExportFormat::R16Float, world::MapExportFilter::Linear),
    field("river", world::MapExportFormat::RG16Float, world::MapExportFilter::Linear),
    field("substrate_id", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest),
    field("soil_depth", world::MapExportFormat::R16Float, world::MapExportFilter::Linear),
    field("biome_mix_ids", world::MapExportFormat::RGBA8Uint, world::MapExportFilter::SparseMixture, "biome"),
    field("biome_mix_weights", world::MapExportFormat::RGBA8Unorm, world::MapExportFilter::SparseMixture, "biome"),
  };
  std::vector<std::string> fields;
  assert(svr::terrainGPUWorldMapFields(terrain, fields, &error));
  assert(fields.size() == 8u && fields.front() == "refined_elevation");
  terrain.field("water_type")->filter = world::MapExportFilter::Linear;
  assert(!svr::terrainGPUWorldMapFields(terrain, fields, &error));
  terrain.field("water_type")->filter = world::MapExportFilter::Nearest;
  terrain.fields.erase(terrain.fields.begin() + 1u);
  assert(!svr::terrainGPUWorldMapFields(terrain, fields, &error));
}

} // namespace

int main()
{
  testNonblockingReadinessAndPinning();
  testCapacityAndRequiredFieldsFailClosed();
  std::cout << "GPU terrain Phase 6 atlas residency tests passed\n";
  return 0;
}
