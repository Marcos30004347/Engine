#pragma once

#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsBuildStepPassCommon.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsCompactReferencesPass : public detail::GPUSignedDistanceFieldsBuildStepPassBase
{
public:
  GPUSignedDistanceFieldsCompactReferencesPass(
      GPUSignedDistanceFieldsManager &manager,
      GPUSignedDistanceFieldsManager::Path path,
      uint32_t cascadeIndex,
      detail::GPUSignedDistanceFieldsBuildPathSettings settings,
      bool dispatchAllCascades = false)
      : GPUSignedDistanceFieldsBuildStepPassBase(manager, path, cascadeIndex, settings, detail::GPUSignedDistanceFieldsBuildKernel::CompactReferences, dispatchAllCascades)
  {
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!isPathBuildable())
      return;

    initializeResources();
    dispatchIndirect(commandBuffer, makeCurrentUniforms(), pathResources().stageIndirectArgs, GPUSignedDistanceFieldsManager::IndirectOffset_CompactReferences);
  }
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
