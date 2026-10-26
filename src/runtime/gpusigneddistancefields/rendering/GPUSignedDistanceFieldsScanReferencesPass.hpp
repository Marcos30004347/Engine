#pragma once

#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsBuildStepPassCommon.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsScanReferencesPass : public detail::GPUSignedDistanceFieldsBuildStepPassBase
{
public:
  GPUSignedDistanceFieldsScanReferencesPass(
      GPUSignedDistanceFieldsManager &manager,
      GPUSignedDistanceFieldsManager::Path path,
      uint32_t cascadeIndex,
      detail::GPUSignedDistanceFieldsBuildPathSettings settings,
      bool dispatchAllCascades = false)
      : GPUSignedDistanceFieldsBuildStepPassBase(manager, path, cascadeIndex, settings, detail::GPUSignedDistanceFieldsBuildKernel::ScanReferences, dispatchAllCascades)
  {
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!isPathBuildable())
      return;

    initializeResources();
    if (dispatchAllCascades())
    {
      dispatchIndirect(commandBuffer, makeCurrentUniforms(), pathResources().stageIndirectArgs, GPUSignedDistanceFieldsManager::IndirectOffset_ScanReferences);
      return;
    }

    if (currentJobVoxelCount() == 0u)
      return;

    dispatchDirect(
        commandBuffer,
        makeCurrentUniforms(),
        divCeil(GPUSignedDistanceFieldsManager::CASCADE_VOXEL_COUNT, GPUSignedDistanceFieldsManager::SCAN_GROUP_SIZE),
        dispatchCascadeCount(),
        1u);
  }
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
