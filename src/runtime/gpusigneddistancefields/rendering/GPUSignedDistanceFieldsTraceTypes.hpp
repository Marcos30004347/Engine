#pragma once

#include <array>
#include <cstdint>

namespace gpusigneddistancefields
{
namespace gpgpu
{

struct alignas(16) TraceRayGPU
{
  std::array<float, 4> originAndMinT{};
  std::array<float, 4> directionAndMaxT{};
  std::array<uint32_t, 4> metadata{};
};

struct alignas(16) TraceHitGPU
{
  std::array<float, 4> worldPositionAndDistance{};
  std::array<float, 4> worldNormalAndStepCount{};
  std::array<float, 4> localUVWAndAux{};
  std::array<uint32_t, 4> metadata{};
  std::array<uint32_t, 4> traceInfo{};
  std::array<uint32_t, 4> surfaceMaterialReference{};
};

static_assert(sizeof(TraceRayGPU) == 48u, "TraceRayGPU must match WGSL layout");
static_assert(sizeof(TraceHitGPU) == 96u, "TraceHitGPU must match WGSL layout");

enum TraceRayFlags : uint32_t
{
  TraceRayFlag_None = 0u,
  TraceRayFlag_Active = 1u << 0u,
};

enum TraceHitFlags : uint32_t
{
  TraceHitFlag_None = 0u,
  TraceHitFlag_Hit = 1u << 0u,
  TraceHitFlag_HasCascade = 1u << 1u,
  TraceHitFlag_HasBrick = 1u << 2u,
  TraceHitFlag_HasUninitialized = 1u << 3u,
  TraceHitFlag_HasValidBrickId = 1u << 4u,
  TraceHitFlag_HasRootAabb = 1u << 5u,
  TraceHitFlag_HasTreeRoot = 1u << 6u,
  TraceHitFlag_HasMidAabb = 1u << 7u,
  TraceHitFlag_HasBottomAabb = 1u << 8u,
  TraceHitFlag_InvalidDirection = 1u << 9u,
  TraceHitFlag_InactiveRay = 1u << 10u,
  TraceHitFlag_ScreenSpace = 1u << 11u,
  TraceHitFlag_SdfFallback = 1u << 12u,
  TraceHitFlag_Sky = 1u << 13u,
};

enum TraceStatsCounter : uint32_t
{
  TraceStatsCounter_TotalRays = 0u,
  TraceStatsCounter_ActiveRays = 1u,
  TraceStatsCounter_InactiveRays = 2u,
  TraceStatsCounter_InvalidDirectionRays = 3u,
  TraceStatsCounter_HitRays = 4u,
  TraceStatsCounter_MissRays = 5u,
  TraceStatsCounter_HasCascadeRays = 6u,
  TraceStatsCounter_HasValidBrickRays = 7u,
  TraceStatsCounter_HasUninitializedRays = 8u,
  TraceStatsCounter_HasRootAabbRays = 9u,
  TraceStatsCounter_HasTreeRootRays = 10u,
  TraceStatsCounter_HasMidAabbRays = 11u,
  TraceStatsCounter_HasBottomAabbRays = 12u,
  TraceStatsCounter_FailedCascadeRays = 13u,
  TraceStatsCounter_FailedRootRays = 14u,
  TraceStatsCounter_FailedMidRays = 15u,
  TraceStatsCounter_FailedBottomRays = 16u,
  TraceStatsCounter_FailedBrickRays = 17u,
  TraceStatsCounter_FailedUninitializedRays = 18u,
  TraceStatsCounter_Count = 19u,
};

static constexpr uint64_t getTraceStatsBufferSize()
{
  return static_cast<uint64_t>(TraceStatsCounter_Count) * sizeof(uint32_t);
}

} // namespace gpgpu
} // namespace gpusigneddistancefields
