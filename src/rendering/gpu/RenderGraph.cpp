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

void RenderGraph::registerConsumer(const std::string &name, const InputResource &res, uint32_t taskId)
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

    id->bufferUsages.push_back(
        BufferResourceUsage{
          .view = res.bufferView,
          .consumer = taskId,
        });
  }
  break;
  case ResourceType::ResourceType_TextureView:
  {
    auto id = resources.textureMetadatas.find(name);

    if (id == resources.textureMetadatas.end())
    {
      throw std::runtime_error("Texture not found");
    }

    id->textureUsages.push_back(
        TextureResourceUsage{
          .view = res.textureView,
          .consumer = taskId,
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

    id->samplerUsages.push_back(
        SamplerResourceUsage{
          .sampler = res.sampler,
          .consumer = taskId,
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

    id->layoutUsages.push_back(
        BindingsLayoutResourceUsage{
          .consumer = taskId,
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

    id->layoutUsages.push_back(
        BindingGroupsResourceUsage{
          .consumer = taskId,
        });
  }
  break;
  default:
    os::Logger::errorf("Unknown resource type %u", res.type);
    exit(1);
    break;
  }
}

RHIResources::RHIResources(RenderGraph *renderGraph) : renderGraph(renderGraph)
{
}

RenderGraph::RenderGraph(RHI *renderingHardwareInterface) : renderingHardwareInterface(renderingHardwareInterface), resources(this)
{
  compiled = false;

  enqueuePass(
      "Initialization",
      RenderGraph::ExecuteOnFirstRun,
      [](const RHIResources &rg, RHICommandBuffer &recorder)
      {
      });
}

void RenderGraph::analyseCommands(RHICommandBuffer &recorder)
{
  for (auto &commands : recorder.recorded)
  {
    std::unordered_map<CommandType, uint32_t> dispatchCount;

    for (auto &cmd : commands.commands)
    {
      dispatchCount[cmd.type] += 1;

      switch (cmd.type)
      {
      case CommandType::CopyBuffer:
        break;
      default:
        if (dispatchCount[cmd.type] > 1)
        {
          os::Logger::error("Command being called multiple times before dispatch");
          exit(1);
        }
        break;
      }

      if (cmd.type >= Draw && cmd.type <= Dispatch)
      {
        dispatchCount.clear();
      }
    }
  }
}

Queue inferQueue(const std::vector<Command> &commands)
{
  if (commands.size() == 0)
  {
    return Queue::None;
  }

  switch (commands.back().type)
  {
  case CopyBuffer:
    return Queue::Transfer;
  case Draw:
  case DrawIndexed:
  case DrawIndexedIndirect:
    return Queue::Graphics;
  case Dispatch:
    return Queue::Compute;
  default:
    return Queue::None;
  }

  return Queue::None;
}

void RenderGraph::enqueuePass(std::string name, std::function<bool(const RenderGraph &)> shouldExecute, std::function<void(RHIResources &, RHICommandBuffer &)> record)
{
  auto pass = RenderGraphPass{
    .name = name,
    .record = record,
    .shouldExecute = shouldExecute,
  };

  passes.enqueue(pass);
}

void RenderGraph::analysePasses()
{
  RHICommandBuffer **ref;
  RenderGraphPass pass;

  while (passes.dequeue(pass))
  {
    RHICommandBuffer recorder;

    pass.record(resources, recorder);

    analyseCommands(recorder);

    uint32_t index = 0;

    for (auto &commands : recorder.recorded)
    {
      uint32_t id = nodes.size();

      if (commands.commands.size() == 0 && id != 0)
      {
        continue;
      }
      auto node = RenderGraphNode();

      node.name = pass.name + "[" + std::to_string(index++) + "]";

      node.id = id;
      node.level = 0;
      node.priority = id;
      node.commands = std::move(commands.commands);
      node.queue = inferQueue(commands.commands);

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
                id);
          }
          if (cmd.args.renderPassInfo->depthStencilAttachment.clearDepth)
          {
            auto &attatchment = cmd.args.renderPassInfo->depthStencilAttachment;
            registerConsumer(
                attatchment.view.texture.name,
                InputResource{
                  .type = ResourceType::ResourceType_TextureView,
                  .textureView = attatchment.view,
                  .layout = attatchment.view.layout,
                  .access = attatchment.view.access,
                },
                id);
          }
          break;
        case EndRenderPass:
          break;
        case CopyBuffer:
          registerConsumer(
              cmd.args.copyBuffer->src.buffer.name,
              InputResource{
                .type = ResourceType::ResourceType_BufferView,
                .bufferView = cmd.args.copyBuffer->src,
                .layout = ResourceLayout::UNDEFINED,
                .access = cmd.args.copyBuffer->src.access,
              },
              id);
          registerConsumer(
              cmd.args.copyBuffer->dst.buffer.name,
              InputResource{
                .type = ResourceType::ResourceType_BufferView,
                .bufferView = cmd.args.copyBuffer->dst,
                .layout = ResourceLayout::UNDEFINED,
                .access = cmd.args.copyBuffer->dst.access,
              },
              id);

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
              id);
          registerConsumer(
              cmd.args.bindGroups->groups.name,
              InputResource{
                .type = ResourceType::ResourceType_BindingGroups,
                .bindingGroups = cmd.args.bindGroups->groups,
                .layout = ResourceLayout::UNDEFINED,
                .access = AccessPattern::NONE,
              },
              id);

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
                  id);
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
                  id);
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
                  id);
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
                  id);
              registerConsumer(
                  texture.sampler.name,
                  InputResource{
                    .type = ResourceType::ResourceType_Sampler,
                    .sampler = texture.sampler,
                    .layout = ResourceLayout::UNDEFINED,
                    .access = AccessPattern::NONE,
                  },
                  id);
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
              id);
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
              id);
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
              id);
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
              id);
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
              id);
          break;
        case Draw:
        case DrawIndexed:
        case Dispatch:
          break;
        default:
          os::Logger::error("Unsuported command");
          exit(1);
          break;
        }
      }
      // taskData[id].registry.sortResources();
    }
  }
  *ref = nullptr;
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
    os::Logger::errorf("Cyclical dependency in Task Graph");
    exit(1);
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

// void RenderGraph::analyseTaskLevels()
// {
//   std::vector<uint32_t> topologicalOrder;

//   tasksTopologicalSort(topologicalOrder);

//   std::vector<bool> visited(taskData.size(), false);

//   for (uint32_t id = 0; id < taskData.size(); id++)
//   {
//     taskData[id].level = 0;
//   }

//   for (uint32_t id : topologicalOrder)
//   {
//     levelDFS(id, visited, 1);
//   }
// }

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
        uint64_t increment = edge.type == EdgeType::ResourceShare ? 0 : 1;
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
        auto srcMeta = resources.scratchBuffersRequestsMetadatas.find(cmd.args.copyBuffer->src.buffer.name);
        srcMeta->firstUsedAt = std::min(srcMeta->firstUsedAt, node.level);
        srcMeta->lastUsedAt = std::max(srcMeta->lastUsedAt, node.level);

        auto dstMeta = resources.scratchBuffersRequestsMetadatas.find(cmd.args.copyBuffer->dst.buffer.name);
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
            auto meta = resources.scratchBuffersRequestsMetadatas.find(buffer.bufferView.buffer.name);
            meta->firstUsedAt = std::min(meta->firstUsedAt, node.level);
            meta->lastUsedAt = std::max(meta->lastUsedAt, node.level);
          }
        }
        break;
      }

      case BindVertexBuffer:
      {
        auto meta = resources.scratchBuffersRequestsMetadatas.find(cmd.args.bindVertexBuffer->buffer.buffer.name);
        meta->firstUsedAt = std::min(meta->firstUsedAt, node.level);
        meta->lastUsedAt = std::max(meta->lastUsedAt, node.level);
        break;
      }

      case BindIndexBuffer:
      {
        auto meta = resources.scratchBuffersRequestsMetadatas.find(cmd.args.bindIndexBuffer->buffer.buffer.name);
        meta->firstUsedAt = std::min(meta->firstUsedAt, node.level);
        meta->lastUsedAt = std::max(meta->lastUsedAt, node.level);
        break;
      }

      case DrawIndexedIndirect:
      {
        auto meta = resources.scratchBuffersRequestsMetadatas.find(cmd.args.drawIndexedIndirect->buffer.buffer.name);
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
        break;

      default:
        os::Logger::error("Unsupported command");
        exit(1);
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
  bool operator==(const AccessConsumerPair &o) const
  {
    return access == o.access && consumer == o.consumer;
  }
  bool operator!=(const AccessConsumerPair &o) const
  {
    return access != o.access || consumer != o.consumer;
  }
};

struct AccessLayoutConsumerTriple
{
  AccessPattern access;
  ResourceLayout layout;
  uint64_t consumer;

  bool operator==(const AccessLayoutConsumerTriple &o) const
  {
    return access == o.access && layout == o.layout && consumer == o.consumer;
  }

  bool operator!=(const AccessLayoutConsumerTriple &o) const
  {
    return access != o.access || layout != o.layout || consumer != o.consumer;
  }
};

void RenderGraph::analyseDependencyGraph()
{
  edges.clear();
  edges.resize(nodes.size());

  for (auto &node : nodes)
  {
    if (node.id == 0)
    {
      continue;
    }
    // Bootstrap to all other edges
    edges[0].push_back(
        RenderGraphEdge{
          .taskId = node.id,
          .resourceId = "",
          .resourceType = ResourceType::ResourceType_Initialization,
          .type = EdgeType::Initialization,
        });
  }

  for (auto [name, meta] : resources.bufferMetadatas)
  {
    std::sort(
        meta.bufferUsages.begin(),
        meta.bufferUsages.end(),
        [this](BufferResourceUsage taskA, BufferResourceUsage taskB)
        {
          return nodes[taskA.consumer].priority < nodes[taskB.consumer].priority;
        });

    std::vector<lib::BoundedTaggedIntervalTree<AccessConsumerPair, uint64_t>::Interval> intervals;

    intervals.reserve(4 * meta.bufferUsages.size());

    lib::BoundedTaggedIntervalTree<AccessConsumerPair, uint64_t> bufferIntevals(meta.bufferUsages.size() * 4);

    bufferIntevals.insert(0, meta.bufferInfo.size - 1, AccessConsumerPair{.access = AccessPattern::NONE, .consumer = (uint64_t)-1});

    for (const auto &usage : meta.bufferUsages)
    {
      intervals.clear();

      bufferIntevals.queryAll(usage.view.offset, usage.view.offset + usage.view.size - 1, intervals);

      for (const auto &interval : intervals)
      {
        if (interval.tag.consumer == usage.consumer)
        {
          continue;
        }

        if (interval.tag.consumer != -1)
        {
          edges[interval.tag.consumer].emplace_back(
              RenderGraphEdge{
                .type = interval.tag.access != usage.view.access ? EdgeType::ResourceDependency : EdgeType::ResourceShare,
                .taskId = usage.consumer,
                .resourceId = meta.bufferInfo.name,
                .resourceType = ResourceType::ResourceType_BufferView,
              });
        }

        bufferIntevals.remove(interval.start, interval.end, interval.tag);
        bufferIntevals.insert(interval.start, interval.end, AccessConsumerPair{.access = usage.view.access, .consumer = usage.consumer});
      }
    }
  }

  for (auto [name, meta] : resources.textureMetadatas)
  {
    std::sort(
        meta.textureUsages.begin(),
        meta.textureUsages.end(),
        [this](TextureResourceUsage taskA, TextureResourceUsage taskB)
        {
          return nodes[taskA.consumer].priority < nodes[taskB.consumer].priority;
        });

    std::vector<lib::BoundedTaggedRectTreap<AccessLayoutConsumerTriple, uint64_t>::Rect> intervals;
    intervals.reserve(resources.textureMetadatas.size() * 4);
    lib::BoundedTaggedRectTreap<AccessLayoutConsumerTriple, uint64_t> textureState(meta.textureUsages.size() * 4);
    textureState.insert(
        0,
        0,
        meta.textureInfo.mipLevels,
        meta.textureInfo.depth,
        AccessLayoutConsumerTriple{
          .access = AccessPattern::NONE,
          .layout = ResourceLayout::UNDEFINED,
          .consumer = (uint64_t)-1,
        });

    for (const auto &usage : meta.textureUsages)
    {
      intervals.clear();

      textureState.queryAll(
          usage.view.baseMipLevel,
          usage.view.baseArrayLayer,
          usage.view.baseMipLevel + usage.view.levelCount - 1,
          usage.view.baseArrayLayer + usage.view.layerCount - 1,
          intervals);

      auto currentTag = AccessLayoutConsumerTriple{.access = usage.view.access, .layout = usage.view.layout, .consumer = usage.consumer};

      for (const auto &interval : intervals)
      {
        if (interval.tag.consumer == usage.consumer)
        {
          continue;
        }

        if (interval.tag.consumer != -1)
        {
          edges[interval.tag.consumer].emplace_back(
              RenderGraphEdge{
                .type = (interval.tag.access != currentTag.access || interval.tag.layout != currentTag.layout) ? EdgeType::ResourceDependency : EdgeType::ResourceShare,
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

  for (auto [name, meta] : resources.scratchBuffersRequestsMetadatas)
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
  std::vector<lib::BoundedTaggedIntervalTree<AccessPattern, uint64_t>::Interval> intervals;

  uint64_t size = 0;

  for (auto [name, meta] : resources.bufferMetadatas)
  {
    size += meta.bufferUsages.size();
  }

  intervals.reserve(4 * size);

  for (auto [name, meta] : resources.bufferMetadatas)
  {

    lib::BoundedTaggedIntervalTree<AccessPattern, uint64_t> bufferIntevals(meta.bufferUsages.size() * 4);
    if (meta.bufferInfo.size == 0)
    {
      continue;
    }
    bufferIntevals.insert(0, meta.bufferInfo.size - 1, AccessPattern::NONE);

    std::sort(
        meta.bufferUsages.begin(),
        meta.bufferUsages.end(),
        [this](const BufferResourceUsage &usageA, const BufferResourceUsage &usageB)
        {
          return nodes[usageA.consumer].level < nodes[usageB.consumer].level;
        });

    for (const auto &usage : meta.bufferUsages)
    {
      intervals.clear();
      // bufferIntevals.print();
      bufferIntevals.query(usage.view.offset, usage.view.offset + usage.view.size - 1, usage.view.access, intervals);

      for (const auto &interval : intervals)
      {
        if (interval.tag != usage.view.access)
        {
          // printf("interval %u %u\n", interval.start, interval.end);

          bufferIntevals.remove(interval.start, interval.end, interval.tag);
          bufferIntevals.insert(interval.start, interval.end, usage.view.access);

          bufferTransitions.emplace_back(
              BufferBarrier{
                .resourceId = meta.bufferInfo.name,
                .fromAccess = interval.tag,
                .toAccess = usage.view.access,
                .offset = interval.start,
                .size = interval.end - interval.start + 1,
                .toLevel = nodes[usage.consumer].level,
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
    size += meta.textureUsages.size();
  }

  intervals.reserve(4 * size);

  for (auto [name, meta] : resources.textureMetadatas)
  {
    lib::BoundedTaggedRectTreap<AccessLayoutConsumerTriple, uint64_t> textureState(meta.textureUsages.size() * 4);

    textureState.insert(
        0,
        0,
        meta.textureInfo.mipLevels,
        meta.textureInfo.depth,
        AccessLayoutConsumerTriple{
          .access = AccessPattern::NONE,
          .layout = ResourceLayout::UNDEFINED,
        });

    // textureState.print();

    std::sort(
        meta.textureUsages.begin(),
        meta.textureUsages.end(),
        [this](const TextureResourceUsage &usageA, const TextureResourceUsage &usageB)
        {
          return nodes[usageA.consumer].level < nodes[usageB.consumer].level;
        });

    for (const auto &usage : meta.textureUsages)
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
      };
      for (const auto &interval : intervals)
      {
        if (interval.tag != currentTag)
        {
          textureState.remove(interval.x1, interval.y1, interval.x2, interval.y2, interval.tag);
          textureState.insert(interval.x1, interval.y1, interval.x2, interval.y2, currentTag);

          textureTransitions.emplace_back(
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

      if (nodes[fromTask].queue != nodes[toTask].queue)
      {
        semaphoresSet.insert(
            Semaphore{
              .signalQueue = nodes[fromTask].queue,
              .waitQueue = nodes[toTask].queue,
              .signalTask = fromTask,
              .waitTask = toTask,
            });
      }
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

void RenderGraph::compile()
{
  nodes.clear();
  edges.clear();

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

  // lib::time::TimeSpan outputCommandsStart = lib::time::TimeSpan::now();
  // outputCommands(program);
  // lib::time::TimeSpan outputCommandsEnd = lib::time::TimeSpan::now();
  os::Logger::logf("[RenderGraph] analysePasses time = %fms", (analysePassesEnd - analysePassesStart).milliseconds());
  os::Logger::logf("[RenderGraph] analyseDependencyGraph time = %fms", (analyseDependencyGraphEnd - analyseDependencyGraphStart).milliseconds());
  os::Logger::logf("[RenderGraph] analyseTaskLevels time = %fms", (analyseTaskLevelsEnd - analyseTaskLevelsStart).milliseconds());
  os::Logger::logf("[RenderGraph] analyseAllocations time = %fms", (analyseAllocationsEnd - analyseAllocationsStart).milliseconds());
  os::Logger::logf("[RenderGraph] analyseSemaphores time = %fms", (analyseSemaphoresEnd - analyseSemaphoresStart).milliseconds());
  // os::Logger::logf("[TimeScheduler] outputCommands time = %fns", (outputCommandsEnd - outputCommandsStart).nanoseconds());

  compiled = true;
}

RHICommandBuffer::RHICommandBuffer()
{
  recorded.push_back(CommandSequence());
}

void RenderGraph::deleteBuffer(const std::string &name)
{
  if (!resources.bufferMetadatas.remove(name))
  {
    throw std::runtime_error("Buffer not found");
  }
}

void RenderGraph::deleteTexture(const std::string &name)
{
  if (!resources.textureMetadatas.remove(name))
  {
    throw std::runtime_error("Buffer not found");
  }
}

void RenderGraph::deleteSampler(const std::string &name)
{
  if (!resources.samplerMetadatas.remove(name))
  {
    throw std::runtime_error("Buffer not found");
  }
}

void RenderGraph::deleteBindingsLayout(const std::string &name)
{
  if (!resources.bindingsLayoutMetadata.remove(name))
  {
    throw std::runtime_error("Buffer not found");
  }
}

void RenderGraph::deleteBindingGroups(const std::string &name)
{
  if (!resources.bindingGroupsMetadata.remove(name))
  {
    throw std::runtime_error("Buffer not found");
  }
}

void RenderGraph::deleteGraphicsPipeline(const std::string &name)
{
  if (!resources.graphicsPipelineMetadata.remove(name))
  {
    throw std::runtime_error("Buffer not found");
  }
}

void RenderGraph::deleteComputePipeline(const std::string &name)
{
  if (!resources.computePipelineMetadata.remove(name))
  {
    throw std::runtime_error("Buffer not found");
  }
}

const Buffer RenderGraph::createBuffer(const BufferInfo &info)
{
  const auto &name = info.name;

  resources.bufferMetadatas.insert(
      name,
      BufferResourceMetadata{
        .bufferInfo = info,
      });

  return Buffer{
    .name = info.name,
  };
}

const Texture RenderGraph::createTexture(const TextureInfo &info)
{
  const auto &name = info.name;

  resources.textureMetadatas.insert(
      name,
      TextureResourceMetadata{
        .textureInfo = info,
      });

  return Texture{
    .name = info.name,
  };
}

const Sampler RenderGraph::createSampler(const SamplerInfo &info)
{
  const auto &name = info.name;

  resources.samplerMetadatas.insert(
      name,
      SamplerResourceMetadata{
        .samplerInfo = info,
      });

  return Sampler{
    .name = info.name,
  };
}

const BindingGroups RenderGraph::createBindingGroups(const BindingGroupsInfo &info)
{
  const auto &name = info.name;

  resources.bindingGroupsMetadata.insert(
      name,
      BindingGroupsResourceMetadata{
        .groupsInfo = info,
      });

  return BindingGroups{
    .name = info.name,
  };
}

const GraphicsPipeline RenderGraph::createGraphicsPipeline(const GraphicsPipelineInfo &info)
{
  const auto &name = info.name;

  resources.graphicsPipelineMetadata.insert(
      name,
      GraphicsPipelineResourceMetadata{
        .pipelineInfo = info,
      });

  return GraphicsPipeline{
    .name = info.name,
  };
}

const ComputePipeline RenderGraph::createComputePipeline(const ComputePipelineInfo &info)
{
  const auto &name = info.name;

  resources.computePipelineMetadata.insert(
      name,
      ComputePipelineResourceMetadata{
        .pipelineInfo = info,
      });

  return ComputePipeline{
    .name = info.name,
  };
}

const BindingsLayout RenderGraph::createBindingsLayout(const BindingsLayoutInfo &info)
{
  const auto &name = info.name;

  resources.bindingsLayoutMetadata.insert(
      name,
      BindingsLayoutResourceMetadata{
        .layoutsInfo = info,
      });

  return BindingsLayout{
    .name = info.name,
  };
}

const Buffer RenderGraph::createScratchBuffer(const BufferInfo &info)
{
  const auto &name = info.name;

  resources.scratchBuffersRequestsMetadatas.insert(
      name,
      ScratchBufferResourceMetadata{
        .bufferInfo = info,
      });

  return Buffer{
    .name = info.name,
  };
}

const Buffer RHIResources::getScratchBuffer(const std::string &name)
{
  if (!scratchMap.contains(name))
  {
    throw std::runtime_error("Buffer not found");
  }

  return Buffer{
    .name = name,
  };
}

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
    throw std::runtime_error("Texture not found");
  }

  return Texture{
    .name = name,
  };
}

void RHICommandBuffer::cmdBeginRenderPass(const RenderPassInfo &info)
{
  Command cmd{CommandType::BeginRenderPass};
  cmd.args.renderPassInfo = new RenderPassInfo();
  *cmd.args.renderPassInfo = info;
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
  recorded.push_back(CommandSequence());
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
  recorded.push_back(CommandSequence());
}

void RHICommandBuffer::cmdDrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, uint32_t vertexOffset, uint32_t firstInstance)
{
  Command cmd{CommandType::DrawIndexed};
  cmd.args.drawIndexed = new DrawIndexedArgs();
  *cmd.args.drawIndexed = {indexCount, instanceCount, firstIndex, firstInstance, vertexOffset};
  recorded.back().commands.push_back(std::move(cmd));
  recorded.push_back(CommandSequence());
}

void RHICommandBuffer::cmdDrawIndexedIndirect(BufferView buffer, uint32_t offset, uint32_t drawCount, uint32_t stride)
{
  Command cmd{CommandType::DrawIndexedIndirect};
  cmd.args.drawIndexedIndirect = new DrawIndexedIndirectArgs();
  *cmd.args.drawIndexedIndirect = {buffer, offset, drawCount, stride};
  recorded.back().commands.push_back(std::move(cmd));
  recorded.push_back(CommandSequence());
}

void RHICommandBuffer::cmdDispatch(uint32_t x, uint32_t y, uint32_t z)
{
  Command cmd{CommandType::Dispatch};
  cmd.args.dispatch = new DispatchArgs();
  *cmd.args.dispatch = {x, y, z};
  recorded.back().commands.push_back(std::move(cmd));
  recorded.push_back(CommandSequence());
}

} // namespace rendering
