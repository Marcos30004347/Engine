#include "voxel/svraster/SparseVoxelTerrainTree64.hpp"
#include "voxel/svraster/SparseVoxelStreaming.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace voxel::svraster
{
namespace
{
bool fail(std::string *error, const char *message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

// One occupied run in page-voxel lattice coordinates. Half open on every axis.
struct VoxelRun
{
  std::array<int32_t, 3> minimum{};
  std::array<int32_t, 3> maximum{};
};

constexpr uint64_t kTerrainTree64VirtualPagePayloadMagic =
    0x5454363456505031ull;

template <typename T>
void appendValue(std::vector<uint8_t> &bytes, const T &value)
{
  const size_t offset = bytes.size();
  bytes.resize(offset + sizeof(T));
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

template <typename T>
void appendValues(std::vector<uint8_t> &bytes, const std::vector<T> &values)
{
  appendValue(bytes, static_cast<uint64_t>(values.size()));
  if (values.empty())
    return;
  const size_t offset = bytes.size();
  bytes.resize(offset + values.size() * sizeof(T));
  std::memcpy(bytes.data() + offset, values.data(), values.size() * sizeof(T));
}

struct TerrainTree64PayloadReader
{
  const std::vector<uint8_t> &bytes;
  size_t offset = 0u;

  template <typename T>
  bool read(T &value)
  {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
      return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
  }

  template <typename T>
  bool readValues(std::vector<T> &values)
  {
    uint64_t count = 0u;
    if (!read(count) ||
        count >
            static_cast<uint64_t>(bytes.size() - offset) / sizeof(T))
      return false;
    values.resize(static_cast<size_t>(count));
    if (values.empty())
      return true;
    std::memcpy(values.data(), bytes.data() + offset,
        values.size() * sizeof(T));
    offset += values.size() * sizeof(T);
    return true;
  }
};

} // namespace

uint32_t selectSparseVoxelTerrainTree64LODVariant(
    const std::vector<SparseVoxelTerrainTree64LODVariant> &variants,
    const uint32_t logicalPageId,
    const uint64_t geometryRevision,
    const float projectedErrorPixels,
    const bool requireResident)
{
  const float error =
      std::isfinite(projectedErrorPixels) && projectedErrorPixels > 0.0f
      ? projectedErrorPixels
      : 0.0f;
  uint32_t best = std::numeric_limits<uint32_t>::max();
  for (uint32_t index = 0u; index < variants.size(); ++index)
  {
    const auto &candidate = variants[index];
    if (candidate.logicalPageId != logicalPageId ||
        candidate.geometryRevision != geometryRevision ||
        candidate.maximumProjectedErrorPixels < error)
      continue;
    if (requireResident &&
        candidate.state != SparseVoxelTerrainTree64LODVariantState::Resident)
      continue;
    if (best == std::numeric_limits<uint32_t>::max() ||
        candidate.internalLOD > variants[best].internalLOD ||
        (candidate.internalLOD == variants[best].internalLOD &&
            candidate.payloadBytes < variants[best].payloadBytes) ||
        (candidate.internalLOD == variants[best].internalLOD &&
            candidate.payloadBytes == variants[best].payloadBytes &&
            index < best))
      best = index;
  }
  return best;
}

namespace
{

bool hasSparseVoxelTerrainTree64GenerationMismatch(
    const std::vector<SparseVoxelTerrainTree64LODVariant> &variants,
    const uint32_t logicalPageId,
    const uint64_t geometryRevision)
{
  bool foundLogicalPage = false;
  for (const auto &variant : variants)
  {
    if (variant.logicalPageId != logicalPageId)
      continue;
    foundLogicalPage = true;
    if (variant.geometryRevision == geometryRevision)
      return false;
  }
  return foundLogicalPage;
}

bool selectSparseVoxelTerrainTree64ConservativeFallback(
    const std::vector<SparseVoxelTerrainTree64LODVariant> &variants,
    const uint32_t logicalPageId,
    const uint64_t geometryRevision,
    const float projectedErrorPixels,
    uint32_t &index)
{
  index = selectSparseVoxelTerrainTree64LODVariant(
      variants, logicalPageId, geometryRevision, projectedErrorPixels, true);
  return index != std::numeric_limits<uint32_t>::max() &&
      variants[index].conservative;
}

void populateSparseVoxelTerrainTree64LightingSelection(
    const SparseVoxelTerrainTree64LODVariant &variant,
    const uint32_t index,
    const SparseVoxelTerrainTree64LightingSelectionStatus status,
    const bool requestedDesiredSidecar,
    const bool usedFallback,
    SparseVoxelTerrainTree64LightingSelection &selection)
{
  selection.status = status;
  selection.selectedVariantIndex = index;
  selection.selectedLogicalPageId = variant.logicalPageId;
  selection.selectedGeometryRevision = variant.geometryRevision;
  selection.selectedInternalLOD = variant.internalLOD;
  selection.traceable = true;
  selection.requestedDesiredSidecar = requestedDesiredSidecar;
  selection.usedFallback = usedFallback;
}

} // namespace

bool selectSparseVoxelTerrainTree64LightingSidecar(
    const std::vector<SparseVoxelTerrainTree64LODVariant> &variants,
    const SparseVoxelTerrainTree64LightingSelectionRequest &request,
    SparseVoxelTerrainTree64LightingSelection &selection,
    SparseVoxelCutDiagnostics *diagnostics,
    std::string *error)
{
  selection = {};
  selection.requestedLogicalPageId = request.logicalPageId;
  selection.requestedGeometryRevision = request.geometryRevision;
  if (error != nullptr)
    error->clear();
  if (request.geometryRevision == 0u ||
      !std::isfinite(request.projectedErrorPixels))
    return fail(error, "Terrain Tree64 lighting sidecar request is invalid");

  selection.desiredVariantIndex = selectSparseVoxelTerrainTree64LODVariant(
      variants,
      request.logicalPageId,
      request.geometryRevision,
      request.projectedErrorPixels,
      false);
  const bool mismatchedGeneration =
      selection.desiredVariantIndex == std::numeric_limits<uint32_t>::max() &&
      hasSparseVoxelTerrainTree64GenerationMismatch(
          variants, request.logicalPageId, request.geometryRevision);
  bool recordedMissingDesired = false;

  if (selection.desiredVariantIndex !=
      std::numeric_limits<uint32_t>::max())
  {
    const auto &desired = variants[selection.desiredVariantIndex];
    if (desired.state == SparseVoxelTerrainTree64LODVariantState::Resident)
    {
      populateSparseVoxelTerrainTree64LightingSelection(
          desired,
          selection.desiredVariantIndex,
          SparseVoxelTerrainTree64LightingSelectionStatus::ResidentSidecar,
          false,
          false,
          selection);
      return true;
    }
    selection.requestedDesiredSidecar = true;
    if (diagnostics != nullptr)
    {
      ++diagnostics->tree64LightingMissCount;
      recordedMissingDesired = true;
    }
  }

  if (request.allowConservativeFallback)
  {
    uint32_t fallback = std::numeric_limits<uint32_t>::max();
    if (selectSparseVoxelTerrainTree64ConservativeFallback(
            variants,
            request.logicalPageId,
            request.geometryRevision,
            request.projectedErrorPixels,
            fallback))
    {
      populateSparseVoxelTerrainTree64LightingSelection(
          variants[fallback],
          fallback,
          SparseVoxelTerrainTree64LightingSelectionStatus::
              ConservativeFallback,
          selection.requestedDesiredSidecar ||
              selection.desiredVariantIndex ==
                  std::numeric_limits<uint32_t>::max(),
          true,
          selection);
      return true;
    }
    if (request.fallbackGeometryRevision != 0u &&
        selectSparseVoxelTerrainTree64ConservativeFallback(
            variants,
            request.fallbackLogicalPageId,
            request.fallbackGeometryRevision,
            request.fallbackProjectedErrorPixels,
            fallback))
    {
      populateSparseVoxelTerrainTree64LightingSelection(
          variants[fallback],
          fallback,
          SparseVoxelTerrainTree64LightingSelectionStatus::
              ConservativeFallback,
          true,
          true,
          selection);
      if (diagnostics != nullptr && !recordedMissingDesired)
      {
        ++diagnostics->tree64LightingMissCount;
        recordedMissingDesired = true;
      }
      return true;
    }
  }

  selection.traceable = false;
  selection.generationMismatch = mismatchedGeneration;
  selection.status = mismatchedGeneration
      ? SparseVoxelTerrainTree64LightingSelectionStatus::GenerationMismatch
      : SparseVoxelTerrainTree64LightingSelectionStatus::MissingDesired;
  if (diagnostics != nullptr)
  {
    if (!recordedMissingDesired)
      ++diagnostics->tree64LightingMissCount;
    if (mismatchedGeneration)
      ++diagnostics->invalidGenerationFallbackCount;
  }
  return true;
}

SparseVoxelTerrainTree64VirtualPagePayload
sparseVoxelTerrainTree64VirtualPayloadFromPagePayload(
    const SparseVoxelTerrainTree64PagePayload &payload)
{
  SparseVoxelTerrainTree64VirtualPagePayload out{};
  out.internalLOD = payload.internalLOD;
  out.regionMappingId = payload.regionMappingId;
  out.geometryRevision = payload.geometryRevision;
  out.modelSide = payload.model.side;
  out.modelLocalBounds = payload.model.localBounds;
  out.modelLocalLeafBounds = payload.model.localLeafBounds;
  out.modelNodes = payload.model.prebuiltNodes;
  out.modelPayloadRevision = payload.model.payloadRevision;
  out.instanceWorldToModel = payload.instance.worldToModel;
  out.instanceWorldBounds = payload.instance.worldBounds;
  out.instanceFlags = payload.instance.flags;
  out.receiverCellSizeMetres = payload.instance.receiverCellSizeMetres;
  out.materialCells = payload.materialCells;
  out.sourceRunCount = payload.sourceRunCount;
  out.clippedRunCount = payload.clippedRunCount;
  out.cellScale = payload.cellScale;
  out.reusedNodes = payload.reusedNodes;
  out.reusedMaterialCells = payload.reusedMaterialCells;
  return out;
}

SparseVoxelTerrainTree64PagePayload
sparseVoxelTerrainTree64VirtualPayloadToPagePayload(
    const SparseVoxelTerrainTree64VirtualPagePayload &payload,
    const uint32_t terrainSlot,
    const uint32_t terrainGeneration)
{
  SparseVoxelTerrainTree64PagePayload out{};
  out.terrainSlot = terrainSlot;
  out.terrainGeneration = terrainGeneration;
  out.internalLOD = payload.internalLOD;
  out.regionMappingId = payload.regionMappingId;
  out.geometryRevision = payload.geometryRevision;
  out.model.modelId = terrainSlot;
  out.model.side = payload.modelSide;
  out.model.localBounds = payload.modelLocalBounds;
  out.model.localLeafBounds = payload.modelLocalLeafBounds;
  out.model.prebuiltNodes = payload.modelNodes;
  out.model.payloadRevision = payload.modelPayloadRevision;
  out.model.physicalNodeSlot = terrainSlot;
  out.instance.stableInstanceId = terrainSlot;
  out.instance.modelId = terrainSlot;
  out.instance.worldToModel = payload.instanceWorldToModel;
  out.instance.worldBounds = payload.instanceWorldBounds;
  out.instance.flags = payload.instanceFlags;
  out.instance.terrainSlot = terrainSlot;
  out.instance.terrainGeneration = terrainGeneration;
  out.instance.regionMappingId = payload.regionMappingId;
  out.instance.receiverCellSizeMetres = payload.receiverCellSizeMetres;
  out.materialCells = payload.materialCells;
  out.sourceRunCount = payload.sourceRunCount;
  out.clippedRunCount = payload.clippedRunCount;
  out.cellScale = payload.cellScale;
  out.reusedNodes = payload.reusedNodes;
  out.reusedMaterialCells = payload.reusedMaterialCells;
  return out;
}

std::vector<uint8_t> serializeSparseVoxelTerrainTree64VirtualPagePayload(
    const SparseVoxelTerrainTree64VirtualPagePayload &payload)
{
  std::vector<uint8_t> bytes;
  appendValue(bytes, kTerrainTree64VirtualPagePayloadMagic);
  appendValue(bytes, payload.internalLOD);
  appendValue(bytes, payload.regionMappingId);
  appendValue(bytes, payload.geometryRevision);
  appendValue(bytes, payload.modelSide);
  appendValue(bytes, payload.modelLocalBounds);
  appendValues(bytes, payload.modelLocalLeafBounds);
  appendValues(bytes, payload.modelNodes);
  appendValue(bytes, payload.modelPayloadRevision);
  appendValue(bytes, payload.instanceWorldToModel);
  appendValue(bytes, payload.instanceWorldBounds);
  appendValue(bytes, payload.instanceFlags);
  appendValue(bytes, payload.receiverCellSizeMetres);
  appendValues(bytes, payload.materialCells);
  appendValue(bytes, payload.sourceRunCount);
  appendValue(bytes, payload.clippedRunCount);
  appendValue(bytes, payload.cellScale);
  const uint8_t reusedNodes = payload.reusedNodes ? 1u : 0u;
  const uint8_t reusedMaterialCells = payload.reusedMaterialCells ? 1u : 0u;
  appendValue(bytes, reusedNodes);
  appendValue(bytes, reusedMaterialCells);
  return bytes;
}

bool deserializeSparseVoxelTerrainTree64VirtualPagePayload(
    const std::vector<uint8_t> &bytes,
    SparseVoxelTerrainTree64VirtualPagePayload &payload,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  TerrainTree64PayloadReader reader{bytes};
  SparseVoxelTerrainTree64VirtualPagePayload out{};
  uint64_t magic = 0u;
  uint8_t reusedNodes = 0u;
  uint8_t reusedMaterialCells = 0u;
  if (!reader.read(magic))
    return fail(error, "Terrain Tree64 virtual page header is truncated");
  if (magic != kTerrainTree64VirtualPagePayloadMagic)
    return fail(error, "Terrain Tree64 virtual page version is unsupported");
  if (!reader.read(out.internalLOD) || !reader.read(out.regionMappingId) ||
      !reader.read(out.geometryRevision) || !reader.read(out.modelSide) ||
      !reader.read(out.modelLocalBounds) ||
      !reader.readValues(out.modelLocalLeafBounds) ||
      !reader.readValues(out.modelNodes) ||
      !reader.read(out.modelPayloadRevision) ||
      !reader.read(out.instanceWorldToModel) ||
      !reader.read(out.instanceWorldBounds) ||
      !reader.read(out.instanceFlags) ||
      !reader.read(out.receiverCellSizeMetres) ||
      !reader.readValues(out.materialCells) ||
      !reader.read(out.sourceRunCount) ||
      !reader.read(out.clippedRunCount) || !reader.read(out.cellScale) ||
      !reader.read(reusedNodes) || !reader.read(reusedMaterialCells) ||
      reader.offset != bytes.size() || out.modelSide == 0u ||
      out.cellScale == 0u || (out.modelNodes.empty() &&
          out.modelLocalLeafBounds.empty()))
    return fail(error, "Terrain Tree64 virtual page payload is corrupt");
  out.reusedNodes = reusedNodes != 0u;
  out.reusedMaterialCells = reusedMaterialCells != 0u;
  payload = std::move(out);
  return true;
}

uint32_t sparseVoxelTerrainTree64SideForCells(const uint32_t cells)
{
  uint32_t side = kSparseVoxelTree64Branching;
  while (side < cells)
  {
    if (side > 65536u / kSparseVoxelTree64Branching)
      return 0u;
    side *= kSparseVoxelTree64Branching;
  }
  return side;
}

bool buildSparseVoxelTerrainTree64PagePayload(
    const SparseVoxelTerrainTree64PagePayloadSource &source,
    SparseVoxelTerrainTree64PagePayload &payload,
    std::string *error)
{
  payload = {};
  if (error != nullptr)
    error->clear();
  if (source.chunks == nullptr ||
      source.chunkIndex >= source.chunks->chunks.size())
    return fail(error, "Terrain Tree64 payload needs a valid chunk record");
  const auto &chunks = *source.chunks;
  const auto &record = chunks.chunks[source.chunkIndex];
  if (record.brickNodeOffset > chunks.bricks.nodes.size() ||
      record.brickNodeCapacity >
          chunks.bricks.nodes.size() - record.brickNodeOffset ||
      record.firstBrickRoot > chunks.bricks.rootIndices.size() ||
      record.brickRootCount >
          chunks.bricks.rootIndices.size() - record.firstBrickRoot ||
      !(record.edgeMetres > 0.0))
    return fail(error, "Terrain Tree64 page ranges are invalid");

  const double pageVoxelSize = record.voxelSizeMetres > 0.0
      ? record.voxelSizeMetres
      : std::ldexp(
            chunks.voxelSizeMetres, static_cast<int>(record.clipmapLOD));
  if (!(pageVoxelSize > 0.0) || !std::isfinite(pageVoxelSize))
    return fail(error, "Terrain Tree64 page voxel size is invalid");
  const double minimumY =
      record.elevationOriginMetres - chunks.elevationOriginMetres;
  const double maximumY = std::max(
      minimumY + pageVoxelSize,
      record.maximumSurfaceMetres - chunks.elevationOriginMetres +
          pageVoxelSize);
  std::array<double, 3> pageMinimum{
    record.worldCenterXMetres - record.edgeMetres * 0.5,
    minimumY,
    record.worldCenterZMetres - record.edgeMetres * 0.5,
  };
  std::array<double, 3> pageMaximum{
    pageMinimum[0] + record.edgeMetres,
    maximumY,
    pageMinimum[2] + record.edgeMetres,
  };
  // Match the raster's canonical-plane expression at streamed page bounds.
  // Independent centre/extent float transforms otherwise leave the shadow
  // page a fraction of a voxel to either side of the visible page, which is
  // enough to self-occlude a receiver at centimetre camera clearance.
  if (chunks.progressiveGeneratedWorld &&
      chunks.canonicalWorldGrid[3] > 0.0f)
  {
    const float spacing = chunks.canonicalWorldGrid[3];
    const int64_t pageCells = std::llround(record.edgeMetres / spacing);
    if (pageCells <= 0)
      return fail(error, "Terrain Tree64 canonical page span is invalid");
    for (const uint32_t axis : {0u, 2u})
    {
      const float origin = chunks.canonicalWorldGrid[axis];
      const int64_t minimumCell = std::llround(
          (pageMinimum[axis] - static_cast<double>(origin)) / spacing);
      const auto canonicalPlane = [&](const int64_t cell)
      {
        return origin + static_cast<float>(cell) * spacing;
      };
      pageMinimum[axis] = canonicalPlane(minimumCell);
      pageMaximum[axis] = canonicalPlane(minimumCell + pageCells);
    }
    const float originY = chunks.canonicalWorldGrid[1];
    const int64_t minimumCellY = std::llround(
        (minimumY - static_cast<double>(originY)) / spacing);
    pageMinimum[1] =
        originY + static_cast<float>(minimumCellY) * spacing;
  }

  payload.terrainSlot = record.clipmapSlot;
  payload.terrainGeneration = record.residencyGeneration;
  payload.internalLOD = record.clipmapLOD;
  payload.regionMappingId = record.regionMappingId;
  payload.geometryRevision = source.geometryRevision;
  payload.instance.stableInstanceId = record.clipmapSlot;
  payload.instance.modelId = record.clipmapSlot;
  // The shared origin bias is expressed in the finest scene-ray lattice and
  // cannot reliably leave a streamed page's larger receiver voxel. Ask the
  // Tree64 tracer to exit exactly that raster voxel. The instance carries the
  // raster scale separately because an over-budget Tree64 page can OR-reduce
  // four or more voxels into one leaf; skipping that whole leaf caused the
  // earlier representation-sized checkerboard holes.
  // The transform built below is identity rotation plus translation, so the
  // traversal can skip the general 4x4 multiply entirely.
  payload.instance.flags = kSparseVoxelTree64DynamicSkipReceiverCell |
      kSparseVoxelTree64DynamicTranslationOnly;
  payload.instance.terrainSlot = record.clipmapSlot;
  payload.instance.terrainGeneration = record.residencyGeneration;
  payload.instance.regionMappingId = record.regionMappingId;
  payload.instance.receiverCellSizeMetres =
      static_cast<float>(pageVoxelSize);
  payload.instance.worldToModel = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    static_cast<float>(-pageMinimum[0]),
    static_cast<float>(-pageMinimum[1]),
    static_cast<float>(-pageMinimum[2]), 1.0f,
  };
  payload.instance.worldBounds.minimum = {
    static_cast<float>(pageMinimum[0]),
    static_cast<float>(minimumY),
    static_cast<float>(pageMinimum[2]),
  };
  payload.instance.worldBounds.maximum = {
    static_cast<float>(pageMaximum[0]),
    static_cast<float>(pageMaximum[1]),
    static_cast<float>(pageMaximum[2]),
  };

  const auto *reusable = source.reusableModel;
  if (reusable != nullptr && reusable->modelId == record.clipmapSlot &&
      reusable->payloadRevision == source.geometryRevision &&
      reusable->side != 0u && !reusable->prebuiltNodes.empty())
  {
    payload.model = *reusable;
    payload.model.physicalNodeSlot = record.clipmapSlot;
    const double cellSize = static_cast<double>(
        payload.model.localBounds.maximum[0] -
        payload.model.localBounds.minimum[0]) /
        static_cast<double>(payload.model.side);
    payload.cellScale = static_cast<uint32_t>(std::max<int64_t>(
        1, std::llround(cellSize / pageVoxelSize)));
    payload.sourceRunCount = source.reusableSourceRunCount;
    payload.clippedRunCount = source.reusableClippedRunCount;
    payload.reusedNodes = true;
    if (source.includeMaterialGeometry &&
        source.reusableMaterialCells != nullptr)
    {
      payload.materialCells = *source.reusableMaterialCells;
      payload.reusedMaterialCells = true;
      return true;
    }
  }
  else
  {
    SparseVoxelTerrainPageTree64Source pageSource{};
    pageSource.bricks = &chunks.bricks;
    pageSource.firstBrickRoot = record.firstBrickRoot;
    pageSource.brickRootCount = record.brickRootCount;
    pageSource.brickNodeOffset = record.brickNodeOffset;
    pageSource.brickNodeCapacity = record.brickNodeCapacity;
    pageSource.pageMinimumMetres = pageMinimum;
    pageSource.edgeMetres = record.edgeMetres;
    pageSource.heightMetres = maximumY - minimumY;
    pageSource.voxelSizeMetres = pageVoxelSize;
    pageSource.requestedCellsPerEdge = source.requestedCellsPerEdge;
    pageSource.maximumNodeCount = source.maximumNodeCount;
    // A page coarser than the set's finest voxel is a distant spatial-LOD
    // page. Half a voxel of slack keeps the comparison off the floating-point
    // boundary when the ratio is an exact power of two.
    pageSource.conservativeInside = source.conservativeInsideCoarsePages &&
        chunks.voxelSizeMetres > 0.0 &&
        pageVoxelSize > chunks.voxelSizeMetres * 1.5;
    SparseVoxelTerrainPageTree64 built{};
    if (!buildSparseVoxelTerrainPageTree64(pageSource, built, error))
      return false;

    payload.model.modelId = record.clipmapSlot;
    payload.model.side = built.side;
    payload.model.localBounds.minimum = built.localMinimum;
    payload.model.localBounds.maximum = built.localMaximum;
    payload.model.prebuiltNodes = std::move(built.tree.nodes);
    payload.model.payloadRevision = source.geometryRevision;
    payload.model.physicalNodeSlot = record.clipmapSlot;
    payload.sourceRunCount = built.runCount;
    payload.clippedRunCount = built.clippedRunCount;
    payload.cellScale = built.cellScale;
  }

  if (!source.includeMaterialGeometry)
    return true;

  // Reduced cells cover several raster voxels and have no single material.
  // Occupancy-only consumers can still use the page conservatively.
  if (payload.cellScale != 1u || record.clipmapSlot > 0x7fffu)
    return true;
  const auto populationCount = [](uint32_t value)
  {
    value -= (value >> 1u) & 0x55555555u;
    value = (value & 0x33333333u) +
        ((value >> 2u) & 0x33333333u);
    value = (value + (value >> 4u)) & 0x0f0f0f0fu;
    return (value * 0x01010101u) >> 24u;
  };
  const uint32_t nodeEnd = record.brickNodeOffset +
      record.brickNodeCapacity;
  for (uint32_t nodeIndex = record.brickNodeOffset;
       nodeIndex < nodeEnd; ++nodeIndex)
  {
    const auto &node = chunks.bricks.nodes[nodeIndex];
    const double nodeVoxelSize = node.minimumVoxelSize[3];
    if (!(nodeVoxelSize > 0.0) ||
        std::abs(nodeVoxelSize - pageVoxelSize) >
            std::max(1.0e-6, pageVoxelSize * 1.0e-5) ||
        node.occupancyIndex >= chunks.bricks.occupancy.size())
      continue;
    const auto &occupancy = chunks.bricks.occupancy[node.occupancyIndex];
    for (uint32_t linearCell = 0u;
         linearCell < kSparseVoxelBrickSide * kSparseVoxelBrickSide *
                 kSparseVoxelBrickSide;
         ++linearCell)
    {
      const uint32_t word = linearCell / 32u;
      const uint32_t bit = linearCell % 32u;
      const uint32_t bits = occupancy.cells[word].bits;
      if ((bits & (uint32_t{1u} << bit)) == 0u)
        continue;
      const uint32_t precedingMask = bit == 0u
          ? 0u
          : ((uint32_t{1u} << bit) - 1u);
      const uint64_t materialAddress =
          static_cast<uint64_t>(node.materialBase) +
          occupancy.cells[word].prefix +
          populationCount(bits & precedingMask);
      if (materialAddress >= chunks.bricks.materialIndices.size() ||
          materialAddress >= chunks.bricks.microSurfaceData.size())
        return fail(
            error,
            "Terrain Tree64 material extraction found an invalid sidecar address");
      const uint64_t packedSurface =
          chunks.bricks.microSurfaceData[materialAddress];
      const uint32_t context =
          unpackSparseVoxelMicroContextValue(packedSurface);
      if (!sparseVoxelMicroContextIsTerrainSurface(context))
        continue;
      const uint64_t paletteAddress =
          static_cast<uint64_t>(node.paletteBase) +
          chunks.bricks.materialIndices[materialAddress];
      if (paletteAddress >= chunks.bricks.paletteDescriptorIds.size())
        return fail(
            error,
            "Terrain Tree64 material extraction found an invalid palette address");
      const uint16_t materialHandle =
          chunks.bricks.paletteDescriptorIds[paletteAddress];
      if (materialHandle == 0u)
        continue;
      const std::array<uint32_t, 3> localCell{
        linearCell % kSparseVoxelBrickSide,
        (linearCell / kSparseVoxelBrickSide) % kSparseVoxelBrickSide,
        linearCell / (kSparseVoxelBrickSide * kSparseVoxelBrickSide),
      };
      std::array<int32_t, 3> modelCell{};
      bool validCell = true;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        const double worldMinimum = node.minimumVoxelSize[axis] +
            static_cast<double>(localCell[axis]) * pageVoxelSize;
        const double coordinate =
            (worldMinimum - pageMinimum[axis]) / pageVoxelSize;
        const double rounded = std::round(coordinate);
        validCell = validCell &&
            std::abs(coordinate - rounded) <=
                std::max(1.0e-5, std::abs(coordinate) * 1.0e-6) &&
            rounded >= 0.0 && rounded < payload.model.side;
        modelCell[axis] = static_cast<int32_t>(rounded);
      }
      if (!validCell)
        continue;
      SparseVoxelTree64MaterialCellGPU material{};
      material.globalCell = {
        static_cast<int32_t>(record.clipmapSlot * 65536u) + modelCell[0],
        modelCell[1],
        modelCell[2],
      };
      material.materialAndFlags = materialHandle |
          kSparseVoxelTree64MaterialCellValid;
      material.surfaceData =
          packSparseVoxelMicroContextDataLegacy(packedSurface);
      material.regionMappingId = node.regionMappingId;
      payload.materialCells.push_back(material);
    }
  }
  std::sort(
      payload.materialCells.begin(), payload.materialCells.end(),
      [](const auto &left, const auto &right)
      {
        return left.globalCell < right.globalCell;
      });
  for (size_t index = 1u; index < payload.materialCells.size(); ++index)
    if (payload.materialCells[index - 1u].globalCell ==
        payload.materialCells[index].globalCell)
    {
      if (payload.materialCells[index - 1u].materialAndFlags !=
              payload.materialCells[index].materialAndFlags ||
          payload.materialCells[index - 1u].surfaceData !=
              payload.materialCells[index].surfaceData ||
          payload.materialCells[index - 1u].auxContext !=
              payload.materialCells[index].auxContext ||
          payload.materialCells[index - 1u].regionMappingId !=
              payload.materialCells[index].regionMappingId)
        return fail(
            error,
            "Terrain Tree64 material page contains conflicting exact cells");
    }
  payload.materialCells.erase(
      std::unique(
          payload.materialCells.begin(), payload.materialCells.end(),
          [](const auto &left, const auto &right)
          {
            return left.globalCell == right.globalCell;
          }),
      payload.materialCells.end());
  return true;
}

bool buildSparseVoxelTerrainPageTree64(
    const SparseVoxelTerrainPageTree64Source &source,
    SparseVoxelTerrainPageTree64 &result,
    std::string *error)
{
  const auto buildStart = std::chrono::steady_clock::now();
  result = {};
  if (error != nullptr)
    error->clear();
  if (source.bricks == nullptr)
    return fail(error, "Terrain Tree64 page needs a brick hierarchy");
  if (!(source.edgeMetres > 0.0) || !(source.voxelSizeMetres > 0.0) ||
      !std::isfinite(source.edgeMetres) ||
      !std::isfinite(source.heightMetres) || source.heightMetres < 0.0)
    return fail(error, "Terrain Tree64 page extents are invalid");
  const auto &bricks = *source.bricks;
  if (source.brickNodeOffset > bricks.nodes.size() ||
      source.brickNodeCapacity > bricks.nodes.size() - source.brickNodeOffset ||
      source.firstBrickRoot > bricks.rootIndices.size() ||
      source.brickRootCount > bricks.rootIndices.size() - source.firstBrickRoot)
    return fail(error, "Terrain Tree64 page ranges are invalid");

  const double voxelSize = source.voxelSizeMetres;
  const int64_t horizontalVoxels =
      std::llround(source.edgeMetres / voxelSize);
  const int64_t verticalVoxels = static_cast<int64_t>(
      std::ceil(source.heightMetres / voxelSize - 1.0e-6));
  const int64_t requiredVoxels =
      std::max<int64_t>({horizontalVoxels, verticalVoxels, 1});
  if (requiredVoxels > 65536)
    return fail(error, "Terrain Tree64 page needs more than 65536 cells");

  // One cell per page voxel unless the page is larger than the representable
  // or requested lattice; the tree then covers the same volume with cells that
  // are a power-of-four multiple of a voxel.
  uint32_t cellScale = 1u;
  uint32_t side = sparseVoxelTerrainTree64SideForCells(
      static_cast<uint32_t>(requiredVoxels));
  if (side == 0u)
    return fail(error, "Terrain Tree64 page side is not representable");
  const uint32_t maximumSide = std::max(
      kSparseVoxelTree64Branching,
      std::min(source.maximumSide, 65536u));
  const uint32_t requestedSide = source.requestedCellsPerEdge == 0u
      ? side
      : std::max(
            kSparseVoxelTree64Branching,
            sparseVoxelTerrainTree64SideForCells(
                std::min(source.requestedCellsPerEdge, 65536u)));
  while (side > kSparseVoxelTree64Branching &&
         (side > maximumSide || side > requestedSide))
  {
    side /= kSparseVoxelTree64Branching;
    cellScale *= kSparseVoxelTree64Branching;
  }

  const std::array<double, 3> origin = source.pageMinimumMetres;
  const auto voxelCoordinate = [&](const double metres, const uint32_t axis)
  {
    return static_cast<int64_t>(
        std::llround((metres - origin[axis]) / voxelSize));
  };

  std::vector<VoxelRun> runs;
  runs.reserve(static_cast<size_t>(source.brickRootCount) * 64u);
  uint64_t occupiedVoxelCount = 0u;
  uint32_t clippedRunCount = 0u;
  const auto appendRun = [&](const std::array<int64_t, 3> &minimum,
                             const std::array<int64_t, 3> &maximum)
  {
    VoxelRun run{};
    bool clipped = false;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const int64_t low = std::max<int64_t>(minimum[axis], 0);
      const int64_t high = std::min<int64_t>(
          maximum[axis], static_cast<int64_t>(side) * cellScale);
      clipped |= low != minimum[axis] || high != maximum[axis];
      if (low >= high)
      {
        ++clippedRunCount;
        return;
      }
      run.minimum[axis] = static_cast<int32_t>(low);
      run.maximum[axis] = static_cast<int32_t>(high);
    }
    if (clipped)
      ++clippedRunCount;
    occupiedVoxelCount += static_cast<uint64_t>(run.maximum[0] - run.minimum[0]) *
        static_cast<uint64_t>(run.maximum[1] - run.minimum[1]) *
        static_cast<uint64_t>(run.maximum[2] - run.minimum[2]);
    runs.push_back(run);
  };

  std::vector<uint32_t> pending;
  pending.reserve(static_cast<size_t>(source.brickRootCount) * 16u);
  for (uint32_t root = 0u; root < source.brickRootCount; ++root)
    pending.push_back(bricks.rootIndices[source.firstBrickRoot + root]);
  const uint32_t nodeEnd = source.brickNodeOffset + source.brickNodeCapacity;
  while (!pending.empty())
  {
    const uint32_t nodeIndex = pending.back();
    pending.pop_back();
    if (nodeIndex < source.brickNodeOffset || nodeIndex >= nodeEnd)
      continue;
    const auto &node = bricks.nodes[nodeIndex];
    const double brickVoxelSize = node.minimumVoxelSize[3];
    if (node.occupiedCount == 0u || !(brickVoxelSize > 0.0))
      continue;
    // Descend to the authored leaves. A coarse node is the OR reduction of its
    // children, so stopping early would grow the terrain by up to one coarse
    // voxel everywhere - the shell of invented occluders the raster can never
    // draw. Children outside this page's node range are not resident, so the
    // node's own reduction is used instead.
    bool childrenResident = node.childCount != 0u &&
        node.firstChild <= bricks.childIndices.size() &&
        node.childCount <= bricks.childIndices.size() - node.firstChild;
    if (childrenResident)
      for (uint32_t child = 0u; child < node.childCount; ++child)
      {
        const uint32_t childIndex = bricks.childIndices[node.firstChild + child];
        if (childIndex < source.brickNodeOffset || childIndex >= nodeEnd)
        {
          childrenResident = false;
          break;
        }
      }
    if (childrenResident)
    {
      for (uint32_t child = 0u; child < node.childCount; ++child)
        pending.push_back(bricks.childIndices[node.firstChild + child]);
      continue;
    }
    if (node.occupancyIndex >= bricks.occupancy.size())
      return fail(error, "Terrain Tree64 brick occupancy is out of range");
    const auto &occupancy = bricks.occupancy[node.occupancyIndex];
    ++result.leafBrickCount;

    // A brick coarser than the page voxel covers a whole block of page voxels
    // per cell. The ratio is a power of two by construction.
    const int64_t brickScale = std::max<int64_t>(
        1, std::llround(brickVoxelSize / voxelSize));
    std::array<int64_t, 3> base{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      base[axis] = voxelCoordinate(node.minimumVoxelSize[axis], axis);

    // Cascade ownership: the finer page draws this XZ prism, so this page must
    // not shadow it. Rounding the rectangle to the voxel lattice keeps every
    // emitted run cell aligned; the seam can differ from the raster by at most
    // half a voxel instead of the whole conservative cell it used to.
    const auto finer = sparseVoxelBrickFinerTerrainClipBounds(node);
    const bool hasFinerOwner = source.clipFinerCascade &&
        sparseVoxelBrickHasFinerTerrainClip(node);
    const int64_t clipMinimumX = hasFinerOwner ? voxelCoordinate(finer[0], 0u) : 0;
    const int64_t clipMaximumX = hasFinerOwner ? voxelCoordinate(finer[2], 0u) : 0;
    const int64_t clipMinimumZ = hasFinerOwner ? voxelCoordinate(finer[1], 2u) : 0;
    const int64_t clipMaximumZ = hasFinerOwner ? voxelCoordinate(finer[3], 2u) : 0;
    const auto appendOwnedRun = [&](const std::array<int64_t, 3> &minimum,
                                    const std::array<int64_t, 3> &maximum)
    {
      const int64_t overlapMinimumX = std::max(minimum[0], clipMinimumX);
      const int64_t overlapMaximumX = std::min(maximum[0], clipMaximumX);
      const int64_t overlapMinimumZ = std::max(minimum[2], clipMinimumZ);
      const int64_t overlapMaximumZ = std::min(maximum[2], clipMaximumZ);
      if (!hasFinerOwner || overlapMinimumX >= overlapMaximumX ||
          overlapMinimumZ >= overlapMaximumZ)
      {
        appendRun(minimum, maximum);
        return;
      }
      // Four non-overlapping strips subtract the finer prism: left and right
      // span all Z, front and back cover the remaining middle X interval.
      const auto appendStrip = [&](const int64_t minimumX, const int64_t maximumX,
                                   const int64_t minimumZ, const int64_t maximumZ)
      {
        if (minimumX >= maximumX || minimumZ >= maximumZ)
          return;
        appendRun({minimumX, minimum[1], minimumZ},
            {maximumX, maximum[1], maximumZ});
      };
      appendStrip(minimum[0], overlapMinimumX, minimum[2], maximum[2]);
      appendStrip(overlapMaximumX, maximum[0], minimum[2], maximum[2]);
      appendStrip(overlapMinimumX, overlapMaximumX, minimum[2], overlapMinimumZ);
      appendStrip(overlapMinimumX, overlapMaximumX, overlapMaximumZ, maximum[2]);
    };

    for (uint32_t z = 0u; z < kSparseVoxelBrickSide; ++z)
      for (uint32_t x = 0u; x < kSparseVoxelBrickSide; ++x)
      {
        uint32_t runBegin = kSparseVoxelBrickSide;
        for (uint32_t y = 0u; y <= kSparseVoxelBrickSide; ++y)
        {
          bool occupied = false;
          if (y < kSparseVoxelBrickSide)
          {
            const uint32_t cell =
                x + kSparseVoxelBrickSide * (y + kSparseVoxelBrickSide * z);
            occupied =
                (occupancy.cells[cell / 32u].bits & (uint32_t{1u} << (cell % 32u))) != 0u;
          }
          if (occupied && runBegin == kSparseVoxelBrickSide)
            runBegin = y;
          if ((!occupied || y == kSparseVoxelBrickSide) &&
              runBegin != kSparseVoxelBrickSide)
          {
            appendOwnedRun(
                {base[0] + static_cast<int64_t>(x) * brickScale,
                 base[1] + static_cast<int64_t>(runBegin) * brickScale,
                 base[2] + static_cast<int64_t>(z) * brickScale},
                {base[0] + static_cast<int64_t>(x + 1u) * brickScale,
                 base[1] + static_cast<int64_t>(y) * brickScale,
                 base[2] + static_cast<int64_t>(z + 1u) * brickScale});
            runBegin = kSparseVoxelBrickSide;
          }
        }
      }
  }

  // Lower every column until its cells are contained by the terrain.
  //
  // A column's exclusive top T means the raster filled cell T-1, whose top face
  // is at T voxels. The authored surface can be anywhere inside that cell, so
  // all this column proves is `surface >= (T - 1)`. A cell c is therefore known
  // to be fully below the surface across its own footprint only when
  // `c + 1 <= min(T) - 1` over the columns that share it, which is
  // `c <= min(T) - 2`. Taking the minimum over the four edge neighbours is what
  // makes this hold on a slope, where the cell's own column says nothing about
  // the ground under its far edge.
  //
  // Runs keep their thickness and slide down, so a page costs the same nodes it
  // did before. Columns whose top reaches the page floor are dropped: there is
  // no cell there that is provably inside, and dropping is the safe direction.
  if (source.conservativeInside && !runs.empty())
  {
    const int64_t columns = static_cast<int64_t>(side) * cellScale;
    std::vector<int32_t> exclusiveTop(
        static_cast<size_t>(columns * columns), -1);
    const auto columnIndex = [&](const int64_t x, const int64_t z)
    {
      return static_cast<size_t>(z * columns + x);
    };
    for (const auto &run : runs)
      for (int64_t z = run.minimum[2]; z < run.maximum[2]; ++z)
        for (int64_t x = run.minimum[0]; x < run.maximum[0]; ++x)
          exclusiveTop[columnIndex(x, z)] = std::max(
              exclusiveTop[columnIndex(x, z)], run.maximum[1]);

    // Neighbours outside the page are unknown rather than absent, so they must
    // not lower the bound - a page edge would otherwise erode itself inward
    // from all four sides every time the cut changes.
    const auto boundedTop = [&](const int64_t x, const int64_t z)
    {
      int32_t bound = exclusiveTop[columnIndex(x, z)];
      const std::array<std::array<int64_t, 2>, 4> neighbours{{
        {x - 1, z}, {x + 1, z}, {x, z - 1}, {x, z + 1},
      }};
      for (const auto &neighbour : neighbours)
      {
        if (neighbour[0] < 0 || neighbour[1] < 0 ||
            neighbour[0] >= columns || neighbour[1] >= columns)
          continue;
        const int32_t candidate = exclusiveTop[columnIndex(neighbour[0], neighbour[1])];
        if (candidate >= 0)
          bound = std::min(bound, candidate);
      }
      return bound;
    };

    std::vector<VoxelRun> loweredRuns;
    loweredRuns.reserve(runs.size());
    uint64_t loweredVoxelCount = 0u;
    for (const auto &run : runs)
    {
      // One run can span several columns when a brick is coarser than the page
      // voxel; the whole run has to satisfy the tightest of them.
      int32_t bound = std::numeric_limits<int32_t>::max();
      for (int64_t z = run.minimum[2]; z < run.maximum[2]; ++z)
        for (int64_t x = run.minimum[0]; x < run.maximum[0]; ++x)
          bound = std::min(bound, boundedTop(x, z));
      const int32_t loweredTop = std::min(run.maximum[1], bound - 1);
      const int32_t thickness = run.maximum[1] - run.minimum[1];
      if (loweredTop <= 0)
        continue;
      VoxelRun lowered = run;
      lowered.maximum[1] = loweredTop;
      lowered.minimum[1] = std::max(0, loweredTop - thickness);
      if (lowered.minimum[1] >= lowered.maximum[1])
        continue;
      loweredVoxelCount +=
          static_cast<uint64_t>(lowered.maximum[0] - lowered.minimum[0]) *
          static_cast<uint64_t>(lowered.maximum[1] - lowered.minimum[1]) *
          static_cast<uint64_t>(lowered.maximum[2] - lowered.minimum[2]);
      loweredRuns.push_back(lowered);
    }
    runs = std::move(loweredRuns);
    occupiedVoxelCount = loweredVoxelCount;
  }

  // Reduce onto a coarser aligned lattice until the tree fits its node slot.
  // Every occupied voxel stays inside an occupied cell, so a page that has to
  // reduce loses sharpness without ever opening a hole.
  std::vector<SparseVoxelTree64AABB> boxes;
  for (;;)
  {
    boxes.clear();
    boxes.reserve(runs.size());
    for (const auto &run : runs)
    {
      SparseVoxelTree64AABB box{};
      bool empty = false;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        double minimum =
            static_cast<double>(run.minimum[axis]) / static_cast<double>(cellScale);
        double maximum =
            static_cast<double>(run.maximum[axis]) / static_cast<double>(cellScale);
        // The reduction has to round the same way the page does. Voxelizing the
        // fractional box marks every cell the run *touches*, which grows the
        // geometry by up to a cell on each side - correct for a shell that must
        // contain the terrain, and the exact failure an inside-conservative
        // page exists to avoid. Snapping inward keeps only whole cells the run
        // fully covers, so a reduced page stays inside just as its unreduced
        // form was.
        if (source.conservativeInside)
        {
          minimum = std::ceil(minimum);
          maximum = std::floor(maximum);
          empty = empty || !(minimum < maximum);
        }
        box.minimum[axis] = minimum;
        box.maximum[axis] = maximum;
      }
      if (empty)
        continue;
      boxes.push_back(box);
    }
    SparseVoxelTree64Chunk tree{};
    if (!buildSparseVoxelTree64Model(side, boxes, tree, error))
      return false;
    if (source.maximumNodeCount == 0u ||
        tree.nodes.size() <= source.maximumNodeCount ||
        side == kSparseVoxelTree64Branching)
    {
      if (source.maximumNodeCount != 0u &&
          tree.nodes.size() > source.maximumNodeCount)
        return fail(error, "Terrain Tree64 page exceeds its node budget");
      result.tree = std::move(tree);
      break;
    }
    side /= kSparseVoxelTree64Branching;
    cellScale *= kSparseVoxelTree64Branching;
  }

  result.side = side;
  result.cellScale = cellScale;
  result.cellSizeMetres = voxelSize * static_cast<double>(cellScale);
  result.originMetres = origin;
  result.extentMetres = result.cellSizeMetres * static_cast<double>(side);
  result.localMinimum = {0.0f, 0.0f, 0.0f};
  result.localMaximum = {
    static_cast<float>(result.extentMetres),
    static_cast<float>(result.extentMetres),
    static_cast<float>(result.extentMetres),
  };
  result.worldToModel = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    static_cast<float>(-origin[0]),
    static_cast<float>(-origin[1]),
    static_cast<float>(-origin[2]), 1.0f,
  };
  result.runCount = static_cast<uint32_t>(runs.size());
  result.clippedRunCount = clippedRunCount;
  result.occupiedVoxelCount = static_cast<uint32_t>(
      std::min<uint64_t>(occupiedVoxelCount, std::numeric_limits<uint32_t>::max()));
  uint32_t levelSide = result.side;
  while (levelSide >= kSparseVoxelTree64Branching)
  {
    ++result.levelCount;
    if (levelSide == kSparseVoxelTree64Branching)
      break;
    levelSide /= kSparseVoxelTree64Branching;
  }
  result.payloadBytes =
      uint64_t{result.tree.nodes.size()} * sizeof(result.tree.nodes.front());
  result.buildMilliseconds =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - buildStart)
          .count();
  return true;
}

bool buildSparseVoxelTerrainPageTree64FromColumns(
    const WorldMapTerrainColumnPage &columns,
    SparseVoxelTerrainPageTree64 &result, std::string *error)
{
  const auto buildStart = std::chrono::steady_clock::now();
  result = {};
  if (error != nullptr)
    error->clear();
  if (!validateWorldMapTerrainColumns(columns, error))
    return false;
  uint32_t requiredCells = columns.side;
  uint64_t occupiedVoxelCount = 0u;
  std::vector<SparseVoxelTree64AABB> boxes;
  boxes.reserve(columns.columns.size());
  for (uint32_t z = 0u; z < columns.side; ++z)
    for (uint32_t x = 0u; x < columns.side; ++x)
    {
      const auto &column = columns.columns[
          static_cast<size_t>(z) * columns.side + x];
      requiredCells = std::max(requiredCells, column.topY + 1u);
      boxes.push_back({
        {static_cast<double>(x), static_cast<double>(column.firstSolidY),
         static_cast<double>(z)},
        {static_cast<double>(x + 1u), static_cast<double>(column.topY + 1u),
         static_cast<double>(z + 1u)},
      });
      occupiedVoxelCount +=
          uint64_t{column.topY} - column.firstSolidY + 1u;
    }
  const uint32_t side = sparseVoxelTerrainTree64SideForCells(requiredCells);
  if (side == 0u)
    return fail(error, "Terrain column Tree64 side is not representable");
  if (!buildSparseVoxelTree64Model(side, boxes, result.tree, error))
    return false;
  result.side = side;
  result.cellScale = 1u;
  result.cellSizeMetres = columns.voxelSizeMetres;
  result.originMetres = {
    -columns.edgeMetres * 0.5, 0.0, -columns.edgeMetres * 0.5};
  result.extentMetres = columns.voxelSizeMetres * side;
  result.localMinimum = {0.0f, 0.0f, 0.0f};
  result.localMaximum = {
    static_cast<float>(result.extentMetres),
    static_cast<float>(result.extentMetres),
    static_cast<float>(result.extentMetres),
  };
  result.worldToModel = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    static_cast<float>(-result.originMetres[0]),
    static_cast<float>(-result.originMetres[1]),
    static_cast<float>(-result.originMetres[2]), 1.0f,
  };
  result.runCount = static_cast<uint32_t>(boxes.size());
  result.occupiedVoxelCount = static_cast<uint32_t>(
      std::min<uint64_t>(
          occupiedVoxelCount, std::numeric_limits<uint32_t>::max()));
  uint32_t levelSide = side;
  while (levelSide >= kSparseVoxelTree64Branching)
  {
    ++result.levelCount;
    if (levelSide == kSparseVoxelTree64Branching)
      break;
    levelSide /= kSparseVoxelTree64Branching;
  }
  result.payloadBytes = uint64_t{result.tree.nodes.size()} *
      sizeof(result.tree.nodes.front());
  result.buildMilliseconds = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - buildStart).count();
  return true;
}

} // namespace voxel::svraster
