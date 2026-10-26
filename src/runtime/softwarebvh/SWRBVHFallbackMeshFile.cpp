#include "softwarebvh/SWRBVHFallbackMeshFile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "algorithm/BinaryIO.hpp"
#include "virtualgeometry/FallbackMeshFile.hpp"

namespace softwarebvh
{

namespace
{

bool writeBytes(FILE *file, const void *data, const size_t size)
{
  return size == 0u || std::fwrite(data, 1u, size, file) == size;
}

bool readBytes(FILE *file, void *data, const size_t size)
{
  return size == 0u || std::fread(data, 1u, size, file) == size;
}

bool writeU32(FILE *file, uint32_t value)
{
  if (!virtualgeometry::isLittleEndian())
  {
    value = virtualgeometry::byteswap(value);
  }
  return writeBytes(file, &value, sizeof(value));
}

bool readU32(FILE *file, uint32_t &value)
{
  if (!readBytes(file, &value, sizeof(value)))
  {
    return false;
  }
  if (!virtualgeometry::isLittleEndian())
  {
    value = virtualgeometry::byteswap(value);
  }
  return true;
}

bool writeU16(FILE *file, uint16_t value)
{
  if (!virtualgeometry::isLittleEndian())
  {
    value = virtualgeometry::byteswap(value);
  }
  return writeBytes(file, &value, sizeof(value));
}

bool readU16(FILE *file, uint16_t &value)
{
  if (!readBytes(file, &value, sizeof(value)))
  {
    return false;
  }
  if (!virtualgeometry::isLittleEndian())
  {
    value = virtualgeometry::byteswap(value);
  }
  return true;
}

bool writeF32(FILE *file, const float value)
{
  static_assert(sizeof(float) == sizeof(uint32_t), "Expected 32-bit float storage");
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));
  return writeU32(file, bits);
}

bool readF32(FILE *file, float &value)
{
  uint32_t bits = 0u;
  if (!readU32(file, bits))
  {
    return false;
  }
  std::memcpy(&value, &bits, sizeof(value));
  return true;
}

bool writeCanonicalNode(FILE *file, const CWBVHBuilder::CanonicalNode &node)
{
  bool ok = true;
  for (const float value : node.bounds.minPoint)
    ok = ok && writeF32(file, value);
  for (const float value : node.bounds.maxPoint)
    ok = ok && writeF32(file, value);
  ok = ok && writeU32(file, node.parent);
  ok = ok && writeU32(file, node.left);
  ok = ok && writeU32(file, node.right);
  ok = ok && writeU32(file, node.primitiveIndex);
  ok = ok && writeU32(file, node.subtreePrimitiveCount);
  ok = ok && writeU32(file, node.isLeaf ? 1u : 0u);
  return ok;
}

bool readCanonicalNode(FILE *file, CWBVHBuilder::CanonicalNode &node)
{
  bool ok = true;
  for (float &value : node.bounds.minPoint)
    ok = ok && readF32(file, value);
  for (float &value : node.bounds.maxPoint)
    ok = ok && readF32(file, value);
  uint32_t isLeaf = 0u;
  ok = ok && readU32(file, node.parent);
  ok = ok && readU32(file, node.left);
  ok = ok && readU32(file, node.right);
  ok = ok && readU32(file, node.primitiveIndex);
  ok = ok && readU32(file, node.subtreePrimitiveCount);
  ok = ok && readU32(file, isLeaf);
  node.isLeaf = isLeaf != 0u;
  return ok;
}

bool writeTriTransform(FILE *file, const CWBVHBuilder::TriBaryTransform &transform)
{
  bool ok = true;
  for (const float value : transform.row0)
    ok = ok && writeF32(file, value);
  for (const float value : transform.row1)
    ok = ok && writeF32(file, value);
  for (const float value : transform.row2)
    ok = ok && writeF32(file, value);
  return ok;
}

bool readTriTransform(FILE *file, CWBVHBuilder::TriBaryTransform &transform)
{
  bool ok = true;
  for (float &value : transform.row0)
    ok = ok && readF32(file, value);
  for (float &value : transform.row1)
    ok = ok && readF32(file, value);
  for (float &value : transform.row2)
    ok = ok && readF32(file, value);
  return ok;
}

bool writeFallbackBlob(FILE *file, const std::string &path, const std::vector<virtualgeometry::FallbackMeshData> &meshes)
{
  const std::string tempFallbackPath = path + ".fallback.tmp";
  const bool saved = virtualgeometry::FallbackMeshFile::save(tempFallbackPath, meshes);
  std::FILE *fallbackFile = saved ? std::fopen(tempFallbackPath.c_str(), "rb") : nullptr;
  bool ok = saved && fallbackFile != nullptr;

  if (ok)
  {
    std::fseek(fallbackFile, 0, SEEK_END);
    const long fallbackFileSize = std::ftell(fallbackFile);
    std::fseek(fallbackFile, 0, SEEK_SET);
    ok = ok && writeU32(file, fallbackFileSize < 0 ? 0u : static_cast<uint32_t>(fallbackFileSize));
    std::vector<uint8_t> fallbackBytes(fallbackFileSize < 0 ? 0u : static_cast<size_t>(fallbackFileSize));
    if (!fallbackBytes.empty())
    {
      ok = ok && readBytes(fallbackFile, fallbackBytes.data(), fallbackBytes.size());
      ok = ok && writeBytes(file, fallbackBytes.data(), fallbackBytes.size());
    }
  }

  if (fallbackFile != nullptr)
    std::fclose(fallbackFile);
  std::remove(tempFallbackPath.c_str());
  return ok;
}

bool readFallbackBlob(FILE *file, const std::string &path, std::vector<virtualgeometry::FallbackMeshData> &outMeshes)
{
  uint32_t fallbackBlobSize = 0u;
  bool ok = readU32(file, fallbackBlobSize);
  std::vector<uint8_t> fallbackBytes(fallbackBlobSize);
  if (ok && !fallbackBytes.empty())
    ok = ok && readBytes(file, fallbackBytes.data(), fallbackBytes.size());

  const std::string tempFallbackPath = path + ".fallback.load.tmp";
  if (ok)
  {
    FILE *tempFallbackFile = std::fopen(tempFallbackPath.c_str(), "wb");
    if (tempFallbackFile == nullptr)
    {
      ok = false;
    }
    else
    {
      ok = ok && writeBytes(tempFallbackFile, fallbackBytes.data(), fallbackBytes.size());
      std::fclose(tempFallbackFile);
    }
  }

  if (ok)
    ok = virtualgeometry::FallbackMeshFile::load(tempFallbackPath, outMeshes);

  std::remove(tempFallbackPath.c_str());
  return ok;
}

bool writeBuildResult(FILE *file, const CWBVHBuilder::BuildResult &build)
{
  bool ok = true;
  ok = ok && writeU32(file, build.primitiveCount);
  ok = ok && writeU32(file, build.rootNodeIndex);
  ok = ok && writeU32(file, static_cast<uint32_t>(build.primitiveRemap.size()));
  ok = ok && writeU32(file, static_cast<uint32_t>(build.triangleTransforms.size()));
  ok = ok && writeU32(file, static_cast<uint32_t>(build.canonicalNodes.size()));

  for (const uint32_t primitiveIndex : build.primitiveRemap)
    ok = ok && writeU32(file, primitiveIndex);
  for (const CWBVHBuilder::TriBaryTransform &transform : build.triangleTransforms)
    ok = ok && writeTriTransform(file, transform);
  for (const CWBVHBuilder::CanonicalNode &node : build.canonicalNodes)
    ok = ok && writeCanonicalNode(file, node);
  return ok;
}

bool readBuildResult(FILE *file, const virtualgeometry::FallbackMeshData &fallbackMesh, CWBVHBuilder::BuildResult &outBuild)
{
  uint32_t primitiveRemapCount = 0u;
  uint32_t triangleTransformCount = 0u;
  uint32_t canonicalNodeCount = 0u;

  bool ok = true;
  ok = ok && readU32(file, outBuild.primitiveCount);
  ok = ok && readU32(file, outBuild.rootNodeIndex);
  ok = ok && readU32(file, primitiveRemapCount);
  ok = ok && readU32(file, triangleTransformCount);
  ok = ok && readU32(file, canonicalNodeCount);

  outBuild.localBounds.minPoint[0] = fallbackMesh.boundsMin[0];
  outBuild.localBounds.minPoint[1] = fallbackMesh.boundsMin[1];
  outBuild.localBounds.minPoint[2] = fallbackMesh.boundsMin[2];
  outBuild.localBounds.maxPoint[0] = fallbackMesh.boundsMax[0];
  outBuild.localBounds.maxPoint[1] = fallbackMesh.boundsMax[1];
  outBuild.localBounds.maxPoint[2] = fallbackMesh.boundsMax[2];

  outBuild.primitiveRemap.resize(primitiveRemapCount);
  outBuild.triangleTransforms.resize(triangleTransformCount);
  outBuild.canonicalNodes.resize(canonicalNodeCount);

  for (uint32_t &primitiveIndex : outBuild.primitiveRemap)
    ok = ok && readU32(file, primitiveIndex);
  for (CWBVHBuilder::TriBaryTransform &transform : outBuild.triangleTransforms)
    ok = ok && readTriTransform(file, transform);
  for (CWBVHBuilder::CanonicalNode &node : outBuild.canonicalNodes)
    ok = ok && readCanonicalNode(file, node);

  return ok;
}

bool writeCard(FILE *file, const RBVHCardPrimitive &card)
{
  bool ok = true;
  for (const float value : card.origin)
    ok = ok && writeF32(file, value);
  ok = ok && writeU32(file, card.flags);
  for (const float value : card.axisU)
    ok = ok && writeF32(file, value);
  ok = ok && writeF32(file, card.halfExtentU);
  for (const float value : card.axisV)
    ok = ok && writeF32(file, value);
  ok = ok && writeF32(file, card.halfExtentV);
  for (const float value : card.axisN)
    ok = ok && writeF32(file, value);
  ok = ok && writeF32(file, card.depthMin);
  for (const float value : card.boundsMin)
    ok = ok && writeF32(file, value);
  ok = ok && writeF32(file, card.depthMax);
  for (const float value : card.boundsMax)
    ok = ok && writeF32(file, value);
  ok = ok && writeF32(file, card.samplingError);
  ok = ok && writeU32(file, card.atlasX);
  ok = ok && writeU32(file, card.atlasY);
  ok = ok && writeU32(file, card.atlasWidth);
  ok = ok && writeU32(file, card.atlasHeight);
  ok = ok && writeU32(file, card.sourceNodeIndex);
  ok = ok && writeU32(file, card.sourcePrimitiveOffset);
  ok = ok && writeU32(file, card.sourcePrimitiveCount);
  ok = ok && writeU32(file, card._padding0);
  return ok;
}

bool readCard(FILE *file, RBVHCardPrimitive &card)
{
  bool ok = true;
  for (float &value : card.origin)
    ok = ok && readF32(file, value);
  ok = ok && readU32(file, card.flags);
  for (float &value : card.axisU)
    ok = ok && readF32(file, value);
  ok = ok && readF32(file, card.halfExtentU);
  for (float &value : card.axisV)
    ok = ok && readF32(file, value);
  ok = ok && readF32(file, card.halfExtentV);
  for (float &value : card.axisN)
    ok = ok && readF32(file, value);
  ok = ok && readF32(file, card.depthMin);
  for (float &value : card.boundsMin)
    ok = ok && readF32(file, value);
  ok = ok && readF32(file, card.depthMax);
  for (float &value : card.boundsMax)
    ok = ok && readF32(file, value);
  ok = ok && readF32(file, card.samplingError);
  ok = ok && readU32(file, card.atlasX);
  ok = ok && readU32(file, card.atlasY);
  ok = ok && readU32(file, card.atlasWidth);
  ok = ok && readU32(file, card.atlasHeight);
  ok = ok && readU32(file, card.sourceNodeIndex);
  ok = ok && readU32(file, card.sourcePrimitiveOffset);
  ok = ok && readU32(file, card.sourcePrimitiveCount);
  ok = ok && readU32(file, card._padding0);
  return ok;
}

uint32_t floorLog2U32(const uint32_t value)
{
  uint32_t result = 0u;
  uint32_t shifted = value;
  while (shifted > 1u)
  {
    shifted >>= 1u;
    ++result;
  }
  return result;
}

uint32_t ceilLog2U32(const uint32_t value)
{
  if (value <= 1u)
  {
    return 0u;
  }
  return floorLog2U32(value - 1u) + 1u;
}

uint32_t rbvhDepthMipLevelOffset(const uint32_t atlasSize, const uint32_t mipLevel)
{
  uint32_t offset = 0u;
  uint32_t levelSize = std::max(atlasSize >> 1u, 1u);
  for (uint32_t level = 1u; level < mipLevel; ++level)
  {
    offset += levelSize * levelSize;
    levelSize = std::max(levelSize >> 1u, 1u);
  }
  return offset;
}

void assignCardDepthMipOffsets(RBVHBuildResult &build)
{
  const uint32_t atlasMipLevelCount = build.atlasSize > 1u ? floorLog2U32(build.atlasSize) : 0u;
  for (RBVHCardPrimitive &card : build.cards)
  {
    const uint32_t cardMaxDimension = std::max(card.atlasWidth, card.atlasHeight);
    const uint32_t cardMipLevelCount = ceilLog2U32(cardMaxDimension);
    const uint32_t rootMipLevel = std::min<uint32_t>(31u, std::min(atlasMipLevelCount, cardMipLevelCount));
    const uint32_t rootMipOffset = rootMipLevel == 0u ? 0u : rbvhDepthMipLevelOffset(build.atlasSize, rootMipLevel);
    card._padding0 = (rootMipOffset << 5u) | rootMipLevel;
  }
}

virtualgeometry::AABB rbvhCardTraversalBounds(const RBVHCardPrimitive &card)
{
  virtualgeometry::AABB bounds{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    bounds.minPoint[axis] = card.boundsMin[axis];
    bounds.maxPoint[axis] = card.boundsMax[axis];
  }
  return bounds;
}

bool rbvhAabbNearlyEqual(const virtualgeometry::AABB &a, const virtualgeometry::AABB &b)
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    const float extent = std::max({std::fabs(a.maxPoint[axis] - a.minPoint[axis]), std::fabs(b.maxPoint[axis] - b.minPoint[axis]), 1.0f});
    const float epsilon = extent * 1.0e-5f;
    if (std::fabs(a.minPoint[axis] - b.minPoint[axis]) > epsilon ||
        std::fabs(a.maxPoint[axis] - b.maxPoint[axis]) > epsilon)
    {
      return false;
    }
  }
  return true;
}

bool rbvhCardBuildMatchesCards(const RBVHBuildResult &build)
{
  if (build.cardBuild.primitiveCount != build.cards.size() ||
      build.cardBuild.primitiveRemap.size() != build.cards.size())
  {
    return false;
  }

  if (build.cards.empty())
  {
    return build.cardBuild.canonicalNodes.empty() &&
           build.cardBuild.rootNodeIndex == CWBVHBuilder::INVALID_NODE;
  }

  if (build.cardBuild.rootNodeIndex >= build.cardBuild.canonicalNodes.size())
  {
    return false;
  }

  uint32_t leafCount = 0u;
  std::vector<uint8_t> cardReferenced(build.cards.size(), 0u);
  for (const CWBVHBuilder::CanonicalNode &node : build.cardBuild.canonicalNodes)
  {
    if (!node.isLeaf)
    {
      continue;
    }

    if (node.primitiveIndex >= build.cardBuild.primitiveRemap.size())
    {
      return false;
    }

    const uint32_t cardIndex = build.cardBuild.primitiveRemap[node.primitiveIndex];
    if (cardIndex >= build.cards.size())
    {
      return false;
    }

    if (!rbvhAabbNearlyEqual(node.bounds, rbvhCardTraversalBounds(build.cards[cardIndex])))
    {
      return false;
    }

    cardReferenced[cardIndex] = 1u;
    ++leafCount;
  }

  if (leafCount != build.cards.size())
  {
    return false;
  }

  for (const uint8_t referenced : cardReferenced)
  {
    if (referenced == 0u)
    {
      return false;
    }
  }
  return true;
}

void rebuildRBVHCardBuildFromCards(RBVHBuildResult &build)
{
  std::vector<CWBVHBuilder::PrimitiveBounds> cardBounds;
  cardBounds.reserve(build.cards.size());
  for (uint32_t cardIndex = 0u; cardIndex < static_cast<uint32_t>(build.cards.size()); ++cardIndex)
  {
    cardBounds.push_back(
        CWBVHBuilder::PrimitiveBounds{
          .bounds = rbvhCardTraversalBounds(build.cards[cardIndex]),
          .primitiveIndex = cardIndex,
        });
  }
  build.cardBuild = CWBVHBuilder::buildFromAabbs(cardBounds);
}

virtualgeometry::FallbackMeshData makeMetadataMeshFromRBVHBuild(const RBVHBuildResult &build)
{
  virtualgeometry::FallbackMeshData mesh{};
  mesh.lodLevel = build.lodLevel;
  const virtualgeometry::AABB &bounds = build.cardBuild.localBounds;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    mesh.boundsMin[axis] = bounds.minPoint[axis];
    mesh.boundsMax[axis] = bounds.maxPoint[axis];
  }
  return mesh;
}

bool writeRBVHBuild(FILE *file, const RBVHBuildResult &build)
{
  bool ok = true;
  ok = ok && writeU32(file, build.lodLevel);
  ok = ok && writeU32(file, build.tileSize);
  ok = ok && writeU32(file, build.atlasSize);
  ok = ok && writeU32(file, build.cardsPerRow);
  ok = ok && writeU32(file, static_cast<uint32_t>(build.cards.size()));
  ok = ok && writeU32(file, static_cast<uint32_t>(build.sourcePrimitiveIndices.size()));
  ok = ok && writeU32(file, static_cast<uint32_t>(build.depthAtlasR16.size()));
  ok = ok && writeU32(file, static_cast<uint32_t>(build.coverageAtlasR8.size()));
  ok = ok && writeU32(file, static_cast<uint32_t>(build.albedoAtlasRGBA8.size()));
  ok = ok && writeU32(file, static_cast<uint32_t>(build.depthMinMaxMipAtlasRG16.size()));

  for (const RBVHCardPrimitive &card : build.cards)
    ok = ok && writeCard(file, card);
  for (const uint32_t primitiveIndex : build.sourcePrimitiveIndices)
    ok = ok && writeU32(file, primitiveIndex);
  for (const uint16_t depth : build.depthAtlasR16)
    ok = ok && writeU16(file, depth);
  if (!build.coverageAtlasR8.empty())
    ok = ok && writeBytes(file, build.coverageAtlasR8.data(), build.coverageAtlasR8.size());
  if (!build.albedoAtlasRGBA8.empty())
    ok = ok && writeBytes(file, build.albedoAtlasRGBA8.data(), build.albedoAtlasRGBA8.size());
  for (const uint32_t minMaxDepth : build.depthMinMaxMipAtlasRG16)
    ok = ok && writeU32(file, minMaxDepth);
  ok = ok && writeBuildResult(file, build.cardBuild);
  return ok;
}

RBVHBuildOptions makeRBVHLodOptions(const RBVHBuildOptions &baseOptions, const uint32_t lodLevel)
{
  RBVHBuildOptions options = baseOptions;
  const uint32_t clampedLod = std::min(lodLevel, SWRBVH_FALLBACK_MESH_LOD_COUNT - 1u);
  const uint32_t resolutionShift = std::min(clampedLod, 3u);
  const uint32_t resolutionScale = 1u << resolutionShift;
  const float lodT = static_cast<float>(clampedLod);

  options.maxCardResolution = std::max(baseOptions.maxCardResolution >> resolutionShift, baseOptions.minCardResolution);
  options.maxAtlasSize = std::max(baseOptions.maxAtlasSize >> resolutionShift, options.maxCardResolution);
  options.maxCards = std::max(baseOptions.maxCards, 1u);
  if (baseOptions.texelsPerWorldUnit > 0.0f)
  {
    options.texelsPerWorldUnit = baseOptions.texelsPerWorldUnit / static_cast<float>(resolutionScale);
  }
  if (baseOptions.pixelToAreaRatio > 0.0f)
  {
    options.pixelToAreaRatio = baseOptions.pixelToAreaRatio / static_cast<float>(resolutionScale * resolutionScale);
  }
  if (baseOptions.targetTileResolution > 0.0f)
  {
    options.targetTileResolution = std::max(baseOptions.targetTileResolution * (1.0f + lodT * 0.25f), static_cast<float>(options.minCardResolution));
  }
  options.texelsPerUvUnit = std::max(baseOptions.texelsPerUvUnit / static_cast<float>(resolutionScale), 1.0f);
  options.projectedAreaThreshold = std::clamp(baseOptions.projectedAreaThreshold - 0.05f * lodT, 0.45f, 1.0f);
  options.minNormalDot = std::clamp(baseOptions.minNormalDot - 0.05f * lodT, 0.25f, 1.0f);
  return options;
}

bool readRBVHBuild(FILE *file, const virtualgeometry::FallbackMeshData &fallbackMesh, const uint32_t fileVersion, RBVHBuildResult &outBuild)
{
  uint32_t cardCount = 0u;
  uint32_t sourcePrimitiveIndexCount = 0u;
  uint32_t depthCount = 0u;
  uint32_t coverageCount = 0u;
  uint32_t albedoCount = 0u;
  uint32_t depthMinMaxMipCount = 0u;

  bool ok = true;
  ok = ok && readU32(file, outBuild.lodLevel);
  ok = ok && readU32(file, outBuild.tileSize);
  ok = ok && readU32(file, outBuild.atlasSize);
  ok = ok && readU32(file, outBuild.cardsPerRow);
  ok = ok && readU32(file, cardCount);
  ok = ok && readU32(file, sourcePrimitiveIndexCount);
  ok = ok && readU32(file, depthCount);
  ok = ok && readU32(file, coverageCount);
  ok = ok && readU32(file, albedoCount);
  if (fileVersion >= 5u)
    ok = ok && readU32(file, depthMinMaxMipCount);

  outBuild.cards.resize(cardCount);
  outBuild.sourcePrimitiveIndices.resize(sourcePrimitiveIndexCount);
  outBuild.depthAtlasR16.resize(depthCount);
  outBuild.coverageAtlasR8.resize(coverageCount);
  outBuild.albedoAtlasRGBA8.resize(albedoCount);
  outBuild.depthMinMaxMipAtlasRG16.resize(depthMinMaxMipCount);

  for (RBVHCardPrimitive &card : outBuild.cards)
    ok = ok && readCard(file, card);
  for (uint32_t &primitiveIndex : outBuild.sourcePrimitiveIndices)
    ok = ok && readU32(file, primitiveIndex);
  for (uint16_t &depth : outBuild.depthAtlasR16)
    ok = ok && readU16(file, depth);
  if (!outBuild.coverageAtlasR8.empty())
    ok = ok && readBytes(file, outBuild.coverageAtlasR8.data(), outBuild.coverageAtlasR8.size());
  if (!outBuild.albedoAtlasRGBA8.empty())
    ok = ok && readBytes(file, outBuild.albedoAtlasRGBA8.data(), outBuild.albedoAtlasRGBA8.size());
  for (uint32_t &minMaxDepth : outBuild.depthMinMaxMipAtlasRG16)
    ok = ok && readU32(file, minMaxDepth);
  if (ok && outBuild.depthMinMaxMipAtlasRG16.empty())
    outBuild.depthMinMaxMipAtlasRG16 = RBVHBuilder::buildDepthMinMaxMipAtlas(outBuild.atlasSize, outBuild.depthAtlasR16, outBuild.coverageAtlasR8);
  if (ok)
    assignCardDepthMipOffsets(outBuild);
  ok = ok && readBuildResult(file, fallbackMesh, outBuild.cardBuild);
  if (ok && !rbvhCardBuildMatchesCards(outBuild))
  {
    rebuildRBVHCardBuildFromCards(outBuild);
  }
  if (ok && outBuild.cardBuild.rootNodeIndex < outBuild.cardBuild.canonicalNodes.size())
  {
    outBuild.cardBuild.localBounds = outBuild.cardBuild.canonicalNodes[outBuild.cardBuild.rootNodeIndex].bounds;
  }
  return ok;
}

bool writeDepthDebugImage(const std::string &path, const RBVHBuildResult &build)
{
  if (build.atlasSize == 0u || build.depthAtlasR16.empty())
  {
    return true;
  }

  FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr)
  {
    return false;
  }

  std::fprintf(file, "P5\n%u %u\n255\n", build.atlasSize, build.atlasSize);
  std::vector<uint8_t> pixels(build.depthAtlasR16.size(), 0u);
  for (size_t index = 0u; index < build.depthAtlasR16.size(); ++index)
  {
    if (index < build.coverageAtlasR8.size() && build.coverageAtlasR8[index] != 0u)
    {
      pixels[index] = static_cast<uint8_t>(build.depthAtlasR16[index] >> 8u);
    }
  }
  const bool ok = writeBytes(file, pixels.data(), pixels.size());
  std::fclose(file);
  return ok;
}

bool writeAlbedoDebugImage(const std::string &path, const RBVHBuildResult &build)
{
  if (build.atlasSize == 0u || build.albedoAtlasRGBA8.empty())
  {
    return true;
  }

  FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr)
  {
    return false;
  }

  std::fprintf(file, "P6\n%u %u\n255\n", build.atlasSize, build.atlasSize);
  std::vector<uint8_t> pixels(static_cast<size_t>(build.atlasSize) * static_cast<size_t>(build.atlasSize) * 3u, 0u);
  const size_t pixelCount = pixels.size() / 3u;
  for (size_t pixelIndex = 0u; pixelIndex < pixelCount; ++pixelIndex)
  {
    const size_t rgbaOffset = pixelIndex * 4u;
    if (rgbaOffset + 3u >= build.albedoAtlasRGBA8.size() || build.albedoAtlasRGBA8[rgbaOffset + 3u] == 0u)
    {
      continue;
    }
    pixels[pixelIndex * 3u + 0u] = build.albedoAtlasRGBA8[rgbaOffset + 0u];
    pixels[pixelIndex * 3u + 1u] = build.albedoAtlasRGBA8[rgbaOffset + 1u];
    pixels[pixelIndex * 3u + 2u] = build.albedoAtlasRGBA8[rgbaOffset + 2u];
  }
  const bool ok = writeBytes(file, pixels.data(), pixels.size());
  std::fclose(file);
  return ok;
}

bool writeDebugAtlasImages(const std::string &basePath, const SWRBVHFallbackMeshData &entry)
{
  bool ok = true;
  std::vector<RBVHBuildResult> fallbackBuilds;
  const std::vector<RBVHBuildResult> *builds = &entry.rbvhBuilds;
  if (builds->empty())
  {
    fallbackBuilds.push_back(entry.rbvhBuild);
    builds = &fallbackBuilds;
  }
  if (!builds->empty())
  {
    ok = ok && writeDepthDebugImage(basePath + ".depth.pgm", builds->front());
    ok = ok && writeAlbedoDebugImage(basePath + ".albedo.ppm", builds->front());
  }
  for (uint32_t lodIndex = 0u; lodIndex < static_cast<uint32_t>(builds->size()); ++lodIndex)
  {
    ok = ok && writeDepthDebugImage(basePath + ".lod" + std::to_string(lodIndex) + ".depth.pgm", (*builds)[lodIndex]);
    ok = ok && writeAlbedoDebugImage(basePath + ".lod" + std::to_string(lodIndex) + ".albedo.ppm", (*builds)[lodIndex]);
  }
  return ok;
}

bool writeEntry(FILE *file, const std::string &path, const SWRBVHFallbackMeshData &entry)
{
  (void)path;
  bool ok = true;
  std::vector<RBVHBuildResult> fallbackBuilds;
  const std::vector<RBVHBuildResult> *builds = &entry.rbvhBuilds;
  if (builds->empty())
  {
    fallbackBuilds.push_back(entry.rbvhBuild);
    builds = &fallbackBuilds;
  }
  ok = ok && writeU32(file, static_cast<uint32_t>(builds->size()));
  for (const RBVHBuildResult &build : *builds)
  {
    ok = ok && writeRBVHBuild(file, build);
  }
  return ok;
}

bool readEntry(FILE *file, const std::string &path, const uint32_t fileVersion, SWRBVHFallbackMeshData &entry)
{
  bool ok = true;
  if (fileVersion < SWRBVH_FALLBACK_MESH_VERSION)
  {
    std::vector<virtualgeometry::FallbackMeshData> meshes;
    ok = readFallbackBlob(file, path, meshes);
    if (!ok || meshes.size() != 1u)
    {
      return false;
    }
    entry.fallbackMesh = std::move(meshes.front());
    ok = ok && readBuildResult(file, entry.fallbackMesh, entry.triangleBuild);
  }

  entry.rbvhBuilds.clear();
  if (fileVersion >= 9u)
  {
    uint32_t rbvhBuildCount = 0u;
    ok = ok && readU32(file, rbvhBuildCount);
    entry.rbvhBuilds.resize(rbvhBuildCount);
    for (RBVHBuildResult &build : entry.rbvhBuilds)
    {
      ok = ok && readRBVHBuild(file, entry.fallbackMesh, fileVersion, build);
    }
  }
  else
  {
    entry.rbvhBuilds.resize(1u);
    ok = ok && readRBVHBuild(file, entry.fallbackMesh, fileVersion, entry.rbvhBuilds.front());
  }
  if (ok && !entry.rbvhBuilds.empty())
  {
    entry.rbvhBuild = entry.rbvhBuilds.front();
    if (fileVersion >= SWRBVH_FALLBACK_MESH_VERSION)
    {
      entry.fallbackMesh = makeMetadataMeshFromRBVHBuild(entry.rbvhBuild);
    }
  }
  return ok;
}

} // namespace

SWRBVHFallbackMeshData SWRBVHFallbackMeshFile::build(const virtualgeometry::FallbackMeshData &fallbackMesh)
{
  return build(fallbackMesh, RBVHBuildOptions{});
}

SWRBVHFallbackMeshData SWRBVHFallbackMeshFile::build(const virtualgeometry::FallbackMeshData &fallbackMesh, const RBVHBuildOptions &options)
{
  return build(fallbackMesh, fallbackMesh, options);
}

SWRBVHFallbackMeshData SWRBVHFallbackMeshFile::build(
    const virtualgeometry::FallbackMeshData &fallbackMesh,
    const virtualgeometry::FallbackMeshData &rbvhSourceMesh,
    const RBVHBuildOptions &options)
{
  SWRBVHFallbackMeshData data{};
  data.fallbackMesh = fallbackMesh;
  data.rbvhBuilds.reserve(SWRBVH_FALLBACK_MESH_LOD_COUNT);
  for (uint32_t lodLevel = 0u; lodLevel < SWRBVH_FALLBACK_MESH_LOD_COUNT; ++lodLevel)
  {
    RBVHBuildResult lodBuild = RBVHBuilder::build(rbvhSourceMesh, makeRBVHLodOptions(options, lodLevel));
    lodBuild.lodLevel = lodLevel;
    data.rbvhBuilds.push_back(std::move(lodBuild));
  }
  if (!data.rbvhBuilds.empty())
  {
    data.rbvhBuild = data.rbvhBuilds.front();
  }
  return data;
}

bool SWRBVHFallbackMeshFile::save(const std::string &path, const SWRBVHFallbackMeshData &data)
{
  FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr)
  {
    return false;
  }

  bool ok = true;
  ok = ok && writeU32(file, SWRBVH_FALLBACK_MESH_MAGIC);
  ok = ok && writeU32(file, SWRBVH_FALLBACK_MESH_VERSION);
  ok = ok && writeU32(file, SWRBVH_FALLBACK_MESH_ENDIAN_LITTLE);
  ok = ok && writeEntry(file, path, data);

  std::fclose(file);
  if (ok)
  {
    ok = ok && writeDebugAtlasImages(path, data);
  }
  return ok;
}

bool SWRBVHFallbackMeshFile::save(const std::string &path, const std::vector<SWRBVHFallbackMeshData> &data)
{
  if (data.size() == 1u)
    return save(path, data.front());

  FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr)
    return false;

  bool ok = true;
  ok = ok && writeU32(file, SWRBVH_FALLBACK_MESH_MAGIC);
  ok = ok && writeU32(file, SWRBVH_FALLBACK_MESH_VERSION_COLLECTION);
  ok = ok && writeU32(file, SWRBVH_FALLBACK_MESH_ENDIAN_LITTLE);
  ok = ok && writeU32(file, static_cast<uint32_t>(data.size()));
  for (const SWRBVHFallbackMeshData &entry : data)
  {
    ok = ok && writeEntry(file, path, entry);
  }

  std::fclose(file);
  if (ok)
  {
    for (uint32_t entryIndex = 0u; entryIndex < static_cast<uint32_t>(data.size()); ++entryIndex)
    {
      ok = ok && writeDebugAtlasImages(path + ".entry" + std::to_string(entryIndex), data[entryIndex]);
    }
  }
  return ok;
}

bool SWRBVHFallbackMeshFile::load(const std::string &path, std::vector<SWRBVHFallbackMeshData> &outData)
{
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
  {
    return false;
  }

  uint32_t magic = 0u;
  uint32_t version = 0u;
  uint32_t endianTag = 0u;
  bool ok = true;
  ok = ok && readU32(file, magic);
  ok = ok && readU32(file, version);
  ok = ok && readU32(file, endianTag);
  const bool supportedVersion =
      version == 3u ||
      version == 4u ||
      version == 5u ||
      version == 6u ||
      version == 9u ||
      version == 10u ||
      version == SWRBVH_FALLBACK_MESH_VERSION ||
      version == SWRBVH_FALLBACK_MESH_VERSION_COLLECTION;
  if (!ok || magic != SWRBVH_FALLBACK_MESH_MAGIC ||
      !supportedVersion ||
      endianTag != SWRBVH_FALLBACK_MESH_ENDIAN_LITTLE)
  {
    std::fclose(file);
    return false;
  }

  outData.clear();
  if (version == 4u || version == 6u || version == 10u || version == SWRBVH_FALLBACK_MESH_VERSION_COLLECTION)
  {
    uint32_t entryCount = 0u;
    ok = ok && readU32(file, entryCount);
    outData.resize(entryCount);
    for (SWRBVHFallbackMeshData &entry : outData)
    {
      ok = ok && readEntry(file, path, version, entry);
      if (!ok)
      {
        break;
      }
    }
  }
  else
  {
    outData.resize(1u);
    ok = ok && readEntry(file, path, version, outData.front());
  }

  std::fclose(file);
  return ok;
}

bool SWRBVHFallbackMeshFile::load(const std::string &path, SWRBVHFallbackMeshData &outData)
{
  std::vector<SWRBVHFallbackMeshData> entries;
  if (!load(path, entries) || entries.empty())
    return false;
  outData = std::move(entries.front());
  return true;
}

bool SWRBVHFallbackMeshFile::loadEntry(const std::string &path, uint32_t meshIndex, SWRBVHFallbackMeshData &outData)
{
  std::vector<SWRBVHFallbackMeshData> entries;
  if (!load(path, entries) || meshIndex >= entries.size())
    return false;
  outData = std::move(entries[meshIndex]);
  return true;
}

} // namespace softwarebvh
