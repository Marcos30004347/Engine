#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "softwarebvh/CWBVHBuilder.hpp"
#include "softwarebvh/RBVHBuilder.hpp"
#include "softwarebvh/SoftwareRayTraceScene.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualgeometry/FallbackMeshFile.hpp"
#include "virtualgeometry/VirtualGeometryBounds.hpp"

namespace softwarebvh
{

class SoftwareBLASManager
{
public:
  static constexpr uint32_t INVALID_BLAS_HANDLE = UINT32_MAX;
  static constexpr uint32_t INVALID_NODE = UINT32_MAX;
  static constexpr uint32_t INVALID_PRIMITIVE_INDEX = UINT32_MAX;
  static constexpr uint32_t NODE_FLAG_LEAF = 1u << 0u;
  static constexpr uint32_t BLAS_FLAG_RBVH_CARD = 1u << 0u;

  struct Settings
  {
    uint32_t maxBlases = 4096u;
    uint32_t maxVertices = 8u * 1024u * 1024u;
    uint32_t maxIndices = 16u * 1024u * 1024u;
    uint32_t maxNodes = 16u * 1024u * 1024u;
    uint32_t maxPrimitives = 8u * 1024u * 1024u;
    uint32_t maxPackedCWBVHNodes = 8u * 1024u * 1024u;
    uint32_t maxRbvhCards = 256u * 1024u;
    uint32_t maxRbvhAtlasTexels = 8u * 1024u * 1024u;
    uint32_t maxRbvhDepthMipTexels = 12u * 1024u * 1024u;
    bool frameLocalBuffers = false;
    uint32_t workgroupSize = 64u;
  };

  struct BuildInput
  {
    std::string sourcePath;
    std::string fallbackPath;
    const virtualgeometry::FallbackMeshData *meshRef = nullptr;
    const CWBVHBuilder::BuildResult *prebuiltRef = nullptr;
    const RBVHBuildResult *rbvhBuildRef = nullptr;
    virtualgeometry::FallbackMeshData mesh;
    std::optional<CWBVHBuilder::BuildResult> prebuilt;
    std::optional<RBVHBuildResult> rbvhBuild;
  };

  struct VertexPositionGPU
  {
    float position[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  };
  static_assert(sizeof(VertexPositionGPU) == 16u, "SoftwareBLASManager::VertexPositionGPU must stay tightly packed");

  struct TriangleBLASMetadataGPU
  {
    uint32_t handleId = INVALID_BLAS_HANDLE;
    uint32_t cwbvhNodeOffset = 0u;
    uint32_t cwbvhNodeCount = 0u;
    uint32_t leafRemapOffset = 0u;
    uint32_t primitiveRemapOffset = 0u;
    uint32_t primitiveCount = 0u;
    uint32_t triangleTransformOffset = 0u;
    uint32_t vertexOffset = 0u;
    uint32_t indexOffset = 0u;
    uint32_t primitiveMaterialOffset = 0u;
  };
  static_assert(sizeof(TriangleBLASMetadataGPU) == 40u, "SoftwareBLASManager::TriangleBLASMetadataGPU must stay tightly packed");

  struct RBVHCardBLASMetadataGPU
  {
    uint32_t handleId = INVALID_BLAS_HANDLE;
    uint32_t cwbvhNodeOffset = 0u;
    uint32_t cwbvhNodeCount = 0u;
    uint32_t leafRemapOffset = 0u;
    uint32_t primitiveRemapOffset = 0u;
    uint32_t primitiveCount = 0u;
    uint32_t rbvhCardOffset = 0u;
    uint32_t rbvhCardCount = 0u;
    uint32_t rbvhDepthAtlasOffset = 0u;
    uint32_t rbvhAtlasSize = 0u;
    uint32_t rbvhAtlasTexelCount = 0u;
    uint32_t rbvhDepthMipOffset = 0u;
    uint32_t rbvhDepthMipLevelCount = 0u;
    uint32_t _padding0 = 0u;
    uint32_t _padding1 = 0u;
    uint32_t _padding2 = 0u;
  };
  static_assert(sizeof(RBVHCardBLASMetadataGPU) == 64u, "SoftwareBLASManager::RBVHCardBLASMetadataGPU must stay tightly packed");

  struct CanonicalNodeGPU
  {
    uint32_t parent = INVALID_NODE;
    uint32_t left = INVALID_NODE;
    uint32_t right = INVALID_NODE;
    uint32_t primitiveIndex = INVALID_PRIMITIVE_INDEX;
    float minPoint[3]{0.0f, 0.0f, 0.0f};
    uint32_t flags = 0u;
    float maxPoint[3]{0.0f, 0.0f, 0.0f};
    uint32_t subtreePrimitiveCount = 0u;
  };
  static_assert(sizeof(CanonicalNodeGPU) == 48u, "SoftwareBLASManager::CanonicalNodeGPU must stay tightly packed");

  struct GPUBuildAABBGPU
  {
    float minPoint[3]{0.0f, 0.0f, 0.0f};
    float _padding0 = 0.0f;
    float maxPoint[3]{0.0f, 0.0f, 0.0f};
    float _padding1 = 0.0f;
  };
  static_assert(sizeof(GPUBuildAABBGPU) == 32u, "SoftwareBLASManager::GPUBuildAABBGPU must stay tightly packed");

  struct GPUBuildUniforms
  {
    uint32_t num = 0u;
    uint32_t targetPrimitivesPerNode = 1u;
    uint32_t targetNumberOfLeaves = 0u;
    uint32_t padding = 0u;
    GPUBuildAABBGPU volumeAabb{};
  };
  static_assert(sizeof(GPUBuildUniforms) == 48u, "SoftwareBLASManager::GPUBuildUniforms must stay tightly packed");

  struct TriangleAABBUniforms
  {
    uint32_t primitiveCount = 0u;
    uint32_t _padding0 = 0u;
    uint32_t _padding1 = 0u;
    uint32_t _padding2 = 0u;
  };
  static_assert(sizeof(TriangleAABBUniforms) == 16u, "SoftwareBLASManager::TriangleAABBUniforms must stay tightly packed");

  struct CWBVHNodeGPU
  {
    uint32_t orxy[2]{};
    uint32_t oeim[2]{};
    uint32_t ntbi[2]{};
    uint32_t info[2]{};
    uint32_t qlox[2]{};
    uint32_t qloy[2]{};
    uint32_t qloz[2]{};
    uint32_t qhix[2]{};
    uint32_t qhiy[2]{};
    uint32_t qhiz[2]{};
  };
  static_assert(sizeof(CWBVHNodeGPU) == 80u, "SoftwareBLASManager::CWBVHNodeGPU must stay tightly packed");

  struct TriangleBaryTransformGPU
  {
    float row0[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float row1[4]{0.0f, 0.0f, 0.0f, 0.0f};
    float row2[4]{0.0f, 0.0f, 0.0f, 0.0f};
  };
  static_assert(sizeof(TriangleBaryTransformGPU) == 48u, "SoftwareBLASManager::TriangleBaryTransformGPU must stay tightly packed");

  struct DispatchIndirectArgsGPU
  {
    uint32_t groupCountX = 0u;
    uint32_t groupCountY = 1u;
    uint32_t groupCountZ = 1u;
  };
  static_assert(sizeof(DispatchIndirectArgsGPU) == 12u, "SoftwareBLASManager::DispatchIndirectArgsGPU must stay tightly packed");

  struct CountersGPU
  {
    uint32_t blasCount = 0u;
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t nodeCount = 0u;
    uint32_t primitiveCount = 0u;
    uint32_t buildRevision = 0u;
    uint32_t buildRequested = 0u;
    uint32_t _padding0 = 0u;
  };
  static_assert(sizeof(CountersGPU) == 32u, "SoftwareBLASManager::CountersGPU must stay tightly packed");

  struct BuildSummary
  {
    uint32_t blasCount = 0u;
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t nodeCount = 0u;
    uint32_t primitiveCount = 0u;
    uint32_t rbvhCardCount = 0u;
    uint32_t buildRevision = 0u;
    bool buildRequested = false;
    bool rebuilt = false;
  };

  struct GPUBuildDescriptor
  {
    uint32_t handle = INVALID_BLAS_HANDLE;
    virtualgeometry::AABB localBounds{};
    uint32_t vertexOffset = 0u;
    uint32_t vertexCount = 0u;
    uint32_t indexOffset = 0u;
    uint32_t indexCount = 0u;
    uint32_t primitiveCount = 0u;
    uint32_t gpuHLBVHNodeOffset = 0u;
    uint32_t gpuHLBVHNodeCount = 0u;
    uint32_t gpuCWBVHNodeOffset = 0u;
    uint32_t gpuCWBVHNodeCount = 0u;
    uint32_t gpuLeafRemapOffset = 0u;
  };

  explicit SoftwareBLASManager(rendering::RenderGraph *renderGraph);
  SoftwareBLASManager(rendering::RenderGraph *renderGraph, Settings settings);
  ~SoftwareBLASManager();

  BuildSummary syncFromCPU(const std::vector<BuildInput> &buildInputs);
  BuildSummary markClean();
  BuildSummary clear();

  const Settings &getSettings() const
  {
    return settings_;
  }

  const BuildSummary &getLastBuildSummary() const
  {
    return lastBuildSummary_;
  }

  uint32_t findHandleBySourcePath(std::string_view sourcePath) const;
  bool tryGetLocalBounds(uint32_t handle, virtualgeometry::AABB *outBounds) const;
  bool tryGetGPUBuildDescriptor(uint32_t handle, GPUBuildDescriptor *outDescriptor) const;

  const rendering::Buffer &getTriangleMetadataBuffer() const
  {
    return triangleMetadataBuffer_;
  }

  const rendering::Buffer &getRbvhCardMetadataBuffer() const
  {
    return rbvhCardMetadataBuffer_;
  }

  const rendering::Buffer &getMetadataBuffer() const
  {
    return getTriangleMetadataBuffer();
  }

  const rendering::Buffer &getVerticesBuffer() const
  {
    return verticesBuffer_;
  }

  const rendering::Buffer &getIndicesBuffer() const
  {
    return indicesBuffer_;
  }

  const rendering::Buffer &getVertexPositionsBuffer() const
  {
    return vertexPositionsBuffer_;
  }

  const rendering::Buffer &getCanonicalNodesBuffer() const
  {
    return canonicalNodesBuffer_;
  }

  const rendering::Buffer &getPrimitiveRemapBuffer() const
  {
    return primitiveRemapBuffer_;
  }

  const rendering::Buffer &getPrimitiveMaterialIdsBuffer() const
  {
    return primitiveMaterialIdsBuffer_;
  }

  const rendering::Buffer &getTriangleTransformsBuffer() const
  {
    return triangleTransformsBuffer_;
  }

  const rendering::Buffer &getSourceBVH8NodesBuffer() const
  {
    return sourceBVH8NodesBuffer_;
  }

  const rendering::Buffer &getRbvhCardsBuffer() const
  {
    return rbvhCardsBuffer_;
  }

  const rendering::Buffer &getRbvhDepthAtlasBuffer() const
  {
    return rbvhDepthAtlasBuffer_;
  }

  const rendering::Texture &getRbvhDepthAtlasTexture() const
  {
    return rbvhDepthAtlasTexture_;
  }

  uint32_t getRbvhDepthAtlasTextureWidth() const
  {
    return rbvhDepthAtlasTextureWidth_;
  }

  uint32_t getRbvhDepthAtlasTextureHeight() const
  {
    return rbvhDepthAtlasTextureHeight_;
  }

  uint32_t getRbvhDepthAtlasTextureTexelCount() const
  {
    return rbvhDepthAtlasTextureTexelCount_;
  }

  const rendering::Buffer &getRbvhCoverageAtlasBuffer() const
  {
    return rbvhCoverageAtlasBuffer_;
  }

  const rendering::Buffer &getRbvhDepthMipAtlasBuffer() const
  {
    return rbvhDepthMipAtlasBuffer_;
  }

  const rendering::Buffer &getBuildIndirectArgsBuffer() const
  {
    return buildIndirectArgsBuffer_;
  }

  const rendering::Buffer &getCountersBuffer() const
  {
    return countersBuffer_;
  }

  uint64_t getTriangleMetadataBufferSize() const
  {
    return triangleMetadataBufferSize_;
  }

  uint64_t getRbvhCardMetadataBufferSize() const
  {
    return rbvhCardMetadataBufferSize_;
  }

  uint64_t getMetadataBufferSize() const
  {
    return getTriangleMetadataBufferSize();
  }

  uint64_t getVerticesBufferSize() const
  {
    return verticesBufferSize_;
  }

  uint64_t getIndicesBufferSize() const
  {
    return indicesBufferSize_;
  }

  uint64_t getPrimitiveRemapBufferSize() const
  {
    return primitiveRemapBufferSize_;
  }

  uint64_t getPrimitiveMaterialIdsBufferSize() const
  {
    return primitiveMaterialIdsBufferSize_;
  }

  uint64_t getTriangleTransformsBufferSize() const
  {
    return triangleTransformsBufferSize_;
  }

  uint64_t getSourceBVH8NodesBufferSize() const
  {
    return sourceBVH8NodesBufferSize_;
  }

  uint64_t getRbvhCardsBufferSize() const
  {
    return rbvhCardsBufferSize_;
  }

  uint64_t getRbvhDepthAtlasBufferSize() const
  {
    return rbvhDepthAtlasBufferSize_;
  }

  uint64_t getRbvhCoverageAtlasBufferSize() const
  {
    return rbvhCoverageAtlasBufferSize_;
  }

  uint64_t getRbvhDepthMipAtlasBufferSize() const
  {
    return rbvhDepthMipAtlasBufferSize_;
  }

  uint64_t getGPUCWBVHNodesBufferSize() const
  {
    return gpuCWBVHNodesBufferSize_;
  }

  uint64_t getGPULeafRemapBufferSize() const
  {
    return gpuLeafRemapBufferSize_;
  }

  uint32_t getGPUCWBVHNodeCount() const
  {
    return static_cast<uint32_t>(gpuCWBVHNodesCPU_.size());
  }

  uint32_t getSourceBVH8NodeCount() const
  {
    return static_cast<uint32_t>(sourceBVH8NodesGPU_.size());
  }

  uint32_t getCanonicalNodeCount() const
  {
    return static_cast<uint32_t>(canonicalNodesGPU_.size());
  }

  const rendering::Buffer &getGPUBuildAabbsBuffer() const
  {
    return gpuBuildAabbsBuffer_;
  }

  const rendering::Buffer &getGPUBuildMortonCodesBuffer() const
  {
    return gpuBuildMortonCodesBuffer_;
  }

  const rendering::Buffer &getGPUBuildPrimitiveIndicesBuffer() const
  {
    return gpuBuildPrimitiveIndicesBuffer_;
  }

  const rendering::Buffer &getGPUBuildFlagsBuffer() const
  {
    return gpuBuildFlagsBuffer_;
  }

  const rendering::Buffer &getGPUBuildSignalsBuffer() const
  {
    return gpuBuildSignalsBuffer_;
  }

  const rendering::Buffer &getGPUBuildHLBVHNodesBuffer() const
  {
    return gpuBuildHLBVHNodesBuffer_;
  }

  const rendering::Buffer &getGPUCWBVHNodesBuffer() const
  {
    return gpuCWBVHNodesBuffer_;
  }

  const rendering::Buffer &getGPULeafRemapBuffer() const
  {
    return gpuLeafRemapBuffer_;
  }

  const rendering::Buffer &getGPUBuildDebugBuffer() const
  {
    return gpuBuildDebugBuffer_;
  }

  void appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const;
  std::vector<SoftwareRayTraceScene::SourceBLAS> buildRayTraceSceneSources() const;

private:
  struct BLASRecord
  {
    uint32_t handle = INVALID_BLAS_HANDLE;
    std::string sourcePath;
    std::string fallbackPath;
    virtualgeometry::AABB localBounds{};
    uint32_t nodeOffset = 0u;
    uint32_t nodeCount = 0u;
    uint32_t rootNodeIndex = INVALID_NODE;
    uint32_t vertexOffset = 0u;
    uint32_t vertexCount = 0u;
    uint32_t indexOffset = 0u;
    uint32_t indexCount = 0u;
    uint32_t primitiveRemapOffset = 0u;
    uint32_t primitiveCount = 0u;
    uint32_t triangleTransformOffset = 0u;
    uint32_t primitiveMaterialOffset = 0u;
    uint32_t lodLevel = 0u;
    uint32_t sourceBVH8NodeOffset = 0u;
    uint32_t sourceBVH8NodeCount = 0u;
    uint32_t gpuHLBVHNodeOffset = 0u;
    uint32_t gpuHLBVHNodeCount = 0u;
    uint32_t gpuCWBVHNodeOffset = 0u;
    uint32_t gpuCWBVHNodeCount = 0u;
    uint32_t gpuLeafRemapOffset = 0u;
    uint32_t gpuLeafRemapCount = 0u;
    uint32_t rbvhCardOffset = 0u;
    uint32_t rbvhCardCount = 0u;
    uint32_t rbvhDepthAtlasOffset = 0u;
    uint32_t rbvhCoverageAtlasOffset = 0u;
    uint32_t rbvhAtlasSize = 0u;
    uint32_t rbvhAtlasTexelCount = 0u;
    uint32_t rbvhDepthMipOffset = 0u;
    uint32_t rbvhDepthMipLevelCount = 0u;
  };

  struct UniqueBuildInput
  {
    uint32_t handle = INVALID_BLAS_HANDLE;
    const BuildInput *input = nullptr;
  };

  void ensureCapacityForBuildInputs(const std::vector<BuildInput> &buildInputs);
  void destroyResources();
  void initializeResources();
  rendering::Buffer createManagedBuffer(rendering::BufferInfo info, bool frameLocal);
  void initializeFrameOverrideBuffer(const rendering::Buffer &buffer, uint64_t size, rendering::BufferUsage usage);
  rendering::BufferId getFrameOverrideBufferId(const rendering::Buffer &buffer, uint32_t frameSlot) const;
  void writeManagedBuffer(const rendering::Buffer &buffer, uint64_t offset, uint64_t size, const void *data) const;
  void ensureRbvhDepthAtlasTexture(uint32_t width, uint32_t height);
  void uploadRbvhDepthAtlasTexture();
  void uploadIdleState();
  void uploadBuildOutputs(bool rebuilt);
  uint32_t allocateHandle();

  rendering::RenderGraph *renderGraph_ = nullptr;
  Settings settings_{};

  rendering::Buffer triangleMetadataBuffer_;
  rendering::Buffer rbvhCardMetadataBuffer_;
  rendering::Buffer verticesBuffer_;
  rendering::Buffer indicesBuffer_;
  rendering::Buffer vertexPositionsBuffer_;
  rendering::Buffer canonicalNodesBuffer_;
  rendering::Buffer primitiveRemapBuffer_;
  rendering::Buffer primitiveMaterialIdsBuffer_;
  rendering::Buffer triangleTransformsBuffer_;
  rendering::Buffer sourceBVH8NodesBuffer_;
  rendering::Buffer rbvhCardsBuffer_;
  rendering::Buffer rbvhDepthAtlasBuffer_;
  rendering::Buffer rbvhCoverageAtlasBuffer_;
  rendering::Buffer rbvhDepthMipAtlasBuffer_;
  rendering::Texture rbvhDepthAtlasTexture_;
  rendering::Buffer gpuBuildAabbsBuffer_;
  rendering::Buffer gpuBuildMortonCodesBuffer_;
  rendering::Buffer gpuBuildPrimitiveIndicesBuffer_;
  rendering::Buffer gpuBuildFlagsBuffer_;
  rendering::Buffer gpuBuildSignalsBuffer_;
  rendering::Buffer gpuBuildHLBVHNodesBuffer_;
  rendering::Buffer gpuCWBVHNodesBuffer_;
  rendering::Buffer gpuLeafRemapBuffer_;
  rendering::Buffer gpuBuildDebugBuffer_;
  rendering::Buffer buildIndirectArgsBuffer_;
  rendering::Buffer countersBuffer_;

  uint64_t triangleMetadataBufferSize_ = 0u;
  uint64_t rbvhCardMetadataBufferSize_ = 0u;
  uint64_t verticesBufferSize_ = 0u;
  uint64_t indicesBufferSize_ = 0u;
  uint64_t vertexPositionsBufferSize_ = 0u;
  uint64_t canonicalNodesBufferSize_ = 0u;
  uint64_t primitiveRemapBufferSize_ = 0u;
  uint64_t primitiveMaterialIdsBufferSize_ = 0u;
  uint64_t triangleTransformsBufferSize_ = 0u;
  uint64_t sourceBVH8NodesBufferSize_ = 0u;
  uint64_t rbvhCardsBufferSize_ = 0u;
  uint64_t rbvhDepthAtlasBufferSize_ = 0u;
  uint64_t rbvhCoverageAtlasBufferSize_ = 0u;
  uint64_t rbvhDepthMipAtlasBufferSize_ = 0u;
  uint32_t rbvhDepthAtlasTextureWidth_ = 1u;
  uint32_t rbvhDepthAtlasTextureHeight_ = 1u;
  uint32_t rbvhDepthAtlasTextureTexelCount_ = 0u;
  bool rbvhDepthAtlasTextureUploaded_ = false;
  uint64_t gpuBuildAabbsBufferSize_ = 0u;
  uint64_t gpuBuildMortonCodesBufferSize_ = 0u;
  uint64_t gpuBuildPrimitiveIndicesBufferSize_ = 0u;
  uint64_t gpuBuildFlagsBufferSize_ = 0u;
  uint64_t gpuBuildSignalsBufferSize_ = 0u;
  uint64_t gpuBuildHLBVHNodesBufferSize_ = 0u;
  uint64_t gpuCWBVHNodesBufferSize_ = 0u;
  uint64_t gpuLeafRemapBufferSize_ = 0u;
  uint64_t gpuBuildDebugBufferSize_ = 0u;
  uint64_t buildIndirectArgsBufferSize_ = 0u;
  uint64_t countersBufferSize_ = 0u;

  std::unordered_map<std::string, std::vector<rendering::BufferId>> frameOverrideBufferIds_;
  std::unordered_map<std::string, uint32_t> handleBySourcePath_;
  std::unordered_map<uint32_t, BLASRecord> recordsByHandle_;
  std::vector<uint32_t> freeHandles_;
  uint32_t nextHandle_ = 0u;

  std::vector<TriangleBLASMetadataGPU> triangleMetadataGPU_;
  std::vector<RBVHCardBLASMetadataGPU> rbvhCardMetadataGPU_;
  std::vector<virtualgeometry::FallbackMeshVertex> verticesGPU_;
  std::vector<uint32_t> indicesGPU_;
  std::vector<VertexPositionGPU> vertexPositionsGPU_;
  std::vector<CanonicalNodeGPU> canonicalNodesGPU_;
  std::vector<uint32_t> primitiveRemapGPU_;
  std::vector<uint32_t> primitiveMaterialIdsGPU_;
  std::vector<TriangleBaryTransformGPU> triangleTransformsGPU_;
  std::vector<SoftwareRayTraceScene::BVH8NodeGPU> sourceBVH8NodesGPU_;
  std::vector<RBVHCardPrimitive> rbvhCardsGPU_;
  std::vector<uint32_t> rbvhDepthAtlasGPU_;
  std::vector<uint32_t> rbvhCoverageAtlasGPU_;
  std::vector<uint32_t> rbvhDepthMipAtlasGPU_;
  std::vector<CWBVHNodeGPU> gpuCWBVHNodesCPU_;
  std::vector<uint32_t> gpuLeafRemapCPU_;
  BuildSummary lastBuildSummary_{};
};

} // namespace softwarebvh
