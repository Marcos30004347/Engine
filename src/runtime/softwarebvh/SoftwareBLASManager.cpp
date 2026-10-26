#include "softwarebvh/SoftwareBLASManager.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace softwarebvh
{

namespace
{

static const virtualgeometry::FallbackMeshData &resolveMesh(const SoftwareBLASManager::BuildInput &input)
{
  return input.meshRef != nullptr ? *input.meshRef : input.mesh;
}

static std::vector<uint32_t> buildFallbackPrimitiveMaterialIds(const virtualgeometry::FallbackMeshData &mesh)
{
  std::vector<uint32_t> materialIds(mesh.indices.size() / 3u, 0u);
  for (const virtualgeometry::FallbackMeshSection &section : mesh.sections)
  {
    const uint32_t triangleBegin = section.indexOffset / 3u;
    const uint32_t triangleEnd = std::min<uint32_t>(
        static_cast<uint32_t>(mesh.indices.size() / 3u),
        (section.indexOffset + section.indexCount + 2u) / 3u);
    for (uint32_t triangleIndex = triangleBegin; triangleIndex < triangleEnd; ++triangleIndex)
    {
      materialIds[triangleIndex] = section.materialIndex;
    }
  }
  return materialIds;
}

static const CWBVHBuilder::BuildResult *resolvePrebuilt(const SoftwareBLASManager::BuildInput &input)
{
  if (input.prebuiltRef != nullptr)
  {
    return input.prebuiltRef;
  }
  return input.prebuilt.has_value() ? &*input.prebuilt : nullptr;
}

static const RBVHBuildResult *resolveRbvhBuild(const SoftwareBLASManager::BuildInput &input)
{
  if (input.rbvhBuildRef != nullptr)
  {
    return input.rbvhBuildRef;
  }
  return input.rbvhBuild.has_value() ? &*input.rbvhBuild : nullptr;
}

static virtualgeometry::AABB rbvhCardTraversalBounds(const RBVHCardPrimitive &card)
{
  virtualgeometry::AABB bounds{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    bounds.minPoint[axis] = card.boundsMin[axis];
    bounds.maxPoint[axis] = card.boundsMax[axis];
  }
  return bounds;
}

static bool rbvhAabbNearlyEqual(const virtualgeometry::AABB &a, const virtualgeometry::AABB &b)
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

static bool rbvhCardBuildMatchesCards(const RBVHBuildResult &build)
{
  if (build.cardBuild.primitiveCount != build.cards.size() ||
      build.cardBuild.primitiveRemap.size() != build.cards.size())
  {
    return false;
  }

  if (build.cards.empty())
  {
    return build.cardBuild.rootNodeIndex == CWBVHBuilder::INVALID_NODE &&
           build.cardBuild.canonicalNodes.empty();
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

static CWBVHBuilder::BuildResult buildRbvhCardBuildFromCards(const RBVHBuildResult &build)
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
  return CWBVHBuilder::buildFromAabbs(cardBounds);
}

static const CWBVHBuilder::BuildResult &selectRbvhCardBuild(const RBVHBuildResult &rbvhBuild, CWBVHBuilder::BuildResult &rebuiltCardBuild)
{
  if (rbvhCardBuildMatchesCards(rbvhBuild))
  {
    return rbvhBuild.cardBuild;
  }

  rebuiltCardBuild = buildRbvhCardBuildFromCards(rbvhBuild);
  return rebuiltCardBuild;
}

static uint32_t divCeilU32(const uint32_t value, const uint32_t divisor)
{
  return divisor == 0u ? 0u : (value + divisor - 1u) / divisor;
}

static uint16_t floatToHalfBits(const float value)
{
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));

  const uint32_t sign = (bits >> 16u) & 0x8000u;
  uint32_t mantissa = bits & 0x007fffffu;
  int32_t exponent = static_cast<int32_t>((bits >> 23u) & 0xffu) - 127 + 15;

  if (exponent <= 0)
  {
    if (exponent < -10)
    {
      return static_cast<uint16_t>(sign);
    }
    mantissa |= 0x00800000u;
    const uint32_t shift = static_cast<uint32_t>(14 - exponent);
    const uint32_t rounded = (mantissa + (1u << (shift - 1u))) >> shift;
    return static_cast<uint16_t>(sign | rounded);
  }

  if (exponent >= 31)
  {
    return static_cast<uint16_t>(sign | 0x7c00u);
  }

  mantissa += 0x00001000u;
  if ((mantissa & 0x00800000u) != 0u)
  {
    mantissa = 0u;
    exponent += 1;
    if (exponent >= 31)
    {
      return static_cast<uint16_t>(sign | 0x7c00u);
    }
  }

  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10u) | (mantissa >> 13u));
}

static uint32_t checkedCapacityU32(const uint64_t value, const char *label)
{
  if (value > std::numeric_limits<uint32_t>::max())
  {
    throw std::runtime_error(std::string("SoftwareBLASManager required capacity exceeds uint32_t for ") + label);
  }
  return static_cast<uint32_t>(value);
}

static uint32_t growCapacityU32(const uint32_t current, const uint64_t required, const char *label)
{
  if (required <= current)
  {
    return current;
  }

  return checkedCapacityU32(required, label);
}

} // namespace

SoftwareBLASManager::SoftwareBLASManager(rendering::RenderGraph *renderGraph) : SoftwareBLASManager(renderGraph, Settings{})
{
}

SoftwareBLASManager::SoftwareBLASManager(rendering::RenderGraph *renderGraph, Settings settings) : renderGraph_(renderGraph), settings_(settings)
{
  if (renderGraph_ == nullptr)
  {
    throw std::runtime_error("SoftwareBLASManager requires a valid RenderGraph");
  }

  if (settings_.maxBlases == 0u || settings_.maxVertices == 0u || settings_.maxIndices == 0u || settings_.maxNodes == 0u ||
      settings_.maxPackedCWBVHNodes == 0u ||
      settings_.maxPrimitives == 0u || settings_.maxRbvhCards == 0u || settings_.maxRbvhAtlasTexels == 0u ||
      settings_.maxRbvhDepthMipTexels == 0u || settings_.workgroupSize == 0u)
  {
    throw std::runtime_error("SoftwareBLASManager settings must be non-zero");
  }

  initializeResources();
  uploadIdleState();
}

SoftwareBLASManager::~SoftwareBLASManager()
{
  destroyResources();
}

void SoftwareBLASManager::destroyResources()
{
  for (const auto &[_, bufferIds] : frameOverrideBufferIds_)
  {
    for (const rendering::BufferId bufferId : bufferIds)
    {
      if (bufferId != rendering::BufferId::Invalid)
      {
        renderGraph_->getRHI()->deleteBuffer(bufferId);
      }
    }
  }
  frameOverrideBufferIds_.clear();

  auto deleteBufferIfValid = [&](rendering::Buffer &buffer)
  {
    if (buffer.isValid())
    {
      renderGraph_->deleteBuffer(buffer);
      buffer = {};
    }
  };

  deleteBufferIfValid(countersBuffer_);
  deleteBufferIfValid(buildIndirectArgsBuffer_);
  deleteBufferIfValid(gpuBuildDebugBuffer_);
  deleteBufferIfValid(gpuLeafRemapBuffer_);
  deleteBufferIfValid(gpuCWBVHNodesBuffer_);
  deleteBufferIfValid(gpuBuildHLBVHNodesBuffer_);
  deleteBufferIfValid(gpuBuildSignalsBuffer_);
  deleteBufferIfValid(gpuBuildFlagsBuffer_);
  deleteBufferIfValid(gpuBuildPrimitiveIndicesBuffer_);
  deleteBufferIfValid(gpuBuildMortonCodesBuffer_);
  deleteBufferIfValid(gpuBuildAabbsBuffer_);
  deleteBufferIfValid(primitiveMaterialIdsBuffer_);
  deleteBufferIfValid(primitiveRemapBuffer_);
  deleteBufferIfValid(canonicalNodesBuffer_);
  deleteBufferIfValid(vertexPositionsBuffer_);
  deleteBufferIfValid(indicesBuffer_);
  deleteBufferIfValid(verticesBuffer_);
  deleteBufferIfValid(rbvhDepthMipAtlasBuffer_);
  deleteBufferIfValid(rbvhCoverageAtlasBuffer_);
  deleteBufferIfValid(rbvhDepthAtlasBuffer_);
  deleteBufferIfValid(rbvhCardsBuffer_);
  deleteBufferIfValid(sourceBVH8NodesBuffer_);
  deleteBufferIfValid(triangleTransformsBuffer_);
  deleteBufferIfValid(rbvhCardMetadataBuffer_);
  deleteBufferIfValid(triangleMetadataBuffer_);
  if (rbvhDepthAtlasTexture_.isValid())
  {
    renderGraph_->deleteTexture(rbvhDepthAtlasTexture_);
    rbvhDepthAtlasTexture_ = {};
  }
  rbvhDepthAtlasTextureUploaded_ = false;
  rbvhDepthAtlasTextureWidth_ = 1u;
  rbvhDepthAtlasTextureHeight_ = 1u;
  rbvhDepthAtlasTextureTexelCount_ = 0u;
}

void SoftwareBLASManager::ensureCapacityForBuildInputs(const std::vector<BuildInput> &buildInputs)
{
  uint64_t requiredBlases = buildInputs.size();
  uint64_t requiredVertices = 0u;
  uint64_t requiredIndices = 0u;
  uint64_t requiredNodes = 0u;
  uint64_t requiredPrimitives = 0u;
  uint64_t requiredTriangleTransforms = 0u;
  uint64_t requiredPackedCwbvhNodes = 0u;
  uint64_t requiredRbvhCards = 0u;
  uint64_t requiredRbvhAtlasTexels = 0u;
  uint64_t requiredRbvhAtlasTextureWidth = 0u;
  uint64_t requiredRbvhAtlasTextureHeight = 0u;
  uint64_t requiredRbvhDepthMipTexels = 0u;

  for (const BuildInput &input : buildInputs)
  {
    const virtualgeometry::FallbackMeshData &mesh = resolveMesh(input);
    const CWBVHBuilder::BuildResult *prebuilt = resolvePrebuilt(input);
    const RBVHBuildResult *rbvhBuild = resolveRbvhBuild(input);

    if (rbvhBuild != nullptr)
    {
      CWBVHBuilder::BuildResult rebuiltCardBuild;
      const CWBVHBuilder::BuildResult &build = selectRbvhCardBuild(*rbvhBuild, rebuiltCardBuild);
      requiredNodes += build.canonicalNodes.size();
      requiredPrimitives += std::max<uint64_t>(build.primitiveRemap.size(), build.primitiveCount);
      requiredPackedCwbvhNodes += build.canonicalNodes.size();
      requiredRbvhCards += rbvhBuild->cards.size();
      if (rbvhBuild->atlasSize > 0u && !rbvhBuild->depthAtlasR16.empty())
      {
        requiredRbvhAtlasTextureWidth = std::max<uint64_t>(requiredRbvhAtlasTextureWidth, rbvhBuild->atlasSize);
        requiredRbvhAtlasTextureHeight += rbvhBuild->atlasSize;
      }
      requiredRbvhDepthMipTexels += rbvhBuild->depthMinMaxMipAtlasRG16.size();
      continue;
    }

    requiredVertices += mesh.vertices.size();
    requiredIndices += mesh.indices.size();

    const uint64_t primitiveCount = prebuilt != nullptr ? prebuilt->primitiveCount : static_cast<uint64_t>(mesh.indices.size() / 3u);
    const uint64_t nodeCount = prebuilt != nullptr
                                   ? static_cast<uint64_t>(prebuilt->canonicalNodes.size())
                                   : (primitiveCount == 0u ? 0u : primitiveCount * 2u - 1u);
    const uint64_t primitiveRemapCount = prebuilt != nullptr ? prebuilt->primitiveRemap.size() : primitiveCount;
    const uint64_t triangleTransformCount = prebuilt != nullptr ? prebuilt->triangleTransforms.size() : primitiveCount;
    requiredNodes += nodeCount;
    requiredPrimitives += std::max(primitiveRemapCount, primitiveCount);
    requiredTriangleTransforms += triangleTransformCount;
    requiredPackedCwbvhNodes += primitiveCount;
  }

  requiredPrimitives = std::max(requiredPrimitives, requiredTriangleTransforms);
  requiredRbvhAtlasTexels = requiredRbvhAtlasTextureWidth * requiredRbvhAtlasTextureHeight;

  Settings grownSettings = settings_;
  grownSettings.maxBlases = growCapacityU32(settings_.maxBlases, requiredBlases, "BLAS count");
  grownSettings.maxVertices = growCapacityU32(settings_.maxVertices, requiredVertices, "vertices");
  grownSettings.maxIndices = growCapacityU32(settings_.maxIndices, requiredIndices, "indices");
  grownSettings.maxNodes = growCapacityU32(settings_.maxNodes, requiredNodes, "canonical nodes");
  grownSettings.maxPrimitives = growCapacityU32(settings_.maxPrimitives, requiredPrimitives, "primitives");
  grownSettings.maxPackedCWBVHNodes = growCapacityU32(settings_.maxPackedCWBVHNodes, requiredPackedCwbvhNodes, "packed CWBVH nodes");
  grownSettings.maxRbvhCards = growCapacityU32(settings_.maxRbvhCards, requiredRbvhCards, "RBVH cards");
  grownSettings.maxRbvhAtlasTexels = growCapacityU32(settings_.maxRbvhAtlasTexels, requiredRbvhAtlasTexels, "RBVH atlas texels");
  grownSettings.maxRbvhDepthMipTexels = growCapacityU32(settings_.maxRbvhDepthMipTexels, requiredRbvhDepthMipTexels, "RBVH depth mip texels");

  if (grownSettings.maxBlases == settings_.maxBlases &&
      grownSettings.maxVertices == settings_.maxVertices &&
      grownSettings.maxIndices == settings_.maxIndices &&
      grownSettings.maxNodes == settings_.maxNodes &&
      grownSettings.maxPrimitives == settings_.maxPrimitives &&
      grownSettings.maxPackedCWBVHNodes == settings_.maxPackedCWBVHNodes &&
      grownSettings.maxRbvhCards == settings_.maxRbvhCards &&
      grownSettings.maxRbvhAtlasTexels == settings_.maxRbvhAtlasTexels &&
      grownSettings.maxRbvhDepthMipTexels == settings_.maxRbvhDepthMipTexels)
  {
    return;
  }

  settings_ = grownSettings;
  destroyResources();
  initializeResources();
}

SoftwareBLASManager::BuildSummary SoftwareBLASManager::syncFromCPU(const std::vector<BuildInput> &buildInputs)
{
  ensureCapacityForBuildInputs(buildInputs);

  if (buildInputs.size() > settings_.maxBlases)
  {
    throw std::runtime_error("SoftwareBLASManager maxBlases exceeded");
  }

  std::unordered_set<std::string> activeSourcePaths;
  activeSourcePaths.reserve(buildInputs.size());
  std::vector<UniqueBuildInput> uniqueBuilds;
  uniqueBuilds.reserve(buildInputs.size());

  for (const BuildInput &input : buildInputs)
  {
    if (input.sourcePath.empty())
    {
      continue;
    }

    const auto [_, inserted] = activeSourcePaths.insert(input.sourcePath);
    if (!inserted)
    {
      continue;
    }

    auto existingHandle = handleBySourcePath_.find(input.sourcePath);
    const uint32_t handle = existingHandle != handleBySourcePath_.end() ? existingHandle->second : allocateHandle();
    handleBySourcePath_[input.sourcePath] = handle;
    uniqueBuilds.push_back(UniqueBuildInput{
        .handle = handle,
        .input = &input,
    });
  }

  bool topologyChanged = false;
  for (auto it = handleBySourcePath_.begin(); it != handleBySourcePath_.end();)
  {
    if (activeSourcePaths.find(it->first) != activeSourcePaths.end())
    {
      ++it;
      continue;
    }

    topologyChanged = true;
    freeHandles_.push_back(it->second);
    recordsByHandle_.erase(it->second);
    it = handleBySourcePath_.erase(it);
  }

  std::sort(
      uniqueBuilds.begin(),
      uniqueBuilds.end(),
      [](const UniqueBuildInput &a, const UniqueBuildInput &b)
      {
        return a.handle < b.handle;
      });

  triangleMetadataGPU_.assign(settings_.maxBlases, TriangleBLASMetadataGPU{});
  rbvhCardMetadataGPU_.assign(settings_.maxBlases, RBVHCardBLASMetadataGPU{});
  for (TriangleBLASMetadataGPU &metadata : triangleMetadataGPU_)
  {
    metadata.handleId = INVALID_BLAS_HANDLE;
  }
  for (RBVHCardBLASMetadataGPU &metadata : rbvhCardMetadataGPU_)
  {
    metadata.handleId = INVALID_BLAS_HANDLE;
  }
  verticesGPU_.clear();
  indicesGPU_.clear();
  vertexPositionsGPU_.clear();
  canonicalNodesGPU_.clear();
  primitiveRemapGPU_.clear();
  primitiveMaterialIdsGPU_.clear();
  triangleTransformsGPU_.clear();
  sourceBVH8NodesGPU_.clear();
  rbvhCardsGPU_.clear();
  rbvhDepthAtlasGPU_.clear();
  rbvhCoverageAtlasGPU_.clear();
  rbvhDepthMipAtlasGPU_.clear();
  gpuCWBVHNodesCPU_.clear();
  gpuLeafRemapCPU_.clear();
  recordsByHandle_.clear();

  verticesGPU_.reserve(std::min<uint32_t>(settings_.maxVertices, 1024u));
  indicesGPU_.reserve(std::min<uint32_t>(settings_.maxIndices, 1024u));
  vertexPositionsGPU_.reserve(std::min<uint32_t>(settings_.maxVertices, 1024u));
  canonicalNodesGPU_.reserve(std::min<uint32_t>(settings_.maxNodes, 2048u));
  primitiveRemapGPU_.reserve(std::min<uint32_t>(settings_.maxPrimitives, 1024u));
  primitiveMaterialIdsGPU_.reserve(std::min<uint32_t>(settings_.maxPrimitives, 1024u));
  triangleTransformsGPU_.reserve(std::min<uint32_t>(settings_.maxPrimitives, 1024u));
  sourceBVH8NodesGPU_.reserve(std::min<uint32_t>(settings_.maxPackedCWBVHNodes, 1024u));
  rbvhCardsGPU_.reserve(std::min<uint32_t>(settings_.maxRbvhCards, 1024u));
  rbvhDepthAtlasGPU_.reserve(std::min<uint32_t>(settings_.maxRbvhAtlasTexels, 1024u));
  rbvhCoverageAtlasGPU_.reserve(std::min<uint32_t>(settings_.maxRbvhAtlasTexels, 1024u));
  rbvhDepthMipAtlasGPU_.reserve(std::min<uint32_t>(settings_.maxRbvhDepthMipTexels, 1024u));
  gpuCWBVHNodesCPU_.reserve(std::min<uint32_t>(settings_.maxPackedCWBVHNodes, 1024u));
  gpuLeafRemapCPU_.reserve(std::min<uint32_t>(settings_.maxPrimitives, 1024u));
  uint32_t nextGpuHLBVHNodeOffset = 0u;
  uint32_t nextGpuCWBVHNodeOffset = 0u;
  uint32_t nextGpuLeafRemapOffset = 0u;

  uint32_t rbvhAtlasTextureWidth = 0u;
  uint32_t rbvhAtlasTextureHeight = 0u;
  for (const UniqueBuildInput &build : uniqueBuilds)
  {
    const RBVHBuildResult *rbvhBuild = resolveRbvhBuild(*build.input);
    if (rbvhBuild != nullptr && rbvhBuild->atlasSize > 0u && !rbvhBuild->depthAtlasR16.empty())
    {
      rbvhAtlasTextureWidth = std::max(rbvhAtlasTextureWidth, rbvhBuild->atlasSize);
      rbvhAtlasTextureHeight += rbvhBuild->atlasSize;
    }
  }

  const uint64_t rbvhAtlasTextureTexelCount64 = static_cast<uint64_t>(rbvhAtlasTextureWidth) * static_cast<uint64_t>(rbvhAtlasTextureHeight);
  if (rbvhAtlasTextureTexelCount64 > settings_.maxRbvhAtlasTexels)
  {
    throw std::runtime_error("SoftwareBLASManager RBVH atlas texture capacity exceeded");
  }
  if (rbvhAtlasTextureTexelCount64 > std::numeric_limits<uint32_t>::max())
  {
    throw std::runtime_error("SoftwareBLASManager RBVH atlas texture is too large");
  }
  rbvhDepthAtlasGPU_.assign(static_cast<size_t>(rbvhAtlasTextureTexelCount64), 0u);
  uint32_t rbvhAtlasTextureNextY = 0u;

  for (const UniqueBuildInput &build : uniqueBuilds)
  {
    const BuildInput &input = *build.input;
    const virtualgeometry::FallbackMeshData &mesh = resolveMesh(input);
    const CWBVHBuilder::BuildResult *prebuilt = resolvePrebuilt(input);
    const RBVHBuildResult *rbvhBuild = resolveRbvhBuild(input);
    const bool rbvhMode = rbvhBuild != nullptr;
    CWBVHBuilder::BuildResult ownedBuild = (!rbvhMode && prebuilt == nullptr) ? CWBVHBuilder::build(mesh) : CWBVHBuilder::BuildResult{};
    CWBVHBuilder::BuildResult rebuiltRbvhCardBuild;
    const CWBVHBuilder::BuildResult &buildResult = rbvhMode
        ? selectRbvhCardBuild(*rbvhBuild, rebuiltRbvhCardBuild)
        : (prebuilt != nullptr ? *prebuilt : ownedBuild);
    CWBVHBuilder::BuildResult rebuiltBVH8Build;
    const CWBVHBuilder::BuildResult *buildForCompression = &buildResult;
    if (!buildResult.canonicalNodes.empty() && buildResult.bvh8Nodes.empty())
    {
      rebuiltBVH8Build = buildResult;
      CWBVHBuilder::rebuildBVH8(rebuiltBVH8Build);
      buildForCompression = &rebuiltBVH8Build;
    }
    const uint32_t rbvhCardCount = rbvhMode ? static_cast<uint32_t>(rbvhBuild->cards.size()) : 0u;
    if (rbvhMode &&
        (buildResult.primitiveCount != rbvhCardCount ||
         buildResult.primitiveRemap.size() != rbvhBuild->cards.size()))
    {
      throw std::runtime_error("SoftwareBLASManager RBVH card BLAS primitive count does not match card count");
    }
    const uint32_t primitiveCount = rbvhMode ? rbvhCardCount : buildResult.primitiveCount;
    const uint32_t meshVertexCount = rbvhMode ? 0u : static_cast<uint32_t>(mesh.vertices.size());
    const uint32_t meshIndexCount = rbvhMode ? 0u : static_cast<uint32_t>(mesh.indices.size());
    const uint32_t rbvhDepthMipTexelCount = rbvhMode ? static_cast<uint32_t>(rbvhBuild->depthMinMaxMipAtlasRG16.size()) : 0u;

    if (verticesGPU_.size() + meshVertexCount > settings_.maxVertices ||
        indicesGPU_.size() + meshIndexCount > settings_.maxIndices ||
        primitiveRemapGPU_.size() + buildResult.primitiveRemap.size() > settings_.maxPrimitives ||
        primitiveMaterialIdsGPU_.size() + primitiveCount > settings_.maxPrimitives ||
        triangleTransformsGPU_.size() + (rbvhMode ? 0u : buildResult.triangleTransforms.size()) > settings_.maxPrimitives ||
        rbvhCardsGPU_.size() + (rbvhMode ? rbvhBuild->cards.size() : 0u) > settings_.maxRbvhCards ||
        rbvhDepthMipAtlasGPU_.size() + rbvhDepthMipTexelCount > settings_.maxRbvhDepthMipTexels)
    {
      throw std::runtime_error("SoftwareBLASManager aggregate geometry capacity exceeded");
    }

    BLASRecord record{};
    record.handle = build.handle;
    record.sourcePath = input.sourcePath;
    record.fallbackPath = input.fallbackPath;
    record.localBounds = buildResult.localBounds;
    record.vertexOffset = static_cast<uint32_t>(verticesGPU_.size());
    record.vertexCount = meshVertexCount;
    record.indexOffset = static_cast<uint32_t>(indicesGPU_.size());
    record.indexCount = meshIndexCount;
    record.primitiveRemapOffset = static_cast<uint32_t>(primitiveRemapGPU_.size());
    record.primitiveCount = primitiveCount;
    record.triangleTransformOffset = static_cast<uint32_t>(triangleTransformsGPU_.size());
    record.primitiveMaterialOffset = static_cast<uint32_t>(primitiveMaterialIdsGPU_.size());
    record.lodLevel = rbvhMode ? rbvhBuild->lodLevel : mesh.lodLevel;
    record.sourceBVH8NodeOffset = static_cast<uint32_t>(sourceBVH8NodesGPU_.size());
    record.gpuHLBVHNodeOffset = nextGpuHLBVHNodeOffset;
    record.gpuHLBVHNodeCount = primitiveCount == 0u ? 0u : (primitiveCount * 2u - 1u);
    record.gpuCWBVHNodeOffset = nextGpuCWBVHNodeOffset;
    record.gpuLeafRemapOffset = nextGpuLeafRemapOffset;
    if (rbvhMode)
    {
      record.rbvhCardOffset = static_cast<uint32_t>(rbvhCardsGPU_.size());
      record.rbvhCardCount = rbvhCardCount;
      record.rbvhDepthAtlasOffset = rbvhAtlasTextureNextY * std::max(rbvhAtlasTextureWidth, 1u);
      record.rbvhCoverageAtlasOffset = 0u;
      record.rbvhAtlasSize = rbvhBuild->atlasSize;
      record.rbvhAtlasTexelCount = static_cast<uint32_t>(rbvhAtlasTextureTexelCount64);
      record.rbvhDepthMipOffset = static_cast<uint32_t>(rbvhDepthMipAtlasGPU_.size());
      record.rbvhDepthMipLevelCount = rbvhBuild->atlasSize > 1u ? static_cast<uint32_t>(std::log2(static_cast<float>(rbvhBuild->atlasSize))) : 0u;
    }

    if (!rbvhMode)
    {
      for (uint32_t vertexIndex = 0u; vertexIndex < mesh.vertices.size(); ++vertexIndex)
      {
        virtualgeometry::FallbackMeshVertex vertex = mesh.vertices[vertexIndex];
        verticesGPU_.push_back(vertex);

        VertexPositionGPU position{};
        position.position[0] = vertex.position[0];
        position.position[1] = vertex.position[1];
        position.position[2] = vertex.position[2];
        position.position[3] = 1.0f;
        vertexPositionsGPU_.push_back(position);
      }
      indicesGPU_.insert(indicesGPU_.end(), mesh.indices.begin(), mesh.indices.end());
    }

    for (const uint32_t primitiveIndex : buildResult.primitiveRemap)
    {
      primitiveRemapGPU_.push_back(primitiveIndex);
    }
    if (!rbvhMode)
    {
      const std::vector<uint32_t> primitiveMaterialIds = buildFallbackPrimitiveMaterialIds(mesh);
      for (uint32_t primitiveIndex = 0u; primitiveIndex < primitiveCount; ++primitiveIndex)
      {
        primitiveMaterialIdsGPU_.push_back(primitiveIndex < primitiveMaterialIds.size() ? primitiveMaterialIds[primitiveIndex] : 0u);
      }
    }
    else
    {
      primitiveMaterialIdsGPU_.insert(primitiveMaterialIdsGPU_.end(), primitiveCount, 0u);
    }
    if (!rbvhMode)
    {
      for (const CWBVHBuilder::TriBaryTransform &transform : buildResult.triangleTransforms)
      {
        TriangleBaryTransformGPU gpuTransform{};
        for (uint32_t i = 0u; i < 4u; ++i)
        {
          gpuTransform.row0[i] = transform.row0[i];
          gpuTransform.row1[i] = transform.row1[i];
          gpuTransform.row2[i] = transform.row2[i];
        }
        triangleTransformsGPU_.push_back(gpuTransform);
      }
      if (sourceBVH8NodesGPU_.size() + buildForCompression->bvh8Nodes.size() > settings_.maxPackedCWBVHNodes)
      {
        throw std::runtime_error("SoftwareBLASManager aggregate source BVH8 capacity exceeded");
      }
      for (const CWBVHBuilder::BVH8Node &node : buildForCompression->bvh8Nodes)
      {
        sourceBVH8NodesGPU_.push_back(SoftwareRayTraceScene::buildBVH8NodeGPU(node));
      }
      record.sourceBVH8NodeCount = static_cast<uint32_t>(sourceBVH8NodesGPU_.size()) - record.sourceBVH8NodeOffset;
    }
    else
    {
      rbvhCardsGPU_.insert(rbvhCardsGPU_.end(), rbvhBuild->cards.begin(), rbvhBuild->cards.end());
      for (uint32_t y = 0u; y < rbvhBuild->atlasSize; ++y)
      {
        for (uint32_t x = 0u; x < rbvhBuild->atlasSize; ++x)
        {
          const size_t sourceIndex = static_cast<size_t>(y) * rbvhBuild->atlasSize + x;
          if (sourceIndex >= rbvhBuild->depthAtlasR16.size())
          {
            continue;
          }
          const size_t targetIndex = static_cast<size_t>(record.rbvhDepthAtlasOffset) + static_cast<size_t>(y) * rbvhAtlasTextureWidth + x;
          if (targetIndex >= rbvhDepthAtlasGPU_.size())
          {
            continue;
          }

          const uint32_t depth = static_cast<uint32_t>(rbvhBuild->depthAtlasR16[sourceIndex]);
          const uint32_t coverage = sourceIndex < rbvhBuild->coverageAtlasR8.size()
                                        ? static_cast<uint32_t>(rbvhBuild->coverageAtlasR8[sourceIndex])
                                        : 0u;
          rbvhDepthAtlasGPU_[targetIndex] = depth | ((coverage & 0xffu) << 16u);
        }
      }
      rbvhAtlasTextureNextY += rbvhBuild->atlasSize;
      rbvhDepthMipAtlasGPU_.insert(rbvhDepthMipAtlasGPU_.end(), rbvhBuild->depthMinMaxMipAtlasRG16.begin(), rbvhBuild->depthMinMaxMipAtlasRG16.end());
    }

    uint32_t localRootNodeIndex = INVALID_NODE;
    if (!buildResult.canonicalNodes.empty())
    {
      if (canonicalNodesGPU_.size() + buildResult.canonicalNodes.size() > settings_.maxNodes)
      {
        throw std::runtime_error("SoftwareBLASManager aggregate node capacity exceeded");
      }

      record.nodeOffset = static_cast<uint32_t>(canonicalNodesGPU_.size());
      localRootNodeIndex = buildResult.rootNodeIndex;
      record.nodeCount = static_cast<uint32_t>(buildResult.canonicalNodes.size());
      record.rootNodeIndex = localRootNodeIndex == INVALID_NODE ? INVALID_NODE : record.nodeOffset + localRootNodeIndex;

      for (const CWBVHBuilder::CanonicalNode &node : buildResult.canonicalNodes)
      {
        CanonicalNodeGPU gpuNode{};
        gpuNode.parent = node.parent == INVALID_NODE ? INVALID_NODE : record.nodeOffset + node.parent;
        gpuNode.left = node.left == INVALID_NODE ? INVALID_NODE : record.nodeOffset + node.left;
        gpuNode.right = node.right == INVALID_NODE ? INVALID_NODE : record.nodeOffset + node.right;
        gpuNode.primitiveIndex = node.primitiveIndex == CWBVHBuilder::INVALID_PRIMITIVE_INDEX
                                     ? INVALID_PRIMITIVE_INDEX
                                     : record.primitiveRemapOffset + node.primitiveIndex;
        gpuNode.flags = node.isLeaf ? NODE_FLAG_LEAF : 0u;
        gpuNode.subtreePrimitiveCount = node.subtreePrimitiveCount;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
          gpuNode.minPoint[axis] = node.bounds.minPoint[axis];
          gpuNode.maxPoint[axis] = node.bounds.maxPoint[axis];
        }
        canonicalNodesGPU_.push_back(gpuNode);
      }

      const CWBVHBuilder::CompressionResult compressedBVH8 = CWBVHBuilder::compressBVH8(buildForCompression->bvh8Nodes, buildForCompression->bvh8LeafRemap);
      if (gpuCWBVHNodesCPU_.size() + compressedBVH8.nodes.size() > settings_.maxPackedCWBVHNodes ||
          gpuLeafRemapCPU_.size() + compressedBVH8.leafRemap.size() > settings_.maxPrimitives)
      {
        throw std::runtime_error("SoftwareBLASManager aggregate packed CWBVH capacity exceeded");
      }

      const uint32_t cwbvhNodeOffset = static_cast<uint32_t>(gpuCWBVHNodesCPU_.size());
      const uint32_t leafRemapOffset = static_cast<uint32_t>(gpuLeafRemapCPU_.size());
      for (const CWBVHBuilder::PackedCWBVHNode &packedNode : compressedBVH8.nodes)
      {
        CWBVHNodeGPU gpuNode{};
        for (uint32_t i = 0u; i < 2u; ++i)
        {
          gpuNode.orxy[i] = packedNode.orxy[i];
          gpuNode.oeim[i] = packedNode.oeim[i];
          gpuNode.ntbi[i] = packedNode.ntbi[i];
          gpuNode.info[i] = packedNode.info[i];
          gpuNode.qlox[i] = packedNode.qlox[i];
          gpuNode.qloy[i] = packedNode.qloy[i];
          gpuNode.qloz[i] = packedNode.qloz[i];
          gpuNode.qhix[i] = packedNode.qhix[i];
          gpuNode.qhiy[i] = packedNode.qhiy[i];
          gpuNode.qhiz[i] = packedNode.qhiz[i];
        }
        gpuCWBVHNodesCPU_.push_back(gpuNode);
      }
      gpuLeafRemapCPU_.insert(gpuLeafRemapCPU_.end(), compressedBVH8.leafRemap.begin(), compressedBVH8.leafRemap.end());
      record.gpuCWBVHNodeOffset = cwbvhNodeOffset;
      record.gpuCWBVHNodeCount = static_cast<uint32_t>(gpuCWBVHNodesCPU_.size()) - cwbvhNodeOffset;
      record.gpuLeafRemapOffset = leafRemapOffset;
      record.gpuLeafRemapCount = static_cast<uint32_t>(gpuLeafRemapCPU_.size()) - leafRemapOffset;
    }
    else
    {
      record.gpuCWBVHNodeCount = 0u;
    }

    TriangleBLASMetadataGPU triangleMetadata{};
    triangleMetadata.handleId = record.handle;
    triangleMetadata.cwbvhNodeOffset = record.gpuCWBVHNodeOffset;
    triangleMetadata.cwbvhNodeCount = record.gpuCWBVHNodeCount;
    triangleMetadata.leafRemapOffset = record.gpuLeafRemapOffset;
    triangleMetadata.primitiveRemapOffset = record.primitiveRemapOffset;
    triangleMetadata.primitiveCount = record.primitiveCount;
    triangleMetadata.triangleTransformOffset = record.triangleTransformOffset;
    triangleMetadata.vertexOffset = record.vertexOffset;
    triangleMetadata.indexOffset = record.indexOffset;
    triangleMetadata.primitiveMaterialOffset = record.primitiveMaterialOffset;
    triangleMetadataGPU_[record.handle] = triangleMetadata;

    RBVHCardBLASMetadataGPU rbvhMetadata{};
    rbvhMetadata.handleId = record.handle;
    rbvhMetadata.cwbvhNodeOffset = record.gpuCWBVHNodeOffset;
    rbvhMetadata.cwbvhNodeCount = record.gpuCWBVHNodeCount;
    rbvhMetadata.leafRemapOffset = record.gpuLeafRemapOffset;
    rbvhMetadata.primitiveRemapOffset = record.primitiveRemapOffset;
    rbvhMetadata.primitiveCount = record.primitiveCount;
    rbvhMetadata.rbvhCardOffset = record.rbvhCardOffset;
    rbvhMetadata.rbvhCardCount = record.rbvhCardCount;
    rbvhMetadata.rbvhDepthAtlasOffset = record.rbvhDepthAtlasOffset;
    rbvhMetadata.rbvhAtlasSize = record.rbvhAtlasSize;
    rbvhMetadata.rbvhAtlasTexelCount = record.rbvhAtlasTexelCount;
    rbvhMetadata.rbvhDepthMipOffset = record.rbvhDepthMipOffset;
    rbvhMetadata.rbvhDepthMipLevelCount = record.rbvhDepthMipLevelCount;
    rbvhCardMetadataGPU_[record.handle] = rbvhMetadata;
    recordsByHandle_[record.handle] = record;

    nextGpuHLBVHNodeOffset += record.gpuHLBVHNodeCount;
    nextGpuCWBVHNodeOffset += record.gpuCWBVHNodeCount;
    nextGpuLeafRemapOffset += record.primitiveCount;
  }

  rbvhDepthAtlasTextureTexelCount_ = static_cast<uint32_t>(rbvhAtlasTextureTexelCount64);
  if (rbvhDepthAtlasTextureTexelCount_ > 0u)
  {
    ensureRbvhDepthAtlasTexture(rbvhAtlasTextureWidth, rbvhAtlasTextureHeight);
  }
  else
  {
    ensureRbvhDepthAtlasTexture(1u, 1u);
    rbvhDepthAtlasTextureWidth_ = 1u;
    rbvhDepthAtlasTextureHeight_ = 1u;
  }

  const bool rebuilt = topologyChanged || !uniqueBuilds.empty() || lastBuildSummary_.blasCount != 0u;
  lastBuildSummary_.blasCount = static_cast<uint32_t>(uniqueBuilds.size());
  lastBuildSummary_.vertexCount = static_cast<uint32_t>(verticesGPU_.size());
  lastBuildSummary_.indexCount = static_cast<uint32_t>(indicesGPU_.size());
  lastBuildSummary_.nodeCount = static_cast<uint32_t>(canonicalNodesGPU_.size());
  lastBuildSummary_.primitiveCount = static_cast<uint32_t>(primitiveRemapGPU_.size());
  lastBuildSummary_.rbvhCardCount = static_cast<uint32_t>(rbvhCardsGPU_.size());
  lastBuildSummary_.buildRequested = rebuilt;
  lastBuildSummary_.rebuilt = rebuilt;
  if (rebuilt)
  {
    ++lastBuildSummary_.buildRevision;
  }

  uploadBuildOutputs(rebuilt);
  return lastBuildSummary_;
}

SoftwareBLASManager::BuildSummary SoftwareBLASManager::markClean()
{
  if (!lastBuildSummary_.buildRequested && !lastBuildSummary_.rebuilt)
  {
    return lastBuildSummary_;
  }

  lastBuildSummary_.buildRequested = false;
  lastBuildSummary_.rebuilt = false;
  uploadIdleState();
  return lastBuildSummary_;
}

SoftwareBLASManager::BuildSummary SoftwareBLASManager::clear()
{
  handleBySourcePath_.clear();
  recordsByHandle_.clear();
  freeHandles_.clear();
  nextHandle_ = 0u;
  triangleMetadataGPU_.clear();
  rbvhCardMetadataGPU_.clear();
  verticesGPU_.clear();
  indicesGPU_.clear();
  vertexPositionsGPU_.clear();
  canonicalNodesGPU_.clear();
  primitiveRemapGPU_.clear();
  primitiveMaterialIdsGPU_.clear();
  triangleTransformsGPU_.clear();
  sourceBVH8NodesGPU_.clear();
  rbvhCardsGPU_.clear();
  rbvhDepthAtlasGPU_.clear();
  rbvhCoverageAtlasGPU_.clear();
  rbvhDepthMipAtlasGPU_.clear();
  gpuCWBVHNodesCPU_.clear();
  gpuLeafRemapCPU_.clear();
  rbvhDepthAtlasTextureTexelCount_ = 0u;
  ensureRbvhDepthAtlasTexture(1u, 1u);
  rbvhDepthAtlasTextureWidth_ = 1u;
  rbvhDepthAtlasTextureHeight_ = 1u;

  lastBuildSummary_ = BuildSummary{};
  uploadRbvhDepthAtlasTexture();
  uploadIdleState();
  return lastBuildSummary_;
}

uint32_t SoftwareBLASManager::findHandleBySourcePath(const std::string_view sourcePath) const
{
  const auto it = handleBySourcePath_.find(std::string(sourcePath));
  return it == handleBySourcePath_.end() ? INVALID_BLAS_HANDLE : it->second;
}

bool SoftwareBLASManager::tryGetLocalBounds(const uint32_t handle, virtualgeometry::AABB *outBounds) const
{
  const auto it = recordsByHandle_.find(handle);
  if (it == recordsByHandle_.end() || outBounds == nullptr)
  {
    return false;
  }

  *outBounds = it->second.localBounds;
  return true;
}

bool SoftwareBLASManager::tryGetGPUBuildDescriptor(const uint32_t handle, GPUBuildDescriptor *outDescriptor) const
{
  const auto it = recordsByHandle_.find(handle);
  if (it == recordsByHandle_.end() || outDescriptor == nullptr)
  {
    return false;
  }

  *outDescriptor = GPUBuildDescriptor{
      .handle = it->second.handle,
      .localBounds = it->second.localBounds,
      .vertexOffset = it->second.vertexOffset,
      .vertexCount = it->second.vertexCount,
      .indexOffset = it->second.indexOffset,
      .indexCount = it->second.indexCount,
      .primitiveCount = it->second.primitiveCount,
      .gpuHLBVHNodeOffset = it->second.gpuHLBVHNodeOffset,
      .gpuHLBVHNodeCount = it->second.gpuHLBVHNodeCount,
      .gpuCWBVHNodeOffset = it->second.gpuCWBVHNodeOffset,
      .gpuCWBVHNodeCount = it->second.gpuCWBVHNodeCount,
      .gpuLeafRemapOffset = it->second.gpuLeafRemapOffset,
  };
  return true;
}

void SoftwareBLASManager::appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, const uint32_t frameSlot) const
{
  for (const auto &[bufferName, bufferIds] : frameOverrideBufferIds_)
  {
    if (bufferIds.empty())
    {
      continue;
    }

    overrides.bufferOverrides.emplace(
        bufferName,
        rendering::RenderGraphBufferOverride{
            .bufferId = bufferIds[frameSlot % bufferIds.size()],
        });
  }
}

void SoftwareBLASManager::initializeResources()
{
  triangleMetadataBufferSize_ = static_cast<uint64_t>(settings_.maxBlases) * sizeof(TriangleBLASMetadataGPU);
  rbvhCardMetadataBufferSize_ = static_cast<uint64_t>(settings_.maxBlases) * sizeof(RBVHCardBLASMetadataGPU);
  verticesBufferSize_ = static_cast<uint64_t>(settings_.maxVertices) * sizeof(virtualgeometry::FallbackMeshVertex);
  indicesBufferSize_ = static_cast<uint64_t>(settings_.maxIndices) * sizeof(uint32_t);
  vertexPositionsBufferSize_ = static_cast<uint64_t>(settings_.maxVertices) * sizeof(VertexPositionGPU);
  canonicalNodesBufferSize_ = static_cast<uint64_t>(settings_.maxNodes) * sizeof(CanonicalNodeGPU);
  primitiveRemapBufferSize_ = static_cast<uint64_t>(settings_.maxPrimitives) * sizeof(uint32_t);
  primitiveMaterialIdsBufferSize_ = static_cast<uint64_t>(settings_.maxPrimitives) * sizeof(uint32_t);
  triangleTransformsBufferSize_ = static_cast<uint64_t>(settings_.maxPrimitives) * sizeof(TriangleBaryTransformGPU);
  sourceBVH8NodesBufferSize_ = static_cast<uint64_t>(settings_.maxPackedCWBVHNodes) * sizeof(SoftwareRayTraceScene::BVH8NodeGPU);
  rbvhCardsBufferSize_ = static_cast<uint64_t>(settings_.maxRbvhCards) * sizeof(RBVHCardPrimitive);
  rbvhDepthAtlasBufferSize_ = static_cast<uint64_t>(settings_.maxRbvhAtlasTexels) * sizeof(uint32_t);
  rbvhCoverageAtlasBufferSize_ = static_cast<uint64_t>(settings_.maxRbvhAtlasTexels) * sizeof(uint32_t);
  rbvhDepthMipAtlasBufferSize_ = static_cast<uint64_t>(settings_.maxRbvhDepthMipTexels) * sizeof(uint32_t);
  gpuBuildAabbsBufferSize_ = static_cast<uint64_t>(settings_.maxPrimitives) * sizeof(GPUBuildAABBGPU);
  gpuBuildMortonCodesBufferSize_ = static_cast<uint64_t>(settings_.maxPrimitives) * sizeof(uint32_t);
  gpuBuildPrimitiveIndicesBufferSize_ = static_cast<uint64_t>(settings_.maxPrimitives) * sizeof(uint32_t);
  gpuBuildFlagsBufferSize_ = static_cast<uint64_t>(std::max(1u, settings_.maxPrimitives * 2u)) * sizeof(uint32_t);
  gpuBuildSignalsBufferSize_ = 64u * sizeof(uint32_t);
  gpuBuildHLBVHNodesBufferSize_ = static_cast<uint64_t>(settings_.maxNodes) * sizeof(CanonicalNodeGPU);
  gpuCWBVHNodesBufferSize_ = static_cast<uint64_t>(settings_.maxPackedCWBVHNodes) * sizeof(CWBVHNodeGPU);
  gpuLeafRemapBufferSize_ = static_cast<uint64_t>(settings_.maxPrimitives) * sizeof(uint32_t);
  gpuBuildDebugBufferSize_ = static_cast<uint64_t>(std::max(1u, settings_.maxPrimitives * 8u)) * sizeof(uint32_t);
  buildIndirectArgsBufferSize_ = sizeof(DispatchIndirectArgsGPU);
  countersBufferSize_ = sizeof(CountersGPU);

  const rendering::BufferUsage storageUsage =
      rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc;

  triangleMetadataBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_TriangleMetadata.buffer",
          .size = triangleMetadataBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  rbvhCardMetadataBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_RBVHCardMetadata.buffer",
          .size = rbvhCardMetadataBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  verticesBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_Vertices.buffer",
          .size = verticesBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  indicesBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_Indices.buffer",
          .size = indicesBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  vertexPositionsBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_VertexPositions.buffer",
          .size = vertexPositionsBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  canonicalNodesBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_CanonicalNodes.buffer",
          .size = canonicalNodesBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  primitiveRemapBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_PrimitiveRemap.buffer",
          .size = primitiveRemapBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  primitiveMaterialIdsBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_PrimitiveMaterialIds.buffer",
          .size = primitiveMaterialIdsBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  triangleTransformsBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_TriangleTransforms.buffer",
          .size = triangleTransformsBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  sourceBVH8NodesBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_SourceBVH8Nodes.buffer",
          .size = sourceBVH8NodesBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  rbvhCardsBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_RBVHCards.buffer",
          .size = rbvhCardsBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  rbvhDepthAtlasBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_RBVHDepthAtlas.buffer",
          .size = rbvhDepthAtlasBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  rbvhCoverageAtlasBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_RBVHCoverageAtlas.buffer",
          .size = rbvhCoverageAtlasBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  rbvhDepthMipAtlasBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_RBVHDepthMipAtlas.buffer",
          .size = rbvhDepthMipAtlasBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  gpuBuildAabbsBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_GPUBuildAABBs.buffer",
          .size = gpuBuildAabbsBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  gpuBuildMortonCodesBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_GPUBuildMortonCodes.buffer",
          .size = gpuBuildMortonCodesBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  gpuBuildPrimitiveIndicesBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_GPUBuildPrimitiveIndices.buffer",
          .size = gpuBuildPrimitiveIndicesBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  gpuBuildFlagsBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_GPUBuildFlags.buffer",
          .size = gpuBuildFlagsBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  gpuBuildSignalsBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_GPUBuildSignals.buffer",
          .size = gpuBuildSignalsBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  gpuBuildHLBVHNodesBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_GPUHLBVHNodes.buffer",
          .size = gpuBuildHLBVHNodesBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  gpuCWBVHNodesBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_GPUCWBVHNodes.buffer",
          .size = gpuCWBVHNodesBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  gpuLeafRemapBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_GPULeafRemap.buffer",
          .size = gpuLeafRemapBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  gpuBuildDebugBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_GPUBuildDebug.buffer",
          .size = gpuBuildDebugBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  buildIndirectArgsBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_BuildIndirectArgs.buffer",
          .size = buildIndirectArgsBufferSize_,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      },
      settings_.frameLocalBuffers);

  countersBuffer_ = createManagedBuffer(
      rendering::BufferInfo{
          .name = "SoftwareBLAS_Counters.buffer",
          .size = countersBufferSize_,
          .usage = storageUsage,
      },
      settings_.frameLocalBuffers);

  ensureRbvhDepthAtlasTexture(1u, 1u);
  uploadRbvhDepthAtlasTexture();
}

rendering::Buffer SoftwareBLASManager::createManagedBuffer(const rendering::BufferInfo info, const bool frameLocal)
{
  const rendering::Buffer buffer = renderGraph_->createBuffer(info);
  if (frameLocal)
  {
    initializeFrameOverrideBuffer(buffer, info.size, info.usage);
  }
  return buffer;
}

void SoftwareBLASManager::initializeFrameOverrideBuffer(
    const rendering::Buffer &buffer,
    const uint64_t size,
    const rendering::BufferUsage usage)
{
  const uint32_t frameCount = std::max(1u, renderGraph_->getMaxFramesInFlight());
  if (frameCount <= 1u)
  {
    return;
  }

  auto &bufferIds = frameOverrideBufferIds_[buffer.name];
  if (!bufferIds.empty())
  {
    return;
  }

  bufferIds.assign(frameCount, rendering::BufferId::Invalid);
  for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
  {
    bufferIds[frameSlot] = renderGraph_->getRHI()->createBuffer(
        rendering::BufferInfo{
            .name = buffer.name + ".override_frame" + std::to_string(frameSlot),
            .size = size,
            .usage = usage,
        });
  }
}

rendering::BufferId SoftwareBLASManager::getFrameOverrideBufferId(const rendering::Buffer &buffer, const uint32_t frameSlot) const
{
  const auto it = frameOverrideBufferIds_.find(buffer.name);
  if (it == frameOverrideBufferIds_.end() || it->second.empty())
  {
    return rendering::BufferId::Invalid;
  }

  return it->second[frameSlot % it->second.size()];
}

void SoftwareBLASManager::writeManagedBuffer(
    const rendering::Buffer &buffer,
    const uint64_t offset,
    const uint64_t size,
    const void *data) const
{
  if (!buffer.isValid() || data == nullptr || size == 0u)
  {
    return;
  }

  const rendering::BufferId overrideBufferId = getFrameOverrideBufferId(buffer, renderGraph_->getCurrentFrameIndex());
  if (overrideBufferId != rendering::BufferId::Invalid)
  {
    renderGraph_->getRHI()->bufferWrite(overrideBufferId, offset, size, const_cast<void *>(data));
    return;
  }

  renderGraph_->bufferWrite(buffer, offset, size, const_cast<void *>(data));
}

void SoftwareBLASManager::ensureRbvhDepthAtlasTexture(const uint32_t width, const uint32_t height)
{
  const uint32_t safeWidth = std::max(width, 1u);
  const uint32_t safeHeight = std::max(height, 1u);
  if (rbvhDepthAtlasTexture_.isValid() &&
      rbvhDepthAtlasTextureWidth_ == safeWidth &&
      rbvhDepthAtlasTextureHeight_ == safeHeight)
  {
    return;
  }

  if (rbvhDepthAtlasTexture_.isValid())
  {
    renderGraph_->deleteTexture(rbvhDepthAtlasTexture_);
    rbvhDepthAtlasTexture_ = {};
  }
  rbvhDepthAtlasTextureUploaded_ = false;

  rbvhDepthAtlasTexture_ = renderGraph_->createTexture(
      rendering::TextureInfo{
          .name = "SoftwareBLAS_RBVHDepthAtlas.texture",
          .format = rendering::Format::Format_RGBA16Float,
          .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
          .usage = rendering::ImageUsage::ImageUsage_Sampled | rendering::ImageUsage::ImageUsage_TransferDst,
          .width = safeWidth,
          .height = safeHeight,
          .depth = 1u,
          .mipLevels = 1u,
          .initialLayout = rendering::ResourceLayout::SHADER_READ_ONLY,
      });
  rbvhDepthAtlasTextureWidth_ = safeWidth;
  rbvhDepthAtlasTextureHeight_ = safeHeight;
}

void SoftwareBLASManager::uploadRbvhDepthAtlasTexture()
{
  if (!rbvhDepthAtlasTexture_.isValid())
  {
    return;
  }

  const uint32_t width = std::max(rbvhDepthAtlasTextureWidth_, 1u);
  const uint32_t height = std::max(rbvhDepthAtlasTextureHeight_, 1u);
  const size_t texelCount = static_cast<size_t>(width) * height;
  std::vector<uint16_t> atlasRGBA16F(texelCount * 4u, 0u);
  const uint16_t one = floatToHalfBits(1.0f);

  for (size_t texelIndex = 0u; texelIndex < texelCount; ++texelIndex)
  {
    const uint32_t packed = texelIndex < rbvhDepthAtlasGPU_.size() ? rbvhDepthAtlasGPU_[texelIndex] : 0u;
    const uint32_t encodedDepth = packed & 0xffffu;
    const uint32_t coverage = (packed >> 16u) & 0xffu;
    const float depth01 = static_cast<float>(encodedDepth) / 65535.0f;
    const size_t dst = texelIndex * 4u;
    atlasRGBA16F[dst + 0u] = floatToHalfBits(depth01);
    atlasRGBA16F[dst + 1u] = coverage == 1u ? one : 0u;
    atlasRGBA16F[dst + 2u] = coverage > 0u ? one : 0u;
    atlasRGBA16F[dst + 3u] = 0u;
  }

  renderGraph_->textureWrite(
      rbvhDepthAtlasTexture_,
      atlasRGBA16F.size() * sizeof(uint16_t),
      atlasRGBA16F.data(),
      rbvhDepthAtlasTextureUploaded_ ? rendering::ResourceLayout::SHADER_READ_ONLY : rendering::ResourceLayout::UNDEFINED);
  rbvhDepthAtlasTextureUploaded_ = true;
}

void SoftwareBLASManager::uploadIdleState()
{
  const DispatchIndirectArgsGPU dispatchArgs{};
  CountersGPU counters{};
  counters.blasCount = lastBuildSummary_.blasCount;
  counters.vertexCount = lastBuildSummary_.vertexCount;
  counters.indexCount = lastBuildSummary_.indexCount;
  counters.nodeCount = lastBuildSummary_.nodeCount;
  counters.primitiveCount = lastBuildSummary_.primitiveCount;
  counters.buildRevision = lastBuildSummary_.buildRevision;
  counters.buildRequested = 0u;

  writeManagedBuffer(buildIndirectArgsBuffer_, 0u, sizeof(dispatchArgs), &dispatchArgs);
  writeManagedBuffer(countersBuffer_, 0u, sizeof(counters), &counters);
}

void SoftwareBLASManager::uploadBuildOutputs(const bool rebuilt)
{
  if (!triangleMetadataGPU_.empty())
  {
    writeManagedBuffer(triangleMetadataBuffer_, 0u, triangleMetadataGPU_.size() * sizeof(TriangleBLASMetadataGPU), triangleMetadataGPU_.data());
  }

  if (!rbvhCardMetadataGPU_.empty())
  {
    writeManagedBuffer(rbvhCardMetadataBuffer_, 0u, rbvhCardMetadataGPU_.size() * sizeof(RBVHCardBLASMetadataGPU), rbvhCardMetadataGPU_.data());
  }

  if (!verticesGPU_.empty())
  {
    writeManagedBuffer(verticesBuffer_, 0u, verticesGPU_.size() * sizeof(virtualgeometry::FallbackMeshVertex), verticesGPU_.data());
  }

  if (!indicesGPU_.empty())
  {
    writeManagedBuffer(indicesBuffer_, 0u, indicesGPU_.size() * sizeof(uint32_t), indicesGPU_.data());
  }

  if (!vertexPositionsGPU_.empty())
  {
    writeManagedBuffer(vertexPositionsBuffer_, 0u, vertexPositionsGPU_.size() * sizeof(VertexPositionGPU), vertexPositionsGPU_.data());
  }

  if (!canonicalNodesGPU_.empty())
  {
    writeManagedBuffer(canonicalNodesBuffer_, 0u, canonicalNodesGPU_.size() * sizeof(CanonicalNodeGPU), canonicalNodesGPU_.data());
  }

  if (!primitiveRemapGPU_.empty())
  {
    writeManagedBuffer(primitiveRemapBuffer_, 0u, primitiveRemapGPU_.size() * sizeof(uint32_t), primitiveRemapGPU_.data());
  }

  if (!primitiveMaterialIdsGPU_.empty())
  {
    writeManagedBuffer(primitiveMaterialIdsBuffer_, 0u, primitiveMaterialIdsGPU_.size() * sizeof(uint32_t), primitiveMaterialIdsGPU_.data());
  }

  if (!triangleTransformsGPU_.empty())
  {
    writeManagedBuffer(triangleTransformsBuffer_, 0u, triangleTransformsGPU_.size() * sizeof(TriangleBaryTransformGPU), triangleTransformsGPU_.data());
  }

  if (!sourceBVH8NodesGPU_.empty())
  {
    writeManagedBuffer(sourceBVH8NodesBuffer_, 0u, sourceBVH8NodesGPU_.size() * sizeof(SoftwareRayTraceScene::BVH8NodeGPU), sourceBVH8NodesGPU_.data());
  }

  if (!rbvhCardsGPU_.empty())
  {
    writeManagedBuffer(rbvhCardsBuffer_, 0u, rbvhCardsGPU_.size() * sizeof(RBVHCardPrimitive), rbvhCardsGPU_.data());
  }

  if (!rbvhDepthAtlasGPU_.empty())
  {
    writeManagedBuffer(rbvhDepthAtlasBuffer_, 0u, rbvhDepthAtlasGPU_.size() * sizeof(uint32_t), rbvhDepthAtlasGPU_.data());
  }
  uploadRbvhDepthAtlasTexture();

  if (!rbvhCoverageAtlasGPU_.empty())
  {
    writeManagedBuffer(rbvhCoverageAtlasBuffer_, 0u, rbvhCoverageAtlasGPU_.size() * sizeof(uint32_t), rbvhCoverageAtlasGPU_.data());
  }

  if (!rbvhDepthMipAtlasGPU_.empty())
  {
    writeManagedBuffer(rbvhDepthMipAtlasBuffer_, 0u, rbvhDepthMipAtlasGPU_.size() * sizeof(uint32_t), rbvhDepthMipAtlasGPU_.data());
  }

  if (!gpuCWBVHNodesCPU_.empty())
  {
    writeManagedBuffer(gpuCWBVHNodesBuffer_, 0u, gpuCWBVHNodesCPU_.size() * sizeof(CWBVHNodeGPU), gpuCWBVHNodesCPU_.data());
  }

  if (!gpuLeafRemapCPU_.empty())
  {
    writeManagedBuffer(gpuLeafRemapBuffer_, 0u, gpuLeafRemapCPU_.size() * sizeof(uint32_t), gpuLeafRemapCPU_.data());
  }

  DispatchIndirectArgsGPU dispatchArgs{};
  if (rebuilt && lastBuildSummary_.primitiveCount > 0u)
  {
    dispatchArgs.groupCountX = divCeilU32(lastBuildSummary_.primitiveCount, settings_.workgroupSize);
  }

  CountersGPU counters{};
  counters.blasCount = lastBuildSummary_.blasCount;
  counters.vertexCount = lastBuildSummary_.vertexCount;
  counters.indexCount = lastBuildSummary_.indexCount;
  counters.nodeCount = lastBuildSummary_.nodeCount;
  counters.primitiveCount = lastBuildSummary_.primitiveCount;
  counters.buildRevision = lastBuildSummary_.buildRevision;
  counters.buildRequested = rebuilt ? 1u : 0u;

  writeManagedBuffer(buildIndirectArgsBuffer_, 0u, sizeof(dispatchArgs), &dispatchArgs);
  writeManagedBuffer(countersBuffer_, 0u, sizeof(counters), &counters);
}

std::vector<SoftwareRayTraceScene::SourceBLAS> SoftwareBLASManager::buildRayTraceSceneSources() const
{
  std::vector<SoftwareRayTraceScene::SourceBLAS> sources;
  sources.reserve(recordsByHandle_.size());
  for (const auto &[_, record] : recordsByHandle_)
  {
    if (record.handle == INVALID_BLAS_HANDLE ||
        record.sourceBVH8NodeCount == 0u ||
        record.gpuCWBVHNodeCount == 0u ||
        record.primitiveCount == 0u)
    {
      continue;
    }

    sources.push_back(
        SoftwareRayTraceScene::SourceBLAS{
            .handle = record.handle,
            .localBVH8NodeOffset = record.sourceBVH8NodeOffset,
            .localTriangleOffset = record.triangleTransformOffset,
            .localLeafRemapOffset = record.gpuLeafRemapOffset,
            .primitiveRemapOffset = record.primitiveRemapOffset,
            .bvh8NodeCount = record.sourceBVH8NodeCount,
            .cwbvhNodeCount = record.gpuCWBVHNodeCount,
            .triangleCount = record.primitiveCount,
            .leafRemapCount = record.gpuLeafRemapCount,
        });
  }
  return sources;
}

uint32_t SoftwareBLASManager::allocateHandle()
{
  if (!freeHandles_.empty())
  {
    const uint32_t handle = freeHandles_.back();
    freeHandles_.pop_back();
    return handle;
  }

  if (nextHandle_ >= settings_.maxBlases)
  {
    throw std::runtime_error("SoftwareBLASManager ran out of BLAS handles");
  }

  return nextHandle_++;
}

} // namespace softwarebvh
