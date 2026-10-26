#pragma once

#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsCombineStepPassCommon.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsMergeCascadesStepPass : public detail::GPUSignedDistanceFieldsCombineStepPassBase
{
public:
  GPUSignedDistanceFieldsMergeCascadesStepPass(GPUSignedDistanceFieldsManager &manager, uint32_t cascadeIndex, bool dispatchAllCascades = false)
      : GPUSignedDistanceFieldsCombineStepPassBase(manager, cascadeIndex, detail::GPUSignedDistanceFieldsCombineKernel::MergeCascades, dispatchAllCascades)
  {
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    initializeResources();
    dispatchDirect(commandBuffer, makeCurrentUniforms(), divCeil(GPUSignedDistanceFieldsManager::CASCADE_VOXEL_COUNT, 64u), dispatchCascadeCount(), 1u);
  }
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
