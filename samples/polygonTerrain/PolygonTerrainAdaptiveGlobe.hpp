#pragma once

#include "PolygonTerrainPreviewPasses.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/PlanetCbt.hpp"
#include "voxel/svraster/PlanetOverviewMesh.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace voxel::svraster::rendering
{
class SparseVoxelPlanetCbtUpdateChain;
class SparseVoxelPlanetOverviewPass;
class SparseVoxelPlanetSurfaceAtlas;
} // namespace voxel::svraster::rendering

namespace polygonterrain
{

// Camera for the globe, in the generator's own terms. Yaw, pitch and zoom are
// the values the existing drag/wheel handlers already maintain; this keeps the
// adaptive globe on exactly the same controls as the fixed dual mesh it
// replaces, so switching between them does not move the camera.
struct AdaptiveGlobeCamera
{
  float yawDegrees = -25.0f;
  float pitchDegrees = -12.0f;
  float zoom = 1.0f;
  std::array<float, 3> lightDirection{-0.38f, 0.44f, 0.82f};
  float lighting = 1.0f;
};

struct AdaptiveGlobeSettings
{
  // Matches SparseVoxelRendererSample's defaults so the generator preview and
  // the renderer subdivide identically for the same package.
  float projectedAreaTarget = voxel::svraster::kPlanetCbtTargetProjectedArea;
  float refineMultiplier = 1.25f;
  float mergeMultiplier = 0.55f;
  // Kilometre-scale relief covers less than a pixel across a whole globe, so
  // the overview exaggerates it visually while package elevations stay
  // physical. Clamped against the planet radius by the pass itself.
  float reliefExaggeration = 48.0f;
  float forestStrength = 0.85f;
  float mountainStrength = 0.90f;
  float riverStrength = 1.15f;
  float oceanStrength = 1.0f;
  bool freezeUpdates = false;
  // 0 shaded, 1 wireframe, 2 subdivision depth.
  int diagnostic = 0;
  bool showOcean = true;
};

// Owns a RenderGraph and the complete adaptive-CBT globe chain: the resident
// surface atlas, the per-frame CBT update chain, the overview draw, and the
// blit into the swapchain image.
//
// The sample is otherwise raw Vulkan. This class is the only RenderGraph in it,
// and it deliberately renders into the acquired swapchain image *before* the
// ImGui overlay render pass runs, so the overlay draws on top of the globe the
// same way SparseVoxelRendererSample layers its own UI.
class PolygonTerrainAdaptiveGlobe final
{
public:
  // Reports whether this device can run the adaptive CBT at all. A device that
  // cannot must fall back to the fixed dual mesh rather than showing nothing.
  static bool deviceSupported(gpu::RHI &rhi);

  PolygonTerrainAdaptiveGlobe(
      TerrainPreviewRHI &rhi,
      gpu::SwapChain swapChain,
      uint32_t width,
      uint32_t height,
      const voxel::svraster::PlanetOverviewMesh &mesh);
  ~PolygonTerrainAdaptiveGlobe();

  PolygonTerrainAdaptiveGlobe(const PolygonTerrainAdaptiveGlobe &) = delete;
  PolygonTerrainAdaptiveGlobe &operator=(const PolygonTerrainAdaptiveGlobe &) = delete;

  // Pushes this frame's camera and LOD settings into the overview pass and the
  // CBT update chain. Call once per frame before render().
  void update(const AdaptiveGlobeCamera &camera, const AdaptiveGlobeSettings &settings);

  // Runs the graph into the acquired swapchain image. swapChainImageIndex is
  // TextureView::index for the image the frame will present.
  void render(uint32_t swapChainImageIndex);

  uint32_t width() const
  {
    return width_;
  }
  uint32_t height() const
  {
    return height_;
  }
  uint32_t cbtCapacity() const;

  // Live bisector count, read back after the last completed frame. This is the
  // direct evidence that zooming subdivides: it climbs as the camera closes in
  // and falls again as it pulls out.
  uint32_t activeBisectors() const
  {
    return activeBisectors_;
  }

private:
  void buildGraph(
      gpu::SwapChain swapChain,
      const voxel::svraster::PlanetOverviewMesh &mesh);

  TerrainPreviewRHI &rhi_;
  uint32_t width_ = 0u;
  uint32_t height_ = 0u;
  uint32_t frameSlot_ = 0u;
  uint32_t maxFramesInFlight_ = 2u;
  double sourceRadiusMetres_ = 0.0;
  double maximumElevationMetres_ = 0.0;
  bool hasPreviousFrame_ = false;

  uint32_t activeBisectors_ = 0u;
  gpu::Buffer cbtCountersReadback_{};

  std::unique_ptr<gpu::RenderGraph> graph_;
  std::unique_ptr<voxel::svraster::rendering::SparseVoxelPlanetSurfaceAtlas> atlas_;
  std::unique_ptr<voxel::svraster::rendering::SparseVoxelPlanetCbtUpdateChain> cbt_;
  // Owned by the graph; deleted through it.
  voxel::svraster::rendering::SparseVoxelPlanetOverviewPass *overviewPass_ = nullptr;
  std::vector<gpu::Pass *> ownedPasses_;
  // One command buffer per swapchain image per frame slot, as the renderer
  // sample does. Indexed [frameSlot][swapChainImageIndex].
  std::vector<std::vector<gpu::RenderGraph::CommandBufferHandle>> initialHandles_;
  std::vector<std::vector<gpu::RenderGraph::CommandBufferHandle>> frameHandles_;
  std::vector<gpu::RenderGraph::Frame> pendingFrames_;
};

} // namespace polygonterrain
