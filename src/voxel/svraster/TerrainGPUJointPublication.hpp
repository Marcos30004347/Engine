#pragma once

#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"
#include "voxel/svraster/TerrainGPUBuildScheduler.hpp"
#include "voxel/svraster/TerrainGPURasterBuild.hpp"
#include "voxel/svraster/TerrainGPUTree64Build.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace voxel::svraster
{

enum class TerrainGPUJointPublicationStatus : uint32_t
{
  Empty = 0u,
  Approved = 1u,
  Rejected = 2u,
};

enum class TerrainGPUJointPublicationRejection : uint32_t
{
  None = 0u,
  InvalidRequest = 1u,
  RasterNotReady = 2u,
  Tree64NotReady = 3u,
  GenerationMismatch = 4u,
  RevisionMismatch = 5u,
  OccupancyMismatch = 6u,
  RasterCapacity = 7u,
  Tree64Capacity = 8u,
  DescriptorMismatch = 9u,
};

// `forceLeafRaster` is retained in the GPU ABI for compatibility, but is a
// bit field. Deferred visibility lets the build chain relocate payload into
// inactive arenas without changing the currently published terrain cut. A
// later metadata-only transaction installs every replacement in one dispatch.
inline constexpr uint32_t kTerrainGPUJointForceLeafRaster = 1u << 0u;
inline constexpr uint32_t kTerrainGPUJointDeferVisibility = 1u << 1u;
// GPU-v2 emits nodes directly in renderer world/canonical coordinates. The
// renderer publication still relocates physical arena indices, but must not
// apply the legacy page-local transform a second time.
inline constexpr uint32_t kTerrainGPUJointSourceWorldSpace = 1u << 2u;

// One inactive physical-page install. Allocation, identity, and source-arena
// fields precede world/canonical transforms and the exact live state expected
// by the existing terrain publication compare-and-swap.
struct alignas(16) TerrainGPUJointPublicationRequestGPU
{
  uint32_t terrainSlot = 0u;
  uint32_t rasterResultIndex = 0u;
  uint32_t tree64ResultIndex = 0u;
  uint32_t publishedFrame = 0u;

  uint32_t pageFlags = kSparseVoxelTerrainPageStateProgressiveGenerated;
  uint32_t internalLOD = 0u;
  uint32_t regionMappingId = 0u;
  uint32_t clipmapLOD = 0u;

  uint32_t nodeOffset = 0u;
  uint32_t nodeCapacity = 0u;
  uint32_t childOffset = 0u;
  uint32_t childCapacity = 0u;

  uint32_t rootOffset = 0u;
  uint32_t rootCapacity = 0u;
  uint32_t materialOffset = 0u;
  uint32_t materialCapacity = 0u;

  uint32_t paletteOffset = 0u;
  uint32_t paletteCapacity = 0u;
  uint32_t tree64NodesPerPage = 0u;
  uint32_t materialPageSlot = 0u;

  uint32_t materialPageGeneration = 0u;
  uint32_t materialCellOffset = 0u;
  uint32_t materialCellCount = 0u;
  uint32_t forceLeafRaster = 0u;

  // Phase 3/4 builders may share fixed-capacity source arenas. These offsets
  // keep a multi-page publication batch from implicitly reading lane zero.
  uint32_t rasterNodeSourceOffset = 0u;
  uint32_t rasterChildSourceOffset = 0u;
  uint32_t rasterRootSourceOffset = 0u;
  uint32_t rasterMaterialSourceOffset = 0u;

  uint32_t tree64NodeSourceOffset = 0u;
  uint32_t nodePageSlot = 0u;
  uint32_t nodePageGeneration = 0u;
  // Generation of the stable dynamic Tree64 scene whose instance record is
  // updated alongside this page. This is scene metadata, not terrain payload.
  uint32_t dynamicContentGeneration = 0u;

  // Size of the raster voxel that produced the shaded receiver, as raw f32
  // bits. This is deliberately independent of the Tree64 model cell: a
  // bounded Tree64 page may conservatively combine several raster voxels into
  // one leaf, but self-intersection rejection must leave exactly the visible
  // receiver voxel rather than that larger acceleration cell.
  uint32_t receiverCellSizeBits = 0u;
  // Four page-local voxel coordinates packed as u8: min X/Z, max X/Z.
  // They describe the rectangular prism owned by finer spatial cascades.
  // Zero means no finer owner. The relocation shader intersects this page
  // rectangle with every emitted brick and writes the renderer's existing
  // tagged per-node exclusion rectangle.
  uint32_t finerClipPacked = 0u;
  uint32_t pageSide = 0u;
  uint32_t reserved0 = 0u;

  std::array<float, 4> translation{};
  // xyz is the signed canonical translation; w is the positive scale.
  std::array<int32_t, 4> canonicalTranslationScale{0, 0, 0, 1};
  std::array<uint32_t, 4> expectedPageState{};
  SparseVoxelTerrainTree64DescriptorGPU expectedTree64{};
};

struct alignas(16) TerrainGPUJointPublicationResultGPU
{
  uint32_t status = static_cast<uint32_t>(TerrainGPUJointPublicationStatus::Empty);
  uint32_t rejectionReason = static_cast<uint32_t>(TerrainGPUJointPublicationRejection::None);
  uint32_t terrainSlot = 0u;
  uint32_t terrainGeneration = 0u;
  uint32_t rasterNodeCount = 0u;
  uint32_t tree64NodeCount = 0u;
  uint32_t occupiedVoxelCount = 0u;
  uint32_t reserved0 = 0u;
};

struct TerrainGPUJointPublicationCapacities
{
  uint32_t requests = 0u;
  uint32_t rasterResults = 0u;
  uint32_t rasterNodes = 0u;
  uint32_t rasterChildren = 0u;
  uint32_t rasterMaterials = 0u;
  uint32_t rasterRoots = 0u;
  uint32_t tree64Results = 0u;
  uint32_t tree64Nodes = 0u;
  uint32_t destinationNodes = 0u;
  uint32_t destinationChildren = 0u;
  uint32_t destinationMaterials = 0u;
  uint32_t destinationRoots = 0u;
  uint32_t destinationTree64Nodes = 0u;
  uint32_t destinationTree64Instances = 0u;
};

bool validateTerrainGPUJointPublicationRequest(const TerrainGPUJointPublicationRequestGPU &request, const TerrainGPUJointPublicationCapacities &capacities, std::string *error = nullptr);

struct TerrainGPUPhysicalPageLease
{
  uint64_t rasterAllocation = 0u;
  uint32_t nodePageSlot = 0u;
  uint32_t nodePageGeneration = 0u;
  uint32_t materialPageSlot = 0u;
  uint32_t materialPageGeneration = 0u;

  bool valid() const
  {
    return rasterAllocation != 0u || nodePageGeneration != 0u || materialPageGeneration != 0u;
  }
};

// CPU ownership mirror for the GPU CAS. Acceptance retires the previous live
// lease; rejection retires only the unreachable replacement. Reclamation is
// fence driven, so neither path can recycle a page still read by the GPU.
class TerrainGPUJointPublicationRetirementQueue
{
public:
  bool begin(uint64_t publicationId, TerrainGPUPhysicalPageLease replacement, std::string *error = nullptr);
  bool complete(uint64_t publicationId, bool accepted, TerrainGPUPhysicalPageLease previous, uint64_t completionFence, std::string *error = nullptr);
  bool retire(TerrainGPUPhysicalPageLease lease, uint64_t completionFence, std::string *error = nullptr);
  std::vector<TerrainGPUPhysicalPageLease> reclaim(uint64_t completedFence);
  size_t pendingCount() const;
  size_t retiredCount() const;

private:
  struct Pending
  {
    uint64_t id = 0u;
    TerrainGPUPhysicalPageLease replacement{};
  };
  struct Retired
  {
    TerrainGPUPhysicalPageLease lease{};
    uint64_t fence = 0u;
  };
  std::vector<Pending> pending_{};
  std::vector<Retired> retired_{};
  uint64_t completedFence_ = 0u;
};

// Logical identity of a reproducible terrain page. Physical slot and
// generation are deliberately excluded: those identify one installation,
// while an evicted logical page is rebuilt into whatever slot is assigned
// next. The CPU directory never owns generated geometry or volume payloads.
struct TerrainGPUPageIdentity
{
  uint64_t bundleSignature = 0u;
  int32_t chunkX = 0;
  int32_t chunkZ = 0;
  uint32_t internalLOD = 0u;
  uint32_t generatorRevision = 0u;

  static TerrainGPUPageIdentity from(const TerrainGPUGenerationKey &key);
  bool operator==(const TerrainGPUPageIdentity &other) const;
  bool operator<(const TerrainGPUPageIdentity &other) const;
};

struct TerrainGPUPageDemand
{
  TerrainGPUBuildRequest build{};
  // Complete resident node, brick, material, palette, and sidecar footprint.
  // This is accounting metadata only; no corresponding CPU payload exists.
  uint64_t residentBytes = 0u;
  bool pinned = false;
};

struct TerrainGPUResidentPage
{
  TerrainGPUPageIdentity identity{};
  TerrainGPUGenerationKey key{};
  TerrainGPUPhysicalPageLease lease{};
  uint64_t residentBytes = 0u;
  uint64_t lastRequiredFrame = 0u;
  bool pinned = false;
};

struct TerrainGPUPageEviction
{
  TerrainGPUPageIdentity identity{};
  TerrainGPUGenerationKey key{};
  TerrainGPUPhysicalPageLease lease{};
  uint64_t residentBytes = 0u;
};

struct TerrainGPUPageResidencySettings
{
  uint32_t maximumResidentPages = 1u;
  uint64_t maximumResidentBytes = 1u;
  // Streaming cuts normally remove every page outside the current required
  // set. Set false only when an LRU tail is intentionally desired.
  bool evictUnrequired = true;
};

struct TerrainGPUPageResidencyUpdate
{
  std::vector<uint64_t> scheduledBuilds{};
  std::vector<uint64_t> cancelledBuilds{};
  std::vector<TerrainGPUPageEviction> evictions{};
};

struct TerrainGPUPageResidencyDiagnostics
{
  uint32_t residentPages = 0u;
  uint32_t pendingBuilds = 0u;
  uint32_t pendingPublications = 0u;
  uint64_t residentBytes = 0u;
  uint64_t cacheHits = 0u;
  uint64_t rebuildRequests = 0u;
  uint64_t pageInstallCount = 0u;
  uint64_t pageUninstallCount = 0u;
  uint64_t evictedPages = 0u;
  uint64_t rejectedPublications = 0u;
  uint64_t reclaimedLeases = 0u;
};

// Metadata-only owner for generated terrain pages. Missing pages are submitted
// to the GPU build scheduler, accepted publications become resident, and
// evicted/rejected physical allocations are reclaimed only after their fence.
// There is intentionally no geometry readback or CPU payload-cache API here.
class TerrainGPUResidentPageCache
{
public:
  explicit TerrainGPUResidentPageCache(
      TerrainGPUPageResidencySettings settings = {});

  bool configure(const TerrainGPUPageResidencySettings &settings,
      std::string *error = nullptr);
  bool updateRequired(uint64_t frameIndex, uint64_t retirementFence,
      const std::vector<TerrainGPUPageDemand> &required,
      TerrainGPUBuildScheduler &scheduler,
      TerrainGPUPageResidencyUpdate &update,
      std::string *error = nullptr);

  // Called once the scheduler exposes a publication and a replacement lease
  // has been allocated. Completion consumes only the small publication result
  // (accepted/rejected plus fence), never the generated terrain buffers.
  bool beginPublication(uint64_t jobId,
      TerrainGPUPhysicalPageLease replacement,
      std::string *error = nullptr);
  bool completePublication(uint64_t jobId, bool accepted,
      uint64_t completionFence, TerrainGPUBuildScheduler &scheduler,
      std::string *error = nullptr);

  std::vector<TerrainGPUPhysicalPageLease> reclaim(uint64_t completedFence);
  std::optional<TerrainGPUResidentPage> resident(
      const TerrainGPUPageIdentity &identity) const;
  TerrainGPUPageResidencyDiagnostics diagnostics() const;

private:
  struct PendingBuild
  {
    TerrainGPUBuildRequest request{};
    uint64_t jobId = 0u;
    uint64_t residentBytes = 0u;
    uint64_t lastRequiredFrame = 0u;
    bool pinned = false;
    bool publicationBegun = false;
    TerrainGPUPhysicalPageLease replacement{};
  };

  TerrainGPUPageResidencySettings settings_{};
  std::map<TerrainGPUPageIdentity, TerrainGPUResidentPage> resident_{};
  std::map<TerrainGPUPageIdentity, PendingBuild> pending_{};
  TerrainGPUJointPublicationRetirementQueue retirement_{};
  TerrainGPUPageResidencyDiagnostics diagnostics_{};
  uint64_t completedFence_ = 0u;
};

static_assert(sizeof(TerrainGPUJointPublicationRequestGPU) == 256u);
static_assert(sizeof(TerrainGPUJointPublicationResultGPU) == 32u);

} // namespace voxel::svraster
