#pragma once

#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelOccupancyVolume.hpp"
#include "voxel/svraster/rendering/SparseVoxelOccupancyBasePass.hpp"
#include "voxel/svraster/rendering/SparseVoxelOccupancyDirtyPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelOccupancyMipPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelOccupancyScenePass.hpp"
#include "voxel/svraster/rendering/SparseVoxelOccupancySourceSplatPass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace voxel::svraster::rendering
{

// Owns the persistent GPU representation of one occupancy volume and pyramid.
// The production path uploads only camera/scene metadata and up to three
// world-space invalidation boxes. The GPU classifies exact packed-word masks,
// seeds dirty-overlapping scene instances, traverses their BLAS hierarchies,
// atomically splats reached leaves into mip 0, and reduces the pyramid. The
// scalar dirty-word path remains available as a test oracle.
class SparseVoxelOccupancyMipChain final
{
public:
  struct GPUGeneratedSources
  {
    ::rendering::Buffer leaves;
    ::rendering::Buffer instances;
    ::rendering::Buffer centersLengths;
    uint32_t sourceCount = 0u;
    uint32_t instanceCapacity = 0u;
    uint32_t centerCapacity = 0u;
  };

  struct GPUSceneTraversal
  {
    ::rendering::Buffer instances;
    ::rendering::Buffer hierarchyNodes;
    ::rendering::Buffer centersLengths;
    ::rendering::Buffer blasRoots;
    uint32_t instanceCount = 0u;
    uint32_t hierarchyNodeCount = 0u;
    uint32_t blasCount = 0u;
    uint32_t traversalLevelCount = 0u;
    uint32_t queueCapacity = 0u;
  };

  SparseVoxelOccupancyMipChain(
      ::rendering::RenderGraph &graph,
      uint32_t &passIndex,
      const SparseVoxelOccupancyVolume &initialVolume,
      const std::vector<SparseVoxelAABB> &initialSourceAABBs,
      const uint32_t sourceAABBCapacity,
      std::string name = "SparseVoxelOccupancy",
      const GPUGeneratedSources *gpuGeneratedSources = nullptr,
      const GPUSceneTraversal *gpuSceneTraversal = nullptr)
      : graph_(&graph), name_(std::move(name)), baseWordCount_(initialVolume.layout().wordCount), mipWordCount_(initialVolume.pyramidLayout().wordCount),
        levelCount_(static_cast<uint32_t>(initialVolume.pyramidLayout().levels.size())), sourceAABBCapacity_(std::max(1u, sourceAABBCapacity))
  {
    if (gpuGeneratedSources != nullptr &&
        gpuSceneTraversal != nullptr)
      throw std::invalid_argument(
          "Sparse voxel occupancy accepts one GPU source producer");
    if (initialVolume.state().valid == 0u || baseWordCount_ == 0u || mipWordCount_ == 0u || levelCount_ == 0u || initialVolume.mipLevels().size() != levelCount_)
      throw std::invalid_argument(
          "Sparse voxel occupancy mip chain requires a valid populated "
          "volume");
    if (initialSourceAABBs.size() > sourceAABBCapacity_ ||
        (gpuGeneratedSources != nullptr &&
         gpuGeneratedSources->sourceCount > sourceAABBCapacity_))
      throw std::invalid_argument("Sparse voxel occupancy source AABBs exceed chain capacity");
    if (gpuSceneTraversal != nullptr &&
        (gpuSceneTraversal->instanceCount == 0u ||
         gpuSceneTraversal->hierarchyNodeCount == 0u ||
         gpuSceneTraversal->blasCount == 0u ||
         gpuSceneTraversal->traversalLevelCount == 0u ||
         gpuSceneTraversal->queueCapacity == 0u ||
         gpuSceneTraversal->queueCapacity >
             sourceAABBCapacity_))
      throw std::invalid_argument(
          "Sparse voxel occupancy scene traversal is invalid");

    baseWords_ = graph_->createBuffer({
      .name = name_ + ".BaseWords",
      .size = baseWordCount_ * sizeof(uint32_t),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push | ::rendering::BufferUsage::BufferUsage_CopySrc,
    });
    dirtyWords_ = graph_->createBuffer({
      .name = name_ + ".DirtyWords",
      .size = (gpuGeneratedSources != nullptr ? 1u : baseWordCount_) *
          sizeof(SparseVoxelOccupancyDirtyWordGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    denseDirtyMasks_ = graph_->createBuffer({
      .name = name_ + ".DenseDirtyMasks",
      .size = baseWordCount_ * sizeof(uint32_t),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage |
          ::rendering::BufferUsage::BufferUsage_Push,
    });
    dirtyWordIndices_ = graph_->createBuffer({
      .name = name_ + ".DirtyWordIndices",
      .size = baseWordCount_ * sizeof(uint32_t),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage |
          ::rendering::BufferUsage::BufferUsage_Push,
    });
    dirtyWordCount_ = graph_->createBuffer({
      .name = name_ + ".DirtyWordCount",
      .size = sizeof(uint32_t),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage |
          ::rendering::BufferUsage::BufferUsage_CopySrc |
          ::rendering::BufferUsage::BufferUsage_Push,
    });
    dirtyDispatchArgs_ = graph_->createBuffer({
      .name = name_ + ".DirtyDispatchArgs",
      .size = uint64_t{5u + levelCount_} *
          sizeof(IndirectDispatchArgsGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage |
          ::rendering::BufferUsage::BufferUsage_Indirect |
          ::rendering::BufferUsage::BufferUsage_CopySrc |
          ::rendering::BufferUsage::BufferUsage_Push,
    });
    mipWordCounts_ = graph_->createBuffer({
      .name = name_ + ".MipWordCounts",
      .size = uint64_t{levelCount_} * sizeof(uint32_t),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage |
          ::rendering::BufferUsage::BufferUsage_Push,
    });
    dirtyQueryBoxes_ = graph_->createBuffer({
      .name = name_ + ".DirtyQueryBoxes",
      .size = 3u * sizeof(SparseVoxelOccupancyAABBGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage |
          ::rendering::BufferUsage::BufferUsage_Push,
    });
    sourceAABBs_ = graph_->createBuffer({
      .name = name_ + ".SourceAABBs",
      .size = (gpuGeneratedSources != nullptr
                   ? 1u
                   : uint64_t{sourceAABBCapacity_}) *
          sizeof(SparseVoxelOccupancyAABBGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage |
          ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    if (gpuSceneTraversal != nullptr)
    {
      sceneQueueA_ = graph_->createBuffer({
        .name = name_ + ".SceneQueueA",
        .size = uint64_t{gpuSceneTraversal->queueCapacity} *
            sizeof(SparseVoxelLODQueueElementGPU),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
            ::rendering::BufferUsage::BufferUsage_Push,
        .frameLocal = true,
      });
      sceneQueueB_ = graph_->createBuffer({
        .name = name_ + ".SceneQueueB",
        .size = uint64_t{gpuSceneTraversal->queueCapacity} *
            sizeof(SparseVoxelLODQueueElementGPU),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
            ::rendering::BufferUsage::BufferUsage_Push,
        .frameLocal = true,
      });
      sceneQueueCounts_ = graph_->createBuffer({
        .name = name_ + ".SceneQueueCounts",
        .size = uint64_t{
                    gpuSceneTraversal->traversalLevelCount} *
            sizeof(uint32_t),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
            ::rendering::BufferUsage::BufferUsage_Push,
        .frameLocal = true,
      });
      sceneDispatchArgs_ = graph_->createBuffer({
        .name = name_ + ".SceneDispatchArgs",
        .size = uint64_t{
                    gpuSceneTraversal->traversalLevelCount} *
            sizeof(IndirectDispatchArgsGPU),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
            ::rendering::BufferUsage::BufferUsage_Indirect |
            ::rendering::BufferUsage::BufferUsage_Push,
        .frameLocal = true,
      });
    }
    mipWords_ = graph_->createBuffer({
      .name = name_ + ".MipWords",
      .size = mipWordCount_ * sizeof(uint32_t),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push | ::rendering::BufferUsage::BufferUsage_CopySrc,
    });
    pendingBaseState_ = graph_->createBuffer({
      .name = name_ + ".PendingBaseState",
      .size = sizeof(SparseVoxelOccupancyVolumeGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage |
          ::rendering::BufferUsage::BufferUsage_Push |
          ::rendering::BufferUsage::BufferUsage_CopySrc,
      .frameLocal = true,
    });
    pendingLevelDescriptors_ = graph_->createBuffer({
      .name = name_ + ".PendingLevelDescriptors",
      .size = uint64_t{levelCount_} * sizeof(SparseVoxelOccupancyMipLevelGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Push |
          ::rendering::BufferUsage::BufferUsage_CopySrc,
      .frameLocal = true,
    });
    pendingPyramidState_ = graph_->createBuffer({
      .name = name_ + ".PendingPyramidState",
      .size = sizeof(SparseVoxelOccupancyPyramidGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Push |
          ::rendering::BufferUsage::BufferUsage_CopySrc,
      .frameLocal = true,
    });
    baseState_ = graph_->createBuffer({
      .name = name_ + ".CommittedBaseState",
      .size = sizeof(SparseVoxelOccupancyVolumeGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage |
          ::rendering::BufferUsage::BufferUsage_CopyDst,
    });
    levelDescriptors_ = graph_->createBuffer({
      .name = name_ + ".CommittedLevelDescriptors",
      .size = uint64_t{levelCount_} *
          sizeof(SparseVoxelOccupancyMipLevelGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage |
          ::rendering::BufferUsage::BufferUsage_CopyDst,
    });
    pyramidState_ = graph_->createBuffer({
      .name = name_ + ".CommittedPyramidState",
      .size = sizeof(SparseVoxelOccupancyPyramidGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
          ::rendering::BufferUsage::BufferUsage_CopyDst,
    });
    occupancyTexture_ = graph_->createTexture({
      .name = name_ + ".Texture",
      .format = ::rendering::Format::Format_R32Uint,
      .memoryProperties = ::rendering::BufferUsage::BufferUsage_Storage,
      .usage = ::rendering::ImageUsage::ImageUsage_Storage | ::rendering::ImageUsage::ImageUsage_Sampled,
      .width = initialVolume.state().wordResolution[0],
      .height = initialVolume.state().wordResolution[1],
      .depth = initialVolume.state().wordResolution[2],
      .mipLevels = 1u,
    });

    std::vector<uint32_t> zeroBaseWords(baseWordCount_, 0u);
    graph_->bufferWrite(baseWords_, 0u, zeroBaseWords.size() * sizeof(uint32_t), zeroBaseWords.data());
    graph_->bufferWrite(
        denseDirtyMasks_,
        0u,
        zeroBaseWords.size() * sizeof(uint32_t),
        zeroBaseWords.data());
    std::vector<uint32_t> zeroMipWords(mipWordCount_, 0u);
    graph_->bufferWrite(mipWords_, 0u, zeroMipWords.size() * sizeof(uint32_t), zeroMipWords.data());
    std::vector<uint32_t> mipWordCounts;
    mipWordCounts.reserve(levelCount_);
    for (const SparseVoxelOccupancyMipLevel &level :
         initialVolume.mipLevels())
      mipWordCounts.push_back(level.state.wordCount);
    graph_->bufferWrite(
        mipWordCounts_,
        0u,
        mipWordCounts.size() * sizeof(uint32_t),
        mipWordCounts.data());
    uploadedSourceRevisionByFrame_.assign(
        graph_->getMaxFramesInFlight(), 0u);

    gpuGenerated_ =
        gpuGeneratedSources != nullptr ||
        gpuSceneTraversal != nullptr;
    sceneTraversal_ = gpuSceneTraversal != nullptr;
    passes_.reserve(
        levelCount_ + 9u +
        (gpuSceneTraversal != nullptr
             ? 3u + 2u *
                   gpuSceneTraversal->traversalLevelCount
             : 0u));
    const SparseVoxelOccupancyBasePass::Buffers baseBuffers{
      baseWords_,
      dirtyWords_,
      sourceAABBs_,
      denseDirtyMasks_,
      dirtyQueryBoxes_,
    };
    const SparseVoxelOccupancyBasePass::Capacities baseCapacities{
      baseWordCount_,
      gpuGenerated_ ? 1u : static_cast<uint32_t>(baseWordCount_),
      gpuGenerated_ ? 1u : sourceAABBCapacity_,
    };
    SparseVoxelOccupancyDirtyPass::Buffers dirtyBuffers{
      dirtyQueryBoxes_,
      denseDirtyMasks_,
      dirtyWordIndices_,
      dirtyWordCount_,
      dirtyDispatchArgs_,
      baseWords_,
      gpuSceneTraversal != nullptr
          ? gpuSceneTraversal->instances
          : (gpuGeneratedSources != nullptr
                 ? gpuGeneratedSources->instances
                 : ::rendering::Buffer{}),
      mipWordCounts_,
    };
    const uint32_t occupancyInstanceCount =
        gpuSceneTraversal != nullptr
            ? gpuSceneTraversal->instanceCount
            : (gpuGeneratedSources != nullptr
                   ? gpuGeneratedSources->instanceCapacity
                   : 0u);
    if (gpuGenerated_)
    {
      const auto addDirtyPass =
          [&](const std::string &suffix,
              const SparseVoxelOccupancyDirtyPass::Kernel kernel,
              const uint32_t queryIndex = 0u)
      {
        auto *pass =
            graph_->registerPass<SparseVoxelOccupancyDirtyPass>(
                name_ + suffix,
                passIndex++,
                kernel,
                dirtyBuffers,
                occupancyTexture_,
                static_cast<uint32_t>(baseWordCount_),
                occupancyInstanceCount,
                levelCount_,
                queryIndex);
        passes_.push_back(pass);
        sparseDirtyPasses_.push_back(pass);
        return pass;
      };
      addDirtyPass(
          ".ResetAndCullDirtyCameraQueries",
          SparseVoxelOccupancyDirtyPass::Kernel::ResetAndCullQueries);
      addDirtyPass(
          ".MarkFullDirtyVolume",
          SparseVoxelOccupancyDirtyPass::Kernel::MarkFull);
      for (uint32_t queryIndex = 0u; queryIndex < 3u; ++queryIndex)
        addDirtyPass(
            ".MarkDirtyQuery" + std::to_string(queryIndex),
            SparseVoxelOccupancyDirtyPass::Kernel::MarkQuery,
            queryIndex);
      addDirtyPass(
          ".FinalizeDirtyState",
          SparseVoxelOccupancyDirtyPass::Kernel::Finalize);
      addDirtyPass(
          ".ClearFullBase",
          SparseVoxelOccupancyDirtyPass::Kernel::ClearFull);
      addDirtyPass(
          ".ClearBase",
          SparseVoxelOccupancyDirtyPass::Kernel::Clear);
    }
    else
    {
      clearBasePass_ =
          graph_->registerPass<SparseVoxelOccupancyBasePass>(
              name_ + ".ClearBase",
              passIndex++,
              SparseVoxelOccupancyBasePass::Kernel::ClearDirtyWords,
              baseBuffers,
              occupancyTexture_,
              baseCapacities);
      passes_.push_back(clearBasePass_);
    }
    if (gpuSceneTraversal != nullptr)
    {
      const SparseVoxelOccupancyScenePass::Buffers sceneBuffers{
        dirtyQueryBoxes_,
        denseDirtyMasks_,
        baseWords_,
        gpuSceneTraversal->instances,
        gpuSceneTraversal->hierarchyNodes,
        gpuSceneTraversal->centersLengths,
        gpuSceneTraversal->blasRoots,
        sceneQueueA_,
        sceneQueueB_,
        sceneQueueCounts_,
        sceneDispatchArgs_,
      };
      const SparseVoxelOccupancySceneTraversalGPU sceneSettings{
        gpuSceneTraversal->instanceCount,
        gpuSceneTraversal->hierarchyNodeCount,
        gpuSceneTraversal->blasCount,
        gpuSceneTraversal->traversalLevelCount,
        gpuSceneTraversal->queueCapacity,
        0u,
        0u,
        0u,
      };
      const auto addScenePass =
          [&](const std::string &suffix,
              const SparseVoxelOccupancyScenePass::Kernel kernel,
              const uint32_t depth)
      {
        SparseVoxelOccupancySceneTraversalGPU settings =
            sceneSettings;
        settings.depth = depth;
        auto *pass =
            graph_->registerPass<
                SparseVoxelOccupancyScenePass>(
                name_ + suffix,
                passIndex++,
                kernel,
                sceneBuffers,
                settings,
                baseWordCount_);
        passes_.push_back(pass);
        sceneTraversalPasses_.push_back(pass);
      };
      addScenePass(
          ".ClearSceneTraversal",
          SparseVoxelOccupancyScenePass::Kernel::Clear,
          0u);
      addScenePass(
          ".SeedSceneTraversal",
          SparseVoxelOccupancyScenePass::Kernel::Seed,
          0u);
      addScenePass(
          ".FinalizeSceneRoots",
          SparseVoxelOccupancyScenePass::Kernel::Finalize,
          0u);
      for (uint32_t depth = 0u;
           depth <
           gpuSceneTraversal->traversalLevelCount;
           ++depth)
      {
        addScenePass(
            ".TraverseScene" + std::to_string(depth),
            SparseVoxelOccupancyScenePass::Kernel::Traverse,
            depth);
        if (depth + 1u <
            gpuSceneTraversal->traversalLevelCount)
          addScenePass(
              ".FinalizeSceneLevel" +
                  std::to_string(depth + 1u),
              SparseVoxelOccupancyScenePass::Kernel::Finalize,
              depth + 1u);
      }
    }
    else if (gpuGenerated_)
    {
      sourceSplatPass_ =
          graph_->registerPass<
              SparseVoxelOccupancySourceSplatPass>(
              name_ + ".SplatSources",
              passIndex++,
              SparseVoxelOccupancySourceSplatPass::Buffers{
                gpuGeneratedSources->leaves,
                gpuGeneratedSources->instances,
                gpuGeneratedSources->centersLengths,
                dirtyQueryBoxes_,
                baseWords_,
                denseDirtyMasks_,
              },
              SparseVoxelOccupancySourceGPU{
                sourceAABBCapacity_,
                gpuGeneratedSources->instanceCapacity,
                gpuGeneratedSources->centerCapacity,
                gpuGeneratedSources->sourceCount + 1u,
              },
              baseWordCount_);
      passes_.push_back(sourceSplatPass_);
    }
    else
    {
      splatBasePass_ =
          graph_->registerPass<SparseVoxelOccupancyBasePass>(
              name_ + ".SplatBase",
              passIndex++,
              SparseVoxelOccupancyBasePass::Kernel::SplatAABBs,
              baseBuffers,
              occupancyTexture_,
              baseCapacities);
      passes_.push_back(splatBasePass_);
    }
    if (gpuGenerated_)
    {
      auto *publishFull =
          graph_->registerPass<SparseVoxelOccupancyDirtyPass>(
              name_ + ".PublishFullBase",
              passIndex++,
              SparseVoxelOccupancyDirtyPass::Kernel::PublishFull,
              dirtyBuffers,
              occupancyTexture_,
              static_cast<uint32_t>(baseWordCount_),
              occupancyInstanceCount,
              levelCount_);
      passes_.push_back(publishFull);
      sparseDirtyPasses_.push_back(publishFull);
      auto *publish =
          graph_->registerPass<SparseVoxelOccupancyDirtyPass>(
              name_ + ".PublishBase",
              passIndex++,
              SparseVoxelOccupancyDirtyPass::Kernel::Publish,
              dirtyBuffers,
              occupancyTexture_,
              static_cast<uint32_t>(baseWordCount_),
              occupancyInstanceCount,
              levelCount_);
      passes_.push_back(publish);
      sparseDirtyPasses_.push_back(publish);
    }
    else
    {
      publishBasePass_ =
          graph_->registerPass<SparseVoxelOccupancyBasePass>(
              name_ + ".PublishBase",
              passIndex++,
              SparseVoxelOccupancyBasePass::Kernel::
                  CopyDirtyWordsToTexture,
              baseBuffers,
              occupancyTexture_,
              baseCapacities);
      passes_.push_back(publishBasePass_);
    }
    mipPasses_.reserve(levelCount_);
    for (uint32_t mipIndex = 0u; mipIndex < levelCount_; ++mipIndex)
    {
      const SparseVoxelOccupancyMipLevel &level = initialVolume.mipLevels()[mipIndex];
      auto *pass = graph_->registerPass<SparseVoxelOccupancyMipPass>(
          name_ + ".BuildMip" + std::to_string(mipIndex + 1u),
          passIndex++,
          gpuGenerated_
              ? (mipIndex == 0u
                     ? SparseVoxelOccupancyMipPass::Kernel::FullFromBase
                     : SparseVoxelOccupancyMipPass::Kernel::FullFromMip)
              : (mipIndex == 0u
                     ? SparseVoxelOccupancyMipPass::Kernel::FromBase
                     : SparseVoxelOccupancyMipPass::Kernel::FromMip),
          SparseVoxelOccupancyMipPass::Buffers{
            baseWords_, mipWords_, dirtyDispatchArgs_},
          baseWordCount_,
          mipWordCount_,
          level.state.wordCount,
          mipIndex);
      passes_.push_back(pass);
      mipPasses_.push_back(pass);
    }
    passes_.push_back(
        graph_->registerPass<::rendering::gpgpu::CopyBufferPass>(
            name_ + ".CommitBaseState",
            passIndex++,
            pendingBaseState_,
            0u,
            sizeof(SparseVoxelOccupancyVolumeGPU),
            baseState_,
            0u,
            sizeof(SparseVoxelOccupancyVolumeGPU)));
    passes_.push_back(
        graph_->registerPass<::rendering::gpgpu::CopyBufferPass>(
            name_ + ".CommitLevelDescriptors",
            passIndex++,
            pendingLevelDescriptors_,
            0u,
            uint64_t{levelCount_} *
                sizeof(SparseVoxelOccupancyMipLevelGPU),
            levelDescriptors_,
            0u,
            uint64_t{levelCount_} *
                sizeof(SparseVoxelOccupancyMipLevelGPU)));
    passes_.push_back(
        graph_->registerPass<::rendering::gpgpu::CopyBufferPass>(
            name_ + ".CommitPyramidState",
            passIndex++,
            pendingPyramidState_,
            0u,
            sizeof(SparseVoxelOccupancyPyramidGPU),
            pyramidState_,
            0u,
            sizeof(SparseVoxelOccupancyPyramidGPU)));

    std::string error;
    const bool initialized =
        sceneTraversal_
            ? updateGenerated(initialVolume, 0u, &error)
            : sourceSplatPass_ != nullptr
            ? updateGenerated(
                  initialVolume,
                  gpuGeneratedSources->sourceCount,
                  &error)
            : update(initialVolume, initialSourceAABBs, &error);
    if (!initialized)
      throw std::invalid_argument(error);
  }

  SparseVoxelOccupancyMipChain(const SparseVoxelOccupancyMipChain &) = delete;
  SparseVoxelOccupancyMipChain &operator=(const SparseVoxelOccupancyMipChain &) = delete;

  ~SparseVoxelOccupancyMipChain()
  {
    if (graph_ == nullptr)
      return;
    for (auto it = passes_.rbegin(); it != passes_.rend(); ++it)
      delete *it;
    if (pyramidState_.isValid())
      graph_->deleteBuffer(pyramidState_);
    if (levelDescriptors_.isValid())
      graph_->deleteBuffer(levelDescriptors_);
    if (baseState_.isValid())
      graph_->deleteBuffer(baseState_);
    if (pendingPyramidState_.isValid())
      graph_->deleteBuffer(pendingPyramidState_);
    if (pendingLevelDescriptors_.isValid())
      graph_->deleteBuffer(pendingLevelDescriptors_);
    if (pendingBaseState_.isValid())
      graph_->deleteBuffer(pendingBaseState_);
    if (sourceAABBs_.isValid())
      graph_->deleteBuffer(sourceAABBs_);
    if (sceneDispatchArgs_.isValid())
      graph_->deleteBuffer(sceneDispatchArgs_);
    if (sceneQueueCounts_.isValid())
      graph_->deleteBuffer(sceneQueueCounts_);
    if (sceneQueueB_.isValid())
      graph_->deleteBuffer(sceneQueueB_);
    if (sceneQueueA_.isValid())
      graph_->deleteBuffer(sceneQueueA_);
    if (dirtyQueryBoxes_.isValid())
      graph_->deleteBuffer(dirtyQueryBoxes_);
    if (dirtyDispatchArgs_.isValid())
      graph_->deleteBuffer(dirtyDispatchArgs_);
    if (mipWordCounts_.isValid())
      graph_->deleteBuffer(mipWordCounts_);
    if (dirtyWordCount_.isValid())
      graph_->deleteBuffer(dirtyWordCount_);
    if (dirtyWordIndices_.isValid())
      graph_->deleteBuffer(dirtyWordIndices_);
    if (denseDirtyMasks_.isValid())
      graph_->deleteBuffer(denseDirtyMasks_);
    if (dirtyWords_.isValid())
      graph_->deleteBuffer(dirtyWords_);
    if (mipWords_.isValid())
      graph_->deleteBuffer(mipWords_);
    if (baseWords_.isValid())
      graph_->deleteBuffer(baseWords_);
    if (occupancyTexture_.isValid())
      graph_->deleteTexture(occupancyTexture_);
  }

  bool update(
      const SparseVoxelOccupancyVolume &volume,
      const std::vector<SparseVoxelAABB> &sourceAABBs,
      std::string *error = nullptr)
  {
    return update(volume, sourceAABBs, true, error);
  }

  bool update(
      const SparseVoxelOccupancyVolume &volume,
      const std::vector<SparseVoxelAABB> &sourceAABBs,
      const bool sourceAABBsChanged,
      std::string *error = nullptr)
  {
    return updateImpl(
        volume,
        &sourceAABBs,
        sourceAABBsChanged,
        static_cast<uint32_t>(sourceAABBs.size()),
        {},
        error);
  }

  bool updateGenerated(
      const SparseVoxelOccupancyVolume &volume,
      const uint32_t sourceCount,
      std::string *error = nullptr)
  {
    return updateImpl(
        volume, nullptr, false, sourceCount, {}, error);
  }

  bool updateGenerated(
      const SparseVoxelOccupancyVolume &volume,
      const uint32_t sourceCount,
      const std::vector<SparseVoxelAABB> &invalidatedWorldBounds,
      std::string *error = nullptr)
  {
    return updateImpl(
        volume,
        nullptr,
        false,
        sourceCount,
        invalidatedWorldBounds,
        error);
  }

private:
  bool updateImpl(
      const SparseVoxelOccupancyVolume &volume,
      const std::vector<SparseVoxelAABB> *sourceAABBs,
      const bool sourceAABBsChanged,
      const uint32_t sourceCount,
      const std::vector<SparseVoxelAABB> &invalidatedWorldBounds,
      std::string *error)
  {
    const auto fail = [&](const std::string &message)
    {
      if (error != nullptr)
        *error = message;
      return false;
    };
    if (volume.state().valid == 0u)
      return fail("Sparse voxel occupancy mip update is not valid");
    if (volume.layout().wordCount != baseWordCount_ || volume.pyramidLayout().wordCount != mipWordCount_ || volume.mipLevels().size() != levelCount_)
      return fail("Sparse voxel occupancy mip update changed the configured layout");
    if (!gpuGenerated_ && volume.words().size() != baseWordCount_)
      return fail("Sparse voxel occupancy base word count does not match its layout");
    if (volume.lastDirtyWords().size() > baseWordCount_)
      return fail("Sparse voxel occupancy dirty base words exceed chain capacity");
    if (!sceneTraversal_ &&
        sourceCount > sourceAABBCapacity_)
      return fail("Sparse voxel occupancy source AABBs exceed chain capacity");
    if (sourceAABBs == nullptr &&
        sourceSplatPass_ == nullptr &&
        !sceneTraversal_)
      return fail(
          "Sparse voxel occupancy generated sources are not configured");
    if (sourceAABBs != nullptr &&
        !sourceAABBsChanged &&
        sourceCount != uploadedSourceAABBCount_)
      return fail(
          "Sparse voxel occupancy unchanged source count does not match "
          "the uploaded source buffer");
    if (sourceAABBs != nullptr && sourceAABBsChanged)
    {
      std::vector<SparseVoxelOccupancyAABBGPU> gpuSourceAABBs;
      gpuSourceAABBs.reserve(sourceAABBs->size());
      for (const SparseVoxelAABB &bounds : *sourceAABBs)
      {
        SparseVoxelOccupancyAABBGPU gpu{};
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
          if (!std::isfinite(bounds.minimum[axis]) ||
              !std::isfinite(bounds.maximum[axis]) ||
              bounds.minimum[axis] > bounds.maximum[axis])
            return fail("Sparse voxel occupancy source AABB is invalid");
          gpu.minimum[axis] = bounds.minimum[axis];
          gpu.maximum[axis] = bounds.maximum[axis];
        }
        gpuSourceAABBs.push_back(gpu);
      }
      cachedSourceAABBs_ = std::move(gpuSourceAABBs);
      uploadedSourceAABBCount_ =
          static_cast<uint32_t>(cachedSourceAABBs_.size());
      if (sourceRevision_ == std::numeric_limits<uint64_t>::max())
        return fail("Sparse voxel occupancy source revision overflow");
      ++sourceRevision_;
    }

    const std::vector<SparseVoxelAABB> queryBoxes =
        gpuGenerated_
            ? sparseVoxelOccupancyQueryBoxes(
                  volume.state(), invalidatedWorldBounds)
            : std::vector<SparseVoxelAABB>{};
    if (queryBoxes.size() > 3u)
      return fail(
          "Sparse voxel occupancy dirty query exceeds GPU box capacity");
    SparseVoxelOccupancyDispatchGPU baseDispatch{};
    baseDispatch.reserved0 =
        static_cast<uint32_t>(queryBoxes.size());
    baseDispatch.sourceAABBCount = gpuGenerated_
        ? (queryBoxes.empty() ? 0u : sourceCount)
        : (volume.lastDirtyWords().empty() ? 0u : sourceCount);
    if (sourceSplatPass_ != nullptr &&
        !sourceSplatPass_->update(
            volume.state(), baseDispatch.sourceAABBCount))
      return fail(
          "Sparse voxel occupancy generated source count exceeds capacity");
    for (SparseVoxelOccupancyScenePass *pass :
         sceneTraversalPasses_)
      pass->update(
          volume.state(),
          static_cast<uint32_t>(queryBoxes.size()));
    for (SparseVoxelOccupancyDirtyPass *pass :
         sparseDirtyPasses_)
      pass->update(volume.state(), queryBoxes);
    if (classifyBasePass_ != nullptr)
      classifyBasePass_->update(volume.state(), baseDispatch);
    if (clearBasePass_ != nullptr)
      clearBasePass_->update(volume.state(), baseDispatch);
    if (splatBasePass_ != nullptr)
      splatBasePass_->update(volume.state(), baseDispatch);
    if (publishBasePass_ != nullptr)
      publishBasePass_->update(volume.state(), baseDispatch);

    std::vector<SparseVoxelOccupancyMipLevelGPU> descriptors;
    descriptors.reserve(levelCount_);
    uint64_t mipDirtyWordCount = 0u;
    for (uint32_t mipIndex = 0u; mipIndex < levelCount_; ++mipIndex)
    {
      const SparseVoxelOccupancyMipLevel &level = volume.mipLevels()[mipIndex];
      const SparseVoxelOccupancyMipLayout &layout = volume.pyramidLayout().levels[mipIndex];
      if (level.state.wordOffset != layout.gpu.wordOffset ||
          level.state.wordCount != layout.gpu.wordCount ||
          (!gpuGenerated_ &&
           level.words.size() != level.state.wordCount) ||
          level.lastDirtyWords.size() > level.state.wordCount)
        return fail("Sparse voxel occupancy mip level does not match its allocation");

      SparseVoxelOccupancyMipBuildGPU build{};
      build.baseState = volume.state();
      if (mipIndex > 0u)
        build.childLevel = volume.mipLevels()[mipIndex - 1u].state;
      build.parentLevel = level.state;
      if (gpuGenerated_)
      {
        if (!mipPasses_[mipIndex]->updateFull(
                build, !queryBoxes.empty()))
          return fail(
              "Sparse voxel occupancy full mip work exceeds pass capacity");
        if (!queryBoxes.empty())
          mipDirtyWordCount += level.state.wordCount;
      }
      else
      {
        if (!mipPasses_[mipIndex]->update(
                build, level.lastDirtyWords))
          return fail(
              "Sparse voxel occupancy mip dirty work exceeds pass capacity");
        mipDirtyWordCount += level.lastDirtyWords.size();
      }
      descriptors.push_back(level.state);
    }
    if (mipDirtyWordCount > std::numeric_limits<uint32_t>::max())
      return fail("Sparse voxel occupancy mip dirty count overflow");

    SparseVoxelOccupancyPyramidGPU pyramid{};
    pyramid.contentGeneration = volume.state().contentGeneration;
    pyramid.levelCount = levelCount_;
    pyramid.valid = 1u;
    pyramid.baseDirtyWordCount = gpuGenerated_
        ? (queryBoxes.empty() ? 0u : 1u)
        : static_cast<uint32_t>(volume.lastDirtyWords().size());
    pyramid.mipDirtyWordCount = static_cast<uint32_t>(mipDirtyWordCount);
    pyramid.mipWordCount = static_cast<uint32_t>(mipWordCount_);
    pyramid.fullRebuild = volume.state().fullRebuild;

    if (gpuGenerated_)
    {
      std::array<SparseVoxelOccupancyAABBGPU, 3u> gpuQueryBoxes{};
      const float maximum = std::numeric_limits<float>::max();
      for (SparseVoxelOccupancyAABBGPU &gpu : gpuQueryBoxes)
      {
        gpu.minimum = {maximum, maximum, maximum};
        gpu.maximum = {-maximum, -maximum, -maximum};
      }
      for (size_t queryIndex = 0u;
           queryIndex < queryBoxes.size();
           ++queryIndex)
      {
        const SparseVoxelAABB &bounds = queryBoxes[queryIndex];
        SparseVoxelOccupancyAABBGPU &gpu = gpuQueryBoxes[queryIndex];
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
          if (!std::isfinite(bounds.minimum[axis]) ||
              !std::isfinite(bounds.maximum[axis]) ||
              bounds.minimum[axis] > bounds.maximum[axis])
            return fail(
                "Sparse voxel occupancy dirty query AABB is invalid");
          gpu.minimum[axis] = bounds.minimum[axis];
          gpu.maximum[axis] = bounds.maximum[axis];
        }
      }
      graph_->bufferWrite(
          dirtyQueryBoxes_,
          0u,
          sizeof(gpuQueryBoxes),
          gpuQueryBoxes.data());
    }
    else if (!volume.lastDirtyWords().empty())
      graph_->bufferWrite(dirtyWords_, 0u, volume.lastDirtyWords().size() * sizeof(SparseVoxelOccupancyDirtyWordGPU), const_cast<SparseVoxelOccupancyDirtyWordGPU *>(volume.lastDirtyWords().data()));
    if (sourceAABBs != nullptr)
    {
      const uint32_t frameSlot = graph_->getCurrentFrameIndex();
      if (frameSlot >= uploadedSourceRevisionByFrame_.size())
        return fail("Sparse voxel occupancy frame slot exceeds source cache");
      if (uploadedSourceRevisionByFrame_[frameSlot] != sourceRevision_)
      {
        if (!cachedSourceAABBs_.empty())
          graph_->bufferWrite(
              sourceAABBs_, 0u,
              cachedSourceAABBs_.size() *
                  sizeof(SparseVoxelOccupancyAABBGPU),
              cachedSourceAABBs_.data());
        uploadedSourceRevisionByFrame_[frameSlot] = sourceRevision_;
      }
    }
    graph_->bufferWrite(
        pendingBaseState_, 0u, sizeof(SparseVoxelOccupancyVolumeGPU),
        const_cast<SparseVoxelOccupancyVolumeGPU *>(&volume.state()));
    graph_->bufferWrite(
        pendingLevelDescriptors_, 0u,
        descriptors.size() * sizeof(SparseVoxelOccupancyMipLevelGPU),
        descriptors.data());
    graph_->bufferWrite(
        pendingPyramidState_, 0u, sizeof(pyramid), &pyramid);
    published_ = pyramid;
    return true;
  }

public:
  const SparseVoxelOccupancyPyramidGPU &publishedState() const
  {
    return published_;
  }

  ::rendering::Buffer baseWords() const
  {
    return baseWords_;
  }
  ::rendering::Buffer mipWords() const
  {
    return mipWords_;
  }
  ::rendering::Buffer dirtyWordCount() const
  {
    return dirtyWordCount_;
  }
  ::rendering::Buffer dirtyDispatchArgs() const
  {
    return dirtyDispatchArgs_;
  }
  ::rendering::Buffer baseState() const
  {
    return baseState_;
  }
  ::rendering::Buffer levelDescriptors() const
  {
    return levelDescriptors_;
  }
  ::rendering::Buffer pyramidState() const
  {
    return pyramidState_;
  }
  ::rendering::Buffer stagedPyramidState() const
  {
    return pendingPyramidState_;
  }
  ::rendering::Texture occupancyTexture() const
  {
    return occupancyTexture_;
  }
  uint32_t sourceCapacity() const
  {
    return sourceAABBCapacity_;
  }

private:
  ::rendering::RenderGraph *graph_ = nullptr;
  std::string name_;
  uint64_t baseWordCount_ = 0u;
  uint64_t mipWordCount_ = 0u;
  uint32_t levelCount_ = 0u;
  uint32_t sourceAABBCapacity_ = 1u;
  uint32_t uploadedSourceAABBCount_ = 0u;
  uint64_t sourceRevision_ = 0u;
  std::vector<SparseVoxelOccupancyAABBGPU> cachedSourceAABBs_;
  std::vector<uint64_t> uploadedSourceRevisionByFrame_;
  bool gpuGenerated_ = false;
  bool sceneTraversal_ = false;
  ::rendering::Buffer baseWords_{};
  ::rendering::Buffer dirtyWords_{};
  ::rendering::Buffer denseDirtyMasks_{};
  ::rendering::Buffer dirtyWordIndices_{};
  ::rendering::Buffer dirtyWordCount_{};
  ::rendering::Buffer dirtyDispatchArgs_{};
  ::rendering::Buffer mipWordCounts_{};
  ::rendering::Buffer dirtyQueryBoxes_{};
  ::rendering::Buffer sourceAABBs_{};
  ::rendering::Buffer sceneQueueA_{};
  ::rendering::Buffer sceneQueueB_{};
  ::rendering::Buffer sceneQueueCounts_{};
  ::rendering::Buffer sceneDispatchArgs_{};
  ::rendering::Buffer mipWords_{};
  ::rendering::Buffer pendingBaseState_{};
  ::rendering::Buffer pendingLevelDescriptors_{};
  ::rendering::Buffer pendingPyramidState_{};
  ::rendering::Buffer baseState_{};
  ::rendering::Buffer levelDescriptors_{};
  ::rendering::Buffer pyramidState_{};
  ::rendering::Texture occupancyTexture_{};
  SparseVoxelOccupancyBasePass *classifyBasePass_ = nullptr;
  SparseVoxelOccupancyBasePass *clearBasePass_ = nullptr;
  std::vector<SparseVoxelOccupancyDirtyPass *> sparseDirtyPasses_;
  SparseVoxelOccupancySourceSplatPass *sourceSplatPass_ = nullptr;
  std::vector<SparseVoxelOccupancyScenePass *>
      sceneTraversalPasses_;
  SparseVoxelOccupancyBasePass *splatBasePass_ = nullptr;
  SparseVoxelOccupancyBasePass *publishBasePass_ = nullptr;
  std::vector<::rendering::Pass *> passes_;
  std::vector<SparseVoxelOccupancyMipPass *> mipPasses_;
  SparseVoxelOccupancyPyramidGPU published_{};
};

} // namespace voxel::svraster::rendering
