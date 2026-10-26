#pragma once

#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsCombineStepPassCommon.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsEikonalPass : public detail::GPUSignedDistanceFieldsCombineStepPassBase
{
public:
  explicit GPUSignedDistanceFieldsEikonalPass(GPUSignedDistanceFieldsManager &manager)
      : GPUSignedDistanceFieldsCombineStepPassBase(manager, GlobalCascadeIndex, detail::GPUSignedDistanceFieldsCombineKernel::Eikonal)
  {
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    initializeResources();
    dispatchIndirect(commandBuffer, makeUniforms(0u), sharedResources().contextIndirectArgs, GPUSignedDistanceFieldsManager::IndirectOffset_Eikonal);
  }
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
