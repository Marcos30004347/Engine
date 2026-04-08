#include "RenderGraph.hpp"

#include <stdexcept>
#include <utility>

namespace rendering
{

namespace
{

template <typename MetadataMap> auto findMetadataOrThrow(MetadataMap &metadataMap, const std::string &name, const char *resourceKind)
{
  auto metadata = metadataMap.find(name);
  if (metadata == metadataMap.end())
  {
    throw std::runtime_error(std::string(resourceKind) + " '" + name + "' not found");
  }

  return metadata;
}

template <typename MetadataMap, typename Usage> void appendUsageOrThrow(MetadataMap &metadataMap, const std::string &name, const char *resourceKind, Usage &&usage)
{
  auto metadata = findMetadataOrThrow(metadataMap, name, resourceKind);
  metadata->second.usages.push_back(std::forward<Usage>(usage));
}

template <typename MetadataMap> void assertMetadataExists(MetadataMap &metadataMap, const std::string &name, const char *resourceKind)
{
  findMetadataOrThrow(metadataMap, name, resourceKind);
}

} // namespace

RHIResources::RHIResources(RenderGraph *renderGraph) : renderGraph(renderGraph)
{
}

void RHIResources::recordConsumerUsage(const std::string &name, const InputResource &res, uint32_t taskId, Queue queue)
{
  // TODO: this stinks
  switch (res.type)
  {
  case ResourceType::ResourceType_BufferView:
    appendUsageOrThrow(
        bufferMetadatas,
        name,
        "Buffer",
        BufferResourceUsage{
          .view = res.bufferView,
          .consumer = taskId,
          .queue = queue,
        });
    return;

  case ResourceType::ResourceType_TextureView:
    appendUsageOrThrow(
        textureMetadatas,
        name,
        "Texture",
        TextureResourceUsage{
          .view = res.textureView,
          .consumer = taskId,
          .queue = queue,
        });
    return;

  case ResourceType::ResourceType_Sampler:
    appendUsageOrThrow(
        samplerMetadatas,
        name,
        "Sampler",
        SamplerResourceUsage{
          .sampler = res.sampler,
          .consumer = taskId,
          .queue = queue,
        });
    return;

  case ResourceType::ResourceType_BindingsLayout:
    appendUsageOrThrow(
        bindingsLayoutMetadata,
        name,
        "BindingsLayout",
        BindingsLayoutResourceUsage{
          .consumer = taskId,
          .queue = queue,
        });
    return;

  case ResourceType::ResourceType_BindingGroups:
    appendUsageOrThrow(
        bindingGroupsMetadata,
        name,
        "BindingGroups",
        BindingGroupsResourceUsage{
          .consumer = taskId,
          .queue = queue,
        });
    return;

  case ResourceType::ResourceType_ComputePipeline:
    appendUsageOrThrow(
        computePipelineMetadata,
        name,
        "ComputePipeline",
        ComputePipelineResourceUsage{
          .consumer = taskId,
          .queue = queue,
        });
    return;

  case ResourceType::ResourceType_GraphicsPipeline:
    appendUsageOrThrow(
        graphicsPipelineMetadata,
        name,
        "GraphicsPipeline",
        GraphicsPipelineResourceUsage{
          .consumer = taskId,
          .queue = queue,
        });
    return;

  default:
    throw std::runtime_error("Unknown resource type in recordConsumerUsage");
  }
}

Format RHIResources::getTextureFormat(const std::string &name)
{
  auto metadata = findMetadataOrThrow(textureMetadatas, name, "Texture");
  return metadata->second.textureInfo.format;
}

const BindingGroups RHIResources::getBindingGroups(const std::string &name)
{
  assertMetadataExists(bindingGroupsMetadata, name, "BindingGroups");
  return BindingGroups{.name = name};
}

const GraphicsPipeline RHIResources::getGraphicsPipeline(const std::string &name)
{
  auto metadata = findMetadataOrThrow(graphicsPipelineMetadata, name, "GraphicsPipeline");
  return GraphicsPipeline{.name = name, .id = metadata->second.resourceId};
}

const ComputePipeline RHIResources::getComputePipeline(const std::string &name)
{
  auto metadata = findMetadataOrThrow(computePipelineMetadata, name, "ComputePipeline");
  return ComputePipeline{.name = name, .id = metadata->second.resourceId};
}

const BindingsLayout RHIResources::getBindingsLayout(const std::string &name)
{
  auto metadata = findMetadataOrThrow(bindingsLayoutMetadata, name, "BindingsLayout");
  return BindingsLayout{.name = name, .id = metadata->second.resourceId};
}

const Sampler RHIResources::getSampler(const std::string &name)
{
  auto metadata = findMetadataOrThrow(samplerMetadatas, name, "Sampler");
  return Sampler{.name = name, .id = metadata->second.resourceId};
}

const Buffer RHIResources::getBuffer(const std::string &name)
{
  assertMetadataExists(bufferMetadatas, name, "Buffer");
  return Buffer{.name = name};
}

const Texture RHIResources::getTexture(const std::string &name)
{
  assertMetadataExists(textureMetadatas, name, "Texture");
  return Texture{.name = name};
}

} // namespace rendering
