#pragma once

#include "ResourceDatabase.hpp"
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "RHI.hpp"
#include "Types.hpp"
#include <stack>

#include "datastructure/ConcurrentHashMap.hpp"
#include "datastructure/ConcurrentQueue.hpp"
#include "datastructure/ThreadLocalStorage.hpp"

namespace rendering
{

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

  StartTimer,
  StopTimer,

  // Note, add new dispatch commands in between those enums
  Draw, // Dispatch Begin
  DrawIndexed,
  DrawIndexedIndirect,
  Dispatch, // Dispatch end
};

struct BufferBarrier
{
  std::string resourceId;
  uint64_t offset;
  uint64_t size;
  AccessPattern fromAccess;
  AccessPattern toAccess;
  // uint64_t fromLevel;
  uint64_t toLevel;
  Queue fromQueue;
  Queue toQueue;
  uint64_t fromNode;
};

struct TextureBarrier
{
  std::string resourceId;
  uint64_t toLevel;
  uint64_t baseMip;
  uint64_t mipCount;
  uint64_t baseLayer;
  uint64_t layerCount;
  AccessPattern fromAccess;
  AccessPattern toAccess;
  ResourceLayout fromLayout;
  ResourceLayout toLayout;
  Queue fromQueue;
  Queue toQueue;
  uint64_t fromNode;
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
struct StartTimerArgs
{
  Timer timer;
  PipelineStage stage;
};
struct StopTimerArgs
{
  Timer timer;
  PipelineStage stage;
};

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
    StartTimerArgs *startTimer;
    StopTimerArgs *stopTimer;

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
  std::string resourceId;
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
  Queue queue;
};

struct BindingGroupsResourceUsage
{
  uint64_t consumer;
  Queue queue;
};

struct GraphicsPipelineResourceUsage
{
  uint64_t consumer;
  Queue queue;
};

struct ComputePipelineResourceUsage
{
  uint64_t consumer;
  Queue queue;
};

struct BufferResourceUsage
{
  BufferView view;
  uint64_t consumer;
  Queue queue;
  AccessPattern access;
};

struct TextureResourceUsage
{
  TextureView view;
  uint64_t consumer;
  Queue queue;
  AccessPattern access;
};

struct SamplerResourceUsage
{
  Sampler sampler;
  uint64_t consumer;
  Queue queue;
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

// struct ScratchBufferResourceMetadata
// {
//   BufferInfo bufferInfo;
//   uint64_t firstUsedAt;
//   uint64_t lastUsedAt;
//   //std::vector<BufferResourceUsage> usages;
// };

struct ShaderResourceMetadata
{
  ShaderInfo info;
};

struct BufferResourceMetadata
{
  BufferInfo bufferInfo;
  uint64_t firstUsedAt;
  uint64_t lastUsedAt;
  std::vector<BufferResourceUsage> usages;
};

struct TextureResourceMetadata
{
  TextureInfo textureInfo;
  std::vector<TextureResourceUsage> usages;
};

struct SamplerResourceMetadata
{
  SamplerInfo samplerInfo;
  std::vector<SamplerResourceUsage> usages;
};

struct BindingsLayoutResourceMetadata
{
  BindingsLayoutInfo layoutsInfo;
  std::vector<BindingsLayoutResourceUsage> usages;
};

struct BindingGroupsResourceMetadata
{
  BindingGroupsInfo groupsInfo;
  std::vector<BindingGroupsResourceUsage> usages;
};

struct GraphicsPipelineResourceMetadata
{
  GraphicsPipelineInfo pipelineInfo;
  std::vector<GraphicsPipelineResourceUsage> usages;
};

struct ComputePipelineResourceMetadata
{
  ComputePipelineInfo pipelineInfo;
  std::vector<ComputePipelineResourceUsage> usages;
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
  uint64_t signalTask;
  uint64_t waitTask;
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

class RHICommandBuffer
{
public:
  // const std::string swapChainImageName = "SwapChainImage.textureView";

  RHICommandBuffer();

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
  void cmdStartTimer(const Timer timer, PipelineStage stage);
  void cmdStopTimer(const Timer timer, PipelineStage stage);
};

class RenderGraph;

class RHIResources
{
  friend class RenderGraph;

private:
  RenderGraph *renderGraph;
  lib::ConcurrentHashMap<std::string, ShaderResourceMetadata> shadersMetadatas;
  lib::ConcurrentHashMap<std::string, BufferAllocation> scratchMap;
  lib::ConcurrentHashMap<BufferUsage, BufferResourceMetadata> scratchBuffers;
  lib::ConcurrentHashMap<std::string, BufferResourceMetadata> bufferMetadatas;
  lib::ConcurrentHashMap<std::string, TextureResourceMetadata> textureMetadatas;
  lib::ConcurrentHashMap<std::string, SamplerResourceMetadata> samplerMetadatas;
  lib::ConcurrentHashMap<std::string, BindingsLayoutResourceMetadata> bindingsLayoutMetadata;
  lib::ConcurrentHashMap<std::string, BindingGroupsResourceMetadata> bindingGroupsMetadata;
  lib::ConcurrentHashMap<std::string, GraphicsPipelineResourceMetadata> graphicsPipelineMetadata;
  lib::ConcurrentHashMap<std::string, ComputePipelineResourceMetadata> computePipelineMetadata;

public:
  RHIResources(RenderGraph *renderGraph);

  const BindingGroups getBindingGroups(const std::string &name);
  const GraphicsPipeline getGraphicsPipeline(const std::string &name);
  const ComputePipeline getComputePipeline(const std::string &name);

  const BindingsLayout getBindingsLayout(const std::string &name);
  const Sampler getSampler(const std::string &name);
  const Buffer getBuffer(const std::string &name);
  const Texture getTexture(const std::string &name);
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
    uint64_t dispatchId;
    uint64_t commandBufferIndex;

    std::vector<uint64_t> signalSemaphores;
    std::vector<uint64_t> waitSemaphores;

    Queue queue;
    std::vector<Command> commands;

    std::vector<TextureBarrier> textureTransitions;
    std::vector<BufferBarrier> bufferTransitions;
  };

  struct RenderGraphPass
  {
    std::string name;
    RHICommandBuffer cmd;
  };

  friend class Task;
  bool compiled;
  uint64_t executions;
  RHI *rhi;

  lib::ConcurrentQueue<RenderGraphPass> passes;

  std::vector<RenderGraphNode> nodes;
  std::vector<std::vector<RenderGraphEdge>> edges;

  // Runtime Info
  RHIResources resources;
  std::vector<Semaphore> semaphores;

  uint64_t commandBuffersCount[Queue::QueuesCount];
  // TODO: remove from here
  // std::vector<TextureBarrier> textureTransitions;
  // std::vector<BufferBarrier> bufferTransitions;

  // std::vector<BufferAllocation> bufferAllocations;
  // std::vector<TextureAllocation> textureAllocations;
  // std::vector<SamplerAllocation> samplerAllocations;
  // std::vector<BindingsLayoutAllocation> bindingsLayoutsAllocations;

  void registerConsumer(const std::string &name, const InputResource &res, uint32_t taskId, Queue queue);

  uint32_t levelDFS(uint32_t id, std::vector<bool> &visited, uint32_t level);

  void topologicalSortDFS(uint32_t id, std::vector<bool> &visited, std::stack<uint32_t> &topologicalSort, std::vector<bool> &recstack);
  void tasksTopologicalSort(std::vector<uint32_t> &outTopologicalOrder);

  void analyseTaskLevels();

  void analyseDependencyGraph();
  // void analyseCommands(RHICommandBuffer &recorder);

  void analysePasses();
  void analyseAllocations();
  void analyseBufferStateTransition();
  void analyseTextureStateTransition();
  void analyseStateTransition();
  void analyseTaskInputs();
  void analyseSemaphores();
  void analyseCommandBuffers();
  // void outputCommands(RHIProgram &program);

public:
  struct Frame
  {
    std::vector<GPUFuture> futures;
  };

  inline static bool ExecuteAlways(const RenderGraph &renderGraph)
  {
    return true;
  }

  inline static bool ExecuteOnFirstRun(const RenderGraph &renderGraph)
  {
    return renderGraph.executions == 0;
  }

  RenderGraph(RHI *rhi);

  void enqueuePass(std::string name, RHICommandBuffer &);
  void compile();
  void run(Frame &outFrame);
  void waitFrame(Frame &frame);

  // const Buffer createScratchBuffer(const BufferInfo &info);
  const Timer createTimer(const TimerInfo &info);
  const Buffer createBuffer(const BufferInfo &info);
  const Texture createTexture(const TextureInfo &info);
  const Sampler createSampler(const SamplerInfo &info);
  const BindingsLayout createBindingsLayout(const BindingsLayoutInfo &info);
  const BindingGroups createBindingGroups(const BindingGroupsInfo &info);
  const GraphicsPipeline createGraphicsPipeline(const GraphicsPipelineInfo &info);
  const ComputePipeline createComputePipeline(const ComputePipelineInfo &info);

  void deleteTimer(const Timer &timer);
  void deleteBuffer(const Buffer &name);
  void deleteTexture(const Texture &name);
  void deleteSampler(const Sampler &name);
  void deleteBindingsLayout(const BindingsLayout &name);
  void deleteBindingGroups(const BindingGroups &name);
  void deleteGraphicsPipeline(const GraphicsPipeline &name);
  void deleteComputePipeline(const ComputePipeline &name);

  // const Buffer getScratchBuffer(BufferInfo &info);
  const BindingGroups getBindingGroups(const std::string &name);
  const GraphicsPipeline getGraphicsPipeline(const std::string &name);
  const ComputePipeline getComputePipeline(const std::string &name);

  const BindingsLayout getBindingsLayout(const std::string &name);
  const Sampler getSampler(const std::string &name);
  const Buffer getBuffer(const std::string &name);
  const Texture getTexture(const std::string &name);

  const Shader createShader(const ShaderInfo data);
  void deleteShader(Shader handle);

  void addSwapChainImages(SwapChain);
  void removeSwapChainImages(SwapChain);

  void bufferRead(const Buffer &buffer, const uint64_t offset, const uint64_t size, std::function<void(const void *)>);
  void bufferWrite(const Buffer &buffer, const uint64_t offset, const uint64_t size, void *data);
  double readTimer(const Timer &timer);
};

}; // namespace rendering
