#include "os/Logger.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "virtualgeometry/VirtualGeometryGeneratedPayloadManager.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace vg = virtualgeometry;

namespace
{

int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

bool isExpectedGpuBackendUnavailable(const std::exception &error)
{
  const std::string message = error.what();
#if defined(__APPLE__)
  if (message.find("failed to create instance") != std::string::npos)
    return true;
#endif
  return message.find("Vulkan is not supported on this device") != std::string::npos;
}

uint32_t floatBits(const float value)
{
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(uint32_t));
  return bits;
}

std::vector<vg::VirtualGeometryHierarchy> makeHierarchy(
    const uint32_t pageCount = 2u)
{
  std::vector<vg::VirtualGeometryHierarchy> hierarchy(pageCount);
  for (uint32_t i = 0u; i < pageCount; ++i)
  {
    auto &node = hierarchy[i];
    node.min_x = static_cast<float>(i) * 64.0f;
    node.min_y = 0.0f;
    node.min_z = 0.0f;
    node.max_x = node.min_x + 64.0f;
    node.max_y = 32.0f;
    node.max_z = 64.0f;
    node.max_center_x = 32.0f + node.min_x;
    node.max_center_y = 16.0f;
    node.max_center_z = 32.0f;
    node.max_radius = 48.0f;
    node.min_lod_error = 0.25f;
    node.max_parent_lod_error = i == 0u ? 16.0f : 4.0f;
    node.child_start = 0u;
    node.child_count = 1u;
    node.pageIndex = i;
    node.meshPartIndex = 0u;
    node.flags = vg::HIERARCHY_LEAF_FLAG;
  }
  return hierarchy;
}

void registerRuntimeObject(
    vg::VirtualGeometryScene &scene,
    const std::string &name,
    const uint32_t pageCount)
{
  vg::VirtualGeometryScene::RuntimeObjectCreateInfo info{};
  info.name = name;
  info.pageCount = pageCount;
  info.hierarchy = makeHierarchy(pageCount);
  info.quantizationConfig.unit_scale = 1.0f;
  const float minPoint[3] = {0.0f, 0.0f, 0.0f};
  const float maxPoint[3] = {128.0f, 32.0f, 64.0f};
  info.localBounds.expandBy(minPoint);
  info.localBounds.expandBy(maxPoint);
  std::string error;
  check(scene.registerRuntimeObject(info, nullptr, &error), "runtime object registers: " + error);
}

vg::VirtualGeometryGeneratedPayloadManager::Request makeRequest(
    const std::string &objectName,
    const uint32_t page,
    const uint64_t generation,
    const bool root)
{
  vg::VirtualGeometryGeneratedPayloadManager::Request request{};
  request.objectName = objectName;
  request.localPageIndex = page;
  request.parentLocalPageIndex = root ? UINT32_MAX : 0u;
  request.fallbackLocalPageIndex = UINT32_MAX;
  request.pageGeneration = generation;
  request.geometryRevision = generation + 100u;
  request.materialRevision = generation + 200u;
  request.unitScaleBits = floatBits(1.0f);
  request.encodedPageBytes = root ? 256u : 0u;
  request.clusterCount = root ? 2u : 3u;
  request.decodedVertexCount = root ? 12u : 18u;
  request.decodedIndexCount = root ? 18u : 27u;
  request.priority = root ? 10u : 20u;
  request.rootPage = root;
  request.hasTracePayload = true;
  request.hasMaterialPayload = false;
  request.requireProtectedParentOrFallback = !root;
  return request;
}

vg::PageTableEntry readPageTableEntry(
    rendering::RenderGraph &renderGraph,
    const vg::VirtualGeometryScene &scene,
    const uint32_t globalPageTableIndex)
{
  vg::PageTableEntry entry{};
  renderGraph.bufferRead(
      scene.pageTableBuffer,
      static_cast<uint64_t>(globalPageTableIndex) * sizeof(vg::PageTableEntry),
      sizeof(vg::PageTableEntry),
      [&](const void *data)
      {
        std::memcpy(&entry, data, sizeof(entry));
      });
  return entry;
}

void testGeneratedPayloadLifecycle(rendering::RenderGraph &renderGraph)
{
  std::cout << "generated VG payload manager reserves, publishes, and cancels GPU-owned pages\n";
  vg::VirtualGeometryScene scene(&renderGraph, 64u * 1024u, 1024u * 1024u);
  registerRuntimeObject(scene, "generated-vg-object", 2u);

  vg::VirtualGeometryGeneratedPayloadManager manager(scene);
  std::string error;
  check(manager.enqueueRequest(makeRequest("generated-vg-object", 0u, 10u, true), &error), "root request enqueues: " + error);
  check(manager.enqueueRequest(makeRequest("generated-vg-object", 0u, 9u, true), &error), "older duplicate request is accepted as deduped: " + error);

  std::vector<vg::VirtualGeometryGeneratedPayloadManager::GpuWriteDescriptor> descriptors;
  check(manager.reserveReady(1u, &descriptors) == 1u, "one request reserves scene GPU write ranges");
  check(descriptors.size() == 1u, "reservation exposes one GPU write descriptor");
  check(descriptors[0].localPageIndex == 0u && descriptors[0].clusterCount == 2u, "descriptor carries page and cluster ranges");
  check(descriptors[0].encodedPageByteSize == 256u, "descriptor carries encoded page range for encoded GPU output");
  check(descriptors[0].globalPageTableIndex != UINT32_MAX, "descriptor exposes a concrete global VG page-table index");
  check(descriptors[0].encodedPageByteOffset != UINT32_MAX, "descriptor exposes the GPU encoded-page write offset");
  check(descriptors[0].decodedVertexCount == 12u && descriptors[0].decodedIndexCount == 18u, "descriptor exposes exact decoded geometry budgets");
  check(descriptors[0].prioritySlot != UINT32_MAX, "descriptor exposes a permanent priority slot");

  vg::VirtualGeometryGeneratedPayloadManager::PublishInfo publishRoot{};
  publishRoot.ticketId = descriptors[0].ticketId;
  publishRoot.gpuWroteEncodedPageData = true;
  publishRoot.gpuWroteDecodedGeometry = false;
  publishRoot.enqueuePageDecode = false;
  check(manager.publishCompleted(publishRoot, &error), "root publish installs the generated page: " + error);

  vg::VirtualGeometryScene::RuntimeObjectDiagnostics diagnostics{};
  check(scene.getRuntimeObjectDiagnostics("generated-vg-object", diagnostics), "diagnostics resolve generated object");
  const auto rootEntry = readPageTableEntry(renderGraph, scene, diagnostics.pageTableOffset);
  check(rootEntry.isInstalled == 1u && rootEntry.clusterCount == 2u, "root page table entry is resident");
  check(rootEntry.bufferOffset == descriptors[0].encodedPageByteOffset && rootEntry.size == descriptors[0].encodedPageByteSize, "root page table aliases the GPU encoded payload range");
  check(rootEntry.decodedVertexOffset == descriptors[0].decodedVertexOffset && rootEntry.decodedIndexOffset == descriptors[0].decodedIndexOffset, "root page table aliases decoded geometry ranges");
  check(scene.consumePendingPageDecodeRequests().empty(), "publish can skip decode when producer already owns scheduling");
  const auto *rootStatus = scene.getRuntimeObjectPageStatus("generated-vg-object", 0u);
  check(rootStatus != nullptr && rootStatus->sceneEncodedPageDataReady, "root status records that the GPU wrote scene encoded page data");
  check(rootStatus != nullptr && rootStatus->pageAllocation.clusterCount == 2u, "root status preserves generated cluster count");

  auto childRequest = makeRequest("generated-vg-object", 1u, 11u, false);
  check(manager.enqueueRequest(childRequest, &error), "child request enqueues after parent is installed: " + error);
  descriptors.clear();
  check(manager.reserveReady(1u, &descriptors) == 1u, "child request reserves decoded GPU write ranges");
  check(descriptors[0].encodedPageByteSize == 0u, "decoded-only page does not reserve encoded bytes");
  check(descriptors[0].decodedVertexCount == 18u && descriptors[0].decodedIndexCount == 27u, "decoded-only descriptor carries exact decoded geometry budget");

  vg::VirtualGeometryGeneratedPayloadManager::PublishInfo publishChild{};
  publishChild.ticketId = descriptors[0].ticketId;
  publishChild.gpuWroteEncodedPageData = false;
  publishChild.gpuWroteDecodedGeometry = true;
  publishChild.enqueuePageDecode = false;
  check(manager.publishCompleted(publishChild, &error), "child publish accepts decoded GPU-owned geometry: " + error);

  const auto *childStatus = scene.getRuntimeObjectPageStatus("generated-vg-object", 1u);
  check(childStatus != nullptr && childStatus->installed, "child page becomes resident");
  check(childStatus != nullptr && childStatus->parentLocalPageIndex == 0u, "child page records protected parent relation");
  check(childStatus != nullptr && !childStatus->sceneEncodedPageDataReady, "decoded-only child does not claim encoded page data");

  auto sameGenerationNewRevision =
      makeRequest("generated-vg-object", 1u, 11u, false);
  sameGenerationNewRevision.geometryRevision = 0x7777u;
  sameGenerationNewRevision.materialRevision = 0x8888u;
  check(
      manager.enqueueRequest(sameGenerationNewRevision, &error),
      "same-generation replacement with a new revision enqueues: " + error);
  descriptors.clear();
  check(
      manager.reserveReady(1u, &descriptors) == 1u,
      "same-generation replacement reserves with a normalized identity");
  check(
      descriptors[0].pageGeneration == 12u &&
          descriptors[0].geometryRevision ==
              sameGenerationNewRevision.geometryRevision,
      "manager bumps generated page generation when revisions change");
  vg::VirtualGeometryGeneratedPayloadManager::PublishInfo publishRevisionChange{};
  publishRevisionChange.ticketId = descriptors[0].ticketId;
  publishRevisionChange.gpuWroteEncodedPageData = false;
  publishRevisionChange.gpuWroteDecodedGeometry = true;
  publishRevisionChange.enqueuePageDecode = false;
  check(
      manager.publishCompleted(publishRevisionChange, &error),
      "revision-changing replacement publishes without VG generation aliasing: " +
          error);
  childStatus = scene.getRuntimeObjectPageStatus("generated-vg-object", 1u);
  check(
      childStatus != nullptr && childStatus->pageGeneration == 12u &&
          childStatus->geometryRevision ==
              sameGenerationNewRevision.geometryRevision,
      "runtime status records the normalized replacement generation");

  auto replacement = makeRequest("generated-vg-object", 1u, 12u, false);
  check(manager.enqueueRequest(replacement, &error), "replacement request enqueues: " + error);
  descriptors.clear();
  check(manager.reserveReady(1u, &descriptors) == 1u, "replacement reserves");
  check(manager.cancel(descriptors[0].ticketId, &error), "replacement reservation cancels cleanly: " + error);
  childStatus = scene.getRuntimeObjectPageStatus("generated-vg-object", 1u);
  check(childStatus != nullptr && childStatus->pageGeneration == 12u, "cancel leaves current resident page intact");

  const auto stats = manager.getStats();
  check(stats.pagesPublished == 3u, "stats count published generated pages");
  check(stats.reservationsCancelled == 1u, "stats count cancelled reservations");
  check(stats.requestsDeduplicated == 1u, "stats count deduped requests");
  check(stats.pendingRequestCount == 0u && stats.activeReservationCount == 0u, "manager drains pending and active work");
}

void testParentFallbackProtection(rendering::RenderGraph &renderGraph)
{
  std::cout << "generated VG payload manager preserves parent/fallback protection\n";
  vg::VirtualGeometryScene scene(&renderGraph, 64u * 1024u, 1024u * 1024u);
  registerRuntimeObject(scene, "protected-generated-vg-object", 2u);

  vg::VirtualGeometryGeneratedPayloadManager manager(scene);
  std::string error;
  check(manager.enqueueRequest(makeRequest("protected-generated-vg-object", 1u, 20u, false), &error), "protected child request enqueues: " + error);

  std::vector<vg::VirtualGeometryGeneratedPayloadManager::GpuWriteDescriptor> descriptors;
  check(manager.reserveReady(1u, &descriptors) == 1u, "protected child reserves before install validation");

  vg::VirtualGeometryGeneratedPayloadManager::PublishInfo publish{};
  publish.ticketId = descriptors[0].ticketId;
  publish.gpuWroteEncodedPageData = false;
  publish.gpuWroteDecodedGeometry = true;
  publish.enqueuePageDecode = false;
  check(!manager.publishCompleted(publish, &error), "publish rejects child when no parent/fallback is resident");
  check(error.find("parent") != std::string::npos || error.find("fallback") != std::string::npos, "rejection explains missing protection");
  check(manager.hasActiveReservation(descriptors[0].ticketId), "failed publish keeps reservation active for retry/cancel");
  check(manager.cancel(descriptors[0].ticketId, &error), "failed protected reservation can be cancelled: " + error);
}

void testRuntimeVGFeedbackStreamsGeneratedChild(rendering::RenderGraph &renderGraph)
{
  std::cout << "runtime VG feedback streams a GPU-generated child page\n";
  constexpr const char *objectName = "feedback-generated-vg-object";
  vg::VirtualGeometryScene scene(&renderGraph, 64u * 1024u, 1024u * 1024u);
  registerRuntimeObject(scene, objectName, 3u);

  vg::VirtualGeometryGeneratedPayloadManager manager(scene);
  std::string error;
  check(
      manager.enqueueRequest(makeRequest(objectName, 0u, 30u, true), &error),
      "root request enqueues before feedback loop: " + error);
  std::vector<vg::VirtualGeometryGeneratedPayloadManager::GpuWriteDescriptor>
      descriptors;
  check(manager.reserveReady(1u, &descriptors) == 1u, "root reserves");
  vg::VirtualGeometryGeneratedPayloadManager::PublishInfo publishRoot{};
  publishRoot.ticketId = descriptors[0].ticketId;
  publishRoot.gpuWroteEncodedPageData = true;
  publishRoot.gpuWroteDecodedGeometry = false;
  publishRoot.enqueuePageDecode = false;
  check(
      manager.publishCompleted(publishRoot, &error),
      "root publishes before child feedback: " + error);

  vg::VirtualGeometryScene::RuntimeObjectDiagnostics diagnostics{};
  check(
      scene.getRuntimeObjectDiagnostics(objectName, diagnostics),
      "feedback object diagnostics resolve");
  const std::array<uint32_t, 3u> priorities{90u, 200u, 100u};
  renderGraph.bufferWrite(
      scene.pagePriorityBuffer,
      static_cast<uint64_t>(diagnostics.prioritySlotOffset) *
          sizeof(uint32_t),
      sizeof(priorities),
      const_cast<uint32_t *>(priorities.data()));

  const auto feedbackRequests =
      scene.collectRuntimeObjectPageRequests(objectName, 8u);
  check(
      feedbackRequests.size() == 2u,
      "collector skips already-installed root page");
  check(
      feedbackRequests.size() >= 2u &&
          feedbackRequests[0].localPageIndex == 1u &&
          feedbackRequests[0].priority == 200u &&
          feedbackRequests[1].localPageIndex == 2u &&
          feedbackRequests[1].priority == 100u,
      "collector returns missing runtime pages ordered by VG priority");

  auto childRequest =
      makeRequest(objectName, feedbackRequests.front().localPageIndex, 31u, false);
  childRequest.priority = feedbackRequests.front().priority;
  check(
      manager.enqueueRequest(childRequest, &error),
      "VG feedback request enqueues generated child payload: " + error);
  descriptors.clear();
  check(manager.reserveReady(1u, &descriptors) == 1u, "feedback child reserves");
  check(
      descriptors[0].localPageIndex ==
              feedbackRequests.front().localPageIndex &&
          descriptors[0].prioritySlot ==
              feedbackRequests.front().prioritySlot,
      "feedback reservation targets the exact VG-requested runtime page");

  vg::VirtualGeometryGeneratedPayloadManager::PublishInfo publishChild{};
  publishChild.ticketId = descriptors[0].ticketId;
  publishChild.gpuWroteEncodedPageData = false;
  publishChild.gpuWroteDecodedGeometry = true;
  publishChild.enqueuePageDecode = false;
  check(
      manager.publishCompleted(publishChild, &error),
      "VG feedback child publishes decoded GPU-owned geometry: " + error);

  const auto *childStatus = scene.getRuntimeObjectPageStatus(
      objectName, feedbackRequests.front().localPageIndex);
  check(
      childStatus != nullptr && childStatus->installed,
      "feedback child becomes resident");
  check(
      childStatus != nullptr && childStatus->parentLocalPageIndex == 0u,
      "feedback child preserves installed parent fallback");
  const auto remainingRequests =
      scene.collectRuntimeObjectPageRequests(objectName, 8u);
  check(
      std::none_of(
          remainingRequests.begin(),
          remainingRequests.end(),
          [](const auto &request) { return request.localPageIndex == 1u; }),
      "installed feedback child is not requested again on the next loop");
}

void runTests(rendering::backend::vulkan::VulkanRHI &rhi)
{
  rendering::RenderGraph renderGraph(&rhi);
  testGeneratedPayloadLifecycle(renderGraph);
  testParentFallbackProtection(renderGraph);
  testRuntimeVGFeedbackStreamsGeneratedChild(renderGraph);
}

} // namespace

int main()
{
  os::Logger::start(100);

  rendering::DeviceRequiredLimits limits{0u, 0u, 0u};
  rendering::DeviceFeatures features = rendering::DeviceFeatures::DeviceFeatures_Compute;
  std::unique_ptr<rendering::backend::vulkan::VulkanRHI> rhi;
  try
  {
    rhi = std::make_unique<rendering::backend::vulkan::VulkanRHI>(
        rendering::backend::vulkan::Vulkan_1_2,
        limits,
        features,
        std::vector<std::string>{});
    std::vector<VkSurfaceKHR> surfaces;
    rhi->init(surfaces);
    runTests(*rhi);
  }
  catch (const std::exception &gpuError)
  {
    if (!isExpectedGpuBackendUnavailable(gpuError))
      throw;

    os::Logger::warningf("[Test] VirtualGeometryGeneratedPayloadManager tests skipped: %s", gpuError.what());
    os::Logger::shutdown();
    std::cout << "VirtualGeometryGeneratedPayloadManagerTests passed (GPU backend skipped)\n";
    return 0;
  }

  os::Logger::shutdown();
  if (failures != 0)
  {
    std::cerr << "VirtualGeometryGeneratedPayloadManagerTests failed with " << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "VirtualGeometryGeneratedPayloadManagerTests passed\n";
  return 0;
}
