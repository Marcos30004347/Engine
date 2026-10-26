#pragma once

#include "virtualgeometry/VirtualGeometryData.hpp"

#include <cstdint>

namespace virtualgeometry
{

// The seam between a route-specific front half and the shared back half of
// the virtual-geometry builder (phase 9a of
// docs/terrain-vg-unified-hierarchy-plan.md).
//
//   FRONT (route-specific)                 BACK (shared, here)
//     triangle mesh: partition ->            packPages: page packing + rewrites
//     simplify -> group -> DAG               encode:    quantise + encode pages
//     voxel octree: the octree IS            (then a file or the scratch file:
//     the DAG (levels are LODs)               compress + write)
//                           \__ VirtualGeometryBuildData __/
//
// Both front halves produce a `VirtualGeometryBuildData` holding clusters
// (vertices, indices, groupId, self/parent bounds, cone), the LOD hierarchy,
// the group infos and the cluster DAG; the back half reads exactly those and
// the asset tables (shapes, meshParts, materialFiles, skeleton) it copies
// through. `groupDAG` and `buildToLinearizedClusterIndex` are front-half
// bookkeeping the back half never reads. The voxel route populates none of
// the mesh-only tables beyond one shape and one mesh part, and the golden
// fingerprint gate (`VirtualGeometryPackerDeterminism`) builds it that way.
//
// Determinism is a hard requirement of every stage here: the encoded asset
// must not depend on thread scheduling, or the byte-identical gate that guards
// this code means nothing. The fan-outs below write an indexed result array -
// page i writes slot i - and nothing accumulates in completion order.
class VirtualGeometryPacker
{
public:
  // Stage 1, in place: assigns groups to pages by walking the DAG cut from the
  // roots, emits per-page dependencies and install/uninstall rewrites, and
  // reorders the clusters to page layout. Serial by design: a group's page
  // depends on what was placed before it, and a different layout changes
  // every dependency and every streaming-leaf rewrite (see
  // `vg-streaming-leaf-parent-test`).
  static void packPages(
      VirtualGeometryBuildData &dag,
      const VirtualGeometryBuildSettings &settings,
      VirtualGeometryBuildTimings *timings = nullptr);

  // Stage 2: quantises and encodes every page, then re-derives the hierarchy
  // bounds from the quantised meshlets. `threads` 0 chooses from the page
  // count and the machine (see `resolveThreads`); 1 runs inline. The bytes
  // are identical either way. A caller that is already one of many workers
  // (the terrain page cache meshes chunks in parallel) passes 1.
  static VirtualGeometryEncodedData encode(
      const VirtualGeometryBuildData &dag,
      const QuantizationConfig &quantization,
      VirtualGeometryBuildTimings *timings = nullptr,
      uint32_t threads = 0u);

  // Both stages.
  static VirtualGeometryEncodedData pack(
      VirtualGeometryBuildData &dag,
      const VirtualGeometryBuildSettings &settings,
      const QuantizationConfig &quantization,
      VirtualGeometryBuildTimings *timings = nullptr,
      uint32_t threads = 0u);

  // Threads for a fan-out over `items` independent jobs. `requested` 0 is
  // automatic: inline under `kParallelThreshold` items, else up to the
  // hardware concurrency capped at 8; `VG_PACK_THREADS` in the environment
  // overrides the automatic choice (1 forces serial, for an A/B).
  static uint32_t resolveThreads(uint32_t requested, size_t items);
  static constexpr size_t kParallelThreshold = 8u;

  // Runs `job(index)` for every index in [0, count) on `threads` threads,
  // claiming indices from a shared counter so uneven jobs balance. The caller
  // owns the result array; index i must write only slot i.
  template <typename Job>
  static void forEachIndex(size_t count, uint32_t threads, const Job &job);
};

} // namespace virtualgeometry

#include <atomic>
#include <thread>
#include <vector>

namespace virtualgeometry
{

template <typename Job>
void VirtualGeometryPacker::forEachIndex(const size_t count, const uint32_t threads, const Job &job)
{
  if (count == 0u)
    return;
  if (threads <= 1u || count == 1u)
  {
    for (size_t index = 0u; index < count; ++index)
      job(index);
    return;
  }
  std::atomic<size_t> next{0u};
  const auto worker = [&]()
  {
    for (size_t index = next.fetch_add(1u); index < count; index = next.fetch_add(1u))
      job(index);
  };
  std::vector<std::thread> pool;
  pool.reserve(threads - 1u);
  for (uint32_t index = 1u; index < threads; ++index)
    pool.emplace_back(worker);
  worker();
  for (auto &thread : pool)
    thread.join();
}

} // namespace virtualgeometry
