#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "virtualshadowmap/VirtualShadowMapManager.hpp"

#include <algorithm>

namespace virtualgeometry
{
namespace gpgpu
{

class VirtualShadowMapBookkeepingPass : public rendering::Pass
{
public:
  struct Settings
  {
    uint32_t depthTextureWidth = 1u;
    uint32_t depthTextureHeight = 1u;
    uint32_t workgroupSizeX = 8u;
    uint32_t workgroupSizeY = 8u;
    uint32_t shadowFilterTaps = 1u;
    int32_t projectedLightIndex = 0;
    float pcfRadiusTexels = 0.0f;
    float localLightMipTransitionScale = 2.0f;
    float localLightMipBias = 0.0f;
    bool debugOutputEnabled = false;
    bool enableLightTiles = true;
    // Opt in after measuring: the parallel light loop wins the current M4 sweep.
    bool enableLightBVH = false;
    uint32_t lightBVHMinLocalLights = 32u;
  };

  explicit VirtualShadowMapBookkeepingPass(VirtualShadowMapManager &manager, rendering::Texture depthTexture, rendering::Texture outputTexture)
      : VirtualShadowMapBookkeepingPass(manager, depthTexture, outputTexture, Settings{})
  {
  }

  VirtualShadowMapBookkeepingPass(VirtualShadowMapManager &manager, rendering::Texture depthTexture, rendering::Texture outputTexture, Settings settings)
      : manager_(manager), depthTexture_(depthTexture), outputTexture_(outputTexture), settings_(settings)
  {
  }

  ~VirtualShadowMapBookkeepingPass() override
  {
    renderGraph->deleteComputePipeline(rankRequestsPipeline_);
    renderGraph->deleteComputePipeline(prefixRequestsPipeline_);
    renderGraph->deleteComputePipeline(allocateCoarsePipeline_);
    renderGraph->deleteBuffer(requestRanks_);
    renderGraph->deleteBuffer(requestBlocks_);
    renderGraph->deleteComputePipeline(lightTilesPipeline_);
    renderGraph->deleteBuffer(lightTilesBuffer_);
    renderGraph->deleteComputePipeline(initAllocatorPipeline_);
    renderGraph->deleteComputePipeline(bookkeepingPipeline_);
    renderGraph->deleteComputePipeline(analyzeVisiblePagesPipeline_);
    renderGraph->deleteComputePipeline(emitPageRequestsPipeline_);
    renderGraph->deleteComputePipeline(allocatePagesPipeline_);
    renderGraph->deleteShader(shader_);
    renderGraph->deleteBindingGroups(bindingGroups_);
    renderGraph->deleteBindingsLayout(layout_);
    renderGraph->deleteBuffer(uniformBuffer_);
  }

  void setDebugOutputEnabled(bool enabled)
  {
    debugOutputEnabled_ = enabled;
    if (uniformBuffer_.name.size())
    {
      writeUniforms();
    }
  }

  void setSettings(const Settings &settings)
  {
    settings_ = settings;
    if (uniformBuffer_.name.size())
    {
      writeUniforms();
    }
  }

  const rendering::Buffer &getLightTilesBuffer() const { return lightTilesBuffer_; }

  void recordCommandBuffer(rendering::CommandRecorder &commandBuffer) override
  {
    uniformBuffer_ = createFrameLocalBuffer(
        rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(Uniforms),
            .usage = rendering::BufferUsage::BufferUsage_Uniform | rendering::BufferUsage::BufferUsage_Push,
        });

    const auto &config = manager_.getSettings();
    tileWords_ = std::max(1u,(config.maxDirectionalLights+config.maxSpotLights+config.maxPointLights+31u)/32u);
    tileCountX_ = (settings_.depthTextureWidth+15u)/16u;
    tileCountY_ = (settings_.depthTextureHeight+15u)/16u;
    lightTilesBuffer_ = createFrameLocalBuffer(rendering::BufferInfo{
        .name=passName+"_LightTiles.buffer", .size=16ull+uint64_t(tileCountX_)*tileCountY_*tileWords_*4u,
        .usage=rendering::BufferUsage::BufferUsage_Storage|rendering::BufferUsage::BufferUsage_CopySrc});
    const uint64_t pages=uint64_t(manager_.getActiveLayerCount())*manager_.getVirtualPageTableResolution()*manager_.getVirtualPageTableResolution();
    requestBlockCount_=std::max(1u,uint32_t((pages+255)/256));
    requestRanks_=createFrameLocalBuffer(rendering::BufferInfo{.name=passName+"_RequestRanks",.size=uint64_t(requestBlockCount_)*256*8,.usage=rendering::BufferUsage::BufferUsage_Storage});
    requestBlocks_=createFrameLocalBuffer(rendering::BufferInfo{.name=passName+"_RequestBlocks",.size=uint64_t(requestBlockCount_+(requestBlockCount_+255)/256)*8,.usage=rendering::BufferUsage::BufferUsage_Storage});
    writeUniforms();

    layout_ = renderGraph->createBindingsLayout(
        rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {rendering::BindingGroupLayout{
                .buffers =
                    {
                        {.name = "uniforms", .binding = 0, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "invalidationMasks", .binding = 1, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeStates", .binding = 2, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cascadeMatrices", .binding = 3, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "cameraState", .binding = 4, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "virtualPageTable", .binding = 6, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "physicalPageTable", .binding = 7, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "allocatorCounters", .binding = 8, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "allocationRequests", .binding = 9, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "futureAllocationRequests", .binding = 10, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "unallocatedPhysicalPages", .binding = 11, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "virtualPageState", .binding = 13, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "directionalLights", .binding = 14, .isDynamic = false, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "lightBVH", .binding = 18, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "requestRanks", .binding = 16, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "requestBlocks", .binding = 17, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                        {.name = "lightTiles", .binding = 15, .type = rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                    },
                .textures = {
                    {.name = "depthTexture", .binding = 5, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
                .storageTextures = {
                    {.name = "outputTexture", .binding = 12, .visibility = rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    bindingGroups_ = renderGraph->createBindingGroups(
        rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_BindingGroups",
            .groups = {
                rendering::GroupInfo{
                    .name = "group0",
                    .buffers = {
                        {.binding = 0, .bufferView = {.buffer = uniformBuffer_, .offset = 0, .size = sizeof(Uniforms), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 1, .bufferView = {.buffer = manager_.getInvalidationMaskBuffer(), .offset = 0, .size = manager_.getInvalidationMaskBufferSize(), .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 2, .bufferView = {.buffer = manager_.getCascadeStatesBuffer(), .offset = 0, .size = manager_.getCascadeStatesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 3, .bufferView = {.buffer = manager_.getCascadeMatricesBuffer(), .offset = 0, .size = manager_.getCascadeMatricesBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 4, .bufferView = {.buffer = manager_.getCameraStateBuffer(), .offset = 0, .size = manager_.getCameraStateBufferSize(), .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding = 6,
                         .bufferView = {.buffer = manager_.getVirtualPageTableBuffer(),
                                        .offset = 0,
                                        .size = manager_.getVirtualPageTableBufferSize(),
                                        .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 7,
                         .bufferView = {.buffer = manager_.getPhysicalPageTableBuffer(),
                                        .offset = 0,
                                        .size = manager_.getPhysicalPageTableBufferSize(),
                                        .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 8,
                         .bufferView = {.buffer = manager_.getAllocatorCountersBuffer(),
                                        .offset = 0,
                                        .size = manager_.getAllocatorCountersBufferSize(),
                                        .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 9,
                         .bufferView = {.buffer = manager_.getAllocationRequestsBuffer(),
                                        .offset = 0,
                                        .size = manager_.getAllocationRequestsBufferSize(),
                                        .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 10,
                         .bufferView = {.buffer = manager_.getFutureAllocationRequestsBuffer(),
                                        .offset = 0,
                                        .size = manager_.getFutureAllocationRequestsBufferSize(),
                                        .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 11,
                         .bufferView = {.buffer = manager_.getUnallocatedPhysicalPagesBuffer(),
                                        .offset = 0,
                                        .size = manager_.getUnallocatedPhysicalPagesBufferSize(),
                                        .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 13,
                         .bufferView = {.buffer = manager_.getVirtualPageStateBuffer(),
                                        .offset = 0,
                                        .size = manager_.getVirtualPageStateBufferSize(),
                                        .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE}},
                        {.binding = 14,
                         .bufferView = {.buffer = manager_.getDirectionalLightsBuffer(),
                                        .offset = 0,
                                        .size = manager_.getDirectionalLightsBufferSize(),
                                        .access = rendering::AccessPattern::SHADER_READ}},
                        {.binding=18,.bufferView={.buffer=manager_.getLightBVHBuffer(),.access=rendering::AccessPattern::SHADER_READ,.offset=0,.size=manager_.getLightBVHBufferSize()}},
                        {.binding=16,.bufferView={.buffer=requestRanks_,.access=rendering::AccessPattern::SHADER_READ_WRITE,.offset=0,.size=renderGraph->getBufferSize(requestRanks_)}},
                        {.binding=17,.bufferView={.buffer=requestBlocks_,.access=rendering::AccessPattern::SHADER_READ_WRITE,.offset=0,.size=renderGraph->getBufferSize(requestBlocks_)}},
                        {.binding=15, .bufferView={.buffer=lightTilesBuffer_, .access=rendering::AccessPattern::SHADER_READ_WRITE,
                            .offset=0,.size=renderGraph->getBufferSize(lightTilesBuffer_)}},
                    },
                    .textures = {
                        {.binding = 5, .textureView = makeDepthTextureView(depthTexture_)},
                    },
                    .storageTextures = {
                        {.binding = 12, .textureView = makeStorageColorTextureView(outputTexture_)},
                    },
                },
            },
        });

    shader_ = renderGraph->createShader(
        rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/vsm-bookkeeping.spirv"),
            .type = rendering::ShaderType::SpirV,
        });

    rankRequestsPipeline_=renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry="rank_page_requests_main",.layout=layout_,.name=passName+"_RankRequests",.shader=shader_});
    prefixRequestsPipeline_=renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry="prefix_request_blocks_main",.layout=layout_,.name=passName+"_PrefixRequests",.shader=shader_});
    allocateCoarsePipeline_=renderGraph->createComputePipeline(rendering::ComputePipelineInfo{.entry="allocate_coarse_pages_main",.layout=layout_,.name=passName+"_AllocateCoarse",.shader=shader_});
    lightTilesPipeline_ = renderGraph->createComputePipeline(rendering::ComputePipelineInfo{
        .entry="build_light_tiles_main", .layout=layout_, .name=passName+"_LightTiles.pipeline", .shader=shader_});
    initAllocatorPipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{.entry = "init_allocator_main", .layout = layout_, .name = passName + "_InitAllocator.pipeline", .shader = shader_});
    bookkeepingPipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{.entry = "bookkeeping_main", .layout = layout_, .name = passName + "_Bookkeeping.pipeline", .shader = shader_});
    analyzeVisiblePagesPipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{.entry = "analyze_visible_pages_main", .layout = layout_, .name = passName + "_AnalyzeVisible.pipeline", .shader = shader_});
    emitPageRequestsPipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{.entry = "emit_page_requests_main", .layout = layout_, .name = passName + "_EmitPageRequests.pipeline", .shader = shader_});
    allocatePagesPipeline_ = renderGraph->createComputePipeline(
        rendering::ComputePipelineInfo{.entry = "allocate_pages_main", .layout = layout_, .name = passName + "_AllocatePages.pipeline", .shader = shader_});
    const uint32_t workgroupsX = std::max(1u, (manager_.getVirtualPageTableResolution() + settings_.workgroupSizeX - 1u) / settings_.workgroupSizeX);
    const uint32_t workgroupsY = std::max(1u, (manager_.getVirtualPageTableResolution() + settings_.workgroupSizeY - 1u) / settings_.workgroupSizeY);
    const uint32_t workgroupsZ = std::max(1u, manager_.getActiveLayerCount());
    const uint32_t depthDispatchX = std::max(1u, (settings_.depthTextureWidth + settings_.workgroupSizeX - 1u) / settings_.workgroupSizeX);
    const uint32_t depthDispatchY = std::max(1u, (settings_.depthTextureHeight + settings_.workgroupSizeY - 1u) / settings_.workgroupSizeY);

    commandBuffer.cmdBindComputePipeline(initAllocatorPipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(1u, 1u, 1u);

    commandBuffer.cmdBindComputePipeline(bookkeepingPipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(workgroupsX, workgroupsY, workgroupsZ);

    commandBuffer.cmdBindComputePipeline(lightTilesPipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(std::max(tileCountX_,1u),std::max(tileCountY_,1u),1u);
    commandBuffer.cmdMemoryBarrier(rendering::PipelineStage::COMPUTE_SHADER,rendering::PipelineStage::COMPUTE_SHADER,
        rendering::AccessPattern::SHADER_WRITE,rendering::AccessPattern::SHADER_READ_WRITE);
    commandBuffer.cmdBindComputePipeline(analyzeVisiblePagesPipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0);
    commandBuffer.cmdDispatch(depthDispatchX, depthDispatchY, 1u);

    auto barrier=[&] { commandBuffer.cmdMemoryBarrier(rendering::PipelineStage::COMPUTE_SHADER,rendering::PipelineStage::COMPUTE_SHADER,
        rendering::AccessPattern::SHADER_WRITE,rendering::AccessPattern::SHADER_READ_WRITE); };
    barrier();
    auto dispatch=[&](rendering::ComputePipeline pipeline,uint32_t count) {
      commandBuffer.cmdBindComputePipeline(pipeline); commandBuffer.cmdBindBindingGroups(bindingGroups_,nullptr,0);
      commandBuffer.cmdDispatch(count,1,1); barrier();
    };
    dispatch(rankRequestsPipeline_,requestBlockCount_);
    dispatch(prefixRequestsPipeline_,(requestBlockCount_+255)/256);
    dispatch(emitPageRequestsPipeline_,requestBlockCount_);
    dispatch(allocateCoarsePipeline_,std::max(1u,(manager_.getFutureAllocationRequestCapacity()+63)/64));
    dispatch(allocatePagesPipeline_,std::max(1u,(manager_.getAllocationRequestCapacity()+63)/64));

  }

private:
  struct alignas(16) Uniforms
  {
    uint32_t pageTableResolution = 0u;
    uint32_t physicalPageTableResolution = 0u;
    uint32_t activeLayers = 0u;
    uint32_t maskWordsPerLayer = 0u;
    uint32_t requestCapacity = 0u;
    uint32_t futureRequestCapacity = 0u;
    uint32_t cascadeCount = 0u;
    uint32_t activeLights = 0u;
    uint32_t debugOutputEnabled = 0u;
    uint32_t fallbackCascadeOffset = 0u;
    uint32_t physicalPageSize = 0u;
    uint32_t shadowFilterTaps = 1u;
    int32_t projectedLightIndex = 0;
    float pcfRadiusTexels = 0.0f;
    float localLightMipTransitionScale = 2.0f;
    float localLightMipBias = 0.0f;
    float localLightPageWorldScale = 1.0f;
    float firstCascadeWorldExtent = 0.0f;
    uint32_t tileCountX=0, tileCountY=0, lightTileWords=0, lightTilesEnabled=0;
    uint32_t maxPageUpdates=0, requestBlockCount=0;
    uint32_t lightBVHEnabled=0, lightBVHMinLocalLights=32, padding[2]{};
  };

  static_assert(sizeof(Uniforms)==112u,"Bookkeeping uniform ABI changed");

  void writeUniforms()
  {
    const Uniforms uniforms{
      .pageTableResolution = manager_.getVirtualPageTableResolution(),
      .physicalPageTableResolution = manager_.getPhysicalPageTableResolution(),
      .activeLayers = manager_.getActiveLayerCount(),
      .maskWordsPerLayer = VirtualShadowMapManager::INVALIDATION_MASK_WORDS_PER_LAYER,
      .requestCapacity = manager_.getAllocationRequestCapacity(),
      .futureRequestCapacity = manager_.getFutureAllocationRequestCapacity(),
      .cascadeCount = manager_.getCascadeCount(),
      .activeLights = manager_.getActiveLightCount(),
      .debugOutputEnabled = debugOutputEnabled_ ? 1u : 0u,
      .fallbackCascadeOffset = manager_.getFallbackCascadeOffset(),
      .physicalPageSize = manager_.getPhysicalPageResolution(),
      .shadowFilterTaps = settings_.shadowFilterTaps,
      .projectedLightIndex = settings_.projectedLightIndex,
      .pcfRadiusTexels = settings_.pcfRadiusTexels,
      .localLightMipTransitionScale = std::max(settings_.localLightMipTransitionScale, 1e-4f),
      .localLightMipBias = settings_.localLightMipBias,
      .localLightPageWorldScale = std::max(manager_.getSettings().pageWorldScale, 1e-4f),
      .firstCascadeWorldExtent = manager_.getFirstCascadeWorldExtent(),
      .tileCountX=tileCountX_, .tileCountY=tileCountY_, .lightTileWords=tileWords_, .lightTilesEnabled=settings_.enableLightTiles ? 1u : 0u,
      .maxPageUpdates=manager_.getSettings().maxPageUpdatesPerFrame,.requestBlockCount=requestBlockCount_,
      .lightBVHEnabled=settings_.enableLightBVH ? 1u : 0u, .lightBVHMinLocalLights=settings_.lightBVHMinLocalLights,
    };
    renderGraph->bufferWrite(uniformBuffer_, 0, sizeof(Uniforms), const_cast<Uniforms *>(&uniforms));
  }

  static rendering::TextureView makeDepthTextureView(rendering::Texture texture)
  {
    return rendering::TextureView{
      .texture = texture,
      .access = rendering::AccessPattern::SHADER_READ,
      .layout = rendering::ResourceLayout::SHADER_READ_ONLY,
      .index = 0,
      .flags = rendering::ImageAspectFlags::Depth,
      .baseArrayLayer = 0,
      .baseMipLevel = 0,
      .layerCount = 1,
      .levelCount = 1,
    };
  }

  static rendering::TextureView makeStorageColorTextureView(rendering::Texture texture)
  {
    return rendering::TextureView{
      .texture = texture,
      .access = rendering::AccessPattern::SHADER_READ | rendering::AccessPattern::SHADER_WRITE,
      .layout = rendering::ResourceLayout::GENERAL,
      .index = 0,
      .flags = rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0,
      .baseMipLevel = 0,
      .layerCount = 1,
      .levelCount = 1,
    };
  }

  VirtualShadowMapManager &manager_;
  rendering::Texture depthTexture_;
  rendering::Texture outputTexture_;
  Settings settings_;
  bool debugOutputEnabled_ = settings_.debugOutputEnabled;

  rendering::BindingsLayout layout_;
  rendering::BindingGroups bindingGroups_;
  rendering::Buffer uniformBuffer_;
  rendering::Shader shader_;
  rendering::Buffer requestRanks_,requestBlocks_;
  uint32_t requestBlockCount_=0;
  rendering::ComputePipeline rankRequestsPipeline_,prefixRequestsPipeline_,allocateCoarsePipeline_;
  rendering::Buffer lightTilesBuffer_;
  rendering::ComputePipeline lightTilesPipeline_;
  uint32_t tileCountX_=0, tileCountY_=0, tileWords_=0;
  rendering::ComputePipeline initAllocatorPipeline_;
  rendering::ComputePipeline bookkeepingPipeline_;
  rendering::ComputePipeline analyzeVisiblePagesPipeline_;
  rendering::ComputePipeline emitPageRequestsPipeline_;
  rendering::ComputePipeline allocatePagesPipeline_;
};

} // namespace gpgpu
} // namespace virtualgeometry
