#pragma once

#include "procedural/PlanetCubeMapping.hpp"
#include "procedural/PlanetGeology.hpp"
#include "procedural/PlanetPackage.hpp"
#include "voxel/material/VoxelMaterialFieldAtlas.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/TransientSparseVoxelPageStore.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace procedural
{
struct Point2;
}

namespace procedural::world
{
enum class CellType : uint8_t;
struct MapExportBundle;
struct MapExportBiomeSample;
}

namespace voxel::svraster
{

// Internal fixed-footprint refinement is limited by the brick-aligned page
// resolution. It is intentionally separate from spatial clipmap coverage.
inline constexpr uint32_t kPlanetVoxelMaximumLODLevels = 8u;
// Sixteen spatial levels cover planetary view distances even when the finest
// authored/runtime page is only a few tens of metres wide. A 31.25 m page and
// radius-one rings need eleven levels for a conservative 25 km cut.
inline constexpr uint32_t kPlanetVoxelMaximumClipmapLevels = 16u;

// Automatic terrain workgroups use the machine aggressively while reserving
// two hardware threads for the renderer, input, and asynchronous coordinator.
// The eight-worker ceiling bounds temporary per-chunk voxelization memory.
uint32_t recommendedPlanetVoxelWorkerCount(uint32_t hardwareThreadCount);

enum class PlanetVoxelChunkStrategy : uint8_t
{
  // Legacy concentric rings. Retained for package/tools compatibility.
  ClipmapRings = 0u,
  // A fixed toroidal grid of independently culled terrain instances. Every
  // slot starts from its coarsest admitted heightmap resolution and GPU
  // feedback rewrites that same slot toward finer internal LODs. Walking
  // remaps entering chunks onto slots leaving the opposite side of the grid;
  // neither refinement nor movement changes the instance count.
  ProgressiveLOD = 1u,
};

struct PlanetVoxelChunkSettings
{
  // Horizontal cells in one independently built runtime page. It need not be
  // a power of two, but cache windows require it to be aligned to the 8-cell
  // brick side. For example, a 31.25 m page with 160 cells has 19.53 cm leaves.
  uint32_t horizontalVoxelCount = 64u;
  // Zero uses metadata.nominalRenderPatchEdgeMetres. Runtime LOD streaming can
  // request a deeper cube-quadtree page and provide its smaller metric extent
  // without changing the exported package contract.
  double patchEdgeMetres = 0.0;
  // Solid terrain retained below the lowest sampled surface. Deeper geology
  // can be generated later without changing the package sampling path.
  uint32_t foundationDepthVoxels = 4u;
  // Generated planet terrain is a height field, so retaining every hidden
  // voxel down to the lowest point wastes most of a distant page. Keep this
  // many cells below each local surface, extending the shell down to lower
  // neighbours where necessary to close visible cliffs. Zero restores the
  // legacy fully solid columns. The bounded shell lets spatial pages retain
  // their full horizontal sampling rate instead of compounding a page-
  // resolution drop with the spatial LOD scale.
  uint32_t surfaceShellDepthVoxels = 4u;
  // Vertical quantization range per page, measured from the shared elevation
  // anchor of the page's ancestor group. The anchor origin is the minimum
  // over the ancestor's whole footprint, so on steep ground a child page high
  // on the slope needs capacity for the ancestor's relief, not its own. At
  // 128 any ground steeper than about two page edges of relief failed
  // generation at every LOD, and because a refinement transaction is
  // all-or-nothing the owning chunk then could never refine: the request was
  // re-accepted and re-rejected forever. This bounds quantization indices,
  // not dense storage - shells stay sparse - so the cost of headroom is an
  // estimate, not memory.
  uint32_t maximumVerticalVoxels = 1024u;
  // Dilation radius, in columns, of the conservative mesh envelope. It must
  // match the column stride of any mesh displaced by it: bilinear
  // interpolation between corners a stride apart is only guaranteed to stay
  // above the enclosed columns when each corner carries the maximum over that
  // whole stride.
  uint32_t surfaceEnvelopeRadius = 1u;
  uint32_t maximumBricks = 1u << 18u;
  // Adds deterministic, cube-seam-safe metre-scale relief below the stored
  // raster resolution. The exported mountain and tectonic maps modulate the
  // amplitude, while the package content hash keeps every regenerated page
  // bit-for-bit stable. Zero preserves the authored height raster exactly.
  double proceduralDetailStrength = 1.0;
  // Smooth height-band remapping creates broad, level shelves separated by
  // short organic risers. Landform-specific weights leave conditioned plains
  // and floodplains largely untouched while plateaus, cliffs, and canyon walls
  // retain strong benches for traversal and asset placement.
  double proceduralTerraceStrength = 1.0;
  double proceduralTerraceHeightMetres = 4.0;
  // Fraction of each height band occupied by the transition at either end.
  // The remainder is exactly level when the landform weight is one.
  double proceduralTerraceTransitionFraction = 0.16;
  // Number of internal resolutions the runtime may request. ProgressiveLOD
  // starts every fixed-footprint chunk at its coarsest heightmap resolution
  // and refines individual resident chunks in place;
  // ClipmapRings retains the legacy overlapping layout for compatibility.
  uint32_t clipmapLevelCount = 1u;
  // Bit N admits internal chunk LOD N (zero is finest). GPU feedback advances
  // one chunk to the next finer admitted bit; unrequested resolutions are
  // never generated.
  uint32_t requestedLODMask = 1u;
  // In the legacy ClipmapRings strategy, coarser levels omit this central
  // square of pages because the next-finer
  // level already covers it. One leaves a one-page transition overlap around
  // that finer square for residency continuity. Packed coarse nodes carry the
  // exact finer-owned rectangle, so raster and Tree64 consumers clip the
  // overlap instead of depth-testing two different terrain resolutions.
  uint32_t clipmapInnerHoleRadius = 1u;
  // Spatial rings may lower their per-page raster resolution after a fixed
  // number of distance doublings. Zero keeps every ring at the base page
  // resolution. The floor must remain brick aligned. These controls affect
  // only ClipmapRings; ProgressiveLOD keeps its existing feedback contract.
  uint32_t clipmapResolutionDropInterval = 0u;
  uint32_t clipmapMinimumHorizontalVoxelCount = 0u;
  PlanetVoxelChunkStrategy strategy = PlanetVoxelChunkStrategy::ProgressiveLOD;
  // Maximum number of independent chunk slots rewritten by one asynchronous
  // update. Zero has no per-update limit.
  uint32_t maximumGeneratedChunksPerUpdate = 8u;
  // Maximum fixed toroidal-grid instance count. Zero leaves it unbounded.
  uint32_t maximumResidentChunks = 0u;
  // Independent missing pages are voxelized by one bounded CPU workgroup.
  // Zero selects the recommended bounded workgroup: up to eight workers while
  // reserving two hardware threads for rendering and input. Explicit values
  // can use up to 64 workers; one keeps deterministic serial execution.
  uint32_t workerCount = 0u;
  // See SparseVoxelBrickBuildSettings::buildMicroSurface: off on routes that
  // never read per-voxel micro-surface words (the brick draw and the Tree64
  // material extraction are their only readers).
  bool buildMicroSurface = true;
  // Optional material-driven microgeometry table. The pointed-to library must
  // outlive the build/update call. Its caller-owned revision participates in
  // cache identity so geometry changes cannot reuse stale transient pages.
  const SparseVoxelMicroMaterialLibrary *microMaterials = nullptr;
  const voxel::material::VoxelMaterialManager *voxelMaterials = nullptr;
  // Terrain builders no longer emit descriptor-driven exact micro geometry.
  // They still write the compact terrain context byte used by material/normal
  // passes, but brick palette entries remain descriptor 0 for terrain chunks.
  bool microGeometry = false;
  uint64_t microMaterialRevision = 0u;
};

// Smallest number of concentric spatial levels whose square resident extent
// reaches requestedDistanceMetres from its centre. Returns zero for invalid
// inputs and clamps to kPlanetVoxelMaximumClipmapLevels.
uint32_t planetVoxelClipmapLevelCountForDistance(double finePageEdgeMetres, uint32_t chunkRadius, double requestedDistanceMetres);

// Stable 32-bit logical identity for a generated planet material-field region.
// It deliberately does not encode cache/clipmap residency.
uint32_t planetVoxelMaterialRegionMappingId(const procedural::PlanetTileAddress &tile);

enum class PlanetVoxelGeneratedMaterialField : uint32_t
{
  Grass = 0u,
  Dirt = 1u,
  Variation = 2u,
  Count = 3u,
};

// Canonical biome material used by planet voxel generation. Background and
// fallback renderers use the same packed sRGB albedo so a representation
// handoff does not silently switch to a separate preview palette.
SparseVoxelMaterialGPU planetVoxelBaseMaterial(uint8_t material);

// Spatially varied surface material shared by the near voxel pages and the
// coarse background planet. Direction is planet-space and therefore remains
// continuous across streamed pages and cube faces.
SparseVoxelMaterialGPU planetVoxelSurfaceMaterial(uint8_t material, const procedural::SpherePoint &direction, const procedural::PlanetPackageMetadata &metadata, double slope = 0.0, double mountain = 0.0);

// Runtime landform detail below the bake's cell size. lithology selects the
// behaviour — terraces on layered rock, tors and boulder fields on resistant
// rock, scree on weak rock — so a caller without it gets the neutral default.
// strength keeps its position ahead of lithology deliberately. Inserting a new
// parameter before an existing defaulted one silently rebinds every positional
// call: the two call sites here passed their strength as the sixth argument,
// and a double-to-uint8_t narrowing is legal in a call, so a strength of 2.0
// became "limestone" with strength falling back to 1.0 and nothing failed to
// compile.
double planetVoxelTerrainDetail(
    const procedural::SpherePoint &direction,
    const procedural::PlanetPackageMetadata &metadata,
    double mountain,
    double tectonics,
    uint8_t material,
    double strength = 1.0,
    uint8_t lithology = static_cast<uint8_t>(procedural::PlanetLithology::Sedimentary));

// A generated region owns semantic R8 data, never material slot numbers or
// physical atlas coordinates. Packing supplies its canonical parent range;
// the material-field resolver converts that range to each material's microgrid.
struct PlanetVoxelMaterialFieldRegion
{
  procedural::PlanetTileAddress tile{};
  uint32_t regionMappingId = 0u;
  std::array<int32_t, 2> canonicalParentOrigin{};
  std::array<uint32_t, 2> canonicalParentExtent{};
  std::array<std::vector<uint8_t>, static_cast<size_t>(PlanetVoxelGeneratedMaterialField::Count)> values;
};

bool generatePlanetVoxelMaterialFieldRegion(
    const procedural::PlanetPackageMetadata &metadata,
    const procedural::PlanetTileAddress &tile,
    const std::array<int32_t, 2> &canonicalParentOrigin,
    const std::array<uint32_t, 2> &canonicalParentExtent,
    PlanetVoxelMaterialFieldRegion &region,
    std::string *error = nullptr);

struct PlanetVoxelChunk
{
  struct MaterialReference
  {
    uint16_t handle = 0u;
    uint32_t contextFields = 0u;
    uint64_t assetHash = 0u;
    std::string assetId;

    bool operator==(const MaterialReference &other) const
    {
      return handle == other.handle && contextFields == other.contextFields && assetHash == other.assetHash && assetId == other.assetId;
    }
  };

  procedural::PlanetTileAddress requestedTile{};
  procedural::PlanetTileAddress sourceTile{};
  SparseVoxelBrickHierarchy bricks;
  // Dense per-column surface height in metres, the exact field the voxel
  // columns were built from: authored raster plus procedural detail. Stored
  // with the same neighbour halo the build already samples, so consumers can
  // reconstruct a seam-free surface across chunk boundaries without knowing
  // anything about the neighbouring pages. Side is horizontalVoxelCount plus
  // twice the halo; row-major, X fastest, both spanning [-halo, side + halo).
  uint32_t surfaceHeightSide = 0u;
  uint32_t surfaceHeightHalo = 0u;
  std::vector<float> surfaceHeights;
  // Conservative mesh envelope: per column, the largest voxel *top face* among
  // its 3x3 neighbourhood, in the same absolute metres and the same halo
  // layout as surfaceHeights. A mesh displaced by this is at or above every
  // enclosed column's top voxel, which is what makes a proxy mesh a strict
  // superset of the voxel surface - interpolating the height field itself
  // leaves every column's top voxel outside the mesh, which under a carve is a
  // hole rather than a seam.
  std::vector<float> surfaceEnvelope;
  // Appearance of each column's topmost voxel, two words per interior column
  // in the same packed brick material format, row-major over
  // horizontalVoxelCount^2 with no halo. Word one carries the micro context
  // byte in bits 20..27, which are otherwise the palette metadata a mesh
  // consumer has no palette for.
  std::vector<uint32_t> surfaceMaterials;
  double edgeMetres = 0.0;
  double voxelSizeMetres = 0.0;
  double elevationOriginMetres = 0.0;
  double minimumSurfaceMetres = 0.0;
  double maximumSurfaceMetres = 0.0;
  double centerSurfaceMetres = 0.0;
  uint8_t centerMaterial = 0u;
  uint32_t leafVoxelCount = 0u;
  // Stable material identity and optional baked context survive transient
  // spill/restore independently from process-local manager slots.
  std::vector<MaterialReference> materialReferences;
  uint32_t bakedContextFields = 0u;
  std::vector<uint16_t> auxiliaryContextIndices;
  std::vector<uint32_t> auxiliaryContextPool;
};

using PlanetVoxelChunkProgress = std::function<void(const char *phase, double progress)>;

struct PlanetVoxelChunkRecord
{
  procedural::PlanetTileAddress requestedTile{};
  procedural::PlanetTileAddress sourceTile{};
  int32_t tileOffsetX = 0;
  int32_t tileOffsetY = 0;
  uint32_t clipmapLOD = 0u;
  uint32_t clipmapSlot = 0u;
  // Stable logical page identity copied into every raster brick in this
  // record. This is diagnostic ownership, not a physical arena address.
  uint32_t regionMappingId = 0u;
  // Deviation this page's geometry carries against the finest representation
  // of the same ground, in world units - the maximum measured brick error in
  // the page, which by monotonicity is its root brick's.
  //
  // The spatial cut sizes pages from voxel size alone, which cannot tell a flat
  // plain from a cliff and so spends the same detail on both. A page that
  // reports what it actually lost lets the cut stop subdividing ground that has
  // nothing left to resolve. Zero means "not measured" - a page the streamer
  // has never built - and the cut falls back to the geometric estimate there.
  //
  // This word was explicit padding. Whole records are hashed into
  // brickPayloadRevision and serialized into the transient page store, so an
  // unwritten gap makes a page's identity depend on whatever the allocator left
  // behind - see the matching note on SparseVoxelBrickNodeGPU.
  float measuredErrorMetres = 0.0f;
  double worldCenterXMetres = 0.0;
  double worldCenterZMetres = 0.0;
  double edgeMetres = 0.0;
  double voxelSizeMetres = 0.0;
  uint32_t brickNodeOffset = 0u;
  uint32_t brickNodeCapacity = 0u;
  uint32_t brickChildOffset = 0u;
  uint32_t brickChildCapacity = 0u;
  uint32_t brickMaterialOffset = 0u;
  uint32_t brickMaterialCapacity = 0u;
  uint32_t brickPaletteOffset = 0u;
  uint32_t brickPaletteCapacity = 0u;
  // Authored payload identity and final translated-node identity are separate:
  // a retained page can move or change cascade ownership without requiring
  // its occupancy, material, and palette pages to be uploaded again.
  uint64_t brickPayloadRevision = 0u;
  uint64_t brickNodeRevision = 0u;
  // Geometry/ownership identity for acceleration structures. Unlike the node
  // revision, this deliberately excludes physical GPU arena offsets so a
  // capacity remap cannot invalidate otherwise unchanged Tree64 models.
  uint64_t tree64Revision = 0u;
  uint32_t firstBrickRoot = 0u;
  uint32_t brickRootCapacity = 0u;
  uint32_t brickRootCount = 0u;
  double elevationOriginMetres = 0.0;
  double minimumSurfaceMetres = 0.0;
  double maximumSurfaceMetres = 0.0;
  double centerSurfaceMetres = 0.0;
  uint8_t centerMaterial = 0u;
  std::array<uint8_t, 3> reservedAfterCenterMaterial{};
  uint32_t leafVoxelCount = 0u;
  uint32_t brickCount = 0u;
  // Non-zero only for a generated-world owner. GPU feedback echoes this value
  // so a request recorded before physical-slot reuse cannot refine the
  // replacement chunk.
  uint32_t residencyGeneration = 0u;
  // CPU-only admission rank used to preserve camera-near/visible ordering
  // after records are packed by their stable physical slot. Higher values are
  // generated first; this is scheduling metadata, not payload identity.
  uint32_t streamingPriority = 0u;
  uint32_t reservedTailPadding = 0u;
};

// Pinned because the record is hashed and serialized as raw bytes: a new field
// that reintroduces an unwritten gap breaks page identity in a way that only
// shows up as spurious cache misses.
static_assert(sizeof(PlanetVoxelChunkRecord) == 208u);
static_assert(offsetof(PlanetVoxelChunkRecord, worldCenterXMetres) == 48u);
static_assert(offsetof(PlanetVoxelChunkRecord, leafVoxelCount) == 188u);

// CPU-side compare-and-swap identity for one fixed generated-world chunk.
// Raster storage and Tree64 geometry use the same logical page identity even
// though their physical allocators are independent. A zero generation is the
// invalid/unmapped identity used for root installation and final removal.
struct PlanetVoxelHierarchyPageIdentity
{
  uint32_t slot = 0xffffffffu;
  uint32_t generation = 0u;
  uint32_t internalLOD = 0u;
  uint64_t rasterAllocation = 0u;
  uint64_t geometryRevision = 0u;

  bool valid() const
  {
    return slot != 0xffffffffu && generation != 0u &&
        rasterAllocation != 0u && geometryRevision != 0u;
  }
};

enum class PlanetVoxelHierarchyRewriteKind : uint8_t
{
  InstallRoot,
  Refine,
  Coarsen,
  ReplaceOwner,
  RemoveOwner,
};

// Publication installs both the raster allocation and the independently
// uploaded Tree64 page before this rewrite becomes visible. `expected` is the
// currently committed parent/cut and `replacement` is the new active cut.
// Keeping this record explicit prevents either renderer from deriving a
// different transition from mutable chunk arrays.
struct PlanetVoxelHierarchyPageRewrite
{
  PlanetVoxelHierarchyRewriteKind kind =
      PlanetVoxelHierarchyRewriteKind::InstallRoot;
  PlanetVoxelHierarchyPageIdentity expected{};
  PlanetVoxelHierarchyPageIdentity replacement{};
};

inline constexpr uint32_t kWorldVoxelInvalidPage = 0xffffffffu;

enum class TerrainPagePatchKind : uint8_t
{
  SetChildRange,
  ClearChildRange,
  SetStreamingLeaf,
  ClearStreamingLeaf,
  SetPageInstalled,
  ClearPageInstalled,
};

struct TerrainPagePatch
{
  TerrainPagePatchKind kind = TerrainPagePatchKind::SetPageInstalled;
  uint32_t pageIndex = kWorldVoxelInvalidPage;
  uint32_t firstChild = kWorldVoxelInvalidPage;
  uint32_t childCount = 0u;
  uint32_t flags = 0u;
};

struct PlanetVoxelTerrainPhysicalRange
{
  uint32_t offset = 0u;
  uint32_t count = 0u;
};

inline constexpr uint32_t kWorldVoxelTerrainRuntimePageResident = 1u << 0u;
inline constexpr uint32_t kWorldVoxelTerrainRuntimePagePayloadInstalled =
    1u << 1u;
inline constexpr uint32_t kWorldVoxelTerrainRuntimePageChildRangeEnabled =
    1u << 2u;
inline constexpr uint32_t kWorldVoxelTerrainRuntimePageStreamingLeaf =
    1u << 3u;
inline constexpr uint32_t kWorldVoxelTerrainRuntimePageRequestable =
    1u << 4u;
inline constexpr uint32_t kWorldVoxelTerrainRuntimeNodeResident = 1u << 0u;
inline constexpr uint32_t kWorldVoxelTerrainRuntimeNodeStreamingLeaf =
    1u << 1u;
inline constexpr uint32_t kWorldVoxelTerrainRuntimeNodeDrawFallback = 1u << 2u;

// Destination ABI for the generated-world terrain page DAG.
//
// Logical pages/nodes stay present while physical payload ranges come and go.
// A missing child page therefore remains requestable, and traversal can draw
// the current node as fallback instead of losing coverage.
struct alignas(16) WorldVoxelTerrainRuntimePageGPU
{
  uint32_t parentPage = kWorldVoxelInvalidPage;
  // Offset into WorldVoxelTerrainRuntimePageTableGPU::childPageIndices.
  uint32_t firstChildPage = kWorldVoxelInvalidPage;
  uint32_t childCount = 0u;
  uint32_t flags = kWorldVoxelTerrainRuntimePageRequestable;
  uint32_t firstNode = kWorldVoxelInvalidPage;
  uint32_t nodeCount = 0u;
  uint32_t rootNode = kWorldVoxelInvalidPage;
  uint32_t payloadSlot = kWorldVoxelInvalidPage;
  uint32_t residencyGeneration = 0u;
  uint32_t payloadRevisionLo = 0u;
  uint32_t payloadRevisionHi = 0u;
  uint32_t reserved0 = 0u;
};

struct alignas(16) WorldVoxelTerrainRuntimeNodeGPU
{
  // xyz minimum and xyz maximum are in renderer-local metres. min.w stores
  // this node's monotonic geometric error; max.w is reserved for parent or
  // fallback error once the traversal stops deriving it from page state.
  std::array<float, 4u> worldBoundsMinError{};
  std::array<float, 4u> worldBoundsMaxError{};
  uint32_t owningPage = kWorldVoxelInvalidPage;
  uint32_t firstChildNode = kWorldVoxelInvalidPage;
  uint32_t childCount = 0u;
  uint32_t flags = 0u;
  uint32_t fallbackBrickRoot = kWorldVoxelInvalidPage;
  uint32_t fallbackBrickRootCount = 0u;
  uint32_t payloadNodeOffset = 0u;
  uint32_t payloadNodeCount = 0u;
};

struct alignas(16) WorldVoxelTerrainRuntimePageRequestGPU
{
  uint32_t pageIndex = kWorldVoxelInvalidPage;
  uint32_t desiredLOD = 0u;
  uint32_t priority = 0u;
  uint32_t requesterNode = kWorldVoxelInvalidPage;
};

static_assert(sizeof(WorldVoxelTerrainRuntimePageGPU) == 48u);
static_assert(sizeof(WorldVoxelTerrainRuntimeNodeGPU) == 64u);
static_assert(sizeof(WorldVoxelTerrainRuntimePageRequestGPU) == 16u);

struct WorldVoxelTerrainRuntimePageTableGPU
{
  std::vector<WorldVoxelTerrainRuntimePageGPU> pages;
  std::vector<WorldVoxelTerrainRuntimeNodeGPU> nodes;
  std::vector<uint32_t> childPageIndices;
  std::vector<uint32_t> rootPages;
};

// CPU publication packet for one generated-world terrain page. The page can be
// uploaded while it is still hidden (`visible == false`); a later hierarchy
// rewrite makes the already-uploaded ranges reachable.
struct PlanetVoxelTerrainPageUpload
{
  PlanetVoxelHierarchyPageIdentity identity{};
  int32_t pageX = 0;
  int32_t pageZ = 0;
  uint32_t spatialLOD = 0u;
  uint32_t residentLOD = 0u;
  uint64_t payloadRevision = 0u;
  uint64_t nodeRevision = 0u;
  bool visible = false;
  PlanetVoxelTerrainPhysicalRange nodes{};
  PlanetVoxelTerrainPhysicalRange occupancy{};
  PlanetVoxelTerrainPhysicalRange childIndices{};
  PlanetVoxelTerrainPhysicalRange roots{};
  PlanetVoxelTerrainPhysicalRange materialIndices{};
  PlanetVoxelTerrainPhysicalRange microSurfaceData{};
  PlanetVoxelTerrainPhysicalRange palette{};
  PlanetVoxelTerrainPhysicalRange paletteDescriptorIds{};
  uint64_t payloadBytes = 0u;
  uint64_t publicationBytes = 0u;
  std::vector<TerrainPagePatch> installPatches;
};

struct PlanetVoxelChunkSet
{
  procedural::PlanetTileAddress centerTile{};
  std::vector<PlanetVoxelChunkRecord> chunks;
  SparseVoxelBrickHierarchy bricks;
  double edgeMetres = 0.0;
  double voxelSizeMetres = 0.0;
  double elevationOriginMetres = 0.0;
  double centerSurfaceMetres = 0.0;
  uint8_t centerMaterial = 0u;
  uint32_t leafVoxelCount = 0u;
  uint64_t clipmapLayoutRevision = 0u;
  // Phase-3 feedback contract. Packaged planet pages leave this disabled and
  // continue to use their original request path.
  bool progressiveGeneratedWorld = false;
  // Generated page payloads are absent from the CPU-side hierarchy. The
  // arrays below reserve renderer-visible physical ranges only; the terrain
  // GPU build chain fills those ranges and atomically publishes their page
  // state. CPU code must not upload the placeholder contents or cache them as
  // geometry.
  bool gpuOwnedGeneratedPayloads = false;
  // Physical GPU arena sizes when generated payload arrays are intentionally
  // absent above. Keeping these counts separate from std::vector::size() is
  // what prevents a metadata-only page plan from allocating a zero-filled
  // CPU mirror of every resident node, occupancy word, and material cell.
  uint32_t gpuArenaNodeCapacity = 0u;
  uint32_t gpuArenaChildCapacity = 0u;
  uint32_t gpuArenaRootCapacity = 0u;
  uint32_t gpuArenaMaterialCapacity = 0u;
  uint32_t gpuArenaPaletteCapacity = 0u;
  // xyz renderer-world origin and w finest spacing of the generated terrain's
  // canonical integer lattice. The raster uses it to evaluate shared voxel
  // planes identically across independently packed pages.
  std::array<float, 4> canonicalWorldGrid{};
  // Physical arrays are a fixed virtual-page arena. Only ranges referenced by
  // the chunk records are active; unused and retired ranges remain zero until
  // a final publication packet activates a replacement hierarchy.
  bool progressiveVirtualPages = false;
  // Full virtual-page slot table, including currently empty boundary slots.
  // Later spatial-cut rewrites may populate any slot in this table.
  uint32_t progressiveSlotCapacity = 0u;
  // Diameter of the requested render-distance cut (or the fixed-grid window
  // used by deterministic unit fixtures). This sizes consumers before
  // asynchronous roots have filled every slot.
  double progressiveWindowExtentMetres = 0.0;
  float progressiveSplitErrorPixels = 4.0f;
  float progressiveMergeErrorPixels = 2.5f;
  float progressiveDistanceErrorGrowthPerKilometre = 0.10f;
  float progressiveDistanceErrorExponent = 1.0f;
  uint32_t progressiveMinimumResidentFrames = 4u;
  uint32_t progressiveCoarseningGraceFrames = 30u;
  // Stable-slot hierarchy rewrites prepared by the generated-world streamer.
  // The render thread validates these identities against both the committed
  // raster page state and the committed Tree64 descriptor, uploads the new
  // payloads, then applies them in the final atomic publication dispatch.
  std::vector<PlanetVoxelHierarchyPageRewrite> hierarchyPageRewrites;
  // Physical page uploads prepared this transaction. This is the generated
  // terrain equivalent of virtual geometry page streaming: payload ranges are
  // copied to their final arena offsets before the owner is published.
  std::vector<PlanetVoxelTerrainPageUpload> terrainPageUploads;
  // Complete runtime page/node directory projected from the CPU page table.
  // This is the CPU-side mirror of the future GPU traversal input.
  WorldVoxelTerrainRuntimePageTableGPU terrainRuntimePageTable;
  // Per-chunk surface height rasters concatenated in `chunks` record order,
  // every chunk contributing surfaceHeightSide^2 floats. One shared side keeps
  // the GPU-resident form a plain array slice per chunk.
  uint32_t surfaceHeightSide = 0u;
  uint32_t surfaceHeightHalo = 0u;
  std::vector<float> surfaceHeights;
  std::vector<float> surfaceEnvelope;
  // Per-chunk surface materials concatenated in the same record order, two
  // words per column over surfaceMaterialSide^2.
  uint32_t surfaceMaterialSide = 0u;
  std::vector<uint32_t> surfaceMaterials;
  std::vector<PlanetVoxelMaterialFieldRegion> materialFieldRegions;
  std::array<double, 3> minimumWorldMetres{};
  std::array<double, 3> maximumWorldMetres{};
};

// Relocatable generated-world terrain page payload.
//
// This is the transient/page-source form, not the renderer publication form:
// offsets are local to the payload arrays, and physical GPU arena placement,
// stable slot ownership, residency generation, and publication rewrites are
// deliberately excluded. The compatibility adapters below convert to and from
// `PlanetVoxelChunkSet` while the existing renderer path is still being
// migrated.
struct TerrainVirtualPageRecord
{
  procedural::PlanetTileAddress requestedTile{};
  procedural::PlanetTileAddress sourceTile{};
  int32_t tileOffsetX = 0;
  int32_t tileOffsetY = 0;
  uint32_t clipmapLOD = 0u;
  uint32_t regionMappingId = 0u;
  float measuredErrorMetres = 0.0f;
  double worldCenterXMetres = 0.0;
  double worldCenterZMetres = 0.0;
  double edgeMetres = 0.0;
  double voxelSizeMetres = 0.0;
  uint32_t localBrickNodeOffset = 0u;
  uint32_t localBrickNodeCount = 0u;
  uint32_t localBrickChildOffset = 0u;
  uint32_t localBrickChildCount = 0u;
  uint32_t localBrickMaterialOffset = 0u;
  uint32_t localBrickMaterialCount = 0u;
  uint32_t localBrickPaletteOffset = 0u;
  uint32_t localBrickPaletteCount = 0u;
  uint64_t brickPayloadRevision = 0u;
  uint64_t brickNodeRevision = 0u;
  uint64_t tree64Revision = 0u;
  uint32_t localFirstBrickRoot = 0u;
  uint32_t localBrickRootCount = 0u;
  double elevationOriginMetres = 0.0;
  double minimumSurfaceMetres = 0.0;
  double maximumSurfaceMetres = 0.0;
  double centerSurfaceMetres = 0.0;
  uint8_t centerMaterial = 0u;
  uint32_t leafVoxelCount = 0u;
  uint32_t brickCount = 0u;
};

struct TerrainVirtualPagePayload
{
  procedural::PlanetTileAddress centerTile{};
  std::vector<TerrainVirtualPageRecord> records;
  SparseVoxelBrickHierarchy bricks;
  double edgeMetres = 0.0;
  double voxelSizeMetres = 0.0;
  double elevationOriginMetres = 0.0;
  double centerSurfaceMetres = 0.0;
  uint8_t centerMaterial = 0u;
  uint32_t leafVoxelCount = 0u;
  uint64_t payloadRevision = 0u;
  uint32_t surfaceHeightSide = 0u;
  uint32_t surfaceHeightHalo = 0u;
  std::vector<float> surfaceHeights;
  std::vector<float> surfaceEnvelope;
  uint32_t surfaceMaterialSide = 0u;
  std::vector<uint32_t> surfaceMaterials;
  std::vector<PlanetVoxelMaterialFieldRegion> materialFieldRegions;
  std::array<double, 3> minimumWorldMetres{};
  std::array<double, 3> maximumWorldMetres{};
};

// Resolves the top face of the resident voxel column containing a world-space
// X/Z position. The returned Y is in the chunk set's renderer-local frame
// (absolute authored elevation minus elevationOriginMetres), exactly matching
// the translated brick geometry. At a page boundary the highest touching
// column wins so a camera or placement cannot start inside either owner.
bool samplePlanetVoxelSurfaceTop(
    const PlanetVoxelChunkSet &chunks,
    double worldXMetres,
    double worldZMetres,
    double &localTopMetres);

// Replaces only planet-owned entries in the atlas. Field semantics declared
// by each material choose which generated channel enters which logical slot.
bool populatePlanetVoxelMaterialFieldAtlas(const PlanetVoxelChunkSet &chunks, const voxel::material::VoxelMaterialManager &materials, voxel::material::VoxelMaterialFieldAtlas &atlas, std::string *error = nullptr);

struct PlanetVoxelChunkCacheEntry
{
  int32_t tileOffsetX = 0;
  int32_t tileOffsetY = 0;
  uint32_t clipmapLOD = 0u;
  uint32_t clipmapSlot = 0u;
  double worldCenterXMetres = 0.0;
  double worldCenterZMetres = 0.0;
  procedural::PlanetTileAddress tile{};
  PlanetVoxelChunk chunk;
};

struct PlanetVoxelChunkCache
{
  procedural::PlanetTileAddress anchorTile{};
  int32_t centerOffsetX = 0;
  int32_t centerOffsetY = 0;
  uint32_t chunkRadius = 0u;
  PlanetVoxelChunkSettings settings{};
  double worldElevationOriginMetres = 0.0;
  bool hasWorldElevationOrigin = false;
  std::vector<PlanetVoxelChunkCacheEntry> entries;
  // Fixed-capacity physical slots keep retained pages at stable packed offsets
  // while entering pages overwrite slots vacated by the toroidal window. Each
  // slot owns independent guarded ranges so one dense page does not inflate
  // every page in its LOD.
  uint32_t clipmapSlotCount = 0u;
  std::vector<uint32_t> clipmapSlotNodeCapacity;
  std::vector<uint32_t> clipmapSlotChildCapacity;
  std::vector<uint32_t> clipmapSlotRootCapacity;
  std::vector<uint32_t> clipmapSlotMaterialCapacity;
  std::vector<uint32_t> clipmapSlotPaletteCapacity;
  uint64_t clipmapLayoutRevision = 0u;
  // Remaining GPU-requested chunk rewrites that did not fit this update's
  // bounded generation budget.
  uint32_t pendingChunkCount = 0u;
  uint32_t requestedChunkCount = 0u;
  // Pages outside the resident window remain compressed in this transient
  // runtime tier. It may spill to its owned temporary directory and is never
  // written back into the authored planet package.
  std::shared_ptr<TransientSparseVoxelPageStore> transientPages;
};

struct PlanetVoxelChunkCacheUpdate
{
  uint32_t workerCount = 0u;
  uint32_t reusedChunkCount = 0u;
  uint32_t restoredMemoryChunkCount = 0u;
  uint32_t restoredDiskChunkCount = 0u;
  uint32_t builtChunkCount = 0u;
  uint32_t evictedChunkCount = 0u;
  uint32_t spilledChunkCount = 0u;
  uint32_t queuedChunkCount = 0u;
  uint32_t pendingChunkCount = 0u;
  uint32_t publishedLOD = 0u;
  bool refinementComplete = true;
};

// GPU refinement feedback addresses the stable physical slot of a currently
// resident page. `priority` is an unsigned screen-space importance score; the
// cache resolves dependencies first, then services larger scores and nearer
// page centres before lower-priority work.
struct PlanetVoxelChunkRefinementRequest
{
  uint32_t clipmapSlot = 0u;
  uint32_t priority = 0u;
};

// Versioned transient payload helpers. They are public so page stores, tools,
// and regression tests all exercise one format and receive the same clear
// version/corruption diagnostics.
std::vector<uint8_t> serializePlanetVoxelTransientPage(const PlanetVoxelChunk &chunk);
bool deserializePlanetVoxelTransientPage(const std::vector<uint8_t> &bytes, PlanetVoxelChunk &chunk, std::string *error = nullptr);

TerrainVirtualPagePayload terrainVirtualPagePayloadFromChunkSet(
    const PlanetVoxelChunkSet &page);
PlanetVoxelChunkSet terrainVirtualPagePayloadToChunkSet(
    const TerrainVirtualPagePayload &payload);
std::vector<uint8_t> serializeTerrainVirtualPagePayload(
    const TerrainVirtualPagePayload &payload);
bool deserializeTerrainVirtualPagePayload(
    const std::vector<uint8_t> &bytes,
    TerrainVirtualPagePayload &payload,
    std::string *error = nullptr);

// One generated-world page in the form the streamer caches it, before any
// physical arena offsets are assigned. Exposed so a round-trip can be tested
// directly: a field added to PlanetVoxelChunkSet and not to the serializer is
// silently lost on the first eviction, which only shows up much later.
std::vector<uint8_t> serializeWorldVoxelTransientPage(
    const PlanetVoxelChunkSet &page);
bool deserializeWorldVoxelTransientPage(
    const std::vector<uint8_t> &bytes,
    PlanetVoxelChunkSet &page,
    std::string *error = nullptr);

// Searches the coarsest available height/material tiles for a broad dry-land
// sample and resolves it to the requested render level. Only those coarse LZ4
// blocks are decoded, so choosing a safe initial spawn does not load the
// planet's detailed map pyramid.
bool findPlanetLandSpawnTile(
    const procedural::PlanetPackageReader &package,
    uint8_t targetLevel,
    procedural::PlanetTileAddress &tile,
    double *surfaceMetres = nullptr,
    uint8_t *material = nullptr,
    std::string *error = nullptr);

// Loads only the independently compressed maps needed for one requested
// cube-quadtree patch, samples the closest stored ancestor when the render
// patch is finer than the package, and builds a local tangent-space brick
// hierarchy. X/Z span the patch and Y is elevation relative to
// elevationOriginMetres.
bool buildPlanetVoxelChunk(
    const procedural::PlanetPackageReader &package,
    const procedural::PlanetTileAddress &tile,
    const PlanetVoxelChunkSettings &settings,
    PlanetVoxelChunk &chunk,
    std::string *error = nullptr,
    const PlanetVoxelChunkProgress &progress = {},
    const std::atomic_bool *cancellation = nullptr);

// Resolves a logical offset in the anchor face's unbounded U/V grid. Crossing
// an edge projects naturally onto the adjacent cube face, keeping the cache
// key stable while the package address changes face and orientation.
bool planetVoxelTileAtOffset(const procedural::PlanetTileAddress &anchorTile, int32_t tileOffsetX, int32_t tileOffsetY, procedural::PlanetTileAddress &tile);

// Updates a resident square around centerOffsetX/Y. Existing logical offsets
// are moved into the new window, only entering chunks are decoded and built,
// and chunks that left the window are evicted after every new chunk succeeds.
// The returned set is a freshly packed multi-root hierarchy suitable for GPU
// publication while the individual cache entries remain reusable.
bool updatePlanetVoxelChunkCache(
    const procedural::PlanetPackageReader &package,
    const procedural::PlanetTileAddress &anchorTile,
    int32_t centerOffsetX,
    int32_t centerOffsetY,
    uint32_t chunkRadius,
    const PlanetVoxelChunkSettings &settings,
    PlanetVoxelChunkCache &cache,
    PlanetVoxelChunkSet &chunks,
    PlanetVoxelChunkCacheUpdate *update = nullptr,
    std::string *error = nullptr,
    const PlanetVoxelChunkProgress &progress = {},
    const std::atomic_bool *cancellation = nullptr,
    bool allowPhysicalLayoutGrowth = true,
    const std::vector<PlanetVoxelChunkRefinementRequest> *refinementRequests = nullptr);

// Convenience wrapper for a fresh cache centered on centerTile. The horizontal
// voxel count must be a multiple of the eight-cell brick side.
bool buildPlanetVoxelChunkGrid(
    const procedural::PlanetPackageReader &package,
    const procedural::PlanetTileAddress &centerTile,
    uint32_t chunkRadius,
    const PlanetVoxelChunkSettings &settings,
    PlanetVoxelChunkSet &chunks,
    std::string *error = nullptr,
    const PlanetVoxelChunkProgress &progress = {},
    const std::atomic_bool *cancellation = nullptr);

// Stable stochastic selection from an exported sparse biome mixture. Hashing
// the canonical finest world column dissolves map-texel boundaries without
// adding material slots or mutable random state.
uint8_t worldMapBiomeForColumn(
    const procedural::world::MapExportBiomeSample &biome,
    const procedural::Point2 &position,
    const procedural::world::MapExportBundle &bundle,
    double finestVoxelSizeMetres);

// Deterministic detail reconstructed below the exported world-map texel. The
// authored landform controls character while authored relief controls the
// complete spectrum's amplitude without saturation.
double proceduralWorldMapTerrainDetail(
    const procedural::Point2 &position,
    const procedural::world::MapExportBundle &bundle,
    double authoredReliefMetres,
    double authoredRoughness,
    procedural::world::CellType landform,
    double strength,
    double riverDistanceMetres = -1.0);

// Optional authored signals that condition the runtime detail spectrum. The
// refined elevation remains authoritative; base elevation is only a stable
// macro datum for terraces, while uplift, erosion, rivers and coasts control
// where added metre-scale detail is appropriate.
struct WorldMapTerrainSignals
{
  bool hasBaseElevation = false;
  double baseElevationMetres = 0.0;
  double upliftMetresPerYear = 0.0;
  double erosionWear = 0.0;
  double erosionFlow = 0.0;
  double erosionDeposition = 0.0;
  double riverDischargeCubicMetresPerSecond = 0.0;
  double coastDistanceMetres = -1.0;
};

// Applies deterministic detail and an organic terrace remap to authored map
// elevation. The remap operates in absolute height bands, so every spatial and
// internal LOD samples the same shelf elevation at the same world coordinate.
double proceduralWorldMapTerrainHeight(
    const procedural::Point2 &position,
    const procedural::world::MapExportBundle &bundle,
    double authoredElevationMetres,
    double authoredReliefMetres,
    double authoredRoughness,
    procedural::world::CellType landform,
    double detailStrength,
    double terraceStrength,
    double terraceHeightMetres,
    double terraceTransitionFraction,
    double riverDistanceMetres = -1.0,
    double infrastructureSmoothingMask = 0.0,
    const WorldMapTerrainSignals &signals = {});

// A map-wide startup composition: a gentle dry origin and a visible relief
// target inside the initial render distance. Normalized coordinates are in the
// export's [0, 1] domain.
struct WorldMapTerrainSpawnView
{
  bool found = false;
  double spawnU = 0.5;
  double spawnV = 0.5;
  double targetU = 0.5;
  double targetV = 0.5;
  double spawnElevationMetres = 0.0;
  double targetElevationMetres = 0.0;
  double score = 0.0;
};

bool selectWorldMapTerrainSpawnView(
    const procedural::world::MapExportBundle &bundle,
    uint8_t requestedLevel,
    double preferredU,
    double preferredV,
    double maximumViewDistanceMetres,
    WorldMapTerrainSpawnView &view,
    std::string *error = nullptr,
    bool keepPreferredSpawn = false);

// Planar generated-world input for the sparse voxel terrain path. Bounds use
// the map export's normalized X/Z coordinates and are half-open. The produced
// chunk set uses the same brick, height-envelope and material contracts as the
// existing terrain renderer, without projecting the map onto a planet.
struct WorldMapVoxelTerrainSettings
{
  std::array<double, 4> normalizedBounds{0.45, 0.45, 0.55, 0.55};
  uint8_t requestedLevel = 0u;
  // Internal hierarchy LOD for this fixed footprint. Zero is finest. When
  // finestHorizontalVoxelCount is non-zero the generated resolution is that
  // value divided by 2^internalLOD; voxel.horizontalVoxelCount remains the
  // canonical finest-grid resolution.
  uint32_t internalLOD = 0u;
  uint32_t finestHorizontalVoxelCount = 0u;
  double finestVoxelSizeMetres = 0.0;
  // Every LOD samples this common coarse anchor grid to choose the same
  // elevation origin. UINT32_MAX uses internalLOD for direct callers.
  uint32_t elevationAnchorLOD = 0xffffffffu;
  // Changes whenever deterministic generation semantics or source-derived
  // payload layout changes. It participates in the content revision/cache key.
  // Revision 14 makes landform transitions and procedural shelves continuous.
  uint64_t generatorRevision = 14u;
  // Emit top cells, exposed cliff cells, and bounded closure only. False
  // retains full-column behavior for direct generator callers and tests.
  bool exteriorShellOnly = false;
  // Sample each page-edge voxel at this many sub-intervals when deriving its
  // vertical closure. Spatially coarse pages use two so their edge wall also
  // covers the midpoint represented by an adjacent 2:1 finer page.
  uint32_t boundaryStitchSubdivisions = 1u;
  // Threads this page may use to sample its own column grid. Column samples
  // are independent, so a transaction with fewer pages than cores still fills
  // the machine. Zero or one samples serially. The caller sets it from the
  // cores its page-level parallelism leaves idle; the build pool runs nested
  // batches on the submitting thread, so a nested request cannot deadlock.
  uint32_t columnSamplingWorkerCount = 0u;
  PlanetVoxelChunkSettings voxel{};
};

// Canonical generated-terrain column ABI. Heights used for ownership are
// quantized to integer units before top/floor selection. The denominator is
// deliberately the same UNorm14 denominator used by the terrain surface
// contract, so column ownership and the G-buffer's analytic height data cannot
// disagree at an exact voxel boundary.
inline constexpr uint32_t kWorldMapTerrainColumnABIVersion = 1u;
inline constexpr uint64_t kWorldMapTerrainColumnSemanticRevision = 1u;
inline constexpr uint32_t kWorldMapTerrainHeightUnitsPerVoxel =
    kSparseVoxelMicroSurfaceCornerMask;

enum WorldMapTerrainColumnFlags : uint32_t
{
  WorldMapTerrainColumnExteriorShell = 1u << 0u,
  WorldMapTerrainColumnBoundaryStitched = 1u << 1u,
};

// Common GPU upload record. Boundary-only and wall-threshold information is
// kept in parallel arrays so future interior-only consumers pay 16 bytes per
// column rather than carrying page-edge data in every record.
struct WorldMapTerrainColumnGPU
{
  uint32_t topY = 0u;
  uint32_t firstSolidY = 0u;
  // bits 0..7 material id; bits 8..15 top-surface micro context.
  uint32_t packedMaterialContext = 0u;
  uint32_t packedFlags = 0u;
};

struct WorldMapTerrainColumnWallGPU
{
  // Floor before a boundary stitch extends the run downward.
  uint32_t firstSolidYWithoutBoundaryStitch = 0u;
  // Lowest of the four neighbour tops. A wall context applies for y greater
  // than this threshold.
  uint32_t lowestNeighbourY = 0u;
  // bits 0..7 deterministic lowest-neighbour wall; bits 8..15 page-boundary
  // wall used only below firstSolidYWithoutBoundaryStitch.
  uint32_t packedContexts = 0u;
  uint32_t reserved = 0u;
};

static_assert(sizeof(WorldMapTerrainColumnGPU) == 16u);
static_assert(sizeof(WorldMapTerrainColumnWallGPU) == 16u);
static_assert(std::is_standard_layout_v<WorldMapTerrainColumnGPU>);
static_assert(std::is_standard_layout_v<WorldMapTerrainColumnWallGPU>);

inline constexpr uint8_t worldMapTerrainColumnMaterial(
    const WorldMapTerrainColumnGPU &column)
{
  return static_cast<uint8_t>(column.packedMaterialContext & 0xffu);
}

inline constexpr uint32_t worldMapTerrainColumnSurfaceContext(
    const WorldMapTerrainColumnGPU &column)
{
  return (column.packedMaterialContext >> 8u) & 0xffu;
}

inline constexpr uint32_t worldMapTerrainColumnWallContext(
    const WorldMapTerrainColumnWallGPU &wall)
{
  return wall.packedContexts & 0xffu;
}

inline constexpr uint32_t worldMapTerrainColumnBoundaryContext(
    const WorldMapTerrainColumnWallGPU &wall)
{
  return (wall.packedContexts >> 8u) & 0xffu;
}

// CPU owner of the versioned upload records and the sidecars required to
// reproduce the current raster and Tree64 semantics without sampling maps.
struct WorldMapTerrainColumnPage
{
  uint32_t abiVersion = kWorldMapTerrainColumnABIVersion;
  uint32_t columnStride = sizeof(WorldMapTerrainColumnGPU);
  uint32_t wallStride = sizeof(WorldMapTerrainColumnWallGPU);
  uint32_t side = 0u;
  uint32_t halo = 0u;
  uint32_t maximumVerticalVoxels = 0u;
  uint32_t internalLOD = 0u;
  uint32_t flags = 0u;
  uint64_t semanticRevision = kWorldMapTerrainColumnSemanticRevision;
  // Canonical global height unit. One finest voxel contains exactly
  // kWorldMapTerrainHeightUnitsPerVoxel of these units.
  double heightUnitMetres = 0.0;
  double voxelSizeMetres = 0.0;
  double edgeMetres = 0.0;
  double elevationOriginMetres = 0.0;
  std::vector<WorldMapTerrainColumnGPU> columns;
  std::vector<WorldMapTerrainColumnWallGPU> walls;
  // Interior canonical corners, (side + 1)^2, row-major. Values are global
  // signed fixed-point heights and therefore compare exactly across pages.
  std::vector<int64_t> canonicalCornerHeights;
  // Four faces times side segments: -X, +X, -Z, +Z.
  std::vector<int64_t> canonicalBoundaryMinimumHeights;
  std::vector<int64_t> canonicalBoundaryMaximumHeights;
};

struct WorldMapTerrainColumnLeafPayload
{
  std::vector<SparseVoxelCenterLengthGPU> leaves;
  std::vector<SparseVoxelMaterialGPU> materials;
  std::vector<uint32_t> indices;
  std::vector<uint32_t> surfaceMaterials;
};

// Terrain-specific CPU fallback payload. Unlike the generic scene hierarchy
// adapter below, this consumes column runs directly: it never materializes one
// leaf record per voxel and never uses ordered-map hierarchy construction.
// Canonical ordering is restored by sorting the bounded brick records before
// each level is emitted, so its renderer payload remains byte-equivalent to
// the CPU reference implementation.
struct WorldMapTerrainColumnHierarchyPayload
{
  SparseVoxelBrickHierarchy hierarchy;
  std::vector<uint32_t> surfaceMaterials;
  uint64_t occupiedFineVoxelCount = 0u;
  uint32_t fineBrickCount = 0u;
  uint64_t peakTemporaryBrickBytes = 0u;
};

// Quantize a world height into the page's canonical signed fixed-point domain.
// Returns zero for an invalid page unit; callers should validate the page when
// invalid input must be distinguished from the representable value zero.
int64_t quantizeWorldMapTerrainHeight(
    double heightMetres, double heightUnitMetres);

bool validateWorldMapTerrainColumns(
    const WorldMapTerrainColumnPage &page,
    std::string *error = nullptr);

// CPU raster reference adapter. It consumes only canonical columns and never
// samples a map. Production generation uses the direct hierarchy adapter;
// this deliberately independent leaf expansion remains the differential-test
// oracle and emergency compatibility reference.
bool buildWorldMapTerrainColumnLeafPayload(
    const WorldMapTerrainColumnPage &page,
    WorldMapTerrainColumnLeafPayload &payload,
    std::string *error = nullptr);

bool buildWorldMapTerrainColumnHierarchy(
    const WorldMapTerrainColumnPage &page,
    uint32_t maximumBricks,
    WorldMapTerrainColumnHierarchyPayload &payload,
    std::string *error = nullptr);

struct WorldMapVoxelGenerationMetrics
{
  double mapSamplingMilliseconds = 0.0;
  double shellExtractionMilliseconds = 0.0;
  double hierarchyBuildMilliseconds = 0.0;
  uint64_t mapSampleCount = 0u;
  uint64_t emittedShellVoxelCount = 0u;
  uint64_t omittedInteriorVoxelCount = 0u;
  uint64_t boundaryStitchVoxelCount = 0u;
  uint64_t conservativeBoundarySurfaceVoxelCount = 0u;
  uint32_t maximumUnexposedDepthVoxels = 0u;
  uint32_t internalLOD = 0u;
  uint32_t horizontalVoxelCount = 0u;
  uint32_t verticalVoxelCapacity = 0u;
  uint32_t brickLevelCount = 0u;
  uint32_t brickNodeCount = 0u;
  uint32_t brickRootCount = 0u;
  uint64_t brickPayloadBytes = 0u;
  uint64_t surfacePayloadBytes = 0u;
  uint64_t contentRevision = 0u;
};

bool buildWorldMapVoxelTerrain(
    const procedural::world::MapExportBundle &bundle,
    const WorldMapVoxelTerrainSettings &settings,
    PlanetVoxelChunkSet &chunks,
    std::string *error = nullptr,
    const PlanetVoxelChunkProgress &progress = {},
    const std::atomic_bool *cancellation = nullptr,
    WorldMapVoxelGenerationMetrics *metrics = nullptr,
    WorldMapTerrainColumnPage *columnPage = nullptr);

// One non-overlapping generated-world quadtree leaf. X/Z are coordinates in
// the grid selected by spatialLOD; its world edge is chunkEdgeMetres * 2^LOD.
// The leaf cut covers the requested distance without allocating every finest
// 10 cm chunk in that area.
struct WorldVoxelChunkAddress
{
  int32_t x = 0;
  int32_t z = 0;
  uint32_t spatialLOD = 0u;
};

constexpr uint32_t kWorldVoxelMaximumResidentRadius = 8u;

bool operator==(
    const WorldVoxelChunkAddress &left,
    const WorldVoxelChunkAddress &right);

struct WorldVoxelChunkRequest
{
  uint32_t slot = 0xffffffffu;
  uint32_t expectedGeneration = 0u;
  uint32_t currentLOD = 0u;
  uint32_t desiredLOD = 0u;
  uint32_t priority = 0u;
};

enum class WorldVoxelPageState : uint32_t
{
  // Known to exist and locatable, with no payload anywhere. This is the state
  // the directory exists to express: today a page the streamer is not holding
  // is simply absent from every structure, so nothing can name it, order it
  // against its parent, or decide it is the next thing worth installing.
  Absent = 0u,
  // Payload recoverable without sampling the world maps again, from either the
  // uncompressed payload cache or the compressed transient store.
  Cached,
  // CPU page work is in flight. The entry has a stable address, parent and
  // payload key, but no payload can be installed yet.
  Generating,
  // Payload exists and has been selected for the current transaction, but the
  // renderer-visible cut has not committed it as Resident yet.
  Installing,
  // Installed in the GPU arena and part of the published cut.
  Resident,
};

struct TerrainPagePatchBatch
{
  uint64_t directoryGeneration = 0u;
  uint32_t pageIndex = kWorldVoxelInvalidPage;
  uint32_t residencyGeneration = 0u;
  std::vector<TerrainPagePatch> patches;
};

struct WorldVoxelPageDirectoryEntry
{
  WorldVoxelChunkAddress address{};
  uint32_t parent = kWorldVoxelInvalidPage;
  // The four quadrants one spatial level finer, as directory indices. A page
  // clipped by the world edge contributes fewer than four.
  std::array<uint32_t, 4u> children{
    kWorldVoxelInvalidPage, kWorldVoxelInvalidPage,
    kWorldVoxelInvalidPage, kWorldVoxelInvalidPage};
  uint32_t childCount = 0u;
  double minimumXMetres = 0.0;
  double minimumZMetres = 0.0;
  double edgeMetres = 0.0;
  // Coarsest deviation this page can represent, in world units. Until measured
  // simplification error exists this is the page voxel, which is exactly what
  // the raster cut metric already divides by distance. Monotonic by
  // construction: a parent's voxel is twice its child's.
  double errorEnvelopeMetres = 0.0;
  bool covered = false;
  bool frontier = false;
  float measuredErrorMetres = 0.0f;
  uint32_t payloadLOD = 0u;
  TransientSparseVoxelPageKey payloadKey{};
  uint64_t payloadRevision = 0u;
  WorldVoxelPageState state = WorldVoxelPageState::Absent;
  uint32_t residentSlot = kWorldVoxelInvalidPage;
  uint32_t residentLOD = 0u;
  // Non-zero only while Resident. Echoes the residency slot generation so a
  // directory entry cannot be confused with a replacement in the same slot.
  uint32_t residencyGeneration = 0u;
  bool streamingLeaf = false;
  bool childRangeEnabled = false;
  uint32_t residentFirstChildPage = kWorldVoxelInvalidPage;
  uint32_t residentChildCount = 0u;
  uint32_t installPatchOffset = 0u;
  uint32_t installPatchCount = 0u;
  uint32_t uninstallPatchOffset = 0u;
  uint32_t uninstallPatchCount = 0u;
};

// Every page the streamer may name this frame: the covered set, every ancestor
// up to a root, and one level of children below the covered set.
//
// The frontier is what makes dependency-ordered installs possible. A cut can
// only ever refine one level at a time, so a directory holding the cut, its
// ancestors and its immediate children can answer "what installs next, and what
// must already be installed for it to be legal" without any payload loaded.
// Materialising the whole quadtree instead is not an option - 16 km of 12.8 m
// pages is 6.25 million leaves.
struct WorldVoxelPageDirectory
{
  // Sorted coarsest first, then by x and z, so a linear walk is already a
  // legal install order and lookup is a binary search.
  std::vector<WorldVoxelPageDirectoryEntry> entries;
  std::vector<uint32_t> roots;
  std::vector<TerrainPagePatch> installPatches;
  std::vector<TerrainPagePatch> uninstallPatches;
  uint64_t patchGeneration = 0u;
  uint32_t coveredCount = 0u;
  uint32_t frontierCount = 0u;

  uint32_t find(const WorldVoxelChunkAddress &address) const;
};

struct WorldVoxelPageStreamingPlan
{
  // Non-resident pages whose payload must be restored from the streaming store
  // or generated. Ordered parent before child so a bounded worker can make
  // forward progress without rediscovering dependencies.
  std::vector<uint32_t> payloadRequests;
  // Cached pages whose parent dependency is already resident and whose install
  // patch batch can be prepared immediately.
  std::vector<uint32_t> readyInstalls;
  // Pages in the wanted closure that are not ready to install yet, either
  // because their payload is absent/generating or because an ancestor has not
  // been installed. This is scheduling state, not an error.
  std::vector<uint32_t> blockedInstalls;
  // Resident pages outside the target closure, ordered child before parent.
  std::vector<uint32_t> uninstalls;
};

enum class WorldVoxelChunkState : uint8_t
{
  Empty,
  CoarseQueued,
  CoarseResident,
  RefinementQueued,
  RefinementBuilding,
  PublicationPending,
  Resident,
  EvictPending,
};

struct WorldVoxelChunkEntry
{
  WorldVoxelChunkAddress address{};
  uint32_t slot = 0xffffffffu;
  uint32_t generation = 0u;
  uint32_t residentLOD = 0u;
  uint32_t requestedLOD = 0u;
  WorldVoxelChunkState state = WorldVoxelChunkState::Empty;
  // Shared and immutable once built: nothing writes into a payload after
  // creation, and the streaming job used to deep-copy the whole cache -
  // every resident payload - on the render thread at launch, 160-570 ms per
  // job. Copying a cache is now a pointer copy per entry.
  std::shared_ptr<const PlanetVoxelChunkSet> chunk =
      std::make_shared<const PlanetVoxelChunkSet>();
  struct PhysicalRange
  {
    uint32_t offset = 0u;
    uint32_t capacity = 0u;
  };
  struct PhysicalAllocation
  {
    uint64_t id = 0u;
    PhysicalRange nodes{};
    PhysicalRange children{};
    PhysicalRange roots{};
    PhysicalRange materials{};
    PhysicalRange palette{};
  } allocation{};
};

struct WorldVoxelPhysicalFreeRange
{
  uint32_t offset = 0u;
  uint32_t capacity = 0u;
};

struct WorldVoxelRetiredAllocation
{
  WorldVoxelChunkEntry::PhysicalAllocation allocation{};
  uint64_t lastReaderFrame = 0u;
  uint64_t releaseFrame = 0u;
};

struct WorldVoxelChunkStreamingSettings
{
  // Zero derives the fixed footprint from finestVoxelSizeMetres times
  // finestHorizontalVoxelCount.
  double chunkEdgeMetres = 0.0;
  double finestVoxelSizeMetres = 0.10;
  uint32_t finestHorizontalVoxelCount = 64u;
  uint32_t residentRadius = 1u;
  uint32_t lodCount = 1u;
  // A positive distance selects the non-overlapping spatial quadtree cut.
  // Zero retains the small fixed-grid mode used by focused unit fixtures.
  double renderDistanceMetres = 0.0;
  double projectionScalePixels = 0.0;
  uint32_t maximumSpatialLOD =
      kPlanetVoxelMaximumClipmapLevels - 1u;
  // Horizontal camera direction used only to order entering and replacement
  // work. A zero-length vector disables the visible-cone tier. Nearby chunks
  // always precede the cone, followed by visible and then off-screen chunks.
  double priorityViewDirectionX = 0.0;
  double priorityViewDirectionZ = -1.0;
  double priorityViewCosine = 0.0;
  uint32_t priorityNearChunkRadius = 1u;
  float splitErrorPixels = 4.0f;
  float mergeErrorPixels = 1.5f;
  // The generated-world hierarchy uses one controllable error metric for its
  // ordinary raster cut and asynchronous page requests:
  // projectedPixels / (1 + growth * distanceKilometres^exponent).
  // Zero growth restores the unmodified projected-pixel policy.
  float distanceErrorGrowthPerKilometre = 0.10f;
  float distanceErrorExponent = 1.0f;
  // A newly published LOD must survive this many rendered frames before it
  // can request another refinement. This bounds stationary feedback and gives
  // the split/merge dead band time to settle.
  uint32_t minimumResidentFrames = 4u;
  uint32_t maximumResidentChunks = 0u;
  uint32_t maximumGeneratedChunksPerUpdate = 8u;
  // GPU-owned payloads currently use one persistent page-build lane. Keep a
  // transaction small enough to publish one complete quadtree ownership group
  // instead of hiding a large serial batch until its last page completes.
  // Zero disables this latency cap.
  uint32_t maximumGPUOwnedPagesPerTransaction = 4u;
  // Plan generated pages as GPU-owned physical reservations. Map sampling,
  // column extraction, brick construction, and Tree64 construction are then
  // performed by the render-graph GPU chain. The CPU retains only the cut,
  // priority, allocation, eviction, and publication metadata.
  bool gpuOwnedPayloads = false;
  // Pack the per-voxel micro-surface words into the brick arena. Only the
  // brick draw and G-buffer passes read them; a route that draws terrain
  // through virtual geometry runs the brick culler for feedback alone, and
  // the words were the largest allocation in the process (one uint64 per
  // material entry: 1.52 GiB at capacity, held by the live arena, the
  // recycled arena and the GPU buffer).
  bool packMicroSurface = true;
  // Fixed inactive-page class used before GPU validation reports exact output
  // counts. The class deliberately bounds scratch/publication without reading
  // generated geometry back to the CPU. Zero selects the production defaults.
  uint32_t gpuPageNodeCapacity = 0u;
  uint32_t gpuPageChildCapacity = 0u;
  uint32_t gpuPageRootCapacity = 0u;
  uint32_t gpuPageMaterialCapacity = 0u;
  uint32_t gpuPagePaletteCapacity = 0u;
  // Opt-in generated-world path where the resident spatial cut advances from
  // page feedback instead of rebuilding the full metric cut every update.
  bool feedbackDrivenPageStreaming = false;
  // Keep the projected-error cut as an idle-queue safety net. Fresh GPU
  // feedback still preempts it, but a clipped or temporarily unobserved page
  // cannot make refinement report complete while coarser-than-requested
  // owners remain. Runtime GPU terrain enables this; focused feedback tests
  // may leave it disabled to exercise one request in isolation.
  bool feedbackDrivenFallbackConvergence = false;
  // Allows generated spatial terrain to select measured-error brick parents for
  // raster. Conservative OR occupancy remains in the hierarchy for Tree64 and
  // shadows; raster only accepts a parent when its measured heightfield error
  // passes the existing projected-pixel test.
  bool rasterSafeCoarseTerrain = false;
  // Bounds independently requested replacements in one asynchronous
  // transaction. Zero removes this refinement-specific cap while the total
  // generation budget above still applies.
  uint32_t maximumRefinedChunksPerUpdate = 1u;
  uint64_t maximumUploadBytesPerFrame = 0u;
  // Reader epoch used by deferred physical-page reclamation. Runtime updates
  // this before launching a worker. Tests may advance it deterministically.
  uint64_t readerFrame = 0u;
  uint32_t deferredReleaseFrames = 3u;
  // One uses the derived resident/detail/replacement arena. Larger values add
  // optional headroom without reserving finest capacity for every chunk.
  float virtualArenaScale = 1.0f;
  // Zero disables the optional deterministic CPU payload LRU.
  uint32_t maximumCachedPayloads = 0u;
  // A fine chunk must remain below the merge threshold or absent from visible
  // feedback for this many frames before it can be replaced by its parent.
  uint32_t coarseningGraceFrames = 30u;
  // Independent residency budgets. Zero keeps the derived/unbounded policy.
  uint64_t maximumCPUCacheBytes = 0u;
  uint64_t maximumGPUPageBytes = 0u;
  uint64_t maximumTree64Bytes = 0u;
  // Second-level page cache behind maximumCPUCacheBytes: LZ4 in RAM up to the
  // memory bound, LRU spill to an owned temporary directory up to the disk
  // bound, then dropped. Zero on both disables the store and leaves the
  // uncompressed payload cache as the only tier.
  uint64_t maximumTransientMemoryBytes = 0u;
  uint64_t maximumTransientDiskBytes = 0u;
  uint64_t generatorRevision = 12u;
  PlanetVoxelChunkSettings voxel{};
};

struct WorldVoxelChunkCachedPayload
{
  uint64_t bundleSignature = 0u;
  WorldVoxelChunkAddress address{};
  uint32_t lod = 0u;
  uint64_t generatorRevision = 0u;
  uint64_t lastUse = 0u;
  // Shared and immutable once built: nothing writes into a payload after
  // creation, and the streaming job used to deep-copy the whole cache -
  // every resident payload - on the render thread at launch, 160-570 ms per
  // job. Copying a cache is now a pointer copy per entry.
  std::shared_ptr<const PlanetVoxelChunkSet> chunk =
      std::make_shared<const PlanetVoxelChunkSet>();
};

enum class WorldVoxelPagePayloadLocation : uint8_t
{
  FrontCache,
  Installing,
  Resident,
};

struct WorldVoxelPagePayloadRecord
{
  WorldVoxelChunkAddress address{};
  uint32_t lod = 0u;
  TransientSparseVoxelPageKey payloadKey{};
  uint64_t payloadRevision = 0u;
  float measuredErrorMetres = 0.0f;
  WorldVoxelPagePayloadLocation location =
      WorldVoxelPagePayloadLocation::FrontCache;
};

enum class WorldVoxelPageResidencyLocation : uint8_t
{
  Installing,
  Resident,
};

struct WorldVoxelPageResidencyRecord
{
  WorldVoxelChunkAddress address{};
  uint32_t residentSlot = kWorldVoxelInvalidPage;
  uint32_t residentLOD = 0u;
  uint32_t residencyGeneration = 0u;
  uint64_t allocationId = 0u;
  WorldVoxelPageResidencyLocation location =
      WorldVoxelPageResidencyLocation::Resident;
};

struct WorldVoxelChunkCache
{
  uint64_t bundleSignature = 0u;
  WorldVoxelChunkStreamingSettings settings{};
  // Renderer-space Y is measured from one immutable elevation origin for the
  // cache lifetime. Recomputing this from an expanding resident cut moves all
  // retained raster and Tree64 pages relative to an unchanged camera.
  bool hasElevationOrigin = false;
  double elevationOriginMetres = 0.0;
  int32_t centerChunkX = 0;
  int32_t centerChunkZ = 0;
  std::vector<WorldVoxelChunkEntry> entries;
  // A replacement spatial cut is accumulated here while the preceding
  // complete cut remains renderer-visible. Parent/child ownership changes are
  // published only after every replacement leaf is ready.
  std::vector<WorldVoxelChunkEntry> stagedSpatialEntries;
  uint32_t slotCount = 0u;
  std::vector<uint32_t> slotGenerations;
  std::vector<uint32_t> slotNodeCapacity;
  std::vector<uint32_t> slotChildCapacity;
  std::vector<uint32_t> slotRootCapacity;
  std::vector<uint32_t> slotMaterialCapacity;
  std::vector<uint32_t> slotPaletteCapacity;
  bool virtualArenaInitialized = false;
  uint32_t arenaNodeCapacity = 0u;
  uint32_t arenaChildCapacity = 0u;
  uint32_t arenaRootCapacity = 0u;
  uint32_t arenaMaterialCapacity = 0u;
  // Whether the arena carries micro-surface words (see packMicroSurface).
  bool arenaMicroSurface = true;
  uint32_t arenaPaletteCapacity = 0u;
  std::vector<WorldVoxelPhysicalFreeRange> freeNodeRanges;
  std::vector<WorldVoxelPhysicalFreeRange> freeChildRanges;
  std::vector<WorldVoxelPhysicalFreeRange> freeRootRanges;
  std::vector<WorldVoxelPhysicalFreeRange> freeMaterialRanges;
  std::vector<WorldVoxelPhysicalFreeRange> freePaletteRanges;
  std::vector<WorldVoxelRetiredAllocation> retiredAllocations;
  uint64_t nextAllocationId = 1u;
  std::vector<WorldVoxelChunkCachedPayload> payloadCache;
  uint64_t payloadCacheClock = 0u;
  uint64_t payloadCacheBytes = 0u;
  // Compressed second-level page cache. A generated world page is a pure
  // function of the bundle, the generator revision and the page address, so a
  // page evicted from payloadCache above can be spilled here and restored
  // later instead of being sampled and voxelized again. Overflow simply drops
  // the oldest page; the generator remains the source of truth.
  std::shared_ptr<TransientSparseVoxelPageStore> transientPages;
  // Every page the streamer can name this frame, resident or not. Rebuilt from
  // the published cut at the end of each transaction.
  WorldVoxelPageDirectory pageDirectory;
  uint32_t pendingChunkCount = 0u;
  // An authored error-curve change must keep converging to its memoryless
  // target across bounded page batches. Treating an intermediate cut as LOD
  // history would let hysteresis cancel the rest of the settings change.
  bool spatialCutMetricConvergencePending = false;
  uint64_t layoutRevision = 0u;
};

std::vector<WorldVoxelPagePayloadRecord> collectWorldVoxelPagePayloadRecords(
    const WorldVoxelChunkCache &cache);
std::vector<WorldVoxelPageResidencyRecord>
collectWorldVoxelPageResidencyRecords(const WorldVoxelChunkCache &cache);

struct WorldVoxelChunkCacheUpdate
{
  // True only when the renderer-visible hierarchy cut changed.
  // A no-op feedback/update pass can skip raster and Tree64 publication.
  bool residentCutChanged = false;
  uint32_t reusedChunkCount = 0u;
  uint32_t builtChunkCount = 0u;
  uint32_t evictedChunkCount = 0u;
  uint32_t pendingChunkCount = 0u;
  uint32_t coarsestResidentLOD = 0u;
  uint32_t restoredCachedChunkCount = 0u;
  uint32_t refinedChunkCount = 0u;
  uint32_t coarsenedChunkCount = 0u;
  uint32_t rejectedStaleRequestCount = 0u;
  // Addresses the screen-error cut asked for that this update could not
  // source, and therefore published as a resident ancestor instead. Nonzero is
  // normal while a refinement wave is generating; permanently nonzero means
  // generation is not keeping up with the cut.
  uint32_t collapsedUnavailableChunkCount = 0u;
  // Pages this update took from the compressed store instead of generating.
  // Both are subsets of restoredCachedChunkCount's sibling path: a payloadCache
  // hit is counted there, a transient hit is counted here.
  uint32_t restoredTransientMemoryChunkCount = 0u;
  uint32_t restoredTransientDiskChunkCount = 0u;
  uint32_t spilledTransientChunkCount = 0u;
  // Fresh CPU-generated virtual pages written to the transient page source
  // immediately after generation, before any later RAM-cache eviction.
  uint32_t storedTransientChunkCount = 0u;
  // Directory shape after this transaction. `directoryAbsentCount` is the
  // interesting one: pages the streamer can name and order but holds no payload
  // for, which is the population an install decision chooses from.
  uint32_t directoryEntryCount = 0u;
  uint32_t directoryCoveredCount = 0u;
  uint32_t directoryFrontierCount = 0u;
  uint32_t directoryAbsentCount = 0u;
  uint32_t directoryPayloadRequestCount = 0u;
  uint32_t directoryReadyInstallCount = 0u;
  uint32_t directoryBlockedInstallCount = 0u;
  uint32_t directoryPlannedUninstallCount = 0u;
  // Ownership groups this transaction moved to their replacement pages, and
  // groups whose replacements are not all staged yet. A refinement that used to
  // publish once now publishes as `advanced` groups over several transactions.
  uint32_t advancedOwnershipGroupCount = 0u;
  uint32_t heldOwnershipGroupCount = 0u;
  // Refinement or merge groups this transaction planned, and the ones its
  // resident budget could not seat alongside the owners still drawing. A
  // deferred group is unchanged demand, not a failure: it is admitted by a
  // later transaction. A deferred count that never falls while the cut stops
  // advancing is the capacity stall.
  uint32_t admittedCapacityGroupCount = 0u;
  uint32_t deferredCapacityGroupCount = 0u;
  uint64_t cpuCacheBytes = 0u;
  uint64_t gpuPageBytes = 0u;
  uint64_t tree64BudgetBytes = 0u;
  uint32_t physicalUploadPageCount = 0u;
  uint64_t physicalUploadBytes = 0u;
  uint32_t hiddenPhysicalUploadPageCount = 0u;
  double mapSamplingMilliseconds = 0.0;
  double shellExtractionMilliseconds = 0.0;
  double hierarchyBuildMilliseconds = 0.0;
  uint64_t emittedShellVoxelCount = 0u;
  uint64_t omittedInteriorVoxelCount = 0u;
  uint32_t maximumUnexposedDepthVoxels = 0u;
  // Old complete owners retained until the caller either commits the GPU
  // transaction or rolls it back. Keeping them here avoids a deep copy of the
  // entire resident cache while refinement builds in scratch memory.
  std::vector<WorldVoxelChunkEntry> refinementRollbackEntries;
  std::vector<uint32_t> previousSlotGenerations;
  std::vector<uint32_t> previousSlotNodeCapacity;
  std::vector<uint32_t> previousSlotChildCapacity;
  std::vector<uint32_t> previousSlotRootCapacity;
  std::vector<uint32_t> previousSlotMaterialCapacity;
  std::vector<uint32_t> previousSlotPaletteCapacity;
  uint64_t previousLayoutRevision = 0u;
  std::vector<WorldVoxelPhysicalFreeRange> previousFreeNodeRanges;
  std::vector<WorldVoxelPhysicalFreeRange> previousFreeChildRanges;
  std::vector<WorldVoxelPhysicalFreeRange> previousFreeRootRanges;
  std::vector<WorldVoxelPhysicalFreeRange> previousFreeMaterialRanges;
  std::vector<WorldVoxelPhysicalFreeRange> previousFreePaletteRanges;
  std::vector<WorldVoxelRetiredAllocation> previousRetiredAllocations;
  uint64_t previousNextAllocationId = 1u;
  uint64_t previousPayloadCacheClock = 0u;
  bool physicalAllocatorSnapshot = false;
  bool previousHasElevationOrigin = false;
  double previousElevationOriginMetres = 0.0;
  int32_t previousCenterChunkX = 0;
  int32_t previousCenterChunkZ = 0;
  uint32_t previousPendingChunkCount = 0u;
  WorldVoxelChunkStreamingSettings previousSettings{};
  bool previousSpatialCutMetricConvergencePending = false;
  std::vector<WorldVoxelChunkEntry> previousStagedSpatialEntries;
  std::vector<uint64_t> newAllocationIds;
  std::vector<WorldVoxelChunkEntry> evictedRollbackEntries;
  std::vector<WorldVoxelChunkCachedPayload> removedPayloadRollbackEntries;
  std::vector<uint64_t> addedPayloadLastUses;
};

// Radius required for the next coarser internal chunk LOD to remain below the
// requested projected error at the edge of the resident window.
uint32_t worldVoxelChunkRadiusForProjection(
    uint32_t viewportHeight,
    float verticalFovRadians,
    uint32_t horizontalVoxelCount,
    float transitionPixelThreshold,
    uint32_t maximumRadius = 8u);

// Rejects stale slot generations and folds duplicate GPU feedback into one
// finest-LOD/highest-priority request per resident owner. Phase 1 exposes the
// policy for tests; Phase 3 connects it to shader feedback.
std::vector<WorldVoxelChunkRequest> coalesceWorldVoxelChunkRequests(
    const WorldVoxelChunkCache &cache,
    const std::vector<WorldVoxelChunkRequest> &requests);

// Camera-aware backlog for GPU terrain feedback. Updating the queue
// revalidates slot generations and requested LODs, then supersedes queued heap
// entries with priorities derived from the current view. Stale heap entries
// are discarded lazily when popped and compacted periodically.
class WorldVoxelChunkRequestQueue
{
public:
  WorldVoxelChunkRequestQueue();
  ~WorldVoxelChunkRequestQueue();

  WorldVoxelChunkRequestQueue(WorldVoxelChunkRequestQueue &&) noexcept;
  WorldVoxelChunkRequestQueue &operator=(
      WorldVoxelChunkRequestQueue &&) noexcept;
  WorldVoxelChunkRequestQueue(const WorldVoxelChunkRequestQueue &) = delete;
  WorldVoxelChunkRequestQueue &operator=(
      const WorldVoxelChunkRequestQueue &) = delete;

  void clear();
  bool empty() const;
  size_t size() const;

  // Holds feedback until a resident cache is available for validation. This
  // is used while the cache is temporarily owned by an asynchronous worker.
  void enqueue(
      const std::vector<WorldVoxelChunkRequest> &observations);

  // Replaces all demand which has not yet been admitted with one completed
  // culling frame. Resident pages and generation already submitted to the GPU
  // are not affected. A page which remains visible will be observed again by
  // the next culling pass instead of retaining stale priority in this queue.
  void replaceFrame(
      const std::vector<WorldVoxelChunkRequest> &observations);

  // Camera coordinates are in map space, matching chunk-address coordinates.
  // New observations are merged with requests already waiting in the queue.
  // Runtime frame feedback should normally enter through replaceFrame(); the
  // merging form remains useful for synthetic camera-owner demand and for
  // callers which intentionally construct a request set incrementally.
  void update(
      const WorldVoxelChunkCache &cache,
      double cameraMapXMetres,
      double cameraMapZMetres,
      const WorldVoxelChunkStreamingSettings &settings,
      const std::vector<WorldVoxelChunkRequest> &observations = {});

  // Revalidates and reprioritizes immediately before removing a batch.
  std::vector<WorldVoxelChunkRequest> take(
      const WorldVoxelChunkCache &cache,
      double cameraMapXMetres,
      double cameraMapZMetres,
      const WorldVoxelChunkStreamingSettings &settings,
      size_t maximumCount);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Deviation already measured for pages the streamer has built, in world units,
// keyed by page address. Pages absent from the map fall back to the geometric
// estimate the cut has always used.
using WorldVoxelMeasuredPageErrors =
    std::vector<std::pair<WorldVoxelChunkAddress, float>>;

// Looks up a measured page error. Linear, because the caller holds at most the
// resident cut and the lookup happens once per visited quadtree node.
float worldVoxelMeasuredPageError(
    const WorldVoxelMeasuredPageErrors &measured,
    const WorldVoxelChunkAddress &address);

std::vector<WorldVoxelChunkAddress> buildWorldVoxelChunkLeafCut(
    double worldExtentMetres,
    double cameraWorldXMetres,
    double cameraWorldZMetres,
    const WorldVoxelChunkStreamingSettings &settings,
    // Optional. Where a page's deviation is known, the split test uses it in
    // place of the page voxel: ground that lost little to coarsening stops
    // subdividing once it is distant, and a cliff keeps refining. The
    // measured-error floor still forces the page intersecting a near-field
    // camera to LOD 0. Without measurements the cut uses the geometric error.
    const WorldVoxelMeasuredPageErrors *measuredErrors = nullptr,
    // Optional previously published cut. Pages that were already subdivided
    // remain subdivided until their error crosses mergeErrorPixels; pages that
    // were coarse still require splitErrorPixels.
    const std::vector<WorldVoxelChunkAddress> *stableCut = nullptr);

// Cache identity of one generated-world page. Regenerating a page is the
// fallback for every miss, so this key has to fold in everything that changes
// the bytes the generator produces. A missing input means a stale page is
// restored under a key that no longer describes it - a defect that only appears
// after an eviction, in whatever region the camera happened to leave.
TransientSparseVoxelPageKey worldTransientPageKey(
    uint64_t bundleIdentity,
    const WorldVoxelChunkStreamingSettings &settings,
    const WorldVoxelChunkAddress &address,
    uint32_t internalLOD);

// Builds the directory covering `covered` - normally the published leaf cut.
// Ancestors are closed upward to the root LOD implied by the render distance,
// and one level of children is added below each covered page. Pages entirely
// outside the world are omitted; nothing else is.
bool buildWorldVoxelPageDirectory(
    double worldExtentMetres,
    const WorldVoxelChunkStreamingSettings &settings,
    const std::vector<WorldVoxelChunkAddress> &covered,
    WorldVoxelPageDirectory &directory,
    std::string *error = nullptr);

// Marks directory entries from the streamer's own residency and caches. Pages
// the directory knows about but nothing holds stay Absent, which is the point:
// they remain nameable and orderable while carrying no payload.
void annotateWorldVoxelPageDirectory(
    const WorldVoxelChunkCache &cache,
    WorldVoxelPageDirectory &directory);

// Assigns the deterministic page-source key every directory entry would use to
// restore or generate its payload. This is separate from topology construction
// because cache identity also depends on the current bundle.
void assignWorldVoxelPageDirectoryPayloadKeys(
    uint64_t bundleIdentity,
    const WorldVoxelChunkStreamingSettings &settings,
    uint32_t internalLOD,
    WorldVoxelPageDirectory &directory);

// Rebuilds topology while carrying forward page-source state for addresses
// that remain nameable. This is the compatibility bridge from a derived
// directory to an authoritative page table.
void preserveWorldVoxelPageDirectoryState(
    const WorldVoxelPageDirectory &previous,
    WorldVoxelPageDirectory &directory);

// Builds the per-page install/uninstall patch ranges from directory topology.
// These patches are CPU-oracle deltas for Phase 4: Phase 5 will translate the
// same semantics to physical GPU ranges.
bool buildWorldVoxelPageDirectoryPatchLists(
    WorldVoxelPageDirectory &directory,
    std::string *error = nullptr);

bool prepareWorldVoxelPageInstall(
    const WorldVoxelPageDirectory &directory,
    uint32_t pageIndex,
    uint32_t residencyGeneration,
    TerrainPagePatchBatch &batch,
    std::string *error = nullptr);
bool commitWorldVoxelPageInstall(
    WorldVoxelPageDirectory &directory,
    const TerrainPagePatchBatch &batch,
    std::string *error = nullptr);
bool prepareWorldVoxelPageUninstall(
    const WorldVoxelPageDirectory &directory,
    uint32_t pageIndex,
    TerrainPagePatchBatch &batch,
    std::string *error = nullptr);
bool commitWorldVoxelPageUninstall(
    WorldVoxelPageDirectory &directory,
    const TerrainPagePatchBatch &batch,
    std::string *error = nullptr);

std::vector<uint32_t> selectedWorldVoxelPageCut(
    const WorldVoxelPageDirectory &directory);
bool validateWorldVoxelPageResidencyOracle(
    const WorldVoxelPageDirectory &directory,
    std::string *error = nullptr);
std::vector<uint32_t> planWorldVoxelPageInstalls(
    const WorldVoxelPageDirectory &directory,
    const std::vector<WorldVoxelChunkAddress> &targetCut);
std::vector<uint32_t> planWorldVoxelPageUninstalls(
    const WorldVoxelPageDirectory &directory,
    const std::vector<WorldVoxelChunkAddress> &targetCut);
WorldVoxelPageStreamingPlan planWorldVoxelPageStreaming(
    const WorldVoxelPageDirectory &directory,
    const std::vector<WorldVoxelChunkAddress> &targetCut);
WorldVoxelTerrainRuntimePageTableGPU buildWorldVoxelTerrainRuntimePageTable(
    const WorldVoxelPageDirectory &directory);
WorldVoxelTerrainRuntimePageTableGPU buildWorldVoxelTerrainRuntimePageTable(
    const WorldVoxelPageDirectory &directory,
    const WorldVoxelChunkCache &cache);
bool validateWorldVoxelTerrainRuntimePageTable(
    const WorldVoxelPageDirectory &directory,
    const WorldVoxelTerrainRuntimePageTableGPU &table,
    std::string *error = nullptr);
bool buildWorldVoxelTerrainRuntimeVisibleBrickRoots(
    const WorldVoxelTerrainRuntimePageTableGPU &table,
    const SparseVoxelBrickHierarchy &bricks,
    std::vector<uint32_t> &roots,
    std::string *error = nullptr);
// Rebuilds the flat raster seed prefix from the runtime page table's resident
// streaming leaves. Page payloads keep their physical root ranges; only this
// compact prefix and the visible records are rewritten.
bool compactWorldVoxelTerrainRuntimeVisibleBrickRoots(
    WorldVoxelTerrainRuntimePageTableGPU &table,
    std::vector<PlanetVoxelChunkRecord> &records,
    SparseVoxelBrickHierarchy &bricks,
    std::string *error = nullptr);

// The finest cut that can be published right now.
//
// A spatial cut is a partition, so a page and its descendants must never be
// visible together and no area may be left without an owner. The published cut
// therefore only ever moves in whole ownership groups: one published page
// together with all the requested pages that tile it, or one requested page
// together with all the published pages it absorbs. A group is taken when every
// requested page in it is staged, and left alone otherwise.
//
// Advancing group by group is what makes refinement incremental. Requiring the
// entire replacement cut to be staged instead leaves the bootstrap visible until
// the last page of a 16 km world is built, which reads as terrain that sits on
// one coarse LOD and then jumps.
//
// The result is closed under the same 2:1 face-neighbour balance the requested
// cut is built with: a group whose commit would leave a neighbour more than one
// level away is held back, because a larger step reads as the dark strips seen
// at LOD boundaries.
// Limits a requested cut to one spatial level finer than the published one.
//
// The requested cut is derived straight from the error metric, so on a fresh
// world it is nine levels below the bootstrap owners. Chasing it in one step
// makes every ownership group span a quarter of the world, and a group that
// large only ever completes at the very end - which is the coarse-then-jump
// behaviour, expressed as a group granularity problem.
//
// Clamping makes each transaction subdivide by exactly one level, so groups are
// one page and its four children. The cut then deepens continuously until it
// meets the error metric, and every transaction publishes visible progress.
std::vector<WorldVoxelChunkAddress> clampWorldVoxelCutToOneLevel(
    const std::vector<WorldVoxelChunkAddress> &published,
    const std::vector<WorldVoxelChunkAddress> &requested);

// Limits how many spatial refinement levels one complete publication may
// advance. The returned cut remains disjoint and 2:1 face balanced; if the
// requested depth cannot preserve those properties it falls back to one level.
std::vector<WorldVoxelChunkAddress> clampWorldVoxelCutToRefinementLevels(
    const std::vector<WorldVoxelChunkAddress> &published,
    const std::vector<WorldVoxelChunkAddress> &requested,
    uint32_t maximumRefinementLevels);

std::vector<WorldVoxelChunkAddress> selectPublishableWorldVoxelCut(
    const std::vector<WorldVoxelChunkAddress> &published,
    const std::vector<WorldVoxelChunkAddress> &requested,
    const std::vector<WorldVoxelChunkAddress> &staged,
    double baseEdgeMetres,
    uint32_t *advancedGroupCount = nullptr,
    uint32_t *heldGroupCount = nullptr);

// Admit whole ownership groups while published and staged pages retain their
// slots. Requested order is priority order; a deferred group keeps its owner
// drawing and is admitted by a later transaction rather than failing this one.
// This is a planning filter, not a publication gate: the pages it admits still
// have to be generated and staged before `selectPublishableWorldVoxelCut`
// will put them on screen.
std::vector<WorldVoxelChunkAddress> limitWorldVoxelCutToCapacity(
    const std::vector<WorldVoxelChunkAddress> &published,
    const std::vector<WorldVoxelChunkAddress> &requested,
    const std::vector<WorldVoxelChunkAddress> &staged,
    double baseEdgeMetres,
    uint32_t slotCapacity,
    uint32_t *admittedGroupCount = nullptr,
    uint32_t *deferredGroupCount = nullptr);

// Maintains a bounded non-overlapping generated-world quadtree leaf cut (or a
// fixed grid when renderDistanceMetres is zero for deterministic fixtures).
// Spatial owner replacements are published only as complete cuts.
bool updateWorldVoxelChunkCache(
    const procedural::world::MapExportBundle &bundle,
    double cameraWorldXMetres,
    double cameraWorldZMetres,
    const WorldVoxelChunkStreamingSettings &settings,
    WorldVoxelChunkCache &cache,
    PlanetVoxelChunkSet &chunks,
    WorldVoxelChunkCacheUpdate *update = nullptr,
    std::string *error = nullptr,
    const PlanetVoxelChunkProgress &progress = {},
    const std::atomic_bool *cancellation = nullptr,
    const std::vector<WorldVoxelChunkRequest> *refinementRequests = nullptr,
    // Reproduce an exact spatial cut instead of deriving one from the camera.
    // A render-graph rebuild starts from an empty cache so every owner emits a
    // physical upload into the fresh arenas; without this it also throws away
    // the refined cut and restarts from the coarse bootstrap one, which is a
    // livelock whenever something rebuilds the graph regularly.
    const std::vector<WorldVoxelChunkAddress> *forcedSpatialCut = nullptr,
    // Records of the cut that last published into the arena mirror `chunks`
    // carries in. A page whose ranges and payload revision both appear there is
    // already written at those offsets, so its translation is skipped: the
    // arena is not zeroed between transactions, and re-translating every
    // resident page to publish a handful of new ones was 1300 ms of a 2300 ms
    // transaction with 210 pages resident.
    const std::vector<PlanetVoxelChunkRecord> *arenaResidentRecords = nullptr);

// Restores the complete owners retained by a successful refinement update.
// Used when its prepared GPU transaction becomes stale or is rejected before
// publication. It is a no-op when the update contained no refinements.
void rollbackWorldVoxelChunkCacheUpdate(
    WorldVoxelChunkCache &cache,
    WorldVoxelChunkCacheUpdate &update);

// Validates that every live and deferred allocation lies inside its arena and
// that no two allocations or free ranges overlap.
bool validateWorldVoxelChunkPhysicalPages(
    const WorldVoxelChunkCache &cache,
    std::string *error = nullptr);

// Arms allocations retired by one or more prepared CPU updates only after the
// final raster/Tree64 publication has completed. Until then their release
// epoch remains infinite, so later workers cannot recycle storage still read
// by the currently displayed terrain cut.
void markWorldVoxelChunkPhysicalPagesPublished(
    WorldVoxelChunkCache &cache,
    uint64_t readerFrame,
    uint32_t deferredReleaseFrames);

} // namespace voxel::svraster
