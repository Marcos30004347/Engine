#include "voxel/svraster/SparseVoxelTree64Dynamic.hpp"
#include "voxel/svraster/SparseVoxelTree64TLASBridge.hpp"

#include <algorithm>
#include <array>
// These tests use assert() as their check mechanism, and Release builds
// define NDEBUG. Without this the assertions vanish - and because many of
// them wrap the call they are checking, the call under test vanishes with
// them, leaving objects unconfigured and the suite crashing or silently
// passing. cassert re-reads NDEBUG on every include, so undefining it here
// restores assert regardless of what included cassert earlier.
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace
{

using namespace voxel::svraster;

std::array<float, 16> identity()
{
  return {
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
  };
}

std::array<float, 16> inverseTranslation(const float x, const float y = 0.0f, const float z = 0.0f)
{
  auto matrix = identity();
  matrix[12] = -x;
  matrix[13] = -y;
  matrix[14] = -z;
  return matrix;
}

SparseVoxelTree64StateGPU state()
{
  SparseVoxelTree64StateGPU value{};
  value.logicalChunkResolution = {4u, 2u, 2u};
  value.chunkSide = 16u;
  value.originWorldChunk = {-1, -1, -1};
  value.valid = 1u;
  value.wrapOffset = {1u, 0u, 1u};
  value.contentGeneration = 7u;
  value.originCell = {-16, -16, -16};
  value.logicalCellResolution = {64u, 32u, 32u};
  value.cellSize = {1.0f, 1.0f, 1.0f};
  value.worldExtent = {64.0f, 32.0f, 32.0f};
  return value;
}

SparseVoxelTree64DynamicSettings settings(const uint32_t referenceCapacity = 256u)
{
  return {
    4u,
    128u,
    16u,
    referenceCapacity,
    256u,
    128u,
  };
}

SparseVoxelTree64DynamicModelSource model()
{
  return {
    17u,
    4u,
    {
      {0.0f, 0.0f, 0.0f},
      {4.0f, 4.0f, 4.0f},
    },
    {
      {
        {1.0f, 1.0f, 1.0f},
        {2.0f, 2.0f, 2.0f},
      },
    },
  };
}

SparseVoxelTree64DynamicInstanceSource instance(const uint32_t id, const float translation)
{
  return {
    id,
    17u,
    inverseTranslation(translation),
    {
      {translation, 0.0f, 0.0f},
      {translation + 4.0f, 4.0f, 4.0f},
    },
  };
}

void validateReferences(const SparseVoxelTree64DynamicSnapshot &snapshot)
{
  for (const auto &membership : snapshot.memberships)
  {
    assert(uint64_t{membership.firstReference} + membership.referenceCount <= snapshot.references.size());
    std::set<uint32_t> unique;
    for (uint32_t offset = 0u; offset < membership.referenceCount; ++offset)
    {
      const uint32_t reference = snapshot.references[membership.firstReference + offset];
      assert(reference < snapshot.instances.size());
      assert(unique.insert(reference).second);
    }
  }
}

void testSharingMembershipAndTrace()
{
  const auto active = state();
  const std::vector models{model()};
  auto receiverInstance = instance(3u, 0.0f);
  receiverInstance.flags = kSparseVoxelTree64DynamicSkipReceiverCell;
  const std::vector instances{
    receiverInstance,
    instance(9u, 20.0f),
  };
  SparseVoxelTree64DynamicSnapshot snapshot;
  std::string error;
  assert(sparseVoxelTree64BuildDynamicSnapshot(active, settings(), 4u, 11u, models, instances, snapshot, &error));
  assert(!snapshot.conservativeFallback);
  assert(sparseVoxelTree64RequiresDynamicTrace(snapshot));
  assert(snapshot.models.size() == 1u);
  assert(snapshot.instances.size() == 2u);
  assert(snapshot.instances[0].flags ==
      (kSparseVoxelTree64DynamicValid |
       kSparseVoxelTree64DynamicSkipReceiverCell));
  assert(snapshot.instances[1].flags == kSparseVoxelTree64DynamicValid);
  assert(snapshot.nodes.size() == snapshot.models[0].nodeCount);
  validateReferences(snapshot);

  SparseVoxelTree64Ray firstRay{
    {-1.0, 1.5, 1.5},
    {1.0, 0.0, 0.0},
    64.0,
  };
  const auto first = traceSparseVoxelTree64DynamicInstance(snapshot.models[0], snapshot.nodes, snapshot.instances[0], firstRay);
  assert(first.hit());
  assert(std::abs(first.distance - 2.0) < 1.0e-6);

  SparseVoxelTree64Ray receiverRay{
    {1.5, 1.5, 1.5},
    {1.0, 0.0, 0.0},
    64.0,
  };
  const auto receiver = traceSparseVoxelTree64DynamicInstance(
      snapshot.models[0], snapshot.nodes, snapshot.instances[0],
      receiverRay);
  assert(receiver.status == SparseVoxelTree64TraceStatus::Miss);
  auto ordinaryInstance = snapshot.instances[0];
  ordinaryInstance.flags = kSparseVoxelTree64DynamicValid;
  const auto ordinaryReceiver = traceSparseVoxelTree64DynamicInstance(
      snapshot.models[0], snapshot.nodes, ordinaryInstance, receiverRay);
  assert(ordinaryReceiver.hit());
  assert(ordinaryReceiver.distance == 0.0);

  SparseVoxelTree64Ray secondRay{
    {18.0, 1.5, 1.5},
    {1.0, 0.0, 0.0},
    64.0,
  };
  const auto second = traceSparseVoxelTree64DynamicInstance(snapshot.models[0], snapshot.nodes, snapshot.instances[1], secondRay);
  assert(second.hit());
  assert(std::abs(second.distance - 3.0) < 1.0e-6);

  SparseVoxelTree64DynamicInstanceGPU reflected = snapshot.instances[0];
  reflected.worldToModel = {
    -0.5f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    2.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f / 3.0f,
    0.0f,
    5.0f,
    0.0f,
    0.0f,
    1.0f,
  };
  SparseVoxelTree64Ray reflectedRay{
    {11.0, 0.75, 4.5},
    {-1.0, 0.0, 0.0},
    64.0,
  };
  const auto transformed = traceSparseVoxelTree64DynamicInstance(snapshot.models[0], snapshot.nodes, reflected, reflectedRay);
  assert(transformed.hit());
  assert(std::abs(transformed.distance - 3.0) < 1.0e-6);
}

void testTransactionalMovementAndRemoval()
{
  const auto active = state();
  const std::vector models{model()};
  SparseVoxelTree64DynamicLayer layer;
  std::string error;
  assert(layer.configure(settings(), &error));
  assert(layer.initialize(active, 1u, models, {instance(3u, 0.0f), instance(9u, 20.0f)}, &error));
  const auto originalNodes = layer.committed().nodes;

  SparseVoxelTree64DynamicUpdate stale;
  assert(layer.prepare(active, 2u, models, {instance(3u, 17.0f), instance(9u, 20.0f)}, stale, &error));
  assert(!stale.modelPayloadChanged);
  assert(stale.snapshot.models.empty());
  assert(stale.snapshot.nodes.empty());
  assert(!stale.changedWorldChunks.empty());
  assert(!layer.commit(3u, stale, &error));
  assert(layer.abort(stale, &error));

  SparseVoxelTree64DynamicUpdate latest;
  assert(layer.prepare(active, 3u, models, {instance(3u, 33.0f), instance(9u, 20.0f)}, latest, &error));
  assert(layer.commit(3u, latest, &error));
  assert(layer.committed().sceneRevision == 3u);
  assert(layer.committed().nodes.size() == originalNodes.size());
  assert(std::memcmp(layer.committed().nodes.data(), originalNodes.data(), originalNodes.size() * sizeof(SparseVoxelTree64NodeGPU)) == 0);

  SparseVoxelTree64DynamicUpdate removal;
  assert(layer.prepare(active, 4u, models, {instance(9u, 20.0f)}, removal, &error));
  assert(layer.commit(4u, removal, &error));
  assert(layer.committed().instances.size() == 1u);
  assert(layer.committed().instances[0].stableInstanceId == 9u);
  validateReferences(layer.committed());
  assert(layer.stats().staleCommits == 1u);
  assert(layer.stats().transformOnlyUpdates == 3u);
}

void testPrebuiltModelPayloadRevision()
{
  std::string error;
  SparseVoxelTree64Chunk firstTree;
  assert(buildSparseVoxelTree64(
      4u,
      {
        {
          {1.0f, 1.0f, 1.0f},
          {2.0f, 2.0f, 2.0f},
        },
      },
      firstTree,
      &error));
  SparseVoxelTree64Chunk secondTree;
  assert(buildSparseVoxelTree64(
      4u,
      {
        {
          {2.0f, 2.0f, 2.0f},
          {3.0f, 3.0f, 3.0f},
        },
      },
      secondTree,
      &error));
  assert(firstTree.nodes.size() == secondTree.nodes.size());
  assert(std::memcmp(firstTree.nodes.data(), secondTree.nodes.data(), firstTree.nodes.size() * sizeof(SparseVoxelTree64NodeGPU)) != 0);

  auto prebuilt = model();
  prebuilt.localLeafBounds.clear();
  prebuilt.prebuiltNodes = firstTree.nodes;
  prebuilt.payloadRevision = 41u;

  SparseVoxelTree64DynamicLayer layer;
  assert(layer.configure(settings(), &error));
  assert(layer.initialize(state(), 1u, {prebuilt}, {instance(3u, 0.0f)}, &error));
  assert(layer.committed().nodes.size() == firstTree.nodes.size());
  assert(std::memcmp(layer.committed().nodes.data(), firstTree.nodes.data(), firstTree.nodes.size() * sizeof(SparseVoxelTree64NodeGPU)) == 0);

  prebuilt.prebuiltNodes = secondTree.nodes;
  prebuilt.payloadRevision = 42u;
  SparseVoxelTree64DynamicUpdate replacement;
  assert(layer.prepare(state(), 2u, {prebuilt}, {instance(3u, 0.0f)}, replacement, &error));
  assert(replacement.modelPayloadChanged);
  assert(replacement.snapshot.nodes.size() == secondTree.nodes.size());
  assert(std::memcmp(replacement.snapshot.nodes.data(), secondTree.nodes.data(), secondTree.nodes.size() * sizeof(SparseVoxelTree64NodeGPU)) == 0);
  assert(layer.commit(2u, replacement, &error));
  assert(layer.committed().nodes.size() == secondTree.nodes.size());
  assert(std::memcmp(layer.committed().nodes.data(), secondTree.nodes.data(), secondTree.nodes.size() * sizeof(SparseVoxelTree64NodeGPU)) == 0);

  SparseVoxelTree64DynamicUpdate unchanged;
  assert(layer.prepare(state(), 3u, {prebuilt}, {instance(3u, 0.0f)}, unchanged, &error));
  assert(!unchanged.modelPayloadChanged);
  assert(unchanged.snapshot.models.empty());
  assert(unchanged.snapshot.nodes.empty());
  assert(layer.abort(unchanged, &error));
}

void testFixedModelNodeSlots()
{
  std::string error;
  SparseVoxelTree64Chunk firstTree;
  SparseVoxelTree64Chunk replacementTree;
  SparseVoxelTree64Chunk secondTree;
  assert(buildSparseVoxelTree64(
      4u, {{{1.0f, 1.0f, 1.0f}, {2.0f, 2.0f, 2.0f}}},
      firstTree, &error));
  assert(buildSparseVoxelTree64(
      4u, {{{2.0f, 1.0f, 1.0f}, {3.0f, 2.0f, 2.0f}}},
      replacementTree, &error));
  assert(buildSparseVoxelTree64(
      4u, {{{1.0f, 2.0f, 1.0f}, {2.0f, 3.0f, 2.0f}}},
      secondTree, &error));

  auto first = model();
  first.localLeafBounds.clear();
  first.prebuiltNodes = firstTree.nodes;
  first.payloadRevision = 1u;
  auto second = first;
  second.modelId = 18u;
  second.prebuiltNodes = secondTree.nodes;
  second.payloadRevision = 1u;
  auto secondInstance = instance(9u, 20.0f);
  secondInstance.modelId = 18u;

  auto fixedSettings = settings();
  fixedSettings.modelCapacity = 2u;
  fixedSettings.nodeCapacity = 32u;
  fixedSettings.modelNodeSlotCapacity = 16u;
  SparseVoxelTree64DynamicLayer layer;
  assert(layer.configure(fixedSettings, &error));
  assert(layer.initialize(
      state(), 1u, {first, second},
      {instance(3u, 0.0f), secondInstance}, &error));
  assert(layer.committed().nodes.size() == 32u);
  assert(layer.committed().models[0].nodeOffset == 0u);
  assert(layer.committed().models[1].nodeOffset == 16u);
  const std::vector<SparseVoxelTree64NodeGPU> retainedSecondSlot(
      layer.committed().nodes.begin() + 16u,
      layer.committed().nodes.end());

  first.prebuiltNodes = replacementTree.nodes;
  first.payloadRevision = 2u;
  SparseVoxelTree64DynamicUpdate update;
  assert(layer.prepare(
      state(), 2u, {first, second},
      {instance(3u, 0.0f), secondInstance}, update, &error));
  assert(update.modelPayloadChanged);
  assert(update.snapshot.nodes.size() == 32u);
  assert(update.snapshot.models[0].nodeOffset == 0u);
  assert(update.snapshot.models[1].nodeOffset == 16u);
  assert(std::memcmp(
      update.snapshot.nodes.data() + 16u,
      retainedSecondSlot.data(),
      retainedSecondSlot.size() * sizeof(SparseVoxelTree64NodeGPU)) == 0);
  assert(layer.commit(2u, update, &error));
}

void testTerrainGenerationLockedPhysicalSlot()
{
  std::string error;
  SparseVoxelTree64Chunk tree;
  assert(buildSparseVoxelTree64(
      4u, {{{1.0f, 1.0f, 1.0f}, {2.0f, 2.0f, 2.0f}}},
      tree, &error));
  auto terrainModel = model();
  terrainModel.localLeafBounds.clear();
  terrainModel.prebuiltNodes = tree.nodes;
  terrainModel.payloadRevision = 41u;
  terrainModel.physicalNodeSlot = 3u;
  auto terrainInstance = instance(3u, 0.0f);
  terrainInstance.terrainSlot = 3u;
  terrainInstance.terrainGeneration = 11u;
  terrainInstance.regionMappingId = 0x13572468u;

  auto terrainSettings = settings();
  terrainSettings.modelCapacity = 4u;
  terrainSettings.instanceCapacity = 4u;
  terrainSettings.nodeCapacity = 64u;
  terrainSettings.modelNodeSlotCapacity = 16u;
  SparseVoxelTree64DynamicLayer layer;
  assert(layer.configure(terrainSettings, &error));
  assert(layer.initialize(
      state(), 1u, {terrainModel}, {terrainInstance}, &error));
  assert(layer.committed().models[0].nodeOffset == 48u);
  assert(layer.committed().instances[0].terrainSlot == 3u);
  assert(layer.committed().instances[0].terrainGeneration == 11u);
  assert(layer.committed().instances[0].regionMappingId == 0x13572468u);

  terrainInstance.terrainGeneration = 12u;
  SparseVoxelTree64DynamicUpdate generationUpdate;
  assert(layer.prepare(
      state(), 2u, {terrainModel}, {terrainInstance}, generationUpdate,
      &error));
  assert(!generationUpdate.modelPayloadChanged);
  assert(!generationUpdate.changedWorldChunks.empty());
  assert(generationUpdate.snapshot.models.empty());
  assert(generationUpdate.snapshot.nodes.empty());
  assert(generationUpdate.snapshot.instances[0].terrainGeneration == 12u);
  assert(layer.commit(2u, generationUpdate, &error));
  assert(layer.committed().models[0].nodeOffset == 48u);
  assert(layer.committed().instances[0].terrainGeneration == 12u);
}

void testIncrementalTerrainMemberships()
{
  std::string error;
  SparseVoxelTree64Chunk firstTree;
  SparseVoxelTree64Chunk replacementTree;
  assert(buildSparseVoxelTree64(
      4u, {{{1.0f, 1.0f, 1.0f}, {2.0f, 2.0f, 2.0f}}},
      firstTree, &error));
  assert(buildSparseVoxelTree64(
      4u, {{{2.0f, 1.0f, 1.0f}, {3.0f, 2.0f, 2.0f}}},
      replacementTree, &error));

  auto firstModel = model();
  firstModel.modelId = 0u;
  firstModel.localLeafBounds.clear();
  firstModel.prebuiltNodes = firstTree.nodes;
  firstModel.payloadRevision = 1u;
  firstModel.physicalNodeSlot = 0u;
  auto secondModel = firstModel;
  secondModel.modelId = 1u;
  secondModel.physicalNodeSlot = 1u;

  auto firstInstance = instance(0u, 0.0f);
  firstInstance.modelId = 0u;
  firstInstance.terrainSlot = 0u;
  firstInstance.terrainGeneration = 7u;
  auto secondInstance = instance(1u, 20.0f);
  secondInstance.modelId = 1u;
  secondInstance.terrainSlot = 1u;
  secondInstance.terrainGeneration = 9u;

  SparseVoxelTree64DynamicSettings terrainSettings{
    4u, 4u, 16u, 64u, 64u, 4u, 16u, true};
  SparseVoxelTree64DynamicLayer layer;
  assert(layer.configure(terrainSettings, &error));
  assert(layer.initialize(
      state(), 1u, {firstModel, secondModel},
      {firstInstance, secondInstance}, &error));
  const auto initial = layer.committed();
  assert(initial.models.size() == terrainSettings.modelCapacity);
  assert(initial.instances.size() == terrainSettings.instanceCapacity);
  assert(initial.references.size() ==
      initial.memberships.size() * terrainSettings.maximumReferencesPerChunk);
  for (uint32_t index = 0u; index < initial.memberships.size(); ++index)
    assert(initial.memberships[index].firstReference ==
        index * terrainSettings.maximumReferencesPerChunk);

  // A physical page replacement is descriptor-indirected. Its payload
  // revision must not disturb any directory, reference, model or instance
  // bytes owned by the stable logical terrain slot.
  firstModel.prebuiltNodes = replacementTree.nodes;
  firstModel.payloadRevision = 2u;
  SparseVoxelTree64DynamicUpdate lodUpdate;
  assert(layer.prepare(
      state(), 2u, {firstModel, secondModel},
      {firstInstance, secondInstance}, lodUpdate, &error));
  assert(!lodUpdate.modelPayloadChanged);
  assert(lodUpdate.sparseTerrainUpdate);
  assert(lodUpdate.snapshot.contentGeneration == initial.contentGeneration);
  assert(lodUpdate.snapshot.nodes.empty());
  assert(lodUpdate.snapshot.models.empty());
  assert(lodUpdate.snapshot.instances.empty());
  assert(lodUpdate.snapshot.memberships.empty());
  assert(lodUpdate.snapshot.references.empty());
  assert(lodUpdate.modelRanges.empty());
  assert(lodUpdate.instanceRanges.empty());
  assert(lodUpdate.membershipRanges.empty());
  assert(lodUpdate.referenceRanges.empty());
  assert(layer.commit(2u, lodUpdate, &error));

  // Advancing the toroidal directory one X cell changes exactly its entering
  // physical plane. Retained physical slots keep byte-identical memberships
  // and fixed reference slices.
  auto movedState = state();
  ++movedState.originWorldChunk[0];
  movedState.wrapOffset[0] =
      (movedState.wrapOffset[0] + 1u) % movedState.logicalChunkResolution[0];
  SparseVoxelTree64DynamicUpdate directoryUpdate;
  assert(layer.prepare(
      movedState, 3u, {firstModel, secondModel},
      {firstInstance, secondInstance}, directoryUpdate, &error));
  uint32_t changedMemberships = 0u;
  for (const auto &range : directoryUpdate.membershipRanges)
    changedMemberships += static_cast<uint32_t>(range.values.size());
  assert(changedMemberships ==
      movedState.logicalChunkResolution[1] *
          movedState.logicalChunkResolution[2]);
  assert(directoryUpdate.snapshot.memberships.empty());
  assert(!directoryUpdate.referenceRanges.empty());
  assert(layer.commit(3u, directoryUpdate, &error));

  // Reusing one stable terrain slot rewrites only that instance record; other
  // stable instance indices never shift.
  firstInstance.terrainGeneration = 8u;
  SparseVoxelTree64DynamicUpdate reuseUpdate;
  assert(layer.prepare(
      movedState, 4u, {firstModel, secondModel},
      {firstInstance, secondInstance}, reuseUpdate, &error));
  assert(reuseUpdate.instanceRanges.size() == 1u);
  assert(reuseUpdate.instanceRanges[0].first == 0u);
  assert(reuseUpdate.instanceRanges[0].values.size() == 1u);
  assert(reuseUpdate.instanceRanges[0].values[0].terrainGeneration == 8u);
  assert(layer.commit(4u, reuseUpdate, &error));
}

void testTerrainTLASStableSlotIndirection()
{
  std::string error;
  auto firstInstance = instance(0u, 0.0f);
  firstInstance.modelId = 0u;
  firstInstance.terrainSlot = 0u;
  firstInstance.terrainGeneration = 7u;
  auto secondInstance = instance(1u, 20.0f);
  secondInstance.modelId = 1u;
  secondInstance.terrainSlot = 1u;
  secondInstance.terrainGeneration = 9u;
  const std::vector<SparseVoxelTree64DynamicInstanceSource> terrainInstances{
    firstInstance, secondInstance};

  // Software BVH leaf order is independent from Tree64 stable terrain slots.
  const std::vector<uint32_t> instanceIndexToInputIndex{1u, 0u};
  const auto stableSlots = sparseVoxelTree64TLASInstanceSlots(
      terrainInstances, instanceIndexToInputIndex, true);
  assert((stableSlots == std::vector<uint32_t>{1u, 0u}));
  assert(sparseVoxelTree64ValidateTLASInstanceSlots(
      terrainInstances, instanceIndexToInputIndex, stableSlots, true,
      &error));

  const auto compactSlots = sparseVoxelTree64TLASInstanceSlots(
      terrainInstances, instanceIndexToInputIndex, false);
  assert((compactSlots == std::vector<uint32_t>{1u, 0u}));
  assert(sparseVoxelTree64ValidateTLASInstanceSlots(
      terrainInstances, instanceIndexToInputIndex, compactSlots, false,
      &error));

  auto staleSlots = stableSlots;
  staleSlots[0] = 0u;
  assert(!sparseVoxelTree64ValidateTLASInstanceSlots(
      terrainInstances, instanceIndexToInputIndex, staleSlots, true,
      &error));
}

void testConservativeOverflow()
{
  SparseVoxelTree64DynamicSnapshot snapshot;
  std::string error;
  assert(sparseVoxelTree64BuildDynamicSnapshot(state(), settings(1u), 5u, 12u, {model()}, {instance(3u, 0.0f), instance(9u, 0.0f)}, snapshot, &error));
  assert(snapshot.conservativeFallback);
  assert(sparseVoxelTree64RequiresDynamicTrace(snapshot));
  assert(snapshot.instances.empty());
  assert(snapshot.references.empty());
  for (const auto &membership : snapshot.memberships)
    assert(membership.flags == (kSparseVoxelTree64DynamicValid | kSparseVoxelTree64DynamicUnknown));

  SparseVoxelTree64DynamicSnapshot staticOnly;
  assert(sparseVoxelTree64BuildDynamicSnapshot(state(), settings(), 6u, 13u, {model()}, {}, staticOnly, &error));
  assert(!sparseVoxelTree64RequiresDynamicTrace(staticOnly));
}

void testDestinationFirstMigration()
{
  SparseVoxelTree64DynamicPolicyTracker policies;
  std::string error;
  assert(policies.initialize(
      {
        {3u, SparseVoxelTree64InstancePolicy::Baked},
        {9u, SparseVoxelTree64InstancePolicy::Dynamic},
      },
      &error));
  SparseVoxelTree64MigrationUpdate toDynamic;
  const auto oldInstance = instance(3u, 0.0f);
  const auto newInstance = instance(3u, 17.0f);
  assert(policies.prepare(state(), 3u, SparseVoxelTree64InstancePolicy::Dynamic, oldInstance.worldBounds, newInstance.worldBounds, toDynamic, &error));
  assert(!toDynamic.bakedInvalidationChunks.empty());
  auto premature = toDynamic;
  assert(!policies.sourceRemoved(premature, &error));
  assert(!policies.commit(toDynamic, &error));
  assert(policies.destinationPublished(toDynamic, &error));
  assert(!policies.abort(toDynamic, &error));
  assert(policies.sourceRemoved(toDynamic, &error));
  assert(policies.commit(toDynamic, &error));
  assert(policies.policy(3u) == SparseVoxelTree64InstancePolicy::Dynamic);

  SparseVoxelTree64MigrationUpdate toBaked;
  const auto dynamicInstance = instance(9u, 20.0f);
  assert(policies.prepare(state(), 9u, SparseVoxelTree64InstancePolicy::Baked, dynamicInstance.worldBounds, dynamicInstance.worldBounds, toBaked, &error));
  assert(policies.destinationPublished(toBaked, &error));
  assert(policies.sourceRemoved(toBaked, &error));
  assert(policies.commit(toBaked, &error));
  assert(policies.policy(9u) == SparseVoxelTree64InstancePolicy::Baked);
}

void benchmarkMembershipUpdates()
{
  const auto active = state();
  const std::vector models{model()};
  constexpr std::array counts{1u, 16u, 64u};
  constexpr uint32_t iterations = 20u;
  std::string error;
  for (const uint32_t count : counts)
  {
    std::vector<SparseVoxelTree64DynamicInstanceSource> initial;
    std::vector<SparseVoxelTree64DynamicInstanceSource> moved;
    for (uint32_t index = 0u; index < count; ++index)
    {
      const float position = static_cast<float>(index % 4u) * 8.0f;
      initial.push_back(instance(index, position));
      moved.push_back(instance(index, position + 1.0f));
    }
    SparseVoxelTree64DynamicLayer layer;
    assert(layer.configure(settings(4096u), &error));
    assert(layer.initialize(active, 1u, models, initial, &error));
    SparseVoxelTree64DynamicUpdate update;
    const auto begin = std::chrono::steady_clock::now();
    for (uint32_t iteration = 0u; iteration < iterations; ++iteration)
    {
      assert(layer.prepare(active, 2u, models, moved, update, &error));
      assert(!update.modelPayloadChanged);
      assert(layer.abort(update, &error));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
    std::vector<SparseVoxelAABB> movedBounds;
    movedBounds.reserve(moved.size());
    for (const auto &source : moved)
      movedBounds.push_back(source.worldBounds);
    uint64_t rebakeNodes = 0u;
    const auto rebakeBegin = std::chrono::steady_clock::now();
    for (uint32_t iteration = 0u; iteration < iterations; ++iteration)
    {
      std::vector<SparseVoxelTree64BuildSourceGPU> sources;
      assert(sparseVoxelTree64MakeBuildSources(active, update.changedWorldChunks, movedBounds, sources, nullptr, &error));
      for (uint32_t job = 0u; job < update.changedWorldChunks.size(); ++job)
      {
        std::vector<SparseVoxelTree64AABB> local;
        for (const auto &source : sources)
          if (source.jobIndex == job)
            local.push_back({
              {source.minimum[0], source.minimum[1], source.minimum[2]},
              {source.maximum[0], source.maximum[1], source.maximum[2]},
            });
        SparseVoxelTree64Chunk tree;
        assert(buildSparseVoxelTree64(active.chunkSide, local, tree, &error));
        rebakeNodes += tree.nodes.size();
      }
    }
    const auto rebakeElapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - rebakeBegin).count();
    const uint64_t uploadBytes = update.snapshot.instances.size() * sizeof(SparseVoxelTree64DynamicInstanceGPU) + update.snapshot.memberships.size() * sizeof(SparseVoxelTree64DynamicMembershipGPU) +
                                 update.snapshot.references.size() * sizeof(uint32_t);
    std::cout << "TREE64_T7_BENCH instances=" << count << " changed_chunks=" << update.changedWorldChunks.size() << " references=" << update.snapshot.references.size() << " transform_upload_bytes=" << uploadBytes
              << " iterations=" << iterations << " dynamic_update_elapsed_us=" << elapsed << " reference_rebake_elapsed_us=" << rebakeElapsed << " rebake_nodes=" << rebakeNodes << '\n';
  }
}

} // namespace

int main()
{
  testSharingMembershipAndTrace();
  testTransactionalMovementAndRemoval();
  testPrebuiltModelPayloadRevision();
  testFixedModelNodeSlots();
  testTerrainGenerationLockedPhysicalSlot();
  testIncrementalTerrainMemberships();
  testTerrainTLASStableSlotIndirection();
  testConservativeOverflow();
  testDestinationFirstMigration();
  benchmarkMembershipUpdates();
  std::cout << "Sparse voxel Tree64 T7 dynamic CPU tests passed\n";
  return 0;
}
