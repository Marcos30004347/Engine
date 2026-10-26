#pragma once

#include <cassert>
#include <cstdint>
#include <vector>

#include "os/Logger.hpp"
#include "rendering/gpu/RenderGraph.hpp"

namespace compute_sort_benchmark
{
constexpr uint32_t kMinElementCount = 1024u;
constexpr uint32_t kMaxElementCount = 20u * 1000u * 1000u;
constexpr const char *kSortPassName = "radixSortPass";
constexpr uint32_t kBenchmarkTotalRuns = 6u;
constexpr uint32_t kBenchmarkWarmupRuns = 1u;

inline std::vector<uint32_t> buildDescendingKeys(uint32_t count)
{
  std::vector<uint32_t> values(count);
  for (uint32_t i = 0; i < count; ++i)
  {
    values[i] = count - i;
  }

  return values;
}

inline std::vector<uint32_t> buildAscendingValues(uint32_t count)
{
  std::vector<uint32_t> values(count);
  for (uint32_t i = 0; i < count; ++i)
  {
    values[i] = i;
  }

  return values;
}

inline std::vector<uint32_t> buildElementCounts()
{
  std::vector<uint32_t> counts;
  for (uint32_t count = kMinElementCount; count <= kMaxElementCount; count *= 2u)
  {
    counts.push_back(count);
  }

  if (counts.empty() || counts.back() != kMaxElementCount)
  {
    counts.push_back(kMaxElementCount);
  }

  return counts;
}

inline void assertStrictlyIncreasing(const uint32_t *values, uint32_t count)
{
  for (uint32_t i = 0; i + 1u < count; ++i)
  {
    assert(values[i + 1u] > values[i]);
  }
}

inline void logBenchmarkResult(const char *label, uint32_t count, double gpuTimeMs)
{
  os::Logger::logf("%s sort count=%u gpu=%.6fms (%.3fus)", label, count, gpuTimeMs, gpuTimeMs * 1000.0);
}

inline void logBenchmarkAverage(const char *label, uint32_t count, double warmupGpuTimeMs, double averageGpuTimeMs, uint32_t measuredRuns)
{
  os::Logger::logf(
      "%s sort count=%u warmup=%.6fms avg(%u)=%.6fms (%.3fus)",
      label,
      count,
      warmupGpuTimeMs,
      measuredRuns,
      averageGpuTimeMs,
      averageGpuTimeMs * 1000.0);
}
} // namespace compute_sort_benchmark
