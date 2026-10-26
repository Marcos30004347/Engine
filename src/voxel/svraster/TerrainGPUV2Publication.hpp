#pragma once

#include "voxel/svraster/SparseVoxelStreaming.hpp"
#include "voxel/svraster/TerrainGPUV2RasterHierarchy.hpp"
#include "voxel/svraster/TerrainGPUV2Scheduling.hpp"
#include "voxel/svraster/TerrainGPUV2Tree64.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace voxel::svraster
{

inline constexpr uint32_t kTerrainGPUV2PublicationMaximumMembers = 8u;

enum class TerrainGPUV2PublicationStatus : uint32_t
{
  Empty,
  Approved,
  Committed,
  Rejected,
};

enum class TerrainGPUV2PublicationRejection : uint32_t
{
  None,
  InvalidRequest,
  InvalidPayload,
  GenerationMismatch,
  RevisionMismatch,
  OccupancyMismatch,
  CoverageGapOrOverlap,
  StaleOwner,
  Capacity,
};

enum TerrainGPUV2PayloadFlags : uint32_t
{
  TerrainGPUV2PayloadOccupancyOnly = 1u << 0u,
  TerrainGPUV2PayloadNoMicroGeometry = 1u << 1u,
  TerrainGPUV2PayloadRasterHierarchy = 1u << 2u,
};

struct TerrainGPUV2CoverageBox
{
  std::array<int32_t, 3u> minimum{};
  std::array<int32_t, 3u> maximum{};

  bool operator==(const TerrainGPUV2CoverageBox &other) const
  {
    return minimum == other.minimum && maximum == other.maximum;
  }
};

struct TerrainGPUV2PayloadResultGPU
{
  uint32_t status = 0u;
  uint32_t validationErrors = 0u;
  uint32_t flags = 0u;
  uint32_t rasterNodeCount = 0u;
  uint32_t rasterMaterialCount = 0u;
  uint32_t rasterChildCount = 0u;
  uint32_t rasterRootCount = 0u;
  uint32_t rasterOccupiedVoxelCount = 0u;
  uint32_t terrainGeneration = 0u;
  uint32_t rasterGeneratorRevision = 0u;
  uint32_t treeNodeCount = 0u;
  uint32_t treeVisitedNodeCount = 0u;
  uint32_t treeOccupiedVoxelCount = 0u;
  uint32_t treeGeneratorRevision = 0u;
  uint32_t geometryRevisionLow = 0u;
  uint32_t geometryRevisionHigh = 0u;
};

struct alignas(16) TerrainGPUV2PublicationMemberGPU
{
  TerrainGPUChunkKeyGPU key{};
  TerrainGPUV2CoverageBox coverage{};
  uint32_t payloadResultIndex = 0u;
  uint32_t ownerFlags = 0u;
  uint64_t leaseId = 0u;
  uint64_t leaseOffset = 0u;
  uint64_t leaseBytes = 0u;
  std::array<uint32_t, 2u> reserved{};
};

struct alignas(16) TerrainGPUV2PublishedOwnerGPU
{
  TerrainGPUChunkKeyGPU key{};
  TerrainGPUV2CoverageBox coverage{};
  uint32_t ownerFlags = 0u;
  uint32_t payloadResultIndex = 0u;
  uint64_t leaseId = 0u;
  uint64_t leaseOffset = 0u;
  uint64_t leaseBytes = 0u;
  uint32_t payloadRevisionLow = 0u;
  uint32_t payloadRevisionHigh = 0u;
};

// Even epochs are stable. An odd epoch is a short GPU seqlock held only while
// the owner range and its identity are replaced in one publication dispatch.
struct alignas(16) TerrainGPUV2OwnershipGroupStateGPU
{
  uint32_t epoch = 0u;
  uint32_t terrainGeneration = 0u;
  uint32_t payloadRevisionLow = 0u;
  uint32_t payloadRevisionHigh = 0u;
  uint32_t ownerOffset = 0u;
  uint32_t ownerCount = 0u;
  uint32_t flags = 0u;
  uint32_t reserved0 = 0u;
  TerrainGPUV2CoverageBox coverage{};
  std::array<uint32_t, 2u> reserved1{};
};

struct alignas(16) TerrainGPUV2PublicationRequestGPU
{
  uint32_t groupIndex = 0u;
  uint32_t expectedEpoch = 0u;
  uint32_t expectedTerrainGeneration = 0u;
  uint32_t expectedPayloadRevisionLow = 0u;
  uint32_t expectedPayloadRevisionHigh = 0u;
  uint32_t replacementTerrainGeneration = 0u;
  uint32_t replacementPayloadRevisionLow = 0u;
  uint32_t replacementPayloadRevisionHigh = 0u;
  uint32_t memberOffset = 0u;
  uint32_t memberCount = 0u;
  uint32_t ownerOffset = 0u;
  uint32_t ownerCapacity = 0u;
  uint32_t expectedOwnerOffset = 0u;
  uint32_t expectedOwnerCount = 0u;
  uint32_t flags = 0u;
  uint32_t reserved = 0u;
};

struct TerrainGPUV2PublicationResultGPU
{
  uint32_t status = static_cast<uint32_t>(TerrainGPUV2PublicationStatus::Empty);
  uint32_t rejectionReason = static_cast<uint32_t>(TerrainGPUV2PublicationRejection::None);
  uint32_t groupIndex = 0u;
  uint32_t committedEpoch = 0u;
  uint32_t ownerCount = 0u;
  uint32_t terrainGeneration = 0u;
  uint32_t payloadRevisionLow = 0u;
  uint32_t payloadRevisionHigh = 0u;
};

struct TerrainGPUV2PublicationCapacities
{
  uint32_t requests = 0u;
  uint32_t members = 0u;
  uint32_t payloadResults = 0u;
  uint32_t groups = 0u;
  uint32_t owners = 0u;
};

struct alignas(16) TerrainGPUV2PublicationBatchGPU
{
  uint32_t requestCount = 0u;
  uint32_t memberCount = 0u;
  uint32_t payloadResultCount = 0u;
  uint32_t groupCount = 0u;
  uint32_t ownerCount = 0u;
  std::array<uint32_t, 3u> reserved{};
};

static_assert(sizeof(TerrainGPUV2CoverageBox) == 24u);
static_assert(sizeof(TerrainGPUV2PayloadResultGPU) == 64u);
static_assert(sizeof(TerrainGPUV2PublicationMemberGPU) == 96u);
static_assert(sizeof(TerrainGPUV2PublishedOwnerGPU) == 96u);
static_assert(sizeof(TerrainGPUV2OwnershipGroupStateGPU) == 64u);
static_assert(sizeof(TerrainGPUV2PublicationRequestGPU) == 64u);
static_assert(sizeof(TerrainGPUV2PublicationResultGPU) == 32u);
static_assert(sizeof(TerrainGPUV2PublicationBatchGPU) == 32u);

bool terrainGPUV2PayloadResult(
    const TerrainGPUV2FineRasterResultGPU &fineRaster,
    const TerrainGPUV2RasterHierarchyResultGPU *hierarchy,
    const TerrainGPUV2Tree64ResultGPU &tree64,
    TerrainGPUV2PayloadResultGPU &result,
    std::string *error = nullptr);

bool terrainGPUV2ProducedPagePayload(
    SparseVoxelPageId page,
    uint64_t expectedPageGeneration,
    uint32_t requiredProducerRevision,
    const TerrainGPUV2PayloadResultGPU &result,
    SparseVoxelProducedPagePayload &payload,
    std::string *error = nullptr);

struct TerrainGPUV2StreamedChunkPages
{
  SparseVoxelPageProducer producer{};
  SparseVoxelPageId rootPage{};
  SparseVoxelPageId desiredPage{};

  bool valid() const
  {
    return producer.valid() && rootPage.valid() && desiredPage.valid() && rootPage.producer == producer.producerId && desiredPage.producer == producer.producerId;
  }
};

uint64_t terrainGPUV2StreamedChunkProducerId(uint64_t worldBundleRevision, uint32_t generatorRevision);
bool terrainGPUV2StreamedChunkPages(const TerrainGPUChunkKey &key, TerrainGPUV2StreamedChunkPages &pages, std::string *error = nullptr);
bool terrainGPUV2StreamedChunkInstance(const TerrainGPUChunkKey &key, uint32_t instanceIndex, uint32_t firstRootNode, uint32_t pageCount, SparseVoxelStreamedInstance &instance, std::string *error = nullptr);

bool validateTerrainGPUV2PublicationBatch(
    const TerrainGPUV2PublicationCapacities &capacities,
    const std::vector<TerrainGPUV2PublicationRequestGPU> &requests,
    const std::vector<TerrainGPUV2PublicationMemberGPU> &members,
    const std::vector<TerrainGPUV2PayloadResultGPU> &payloads,
    const std::vector<TerrainGPUV2OwnershipGroupStateGPU> &groups,
    std::string *error = nullptr);

bool applyTerrainGPUV2PublicationOracle(
    const TerrainGPUV2PublicationCapacities &capacities,
    const std::vector<TerrainGPUV2PublicationRequestGPU> &requests,
    const std::vector<TerrainGPUV2PublicationMemberGPU> &members,
    const std::vector<TerrainGPUV2PayloadResultGPU> &payloads,
    std::vector<TerrainGPUV2OwnershipGroupStateGPU> &groups,
    std::vector<TerrainGPUV2PublishedOwnerGPU> &owners,
    std::vector<TerrainGPUV2PublicationResultGPU> &results,
    std::string *error = nullptr);

enum class TerrainGPUV2ResidentCategory : uint32_t
{
  RasterNodes,
  RasterOccupancy,
  RasterMaterials,
  RasterTopology,
  Tree64Nodes,
  Atlas,
  Scratch,
  Metadata,
  Count,
};

struct TerrainGPUV2ResidentFootprint
{
  std::array<uint64_t, static_cast<size_t>(TerrainGPUV2ResidentCategory::Count)> bytes{};
  uint64_t total() const;
  bool add(const TerrainGPUV2ResidentFootprint &other);
};

struct TerrainGPUV2ResidencySettings
{
  uint64_t hardBudgetBytes = 2048ull << 20u;
  uint64_t highWatermarkBytes = 1740ull << 20u;
  uint64_t emergencyWatermarkBytes = 1945ull << 20u;
  uint64_t minimumAllocationClassBytes = 256u;
  uint32_t maximumResidentChunks = 65536u;
};

struct TerrainGPUV2ChunkIdentity
{
  uint64_t worldBundleRevision = 0u;
  int32_t chunkX = 0;
  int32_t chunkY = 0;
  int32_t chunkZ = 0;
  uint16_t clipmapLOD = 0u;
  uint16_t generatorRevision = 0u;

  static TerrainGPUV2ChunkIdentity from(const TerrainGPUChunkKey &key);
  bool operator==(const TerrainGPUV2ChunkIdentity &other) const;
  bool operator<(const TerrainGPUV2ChunkIdentity &other) const;
};

struct TerrainGPUV2HiddenOutput
{
  uint64_t handle = 0u;
  uint64_t ownershipGroup = 0u;
  TerrainGPUChunkKey key{};
  TerrainGPUV2CoverageBox coverage{};
  uint64_t payloadRevision = 0u;
  TerrainGPUV2PayloadResultGPU payload{};
  TerrainGPUV2Lease lease{};
  TerrainGPUV2ResidentFootprint footprint{};
  uint64_t lastRequiredFrame = 0u;
  bool required = true;
  bool pinned = false;
};

struct TerrainGPUV2ActiveOwner
{
  TerrainGPUChunkKey key{};
  TerrainGPUV2CoverageBox coverage{};
  uint64_t payloadRevision = 0u;
  TerrainGPUV2Lease lease{};
  TerrainGPUV2ResidentFootprint footprint{};
  uint64_t lastRequiredFrame = 0u;
  bool required = true;
  bool pinned = false;
};

struct TerrainGPUV2SharedPageResidency
{
  SparseVoxelPageId page{};
  SparseVoxelPageState state = SparseVoxelPageState::Unloaded;
  TerrainGPUV2ChunkIdentity terrainIdentity{};
  uint64_t legacyOwnershipGroup = 0u;
  uint64_t legacyHiddenHandle = 0u;
  uint64_t payloadRevision = 0u;
  uint32_t terrainGeneration = 0u;
  uint64_t lastRequiredFrame = 0u;
  bool required = true;
  bool pinned = false;
};

struct TerrainGPUV2OwnershipExpectation
{
  uint64_t group = 0u;
  uint32_t epoch = 0u;
  uint32_t terrainGeneration = 0u;
  uint64_t payloadRevision = 0u;
  std::vector<TerrainGPUV2ChunkIdentity> owners{};
};

struct TerrainGPUV2GroupPublication
{
  TerrainGPUV2OwnershipExpectation expected{};
  TerrainGPUV2CoverageBox coverage{};
  uint32_t replacementTerrainGeneration = 0u;
  uint64_t replacementPayloadRevision = 0u;
  std::vector<uint64_t> hiddenHandles{};
  // Compatibility escape hatch for the old terrain owner-group cut. New
  // renderer-facing code should consume sharedPageResidency() and publish
  // ordinary SparseVoxelPageId install/uninstall rewrites instead.
  bool allowLegacyVisibleCutPublication = false;
};

struct TerrainGPUV2ResidencyDiagnostics
{
  TerrainGPUV2ResidentFootprint categories{};
  uint64_t accountedBytes = 0u;
  uint64_t peakAccountedBytes = 0u;
  uint64_t payloadReservedBytes = 0u;
  uint64_t fragmentationBytes = 0u;
  uint32_t activeChunks = 0u;
  uint32_t hiddenChunks = 0u;
  uint32_t retiredChunks = 0u;
  uint64_t acceptedPublications = 0u;
  uint64_t rejectedPublications = 0u;
  uint64_t pageInstallCount = 0u;
  uint64_t pageUninstallCount = 0u;
  uint64_t evictedChunks = 0u;
  uint64_t reclaimedChunks = 0u;
  uint64_t rebuildRequests = 0u;
  uint64_t hardBudgetRejections = 0u;
  uint64_t highWatermarkTrims = 0u;
  uint64_t emergencyTrims = 0u;
  uint64_t cpuPayloadBytes = 0u;
};

class TerrainGPUV2ResidencyManager
{
public:
  explicit TerrainGPUV2ResidencyManager(TerrainGPUV2ResidencySettings settings = {});
  ~TerrainGPUV2ResidencyManager();

  bool configure(const TerrainGPUV2ResidencySettings &settings, std::string *error = nullptr);
  bool setExternalFootprint(const TerrainGPUV2ResidentFootprint &footprint, uint64_t retirementFence, std::string *error = nullptr);
  bool reserveHidden(
      uint64_t ownershipGroup,
      const TerrainGPUChunkKey &key,
      const TerrainGPUV2CoverageBox &coverage,
      uint64_t payloadRevision,
      const TerrainGPUV2PayloadResultGPU &payload,
      const TerrainGPUV2ResidentFootprint &footprint,
      uint64_t frameIndex,
      uint64_t retirementFence,
      bool required,
      bool pinned,
      uint64_t &handle,
      std::string *error = nullptr);
  bool publish(const TerrainGPUV2GroupPublication &publication, uint64_t completionFence, TerrainGPUV2PublicationResultGPU &result, std::string *error = nullptr);
  bool setGroupRequired(uint64_t group, bool required, uint64_t frameIndex);
  bool evictUnrequired(uint64_t retirementFence, std::vector<TerrainGPUV2ChunkIdentity> &evicted, std::string *error = nullptr);
  uint32_t reclaim(uint64_t completedFence);
  bool require(const TerrainGPUChunkKey &key);
  std::vector<TerrainGPUChunkKey> takeRebuildRequests();
  std::vector<TerrainGPUV2ActiveOwner> visible(uint64_t group) const;
  std::vector<TerrainGPUV2SharedPageResidency> sharedPageResidency() const;
  std::optional<TerrainGPUV2HiddenOutput> hidden(uint64_t handle) const;
  TerrainGPUV2OwnershipExpectation expectation(uint64_t group) const;
  TerrainGPUV2ResidencyDiagnostics diagnostics() const;

private:
  struct Group;
  struct Retired;
  bool trim(uint64_t projectedBytes, uint64_t fence, bool emergency, bool requireChunkSlot, std::string *error);
  bool retire(const TerrainGPUV2Lease &lease, const TerrainGPUV2ResidentFootprint &footprint, uint64_t fence, std::string *error);
  void refreshDiagnostics();

  TerrainGPUV2ResidencySettings settings_{};
  TerrainGPUV2FencedArena arena_{};
  TerrainGPUV2ResidentFootprint external_{};
  std::map<uint64_t, Group> groups_{};
  std::map<uint64_t, TerrainGPUV2HiddenOutput> hidden_{};
  std::vector<Retired> retired_{};
  std::map<TerrainGPUV2ChunkIdentity, TerrainGPUChunkKey> rebuild_{};
  TerrainGPUV2ResidencyDiagnostics diagnostics_{};
  uint64_t nextHiddenHandle_ = 1u;
  uint64_t completedFence_ = 0u;
};

} // namespace voxel::svraster
