#include "RenderGraph.hpp"
#include "os/Logger.hpp"
#include <algorithm>
#include <map>
#include <sstream>
#include <unordered_map>

#include "datastructure/BoundedTaggedRectTreap.hpp"
#include "datastructure/ConcurrentQueue.hpp"
#include "datastructure/TaggedInternvalTree.hpp"
#include "time/TimeSpan.hpp"

#define RENDER_GRAPH_FATAL(...)                                                                                                                                                    \
  do                                                                                                                                                                               \
  {                                                                                                                                                                                \
    os::Logger::errorf(__VA_ARGS__);                                                                                                                                               \
    exit(1);                                                                                                                                                                       \
  } while (0)

namespace rendering
{

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

size_t formatPixelSize(Format fmt)
{
  switch (fmt)
  {
  case Format_R8Unorm:
  case Format_R8Snorm:
  case Format_R8Uint:
  case Format_R8Sint:
    return 1;

  case Format_R16Uint:
  case Format_R16Sint:
  case Format_R16Float:
  case Format_RG8Unorm:
  case Format_RG8Snorm:
  case Format_RG8Uint:
  case Format_RG8Sint:
    return 2;

  case Format_R32Uint:
  case Format_R32Sint:
  case Format_R32Float:
  case Format_RG16Uint:
  case Format_RG16Sint:
  case Format_RG16Float:
  case Format_RGBA8Unorm:
  case Format_RGBA8UnormSrgb:
  case Format_RGBA8Snorm:
  case Format_RGBA8Uint:
  case Format_RGBA8Sint:
  case Format_BGRA8Unorm:
  case Format_BGRA8UnormSrgb:
  case Format_RGB10A2Uint:
  case Format_RGB10A2Unorm:
  case Format_RG11B10UFloat:
  case Format_RGB9E5UFloat:
    return 4;

  case Format_RG32Uint:
  case Format_RG32Sint:
  case Format_RG32Float:
  case Format_RGBA16Uint:
  case Format_RGBA16Sint:
  case Format_RGBA16Float:
    return 8;

  case Format_RGBA32Uint:
  case Format_RGBA32Sint:
  case Format_RGBA32Float:
    return 16;

  case Format_Stencil8:
    return 1;
  case Format_Depth16Unorm:
    return 2;
  case Format_Depth24Plus:
  case Format_Depth24PlusStencil8:
    return 4;
  case Format_Depth32Float:
  case Format_Depth32FloatStencil8:
    return 4;

  default:
    return 0;
  }
}

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

  switch (res.type)
  {
  case ResourceType::ResourceType_BufferView:
  {
    auto id = resources.bufferMetadatas.find(name);

    if (id == resources.bufferMetadatas.end())
    {
      throw std::runtime_error("Buffer not found");
    }

    id->usages.push_back(
        BufferResourceUsage{
          .view = res.bufferView,
          .consumer = taskId,
          .queue = queue,
        });
  }
  break;
  case ResourceType::ResourceType_TextureView:
  {
    auto id = resources.textureMetadatas.find(name);

    if (id == resources.textureMetadatas.end())
    {
      os::print("Texture %s\n", name.c_str());
      throw std::runtime_error("Texture not found");
    }

    id->usages.push_back(
        TextureResourceUsage{
          .view = res.textureView,
          .consumer = taskId,
          .queue = queue,
        });
  }
  break;
  case ResourceType::ResourceType_Sampler:
  {
    auto id = resources.samplerMetadatas.find(name);

    if (id == resources.samplerMetadatas.end())
    {
      throw std::runtime_error("Sampler not found");
    }

    id->usages.push_back(
        SamplerResourceUsage{
          .sampler = res.sampler,
          .consumer = taskId,
          .queue = queue,
        });
  }
  break;
  case ResourceType::ResourceType_BindingsLayout:
  {
    auto id = resources.bindingsLayoutMetadata.find(name);
    if (id == resources.bindingsLayoutMetadata.end())
    {
      throw std::runtime_error("BindingsLayout not found");
    }

    id->usages.push_back(
        BindingsLayoutResourceUsage{
          .consumer = taskId,
          .queue = queue,
        });
  }
  break;
  case ResourceType::ResourceType_BindingGroups:
  {
    auto id = resources.bindingGroupsMetadata.find(name);
    if (id == resources.bindingGroupsMetadata.end())
    {
      throw std::runtime_error("BindingGroups not found");
    }

    id->usages.push_back(
        BindingGroupsResourceUsage{
          .consumer = taskId,
          .queue = queue,
        });
  }
  break;
  case ResourceType::ResourceType_ComputePipeline:
  {
    auto id = resources.computePipelineMetadata.find(name);
    if (id == resources.computePipelineMetadata.end())
    {
      throw std::runtime_error("ComputePipeline not found");
    }

    id->usages.push_back(
        ComputePipelineResourceUsage{
          .consumer = taskId,
          .queue = queue,
        });
  }
  break;
  case ResourceType::ResourceType_GraphicsPipeline:
  {
    auto id = resources.graphicsPipelineMetadata.find(name);
    if (id == resources.graphicsPipelineMetadata.end())
    {
      throw std::runtime_error("GraphicsPipeline not found");
    }

    id->usages.push_back(
        GraphicsPipelineResourceUsage{
          .consumer = taskId,
          .queue = queue,
        });
  }
  break;
  default:
    RENDER_GRAPH_FATAL("Unknown resource type %u", res.type);
    break;
  }
}

RHIResources::RHIResources(RenderGraph *renderGraph) : renderGraph(renderGraph)
{
}

RenderGraph::RenderGraph(RHI *renderingHardwareInterface) : rhi(renderingHardwareInterface), resources(this)
{
  compiled = false;
}

// void RenderGraph::analyseCommands(RHICommandBuffer &recorder)
// {
//   for (auto &commands : recorder.recorded)
//   {
//     std::unordered_map<CommandType, uint32_t> dispatchCount;

//     for (auto &cmd : commands.commands)
//     {

//       dispatchCount[cmd.type] += 1;

//       switch (cmd.type)
//       {
//       case CommandType::CopyBuffer:
//         break;
//       default:
//         if (dispatchCount[cmd.type] > 1)
//         {
//           RENDER_GRAPH_FATAL("Command being called multiple times before dispatch");
//         }
//         break;
//       }

//       if (cmd.type >= Draw && cmd.type <= Dispatch)
//       {
//         dispatchCount.clear();
//       }
//     }
//   }
// }

Queue inferQueue(const std::vector<Command> &commands)
{
  if (commands.size() == 0)
  {
    return Queue::None;
  }
  auto type = commands.back().type;

  for (int i = commands.size() - 1; i >= 0; i--)
  {
    type = commands[i].type;
    if (type != BindBindingGroups && type != StartTimer && type != StopTimer)
    {
      break;
    }
  }
  switch (type)
  {
  case BeginRenderPass:
  case EndRenderPass:
  case BindGraphicsPipeline:
  case BindVertexBuffer:
  case BindIndexBuffer:
  case Draw:
  case DrawIndexed:
  case DrawIndexedIndirect:
    return Queue::Graphics;
  case Dispatch:
  case BindComputePipeline:
    return Queue::Compute;
  case CopyBuffer:
    return Queue::Transfer;

  default:
    RENDER_GRAPH_FATAL("[RenderGraph] Invalid command type %u on sequence of size %u", type, commands.size());
  }

  return Queue::None;
}

static bool isTransferOnlyCommand(CommandType type)
{
  return type == CopyBuffer;
}

static bool isDrawCommand(CommandType type)
{
  return type == Draw || type == DrawIndexed || type == DrawIndexedIndirect;
}

std::vector<RHICommandBuffer::CommandSequence> splitCommands(RHICommandBuffer::CommandSequence &cmds)
{
  // TODO: improve
  std::vector<RHICommandBuffer::CommandSequence> result;
  // Queue lastQueue = Queue::None;
  result.emplace_back();

  for (auto &command : cmds.commands)
  {
    Queue currentQueue = Queue::None;
    result.back().commands.push_back(command);

    switch (command.type)
    {
    case StartTimer:
    case StopTimer:
      break;
    case Draw:
    case DrawIndexed:
    case DrawIndexedIndirect:
    case Dispatch:
    case CopyBuffer:
      result.emplace_back();
      break;
    case BindComputePipeline:
    case BindGraphicsPipeline:
    case BindBindingGroups:
    case BeginRenderPass:
    case EndRenderPass:
    case BindVertexBuffer:
    case BindIndexBuffer:
      break;
    default:
      RENDER_GRAPH_FATAL("[RenderGraph] Invalid command type %u", command.type);
    }

    // if (result.empty())
    // {
    //   lastQueue = currentQueue;
    // }
    // if (currentQueue != lastQueue && currentQueue != Queue::None)
    // {
    //   if (result.back().commands.size() > 0 && result.back().commands.back().type != StartTimer && result.back().commands.back().type != StopTimer)
    //   {
    //     result.emplace_back();
    //   }
    //   lastQueue = currentQueue;
    // }
  }

  for (size_t seqIndex = 0; seqIndex < result.size(); ++seqIndex)
  {
    auto &sequence = result[seqIndex];

    bool hasGraphicsPipeline = false;
    bool hasComputePipeline = false;
    bool hasBindings = false;
    bool hasDraw = false;
    bool hasDispatch = false;
    bool hasOnlyTransfer = true;

    for (auto &cmd : sequence.commands)
    {
      switch (cmd.type)
      {
      case BindGraphicsPipeline:
        hasGraphicsPipeline = true;
        hasOnlyTransfer = false;
        break;

      case BindComputePipeline:
        hasComputePipeline = true;
        hasOnlyTransfer = false;
        break;

      case BindBindingGroups:
        hasBindings = true;
        break;

      case Draw:
      case DrawIndexed:
      case DrawIndexedIndirect:
        hasDraw = true;
        hasOnlyTransfer = false;
        break;

      case Dispatch:
        hasDispatch = true;
        hasOnlyTransfer = false;
        break;

      case CopyBuffer:
        // transfer-only allowed
        break;

      default:
        hasOnlyTransfer = false;
        break;
      }
    }

    if (hasOnlyTransfer)
      continue;

    if (hasDraw)
    {
      if (!hasGraphicsPipeline)
      {
        RENDER_GRAPH_FATAL(
            "[RenderGraph] Invalid graphics submission in CommandSequence %zu\n"
            "  hasPipeline=%d hasBindings=%d hasDraw=%d",
            seqIndex,
            hasGraphicsPipeline,
            hasBindings,
            hasDraw);
      }
    }

    if (hasDispatch)
    {
      if (!hasComputePipeline)
      {
        RENDER_GRAPH_FATAL(
            "[RenderGraph] Invalid compute submission in CommandSequence %zu\n"
            "  hasPipeline=%d hasBindings=%d hasDispatch=%d",
            seqIndex,
            hasComputePipeline,
            hasBindings,
            hasDispatch);
      }
    }
  }

  return result;
}

void RenderGraph::enqueuePass(std::string name, RHICommandBuffer &cmd)
{
  auto pass = RenderGraphPass{
    .name = name,
    .cmd = cmd,
  };

  passes.enqueue(pass);
}

void RenderGraph::analysePasses()
{
  RenderGraphPass pass;

  while (passes.dequeue(pass))
  {
    RHICommandBuffer recorder = pass.cmd;
    // analyseCommands(recorder);

    uint32_t index = 0;
    uint32_t dispatchId = nodes.size();

    for (auto &recordedCommands : pass.cmd.recorded)
    {
      auto split = splitCommands(recordedCommands);
      for (auto commands : split)
      {
        uint32_t id = nodes.size();

        if (commands.commands.size() == 0)
        {
          continue;
        }

        auto node = RenderGraphNode();

        node.name = pass.name + "[" + std::to_string(index++) + "]";
        node.dispatchId = dispatchId;
        node.commandBufferIndex = -1;
        node.id = id;
        node.level = 0;
        node.priority = id;
        node.commands = std::move(commands.commands);
        node.queue = inferQueue(node.commands);

        if (node.queue == Queue::None)
        {
          RENDER_GRAPH_FATAL("[RenderGraph] %s is not submitted to any queue", pass.name.c_str());
        }

        nodes.emplace_back(node);

        auto symbol = resources.bindingGroupsMetadata.end();

        for (const auto &cmd : node.commands)
        {
          switch (cmd.type)
          {
          case BeginRenderPass:
            for (auto &attatchment : cmd.args.renderPassInfo->colorAttachments)
            {
              registerConsumer(
                  attatchment.view.texture.name,
                  InputResource{
                    .type = ResourceType::ResourceType_TextureView,
                    .textureView = attatchment.view,
                    .layout = attatchment.view.layout,
                    .access = attatchment.view.access,
                  },

                  id,
                  node.queue);
            }
            if (cmd.args.renderPassInfo->depthStencilAttachment)
            {
              auto &attatchment = *cmd.args.renderPassInfo->depthStencilAttachment;
              registerConsumer(
                  attatchment.view.texture.name,
                  InputResource{
                    .type = ResourceType::ResourceType_TextureView,
                    .textureView = attatchment.view,
                    .layout = attatchment.view.layout,
                    .access = attatchment.view.access,
                  },
                  id,
                  node.queue);
            }
            break;
          case EndRenderPass:
            break;
          case CopyBuffer:
          {
            auto src = cmd.args.copyBuffer->src;
            auto dst = cmd.args.copyBuffer->dst;
            auto srcMeta = resources.bufferMetadatas.find(cmd.args.copyBuffer->src.buffer.name);
            auto dstMeta = resources.bufferMetadatas.find(cmd.args.copyBuffer->dst.buffer.name);

            if (srcMeta == resources.bufferMetadatas.end())
            {
              RENDER_GRAPH_FATAL("[RHI][CopyBuffer] Source buffer '%s' not found in metadata", src.buffer.name.c_str());
            }

            if (dstMeta == resources.bufferMetadatas.end())
            {
              RENDER_GRAPH_FATAL("[RHI][CopyBuffer] Destination buffer '%s' not found in metadata", dst.buffer.name.c_str());
            }

            const BufferInfo &srcInfo = srcMeta->bufferInfo;
            const BufferInfo &dstInfo = dstMeta->bufferInfo;

            /* ================= BASIC VALIDATION ================= */

            if (src.size == 0)
            {
              RENDER_GRAPH_FATAL("[RHI][CopyBuffer] Copy size is zero (src='%s')", src.buffer.name.c_str());
            }

            if (src.buffer == dst.buffer)
            {
              RENDER_GRAPH_FATAL("[RHI][CopyBuffer] Source and destination buffers are the same ('%s')", src.buffer.name.c_str());
            }

            if (src.offset + src.size > srcInfo.size)
            {
              RENDER_GRAPH_FATAL(
                  "[RHI][CopyBuffer] Source buffer '%s' overflow (offset=%llu size=%llu bufferSize=%llu)", src.buffer.name.c_str(), src.offset, src.size, srcInfo.size);
            }

            if (dst.offset + dst.size > dstInfo.size)
            {
              RENDER_GRAPH_FATAL(
                  "[RHI][CopyBuffer] Destination buffer '%s' overflow (offset=%llu size=%llu bufferSize=%llu)", dst.buffer.name.c_str(), dst.offset, dst.size, dstInfo.size);
            }

            if (src.size != dst.size)
            {
              RENDER_GRAPH_FATAL("[RHI][CopyBuffer] Source and destination copy sizes differ (src=%llu dst=%llu)", src.size, dst.size);
            }

            /* ================= USAGE VALIDATION (WebGPU-style) ================= */

            if (!(srcInfo.usage & BufferUsage_CopySrc))
            {
              RENDER_GRAPH_FATAL("[RHI][CopyBuffer] Source buffer '%s' missing BufferUsage_CopySrc", src.buffer.name.c_str());
            }

            if (!(dstInfo.usage & BufferUsage_CopyDst))
            {
              RENDER_GRAPH_FATAL("[RHI][CopyBuffer] Destination buffer '%s' missing BufferUsage_CopyDst", dst.buffer.name.c_str());
            }

            /* ================= RECORD COMMAND ================= */

            registerConsumer(
                cmd.args.copyBuffer->src.buffer.name,
                InputResource{
                  .type = ResourceType::ResourceType_BufferView,
                  .bufferView = cmd.args.copyBuffer->src,
                  .layout = ResourceLayout::UNDEFINED,
                  .access = cmd.args.copyBuffer->src.access,
                },
                id,
                node.queue);
            registerConsumer(
                cmd.args.copyBuffer->dst.buffer.name,
                InputResource{
                  .type = ResourceType::ResourceType_BufferView,
                  .bufferView = cmd.args.copyBuffer->dst,
                  .layout = ResourceLayout::UNDEFINED,
                  .access = cmd.args.copyBuffer->dst.access,
                },
                id,
                node.queue);
          }
          break;
          case BindBindingGroups:
            symbol = resources.bindingGroupsMetadata.find(cmd.args.bindGroups->groups.name);
            if (symbol == resources.bindingGroupsMetadata.end())
            {
              throw std::runtime_error("Bunding Groups not found");
            }
            registerConsumer(
                symbol->groupsInfo.layout.name,
                InputResource{
                  .type = ResourceType::ResourceType_BindingsLayout,
                  .layout = ResourceLayout::UNDEFINED,
                  .access = AccessPattern::NONE,
                },
                id,
                node.queue);
            registerConsumer(
                cmd.args.bindGroups->groups.name,
                InputResource{
                  .type = ResourceType::ResourceType_BindingGroups,
                  .bindingGroups = cmd.args.bindGroups->groups,
                  .layout = ResourceLayout::UNDEFINED,
                  .access = AccessPattern::NONE,
                },
                id,
                node.queue);

            for (auto &group : symbol->groupsInfo.groups)
            {
              for (auto &buffer : group.buffers)
              {
                registerConsumer(
                    buffer.bufferView.buffer.name,
                    InputResource{
                      .type = ResourceType::ResourceType_BufferView,
                      .bufferView = buffer.bufferView,
                      .layout = ResourceLayout::UNDEFINED,
                      .access = buffer.bufferView.access,
                    },
                    id,
                    node.queue);
              }

              for (auto &texture : group.textures)
              {
                registerConsumer(
                    texture.textureView.texture.name,
                    InputResource{
                      .type = ResourceType::ResourceType_TextureView,
                      .textureView = texture.textureView,
                      .layout = texture.textureView.layout,
                      .access = texture.textureView.access,
                    },
                    id,
                    node.queue);
              }

              for (auto &texture : group.storageTextures)
              {
                registerConsumer(
                    texture.textureView.texture.name,
                    InputResource{
                      .type = ResourceType::ResourceType_TextureView,
                      .textureView = texture.textureView,
                      .layout = texture.textureView.layout,
                      .access = texture.textureView.access,
                    },
                    id,
                    node.queue);
              }

              for (auto &texture : group.samplers)
              {
                registerConsumer(
                    texture.view.texture.name,
                    InputResource{
                      .type = ResourceType::ResourceType_TextureView,
                      .textureView = texture.view,
                      .layout = texture.view.layout,
                      .access = texture.view.access,
                    },
                    id,
                    node.queue);
                registerConsumer(
                    texture.sampler.name,
                    InputResource{
                      .type = ResourceType::ResourceType_Sampler,
                      .sampler = texture.sampler,
                      .layout = ResourceLayout::UNDEFINED,
                      .access = AccessPattern::NONE,
                    },
                    id,
                    node.queue);
              }
            }
            break;
          case BindVertexBuffer:
            registerConsumer(
                cmd.args.bindVertexBuffer->buffer.buffer.name,
                InputResource{
                  .type = ResourceType::ResourceType_BufferView,
                  .bufferView = cmd.args.bindVertexBuffer->buffer,
                  .layout = ResourceLayout::UNDEFINED,
                  .access = cmd.args.bindVertexBuffer->buffer.access,
                },
                id,
                node.queue);
            break;
          case BindIndexBuffer:
            registerConsumer(
                cmd.args.bindIndexBuffer->buffer.buffer.name,
                InputResource{
                  .type = ResourceType::ResourceType_BufferView,
                  .bufferView = cmd.args.bindIndexBuffer->buffer,
                  .layout = ResourceLayout::UNDEFINED,
                  .access = cmd.args.bindIndexBuffer->buffer.access,
                },
                id,
                node.queue);
            break;
          case BindComputePipeline:
            registerConsumer(
                cmd.args.computePipeline->name,
                InputResource{
                  .type = ResourceType::ResourceType_ComputePipeline,
                  .computePipeline = *cmd.args.computePipeline,
                  .layout = ResourceLayout::UNDEFINED,
                  .access = AccessPattern::NONE,
                },
                id,
                node.queue);
            break;
          case BindGraphicsPipeline:
            registerConsumer(
                cmd.args.graphicsPipeline->name,
                InputResource{
                  .type = ResourceType::ResourceType_GraphicsPipeline,
                  .graphicsPipeline = *cmd.args.graphicsPipeline,
                  .layout = ResourceLayout::UNDEFINED,
                  .access = AccessPattern::NONE,
                },
                id,
                node.queue);
            break;
          case DrawIndexedIndirect:
            registerConsumer(
                cmd.args.drawIndexedIndirect->buffer.buffer.name,
                InputResource{
                  .type = ResourceType::ResourceType_BufferView,
                  .bufferView = cmd.args.drawIndexedIndirect->buffer,
                  .layout = ResourceLayout::UNDEFINED,
                  .access = cmd.args.drawIndexedIndirect->buffer.access,
                },
                id,
                node.queue);
            break;
          case Draw:
          case DrawIndexed:
          case Dispatch:
          case StartTimer:
          case StopTimer:
            break;
          default:
            RENDER_GRAPH_FATAL("Unsuported command");
            break;
          }
        }
        // taskData[id].registry.sortResources();
      }
    }
  }
}

uint32_t RenderGraph::levelDFS(uint32_t id, std::vector<bool> &visited, uint32_t level)
{
  uint32_t max = level;

  if (nodes[id].level < level)
  {
    nodes[id].level = level;
    for (auto &edge : edges[id])
    {
      uint32_t maxChilds = levelDFS(edge.taskId, visited, level + 1);

      if (maxChilds > max)
      {
        max = maxChilds;
      }
    }
  }

  return max;
}

void RenderGraph::topologicalSortDFS(uint32_t id, std::vector<bool> &visited, std::stack<uint32_t> &topologicalSort, std::vector<bool> &isParent)
{
  if (isParent[id])
  {
    RENDER_GRAPH_FATAL("Cyclical dependency in Task Graph");
  }

  if (visited[id])
  {
    return;
  }

  visited[id] = true;
  isParent[id] = true;

  for (auto &edge : edges[id])
  {
    topologicalSortDFS(edge.taskId, visited, topologicalSort, isParent);
  }

  isParent[id] = false;
  topologicalSort.push(id);
}

void RenderGraph::tasksTopologicalSort(std::vector<uint32_t> &order)
{
  std::vector<bool> visited(nodes.size(), false);
  std::vector<bool> recStack(nodes.size(), false);

  std::stack<uint32_t> topologicalSort;

  for (uint32_t taskId = 0; taskId < nodes.size(); taskId++)
  {
    if (visited[taskId])
    {
      continue;
    }

    topologicalSortDFS(taskId, visited, topologicalSort, recStack);
  }

  while (topologicalSort.size())
  {
    order.push_back(topologicalSort.top());
    topologicalSort.pop();
  }
}

void RenderGraph::analyseTaskLevels()
{
  std::vector<uint32_t> topologicalOrder;

  tasksTopologicalSort(topologicalOrder);

  for (auto &task : nodes)

    for (uint32_t id : topologicalOrder)
    {
      uint32_t currentLevel = nodes[id].level;
      for (auto &edge : edges[id])
      {
        uint64_t increment = 1; // edge.type == EdgeType::ResourceShare ? 0 : 1;
        nodes[edge.taskId].level = std::max(nodes[edge.taskId].level, currentLevel + increment);
      }
    }

  for (const auto &node : nodes)
  {
    os::Logger::logf("[RenderGraph] %s dispatched at level %u", node.name.c_str(), node.level);

    for (auto &cmd : node.commands)
    {
      switch (cmd.type)
      {
      case CopyBuffer:
      {
        auto srcMeta = resources.bufferMetadatas.find(cmd.args.copyBuffer->src.buffer.name);
        srcMeta->firstUsedAt = std::min(srcMeta->firstUsedAt, node.level);
        srcMeta->lastUsedAt = std::max(srcMeta->lastUsedAt, node.level);
        auto dstMeta = resources.bufferMetadatas.find(cmd.args.copyBuffer->dst.buffer.name);
        dstMeta->firstUsedAt = std::min(dstMeta->firstUsedAt, node.level);
        dstMeta->lastUsedAt = std::max(dstMeta->lastUsedAt, node.level);
        break;
      }

      case BindBindingGroups:
      {
        const auto &groupsMeta = resources.bindingGroupsMetadata[cmd.args.bindGroups->groups.name]->groupsInfo.groups;
        for (const auto &group : groupsMeta)
        {
          for (const auto &buffer : group.buffers)
          {
            auto meta = resources.bufferMetadatas.find(buffer.bufferView.buffer.name);
            meta->firstUsedAt = std::min(meta->firstUsedAt, node.level);
            meta->lastUsedAt = std::max(meta->lastUsedAt, node.level);
          }
        }
        break;
      }

      case BindVertexBuffer:
      {
        auto meta = resources.bufferMetadatas.find(cmd.args.bindVertexBuffer->buffer.buffer.name);
        meta->firstUsedAt = std::min(meta->firstUsedAt, node.level);
        meta->lastUsedAt = std::max(meta->lastUsedAt, node.level);
        break;
      }

      case BindIndexBuffer:
      {
        auto meta = resources.bufferMetadatas.find(cmd.args.bindIndexBuffer->buffer.buffer.name);
        meta->firstUsedAt = std::min(meta->firstUsedAt, node.level);
        meta->lastUsedAt = std::max(meta->lastUsedAt, node.level);
        break;
      }

      case DrawIndexedIndirect:
      {
        auto meta = resources.bufferMetadatas.find(cmd.args.drawIndexedIndirect->buffer.buffer.name);
        meta->firstUsedAt = std::min(meta->firstUsedAt, node.level);
        meta->lastUsedAt = std::max(meta->lastUsedAt, node.level);
        break;
      }

      case BeginRenderPass:
      case EndRenderPass:
      case BindComputePipeline:
      case BindGraphicsPipeline:
      case Draw:
      case DrawIndexed:
      case Dispatch:
      case StartTimer:
      case StopTimer:
        break;

      default:
        RENDER_GRAPH_FATAL("Unsupported command");
      }
    }
  }
}

bool intervalsOverlap(float aOffset, float aSize, float bOffset, float bSize)
{
  float aEnd = aOffset + aSize;
  float bEnd = bOffset + bSize;
  return (aOffset < bEnd) && (bOffset < aEnd);
}
struct AccessConsumerPair
{
  AccessPattern access;
  uint64_t consumer;
  Queue queue;
  bool operator==(const AccessConsumerPair &o) const
  {
    return access == o.access && consumer == o.consumer && o.queue == queue;
  }
  bool operator!=(const AccessConsumerPair &o) const
  {
    return access != o.access || consumer != o.consumer || o.queue != queue;
  }
};

struct AccessLayoutConsumerTriple
{
  AccessPattern access;
  ResourceLayout layout;
  uint64_t consumer;
  Queue queue;

  bool operator==(const AccessLayoutConsumerTriple &o) const
  {
    return access == o.access && layout == o.layout && queue == o.queue;
  }

  bool operator!=(const AccessLayoutConsumerTriple &o) const
  {
    return access != o.access || layout != o.layout || queue != o.queue;
  }
};

struct AccessConsumerTupple
{
  AccessPattern access;
  uint64_t consumer;

  Queue queue;

  bool operator==(const AccessConsumerTupple &o) const
  {
    return access == o.access && queue == o.queue;
  }

  bool operator!=(const AccessConsumerTupple &o) const
  {
    return access != o.access || queue != o.queue;
  }
};

void RenderGraph::analyseDependencyGraph()
{
  edges.clear();
  edges.resize(nodes.size());

  for (auto [name, meta] : resources.bufferMetadatas)
  {
    std::sort(
        meta.usages.begin(),
        meta.usages.end(),
        [this](BufferResourceUsage taskA, BufferResourceUsage taskB)
        {
          return nodes[taskA.consumer].priority < nodes[taskB.consumer].priority;
        });

    std::vector<lib::BoundedTaggedIntervalTree<AccessConsumerPair, uint64_t>::Interval> intervals;

    intervals.reserve(4 * meta.usages.size());

    lib::BoundedTaggedIntervalTree<AccessConsumerPair, uint64_t> bufferIntevals(meta.usages.size() * 4);

    bufferIntevals.insert(
        0,
        meta.bufferInfo.size - 1,
        AccessConsumerPair{
          .access = AccessPattern::NONE,
          .consumer = (uint64_t)-1,
          .queue = Queue::None,
        });

    for (const auto &usage : meta.usages)
    {
      intervals.clear();

      bufferIntevals.queryAll(usage.view.offset, usage.view.offset + usage.view.size - 1, intervals);

      for (const auto &interval : intervals)
      {
        nodes[usage.consumer].bufferTransitions.emplace_back(
            BufferBarrier{
              .resourceId = meta.bufferInfo.name,
              .fromAccess = interval.tag.access,
              .toAccess = usage.view.access,
              .offset = interval.start,
              .size = interval.end - interval.start + 1,
              .toLevel = nodes[usage.consumer].level,
              .fromQueue = interval.tag.queue,
              .toQueue = nodes[usage.consumer].queue,
              .fromNode = interval.tag.consumer,
            });
        if (interval.tag.consumer == usage.consumer)
        {

          continue;
        }

        if (interval.tag.consumer != -1)
        {
          edges[interval.tag.consumer].emplace_back(
              RenderGraphEdge{
                .type = (interval.tag.access != usage.view.access || interval.tag.queue != usage.queue) ? EdgeType::ResourceDependency : EdgeType::ResourceShare,
                .taskId = usage.consumer,
                .resourceId = meta.bufferInfo.name,
                .resourceType = ResourceType::ResourceType_BufferView,
              });
        }

        bufferIntevals.remove(interval.start, interval.end, interval.tag);
        bufferIntevals.insert(
            interval.start,
            interval.end,
            AccessConsumerPair{
              .access = usage.view.access,
              .consumer = usage.consumer,
              .queue = usage.queue,
            });
      }
    }
  }

  for (auto [name, meta] : resources.textureMetadatas)
  {
    std::sort(
        meta.usages.begin(),
        meta.usages.end(),
        [this](TextureResourceUsage taskA, TextureResourceUsage taskB)
        {
          return nodes[taskA.consumer].priority < nodes[taskB.consumer].priority;
        });

    std::vector<lib::BoundedTaggedRectTreap<AccessLayoutConsumerTriple, uint64_t>::Rect> intervals;
    intervals.reserve(resources.textureMetadatas.size() * 4);
    lib::BoundedTaggedRectTreap<AccessLayoutConsumerTriple, uint64_t> textureState(meta.usages.size() * 4);
    textureState.insert(
        0,
        0,
        meta.textureInfo.mipLevels,
        meta.textureInfo.depth,
        AccessLayoutConsumerTriple{
          .access = AccessPattern::NONE,
          .layout = ResourceLayout::UNDEFINED,
          .consumer = (uint64_t)-1,
          .queue = Queue::None,
        });

    for (const auto &usage : meta.usages)
    {
      intervals.clear();

      textureState.queryAll(
          usage.view.baseMipLevel,
          usage.view.baseArrayLayer,
          usage.view.baseMipLevel + usage.view.levelCount - 1,
          usage.view.baseArrayLayer + usage.view.layerCount - 1,
          intervals);

      auto currentTag = AccessLayoutConsumerTriple{
        .access = usage.view.access,
        .layout = usage.view.layout,
        .consumer = usage.consumer,
        .queue = nodes[usage.consumer].queue,
      };

      for (const auto &interval : intervals)
      {
        nodes[usage.consumer].textureTransitions.emplace_back(
            TextureBarrier{
              .resourceId = meta.textureInfo.name,
              .fromAccess = interval.tag.access,
              .toAccess = usage.view.access,
              .fromLayout = interval.tag.layout,
              .toLayout = usage.view.layout,
              .baseMip = interval.x1,
              .mipCount = interval.x2 - interval.x1 + 1,
              .baseLayer = interval.y1,
              .layerCount = interval.y2 - interval.y1 + 1,
              .toLevel = nodes[usage.consumer].level,
              .fromQueue = interval.tag.queue,
              .toQueue = nodes[usage.consumer].queue,
              .fromNode = interval.tag.consumer,
            });

        if (interval.tag.consumer == usage.consumer)
        {
          continue;
        }

        if (interval.tag.consumer != -1)
        {
          edges[interval.tag.consumer].emplace_back(
              RenderGraphEdge{
                .type = (interval.tag.access != currentTag.access || interval.tag.layout != currentTag.layout || interval.tag.queue != currentTag.queue)
                            ? EdgeType::ResourceDependency
                            : EdgeType::ResourceShare,
                .taskId = usage.consumer,
                .resourceId = meta.textureInfo.name,
                .resourceType = ResourceType::ResourceType_TextureView,
              });
        }

        textureState.remove(interval.x1, interval.y1, interval.x2, interval.y2, interval.tag);
        textureState.insert(interval.x1, interval.y1, interval.x2, interval.y2, currentTag);
      }
    }
  }
}

struct Request
{
  std::string id;
  uint64_t size;
  uint64_t start;
  uint64_t end;
};

inline size_t alignUp(size_t value, size_t alignment)
{
  return (value + alignment - 1) & ~(alignment - 1);
}

std::pair<std::map<std::string, BufferSlice>, size_t> allocateBuffersGraphColoring(std::vector<Request> &requests, size_t alignment)
{
  std::sort(
      requests.begin(),
      requests.end(),
      [](const Request &a, const Request &b)
      {
        return a.start < b.start;
      });

  struct ColorSlot
  {
    uint64_t offsetBase;
    uint64_t currentSize;
    int lastEnd;
  };

  std::vector<ColorSlot> colors;
  std::map<std::string, BufferSlice> allocations;

  for (size_t i = 0; i < requests.size(); ++i)
  {
    const Request &req = requests[i];

    int chosenColor = -1;

    for (size_t c = 0; c < colors.size(); ++c)
    {
      if (req.start > colors[c].lastEnd)
      {
        chosenColor = static_cast<int>(c);
        break;
      }
    }

    if (chosenColor == -1)
    {
      size_t offsetBase = colors.empty() ? 0 : alignUp(colors.back().offsetBase + colors.back().currentSize, alignment);
      colors.push_back(ColorSlot{offsetBase, 0, -1});
      chosenColor = static_cast<int>(colors.size() - 1);
    }

    ColorSlot &slot = colors[chosenColor];
    slot.currentSize = std::max(slot.currentSize, req.size);

    slot.lastEnd = req.end;

    allocations[req.id] = BufferSlice{
      requests[i].id,
      slot.offsetBase,
      req.size,
    };
  }

  size_t totalSize = 0;
  for (size_t i = 0; i < colors.size(); ++i)
  {
    totalSize = alignUp(totalSize, alignment);
    totalSize += colors[i].currentSize;
  }

  return std::make_pair(allocations, totalSize);
}

void RenderGraph::analyseAllocations()
{
  resources.scratchBuffers.clear();

  std::unordered_map<BufferUsage, std::vector<Request>> memoryRequests;

  for (auto [name, meta] : resources.bufferMetadatas)
  {
    if (meta.bufferInfo.scratch && meta.usages.size())
    {
      const BufferInfo &info = meta.bufferInfo;

      memoryRequests[info.usage].push_back(
          Request{
            .id = meta.bufferInfo.name,
            .start = meta.firstUsedAt,
            .end = meta.lastUsedAt,
            .size = info.size,
          });
    }
  }

  resources.scratchMap.clear();

  for (auto &[usage, requests] : memoryRequests)
  {
    auto [allocations, totalSize] = allocateBuffersGraphColoring(requests, 16);
    uint32_t bufferAllocationId = resources.bufferMetadatas.size();

    BufferInfo info;
    info.name = bufferUsageToString(usage) + ".buffer";
    info.size = totalSize;
    info.usage = usage;

    auto metadata = BufferResourceMetadata{
      .bufferInfo = info,
    };

    resources.scratchBuffers.insert(usage, metadata);

    os::Logger::logf("[RenderGraph] Reserving %u bytes for %s", info.size, info.name.c_str());

    for (auto &allocation : allocations)
    {
      auto scratch = resources.scratchMap[allocation.second.bufferId];

      scratch->usage = usage;
      scratch->offset = allocation.second.offset;
      scratch->size = allocation.second.size;

      os::Logger::logf(
          "[RenderGraph] Reserving slice of %s, offset = %u, size = %u, for %s",
          info.name.c_str(),
          allocation.second.offset,
          allocation.second.size,
          resources.bufferMetadatas[allocation.second.bufferId]->bufferInfo.name.c_str());
    }
  }
}

struct Interval
{
  uint32_t start;
  uint32_t end;
};

struct Result
{
  std::vector<Interval> remainders;
  Interval overlap;
  bool hasOverlap;
};

enum class RemainderMode
{
  FirstOnly,
  SecondOnly,
  Both
};

void RenderGraph::analyseBufferStateTransition()
{
  std::vector<lib::BoundedTaggedIntervalTree<AccessConsumerTupple, uint64_t>::Interval> intervals;

  uint64_t size = 0;

  for (auto [name, meta] : resources.bufferMetadatas)
  {
    size += meta.usages.size();
  }

  intervals.reserve(4 * size);
  os::print(">>>> Usage %u\n", resources.bufferMetadatas.size());
  for (auto [name, meta] : resources.bufferMetadatas)
  {
    os::print(">>>> Usage\n");

    lib::BoundedTaggedIntervalTree<AccessConsumerTupple, uint64_t> bufferIntevals(meta.usages.size() * 4);
    if (meta.bufferInfo.size == 0)
    {
      continue;
    }

    bufferIntevals.insert(
        0,
        meta.bufferInfo.size - 1,
        AccessConsumerTupple{
          .access = AccessPattern::NONE,
          .consumer = (uint64_t)-1,
          .queue = Queue::None,
        });

    std::sort(
        meta.usages.begin(),
        meta.usages.end(),
        [this](const BufferResourceUsage &usageA, const BufferResourceUsage &usageB)
        {
          return nodes[usageA.consumer].level < nodes[usageB.consumer].level;
        });

    for (const auto &usage : meta.usages)
    {
      intervals.clear();

      auto curr = AccessConsumerTupple{
        .access = usage.view.access,
        .consumer = usage.consumer,
        .queue = nodes[usage.consumer].queue,
      };

      bufferIntevals.query(usage.view.offset, usage.view.offset + usage.view.size - 1, curr, intervals);

      for (const auto &interval : intervals)
      {
        if (interval.tag.access != usage.view.access)
        {
          // printf("interval %u %u\n", interval.start, interval.end);

          bufferIntevals.remove(interval.start, interval.end, interval.tag);
          bufferIntevals.insert(
              interval.start,
              interval.end,
              AccessConsumerTupple{
                .access = usage.view.access,
                .consumer = usage.consumer,
                .queue = nodes[usage.consumer].queue,
              });

          nodes[usage.consumer].bufferTransitions.emplace_back(
              BufferBarrier{
                .resourceId = meta.bufferInfo.name,
                .fromAccess = interval.tag.access,
                .toAccess = usage.view.access,
                .offset = interval.start,
                .size = interval.end - interval.start + 1,
                .toLevel = nodes[usage.consumer].level,
                .fromQueue = interval.tag.queue,
                .toQueue = nodes[usage.consumer].queue,
                .fromNode = interval.tag.consumer,
              });
        }
      }

      // printf("end\n");
      // bufferIntevals.print();
    }
  }

  // for (auto &t : bufferTransitions)
  // {
  //   os::Logger::logf(
  //       "[RenderGraph] Buffer Barrier - %s[%u ... %u]: %s -> %s at dispatch %u",
  //       resources.bufferMetadatas[t.resourceId].bufferInfo.name.c_str(),
  //       t.offset,
  //       t.offset + t.size - 1,
  //       toString(t.fromAccess).c_str(),
  //       toString(t.toAccess).c_str(),
  //       t.toLevel);
  // }
}

void RenderGraph::analyseTextureStateTransition()
{
  std::vector<lib::BoundedTaggedRectTreap<AccessLayoutConsumerTriple, uint64_t>::Rect> intervals;

  uint64_t size = 0;

  for (auto [name, meta] : resources.textureMetadatas)
  {
    size += meta.usages.size();
  }

  intervals.reserve(4 * size);

  for (auto [name, meta] : resources.textureMetadatas)
  {
    lib::BoundedTaggedRectTreap<AccessLayoutConsumerTriple, uint64_t> textureState(meta.usages.size() * 4);

    textureState.insert(
        0,
        0,
        meta.textureInfo.mipLevels,
        meta.textureInfo.depth,
        AccessLayoutConsumerTriple{
          .access = AccessPattern::NONE,
          .layout = ResourceLayout::UNDEFINED,
          .queue = Queue::None,
        });

    // textureState.print();

    std::sort(
        meta.usages.begin(),
        meta.usages.end(),
        [this](const TextureResourceUsage &usageA, const TextureResourceUsage &usageB)
        {
          return nodes[usageA.consumer].level < nodes[usageB.consumer].level;
        });

    for (const auto &usage : meta.usages)
    {
      intervals.clear();

      textureState.query(
          usage.view.baseMipLevel,
          usage.view.baseArrayLayer,
          usage.view.baseMipLevel + usage.view.levelCount - 1,
          usage.view.baseArrayLayer + usage.view.layerCount - 1,
          intervals);

      auto currentTag = AccessLayoutConsumerTriple{
        .access = usage.view.access,
        .layout = usage.view.layout,
        .queue = nodes[usage.consumer].queue,
      };

      for (const auto &interval : intervals)
      {
        if (interval.tag != currentTag)
        {
          textureState.remove(interval.x1, interval.y1, interval.x2, interval.y2, interval.tag);
          textureState.insert(interval.x1, interval.y1, interval.x2, interval.y2, currentTag);

          nodes[usage.consumer].textureTransitions.emplace_back(
              TextureBarrier{
                .resourceId = meta.textureInfo.name,
                .fromAccess = interval.tag.access,
                .toAccess = usage.view.access,
                .fromLayout = interval.tag.layout,
                .toLayout = usage.view.layout,
                .baseMip = interval.x1,
                .mipCount = interval.x2 - interval.x1,
                .baseLayer = interval.y1,
                .layerCount = interval.y2 - interval.y1,
                .toLevel = nodes[usage.consumer].level,
                .fromQueue = interval.tag.queue,
                .toQueue = nodes[usage.consumer].queue,
                .fromNode = interval.tag.consumer,
              });
        }
      }
      // printf("after\n");
      // textureIntervals.print();
    }
  }

  // for (auto &t : textureTransitions)
  // {
  //   os::Logger::logf(
  //       "[RenderGraph] Texture Barrier - %s, mip:[%u ... %u], layer:[%u ... %u] at dispatch %u, %u->%u, %u->%u",
  //       resources.textureMetadatas[t.resourceId].textureInfo.name.c_str(),
  //       t.baseMip,
  //       t.baseMip + t.mipCount - 1,
  //       t.baseLayer,
  //       t.baseLayer + t.layerCount - 1,
  //       t.toLevel,
  //       t.fromAccess,
  //       t.toAccess,
  //       t.fromLayout,
  //       t.toLayout);
  // }
}

void RenderGraph::analyseStateTransition()
{
  analyseBufferStateTransition();
  analyseTextureStateTransition();
}

struct SemaphoreHash
{
  std::size_t operator()(const Semaphore &s) const noexcept
  {
    std::size_t h = 0;
    auto hc = [](auto seed, auto v)
    {
      seed ^= std::hash<decltype(v)>{}(v) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
      return seed;
    };
    h = hc(h, static_cast<int>(s.signalQueue));
    h = hc(h, static_cast<int>(s.waitQueue));
    h = hc(h, s.signalTask);
    h = hc(h, s.waitTask);
    return h;
  }
};

struct SemaphoreEq
{
  bool operator()(const Semaphore &a, const Semaphore &b) const noexcept
  {
    return a.signalQueue == b.signalQueue && a.waitQueue == b.waitQueue && a.signalTask == b.signalTask && a.waitTask == b.waitTask;
  }
};

void RenderGraph::analyseSemaphores()
{
  uint32_t fromTask = 0;
  std::unordered_set<Semaphore, SemaphoreHash, SemaphoreEq> semaphoresSet;

  for (auto &taskEdges : edges)
  {
    for (auto &edge : taskEdges)
    {
      uint32_t toTask = edge.taskId;

      semaphoresSet.insert(
          Semaphore{
            .signalQueue = nodes[fromTask].queue,
            .waitQueue = nodes[toTask].queue,
            .signalTask = fromTask,
            .waitTask = toTask,
          });
    }

    fromTask += 1;
  }

  uint32_t at = 0;

  for (const auto &semaphore : semaphoresSet)
  {
    semaphores.push_back(semaphore);
    nodes[semaphore.signalTask].signalSemaphores.push_back(at);
    nodes[semaphore.waitTask].waitSemaphores.push_back(at);
    at++;
  }
}
auto logQueue = [](Queue q)
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
  return "";
};

void RenderGraph::analyseCommandBuffers()
{
  for (auto &count : commandBuffersCount)
  {
    count = 0;
  }

  for (auto &currentNode : nodes)
  {
    bool canReuseCommandBuffer = true;

    for (uint64_t wait : currentNode.waitSemaphores)
    {
      uint64_t from = semaphores[wait].signalTask;
      if (nodes[from].queue != currentNode.queue || nodes[from].dispatchId != currentNode.dispatchId)
      {
        canReuseCommandBuffer = false;
        break;
      }
    }
#define RANDER_GRAPH_COMMAND_BUFFER_PER_NODE
#ifdef RANDER_GRAPH_COMMAND_BUFFER_PER_NODE
    if (commandBuffersCount[currentNode.queue] == 0)
    {
      commandBuffersCount[currentNode.queue] += 1;
    }
    else if (!canReuseCommandBuffer)
    {
      commandBuffersCount[currentNode.queue] += 1;
    }

    currentNode.commandBufferIndex = commandBuffersCount[currentNode.queue] - 1;
#else
    commandBuffersCount[currentNode.queue] += 1;
    currentNode.commandBufferIndex = commandBuffersCount[currentNode.queue] - 1;
#endif
  }
}

void RenderGraph::compile()
{
  nodes.clear();
  edges.clear();

  for (const auto &[name, meta] : resources.bufferMetadatas)
  {
    meta.usages.clear();
  }
  for (const auto &[name, meta] : resources.textureMetadatas)
  {
    meta.usages.clear();
  }
  for (const auto &[name, meta] : resources.samplerMetadatas)
  {
    meta.usages.clear();
  }
  for (const auto &[name, meta] : resources.bindingsLayoutMetadata)
  {
    meta.usages.clear();
  }
  for (const auto &[name, meta] : resources.bindingGroupsMetadata)
  {
    meta.usages.clear();
  }
  for (const auto &[name, meta] : resources.graphicsPipelineMetadata)
  {
    meta.usages.clear();
  }
  for (const auto &[name, meta] : resources.computePipelineMetadata)
  {
    meta.usages.clear();
  }

  for (const auto &[name, meta] : resources.scratchBuffers)
  {
    meta.usages.clear();
  }

  lib::time::TimeSpan analysePassesStart = lib::time::TimeSpan::now();
  analysePasses();
  lib::time::TimeSpan analysePassesEnd = lib::time::TimeSpan::now();

  lib::time::TimeSpan analyseDependencyGraphStart = lib::time::TimeSpan::now();
  analyseDependencyGraph();
  lib::time::TimeSpan analyseDependencyGraphEnd = lib::time::TimeSpan::now();

  lib::time::TimeSpan analyseTaskLevelsStart = lib::time::TimeSpan::now();
  analyseTaskLevels();
  lib::time::TimeSpan analyseTaskLevelsEnd = lib::time::TimeSpan::now();

  lib::time::TimeSpan analyseAllocationsStart = lib::time::TimeSpan::now();
  analyseAllocations();
  lib::time::TimeSpan analyseAllocationsEnd = lib::time::TimeSpan::now();

  lib::time::TimeSpan analyseSemaphoresStart = lib::time::TimeSpan::now();
  analyseSemaphores();
  lib::time::TimeSpan analyseSemaphoresEnd = lib::time::TimeSpan::now();

  lib::time::TimeSpan analyseCommandBuffersStart = lib::time::TimeSpan::now();
  analyseCommandBuffers();
  lib::time::TimeSpan analyseCommandBuffersEnd = lib::time::TimeSpan::now();

  // lib::time::TimeSpan outputCommandsStart = lib::time::TimeSpan::now();
  // outputCommands(program);
  // lib::time::TimeSpan outputCommandsEnd = lib::time::TimeSpan::now();
  os::Logger::logf("[RenderGraph] analysePasses time = %fms", (analysePassesEnd - analysePassesStart).milliseconds());
  os::Logger::logf("[RenderGraph] analyseDependencyGraph time = %fms", (analyseDependencyGraphEnd - analyseDependencyGraphStart).milliseconds());
  // os::Logger::logf("[RenderGraph] analyseStateTransition time = %fms", (analyseStateTransitionEnd - analyseStateTransitionStart).milliseconds());

  os::Logger::logf("[RenderGraph] analyseTaskLevels time = %fms", (analyseTaskLevelsEnd - analyseTaskLevelsStart).milliseconds());
  os::Logger::logf("[RenderGraph] analyseAllocations time = %fms", (analyseAllocationsEnd - analyseAllocationsStart).milliseconds());
  os::Logger::logf("[RenderGraph] analyseSemaphores time = %fms", (analyseSemaphoresEnd - analyseSemaphoresStart).milliseconds());
  os::Logger::logf("[RenderGraph] analyseCommandBuffers time = %fms", (analyseCommandBuffersEnd - analyseCommandBuffersStart).milliseconds());

  // os::Logger::logf("[TimeScheduler] outputCommands time = %fns", (outputCommandsEnd - outputCommandsStart).nanoseconds());

  for (const auto &[name, meta] : resources.bufferMetadatas)
  {
    if (meta.usages.empty())
    {
      os::Logger::warningf("Buffer %s not used in current graph", name.c_str());
    }
  }

  for (const auto &[name, meta] : resources.textureMetadatas)
  {
    if (meta.usages.empty())
    {
      os::Logger::warningf("Buffer %s not used in current graph", name.c_str());
    }
  }
  for (const auto &[name, meta] : resources.samplerMetadatas)
  {
    if (meta.usages.empty())
    {
      os::Logger::warningf("Sampler %s not used in current graph", name.c_str());
    }
  }
  for (const auto &[name, meta] : resources.bindingsLayoutMetadata)
  {
    if (meta.usages.empty())
    {
      os::Logger::warningf("Binding Layout %s not used in current graph", name.c_str());
    }
  }

  for (const auto &[name, meta] : resources.bindingGroupsMetadata)
  {
    if (meta.usages.empty())
    {
      os::Logger::warningf("Binding Groups %s not used in current graph", name.c_str());
    }
  }

  for (const auto &[name, meta] : resources.graphicsPipelineMetadata)
  {
    if (meta.usages.empty())
    {
      os::Logger::warningf("Graphics Pipeline %s not used in current graph", name.c_str());
    }
  }
  for (const auto &[name, meta] : resources.computePipelineMetadata)
  {
    if (meta.usages.empty())
    {
      os::Logger::warningf("Compute Pipeline %s not used in current graph", name.c_str());
    }
  }

  compiled = true;
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

void RenderGraph::run(Frame &frame)
{
  auto runStart = lib::time::TimeSpan::now();

  os::Logger::logf("[RenderGraph] ===== Begin run =====");
  os::Logger::logf("[RenderGraph] Node count = %zu", nodes.size());
  uint64_t maxLevel = 0;

  std::sort(
      nodes.begin(),
      nodes.end(),
      [&maxLevel](const RenderGraphNode &a, const RenderGraphNode &b)
      {
        maxLevel = std::max(maxLevel, a.level);
        maxLevel = std::max(maxLevel, b.level);
        return a.level < b.level;
      });

  os::Logger::logf("[RenderGraph] Max level = %u", maxLevel);

  frame.futures = std::vector<GPUFuture>(nodes.size());

  std::vector<CommandBuffer> commandBuffers[Queue::QueuesCount];

  if (commandBuffersCount[Queue::Compute] > 0)
  {
    commandBuffers[Queue::Compute] = rhi->allocateCommandBuffers(Queue::Compute, commandBuffersCount[Queue::Compute]);
  }
  if (commandBuffersCount[Queue::Graphics] > 0)
  {
    commandBuffers[Queue::Graphics] = rhi->allocateCommandBuffers(Queue::Graphics, commandBuffersCount[Queue::Graphics]);
  }
  if (commandBuffersCount[Queue::Transfer] > 0)
  {
    commandBuffers[Queue::Transfer] = rhi->allocateCommandBuffers(Queue::Transfer, commandBuffersCount[Queue::Transfer]);
  }
  if (commandBuffersCount[Queue::Present] > 0)
  {
    commandBuffers[Queue::Present] = rhi->allocateCommandBuffers(Queue::Present, commandBuffersCount[Queue::Present]);
  }

  for (auto &cmd : commandBuffers[Queue::Compute])
  {
    rhi->beginCommandBuffer(cmd);
  }
  for (auto &cmd : commandBuffers[Queue::Graphics])
  {
    rhi->beginCommandBuffer(cmd);
  }
  for (auto &cmd : commandBuffers[Queue::Transfer])
  {
    rhi->beginCommandBuffer(cmd);
  }
  for (auto &cmd : commandBuffers[Queue::Present])
  {
    rhi->beginCommandBuffer(cmd);
  }
  // for (auto &node : nodes)
  // {
  //   commandBuffers[node.id] = rhi->allocateCommandBuffers(node.queue, 1)[0];
  //   rhi->beginCommandBuffer(commandBuffers[node.id]);
  //   os::Logger::logf("[RenderGraph] Allocated CommandBuffer for node %u (level=%u queue=%d)", node.id, node.level, logQueue(node.queue));
  // }

  std::unordered_map<CommandBuffer, std::unordered_set<CommandBuffer>> commandBufferWaits;

  for (auto &currentNode : nodes)
  {
    CommandBuffer &commandBuffer = commandBuffers[currentNode.queue][currentNode.commandBufferIndex];

    for (auto &wait : currentNode.waitSemaphores)
    {
      auto &semaphore = semaphores[wait];
      auto &from = nodes[semaphore.signalTask];

      if (commandBuffer != commandBuffers[from.queue][from.commandBufferIndex])
      {
        commandBufferWaits[commandBuffer].insert(commandBuffers[from.queue][from.commandBufferIndex]);
      }
    }

    os::Logger::logf(
        "[RenderGraph] * Recording %s (level=%u queue=%s, commandBuffer %u)",
        currentNode.name.c_str(),
        currentNode.level,
        logQueue(currentNode.queue),
        currentNode.commandBufferIndex);

    /* ================= BUFFER BARRIERS ================= */

    for (auto &transition : currentNode.bufferTransitions)
    {

      auto buffer = getBuffer(transition.resourceId);
      PipelineStage fromStage = PipelineStage::ALL_COMMANDS;
      PipelineStage toStage = PipelineStage::ALL_COMMANDS;

      if (transition.fromNode != -1)
      {
        auto &fromNode = nodes[transition.fromNode];
        switch (fromNode.queue)
        {
        case Queue::Compute:
          fromStage = PipelineStage::COMPUTE_SHADER;
          break;
        case Queue::Graphics:
          fromStage = PipelineStage::ALL_GRAPHICS;
          break;
        case Queue::Transfer:
          fromStage = PipelineStage::TRANSFER;
          break;
        default:
          break;
        }
      }

      switch (currentNode.queue)
      {
      case Queue::Compute:
        toStage = PipelineStage::COMPUTE_SHADER;
        break;
      case Queue::Graphics:
        toStage = PipelineStage::ALL_GRAPHICS;
        break;
      case Queue::Transfer:
        toStage = PipelineStage::TRANSFER;
        break;
      default:
        break;
      }

      // if(currentNode.queue)

      if (transition.toQueue != transition.fromQueue)
      {
        os::Logger::logf(
            "[RenderGraph][Barrier][Buffer][QueueTransfer] '%s' fromNode=%u -> node=%u offset=%zu size=%zu fromAccess=%u toAccess=%u fromQueue=%s toQueue=%s, fromNode %u",
            buffer.name.c_str(),
            transition.fromNode,
            currentNode.id,
            transition.offset,
            transition.size,
            (uint32_t)transition.fromAccess,
            (uint32_t)transition.toAccess,
            logQueue(transition.fromQueue),
            logQueue(transition.toQueue),
            transition.fromNode);

        if (transition.fromNode == -1)
        {
          rhi->cmdBufferBarrier(
              commandBuffer, buffer, fromStage, toStage, transition.fromAccess, transition.toAccess, transition.offset, transition.size, Queue::None, Queue::None);
        }
        else
        {

          auto &fromNode = nodes[transition.fromNode];

          rhi->cmdBufferBarrier(
              commandBuffers[fromNode.queue][fromNode.commandBufferIndex],
              buffer,
              fromStage,
              toStage,
              transition.fromAccess,
              AccessPattern::NONE,
              transition.offset,
              transition.size,
              transition.fromQueue,
              transition.toQueue);

          rhi->cmdBufferBarrier(
              commandBuffer, buffer, fromStage, toStage, AccessPattern::NONE, transition.toAccess, transition.offset, transition.size, transition.fromQueue, transition.toQueue);
        }
      }
      else
      {
        os::Logger::logf(
            "[RenderGraph][Barrier][Buffer] '%s' offset=%zu size=%zu fromAccess=%u toAccess=%u queue=%s",
            buffer.name.c_str(),
            transition.offset,
            transition.size,
            (uint32_t)transition.fromAccess,
            (uint32_t)transition.toAccess,
            logQueue(transition.fromQueue));

        rhi->cmdBufferBarrier(
            commandBuffer, buffer, fromStage, toStage, transition.fromAccess, transition.toAccess, transition.offset, transition.size, transition.fromQueue, transition.toQueue);
      }
    }

    /* ================= IMAGE BARRIERS ================= */

    for (auto &transition : currentNode.textureTransitions)
    {

      auto texture = getTexture(transition.resourceId);
      PipelineStage fromStage = PipelineStage::ALL_COMMANDS;
      PipelineStage toStage = PipelineStage::ALL_COMMANDS;
      if (transition.fromNode != -1)
      {
        auto &fromNode = nodes[transition.fromNode];
        switch (fromNode.queue)
        {
        case Queue::Compute:
          fromStage = PipelineStage::COMPUTE_SHADER;
          break;
        case Queue::Graphics:
          fromStage = PipelineStage::ALL_GRAPHICS;
          break;
        case Queue::Transfer:
          fromStage = PipelineStage::TRANSFER;
          break;
        default:
          break;
        }
      }
      switch (currentNode.queue)
      {
      case Queue::Compute:
        toStage = PipelineStage::COMPUTE_SHADER;
        break;
      case Queue::Graphics:
        toStage = PipelineStage::ALL_GRAPHICS;
        break;
      case Queue::Transfer:
        toStage = PipelineStage::TRANSFER;
        break;
      default:
        break;
      }

      if (transition.toQueue != transition.fromQueue)
      {
        os::Logger::logf(
            "[RenderGraph][Barrier][Image][QueueTransfer] '%s' fromNode=%u -> node=%u layout %u -> %u access %u -> %u mips [%u..%u) layers [%u..%u) fromQueue=%s toQueue=%s",
            texture.name.c_str(),
            transition.fromNode,
            currentNode.id,
            (uint32_t)transition.fromLayout,
            (uint32_t)transition.toLayout,
            (uint32_t)transition.fromAccess,
            (uint32_t)transition.toAccess,
            transition.baseMip,
            transition.baseMip + transition.mipCount,
            transition.baseLayer,
            transition.baseLayer + transition.layerCount,
            logQueue(transition.fromQueue),
            logQueue(transition.toQueue));

        if (transition.fromNode == -1)
        {
          rhi->cmdImageBarrier(
              commandBuffer,
              texture,
              fromStage,
              toStage,
              transition.fromAccess,
              transition.toAccess,
              transition.fromLayout,
              transition.toLayout,
              GetImageAspectFlags(resources.textureMetadatas[transition.resourceId]->textureInfo.format),
              transition.baseMip,
              transition.mipCount,
              transition.baseLayer,
              transition.layerCount,
              Queue::None,
              Queue::None);
        }
        else
        {
          auto &fromNode = nodes[transition.fromNode];

          rhi->cmdImageBarrier(
              commandBuffers[fromNode.queue][fromNode.commandBufferIndex],
              texture,
              fromStage,
              toStage,
              transition.fromAccess,
              AccessPattern::NONE,
              transition.fromLayout,
              transition.toLayout,
              GetImageAspectFlags(resources.textureMetadatas[transition.resourceId]->textureInfo.format),
              transition.baseMip,
              transition.mipCount,
              transition.baseLayer,
              transition.layerCount,
              transition.fromQueue,
              transition.toQueue);
          rhi->cmdImageBarrier(
              commandBuffer,
              texture,
              fromStage,
              toStage,
              AccessPattern::NONE,
              transition.toAccess,
              transition.fromLayout,
              transition.toLayout,
              GetImageAspectFlags(resources.textureMetadatas[transition.resourceId]->textureInfo.format),
              transition.baseMip,
              transition.mipCount,
              transition.baseLayer,
              transition.layerCount,
              transition.fromQueue,
              transition.toQueue);
        }
      }
      else
      {
        os::Logger::logf(
            "[RenderGraph][Barrier][Image] '%s' layout %u -> %u access %u -> %u mips [%u..%u) layers [%u..%u) queue=%s",
            texture.name.c_str(),
            (uint32_t)transition.fromLayout,
            (uint32_t)transition.toLayout,
            (uint32_t)transition.fromAccess,
            (uint32_t)transition.toAccess,
            transition.baseMip,
            transition.baseMip + transition.mipCount,
            transition.baseLayer,
            transition.baseLayer + transition.layerCount,
            logQueue(transition.fromQueue));

        rhi->cmdImageBarrier(
            commandBuffer,
            texture,
            fromStage,
            toStage,
            transition.fromAccess,
            transition.toAccess,
            transition.fromLayout,
            transition.toLayout,
            GetImageAspectFlags(resources.textureMetadatas[transition.resourceId]->textureInfo.format),
            transition.baseMip,
            transition.mipCount,
            transition.baseLayer,
            transition.layerCount,
            transition.fromQueue,
            transition.toQueue);
      }
    }

    /* ================= COMMANDS ================= */

    for (auto &cmd : currentNode.commands)
    {
      switch (cmd.type)
      {
      case BeginRenderPass:
        os::Logger::logf("[RenderGraph][Cmd] BeginRenderPass '%s'", cmd.args.renderPassInfo->name.c_str());
        rhi->cmdBeginRenderPass(commandBuffer, *cmd.args.renderPassInfo);
        break;

      case EndRenderPass:
        os::Logger::logf("[RenderGraph][Cmd] EndRenderPass");
        rhi->cmdEndRenderPass(commandBuffer);
        break;

      case CopyBuffer:
        os::Logger::logf(
            "[RenderGraph][Cmd] CopyBuffer '%s'[%zu] -> '%s'[%zu] size=%zu",
            cmd.args.copyBuffer->src.buffer.name.c_str(),
            cmd.args.copyBuffer->src.offset,
            cmd.args.copyBuffer->dst.buffer.name.c_str(),
            cmd.args.copyBuffer->dst.offset,
            cmd.args.copyBuffer->src.size);

        rhi->cmdCopyBuffer(
            commandBuffer,
            cmd.args.copyBuffer->src.buffer,
            cmd.args.copyBuffer->dst.buffer,
            cmd.args.copyBuffer->src.offset,
            cmd.args.copyBuffer->dst.offset,
            cmd.args.copyBuffer->src.size);
        break;

      case BindBindingGroups:
      {
        auto metadata = resources.bindingGroupsMetadata[cmd.args.bindGroups->groups.name];

        const BindingGroupsInfo &info = metadata->groupsInfo;

        os::Logger::logf(
            "[RenderGraph][Cmd] BindBindingGroups '%s' groupCount=%zu dynamicOffsets=%zu", info.name.c_str(), info.groups.size(), cmd.args.bindGroups->dynamicOffsets.size());

        /* ===== Dynamic Offsets ===== */

        for (size_t i = 0; i < cmd.args.bindGroups->dynamicOffsets.size(); ++i)
        {
          os::Logger::logf("[RenderGraph]       [DynamicOffset] index=%zu value=%u", i, cmd.args.bindGroups->dynamicOffsets[i]);
        }

        /* ===== Groups ===== */

        for (size_t g = 0; g < info.groups.size(); ++g)
        {
          const GroupInfo &group = info.groups[g];

          os::Logger::logf("[RenderGraph]       [BindingGroup] index=%zu name='%s'", g, group.name.c_str());

          /* -------- Buffers -------- */

          for (const BindingBuffer &buf : group.buffers)
          {
            const BufferView &view = buf.bufferView;

            os::Logger::logf(
                "[RenderGraph]          [Buffer] binding=%u name='%s' offset=%llu size=%llu access=%u",
                buf.binding,
                view.buffer.name.c_str(),
                (unsigned long long)view.offset,
                (unsigned long long)view.size,
                (uint32_t)view.access);
          }

          /* -------- Samplers -------- */

          for (const BindingSampler &sampler : group.samplers)
          {
            const TextureView &view = sampler.view;

            const char *textureName = view.texture.name.c_str();

            os::Logger::logf(
                "[RenderGraph]          [Sampler] binding=%u sampler='%s' texture='%s' "
                "mips=[%u..%u) layers=[%u..%u) aspect=%u layout=%u access=%u",
                sampler.binding,
                sampler.sampler.name.c_str(),
                textureName,
                view.baseMipLevel,
                view.baseMipLevel + view.levelCount,
                view.baseArrayLayer,
                view.baseArrayLayer + view.layerCount,
                (uint32_t)view.flags,
                (uint32_t)view.layout,
                (uint32_t)view.access);
          }

          /* -------- Sampled Textures -------- */

          for (const BindingTextureInfo &tex : group.textures)
          {
            const TextureView &view = tex.textureView;

            const char *textureName = view.texture.name.c_str();

            os::Logger::logf(
                "[RenderGraph]          [Texture] binding=%u texture='%s' "
                "mips=[%u..%u) layers=[%u..%u) aspect=%u layout=%u access=%u",
                tex.binding,
                textureName,
                view.baseMipLevel,
                view.baseMipLevel + view.levelCount,
                view.baseArrayLayer,
                view.baseArrayLayer + view.layerCount,
                (uint32_t)view.flags,
                (uint32_t)view.layout,
                (uint32_t)view.access);
          }

          /* -------- Storage Textures -------- */

          for (const BindingStorageTextureInfo &tex : group.storageTextures)
          {
            const TextureView &view = tex.textureView;

            const char *textureName = view.texture.name.c_str();

            os::Logger::logf(
                "[RenderGraph]          [StorageTexture] binding=%u texture='%s' "
                "mips=[%u..%u) layers=[%u..%u) aspect=%u layout=%u access=%u",
                tex.binding,
                textureName,
                view.baseMipLevel,
                view.baseMipLevel + view.levelCount,
                view.baseArrayLayer,
                view.baseArrayLayer + view.layerCount,
                (uint32_t)view.flags,
                (uint32_t)view.layout,
                (uint32_t)view.access);
          }
        }

        rhi->cmdBindBindingGroups(commandBuffer, cmd.args.bindGroups->groups, cmd.args.bindGroups->dynamicOffsets.data(), cmd.args.bindGroups->dynamicOffsets.size());

        break;
      }

      case BindGraphicsPipeline:
        os::Logger::logf("[RenderGraph][Cmd] BindGraphicsPipeline '%s'", cmd.args.graphicsPipeline->name.c_str());
        rhi->cmdBindGraphicsPipeline(commandBuffer, *cmd.args.graphicsPipeline);
        break;

      case BindComputePipeline:
        os::Logger::logf("[RenderGraph][Cmd] BindComputePipeline '%s'", cmd.args.computePipeline->name.c_str());
        rhi->cmdBindComputePipeline(commandBuffer, *cmd.args.computePipeline);
        break;

      case BindVertexBuffer:
        os::Logger::logf(
            "[RenderGraph][Cmd] BindVertexBuffer slot=%u '%s' offset=%zu",
            cmd.args.bindVertexBuffer->slot,
            cmd.args.bindVertexBuffer->buffer.buffer.name.c_str(),
            cmd.args.bindVertexBuffer->buffer.offset);

        rhi->cmdBindVertexBuffer(commandBuffer, cmd.args.bindVertexBuffer->slot, cmd.args.bindVertexBuffer->buffer.buffer, cmd.args.bindVertexBuffer->buffer.offset);
        break;

      case BindIndexBuffer:
        os::Logger::logf(
            "[RenderGraph][Cmd] BindIndexBuffer '%s' offset=%zu type=%u",
            cmd.args.bindIndexBuffer->buffer.buffer.name.c_str(),
            cmd.args.bindIndexBuffer->buffer.offset,
            (uint32_t)cmd.args.bindIndexBuffer->type);

        rhi->cmdBindIndexBuffer(commandBuffer, cmd.args.bindIndexBuffer->buffer.buffer, cmd.args.bindIndexBuffer->type, cmd.args.bindIndexBuffer->buffer.offset);
        break;

      case Draw:
        os::Logger::logf(
            "[RenderGraph][Cmd] Draw vertices=%u instances=%u firstVertex=%u firstInstance=%u",
            cmd.args.draw->vertexCount,
            cmd.args.draw->instanceCount,
            cmd.args.draw->firstVertex,
            cmd.args.draw->firstInstance);

        rhi->cmdDraw(commandBuffer, cmd.args.draw->vertexCount, cmd.args.draw->instanceCount, cmd.args.draw->firstVertex, cmd.args.draw->firstInstance);
        break;

      case DrawIndexed:
        os::Logger::logf(
            "[RenderGraph][Cmd] DrawIndexed indices=%u instances=%u firstIndex=%u vertexOffset=%d firstInstance=%u",
            cmd.args.drawIndexed->indexCount,
            cmd.args.drawIndexed->instanceCount,
            cmd.args.drawIndexed->firstIndex,
            cmd.args.drawIndexed->vertexOffset,
            cmd.args.drawIndexed->firstInstance);

        rhi->cmdDrawIndexed(
            commandBuffer,
            cmd.args.drawIndexed->indexCount,
            cmd.args.drawIndexed->instanceCount,
            cmd.args.drawIndexed->firstIndex,
            cmd.args.drawIndexed->vertexOffset,
            cmd.args.drawIndexed->firstInstance);
        break;

      case DrawIndexedIndirect:
        os::Logger::logf(
            "[RenderGraph][Cmd] DrawIndexedIndirect buffer='%s' offset=%zu count=%u stride=%u",
            cmd.args.drawIndexedIndirect->buffer.buffer.name.c_str(),
            cmd.args.drawIndexedIndirect->buffer.offset,
            cmd.args.drawIndexedIndirect->drawCount,
            cmd.args.drawIndexedIndirect->stride);

        rhi->cmdDrawIndexedIndirect(
            commandBuffer,
            cmd.args.drawIndexedIndirect->buffer.buffer,
            cmd.args.drawIndexedIndirect->buffer.offset,
            cmd.args.drawIndexedIndirect->drawCount,
            cmd.args.drawIndexedIndirect->stride);
        break;

      case Dispatch:
        os::Logger::logf("[RenderGraph][Cmd] Dispatch (%u, %u, %u)", cmd.args.dispatch->x, cmd.args.dispatch->y, cmd.args.dispatch->z);

        rhi->cmdDispatch(commandBuffer, cmd.args.dispatch->x, cmd.args.dispatch->y, cmd.args.dispatch->z);
        break;

      case StartTimer:
        rhi->cmdStartTimer(commandBuffer, cmd.args.startTimer->timer, cmd.args.startTimer->stage);
        break;
      case StopTimer:
        rhi->cmdStopTimer(commandBuffer, cmd.args.stopTimer->timer, cmd.args.stopTimer->stage);
        break;
      }
    }
  }

  for (auto &cmd : commandBuffers[Queue::Compute])
  {
    rhi->endCommandBuffer(cmd);
  }
  for (auto &cmd : commandBuffers[Queue::Graphics])
  {
    rhi->endCommandBuffer(cmd);
  }
  for (auto &cmd : commandBuffers[Queue::Transfer])
  {
    rhi->endCommandBuffer(cmd);
  }

  auto submitStart = lib::time::TimeSpan::now();

  std::unordered_map<CommandBuffer, GPUFuture> futures;

  std::vector<CommandBuffer> orderedBuffers;
  uint64_t at[Queue::QueuesCount] = {};

  for (auto queue = Queue::None; queue < Queue::QueuesCount; queue = Queue((uint64_t)queue + 1))
  {
    at[queue] = 0;
  }

  while (true)
  {
    bool finished = true;

    for (auto queue = Queue::None; queue < Queue::QueuesCount; queue = Queue((uint64_t)queue + 1))
    {
      if (at[queue] != commandBuffers[queue].size())
      {
        finished = false;
      }
    }

    if (finished == true)
    {
      break;
    }

    std::vector<GPUFuture> waits;

    CommandBuffer commandBuffer;

    Queue queue;

    for (queue = Queue::None; queue < Queue::QueuesCount; queue = Queue((uint64_t)queue + 1))
    {
      if (commandBuffers[queue].size() == 0 || at[queue] == commandBuffers[queue].size())
      {
        continue;
      }

      bool allDependenciesSubmitted = true;

      waits.clear();

      commandBuffer = commandBuffers[queue][at[queue]];

      for (const auto &waitCommandBuffer : commandBufferWaits[commandBuffer])
      {
        allDependenciesSubmitted = futures.count(waitCommandBuffer) > 0;
        if (!allDependenciesSubmitted)
        {
          break;
        }
        waits.push_back(futures[waitCommandBuffer]);
      }

      if (allDependenciesSubmitted)
      {
        at[queue] += 1;
        break;
      }
    }

    futures[commandBuffer] = rhi->submit(queue, &commandBuffer, 1, waits.data(), waits.size());
    os::Logger::logf("[RenderGraph][Submit] Submitting commandBuffer %u queue=%s waits=%zu", (uint64_t)commandBuffer, logQueue(queue), waits.size());

    frame.futures.push_back(futures[commandBuffer]);
  }

  auto runEnd = lib::time::TimeSpan::now();

  os::Logger::logf("[RenderGraph] ===== End run recordTime=%fms, submitTime=%fms =====", (submitStart - runStart).milliseconds(), (runEnd - runStart).milliseconds());
}

void RenderGraph::waitFrame(Frame &frame)
{
  for (auto &future : frame.futures)
  {
    while (!rhi->isCompleted(future))
    {
      // TODO: yhield in job system
    }
  }
}

double RenderGraph::readTimer(const Timer &timer)
{
  return rhi->readTimer(timer);
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

    if (has(BufferUsage_Storage) || has(BufferUsage_Uniform) || has(BufferUsage_Vertex) || has(BufferUsage_Index) || has(BufferUsage_Indirect) || has(BufferUsage_Timestamp) ||
        has(BufferUsage_CopySrc))
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

  if (!(has(BufferUsage_CopySrc) || has(BufferUsage_CopyDst) || has(BufferUsage_Uniform) || has(BufferUsage_Storage) || has(BufferUsage_Vertex) || has(BufferUsage_Index) ||
        has(BufferUsage_Indirect) || has(BufferUsage_Timestamp)))
  {
    RENDER_GRAPH_FATAL("[RenderGraph] Buffer '%s' has no GPU-visible usage flags", info.name.c_str());
  }
}

RHICommandBuffer::RHICommandBuffer()
{
  recorded.push_back(CommandSequence());
}

void RenderGraph::deleteBuffer(const Buffer &name)
{
  if (!resources.bufferMetadatas.remove(name.name))
  {
    RENDER_GRAPH_FATAL("Buffer %s not found", name.name.c_str());
  }

  rhi->deleteBuffer(name);
}

void RenderGraph::deleteTexture(const Texture &name)
{
  if (!resources.textureMetadatas.remove(name.name))
  {
    RENDER_GRAPH_FATAL("Texture %s not found", name.name.c_str());
  }
  rhi->deleteTexture(name);
}

void RenderGraph::deleteSampler(const Sampler &name)
{
  if (!resources.samplerMetadatas.remove(name.name))
  {
    RENDER_GRAPH_FATAL("Sampler %s not found", name.name.c_str());
  }
  rhi->deleteSampler(name);
}

void RenderGraph::deleteBindingsLayout(const BindingsLayout &name)
{
  if (!resources.bindingsLayoutMetadata.remove(name.name))
  {
    RENDER_GRAPH_FATAL("Bindings Layout %s not found", name.name.c_str());
  }

  rhi->deleteBindingsLayout(name);
}

void RenderGraph::deleteBindingGroups(const BindingGroups &name)
{
  if (!resources.bindingGroupsMetadata.remove(name.name))
  {
    RENDER_GRAPH_FATAL("Binding Groups %s not found", name.name.c_str());
  }

  rhi->deleteBindingGroups(name);
}

void RenderGraph::deleteGraphicsPipeline(const GraphicsPipeline &name)
{
  if (!resources.graphicsPipelineMetadata.remove(name.name))
  {
    RENDER_GRAPH_FATAL("Graphics Pipeline %s not found", name.name.c_str());
  }
  rhi->deleteGraphicsPipeline(name);
}

void RenderGraph::deleteComputePipeline(const ComputePipeline &name)
{
  if (!resources.computePipelineMetadata.remove(name.name))
  {
    RENDER_GRAPH_FATAL("Compute Pipeline %s not found", name.name.c_str());
  }
  rhi->deleteComputePipeline(name);
}

const Buffer RenderGraph::createBuffer(const BufferInfo &info)
{
  if (resources.bufferMetadatas.contains(info.name))
  {
    throw std::runtime_error("Buffer already created");
  }

  validateBufferUsage(info);

  const auto &name = info.name;

  resources.bufferMetadatas.insert(
      name,
      BufferResourceMetadata{
        .bufferInfo = info,
        .firstUsedAt = UINT64_MAX,
        .lastUsedAt = 0,
      });

  return rhi->createBuffer(info);
}

const Texture RenderGraph::createTexture(const TextureInfo &info)
{
  const auto &name = info.name;
  if (resources.textureMetadatas.contains(info.name))
  {
    throw std::runtime_error("Texture already created");
  }
  resources.textureMetadatas.insert(
      name,
      TextureResourceMetadata{
        .textureInfo = info,
      });

  return rhi->createTexture(info);
}

const Sampler RenderGraph::createSampler(const SamplerInfo &info)
{
  const auto &name = info.name;
  if (resources.samplerMetadatas.contains(info.name))
  {
    throw std::runtime_error("Sampler already created");
  }
  resources.samplerMetadatas.insert(
      name,
      SamplerResourceMetadata{
        .samplerInfo = info,
      });

  return rhi->createSampler(info);
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
  const auto &name = info.name;

  if (resources.bindingGroupsMetadata.contains(info.name))
  {
    throw std::runtime_error("Binding Groups already created");
  }

  auto layoutObject = resources.bindingsLayoutMetadata[info.layout.name];

  if (layoutObject->layoutsInfo.groups.size() != info.groups.size())
  {
    RENDER_GRAPH_FATAL("[RenderGraph] binding groups %s size don't match given layout %s", info.name.c_str(), info.layout.name.c_str());
  }

  for (uint32_t i = 0; i < info.groups.size(); i++)
  {
    if (layoutObject->layoutsInfo.groups[i].buffers.size() != info.groups[i].buffers.size())
    {
      RENDER_GRAPH_FATAL("[RenderGraph] binding groups %s buffers size don't match given layout %s", info.name.c_str(), info.layout.name.c_str());
    }
    if (layoutObject->layoutsInfo.groups[i].samplers.size() != info.groups[i].samplers.size())
    {
      RENDER_GRAPH_FATAL("[RenderGraph] binding groups %s samplers size don't match given layout %s", info.name.c_str(), info.layout.name.c_str());
    }
    if (layoutObject->layoutsInfo.groups[i].storageTextures.size() != info.groups[i].storageTextures.size())
    {
      RENDER_GRAPH_FATAL("[RenderGraph] binding groups %s storageTextures size don't match given layout %s", info.name.c_str(), info.layout.name.c_str());
    }
    if (layoutObject->layoutsInfo.groups[i].textures.size() != info.groups[i].textures.size())
    {
      RENDER_GRAPH_FATAL("[RenderGraph] binding groups %s textures size don't match given layout %s", info.name.c_str(), info.layout.name.c_str());
    }
  }

  for (uint32_t i = 0; i < info.groups.size(); i++)
  {
    for (uint32_t j = 0; j < layoutObject->layoutsInfo.groups[i].buffers.size(); j++)
    {
      if (layoutObject->layoutsInfo.groups[i].buffers[j].type == BufferBindingType::BufferBindingType_StorageBuffer)
      {
        auto usage = resources.bufferMetadatas[info.groups[i].buffers[j].bufferView.buffer.name]->bufferInfo.usage;
        if ((usage & BufferUsage::BufferUsage_Storage) == 0)
        {
          RENDER_GRAPH_FATAL(
              "[RenderGraph] binding groups %s at group %u, buffer %s bound with type BufferBindingType_StorageBuffer, but buffer usage did not include BufferUsage_Storage",
              info.name.c_str(),
              i,
              info.groups[i].buffers[j].bufferView.buffer.name.c_str());
        }
      }

      if (layoutObject->layoutsInfo.groups[i].buffers[j].type == BufferBindingType::BufferBindingType_UniformBuffer)
      {
        auto usage = resources.bufferMetadatas[info.groups[i].buffers[j].bufferView.buffer.name]->bufferInfo.usage;
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
        RENDER_GRAPH_FATAL(
            "[RenderGraph] Invalid layout for sampler %s in group %i, expects GENERAL, SHADER_READ_ONLY or DEPTH_STENCIL_READ_ONLY",
            info.groups[i].samplers[j].sampler.name.c_str(),
            i);
      }
    }
  }

  resources.bindingGroupsMetadata.insert(
      name,
      BindingGroupsResourceMetadata{
        .groupsInfo = info,
      });

  assert(resources.bindingGroupsMetadata.contains(name));

  return rhi->createBindingGroups(info);
}

const GraphicsPipeline RenderGraph::createGraphicsPipeline(const GraphicsPipelineInfo &info)
{
  const auto &name = info.name;

  if (resources.graphicsPipelineMetadata.contains(info.name))
  {
    throw std::runtime_error("Graphics Pipeline already created");
  }

  resources.graphicsPipelineMetadata.insert(
      name,
      GraphicsPipelineResourceMetadata{
        .pipelineInfo = info,
      });

  return rhi->createGraphicsPipeline(info);
}

const ComputePipeline RenderGraph::createComputePipeline(const ComputePipelineInfo &info)
{
  const auto &name = info.name;

  if (resources.computePipelineMetadata.contains(info.name))
  {
    throw std::runtime_error("Compute Pipeline already created");
  }

  resources.computePipelineMetadata.insert(
      name,
      ComputePipelineResourceMetadata{
        .pipelineInfo = info,
      });

  return rhi->createComputePipeline(info);
}

const BindingsLayout RenderGraph::createBindingsLayout(const BindingsLayoutInfo &info)
{
  const auto &name = info.name;

  if (resources.bindingsLayoutMetadata.contains(info.name))
  {
    throw std::runtime_error("Binding Layout already created");
  }

  resources.bindingsLayoutMetadata.insert(
      name,
      BindingsLayoutResourceMetadata{
        .layoutsInfo = info,
      });

  assert(resources.bindingsLayoutMetadata.contains(name));
  return rhi->createBindingsLayout(info);
}

const Shader RenderGraph::createShader(const ShaderInfo info)
{
  const auto &name = info.name;

  if (resources.shadersMetadatas.contains(info.name))
  {
    throw std::runtime_error("Shader already created");
  }

  resources.shadersMetadatas.insert(
      name,
      ShaderResourceMetadata{
        .info = info,
      });

  return rhi->createShader(info);
}

void RenderGraph::deleteShader(Shader handle)
{
  if (!resources.shadersMetadatas.remove(handle.name))
  {
    RENDER_GRAPH_FATAL("Shader %s not found", handle.name.c_str());
  }

  rhi->deleteShader(handle);
}

void RenderGraph::bufferRead(const Buffer &buffer, const uint64_t offset, const uint64_t size, std::function<void(const void *)> callback)
{
  rhi->bufferRead(buffer, offset, size, callback);
}

void RenderGraph::bufferWrite(const Buffer &buffer, const uint64_t offset, const uint64_t size, void *data)
{
  // TODO: check buffer usage
  rhi->bufferWrite(buffer, offset, size, data);
}

// const Buffer RenderGraph::createScratchBuffer(const BufferInfo &info)
// {
//   const auto &name = info.name;

//   resources.scratchBuffersRequestsMetadatas.insert(
//       name,
//       ScratchBufferResourceMetadata{
//         .bufferInfo = info,
//       });

//   return Buffer{
//     .name = info.name,
//   };
// }

// const Buffer RHIResources::getScratchBuffer(const BufferInfo &info)
// {
//   if (!scratchBuffersRequestsMetadatas.contains(info.name))
//   {
//     scratchBuffersRequestsMetadatas.insert(
//         info.name,
//         ScratchBufferResourceMetadata{
//           .bufferInfo = info,
//           .firstUsedAt = UINT64_MAX,
//           .lastUsedAt = 0,
//         });
//   }

//   return Buffer{
//     .name = info.name,
//   };
// }

const Buffer RHIResources::getBuffer(const std::string &name)
{
  if (!bufferMetadatas.contains(name))
  {
    throw std::runtime_error("Buffer not found");
  }

  return Buffer{
    .name = name,
  };
}

const BindingsLayout RHIResources::getBindingsLayout(const std::string &name)
{
  if (!bindingsLayoutMetadata.contains(name))
  {
    throw std::runtime_error("BindingsLayout not found");
  }

  return BindingsLayout{
    .name = name,
  };
}

const BindingGroups RHIResources::getBindingGroups(const std::string &name)
{
  if (!bindingGroupsMetadata.contains(name))
  {
    throw std::runtime_error("BindingGroups not found");
  }

  return BindingGroups{
    .name = name,
  };
}
const GraphicsPipeline RHIResources::getGraphicsPipeline(const std::string &name)
{
  if (!graphicsPipelineMetadata.contains(name))
  {
    throw std::runtime_error("GraphicsPipeline not found");
  }

  return GraphicsPipeline{
    .name = name,
  };
}

const ComputePipeline RHIResources::getComputePipeline(const std::string &name)
{
  if (!computePipelineMetadata.contains(name))
  {
    throw std::runtime_error("ComputePipeline not found");
  }

  return ComputePipeline{
    .name = name,
  };
}

const Sampler RHIResources::getSampler(const std::string &name)
{
  if (!samplerMetadatas.contains(name))
  {
    throw std::runtime_error("Sampler not found");
  }
  return Sampler{
    .name = name,
  };
}

const Texture RHIResources::getTexture(const std::string &name)
{
  if (!textureMetadatas.contains(name))
  {
    os::print("texture = %s\n", name.c_str());
    throw std::runtime_error("Texture not found");
  }

  return Texture{
    .name = name,
  };
}

// const Buffer RenderGraph::getScratchBuffer(BufferInfo &info)
// {
//   return resources.getScratchBuffer(info);
// }

// const Buffer RenderGraph::getScratchBuffer(BufferInfo &info)
// {
//   return resources.getScratchBuffer(info);
// }
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

void RHICommandBuffer::cmdBeginRenderPass(const RenderPassInfo &info)
{
  Command cmd{CommandType::BeginRenderPass};
  cmd.args.renderPassInfo = new RenderPassInfo();
  *cmd.args.renderPassInfo = info;
  recorded.back().commands.push_back(std::move(cmd));
}

void RHICommandBuffer::cmdStartTimer(const Timer timer, PipelineStage stage)
{
  Command cmd{CommandType::StartTimer};
  cmd.args.startTimer = new StartTimerArgs();
  cmd.args.startTimer->timer = timer;
  cmd.args.startTimer->stage = stage;

  recorded.back().commands.push_back(std::move(cmd));
}
void RHICommandBuffer::cmdStopTimer(const Timer timer, PipelineStage stage)
{
  Command cmd{CommandType::StopTimer};
  cmd.args.stopTimer = new StopTimerArgs();
  cmd.args.stopTimer->timer = timer;
  cmd.args.stopTimer->stage = stage;
  recorded.back().commands.push_back(std::move(cmd));
}

void RHICommandBuffer::cmdEndRenderPass()
{
  recorded.back().commands.push_back({CommandType::EndRenderPass});
}

void RHICommandBuffer::cmdCopyBuffer(BufferView src, BufferView dst)
{
  Command cmd{CommandType::CopyBuffer};
  cmd.args.copyBuffer = new CopyBufferArgs();
  *cmd.args.copyBuffer = {src, dst};

  recorded.back().commands.push_back(std::move(cmd));
}

void RHICommandBuffer::cmdBindBindingGroups(BindingGroups groups, uint32_t *dynamicOffsets, uint32_t dynamicOffsetsCount)
{
  Command cmd{CommandType::BindBindingGroups};
  cmd.args.bindGroups = new BindGroupsArgs();
  cmd.args.bindGroups->groups = groups;
  cmd.args.bindGroups->dynamicOffsets.assign(dynamicOffsets, dynamicOffsets + dynamicOffsetsCount);
  recorded.back().commands.push_back(std::move(cmd));
}

void RHICommandBuffer::cmdBindGraphicsPipeline(GraphicsPipeline pipeline)
{
  Command cmd{CommandType::BindGraphicsPipeline};
  cmd.args.graphicsPipeline = new GraphicsPipeline();
  *cmd.args.graphicsPipeline = pipeline;
  recorded.back().commands.push_back(std::move(cmd));
}

void RHICommandBuffer::cmdBindComputePipeline(ComputePipeline pipeline)
{
  Command cmd{CommandType::BindComputePipeline};
  cmd.args.computePipeline = new ComputePipeline();
  *cmd.args.computePipeline = pipeline;
  recorded.back().commands.push_back(std::move(cmd));
}

void RHICommandBuffer::cmdBindVertexBuffer(uint32_t slot, BufferView view)
{
  Command cmd{CommandType::BindVertexBuffer};
  cmd.args.bindVertexBuffer = new BindVertexBufferArgs();
  *cmd.args.bindVertexBuffer = {slot, view};
  recorded.back().commands.push_back(std::move(cmd));
}

void RHICommandBuffer::cmdBindIndexBuffer(BufferView view, Type type)
{
  Command cmd{CommandType::BindIndexBuffer};
  cmd.args.bindIndexBuffer = new BindIndexBufferArgs();
  *cmd.args.bindIndexBuffer = {view, type};
  recorded.back().commands.push_back(std::move(cmd));
}

void RHICommandBuffer::cmdDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
  Command cmd{CommandType::Draw};
  cmd.args.draw = new DrawArgs();
  *cmd.args.draw = {vertexCount, instanceCount, firstVertex, firstInstance};
  recorded.back().commands.push_back(std::move(cmd));
  // recorded.push_back(CommandSequence());
}

void RHICommandBuffer::cmdDrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, uint32_t vertexOffset, uint32_t firstInstance)
{
  Command cmd{CommandType::DrawIndexed};
  cmd.args.drawIndexed = new DrawIndexedArgs();
  *cmd.args.drawIndexed = {indexCount, instanceCount, firstIndex, firstInstance, vertexOffset};
  recorded.back().commands.push_back(std::move(cmd));
  // recorded.push_back(CommandSequence());
}

void RHICommandBuffer::cmdDrawIndexedIndirect(BufferView buffer, uint32_t offset, uint32_t drawCount, uint32_t stride)
{
  Command cmd{CommandType::DrawIndexedIndirect};
  cmd.args.drawIndexedIndirect = new DrawIndexedIndirectArgs();
  *cmd.args.drawIndexedIndirect = {buffer, offset, drawCount, stride};
  recorded.back().commands.push_back(std::move(cmd));
  // recorded.push_back(CommandSequence());
}

void RHICommandBuffer::cmdDispatch(uint32_t x, uint32_t y, uint32_t z)
{
  Command cmd{CommandType::Dispatch};
  cmd.args.dispatch = new DispatchArgs();
  *cmd.args.dispatch = {x, y, z};
  recorded.back().commands.push_back(std::move(cmd));
  // recorded.push_back(CommandSequence());
}

void RenderGraph::addSwapChainImages(SwapChain sc)
{
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

    resources.textureMetadatas.insert(
        info.name,
        TextureResourceMetadata{
          .textureInfo = info,
        });
  }
}
void RenderGraph::removeSwapChainImages(SwapChain sc)
{
  uint64_t imagesCount = rhi->getSwapChainImagesCount(sc);

  for (uint64_t index = 0; index < imagesCount; index++)
  {
    auto name = "_SwapChainImage[" + std::to_string((uint64_t)sc) + "," + std::to_string(index) + "].texture";
    resources.textureMetadatas.remove(name);
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
