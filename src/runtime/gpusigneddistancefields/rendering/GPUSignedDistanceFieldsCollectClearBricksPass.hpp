#pragma once

#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsBuildStepPassCommon.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsCollectClearBricksPass : public detail::GPUSignedDistanceFieldsBuildStepPassBase
{
public:
  GPUSignedDistanceFieldsCollectClearBricksPass(GPUSignedDistanceFieldsManager &manager, GPUSignedDistanceFieldsManager::Path path, detail::GPUSignedDistanceFieldsBuildPathSettings settings)
      : GPUSignedDistanceFieldsBuildStepPassBase(manager, path, GlobalCascadeIndex, settings, detail::GPUSignedDistanceFieldsBuildKernel::CollectClearBricks)
  {
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!isPathBuildable() || !settings().performContextBegin)
      return;

    initializeResources();
    dispatchDirect(commandBuffer, makeUniforms(0u), divCeil(static_cast<uint32_t>(sharedResources().brickVoxelMapSize / sizeof(uint32_t)), 64u), 1u, 1u);
  }
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
