#pragma once

#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelTree64Dynamic.hpp"
#include "voxel/svraster/SparseVoxelTree64Material.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"
#include "voxel/svraster/PlanetVoxelChunk.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace voxel::svraster
{

struct SparseVoxelCutDiagnostics;

// Voxel-exact Tree64 for one terrain page.
//
// The lighting Tree64 and the rasterized bricks are two representations of the
// same terrain, and shadow rays are only free of representation artifacts when
// they agree cell for voxel. The earlier page builder always sampled a page
// into a fixed 64-cell lattice, which matched only when a page happened to
// carry 64 voxels per edge. Automatic page selection targets a 20 cm voxel and
// produces 160-voxel pages, so every shadow cell covered 2.5 raster voxels and
// did not even align with a voxel boundary.
//
// This builder derives the lattice from the page instead: one Tree64 cell per
// page voxel, sourced from the same 512 occupancy bits the brick DDA consumes.
// Cell scale stays a power of four so the tree can be OR-reduced onto a coarser
// aligned lattice whenever the exact tree would exceed its node budget. Every
// occupied source voxel remains contained by an occupied cell at any scale, so
// the result is conservative for visibility at every reduction.
struct SparseVoxelTerrainPageTree64Source
{
  const SparseVoxelBrickHierarchy *bricks = nullptr;
  uint32_t firstBrickRoot = 0u;
  uint32_t brickRootCount = 0u;
  // Range of `bricks->nodes` owned by this page. Nodes outside it are treated
  // as absent, and a node whose children fall outside contributes its own
  // OR-reduced occupancy instead.
  uint32_t brickNodeOffset = 0u;
  uint32_t brickNodeCapacity = 0u;
  // Minimum corner of the page, in the frame the brick nodes are expressed in.
  std::array<double, 3> pageMinimumMetres{};
  // Horizontal page edge and the vertical span to cover above pageMinimum[1].
  double edgeMetres = 0.0;
  double heightMetres = 0.0;
  // The page's finest voxel. One cell of the unreduced tree.
  double voxelSizeMetres = 0.0;
  // Cells per model edge the tree may not exceed. 65536 is the representable
  // maximum; the practical cap is the node budget below.
  uint32_t maximumSide = 1024u;
  // Reduce until the tree fits. Zero leaves the exact tree whatever its size.
  uint32_t maximumNodeCount = 0u;
  // Optional coarser request, in cells per page edge. Zero keeps one cell per
  // page voxel. Anything finer than a voxel is ignored: the source has no
  // detail below one voxel.
  uint32_t requestedCellsPerEdge = 0u;
  // Exclude the XZ prism a finer terrain cascade owns, matching the ownership
  // rule the brick G-buffer applies to the same coarse pages.
  bool clipFinerCascade = true;
  // Build geometry that is contained *by* the terrain instead of geometry that
  // contains it.
  //
  // The raster shell rounds outward on purpose - a column's top cell is
  // `floor(max of four corner heights / voxel)` and the whole cell is filled -
  // because an outward shell is what keeps cracks out of the G-buffer at LOD
  // seams. A shadow caster wants the opposite: at a coarse spatial LOD the
  // voxel is tens of metres, so an outward surface stands that far above the
  // real ground over a whole page and shadows terrain that nothing occludes.
  //
  // Under this flag each column's top is lowered until the cell is below the
  // surface everywhere in its own footprint, bounded using the neighbouring
  // columns' tops. Missing a thin real occluder at range is acceptable;
  // inventing one is not. Runs keep their thickness so the node cost does not
  // change, and columns with nowhere left to go are dropped.
  bool conservativeInside = false;
};

struct SparseVoxelTerrainPageTree64
{
  SparseVoxelTree64Chunk tree{};
  uint32_t side = 0u;
  // Page voxels per cell edge. One is voxel-exact; four and sixteen are the
  // conservative reductions the node budget can force.
  uint32_t cellScale = 1u;
  double cellSizeMetres = 0.0;
  // World minimum corner of cell (0, 0, 0) and the cube the model spans.
  std::array<double, 3> originMetres{};
  double extentMetres = 0.0;
  // Placement of the dynamic model and its instance. Model space is the cell
  // cube itself, so the derived cell size is the page voxel exactly and the
  // model minimum is the origin rather than a large world coordinate that a
  // float would round. Keeping this beside the tree is what stops a consumer
  // from re-deriving a placement the tree was not built for.
  std::array<float, 3> localMinimum{};
  std::array<float, 3> localMaximum{};
  std::array<float, 16> worldToModel{};
  uint32_t leafBrickCount = 0u;
  uint32_t runCount = 0u;
  // Occupied runs the cell cube could not hold. Terrain is expected to fit its
  // own page, so anything here is geometry the raster draws and the lighting
  // representation does not - a light leak rather than an artifact, and the
  // one failure of this builder that is invisible in a single frame.
  uint32_t clippedRunCount = 0u;
  uint32_t occupiedVoxelCount = 0u;
  // Phase-zero instrumentation. levelCount includes the terminal 4-cubed
  // level and every four-to-one parent level through the root.
  uint32_t levelCount = 0u;
  uint64_t payloadBytes = 0u;
  double buildMilliseconds = 0.0;
};

bool buildSparseVoxelTerrainPageTree64(
    const SparseVoxelTerrainPageTree64Source &source,
    SparseVoxelTerrainPageTree64 &result,
    std::string *error = nullptr);

// CPU Tree64 reference adapter for the canonical terrain-column ABI. It builds
// exact one-column runs without walking or decoding a raster hierarchy.
bool buildSparseVoxelTerrainPageTree64FromColumns(
    const WorldMapTerrainColumnPage &columns,
    SparseVoxelTerrainPageTree64 &result,
    std::string *error = nullptr);

// Smallest power of four that is at least `cells`, clamped to the Tree64 model
// range. Zero when `cells` cannot be represented.
uint32_t sparseVoxelTerrainTree64SideForCells(uint32_t cells);

// Immutable output of one terrain-chunk Tree64 build. This is the unit passed
// from asynchronous CPU generation to the page uploader; it contains no
// renderer pointers or GPU addresses. A physical page handle is assigned only
// after the worker result has passed its terrain-generation check.
struct SparseVoxelTerrainTree64PagePayload
{
  uint32_t terrainSlot = 0u;
  uint32_t terrainGeneration = 0u;
  uint32_t internalLOD = 0u;
  uint32_t regionMappingId = 0u;
  uint64_t geometryRevision = 0u;
  SparseVoxelTree64DynamicModelSource model{};
  SparseVoxelTree64DynamicInstanceSource instance{};
  std::vector<SparseVoxelTree64MaterialCellGPU> materialCells;
  uint32_t sourceRunCount = 0u;
  uint32_t clippedRunCount = 0u;
  uint32_t cellScale = 1u;
  bool reusedNodes = false;
  bool reusedMaterialCells = false;
};

// Relocatable Tree64 sidecar for one generated terrain page.
//
// This is the page-source/cache form. It owns immutable model data and material
// sidecar cells, but excludes publication-time identity: terrain slot,
// terrain generation, dynamic model id, stable instance id, and physical node
// slot are assigned by the install transaction.
struct SparseVoxelTerrainTree64VirtualPagePayload
{
  uint32_t internalLOD = 0u;
  uint32_t regionMappingId = 0u;
  uint64_t geometryRevision = 0u;
  uint32_t modelSide = 0u;
  SparseVoxelAABB modelLocalBounds{};
  std::vector<SparseVoxelAABB> modelLocalLeafBounds;
  std::vector<SparseVoxelTree64NodeGPU> modelNodes;
  uint64_t modelPayloadRevision = 0u;
  std::array<float, 16> instanceWorldToModel{};
  SparseVoxelAABB instanceWorldBounds{};
  uint32_t instanceFlags = 0u;
  float receiverCellSizeMetres = 0.0f;
  std::vector<SparseVoxelTree64MaterialCellGPU> materialCells;
  uint32_t sourceRunCount = 0u;
  uint32_t clippedRunCount = 0u;
  uint32_t cellScale = 1u;
  bool reusedNodes = false;
  bool reusedMaterialCells = false;
};

enum class SparseVoxelTerrainTree64LODVariantState : uint32_t
{
  Missing = 0u,
  Requested,
  Produced,
  Resident,
};

struct SparseVoxelTerrainTree64LODVariant
{
  uint32_t logicalPageId = 0u;
  uint32_t internalLOD = 0u;
  uint64_t geometryRevision = 0u;
  // Highest projected geometry error this sidecar may represent. Runtime
  // selection chooses the coarsest resident version whose threshold still
  // covers the current chunk error.
  float maximumProjectedErrorPixels = 0.0f;
  uint32_t requestedCellsPerEdge = 0u;
  uint64_t payloadBytes = 0u;
  SparseVoxelTerrainTree64LODVariantState state =
      SparseVoxelTerrainTree64LODVariantState::Missing;
  bool conservative = true;
};

uint32_t selectSparseVoxelTerrainTree64LODVariant(
    const std::vector<SparseVoxelTerrainTree64LODVariant> &variants,
    uint32_t logicalPageId,
    uint64_t geometryRevision,
    float projectedErrorPixels,
    bool requireResident = true);

enum class SparseVoxelTerrainTree64LightingSelectionStatus : uint32_t
{
  MissingDesired = 0u,
  ResidentSidecar,
  ConservativeFallback,
  GenerationMismatch,
};

struct SparseVoxelTerrainTree64LightingSelectionRequest
{
  uint32_t logicalPageId = 0u;
  uint64_t geometryRevision = 0u;
  float projectedErrorPixels = 0.0f;
  uint32_t fallbackLogicalPageId = 0u;
  uint64_t fallbackGeometryRevision = 0u;
  float fallbackProjectedErrorPixels = std::numeric_limits<float>::max();
  bool allowConservativeFallback = true;
};

struct SparseVoxelTerrainTree64LightingSelection
{
  SparseVoxelTerrainTree64LightingSelectionStatus status =
      SparseVoxelTerrainTree64LightingSelectionStatus::MissingDesired;
  uint32_t desiredVariantIndex = std::numeric_limits<uint32_t>::max();
  uint32_t selectedVariantIndex = std::numeric_limits<uint32_t>::max();
  uint32_t requestedLogicalPageId = 0u;
  uint64_t requestedGeometryRevision = 0u;
  uint32_t selectedLogicalPageId = 0u;
  uint64_t selectedGeometryRevision = 0u;
  uint32_t selectedInternalLOD = 0u;
  bool traceable = false;
  bool requestedDesiredSidecar = false;
  bool usedFallback = false;
  bool generationMismatch = false;
};

bool selectSparseVoxelTerrainTree64LightingSidecar(
    const std::vector<SparseVoxelTerrainTree64LODVariant> &variants,
    const SparseVoxelTerrainTree64LightingSelectionRequest &request,
    SparseVoxelTerrainTree64LightingSelection &selection,
    SparseVoxelCutDiagnostics *diagnostics = nullptr,
    std::string *error = nullptr);

SparseVoxelTerrainTree64VirtualPagePayload
sparseVoxelTerrainTree64VirtualPayloadFromPagePayload(
    const SparseVoxelTerrainTree64PagePayload &payload);
SparseVoxelTerrainTree64PagePayload
sparseVoxelTerrainTree64VirtualPayloadToPagePayload(
    const SparseVoxelTerrainTree64VirtualPagePayload &payload,
    uint32_t terrainSlot,
    uint32_t terrainGeneration);
std::vector<uint8_t> serializeSparseVoxelTerrainTree64VirtualPagePayload(
    const SparseVoxelTerrainTree64VirtualPagePayload &payload);
bool deserializeSparseVoxelTerrainTree64VirtualPagePayload(
    const std::vector<uint8_t> &bytes,
    SparseVoxelTerrainTree64VirtualPagePayload &payload,
    std::string *error = nullptr);

struct SparseVoxelTerrainTree64PagePayloadSource
{
  const PlanetVoxelChunkSet *chunks = nullptr;
  uint32_t chunkIndex = 0u;
  uint32_t requestedCellsPerEdge = 0u;
  uint32_t maximumNodeCount = 0u;
  uint64_t geometryRevision = 0u;
  // Optional immutable model with the exact geometry revision. Instance
  // placement and material cells are always reconstructed from the new raster
  // record, so reuse cannot preserve stale toroidal ownership.
  const SparseVoxelTree64DynamicModelSource *reusableModel = nullptr;
  const std::vector<SparseVoxelTree64MaterialCellGPU> *reusableMaterialCells =
      nullptr;
  uint32_t reusableSourceRunCount = 0u;
  uint32_t reusableClippedRunCount = 0u;
  // Ten-centimetre terrain uses Tree64 occupancy directly. Material cells are
  // needed only by the retired exact sub-voxel compatibility fixtures.
  bool includeMaterialGeometry = false;
  // Build coarse spatial-LOD pages inside the terrain instead of around it.
  //
  // The finest level is left exact: it is voxel-for-voxel the geometry the
  // raster draws, its voxel is 10 cm, and the near field is where an exact
  // match matters. Every coarser level has a voxel of metres to hundreds of
  // metres, and an outward page there is a false occluder the size of the
  // error. See SparseVoxelTerrainPageTree64Source::conservativeInside.
  bool conservativeInsideCoarsePages = true;
};

bool buildSparseVoxelTerrainTree64PagePayload(
    const SparseVoxelTerrainTree64PagePayloadSource &source,
    SparseVoxelTerrainTree64PagePayload &payload,
    std::string *error = nullptr);

} // namespace voxel::svraster
