#pragma once

#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsCombineStepPassCommon.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsMergeBricksPass : public detail::GPUSignedDistanceFieldsCombineStepPassBase
{
public:
  GPUSignedDistanceFieldsMergeBricksPass(GPUSignedDistanceFieldsManager &manager, uint32_t cascadeIndex = GlobalCascadeIndex)
      : GPUSignedDistanceFieldsCombineStepPassBase(manager, cascadeIndex, detail::GPUSignedDistanceFieldsCombineKernel::MergeBricks)
  {
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    initializeResources();
    dispatchIndirect(commandBuffer, makeCurrentUniforms(), sharedResources().contextIndirectArgs, GPUSignedDistanceFieldsManager::IndirectOffset_MergeBricks);
  }
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
