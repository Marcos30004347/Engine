#pragma once

#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsBuildStepPassCommon.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsMarkCascadeUninitializedPass : public detail::GPUSignedDistanceFieldsBuildStepPassBase
{
public:
  GPUSignedDistanceFieldsMarkCascadeUninitializedPass(
      GPUSignedDistanceFieldsManager &manager,
      GPUSignedDistanceFieldsManager::Path path,
      uint32_t cascadeIndex,
      detail::GPUSignedDistanceFieldsBuildPathSettings settings,
      bool dispatchAllCascades = false)
      : GPUSignedDistanceFieldsBuildStepPassBase(manager, path, cascadeIndex, settings, detail::GPUSignedDistanceFieldsBuildKernel::MarkCascadeUninitialized, dispatchAllCascades)
  {
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!isPathBuildable())
      return;

    if (!dispatchAllCascades() && manager().getCascadeInfo(cascadeIndex()).isInitialized != 0u)
      return;

    if (dispatchAllCascades() && !anyCascadeNeedsInitialization())
      return;

    initializeResources();
    dispatchDirect(commandBuffer, makeCurrentUniforms(), divCeil(GPUSignedDistanceFieldsManager::CASCADE_VOXEL_COUNT, 64u), dispatchCascadeCount(), 1u);
  }
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
