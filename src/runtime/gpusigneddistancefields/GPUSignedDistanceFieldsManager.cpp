#include "gpusigneddistancefields/GPUSignedDistanceFieldsManager.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace gpusigneddistancefields
{

namespace
{

using Path = GPUSignedDistanceFieldsManager::Path;

static uint64_t cubeU64(uint32_t v)
{
  return static_cast<uint64_t>(v) * static_cast<uint64_t>(v) * static_cast<uint64_t>(v);
}

static uint32_t divCeilU32(uint32_t value, uint32_t divisor)
{
  return (value + divisor - 1u) / divisor;
}

static uint64_t divCeilU64(uint64_t value, uint64_t divisor)
{
  return (value + divisor - 1u) / divisor;
}

static uint32_t ceilSqrtU32(uint32_t value)
{
  if (value == 0u)
  {
    return 0u;
  }

  uint32_t root = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(value))));
  while (static_cast<uint64_t>(root) * static_cast<uint64_t>(root) < static_cast<uint64_t>(value))
  {
    ++root;
  }
  while (root > 0u && static_cast<uint64_t>(root - 1u) * static_cast<uint64_t>(root - 1u) >= static_cast<uint64_t>(value))
  {
    --root;
  }
  return root;
}

static void clearTextureIfValid(rendering::RenderGraph *renderGraph, const rendering::Texture &texture, const uint64_t byteSize)
{
  if (renderGraph == nullptr || !texture.isValid() || byteSize == 0u)
  {
    return;
  }

  std::vector<uint8_t> zeros(static_cast<size_t>(byteSize), 0u);
  renderGraph->textureWrite(texture, byteSize, zeros.data(), rendering::ResourceLayout::UNDEFINED);
}

} // namespace

bool GPUSignedDistanceFieldsManager::isSurfaceCachePageResolutionValid(const uint32_t pageResolution)
{
  return pageResolution == 0u || pageResolution == 1u || pageResolution == 2u || pageResolution == 4u || pageResolution == 8u;
}

bool GPUSignedDistanceFieldsManager::isSurfaceCacheEnabled(const Settings &settings)
{
  return settings.surfaceCachePageResolution != 0u;
}

GPUSignedDistanceFieldsManager::SurfaceCacheAtlasLayout GPUSignedDistanceFieldsManager::makeSurfaceCacheAtlasLayout(const Settings &settings)
{
  SurfaceCacheAtlasLayout layout{};
  layout.pageResolution = settings.surfaceCachePageResolution;
  if (!isSurfaceCacheEnabled(settings))
  {
    return layout;
  }

  const uint64_t pageCount64 = static_cast<uint64_t>(settings.maxBricks) * SURFACE_CACHE_FACE_COUNT;
  layout.pageCount = static_cast<uint32_t>(pageCount64);
  layout.pagesPerRow = ceilSqrtU32(layout.pageCount);
  const uint64_t pageRows = divCeilU64(pageCount64, std::max<uint32_t>(layout.pagesPerRow, 1u));
  layout.atlasWidth = layout.pagesPerRow * layout.pageResolution;
  layout.atlasHeight = static_cast<uint32_t>(pageRows) * layout.pageResolution;
  layout.texelCount = static_cast<uint64_t>(layout.atlasWidth) * static_cast<uint64_t>(layout.atlasHeight);
  return layout;
}

uint64_t GPUSignedDistanceFieldsManager::surfaceCacheResidentByteSize(const Settings &settings, const uint32_t bytesPerTexel)
{
  if (!isSurfaceCacheEnabled(settings) || bytesPerTexel == 0u)
  {
    return 0u;
  }

  return makeSurfaceCacheAtlasLayout(settings).texelCount * static_cast<uint64_t>(bytesPerTexel);
}

GPUSignedDistanceFieldsManager::GPUSignedDistanceFieldsManager(rendering::RenderGraph *renderGraph) : GPUSignedDistanceFieldsManager(renderGraph, Settings{})
{
}

GPUSignedDistanceFieldsManager::GPUSignedDistanceFieldsManager(rendering::RenderGraph *renderGraph, Settings settings) : renderGraph_(renderGraph), settings_(settings)
{
  if (renderGraph_ == nullptr)
  {
    throw std::runtime_error("GPUSignedDistanceFieldsManager requires a valid RenderGraph");
  }

  if (settings_.cascadeCount == 0u || settings_.atlasBrickCapacityPerAxis == 0u || settings_.maxBricks == 0u || settings_.maxReferences == 0u || settings_.maxBricksPerBake == 0u)
  {
    throw std::runtime_error("GPUSignedDistanceFieldsManager settings must be non-zero");
  }

  if (settings_.firstCascadeVoxelSize <= 0.0f || !std::isfinite(settings_.firstCascadeVoxelSize) || settings_.cascadeVoxelSizeScale < 1.0f || !std::isfinite(settings_.cascadeVoxelSizeScale))
  {
    throw std::runtime_error("GPUSignedDistanceFieldsManager requires finite positive cascade voxel sizes");
  }

  if (!isSurfaceCachePageResolutionValid(settings_.surfaceCachePageResolution))
  {
    throw std::runtime_error("GPUSignedDistanceFieldsManager surface cache page resolution must be 0, 1, 2, 4, or 8");
  }

  if (isSurfaceCacheEnabled(settings_) && static_cast<uint64_t>(settings_.maxBricks) * SURFACE_CACHE_FACE_COUNT > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
  {
    throw std::runtime_error("GPUSignedDistanceFieldsManager surface cache page count exceeds uint32 range");
  }

  if (isSurfaceCacheEnabled(settings_) && (settings_.surfaceCacheDistanceThreshold <= 0.0f || !std::isfinite(settings_.surfaceCacheDistanceThreshold)))
  {
    throw std::runtime_error("GPUSignedDistanceFieldsManager surface cache distance threshold must be finite and positive");
  }

  if (cubeU64(settings_.atlasBrickCapacityPerAxis) < settings_.maxBricks)
  {
    throw std::runtime_error("GPUSignedDistanceFieldsManager maxBricks exceeds atlas brick capacity");
  }

  cascadeInfos_.resize(settings_.cascadeCount);
  cascadeRuntime_.resize(settings_.cascadeCount);
  uploadedStaticJobs_.resize(settings_.cascadeCount);
  uploadedSkinnedJobs_.resize(settings_.cascadeCount);

  initializeSharedResources();
  initializePathResources();
  initializeBufferContents();
}

GPUSignedDistanceFieldsManager::~GPUSignedDistanceFieldsManager()
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

  auto deleteBufferIfValid = [&](const rendering::Buffer &buffer)
  {
    if (buffer.isValid())
    {
      renderGraph_->deleteBuffer(buffer);
    }
  };

  auto deleteTextureIfValid = [&](const rendering::Texture &texture)
  {
    if (texture.isValid())
    {
      renderGraph_->deleteTexture(texture);
    }
  };

  auto deletePathResources = [&](PathResources &resources)
  {
    deleteBufferIfValid(resources.instances);
    deleteBufferIfValid(resources.cascadeBuildInfos);
    deleteBufferIfValid(resources.stageIndirectArgs);
    deleteBufferIfValid(resources.jobSubvoxelOffsetBuffers);
    deleteBufferIfValid(resources.jobBuffers);
    deleteBufferIfValid(resources.aabbTrees);
    deleteBufferIfValid(resources.brickMaps);

    deleteBufferIfValid(resources.scratch.brickClearList);
    deleteBufferIfValid(resources.scratch.brickCompressionList);
    deleteBufferIfValid(resources.scratch.brickStorageOffsets);
    deleteBufferIfValid(resources.scratch.brickStorage);
    deleteBufferIfValid(resources.scratch.stampGlobalScan);
    deleteBufferIfValid(resources.scratch.stampScan);
    deleteBufferIfValid(resources.scratch.referenceGlobalScan);
    deleteBufferIfValid(resources.scratch.referenceCounterScan);
    deleteBufferIfValid(resources.scratch.jobGlobalCountersScan);
    deleteBufferIfValid(resources.scratch.jobCountersScan);
    deleteBufferIfValid(resources.scratch.voxelAllocationFailCounters);
    deleteBufferIfValid(resources.scratch.referenceCounters);
    deleteBufferIfValid(resources.scratch.compactedReferences);
    deleteBufferIfValid(resources.scratch.rawReferences);
    deleteBufferIfValid(resources.scratch.indexSwap);
    deleteBufferIfValid(resources.scratch.jobCounters);
    deleteBufferIfValid(resources.scratch.counters);
  };

  deletePathResources(staticResources_);
  deletePathResources(skinnedResources_);
  deletePathResources(combinedResources_);

  deleteBufferIfValid(sharedResources_.surfaceCacheMaterialReferenceScores);
  deleteTextureIfValid(sharedResources_.surfaceCacheMaterialReferenceBasisAtlas);
  deleteTextureIfValid(sharedResources_.surfaceCacheMaterialReferenceAtlas);
  deleteTextureIfValid(sharedResources_.sdfAtlas);
  deleteBufferIfValid(sharedResources_.cascadeInfos);
  deleteBufferIfValid(sharedResources_.contextIndirectArgs);
  deleteBufferIfValid(sharedResources_.contextCounters);
  deleteBufferIfValid(sharedResources_.brickEikonalCounters);
  deleteBufferIfValid(sharedResources_.brickMergeList);
  deleteBufferIfValid(sharedResources_.brickDirtyList);
  deleteBufferIfValid(sharedResources_.brickClearList);
  deleteBufferIfValid(sharedResources_.brickFreeList);
  deleteBufferIfValid(sharedResources_.brickAABB);
  deleteBufferIfValid(sharedResources_.brickVoxelMap);
  deleteBufferIfValid(sharedResources_.boneMatrices);
  deleteBufferIfValid(sharedResources_.boneWeights);
  deleteBufferIfValid(sharedResources_.indices);
  deleteBufferIfValid(sharedResources_.vertices);
  deleteBufferIfValid(sharedResources_.surfaceMaterials);
  deleteBufferIfValid(sharedResources_.meshTable);
}

void GPUSignedDistanceFieldsManager::initializeSharedResources()
{
  sharedResources_.meshTableSize = static_cast<uint64_t>(settings_.maxMeshes) * sizeof(MeshTableEntryGPU);
  sharedResources_.surfaceMaterialsSize = static_cast<uint64_t>(settings_.maxMeshes) * sizeof(SurfaceMaterialGPU);
  sharedResources_.verticesSize = static_cast<uint64_t>(settings_.maxVertices) * sizeof(VertexGPU);
  sharedResources_.indicesSize = static_cast<uint64_t>(settings_.maxIndices) * sizeof(uint32_t);
  sharedResources_.boneWeightsSize = static_cast<uint64_t>(settings_.maxBoneWeights) * sizeof(BoneWeightGPU);
  sharedResources_.boneMatricesSize = static_cast<uint64_t>(settings_.maxBoneMatrices) * sizeof(math::Mat4f);
  sharedResources_.brickVoxelMapSize = static_cast<uint64_t>(settings_.maxBricks) * sizeof(uint32_t);
  sharedResources_.brickAABBSize = static_cast<uint64_t>(settings_.maxBricks) * sizeof(uint32_t);
  sharedResources_.brickFreeListSize = static_cast<uint64_t>(settings_.maxBricks) * sizeof(uint32_t);
  sharedResources_.brickClearListSize = static_cast<uint64_t>(settings_.maxBricks) * sizeof(uint32_t);
  sharedResources_.brickDirtyListSize = static_cast<uint64_t>(settings_.maxBricks) * sizeof(uint32_t);
  sharedResources_.brickMergeListSize = static_cast<uint64_t>(settings_.maxBricks) * sizeof(uint32_t) * 2u;
  sharedResources_.brickEikonalCountersSize = static_cast<uint64_t>(settings_.maxBricks) * sizeof(uint32_t);
  sharedResources_.contextCountersSize = static_cast<uint64_t>(ContextCounter_Count) * sizeof(uint32_t);
  sharedResources_.contextIndirectArgsSize = IndirectOffset_Count;
  sharedResources_.cascadeInfosSize = static_cast<uint64_t>(settings_.cascadeCount) * sizeof(CascadeInfoGPU);
  sharedResources_.surfaceCacheLayout = makeSurfaceCacheAtlasLayout(settings_);
  sharedResources_.surfaceCacheMaterialReferenceAtlasSize = surfaceCacheResidentByteSize(settings_, 16u);
  sharedResources_.surfaceCacheMaterialReferenceBasisAtlasSize = surfaceCacheResidentByteSize(settings_, 16u);
  sharedResources_.surfaceCacheMaterialReferenceScoresSize = std::max<uint64_t>(sizeof(uint32_t), surfaceCacheResidentByteSize(settings_, 4u));

  const uint32_t atlasResolution = settings_.atlasBrickCapacityPerAxis * BRICK_RESOLUTION;

  sharedResources_.meshTable = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFields_MeshTable.buffer",
          .size = sharedResources_.meshTableSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  sharedResources_.surfaceMaterials = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFields_SurfaceMaterials.buffer",
          .size = sharedResources_.surfaceMaterialsSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  sharedResources_.vertices = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFields_Vertices.buffer",
          .size = sharedResources_.verticesSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  sharedResources_.indices = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFields_Indices.buffer",
          .size = sharedResources_.indicesSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  sharedResources_.boneWeights = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFields_BoneWeights.buffer",
          .size = sharedResources_.boneWeightsSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  sharedResources_.boneMatrices = createManagedBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFields_BoneMatrices.buffer",
          .size = sharedResources_.boneMatricesSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      },
      true);
  sharedResources_.brickVoxelMap = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFields_BrickVoxelMap.buffer",
          .size = sharedResources_.brickVoxelMapSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  sharedResources_.brickAABB = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFields_BrickAABB.buffer",
          .size = sharedResources_.brickAABBSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  sharedResources_.brickFreeList = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFields_BrickFreeList.buffer",
          .size = sharedResources_.brickFreeListSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  sharedResources_.brickClearList = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFields_BrickClearList.buffer",
          .size = sharedResources_.brickClearListSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  sharedResources_.brickDirtyList = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFields_BrickDirtyList.buffer",
          .size = sharedResources_.brickDirtyListSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  sharedResources_.brickMergeList = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFields_BrickMergeList.buffer",
          .size = sharedResources_.brickMergeListSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  sharedResources_.brickEikonalCounters = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFields_BrickEikonalCounters.buffer",
          .size = sharedResources_.brickEikonalCountersSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  sharedResources_.contextCounters = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFields_ContextCounters.buffer",
          .size = sharedResources_.contextCountersSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  sharedResources_.contextIndirectArgs = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFields_ContextIndirectArgs.buffer",
          .size = sharedResources_.contextIndirectArgsSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  sharedResources_.cascadeInfos = createManagedBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFields_CascadeInfos.buffer",
          .size = sharedResources_.cascadeInfosSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      },
      true);
  sharedResources_.sdfAtlas = renderGraph_->createTexture(
      rendering::TextureInfo{
          .name = "GPUSignedDistanceFields_SdfAtlas.texture",
          .format = rendering::Format::Format_R32Float,
          .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
          .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
          .width = atlasResolution,
          .height = atlasResolution,
          .depth = atlasResolution,
          .mipLevels = 1u,
      });

  if (!isSurfaceCacheEnabled(settings_))
  {
    sharedResources_.surfaceCacheMaterialReferenceAtlas = renderGraph_->createTexture(
        rendering::TextureInfo{
            .name = "GPUSignedDistanceFields_SurfaceCacheMaterialReferenceAtlasFallback.texture",
            .format = rendering::Format::Format_RGBA32Uint,
            .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
            .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
            .width = 1u,
            .height = 1u,
            .depth = 1u,
            .mipLevels = 1u,
        });
    sharedResources_.surfaceCacheMaterialReferenceBasisAtlas = renderGraph_->createTexture(
        rendering::TextureInfo{
            .name = "GPUSignedDistanceFields_SurfaceCacheMaterialReferenceBasisAtlasFallback.texture",
            .format = rendering::Format::Format_RGBA32Uint,
            .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
            .usage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled,
            .width = 1u,
            .height = 1u,
            .depth = 1u,
            .mipLevels = 1u,
        });
  }

  if (isSurfaceCacheEnabled(settings_))
  {
    const SurfaceCacheAtlasLayout &layout = sharedResources_.surfaceCacheLayout;
    const rendering::ImageUsage surfaceCacheUsage = rendering::ImageUsage::ImageUsage_Storage | rendering::ImageUsage::ImageUsage_Sampled;
    sharedResources_.surfaceCacheMaterialReferenceAtlas = renderGraph_->createTexture(
        rendering::TextureInfo{
            .name = "GPUSignedDistanceFields_SurfaceCacheMaterialReferenceAtlas.texture",
            .format = rendering::Format::Format_RGBA32Uint,
            .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
            .usage = surfaceCacheUsage,
            .width = layout.atlasWidth,
            .height = layout.atlasHeight,
            .depth = 1u,
            .mipLevels = 1u,
        });
    sharedResources_.surfaceCacheMaterialReferenceBasisAtlas = renderGraph_->createTexture(
        rendering::TextureInfo{
            .name = "GPUSignedDistanceFields_SurfaceCacheMaterialReferenceBasisAtlas.texture",
            .format = rendering::Format::Format_RGBA32Uint,
            .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
            .usage = surfaceCacheUsage,
            .width = layout.atlasWidth,
            .height = layout.atlasHeight,
            .depth = 1u,
            .mipLevels = 1u,
        });
    clearTextureIfValid(renderGraph_, sharedResources_.surfaceCacheMaterialReferenceAtlas, sharedResources_.surfaceCacheMaterialReferenceAtlasSize);
    clearTextureIfValid(renderGraph_, sharedResources_.surfaceCacheMaterialReferenceBasisAtlas, sharedResources_.surfaceCacheMaterialReferenceBasisAtlasSize);
  }

  sharedResources_.surfaceCacheMaterialReferenceScores = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = "GPUSignedDistanceFields_SurfaceCacheMaterialReferenceScores.buffer",
          .size = sharedResources_.surfaceCacheMaterialReferenceScoresSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push,
      });
}

void GPUSignedDistanceFieldsManager::initializePathResources()
{
  initializePathResource(Path::Static);
  initializePathResource(Path::Skinned);
  initializePathResource(Path::Combined);
}

void GPUSignedDistanceFieldsManager::initializePathResource(Path path)
{
  PathResources *resources = nullptr;
  switch (path)
  {
  case Path::Static:
    resources = &staticResources_;
    break;
  case Path::Skinned:
    resources = &skinnedResources_;
    break;
  case Path::Combined:
    resources = &combinedResources_;
    break;
  default:
    throw std::runtime_error("Unknown GPUSignedDistanceFields path");
  }

  resources->instancesSize = static_cast<uint64_t>(settings_.maxInstancesPerPath) * sizeof(InstanceGPU);
  resources->instanceCount = 0u;
  resources->activeInstanceCount = 0u;
  resources->cascades.resize(settings_.cascadeCount);
  resources->cascadeBuildInfosSize = static_cast<uint64_t>(settings_.cascadeCount) * sizeof(CascadeBuildInfoGPU);
  resources->stageIndirectArgsSize = IndirectOffset_Count;
  resources->brickMapsSize = static_cast<uint64_t>(settings_.cascadeCount) * CASCADE_VOXEL_COUNT * sizeof(uint32_t);
  resources->aabbTreesSize = static_cast<uint64_t>(settings_.cascadeCount) * BUILD_TREE_FLOAT4_COUNT * sizeof(float) * 4u;

  resources->brickMaps = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_BrickMaps.buffer",
          .size = resources->brickMapsSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });
  resources->aabbTrees = renderGraph_->createBuffer(
      rendering::BufferInfo{
          .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_AABBTrees.buffer",
          .size = resources->aabbTreesSize,
          .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
      });

  if (path != Path::Combined)
  {
    resources->cascadeBuildInfos = createManagedBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_CascadeBuildInfos.buffer",
            .size = resources->cascadeBuildInfosSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        },
        true);
    resources->stageIndirectArgs = createManagedBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_StageIndirectArgs.buffer",
            .size = resources->stageIndirectArgsSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        },
        true);
    resources->scratch.indirectArgs = resources->stageIndirectArgs;
    resources->scratch.indirectArgsSize = resources->stageIndirectArgsSize;
  }

  if (path != Path::Combined)
  {
    resources->instances = createManagedBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_Instances.buffer",
            .size = resources->instancesSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        },
        true);

    const uint32_t maxTriangleSwapWords = settings_.maxTriangleSwapWords != 0u ? settings_.maxTriangleSwapWords : (settings_.maxReferences * 9u);
    const uint32_t maxJobScanGroups = std::max<uint32_t>(1u, divCeilU32(settings_.maxJobsPerCascade, SCAN_GROUP_SIZE));
    const uint32_t maxVoxelScanGroups = std::max<uint32_t>(1u, divCeilU32(CASCADE_VOXEL_COUNT, SCAN_GROUP_SIZE));
    resources->jobBuffersSize = static_cast<uint64_t>(settings_.cascadeCount) * settings_.maxJobsPerCascade * sizeof(JobGPU);
    resources->jobSubvoxelOffsetBuffersSize = static_cast<uint64_t>(settings_.cascadeCount) * settings_.maxJobsPerCascade * sizeof(uint32_t);

    resources->jobBuffers = createManagedBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_Jobs.buffer",
            .size = resources->jobBuffersSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        },
        true);
    resources->jobSubvoxelOffsetBuffers = createManagedBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_JobSubvoxelOffsets.buffer",
            .size = resources->jobSubvoxelOffsetBuffersSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        },
        true);

    resources->scratch.countersSize = static_cast<uint64_t>(settings_.cascadeCount) * ScratchCounter_Count * sizeof(uint32_t);
    resources->scratch.jobCountersSize = static_cast<uint64_t>(settings_.cascadeCount) * settings_.maxJobsPerCascade * sizeof(uint32_t);
    resources->scratch.indexSwapSize = static_cast<uint64_t>(settings_.cascadeCount) * maxTriangleSwapWords * sizeof(uint32_t);
    resources->scratch.rawReferencesSize = static_cast<uint64_t>(settings_.cascadeCount) * settings_.maxReferences * sizeof(RawTriangleReferenceGPU);
    resources->scratch.compactedReferencesSize = static_cast<uint64_t>(settings_.cascadeCount) * settings_.maxReferences * sizeof(uint32_t);
    resources->scratch.referenceCountersSize = static_cast<uint64_t>(settings_.cascadeCount) * CASCADE_VOXEL_COUNT * sizeof(uint32_t);
    resources->scratch.voxelAllocationFailCountersSize = static_cast<uint64_t>(settings_.cascadeCount) * CASCADE_VOXEL_COUNT * sizeof(uint32_t);
    resources->scratch.jobCountersScanSize = static_cast<uint64_t>(settings_.cascadeCount) * settings_.maxJobsPerCascade * sizeof(uint32_t);
    resources->scratch.jobGlobalCountersScanSize = static_cast<uint64_t>(settings_.cascadeCount) * maxJobScanGroups * sizeof(uint32_t);
    resources->scratch.referenceCounterScanSize = static_cast<uint64_t>(settings_.cascadeCount) * CASCADE_VOXEL_COUNT * sizeof(uint32_t);
    resources->scratch.referenceGlobalScanSize = static_cast<uint64_t>(settings_.cascadeCount) * maxVoxelScanGroups * sizeof(uint32_t);
    resources->scratch.stampScanSize = static_cast<uint64_t>(settings_.cascadeCount) * CASCADE_VOXEL_COUNT * sizeof(uint32_t);
    resources->scratch.stampGlobalScanSize = static_cast<uint64_t>(settings_.cascadeCount) * maxVoxelScanGroups * sizeof(uint32_t);
    resources->scratch.brickStorageSize = static_cast<uint64_t>(settings_.cascadeCount) * settings_.maxBricksPerBake * BRICK_CELL_COUNT * sizeof(uint32_t);
    resources->scratch.brickStorageOffsetsSize = static_cast<uint64_t>(settings_.cascadeCount) * settings_.maxBricks * sizeof(uint32_t);
    resources->scratch.brickCompressionListSize = static_cast<uint64_t>(settings_.cascadeCount) * settings_.maxBricksPerBake * sizeof(uint32_t);
    resources->scratch.brickClearListSize = static_cast<uint64_t>(settings_.cascadeCount) * settings_.maxBricksPerBake * sizeof(uint32_t);
    resources->scratch.counters = renderGraph_->createBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_ScratchCounters.buffer",
            .size = resources->scratch.countersSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    resources->scratch.jobCounters = renderGraph_->createBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_ScratchJobCounters.buffer",
            .size = resources->scratch.jobCountersSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    resources->scratch.indexSwap = renderGraph_->createBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_ScratchIndexSwap.buffer",
            .size = resources->scratch.indexSwapSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    resources->scratch.rawReferences = renderGraph_->createBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_ScratchRawReferences.buffer",
            .size = resources->scratch.rawReferencesSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_CopySrc | rendering::BufferUsage::BufferUsage_CopyDst,
        });
    resources->scratch.compactedReferences = renderGraph_->createBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_ScratchCompactedReferences.buffer",
            .size = resources->scratch.compactedReferencesSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_CopySrc | rendering::BufferUsage::BufferUsage_CopyDst,
        });
    resources->scratch.referenceCounters = renderGraph_->createBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_ScratchReferenceCounters.buffer",
            .size = resources->scratch.referenceCountersSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    resources->scratch.voxelAllocationFailCounters = renderGraph_->createBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_ScratchVoxelAllocationFailCounters.buffer",
            .size = resources->scratch.voxelAllocationFailCountersSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    resources->scratch.jobCountersScan = renderGraph_->createBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_ScratchJobCountersScan.buffer",
            .size = resources->scratch.jobCountersScanSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    resources->scratch.jobGlobalCountersScan = renderGraph_->createBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_ScratchJobGlobalCountersScan.buffer",
            .size = resources->scratch.jobGlobalCountersScanSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    resources->scratch.referenceCounterScan = renderGraph_->createBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_ScratchReferenceCounterScan.buffer",
            .size = resources->scratch.referenceCounterScanSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    resources->scratch.referenceGlobalScan = renderGraph_->createBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_ScratchReferenceGlobalScan.buffer",
            .size = resources->scratch.referenceGlobalScanSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    resources->scratch.stampScan = renderGraph_->createBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_ScratchStampScan.buffer",
            .size = resources->scratch.stampScanSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    resources->scratch.stampGlobalScan = renderGraph_->createBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_ScratchStampGlobalScan.buffer",
            .size = resources->scratch.stampGlobalScanSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    resources->scratch.brickStorage = renderGraph_->createBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_ScratchBrickStorage.buffer",
            .size = resources->scratch.brickStorageSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    resources->scratch.brickStorageOffsets = renderGraph_->createBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_ScratchBrickStorageOffsets.buffer",
            .size = resources->scratch.brickStorageOffsetsSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    resources->scratch.brickCompressionList = renderGraph_->createBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_ScratchBrickCompressionList.buffer",
            .size = resources->scratch.brickCompressionListSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
    resources->scratch.brickClearList = renderGraph_->createBuffer(
        rendering::BufferInfo{
            .name = std::string("GPUSignedDistanceFields_") + pathName(path) + "_ScratchBrickClearList.buffer",
            .size = resources->scratch.brickClearListSize,
            .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Push | rendering::BufferUsage::BufferUsage_CopySrc,
        });
  }

  for (uint32_t cascadeIndex = 0u; cascadeIndex < settings_.cascadeCount; ++cascadeIndex)
  {
    CascadeResources &cascade = resources->cascades[cascadeIndex];
    cascade.brickMapSize = static_cast<uint64_t>(CASCADE_VOXEL_COUNT) * sizeof(uint32_t);
    cascade.aabbTreeSize = static_cast<uint64_t>(BUILD_TREE_FLOAT4_COUNT) * sizeof(float) * 4u;
    cascade.jobBufferSize = static_cast<uint64_t>(settings_.maxJobsPerCascade) * sizeof(JobGPU);
    cascade.jobSubvoxelOffsetBufferSize = static_cast<uint64_t>(settings_.maxJobsPerCascade) * sizeof(uint32_t);
    cascade.brickMapOffset = static_cast<uint64_t>(cascadeIndex) * cascade.brickMapSize;
    cascade.aabbTreeOffset = static_cast<uint64_t>(cascadeIndex) * cascade.aabbTreeSize;
    cascade.jobBufferOffset = static_cast<uint64_t>(cascadeIndex) * cascade.jobBufferSize;
    cascade.jobSubvoxelOffsetBufferOffset = static_cast<uint64_t>(cascadeIndex) * cascade.jobSubvoxelOffsetBufferSize;
    cascade.brickMap = resources->brickMaps;
    cascade.aabbTree = resources->aabbTrees;

    if (path != Path::Combined)
    {
      cascade.jobBuffer = resources->jobBuffers;
      cascade.jobSubvoxelOffsetBuffer = resources->jobSubvoxelOffsetBuffers;
    }
  }
}

rendering::Buffer GPUSignedDistanceFieldsManager::createManagedBuffer(rendering::BufferInfo info, const bool frameLocal)
{
  const rendering::Buffer buffer = renderGraph_->createBuffer(info);
  if (frameLocal)
  {
    initializeFrameOverrideBuffer(buffer, info.size, info.usage);
  }
  return buffer;
}

void GPUSignedDistanceFieldsManager::initializeFrameOverrideBuffer(
    const rendering::Buffer &buffer,
    const uint64_t size,
    const rendering::BufferUsage usage)
{
  if (renderGraph_ == nullptr)
  {
    return;
  }

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

rendering::BufferId GPUSignedDistanceFieldsManager::getFrameOverrideBufferId(const rendering::Buffer &buffer, const uint32_t frameSlot) const
{
  const auto it = frameOverrideBufferIds_.find(buffer.name);
  if (it == frameOverrideBufferIds_.end() || it->second.empty())
  {
    return rendering::BufferId::Invalid;
  }

  return it->second[frameSlot % it->second.size()];
}

void GPUSignedDistanceFieldsManager::writeManagedBuffer(
    const rendering::Buffer &buffer,
    const uint64_t offset,
    const uint64_t size,
    const void *data) const
{
  if (renderGraph_ == nullptr || !buffer.isValid() || data == nullptr || size == 0u)
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

void GPUSignedDistanceFieldsManager::appendFrameOverrides(rendering::RenderGraph::Overrides &overrides, const uint32_t frameSlot) const
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

void GPUSignedDistanceFieldsManager::initializeBufferContents()
{
  uploadSurfaceMaterials(std::vector<SurfaceMaterialGPU>{SurfaceMaterialGPU{}});
  uploadUIntBufferFill(sharedResources_.brickVoxelMap, sharedResources_.brickVoxelMapSize, INVALID_ID);
  uploadUIntBufferFill(sharedResources_.brickAABB, sharedResources_.brickAABBSize, 0u);
  uploadUIntBufferFill(sharedResources_.brickFreeList, sharedResources_.brickFreeListSize, 0u);
  uploadUIntBufferFill(sharedResources_.brickClearList, sharedResources_.brickClearListSize, 0u);
  uploadUIntBufferFill(sharedResources_.brickDirtyList, sharedResources_.brickDirtyListSize, 0u);
  uploadUIntBufferFill(sharedResources_.brickMergeList, sharedResources_.brickMergeListSize, 0u);
  uploadUIntBufferFill(sharedResources_.brickEikonalCounters, sharedResources_.brickEikonalCountersSize, 0u);
  uploadUIntBufferFill(sharedResources_.contextCounters, sharedResources_.contextCountersSize, 0u);
  uploadUIntBufferFill(sharedResources_.contextIndirectArgs, sharedResources_.contextIndirectArgsSize, 0u);
  uploadCascadeInfos();

  auto initializePath = [&](Path path, PathResources &resources, bool hasBuildResources)
  {
    if (hasBuildResources)
    {
      uploadUIntBufferFill(resources.instances, resources.instancesSize, 0u);
      uploadUIntBufferFill(resources.cascadeBuildInfos, resources.cascadeBuildInfosSize, 0u);
      uploadUIntBufferFill(resources.stageIndirectArgs, resources.stageIndirectArgsSize, 0u);
      uploadUIntBufferFill(resources.scratch.counters, resources.scratch.countersSize, 0u);
      uploadUIntBufferFill(resources.scratch.jobCounters, resources.scratch.jobCountersSize, 0u);
      uploadUIntBufferFill(resources.scratch.indexSwap, resources.scratch.indexSwapSize, 0u);
      uploadUIntBufferFill(resources.scratch.referenceCounters, resources.scratch.referenceCountersSize, 0u);
      uploadUIntBufferFill(resources.scratch.voxelAllocationFailCounters, resources.scratch.voxelAllocationFailCountersSize, 0u);
      uploadUIntBufferFill(resources.scratch.jobCountersScan, resources.scratch.jobCountersScanSize, 0u);
      uploadUIntBufferFill(resources.scratch.jobGlobalCountersScan, resources.scratch.jobGlobalCountersScanSize, 0u);
      uploadUIntBufferFill(resources.scratch.referenceCounterScan, resources.scratch.referenceCounterScanSize, 0u);
      uploadUIntBufferFill(resources.scratch.referenceGlobalScan, resources.scratch.referenceGlobalScanSize, 0u);
      uploadUIntBufferFill(resources.scratch.stampScan, resources.scratch.stampScanSize, 0u);
      uploadUIntBufferFill(resources.scratch.stampGlobalScan, resources.scratch.stampGlobalScanSize, 0u);
      uploadUIntBufferFill(resources.scratch.brickStorage, resources.scratch.brickStorageSize, 0u);
      uploadUIntBufferFill(resources.scratch.brickStorageOffsets, resources.scratch.brickStorageOffsetsSize, 0u);
      uploadUIntBufferFill(resources.scratch.brickCompressionList, resources.scratch.brickCompressionListSize, 0u);
      uploadUIntBufferFill(resources.scratch.brickClearList, resources.scratch.brickClearListSize, 0u);
      uploadUIntBufferFill(resources.jobBuffers, resources.jobBuffersSize, 0u);
      uploadUIntBufferFill(resources.jobSubvoxelOffsetBuffers, resources.jobSubvoxelOffsetBuffersSize, 0u);
    }

    uploadUIntBufferFill(resources.brickMaps, resources.brickMapsSize, UNINITIALIZED_ID);
    uploadUIntBufferFill(resources.aabbTrees, resources.aabbTreesSize, 0u);

    for (uint32_t cascadeIndex = 0u; cascadeIndex < static_cast<uint32_t>(resources.cascades.size()); ++cascadeIndex)
    {
      if (hasBuildResources)
        uploadCascadeBuildInfo(path, cascadeIndex);
    }
  };

  initializePath(Path::Static, staticResources_, true);
  initializePath(Path::Skinned, skinnedResources_, true);
  initializePath(Path::Combined, combinedResources_, false);
}

void GPUSignedDistanceFieldsManager::updateClipmaps(const math::Vec3f &center)
{
  const float halfCascade = static_cast<float>(CASCADE_RESOLUTION) * 0.5f;

  for (uint32_t cascadeIndex = 0u; cascadeIndex < settings_.cascadeCount; ++cascadeIndex)
  {
    CascadeRuntime &runtime = cascadeRuntime_[cascadeIndex];
    CascadeInfoGPU &cascade = cascadeInfos_[cascadeIndex];

    const float voxelSize = settings_.firstCascadeVoxelSize * std::pow(settings_.cascadeVoxelSizeScale, static_cast<float>(cascadeIndex));
    const int32_t quantizedGridMinX = static_cast<int32_t>(std::floor(center[0] / voxelSize - halfCascade));
    const int32_t quantizedGridMinY = static_cast<int32_t>(std::floor(center[1] / voxelSize - halfCascade));
    const int32_t quantizedGridMinZ = static_cast<int32_t>(std::floor(center[2] / voxelSize - halfCascade));

    const int32_t deltaX = runtime.hasPreviousQuantizedMin ? (quantizedGridMinX - runtime.quantizedGridMin[0]) : 0;
    const int32_t deltaY = runtime.hasPreviousQuantizedMin ? (quantizedGridMinY - runtime.quantizedGridMin[1]) : 0;
    const int32_t deltaZ = runtime.hasPreviousQuantizedMin ? (quantizedGridMinZ - runtime.quantizedGridMin[2]) : 0;

    runtime.clipmapOffset[0] = positiveModulo(static_cast<int32_t>(runtime.clipmapOffset[0]) + deltaX, CASCADE_RESOLUTION);
    runtime.clipmapOffset[1] = positiveModulo(static_cast<int32_t>(runtime.clipmapOffset[1]) + deltaY, CASCADE_RESOLUTION);
    runtime.clipmapOffset[2] = positiveModulo(static_cast<int32_t>(runtime.clipmapOffset[2]) + deltaZ, CASCADE_RESOLUTION);
    runtime.quantizedGridMin[0] = quantizedGridMinX;
    runtime.quantizedGridMin[1] = quantizedGridMinY;
    runtime.quantizedGridMin[2] = quantizedGridMinZ;
    runtime.hasPreviousQuantizedMin = true;

    const math::Vec3f gridMin(
        static_cast<float>(quantizedGridMinX) * voxelSize,
        static_cast<float>(quantizedGridMinY) * voxelSize,
        static_cast<float>(quantizedGridMinZ) * voxelSize);
    const float cascadeExtent = voxelSize * static_cast<float>(CASCADE_RESOLUTION);
    const math::Vec3f gridMax = gridMin + math::Vec3f(cascadeExtent, cascadeExtent, cascadeExtent);

    cascade.gridMin = gridMin;
    cascade.gridMax = gridMax;
    cascade.gridMid = (gridMin + gridMax) * 0.5f;
    cascade.voxelSize = voxelSize;
    cascade.iVoxelSize = 1.0f / voxelSize;
    cascade.clipmapOffset[0] = runtime.clipmapOffset[0];
    cascade.clipmapOffset[1] = runtime.clipmapOffset[1];
    cascade.clipmapOffset[2] = runtime.clipmapOffset[2];
    cascade.clipmapInvalidationOffset[0] = deltaX;
    cascade.clipmapInvalidationOffset[1] = deltaY;
    cascade.clipmapInvalidationOffset[2] = deltaZ;
    cascade.ioffset[0] = quantizedGridMinX;
    cascade.ioffset[1] = quantizedGridMinY;
    cascade.ioffset[2] = quantizedGridMinZ;
    cascade.index = cascadeIndex;
    cascade.flags = 0u;
    cascade.isEnabled = 1u;
    cascade.isInitialized = runtime.isInitialized ? 1u : 0u;
  }

  uploadCascadeInfos();
}

void GPUSignedDistanceFieldsManager::markCascadeInitialized(uint32_t cascadeIndex)
{
  if (cascadeIndex >= cascadeRuntime_.size())
  {
    return;
  }

  cascadeRuntime_[cascadeIndex].isInitialized = true;
  cascadeInfos_[cascadeIndex].isInitialized = 1u;
  uploadCascadeInfos();
}

void GPUSignedDistanceFieldsManager::markAllCascadesUninitialized()
{
  for (uint32_t i = 0u; i < static_cast<uint32_t>(cascadeRuntime_.size()); ++i)
  {
    cascadeRuntime_[i].isInitialized = false;
    cascadeInfos_[i].isInitialized = 0u;
  }
  // Reset the context so CollectClearBricksCS re-runs in initialization mode,
  // clearing ContextBrickVoxelMap for all bricks and rebuilding the free list.
  // Without this, ContextBrickVoxelMap still has stale voxel references from the
  // previous build, so CollectClearBricksCS sees no free bricks and FreeBricks=0.
  needsContextInitialization_ = true;
  uploadCascadeInfos();
}

void GPUSignedDistanceFieldsManager::markContextInitialized()
{
  needsContextInitialization_ = false;
}

void GPUSignedDistanceFieldsManager::uploadMeshTable(const std::vector<MeshTableEntryGPU> &meshTable)
{
  const uint64_t uploadSize = static_cast<uint64_t>(meshTable.size()) * sizeof(MeshTableEntryGPU);
  if (uploadSize > sharedResources_.meshTableSize)
  {
    throw std::runtime_error("GPUSignedDistanceFields mesh table upload exceeds configured capacity");
  }
  uploadedMeshTable_ = meshTable;
  uploadWholeBuffer(sharedResources_.meshTable, sharedResources_.meshTableSize, meshTable.data(), uploadSize);
}

void GPUSignedDistanceFieldsManager::uploadSurfaceMaterials(const std::vector<SurfaceMaterialGPU> &materials)
{
  const uint64_t uploadSize = static_cast<uint64_t>(materials.size()) * sizeof(SurfaceMaterialGPU);
  if (uploadSize > sharedResources_.surfaceMaterialsSize)
  {
    throw std::runtime_error("GPUSignedDistanceFields surface material upload exceeds configured capacity");
  }
  uploadedSurfaceMaterials_ = materials;
  uploadWholeBuffer(sharedResources_.surfaceMaterials, sharedResources_.surfaceMaterialsSize, materials.data(), uploadSize);
}

void GPUSignedDistanceFieldsManager::uploadVertices(const std::vector<VertexGPU> &vertices)
{
  const uint64_t uploadSize = static_cast<uint64_t>(vertices.size()) * sizeof(VertexGPU);
  if (uploadSize > sharedResources_.verticesSize)
  {
    throw std::runtime_error("GPUSignedDistanceFields vertex upload exceeds configured capacity");
  }
  uploadedVertices_ = vertices;
  uploadWholeBuffer(sharedResources_.vertices, sharedResources_.verticesSize, vertices.data(), uploadSize);
}

void GPUSignedDistanceFieldsManager::uploadIndices(const std::vector<uint32_t> &indices)
{
  const uint64_t uploadSize = static_cast<uint64_t>(indices.size()) * sizeof(uint32_t);
  if (uploadSize > sharedResources_.indicesSize)
  {
    throw std::runtime_error("GPUSignedDistanceFields index upload exceeds configured capacity");
  }
  uploadedIndices_ = indices;
  uploadWholeBuffer(sharedResources_.indices, sharedResources_.indicesSize, indices.data(), uploadSize);
}

void GPUSignedDistanceFieldsManager::uploadBoneWeights(const std::vector<BoneWeightGPU> &boneWeights)
{
  const uint64_t uploadSize = static_cast<uint64_t>(boneWeights.size()) * sizeof(BoneWeightGPU);
  if (uploadSize > sharedResources_.boneWeightsSize)
  {
    throw std::runtime_error("GPUSignedDistanceFields bone-weight upload exceeds configured capacity");
  }
  uploadedBoneWeights_ = boneWeights;
  uploadWholeBuffer(sharedResources_.boneWeights, sharedResources_.boneWeightsSize, boneWeights.data(), uploadSize);
}

void GPUSignedDistanceFieldsManager::uploadBoneMatrices(const std::vector<math::Mat4f> &boneMatrices)
{
  const uint64_t uploadSize = static_cast<uint64_t>(boneMatrices.size()) * sizeof(math::Mat4f);
  if (uploadSize > sharedResources_.boneMatricesSize)
  {
    throw std::runtime_error("GPUSignedDistanceFields bone-matrix upload exceeds configured capacity");
  }
  uploadedBoneMatrices_ = boneMatrices;
  uploadWholeBuffer(sharedResources_.boneMatrices, sharedResources_.boneMatricesSize, boneMatrices.data(), uploadSize);
}

void GPUSignedDistanceFieldsManager::uploadInstanceSlice(Path path, uint32_t index, const InstanceGPU &instance)
{
  if (path == Path::Combined)
  {
    throw std::runtime_error("Combined GPUSignedDistanceFields path does not accept instance uploads");
  }

  PathResources &resources = (path == Path::Static) ? staticResources_ : skinnedResources_;
  const uint64_t byteOffset = static_cast<uint64_t>(index) * sizeof(InstanceGPU);
  if (byteOffset + sizeof(InstanceGPU) > resources.instancesSize)
  {
    throw std::runtime_error("GPUSignedDistanceFields instance slice upload out of range");
  }

  renderGraph_->bufferWrite(resources.instances, byteOffset, sizeof(InstanceGPU), const_cast<InstanceGPU *>(&instance));

  auto &uploaded = (path == Path::Static) ? uploadedStaticInstances_ : uploadedSkinnedInstances_;
  if (index >= static_cast<uint32_t>(uploaded.size()))
  {
    uploaded.resize(index + 1u);
  }
  uploaded[index] = instance;
}

void GPUSignedDistanceFieldsManager::uploadBoneMatricesSlice(uint32_t startIndex, uint32_t count, const math::Mat4f *matrices)
{
  if (count == 0u)
  {
    return;
  }

  const uint64_t byteOffset = static_cast<uint64_t>(startIndex) * sizeof(math::Mat4f);
  const uint64_t byteSize = static_cast<uint64_t>(count) * sizeof(math::Mat4f);
  if (byteOffset + byteSize > sharedResources_.boneMatricesSize)
  {
    throw std::runtime_error("GPUSignedDistanceFields bone matrix slice upload out of range");
  }

  renderGraph_->bufferWrite(sharedResources_.boneMatrices, byteOffset, byteSize, const_cast<math::Mat4f *>(matrices));

  if (startIndex + count > static_cast<uint32_t>(uploadedBoneMatrices_.size()))
  {
    uploadedBoneMatrices_.resize(startIndex + count);
  }
  std::copy(matrices, matrices + count, uploadedBoneMatrices_.data() + startIndex);
}

void GPUSignedDistanceFieldsManager::setInstanceCount(Path path, uint32_t count)
{
  if (path == Path::Combined)
  {
    throw std::runtime_error("Combined GPUSignedDistanceFields path does not accept instance count updates");
  }

  PathResources &resources = (path == Path::Static) ? staticResources_ : skinnedResources_;
  resources.instanceCount = count;
}

void GPUSignedDistanceFieldsManager::setActiveInstanceCount(Path path, uint32_t count)
{
  if (path == Path::Combined)
  {
    throw std::runtime_error("Combined GPUSignedDistanceFields path does not accept active instance count updates");
  }

  PathResources &resources = (path == Path::Static) ? staticResources_ : skinnedResources_;
  resources.activeInstanceCount = count;
}

void GPUSignedDistanceFieldsManager::uploadInstances(Path path, const std::vector<InstanceGPU> &instances)
{
  if (path == Path::Combined)
  {
    throw std::runtime_error("Combined GPUSignedDistanceFields path does not accept instance uploads");
  }

  PathResources &resources = (path == Path::Static) ? staticResources_ : skinnedResources_;
  const uint64_t uploadSize = static_cast<uint64_t>(instances.size()) * sizeof(InstanceGPU);
  if (uploadSize > resources.instancesSize)
  {
    throw std::runtime_error("GPUSignedDistanceFields instance upload exceeds configured capacity");
  }

  resources.instanceCount = static_cast<uint32_t>(instances.size());
  resources.activeInstanceCount = static_cast<uint32_t>(instances.size());
  if (path == Path::Static)
  {
    uploadedStaticInstances_ = instances;
  }
  else
  {
    uploadedSkinnedInstances_ = instances;
  }
  uploadWholeBuffer(resources.instances, resources.instancesSize, instances.data(), uploadSize);
}

void GPUSignedDistanceFieldsManager::uploadJobs(Path path, uint32_t cascadeIndex, const std::vector<JobGPU> &jobs)
{
  if (path == Path::Combined)
  {
    throw std::runtime_error("Combined GPUSignedDistanceFields path does not accept job uploads");
  }

  if (cascadeIndex >= settings_.cascadeCount)
  {
    throw std::runtime_error("GPUSignedDistanceFields job upload targets an invalid cascade");
  }

  PathResources &resources = (path == Path::Static) ? staticResources_ : skinnedResources_;
  CascadeResources &cascade = resources.cascades[cascadeIndex];

  const uint64_t uploadSize = static_cast<uint64_t>(jobs.size()) * sizeof(JobGPU);
  if (uploadSize > cascade.jobBufferSize)
  {
    throw std::runtime_error("GPUSignedDistanceFields job upload exceeds configured capacity");
  }

  std::vector<uint32_t> subvoxelOffsets(jobs.size(), 0u);
  uint64_t runningSubvoxelOffset = 0u;
  for (size_t jobIndex = 0u; jobIndex < jobs.size(); ++jobIndex)
  {
    const JobGPU &job = jobs[jobIndex];
    subvoxelOffsets[jobIndex] = static_cast<uint32_t>(runningSubvoxelOffset);
    const uint64_t dimX = std::max<uint32_t>(job.aabbMax[0], job.aabbMin[0]) - std::min<uint32_t>(job.aabbMax[0], job.aabbMin[0]);
    const uint64_t dimY = std::max<uint32_t>(job.aabbMax[1], job.aabbMin[1]) - std::min<uint32_t>(job.aabbMax[1], job.aabbMin[1]);
    const uint64_t dimZ = std::max<uint32_t>(job.aabbMax[2], job.aabbMin[2]) - std::min<uint32_t>(job.aabbMax[2], job.aabbMin[2]);
    runningSubvoxelOffset += dimX * dimY * dimZ;
  }

  cascade.jobCount = static_cast<uint32_t>(jobs.size());
  if (runningSubvoxelOffset > std::numeric_limits<uint32_t>::max())
  {
    throw std::runtime_error("GPUSignedDistanceFields job voxel count exceeds 32-bit range");
  }
  cascade.jobVoxelCount = static_cast<uint32_t>(runningSubvoxelOffset);
  uploadCascadeBuildInfo(path, cascadeIndex);
  uploadDynamicStageIndirectArgs(path);
  // Job consumers are bounded by jobCount/numJobs, so stale tail entries are never read.
  // Avoid clearing the full unused capacity every frame because that dominates CPU time
  // when maxJobsPerCascade is large.
  uploadBufferPrefix(cascade.jobBuffer, cascade.jobBufferOffset, jobs.data(), uploadSize);
  uploadBufferPrefix(
      cascade.jobSubvoxelOffsetBuffer,
      cascade.jobSubvoxelOffsetBufferOffset,
      subvoxelOffsets.data(),
      static_cast<uint64_t>(subvoxelOffsets.size()) * sizeof(uint32_t));

  auto &uploadedJobs = (path == Path::Static) ? uploadedStaticJobs_ : uploadedSkinnedJobs_;
  if (cascadeIndex >= uploadedJobs.size())
  {
    uploadedJobs.resize(cascadeIndex + 1u);
  }
  uploadedJobs[cascadeIndex] = jobs;
}

void GPUSignedDistanceFieldsManager::syncFrameLocalInputs()
{
  uploadCascadeInfos();

  if (sharedResources_.boneMatrices.isValid() && !uploadedBoneMatrices_.empty())
  {
    uploadBufferPrefix(
        sharedResources_.boneMatrices,
        0u,
        uploadedBoneMatrices_.data(),
        static_cast<uint64_t>(uploadedBoneMatrices_.size()) * sizeof(math::Mat4f));
  }

  syncPathFrameLocalInputs(Path::Static);
  syncPathFrameLocalInputs(Path::Skinned);
}

const GPUSignedDistanceFieldsManager::Settings &GPUSignedDistanceFieldsManager::getSettings() const
{
  return settings_;
}

const GPUSignedDistanceFieldsManager::SharedResources &GPUSignedDistanceFieldsManager::getSharedResources() const
{
  return sharedResources_;
}

const GPUSignedDistanceFieldsManager::PathResources &GPUSignedDistanceFieldsManager::getPathResources(Path path) const
{
  switch (path)
  {
  case Path::Static:
    return staticResources_;
  case Path::Skinned:
    return skinnedResources_;
  case Path::Combined:
    return combinedResources_;
  default:
    throw std::runtime_error("Unknown GPUSignedDistanceFields path");
  }
}

const std::vector<GPUSignedDistanceFieldsManager::MeshTableEntryGPU> &GPUSignedDistanceFieldsManager::getUploadedMeshTable() const
{
  return uploadedMeshTable_;
}

const std::vector<GPUSignedDistanceFieldsManager::SurfaceMaterialGPU> &GPUSignedDistanceFieldsManager::getUploadedSurfaceMaterials() const
{
  return uploadedSurfaceMaterials_;
}

const std::vector<GPUSignedDistanceFieldsManager::VertexGPU> &GPUSignedDistanceFieldsManager::getUploadedVertices() const
{
  return uploadedVertices_;
}

const std::vector<uint32_t> &GPUSignedDistanceFieldsManager::getUploadedIndices() const
{
  return uploadedIndices_;
}

const std::vector<GPUSignedDistanceFieldsManager::BoneWeightGPU> &GPUSignedDistanceFieldsManager::getUploadedBoneWeights() const
{
  return uploadedBoneWeights_;
}

const std::vector<math::Mat4f> &GPUSignedDistanceFieldsManager::getUploadedBoneMatrices() const
{
  return uploadedBoneMatrices_;
}

const std::vector<GPUSignedDistanceFieldsManager::InstanceGPU> &GPUSignedDistanceFieldsManager::getUploadedInstances(Path path) const
{
  switch (path)
  {
  case Path::Static:
    return uploadedStaticInstances_;
  case Path::Skinned:
    return uploadedSkinnedInstances_;
  case Path::Combined:
  default:
    throw std::runtime_error("Combined GPUSignedDistanceFields path does not expose uploaded instances");
  }
}

const std::vector<std::vector<GPUSignedDistanceFieldsManager::JobGPU>> &GPUSignedDistanceFieldsManager::getUploadedJobs(Path path) const
{
  switch (path)
  {
  case Path::Static:
    return uploadedStaticJobs_;
  case Path::Skinned:
    return uploadedSkinnedJobs_;
  case Path::Combined:
  default:
    throw std::runtime_error("Combined GPUSignedDistanceFields path does not expose uploaded jobs");
  }
}

const GPUSignedDistanceFieldsManager::CascadeInfoGPU &GPUSignedDistanceFieldsManager::getCascadeInfo(uint32_t cascadeIndex) const
{
  if (cascadeIndex >= cascadeInfos_.size())
  {
    throw std::runtime_error("GPUSignedDistanceFields cascade index out of range");
  }
  return cascadeInfos_[cascadeIndex];
}

uint64_t GPUSignedDistanceFieldsManager::getCascadeBuildInfoOffset(uint32_t cascadeIndex) const
{
  return static_cast<uint64_t>(cascadeIndex) * sizeof(CascadeBuildInfoGPU);
}

uint64_t GPUSignedDistanceFieldsManager::getCascadeInfoOffset(uint32_t cascadeIndex) const
{
  return static_cast<uint64_t>(cascadeIndex) * sizeof(CascadeInfoGPU);
}

uint32_t GPUSignedDistanceFieldsManager::getCascadeCount() const
{
  return settings_.cascadeCount;
}

bool GPUSignedDistanceFieldsManager::needsContextInitialization() const
{
  return needsContextInitialization_;
}

bool GPUSignedDistanceFieldsManager::isPathBuildable(Path path) const
{
  switch (path)
  {
  case Path::Static:
    return true;
  case Path::Skinned:
  {
    if (skinnedResources_.activeInstanceCount > 0u)
      return true;
    for (const CascadeResources &cascade : skinnedResources_.cascades)
    {
      if (cascade.jobCount > 0u || cascade.jobVoxelCount > 0u)
        return true;
    }
    return false;
  }
  case Path::Combined:
  default:
    return false;
  }
}

void GPUSignedDistanceFieldsManager::syncPathFrameLocalInputs(Path path)
{
  if (path == Path::Combined)
  {
    return;
  }

  PathResources &resources = (path == Path::Static) ? staticResources_ : skinnedResources_;
  const std::vector<InstanceGPU> &uploadedInstances = (path == Path::Static) ? uploadedStaticInstances_ : uploadedSkinnedInstances_;
  const std::vector<std::vector<JobGPU>> &uploadedJobs = (path == Path::Static) ? uploadedStaticJobs_ : uploadedSkinnedJobs_;

  if (resources.instances.isValid() && !uploadedInstances.empty())
  {
    const uint32_t instanceUploadCount = std::min(resources.instanceCount, static_cast<uint32_t>(uploadedInstances.size()));
    if (instanceUploadCount > 0u)
    {
      uploadBufferPrefix(
          resources.instances,
          0u,
          uploadedInstances.data(),
          static_cast<uint64_t>(instanceUploadCount) * sizeof(InstanceGPU));
    }
  }

  if (resources.cascadeBuildInfos.isValid())
  {
    std::vector<CascadeBuildInfoGPU> infos(resources.cascades.size());
    for (uint32_t cascadeIndex = 0u; cascadeIndex < static_cast<uint32_t>(resources.cascades.size()); ++cascadeIndex)
    {
      infos[cascadeIndex].numJobs = resources.cascades[cascadeIndex].jobCount;
      infos[cascadeIndex].numJobVoxels = resources.cascades[cascadeIndex].jobVoxelCount;
    }
    uploadWholeBuffer(
        resources.cascadeBuildInfos,
        resources.cascadeBuildInfosSize,
        infos.empty() ? nullptr : infos.data(),
        static_cast<uint64_t>(infos.size()) * sizeof(CascadeBuildInfoGPU));
  }

  if (resources.stageIndirectArgs.isValid())
  {
    constexpr uint32_t kIndirectWordCount = IndirectOffset_Count / sizeof(uint32_t);
    static_assert(IndirectOffset_Count % sizeof(uint32_t) == 0u, "Indirect argument buffer must be uint32 aligned");
    std::array<uint32_t, kIndirectWordCount> args{};

    uint32_t maxJobCount = 0u;
    uint32_t maxJobVoxelCount = 0u;
    for (const CascadeResources &cascade : resources.cascades)
    {
      maxJobCount = std::max(maxJobCount, cascade.jobCount);
      maxJobVoxelCount = std::max(maxJobVoxelCount, cascade.jobVoxelCount);
    }

    bool anyCascadeScrolled = false;
    for (const CascadeInfoGPU &cascadeInfo : cascadeInfos_)
    {
      if (cascadeInfo.clipmapInvalidationOffset[0] != 0 ||
          cascadeInfo.clipmapInvalidationOffset[1] != 0 ||
          cascadeInfo.clipmapInvalidationOffset[2] != 0)
      {
        anyCascadeScrolled = true;
        break;
      }
    }

    const uint32_t dispatchY = settings_.cascadeCount;
    const auto storeArgs = [&](IndirectOffset offset, uint32_t x, uint32_t y, uint32_t z)
    {
      const uint32_t baseIndex = static_cast<uint32_t>(offset) / sizeof(uint32_t);
      args[baseIndex + 0u] = x;
      args[baseIndex + 1u] = y;
      args[baseIndex + 2u] = z;
      args[baseIndex + 3u] = 0u;
    };

    storeArgs(
        IndirectOffset_ScrollCascade,
        anyCascadeScrolled ? divCeilU32(CASCADE_VOXEL_COUNT, 64u) : 0u,
        dispatchY,
        1u);
    storeArgs(
        IndirectOffset_ClearRefCounters,
        maxJobVoxelCount > 0u ? divCeilU32(CASCADE_VOXEL_COUNT, 64u) : 0u,
        dispatchY,
        1u);
    storeArgs(
        IndirectOffset_ClearJobCounter,
        maxJobCount > 0u ? divCeilU32(maxJobCount, 64u) : 0u,
        dispatchY,
        1u);
    storeArgs(
        IndirectOffset_InvalidateJobAreas,
        maxJobVoxelCount > 0u ? divCeilU32(maxJobVoxelCount, VOXELIZER_GROUP_SIZE) : 0u,
        dispatchY,
        1u);
    storeArgs(
        IndirectOffset_CoarseCulling,
        maxJobVoxelCount > 0u ? divCeilU32(maxJobVoxelCount, VOXELIZER_GROUP_SIZE) : 0u,
        dispatchY,
        1u);
    storeArgs(
        IndirectOffset_ScanJobs,
        maxJobCount > 0u ? divCeilU32(maxJobCount, SCAN_GROUP_SIZE) : 0u,
        dispatchY,
        1u);
    storeArgs(
        IndirectOffset_FinalizeJobScan,
        maxJobCount > 0u ? 1u : 0u,
        dispatchY,
        1u);
    storeArgs(
        IndirectOffset_ScanReferences,
        maxJobVoxelCount > 0u ? divCeilU32(CASCADE_VOXEL_COUNT, SCAN_GROUP_SIZE) : 0u,
        dispatchY,
        1u);
    storeArgs(
        IndirectOffset_FinalizeReferenceScan,
        maxJobVoxelCount > 0u ? 1u : 0u,
        dispatchY,
        1u);

    uploadWholeBuffer(resources.stageIndirectArgs, resources.stageIndirectArgsSize, args.data(), sizeof(args));
  }

  for (uint32_t cascadeIndex = 0u; cascadeIndex < static_cast<uint32_t>(resources.cascades.size()); ++cascadeIndex)
  {
    CascadeResources &cascade = resources.cascades[cascadeIndex];
    if (cascadeIndex >= uploadedJobs.size())
    {
      continue;
    }

    const std::vector<JobGPU> &jobs = uploadedJobs[cascadeIndex];
    const uint32_t jobUploadCount = std::min(cascade.jobCount, static_cast<uint32_t>(jobs.size()));
    if (jobUploadCount == 0u)
    {
      continue;
    }

    uploadBufferPrefix(
        cascade.jobBuffer,
        cascade.jobBufferOffset,
        jobs.data(),
        static_cast<uint64_t>(jobUploadCount) * sizeof(JobGPU));

    std::vector<uint32_t> subvoxelOffsets(jobUploadCount, 0u);
    uint64_t runningSubvoxelOffset = 0u;
    for (uint32_t jobIndex = 0u; jobIndex < jobUploadCount; ++jobIndex)
    {
      const JobGPU &job = jobs[jobIndex];
      subvoxelOffsets[jobIndex] = static_cast<uint32_t>(runningSubvoxelOffset);
      const uint64_t dimX = std::max<uint32_t>(job.aabbMax[0], job.aabbMin[0]) - std::min<uint32_t>(job.aabbMax[0], job.aabbMin[0]);
      const uint64_t dimY = std::max<uint32_t>(job.aabbMax[1], job.aabbMin[1]) - std::min<uint32_t>(job.aabbMax[1], job.aabbMin[1]);
      const uint64_t dimZ = std::max<uint32_t>(job.aabbMax[2], job.aabbMin[2]) - std::min<uint32_t>(job.aabbMax[2], job.aabbMin[2]);
      runningSubvoxelOffset += dimX * dimY * dimZ;
    }

    uploadBufferPrefix(
        cascade.jobSubvoxelOffsetBuffer,
        cascade.jobSubvoxelOffsetBufferOffset,
        subvoxelOffsets.data(),
        static_cast<uint64_t>(subvoxelOffsets.size()) * sizeof(uint32_t));
  }
}

void GPUSignedDistanceFieldsManager::uploadWholeBuffer(const rendering::Buffer &buffer, uint64_t bufferSize, const void *data, uint64_t dataSize)
{
  if (!buffer.isValid())
  {
    return;
  }

  if (data == nullptr || dataSize == 0u)
  {
    uploadUIntBufferFill(buffer, bufferSize, 0u);
    return;
  }

  writeManagedBuffer(buffer, 0u, dataSize, data);
  if (dataSize < bufferSize)
  {
    const uint64_t clearBytes = bufferSize - dataSize;
    const uint64_t clearWordCount = (clearBytes + sizeof(uint32_t) - 1u) / sizeof(uint32_t);
    std::vector<uint32_t> tail(clearWordCount, 0u);
    writeManagedBuffer(buffer, dataSize, clearBytes, tail.data());
  }
}

void GPUSignedDistanceFieldsManager::uploadCascadeInfos()
{
  if (!sharedResources_.cascadeInfos.isValid() || cascadeInfos_.empty())
    return;
  writeManagedBuffer(
      sharedResources_.cascadeInfos,
      0u,
      static_cast<uint64_t>(cascadeInfos_.size()) * sizeof(CascadeInfoGPU),
      cascadeInfos_.data());
}

void GPUSignedDistanceFieldsManager::uploadCascadeBuildInfo(Path path, uint32_t cascadeIndex)
{
  if (path == Path::Combined)
    return;

  PathResources &resources = (path == Path::Static) ? staticResources_ : skinnedResources_;
  if (!resources.cascadeBuildInfos.isValid() || cascadeIndex >= resources.cascades.size())
    return;

  const CascadeResources &cascade = resources.cascades[cascadeIndex];
  CascadeBuildInfoGPU info{};
  info.numJobs = cascade.jobCount;
  info.numJobVoxels = cascade.jobVoxelCount;
  writeManagedBuffer(
      resources.cascadeBuildInfos,
      getCascadeBuildInfoOffset(cascadeIndex),
      sizeof(CascadeBuildInfoGPU),
      &info);
}

void GPUSignedDistanceFieldsManager::uploadDynamicStageIndirectArgs(Path path)
{
  if (path == Path::Combined)
    return;

  PathResources &resources = (path == Path::Static) ? staticResources_ : skinnedResources_;
  if (!resources.stageIndirectArgs.isValid())
    return;

  uint32_t maxJobCount = 0u;
  uint32_t maxJobVoxelCount = 0u;
  for (const CascadeResources &cascade : resources.cascades)
  {
    maxJobCount = std::max(maxJobCount, cascade.jobCount);
    maxJobVoxelCount = std::max(maxJobVoxelCount, cascade.jobVoxelCount);
  }

  bool anyCascadeScrolled = false;
  for (const CascadeInfoGPU &cascadeInfo : cascadeInfos_)
  {
    if (cascadeInfo.clipmapInvalidationOffset[0] != 0 ||
        cascadeInfo.clipmapInvalidationOffset[1] != 0 ||
        cascadeInfo.clipmapInvalidationOffset[2] != 0)
    {
      anyCascadeScrolled = true;
      break;
    }
  }

  const uint32_t dispatchY = settings_.cascadeCount;
  const auto writeIndirectArgs = [&](IndirectOffset offset, uint32_t x, uint32_t y, uint32_t z)
  {
    const std::array<uint32_t, 4u> args{x, y, z, 0u};
    writeManagedBuffer(resources.stageIndirectArgs, static_cast<uint64_t>(offset), sizeof(args), args.data());
  };

  writeIndirectArgs(
      IndirectOffset_ScrollCascade,
      anyCascadeScrolled ? divCeilU32(CASCADE_VOXEL_COUNT, 64u) : 0u,
      dispatchY,
      1u);
  writeIndirectArgs(
      IndirectOffset_ClearRefCounters,
      maxJobVoxelCount > 0u ? divCeilU32(CASCADE_VOXEL_COUNT, 64u) : 0u,
      dispatchY,
      1u);
  writeIndirectArgs(
      IndirectOffset_ClearJobCounter,
      maxJobCount > 0u ? divCeilU32(maxJobCount, 64u) : 0u,
      dispatchY,
      1u);
  writeIndirectArgs(
      IndirectOffset_InvalidateJobAreas,
      maxJobVoxelCount > 0u ? divCeilU32(maxJobVoxelCount, VOXELIZER_GROUP_SIZE) : 0u,
      dispatchY,
      1u);
  writeIndirectArgs(
      IndirectOffset_CoarseCulling,
      maxJobVoxelCount > 0u ? divCeilU32(maxJobVoxelCount, VOXELIZER_GROUP_SIZE) : 0u,
      dispatchY,
      1u);
  writeIndirectArgs(
      IndirectOffset_ScanJobs,
      maxJobCount > 0u ? divCeilU32(maxJobCount, SCAN_GROUP_SIZE) : 0u,
      dispatchY,
      1u);
  writeIndirectArgs(
      IndirectOffset_FinalizeJobScan,
      maxJobCount > 0u ? 1u : 0u,
      dispatchY,
      1u);
  writeIndirectArgs(
      IndirectOffset_ScanReferences,
      maxJobVoxelCount > 0u ? divCeilU32(CASCADE_VOXEL_COUNT, SCAN_GROUP_SIZE) : 0u,
      dispatchY,
      1u);
  writeIndirectArgs(
      IndirectOffset_FinalizeReferenceScan,
      maxJobVoxelCount > 0u ? 1u : 0u,
      dispatchY,
      1u);
}

void GPUSignedDistanceFieldsManager::uploadBufferPrefix(const rendering::Buffer &buffer, uint64_t bufferOffset, const void *data, uint64_t dataSize)
{
  if (!buffer.isValid() || data == nullptr || dataSize == 0u)
  {
    return;
  }

  writeManagedBuffer(buffer, bufferOffset, dataSize, data);
}

void GPUSignedDistanceFieldsManager::uploadUIntBufferFill(const rendering::Buffer &buffer, uint64_t bufferSize, uint32_t value)
{
  if (!buffer.isValid() || bufferSize == 0u)
  {
    return;
  }

  std::vector<uint32_t> data((bufferSize + sizeof(uint32_t) - 1u) / sizeof(uint32_t), value);
  writeManagedBuffer(buffer, 0u, bufferSize, data.data());
}

uint32_t GPUSignedDistanceFieldsManager::positiveModulo(int32_t value, uint32_t modulus)
{
  const int32_t mod = static_cast<int32_t>(modulus);
  const int32_t result = value % mod;
  return static_cast<uint32_t>(result < 0 ? result + mod : result);
}

const char *GPUSignedDistanceFieldsManager::pathName(Path path)
{
  switch (path)
  {
  case Path::Static:
    return "Static";
  case Path::Skinned:
    return "Skinned";
  case Path::Combined:
    return "Combined";
  default:
    return "Unknown";
  }
}

} // namespace gpusigneddistancefields
