// The unified visibility path on the GPU: both rasterizers write one 64-bit
// key per pixel, a merge folds them, and a fullscreen resolve writes the whole
// G-buffer from the key. Every section renders the same synthetic voxel chunk
// through VirtualGeometryRendererPass with two-pass occlusion, as the sample
// does, and reads the G-buffer back:
//
//   [hw-resolve]  with the software rasterizer off, the resolved G-buffer is
//                 the hardware draw's G-buffer: depth bits, ids and material
//                 exact, normal and UV within interpolation tolerance;
//   [sw-parity]   with every cluster routed to the software rasterizer, the
//                 picture is the hardware picture: coverage, depth and
//                 triangle ids agree everywhere but a sliver of edge pixels;
//   [mixed]       with a routing threshold that splits clusters between the
//                 two rasterizers, the merged picture still matches the
//                 hardware-only picture, settles to a bit-identical G-buffer
//                 on a static camera, and phase two never re-emits;
//   [lod]         every cluster drawn by either rasterizer passes the
//                 projected-error cut test;
//   [culling]     with the software path on, disabling occlusion or frustum
//                 culling draws more clusters and the identical pixels.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "SparseVoxelVGTestFixture.hpp"

#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualgeometry/rendering/VirtualGeometryGBufferReadbackPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryRendererPass.hpp"

using namespace rendering;
using namespace virtualgeometry;
using namespace virtualgeometry::gpgpu;
using namespace vgfixture;

namespace
{

constexpr uint32_t kImageSide = 256u;
constexpr uint32_t kMaxQueueElements = 1u << 20;
constexpr float kNear = 0.5f;
constexpr float kFar = 20000.0f;
constexpr float kErrorBudget = 1.0f;
// The culling statistics buffer: 32 words. Word 31 of the phase-one pass is
// the size of last frame's set as the advance kernel produced it.
constexpr uint64_t kStatsBytes = 32u * sizeof(uint32_t);
constexpr uint32_t kAdvanceCountWord = 31u;

int failures = 0;

void check(const bool condition, const std::string &message)
{
  os::Logger::logf("  %s %s", condition ? "PASSED" : "FAILED", message.c_str());
  failures += condition ? 0 : 1;
}

struct CullingCounters
{
  uint32_t hierarchyQueueSize;
  uint32_t clusterQueueSize;
  uint32_t visibleClusterHardwareCount;
  uint32_t readQueueSize;
  uint32_t visibleClusterSoftwareCount;
};

struct GBufferPixel
{
  uint32_t depthBits = 0u;
  uint32_t lo = 0u;
  uint32_t hi = 0u;
  uint32_t material = 0u;
  float nx = 0.0f;
  float ny = 0.0f;
  float nz = 0.0f;
  float ux = 0.0f;
  float uy = 0.0f;
  bool covered() const { return depthBits != 0u; }
};

struct FrameResult
{
  uint32_t phaseOneHardware = 0u;
  uint32_t phaseTwoHardware = 0u;
  uint32_t phaseOneSoftware = 0u;
  uint32_t phaseTwoSoftware = 0u;
  // What the advance kernel handed phase one this frame.
  uint32_t advanceCount = 0u;
  std::vector<GBufferPixel> pixels;
  std::set<uint64_t> drawn;
  uint32_t duplicateEntries = 0u;
  uint32_t covered = 0u;
  uint64_t hash = 0u;
  // Phase one's software depth buffer, straight from the rasterizer.
  std::vector<uint32_t> softwareDepth;
  uint32_t drawnTotal() const { return phaseOneHardware + phaseTwoHardware + phaseOneSoftware + phaseTwoSoftware; }
};

struct RigSettings
{
  bool unified = false;
  bool software = false;
  float threshold = 32.0f;
  bool occlusion = true;
  bool frustum = true;
};

uint64_t clusterKey(const VisibleClusterInfo &info)
{
  return (static_cast<uint64_t>(info.pageIndex) << 32u) | info.pageLocalClusterIndex;
}

// One graph, scene and renderer configuration; every section owns as many
// as it compares.
class Rig
{
public:
  Rig(backend::vulkan::VulkanRHI *rhi, const ChunkAsset &asset, const RigSettings &rig, const std::string &prefix, const uint32_t framesInFlight = 1u)
      : asset_(asset), rig_(rig), prefix_(prefix), framesInFlight_(std::max(1u, framesInFlight))
  {
    const uint32_t hierarchySize = static_cast<uint32_t>(asset.encoded.hierarchy.size());
    const uint32_t hierarchyBytes = std::max(hierarchySize * static_cast<uint32_t>(sizeof(VirtualGeometryHierarchy)) * 12u, 65536u);
    graphStorage_ = std::make_unique<RenderGraph>(rhi, RenderGraph::Settings{.maxFramesInFlight = framesInFlight_});
    graph_ = graphStorage_.get();
    VirtualGeometryScene::Settings sceneSettings{};
    // Keep the reference hardware rig expanded; every other rig exercises
    // encoded reads, so comparisons catch shared decoder errors as well.
    sceneSettings.compressedGeometry = std::getenv("VG_TEST_COMPRESSED") != nullptr && prefix != "Hw_";
    sceneSettings.asyncRuntimeUploads = std::getenv("VG_TEST_NATIVE_ATOMIC64") != nullptr;
    sceneSettings.metalExportableBuffers = std::getenv("VG_TEST_NATIVE_ATOMIC64") != nullptr;
    scene_ = std::make_unique<VirtualGeometryScene>(graph_, hierarchyBytes, 256ull * 1024 * 1024, sceneSettings);

    VirtualGeometryScene::RuntimeEncodedObjectInstallInfo install{};
    install.name = "voxel_chunk";
    install.asset = &asset_.encoded;
    install.basePageGeneration = 1u;
    install.geometryRevision = 1u;
    install.materialRevision = 1u;
    install.position = math::Vec3f(0.0f, 0.0f, 0.0f);
    install.hasTracePayload = true;
    install.enqueuePageDecode = true;
    std::string installError;
    installed_ = scene_->installRuntimeEncodedObject(install, nullptr, &installError);
    if (!installed_)
      os::Logger::errorf("%s: install failed: %s", prefix.c_str(), installError.c_str());
    scene_->updateInstanceBuffer();

    const auto color = [&](const char *name, const Format format)
    {
      return graph_->createTexture(TextureInfo{
        .name = prefix_ + name,
        .format = format,
        .memoryProperties = BufferUsage::BufferUsage_None,
        .usage = ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled,
        .width = kImageSide,
        .height = kImageSide,
        .mipLevels = 1u,
      });
    };
    const auto colorView = [](const Texture texture)
    {
      return TextureView{
        .texture = texture,
        .index = 0u,
        .flags = ImageAspectFlags::Color,
        .baseMipLevel = 0u,
        .levelCount = 1u,
        .baseArrayLayer = 0u,
        .layerCount = 1u,
        .access = AccessPattern::COLOR_ATTACHMENT_WRITE,
        .layout = ResourceLayout::COLOR_ATTACHMENT,
      };
    };
    depth_ = graph_->createTexture(TextureInfo{
      .name = prefix_ + "Depth.texture",
      .format = Format::Format_Depth32Float,
      .memoryProperties = BufferUsage::BufferUsage_None,
      .usage = ImageUsage::ImageUsage_DepthStencilAttachment | ImageUsage::ImageUsage_Sampled,
      .width = kImageSide,
      .height = kImageSide,
      .mipLevels = 1u,
    });
    const TextureView depthView{
      .texture = depth_,
      .index = 0u,
      .flags = ImageAspectFlags::Depth,
      .baseMipLevel = 0u,
      .levelCount = 1u,
      .baseArrayLayer = 0u,
      .layerCount = 1u,
      .access = AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE | AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ,
      .layout = ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
    };
    idsLo_ = color("IdsLo.texture", Format::Format_R32Uint);
    idsHi_ = color("IdsHi.texture", Format::Format_R32Uint);
    materialId_ = color("MaterialId.texture", Format::Format_R32Uint);
    materialUV_ = color("MaterialUV.texture", Format::Format_RG32Float);
    geometryNormal_ = color("GeometryNormal.texture", Format::Format_RGBA16Float);
    Texture motion = color("Motion.texture", Format::Format_RG16Float);
    Texture baseColor = color("BaseColor.texture", Format::Format_RGBA16Float);
    Texture materialProperties = color("MaterialProperties.texture", Format::Format_RGBA16Float);
    Texture emissive = color("Emissive.texture", Format::Format_RGBA16Float);
    Texture normal = color("Normal.texture", Format::Format_RGBA16Float);
    Texture colorTexture = color("Color.texture", Format::Format_RGBA16Float);
    hzbMipCount_ = 1u;
    for (uint32_t dimension = kImageSide; dimension > 1u; dimension >>= 1u)
      ++hzbMipCount_;
    hzbMipCount_ = std::min(hzbMipCount_, 7u);
    Texture hzb = graph_->createTexture(TextureInfo{
      .name = prefix_ + "HZB.texture",
      .format = Format::Format_R32Float,
      .memoryProperties = BufferUsage::BufferUsage_Storage,
      .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
      .width = kImageSide,
      .height = kImageSide,
      .mipLevels = hzbMipCount_,
    });
    VirtualGeometryHardwareDrawPass::FrameTarget frameTarget{
      .packedGeometryIdsLoView = colorView(idsLo_),
      .packedGeometryIdsHiView = colorView(idsHi_),
      .materialIdView = colorView(materialId_),
      .materialUVView = colorView(materialUV_),
      .baseColorView = colorView(baseColor),
      .materialPropertiesView = colorView(materialProperties),
      .emissiveView = colorView(emissive),
      .geometryNormalView = colorView(geometryNormal_),
      .normalView = colorView(normal),
      .motionVectorView = colorView(motion),
      .depthView = depthView,
      .depthTexture = depth_,
      .baseColorTexture = baseColor,
      .materialPropertiesTexture = materialProperties,
      .emissiveTexture = emissive,
      .geometryNormalTexture = geometryNormal_,
      .normalTexture = normal,
      .motionVectorTexture = motion,
      .colorView = colorView(colorTexture),
      .colorTexture = colorTexture,
    };
    VirtualGeometryDepthPrePassDrawPass::FrameTarget prePassTarget{.depthView = depthView, .depthTexture = depth_};

    VirtualGeometryRendererPass::Settings settings{};
    settings.prepassCullingSettings.maxHierarchyLevels = 32u;
    settings.prepassCullingSettings.clustersQueueBufferSize = kMaxQueueElements * sizeof(uint32_t) * 4u;
    settings.prepassCullingSettings.errorTreshhold = kErrorBudget;
    settings.prepassCullingSettings.viewPortWidth = kImageSide;
    settings.prepassCullingSettings.viewPortHeight = kImageSide;
    settings.prepassCullingSettings.hiZMipLevels = hzbMipCount_;
    settings.prepassCullingSettings.useExternalHiZTexture = true;
    settings.prepassCullingSettings.enableFrustumCulling = rig_.frustum;
    settings.prepassCullingSettings.enableOcclusionCulling = rig_.occlusion;
    settings.prepassCullingSettings.enableStreamingPriorityUpdates = false;
    settings.prepassCullingSettings.swTriangleEdgePixelThreshold = 0.0f;
    settings.finalCullingSettings = settings.prepassCullingSettings;
    settings.finalCullingSettings.enableStreamingPriorityUpdates = true;
    settings.depthPrePassSettings.viewPortWidth = kImageSide;
    settings.depthPrePassSettings.viewPortHeight = kImageSide;
    settings.depthPrePassSettings.cullMode = CullMode::None;
    settings.depthPrePassSettings.directDrawWhenIndirectCountUnavailable = false;
    settings.depthPyramidSettings.width = kImageSide;
    settings.depthPyramidSettings.height = kImageSide;
    settings.depthPyramidSettings.outputWidth = kImageSide;
    settings.depthPyramidSettings.outputHeight = kImageSide;
    settings.depthPyramidSettings.mipCount = hzbMipCount_;
    settings.depthPyramidSettings.outputFormat = Format::Format_R32Float;
    settings.hardwareDrawSettings.viewPortWidth = kImageSide;
    settings.hardwareDrawSettings.viewPortHeight = kImageSide;
    settings.hardwareDrawSettings.colorFormat = Format::Format_RGBA16Float;
    settings.hardwareDrawSettings.motionVectorFormat = Format::Format_RG16Float;
    settings.hardwareDrawSettings.depthFormat = Format::Format_Depth32Float;
    settings.hardwareDrawSettings.cullMode = CullMode::None;
    settings.hardwareDrawSettings.directDrawWhenIndirectCountUnavailable = false;
    settings.computeRasterSettings.enabled = false;
    settings.twoPhaseOcclusion = true;
    settings.registerScreenSpaceRayTracePass = false;
    settings.registerMaterialPass = false;
    settings.unifiedVisibility = rig_.unified;
    settings.softwareRaster.enabled = rig_.software;
    settings.softwareRaster.projectedTriangleEdgePixelThreshold = rig_.threshold;

    uint32_t passIndex = 0u;
    renderer_ = graph_->registerPass<VirtualGeometryRendererPass>(
        prefix_ + "renderer", passIndex++, *scene_, frameTarget, prePassTarget, Texture{}, hzb, 0.0f, settings);
    passIndex += VirtualGeometryRendererPass::childPassCount(settings);

    gbufferBytes_ = VirtualGeometryGBufferReadbackPass::outputBytes(kImageSide, kImageSide);
    gbufferBuffer_ = graph_->createBuffer(BufferInfo{
      .name = prefix_ + "GBuffer.buffer",
      .size = gbufferBytes_,
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
    });
    const auto staging = [&](const char *name, const uint64_t bytes)
    {
      return graph_->createBuffer(BufferInfo{
        .name = prefix_ + name,
        .size = bytes,
        .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
      });
    };
    gbufferStaging_ = staging("GBuffer.staging", gbufferBytes_);
    visibleBytes_ = static_cast<uint64_t>(VirtualGeometryScene::MAX_VISIBLE_CLUSTERS) * sizeof(VisibleClusterInfo);
    preStatsStaging_ = staging("PreStats.staging", kStatsBytes);
    finalStatsStaging_ = staging("FinalStats.staging", kStatsBytes);
    preHardwareStaging_ = staging("PreHardware.staging", visibleBytes_);
    preSoftwareStaging_ = staging("PreSoftware.staging", visibleBytes_);
    finalHardwareStaging_ = staging("FinalHardware.staging", visibleBytes_);
    finalSoftwareStaging_ = staging("FinalSoftware.staging", visibleBytes_);
    // With frames in flight the copies land in per-slot buffers substituted
    // through the frame overrides, as the sample does.
    if (framesInFlight_ > 1u)
      for (uint32_t slot = 0u; slot < framesInFlight_; ++slot)
      {
        const std::string suffix = std::to_string(slot);
        slotStaging_.push_back(SlotStaging{
          .gbuffer = staging(("GBuffer.staging" + suffix).c_str(), gbufferBytes_),
          .preStats = staging(("PreStats.staging" + suffix).c_str(), kStatsBytes),
          .finalStats = staging(("FinalStats.staging" + suffix).c_str(), kStatsBytes),
          .preHardware = staging(("PreHardware.staging" + suffix).c_str(), visibleBytes_),
          .preSoftware = staging(("PreSoftware.staging" + suffix).c_str(), visibleBytes_),
          .finalHardware = staging(("FinalHardware.staging" + suffix).c_str(), visibleBytes_),
          .finalSoftware = staging(("FinalSoftware.staging" + suffix).c_str(), visibleBytes_),
        });
      }

    graph_->registerPass<VirtualGeometryGBufferReadbackPass>(
        prefix_ + "gbufferReadback", passIndex++,
        VirtualGeometryGBufferReadbackPass::Inputs{
          .depth = depth_,
          .packedGeometryIdsLo = idsLo_,
          .packedGeometryIdsHi = idsHi_,
          .materialId = materialId_,
          .geometryNormal = geometryNormal_,
          .materialUV = materialUV_,
        },
        gbufferBuffer_, kImageSide, kImageSide);
    graph_->registerPass<rendering::gpgpu::CopyBufferPass>(
        prefix_ + "copyGBuffer", passIndex++, gbufferBuffer_, 0, gbufferBytes_, gbufferStaging_, 0, gbufferBytes_);
    graph_->compile();

    preCulling_ = renderer_->getPreCullingPass();
    finalCulling_ = renderer_->getFinalCullingPass();
    phaseOneDraw_ = renderer_->getPhaseOneDrawPass();
    hardwareDraw_ = renderer_->getHardwareDrawPass();
    valid_ = installed_ && preCulling_ != nullptr && finalCulling_ != nullptr && phaseOneDraw_ != nullptr && hardwareDraw_ != nullptr;
    if (!valid_)
      return;
    graph_->registerPass<rendering::gpgpu::CopyBufferPass>(
        prefix_ + "copyPreStats", passIndex++, preCulling_->getCullingStatisticsBuffer(), 0, kStatsBytes, preStatsStaging_, 0, kStatsBytes);
    graph_->registerPass<rendering::gpgpu::CopyBufferPass>(
        prefix_ + "copyFinalStats", passIndex++, finalCulling_->getCullingStatisticsBuffer(), 0, kStatsBytes, finalStatsStaging_, 0, kStatsBytes);
    graph_->registerPass<rendering::gpgpu::CopyBufferPass>(
        prefix_ + "copyPreHardware", passIndex++, preCulling_->getHWVisibleClusterInfosBuffer(), 0, visibleBytes_, preHardwareStaging_, 0, visibleBytes_);
    graph_->registerPass<rendering::gpgpu::CopyBufferPass>(
        prefix_ + "copyPreSoftware", passIndex++, preCulling_->getSWVisibleClusterInfosBuffer(), 0, visibleBytes_, preSoftwareStaging_, 0, visibleBytes_);
    graph_->registerPass<rendering::gpgpu::CopyBufferPass>(
        prefix_ + "copyFinalHardware", passIndex++, finalCulling_->getHWVisibleClusterInfosBuffer(), 0, visibleBytes_, finalHardwareStaging_, 0, visibleBytes_);
    graph_->registerPass<rendering::gpgpu::CopyBufferPass>(
        prefix_ + "copyFinalSoftware", passIndex++, finalCulling_->getSWVisibleClusterInfosBuffer(), 0, visibleBytes_, finalSoftwareStaging_, 0, visibleBytes_);
    if (auto *software = renderer_->getPhaseOneSoftwareRasterPass())
    {
      softwareDepthBytes_ = software->pixelBufferBytes();
      softwareDepthStaging_ = staging("SoftwareDepth.staging", softwareDepthBytes_);
      graph_->registerPass<rendering::gpgpu::CopyBufferPass>(
          prefix_ + "copySoftwareDepth", passIndex++, software->getDepthBuffer(), 0, softwareDepthBytes_, softwareDepthStaging_, 0, softwareDepthBytes_);
    }
    pageTableStaging_ = graph_->createBuffer(BufferInfo{
      .name = prefix_ + "PageTable.staging", .size = scene_->pagesTableBufferSize,
      .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
    });
    graph_->registerPass<::rendering::gpgpu::CopyBufferPass>(prefix_ + "PageTableCopy", passIndex++,
        scene_->pageTableBuffer, 0u, scene_->pagesTableBufferSize,
        pageTableStaging_, 0u, scene_->pagesTableBufferSize);
    graph_->compile();
  }

  bool valid() const
  {
    if (scene_->settings.compressedGeometry) {
      const auto usage = scene_->getRuntimePoolUsage();
      if (usage.decodedVerticesUsed != 0u || usage.decodedIndicesUsed != 0u ||
          scene_->decodedVertexBufferSize > sizeof(DecodedVertexData) ||
          scene_->decodedIndexBufferSize > sizeof(uint32_t)) return false;
    }
    if (rig_.software && std::getenv("VG_TEST_NATIVE_ATOMIC64") != nullptr)
    {
      const auto *pass = renderer_->getSoftwareRasterPass();
      return valid_ && pass != nullptr && pass->usesNativeAtomic64();
    }
    return valid_;
  }

  // Install an additional object at a position, as a terrain refinement
  // does when children arrive next to their siblings.
  bool installObject(const std::string &name, const uint64_t generation, const math::Vec3f &position)
  {
    std::string error;
    VirtualGeometryScene::RuntimeEncodedObjectInstallInfo install{};
    install.name = name;
    install.asset = &asset_.encoded;
    install.basePageGeneration = generation;
    install.geometryRevision = generation;
    install.materialRevision = 1u;
    install.position = position;
    install.hasTracePayload = true;
    install.enqueuePageDecode = true;
    if (!scene_->installRuntimeEncodedObject(install, nullptr, &error))
    {
      os::Logger::errorf("%s: install of %s failed: %s", prefix_.c_str(), name.c_str(), error.c_str());
      return false;
    }
    scene_->updateInstanceBuffer();
    return true;
  }

  bool unregisterObject(const std::string &name)
  {
    std::string error;
    if (!scene_->unregisterRuntimeObject(name, &error))
    {
      os::Logger::errorf("%s: unregister of %s failed: %s", prefix_.c_str(), name.c_str(), error.c_str());
      return false;
    }
    return true;
  }

  // Unregister + install into fresh ranges, as a terrain chunk rebuild does.
  bool replaceObject(const uint64_t generation)
  {
    std::string error;
    if (!scene_->unregisterRuntimeObject("voxel_chunk", &error))
    {
      os::Logger::errorf("%s: unregister failed: %s", prefix_.c_str(), error.c_str());
      return false;
    }
    VirtualGeometryScene::RuntimeEncodedObjectInstallInfo install{};
    install.name = "voxel_chunk";
    install.asset = &asset_.encoded;
    install.basePageGeneration = generation;
    install.geometryRevision = generation;
    install.materialRevision = 1u;
    install.position = math::Vec3f(0.0f, 0.0f, 0.0f);
    install.hasTracePayload = true;
    install.enqueuePageDecode = true;
    if (!scene_->installRuntimeEncodedObject(install, nullptr, &error))
    {
      os::Logger::errorf("%s: reinstall failed: %s", prefix_.c_str(), error.c_str());
      return false;
    }
    scene_->updateInstanceBuffer();
    return true;
  }

  struct InFlight
  {
    RenderGraph::Frame frame{};
    uint32_t slot = 0u;
  };

  // Records and submits a frame without waiting for it; readFrame collects
  // it. Only meaningful with framesInFlight > 1.
  InFlight recordFrame(const CameraState &camera)
  {
    const uint32_t slot = frameSlot_;
    graph_->setCurrentFrameIndex(slot);
    updateUniforms(camera);
    scene_->prepareFrame(slot);
    RenderGraph::Overrides overrides;
    renderer_->appendFrameOverrides(overrides, slot);
    if (!slotStaging_.empty())
    {
      const SlotStaging &target = slotStaging_[slot % slotStaging_.size()];
      const auto override = [&](const Buffer &virtualBuffer, const Buffer &real)
      {
        overrides.bufferOverrides.emplace(virtualBuffer.name, RenderGraphBufferOverride{.bufferId = graph_->resolveBufferId(real)});
      };
      override(gbufferStaging_, target.gbuffer);
      override(preStatsStaging_, target.preStats);
      override(finalStatsStaging_, target.finalStats);
      override(preHardwareStaging_, target.preHardware);
      override(preSoftwareStaging_, target.preSoftware);
      override(finalHardwareStaging_, target.finalHardware);
      override(finalSoftwareStaging_, target.finalSoftware);
    }
    InFlight inFlight{};
    inFlight.slot = slot;
    inFlight.frame.frameIndex = slot;
    graph_->run(inFlight.frame, overrides);
    frameSlot_ = (frameSlot_ + 1u) % framesInFlight_;
    return inFlight;
  }

  FrameResult readFrame(InFlight &inFlight)
  {
    graph_->waitFrame(inFlight.frame);
    graph_->setCurrentFrameIndex(inFlight.slot);
    if (slotStaging_.empty())
      return collect(gbufferStaging_, preStatsStaging_, finalStatsStaging_, preHardwareStaging_, preSoftwareStaging_, finalHardwareStaging_, finalSoftwareStaging_);
    const SlotStaging &target = slotStaging_[inFlight.slot % slotStaging_.size()];
    return collect(target.gbuffer, target.preStats, target.finalStats, target.preHardware, target.preSoftware, target.finalHardware, target.finalSoftware);
  }

  void setReleaseDelay(const uint32_t frames) { scene_->setRuntimeReleaseDelayFrames(frames); }

  std::vector<PageTableEntry> pageTable() const
  {
    std::vector<PageTableEntry> table(scene_->nextPageTableSlot);
    if (!table.empty())
      graph_->bufferRead(pageTableStaging_, 0, table.size() * sizeof(PageTableEntry),
                         [&](const void *data) { std::memcpy(table.data(), data, table.size() * sizeof(PageTableEntry)); });
    return table;
  }

  FrameResult run(const CameraState &camera)
  {
    InFlight inFlight = recordFrame(camera);
    return readFrame(inFlight);
  }

private:
  void updateUniforms(const CameraState &camera)
  {
    ++frameNumber_;
    scene_->setRuntimeStreamingFrame(frameNumber_);
    scene_->touchRuntimePagesFromGPUPriorities();
    const float *view = camera.view.data;
    const float *projection = camera.projection.data;
    float viewPosition[4] = {camera.eye[0], camera.eye[1], camera.eye[2], 1.0f};
    preCulling_->updateUniforms(view, projection, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount_);
    finalCulling_->updateUniforms(view, projection, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount_);
    phaseOneDraw_->updateUniforms(view, projection, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount_, kErrorBudget);
    hardwareDraw_->updateUniforms(view, projection, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount_, kErrorBudget);
    if (auto *pass = renderer_->getPhaseOneVisibilityResolvePass())
      pass->updateUniforms(view, projection, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount_, kErrorBudget);
    if (auto *pass = renderer_->getVisibilityResolvePass())
      pass->updateUniforms(view, projection, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount_, kErrorBudget);
  }

  FrameResult collect(
      const Buffer &gbufferStaging, const Buffer &preStatsStaging, const Buffer &finalStatsStaging, const Buffer &preHardwareStaging,
      const Buffer &preSoftwareStaging, const Buffer &finalHardwareStaging, const Buffer &finalSoftwareStaging)
  {
    FrameResult result{};
    CullingCounters pre{};
    CullingCounters fin{};
    std::array<uint32_t, 32u> preWords{};
    graph_->bufferRead(preStatsStaging, 0, kStatsBytes, [&](const void *data) { std::memcpy(preWords.data(), data, kStatsBytes); });
    std::memcpy(&pre, preWords.data(), sizeof(CullingCounters));
    result.advanceCount = preWords[kAdvanceCountWord];
    graph_->bufferRead(finalStatsStaging, 0, sizeof(CullingCounters), [&](const void *data) { std::memcpy(&fin, data, sizeof(CullingCounters)); });
    result.phaseOneHardware = pre.visibleClusterHardwareCount;
    result.phaseOneSoftware = pre.visibleClusterSoftwareCount;
    result.phaseTwoHardware = fin.visibleClusterHardwareCount;
    result.phaseTwoSoftware = fin.visibleClusterSoftwareCount;
    const auto readVisible = [&](const Buffer &stagingBuffer, const uint32_t count)
    {
      if (count == 0u || count > VirtualGeometryScene::MAX_VISIBLE_CLUSTERS)
        return;
      std::vector<VisibleClusterInfo> visible(count);
      graph_->bufferRead(stagingBuffer, 0, count * sizeof(VisibleClusterInfo),
                         [&](const void *data) { std::memcpy(visible.data(), data, count * sizeof(VisibleClusterInfo)); });
      for (const auto &info : visible)
        if (!result.drawn.insert(clusterKey(info)).second)
          ++result.duplicateEntries;
    };
    readVisible(preHardwareStaging, result.phaseOneHardware);
    readVisible(preSoftwareStaging, result.phaseOneSoftware);
    readVisible(finalHardwareStaging, result.phaseTwoHardware);
    readVisible(finalSoftwareStaging, result.phaseTwoSoftware);

    std::vector<uint32_t> words(gbufferBytes_ / sizeof(uint32_t), 0u);
    graph_->bufferRead(gbufferStaging, 0, gbufferBytes_, [&](const void *data) { std::memcpy(words.data(), data, gbufferBytes_); });
    result.pixels.resize(kImageSide * kImageSide);
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t pixel = 0u; pixel < kImageSide * kImageSide; ++pixel)
    {
      const uint32_t *w = words.data() + pixel * VirtualGeometryGBufferReadbackPass::WORDS_PER_PIXEL;
      GBufferPixel &out = result.pixels[pixel];
      out.depthBits = w[0];
      out.lo = w[1];
      out.hi = w[2];
      out.material = w[3];
      std::memcpy(&out.nx, &w[4], sizeof(float));
      std::memcpy(&out.ny, &w[5], sizeof(float));
      std::memcpy(&out.nz, &w[6], sizeof(float));
      std::memcpy(&out.ux, &w[7], sizeof(float));
      std::memcpy(&out.uy, &w[8], sizeof(float));
      result.covered += out.covered() ? 1u : 0u;
      hash = (hash ^ out.depthBits) * 1099511628211ull;
      hash = (hash ^ out.hi) * 1099511628211ull;
    }
    result.hash = hash;
    if (softwareDepthStaging_.isValid() && slotStaging_.empty())
    {
      result.softwareDepth.resize(softwareDepthBytes_ / sizeof(uint32_t), 0u);
      graph_->bufferRead(softwareDepthStaging_, 0, softwareDepthBytes_,
                         [&](const void *data) { std::memcpy(result.softwareDepth.data(), data, softwareDepthBytes_); });
    }
    return result;
  }

public:
  struct SlotStaging
  {
    Buffer gbuffer{};
    Buffer preStats{};
    Buffer finalStats{};
    Buffer preHardware{};
    Buffer preSoftware{};
    Buffer finalHardware{};
    Buffer finalSoftware{};
  };

private:
  const ChunkAsset &asset_;
  RigSettings rig_;
  std::string prefix_;
  uint32_t framesInFlight_ = 1u;
  std::vector<SlotStaging> slotStaging_;
  std::unique_ptr<RenderGraph> graphStorage_;
  RenderGraph *graph_ = nullptr;
  std::unique_ptr<VirtualGeometryScene> scene_;
  bool installed_ = false;
  Buffer pageTableStaging_{};
  bool valid_ = false;
  Texture depth_{};
  Texture idsLo_{};
  Texture idsHi_{};
  Texture materialId_{};
  Texture materialUV_{};
  Texture geometryNormal_{};
  uint32_t hzbMipCount_ = 1u;
  VirtualGeometryRendererPass *renderer_ = nullptr;
  VirtualGeometryCullingMultipleDispatchesPass *preCulling_ = nullptr;
  VirtualGeometryCullingMultipleDispatchesPass *finalCulling_ = nullptr;
  VirtualGeometryHardwareDrawPass *phaseOneDraw_ = nullptr;
  VirtualGeometryHardwareDrawPass *hardwareDraw_ = nullptr;
  uint64_t gbufferBytes_ = 0u;
  uint64_t visibleBytes_ = 0u;
  Buffer gbufferBuffer_{};
  Buffer gbufferStaging_{};
  Buffer preStatsStaging_{};
  Buffer finalStatsStaging_{};
  Buffer preHardwareStaging_{};
  Buffer preSoftwareStaging_{};
  Buffer finalHardwareStaging_{};
  Buffer finalSoftwareStaging_{};
  Buffer softwareDepthStaging_{};
  uint64_t softwareDepthBytes_ = 0u;
  uint64_t frameNumber_ = 0u;
  uint32_t frameSlot_ = 0u;
};

struct Comparison
{
  uint32_t coveredA = 0u;
  uint32_t coveredB = 0u;
  uint32_t coveredBoth = 0u;
  uint32_t coveredOnlyA = 0u;
  uint32_t coveredOnlyB = 0u;
  uint32_t depthExact = 0u;
  uint32_t depthClose = 0u;
  uint32_t hiEqual = 0u;
  uint32_t loEqual = 0u;
  uint32_t materialEqual = 0u;
  uint32_t normalClose = 0u;
  uint32_t uvClose = 0u;
  float maxNormalDelta = 0.0f;
  float maxUvDelta = 0.0f;
  float maxDepthDelta = 0.0f;
};

Comparison compare(const FrameResult &a, const FrameResult &b)
{
  Comparison c{};
  for (uint32_t pixel = 0u; pixel < a.pixels.size(); ++pixel)
  {
    const GBufferPixel &pa = a.pixels[pixel];
    const GBufferPixel &pb = b.pixels[pixel];
    c.coveredA += pa.covered() ? 1u : 0u;
    c.coveredB += pb.covered() ? 1u : 0u;
    if (pa.covered() != pb.covered())
    {
      c.coveredOnlyA += pa.covered() ? 1u : 0u;
      c.coveredOnlyB += pb.covered() ? 1u : 0u;
      continue;
    }
    if (!pa.covered())
      continue;
    ++c.coveredBoth;
    float da = 0.0f;
    float db = 0.0f;
    std::memcpy(&da, &pa.depthBits, sizeof(float));
    std::memcpy(&db, &pb.depthBits, sizeof(float));
    const float depthDelta = std::fabs(da - db);
    c.maxDepthDelta = std::max(c.maxDepthDelta, depthDelta);
    c.depthExact += pa.depthBits == pb.depthBits ? 1u : 0u;
    c.depthClose += depthDelta <= 1.0e-5f + 1.0e-4f * std::fabs(da) ? 1u : 0u;
    c.hiEqual += pa.hi == pb.hi ? 1u : 0u;
    c.loEqual += pa.lo == pb.lo ? 1u : 0u;
    c.materialEqual += pa.material == pb.material ? 1u : 0u;
    const float normalDelta = std::max({std::fabs(pa.nx - pb.nx), std::fabs(pa.ny - pb.ny), std::fabs(pa.nz - pb.nz)});
    const float uvDelta = std::max(std::fabs(pa.ux - pb.ux), std::fabs(pa.uy - pb.uy));
    c.maxNormalDelta = std::max(c.maxNormalDelta, normalDelta);
    c.maxUvDelta = std::max(c.maxUvDelta, uvDelta);
    c.normalClose += normalDelta <= 4.0e-3f ? 1u : 0u;
    // UVs span whole voxels; the hardware's fixed-point barycentrics differ
    // from the resolve's float ones by up to a hundredth of a texel at a
    // triangle edge.
    c.uvClose += uvDelta <= 1.0e-2f ? 1u : 0u;
  }
  return c;
}

std::string describe(const Comparison &c)
{
  std::ostringstream text;
  text << "coveredA=" << c.coveredA << " coveredB=" << c.coveredB << " both=" << c.coveredBoth
       << " onlyA=" << c.coveredOnlyA << " onlyB=" << c.coveredOnlyB << " depthExact=" << c.depthExact
       << " depthClose=" << c.depthClose << " hiEqual=" << c.hiEqual << " loEqual=" << c.loEqual
       << " materialEqual=" << c.materialEqual << " normalClose=" << c.normalClose << " uvClose=" << c.uvClose
       << " maxNormalDelta=" << c.maxNormalDelta << " maxUvDelta=" << c.maxUvDelta << " maxDepthDelta=" << c.maxDepthDelta;
  return text.str();
}

std::string describe(const FrameResult &f)
{
  std::ostringstream text;
  text << "p1hw=" << f.phaseOneHardware << " p1sw=" << f.phaseOneSoftware << " p2hw=" << f.phaseTwoHardware
       << " p2sw=" << f.phaseTwoSoftware << " adv=" << f.advanceCount << " drawn=" << f.drawn.size() << " px=" << f.covered;
  return text.str();
}

// CPU replica of virtualgeometry-software-raster-cs.wgsl over the build
// data, for diagnosing where the GPU rasterizer and the algorithm disagree.
struct CpuRaster
{
  std::vector<uint32_t> depthBits;
  std::vector<uint32_t> payload;
};

void transformPoint(const math::Mat4f &m, const float in[3], float out[4])
{
  for (uint32_t row = 0u; row < 4u; ++row)
    out[row] = m.data[0u * 4u + row] * in[0] + m.data[1u * 4u + row] * in[1] + m.data[2u * 4u + row] * in[2] + m.data[3u * 4u + row];
}

float cpuEdge(const float ax, const float ay, const float bx, const float by, const float px, const float py)
{
  return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

bool cpuTopLeft(const float ax, const float ay, const float bx, const float by)
{
  const float dx = bx - ax;
  const float dy = by - ay;
  return dy < 0.0f || (dy == 0.0f && dx < 0.0f);
}

CpuRaster cpuRasterize(
    const ChunkAsset &asset, const std::vector<PageTableEntry> &pageTable, const std::set<uint64_t> &drawn,
    const CameraState &camera)
{
  CpuRaster raster;
  raster.depthBits.assign(kImageSide * kImageSide, 0u);
  raster.payload.assign(kImageSide * kImageSide, 0u);
  math::Mat4f viewProj = camera.projection * camera.view;
  for (const uint64_t key : drawn)
  {
    const uint32_t page = static_cast<uint32_t>(key >> 32u);
    const uint32_t local = static_cast<uint32_t>(key & 0xffffffffu);
    if (page >= asset.build.pages.size() || page >= pageTable.size() || local >= asset.build.pages[page].clusterCount)
      continue;
    const uint32_t globalCluster = pageTable[page].clusterOffset + local;
    const VirtualGeometryCluster &cluster = asset.build.clusters[asset.build.pages[page].clusterOffset + local];
    const uint32_t triangleCount = static_cast<uint32_t>(cluster.indices.size() / 3u);
    for (uint32_t triangle = 0u; triangle < triangleCount; ++triangle)
    {
      float sx[3], sy[3], sz[3];
      bool valid = true;
      for (uint32_t corner = 0u; corner < 3u; ++corner)
      {
        const uint32_t index = cluster.indices[triangle * 3u + corner];
        if (index >= cluster.vertices.size())
        {
          valid = false;
          break;
        }
        float clip[4];
        transformPoint(viewProj, cluster.vertices[index].pos, clip);
        clip[1] = -clip[1];
        if (clip[3] <= 1.0e-6f)
        {
          valid = false;
          break;
        }
        sx[corner] = (clip[0] / clip[3] * 0.5f + 0.5f) * static_cast<float>(kImageSide);
        sy[corner] = (clip[1] / clip[3] * 0.5f + 0.5f) * static_cast<float>(kImageSide);
        sz[corner] = clip[2] / clip[3];
      }
      if (!valid)
        continue;
      float area = cpuEdge(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2]);
      if (area == 0.0f)
        continue;
      if (area < 0.0f)
      {
        std::swap(sx[1], sx[2]);
        std::swap(sy[1], sy[2]);
        std::swap(sz[1], sz[2]);
        area = -area;
      }
      const float minX = std::max(std::floor(std::min({sx[0], sx[1], sx[2]})), 0.0f);
      const float minY = std::max(std::floor(std::min({sy[0], sy[1], sy[2]})), 0.0f);
      const float maxX = std::min(std::ceil(std::max({sx[0], sx[1], sx[2]})), static_cast<float>(kImageSide));
      const float maxY = std::min(std::ceil(std::max({sy[0], sy[1], sy[2]})), static_cast<float>(kImageSide));
      if (maxX <= minX || maxY <= minY)
        continue;
      const bool tl0 = cpuTopLeft(sx[0], sy[0], sx[1], sy[1]);
      const bool tl1 = cpuTopLeft(sx[1], sy[1], sx[2], sy[2]);
      const bool tl2 = cpuTopLeft(sx[2], sy[2], sx[0], sy[0]);
      const float invArea = 1.0f / area;
      const uint32_t payload = ((globalCluster & 0x7ffffu) << 7u) | (triangle & 0x7fu);
      for (uint32_t y = static_cast<uint32_t>(minY); y < static_cast<uint32_t>(maxY); ++y)
        for (uint32_t x = static_cast<uint32_t>(minX); x < static_cast<uint32_t>(maxX); ++x)
        {
          const float px = static_cast<float>(x) + 0.5f;
          const float py = static_cast<float>(y) + 0.5f;
          const float w0 = cpuEdge(sx[1], sy[1], sx[2], sy[2], px, py);
          const float w1 = cpuEdge(sx[2], sy[2], sx[0], sy[0], px, py);
          const float w2 = cpuEdge(sx[0], sy[0], sx[1], sy[1], px, py);
          const bool inside = (w0 > 0.0f || (w0 == 0.0f && tl1)) && (w1 > 0.0f || (w1 == 0.0f && tl2)) && (w2 > 0.0f || (w2 == 0.0f && tl0));
          if (!inside)
            continue;
          const float z = (w0 * sz[0] + w1 * sz[1] + w2 * sz[2]) * invArea;
          if (z < 0.0f || z > 1.0f)
            continue;
          uint32_t bits = 0u;
          std::memcpy(&bits, &z, sizeof(bits));
          const uint32_t pixel = y * kImageSide + x;
          if (bits > raster.depthBits[pixel] || (bits == raster.depthBits[pixel] && payload > raster.payload[pixel]))
          {
            raster.depthBits[pixel] = bits;
            raster.payload[pixel] = payload;
          }
        }
    }
  }
  return raster;
}

} // namespace

int main()
{
  os::Logger::start();
  os::Logger::log("Sparse voxel virtual geometry unified visibility (GPU)");
  os::Logger::log("=======================================================");

  constexpr uint32_t kSide = 64u;
  ChunkAsset asset = buildChunkAsset(kSide, 8u);
  check(asset.valid, "the chunk builds a streamable virtual geometry asset");
  if (!asset.valid)
  {
    os::Logger::shutdown();
    return 1;
  }

  DeviceRequiredLimits limits{};
  const DeviceFeatures features = DeviceFeatures::DeviceFeatures_Compute | DeviceFeatures::DeviceFeatures_Graphics |
      DeviceFeatures::DeviceFeatures_Subgroup_Basic | DeviceFeatures::DeviceFeatures_Subgroup_Shuffle |
      DeviceFeatures::DeviceFeatures_Timestamp | DeviceFeatures::DeviceFeatures_ClipDistances;
  auto *rhi = new backend::vulkan::VulkanRHI(backend::vulkan::Vulkan_1_2, limits, features, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi->init(surfaces);

  int exitCode = 0;
  try
  {
#if defined(__APPLE__)
    if (std::getenv("VG_TEST_NATIVE_ATOMIC64") != nullptr)
    {
      auto commands = static_cast<RHI *>(rhi)->allocateCommandBuffers(Queue::Compute, 1u);
      static_cast<RHI *>(rhi)->beginCommandBuffer(commands.front(), true);
      static_cast<RHI *>(rhi)->cmdMetalCompute(commands.front(), [](void *, uint64_t, uint64_t) {
        throw std::runtime_error("injected native encoder failure");
      });
      static_cast<RHI *>(rhi)->endCommandBuffer(commands.front());
      bool rejected = false;
      try
      {
        static_cast<RHI *>(rhi)->submit(Queue::Compute, commands.data(), 1u, nullptr, 0u);
      }
      catch (const std::runtime_error &error)
      {
        rejected = std::string(error.what()) == "injected native encoder failure";
      }
      check(rejected, "native encoder failure retires its submitted prefix without hanging cleanup");
    }
#endif
    const math::Vec3f chunkCenter(0.0f, 8.0f, 0.0f);
    // Near enough that clusters span a range of triangle sizes: a routing
    // threshold in the middle of that range splits them between rasterizers.
    const CameraState camera = makeCamera(math::Vec3f(0.0f, 18.5f, 30.0f), chunkCenter, kNear, kFar);
    constexpr float kMixedThreshold = 4.0f;

    const auto settle = [&](Rig &rig, const uint32_t frames) -> FrameResult
    {
      FrameResult result{};
      for (uint32_t frame = 0u; frame < frames; ++frame)
        result = rig.run(camera);
      return result;
    };

    Rig hardware(rhi, asset, RigSettings{.unified = false, .software = false}, "Hw_");
    check(hardware.valid(), "the plain hardware rig builds");
    const FrameResult hardwareFrame = settle(hardware, 3u);
    os::Logger::logf("  hardware: %s", describe(hardwareFrame).c_str());
    check(hardwareFrame.covered > 1000u && hardwareFrame.drawn.size() > 0u, "the hardware rig draws the chunk");

    // ── [hw-resolve] ────────────────────────────────────────────────────────
    os::Logger::log("[hw-resolve] the resolve reproduces the hardware G-buffer from the key alone");
    {
      Rig unified(rhi, asset, RigSettings{.unified = true, .software = false}, "HwUnified_");
      check(unified.valid(), "the unified rig without software raster builds");
      const FrameResult frame = settle(unified, 3u);
      os::Logger::logf("  unified: %s", describe(frame).c_str());
      const Comparison c = compare(hardwareFrame, frame);
      os::Logger::logf("  %s", describe(c).c_str());
      check(c.coveredOnlyA == 0u && c.coveredOnlyB == 0u, "the resolve covers exactly the hardware pixels");
      check(c.depthExact == c.coveredBoth, "every resolved depth is bit-identical to the hardware depth");
      check(c.hiEqual == c.coveredBoth && c.loEqual == c.coveredBoth,
            "every resolved pixel carries the hardware pixel's cluster, triangle and instance");
      check(c.materialEqual == c.coveredBoth, "every resolved pixel carries the hardware pixel's material");
      check(c.normalClose == c.coveredBoth, "every resolved normal matches the interpolated hardware normal");
      check(c.uvClose == c.coveredBoth, "every resolved UV matches the interpolated hardware UV");
      check(frame.drawn == hardwareFrame.drawn, "the unified rig draws the same cluster set");
    }

    // ── [sw-parity] ─────────────────────────────────────────────────────────
    os::Logger::log("[sw-parity] every cluster through the software rasterizer draws the hardware picture");
    {
      Rig software(rhi, asset, RigSettings{.unified = true, .software = true, .threshold = 1.0e9f}, "SwAll_");
      check(software.valid(), "the all-software rig builds");
      const FrameResult frame = settle(software, 3u);
      os::Logger::logf("  software: %s", describe(frame).c_str());
      check(frame.phaseOneSoftware + frame.phaseTwoSoftware > 0u && frame.phaseOneHardware + frame.phaseTwoHardware == 0u,
            "the culler routed every cluster to the software rasterizer");
      const Comparison c = compare(hardwareFrame, frame);
      os::Logger::logf("  %s", describe(c).c_str());
      {
        uint32_t nearer = 0u;
        uint32_t farther = 0u;
        uint32_t sameDepthOtherTriangle = 0u;
        std::ostringstream samples;
        uint32_t listed = 0u;
        for (uint32_t pixel = 0u; pixel < frame.pixels.size(); ++pixel)
        {
          const GBufferPixel &hw = hardwareFrame.pixels[pixel];
          const GBufferPixel &sw = frame.pixels[pixel];
          if (!hw.covered() || !sw.covered() || hw.hi == sw.hi)
            continue;
          float dh = 0.0f;
          float ds = 0.0f;
          std::memcpy(&dh, &hw.depthBits, sizeof(float));
          std::memcpy(&ds, &sw.depthBits, sizeof(float));
          if (std::fabs(ds - dh) <= 1.0e-5f + 1.0e-4f * std::fabs(dh))
            ++sameDepthOtherTriangle;
          else if (ds > dh)
            ++nearer;
          else
            ++farther;
          if (listed < 6u)
          {
            samples << "\n    (" << pixel % kImageSide << "," << pixel / kImageSide << ") hw d=" << dh
                    << " c=" << (hw.hi >> 7u) << " t=" << (hw.hi & 127u) << " | sw d=" << ds << " c=" << (sw.hi >> 7u)
                    << " t=" << (sw.hi & 127u);
            ++listed;
          }
        }
        os::Logger::logf("  mismatches: sameDepthOtherTriangle=%u swNearer=%u swFarther=%u%s", sameDepthOtherTriangle, nearer, farther, samples.str().c_str());
        // Per-cluster pixel counts: a cluster the software rasterizer never
        // draws shows up with zero pixels here.
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> perCluster;
        for (uint32_t pixel = 0u; pixel < frame.pixels.size(); ++pixel)
        {
          if (hardwareFrame.pixels[pixel].covered())
            ++perCluster[hardwareFrame.pixels[pixel].hi >> 7u].first;
          if (frame.pixels[pixel].covered())
            ++perCluster[frame.pixels[pixel].hi >> 7u].second;
        }
        std::ostringstream clusterText;
        uint32_t listedClusters = 0u;
        for (const auto &[cluster, counts] : perCluster)
          if (counts.second * 2u < counts.first && listedClusters++ < 12u)
            clusterText << " c" << cluster << "=" << counts.first << "/" << counts.second;
        if (!clusterText.str().empty())
          os::Logger::logf("  clusters with under half their hardware pixels in software:%s", clusterText.str().c_str());
        // Where the software depth pass itself lost the nearest surface, as
        // opposed to the payload pass or the merge losing it afterwards.
        uint32_t depthPassFarther = 0u;
        uint32_t depthPassMatches = 0u;
        uint32_t depthPassNearer = 0u;
        if (!frame.softwareDepth.empty())
          for (uint32_t pixel = 0u; pixel < frame.pixels.size(); ++pixel)
          {
            const GBufferPixel &hw = hardwareFrame.pixels[pixel];
            if (!hw.covered())
              continue;
            float dh = 0.0f;
            float ds = 0.0f;
            std::memcpy(&dh, &hw.depthBits, sizeof(float));
            std::memcpy(&ds, &frame.softwareDepth[pixel], sizeof(float));
            if (std::fabs(ds - dh) <= 1.0e-5f + 1.0e-4f * std::fabs(dh))
              ++depthPassMatches;
            else if (ds > dh)
              ++depthPassNearer;
            else
              ++depthPassFarther;
          }
        os::Logger::logf("  software depth pass vs hardware depth: matches=%u nearer=%u farther=%u", depthPassMatches, depthPassNearer, depthPassFarther);
        check(depthPassFarther == 0u && depthPassNearer == 0u,
              "the software depth pass alone reproduces the hardware depth on every covered pixel");
        // The same algorithm on the CPU: does it lose the same surfaces?
        const CpuRaster cpu = cpuRasterize(asset, software.pageTable(), frame.drawn, camera);
        uint32_t cpuMatchesHardware = 0u;
        uint32_t cpuFarther = 0u;
        uint32_t cpuNearer = 0u;
        uint32_t cpuEqualsGpuBits = 0u;
        uint32_t cpuCovered = 0u;
        std::ostringstream cpuSamples;
        uint32_t cpuListed = 0u;
        for (uint32_t pixel = 0u; pixel < frame.pixels.size(); ++pixel)
        {
          cpuCovered += cpu.depthBits[pixel] != 0u ? 1u : 0u;
          if (!frame.softwareDepth.empty() && cpu.depthBits[pixel] == frame.softwareDepth[pixel])
            ++cpuEqualsGpuBits;
          const GBufferPixel &hw = hardwareFrame.pixels[pixel];
          if (!hw.covered())
            continue;
          float dh = 0.0f;
          float dc = 0.0f;
          std::memcpy(&dh, &hw.depthBits, sizeof(float));
          std::memcpy(&dc, &cpu.depthBits[pixel], sizeof(float));
          if (std::fabs(dc - dh) <= 1.0e-5f + 1.0e-4f * std::fabs(dh))
            ++cpuMatchesHardware;
          else if (dc > dh)
            ++cpuNearer;
          else
          {
            ++cpuFarther;
            if (cpuListed < 4u)
            {
              cpuSamples << "\n    (" << pixel % kImageSide << "," << pixel / kImageSide << ") hw d=" << dh << " c=" << (hw.hi >> 7u)
                         << " t=" << (hw.hi & 127u) << " | cpu d=" << dc << " c=" << (cpu.payload[pixel] >> 7u) << " t=" << (cpu.payload[pixel] & 127u);
              ++cpuListed;
            }
          }
        }
        os::Logger::logf("  cpu replica vs hardware: matches=%u nearer=%u farther=%u | cpu==gpu depth bits on %u pixels, cpu covered=%u%s",
                         cpuMatchesHardware, cpuNearer, cpuFarther, cpuEqualsGpuBits, cpuCovered, cpuSamples.str().c_str());
        // Attribute the pixels the GPU lost to the CPU-winning triangle: a
        // triangle losing all of its pixels points at its thread, part of
        // them at the per-pixel loop.
        if (!frame.softwareDepth.empty())
        {
          std::map<uint32_t, std::pair<uint32_t, uint32_t>> perTriangle; // payload -> (cpu pixels, lost pixels)
          for (uint32_t pixel = 0u; pixel < frame.pixels.size(); ++pixel)
          {
            if (cpu.depthBits[pixel] == 0u)
              continue;
            auto &entry = perTriangle[cpu.payload[pixel]];
            ++entry.first;
            float dc = 0.0f;
            float dg = 0.0f;
            std::memcpy(&dc, &cpu.depthBits[pixel], sizeof(float));
            std::memcpy(&dg, &frame.softwareDepth[pixel], sizeof(float));
            if (dg < dc - (1.0e-5f + 1.0e-4f * std::fabs(dc)))
              ++entry.second;
          }
          uint32_t wholeLost = 0u;
          uint32_t partLost = 0u;
          std::ostringstream text;
          uint32_t listed = 0u;
          for (const auto &[payload, counts] : perTriangle)
          {
            if (counts.second == 0u)
              continue;
            if (counts.second == counts.first)
              ++wholeLost;
            else
              ++partLost;
            if (listed++ < 10u)
              text << " c" << (payload >> 7u) << "t" << (payload & 127u) << "=" << counts.second << "/" << counts.first;
          }
          if (wholeLost + partLost != 0u)
            os::Logger::logf("  lost triangles: whole=%u partial=%u%s", wholeLost, partLost, text.str().c_str());
          // Screen-space geometry of the first lost triangles.
          std::ostringstream geometryText;
          uint32_t geometryListed = 0u;
          const std::vector<PageTableEntry> table = software.pageTable();
          math::Mat4f viewProj = camera.projection * camera.view;
          for (const auto &[payload, counts] : perTriangle)
          {
            if (counts.second == 0u || geometryListed++ >= 3u)
              continue;
            const uint32_t globalCluster = payload >> 7u;
            const uint32_t triangle = payload & 127u;
            for (uint32_t page = 0u; page < table.size() && page < asset.build.pages.size(); ++page)
            {
              if (globalCluster < table[page].clusterOffset || globalCluster >= table[page].clusterOffset + table[page].clusterCount)
                continue;
              const uint32_t local = globalCluster - table[page].clusterOffset;
              const VirtualGeometryCluster &cluster = asset.build.clusters[asset.build.pages[page].clusterOffset + local];
              geometryText << "\n    c" << globalCluster << "t" << triangle << " (page " << page << " local " << local << ", "
                           << cluster.indices.size() / 3u << " tris, " << cluster.vertices.size() << " verts):";
              for (uint32_t corner = 0u; corner < 3u; ++corner)
              {
                const uint32_t index = cluster.indices[triangle * 3u + corner];
                float clip[4];
                transformPoint(viewProj, cluster.vertices[index].pos, clip);
                clip[1] = -clip[1];
                geometryText << " v" << index << "=(" << cluster.vertices[index].pos[0] << "," << cluster.vertices[index].pos[1] << ","
                             << cluster.vertices[index].pos[2] << ")->(" << (clip[0] / clip[3] * 0.5f + 0.5f) * kImageSide << ","
                             << (clip[1] / clip[3] * 0.5f + 0.5f) * kImageSide << ",z=" << clip[2] / clip[3] << ")";
              }
            }
          }
          if (wholeLost + partLost != 0u)
            os::Logger::logf("  lost triangle geometry:%s", geometryText.str().c_str());
        }
      }
      const uint32_t coverageDelta = c.coveredOnlyA + c.coveredOnlyB;
      check(coverageDelta * 100u <= c.coveredA,
            "software coverage differs from hardware coverage on at most 1% of pixels (" + std::to_string(coverageDelta) + ")");
      check(c.hiEqual * 100u >= c.coveredBoth * 99u,
            "the software rasterizer resolves the hardware triangle on at least 99% of shared pixels");
      check(c.depthClose * 100u >= c.coveredBoth * 99u,
            "software depth matches hardware depth on at least 99% of shared pixels");
      check(c.loEqual == c.coveredBoth && c.materialEqual == c.coveredBoth,
            "instance and material agree on every shared pixel");
      check(frame.drawn == hardwareFrame.drawn, "the software route draws the same cluster set");
    }

    // ── [mixed] ─────────────────────────────────────────────────────────────
    os::Logger::log("[mixed] clusters split between rasterizers merge into the hardware picture and settle");
    Rig mixed(rhi, asset, RigSettings{.unified = true, .software = true, .threshold = kMixedThreshold}, "Mixed_");
    check(mixed.valid(), "the mixed rig builds");
    FrameResult mixedFrame{};
    {
      std::vector<FrameResult> frames;
      constexpr uint32_t kSettleFrames = 24u;
      for (uint32_t frame = 0u; frame < kSettleFrames; ++frame)
        frames.push_back(mixed.run(camera));
      mixedFrame = frames.back();
      os::Logger::logf("  frame 0: %s", describe(frames[0]).c_str());
      os::Logger::logf("  settled: %s", describe(mixedFrame).c_str());
      check(mixedFrame.phaseOneSoftware > 0u && mixedFrame.phaseOneHardware > 0u,
            "the settled cut has clusters on both rasterizers");
      uint32_t unstable = 0u;
      uint32_t reemits = 0u;
      uint32_t duplicates = 0u;
      for (uint32_t frame = 2u; frame < kSettleFrames; ++frame)
      {
        if (frames[frame].hash != frames[2].hash || frames[frame].drawn != frames[2].drawn)
          ++unstable;
        if (frames[frame].phaseTwoHardware + frames[frame].phaseTwoSoftware != 0u)
          ++reemits;
        duplicates += frames[frame].duplicateEntries;
      }
      check(unstable == 0u, "the merged G-buffer and drawn set are identical on every settled frame (" + std::to_string(unstable) + " differed)");
      uint32_t advanceMismatches = 0u;
      for (uint32_t frame = 1u; frame < kSettleFrames; ++frame)
        if (frames[frame].phaseOneHardware + frames[frame].phaseOneSoftware != frames[frame].advanceCount)
          ++advanceMismatches;
      check(advanceMismatches == 0u,
            "phase one draws exactly the set the advance kernel produced on every frame (" + std::to_string(advanceMismatches) + " mismatches)");
      check(reemits == 0u, "phase two never re-emits once settled (" + std::to_string(reemits) + " frames)");
      check(duplicates == 0u, "no cluster is emitted by both rasterizers or both phases");
      const Comparison c = compare(hardwareFrame, mixedFrame);
      os::Logger::logf("  %s", describe(c).c_str());
      const uint32_t coverageDelta = c.coveredOnlyA + c.coveredOnlyB;
      check(coverageDelta * 100u <= c.coveredA,
            "mixed coverage differs from hardware coverage on at most 1% of pixels (" + std::to_string(coverageDelta) + ")");
      check(c.hiEqual * 100u >= c.coveredBoth * 99u, "the mixed picture resolves the hardware triangle on at least 99% of shared pixels");
      check(c.depthClose * 100u >= c.coveredBoth * 99u, "mixed depth matches hardware depth on at least 99% of shared pixels");
      check(mixedFrame.drawn == hardwareFrame.drawn, "the mixed route draws the same cluster set as hardware alone");
    }

    // ── [lod] ───────────────────────────────────────────────────────────────
    os::Logger::log("[lod] every cluster drawn by either rasterizer passes the projected-error cut test");
    {
      uint32_t violations = 0u;
      uint32_t tested = 0u;
      for (const uint64_t key : mixedFrame.drawn)
      {
        const uint32_t page = static_cast<uint32_t>(key >> 32u);
        const uint32_t local = static_cast<uint32_t>(key & 0xffffffffu);
        if (page >= asset.build.pages.size() || local >= asset.build.pages[page].clusterCount)
        {
          ++violations;
          continue;
        }
        const uint32_t cluster = asset.build.pages[page].clusterOffset + local;
        const VirtualGeometryCluster &data = asset.build.clusters[cluster];
        const float projSelf = projectedError(camera, data.self.center, data.self.radius, data.self.error, kNear, kImageSide);
        const bool root = data.parent.error == std::numeric_limits<float>::max();
        const float projParent = root ? std::numeric_limits<float>::max()
                                      : projectedError(camera, data.parent.center, data.parent.radius, data.parent.error, kNear, kImageSide);
        if (!(projSelf < kErrorBudget) || !(projParent >= kErrorBudget))
          ++violations;
        ++tested;
      }
      check(tested > 0u, "the mixed cut drew clusters to test");
      check(violations == 0u, "every drawn cluster satisfies projSelf < budget <= projParent (" + std::to_string(violations) + " violations)");
    }

    // ── [culling] ───────────────────────────────────────────────────────────
    os::Logger::log("[culling] with the software path on, culling removes clusters and never a pixel");
    {
      Rig noOcclusion(rhi, asset, RigSettings{.unified = true, .software = true, .threshold = kMixedThreshold, .occlusion = false}, "MixedNoOcc_");
      Rig noFrustum(rhi, asset, RigSettings{.unified = true, .software = true, .threshold = kMixedThreshold, .frustum = false}, "MixedNoFrustum_");
      check(noOcclusion.valid() && noFrustum.valid(), "the culling variant rigs build");
      const FrameResult occlusionOff = settle(noOcclusion, 3u);
      const FrameResult frustumOff = settle(noFrustum, 3u);
      os::Logger::logf("  occlusion off: %s", describe(occlusionOff).c_str());
      os::Logger::logf("  frustum off: %s", describe(frustumOff).c_str());
      check(occlusionOff.hash == mixedFrame.hash, "occlusion culling never changes the merged G-buffer");
      check(frustumOff.hash == mixedFrame.hash, "frustum culling never changes the merged G-buffer");
      check(frustumOff.drawnTotal() >= mixedFrame.drawnTotal(), "frustum culling never adds clusters");
    }

    // ── [replace] ───────────────────────────────────────────────────────────
    //
    // A chunk rebuild is unregister + install into fresh cluster slots, whose
    // visibility bits start clear: the frame after the install draws
    // everything in phase two, and the frame after that has to draw it all
    // in phase one again. Every one of those frames must cover the chunk.
    os::Logger::log("[replace] replacing the object under the unified path never leaves an empty frame");
    {
      Rig replace(rhi, asset, RigSettings{.unified = true, .software = true, .threshold = kMixedThreshold}, "Replace_", 2u);
      check(replace.valid(), "the replacement rig builds");
      replace.setReleaseDelay(2u);
      std::vector<FrameResult> base;
      for (uint32_t frame = 0u; frame < 4u; ++frame)
        base.push_back(replace.run(camera));
      const uint32_t baseCovered = base.back().covered;
      uint32_t emptyFrames = 0u;
      uint32_t holes = 0u;
      std::ostringstream text;
      for (uint32_t replacement = 0u; replacement < 4u; ++replacement)
      {
        check(replace.replaceObject(100u + replacement), "the object replaces into fresh ranges");
        text << "\n    replacement " << replacement << ":";
        for (uint32_t frame = 0u; frame < 4u; ++frame)
        {
          const FrameResult result = replace.run(camera);
          text << " [f+" << frame << " " << describe(result) << "]";
          if (result.covered == 0u)
            ++emptyFrames;
          if (result.covered < baseCovered)
            ++holes;
        }
      }
      os::Logger::logf("  base px=%u%s", baseCovered, text.str().c_str());
      check(emptyFrames == 0u, "no frame around a replacement comes back empty (" + std::to_string(emptyFrames) + " empty)");
      check(holes == 0u, "no frame around a replacement covers fewer pixels than the settled frame (" + std::to_string(holes) + " holes)");
    }

    // ── [grow] ──────────────────────────────────────────────────────────────
    //
    // The sample's shape of a refinement: several objects resident, new ones
    // installed beside them while an old one retires with a release delay,
    // frames in flight throughout. Every frame must keep covering at least
    // what the previous settled frame covered.
    os::Logger::log("[grow] installing and retiring objects with frames in flight never empties a frame");
    {
      Rig grow(rhi, asset, RigSettings{.unified = true, .software = true, .threshold = kMixedThreshold}, "Grow_", 2u);
      check(grow.valid(), "the growing rig builds");
      grow.setReleaseDelay(2u);
      std::vector<Rig::InFlight> queue;
      std::vector<FrameResult> results;
      const auto step = [&](const char *label)
      {
        queue.push_back(grow.recordFrame(camera));
        if (queue.size() == 2u)
        {
          results.push_back(grow.readFrame(queue.front()));
          queue.erase(queue.begin());
          os::Logger::logf("  %s: %s", label, describe(results.back()).c_str());
        }
      };
      for (uint32_t frame = 0u; frame < 4u; ++frame)
        step("settle");
      uint32_t emptyFrames = 0u;
      for (uint32_t round = 0u; round < 4u; ++round)
      {
        const float side = static_cast<float>(kSide);
        check(grow.installObject("grow_a" + std::to_string(round), 500u + round * 10u, math::Vec3f(-side, 0.0f, -side * static_cast<float>(round + 1u))), "a new object installs beside the first");
        check(grow.installObject("grow_b" + std::to_string(round), 505u + round * 10u, math::Vec3f(side, 0.0f, -side * static_cast<float>(round + 1u))), "a second new object installs beside the first");
        if (round > 0u)
          check(grow.unregisterObject("grow_a" + std::to_string(round - 1u)), "an older object retires with a release delay");
        for (uint32_t frame = 0u; frame < 3u; ++frame)
          step(("round " + std::to_string(round)).c_str());
      }
      while (!queue.empty())
      {
        results.push_back(grow.readFrame(queue.front()));
        queue.erase(queue.begin());
      }
      for (const auto &result : results)
        emptyFrames += result.covered == 0u ? 1u : 0u;
      check(emptyFrames == 0u, "no frame while growing and retiring comes back empty (" + std::to_string(emptyFrames) + " empty)");
      // A retired object's clusters keep their bits until the slots are
      // reused, so the advance set can exceed what phase one draws on the
      // frames after a retirement; it can never be smaller.
      uint32_t growAdvanceOverdraw = 0u;
      for (uint32_t frame = 1u; frame < results.size(); ++frame)
        if (results[frame].phaseOneHardware + results[frame].phaseOneSoftware > results[frame].advanceCount)
          ++growAdvanceOverdraw;
      check(growAdvanceOverdraw == 0u,
            "while growing, phase one never draws more than the advance set (" + std::to_string(growAdvanceOverdraw) + " frames did)");
    }

    // ── [pipelined] ─────────────────────────────────────────────────────────
    //
    // The sample keeps two frames in flight and records the next frame before
    // the previous one completes. Every frame of a static camera must still
    // resolve the same picture: a frame that comes back empty means a pass
    // read a slot the other frame was writing.
    os::Logger::log("[pipelined] two frames in flight resolve the identical picture on every frame");
    {
      Rig pipelined(rhi, asset, RigSettings{.unified = true, .software = true, .threshold = kMixedThreshold}, "Pipelined_", 2u);
      check(pipelined.valid(), "the two-frame unified rig builds");
      constexpr uint32_t kFrames = 30u;
      std::vector<FrameResult> results;
      std::vector<Rig::InFlight> queue;
      for (uint32_t frame = 0u; frame < kFrames; ++frame)
      {
        queue.push_back(pipelined.recordFrame(camera));
        if (queue.size() == 2u)
        {
          results.push_back(pipelined.readFrame(queue.front()));
          queue.erase(queue.begin());
        }
      }
      while (!queue.empty())
      {
        results.push_back(pipelined.readFrame(queue.front()));
        queue.erase(queue.begin());
      }
      uint32_t emptyFrames = 0u;
      uint32_t unstable = 0u;
      for (uint32_t frame = 3u; frame < results.size(); ++frame)
      {
        if (results[frame].covered == 0u)
          ++emptyFrames;
        if (results[frame].hash != results[3].hash)
          ++unstable;
      }
      os::Logger::logf("  frame 3: %s | last: %s", describe(results[3]).c_str(), describe(results.back()).c_str());
      check(results[3].covered == mixedFrame.covered, "the pipelined rig covers the same pixels as the single-frame rig");
      check(emptyFrames == 0u, "no pipelined frame comes back empty (" + std::to_string(emptyFrames) + " empty)");
      uint32_t pipelinedAdvanceMismatches = 0u;
      for (uint32_t frame = 1u; frame < results.size(); ++frame)
        if (results[frame].phaseOneHardware + results[frame].phaseOneSoftware != results[frame].advanceCount)
          ++pipelinedAdvanceMismatches;
      check(pipelinedAdvanceMismatches == 0u,
            "with frames in flight phase one still draws exactly the advance set (" + std::to_string(pipelinedAdvanceMismatches) + " mismatches)");
      check(unstable == 0u, "every pipelined frame resolves the identical G-buffer (" + std::to_string(unstable) + " differed)");
    }
  }
  catch (const std::exception &exception)
  {
    os::Logger::errorf("EXCEPTION: %s", exception.what());
    exitCode = 1;
  }

  delete rhi;
  if (failures != 0 || exitCode != 0)
  {
    os::Logger::errorf("%d sparse voxel VG visibility checks failed", failures);
    os::Logger::shutdown();
    return 1;
  }
  os::Logger::log("all sparse voxel VG visibility checks passed");
  os::Logger::shutdown();
  return 0;
}
