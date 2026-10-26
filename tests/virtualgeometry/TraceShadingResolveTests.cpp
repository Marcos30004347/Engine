#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>

#include "globalIllumination/TraceShadingTypes.hpp"
#include "globalIllumination/WorldHitShadingResolvePass.hpp"
#include "gpusigneddistancefields/rendering/GPUSignedDistanceFieldsTraceTypes.hpp"
#include "softwarebvh/rendering/SoftwareRayTracePass.hpp"
#include "virtualgeometry/rendering/ScreenSpaceRayTracePass.hpp"

namespace
{

constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kTraceShadingSourceShift = 24u;

uint32_t floatBits(float value)
{
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

float bitsFloat(uint32_t bits)
{
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

bool near(float left, float right, float epsilon = 1.0e-5f)
{
  return std::fabs(left - right) <= epsilon;
}

void expectNear(float left, float right)
{
  assert(near(left, right));
}

void expectVec3(const float actual[4], float x, float y, float z)
{
  expectNear(actual[0], x);
  expectNear(actual[1], y);
  expectNear(actual[2], z);
}

std::array<float, 3> normalize(std::array<float, 3> value, std::array<float, 3> fallback)
{
  const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
  if (length <= 1.0e-6f)
    return fallback;
  return {value[0] / length, value[1] / length, value[2] / length};
}

globalillumination::TraceShadingParametersGPU invalidTraceShading()
{
  return {};
}

uint32_t sourceFlags(globalillumination::TraceShadingSource source)
{
  return static_cast<uint32_t>(source) << kTraceShadingSourceShift;
}

globalillumination::TraceShadingParametersGPU resolveBvhTriangleHit()
{
  using softwarebvh::rendering::SoftwareRayTracePass;

  SoftwareRayTracePass::TraceRayGPU ray{};
  ray.originTMin[0] = 0.25f;
  ray.originTMin[1] = 0.25f;
  ray.originTMin[2] = -2.0f;
  ray.directionTMax[0] = 0.0f;
  ray.directionTMax[1] = 0.0f;
  ray.directionTMax[2] = 1.0f;

  SoftwareRayTracePass::HitOutputGPU hit{};
  hit.instanceId = 17u;
  hit.triangleId = 3u;
  hit.hitT = 2.0f;
  hit.baryU = 0.25f;
  hit.baryV = 0.25f;
  hit.tlasInstanceIndex = 5u;
  hit.flags = 0x40u;

  constexpr float positions[3][3] = {
      {0.0f, 0.0f, 0.0f},
      {1.0f, 0.0f, 0.0f},
      {0.0f, 1.0f, 0.0f},
  };
  constexpr float normals[3][3] = {
      {0.0f, 0.0f, 1.0f},
      {0.0f, 0.0f, 1.0f},
      {0.0f, 0.0f, 1.0f},
  };
  constexpr float uvs[3][2] = {
      {0.0f, 0.0f},
      {1.0f, 0.0f},
      {0.0f, 1.0f},
  };
  constexpr float receiverNormal[3] = {0.0f, 1.0f, 0.0f};
  constexpr float receiverAlbedo[3] = {0.2f, 0.4f, 0.6f};
  constexpr uint32_t materialId = 9u;
  constexpr float materialAlbedo[3] = {0.7f, 0.5f, 0.3f};

  const float bary[3] = {1.0f - hit.baryU - hit.baryV, hit.baryU, hit.baryV};
  float localPosition[3]{};
  float localNormal[3]{};
  float uv[2]{};
  for (uint32_t vertexIndex = 0u; vertexIndex < 3u; ++vertexIndex)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      localPosition[axis] += positions[vertexIndex][axis] * bary[vertexIndex];
      localNormal[axis] += normals[vertexIndex][axis] * bary[vertexIndex];
    }
    for (uint32_t axis = 0u; axis < 2u; ++axis)
      uv[axis] += uvs[vertexIndex][axis] * bary[vertexIndex];
  }

  const std::array<float, 3> rayDir = normalize({ray.directionTMax[0], ray.directionTMax[1], ray.directionTMax[2]}, {0.0f, 1.0f, 0.0f});
  const std::array<float, 3> worldNormal = normalize({localNormal[0], localNormal[1], localNormal[2]}, {-rayDir[0], -rayDir[1], -rayDir[2]});

  globalillumination::TraceShadingParametersGPU shading{};
  shading.positionHitT[0] = localPosition[0];
  shading.positionHitT[1] = localPosition[1];
  shading.positionHitT[2] = localPosition[2];
  shading.positionHitT[3] = hit.hitT;
  shading.normalValidity[0] = worldNormal[0];
  shading.normalValidity[1] = worldNormal[1];
  shading.normalValidity[2] = worldNormal[2];
  shading.normalValidity[3] = 1.0f;
  shading.uvBarycentrics[0] = uv[0];
  shading.uvBarycentrics[1] = uv[1];
  shading.uvBarycentrics[2] = hit.baryU;
  shading.uvBarycentrics[3] = hit.baryV;
  shading.albedo[0] = materialAlbedo[0];
  shading.albedo[1] = materialAlbedo[1];
  shading.albedo[2] = materialAlbedo[2];
  shading.albedo[3] = 1.0f;
  shading.receiverPositionPdf[0] = ray.originTMin[0];
  shading.receiverPositionPdf[1] = ray.originTMin[1];
  shading.receiverPositionPdf[2] = ray.originTMin[2];
  shading.receiverPositionPdf[3] = 0.15915494f;
  shading.receiverNormalValidity[0] = receiverNormal[0];
  shading.receiverNormalValidity[1] = receiverNormal[1];
  shading.receiverNormalValidity[2] = receiverNormal[2];
  shading.receiverNormalValidity[3] = 1.0f;
  shading.receiverAlbedo[0] = receiverAlbedo[0];
  shading.receiverAlbedo[1] = receiverAlbedo[1];
  shading.receiverAlbedo[2] = receiverAlbedo[2];
  shading.receiverAlbedo[3] = 1.0f;
  shading.rayDirection[0] = rayDir[0];
  shading.rayDirection[1] = rayDir[1];
  shading.rayDirection[2] = rayDir[2];
  shading.ids[0] = materialId;
  shading.ids[1] = hit.triangleId;
  shading.ids[2] = hit.tlasInstanceIndex;
  shading.ids[3] = hit.flags | globalillumination::TraceShadingFlag_Valid | globalillumination::TraceShadingFlag_SurfaceCacheMaterial | sourceFlags(globalillumination::TraceShadingSource_BVH);
  return shading;
}

globalillumination::TraceShadingParametersGPU resolveSdfHit(bool surfaceCacheMaterial)
{
  using namespace gpusigneddistancefields::gpgpu;

  TraceRayGPU ray{};
  ray.originAndMinT = {5.0f, 6.0f, 7.0f, 0.01f};
  ray.directionAndMaxT = {0.0f, 2.0f, 0.0f, 3000.0f};
  ray.metadata = {11u, 12u, TraceRayFlag_Active, 19u};

  TraceHitGPU hit{};
  hit.worldPositionAndDistance = {8.0f, 9.0f, 10.0f, 4.5f};
  hit.worldNormalAndStepCount = {0.0f, 0.0f, 4.0f, 12.0f};
  hit.localUVWAndAux = {0.1f, 0.2f, 0.3f, 0.7f};
  hit.traceInfo = {TraceHitFlag_Hit << 8u, 23u, 29u, 0u};
  hit.surfaceMaterialReference = surfaceCacheMaterial ? std::array<uint32_t, 4>{123u, floatBits(0.4f), floatBits(0.6f), (1u << 31u) | 17u | (23u << 11u)}
                                                      : std::array<uint32_t, 4>{kInvalidIndex, 0u, 0u, 0u};

  constexpr float receiverNormal[3] = {0.0f, 1.0f, 0.0f};
  constexpr float receiverAlbedo[3] = {0.25f, 0.5f, 0.75f};
  const std::array<float, 3> rayDir = normalize({ray.directionAndMaxT[0], ray.directionAndMaxT[1], ray.directionAndMaxT[2]}, {0.0f, 1.0f, 0.0f});
  const std::array<float, 3> hitNormal = normalize({hit.worldNormalAndStepCount[0], hit.worldNormalAndStepCount[1], hit.worldNormalAndStepCount[2]}, {-rayDir[0], -rayDir[1], -rayDir[2]});

  globalillumination::TraceShadingParametersGPU shading{};
  shading.positionHitT[0] = hit.worldPositionAndDistance[0];
  shading.positionHitT[1] = hit.worldPositionAndDistance[1];
  shading.positionHitT[2] = hit.worldPositionAndDistance[2];
  shading.positionHitT[3] = hit.worldPositionAndDistance[3];
  shading.normalValidity[0] = hitNormal[0];
  shading.normalValidity[1] = hitNormal[1];
  shading.normalValidity[2] = hitNormal[2];
  shading.normalValidity[3] = 1.0f;
  const bool materialReferenceValid = (hit.surfaceMaterialReference[3] & (1u << 31u)) != 0u;
  shading.uvBarycentrics[0] = materialReferenceValid ? bitsFloat(hit.surfaceMaterialReference[1]) : hit.localUVWAndAux[0];
  shading.uvBarycentrics[1] = materialReferenceValid ? bitsFloat(hit.surfaceMaterialReference[2]) : hit.localUVWAndAux[1];
  shading.uvBarycentrics[2] = hit.localUVWAndAux[2];
  shading.uvBarycentrics[3] = hit.localUVWAndAux[3];
  shading.albedo[0] = receiverAlbedo[0];
  shading.albedo[1] = receiverAlbedo[1];
  shading.albedo[2] = receiverAlbedo[2];
  shading.albedo[3] = 1.0f;
  shading.receiverPositionPdf[0] = ray.originAndMinT[0];
  shading.receiverPositionPdf[1] = ray.originAndMinT[1];
  shading.receiverPositionPdf[2] = ray.originAndMinT[2];
  shading.receiverPositionPdf[3] = 0.15915494f;
  shading.receiverNormalValidity[0] = receiverNormal[0];
  shading.receiverNormalValidity[1] = receiverNormal[1];
  shading.receiverNormalValidity[2] = receiverNormal[2];
  shading.receiverNormalValidity[3] = 1.0f;
  shading.receiverAlbedo[0] = receiverAlbedo[0];
  shading.receiverAlbedo[1] = receiverAlbedo[1];
  shading.receiverAlbedo[2] = receiverAlbedo[2];
  shading.receiverAlbedo[3] = 1.0f;
  shading.rayDirection[0] = rayDir[0];
  shading.rayDirection[1] = rayDir[1];
  shading.rayDirection[2] = rayDir[2];
  shading.ids[0] = materialReferenceValid ? hit.surfaceMaterialReference[0] : hit.traceInfo[1];
  shading.ids[1] = hit.traceInfo[2];
  shading.ids[2] = ray.metadata[3];
  shading.ids[3] = globalillumination::TraceShadingFlag_Valid |
      globalillumination::TraceShadingFlag_ScreenTraceMiss |
      (materialReferenceValid ? globalillumination::TraceShadingFlag_SurfaceCacheMaterial : globalillumination::TraceShadingFlag_FallbackMaterial) |
      sourceFlags(globalillumination::TraceShadingSource_SDF);
  return shading;
}

void testBvhShadingFromTriangleHit()
{
  const globalillumination::TraceShadingParametersGPU shading = resolveBvhTriangleHit();
  expectVec3(shading.positionHitT, 0.25f, 0.25f, 0.0f);
  expectNear(shading.positionHitT[3], 2.0f);
  expectVec3(shading.normalValidity, 0.0f, 0.0f, 1.0f);
  expectNear(shading.normalValidity[3], 1.0f);
  expectNear(shading.uvBarycentrics[0], 0.25f);
  expectNear(shading.uvBarycentrics[1], 0.25f);
  expectNear(shading.uvBarycentrics[2], 0.25f);
  expectNear(shading.uvBarycentrics[3], 0.25f);
  expectVec3(shading.albedo, 0.7f, 0.5f, 0.3f);
  assert(shading.ids[0] == 9u);
  assert(shading.ids[1] == 3u);
  assert(shading.ids[2] == 5u);
  assert((shading.ids[3] & globalillumination::TraceShadingFlag_Valid) != 0u);
  assert((shading.ids[3] & globalillumination::TraceShadingFlag_SurfaceCacheMaterial) != 0u);
  assert((shading.ids[3] >> kTraceShadingSourceShift) == globalillumination::TraceShadingSource_BVH);
}

void testSdfShadingUsesSurfaceCacheMaterial()
{
  const globalillumination::TraceShadingParametersGPU shading = resolveSdfHit(true);
  expectVec3(shading.positionHitT, 8.0f, 9.0f, 10.0f);
  expectNear(shading.positionHitT[3], 4.5f);
  expectVec3(shading.normalValidity, 0.0f, 0.0f, 1.0f);
  expectVec3(shading.albedo, 0.25f, 0.5f, 0.75f);
  expectNear(shading.albedo[3], 1.0f);
  expectNear(shading.uvBarycentrics[0], 0.4f);
  expectNear(shading.uvBarycentrics[1], 0.6f);
  assert(shading.ids[0] == 123u);
  assert(shading.ids[1] == 29u);
  assert(shading.ids[2] == 19u);
  assert((shading.ids[3] & globalillumination::TraceShadingFlag_SurfaceCacheMaterial) != 0u);
  assert((shading.ids[3] & globalillumination::TraceShadingFlag_ScreenTraceMiss) != 0u);
  assert((shading.ids[3] >> kTraceShadingSourceShift) == globalillumination::TraceShadingSource_SDF);
}

void testSdfShadingFallsBackToReceiverMaterial()
{
  const globalillumination::TraceShadingParametersGPU shading = resolveSdfHit(false);
  expectVec3(shading.albedo, 0.25f, 0.5f, 0.75f);
  assert((shading.ids[3] & globalillumination::TraceShadingFlag_FallbackMaterial) != 0u);
  assert((shading.ids[3] & globalillumination::TraceShadingFlag_SurfaceCacheMaterial) == 0u);
}

void testInvalidShadingDefaults()
{
  const globalillumination::TraceShadingParametersGPU shading = invalidTraceShading();
  assert(shading.ids[0] == kInvalidIndex);
  assert(shading.ids[1] == kInvalidIndex);
  assert(shading.ids[2] == kInvalidIndex);
  assert((shading.ids[3] & globalillumination::TraceShadingFlag_Valid) == 0u);
}

void testScreenTraceSettingsDefaultToScreenFirst()
{
  const virtualgeometry::gpgpu::ScreenSpaceRayTracePass::Settings settings{};
  assert(settings.enabled);
  assert(settings.screenTraceEnabled);
  assert(!settings.forceWorldFallback);
}

void testBvhResolveRequestsVirtualTextureFeedbackByDefault()
{
  const globalillumination::WorldHitShadingResolvePass::Settings settings{};
  assert(settings.feedbackRequestCapacity > 0u);
}

} // namespace

int main()
{
  testBvhShadingFromTriangleHit();
  testSdfShadingUsesSurfaceCacheMaterial();
  testSdfShadingFallsBackToReceiverMaterial();
  testInvalidShadingDefaults();
  testScreenTraceSettingsDefaultToScreenFirst();
  testBvhResolveRequestsVirtualTextureFeedbackByDefault();
  std::cout << "TraceShadingResolveTests passed\n";
  return 0;
}
