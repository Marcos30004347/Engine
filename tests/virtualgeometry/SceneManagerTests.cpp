#include <cassert>
#include <cstdint>

#include "scene/SceneManager.hpp"

int main()
{
  engine::SceneManager sceneManager;
  assert(!sceneManager.hasVirtualScene());
  assert(!sceneManager.hasRaytraceScene());
  assert(sceneManager.associationCount() == 0u);

  sceneManager.bindRaytraceScene(engine::SceneManager::RaytraceScene{
      .softwareBLAS = reinterpret_cast<softwarebvh::SoftwareBLASManager *>(uintptr_t(1u)),
      .softwareTLAS = reinterpret_cast<softwarebvh::SoftwareTLASManager *>(uintptr_t(2u)),
  });
  assert(sceneManager.hasRaytraceScene());

  sceneManager.associateSceneInstance(7u, 42u, 3u);
  assert(sceneManager.associationCount() == 1u);

  engine::SceneManager::SceneInstanceAssociation association{};
  assert(sceneManager.findAssociation(42u, association));
  assert(association.objectInstanceId == 7u);
  assert(association.sceneInstanceId == 42u);
  assert(association.shapeIndex == 3u);

  sceneManager.associateSceneInstance(9u, 42u, 5u);
  assert(sceneManager.associationCount() == 1u);
  assert(sceneManager.findAssociation(42u, association));
  assert(association.objectInstanceId == 9u);
  assert(association.shapeIndex == 5u);

  sceneManager.associateSceneInstance(11u, virtualgeometry::INVALID_SCENE_INSTANCE_ID, 1u);
  assert(sceneManager.associationCount() == 1u);

  sceneManager.clearSceneInstanceAssociations();
  assert(sceneManager.associationCount() == 0u);
  assert(!sceneManager.findAssociation(42u, association));

  sceneManager.clearRaytraceScene();
  assert(!sceneManager.hasRaytraceScene());
  assert(sceneManager.consumeVirtualSceneTransformUpdates(0u).empty());
  return 0;
}
