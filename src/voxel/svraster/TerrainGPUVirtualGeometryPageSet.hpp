#pragma once

// The page set of the GPU-generated terrain virtual-geometry object.
//
// The virtual-geometry scene registers the terrain as one runtime object with
// a fixed number of page slots. The streamer's page directory changes every
// commit (pages enter as the cut refines, leave as it coarsens or the camera
// moves), so this set maps directory addresses to stable slots: a slot keeps
// its address for as long as the directory holds it, its leaf in the scene is
// rewritten as it becomes requestable or parks, and its children's residency
// is tracked per quadrant so the parent's clusters can be flagged.
//
// Bounds are fixed when a slot is assigned and never change while it lives:
// a page's clusters, its children's parent test and its own leaf all project
// the same sphere, which is what keeps the cut crack-free across levels. X/Z
// come from the address; Y comes from the chunk record when the page has one
// and from the parent slot's bounds (one parent voxel wider) otherwise.

#include "voxel/svraster/PlanetVoxelChunk.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace voxel::svraster
{

struct TerrainGPUVGPageAddressHash
{
  size_t operator()(const WorldVoxelChunkAddress &address) const noexcept
  {
    uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&](const uint64_t value)
    {
      hash ^= value;
      hash *= 0x100000001b3ull;
    };
    mix(static_cast<uint32_t>(address.x));
    mix(static_cast<uint32_t>(address.z));
    mix(address.spatialLOD);
    return static_cast<size_t>(hash ^ (hash >> 32u));
  }
};

struct TerrainGPUVGPageSetSettings
{
  uint32_t capacity = 0u;
  // Base page edge (spatial LOD 0) and the world extent: the lattice is
  // anchored at the world minimum corner, records are centred on the origin.
  double baseEdgeMetres = 0.0;
  double worldExtentMetres = 0.0;
  // Error of a level-L page is the level's voxel: finestVoxel * 2^L.
  double finestVoxelMetres = 0.0;
  // Renderer-space Y is measured from this origin (the streamer's elevation
  // origin); records carry absolute elevations.
  double elevationOriginMetres = 0.0;
};

// One directory page as the set consumes it.
struct TerrainGPUVGDirectoryPage
{
  WorldVoxelChunkAddress address{};
  // Present when a chunk record exists for the page (generated or resident).
  bool hasRecord = false;
  double recordMinimumSurfaceMetres = 0.0;
  double recordMaximumSurfaceMetres = 0.0;
  // Floor of the chunk's own vertical range. With the generated world's zero
  // surface-shell depth a column is solid from here up to its surface, and a
  // chunk's border columns emit wall faces all the way down, so the chunk's
  // *geometry* reaches this far - well below its minimum surface. Bounding a
  // chunk by its surface range instead understates it by the whole solid
  // column, which is what made the cross-chunk coverage test refuse 96% of the
  // yields whose children were fully resident.
  double recordElevationOriginMetres = 0.0;
  bool hasElevationOrigin = false;
};

struct TerrainGPUVGPage
{
  WorldVoxelChunkAddress address{};
  bool live = false;
  bool installed = false;
  bool boundsValid = false;
  bool rootPage = false;
  std::array<float, 3u> boundsMin{};
  std::array<float, 3u> boundsMax{};
  uint32_t parentSlot = UINT32_MAX;
  // Slots of the children by quadrant (bit 0: +x half, bit 1: +z half).
  std::array<uint32_t, 4u> childSlots{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
  uint32_t residentChildMask = 0u;
  // Reserved cluster range once installed, for flag rewrites.
  uint32_t clusterOffset = 0u;
  uint32_t clusterCount = 0u;
  uint64_t pageGeneration = 0u;
  // Runtime page slots this chunk owns beyond its primary one. A CPU-meshed
  // chunk is a virtual-geometry asset of several pages (meshlet counts run 400
  // to 3600), and a runtime page slot holds exactly one page, so the chunk owns
  // `1 + extraSlots.size()` of them. They are deliberately not required to be
  // contiguous: the free list would fragment and a contiguous reservation would
  // start failing while plenty of slots remained. Asset page i maps to
  // `slotForAssetPage(primarySlot, i)`, and every parent, dependency and
  // rewrite index is remapped through that.
  //
  // Parent, child, quadrant and bounds state stays on the primary slot only,
  // because those describe the *chunk*, not its pages.
  std::vector<uint32_t> extraSlots;
  // True on a slot that holds one of a chunk's non-primary asset pages. Only
  // the primary slot represents the chunk: it is the one the directory sync
  // assigns and releases, the one `slotByAddress_` maps to, and the one that
  // carries parent, child and bounds state. An extra slot is owned by its
  // primary and is released with it.
  bool extraPage = false;
  // For an extra page: the chunk slot whose asset it belongs to. The culler
  // publishes demand per page slot, and the chunk to rebuild is the owner.
  uint32_t ownerSlot = UINT32_MAX;
};

struct TerrainGPUVGPageSetSync
{
  // Slots that received an address this sync (leaves to publish).
  std::vector<uint32_t> assigned;
  // Slots whose address left the directory (pages to uninstall and leaves to
  // park). Their entries are already cleared.
  std::vector<uint32_t> released;
  // Slots whose bounds became valid this sync (leaf metadata to publish).
  std::vector<uint32_t> boundsChanged;
  // Parent slots whose resident child mask changed because a child left.
  std::vector<uint32_t> maskChanged;
  uint32_t rejected = 0u;
};

class TerrainGPUVirtualGeometryPageSet
{
public:
  bool configure(const TerrainGPUVGPageSetSettings &settings, std::string *error = nullptr);
  bool configured() const { return settings_.capacity != 0u; }
  const TerrainGPUVGPageSetSettings &settings() const { return settings_; }
  uint32_t capacity() const { return settings_.capacity; }
  uint32_t liveCount() const { return liveCount_; }

  // Brings the set in line with a directory snapshot. Ancestors must precede
  // descendants in `pages` (the directory orders by level, coarse first).
  TerrainGPUVGPageSetSync sync(const std::vector<TerrainGPUVGDirectoryPage> &pages);

  uint32_t slotOf(const WorldVoxelChunkAddress &address) const;
  const TerrainGPUVGPage *page(uint32_t slot) const;
  const TerrainGPUVGPage *find(const WorldVoxelChunkAddress &address) const;

  // Residency transitions. Both return the parent slot whose child mask
  // changed, or UINT32_MAX.
  uint32_t markInstalled(uint32_t slot, uint32_t clusterOffset, uint32_t clusterCount, uint64_t pageGeneration);
  uint32_t markUninstalled(uint32_t slot);

  // Gives `slot` exactly `pageCount` runtime page slots in total, allocating or
  // returning extras as needed. False means the pool is exhausted and nothing
  // was changed - the caller must not install a partially reserved chunk.
  bool reservePageSlots(uint32_t slot, uint32_t pageCount, std::string *error = nullptr);
  // Slot holding asset page `assetPageIndex` of the chunk at `slot`, or
  // UINT32_MAX when the chunk does not own that many pages.
  uint32_t slotForAssetPage(uint32_t slot, uint32_t assetPageIndex) const;
  // The chunk slot owning a page slot: itself for a first page, the reserving
  // slot for an extra page, UINT32_MAX when not live.
  uint32_t ownerOf(uint32_t pageSlot) const;
  // The asset page index a page slot holds within its owning chunk (the
  // inverse of slotForAssetPage), UINT32_MAX when not live.
  uint32_t assetPageOf(uint32_t pageSlot) const;
  uint32_t reservedPageCount(uint32_t slot) const;

  // Errors of a level: the level's voxel and its parent's.
  float selfError(uint32_t spatialLOD) const;
  float parentError(uint32_t spatialLOD) const;
  // World-space X/Z square of an address.
  void addressBounds(const WorldVoxelChunkAddress &address, double &minimumX, double &minimumZ, double &edge) const;
  static WorldVoxelChunkAddress parentAddress(const WorldVoxelChunkAddress &address);
  static uint32_t quadrantOf(const WorldVoxelChunkAddress &address);

private:
  uint32_t allocate();
  void release(uint32_t slot);
  void assignBounds(TerrainGPUVGPage &page, const TerrainGPUVGDirectoryPage &source);

  TerrainGPUVGPageSetSettings settings_{};
  std::vector<TerrainGPUVGPage> pages_;
  std::vector<uint32_t> freeSlots_;
  std::unordered_map<WorldVoxelChunkAddress, uint32_t, TerrainGPUVGPageAddressHash> slotByAddress_;
  uint32_t liveCount_ = 0u;
};

// ---------------------------------------------------------------------------
// Phase 4 of docs/terrain-vg-unified-hierarchy-plan.md: the GPU asks, the CPU
// generates.
//
// The culler publishes per-page demand; `collectRuntimeObjectPageRequests`
// reads it back as page slots. This turns those slots into chunk builds. It is
// a pure function so the policy - which requests survive, in what order, and
// how many - can be tested without a GPU.

struct TerrainVGPageDemand
{
  uint32_t slot = UINT32_MAX;
  uint32_t priority = 0u;
};

struct TerrainVGBuildRequest
{
  WorldVoxelChunkAddress address{};
  // The chunk's primary slot: a demand naming one of a chunk's extra page slots
  // resolves here, because a chunk is built and installed whole.
  uint32_t slot = UINT32_MAX;
  uint32_t priority = 0u;
};

// Re-score a pending generation batch when the camera moves. Siblings stay
// together so a coarse quadrant can relinquish coverage promptly; queued
// ancestors precede their children. Returns an ordering of input indices.
std::vector<size_t> orderTerrainVGPageBuilds(
    const TerrainGPUVirtualGeometryPageSet &pageSet,
    const std::vector<uint32_t> &slots,
    const std::array<double, 3> &camera);

// Highest priority first, ties broken by slot so a frame's plan is
// reproducible. Drops demand for pages that are already installed or already
// being built, collapses several demands for one chunk into a single build, and
// stops at `maximumRequests`.
std::vector<TerrainVGBuildRequest> planTerrainVGPageBuilds(
    const TerrainGPUVirtualGeometryPageSet &pageSet,
    const std::vector<TerrainVGPageDemand> &demand,
    const std::vector<uint32_t> &inFlightSlots,
    uint32_t maximumRequests);

} // namespace voxel::svraster
