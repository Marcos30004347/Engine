#pragma once

#include "voxel/svraster/SparseVoxelTree64.hpp"
#include "voxel/svraster/SparseVoxelTree64Reference.hpp"
#include "voxel/svraster/SparseVoxelTree64Scene.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace voxel::svraster
{

struct SparseVoxelTree64DynamicModelSource
{
  uint32_t modelId = 0u;
  uint32_t side = 0u;
  SparseVoxelAABB localBounds{};
  std::vector<SparseVoxelAABB> localLeafBounds;
  // Optional immutable CPU-built payload. When present, snapshot assembly
  // copies these nodes directly instead of voxelizing localLeafBounds again.
  // payloadRevision lets streaming replace a same-sized tree without an
  // expensive node-by-node comparison.
  std::vector<SparseVoxelTree64NodeGPU> prebuiltNodes;
  uint64_t payloadRevision = 0u;
  // Optional fixed physical BLAS slot. Terrain uses its stable chunk slot so
  // changing or removing another model never shifts retained node pages.
  uint32_t physicalNodeSlot = std::numeric_limits<uint32_t>::max();
};

struct SparseVoxelTree64DynamicInstanceSource
{
  uint32_t stableInstanceId = 0u;
  uint32_t modelId = 0u;
  std::array<float, 16> worldToModel{};
  SparseVoxelAABB worldBounds{};
  uint32_t flags = 0u;
  // Zero generation denotes an ordinary dynamic instance. Generated terrain
  // carries the raster slot generation and is accepted by traversal only when
  // the atomically published terrain page-state table matches it.
  uint32_t terrainSlot = std::numeric_limits<uint32_t>::max();
  uint32_t terrainGeneration = 0u;
  uint32_t regionMappingId = 0u;
  // Size of the raster receiver voxel in model-space metres. Terrain Tree64
  // may be conservatively reduced, so this can be smaller than a tree leaf.
  // Zero selects the Tree64 leaf size for non-terrain diagnostic instances.
  float receiverCellSizeMetres = 0.0f;
};

struct SparseVoxelTree64DynamicSettings
{
  uint32_t modelCapacity = 0u;
  uint32_t instanceCapacity = 0u;
  uint32_t membershipCapacity = 0u;
  uint32_t referenceCapacity = 0u;
  uint32_t nodeCapacity = 0u;
  uint32_t maximumReferencesPerChunk = 0u;
  // Optional fixed physical allocation for every sorted model. Streamed
  // instances can then rewrite one BLAS without shifting all following node
  // offsets. Zero retains compact packing for ordinary dynamic scenes.
  uint32_t modelNodeSlotCapacity = 0u;
  // Terrain pages use their logical terrain slot as a permanent model and
  // instance index. Membership references then occupy one fixed slice per
  // physical directory slot, allowing an entering toroidal row to be
  // rewritten without repacking retained rows. The dynamic generation stays
  // stable; terrainGeneration remains the stale-owner guard for each page.
  bool stableTerrainSlots = false;
};

struct SparseVoxelTree64DynamicSnapshot
{
  uint32_t contentGeneration = 0u;
  uint64_t sceneRevision = 0u;
  bool conservativeFallback = false;
  std::vector<SparseVoxelTree64DynamicModelGPU> models;
  std::vector<SparseVoxelTree64NodeGPU> nodes;
  std::vector<SparseVoxelTree64DynamicInstanceGPU> instances;
  std::vector<SparseVoxelTree64DynamicMembershipGPU> memberships;
  std::vector<uint32_t> references;
};

// Stable generated terrain publishes only the records that changed. Values in
// one range are contiguous in the destination buffer; ordinary dynamic scenes
// continue to use the complete snapshot above.
template <typename T>
struct SparseVoxelTree64DynamicRangeUpdate
{
  uint32_t first = 0u;
  std::vector<T> values;
};

struct SparseVoxelTree64DynamicUpdate
{
  uint64_t transaction = 0u;
  uint64_t previousSceneRevision = 0u;
  uint64_t targetSceneRevision = 0u;
  bool modelPayloadChanged = false;
  bool sparseTerrainUpdate = false;
  std::vector<std::array<int32_t, 3>> changedWorldChunks;
  std::vector<SparseVoxelTree64DynamicRangeUpdate<
      SparseVoxelTree64DynamicModelGPU>> modelRanges;
  std::vector<SparseVoxelTree64DynamicRangeUpdate<
      SparseVoxelTree64DynamicInstanceGPU>> instanceRanges;
  std::vector<SparseVoxelTree64DynamicRangeUpdate<
      SparseVoxelTree64DynamicMembershipGPU>> membershipRanges;
  std::vector<SparseVoxelTree64DynamicRangeUpdate<uint32_t>> referenceRanges;
  SparseVoxelTree64DynamicSnapshot snapshot;
};

struct SparseVoxelTree64DynamicStats
{
  uint32_t models = 0u;
  uint32_t sharedModelInstances = 0u;
  uint32_t instances = 0u;
  uint32_t memberships = 0u;
  uint32_t references = 0u;
  uint32_t maximumReferencesInChunk = 0u;
  uint32_t nodes = 0u;
  uint64_t preparedUpdates = 0u;
  uint64_t committedUpdates = 0u;
  uint64_t staleCommits = 0u;
  uint64_t conservativeFallbacks = 0u;
  uint64_t modelPayloadUpdates = 0u;
  uint64_t transformOnlyUpdates = 0u;
};

enum class SparseVoxelTree64InstancePolicy : uint32_t
{
  Baked = 0u,
  Dynamic = 1u,
};

enum class SparseVoxelTree64MigrationPhase : uint32_t
{
  Prepared = 0u,
  DestinationPublished = 1u,
  SourceRemoved = 2u,
};

struct SparseVoxelTree64MigrationUpdate
{
  uint64_t transaction = 0u;
  uint32_t stableInstanceId = 0u;
  SparseVoxelTree64InstancePolicy previousPolicy = SparseVoxelTree64InstancePolicy::Baked;
  SparseVoxelTree64InstancePolicy targetPolicy = SparseVoxelTree64InstancePolicy::Baked;
  SparseVoxelTree64MigrationPhase phase = SparseVoxelTree64MigrationPhase::Prepared;
  std::vector<std::array<int32_t, 3>> bakedInvalidationChunks;
};

bool sparseVoxelTree64BuildDynamicSnapshot(
    const SparseVoxelTree64StateGPU &state,
    const SparseVoxelTree64DynamicSettings &settings,
    uint32_t contentGeneration,
    uint64_t sceneRevision,
    const std::vector<SparseVoxelTree64DynamicModelSource> &models,
    const std::vector<SparseVoxelTree64DynamicInstanceSource> &instances,
    SparseVoxelTree64DynamicSnapshot &snapshot,
    std::string *error = nullptr);

// Slow, exhaustive membership classifier retained for T8 parity tests and
// before/after measurements. Runtime code must use the range-specialized
// sparseVoxelTree64BuildDynamicSnapshot path above.
bool sparseVoxelTree64BuildDynamicSnapshotBruteForceReference(
    const SparseVoxelTree64StateGPU &state,
    const SparseVoxelTree64DynamicSettings &settings,
    uint32_t contentGeneration,
    uint64_t sceneRevision,
    const std::vector<SparseVoxelTree64DynamicModelSource> &models,
    const std::vector<SparseVoxelTree64DynamicInstanceSource> &instances,
    SparseVoxelTree64DynamicSnapshot &snapshot,
    std::string *error = nullptr);

bool sparseVoxelTree64RequiresDynamicTrace(const SparseVoxelTree64DynamicSnapshot &snapshot);

std::vector<std::array<int32_t, 3>>
sparseVoxelTree64DynamicChangedChunks(const SparseVoxelTree64StateGPU &state, const std::vector<SparseVoxelTree64DynamicInstanceSource> &previous, const std::vector<SparseVoxelTree64DynamicInstanceSource> &current);

SparseVoxelTree64TraceResult traceSparseVoxelTree64DynamicInstance(
    const SparseVoxelTree64DynamicModelGPU &model,
    const std::vector<SparseVoxelTree64NodeGPU> &nodes,
    const SparseVoxelTree64DynamicInstanceGPU &instance,
    const SparseVoxelTree64Ray &worldRay,
    uint32_t maximumQueries = std::numeric_limits<uint32_t>::max());

class SparseVoxelTree64DynamicLayer
{
public:
  bool configure(const SparseVoxelTree64DynamicSettings &settings, std::string *error = nullptr);
  bool initialize(
      const SparseVoxelTree64StateGPU &state,
      uint64_t sceneRevision,
      const std::vector<SparseVoxelTree64DynamicModelSource> &models,
      const std::vector<SparseVoxelTree64DynamicInstanceSource> &instances,
      std::string *error = nullptr);
  bool prepare(
      const SparseVoxelTree64StateGPU &state,
      uint64_t sceneRevision,
      const std::vector<SparseVoxelTree64DynamicModelSource> &models,
      const std::vector<SparseVoxelTree64DynamicInstanceSource> &instances,
      SparseVoxelTree64DynamicUpdate &update,
      std::string *error = nullptr);
  bool commit(uint64_t sceneRevision, const SparseVoxelTree64DynamicUpdate &update, std::string *error = nullptr);
  // The sources the last commit replaced. Their leaf bounds and prebuilt nodes
  // are large; release them on a worker, not where the frame is built.
  std::pair<std::vector<SparseVoxelTree64DynamicModelSource>, std::vector<SparseVoxelTree64DynamicInstanceSource>>
  takeRetiredSources()
  {
    return {std::move(retiredModelSources_), std::move(retiredInstanceSources_)};
  }
  bool validateCommit(uint64_t sceneRevision, const SparseVoxelTree64DynamicUpdate &update, std::string *error = nullptr) const;
  bool abort(const SparseVoxelTree64DynamicUpdate &update, std::string *error = nullptr);

  bool configured() const;
  bool initialized() const;
  bool hasPendingUpdate() const;
  const SparseVoxelTree64DynamicSnapshot &committed() const;
  SparseVoxelTree64DynamicStats stats() const;

private:
  struct Pending
  {
    SparseVoxelTree64DynamicUpdate update;
    std::vector<SparseVoxelTree64DynamicModelSource> modelSources;
    std::vector<SparseVoxelTree64DynamicInstanceSource> instanceSources;
    SparseVoxelTree64StateGPU state{};
  };

  void refreshStats();

  SparseVoxelTree64DynamicSettings settings_{};
  SparseVoxelTree64DynamicSnapshot committed_{};
  std::vector<SparseVoxelTree64DynamicModelSource> committedModelSources_;
  std::vector<SparseVoxelTree64DynamicModelSource> retiredModelSources_;
  std::vector<SparseVoxelTree64DynamicInstanceSource> retiredInstanceSources_;
  std::vector<SparseVoxelTree64DynamicInstanceSource> committedInstanceSources_;
  SparseVoxelTree64StateGPU committedState_{};
  std::optional<Pending> pending_;
  uint64_t nextTransaction_ = 1u;
  SparseVoxelTree64DynamicStats stats_{};
  bool configured_ = false;
  bool initialized_ = false;
};

// Enforces destination-first migration. During DestinationPublished the
// instance may conservatively exist in both representations; source removal
// cannot occur before that point, so it can never disappear from both.
class SparseVoxelTree64DynamicPolicyTracker
{
public:
  bool initialize(const std::vector<std::pair<uint32_t, SparseVoxelTree64InstancePolicy>> &policies, std::string *error = nullptr);
  bool prepare(
      const SparseVoxelTree64StateGPU &state,
      uint32_t stableInstanceId,
      SparseVoxelTree64InstancePolicy targetPolicy,
      const SparseVoxelAABB &previousBounds,
      const SparseVoxelAABB &currentBounds,
      SparseVoxelTree64MigrationUpdate &update,
      std::string *error = nullptr);
  bool destinationPublished(SparseVoxelTree64MigrationUpdate &update, std::string *error = nullptr);
  bool sourceRemoved(SparseVoxelTree64MigrationUpdate &update, std::string *error = nullptr);
  bool commit(const SparseVoxelTree64MigrationUpdate &update, std::string *error = nullptr);
  bool abort(const SparseVoxelTree64MigrationUpdate &update, std::string *error = nullptr);

  std::optional<SparseVoxelTree64InstancePolicy> policy(uint32_t stableInstanceId) const;
  bool hasPendingMigration() const;

private:
  std::map<uint32_t, SparseVoxelTree64InstancePolicy> policies_;
  std::optional<SparseVoxelTree64MigrationUpdate> pending_;
  uint64_t nextTransaction_ = 1u;
};

} // namespace voxel::svraster
