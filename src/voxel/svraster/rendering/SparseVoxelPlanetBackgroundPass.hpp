#pragma once

#include "os/File.hpp"
#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace voxel::svraster::rendering
{

struct SparseVoxelPlanetBackgroundGrid
{
  uint32_t ringCount = 0u;
  uint32_t segmentCount = 0u;

  bool valid() const
  {
    return ringCount != 0u && segmentCount >= 3u;
  }
};

inline uint32_t sparseVoxelPlanetBackgroundGridStride(
    const SparseVoxelPlanetBackgroundGrid grid, const uint32_t requested)
{
  if (!grid.valid()) return 1u;
  uint32_t result = 1u;
  while (result <= requested / 2u &&
         grid.ringCount % (result * 2u) == 0u &&
         grid.segmentCount % (result * 2u) == 0u)
    result *= 2u;
  return result;
}

inline std::vector<uint32_t> sparseVoxelPlanetBackgroundGridIndices(
    const SparseVoxelPlanetBackgroundGrid grid,
    const uint32_t requestedStride)
{
  if (!grid.valid()) return {};
  const uint32_t stride = sparseVoxelPlanetBackgroundGridStride(
      grid, std::max(requestedStride, 1u));
  const uint32_t submittedRings = grid.ringCount / stride;
  const uint32_t submittedSegments = grid.segmentCount / stride;
  std::vector<uint32_t> result;
  result.reserve(static_cast<size_t>(submittedSegments) * 3u +
      static_cast<size_t>(submittedRings - 1u) * submittedSegments * 6u);
  const auto vertex = [grid](const uint32_t ring, const uint32_t segment)
  {
    return 1u + (ring - 1u) * grid.segmentCount +
        (segment % grid.segmentCount);
  };
  for (uint32_t segment = 0u; segment < grid.segmentCount;
       segment += stride)
  {
    const uint32_t next = (segment + stride) % grid.segmentCount;
    result.insert(result.end(),
        {0u, vertex(stride, segment), vertex(stride, next)});
  }
  for (uint32_t ring = stride; ring < grid.ringCount; ring += stride)
    for (uint32_t segment = 0u; segment < grid.segmentCount;
         segment += stride)
    {
      const uint32_t next = (segment + stride) % grid.segmentCount;
      result.insert(result.end(),
          {vertex(ring, segment), vertex(ring + stride, segment),
           vertex(ring + stride, next), vertex(ring, segment),
           vertex(ring + stride, next), vertex(ring, next)});
    }
  return result;
}

// Returns a conservative prefix of the quadratic polar grid. The planet's
// global minimum elevation defines an opaque inner sphere. A surface point
// beyond the sum of the camera and tallest-peak tangent angles is guaranteed
// to sit behind that sphere, regardless of azimuth or local terrain slope.
inline uint32_t sparseVoxelPlanetBackgroundVisibleRingCount(
    const SparseVoxelPlanetBackgroundGrid grid,
    const uint32_t requestedStride,
    const double visualRadiusMetres,
    const double cameraDistanceFromCentreMetres,
    const double minimumElevationMetres,
    const double maximumElevationMetres,
    const double maximumSurfaceDistanceMetres,
    const double anchorOffsetDistanceMetres,
    const bool horizonCulling)
{
  if (!grid.valid() || !horizonCulling ||
      !(visualRadiusMetres > 0.0) ||
      !(maximumSurfaceDistanceMetres > 0.0))
    return grid.ringCount;
  const double innerRadius = visualRadiusMetres + minimumElevationMetres;
  const double outerRadius = visualRadiusMetres + maximumElevationMetres;
  if (!(innerRadius > 0.0) ||
      !(cameraDistanceFromCentreMetres > innerRadius) ||
      !(outerRadius >= innerRadius))
    return grid.ringCount;
  const auto unitRatio = [](const double numerator, const double denominator)
  {
    return std::clamp(numerator / denominator, -1.0, 1.0);
  };
  const double cameraTangentAngle = std::acos(
      unitRatio(innerRadius, cameraDistanceFromCentreMetres));
  const double peakTangentAngle = std::acos(
      unitRatio(innerRadius, outerRadius));
  const double visibleDistance =
      (cameraTangentAngle + peakTangentAngle) * visualRadiusMetres +
      std::max(anchorOffsetDistanceMetres, 0.0);
  const double fraction = std::sqrt(std::clamp(
      visibleDistance / maximumSurfaceDistanceMetres, 0.0, 1.0));
  const uint32_t stride = sparseVoxelPlanetBackgroundGridStride(
      grid, std::max(requestedStride, 1u));
  // One submitted ring outside the analytic horizon covers triangles crossing
  // the boundary and absorbs floating-point/height interpolation error.
  const uint32_t ring = static_cast<uint32_t>(std::ceil(
      fraction * static_cast<double>(grid.ringCount)));
  return std::min(grid.ringCount,
      ((ring + stride - 1u) / stride + 1u) * stride);
}

inline uint32_t sparseVoxelPlanetBackgroundGridIndexCount(
    const SparseVoxelPlanetBackgroundGrid grid,
    const uint32_t requestedStride,
    const uint32_t visibleRingCount)
{
  if (!grid.valid() || visibleRingCount == 0u) return 0u;
  const uint32_t stride = sparseVoxelPlanetBackgroundGridStride(
      grid, std::max(requestedStride, 1u));
  const uint32_t submittedSegments = grid.segmentCount / stride;
  const uint32_t submittedRings = std::min(
      grid.ringCount / stride,
      (visibleRingCount + stride - 1u) / stride);
  if (submittedRings == 0u) return 0u;
  const uint64_t count = static_cast<uint64_t>(submittedSegments) * 3u +
      static_cast<uint64_t>(submittedRings - 1u) * submittedSegments * 6u;
  return static_cast<uint32_t>(std::min<uint64_t>(
      count, std::numeric_limits<uint32_t>::max()));
}

// Rasterizes the package's textured indexed surface mesh independently from
// the camera-local voxel hierarchy.
class SparseVoxelPlanetBackgroundPass final : public ::rendering::Pass
{
public:
  struct SurfaceMaps
  {
    uint32_t width = 0u;
    uint32_t height = 0u;
    // sRGB top albedo plus linear roughness.
    std::vector<uint8_t> albedoRoughnessRGBA8;
    // sRGB exposed-soil albedo plus a slope-blend enable mask.
    std::vector<uint8_t> soilBlendRGBA8;
    // Material ID, mountain, tectonic uplift, and reserved coverage.
    std::vector<uint8_t> terrainFeaturesRGBA8;

    bool valid() const
    {
      const size_t bytes = static_cast<size_t>(width) * height * 4u;
      return width != 0u && height != 0u &&
          albedoRoughnessRGBA8.size() == bytes &&
          soilBlendRGBA8.size() == bytes &&
          terrainFeaturesRGBA8.size() == bytes;
    }
  };

  struct alignas(16) Vertex
  {
    // Unit planet-space direction and decoded elevation in metres.
    std::array<float, 4> directionElevation{};
    // xy is the continuous unit-disk coordinate of the local polar texture.
    // Its radius is sqrt(surfaceDistance / capDistance), matching the mesh's
    // quadratic ring distribution without an angular seam at the centre.
    std::array<float, 4> surfaceCoordinate{};
  };
  static_assert(sizeof(Vertex) == 32u);

  struct Settings
  {
    uint32_t width = 1u;
    uint32_t height = 1u;
    bool enabled = false;
    // Power-of-two step through the regular polar grid. Two is the production
    // default (one quarter of the source triangles); one is the exact source
    // topology for visual qualification.
    uint32_t subdivisionStride = 2u;
    // Reject only radial rings that are provably hidden behind the planet's
    // minimum-radius sphere. Disable for a controlled full-cap comparison.
    bool horizonCulling = true;
    // Planet fragments are unnecessary where a voxel hit is still at full
    // weight. The fragment shader uses this exact merge boundary and the
    // voxel coverage/depth buffers, retaining holes and every blend pixel.
    float transitionStart = 700.0f;
    bool voxelOwnershipCulling = true;
    // Rows of the planet-space to local tangent-frame rotation. The current
    // surface direction is the Y row; X/Z follow the package cube map.
    std::array<float, 4> tangentX{};
    std::array<float, 4> tangentUp{};
    std::array<float, 4> tangentZ{};
    // xyz is the tangent surface point relative to the camera. w is radius.
    std::array<float, 4> tangentCameraRadius{};
    // x is the package's physical atlas radius. y is the largest visual
    // central angle retained by the camera-region horizon cap.
    std::array<float, 4> sourceRadiusCap{};
    // rgb constant colour; w selects constant (1) or textured biomes (0).
    std::array<float, 4> baseColor{0.13f, 0.32f, 0.16f, 0.0f};
    std::array<float, 4> skyUpIntensity{0.0f, 1.0f, 0.0f, 1.0f};
    std::array<float, 4> skyHorizonEnabled{0.55f, 0.72f, 1.0f, 1.0f};
    // xyz zenith radiance, w the same horizon exponent used by voxel sky rays.
    std::array<float, 4> skyZenith{0.08f, 0.24f, 0.55f, 0.35f};
    std::array<float, 4> skyGround{0.05f, 0.045f, 0.04f, 0.0f};
    std::array<float, 4> directionalDirectionIntensity{0.0f, -1.0f, 0.0f, 1.0f};
    std::array<float, 4> directionalColorEnabled{1.0f, 1.0f, 1.0f, 0.0f};
    // x is the voxel probe's normal-coherence damping strength; y is the
    // finest terrain voxel size in metres. The raster planet uses both to
    // reproduce the band-limited irradiance seen at the representation seam.
    std::array<float, 4> lightingMatch{};
    // Same light-space depth field used by aerial perspective. Values are in
    // kilometres: x enabled, y half extent, z half depth range, w depth bias.
    std::array<float, 4> terrainShadowControl{};
    // x/y sun-elevation fade, z map-edge fade start, w shadow strength.
    std::array<float, 4> terrainShadowFade{0.04f, 0.12f, 0.82f, 1.0f};
    SparseVoxelRasterUniformsGPU raster{};
  };

  SparseVoxelPlanetBackgroundPass(
      const ::rendering::Texture color,
      const ::rendering::Texture normalDepth,
      const ::rendering::Texture depth,
      const ::rendering::Buffer sceneLights,
      const ::rendering::Texture terrainShadow,
      const ::rendering::Buffer coverage,
      const ::rendering::Buffer linearDepth,
      std::vector<Vertex> vertices,
      std::vector<uint32_t> indices,
      SurfaceMaps surfaceMaps,
      Settings settings)
      : color_(color), normalDepth_(normalDepth), depth_(depth),
        sceneLights_(sceneLights), terrainShadow_(terrainShadow),
        coverage_(coverage), linearDepth_(linearDepth),
        vertices_(std::move(vertices)),
        surfaceMaps_(std::move(surfaceMaps)),
        settings_(settings), sourceIndexCount_(indices.size())
  {
    if (!surfaceMaps_.valid())
      throw std::invalid_argument("Planet background surface maps are invalid");
    configureIndices(std::move(indices));
    updateElevationRange();
  }

  ~SparseVoxelPlanetBackgroundPass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteGraphicsPipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (surfaceSampler_.isValid()) renderGraph->deleteSampler(surfaceSampler_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (terrainFeatures_.isValid()) renderGraph->deleteTexture(terrainFeatures_);
    if (soilBlend_.isValid()) renderGraph->deleteTexture(soilBlend_);
    if (albedoRoughness_.isValid()) renderGraph->deleteTexture(albedoRoughness_);
    if (vertexBuffer_.isValid()) renderGraph->deleteBuffer(vertexBuffer_);
    if (indexBuffer_.isValid()) renderGraph->deleteBuffer(indexBuffer_);
    if (drawIndirect_.isValid()) renderGraph->deleteBuffer(drawIndirect_);
    if (uniformBuffer_.isValid()) renderGraph->deleteBuffer(uniformBuffer_);
  }

  void update(const Settings &settings)
  {
    // The index buffer is graph-lifetime storage. A quality change rebuilds
    // the sample graph, so never rewrite that shared topology while older
    // frame slots may still be consuming it.
    const uint32_t topologyStride = settings_.subdivisionStride;
    settings_ = settings;
    settings_.width = std::max(1u, settings_.width);
    settings_.height = std::max(1u, settings_.height);
    settings_.subdivisionStride = topologyStride;
    if (uniformBuffer_.isValid()) upload();
  }

  // Destination caps use a fixed topology, so teleport publication can
  // replace their attributes without reallocating graph-owned resources.
  bool updateMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices,
                  SurfaceMaps surfaceMaps)
  {
    if (vertices.size() != vertices_.size() ||
        indices.size() != sourceIndexCount_ || !surfaceMaps.valid() ||
        surfaceMaps.width != surfaceMaps_.width ||
        surfaceMaps.height != surfaceMaps_.height)
      return false;
    vertices_ = std::move(vertices);
    surfaceMaps_ = std::move(surfaceMaps);
    configureIndices(std::move(indices));
    updateElevationRange();
    if (vertexBuffer_.isValid() && !vertices_.empty())
      renderGraph->bufferWrite(
          vertexBuffer_, 0u, vertices_.size() * sizeof(Vertex),
          vertices_.data());
    if (indexBuffer_.isValid() && !indices_.empty())
      renderGraph->bufferWrite(
          indexBuffer_, 0u, indices_.size() * sizeof(uint32_t),
          indices_.data());
    if (albedoRoughness_.isValid())
      uploadSurfaceMaps();
    return true;
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    upload();

    ::rendering::ColorAttachmentInfo color{};
    color.name = passName + "_Color";
    color.view = colorView(color_);
    // Alpha zero distinguishes clear sky from a triangle hit in composition.
    color.clearValue = ::rendering::Color::rgba(0.0f, 0.0f, 0.0f, 0.0f);
    ::rendering::ColorAttachmentInfo normalDepth{};
    normalDepth.name = passName + "_NormalDepth";
    normalDepth.view = colorView(normalDepth_);
    normalDepth.clearValue =
        ::rendering::Color::rgba(0.0f, 1.0f, 0.0f, 0.0f);

    ::rendering::RenderPassInfo renderPass{};
    renderPass.name = passName + "_RenderPass";
    renderPass.scissor = ::rendering::Rect2D(0, 0, settings_.width, settings_.height);
    renderPass.viewport = ::rendering::Viewport(settings_.width, settings_.height);
    renderPass.colorAttachments = {color, normalDepth};
    renderPass.depthStencilAttachment = {
      .name = passName + "_Depth",
      .view = depthView(depth_),
      .clearDepth = 0.0f,
      .clearStencil = 0u,
      .enabled = true,
    };

    commandBuffer.cmdBindGraphicsPipeline(pipeline_);
    commandBuffer.cmdBeginRenderPass(renderPass);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    // Render-graph commands are cached. Keep the indirect draw recorded and
    // gate its current frame slot with a zero index count from upload().
    if (!vertices_.empty() && !indices_.empty())
    {
      commandBuffer.cmdBindVertexBuffer(
          0u,
          {.buffer = vertexBuffer_, .offset = 0u,
           .size = vertices_.size() * sizeof(Vertex),
           .access = ::rendering::AccessPattern::VERTEX_ATTRIBUTE_READ});
      commandBuffer.cmdBindIndexBuffer(
          {.buffer = indexBuffer_, .offset = 0u,
           .size = indices_.size() * sizeof(uint32_t),
           .access = ::rendering::AccessPattern::INDEX_READ},
          ::rendering::Type_Uint32);
      commandBuffer.cmdDrawIndexedIndirect(
          {.buffer = drawIndirect_, .offset = 0u,
           .size = sizeof(DrawIndexedIndirectGPU),
           .access = ::rendering::AccessPattern::INDIRECT_COMMAND_READ},
          0u, 1u, sizeof(DrawIndexedIndirectGPU));
    }
    commandBuffer.cmdEndRenderPass();
  }

private:
  struct DrawIndexedIndirectGPU
  {
    uint32_t indexCount = 0u;
    uint32_t instanceCount = 1u;
    uint32_t firstIndex = 0u;
    int32_t vertexOffset = 0;
    uint32_t firstInstance = 0u;
  };
  static_assert(sizeof(DrawIndexedIndirectGPU) == 20u);

  struct alignas(16) UniformsGPU
  {
    std::array<float, 4> tangentX{};
    std::array<float, 4> tangentUp{};
    std::array<float, 4> tangentZ{};
    std::array<float, 4> tangentCameraRadius{};
    std::array<float, 4> sourceRadiusCap{};
    std::array<float, 4> focalViewport{};
    std::array<float, 4> principalViewport{};
    std::array<float, 4> baseColor{};
    std::array<float, 4> skyUpIntensity{};
    std::array<float, 4> skyHorizonEnabled{};
    std::array<float, 4> skyZenith{};
    std::array<float, 4> skyGround{};
    std::array<float, 4> directionalDirectionIntensity{};
    std::array<float, 4> directionalColorEnabled{};
    std::array<float, 4> lightingMatch{};
    std::array<float, 4> terrainShadowControl{};
    std::array<float, 4> terrainShadowFade{};
    // xy texture dimensions; z/w their reciprocals.
    std::array<float, 4> surfaceMapInfo{};
    std::array<float, 16> worldToCamera{};
    std::array<float, 16> cameraToWorld{};
  };
  static_assert(sizeof(UniformsGPU) == 416u);

  static ::rendering::TextureView colorView(const ::rendering::Texture texture)
  {
    return {.texture = texture,
            .access = ::rendering::AccessPattern::COLOR_ATTACHMENT_WRITE,
            .layout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
            .index = 0u, .flags = ::rendering::ImageAspectFlags::Color,
            .baseArrayLayer = 0u, .baseMipLevel = 0u,
            .layerCount = 1u, .levelCount = 1u};
  }

  static ::rendering::TextureView depthView(const ::rendering::Texture texture)
  {
    return {.texture = texture,
            .access = ::rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ |
                ::rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE,
            .layout = ::rendering::ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
            .index = 0u, .flags = ::rendering::ImageAspectFlags::Depth,
            .baseArrayLayer = 0u, .baseMipLevel = 0u,
            .layerCount = 1u, .levelCount = 1u};
  }

  void upload()
  {
    UniformsGPU uniforms{};
    uniforms.tangentX = settings_.tangentX;
    uniforms.tangentUp = settings_.tangentUp;
    uniforms.tangentZ = settings_.tangentZ;
    uniforms.tangentCameraRadius = settings_.tangentCameraRadius;
    uniforms.sourceRadiusCap = settings_.sourceRadiusCap;
    uniforms.focalViewport = {
      settings_.raster.focalPrincipal[0], settings_.raster.focalPrincipal[1],
      static_cast<float>(settings_.width), static_cast<float>(settings_.height)};
    uniforms.principalViewport = {
      settings_.raster.focalPrincipal[2], settings_.raster.focalPrincipal[3],
      settings_.voxelOwnershipCulling
          ? std::max(settings_.transitionStart, 0.0f)
          : -1.0f,
      settings_.raster.cameraWorldPositionNear[3]};
    uniforms.baseColor = settings_.baseColor;
    uniforms.skyUpIntensity = settings_.skyUpIntensity;
    uniforms.skyHorizonEnabled = settings_.skyHorizonEnabled;
    uniforms.skyZenith = settings_.skyZenith;
    uniforms.skyGround = settings_.skyGround;
    uniforms.directionalDirectionIntensity =
        settings_.directionalDirectionIntensity;
    uniforms.directionalColorEnabled = settings_.directionalColorEnabled;
    uniforms.lightingMatch = settings_.lightingMatch;
    uniforms.terrainShadowControl = settings_.terrainShadowControl;
    uniforms.terrainShadowFade = settings_.terrainShadowFade;
    uniforms.surfaceMapInfo = {
      static_cast<float>(surfaceMaps_.width),
      static_cast<float>(surfaceMaps_.height),
      1.0f / static_cast<float>(surfaceMaps_.width),
      1.0f / static_cast<float>(surfaceMaps_.height),
    };
    uniforms.worldToCamera = settings_.raster.worldToCamera;
    uniforms.cameraToWorld = settings_.raster.cameraToWorld;
    renderGraph->bufferWrite(uniformBuffer_, 0u, sizeof(uniforms), &uniforms);
    DrawIndexedIndirectGPU draw{};
    draw.indexCount = settings_.enabled ? visibleIndexCount() : 0u;
    renderGraph->bufferWrite(
        drawIndirect_, 0u, sizeof(draw), &draw);
  }

  void ensureResources()
  {
    if (layout_.isValid()) return;
    const auto sampledTexture = [&](const char *suffix,
                                    const ::rendering::Format format)
    {
      return renderGraph->createTexture({
        .name = passName + suffix,
        .format = format,
        .height = surfaceMaps_.height,
        .width = surfaceMaps_.width,
        .memoryProperties = ::rendering::BufferUsage::BufferUsage_None,
        .mipLevels = 1u,
        .usage = ::rendering::ImageUsage::ImageUsage_Sampled |
            ::rendering::ImageUsage::ImageUsage_TransferDst,
      });
    };
    albedoRoughness_ = sampledTexture(
        "_AlbedoRoughness", ::rendering::Format::Format_RGBA8UnormSrgb);
    soilBlend_ = sampledTexture(
        "_SoilBlend", ::rendering::Format::Format_RGBA8UnormSrgb);
    terrainFeatures_ = sampledTexture(
        "_TerrainFeatures", ::rendering::Format::Format_RGBA8Unorm);
    uploadSurfaceMaps();
    uniformBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Uniforms", .size = sizeof(UniformsGPU),
         .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
             ::rendering::BufferUsage::BufferUsage_Push,
         .frameLocal = true});
    drawIndirect_ = renderGraph->createBuffer(
        {.name = passName + "_DrawIndirect",
         .size = sizeof(DrawIndexedIndirectGPU),
         .usage = ::rendering::BufferUsage::BufferUsage_Indirect |
             ::rendering::BufferUsage::BufferUsage_Push,
         .frameLocal = true});
    vertexBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Vertices",
         .size = std::max<uint64_t>(sizeof(Vertex), vertices_.size() * sizeof(Vertex)),
         .usage = ::rendering::BufferUsage::BufferUsage_Vertex |
             ::rendering::BufferUsage::BufferUsage_Push});
    if (!vertices_.empty())
      renderGraph->bufferWrite(
          vertexBuffer_, 0u, vertices_.size() * sizeof(Vertex), vertices_.data());
    indexBuffer_ = renderGraph->createBuffer(
        {.name = passName + "_Indices",
         .size = std::max<uint64_t>(sizeof(uint32_t), sourceIndexCount_ *
             sizeof(uint32_t)),
         .usage = ::rendering::BufferUsage::BufferUsage_Index |
             ::rendering::BufferUsage::BufferUsage_Push});
    if (!indices_.empty())
      renderGraph->bufferWrite(
          indexBuffer_, 0u, indices_.size() * sizeof(uint32_t),
          indices_.data());

    layout_ = renderGraph->createBindingsLayout(
        {.name = passName + "_Layout",
         .groups = {::rendering::BindingGroupLayout{
           .buffers = {
             {.name = "uniforms", .binding = 0u, .isDynamic = false,
              .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
              .visibility =
                  ::rendering::BindingVisibility::BindingVisibility_Vertex |
                  ::rendering::BindingVisibility::BindingVisibility_Fragment},
             {.name = "sceneLights", .binding = 5u, .isDynamic = false,
              .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
             {.name = "coverage", .binding = 7u, .isDynamic = false,
              .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
             {.name = "linearDepth", .binding = 8u, .isDynamic = false,
              .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
           },
           .samplers = {
             {.name = "surfaceSampler", .binding = 4u,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
           },
           .textures = {
             {.name = "surfaceAlbedoRoughness", .binding = 1u,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
             {.name = "surfaceSoilBlend", .binding = 2u,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
             {.name = "surfaceTerrainFeatures", .binding = 3u,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
             {.name = "terrainShadowMap", .binding = 6u,
              .visibility = ::rendering::BindingVisibility::BindingVisibility_Fragment},
           },
         }}});
    surfaceSampler_ = renderGraph->createSampler({
      .name = passName + "_SurfaceSampler",
      .minFilter = ::rendering::Filter::Linear,
      .magFilter = ::rendering::Filter::Linear,
      .addressModeU = ::rendering::SamplerAddressMode::Repeat,
      .addressModeV = ::rendering::SamplerAddressMode::ClampToEdge,
      .addressModeW = ::rendering::SamplerAddressMode::ClampToEdge,
      .anisotropyEnable = false,
      .maxAnisotropy = 1.0f,
      .maxLod = 0.0f,
    });
    const auto sampled = [](const ::rendering::Texture texture)
    {
      return ::rendering::TextureView{
        .texture = texture,
        .access = ::rendering::AccessPattern::SHADER_READ,
        .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0u,
        .flags = ::rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
      };
    };
    const auto sampledDepth = [](const ::rendering::Texture texture)
    {
      return ::rendering::TextureView{
        .texture = texture,
        .access = ::rendering::AccessPattern::SHADER_READ,
        .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0u,
        .flags = ::rendering::ImageAspectFlags::Depth,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
      };
    };
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_, .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u, .bufferView = {.buffer = uniformBuffer_, .offset = 0u,
               .size = sizeof(UniformsGPU),
               .access = ::rendering::AccessPattern::UNIFORM_READ}},
             {.binding = 5u, .bufferView = {.buffer = sceneLights_, .offset = 0u,
               .size = globalillumination::worldgi::worldGILightBufferSize(8u),
               .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 7u, .bufferView = {.buffer = coverage_, .offset = 0u,
               .size = static_cast<uint64_t>(settings_.width) *
                   settings_.height * sizeof(uint32_t),
               .access = ::rendering::AccessPattern::SHADER_READ}},
             {.binding = 8u, .bufferView = {.buffer = linearDepth_, .offset = 0u,
               .size = static_cast<uint64_t>(settings_.width) *
                   settings_.height * sizeof(float),
               .access = ::rendering::AccessPattern::SHADER_READ}},
           },
           .samplers = {
             {.binding = 4u, .sampler = surfaceSampler_,
              .view = sampled(albedoRoughness_)},
           },
           .textures = {
             {.binding = 1u, .textureView = sampled(albedoRoughness_)},
             {.binding = 2u, .textureView = sampled(soilBlend_)},
             {.binding = 3u, .textureView = sampled(terrainFeatures_)},
             {.binding = 6u, .textureView = sampledDepth(terrainShadow_)},
           },
         }}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader", .layout = layout_,
         .src = os::io::readRelativeFile(
             "assets/shaders/spirv/svraster/sparse-voxel-planet-background.spirv"),
         .type = ::rendering::ShaderType::SpirV});

    ::rendering::DepthAttatchment depth{};
    depth.enabled = true;
    depth.comparison = ::rendering::ComparisonOp::ComparisonOp_GreaterOrEqual;
    depth.format = ::rendering::Format::Format_Depth32Float;
    depth.loadOp = ::rendering::LoadOp::LoadOp_Clear;
    depth.storeOp = ::rendering::StoreOp::StoreOp_Store;
    ::rendering::GraphicsPipelineInfo pipeline{};
    pipeline.name = passName + "_Pipeline";
    pipeline.layout = layout_;
    pipeline.vertexStage.vertexShader = shader_;
    pipeline.vertexStage.shaderEntry = "drawPlanetVertex";
    pipeline.vertexStage.vertexLayoutElements = {
      {.name = "directionElevation", .type = ::rendering::Type_Float32x4,
       .binding = 0u, .offset = 0u, .location = 0u},
      {.name = "surfaceCoordinate", .type = ::rendering::Type_Float32x4,
       .binding = 0u, .offset = 16u, .location = 1u},
    };
    // The CPU submits a dense camera-region cap. Clip-space frustum/depth
    // clipping trims it to the current view and the shader rejects its outer
    // guard ring. Cube-face tangent frames may change handedness, so a
    // winding-dependent cull is deliberately invalid here.
    pipeline.vertexStage.cullType = ::rendering::CullMode::None;
    pipeline.vertexStage.winding = ::rendering::WindingOrder::CCW;
    pipeline.vertexStage.primitiveType = ::rendering::PrimitiveType_Triangles;
    pipeline.fragmentStage.fragmentShader = shader_;
    pipeline.fragmentStage.shaderEntry = "drawPlanetFragment";
    pipeline.fragmentStage.colorAttatchments = {{
      .format = ::rendering::Format::Format_RGBA16Float,
      .loadOp = ::rendering::LoadOp::LoadOp_Clear,
      .storeOp = ::rendering::StoreOp::StoreOp_Store,
      .blendMode = ::rendering::ColorBlendMode::Replace,
      .initialLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
      .finalLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
    }, {
      .format = ::rendering::Format::Format_RGBA32Float,
      .loadOp = ::rendering::LoadOp::LoadOp_Clear,
      .storeOp = ::rendering::StoreOp::StoreOp_Store,
      .blendMode = ::rendering::ColorBlendMode::Replace,
      .initialLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
      .finalLayout = ::rendering::ResourceLayout::COLOR_ATTACHMENT,
    }};
    pipeline.fragmentStage.depthAttatchment = depth;
    pipeline_ = renderGraph->createGraphicsPipeline(pipeline);
  }

  ::rendering::Texture color_{};
  ::rendering::Texture normalDepth_{};
  ::rendering::Texture depth_{};
  ::rendering::Buffer sceneLights_{};
  ::rendering::Texture terrainShadow_{};
  ::rendering::Buffer coverage_{};
  ::rendering::Buffer linearDepth_{};
  std::vector<Vertex> vertices_;
  std::vector<uint32_t> indices_;
  SurfaceMaps surfaceMaps_{};
  Settings settings_{};
  SparseVoxelPlanetBackgroundGrid grid_{};
  size_t sourceIndexCount_ = 0u;
  float minimumElevation_ = 0.0f;
  float maximumElevation_ = 0.0f;
  ::rendering::Buffer uniformBuffer_{};
  ::rendering::Buffer drawIndirect_{};
  ::rendering::Buffer vertexBuffer_{};
  ::rendering::Buffer indexBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Texture albedoRoughness_{};
  ::rendering::Texture soilBlend_{};
  ::rendering::Texture terrainFeatures_{};
  ::rendering::Sampler surfaceSampler_{};
  ::rendering::Shader shader_{};
  ::rendering::GraphicsPipeline pipeline_{};

  void configureIndices(std::vector<uint32_t> sourceIndices)
  {
    grid_ = {};
    if (surfaceMaps_.height > 1u)
    {
      const SparseVoxelPlanetBackgroundGrid candidate{
        surfaceMaps_.height - 1u, surfaceMaps_.width};
      const size_t expectedVertices = 1u +
          static_cast<size_t>(candidate.ringCount) * candidate.segmentCount;
      const size_t expectedIndices =
          static_cast<size_t>(candidate.segmentCount) * 3u +
          static_cast<size_t>(candidate.ringCount - 1u) *
              candidate.segmentCount * 6u;
      if (candidate.valid() && vertices_.size() == expectedVertices &&
          sourceIndices.size() == expectedIndices)
        grid_ = candidate;
    }
    settings_.subdivisionStride = sparseVoxelPlanetBackgroundGridStride(
        grid_, std::max(settings_.subdivisionStride, 1u));
    if (grid_.valid())
      rebuildGridIndices();
    else
      indices_ = std::move(sourceIndices);
  }

  void rebuildGridIndices()
  {
    indices_ = sparseVoxelPlanetBackgroundGridIndices(
        grid_, settings_.subdivisionStride);
    if (indexBuffer_.isValid() && !indices_.empty())
      renderGraph->bufferWrite(indexBuffer_, 0u,
          indices_.size() * sizeof(uint32_t), indices_.data());
  }

  void updateElevationRange()
  {
    if (vertices_.empty())
    {
      minimumElevation_ = 0.0f;
      maximumElevation_ = 0.0f;
      return;
    }
    minimumElevation_ = vertices_.front().directionElevation[3];
    maximumElevation_ = minimumElevation_;
    for (const Vertex &vertex : vertices_)
    {
      minimumElevation_ = std::min(
          minimumElevation_, vertex.directionElevation[3]);
      maximumElevation_ = std::max(
          maximumElevation_, vertex.directionElevation[3]);
    }
  }

  uint32_t visibleIndexCount() const
  {
    if (!grid_.valid())
      return static_cast<uint32_t>(std::min<size_t>(
          indices_.size(), std::numeric_limits<uint32_t>::max()));
    const std::array<float, 3> centreDirection{
      vertices_.front().directionElevation[0],
      vertices_.front().directionElevation[1],
      vertices_.front().directionElevation[2]};
    const double anchorCosine = std::clamp(
        static_cast<double>(centreDirection[0]) * settings_.tangentUp[0] +
            static_cast<double>(centreDirection[1]) * settings_.tangentUp[1] +
            static_cast<double>(centreDirection[2]) * settings_.tangentUp[2],
        -1.0, 1.0);
    const double sourceRadius = std::max<double>(
        settings_.sourceRadiusCap[0], 1.0);
    const double anchorDistance = std::acos(anchorCosine) * sourceRadius;
    const double visualRadius = std::max<double>(
        settings_.tangentCameraRadius[3], 1.0);
    const double centreX = settings_.tangentCameraRadius[0];
    const double centreY = settings_.tangentCameraRadius[1] - visualRadius;
    const double centreZ = settings_.tangentCameraRadius[2];
    const double cameraDistance = std::sqrt(
        centreX * centreX + centreY * centreY + centreZ * centreZ);
    const uint32_t visibleRings =
        sparseVoxelPlanetBackgroundVisibleRingCount(
            grid_, settings_.subdivisionStride, visualRadius, cameraDistance,
            minimumElevation_, maximumElevation_,
            settings_.sourceRadiusCap[2], anchorDistance,
            settings_.horizonCulling);
    return std::min<uint32_t>(
        sparseVoxelPlanetBackgroundGridIndexCount(
            grid_, settings_.subdivisionStride, visibleRings),
        static_cast<uint32_t>(std::min<size_t>(
            indices_.size(), std::numeric_limits<uint32_t>::max())));
  }

  void uploadSurfaceMaps()
  {
    renderGraph->textureWrite(albedoRoughness_,
        surfaceMaps_.albedoRoughnessRGBA8.size(),
        surfaceMaps_.albedoRoughnessRGBA8.data());
    renderGraph->textureWrite(soilBlend_, surfaceMaps_.soilBlendRGBA8.size(),
        surfaceMaps_.soilBlendRGBA8.data());
    renderGraph->textureWrite(terrainFeatures_,
        surfaceMaps_.terrainFeaturesRGBA8.size(),
        surfaceMaps_.terrainFeaturesRGBA8.data());
  }
};

} // namespace voxel::svraster::rendering
