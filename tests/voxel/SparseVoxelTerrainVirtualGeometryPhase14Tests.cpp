#include "voxel/svraster/SparseVoxelTerrainVirtualGeometrySceneBridge.hpp"

#include "os/Logger.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace svr = voxel::svraster;
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

std::vector<vg::VirtualGeometryHierarchy> makeRuntimeHierarchy(const uint32_t localPageIndex)
{
  vg::VirtualGeometryHierarchy root{};
  root.min_x = 0.0f;
  root.min_y = 0.0f;
  root.min_z = 0.0f;
  root.max_x = 128.0f;
  root.max_y = 48.0f;
  root.max_z = 128.0f;
  root.max_center_x = 64.0f;
  root.max_center_y = 24.0f;
  root.max_center_z = 64.0f;
  root.max_radius = 96.0f;
  root.min_lod_error = 0.5f;
  root.max_parent_lod_error = 16.0f;
  root.child_start = UINT32_MAX;
  root.child_count = 0u;
  root.pageIndex = localPageIndex;
  root.meshPartIndex = 0u;
  root.flags = vg::HIERARCHY_LEAF_FLAG;
  return {root};
}

vg::VirtualGeometryPage makeDrawableRuntimePage()
{
  vg::VirtualGeometryPage page{};
  page.groupCount = 1u;
  page.groupMeshletOffsets = {0u};
  vg::Meshlet meshlet{};
  meshlet.vertex_count = 3u;
  meshlet.triangle_count = 1u;
  meshlet.self.center[0] = 0.5f;
  meshlet.self.center[1] = 0.0f;
  meshlet.self.center[2] = 0.5f;
  meshlet.self.radius = 1.0f;
  meshlet.self.error = 0.0f;
  meshlet.parent = meshlet.self;
  meshlet.parent.error = 1.0f;
  meshlet.cone.axis[1] = 1.0f;
  meshlet.cone.cutoff = -1.0f;
  page.meshlets.push_back(meshlet);
  page.vertexBuffers.normals = {0u, 0u, 0u};
  page.vertexBuffers.uvs = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
  page.vertexBuffers.indices = {0u, 1u, 2u, 0u};
  return page;
}

svr::SparseVoxelTerrainVGScenePageInstall makeTerrainInstall(
    const uint32_t localPageIndex,
    const uint32_t prioritySlot,
    const uint64_t generation,
    const uint64_t geometryRevision,
    const uint32_t clusterCount)
{
  svr::SparseVoxelTerrainVGScenePageInstall install{};
  install.page = {0x5445525241494e31ull, localPageIndex};
  install.pageGeneration = generation;
  install.geometryRevision = geometryRevision;
  install.materialRevision = geometryRevision + 1u;
  install.traceFormat = svr::kVoxelTraceFormatCubic64;
  install.localPageIndex = localPageIndex;
  install.globalPageTableIndex = localPageIndex;
  install.prioritySlot = prioritySlot;
  install.pageSlot = localPageIndex;
  install.pagelet = 0u;
  install.readyForSceneInstall = true;
  install.hasTracePayload = true;
  install.hasMaterialPayload = true;

  install.virtualPageAllocation.pageId = localPageIndex;
  install.virtualPageAllocation.bufferOffset = 256u * localPageIndex;
  install.virtualPageAllocation.size = 256u;
  install.virtualPageAllocation.clusterOffset = 32u * localPageIndex;
  install.virtualPageAllocation.clusterCount = clusterCount;
  install.virtualPageAllocation.unitScaleBits = floatBits(1.0f);
  install.virtualPageAllocation.decodedVertexOffset = 1024u + localPageIndex * 128u;
  install.virtualPageAllocation.decodedVertexCount = clusterCount * 4u;
  install.virtualPageAllocation.decodedIndexOffset = 2048u + localPageIndex * 192u;
  install.virtualPageAllocation.decodedIndexCount = clusterCount * 6u;
  install.virtualPageAllocation.decodedBoneWeightOffset = 0u;
  install.virtualPageAllocation.decodedBoneWeightCount = 0u;
  install.virtualPageAllocation.prioritySlot = prioritySlot;
  install.virtualPageAllocation.isInstalled = true;

  install.pageTableEntry.bufferOffset = install.virtualPageAllocation.bufferOffset;
  install.pageTableEntry.size = install.virtualPageAllocation.size;
  install.pageTableEntry.clusterOffset = install.virtualPageAllocation.clusterOffset;
  install.pageTableEntry.clusterCount = install.virtualPageAllocation.clusterCount;
  install.pageTableEntry.isInstalled = 1u;
  install.pageTableEntry.prioritySlot = prioritySlot;
  install.pageTableEntry.unitScaleBits = install.virtualPageAllocation.unitScaleBits;
  install.pageTableEntry.decodedVertexOffset = install.virtualPageAllocation.decodedVertexOffset;
  install.pageTableEntry.decodedIndexOffset = install.virtualPageAllocation.decodedIndexOffset;
  install.pageTableEntry.decodedBoneWeightOffset = install.virtualPageAllocation.decodedBoneWeightOffset;
  install.pageTableEntry._padding0 = UINT32_MAX;
  install.pageTableEntry._padding1 = 0u;
  return install;
}

svr::SparseVoxelTerrainVGPUPageBuildResult makeReadyGeneratedBuild(
    const uint32_t localPageIndex,
    const bool root)
{
  svr::SparseVoxelTerrainVGPUPageBuildResult build{};
  build.page = {0x5445525241494e31ull, localPageIndex};
  build.parentPage = root
      ? svr::SparseVoxelPageId{}
      : svr::SparseVoxelPageId{0x5445525241494e31ull, 0u};
  build.fallbackPage = build.parentPage;
  build.expectedPageGeneration = 10u + localPageIndex;
  build.requiredProducerRevision = 77u;
  build.payloadRevision = 100u + localPageIndex;
  build.geometryRevision = 200u + localPageIndex;
  build.materialRevision = 200u + localPageIndex;
  build.productionFence = 300u + localPageIndex;
  build.terrainGeneration = 400u + localPageIndex;
  build.buildRevision = 500u + localPageIndex;
  build.status =
      static_cast<uint32_t>(svr::SparseVoxelTerrainVGPUPageBuildStatus::Ready);
  build.stageFlags = svr::kSparseVoxelTerrainVGPURequiredStages;
  build.sourceVoxelCount = 512u;
  build.exposedFaceCount = 32u;
  build.greedyQuadCount = 32u;
  build.vgMeshletCount = root ? 1u : 2u;
  build.vgGroupCount = 1u;
  build.vgHierarchyNodeCount = 1u;
  build.vgInstallRewriteCount = 1u;
  build.vgUninstallRewriteCount = 1u;
  build.vgVertexCount = 128u;
  build.vgTriangleCount = 64u;
  build.vgMaxVerticesPerMeshlet = root ? 128u : 64u;
  build.vgMaxTrianglesPerMeshlet = root ? 64u : 32u;
  build.vgPositionWordCount = build.vgVertexCount;
  build.vgNormalWordCount = build.vgVertexCount;
  build.vgUVWordCount = build.vgVertexCount * 2u;
  build.vgIndexByteCount = build.vgTriangleCount * 3u;
  build.traceFormat = svr::kVoxelTraceFormatCubic64;
  build.traceNodeCount = 1u;
  build.traceErrorWordCount = 1u;
  build.cullErrorWordCount = 1u;
  build.dependencyCount = root ? 0u : 1u;
  build.rootPage = root;
  return build;
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

void testTerrainRuntimePageInstallsIntoScene(rendering::RenderGraph &renderGraph)
{
  std::cout << "terrain runtime page installs into VirtualGeometryScene\n";
  vg::VirtualGeometryScene scene(&renderGraph, 64u * 1024u, 1024u * 1024u);
  std::string error;
  vg::VirtualGeometryScene::RuntimeObjectId objectId = vg::VirtualGeometryScene::INVALID_RUNTIME_OBJECT_ID;

  check(
      svr::registerSparseVoxelTerrainVGRuntimeObject(
          scene,
          "terrain-runtime-phase14",
          3u,
          makeRuntimeHierarchy(0u),
          {},
          &objectId,
          &error),
      "runtime terrain object registers: " + error);

  vg::VirtualGeometryScene::RuntimeObjectDiagnostics diagnostics{};
  check(scene.getRuntimeObjectDiagnostics("terrain-runtime-phase14", diagnostics), "runtime diagnostics are visible");
  check(diagnostics.registered && !diagnostics.usesCPUCulling, "runtime object is GPU-owned and not CPU-culled");
  check(diagnostics.pageCount == 3u && diagnostics.hierarchyNodeCount == 1u, "runtime object reserves pages and hierarchy");
  check(objectId != vg::VirtualGeometryScene::INVALID_RUNTIME_OBJECT_ID, "runtime object id is valid");
  check(scene.getInstanceCount() == 1u, "runtime object creates one VG scene instance");
  check(scene.files.find("terrain-runtime-phase14") == scene.files.end(), "runtime object does not consume file-backed object storage");

  const auto install = makeTerrainInstall(0u, diagnostics.prioritySlotOffset, 10u, 20u, 4u);
  check(
      svr::installSparseVoxelTerrainVGScenePage(
          scene,
          "terrain-runtime-phase14",
          install,
          true,
          UINT32_MAX,
          UINT32_MAX,
          &error),
      "committed terrain descriptor installs root page: " + error);

  const auto pageTableEntry = readPageTableEntry(renderGraph, scene, diagnostics.pageTableOffset);
  check(pageTableEntry.isInstalled == 1u, "installed page is resident in the VG page table");
  check(pageTableEntry.prioritySlot == diagnostics.prioritySlotOffset, "page table preserves permanent priority slot");
  check(pageTableEntry.clusterCount == 4u && pageTableEntry.clusterOffset == 0u, "page table exposes the terrain cluster range");
  check(pageTableEntry.decodedVertexOffset == install.pageTableEntry.decodedVertexOffset, "page table exposes decoded vertex range");
  check(pageTableEntry._padding0 == diagnostics.pageTableOffset, "scene stamps authoritative global page identity");

  const auto *status = scene.getRuntimeObjectPageStatus("terrain-runtime-phase14", 0u);
  check(status != nullptr && status->installed && status->rootPage, "runtime page status tracks root residency");
  check(status != nullptr && status->hasTracePayload && status->hasMaterialPayload, "runtime status tracks trace/material parity");
  check(status != nullptr && !status->sceneEncodedPageDataReady, "shared terrain install does not claim scene encoded-page data");
  check(scene.consumePendingPageDecodeRequests().empty(), "shared terrain install does not enqueue a decode request without encoded page data");

  check(scene.getRuntimeObjectDiagnostics("terrain-runtime-phase14", diagnostics), "post-install diagnostics are visible");
  check(diagnostics.installedPageCount == 1u, "diagnostics count installed pages");
  check(diagnostics.traceablePageCount == 1u && diagnostics.materialPageCount == 1u, "diagnostics count trace/material-ready pages");
  check(diagnostics.sceneEncodedPageCount == 0u && diagnostics.queuedDecodeRequestCount == 0u, "diagnostics keep shared terrain payload separate from encoded VG decode");

  std::vector<vg::VirtualGeometryHierarchy> before, after;
  check(scene.debugReadRuntimeObjectHierarchy("terrain-runtime-phase14", before) && !before.empty(),
        "installed hierarchy is available for the traversal-gate regression");
  if (!before.empty())
  {
    // The pre-install root has stale residency/cluster offsets. Changing its
    // traversal band must preserve the live install patches, including when
    // restoring a fallback after a replacement child leaves.
    for (const float parentError : {0.0f, 16.0f})
    {
      check(scene.setRuntimeObjectHierarchyParentError(
                "terrain-runtime-phase14", 0u, parentError, &error),
            "traversal gate changes after page installation: " + error);
      check(scene.debugReadRuntimeObjectHierarchy("terrain-runtime-phase14", after),
            "updated hierarchy can be read");
      if (!after.empty())
      {
        auto expected = before[0];
        expected.max_parent_lod_error = parentError;
        check(std::memcmp(&expected, &after[0], sizeof(expected)) == 0,
              "gate disable/restore preserves all installed node fields");
      }
    }
  }
}

// A cluster mask applied after the install has to survive the uninstall replay.
//
// `rebuildRuntimeObjectHierarchyMasks` reconstructs every leaf's masks in the
// object from the installed pages alone, so a mask written only into the
// hierarchy mirror is discarded by the next uninstall of *any* page. An
// ordinary asset never notices, because all of its masks come from its pages'
// own install rewrites; anything maintaining a relation the builder could not
// put in a single page's rewrites - terrain's cross-chunk yield, between two
// chunks sharing one runtime object - has to ask for it to be persisted.
//
// The harness cannot reach this: `evicted=0` in every configuration that keeps
// the terrain cut healthy, so nothing uninstalls and the replay never runs.
void testExternalClusterMasksSurviveTheUninstallReplay(rendering::RenderGraph &renderGraph)
{
  std::cout << "external cluster masks survive a page uninstall\n";
  vg::VirtualGeometryScene scene(&renderGraph, 64u * 1024u, 1024u * 1024u);
  std::string error;
  vg::VirtualGeometryScene::RuntimeObjectId objectId =
      vg::VirtualGeometryScene::INVALID_RUNTIME_OBJECT_ID;

  // Two leaves, one per page, so uninstalling the second still leaves the
  // first installed and the replay has something to rebuild from.
  std::vector<vg::VirtualGeometryHierarchy> hierarchy = makeRuntimeHierarchy(0u);
  vg::VirtualGeometryHierarchy second = hierarchy[0];
  second.pageIndex = 1u;
  hierarchy.push_back(second);
  for (auto &node : hierarchy)
  {
    node.child_start = 0u;
    node.child_count = 4u;
  }

  check(
      svr::registerSparseVoxelTerrainVGRuntimeObject(
          scene, "terrain-external-masks", 2u, hierarchy, {}, &objectId, &error),
      "runtime terrain object registers: " + error);

  vg::VirtualGeometryScene::RuntimeObjectDiagnostics diagnostics{};
  check(
      scene.getRuntimeObjectDiagnostics("terrain-external-masks", diagnostics),
      "runtime diagnostics are visible");

  for (uint32_t page = 0u; page < 2u; ++page)
  {
    const auto install = makeTerrainInstall(
        page, diagnostics.prioritySlotOffset + page, 10u + page, 20u + page, 4u);
    // A non-root page needs its parent installed, or the replay below has
    // nothing to uninstall and the assertion passes vacuously.
    check(
        svr::installSparseVoxelTerrainVGScenePage(
            scene, "terrain-external-masks", install, page == 0u,
            page == 0u ? UINT32_MAX : 0u, UINT32_MAX, &error),
        "terrain page installs: " + error);
  }

  // The cross-chunk yield: clear the streaming bits on the node owned by page 0.
  std::vector<vg::HierarchyClusterFlagsUpdate> updates(1u);
  updates[0].hierarchyNodeIndex = 0u;
  updates[0].streamingLeafsBitset = 0u;
  updates[0].enabledClustersBitset = 0x0fu;

  check(
      scene.applyRuntimeObjectHierarchyClusterFlags(
          "terrain-external-masks", updates, &error, /*persist=*/true),
      "external cluster flags apply: " + error);

  const auto streamingBitsOfNode = [&](const uint32_t nodeIndex) -> uint8_t
  {
    vg::VirtualGeometryHierarchy node{};
    renderGraph.bufferRead(
        scene.hierarchyBuffer,
        static_cast<uint64_t>(nodeIndex) * sizeof(vg::VirtualGeometryHierarchy),
        sizeof(vg::VirtualGeometryHierarchy),
        [&](const void *data) { std::memcpy(&node, data, sizeof(node)); });
    return static_cast<uint8_t>(
        (node.flags & vg::HIERARCHY_STREAMING_MASK_BITS) >>
        vg::HIERARCHY_STREAMING_MASK_SHIFT);
  };

  check(streamingBitsOfNode(0u) == 0u, "the external mask reaches the hierarchy");

  // Uninstalling any page in the object replays the whole object's masks.
  const auto *beforeUninstall =
      scene.getRuntimeObjectPageStatus("terrain-external-masks", 1u);
  check(
      beforeUninstall != nullptr && beforeUninstall->installed,
      "the second page is installed, so the uninstall really replays");
  check(
      scene.uninstallRuntimeObjectPage("terrain-external-masks", 1u, 0u, &error),
      "the second page uninstalls: " + error);
  const auto *afterUninstall =
      scene.getRuntimeObjectPageStatus("terrain-external-masks", 1u);
  check(
      afterUninstall != nullptr && !afterUninstall->installed,
      "the second page is gone");

  check(
      streamingBitsOfNode(0u) == 0u,
      "the external mask survives the replay that the uninstall runs");
}

void testRuntimeReinstallAndStaleGenerationGuards(rendering::RenderGraph &renderGraph)
{
  std::cout << "runtime page reinstall rewrites newer generations and rejects stale ones\n";
  vg::VirtualGeometryScene scene(&renderGraph, 64u * 1024u, 1024u * 1024u);
  std::string error;
  check(
      svr::registerSparseVoxelTerrainVGRuntimeObject(
          scene,
          "terrain-runtime-reinstall",
          1u,
          makeRuntimeHierarchy(0u),
          {},
          nullptr,
          &error),
      "runtime object registers: " + error);

  vg::VirtualGeometryScene::RuntimeObjectDiagnostics diagnostics{};
  check(scene.getRuntimeObjectDiagnostics("terrain-runtime-reinstall", diagnostics), "diagnostics resolve priority slot");

  auto install = makeTerrainInstall(0u, diagnostics.prioritySlotOffset, 4u, 40u, 2u);
  check(
      svr::installSparseVoxelTerrainVGScenePage(
          scene,
          "terrain-runtime-reinstall",
          install,
          true,
          UINT32_MAX,
          UINT32_MAX,
          &error),
      "initial generation installs: " + error);

  auto newer = makeTerrainInstall(0u, diagnostics.prioritySlotOffset, 5u, 50u, 7u);
  check(
      svr::installSparseVoxelTerrainVGScenePage(
          scene,
          "terrain-runtime-reinstall",
          newer,
          true,
          UINT32_MAX,
          UINT32_MAX,
          &error),
      "newer generation rewrites page table: " + error);

  const auto pageTableEntry = readPageTableEntry(renderGraph, scene, diagnostics.pageTableOffset);
  check(pageTableEntry.clusterCount == 7u, "newer generation updates cluster count");
  const auto *status = scene.getRuntimeObjectPageStatus("terrain-runtime-reinstall", 0u);
  check(status != nullptr && status->pageGeneration == 5u && status->geometryRevision == 50u, "newer generation updates runtime status");

  auto stale = makeTerrainInstall(0u, diagnostics.prioritySlotOffset, 4u, 60u, 9u);
  check(
      !svr::installSparseVoxelTerrainVGScenePage(
          scene,
          "terrain-runtime-reinstall",
          stale,
          true,
          UINT32_MAX,
          UINT32_MAX,
          &error),
      "stale generation is rejected");
}

void testRuntimeParentFallbackProtection(rendering::RenderGraph &renderGraph)
{
  std::cout << "runtime page uninstall protects parent/fallback pages\n";
  vg::VirtualGeometryScene scene(&renderGraph, 64u * 1024u, 1024u * 1024u);
  std::string error;
  check(
      svr::registerSparseVoxelTerrainVGRuntimeObject(
          scene,
          "terrain-runtime-dependencies",
          2u,
          makeRuntimeHierarchy(0u),
          {},
          nullptr,
          &error),
      "runtime object registers: " + error);

  vg::VirtualGeometryScene::RuntimeObjectDiagnostics diagnostics{};
  check(scene.getRuntimeObjectDiagnostics("terrain-runtime-dependencies", diagnostics), "diagnostics resolve dependency slots");

  const auto parent = makeTerrainInstall(0u, diagnostics.prioritySlotOffset + 0u, 10u, 100u, 1u);
  check(
      svr::installSparseVoxelTerrainVGScenePage(
          scene,
          "terrain-runtime-dependencies",
          parent,
          true,
          UINT32_MAX,
          UINT32_MAX,
          &error),
      "parent/root page installs: " + error);

  const auto child = makeTerrainInstall(1u, diagnostics.prioritySlotOffset + 1u, 11u, 110u, 3u);
  check(
      svr::installSparseVoxelTerrainVGScenePage(
          scene,
          "terrain-runtime-dependencies",
          child,
          false,
          0u,
          UINT32_MAX,
          &error),
      "child page installs only after parent is resident: " + error);

  check(
      !scene.uninstallRuntimeObjectPage("terrain-runtime-dependencies", 0u, 10u, &error),
      "parent uninstall is rejected while protecting an installed child");
  check(
      scene.uninstallRuntimeObjectPage("terrain-runtime-dependencies", 1u, 11u, &error),
      "child uninstall succeeds first: " + error);
  check(
      scene.uninstallRuntimeObjectPage("terrain-runtime-dependencies", 0u, 10u, &error),
      "parent uninstall succeeds after child retirement: " + error);

  const auto pageTableEntry = readPageTableEntry(renderGraph, scene, diagnostics.pageTableOffset);
  check(pageTableEntry.isInstalled == 0u && pageTableEntry.prioritySlot == diagnostics.prioritySlotOffset, "uninstall clears residency but preserves priority slot");
}

void testGenericEncodedRuntimeInstallQueuesDecode(rendering::RenderGraph &renderGraph)
{
  std::cout << "generic encoded runtime install queues page decode\n";
  vg::VirtualGeometryScene scene(&renderGraph, 64u * 1024u, 1024u * 1024u);
  std::string error;

  vg::VirtualGeometryScene::RuntimeObjectCreateInfo createInfo{};
  createInfo.name = "encoded-runtime-phase14";
  createInfo.pageCount = 1u;
  createInfo.hierarchy = makeRuntimeHierarchy(0u);
  check(scene.registerRuntimeObject(createInfo, nullptr, &error), "generic runtime object registers: " + error);

  vg::VirtualGeometryScene::RuntimeObjectDiagnostics diagnostics{};
  check(scene.getRuntimeObjectDiagnostics("encoded-runtime-phase14", diagnostics), "generic diagnostics resolve page identity");

  vg::VirtualGeometryScene::RuntimePageInstallInfo install{};
  install.localPageIndex = 0u;
  install.pageGeneration = 1u;
  install.geometryRevision = 2u;
  install.materialRevision = 3u;
  install.pageAllocation.pageId = 0u;
  install.pageAllocation.bufferOffset = 0u;
  install.pageAllocation.size = 64u;
  install.pageAllocation.clusterOffset = 16u;
  install.pageAllocation.clusterCount = 1u;
  install.pageAllocation.unitScaleBits = floatBits(1.0f);
  install.pageAllocation.prioritySlot = diagnostics.prioritySlotOffset;
  install.pageAllocation.isInstalled = true;
  install.pageTableEntry.bufferOffset = 0u;
  install.pageTableEntry.size = 64u;
  install.pageTableEntry.clusterOffset = 16u;
  install.pageTableEntry.clusterCount = 1u;
  install.pageTableEntry.isInstalled = 1u;
  install.pageTableEntry.prioritySlot = diagnostics.prioritySlotOffset;
  install.pageTableEntry.unitScaleBits = floatBits(1.0f);
  install.rootPage = true;
  install.hasTracePayload = true;
  install.encodedPageWords = {1u, 2u, 3u, 4u};
  install.enqueuePageDecode = true;

  check(scene.installRuntimeObjectPage("encoded-runtime-phase14", install, &error), "encoded runtime page installs: " + error);
  const auto decodeRequests = scene.consumePendingPageDecodeRequests();
  check(decodeRequests.size() == 1u && decodeRequests[0] == diagnostics.pageTableOffset, "encoded install queues exactly one global page decode request");

  uint32_t encodedWords[4] = {};
  renderGraph.bufferRead(
      scene.pagesBuffer,
      0u,
      sizeof(encodedWords),
      [&](const void *data)
      {
        std::memcpy(encodedWords, data, sizeof(encodedWords));
      });
  check(encodedWords[0] == 1u && encodedWords[3] == 4u, "encoded page words upload into the scene pages buffer");

  check(scene.getRuntimeObjectDiagnostics("encoded-runtime-phase14", diagnostics), "encoded diagnostics refresh");
  check(diagnostics.sceneEncodedPageCount == 1u && diagnostics.queuedDecodeRequestCount == 1u, "diagnostics count encoded runtime decode work");
}

void testGeneratedTerrainVGPayloadManagerPublishesDecodedScenePage(rendering::RenderGraph &renderGraph)
{
  std::cout << "terrain generated payload manager publishes decoded VG scene page\n";
  vg::VirtualGeometryScene scene(&renderGraph, 64u * 1024u, 1024u * 1024u);
  std::string error;
  check(
      svr::registerSparseVoxelTerrainVGRuntimeObject(
          scene,
          "terrain-generated-vg",
          2u,
          makeRuntimeHierarchy(0u),
          {},
          nullptr,
          &error),
      "generated terrain VG runtime object registers: " + error);

  vg::VirtualGeometryScene::RuntimeObjectDiagnostics diagnostics{};
  check(scene.getRuntimeObjectDiagnostics("terrain-generated-vg", diagnostics), "generated terrain diagnostics resolve");
  vg::VirtualGeometryGeneratedPayloadManager manager(scene);

  auto rootBuild = makeReadyGeneratedBuild(0u, true);
  vg::VirtualGeometryGeneratedPayloadManager::GpuWriteDescriptor descriptor{};
  check(
      svr::reserveSparseVoxelTerrainVGGeneratedPayload(
          manager,
          rootBuild,
          "terrain-generated-vg",
          0u,
          UINT32_MAX,
          UINT32_MAX,
          floatBits(1.0f),
          64u,
          descriptor,
          &error),
      "root generated VG page reserves scene decoded ranges: " + error);
  check(descriptor.encodedPageByteSize == 0u, "decoded generated terrain page does not reserve encoded bytes");
  check(descriptor.clusterCount == rootBuild.vgMeshletCount, "descriptor carries generated meshlet budget");
  check(descriptor.decodedVertexCount == rootBuild.vgVertexCount, "descriptor carries generated vertex budget");
  check(descriptor.decodedIndexCount == rootBuild.vgTriangleCount * 3u, "descriptor carries generated index budget");

  // The runtime producer writes decodedClusterCull/Geometry/Vertex/Index
  // buffers at the descriptor offsets before this publish. This test validates
  // the scene-visible metadata transition without relying on CPU page decode.
  check(
      svr::publishSparseVoxelTerrainVGGeneratedDecodedPayload(
          manager,
          descriptor.ticketId,
          &error),
      "root generated decoded page publishes into the VG scene: " + error);
  const auto rootEntry = readPageTableEntry(renderGraph, scene, diagnostics.pageTableOffset);
  check(rootEntry.isInstalled == 1u, "generated root page becomes resident");
  check(rootEntry.clusterOffset == descriptor.clusterOffset, "page table aliases generated cluster range");
  check(rootEntry.decodedVertexOffset == descriptor.decodedVertexOffset, "page table aliases generated vertex range");
  check(rootEntry.decodedIndexOffset == descriptor.decodedIndexOffset, "page table aliases generated index range");
  check(scene.consumePendingPageDecodeRequests().empty(), "decoded generated terrain page does not enqueue page decode");
  const auto *rootStatus = scene.getRuntimeObjectPageStatus("terrain-generated-vg", 0u);
  check(rootStatus != nullptr && rootStatus->installed && rootStatus->rootPage, "root runtime status is installed");
  check(rootStatus != nullptr && !rootStatus->sceneEncodedPageDataReady, "decoded generated page does not claim encoded scene bytes");
  check(rootStatus != nullptr && rootStatus->ownsSceneBufferAllocation, "generated page owns VG scene allocation ranges");

  auto childBuild = makeReadyGeneratedBuild(1u, false);
  descriptor = {};
  check(
      svr::reserveSparseVoxelTerrainVGGeneratedPayload(
          manager,
          childBuild,
          "terrain-generated-vg",
          1u,
          0u,
          UINT32_MAX,
          floatBits(1.0f),
          128u,
          descriptor,
          &error),
      "child generated VG page reserves after root: " + error);
  check(
      svr::publishSparseVoxelTerrainVGGeneratedDecodedPayload(
          manager,
          descriptor.ticketId,
          &error),
      "child generated decoded page publishes with parent protection: " + error);
  const auto *childStatus = scene.getRuntimeObjectPageStatus("terrain-generated-vg", 1u);
  check(childStatus != nullptr && childStatus->installed, "child generated page becomes resident");
  check(childStatus != nullptr && childStatus->parentLocalPageIndex == 0u, "child records parent dependency");

  check(scene.getRuntimeObjectDiagnostics("terrain-generated-vg", diagnostics), "generated diagnostics refresh");
  check(diagnostics.installedPageCount == 2u, "diagnostics count generated pages");
  check(diagnostics.sceneEncodedPageCount == 0u, "generated decoded pages remain separate from encoded-page decode");
}

void testTerrainEncodedInstallUsesNormalVirtualGeometryDecodePath(rendering::RenderGraph &renderGraph)
{
  std::cout << "terrain encoded page installs through normal VirtualGeometryScene decode path\n";
  vg::VirtualGeometryScene scene(&renderGraph, 64u * 1024u, 1024u * 1024u);
  std::string error;
  check(
      svr::registerSparseVoxelTerrainVGRuntimeObject(
          scene,
          "terrain-runtime-drawable-vg",
          1u,
          makeRuntimeHierarchy(0u),
          {},
          nullptr,
          &error),
      "runtime terrain object registers: " + error);

  vg::VirtualGeometryScene::RuntimeObjectDiagnostics diagnostics{};
  check(scene.getRuntimeObjectDiagnostics("terrain-runtime-drawable-vg", diagnostics), "terrain diagnostics resolve scene slots");

  auto install = makeTerrainInstall(0u, diagnostics.prioritySlotOffset, 21u, 42u, 1u);
  check(
      !svr::sparseVoxelTerrainVGSceneInstallHasDrawablePayload(install),
      "metadata-only terrain install is not drawable");
  check(
      svr::attachSparseVoxelTerrainVGSceneEncodedPage(
          install,
          makeDrawableRuntimePage(),
          &error),
      "terrain page attaches encoded VG payload: " + error);
  check(
      svr::sparseVoxelTerrainVGSceneInstallHasDrawablePayload(install),
      "encoded terrain install is scene-visible");

  check(
      svr::installSparseVoxelTerrainVGScenePage(
          scene,
          "terrain-runtime-drawable-vg",
          install,
          true,
          UINT32_MAX,
          UINT32_MAX,
          &error),
      "terrain encoded page installs into VirtualGeometryScene: " + error);

  const auto decodeRequests = scene.consumePendingPageDecodeRequests();
  check(
      decodeRequests.size() == 1u &&
          decodeRequests[0] == diagnostics.pageTableOffset,
      "terrain encoded install queues the normal VG page decode pass");

  const auto *status =
      scene.getRuntimeObjectPageStatus("terrain-runtime-drawable-vg", 0u);
  check(status != nullptr && status->installed, "drawable terrain page is resident");
  check(status != nullptr && status->sceneEncodedPageDataReady, "drawable terrain page has scene encoded data");
  check(status != nullptr && status->ownsSceneBufferAllocation, "drawable terrain page owns scene VG buffer ranges");
  check(status != nullptr && status->pageAllocation.clusterCount == 1u, "scene allocation exposes one VG meshlet");
  check(status != nullptr && status->pageAllocation.decodedVertexCount == 3u, "scene allocation reserves decoded vertices");
  check(status != nullptr && status->pageAllocation.decodedIndexCount == 3u, "scene allocation reserves decoded indices");

  check(scene.getRuntimeObjectDiagnostics("terrain-runtime-drawable-vg", diagnostics), "drawable terrain diagnostics refresh");
  check(diagnostics.sceneEncodedPageCount == 1u, "diagnostics count terrain page as scene-visible VG payload");
}

void testTerrainEncodedAssetInstallsAsRuntimeVirtualGeometryObject(rendering::RenderGraph &renderGraph)
{
  std::cout << "terrain encoded asset installs as normal runtime virtual geometry\n";
  vg::VirtualGeometryScene scene(&renderGraph, 64u * 1024u, 1024u * 1024u);
  std::string error;

  vg::VirtualGeometryEncodedData asset{};
  asset.hierarchy = makeRuntimeHierarchy(0u);
  asset.pages.push_back(makeDrawableRuntimePage());
  asset.rootPageIndex = 0u;
  asset.quantizationConfig.unit_scale = 1.0f;
  asset.quantizationConfig.padMeshlets = true;

  svr::SparseVoxelTerrainVGEncodedAssetRuntimeInstallSettings settings{};
  settings.basePageGeneration = 100u;
  settings.geometryRevision = 200u;
  settings.materialRevision = 300u;
  settings.hasTracePayload = true;
  settings.hasMaterialPayload = true;
  settings.enqueueScenePageDecode = true;

  svr::SparseVoxelTerrainVGEncodedAssetRuntimeInstallStats stats{};
  check(
      svr::installSparseVoxelTerrainVGEncodedAssetRuntimeObject(
          scene,
          "terrain-runtime-encoded-asset",
          asset,
          settings,
          &stats,
          &error),
      "encoded terrain asset registers and installs into VG scene: " + error);
  check(stats.objectId != vg::VirtualGeometryScene::INVALID_RUNTIME_OBJECT_ID, "encoded asset reports a runtime object id");
  check(stats.installedPageCount == 1u, "encoded asset installs every page");
  check(stats.queuedDecodePageCount == 1u, "encoded asset queues every page for VG decode");
  check(stats.encodedMeshletCount == 1u, "encoded asset stats count meshlets");
  check(stats.encodedPageBytes > 0u, "encoded asset stats count encoded bytes");

  const auto decodeRequests = scene.consumePendingPageDecodeRequests();
  vg::VirtualGeometryScene::RuntimeObjectDiagnostics diagnostics{};
  check(scene.getRuntimeObjectDiagnostics("terrain-runtime-encoded-asset", diagnostics), "encoded asset diagnostics resolve");
  check(
      decodeRequests.size() == 1u &&
          decodeRequests[0] == diagnostics.pageTableOffset,
      "encoded asset feeds the normal VG page decode queue");
  check(diagnostics.installedPageCount == 1u, "encoded asset page is resident");
  check(diagnostics.sceneEncodedPageCount == 1u, "encoded asset page owns scene-visible encoded data");
  check(diagnostics.traceablePageCount == 1u && diagnostics.materialPageCount == 1u, "encoded asset preserves trace/material readiness");

  const auto *status =
      scene.getRuntimeObjectPageStatus("terrain-runtime-encoded-asset", 0u);
  check(status != nullptr && status->rootPage, "encoded asset root page is marked");
  check(status != nullptr && status->ownsSceneBufferAllocation, "encoded asset owns VG scene allocations");

  // A forest's initial cut can span independent pages. The last root is not
  // necessarily a dependent of earlier roots, so pin the complete prefix.
  auto forest = asset;
  forest.pages.push_back(makeDrawableRuntimePage());
  auto secondRoot = makeRuntimeHierarchy(1u);
  forest.hierarchy.insert(forest.hierarchy.end(), secondRoot.begin(), secondRoot.end());
  forest.rootPageIndex = 1u;
  check(svr::installSparseVoxelTerrainVGEncodedAssetRuntimeObject(
      scene, "terrain-independent-roots", forest, settings, nullptr, &error),
      "an independent two-page root cut installs: " + error);
  scene.evictLeastRecentlyUsedRuntimePagesForHeadroom(1.0, 16u);
  for (uint32_t page = 0u; page <= forest.rootPageIndex; ++page)
  {
    const auto *root = scene.getRuntimeObjectPageStatus("terrain-independent-roots", page);
    check(root != nullptr && root->installed && root->rootPage,
        "LRU pressure preserves every page in the initial root cut");
  }
}

// Phase 8g of docs/terrain-vg-unified-hierarchy-plan.md: a runtime page is a
// location the scene reads itself, so an eviction is reversible by the scene
// alone and a page never installed can be served from its location on request.
struct MemoryPageSource final : vg::VirtualGeometryPageSource
{
  std::vector<std::vector<uint32_t>> pages;
  mutable uint32_t reads = 0u;
  bool refuse = false;

  vg::VirtualGeometryPageLocation location(const uint32_t index) const
  {
    vg::VirtualGeometryPageLocation location{};
    location.source = this;
    location.key = 7u;
    location.index = index;
    location.offset = static_cast<uint64_t>(index) * 4096u;
    location.storedBytes = static_cast<uint32_t>(pages[index].size() * sizeof(uint32_t));
    location.uncompressedBytes = location.storedBytes;
    return location;
  }

  bool readEncodedPage(
      const vg::VirtualGeometryPageLocation &location,
      std::vector<uint32_t> &words,
      std::string *error) const override
  {
    if (refuse || location.source != this || location.key != 7u || location.index >= pages.size())
    {
      if (error != nullptr)
        *error = "memory page source refused the location";
      return false;
    }
    words = pages[location.index];
    ++reads;
    return true;
  }
};

void testRuntimePagesAreLocationsTheSceneReadsItself(rendering::RenderGraph &renderGraph)
{
  std::cout << "runtime pages are locations the scene reads itself (phase 8g)\n";
  vg::VirtualGeometryScene scene(&renderGraph, 64u * 1024u, 1024u * 1024u);
  std::string error;
  const std::string name = "terrain-runtime-page-source";
  std::vector<vg::VirtualGeometryHierarchy> hierarchy;
  hierarchy.push_back(makeRuntimeHierarchy(0u)[0]);
  hierarchy.push_back(makeRuntimeHierarchy(1u)[0]);
  vg::QuantizationConfig quantization{};
  quantization.unit_scale = 1.0f;
  quantization.padMeshlets = true;
  check(
      svr::registerSparseVoxelTerrainVGRuntimeObject(scene, name, 2u, hierarchy, quantization, nullptr, &error),
      "runtime object registers: " + error);

  MemoryPageSource source;
  source.pages.push_back(vg::PageBuffer::encode(makeDrawableRuntimePage()).data);
  source.pages.push_back(vg::PageBuffer::encode(makeDrawableRuntimePage()).data);
  const auto makeInstall = [&](const uint32_t page)
  {
    vg::VirtualGeometryScene::RuntimeEncodedPageInstallInfo install{};
    install.localPageIndex = page;
    install.pageGeneration = 10u + page;
    install.geometryRevision = 20u;
    install.materialRevision = 30u;
    install.unitScaleBits = floatBits(1.0f);
    install.rootPage = page == 0u;
    install.hasTracePayload = true;
    install.enqueuePageDecode = true;
    install.location = source.location(page);
    if (page != 0u)
      install.dependencies = {0u};
    return install;
  };

  check(scene.installRuntimeObjectEncodedPage(name, makeInstall(0u), &error), "the root page installs from its location alone: " + error);
  check(scene.installRuntimeObjectEncodedPage(name, makeInstall(1u), &error), "and the page that depends on it: " + error);
  check(source.reads == 2u, "the scene read both pages through the source");
  vg::VirtualGeometryScene::RuntimeObjectDiagnostics diagnostics{};
  check(scene.getRuntimeObjectDiagnostics(name, diagnostics) && diagnostics.installedPageCount == 2u, "both pages are resident");
  const auto *status = scene.getRuntimeObjectPageStatus(name, 1u);
  check(status != nullptr && status->reinstall.valid() && status->reinstall.location.index == 1u, "the install recorded the page's location");
  check(scene.countReinstallableRuntimePages(name) == 0u, "nothing to bring back while everything is installed");

  // Eviction is reversible.
  check(!scene.uninstallRuntimeObjectPage(name, 0u, 0u, &error), "the root is protected by the page depending on it");
  check(scene.uninstallRuntimeObjectPage(name, 1u, 0u, &error), "the dependent page uninstalls: " + error);
  check(scene.countReinstallableRuntimePages(name) == 1u, "and is reinstallable");
  check(scene.reinstallRuntimeObjectPageFromSource(name, 1u, &error), "it comes back from its location: " + error);
  check(source.reads == 3u, "with one read");
  status = scene.getRuntimeObjectPageStatus(name, 1u);
  check(
      status != nullptr && status->installed && status->pageGeneration == 11u &&
          status->geometryRevision == 20u && status->materialRevision == 30u,
      "with the generation and revisions it was installed with");
  check(!scene.uninstallRuntimeObjectPage(name, 0u, 0u, &error), "and its dependency on the root is restored");
  check(!scene.reinstallRuntimeObjectPageFromSource(name, 1u, &error), "reinstalling an installed page is refused");

  // Evicted dependencies come back first.
  check(
      scene.uninstallRuntimeObjectPage(name, 1u, 0u, &error) &&
          scene.uninstallRuntimeObjectPage(name, 0u, 0u, &error),
      "both pages uninstall: " + error);
  check(scene.countReinstallableRuntimePages(name) == 2u, "both are reinstallable");
  check(scene.reinstallRuntimeObjectPageFromSource(name, 1u, &error), "reinstalling the dependent page: " + error);
  check(scene.getRuntimeObjectDiagnostics(name, diagnostics) && diagnostics.installedPageCount == 2u, "brings its evicted dependency back with it");
  check(source.reads == 5u, "two reads");
  auto stats = scene.getRuntimePageSourceStats();
  check(stats.reads == 5u && stats.reinstalls == 3u && stats.readFailures == 0u, "the scene counts reads and reinstalls");

  // The culler's requests.
  check(scene.uninstallRuntimeObjectPage(name, 1u, 0u, &error), "uninstall the dependent page again: " + error);
  std::vector<vg::VirtualGeometryScene::RuntimeObjectPageRequest> requests(1u);
  requests[0].localPageIndex = 1u;
  requests[0].priority = 5u;
  uint32_t failures = 0u;
  check(
      scene.reinstallRequestedRuntimePagesFromSource(name, requests, 8u, &failures) == 1u && failures == 0u,
      "a culler request for the page is served by the scene");
  check(
      scene.reinstallRequestedRuntimePagesFromSource(name, requests, 8u, &failures) == 0u && failures == 0u,
      "a request for an installed page is skipped, not failed");

  // A source that refuses.
  check(scene.uninstallRuntimeObjectPage(name, 1u, 0u, &error), "uninstall once more: " + error);
  source.refuse = true;
  check(!scene.reinstallRuntimeObjectPageFromSource(name, 1u, &error), "a refused read fails the reinstall");
  check(
      scene.reinstallRequestedRuntimePagesFromSource(name, requests, 8u, &failures) == 0u && failures == 1u,
      "and counts against the request");
  source.refuse = false;
  stats = scene.getRuntimePageSourceStats();
  check(stats.readFailures == 2u && stats.reinstallFailures == 2u, "the scene counts the failures");
  check(scene.getRuntimeObjectDiagnostics(name, diagnostics) && diagnostics.installedPageCount == 1u, "and the root stayed resident throughout");

  // Priming: a page never installed becomes the scene's to install on request.
  scene.forgetRuntimeObjectPageSource(name, 1u);
  check(!scene.reinstallRuntimeObjectPageFromSource(name, 1u, &error), "a forgotten page has nothing to come back from");
  check(scene.countReinstallableRuntimePages(name) == 0u, "and is not counted");
  check(scene.primeRuntimeObjectPageSource(name, makeInstall(1u), &error), "priming records the location without installing: " + error);
  status = scene.getRuntimeObjectPageStatus(name, 1u);
  check(status != nullptr && !status->installed && status->reinstall.valid(), "the page stays uninstalled and reinstallable");
  check(!scene.primeRuntimeObjectPageSource(name, makeInstall(0u), &error), "priming an installed page is refused");
  check(scene.reinstallRequestedRuntimePagesFromSource(name, requests, 8u, &failures) == 1u, "a request for the primed page is served");
  check(scene.getRuntimeObjectDiagnostics(name, diagnostics) && diagnostics.installedPageCount == 2u, "both pages resident again");

  // Words and a location together: the words are used, the location kept.
  {
    auto install = makeInstall(1u);
    install.encodedPageWords = source.pages[1];
    const uint32_t readsBefore = source.reads;
    check(scene.installRuntimeObjectEncodedPage(name, install, &error), "an install with words and a location: " + error);
    check(source.reads == readsBefore, "does not read");
    status = scene.getRuntimeObjectPageStatus(name, 1u);
    check(status != nullptr && status->reinstall.valid(), "and keeps the location");
  }

  // Forgetting a source drops every location that reads from it.
  scene.forgetRuntimePageSource(&source);
  check(scene.uninstallRuntimeObjectPage(name, 1u, 0u, &error), "uninstall after forgetting the source: " + error);
  check(!scene.reinstallRuntimeObjectPageFromSource(name, 1u, &error), "a forgotten source serves nothing");
  check(scene.countReinstallableRuntimePages(name) == 0u, "no page reads from it any more");

  // Words without a location record nothing to come back from.
  {
    auto install = makeInstall(1u);
    install.location = {};
    install.encodedPageWords = source.pages[1];
    check(scene.installRuntimeObjectEncodedPage(name, install, &error), "an install from words alone: " + error);
    status = scene.getRuntimeObjectPageStatus(name, 1u);
    check(status != nullptr && status->installed && !status->reinstall.valid(), "records no location");
  }
  vg::VirtualGeometryScene::RuntimeEncodedPageInstallInfo empty{};
  empty.localPageIndex = 1u;
  empty.pageGeneration = 40u;
  empty.geometryRevision = 20u;
  check(!scene.installRuntimeObjectEncodedPage(name, empty, &error), "an install with neither words nor a location is refused");
}

void testPrimedPageDependenciesProtectEveryResidentChild(rendering::RenderGraph &renderGraph)
{
  std::cout << "primed and reinstalled pages retain dependency protection\n";
  vg::VirtualGeometryScene scene(&renderGraph, 64u * 1024u, 1024u * 1024u);
  const std::string name = "terrain-primed-dependencies";
  std::string error;
  std::vector<vg::VirtualGeometryHierarchy> hierarchy;
  for (uint32_t page = 0u; page < 3u; ++page)
    hierarchy.push_back(makeRuntimeHierarchy(page)[0]);
  vg::QuantizationConfig quantization{};
  quantization.unit_scale = 1.0f;
  check(svr::registerSparseVoxelTerrainVGRuntimeObject(
            scene, name, 3u, hierarchy, quantization, nullptr, &error),
        "dependency fixture registers: " + error);
  MemoryPageSource source;
  source.pages.resize(3u, vg::PageBuffer::encode(makeDrawableRuntimePage()).data);
  const auto install = [&](const uint32_t page)
  {
    vg::VirtualGeometryScene::RuntimeEncodedPageInstallInfo result{};
    result.localPageIndex = page;
    result.pageGeneration = 1u;
    result.geometryRevision = 1u;
    result.unitScaleBits = floatBits(1.0f);
    result.rootPage = page == 0u;
    result.location = source.location(page);
    for (uint32_t parent = 0u; parent < page; ++parent)
      result.dependencies.push_back(parent);
    return result;
  };
  check(scene.installRuntimeObjectEncodedPage(name, install(0u), &error) &&
            scene.installRuntimeObjectEncodedPage(name, install(1u), &error) &&
            scene.primeRuntimeObjectPageSource(name, install(2u), &error) &&
            scene.reinstallRuntimeObjectPageFromSource(name, 2u, &error),
        "two dependents install, one through a primed source: " + error);
  const auto rootReferences = [&]()
  {
    const auto *root = scene.getRuntimeObjectPageStatus(name, 0u);
    return root != nullptr ? root->dependedOnBy : UINT32_MAX;
  };
  check(rootReferences() == 2u, "priming cannot borrow another resident page's dependency reference");
  check(scene.installRuntimeObjectEncodedPage(name, install(2u), &error) && rootReferences() == 2u,
        "replacing an installed page keeps exactly one reference per dependent");
  check(scene.uninstallRuntimeObjectPage(name, 2u, 0u, &error) && rootReferences() == 1u,
        "evicting one sibling leaves the other dependency protected");
  check(scene.reinstallRuntimeObjectPageFromSource(name, 2u, &error) && rootReferences() == 2u,
        "reinstallation claims its reference without releasing an unowned one");
  check(scene.uninstallRuntimeObjectPage(name, 2u, 0u, &error), "the last page can be evicted again");
  check(!scene.uninstallRuntimeObjectPage(name, 0u, 0u, &error),
        "the root cannot be evicted while its first dependent still draws");
}

void runTests(rendering::backend::vulkan::VulkanRHI &rhi)
{
  rendering::RenderGraph renderGraph(&rhi);
  testTerrainRuntimePageInstallsIntoScene(renderGraph);
  testExternalClusterMasksSurviveTheUninstallReplay(renderGraph);
  testRuntimeReinstallAndStaleGenerationGuards(renderGraph);
  testRuntimeParentFallbackProtection(renderGraph);
  testGenericEncodedRuntimeInstallQueuesDecode(renderGraph);
  testGeneratedTerrainVGPayloadManagerPublishesDecodedScenePage(renderGraph);
  testTerrainEncodedInstallUsesNormalVirtualGeometryDecodePath(renderGraph);
  testTerrainEncodedAssetInstallsAsRuntimeVirtualGeometryObject(renderGraph);
  testRuntimePagesAreLocationsTheSceneReadsItself(renderGraph);
  testPrimedPageDependenciesProtectEveryResidentChild(renderGraph);
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

    os::Logger::warningf("[Test] Terrain VG Phase 14 tests skipped: %s", gpuError.what());
    os::Logger::shutdown();
    std::cout << "SparseVoxelTerrainVirtualGeometryPhase14Tests passed (GPU backend skipped)\n";
    return 0;
  }

  os::Logger::shutdown();
  if (failures != 0)
  {
    std::cerr << "SparseVoxelTerrainVirtualGeometryPhase14Tests failed with " << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "SparseVoxelTerrainVirtualGeometryPhase14Tests passed\n";
  return 0;
}
