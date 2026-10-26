#pragma once

#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsCombineStepPassCommon.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsPrepareEikonalArgsPass : public detail::GPUSignedDistanceFieldsCombineStepPassBase
{
public:
  explicit GPUSignedDistanceFieldsPrepareEikonalArgsPass(GPUSignedDistanceFieldsManager &manager)
      : GPUSignedDistanceFieldsCombineStepPassBase(manager, GlobalCascadeIndex, detail::GPUSignedDistanceFieldsCombineKernel::PrepareEikonalArgs)
  {
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    initializeResources();
    dispatchDirect(commandBuffer, makeUniforms(0u), 1u, 1u, 1u);
  }
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
