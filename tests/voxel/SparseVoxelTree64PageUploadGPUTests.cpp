#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/rendering/SparseVoxelTerrainUploadPass.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#undef assert
#define assert(condition)                                                \
  do                                                                     \
  {                                                                      \
    if (!(condition))                                                    \
    {                                                                    \
      std::fprintf(stderr, "test assertion failed: %s (%s:%d)\n",       \
                   #condition, __FILE__, __LINE__);                       \
      std::abort();                                                      \
    }                                                                    \
  } while (false)

namespace
{

namespace svrRendering = voxel::svraster::rendering;
using namespace voxel::svraster;

void testUnpublishedPageIsolation(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const bool completePage)
{
  using namespace ::rendering;
  using UploadPass = svrRendering::SparseVoxelTerrainUploadPass;
  RenderGraph graph(
      &rhi,
      RenderGraph::Settings{.maxTimers = 16u, .maxFramesInFlight = 2u});
  auto storage = [&](const char *name, const uint64_t size)
  {
    return graph.createBuffer({
      .name = name,
      .size = size,
      .usage = BufferUsage::BufferUsage_Storage |
          BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
    });
  };
  constexpr uint32_t compactWords = 6u;
  constexpr uint32_t paddedWords = 8u;
  constexpr uint32_t materialWords = 8u;
  std::array<Buffer, UploadPass::kBufferDestinationCount> destinations{};
  std::array<std::string, UploadPass::kBufferDestinationCount> names{};
  for (uint32_t index = 0u; index < destinations.size(); ++index)
  {
    names[index] = "tree64.page.destination." + std::to_string(index);
    destinations[index] = storage(names[index].c_str(), 64u);
  }
  destinations[static_cast<uint32_t>(UploadPass::Destination::Tree64PageNodes)] =
      storage("tree64.page.nodes", compactWords * sizeof(uint32_t));
  destinations[static_cast<uint32_t>(UploadPass::Destination::Tree64PageNodesPadded)] =
      storage("tree64.page.nodesPadded", paddedWords * sizeof(uint32_t));
  destinations[static_cast<uint32_t>(UploadPass::Destination::Tree64PageMaterialCells)] =
      storage("tree64.page.materials", materialWords * sizeof(uint32_t));

  const Buffer staging = storage("tree64.page.staging", 64u);
  constexpr uint32_t readbackWords =
      4u + compactWords + paddedWords + 2u;
  const Buffer readback = graph.createBuffer({
    .name = "tree64.page.readback",
    .size = readbackWords * sizeof(uint32_t),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  const Texture atlas = graph.createTexture({
    .name = "tree64.page.atlas",
    .format = Format::Format_RGBA8Uint,
    .memoryProperties = BufferUsage::BufferUsage_None,
    .usage = ImageUsage::ImageUsage_Storage,
    .width = 1u,
    .height = 1u,
    .mipLevels = 1u,
  });
  const std::array<uint32_t, 4> liveTraceInputs{
    0x13572468u, 0x24681357u, 0x55aa55aau, 0xaa55aa55u};
  graph.bufferWrite(
      destinations[static_cast<uint32_t>(UploadPass::Destination::Tree64Nodes)],
      0u, sizeof(liveTraceInputs),
      const_cast<uint32_t *>(liveTraceInputs.data()));
  std::array<uint32_t, 16> uploadWords{};
  for (uint32_t index = 0u; index < uploadWords.size(); ++index)
    uploadWords[index] = 0x90000000u + index;
  graph.bufferWrite(staging, 0u, sizeof(uploadWords), uploadWords.data());

  UploadPass::Capacities capacities{};
  capacities.stagingBytes = sizeof(uploadWords);
  for (uint32_t index = 0u; index < destinations.size(); ++index)
    capacities.destinationBytes[index] = graph.getBufferSize(destinations[index]);
  uint32_t passIndex = 0u;
  auto *upload = graph.registerPass<UploadPass>(
      "tree64.page.upload", passIndex++,
      UploadPass::Buffers{staging, destinations, atlas}, capacities);
  UploadPass::SettingsGPU settings{};
  settings.atlasWidth = 1u;
  settings.uploadWords = completePage ? 16u : 2u;
  settings.sectionCount = completePage ? 3u : 1u;
  settings.sections[0] = {
    0u, 0u, completePage ? compactWords : 2u,
    static_cast<uint32_t>(UploadPass::Destination::Tree64PageNodes)};
  if (completePage)
  {
    settings.sections[1] = {
      compactWords, 0u, paddedWords,
      static_cast<uint32_t>(UploadPass::Destination::Tree64PageNodesPadded)};
    settings.sections[2] = {
      compactWords + paddedWords, 0u, 2u,
      static_cast<uint32_t>(UploadPass::Destination::Tree64PageMaterialCells)};
  }
  assert(upload->update(settings));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "tree64.page.copyLive", passIndex++,
      destinations[static_cast<uint32_t>(UploadPass::Destination::Tree64Nodes)],
      0u, sizeof(liveTraceInputs), readback, 0u, sizeof(liveTraceInputs));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "tree64.page.copyNodes", passIndex++,
      destinations[static_cast<uint32_t>(UploadPass::Destination::Tree64PageNodes)],
      0u, compactWords * sizeof(uint32_t), readback,
      4u * sizeof(uint32_t), compactWords * sizeof(uint32_t));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "tree64.page.copyNodesPadded", passIndex++,
      destinations[static_cast<uint32_t>(UploadPass::Destination::Tree64PageNodesPadded)],
      0u, paddedWords * sizeof(uint32_t), readback,
      (4u + compactWords) * sizeof(uint32_t),
      paddedWords * sizeof(uint32_t));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "tree64.page.copyMaterials", passIndex++,
      destinations[static_cast<uint32_t>(UploadPass::Destination::Tree64PageMaterialCells)],
      0u, 2u * sizeof(uint32_t), readback,
      (4u + compactWords + paddedWords) * sizeof(uint32_t),
      2u * sizeof(uint32_t));
  graph.compile();
  RenderGraph::Frame frame{};
  RenderGraph::Overrides overrides{};
  graph.run(frame, overrides);
  graph.waitFrame(frame);
  std::array<uint32_t, readbackWords> result{};
  graph.bufferRead(
      readback, 0u, sizeof(result),
      [&](const void *data)
      {
        std::memcpy(result.data(), data, sizeof(result));
      });
  // Upload-to-copy synchronization also exercises the page-pool write
  // barrier. The compatibility trace/directory buffers remain byte-identical,
  // so neither a partial nor complete unpublished page can affect a lookup.
  assert(std::equal(
      liveTraceInputs.begin(), liveTraceInputs.end(), result.begin()));
  const uint32_t compactCount = completePage ? compactWords : 2u;
  for (uint32_t word = 0u; word < compactCount; ++word)
    assert(result[4u + word] == uploadWords[word]);
  if (completePage)
  {
    for (uint32_t word = 0u; word < paddedWords; ++word)
      assert(result[4u + compactWords + word] ==
          uploadWords[compactWords + word]);
    for (uint32_t word = 0u; word < 2u; ++word)
      assert(result[4u + compactWords + paddedWords + word] ==
          uploadWords[compactWords + paddedWords + word]);
  }
}

struct PublicationReadback
{
  std::array<uint32_t, 4> activation{};
  std::array<SparseVoxelTerrainPageStateGPU, 2> pageStates{};
  std::array<SparseVoxelTerrainTree64DescriptorGPU, 2> descriptors{};
  std::array<SparseVoxelTree64DynamicInstanceGPU, 2> instances{};
  SparseVoxelTerrainPublicationStatusGPU status{};
};

std::array<uint32_t, 4> pageStateWords(
    const SparseVoxelTerrainPageStateGPU &state)
{
  std::array<uint32_t, 4> words{};
  std::memcpy(words.data(), &state, sizeof(state));
  return words;
}

void testAtomicTerrainPublication(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  using namespace ::rendering;
  using UploadPass = svrRendering::SparseVoxelTerrainUploadPass;
  RenderGraph graph(&rhi);
  const auto storage = [&](const std::string &name, const uint64_t size)
  {
    return graph.createBuffer({
      .name = name,
      .size = std::max<uint64_t>(size, sizeof(uint32_t)),
      .usage = BufferUsage::BufferUsage_Storage |
          BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
    });
  };
  std::array<Buffer, UploadPass::kBufferDestinationCount> destinations{};
  for (uint32_t index = 0u; index < destinations.size(); ++index)
    destinations[index] = storage(
        "tree64.publish.destination." + std::to_string(index), 64u);
  const Buffer pageStatesBuffer = storage(
      "tree64.publish.pageStates",
      2u * sizeof(SparseVoxelTerrainPageStateGPU));
  const Buffer nodeGenerationsBuffer = storage(
      "tree64.publish.nodeGenerations", 2u * sizeof(uint32_t));
  const Buffer materialGenerationsBuffer = storage(
      "tree64.publish.materialGenerations", 2u * sizeof(uint32_t));
  destinations[static_cast<uint32_t>(
      UploadPass::Destination::TerrainPageStates)] = pageStatesBuffer;
  destinations[static_cast<uint32_t>(
      UploadPass::Destination::Tree64NodePageGenerations)] =
      nodeGenerationsBuffer;
  destinations[static_cast<uint32_t>(
      UploadPass::Destination::Tree64MaterialPageGenerations)] =
      materialGenerationsBuffer;
  const Buffer descriptorsBuffer = storage(
      "tree64.publish.descriptors",
      2u * sizeof(SparseVoxelTerrainTree64DescriptorGPU));
  const Buffer instancesBuffer = storage(
      "tree64.publish.instances",
      2u * sizeof(SparseVoxelTree64DynamicInstanceGPU));
  destinations[static_cast<uint32_t>(
      UploadPass::Destination::Tree64Instances)] = instancesBuffer;
  const Buffer staging = storage("tree64.publish.staging", 64u);
  const Texture atlas = graph.createTexture({
    .name = "tree64.publish.atlas",
    .format = Format::Format_RGBA8Uint,
    .memoryProperties = BufferUsage::BufferUsage_None,
    .usage = ImageUsage::ImageUsage_Storage,
    .width = 1u,
    .height = 1u,
    .mipLevels = 1u,
  });

  const SparseVoxelTerrainPageStateGPU oldState{3u, 2u, 10u, 1u};
  const SparseVoxelTerrainPageStateGPU newState{4u, 1u, 20u, 1u};
  SparseVoxelTerrainTree64DescriptorGPU oldDescriptor{
    0u, 3u, 2u, kSparseVoxelTree64TerrainPageValid,
    0u, 5u, 0u, 4u,
    0u, 5u, 0u, 2u,
    10u, 0u, 4u, 0x3f800000u,
  };
  SparseVoxelTerrainTree64DescriptorGPU newDescriptor{
    0u, 4u, 1u, kSparseVoxelTree64TerrainPageValid,
    1u, 7u, 0u, 6u,
    1u, 7u, 0u, 3u,
    20u, 0u, 4u, 0x3f800000u,
  };
  std::array<SparseVoxelTerrainPageStateGPU, 2> states{oldState, {}};
  std::array<SparseVoxelTerrainTree64DescriptorGPU, 2> descriptors{
    oldDescriptor, {}};
  std::array<uint32_t, 2> nodeGenerations{5u, 0u};
  std::array<uint32_t, 2> materialGenerations{5u, 0u};
  const std::array<uint32_t, 4> oldActivation{
    0x11111111u, 0u, 0u, 0u};
  graph.bufferWrite(pageStatesBuffer, 0u, sizeof(states), states.data());
  graph.bufferWrite(
      descriptorsBuffer, 0u, sizeof(descriptors), descriptors.data());
  graph.bufferWrite(
      nodeGenerationsBuffer, 0u, sizeof(nodeGenerations),
      nodeGenerations.data());
  graph.bufferWrite(
      materialGenerationsBuffer, 0u, sizeof(materialGenerations),
      materialGenerations.data());
  graph.bufferWrite(
      destinations[static_cast<uint32_t>(
          UploadPass::Destination::BrickNodes)],
      0u, sizeof(oldActivation),
      const_cast<uint32_t *>(oldActivation.data()));

  UploadPass::Capacities capacities{};
  capacities.stagingBytes = 64u;
  for (uint32_t index = 0u; index < destinations.size(); ++index)
    capacities.destinationBytes[index] =
        graph.getBufferSize(destinations[index]);
  capacities.terrainTree64DescriptorBytes =
      graph.getBufferSize(descriptorsBuffer);
  capacities.publicationRecordCount = 2u;
  uint32_t passIndex = 0u;
  auto *upload = graph.registerPass<UploadPass>(
      "tree64.publish.upload", passIndex++,
      UploadPass::Buffers{
        staging, destinations, atlas, descriptorsBuffer}, capacities);
  UploadPass::SettingsGPU initialSettings{};
  initialSettings.atlasWidth = 1u;
  assert(upload->update(initialSettings));

  const Buffer readback = graph.createBuffer({
    .name = "tree64.publish.readback",
    .size = sizeof(PublicationReadback),
    .usage = BufferUsage::BufferUsage_Pull |
        BufferUsage::BufferUsage_CopyDst,
  });
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "tree64.publish.copyActivation", passIndex++,
      destinations[static_cast<uint32_t>(
          UploadPass::Destination::BrickNodes)],
      0u, sizeof(PublicationReadback::activation), readback,
      offsetof(PublicationReadback, activation),
      sizeof(PublicationReadback::activation));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "tree64.publish.copyStates", passIndex++, pageStatesBuffer, 0u,
      sizeof(PublicationReadback::pageStates), readback,
      offsetof(PublicationReadback, pageStates),
      sizeof(PublicationReadback::pageStates));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "tree64.publish.copyDescriptors", passIndex++, descriptorsBuffer, 0u,
      sizeof(PublicationReadback::descriptors), readback,
      offsetof(PublicationReadback, descriptors),
      sizeof(PublicationReadback::descriptors));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "tree64.publish.copyInstances", passIndex++, instancesBuffer, 0u,
      sizeof(PublicationReadback::instances), readback,
      offsetof(PublicationReadback, instances),
      sizeof(PublicationReadback::instances));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "tree64.publish.copyStatus", passIndex++,
      upload->publicationStatus(), 0u,
      sizeof(SparseVoxelTerrainPublicationStatusGPU), readback,
      offsetof(PublicationReadback, status),
      sizeof(SparseVoxelTerrainPublicationStatusGPU));
  graph.compile();

  uint32_t nextFrameIndex = 0u;
  const auto stage = [&] (
      const UploadPass::SettingsGPU &settings,
      const SparseVoxelTerrainPublicationRecordGPU *records = nullptr)
  {
    graph.setCurrentFrameIndex(nextFrameIndex);
    return upload->update(settings, records);
  };
  const auto execute = [&]()
  {
    RenderGraph::Frame frame{.frameIndex = nextFrameIndex++};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);
    graph.setCurrentFrameIndex(frame.frameIndex);
    PublicationReadback result{};
    graph.bufferRead(
        readback, 0u, sizeof(result),
        [&](const void *data)
        {
          std::memcpy(&result, data, sizeof(result));
        });
    return result;
  };
  const auto resetOld = [&]()
  {
    states = {oldState, {}};
    descriptors = {oldDescriptor, {}};
    graph.bufferWrite(pageStatesBuffer, 0u, sizeof(states), states.data());
    graph.bufferWrite(
        descriptorsBuffer, 0u, sizeof(descriptors), descriptors.data());
    graph.bufferWrite(
        destinations[static_cast<uint32_t>(
            UploadPass::Destination::BrickNodes)],
        0u, sizeof(oldActivation),
        const_cast<uint32_t *>(oldActivation.data()));
  };

  // Forced split: physical-page generation metadata becomes resident in one
  // dispatch but remains unreachable until the following descriptor swap.
  const std::array<uint32_t, 2> generations{7u, 7u};
  graph.bufferWrite(
      staging, 0u, sizeof(generations),
      const_cast<uint32_t *>(generations.data()));
  UploadPass::SettingsGPU metadataSettings{};
  metadataSettings.generation = 40u;
  metadataSettings.atlasWidth = 1u;
  metadataSettings.uploadWords = 2u;
  metadataSettings.sectionCount = 2u;
  metadataSettings.sections[0] = {
    0u, 1u, 1u,
    static_cast<uint32_t>(
        UploadPass::Destination::Tree64NodePageGenerations)};
  metadataSettings.sections[1] = {
    1u, 1u, 1u,
    static_cast<uint32_t>(
        UploadPass::Destination::Tree64MaterialPageGenerations)};
  assert(stage(metadataSettings));
  const auto metadataOnly = execute();
  assert(metadataOnly.pageStates[0].generation == oldState.generation);
  assert(metadataOnly.descriptors[0].nodePageSlot ==
      oldDescriptor.nodePageSlot);

  SparseVoxelTerrainPublicationRecordGPU record{};
  record.terrainSlot = 0u;
  record.flags = kSparseVoxelTerrainPublicationValid;
  record.expectedPageState = pageStateWords(oldState);
  record.replacementPageState = pageStateWords(newState);
  record.expectedTree64 = oldDescriptor;
  record.replacementTree64 = newDescriptor;
  const uint32_t newActivation = 0x22222222u;
  graph.bufferWrite(
      staging, 0u, sizeof(newActivation),
      const_cast<uint32_t *>(&newActivation));
  UploadPass::SettingsGPU publishSettings{};
  publishSettings.generation = 41u;
  publishSettings.atlasWidth = 1u;
  publishSettings.uploadWords = 1u;
  publishSettings.sectionCount = 1u;
  publishSettings.publicationCount = 1u;
  publishSettings.nodesPerPage = 16u;
  publishSettings.materialCellsPerPage = 8u;
  publishSettings.sections[0] = {
    0u, 0u, 1u,
    static_cast<uint32_t>(UploadPass::Destination::BrickNodes)};
  assert(stage(publishSettings, &record));
  const auto accepted = execute();
  assert(accepted.status.accepted == 1u);
  assert(accepted.status.generation == publishSettings.generation);
  assert(accepted.status.publishedRecordCount == 1u);
  assert(accepted.activation[0] == newActivation);
  assert(accepted.pageStates[0].generation == newState.generation);
  assert(accepted.pageStates[0].currentLOD == newState.currentLOD);
  assert(accepted.descriptors[0].nodePageSlot ==
      newDescriptor.nodePageSlot);
  assert(accepted.descriptors[0].nodePageGeneration ==
      newDescriptor.nodePageGeneration);
  SparseVoxelTerrainPublicationStatusGPU acceptedStatus{};
  assert(upload->readPublicationStatus(acceptedStatus));
  assert(acceptedStatus.accepted == 1u);
  assert(acceptedStatus.generation == publishSettings.generation);
  assert(acceptedStatus.publishedRecordCount == 1u);

  // A stable logical slot may refine or coarsen in place. CPU control reads
  // only the compact GPU completion record, authors this full instance
  // install command, and swaps it with the page state/descriptor. Verify that
  // transform and bounds are replaced too—not only the visibility words.
  SparseVoxelTerrainPageStateGPU refinedState = newState;
  ++refinedState.generation;
  refinedState.currentLOD = 0u;
  SparseVoxelTerrainTree64DescriptorGPU refinedDescriptor = newDescriptor;
  refinedDescriptor.terrainGeneration = refinedState.generation;
  refinedDescriptor.internalLOD = refinedState.currentLOD;
  refinedDescriptor.geometryRevisionLo = 0x33445566u;
  SparseVoxelTree64DynamicInstanceGPU refinedInstance{};
  refinedInstance.worldToModel = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    -17.0f, -23.0f, -31.0f, 1.0f};
  refinedInstance.worldBoundsMinimum = {17.0f, 23.0f, 31.0f, 0.0f};
  refinedInstance.worldBoundsMaximum = {25.0f, 39.0f, 47.0f, 0.0f};
  refinedInstance.modelIndex = 0u;
  refinedInstance.stableInstanceId = 0u;
  refinedInstance.contentGeneration = 77u;
  refinedInstance.flags = kSparseVoxelTree64DynamicValid |
      kSparseVoxelTree64DynamicSkipReceiverCell |
      kSparseVoxelTree64DynamicTranslationOnly;
  refinedInstance.terrainSlot = 0u;
  refinedInstance.terrainGeneration = refinedState.generation;
  refinedInstance.regionMappingId = 19u;
  refinedInstance.receiverCellSizeBits = 0x3dcccccdu;
  SparseVoxelTerrainPublicationRecordGPU refinedRecord{};
  refinedRecord.terrainSlot = 0u;
  refinedRecord.flags = kSparseVoxelTerrainPublicationValid |
      kSparseVoxelTerrainPublicationDynamicInstance;
  refinedRecord.reserved0 = refinedInstance.contentGeneration;
  refinedRecord.reserved1 = refinedInstance.flags;
  refinedRecord.expectedPageState = pageStateWords(newState);
  refinedRecord.replacementPageState = pageStateWords(refinedState);
  refinedRecord.expectedTree64 = newDescriptor;
  refinedRecord.replacementTree64 = refinedDescriptor;
  refinedRecord.replacementDynamicInstance = refinedInstance;
  ++publishSettings.generation;
  assert(stage(publishSettings, &refinedRecord));
  const auto refined = execute();
  assert(refined.status.accepted == 1u);
  assert(refined.pageStates[0].generation == refinedState.generation);
  assert(refined.descriptors[0].internalLOD == refinedState.currentLOD);
  assert(std::memcmp(
      &refined.instances[0], &refinedInstance,
      sizeof(refinedInstance)) == 0);

  // A refine transaction keeps the incumbent parent page available to the
  // raster cut until all children are published, but lighting must stop
  // tracing that conservative parent at the same instant. Verify the
  // flags-only publication preserves every other instance word.
  SparseVoxelTerrainPublicationRecordGPU suppressLightingRecord{};
  suppressLightingRecord.terrainSlot = 0u;
  suppressLightingRecord.flags = kSparseVoxelTerrainPublicationValid |
      kSparseVoxelTerrainPublicationDynamicInstance |
      kSparseVoxelTerrainPublicationDynamicFlagsOnly;
  suppressLightingRecord.reserved1 = refinedInstance.flags |
      kSparseVoxelTree64DynamicDisableAnyHit;
  suppressLightingRecord.expectedPageState = pageStateWords(refinedState);
  suppressLightingRecord.replacementPageState = pageStateWords(refinedState);
  suppressLightingRecord.expectedTree64 = refinedDescriptor;
  suppressLightingRecord.replacementTree64 = refinedDescriptor;
  ++publishSettings.generation;
  assert(stage(publishSettings, &suppressLightingRecord));
  const auto lightingSuppressed = execute();
  assert(lightingSuppressed.status.accepted == 1u);
  auto expectedSuppressedInstance = refinedInstance;
  expectedSuppressedInstance.flags |=
      kSparseVoxelTree64DynamicDisableAnyHit;
  assert(std::memcmp(
      &lightingSuppressed.instances[0], &expectedSuppressedInstance,
      sizeof(expectedSuppressedInstance)) == 0);

  // Exercise the inverse substitution on the same stable slot. This is the
  // runtime contract used when CPU hierarchy selection restores a coarser
  // parent after the GPU has generated its replacement page.
  SparseVoxelTerrainPageStateGPU coarsenedState = refinedState;
  ++coarsenedState.generation;
  coarsenedState.currentLOD = 3u;
  SparseVoxelTerrainTree64DescriptorGPU coarsenedDescriptor =
      refinedDescriptor;
  coarsenedDescriptor.terrainGeneration = coarsenedState.generation;
  coarsenedDescriptor.internalLOD = coarsenedState.currentLOD;
  coarsenedDescriptor.geometryRevisionLo = 0x778899aau;
  auto coarsenedInstance = refinedInstance;
  coarsenedInstance.worldToModel[12] = -8.0f;
  coarsenedInstance.worldToModel[13] = -16.0f;
  coarsenedInstance.worldToModel[14] = -24.0f;
  coarsenedInstance.worldBoundsMinimum = {8.0f, 16.0f, 24.0f, 0.0f};
  coarsenedInstance.worldBoundsMaximum = {40.0f, 48.0f, 56.0f, 0.0f};
  ++coarsenedInstance.contentGeneration;
  coarsenedInstance.terrainGeneration = coarsenedState.generation;
  SparseVoxelTerrainPublicationRecordGPU coarsenedRecord{};
  coarsenedRecord.terrainSlot = 0u;
  coarsenedRecord.flags = kSparseVoxelTerrainPublicationValid |
      kSparseVoxelTerrainPublicationDynamicInstance;
  coarsenedRecord.reserved0 = coarsenedInstance.contentGeneration;
  coarsenedRecord.reserved1 = coarsenedInstance.flags;
  coarsenedRecord.expectedPageState = pageStateWords(refinedState);
  coarsenedRecord.replacementPageState = pageStateWords(coarsenedState);
  coarsenedRecord.expectedTree64 = refinedDescriptor;
  coarsenedRecord.replacementTree64 = coarsenedDescriptor;
  coarsenedRecord.replacementDynamicInstance = coarsenedInstance;
  ++publishSettings.generation;
  assert(stage(publishSettings, &coarsenedRecord));
  const auto coarsened = execute();
  assert(coarsened.status.accepted == 1u);
  assert(coarsened.pageStates[0].generation == coarsenedState.generation);
  assert(coarsened.descriptors[0].internalLOD ==
      coarsenedState.currentLOD);
  assert(std::memcmp(
      &coarsened.instances[0], &coarsenedInstance,
      sizeof(coarsenedInstance)) == 0);

  // Replaying the old refinement command must fail its CPU-authored CAS and
  // leave all three ownership components byte-identical.
  ++publishSettings.generation;
  assert(stage(publishSettings, &refinedRecord));
  const auto staleRefinement = execute();
  assert(staleRefinement.status.accepted == 0u);
  assert(staleRefinement.status.rejectedRecord == 0u);
  assert(std::memcmp(
      &staleRefinement.pageStates[0], &coarsenedState,
      sizeof(coarsenedState)) == 0);
  assert(std::memcmp(
      &staleRefinement.descriptors[0], &coarsenedDescriptor,
      sizeof(coarsenedDescriptor)) == 0);
  assert(std::memcmp(
      &staleRefinement.instances[0], &coarsenedInstance,
      sizeof(coarsenedInstance)) == 0);
  publishSettings.generation -= 3u;

  // Validation covers the complete record set before any record or generic
  // visibility word is written. A stale second owner must therefore prevent
  // an otherwise valid first owner from advancing.
  resetOld();
  auto staleSecondExpected = SparseVoxelTerrainPageStateGPU{};
  staleSecondExpected.generation = 99u;
  SparseVoxelTerrainPublicationRecordGPU staleSecond{};
  staleSecond.terrainSlot = 1u;
  staleSecond.flags = kSparseVoxelTerrainPublicationValid;
  staleSecond.expectedPageState = pageStateWords(staleSecondExpected);
  const std::array<SparseVoxelTerrainPublicationRecordGPU, 2> mixedRecords{
    record, staleSecond};
  publishSettings.publicationCount = 2u;
  assert(stage(publishSettings, mixedRecords.data()));
  const auto mixed = execute();
  assert(mixed.status.accepted == 0u);
  assert(mixed.status.rejectedRecord == 1u);
  assert(mixed.status.publishedRecordCount == 0u);
  assert(mixed.activation[0] == oldActivation[0]);
  assert(mixed.pageStates[0].generation == oldState.generation);
  assert(mixed.descriptors[0].nodePageSlot == oldDescriptor.nodePageSlot);
  publishSettings.publicationCount = 1u;

  // A stale raster owner rejects the complete batch: neither the activation
  // word nor the Tree64 descriptor may advance.
  resetOld();
  auto staleState = oldState;
  ++staleState.generation;
  states[0] = staleState;
  graph.bufferWrite(pageStatesBuffer, 0u, sizeof(states), states.data());
  assert(stage(publishSettings, &record));
  const auto stale = execute();
  assert(stale.status.accepted == 0u);
  assert(stale.activation[0] == oldActivation[0]);
  assert(stale.pageStates[0].generation == staleState.generation);
  assert(stale.descriptors[0].nodePageSlot == oldDescriptor.nodePageSlot);

  // Publication freeze uses the same retain-complete path.
  resetOld();
  publishSettings.freezePublication = 1u;
  assert(stage(publishSettings, &record));
  const auto frozen = execute();
  assert(frozen.status.accepted == 0u);
  assert(frozen.activation[0] == oldActivation[0]);
  assert(frozen.pageStates[0].generation == oldState.generation);
  assert(frozen.descriptors[0].nodePageSlot == oldDescriptor.nodePageSlot);

  // Descriptor overflow is rejected before any raster-side visibility write.
  publishSettings.freezePublication = 0u;
  auto overflowing = record;
  overflowing.replacementTree64.nodeCount =
      publishSettings.nodesPerPage + 1u;
  assert(stage(publishSettings, &overflowing));
  const auto overflow = execute();
  assert(overflow.status.accepted == 0u);
  assert(overflow.activation[0] == oldActivation[0]);
  assert(overflow.pageStates[0].generation == oldState.generation);
  assert(overflow.descriptors[0].nodePageSlot == oldDescriptor.nodePageSlot);

  // Material-sidecar overflow has the same transaction-wide retain path.
  resetOld();
  auto materialOverflowing = record;
  materialOverflowing.replacementTree64.materialCellCount =
      publishSettings.materialCellsPerPage + 1u;
  assert(stage(publishSettings, &materialOverflowing));
  const auto materialOverflow = execute();
  assert(materialOverflow.status.accepted == 0u);
  assert(materialOverflow.activation[0] == oldActivation[0]);
  assert(materialOverflow.pageStates[0].generation == oldState.generation);
  assert(materialOverflow.descriptors[0].nodePageSlot ==
      oldDescriptor.nodePageSlot);

  // A recycled physical material page is incomplete for this descriptor,
  // even if every logical raster and Tree64 owner still matches.
  resetOld();
  const std::array<uint32_t, 2> staleMaterialGenerations{5u, 8u};
  graph.bufferWrite(
      materialGenerationsBuffer, 0u, sizeof(staleMaterialGenerations),
      const_cast<uint32_t *>(staleMaterialGenerations.data()));
  assert(stage(publishSettings, &record));
  const auto staleMaterial = execute();
  assert(staleMaterial.status.accepted == 0u);
  assert(staleMaterial.activation[0] == oldActivation[0]);
  assert(staleMaterial.pageStates[0].generation == oldState.generation);
  assert(staleMaterial.descriptors[0].nodePageSlot ==
      oldDescriptor.nodePageSlot);

  // Publication completion is consumed asynchronously by the renderer. Keep
  // its acceptance result tied to the submitted frame even when a following
  // no-op frame has already reset and executed the same pass.
  resetOld();
  graph.bufferWrite(
      materialGenerationsBuffer, 0u, sizeof(generations),
      const_cast<uint32_t *>(generations.data()));
  assert(stage(publishSettings, &record));
  const auto delayedPublication = execute();
  assert(delayedPublication.status.accepted == 1u);
  assert(stage({}));
  const auto laterNoOp = execute();
  assert(laterNoOp.status.accepted == 1u);
  assert(laterNoOp.status.generation == publishSettings.generation);
  assert(laterNoOp.status.publishedRecordCount == 1u);

  SparseVoxelTerrainPublicationStatusGPU publicationFrameStatus{};
  assert(upload->readPublicationStatus(publicationFrameStatus));
  assert(publicationFrameStatus.accepted == 1u);
  assert(publicationFrameStatus.generation == publishSettings.generation);
  assert(publicationFrameStatus.publishedRecordCount == 1u);

  // Exercise the real GPU publication pass as a sustained rewrite stream,
  // not just a single swap. Every iteration first installs an unreachable
  // physical-page generation, proves the old raster/Tree64 owner is still
  // published, and then atomically advances both identities. Periodic stale
  // transactions must leave the last accepted generation byte-for-byte live.
  constexpr uint32_t rewriteCount = 128u;
  SparseVoxelTerrainPageStateGPU currentState = newState;
  SparseVoxelTerrainTree64DescriptorGPU currentDescriptor = newDescriptor;
  uint32_t currentActivation = newActivation;
  for (uint32_t rewrite = 0u; rewrite < rewriteCount; ++rewrite)
  {
    const uint32_t physicalSlot = rewrite & 1u;
    const uint32_t physicalGeneration = 100u + rewrite;
    const std::array<uint32_t, 2> pageGenerations{
      physicalGeneration, physicalGeneration};
    graph.bufferWrite(
        staging, 0u, sizeof(pageGenerations),
        const_cast<uint32_t *>(pageGenerations.data()));

    UploadPass::SettingsGPU pageUpload{};
    pageUpload.generation = 1000u + rewrite * 3u;
    pageUpload.atlasWidth = 1u;
    pageUpload.uploadWords = 2u;
    pageUpload.sectionCount = 2u;
    pageUpload.sections[0] = {
      0u, physicalSlot, 1u,
      static_cast<uint32_t>(
          UploadPass::Destination::Tree64NodePageGenerations)};
    pageUpload.sections[1] = {
      1u, physicalSlot, 1u,
      static_cast<uint32_t>(
          UploadPass::Destination::Tree64MaterialPageGenerations)};
    assert(stage(pageUpload));
    const auto installedUnreachable = execute();
    assert(installedUnreachable.pageStates[0].generation ==
        currentState.generation);
    assert(installedUnreachable.pageStates[0].currentLOD ==
        currentState.currentLOD);
    assert(std::memcmp(
        &installedUnreachable.descriptors[0], &currentDescriptor,
        sizeof(currentDescriptor)) == 0);

    SparseVoxelTerrainPageStateGPU replacementState = currentState;
    replacementState.generation = 5u + rewrite;
    replacementState.currentLOD = rewrite & 3u;
    replacementState.publishedFrame = 200u + rewrite;
    SparseVoxelTerrainTree64DescriptorGPU replacementDescriptor =
        currentDescriptor;
    replacementDescriptor.terrainGeneration = replacementState.generation;
    replacementDescriptor.internalLOD = replacementState.currentLOD;
    replacementDescriptor.nodePageSlot = physicalSlot;
    replacementDescriptor.nodePageGeneration = physicalGeneration;
    replacementDescriptor.nodeCount = 4u + (rewrite & 3u);
    replacementDescriptor.materialPageSlot = physicalSlot;
    replacementDescriptor.materialPageGeneration = physicalGeneration;
    replacementDescriptor.materialCellCount = 1u + (rewrite & 1u);
    replacementDescriptor.geometryRevisionLo = 0xabc00000u + rewrite;
    replacementDescriptor.geometryRevisionHi = rewrite >> 4u;

    SparseVoxelTerrainPublicationRecordGPU rewriteRecord{};
    rewriteRecord.terrainSlot = 0u;
    rewriteRecord.flags = kSparseVoxelTerrainPublicationValid;
    rewriteRecord.expectedPageState = pageStateWords(currentState);
    rewriteRecord.replacementPageState = pageStateWords(replacementState);
    rewriteRecord.expectedTree64 = currentDescriptor;
    rewriteRecord.replacementTree64 = replacementDescriptor;
    const uint32_t replacementActivation = 0x40000000u + rewrite;
    graph.bufferWrite(
        staging, 0u, sizeof(replacementActivation),
        const_cast<uint32_t *>(&replacementActivation));
    UploadPass::SettingsGPU rewriteSettings{};
    rewriteSettings.generation = pageUpload.generation + 1u;
    rewriteSettings.atlasWidth = 1u;
    rewriteSettings.uploadWords = 1u;
    rewriteSettings.sectionCount = 1u;
    rewriteSettings.publicationCount = 1u;
    rewriteSettings.nodesPerPage = 16u;
    rewriteSettings.materialCellsPerPage = 8u;
    rewriteSettings.sections[0] = {
      0u, 0u, 1u,
      static_cast<uint32_t>(UploadPass::Destination::BrickNodes)};

    if ((rewrite % 11u) == 5u)
    {
      auto staleRecord = rewriteRecord;
      ++staleRecord.expectedPageState[0];
      assert(stage(rewriteSettings, &staleRecord));
      const auto rejectedRewrite = execute();
      assert(rejectedRewrite.status.accepted == 0u);
      assert(rejectedRewrite.activation[0] == currentActivation);
      assert(std::memcmp(
          &rejectedRewrite.pageStates[0], &currentState,
          sizeof(currentState)) == 0);
      assert(std::memcmp(
          &rejectedRewrite.descriptors[0], &currentDescriptor,
          sizeof(currentDescriptor)) == 0);
    }

    assert(stage(rewriteSettings, &rewriteRecord));
    const auto installed = execute();
    assert(installed.status.accepted == 1u);
    assert(installed.status.publishedRecordCount == 1u);
    assert(installed.activation[0] == replacementActivation);
    assert(std::memcmp(
        &installed.pageStates[0], &replacementState,
        sizeof(replacementState)) == 0);
    assert(std::memcmp(
        &installed.descriptors[0], &replacementDescriptor,
        sizeof(replacementDescriptor)) == 0);
    currentState = replacementState;
    currentDescriptor = replacementDescriptor;
    currentActivation = replacementActivation;
  }
}

} // namespace

int main()
{
  os::Logger::start();
  using namespace ::rendering::backend;
  ::rendering::DeviceRequiredLimits limits{
    .minimumMemory = 0u,
    .minimumComputeSharedMemory = 0u,
    .minimumComputeWorkGroupInvocations = 64u,
  };
  const ::rendering::DeviceFeatures features =
      ::rendering::DeviceFeatures::DeviceFeatures_Compute;
  vulkan::VulkanRHI rhi(vulkan::Vulkan_1_2, limits, features, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  testUnpublishedPageIsolation(rhi, false);
  testUnpublishedPageIsolation(rhi, true);
  testAtomicTerrainPublication(rhi);
  os::Logger::shutdown();
  return 0;
}
