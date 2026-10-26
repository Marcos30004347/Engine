#include "voxel/svraster/SparseVoxelTree64Dynamic.hpp"

#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace voxel::svraster
{
namespace
{

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool validBounds(const SparseVoxelAABB &bounds, const bool positiveExtent)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (!std::isfinite(bounds.minimum[axis]) || !std::isfinite(bounds.maximum[axis]) || bounds.minimum[axis] > bounds.maximum[axis] || (positiveExtent && !(bounds.maximum[axis] > bounds.minimum[axis])))
      return false;
  }
  return true;
}

bool validMatrix(const std::array<float, 16> &matrix)
{
  for (const float value : matrix)
    if (!std::isfinite(value))
      return false;
  return true;
}

uint64_t membershipCount(const SparseVoxelTree64StateGPU &state)
{
  return uint64_t{state.logicalChunkResolution[0]} * state.logicalChunkResolution[1] * state.logicalChunkResolution[2];
}

bool validState(const SparseVoxelTree64StateGPU &state)
{
  if (state.valid == 0u || !sparseVoxelTree64SupportedSide(state.chunkSide))
    return false;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const int64_t lastChunk = static_cast<int64_t>(state.originWorldChunk[axis]) + state.logicalChunkResolution[axis] - 1u;
    if (state.logicalChunkResolution[axis] == 0u || lastChunk > std::numeric_limits<int32_t>::max() || !std::isfinite(state.cellSize[axis]) || !(state.cellSize[axis] > 0.0f))
      return false;
  }
  return membershipCount(state) <= std::numeric_limits<uint32_t>::max();
}

uint32_t physicalIndex(const std::array<uint32_t, 3> &physical, const std::array<uint32_t, 3> &resolution)
{
  return physical[0] + resolution[0] * (physical[1] + resolution[1] * physical[2]);
}

std::vector<std::array<int32_t, 3>> activeChunks(const SparseVoxelTree64StateGPU &state)
{
  std::vector<std::array<int32_t, 3>> chunks;
  chunks.reserve(static_cast<size_t>(membershipCount(state)));
  for (uint32_t z = 0u; z < state.logicalChunkResolution[2]; ++z)
    for (uint32_t y = 0u; y < state.logicalChunkResolution[1]; ++y)
      for (uint32_t x = 0u; x < state.logicalChunkResolution[0]; ++x)
        chunks.push_back({
          static_cast<int32_t>(static_cast<int64_t>(state.originWorldChunk[0]) + x),
          static_cast<int32_t>(static_cast<int64_t>(state.originWorldChunk[1]) + y),
          static_cast<int32_t>(static_cast<int64_t>(state.originWorldChunk[2]) + z),
        });
  return chunks;
}

bool intersects(const SparseVoxelTree64StateGPU &state, const std::array<int32_t, 3> &worldChunk, const SparseVoxelAABB &bounds)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const double minimum = static_cast<double>(worldChunk[axis]) * state.chunkSide * state.cellSize[axis];
    const double maximum = minimum + state.chunkSide * state.cellSize[axis];
    if (static_cast<double>(bounds.maximum[axis]) < minimum || static_cast<double>(bounds.minimum[axis]) > maximum)
      return false;
  }
  return true;
}

enum class MembershipBuild
{
  RangeSpecialized,
  BruteForceReference,
};

bool appendReference(const SparseVoxelTree64StateGPU &state, const std::array<int32_t, 3> &chunk, const uint32_t instanceIndex, std::vector<std::vector<uint32_t>> &chunkReferences, std::string *error)
{
  std::array<uint32_t, 3> physical{};
  if (!sparseVoxelTree64DirectorySlot(chunk, state.originWorldChunk, state.logicalChunkResolution, state.wrapOffset, physical))
    return fail(error, "Tree64 dynamic chunk mapping failed");
  chunkReferences[physicalIndex(physical, state.logicalChunkResolution)].push_back(instanceIndex);
  return true;
}

bool buildChunkReferences(
    const SparseVoxelTree64StateGPU &state,
    const std::vector<SparseVoxelTree64DynamicInstanceSource> &instances,
    const std::vector<std::array<int32_t, 3>> &chunks,
    const MembershipBuild build,
    std::vector<std::vector<uint32_t>> &chunkReferences,
    std::string *error)
{
  if (build == MembershipBuild::BruteForceReference)
  {
    for (uint32_t instanceIndex = 0u; instanceIndex < instances.size(); ++instanceIndex)
      for (const auto &chunk : chunks)
        if (intersects(state, chunk, instances[instanceIndex].worldBounds) && !appendReference(state, chunk, instanceIndex, chunkReferences, error))
          return false;
    return true;
  }

  for (uint32_t instanceIndex = 0u; instanceIndex < instances.size(); ++instanceIndex)
  {
    std::array<int32_t, 3> first{};
    std::array<int32_t, 3> last{};
    bool outside = false;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const double chunkExtent = static_cast<double>(state.chunkSide) * state.cellSize[axis];
      // Expand by one candidate on either side and retain the exact closed-AABB
      // predicate below. The expansion makes this insensitive to division
      // rounding when a float bound lies exactly on a computed chunk boundary.
      const double candidateFirst = std::floor(static_cast<double>(instances[instanceIndex].worldBounds.minimum[axis]) / chunkExtent) - 1.0;
      const double candidateLast = std::floor(static_cast<double>(instances[instanceIndex].worldBounds.maximum[axis]) / chunkExtent) + 1.0;
      const double activeFirst = state.originWorldChunk[axis];
      const double activeLast = static_cast<int64_t>(state.originWorldChunk[axis]) + state.logicalChunkResolution[axis] - 1u;
      const double clampedFirst = std::max(candidateFirst, activeFirst);
      const double clampedLast = std::min(candidateLast, activeLast);
      if (clampedFirst > clampedLast)
      {
        outside = true;
        break;
      }
      first[axis] = static_cast<int32_t>(clampedFirst);
      last[axis] = static_cast<int32_t>(clampedLast);
    }
    if (outside)
      continue;

    // Preserve the reference classifier's z/y/x order so every membership
    // range remains deterministic and byte-identical.
    for (int64_t z = first[2]; z <= last[2]; ++z)
      for (int64_t y = first[1]; y <= last[1]; ++y)
        for (int64_t x = first[0]; x <= last[0]; ++x)
        {
          const std::array<int32_t, 3> chunk{static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(z)};
          if (intersects(state, chunk, instances[instanceIndex].worldBounds) && !appendReference(state, chunk, instanceIndex, chunkReferences, error))
            return false;
        }
  }
  return true;
}

void makeUnknownSnapshot(const SparseVoxelTree64StateGPU &state, const uint32_t generation, const uint64_t sceneRevision, SparseVoxelTree64DynamicSnapshot &snapshot)
{
  snapshot = {};
  snapshot.contentGeneration = generation;
  snapshot.sceneRevision = sceneRevision;
  snapshot.conservativeFallback = true;
  snapshot.memberships.resize(static_cast<size_t>(membershipCount(state)));
  for (const auto &chunk : activeChunks(state))
  {
    std::array<uint32_t, 3> physical{};
    const bool mapped = sparseVoxelTree64DirectorySlot(chunk, state.originWorldChunk, state.logicalChunkResolution, state.wrapOffset, physical);
    if (!mapped)
      continue;
    snapshot.memberships[physicalIndex(physical, state.logicalChunkResolution)] = {
      chunk,
      0u,
      0u,
      generation,
      kSparseVoxelTree64DynamicValid | kSparseVoxelTree64DynamicUnknown,
      0u,
    };
  }
}

bool sameSource(const SparseVoxelTree64DynamicInstanceSource &left, const SparseVoxelTree64DynamicInstanceSource &right)
{
  return left.modelId == right.modelId && left.flags == right.flags &&
      left.terrainSlot == right.terrainSlot &&
      left.terrainGeneration == right.terrainGeneration &&
      left.regionMappingId == right.regionMappingId &&
      left.receiverCellSizeMetres == right.receiverCellSizeMetres &&
      left.worldToModel == right.worldToModel &&
      left.worldBounds.minimum == right.worldBounds.minimum &&
      left.worldBounds.maximum == right.worldBounds.maximum;
}

bool sameModels(const std::vector<SparseVoxelTree64DynamicModelSource> &left, const std::vector<SparseVoxelTree64DynamicModelSource> &right)
{
  if (left.size() != right.size())
    return false;
  for (size_t index = 0u; index < left.size(); ++index)
  {
    if (left[index].modelId != right[index].modelId || left[index].side != right[index].side || left[index].localBounds.minimum != right[index].localBounds.minimum ||
        left[index].localBounds.maximum != right[index].localBounds.maximum || left[index].localLeafBounds.size() != right[index].localLeafBounds.size() ||
        left[index].prebuiltNodes.size() != right[index].prebuiltNodes.size() ||
        left[index].payloadRevision != right[index].payloadRevision ||
        left[index].physicalNodeSlot != right[index].physicalNodeSlot)
      return false;
    for (size_t leaf = 0u; leaf < left[index].localLeafBounds.size(); ++leaf)
      if (left[index].localLeafBounds[leaf].minimum != right[index].localLeafBounds[leaf].minimum || left[index].localLeafBounds[leaf].maximum != right[index].localLeafBounds[leaf].maximum)
        return false;
  }
  return true;
}

bool matchesModelPayload(const std::vector<SparseVoxelTree64DynamicModelSource> &sources, const SparseVoxelTree64DynamicSnapshot &snapshot)
{
  if (sources.size() != snapshot.models.size())
    return false;
  for (size_t index = 0u; index < sources.size(); ++index)
    if (sources[index].modelId != snapshot.models[index].modelId || snapshot.models[index].nodeCount == 0u || uint64_t{snapshot.models[index].nodeOffset} + snapshot.models[index].nodeCount > snapshot.nodes.size())
      return false;
  return true;
}

bool makeDynamicInstance(
    const SparseVoxelTree64DynamicInstanceSource &source,
    const uint32_t contentGeneration,
    SparseVoxelTree64DynamicInstanceGPU &instance,
    std::string *error)
{
  static_cast<void>(error);
  instance = {};
  instance.worldToModel = source.worldToModel;
  instance.worldBoundsMinimum = {
    source.worldBounds.minimum[0], source.worldBounds.minimum[1],
    source.worldBounds.minimum[2], 0.0f};
  instance.worldBoundsMaximum = {
    source.worldBounds.maximum[0], source.worldBounds.maximum[1],
    source.worldBounds.maximum[2], 0.0f};
  instance.modelIndex = source.modelId;
  instance.stableInstanceId = source.stableInstanceId;
  instance.contentGeneration = contentGeneration;
  instance.flags = kSparseVoxelTree64DynamicValid | source.flags;
  instance.terrainSlot = source.terrainSlot;
  instance.terrainGeneration = source.terrainGeneration;
  instance.regionMappingId = source.regionMappingId;
  static_assert(
      sizeof(instance.receiverCellSizeBits) ==
      sizeof(source.receiverCellSizeMetres));
  std::memcpy(
      &instance.receiverCellSizeBits, &source.receiverCellSizeMetres,
      sizeof(instance.receiverCellSizeBits));
  return true;
}

bool makeDynamicModel(
    const SparseVoxelTree64DynamicModelSource &source,
    const SparseVoxelTree64DynamicSettings &settings,
    SparseVoxelTree64DynamicModelGPU &model,
    std::string *error)
{
  const uint32_t slot = source.physicalNodeSlot !=
          std::numeric_limits<uint32_t>::max()
      ? source.physicalNodeSlot
      : source.modelId;
  if (slot >= settings.modelCapacity ||
      settings.modelNodeSlotCapacity == 0u)
    return fail(error, "Tree64 terrain model slot is invalid");
  const uint64_t nodeOffset =
      uint64_t{slot} * settings.modelNodeSlotCapacity;
  if (nodeOffset > std::numeric_limits<uint32_t>::max() ||
      source.prebuiltNodes.empty() ||
      source.prebuiltNodes.size() > settings.modelNodeSlotCapacity)
    return fail(error, "Tree64 terrain model payload exceeds its fixed slot");
  model = {};
  model.localMinimum = source.localBounds.minimum;
  model.nodeOffset = static_cast<uint32_t>(nodeOffset);
  for (uint32_t axis = 0u; axis < 3u; ++axis)
    model.localCellSize[axis] =
        (source.localBounds.maximum[axis] - source.localBounds.minimum[axis]) /
        static_cast<float>(source.side);
  model.nodeCount = static_cast<uint32_t>(source.prebuiltNodes.size());
  model.side = source.side;
  model.modelId = source.modelId;
  model.flags = kSparseVoxelTree64DynamicValid;
  return true;
}

bool buildStableTerrainSnapshot(
    const SparseVoxelTree64StateGPU &state,
    const SparseVoxelTree64DynamicSettings &settings,
    const uint64_t sceneRevision,
    const std::vector<SparseVoxelTree64DynamicModelSource> &models,
    const std::vector<SparseVoxelTree64DynamicInstanceSource> &instances,
    const SparseVoxelTree64DynamicSnapshot *previous,
    const std::vector<SparseVoxelTree64DynamicInstanceSource> *previousSources,
    SparseVoxelTree64DynamicSnapshot &snapshot,
    std::string *error)
{
  if (!validState(state) || settings.modelNodeSlotCapacity == 0u)
    return fail(error, "Tree64 stable terrain state is invalid");
  const uint64_t activeCount = membershipCount(state);
  const uint64_t fixedReferenceCount =
      activeCount * settings.maximumReferencesPerChunk;
  if (activeCount > settings.membershipCapacity ||
      fixedReferenceCount > settings.referenceCapacity ||
      fixedReferenceCount > std::numeric_limits<uint32_t>::max())
    return fail(error, "Tree64 stable terrain reference slices exceed capacity");

  const uint32_t generation = previous != nullptr
      ? previous->contentGeneration
      : 1u;
  snapshot = {};
  snapshot.contentGeneration = generation;
  snapshot.sceneRevision = sceneRevision;
  snapshot.models = previous != nullptr
      ? previous->models
      : std::vector<SparseVoxelTree64DynamicModelGPU>(settings.modelCapacity);
  snapshot.instances = previous != nullptr
      ? previous->instances
      : std::vector<SparseVoxelTree64DynamicInstanceGPU>(settings.instanceCapacity);
  snapshot.memberships = previous != nullptr
      ? previous->memberships
      : std::vector<SparseVoxelTree64DynamicMembershipGPU>(static_cast<size_t>(activeCount));
  snapshot.references = previous != nullptr
      ? previous->references
      : std::vector<uint32_t>(static_cast<size_t>(fixedReferenceCount));
  if (snapshot.models.size() != settings.modelCapacity ||
      snapshot.instances.size() != settings.instanceCapacity ||
      snapshot.memberships.size() != activeCount ||
      snapshot.references.size() != fixedReferenceCount)
    return fail(error, "Tree64 stable terrain allocation changed");

  std::vector<const SparseVoxelTree64DynamicModelSource *> modelBySlot(
      settings.modelCapacity, nullptr);
  for (const auto &source : models)
  {
    const uint32_t slot = source.physicalNodeSlot !=
            std::numeric_limits<uint32_t>::max()
        ? source.physicalNodeSlot
        : source.modelId;
    if (slot >= modelBySlot.size() || modelBySlot[slot] != nullptr ||
        source.modelId != slot ||
        !sparseVoxelTree64SupportedModelSide(source.side) ||
        !validBounds(source.localBounds, true) ||
        source.localBounds.minimum != std::array<float, 3>{0.0f, 0.0f, 0.0f} ||
        source.prebuiltNodes.empty() ||
        source.prebuiltNodes.size() > settings.modelNodeSlotCapacity)
      return fail(error, "Tree64 stable terrain models are invalid or have duplicate logical slots");
    const float cellSize =
        source.localBounds.maximum[0] / static_cast<float>(source.side);
    if (!(cellSize > 0.0f) ||
        std::abs((source.localBounds.maximum[1] / source.side) - cellSize) > 1.0e-5f ||
        std::abs((source.localBounds.maximum[2] / source.side) - cellSize) > 1.0e-5f)
      return fail(error, "Tree64 stable terrain model lattice is not uniform");
    modelBySlot[slot] = &source;
  }
  std::vector<const SparseVoxelTree64DynamicInstanceSource *> instanceBySlot(
      settings.instanceCapacity, nullptr);
  for (const auto &source : instances)
  {
    if (source.stableInstanceId >= instanceBySlot.size() ||
        instanceBySlot[source.stableInstanceId] != nullptr ||
        source.modelId != source.stableInstanceId ||
        (source.flags & ~kSparseVoxelTree64DynamicSourceFlagMask) != 0u ||
        !validMatrix(source.worldToModel) ||
        !validBounds(source.worldBounds, true) ||
        source.terrainSlot != source.stableInstanceId ||
        source.terrainGeneration == 0u)
      return fail(error, "Tree64 stable terrain instances are invalid or have duplicate logical slots");
    instanceBySlot[source.stableInstanceId] = &source;
  }

  std::vector<bool> changedInstances(settings.instanceCapacity, previous == nullptr);
  std::vector<const SparseVoxelTree64DynamicInstanceSource *> oldBySlot(
      settings.instanceCapacity, nullptr);
  if (previousSources != nullptr)
    for (const auto &source : *previousSources)
      if (source.stableInstanceId < oldBySlot.size())
        oldBySlot[source.stableInstanceId] = &source;

  for (uint32_t slot = 0u; slot < settings.modelCapacity; ++slot)
  {
    if (modelBySlot[slot] == nullptr)
    {
      snapshot.models[slot] = {};
      continue;
    }
    // A resident terrain slot keeps stable model metadata. Internal LOD node
    // and material data are selected by its independently published page
    // descriptor, so changing payloadRevision must not rewrite this array.
    if (previous == nullptr ||
        (snapshot.models[slot].flags & kSparseVoxelTree64DynamicValid) == 0u)
      if (!makeDynamicModel(*modelBySlot[slot], settings,
                            snapshot.models[slot], error))
        return false;
  }
  for (uint32_t slot = 0u; slot < settings.instanceCapacity; ++slot)
  {
    const auto *current = instanceBySlot[slot];
    const auto *old = oldBySlot[slot];
    changedInstances[slot] = previous == nullptr ||
        (current == nullptr) != (old == nullptr) ||
        (current != nullptr && old != nullptr && !sameSource(*current, *old));
    if (!changedInstances[slot])
      continue;
    if (current == nullptr)
      snapshot.instances[slot] = {};
    else if (!makeDynamicInstance(*current, generation,
                                  snapshot.instances[slot], error))
      return false;
  }

  std::vector<bool> rewriteMembership(static_cast<size_t>(activeCount), previous == nullptr);
  const auto chunks = activeChunks(state);
  for (const auto &chunk : chunks)
  {
    std::array<uint32_t, 3> physical{};
    if (!sparseVoxelTree64DirectorySlot(
            chunk, state.originWorldChunk, state.logicalChunkResolution,
            state.wrapOffset, physical))
      return fail(error, "Tree64 stable terrain chunk mapping failed");
    const uint32_t index = physicalIndex(physical, state.logicalChunkResolution);
    if (snapshot.memberships[index].worldChunk != chunk)
      rewriteMembership[index] = true;
    if (!rewriteMembership[index])
      for (uint32_t slot = 0u; slot < changedInstances.size(); ++slot)
        if (changedInstances[slot] &&
            ((oldBySlot[slot] != nullptr && intersects(state, chunk, oldBySlot[slot]->worldBounds)) ||
             (instanceBySlot[slot] != nullptr && intersects(state, chunk, instanceBySlot[slot]->worldBounds))))
        {
          rewriteMembership[index] = true;
          break;
        }
    if (!rewriteMembership[index])
      continue;

    const uint32_t firstReference =
        index * settings.maximumReferencesPerChunk;
    uint32_t count = 0u;
    for (uint32_t slot = 0u; slot < instanceBySlot.size(); ++slot)
      if (instanceBySlot[slot] != nullptr &&
          intersects(state, chunk, instanceBySlot[slot]->worldBounds))
      {
        if (count == settings.maximumReferencesPerChunk)
          return fail(error, "Tree64 stable terrain membership exceeded its reference slice");
        snapshot.references[firstReference + count++] = slot;
      }
    std::fill(snapshot.references.begin() + firstReference + count,
              snapshot.references.begin() + firstReference +
                  settings.maximumReferencesPerChunk,
              0u);
    snapshot.memberships[index] = {
      chunk, firstReference, count, generation,
      kSparseVoxelTree64DynamicValid, 0u};
  }
  return true;
}

template <typename T>
void appendRangeValue(
    std::vector<SparseVoxelTree64DynamicRangeUpdate<T>> &ranges,
    const uint32_t index,
    const T &value)
{
  if (ranges.empty() ||
      uint64_t{ranges.back().first} + ranges.back().values.size() != index)
    ranges.push_back({index, {}});
  ranges.back().values.push_back(value);
}

bool buildStableTerrainUpdate(
    const SparseVoxelTree64StateGPU &state,
    const SparseVoxelTree64DynamicSettings &settings,
    const uint64_t sceneRevision,
    const std::vector<SparseVoxelTree64DynamicModelSource> &models,
    const std::vector<SparseVoxelTree64DynamicInstanceSource> &instances,
    const SparseVoxelTree64DynamicSnapshot &committed,
    const std::vector<SparseVoxelTree64DynamicInstanceSource> &previousSources,
    SparseVoxelTree64DynamicUpdate &update,
    std::string *error)
{
  if (!validState(state) || settings.modelNodeSlotCapacity == 0u)
    return fail(error, "Tree64 stable terrain state is invalid");
  const uint64_t activeCount = membershipCount(state);
  const uint64_t fixedReferenceCount =
      activeCount * settings.maximumReferencesPerChunk;
  if (activeCount > settings.membershipCapacity ||
      fixedReferenceCount > settings.referenceCapacity ||
      fixedReferenceCount > std::numeric_limits<uint32_t>::max() ||
      committed.models.size() != settings.modelCapacity ||
      committed.instances.size() != settings.instanceCapacity ||
      committed.memberships.size() != activeCount ||
      committed.references.size() != fixedReferenceCount)
    return fail(error, "Tree64 stable terrain sparse allocation changed");

  std::vector<const SparseVoxelTree64DynamicModelSource *> modelBySlot(
      settings.modelCapacity, nullptr);
  for (const auto &source : models)
  {
    const uint32_t slot = source.physicalNodeSlot !=
            std::numeric_limits<uint32_t>::max()
        ? source.physicalNodeSlot
        : source.modelId;
    if (slot >= modelBySlot.size() || modelBySlot[slot] != nullptr ||
        source.modelId != slot ||
        !sparseVoxelTree64SupportedModelSide(source.side) ||
        !validBounds(source.localBounds, true) ||
        source.localBounds.minimum !=
            std::array<float, 3>{0.0f, 0.0f, 0.0f})
      return fail(error, "Tree64 stable terrain models are invalid or have duplicate logical slots");
    const bool modelAlreadyPublished =
        (committed.models[slot].flags &
         kSparseVoxelTree64DynamicValid) != 0u;
    if ((!modelAlreadyPublished && source.prebuiltNodes.empty()) ||
        source.prebuiltNodes.size() > settings.modelNodeSlotCapacity)
      return fail(error, "Tree64 stable terrain model payload exceeds its fixed slot");
    const float cellSize =
        (source.localBounds.maximum[0] - source.localBounds.minimum[0]) /
        static_cast<float>(source.side);
    if (!(cellSize > 0.0f) ||
        std::abs(((source.localBounds.maximum[1] -
                       source.localBounds.minimum[1]) /
                      source.side) -
                 cellSize) > 1.0e-5f ||
        std::abs(((source.localBounds.maximum[2] -
                       source.localBounds.minimum[2]) /
                      source.side) -
                 cellSize) > 1.0e-5f)
      return fail(error, "Tree64 stable terrain model lattice is not uniform");
    modelBySlot[slot] = &source;
  }

  std::vector<const SparseVoxelTree64DynamicInstanceSource *> instanceBySlot(
      settings.instanceCapacity, nullptr);
  for (const auto &source : instances)
  {
    if (source.stableInstanceId >= instanceBySlot.size() ||
        instanceBySlot[source.stableInstanceId] != nullptr ||
        source.modelId != source.stableInstanceId ||
        (source.flags & ~kSparseVoxelTree64DynamicSourceFlagMask) != 0u ||
        !validMatrix(source.worldToModel) ||
        !validBounds(source.worldBounds, true) ||
        source.terrainSlot != source.stableInstanceId ||
        source.terrainGeneration == 0u)
      return fail(error, "Tree64 stable terrain instances are invalid or have duplicate logical slots");
    instanceBySlot[source.stableInstanceId] = &source;
  }

  std::vector<const SparseVoxelTree64DynamicInstanceSource *> oldBySlot(
      settings.instanceCapacity, nullptr);
  for (const auto &source : previousSources)
    if (source.stableInstanceId < oldBySlot.size())
      oldBySlot[source.stableInstanceId] = &source;

  std::vector<bool> changedInstances(settings.instanceCapacity, false);
  for (uint32_t slot = 0u; slot < settings.modelCapacity; ++slot)
  {
    const bool wasValid =
        (committed.models[slot].flags &
         kSparseVoxelTree64DynamicValid) != 0u;
    if (modelBySlot[slot] == nullptr)
    {
      if (wasValid)
        appendRangeValue(update.modelRanges, slot,
                         SparseVoxelTree64DynamicModelGPU{});
      continue;
    }
    // Internal LOD data lives in the physical page descriptor. The stable
    // model record is installed only when a logical slot first becomes live.
    if (!wasValid)
    {
      SparseVoxelTree64DynamicModelGPU model{};
      if (!makeDynamicModel(*modelBySlot[slot], settings, model, error))
        return false;
      appendRangeValue(update.modelRanges, slot, model);
    }
  }
  for (uint32_t slot = 0u; slot < settings.instanceCapacity; ++slot)
  {
    const auto *current = instanceBySlot[slot];
    const auto *old = oldBySlot[slot];
    changedInstances[slot] =
        (current == nullptr) != (old == nullptr) ||
        (current != nullptr && old != nullptr && !sameSource(*current, *old));
    if (!changedInstances[slot])
      continue;
    SparseVoxelTree64DynamicInstanceGPU instance{};
    if (current != nullptr &&
        !makeDynamicInstance(*current, committed.contentGeneration,
                             instance, error))
      return false;
    appendRangeValue(update.instanceRanges, slot, instance);
  }

  struct MembershipWrite
  {
    uint32_t index = 0u;
    SparseVoxelTree64DynamicMembershipGPU membership{};
    std::vector<uint32_t> references;
  };
  std::vector<MembershipWrite> writes;
  const auto chunks = activeChunks(state);
  for (const auto &chunk : chunks)
  {
    std::array<uint32_t, 3> physical{};
    if (!sparseVoxelTree64DirectorySlot(
            chunk, state.originWorldChunk, state.logicalChunkResolution,
            state.wrapOffset, physical))
      return fail(error, "Tree64 stable terrain chunk mapping failed");
    const uint32_t index =
        physicalIndex(physical, state.logicalChunkResolution);
    bool rewrite = committed.memberships[index].worldChunk != chunk;
    if (!rewrite)
      for (uint32_t slot = 0u; slot < changedInstances.size(); ++slot)
        if (changedInstances[slot] &&
            ((oldBySlot[slot] != nullptr &&
              intersects(state, chunk, oldBySlot[slot]->worldBounds)) ||
             (instanceBySlot[slot] != nullptr &&
              intersects(state, chunk, instanceBySlot[slot]->worldBounds))))
        {
          rewrite = true;
          break;
        }
    if (!rewrite)
      continue;

    MembershipWrite write{};
    write.index = index;
    write.references.assign(settings.maximumReferencesPerChunk, 0u);
    uint32_t count = 0u;
    for (uint32_t slot = 0u; slot < instanceBySlot.size(); ++slot)
      if (instanceBySlot[slot] != nullptr &&
          intersects(state, chunk, instanceBySlot[slot]->worldBounds))
      {
        if (count == settings.maximumReferencesPerChunk)
          return fail(error, "Tree64 stable terrain membership exceeded its reference slice");
        write.references[count++] = slot;
      }
    write.membership = {
      chunk, index * settings.maximumReferencesPerChunk, count,
      committed.contentGeneration, kSparseVoxelTree64DynamicValid, 0u};
    if (committed.memberships[index].worldChunk != chunk)
    {
      update.changedWorldChunks.push_back(
          committed.memberships[index].worldChunk);
      update.changedWorldChunks.push_back(chunk);
    }
    writes.push_back(std::move(write));
  }
  std::sort(writes.begin(), writes.end(),
            [](const auto &left, const auto &right)
            {
              return left.index < right.index;
            });
  for (const auto &write : writes)
  {
    appendRangeValue(
        update.membershipRanges, write.index, write.membership);
    const uint32_t firstReference =
        write.index * settings.maximumReferencesPerChunk;
    if (update.referenceRanges.empty() ||
        uint64_t{update.referenceRanges.back().first} +
                update.referenceRanges.back().values.size() !=
            firstReference)
      update.referenceRanges.push_back({firstReference, {}});
    update.referenceRanges.back().values.insert(
        update.referenceRanges.back().values.end(),
        write.references.begin(), write.references.end());
  }

  update.sparseTerrainUpdate = true;
  update.snapshot.contentGeneration = committed.contentGeneration;
  update.snapshot.sceneRevision = sceneRevision;
  std::sort(update.changedWorldChunks.begin(),
            update.changedWorldChunks.end());
  update.changedWorldChunks.erase(
      std::unique(update.changedWorldChunks.begin(),
                  update.changedWorldChunks.end()),
      update.changedWorldChunks.end());
  return true;
}

std::array<double, 3> transformPoint(const std::array<float, 16> &matrix, const std::array<double, 3> &point)
{
  return {
    matrix[0] * point[0] + matrix[4] * point[1] + matrix[8] * point[2] + matrix[12],
    matrix[1] * point[0] + matrix[5] * point[1] + matrix[9] * point[2] + matrix[13],
    matrix[2] * point[0] + matrix[6] * point[1] + matrix[10] * point[2] + matrix[14],
  };
}

std::array<double, 3> transformVector(const std::array<float, 16> &matrix, const std::array<double, 3> &vector)
{
  return {
    matrix[0] * vector[0] + matrix[4] * vector[1] + matrix[8] * vector[2],
    matrix[1] * vector[0] + matrix[5] * vector[1] + matrix[9] * vector[2],
    matrix[2] * vector[0] + matrix[6] * vector[1] + matrix[10] * vector[2],
  };
}

} // namespace

bool buildDynamicSnapshotInternal(
    const SparseVoxelTree64StateGPU &state,
    const SparseVoxelTree64DynamicSettings &settings,
    const uint32_t contentGeneration,
    const uint64_t sceneRevision,
    const std::vector<SparseVoxelTree64DynamicModelSource> &models,
    const std::vector<SparseVoxelTree64DynamicInstanceSource> &instances,
    const SparseVoxelTree64DynamicSnapshot *sharedModelPayload,
    const MembershipBuild membershipBuild,
    SparseVoxelTree64DynamicSnapshot &snapshot,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  snapshot = {};
  if (!validState(state) || contentGeneration == 0u)
    return fail(error, "Tree64 dynamic state or generation is invalid");
  const uint64_t activeCount = membershipCount(state);
  if (activeCount > settings.membershipCapacity)
    return fail(error, "Tree64 dynamic membership capacity cannot represent the active directory");

  uint32_t previousModelId = 0u;
  std::set<uint32_t> physicalNodeSlots;
  bool first = true;
  uint32_t sequentialPhysicalNodeSlot = 0u;
  for (const auto &model : models)
  {
    if ((!first && model.modelId <= previousModelId) ||
        !sparseVoxelTree64SupportedModelSide(model.side) ||
        !validBounds(model.localBounds, true))
      return fail(error, "Tree64 dynamic model sources are invalid or unsorted");
    if (settings.modelNodeSlotCapacity != 0u)
    {
      const uint32_t physicalNodeSlot =
          model.physicalNodeSlot != std::numeric_limits<uint32_t>::max()
          ? model.physicalNodeSlot
          : sequentialPhysicalNodeSlot;
      if (physicalNodeSlot >= settings.modelCapacity ||
          !physicalNodeSlots.insert(physicalNodeSlot).second)
        return fail(error, "Tree64 dynamic physical model slots are invalid or duplicated");
    }
    for (const auto &leaf : model.localLeafBounds)
      if (!validBounds(leaf, false))
        return fail(error, "Tree64 dynamic model leaf bounds are invalid");
    previousModelId = model.modelId;
    first = false;
    ++sequentialPhysicalNodeSlot;
  }
  uint32_t previousInstanceId = 0u;
  first = true;
  for (const auto &instance : instances)
  {
    if ((!first && instance.stableInstanceId <= previousInstanceId) ||
        (instance.flags & ~kSparseVoxelTree64DynamicSourceFlagMask) != 0u ||
        !validMatrix(instance.worldToModel) ||
        !validBounds(instance.worldBounds, true))
      return fail(error, "Tree64 dynamic instance sources are invalid or unsorted");
    previousInstanceId = instance.stableInstanceId;
    first = false;
  }

  snapshot.contentGeneration = contentGeneration;
  snapshot.sceneRevision = sceneRevision;
  std::map<uint32_t, uint32_t> modelIndices;
  if (sharedModelPayload != nullptr)
  {
    if (sharedModelPayload->models.size() != models.size())
      return fail(error, "Tree64 shared dynamic model payload is stale");
    for (uint32_t index = 0u; index < sharedModelPayload->models.size(); ++index)
    {
      if (sharedModelPayload->models[index].modelId != models[index].modelId)
        return fail(error, "Tree64 shared dynamic model identity is stale");
      modelIndices[sharedModelPayload->models[index].modelId] = index;
    }
  }
  else
  {
    for (const auto &source : models)
    {
      SparseVoxelTree64DynamicModelGPU model{};
      model.localMinimum = source.localBounds.minimum;
      const uint32_t physicalNodeSlot =
          source.physicalNodeSlot != std::numeric_limits<uint32_t>::max()
          ? source.physicalNodeSlot
          : static_cast<uint32_t>(snapshot.models.size());
      const uint64_t nodeOffset = settings.modelNodeSlotCapacity != 0u
          ? uint64_t{physicalNodeSlot} * settings.modelNodeSlotCapacity
          : snapshot.nodes.size();
      if (nodeOffset > std::numeric_limits<uint32_t>::max())
        return fail(error, "Tree64 dynamic physical model slot exceeds 32-bit addressing");
      model.nodeOffset = static_cast<uint32_t>(nodeOffset);
      model.side = source.side;
      model.modelId = source.modelId;
      model.flags = kSparseVoxelTree64DynamicValid;
      std::vector<SparseVoxelTree64AABB> local;
      local.reserve(source.localLeafBounds.size());
      for (uint32_t axis = 0u; axis < 3u; ++axis)
        model.localCellSize[axis] = (source.localBounds.maximum[axis] - source.localBounds.minimum[axis]) / static_cast<float>(source.side);
      for (const auto &leaf : source.localLeafBounds)
      {
        SparseVoxelTree64AABB treeBounds{};
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
          treeBounds.minimum[axis] = (leaf.minimum[axis] - model.localMinimum[axis]) / model.localCellSize[axis];
          treeBounds.maximum[axis] = (leaf.maximum[axis] - model.localMinimum[axis]) / model.localCellSize[axis];
        }
        local.push_back(treeBounds);
      }
      SparseVoxelTree64Chunk tree{};
      tree.side = source.side;
      if (!source.prebuiltNodes.empty())
      {
        tree.nodes = source.prebuiltNodes;
        if (!validateSparseVoxelTree64Model(tree, error))
          return false;
      }
      else if (!buildSparseVoxelTree64Model(
                   source.side, local, tree, error))
        return false;
      model.rootNodeOffset = 0u;
      model.nodeCount = static_cast<uint32_t>(tree.nodes.size());
      if (settings.modelNodeSlotCapacity != 0u &&
          model.nodeCount > settings.modelNodeSlotCapacity)
        return fail(
            error, "Tree64 dynamic model exceeded its fixed node slot");
      modelIndices[source.modelId] = static_cast<uint32_t>(snapshot.models.size());
      snapshot.models.push_back(model);
      if (settings.modelNodeSlotCapacity != 0u)
      {
        snapshot.nodes.resize(
            static_cast<size_t>(model.nodeOffset) +
            settings.modelNodeSlotCapacity);
        std::copy(
            tree.nodes.begin(), tree.nodes.end(),
            snapshot.nodes.begin() + model.nodeOffset);
      }
      else
        snapshot.nodes.insert(
            snapshot.nodes.end(), tree.nodes.begin(), tree.nodes.end());
    }
  }

  for (const auto &source : instances)
  {
    const auto model = modelIndices.find(source.modelId);
    if (model == modelIndices.end())
      return fail(error, "Tree64 dynamic instance references a missing model");
    SparseVoxelTree64DynamicInstanceGPU instance{};
    instance.worldToModel = source.worldToModel;
    instance.worldBoundsMinimum = {
      source.worldBounds.minimum[0],
      source.worldBounds.minimum[1],
      source.worldBounds.minimum[2],
      0.0f,
    };
    instance.worldBoundsMaximum = {
      source.worldBounds.maximum[0],
      source.worldBounds.maximum[1],
      source.worldBounds.maximum[2],
      0.0f,
    };
    instance.modelIndex = model->second;
    instance.stableInstanceId = source.stableInstanceId;
    instance.contentGeneration = contentGeneration;
    instance.flags = kSparseVoxelTree64DynamicValid | source.flags;
    instance.terrainSlot = source.terrainSlot;
    instance.terrainGeneration = source.terrainGeneration;
    instance.regionMappingId = source.regionMappingId;
    static_assert(
        sizeof(instance.receiverCellSizeBits) ==
        sizeof(source.receiverCellSizeMetres));
    std::memcpy(
        &instance.receiverCellSizeBits, &source.receiverCellSizeMetres,
        sizeof(instance.receiverCellSizeBits));
    snapshot.instances.push_back(instance);
  }

  if (snapshot.models.size() > settings.modelCapacity || snapshot.instances.size() > settings.instanceCapacity || snapshot.nodes.size() > settings.nodeCapacity)
  {
    makeUnknownSnapshot(state, contentGeneration, sceneRevision, snapshot);
    return true;
  }

  const auto chunks = activeChunks(state);
  std::vector<std::vector<uint32_t>> chunkReferences(static_cast<size_t>(activeCount));
  if (!buildChunkReferences(state, instances, chunks, membershipBuild, chunkReferences, error))
    return false;

  uint64_t referenceCount = 0u;
  for (const auto &references : chunkReferences)
  {
    referenceCount += references.size();
    if (references.size() > settings.maximumReferencesPerChunk)
    {
      makeUnknownSnapshot(state, contentGeneration, sceneRevision, snapshot);
      return true;
    }
  }
  if (referenceCount > settings.referenceCapacity)
  {
    makeUnknownSnapshot(state, contentGeneration, sceneRevision, snapshot);
    return true;
  }

  snapshot.memberships.resize(static_cast<size_t>(activeCount));
  snapshot.references.reserve(static_cast<size_t>(referenceCount));
  for (const auto &chunk : chunks)
  {
    std::array<uint32_t, 3> physical{};
    if (!sparseVoxelTree64DirectorySlot(chunk, state.originWorldChunk, state.logicalChunkResolution, state.wrapOffset, physical))
      return fail(error, "Tree64 dynamic chunk mapping failed");
    const uint32_t index = physicalIndex(physical, state.logicalChunkResolution);
    const auto &references = chunkReferences[index];
    snapshot.memberships[index] = {
      chunk,
      static_cast<uint32_t>(snapshot.references.size()),
      static_cast<uint32_t>(references.size()),
      contentGeneration,
      kSparseVoxelTree64DynamicValid,
      0u,
    };
    snapshot.references.insert(snapshot.references.end(), references.begin(), references.end());
  }
  return true;
}

bool sparseVoxelTree64BuildDynamicSnapshot(
    const SparseVoxelTree64StateGPU &state,
    const SparseVoxelTree64DynamicSettings &settings,
    const uint32_t contentGeneration,
    const uint64_t sceneRevision,
    const std::vector<SparseVoxelTree64DynamicModelSource> &models,
    const std::vector<SparseVoxelTree64DynamicInstanceSource> &instances,
    SparseVoxelTree64DynamicSnapshot &snapshot,
    std::string *error)
{
  return buildDynamicSnapshotInternal(state, settings, contentGeneration, sceneRevision, models, instances, nullptr, MembershipBuild::RangeSpecialized, snapshot, error);
}

bool sparseVoxelTree64BuildDynamicSnapshotBruteForceReference(
    const SparseVoxelTree64StateGPU &state,
    const SparseVoxelTree64DynamicSettings &settings,
    const uint32_t contentGeneration,
    const uint64_t sceneRevision,
    const std::vector<SparseVoxelTree64DynamicModelSource> &models,
    const std::vector<SparseVoxelTree64DynamicInstanceSource> &instances,
    SparseVoxelTree64DynamicSnapshot &snapshot,
    std::string *error)
{
  return buildDynamicSnapshotInternal(state, settings, contentGeneration, sceneRevision, models, instances, nullptr, MembershipBuild::BruteForceReference, snapshot, error);
}

bool sparseVoxelTree64RequiresDynamicTrace(const SparseVoxelTree64DynamicSnapshot &snapshot)
{
  return snapshot.conservativeFallback || !snapshot.instances.empty();
}

std::vector<std::array<int32_t, 3>>
sparseVoxelTree64DynamicChangedChunks(const SparseVoxelTree64StateGPU &state, const std::vector<SparseVoxelTree64DynamicInstanceSource> &previous, const std::vector<SparseVoxelTree64DynamicInstanceSource> &current)
{
  std::vector<SparseVoxelAABB> bounds;
  size_t previousIndex = 0u;
  size_t currentIndex = 0u;
  while (previousIndex < previous.size() || currentIndex < current.size())
  {
    if (currentIndex >= current.size() || (previousIndex < previous.size() && previous[previousIndex].stableInstanceId < current[currentIndex].stableInstanceId))
    {
      bounds.push_back(previous[previousIndex++].worldBounds);
    }
    else if (previousIndex >= previous.size() || current[currentIndex].stableInstanceId < previous[previousIndex].stableInstanceId)
    {
      bounds.push_back(current[currentIndex++].worldBounds);
    }
    else
    {
      if (!sameSource(previous[previousIndex], current[currentIndex]))
      {
        bounds.push_back(previous[previousIndex].worldBounds);
        bounds.push_back(current[currentIndex].worldBounds);
      }
      ++previousIndex;
      ++currentIndex;
    }
  }
  std::vector<std::array<int32_t, 3>> chunks;
  sparseVoxelTree64ClassifyDirtyChunks(state, bounds, true, chunks, nullptr, nullptr);
  return chunks;
}

SparseVoxelTree64TraceResult traceSparseVoxelTree64DynamicInstance(
    const SparseVoxelTree64DynamicModelGPU &model,
    const std::vector<SparseVoxelTree64NodeGPU> &nodes,
    const SparseVoxelTree64DynamicInstanceGPU &instance,
    const SparseVoxelTree64Ray &worldRay,
    const uint32_t maximumQueries)
{
  SparseVoxelTree64TraceResult invalid{};
  if ((model.flags & kSparseVoxelTree64DynamicValid) == 0u || (instance.flags & kSparseVoxelTree64DynamicValid) == 0u || model.nodeCount == 0u || uint64_t{model.nodeOffset} + model.nodeCount > nodes.size())
    return invalid;
  SparseVoxelTree64Chunk tree{};
  tree.side = model.side;
  tree.nodes.assign(nodes.begin() + model.nodeOffset, nodes.begin() + model.nodeOffset + model.nodeCount);
  SparseVoxelTree64Ray localRay{};
  const auto localOrigin = transformPoint(instance.worldToModel, worldRay.origin);
  const auto localDirection = transformVector(instance.worldToModel, worldRay.direction);
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (!(model.localCellSize[axis] > 0.0f))
      return invalid;
    localRay.origin[axis] = (localOrigin[axis] - model.localMinimum[axis]) / model.localCellSize[axis];
    localRay.direction[axis] = localDirection[axis] / model.localCellSize[axis];
  }
  localRay.maximumDistance = worldRay.maximumDistance;
  double receiverAdvance = 0.0;
  if ((instance.flags & kSparseVoxelTree64DynamicSkipReceiverCell) != 0u)
  {
    bool inside = true;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      inside &= localRay.origin[axis] >= 0.0 &&
          localRay.origin[axis] < static_cast<double>(model.side);
    if (inside)
    {
      float requestedReceiverCellSize = 0.0f;
      static_assert(
          sizeof(requestedReceiverCellSize) ==
          sizeof(instance.receiverCellSizeBits));
      std::memcpy(
          &requestedReceiverCellSize, &instance.receiverCellSizeBits,
          sizeof(requestedReceiverCellSize));
      double exit = std::numeric_limits<double>::infinity();
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        const double receiverScale =
            std::isfinite(requestedReceiverCellSize) &&
                requestedReceiverCellSize > 0.0f
            ? static_cast<double>(requestedReceiverCellSize) /
                model.localCellSize[axis]
            : 1.0;
        const double coordinate =
            localRay.origin[axis] / receiverScale;
        const double step =
            localRay.direction[axis] / receiverScale;
        if (std::abs(step) < 1.0e-15)
          continue;
        const double boundary = step > 0.0
            ? std::floor(coordinate) + 1.0
            : std::floor(coordinate);
        exit = std::min(exit, (boundary - coordinate) / step);
      }
      if (std::isfinite(exit))
      {
        receiverAdvance = std::min(
            std::nextafter(std::max(exit, 0.0),
                std::numeric_limits<double>::infinity()),
            localRay.maximumDistance);
        for (uint32_t axis = 0u; axis < 3u; ++axis)
          localRay.origin[axis] +=
              localRay.direction[axis] * receiverAdvance;
        localRay.maximumDistance -= receiverAdvance;
      }
    }
  }
  auto result = traceSparseVoxelTree64(tree, localRay, maximumQueries);
  if (result.status == SparseVoxelTree64TraceStatus::Hit ||
      result.status == SparseVoxelTree64TraceStatus::QueryLimit)
    result.distance += receiverAdvance;
  return result;
}

bool SparseVoxelTree64DynamicLayer::configure(const SparseVoxelTree64DynamicSettings &settings, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (settings.modelCapacity == 0u || settings.instanceCapacity == 0u || settings.membershipCapacity == 0u || settings.referenceCapacity == 0u || settings.nodeCapacity == 0u || settings.maximumReferencesPerChunk == 0u)
    return fail(error, "Tree64 dynamic capacities are invalid");
  if (!settings.stableTerrainSlots &&
      settings.modelNodeSlotCapacity != 0u &&
      uint64_t{settings.modelNodeSlotCapacity} * settings.modelCapacity >
          settings.nodeCapacity)
    return fail(error, "Tree64 fixed model node slots exceed node capacity");
  if (settings.stableTerrainSlots && settings.modelNodeSlotCapacity == 0u)
    return fail(error, "Tree64 stable terrain slots require fixed model node slots");
  if (settings.stableTerrainSlots &&
      (settings.modelCapacity != settings.instanceCapacity ||
       uint64_t{settings.membershipCapacity} *
               settings.maximumReferencesPerChunk >
           settings.referenceCapacity))
    return fail(
        error,
        "Tree64 stable terrain slot and reference capacities are incompatible");
  settings_ = settings;
  committed_ = {};
  committedModelSources_.clear();
  committedInstanceSources_.clear();
  committedState_ = {};
  pending_.reset();
  nextTransaction_ = 1u;
  stats_ = {};
  configured_ = true;
  initialized_ = false;
  return true;
}

bool SparseVoxelTree64DynamicLayer::initialize(
    const SparseVoxelTree64StateGPU &state,
    const uint64_t sceneRevision,
    const std::vector<SparseVoxelTree64DynamicModelSource> &models,
    const std::vector<SparseVoxelTree64DynamicInstanceSource> &instances,
    std::string *error)
{
  if (!configured_)
    return fail(error, "Tree64 dynamic layer is not configured");
  SparseVoxelTree64DynamicSnapshot snapshot;
  const bool built = settings_.stableTerrainSlots
      ? buildStableTerrainSnapshot(
            state, settings_, sceneRevision, models, instances, nullptr,
            nullptr, snapshot, error)
      : sparseVoxelTree64BuildDynamicSnapshot(
            state, settings_, 1u, sceneRevision, models, instances,
            snapshot, error);
  if (!built)
    return false;
  committed_ = std::move(snapshot);
  committedModelSources_ = models;
  if (settings_.stableTerrainSlots)
    for (auto &source : committedModelSources_)
    {
      source.localLeafBounds.clear();
      source.prebuiltNodes.clear();
    }
  committedInstanceSources_ = instances;
  committedState_ = state;
  pending_.reset();
  nextTransaction_ = 1u;
  initialized_ = true;
  refreshStats();
  return true;
}

bool SparseVoxelTree64DynamicLayer::prepare(
    const SparseVoxelTree64StateGPU &state,
    const uint64_t sceneRevision,
    const std::vector<SparseVoxelTree64DynamicModelSource> &models,
    const std::vector<SparseVoxelTree64DynamicInstanceSource> &instances,
    SparseVoxelTree64DynamicUpdate &update,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  update = {};
  if (!initialized_)
    return fail(error, "Tree64 dynamic layer is not initialized");
  if (pending_.has_value())
    return fail(error, "Tree64 dynamic layer already has a pending update");
  if (sceneRevision < committed_.sceneRevision)
    return fail(error, "Tree64 dynamic scene revision is stale");
  if ((!settings_.stableTerrainSlots &&
       committed_.contentGeneration == std::numeric_limits<uint32_t>::max()) ||
      nextTransaction_ == std::numeric_limits<uint64_t>::max())
    return fail(error, "Tree64 dynamic generation space is exhausted");
  update.transaction = nextTransaction_++;
  update.previousSceneRevision = committed_.sceneRevision;
  update.targetSceneRevision = sceneRevision;
  update.changedWorldChunks = sparseVoxelTree64DynamicChangedChunks(state, committedInstanceSources_, instances);
  if (settings_.stableTerrainSlots)
  {
    update.modelPayloadChanged = false;
    if (!buildStableTerrainUpdate(
            state, settings_, sceneRevision, models, instances, committed_,
            committedInstanceSources_, update, error))
      return false;
  }
  else
  {
    update.modelPayloadChanged = !sameModels(committedModelSources_, models) || !matchesModelPayload(models, committed_);
    if (!buildDynamicSnapshotInternal(
            state, settings_, committed_.contentGeneration + 1u,
            sceneRevision, models, instances,
            update.modelPayloadChanged ? nullptr : &committed_,
            MembershipBuild::RangeSpecialized, update.snapshot, error))
      return false;
  }
  pending_ = Pending{update, models, instances, state};
  if (settings_.stableTerrainSlots)
    for (auto &source : pending_->modelSources)
    {
      source.localLeafBounds.clear();
      source.prebuiltNodes.clear();
    }
  ++stats_.preparedUpdates;
  if (update.snapshot.conservativeFallback)
    ++stats_.conservativeFallbacks;
  if (update.modelPayloadChanged)
    ++stats_.modelPayloadUpdates;
  else
    ++stats_.transformOnlyUpdates;
  return true;
}

bool SparseVoxelTree64DynamicLayer::commit(const uint64_t sceneRevision, const SparseVoxelTree64DynamicUpdate &update, std::string *error)
{
  if (!validateCommit(sceneRevision, update, error))
  {
    if (pending_.has_value() &&
        pending_->update.transaction == update.transaction &&
        sceneRevision != pending_->update.targetSceneRevision)
      ++stats_.staleCommits;
    return false;
  }
  static const bool traceCommit = std::getenv("SVR_TRACE_TERRAIN") != nullptr;
  const auto tC0 = std::chrono::steady_clock::now();
  auto tCMoved = tC0;
  auto tCFreed = tC0;
  if (pending_->update.sparseTerrainUpdate)
  {
    const auto apply = [](auto &destination, const auto &ranges)
    {
      for (const auto &range : ranges)
        std::copy(range.values.begin(), range.values.end(),
                  destination.begin() + range.first);
    };
    apply(committed_.models, pending_->update.modelRanges);
    apply(committed_.instances, pending_->update.instanceRanges);
    apply(committed_.memberships, pending_->update.membershipRanges);
    apply(committed_.references, pending_->update.referenceRanges);
    committed_.sceneRevision = pending_->update.targetSceneRevision;
    committed_.contentGeneration =
        pending_->update.snapshot.contentGeneration;
    committed_.conservativeFallback = false;
    tCMoved = std::chrono::steady_clock::now();
    tCFreed = tCMoved;
  }
  else
  {
    std::vector<SparseVoxelTree64DynamicModelGPU> retainedModels;
    std::vector<SparseVoxelTree64NodeGPU> retainedNodes;
    const bool retainModels = pending_->update.snapshot.models.empty();
    const bool retainNodes = pending_->update.snapshot.nodes.empty();
    if (retainModels)
      retainedModels = std::move(committed_.models);
    if (retainNodes)
      retainedNodes = std::move(committed_.nodes);
    // The previous snapshot's arrays are freed here, on the caller's thread.
    SparseVoxelTree64DynamicSnapshot retired = std::move(committed_);
    committed_ = std::move(pending_->update.snapshot);
    if (retainModels)
      committed_.models = std::move(retainedModels);
    if (retainNodes)
      committed_.nodes = std::move(retainedNodes);
    tCMoved = std::chrono::steady_clock::now();
    retired = SparseVoxelTree64DynamicSnapshot{};
    tCFreed = std::chrono::steady_clock::now();
  }
  // The previous sources carry every model's leaf bounds and prebuilt nodes;
  // freeing them here cost 28-54 ms on the render thread per transaction.
  // They are parked for the caller to release off-thread.
  retiredModelSources_ = std::move(committedModelSources_);
  retiredInstanceSources_ = std::move(committedInstanceSources_);
  committedModelSources_ = std::move(pending_->modelSources);
  committedInstanceSources_ = std::move(pending_->instanceSources);
  committedState_ = pending_->state;
  const auto tCPending0 = std::chrono::steady_clock::now();
  pending_.reset();
  const auto tCPending = std::chrono::steady_clock::now();
  ++stats_.committedUpdates;
  refreshStats();
  if (traceCommit)
  {
    const auto ms = [](const auto a, const auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    std::fprintf(stderr, "Tree64 layer commit: sparse=%d move=%.1fms free=%.1fms sources=%.1fms pendingReset=%.1fms stats=%.1fms nodes=%zu instances=%zu\n",
        pending_.has_value() ? -1 : (committed_.conservativeFallback ? 0 : 1), ms(tC0, tCMoved), ms(tCMoved, tCFreed), ms(tCFreed, tCPending0),
        ms(tCPending0, tCPending), ms(tCPending, std::chrono::steady_clock::now()), committed_.nodes.size(), committed_.instances.size());
  }
  return true;
}

bool SparseVoxelTree64DynamicLayer::validateCommit(
    const uint64_t sceneRevision,
    const SparseVoxelTree64DynamicUpdate &update,
    std::string *error) const
{
  if (error != nullptr)
    error->clear();
  if (!pending_.has_value() ||
      pending_->update.transaction != update.transaction)
    return fail(error, "Tree64 dynamic transaction is stale");
  if (sceneRevision != pending_->update.targetSceneRevision)
    return fail(error, "Tree64 dynamic scene changed before publication");
  return true;
}

bool SparseVoxelTree64DynamicLayer::abort(const SparseVoxelTree64DynamicUpdate &update, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!pending_.has_value() || pending_->update.transaction != update.transaction)
    return fail(error, "Tree64 dynamic transaction is stale");
  pending_.reset();
  return true;
}

bool SparseVoxelTree64DynamicLayer::configured() const
{
  return configured_;
}

bool SparseVoxelTree64DynamicLayer::initialized() const
{
  return initialized_;
}

bool SparseVoxelTree64DynamicLayer::hasPendingUpdate() const
{
  return pending_.has_value();
}

const SparseVoxelTree64DynamicSnapshot &SparseVoxelTree64DynamicLayer::committed() const
{
  return committed_;
}

SparseVoxelTree64DynamicStats SparseVoxelTree64DynamicLayer::stats() const
{
  return stats_;
}

void SparseVoxelTree64DynamicLayer::refreshStats()
{
  stats_.models = static_cast<uint32_t>(committed_.models.size());
  stats_.instances = static_cast<uint32_t>(committed_.instances.size());
  stats_.memberships = static_cast<uint32_t>(committed_.memberships.size());
  stats_.references = static_cast<uint32_t>(committed_.references.size());
  stats_.nodes = static_cast<uint32_t>(committed_.nodes.size());
  stats_.maximumReferencesInChunk = 0u;
  std::map<uint32_t, uint32_t> modelUse;
  for (const auto &instance : committed_.instances)
    ++modelUse[instance.modelIndex];
  stats_.sharedModelInstances = 0u;
  for (const auto &[model, count] : modelUse)
  {
    static_cast<void>(model);
    if (count > 1u)
      stats_.sharedModelInstances += count;
  }
  for (const auto &membership : committed_.memberships)
    stats_.maximumReferencesInChunk = std::max(stats_.maximumReferencesInChunk, membership.referenceCount);
}

bool SparseVoxelTree64DynamicPolicyTracker::initialize(const std::vector<std::pair<uint32_t, SparseVoxelTree64InstancePolicy>> &policies, std::string *error)
{
  if (error != nullptr)
    error->clear();
  policies_.clear();
  pending_.reset();
  nextTransaction_ = 1u;
  uint32_t previous = 0u;
  bool first = true;
  for (const auto &[instance, policy] : policies)
  {
    if ((!first && instance <= previous) || (policy != SparseVoxelTree64InstancePolicy::Baked && policy != SparseVoxelTree64InstancePolicy::Dynamic))
    {
      policies_.clear();
      return fail(error, "Tree64 dynamic policies are invalid or unsorted");
    }
    policies_[instance] = policy;
    previous = instance;
    first = false;
  }
  return true;
}

bool SparseVoxelTree64DynamicPolicyTracker::prepare(
    const SparseVoxelTree64StateGPU &state,
    const uint32_t stableInstanceId,
    const SparseVoxelTree64InstancePolicy targetPolicy,
    const SparseVoxelAABB &previousBounds,
    const SparseVoxelAABB &currentBounds,
    SparseVoxelTree64MigrationUpdate &update,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  update = {};
  if (pending_.has_value())
    return fail(error, "Tree64 dynamic migration is already pending");
  const auto currentPolicy = policies_.find(stableInstanceId);
  if (currentPolicy == policies_.end())
    return fail(error, "Tree64 dynamic migration instance is unknown");
  if (targetPolicy != SparseVoxelTree64InstancePolicy::Baked && targetPolicy != SparseVoxelTree64InstancePolicy::Dynamic)
    return fail(error, "Tree64 dynamic migration target policy is invalid");
  if (currentPolicy->second == targetPolicy)
    return fail(error, "Tree64 dynamic migration policy is unchanged");
  if (!validBounds(previousBounds, true) || !validBounds(currentBounds, true) || !validState(state))
    return fail(error, "Tree64 dynamic migration bounds are invalid");
  if (nextTransaction_ == std::numeric_limits<uint64_t>::max())
    return fail(error, "Tree64 dynamic migration space is exhausted");
  update.transaction = nextTransaction_++;
  update.stableInstanceId = stableInstanceId;
  update.previousPolicy = currentPolicy->second;
  update.targetPolicy = targetPolicy;
  update.phase = SparseVoxelTree64MigrationPhase::Prepared;
  if (!sparseVoxelTree64ClassifyDirtyChunks(state, {previousBounds, currentBounds}, true, update.bakedInvalidationChunks, nullptr, error))
    return false;
  pending_ = update;
  return true;
}

bool SparseVoxelTree64DynamicPolicyTracker::destinationPublished(SparseVoxelTree64MigrationUpdate &update, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!pending_.has_value() || pending_->transaction != update.transaction || pending_->phase != SparseVoxelTree64MigrationPhase::Prepared || update.phase != SparseVoxelTree64MigrationPhase::Prepared)
    return fail(error, "Tree64 dynamic migration destination publication is out of order");
  pending_->phase = SparseVoxelTree64MigrationPhase::DestinationPublished;
  update.phase = SparseVoxelTree64MigrationPhase::DestinationPublished;
  return true;
}

bool SparseVoxelTree64DynamicPolicyTracker::sourceRemoved(SparseVoxelTree64MigrationUpdate &update, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!pending_.has_value() || pending_->transaction != update.transaction || pending_->phase != SparseVoxelTree64MigrationPhase::DestinationPublished ||
      update.phase != SparseVoxelTree64MigrationPhase::DestinationPublished)
    return fail(error, "Tree64 dynamic migration source removal is out of order");
  pending_->phase = SparseVoxelTree64MigrationPhase::SourceRemoved;
  update.phase = SparseVoxelTree64MigrationPhase::SourceRemoved;
  return true;
}

bool SparseVoxelTree64DynamicPolicyTracker::commit(const SparseVoxelTree64MigrationUpdate &update, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!pending_.has_value() || pending_->transaction != update.transaction || pending_->phase != SparseVoxelTree64MigrationPhase::SourceRemoved || update.phase != SparseVoxelTree64MigrationPhase::SourceRemoved)
    return fail(error, "Tree64 dynamic migration is incomplete");
  policies_[pending_->stableInstanceId] = pending_->targetPolicy;
  pending_.reset();
  return true;
}

bool SparseVoxelTree64DynamicPolicyTracker::abort(const SparseVoxelTree64MigrationUpdate &update, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!pending_.has_value() || pending_->transaction != update.transaction)
    return fail(error, "Tree64 dynamic migration transaction is stale");
  if (pending_->phase != SparseVoxelTree64MigrationPhase::Prepared)
    return fail(error, "Tree64 dynamic migration cannot roll back after destination publication");
  pending_.reset();
  return true;
}

std::optional<SparseVoxelTree64InstancePolicy> SparseVoxelTree64DynamicPolicyTracker::policy(const uint32_t stableInstanceId) const
{
  const auto found = policies_.find(stableInstanceId);
  if (found == policies_.end())
    return {};
  return found->second;
}

bool SparseVoxelTree64DynamicPolicyTracker::hasPendingMigration() const
{
  return pending_.has_value();
}

} // namespace voxel::svraster
