#pragma once

#include "ResourceDatabase.hpp"
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "RHI.hpp"
#include "Types.hpp"
#include <stack>

#include "datastructure/ThreadLocalStorage.hpp"
#include "datastructure/ConcurrentHashMap.hpp"
#include "datastructure/ConcurrentLinkedList.hpp"
#include "datastructure/ConcurrentUnorderedSkipListMap.hpp"

namespace rendering
{

struct BufferInfo
{
  std::string name;
  uint32_t size = 0U;
  BufferUsage usage;
  // bool persistent = true;
};

struct TextureInfo
{
  std::string name;
  Format format = Format::Format_RGBA8Uint;
  BufferUsage memoryProperties;
  ImageUsage usage;
  uint32_t width = 0U;
  uint32_t height = 0U;
  uint32_t depth = 0U;
  uint32_t mipLevels = 0U;
};

struct SamplerInfo
{
  std::string name;
  Filter minFilter = Filter::Linear;
  Filter magFilter = Filter::Linear;
  SamplerAddressMode addressModeU = SamplerAddressMode::Repeat;
  SamplerAddressMode addressModeV = SamplerAddressMode::Repeat;
  SamplerAddressMode addressModeW = SamplerAddressMode::Repeat;
  bool anisotropyEnable = false;
  float maxAnisotropy = 1.0f;
  float maxLod = 1.0f;
};

struct BindingBuffer
{
  BufferView bufferView;
  uint32_t binding;
  bool isDynamic = false;
};

struct BindingSampler
{
  Sampler sampler;
  TextureView view;
  uint32_t binding;
};

struct BindingTextureInfo
{
  TextureView textureView;
  uint32_t binding;
};

struct BindingStorageTextureInfo
{
  TextureView textureView;
  uint32_t binding;
};

struct GroupInfo
{
  std::string name;
  std::vector<BindingBuffer> buffers;
  std::vector<BindingSampler> samplers;
  std::vector<BindingTextureInfo> textures;
  std::vector<BindingStorageTextureInfo> storageTextures;
};

struct BindingGroupsInfo
{
  std::string name;
  BindingsLayout layout;
  std::vector<GroupInfo> groups;
};

struct BindingGroupLayoutBufferEntry
{
  std::string name;
  uint32_t binding;
  BindingVisibility visibility;
  bool isDynamic = false;
};

struct BindingGroupLayoutSamplerEntry
{
  std::string name;
  uint32_t binding;
  BindingVisibility visibility;
};

struct BindingGroupLayoutTextureEntry
{
  std::string name;
  uint32_t binding;
  BindingVisibility visibility;
  bool multisampled = false;
};

struct BindingGroupLayoutStorageTextureEntry
{
  std::string name;
  uint32_t binding;
  BindingVisibility visibility;
};

struct BindingGroupLayout
{
  std::vector<BindingGroupLayoutBufferEntry> buffers;
  std::vector<BindingGroupLayoutSamplerEntry> samplers;
  std::vector<BindingGroupLayoutTextureEntry> textures;
  std::vector<BindingGroupLayoutStorageTextureEntry> storageTextures;
};

struct BindingsLayoutInfo
{
  std::string name;
  std::vector<BindingGroupLayout> groups;
};

struct ColorAttachmentInfo
{
  std::string name;
  TextureView view;
  Color clearValue = {0.0f, 0.0f, 0.0f, 1.0f};
};

struct DepthStencilAttachmentInfo
{
  std::string name;
  TextureView view;
  float clearDepth = 1.0f;
  uint32_t clearStencil = 0U;
};
struct VertexLayoutElement
{
  std::string name;
  Type type;
  uint32_t binding;
  uint32_t offset;
  uint32_t location;
};
struct GraphicsPipelineVertexStage
{
  Shader vertexShader;
  std::string shaderEntry;

  std::vector<VertexLayoutElement> vertexLayoutElements;

  PrimitiveType primitiveType;
  PrimitiveCullType cullType;
};

struct ColorAttatchment
{
  Format format;
  LoadOp loadOp;
  StoreOp storeOp;
};

struct DepthAttatchment
{
  Format format;
  LoadOp loadOp;
  StoreOp storeOp;
};

struct GraphicsPipelineFragmentStage
{
  Shader fragmentShader;
  std::string shaderEntry;
  std::vector<ColorAttatchment> colorAttatchments;
  DepthAttatchment depthAttatchment;
};

struct GraphicsPipelineInfo
{
  std::string name;
  BindingsLayout layout;
  GraphicsPipelineVertexStage vertexStage;
  GraphicsPipelineFragmentStage fragmentStage;
};

struct ComputePipelineInfo
{
  std::string name;
  Shader shader;
  const char *entry;
  BindingsLayout layout;
};

struct RenderPassInfo
{
  std::string name;
  Viewport viewport;
  Rect2D scissor;

  std::vector<ColorAttachmentInfo> colorAttachments;
  std::vector<DepthStencilAttachmentInfo> depthStencilAttachment;
};

enum CommandType
{
  BeginRenderPass,
  EndRenderPass,
  // GetNextSwapChainImage,
  CopyBuffer,
  BindBindingGroups,
  BindGraphicsPipeline,
  BindComputePipeline,
  BindVertexBuffer,
  BindIndexBuffer,
  // Note, add new dispatch commands in between those enums
  Draw, // Dispatch Begin
  DrawIndexed,
  DrawIndexedIndirect,
  Dispatch, // Dispatch end
};

struct BufferBarrier
{
  uint64_t resourceId;
  uint64_t offset;
  uint64_t size;
  AccessPattern fromAccess;
  AccessPattern toAccess;
  // uint64_t fromLevel;
  uint64_t toLevel;
};

struct TextureBarrier
{
  uint64_t resourceId;
  uint64_t toLevel;
  uint64_t baseMip;
  uint64_t mipCount;
  uint64_t baseLayer;
  uint64_t layerCount;
  AccessPattern fromAccess;
  AccessPattern toAccess;
  ResourceLayout fromLayout;
  ResourceLayout toLayout;
};

struct CopyBufferArgs
{
  BufferView src, dst;
};

struct BindGroupsArgs
{
  BindingGroups groups;
  std::vector<uint32_t> dynamicOffsets;
};

struct BindVertexBufferArgs
{
  uint32_t slot;
  BufferView buffer;
};

struct BindIndexBufferArgs
{
  BufferView buffer;
  Type type;
};

struct DrawArgs
{
  uint32_t vertexCount, instanceCount, firstVertex, firstInstance;
};

struct DrawIndexedArgs
{
  uint32_t indexCount, instanceCount, firstIndex, firstInstance;
  uint32_t vertexOffset;
};

struct DrawIndexedIndirectArgs
{
  BufferView buffer;
  uint32_t offset;
  uint32_t drawCount, stride;
};

struct DispatchArgs
{
  uint32_t x, y, z;
};

// struct GetNextSwapChainImageArgs
// {
//   SwapChain swapChain;
// };

struct Command
{
  CommandType type;
  union
  {
    RenderPassInfo *renderPassInfo;
    CopyBufferArgs *copyBuffer;
    BindGroupsArgs *bindGroups;
    GraphicsPipeline *graphicsPipeline;
    ComputePipeline *computePipeline;
    BindVertexBufferArgs *bindVertexBuffer;
    BindIndexBufferArgs *bindIndexBuffer;
    DrawArgs *draw;
    DrawIndexedArgs *drawIndexed;
    DrawIndexedIndirectArgs *drawIndexedIndirect;
    DispatchArgs *dispatch;
    // GetNextSwapChainImageArgs *getNextSwapChainImageArgs;
  } args;
};

enum ResourceType
{
  ResourceType_Initialization = 0,
  ResourceType_Buffer,
  ResourceType_BufferView,
  ResourceType_Texture,
  ResourceType_TextureView,
  ResourceType_Sampler,
  ResourceType_Shader,
  ResourceType_BindingsLayout,
  ResourceType_BindingGroups,
  ResourceType_ComputePipeline,
  ResourceType_GraphicsPipeline,
  ResourceType_ResourcesCount,
};

enum DependencyType
{
  DependencyType_ResouceStateChange,
  DependencyType_ResouceWrite,
};

enum EdgeType
{
  ResourceDependency,
  ResourceShare,
  Initialization,
};

struct RenderGraphEdge
{
  uint64_t taskId;
  EdgeType type;

  ResourceType resourceType;
  uint64_t resourceId;
};

// struct Surface
// {
//   uint64_t id;
// };

// struct SwapChain
// {
//   std::string name;
// };

// struct SwapChainInfo
// {
//   std::string name;
//   Surface surface;
//   uint64_t width;
//   uint64_t height;
// };

struct ConsumerInfo
{
  uint32_t taskId;
  // ResourceUsage usage;
  ResourceLayout layout;
  AccessPattern access;

  bool operator==(const ConsumerInfo &other) const noexcept
  {
    return taskId == other.taskId && layout == other.layout && access == other.access;
  }
};

struct ConsumerInfoHash
{
  std::size_t operator()(const ConsumerInfo &ci) const noexcept
  {
    return std::hash<uint32_t>()(ci.taskId) ^ (std::hash<int>()((int)ci.layout) << 1) ^ (std::hash<int>()((int)ci.access) << 2);
  }
};

class Task;

struct BindingsLayoutResourceUsage
{
  uint64_t consumer;
};

struct BindingGroupsResourceUsage
{
  uint64_t consumer;
};

struct GraphicsPipelineResourceUsage
{
  uint64_t consumer;
};

struct ComputePipelineResourceUsage
{
  uint64_t consumer;
};

struct BufferResourceUsage
{
  BufferView view;
  uint64_t consumer;
};

struct TextureResourceUsage
{
  TextureView view;
  uint64_t consumer;
};

struct SamplerResourceUsage
{
  Sampler sampler;
  uint64_t consumer;
};

// struct ResourceMetadata
// {
//   ResourceType type;
//   std::string name;
//   uint32_t producer;

//   BufferInfo bufferInfo;
//   TextureInfo textureInfo;
//   SamplerInfo samplerInfo;
//   BindingsLayoutInfo layoutsInfo;
//   uint32_t firstUsedAt;
//   uint32_t lastUsedAt;

//   AccessPattern initialAccess;
//   ResourceLayout initialLayout;

//   std::vector<BufferResourceUsage> bufferUsages;
//   std::vector<TextureResourceUsage> textureUsages;
//   std::vector<SamplerResourceUsage> samplerUsages;
//   std::vector<BindingsLayoutResourceUsage> layoutUsages;
// };

struct ScratchBufferResourceMetadata
{
  uint64_t id;
  BufferInfo bufferInfo;
  uint64_t firstUsedAt;
  uint64_t lastUsedAt;
  std::vector<BufferResourceUsage> bufferUsages;
};

struct BufferResourceMetadata
{
  uint64_t id;
  BufferInfo bufferInfo;
  std::vector<BufferResourceUsage> bufferUsages;
};

struct TextureResourceMetadata
{
  uint64_t id;
  TextureInfo textureInfo;
  std::vector<TextureResourceUsage> textureUsages;
};

struct SamplerResourceMetadata
{
  uint64_t id;
  SamplerInfo samplerInfo;
  std::vector<SamplerResourceUsage> samplerUsages;
};

struct BindingsLayoutResourceMetadata
{
  uint64_t id;
  BindingsLayoutInfo layoutsInfo;
  std::vector<BindingsLayoutResourceUsage> layoutUsages;
};

struct BindingGroupsResourceMetadata
{
  uint64_t id;
  BindingGroupsInfo groupsInfo;
  std::vector<BindingGroupsResourceUsage> layoutUsages;
};

struct GraphicsPipelineResourceMetadata
{
  uint64_t id;
  GraphicsPipelineInfo pipelineInfo;
  std::vector<GraphicsPipelineResourceUsage> layoutUsages;
};

struct ComputePipelineResourceMetadata
{
  uint64_t id;
  ComputePipelineInfo pipelineInfo;
  std::vector<ComputePipelineResourceUsage> layoutUsages;
};

struct OutputResource
{
  ResourceType type;

  BufferInfo bufferInfo;
  TextureInfo textureInfo;
  SamplerInfo samplerInfo;
  BindingsLayoutInfo bindingsLayoutsInfo;

  ResourceLayout layout;
  AccessPattern access;
};

struct InputResource
{
  // uint64_t id;
  ResourceType type;
  BufferView bufferView;
  TextureView textureView;
  ComputePipeline computePipeline;
  GraphicsPipeline graphicsPipeline;
  BindingGroups bindingGroups;
  BindingsLayout bindingLayout;
  Sampler sampler;
  AccessPattern access;
  ResourceLayout layout;
};

std::string toString(Queue queue);

struct Semaphore
{
  Queue signalQueue;
  Queue waitQueue;
  uint32_t signalTask;
  uint32_t waitTask;
  bool operator==(const Semaphore &other) const noexcept
  {
    return signalQueue == other.signalQueue && waitQueue == other.waitQueue && signalTask == other.signalTask && waitTask == other.waitTask;
  }
};

// struct BufferAccess
// {
//   size_t offset;
//   size_t size;
//   uint32_t bufferAllocationId;
// };

// struct TextureAccess
// {
//   uint32_t textureAllocationId;
// };

// struct SamplerAccess
// {
//   uint32_t samplerAllocationId;
// };

struct BufferAllocation
{
  BufferUsage usage;
  uint64_t offset;
  uint64_t size;
};

class CommandRecorder
{
public:
  // const std::string swapChainImageName = "SwapChainImage.textureView";

  CommandRecorder();

  struct OutputResource
  {
    ResourceType type;
    SamplerInfo sampler;
    BufferInfo buffer;
    TextureInfo texture;
    BindingsLayoutInfo bindingLayout;
    AccessPattern access;
    ResourceLayout layout;
  };

  struct CommandSequence
  {
    // std::vector<OutputResource> outputResources;
    std::vector<Command> commands;
    // std::vector<uint64_t> buffersAllocated;
    // std::vector<uint64_t> texturesAllocated;
    // std::vector<uint64_t> samplersAllocated;
    // std::vector<uint64_t> bindingLayoutsAllocated;
  };

  std::vector<CommandSequence> recorded;

  // const SwapChain createSwapChain(const SwapChainInfo &info);

  void cmdBeginRenderPass(const RenderPassInfo &info);
  void cmdEndRenderPass();
  void cmdCopyBuffer(BufferView src, BufferView dst);
  void cmdBindBindingGroups(BindingGroups groups, uint32_t *dynamicOffsets, uint32_t dynamicOffsetsCount);
  void cmdBindGraphicsPipeline(GraphicsPipeline);
  void cmdBindComputePipeline(ComputePipeline);
  void cmdBindVertexBuffer(uint32_t slot, BufferView);
  void cmdBindIndexBuffer(BufferView, Type type);
  void cmdDraw(uint32_t vertexCount, uint32_t instanceCount = 1U, uint32_t firstVertex = 0U, uint32_t firstInstance = 0U);
  void cmdDrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1U, uint32_t firstIndex = 0U, uint32_t vertexOffset = 0U, uint32_t firstInstance = 0U);
  void cmdDrawIndexedIndirect(BufferView indirectBuffer, uint32_t offset, uint32_t drawCount, uint32_t stride);
  void cmdDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ);
};

class RenderGraph;

class ResourceDatabase
{
  friend class RenderGraph;

private:
  RenderGraph *renderGraph;

  std::atomic<uint64_t> scratchBuffersAllocated;
  std::atomic<uint64_t> buffersAllocated;
  std::atomic<uint64_t> texturesAllocated;
  std::atomic<uint64_t> samplersAllocated;
  std::atomic<uint64_t> bindingLayoutsAllocated;
  std::atomic<uint64_t> bindingGroupsAllocated;
  std::atomic<uint64_t> graphicsPipelinesAllocated;
  std::atomic<uint64_t> computePipelinesAllocated;

  lib::ConcurrentHashMap<std::string, uint64_t> scratchBufferSymbols;
  lib::ConcurrentHashMap<std::string, uint64_t> bufferSymbols;
  lib::ConcurrentHashMap<std::string, uint64_t> textureSymbols;
  lib::ConcurrentHashMap<std::string, uint64_t> samplerSymbols;
  lib::ConcurrentHashMap<std::string, uint64_t> bindingLayoutSymbols;
  lib::ConcurrentHashMap<std::string, uint64_t> bindingGroupsSymbols;
  lib::ConcurrentHashMap<std::string, uint64_t> graphicsPipelineSymbols;
  lib::ConcurrentHashMap<std::string, uint64_t> computePipelineSymbols;

  // Request id -> (usage, offset, size)
  std::vector<BufferAllocation> scratchMap;
  // Usage -> metadata
  lib::ConcurrentHashMap<BufferUsage, BufferResourceMetadata> scratchBuffers;

  std::vector<ScratchBufferResourceMetadata> scratchBuffersRequestsMetadatas;
  std::vector<BufferResourceMetadata> bufferMetadatas;
  std::vector<TextureResourceMetadata> textureMetadatas;
  std::vector<SamplerResourceMetadata> samplerMetadatas;
  std::vector<BindingsLayoutResourceMetadata> bindingsLayoutMetadata;
  std::vector<BindingGroupsResourceMetadata> bindingGroupsMetadata;
  std::vector<GraphicsPipelineResourceMetadata> graphicsPipelineMetadata;
  std::vector<ComputePipelineResourceMetadata> computePipelineMetadata;

public:
  ResourceDatabase(RenderGraph *renderGraph);

  const Buffer getScratchBuffer(BufferInfo &info);
  const BindingGroups getBindingGroups(const std::string &name);
  const GraphicsPipeline getGraphicsPipeline(const std::string &name);
  const ComputePipeline getComputePipeline(const std::string &name);

  const BindingsLayout getBindingsLayout(const std::string &name);
  const Sampler getSampler(const std::string &name);
  const Buffer getBuffer(const std::string &name);
  const Buffer getScratchBuffer(const std::string &name);
  const Texture getTexture(const std::string &name);
  // const TextureView getNextSwapChainTexture(const SwapChain &swapChain) const;
};

class RenderGraph
{
private:
  struct RenderGraphNode
  {
    std::string name;
    uint64_t id;
    uint64_t level;
    uint64_t priority;

    std::vector<uint32_t> signalSemaphores;
    std::vector<uint32_t> waitSemaphores;

    Queue queue;
    std::vector<Command> commands;
  };

  struct RenderGraphPass
  {
    std::string name;
    std::function<bool(const RenderGraph &)> shouldExecute;
    std::function<void(ResourceDatabase &, CommandRecorder &)> record;
  };

  friend class Task;
  bool compiled;
  uint64_t executions;
  RHI *renderingHardwareInterface;

  lib::ConcurrentHashMap<std::string, RenderGraphPass> passes;

  std::vector<RenderGraphNode> nodes;
  std::vector<std::vector<RenderGraphEdge>> edges;

  // Runtime Info
  ResourceDatabase resources;
  std::vector<Semaphore> semaphores;

  // TODO: remove from here
  std::vector<TextureBarrier> textureTransitions;
  std::vector<BufferBarrier> bufferTransitions;

  // std::vector<BufferAllocation> bufferAllocations;
  // std::vector<TextureAllocation> textureAllocations;
  // std::vector<SamplerAllocation> samplerAllocations;
  // std::vector<BindingsLayoutAllocation> bindingsLayoutsAllocations;

  lib::ThreadLocalStorage<CommandRecorder **> currentRecorder;

  void registerConsumer(const std::string &name, const InputResource &res, uint32_t taskId);

  uint32_t levelDFS(uint32_t id, std::vector<bool> &visited, uint32_t level);

  void topologicalSortDFS(uint32_t id, std::vector<bool> &visited, std::stack<uint32_t> &topologicalSort, std::vector<bool> &recstack);
  void tasksTopologicalSort(std::vector<uint32_t> &outTopologicalOrder);

  void analyseTaskLevels();

  void analyseDependencyGraph();
  void analyseCommands(CommandRecorder &recorder);

  void analysePasses();
  void analyseAllocations();
  void analyseBufferStateTransition();
  void analyseTextureStateTransition();
  void analyseStateTransition();
  void analyseTaskInputs();
  void analyseSemaphores();
  // void outputCommands(RHIProgram &program);

public:
  inline static bool ExecuteAlways(const RenderGraph &renderGraph)
  {
    return true;
  }

  inline static bool ExecuteOnFirstRun(const RenderGraph &renderGraph)
  {
    return renderGraph.executions == 0;
  }

  RenderGraph(RHI *rhi);

  void addPass(std::string name, std::function<bool(const RenderGraph &)> shouldExecute, std::function<void(ResourceDatabase &, CommandRecorder &)> handler);
  void compile();

  const Buffer createScratchBuffer(const BufferInfo &info);

  const Buffer createBuffer(const BufferInfo &info);
  const Texture createTexture(const TextureInfo &info);
  const Sampler createSampler(const SamplerInfo &info);
  const BindingsLayout createBindingsLayout(const BindingsLayoutInfo &info);
  const BindingGroups createBindingGroups(const BindingGroupsInfo &info);
  const GraphicsPipeline createGraphicsPipeline(const GraphicsPipelineInfo &info);
  const ComputePipeline createComputePipeline(const ComputePipelineInfo &info);
};

}; // namespace rendering
