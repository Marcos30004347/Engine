#include "PolygonTerrainAdaptiveGlobe.hpp"

#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelBlitPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelPlanetCbtUpdateChain.hpp"
#include "voxel/svraster/rendering/SparseVoxelPlanetOverviewPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelPlanetSurfaceAtlas.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace polygonterrain
{

namespace svr = voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;

namespace
{

constexpr float kPi = 3.14159265358979323846f;
// The fixed dual-mesh preview has always framed the globe with a 48 degree
// vertical field of view. Keeping the same value means the adaptive globe
// occupies the same screen area, which matters because the CBT's subdivision
// criterion is projected area in pixels.
constexpr float kVerticalFov = 48.0f * kPi / 180.0f;

using Matrix = std::array<float, 16>;

Matrix identityMatrix()
{
  Matrix out{};
  out[0] = 1.0f;
  out[5] = 1.0f;
  out[10] = 1.0f;
  out[15] = 1.0f;
  return out;
}

// Deliberately identical to TerrainPreviewSpherePass's rotations. The globe
// camera must not shift when the preview switches between the fixed mesh and
// the adaptive one, and the only way to guarantee that is to derive both from
// the same convention: column-major, model = Ry(yaw) * Rx(pitch).
Matrix rotationX(const float radians)
{
  Matrix out = identityMatrix();
  const float c = std::cos(radians);
  const float s = std::sin(radians);
  out[5] = c;
  out[6] = s;
  out[9] = -s;
  out[10] = c;
  return out;
}

Matrix rotationY(const float radians)
{
  Matrix out = identityMatrix();
  const float c = std::cos(radians);
  const float s = std::sin(radians);
  out[0] = c;
  out[2] = -s;
  out[8] = s;
  out[10] = c;
  return out;
}

Matrix multiplyMatrix(const Matrix &a, const Matrix &b)
{
  Matrix out{};
  for (uint32_t column = 0u; column < 4u; ++column)
    for (uint32_t row = 0u; row < 4u; ++row)
    {
      float sum = 0.0f;
      for (uint32_t k = 0u; k < 4u; ++k)
        sum += a[k * 4u + row] * b[column * 4u + k];
      out[column * 4u + row] = sum;
    }
  return out;
}

// The sphere preview rotates the *model* and leaves the camera on +Z. The
// overview pass instead wants the camera's own basis in planet space, which is
// that rotation inverted. For a rotation the inverse is the transpose, and the
// transpose's columns are the rows of the original, so each planet-space basis
// vector is simply one column of the model matrix.
std::array<float, 3> matrixColumn(const Matrix &matrix, const uint32_t column)
{
  return {matrix[column * 4u + 0u], matrix[column * 4u + 1u], matrix[column * 4u + 2u]};
}

} // namespace

bool PolygonTerrainAdaptiveGlobe::deviceSupported(gpu::RHI &rhi)
{
  const auto features = rhi.getFeatures();
  const auto properties = rhi.GetProperties();
  return svr::planetCbtAdaptiveSupported({
    .compute = (features & gpu::DeviceFeatures_Compute) != 0,
    .graphics = (features & gpu::DeviceFeatures_Graphics) != 0,
    .maximumComputeWorkgroupInvocations = static_cast<uint32_t>(
        std::min<size_t>(properties.maxComputeWorkGroupInvocations,
            std::numeric_limits<uint32_t>::max())),
    .memoryBytes = properties.maxMemory,
  });
}

PolygonTerrainAdaptiveGlobe::PolygonTerrainAdaptiveGlobe(
    TerrainPreviewRHI &rhi,
    const gpu::SwapChain swapChain,
    const uint32_t width,
    const uint32_t height,
    const svr::PlanetOverviewMesh &mesh)
    : rhi_(rhi),
      width_(std::max(1u, width)),
      height_(std::max(1u, height)),
      sourceRadiusMetres_(mesh.sourceRadiusMetres),
      maximumElevationMetres_(mesh.maximumElevationMetres)
{
  maxFramesInFlight_ = std::clamp(rhi_.getSwapChainImagesCount(swapChain), 1u, 2u);
  graph_ = std::make_unique<gpu::RenderGraph>(
      &rhi_,
      gpu::RenderGraph::Settings{
        .maxTimers = 64u,
        .maxFramesInFlight = maxFramesInFlight_,
        .enableAutomaticNodeTimers = false,
        .mergeSingleQueueCommandBuffers = true,
      });
  buildGraph(swapChain, mesh);
}

void PolygonTerrainAdaptiveGlobe::buildGraph(
    const gpu::SwapChain swapChain,
    const svr::PlanetOverviewMesh &mesh)
{
  graph_->addSwapChainImages(swapChain);

  const gpu::Texture overviewColor = graph_->createTexture({
    .name = "polygonTerrain.globe.Color",
    .format = gpu::Format::Format_RGBA16Float,
    .memoryProperties = gpu::BufferUsage::BufferUsage_None,
    .usage = gpu::ImageUsage::ImageUsage_ColorAttachment | gpu::ImageUsage::ImageUsage_Sampled,
    .width = width_,
    .height = height_,
    .depth = 1u,
    .arrayLayers = 1u,
    .mipLevels = 1u,
  });
  const gpu::Texture overviewDepth = graph_->createTexture({
    .name = "polygonTerrain.globe.Depth",
    .format = gpu::Format::Format_Depth32Float,
    .memoryProperties = gpu::BufferUsage::BufferUsage_None,
    .usage = gpu::ImageUsage::ImageUsage_DepthStencilAttachment | gpu::ImageUsage::ImageUsage_Sampled,
    .width = width_,
    .height = height_,
    .depth = 1u,
    .arrayLayers = 1u,
    .mipLevels = 1u,
  });
  const gpu::Texture swapChainTexture = graph_->createTexture({
    .name = "polygonTerrain.globe.SwapChain",
    .format = rhi_.getSwapChainFormat(swapChain),
    .memoryProperties = gpu::BufferUsage::BufferUsage_None,
    .usage = gpu::ImageUsage::ImageUsage_ColorAttachment,
    .width = width_,
    .height = height_,
    .depth = 1u,
    .arrayLayers = 1u,
    .mipLevels = 1u,
    .isVirtual = true,
  });

  uint32_t passIndex = 0u;
  atlas_ = std::make_unique<svrRendering::SparseVoxelPlanetSurfaceAtlas>(
      *graph_, mesh.surfaceMaps, "polygonTerrain.globe.SurfaceAtlas");
  cbt_ = std::make_unique<svrRendering::SparseVoxelPlanetCbtUpdateChain>(
      *graph_, passIndex, atlas_->resources(), svr::kPlanetCbtDefaultDepth,
      "polygonTerrain.globe.Cbt");

  svrRendering::SparseVoxelPlanetOverviewPass::Settings overviewSettings{};
  overviewSettings.width = width_;
  overviewSettings.height = height_;
  overviewSettings.enabled = false;
  overviewSettings.verticalFovRadians = kVerticalFov;
  overviewSettings.cameraUpSourceRadius[3] = static_cast<float>(mesh.sourceRadiusMetres);
  const svrRendering::SparseVoxelPlanetOverviewPass::CbtBuffers cbtBuffers{
    cbt_->pool(),
    cbt_->pointers(),
    cbt_->roots(),
    cbt_->drawIndirect(),
    cbt_->capacity(),
  };
  overviewPass_ = graph_->registerPass<svrRendering::SparseVoxelPlanetOverviewPass>(
      "polygonTerrain.globe.Overview", passIndex++, overviewColor, overviewDepth,
      mesh.vertices, mesh.indices, atlas_->resources(), cbtBuffers, overviewSettings);
  ownedPasses_.push_back(overviewPass_);
  ownedPasses_.push_back(graph_->registerPass<svrRendering::SparseVoxelBlitPass>(
      "polygonTerrain.globe.Blit", passIndex++, swapChainTexture,
      rhi_.getSwapChainFormat(swapChain), overviewColor, width_, height_));

  cbtCountersReadback_ = graph_->createBuffer({
    .name = "polygonTerrain.globe.CbtCountersReadback",
    .size = sizeof(svr::PlanetCbtCountersGPU),
    .usage = gpu::BufferUsage::BufferUsage_Pull | gpu::BufferUsage::BufferUsage_CopyDst,
  });
  ownedPasses_.push_back(graph_->registerPass<::rendering::gpgpu::CopyBufferPass>(
      "polygonTerrain.globe.CbtCountersReadback", passIndex++,
      cbt_->counters(), 0u, sizeof(svr::PlanetCbtCountersGPU),
      cbtCountersReadback_, 0u, sizeof(svr::PlanetCbtCountersGPU)));

  graph_->compile();

  const uint32_t imageCount = rhi_.getSwapChainImagesCount(swapChain);
  initialHandles_.resize(maxFramesInFlight_);
  frameHandles_.resize(maxFramesInFlight_);
  for (uint32_t frameSlot = 0u; frameSlot < maxFramesInFlight_; ++frameSlot)
  {
    initialHandles_[frameSlot].reserve(imageCount);
    frameHandles_[frameSlot].reserve(imageCount);
    for (uint32_t imageIndex = 0u; imageIndex < imageCount; ++imageIndex)
    {
      gpu::RenderGraph::Overrides overrides;
      const gpu::TextureView view = rhi_.getSwapChainTextureView(swapChain, imageIndex);
      overrides.textureOverrides.emplace(
          swapChainTexture.name,
          gpu::RenderGraph::TextureOverride{
            .textureId = view.resourceId,
            .layout = gpu::ResourceLayout::UNDEFINED,
          });
      initialHandles_[frameSlot].push_back(graph_->createCommandBuffer(overrides, frameSlot, false));
      frameHandles_[frameSlot].push_back(graph_->createCommandBuffer(overrides, frameSlot, true));
    }
  }
}

PolygonTerrainAdaptiveGlobe::~PolygonTerrainAdaptiveGlobe()
{
  if (graph_)
  {
    for (auto &frame : pendingFrames_)
      graph_->waitFrame(frame);
    pendingFrames_.clear();
  }
  rhi_.waitIdle();
  for (auto iterator = ownedPasses_.rbegin(); iterator != ownedPasses_.rend(); ++iterator)
    delete *iterator;
  ownedPasses_.clear();
  overviewPass_ = nullptr;
  // The update chain and the atlas hold graph resources, so they must go before
  // the graph itself.
  cbt_.reset();
  atlas_.reset();
  graph_.reset();
}

uint32_t PolygonTerrainAdaptiveGlobe::cbtCapacity() const
{
  return cbt_ ? cbt_->capacity() : 0u;
}

void PolygonTerrainAdaptiveGlobe::update(
    const AdaptiveGlobeCamera &camera,
    const AdaptiveGlobeSettings &settings)
{
  if (overviewPass_ == nullptr || !cbt_)
    return;

  const Matrix model = multiplyMatrix(
      rotationY(camera.yawDegrees * kPi / 180.0f),
      rotationX(camera.pitchDegrees * kPi / 180.0f));
  const std::array<float, 3> right = matrixColumn(model, 0u);
  const std::array<float, 3> up = matrixColumn(model, 1u);
  const std::array<float, 3> direction = matrixColumn(model, 2u);
  // The fixed mesh places the camera at 3 / zoom on a unit globe. Reusing that
  // exact expression is what keeps the two previews framed identically.
  const float distance = 3.0f / std::max(0.1f, camera.zoom);

  // Package elevations stay physical; the overview exaggerates relief so that
  // kilometre-scale features are visible across a whole globe. Cap it against
  // the radius or a small planet's mountains become spikes.
  float relief = settings.reliefExaggeration;
  if (sourceRadiusMetres_ > 0.0 && maximumElevationMetres_ > 0.0)
    relief = static_cast<float>(std::min<double>(
        settings.reliefExaggeration,
        std::max(0.0, sourceRadiusMetres_ * 0.012 / maximumElevationMetres_)));

  const float lightLength = std::sqrt(
      camera.lightDirection[0] * camera.lightDirection[0] +
      camera.lightDirection[1] * camera.lightDirection[1] +
      camera.lightDirection[2] * camera.lightDirection[2]);
  const float inverseLightLength = lightLength > 0.0001f ? 1.0f / lightLength : 1.0f;

  svrRendering::SparseVoxelPlanetOverviewPass::Settings overview{};
  overview.width = width_;
  overview.height = height_;
  overview.enabled = true;
  overview.verticalFovRadians = kVerticalFov;
  overview.cameraDirectionDistance = {direction[0], direction[1], direction[2], distance};
  overview.cameraRightRelief = {right[0], right[1], right[2], relief};
  overview.cameraUpSourceRadius = {up[0], up[1], up[2], static_cast<float>(sourceRadiusMetres_)};
  // The generator's globe is lit by the same authored direction the fixed mesh
  // uses. Negated because the overview pass wants the direction light travels,
  // while the preview settings store the direction towards the light.
  overview.sunDirectionIntensity = {
    -camera.lightDirection[0] * inverseLightLength,
    -camera.lightDirection[1] * inverseLightLength,
    -camera.lightDirection[2] * inverseLightLength,
    camera.lighting};
  overview.sunColorAtmosphere = {1.0f, 0.96f, 0.88f, 0.0f};
  overview.featureStrengths = {
    settings.forestStrength,
    settings.mountainStrength,
    settings.riverStrength,
    settings.showOcean ? settings.oceanStrength : 0.0f};
  overview.geometry = svrRendering::SparseVoxelPlanetOverviewPass::Geometry::AdaptiveCbt;
  overview.diagnostic = static_cast<svrRendering::SparseVoxelPlanetOverviewPass::Diagnostic>(
      std::clamp(settings.diagnostic, 0, 2));
  overview.diagnosticMaximumDepth = svr::kPlanetCbtMaximumSubdivisionDepth;
  overview.renderSubdivisionDepth = svr::kPlanetCbtMaximumRenderSubdivisionDepth;
  overviewPass_->update(overview);

  svr::PlanetCbtUpdateSettingsGPU cbtSettings{};
  cbtSettings.viewportFocal = {
    static_cast<float>(width_),
    static_cast<float>(height_),
    1.0f / std::tan(std::max(kVerticalFov, 0.01f) * 0.5f),
    0.001f};
  cbtSettings.cameraDirectionDistance = overview.cameraDirectionDistance;
  cbtSettings.cameraRight = overview.cameraRightRelief;
  cbtSettings.cameraUp = overview.cameraUpSourceRadius;
  cbtSettings.lodThresholds = {
    std::max(settings.projectedAreaTarget, 0.001f),
    std::max(settings.refineMultiplier, 1.0f),
    std::clamp(settings.mergeMultiplier, 0.0f, 0.99f),
    16.0f};
  cbtSettings.enableProjectedLod = settings.freezeUpdates ? 0u : 1u;
  cbtSettings.maximumDepth = svr::kPlanetCbtMaximumSubdivisionDepth;
  cbtSettings.renderSubdivisionDepth = overview.renderSubdivisionDepth;
  cbtSettings.minimumFreeSlots = svr::kPlanetCbtMaximumCompatibilityChain * 2u;
  cbt_->updateLifecycle(
      cbtSettings,
      {
        .overviewContribution = 1.0f,
        .adaptiveGeometry = true,
        .updatesEnabled = !settings.freezeUpdates,
        .packageGeneration = 1u,
      });
}

void PolygonTerrainAdaptiveGlobe::render(const uint32_t swapChainImageIndex)
{
  if (!graph_)
    return;
  const auto &handles = hasPreviousFrame_ ? frameHandles_ : initialHandles_;
  if (frameSlot_ >= handles.size() || swapChainImageIndex >= handles[frameSlot_].size())
    return;

  graph_->setCurrentFrameIndex(frameSlot_);
  gpu::RenderGraph::Frame frame{};
  frame.frameIndex = frameSlot_;
  const gpu::RenderGraph::Frame *previousFrame =
      pendingFrames_.empty() ? nullptr : &pendingFrames_.back();
  graph_->run(frame, handles[frameSlot_][swapChainImageIndex], previousFrame);

  // The ImGui overlay submits straight after this and presents the same image,
  // so the globe's work has to have landed before the overlay's render pass
  // loads it. Retire every frame but the one just submitted, then wait on that
  // one too: the overlay is not synchronized against the graph by a semaphore.
  pendingFrames_.push_back(std::move(frame));
  while (!pendingFrames_.empty())
  {
    graph_->waitFrame(pendingFrames_.front());
    pendingFrames_.erase(pendingFrames_.begin());
  }

  if (cbtCountersReadback_.isValid())
  {
    svr::PlanetCbtCountersGPU counters{};
    graph_->bufferRead(cbtCountersReadback_, 0u, sizeof(counters),
        [&](const void *data) { std::memcpy(&counters, data, sizeof(counters)); });
    activeBisectors_ = counters.activeCount;
  }

  hasPreviousFrame_ = true;
  frameSlot_ = (frameSlot_ + 1u) % maxFramesInFlight_;
}

} // namespace polygonterrain
