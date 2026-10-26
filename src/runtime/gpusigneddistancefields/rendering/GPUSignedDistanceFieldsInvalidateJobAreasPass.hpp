#pragma once

#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsBuildStepPassCommon.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsInvalidateJobAreasPass : public detail::GPUSignedDistanceFieldsBuildStepPassBase
{
public:
  GPUSignedDistanceFieldsInvalidateJobAreasPass(
      GPUSignedDistanceFieldsManager &manager,
      GPUSignedDistanceFieldsManager::Path path,
      uint32_t cascadeIndex,
      detail::GPUSignedDistanceFieldsBuildPathSettings settings,
      bool dispatchAllCascades = false)
      : GPUSignedDistanceFieldsBuildStepPassBase(manager, path, cascadeIndex, settings, detail::GPUSignedDistanceFieldsBuildKernel::InvalidateJobAreas, dispatchAllCascades)
  {
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!isPathBuildable())
      return;

    initializeResources();
    if (dispatchAllCascades())
    {
      dispatchIndirect(commandBuffer, makeCurrentUniforms(), pathResources().stageIndirectArgs, GPUSignedDistanceFieldsManager::IndirectOffset_InvalidateJobAreas);
      return;
    }

    const uint32_t jobVoxelCount = currentJobVoxelCount();
    if (jobVoxelCount == 0u)
      return;

    const uint32_t dispatchX = divCeil(jobVoxelCount, GPUSignedDistanceFieldsManager::VOXELIZER_GROUP_SIZE);
    dispatchDirect(commandBuffer, makeCurrentUniforms(), dispatchX, dispatchCascadeCount(), 1u);
  }
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
