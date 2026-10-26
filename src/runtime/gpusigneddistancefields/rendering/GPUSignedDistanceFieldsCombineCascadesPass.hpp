#pragma once

#include <string>
#include <utility>
#include <vector>

#include "gpusigneddistancefields/GPUSignedDistanceFieldsManager.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsBuildTreeAabbPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsMergeBricksPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsMergeCascadesStepPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsPrepareMergeBricksArgsPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsCombineCascadesPass : public rendering::Pass
{
public:
  static constexpr uint32_t kPassCount = 6u;

  static uint32_t maxChildPassCount(uint32_t cascadeCount)
  {
    (void)cascadeCount;
    return kPassCount;
  }

  explicit GPUSignedDistanceFieldsCombineCascadesPass(GPUSignedDistanceFieldsManager &manager) : manager_(manager)
  {
  }

  ~GPUSignedDistanceFieldsCombineCascadesPass() override
  {
    destroyChildPasses();
  }

  void recordCommandBuffer(rendering::CommandRecorder &) override
  {
    destroyChildPasses();

    uint32_t childIndex = getIndex() + 1u;
    registerCombineChild<GPUSignedDistanceFieldsMergeCascadesStepPass>(passName + ".mergeCascades", childIndex++, manager_, 0u, true);
    registerCombineChild<GPUSignedDistanceFieldsPrepareMergeBricksArgsPass>(passName + ".prepareMergeBricksArgs", childIndex++, manager_);
    registerCombineChild<GPUSignedDistanceFieldsMergeBricksPass>(passName + ".mergeBricks", childIndex++, manager_);
    registerCombineChild<GPUSignedDistanceFieldsBuildTreeAabbPass>(passName + ".buildTreeAabb.bottom", childIndex++, manager_, 0u, 0u, true);
    registerCombineChild<GPUSignedDistanceFieldsBuildTreeAabbPass>(passName + ".buildTreeAabb.mid", childIndex++, manager_, 0u, 1u, true);
    registerCombineChild<GPUSignedDistanceFieldsBuildTreeAabbPass>(passName + ".buildTreeAabb", childIndex++, manager_, 0u, 2u, true);
  }

  void preFrame()
  {
    for (detail::GPUSignedDistanceFieldsCombineStepPassBase *pass : combineStepChildPasses_)
      pass->writeCurrentUniforms();
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    for (const detail::GPUSignedDistanceFieldsCombineStepPassBase *pass : combineStepChildPasses_)
      pass->appendFrameOverrides(overrides, frameSlot);
  }

  void postFrame()
  {
  }

private:
  template <typename T, typename... Args> void registerCombineChild(const std::string &name, uint32_t index, Args &&...args)
  {
    T *pass = renderGraph->registerPass<T>(name, index, std::forward<Args>(args)...);
    childPasses_.push_back(pass);
    combineStepChildPasses_.push_back(pass);
  }

  void destroyChildPasses()
  {
    for (rendering::Pass *pass : childPasses_)
      delete pass;
    childPasses_.clear();
    combineStepChildPasses_.clear();
  }

  GPUSignedDistanceFieldsManager &manager_;
  std::vector<rendering::Pass *> childPasses_{};
  std::vector<detail::GPUSignedDistanceFieldsCombineStepPassBase *> combineStepChildPasses_{};
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
