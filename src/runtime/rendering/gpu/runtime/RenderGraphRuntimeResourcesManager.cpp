#include "RenderGraphRuntimeResourcesManager.hpp"

#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace rendering
{

namespace
{

template <typename Map, typename Value> void upsert(Map &map, const std::string &key, Value &&value)
{
  map.insert(key, std::forward<Value>(value));
}

} // namespace

RenderGraphRuntimeResourcesManager::RenderGraphRuntimeResourcesManager(RenderGraph &renderGraph) : renderGraph(&renderGraph)
{
}

void RenderGraphRuntimeResourcesManager::beginRun(const RenderGraphOverrides &overrides, uint32_t frameIndex)
{
  resetMetrics();
  currentOverrides = &overrides;
  currentFrameIndex = frameIndex;
  runBufferCache.clear();
  runTextureCache.clear();
  runTextureFormatCache.clear();
  preparedBindingGroupsCache.clear();
  overrideBindingGroups.clear();

  prepareBaseRuntimeResources();
  applyOverridesToRuntimeContext();
  preparedBindingGroupsCache.clear();
}

void RenderGraphRuntimeResourcesManager::endRun()
{
  RetainedResources retainedResources = detachRunResources();

  for (const BindingGroupsId bindingGroupsId : retainedResources.bindingGroups)
  {
    if (bindingGroupsId != BindingGroupsId::Invalid)
    {
      renderGraph->rhi->deleteBindingGroups(bindingGroupsId);
    }
  }
}

RenderGraphRuntimeResourcesManager::RetainedResources RenderGraphRuntimeResourcesManager::detachRunResources()
{
  RetainedResources retainedResources;
  auto &runtimeContext = renderGraph->runtimeContext;

  for (auto it = overrideBindingGroups.rbegin(); it != overrideBindingGroups.rend(); ++it)
  {
    auto &runtimeMetadata = runtimeContext.bindingGroups[it->first];
    const BindingGroupsId overrideId = runtimeMetadata.resourceId;

    if (overrideId != BindingGroupsId::Invalid && overrideId != it->second)
    {
      retainedResources.bindingGroups.push_back(overrideId);
    }

    runtimeMetadata.resourceId = it->second;
  }

  currentOverrides = nullptr;
  runBufferCache.clear();
  runTextureCache.clear();
  runTextureFormatCache.clear();
  preparedBindingGroupsCache.clear();
  overrideBindingGroups.clear();

  for (auto &texture : runtimeContext.textures)
  {
    texture.overrideLayout = ResourceLayout::UNDEFINED;
  }

  return retainedResources;
}

uint32_t RenderGraphRuntimeResourcesManager::getFrameSlot() const
{
  return renderGraph->getMaxFramesInFlight() == 0u ? 0u : (currentFrameIndex % renderGraph->getMaxFramesInFlight());
}

BufferId RenderGraphRuntimeResourcesManager::resolveCreatedBufferId(const std::string &graphBufferName, const BufferInfo &bufferInfo)
{
  auto created = createdBuffers.find(graphBufferName);
  if (!created)
  {
    std::vector<BufferId> frameResources(bufferInfo.frameLocal ? renderGraph->getMaxFramesInFlight() : 1u, BufferId::Invalid);
    createdBuffers.insert(graphBufferName, frameResources);
    created = createdBuffers.find(graphBufferName);
  }

  auto &frameResources = *created;
  const uint32_t frameSlot = bufferInfo.frameLocal ? getFrameSlot() : 0u;
  if (frameSlot >= frameResources.size())
  {
    frameResources.resize(frameSlot + 1u, BufferId::Invalid);
  }

  if (frameResources[frameSlot] == BufferId::Invalid)
  {
    frameResources[frameSlot] = measureMetric(MetricId::ResolveBufferCreateRHI, [&]() { return renderGraph->rhi->createBuffer(bufferInfo); });
  }

  return frameResources[frameSlot];
}

TextureId RenderGraphRuntimeResourcesManager::resolveCreatedTextureId(const std::string &graphTextureName, const TextureInfo &textureInfo)
{
  auto created = createdTextures.find(graphTextureName);
  if (!created)
  {
    std::vector<TextureId> frameResources(textureInfo.frameLocal ? renderGraph->getMaxFramesInFlight() : 1u, TextureId::Invalid);
    createdTextures.insert(graphTextureName, frameResources);
    created = createdTextures.find(graphTextureName);
  }

  auto &frameResources = *created;
  const uint32_t frameSlot = textureInfo.frameLocal ? getFrameSlot() : 0u;
  if (frameSlot >= frameResources.size())
  {
    frameResources.resize(frameSlot + 1u, TextureId::Invalid);
  }

  if (frameResources[frameSlot] == TextureId::Invalid)
  {
    frameResources[frameSlot] = measureMetric(MetricId::ResolveTextureCreateRHI, [&]() { return renderGraph->rhi->createTexture(textureInfo); });
  }

  return frameResources[frameSlot];
}

void RenderGraphRuntimeResourcesManager::prepareBaseRuntimeResources()
{
  auto &runtimeContext = renderGraph->runtimeContext;

  for (auto &buffer : runtimeContext.buffers)
  {
    buffer.resourceId = BufferId::Invalid;

    auto databaseIt = renderGraph->resources.bufferMetadatas.find(buffer.name);
    if (databaseIt != renderGraph->resources.bufferMetadatas.end())
    {
      buffer.resourceId = databaseIt->second.resourceId;
    }

    if (buffer.resourceId == BufferId::Invalid && !buffer.bufferInfo.isVirtual)
    {
      buffer.resourceId = resolveCreatedBufferId(buffer.name, buffer.bufferInfo);
    }

    runBufferCache.insert(
        buffer.name,
        BufferRunCacheEntry{
            .resourceId = buffer.resourceId,
            .source = buffer.resourceId == BufferId::Invalid ? RuntimeResourceSource::Passthrough : RuntimeResourceSource::Created,
        });
  }

  for (auto &texture : runtimeContext.textures)
  {
    texture.overrideLayout = ResourceLayout::UNDEFINED;
    texture.resourceId = TextureId::Invalid;

    auto databaseIt = renderGraph->resources.textureMetadatas.find(texture.name);
    if (databaseIt != renderGraph->resources.textureMetadatas.end())
    {
      texture.resourceId = databaseIt->second.resourceId;
    }

    if (texture.resourceId == TextureId::Invalid && !texture.textureInfo.isVirtual)
    {
      texture.resourceId = resolveCreatedTextureId(texture.name, texture.textureInfo);
    }

    runTextureCache.insert(
        texture.name,
        TextureRunCacheEntry{
            .resourceId = texture.resourceId,
            .source = texture.resourceId == TextureId::Invalid ? RuntimeResourceSource::Passthrough : RuntimeResourceSource::Created,
        });
  }

  for (auto &bindingGroups : runtimeContext.bindingGroups)
  {
    bindingGroups.resourceId = BindingGroupsId::Invalid;
  }

  for (auto &bindingGroups : runtimeContext.bindingGroups)
  {
    bindingGroups.resourceId = resolveBindingGroups(BindingGroups{.name = bindingGroups.name});
  }
}

void RenderGraphRuntimeResourcesManager::applyOverridesToRuntimeContext()
{
  if (currentOverrides == nullptr)
  {
    return;
  }

  auto &runtimeContext = renderGraph->runtimeContext;

  for (const auto &[name, overrideInfo] : currentOverrides->bufferOverrides)
  {
    auto runtimeIdIt = runtimeContext.bufferNameToRuntimeId.find(name);
    if (runtimeIdIt == runtimeContext.bufferNameToRuntimeId.end())
    {
      continue;
    }

    runtimeContext.buffers[runtimeIdIt->second].resourceId = overrideInfo.bufferId;
    runBufferCache.insert(
        name,
        BufferRunCacheEntry{
            .resourceId = overrideInfo.bufferId,
            .source = RuntimeResourceSource::Override,
        });
  }

  for (const auto &[name, overrideInfo] : currentOverrides->textureOverrides)
  {
    auto runtimeIdIt = runtimeContext.textureNameToRuntimeId.find(name);
    if (runtimeIdIt == runtimeContext.textureNameToRuntimeId.end())
    {
      continue;
    }

    auto &runtimeMetadata = runtimeContext.textures[runtimeIdIt->second];
    runtimeMetadata.resourceId = overrideInfo.textureId;
    runtimeMetadata.overrideLayout = overrideInfo.layout;

    runTextureCache.insert(
        name,
        TextureRunCacheEntry{
            .resourceId = overrideInfo.textureId,
            .source = RuntimeResourceSource::Override,
        });
  }

  applyBindingGroupsOverrides(*currentOverrides);
}

void RenderGraphRuntimeResourcesManager::applyBindingGroupsOverrides(const RenderGraphOverrides &overrides)
{
  auto &runtimeContext = renderGraph->runtimeContext;
  std::unordered_set<uint32_t> touchedBindingGroups;

  for (const auto &[name, _] : overrides.bufferOverrides)
  {
    auto runtimeIdIt = runtimeContext.bufferNameToRuntimeId.find(name);
    if (runtimeIdIt == runtimeContext.bufferNameToRuntimeId.end())
    {
      continue;
    }

    auto refsIt = runtimeContext.bufferToBindingGroupRefs.find(runtimeIdIt->second);
    if (refsIt == runtimeContext.bufferToBindingGroupRefs.end())
    {
      continue;
    }

    for (const auto &ref : refsIt->second)
    {
      touchedBindingGroups.insert(ref.bgRuntimeId);
    }
  }

  for (const auto &[name, _] : overrides.textureOverrides)
  {
    auto runtimeIdIt = runtimeContext.textureNameToRuntimeId.find(name);
    if (runtimeIdIt == runtimeContext.textureNameToRuntimeId.end())
    {
      continue;
    }

    auto refsIt = runtimeContext.textureToBindingGroupRefs.find(runtimeIdIt->second);
    if (refsIt == runtimeContext.textureToBindingGroupRefs.end())
    {
      continue;
    }

    for (const auto &ref : refsIt->second)
    {
      touchedBindingGroups.insert(ref.bgRuntimeId);
    }
  }

  for (const uint32_t bgRuntimeId : touchedBindingGroups)
  {
    auto &bindingGroups = runtimeContext.bindingGroups[bgRuntimeId];
    BindingGroupsInfo resolvedInfo = bindingGroups.groupsInfo;

    for (auto &group : resolvedInfo.groups)
    {
      for (auto &bufferBinding : group.buffers)
      {
        bufferBinding.bufferView = resolveBufferView(bufferBinding.bufferView);
      }

      for (auto &samplerBinding : group.samplers)
      {
        samplerBinding.view = resolveTextureView(samplerBinding.view);
      }

      for (auto &textureBinding : group.textures)
      {
        textureBinding.textureView = resolveTextureView(textureBinding.textureView);
      }

      for (auto &storageTextureBinding : group.storageTextures)
      {
        storageTextureBinding.textureView = resolveTextureView(storageTextureBinding.textureView);
      }
    }

    const BindingGroupsId previousId = bindingGroups.resourceId;
    const BindingGroupsId overrideId = measureMetric(MetricId::ResolveBindingGroupsCreateRHI, [&]() { return renderGraph->rhi->createBindingGroups(resolvedInfo); });

    overrideBindingGroups.push_back({bgRuntimeId, previousId});
    bindingGroups.resourceId = overrideId;
  }
}

void RenderGraphRuntimeResourcesManager::resetMetrics()
{
  for (auto &metric : metricStats)
  {
    metric = MetricStat{};
  }
}

void RenderGraphRuntimeResourcesManager::recordMetric(MetricId id, uint64_t elapsedNs)
{
  MetricStat &metric = metricStats[static_cast<size_t>(id)];
  metric.totalNs += elapsedNs;
  metric.callCount += 1;
  metric.maxNs = std::max(metric.maxNs, elapsedNs);
}

const RenderGraphRuntimeResourcesManager::MetricStatsArray &RenderGraphRuntimeResourcesManager::getMetricStats() const
{
  return metricStats;
}

const char *RenderGraphRuntimeResourcesManager::metricName(MetricId id)
{
  switch (id)
  {
  case MetricId::ResolveBufferTotal:
    return "resolveBuffer.total";
  case MetricId::ResolveBufferOverrideHit:
    return "resolveBuffer.overrideHit";
  case MetricId::ResolveBufferCreatedHit:
    return "resolveBuffer.cacheHit";
  case MetricId::ResolveBufferCreateRHI:
    return "resolveBuffer.createRHI";
  case MetricId::ResolveTextureTotal:
    return "resolveTexture.total";
  case MetricId::ResolveTextureOverrideHit:
    return "resolveTexture.overrideHit";
  case MetricId::ResolveTextureCreatedHit:
    return "resolveTexture.cacheHit";
  case MetricId::ResolveTextureCreateRHI:
    return "resolveTexture.createRHI";
  case MetricId::ResolveRenderPassInfoTotal:
    return "resolveRenderPassInfo.total";
  case MetricId::ResolveBindingGroupsTotal:
    return "resolveBindingGroups.total";
  case MetricId::ResolveBindingGroupsResolveMembers:
    return "resolveBindingGroups.resolveMembers";
  case MetricId::ResolveBindingGroupsCompare:
    return "resolveBindingGroups.compare";
  case MetricId::ResolveBindingGroupsDeletePrevious:
    return "resolveBindingGroups.deletePrevious";
  case MetricId::ResolveBindingGroupsCreateRHI:
    return "resolveBindingGroups.createRHI";
  case MetricId::InvalidateAllBindingGroups:
    return "bindingGroups.invalidateAll";
  case MetricId::ReleaseBuffer:
    return "releaseBuffer";
  case MetricId::ReleaseTexture:
    return "releaseTexture";
  case MetricId::ReleaseBindingGroups:
    return "releaseBindingGroups";
  case MetricId::Count:
    break;
  }

  return "unknownMetric";
}

const RenderGraphBufferOverride *RenderGraphRuntimeResourcesManager::findBufferOverride(const std::string &resourceName) const
{
  if (currentOverrides == nullptr)
  {
    return nullptr;
  }

  auto it = currentOverrides->bufferOverrides.find(resourceName);
  return it == currentOverrides->bufferOverrides.end() ? nullptr : &it->second;
}

const RenderGraphTextureOverride *RenderGraphRuntimeResourcesManager::findTextureOverride(const std::string &resourceName) const
{
  if (currentOverrides == nullptr)
  {
    return nullptr;
  }

  auto it = currentOverrides->textureOverrides.find(resourceName);
  return it == currentOverrides->textureOverrides.end() ? nullptr : &it->second;
}

BufferId RenderGraphRuntimeResourcesManager::resolveBuffer(const Buffer &buffer)
{
  return resolveBufferByName(buffer.name);
}

BufferId RenderGraphRuntimeResourcesManager::resolveBufferByName(const std::string &graphBufferName)
{
  return measureMetric(
      MetricId::ResolveBufferTotal,
      [&]()
      {
        const bool runContextActive = currentOverrides != nullptr;
        if (runContextActive)
        {
          auto runtimeIdIt = renderGraph->runtimeContext.bufferNameToRuntimeId.find(graphBufferName);
          if (runtimeIdIt != renderGraph->runtimeContext.bufferNameToRuntimeId.end())
          {
            const BufferId runtimeId = renderGraph->runtimeContext.buffers[runtimeIdIt->second].resourceId;
            if (runtimeId != BufferId::Invalid)
            {
              return runtimeId;
            }
          }
        }

        auto metadataIt = renderGraph->resources.bufferMetadatas.find(graphBufferName);
        if (metadataIt != renderGraph->resources.bufferMetadatas.end())
        {
          if (metadataIt->second.resourceId == BufferId::Invalid && !metadataIt->second.bufferInfo.isVirtual)
          {
            return resolveCreatedBufferId(graphBufferName, metadataIt->second.bufferInfo);
          }
          return metadataIt->second.resourceId;
        }

        auto cached = runBufferCache.find(graphBufferName);
        return cached ? cached->resourceId : BufferId::Invalid;
      });
}

BufferView RenderGraphRuntimeResourcesManager::resolveBufferView(const BufferView &view)
{
  BufferView resolved = view;
  resolved.resourceId = resolveBuffer(view.buffer);
  return resolved;
}

TextureId RenderGraphRuntimeResourcesManager::resolveTexture(const Texture &texture)
{
  return resolveTextureByName(texture.name);
}

TextureId RenderGraphRuntimeResourcesManager::resolveTextureByName(const std::string &graphTextureName)
{
  return measureMetric(
      MetricId::ResolveTextureTotal,
      [&]()
      {
        const bool runContextActive = currentOverrides != nullptr;
        if (runContextActive)
        {
          auto runtimeIdIt = renderGraph->runtimeContext.textureNameToRuntimeId.find(graphTextureName);
          if (runtimeIdIt != renderGraph->runtimeContext.textureNameToRuntimeId.end())
          {
            const TextureId runtimeId = renderGraph->runtimeContext.textures[runtimeIdIt->second].resourceId;
            if (runtimeId != TextureId::Invalid)
            {
              return runtimeId;
            }
          }
        }

        auto metadataIt = renderGraph->resources.textureMetadatas.find(graphTextureName);
        if (metadataIt != renderGraph->resources.textureMetadatas.end())
        {
          if (metadataIt->second.resourceId == TextureId::Invalid && !metadataIt->second.textureInfo.isVirtual)
          {
            return resolveCreatedTextureId(graphTextureName, metadataIt->second.textureInfo);
          }
          return metadataIt->second.resourceId;
        }

        auto cached = runTextureCache.find(graphTextureName);
        return cached ? cached->resourceId : TextureId::Invalid;
      });
}

TextureView RenderGraphRuntimeResourcesManager::resolveTextureView(const TextureView &view)
{
  TextureView resolved = view;
  resolved.resourceId = resolveTexture(view.texture);
  return resolved;
}

RenderPassInfo RenderGraphRuntimeResourcesManager::resolveRenderPassInfo(const RenderPassInfo &info)
{
  return measureMetric(
      MetricId::ResolveRenderPassInfoTotal,
      [&]()
      {
        RenderPassInfo resolved = info;

        for (auto &colorAttachment : resolved.colorAttachments)
        {
          colorAttachment.view = resolveTextureView(colorAttachment.view);
        }

        if (resolved.depthStencilAttachment.enabled)
        {
          resolved.depthStencilAttachment.view = resolveTextureView(resolved.depthStencilAttachment.view);
        }

        return resolved;
      });
}

bool RenderGraphRuntimeResourcesManager::bindingGroupsInfoEquals(const BindingGroupsInfo &a, const BindingGroupsInfo &b) const
{
  if (a.name != b.name || a.layout != b.layout || a.groups.size() != b.groups.size())
  {
    return false;
  }

  for (size_t i = 0; i < a.groups.size(); ++i)
  {
    const auto &groupA = a.groups[i];
    const auto &groupB = b.groups[i];

    if (groupA.name != groupB.name || groupA.buffers.size() != groupB.buffers.size() || groupA.samplers.size() != groupB.samplers.size() || groupA.textures.size() != groupB.textures.size() ||
        groupA.storageTextures.size() != groupB.storageTextures.size())
    {
      return false;
    }

    for (size_t j = 0; j < groupA.buffers.size(); ++j)
    {
      if (groupA.buffers[j].binding != groupB.buffers[j].binding || groupA.buffers[j].bufferView != groupB.buffers[j].bufferView)
      {
        return false;
      }
    }

    for (size_t j = 0; j < groupA.samplers.size(); ++j)
    {
      if (groupA.samplers[j].binding != groupB.samplers[j].binding || groupA.samplers[j].sampler != groupB.samplers[j].sampler || groupA.samplers[j].view != groupB.samplers[j].view)
      {
        return false;
      }
    }

    for (size_t j = 0; j < groupA.textures.size(); ++j)
    {
      if (groupA.textures[j].binding != groupB.textures[j].binding || groupA.textures[j].textureView != groupB.textures[j].textureView)
      {
        return false;
      }
    }

    for (size_t j = 0; j < groupA.storageTextures.size(); ++j)
    {
      if (groupA.storageTextures[j].binding != groupB.storageTextures[j].binding || groupA.storageTextures[j].textureView != groupB.storageTextures[j].textureView)
      {
        return false;
      }
    }
  }

  return true;
}

BindingGroupsId RenderGraphRuntimeResourcesManager::resolveBindingGroups(const BindingGroups &groups)
{
  return measureMetric(
      MetricId::ResolveBindingGroupsTotal,
      [&]()
      {
        auto preparedBindingGroups = preparedBindingGroupsCache.find(groups.name);
        if (preparedBindingGroups)
        {
          return *preparedBindingGroups;
        }

        auto bindingGroupsMetadata = renderGraph->resources.bindingGroupsMetadata.find(groups.name);
        if (bindingGroupsMetadata == renderGraph->resources.bindingGroupsMetadata.end())
        {
          return BindingGroupsId::Invalid;
        }

        BindingGroupsInfo resolvedInfo = bindingGroupsMetadata->second.groupsInfo;

        measureMetric(
            MetricId::ResolveBindingGroupsResolveMembers,
            [&]()
            {
              for (auto &groupInfo : resolvedInfo.groups)
              {
                for (auto &bufferBinding : groupInfo.buffers)
                {
                  bufferBinding.bufferView = resolveBufferView(bufferBinding.bufferView);
                }

                for (auto &samplerBinding : groupInfo.samplers)
                {
                  samplerBinding.view = resolveTextureView(samplerBinding.view);
                }

                for (auto &textureBinding : groupInfo.textures)
                {
                  textureBinding.textureView = resolveTextureView(textureBinding.textureView);
                }

                for (auto &storageTextureBinding : groupInfo.storageTextures)
                {
                  storageTextureBinding.textureView = resolveTextureView(storageTextureBinding.textureView);
                }
              }
            });

        auto existingBindingGroups = bindingGroupsMappings.find(groups.name);
        if (existingBindingGroups)
        {
          for (const auto &variant : *existingBindingGroups)
          {
            const bool sameBindingGroupsInfo = measureMetric(
                MetricId::ResolveBindingGroupsCompare,
                [&]()
                {
                  return bindingGroupsInfoEquals(variant.resolvedInfo, resolvedInfo);
                });
            if (!sameBindingGroupsInfo)
            {
              continue;
            }

            preparedBindingGroupsCache.insert(groups.name, variant.resourceId);

            auto runtimeIdIt = renderGraph->runtimeContext.bindingGroupsNameToRuntimeId.find(groups.name);
            if (runtimeIdIt != renderGraph->runtimeContext.bindingGroupsNameToRuntimeId.end())
            {
              renderGraph->runtimeContext.bindingGroups[runtimeIdIt->second].resourceId = variant.resourceId;
            }

            return variant.resourceId;
          }
        }

        const BindingGroupsId createdBindingGroups = measureMetric(MetricId::ResolveBindingGroupsCreateRHI, [&]() { return renderGraph->rhi->createBindingGroups(resolvedInfo); });
        auto variants = bindingGroupsMappings.find(groups.name);
        if (!variants)
        {
          bindingGroupsMappings.insert(groups.name, std::vector<BindingGroupsVariant>{});
          variants = bindingGroupsMappings.find(groups.name);
        }
        variants->push_back(BindingGroupsVariant{
            .resourceId = createdBindingGroups,
            .resolvedInfo = resolvedInfo,
        });
        preparedBindingGroupsCache.insert(groups.name, createdBindingGroups);

        auto createdRuntimeIdIt = renderGraph->runtimeContext.bindingGroupsNameToRuntimeId.find(groups.name);
        if (createdRuntimeIdIt != renderGraph->runtimeContext.bindingGroupsNameToRuntimeId.end())
        {
          renderGraph->runtimeContext.bindingGroups[createdRuntimeIdIt->second].resourceId = createdBindingGroups;
        }

        return createdBindingGroups;
      });
}

ResourceLayout RenderGraphRuntimeResourcesManager::resolveTextureBarrierFromLayout(const std::string &graphTextureName, ResourceLayout fallbackLayout) const
{
  auto runtimeIdIt = renderGraph->runtimeContext.textureNameToRuntimeId.find(graphTextureName);
  if (runtimeIdIt == renderGraph->runtimeContext.textureNameToRuntimeId.end())
  {
    return fallbackLayout;
  }

  const auto &runtimeMetadata = renderGraph->runtimeContext.textures[runtimeIdIt->second];
  return runtimeMetadata.overrideLayout != ResourceLayout::UNDEFINED ? runtimeMetadata.overrideLayout : fallbackLayout;
}

Format RenderGraphRuntimeResourcesManager::getTextureFormatForGraphResource(const std::string &graphTextureName)
{
  auto cachedFormat = runTextureFormatCache.find(graphTextureName);
  if (cachedFormat)
  {
    return *cachedFormat;
  }

  auto graphTextureMetadata = renderGraph->resources.textureMetadatas.find(graphTextureName);
  if (graphTextureMetadata != renderGraph->resources.textureMetadatas.end())
  {
    runTextureFormatCache.insert(graphTextureName, graphTextureMetadata->second.textureInfo.format);
    return graphTextureMetadata->second.textureInfo.format;
  }

  std::ostringstream stream;
  stream << "[RenderGraphRuntimeResourcesManager] Texture format for resource '" << graphTextureName << "' not found in RenderGraph metadata";
  throw std::runtime_error(stream.str());
}

void RenderGraphRuntimeResourcesManager::invalidateAllBindingGroups()
{
  measureMetric(
      MetricId::InvalidateAllBindingGroups,
      [&]()
      {
        bindingGroupsMappings.forEach([&](const std::string &, std::vector<BindingGroupsVariant> &variants)
        {
          for (const auto &variant : variants)
          {
            if (variant.resourceId != BindingGroupsId::Invalid)
            {
              renderGraph->rhi->deleteBindingGroups(variant.resourceId);
            }
          }
        });

        bindingGroupsMappings.clear();
        preparedBindingGroupsCache.clear();
      });
}

void RenderGraphRuntimeResourcesManager::releaseBuffer(const std::string &graphBufferName)
{
  measureMetric(
      MetricId::ReleaseBuffer,
      [&]()
      {
        auto created = createdBuffers.find(graphBufferName);
        if (created)
        {
          for (const BufferId bufferId : *created)
          {
            if (bufferId != BufferId::Invalid)
            {
              renderGraph->rhi->deleteBuffer(bufferId);
            }
          }
          createdBuffers.erase(graphBufferName);
        }

        runBufferCache.erase(graphBufferName);
        invalidateAllBindingGroups();
      });
}

void RenderGraphRuntimeResourcesManager::releaseTexture(const std::string &graphTextureName)
{
  measureMetric(
      MetricId::ReleaseTexture,
      [&]()
      {
        auto created = createdTextures.find(graphTextureName);
        if (created)
        {
          for (const TextureId textureId : *created)
          {
            if (textureId != TextureId::Invalid)
            {
              renderGraph->rhi->deleteTexture(textureId);
            }
          }
          createdTextures.erase(graphTextureName);
        }

        runTextureCache.erase(graphTextureName);
        runTextureFormatCache.erase(graphTextureName);
        invalidateAllBindingGroups();
      });
}

void RenderGraphRuntimeResourcesManager::releaseBindingGroups(const std::string &bindingGroupsName)
{
  measureMetric(
      MetricId::ReleaseBindingGroups,
      [&]()
      {
        auto bindingGroups = bindingGroupsMappings.find(bindingGroupsName);
        if (bindingGroups)
        {
          for (const auto &variant : *bindingGroups)
          {
            if (variant.resourceId != BindingGroupsId::Invalid)
            {
              renderGraph->rhi->deleteBindingGroups(variant.resourceId);
            }
          }
          bindingGroupsMappings.erase(bindingGroupsName);
        }
        preparedBindingGroupsCache.erase(bindingGroupsName);
      });
}

} // namespace rendering
