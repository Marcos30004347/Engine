#include "VirtualGeometryPacker.hpp"

#include "VirtualGeometryCompressor.hpp"
#include "VirtualGeometryHierarchyBuilder.hpp"
#include "VirtualGeometryHierarchyRewrites.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace virtualgeometry
{

namespace
{

using Clock = std::chrono::steady_clock;

double millisecondsSince(const Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

} // namespace

uint32_t VirtualGeometryPacker::resolveThreads(const uint32_t requested, const size_t items)
{
  if (items <= 1u)
    return 1u;
  uint32_t threads = requested;
  if (threads == 0u)
  {
    static const int environment = []()
    {
      const char *value = std::getenv("VG_PACK_THREADS");
      return value != nullptr ? std::atoi(value) : 0;
    }();
    if (environment > 0)
      threads = static_cast<uint32_t>(environment);
    else if (items < kParallelThreshold)
      threads = 1u;
    else
      threads = std::min<uint32_t>(std::max(1u, std::thread::hardware_concurrency()), 8u);
  }
  return static_cast<uint32_t>(std::min<size_t>(threads, items));
}

void VirtualGeometryPacker::packPages(
    VirtualGeometryBuildData &dag,
    const VirtualGeometryBuildSettings &settings,
    VirtualGeometryBuildTimings *timings)
{
  const auto start = Clock::now();
  dag.buildSettings = settings;
  VirtualGeometryHierarchyRewrites::buildPagesAndRewrites(dag, settings);
  validateClusterDAGParentSelfConsistency(dag.clusters, dag.clusterDAG, "post_rewrite");
  const double elapsed = millisecondsSince(start);
  dag.timings.packPages += elapsed;
  if (timings != nullptr && timings != &dag.timings)
    timings->packPages += elapsed;
}

VirtualGeometryEncodedData VirtualGeometryPacker::encode(
    const VirtualGeometryBuildData &dag,
    const QuantizationConfig &quantization,
    VirtualGeometryBuildTimings *timings,
    const uint32_t threads)
{
  const auto start = Clock::now();
  VirtualGeometryEncodedData result;
  result.quantizationConfig = quantization;
  result.buildSettings = dag.buildSettings;
  result.hierarchy = dag.lodLevelHierarchy;
  result.shapes = dag.shapes;
  result.materialFiles = dag.materialFiles;
  result.meshParts = dag.meshParts;
  result.skeleton = dag.skeleton;
  result.root_node_index = 0;
  result.rootPageIndex = result.shapes.empty() ? 0u : result.shapes.front().root_page_index;
  result.timings = dag.timings;

  result.pages.resize(dag.pages.size());
  const uint32_t workers = resolveThreads(threads, dag.pages.size());
  // Page i writes result.pages[i] and nothing else, so the fan-out is
  // deterministic by construction whatever order the pages finish in.
  forEachIndex(
      dag.pages.size(), workers,
      [&](const size_t pageIndex)
      {
        VirtualGeometryCompressor::encodePage(
            dag, static_cast<uint32_t>(pageIndex), quantization, result.pages[pageIndex]);
      });
  VirtualGeometryCompressor::rebuildHierarchyBounds(result, quantization);

  result.timings.encode += millisecondsSince(start);
  result.timings.encodeThreads = workers;
  if (timings != nullptr)
  {
    timings->encode = result.timings.encode;
    timings->encodeThreads = workers;
  }
  return result;
}

VirtualGeometryEncodedData VirtualGeometryPacker::pack(
    VirtualGeometryBuildData &dag,
    const VirtualGeometryBuildSettings &settings,
    const QuantizationConfig &quantization,
    VirtualGeometryBuildTimings *timings,
    const uint32_t threads)
{
  packPages(dag, settings, timings);
  return encode(dag, quantization, timings, threads);
}

} // namespace virtualgeometry
