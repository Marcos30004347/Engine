#pragma once

#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsBuildStepPassCommon.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsClearBrickEntryPass : public detail::GPUSignedDistanceFieldsBuildStepPassBase
{
public:
  GPUSignedDistanceFieldsClearBrickEntryPass(GPUSignedDistanceFieldsManager &manager, GPUSignedDistanceFieldsManager::Path path, detail::GPUSignedDistanceFieldsBuildPathSettings settings)
      : GPUSignedDistanceFieldsBuildStepPassBase(manager, path, GlobalCascadeIndex, settings, detail::GPUSignedDistanceFieldsBuildKernel::ClearBrickEntry)
  {
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    if (!isPathBuildable() || !settings().performContextBegin)
      return;

    initializeResources();
    dispatchIndirect(commandBuffer, makeUniforms(0u), sharedResources().contextIndirectArgs, GPUSignedDistanceFieldsManager::IndirectOffset_ClearBricks);
  }
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
