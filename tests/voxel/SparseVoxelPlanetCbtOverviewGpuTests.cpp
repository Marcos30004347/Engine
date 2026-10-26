#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/PlanetCbtReference.hpp"
#include "voxel/svraster/PlanetOverviewMesh.hpp"
#include "voxel/svraster/rendering/SparseVoxelPlanetCbtUpdateChain.hpp"
#include "voxel/svraster/rendering/SparseVoxelPlanetOverviewCoveragePass.hpp"
#include "voxel/svraster/rendering/SparseVoxelPlanetOverviewPass.hpp"

#include <algorithm>
#include <array>
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace ::rendering;
using namespace voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;
using Address = procedural::PlanetSurfaceBisectorAddress;

constexpr uint32_t kWidth = 320u;
constexpr uint32_t kHeight = 240u;

PlanetCbtRequestGPU requestFor(const Address &address, const uint32_t command)
{
  return {
    .rootHalfedge = address.rootHalfedge,
    .depth = address.depth,
    .pathLow = static_cast<uint32_t>(address.path),
    .pathHigh = static_cast<uint32_t>(address.path >> 32u),
    .command = command,
  };
}

PlanetOverviewMesh radialMesh(
    const PlanetCbtReference &reference,
    const float elevationMetres = 0.0f,
    const double sourceRadiusMetres = 6371000.0)
{
  PlanetOverviewMesh mesh;
  for (const Address &address : reference.activeAddresses())
  {
    procedural::PlanetSurfaceTriangle triangle{};
    assert(planetCbtReferenceTriangle(reference.mesh(), address, triangle));
    for (const auto &direction : triangle.vertices)
    {
      PlanetOverviewVertex vertex{};
      vertex.directionElevation = {static_cast<float>(direction.x), static_cast<float>(direction.y), static_cast<float>(direction.z), elevationMetres};
      vertex.baseColorForest = {0.36f, 0.48f, 0.28f, 0.0f};
      vertex.soilColorMountain = {0.48f, 0.45f, 0.40f, 0.0f};
      vertex.climateRiverWater = {0.5f, 0.5f, 0.0f, 0.0f};
      mesh.indices.push_back(static_cast<uint32_t>(mesh.vertices.size()));
      mesh.vertices.push_back(vertex);
    }
  }
  mesh.sourceRadiusMetres = sourceRadiusMetres;
  return mesh;
}

PlanetOverviewMesh::SurfaceMaps syntheticHeightMaps(
    const uint32_t faceResolution,
    const double minimumMetres,
    const double rangeMetres,
    const bool checker,
    const bool conservativeFullRange)
{
  PlanetOverviewMesh::SurfaceMaps maps{};
  maps.faceResolution = faceResolution;
  maps.width = faceResolution * 3u;
  maps.height = faceResolution * 2u;
  maps.detailMinimumMetres = minimumMetres;
  maps.detailRangeMetres = rangeMetres;
  maps.oceanFloorRangeMetres = 1.0;
  const size_t pixels = static_cast<size_t>(maps.width) * maps.height;
  maps.albedoRoughnessRGBA8.assign(pixels * 4u, 255u);
  maps.terrainFeaturesRGBA8.assign(pixels * 4u, 0u);
  maps.heightR16Float.resize(pixels);
  maps.oceanFloorR16Float.assign(
      pixels, planetOverviewFloatToHalf(0.0f));
  maps.heightMinMaxRG16Float.resize(pixels * 2u);
  for (uint32_t face = 0u; face < 6u; ++face)
    for (uint32_t y = 0u; y < faceResolution; ++y)
      for (uint32_t x = 0u; x < faceResolution; ++x)
      {
        const size_t pixel =
            static_cast<size_t>((face / 3u) * faceResolution + y) *
                maps.width +
            (face % 3u) * faceResolution + x;
        const float height = checker && ((x ^ y ^ face) & 1u) != 0u
            ? 1.0f : 0.0f;
        maps.heightR16Float[pixel] = planetOverviewFloatToHalf(height);
        maps.heightMinMaxRG16Float[pixel * 2u] =
            planetOverviewFloatToHalf(conservativeFullRange ? 0.0f : height);
        maps.heightMinMaxRG16Float[pixel * 2u + 1u] =
            planetOverviewFloatToHalf(conservativeFullRange ? 1.0f : height);
      }
  assert(maps.valid());
  return maps;
}

struct Bounds
{
  uint32_t count = 0u;
  uint32_t minimumX = kWidth;
  uint32_t minimumY = kHeight;
  uint32_t maximumX = 0u;
  uint32_t maximumY = 0u;
  bool center = false;
};

Bounds bounds(const std::vector<uint32_t> &pixels)
{
  Bounds result{};
  for (uint32_t y = 0u; y < kHeight; ++y)
    for (uint32_t x = 0u; x < kWidth; ++x)
    {
      const bool covered = pixels[uint64_t{y} * kWidth + x] != 0u;
      if (!covered)
        continue;
      ++result.count;
      result.minimumX = std::min(result.minimumX, x);
      result.minimumY = std::min(result.minimumY, y);
      result.maximumX = std::max(result.maximumX, x);
      result.maximumY = std::max(result.maximumY, y);
    }
  result.center = pixels[uint64_t{kHeight / 2u} * kWidth + kWidth / 2u] != 0u;
  return result;
}

uint64_t colorSignature(const std::vector<uint32_t> &pixels)
{
  uint64_t value = 1469598103934665603ull;
  for (const uint32_t pixel : pixels)
  {
    value ^= pixel;
    value *= 1099511628211ull;
  }
  return value;
}

struct OverviewFixture
{
  OverviewFixture(
      ::rendering::backend::vulkan::VulkanRHI &rhi,
      PlanetOverviewMesh mesh,
      const char *name,
      const float relief = 0.0f,
      const uint32_t cbtDepth = 8u)
      : graph(&rhi), sourceRadius(mesh.sourceRadiusMetres), relief(relief)
  {
    color = graph.createTexture({
      .name = std::string(name) + ".Color",
      .format = Format::Format_RGBA16Float,
      .height = kHeight,
      .width = kWidth,
      .memoryProperties = BufferUsage::BufferUsage_None,
      .mipLevels = 1u,
      .usage = ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled,
    });
    depth = graph.createTexture({
      .name = std::string(name) + ".Depth",
      .format = Format::Format_Depth32Float,
      .height = kHeight,
      .width = kWidth,
      .memoryProperties = BufferUsage::BufferUsage_None,
      .mipLevels = 1u,
      .usage = ImageUsage::ImageUsage_DepthStencilAttachment,
    });
    coverage = graph.createBuffer({
      .name = std::string(name) + ".Coverage",
      .size = uint64_t{kWidth} * kHeight * sizeof(uint32_t),
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
    });
    coverageReadback = graph.createBuffer({
      .name = std::string(name) + ".CoverageReadback",
      .size = uint64_t{kWidth} * kHeight * sizeof(uint32_t),
      .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
    });
    drawReadback = graph.createBuffer({
      .name = std::string(name) + ".DrawReadback",
      .size = sizeof(PlanetCbtDrawIndirectGPU),
      .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
    });
    countersReadback = graph.createBuffer({
      .name = std::string(name) + ".CountersReadback",
      .size = sizeof(PlanetCbtCountersGPU),
      .usage = BufferUsage::BufferUsage_Pull |
          BufferUsage::BufferUsage_CopyDst,
    });
    atlas = new svrRendering::SparseVoxelPlanetSurfaceAtlas(
        graph, mesh.surfaceMaps, std::string(name) + ".Atlas");
    chain = new svrRendering::SparseVoxelPlanetCbtUpdateChain(
        graph, passIndex, atlas->resources(), cbtDepth,
        std::string(name) + ".Cbt");
    svrRendering::SparseVoxelPlanetOverviewPass::Settings initial{};
    initial.width = kWidth;
    initial.height = kHeight;
    initial.enabled = true;
    initial.cameraDirectionDistance = cameraDirectionDistance;
    initial.cameraRightRelief = cameraRight;
    initial.cameraUpSourceRadius = cameraUp;
    const svrRendering::SparseVoxelPlanetOverviewPass::CbtBuffers buffers{
      chain->pool(),
      chain->pointers(),
      chain->roots(),
      chain->drawIndirect(),
      chain->capacity(),
    };
    overview = graph.registerPass<svrRendering::SparseVoxelPlanetOverviewPass>(
        std::string(name) + ".Overview", passIndex++, color, depth, std::move(mesh.vertices), std::move(mesh.indices), atlas->resources(), buffers, initial);
    coveragePass = graph.registerPass<svrRendering::SparseVoxelPlanetOverviewCoveragePass>(std::string(name) + ".CoveragePass", passIndex++, color, coverage, kWidth, kHeight);
    coverageCopy = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        std::string(name) + ".CoverageCopy", passIndex++, coverage, 0u, uint64_t{kWidth} * kHeight * sizeof(uint32_t), coverageReadback, 0u, uint64_t{kWidth} * kHeight * sizeof(uint32_t));
    drawCopy = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        std::string(name) + ".DrawCopy", passIndex++, chain->drawIndirect(), 0u, sizeof(PlanetCbtDrawIndirectGPU), drawReadback, 0u, sizeof(PlanetCbtDrawIndirectGPU));
    countersCopy = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        std::string(name) + ".CountersCopy", passIndex++, chain->counters(),
        0u, sizeof(PlanetCbtCountersGPU), countersReadback, 0u,
        sizeof(PlanetCbtCountersGPU));
    graph.compile();
  }

  ~OverviewFixture()
  {
    delete countersCopy;
    delete drawCopy;
    delete coverageCopy;
    delete coveragePass;
    delete overview;
    delete chain;
    delete atlas;
    graph.deleteBuffer(countersReadback);
    graph.deleteBuffer(drawReadback);
    graph.deleteBuffer(coverageReadback);
    graph.deleteBuffer(coverage);
    graph.deleteTexture(depth);
    graph.deleteTexture(color);
  }

  std::vector<uint32_t>
  run(const svrRendering::SparseVoxelPlanetOverviewPass::Geometry geometry,
      const svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic diagnostic,
      const std::vector<PlanetCbtRequestGPU> &requests = {},
      const PlanetCbtLifecycleInput *lifecycle = nullptr,
      const bool enabled = true)
  {
    PlanetCbtUpdateSettingsGPU cbt{};
    cbt.viewportFocal = {static_cast<float>(kWidth), static_cast<float>(kHeight),
        1.0f / std::tan(1.04719755f * 0.5f), 0.001f};
    cbt.cameraDirectionDistance = cameraDirectionDistance;
    cbt.cameraRight = cameraRight;
    cbt.cameraRight[3] = relief;
    cbt.cameraUp = cameraUp;
    cbt.cameraUp[3] = static_cast<float>(sourceRadius);
    cbt.lodThresholds = lodThresholds;
    cbt.enableProjectedLod = projectedLod ? 1u : 0u;
    cbt.maximumDepth = kPlanetCbtMaximumSubdivisionDepth;
    cbt.lifecycleFlags = lifecycleFlags;
    cbt.renderSubdivisionDepth = renderSubdivisionDepth;
    if (lifecycle != nullptr)
      assert(chain->updateLifecycle(cbt, *lifecycle, requests));
    else
      assert(chain->update(cbt, requests));
    svrRendering::SparseVoxelPlanetOverviewPass::Settings settings{};
    settings.width = kWidth;
    settings.height = kHeight;
    settings.enabled = enabled;
    settings.cameraDirectionDistance = cameraDirectionDistance;
    settings.cameraRightRelief = cameraRight;
    settings.cameraRightRelief[3] = relief;
    settings.cameraUpSourceRadius = cameraUp;
    settings.cameraUpSourceRadius[3] = static_cast<float>(sourceRadius);
    settings.geometry = geometry;
    settings.diagnostic = diagnostic;
    settings.diagnosticMaximumDepth = 8u;
    settings.renderSubdivisionDepth = renderSubdivisionDepth;
    overview->update(settings);
    RenderGraph::Frame frame{};
    RenderGraph::Overrides overrides{};
    graph.run(frame, overrides);
    graph.waitFrame(frame);
    std::vector<uint32_t> result(uint64_t{kWidth} * kHeight);
    graph.bufferRead(
        coverageReadback,
        0u,
        result.size() * sizeof(uint32_t),
        [&](const void *data)
        {
          std::memcpy(result.data(), data, result.size() * sizeof(uint32_t));
        });
    graph.bufferRead(
        drawReadback,
        0u,
        sizeof(draw),
        [&](const void *data)
        {
          std::memcpy(&draw, data, sizeof(draw));
        });
    graph.bufferRead(
        countersReadback,
        0u,
        sizeof(counters),
        [&](const void *data)
        {
          std::memcpy(&counters, data, sizeof(counters));
        });
    return result;
  }

  RenderGraph graph;
  uint32_t passIndex = 0u;
  Texture color{};
  Texture depth{};
  Buffer coverage{};
  Buffer coverageReadback{};
  Buffer drawReadback{};
  Buffer countersReadback{};
  svrRendering::SparseVoxelPlanetCbtUpdateChain *chain = nullptr;
  svrRendering::SparseVoxelPlanetSurfaceAtlas *atlas = nullptr;
  svrRendering::SparseVoxelPlanetOverviewPass *overview = nullptr;
  svrRendering::SparseVoxelPlanetOverviewCoveragePass *coveragePass = nullptr;
  ::rendering::gpgpu::CopyBufferPass *coverageCopy = nullptr;
  ::rendering::gpgpu::CopyBufferPass *drawCopy = nullptr;
  ::rendering::gpgpu::CopyBufferPass *countersCopy = nullptr;
  PlanetCbtDrawIndirectGPU draw{};
  PlanetCbtCountersGPU counters{};
  double sourceRadius = 6371000.0;
  float relief = 0.0f;
  bool projectedLod = false;
  std::array<float, 4> lodThresholds{49.0f, 1.25f, 0.55f, 0.0f};
  uint32_t lifecycleFlags = PlanetCbtLifecycleFlag_None;
  uint32_t renderSubdivisionDepth = 0u;
  std::array<float, 4> cameraDirectionDistance{0.0f, 0.0f, 1.0f, 2.35f};
  std::array<float, 4> cameraRight{1.0f, 0.0f, 0.0f, 0.0f};
  std::array<float, 4> cameraUp{0.0f, 1.0f, 0.0f, 0.0f};
};

void compareCoverage(const std::vector<uint32_t> &staticPixels, const std::vector<uint32_t> &adaptivePixels)
{
  assert(staticPixels.size() == adaptivePixels.size());
  uint32_t difference = 0u;
  for (size_t index = 0u; index < staticPixels.size(); ++index)
    difference += (staticPixels[index] != 0u) != (adaptivePixels[index] != 0u);
  const Bounds staticBounds = bounds(staticPixels);
  const Bounds adaptiveBounds = bounds(adaptivePixels);
  assert(staticBounds.center && adaptiveBounds.center);
  assert(staticBounds.count > kWidth * kHeight / 10u);
  assert(difference <= std::max(32u, staticBounds.count / 100u));
  assert(std::abs(static_cast<int>(staticBounds.minimumX) - static_cast<int>(adaptiveBounds.minimumX)) <= 1);
  assert(std::abs(static_cast<int>(staticBounds.minimumY) - static_cast<int>(adaptiveBounds.minimumY)) <= 1);
  assert(std::abs(static_cast<int>(staticBounds.maximumX) - static_cast<int>(adaptiveBounds.maximumX)) <= 1);
  assert(std::abs(static_cast<int>(staticBounds.maximumY) - static_cast<int>(adaptiveBounds.maximumY)) <= 1);
}

void testRootAndRefinedCoverage(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  procedural::PlanetSurfaceHalfedgeMesh halfedges;
  assert(buildPlanetCbtIcosahedron(halfedges));
  PlanetCbtReference roots;
  assert(roots.initialize(halfedges, 8u));
  OverviewFixture rootFixture(rhi, radialMesh(roots), "PlanetCbt.RootOverview");
  const auto rootStatic = rootFixture.run(svrRendering::SparseVoxelPlanetOverviewPass::Geometry::Static, svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded);
  const auto rootAdaptive = rootFixture.run(svrRendering::SparseVoxelPlanetOverviewPass::Geometry::AdaptiveCbt, svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded);
  compareCoverage(rootStatic, rootAdaptive);
  assert(rootFixture.draw.vertexCount == kPlanetCbtRootBisectorCount * 3u);

  const auto hiddenStatic = rootFixture.run(
      svrRendering::SparseVoxelPlanetOverviewPass::Geometry::Static,
      svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded,
      {}, nullptr, false);
  assert(bounds(hiddenStatic).count == 0u);
  const auto visibleStaticAgain = rootFixture.run(
      svrRendering::SparseVoxelPlanetOverviewPass::Geometry::Static,
      svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded);
  assert(visibleStaticAgain == rootStatic);

  PlanetCbtReference refined = roots;
  const Address root = refined.activeAddresses().front();
  assert(refined.split(root));
  const Address child = procedural::planetSurfaceBisectorChild(root, 0u);
  assert(refined.split(child));
  OverviewFixture refinedFixture(rhi, radialMesh(refined), "PlanetCbt.RefinedOverview");
  refinedFixture.run(svrRendering::SparseVoxelPlanetOverviewPass::Geometry::Static, svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded, {requestFor(root, PlanetCbtCommand_Split)});
  refinedFixture.run(svrRendering::SparseVoxelPlanetOverviewPass::Geometry::Static, svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded, {requestFor(child, PlanetCbtCommand_Split)});
  const auto refinedStatic = refinedFixture.run(svrRendering::SparseVoxelPlanetOverviewPass::Geometry::Static, svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded);
  const auto refinedAdaptive = refinedFixture.run(svrRendering::SparseVoxelPlanetOverviewPass::Geometry::AdaptiveCbt, svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded);
  compareCoverage(refinedStatic, refinedAdaptive);
  assert(refinedFixture.draw.vertexCount == refined.activeCount() * 3u);

  const auto wireframe = refinedFixture.run(svrRendering::SparseVoxelPlanetOverviewPass::Geometry::AdaptiveCbt, svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Wireframe);
  const auto depth = refinedFixture.run(svrRendering::SparseVoxelPlanetOverviewPass::Geometry::AdaptiveCbt, svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::SubdivisionDepth);
  compareCoverage(refinedAdaptive, wireframe);
  compareCoverage(refinedAdaptive, depth);
  assert(colorSignature(refinedAdaptive) != colorSignature(wireframe));
  assert(colorSignature(refinedAdaptive) != colorSignature(depth));

  const auto staticAgain = refinedFixture.run(svrRendering::SparseVoxelPlanetOverviewPass::Geometry::Static, svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded);
  assert(staticAgain == refinedStatic);
  std::cout << "  root/refined coverage, hidden indirect gate, diagnostics, and live switching passed\n";
}

void testTerrainDisplacementAndLod(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  procedural::PlanetSurfaceHalfedgeMesh halfedges;
  assert(buildPlanetCbtIcosahedron(halfedges));
  PlanetCbtReference roots;
  assert(roots.initialize(halfedges, 8u));

  PlanetOverviewMesh constant = radialMesh(roots, 100.0f, 1000.0);
  constant.surfaceMaps = syntheticHeightMaps(8u, 100.0, 1.0,
      false, false);
  OverviewFixture constantFixture(
      rhi, std::move(constant), "PlanetCbt.ConstantHeight", 1.0f);
  const auto constantStatic = constantFixture.run(
      svrRendering::SparseVoxelPlanetOverviewPass::Geometry::Static,
      svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded);
  const auto constantAdaptive = constantFixture.run(
      svrRendering::SparseVoxelPlanetOverviewPass::Geometry::AdaptiveCbt,
      svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded);
  compareCoverage(constantStatic, constantAdaptive);
  assert(constantFixture.draw.vertexCount ==
      kPlanetCbtRootBisectorCount * 3u);

  PlanetOverviewMesh control = radialMesh(roots, 0.0f, 1000.0);
  control.surfaceMaps = syntheticHeightMaps(8u, 0.0, 800.0,
      false, false);
  OverviewFixture controlFixture(
      rhi, std::move(control), "PlanetCbt.FlatLod", 1.0f);
  controlFixture.projectedLod = true;
  controlFixture.lifecycleFlags =
      PlanetCbtLifecycleFlag_DisableMinimumVisibleDepth;
  controlFixture.lodThresholds = {5000.0f, 1.25f, 0.55f, 0.0f};
  controlFixture.run(
      svrRendering::SparseVoxelPlanetOverviewPass::Geometry::AdaptiveCbt,
      svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded);
  const uint32_t flatVertexCount = controlFixture.draw.vertexCount;

  PlanetOverviewMesh ridge = radialMesh(roots, 0.0f, 1000.0);
  ridge.surfaceMaps = syntheticHeightMaps(8u, 0.0, 800.0,
      true, true);
  OverviewFixture ridgeFixture(
      rhi, std::move(ridge), "PlanetCbt.RidgeLod", 1.0f);
  ridgeFixture.projectedLod = true;
  ridgeFixture.lifecycleFlags =
      PlanetCbtLifecycleFlag_DisableMinimumVisibleDepth;
  ridgeFixture.lodThresholds = {5000.0f, 1.25f, 0.55f, 0.0f};
  const auto ridgeCoverage = ridgeFixture.run(
      svrRendering::SparseVoxelPlanetOverviewPass::Geometry::AdaptiveCbt,
      svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded);
  assert(bounds(ridgeCoverage).center);
  assert(flatVertexCount == kPlanetCbtRootBisectorCount * 3u);
  assert(ridgeFixture.draw.vertexCount > flatVertexCount);
  std::cout << "  constant-height displacement and terrain-aware LOD passed\n";
}

void testNavigationLifecycleAndStability(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  procedural::PlanetSurfaceHalfedgeMesh halfedges;
  assert(buildPlanetCbtIcosahedron(halfedges));
  PlanetCbtReference roots;
  assert(roots.initialize(halfedges, 8u));
  OverviewFixture fixture(
      rhi, radialMesh(roots), "PlanetCbt.Lifecycle");
  fixture.projectedLod = true;
  fixture.lodThresholds = {36.0f, 1.25f, 0.55f, 16.0f};
  PlanetCbtLifecycleInput lifecycle{
    .overviewContribution = 1.0f,
    .adaptiveGeometry = true,
    .packageGeneration = 11u,
  };
  const uint64_t storageBytes = fixture.chain->storageBytes();
  const std::string poolName = fixture.chain->pool().name;
  auto image = fixture.run(
      svrRendering::SparseVoxelPlanetOverviewPass::Geometry::AdaptiveCbt,
      svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded,
      {}, &lifecycle);
  assert(bounds(image).center);
  assert(fixture.chain->lastLifecycleDecision().updateRoundCount ==
      kPlanetCbtMaximumUpdateRounds);
  assert(fixture.draw.vertexCount >= kPlanetCbtRootBisectorCount * 3u);

  // The first run above consumes one warm-up frame. Run the remaining warm-up
  // and convergence schedule plus the first settled frame.
  for (uint32_t frame = 0u;
       frame < kPlanetCbtWarmupFrameCount +
               kPlanetCbtConvergenceFrameCount;
       ++frame)
    image = fixture.run(
        svrRendering::SparseVoxelPlanetOverviewPass::Geometry::AdaptiveCbt,
        svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded,
        {}, &lifecycle);
  assert(fixture.chain->lastLifecycleDecision().settled);
  assert(fixture.chain->lastLifecycleDecision().updateRoundCount == 0u);
  assert(fixture.counters.splitCount == 0u &&
      fixture.counters.mergeCount == 0u);
  const uint32_t settledVertices = fixture.draw.vertexCount;

  lifecycle.overviewContribution = 0.0f;
  image = fixture.run(
      svrRendering::SparseVoxelPlanetOverviewPass::Geometry::AdaptiveCbt,
      svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded,
      {}, &lifecycle);
  assert(fixture.draw.vertexCount == 0u);
  assert(fixture.chain->lastLifecycleDecision().updateRoundCount == 0u);

  lifecycle.overviewContribution = 1.0f;
  uint32_t previousVertices = settledVertices;
  for (uint32_t step = 0u; step < 36u; ++step)
  {
    const float angle = static_cast<float>(step) * 0.174532925f;
    fixture.cameraDirectionDistance = {
      std::sin(angle), 0.0f, std::cos(angle), 2.35f};
    fixture.cameraRight = {
      std::cos(angle), 0.0f, -std::sin(angle), 0.0f};
    image = fixture.run(
        svrRendering::SparseVoxelPlanetOverviewPass::Geometry::AdaptiveCbt,
        svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded,
        {}, &lifecycle);
    assert(bounds(image).center);
    assert(fixture.draw.vertexCount >= kPlanetCbtRootBisectorCount * 3u);
    assert(fixture.draw.vertexCount <= fixture.chain->capacity() * 3u);
    assert(fixture.draw.vertexCount <= previousVertices * 2u &&
        previousVertices <= fixture.draw.vertexCount * 2u);
    previousVertices = fixture.draw.vertexCount;
  }
  assert(fixture.chain->storageBytes() == storageBytes &&
      fixture.chain->pool().name == poolName);

  lifecycle.packageGeneration = 12u;
  image = fixture.run(
      svrRendering::SparseVoxelPlanetOverviewPass::Geometry::AdaptiveCbt,
      svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded,
      {}, &lifecycle);
  assert(fixture.chain->lastLifecycleDecision().resetTopology);
  assert(fixture.draw.vertexCount == kPlanetCbtRootBisectorCount * 3u);
  assert(fixture.counters.activeCount == kPlanetCbtRootBisectorCount);
  assert(bounds(image).center);

  PlanetCbtReference tinyRoots;
  assert(tinyRoots.initialize(halfedges, 6u));
  OverviewFixture tiny(
      rhi, radialMesh(tinyRoots), "PlanetCbt.TinyLifecycle", 0.0f, 6u);
  tiny.projectedLod = true;
  tiny.lodThresholds = {0.001f, 1.25f, 0.55f, 16.0f};
  PlanetCbtLifecycleInput tinyLifecycle{
    .overviewContribution = 1.0f,
    .adaptiveGeometry = true,
    .packageGeneration = 1u,
  };
  const auto tinyImage = tiny.run(
      svrRendering::SparseVoxelPlanetOverviewPass::Geometry::AdaptiveCbt,
      svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded,
      {}, &tinyLifecycle);
  assert(bounds(tinyImage).center);
  assert(tiny.draw.vertexCount == kPlanetCbtRootBisectorCount * 3u);
  assert(tiny.counters.overflowCount == 0u);
  std::cout << "  warm-up, pause, orbit stability, reload, and tiny-pool fallback passed\n";
}

void testFarOrbitMinimumTessellation(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  procedural::PlanetSurfaceHalfedgeMesh halfedges;
  assert(buildPlanetCbtIcosahedron(halfedges));
  PlanetCbtReference roots;
  assert(roots.initialize(halfedges, 8u));
  PlanetOverviewMesh mesh = radialMesh(roots);
  mesh.surfaceMaps = syntheticHeightMaps(
      8u, 0.0, 1.0, false, false);
  for (uint32_t resolution = 4u; resolution != 0u; resolution /= 2u)
  {
    PlanetOverviewMesh::SurfaceMaps::MipLevel mip{};
    mip.faceResolution = resolution;
    mip.width = resolution * 3u;
    mip.height = resolution * 2u;
    const size_t pixels = static_cast<size_t>(mip.width) * mip.height;
    mip.albedoRoughnessRGBA8.assign(pixels * 4u, 255u);
    mip.terrainFeaturesRGBA8.assign(pixels * 4u, 0u);
    mip.heightR16Float.assign(
        pixels, planetOverviewFloatToHalf(0.0f));
    mip.oceanFloorR16Float.assign(
        pixels, planetOverviewFloatToHalf(0.0f));
    mip.heightMinMaxRG16Float.assign(
        pixels * 2u, planetOverviewFloatToHalf(0.0f));
    mesh.surfaceMaps.mipLevels.push_back(std::move(mip));
  }
  OverviewFixture fixture(
      rhi, std::move(mesh), "PlanetCbt.FarOrbitFloor", 0.0f, 12u);
  fixture.projectedLod = true;
  // Projected area alone requests roots at this threshold. The production
  // visible-depth floor must still converge to a smooth globe silhouette.
  fixture.lodThresholds = {1.0e9f, 1.25f, 0.55f, 16.0f};
  fixture.cameraDirectionDistance = {0.0f, 0.0f, 1.0f, 8.0f};
  fixture.renderSubdivisionDepth =
      kPlanetCbtMaximumRenderSubdivisionDepth;
  PlanetCbtLifecycleInput lifecycle{
    .overviewContribution = 1.0f,
    .adaptiveGeometry = true,
    .packageGeneration = 1u,
  };
  std::vector<uint32_t> image;
  uint32_t maximumObservedDepth = 0u;
  for (uint32_t frame = 0u;
       frame < kPlanetCbtWarmupFrameCount +
               kPlanetCbtConvergenceFrameCount + 2u;
       ++frame)
  {
    image = fixture.run(
        svrRendering::SparseVoxelPlanetOverviewPass::Geometry::AdaptiveCbt,
        svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic::Shaded,
        {}, &lifecycle);
    maximumObservedDepth = std::max(
        maximumObservedDepth, fixture.counters.maximumDepth);
  }
  assert(bounds(image).center);
  assert(maximumObservedDepth >=
      kPlanetCbtMinimumVisibleSubdivisionDepth);
  assert(fixture.counters.activeCount >
      kPlanetCbtRootBisectorCount * 8u);
  assert(fixture.draw.instanceCount ==
      kPlanetCbtMaximumRenderSubtriangleCount);
  // This fixture deliberately places the globe much farther away than the
  // normal framed overview. Even there, render subdivision keeps the draw
  // near one million triangles at a tiny 320x240 test resolution.
  assert(uint64_t{fixture.draw.vertexCount / 3u} *
      fixture.draw.instanceCount >= 900000u);
  assert(fixture.counters.overflowCount == 0u);
  std::cout << "  far-orbit visible tessellation floor passed\n";
}

} // namespace

int main(const int argc, char **argv)
{
  os::Logger::start();
  ::rendering::DeviceRequiredLimits limits{
    .minimumMemory = 32u * 1024u * 1024u,
    .minimumComputeSharedMemory = 0u,
    .minimumComputeWorkGroupInvocations = 64u,
  };
  const auto features = static_cast<::rendering::DeviceFeatures>(::rendering::DeviceFeatures::DeviceFeatures_Compute | ::rendering::DeviceFeatures::DeviceFeatures_Graphics);
  ::rendering::backend::vulkan::VulkanRHI rhi(::rendering::backend::vulkan::Vulkan_1_2, limits, features, {}, true);
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  const bool phase5 = argc > 1 && std::string(argv[1]) == "--phase5";
  const bool phase6 = argc > 1 && std::string(argv[1]) == "--phase6";
  const bool phase7 = argc > 1 && std::string(argv[1]) == "--phase7";
  if (phase6 || phase7)
  {
    std::cout << "Planet CBT phase-" << (phase7 ? 7 : 6)
              << " navigation/rollout regression\n";
    testNavigationLifecycleAndStability(rhi);
    testFarOrbitMinimumTessellation(rhi);
    std::cout << "Planet CBT phase-" << (phase7 ? 7 : 6)
              << " GPU tests passed\n";
  }
  else if (phase5)
  {
    std::cout << "Planet CBT phase-5 terrain displacement\n";
    testTerrainDisplacementAndLod(rhi);
    std::cout << "Planet CBT phase-5 GPU tests passed\n";
  }
  else
  {
    std::cout << "Planet CBT phase-4 adaptive overview\n";
    testRootAndRefinedCoverage(rhi);
    testFarOrbitMinimumTessellation(rhi);
    std::cout << "Planet CBT phase-4 GPU tests passed\n";
  }
  os::Logger::shutdown();
  return 0;
}
