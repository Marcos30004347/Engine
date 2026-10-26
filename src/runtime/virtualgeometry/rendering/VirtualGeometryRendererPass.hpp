#pragma once

#include <algorithm>
#include <memory>

#include "os/Logger.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualgeometry/rendering/VirtualGeometryCullingMultipleDispatchesPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryComputeRasterPass.hpp"
#include <cstdlib>
#include "virtualgeometry/rendering/VirtualGeometryDepthPrePassDrawPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryDepthPyramidPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryHardwareDrawPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryPageDecodePass.hpp"
#include "virtualgeometry/rendering/MaterialPass.hpp"
#include "virtualgeometry/rendering/ScreenSpaceRayTracePass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryStreamingPageSelectionPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometrySoftwareRasterPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryVisibilityMergePass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryVisibilityResolvePass.hpp"

namespace virtualgeometry
{
namespace gpgpu
{

class VirtualGeometryRendererPass : public rendering::Pass
{
public:
  struct Settings
  {
    VirtualGeometryCullingMultipleDispatchesPass::Settings prepassCullingSettings;
    VirtualGeometryDepthPrePassDrawPass::Settings depthPrePassSettings;
    VirtualGeometryDepthPyramidPass::Settings depthPyramidSettings;
    VirtualGeometryCullingMultipleDispatchesPass::Settings finalCullingSettings;
    VirtualGeometryHardwareDrawPass::Settings hardwareDrawSettings;
    MaterialPass::Settings materialPassSettings;
    ScreenSpaceRayTracePass::Settings screenSpaceRayTraceSettings;
    VirtualGeometryComputeRasterPass::Settings computeRasterSettings;
    // Render only the visibility prepass at a reduced resolution, then build
    // the current-frame HZB from that smaller depth target. LOD/error math and
    // final hardware draw remain at the full viewport resolution.
    uint32_t prepassResolutionDivisor = 2u;
    // Two-pass occlusion, Nanite-style. Phase one redraws the clusters that
    // were drawn last frame with the current camera into the real target;
    // phase two draws what survives the pyramid built from that depth and was
    // not already drawn. Both phases write the full payload, so the frame is
    // rasterized once rather than once for depth and again for the G-buffer,
    // and prepassResolutionDivisor stops applying - phase one's pixels are
    // kept, so they have to be full resolution.
    bool twoPhaseOcclusion = false;
    bool registerScreenSpaceRayTracePass = true;
    bool registerMaterialPass = true;
    // The unified visibility path: every rasterizer's output is merged into
    // one 64-bit key per pixel (virtualgeometry-visibility.wgsl) and a
    // fullscreen resolve writes the whole G-buffer from it, so a pixel drawn
    // by the hardware path and one drawn by the software rasterizer are
    // indistinguishable downstream. Required by the software rasterizer.
    bool unifiedVisibility = false;
    struct SoftwareRaster
    {
      bool enabled = false;
      // Clusters whose average projected triangle edge is under this many
      // pixels are rasterized in compute.
      float projectedTriangleEdgePixelThreshold = 32.0f;
    } softwareRaster;
  };

  // Child passes this pass registers for the given settings; callers that
  // hand out pass indices sequentially advance by this much.
  static uint32_t childPassCount(const Settings &settings)
  {
    // pageDecode, preCulling, (phaseOneDraw | depthPrePass), depthPyramid,
    // finalCulling, streamingSelection, hardwareDraw.
    uint32_t count = 7u;
    const bool unified = settings.unifiedVisibility || settings.softwareRaster.enabled;
    if (unified)
    {
      // merge + resolve after the final draw, and after phase one too.
      count += 2u;
      if (settings.twoPhaseOcclusion)
        count += 2u;
      if (settings.softwareRaster.enabled)
        count += settings.twoPhaseOcclusion ? 2u : 1u;
    }
    if (settings.computeRasterSettings.enabled)
      ++count;
    if (settings.registerScreenSpaceRayTracePass)
      ++count;
    if (settings.registerMaterialPass)
      count += 3u;
    return count;
  }

  VirtualGeometryRendererPass(
      VirtualGeometryScene &scene,
      VirtualGeometryHardwareDrawPass::FrameTarget frameTarget,
      VirtualGeometryDepthPrePassDrawPass::FrameTarget prePassTarget,
      rendering::Texture shadowLightingTexture,
      rendering::Texture previousDepthHZBTexture,
      float clearDepth,
      Settings settings)
      : scene_(scene), frameTarget_(frameTarget), prePassTarget_(prePassTarget), shadowLightingTexture_(shadowLightingTexture), previousDepthHZBTexture_(previousDepthHZBTexture), clearDepth_(clearDepth),
        settings_(settings)
  {
  }

  ~VirtualGeometryRendererPass() override
  {
    delete materialDispatchPass_;
    materialDispatchPass_ = nullptr;
    delete materialPreparePass_;
    materialPreparePass_ = nullptr;
    delete materialSetupPass_;
    materialSetupPass_ = nullptr;
    materialPass_.reset();
    delete hardwareDrawPass_;
    hardwareDrawPass_ = nullptr;
    delete visibilityResolvePass_;
    visibilityResolvePass_ = nullptr;
    delete visibilityMergePass_;
    visibilityMergePass_ = nullptr;
    delete softwareRasterPass_;
    softwareRasterPass_ = nullptr;
    delete phaseOneVisibilityResolvePass_;
    phaseOneVisibilityResolvePass_ = nullptr;
    delete phaseOneVisibilityMergePass_;
    phaseOneVisibilityMergePass_ = nullptr;
    delete phaseOneSoftwareRasterPass_;
    phaseOneSoftwareRasterPass_ = nullptr;
    delete computeRasterPass_;
    computeRasterPass_ = nullptr;
    delete screenSpaceRayTracePass_;
    screenSpaceRayTracePass_ = nullptr;
    delete pageDecodePass_;
    pageDecodePass_ = nullptr;
    delete streamingPageSelectionPass_;
    streamingPageSelectionPass_ = nullptr;
    delete finalCullingPass_;
    finalCullingPass_ = nullptr;
    delete depthPyramidPass_;
    depthPyramidPass_ = nullptr;
    delete depthPrePass_;
    depthPrePass_ = nullptr;
    delete preCullingPass_;
    preCullingPass_ = nullptr;
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    uint32_t childIndex = getIndex() + 1u;
    auto prepassCullingSettings = settings_.prepassCullingSettings;
    auto finalCullingSettings = settings_.finalCullingSettings;
    auto depthPrePassSettings = settings_.depthPrePassSettings;
    auto depthPyramidSettings = settings_.depthPyramidSettings;
    prepassCullingSettings.enableStreamingPriorityUpdates = false;
    finalCullingSettings.enableStreamingPriorityUpdates = true;
    const bool unifiedVisibility =
        settings_.unifiedVisibility || settings_.softwareRaster.enabled;
    const bool softwareRaster = settings_.softwareRaster.enabled;
    // Diagnostics: VG_UNIFIED_SKIP_PHASE_ONE_RESOLVE leaves phase one's
    // attachments as the hardware draw wrote them (the pyramid then sees no
    // software surfaces); VG_UNIFIED_SKIP_FINAL_RESOLVE leaves the final
    // attachments as drawn.
    const bool skipPhaseOneResolve = std::getenv("VG_UNIFIED_SKIP_PHASE_ONE_RESOLVE") != nullptr;
    const bool skipFinalResolve = std::getenv("VG_UNIFIED_SKIP_FINAL_RESOLVE") != nullptr;
    prepassCullingSettings.enableSoftwareRaster = false;
    finalCullingSettings.enableSoftwareRaster =
        VirtualGeometryComputeRasterPass::canRouteSoftwareClusters(
            settings_.computeRasterSettings);
    if (finalCullingSettings.enableSoftwareRaster)
      finalCullingSettings.swTriangleEdgePixelThreshold =
          settings_.computeRasterSettings.projectedTriangleEdgePixelThreshold;
    if (softwareRaster)
    {
      prepassCullingSettings.metalExportableSoftwareRasterBuffers = scene_.settings.metalExportableBuffers;
      finalCullingSettings.metalExportableSoftwareRasterBuffers = scene_.settings.metalExportableBuffers;
      // Both phases route small clusters to the compute rasterizer; each
      // phase has its own rasterizer instance and buffers.
      prepassCullingSettings.enableSoftwareRaster = true;
      prepassCullingSettings.swTriangleEdgePixelThreshold =
          settings_.softwareRaster.projectedTriangleEdgePixelThreshold;
      prepassCullingSettings.duplicateSoftwareRasterClustersToHardware = false;
      finalCullingSettings.enableSoftwareRaster = true;
      finalCullingSettings.swTriangleEdgePixelThreshold =
          settings_.softwareRaster.projectedTriangleEdgePixelThreshold;
      finalCullingSettings.duplicateSoftwareRasterClustersToHardware = false;
    }

    const uint32_t fullWidth =
        std::max(1u, settings_.hardwareDrawSettings.viewPortWidth);
    const uint32_t fullHeight =
        std::max(1u, settings_.hardwareDrawSettings.viewPortHeight);
    const bool twoPhase = settings_.twoPhaseOcclusion;
    if (twoPhase)
    {
      prepassCullingSettings.twoPhaseOcclusion = true;
      prepassCullingSettings.phaseTwo = false;
      // Phase one exists to produce this frame's depth. Testing it against last
      // frame's pyramid would defeat the point, and the set it draws is already
      // bounded to what was visible.
      prepassCullingSettings.enableOcclusionCulling = false;
      finalCullingSettings.twoPhaseOcclusion = true;
      finalCullingSettings.phaseTwo = true;
    }
    const uint32_t divisor =
        twoPhase ? 1u : std::max(1u, settings_.prepassResolutionDivisor);
    const uint32_t prepassWidth = ceilDiv(fullWidth, divisor);
    const uint32_t prepassHeight = ceilDiv(fullHeight, divisor);
    resolvedPrepassWidth_ = prepassWidth;
    resolvedPrepassHeight_ = prepassHeight;
    resolvedPrepassHZBMipCount_ =
        depthPyramidSettings.mipCount == 0u
        ? computeMipCount(prepassWidth, prepassHeight)
        : std::min(
              depthPyramidSettings.mipCount,
              computeMipCount(prepassWidth, prepassHeight));
    resolvedPrepassHZBMipCount_ = std::max(1u, resolvedPrepassHZBMipCount_);

    VirtualGeometryDepthPrePassDrawPass::FrameTarget activePrePassTarget =
        prePassTarget_;
    activeDepthHZBTexture_ = previousDepthHZBTexture_;
    if (divisor > 1u)
    {
      activePrePassTarget.depthTexture = createFrameLocalTexture(
          rendering::TextureInfo{
            .name = passName + ".reducedPreDepth.texture",
            .format = depthPrePassSettings.depthFormat,
            .memoryProperties = rendering::BufferUsage::BufferUsage_None,
            .usage = rendering::ImageUsage::ImageUsage_DepthStencilAttachment |
                rendering::ImageUsage::ImageUsage_Sampled,
            .width = prepassWidth,
            .height = prepassHeight,
            .depth = 1u,
            .mipLevels = 1u,
            .frameLocal = true,
          });
      activePrePassTarget.depthView = rendering::TextureView{
        .texture = activePrePassTarget.depthTexture,
        .index = 0u,
        .flags = rendering::ImageAspectFlags::Depth,
        .baseMipLevel = 0u,
        .levelCount = 1u,
        .baseArrayLayer = 0u,
        .layerCount = 1u,
        .access = rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE,
        .layout = rendering::ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
      };
      activeDepthHZBTexture_ = createFrameLocalTexture(
          rendering::TextureInfo{
            .name = passName + ".reducedHZB.texture",
            .format = depthPyramidSettings.outputFormat,
            .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
            .usage = rendering::ImageUsage::ImageUsage_Storage |
                rendering::ImageUsage::ImageUsage_Sampled,
            .width = prepassWidth,
            .height = prepassHeight,
            .depth = 1u,
            .mipLevels = resolvedPrepassHZBMipCount_,
            .frameLocal = true,
          });
      depthPrePassSettings.viewPortWidth = prepassWidth;
      depthPrePassSettings.viewPortHeight = prepassHeight;
      depthPyramidSettings.width = prepassWidth;
      depthPyramidSettings.height = prepassHeight;
      depthPyramidSettings.outputWidth = prepassWidth;
      depthPyramidSettings.outputHeight = prepassHeight;
      depthPyramidSettings.mipCount = resolvedPrepassHZBMipCount_;
      finalCullingSettings.hiZMipLevels = resolvedPrepassHZBMipCount_;
      os::Logger::logf(
          "Virtual Geometry low-resolution prepass: pass=%s full=%ux%u "
          "prepass=%ux%u divisor=%u hzbMips=%u",
          passName.c_str(),
          fullWidth,
          fullHeight,
          prepassWidth,
          prepassHeight,
          divisor,
          resolvedPrepassHZBMipCount_);
    }
    const bool prepassWritesFinalDepth =
        activePrePassTarget.depthTexture == frameTarget_.depthTexture;
    const rendering::LoadOp hardwareDepthLoadOp =
        prepassWritesFinalDepth
        ? rendering::LoadOp::LoadOp_Load
        : rendering::LoadOp::LoadOp_Clear;

    // Keep the composite VG chain in the caller's submission recorder. Page
    // decode, pre-cull/depth, final cull, streaming selection, draw, and the
    // optional material/ray passes all consume each other's freshly written
    // buffers/textures. Splitting them across hidden recorders made external
    // consumers, such as sparse-voxel G-buffer adapters, depend on coarse
    // command-buffer ordering instead of the normal render-graph resource
    // chain.
    if (twoPhase)
    {
      // Contract line for the two-pass chain, replacing the low-resolution
      // prepass contract it retires: both phases draw the real target, so the
      // first one is full resolution by construction.
      os::Logger::logf(
          "Virtual Geometry two-pass occlusion: pass=%s full=%ux%u hzbMips=%u",
          passName.c_str(),
          fullWidth,
          fullHeight,
          resolvedPrepassHZBMipCount_);
    }

    pageDecodePass_ = renderGraph->registerPass<VirtualGeometryPageDecodePass>(passName + ".pageDecode", childIndex++, commandBuffer, scene_);

    preCullingPass_ = renderGraph->registerPass<VirtualGeometryCullingMultipleDispatchesPass>(passName + ".preCulling", childIndex++, commandBuffer, scene_, prepassCullingSettings, previousDepthHZBTexture_);

    if (twoPhase)
    {
      // Phase one consumes the pre-culling pass's indirect arguments and
      // visible list the moment they are written; the same conservative
      // barrier the final draw gets, for the same backends.
      commandBuffer.cmdMemoryBarrier(
          rendering::PipelineStage::ALL_COMMANDS,
          rendering::PipelineStage::ALL_COMMANDS,
          rendering::AccessPattern::SHADER_WRITE |
              rendering::AccessPattern::MEMORY_WRITE,
          rendering::AccessPattern::MEMORY_READ |
              rendering::AccessPattern::SHADER_READ |
              rendering::AccessPattern::INDIRECT_COMMAND_READ);
      // Phase one is a full draw into the real target, not a throwaway depth
      // pass: its pixels are the ones phase two builds on.
      phaseOneDrawPass_ =
          renderGraph->registerPass<VirtualGeometryHardwareDrawPass>(
              passName + ".phaseOneDraw",
              childIndex++,
              commandBuffer,
              scene_,
              preCullingPass_->getHWDrawIndirectBuffer(),
              preCullingPass_->getHWVisibleClusterInfosBuffer(),
              preCullingPass_->getCullingStatisticsBuffer(),
              frameTarget_,
              rendering::LoadOp::LoadOp_Clear,
              rendering::LoadOp::LoadOp_Clear,
              clearDepth_,
              settings_.hardwareDrawSettings);
      if (unifiedVisibility && !skipPhaseOneResolve)
      {
        if (softwareRaster)
        {
          phaseOneSoftwareRasterPass_ =
              renderGraph->registerPass<VirtualGeometrySoftwareRasterPass>(
                  passName + ".phaseOneSoftwareRaster",
                  childIndex++,
                  commandBuffer,
                  scene_,
                  preCullingPass_->getUniformsBuffer(),
                  preCullingPass_->getCullingStatisticsBuffer(),
                  preCullingPass_->getSWVisibleClusterInfosBuffer(),
                  preCullingPass_->getSWDispatchArgsBuffer(),
                  VirtualGeometrySoftwareRasterPass::Settings{
                    .viewportWidth = fullWidth,
                    .viewportHeight = fullHeight,
                  });
        }
      // The merge samples the depth and payload attachments the draw just
      // wrote, and on some backends the graph's own transition is not enough
      // for a compute read of an attachment written by the previous pass.
      commandBuffer.cmdMemoryBarrier(
          rendering::PipelineStage::ALL_COMMANDS,
          rendering::PipelineStage::ALL_COMMANDS,
          rendering::AccessPattern::SHADER_WRITE |
              rendering::AccessPattern::MEMORY_WRITE |
              rendering::AccessPattern::COLOR_ATTACHMENT_WRITE |
              rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE,
          rendering::AccessPattern::MEMORY_READ |
              rendering::AccessPattern::SHADER_READ);
        VirtualGeometryVisibilityMergePass::SoftwareInputs phaseOneInputs{};
        if (phaseOneSoftwareRasterPass_ != nullptr)
        {
          phaseOneInputs.depthA = phaseOneSoftwareRasterPass_->getDepthBuffer();
          phaseOneInputs.payloadA = phaseOneSoftwareRasterPass_->getPayloadBuffer();
        }
        phaseOneVisibilityMergePass_ =
            renderGraph->registerPass<VirtualGeometryVisibilityMergePass>(
                passName + ".phaseOneVisibilityMerge",
                childIndex++,
                commandBuffer,
                frameTarget_.depthTexture,
                frameTarget_.packedGeometryIdsHiView.texture,
                phaseOneInputs,
                VirtualGeometryVisibilityMergePass::Settings{
                  .viewportWidth = fullWidth,
                  .viewportHeight = fullHeight,
                  .softwarePhases = softwareRaster ? 1u : 0u,
                });
        // Phase one's depth is what the pyramid is built from, so the
        // software surfaces have to be in the depth attachment before it.
        phaseOneVisibilityResolvePass_ =
            renderGraph->registerPass<VirtualGeometryVisibilityResolvePass>(
                passName + ".phaseOneVisibilityResolve",
                childIndex++,
                commandBuffer,
                scene_,
                phaseOneVisibilityMergePass_->getVisibilityKeysBuffer(),
                frameTarget_,
                VirtualGeometryVisibilityResolvePass::Settings{
                  .viewportWidth = fullWidth,
                  .viewportHeight = fullHeight,
                  .motionVectorFormat = settings_.hardwareDrawSettings.motionVectorFormat,
                  .depthFormat = settings_.hardwareDrawSettings.depthFormat,
                });
      }
    }
    else
    {
      depthPrePass_ = renderGraph->registerPass<VirtualGeometryDepthPrePassDrawPass>(
          passName + ".depthPrePass",
          childIndex++,
          commandBuffer,
          scene_,
          preCullingPass_->getHWDrawIndirectBuffer(),
          preCullingPass_->getHWVisibleClusterInfosBuffer(),
          preCullingPass_->getCullingStatisticsBuffer(),
          activePrePassTarget,
          rendering::LoadOp::LoadOp_Clear,
          clearDepth_,
          depthPrePassSettings);
    }

    if (phaseOneVisibilityResolvePass_ != nullptr)
    {
      // The pyramid and phase two consume the depth the resolve just wrote.
      commandBuffer.cmdMemoryBarrier(
          rendering::PipelineStage::ALL_COMMANDS,
          rendering::PipelineStage::ALL_COMMANDS,
          rendering::AccessPattern::COLOR_ATTACHMENT_WRITE |
              rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE |
              rendering::AccessPattern::MEMORY_WRITE,
          rendering::AccessPattern::MEMORY_READ |
              rendering::AccessPattern::SHADER_READ |
              rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ);
    }
    depthPyramidPass_ =
        renderGraph->registerPass<VirtualGeometryDepthPyramidPass>(
            passName + ".depthPyramid",
            childIndex++,
            commandBuffer,
            twoPhase ? frameTarget_.depthTexture
                     : activePrePassTarget.depthTexture,
            activeDepthHZBTexture_,
            depthPyramidSettings);

    finalCullingPass_ = renderGraph->registerPass<VirtualGeometryCullingMultipleDispatchesPass>(passName + ".finalCulling", childIndex++, commandBuffer, scene_, finalCullingSettings, activeDepthHZBTexture_);

    streamingPageSelectionPass_ = renderGraph->registerPass<VirtualGeometryStreamingPageSelectionPass>(passName + ".streamingSelection", childIndex++, commandBuffer, scene_);

    if (settings_.computeRasterSettings.enabled)
    {
      auto computeRasterSettings = settings_.computeRasterSettings;
      computeRasterSettings.viewportWidth = fullWidth;
      computeRasterSettings.viewportHeight = fullHeight;
      computeRasterPass_ =
          renderGraph->registerPass<VirtualGeometryComputeRasterPass>(
              passName + ".computeRaster",
              childIndex++,
              commandBuffer,
              scene_,
              finalCullingPass_->getSWDrawIndirectBuffer(),
              finalCullingPass_->getSWVisibleClusterInfosBuffer(),
              finalCullingPass_->getCullingStatisticsBuffer(),
              finalCullingPass_->getUniformsBuffer(),
              frameTarget_,
              computeRasterSettings);
    }

    // Generated/runtime-published VG pages can be decoded, culled, and drawn
    // in the same graph frame. The graph also records per-resource barriers,
    // but keep a conservative pass-chain barrier here so the hardware draw
    // never observes stale decoded buffers, visible-cluster records, or
    // no-DrawIndirectCount draw arguments on backends with weaker indirect
    // argument visibility (notably MoltenVK-on-Metal).
    commandBuffer.cmdMemoryBarrier(
        rendering::PipelineStage::ALL_COMMANDS,
        rendering::PipelineStage::ALL_COMMANDS,
        rendering::AccessPattern::SHADER_WRITE |
            rendering::AccessPattern::MEMORY_WRITE,
        rendering::AccessPattern::MEMORY_READ |
            rendering::AccessPattern::SHADER_READ |
            rendering::AccessPattern::INDIRECT_COMMAND_READ);

    hardwareDrawPass_ = renderGraph->registerPass<VirtualGeometryHardwareDrawPass>(
        passName + ".hardwareDraw",
        childIndex++,
        commandBuffer,
        scene_,
        finalCullingPass_->getHWDrawIndirectBuffer(),
        finalCullingPass_->getHWVisibleClusterInfosBuffer(),
        finalCullingPass_->getCullingStatisticsBuffer(),
        frameTarget_,
        twoPhase ? rendering::LoadOp::LoadOp_Load : hardwareDepthLoadOp,
        twoPhase ? rendering::LoadOp::LoadOp_Load
                 : rendering::LoadOp::LoadOp_Clear,
        clearDepth_,
        settings_.hardwareDrawSettings);

    if (unifiedVisibility && !skipFinalResolve)
    {
      if (softwareRaster)
      {
        softwareRasterPass_ =
            renderGraph->registerPass<VirtualGeometrySoftwareRasterPass>(
                passName + ".softwareRaster",
                childIndex++,
                commandBuffer,
                scene_,
                finalCullingPass_->getUniformsBuffer(),
                finalCullingPass_->getCullingStatisticsBuffer(),
                finalCullingPass_->getSWVisibleClusterInfosBuffer(),
                finalCullingPass_->getSWDispatchArgsBuffer(),
                VirtualGeometrySoftwareRasterPass::Settings{
                  .viewportWidth = fullWidth,
                  .viewportHeight = fullHeight,
                });
      }
      // The merge samples the depth and payload attachments the draw just
      // wrote, and on some backends the graph's own transition is not enough
      // for a compute read of an attachment written by the previous pass.
      commandBuffer.cmdMemoryBarrier(
          rendering::PipelineStage::ALL_COMMANDS,
          rendering::PipelineStage::ALL_COMMANDS,
          rendering::AccessPattern::SHADER_WRITE |
              rendering::AccessPattern::MEMORY_WRITE |
              rendering::AccessPattern::COLOR_ATTACHMENT_WRITE |
              rendering::AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE,
          rendering::AccessPattern::MEMORY_READ |
              rendering::AccessPattern::SHADER_READ);
      VirtualGeometryVisibilityMergePass::SoftwareInputs inputs{};
      uint32_t softwarePhases = 0u;
      if (phaseOneSoftwareRasterPass_ != nullptr)
      {
        inputs.depthA = phaseOneSoftwareRasterPass_->getDepthBuffer();
        inputs.payloadA = phaseOneSoftwareRasterPass_->getPayloadBuffer();
        ++softwarePhases;
      }
      if (softwareRasterPass_ != nullptr)
      {
        if (softwarePhases == 0u)
        {
          inputs.depthA = softwareRasterPass_->getDepthBuffer();
          inputs.payloadA = softwareRasterPass_->getPayloadBuffer();
        }
        else
        {
          inputs.depthB = softwareRasterPass_->getDepthBuffer();
          inputs.payloadB = softwareRasterPass_->getPayloadBuffer();
        }
        ++softwarePhases;
      }
      visibilityMergePass_ =
          renderGraph->registerPass<VirtualGeometryVisibilityMergePass>(
              passName + ".visibilityMerge",
              childIndex++,
              commandBuffer,
              frameTarget_.depthTexture,
              frameTarget_.packedGeometryIdsHiView.texture,
              inputs,
              VirtualGeometryVisibilityMergePass::Settings{
                .viewportWidth = fullWidth,
                .viewportHeight = fullHeight,
                .softwarePhases = softwarePhases,
              });
      visibilityResolvePass_ =
          renderGraph->registerPass<VirtualGeometryVisibilityResolvePass>(
              passName + ".visibilityResolve",
              childIndex++,
              commandBuffer,
              scene_,
              visibilityMergePass_->getVisibilityKeysBuffer(),
              frameTarget_,
              VirtualGeometryVisibilityResolvePass::Settings{
                .viewportWidth = fullWidth,
                .viewportHeight = fullHeight,
                .motionVectorFormat = settings_.hardwareDrawSettings.motionVectorFormat,
                .depthFormat = settings_.hardwareDrawSettings.depthFormat,
              });
    }

    if (settings_.registerScreenSpaceRayTracePass)
    {
      auto screenSpaceRayTraceSettings = settings_.screenSpaceRayTraceSettings;
      if (screenSpaceRayTraceSettings.width == 0u)
        screenSpaceRayTraceSettings.width = settings_.hardwareDrawSettings.viewPortWidth;
      if (screenSpaceRayTraceSettings.height == 0u)
        screenSpaceRayTraceSettings.height = settings_.hardwareDrawSettings.viewPortHeight;
      screenSpaceRayTraceSettings.hzbWidth =
          depthPyramidSettings.outputWidth != 0u
          ? depthPyramidSettings.outputWidth
          : prepassWidth;
      screenSpaceRayTraceSettings.hzbHeight =
          depthPyramidSettings.outputHeight != 0u
          ? depthPyramidSettings.outputHeight
          : prepassHeight;
      screenSpaceRayTraceSettings.hzbMipCount =
          screenSpaceRayTraceSettings.hzbMipCount == 0u
          ? resolvedPrepassHZBMipCount_
          : std::min(
                screenSpaceRayTraceSettings.hzbMipCount,
                resolvedPrepassHZBMipCount_);
      screenSpaceRayTracePass_ = renderGraph->registerPass<ScreenSpaceRayTracePass>(
          passName + ".screenSpaceRayTrace",
          childIndex++,
          commandBuffer,
          frameTarget_.depthTexture,
          frameTarget_.geometryNormalTexture,
          activeDepthHZBTexture_,
          screenSpaceRayTraceSettings);
    }

    if (settings_.registerMaterialPass)
    {
      materialPass_ = std::make_unique<MaterialPass>(scene_, frameTarget_, shadowLightingTexture_, settings_.materialPassSettings);
      materialPass_->initialize(*renderGraph, passName + ".material");
      materialSetupPass_ = renderGraph->registerPass<MaterialSetupPass>(passName + ".material.setup", childIndex++, commandBuffer, *materialPass_);
      materialPreparePass_ = renderGraph->registerPass<MaterialPreparePass>(passName + ".material.prepare", childIndex++, commandBuffer, *materialPass_);
      materialDispatchPass_ = renderGraph->registerPass<MaterialDispatchPass>(passName + ".material.dispatch", childIndex++, commandBuffer, *materialPass_);
    }

  }

  VirtualGeometryCullingMultipleDispatchesPass *getPreCullingPass() const
  {
    return preCullingPass_;
  }
  VirtualGeometryDepthPrePassDrawPass *getDepthPrePass() const
  {
    return depthPrePass_;
  }
  VirtualGeometryDepthPyramidPass *getDepthPyramidPass() const
  {
    return depthPyramidPass_;
  }
  VirtualGeometryCullingMultipleDispatchesPass *getFinalCullingPass() const
  {
    return finalCullingPass_;
  }
  VirtualGeometryHardwareDrawPass *getHardwareDrawPass() const
  {
    return hardwareDrawPass_;
  }
  // Phase one of the two-pass occlusion cut. It is a full hardware draw and
  // needs the same per-frame uniforms as phase two - without them it
  // rasterizes with an identity transform and silently produces no pixels.
  VirtualGeometryHardwareDrawPass *getPhaseOneDrawPass() const
  {
    return phaseOneDrawPass_;
  }
  VirtualGeometryComputeRasterPass *getComputeRasterPass() const
  {
    return computeRasterPass_;
  }
  VirtualGeometrySoftwareRasterPass *getSoftwareRasterPass() const
  {
    return softwareRasterPass_;
  }
  VirtualGeometrySoftwareRasterPass *getPhaseOneSoftwareRasterPass() const
  {
    return phaseOneSoftwareRasterPass_;
  }
  VirtualGeometryVisibilityMergePass *getVisibilityMergePass() const
  {
    return visibilityMergePass_;
  }
  VirtualGeometryVisibilityMergePass *getPhaseOneVisibilityMergePass() const
  {
    return phaseOneVisibilityMergePass_;
  }
  // The resolve passes carry the same per-frame uniforms as the draws and
  // must be updated alongside them.
  VirtualGeometryVisibilityResolvePass *getVisibilityResolvePass() const
  {
    return visibilityResolvePass_;
  }
  VirtualGeometryVisibilityResolvePass *getPhaseOneVisibilityResolvePass() const
  {
    return phaseOneVisibilityResolvePass_;
  }
  MaterialPass *getMaterialPass() const
  {
    return materialPass_.get();
  }
  ScreenSpaceRayTracePass *getScreenSpaceRayTracePass() const
  {
    return screenSpaceRayTracePass_;
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    scene_.appendFrameOverrides(overrides, frameSlot);
    if (preCullingPass_ != nullptr)
      preCullingPass_->appendFrameOverrides(overrides, frameSlot);
    if (depthPrePass_ != nullptr)
      depthPrePass_->appendFrameOverrides(overrides, frameSlot);
    if (phaseOneDrawPass_ != nullptr)
      phaseOneDrawPass_->appendFrameOverrides(overrides, frameSlot);
    if (finalCullingPass_ != nullptr)
      finalCullingPass_->appendFrameOverrides(overrides, frameSlot);
    if (hardwareDrawPass_ != nullptr)
      hardwareDrawPass_->appendFrameOverrides(overrides, frameSlot);
    if (visibilityResolvePass_ != nullptr)
      visibilityResolvePass_->appendFrameOverrides(overrides, frameSlot);
    if (phaseOneVisibilityResolvePass_ != nullptr)
      phaseOneVisibilityResolvePass_->appendFrameOverrides(overrides, frameSlot);
    if (screenSpaceRayTracePass_ != nullptr)
      screenSpaceRayTracePass_->appendFrameOverrides(overrides, frameSlot);
    if (materialPass_ != nullptr)
      materialPass_->appendFrameOverrides(overrides, frameSlot);
  }

  VirtualGeometryStreamingPageSelectionPass *getStreamingPageSelectionPass() const
  {
    return streamingPageSelectionPass_;
  }
  rendering::CommandRecorder &getCompositeCommandRecorder()
  {
    return getCommandBuffer();
  }
  const rendering::Texture &getMaterialIdTexture() const
  {
    return frameTarget_.materialIdView.texture;
  }
  const rendering::Texture &getColorTexture() const
  {
    return frameTarget_.colorView.texture;
  }
  const rendering::Texture &getBaseColorTexture() const
  {
    return frameTarget_.baseColorTexture;
  }
  const rendering::Texture &getGeometryNormalTexture() const
  {
    return frameTarget_.geometryNormalTexture;
  }
  const rendering::Texture &getNormalTexture() const
  {
    return frameTarget_.normalTexture;
  }
  const rendering::Texture &getSceneDepthTexture() const
  {
    return frameTarget_.depthTexture;
  }
  const rendering::Texture &getMotionVectorTexture() const
  {
    return frameTarget_.motionVectorTexture;
  }
  const rendering::Texture &getPrepassDepthHZBTexture() const
  {
    return activeDepthHZBTexture_;
  }
  uint32_t getPrepassDepthHZBMipCount() const
  {
    return resolvedPrepassHZBMipCount_;
  }
  const rendering::Texture &getScreenSpaceRayTraceStepTexture() const
  {
    if (screenSpaceRayTracePass_ != nullptr)
      return screenSpaceRayTracePass_->getStepCountTexture();
    return frameTarget_.colorTexture;
  }
private:
  static uint32_t ceilDiv(uint32_t value, uint32_t divisor)
  {
    return (value + divisor - 1u) / divisor;
  }

  static uint32_t computeMipCount(uint32_t width, uint32_t height)
  {
    uint32_t maxDim = std::max(width, height);
    uint32_t levels = 1u;
    while (maxDim > 1u)
    {
      maxDim >>= 1u;
      ++levels;
    }
    return levels;
  }

  VirtualGeometryScene &scene_;
  VirtualGeometryHardwareDrawPass::FrameTarget frameTarget_;
  VirtualGeometryDepthPrePassDrawPass::FrameTarget prePassTarget_;
  rendering::Texture activeDepthHZBTexture_;
  uint32_t resolvedPrepassWidth_ = 1u;
  uint32_t resolvedPrepassHeight_ = 1u;
  uint32_t resolvedPrepassHZBMipCount_ = 1u;
  rendering::Texture shadowLightingTexture_;
  rendering::Texture previousDepthHZBTexture_;
  float clearDepth_ = 0.0f;
  Settings settings_;

  VirtualGeometryCullingMultipleDispatchesPass *preCullingPass_ = nullptr;
  VirtualGeometryPageDecodePass *pageDecodePass_ = nullptr;
  VirtualGeometryDepthPrePassDrawPass *depthPrePass_ = nullptr;
  VirtualGeometryDepthPyramidPass *depthPyramidPass_ = nullptr;
  VirtualGeometryCullingMultipleDispatchesPass *finalCullingPass_ = nullptr;
  VirtualGeometryStreamingPageSelectionPass *streamingPageSelectionPass_ = nullptr;
  VirtualGeometryHardwareDrawPass *hardwareDrawPass_ = nullptr;
  VirtualGeometryComputeRasterPass *computeRasterPass_ = nullptr;
  VirtualGeometryHardwareDrawPass *phaseOneDrawPass_ = nullptr;
  VirtualGeometrySoftwareRasterPass *softwareRasterPass_ = nullptr;
  VirtualGeometrySoftwareRasterPass *phaseOneSoftwareRasterPass_ = nullptr;
  VirtualGeometryVisibilityMergePass *visibilityMergePass_ = nullptr;
  VirtualGeometryVisibilityMergePass *phaseOneVisibilityMergePass_ = nullptr;
  VirtualGeometryVisibilityResolvePass *visibilityResolvePass_ = nullptr;
  VirtualGeometryVisibilityResolvePass *phaseOneVisibilityResolvePass_ = nullptr;
  ScreenSpaceRayTracePass *screenSpaceRayTracePass_ = nullptr;
  MaterialSetupPass *materialSetupPass_ = nullptr;
  MaterialPreparePass *materialPreparePass_ = nullptr;
  MaterialDispatchPass *materialDispatchPass_ = nullptr;
  std::unique_ptr<MaterialPass> materialPass_;
};

} // namespace gpgpu
} // namespace virtualgeometry
