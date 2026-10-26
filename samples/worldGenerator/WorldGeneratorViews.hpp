#pragma once

#include "procedural/world/MapClimate.hpp"
#include "procedural/world/MapCoast.hpp"
#include "procedural/world/MapContours.hpp"
#include "procedural/world/MapGraph.hpp"
#include "procedural/world/MapRegions.hpp"
#include "procedural/world/MapRivers.hpp"
#include "procedural/world/land/Blend.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"

#include <backends/imgui_impl_vulkan.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace worldgen
{

namespace gpu = rendering;
namespace vkbackend = rendering::backend::vulkan;

class WorldMapRHI final : public vkbackend::VulkanRHI
{
public:
  using vkbackend::VulkanRHI::VulkanRHI;

  vkbackend::VulkanTextureView createDepthTextureView(gpu::TextureId texture);
  void destroyDepthTextureView(vkbackend::VulkanTextureView &view);
  VkBuffer bufferHandle(gpu::BufferId buffer);
};

// Clears the swapchain image, invokes the dedicated map pass, then draws ImGui
// controls and vector overlays in the same render pass.
class WorldMapOverlayPass
{
public:
  explicit WorldMapOverlayPass(WorldMapRHI &rhi);
  ~WorldMapOverlayPass();

  WorldMapOverlayPass(const WorldMapOverlayPass &) = delete;
  WorldMapOverlayPass &operator=(const WorldMapOverlayPass &) = delete;

  void initialize(gpu::SwapChain swapChain);
  void destroy();
  void destroyFramebuffers();
  void onSwapChainRecreated(gpu::SwapChain swapChain);
  void render(gpu::SwapChain &swapChain, const gpu::TextureView &swapChainView, ImDrawData *drawData, const std::function<void(VkCommandBuffer, VkExtent2D)> &beforeImGui = {});

  static void checkVkResult(VkResult result);
  VkRenderPass renderPass() const
  {
    return renderPass_;
  }

private:
  struct FramebufferResources
  {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    gpu::TextureId depthTexture = gpu::TextureId::Invalid;
    vkbackend::VulkanTextureView depthView{};
  };

  static VkRenderPass createRenderPass(VkDevice device, VkFormat format);

  WorldMapRHI &rhi_;
  VkRenderPass renderPass_ = VK_NULL_HANDLE;
  std::unordered_map<uintptr_t, FramebufferResources> framebuffers_;
};

// ---------------------------------------------------------------------------
// Map rendering
// ---------------------------------------------------------------------------

// The colour a cell type contributes to the blend, linear 0..1 so that mixing
// happens in the same space the weights were computed in.
std::array<float, 3> cellTypeColour(procedural::world::CellType type);

// What the fill shows. Overlays are independent of this, so rivers and the
// coastline stay on top of whichever layer is selected.
enum class MapLayer : uint8_t
{
  // Region content, blended - the fantasy-map read.
  Content = 0u,
  // Landform, blended. What the ground does, before anything grows on it.
  Landform,
  // Hypsometric: a bathymetric ramp below the datum and a land ramp above it,
  // with the shoreline landing exactly on the datum rather than somewhere in
  // the middle of a continuous ramp.
  Elevation,
  // Uplift rate. Separate from elevation on purpose: they are different fields
  // and a range that is high without being pushed up is the failure this view
  // exists to make visible.
  Uplift,
  // Land type, blended. The climate stage's answer rather than the region
  // stage's.
  Biome,
  // Hillshaded relief: the heightmap as terrain rather than as a colour ramp.
  // A ramp says how high; a hillshade says what shape, and drainage and
  // escarpments are only legible in the second.
  Heightmap,
  // One flat colour per region, to see the partition itself.
  Region,

  Count,
};

const char *mapLayerName(MapLayer layer);

struct MapShading
{
  // The land type layer, because it is the one that says something. `Content`
  // is the older region-scored cover and comes out as a single landform across
  // a landmass, which is what the default view used to show.
  MapLayer layer = MapLayer::Biome;

  // Normalization for the elevation and uplift ramps. Taken from the brief so
  // a flatter world does not simply come out darker.
  double seaLevelMetres = 0.0;
  double reliefMetres = 3000.0;
  double peakUpliftMetresPerYear = 1.0e-3;

  // Sun bearing and altitude for the hillshade, in degrees. 315/45 is the
  // cartographic convention - lit from the top left, because relief lit from
  // below reads as inverted to most people.
  double sunBearingDegrees = 315.0;
  double sunAltitudeDegrees = 45.0;
  double verticalExaggeration = 2.5;

  // Subdivision levels for the noisy cell boundaries, after Red Blob Games'
  // "noisy edges". Zero draws the straight Voronoi edges.
  //
  // The wiggle belongs to the *edge*, not to either cell: an edge is shared by
  // exactly two cells and is defined by the same quadrilateral - its two
  // Voronoi corners and the two sites either side - so both cells generate the
  // same path from it and the fills meet exactly. Generating it per cell
  // instead leaves gaps along every boundary.
  // Boundary shaping. The map's edges are welded into a shared structure and
  // then displaced through one smooth field of position, so every cell reading
  // a boundary reads the same points.
  double weldToleranceMetres = 1.0;
  double edgeSegmentMetres = 2500.0;
  double edgeAmplitudeMetres = 4200.0;
  double edgeWavelengthMetres = 30000.0;
  // Categorical edges use a calmer displacement than coastlines. Boundary
  // rounding is intentionally disabled; only this shared domain warp shapes
  // them beyond their original cell edge.
  double boundaryNoiseScale = 0.18;

  // Terrain-aware categorical threshold. Seeded per-cell weights first move
  // transitions away from uniform cell midpoints, then local height variation
  // shifts the hard winner on slopes and escarpments. Flat cells bypasses the
  // field for debug.
  procedural::world::HeightBlendSettings heightBlend{};
  bool flatCells = false;
  // Preview residency cut. Two exposes all current 1024 maps; lowering it
  // leaves finer logical addresses unresolved so the shader exercises ancestor
  // fallback without rebuilding or re-uploading the page table.
  uint32_t materialResidentLevel = 2u;

  // Tints coastal land toward beach and shallow water toward a shelf blue.
  //
  // Purely a view decision, and switchable, because coastal and shallow are
  // derived flags rather than cell types: nothing in the graph changes. Until
  // the elevation and region stages give land more than one type, this is the
  // only variation the blend has to work with, so without it the map is two
  // flat colours and there is nothing to judge the blending by.
  bool shadeShoreAndShelf = true;
  double shoreStrength = 0.45;
  double shelfStrength = 0.40;
};

// Colour a single cell contributes to the blend. Type first, then the derived
// shore and shelf tint.
std::array<float, 3> nodeColour(const procedural::world::MapNode &node, const MapShading &shading);

// A distinct, stable colour per region, for the Region layer.
std::array<float, 3> regionColour(uint32_t region);

// Atlas colours for the land types.
std::array<float, 3> biomeColour(procedural::world::Biome biome);

// Elevation at a point on the shared surface used by contours and renderers.
double blendedElevation(
    const procedural::world::MapSurfaceField &surface,
    procedural::Point2 position);

// Lambertian shade for a surface gradient, 0..1.
double hillshade(double dzdx, double dzdy, const MapShading &shading);

// A label placed on the map, in world metres. Produced with the mesh so the
// draw path has nothing to compute.
struct MapLabel
{
  procedural::Point2 position;
  std::string text;
  // Larger reads as more important. 1 is a region, 0 a river.
  uint8_t rank = 1u;
  // Breaks collisions: the bigger region and the bigger river keep their label.
  double weight = 0.0;
};

struct MapVertex
{
  // World metres. The screen transform happens at draw time, so panning and
  // zooming never rebuilds the mesh.
  float x = 0.0f;
  float y = 0.0f;
  // Renderer-neutral material channels. Categorical fields are pre-blended at
  // the vertex; elevation stays scalar so the GPU can re-ramp it and derive
  // hillshade after interpolation without a mesh rebuild.
  uint32_t contentColour = 0u;
  uint32_t landformColour = 0u;
  uint32_t upliftColour = 0u;
  uint32_t biomeColour = 0u;
  uint32_t regionColour = 0u;
  float elevationMetres = 0.0f;
};

// One draw-list submission's worth of geometry. ImGui indices are 16 bit, and
// PrimReserve only starts a new vertex offset between reservations, so the mesh
// is split into batches that each fit rather than left as one array that would
// silently wrap.
struct MapBatch
{
  std::vector<MapVertex> vertices;
  std::vector<uint16_t> indices;
};

// A cell as it is drawn: its boundary, noisy or not, and its colour. Both
// renderers work from this, so the window and the exported image cannot
// disagree about where a cell ends.
struct MapCellShape
{
  std::vector<procedural::Point2> boundary;
  procedural::Point2 site;
  uint32_t node = procedural::world::kInvalidNode;
  uint32_t colour = 0u;
};

// One texel of the categorical material field. The first biome channel keeps
// the hard winner for classification; biomeBlendColour is a presentation-only
// mixture of the same weighted votes and is sampled bilinearly by the renderer.
struct MapMaterialSample
{
  uint32_t contentColour = 0u;
  uint32_t landformColour = 0u;
  uint32_t biomeColour = 0u;
  uint32_t biomeBlendColour = 0u;
  uint32_t regionColour = 0u;
  // Zero is water, one is land. Bilinear reconstruction thresholds this mask
  // before blending colours so an ecotone never smears across a shoreline.
  uint32_t surfaceClass = 0u;
  // Four slot-matched bytes, identical in meaning to biome_mix_ids and
  // biome_mix_weights in export schema v4. Keeping them in the preview field
  // lets the GPU exercise the real semantic sampling path.
  uint32_t biomeIds = 0u;
  uint32_t biomeWeights = 0u;
};

struct MapMesh
{
  std::vector<MapBatch> batches;
  std::vector<MapCellShape> cells;
  std::vector<procedural::world::RiverSegment> rivers;
  // Short cross-channel cartographic marks at selected knickpoints.
  std::vector<std::array<float, 4>> waterfallMarks;
  std::vector<procedural::world::ContourPolyline> contours;
  std::vector<MapLabel> labels;
  std::vector<MapMaterialSample> materialField;
  uint32_t materialFieldSide = 0u;
  double materialFieldExtentMetres = 0.0;
  // Fully resident preview cut of the export-style quadtree. Page-table entries
  // contain physical page index + 1; zero means missing and triggers ancestor
  // fallback in the shader. Every physical page includes the declared gutter.
  std::vector<uint32_t> materialPageTable;
  std::vector<MapMaterialSample> materialPages;
  uint32_t materialPageSize = 256u;
  uint32_t materialPageGutter = 1u;
  uint32_t materialPageStoredSide = 0u;
  uint32_t materialMaximumPageLevel = 0u;
  double largestDischargeCumecs = 0.0;
  // Cell boundaries, in world metres, as unique segments. Debug overlay only.
  std::vector<std::array<float, 4>> cellEdges;
  std::vector<procedural::world::CoastPolyline> coastline;
  size_t triangleCount = 0u;

  bool empty() const
  {
    return batches.empty();
  }
};

// Builds cell coverage plus a separate thresholded categorical field.
//
// Each cell still supplies watertight coverage and continuous elevation. The
// categorical layers come from a regular material field whose smooth regional
// and height influences have already been resolved through an arg-max cutoff.
// GPU and image export therefore select exactly one type at a sample—never an
// interpolated colour—while layer changes still avoid rebuilding geometry.
MapMesh buildMapMesh(
    const procedural::world::MapGraph &graph,
    const std::vector<procedural::world::CoastPolyline> &coastline,
    const procedural::world::RiverNetwork &rivers,
    const procedural::world::RegionMap &regions,
    const procedural::world::ContourSet &contours,
    const procedural::world::MapSurfaceField &surface,
    const procedural::world::SiteIndex &index,
    const procedural::world::BlendSettings &settings,
    const MapShading &shading);

// Maps world metres to screen pixels. Y is negated: world y increases north and
// screen y increases down.
struct MapView
{
  double centreX = 0.0;
  double centreY = 0.0;
  double metresPerPixel = 1.0;
};

MapView fitView(double extentMetres, float widthPixels, float heightPixels);

struct MapOverlayStyle;

// Dedicated GPU map path. Generation remains on the CPU, but material channels
// are renderer-neutral: stable fill triangles and analytic line instances
// upload only when the map or categorical blend changes, while layer choice,
// relief shading, pan, zoom, visibility, and stroke widths are uniforms/draws.
class WorldMapGpuPass
{
public:
  WorldMapGpuPass(WorldMapRHI &rhi, std::string shaderPath);
  ~WorldMapGpuPass();

  WorldMapGpuPass(const WorldMapGpuPass &) = delete;
  WorldMapGpuPass &operator=(const WorldMapGpuPass &) = delete;

  void initialize(VkRenderPass renderPass);
  void destroy();
  bool upload(const MapMesh &mesh, std::string *error = nullptr);
  void render(VkCommandBuffer commandBuffer, VkExtent2D framebufferExtent, const MapView &view, ImVec2 origin, ImVec2 size, const MapShading &shading, const MapOverlayStyle &style, bool drawFill, bool drawLines);
  bool ready() const;
  bool linesReady() const;
  size_t vertexCount() const
  {
    return vertexCount_;
  }
  size_t indexCount() const
  {
    return indexCount_;
  }
  size_t lineInstanceCount() const
  {
    return lineInstanceCount_;
  }
  size_t materialPageCount() const
  {
    return materialPageCount_;
  }
  size_t materialPageBytes() const
  {
    return materialPageBytes_;
  }

private:
  struct Uniforms
  {
    std::array<float, 4> transform{};
    std::array<float, 4> viewport{};
    std::array<float, 4> lineWidths0{};
    std::array<float, 4> lineWidths1{};
    // Sea level, relief, vertical exaggeration, metres per screen pixel.
    std::array<float, 4> shading{};
    // Unit direction from the surface toward the sun.
    std::array<float, 4> light{};
    // MapLayer in x; remaining values reserved for renderer flags.
    std::array<uint32_t, 4> options{};
    // Page interior, gutter, maximum quadtree level, stored page side.
    std::array<uint32_t, 4> paging{};
  };

  struct LineInstance
  {
    float fromX = 0.0f;
    float fromY = 0.0f;
    float toX = 0.0f;
    float toY = 0.0f;
    uint32_t colour = 0u;
    float widthFactor = 1.0f;
    uint32_t style = 0u;
    uint32_t padding = 0u;
  };

  struct LineRange
  {
    uint32_t firstInstance = 0u;
    uint32_t count = 0u;
  };

  static std::string readBinaryFile(const std::string &path);
  static VkShaderModule createShaderModule(VkDevice device, const std::string &spirv);
  void createDescriptors();
  void createPipeline(VkRenderPass renderPass);
  void destroyMesh();

  WorldMapRHI &rhi_;
  std::string shaderPath_;
  gpu::BufferId vertexBuffer_ = gpu::BufferId::Invalid;
  gpu::BufferId indexBuffer_ = gpu::BufferId::Invalid;
  gpu::BufferId lineInstanceBuffer_ = gpu::BufferId::Invalid;
  gpu::BufferId materialPageTableBuffer_ = gpu::BufferId::Invalid;
  gpu::BufferId materialPageBuffer_ = gpu::BufferId::Invalid;
  gpu::BufferId uniformBuffer_ = gpu::BufferId::Invalid;
  size_t vertexCount_ = 0u;
  size_t indexCount_ = 0u;
  size_t lineInstanceCount_ = 0u;
  size_t materialPageCount_ = 0u;
  size_t materialPageBytes_ = 0u;
  uint32_t materialFieldSide_ = 0u;
  float materialFieldExtentMetres_ = 0.0f;
  uint32_t materialPageSize_ = 0u;
  uint32_t materialPageGutter_ = 0u;
  uint32_t materialPageStoredSide_ = 0u;
  uint32_t materialMaximumPageLevel_ = 0u;
  LineRange cellLines_{};
  LineRange riverLines_{};
  LineRange minorContourLines_{};
  LineRange majorContourLines_{};
  LineRange coastLines_{};
  VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
  VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkPipeline linePipeline_ = VK_NULL_HANDLE;
  VkShaderModule shaderModule_ = VK_NULL_HANDLE;
};

struct MapOverlayStyle
{
  // Off by default now: the cell fills follow noisy boundaries, so the coast is
  // drawn by the land meeting the sea rather than by a line laid over the join.
  bool showCoastline = false;
  bool showRivers = true;
  bool showLabels = true;
  bool showCellEdges = false;
  bool showContours = true;
  bool showWorldBorder = true;
  float coastlineThickness = 1.6f;
  float riverWidthScale = 5.0f;
  float contourThickness = 0.75f;
  float majorContourThickness = 1.35f;
};

// Draws the blended fill, then the overlays, into the current window's draw
// list. `origin` is the top-left of the map panel in screen pixels and `size`
// its extent; the view supplies the world-to-screen mapping.
void drawMapMesh(ImDrawList *drawList, const MapMesh &mesh, const MapView &view, ImVec2 origin, ImVec2 size, const MapShading &shading, const MapOverlayStyle &style, double extentMetres, bool drawFill = true, bool drawGpuLines = false);

// Screen pixel to world metres, the inverse of what drawMapMesh applies. Used
// by picking and by drag-to-pan.
procedural::Point2 screenToWorld(const MapView &view, ImVec2 origin, ImVec2 size, ImVec2 screen);

// Exact per-pixel render of the same field the mesh approximates. Used by the
// headless path, where it is the reference image the phase gate is judged on.
std::vector<uint8_t> renderMapImage(
    const procedural::world::MapGraph &graph,
    const std::vector<procedural::world::CoastPolyline> &coastline,
    const procedural::world::RiverNetwork &rivers,
    const MapMesh &mesh,
    const procedural::world::MapSurfaceField &surface,
    const procedural::world::SiteIndex &index,
    const procedural::world::BlendSettings &settings,
    const MapShading &shading,
    uint32_t width,
    uint32_t height,
    double extentMetres,
    const MapOverlayStyle &style);

bool writePng(const std::string &path, uint32_t width, uint32_t height, const std::vector<uint8_t> &rgba, std::string &error);

} // namespace worldgen
