#pragma once

#include "datastructure/FlatMap.hpp"
#include "rendering/gpu/Types.hpp"
#include "time/TimeSpan.hpp"

#include <array>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace rendering
{

class RenderGraph;
struct RenderGraphBufferOverride;
struct RenderGraphTextureOverride;
struct RenderGraphOverrides;

class RenderGraphRuntimeResourcesManager
{
public:
  struct RetainedResources
  {
    std::vector<BindingGroupsId> bindingGroups;
  };

  enum class MetricId : uint32_t
  {
    ResolveBufferTotal = 0,
    ResolveBufferOverrideHit,
    ResolveBufferCreatedHit,
    ResolveBufferCreateRHI,
    ResolveTextureTotal,
    ResolveTextureOverrideHit,
    ResolveTextureCreatedHit,
    ResolveTextureCreateRHI,
    ResolveRenderPassInfoTotal,
    ResolveBindingGroupsTotal,
    ResolveBindingGroupsResolveMembers,
    ResolveBindingGroupsCompare,
    ResolveBindingGroupsDeletePrevious,
    ResolveBindingGroupsCreateRHI,
    InvalidateAllBindingGroups,
    ReleaseBuffer,
    ReleaseTexture,
    ReleaseBindingGroups,
    Count
  };

  struct MetricStat
  {
    uint64_t totalNs = 0;
    uint64_t callCount = 0;
    uint64_t maxNs = 0;
  };

  using MetricStatsArray = std::array<MetricStat, static_cast<size_t>(MetricId::Count)>;

  explicit RenderGraphRuntimeResourcesManager(RenderGraph &renderGraph);

  void beginRun(const RenderGraphOverrides &overrides, uint32_t frameIndex);
  RetainedResources detachRunResources();
  void endRun();

  BufferId resolveBuffer(const Buffer &buffer);
  BufferId resolveBufferByName(const std::string &graphBufferName);
  BufferView resolveBufferView(const BufferView &view);

  TextureId resolveTexture(const Texture &texture);
  TextureId resolveTextureByName(const std::string &graphTextureName);
  TextureView resolveTextureView(const TextureView &view);

  RenderPassInfo resolveRenderPassInfo(const RenderPassInfo &info);

  BindingGroupsId resolveBindingGroups(const BindingGroups &groups);

  ResourceLayout resolveTextureBarrierFromLayout(const std::string &graphTextureName, ResourceLayout fallbackLayout) const;
  Format getTextureFormatForGraphResource(const std::string &graphTextureName);

  void releaseBuffer(const std::string &graphBufferName);
  void releaseTexture(const std::string &graphTextureName);
  void releaseBindingGroups(const std::string &bindingGroupsName);

  const MetricStatsArray &getMetricStats() const;
  static const char *metricName(MetricId id);

private:
  enum class RuntimeResourceSource : uint8_t
  {
    Passthrough = 0,
    Override,
    Created,
  };

  struct BufferRunCacheEntry
  {
    BufferId resourceId = BufferId::Invalid;
    RuntimeResourceSource source = RuntimeResourceSource::Passthrough;
  };

  struct TextureRunCacheEntry
  {
    TextureId resourceId = TextureId::Invalid;
    RuntimeResourceSource source = RuntimeResourceSource::Passthrough;
  };

  RenderGraph *renderGraph = nullptr;
  const RenderGraphOverrides *currentOverrides = nullptr;
  uint32_t currentFrameIndex = 0u;
  MetricStatsArray metricStats = {};

  lib::FlatMap<std::string, BufferRunCacheEntry> runBufferCache;
  lib::FlatMap<std::string, TextureRunCacheEntry> runTextureCache;
  lib::FlatMap<std::string, Format> runTextureFormatCache;

  lib::FlatMap<std::string, std::vector<BufferId>> createdBuffers;
  lib::FlatMap<std::string, std::vector<TextureId>> createdTextures;

  struct BindingGroupsVariant
  {
    BindingGroupsId resourceId = BindingGroupsId::Invalid;
    BindingGroupsInfo resolvedInfo;
  };

  lib::FlatMap<std::string, std::vector<BindingGroupsVariant>> bindingGroupsMappings;
  lib::FlatMap<std::string, BindingGroupsId> preparedBindingGroupsCache;
  std::vector<std::pair<uint32_t, BindingGroupsId>> overrideBindingGroups;

  const RenderGraphBufferOverride *findBufferOverride(const std::string &resourceName) const;
  const RenderGraphTextureOverride *findTextureOverride(const std::string &resourceName) const;

  void prepareBaseRuntimeResources();
  void applyOverridesToRuntimeContext();
  void applyBindingGroupsOverrides(const RenderGraphOverrides &overrides);

  bool bindingGroupsInfoEquals(const BindingGroupsInfo &a, const BindingGroupsInfo &b) const;
  uint32_t getFrameSlot() const;
  BufferId resolveCreatedBufferId(const std::string &graphBufferName, const BufferInfo &bufferInfo);
  TextureId resolveCreatedTextureId(const std::string &graphTextureName, const TextureInfo &textureInfo);

  void invalidateAllBindingGroups();
  void resetMetrics();
  void recordMetric(MetricId id, uint64_t elapsedNs);

  template <typename Fn> auto measureMetric(MetricId id, Fn &&fn)
  {
    const lib::time::TimeSpan start = lib::time::TimeSpan::now();

    if constexpr (std::is_void_v<std::invoke_result_t<Fn>>)
    {
      std::forward<Fn>(fn)();
      const lib::time::TimeSpan end = lib::time::TimeSpan::now();
      recordMetric(id, static_cast<uint64_t>((end - start).nanoseconds()));
      return;
    }
    else
    {
      auto result = std::forward<Fn>(fn)();
      const lib::time::TimeSpan end = lib::time::TimeSpan::now();
      recordMetric(id, static_cast<uint64_t>((end - start).nanoseconds()));
      return result;
    }
  }
};

} // namespace rendering
