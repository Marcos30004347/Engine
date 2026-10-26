#include "voxel/svraster/SparseVoxelTerrainTriangleRaster.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>

namespace svr = voxel::svraster;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

svr::SparseVoxelTerrainTriangleFrustumPlane plane(
    const float x,
    const float y,
    const float z,
    const float d)
{
  svr::SparseVoxelTerrainTriangleFrustumPlane result{};
  result.normal = {x, y, z};
  result.distance = d;
  return result;
}

svr::SparseVoxelTerrainTriangleDrawRecord draw(
    const uint32_t index,
    const std::array<float, 3> &boundsMin,
    const std::array<float, 3> &boundsMax)
{
  svr::SparseVoxelTerrainTriangleDrawRecord result{};
  result.page = {0x545249414e473430ull, index};
  result.instanceIndex = 1u;
  result.clusterIndex = index;
  result.lodGroup = index;
  result.boundsMin = boundsMin;
  result.boundsMax = boundsMax;
  result.indirect.indexCount = 6u + index * 3u;
  result.indirect.instanceCount = 1u;
  result.indirect.firstIndex = index * 12u;
  result.indirect.vertexOffset = static_cast<int32_t>(index * 4u);
  result.indirect.firstInstance = index;
  return result;
}

svr::SparseVoxelTerrainTriangleRasterDrawList drawList()
{
  svr::SparseVoxelTerrainTriangleRasterDrawList result{};
  result.triangleDraws.push_back(draw(0u, {0.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.5f}));
  result.triangleDraws.push_back(draw(1u, {1.0f, 0.0f, 0.0f}, {1.5f, 0.5f, 0.5f}));
  result.triangleDraws.push_back(draw(2u, {5.0f, 0.0f, 0.0f}, {5.5f, 0.5f, 0.5f}));
  result.triangleDraws.push_back(draw(3u, {2.0f, 0.0f, 0.0f}, {2.5f, 0.5f, 0.5f}));
  return result;
}

svr::SparseVoxelTerrainTriangleCullSettings boxFrustumSettings()
{
  svr::SparseVoxelTerrainTriangleCullSettings settings{};
  settings.enableFrustum = true;
  settings.frustumPlaneCount = 4u;
  settings.frustumPlanes[0] = plane(1.0f, 0.0f, 0.0f, 0.25f);   // x >= -0.25
  settings.frustumPlanes[1] = plane(-1.0f, 0.0f, 0.0f, 4.0f);   // x <= 4
  settings.frustumPlanes[2] = plane(0.0f, 0.0f, 1.0f, 0.25f);   // z >= -0.25
  settings.frustumPlanes[3] = plane(0.0f, 0.0f, -1.0f, 2.0f);   // z <= 2
  return settings;
}

void testCullingEquivalenceWithHZBDisabled()
{
  std::cout << "triangle cluster culling is equivalent with HZB disabled\n";
  auto settings = boxFrustumSettings();
  settings.enableHZB = false;
  settings.previouslyVisible = {1u, 0u, 1u, 0u};
  settings.hzbOccluded = {0u, 1u, 0u, 1u};

  svr::SparseVoxelTerrainTriangleCullPlan plan{};
  std::string error;
  check(
      svr::buildSparseVoxelTerrainTriangleCullPlan(drawList(), settings, plan, &error),
      "HZB-disabled cull plan builds: " + error);
  check(plan.inputDrawCount == 4u, "all input draws are counted");
  check(plan.frustumRejectedCount == 1u && plan.rejectedDrawIndices.size() == 1u && plan.rejectedDrawIndices[0] == 2u, "out-of-frustum cluster is rejected");
  check(plan.hzbRejectedCount == 0u, "HZB disabled ignores occlusion flags");
  check(plan.earlyDrawIndices.size() == 1u && plan.earlyDrawIndices[0] == 0u, "previously visible in-frustum cluster enters early pass");
  check(plan.lateDrawIndices.size() == 2u && plan.lateDrawIndices[0] == 1u && plan.lateDrawIndices[1] == 3u, "remaining in-frustum clusters enter late pass");
}

void testHZBRejectsOnlyLateNonCameraCutWork()
{
  std::cout << "HZB rejects only late non-camera-cut work\n";
  auto settings = boxFrustumSettings();
  settings.enableHZB = true;
  settings.previouslyVisible = {1u, 0u, 0u, 0u};
  settings.hzbOccluded = {1u, 1u, 0u, 0u};

  svr::SparseVoxelTerrainTriangleCullPlan plan{};
  std::string error;
  check(
      svr::buildSparseVoxelTerrainTriangleCullPlan(drawList(), settings, plan, &error),
      "HZB-enabled cull plan builds: " + error);
  check(plan.earlyDrawIndices.size() == 1u && plan.earlyDrawIndices[0] == 0u, "previously visible cluster is kept for the early occluder pass even if old HZB says occluded");
  check(plan.hzbRejectedCount == 1u && plan.rejectedDrawIndices.size() == 2u, "late HZB rejection removes one in-frustum cluster plus frustum rejection");
  check(plan.lateDrawIndices.size() == 1u && plan.lateDrawIndices[0] == 3u, "non-occluded late cluster remains drawable");
  check(plan.submittedIndexCount == 6u + 15u, "submitted index count tracks early plus late work");
  check(plan.rejectedIndexCount == 9u + 12u, "rejected index count tracks HZB plus frustum work");
}

void testCameraCutDisablesPreviousHZBAndEarlyReuse()
{
  std::cout << "camera cuts disable previous visibility and HZB rejection\n";
  auto settings = boxFrustumSettings();
  settings.enableHZB = true;
  settings.cameraCut = true;
  settings.previouslyVisible = {1u, 0u, 1u, 0u};
  settings.hzbOccluded = {1u, 1u, 0u, 1u};

  svr::SparseVoxelTerrainTriangleCullPlan plan{};
  std::string error;
  check(
      svr::buildSparseVoxelTerrainTriangleCullPlan(drawList(), settings, plan, &error),
      "camera-cut cull plan builds: " + error);
  check(plan.earlyDrawIndices.empty(), "camera cut emits no previous-frame early draws");
  check(plan.hzbRejectedCount == 0u, "camera cut disables previous HZB rejection");
  check(plan.frustumRejectedCount == 1u, "camera cut still applies current frustum rejection");
  check(plan.lateDrawIndices.size() == 3u, "all in-frustum clusters are redrawn after teleport");
}

void testOccluderSeedCanPopulateEarlyPass()
{
  std::cout << "occluder seed can populate the early pass deterministically\n";
  auto settings = boxFrustumSettings();
  settings.occluderStride = 2u;

  svr::SparseVoxelTerrainTriangleCullPlan plan{};
  std::string error;
  check(
      svr::buildSparseVoxelTerrainTriangleCullPlan(drawList(), settings, plan, &error),
      "occluder-seeded cull plan builds: " + error);
  check(plan.earlyDrawIndices.size() == 1u && plan.earlyDrawIndices[0] == 0u, "in-frustum stride-selected cluster enters early pass");
  check(plan.occluderEarlyCount == 1u, "occluder early counter excludes frustum-rejected seeds");
  check(plan.frustumRejectedCount == 1u && plan.rejectedDrawIndices[0] == 2u, "out-of-frustum seed is still rejected");
}

} // namespace

int main()
{
  testCullingEquivalenceWithHZBDisabled();
  testHZBRejectsOnlyLateNonCameraCutWork();
  testCameraCutDisablesPreviousHZBAndEarlyReuse();
  testOccluderSeedCanPopulateEarlyPass();
  if (failures != 0)
  {
    std::cerr << failures << " triangle phase4 checks failed\n";
    return 1;
  }
  std::cout << "Sparse voxel terrain triangle Phase 4 culling tests passed\n";
  return 0;
}
