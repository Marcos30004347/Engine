#pragma once

#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsCombineStepPassCommon.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsPrepareMergeBricksArgsPass : public detail::GPUSignedDistanceFieldsCombineStepPassBase
{
public:
  GPUSignedDistanceFieldsPrepareMergeBricksArgsPass(GPUSignedDistanceFieldsManager &manager, uint32_t cascadeIndex = GlobalCascadeIndex)
      : GPUSignedDistanceFieldsCombineStepPassBase(manager, cascadeIndex, detail::GPUSignedDistanceFieldsCombineKernel::PrepareMergeBricksArgs)
  {
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    initializeResources();
    dispatchDirect(commandBuffer, makeCurrentUniforms(), 1u, 1u, 1u);
  }
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
