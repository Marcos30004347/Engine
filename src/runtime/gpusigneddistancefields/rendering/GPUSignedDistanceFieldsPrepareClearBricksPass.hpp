#pragma once

#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsBuildStepPassCommon.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsPrepareClearBricksPass : public detail::GPUSignedDistanceFieldsBuildStepPassBase
{
public:
  GPUSignedDistanceFieldsPrepareClearBricksPass(GPUSignedDistanceFieldsManager &manager, GPUSignedDistanceFieldsManager::Path path, detail::GPUSignedDistanceFieldsBuildPathSettings settings)
      : GPUSignedDistanceFieldsBuildStepPassBase(manager, path, GlobalCascadeIndex, settings, detail::GPUSignedDistanceFieldsBuildKernel::PrepareClearBricks)
  {
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!isPathBuildable() || !settings().performContextBegin)
      return;

    initializeResources();
    dispatchDirect(commandBuffer, makeUniforms(0u), 1u, 1u, 1u);
  }
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
