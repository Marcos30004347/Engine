#include "voxel/svraster/TerrainGPUV2Stability.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace voxel::svraster
{
namespace
{
bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool sampleHeight(const TerrainGPUV2GroundHeightField &field, const double x, const double z, double &height)
{
  if (field.spacingMetres <= 0.0 || !std::isfinite(field.spacingMetres) || field.width == 0u || field.depth == 0u || field.fineSurfaceHeightMetres.size() != uint64_t{field.width} * field.depth)
    return false;
  const double localX = (x - field.originMetres[0]) / field.spacingMetres;
  const double localZ = (z - field.originMetres[1]) / field.spacingMetres;
  if (!std::isfinite(localX) || !std::isfinite(localZ) || localX < 0.0 || localZ < 0.0 || localX >= field.width || localZ >= field.depth)
    return false;
  const uint32_t ix = std::min(static_cast<uint32_t>(std::floor(localX)), field.width - 1u);
  const uint32_t iz = std::min(static_cast<uint32_t>(std::floor(localZ)), field.depth - 1u);
  height = field.fineSurfaceHeightMetres[uint64_t{iz} * field.width + ix];
  return std::isfinite(height);
}

bool validCapture(const TerrainGPUV2VisualCapture &capture)
{
  return capture.width != 0u && capture.height != 0u && capture.pixels.size() == uint64_t{capture.width} * capture.height && static_cast<uint32_t>(capture.layer) < kTerrainGPUV2CaptureLayerCount;
}

bool finitePixel(const TerrainGPUV2CapturePixel &pixel)
{
  if (!std::isfinite(pixel.depthMetres) || !std::isfinite(pixel.lightVisibility))
    return false;
  for (const float channel : pixel.color)
    if (!std::isfinite(channel))
      return false;
  return true;
}

bool visualDefect(const TerrainGPUV2CapturePixel &pixel)
{
  return pixel.expectedTerrain != 0u && (pixel.coverageCount == 0u || pixel.coverageCount > 1u || pixel.boundaryConsistent == 0u || !finitePixel(pixel));
}

bool sameIdentity(const TerrainGPUV2CapturePixel &left, const TerrainGPUV2CapturePixel &right)
{
  return left.primitiveId == right.primitiveId && left.brickId == right.brickId && left.cellId == right.cellId && left.faceId == right.faceId;
}

bool differs(const float left, const float right, const float threshold)
{
  return std::abs(left - right) > threshold;
}

std::string tableText(std::string value)
{
  std::replace(value.begin(), value.end(), '|', '/');
  std::replace(value.begin(), value.end(), '\n', ' ');
  return value;
}
} // namespace

bool buildTerrainGPUV2GroundPath(const TerrainGPUV2GroundHeightField &heightField, const TerrainGPUV2GroundPathSettings &settings, std::vector<TerrainGPUV2GroundCameraSample> &samples, std::string *error)
{
  samples.clear();
  if (settings.travelFrames == 0u || settings.stationaryFrames < 2u || settings.closeFrames < 2u || !std::isfinite(settings.standingClearanceMetres) || settings.standingClearanceMetres <= 0.0 ||
      !std::isfinite(settings.closeClearanceMetres) || settings.closeClearanceMetres <= 0.0 || settings.closeClearanceMetres > settings.standingClearanceMetres || !std::isfinite(settings.projectionJitterPixels) ||
      settings.projectionJitterPixels < 0.0)
    return fail(error, "terrain GPU V2 ground path settings are invalid");

  const uint64_t total = uint64_t{settings.travelFrames} + settings.stationaryFrames + settings.closeFrames;
  if (total > std::numeric_limits<uint32_t>::max())
    return fail(error, "terrain GPU V2 ground path is too long");
  samples.reserve(static_cast<size_t>(total));
  const std::array<std::array<double, 2u>, 4u> jitter{{
    {{-0.5, -0.25}},
    {{0.25, -0.5}},
    {{0.5, 0.25}},
    {{-0.25, 0.5}},
  }};
  for (uint32_t frame = 0u; frame < total; ++frame)
  {
    const bool travelling = frame < settings.travelFrames;
    const bool close = frame >= settings.travelFrames + settings.stationaryFrames;
    const double t = settings.travelFrames <= 1u ? 1.0 : std::min(frame, settings.travelFrames - 1u) / static_cast<double>(settings.travelFrames - 1u);
    const double x = settings.startMetres[0] + settings.travelMetres[0] * t;
    const double z = settings.startMetres[1] + settings.travelMetres[1] * t;
    double surface = 0.0;
    if (!sampleHeight(heightField, x, z, surface))
      return fail(error, "terrain GPU V2 ground path left the fine-surface height field");
    const double clearance = close ? settings.closeClearanceMetres : settings.standingClearanceMetres;
    const auto pattern = jitter[frame % jitter.size()];
    TerrainGPUV2GroundCameraSample sample{};
    sample.frameIndex = frame;
    sample.comparisonGroup = close ? 2u : travelling ? 0u : 1u;
    sample.positionMetres = {x, surface + clearance, z};
    sample.projectionJitterPixels = {pattern[0] * settings.projectionJitterPixels, pattern[1] * settings.projectionJitterPixels};
    sample.surfaceHeightMetres = surface;
    sample.clearanceMetres = clearance;
    sample.cameraMoving = travelling && frame + 1u < settings.travelFrames;
    sample.closeRange = close;
    samples.push_back(sample);
  }
  if (error != nullptr)
    error->clear();
  return true;
}

bool analyzeTerrainGPUV2Stability(const std::vector<TerrainGPUV2VisualCapture> &captures, const TerrainGPUV2StabilityThresholds &thresholds, TerrainGPUV2StabilityReport &report, std::string *error)
{
  report = {};
  if (captures.empty() || thresholds.depthMetres < 0.0f || thresholds.color < 0.0f || thresholds.lightVisibility < 0.0f || thresholds.rayOriginOffsetMetres < 0.0 || thresholds.closeRangeMaximumMetres <= 0.0)
    return fail(error, "terrain GPU V2 stability input is invalid");

  using SequenceKey = std::tuple<uint32_t, bool, uint64_t>;
  using FrameKey = std::pair<uint64_t, uint64_t>;
  using CaptureKey = std::tuple<uint32_t, bool, uint64_t, uint64_t>;
  std::map<SequenceKey, std::vector<const TerrainGPUV2VisualCapture *>> sequences;
  std::map<FrameKey, const TerrainGPUV2VisualCapture *> nativeRaw;
  std::map<FrameKey, const TerrainGPUV2VisualCapture *> temporalRaw;
  std::set<CaptureKey> captureKeys;
  bool hasNativeRaw = false;
  bool hasTemporalRaw = false;
  bool hasStationaryJitterPair = false;
  bool hasClosePair = false;

  for (const auto &capture : captures)
  {
    if (!validCapture(capture))
      return fail(error, "terrain GPU V2 visual capture dimensions are invalid");
    if (!captureKeys.emplace(static_cast<uint32_t>(capture.layer), capture.temporalUpscaling, capture.comparisonGroup, capture.frameIndex).second)
      return fail(error, "terrain GPU V2 visual capture key is duplicated");
    ++report.captures;
    report.captureLayerMask |= 1u << static_cast<uint32_t>(capture.layer);
    sequences[{static_cast<uint32_t>(capture.layer), capture.temporalUpscaling, capture.comparisonGroup}].push_back(&capture);
    if (capture.layer == TerrainGPUV2CaptureLayer::RawGBuffer)
    {
      const FrameKey key{capture.comparisonGroup, capture.frameIndex};
      if (capture.temporalUpscaling)
      {
        temporalRaw[key] = &capture;
        hasTemporalRaw = true;
      }
      else
      {
        nativeRaw[key] = &capture;
        hasNativeRaw = true;
      }
    }

    if (capture.layer == TerrainGPUV2CaptureLayer::RawGBuffer && std::abs(capture.primaryRayOriginOffsetMetres) > thresholds.rayOriginOffsetMetres)
      ++report.rayOriginOffsetViolations;
    if (capture.layer == TerrainGPUV2CaptureLayer::DirectLightOnly && std::abs(capture.directShadowRayOriginOffsetMetres) > thresholds.rayOriginOffsetMetres)
      ++report.rayOriginOffsetViolations;
    for (const auto &pixel : capture.pixels)
    {
      if (!finitePixel(pixel))
        ++report.nonFinitePixels;
      if (pixel.expectedTerrain != 0u && pixel.coverageCount == 0u)
        ++report.blackCrackPixels;
      if (pixel.coverageCount > 1u)
        ++report.overlapPixels;
      if (pixel.boundaryConsistent == 0u)
        ++report.tJunctionPixels;
      if (pixel.expectedTerrain != 0u)
      {
        ++report.occupancyComparedPixels;
        if (pixel.cpuOccupied != pixel.gpuOccupied)
          ++report.occupancyMismatches;
      }
      if (capture.cameraClearanceMetres <= thresholds.closeRangeMaximumMetres)
      {
        ++report.closeRangePixels;
        if (!finitePixel(pixel) || pixel.coverageCount != (pixel.expectedTerrain != 0u ? 1u : 0u))
          ++report.closeRangeUnstablePixels;
      }
    }
  }

  for (auto &[key, sequence] : sequences)
  {
    std::sort(
        sequence.begin(),
        sequence.end(),
        [](const auto *left, const auto *right)
        {
          return left->frameIndex < right->frameIndex;
        });
    for (size_t frame = 1u; frame < sequence.size(); ++frame)
    {
      const auto &previous = *sequence[frame - 1u];
      const auto &current = *sequence[frame];
      if (previous.cameraMoving || current.cameraMoving || previous.width != current.width || previous.height != current.height)
        continue;
      if (previous.projectionJitterEnabled && current.projectionJitterEnabled && previous.layer == TerrainGPUV2CaptureLayer::RawGBuffer)
        hasStationaryJitterPair = true;
      if (current.layer == TerrainGPUV2CaptureLayer::RawGBuffer && previous.cameraClearanceMetres <= thresholds.closeRangeMaximumMetres && current.cameraClearanceMetres <= thresholds.closeRangeMaximumMetres)
        hasClosePair = true;
      for (size_t pixelIndex = 0u; pixelIndex < current.pixels.size(); ++pixelIndex)
      {
        const auto &left = previous.pixels[pixelIndex];
        const auto &right = current.pixels[pixelIndex];
        if (left.expectedTerrain == 0u || right.expectedTerrain == 0u || left.coverageCount != 1u || right.coverageCount != 1u)
          continue;
        ++report.comparedPixels;
        if (!sameIdentity(left, right))
          ++report.identityChanges;
        if (differs(left.depthMetres, right.depthMetres, thresholds.depthMetres))
          ++report.depthChanges;
        for (uint32_t channel = 0u; channel < 4u; ++channel)
          if (differs(left.color[channel], right.color[channel], thresholds.color))
          {
            ++report.imageChanges;
            break;
          }
        if ((current.layer == TerrainGPUV2CaptureLayer::DirectLightOnly || current.layer == TerrainGPUV2CaptureLayer::FullLighting) && previous.lightStationary && current.lightStationary &&
            differs(left.lightVisibility, right.lightVisibility, thresholds.lightVisibility))
          ++report.shadowChanges;
        if (current.cameraClearanceMetres <= thresholds.closeRangeMaximumMetres && (!sameIdentity(left, right) || differs(left.depthMetres, right.depthMetres, thresholds.depthMetres)))
          ++report.closeRangeUnstablePixels;
      }
    }
  }

  bool hasMatchedRawPair = false;
  for (const auto &[key, native] : nativeRaw)
  {
    const auto temporal = temporalRaw.find(key);
    if (temporal == temporalRaw.end() || native->width != temporal->second->width || native->height != temporal->second->height)
      continue;
    hasMatchedRawPair = true;
    for (size_t pixelIndex = 0u; pixelIndex < native->pixels.size(); ++pixelIndex)
    {
      const auto &left = native->pixels[pixelIndex];
      const auto &right = temporal->second->pixels[pixelIndex];
      if (visualDefect(left) && !visualDefect(right))
        ++report.upscalerHiddenDefects;
      if (left.expectedTerrain != 0u && right.expectedTerrain != 0u && (!sameIdentity(left, right) || differs(left.depthMetres, right.depthMetres, thresholds.depthMetres)))
        ++report.upscalerGeometryMismatches;
    }
  }

  const uint32_t allLayers = (1u << kTerrainGPUV2CaptureLayerCount) - 1u;
  if (report.captureLayerMask != allLayers)
    ++report.missingCaptureRequirements;
  if (!hasNativeRaw || !hasTemporalRaw || !hasMatchedRawPair)
    ++report.missingCaptureRequirements;
  if (!hasStationaryJitterPair)
    ++report.missingCaptureRequirements;
  if (!hasClosePair || report.closeRangePixels == 0u)
    ++report.missingCaptureRequirements;
  if (report.occupancyComparedPixels == 0u)
    ++report.missingCaptureRequirements;

  report.qualified = report.nonFinitePixels == 0u && report.blackCrackPixels == 0u && report.overlapPixels == 0u && report.tJunctionPixels == 0u && report.identityChanges == 0u && report.depthChanges == 0u &&
                     report.imageChanges == 0u && report.shadowChanges == 0u && report.upscalerGeometryMismatches == 0u && report.upscalerHiddenDefects == 0u && report.rayOriginOffsetViolations == 0u &&
                     report.occupancyMismatches == 0u && report.closeRangeUnstablePixels == 0u && report.missingCaptureRequirements == 0u;
  if (error != nullptr)
    error->clear();
  return true;
}

bool TerrainGPUV2QualificationTable::add(const TerrainGPUV2DeviceQualification &qualification, std::string *error)
{
  if (qualification.backend.empty() || qualification.device.empty() || qualification.driver.empty() || (qualification.status == TerrainGPUV2QualificationStatus::Qualified && !qualification.report.qualified))
    return fail(error, "terrain GPU V2 device qualification entry is invalid");
  const auto duplicate = std::find_if(
      entries_.begin(),
      entries_.end(),
      [&](const auto &entry)
      {
        return entry.backend == qualification.backend && entry.device == qualification.device && entry.driver == qualification.driver && entry.deviceProfile == qualification.deviceProfile;
      });
  if (duplicate != entries_.end())
    return fail(error, "terrain GPU V2 device qualification entry already exists");
  entries_.push_back(qualification);
  std::sort(
      entries_.begin(),
      entries_.end(),
      [](const auto &left, const auto &right)
      {
        return std::tie(left.backend, left.device, left.driver, left.deviceProfile) < std::tie(right.backend, right.device, right.driver, right.deviceProfile);
      });
  if (error != nullptr)
    error->clear();
  return true;
}

std::string TerrainGPUV2QualificationTable::markdown() const
{
  std::ostringstream stream;
  stream << "| Backend | Device | Driver | Profile | Status | Cracks/overlaps | Identity/depth | Shadow | CPU/GPU | Reason |\n"
         << "|---|---|---|---:|---|---:|---:|---:|---:|---|\n";
  for (const auto &entry : entries_)
    stream << "| " << tableText(entry.backend) << " | " << tableText(entry.device) << " | " << tableText(entry.driver) << " | " << entry.deviceProfile << " | " << terrainGPUV2QualificationStatusName(entry.status)
           << " | " << entry.report.blackCrackPixels << '/' << entry.report.overlapPixels << " | " << entry.report.identityChanges << '/' << entry.report.depthChanges << " | " << entry.report.shadowChanges << " | "
           << entry.report.occupancyMismatches << " | " << tableText(entry.reason) << " |\n";
  return stream.str();
}

const char *terrainGPUV2CaptureLayerName(const TerrainGPUV2CaptureLayer layer)
{
  switch (layer)
  {
  case TerrainGPUV2CaptureLayer::RawGBuffer:
    return "raw-gbuffer";
  case TerrainGPUV2CaptureLayer::UnlitAlbedo:
    return "unlit-albedo";
  case TerrainGPUV2CaptureLayer::DirectLightOnly:
    return "direct-light-only";
  case TerrainGPUV2CaptureLayer::FullLighting:
    return "full-lighting";
  case TerrainGPUV2CaptureLayer::Upscaled:
    return "upscaled";
  }
  return "unknown";
}

const char *terrainGPUV2QualificationStatusName(const TerrainGPUV2QualificationStatus status)
{
  switch (status)
  {
  case TerrainGPUV2QualificationStatus::Qualified:
    return "qualified";
  case TerrainGPUV2QualificationStatus::Failed:
    return "failed";
  case TerrainGPUV2QualificationStatus::Unsupported:
    return "unsupported";
  }
  return "unknown";
}

} // namespace voxel::svraster
