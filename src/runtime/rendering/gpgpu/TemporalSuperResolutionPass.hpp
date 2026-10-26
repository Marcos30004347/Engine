#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace rendering
{
namespace gpgpu
{

class TemporalSuperResolutionPass : public rendering::Pass
{
public:
  // Which accumulator runs. Both pipelines are created up front and the choice
  // is made at dispatch, so switching costs nothing and needs no graph rebuild -
  // which is what makes an A/B on the same scene and camera possible at all.
  enum class Mode : uint32_t
  {
    Fsr2 = 0u,
    Fsr31 = 1u,
  };

  struct Settings
  {
    Mode mode = Mode::Fsr2;
    uint32_t sourceViewPortWidth = 1u;
    uint32_t sourceViewPortHeight = 1u;
    uint32_t outputViewPortWidth = 1u;
    uint32_t outputViewPortHeight = 1u;
    float currentJitter[2] = {0.0f, 0.0f};
    float previousJitter[2] = {0.0f, 0.0f};
    uint32_t frameIndex = 0u;
    bool historyValid = false;
    bool reverseDepth = true;
    // Ray-traced lighting is a stochastic signal. Its history must not be
    // rectified against a single noisy current-frame sample, and camera
    // motion must not invalidate otherwise correctly reprojected history.
    bool stochasticInput = false;
    // Reject history whose geometry identity differs from the surface now
    // covering the pixel. Required for voxel content, where a whole wall
    // shares one axis-aligned normal and reverse-Z depth is not usable as a
    // rejection signal, so identity is the only thing that separates two
    // adjacent surfaces. Leave off when the supplied identity textures do not
    // carry meaningful ids.
    bool identityRejection = false;
    // Rejection an identity mismatch causes on its own; see the accumulator.
    float identityRejectionStrength = 0.15f;
    // Rejection a normal mismatch causes when identity says the surface is
    // still under this pixel. One reproduces the old behaviour, where a normal
    // disagreement vetoed on its own - which on axis-aligned voxel faces fires
    // on every silhouette pixel a sub-pixel jitter moves across a face
    // boundary. Only consulted when identityRejection is on; without a usable
    // identity there is nothing to distinguish a face flip from a disocclusion
    // and the normal vetoes at full strength regardless.
    float normalRejectionStrength = 1.0f;
    // FSR 3.1 only: how hard the accumulator reacts to a pixel whose luminance
    // has been oscillating across the four-frame luma ring. Zero reduces the
    // FSR 3.1 accumulator to FSR2's history weighting plus the reworked lock.
    float lumaInstabilityStrength = 1.0f;
    float accumulationFrames = 16.0f;
    float depthThreshold = 0.0025f;
    float normalThreshold = 0.85f;
    float motionScale = 0.08f;
    float reactiveScale = 4.0f;
    float disocclusionScale = 1.25f;
    float sharpenStrength = 0.20f;
    float inputDisocclusionScale = 1.0f;
    float stabilityFactor = 0.70f;
    // Whether `motionVectorTexture` carries the frame-to-frame projection
    // jitter, as it does when both the current and previous world-to-clip
    // matrices are jittered with their own frame's offset. The dilate stage
    // subtracts the delta back out when it does. Declaring this wrongly is
    // worse than either behaviour on its own - a subtraction against
    // jitter-free motion manufactures a jitter-sized velocity everywhere, which
    // reads as a stationary image that will not settle.
    bool motionIncludesJitter = true;
    // Run reconstruction and accumulation in a reversibly range-compressed
    // space instead of on raw linear HDR. The Lanczos kernel, the rectification
    // box and the history clip all assume a neighbourhood's values are
    // commensurable, which on an untonemapped HDR image they are not; see
    // `tonemap_for_accumulation`. Off by default only because it changes the
    // meaning of `reactiveScale` and the rectification margin, both of which
    // were tuned in linear space - a caller turning it on should re-measure.
    bool tonemapAccumulation = false;
    // Drive the history blend from a per-pixel accumulated kernel weight rather
    // than one screen-wide rate derived from `accumulationFrames`. This is what
    // FSR2 does and what makes upsampled reconstruction converge: a pixel's
    // blend rate becomes 1/n in the evidence it has actually accumulated, so a
    // freshly disoccluded pixel ramps up instead of being handed full retention
    // on its second frame. `accumulationFrames` still sets where the ramp
    // settles. Off by default so the existing tested behaviour is unchanged.
    bool accumulationWeighting = false;
    // Sharpen with real RCAS rather than the unsharp mask this pass shipped
    // with. An unsharp mask cannot bound its own overshoot, so it haloes hard
    // edges and multiplies up whatever sits in a flat neighbourhood - which
    // after a 0.5-scale reconstruction is per-frame resampling residual. The old
    // path also mixed the image toward a box blur by `0.22 * stabilityFactor`
    // before sharpening it. Off by default so the tested output is unchanged.
    bool robustSharpen = false;
    // Derive the disocclusion signal from a reconstructed previous-frame depth
    // instead of differencing this frame's depth against the history depth.
    //
    // FSR2 has a whole pass for this and this pipeline never had it. What the
    // depth-clip stage does without it is compare source-resolution device depth
    // against the output-resolution history depth the accumulator wrote, which
    // is a different quantity at a different resolution, and a reverse-Z
    // magnitude difference is not a usable rejection signal under camera
    // translation - the stage's own comments say so before the non-stochastic
    // path uses it anyway. Requires depthLinearA/B to be set.
    bool reconstructedDepth = false;
    // Relative view-distance separation counting as a full disocclusion. FSR2
    // derives an absolute separation from view-space geometry; a fraction of the
    // view distance is scene-scale free, which matters more here than matching
    // its constant.
    float depthSeparationScale = 0.02f;
    // Projection matrix (2,2) and (2,3), used as
    // `viewDistance = B / (deviceDepth + A)`. The entries rather than near and
    // far, so reverse-Z, normal Z and an infinite far plane all work without
    // this pass having to know which is in use. Only read when
    // reconstructedDepth is on.
    float depthLinearA = 0.0f;
    float depthLinearB = 1.0f;
    // Compute an exposure from the frame's own geometric-mean luminance and apply
    // it before range compression.
    //
    // Only meaningful with tonemapAccumulation on - exposure without compression
    // just rescales the image. With it, this is what makes the compression do
    // anything: an unexposed linear 12 compresses to 0.923 and 100 to 0.990, so
    // every lit value in the scene lands in the top few percent of the range where
    // the rectification box, the history clip and the Lanczos kernel cannot
    // separate them.
    bool exposureEnabled = false;
    // Extra width on the neighbourhood rectification box; see the shader. One
    // leaves it as fitted. Only worth moving when the space the box is measured in
    // changes, which is what tonemapAccumulation does.
    float rectificationBoxScale = 1.0f;
    // FSR 3.1's shading-change detection: how hard a coarse-scale luminance change
    // costs a pixel its history. Zero disables it.
    //
    // The only term in the accumulator that can react to lighting moving under
    // stationary geometry - which every geometric validation term correctly reports
    // as "nothing happened", and which is what ghosts when probe lighting changes.
    // FSR 3.1 only. Needs the coarse-luma textures to be real surfaces; pass
    // placeholders and leave this at zero if the caller has no ring for them.
    float shadingChangeStrength = 0.0f;
    // Stops of coarse luminance change below which nothing is charged; a full
    // change is two stops above it. This is the line between "the lighting
    // changed" and "the lighting estimate is noisy", and where it belongs depends
    // on the renderer: a stochastic, scheduled lighting solution moves a cell's
    // mean every frame by design, and a threshold under that noise floor drops the
    // history that was suppressing the noise and exposes more of it.
    float shadingChangeStops = 1.0f;
    // At a stationary silhouette, projection jitter can select a different
    // voxel or face even though the scene did not disocclude. Blend geometric
    // rejection down for that narrow case instead of restarting accumulation.
    float stationaryEdgeBlend = 1.0f;
    // Mix a positive reconstruction kernel into Lanczos at detected geometry
    // boundaries to suppress residual sub-pixel ringing without blurring the
    // rest of the image.
    float edgeSoftness = 0.50f;
    // Write the accumulator's history-weight term chain to the two debug
    // targets. Diagnostic only; leave off unless something downstream reduces
    // them, and bind placeholders for the targets when it is off.
    bool debugTerms = false;
  };

  TemporalSuperResolutionPass(
      rendering::Texture currentColorTexture,
      rendering::Texture currentNormalTexture,
      rendering::Texture sceneDepthTexture,
      rendering::Texture motionVectorTexture,
      rendering::Texture inputDisocclusionMaskTexture,
      rendering::Texture currentInstanceTexture,
      rendering::Texture currentVoxelTexture,
      rendering::Texture historyColorTexture,
      rendering::Texture historyDepthTexture,
      rendering::Texture historyNormalTexture,
      rendering::Texture historyIdTexture,
      rendering::Texture historyLockStatusTexture,
      rendering::Texture historyLumaTexture,
      rendering::Texture historyDilatedMotionTexture,
      rendering::Texture historyAccumulationTexture,
      rendering::Texture historyCoarseLumaTexture,
      rendering::Texture outputTexture,
      rendering::Texture outputHistoryColorTexture,
      rendering::Texture outputHistoryDepthTexture,
      rendering::Texture outputHistoryNormalTexture,
      rendering::Texture outputHistoryIdTexture,
      rendering::Texture outputLockStatusTexture,
      rendering::Texture outputLumaTexture,
      rendering::Texture outputDilatedMotionTexture,
      rendering::Texture outputDisocclusionMaskTexture,
      rendering::Texture outputDebugTermsATexture,
      rendering::Texture outputDebugTermsBTexture,
      rendering::Texture outputAccumulationTexture,
      rendering::Texture outputCoarseLumaTexture,
      Settings settings)
      : currentColorTexture_(currentColorTexture),
        currentNormalTexture_(currentNormalTexture),
        sceneDepthTexture_(sceneDepthTexture),
        motionVectorTexture_(motionVectorTexture),
        inputDisocclusionMaskTexture_(inputDisocclusionMaskTexture),
        currentInstanceTexture_(currentInstanceTexture),
        currentVoxelTexture_(currentVoxelTexture),
        historyColorTexture_(historyColorTexture),
        historyDepthTexture_(historyDepthTexture),
        historyNormalTexture_(historyNormalTexture),
        historyIdTexture_(historyIdTexture),
        historyLockStatusTexture_(historyLockStatusTexture),
        historyLumaTexture_(historyLumaTexture),
        historyDilatedMotionTexture_(historyDilatedMotionTexture),
        historyAccumulationTexture_(historyAccumulationTexture),
        historyCoarseLumaTexture_(historyCoarseLumaTexture),
        outputTexture_(outputTexture),
        outputHistoryColorTexture_(outputHistoryColorTexture),
        outputHistoryDepthTexture_(outputHistoryDepthTexture),
        outputHistoryNormalTexture_(outputHistoryNormalTexture),
        outputHistoryIdTexture_(outputHistoryIdTexture),
        outputLockStatusTexture_(outputLockStatusTexture),
        outputLumaTexture_(outputLumaTexture),
        outputDilatedMotionTexture_(outputDilatedMotionTexture),
        outputDisocclusionMaskTexture_(outputDisocclusionMaskTexture),
        outputDebugTermsATexture_(outputDebugTermsATexture),
        outputDebugTermsBTexture_(outputDebugTermsBTexture),
        outputAccumulationTexture_(outputAccumulationTexture),
        outputCoarseLumaTexture_(outputCoarseLumaTexture),
        settings_(settings)
  {
  }

  ~TemporalSuperResolutionPass() override
  {
    destroy();
  }

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    if (uniformBuffer_.isValid())
      writeUniforms();
  }

  const rendering::Texture &getOutputTexture() const { return outputTexture_; }
  const rendering::Texture &getOutputHistoryColorTexture() const { return outputHistoryColorTexture_; }
  const rendering::Texture &getOutputHistoryDepthTexture() const { return outputHistoryDepthTexture_; }
  const rendering::Texture &getOutputHistoryNormalTexture() const { return outputHistoryNormalTexture_; }
  const rendering::Texture &getOutputHistoryIdTexture() const { return outputHistoryIdTexture_; }
  const rendering::Texture &getOutputLockStatusTexture() const { return outputLockStatusTexture_; }
  const rendering::Texture &getOutputLumaTexture() const { return outputLumaTexture_; }
  const rendering::Texture &getOutputDilatedMotionTexture() const { return outputDilatedMotionTexture_; }
  const rendering::Texture &getOutputDisocclusionMaskTexture() const { return outputDisocclusionMaskTexture_; }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    createResources();

    // The scatter in ReconstructDilate writes into slots belonging to other
    // pixels, so the reset has to be a separate dispatch that completes first.
    // The exposure accumulators are cleared in the same dispatch for the same
    // reason.
    dispatchLinearStage(
        commandBuffer, Stage::FrameReset,
        std::max(settings_.sourceViewPortWidth, 1u) *
            std::max(settings_.sourceViewPortHeight, 1u));
    // Exposure is measured from this frame's own colour and consumed by every
    // stage after it, so it has to be reduced and resolved before the dilate.
    // A reduction cannot read its own total, hence the split.
    dispatchStage(commandBuffer, Stage::ExposureReduce, settings_.sourceViewPortWidth, settings_.sourceViewPortHeight);
    dispatchLinearStage(commandBuffer, Stage::ExposureResolve, 1u);
    // Coarse luminance for shading-change detection. Reduced from the current
    // colour, so it only has to precede the accumulator that reads it.
    dispatchStage(
        commandBuffer, Stage::CoarseLuma,
        (std::max(settings_.sourceViewPortWidth, 1u) + kCoarseLumaTile - 1u) / kCoarseLumaTile,
        (std::max(settings_.sourceViewPortHeight, 1u) + kCoarseLumaTile - 1u) / kCoarseLumaTile);
    dispatchStage(commandBuffer, Stage::ReconstructDilate, settings_.sourceViewPortWidth, settings_.sourceViewPortHeight);
    dispatchStage(commandBuffer, Stage::DepthReactive, settings_.sourceViewPortWidth, settings_.sourceViewPortHeight);
    dispatchStage(commandBuffer, Stage::Lock, settings_.outputViewPortWidth, settings_.outputViewPortHeight);
    dispatchStage(
        commandBuffer,
        settings_.mode == Mode::Fsr31 ? Stage::AccumulateFsr31
                                      : Stage::Accumulate,
        settings_.outputViewPortWidth,
        settings_.outputViewPortHeight);
    dispatchStage(
        commandBuffer,
        Stage::HistoryCopy,
        settings_.sourceViewPortWidth,
        settings_.sourceViewPortHeight);
    dispatchStage(commandBuffer, Stage::Rcas, settings_.outputViewPortWidth, settings_.outputViewPortHeight);
  }

private:
  enum class Stage : uint32_t
  {
    FrameReset = 0u,
    ExposureReduce,
    ExposureResolve,
    CoarseLuma,
    ReconstructDilate,
    DepthReactive,
    Lock,
    Accumulate,
    AccumulateFsr31,
    HistoryCopy,
    Rcas,
    Count,
  };

  static constexpr uint32_t kStageCount = static_cast<uint32_t>(Stage::Count);
  // Must match EXPOSURE_SLOTS in fsr2-temporal-common.wgsl.
  static constexpr uint32_t kExposureSlots = 4u;
  // Must match COARSE_LUMA_TILE in fsr2-temporal-common.wgsl.
  static constexpr uint32_t kCoarseLumaTile = 16u;

  struct Uniforms
  {
    uint32_t sourceViewport[2] = {1u, 1u};
    uint32_t outputViewport[2] = {1u, 1u};
    float currentJitter[2] = {0.0f, 0.0f};
    float previousJitter[2] = {0.0f, 0.0f};
    uint32_t historyValid = 0u;
    uint32_t reverseDepth = 1u;
    uint32_t frameIndex = 0u;
    uint32_t stochasticInput = 0u;
    float accumulationFrames = 16.0f;
    float depthThreshold = 0.0025f;
    float normalThreshold = 0.85f;
    float motionScale = 0.08f;
    float reactiveScale = 4.0f;
    float disocclusionScale = 1.25f;
    float sharpenStrength = 0.35f;
    float inputDisocclusionScale = 1.0f;
    float stabilityFactor = 0.70f;
    uint32_t identityRejection = 0u;
    // How much a voxel identity mismatch rejects history on its own, with
    // depth and normal in agreement. One reproduces the old behaviour where
    // identity vetoed both; zero ignores identity entirely.
    float identityRejectionStrength = 0.15f;
    uint32_t debugTerms = 0u;
    uint32_t motionIncludesJitter = 1u;
    float normalRejectionStrength = 1.0f;
    float lumaInstabilityStrength = 1.0f;
    uint32_t tonemapAccumulation = 0u;
    uint32_t accumulationWeighting = 0u;
    uint32_t robustSharpen = 0u;
    float depthLinearA = 0.0f;
    float depthLinearB = 1.0f;
    float depthSeparationScale = 0.02f;
    uint32_t reconstructedDepth = 0u;
    uint32_t exposureEnabled = 0u;
    float rectificationBoxScale = 1.0f;
    float shadingChangeStrength = 0.0f;
    float shadingChangeStops = 1.0f;
    float stationaryEdgeBlend = 1.0f;
    float edgeSoftness = 0.50f;
  };
  static_assert(sizeof(Uniforms) == 160u, "TemporalSuperResolutionPass::Uniforms size mismatch");

  void destroy()
  {
    if (renderGraph == nullptr)
      return;

    for (auto &pipeline : pipelines_)
      if (pipeline.isValid())
        renderGraph->deleteComputePipeline(pipeline);
    for (auto &shader : shaders_)
      if (shader.isValid())
        renderGraph->deleteShader(shader);
    for (auto &groups : bindingGroups_)
      if (groups.isValid())
        renderGraph->deleteBindingGroups(groups);
    for (auto &layout : layouts_)
      if (layout.isValid())
        renderGraph->deleteBindingsLayout(layout);
    if (uniformBuffer_.isValid())
      renderGraph->deleteBuffer(uniformBuffer_);
    if (reconstructedDepthBuffer_.isValid())
      renderGraph->deleteBuffer(reconstructedDepthBuffer_);
    if (exposureBuffer_.isValid())
      renderGraph->deleteBuffer(exposureBuffer_);
    for (auto &texture : internalTextures_)
      if (texture.isValid())
        renderGraph->deleteTexture(texture);
  }

  rendering::Texture createInternalTexture(const char *suffix, rendering::Format format, uint32_t width, uint32_t height)
  {
    return createFrameLocalTexture(
        rendering::TextureInfo{
            .name = passName + "_" + std::string(suffix) + ".texture",
            .format = format,
            .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
            .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
            .width = std::max(width, 1u),
            .height = std::max(height, 1u),
            .depth = 1u,
            .mipLevels = 1u,
        });
  }

  void createResources()
  {
    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(Uniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });
    writeUniforms();

    preparedColorTexture_ = createInternalTexture("PreparedColor", rendering::Format::Format_RGBA16Float, settings_.sourceViewPortWidth, settings_.sourceViewPortHeight);
    dilatedDepthTexture_ = createInternalTexture("DilatedDepth", rendering::Format::Format_R32Float, settings_.sourceViewPortWidth, settings_.sourceViewPortHeight);
    dilatedMotionTexture_ = createInternalTexture("DilatedMotion", rendering::Format::Format_RGBA16Float, settings_.sourceViewPortWidth, settings_.sourceViewPortHeight);
    lockInputLumaTexture_ = createInternalTexture("LockInputLuma", rendering::Format::Format_R32Float, settings_.sourceViewPortWidth, settings_.sourceViewPortHeight);
    depthClipTexture_ = createInternalTexture("DepthClip", rendering::Format::Format_R32Float, settings_.sourceViewPortWidth, settings_.sourceViewPortHeight);
    reactiveMasksTexture_ = createInternalTexture("ReactiveMasks", rendering::Format::Format_RGBA16Float, settings_.sourceViewPortWidth, settings_.sourceViewPortHeight);
    lockStatusScratchTexture_ = createInternalTexture("LockStatusScratch", rendering::Format::Format_RGBA16Float, settings_.outputViewPortWidth, settings_.outputViewPortHeight);
    newLocksTexture_ = createInternalTexture("NewLocks", rendering::Format::Format_R32Float, settings_.outputViewPortWidth, settings_.outputViewPortHeight);
    // Accumulation history and the pre-RCAS color are byte-identical. Sample
    // the history output directly in RCAS instead of allocating another
    // output-sized texture and writing every pixel twice.
    internalColorTexture_ = outputHistoryColorTexture_;
    internalTextures_ = {
      preparedColorTexture_,
      dilatedDepthTexture_,
      dilatedMotionTexture_,
      lockInputLumaTexture_,
      depthClipTexture_,
      reactiveMasksTexture_,
      lockStatusScratchTexture_,
      newLocksTexture_,
    };

    reconstructedDepthBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_ReconstructedDepth.buffer",
            .size = uint64_t{std::max(settings_.sourceViewPortWidth, 1u)} *
                std::max(settings_.sourceViewPortHeight, 1u) * sizeof(uint32_t),
            .usage = rendering::BufferUsage::BufferUsage_Storage,
        });

    exposureBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Exposure.buffer",
            .size = kExposureSlots * sizeof(uint32_t),
            .usage = rendering::BufferUsage::BufferUsage_Storage,
        });

    createFrameResetStage();
    createExposureStages();
    createCoarseLumaStage();
    createReconstructDilateStage();
    createDepthReactiveStage();
    createLockStage();
    createAccumulateStage(
        Stage::Accumulate, "assets/shaders/spirv/fsr2-accumulate-cs.spirv");
    createAccumulateStage(
        Stage::AccumulateFsr31,
        "assets/shaders/spirv/fsr31-accumulate-cs.spirv");
    createHistoryCopyStage();
    createRcasStage();
  }

  void dispatchStage(rendering::CommandRecorder &commandBuffer, Stage stage, uint32_t width, uint32_t height)
  {
    const uint32_t index = static_cast<uint32_t>(stage);
    commandBuffer.cmdBindComputePipeline(pipelines_[index]);
    commandBuffer.cmdBindBindingGroups(bindingGroups_[index], nullptr, 0);
    commandBuffer.cmdDispatch((std::max(width, 1u) + 7u) / 8u, (std::max(height, 1u) + 7u) / 8u, 1u);
  }

  // For the stages whose domain is the flat depth buffer rather than the image.
  void dispatchLinearStage(
      rendering::CommandRecorder &commandBuffer, Stage stage, uint32_t count)
  {
    const uint32_t index = static_cast<uint32_t>(stage);
    commandBuffer.cmdBindComputePipeline(pipelines_[index]);
    commandBuffer.cmdBindBindingGroups(bindingGroups_[index], nullptr, 0);
    commandBuffer.cmdDispatch((std::max(count, 1u) + 63u) / 64u, 1u, 1u);
  }

  void writeUniforms()
  {
    const Uniforms uniforms{
      .sourceViewport = {std::max(settings_.sourceViewPortWidth, 1u), std::max(settings_.sourceViewPortHeight, 1u)},
      .outputViewport = {std::max(settings_.outputViewPortWidth, 1u), std::max(settings_.outputViewPortHeight, 1u)},
      .currentJitter = {settings_.currentJitter[0], settings_.currentJitter[1]},
      .previousJitter = {settings_.previousJitter[0], settings_.previousJitter[1]},
      .historyValid = settings_.historyValid ? 1u : 0u,
      .reverseDepth = settings_.reverseDepth ? 1u : 0u,
      .frameIndex = settings_.frameIndex,
      .stochasticInput = settings_.stochasticInput ? 1u : 0u,
      .accumulationFrames = std::clamp(settings_.accumulationFrames, 1.0f, 64.0f),
      .depthThreshold = std::max(settings_.depthThreshold, 1.0e-6f),
      .normalThreshold = std::clamp(settings_.normalThreshold, -1.0f, 1.0f),
      .motionScale = std::max(settings_.motionScale, 0.0f),
      .reactiveScale = std::max(settings_.reactiveScale, 0.0f),
      .disocclusionScale = std::max(settings_.disocclusionScale, 0.0f),
      .sharpenStrength = std::clamp(settings_.sharpenStrength, 0.0f, 1.5f),
      .inputDisocclusionScale = std::max(settings_.inputDisocclusionScale, 0.0f),
      .stabilityFactor = std::clamp(settings_.stabilityFactor, 0.0f, 1.0f),
      .identityRejection = settings_.identityRejection ? 1u : 0u,
      .identityRejectionStrength =
          std::clamp(settings_.identityRejectionStrength, 0.0f, 1.0f),
      .debugTerms = settings_.debugTerms ? 1u : 0u,
      .motionIncludesJitter = settings_.motionIncludesJitter ? 1u : 0u,
      .normalRejectionStrength =
          std::clamp(settings_.normalRejectionStrength, 0.0f, 1.0f),
      .lumaInstabilityStrength =
          std::clamp(settings_.lumaInstabilityStrength, 0.0f, 1.0f),
      .tonemapAccumulation = settings_.tonemapAccumulation ? 1u : 0u,
      .accumulationWeighting = settings_.accumulationWeighting ? 1u : 0u,
      .robustSharpen = settings_.robustSharpen ? 1u : 0u,
      .depthLinearA = settings_.depthLinearA,
      .depthLinearB = settings_.depthLinearB,
      .depthSeparationScale = std::max(settings_.depthSeparationScale, 1.0e-5f),
      .reconstructedDepth = settings_.reconstructedDepth ? 1u : 0u,
      .exposureEnabled = settings_.exposureEnabled ? 1u : 0u,
      .rectificationBoxScale =
          std::clamp(settings_.rectificationBoxScale, 0.25f, 8.0f),
      .shadingChangeStrength =
          std::clamp(settings_.shadingChangeStrength, 0.0f, 1.0f),
      .shadingChangeStops = std::max(settings_.shadingChangeStops, 0.0f),
      .stationaryEdgeBlend =
          std::clamp(settings_.stationaryEdgeBlend, 0.0f, 1.0f),
      .edgeSoftness = std::clamp(settings_.edgeSoftness, 0.0f, 1.0f),
    };
    renderGraph->bufferWrite(uniformBuffer_, 0, sizeof(uniforms), const_cast<Uniforms *>(&uniforms));
  }

  rendering::BufferView reconstructedDepthView(
      const rendering::AccessPattern access) const
  {
    return rendering::BufferView{
        .buffer = reconstructedDepthBuffer_,
        .access = access,
        .offset = 0,
        .size = uint64_t{std::max(settings_.sourceViewPortWidth, 1u)} *
            std::max(settings_.sourceViewPortHeight, 1u) * sizeof(uint32_t)};
  }

  rendering::BufferView exposureView(const rendering::AccessPattern access) const
  {
    return rendering::BufferView{
        .buffer = exposureBuffer_,
        .access = access,
        .offset = 0,
        .size = kExposureSlots * sizeof(uint32_t)};
  }

  rendering::BufferView uniformView() const
  {
    return rendering::BufferView{.buffer = uniformBuffer_, .access = rendering::AccessPattern::SHADER_READ, .offset = 0, .size = sizeof(Uniforms)};
  }

  static rendering::TextureView makeSampledColorView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ,
        .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0u,
        .flags = rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  static rendering::TextureView makeSampledDepthView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ,
        .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
        .index = 0u,
        .flags = rendering::ImageAspectFlags::Depth,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  static rendering::TextureView makeStorageColorView(rendering::Texture texture)
  {
    return rendering::TextureView{
        .texture = texture,
        .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
        .layout = rendering::ResourceLayout::GENERAL,
        .index = 0u,
        .flags = rendering::ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
    };
  }

  void createPipeline(Stage stage, const char *shaderPath, const rendering::BindingsLayoutInfo &layoutInfo, const rendering::BindingGroupsInfo &groupsInfo, const char *entry = "main")
  {
    const uint32_t index = static_cast<uint32_t>(stage);
    layouts_[index] = renderGraph->createBindingsLayout(layoutInfo);
    rendering::BindingGroupsInfo patchedGroupsInfo = groupsInfo;
    patchedGroupsInfo.layout = layouts_[index];
    bindingGroups_[index] = renderGraph->createBindingGroups(patchedGroupsInfo);
    shaders_[index] = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_" + std::to_string(index) + "_Shader",
            .layout = layouts_[index],
            .src = os::io::readRelativeFile(shaderPath),
            .type = rendering::ShaderType::SpirV,
        });
    pipelines_[index] = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{
            .entry = entry,
            .layout = layouts_[index],
            .name = passName + "_" + std::to_string(index) + "_Pipeline",
            .shader = shaders_[index],
        });
  }

  void createFrameResetStage()
  {
    createPipeline(
        Stage::FrameReset,
        "assets/shaders/spirv/fsr2-frame-reset-cs.spirv",
        rendering::BindingsLayoutInfo{
            .name = passName + "_FrameResetLayout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "reconstructedDepth", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "exposure", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        },
        rendering::BindingGroupsInfo{
            .name = passName + "_FrameResetGroups",
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers = {
                    {.binding = 0, .bufferView = uniformView()},
                    {.binding = 1, .bufferView = reconstructedDepthView(rendering::AccessPattern::SHADER_WRITE)},
                    {.binding = 2, .bufferView = exposureView(rendering::AccessPattern::SHADER_WRITE)},
                },
            }}});
  }

  // Both entry points live in one module: they are one algorithm split by the
  // barrier a reduction needs before it can read its own total.
  void createExposureStages()
  {
    const auto layout = [&](const char *suffix)
    {
      return rendering::BindingsLayoutInfo{
          .name = passName + "_Exposure" + suffix + "Layout",
          .groups = {rendering::BindingGroupLayout{
              .buffers = {
                  {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                  {.name = "exposure", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
              },
              .textures = {
                  {.name = "currentColorTexture", .binding = 1, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
              },
          }},
      };
    };
    const auto groups = [&](const char *suffix)
    {
      return rendering::BindingGroupsInfo{
          .name = passName + "_Exposure" + suffix + "Groups",
          .groups = {rendering::GroupInfo{
              .name = "group0",
              .buffers = {
                  {.binding = 0, .bufferView = uniformView()},
                  {.binding = 2, .bufferView = exposureView(rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
              },
              .textures = {
                  {.binding = 1, .textureView = makeSampledColorView(currentColorTexture_)},
              },
          }}};
    };
    createPipeline(
        Stage::ExposureReduce, "assets/shaders/spirv/fsr2-exposure-cs.spirv",
        layout("Reduce"), groups("Reduce"), "reduceExposure");
    createPipeline(
        Stage::ExposureResolve, "assets/shaders/spirv/fsr2-exposure-cs.spirv",
        layout("Resolve"), groups("Resolve"), "resolveExposure");
  }

  void createCoarseLumaStage()
  {
    createPipeline(
        Stage::CoarseLuma,
        "assets/shaders/spirv/fsr2-coarse-luma-cs.spirv",
        rendering::BindingsLayoutInfo{
            .name = passName + "_CoarseLumaLayout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {{.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute}},
                .textures = {
                    {.name = "currentColorTexture", .binding = 1, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "outputCoarseLumaTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        },
        rendering::BindingGroupsInfo{
            .name = passName + "_CoarseLumaGroups",
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers = {{.binding = 0, .bufferView = uniformView()}},
                .textures = {
                    {.binding = 1, .textureView = makeSampledColorView(currentColorTexture_)},
                },
                .storageTextures = {
                    {.binding = 2, .textureView = makeStorageColorView(outputCoarseLumaTexture_)},
                },
            }}});
  }

  void createReconstructDilateStage()
  {
    createPipeline(
        Stage::ReconstructDilate,
        "assets/shaders/spirv/fsr2-reconstruct-dilate-cs.spirv",
        rendering::BindingsLayoutInfo{
            .name = passName + "_ReconstructDilateLayout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "reconstructedDepth", .binding = 8, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "exposureSlots", .binding = 9, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "currentColorTexture", .binding = 1, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "sceneDepthTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "motionVectorTexture", .binding = 3, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "preparedColorTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "dilatedDepthTexture", .binding = 5, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "dilatedMotionTexture", .binding = 6, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "lockInputLumaTexture", .binding = 7, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        },
        rendering::BindingGroupsInfo{
            .name = passName + "_ReconstructDilateGroups",
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers = {
                    {.binding = 0, .bufferView = uniformView()},
                    {.binding = 8, .bufferView = reconstructedDepthView(rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE)},
                    {.binding = 9, .bufferView = exposureView(rendering::AccessPattern::SHADER_READ)},
                },
                .textures = {
                    {.binding = 1, .textureView = makeSampledColorView(currentColorTexture_)},
                    {.binding = 2, .textureView = makeSampledDepthView(sceneDepthTexture_)},
                    {.binding = 3, .textureView = makeSampledColorView(motionVectorTexture_)},
                },
                .storageTextures = {
                    {.binding = 4, .textureView = makeStorageColorView(preparedColorTexture_)},
                    {.binding = 5, .textureView = makeStorageColorView(dilatedDepthTexture_)},
                    {.binding = 6, .textureView = makeStorageColorView(dilatedMotionTexture_)},
                    {.binding = 7, .textureView = makeStorageColorView(lockInputLumaTexture_)},
                },
            }}});
  }

  void createDepthReactiveStage()
  {
    createPipeline(
        Stage::DepthReactive,
        "assets/shaders/spirv/fsr2-depth-reactive-cs.spirv",
        rendering::BindingsLayoutInfo{
            .name = passName + "_DepthReactiveLayout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "reconstructedDepth", .binding = 9, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "currentColorTexture", .binding = 1, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "dilatedDepthTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "dilatedMotionTexture", .binding = 3, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "inputDisocclusionMaskTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyDepthTexture", .binding = 5, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyDilatedMotionTexture", .binding = 6, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "depthClipTexture", .binding = 7, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "reactiveMasksTexture", .binding = 8, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        },
        rendering::BindingGroupsInfo{
            .name = passName + "_DepthReactiveGroups",
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers = {
                    {.binding = 0, .bufferView = uniformView()},
                    {.binding = 9, .bufferView = reconstructedDepthView(rendering::AccessPattern::SHADER_READ)},
                },
                .textures = {
                    {.binding = 1, .textureView = makeSampledColorView(currentColorTexture_)},
                    {.binding = 2, .textureView = makeSampledColorView(dilatedDepthTexture_)},
                    {.binding = 3, .textureView = makeSampledColorView(dilatedMotionTexture_)},
                    {.binding = 4, .textureView = makeSampledColorView(inputDisocclusionMaskTexture_)},
                    {.binding = 5, .textureView = makeSampledColorView(historyDepthTexture_)},
                    {.binding = 6, .textureView = makeSampledColorView(historyDilatedMotionTexture_)},
                },
                .storageTextures = {
                    {.binding = 7, .textureView = makeStorageColorView(depthClipTexture_)},
                    {.binding = 8, .textureView = makeStorageColorView(reactiveMasksTexture_)},
                },
            }}});
  }

  void createLockStage()
  {
    createPipeline(
        Stage::Lock,
        "assets/shaders/spirv/fsr2-lock-cs.spirv",
        rendering::BindingsLayoutInfo{
            .name = passName + "_LockLayout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {{.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute}},
                .textures = {
                    {.name = "lockInputLumaTexture", .binding = 1, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "dilatedMotionTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyLockStatusTexture", .binding = 3, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "lockStatusScratchTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "newLocksTexture", .binding = 5, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        },
        rendering::BindingGroupsInfo{
            .name = passName + "_LockGroups",
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers = {{.binding = 0, .bufferView = uniformView()}},
                .textures = {
                    {.binding = 1, .textureView = makeSampledColorView(lockInputLumaTexture_)},
                    {.binding = 2, .textureView = makeSampledColorView(dilatedMotionTexture_)},
                    {.binding = 3, .textureView = makeSampledColorView(historyLockStatusTexture_)},
                },
                .storageTextures = {
                    {.binding = 4, .textureView = makeStorageColorView(lockStatusScratchTexture_)},
                    {.binding = 5, .textureView = makeStorageColorView(newLocksTexture_)},
                },
            }}});
  }

  // Both accumulators share one binding layout, one set of inputs and one set
  // of outputs; they differ only in the shader bound. Keeping the layout common
  // is what lets the mode be a dispatch-time choice.
  void createAccumulateStage(const Stage stage, const char *shaderPath)
  {
    createPipeline(
        stage,
        shaderPath,
        rendering::BindingsLayoutInfo{
            .name = passName + "_AccumulateLayout" + std::to_string(static_cast<uint32_t>(stage)),
            .groups = {rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "exposureSlots", .binding = 27, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {
                    {.name = "preparedColorTexture", .binding = 1, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "dilatedDepthTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "dilatedMotionTexture", .binding = 3, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "depthClipTexture", .binding = 4, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "reactiveMasksTexture", .binding = 5, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "currentNormalTexture", .binding = 6, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyColorTexture", .binding = 7, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyDepthTexture", .binding = 8, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyNormalTexture", .binding = 9, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "lockStatusScratchTexture", .binding = 10, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyLumaTexture", .binding = 11, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "newLocksTexture", .binding = 12, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyAccumulationTexture", .binding = 13, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "currentInstanceTexture", .binding = 20, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "currentVoxelTexture", .binding = 21, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyIdTexture", .binding = 22, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "coarseLumaTexture", .binding = 28, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "historyCoarseLumaTexture", .binding = 29, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "outputHistoryColorTexture", .binding = 14, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "outputHistoryDepthTexture", .binding = 15, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "outputHistoryNormalTexture", .binding = 16, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "outputLumaTexture", .binding = 17, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "outputLockStatusTexture", .binding = 18, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "outputDisocclusionMaskTexture", .binding = 19, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "outputHistoryIdTexture", .binding = 23, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "outputDebugTermsATexture", .binding = 24, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "outputDebugTermsBTexture", .binding = 25, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "outputAccumulationTexture", .binding = 26, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        },
        rendering::BindingGroupsInfo{
            .name = passName + "_AccumulateGroups" + std::to_string(static_cast<uint32_t>(stage)),
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers = {
                    {.binding = 0, .bufferView = uniformView()},
                    {.binding = 27, .bufferView = exposureView(rendering::AccessPattern::SHADER_READ)},
                },
                .textures = {
                    {.binding = 1, .textureView = makeSampledColorView(preparedColorTexture_)},
                    {.binding = 2, .textureView = makeSampledColorView(dilatedDepthTexture_)},
                    {.binding = 3, .textureView = makeSampledColorView(dilatedMotionTexture_)},
                    {.binding = 4, .textureView = makeSampledColorView(depthClipTexture_)},
                    {.binding = 5, .textureView = makeSampledColorView(reactiveMasksTexture_)},
                    {.binding = 6, .textureView = makeSampledColorView(currentNormalTexture_)},
                    {.binding = 7, .textureView = makeSampledColorView(historyColorTexture_)},
                    {.binding = 8, .textureView = makeSampledColorView(historyDepthTexture_)},
                    {.binding = 9, .textureView = makeSampledColorView(historyNormalTexture_)},
                    {.binding = 10, .textureView = makeSampledColorView(lockStatusScratchTexture_)},
                    {.binding = 11, .textureView = makeSampledColorView(historyLumaTexture_)},
                    {.binding = 12, .textureView = makeSampledColorView(newLocksTexture_)},
                    {.binding = 13, .textureView = makeSampledColorView(historyAccumulationTexture_)},
                    {.binding = 20, .textureView = makeSampledColorView(currentInstanceTexture_)},
                    {.binding = 21, .textureView = makeSampledColorView(currentVoxelTexture_)},
                    {.binding = 22, .textureView = makeSampledColorView(historyIdTexture_)},
                    // This frame's grid, written by the stage above and read back as
                    // sampled in the same frame - the same arrangement RCAS uses for
                    // the accumulator's colour output.
                    {.binding = 28, .textureView = makeSampledColorView(outputCoarseLumaTexture_)},
                    {.binding = 29, .textureView = makeSampledColorView(historyCoarseLumaTexture_)},
                },
                .storageTextures = {
                    {.binding = 14, .textureView = makeStorageColorView(outputHistoryColorTexture_)},
                    {.binding = 15, .textureView = makeStorageColorView(outputHistoryDepthTexture_)},
                    {.binding = 16, .textureView = makeStorageColorView(outputHistoryNormalTexture_)},
                    {.binding = 17, .textureView = makeStorageColorView(outputLumaTexture_)},
                    {.binding = 18, .textureView = makeStorageColorView(outputLockStatusTexture_)},
                    {.binding = 19, .textureView = makeStorageColorView(outputDisocclusionMaskTexture_)},
                    {.binding = 23, .textureView = makeStorageColorView(outputHistoryIdTexture_)},
                    {.binding = 24, .textureView = makeStorageColorView(outputDebugTermsATexture_)},
                    {.binding = 25, .textureView = makeStorageColorView(outputDebugTermsBTexture_)},
                    {.binding = 26, .textureView = makeStorageColorView(outputAccumulationTexture_)},
                },
            }}});
  }

  void createHistoryCopyStage()
  {
    createPipeline(
        Stage::HistoryCopy,
        "assets/shaders/spirv/fsr2-history-copy-cs.spirv",
        rendering::BindingsLayoutInfo{
            .name = passName + "_HistoryCopyLayout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {{.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute}},
                .textures = {
                    {.name = "dilatedMotionTexture", .binding = 6, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "outputDilatedMotionTexture", .binding = 12, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        },
        rendering::BindingGroupsInfo{
            .name = passName + "_HistoryCopyGroups",
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers = {{.binding = 0, .bufferView = uniformView()}},
                .textures = {
                    {.binding = 6, .textureView = makeSampledColorView(dilatedMotionTexture_)},
                },
                .storageTextures = {
                    {.binding = 12, .textureView = makeStorageColorView(outputDilatedMotionTexture_)},
                },
            }}});
  }

  void createRcasStage()
  {
    createPipeline(
        Stage::Rcas,
        "assets/shaders/spirv/fsr2-rcas-cs.spirv",
        rendering::BindingsLayoutInfo{
            .name = passName + "_RcasLayout",
            .groups = {rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "exposureSlots", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .textures = {{.name = "internalColorTexture", .binding = 1, .visibility = rendering::BindingVisibility::BindingVisibility_Compute}},
                .storageTextures = {{.name = "outputTexture", .binding = 2, .visibility = rendering::BindingVisibility::BindingVisibility_Compute}},
            }},
        },
        rendering::BindingGroupsInfo{
            .name = passName + "_RcasGroups",
            .groups = {rendering::GroupInfo{
                .name = "group0",
                .buffers = {
                    {.binding = 0, .bufferView = uniformView()},
                    {.binding = 3, .bufferView = exposureView(rendering::AccessPattern::SHADER_READ)},
                },
                .textures = {{.binding = 1, .textureView = makeSampledColorView(internalColorTexture_)}},
                .storageTextures = {{.binding = 2, .textureView = makeStorageColorView(outputTexture_)}},
            }}});
  }

  rendering::Texture currentColorTexture_{};
  rendering::Texture currentNormalTexture_{};
  rendering::Texture sceneDepthTexture_{};
  rendering::Texture motionVectorTexture_{};
  rendering::Texture inputDisocclusionMaskTexture_{};
  rendering::Texture currentInstanceTexture_{};
  rendering::Texture currentVoxelTexture_{};
  rendering::Texture historyColorTexture_{};
  rendering::Texture historyDepthTexture_{};
  rendering::Texture historyNormalTexture_{};
  rendering::Texture historyIdTexture_{};
  rendering::Texture historyLockStatusTexture_{};
  rendering::Texture historyLumaTexture_{};
  rendering::Texture historyDilatedMotionTexture_{};
  rendering::Texture historyAccumulationTexture_{};
  rendering::Texture historyCoarseLumaTexture_{};
  rendering::Texture outputTexture_{};
  rendering::Texture outputHistoryColorTexture_{};
  rendering::Texture outputHistoryDepthTexture_{};
  rendering::Texture outputHistoryNormalTexture_{};
  rendering::Texture outputHistoryIdTexture_{};
  rendering::Texture outputLockStatusTexture_{};
  rendering::Texture outputLumaTexture_{};
  rendering::Texture outputDilatedMotionTexture_{};
  rendering::Texture outputDisocclusionMaskTexture_{};
  rendering::Texture outputDebugTermsATexture_{};
  rendering::Texture outputDebugTermsBTexture_{};
  rendering::Texture outputAccumulationTexture_{};
  rendering::Texture outputCoarseLumaTexture_{};
  rendering::Texture preparedColorTexture_{};
  rendering::Texture dilatedDepthTexture_{};
  rendering::Texture dilatedMotionTexture_{};
  rendering::Texture lockInputLumaTexture_{};
  rendering::Texture depthClipTexture_{};
  rendering::Texture reactiveMasksTexture_{};
  rendering::Texture lockStatusScratchTexture_{};
  rendering::Texture newLocksTexture_{};
  rendering::Texture internalColorTexture_{};
  std::array<rendering::Texture, 8u> internalTextures_{};
  Settings settings_{};
  std::array<rendering::BindingsLayout, kStageCount> layouts_{};
  std::array<rendering::BindingGroups, kStageCount> bindingGroups_{};
  std::array<rendering::Shader, kStageCount> shaders_{};
  std::array<rendering::ComputePipeline, kStageCount> pipelines_{};
  rendering::Buffer uniformBuffer_{};
  rendering::Buffer reconstructedDepthBuffer_{};
  rendering::Buffer exposureBuffer_{};
};

} // namespace gpgpu
} // namespace rendering
