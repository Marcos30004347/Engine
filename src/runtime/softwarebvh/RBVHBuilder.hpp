#pragma once

#include <cstdint>
#include <vector>

#include "softwarebvh/CWBVHBuilder.hpp"
#include "virtualgeometry/FallbackMeshFile.hpp"

namespace softwarebvh
{

enum class RBVHRefinementCriterion : uint32_t
{
  NormalCone = 0u,
  ProjectedArea = 1u,
};

enum class RBVHSplitStrategy : uint32_t
{
  SurfaceAreaHeuristic = 0u,
  SpatialMedian = 1u,
};

struct RBVHBuildOptions
{
  uint32_t sahBinCount = 8u;
  uint32_t minCardResolution = 4u;
  uint32_t maxCardResolution = 1024u;
  uint32_t maxAtlasSize = 8192u;
  uint32_t maxCards = 32768u;
  float texelsPerWorldUnit = 0.0f;
  float texelsPerUvUnit = 256.0f;
  float pixelToAreaRatio = 0.0f;
  float targetTileResolution = 0.0f;
  RBVHRefinementCriterion refinementCriterion = RBVHRefinementCriterion::ProjectedArea;
  float projectedAreaThreshold = 0.78f;
  float minNormalDot = 0.5f;
  float cardBoundsPadding = 1.0e-4f;
  float splitOverlapFraction = 0.0f;
  bool adaptiveLodEnabled = false;
  RBVHSplitStrategy splitStrategy = RBVHSplitStrategy::SpatialMedian;
  float adaptiveLodPoint[3] = {0.0f, 0.0f, 0.0f};
  float adaptiveLodNearDistance = 0.0f;
  float adaptiveLodFarDistance = 1.0f;
  float adaptiveLodFarMinNormalDot = 0.25f;
  float adaptiveLodFarProjectedAreaThreshold = 0.45f;
  float adaptiveLodFarPixelToAreaScale = 0.25f;
  float adaptiveLodFarTargetTileResolutionScale = 1.0f;
};

struct RBVHCardPrimitive
{
  float origin[3] = {0.0f, 0.0f, 0.0f};
  uint32_t flags = 0u;

  float axisU[3] = {1.0f, 0.0f, 0.0f};
  float halfExtentU = 0.0f;

  float axisV[3] = {0.0f, 1.0f, 0.0f};
  float halfExtentV = 0.0f;

  float axisN[3] = {0.0f, 0.0f, 1.0f};
  float depthMin = 0.0f;

  float boundsMin[3] = {0.0f, 0.0f, 0.0f};
  float depthMax = 0.0f;

  float boundsMax[3] = {0.0f, 0.0f, 0.0f};
  float samplingError = 0.0f;

  uint32_t atlasX = 0u;
  uint32_t atlasY = 0u;
  uint32_t atlasWidth = 0u;
  uint32_t atlasHeight = 0u;

  uint32_t sourceNodeIndex = CWBVHBuilder::INVALID_NODE;
  uint32_t sourcePrimitiveOffset = 0u;
  uint32_t sourcePrimitiveCount = 0u;
  uint32_t _padding0 = 0u;
};
static_assert(sizeof(RBVHCardPrimitive) == 128u, "RBVHCardPrimitive must stay tightly packed");

struct RBVHBuildResult
{
  uint32_t lodLevel = 0u;
  uint32_t tileSize = 0u;
  uint32_t atlasSize = 0u;
  uint32_t cardsPerRow = 0u;
  std::vector<RBVHCardPrimitive> cards;
  std::vector<uint32_t> sourcePrimitiveIndices;
  std::vector<uint16_t> depthAtlasR16;
  std::vector<uint8_t> coverageAtlasR8;
  std::vector<uint32_t> depthMinMaxMipAtlasRG16;
  std::vector<uint8_t> albedoAtlasRGBA8;
  CWBVHBuilder::BuildResult cardBuild;
};

class RBVHBuilder
{
public:
  static RBVHBuildResult build(const virtualgeometry::FallbackMeshData &mesh);
  static RBVHBuildResult build(const virtualgeometry::FallbackMeshData &mesh, const RBVHBuildOptions &options);
  static RBVHBuildResult build(const virtualgeometry::FallbackMeshData &mesh, const CWBVHBuilder::BuildResult &triangleBuild, const RBVHBuildOptions &options);
  static RBVHBuildResult build(const virtualgeometry::FallbackMeshData &mesh, const std::vector<CWBVHBuilder::PrimitiveBounds> &primitiveBounds, const RBVHBuildOptions &options);
  static std::vector<uint32_t> buildDepthMinMaxMipAtlas(const uint32_t atlasSize, const std::vector<uint16_t> &depthAtlasR16, const std::vector<uint8_t> &coverageAtlasR8);
};

} // namespace softwarebvh
