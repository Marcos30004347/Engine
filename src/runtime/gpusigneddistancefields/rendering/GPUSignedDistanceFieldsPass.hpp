#pragma once

#include "gpusigneddistancefields/GPUSignedDistanceFieldsManager.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsBuildPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsCombineCascadesPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsFillBricksPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsPass : public rendering::Pass
{
public:
  struct Settings
  {
    GPUSignedDistanceFieldsBuildPass::Settings staticBuildSettings{
        .performContextBegin = true,
        .performSkinning = false,
    };
    GPUSignedDistanceFieldsBuildPass::Settings skinnedBuildSettings{
        .performContextBegin = false,
        .performSkinning = true,
    };
  };

  static uint32_t maxChildPassCount(uint32_t cascadeCount);
  static uint32_t maxChildPassCount(uint32_t cascadeCount, const Settings &settings);

  explicit GPUSignedDistanceFieldsPass(GPUSignedDistanceFieldsManager &manager) : GPUSignedDistanceFieldsPass(manager, Settings{})
  {
  }

  GPUSignedDistanceFieldsPass(GPUSignedDistanceFieldsManager &manager, Settings settings) : manager_(manager), settings_(settings)
  {
  }

  ~GPUSignedDistanceFieldsPass() override
  {
    delete combinePass_;
    combinePass_ = nullptr;
    delete fillBricksPass_;
    fillBricksPass_ = nullptr;
    delete skinnedBuildPass_;
    skinnedBuildPass_ = nullptr;
    delete staticBuildPass_;
    staticBuildPass_ = nullptr;
  }

  void recordCommandBuffer(rendering::CommandRecorder &) override
  {
    const uint32_t cascadeCount = manager_.getCascadeCount();
    const uint32_t staticBuildIndex = getIndex() + 1u;
    const uint32_t skinnedBuildIndex =
        staticBuildIndex + GPUSignedDistanceFieldsBuildPass::maxChildPassCount(cascadeCount, settings_.staticBuildSettings) + 1u;
    const uint32_t fillBricksIndex =
        skinnedBuildIndex + GPUSignedDistanceFieldsBuildPass::maxChildPassCount(cascadeCount, settings_.skinnedBuildSettings) + 1u;
    const uint32_t combineIndex = fillBricksIndex + GPUSignedDistanceFieldsFillBricksPass::kPassCount + 1u;

    staticBuildPass_ = renderGraph->registerPass<GPUSignedDistanceFieldsBuildPass>(
        passName + ".staticBuild",
        staticBuildIndex,
        manager_,
        GPUSignedDistanceFieldsManager::Path::Static,
        settings_.staticBuildSettings);
    skinnedBuildPass_ = renderGraph->registerPass<GPUSignedDistanceFieldsBuildPass>(
        passName + ".skinnedBuild",
        skinnedBuildIndex,
        manager_,
        GPUSignedDistanceFieldsManager::Path::Skinned,
        settings_.skinnedBuildSettings);
    fillBricksPass_ = renderGraph->registerPass<GPUSignedDistanceFieldsFillBricksPass>(passName + ".fillBricks", fillBricksIndex, manager_);
    combinePass_ = renderGraph->registerPass<GPUSignedDistanceFieldsCombineCascadesPass>(passName + ".combine", combineIndex, manager_);
  }

  // Call once per frame after sync() and before run() to refresh per-pass uniforms
  void preFrame()
  {
    manager_.syncFrameLocalInputs();
    if (staticBuildPass_ != nullptr)
      staticBuildPass_->preFrame();
    if (skinnedBuildPass_ != nullptr)
      skinnedBuildPass_->preFrame();
    if (fillBricksPass_ != nullptr)
      fillBricksPass_->preFrame();
    if (combinePass_ != nullptr)
      combinePass_->preFrame();
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    manager_.appendFrameOverrides(overrides, frameSlot);
    if (staticBuildPass_ != nullptr)
      staticBuildPass_->appendFrameOverrides(overrides, frameSlot);
    if (skinnedBuildPass_ != nullptr)
      skinnedBuildPass_->appendFrameOverrides(overrides, frameSlot);
    if (fillBricksPass_ != nullptr)
      fillBricksPass_->appendFrameOverrides(overrides, frameSlot);
    if (combinePass_ != nullptr)
      combinePass_->appendFrameOverrides(overrides, frameSlot);
  }

  // Call once per frame after GPU completion to flip initialization flags
  void postFrame()
  {
    if (staticBuildPass_ != nullptr)
      staticBuildPass_->postFrame();
    if (skinnedBuildPass_ != nullptr)
      skinnedBuildPass_->postFrame();
    if (combinePass_ != nullptr)
      combinePass_->postFrame();
  }

  GPUSignedDistanceFieldsBuildPass *getStaticBuildPass() const
  {
    return staticBuildPass_;
  }

  GPUSignedDistanceFieldsBuildPass *getSkinnedBuildPass() const
  {
    return skinnedBuildPass_;
  }

  GPUSignedDistanceFieldsCombineCascadesPass *getCombinePass() const
  {
    return combinePass_;
  }

  GPUSignedDistanceFieldsFillBricksPass *getFillBricksPass() const
  {
    return fillBricksPass_;
  }

private:
  GPUSignedDistanceFieldsManager &manager_;
  Settings settings_{};
  GPUSignedDistanceFieldsBuildPass *staticBuildPass_ = nullptr;
  GPUSignedDistanceFieldsBuildPass *skinnedBuildPass_ = nullptr;
  GPUSignedDistanceFieldsFillBricksPass *fillBricksPass_ = nullptr;
  GPUSignedDistanceFieldsCombineCascadesPass *combinePass_ = nullptr;
};

inline uint32_t GPUSignedDistanceFieldsPass::maxChildPassCount(uint32_t cascadeCount)
{
  return maxChildPassCount(cascadeCount, Settings{});
}

inline uint32_t GPUSignedDistanceFieldsPass::maxChildPassCount(uint32_t cascadeCount, const Settings &settings)
{
  const uint32_t staticBuildChildCount =
      GPUSignedDistanceFieldsBuildPass::maxChildPassCount(cascadeCount, settings.staticBuildSettings);
  const uint32_t skinnedBuildChildCount =
      GPUSignedDistanceFieldsBuildPass::maxChildPassCount(cascadeCount, settings.skinnedBuildSettings);
  const uint32_t combineChildCount = GPUSignedDistanceFieldsCombineCascadesPass::maxChildPassCount(cascadeCount);

  // Four direct child passes plus the maximum children that each composite pass can register.
  return 4u + staticBuildChildCount + skinnedBuildChildCount + GPUSignedDistanceFieldsFillBricksPass::kPassCount + combineChildCount;
}

} // namespace gpgpu
} // namespace gpusigneddistancefields
