#pragma once

#include "rendering/gpu/RenderGraph.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace atmosphere
{

enum class AtmospherePassKind : uint32_t
{
  Transmittance = 0u,
  MultipleScattering,
  TerrainShadow,
  SkyView,
  AerialPerspective,
  Ambient,
  Composite,
  Overview,
  Debug,
  Count,
};

inline constexpr size_t kAtmospherePassKindCount =
    static_cast<size_t>(AtmospherePassKind::Count);

struct AtmosphereTimingStatistics
{
  double latestMs = 0.0;
  double medianMs = 0.0;
  double percentile95Ms = 0.0;
  uint32_t sampleCount = 0u;
};

class AtmosphereDiagnostics
{
public:
  static constexpr size_t kHistoryLength = 180u;

  void pushFrame(
      uint64_t frameIndex,
      const std::vector<rendering::RenderGraph::TimerReadbackRecord> &records)
  {
    if (hasFrame_ && frameIndex == lastFrameIndex_)
      return;
    hasFrame_ = true;
    lastFrameIndex_ = frameIndex;
    for (auto &history : histories_)
      history[next_] = 0.0;
    total_[next_] = 0.0;
    for (const auto &record : records)
    {
      const AtmospherePassKind kind = classify(record.passName);
      if (kind == AtmospherePassKind::Count ||
          !std::isfinite(record.gpuTimeMs) || record.gpuTimeMs < 0.0)
        continue;
      histories_[static_cast<size_t>(kind)][next_] += record.gpuTimeMs;
      total_[next_] += record.gpuTimeMs;
    }
    next_ = (next_ + 1u) % kHistoryLength;
    count_ = std::min(count_ + 1u, kHistoryLength);
  }

  AtmosphereTimingStatistics statistics(const AtmospherePassKind kind) const
  {
    if (kind == AtmospherePassKind::Count)
      return statisticsFor(total_);
    return statisticsFor(histories_[static_cast<size_t>(kind)]);
  }

  AtmosphereTimingStatistics totalStatistics() const
  {
    return statisticsFor(total_);
  }

  size_t frameCount() const { return count_; }

  static const char *label(const AtmospherePassKind kind)
  {
    switch (kind)
    {
      case AtmospherePassKind::Transmittance: return "Transmittance";
      case AtmospherePassKind::MultipleScattering: return "Multiple scattering";
      case AtmospherePassKind::TerrainShadow: return "Terrain shadow";
      case AtmospherePassKind::SkyView: return "Sky view";
      case AtmospherePassKind::AerialPerspective: return "Aerial perspective";
      case AtmospherePassKind::Ambient: return "Ambient";
      case AtmospherePassKind::Composite: return "Composite";
      case AtmospherePassKind::Overview: return "Overview";
      case AtmospherePassKind::Debug: return "Debug";
      default: return "Total";
    }
  }

  static AtmospherePassKind classify(const std::string &passName)
  {
    std::string normalized = passName;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (normalized.find("atmosphere") == std::string::npos)
      return AtmospherePassKind::Count;
    if (normalized.find("transmittance") != std::string::npos)
      return AtmospherePassKind::Transmittance;
    if (normalized.find("multiple") != std::string::npos)
      return AtmospherePassKind::MultipleScattering;
    if (normalized.find("terrainshadow") != std::string::npos ||
        normalized.find("terrain_shadow") != std::string::npos ||
        normalized.find("terrain shadow") != std::string::npos)
      return AtmospherePassKind::TerrainShadow;
    if (normalized.find("skyview") != std::string::npos ||
        normalized.find("sky_view") != std::string::npos ||
        normalized.find("sky view") != std::string::npos)
      return AtmospherePassKind::SkyView;
    if (normalized.find("aerial") != std::string::npos)
      return AtmospherePassKind::AerialPerspective;
    if (normalized.find("ambient") != std::string::npos)
      return AtmospherePassKind::Ambient;
    if (normalized.find("composite") != std::string::npos)
      return AtmospherePassKind::Composite;
    if (normalized.find("overview") != std::string::npos ||
        normalized.find("orbit") != std::string::npos)
      return AtmospherePassKind::Overview;
    return AtmospherePassKind::Debug;
  }

private:
  AtmosphereTimingStatistics statisticsFor(
      const std::array<double, kHistoryLength> &history) const
  {
    AtmosphereTimingStatistics result{};
    result.sampleCount = static_cast<uint32_t>(count_);
    if (count_ == 0u)
      return result;
    result.latestMs = history[(next_ + kHistoryLength - 1u) % kHistoryLength];
    std::vector<double> ordered;
    ordered.reserve(count_);
    const size_t first = (next_ + kHistoryLength - count_) % kHistoryLength;
    for (size_t index = 0u; index < count_; ++index)
      ordered.push_back(history[(first + index) % kHistoryLength]);
    std::sort(ordered.begin(), ordered.end());
    result.medianMs = ordered[(ordered.size() - 1u) / 2u];
    const size_t p95 = static_cast<size_t>(
        std::ceil(static_cast<double>(ordered.size()) * 0.95)) - 1u;
    result.percentile95Ms = ordered[std::min(p95, ordered.size() - 1u)];
    return result;
  }

  std::array<std::array<double, kHistoryLength>,
      kAtmospherePassKindCount> histories_{};
  std::array<double, kHistoryLength> total_{};
  size_t next_ = 0u;
  size_t count_ = 0u;
  uint64_t lastFrameIndex_ = 0u;
  bool hasFrame_ = false;
};

} // namespace atmosphere
