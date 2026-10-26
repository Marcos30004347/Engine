#pragma once

#include "rendering/gpgpu/MetalFXPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/runtime/RenderGraphRuntimeResourcesManager.hpp"
#include <stdexcept>

#if defined(__APPLE__)
namespace rendering::gpgpu
{
// Reconstruction is an intermediate HDR texture producer. Effects, readbacks,
// overlays and the final display pass consume output through normal graph edges.
// Native scaler lifetime must cover this pass and all submitted graph frames.
class MetalFXReconstructionPass final : public Pass
{
public:
  struct Textures
  {
    Texture color, depth, motion, output, diffuseAlbedo, normal;
  };
  using Frame = MetalFXTemporalDenoisedScalerPass::Frame;

  MetalFXReconstructionPass(Textures textures, MetalFXTemporalUpscalerPass *upscaler,
                           MetalFXTemporalDenoisedScalerPass *denoiser)
      : textures_(std::move(textures)), upscaler_(upscaler), denoiser_(denoiser)
  {
    if ((upscaler == nullptr) == (denoiser == nullptr))
      throw std::invalid_argument("MetalFX reconstruction requires exactly one scaler");
    for (auto texture : {textures_.color, textures_.depth, textures_.motion, textures_.output})
      if (!texture.isValid()) throw std::invalid_argument("MetalFX reconstruction requires complete textures");
    if (denoiser && (!textures_.diffuseAlbedo.isValid() || !textures_.normal.isValid()))
      throw std::invalid_argument("MetalFX denoising requires albedo and normals");
  }

  // Set before graph recording; prepare snapshots camera/history parameters so
  // later CPU frames cannot mutate an already queued native submission.
  void setFrame(const Frame &frame) { *frame_ = frame; }
  Texture output() const { return textures_.output; }

  void recordCommandBuffer(CommandRecorder &recorder) override
  {
    recorder.recorded.back().commands.push_back(
        std::make_shared<NativeCommand>(textures_, upscaler_, denoiser_, frame_));
  }

private:
  class NativeCommand final : public Command
  {
  public:
    NativeCommand(Textures textures, MetalFXTemporalUpscalerPass *upscaler,
                  MetalFXTemporalDenoisedScalerPass *denoiser, std::shared_ptr<Frame> frame)
        : textures_(std::move(textures)), upscaler_(upscaler), denoiser_(denoiser), frame_(std::move(frame)) {}
    std::shared_ptr<Command> clone() const override { return std::make_shared<NativeCommand>(*this); }
    Queue queueHint() const override { return Queue::Compute; }
    bool triggersNodeSplit() const override { return true; }
    void externalTextureViews(std::vector<TextureView> &views) const override
    {
      for (auto texture : {textures_.color, textures_.depth, textures_.motion,
                           textures_.output, textures_.diffuseAlbedo, textures_.normal})
      {
        if (!texture.isValid()) continue;
        TextureView view{};
        view.texture = texture;
        view.flags = texture == textures_.depth ? ImageAspectFlags::Depth : ImageAspectFlags::Color;
        view.access = texture == textures_.output ? AccessPattern::SHADER_WRITE : AccessPattern::SHADER_READ;
        view.layout = ResourceLayout::GENERAL;
        views.push_back(view);
      }
    }
    void prepare(CommandPrepareContext &context) override
    {
      if (!context.runtimeResources) throw std::runtime_error("MetalFX requires graph texture resolution");
      prepared_ = *frame_;
      auto resolve = [&](const Texture &texture) {
        return texture.isValid() ? context.runtimeResources->resolveTexture(texture) : TextureId::Invalid;
      };
      prepared_.color = resolve(textures_.color);
      prepared_.depth = resolve(textures_.depth);
      prepared_.motion = resolve(textures_.motion);
      prepared_.output = resolve(textures_.output);
      prepared_.diffuseAlbedo = resolve(textures_.diffuseAlbedo);
      prepared_.normal = resolve(textures_.normal);
    }
    void run(CommandRunContext &context) const override
    {
      context.rhi->cmdMetalCompute(context.commandBuffer,
          [frame = prepared_, upscaler = upscaler_, denoiser = denoiser_](void *event, uint64_t ready, uint64_t done) mutable {
            frame.synchronization = {event, ready, done};
            std::string error;
            const bool ok = denoiser ? denoiser->encode(frame, &error) : upscaler->encode(frame, &error);
            if (!ok) throw std::runtime_error("MetalFX reconstruction failed: " + error);
          });
    }
  private:
    Textures textures_;
    MetalFXTemporalUpscalerPass *upscaler_;
    MetalFXTemporalDenoisedScalerPass *denoiser_;
    std::shared_ptr<Frame> frame_;
    Frame prepared_{};
  };
  Textures textures_;
  MetalFXTemporalUpscalerPass *upscaler_;
  MetalFXTemporalDenoisedScalerPass *denoiser_;
  std::shared_ptr<Frame> frame_ = std::make_shared<Frame>();
};
// Interpolation is a graph producer too: post effects consume generated color.
class MetalFXInterpolationPass final : public Pass
{
public:
  struct Textures
  {
    Texture currentColor, previousColor, depth, motion, output;
  };
  using Frame = MetalFXFrameGenerationPass::Frame;

  MetalFXInterpolationPass(Textures textures, MetalFXFrameGenerationPass *interpolator)
      : textures_(std::move(textures)), interpolator_(interpolator) {}

  // Set before graph recording; prepare snapshots camera/history parameters so
  // later CPU frames cannot mutate an already queued native submission.
  void setFrame(const Frame &frame) { *frame_ = frame; }
  Texture output() const { return textures_.output; }

  void recordCommandBuffer(CommandRecorder &recorder) override
  {
    recorder.recorded.back().commands.push_back(
        std::make_shared<NativeCommand>(textures_, interpolator_, frame_));
  }

private:
  class NativeCommand final : public Command
  {
  public:
    NativeCommand(Textures textures, MetalFXFrameGenerationPass *interpolator, std::shared_ptr<Frame> frame)
        : textures_(std::move(textures)), interpolator_(interpolator), frame_(std::move(frame)) {}
    std::shared_ptr<Command> clone() const override { return std::make_shared<NativeCommand>(*this); }
    Queue queueHint() const override { return Queue::Compute; }
    bool triggersNodeSplit() const override { return true; }
    void externalTextureViews(std::vector<TextureView> &views) const override
    {
      for (auto texture : {textures_.currentColor, textures_.previousColor, textures_.depth, textures_.motion, textures_.output})
      {
        if (!texture.isValid()) continue;
        TextureView view{};
        view.texture = texture;
        view.flags = texture == textures_.depth ? ImageAspectFlags::Depth : ImageAspectFlags::Color;
        view.access = texture == textures_.output ? AccessPattern::SHADER_WRITE : AccessPattern::SHADER_READ;
        view.layout = ResourceLayout::GENERAL;
        views.push_back(view);
      }
    }
    void prepare(CommandPrepareContext &context) override
    {
      if (!context.runtimeResources) throw std::runtime_error("MetalFX requires graph texture resolution");
      prepared_ = *frame_;
      auto resolve = [&](const Texture &texture) {
        return texture.isValid() ? context.runtimeResources->resolveTexture(texture) : TextureId::Invalid;
      };
      prepared_.currentColor = resolve(textures_.currentColor);
      prepared_.previousColor = resolve(textures_.previousColor);
      prepared_.depth = resolve(textures_.depth);
      prepared_.motion = resolve(textures_.motion);
      prepared_.output = resolve(textures_.output);
    }
    void run(CommandRunContext &context) const override
    {
      context.rhi->cmdMetalCompute(context.commandBuffer,
          [frame = prepared_, interpolator = interpolator_](void *event, uint64_t ready, uint64_t done) mutable {
            frame.synchronization = {event, ready, done};
            std::string error;
            const bool ok = interpolator->encode(frame, &error);
            if (!ok) throw std::runtime_error("MetalFX interpolation failed: " + error);
          });
    }
  private:
    Textures textures_;
    MetalFXFrameGenerationPass *interpolator_;
    std::shared_ptr<Frame> frame_;
    Frame prepared_{};
  };
  Textures textures_;
  MetalFXFrameGenerationPass *interpolator_;
  std::shared_ptr<Frame> frame_ = std::make_shared<Frame>();
};
} // namespace rendering::gpgpu
#endif
