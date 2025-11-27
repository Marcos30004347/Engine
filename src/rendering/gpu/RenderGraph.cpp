#include "RenderGraph.hpp"
#include "os/Logger.hpp"
#include <algorithm>
#include <map>
#include <sstream>
#include <unordered_map>

#include "datastructure/BoundedTaggedRectTreap.hpp"
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
  uint64_t bufferId;
  size_t offset;
  size_t size;
};

void RenderGraph::registerConsumer(const std::string &name, const InputResource &res, uint32_t taskId)
{
  // if (meta != resourcesMetadatas.end() && meta->second.type != res.type)
  // {
  //   os::Logger::errorf("[RenderGraph] %s already used with a different type", name.c_str());
  //   exit(1);
  // }

  switch (res.type)
  {
  case ResourceType::ResourceType_BufferView:
  {

    auto id = resources.bufferSymbols.find(name);

    if (id == resources.bufferSymbols.end())
    {
      throw std::runtime_error("Buffer not found");
    }

    resources.bufferMetadatas[id.value()].id = id.value();
    resources.bufferMetadatas[id.value()].bufferUsages.push_back(
        BufferResourceUsage{
          .view = res.bufferView,
          .consumer = taskId,
        });
  }
  break;
  case ResourceType::ResourceType_TextureView:
  {
    auto id = resources.textureSymbols.find(name);

    if (id == resources.textureSymbols.end())
    {
      throw std::runtime_error("Texture not found");
    }

    resources.textureMetadatas[id.value()].id = id.value();
    resources.textureMetadatas[id.value()].textureUsages.push_back(
        TextureResourceUsage{
          .view = res.textureView,
          .consumer = taskId,
        });
  }
  break;
  case ResourceType::ResourceType_Sampler:
  {
    auto id = resources.samplerSymbols.find(name);

    if (id == resources.samplerSymbols.end())
    {
      throw std::runtime_error("Sampler not found");
    }

    resources.samplerMetadatas[id.value()].id = id.value();
    resources.samplerMetadatas[id.value()].samplerUsages.push_back(
        SamplerResourceUsage{
          .sampler = res.sampler,
          .consumer = taskId,
        });
  }
  break;
  case ResourceType::ResourceType_BindingsLayout:
  {
    auto id = resources.bindingLayoutSymbols.find(name);

    if (id == resources.bindingLayoutSymbols.end())
    {
      throw std::runtime_error("BindingsLayout not found");
    }

    resources.bindingsLayoutMetadata[id.value()].id = id.value();
    resources.bindingsLayoutMetadata[id.value()].layoutUsages.push_back(
        BindingsLayoutResourceUsage{
          .consumer = taskId,
        });
  }
  break;
  case ResourceType::ResourceType_BindingGroups:
  {
    auto id = resources.bindingGroupsSymbols.find(name);

    if (id == resources.bindingGroupsSymbols.end())
    {
      throw std::runtime_error("BindingGroups not found");
    }

    resources.bindingGroupsMetadata[id.value()].id = id.value();
    resources.bindingGroupsMetadata[id.value()].layoutUsages.push_back(
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

// void RenderGraph::registerProducer(const OutputResource &res, uint32_t taskId)
// {
//   std::string name = "";

//   BufferInfo bufferInfo;
//   TextureInfo textureInfo;
//   SamplerInfo samplerInfo;
//   BindingsLayoutInfo layoutInfo;

//   switch (res.type)
//   {
//   case ResourceType::ResourceType_Buffer:
//   {
//     name = res.bufferInfo.name;

//     bufferInfo = res.bufferInfo;

//     auto it = resources.bufferSymbols.find(name);
//     if (it == resources.bufferSymbols.end())
//     {
//       resources.bufferSymbols[name] = resources.bufferMetadatas.size();
//       resources.bufferMetadatas.push_back({});
//       it = resources.bufferSymbols.find(name);
//     }
//     uint64_t id = it->second;

//     resources.bufferMetadatas[id].id = id;
//     resources.bufferMetadatas[id].producer = taskId;
//     resources.bufferMetadatas[id].bufferInfo = bufferInfo;
//     resources.bufferMetadatas[id].initialAccess = res.access;
//   }
//   break;
//   case ResourceType::ResourceType_Texture:
//   {
//     name = res.textureInfo.name;
//     textureInfo = res.textureInfo;

//     auto it = resources.textureSymbols.find(name);
//     if (it == resources.textureSymbols.end())
//     {
//       resources.textureSymbols[name] = resources.textureMetadatas.size();
//       resources.textureMetadatas.push_back({});
//       it = resources.textureSymbols.find(name);
//     }
//     uint64_t id = it->second;

//     resources.textureMetadatas[id].id = id;
//     resources.textureMetadatas[id].producer = taskId;
//     resources.textureMetadatas[id].textureInfo = textureInfo;
//     resources.textureMetadatas[id].initialLayout = res.layout;
//     resources.textureMetadatas[id].initialAccess = res.access;
//   }
//   break;
//   case ResourceType::ResourceType_Sampler:
//   {
//     name = res.samplerInfo.name;
//     samplerInfo = res.samplerInfo;

//     auto it = resources.samplerSymbols.find(name);
//     if (it == resources.samplerSymbols.end())
//     {
//       resources.samplerSymbols[name] = resources.samplerMetadatas.size();
//       resources.samplerMetadatas.push_back({});
//       it = resources.samplerSymbols.find(name);
//     }
//     uint64_t id = it->second;

//     resources.samplerMetadatas[id].id = id;
//     resources.samplerMetadatas[id].producer = taskId;
//     resources.samplerMetadatas[id].samplerInfo = samplerInfo;
//   }
//   break;
//   case ResourceType::ResourceType_BindingsLayout:
//   {
//     name = res.bindingsLayoutsInfo.name;
//     layoutInfo = res.bindingsLayoutsInfo;

//     auto it = resources.bindingLayoutSymbols.find(name);
//     if (it == resources.bindingLayoutSymbols.end())
//     {
//       resources.bindingLayoutSymbols[name] = resources.bindingsLayoutMetadata.size();
//       resources.bindingsLayoutMetadata.push_back({});
//       it = resources.bindingLayoutSymbols.find(name);
//     }
//     uint64_t id = it->second;

//     resources.bindingsLayoutMetadata[id].id = id;
//     resources.bindingsLayoutMetadata[id].producer = taskId;
//     resources.bindingsLayoutMetadata[id].layoutsInfo = layoutInfo;
//   }
//   break;
//   default:
//     os::Logger::error("Unknown resource type");
//     exit(1);
//     break;
//   }
// }

ResourceDatabase::ResourceDatabase(RenderGraph *renderGraph)
    : renderGraph(renderGraph), scratchBuffersAllocated(0), buffersAllocated(0), texturesAllocated(0), samplersAllocated(0), bindingLayoutsAllocated(0), bindingGroupsAllocated(0),
      graphicsPipelinesAllocated(0), computePipelinesAllocated(0)
{
}

RenderGraph::RenderGraph(RHI *renderingHardwareInterface) : renderingHardwareInterface(renderingHardwareInterface), resources(this)
{
  // currentTaskId.set(-1);
  compiled = false;

  addPass(
      "Initialization",
      RenderGraph::ExecuteOnFirstRun,
      [](const ResourceDatabase &rg, CommandRecorder &recorder)
      {
      });
}

void RenderGraph::analyseCommands(CommandRecorder &recorder)
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

void RenderGraph::addPass(std::string name, std::function<bool(const RenderGraph &)> shouldExecute, std::function<void(ResourceDatabase &, CommandRecorder &)> record)
{
  passes.insert(
      name,
      RenderGraphPass{
        .name = name,
        .record = record,
        .shouldExecute = shouldExecute,
      });
}

void RenderGraph::analysePasses()
{
  CommandRecorder **ref;

  for (auto [name, pass] : passes)
  {
    CommandRecorder recorder;

    if (!currentRecorder.get(ref))
    {
      ref = (CommandRecorder **)(malloc(sizeof(CommandRecorder *)));
      currentRecorder.set(ref);
    }

    *ref = &recorder;

    pass.record(resources, recorder);

    analyseCommands(recorder);

    uint32_t index = 0;

    for (auto &commands : recorder.recorded)
    {
      uint32_t id = nodes.size();

      if (commands.commands.size() == 0 && id != 0)
      {
        uint64_t allocations = 0;

        // allocations += commands.buffersAllocated.size();
        // allocations += commands.texturesAllocated.size();
        // allocations += commands.samplersAllocated.size();
        // allocations += commands.bindingLayoutsAllocated.size();

        if (allocations == 0)
        {
          continue;
        }
      }

      nodes.push_back(RenderGraphNode());

      nodes[id].name = pass.name + "[" + std::to_string(index++) + "]";
      nodes[id].id = id;
      nodes[id].level = 0;
      nodes[id].priority = id;
      nodes[id].commands = commands.commands;
      nodes[id].queue = inferQueue(commands.commands);

      // for (const auto &resourceId : commands.buffersAllocated)
      // {
      //   resources.bufferMetadatas[resourceId].bufferUsages.push_back(
      //       BufferResourceUsage{
      //         .consumer = id,
      //         .view = {
      //           .access = AccessPattern::NONE,
      //           .buffer =
      //               {
      //                 .name = resources.bufferMetadatas[resourceId].bufferInfo.name,
      //               },
      //           .offset = 0,
      //           .size = resources.bufferMetadatas[resourceId].bufferInfo.size,
      //         }});
      // }
      // for (const auto &resourceId : commands.texturesAllocated)
      // {
      //   resources.textureMetadatas[resourceId].textureUsages.push_back(
      //       TextureResourceUsage{
      //         .consumer = id,
      //         .view = {
      //           .access = AccessPattern::NONE,
      //           .layout = ResourceLayout::UNDEFINED,
      //           .texture =
      //               {
      //                 .name = resources.textureMetadatas[resourceId].textureInfo.name,
      //               },
      //           .baseArrayLayer = 0,
      //           .baseMipLevel = 0,
      //           .levelCount = resources.textureMetadatas[resourceId].textureInfo.mipLevels,
      //           .layerCount = resources.textureMetadatas[resourceId].textureInfo.depth,
      //         }});
      // }
      // for (const auto &resourceId : commands.samplersAllocated)
      // {
      //   resources.samplerMetadatas[resourceId].samplerUsages.push_back(
      //       SamplerResourceUsage{
      //         .consumer = id,
      //         .sampler =
      //             {
      //               .name = resources.samplerMetadatas[resourceId].samplerInfo.name,
      //             },
      //       });
      // }
      // for (const auto &resourceId : commands.bindingLayoutsAllocated)
      // {
      //   resources.bindingsLayoutMetadata[resourceId].layoutUsages.push_back(
      //       BindingsLayoutResourceUsage{
      //         .consumer = id,
      //       });
      // }
      auto symbolId = resources.bindingGroupsSymbols.end();

      for (auto &cmd : commands.commands)
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
          for (auto &attatchment : cmd.args.renderPassInfo->depthStencilAttachment)
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
          symbolId = resources.bindingGroupsSymbols.find(cmd.args.bindGroups->groups.name);
          if (symbolId == resources.bindingGroupsSymbols.end())
          {
            throw std::runtime_error("Bunding Groups not found");
          }
          registerConsumer(
              resources.bindingGroupsMetadata[symbolId].groupsInfo.layout.name,
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

          for (auto &group : resources.bindingGroupsMetadata[symbolId].groupsInfo.groups)
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

inline uint64_t getIdOrThrow(lib::ConcurrentHashMap<std::string, uint64_t> &map, const std::string &name, const char *errorMsgPrefix)
{
  auto id = map.find(name);

  if (id == map.end())
  {
    throw std::runtime_error(std::string(errorMsgPrefix) + ": symbol '" + name + "' not found");
  }

  return id;
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
        if (resources.scratchBufferSymbols.find(cmd.args.copyBuffer->src.buffer.name) != resources.scratchBufferSymbols.end())
        {
          uint64_t srcId = getIdOrThrow(resources.scratchBufferSymbols, cmd.args.copyBuffer->src.buffer.name, "Scratch buffer not found");
          auto &srcMeta = resources.scratchBuffersRequestsMetadatas[srcId];
          srcMeta.firstUsedAt = std::min(srcMeta.firstUsedAt, node.level);
          srcMeta.lastUsedAt = std::max(srcMeta.lastUsedAt, node.level);
        }

        if (resources.scratchBufferSymbols.find(cmd.args.copyBuffer->dst.buffer.name) != resources.scratchBufferSymbols.end())
        {
          uint64_t dstId = getIdOrThrow(resources.scratchBufferSymbols, cmd.args.copyBuffer->dst.buffer.name, "Scratch buffer not found");
          auto &dstMeta = resources.scratchBuffersRequestsMetadatas[dstId];
          dstMeta.firstUsedAt = std::min(dstMeta.firstUsedAt, node.level);
          dstMeta.lastUsedAt = std::max(dstMeta.lastUsedAt, node.level);
        }
        break;
      }

      case BindBindingGroups:
      {
        uint64_t groupSymbolId = getIdOrThrow(resources.bindingGroupsSymbols, cmd.args.bindGroups->groups.name, "Binding group symbol not found");

        const auto &groupsMeta = resources.bindingGroupsMetadata[groupSymbolId].groupsInfo.groups;

        for (const auto &group : groupsMeta)
        {
          for (const auto &buffer : group.buffers)
          {
            if (resources.scratchBufferSymbols.find(buffer.bufferView.buffer.name) != resources.scratchBufferSymbols.end())
            {
              uint64_t bufId = getIdOrThrow(resources.scratchBufferSymbols, buffer.bufferView.buffer.name, "Scratch buffer not found");
              auto &meta = resources.scratchBuffersRequestsMetadatas[bufId];
              meta.firstUsedAt = std::min(meta.firstUsedAt, node.level);
              meta.lastUsedAt = std::max(meta.lastUsedAt, node.level);
            }
          }
        }
        break;
      }

      case BindVertexBuffer:
      {
        if (resources.scratchBufferSymbols.find(cmd.args.bindVertexBuffer->buffer.buffer.name) != resources.scratchBufferSymbols.end())
        {
          uint64_t bufId = getIdOrThrow(resources.scratchBufferSymbols, cmd.args.bindVertexBuffer->buffer.buffer.name, "Scratch buffer not found");
          auto &meta = resources.scratchBuffersRequestsMetadatas[bufId];
          meta.firstUsedAt = std::min(meta.firstUsedAt, node.level);
          meta.lastUsedAt = std::max(meta.lastUsedAt, node.level);
        }
        break;
      }

      case BindIndexBuffer:
      {
        if (resources.scratchBufferSymbols.find(cmd.args.bindIndexBuffer->buffer.buffer.name) != resources.scratchBufferSymbols.end())
        {
          uint64_t bufId = getIdOrThrow(resources.scratchBufferSymbols, cmd.args.bindIndexBuffer->buffer.buffer.name, "Scratch buffer not found");
          auto &meta = resources.scratchBuffersRequestsMetadatas[bufId];
          meta.firstUsedAt = std::min(meta.firstUsedAt, node.level);
          meta.lastUsedAt = std::max(meta.lastUsedAt, node.level);
        }
        break;
      }

      case DrawIndexedIndirect:
      {
        if (resources.scratchBufferSymbols.find(cmd.args.drawIndexedIndirect->buffer.buffer.name) != resources.scratchBufferSymbols.end())
        {
          uint64_t bufId = getIdOrThrow(resources.scratchBufferSymbols, cmd.args.drawIndexedIndirect->buffer.buffer.name, "Scratch buffer not found");
          auto &meta = resources.scratchBuffersRequestsMetadatas[bufId];
          meta.firstUsedAt = std::min(meta.firstUsedAt, node.level);
          meta.lastUsedAt = std::max(meta.lastUsedAt, node.level);
        }
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
          .resourceId = (uint64_t)-1,
          .resourceType = ResourceType::ResourceType_Initialization,
          .type = EdgeType::Initialization,
        });
  }

  for (auto &meta : resources.bufferMetadatas)
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
                .resourceId = meta.id,
                .resourceType = ResourceType::ResourceType_BufferView,
              });
        }

        bufferIntevals.remove(interval.start, interval.end, interval.tag);
        bufferIntevals.insert(interval.start, interval.end, AccessConsumerPair{.access = usage.view.access, .consumer = usage.consumer});
      }
    }
  }

  for (auto &meta : resources.textureMetadatas)
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
                .resourceId = meta.id,
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
  uint64_t id;
  uint64_t size;
  uint64_t start;
  uint64_t end;
};

inline size_t alignUp(size_t value, size_t alignment)
{
  return (value + alignment - 1) & ~(alignment - 1);
}

std::pair<std::vector<BufferSlice>, size_t> allocateBuffersGraphColoring(std::vector<Request> &requests, size_t alignment)
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
  std::vector<BufferSlice> allocations;

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

  for (auto &meta : resources.scratchBuffersRequestsMetadatas)
  {
    const BufferInfo &info = meta.bufferInfo;

    memoryRequests[info.usage].push_back(
        Request{
          .id = meta.id,
          .start = meta.firstUsedAt,
          .end = meta.lastUsedAt,
          .size = info.size,
        });
  }

  resources.scratchMap = std::vector<BufferAllocation>(resources.scratchBuffersRequestsMetadatas.size());

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
      .id = resources.scratchBuffers.size(),
    };

    resources.scratchBuffers.insert(usage, metadata);

    os::Logger::logf("[RenderGraph] Reserving %u bytes for %s", info.size, info.name.c_str());

    for (auto &allocation : allocations)
    {
      resources.scratchMap[allocation.bufferId].usage = usage;
      resources.scratchMap[allocation.bufferId].offset = allocation.offset;
      resources.scratchMap[allocation.bufferId].size = allocation.size;

      os::Logger::logf(
          "[RenderGraph] Reserving slice of %s, offset = %u, size = %u, for %s",
          info.name.c_str(),
          allocation.offset,
          allocation.size,
          resources.bufferMetadatas[allocation.bufferId].bufferInfo.name.c_str());
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

  for (auto &meta : resources.bufferMetadatas)
  {
    size += meta.bufferUsages.size();
  }

  intervals.reserve(4 * size);

  for (auto &meta : resources.bufferMetadatas)
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
                .resourceId = meta.id,
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

  for (auto &meta : resources.textureMetadatas)
  {
    size += meta.textureUsages.size();
  }

  intervals.reserve(4 * size);

  for (auto &meta : resources.textureMetadatas)
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
                .resourceId = meta.id,
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

// void RenderGraph::outputCommands(RHIProgram &RHIProgram)
// {
//   RHIProgram.buffers.reserve(bufferAllocations.size());
//   RHIProgram.textures.reserve(textureAllocations.size());
//   RHIProgram.samplers.reserve(samplerAllocations.size());
//   RHIProgram.bindingLayouts.reserve(bindingsLayoutsAllocations.size());
//   RHIProgram.semaphores.reserve(semaphores.size());
//   RHIProgram.commands.reserve(nodes.size() * 5);
//   RHIProgram.dispatches.reserve(nodes.size());

//   for (const auto &buffer : bufferAllocations)
//   {
//     RHIProgram.buffers.emplace_back(RHIBufferInfo{.name = buffer.info.name, .persistent = buffer.info.persistent, .size = buffer.info.size, .usage = buffer.info.usage});
//   }

//   for (const auto &texture : textureAllocations)
//   {
//     RHIProgram.textures.emplace_back(
//         RHITextureInfo{
//           .name = texture.info.name,
//           .depth = texture.info.depth,
//           .format = texture.info.format,
//           .height = texture.info.height,
//           .memoryProperties = texture.info.memoryProperties,
//           .mipLevels = texture.info.mipLevels,
//           .usage = texture.info.usage,
//           .width = texture.info.width});
//   }

//   for (const auto &sampler : samplerAllocations)
//   {
//     RHIProgram.samplers.emplace_back(
//         RHISamplerInfo{
//           .name = sampler.info.name,
//           .addressModeU = sampler.info.addressModeU,
//           .addressModeV = sampler.info.addressModeV,
//           .addressModeW = sampler.info.addressModeW,
//           .anisotropyEnable = sampler.info.anisotropyEnable,
//           .magFilter = sampler.info.magFilter,
//           .maxAnisotropy = sampler.info.maxAnisotropy,
//           .maxLod = sampler.info.maxLod,
//           .minFilter = sampler.info.minFilter});
//   }

//   for (const auto &binding : bindingsLayoutsAllocations)
//   {
//     RHIProgram.bindingLayouts.emplace_back();
//     RHIBindingsLayoutInfo &rhiBindingLayoutInfo = RHIProgram.bindingLayouts.back();
//     rhiBindingLayoutInfo.name = binding.info.name;
//     rhiBindingLayoutInfo.groups.reserve(binding.info.groups.size());

//     for (const auto &group : binding.info.groups)
//     {
//       rhiBindingLayoutInfo.groups.emplace_back();
//       RHIBindingGroupLayout &rhiGroupLayout = rhiBindingLayoutInfo.groups.back();

//       rhiGroupLayout.buffers.reserve(group.buffers.size());
//       rhiGroupLayout.samplers.reserve(group.samplers.size());
//       rhiGroupLayout.textures.reserve(group.textures.size());
//       rhiGroupLayout.storageTextures.reserve(group.storageTextures.size());

//       for (const auto &buffer : group.buffers)
//       {
//         rhiGroupLayout.buffers.emplace_back(
//             RHIBindingGroupLayoutBufferEntry{.binding = buffer.binding, .isDynamic = buffer.isDynamic, .name = buffer.name, .visibility = buffer.visibility});
//       }

//       for (const auto &sampler : group.samplers)
//       {
//         rhiGroupLayout.samplers.emplace_back(RHIBindingGroupLayoutSamplerEntry{.binding = sampler.binding, .name = sampler.name, .visibility = sampler.visibility});
//       }

//       for (const auto &texture : group.textures)
//       {
//         rhiGroupLayout.textures.emplace_back(
//             RHIBindingGroupLayoutTextureEntry{.binding = texture.binding, .name = texture.name, .multisampled = texture.multisampled, .visibility = texture.visibility});
//       }

//       for (const auto &storageTexture : group.storageTextures)
//       {
//         rhiGroupLayout.storageTextures.emplace_back(
//             RHIBindingGroupLayoutStorageTextureEntry{.binding = storageTexture.binding, .name = storageTexture.name, .visibility = storageTexture.visibility});
//       }
//     }
//   }

//   std::sort(
//       nodes.begin(),
//       nodes.end(),
//       [](const RenderGraphNode &A, const RenderGraphNode &B)
//       {
//         return A.level < B.level;
//       });

//   uint32_t maxLevel = 0;
//   for (const auto &task : nodes)
//   {
//     maxLevel = std::max(maxLevel, task.level);
//   }

//   std::vector<std::vector<BufferBarrier>> bufferBarriersPerLevel(maxLevel);
//   std::vector<std::vector<TextureBarrier>> textureBarriersPerLevel(maxLevel);

//   for (auto &&barrier : bufferTransitions)
//   {
//     if (barrier.toLevel > 0)
//       bufferBarriersPerLevel[barrier.toLevel - 1].push_back(std::move(barrier));
//   }

//   for (auto &&barrier : textureTransitions)
//   {
//     if (barrier.toLevel > 0)
//       textureBarriersPerLevel[barrier.toLevel - 1].push_back(std::move(barrier));
//   }

//   for (const auto &semaphore : semaphores)
//   {
//     RHIProgram.semaphores.emplace_back(
//         RHISemaphore{.signalQueue = semaphore.signalQueue, .signalTask = semaphore.signalTask, .waitQueue = semaphore.waitQueue, .waitTask = semaphore.waitTask});
//   }

//   uint32_t currentLevel = 0;

//   for (const RenderGraphNode &data : nodes)
//   {
//     RHIProgram::ParallelDispatch dispatch;
//     dispatch.level = data.level;
//     dispatch.start = RHIProgram.commands.size();

//     if (data.level != currentLevel)
//     {
//       currentLevel = data.level;

//       if (currentLevel > 0 && currentLevel <= maxLevel)
//       {
//         for (auto &&barrier : bufferBarriersPerLevel[currentLevel - 1])
//         {
//           RHIBufferBarrier *rhiBarrier = new RHIBufferBarrier{
//             .buffer = barrier.resourceId,
//             .offset = resources.bufferResourceAllocationView[barrier.resourceId].offset + barrier.offset,
//             .size = barrier.size,
//             .fromAccess = barrier.fromAccess,
//             .toAccess = barrier.toAccess,
//             .toLevel = barrier.toLevel};

//           RHIProgram.commands.emplace_back(RHIInstruction{.type = CmdBufferBarrier, .args = {.bufferBarrier = rhiBarrier}});
//         }

//         for (auto &&barrier : textureBarriersPerLevel[currentLevel - 1])
//         {
//           RHITextureBarrier *rhiBarrier = new RHITextureBarrier{
//             .texture = barrier.resourceId,
//             .baseLayer = barrier.baseLayer,
//             .baseMip = barrier.baseMip,
//             .layerCount = barrier.layerCount,
//             .mipCount = barrier.mipCount,
//             .fromAccess = barrier.fromAccess,
//             .toAccess = barrier.toAccess,
//             .fromLayout = barrier.fromLayout,
//             .toLayout = barrier.toLayout,
//             .toLevel = barrier.toLevel};

//           RHIProgram.commands.emplace_back(RHIInstruction{.type = CmdTextureBarrier, .args = {.textureBarrier = rhiBarrier}});
//         }
//       }
//     }

//     for (const Command &cmd : data.commands)
//     {
//       switch (cmd.type)
//       {
//       case BeginRenderPass:
//       {
//         auto dst = new RHIInstructionRenderPassInfo();
//         const auto &src = cmd.args.renderPassInfo;

//         dst->name = src->name;
//         dst->viewport = src->viewport;
//         dst->scissor = src->scissor;

//         dst->colorAttachments.reserve(src->colorAttachments.size());
//         dst->depthStencilAttachment.reserve(src->depthStencilAttachment.size());

//         for (const auto &ca : src->colorAttachments)
//         {
//           RHITextureView tview{
//             .textureId = resources.textureSymbols[ca.view.texture.name],
//             .baseArrayLayer = ca.view.baseArrayLayer,
//             .baseMipLevel = ca.view.baseMipLevel,
//             .layerCount = ca.view.layerCount,
//             .levelCount = ca.view.levelCount,
//             .flags = ca.view.flags};

//           dst->colorAttachments.emplace_back(RHIColorAttachmentInfo{.name = ca.name, .view = tview, .clearValue = ca.clearValue});
//         }

//         for (const auto &da : src->depthStencilAttachment)
//         {
//           RHITextureView tview{
//             .textureId = resources.textureSymbols[da.view.texture.name],
//             .baseArrayLayer = da.view.baseArrayLayer,
//             .baseMipLevel = da.view.baseMipLevel,
//             .layerCount = da.view.layerCount,
//             .levelCount = da.view.levelCount,
//             .flags = da.view.flags};

//           dst->depthStencilAttachment.emplace_back(RHIDepthStencilAttachmentInfo{.name = da.name, .view = tview, .clearDepth = da.clearDepth, .clearStencil = da.clearStencil});
//         }

//         RHIProgram.commands.emplace_back(RHIInstruction{.type = CmdBeginRenderPass, .args = {.renderPassInfo = dst}});
//         break;
//       }

//       case EndRenderPass:
//       {
//         RHIProgram.commands.emplace_back(RHIInstruction{.type = CmdEndRenderPass, .args = {}});
//         break;
//       }

//       case CopyBuffer:
//       {
//         uint64_t srcBufferId = resources.bufferSymbols[cmd.args.copyBuffer->src.buffer.name];
//         uint64_t dstBufferId = resources.bufferSymbols[cmd.args.copyBuffer->dst.buffer.name];

//         auto dst = new RHIInstructionCopyBufferArguments{
//           .src = srcBufferId,
//           .srcOffset = resources.bufferResourceAllocationView[srcBufferId].offset + cmd.args.copyBuffer->src.offset,
//           .srcSize = cmd.args.copyBuffer->src.size,
//           .dst = dstBufferId,
//           .dstOffset = resources.bufferResourceAllocationView[dstBufferId].offset + cmd.args.copyBuffer->dst.offset,
//           .dstSize = cmd.args.copyBuffer->dst.size};

//         RHIProgram.commands.emplace_back(RHIInstruction{.type = CmdCopyBuffer, .args = {.copyBuffer = dst}});
//         break;
//       }

//       case BindBindingGroups:
//       {
//         auto dst = new RHIInstructionBindGroupsArgs();
//         const auto &src = cmd.args.bindGroups;

//         dst->groups.name = src->groups.name;
//         dst->groups.layout = resources.bindingLayoutSymbols[src->groups.layout.name];
//         dst->groups.groups.reserve(src->groups.groups.size());

//         for (const auto &groupInfo : src->groups.groups)
//         {
//           dst->groups.groups.emplace_back();
//           RHIBindingGroupInfo &dstGroup = dst->groups.groups.back();

//           dstGroup.buffers.reserve(groupInfo.buffers.size());
//           dstGroup.samplers.reserve(groupInfo.samplers.size());
//           dstGroup.textures.reserve(groupInfo.textures.size());
//           dstGroup.storageTextures.reserve(groupInfo.storageTextures.size());

//           for (const auto &b : groupInfo.buffers)
//           {
//             uint32_t bufferId = resources.bufferSymbols[b.bufferView.buffer.name];
//             dstGroup.buffers.emplace_back(
//                 RHIBindingBuffer{
//                   .binding = b.binding,
//                   .bufferIndex = bufferId,
//                   .offset = resources.bufferResourceAllocationView[bufferId].offset + b.bufferView.offset,
//                   .size = b.bufferView.size,
//                   .isDynamic = b.isDynamic});
//           }

//           for (const auto &s : groupInfo.samplers)
//           {
//             RHIProgram.textureViews.emplace_back(
//                 RHITextureView{
//                   .textureId = resources.textureSymbols[s.view.texture.name],
//                   .baseArrayLayer = s.view.baseArrayLayer,
//                   .baseMipLevel = s.view.baseMipLevel,
//                   .layerCount = s.view.layerCount,
//                   .levelCount = s.view.levelCount,
//                   .flags = s.view.flags});
//             uint32_t viewIndex = RHIProgram.textureViews.size() - 1;

//             dstGroup.samplers.emplace_back(RHIBindingSampler{.binding = s.binding, .sampler = resources.samplerSymbols[s.sampler.name], .textureView = viewIndex, .layout =
//             s.view.layout});
//           }

//           for (const auto &t : groupInfo.textures)
//           {
//             RHIProgram.textureViews.emplace_back(
//                 RHITextureView{
//                   .textureId = resources.textureSymbols[t.textureView.texture.name],
//                   .baseArrayLayer = t.textureView.baseArrayLayer,
//                   .baseMipLevel = t.textureView.baseMipLevel,
//                   .layerCount = t.textureView.layerCount,
//                   .levelCount = t.textureView.levelCount,
//                   .flags = t.textureView.flags});
//             uint32_t viewIndex = RHIProgram.textureViews.size() - 1;

//             dstGroup.textures.emplace_back(RHIBindingTextureInfo{.binding = t.binding, .textureView = viewIndex, .layout = t.textureView.layout});
//           }

//           for (const auto &st : groupInfo.storageTextures)
//           {
//             RHIProgram.textureViews.emplace_back(
//                 RHITextureView{
//                   .textureId = resources.textureSymbols[st.textureView.texture.name],
//                   .baseArrayLayer = st.textureView.baseArrayLayer,
//                   .baseMipLevel = st.textureView.baseMipLevel,
//                   .layerCount = st.textureView.layerCount,
//                   .levelCount = st.textureView.levelCount,
//                   .flags = st.textureView.flags});
//             uint32_t viewIndex = RHIProgram.textureViews.size() - 1;

//             dstGroup.storageTextures.emplace_back(RHIBindingStorageTextureInfo{.binding = st.binding, .textureView = viewIndex, .layout = st.textureView.layout});
//           }
//         }

//         dst->dynamicOffsets = src->dynamicOffsets;
//         RHIProgram.commands.emplace_back(RHIInstruction{.type = CmdBindBindingGroups, .args = {.bindGroups = dst}});
//         break;
//       }

//       case BindGraphicsPipeline:
//       {
//         auto dst = new RHIGraphicsPipelineInfo();
//         const auto &src = cmd.args.graphicsPipeline;

//         dst->layout = resources.bindingLayoutSymbols[src->layout.name];

//         dst->vertexStage.cullType = src->vertexStage.cullType;
//         dst->vertexStage.primitiveType = src->vertexStage.primitiveType;
//         dst->vertexStage.shaderEntry = src->vertexStage.shaderEntry;
//         dst->vertexStage.vertexShader = src->vertexStage.vertexShader;
//         dst->vertexStage.vertexLayoutElements.reserve(src->vertexStage.vertexLayoutElements.size());

//         for (const auto &element : src->vertexStage.vertexLayoutElements)
//         {
//           dst->vertexStage.vertexLayoutElements.emplace_back(
//               RHIVertexLayoutElement{.binding = element.binding, .location = element.location, .name = element.name, .type = element.type});
//         }

//         dst->fragmentStage.colorAttatchments.reserve(src->fragmentStage.colorAttatchments.size());
//         for (const auto &element : src->fragmentStage.colorAttatchments)
//         {
//           dst->fragmentStage.colorAttatchments.emplace_back(RHIColorAttatchment{.format = element.format, .loadOp = element.loadOp, .storeOp = element.storeOp});
//         }

//         dst->fragmentStage.depthAttatchment.format = src->fragmentStage.depthAttatchment.format;
//         dst->fragmentStage.depthAttatchment.loadOp = src->fragmentStage.depthAttatchment.loadOp;
//         dst->fragmentStage.depthAttatchment.storeOp = src->fragmentStage.depthAttatchment.storeOp;

//         dst->fragmentStage.fragmentShader = src->fragmentStage.fragmentShader;
//         dst->fragmentStage.shaderEntry = src->fragmentStage.shaderEntry;

//         RHIProgram.commands.emplace_back(RHIInstruction{.type = CmdBindGraphicsPipeline, .args = {.graphicsPipeline = dst}});
//         break;
//       }

//       case BindComputePipeline:
//       {
//         auto dst = new RHIComputePipelineInfo{
//           .entry = cmd.args.computePipeline->entry, .layout = resources.bindingLayoutSymbols[cmd.args.computePipeline->layout.name], .shader = cmd.args.computePipeline->shader};

//         RHIProgram.commands.emplace_back(RHIInstruction{.type = CmdBindComputePipeline, .args = {.computePipeline = dst}});
//         break;
//       }

//       case BindVertexBuffer:
//       {
//         uint32_t bufferId = resources.bufferSymbols[cmd.args.bindVertexBuffer->buffer.buffer.name];

//         auto dst = new RHIInstructionBindVertexBufferArgs{
//           .slot = cmd.args.bindVertexBuffer->slot,
//           .buffer = bufferId,
//           .offset = resources.bufferResourceAllocationView[bufferId].offset + cmd.args.bindVertexBuffer->buffer.offset,
//           .size = cmd.args.bindVertexBuffer->buffer.size};

//         RHIProgram.commands.emplace_back(RHIInstruction{.type = CmdBindVertexBuffer, .args = {.bindVertexBuffer = dst}});
//         break;
//       }

//       case BindIndexBuffer:
//       {
//         uint32_t bufferId = resources.bufferSymbols[cmd.args.bindIndexBuffer->buffer.buffer.name];

//         auto dst = new RHIInstructionBindIndexBufferArgs{
//           .buffer = bufferId,
//           .offset = resources.bufferResourceAllocationView[bufferId].offset + cmd.args.bindIndexBuffer->buffer.offset,
//           .size = cmd.args.bindIndexBuffer->buffer.size,
//           .type = cmd.args.bindIndexBuffer->type};

//         RHIProgram.commands.emplace_back(RHIInstruction{.type = CmdBindIndexBuffer, .args = {.bindIndexBuffer = dst}});
//         break;
//       }

//       case Draw:
//       {
//         auto dst = new RHIInstructionDrawArgs{
//           .vertexCount = cmd.args.draw->vertexCount,
//           .instanceCount = cmd.args.draw->instanceCount,
//           .firstVertex = cmd.args.draw->firstVertex,
//           .firstInstance = cmd.args.draw->firstInstance,
//           .waitSemaphores = data.waitSemaphores,
//           .signalSemaphores = data.signalSemaphores};

//         RHIProgram.commands.emplace_back(RHIInstruction{.type = CmdDraw, .args = {.draw = dst}});
//         break;
//       }

//       case DrawIndexed:
//       {
//         auto dst = new RHIInstructionDrawIndexedArgs{
//           .indexCount = cmd.args.drawIndexed->indexCount,
//           .instanceCount = cmd.args.drawIndexed->instanceCount,
//           .firstIndex = cmd.args.drawIndexed->firstIndex,
//           .vertexOffset = cmd.args.drawIndexed->vertexOffset,
//           .firstInstance = cmd.args.drawIndexed->firstInstance,
//           .waitSemaphores = data.waitSemaphores,
//           .signalSemaphores = data.signalSemaphores};

//         RHIProgram.commands.emplace_back(RHIInstruction{.type = CmdDrawIndexed, .args = {.drawIndexed = dst}});
//         break;
//       }

//       case DrawIndexedIndirect:
//       {
//         auto dst = new RHIInstructionDrawIndexedIndirectArgs{
//           .buffer = resources.bufferSymbols[cmd.args.drawIndexedIndirect->buffer.buffer.name],
//           .offset = cmd.args.drawIndexedIndirect->offset,
//           .drawCount = cmd.args.drawIndexedIndirect->drawCount,
//           .stride = cmd.args.drawIndexedIndirect->stride,
//           .waitSemaphores = data.waitSemaphores,
//           .signalSemaphores = data.signalSemaphores};

//         RHIProgram.commands.emplace_back(RHIInstruction{.type = CmdDrawIndexedIndirect, .args = {.drawIndexedIndirect = dst}});
//         break;
//       }

//       case Dispatch:
//       {
//         auto dst = new RHIInstructionDispatchArgs{
//           .x = cmd.args.dispatch->x, .y = cmd.args.dispatch->y, .z = cmd.args.dispatch->z, .waitSemaphores = data.waitSemaphores, .signalSemaphores = data.signalSemaphores};

//         RHIProgram.commands.emplace_back(RHIInstruction{.type = CmdDispatch, .args = {.dispatch = dst}});
//         break;
//       }

//       default:
//         os::Logger::errorf("[RenderGraph] Internal error: unsupported Command type %u", cmd.type);
//         exit(1);
//       }
//     }

//     dispatch.end = RHIProgram.commands.size();
//     RHIProgram.dispatches.push_back(std::move(dispatch));
//   }
// }

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
  os::Logger::logf("[TimeScheduler] analysePasses time = %fms", (analysePassesEnd - analysePassesStart).milliseconds());
  os::Logger::logf("[TimeScheduler] analyseDependencyGraph time = %fms", (analyseDependencyGraphEnd - analyseDependencyGraphStart).milliseconds());
  os::Logger::logf("[TimeScheduler] analyseTaskLevels time = %fms", (analyseTaskLevelsEnd - analyseTaskLevelsStart).milliseconds());
  os::Logger::logf("[TimeScheduler] analyseAllocations time = %fms", (analyseAllocationsEnd - analyseAllocationsStart).milliseconds());
  os::Logger::logf("[TimeScheduler] analyseSemaphores time = %fms", (analyseSemaphoresEnd - analyseSemaphoresStart).milliseconds());
  // os::Logger::logf("[TimeScheduler] outputCommands time = %fns", (outputCommandsEnd - outputCommandsStart).nanoseconds());

  // for (auto &data : nodes)
  // {
  //   for (auto &command : data.commands)
  //   {
  //     switch (command.type)
  //     {
  //     case BeginRenderPass:
  //       delete command.args.renderPassInfo;
  //       break;
  //     case EndRenderPass:
  //       break;
  //     case CopyBuffer:
  //       delete command.args.copyBuffer;
  //       break;
  //     case BindBindingGroups:
  //       delete command.args.bindGroups;
  //       break;
  //     case BindGraphicsPipeline:
  //       delete command.args.graphicsPipeline;
  //       break;
  //     case BindComputePipeline:
  //       delete command.args.computePipeline;
  //       break;
  //     case BindVertexBuffer:
  //       delete command.args.bindVertexBuffer;
  //       break;
  //     case BindIndexBuffer:
  //       delete command.args.bindIndexBuffer;
  //       break;
  //     case Draw:
  //       delete command.args.draw;
  //       break;
  //     case DrawIndexed:
  //       delete command.args.drawIndexed;
  //       break;
  //     case DrawIndexedIndirect:
  //       delete command.args.drawIndexedIndirect;
  //       break;
  //     case Dispatch:
  //       delete command.args.dispatch;
  //       break;
  //     default:
  //       os::Logger::errorf("[RenderGraph] internal error: invalid command type %u", command.type);
  //       exit(1);
  //       break;
  //     }
  //   }
  // }

  compiled = true;
}

CommandRecorder::CommandRecorder()
{
  recorded.push_back(CommandSequence());
}

const Buffer RenderGraph::createBuffer(const BufferInfo &info)
{
  const auto &name = info.name;

  auto id = resources.bufferSymbols.find(name);

  if (id == resources.bufferSymbols.end())
  {
    throw std::runtime_error("Buffer already created");
  }

  id = resources.buffersAllocated.fetch_add(1);

  resources.bufferSymbols.insert(name, id);
  resources.bufferMetadatas.push_back({});

  resources.bufferMetadatas[id].id = id;
  resources.bufferMetadatas[id].bufferInfo = info;

  return Buffer{
    .name = info.name,
  };
}

const Texture RenderGraph::createTexture(const TextureInfo &info)
{
  const auto &name = info.name;

  auto id = resources.textureSymbols.find(name);

  if (id == resources.textureSymbols.end())
  {
    throw std::runtime_error("Texture already created");
  }

  id = resources.texturesAllocated.fetch_add(1);

  resources.textureSymbols.insert(name, id);
  resources.textureMetadatas.push_back({});

  resources.textureMetadatas[id].id = id;
  resources.textureMetadatas[id].textureInfo = info;

  return Texture{
    .name = info.name,
  };
}

const Sampler RenderGraph::createSampler(const SamplerInfo &info)
{
  const auto &name = info.name;

  auto id = resources.samplerSymbols.find(name);

  if (id == resources.samplerSymbols.end())
  {
    throw std::runtime_error("Sampler already created");
  }

  id = resources.samplersAllocated.fetch_add(1);

  resources.samplerSymbols.insert(name, id);
  resources.samplerMetadatas.push_back({});

  resources.samplerMetadatas[id].id = id;
  resources.samplerMetadatas[id].samplerInfo = info;

  return Sampler{
    .name = info.name,
  };
}

const BindingGroups RenderGraph::createBindingGroups(const BindingGroupsInfo &info)
{
  const auto &name = info.name;
  auto id = resources.bindingGroupsSymbols.find(name);

  if (id == resources.bindingGroupsSymbols.end())
  {
    throw std::runtime_error("BindingGroups already created");
  }

  id = resources.bindingGroupsAllocated.fetch_add(1);

  resources.bindingGroupsSymbols.insert(name, id);
  resources.bindingGroupsMetadata.push_back({});

  resources.bindingGroupsMetadata[id].id = id;
  resources.bindingGroupsMetadata[id].groupsInfo = info;

  return BindingGroups{
    .name = info.name,
  };
}

const GraphicsPipeline RenderGraph::createGraphicsPipeline(const GraphicsPipelineInfo &info)
{
  const auto &name = info.name;

  auto id = resources.graphicsPipelineSymbols.find(name);

  if (id == resources.graphicsPipelineSymbols.end())
  {
    throw std::runtime_error("GraphicsPipeline already created");
  }

  id = resources.graphicsPipelinesAllocated.fetch_add(1);

  resources.graphicsPipelineSymbols.insert(name, id);
  resources.graphicsPipelineMetadata.push_back({});

  resources.graphicsPipelineMetadata[id].id = id;
  resources.graphicsPipelineMetadata[id].pipelineInfo = info;

  return GraphicsPipeline{
    .name = info.name,
  };
}

const ComputePipeline RenderGraph::createComputePipeline(const ComputePipelineInfo &info)
{
  const auto &name = info.name;

  auto id = resources.computePipelineSymbols.find(name);

  if (id == resources.computePipelineSymbols.end())
  {
    throw std::runtime_error("ComputePipeline already created");
  }
  id = resources.computePipelinesAllocated.fetch_add(1);

  resources.computePipelineSymbols.insert(name, id);
  resources.computePipelineMetadata.push_back({});

  resources.computePipelineMetadata[id].id = id;
  resources.computePipelineMetadata[id].pipelineInfo = info;

  return ComputePipeline{
    .name = info.name,
  };
}

const BindingsLayout RenderGraph::createBindingsLayout(const BindingsLayoutInfo &info)
{
  const auto &name = info.name;

  auto id = resources.bindingLayoutSymbols.find(name);

  if (id == resources.bindingLayoutSymbols.end())
  {
    throw std::runtime_error("BindingsLayout already created");
  }

  id = resources.bindingLayoutsAllocated.fetch_add(1);

  resources.bindingLayoutSymbols.insert(name, id);
  resources.bindingsLayoutMetadata.push_back({});

  resources.bindingsLayoutMetadata[id].id = id;
  resources.bindingsLayoutMetadata[id].layoutsInfo = info;

  return BindingsLayout{
    .name = info.name,
  };
}

const Buffer RenderGraph::createScratchBuffer(const BufferInfo &info)
{
  const auto &name = info.name;

  auto id = resources.scratchBufferSymbols.find(name);

  if (id == resources.scratchBufferSymbols.end())
  {
    throw std::runtime_error("Scratch Buffer already created");
  }

  id = resources.scratchBuffersAllocated.fetch_add(1);

  resources.scratchBufferSymbols.insert(name, id);
  resources.scratchBuffersRequestsMetadatas.push_back({});

  resources.scratchBuffersRequestsMetadatas[id].id = id;
  resources.scratchBuffersRequestsMetadatas[id].bufferInfo = info;

  return Buffer{
    .name = info.name,
  };
}

const Buffer ResourceDatabase::getScratchBuffer(const std::string &name)
{
  if (!scratchBufferSymbols.contains(name))
  {
    throw std::runtime_error("Buffer not found");
  }

  return Buffer{
    .name = name,
  };
}

const Buffer ResourceDatabase::getBuffer(const std::string &name)
{
  if (!bufferSymbols.contains(name))
  {
    throw std::runtime_error("Buffer not found");
  }

  return Buffer{
    .name = name,
  };
}

const BindingsLayout ResourceDatabase::getBindingsLayout(const std::string &name)
{
  if (!bindingLayoutSymbols.contains(name))
  {
    throw std::runtime_error("BindingsLayout not found");
  }

  return BindingsLayout{
    .name = name,
  };
}

const BindingGroups ResourceDatabase::getBindingGroups(const std::string &name)
{
  if (!bindingGroupsSymbols.contains(name))
  {
    throw std::runtime_error("BindingGroups not found");
  }

  return BindingGroups{
    .name = name,
  };
}
const GraphicsPipeline ResourceDatabase::getGraphicsPipeline(const std::string &name)
{
  if (!graphicsPipelineSymbols.contains(name))
  {
    throw std::runtime_error("GraphicsPipeline not found");
  }

  return GraphicsPipeline{
    .name = name,
  };
}

const ComputePipeline ResourceDatabase::getComputePipeline(const std::string &name)
{
  if (!computePipelineSymbols.contains(name))
  {
    throw std::runtime_error("ComputePipeline not found");
  }

  return ComputePipeline{
    .name = name,
  };
}

const Sampler ResourceDatabase::getSampler(const std::string &name)
{
  if (!samplerSymbols.contains(name))
  {
    throw std::runtime_error("Sampler not found");
  }
  return Sampler{
    .name = name,
  };
}

const Texture ResourceDatabase::getTexture(const std::string &name)
{
  if (!textureSymbols.contains(name))
  {
    throw std::runtime_error("Texture not found");
  }

  return Texture{
    .name = name,
  };
}

// const TextureView ResourceDatabase::getNextSwapChainTexture(const SwapChain &swapChain) const
// {
// // TODO:
//   return TextureView{
//     .texture =
//         Texture{
//           .name = "<SwapChainRenderTexture",
//         },
//     .access = AccessPattern::COLOR_ATTACHMENT_WRITE,
//     .baseArrayLayer = 0,
//     .baseMipLevel = 0,
//     .layerCount = 1,
//     .levelCount = 1,
//     .layout = ResourceLayout::COLOR_ATTACHMENT,
//     .flags = ImageAspectFlags::Color,
//   };
// }

void CommandRecorder::cmdBeginRenderPass(const RenderPassInfo &info)
{
  Command cmd{CommandType::BeginRenderPass};
  cmd.args.renderPassInfo = new RenderPassInfo();
  *cmd.args.renderPassInfo = info;
  recorded.back().commands.push_back(std::move(cmd));
}

void CommandRecorder::cmdEndRenderPass()
{
  recorded.back().commands.push_back({CommandType::EndRenderPass});
}

void CommandRecorder::cmdCopyBuffer(BufferView src, BufferView dst)
{
  Command cmd{CommandType::CopyBuffer};
  cmd.args.copyBuffer = new CopyBufferArgs();
  *cmd.args.copyBuffer = {src, dst};
  recorded.back().commands.push_back(std::move(cmd));
  recorded.push_back(CommandSequence());
}

void CommandRecorder::cmdBindBindingGroups(BindingGroups groups, uint32_t *dynamicOffsets, uint32_t dynamicOffsetsCount)
{
  Command cmd{CommandType::BindBindingGroups};
  cmd.args.bindGroups = new BindGroupsArgs();
  cmd.args.bindGroups->groups = groups;
  cmd.args.bindGroups->dynamicOffsets.assign(dynamicOffsets, dynamicOffsets + dynamicOffsetsCount);
  recorded.back().commands.push_back(std::move(cmd));
}

void CommandRecorder::cmdBindGraphicsPipeline(GraphicsPipeline pipeline)
{
  Command cmd{CommandType::BindGraphicsPipeline};
  cmd.args.graphicsPipeline = new GraphicsPipeline();
  *cmd.args.graphicsPipeline = pipeline;
  recorded.back().commands.push_back(std::move(cmd));
}

void CommandRecorder::cmdBindComputePipeline(ComputePipeline pipeline)
{
  Command cmd{CommandType::BindComputePipeline};
  cmd.args.computePipeline = new ComputePipeline();
  *cmd.args.computePipeline = pipeline;
  recorded.back().commands.push_back(std::move(cmd));
}

void CommandRecorder::cmdBindVertexBuffer(uint32_t slot, BufferView view)
{
  Command cmd{CommandType::BindVertexBuffer};
  cmd.args.bindVertexBuffer = new BindVertexBufferArgs();
  *cmd.args.bindVertexBuffer = {slot, view};
  recorded.back().commands.push_back(std::move(cmd));
}

void CommandRecorder::cmdBindIndexBuffer(BufferView view, Type type)
{
  Command cmd{CommandType::BindIndexBuffer};
  cmd.args.bindIndexBuffer = new BindIndexBufferArgs();
  *cmd.args.bindIndexBuffer = {view, type};
  recorded.back().commands.push_back(std::move(cmd));
}

void CommandRecorder::cmdDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
{
  Command cmd{CommandType::Draw};
  cmd.args.draw = new DrawArgs();
  *cmd.args.draw = {vertexCount, instanceCount, firstVertex, firstInstance};
  recorded.back().commands.push_back(std::move(cmd));
  recorded.push_back(CommandSequence());
}

void CommandRecorder::cmdDrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, uint32_t vertexOffset, uint32_t firstInstance)
{
  Command cmd{CommandType::DrawIndexed};
  cmd.args.drawIndexed = new DrawIndexedArgs();
  *cmd.args.drawIndexed = {indexCount, instanceCount, firstIndex, firstInstance, vertexOffset};
  recorded.back().commands.push_back(std::move(cmd));
  recorded.push_back(CommandSequence());
}

void CommandRecorder::cmdDrawIndexedIndirect(BufferView buffer, uint32_t offset, uint32_t drawCount, uint32_t stride)
{
  Command cmd{CommandType::DrawIndexedIndirect};
  cmd.args.drawIndexedIndirect = new DrawIndexedIndirectArgs();
  *cmd.args.drawIndexedIndirect = {buffer, offset, drawCount, stride};
  recorded.back().commands.push_back(std::move(cmd));
  recorded.push_back(CommandSequence());
}

void CommandRecorder::cmdDispatch(uint32_t x, uint32_t y, uint32_t z)
{
  Command cmd{CommandType::Dispatch};
  cmd.args.dispatch = new DispatchArgs();
  *cmd.args.dispatch = {x, y, z};
  recorded.back().commands.push_back(std::move(cmd));
  recorded.push_back(CommandSequence());
}

// void RHIProgram::log()
// {
//   // TODO: move resources to RHIProgram and log names

//   for (size_t i = 0; i < commands.size(); ++i)
//   {
//     const auto &cmd = commands[i];

//     switch (cmd.type)
//     {
//     case CmdBeginRenderPass:
//     {
//       const auto &rp = cmd.args.renderPassInfo;

//       os::Logger::logf("[RHIProgram] CmdBeginRenderPass - name: %s", rp->name.c_str());
//       os::Logger::logf("[RHIProgram]    Viewport: width=%.2f height=%.2f", rp->viewport.width, rp->viewport.height);
//       os::Logger::logf("[RHIProgram]    Scissor: offset=(%d,%d) extent=(%u,%u)", rp->scissor.x, rp->scissor.y, rp->scissor.width, rp->scissor.height);

//       if (!rp->colorAttachments.empty())
//       {
//         os::Logger::logf("[RHIProgram]    Color Attachments (%zu):", rp->colorAttachments.size());
//         for (size_t j = 0; j < rp->colorAttachments.size(); ++j)
//         {
//           const auto &color = rp->colorAttachments[j];
//           os::Logger::logf("[RHIProgram]      [%zu] name: %s", j, color.name.c_str());
//           os::Logger::logf(
//               "[RHIProgram]           texture=%s clear=(%.2f, %.2f, %.2f, %.2f)",
//               textures[color.view.textureId].name.c_str(),
//               color.clearValue.R,
//               color.clearValue.G,
//               color.clearValue.B,
//               color.clearValue.A);
//         }
//       }
//       else
//       {
//         os::Logger::logf("[RHIProgram]    No color attachments.");
//       }

//       if (!rp->depthStencilAttachment.empty())
//       {
//         os::Logger::logf("[RHIProgram]    Depth-Stencil Attachments (%zu):", rp->depthStencilAttachment.size());
//         for (size_t j = 0; j < rp->depthStencilAttachment.size(); ++j)
//         {
//           const auto &depth = rp->depthStencilAttachment[j];
//           os::Logger::logf("[RHIProgram]      [%zu] name: %s", j, depth.name.c_str());
//           os::Logger::logf("[RHIProgram]           texture=%s clearDepth=%.2f clearStencil=%u", textures[depth.view.textureId].name.c_str(), depth.clearDepth,
//           depth.clearStencil);
//         }
//       }
//       else
//       {
//         os::Logger::logf("[RHIProgram]    No depth-stencil attachments.");
//       }
//     }
//     break;

//     case CmdEndRenderPass:
//       os::Logger::logf("[RHIProgram] CmdEndRenderPass");
//       break;

//     case CmdCopyBuffer:
//       if (cmd.args.copyBuffer)
//       {
//         const auto &c = *cmd.args.copyBuffer;
//         os::Logger::logf(
//             "[RHIProgram] CmdCopyBuffer src=%s dst=%s srcOffset=%u dstOffset=%u size=%u",
//             buffers[c.src].name.c_str(),
//             buffers[c.dst].name.c_str(),
//             c.srcOffset,
//             c.dstOffset,
//             c.srcSize);
//       }
//       break;

//     case CmdBindBindingGroups:
//     {
//       const auto *args = cmd.args.bindGroups;
//       if (!args)
//       {
//         os::Logger::logf("[RHIProgram] CmdBindBindingGroups - (null)");
//         break;
//       }
//       const auto &groups = args->groups;
//       const auto &layoutInfo = bindingLayouts[groups.layout];

//       os::Logger::logf("[RHIProgram] CmdBindBindingGroups - name: %s", groups.name.c_str());
//       os::Logger::logf("[RHIProgram]    Layout: %s (%u groups)", layoutInfo.name.c_str(), (uint32_t)layoutInfo.groups.size());

//       for (size_t groupIndex = 0; groupIndex < groups.groups.size(); ++groupIndex)
//       {
//         const auto &group = groups.groups[groupIndex];
//         const auto &groupLayout = layoutInfo.groups[groupIndex];

//         if (!group.buffers.empty())
//         {
//           for (const auto &b : group.buffers)
//           {
//             uint32_t at = 0;

//             for (auto &g : bindingLayouts[groups.layout].groups[groupIndex].buffers)
//             {
//               if (g.binding == b.binding)
//               {
//                 break;
//               }
//               at += 1;
//             }

//             os::Logger::logf(
//                 "[RHIProgram]    * [group=%u, binding=%u] buffer=%s offset=%u size=%u %s%s",
//                 groupIndex,
//                 b.binding,
//                 buffers[b.bufferIndex].name.c_str(),
//                 b.offset,
//                 b.size,
//                 groupLayout.buffers[at].isDynamic ? "(dynamic) " : "",
//                 groupLayout.buffers[at].name.c_str());
//           }
//         }

//         if (!group.samplers.empty())
//         {
//           for (const auto &s : group.samplers)
//           {
//             os::Logger::logf(
//                 "[RHIProgram]    * [group=%u, binding=%u] sampler=%u texture=%s mips=[%u..%u] layers=[%u..%u] aspect=%u",
//                 groupIndex,
//                 s.binding,
//                 s.sampler,
//                 textures[textureViews[s.textureView].textureId].name.c_str(),
//                 textureViews[s.textureView].baseMipLevel,
//                 textureViews[s.textureView].baseMipLevel + textureViews[s.textureView].levelCount - 1,
//                 textureViews[s.textureView].baseArrayLayer,
//                 textureViews[s.textureView].baseArrayLayer + textureViews[s.textureView].layerCount - 1,
//                 (uint32_t)textureViews[s.textureView].flags);
//           }
//         }

//         if (!group.textures.empty())
//         {
//           for (const auto &t : group.textures)
//           {
//             os::Logger::logf(
//                 "[RHIProgram]    * [group=%u, binding=%u] texture=%s mips=[%u..%u] layers=[%u..%u] aspect=%u",
//                 groupIndex,
//                 t.binding,
//                 textures[textureViews[t.textureView].textureId].name.c_str(), // optional safeguard
//                 textureViews[t.textureView].baseMipLevel,
//                 textureViews[t.textureView].baseMipLevel + textureViews[t.textureView].levelCount - 1,
//                 textureViews[t.textureView].baseArrayLayer,
//                 textureViews[t.textureView].baseArrayLayer + textureViews[t.textureView].layerCount - 1,
//                 (uint32_t)textureViews[t.textureView].flags);
//           }
//         }

//         if (!group.storageTextures.empty())
//         {
//           for (const auto &t : group.storageTextures)
//           {
//             os::Logger::logf(
//                 "[RHIProgram]    * [giroup=%u, binding=%u] texture=%s mips=[%u..%u] layers=[%u..%u] aspect=%u",
//                 groupIndex,
//                 t.binding,
//                 textures[textureViews[t.textureView].textureId].name.c_str(),
//                 textureViews[t.textureView].baseMipLevel,
//                 textureViews[t.textureView].baseMipLevel + textureViews[t.textureView].levelCount - 1,
//                 textureViews[t.textureView].baseArrayLayer,
//                 textureViews[t.textureView].baseArrayLayer + textureViews[t.textureView].layerCount - 1,
//                 (uint32_t)textureViews[t.textureView].flags);
//           }
//         }

//         if (group.buffers.empty() && group.samplers.empty() && group.textures.empty() && group.storageTextures.empty())
//         {
//           os::Logger::logf("      (empty group)");
//         }
//       }

//       if (args->dynamicOffsets.size())
//       {
//         os::Logger::logf("[RHIProgram]    Dynamic Offsets: %zu", args->dynamicOffsets.size());

//         for (size_t i = 0; i < args->dynamicOffsets.size(); ++i)
//         {
//           os::Logger::logf("[RHIProgram]     [%zu] offset=%u", i, args->dynamicOffsets[i]);
//         }
//       }
//     }
//     break;

//     case CmdBindGraphicsPipeline:
//       os::Logger::logf("[RHIProgram] CmdBindGraphicsPipeline");
//       break;

//     case CmdBindComputePipeline:
//       os::Logger::logf("[RHIProgram] CmdBindComputePipeline");
//       break;

//     case CmdBindVertexBuffer:
//       if (cmd.args.bindVertexBuffer)
//       {
//         const auto &b = *cmd.args.bindVertexBuffer;
//         os::Logger::logf("[RHIProgram] CmdBindVertexBuffer slot=%u buffer=%s offset=%u size=%u", b.slot, buffers[b.buffer].name.c_str(), b.offset, b.size);
//       }
//       break;

//     case CmdBindIndexBuffer:
//       if (cmd.args.bindIndexBuffer)
//       {
//         const auto &b = *cmd.args.bindIndexBuffer;
//         os::Logger::logf("[RHIProgram] CmdBindIndexBuffer buffer=%s offset=%u size=%u type=%d", buffers[b.buffer].name.c_str(), b.offset, b.size, (int)b.type);
//       }
//       break;

//     case CmdDraw:
//       if (cmd.args.draw)
//       {
//         const auto &d = *cmd.args.draw;
//         os::Logger::logf("[RHIProgram] CmdDraw vertexCount=%u instanceCount=%u firstVertex=%u firstInstance=%u", d.vertexCount, d.instanceCount, d.firstVertex, d.firstInstance);
//         for (auto &semaphore : cmd.args.draw->waitSemaphores)
//         {
//           os::Logger::logf(
//               "[RHIProgram] * [wait semaphore] %u, %s -> %s", wait, toString(semaphores[semaphore].signalQueue).c_str(), toString(semaphores[semaphore].waitQueue).c_str());
//         }
//         for (auto &semaphore : cmd.args.draw->signalSemaphores)
//         {
//           os::Logger::logf(
//               "[RHIProgram] * [signal semaphore] %u, %s -> %s", wait, toString(semaphores[semaphore].signalQueue).c_str(), toString(semaphores[semaphore].waitQueue).c_str());
//         }
//       }
//       break;

//     case CmdDrawIndexed:
//       if (cmd.args.drawIndexed)
//       {
//         const auto &d = *cmd.args.drawIndexed;
//         os::Logger::logf(
//             "[RHIProgram] CmdDrawIndexed indexCount=%u instanceCount=%u firstIndex=%u vertexOffset=%d firstInstance=%u",
//             d.indexCount,
//             d.instanceCount,
//             d.firstIndex,
//             d.vertexOffset,
//             d.firstInstance);
//       }
//       for (auto &semaphore : cmd.args.drawIndexed->waitSemaphores)
//       {
//         os::Logger::logf(
//             "[RHIProgram] * [wait semaphore] %u, %s -> %s", wait, toString(semaphores[semaphore].signalQueue).c_str(), toString(semaphores[semaphore].waitQueue).c_str());
//       }
//       for (auto &semaphore : cmd.args.drawIndexed->signalSemaphores)
//       {
//         os::Logger::logf(
//             "[RHIProgram] * [signal semaphore] %u, %s -> %s", wait, toString(semaphores[semaphore].signalQueue).c_str(), toString(semaphores[semaphore].waitQueue).c_str());
//       }
//       break;

//     case CmdDrawIndexedIndirect:
//       if (cmd.args.drawIndexedIndirect)
//       {
//         const auto &d = *cmd.args.drawIndexedIndirect;
//         os::Logger::logf("[RHIProgram] CmdDrawIndexedIndirect buffer=%s offset=%u drawCount=%u stride=%u", buffers[d.buffer].name.c_str(), d.offset, d.drawCount, d.stride);
//         for (auto &semaphore : cmd.args.drawIndexedIndirect->waitSemaphores)
//         {
//           os::Logger::logf(
//               "[RHIProgram] * [wait semaphore] %u, %s -> %s", wait, toString(semaphores[semaphore].signalQueue).c_str(), toString(semaphores[semaphore].waitQueue).c_str());
//         }
//         for (auto &semaphore : cmd.args.drawIndexedIndirect->signalSemaphores)
//         {
//           os::Logger::logf(
//               "[RHIProgram] * [signal semaphore] %u, %s -> %s", wait, toString(semaphores[semaphore].signalQueue).c_str(), toString(semaphores[semaphore].waitQueue).c_str());
//         }
//       }
//       break;

//     case CmdDispatch:
//       if (cmd.args.dispatch)
//       {
//         os::Logger::logf("[RHIProgram] CmdDispatch x=%u y=%u z=%u", cmd.args.dispatch->x, cmd.args.dispatch->y, cmd.args.dispatch->z);
//       }
//       for (auto &semaphore : cmd.args.dispatch->waitSemaphores)
//       {
//         os::Logger::logf(
//             "[RHIProgram] * [wait semaphore] %u, %s -> %s", wait, toString(semaphores[semaphore].signalQueue).c_str(), toString(semaphores[semaphore].waitQueue).c_str());
//       }
//       for (auto &semaphore : cmd.args.dispatch->signalSemaphores)
//       {
//         os::Logger::logf(
//             "[RHIProgram] * [signal semaphore] %u, %s -> %s", wait, toString(semaphores[semaphore].signalQueue).c_str(), toString(semaphores[semaphore].waitQueue).c_str());
//       }
//       break;

//     case CmdBufferBarrier:
//       if (cmd.args.bufferBarrier)
//       {
//         const auto &b = *cmd.args.bufferBarrier;
//         os::Logger::logf(
//             "[RHIProgram] CmdBufferBarrier %s offset=%u size=%u access=%s->%s",
//             buffers[b.buffer].name.c_str(),
//             b.offset,
//             b.size,
//             toString(b.fromAccess).c_str(),
//             toString(b.toAccess).c_str());
//       }
//       break;

//     case CmdTextureBarrier:
//       if (cmd.args.textureBarrier)
//       {
//         const auto &t = *cmd.args.textureBarrier;
//         os::Logger::logf(
//             "[RHIProgram] CmdTextureBarrier %s mip=[%u...%u] layer=[%u...%u] layout=%s->%s access=%s->%s",
//             textures[t.texture].name.c_str(),
//             t.baseMip,
//             t.baseMip + t.mipCount - 1,
//             t.baseLayer,
//             t.baseLayer + t.layerCount - 1,
//             toString(t.fromLayout).c_str(),
//             toString(t.toLayout).c_str(),
//             toString(t.fromAccess).c_str(),
//             toString(t.toAccess).c_str());
//       }
//       break;

//     default:
//       os::Logger::logf("  Unknown command type (%d)", (int)cmd.type);
//       break;
//     }
//   }
// }

} // namespace rendering
