#pragma once

#include "voxel/svraster/SparseVoxelBrick.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace voxel::svraster
{

// Exposed-face surface meshing of one brick node's 8^3 occupancy.
//
// This is the geometry half of the voxel virtual-geometry path. It deliberately
// does not touch hash maps: a brick is 512 cells, which is eight 64-bit words,
// so every neighbour test for a whole face direction is a shift and a mask. The
// hash-map meshing the first sidecar prototype used spent almost all of its time
// probing `unordered_map` six times per occupied cell; at page scale that is the
// difference between microseconds and tens of milliseconds per page.
//
// Face directions are indexed so that `direction >> 1` is the axis and
// `direction & 1` is the positive side:
//   0 = -X, 1 = +X, 2 = -Y, 3 = +Y, 4 = -Z, 5 = +Z
inline constexpr uint32_t kSparseVoxelBrickFaceDirections = 6u;
inline constexpr uint32_t kSparseVoxelBrickPlanes = kSparseVoxelBrickSide;
inline constexpr uint8_t kSparseVoxelBrickEmptyCellMaterial = 0xffu;

// One z-slab of a brick per entry: bit (x + 8 * y) of `planes[z]`.
using SparseVoxelBrickOccupancyPlanes =
    std::array<uint64_t, kSparseVoxelBrickPlanes>;

using SparseVoxelBrickFaceMasks =
    std::array<SparseVoxelBrickOccupancyPlanes, kSparseVoxelBrickFaceDirections>;

// Same-level neighbours of one brick, in face-direction order. A null entry is
// treated as empty space, which is what makes a page border emit its faces
// instead of silently opening a hole into the next page.
struct SparseVoxelBrickNeighbourOccupancy
{
  std::array<const SparseVoxelBrickOccupancyGPU *, kSparseVoxelBrickFaceDirections>
      neighbours{};
};

// One greedy-merged surface quad in brick-local cell coordinates.
//
// `origin` is the minimum cell corner of the quad's footprint and `width` /
// `height` count cells along the direction's u/v axes (see
// sparseVoxelBrickFaceAxes). A quad is always axis aligned and always sits on a
// cell boundary, so the emitted geometry reproduces the voxel surface exactly.
struct SparseVoxelBrickSurfaceQuad
{
  std::array<uint8_t, 3> origin{};
  uint8_t direction = 0u;
  uint8_t width = 1u;
  uint8_t height = 1u;
  // Palette index of the voxel behind the face, or
  // kSparseVoxelBrickEmptyCellMaterial when the caller supplied no materials.
  uint8_t material = kSparseVoxelBrickEmptyCellMaterial;
  uint8_t reserved = 0u;
};

// u/v axes for a face direction, chosen so that `cross(u, v)` is the positive
// axis. Winding for a negative face is reversed by the geometry emitter.
inline std::array<uint32_t, 2> sparseVoxelBrickFaceAxes(const uint32_t direction)
{
  switch (direction >> 1u)
  {
  case 0u:
    return {1u, 2u};
  case 1u:
    return {2u, 0u};
  default:
    return {0u, 1u};
  }
}

// Unpack a brick's occupancy words into one 64-bit mask per z slab.
SparseVoxelBrickOccupancyPlanes sparseVoxelBrickOccupancyPlanes(
    const SparseVoxelBrickOccupancyGPU &occupancy);

// Exposed-face masks for all six directions. A face is exposed when its own
// cell is occupied and the cell across the face is not, where cells outside the
// brick come from `neighbours`. Returns the total number of exposed faces.
uint32_t sparseVoxelBrickExposedFaces(
    const SparseVoxelBrickOccupancyGPU &occupancy,
    const SparseVoxelBrickNeighbourOccupancy &neighbours,
    SparseVoxelBrickFaceMasks &exposed);

// Close each independently selectable octree block, including interfaces with
// occupied neighbouring blocks. A neighbour may be drawn at a different LOD.
// blockCells is an octree block side: 1, 2, 4 or 8 cells.
uint32_t sparseVoxelBrickClosedBlockFaces(
    const SparseVoxelBrickOccupancyGPU &occupancy, uint32_t blockCells,
    SparseVoxelBrickFaceMasks &exposed);


// Dense per-cell palette index for one brick, 0xff for empty cells.
//
// `materialIndices` is the hierarchy-wide array and `materialBase` the node's
// run inside it. Passing an empty array fills every occupied cell with zero,
// which is what a geometry-only build wants.
bool sparseVoxelBrickCellMaterials(
    const SparseVoxelBrickOccupancyGPU &occupancy,
    const std::vector<uint8_t> &materialIndices,
    uint64_t materialBase,
    std::array<uint8_t, kSparseVoxelBrickVoxelCount> &cellMaterials);

// Greedy-merge exposed faces into quads, appended to `quads`.
//
// `blockCells` bounds merging to an octree-aligned block: a run never crosses a
// multiple of `blockCells` on either axis, so every emitted quad belongs to
// exactly one block of that size. That is what lets a coarse parent block and
// the finer child blocks under it describe exactly the same volume, which is
// the whole basis of the voxel LOD DAG. Pass kSparseVoxelBrickSide for
// whole-brick merging and 1 to disable merging entirely.
//
// Returns the number of quads appended.
uint32_t sparseVoxelBrickGreedyQuads(
    const SparseVoxelBrickFaceMasks &exposed,
    const std::array<uint8_t, kSparseVoxelBrickVoxelCount> &cellMaterials,
    uint32_t blockCells,
    bool mergeSameMaterial,
    std::vector<SparseVoxelBrickSurfaceQuad> &quads);

// Corner cell coordinates of a quad, in emission order. The winding is
// counter-clockwise seen from outside the surface.
std::array<std::array<uint32_t, 3>, 4> sparseVoxelBrickQuadCorners(
    const SparseVoxelBrickSurfaceQuad &quad);

// Outward unit normal of a quad.
inline std::array<float, 3> sparseVoxelBrickQuadNormal(
    const SparseVoxelBrickSurfaceQuad &quad)
{
  std::array<float, 3> normal{};
  normal[quad.direction >> 1u] = (quad.direction & 1u) != 0u ? 1.0f : -1.0f;
  return normal;
}

} // namespace voxel::svraster
