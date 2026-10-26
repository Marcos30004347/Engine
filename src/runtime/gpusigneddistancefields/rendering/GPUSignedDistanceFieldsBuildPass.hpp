#pragma once

#include <string>
#include <utility>
#include <vector>

#include "gpusigneddistancefields/GPUSignedDistanceFieldsManager.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsBuildStepPassCommon.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsClearBrickEntryPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsClearBuildCountersPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsClearBrickStoragePass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsClearCountersPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsClearJobCounterPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsClearRefCountersPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsCoarseCullingPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsCollectClearBricksPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsCompactReferencesPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsCompressBricksPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsEmitSdfPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsFinalizeJobScanPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsFinalizeReferenceScanPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsInitializeCascadePass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsInvalidateJobAreasPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsMarkCascadeUninitializedPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsPrepareClearBricksPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsScanJobsPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsScanReferencesPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsScrollCascadePass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsVoxelizePass.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsBuildPass : public rendering::Pass
{
public:
  using Settings = detail::GPUSignedDistanceFieldsBuildPathSettings;

  static constexpr uint32_t kContextBeginPassCount = 4u;
  static constexpr uint32_t kCascadeStagePassCount = 17u;

  static uint32_t maxChildPassCount(uint32_t cascadeCount, Settings settings)
  {
    (void)cascadeCount;
    return (settings.performContextBegin ? kContextBeginPassCount : 0u) + kCascadeStagePassCount;
  }

  GPUSignedDistanceFieldsBuildPass(GPUSignedDistanceFieldsManager &manager, GPUSignedDistanceFieldsManager::Path path, Settings settings)
      : manager_(manager), path_(path), settings_(settings)
  {
  }

  ~GPUSignedDistanceFieldsBuildPass() override
  {
    destroyChildPasses();
  }

  void recordCommandBuffer(rendering::CommandRecorder &) override
  {
    destroyChildPasses();

    if (!manager_.isPathBuildable(path_))
      return;

    uint32_t childIndex = getIndex() + 1u;

    if (settings_.performContextBegin)
    {
      registerContextBeginChild<GPUSignedDistanceFieldsClearCountersPass>(passName + ".clearCounters", childIndex++, manager_, path_, settings_);
      registerContextBeginChild<GPUSignedDistanceFieldsCollectClearBricksPass>(passName + ".collectClearBricks", childIndex++, manager_, path_, settings_);
      registerContextBeginChild<GPUSignedDistanceFieldsPrepareClearBricksPass>(passName + ".prepareClearBricks", childIndex++, manager_, path_, settings_);
      registerContextBeginChild<GPUSignedDistanceFieldsClearBrickEntryPass>(passName + ".clearBrickEntry", childIndex++, manager_, path_, settings_);
    }

    registerCascadeChild<GPUSignedDistanceFieldsClearBuildCountersPass>(passName + ".clearBuildCounters", childIndex++, manager_, path_, 0u, settings_, true);
    registerCascadeChild<GPUSignedDistanceFieldsMarkCascadeUninitializedPass>(passName + ".markCascadeUninitialized", childIndex++, manager_, path_, 0u, settings_, true);
    registerCascadeChild<GPUSignedDistanceFieldsScrollCascadePass>(passName + ".scrollCascade", childIndex++, manager_, path_, 0u, settings_, true);
    registerCascadeChild<GPUSignedDistanceFieldsClearRefCountersPass>(passName + ".clearRefCounters", childIndex++, manager_, path_, 0u, settings_, true);
    registerCascadeChild<GPUSignedDistanceFieldsInitializeCascadePass>(passName + ".initializeCascade", childIndex++, manager_, path_, 0u, settings_, true);
    registerCascadeChild<GPUSignedDistanceFieldsClearJobCounterPass>(passName + ".clearJobCounter", childIndex++, manager_, path_, 0u, settings_, true);
    registerCascadeChild<GPUSignedDistanceFieldsInvalidateJobAreasPass>(passName + ".invalidateJobAreas", childIndex++, manager_, path_, 0u, settings_, true);
    registerCascadeChild<GPUSignedDistanceFieldsCoarseCullingPass>(passName + ".coarseCulling", childIndex++, manager_, path_, 0u, settings_, true);
    registerCascadeChild<GPUSignedDistanceFieldsScanJobsPass>(passName + ".scanJobs", childIndex++, manager_, path_, 0u, settings_, true);
    registerCascadeChild<GPUSignedDistanceFieldsFinalizeJobScanPass>(passName + ".finalizeJobScan", childIndex++, manager_, path_, 0u, settings_, true);
    registerCascadeChild<GPUSignedDistanceFieldsVoxelizePass>(passName + ".voxelize", childIndex++, manager_, path_, 0u, settings_, true);
    registerCascadeChild<GPUSignedDistanceFieldsScanReferencesPass>(passName + ".scanReferences", childIndex++, manager_, path_, 0u, settings_, true);
    registerCascadeChild<GPUSignedDistanceFieldsFinalizeReferenceScanPass>(passName + ".finalizeReferenceScan", childIndex++, manager_, path_, 0u, settings_, true);
    registerCascadeChild<GPUSignedDistanceFieldsCompactReferencesPass>(passName + ".compactReferences", childIndex++, manager_, path_, 0u, settings_, true);
    registerCascadeChild<GPUSignedDistanceFieldsClearBrickStoragePass>(passName + ".clearBrickStorage", childIndex++, manager_, path_, 0u, settings_, true);
    registerCascadeChild<GPUSignedDistanceFieldsEmitSdfPass>(passName + ".emitSdf", childIndex++, manager_, path_, 0u, settings_, true);
    registerCascadeChild<GPUSignedDistanceFieldsCompressBricksPass>(passName + ".compressBricks", childIndex++, manager_, path_, 0u, settings_, true);
  }

  // Call once per frame BEFORE run(), after sync(), to refresh per-pass uniforms.
  void preFrame()
  {
    for (detail::GPUSignedDistanceFieldsBuildStepPassBase *pass : contextBeginChildPasses_)
      pass->writeContextBeginUniforms();
    for (detail::GPUSignedDistanceFieldsBuildStepPassBase *pass : cascadeStepChildPasses_)
      pass->writeCurrentUniforms();
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    for (const detail::GPUSignedDistanceFieldsBuildStepPassBase *pass : contextBeginChildPasses_)
      pass->appendFrameOverrides(overrides, frameSlot);
    for (const detail::GPUSignedDistanceFieldsBuildStepPassBase *pass : cascadeStepChildPasses_)
      pass->appendFrameOverrides(overrides, frameSlot);
  }

  // Call this after the frame has completed on the GPU.
  // It flips the initialization flags so that future frames stop re-initializing the context and cascades.
  void postFrame()
  {
    // Flip the context initialization flag so subsequent frames get doInitialization = 0.
    manager_.markContextInitialized();

    // Mark all cascades initialized so next frame's uniforms see isInitialized=1
    // (preventing InitializeCascadeCS from re-running after the first frame).
    const uint32_t cascadeCount = manager_.getCascadeCount();
    for (uint32_t i = 0u; i < cascadeCount; ++i)
      manager_.markCascadeInitialized(i);
  }

private:
  template <typename T, typename... Args> void registerChild(const std::string &name, uint32_t index, Args &&...args)
  {
    childPasses_.push_back(renderGraph->registerPass<T>(name, index, std::forward<Args>(args)...));
  }

  // Register a context-begin (global) pass: tracked for per-frame doInitialization refresh.
  template <typename T, typename... Args> void registerContextBeginChild(const std::string &name, uint32_t index, Args &&...args)
  {
    T *pass = renderGraph->registerPass<T>(name, index, std::forward<Args>(args)...);
    childPasses_.push_back(pass);
    contextBeginChildPasses_.push_back(pass);
  }

  // Register a cascade-level step pass: tracked in both childPasses_ (for ownership/deletion)
  // and cascadeStepChildPasses_ (for per-frame uniform refresh via preFrame()).
  template <typename T, typename... Args> void registerCascadeChild(const std::string &name, uint32_t index, Args &&...args)
  {
    T *pass = renderGraph->registerPass<T>(name, index, std::forward<Args>(args)...);
    childPasses_.push_back(pass);
    cascadeStepChildPasses_.push_back(pass);
  }

  void destroyChildPasses()
  {
    for (rendering::Pass *pass : childPasses_)
      delete pass;
    childPasses_.clear();
    contextBeginChildPasses_.clear(); // same pointers — already deleted above
    cascadeStepChildPasses_.clear();  // same pointers — already deleted above
  }

  GPUSignedDistanceFieldsManager &manager_;
  GPUSignedDistanceFieldsManager::Path path_;
  Settings settings_{};
  std::vector<rendering::Pass *> childPasses_{};
  std::vector<detail::GPUSignedDistanceFieldsBuildStepPassBase *> contextBeginChildPasses_{};
  std::vector<detail::GPUSignedDistanceFieldsBuildStepPassBase *> cascadeStepChildPasses_{};
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
