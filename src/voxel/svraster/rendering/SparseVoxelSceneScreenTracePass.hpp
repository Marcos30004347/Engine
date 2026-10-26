#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster::rendering
{

// Near-field screen-space visibility for scene rays.
//
// This pass answers only the first few metres in front of a receiver. The
// Tree64 leg still traces every ray over its full range from its own
// conservatively biased origin, so a hit here is an addition to that answer and
// a miss costs nothing. The division exists because the world tracer's origin
// is pushed out of the containing *trace* cell - the coarsened clipmap cell,
// metres across on terrain - and everything inside that displacement is
// invisible to it. That is where contact shadows, voxel-face self-shadowing and
// ambient occlusion live, and the depth buffer already holds that geometry at
// pixel resolution.
//
// The pass builds its own min/max depth hierarchy. The renderer's ordinary HZB
// keeps only the farthest reverse-Z depth, which is enough for raster culling
// but cannot conservatively decide whether a ray overlaps any surface in a
// cell. Leaves are tested as planes reconstructed from depth and the geometric
// normal; a small depth slab is retained only at discontinuities and for rays
// parallel to the leaf plane. This is the representation described by Widmer
// et al., "An Adaptive Acceleration Structure for Screen-space Ray Tracing".
class SparseVoxelSceneScreenTracePass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer rays;
    ::rendering::Buffer hits;
    ::rendering::Buffer activeRayIndices;
    ::rendering::Buffer activeRayState;
    ::rendering::Buffer statistics;
  };
  struct Settings
  {
    uint32_t width = 1u, height = 1u;
    uint32_t rayCount = 1u, maximumSteps = 64u;
    bool reverseZ = true;
    bool enabled = true;
    bool collectStatistics = false;
    // How far past a surface the leaf test may still call an occluder solid,
    // measured in cell traversal lengths. The depth buffer records a front
    // face and says nothing about how deep it goes, so without an upper bound
    // every distant silhouette casts a halo over everything in front of it.
    //
    // One traversal length, not two: the interval is what a grazing ray spans
    // inside a single cell, which is already far more world distance than the
    // surface is thick, and doubling it produced false direct shadows the
    // world trace does not agree with. `maximumThickness` is the real bound.
    float thicknessSteps = 1.0f;
    // Near-field cap in world units. This must NOT be the occupancy volume
    // diagonal: a screen-space trace is reliable only over the current visible
    // layer, and the world-space Tree64 tracer owns the rest of the ray.
    float maximumDistance = 4.0f;
    // Where traversal starts, in world-pixel widths at the receiver's depth.
    // Enough to clear the receiver's own texel and no more.
    float startPixels = 2.0f;
    // Smallest world-space thickness accepted as an occluder. Pixel-sized
    // thickness alone collapses below one voxel as resolution or view depth
    // changes. The caller supplies the receiver trace-cell size here so a
    // voxel-scale silhouette still has a meaningful finite slab.
    float minimumThickness = 0.0f;
    // Largest world-space thickness a single depth-buffer surface may claim.
    // A slab deeper than the geometry actually is invents occluders behind
    // every silhouette; those false hits win over the world trace in the merge
    // and are the near-field shadows that flicker as an occluder leaves the
    // screen. Zero leaves the slab unbounded, which is the old behaviour.
    float maximumThickness = 0.0f;
    // viewDepth = depthLinearB / (deviceDepth + depthLinearA). Reverse Z with
    // an infinite far plane makes this near / deviceDepth.
    float depthLinearA = 0.0f;
    float depthLinearB = 1.0f;
    SparseVoxelRasterUniformsGPU raster{};
  };

  SparseVoxelSceneScreenTracePass(
      ::rendering::Texture depth, ::rendering::Texture geometryNormal,
      ::rendering::Texture instanceId, ::rendering::Texture voxelId,
      Buffers buffers, Settings settings)
      : depth_(depth), geometryNormal_(geometryNormal),
        instanceId_(instanceId), voxelId_(voxelId), buffers_(buffers),
        settings_(sanitize(settings)) {}

  void update(
      const SparseVoxelRasterUniformsGPU &raster, const bool enabled)
  {
    settings_.raster = raster;
    settings_.enabled = enabled;
    // The near plane travels in the raster uniform, so the depth
    // linearization tracks it without a second update path.
    if (settings_.reverseZ)
    {
      settings_.depthLinearA = 0.0f;
      settings_.depthLinearB = std::max(raster.cameraWorldPositionNear[3], 1.0e-6f);
    }
    upload();
  }
  ~SparseVoxelSceneScreenTracePass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (depthBoundsInitializePipeline_.isValid())
      renderGraph->deleteComputePipeline(depthBoundsInitializePipeline_);
    if (depthBoundsReducePipeline_.isValid())
      renderGraph->deleteComputePipeline(depthBoundsReducePipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    for (const auto &groups : depthBoundsReduceGroups_)
      if (groups.isValid()) renderGraph->deleteBindingGroups(groups);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (uniforms_.isValid()) renderGraph->deleteBuffer(uniforms_);
    if (rasterBuffer_.isValid()) renderGraph->deleteBuffer(rasterBuffer_);
    if (depthBounds_.isValid()) renderGraph->deleteBuffer(depthBounds_);
    for (const auto &uniforms : depthBoundsBuildUniforms_)
      if (uniforms.isValid()) renderGraph->deleteBuffer(uniforms);
    if (ownedActiveRayIndices_.isValid())
      renderGraph->deleteBuffer(ownedActiveRayIndices_);
    if (ownedActiveRayState_.isValid())
      renderGraph->deleteBuffer(ownedActiveRayState_);
    if (ownedStatistics_.isValid())
      renderGraph->deleteBuffer(ownedStatistics_);
  }
  void recordCommandBuffer(::rendering::CommandRecorder &cmd) override
  {
    ensure(); upload();
    // Each reduction consumes the level written immediately before it.
    cmd.cmdBindComputePipeline(depthBoundsInitializePipeline_);
    cmd.cmdBindBindingGroups(groups_, nullptr, 0u);
    cmd.cmdDispatch((settings_.width + 7u) / 8u,
                    (settings_.height + 7u) / 8u, 1u);
    cmd.cmdMemoryBarrier();
    uint32_t mipWidth = settings_.width;
    uint32_t mipHeight = settings_.height;
    for (uint32_t mip = 1u; mip < depthBoundsBuildUniforms_.size(); ++mip)
    {
      mipWidth = (mipWidth + 1u) / 2u;
      mipHeight = (mipHeight + 1u) / 2u;
      cmd.cmdBindComputePipeline(depthBoundsReducePipeline_);
      cmd.cmdBindBindingGroups(depthBoundsReduceGroups_[mip - 1u], nullptr, 0u);
      cmd.cmdDispatch((mipWidth + 7u) / 8u, (mipHeight + 7u) / 8u, 1u);
      cmd.cmdMemoryBarrier();
    }
    cmd.cmdBindComputePipeline(pipeline_);
    cmd.cmdBindBindingGroups(groups_, nullptr, 0u);
    cmd.cmdDispatchIndirect(buffers_.activeRayState, 0u);
  }

private:
  struct alignas(16) Uniforms
  {
    std::array<uint32_t, 4> viewport{};
    std::array<uint32_t, 4> controls{};
    std::array<float, 4> traceParams{};
    std::array<float, 4> depthConversion{};
  };
  static_assert(sizeof(Uniforms) == 64u);
  struct alignas(16) DepthBoundsBuildUniforms
  {
    uint32_t destinationLevel = 0u;
    std::array<uint32_t, 3> padding{};
  };
  static_assert(sizeof(DepthBoundsBuildUniforms) == 16u);
  static constexpr uint32_t kMaximumDepthBoundsMipCount = 16u;

  static uint32_t mipCount(uint32_t width, uint32_t height)
  {
    uint32_t result = 1u;
    while ((width > 1u || height > 1u) &&
           result < kMaximumDepthBoundsMipCount)
    {
      width = (width + 1u) / 2u;
      height = (height + 1u) / 2u;
      ++result;
    }
    return result;
  }
  static uint64_t depthBoundsElementCount(uint32_t width, uint32_t height)
  {
    uint64_t result = 0u;
    for (uint32_t mip = 0u; mip < mipCount(width, height); ++mip)
    {
      result += static_cast<uint64_t>(width) * height;
      width = (width + 1u) / 2u;
      height = (height + 1u) / 2u;
    }
    return result;
  }
  static Settings sanitize(Settings s)
  {
    s.width = std::max(1u, s.width); s.height = std::max(1u, s.height);
    s.rayCount = std::max(1u, s.rayCount);
    s.maximumSteps = std::clamp(s.maximumSteps, 1u, 256u);
    s.thicknessSteps = std::max(s.thicknessSteps, 0.0f);
    s.maximumDistance = std::max(s.maximumDistance, 1.0e-4f);
    s.startPixels = std::max(s.startPixels, 1.0f);
    s.minimumThickness = std::max(s.minimumThickness, 0.0f);
    s.maximumThickness = std::max(s.maximumThickness, 0.0f);
    s.depthLinearB = std::max(s.depthLinearB, 1.0e-6f);
    return s;
  }
  static ::rendering::BufferView bufferView(::rendering::Buffer b, uint64_t size, ::rendering::AccessPattern access)
  { return {.buffer=b,.offset=0u,.size=std::max<uint64_t>(size, 4u),.access=access}; }
  static ::rendering::TextureView depthView(::rendering::Texture t)
  { return {.texture=t,.access=::rendering::AccessPattern::SHADER_READ,.layout=::rendering::ResourceLayout::SHADER_READ_ONLY,.index=0u,.flags=::rendering::ImageAspectFlags::Depth,.baseArrayLayer=0u,.baseMipLevel=0u,.layerCount=1u,.levelCount=1u}; }
  void upload()
  {
    if (!uniforms_.isValid()) return;
    const Uniforms u{
      {settings_.width,settings_.height,settings_.rayCount,settings_.maximumSteps},
      {mipCount(settings_.width,settings_.height),
       settings_.collectStatistics ? 1u : 0u,0u,
       (settings_.reverseZ ? 1u : 0u)},
      {settings_.thicknessSteps,settings_.maximumDistance,
       settings_.enabled ? 1.0f : 0.0f,settings_.startPixels},
      {settings_.depthLinearA,settings_.depthLinearB,
       settings_.minimumThickness,settings_.maximumThickness}};
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(u), const_cast<Uniforms *>(&u));
    renderGraph->bufferWrite(
        rasterBuffer_, 0u, sizeof(settings_.raster), &settings_.raster);
  }
  void ensure()
  {
    if (layout_.isValid()) return;
    const uint64_t count=settings_.rayCount;
    if (!buffers_.activeRayIndices.isValid())
    {
      ownedActiveRayIndices_=renderGraph->createBuffer({
        .name=passName+"_FallbackActiveRayIndices",
        .size=std::max<uint64_t>(count*sizeof(uint32_t),4u),
        .usage=::rendering::BufferUsage::BufferUsage_Storage,
        .frameLocal=true});
      buffers_.activeRayIndices=ownedActiveRayIndices_;
    }
    if (!buffers_.activeRayState.isValid())
    {
      ownedActiveRayState_=renderGraph->createBuffer({
        .name=passName+"_FallbackActiveRayState",
        .size=16u,
        .usage=::rendering::BufferUsage::BufferUsage_Storage |
               ::rendering::BufferUsage::BufferUsage_Push,
        .frameLocal=true});
      const std::array<uint32_t,4> disabled{0u,1u,1u,0u};
      renderGraph->bufferWrite(
          ownedActiveRayState_,0u,sizeof(disabled),
          const_cast<uint32_t *>(disabled.data()));
      buffers_.activeRayState=ownedActiveRayState_;
    }
    if (!buffers_.statistics.isValid())
    {
      ownedStatistics_=renderGraph->createBuffer({
        .name=passName+"_FallbackStatistics",
        .size=sizeof(SparseVoxelScreenTraceStatisticsGPU),
        .usage=::rendering::BufferUsage::BufferUsage_Storage,
        .frameLocal=true});
      buffers_.statistics=ownedStatistics_;
    }
    uniforms_ = renderGraph->createBuffer({.name=passName+"_Uniforms",.size=sizeof(Uniforms),.usage=::rendering::BufferUsage::BufferUsage_Uniform|::rendering::BufferUsage::BufferUsage_Push,.frameLocal=true});
    rasterBuffer_ = renderGraph->createBuffer({.name=passName+"_Raster",.size=sizeof(settings_.raster),.usage=::rendering::BufferUsage::BufferUsage_Uniform|::rendering::BufferUsage::BufferUsage_Push,.frameLocal=true});
    const uint64_t depthBoundsBytes =
        depthBoundsElementCount(settings_.width, settings_.height) *
        sizeof(float) * 2u;
    depthBounds_ = renderGraph->createBuffer({
      .name=passName+"_DepthBounds",
      .size=std::max<uint64_t>(depthBoundsBytes, 8u),
      .usage=::rendering::BufferUsage::BufferUsage_Storage,
      .frameLocal=true});
    const uint32_t levels = mipCount(settings_.width, settings_.height);
    depthBoundsBuildUniforms_.reserve(levels);
    for (uint32_t mip = 0u; mip < levels; ++mip)
    {
      const auto buildUniforms = renderGraph->createBuffer({
        .name=passName+"_DepthBoundsBuildUniforms"+std::to_string(mip),
        .size=sizeof(DepthBoundsBuildUniforms),
        .usage=::rendering::BufferUsage::BufferUsage_Uniform |
               ::rendering::BufferUsage::BufferUsage_Push,
        .frameLocal=false});
      const DepthBoundsBuildUniforms value{mip, {0u, 0u, 0u}};
      renderGraph->bufferWrite(
          buildUniforms, 0u, sizeof(value),
          const_cast<DepthBoundsBuildUniforms *>(&value));
      depthBoundsBuildUniforms_.push_back(buildUniforms);
    }
    layout_ = renderGraph->createBindingsLayout({.name=passName+"_Layout",.groups={::rendering::BindingGroupLayout{.buffers={
      {.name="settings",.binding=0u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_UniformBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},
      {.name="rays",.binding=3u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},
      {.name="hits",.binding=4u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},
      {.name="activeRayIndices",.binding=5u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},
      {.name="activeRayState",.binding=6u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},
      {.name="raster",.binding=7u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_UniformBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},
      {.name="depthBounds",.binding=8u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},
      {.name="depthBoundsBuildSettings",.binding=9u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_UniformBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},
      {.name="statistics",.binding=10u,.isDynamic=false,.type=::rendering::BufferBindingType::BufferBindingType_StorageBuffer,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},},.textures={
      {.name="sceneDepth",.binding=1u,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},
      {.name="geometryNormal",.binding=2u,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},
      {.name="instanceId",.binding=11u,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute},
      {.name="voxelId",.binding=12u,.visibility=::rendering::BindingVisibility::BindingVisibility_Compute}}}}});
    const auto createGroups = [&](const ::rendering::Buffer buildUniforms,
                                  const std::string &name)
    {
      return renderGraph->createBindingGroups({.layout=layout_,.name=name,.groups={::rendering::GroupInfo{.name="group0",.buffers={
        {.binding=0u,.bufferView=bufferView(uniforms_,sizeof(Uniforms),::rendering::AccessPattern::UNIFORM_READ)}, {.binding=3u,.bufferView=bufferView(buffers_.rays,count*sizeof(SparseVoxelSceneRayGPU),::rendering::AccessPattern::SHADER_READ)}, {.binding=4u,.bufferView=bufferView(buffers_.hits,count*sizeof(SparseVoxelSceneRayHitGPU),::rendering::AccessPattern::SHADER_WRITE)}, {.binding=5u,.bufferView=bufferView(buffers_.activeRayIndices,count*sizeof(uint32_t),::rendering::AccessPattern::SHADER_READ)}, {.binding=6u,.bufferView=bufferView(buffers_.activeRayState,16u,::rendering::AccessPattern::SHADER_READ)}, {.binding=7u,.bufferView=bufferView(rasterBuffer_,sizeof(settings_.raster),::rendering::AccessPattern::UNIFORM_READ)}, {.binding=8u,.bufferView=bufferView(depthBounds_,depthBoundsBytes,::rendering::AccessPattern::SHADER_READ_WRITE)}, {.binding=9u,.bufferView=bufferView(buildUniforms,sizeof(DepthBoundsBuildUniforms),::rendering::AccessPattern::UNIFORM_READ)}, {.binding=10u,.bufferView=bufferView(buffers_.statistics,sizeof(SparseVoxelScreenTraceStatisticsGPU),::rendering::AccessPattern::SHADER_READ_WRITE)}},.textures={{.binding=1u,.textureView=depthView(depth_)},{.binding=2u,.textureView={.texture=geometryNormal_,.access=::rendering::AccessPattern::SHADER_READ,.layout=::rendering::ResourceLayout::SHADER_READ_ONLY,.index=0u,.flags=::rendering::ImageAspectFlags::Color,.baseArrayLayer=0u,.baseMipLevel=0u,.layerCount=1u,.levelCount=1u}},{.binding=11u,.textureView={.texture=instanceId_,.access=::rendering::AccessPattern::SHADER_READ,.layout=::rendering::ResourceLayout::SHADER_READ_ONLY,.index=0u,.flags=::rendering::ImageAspectFlags::Color,.baseArrayLayer=0u,.baseMipLevel=0u,.layerCount=1u,.levelCount=1u}},{.binding=12u,.textureView={.texture=voxelId_,.access=::rendering::AccessPattern::SHADER_READ,.layout=::rendering::ResourceLayout::SHADER_READ_ONLY,.index=0u,.flags=::rendering::ImageAspectFlags::Color,.baseArrayLayer=0u,.baseMipLevel=0u,.layerCount=1u,.levelCount=1u}}}}}});
    };
    groups_ = createGroups(
        depthBoundsBuildUniforms_.front(), passName+"_Groups");
    depthBoundsReduceGroups_.reserve(levels - 1u);
    for (uint32_t mip = 1u; mip < levels; ++mip)
      depthBoundsReduceGroups_.push_back(createGroups(
          depthBoundsBuildUniforms_[mip],
          passName+"_DepthBoundsReduceGroups"+std::to_string(mip)));
    shader_=renderGraph->createShader({.name=passName+"_Shader",.layout=layout_,.src=os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-scene-screen-trace.spirv"),.type=::rendering::ShaderType::SpirV});
    depthBoundsInitializePipeline_=renderGraph->createComputePipeline({.entry="initializeSparseVoxelScreenDepthBounds",.layout=layout_,.name=passName+"_DepthBoundsInitializePipeline",.shader=shader_});
    depthBoundsReducePipeline_=renderGraph->createComputePipeline({.entry="reduceSparseVoxelScreenDepthBounds",.layout=layout_,.name=passName+"_DepthBoundsReducePipeline",.shader=shader_});
    pipeline_=renderGraph->createComputePipeline({.entry="traceSparseVoxelSceneRays",.layout=layout_,.name=passName+"_Pipeline",.shader=shader_});
  }
  ::rendering::Texture depth_{}, geometryNormal_{}, instanceId_{}, voxelId_{};
  Buffers buffers_{};
  Settings settings_{};
  ::rendering::Buffer uniforms_{}, rasterBuffer_{}, depthBounds_{},
      ownedActiveRayIndices_{}, ownedActiveRayState_{}, ownedStatistics_{};
  std::vector<::rendering::Buffer> depthBoundsBuildUniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  std::vector<::rendering::BindingGroups> depthBoundsReduceGroups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{}, depthBoundsInitializePipeline_{},
      depthBoundsReducePipeline_{};
};

} // namespace voxel::svraster::rendering
