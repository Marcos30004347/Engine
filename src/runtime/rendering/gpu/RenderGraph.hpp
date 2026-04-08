#pragma once

#include "commands/Command.hpp"
#include "runtime/RenderGraphRuntimeCallbacks.hpp"
#include "runtime/RenderGraphRuntimeResourcesManager.hpp"
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "RHI.hpp"
#include "Types.hpp"
#include <limits>
#include <stack>
#include <type_traits>
#include <typeindex>

#include "datastructure/ConcurrentHashMap.hpp"
#include "datastructure/ConcurrentQueue.hpp"
#include "datastructure/ThreadLocalStorage.hpp"
#include "time/TimeSpan.hpp"

namespace rendering
{

class RenderGraph;
class RenderGraphCompiler;
class PassesAnalysis;
class DependencyGraphAnalysis;
class TaskLevelsAnalysis;
class AllocationsAnalysis;
class SemaphoresAnalysis;
class RenderGraphBarrierDispatcher;
class RenderGraphBufferBarrierDispatcher;
class RenderGraphTextureBarrierDispatcher;
class RenderGraphCommandBufferDispatcher;

struct BufferBarrier
{
  enum class Phase : uint8_t
  {
    Normal = 0,
    QueueTransferAcquire,
    QueueTransferRelease,
  };

  uint32_t runtimeId;
  uint64_t offset;
  uint64_t size;
  AccessPattern fromAccess;
  AccessPattern toAccess;
  // uint64_t fromLevel;
  uint64_t toLevel;
  Queue fromQueue;
  Queue toQueue;
  uint64_t fromNode;
  Phase phase = Phase::Normal;
};

struct TextureBarrier
{
  enum class Phase : uint8_t
  {
    Normal = 0,
    QueueTransferAcquire,
    QueueTransferRelease,
  };

  uint32_t runtimeId;
  Format format; // from textureInfo.format, set at compile time for aspect flag computation
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
  Phase phase = Phase::Normal;
};

// Compile-time record: which (BindingGroups, groupIndex, bindingIndex) reference a given buffer name
struct BindingGroupBufferRef
{
  uint32_t bgRuntimeId;
  uint32_t groupIndex;
  uint32_t bindingIndex;
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

  // ResourceType resourceType;
  // std::string resourceId;
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
  ShaderId resourceId = ShaderId::Invalid;
};

struct BufferResourceMetadata
{
  BufferInfo bufferInfo;
  BufferId resourceId = BufferId::Invalid;
  uint64_t firstUsedAt;
  uint64_t lastUsedAt;
  std::vector<BufferResourceUsage> usages;
};

struct TextureResourceMetadata
{
  TextureInfo textureInfo;
  TextureId resourceId = TextureId::Invalid;
  std::vector<TextureResourceUsage> usages;
};

struct SamplerResourceMetadata
{
  SamplerInfo samplerInfo;
  SamplerId resourceId = SamplerId::Invalid;
  std::vector<SamplerResourceUsage> usages;
};

struct BindingsLayoutResourceMetadata
{
  BindingsLayoutInfo layoutsInfo;
  BindingsLayoutId resourceId = BindingsLayoutId::Invalid;
  std::vector<BindingsLayoutResourceUsage> usages;
};

struct BindingGroupsResourceMetadata
{
  BindingGroupsInfo groupsInfo;
  BindingGroupsId resourceId = BindingGroupsId::Invalid;
  std::vector<BindingGroupsResourceUsage> usages;
};

struct GraphicsPipelineResourceMetadata
{
  GraphicsPipelineInfo pipelineInfo;
  GraphicsPipelineId resourceId = GraphicsPipelineId::Invalid;
  std::vector<GraphicsPipelineResourceUsage> usages;
};

struct ComputePipelineResourceMetadata
{
  ComputePipelineInfo pipelineInfo;
  ComputePipelineId resourceId = ComputePipelineId::Invalid;
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

struct RenderGraphTextureOverride
{
  TextureId textureId = TextureId::Invalid;
  ResourceLayout layout;
};

struct RenderGraphBufferOverride
{
  BufferId bufferId = BufferId::Invalid;
};

struct RenderGraphOverrides
{
  std::unordered_map<std::string, const RenderGraphBufferOverride> bufferOverrides = {};
  std::unordered_map<std::string, const RenderGraphTextureOverride> textureOverrides = {};
};

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
    std::vector<CommandPtr> commands;
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
  void cmdCopyImage(TextureView src, TextureView dst);
  void cmdBindBindingGroups(BindingGroups groups, uint32_t *dynamicOffsets, uint32_t dynamicOffsetsCount);
  void cmdBindGraphicsPipeline(GraphicsPipeline);
  void cmdBindComputePipeline(ComputePipeline);
  void cmdBindVertexBuffer(uint32_t slot, BufferView);
  void cmdBindIndexBuffer(BufferView, Type type);
  void cmdDraw(uint32_t vertexCount, uint32_t instanceCount = 1U, uint32_t firstVertex = 0U, uint32_t firstInstance = 0U);
  void cmdDrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1U, uint32_t firstIndex = 0U, uint32_t vertexOffset = 0U, uint32_t firstInstance = 0U);
  void cmdDrawIndexedIndirect(const BufferView indirectBuffer, uint32_t offset, uint32_t drawCount, uint32_t stride);
  void cmdDrawIndirectCount(const BufferView indirectBuffer, size_t offset, BufferView countBuffer, size_t countOffset, uint32_t maxDrawCount, uint32_t stride);
  void cmdDrawIndirect(const BufferView indirectBuffer, uint32_t offset, uint32_t drawCount, uint32_t stride);
  void cmdDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ);
  void cmdStartTimer(const Timer timer, PipelineStage stage, uint32_t sampleIndex = 0u);
  void cmdStopTimer(const Timer timer, PipelineStage stage, uint32_t sampleIndex = 0u);
  void cmdDispatchIndirect(const Buffer indirectBuffer, uint64_t max);

private:
  void beginSequence();

  friend class RenderGraph;
};

class RHIResources
{
  friend class RenderGraph;
  friend class RenderGraphCompiler;
  friend class PassesAnalysis;
  friend class DependencyGraphAnalysis;
  friend class TaskLevelsAnalysis;
  friend class AllocationsAnalysis;
  friend class SemaphoresAnalysis;
  friend class RenderGraphRuntimeResourcesManager;

private:
  RenderGraph *renderGraph;
  std::unordered_map<std::string, ShaderResourceMetadata> shadersMetadatas;
  std::unordered_map<std::string, BufferAllocation> scratchMap;
  std::unordered_map<BufferUsage, BufferResourceMetadata> scratchBuffers;
  std::unordered_map<std::string, BufferResourceMetadata> bufferMetadatas;
  std::unordered_map<std::string, TextureResourceMetadata> textureMetadatas;
  std::unordered_map<std::string, SamplerResourceMetadata> samplerMetadatas;
  std::unordered_map<std::string, BindingsLayoutResourceMetadata> bindingsLayoutMetadata;
  std::unordered_map<std::string, BindingGroupsResourceMetadata> bindingGroupsMetadata;
  std::unordered_map<std::string, GraphicsPipelineResourceMetadata> graphicsPipelineMetadata;
  std::unordered_map<std::string, ComputePipelineResourceMetadata> computePipelineMetadata;

public:
  RHIResources(RenderGraph *renderGraph);
  void recordConsumerUsage(const std::string &name, const InputResource &res, uint32_t taskId, Queue queue);
  Format getTextureFormat(const std::string &name);

  const BindingGroups getBindingGroups(const std::string &name);
  const GraphicsPipeline getGraphicsPipeline(const std::string &name);
  const ComputePipeline getComputePipeline(const std::string &name);

  const BindingsLayout getBindingsLayout(const std::string &name);
  const Sampler getSampler(const std::string &name);
  const Buffer getBuffer(const std::string &name);
  const Texture getTexture(const std::string &name);
};

using RenderGraphResourcesDatabase = RHIResources;

enum class BindingGroupTextureRefKind : uint8_t
{
  SampledTexture = 0,
  StorageTexture,
  SamplerView,
};

struct BindingGroupTextureRef
{
  uint32_t bgRuntimeId;
  uint32_t groupIndex;
  uint32_t bindingIndex;
  BindingGroupTextureRefKind kind;
};

struct BufferRuntimeMetadata
{
  std::string name;
  BufferInfo bufferInfo;
  uint64_t firstUsedAt = UINT64_MAX;
  uint64_t lastUsedAt = 0;
  std::vector<BufferResourceUsage> usages;
  BufferId resourceId = BufferId::Invalid;
};

struct TextureRuntimeMetadata
{
  std::string name;
  TextureInfo textureInfo;
  std::vector<TextureResourceUsage> usages;
  TextureId resourceId = TextureId::Invalid;
  ResourceLayout overrideLayout = ResourceLayout::UNDEFINED;
};

struct BindingGroupsRuntimeMetadata
{
  std::string name;
  BindingGroupsInfo groupsInfo;
  BindingGroupsId resourceId = BindingGroupsId::Invalid;
};

class RenderGraphResourcesRuntimeContext
{
public:
  std::vector<BufferRuntimeMetadata> buffers;
  std::vector<TextureRuntimeMetadata> textures;
  std::vector<BindingGroupsRuntimeMetadata> bindingGroups;

  std::unordered_map<std::string, uint32_t> bufferNameToRuntimeId;
  std::unordered_map<std::string, uint32_t> textureNameToRuntimeId;
  std::unordered_map<std::string, uint32_t> bindingGroupsNameToRuntimeId;

  std::unordered_map<uint32_t, std::vector<BindingGroupBufferRef>> bufferToBindingGroupRefs;
  std::unordered_map<uint32_t, std::vector<BindingGroupTextureRef>> textureToBindingGroupRefs;

  void clear()
  {
    buffers.clear();
    textures.clear();
    bindingGroups.clear();
    bufferNameToRuntimeId.clear();
    textureNameToRuntimeId.clear();
    bindingGroupsNameToRuntimeId.clear();
    bufferToBindingGroupRefs.clear();
    textureToBindingGroupRefs.clear();
  }
};

class Pass
{
  friend class RenderGraph;
  friend class RenderGraphCompiler;
  friend class PassesAnalysis;

protected:
  RenderGraph *renderGraph;
  std::string passName;
  CommandRecorder implicitCommandBuffer;
  CommandRecorder *submissionCommandBuffer = nullptr;

private:
  uint32_t index;

  void registerSelf(RenderGraph *rg, std::string &name, uint64_t index, CommandRecorder *sharedCommandBuffer = nullptr)
  {
    this->renderGraph = rg;
    this->passName = name;
    this->index = index;
    this->submissionCommandBuffer = sharedCommandBuffer != nullptr ? sharedCommandBuffer : &implicitCommandBuffer;
  }

  virtual void recordCommandBuffer(CommandRecorder &commandBuffer)
  {
    (void)commandBuffer;
    recordCommandBuffer();
  }
  virtual void recordCommandBuffer(CommandRecorder &commandBuffer, uint32_t frameIndex)
  {
    (void)frameIndex;
    recordCommandBuffer(commandBuffer);
  }
  virtual void recordCommandBuffer() {}
  virtual void submit();
  void instrumentTimedCommands();

public:
  virtual ~Pass();

protected:
  CommandRecorder &getCommandBuffer()
  {
    return *submissionCommandBuffer;
  }

  const CommandRecorder &getCommandBuffer() const
  {
    return *submissionCommandBuffer;
  }

  uint32_t getCurrentFrameIndex() const;
  uint32_t getMaxFramesInFlight() const;
  Buffer createFrameLocalBuffer(BufferInfo info) const;
  Texture createFrameLocalTexture(TextureInfo info) const;

public:
  uint32_t getIndex() const
  {
    return index;
  }
};

class RenderGraph
{
public:
  struct Frame;

  struct CommandBufferHandle
  {
    uint64_t value = 0u;

    bool isValid() const noexcept
    {
      return value != 0u;
    }

    bool operator==(const CommandBufferHandle &other) const noexcept
    {
      return value == other.value;
    }

    bool operator!=(const CommandBufferHandle &other) const noexcept
    {
      return !(*this == other);
    }
  };

  struct CpuTransferStats
  {
    uint64_t callCount = 0u;
    uint64_t totalBytes = 0u;
    uint64_t totalNs = 0u;
    uint64_t maxNs = 0u;
  };

  struct CpuStats
  {
    CpuTransferStats bufferWrites{};
    CpuTransferStats bufferReads{};
  };

  struct FrameWaitSummary
  {
    uint32_t blockedSubmissionCount = 0u;
    uint32_t waitedNodeCount = 0u;
    uint64_t totalWaitNs = 0u;
  };

  struct PassTimingSummary
  {
    std::string name;
    uint32_t passIndex = 0u;
    uint32_t timerCount = 0u;
    double gpuTimeMs = 0.0;
    bool hasComputeWork = false;
    bool hasGraphicsWork = false;
  };

  struct TimerReadbackRecord
  {
    std::string passName;
    std::string timerName;
    uint32_t passIndex = 0u;
    double gpuTimeMs = 0.0;
    bool hasComputeWork = false;
    bool hasGraphicsWork = false;
  };

  struct RunPhaseRecord
  {
    std::string name;
    uint64_t totalNs = 0u;
  };

  struct RuntimeMetricRecord
  {
    std::string name;
    uint64_t totalNs = 0u;
    uint64_t callCount = 0u;
    uint64_t maxNs = 0u;
  };

  struct RunDebugStats
  {
    uint32_t submissionCount = 0u;
    uint32_t submittedCommandBufferCount = 0u;
    uint32_t externalWaitDependencyCount = 0u;
    uint32_t sameQueueWaitDependencyCount = 0u;
    uint32_t emittedBufferBarrierCount = 0u;
    uint32_t emittedTextureBarrierCount = 0u;
    uint32_t bufferQueueTransferCount = 0u;
    uint32_t textureQueueTransferCount = 0u;
    uint32_t skippedReadOnlyBufferBarrierCount = 0u;
    uint32_t skippedReadOnlyTextureBarrierCount = 0u;
  };

  struct Settings
  {
    uint32_t maxTimers = 1024u;
    uint32_t maxFramesInFlight = 1u;
  };

  struct InterFrameDependency
  {
    std::string waitNodeName;
    std::string signalNodeName;
  };

  struct InterFrameSignalState
  {
    uint32_t frameIndex = std::numeric_limits<uint32_t>::max();
    std::vector<GPUFuture> futures;
  };

private:
  struct TimerReadbackMetadata
  {
    std::string passName;
    std::string timerName;
    uint32_t passIndex = 0u;
    bool hasComputeWork = false;
    bool hasGraphicsWork = false;
    TimerUnit unit = TimerUnit::Miliseconds;
    uint64_t resolvedBufferOffset = 0u;
    uint32_t resolvedQueryCount = 0u;
    Timer timer{};
  };

  struct RenderGraphNode
  {
    struct TimerBinding
    {
      std::string name;
      TimerInfo info{};
      Timer timer{};
      PipelineStage startStage = PipelineStage::TOP_OF_PIPE;
      PipelineStage stopStage = PipelineStage::BOTTOM_OF_PIPE;
      bool hasComputeWork = false;
      bool hasGraphicsWork = false;
    };

    std::string name;
    uint64_t id;
    uint64_t level;
    uint64_t priority;
    uint64_t dispatchId;

    std::vector<uint64_t> signalSemaphores;
    std::vector<uint64_t> waitSemaphores;

    Queue queue;
    std::vector<CommandPtr> commands;
    std::vector<TimerBinding> timers;

    std::vector<TextureBarrier> preTextureTransitions;
    std::vector<TextureBarrier> postTextureTransitions;
    std::vector<BufferBarrier> preBufferTransitions;
    std::vector<BufferBarrier> postBufferTransitions;
  };

  struct RenderGraphPass
  {
    std::string name;
    CommandRecorder cmd;
  };

  struct RecordedCommandBuffers
  {
    uint32_t frameIndex = 0u;
    CommandBuffersByQueue commandBuffers = {};
    std::vector<CommandBuffer> nodeCommandBuffers;
    std::unordered_map<CommandBuffer, std::unordered_set<CommandBuffer>> waits;
    std::unordered_map<CommandBuffer, std::vector<std::string>> interFrameWaits;
    std::unordered_map<CommandBuffer, std::vector<uint32_t>> nodes;
    std::vector<GPUFuture> inFlightFutures;
    std::vector<BindingGroupsId> retainedBindingGroups;
  };

  friend class Task;
  friend class RenderGraphCompiler;
  friend class PassesAnalysis;
  friend class DependencyGraphAnalysis;
  friend class TaskLevelsAnalysis;
  friend class AllocationsAnalysis;
  friend class SemaphoresAnalysis;
  friend class RenderGraphBarrierDispatcher;
  friend class RenderGraphBufferBarrierDispatcher;
  friend class RenderGraphTextureBarrierDispatcher;
  friend class RenderGraphCommandBufferDispatcher;
  friend class RenderGraphRuntimeResourcesManager;
  bool compiled;
  uint64_t executions;
  RHI *rhi;
  Settings settings;
  CpuStats cpuStats;
  FrameWaitSummary frameWaitSummary{};
  mutable std::vector<PassTimingSummary> passTimingSummaries;
  mutable bool passTimingSummariesDirty = true;
  std::vector<TimerReadbackMetadata> timerReadbackMetadata;
  std::vector<Timer> timerReadbackScratch;
  std::vector<double> timerReadbackValuesScratch;
  std::vector<TimerReadbackRecord> timerReadbackRecords;
  std::vector<BufferId> timerReadbackBufferIds;
  uint64_t timerReadbackBufferSize = 0u;
  std::vector<RunPhaseRecord> lastRunPhaseRecords;
  std::vector<RuntimeMetricRecord> lastRuntimeMetricRecords;
  RunDebugStats lastRunDebugStats{};
  uint64_t lastRunTotalNs = 0u;
  uint64_t lastWaitFrameSpanNs = 0u;
  uint64_t lastWaitFrameBlockSpanNs = 0u;
  uint64_t lastTimerReadbackSpanNs = 0u;
  uint32_t lastSubmissionCount = 0u;
  uint32_t currentFrameIndex = 0u;
  bool timerReadbackEnabled = true;

  lib::ConcurrentQueue<RenderGraphPass> passes;
  std::unordered_set<const CommandRecorder *> enqueuedCommandBufferIdentities;

  lib::ConcurrentHashMap<std::string, Pass *> registeredPasses;

  std::vector<RenderGraphNode> nodes;
  std::vector<std::vector<RenderGraphEdge>> edges;

  // Runtime Info
  RHIResources resources;
  RenderGraphResourcesRuntimeContext runtimeContext;
  RenderGraphRuntimeResourcesManager runtimeResourcesManager;
  std::vector<Semaphore> semaphores;
  std::vector<InterFrameDependency> interFrameDependencies;
  std::unordered_map<std::string, InterFrameSignalState> lastInterFrameSignalFutures;

  std::unordered_map<uint64_t, RecordedCommandBuffers> recordedCommandBuffers;
  uint64_t nextRecordedCommandBufferHandle = 1u;
  // TODO: remove from here
  // std::vector<TextureBarrier> textureTransitions;
  // std::vector<BufferBarrier> bufferTransitions;

  // std::vector<BufferAllocation> bufferAllocations;
  // std::vector<TextureAllocation> textureAllocations;
  // std::vector<SamplerAllocation> samplerAllocations;
  // std::vector<BindingsLayoutAllocation> bindingsLayoutsAllocations;

  void registerConsumer(const std::string &name, const InputResource &res, uint32_t taskId, Queue queue);
  void analyseTaskInputs();
  RecordedCommandBuffers recordCommandBuffers(const RenderGraphOverrides &overrides, bool oneTimeSubmit, uint32_t frameIndex);
  void submitRecordedCommandBuffers(Frame &frame, RecordedCommandBuffers &recorded, bool releaseCommandBuffersOnCompletion);
  void releaseRecordedCommandBuffers(RecordedCommandBuffers &recorded);
  void releaseAllRecordedCommandBuffers();
  void rebuildTimerReadbackMetadata();
  void rebuildTimerReadbackBuffers();
  void releaseTimerReadbackBuffers();
  void rebuildPassTimingSummaries() const;

public:
  struct Frame
  {
    uint32_t frameIndex = 0u;

    struct SubmissionFuture
    {
      struct NodeRecord
      {
        std::string name;
        uint64_t id = 0u;
        uint64_t level = 0u;
      };

      GPUFuture future;
      std::string name;
      std::vector<NodeRecord> nodes;
      Queue queue = Queue::None;
    };

    std::vector<GPUFuture> futures;
    std::vector<SubmissionFuture> submissionFutures;
    std::vector<BindingGroupsId> retainedBindingGroups;
  };

  using TextureOverride = RenderGraphTextureOverride;
  using BufferOverride = RenderGraphBufferOverride;
  using Overrides = RenderGraphOverrides;
  inline static bool ExecuteAlways(const RenderGraph &renderGraph)
  {
    return true;
  }

  inline static bool ExecuteOnFirstRun(const RenderGraph &renderGraph)
  {
    return renderGraph.executions == 0;
  }

  RenderGraph(RHI *rhi);
  RenderGraph(RHI *rhi, Settings settings);
  ~RenderGraph();

  void enqueueCommandBuffer(std::string name, const CommandRecorder &, const CommandRecorder *identity = nullptr);
  void compile();

  CommandBufferHandle createCommandBuffer(const Overrides &overrides);
  CommandBufferHandle createCommandBuffer(const Overrides &overrides, uint32_t frameIndex);
  void destroyCommandBuffer(CommandBufferHandle handle);
  void run(Frame &outFrame, CommandBufferHandle handle);
  void run(Frame &outFrame, const Overrides &overrides);

  void waitFrame(Frame &frame);
  void setCurrentFrameIndex(uint32_t frameIndex);
  uint32_t getCurrentFrameIndex() const;
  uint32_t getMaxFramesInFlight() const;

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
  void resetCpuStats();
  CpuStats getCpuStats() const;
  double getLastRunTotalMs() const;
  const std::vector<RunPhaseRecord> &getLastRunPhaseRecords() const;
  const std::vector<RuntimeMetricRecord> &getLastRuntimeMetricRecords() const;
  const RunDebugStats &getLastRunDebugStats() const;
  const FrameWaitSummary &getFrameWaitSummary() const
  {
    return frameWaitSummary;
  }
  double getLastWaitFrameSpanMs() const;
  double getLastWaitFrameBlockSpanMs() const;
  double getLastTimerReadbackSpanMs() const;
  void setTimerReadbackEnabled(bool enabled);
  bool isTimerReadbackEnabled() const;
  void addInterFrameDependency(std::string waitNodeName, std::string signalNodeName);
  void clearInterFrameDependencies();
  void logFrameStats() const;
  void clearFrameStats();
  double readTimer(const Timer &timer);
  std::vector<TimerReadbackRecord> readResolvedTimerRecords(uint32_t frameIndex) const;
  const std::vector<PassTimingSummary> &getPassTimingSummaries() const;
  const std::vector<TimerReadbackRecord> &getTimerReadbackRecords() const;

  inline RHI *getRHI()
  {
    return rhi;
  }

  inline const uint64_t getBufferSize(const Buffer &buffer)
  {
    auto it = resources.bufferMetadatas.find(buffer.name);

    if (it == resources.bufferMetadatas.end())
    {
      throw std::runtime_error("buffer not found");
    }

    return it->second.bufferInfo.size;
  }

  template <typename T, typename... Args> T *registerPass(std::string name, uint32_t index, Args &&...args)
  {
    os::print("Registering pass %s\n", name.c_str());
    const auto start = lib::time::TimeSpan::now();

    static_assert(std::is_base_of<Pass, T>::value, "T must inherit from Pass");
    T *plugin = new T(std::forward<Args>(args)...);

    plugin->registerSelf(this, name, index);
    plugin->submissionCommandBuffer->beginSequence();
    static_cast<Pass *>(plugin)->recordCommandBuffer(*plugin->submissionCommandBuffer, currentFrameIndex);
    plugin->instrumentTimedCommands();
    registeredPasses.insert(name, plugin);
    const double elapsedMs = (lib::time::TimeSpan::now() - start).milliseconds();
    os::print("Registered pass %s in %.2f ms\n", name.c_str(), elapsedMs);
    return plugin;
  }

  template <typename T, typename... Args> T *registerPass(std::string name, uint32_t index, CommandRecorder &commandBuffer, Args &&...args)
  {
    os::print("Registering pass %s\n", name.c_str());
    const auto start = lib::time::TimeSpan::now();

    static_assert(std::is_base_of<Pass, T>::value, "T must inherit from Pass");
    T *plugin = new T(std::forward<Args>(args)...);

    plugin->registerSelf(this, name, index, &commandBuffer);
    commandBuffer.beginSequence();
    static_cast<Pass *>(plugin)->recordCommandBuffer(commandBuffer, currentFrameIndex);
    plugin->instrumentTimedCommands();
    registeredPasses.insert(name, plugin);
    const double elapsedMs = (lib::time::TimeSpan::now() - start).milliseconds();
    os::print("Registered pass %s in %.2f ms\n", name.c_str(), elapsedMs);
    return plugin;
  }

  template <typename T> T *get(std::string name)
  {
    static_assert(std::is_base_of<Pass, T>::value, "T must inherit from Pass");
    auto it = registeredPasses.find(name);

    if (it == registeredPasses.end())
    {
      return nullptr;
    }

    return reinterpret_cast<T *>(it.value());
  }

  template <typename T> bool has(std::string name)
  {
    static_assert(std::is_base_of<Pass, T>::value, "T must inherit from Pass");
    return registeredPasses.contains(name);
  }

  template <typename T> void removePass(std::string name)
  {
    static_assert(std::is_base_of<Pass, T>::value, "T must inherit from Pass");
    auto it = registeredPasses.find(name);
    if (it != registeredPasses.end())
    {
      registeredPasses.remove(name);
      T *pass = reinterpret_cast<T *>(it.value());
      delete pass;
    }
  }
};

}; // namespace rendering
