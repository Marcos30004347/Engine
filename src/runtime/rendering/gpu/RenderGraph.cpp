#include "RenderGraph.hpp"
#include "commands/Commands.hpp"
#include "compiler/RenderGraphCompiler.hpp"
#include "os/Logger.hpp"
#include "runtime/RenderGraphBarrierDispatcher.hpp"
#include "runtime/RenderGraphCommandBufferDispatcher.hpp"
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <map>
#include <numeric>
#include <sstream>
#include <unordered_set>
#include <unordered_map>

#include "datastructure/BoundedTaggedRectTreap.hpp"
#include "datastructure/ConcurrentQueue.hpp"
#include "datastructure/TaggedInternvalTree.hpp"
#include "time/TimeSpan.hpp"

#ifndef REPORT_FUTURE_TIMINGS
#define REPORT_FUTURE_TIMINGS 1
#endif

#define RENDER_GRAPH_FATAL(...)                                                                                                                                                                                            \
  do                                                                                                                                                                                                                       \
  {                                                                                                                                                                                                                        \
    os::Logger::errorf(__VA_ARGS__);                                                                                                                                                                                       \
    exit(1);                                                                                                                                                                                                               \
  } while (0)

namespace rendering
{

static uint64_t currentSteadyClockNs()
{
  return static_cast<uint64_t>(lib::time::TimeSpan::now().nanoseconds());
}

static const Type formatToTypeTable[Format_Count] = {
  // 8-bit formats
  [Format_R8Unorm] = Type_Uint8,
  [Format_R8Snorm] = Type_Int8,
  [Format_R8Uint] = Type_Uint8,
  [Format_R8Sint] = Type_Int8,

  // 16-bit formats
  [Format_R16Uint] = Type_Uint16,
  [Format_R16Sint] = Type_Int16,
  [Format_R16Float] = Type_Float16,
  [Format_RG8Unorm] = Type_Uint8x2,
  [Format_RG8Snorm] = Type_Int8x2,
  [Format_RG8Uint] = Type_Uint8x2,
  [Format_RG8Sint] = Type_Int8x2,

  // 32-bit single channel
  [Format_R32Uint] = Type_Uint32,
  [Format_R32Sint] = Type_Int32,
  [Format_R32Float] = Type_Float32,

  // 32-bit two channel
  [Format_RG16Uint] = Type_Uint16x2,
  [Format_RG16Sint] = Type_Int16x2,
  [Format_RG16Float] = Type_Float16x2,

  // 32-bit four channel (8-bit each)
  [Format_RGBA8Unorm] = Type_Uint8x4,
  [Format_RGBA8UnormSrgb] = Type_Uint8x4,
  [Format_RGBA8Snorm] = Type_Int8x4,
  [Format_RGBA8Uint] = Type_Uint8x4,
  [Format_RGBA8Sint] = Type_Int8x4,
  [Format_BGRA8Unorm] = Type_Uint8x4,
  [Format_BGRA8UnormSrgb] = Type_Uint8x4,

  // 32-bit packed
  [Format_RGB10A2Uint] = Type_Packed_Uint_2_10_10_10,
  [Format_RGB10A2Unorm] = Type_Packed_Uint_2_10_10_10,
  [Format_RG11B10UFloat] = Type_Packed_UFloat_11_11_10,
  [Format_RGB9E5UFloat] = Type_Packed_UFloat_9995,

  // 64-bit formats
  [Format_RG32Uint] = Type_Uint32x2,
  [Format_RG32Sint] = Type_Int32x2,
  [Format_RG32Float] = Type_Float32x2,

  [Format_RGBA16Uint] = Type_Uint16x4,
  [Format_RGBA16Sint] = Type_Int16x4,
  [Format_RGBA16Float] = Type_Float16x4,

  [Format_RGB8Unorm] = Type_Uint8x3,
  [Format_RGB8Snorm] = Type_Int8x3,
  [Format_RGB8Uint] = Type_Uint8x3,
  [Format_RGB8Sint] = Type_Int8x3,

  [Format_RGB16Uint] = Type_Uint16x3,
  [Format_RGB16Sint] = Type_Int16x3,
  [Format_RGB16Float] = Type_Float16x3,

  [Format_RGB32Uint] = Type_Uint32x3,
  [Format_RGB32Sint] = Type_Int32x3,
  [Format_RGB32Float] = Type_Float32x3,

  // 128-bit formats
  [Format_RGBA32Uint] = Type_Uint32x4,
  [Format_RGBA32Sint] = Type_Int32x4,
  [Format_RGBA32Float] = Type_Float32x4,

  // Depth / stencil
  [Format_Stencil8] = Type_Stencil,
  [Format_Depth16Unorm] = Type_Depth,
  [Format_Depth24Plus] = Type_Depth,
  [Format_Depth24PlusStencil8] = Type_Depth,
  [Format_Depth32Float] = Type_Depth,
  [Format_Depth32FloatStencil8] = Type_Depth,
};

static const Format typeToFormatTable[Type_Count] = {
  [Type_None] = Format_None,

  // Unsigned integers
  [Type_Uint8] = Format_R8Uint,
  [Type_Uint8x2] = Format_RG8Uint,
  [Type_Uint8x3] = Format_RGB8Uint,
  [Type_Uint8x4] = Format_RGBA8Uint,

  [Type_Uint16] = Format_R16Uint,
  [Type_Uint16x2] = Format_RG16Uint,
  [Type_Uint16x3] = Format_RGB16Uint,
  [Type_Uint16x4] = Format_RGBA16Uint,

  [Type_Uint32] = Format_R32Uint,
  [Type_Uint32x2] = Format_RG32Uint,
  [Type_Uint32x3] = Format_RGB32Uint,
  [Type_Uint32x4] = Format_RGBA32Uint,

  // Signed integers
  [Type_Int8] = Format_R8Sint,
  [Type_Int8x2] = Format_RG8Sint,
  [Type_Int8x3] = Format_RGB8Sint,
  [Type_Int8x4] = Format_RGBA8Sint,

  [Type_Int16] = Format_R16Sint,
  [Type_Int16x2] = Format_RG16Sint,
  [Type_Int16x3] = Format_RGB16Sint,
  [Type_Int16x4] = Format_RGBA16Sint,

  [Type_Int32] = Format_R32Sint,
  [Type_Int32x2] = Format_RG32Sint,
  [Type_Int32x3] = Format_RGB32Sint,
  [Type_Int32x4] = Format_RGBA32Sint,

  // Floats
  [Type_Float16] = Format_R16Float,
  [Type_Float16x2] = Format_RG16Float,
  [Type_Float16x3] = Format_RGB16Float,
  [Type_Float16x4] = Format_RGBA16Float,

  [Type_Float32] = Format_R32Float,
  [Type_Float32x2] = Format_RG32Float,
  [Type_Float32x3] = Format_RGB32Float,
  [Type_Float32x4] = Format_RGBA32Float,

  // Packed
  [Type_Packed_Uint_2_10_10_10] = Format_RGB10A2Uint,
  [Type_Packed_UFloat_11_11_10] = Format_RG11B10UFloat,
  [Type_Packed_UFloat_9995] = Format_RGB9E5UFloat,

  // Depth / stencil
  [Type_Depth] = Format_Depth32Float,
  [Type_Stencil] = Format_Stencil8,
};

Type formatToType(Format format)
{
  return formatToTypeTable[format];
}

Format typeToFormat(Type type)
{
  return typeToFormatTable[type];
}

// size_t formatPixelSize(Format fmt)
// {
//   switch (fmt)
//   {
//   case Format_R8Unorm:
//   case Format_R8Snorm:
//   case Format_R8Uint:
//   case Format_R8Sint:
//     return 1;

//   case Format_R16Uint:
//   case Format_R16Sint:
//   case Format_R16Float:
//   case Format_RG8Unorm:
//   case Format_RG8Snorm:
//   case Format_RG8Uint:
//   case Format_RG8Sint:
//     return 2;

//   case Format_R32Uint:
//   case Format_R32Sint:
//   case Format_R32Float:
//   case Format_RG16Uint:
//   case Format_RG16Sint:
//   case Format_RG16Float:
//   case Format_RGBA8Unorm:
//   case Format_RGBA8UnormSrgb:
//   case Format_RGBA8Snorm:
//   case Format_RGBA8Uint:
//   case Format_RGBA8Sint:
//   case Format_BGRA8Unorm:
//   case Format_BGRA8UnormSrgb:
//   case Format_RGB10A2Uint:
//   case Format_RGB10A2Unorm:
//   case Format_RG11B10UFloat:
//   case Format_RGB9E5UFloat:
//     return 4;

//   case Format_RG32Uint:
//   case Format_RG32Sint:
//   case Format_RG32Float:
//   case Format_RGBA16Uint:
//   case Format_RGBA16Sint:
//   case Format_RGBA16Float:
//     return 8;

//   case Format_RGBA32Uint:
//   case Format_RGBA32Sint:
//   case Format_RGBA32Float:
//     return 16;

//   case Format_Stencil8:
//     return 1;
//   case Format_Depth16Unorm:
//     return 2;
//   case Format_Depth24Plus:
//   case Format_Depth24PlusStencil8:
//     return 4;
//   case Format_Depth32Float:
//   case Format_Depth32FloatStencil8:
//     return 4;

//   default:
//     return 0;
//   }
// }

std::string toString(ResourceLayout layout)
{
  switch (layout)
  {
  case ResourceLayout::UNDEFINED:
    return "UNDEFINED";
  case ResourceLayout::GENERAL:
    return "GENERAL";
  case ResourceLayout::COLOR_ATTACHMENT:
    return "COLOR_ATTACHMENT";
  case ResourceLayout::DEPTH_STENCIL_ATTACHMENT:
    return "DEPTH_STENCIL_ATTACHMENT";
  case ResourceLayout::DEPTH_STENCIL_READ_ONLY:
    return "DEPTH_STENCIL_READ_ONLY";
  case ResourceLayout::SHADER_READ_ONLY:
    return "SHADER_READ_ONLY";
  case ResourceLayout::TRANSFER_SRC:
    return "TRANSFER_SRC";
  case ResourceLayout::TRANSFER_DST:
    return "TRANSFER_DST";
  case ResourceLayout::PREINITIALIZED:
    return "PREINITIALIZED";
  case ResourceLayout::PRESENT_SRC:
    return "PRESENT_SRC";
  default:
    return "UNKNOWN_RESOURCE_LAYOUT";
  }
}

std::string toString(PipelineStage stage)
{
  switch (stage)
  {
  case PipelineStage::TOP_OF_PIPE:
    return "TOP_OF_PIPE";
  case PipelineStage::VERTEX_INPUT:
    return "VERTEX_INPUT";
  case PipelineStage::VERTEX_SHADER:
    return "VERTEX_SHADER";
  case PipelineStage::FRAGMENT_SHADER:
    return "FRAGMENT_SHADER";
  case PipelineStage::COMPUTE_SHADER:
    return "COMPUTE_SHADER";
  case PipelineStage::TRANSFER:
    return "TRANSFER";
  case PipelineStage::BOTTOM_OF_PIPE:
    return "BOTTOM_OF_PIPE";
  case PipelineStage::ALL_GRAPHICS:
    return "ALL_GRAPHICS";
  case PipelineStage::HOST:
    return "HOST";
  case PipelineStage::ALL_COMMANDS:
    return "ALL_COMMANDS";
  }
}

std::string toString(Queue queue)
{
  switch (queue)
  {
  case None:
    return "None";
  case Graphics:
    return "Graphics";
  case Compute:
    return "Compute";
  case Transfer:
    return "Transfer";
  case Present:
    return "Present";
  default:
    return "EOF";
  }

  return "EOF";
}
std::string toString(AccessPattern access)
{
  switch (access)
  {
  case AccessPattern::NONE:
    return "NONE";
  case AccessPattern::VERTEX_ATTRIBUTE_READ:
    return "VERTEX_ATTRIBUTE_READ";
  case AccessPattern::INDEX_READ:
    return "INDEX_READ";
  case AccessPattern::UNIFORM_READ:
    return "UNIFORM_READ";
  case AccessPattern::SHADER_READ:
    return "SHADER_READ";
  case AccessPattern::SHADER_WRITE:
    return "SHADER_WRITE";
  case AccessPattern::COLOR_ATTACHMENT_READ:
    return "COLOR_ATTACHMENT_READ";
  case AccessPattern::COLOR_ATTACHMENT_WRITE:
    return "COLOR_ATTACHMENT_WRITE";
  case AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ:
    return "DEPTH_STENCIL_ATTACHMENT_READ";
  case AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE:
    return "DEPTH_STENCIL_ATTACHMENT_WRITE";
  case AccessPattern::TRANSFER_READ:
    return "TRANSFER_READ";
  case AccessPattern::TRANSFER_WRITE:
    return "TRANSFER_WRITE";
  case AccessPattern::INDIRECT_COMMAND_READ:
    return "INDIRECT_COMMAND_READ";
  case AccessPattern::MEMORY_READ:
    return "MEMORY_READ";
  case AccessPattern::MEMORY_WRITE:
    return "MEMORY_WRITE";
  default:
    return "UNKNOWN_ACCESS_PATTERN";
  }
}

std::string bufferUsageToString(int usage)
{
  if (usage == BufferUsage_None)
    return "None";

  std::ostringstream oss;
  bool first = true;

  auto addFlag = [&](int flag, const char *name)
  {
    if (usage & flag)
    {
      if (!first)
        oss << " | ";
      oss << name;
      first = false;
    }
  };

  addFlag(BufferUsage_Uniform, "Uniform");
  addFlag(BufferUsage_Storage, "Storage");
  addFlag(BufferUsage_Push, "Push");
  addFlag(BufferUsage_Pull, "Pull");
  addFlag(BufferUsage_Vertex, "Vertex");
  addFlag(BufferUsage_Indirect, "Indirect");
  addFlag(BufferUsage_Timestamp, "Timestamp");
  addFlag(BufferUsage_Index, "Index");

  return oss.str();
}

struct BufferSlice
{
  std::string bufferId;
  size_t offset;
  size_t size;
};

void RenderGraph::registerConsumer(const std::string &name, const InputResource &res, uint32_t taskId, Queue queue)
{
  resources.recordConsumerUsage(name, res, taskId, queue);
}

RenderGraph::RenderGraph(RHI *renderingHardwareInterface) : RenderGraph(renderingHardwareInterface, Settings{})
{
}

RenderGraph::RenderGraph(RHI *renderingHardwareInterface, Settings graphSettings)
    : compiled(false), executions(0u), rhi(renderingHardwareInterface), settings(graphSettings), resources(this), runtimeResourcesManager(*this)
{
  if (settings.maxFramesInFlight == 0u)
  {
    settings.maxFramesInFlight = 1u;
  }
  rhi->reserveTimerCapacity(std::max(1u, settings.maxTimers));
}

RenderGraph::~RenderGraph()
{
  releaseAllRecordedCommandBuffers();
  releaseTimerReadbackBuffers();
}

void Pass::submit()
{
  renderGraph->enqueueCommandBuffer(passName, *submissionCommandBuffer, submissionCommandBuffer);
}

uint32_t Pass::getCurrentFrameIndex() const
{
  return renderGraph != nullptr ? renderGraph->getCurrentFrameIndex() : 0u;
}

uint32_t Pass::getMaxFramesInFlight() const
{
  return renderGraph != nullptr ? renderGraph->getMaxFramesInFlight() : 1u;
}

Buffer Pass::createFrameLocalBuffer(BufferInfo info) const
{
  if (renderGraph != nullptr && renderGraph->getMaxFramesInFlight() > 1u)
  {
    info.frameLocal = true;
  }

  return renderGraph->createBuffer(info);
}

Texture Pass::createFrameLocalTexture(TextureInfo info) const
{
  if (renderGraph != nullptr && renderGraph->getMaxFramesInFlight() > 1u)
  {
    info.frameLocal = true;
  }

  return renderGraph->createTexture(info);
}

void RenderGraph::enqueueCommandBuffer(std::string name, const CommandRecorder &cmd, const CommandRecorder *identity)
{
  if (identity != nullptr && !enqueuedCommandBufferIdentities.insert(identity).second)
  {
    return;
  }

  auto pass = RenderGraphPass{
    .name = name,
    .cmd = cmd,
  };

  passes.enqueue(pass);
}

void RenderGraph::compile()
{
  releaseAllRecordedCommandBuffers();
  enqueuedCommandBufferIdentities.clear();
  lastInterFrameSignalFutures.clear();
  RenderGraphCompiler compiler(*this);
  compiler.compile();
  rebuildTimerReadbackMetadata();
  rebuildTimerReadbackBuffers();
}

void RenderGraph::releaseRecordedCommandBuffers(RecordedCommandBuffers &recorded)
{
  for (auto &future : recorded.inFlightFutures)
  {
    if (future.valid())
    {
      rhi->blockUntil(future);
    }
  }

  for (Queue queue = Queue::None; queue < Queue::QueuesCount; queue = Queue((uint64_t)queue + 1))
  {
    if (!recorded.commandBuffers[queue].empty())
    {
      rhi->releaseCommandBuffer(recorded.commandBuffers[queue]);
      recorded.commandBuffers[queue].clear();
    }
  }

  for (const BindingGroupsId bindingGroupsId : recorded.retainedBindingGroups)
  {
    if (bindingGroupsId != BindingGroupsId::Invalid)
    {
      rhi->deleteBindingGroups(bindingGroupsId);
    }
  }

  rhi->flushDeferredDeletions();
  recorded.inFlightFutures.clear();
  recorded.retainedBindingGroups.clear();
  recorded.nodeCommandBuffers.clear();
  recorded.waits.clear();
  recorded.interFrameWaits.clear();
  recorded.nodes.clear();
}

void RenderGraph::releaseAllRecordedCommandBuffers()
{
  for (auto &[_, recorded] : recordedCommandBuffers)
  {
    releaseRecordedCommandBuffers(recorded);
  }

  recordedCommandBuffers.clear();
  nextRecordedCommandBufferHandle = 1u;
  lastInterFrameSignalFutures.clear();
}

RenderGraph::CommandBufferHandle RenderGraph::createCommandBuffer(const Overrides &overrides)
{
  return createCommandBuffer(overrides, currentFrameIndex);
}

RenderGraph::CommandBufferHandle RenderGraph::createCommandBuffer(const Overrides &overrides, uint32_t frameIndex)
{
  setCurrentFrameIndex(frameIndex);
  RecordedCommandBuffers recorded = recordCommandBuffers(overrides, false, currentFrameIndex);
  const uint64_t handleValue = nextRecordedCommandBufferHandle++;
  recordedCommandBuffers.emplace(handleValue, std::move(recorded));
  return CommandBufferHandle{.value = handleValue};
}

void RenderGraph::destroyCommandBuffer(CommandBufferHandle handle)
{
  auto recordedIt = recordedCommandBuffers.find(handle.value);
  if (recordedIt == recordedCommandBuffers.end())
  {
    return;
  }

  releaseRecordedCommandBuffers(recordedIt->second);
  recordedCommandBuffers.erase(recordedIt);
}

// AccessPattern removeReadAccesses(AccessPattern access)
// {
//   // TODO: fix
//   return access;

//   AccessPattern READ_ACCESS_MASK = AccessPattern::VERTEX_ATTRIBUTE_READ | AccessPattern::INDEX_READ | AccessPattern::UNIFORM_READ | AccessPattern::SHADER_READ |
//                                    AccessPattern::COLOR_ATTACHMENT_READ | AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ | AccessPattern::TRANSFER_READ |
//                                    AccessPattern::INDIRECT_COMMAND_READ | AccessPattern::MEMORY_READ;

//   return static_cast<AccessPattern>(static_cast<uint32_t>(access) & ~static_cast<uint32_t>(READ_ACCESS_MASK));
// }

// #define RENDER_GRAPH_LOG_BARRIERS
// #define RENDER_GRAPH_LOG_EXECUTION
// #define RENDER_GRAPH_LOG_COMMANDS

#ifndef RENDER_GRAPH_ENABLE_DIAGNOSTICS
#define RENDER_GRAPH_ENABLE_DIAGNOSTICS 1
#endif

#ifndef RENDER_GRAPH_ENABLE_BENCHMARKS
#define RENDER_GRAPH_ENABLE_BENCHMARKS RENDER_GRAPH_ENABLE_DIAGNOSTICS
#endif

#ifndef RENDER_GRAPH_ENABLE_LOGGING
#define RENDER_GRAPH_ENABLE_LOGGING RENDER_GRAPH_ENABLE_DIAGNOSTICS
#endif

#ifndef RENDER_GRAPH_ENABLE_DETAILED_STATS
#define RENDER_GRAPH_ENABLE_DETAILED_STATS 0
#endif

#if !RENDER_GRAPH_ENABLE_DIAGNOSTICS
#undef RENDER_GRAPH_ENABLE_BENCHMARKS
#define RENDER_GRAPH_ENABLE_BENCHMARKS 0
#undef RENDER_GRAPH_ENABLE_LOGGING
#define RENDER_GRAPH_ENABLE_LOGGING 0
#endif

#if RENDER_GRAPH_ENABLE_LOGGING
#define RG_LOG(...) os::Logger::logf(__VA_ARGS__)
#else
#define RG_LOG(...) ((void)0)
#endif

#if RENDER_GRAPH_ENABLE_LOGGING && defined(RENDER_GRAPH_LOG_COMMANDS)
#define RG_LOG_CMD(...) RG_LOG(__VA_ARGS__)
#else
#define RG_LOG_CMD(...) ((void)0)
#endif

#if RENDER_GRAPH_ENABLE_LOGGING && defined(RENDER_GRAPH_LOG_BARRIERS)
#define RG_LOG_BARRIER(...) RG_LOG(__VA_ARGS__)
#else
#define RG_LOG_BARRIER(...) ((void)0)
#endif

#if RENDER_GRAPH_ENABLE_LOGGING && defined(RENDER_GRAPH_LOG_EXECUTION)
#define RG_LOG_EXEC(...) RG_LOG(__VA_ARGS__)
#define RENDER_GRAPH_EXECUTION_LOGGING 1
#else
#define RG_LOG_EXEC(...) ((void)0)
#define RENDER_GRAPH_EXECUTION_LOGGING 0
#endif

static const char *logQueue(Queue q)
{
  switch (q)
  {
  case Queue::None:
    return "None";
  case Queue::Graphics:
    return "Graphics";
  case Queue::Compute:
    return "Compute";
  case Queue::Transfer:
    return "Transfer";
  case Queue::Present:
    return "Present";
  default:
    return "Unknown";
  }
}

static bool matchesInterFrameDependencyTarget(const std::string &nodeName, const std::string &targetName)
{
  if (nodeName == targetName)
  {
    return true;
  }

  if (nodeName.size() <= targetName.size())
  {
    return false;
  }

  return nodeName.compare(0u, targetName.size(), targetName) == 0 && nodeName[targetName.size()] == '_';
}

RenderGraph::RecordedCommandBuffers RenderGraph::recordCommandBuffers(const RenderGraphOverrides &overrides, bool oneTimeSubmit, uint32_t frameIndex)
{
  auto logRecordProgress = [&](const char *message)
  {
    os::Logger::logf("[RenderGraph][Record] %s", message);
  };

  auto logNodeRecordProgress = [&](const char *step, uint32_t nodeIndex, Queue queue, CommandBuffer commandBuffer)
  {
    os::Logger::logf(
        "[RenderGraph][Record] node=%u name=%s queue=%s cmd=%llu step=%s",
        nodeIndex,
        nodes[nodeIndex].name.c_str(),
        logQueue(queue),
        static_cast<unsigned long long>(commandBuffer),
        step);
  };

  enum Phase
  {
    Phase_Prepare = 0,
    Phase_PrepareRuntimeResources,
    Phase_PrepareCommands,
    Phase_AllocCmdBuffers,
    Phase_BuildWaits,
    Phase_Barriers,
    Phase_Commands,
    Phase_EndCmdBuffers,
    Phase_Submit,
    Phase_COUNT
  };

  static const char *phaseNames[Phase_COUNT] = {
    "Prepare",
    "PrepareResources",
    "PrepareCommands",
    "AllocCmdBuffers",
    "BuildWaits",
    "Barriers",
    "Commands",
    "EndCmdBuffers",
    "Submit",
  };

  auto phaseTime = [&](uint64_t &phaseSlot, auto &&fn)
  {
    const lib::time::TimeSpan phaseStart = lib::time::TimeSpan::now();
    fn();
    phaseSlot += static_cast<uint64_t>((lib::time::TimeSpan::now() - phaseStart).nanoseconds());
  };

  lastRunPhaseRecords.clear();
  lastRuntimeMetricRecords.clear();
  lastRunDebugStats = RunDebugStats{};
  lastRunTotalNs = 0u;

  const lib::time::TimeSpan runStart = lib::time::TimeSpan::now();
  uint64_t phaseNs[Phase_COUNT] = {};

  RenderGraphRuntimeCallbacks runtimeCallbacks = {};
#if RENDER_GRAPH_ENABLE_LOGGING && defined(RENDER_GRAPH_LOG_BARRIERS)
  runtimeCallbacks.logBarrier = [&](const std::string &message)
  {
    RG_LOG_BARRIER("%s", message.c_str());
  };
#else
  runtimeCallbacks.logBarrier = nullptr;
#endif

  RenderGraphCommandBufferDispatcher commandBufferDispatcher;
  RenderGraphBarrierDispatcher barrierDispatcher;
  RecordedCommandBuffers recorded;
  recorded.frameIndex = frameIndex;
  std::vector<uint32_t> sortedNodes(nodes.size(), 0u);
  recorded.nodeCommandBuffers.resize(nodes.size(), CommandBuffer::Invalid);

  logRecordProgress("phase=Prepare enter");
  phaseTime(
      phaseNs[Phase_Prepare],
      [&]()
      {
        for (uint32_t i = 0; i < nodes.size(); ++i)
        {
          sortedNodes[i] = i;
        }

        std::sort(
            sortedNodes.begin(),
            sortedNodes.end(),
            [this](const uint32_t &a, const uint32_t &b)
            {
              return nodes[a].level < nodes[b].level;
            });
      });

  logRecordProgress("phase=PrepareResources enter");
  phaseTime(
      phaseNs[Phase_PrepareRuntimeResources],
      [&]()
      {
        runtimeResourcesManager.beginRun(overrides, frameIndex);
      });

  logRecordProgress("phase=PrepareCommands enter");
  phaseTime(
      phaseNs[Phase_PrepareCommands],
      [&]()
      {
        CommandPrepareContext prepareContext = {
          .runtimeResources = &runtimeResourcesManager,
        };

        for (const uint32_t nodeIndex : sortedNodes)
        {
          auto &currentNode = nodes[nodeIndex];
          for (auto &command : currentNode.commands)
          {
            command->prepare(prepareContext);
          }
        }
      });

  logRecordProgress("phase=AllocCmdBuffers enter");
  phaseTime(
      phaseNs[Phase_AllocCmdBuffers],
      [&]()
      {
        for (const uint32_t nodeIndex : sortedNodes)
        {
          const Queue queue = nodes[nodeIndex].queue;
          CommandBuffer commandBuffer = commandBufferDispatcher.allocate(*this, queue, runtimeCallbacks);
          recorded.nodeCommandBuffers[nodeIndex] = commandBuffer;
          recorded.commandBuffers[queue].push_back(commandBuffer);
          recorded.nodes[commandBuffer].push_back(nodeIndex);
        }
      });

  logRecordProgress("phase=BuildWaits enter");
  phaseTime(
      phaseNs[Phase_BuildWaits],
      [&]()
      {
        for (const uint32_t nodeIndex : sortedNodes)
        {
          auto &currentNode = nodes[nodeIndex];
          const CommandBuffer commandBuffer = recorded.nodeCommandBuffers[nodeIndex];

          for (const auto &wait : currentNode.waitSemaphores)
          {
            auto &semaphore = semaphores[wait];
            const CommandBuffer fromCommandBuffer = recorded.nodeCommandBuffers[semaphore.signalTask];
            if (commandBuffer != fromCommandBuffer)
            {
              recorded.waits[commandBuffer].insert(fromCommandBuffer);
            }
          }

          for (const auto &dependency : interFrameDependencies)
          {
            if (matchesInterFrameDependencyTarget(currentNode.name, dependency.waitNodeName))
            {
              recorded.interFrameWaits[commandBuffer].push_back(dependency.signalNodeName);
            }
          }
        }
      });

  logRecordProgress("phase=RecordNodes enter");
  for (const uint32_t nodeIndex : sortedNodes)
  {
    auto &currentNode = nodes[nodeIndex];
    const CommandBuffer commandBuffer = recorded.nodeCommandBuffers[nodeIndex];

    logNodeRecordProgress("begin", nodeIndex, currentNode.queue, commandBuffer);
    phaseTime(
        phaseNs[Phase_AllocCmdBuffers],
        [&]()
        {
          commandBufferDispatcher.begin(*this, commandBuffer, runtimeCallbacks, oneTimeSubmit);
        });

    std::unordered_set<std::string> resetTimers;
    resetTimers.reserve(currentNode.timers.size());
    for (const auto &timerBinding : currentNode.timers)
    {
      if (!resetTimers.insert(timerBinding.name).second)
      {
        continue;
      }

      rhi->cmdResetTimer(commandBuffer, timerBinding.timer);
    }
    for (const auto &command : currentNode.commands)
    {
      auto *startTimer = dynamic_cast<StartTimerCommand *>(command.get());
      if (startTimer == nullptr)
      {
        continue;
      }

      if (!resetTimers.insert(startTimer->timer.name).second)
      {
        continue;
      }

      rhi->cmdResetTimer(commandBuffer, startTimer->timer);
    }
    for (const auto &timerBinding : currentNode.timers)
    {
      rhi->cmdStartTimer(commandBuffer, timerBinding.timer, 0u, timerBinding.startStage);
    }

    logNodeRecordProgress("pre-barriers", nodeIndex, currentNode.queue, commandBuffer);
    phaseTime(
        phaseNs[Phase_Barriers],
        [&]()
        {
          barrierDispatcher.submitPreBarriers(*this, nodeIndex, commandBuffer, runtimeResourcesManager, runtimeCallbacks);
        });

    logNodeRecordProgress("commands", nodeIndex, currentNode.queue, commandBuffer);
    phaseTime(
        phaseNs[Phase_Commands],
        [&]()
        {
          CommandRunContext runContext = {
            .rhi = rhi,
            .commandBuffer = commandBuffer,
            .runtimeResources = &runtimeResourcesManager,
          };

          runContext.callbacks.beginTiming = [](const char *)
          {
          };
          runContext.callbacks.endTiming = []()
          {
          };
          runContext.callbacks.log = [&](const std::string &message)
          {
            RG_LOG_CMD("%s", message.c_str());
          };

          for (const auto &command : currentNode.commands)
          {
            command->run(runContext);
          }
        });

    logNodeRecordProgress("post-barriers", nodeIndex, currentNode.queue, commandBuffer);
    phaseTime(
        phaseNs[Phase_Barriers],
        [&]()
        {
          barrierDispatcher.submitPostBarriers(*this, nodeIndex, commandBuffer, runtimeResourcesManager, runtimeCallbacks);
        });
    for (const auto &timerBinding : currentNode.timers)
    {
      rhi->cmdStopTimer(commandBuffer, timerBinding.timer, 0u, timerBinding.stopStage);
    }

    logNodeRecordProgress("end", nodeIndex, currentNode.queue, commandBuffer);
    phaseTime(
        phaseNs[Phase_EndCmdBuffers],
        [&]()
        {
          commandBufferDispatcher.end(*this, commandBuffer, runtimeCallbacks);
        });
  }

  logRecordProgress("phase=DetachRunResources enter");
  recorded.retainedBindingGroups = runtimeResourcesManager.detachRunResources().bindingGroups;

  lastRunPhaseRecords.reserve(Phase_COUNT);
  for (uint32_t phaseIndex = 0; phaseIndex < Phase_COUNT; ++phaseIndex)
  {
    lastRunPhaseRecords.push_back(
        RunPhaseRecord{
          .name = phaseNames[phaseIndex],
          .totalNs = phaseNs[phaseIndex],
        });
  }

  const auto &runtimeMetrics = runtimeResourcesManager.getMetricStats();
  lastRuntimeMetricRecords.reserve(runtimeMetrics.size());
  for (uint32_t metricIndex = 0; metricIndex < static_cast<uint32_t>(RenderGraphRuntimeResourcesManager::MetricId::Count); ++metricIndex)
  {
    const auto id = static_cast<RenderGraphRuntimeResourcesManager::MetricId>(metricIndex);
    const auto &metric = runtimeMetrics[metricIndex];
    if (metric.callCount == 0u && metric.totalNs == 0u && metric.maxNs == 0u)
    {
      continue;
    }

    lastRuntimeMetricRecords.push_back(
        RuntimeMetricRecord{
          .name = RenderGraphRuntimeResourcesManager::metricName(id),
          .totalNs = metric.totalNs,
          .callCount = metric.callCount,
          .maxNs = metric.maxNs,
        });
  }

  std::sort(
      lastRuntimeMetricRecords.begin(),
      lastRuntimeMetricRecords.end(),
      [](const RuntimeMetricRecord &a, const RuntimeMetricRecord &b)
      {
        if (a.totalNs != b.totalNs)
        {
          return a.totalNs > b.totalNs;
        }
        return a.name < b.name;
      });

  lastRunTotalNs = static_cast<uint64_t>((lib::time::TimeSpan::now() - runStart).nanoseconds());
  logRecordProgress("complete");
  return recorded;
}

void RenderGraph::submitRecordedCommandBuffers(Frame &frame, RecordedCommandBuffers &recorded, bool releaseCommandBuffersOnCompletion)
{
  auto logSubmitProgress = [&](const char *message)
  {
    os::Logger::logf("[RenderGraph][Submit] %s", message);
  };

  enum Phase
  {
    Phase_Prepare = 0,
    Phase_PrepareRuntimeResources,
    Phase_PrepareCommands,
    Phase_AllocCmdBuffers,
    Phase_BuildWaits,
    Phase_Barriers,
    Phase_Commands,
    Phase_EndCmdBuffers,
    Phase_Submit,
    Phase_COUNT
  };

  static const char *phaseNames[Phase_COUNT] = {
    "Prepare",
    "PrepareResources",
    "PrepareCommands",
    "AllocCmdBuffers",
    "BuildWaits",
    "Barriers",
    "Commands",
    "EndCmdBuffers",
    "Submit",
  };

  if (lastRunPhaseRecords.size() != Phase_COUNT)
  {
    lastRunPhaseRecords.clear();
    lastRunPhaseRecords.reserve(Phase_COUNT);
    for (uint32_t phaseIndex = 0; phaseIndex < Phase_COUNT; ++phaseIndex)
    {
      lastRunPhaseRecords.push_back(
          RunPhaseRecord{
            .name = phaseNames[phaseIndex],
            .totalNs = 0u,
          });
    }
  }

  const lib::time::TimeSpan submitStart = lib::time::TimeSpan::now();
  logSubmitProgress("enter");
  frame.futures.clear();
  frame.futures.reserve(Queue::QueuesCount);
  frame.submissionFutures.clear();
  frame.retainedBindingGroups.clear();

  if (!releaseCommandBuffersOnCompletion)
  {
    for (auto &future : recorded.inFlightFutures)
    {
      if (future.valid() && !rhi->isCompleted(future))
      {
        RENDER_GRAPH_FATAL("[RenderGraph] Recorded command buffer handle is already in flight");
      }
    }
    recorded.inFlightFutures.clear();
  }

  std::unordered_map<CommandBuffer, GPUFuture> futures;
  GPUFuture queueCompletionFutures[Queue::QueuesCount];
  uint64_t queueIndex[Queue::QueuesCount] = {};

  std::unordered_map<CommandBuffer, Queue> commandBufferQueues;
  std::unordered_map<CommandBuffer, uint64_t> commandBufferQueueOrders;
  for (Queue queue = Queue::None; queue < Queue::QueuesCount; queue = Queue((uint64_t)queue + 1))
  {
    commandBufferQueues.reserve(commandBufferQueues.size() + recorded.commandBuffers[queue].size());
    commandBufferQueueOrders.reserve(commandBufferQueueOrders.size() + recorded.commandBuffers[queue].size());
    for (uint64_t commandBufferIndex = 0u; commandBufferIndex < recorded.commandBuffers[queue].size(); ++commandBufferIndex)
    {
      const CommandBuffer commandBuffer = recorded.commandBuffers[queue][commandBufferIndex];
      commandBufferQueues.emplace(commandBuffer, queue);
      commandBufferQueueOrders.emplace(commandBuffer, commandBufferIndex);
    }
  }

  auto buildSubmissionName = [&](const Queue queue, const std::vector<CommandBuffer> &submittedCommandBuffers)
  {
    std::ostringstream name;
    name << "queue=" << logQueue(queue) << " cmdCount=" << submittedCommandBuffers.size();
    if (!submittedCommandBuffers.empty())
    {
      name << " cmds=";
      for (size_t commandBufferIndex = 0u; commandBufferIndex < submittedCommandBuffers.size(); ++commandBufferIndex)
      {
        if (commandBufferIndex != 0u)
        {
          name << ",";
        }
        name << static_cast<uint64_t>(submittedCommandBuffers[commandBufferIndex]);
      }
    }

    bool firstNode = true;
    bool wroteNodeHeader = false;
    for (const CommandBuffer commandBuffer : submittedCommandBuffers)
    {
      const auto commandBufferNodesIt = recorded.nodes.find(commandBuffer);
      if (commandBufferNodesIt == recorded.nodes.end() || commandBufferNodesIt->second.empty())
      {
        continue;
      }

      if (!wroteNodeHeader)
      {
        name << " nodes=";
        wroteNodeHeader = true;
      }

      for (const uint32_t submittedNodeIndex : commandBufferNodesIt->second)
      {
        if (!firstNode)
        {
          name << ",";
        }
        name << nodes[submittedNodeIndex].name;
        firstNode = false;
      }
    }

    return name.str();
  };

  while (true)
  {
    bool finished = true;
    for (Queue queue = Queue::None; queue < Queue::QueuesCount; queue = Queue((uint64_t)queue + 1))
    {
      if (queueIndex[queue] != recorded.commandBuffers[queue].size())
      {
        finished = false;
        break;
      }
    }

    if (finished)
    {
      break;
    }

    bool madeProgress = false;
    for (Queue queue = Queue::None; queue < Queue::QueuesCount; queue = Queue((uint64_t)queue + 1))
    {
      if (recorded.commandBuffers[queue].empty() || queueIndex[queue] == recorded.commandBuffers[queue].size())
      {
        continue;
      }

      const uint64_t batchStartIndex = queueIndex[queue];
      os::Logger::logf("[RenderGraph][Submit] queue=%s batchStart=%llu pending=%llu", logQueue(queue), static_cast<unsigned long long>(batchStartIndex), static_cast<unsigned long long>(recorded.commandBuffers[queue].size() - batchStartIndex));
      std::vector<CommandBuffer> readyCommandBuffers;
      std::vector<GPUFuture> waits;
      std::unordered_set<CommandBuffer> externalWaitCommandBuffers;
      readyCommandBuffers.reserve(recorded.commandBuffers[queue].size() - batchStartIndex);

      for (uint64_t scanIndex = batchStartIndex; scanIndex < recorded.commandBuffers[queue].size(); ++scanIndex)
      {
        const CommandBuffer commandBuffer = recorded.commandBuffers[queue][scanIndex];
        bool allDependenciesSubmitted = true;

        auto waitsIt = recorded.waits.find(commandBuffer);
        if (waitsIt != recorded.waits.end())
        {
          for (const CommandBuffer waitCommandBuffer : waitsIt->second)
          {
            const auto waitQueueIt = commandBufferQueues.find(waitCommandBuffer);
            const auto waitOrderIt = commandBufferQueueOrders.find(waitCommandBuffer);
            if (waitQueueIt != commandBufferQueues.end() && waitOrderIt != commandBufferQueueOrders.end() && waitQueueIt->second == queue)
            {
              if (waitOrderIt->second >= scanIndex)
              {
                allDependenciesSubmitted = false;
                break;
              }

              lastRunDebugStats.sameQueueWaitDependencyCount += 1u;
              continue;
            }

            const auto futureIt = futures.find(waitCommandBuffer);
            if (futureIt == futures.end())
            {
              allDependenciesSubmitted = false;
              break;
            }

            if (externalWaitCommandBuffers.insert(waitCommandBuffer).second)
            {
              waits.push_back(futureIt->second);
            }
          }
        }

        auto interFrameWaitsIt = recorded.interFrameWaits.find(commandBuffer);
        if (interFrameWaitsIt != recorded.interFrameWaits.end())
        {
          for (const std::string &signalNodeName : interFrameWaitsIt->second)
          {
            const auto signalFutureIt = lastInterFrameSignalFutures.find(signalNodeName);
            if (signalFutureIt == lastInterFrameSignalFutures.end())
            {
              continue;
            }

            auto &signalState = signalFutureIt->second;
            auto &signalFutures = signalState.futures;
            signalFutures.erase(
                std::remove_if(
                    signalFutures.begin(),
                    signalFutures.end(),
                    [&](GPUFuture &signalFuture)
                    {
                      return !signalFuture.valid() || rhi->isCompleted(signalFuture);
                    }),
                signalFutures.end());

            for (const GPUFuture &signalFuture : signalFutures)
            {
              waits.push_back(signalFuture);
            }
          }
        }

        if (!allDependenciesSubmitted)
        {
          break;
        }

        readyCommandBuffers.push_back(commandBuffer);
      }

      if (readyCommandBuffers.empty())
      {
        os::Logger::logf("[RenderGraph][Submit] queue=%s no-ready-command-buffers", logQueue(queue));
        continue;
      }

      os::Logger::logf("[RenderGraph][Submit] queue=%s submitting=%u waits=%u", logQueue(queue), static_cast<unsigned>(readyCommandBuffers.size()), static_cast<unsigned>(waits.size()));
      lastRunDebugStats.externalWaitDependencyCount += static_cast<uint32_t>(waits.size());
      GPUFuture submissionFuture = rhi->submit(
          queue,
          readyCommandBuffers.data(),
          static_cast<uint32_t>(readyCommandBuffers.size()),
          waits.data(),
          static_cast<uint32_t>(waits.size()),
          releaseCommandBuffersOnCompletion);

      for (const CommandBuffer submittedCommandBuffer : readyCommandBuffers)
      {
        futures[submittedCommandBuffer] = submissionFuture;
      }

      std::vector<RenderGraph::Frame::SubmissionFuture::NodeRecord> submissionNodeRecords;
      for (const CommandBuffer submittedCommandBuffer : readyCommandBuffers)
      {
        const auto submissionNodesIt = recorded.nodes.find(submittedCommandBuffer);
        if (submissionNodesIt == recorded.nodes.end())
        {
          continue;
        }

        for (const uint32_t submittedNode : submissionNodesIt->second)
        {
          const auto &submittedNodeInfo = nodes[submittedNode];
          submissionNodeRecords.push_back(
              RenderGraph::Frame::SubmissionFuture::NodeRecord{
                .name = submittedNodeInfo.name,
                .id = submittedNodeInfo.id,
                .level = submittedNodeInfo.level,
              });
        }
      }

      std::unordered_set<std::string> matchedSignalTargets;
      for (const auto &nodeRecord : submissionNodeRecords)
      {
        for (const auto &dependency : interFrameDependencies)
        {
          if (!matchesInterFrameDependencyTarget(nodeRecord.name, dependency.signalNodeName))
          {
            continue;
          }

          matchedSignalTargets.insert(dependency.signalNodeName);
        }
      }

      for (const std::string &signalTarget : matchedSignalTargets)
      {
        auto &signalState = lastInterFrameSignalFutures[signalTarget];
        if (signalState.frameIndex != recorded.frameIndex)
        {
          signalState.frameIndex = recorded.frameIndex;
          signalState.futures.clear();
        }
        signalState.futures.push_back(submissionFuture);
      }

      frame.submissionFutures.push_back(
          RenderGraph::Frame::SubmissionFuture{
            .future = submissionFuture,
            .name = buildSubmissionName(queue, readyCommandBuffers),
            .nodes = std::move(submissionNodeRecords),
            .queue = queue,
          });

      queueCompletionFutures[queue] = submissionFuture;
      queueIndex[queue] += readyCommandBuffers.size();
      lastRunDebugStats.submissionCount += 1u;
      lastRunDebugStats.submittedCommandBufferCount += static_cast<uint32_t>(readyCommandBuffers.size());
      madeProgress = true;
    }

    if (!madeProgress)
    {
      RENDER_GRAPH_FATAL("[RenderGraph] Submit scheduling stalled while pending command buffers remain");
    }
  }

  logSubmitProgress("complete");
  for (Queue queue = Queue::None; queue < Queue::QueuesCount; queue = Queue((uint64_t)queue + 1))
  {
    if (queueCompletionFutures[queue].valid())
    {
      frame.futures.push_back(queueCompletionFutures[queue]);
    }
  }

  if (releaseCommandBuffersOnCompletion)
  {
    frame.retainedBindingGroups = recorded.retainedBindingGroups;
  }
  else
  {
    recorded.inFlightFutures = frame.futures;
  }

  const auto featuresBits = static_cast<uint32_t>(rhi->getFeatures());
  auto chooseTimerResolveQueue = [&]() -> Queue
  {
    if ((featuresBits & static_cast<uint32_t>(DeviceFeatures_Graphics)) != 0u)
    {
      return Queue::Graphics;
    }
    if ((featuresBits & static_cast<uint32_t>(DeviceFeatures_Compute)) != 0u)
    {
      return Queue::Compute;
    }
    return Queue::None;
  };

  if (timerReadbackEnabled &&
      !timerReadbackScratch.empty() &&
      currentFrameIndex < timerReadbackBufferIds.size() &&
      timerReadbackBufferIds[currentFrameIndex] != BufferId::Invalid)
  {
    const Queue resolveQueue = chooseTimerResolveQueue();
    auto resolveCommandBuffers = resolveQueue != Queue::None
                                     ? rhi->allocateCommandBuffers(resolveQueue, 1u)
                                     : std::vector<CommandBuffer>{};
    if (!resolveCommandBuffers.empty())
    {
      const CommandBuffer resolveCommandBuffer = resolveCommandBuffers.front();
      rhi->beginCommandBuffer(resolveCommandBuffer, true);
      rhi->cmdResolveTimersToBuffer(
          resolveCommandBuffer,
          timerReadbackScratch.data(),
          static_cast<uint32_t>(timerReadbackScratch.size()),
          timerReadbackBufferIds[currentFrameIndex],
          0u);
      rhi->endCommandBuffer(resolveCommandBuffer);

      std::vector<GPUFuture> waits;
      waits.reserve(Queue::QueuesCount);
      for (Queue queue = Queue::None; queue < Queue::QueuesCount; queue = Queue((uint64_t)queue + 1))
      {
        if (queueCompletionFutures[queue].valid())
        {
          waits.push_back(queueCompletionFutures[queue]);
        }
      }

      GPUFuture resolveFuture = rhi->submit(
          resolveQueue,
          resolveCommandBuffers.data(),
          static_cast<uint32_t>(resolveCommandBuffers.size()),
          waits.data(),
          static_cast<uint32_t>(waits.size()),
          true);
      frame.futures.push_back(resolveFuture);
      frame.submissionFutures.push_back(
          RenderGraph::Frame::SubmissionFuture{
            .future = resolveFuture,
            .name = "TimerResolve",
            .queue = resolveQueue,
          });
      if (!releaseCommandBuffersOnCompletion)
      {
        recorded.inFlightFutures.push_back(resolveFuture);
      }
      lastRunDebugStats.submissionCount += 1u;
      lastRunDebugStats.submittedCommandBufferCount += static_cast<uint32_t>(resolveCommandBuffers.size());
    }
  }

  lastSubmissionCount = static_cast<uint32_t>(frame.submissionFutures.size());
  const uint64_t submitNs = static_cast<uint64_t>((lib::time::TimeSpan::now() - submitStart).nanoseconds());
  lastRunPhaseRecords[Phase_Submit].totalNs = submitNs;
  lastRunTotalNs += submitNs;
}

void RenderGraph::run(Frame &frame, CommandBufferHandle handle)
{
  auto recordedIt = recordedCommandBuffers.find(handle.value);
  if (recordedIt == recordedCommandBuffers.end())
  {
    RENDER_GRAPH_FATAL("[RenderGraph] Invalid recorded command buffer handle: %llu", handle.value);
  }

  lastRunPhaseRecords.clear();
  lastRuntimeMetricRecords.clear();
  lastRunDebugStats = RunDebugStats{};
  lastRunTotalNs = 0u;
  frame.frameIndex = recordedIt->second.frameIndex;
  setCurrentFrameIndex(frame.frameIndex);
  submitRecordedCommandBuffers(frame, recordedIt->second, false);
  executions += 1u;
}

void RenderGraph::run(Frame &frame, const Overrides &overrides)
{
  setCurrentFrameIndex(frame.frameIndex);
  RecordedCommandBuffers recorded = recordCommandBuffers(overrides, true, currentFrameIndex);
  submitRecordedCommandBuffers(frame, recorded, true);
  executions += 1u;
}

void RenderGraph::waitFrame(Frame &frame)
{
  const uint64_t waitFrameStartNs = currentSteadyClockNs();
  const uint64_t waitBlockStartNs = waitFrameStartNs;
  frameWaitSummary = FrameWaitSummary{};
#if REPORT_FUTURE_TIMINGS
  std::vector<bool> completedSubmissions(frame.submissionFutures.size(), false);
  size_t pendingSubmissions = frame.submissionFutures.size();

  auto markCompletedSubmissions = [&]()
  {
    for (size_t submissionIndex = 0; submissionIndex < frame.submissionFutures.size(); ++submissionIndex)
    {
      if (completedSubmissions[submissionIndex])
      {
        continue;
      }

      auto &submission = frame.submissionFutures[submissionIndex];
      if (submission.future.valid() && !rhi->isCompleted(submission.future))
      {
        continue;
      }
      completedSubmissions[submissionIndex] = true;
      pendingSubmissions -= 1u;
    }
  };

  markCompletedSubmissions();

  while (pendingSubmissions > 0u)
  {
    size_t nextPendingSubmissionIndex = frame.submissionFutures.size();
    for (size_t submissionIndex = 0; submissionIndex < frame.submissionFutures.size(); ++submissionIndex)
    {
      if (!completedSubmissions[submissionIndex])
      {
        nextPendingSubmissionIndex = submissionIndex;
        break;
      }
    }

    if (nextPendingSubmissionIndex == frame.submissionFutures.size())
    {
      break;
    }

    auto &submission = frame.submissionFutures[nextPendingSubmissionIndex];
    const uint64_t waitStartNs = currentSteadyClockNs();
    rhi->blockUntil(submission.future);
    const uint64_t waitEndNs = currentSteadyClockNs();
    const uint64_t waitNs = waitEndNs - waitStartNs;
    frameWaitSummary.blockedSubmissionCount += 1u;
    frameWaitSummary.waitedNodeCount += static_cast<uint32_t>(submission.nodes.size());
    frameWaitSummary.totalWaitNs += waitNs;
    markCompletedSubmissions();
  }
#else
  for (auto &future : frame.futures)
  {
    const uint64_t waitStartNs = currentSteadyClockNs();
    rhi->blockUntil(future);
    const uint64_t waitEndNs = currentSteadyClockNs();
    const uint64_t waitNs = waitEndNs - waitStartNs;
    frameWaitSummary.blockedSubmissionCount += 1u;
    frameWaitSummary.totalWaitNs += waitNs;
  }
#endif
  lastWaitFrameBlockSpanNs = currentSteadyClockNs() - waitBlockStartNs;
  lastTimerReadbackSpanNs = 0u;
  lastWaitFrameSpanNs = currentSteadyClockNs() - waitFrameStartNs;

  for (const BindingGroupsId bindingGroupsId : frame.retainedBindingGroups)
  {
    if (bindingGroupsId != BindingGroupsId::Invalid)
    {
      rhi->deleteBindingGroups(bindingGroupsId);
    }
  }

  frame.futures.clear();
  frame.submissionFutures.clear();
  frame.retainedBindingGroups.clear();
  rhi->flushDeferredDeletions();
}

double RenderGraph::readTimer(const Timer &timer)
{
  return rhi->readTimer(timer);
}

void RenderGraph::setCurrentFrameIndex(uint32_t frameIndex)
{
  currentFrameIndex = settings.maxFramesInFlight == 0u ? 0u : (frameIndex % settings.maxFramesInFlight);
}

uint32_t RenderGraph::getCurrentFrameIndex() const
{
  return currentFrameIndex;
}

uint32_t RenderGraph::getMaxFramesInFlight() const
{
  return std::max(1u, settings.maxFramesInFlight);
}

static void validateBufferUsage(const BufferInfo &info)
{
  const BufferUsage u = info.usage;

  auto has = [&](BufferUsage flag)
  {
    return (u & flag) != 0;
  };

  if (has(BufferUsage_Push) && has(BufferUsage_Pull))
  {
    RENDER_GRAPH_FATAL("[RenderGraph] Buffer '%s' cannot have both MAP_WRITE (Push) and MAP_READ (Pull)", info.name.c_str());
  }

  if (has(BufferUsage_Pull))
  {
    if (!has(BufferUsage_CopyDst))
    {
      RENDER_GRAPH_FATAL("[RenderGraph] Buffer '%s' BufferUsage_Pull (Pull) requires BufferUsage_CopyDst usage", info.name.c_str());
    }

    if (has(BufferUsage_Storage) || has(BufferUsage_Uniform) || has(BufferUsage_Vertex) || has(BufferUsage_Index) || has(BufferUsage_Indirect) || has(BufferUsage_Timestamp) || has(BufferUsage_CopySrc))
    {
      RENDER_GRAPH_FATAL("[RenderGraph] Buffer '%s' BufferUsage_Pull buffers may not have GPU write or bind usages", info.name.c_str());
    }
  }

  if (has(BufferUsage_Push) && has(BufferUsage_CopyDst))
  {
    RENDER_GRAPH_FATAL("[RenderGraph] Buffer '%s' BufferUsage_Push buffers cannot have BufferUsage_CopyDst usage", info.name.c_str());
  }

  if (has(BufferUsage_CopySrc))
  {
    if (has(BufferUsage_Pull))
    {
      RENDER_GRAPH_FATAL("[RenderGraph] Buffer '%s' BufferUsage_CopySrc buffers cannot be BufferUsage_Pull", info.name.c_str());
    }
  }

  if (has(BufferUsage_CopyDst))
  {
    if (has(BufferUsage_Push))
    {
      RENDER_GRAPH_FATAL("[RenderGraph] Buffer '%s' BufferUsage_CopyDst buffers cannot be BufferUsage_Push", info.name.c_str());
    }
  }

  if (has(BufferUsage_Timestamp))
  {
    if (has(BufferUsage_Push) || has(BufferUsage_Pull))
    {
      RENDER_GRAPH_FATAL("[RenderGraph] Buffer '%s' Timestamp buffers cannot be CPU mapped", info.name.c_str());
    }

    if (has(BufferUsage_Storage) || has(BufferUsage_Uniform) || has(BufferUsage_Vertex) || has(BufferUsage_Index))
    {
      RENDER_GRAPH_FATAL("[RenderGraph] Buffer '%s' Timestamp buffers cannot be bound to shaders", info.name.c_str());
    }

    if (!has(BufferUsage_CopyDst))
    {
      RENDER_GRAPH_FATAL("[RenderGraph] Buffer '%s' Timestamp buffers must include CopyDst usage", info.name.c_str());
    }
  }

  if (has(BufferUsage_Uniform))
  {
    if (has(BufferUsage_Pull))
    {
      RENDER_GRAPH_FATAL("[RenderGraph] Buffer '%s' Uniform buffers cannot be BufferUsage_Pull", info.name.c_str());
    }

    if (has(BufferUsage_Storage))
    {
      RENDER_GRAPH_FATAL("[RenderGraph] Buffer '%s' cannot be both BufferUsage_Uniform and BufferUsage_Storage", info.name.c_str());
    }
  }

  if ((has(BufferUsage_Vertex) || has(BufferUsage_Index)) && has(BufferUsage_Pull))
  {
    RENDER_GRAPH_FATAL("[RenderGraph] Buffer '%s' Vertex/Index buffers cannot be MAP_READ", info.name.c_str());
  }

  if (!(has(BufferUsage_CopySrc) || has(BufferUsage_CopyDst) || has(BufferUsage_Uniform) || has(BufferUsage_Storage) || has(BufferUsage_Vertex) || has(BufferUsage_Index) || has(BufferUsage_Indirect) ||
        has(BufferUsage_Timestamp)))
  {
    RENDER_GRAPH_FATAL("[RenderGraph] Buffer '%s' has no GPU-visible usage flags", info.name.c_str());
  }
}

CommandRecorder::CommandRecorder()
{
  recorded.push_back(CommandSequence());
}

Pass::~Pass()
{
}

void Pass::instrumentTimedCommands()
{
  // Timing is injected after node splitting in PassesAnalysis.
}

void CommandRecorder::beginSequence()
{
  if (recorded.empty() || !recorded.back().commands.empty())
  {
    recorded.push_back(CommandSequence());
  }
}

void RenderGraph::deleteBuffer(const Buffer &name)
{
  auto it = resources.bufferMetadatas.find(name.name);
  if (it == resources.bufferMetadatas.end())
  {
    RENDER_GRAPH_FATAL("Buffer %s not found", name.name.c_str());
  }

  if (it->second.resourceId != BufferId::Invalid)
  {
    rhi->deleteBuffer(it->second.resourceId);
  }

  resources.bufferMetadatas.erase(it);

  releaseAllRecordedCommandBuffers();
  compiled = false;
  runtimeResourcesManager.releaseBuffer(name.name);
}

void RenderGraph::deleteTexture(const Texture &name)
{
  auto it = resources.textureMetadatas.find(name.name);
  if (it == resources.textureMetadatas.end())
  {
    RENDER_GRAPH_FATAL("Texture %s not found", name.name.c_str());
  }

  if (it->second.resourceId != TextureId::Invalid)
  {
    rhi->deleteTexture(it->second.resourceId);
  }

  resources.textureMetadatas.erase(it);
  releaseAllRecordedCommandBuffers();
  compiled = false;
  runtimeResourcesManager.releaseTexture(name.name);
}

void RenderGraph::deleteSampler(const Sampler &name)
{
  auto it = resources.samplerMetadatas.find(name.name);
  if (it == resources.samplerMetadatas.end())
  {
    RENDER_GRAPH_FATAL("Sampler %s not found", name.name.c_str());
  }
  rhi->deleteSampler(it->second.resourceId);
  resources.samplerMetadatas.erase(it);
  releaseAllRecordedCommandBuffers();
  compiled = false;
}

void RenderGraph::deleteBindingsLayout(const BindingsLayout &name)
{
  auto it = resources.bindingsLayoutMetadata.find(name.name);
  if (it == resources.bindingsLayoutMetadata.end())
  {
    RENDER_GRAPH_FATAL("Bindings Layout %s not found", name.name.c_str());
  }

  rhi->deleteBindingsLayout(it->second.resourceId);
  resources.bindingsLayoutMetadata.erase(it);
  releaseAllRecordedCommandBuffers();
  compiled = false;
}

void RenderGraph::deleteBindingGroups(const BindingGroups &name)
{
  auto it = resources.bindingGroupsMetadata.find(name.name);
  if (it == resources.bindingGroupsMetadata.end())
  {
    RENDER_GRAPH_FATAL("Binding Groups %s not found", name.name.c_str());
  }

  if (it->second.resourceId != BindingGroupsId::Invalid)
  {
    rhi->deleteBindingGroups(it->second.resourceId);
  }

  resources.bindingGroupsMetadata.erase(it);

  runtimeResourcesManager.releaseBindingGroups(name.name);
  releaseAllRecordedCommandBuffers();
  compiled = false;
}

void RenderGraph::deleteGraphicsPipeline(const GraphicsPipeline &name)
{
  auto it = resources.graphicsPipelineMetadata.find(name.name);
  if (it == resources.graphicsPipelineMetadata.end())
  {
    RENDER_GRAPH_FATAL("Graphics Pipeline %s not found", name.name.c_str());
  }
  rhi->deleteGraphicsPipeline(it->second.resourceId);
  resources.graphicsPipelineMetadata.erase(it);
  releaseAllRecordedCommandBuffers();
  compiled = false;
}

void RenderGraph::deleteComputePipeline(const ComputePipeline &name)
{
  auto it = resources.computePipelineMetadata.find(name.name);
  if (it == resources.computePipelineMetadata.end())
  {
    RENDER_GRAPH_FATAL("Compute Pipeline %s not found", name.name.c_str());
  }
  rhi->deleteComputePipeline(it->second.resourceId);
  resources.computePipelineMetadata.erase(it);
  releaseAllRecordedCommandBuffers();
  compiled = false;
}

const Buffer RenderGraph::createBuffer(const BufferInfo &info)
{
  if (resources.bufferMetadatas.count(info.name) > 0)
  {
    throw std::runtime_error("Buffer already created");
  }

  validateBufferUsage(info);

  resources.bufferMetadatas[info.name] = BufferResourceMetadata{
    .bufferInfo = info,
    .resourceId = BufferId::Invalid,
    .firstUsedAt = UINT64_MAX,
    .lastUsedAt = 0,
  };
  releaseAllRecordedCommandBuffers();
  compiled = false;

  if (!info.isVirtual && !info.scratch)
  {
    os::print("Creating buffer %s\n", info.name.c_str());
    resources.bufferMetadatas[info.name].resourceId = rhi->createBuffer(info);
  }

  return Buffer{.name = info.name};
}

const Texture RenderGraph::createTexture(const TextureInfo &info)
{
  if (resources.textureMetadatas.count(info.name) > 0)
  {
    throw std::runtime_error("Texture already created");
  }
  resources.textureMetadatas[info.name] = TextureResourceMetadata{
    .textureInfo = info,
    .resourceId = TextureId::Invalid,
  };
  releaseAllRecordedCommandBuffers();
  compiled = false;

  if (!info.isVirtual)
  {
    resources.textureMetadatas[info.name].resourceId = rhi->createTexture(info);
  }

  return Texture{.name = info.name};
}

const Sampler RenderGraph::createSampler(const SamplerInfo &info)
{
  if (resources.samplerMetadatas.count(info.name) > 0)
  {
    throw std::runtime_error("Sampler already created");
  }
  resources.samplerMetadatas[info.name] = SamplerResourceMetadata{
    .samplerInfo = info,
    .resourceId = rhi->createSampler(info),
  };
  releaseAllRecordedCommandBuffers();
  compiled = false;

  return Sampler{.name = info.name, .id = resources.samplerMetadatas[info.name].resourceId};
}

bool isSamplerCompatible(ResourceLayout layout)
{
  switch (layout)
  {
  case ResourceLayout::SHADER_READ_ONLY:
  case ResourceLayout::GENERAL:
  case ResourceLayout::DEPTH_STENCIL_READ_ONLY:
    return true;
  default:
    return false;
  }
}

const BindingGroups RenderGraph::createBindingGroups(const BindingGroupsInfo &info)
{
  // TODO: check that all textures and samplers define an access and layout that is not undefined
  const auto &name = info.name;

  if (resources.bindingGroupsMetadata.count(info.name) > 0)
  {
    throw std::runtime_error("Binding Groups already created");
  }

  auto &layoutObject = resources.bindingsLayoutMetadata.at(info.layout.name);

  if (layoutObject.layoutsInfo.groups.size() != info.groups.size())
  {
    RENDER_GRAPH_FATAL("[RenderGraph] binding groups %s size don't match given layout %s", info.name.c_str(), info.layout.name.c_str());
  }

  for (uint32_t i = 0; i < info.groups.size(); i++)
  {
    if (layoutObject.layoutsInfo.groups[i].buffers.size() != info.groups[i].buffers.size())
    {
      RENDER_GRAPH_FATAL("[RenderGraph] binding groups %s buffers size don't match given layout %s", info.name.c_str(), info.layout.name.c_str());
    }
    if (layoutObject.layoutsInfo.groups[i].samplers.size() != info.groups[i].samplers.size())
    {
      RENDER_GRAPH_FATAL("[RenderGraph] binding groups %s samplers size don't match given layout %s", info.name.c_str(), info.layout.name.c_str());
    }
    if (layoutObject.layoutsInfo.groups[i].storageTextures.size() != info.groups[i].storageTextures.size())
    {
      RENDER_GRAPH_FATAL("[RenderGraph] binding groups %s storageTextures size don't match given layout %s", info.name.c_str(), info.layout.name.c_str());
    }
    if (layoutObject.layoutsInfo.groups[i].textures.size() != info.groups[i].textures.size())
    {
      RENDER_GRAPH_FATAL("[RenderGraph] binding groups %s textures size don't match given layout %s", info.name.c_str(), info.layout.name.c_str());
    }
  }

  for (uint32_t i = 0; i < info.groups.size(); i++)
  {
    for (uint32_t j = 0; j < layoutObject.layoutsInfo.groups[i].buffers.size(); j++)
    {
      if (layoutObject.layoutsInfo.groups[i].buffers[j].type == BufferBindingType::BufferBindingType_StorageBuffer)
      {
        auto usage = resources.bufferMetadatas.at(info.groups[i].buffers[j].bufferView.buffer.name).bufferInfo.usage;
        if ((usage & BufferUsage::BufferUsage_Storage) == 0)
        {
          RENDER_GRAPH_FATAL(
              "[RenderGraph] binding groups %s at group %u, binding %u, buffer %s bound with type BufferBindingType_StorageBuffer, but buffer usage did not include "
              "BufferUsage_Storage",
              info.name.c_str(),
              i,
              info.groups[i].buffers[j].binding,
              info.groups[i].buffers[j].bufferView.buffer.name.c_str());
        }
      }

      if (layoutObject.layoutsInfo.groups[i].buffers[j].type == BufferBindingType::BufferBindingType_UniformBuffer)
      {
        auto usage = resources.bufferMetadatas.at(info.groups[i].buffers[j].bufferView.buffer.name).bufferInfo.usage;
        if ((usage & BufferUsage::BufferUsage_Uniform) == 0)
        {
          RENDER_GRAPH_FATAL(
              "[RenderGraph] binding groups %s at group %u, buffer %s bound with type BufferBindingType_UniformBuffer, but buffer usage did not include BufferUsage_Uniform",
              info.name.c_str(),
              i,
              info.groups[i].buffers[j].bufferView.buffer.name.c_str());
        }
      }
    }
  }

  for (uint32_t i = 0; i < info.groups.size(); i++)
  {
    for (uint32_t j = 0; j < info.groups[i].samplers.size(); j++)
    {
      if (!isSamplerCompatible(info.groups[i].samplers[j].view.layout))
      {
        RENDER_GRAPH_FATAL("[RenderGraph] Invalid layout for sampler %s in group %i, expects GENERAL, SHADER_READ_ONLY or DEPTH_STENCIL_READ_ONLY", info.groups[i].samplers[j].sampler.name.c_str(), i);
      }
    }
  }

  resources.bindingGroupsMetadata[name] = BindingGroupsResourceMetadata{
    .groupsInfo = info,
    .resourceId = BindingGroupsId::Invalid,
  };
  releaseAllRecordedCommandBuffers();
  compiled = false;

  assert(resources.bindingGroupsMetadata.count(name) > 0);

  return BindingGroups{.name = info.name};
}

const GraphicsPipeline RenderGraph::createGraphicsPipeline(const GraphicsPipelineInfo &info)
{
  if (resources.graphicsPipelineMetadata.count(info.name) > 0)
  {
    throw std::runtime_error("Graphics Pipeline already created");
  }

  resources.graphicsPipelineMetadata[info.name] = GraphicsPipelineResourceMetadata{
    .pipelineInfo = info,
    .resourceId = rhi->createGraphicsPipeline(info),
  };
  releaseAllRecordedCommandBuffers();
  compiled = false;

  return GraphicsPipeline{.name = info.name, .id = resources.graphicsPipelineMetadata[info.name].resourceId};
}

const ComputePipeline RenderGraph::createComputePipeline(const ComputePipelineInfo &info)
{
  if (resources.computePipelineMetadata.count(info.name) > 0)
  {
    throw std::runtime_error("Compute Pipeline already created");
  }

  resources.computePipelineMetadata[info.name] = ComputePipelineResourceMetadata{
    .pipelineInfo = info,
    .resourceId = rhi->createComputePipeline(info),
  };
  releaseAllRecordedCommandBuffers();
  compiled = false;

  return ComputePipeline{.name = info.name, .id = resources.computePipelineMetadata[info.name].resourceId};
}

const BindingsLayout RenderGraph::createBindingsLayout(const BindingsLayoutInfo &info)
{
  if (resources.bindingsLayoutMetadata.count(info.name) > 0)
  {
    throw std::runtime_error("Binding Layout already created");
  }

  resources.bindingsLayoutMetadata[info.name] = BindingsLayoutResourceMetadata{
    .layoutsInfo = info,
    .resourceId = rhi->createBindingsLayout(info),
  };
  releaseAllRecordedCommandBuffers();
  compiled = false;

  assert(resources.bindingsLayoutMetadata.count(info.name) > 0);
  return BindingsLayout{.name = info.name, .id = resources.bindingsLayoutMetadata[info.name].resourceId};
}

const Shader RenderGraph::createShader(const ShaderInfo info)
{
  if (resources.shadersMetadatas.count(info.name) > 0)
  {
    throw std::runtime_error("Shader already created");
  }

  resources.shadersMetadatas[info.name] = ShaderResourceMetadata{
    .info = info,
    .resourceId = rhi->createShader(info),
  };
  releaseAllRecordedCommandBuffers();
  compiled = false;

  return Shader{.name = info.name, .id = resources.shadersMetadatas[info.name].resourceId};
}

void RenderGraph::deleteShader(Shader handle)
{
  auto it = resources.shadersMetadatas.find(handle.name);
  if (it == resources.shadersMetadatas.end())
  {
    RENDER_GRAPH_FATAL("Shader %s not found", handle.name.c_str());
  }

  rhi->deleteShader(it->second.resourceId);
  resources.shadersMetadatas.erase(it);
  releaseAllRecordedCommandBuffers();
  compiled = false;
}

void RenderGraph::bufferRead(const Buffer &buffer, const uint64_t offset, const uint64_t size, std::function<void(const void *)> callback)
{
  const auto start = lib::time::TimeSpan::now();
  rhi->bufferRead(runtimeResourcesManager.resolveBuffer(buffer), offset, size, callback);
  const uint64_t elapsedNs = static_cast<uint64_t>((lib::time::TimeSpan::now() - start).nanoseconds());
  cpuStats.bufferReads.callCount += 1u;
  cpuStats.bufferReads.totalBytes += size;
  cpuStats.bufferReads.totalNs += elapsedNs;
  cpuStats.bufferReads.maxNs = std::max(cpuStats.bufferReads.maxNs, elapsedNs);
}

void RenderGraph::bufferWrite(const Buffer &buffer, const uint64_t offset, const uint64_t size, void *data)
{
  // TODO: check buffer usage
  const auto start = lib::time::TimeSpan::now();
  rhi->bufferWrite(runtimeResourcesManager.resolveBuffer(buffer), offset, size, data);
  const uint64_t elapsedNs = static_cast<uint64_t>((lib::time::TimeSpan::now() - start).nanoseconds());
  cpuStats.bufferWrites.callCount += 1u;
  cpuStats.bufferWrites.totalBytes += size;
  cpuStats.bufferWrites.totalNs += elapsedNs;
  cpuStats.bufferWrites.maxNs = std::max(cpuStats.bufferWrites.maxNs, elapsedNs);
}

void RenderGraph::resetCpuStats()
{
  cpuStats = CpuStats{};
}

RenderGraph::CpuStats RenderGraph::getCpuStats() const
{
  return cpuStats;
}

double RenderGraph::getLastRunTotalMs() const
{
  return static_cast<double>(lastRunTotalNs) / 1e6;
}

const std::vector<RenderGraph::RunPhaseRecord> &RenderGraph::getLastRunPhaseRecords() const
{
  return lastRunPhaseRecords;
}

const std::vector<RenderGraph::RuntimeMetricRecord> &RenderGraph::getLastRuntimeMetricRecords() const
{
  return lastRuntimeMetricRecords;
}

const RenderGraph::RunDebugStats &RenderGraph::getLastRunDebugStats() const
{
  return lastRunDebugStats;
}

double RenderGraph::getLastWaitFrameSpanMs() const
{
  return static_cast<double>(lastWaitFrameSpanNs) / 1e6;
}

double RenderGraph::getLastWaitFrameBlockSpanMs() const
{
  return static_cast<double>(lastWaitFrameBlockSpanNs) / 1e6;
}

double RenderGraph::getLastTimerReadbackSpanMs() const
{
  return static_cast<double>(lastTimerReadbackSpanNs) / 1e6;
}

void RenderGraph::setTimerReadbackEnabled(bool enabled)
{
  timerReadbackEnabled = enabled;
}

bool RenderGraph::isTimerReadbackEnabled() const
{
  return timerReadbackEnabled;
}

void RenderGraph::addInterFrameDependency(std::string waitNodeName, std::string signalNodeName)
{
  releaseAllRecordedCommandBuffers();
  interFrameDependencies.push_back(
      InterFrameDependency{
          .waitNodeName = std::move(waitNodeName),
          .signalNodeName = std::move(signalNodeName),
      });
  lastInterFrameSignalFutures.clear();
}

void RenderGraph::clearInterFrameDependencies()
{
  releaseAllRecordedCommandBuffers();
  interFrameDependencies.clear();
  lastInterFrameSignalFutures.clear();
}

void RenderGraph::logFrameStats() const
{
  const auto &passSummaries = getPassTimingSummaries();
  if (frameWaitSummary.blockedSubmissionCount == 0u && passSummaries.empty())
  {
    return;
  }

  const double totalPassGpuMs = std::accumulate(
      passSummaries.begin(),
      passSummaries.end(),
      0.0,
      [](double total, const PassTimingSummary &record)
      {
        return total + record.gpuTimeMs;
      });
  const uint32_t totalTimerCount = std::accumulate(
      passSummaries.begin(),
      passSummaries.end(),
      0u,
      [](uint32_t total, const PassTimingSummary &record)
      {
        return total + record.timerCount;
      });
  const double maxPassGpuMs = std::accumulate(
      passSummaries.begin(),
      passSummaries.end(),
      0.0,
      [](double maxValue, const PassTimingSummary &record)
      {
        return std::max(maxValue, record.gpuTimeMs);
      });
  const double avgBlockedWaitMs =
      frameWaitSummary.blockedSubmissionCount > 0u
          ? (static_cast<double>(frameWaitSummary.totalWaitNs) / static_cast<double>(frameWaitSummary.blockedSubmissionCount) / 1e6)
          : 0.0;
  const double avgPassGpuMs =
      !passSummaries.empty()
          ? (totalPassGpuMs / static_cast<double>(passSummaries.size()))
          : 0.0;

  std::ostringstream summary;
  summary << "[RenderGraph][FrameStats] submissions=" << lastSubmissionCount
          << " cpuWaitAvg=" << std::fixed << std::setprecision(3) << avgBlockedWaitMs << "ms"
          << " cpuWaitTotal=" << (static_cast<double>(frameWaitSummary.totalWaitNs) / 1e6) << "ms"
          << " cpuWaitBlocked=" << frameWaitSummary.blockedSubmissionCount
          << " cpuWaitBlock=" << (static_cast<double>(lastWaitFrameBlockSpanNs) / 1e6) << "ms"
          << " cpuWaitSpan=" << (static_cast<double>(lastWaitFrameSpanNs) / 1e6) << "ms"
          << " timerReadback=" << (static_cast<double>(lastTimerReadbackSpanNs) / 1e6) << "ms"
          << " gpuPassAvg=" << avgPassGpuMs << "ms"
          << " gpuPassTotal=" << totalPassGpuMs << "ms"
          << " gpuPassMax=" << maxPassGpuMs << "ms"
          << " passCount=" << passSummaries.size()
          << " timerCount=" << totalTimerCount;
  os::Logger::log(summary.str());

}

void RenderGraph::clearFrameStats()
{
  frameWaitSummary = FrameWaitSummary{};
  timerReadbackRecords.clear();
  passTimingSummaries.clear();
  passTimingSummariesDirty = true;
  lastWaitFrameSpanNs = 0u;
  lastWaitFrameBlockSpanNs = 0u;
  lastTimerReadbackSpanNs = 0u;
  lastRunPhaseRecords.clear();
  lastRuntimeMetricRecords.clear();
  lastRunDebugStats = RunDebugStats{};
  lastRunTotalNs = 0u;
  lastSubmissionCount = 0u;
  cpuStats = CpuStats{};
}

const std::vector<RenderGraph::PassTimingSummary> &RenderGraph::getPassTimingSummaries() const
{
  if (passTimingSummariesDirty)
  {
    rebuildPassTimingSummaries();
  }

  return passTimingSummaries;
}

const std::vector<RenderGraph::TimerReadbackRecord> &RenderGraph::getTimerReadbackRecords() const
{
  return timerReadbackRecords;
}

std::vector<RenderGraph::TimerReadbackRecord> RenderGraph::readResolvedTimerRecords(uint32_t frameIndex) const
{
  if (frameIndex >= timerReadbackBufferIds.size() ||
      timerReadbackBufferIds[frameIndex] == BufferId::Invalid ||
      timerReadbackBufferSize == 0u ||
      timerReadbackMetadata.empty())
  {
    return {};
  }

  std::vector<uint64_t> resolvedQueries(timerReadbackBufferSize / sizeof(uint64_t), 0u);
  rhi->bufferRead(
      timerReadbackBufferIds[frameIndex],
      0u,
      timerReadbackBufferSize,
      [&resolvedQueries](const void *gpuData)
      {
        std::memcpy(resolvedQueries.data(), gpuData, resolvedQueries.size() * sizeof(uint64_t));
      });

  const double timestampPeriodNs = rhi->getTimestampPeriodNs();
  std::vector<TimerReadbackRecord> records(timerReadbackMetadata.size());
  for (size_t timerIndex = 0u; timerIndex < timerReadbackMetadata.size(); ++timerIndex)
  {
    const TimerReadbackMetadata &metadata = timerReadbackMetadata[timerIndex];
    const uint64_t firstQuery = metadata.resolvedBufferOffset / sizeof(uint64_t);
    const uint32_t sampleCount = std::max(1u, metadata.timer.sampleCount);

    double timeNs = 0.0;
    for (uint32_t sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
    {
      const uint64_t queryIndex = firstQuery + static_cast<uint64_t>(sampleIndex) * 2u;
      const uint64_t start = resolvedQueries[queryIndex + 0u];
      const uint64_t end = resolvedQueries[queryIndex + 1u];
      if (end >= start)
      {
        timeNs += static_cast<double>(end - start) * timestampPeriodNs;
      }
    }

    double resolvedValue = timeNs * 1e-6;
    switch (metadata.unit)
    {
    case TimerUnit::Nanoseconds:
      resolvedValue = timeNs;
      break;
    case TimerUnit::Miliseconds:
      resolvedValue = timeNs * 1e-6;
      break;
    case TimerUnit::Seconds:
      resolvedValue = timeNs * 1e-9;
      break;
    }

    records[timerIndex] = TimerReadbackRecord{
      .passName = metadata.passName,
      .timerName = metadata.timerName,
      .passIndex = metadata.passIndex,
      .gpuTimeMs = resolvedValue,
      .hasComputeWork = metadata.hasComputeWork,
      .hasGraphicsWork = metadata.hasGraphicsWork,
    };
  }

  return records;
}

void RenderGraph::rebuildTimerReadbackMetadata()
{
  timerReadbackMetadata.clear();
  timerReadbackScratch.clear();
  timerReadbackBufferSize = 0u;

  size_t timerCount = 0u;
  for (const auto &node : nodes)
  {
    timerCount += node.timers.size();
  }

  timerReadbackMetadata.reserve(timerCount);
  timerReadbackScratch.reserve(timerCount);
  uint64_t resolvedBufferOffset = 0u;
  for (const auto &node : nodes)
  {
    for (const auto &timerBinding : node.timers)
    {
      const uint32_t resolvedQueryCount = std::max(1u, timerBinding.info.sampleCount) * 2u;
      timerReadbackMetadata.push_back(
          TimerReadbackMetadata{
            .passName = node.name,
            .timerName = timerBinding.name,
            .passIndex = static_cast<uint32_t>(node.id),
            .hasComputeWork = timerBinding.hasComputeWork,
            .hasGraphicsWork = timerBinding.hasGraphicsWork,
            .unit = timerBinding.info.unit,
            .resolvedBufferOffset = resolvedBufferOffset,
            .resolvedQueryCount = resolvedQueryCount,
            .timer = timerBinding.timer,
          });
      timerReadbackScratch.push_back(timerBinding.timer);
      resolvedBufferOffset += static_cast<uint64_t>(resolvedQueryCount) * sizeof(uint64_t);
    }
  }
  timerReadbackBufferSize = resolvedBufferOffset;

  timerReadbackValuesScratch.clear();
  timerReadbackValuesScratch.resize(timerReadbackScratch.size(), 0.0);
  timerReadbackRecords.clear();
  passTimingSummaries.clear();
  passTimingSummariesDirty = true;
}

void RenderGraph::releaseTimerReadbackBuffers()
{
  for (const BufferId bufferId : timerReadbackBufferIds)
  {
    if (bufferId != BufferId::Invalid)
    {
      rhi->deleteBuffer(bufferId);
    }
  }
  timerReadbackBufferIds.clear();
}

void RenderGraph::rebuildTimerReadbackBuffers()
{
  releaseTimerReadbackBuffers();
  if (timerReadbackBufferSize == 0u)
  {
    return;
  }

  const uint32_t frameCount = getMaxFramesInFlight();
  timerReadbackBufferIds.assign(frameCount, BufferId::Invalid);
  for (uint32_t frameSlot = 0u; frameSlot < frameCount; ++frameSlot)
  {
    timerReadbackBufferIds[frameSlot] = rhi->createBuffer(
        BufferInfo{
          .name = "RenderGraphTimers.readback.frame" + std::to_string(frameSlot),
          .size = timerReadbackBufferSize,
          .usage = BufferUsage_Timestamp | BufferUsage_CopyDst | BufferUsage_Pull,
        });
  }
}

void RenderGraph::rebuildPassTimingSummaries() const
{
  passTimingSummaries.clear();
  passTimingSummaries.reserve(timerReadbackRecords.size());

  std::unordered_map<std::string, size_t> passIndices;
  passIndices.reserve(timerReadbackRecords.size());
  for (const TimerReadbackRecord &record : timerReadbackRecords)
  {
    const auto [it, inserted] = passIndices.emplace(record.passName, passTimingSummaries.size());
    if (inserted)
    {
      passTimingSummaries.push_back(
          PassTimingSummary{
            .name = record.passName,
            .passIndex = record.passIndex,
          });
    }

    PassTimingSummary &summary = passTimingSummaries[it->second];
    summary.gpuTimeMs += record.gpuTimeMs;
    summary.timerCount += 1u;
    summary.hasComputeWork = summary.hasComputeWork || record.hasComputeWork;
    summary.hasGraphicsWork = summary.hasGraphicsWork || record.hasGraphicsWork;
  }

  std::sort(
      passTimingSummaries.begin(),
      passTimingSummaries.end(),
      [](const PassTimingSummary &a, const PassTimingSummary &b)
      {
        if (a.passIndex != b.passIndex)
        {
          return a.passIndex < b.passIndex;
        }

        return a.name < b.name;
      });

  passTimingSummariesDirty = false;
}

const BindingGroups RenderGraph::getBindingGroups(const std::string &name)
{
  return resources.getBindingGroups(name);
}
const GraphicsPipeline RenderGraph::getGraphicsPipeline(const std::string &name)
{
  return resources.getGraphicsPipeline(name);
}
const ComputePipeline RenderGraph::getComputePipeline(const std::string &name)
{
  return resources.getComputePipeline(name);
}
const BindingsLayout RenderGraph::getBindingsLayout(const std::string &name)
{
  return resources.getBindingsLayout(name);
}
const Sampler RenderGraph::getSampler(const std::string &name)
{
  return resources.getSampler(name);
}
const Buffer RenderGraph::getBuffer(const std::string &name)
{
  return resources.getBuffer(name);
}
// const Buffer RenderGraph::getScratchBuffer(const std::string &name)
// {
//   return resources.getScratchBuffer(name);
// }
const Texture RenderGraph::getTexture(const std::string &name)
{
  return resources.getTexture(name);
}

void CommandRecorder::cmdBeginRenderPass(const RenderPassInfo &info)
{
  recorded.back().commands.push_back(std::make_shared<BeginRenderPassCommand>(info));
}

void CommandRecorder::cmdStartTimer(const Timer timer, PipelineStage stage, uint32_t sampleIndex)
{
  recorded.back().commands.push_back(std::make_shared<StartTimerCommand>(timer, sampleIndex, stage));
}
void CommandRecorder::cmdStopTimer(const Timer timer, PipelineStage stage, uint32_t sampleIndex)
{
  recorded.back().commands.push_back(std::make_shared<StopTimerCommand>(timer, sampleIndex, stage));
}

void CommandRecorder::cmdEndRenderPass()
{
  recorded.back().commands.push_back(std::make_shared<EndRenderPassCommand>());
}

void CommandRecorder::cmdCopyBuffer(BufferView src, BufferView dst)
{
  recorded.back().commands.push_back(std::make_shared<CopyBufferCommand>(src, dst));
}

void CommandRecorder::cmdCopyImage(TextureView src, TextureView dst)
{
  recorded.back().commands.push_back(std::make_shared<CopyImageCommand>(src, dst));
}

void CommandRecorder::cmdBindBindingGroups(BindingGroups groups, uint32_t *dynamicOffsets, uint32_t dynamicOffsetsCount)
{
  std::vector<uint32_t> offsets(dynamicOffsets, dynamicOffsets + dynamicOffsetsCount);
  recorded.back().commands.push_back(std::make_shared<BindBindingGroupsCommand>(groups, offsets));
}

void CommandRecorder::cmdBindGraphicsPipeline(GraphicsPipeline pipeline)
{
  recorded.back().commands.push_back(std::make_shared<BindGraphicsPipelineCommand>(pipeline));
}

void CommandRecorder::cmdBindComputePipeline(ComputePipeline pipeline)
{
  recorded.back().commands.push_back(std::make_shared<BindComputePipelineCommand>(pipeline));
}

void CommandRecorder::cmdBindVertexBuffer(uint32_t slot, BufferView view)
{
  recorded.back().commands.push_back(std::make_shared<BindVertexBufferCommand>(slot, view));
}

void CommandRecorder::cmdBindIndexBuffer(BufferView view, Type type)
{
  recorded.back().commands.push_back(std::make_shared<BindIndexBufferCommand>(view, type));
}

void CommandRecorder::cmdDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
  recorded.back().commands.push_back(std::make_shared<DrawCommand>(vertexCount, instanceCount, firstVertex, firstInstance));
}

void CommandRecorder::cmdDrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, uint32_t vertexOffset, uint32_t firstInstance)
{
  recorded.back().commands.push_back(std::make_shared<DrawIndexedCommand>(indexCount, instanceCount, firstIndex, static_cast<int32_t>(vertexOffset), firstInstance));
}

void CommandRecorder::cmdDrawIndexedIndirect(const BufferView buffer, uint32_t offset, uint32_t drawCount, uint32_t stride)
{
  recorded.back().commands.push_back(std::make_shared<DrawIndexedIndirectCommand>(buffer, offset, drawCount, stride));
}
void CommandRecorder::cmdDrawIndirect(const BufferView buffer, uint32_t offset, uint32_t drawCount, uint32_t stride)
{
  recorded.back().commands.push_back(std::make_shared<DrawIndirectCommand>(buffer, offset, drawCount, stride));
}
void CommandRecorder::cmdDrawIndirectCount(const BufferView indirectBuffer, size_t offset, BufferView countBuffer, size_t countOffset, uint32_t maxDrawCount, uint32_t stride)
{
  recorded.back().commands.push_back(std::make_shared<DrawIndirectCountCommand>(indirectBuffer, offset, countBuffer, countOffset, maxDrawCount, stride));
}
void CommandRecorder::cmdDispatchIndirect(const Buffer indirectBuffer, uint64_t offset)
{
  recorded.back().commands.push_back(std::make_shared<DispatchIndirectCommand>(indirectBuffer, offset));
}

void CommandRecorder::cmdDispatch(uint32_t x, uint32_t y, uint32_t z)
{
  recorded.back().commands.push_back(std::make_shared<DispatchCommand>(x, y, z));
}

void RenderGraph::addSwapChainImages(SwapChain sc)
{
  releaseAllRecordedCommandBuffers();
  uint64_t imagesCount = rhi->getSwapChainImagesCount(sc);

  for (uint64_t index = 0; index < imagesCount; index++)
  {
    TextureInfo info = {
      .name = "_SwapChainImage[" + std::to_string((uint64_t)sc) + "," + std::to_string(index) + "].texture",
      .format = rhi->getSwapChainFormat(sc),
      .depth = 1,
      .mipLevels = 1,
      .usage = ImageUsage::ImageUsage_ColorAttachment,
      .memoryProperties = BufferUsage::BufferUsage_None,
      .height = rhi->getSwapChainImagesHeight(sc),
      .width = rhi->getSwapChainImagesWidth(sc),
    };

    os::Logger::logf("Swap chain image %u resolution = %u %u", index, rhi->getSwapChainImagesWidth(sc), rhi->getSwapChainImagesHeight(sc));

    resources.textureMetadatas[info.name] = TextureResourceMetadata{
      .textureInfo = info,
    };
  }
}
void RenderGraph::removeSwapChainImages(SwapChain sc)
{
  releaseAllRecordedCommandBuffers();
  uint64_t imagesCount = rhi->getSwapChainImagesCount(sc);

  for (uint64_t index = 0; index < imagesCount; index++)
  {
    auto name = "_SwapChainImage[" + std::to_string((uint64_t)sc) + "," + std::to_string(index) + "].texture";
    resources.textureMetadatas.erase(name);
  }
}

const Timer RenderGraph::createTimer(const TimerInfo &info)
{
  return rhi->createTimer(info);
}

void RenderGraph::deleteTimer(const Timer &timer)
{
  rhi->deleteTimer(timer);
}

} // namespace rendering
