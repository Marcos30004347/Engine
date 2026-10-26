#include "procedural/world/MapExportGpuCache.hpp"
#include "procedural/world/MapExportSampler.hpp"
#include "voxel/svraster/SparseVoxelStreaming.hpp"
#include "voxel/svraster/TerrainGPUV2Columns.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#undef NDEBUG
#include <cassert>

namespace
{
namespace world = procedural::world;
namespace svr = voxel::svraster;

std::vector<uint8_t> texel(const std::string &name)
{
  if (name == "refined_elevation" || name == "base_elevation")
    return {0u, 0u};
  if (name == "uplift" || name == "water_level" || name == "coast_distance" || name == "erosion_deposition")
    return {0u, 0u};
  if (name == "relief_roughness" || name == "river" || name == "erosion_wear_flow")
    return {0u, 0u, 0u, 0u};
  if (name == "landform_id")
    return {2u};
  if (name == "water_type" || name == "roads")
    return {0u};
  if (name == "substrate_id")
    return {3u};
  if (name == "soil_depth")
    return {0u, 0x3cu};
  if (name == "biome_mix_ids")
    return {13u, 0u, 0u, 0u};
  if (name == "biome_mix_weights")
    return {255u, 0u, 0u, 0u};
  if (name == "climate")
    return {0u, 0u, 0u, 0x38u};
  if (name == "placement_potential" || name == "soil_layers")
    return {0u, 0u, 0u, 0u};
  if (name == "region_id")
    return {42u, 0u, 0u, 0u};
  return {};
}

world::MapExportField field(const std::string &name, const world::MapExportFormat format, const world::MapExportFilter filter, const std::string &group = {}, const bool includeDetailPages = false)
{
  world::MapExportField result{};
  result.name = name;
  result.format = format;
  result.filter = filter;
  result.available = true;
  result.width = 8u;
  result.height = 8u;
  result.samplingGroup = group;
  if (name == "refined_elevation")
    result.offset[0] = 320.0;
  if (name == "base_elevation")
    result.offset[0] = 280.0;
  const auto value = texel(name);
  assert(value.size() == world::mapExportTexelBytes(format));
  constexpr uint32_t storedSide = 6u;
  for (uint8_t level = 0u; level <= (includeDetailPages ? 1u : 0u); ++level)
  {
    const uint32_t dimension = 1u << level;
    for (uint32_t y = 0u; y < dimension; ++y)
      for (uint32_t x = 0u; x < dimension; ++x)
      {
        world::MapExportPage page{};
        page.address = {level, x, y};
        page.width = storedSide;
        page.height = storedSide;
        page.data.resize(uint64_t{storedSide} * storedSide * value.size());
        for (uint32_t pixel = 0u; pixel < storedSide * storedSide; ++pixel)
          std::memcpy(page.data.data() + uint64_t{pixel} * value.size(), value.data(), value.size());
        result.pages.push_back(std::move(page));
      }
  }
  return result;
}

world::MapExportBundle semanticBundle(const bool includeDetailPages = false)
{
  world::MapExportBundle result{};
  result.resolution = 8u;
  result.pageSizePixels = 4u;
  result.gutterPixels = 1u;
  result.maximumPageLevel = 1u;
  result.extentMetres = 64.0;
  result.seed = 0x700d5eedull;
  result.bundleSignature = 0x7e77a17c0ffeu;
  result.fields = {
    field("refined_elevation", world::MapExportFormat::R16Unorm, world::MapExportFilter::Linear, {}, includeDetailPages),
    field("base_elevation", world::MapExportFormat::R16Unorm, world::MapExportFilter::Linear, {}, includeDetailPages),
    field("uplift", world::MapExportFormat::R16Float, world::MapExportFilter::Linear, {}, includeDetailPages),
    field("relief_roughness", world::MapExportFormat::RG16Float, world::MapExportFilter::Linear, {}, includeDetailPages),
    field("landform_id", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest, {}, includeDetailPages),
    field("water_type", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest, {}, includeDetailPages),
    field("water_level", world::MapExportFormat::R16Float, world::MapExportFilter::Linear, {}, includeDetailPages),
    field("river", world::MapExportFormat::RG16Float, world::MapExportFilter::Linear, {}, includeDetailPages),
    field("coast_distance", world::MapExportFormat::R16Float, world::MapExportFilter::Linear, {}, includeDetailPages),
    field("erosion_wear_flow", world::MapExportFormat::RG16Float, world::MapExportFilter::Linear, {}, includeDetailPages),
    field("erosion_deposition", world::MapExportFormat::R16Float, world::MapExportFilter::Linear, {}, includeDetailPages),
    field("substrate_id", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest, {}, includeDetailPages),
    field("soil_depth", world::MapExportFormat::R16Float, world::MapExportFilter::Linear, {}, includeDetailPages),
    field("biome_mix_ids", world::MapExportFormat::RGBA8Uint, world::MapExportFilter::SparseMixture, "biome", includeDetailPages),
    field("biome_mix_weights", world::MapExportFormat::RGBA8Unorm, world::MapExportFilter::SparseMixture, "biome", includeDetailPages),
    field("climate", world::MapExportFormat::RG16Float, world::MapExportFilter::Linear, {}, includeDetailPages),
    field("placement_potential", world::MapExportFormat::RGBA8Unorm, world::MapExportFilter::Linear, {}, includeDetailPages),
    field("soil_layers", world::MapExportFormat::RGBA8Unorm, world::MapExportFilter::Linear, {}, includeDetailPages),
    field("roads", world::MapExportFormat::R8Uint, world::MapExportFilter::Nearest, {}, includeDetailPages),
    field("region_id", world::MapExportFormat::R32Uint, world::MapExportFilter::Nearest, {}, includeDetailPages),
  };
  return result;
}

svr::TerrainGPUV2ColumnConfigGPU columnConfig(const world::MapExportBundle &bundle)
{
  world::MapExportGpuPageLayout layout;
  std::string error;
  assert(layout.initialize(bundle, 128u, &error));
  svr::TerrainGPUV2ColumnBuildSettings settings{};
  settings.key.worldBundleRevision = 17u;
  settings.key.tileX = 0;
  settings.key.tileZ = 0;
  settings.key.clipmapLOD = 0u;
  settings.key.terrainSlot = 3u;
  settings.key.expectedGeneration = 5u;
  settings.key.shape = {8u, 8u, 1u, 0u};
  settings.requestedMapLevel = 1u;
  settings.worldOriginCanonicalXZ = {0, 0};
  settings.worldExtentCanonical = 64u;
  settings.heightUnitMetres = 1.0f;
  settings.expectedAtlasRevision = 9u;
  settings.faultCapacity = 128u;
  svr::TerrainGPUV2ColumnConfigGPU config{};
  assert(svr::planTerrainGPUV2Columns(bundle, layout, settings, config, &error));
  return config;
}

const svr::TerrainGPUV2SemanticFieldSample *sampleByName(const svr::TerrainGPUV2SemanticSampleReport &report, const std::string &name)
{
  const auto found = std::find_if(
      report.fields.begin(),
      report.fields.end(),
      [&](const auto &sample)
      {
        return sample.fieldName == name;
      });
  return found == report.fields.end() ? nullptr : &*found;
}

svr::SparseVoxelPageProducer traceProducer(const uint64_t id = 0x7700u)
{
  svr::SparseVoxelPageProducer result{};
  result.producerId = id;
  result.kind = svr::SparseVoxelPageProducerKind::CPUGenerated;
  result.producerRevision = 7u;
  result.producesOnCPU = true;
  result.debugName = "phase7-trace-cut";
  return result;
}

svr::SparseVoxelStreamedInstance traceInstance(
    const uint32_t index,
    const svr::SparseVoxelPageProducer &producer,
    const svr::SparseVoxelPageId rootPage,
    const uint32_t firstRootNode = 0u,
    const uint32_t pageCount = 2u)
{
  svr::SparseVoxelStreamedInstance instance{};
  instance.instanceIndex = index;
  instance.producer = producer;
  instance.rootPage = rootPage;
  instance.firstRootNode = firstRootNode;
  instance.rootNodeCount = 1u;
  instance.firstPage = 0u;
  instance.pageCount = pageCount;
  for (uint32_t diagonal = 0u; diagonal < 4u; ++diagonal)
    instance.modelToWorld[diagonal * 4u + diagonal] = 1.0f;
  return instance;
}

svr::SparseVoxelProducedPagePayload tracePayload(
    const svr::SparseVoxelPageId page,
    const uint64_t expectedGeneration,
    const uint32_t producerRevision,
    const uint64_t payloadRevision,
    const uint32_t nodes = 1u)
{
  svr::SparseVoxelProducedPagePayload payload{};
  payload.page = page;
  payload.expectedPageGeneration = expectedGeneration;
  payload.requiredProducerRevision = producerRevision;
  payload.payloadRevision = payloadRevision;
  payload.requirements.page = page;
  payload.requirements.hierarchyNodes.count = nodes;
  payload.requirements.childIndices.count = nodes;
  payload.requirements.brickNodes.count = nodes;
  payload.requirements.brickOccupancy.count = nodes;
  payload.requirements.materials.count = nodes;
  payload.requirements.materialCells.count = nodes;
  payload.installRewriteCount = 1u;
  payload.uninstallRewriteCount = 1u;
  payload.payloadComplete = true;
  return payload;
}

svr::SparseVoxelPageInstallRewrite traceInstallRewrite(const svr::SparseVoxelReservedPagePayload &reservation)
{
  svr::SparseVoxelPageInstallRewrite rewrite{};
  rewrite.page = reservation.produced.page;
  rewrite.allocation = reservation.allocation;
  rewrite.requiredProducerRevision = reservation.produced.requiredProducerRevision;
  rewrite.rewrite.nodeIndex = reservation.allocation.hierarchyNodes.first;
  rewrite.rewrite.firstChild = reservation.allocation.childIndices.empty() ? 0u : reservation.allocation.childIndices.first;
  rewrite.rewrite.childCount = reservation.allocation.childIndices.count;
  rewrite.rewrite.flags = 1u;
  return rewrite;
}

void installTracePage(
    svr::SparseVoxelSharedPageTable &table,
    const svr::SparseVoxelPageProducer &producer,
    const svr::SparseVoxelPageId page,
    const uint64_t payloadRevision,
    std::string &error)
{
  const auto payload = tracePayload(page, table.entry(page)->generation, producer.producerRevision, payloadRevision);
  svr::SparseVoxelReservedPagePayload reservation{};
  assert(table.reserveProducedPagePayload(payload, reservation, &error));
  assert(table.markPayloadCopied(reservation, payloadRevision + 100u, &error));
  assert(table.installCopiedPagePayload(reservation, {traceInstallRewrite(reservation)}, &error));
}

svr::SparseVoxelTraceFeedbackInput traceInput(
    const svr::SparseVoxelStreamedInstance &instance,
    const svr::SparseVoxelPageId desiredPage,
    const svr::SparseVoxelTraceRayClass rayClass,
    const svr::SparseVoxelTraceLODMode mode,
    const bool offscreen,
    const uint32_t priority = 10u)
{
  svr::SparseVoxelTraceFeedbackInput input{};
  input.instance = instance;
  input.desiredPage = desiredPage;
  input.desiredLOD = 1u;
  input.rayClass = rayClass;
  input.mode = mode;
  input.priority = priority;
  input.desiredError = 0.25f;
  input.rayConeWidth = 0.05f;
  input.minimumT = 0.0f;
  input.maximumT = 256.0f;
  input.offscreen = offscreen;
  return input;
}

void testSemanticSamplerMatchesMapExportSampler()
{
  auto bundle = semanticBundle(false);
  const auto config = columnConfig(bundle);
  svr::TerrainGPUV2SemanticSampleReport report{};
  std::vector<svr::TerrainGPUV2MapFaultGPU> faults;
  std::string error;
  assert(svr::sampleTerrainGPUV2SemanticInputs(bundle, config, 0.62, 0.37, report, &faults, &error));
  assert(faults.empty());
  assert(report.sampledFields == 18u);
  assert(report.ancestorFallbacks >= 19u);
  assert(report.unavailableFieldFallbacks >= 2u);
  assert(report.missingPageFaults == 0u);
  assert(report.material == 13u);
  assert(report.materialFlags == svr::TerrainGPUV2ColumnValid);

  world::MapExportFieldSample elevation{};
  assert(world::sampleMapExportField(bundle, "refined_elevation", 0.62, 0.37, 1u, elevation, &error));
  const auto *reportedElevation = sampleByName(report, "refined_elevation");
  assert(reportedElevation != nullptr);
  assert(reportedElevation->fallback == svr::TerrainGPUV2SemanticFallbackKind::AncestorPage);
  assert(reportedElevation->requestedPage == elevation.requestedPage);
  assert(reportedElevation->resolvedPage == elevation.resolvedPage);
  assert(reportedElevation->componentCount == elevation.componentCount);
  assert(std::abs(reportedElevation->values[0] - elevation.values[0]) < 1.0e-9);

  world::MapExportFieldSample water{};
  assert(world::sampleMapExportField(bundle, "water_type", 0.62, 0.37, 1u, water, &error));
  const auto *reportedWater = sampleByName(report, "water_type");
  assert(reportedWater != nullptr);
  assert(reportedWater->values[0] == water.values[0]);

  world::MapExportFieldSample region{};
  assert(world::sampleMapExportField(bundle, "region_id", 0.62, 0.37, 1u, region, &error));
  const auto *reportedRegion = sampleByName(report, "region_id");
  assert(reportedRegion != nullptr);
  assert(reportedRegion->fieldSlot == svr::kTerrainGPUV2ColumnInvalidField);
  assert(reportedRegion->values[0] == 42.0);

  world::MapExportBiomeSample biome{};
  assert(world::sampleMapExportBiome(bundle, 0.62, 0.37, 1u, biome, &error));
  assert(report.biome.fallback == svr::TerrainGPUV2SemanticFallbackKind::AncestorPage);
  assert(report.biome.requestedPage == biome.requestedPage);
  assert(report.biome.resolvedPage == biome.resolvedPage);
  assert(report.biome.count == biome.count && report.biome.ids[0] == 13u);
  double weightSum = 0.0;
  for (uint32_t slot = 0u; slot < report.biome.count; ++slot)
    weightSum += report.biome.weights[slot];
  assert(std::abs(weightSum - 1.0) < 1.0e-9);

  assert(sampleByName(report, "rock_potential") != nullptr);
  assert(sampleByName(report, "rock_potential")->fallback == svr::TerrainGPUV2SemanticFallbackKind::UnavailableField);
  assert(sampleByName(report, "vegetation_potential") != nullptr);
  assert(sampleByName(report, "vegetation_potential")->fallback == svr::TerrainGPUV2SemanticFallbackKind::UnavailableField);
}

void testAncestorFallbackProducesValidColumnsAndFootprint()
{
  const auto bundle = semanticBundle(false);
  world::MapExportGpuPageLayout layout;
  std::string error;
  assert(layout.initialize(bundle, 128u, &error));
  const auto config = columnConfig(bundle);
  world::MapExportStreamRequest footprint{};
  assert(svr::terrainGPUV2ColumnFootprint(bundle, layout, config, footprint, &error));
  const auto hasFootprintField = [&](const std::string &name)
  {
    return std::find(footprint.fields.begin(), footprint.fields.end(), name) != footprint.fields.end();
  };
  assert(footprint.level == 1u);
  assert(hasFootprintField("refined_elevation"));
  assert(hasFootprintField("river"));
  assert(hasFootprintField("biome_mix_ids"));
  assert(hasFootprintField("biome_mix_weights"));

  std::vector<svr::TerrainGPUV2HeightSampleGPU> heights;
  std::vector<svr::TerrainGPUV2ColumnGPU> columns;
  assert(svr::buildTerrainGPUV2ColumnOracle(bundle, config, heights, columns, &error));
  assert(!heights.empty() && !columns.empty());
  assert(
      std::all_of(
          heights.begin(),
          heights.end(),
          [](const auto &height)
          {
            return height.heightUnits == 320 &&
                svr::terrainGPUV2HeightSampleFlags(height) ==
                    svr::TerrainGPUV2ColumnValid;
          }));
  assert(
      std::all_of(
          columns.begin(),
          columns.end(),
          [](const auto &column)
          {
            return column.minimumHeightUnits == 320 && column.maximumHeightUnits == 320 && column.material == 13u && column.flags == svr::TerrainGPUV2ColumnValid;
          }));
}

void testMissingRequiredSemanticPageReportsExactFault()
{
  auto bundle = semanticBundle(true);
  auto *river = bundle.field("river");
  assert(river != nullptr);
  river->pages.clear();
  const auto config = columnConfig(bundle);
  svr::TerrainGPUV2SemanticSampleReport report{};
  std::vector<svr::TerrainGPUV2MapFaultGPU> faults;
  std::string error;
  assert(!svr::sampleTerrainGPUV2SemanticInputs(bundle, config, 0.75, 0.25, report, &faults, &error));
  assert(report.missingPageFaults == 1u);
  assert(!faults.empty());
  assert(faults.front().field == static_cast<uint32_t>(svr::TerrainGPUV2ColumnField::River));
  assert(faults.front().level == 1u);
  const auto *riverSample = sampleByName(report, "river");
  assert(riverSample != nullptr);
  assert(riverSample->fallback == svr::TerrainGPUV2SemanticFallbackKind::MissingPage);
  assert(error.find("river") != std::string::npos);
}

void testTraceCutUsesScreenReceiverGeneration()
{
  const auto producer = traceProducer();
  const svr::SparseVoxelPageId receiverRoot{producer.producerId, 0u};
  const svr::SparseVoxelPageId receiverFine{producer.producerId, 1u};
  const svr::SparseVoxelPageId occluderRoot{producer.producerId, 2u};
  const svr::SparseVoxelPageId occluderFine{producer.producerId, 3u};

  std::string error;
  svr::SparseVoxelSharedPageTable table{};
  assert(table.configure({16u, 16u, 16u, 16u, 16u, 16u, 0u, 0u}, &error));
  assert(table.addPage(receiverRoot, producer, {}, true, &error));
  assert(table.addPage(receiverFine, producer, {receiverRoot}, false, &error));
  assert(table.addPage(occluderRoot, producer, {}, true, &error));
  assert(table.addPage(occluderFine, producer, {occluderRoot}, false, &error));
  installTracePage(table, producer, receiverRoot, 10u, error);
  installTracePage(table, producer, receiverFine, 11u, error);
  installTracePage(table, producer, occluderRoot, 12u, error);

  const auto receiver = svr::makeSparseVoxelTraceReceiverGPU(
      receiverFine,
      table.entry(receiverFine)->generation,
      1u,
      false);
  const auto occluderInstance = traceInstance(7u, producer, occluderRoot, 64u);

  svr::SparseVoxelTraceRequestQueue queue{};
  queue.configure(8u);
  svr::SparseVoxelTraceCutDiagnostics diagnostics{};
  auto input = traceInput(
      occluderInstance,
      occluderFine,
      svr::SparseVoxelTraceRayClass::Shadow,
      svr::SparseVoxelTraceLODMode::ConservativeVisibility,
      true,
      40u);

  assert(svr::evaluateSparseVoxelTraceCutConsistency(table, receiver, input, queue, diagnostics, &error));
  assert(diagnostics.receiverValid);
  assert(diagnostics.receiverGenerationMatches);
  assert(diagnostics.screenSelectedPage == receiverFine);
  assert(diagnostics.screenSelectedGeneration == table.entry(receiverFine)->generation);
  assert(diagnostics.screenSelectedLOD == 1u);
  assert(diagnostics.traceDesiredPage == occluderFine);
  assert(diagnostics.traceSelectedPage == occluderRoot);
  assert(diagnostics.traceFallbackPage == occluderRoot);
  assert(diagnostics.traceRequestPage == occluderFine);
  assert(diagnostics.traceOffscreen);
  assert(diagnostics.traceConservativeFallback);
  assert(diagnostics.traceRequestEmitted);
  assert(queue.size() == 1u);
  assert(svr::sparseVoxelTraceRequestReceiverPageId(queue.requests().front(), producer.producerId) == receiverFine);
  assert(svr::sparseVoxelTraceRequestReceiverGeneration(queue.requests().front()) == table.entry(receiverFine)->generation);

  auto staleReceiver = receiver;
  staleReceiver.selectedGenerationLo += 1u;
  assert(!svr::evaluateSparseVoxelTraceCutConsistency(table, staleReceiver, input, queue, diagnostics, &error));
  assert(!diagnostics.receiverGenerationMatches);
}

void testOffscreenTraceCutStableAcrossCameraRotation()
{
  const auto producer = traceProducer(0x7701u);
  const svr::SparseVoxelPageId receiverRoot{producer.producerId, 0u};
  const svr::SparseVoxelPageId occluderRoot{producer.producerId, 10u};
  const svr::SparseVoxelPageId occluderFine{producer.producerId, 11u};

  std::string error;
  svr::SparseVoxelSharedPageTable table{};
  assert(table.configure({12u, 12u, 12u, 12u, 12u, 12u, 0u, 0u}, &error));
  assert(table.addPage(receiverRoot, producer, {}, true, &error));
  assert(table.addPage(occluderRoot, producer, {}, true, &error));
  assert(table.addPage(occluderFine, producer, {occluderRoot}, false, &error));
  installTracePage(table, producer, receiverRoot, 20u, error);
  installTracePage(table, producer, occluderRoot, 21u, error);

  const auto receiver = svr::makeSparseVoxelTraceReceiverGPU(
      receiverRoot,
      table.entry(receiverRoot)->generation,
      0u,
      true);
  const auto occluderInstance = traceInstance(8u, producer, occluderRoot, 128u);

  svr::SparseVoxelTraceRequestQueue queue{};
  queue.configure(4u);
  svr::SparseVoxelTraceCutDiagnostics first{};
  auto firstInput = traceInput(
      occluderInstance,
      occluderFine,
      svr::SparseVoxelTraceRayClass::Shadow,
      svr::SparseVoxelTraceLODMode::ConservativeVisibility,
      true,
      10u);
  assert(svr::evaluateSparseVoxelTraceCutConsistency(table, receiver, firstInput, queue, first, &error));

  svr::SparseVoxelTraceCutDiagnostics rotated{};
  auto rotatedInput = firstInput;
  rotatedInput.priority = 100u;
  rotatedInput.rayConeWidth = 0.2f;
  assert(svr::evaluateSparseVoxelTraceCutConsistency(table, receiver, rotatedInput, queue, rotated, &error));

  assert(first.traceSelectedPage == occluderRoot);
  assert(rotated.traceSelectedPage == first.traceSelectedPage);
  assert(first.traceRequestPage == occluderFine);
  assert(rotated.traceRequestPage == first.traceRequestPage);
  assert(queue.size() == 1u);
  assert(queue.stats().deduplicated == 1u);
  assert(queue.requests().front().priority == 100u);
}

void testMissingFineVisibleReceiverExactRayIsUnresolved()
{
  const auto producer = traceProducer(0x7702u);
  const svr::SparseVoxelPageId receiverRoot{producer.producerId, 0u};
  const svr::SparseVoxelPageId receiverFine{producer.producerId, 1u};

  std::string error;
  svr::SparseVoxelSharedPageTable table{};
  assert(table.configure({8u, 8u, 8u, 8u, 8u, 8u, 0u, 0u}, &error));
  assert(table.addPage(receiverRoot, producer, {}, true, &error));
  assert(table.addPage(receiverFine, producer, {receiverRoot}, false, &error));
  installTracePage(table, producer, receiverRoot, 30u, error);

  const auto receiver = svr::makeSparseVoxelTraceReceiverGPU(
      receiverRoot,
      table.entry(receiverRoot)->generation,
      0u,
      true);
  const auto receiverInstance = traceInstance(9u, producer, receiverRoot, 0u);

  svr::SparseVoxelTraceRequestQueue queue{};
  queue.configure(4u);
  svr::SparseVoxelTraceCutDiagnostics diagnostics{};
  auto input = traceInput(
      receiverInstance,
      receiverFine,
      svr::SparseVoxelTraceRayClass::Primary,
      svr::SparseVoxelTraceLODMode::Exact,
      false,
      80u);
  assert(svr::evaluateSparseVoxelTraceCutConsistency(table, receiver, input, queue, diagnostics, &error));
  assert(diagnostics.receiverGenerationMatches);
  assert(diagnostics.traceExactUnresolved);
  assert(!diagnostics.traceSelectedPage.valid());
  assert(!diagnostics.traceFallbackPage.valid());
  assert(diagnostics.traceRequestPage == receiverFine);
  assert(diagnostics.traceRequestEmitted);
  assert(queue.size() == 1u);
  assert((queue.requests().front().flags & svr::kSparseVoxelTraceRequestExactUnresolved) != 0u);
  assert((queue.requests().front().flags & svr::kSparseVoxelTraceRequestVisibleReceiver) != 0u);
}

} // namespace

int main()
{
  testSemanticSamplerMatchesMapExportSampler();
  testAncestorFallbackProducesValidColumnsAndFootprint();
  testMissingRequiredSemanticPageReportsExactFault();
  testTraceCutUsesScreenReceiverGeneration();
  testOffscreenTraceCutStableAcrossCameraRotation();
  testMissingFineVisibleReceiverExactRayIsUnresolved();
  std::cout << "Sparse voxel terrain convergence Phase 7 tests passed\n";
  return 0;
}
