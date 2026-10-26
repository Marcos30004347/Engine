#pragma once

#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsBuildStepPassCommon.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsClearRefCountersPass : public detail::GPUSignedDistanceFieldsBuildStepPassBase
{
public:
  GPUSignedDistanceFieldsClearRefCountersPass(
      GPUSignedDistanceFieldsManager &manager,
      GPUSignedDistanceFieldsManager::Path path,
      uint32_t cascadeIndex,
      detail::GPUSignedDistanceFieldsBuildPathSettings settings,
      bool dispatchAllCascades = false)
      : GPUSignedDistanceFieldsBuildStepPassBase(manager, path, cascadeIndex, settings, detail::GPUSignedDistanceFieldsBuildKernel::ClearRefCounters, dispatchAllCascades)
  {
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!isPathBuildable())
      return;

    initializeResources();
    if (dispatchAllCascades())
    {
      dispatchIndirect(commandBuffer, makeCurrentUniforms(), pathResources().stageIndirectArgs, GPUSignedDistanceFieldsManager::IndirectOffset_ClearRefCounters);
      return;
    }

    if (currentJobVoxelCount() == 0u)
      return;

    dispatchDirect(commandBuffer, makeCurrentUniforms(), divCeil(GPUSignedDistanceFieldsManager::CASCADE_VOXEL_COUNT, 64u), dispatchCascadeCount(), 1u);
  }
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
