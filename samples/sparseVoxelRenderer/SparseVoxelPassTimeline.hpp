#pragma once

#include "rendering/gpu/RenderGraph.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sparse_voxel_sample
{
class PassTimelineHistory
{
public:
  static constexpr size_t kSampleCount = 180u;

  enum class Filter
  {
    Compute,
    Graphics,
  };

  struct Series
  {
    std::string name;
    uint32_t passIndex = 0u;
    ImU32 color = IM_COL32_WHITE;
    std::array<float, kSampleCount> values{};
    std::array<uint32_t, kSampleCount> timerCounts{};
    bool hasComputeWork = false;
    bool hasGraphicsWork = false;
  };

  void pushFrame(
      const uint32_t frameIndex,
      const std::vector<rendering::RenderGraph::TimerReadbackRecord> &records,
      const double timestampSpanMs = 0.0)
  {
    if (hasFrames_ && lastFrameIndex_ == frameIndex)
      return;
    hasFrames_ = true;
    lastFrameIndex_ = frameIndex;
    frameIndices_[nextSampleIndex_] = frameIndex;
    timestampSpans_[nextSampleIndex_] =
        static_cast<float>(std::max(0.0, timestampSpanMs));
    for (Series &entry : series_)
    {
      entry.values[nextSampleIndex_] = 0.0f;
      entry.timerCounts[nextSampleIndex_] = 0u;
    }
    for (const auto &record : records)
    {
      Series &entry = ensureSeries(record);
      entry.values[nextSampleIndex_] += static_cast<float>(record.gpuTimeMs);
      entry.timerCounts[nextSampleIndex_] += 1u;
      entry.hasComputeWork = entry.hasComputeWork || record.hasComputeWork;
      entry.hasGraphicsWork = entry.hasGraphicsWork || record.hasGraphicsWork;
    }
    nextSampleIndex_ = (nextSampleIndex_ + 1u) % kSampleCount;
    count_ = std::min(count_ + 1u, kSampleCount);
    ++totalCount_;
  }

  size_t count() const { return count_; }
  uint64_t totalCount() const { return totalCount_; }
  const std::vector<Series> &series() const { return series_; }

  float sampleValue(const Series &entry, const size_t orderedSampleIndex) const
  {
    return orderedSampleIndex < count_ ? entry.values[storageIndex(orderedSampleIndex)] : 0.0f;
  }

  uint32_t sampleTimerCount(const Series &entry, const size_t orderedSampleIndex) const
  {
    return orderedSampleIndex < count_ ? entry.timerCounts[storageIndex(orderedSampleIndex)] : 0u;
  }

  uint32_t frameIndex(const size_t orderedSampleIndex) const
  {
    return orderedSampleIndex < count_ ? frameIndices_[storageIndex(orderedSampleIndex)] : 0u;
  }

  float timestampSpanMs(const size_t orderedSampleIndex) const
  {
    return orderedSampleIndex < count_
        ? timestampSpans_[storageIndex(orderedSampleIndex)]
        : 0.0f;
  }

  float totalMs(const size_t orderedSampleIndex) const
  {
    float total = 0.0f;
    for (const Series &entry : series_)
      total += sampleValue(entry, orderedSampleIndex);
    return total;
  }

  float totalMs(const Filter filter, const size_t orderedSampleIndex) const
  {
    float total = 0.0f;
    for (const Series &entry : series_)
      if (matches(entry, filter))
        total += sampleValue(entry, orderedSampleIndex);
    return total;
  }

  std::vector<const Series *> sortedForSample(const Filter filter, const size_t orderedSampleIndex) const
  {
    std::vector<const Series *> result;
    for (const Series &entry : series_)
      if (matches(entry, filter))
        result.push_back(&entry);
    std::sort(result.begin(), result.end(), [&](const Series *left, const Series *right) {
      const float leftMs = sampleValue(*left, orderedSampleIndex);
      const float rightMs = sampleValue(*right, orderedSampleIndex);
      if (leftMs != rightMs)
        return leftMs < rightMs;
      return left->passIndex < right->passIndex;
    });
    return result;
  }

private:
  static ImU32 colorFor(const std::string &name, const uint32_t passIndex)
  {
    const size_t hash = std::hash<std::string>{}(name) ^ (static_cast<size_t>(passIndex) * 0x9e3779b97f4a7c15ull);
    const float hue = static_cast<float>(hash % 360u) / 360.0f;
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    ImGui::ColorConvertHSVtoRGB(hue, 0.65f, 0.95f, red, green, blue);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(red, green, blue, 1.0f));
  }

  Series &ensureSeries(const rendering::RenderGraph::TimerReadbackRecord &record)
  {
    const auto [it, inserted] = indices_.emplace(record.passName, series_.size());
    if (inserted)
      series_.push_back({.name = record.passName, .passIndex = record.passIndex, .color = colorFor(record.passName, record.passIndex)});
    Series &entry = series_[it->second];
    entry.passIndex = record.passIndex;
    return entry;
  }

  static bool matches(const Series &entry, const Filter filter)
  {
    return filter == Filter::Compute ? entry.hasComputeWork : entry.hasGraphicsWork;
  }

  size_t storageIndex(const size_t orderedSampleIndex) const
  {
    return (nextSampleIndex_ + kSampleCount - count_ + orderedSampleIndex) % kSampleCount;
  }

  std::vector<Series> series_;
  std::unordered_map<std::string, size_t> indices_;
  std::array<uint32_t, kSampleCount> frameIndices_{};
  std::array<float, kSampleCount> timestampSpans_{};
  size_t nextSampleIndex_ = 0u;
  size_t count_ = 0u;
  uint64_t totalCount_ = 0u;
  uint32_t lastFrameIndex_ = 0u;
  bool hasFrames_ = false;
};

inline const char *stageLabel(const PassTimelineHistory::Series &entry)
{
  if (entry.hasComputeWork && entry.hasGraphicsWork)
    return "mixed";
  return entry.hasComputeWork ? "compute" : (entry.hasGraphicsWork ? "graphics" : "none");
}

inline const char *phaseLabel(const std::string &name)
{
  if (name.find("svr.sample.Pre.") != std::string::npos)
    return "Temporal raster";
  if (name.find("ProvisionalHZB") != std::string::npos)
    return "Provisional HZB";
  if (name.find("FinalHZB") != std::string::npos)
    return "Final HZB";
  if (name.find("svr.sample.Final.") != std::string::npos || name.find("svr.sample.GBuffer") != std::string::npos || name.find("WriteVoxelDepth") != std::string::npos)
    return "Actual scene";
  if (name.find("Copy") != std::string::npos)
    return "Readback";
  return "Present / other";
}

inline constexpr std::array<const char *, 6u> kActualSceneStageLabels{
  "Current-frame TLAS frustum + HZB culling",
  "BLAS / octree LOD traversal",
  "Visible voxel projection",
  "Tile bucket count + scatter",
  "Solid-voxel ray-box resolve",
  "Final G-buffer / depth-only output",
};

inline int actualSceneStageIndex(const std::string &name)
{
  if (name.find("svr.sample.Final.TLAS") != std::string::npos)
    return 0;
  if (name.find("svr.sample.Final.LOD") != std::string::npos)
    return 1;
  if (name.find("svr.sample.Final.Preprocess") != std::string::npos ||
      name.find("svr.sample.Final.ScanBlocks") != std::string::npos ||
      name.find("svr.sample.Final.ScanBlockSums") != std::string::npos ||
      name.find("svr.sample.Final.ScanAdd") != std::string::npos ||
      name.find("svr.sample.Final.Emit") != std::string::npos)
    return 2;
  if (name.find("svr.sample.Final.ClearBuckets") != std::string::npos ||
      name.find("svr.sample.Final.CountBuckets") != std::string::npos ||
      // Matches every stage of the parallel bucket prefix scan.
      name.find("svr.sample.Final.PrefixBucket") != std::string::npos ||
      name.find("svr.sample.Final.ScatterBuckets") != std::string::npos ||
      name.find("svr.sample.Final.Radix") != std::string::npos ||
      name.find("svr.sample.Final.ClearRanges") != std::string::npos ||
      name.find("svr.sample.Final.BuildRanges") != std::string::npos ||
      // The brick resolver's cull is its binning equivalent: it is what turns
      // the scene into the per-draw work list.
      name.find("svr.sample.Final.BrickReset") != std::string::npos ||
      name.find("svr.sample.Final.BrickCull") != std::string::npos ||
      name.find("svr.sample.Final.BrickDrawArgs") != std::string::npos ||
      (name.find("svr.sample.Final.BrickPhase") != std::string::npos &&
       (name.find(".Reset") != std::string::npos ||
        name.find(".Cull") != std::string::npos ||
        name.find(".CommitLODHistory") != std::string::npos ||
        name.find(".DrawArgs") != std::string::npos)) ||
      // R1/R2 prepare the Phase0 visible list into residual occupancy and
      // compact parent records. These production kernels are the brick
      // resolver's bucket/scatter equivalent. Keep the optional
      // MicroPrepareScan out: it is diagnostic sizing work and must never enter
      // performance totals.
      name.find("svr.sample.Final.BrickPhase0.MicroPrepareReset") !=
          std::string::npos ||
      name.find("svr.sample.Final.BrickPhase0.MicroPrepareTransform") !=
          std::string::npos ||
      name.find("svr.sample.Final.BrickCopy") != std::string::npos)
    return 3;
  // Stage 4 is "resolve the primary visibility", whichever resolver does it.
  if (name.find("svr.sample.Final.Resolve") != std::string::npos ||
      name.find("svr.sample.Final.Tree64Primary") != std::string::npos ||
      name.find("svr.sample.Final.BrickDraw_") != std::string::npos ||
      name.find("svr.sample.Final.BrickPhase0.Draw") != std::string::npos ||
      name.find("svr.sample.Final.BrickPhase0.ResidualDraw") !=
          std::string::npos ||
      name.find("svr.sample.Final.BrickPhase0.PreparedMicroDraw") !=
          std::string::npos ||
      name.find("svr.sample.Final.BrickPhase0.PreparedTieReplayDraw") !=
          std::string::npos ||
      name.find("svr.sample.Final.BrickPhase1.Draw") != std::string::npos ||
      name.find("svr.sample.Final.BrickResolve") != std::string::npos)
    return 4;
  if ((name.find("svr.sample.GBuffer") != std::string::npos && name.find("Signature") == std::string::npos) ||
      name.find("svr.sample.WriteVoxelDepth") != std::string::npos)
    return 5;
  return -1;
}

inline std::array<float, kActualSceneStageLabels.size()> actualSceneStageTotals(
    const PassTimelineHistory &history,
    const size_t sampleIndex)
{
  std::array<float, kActualSceneStageLabels.size()> totals{};
  for (const auto &entry : history.series())
  {
    const int stageIndex = actualSceneStageIndex(entry.name);
    if (stageIndex >= 0)
      totals[static_cast<size_t>(stageIndex)] += history.sampleValue(entry, sampleIndex);
  }
  return totals;
}

inline void drawPassGraph(
    const char *id,
    const PassTimelineHistory &history,
    const PassTimelineHistory::Filter filter,
    const size_t visibleFrameCount,
    const size_t selectedOffset,
    const ImVec2 requestedSize)
{
  ImVec2 size = requestedSize;
  if (size.x <= 0.0f)
    size.x = std::max(1.0f, ImGui::GetContentRegionAvail().x);
  if (size.y <= 0.0f)
    size.y = 190.0f;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, size);
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 maximum(origin.x + size.x, origin.y + size.y);
  draw->AddRectFilled(origin, maximum, IM_COL32(14, 18, 24, 220), 6.0f);
  draw->AddRect(origin, maximum, IM_COL32(90, 98, 110, 255), 6.0f);
  if (history.count() == 0u)
  {
    draw->AddText(ImVec2(origin.x + 10.0f, origin.y + 10.0f), IM_COL32(210, 215, 220, 255), "No GPU timer samples yet");
    return;
  }

  const float left = 54.0f;
  const float right = 8.0f;
  const float top = 10.0f;
  const float bottom = 20.0f;
  const ImVec2 plotMinimum(origin.x + left, origin.y + top);
  const ImVec2 plotMaximum(maximum.x - right, maximum.y - bottom);
  const size_t visibleCount = std::min(history.count(), std::max<size_t>(1u, visibleFrameCount));
  const size_t firstSample = history.count() - visibleCount;
  float maximumMs = 0.0f;
  for (size_t sample = 0u; sample < visibleCount; ++sample)
    maximumMs = std::max(maximumMs, history.totalMs(filter, firstSample + sample));
  maximumMs = std::max(0.1f, maximumMs * 1.1f);
  for (uint32_t grid = 0u; grid < 5u; ++grid)
  {
    const float fraction = static_cast<float>(grid) / 4.0f;
    const float y = plotMaximum.y + (plotMinimum.y - plotMaximum.y) * fraction;
    draw->AddLine(ImVec2(plotMinimum.x, y), ImVec2(plotMaximum.x, y), IM_COL32(58, 66, 78, 200));
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", maximumMs * fraction);
    draw->AddText(ImVec2(origin.x + 5.0f, y - 7.0f), IM_COL32(180, 188, 198, 255), text);
  }

  const size_t selectedLocal = visibleCount - 1u - std::min(selectedOffset, visibleCount - 1u);
  const float selectedX = visibleCount > 1u
                              ? plotMinimum.x + (plotMaximum.x - plotMinimum.x) * static_cast<float>(selectedLocal) / static_cast<float>(visibleCount - 1u)
                              : plotMinimum.x;
  draw->AddLine(ImVec2(selectedX, plotMinimum.y), ImVec2(selectedX, plotMaximum.y), IM_COL32(235, 235, 240, 130));

  const size_t selectedOrdered = firstSample + selectedLocal;
  const auto series = history.sortedForSample(filter, selectedOrdered);
  std::array<float, PassTimelineHistory::kSampleCount> cumulative{};
  draw->PushClipRect(plotMinimum, plotMaximum, true);
  for (const auto *entry : series)
  {
    ImVec2 previous{};
    bool hasPrevious = false;
    bool hasWork = false;
    for (size_t sample = 0u; sample < visibleCount; ++sample)
    {
      const float value = history.sampleValue(*entry, firstSample + sample);
      cumulative[sample] += value;
      hasWork = hasWork || value > 0.0f;
      const float x = visibleCount > 1u
                          ? plotMinimum.x + (plotMaximum.x - plotMinimum.x) * static_cast<float>(sample) / static_cast<float>(visibleCount - 1u)
                          : plotMinimum.x;
      const float y = plotMaximum.y - (plotMaximum.y - plotMinimum.y) * std::clamp(cumulative[sample] / maximumMs, 0.0f, 1.0f);
      const ImVec2 point(x, y);
      if (hasPrevious && hasWork)
        draw->AddLine(previous, point, entry->color, 1.5f);
      previous = point;
      hasPrevious = true;
    }
  }
  draw->PopClipRect();
  draw->AddText(ImVec2(plotMinimum.x, maximum.y - 17.0f), IM_COL32(155, 162, 172, 255), "oldest");
  draw->AddText(ImVec2(plotMaximum.x - 45.0f, maximum.y - 17.0f), IM_COL32(155, 162, 172, 255), "newest");
}

inline void drawPassTimelineOverlay(PassTimelineHistory &history, bool *open)
{
  if (open == nullptr || !*open)
    return;
  ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 760.0f, viewport->WorkPos.y + 16.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(744.0f, std::min(820.0f, viewport->WorkSize.y - 32.0f)), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.88f);
  if (!ImGui::Begin("Voxel Pass Timeline", open, ImGuiWindowFlags_NoFocusOnAppearing))
  {
    ImGui::End();
    return;
  }

  static int visibleFrames = 60;
  static int selectedOffset = 0;
  const int available = static_cast<int>(std::max<size_t>(1u, history.count()));
  visibleFrames = std::clamp(visibleFrames, 1, available);
  selectedOffset = std::clamp(selectedOffset, 0, visibleFrames - 1);
  ImGui::Text("Per-pass GPU history over %zu frames", history.count());
  ImGui::TextDisabled("Timestamp resolve/readback is active only while this window is open");
  ImGui::SliderInt("Visible frames", &visibleFrames, 1, available);
  ImGui::SliderInt("Inspect N-", &selectedOffset, 0, std::max(0, visibleFrames - 1));

  size_t selectedSample = 0u;
  if (history.count() != 0u)
  {
    selectedSample = history.count() - 1u - static_cast<size_t>(selectedOffset);
    ImGui::Text(
        "Frame %u   Timed pass sum %.3f ms   GPU span %.3f ms",
        history.frameIndex(selectedSample), history.totalMs(selectedSample),
        history.timestampSpanMs(selectedSample));
    ImGui::Text(
        "Compute %.3f ms   Graphics %.3f ms",
        history.totalMs(PassTimelineHistory::Filter::Compute, selectedSample),
        history.totalMs(PassTimelineHistory::Filter::Graphics, selectedSample));
    ImGui::TextDisabled(
        "GPU span includes barriers and queue gaps omitted by the pass sum");
  }

  ImGui::TextUnformatted("Compute passes");
  drawPassGraph("##VoxelComputeTimeline", history, PassTimelineHistory::Filter::Compute, visibleFrames, selectedOffset, ImVec2(-1.0f, 190.0f));
  ImGui::TextUnformatted("Graphics passes");
  drawPassGraph("##VoxelGraphicsTimeline", history, PassTimelineHistory::Filter::Graphics, visibleFrames, selectedOffset, ImVec2(-1.0f, 150.0f));

  if (history.count() != 0u)
  {
    struct Cost
    {
      const PassTimelineHistory::Series *entry = nullptr;
      float milliseconds = 0.0f;
    };
    std::vector<Cost> costs;
    std::unordered_map<std::string, float> phaseTotals;
    for (const auto &entry : history.series())
    {
      const float milliseconds = history.sampleValue(entry, selectedSample);
      if (milliseconds > 0.0f)
      {
        costs.push_back({&entry, milliseconds});
        phaseTotals[phaseLabel(entry.name)] += milliseconds;
      }
    }
    std::sort(costs.begin(), costs.end(), [](const Cost &left, const Cost &right) { return left.milliseconds > right.milliseconds; });

    if (ImGui::BeginTable("VoxelPhaseTotals", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
    {
      ImGui::TableSetupColumn("Phase");
      ImGui::TableSetupColumn("GPU time");
      ImGui::TableHeadersRow();
      for (const char *phase : {"Temporal raster", "Provisional HZB", "Actual scene", "Final HZB", "Readback", "Present / other"})
      {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(phase);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%.3f ms", phaseTotals[phase]);
      }
      ImGui::EndTable();
    }

    const auto actualStageTotals = actualSceneStageTotals(history, selectedSample);
    ImGui::TextUnformatted("Actual-scene stage timers");
    if (ImGui::BeginTable("VoxelActualSceneStageTotals", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
    {
      ImGui::TableSetupColumn("Stage");
      ImGui::TableSetupColumn("GPU time");
      ImGui::TableHeadersRow();
      for (size_t stageIndex = 0u; stageIndex < kActualSceneStageLabels.size(); ++stageIndex)
      {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(kActualSceneStageLabels[stageIndex]);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%.3f ms", actualStageTotals[stageIndex]);
      }
      ImGui::EndTable();
    }

    const ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("VoxelPassCosts", 5, tableFlags, ImVec2(0.0f, 220.0f)))
    {
      ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 85.0f);
      ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_WidthFixed, 65.0f);
      ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthFixed, 75.0f);
      ImGui::TableSetupColumn("Phase", ImGuiTableColumnFlags_WidthFixed, 120.0f);
      ImGui::TableSetupColumn("Pass");
      ImGui::TableHeadersRow();
      const float total = history.totalMs(selectedSample);
      for (const Cost &cost : costs)
      {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%.3f ms", cost.milliseconds);
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%.1f%%", total > 0.0f ? cost.milliseconds * 100.0f / total : 0.0f);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(stageLabel(*cost.entry));
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(phaseLabel(cost.entry->name));
        ImGui::TableSetColumnIndex(4);
        ImGui::TextUnformatted(cost.entry->name.c_str());
      }
      ImGui::EndTable();
    }
  }
  ImGui::End();
}

struct CpuSectionTiming
{
  const char *name = "";
  uint32_t order = 0u;
  float milliseconds = 0.0f;
};

class CpuTimelineHistory
{
public:
  static constexpr size_t kSampleCount = 180u;
  struct Series
  {
    std::string name;
    uint32_t order = 0u;
    ImU32 color = IM_COL32_WHITE;
    std::array<float, kSampleCount> values{};
  };

  void pushFrame(const uint32_t frameIndex, const std::vector<CpuSectionTiming> &sections)
  {
    frameIndices_[nextSampleIndex_] = frameIndex;
    for (Series &entry : series_)
      entry.values[nextSampleIndex_] = 0.0f;
    for (const CpuSectionTiming &section : sections)
    {
      const auto [it, inserted] = indices_.emplace(section.name, series_.size());
      if (inserted)
      {
        const size_t hash = std::hash<std::string>{}(section.name);
        float red = 0.0f;
        float green = 0.0f;
        float blue = 0.0f;
        ImGui::ColorConvertHSVtoRGB(static_cast<float>(hash % 360u) / 360.0f, 0.55f, 0.95f, red, green, blue);
        series_.push_back({
            .name = section.name,
            .order = section.order,
            .color = ImGui::ColorConvertFloat4ToU32(ImVec4(red, green, blue, 1.0f)),
        });
      }
      series_[it->second].values[nextSampleIndex_] += section.milliseconds;
    }
    nextSampleIndex_ = (nextSampleIndex_ + 1u) % kSampleCount;
    count_ = std::min(count_ + 1u, kSampleCount);
    ++totalCount_;
  }

  size_t count() const { return count_; }
  uint64_t totalCount() const { return totalCount_; }
  const std::vector<Series> &series() const { return series_; }
  size_t storageIndex(const size_t orderedSampleIndex) const
  {
    return (nextSampleIndex_ + kSampleCount - count_ + orderedSampleIndex) % kSampleCount;
  }
  float sampleValue(const Series &entry, const size_t orderedSampleIndex) const
  {
    return orderedSampleIndex < count_ ? entry.values[storageIndex(orderedSampleIndex)] : 0.0f;
  }
  float totalMs(const size_t orderedSampleIndex) const
  {
    float result = 0.0f;
    for (const Series &entry : series_)
      result += sampleValue(entry, orderedSampleIndex);
    return result;
  }
  uint32_t frameIndex(const size_t orderedSampleIndex) const
  {
    return orderedSampleIndex < count_ ? frameIndices_[storageIndex(orderedSampleIndex)] : 0u;
  }

private:
  std::vector<Series> series_;
  std::unordered_map<std::string, size_t> indices_;
  std::array<uint32_t, kSampleCount> frameIndices_{};
  size_t nextSampleIndex_ = 0u;
  size_t count_ = 0u;
  uint64_t totalCount_ = 0u;
};

inline void drawCpuTimelineGraph(
    const CpuTimelineHistory &history,
    const size_t visibleFrameCount,
    const size_t selectedOffset,
    const ImVec2 requestedSize)
{
  ImVec2 size = requestedSize;
  if (size.x <= 0.0f)
    size.x = std::max(1.0f, ImGui::GetContentRegionAvail().x);
  if (size.y <= 0.0f)
    size.y = 180.0f;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##VoxelCpuTimelineCanvas", size);
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 maximum(origin.x + size.x, origin.y + size.y);
  draw->AddRectFilled(origin, maximum, IM_COL32(14, 18, 24, 220), 6.0f);
  draw->AddRect(origin, maximum, IM_COL32(90, 98, 110, 255), 6.0f);
  if (history.count() == 0u)
  {
    draw->AddText(ImVec2(origin.x + 10.0f, origin.y + 10.0f), IM_COL32(210, 215, 220, 255), "No CPU samples yet");
    return;
  }

  const ImVec2 plotMinimum(origin.x + 54.0f, origin.y + 10.0f);
  const ImVec2 plotMaximum(maximum.x - 8.0f, maximum.y - 20.0f);
  const size_t visibleCount = std::min(history.count(), std::max<size_t>(1u, visibleFrameCount));
  const size_t firstSample = history.count() - visibleCount;
  float maximumMs = 0.0f;
  for (size_t sample = 0u; sample < visibleCount; ++sample)
    maximumMs = std::max(maximumMs, history.totalMs(firstSample + sample));
  maximumMs = std::max(0.1f, maximumMs * 1.1f);
  for (uint32_t grid = 0u; grid < 5u; ++grid)
  {
    const float fraction = static_cast<float>(grid) / 4.0f;
    const float y = plotMaximum.y + (plotMinimum.y - plotMaximum.y) * fraction;
    draw->AddLine(ImVec2(plotMinimum.x, y), ImVec2(plotMaximum.x, y), IM_COL32(58, 66, 78, 200));
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", maximumMs * fraction);
    draw->AddText(ImVec2(origin.x + 5.0f, y - 7.0f), IM_COL32(180, 188, 198, 255), text);
  }

  std::vector<const CpuTimelineHistory::Series *> ordered;
  for (const auto &entry : history.series())
    ordered.push_back(&entry);
  std::sort(ordered.begin(), ordered.end(), [](const auto *left, const auto *right) { return left->order < right->order; });
  std::array<float, CpuTimelineHistory::kSampleCount> cumulative{};
  draw->PushClipRect(plotMinimum, plotMaximum, true);
  for (const auto *entry : ordered)
  {
    ImVec2 previous{};
    bool hasPrevious = false;
    for (size_t sample = 0u; sample < visibleCount; ++sample)
    {
      cumulative[sample] += history.sampleValue(*entry, firstSample + sample);
      const float x = visibleCount > 1u
                          ? plotMinimum.x + (plotMaximum.x - plotMinimum.x) * static_cast<float>(sample) / static_cast<float>(visibleCount - 1u)
                          : plotMinimum.x;
      const float y = plotMaximum.y - (plotMaximum.y - plotMinimum.y) * std::clamp(cumulative[sample] / maximumMs, 0.0f, 1.0f);
      const ImVec2 point(x, y);
      if (hasPrevious)
        draw->AddLine(previous, point, entry->color, 1.6f);
      previous = point;
      hasPrevious = true;
    }
  }
  draw->PopClipRect();
  const size_t selectedLocal = visibleCount - 1u - std::min(selectedOffset, visibleCount - 1u);
  const float selectedX = visibleCount > 1u
                              ? plotMinimum.x + (plotMaximum.x - plotMinimum.x) * static_cast<float>(selectedLocal) / static_cast<float>(visibleCount - 1u)
                              : plotMinimum.x;
  draw->AddLine(ImVec2(selectedX, plotMinimum.y), ImVec2(selectedX, plotMaximum.y), IM_COL32(235, 235, 240, 130));
}

inline void drawCpuTimelineOverlay(const CpuTimelineHistory &history, const bool open)
{
  if (!open)
    return;
  ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 16.0f, viewport->WorkPos.y + viewport->WorkSize.y - 476.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(700.0f, 460.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.88f);
  ImGui::Begin("Voxel CPU Timeline", nullptr, ImGuiWindowFlags_NoFocusOnAppearing);
  static int visibleFrames = 60;
  static int selectedOffset = 0;
  const int available = static_cast<int>(std::max<size_t>(1u, history.count()));
  visibleFrames = std::clamp(visibleFrames, 1, available);
  selectedOffset = std::clamp(selectedOffset, 0, visibleFrames - 1);
  ImGui::Text("CPU phase history over %zu frames", history.count());
  ImGui::SliderInt("CPU visible frames", &visibleFrames, 1, available);
  ImGui::SliderInt("CPU inspect N-", &selectedOffset, 0, std::max(0, visibleFrames - 1));
  size_t selectedSample = 0u;
  if (history.count() != 0u)
  {
    selectedSample = history.count() - 1u - static_cast<size_t>(selectedOffset);
    ImGui::Text("Frame %u   tracked frame %.3f ms", history.frameIndex(selectedSample), history.totalMs(selectedSample));
  }
  drawCpuTimelineGraph(history, visibleFrames, selectedOffset, ImVec2(-1.0f, 180.0f));
  if (history.count() != 0u && ImGui::BeginTable("VoxelCpuPhaseCosts", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
  {
    ImGui::TableSetupColumn("Phase");
    ImGui::TableSetupColumn("Time");
    ImGui::TableSetupColumn("Share");
    ImGui::TableHeadersRow();
    std::vector<const CpuTimelineHistory::Series *> ordered;
    for (const auto &entry : history.series())
      ordered.push_back(&entry);
    std::sort(ordered.begin(), ordered.end(), [&](const auto *left, const auto *right) {
      return history.sampleValue(*left, selectedSample) > history.sampleValue(*right, selectedSample);
    });
    const float total = history.totalMs(selectedSample);
    for (const auto *entry : ordered)
    {
      const float milliseconds = history.sampleValue(*entry, selectedSample);
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(entry->name.c_str());
      ImGui::TableSetColumnIndex(1);
      ImGui::Text("%.3f ms", milliseconds);
      ImGui::TableSetColumnIndex(2);
      ImGui::Text("%.1f%%", total > 0.0f ? milliseconds * 100.0f / total : 0.0f);
    }
    ImGui::EndTable();
  }
  ImGui::End();
}
} // namespace sparse_voxel_sample
