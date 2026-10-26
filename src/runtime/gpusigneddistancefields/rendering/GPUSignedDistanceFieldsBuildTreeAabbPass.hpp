#pragma once

#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsCombineStepPassCommon.hpp"

namespace gpusigneddistancefields
{
namespace gpgpu
{

class GPUSignedDistanceFieldsBuildTreeAabbPass : public detail::GPUSignedDistanceFieldsCombineStepPassBase
{
public:
  GPUSignedDistanceFieldsBuildTreeAabbPass(GPUSignedDistanceFieldsManager &manager, uint32_t cascadeIndex, uint32_t treeIteration, bool dispatchAllCascades = false)
      : GPUSignedDistanceFieldsCombineStepPassBase(manager, cascadeIndex, detail::GPUSignedDistanceFieldsCombineKernel::BuildTreeAabb, dispatchAllCascades),
        treeIteration_(treeIteration)
  {
  }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    initializeResources();

    DispatchUniforms uniforms = makeCurrentUniforms();
    uniforms.treeIteration = treeIteration_;
    const uint32_t dispatchSize = treeIteration_ == 0u ? 16u : (treeIteration_ == 1u ? 4u : 1u);
    const uint32_t dispatchX = dispatchAllCascades() ? dispatchSize * dispatchCascadeCount() : dispatchSize;
    dispatchDirect(commandBuffer, uniforms, dispatchX, dispatchSize, dispatchSize);
  }

private:
  DispatchUniforms makeCurrentUniforms() const override
  {
    DispatchUniforms uniforms = GPUSignedDistanceFieldsCombineStepPassBase::makeCurrentUniforms();
    uniforms.treeIteration = treeIteration_;
    return uniforms;
  }

  uint32_t treeIteration_ = 0u;
};

} // namespace gpgpu
} // namespace gpusigneddistancefields
