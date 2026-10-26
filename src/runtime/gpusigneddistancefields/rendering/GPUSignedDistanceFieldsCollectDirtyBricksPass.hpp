#pragma once

#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsCombineStepPassCommon.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsCollectDirtyBricksPass : public detail::GPUSignedDistanceFieldsCombineStepPassBase
{
public:
  explicit GPUSignedDistanceFieldsCollectDirtyBricksPass(GPUSignedDistanceFieldsManager &manager)
      : GPUSignedDistanceFieldsCombineStepPassBase(manager, GlobalCascadeIndex, detail::GPUSignedDistanceFieldsCombineKernel::CollectDirtyBricks)
  {
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    initializeResources();
    dispatchDirect(commandBuffer, makeUniforms(0u), divCeil(manager().getSettings().maxBricks, 64u), 1u, 1u);
  }
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
