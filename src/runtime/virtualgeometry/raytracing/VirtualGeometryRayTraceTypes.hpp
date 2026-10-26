#pragma once

#include <cstdint>
#include <cstddef>

namespace virtualgeometry::raytracing
{
// Immutable geometry identity, independent of decoded-buffer offsets or instances.
struct GeometryKey
{
  uint64_t object = 0;
  uint64_t revision = 0;
  uint32_t cluster = 0;
  uint32_t format = 0;
  bool operator==(const GeometryKey &o) const
  {
    return object == o.object && revision == o.revision && cluster == o.cluster && format == o.format;
  }
};

// Native-independent GPU input. Four column-major packed float3 columns.
// Handles are monotonically assigned; an evicted handle is never reused.
struct RayTraceInstance
{
  float transform[12] = {1,0,0, 0,1,0, 0,0,1, 0,0,0};
  uint32_t geometryHandle = 0;
  uint32_t mask = 0xff;
};
static_assert(sizeof(RayTraceInstance) == 56);

struct ShadowRay
{
  float origin[3] = {};
  float minDistance = 0.001f;
  float direction[3] = {0, 0, -1};
  float maxDistance = 1000;
  uint32_t mask = 0xff;
  uint32_t padding[3] = {};
};
static_assert(sizeof(ShadowRay) == 48);

// Conservative world-space caster bounds / receiver-to-light volumes.
struct RayTraceBounds
{
  float minimum[3] = {}; float padding0 = 0;
  float maximum[3] = {}; float padding1 = 0;
};
static_assert(sizeof(RayTraceBounds) == 32);

// Closest-hit output stores the ray parameter t (world distance for unit rays).
// Miss and invalid are explicit states; neither may be accumulated as a hit.
struct ClosestHitResult
{
  float distance = 0;
  uint32_t state = 2; // ShadowResult
  uint32_t instance = UINT32_MAX;
  uint32_t primitive = UINT32_MAX;
};
static_assert(sizeof(ClosestHitResult) == 16);

enum class ShadowResult : uint32_t { Visible = 0, Occluded = 1, InvalidScene = 2 };

struct RayTraceFrameStatistics
{
  uint32_t instances = 0;
  uint32_t invalidScene = 0;
  uint32_t rays = 0;
  uint32_t reserved = 0;
  uint32_t instanceDispatch[3] = {};
  uint32_t rayDispatch[3] = {};
};
static_assert(sizeof(RayTraceFrameStatistics) == 40);
}
