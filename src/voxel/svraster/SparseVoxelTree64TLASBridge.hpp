#pragma once

#include "softwarebvh/SoftwareTLASManager.hpp"
#include "voxel/svraster/SparseVoxelTree64Dynamic.hpp"

#include <string>
#include <vector>

namespace voxel::svraster
{

// Present committed Tree64 dynamic instances to the software BVH4 TLAS.
//
// The Tree64 broad phase today is the clipmap directory: a uniform grid that
// bins instance references per cell. That costs one reference per cell an
// instance touches, and a single cell size (64-128 m) has to serve terrain
// pages spanning 12.8 m to 13 km — measured at 677970 references for 930
// instances, 729 apiece. A BVH has no replication term, and the gap only widens
// once instances stop being a regular grid of similar-sized pages.
//
// Two conventions differ and are converted here rather than at the call site,
// because getting either backwards produces a TLAS that traverses cleanly and
// intersects nothing:
//
//   - Tree64 stores `worldToModel`; the TLAS wants object-to-world, so the
//     transform is inverted. Terrain placement is a pure translation, which is
//     inverted exactly; a general instance falls back to a full inverse.
//   - Tree64 identifies its geometry by `modelId`. That becomes the TLAS
//     `blasHandle`, so a BLAS handle addresses a Tree64 model rather than a
//     triangle mesh.
//
// Instances carrying a terrain generation are marked dynamic: they are
// republished as pages stream, which is what the manager's refit and partial
// rebuild paths key on.
std::vector<softwarebvh::SoftwareTLASManager::InstanceBuildInput>
sparseVoxelTree64TLASInstances(
    const std::vector<SparseVoxelTree64DynamicInstanceSource> &instances);

// The indirection the BVH4 traversal needs to name a Tree64 instance.
//
// A BVH4 leaf carries a *TLAS* instance index: a position among the instances
// the builder accepted, assigned by its own compaction. A Tree64 dynamic
// instance lives at a *stable slot* that is sparse by construction — the slot a
// terrain page owns does not move when a neighbouring page streams out, which
// is what lets a page be republished without rewriting the whole array. The two
// numberings agree only when nothing is missing, so relying on them agreeing is
// a bug that hides until the first eviction and then traverses a real, wrong
// page.
//
// `instanceIndexToInputIndex` comes from SoftwareTLASManager and is indexed by
// TLAS instance index; `instances` is the same vector that was handed to
// sparseVoxelTree64TLASInstances. `stableTerrainSlots` must match the dynamic
// layer's setting of the same name, because it is what decides where the layer
// put each instance: stable slots place an instance at its stableInstanceId,
// and the ordinary path appends in source order. The result is indexed by TLAS
// instance index and holds the Tree64 instance index, ready to upload as the
// shader's `tlasInstanceSlots`. An entry the map cannot resolve is left at
// 0xffffffff, which the traversal rejects.
std::vector<uint32_t> sparseVoxelTree64TLASInstanceSlots(
    const std::vector<SparseVoxelTree64DynamicInstanceSource> &instances,
    const std::vector<uint32_t> &instanceIndexToInputIndex,
    bool stableTerrainSlots);

// Validates the indirection written beside a software BVH4 TLAS. Generated
// terrain is intentionally sparse and stable by terrain slot, so a stale or
// compacted slot index can make TLAS culling reach a different Tree64 page
// than the atomically published raster/Tree64 descriptor.
bool sparseVoxelTree64ValidateTLASInstanceSlots(
    const std::vector<SparseVoxelTree64DynamicInstanceSource> &instances,
    const std::vector<uint32_t> &instanceIndexToInputIndex,
    const std::vector<uint32_t> &slots,
    bool stableTerrainSlots,
    std::string *error = nullptr);

// True when `worldToModel` is identity rotation with no scale or shear, so the
// inverse is the negated translation. Shared with the traversal's
// kSparseVoxelTree64DynamicTranslationOnly fast path so both agree on what
// "translation only" means.
bool sparseVoxelTree64TransformIsTranslationOnly(
    const std::array<float, 16> &worldToModel);

} // namespace voxel::svraster
