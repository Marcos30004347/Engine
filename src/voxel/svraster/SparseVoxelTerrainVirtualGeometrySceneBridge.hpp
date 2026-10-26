#pragma once

#include "voxel/svraster/SparseVoxelTerrainVirtualGeometryGPUBuild.hpp"

#include "virtualgeometry/VirtualGeometryGeneratedPayloadManager.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"

#include <array>
#include <functional>
#include <limits>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

struct SparseVoxelTerrainVGSceneInstallSettings
{
  uint32_t localPageIndex = UINT32_MAX;
  uint32_t pageTableOffset = 0u;
  uint32_t prioritySlot = UINT32_MAX;
  uint32_t unitScaleBits = 0u;
  uint32_t packedSlotStrideBytes = kSparseVoxelPackedPageSlotBytes;
  bool requireTracePayload = true;
  bool requireMaterialPayload = false;
};

struct SparseVoxelTerrainVGScenePageInstall
{
  SparseVoxelPageId page{};
  uint64_t pageGeneration = 0u;
  uint64_t geometryRevision = 0u;
  uint64_t materialRevision = 0u;
  uint32_t traceFormat = kVoxelTraceFormatInvalid;
  uint32_t localPageIndex = UINT32_MAX;
  uint32_t globalPageTableIndex = UINT32_MAX;
  uint32_t prioritySlot = UINT32_MAX;
  uint32_t pageSlot = kSparseVoxelInvalidPackedPageSlot;
  uint32_t pagelet = kSparseVoxelInvalidPackedPagelet;
  bool readyForSceneInstall = false;
  bool hasTracePayload = false;
  bool hasMaterialPayload = false;
  bool sceneVisibleRasterPayloadReady = false;
  bool enqueueScenePageDecode = true;

  SparseVoxelPhysicalPageAllocation sharedAllocation{};
  virtualgeometry::PageAllocation virtualPageAllocation{};
  virtualgeometry::PageTableEntry pageTableEntry{};
  std::vector<uint32_t> encodedPageWords{};
};

bool buildSparseVoxelTerrainVGScenePageInstall(
    const SparseVoxelSharedPageTable &table,
    const SparseVoxelTerrainVGPUPageBuildResult &build,
    const SparseVoxelTerrainVGSceneInstallSettings &settings,
    SparseVoxelTerrainVGScenePageInstall &install,
    std::string *error = nullptr);

bool registerSparseVoxelTerrainVGRuntimeObject(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    uint32_t pageCount,
    const std::vector<virtualgeometry::VirtualGeometryHierarchy> &hierarchy,
    const virtualgeometry::QuantizationConfig &quantizationConfig,
    virtualgeometry::VirtualGeometryScene::RuntimeObjectId *outObjectId = nullptr,
    std::string *error = nullptr);

bool registerSparseVoxelTerrainVGRuntimeObjectWithPlacement(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    uint32_t pageCount,
    const std::vector<virtualgeometry::VirtualGeometryHierarchy> &hierarchy,
    const virtualgeometry::QuantizationConfig &quantizationConfig,
    const virtualgeometry::AABB &localBounds,
    const math::Vec3f &position,
    virtualgeometry::VirtualGeometryScene::RuntimeObjectId *outObjectId = nullptr,
    std::string *error = nullptr);

bool buildSparseVoxelTerrainVGGeneratedPayloadRequest(
    const SparseVoxelTerrainVGPUPageBuildResult &build,
    const std::string &objectName,
    uint32_t localPageIndex,
    uint32_t parentLocalPageIndex,
    uint32_t fallbackLocalPageIndex,
    uint32_t unitScaleBits,
    uint32_t priority,
    virtualgeometry::VirtualGeometryGeneratedPayloadManager::Request &request,
    std::string *error = nullptr);

bool reserveSparseVoxelTerrainVGGeneratedPayload(
    virtualgeometry::VirtualGeometryGeneratedPayloadManager &manager,
    const SparseVoxelTerrainVGPUPageBuildResult &build,
    const std::string &objectName,
    uint32_t localPageIndex,
    uint32_t parentLocalPageIndex,
    uint32_t fallbackLocalPageIndex,
    uint32_t unitScaleBits,
    uint32_t priority,
    virtualgeometry::VirtualGeometryGeneratedPayloadManager::GpuWriteDescriptor &descriptor,
    std::string *error = nullptr);

bool publishSparseVoxelTerrainVGGeneratedDecodedPayload(
    virtualgeometry::VirtualGeometryGeneratedPayloadManager &manager,
    virtualgeometry::VirtualGeometryGeneratedPayloadManager::TicketId ticketId,
    std::string *error = nullptr);

bool installSparseVoxelTerrainVGScenePage(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const SparseVoxelTerrainVGScenePageInstall &install,
    bool rootPage,
    uint32_t parentLocalPageIndex = UINT32_MAX,
    uint32_t fallbackLocalPageIndex = UINT32_MAX,
    std::string *error = nullptr);

bool attachSparseVoxelTerrainVGSceneEncodedPage(
    SparseVoxelTerrainVGScenePageInstall &install,
    const virtualgeometry::VirtualGeometryPage &page,
    std::string *error = nullptr);

struct SparseVoxelTerrainVGEncodedAssetRuntimeInstallSettings
{
  uint64_t basePageGeneration = 1u;
  uint64_t geometryRevision = 1u;
  uint64_t materialRevision = 1u;
  math::Vec3f position{0.0f, 0.0f, 0.0f};
  uint32_t materialIndex = 0u;
  bool hasTracePayload = true;
  bool hasMaterialPayload = false;
  bool enqueueScenePageDecode = true;
  // `max_parent_lod_error` written into the chunk root: the finest cluster
  // error of the installed parent chunk, so the culler visits this chunk only
  // where the parent's finest clusters are too coarse. Infinite (the default)
  // when no parent is installed: nothing coarser draws here.
  float rootParentError = std::numeric_limits<float>::max();
  // Install every page of the asset at once (the default until now) or only
  // its root page, leaving the finer pages for the culler to request. At 342
  // resident chunks 1,250 pages were installed and the culler touched 39 per
  // frame: far chunks and the coarse pages of near ones were meshed, encoded,
  // uploaded and decoded for nothing.
  bool installAllPages = true;
};

struct SparseVoxelTerrainVGEncodedAssetRuntimeInstallStats
{
  virtualgeometry::VirtualGeometryScene::RuntimeObjectId objectId =
      virtualgeometry::VirtualGeometryScene::INVALID_RUNTIME_OBJECT_ID;
  uint32_t installedPageCount = 0u;
  uint32_t queuedDecodePageCount = 0u;
  uint32_t encodedMeshletCount = 0u;
  uint64_t encodedPageBytes = 0u;
};

bool installSparseVoxelTerrainVGEncodedAssetRuntimeObject(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const virtualgeometry::VirtualGeometryEncodedData &asset,
    const SparseVoxelTerrainVGEncodedAssetRuntimeInstallSettings &settings,
    SparseVoxelTerrainVGEncodedAssetRuntimeInstallStats *stats = nullptr,
    std::string *error = nullptr);

bool sparseVoxelTerrainVGSceneInstallHasDrawablePayload(
    const SparseVoxelTerrainVGScenePageInstall &install);

// ---------------------------------------------------------------------------
// One runtime object holding many CPU-meshed chunks.
// See docs/terrain-vg-unified-hierarchy-plan.md, phases 2 and 3.
//
// A chunk arrives as a virtual-geometry asset with its own DAG - measured at 49
// hierarchy nodes for one page - so a shared object cannot use the flat
// one-leaf-per-slot hierarchy the GPU-generated route uses. Each slot instead
// owns a contiguous slice of the object's hierarchy, and a chunk's nodes are
// copied into it with every index shifted.
//
// Layout, for `capacity` slots and `maxChunkNodes` nodes per chunk:
//
//   node 0                      shared root, children [1, 1+capacity)
//   node 1 + slot               that slot's chunk root (an inert stub when the
//                               slot holds nothing)
//   node 1 + capacity
//        + slot*(maxChunkNodes-1)  the chunk's remaining nodes, in order
//
// The root's children have to be one contiguous run, which is why the chunk
// roots are gathered in front rather than each sitting at the head of its own
// slice.
struct SparseVoxelTerrainVGSharedObjectLayout
{
  uint32_t capacity = 0u;
  uint32_t maxChunkNodes = 0u;

  uint32_t hierarchyNodeCount() const
  {
    return 1u + capacity + capacity * (maxChunkNodes - 1u);
  }
  // Object-local index of chunk hierarchy node `chunkNode` for `slot`.
  uint32_t nodeIndex(uint32_t slot, uint32_t chunkNode) const
  {
    return chunkNode == 0u
        ? 1u + slot
        : 1u + capacity + slot * (maxChunkNodes - 1u) + (chunkNode - 1u);
  }
  bool valid() const { return capacity != 0u && maxChunkNodes >= 2u; }
};

// Builds the shared hierarchy: a force-traversal root over `capacity` inert
// chunk-root stubs, and empty space for their slices.
bool buildSparseVoxelTerrainVGSharedObjectHierarchy(
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    const virtualgeometry::AABB &bounds,
    std::vector<virtualgeometry::VirtualGeometryHierarchy> &hierarchy,
    std::string *error = nullptr);

// Remaps a chunk's hierarchy into `slot`'s slice: child runs shifted, page
// indices mapped through `pageSlots` (asset page i lives in page slot
// pageSlots[i]), and the chunk root written to `1 + slot`.
bool remapSparseVoxelTerrainVGChunkHierarchy(
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    uint32_t slot,
    const std::vector<virtualgeometry::VirtualGeometryHierarchy> &chunkHierarchy,
    const std::vector<uint32_t> &pageSlots,
    std::vector<virtualgeometry::VirtualGeometryHierarchy> &chunkRoot,
    std::vector<virtualgeometry::VirtualGeometryHierarchy> &slice,
    std::string *error = nullptr);

// Remaps a page's install rewrites into `slot`'s slice.
std::vector<virtualgeometry::HierarchyClusterFlagsUpdate>
remapSparseVoxelTerrainVGChunkRewrites(
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    uint32_t slot,
    const std::vector<virtualgeometry::HierarchyClusterFlagsUpdate> &updates);

// Writes the chunk's hierarchy into its slice and installs every page of the
// asset into the page slots `pageSlots` names. All or nothing.
bool installSparseVoxelTerrainVGChunkIntoSlot(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    uint32_t slot,
    const std::vector<uint32_t> &pageSlots,
    const virtualgeometry::VirtualGeometryEncodedData &asset,
    const SparseVoxelTerrainVGEncodedAssetRuntimeInstallSettings &settings,
    std::string *error = nullptr,
    virtualgeometry::VirtualGeometryHierarchy *chunkRootOut = nullptr);

// One page's encoded bytes, however they were obtained: from a chunk asset in
// memory, or from the phase 8 scratch file. Everything the scene needs to
// install a page is here, so the install path does not care which.
struct SparseVoxelTerrainVGChunkPageBytes
{
  // `PageBuffer::encode` output for the page, or null when `location` says
  // where the scene can read it itself (phase 8g). A valid location is
  // recorded on the page either way, so the scene can bring the page back
  // after evicting it without asking anyone.
  const std::vector<uint32_t> *encodedWords = nullptr;
  virtualgeometry::VirtualGeometryPageLocation location{};
  // Asset-local page indices; mapped through `pageSlots` by the install.
  const std::vector<uint32_t> *dependencies = nullptr;
  // Without these every cluster on the page is a streaming leaf, its self
  // error is forced to zero, and every LOD level draws at once.
  const std::vector<virtualgeometry::HierarchyClusterFlagsUpdate> *installUpdates = nullptr;
};

// Installs one page from its encoded bytes. `rootPageIndex` and
// `unitScaleBits` come from the chunk, not the page: the first decides whether
// this is the chunk's root page, the second is the scale the decoder
// reconstructs vertex positions with (a zero collapses every vertex to the
// origin, and the object still reports every page traceable).
bool installSparseVoxelTerrainVGChunkPageBytes(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    uint32_t slot,
    const std::vector<uint32_t> &pageSlots,
    const SparseVoxelTerrainVGChunkPageBytes &page,
    uint32_t pageIndex,
    uint32_t rootPageIndex,
    uint32_t unitScaleBits,
    const SparseVoxelTerrainVGEncodedAssetRuntimeInstallSettings &settings,
    std::string *error = nullptr);

// Phase 8g: records a page's location and install parameters on a page that
// is not installed, so the scene can install it itself when the culler asks
// for it. Same inputs as the install; `page.location` must be valid.
bool primeSparseVoxelTerrainVGChunkPageSource(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    uint32_t slot,
    const std::vector<uint32_t> &pageSlots,
    const SparseVoxelTerrainVGChunkPageBytes &page,
    uint32_t pageIndex,
    uint32_t rootPageIndex,
    uint32_t unitScaleBits,
    const SparseVoxelTerrainVGEncodedAssetRuntimeInstallSettings &settings,
    std::string *error = nullptr);

// Installs a chunk whose geometry is not resident: the hierarchy and the chunk
// metadata are given directly, and each page's bytes are fetched on demand
// through `fetchPage`, which is called for the pages this install writes and,
// after them, once per page it leaves for the culler - those are primed with
// their locations (phase 8g) when the fetch yields one, so the scene serves
// their requests itself. The hierarchy-before-pages order (see the asset
// overload) is kept here, so a caller never has to reproduce it.
bool installSparseVoxelTerrainVGChunkIntoSlotFromBytes(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    uint32_t slot,
    const std::vector<uint32_t> &pageSlots,
    const std::vector<virtualgeometry::VirtualGeometryHierarchy> &chunkHierarchy,
    uint32_t pageCount,
    uint32_t rootPageIndex,
    uint32_t unitScaleBits,
    const std::function<bool(uint32_t, SparseVoxelTerrainVGChunkPageBytes &, std::string *)> &fetchPage,
    const SparseVoxelTerrainVGEncodedAssetRuntimeInstallSettings &settings,
    std::string *error = nullptr,
    virtualgeometry::VirtualGeometryHierarchy *chunkRootOut = nullptr);

// Installs one page of an already installed chunk: the culler requested it
// (its leaf carried PAGE_NOT_INSTALLED_BIT) and the page cache still holds the
// asset. `settings` must repeat the chunk install's generation and revision.
bool installSparseVoxelTerrainVGChunkPage(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    uint32_t slot,
    const std::vector<uint32_t> &pageSlots,
    const virtualgeometry::VirtualGeometryEncodedData &asset,
    uint32_t pageIndex,
    const SparseVoxelTerrainVGEncodedAssetRuntimeInstallSettings &settings,
    std::string *error = nullptr);

// Rewrites an installed chunk's root with a new `max_parent_lod_error`:
// `chunkRoot` is the node the install handed back, everything else in it is
// kept. Used when the parent chunk arrives, changes or leaves.
bool setSparseVoxelTerrainVGChunkRootParentError(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    uint32_t slot,
    virtualgeometry::VirtualGeometryHierarchy &chunkRoot,
    float parentError,
    std::string *error = nullptr);

// ---------------------------------------------------------------------------
// Phase 3: a coarse chunk stops being the streaming boundary where its children
// have arrived.
//
// A cluster is a *streaming leaf* when the finer geometry it stands in for is
// not resident: the culler may draw it but may not refine past it. While a
// coarse chunk's four children are absent, every one of its clusters is such a
// boundary. As each child lands, the coarse clusters covering that child's
// quadrant must stop being boundaries, or the cut can never descend into the
// child and the terrain sits one level too coarse forever.
//
// Coverage is decided per hierarchy node, not per cluster, because cluster
// bounds live in the encoded page while node bounds are right here - and it
// costs nothing in practice: a chunk's octree splits on the same axes as the
// quadrants, so below the chunk root every node lies wholly inside one
// quadrant. A node that does straddle simply waits for all the quadrants it
// touches, which is the conservative direction: never clear a boundary whose
// geometry is not fully resident.
//
// This does not coarsen LOD granularity. Which cluster to draw is still decided
// per cluster by the culler's self/parent error test; these bits only say how
// far it is allowed to descend.
//
// Quadrant bits match TerrainGPUVirtualGeometryPageSet::quadrantOf:
// bit 0 is the +x half, bit 1 the +z half.
std::vector<uint8_t> sparseVoxelTerrainVGChunkNodeQuadrants(
    const std::vector<virtualgeometry::VirtualGeometryHierarchy> &chunkHierarchy,
    float centerX,
    float centerZ);

// The chunk's own install rewrites, adjusted for which of its children are
// resident, and remapped into `slot`'s slice. `residentChildMask` is the same
// four-bit mask the page set maintains.
std::vector<virtualgeometry::HierarchyClusterFlagsUpdate>
sparseVoxelTerrainVGChunkRewritesForResidentChildren(
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    uint32_t slot,
    const std::vector<virtualgeometry::HierarchyClusterFlagsUpdate> &baseUpdates,
    const std::vector<uint8_t> &nodeQuadrants,
    uint32_t residentChildMask,
    bool replaceCoveredClusters = false);

// Per-cluster quadrant coverage, one entry per hierarchy node, eight masks per
// entry - `streamingLeafsBitset` bit i is cluster i of that node's group, so the
// cross-chunk boundary can be decided per cluster instead of per node.
//
// The node-level test above has to be conservative: a node whose box straddles
// the centre reaches several quadrants, so it waits for all of them, and when
// it does clear it clears every cluster it holds. A cluster's *block* box is
// far smaller than its node's - the octree builder emits one cluster per
// octree-aligned block - so almost every cluster lies wholly inside one
// quadrant and the boundary lands where the geometry actually changes hands.
struct SparseVoxelTerrainVGClusterBlock
{
  // Quadrants of the parent tile this cluster's block reaches; 0 when unknown.
  uint8_t quadrants = 0u;
  // The block's vertical span. Cross-chunk volume correspondence is *not*
  // exact the way it is inside a chunk: a child tile covers a quarter of its
  // parent in XZ, but its vertical extent comes from its own surface range, so
  // a parent block can reach heights the child never reproduces. Yielding
  // there hands coverage to geometry that does not exist.

};

struct SparseVoxelTerrainVGChunkClusterQuadrants
{
  // Indexed by hierarchy node; zeroed for nodes that are not cluster leaves.
  std::vector<std::array<SparseVoxelTerrainVGClusterBlock, 8>> byNode;
  bool empty() const { return byNode.empty(); }
};

// What one child tile can actually cover, as the page set knows it.
struct SparseVoxelTerrainVGChildCoverage
{
  bool installed = false;

};

// Builds it from the cluster block boxes the octree builder records. `blocks`
// is indexed by linearized cluster index and holds (minX, minZ, extent); a
// hierarchy leaf's `child_start` names clusters, not nodes.
// `blocks` is indexed by linearized cluster index and holds
// (minX, minY, minZ, extent); a hierarchy leaf's `child_start` names clusters,
// not nodes.
// Tight bounds of one cluster's own emitted geometry.
//
// The quadrant mask says which child chunks a cluster's ground reaches, and it
// has to come from what the cluster actually covers rather than from the octree
// block cube it was cut from - the cube is eight cells on a side and mostly
// air, so a cluster whose geometry sits well inside one quadrant would be
// reported as straddling two.
struct SparseVoxelTerrainVGClusterSurface
{
  std::array<float, 3> minimum{};
  std::array<float, 3> maximum{};
  bool valid = false;
};

SparseVoxelTerrainVGChunkClusterQuadrants
sparseVoxelTerrainVGChunkClusterQuadrantsFromBlocks(
    const std::vector<virtualgeometry::VirtualGeometryHierarchy> &chunkHierarchy,
    const std::vector<std::array<float, 4>> &blocks,
    float centerX,
    float centerZ,
    // Per cluster, parallel to `blocks`. When absent the octree block cube is
    // used for both the quadrant mask and the vertical span.
    const std::vector<SparseVoxelTerrainVGClusterSurface> *surfaces = nullptr);

// The per-cluster form of sparseVoxelTerrainVGChunkRewritesForResidentChildren:
// clears cluster i's streaming bit only when every quadrant *that cluster's
// block* reaches is resident.
// Clears cluster i's streaming bit only when every quadrant that cluster's
// block reaches holds a child that is installed *and* whose bounds contain the
// block's vertical span - the exact "the child covers what the parent gives
// up" test, rather than "a child exists there".
// Why a cluster's streaming bit was kept when it was offered for yielding.
// Only one reason remains, and it drains on its own: the child chunks holding
// this group's DAG children are not installed yet.
struct SparseVoxelTerrainVGYieldBlockStats
{
  uint64_t offered = 0u;
  uint64_t cleared = 0u;
  uint64_t blockedNotInstalled = 0u;
  uint64_t blockedUnknownBlock = 0u;
};

std::vector<virtualgeometry::HierarchyClusterFlagsUpdate>
sparseVoxelTerrainVGChunkRewritesForResidentChildrenPerCluster(
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    uint32_t slot,
    const std::vector<virtualgeometry::HierarchyClusterFlagsUpdate> &baseUpdates,
    const SparseVoxelTerrainVGChunkClusterQuadrants &clusterQuadrants,
    const std::array<SparseVoxelTerrainVGChildCoverage, 4> &children,
    SparseVoxelTerrainVGYieldBlockStats *stats = nullptr,
    bool replaceCoveredClusters = false);

// Parks a slot: its chunk root becomes inert so the culler skips the slice.
bool parkSparseVoxelTerrainVGChunkSlot(
    virtualgeometry::VirtualGeometryScene &scene,
    const std::string &objectName,
    const SparseVoxelTerrainVGSharedObjectLayout &layout,
    uint32_t slot,
    const virtualgeometry::AABB &bounds,
    std::string *error = nullptr);

} // namespace voxel::svraster
