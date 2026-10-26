#include "voxel/svraster/SparseVoxelBrick.hpp"

#include "voxel/material/VoxelMaterial.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>

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

using BrickKey = std::array<int32_t, 3>;

struct BrickCells
{
  std::array<uint32_t, kSparseVoxelBrickOccupancyWords> words{};
  // Material per occupied cell, keyed by linear cell index so the flattening
  // pass can walk cells in bit order.
  std::map<uint32_t, uint32_t> materials;
  // Deviation of this brick from the finest representation of the same volume,
  // in world units. Zero at the finest level, which is exact by definition.
  float selfError = 0.0f;
};

uint32_t cellIndex(const uint32_t x, const uint32_t y, const uint32_t z)
{
  return x + kSparseVoxelBrickSide * (y + kSparseVoxelBrickSide * z);
}

void setCell(BrickCells &brick, const uint32_t index, const uint32_t materialIndex)
{
  const uint32_t word = index / 32u;
  const uint32_t bit = index % 32u;
  if ((brick.words[word] & (1u << bit)) == 0u)
  {
    brick.words[word] |= 1u << bit;
    brick.materials.emplace(index, materialIndex);
  }
}

int32_t floorDivide(const int32_t value, const int32_t divisor)
{
  const int32_t quotient = value / divisor;
  return quotient - ((value % divisor != 0 && (value < 0) != (divisor < 0)) ? 1 : 0);
}

int32_t positiveModulo(const int32_t value, const int32_t divisor)
{
  const int32_t remainder = value % divisor;
  return remainder < 0 ? remainder + divisor : remainder;
}

// A leaf boundary that is a grid multiple in exact arithmetic can land a few
// ULP either side of it in float, and floor()/ceil() turn that into a whole
// extra cell - which dilates the brick set by one voxel wherever it happens.
// Snapping first keeps the fill exactly as wide as the leaf really is.
constexpr double kGridSnapTolerance = 1.0 / 1024.0;

double snappedGridCoordinate(const float value, const float voxelSize)
{
  const double cells = static_cast<double>(value) / static_cast<double>(voxelSize);
  const double nearest = std::round(cells);
  return std::abs(cells - nearest) < kGridSnapTolerance ? nearest : cells;
}

// The octree stores emissive as three halves; the brick form has one byte.
float halfToFloat(const uint32_t half)
{
  const uint32_t sign = (half >> 15u) & 1u;
  const uint32_t exponent = (half >> 10u) & 0x1fu;
  const uint32_t mantissa = half & 0x3ffu;
  float value = 0.0f;
  if (exponent == 0u)
    value = static_cast<float>(mantissa) * 5.9604645e-8f;
  else if (exponent == 0x1fu)
    value = mantissa == 0u ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
  else
    value = std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f, static_cast<int32_t>(exponent) - 15);
  return sign != 0u ? -value : value;
}

uint32_t popcount32(uint32_t value)
{
  value = value - ((value >> 1u) & 0x55555555u);
  value = (value & 0x33333333u) + ((value >> 2u) & 0x33333333u);
  value = (value + (value >> 4u)) & 0x0f0f0f0fu;
  return (value * 0x01010101u) >> 24u;
}

uint32_t quantizeUnorm8(const float value)
{
  const float clamped = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
  return static_cast<uint32_t>(std::lround(clamped * 255.0f)) & 0xffu;
}

} // namespace

namespace
{

// These six channels drive palette clustering: R, G, B, roughness, emissive,
// metallic. Optional secondary relationships are resolved only after the
// palette colours are fixed, so their index/mode metadata must not distort the
// colour-distance metric.
struct BrickMaterialFeature
{
  std::array<float, 6> value{};
};

struct BrickPaletteKey
{
  uint32_t packed0 = 0u;
  uint32_t packed1 = 0u;
  uint16_t descriptorId = 0u;

  bool operator<(const BrickPaletteKey &other) const
  {
    return std::tie(packed0, packed1, descriptorId) <
        std::tie(other.packed0, other.packed1, other.descriptorId);
  }
};

struct BrickSecondarySource
{
  std::pair<uint32_t, uint32_t> top{};
  std::pair<uint32_t, uint32_t> bottom{};
  uint16_t descriptorId = 0u;
  uint32_t faceModes = 0u;

  bool operator<(const BrickSecondarySource &other) const
  {
    return std::tie(top, bottom, descriptorId, faceModes) <
        std::tie(
            other.top, other.bottom, other.descriptorId, other.faceModes);
  }
};

BrickMaterialFeature brickMaterialFeature(const SparseVoxelBrickMaterialGPU &material)
{
  return BrickMaterialFeature{{{
    static_cast<float>(material.packed[0] & 0xffu),
    static_cast<float>((material.packed[0] >> 8u) & 0xffu),
    static_cast<float>((material.packed[0] >> 16u) & 0xffu),
    static_cast<float>((material.packed[0] >> 24u) & 0xffu),
    static_cast<float>(material.packed[1] & 0xffu),
    static_cast<float>((material.packed[1] >> 8u) & 0xffu),
  }}};
}

SparseVoxelBrickMaterialGPU brickMaterialFromFeature(const BrickMaterialFeature &feature)
{
  const auto byteOf = [](const float value)
  {
    return static_cast<uint32_t>(
        std::lround(std::min(std::max(value, 0.0f), 255.0f)));
  };
  return SparseVoxelBrickMaterialGPU{{
    byteOf(feature.value[0]) | (byteOf(feature.value[1]) << 8u) |
        (byteOf(feature.value[2]) << 16u) | (byteOf(feature.value[3]) << 24u),
    byteOf(feature.value[4]) | (byteOf(feature.value[5]) << 8u)}};
}

float brickMaterialDistanceSquared(
    const BrickMaterialFeature &a, const BrickMaterialFeature &b)
{
  // Albedo dominates deliberately. Roughness, metallic and emissive are almost
  // always constant across an authored material, so weighting them equally
  // with colour spends palette entries separating records that look identical.
  static constexpr std::array<float, 6> kWeights{1.0f, 1.0f, 1.0f, 0.25f, 0.5f, 0.25f};
  float total = 0.0f;
  for (uint32_t axis = 0u; axis < 6u; ++axis)
  {
    const float delta = (a.value[axis] - b.value[axis]) * kWeights[axis];
    total += delta * delta;
  }
  return total;
}

// Palette for one instance, over its distinct records weighted by how many
// voxels carry each.
//
// Fewer distinct records than entries is the common case - most authored
// materials voxelise to well under 256 - and it is reproduced exactly, so the
// byte indirection costs nothing in quality. Only the overflow case clusters.
//
// Weighted k-means, seeded k-means++ style by farthest-point over the weights.
// Deterministic: the candidate order is the sorted distinct-record order and
// ties resolve to the lowest index, so a rebuild produces the same palette.
struct BuiltPaletteEntry
{
  SparseVoxelBrickMaterialGPU material{};
  uint16_t descriptorId = 0u;
};

std::vector<BuiltPaletteEntry> buildBrickPalette(
    const std::map<BrickPaletteKey, uint64_t> &records,
    const uint32_t paletteSize,
    const uint32_t refinementIterations)
{
  std::vector<BuiltPaletteEntry> palette;
  if (records.empty() || paletteSize == 0u)
    return palette;
  std::vector<BrickMaterialFeature> features;
  std::vector<float> weights;
  features.reserve(records.size());
  weights.reserve(records.size());
  for (const auto &[key, count] : records)
  {
    features.push_back(
        brickMaterialFeature(
            SparseVoxelBrickMaterialGPU{{key.packed0, key.packed1}}));
    weights.push_back(static_cast<float>(count));
  }
  if (features.size() <= paletteSize)
  {
    palette.reserve(features.size());
    for (const auto &[key, count] : records)
    {
      (void)count;
      palette.push_back({
        SparseVoxelBrickMaterialGPU{{key.packed0, key.packed1}},
        key.descriptorId});
    }
    return palette;
  }

  // Seed: the heaviest record, then repeatedly the record farthest from every
  // centroid chosen so far, scaled by its weight so a heavily used outlier
  // wins over a single stray voxel.
  std::vector<uint32_t> seeds;
  seeds.reserve(paletteSize);
  {
    uint32_t heaviest = 0u;
    for (uint32_t index = 1u; index < weights.size(); ++index)
      if (weights[index] > weights[heaviest])
        heaviest = index;
    seeds.push_back(heaviest);
  }
  std::vector<float> nearest(features.size(), std::numeric_limits<float>::max());
  while (seeds.size() < paletteSize)
  {
    const BrickMaterialFeature &last = features[seeds.back()];
    uint32_t best = 0u;
    float bestScore = -1.0f;
    for (uint32_t index = 0u; index < features.size(); ++index)
    {
      nearest[index] = std::min(
          nearest[index], brickMaterialDistanceSquared(features[index], last));
      const float score = nearest[index] * weights[index];
      if (score > bestScore)
      {
        bestScore = score;
        best = index;
      }
    }
    if (bestScore <= 0.0f)
      break;
    seeds.push_back(best);
  }

  std::vector<BrickMaterialFeature> centroids;
  centroids.reserve(seeds.size());
  for (const uint32_t seed : seeds)
    centroids.push_back(features[seed]);

  std::vector<uint32_t> assignment(features.size(), 0u);
  for (uint32_t iteration = 0u; iteration < refinementIterations; ++iteration)
  {
    bool moved = false;
    for (uint32_t index = 0u; index < features.size(); ++index)
    {
      uint32_t best = 0u;
      float bestDistance = std::numeric_limits<float>::max();
      for (uint32_t centroid = 0u; centroid < centroids.size(); ++centroid)
      {
        const float distance =
            brickMaterialDistanceSquared(features[index], centroids[centroid]);
        if (distance < bestDistance)
        {
          bestDistance = distance;
          best = centroid;
        }
      }
      if (assignment[index] != best)
      {
        assignment[index] = best;
        moved = true;
      }
    }
    if (!moved)
      break;
    std::vector<std::array<double, 6>> sums(centroids.size(), std::array<double, 6>{});
    std::vector<double> totals(centroids.size(), 0.0);
    for (uint32_t index = 0u; index < features.size(); ++index)
    {
      const uint32_t centroid = assignment[index];
      totals[centroid] += weights[index];
      for (uint32_t axis = 0u; axis < 6u; ++axis)
        sums[centroid][axis] +=
            static_cast<double>(features[index].value[axis]) * weights[index];
    }
    for (uint32_t centroid = 0u; centroid < centroids.size(); ++centroid)
    {
      // An emptied centroid keeps its previous position rather than being
      // dropped: removing it would change the palette size mid-build and the
      // indices already written against it.
      if (totals[centroid] <= 0.0)
        continue;
      for (uint32_t axis = 0u; axis < 6u; ++axis)
        centroids[centroid].value[axis] =
            static_cast<float>(sums[centroid][axis] / totals[centroid]);
    }
  }

  palette.reserve(centroids.size());
  for (const BrickMaterialFeature &centroid : centroids)
    palette.push_back({brickMaterialFromFeature(centroid), 0u});
  return palette;
}

uint8_t nearestBrickPaletteEntry(
    const std::vector<SparseVoxelBrickMaterialGPU> &palette,
    const std::vector<uint16_t> &descriptorIds,
    const uint32_t paletteBase, const uint32_t paletteCount,
    const SparseVoxelBrickMaterialGPU &material,
    const uint16_t descriptorId)
{
  if (paletteCount == 0u)
    return 0u;
  const BrickMaterialFeature target = brickMaterialFeature(material);
  uint32_t best = 0u;
  float bestDistance = std::numeric_limits<float>::max();
  for (uint32_t entry = 0u; entry < paletteCount; ++entry)
  {
    if (descriptorIds[paletteBase + entry] != descriptorId)
      continue;
    const float distance = brickMaterialDistanceSquared(
        target, brickMaterialFeature(palette[paletteBase + entry]));
    if (distance < bestDistance)
    {
      bestDistance = distance;
      best = entry;
    }
  }
  return static_cast<uint8_t>(best);
}

} // namespace

SparseVoxelBrickMaterialGPU packSparseVoxelBrickMaterial(const SparseVoxelMaterialGPU &material)
{
  const uint32_t albedo = material.albedoRGBA8 & 0x00ffffffu;
  const uint32_t roughness = material.roughnessMetallicUNorm8 & 0xffu;
  const uint32_t metallic = (material.roughnessMetallicUNorm8 >> 8u) & 0xffu;
  // Luminance of the emissive triple, normalized against the scale the decode
  // side multiplies back in. Voxel emitters are tinted by their own albedo, so
  // collapsing the colour to an intensity loses only the hue difference
  // between the emitter and the surface it sits on.
  const float emissiveRed = halfToFloat(material.emissiveRG16F & 0xffffu);
  const float emissiveGreen = halfToFloat((material.emissiveRG16F >> 16u) & 0xffffu);
  const float emissiveBlue = halfToFloat(material.emissiveB16F & 0xffffu);
  const float luminance =
      0.2126f * emissiveRed + 0.7152f * emissiveGreen + 0.0722f * emissiveBlue;
  const uint32_t emissive = quantizeUnorm8(luminance / kSparseVoxelBrickEmissiveScale);
  return SparseVoxelBrickMaterialGPU{
    {albedo | (roughness << 24u),
     emissive | (metallic << 8u)}};
}

namespace
{

bool hasSparseVoxelSecondaryMaterial(const SparseVoxelMaterialGPU &material)
{
  return (material.reserved0 >> 24u) != 0u;
}

SparseVoxelBrickMaterialGPU packSparseVoxelBrickBottomMaterial(
    const SparseVoxelMaterialGPU &material)
{
  SparseVoxelMaterialGPU bottom = material;
  bottom.albedoRGBA8 = material.reserved0;
  bottom.flags = 0u;
  bottom.reserved0 = 0u;
  bottom.reserved1 = 0u;
  return packSparseVoxelBrickMaterial(bottom);
}

uint32_t sparseVoxelBrickSecondaryMetadata(
    const uint8_t bottomPaletteIndex, const uint32_t faceModes)
{
  uint32_t metadata = kSparseVoxelBrickSecondaryEnabled |
      (static_cast<uint32_t>(bottomPaletteIndex) <<
       kSparseVoxelBrickSecondaryPaletteShift);
  if ((faceModes & kSparseVoxelMaterialBlendTopBottomFaces) != 0u)
    metadata |= kSparseVoxelBrickBlendTopBottomFaces;
  if ((faceModes & kSparseVoxelMaterialBlendSideFaces) != 0u)
    metadata |= kSparseVoxelBrickBlendSideFaces;
  return metadata;
}

} // namespace

bool buildSparseVoxelBrickLODGroups(
    SparseVoxelBrickHierarchy &hierarchy,
    uint32_t groupSideBricks,
    std::string *error)
{
  hierarchy.lodGroups.clear();
  hierarchy.lodGroupNodeIndices.clear();
  hierarchy.nodeLODGroupIndices.clear();
  hierarchy.representativeLODGroupIndices.clear();
  hierarchy.lodGroupSideBricks = groupSideBricks;

  if (groupSideBricks == 0u)
    return true;
  if (hierarchy.nodes.empty())
    return true;
  if (!(hierarchy.finestVoxelSize > 0.0f) ||
      !std::isfinite(hierarchy.finestVoxelSize))
    return fail(error, "Brick LOD groups need a valid finest voxel size");

  struct GroupKey
  {
    uint32_t level = 0u;
    std::array<int32_t, 3> coordinate{};

    bool operator<(const GroupKey &other) const
    {
      return std::tie(level, coordinate[2], coordinate[1], coordinate[0]) <
          std::tie(
              other.level,
              other.coordinate[2],
              other.coordinate[1],
              other.coordinate[0]);
    }
  };

  struct GroupAccum
  {
    std::vector<uint32_t> nodes;
    float minimumError = std::numeric_limits<float>::max();
    float maximumError = 0.0f;
    int64_t sideInFinestCells = 0;
  };

  const auto floorDivide64 = [](const int64_t value, const int64_t divisor)
  {
    const int64_t quotient = value / divisor;
    return quotient - ((value % divisor != 0 && value < 0) ? 1 : 0);
  };
  const auto occupiedMinimum = [](const uint32_t packed, const uint32_t axis)
  {
    return (packed >> (axis * 3u)) & 7u;
  };
  const auto occupiedMaximum = [](const uint32_t packed, const uint32_t axis)
  {
    return (packed >> (9u + axis * 3u)) & 7u;
  };
  constexpr std::array<uint32_t, 3> kBoundaryMinusBits{
    kSparseVoxelBrickLODGroupBoundaryMinusX,
    kSparseVoxelBrickLODGroupBoundaryMinusY,
    kSparseVoxelBrickLODGroupBoundaryMinusZ};
  constexpr std::array<uint32_t, 3> kBoundaryPlusBits{
    kSparseVoxelBrickLODGroupBoundaryPlusX,
    kSparseVoxelBrickLODGroupBoundaryPlusY,
    kSparseVoxelBrickLODGroupBoundaryPlusZ};

  std::map<GroupKey, GroupAccum> groups;
  hierarchy.nodeLODGroupIndices.assign(
      hierarchy.nodes.size(), kSparseVoxelBrickLODGroupInvalidIndex);
  hierarchy.representativeLODGroupIndices.assign(
      hierarchy.nodes.size(), kSparseVoxelBrickLODGroupInvalidIndex);
  const int64_t brickSide = static_cast<int64_t>(kSparseVoxelBrickSide);
  const int64_t groupSide = static_cast<int64_t>(groupSideBricks);

  for (uint32_t nodeIndex = 0u; nodeIndex < hierarchy.nodes.size(); ++nodeIndex)
  {
    const SparseVoxelBrickNodeGPU &node = hierarchy.nodes[nodeIndex];
    const int32_t scale = node.canonicalGridOriginScale[3];
    if (scale <= 0)
      return fail(error, "Brick LOD group node has an invalid canonical scale");
    const int64_t groupSpan =
        groupSide * brickSide * static_cast<int64_t>(scale);
    if (groupSpan <= 0 ||
        groupSpan > static_cast<int64_t>(std::numeric_limits<int32_t>::max()))
      return fail(error, "Brick LOD group span exceeds the supported canonical range");
    if (!std::isfinite(node.selfError))
      return fail(error, "Brick LOD group node error is not finite");

    GroupKey key{};
    key.level = node.level & 0xffu;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const int64_t coordinate =
          floorDivide64(
              static_cast<int64_t>(node.canonicalGridOriginScale[axis]),
              groupSpan);
      if (coordinate < std::numeric_limits<int32_t>::min() ||
          coordinate > std::numeric_limits<int32_t>::max())
        return fail(error, "Brick LOD group coordinate exceeds the supported range");
      key.coordinate[axis] = static_cast<int32_t>(coordinate);
    }

    GroupAccum &group = groups[key];
    group.nodes.push_back(nodeIndex);
    group.minimumError = std::min(group.minimumError, node.selfError);
    group.maximumError = std::max(group.maximumError, node.selfError);
    group.sideInFinestCells = groupSpan;
  }

  hierarchy.lodGroups.reserve(groups.size());
  hierarchy.lodGroupNodeIndices.reserve(hierarchy.nodes.size());
  for (const auto &[key, accum] : groups)
  {
    if (accum.nodes.empty())
      continue;
    const uint32_t groupIndex =
        static_cast<uint32_t>(hierarchy.lodGroups.size());
    const uint32_t firstNode =
        static_cast<uint32_t>(hierarchy.lodGroupNodeIndices.size());
    if (accum.nodes.size() >
        std::numeric_limits<uint32_t>::max() - hierarchy.lodGroupNodeIndices.size())
      return fail(error, "Brick LOD group node index table exceeds 32 bits");

    SparseVoxelBrickLODGroupGPU group{};
    group.firstNode = firstNode;
    group.nodeCount = static_cast<uint32_t>(accum.nodes.size());
    group.level = key.level;
    group.sharedError = accum.maximumError;
    group.minimumError =
        accum.minimumError == std::numeric_limits<float>::max()
        ? 0.0f
        : accum.minimumError;
    group.maximumError = accum.maximumError;

    std::array<int64_t, 3> groupOrigin{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      groupOrigin[axis] =
          static_cast<int64_t>(key.coordinate[axis]) * accum.sideInFinestCells;
      if (groupOrigin[axis] < std::numeric_limits<int32_t>::min() ||
          groupOrigin[axis] > std::numeric_limits<int32_t>::max())
        return fail(error, "Brick LOD group origin exceeds the supported canonical range");
      group.canonicalGridOriginScale[axis] =
          static_cast<int32_t>(groupOrigin[axis]);
      group.minimumExtent[axis] =
          static_cast<float>(static_cast<double>(groupOrigin[axis]) *
                             static_cast<double>(hierarchy.finestVoxelSize));
    }
    group.canonicalGridOriginScale[3] =
        static_cast<int32_t>(accum.sideInFinestCells);
    group.minimumExtent[3] =
        static_cast<float>(static_cast<double>(accum.sideInFinestCells) *
                           static_cast<double>(hierarchy.finestVoxelSize));

    uint32_t sharedParent = kSparseVoxelBrickInvalidIndex;
    bool hasSharedParent = true;
    for (const uint32_t nodeIndex : accum.nodes)
    {
      const SparseVoxelBrickNodeGPU &node = hierarchy.nodes[nodeIndex];
      if (node.parentIndex == kSparseVoxelBrickInvalidIndex)
      {
        hasSharedParent = false;
      }
      else if (sharedParent == kSparseVoxelBrickInvalidIndex)
      {
        sharedParent = node.parentIndex;
      }
      else if (sharedParent != node.parentIndex)
      {
        hasSharedParent = false;
      }
      const int64_t nodeSpan =
          brickSide * static_cast<int64_t>(node.canonicalGridOriginScale[3]);
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        const int64_t nodeOrigin =
            static_cast<int64_t>(node.canonicalGridOriginScale[axis]);
        if (nodeOrigin == groupOrigin[axis] &&
            occupiedMinimum(node.packedOccupiedBounds, axis) == 0u)
          group.flags |= kBoundaryMinusBits[axis];
        if (nodeOrigin + nodeSpan ==
                groupOrigin[axis] + accum.sideInFinestCells &&
            occupiedMaximum(node.packedOccupiedBounds, axis) ==
                kSparseVoxelBrickSide - 1u)
          group.flags |= kBoundaryPlusBits[axis];
      }
    }
    if (hasSharedParent &&
        sharedParent != kSparseVoxelBrickInvalidIndex &&
        sharedParent < hierarchy.nodes.size())
    {
      const SparseVoxelBrickNodeGPU &parent = hierarchy.nodes[sharedParent];
      const int64_t parentSpan =
          brickSide *
          static_cast<int64_t>(parent.canonicalGridOriginScale[3]);
      bool parentMatchesGroupBounds =
          parentSpan == accum.sideInFinestCells &&
          (parent.level & 0xffu) == key.level + 1u;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
        parentMatchesGroupBounds = parentMatchesGroupBounds &&
            static_cast<int64_t>(parent.canonicalGridOriginScale[axis]) ==
                groupOrigin[axis];
      if (parentMatchesGroupBounds)
      {
        group.representativeNode = sharedParent;
        hierarchy.representativeLODGroupIndices[sharedParent] = groupIndex;
        group.sharedError = parent.selfError;
        group.maximumError = std::max(group.maximumError, parent.selfError);
      }
    }

    hierarchy.lodGroups.push_back(group);
    for (const uint32_t nodeIndex : accum.nodes)
    {
      hierarchy.nodeLODGroupIndices[nodeIndex] = groupIndex;
      hierarchy.lodGroupNodeIndices.push_back(nodeIndex);
    }
  }

  return true;
}

bool buildSparseVoxelBrickHierarchy(
    const std::vector<SparseVoxelCenterLengthGPU> &centersLengths,
    const std::vector<SparseVoxelMaterialGPU> &materials,
    const std::vector<uint32_t> &leafVoxelIndices,
    const SparseVoxelBrickBuildSettings &settings,
    SparseVoxelBrickHierarchy &hierarchy,
    std::string *error,
    const std::vector<uint32_t> &leafInstances)
{
  hierarchy = {};
  if (leafVoxelIndices.empty())
    return fail(error, "Brick hierarchy needs at least one leaf voxel");

  float voxelSize = settings.voxelSize;
  if (!(voxelSize > 0.0f))
  {
    voxelSize = std::numeric_limits<float>::max();
    for (const uint32_t voxelIndex : leafVoxelIndices)
    {
      if (voxelIndex >= centersLengths.size())
        return fail(error, "Brick hierarchy leaf index is out of range");
      const float length = centersLengths[voxelIndex].length;
      if (length > 0.0f)
        voxelSize = std::min(voxelSize, length);
    }
  }
  if (!(voxelSize > 0.0f) || !std::isfinite(voxelSize))
    return fail(error, "Brick hierarchy voxel size is invalid");

  // Level 0: rasterize every leaf into the finest brick grid. A leaf coarser
  // than the brick voxel size covers several cells, so it is filled rather than
  // point-sampled - dropping to its centre would punch holes wherever the
  // source octree kept a coarse node.
  std::map<BrickKey, BrickCells> level;
  const int32_t brickSide = static_cast<int32_t>(kSparseVoxelBrickSide);
  for (const uint32_t voxelIndex : leafVoxelIndices)
  {
    if (voxelIndex >= centersLengths.size() || voxelIndex >= materials.size())
      return fail(error, "Brick hierarchy leaf index is out of range");
    const SparseVoxelCenterLengthGPU &leaf = centersLengths[voxelIndex];
    const float radius = 0.5f * leaf.length;
    std::array<int32_t, 3> minimumCell{};
    std::array<int32_t, 3> maximumCell{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const float low = leaf.center[axis] - radius;
      const float high = leaf.center[axis] + radius;
      if (!std::isfinite(low) || !std::isfinite(high))
        return fail(error, "Brick hierarchy leaf bounds are not finite");
      minimumCell[axis] = static_cast<int32_t>(std::floor(snappedGridCoordinate(low, voxelSize)));
      // Half-open on the far side so abutting leaves do not double-fill a cell.
      maximumCell[axis] = static_cast<int32_t>(std::ceil(snappedGridCoordinate(high, voxelSize))) - 1;
      maximumCell[axis] = std::max(maximumCell[axis], minimumCell[axis]);
    }
    for (int32_t z = minimumCell[2]; z <= maximumCell[2]; ++z)
      for (int32_t y = minimumCell[1]; y <= maximumCell[1]; ++y)
        for (int32_t x = minimumCell[0]; x <= maximumCell[0]; ++x)
        {
          const BrickKey key{floorDivide(x, brickSide), floorDivide(y, brickSide), floorDivide(z, brickSide)};
          BrickCells &brick = level[key];
          setCell(
              brick,
              cellIndex(
                  static_cast<uint32_t>(positiveModulo(x, brickSide)),
                  static_cast<uint32_t>(positiveModulo(y, brickSide)),
                  static_cast<uint32_t>(positiveModulo(z, brickSide))),
              voxelIndex);
          if (level.size() > settings.maximumBricks)
            return fail(error, "Brick hierarchy exceeded the configured brick budget");
        }
  }

  // Levels are kept as (key -> node index) so a parent can find its children by
  // doubling its own key.
  struct EmittedLevel
  {
    std::map<BrickKey, uint32_t> nodeIndices;
    float voxelSize = 0.0f;
  };
  std::vector<std::map<BrickKey, BrickCells>> levels;
  std::vector<float> levelVoxelSizes;
  levels.push_back(std::move(level));
  levelVoxelSizes.push_back(voxelSize);

  const auto contextRank = [&](const uint32_t materialIndex)
  {
    if (materialIndex >= materials.size())
      return 0u;
    const uint32_t context =
        materials[materialIndex].flags & kSparseVoxelMicroContextMask;
    if (context >= static_cast<uint32_t>(
                       SparseVoxelMicroContext::WallNegativeX) &&
        context <= static_cast<uint32_t>(
                       SparseVoxelMicroContext::WallPositiveZ))
      return 4u;
    if (sparseVoxelMicroContextIsSurface(context))
      return 3u;
    return context == static_cast<uint32_t>(
                          SparseVoxelMicroContext::FlatSurface)
        ? 1u
        : 0u;
  };

  // OR-reduce until a level fits in a single brick, or the tree stops shrinking.
  while (levels.back().size() > 1u)
  {
    const auto &finer = levels.back();
    const float finerVoxelSize = levelVoxelSizes.back();
    std::map<BrickKey, BrickCells> coarser;
    for (const auto &[key, brick] : finer)
    {
      const BrickKey parentKey{floorDivide(key[0], 2), floorDivide(key[1], 2), floorDivide(key[2], 2)};
      BrickCells &parent = coarser[parentKey];
      // The child occupies one octant of the parent brick, and each parent cell
      // is the OR of the 2x2x2 child cells under it.
      const uint32_t offsetX = static_cast<uint32_t>(positiveModulo(key[0], 2)) * (kSparseVoxelBrickSide / 2u);
      const uint32_t offsetY = static_cast<uint32_t>(positiveModulo(key[1], 2)) * (kSparseVoxelBrickSide / 2u);
      const uint32_t offsetZ = static_cast<uint32_t>(positiveModulo(key[2], 2)) * (kSparseVoxelBrickSide / 2u);
      for (uint32_t z = 0u; z < kSparseVoxelBrickSide; ++z)
        for (uint32_t y = 0u; y < kSparseVoxelBrickSide; ++y)
          for (uint32_t x = 0u; x < kSparseVoxelBrickSide; ++x)
          {
            const uint32_t index = cellIndex(x, y, z);
            if ((brick.words[index / 32u] & (1u << (index % 32u))) == 0u)
              continue;
            const uint32_t parentIndex = cellIndex(offsetX + x / 2u, offsetY + y / 2u, offsetZ + z / 2u);
            const auto material = brick.materials.find(index);
            const uint32_t candidateMaterial =
                material != brick.materials.end() ? material->second : 0u;
            const auto existing = parent.materials.find(parentIndex);
            if (existing == parent.materials.end())
              setCell(parent, parentIndex, candidateMaterial);
            else if (contextRank(candidateMaterial) >
                     contextRank(existing->second))
              existing->second = candidateMaterial;
          }
    }
    if (coarser.size() >= finer.size())
      break;
    // Measure what the reduction just lost, per parent brick.
    //
    // The signal that separates flat ground from a cliff is vertical structure
    // inside one parent cell. Each parent cell covers four child sub-columns of
    // two cells each; where all four hold the same amount of solid, the parent
    // represents them exactly and coarsening costs nothing. Where they differ,
    // the surface inside that cell moved, and the spread is how far.
    //
    // Occupancy stays the OR of the children - it has to, because the Tree64
    // and the shadow trace read this same structure and an occupancy that can
    // lose a caster is a light leak. Only the error is measured.
    for (auto &[parentKey, parent] : coarser)
    {
      float worstDeviation = 0.0f;
      for (uint32_t z = 0u; z < kSparseVoxelBrickSide; ++z)
        for (uint32_t y = 0u; y < kSparseVoxelBrickSide; ++y)
          for (uint32_t x = 0u; x < kSparseVoxelBrickSide; ++x)
          {
            const uint32_t index = cellIndex(x, y, z);
            if ((parent.words[index / 32u] & (1u << (index % 32u))) == 0u)
              continue;
            // The four child sub-columns under this parent cell, in the child
            // brick that owns them.
            const int32_t childKeyX = parentKey[0] * 2 +
                static_cast<int32_t>(x / (kSparseVoxelBrickSide / 2u));
            const int32_t childKeyY = parentKey[1] * 2 +
                static_cast<int32_t>(y / (kSparseVoxelBrickSide / 2u));
            const int32_t childKeyZ = parentKey[2] * 2 +
                static_cast<int32_t>(z / (kSparseVoxelBrickSide / 2u));
            const auto child = finer.find({childKeyX, childKeyY, childKeyZ});
            if (child == finer.end())
              continue;
            const uint32_t baseX =
                (x % (kSparseVoxelBrickSide / 2u)) * 2u;
            const uint32_t baseY =
                (y % (kSparseVoxelBrickSide / 2u)) * 2u;
            const uint32_t baseZ =
                (z % (kSparseVoxelBrickSide / 2u)) * 2u;
            uint32_t lowest = 3u;
            uint32_t highest = 0u;
            for (uint32_t subZ = 0u; subZ != 2u; ++subZ)
              for (uint32_t subX = 0u; subX != 2u; ++subX)
              {
                uint32_t solid = 0u;
                for (uint32_t subY = 0u; subY != 2u; ++subY)
                {
                  const uint32_t childIndex = cellIndex(
                      baseX + subX, baseY + subY, baseZ + subZ);
                  if ((child->second.words[childIndex / 32u] &
                       (1u << (childIndex % 32u))) != 0u)
                    ++solid;
                }
                lowest = std::min(lowest, solid);
                highest = std::max(highest, solid);
              }
            if (highest >= lowest)
              worstDeviation = std::max(
                  worstDeviation,
                  static_cast<float>(highest - lowest) * finerVoxelSize);
          }
      // Monotonic: a parent is never finer than the children it replaces, or
      // the two-test cut admits more than one node on a root-to-leaf path.
      for (const auto &[childKey, childBrick] : finer)
        if (floorDivide(childKey[0], 2) == parentKey[0] &&
            floorDivide(childKey[1], 2) == parentKey[1] &&
            floorDivide(childKey[2], 2) == parentKey[2])
          worstDeviation = std::max(worstDeviation, childBrick.selfError);
      parent.selfError = worstDeviation;
    }
    levels.push_back(std::move(coarser));
    levelVoxelSizes.push_back(finerVoxelSize * 2.0f);
  }

  // Palettes, one per instance, built before any node is emitted because a
  // node's indices are resolved against a finished palette.
  //
  // Only the finest level is surveyed. Coarser levels are OR-reductions that
  // reuse their children's source voxel indices, so they introduce no record
  // the finest level does not already contain - surveying them would double
  // count and bias the weights toward whatever survives coarsening.
  const uint32_t paletteStride = std::max(settings.paletteSize, 1u);
  hierarchy.paletteStride = paletteStride;
  uint32_t instanceCount = 1u;
  for (const uint32_t instance : leafInstances)
    instanceCount = std::max(instanceCount, instance + 1u);
  if (leafInstances.empty())
    instanceCount = 1u;
  const auto microDescriptorId = [&](const uint32_t materialIndex)
  {
    if (materialIndex >= materials.size())
      return uint16_t{0u};
    uint16_t descriptorId = 0u;
    if (settings.voxelMaterials != nullptr)
      descriptorId = settings.voxelMaterials->handleForSourceMaterial(
          materials[materialIndex].materialId);
    else if (settings.microMaterials != nullptr)
      descriptorId = settings.microMaterials->descriptorId(
          materials[materialIndex].materialId);
    if (descriptorId == 0u)
      return uint16_t{0u};
    return descriptorId;
  };
  std::vector<std::map<BrickPaletteKey, uint64_t>> instanceRecords(instanceCount);
  std::map<BrickPaletteKey, uint64_t> globalMicroRecords;
  std::vector<std::map<BrickSecondarySource, uint64_t>> instanceSecondaryRecords(
      instanceCount);
  for (const auto &[key, brick] : levels.front())
  {
    (void)key;
    for (const auto &[cell, materialIndex] : brick.materials)
    {
      (void)cell;
      if (materialIndex >= materials.size())
        continue;
      const SparseVoxelMaterialGPU &sourceMaterial = materials[materialIndex];
      const SparseVoxelBrickMaterialGPU record =
          packSparseVoxelBrickMaterial(sourceMaterial);
      const uint16_t descriptorId = microDescriptorId(materialIndex);
      const uint32_t instance =
          materialIndex < leafInstances.size() ? leafInstances[materialIndex] : 0u;
      if (instance >= instanceRecords.size())
        continue;
      const BrickPaletteKey topKey{
        record.packed[0], record.packed[1], descriptorId};
      ++instanceRecords[instance][topKey];
      if (descriptorId != 0u)
        globalMicroRecords.emplace(topKey, 1u);
      if (hasSparseVoxelSecondaryMaterial(sourceMaterial))
      {
        const SparseVoxelBrickMaterialGPU bottom =
            packSparseVoxelBrickBottomMaterial(sourceMaterial);
        ++instanceRecords[instance][
            BrickPaletteKey{bottom.packed[0], bottom.packed[1], 0u}];
        ++instanceSecondaryRecords[instance][BrickSecondarySource{
          {record.packed[0], record.packed[1]},
          {bottom.packed[0], bottom.packed[1]},
          descriptorId,
          sourceMaterial.reserved1 &
              kSparseVoxelMaterialSecondaryFaceMask}];
      }
    }
  }
  // A straddling brick may choose either contributing instance's palette. Add
  // every exact micro-capable record to every instance palette so that choice
  // can approximate colour but can never silently substitute another shape.
  for (auto &records : instanceRecords)
    for (const auto &[key, count] : globalMicroRecords)
      records.emplace(key, count);
  hierarchy.palette.assign(
      static_cast<size_t>(instanceCount) * paletteStride,
      SparseVoxelBrickMaterialGPU{});
  hierarchy.paletteDescriptorIds.assign(
      hierarchy.palette.size(), uint16_t{0u});
  std::vector<uint32_t> paletteCounts(instanceCount, 0u);
  // Exact-record lookup per instance, so the common case - an instance with
  // fewer distinct records than the palette holds - costs a hash probe rather
  // than a linear scan over 256 entries for every one of millions of voxels.
  std::vector<std::map<BrickPaletteKey, uint8_t>> paletteLookup(instanceCount);
  // Resolved nearest entries and their error, keyed by palette and record.
  //
  // The fallback is a linear scan over the palette, and on a scene where most
  // voxels are approximated that is 256 distance computations *per voxel* -
  // measured at over ten minutes on Sponza before this cache. Distinct records
  // are orders of magnitude fewer than voxels, and the answer depends only on
  // (palette, record), so each one is resolved once. Selecting a straddling
  // brick's palette by error rather than by cell count reads the same cache,
  // which is what keeps that search affordable.
  struct NearestEntry
  {
    uint8_t entry = 0u;
    double error = 0.0;
  };
  std::vector<std::map<BrickPaletteKey, NearestEntry>> nearestCache(
      instanceCount);
  const auto resolveEntry =
      [&](const uint32_t palette,
          const SparseVoxelBrickMaterialGPU &record,
          const uint16_t descriptorId) -> NearestEntry
  {
    const BrickPaletteKey key{
      record.packed[0], record.packed[1] & ~kSparseVoxelBrickMicroEnabled,
      descriptorId};
    const auto exact = paletteLookup[palette].find(key);
    if (exact != paletteLookup[palette].end())
      return NearestEntry{exact->second, 0.0};
    const auto cached = nearestCache[palette].find(key);
    if (cached != nearestCache[palette].end())
      return cached->second;
    const uint32_t base = palette * paletteStride;
    const uint8_t entry = nearestBrickPaletteEntry(
        hierarchy.palette, hierarchy.paletteDescriptorIds, base,
        paletteCounts[palette], record, descriptorId);
    const NearestEntry resolved{
      entry,
      std::sqrt(static_cast<double>(brickMaterialDistanceSquared(
          brickMaterialFeature(record),
          brickMaterialFeature(hierarchy.palette[base + entry]))))};
    nearestCache[palette].emplace(key, resolved);
    return resolved;
  };
  uint64_t approximatedVoxels = 0u;
  for (uint32_t instance = 0u; instance < instanceCount; ++instance)
  {
    if (!globalMicroRecords.empty() &&
        instanceRecords[instance].size() > paletteStride)
      return fail(
          error,
          "Micro-aware brick palette exceeds its exact descriptor capacity");
    const std::vector<BuiltPaletteEntry> entries = buildBrickPalette(
        instanceRecords[instance], paletteStride,
        settings.paletteRefinementIterations);
    paletteCounts[instance] = static_cast<uint32_t>(entries.size());
    if (entries.size() >= instanceRecords[instance].size())
      ++hierarchy.paletteExactInstances;
    for (uint32_t entry = 0u; entry < entries.size(); ++entry)
    {
      hierarchy.palette[static_cast<size_t>(instance) * paletteStride + entry] =
          entries[entry].material;
      hierarchy.paletteDescriptorIds[
          static_cast<size_t>(instance) * paletteStride + entry] =
          entries[entry].descriptorId;
      if (entries[entry].descriptorId != 0u)
        hierarchy.palette[
            static_cast<size_t>(instance) * paletteStride + entry]
            .packed[1] |= kSparseVoxelBrickMicroEnabled;
      paletteLookup[instance].emplace(
          BrickPaletteKey{
            entries[entry].material.packed[0],
            entries[entry].material.packed[1] &
                ~kSparseVoxelBrickMicroEnabled,
            entries[entry].descriptorId},
          static_cast<uint8_t>(entry));
    }
    // Resolve each authored bottom colour into this same 256-entry palette.
    // Several source records can cluster onto one top entry; in that overflow
    // case retain the weighted-most-common secondary relationship. Exact
    // terrain palettes never take this fallback.
    std::map<uint8_t, std::map<uint32_t, uint64_t>> secondaryVotes;
    const uint32_t base = instance * paletteStride;
    for (const auto &[secondary, count] : instanceSecondaryRecords[instance])
    {
      const SparseVoxelBrickMaterialGPU top{{
        secondary.top.first, secondary.top.second}};
      const SparseVoxelBrickMaterialGPU bottom{{
        secondary.bottom.first, secondary.bottom.second}};
      const uint8_t topEntry = nearestBrickPaletteEntry(
          hierarchy.palette, hierarchy.paletteDescriptorIds, base,
          paletteCounts[instance], top, secondary.descriptorId);
      const uint8_t bottomEntry = nearestBrickPaletteEntry(
          hierarchy.palette, hierarchy.paletteDescriptorIds, base,
          paletteCounts[instance], bottom, 0u);
      const uint32_t metadata = sparseVoxelBrickSecondaryMetadata(
          bottomEntry, secondary.faceModes);
      secondaryVotes[topEntry][metadata] += count;
    }
    for (const auto &[topEntry, votes] : secondaryVotes)
    {
      uint32_t selected = 0u;
      uint64_t selectedCount = 0u;
      for (const auto &[metadata, count] : votes)
      {
        if (count > selectedCount)
        {
          selected = metadata;
          selectedCount = count;
        }
      }
      hierarchy.palette[base + topEntry].packed[1] =
          (hierarchy.palette[base + topEntry].packed[1] &
           ~kSparseVoxelBrickSecondaryMetadataMask) |
          selected;
    }
    // Weighted residual over the records this instance actually carries. An
    // exact palette contributes nothing, which is why the mean is reported
    // against the approximated voxels alone rather than against every voxel -
    // averaging over an exact majority would report every scene as perfect.
    for (const auto &[key, count] : instanceRecords[instance])
    {
      if (paletteLookup[instance].count(key) != 0u)
        continue;
      const SparseVoxelBrickMaterialGPU record{{key.packed0, key.packed1}};
      const uint8_t nearest = nearestBrickPaletteEntry(
          hierarchy.palette, hierarchy.paletteDescriptorIds, base,
          paletteCounts[instance], record, key.descriptorId);
      const double error = std::sqrt(static_cast<double>(
          brickMaterialDistanceSquared(
              brickMaterialFeature(record),
              brickMaterialFeature(hierarchy.palette[base + nearest]))));
      hierarchy.paletteMeanError += error * static_cast<double>(count);
      approximatedVoxels += count;
      hierarchy.paletteMaximumError =
          std::max(hierarchy.paletteMaximumError, error);
    }
  }
  hierarchy.paletteInstanceCount = instanceCount;
  if (approximatedVoxels != 0u)
    hierarchy.paletteMeanError /= static_cast<double>(approximatedVoxels);

  std::vector<EmittedLevel> emitted(levels.size());
  // Finest level first: a node references children one level finer, so those
  // must already be emitted when the parent is written.
  for (size_t levelIndex = 0u; levelIndex < levels.size(); ++levelIndex)
  {
    const auto &bricks = levels[levelIndex];
    const float levelVoxelSize = levelVoxelSizes[levelIndex];
    emitted[levelIndex].voxelSize = levelVoxelSize;
    for (const auto &[key, brick] : bricks)
    {
      if (levelIndex >= 31u)
        return fail(error, "Brick hierarchy canonical grid scale exceeds 32 bits");
      const int64_t finestScale = int64_t{1} << levelIndex;
      std::array<int32_t, 3> canonicalOrigin{};
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        const int64_t value = static_cast<int64_t>(key[axis]) *
            brickSide * finestScale;
        if (value < std::numeric_limits<int32_t>::min() ||
            value > std::numeric_limits<int32_t>::max())
          return fail(error, "Brick hierarchy canonical grid origin exceeds 32 bits");
        canonicalOrigin[axis] = static_cast<int32_t>(value);
      }
      SparseVoxelBrickNodeGPU node{};
      node.minimumVoxelSize = {
        static_cast<float>(key[0] * brickSide) * levelVoxelSize,
        static_cast<float>(key[1] * brickSide) * levelVoxelSize,
        static_cast<float>(key[2] * brickSide) * levelVoxelSize,
        levelVoxelSize,
      };
      node.canonicalGridOriginScale = {
        canonicalOrigin[0], canonicalOrigin[1], canonicalOrigin[2],
        static_cast<int32_t>(finestScale),
      };
      node.level = static_cast<uint32_t>(levelIndex);
      // A floor at a fraction of the voxel keeps a brick that is geometrically
      // exact but materially varied - flat ground with texture on it - from
      // claiming zero error and never refining at any distance.
      node.selfError = std::max(
          brick.selfError, levelVoxelSize * kSparseVoxelBrickErrorFloor);
      node.occupancyIndex = static_cast<uint32_t>(hierarchy.occupancy.size());
      node.materialBase = static_cast<uint32_t>(hierarchy.materialIndices.size());
      node.occupiedCount = static_cast<uint32_t>(brick.materials.size());
      // Which instance's palette this brick draws from. `brick.materials` holds
      // the source voxel index at every level - the coarsening pass carries the
      // child's index into the parent - so this works the same for an
      // OR-reduced brick as for a leaf one. A brick has to pick one palette,
      // because the lookup it feeds takes a single base offset.
      //
      // Chosen by the error it actually produces, not by which instance
      // contributes the most cells. The two differ exactly where it matters: a
      // brick can be 60% one instance and 40% another whose colours the first
      // palette cannot represent at all, and counting cells picks the palette
      // that is wrong for the harder 40%. Evaluating the candidates costs one
      // cached lookup per distinct record per candidate, and the candidates are
      // only the instances actually present.
      if (!leafInstances.empty())
      {
        std::map<uint32_t, uint32_t> instanceCounts;
        for (const auto &[cell, materialIndex] : brick.materials)
        {
          (void)cell;
          if (materialIndex < leafInstances.size())
            ++instanceCounts[leafInstances[materialIndex]];
        }
        uint32_t totalCount = 0u;
        uint32_t majorityCount = 0u;
        for (const auto &[instanceIndex, count] : instanceCounts)
        {
          totalCount += count;
          majorityCount = std::max(majorityCount, count);
        }
        if (instanceCounts.size() == 1u)
        {
          node.instanceIndex = instanceCounts.begin()->first;
        }
        else if (!instanceCounts.empty())
        {
          ++hierarchy.paletteStraddlingBricks;
          hierarchy.paletteStraddlingVoxels += totalCount - majorityCount;
          double bestError = std::numeric_limits<double>::max();
          for (const auto &[candidate, count] : instanceCounts)
          {
            (void)count;
            if (candidate >= paletteCounts.size())
              continue;
            double error = 0.0;
            for (const auto &[cell, materialIndex] : brick.materials)
            {
              (void)cell;
              const SparseVoxelBrickMaterialGPU record =
                  materialIndex < materials.size()
                      ? packSparseVoxelBrickMaterial(materials[materialIndex])
                      : SparseVoxelBrickMaterialGPU{};
              error += resolveEntry(
                  candidate, record, microDescriptorId(materialIndex)).error;
            }
            // Ties resolve to the lowest instance index, which map order
            // already gives, so a rebuild picks the same palette.
            if (error < bestError)
            {
              bestError = error;
              node.instanceIndex = candidate;
            }
          }
        }
      }
      std::array<uint32_t, 3> occupiedMinimum{
        kSparseVoxelBrickSide - 1u, kSparseVoxelBrickSide - 1u, kSparseVoxelBrickSide - 1u};
      std::array<uint32_t, 3> occupiedMaximum{0u, 0u, 0u};
      for (const auto &[cell, materialIndex] : brick.materials)
      {
        (void)materialIndex;
        const std::array<uint32_t, 3> coordinate{
          cell % kSparseVoxelBrickSide,
          (cell / kSparseVoxelBrickSide) % kSparseVoxelBrickSide,
          cell / (kSparseVoxelBrickSide * kSparseVoxelBrickSide)};
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
          occupiedMinimum[axis] = std::min(occupiedMinimum[axis], coordinate[axis]);
          occupiedMaximum[axis] = std::max(occupiedMaximum[axis], coordinate[axis]);
        }
      }
      node.packedOccupiedBounds =
          (occupiedMinimum[0] & 7u) | ((occupiedMinimum[1] & 7u) << 3u) |
          ((occupiedMinimum[2] & 7u) << 6u) | ((occupiedMaximum[0] & 7u) << 9u) |
          ((occupiedMaximum[1] & 7u) << 12u) | ((occupiedMaximum[2] & 7u) << 15u);

      // Occupancy words with their running popcount, and the 4-cubed coarse
      // mask derived from them. Both are pure functions of `brick.words`, so
      // they cost nothing at runtime and remove a loop from every fragment.
      SparseVoxelBrickOccupancyGPU occupancy{};
      uint32_t prefix = 0u;
      for (uint32_t word = 0u; word < kSparseVoxelBrickOccupancyWords; ++word)
      {
        occupancy.cells[word].bits = brick.words[word];
        occupancy.cells[word].prefix = prefix;
        prefix += popcount32(brick.words[word]);
      }
      hierarchy.occupancy.push_back(occupancy);

      uint64_t coarse = 0u;
      for (const auto &[cell, materialIndex] : brick.materials)
      {
        (void)materialIndex;
        const uint32_t coarseX = (cell % kSparseVoxelBrickSide) / 2u;
        const uint32_t coarseY = ((cell / kSparseVoxelBrickSide) % kSparseVoxelBrickSide) / 2u;
        const uint32_t coarseZ = (cell / (kSparseVoxelBrickSide * kSparseVoxelBrickSide)) / 2u;
        const uint32_t coarseIndex =
            coarseX + kSparseVoxelBrickCoarseSide * (coarseY + kSparseVoxelBrickCoarseSide * coarseZ);
        coarse |= uint64_t{1} << coarseIndex;
      }
      node.coarseOccupancyLow = static_cast<uint32_t>(coarse & 0xffffffffu);
      node.coarseOccupancyHigh = static_cast<uint32_t>(coarse >> 32u);

      // Palette indices in occupancy bit order, so the popcount of the
      // preceding bits addresses them. `brick.materials` is keyed by linear
      // cell index and std::map iterates in that order, which is the same
      // order.
      //
      // The entry is resolved against *this node's* palette, which is the
      // majority instance's. A cell contributed by a different instance in a
      // straddling brick therefore resolves to the nearest entry its neighbour
      // offers rather than an exact one - the alternative is a palette per
      // brick, which costs far more than the handful of cells involved.
      const uint32_t nodePalette =
          node.instanceIndex < paletteCounts.size() ? node.instanceIndex : 0u;
      uint32_t nodePaletteBase = nodePalette * hierarchy.paletteStride;
      uint32_t nodePaletteCount =
          nodePalette < paletteCounts.size() ? paletteCounts[nodePalette] : 0u;
      // A brick drawing from more than one instance resolves its minority cells
      // through the majority's palette. Giving those bricks a dedicated palette
      // was tried and measured worse on both axes: palette entries went
      // 6400 -> 1,093,730 (material bytes 6.7 MB -> 15.4 MB, a third of the
      // saving) while the all-levels maximum error rose 166 -> 256. The mean
      // improved (3.012 -> 2.490) but not enough to buy that back, and the
      // straddling cells are 5.3% of voxels confined to instance boundaries.
      node.paletteBase = nodePaletteBase;
      const auto &activeLookup = paletteLookup[nodePalette];
      for (const auto &[cell, materialIndex] : brick.materials)
      {
        (void)cell;
        const SparseVoxelBrickMaterialGPU record =
            materialIndex < materials.size()
                ? packSparseVoxelBrickMaterial(materials[materialIndex])
                : SparseVoxelBrickMaterialGPU{};
        const NearestEntry resolved = resolveEntry(
            nodePalette, record, microDescriptorId(materialIndex));
        hierarchy.materialIndices.push_back(resolved.entry);
        uint64_t terrainPatch = 0u;
        if (settings.terrainSurfacePatches != nullptr &&
            materialIndex < settings.terrainSurfacePatches->size())
          terrainPatch = (*settings.terrainSurfacePatches)[materialIndex];
        if (settings.buildMicroSurface)
        hierarchy.microSurfaceData.push_back(materialIndex < materials.size()
            ? packSparseVoxelMicroContextData(
                  materials[materialIndex].flags &
                      kSparseVoxelMicroContextMask,
                  terrainPatch)
            : 0u);
        ++hierarchy.paletteEmittedVoxels;
        if (resolved.error > 0.0)
        {
          ++hierarchy.paletteEmittedApproximated;
          hierarchy.paletteEmittedMeanError += resolved.error;
          hierarchy.paletteEmittedMaximumError =
              std::max(hierarchy.paletteEmittedMaximumError, resolved.error);
        }
      }

      // Children live one level finer, at the doubled key. They are recorded
      // in a side table because nodes are emitted in spatial-key order, so a
      // node's children are not adjacent in the node array.
      if (levelIndex > 0u)
      {
        const auto &childLevel = emitted[levelIndex - 1u].nodeIndices;
        node.firstChild = static_cast<uint32_t>(hierarchy.childIndices.size());
        for (uint32_t octant = 0u; octant < 8u; ++octant)
        {
          const BrickKey childKey{
            key[0] * 2 + static_cast<int32_t>(octant & 1u),
            key[1] * 2 + static_cast<int32_t>((octant >> 1u) & 1u),
            key[2] * 2 + static_cast<int32_t>((octant >> 2u) & 1u),
          };
          const auto child = childLevel.find(childKey);
          if (child == childLevel.end())
            continue;
          hierarchy.childIndices.push_back(child->second);
          hierarchy.nodes[child->second].parentIndex = static_cast<uint32_t>(hierarchy.nodes.size());
          // The child carries its parent's error so cut selection stays two
          // independent tests. Roots keep the infinity set at construction.
          hierarchy.nodes[child->second].parentError = node.selfError;
          node.childCount += 1u;
        }
      }
      emitted[levelIndex].nodeIndices.emplace(key, static_cast<uint32_t>(hierarchy.nodes.size()));
      hierarchy.nodes.push_back(node);
    }
  }

  if (hierarchy.paletteEmittedApproximated != 0u)
    hierarchy.paletteEmittedMeanError /=
        static_cast<double>(hierarchy.paletteEmittedApproximated);
  hierarchy.levelCount = static_cast<uint32_t>(levels.size());
  hierarchy.finestVoxelSize = voxelSize;
  for (const auto &[key, nodeIndex] : emitted.back().nodeIndices)
  {
    (void)key;
    hierarchy.rootIndices.push_back(nodeIndex);
  }
  hierarchy.rootCount = static_cast<uint32_t>(hierarchy.rootIndices.size());
  if (hierarchy.nodes.empty())
    return fail(error, "Brick hierarchy produced no nodes");
  if (!buildSparseVoxelBrickLODGroups(
          hierarchy, settings.lodGroupSideBricks, error))
    return false;
  return true;
}

} // namespace voxel::svraster
