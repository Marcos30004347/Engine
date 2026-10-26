#pragma once

#include <cstdint>

namespace globalillumination
{

enum TraceShadingSource : uint32_t
{
  TraceShadingSource_None = 0u,
  TraceShadingSource_ScreenSpace = 1u,
  TraceShadingSource_SDF = 2u,
  TraceShadingSource_BVH = 3u,
};

enum TraceShadingFlags : uint32_t
{
  TraceShadingFlag_None = 0u,
  TraceShadingFlag_Valid = 1u << 0u,
  TraceShadingFlag_SurfaceCacheMaterial = 1u << 1u,
  TraceShadingFlag_FallbackMaterial = 1u << 2u,
  TraceShadingFlag_ScreenTraceMiss = 1u << 3u,
};

struct alignas(16) TraceShadingParametersGPU
{
  float positionHitT[4]{0.0f, 0.0f, 0.0f, 0.0f};
  float normalValidity[4]{0.0f, 1.0f, 0.0f, 0.0f};
  float uvBarycentrics[4]{0.0f, 0.0f, 0.0f, 0.0f};
  float albedo[4]{1.0f, 1.0f, 1.0f, 0.0f};
  float receiverPositionPdf[4]{0.0f, 0.0f, 0.0f, 0.15915494f};
  float receiverNormalValidity[4]{0.0f, 1.0f, 0.0f, 0.0f};
  float receiverAlbedo[4]{1.0f, 1.0f, 1.0f, 0.0f};
  float rayDirection[4]{0.0f, 1.0f, 0.0f, 0.0f};
  uint32_t ids[4]{UINT32_MAX, UINT32_MAX, UINT32_MAX, 0u};
};

static_assert(sizeof(TraceShadingParametersGPU) == 144u, "TraceShadingParametersGPU must match WGSL layout");

inline uint64_t traceShadingParametersBufferSize(const uint32_t rayCount)
{
  return static_cast<uint64_t>(rayCount == 0u ? 1u : rayCount) * sizeof(TraceShadingParametersGPU);
}

using WorldHitShadingGPU = TraceShadingParametersGPU;

inline uint64_t worldHitShadingBufferSize(const uint32_t rayCount)
{
  return traceShadingParametersBufferSize(rayCount);
}

} // namespace globalillumination
