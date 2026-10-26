#include "voxel/svraster/SparseVoxelTerrainVirtualGeometryBenchmark.hpp"
#include "voxel/svraster/rendering/TerrainGPUV2GeneratedVGEmitPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryComputeRasterPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryMetalComputeRasterPass.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace svr = voxel::svraster;
namespace svrender = voxel::svraster::rendering;
namespace vgrender = virtualgeometry::gpgpu;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

std::filesystem::path sourceRoot()
{
#if defined(ENGINE_SOURCE_DIR)
  return std::filesystem::path(ENGINE_SOURCE_DIR);
#else
  return std::filesystem::current_path();
#endif
}

std::string readTextFile(const std::filesystem::path &path)
{
  std::ifstream input(path);
  return std::string(
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>());
}

svr::SparseVoxelTerrainComputeRasterCapabilities appleMoltenVKWithoutVulkanAtomic64()
{
  svr::SparseVoxelTerrainComputeRasterCapabilities caps{};
  caps.platformApple = true;
  caps.moltenVKActive = true;
  caps.nativeMetalDeviceAvailable = true;
  caps.nativeMetalQueueCompatibleWithMoltenVK = true;
  caps.nativeMetalBufferInteropAvailable = true;
  caps.nativeMetalSupportsAtomic64MinMax = true;
  caps.vulkanShaderBufferInt64Atomics = false;
  caps.vulkanShaderBufferInt32Atomics = true;
  caps.maximumCandidatesPerTile = 413u;
  return caps;
}

svr::SparseVoxelTerrainComputeRasterSettings autoSettings()
{
  svr::SparseVoxelTerrainComputeRasterSettings settings{};
  settings.request = svr::SparseVoxelTerrainComputeRasterRequest::Auto;
  settings.gbufferRoute = svr::SparseVoxelTerrainGBufferRoute::VirtualGeometry;
  return settings;
}

void testDisabledPathAllocatesNothing()
{
  std::cout << "Phase 17 disabled path allocates and registers nothing\n";
  const auto caps = appleMoltenVKWithoutVulkanAtomic64();
  svr::SparseVoxelTerrainComputeRasterSettings settings{};
  settings.request = svr::SparseVoxelTerrainComputeRasterRequest::Disabled;
  svr::SparseVoxelTerrainComputeRasterRouteDecision decision{};
  std::string error;

  check(
      svr::selectSparseVoxelTerrainComputeRasterRoute(
          caps,
          settings,
          decision,
          &error),
      "disabled route is valid: " + error);
  check(!decision.enabled, "disabled route is not enabled");
  check(!decision.allocateComputeRasterBuffers, "disabled route allocates no compute-raster buffers");
  check(!decision.registerComputeRasterPasses, "disabled route registers no compute-raster passes");
}

void testNativeMetalBypassesMoltenVKAtomic64Feature()
{
  std::cout << "Phase 17 native Metal path can bypass MoltenVK Vulkan atomic64\n";
  const auto caps = appleMoltenVKWithoutVulkanAtomic64();
  const auto settings = autoSettings();
  svr::SparseVoxelTerrainComputeRasterRouteDecision decision{};
  std::string error;

  check(
      svr::selectSparseVoxelTerrainComputeRasterRoute(
          caps,
          settings,
          decision,
          &error),
      "native Metal route is valid: " + error);
  check(decision.enabled, "native Metal compute raster is enabled");
  check(
      decision.backend ==
          svr::SparseVoxelTerrainComputeRasterBackend::NativeMetalAtomic64,
      "native Metal atomic64 backend is selected");
  check(decision.bypassesMoltenVKAtomic64Feature, "route records the MoltenVK atomic64 bypass");
  check(decision.usesPacked64DepthPayloadAtomic, "route uses packed 64-bit depth/payload atomics");
  check(!decision.usesPacked32TileDepthSlotAtomic, "route does not use the 32-bit fallback packing");
}

void testNativeMetalPassPlanRegistersAtomic64Kernels()
{
  std::cout << "Phase 17 native Metal pass plan registers atomic64 kernels\n";
  const auto caps = appleMoltenVKWithoutVulkanAtomic64();
  const auto settings = autoSettings();
  svr::SparseVoxelTerrainComputeRasterRouteDecision decision{};
  std::string error;
  check(
      svr::selectSparseVoxelTerrainComputeRasterRoute(
          caps,
          settings,
          decision,
          &error),
      "native Metal route is valid before building pass plan: " + error);

  vgrender::VirtualGeometryMetalComputeRasterPassPlan plan{};
  check(
      vgrender::buildVirtualGeometryMetalComputeRasterPassPlan(
          decision,
          caps,
          settings,
          plan,
          &error),
      "native Metal pass plan builds: " + error);
  check(plan.enabled, "native Metal pass plan is enabled");
  check(plan.registerNativeMetalEncoder, "native Metal pass plan registers a native encoder");
  check(plan.allocateVisibilityKeyBuffer, "native Metal pass plan allocates visibility keys");
  check(plan.allocateTileRangeBuffers, "native Metal pass plan allocates tile ranges");
  check(plan.allocateResolvedPayloadBuffer, "native Metal pass plan allocates resolved payloads");
  check(plan.usesPacked64DepthPayloadAtomic, "native Metal pass plan uses packed 64-bit visibility keys");
  check(plan.bypassesMoltenVKAtomic64Feature, "native Metal pass plan records the MoltenVK bypass");
  check(plan.tileSize == 16u, "native Metal pass plan uses 16x16 tiles");
  check(
      std::string(plan.entryPoints[4]) ==
          "terrainComputeRasterAtomic64RasterizeTiles",
      "native Metal pass plan exposes the raster kernel entry point");
  check(
      std::string(plan.entryPoints[5]) ==
          "terrainComputeRasterAtomic64CountDecodedVGClusters",
      "native Metal pass plan exposes the decoded-VG cluster count kernel");
  check(
      std::string(plan.entryPoints[7]) ==
          "terrainComputeRasterAtomic64RasterizeDecodedVG",
      "native Metal pass plan exposes the decoded-VG raster kernel");
  check(
      std::string(plan.entryPoints[1]) ==
          "terrainComputeRasterAtomic64ClearTiles",
      "native Metal pass plan exposes the tile-clear kernel");
}

void testNativeMetalPassPlanRequiresBufferInterop()
{
  std::cout << "Phase 17 native Metal pass plan fails without buffer interop\n";
  auto caps = appleMoltenVKWithoutVulkanAtomic64();
  const auto settings = autoSettings();
  svr::SparseVoxelTerrainComputeRasterRouteDecision decision{};
  std::string error;
  check(
      svr::selectSparseVoxelTerrainComputeRasterRoute(
          caps,
          settings,
          decision,
          &error),
      "native Metal route is valid before disabling buffer interop: " + error);

  caps.nativeMetalBufferInteropAvailable = false;
  vgrender::VirtualGeometryMetalComputeRasterPassPlan plan{};
  check(
      !vgrender::buildVirtualGeometryMetalComputeRasterPassPlan(
          decision,
          caps,
          settings,
          plan,
          &error),
      "native Metal pass plan rejects missing shared buffers");
  check(
      error.find("shared/exported Metal buffers") != std::string::npos,
      "native Metal pass plan explains the missing buffer interop");
}

void testNativeMetalBufferInteropIsExplicitOptIn()
{
  std::cout << "Phase 17 native Metal buffer interop is explicit opt-in\n";
  ::rendering::BufferInfo buffer{};
  buffer.name = "virtualgeometry.compute-raster.visibilityKeys";
  buffer.size = 1024u;
  buffer.usage = ::rendering::BufferUsage_Storage;

  check(!buffer.metalExportable, "ordinary RHI buffers are not Metal-exportable by default");
  buffer.metalExportable = true;
  check(buffer.metalExportable, "VG compute-raster buffers can request MTLBuffer export");
}

#if defined(__APPLE__)
void testNativeMetalPreparedTileRuntimeContract()
{
  std::cout << "Phase 17 native Metal prepared-tile runtime contract is compact\n";
  vgrender::VirtualGeometryMetalComputeRasterPass::Uniforms uniforms{};
  uniforms.viewportWidth = 1220u;
  uniforms.viewportHeight = 720u;
  uniforms.tileGridWidth = 77u;
  uniforms.tileGridHeight = 45u;
  uniforms.triangleCount = 1u;
  uniforms.tileReferenceCapacity = 4096u;

  vgrender::VirtualGeometryMetalComputeRasterPass::Frame frame{};
  check(
      sizeof(vgrender::VirtualGeometryMetalComputeRasterPass::Uniforms) == 32u,
      "Metal compute-raster uniforms stay shader-ABI compact");
  check(
      frame.visibilityKeys == ::rendering::BufferId::Invalid &&
          frame.resolvedPayloads == ::rendering::BufferId::Invalid &&
          frame.uniforms == ::rendering::BufferId::Invalid &&
          frame.triangles == ::rendering::BufferId::Invalid &&
          frame.tileRanges == ::rendering::BufferId::Invalid &&
          frame.tileTriangleIndices == ::rendering::BufferId::Invalid,
      "runtime frame starts with no implicit fallback buffers");
  check(
      !frame.tileRangesPrepared,
      "native pass stays gated until direct decoded-VG tile ranges and depth merge exist");
  check(
      !frame.directDecodedVGPrepared,
      "decoded-VG native encode starts explicitly disabled");
  check(
      frame.cullingUniforms == ::rendering::BufferId::Invalid &&
          frame.swVisibleClusterInfos == ::rendering::BufferId::Invalid &&
          frame.decodedClusterGeometry == ::rendering::BufferId::Invalid &&
          frame.decodedVertices == ::rendering::BufferId::Invalid &&
          frame.decodedIndices == ::rendering::BufferId::Invalid,
      "decoded-VG frame starts with no implicit culling or decoded payload buffers");
  check(
      uniforms.tileGridWidth * uniforms.tileGridHeight == 3465u,
      "1220x720 uses the same 16x16 prepared-tile grid expected by the shader");
}
#endif

void testNativeMetalShaderSourceDeclaresAtomic64MinPath()
{
  std::cout << "Phase 17 native Metal shader source declares atomic64 min path\n";
  const auto shaderPath =
      sourceRoot() / vgrender::kVirtualGeometryMetalComputeRasterShaderPath;
  const std::string shader = readTextFile(shaderPath);
  check(!shader.empty(), "native Metal compute-raster shader exists");
  check(
      shader.find("device atomic_ulong *visibilityKeys") != std::string::npos,
      "native Metal shader stores visibility keys as atomic_ulong");
  check(
      shader.find("atomic_min_explicit") != std::string::npos,
      "native Metal shader uses atomic_min_explicit");
  check(
      shader.find("packVisibilityKey") != std::string::npos &&
          shader.find("depthBits") != std::string::npos,
      "native Metal shader packs depth and payload into one key");
  check(
      shader.find("terrainComputeRasterAtomic64RasterizeTiles") !=
          std::string::npos,
      "native Metal shader exposes the raster entry point");
  check(
      shader.find("terrainComputeRasterAtomic64ClearTiles") !=
              std::string::npos &&
          shader.find("tileCandidateCapacity") != std::string::npos,
      "native Metal shader clears fixed per-tile bins without a prefix-sum dependency");
  check(
      shader.find("terrainComputeRasterAtomic64CountDecodedVGClusters") !=
              std::string::npos &&
          shader.find("terrainComputeRasterAtomic64ScatterDecodedVGClusters") !=
              std::string::npos &&
          shader.find("terrainComputeRasterAtomic64RasterizeDecodedVG") !=
              std::string::npos,
      "native Metal shader exposes the direct decoded-VG count/scatter/raster kernels");
  check(
      shader.find("VGVisibleClusterInfo") != std::string::npos &&
          shader.find("VGDecodedClusterGeometryInfo") != std::string::npos &&
          shader.find("decodedVertices[geometry.vertexOffset") !=
              std::string::npos,
      "native Metal shader consumes normal VG visible-cluster and decoded geometry streams directly");
}

void testVirtualGeometryComputeRasterBufferPlan()
{
  std::cout << "Phase 17 VG compute raster allocates exportable runtime buffers\n";
  auto settings =
      virtualgeometry::gpgpu::makeVirtualGeometryComputeRasterSettings(
          true,
          1220u,
          720u,
          32.0f,
          4096u,
          virtualgeometry::gpgpu::VirtualGeometryComputeRasterPass::Backend::
              NativeMetalAtomic64);
  settings.maxSoftwareClusters = 64u;
  virtualgeometry::gpgpu::VirtualGeometryComputeRasterPass::BufferPlan plan{};
  std::string error;
  check(
      virtualgeometry::gpgpu::VirtualGeometryComputeRasterPass::buildBufferPlan(
          settings,
          plan,
          &error),
      "VG compute raster buffer plan builds: " + error);
  check(plan.enabled, "VG compute raster buffer plan is enabled");
  check(plan.metalExportable, "native Metal VG compute raster buffers are MTL-exportable");
  check(!plan.directDecodedVGConsumer, "decoded-VG consumer readiness is explicit");
  check(!plan.finalDepthPayloadMerge, "final depth/payload merge readiness is explicit");
  check(!plan.routesSoftwareClusters, "allocation alone does not route clusters into compute raster");
  check(plan.tileGridWidth == 77u && plan.tileGridHeight == 45u, "1220x720 maps to a 77x45 16px tile grid");
  check(plan.tileCount == 3465u, "tile count is explicit");
  check(plan.triangleCapacity == 64u * 128u, "triangle capacity derives from visible SW cluster capacity");
  check(
      plan.tileReferenceCapacity == plan.tileCount * 4096u,
      "tile reference capacity is fixed per tile for the no-prefix native path");
  check(
      plan.visibilityKeyBytes == uint64_t{1220u} * 720u * sizeof(uint64_t),
      "visibility key buffer stores one packed 64-bit key per pixel");
  check(
      plan.resolvedPayloadBytes ==
          uint64_t{1220u} * 720u *
              sizeof(virtualgeometry::gpgpu::VirtualGeometryComputeRasterPass::
                         PixelPayload),
      "resolved payload buffer stores one payload/depth pair per pixel");
}

void testVirtualGeometryComputeRasterUsesDecodedVGContract()
{
  std::cout << "Phase 17 VG compute raster contract consumes decoded VG directly\n";
  const auto headerPath =
      sourceRoot() /
      "src/runtime/virtualgeometry/rendering/VirtualGeometryComputeRasterPass.hpp";
  const std::string header = readTextFile(headerPath);
  check(!header.empty(), "VG compute raster header is readable");
  check(
      header.find("decodedClusterGeometry") != std::string::npos &&
          header.find("decodedVertices") != std::string::npos &&
          header.find("decodedIndices") != std::string::npos &&
          header.find("decodedClusterCull") != std::string::npos,
      "VG compute raster frame buffers expose decoded cull, cluster, vertex and index sources");
  check(
      header.find("consumes VirtualGeometryScene's decoded cluster") !=
          std::string::npos,
      "VG compute raster production contract is a direct decoded-VG consumer");
  check(
      header.find("triangle-preparation stage") == std::string::npos,
      "VG compute raster contract no longer depends on a separate triangle-preparation stage caveat");
}

void testPageDecodeSkipsDecodedOnlyGeneratedPages()
{
  std::cout << "Phase 17 VG page decode preserves decoded-only generated pages\n";
  const auto shaderPath =
      sourceRoot() /
      "assets/shaders/virtualgeometry/wgsl/virtualgeometry-page-decode-cs.wgsl";
  const std::string shader = readTextFile(shaderPath);
  check(!shader.empty(), "VG page decode shader exists");
  check(
      shader.find("entry.bufferOffset == 0xffffffffu") != std::string::npos &&
          shader.find("entry.size == 0u") != std::string::npos,
      "decoded-only generated pages skip encoded page decode");
}

void testVirtualGeometryComputeRasterRoutesOnlyWhenMergeIsReady()
{
  std::cout << "Phase 17 VG compute raster routes only after decoded-consumer and merge readiness\n";
  using Pass = virtualgeometry::gpgpu::VirtualGeometryComputeRasterPass;
  auto settings =
      virtualgeometry::gpgpu::makeVirtualGeometryComputeRasterSettings(
          true,
          1220u,
          720u,
          32.0f,
          4096u,
          Pass::Backend::NativeMetalAtomic64);
  check(
      !Pass::canRouteSoftwareClusters(settings),
      "enabled compute raster without the direct consumer/merge remains non-routing");
  settings.directDecodedVGConsumer = true;
  check(
      !Pass::canRouteSoftwareClusters(settings),
      "decoded consumer alone is not enough to steal clusters from hardware");
  settings.finalDepthPayloadMerge = true;
  check(
      Pass::canRouteSoftwareClusters(settings),
      "decoded consumer plus deterministic final merge can route SW clusters");

  Pass::BufferPlan plan{};
  std::string error;
  check(
      Pass::buildBufferPlan(settings, plan, &error),
      "routing-ready compute raster plan still builds: " + error);
  check(plan.routesSoftwareClusters, "buffer plan records production routing readiness");
}

void testVirtualGeometryComputeRasterRejectsInvalidPlans()
{
  std::cout << "Phase 17 VG compute raster rejects invalid runtime plans\n";
  using Pass = virtualgeometry::gpgpu::VirtualGeometryComputeRasterPass;
  Pass::Settings settings{};
  settings.enabled = true;
  settings.backend = Pass::Backend::NativeMetalAtomic64;
  settings.viewportWidth = 0u;
  settings.viewportHeight = 720u;
  Pass::BufferPlan plan{};
  std::string error;
  check(
      !Pass::buildBufferPlan(settings, plan, &error),
      "zero viewport is rejected");
  check(error.find("viewport") != std::string::npos, "viewport rejection is explicit");

  settings.viewportWidth = 1220u;
  settings.tileCandidateCapacity = 0u;
  check(
      !Pass::buildBufferPlan(settings, plan, &error),
      "zero tile candidate capacity is rejected");
  check(error.find("candidate") != std::string::npos, "tile-capacity rejection is explicit");
}

void testVirtualGeometryRendererRegistersComputeRasterStage()
{
  std::cout << "Phase 17 VG renderer owns the compute-raster stage\n";
  const auto shaderPath = sourceRoot() /
      "src/runtime/virtualgeometry/rendering/VirtualGeometryRendererPass.hpp";
  const std::string source = readTextFile(shaderPath);
  check(!source.empty(), "VG renderer source exists");
  check(
      source.find("VirtualGeometryComputeRasterPass::Settings computeRasterSettings") !=
          std::string::npos,
      "VG renderer settings expose compute-raster configuration");
  check(
      source.find("finalCullingSettings.enableSoftwareRaster =") !=
              std::string::npos &&
          source.find("VirtualGeometryComputeRasterPass::canRouteSoftwareClusters") !=
              std::string::npos,
      "VG final culling routes SW clusters only when decoded compute raster and final merge are ready");
  check(
      source.find("renderGraph->registerPass<VirtualGeometryComputeRasterPass>") !=
              std::string::npos &&
          source.find("finalCullingPass_->getSWVisibleClusterInfosBuffer()") !=
              std::string::npos,
      "VG renderer registers the compute pass from final-culling SW outputs");
  check(
      source.find("getComputeRasterPass()") != std::string::npos,
      "VG renderer exposes the compute-raster pass for runtime/native Metal encoding");
}

void testVirtualGeometryAdapterPreservesPackedTerrainIds()
{
  std::cout << "Phase 17 terrain VG adapter preserves packed IDs for final rendering\n";
  const auto shaderPath = sourceRoot() /
      "assets/shaders/voxel/svraster/wgsl/sparse_voxel_vg_gbuffer_adapter.wgsl";
  const std::string shader = readTextFile(shaderPath);
  check(!shader.empty(), "terrain VG G-buffer adapter shader exists");
  check(
      shader.find("packedGeometryIdsLoTexture") != std::string::npos &&
          shader.find("packedGeometryIdsHiTexture") != std::string::npos,
      "adapter reads the normal VG packed identity targets");
  check(
      shader.find("idsHi = swPixel.payload") != std::string::npos &&
          shader.find("let triangleId = idsHi & VG_ADAPTER_PAYLOAD_TRIANGLE_MASK") !=
              std::string::npos,
      "adapter decodes the unified visibility payload (cluster and triangle) from either rasterizer");
  check(
      shader.find("textureStore(baseColorTexture") != std::string::npos &&
          shader.find("vg_adapter_pack_rgb8(debugColor") != std::string::npos,
      "adapter materializes a stable debug albedo");
  check(
      shader.find("textureStore(normalTexture") != std::string::npos &&
          shader.find("geometryNormalTexture") != std::string::npos,
      "adapter forwards VG geometry normals to the final sparse-voxel normal target");
  check(
      shader.find("SPARSE_VOXEL_SURFACE_PACKED_MATERIAL") != std::string::npos,
      "adapter marks terrain hits as inline packed-material surfaces");
}

void testVirtualGeometryCullerExposesComputeRasterRouting()
{
  std::cout << "Phase 17 VG culler exposes compute-raster routing controls\n";
  const auto shaderPath =
      sourceRoot() / "assets/shaders/virtualgeometry/wgsl/culling_multipass.wgsl";
  const std::string shader = readTextFile(shaderPath);
  check(!shader.empty(), "VG multipass culling shader exists");
  check(
      shader.find("CULLING_FLAG_SOFTWARE_RASTER") != std::string::npos,
      "VG culler reads the software-raster enable flag");
  check(
      shader.find("let swThreshold = bitcast<f32>(uniforms.swTriangleEdgePixelThresholdBits)") !=
          std::string::npos,
      "VG culler reads the packed software-raster triangle-size threshold");
  check(
      shader.find("triangleEdgePixels < swThreshold") != std::string::npos,
      "VG culler routes clusters with small projected triangle edges to the software list");
  check(
      shader.find("let useSoftware = false") == std::string::npos,
      "VG culler is no longer hardwired to hardware-only raster");
  check(
      shader.find("CULLING_FLAG_DUPLICATE_SOFTWARE_RASTER_TO_HARDWARE") !=
              std::string::npos &&
          shader.find("duplicateSoftwareToHardware") != std::string::npos,
      "VG culler can mirror SW-routed clusters to hardware during native raster bring-up");
}

void testSparseVoxelSampleRunsNativeDecodedVGMetalEncoder()
{
  std::cout << "Phase 17 sample runs native decoded-VG Metal encoder\n";
  const auto samplePath =
      sourceRoot() /
      "samples/sparseVoxelRenderer/SparseVoxelRendererSample.cpp";
  const auto shaderPath =
      sourceRoot() /
      "assets/shaders/voxel/svraster/wgsl/terrain_gpu_v2_generated_vg_emit.wgsl";
  const std::string sample = readTextFile(samplePath);
  const std::string shader = readTextFile(shaderPath);
  check(!sample.empty(), "sample source is readable");
  check(!shader.empty(), "generated VG emit shader is readable");
  check(
      sample.find("VirtualGeometryMetalComputeRasterPass") !=
          std::string::npos,
      "sample owns the native Metal VG compute-raster pass");
  check(
      sample.find("makeVirtualGeometryComputeRasterSettings") !=
              std::string::npos &&
          sample.find("Backend::NativeMetalAtomic64") != std::string::npos,
      "sample enables the VG compute-raster settings for native Metal atomic64");
  check(
      sample.find("directDecodedVGConsumer = true") != std::string::npos &&
          sample.find("finalDepthPayloadMerge = true") != std::string::npos,
      "sample enables direct decoded-VG consumption and final merge readiness");
  check(
      sample.find("metalExportableBuffers = virtualGeometryMetalRaster") != std::string::npos &&
          sample.find("metalExportableSoftwareRasterBuffers") !=
              std::string::npos,
      "sample requests exportable VG scene and SW culling buffers for native Metal interop");
  check(
      sample.find("computeBuffers.decodedClusterGeometry") !=
              std::string::npos &&
          sample.find("computeBuffers.decodedVertices") !=
              std::string::npos &&
          sample.find("computeBuffers.decodedIndices") !=
              std::string::npos,
      "sample resolves decoded VG buffers into native Metal buffer ids");
  check(
      sample.find("encodeDecodedVG") != std::string::npos,
      "sample calls the direct decoded-VG native Metal encoder");
  check(
      sample.find("duplicateSoftwareRasterClustersToHardware = true") !=
          std::string::npos,
      "sample keeps hardware VG rendering alive while validating the native SW path");
}

void testVulkanTiled32FallbackRemainsBounded()
{
  std::cout << "Phase 17 Vulkan/MoltenVK fallback remains bounded to tile capacity\n";
  auto caps = appleMoltenVKWithoutVulkanAtomic64();
  caps.nativeMetalDeviceAvailable = false;
  caps.nativeMetalSupportsAtomic64MinMax = false;
  caps.maximumCandidatesPerTile = 4096u;
  const auto settings = autoSettings();
  svr::SparseVoxelTerrainComputeRasterRouteDecision decision{};
  std::string error;

  check(
      svr::selectSparseVoxelTerrainComputeRasterRoute(
          caps,
          settings,
          decision,
          &error),
      "bounded 32-bit fallback route is valid: " + error);
  check(
      decision.backend ==
          svr::SparseVoxelTerrainComputeRasterBackend::VulkanTiledAtomic32,
      "Vulkan tiled 32-bit backend is selected");
  check(decision.usesPacked32TileDepthSlotAtomic, "route uses packed 32-bit depth/tile-slot atomics");

  caps.maximumCandidatesPerTile = 4097u;
  decision = {};
  check(
      svr::selectSparseVoxelTerrainComputeRasterRoute(
          caps,
          settings,
          decision,
          &error),
      "auto unsupported overflow route fails open to hardware: " + error);
  check(!decision.enabled, "auto route disables compute raster when candidate capacity is exceeded");
}

void testComputeRasterRequiresVirtualGeometryGBufferRoute()
{
  std::cout << "Phase 17 compute raster is gated to the VG G-buffer route\n";
  const auto caps = appleMoltenVKWithoutVulkanAtomic64();
  auto settings = autoSettings();
  settings.gbufferRoute = svr::SparseVoxelTerrainGBufferRoute::BrickFallback;
  svr::SparseVoxelTerrainComputeRasterRouteDecision decision{};
  std::string error;

  check(
      svr::selectSparseVoxelTerrainComputeRasterRoute(
          caps,
          settings,
          decision,
          &error),
      "auto brick G-buffer route disables compute raster without throwing: " + error);
  check(!decision.enabled, "brick G-buffer route does not enable VG compute raster");
  check(
      decision.backend == svr::SparseVoxelTerrainComputeRasterBackend::None,
      "brick G-buffer route selects no compute-raster backend");
  check(
      decision.reason.find("virtual-geometry G-buffer") != std::string::npos,
      "disabled reason names the required VG G-buffer route");

  settings.request = svr::SparseVoxelTerrainComputeRasterRequest::Force;
  decision = {};
  check(
      !svr::selectSparseVoxelTerrainComputeRasterRoute(
          caps,
          settings,
          decision,
          &error),
      "forced compute raster fails closed outside the VG G-buffer route");
  check(
      error.find("virtual-geometry G-buffer") != std::string::npos,
      "forced failure names the required VG G-buffer route");
}

void testExplicitLegacyComputeRasterEscapeHatchStaysPossibleForDiagnostics()
{
  std::cout << "Phase 17 diagnostic escape hatch can opt out of VG route gating\n";
  const auto caps = appleMoltenVKWithoutVulkanAtomic64();
  auto settings = autoSettings();
  settings.gbufferRoute = svr::SparseVoxelTerrainGBufferRoute::BrickFallback;
  settings.requireVirtualGeometryGBufferRoute = false;
  svr::SparseVoxelTerrainComputeRasterRouteDecision decision{};
  std::string error;

  check(
      svr::selectSparseVoxelTerrainComputeRasterRoute(
          caps,
          settings,
          decision,
          &error),
      "diagnostic route can bypass VG gating explicitly: " + error);
  check(decision.enabled, "explicit diagnostic bypass can still select a compute backend");
}

void testForceUnsupportedFailsClosed()
{
  std::cout << "Phase 17 force-on unsupported path fails closed\n";
  svr::SparseVoxelTerrainComputeRasterCapabilities caps{};
  caps.platformApple = true;
  caps.moltenVKActive = true;
  caps.vulkanShaderBufferInt32Atomics = false;
  auto settings = autoSettings();
  settings.request = svr::SparseVoxelTerrainComputeRasterRequest::Force;
  svr::SparseVoxelTerrainComputeRasterRouteDecision decision{};
  std::string error;

  check(
      !svr::selectSparseVoxelTerrainComputeRasterRoute(
          caps,
          settings,
          decision,
          &error),
      "force route rejects unsupported backend");
  check(!error.empty(), "force route reports a clear error");
}

void testTriangleSizeAndOverdrawRouting()
{
  std::cout << "Phase 17 cluster routing uses triangle size and overdraw\n";
  const auto caps = appleMoltenVKWithoutVulkanAtomic64();
  const auto settings = autoSettings();
  svr::SparseVoxelTerrainComputeRasterRouteDecision decision{};
  std::string error;
  check(
      svr::selectSparseVoxelTerrainComputeRasterRoute(
          caps,
          settings,
          decision,
          &error),
      "compute raster route is available for cluster routing: " + error);

  svr::SparseVoxelTerrainComputeRasterClusterStats cluster{};
  cluster.averageProjectedTriangleEdgePixels = 31.9;
  cluster.overdrawDepthComplexityProxy = 1.0;
  cluster.maximumCandidatesPerTile = 32u;
  check(
      svr::classifySparseVoxelTerrainComputeRasterCluster(
          decision,
          settings,
          cluster) ==
          svr::SparseVoxelTerrainComputeRasterClusterRoute::Compute,
      "clusters with triangle edges smaller than the default 32 pixel threshold route to compute");

  cluster.averageProjectedTriangleEdgePixels = 32.0;
  check(
      svr::classifySparseVoxelTerrainComputeRasterCluster(
          decision,
          settings,
          cluster) ==
          svr::SparseVoxelTerrainComputeRasterClusterRoute::Hardware,
      "clusters at the 32 pixel edge threshold stay on hardware unless overdraw is high");

  cluster.averageProjectedTriangleEdgePixels = 64.0;
  cluster.overdrawDepthComplexityProxy = 4.0;
  check(
      svr::classifySparseVoxelTerrainComputeRasterCluster(
          decision,
          settings,
          cluster) ==
          svr::SparseVoxelTerrainComputeRasterClusterRoute::Compute,
      "high-overdraw clusters may route to compute even with larger triangles");

  cluster.maximumCandidatesPerTile = settings.tileCandidateCapacity + 1u;
  check(
      svr::classifySparseVoxelTerrainComputeRasterCluster(
          decision,
          settings,
          cluster) ==
          svr::SparseVoxelTerrainComputeRasterClusterRoute::HardwareOverflowFallback,
      "tile candidate overflow falls back to hardware");
}

void testGeneratedVGEmitRequestUsesDecodedOnlyTickets()
{
  std::cout << "Phase 17 generated VG emitter consumes payload-manager decoded reservations\n";
  virtualgeometry::VirtualGeometryGeneratedPayloadManager::GpuWriteDescriptor descriptor{};
  descriptor.ticketId = 0x123456789ull;
  descriptor.localPageIndex = 7u;
  descriptor.globalPageTableIndex = 41u;
  descriptor.clusterOffset = 128u;
  descriptor.clusterCount = 8u;
  descriptor.decodedVertexOffset = 2048u;
  descriptor.decodedVertexCount =
      svrender::kTerrainGPUV2GeneratedVGEmitFacesPerCluster * 8u *
      svrender::kTerrainGPUV2GeneratedVGEmitVerticesPerFace;
  descriptor.decodedIndexOffset = 4096u;
  descriptor.decodedIndexCount =
      svrender::kTerrainGPUV2GeneratedVGEmitFacesPerCluster * 8u *
      svrender::kTerrainGPUV2GeneratedVGEmitIndicesPerFace;

  svrender::TerrainGPUV2GeneratedVGEmitPageInfo page{};
  page.boundsMin = {1.0f, 2.0f, 3.0f};
  page.boundsMax = {9.0f, 10.0f, 11.0f};
  page.parentBoundsMin = {-7.0f, 2.0f, -5.0f};
  page.parentBoundsMax = {9.0f, 12.0f, 11.0f};
  page.selfError = 0.125f;
  page.parentError = 0.5f;
  page.residentChildMask = 0x5u;

  // A count transaction needs no reservation.
  svrender::TerrainGPUV2GeneratedVGEmitRequestGPU count{};
  std::string error;
  check(
      svrender::buildTerrainGPUV2GeneratedVGCountRequest(page, 2u, 1024u, 12u, 5u, count, &error),
      "generated VG count request builds without a reservation: " + error);
  check((count.flags & svrender::kTerrainGPUV2GeneratedVGEmitCountMode) != 0u, "count request carries the count mode");
  check(count.clusterCount == 0u && count.sourceNodeCount == 12u && count.sourceLane == 2u, "count request names only its source lane");

  // The cluster plan from counted tile faces: clusters tile by tile.
  std::array<uint32_t, svrender::kTerrainGPUV2GeneratedVGEmitTileCount> tileFaces{};
  tileFaces[0] = 48u;
  tileFaces[3] = 49u;
  tileFaces[63] = 1u;
  const auto plan = svrender::terrainGPUV2GeneratedVGClusterPlan(tileFaces);
  check(plan.clusterCount == 4u && plan.faceCount == 98u, "cluster plan allocates ceil(faces / 48) clusters per tile");
  check(plan.tileClusterBase[0] == 0u && plan.tileClusterBase[3] == 1u && plan.tileClusterBase[4] == 3u && plan.tileClusterBase[63] == 3u,
        "cluster plan bases are the prefix sum over tiles");

  svrender::TerrainGPUV2GeneratedVGEmitRequestGPU request{};
  check(
      svrender::buildTerrainGPUV2GeneratedVGEmitRequest(
          descriptor, page, 2u, 1024u, 12u, 5u, plan, request, &error),
      "generated VG emit request builds from flat manager descriptor: " + error);
  check(request.ticketIdLow == 0x23456789u, "request preserves low ticket bits");
  check(request.ticketIdHigh == 0x1u, "request preserves high ticket bits");
  check(request.localPageIndex == descriptor.localPageIndex, "request preserves local page index");
  check(request.globalPageTableIndex == descriptor.globalPageTableIndex, "request preserves global page table index");
  check(request.clusterOffset == descriptor.clusterOffset, "request writes into reserved cluster range");
  check(request.clusterCount == plan.clusterCount, "request advertises the planned cluster count");
  check(request.decodedVertexOffset == descriptor.decodedVertexOffset, "request writes into reserved decoded vertex range");
  check(request.decodedIndexOffset == descriptor.decodedIndexOffset, "request writes into reserved decoded index range");
  check(request.maxFacesPerCluster == svrender::kTerrainGPUV2GeneratedVGEmitFacesPerCluster, "request uses face-local VG cluster packing");
  check(request.parentError >= request.selfError, "request keeps parent error monotonic");
  check(request.residentChildMask == 0x5u && request.parentBoundsMin[0] == -7.0f, "request carries the child mask and parent sphere");
  check((request.flags & svrender::kTerrainGPUV2GeneratedVGEmitCountMode) == 0u, "emit request is not a count request");
  check(request.tileClusterBase == plan.tileClusterBase, "request carries the tile cluster bases");

  // A plan that outgrows the reservation is refused.
  tileFaces[10] = 48u * 6u;
  const auto oversized = svrender::terrainGPUV2GeneratedVGClusterPlan(tileFaces);
  check(
      !svrender::buildTerrainGPUV2GeneratedVGEmitRequest(
          descriptor, page, 2u, 1024u, 12u, 5u, oversized, request, &error),
      "generated VG emit request rejects a plan larger than the reservation");

  descriptor.decodedVertexCount = 0u;
  check(
      !svrender::buildTerrainGPUV2GeneratedVGEmitRequest(
          descriptor, page, 0u, 1u, 1u, 1u, plan, request, &error),
      "generated VG emit request rejects missing decoded geometry budget");
}

void testGeneratedVGEmitShaderWritesNormalVGDecodedBuffers()
{
  std::cout << "Phase 17 generated VG emitter shader writes normal VG decoded streams\n";
  const auto path = sourceRoot() /
      "assets/shaders/voxel/svraster/wgsl/terrain_gpu_v2_generated_vg_emit.wgsl";
  const std::string source = readTextFile(path);

  check(
      source.find("decodedClusterCullBuffer") != std::string::npos,
      "shader writes decoded cluster cull data");
  check(
      source.find("decodedClusterGeometryBuffer") != std::string::npos,
      "shader writes decoded cluster geometry data");
  check(
      source.find("decodedVertexBuffer") != std::string::npos,
      "shader writes decoded vertices");
  check(
      source.find("decodedIndexBuffer") != std::string::npos,
      "shader writes decoded indices");
  check(
      source.find("atomicAdd(&counters[counterBase(requestIndex)]") !=
          std::string::npos,
      "shader compacts exposed faces on GPU");
  check(
      source.find("faceUV(corner)") != std::string::npos,
      "shader emits cube-unwrap UVs per voxel face");
  check(
      source.find("faceNormal(face)") != std::string::npos,
      "shader emits one normal per voxel face");
  check(
      source.find("face != 3u") == std::string::npos &&
          source.find("positive-Y representative") == std::string::npos,
      "shader does not collapse generated terrain VG payloads to top faces only");
  check(
      source.find("TERRAIN_V2_GENERATED_VG_FLAG_OMIT_NEGATIVE_Y_FACE") !=
              std::string::npos &&
          source.find("face == 2u") != std::string::npos,
      "shader can explicitly omit only the hidden terrain underside without dropping vertical faces");
  check(
      source.find("neighborOccupied(request, occupancyBase, node, localNode, x, y, z, face)") !=
              std::string::npos &&
          source.find("buildTerrainGPUV2GeneratedVGNeighbours") != std::string::npos &&
          source.find("neighbourNodes[neighbourTableIndex(") != std::string::npos,
      "shader tests exposure per face against a precomputed neighbour brick table");
  // The cut is decided per tile: a cluster carries its tile's sphere with its
  // page's voxel as self error, and the parent page tile covering the same
  // ground with the parent level's voxel as parent error, so a parent's
  // clusters and its children's evaluate identical numbers on both sides of
  // the LOD test (no cracks, no double draw).
  check(
      source.find("decodedClusterCullBuffer[globalCluster].selfError = request.selfError") !=
              std::string::npos &&
          source.find("writeClusterSpheres(globalCluster, request, tile)") != std::string::npos &&
          source.find("fn tileBounds(") != std::string::npos &&
          source.find("request.parentBoundsMin") != std::string::npos,
      "shader stamps the page's errors and the tile/parent-tile spheres on every generated cluster");
  check(
      source.find("decodedVertexBuffer[vertexBase + vertex]") !=
              std::string::npos &&
          source.find("decodedClusterCullBuffer[globalCluster].localMinX = localMin.x") !=
              std::string::npos,
      "shader finalizes cluster-local bounds from emitted VG vertices");
  check(
      source.find("countTerrainGPUV2GeneratedVGFaces") != std::string::npos &&
          source.find("tileClusterBase") != std::string::npos &&
          source.find("DECODED_CLUSTER_FLAG_STREAMING_LEAF") != std::string::npos,
      "shader counts faces per tile before the emit and flags streaming leaves per quadrant");
  check(
      source.find("pagesBuffer") == std::string::npos,
      "shader does not depend on encoded VG page bytes");
  check(
      source.find("Tree64") == std::string::npos &&
          source.find("trace") == std::string::npos,
      "shader leaves Tree64/trace acceleration emission to a separate producer pass");
}

void testGeneratedVGEmitABIStaysVirtualGeometryCompatible()
{
  std::cout << "Phase 17 generated VG emitter ABI stays VG-compatible\n";
  check(
      sizeof(svrender::TerrainGPUV2GeneratedVGEmitRequestGPU) == 672u,
      "generated VG emit request ABI is fixed");
  check(
      sizeof(svrender::TerrainGPUV2GeneratedVGEmitResultGPU) == 320u,
      "generated VG emit result ABI is fixed");
  check(
      svrender::kTerrainGPUV2GeneratedVGEmitFacesPerCluster *
              svrender::kTerrainGPUV2GeneratedVGEmitVerticesPerFace <=
          virtualgeometry::VirtualGeometryScene::MAX_CLUSTER_VERTICES,
      "face-local cluster packing respects VG max vertices");
  check(
      svrender::kTerrainGPUV2GeneratedVGEmitFacesPerCluster *
              svrender::kTerrainGPUV2GeneratedVGEmitTrianglesPerFace <=
          virtualgeometry::VirtualGeometryScene::MAX_CLUSTER_TRIANGLES,
      "face-local cluster packing respects VG max triangles");
  check(
      svrender::kTerrainGPUV2GeneratedVGEmitCounterWords ==
          1u + svrender::kTerrainGPUV2GeneratedVGEmitTileCount,
      "generated VG emit scratch stores the face total plus one counter per tile");
  svrender::TerrainGPUV2GeneratedVGEmitResultGPU result{};
  result.ticketIdLow = 0x89abcdefu;
  result.ticketIdHigh = 0x01234567u;
  check(
      svrender::terrainGPUV2GeneratedVGEmitTicket(result) ==
          0x0123456789abcdefull,
      "emitter readback reconstructs 64-bit payload-manager tickets");
}

void testSparseVoxelSampleHooksGeneratedVGPageBuildChain()
{
  std::cout << "Phase 17 sample hooks generated VG page-build chain\n";
  const auto samplePath =
      sourceRoot() /
      "samples/sparseVoxelRenderer/SparseVoxelRendererSample.cpp";
  const auto shaderPath =
      sourceRoot() /
      "assets/shaders/voxel/svraster/wgsl/terrain_gpu_v2_generated_vg_emit.wgsl";
  const std::string sample = readTextFile(samplePath);
  const std::string shader = readTextFile(shaderPath);
  check(!sample.empty(), "sample source is readable");
  check(!shader.empty(), "generated VG emit shader is readable");
  check(
      sample.find("TerrainGPUV2GeneratedVGPageBuildChain") !=
          std::string::npos,
      "sample owns the generated VG page-build chain");
  check(
      sample.find("terrainGPUVGEmitResultReadback") != std::string::npos &&
          sample.find("TerrainGPUV2GeneratedVGEmitResultGPU") !=
              std::string::npos,
      "sample reads generated VG emit results back after the GPU transaction");
  check(
      sample.find("publishTerrainGPUV2GeneratedVGEmitResult") !=
          std::string::npos,
      "sample publishes decoded-only generated VG pages through the payload manager");
  check(
      sample.find("terrainGPUVGPageBuildChain->updateBatch") !=
          std::string::npos,
      "sample stages column, fine-raster and decoded-VG emit passes together");
  check(
      sample.find("uint32_t runtimeVGPageIndex = UINT32_MAX") !=
              std::string::npos &&
          sample.find("terrainGPUVGPageSet.slotOf(") != std::string::npos &&
          sample.find("configureTerrainGPUVGBuildIdentity(") != std::string::npos,
      "generated VG build maps chunk records to runtime VG page slots through the page set");
  check(
      sample.find("findGeneratedVGRuntimePageIndex") !=
              std::string::npos &&
          sample.find("record.worldCenterXMetres - record.edgeMetres * 0.5") !=
              std::string::npos &&
          sample.find("closeEnough(node.worldBoundsMinError[0], minimumX)") !=
              std::string::npos &&
          sample.find("closeEnough(node.worldBoundsMaxError[2], maximumZ)") !=
              std::string::npos,
      "generated VG runtime-page lookup can fall back to page bounds before slot metadata is visible");
  check(
      sample.find("request.localPageIndex = laneActive.runtimeVGPageIndex") !=
              std::string::npos &&
          sample.find("request.localPageIndex = record.clipmapSlot") ==
              std::string::npos,
      "generated VG payload publication uses the culler's runtime page identity, not the physical clipmap slot");
  check(
      sample.find("request.parentLocalPageIndex = laneActive.runtimeVGParentPageIndex") !=
              std::string::npos &&
          sample.find("request.requireProtectedParentOrFallback =\n"
                      "                laneActive.runtimeVGParentPageIndex != UINT32_MAX") !=
              std::string::npos,
      "generated VG child page installs preserve parent/fallback dependency metadata");
  check(
      sample.find("request.hierarchySelfError = pageInfo.selfError") !=
              std::string::npos &&
          sample.find("request.hierarchyParentError = pageInfo.parentError") !=
              std::string::npos,
      "generated VG pages carry their level's voxel and their parent level's as the leaf errors");
  check(
      shader.find("TERRAIN_V2_GENERATED_VG_FLAG_OMIT_NEGATIVE_Y_FACE") !=
              std::string::npos &&
          sample.find("kTerrainGPUV2GeneratedVGEmitOmitNegativeYFace") ==
              std::string::npos,
      "runtime generated terrain VG pages keep all exposed voxel faces by default");
  check(
      sample.find("planetChunkVoxelsProvided ? planetChunkVoxels : 64u") !=
          std::string::npos,
      "generated-world terrain defaults to 64-voxel pages that fit face-local VG payloads in CPU and GPU modes");
  check(
      sample.find("result.worldChunkSettings.maximumGPUPageBytes =\n"
                  "                  retainCPUGeneratedPages\n"
                  "                    ? 0u\n"
                  "                    : terrainGPUMemoryMiB * 1024u * 1024u;") !=
          std::string::npos,
      "explicit CPU terrain generation does not reject the reference path with the GPU-owned arena budget");
  check(
      sample.find("auto state = svr::SparseVoxelTerrainPageStateGPU{\n"
                  "        chunk.residencyGeneration,\n"
                  "        chunk.clipmapLOD,\n"
                  "        publishedFrame,") != std::string::npos,
      "GPU-owned generated terrain page states preserve generation and LOD for feedback refinement");
  check(
      sample.find("chunks.gpuOwnedGeneratedPayloads ? 0u : chunk.residencyGeneration") ==
              std::string::npos &&
          sample.find("chunks.gpuOwnedGeneratedPayloads ? 0u : chunk.clipmapLOD") ==
              std::string::npos,
      "GPU-owned generated terrain page states are not zeroed before feedback");
  check(
      sample.find("collectRuntimeObjectPageRequests(\n"
                  "                            renderer.terrainVGGeneratedRuntimeObjectName") !=
              std::string::npos &&
          sample.find("renderer.terrainGPUVGPageSet.page(requestedPage->parentSlot)") !=
              std::string::npos &&
          sample.find("parentRecord->clipmapLOD - 1u") !=
              std::string::npos,
      "VG-generated terrain consumes normal VG page-priority feedback and schedules parent-owned refinement");
  check(
      sample.find("const bool vgFeedbackAvailable =\n"
                  "                renderer.terrainVirtualGeometryScene != nullptr;") !=
              std::string::npos &&
          sample.find("(pending.terrainFeedbackReadbackSubmitted ||\n"
                      "                 vgFeedbackAvailable)") !=
              std::string::npos &&
          sample.find("pending.terrainFeedbackReadbackSubmitted &&\n"
                      "                  renderer.readWorldTerrainPageRequests(frameRequests)") !=
              std::string::npos,
      "VG terrain LOD feedback is consumed even when the legacy brick-feedback copy pass is not submitted");
  check(
      sample.find("inPlanetChunks->terrainRuntimePageTable.pages.size()") !=
              std::string::npos &&
          sample.find("inPlanetChunks->progressiveSlotCapacity") !=
              std::string::npos &&
          sample.find("terrainPageRequestCapacity = inPlanetChunks != nullptr") !=
              std::string::npos,
      "renderer terrain page-state/request tables cover generated VG runtime pages as well as physical slots");
}

void testCPUTerrainGenerationDefaultsToBrickGBuffer()
{
  // CPU terrain generation used to force the brick G-buffer, because the CPU
  // lane published only brick/Tree64 payloads. It now builds and installs
  // generated virtual geometry pages per chunk through the terrain page cache,
  // so forcing bricks meant CPU terrain always rendered as coarse brick boxes
  // with no per-cluster LOD cut. The route is now `auto`, which prefers the
  // terrain virtual-geometry G-buffer once a chunk's pages are installed and
  // falls back to bricks otherwise.
  std::cout << "Phase 17 CPU terrain generation defaults to the auto G-buffer route\n";
  const auto samplePath =
      sourceRoot() /
      "samples/sparseVoxelRenderer/SparseVoxelRendererSample.cpp";
  const std::string sample = readTextFile(samplePath);
  check(!sample.empty(), "sample source is readable");
  check(
      sample.find("std::string gbufferResolver = \"terrain-vg\"") !=
              std::string::npos &&
          sample.find("bool gbufferResolverProvided = false") !=
              std::string::npos,
      "renderer default remains generated terrain VG unless the user overrides it");
  check(
      sample.find("gbufferResolverProvided = true") != std::string::npos,
      "explicit --gbuffer-resolver is tracked");
  check(
      sample.find("terrainBuilderMode == svr::TerrainGenerationMode::CPU &&\n"
                  "        !gbufferResolverProvided") !=
              std::string::npos &&
          sample.find("gbufferResolver = \"auto\"") != std::string::npos,
      "CPU terrain generation without a resolver selects the auto route");
  check(
      sample.find("gbufferResolver = \"bricks\";") == std::string::npos,
      "no path silently forces the brick G-buffer for CPU terrain");
  check(
      sample.find("svr::TerrainGenerationMode::CPU;\n"
                  "    svr::TerrainGPUV2RequestedMode terrainGenerationMode =\n"
                  "        svr::TerrainGPUV2RequestedMode::CPU;") != std::string::npos,
      "CPU chunk generation is the renderer's default lane");
  check(
      sample.find("CPU terrain generation selected without an explicit") !=
          std::string::npos,
      "CPU fallback logs why the route changed");
}

void testSparseVoxelSampleMouseLookSurvivesOverlayCapture()
{
  std::cout << "Phase 17 sample mouse-look survives debug overlay capture\n";
  const auto samplePath =
      sourceRoot() /
      "samples/sparseVoxelRenderer/SparseVoxelRendererSample.cpp";
  const std::string sample = readTextFile(samplePath);
  check(!sample.empty(), "sample source is readable");
  check(
      sample.find("lookDragButtons |= SDL_BUTTON_MASK(event.button.button)") !=
              std::string::npos &&
          sample.find("lookDragButtons &= ~SDL_BUTTON_MASK(event.button.button)") !=
              std::string::npos,
      "sample still tracks explicit right/middle mouse-look button state for future capture modes");
  check(
      sample.find("state.planetNavigation.surfaceInputEnabled() &&\n"
                  "            !wantsKeyboard &&\n"
                  "            (mouseDeltaX != 0.0f || mouseDeltaY != 0.0f)") !=
          std::string::npos,
      "surface mouse-look is not blocked by passive ImGui mouse capture");
  check(
      sample.find("const bool surfaceControlsMouse =") !=
              std::string::npos &&
          sample.find("state.planetNavigation.surfaceInputEnabled() &&\n"
                      "                !ImGui::GetIO().WantTextInput;") !=
              std::string::npos &&
          sample.find("if (surfaceControlsMouse)\n"
                      "            {\n"
                      "              mouseDeltaX += event.motion.xrel;") !=
              std::string::npos,
      "free surface mouse/trackpad motion still drives look while the overlay passively captures the pointer");
  check(
      sample.find("if (!ImGui::GetIO().WantTextInput &&\n"
                  "                (event.button.button == SDL_BUTTON_MIDDLE") !=
          std::string::npos,
      "mouse-look start is blocked only while text input is active");
  check(
      sample.find("if (ImGui::GetIO().WantTextInput &&\n"
                  "                (event.button.button == SDL_BUTTON_MIDDLE") ==
          std::string::npos,
      "mouse-look is not guarded by an inverted text-input condition");
}

} // namespace

int main()
{
  testDisabledPathAllocatesNothing();
  testNativeMetalBypassesMoltenVKAtomic64Feature();
  testNativeMetalPassPlanRegistersAtomic64Kernels();
  testNativeMetalPassPlanRequiresBufferInterop();
  testNativeMetalBufferInteropIsExplicitOptIn();
#if defined(__APPLE__)
  testNativeMetalPreparedTileRuntimeContract();
#endif
  testNativeMetalShaderSourceDeclaresAtomic64MinPath();
  testVirtualGeometryComputeRasterBufferPlan();
  testVirtualGeometryComputeRasterUsesDecodedVGContract();
  testPageDecodeSkipsDecodedOnlyGeneratedPages();
  testVirtualGeometryComputeRasterRoutesOnlyWhenMergeIsReady();
  testVirtualGeometryComputeRasterRejectsInvalidPlans();
  testVirtualGeometryRendererRegistersComputeRasterStage();
  testVirtualGeometryAdapterPreservesPackedTerrainIds();
  testVirtualGeometryCullerExposesComputeRasterRouting();
  testSparseVoxelSampleRunsNativeDecodedVGMetalEncoder();
  testComputeRasterRequiresVirtualGeometryGBufferRoute();
  testExplicitLegacyComputeRasterEscapeHatchStaysPossibleForDiagnostics();
  testVulkanTiled32FallbackRemainsBounded();
  testForceUnsupportedFailsClosed();
  testTriangleSizeAndOverdrawRouting();
  testGeneratedVGEmitRequestUsesDecodedOnlyTickets();
  testGeneratedVGEmitShaderWritesNormalVGDecodedBuffers();
  testGeneratedVGEmitABIStaysVirtualGeometryCompatible();
  testSparseVoxelSampleHooksGeneratedVGPageBuildChain();
  testCPUTerrainGenerationDefaultsToBrickGBuffer();
  testSparseVoxelSampleMouseLookSurvivesOverlayCapture();

  if (failures != 0)
  {
    std::cerr << failures << " terrain VG Phase 17 compute-raster checks failed\n";
    return 1;
  }

  std::cout << "Sparse voxel terrain virtual-geometry Phase 17 compute-raster tests passed\n";
  return 0;
}
