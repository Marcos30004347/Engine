#pragma once

#include <string>
#include <utility>
#include <vector>

#include "gpusigneddistancefields/GPUSignedDistanceFieldsManager.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsCombineStepPassCommon.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsCollectDirtyBricksPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsEikonalPass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsPrepareEikonalArgsPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsFillBricksPass : public rendering::Pass
{
public:
  static constexpr uint32_t kPassCount = 3u;

  explicit GPUSignedDistanceFieldsFillBricksPass(GPUSignedDistanceFieldsManager &manager) : manager_(manager)
  {
  }

  ~GPUSignedDistanceFieldsFillBricksPass() override
  {
    destroyChildPasses();
  }

  void recordCommandBuffer(rendering::CommandRecorder &) override
  {
    destroyChildPasses();

    uint32_t childIndex = getIndex() + 1u;
    registerCombineChild<GPUSignedDistanceFieldsCollectDirtyBricksPass>(passName + ".collectDirtyBricks", childIndex++, manager_);
    registerCombineChild<GPUSignedDistanceFieldsPrepareEikonalArgsPass>(passName + ".prepareEikonalArgs", childIndex++, manager_);
    registerCombineChild<GPUSignedDistanceFieldsEikonalPass>(passName + ".eikonal", childIndex++, manager_);
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

private:
  template <typename T, typename... Args> void registerChild(const std::string &name, uint32_t index, Args &&...args)
  {
    childPasses_.push_back(renderGraph->registerPass<T>(name, index, std::forward<Args>(args)...));
  }

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
