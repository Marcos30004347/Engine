#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpusigneddistancefields/GPUSignedDistanceFieldsManager.hpp"
#include "gpusigneddistancefields/GPUSignedDistanceFieldsSceneManager.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "softwarebvh/SoftwareBLASManager.hpp"
#include "softwarebvh/SoftwareBLASSceneBridge.hpp"
#include "softwarebvh/SoftwareTLASManager.hpp"
#include "softwarebvh/SoftwareTLASSceneBridge.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

namespace engine
{

class SceneManager
{
public:
  struct RaytraceScene
  {
    softwarebvh::SoftwareBLASManager *softwareBLAS = nullptr;
    softwarebvh::SoftwareTLASManager *softwareTLAS = nullptr;

    bool isReady() const
    {
      return softwareBLAS != nullptr && softwareTLAS != nullptr;
    }
  };

  struct SceneInstanceAssociation
  {
    virtualgeometry::InstanceId objectInstanceId = virtualgeometry::INVALID_INSTANCE_ID;
    virtualgeometry::SceneInstanceId sceneInstanceId = virtualgeometry::INVALID_SCENE_INSTANCE_ID;
    uint32_t shapeIndex = UINT32_MAX;
  };

  void bindVirtualScene(virtualgeometry::VirtualGeometryScene *scene)
  {
    virtualScene_ = scene;
  }

  void bindRaytraceScene(const RaytraceScene &scene)
  {
    raytraceScene_ = scene;
  }

  void clearRaytraceScene()
  {
    raytraceScene_ = RaytraceScene{};
  }

  void createSoftwareRaytraceScene(rendering::RenderGraph *renderGraph)
  {
    clearSoftwareRaytraceScene();
    if (renderGraph == nullptr)
      return;

    softwareBLAS_ = std::make_unique<softwarebvh::SoftwareBLASManager>(renderGraph);
    softwareBLASBridge_ = std::make_unique<softwarebvh::SoftwareBLASSceneBridge>(*softwareBLAS_);
    softwareTLAS_ = std::make_unique<softwarebvh::SoftwareTLASManager>(renderGraph);
    softwareTLASBridge_ = std::make_unique<softwarebvh::SoftwareTLASSceneBridge>(*softwareTLAS_, softwareBLAS_.get());
    bindRaytraceScene(RaytraceScene{
        .softwareBLAS = softwareBLAS_.get(),
        .softwareTLAS = softwareTLAS_.get(),
    });
    if (virtualScene_ != nullptr)
      softwareBLASBridge_->rebuildFromScene(*virtualScene_);
  }

  void clearSoftwareRaytraceScene()
  {
    softwareTLASBridge_.reset();
    softwareTLAS_.reset();
    softwareBLASBridge_.reset();
    softwareBLAS_.reset();
    clearRaytraceScene();
  }

  softwarebvh::SoftwareBLASManager *getSoftwareBLASManager() const
  {
    return softwareBLAS_.get();
  }

  softwarebvh::SoftwareBLASSceneBridge *getSoftwareBLASSceneBridge() const
  {
    return softwareBLASBridge_.get();
  }

  softwarebvh::SoftwareTLASManager *getSoftwareTLASManager() const
  {
    return softwareTLAS_.get();
  }

  softwarebvh::SoftwareTLASSceneBridge *getSoftwareTLASSceneBridge() const
  {
    return softwareTLASBridge_.get();
  }

  bool syncSoftwareRaytraceScene(
      const softwarebvh::SoftwareBLASSceneBridge::Settings &blasSettings,
      const softwarebvh::SoftwareTLASSceneBridge::DebugInstanceFilter &tlasFilter)
  {
    const bool blasSynced = syncSoftwareBLASScene(blasSettings);
    const bool tlasSynced = syncSoftwareTLASScene(tlasFilter);
    return blasSynced && tlasSynced;
  }

  bool syncSoftwareBLASScene(const softwarebvh::SoftwareBLASSceneBridge::Settings &settings)
  {
    if (virtualScene_ == nullptr || softwareBLASBridge_ == nullptr)
      return false;

    softwareBLASBridge_->setSettings(settings);
    softwareBLASBridge_->sync(*virtualScene_);
    return true;
  }

  bool syncSoftwareTLASScene(const softwarebvh::SoftwareTLASSceneBridge::DebugInstanceFilter &filter)
  {
    if (virtualScene_ == nullptr || softwareTLASBridge_ == nullptr)
      return false;

    softwareTLASBridge_->setDebugInstanceFilter(filter);
    softwareTLASBridge_->sync(*virtualScene_);
    rebuildAssociationsFromVirtualScene();
    return true;
  }

  void createSignedDistanceFieldScene(
      rendering::RenderGraph *renderGraph,
      const gpusigneddistancefields::GPUSignedDistanceFieldsManager::Settings &managerSettings,
      const gpusigneddistancefields::GPUSignedDistanceFieldsSceneManager::Settings &sceneSettings)
  {
    clearSignedDistanceFieldScene();
    if (renderGraph == nullptr)
      return;

    gpuSignedDistanceFieldsManager_ = std::make_unique<gpusigneddistancefields::GPUSignedDistanceFieldsManager>(renderGraph, managerSettings);
    gpuSignedDistanceFieldsSceneManager_ = std::make_unique<gpusigneddistancefields::GPUSignedDistanceFieldsSceneManager>(*gpuSignedDistanceFieldsManager_, sceneSettings);
  }

  void clearSignedDistanceFieldScene()
  {
    gpuSignedDistanceFieldsSceneManager_.reset();
    gpuSignedDistanceFieldsManager_.reset();
  }

  gpusigneddistancefields::GPUSignedDistanceFieldsManager *getSignedDistanceFieldManager() const
  {
    return gpuSignedDistanceFieldsManager_.get();
  }

  gpusigneddistancefields::GPUSignedDistanceFieldsSceneManager *getSignedDistanceFieldSceneManager() const
  {
    return gpuSignedDistanceFieldsSceneManager_.get();
  }

  bool syncSignedDistanceFieldScene(const math::Vec3f &clipmapCenter)
  {
    if (virtualScene_ == nullptr || gpuSignedDistanceFieldsSceneManager_ == nullptr)
      return false;

    gpuSignedDistanceFieldsSceneManager_->sync(*virtualScene_, clipmapCenter);
    return true;
  }

  virtualgeometry::VirtualGeometryScene *getVirtualScene() const
  {
    return virtualScene_;
  }

  void registerObjectForStreaming(const std::string &name, const std::string &file)
  {
    if (virtualScene_ != nullptr)
      virtualScene_->registerObjectForStreaming(name, file);
  }

  virtualgeometry::InstanceId instantiateObjectInstance(
      const std::string &objectName,
      const math::Vec3f &position,
      const math::Quatf &rotation,
      float scale = 1.0f,
      uint32_t materialIndexOverride = UINT32_MAX)
  {
    if (virtualScene_ == nullptr)
      return virtualgeometry::INVALID_INSTANCE_ID;

    const virtualgeometry::InstanceId instanceId = virtualScene_->instantiateObjectInstance(objectName, position, rotation, scale, materialIndexOverride);
    rebuildAssociationsFromVirtualScene();
    if (softwareBLASBridge_ != nullptr)
      softwareBLASBridge_->registerObjectInstance(*virtualScene_, instanceId);
    return instanceId;
  }

  bool updateObjectInstanceTransform(
      virtualgeometry::InstanceId instanceId,
      const math::Vec3f &position,
      const math::Quatf &rotation,
      float scale)
  {
    return virtualScene_ != nullptr && virtualScene_->updateObjectInstanceTransform(instanceId, position, rotation, scale);
  }

  bool applyAnimationFrame(
      virtualgeometry::InstanceId instanceId,
      virtualgeometry::animation::AnimationPlayer &animationPlayer,
      const std::string &animationName,
      float timeSeconds,
      bool looping = true)
  {
    return virtualScene_ != nullptr && virtualScene_->applyAnimationFrame(instanceId, animationPlayer, animationName, timeSeconds, looping);
  }

  void updateInstanceBuffer(bool uploadAllFrameSlots = false)
  {
    if (virtualScene_ != nullptr)
      virtualScene_->updateInstanceBuffer(uploadAllFrameSlots);
  }

  void prepareFrame(uint32_t frameSlot)
  {
    if (virtualScene_ != nullptr)
      virtualScene_->prepareFrame(frameSlot);
  }

  void updatePageStreaming(const std::string &objectName, uint32_t maxPageInstallsPerUpdate = UINT32_MAX)
  {
    if (virtualScene_ != nullptr)
      virtualScene_->updatePageStreaming(objectName, maxPageInstallsPerUpdate);
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    if (virtualScene_ != nullptr)
      virtualScene_->appendFrameOverrides(overrides, frameSlot);
  }

  void setStreamingResidencyInvalidationCallback(std::function<void(const virtualgeometry::AABB &)> callback)
  {
    if (virtualScene_ != nullptr)
      virtualScene_->setStreamingResidencyInvalidationCallback(std::move(callback));
  }

  size_t getObjectInstanceCount() const
  {
    return virtualScene_ != nullptr ? virtualScene_->objectInstances.size() : 0u;
  }

  virtualgeometry::InstanceId getObjectInstanceIdByDenseIndex(uint32_t denseIndex) const
  {
    if (virtualScene_ == nullptr || denseIndex >= virtualScene_->objectDenseToSparse.size())
      return virtualgeometry::INVALID_INSTANCE_ID;

    return virtualScene_->objectDenseToSparse[denseIndex];
  }

  virtualgeometry::VirtualGeometryScene::ObjectInstanceRecord *getObjectInstanceRecordByDenseIndex(uint32_t denseIndex) const
  {
    if (virtualScene_ == nullptr || denseIndex >= virtualScene_->objectInstances.size())
      return nullptr;

    return &virtualScene_->objectInstances[denseIndex];
  }

  bool setObjectInstanceSdfDynamic(virtualgeometry::InstanceId instanceId, bool dynamic)
  {
    return virtualScene_ != nullptr && virtualScene_->setObjectInstanceSdfDynamic(instanceId, dynamic);
  }

  bool isObjectInstanceSdfDynamic(virtualgeometry::InstanceId instanceId) const
  {
    return virtualScene_ != nullptr && virtualScene_->isObjectInstanceSdfDynamic(instanceId);
  }

  bool requestObjectInstanceSdfRebuild(virtualgeometry::InstanceId instanceId)
  {
    return virtualScene_ != nullptr && virtualScene_->requestObjectInstanceSdfRebuild(instanceId);
  }

  const RaytraceScene &getRaytraceScene() const
  {
    return raytraceScene_;
  }

  bool hasVirtualScene() const
  {
    return virtualScene_ != nullptr;
  }

  bool hasRaytraceScene() const
  {
    return raytraceScene_.isReady();
  }

  void clearSceneInstanceAssociations()
  {
    associationsBySceneInstance_.clear();
  }

  void associateSceneInstance(
      virtualgeometry::InstanceId objectInstanceId,
      virtualgeometry::SceneInstanceId sceneInstanceId,
      uint32_t shapeIndex)
  {
    if (sceneInstanceId == virtualgeometry::INVALID_SCENE_INSTANCE_ID)
      return;

    associationsBySceneInstance_[sceneInstanceId] = SceneInstanceAssociation{
        .objectInstanceId = objectInstanceId,
        .sceneInstanceId = sceneInstanceId,
        .shapeIndex = shapeIndex,
    };
  }

  bool findAssociation(
      virtualgeometry::SceneInstanceId sceneInstanceId,
      SceneInstanceAssociation &outAssociation) const
  {
    const auto it = associationsBySceneInstance_.find(sceneInstanceId);
    if (it == associationsBySceneInstance_.end())
      return false;

    outAssociation = it->second;
    return true;
  }

  void rebuildAssociationsFromVirtualScene()
  {
    associationsBySceneInstance_.clear();
    if (virtualScene_ == nullptr)
      return;

    for (uint32_t denseIndex = 0u; denseIndex < static_cast<uint32_t>(virtualScene_->objectInstances.size()); ++denseIndex)
    {
      if (denseIndex >= virtualScene_->objectDenseToSparse.size())
        continue;

      const virtualgeometry::InstanceId objectInstanceId = virtualScene_->objectDenseToSparse[denseIndex];
      if (objectInstanceId == virtualgeometry::INVALID_INSTANCE_ID)
        continue;

      const virtualgeometry::VirtualGeometryScene::ObjectInstanceRecord &record = virtualScene_->objectInstances[denseIndex];
      for (uint32_t shapeIndex = 0u; shapeIndex < static_cast<uint32_t>(record.sceneInstanceIds.size()); ++shapeIndex)
      {
        associateSceneInstance(objectInstanceId, record.sceneInstanceIds[shapeIndex], shapeIndex);
      }
    }
  }

  std::vector<virtualgeometry::VirtualGeometryScene::SceneInstanceTransformUpdate> consumeVirtualSceneTransformUpdates(uint32_t frameSlot)
  {
    if (virtualScene_ == nullptr)
      return {};

    return virtualScene_->consumeFrameTransformUpdates(frameSlot);
  }

  size_t associationCount() const
  {
    return associationsBySceneInstance_.size();
  }

private:
  virtualgeometry::VirtualGeometryScene *virtualScene_ = nullptr;
  RaytraceScene raytraceScene_{};
  std::unique_ptr<softwarebvh::SoftwareBLASManager> softwareBLAS_;
  std::unique_ptr<softwarebvh::SoftwareBLASSceneBridge> softwareBLASBridge_;
  std::unique_ptr<softwarebvh::SoftwareTLASManager> softwareTLAS_;
  std::unique_ptr<softwarebvh::SoftwareTLASSceneBridge> softwareTLASBridge_;
  std::unique_ptr<gpusigneddistancefields::GPUSignedDistanceFieldsManager> gpuSignedDistanceFieldsManager_;
  std::unique_ptr<gpusigneddistancefields::GPUSignedDistanceFieldsSceneManager> gpuSignedDistanceFieldsSceneManager_;
  std::unordered_map<virtualgeometry::SceneInstanceId, SceneInstanceAssociation> associationsBySceneInstance_;
};

} // namespace engine
