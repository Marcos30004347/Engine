#include "SparseVoxelShadowBenchmark.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <numeric>
#include <sstream>
#include <system_error>

namespace sparse_voxel_sample
{
namespace
{
struct Statistics
{
  double minimum = 0.0;
  double p50 = 0.0;
  double p90 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
  double maximum = 0.0;
  double mean = 0.0;
  double standardDeviation = 0.0;
  double medianAbsoluteDeviation = 0.0;
};

std::string escapeJSON(const std::string &value)
{
  std::ostringstream stream;
  for (const unsigned char character : value)
  {
    switch (character)
    {
    case '"':
      stream << "\\\"";
      break;
    case '\\':
      stream << "\\\\";
      break;
    case '\b':
      stream << "\\b";
      break;
    case '\f':
      stream << "\\f";
      break;
    case '\n':
      stream << "\\n";
      break;
    case '\r':
      stream << "\\r";
      break;
    case '\t':
      stream << "\\t";
      break;
    default:
      if (character < 0x20u)
        stream << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<uint32_t>(character) << std::dec;
      else
        stream << static_cast<char>(character);
      break;
    }
  }
  return stream.str();
}

double percentile(const std::vector<double> &sortedValues, const double fraction)
{
  if (sortedValues.empty())
    return 0.0;
  const double position = fraction * static_cast<double>(sortedValues.size() - 1u);
  const size_t lower = static_cast<size_t>(std::floor(position));
  const size_t upper = static_cast<size_t>(std::ceil(position));
  const double blend = position - static_cast<double>(lower);
  return sortedValues[lower] + (sortedValues[upper] - sortedValues[lower]) * blend;
}

Statistics statistics(std::vector<double> values)
{
  Statistics result{};
  if (values.empty())
    return result;
  std::sort(values.begin(), values.end());
  result.minimum = values.front();
  result.p50 = percentile(values, 0.50);
  result.p90 = percentile(values, 0.90);
  result.p95 = percentile(values, 0.95);
  result.p99 = percentile(values, 0.99);
  result.maximum = values.back();
  result.mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
  double variance = 0.0;
  for (const double value : values)
  {
    const double difference = value - result.mean;
    variance += difference * difference;
  }
  result.standardDeviation = std::sqrt(variance / static_cast<double>(values.size()));
  std::vector<double> absoluteDeviations;
  absoluteDeviations.reserve(values.size());
  for (const double value : values)
    absoluteDeviations.push_back(std::abs(value - result.p50));
  std::sort(absoluteDeviations.begin(), absoluteDeviations.end());
  result.medianAbsoluteDeviation = percentile(absoluteDeviations, 0.50);
  return result;
}

void writeNumber(std::ostream &stream, const double value)
{
  if (std::isfinite(value))
    stream << std::setprecision(12) << value;
  else
    stream << "null";
}

void writeStatistics(std::ostream &stream, const Statistics &value)
{
  stream << "{\n";
  stream << "        \"min\": ";
  writeNumber(stream, value.minimum);
  stream << ",\n        \"p50\": ";
  writeNumber(stream, value.p50);
  stream << ",\n        \"p90\": ";
  writeNumber(stream, value.p90);
  stream << ",\n        \"p95\": ";
  writeNumber(stream, value.p95);
  stream << ",\n        \"p99\": ";
  writeNumber(stream, value.p99);
  stream << ",\n        \"max\": ";
  writeNumber(stream, value.maximum);
  stream << ",\n        \"mean\": ";
  writeNumber(stream, value.mean);
  stream << ",\n        \"standard_deviation\": ";
  writeNumber(stream, value.standardDeviation);
  stream << ",\n        \"median_absolute_deviation\": ";
  writeNumber(stream, value.medianAbsoluteDeviation);
  stream << "\n      }";
}

std::string hexadecimal(const uint64_t value)
{
  std::ostringstream stream;
  stream << std::hex << std::setw(16) << std::setfill('0') << value;
  return stream.str();
}

std::map<std::string, std::vector<double>> collectNamedTimings(const std::vector<ShadowBenchmarkFrame> &frames, const bool cpu)
{
  std::map<std::string, std::vector<double>> result;
  for (const ShadowBenchmarkFrame &frame : frames)
  {
    const auto &timings = cpu ? frame.cpuSections : frame.gpuPasses;
    for (const ShadowBenchmarkNamedTiming &timing : timings)
      result[timing.name].push_back(timing.milliseconds);
  }
  return result;
}

bool isBrickPhase0Timing(const std::string &name)
{
  static constexpr std::array<const char *, 10u> markers{
      ".BrickPhase0.Reset_",
      ".BrickPhase0.Cull_",
      ".BrickPhase0.DrawArgs_",
      ".BrickPhase0.Draw_",
      ".BrickPhase0.MicroPrepareReset_",
      ".BrickPhase0.MicroPrepareTransform_",
      ".BrickPhase0.ResidualDraw_",
      ".BrickPhase0.PreparedMicroDraw_",
      ".BrickPhase0.PreparedTieReplayDraw_",
      ".BrickPhaseHZB_",
  };
  return std::any_of(
      markers.begin(), markers.end(),
      [&name](const char *marker)
      {
        return name.find(marker) != std::string::npos;
      });
}

double brickPhase0Total(const ShadowBenchmarkFrame &frame)
{
  return std::accumulate(
      frame.gpuPasses.begin(), frame.gpuPasses.end(), 0.0,
      [](const double total, const ShadowBenchmarkNamedTiming &timing)
      {
        return total +
            (isBrickPhase0Timing(timing.name) ? timing.milliseconds : 0.0);
      });
}

void writeNamedTimings(std::ostream &stream, const std::vector<ShadowBenchmarkNamedTiming> &timings, const std::string &indent)
{
  stream << "[";
  if (!timings.empty())
    stream << "\n";
  for (size_t index = 0u; index < timings.size(); ++index)
  {
    const auto &timing = timings[index];
    stream << indent << "  {\"name\": \"" << escapeJSON(timing.name) << "\", \"milliseconds\": ";
    writeNumber(stream, timing.milliseconds);
    stream << "}";
    if (index + 1u < timings.size())
      stream << ",";
    stream << "\n";
  }
  if (!timings.empty())
    stream << indent;
  stream << "]";
}

void writeNamedAggregates(std::ostream &stream, const std::map<std::string, std::vector<double>> &series)
{
  stream << "{";
  if (!series.empty())
    stream << "\n";
  size_t index = 0u;
  for (const auto &[name, values] : series)
  {
    stream << "      \"" << escapeJSON(name) << "\": ";
    writeStatistics(stream, statistics(values));
    if (++index < series.size())
      stream << ",";
    stream << "\n";
  }
  if (!series.empty())
    stream << "    ";
  stream << "}";
}
} // namespace

ShadowBenchmarkFrame &ShadowBenchmarkReport::frame(const uint32_t frameIndex)
{
  const auto existing = std::find_if(
      frames.begin(),
      frames.end(),
      [frameIndex](const ShadowBenchmarkFrame &candidate)
      {
        return candidate.frameIndex == frameIndex;
      });
  if (existing != frames.end())
    return *existing;
  frames.push_back({.frameIndex = frameIndex});
  return frames.back();
}

bool ShadowBenchmarkReport::write(const std::filesystem::path &path, std::string *error) const
{
  if (error != nullptr)
    error->clear();
  if (path.empty())
  {
    if (error != nullptr)
      *error = "Benchmark output path is empty";
    return false;
  }
  if (frames.size() != configuration.measuredFrames)
  {
    if (error != nullptr)
      *error = "Benchmark did not capture the requested number of frames";
    return false;
  }
  for (const ShadowBenchmarkFrame &sample : frames)
  {
    if (sample.cpuSections.empty() ||
        (configuration.gpuTimingsAvailable && sample.gpuPasses.empty()))
    {
      if (error != nullptr)
        *error = "Benchmark frame is missing required CPU or GPU timings";
      return false;
    }
  }

  std::error_code filesystemError;
  if (!path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path(), filesystemError);
  if (filesystemError)
  {
    if (error != nullptr)
      *error = "Could not create benchmark output directory: " + filesystemError.message();
    return false;
  }

  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream)
  {
    if (error != nullptr)
      *error = "Could not open benchmark output";
    return false;
  }

  std::vector<ShadowBenchmarkFrame> orderedFrames = frames;
  std::sort(
      orderedFrames.begin(),
      orderedFrames.end(),
      [](const auto &left, const auto &right)
      {
        return left.frameIndex < right.frameIndex;
      });
  std::vector<double> cpuTotals;
  std::vector<double> gpuTotals;
  std::vector<double> brickPhase0Totals;
  std::vector<double> occupancySource;
  std::vector<double> occupancyPlanning;
  std::vector<double> occupancyUpload;
  std::vector<double> tree64Planning;
  std::vector<double> tree64Upload;
  std::vector<double> terrainUploadBytes;
  cpuTotals.reserve(orderedFrames.size());
  gpuTotals.reserve(orderedFrames.size());
  brickPhase0Totals.reserve(orderedFrames.size());
  for (const ShadowBenchmarkFrame &sample : orderedFrames)
  {
    cpuTotals.push_back(sample.cpuTotalMilliseconds);
    gpuTotals.push_back(sample.gpuTotalMilliseconds);
    brickPhase0Totals.push_back(brickPhase0Total(sample));
    occupancySource.push_back(sample.occupancySourceMilliseconds);
    occupancyPlanning.push_back(sample.occupancyPlanningMilliseconds);
    occupancyUpload.push_back(sample.occupancyUploadMilliseconds);
    tree64Planning.push_back(sample.tree64PlanningMilliseconds);
    tree64Upload.push_back(sample.tree64UploadMilliseconds);
    if (sample.terrainStreaming.valid)
      terrainUploadBytes.push_back(
          static_cast<double>(
              sample.terrainStreaming.uploadBytesSubmitted));
  }

  stream << "{\n";
  stream << "  \"schema_version\": " << kSchemaVersion << ",\n";
  stream << "  \"configuration\": {\n";
  stream << "    \"workload\": \"" << escapeJSON(configuration.workload)
         << "\",\n";
  stream << "    \"backend\": \"" << escapeJSON(configuration.backend) << "\",\n";
  stream << "    \"tree64_traversal\": \""
         << escapeJSON(configuration.tree64Traversal) << "\",\n";
  stream << "    \"sequence\": \"" << escapeJSON(configuration.sequence) << "\",\n";
  stream << "    \"terrain_performance_preset\": \""
         << escapeJSON(configuration.terrainPerformancePreset) << "\",\n";
  stream << "    \"terrain_streaming_mode\": \""
         << escapeJSON(configuration.terrainStreamingMode) << "\",\n";
  stream << "    \"terrain_pages_per_update\": "
         << configuration.terrainPagesPerUpdate << ",\n";
  stream << "    \"terrain_upload_budget_bytes\": "
         << configuration.terrainUploadBudgetBytes << ",\n";
  stream << "    \"terrain_feedback_interval\": "
         << configuration.terrainFeedbackInterval << ",\n";
  stream << "    \"terrain_feedback_pending_max_stale_frames\": "
         << configuration.terrainFeedbackPendingMaxStaleFrames << ",\n";
  stream << "    \"screen_trace_statistics_enabled\": "
         << (configuration.screenTraceStatisticsEnabled ? "true" : "false")
         << ",\n";
  stream << "    \"gpu_timings_available\": "
         << (configuration.gpuTimingsAvailable ? "true" : "false") << ",\n";
  stream << "    \"warmup_frames\": " << configuration.warmupFrames << ",\n";
  stream << "    \"measured_frames\": " << configuration.measuredFrames << ",\n";
  stream << "    \"resolution\": [" << configuration.width << ", " << configuration.height << "],\n";
  stream << "    \"raster_lod_pixel_threshold\": ";
  writeNumber(stream, configuration.rasterLODPixelThreshold);
  stream << ",\n    \"tree64_lod_pixel_threshold\": ";
  writeNumber(stream, configuration.tree64LODPixelThreshold);
  stream << ",\n";
  stream << "    \"svgf_atrous_iterations\": "
         << configuration.svgfAtrousIterations << ",\n";
  stream << "    \"probe_tile_size\": "
         << configuration.probeTileSize << ",\n";
  stream << "    \"probe_adaptive_percent\": "
         << configuration.probeAdaptivePercent << ",\n";
  stream << "    \"probe_awake_rays\": "
         << configuration.probeAwakeRays << ",\n";
  stream << "    \"planet_background_stride\": "
         << configuration.planetBackgroundStride << ",\n";
  stream << "    \"terrain_micro_materials_enabled\": "
         << (configuration.terrainMicroMaterialsEnabled ? "true" : "false")
         << ",\n";
  stream << "    \"brick_micro_raster_enabled\": "
         << (configuration.brickMicroRasterEnabled ? "true" : "false")
         << ",\n";
  stream << "    \"brick_micro_raster_mode\": \""
         << escapeJSON(configuration.brickMicroRasterMode) << "\",\n";
  stream << "    \"brick_micro_record_capacity\": "
         << configuration.brickMicroRecordCapacity << ",\n";
  stream << "    \"brick_exact_micro_distance_cutoff_metres\": ";
  writeNumber(stream, configuration.brickExactMicroDistanceCutoffMetres);
  stream << ",\n";
  stream << "    \"probe_density_half_distance_metres\": ";
  writeNumber(stream, configuration.probeDensityHalfDistanceMetres);
  stream << ",\n    \"probe_density_quarter_distance_metres\": ";
  writeNumber(stream, configuration.probeDensityQuarterDistanceMetres);
  stream << ",\n    \"probe_trace_fade_start_distance_metres\": ";
  writeNumber(stream, configuration.probeTraceFadeStartDistanceMetres);
  stream << ",\n    \"probe_analytical_distance_metres\": ";
  writeNumber(stream, configuration.probeAnalyticalDistanceMetres);
  stream << ",\n";
  stream << "    \"occupancy_resolution\": [" << configuration.occupancyResolution[0] << ", " << configuration.occupancyResolution[1] << ", " << configuration.occupancyResolution[2] << "],\n";
  stream << "    \"occupancy_cell_size\": [";
  writeNumber(stream, configuration.occupancyCellSize[0]);
  stream << ", ";
  writeNumber(stream, configuration.occupancyCellSize[1]);
  stream << ", ";
  writeNumber(stream, configuration.occupancyCellSize[2]);
  stream << "],\n";
  stream << "    \"occupancy_world_extent\": [";
  writeNumber(stream, configuration.occupancyWorldExtent[0]);
  stream << ", ";
  writeNumber(stream, configuration.occupancyWorldExtent[1]);
  stream << ", ";
  writeNumber(stream, configuration.occupancyWorldExtent[2]);
  stream << "],\n";
  stream << "    \"tree64_directory_resolution\": [" << configuration.tree64DirectoryResolution[0] << ", " << configuration.tree64DirectoryResolution[1] << ", " << configuration.tree64DirectoryResolution[2] << "],\n";
  stream << "    \"tree64_chunk_side\": " << configuration.tree64ChunkSide << ",\n";
  stream << "    \"tree64_build_batch_capacity\": " << configuration.tree64BuildBatchCapacity << ",\n";
  stream << "    \"tree64_shadow_lod_world_size\": ";
  writeNumber(stream, configuration.tree64ShadowLODWorldSize);
  stream << ",\n    \"tree64_shadow_lod_distance_slope\": ";
  writeNumber(stream, configuration.tree64ShadowLODDistanceSlope);
  stream << ",\n    \"tree64_shadow_lod_start_distance\": ";
  writeNumber(stream, configuration.tree64ShadowLODStartDistance);
  stream << ",\n    \"tree64_shadow_lod_maximum_world_size\": ";
  writeNumber(stream, configuration.tree64ShadowLODMaximumWorldSize);
  stream << ",\n    \"sky_guiding_strength\": ";
  writeNumber(stream, configuration.skyGuidingStrength);
  stream << ",\n    \"sky_guiding_learning_rate\": ";
  writeNumber(stream, configuration.skyGuidingLearningRate);
  stream << ",\n    \"ray_refresh_interval\": ";
  writeNumber(stream, configuration.rayRefreshInterval);
  stream << ",\n    \"minimum_converged_samples\": "
         << configuration.minimumConvergedSamples;
  stream << ",\n    \"convergence_relative_error\": ";
  writeNumber(stream, configuration.convergenceRelativeError);
  stream << ",\n    \"stable_observation_count\": "
         << configuration.stableObservationCount;
  stream << ",\n    \"region_stability_tile_size\": "
         << configuration.regionStabilityTileSize;
  stream << ",\n    \"region_wake_frames\": "
         << configuration.regionWakeFrames;
  stream << ",\n    \"region_soft_visibility_threshold\": ";
  writeNumber(stream, configuration.regionSoftVisibilityThreshold);
  stream << ",\n    \"region_soft_visibility_streak\": "
         << configuration.regionSoftVisibilityStreak;
  stream << ",\n    \"region_soft_prefill_stride\": "
         << configuration.regionSoftPrefillStride;
  stream << ",\n    \"ray_tile_budget_fraction\": ";
  writeNumber(stream, configuration.rayTileBudgetFraction);
  stream << ",\n    \"ray_tile_budget_boost\": ";
  writeNumber(stream, configuration.rayTileBudgetBoost);
  stream << ",\n    \"ray_tile_budget_heartbeat\": "
         << configuration.rayTileBudgetHeartbeat;
  stream << ",\n    \"occluder_settle_frames\": "
         << configuration.occluderSettleFrames << "\n";
  stream << "  },\n";

  stream << "  \"environment\": {\n";
  stream << "    \"git_commit\": \"" << escapeJSON(environment.gitCommit) << "\",\n";
  stream << "    \"dirty_worktree\": " << (environment.dirtyWorktree ? "true" : "false") << ",\n";
  stream << "    \"build_type\": \"" << escapeJSON(environment.buildType) << "\",\n";
  stream << "    \"compiler\": \"" << escapeJSON(environment.compiler) << "\",\n";
  stream << "    \"operating_system\": \"" << escapeJSON(environment.operatingSystem) << "\",\n";
  stream << "    \"device_name\": \"" << escapeJSON(environment.deviceName) << "\",\n";
  stream << "    \"vendor_id\": " << environment.vendorId << ",\n";
  stream << "    \"device_id\": " << environment.deviceId << ",\n";
  stream << "    \"driver_version\": " << environment.driverVersion << ",\n";
  stream << "    \"subgroup_size\": " << environment.subgroupSize << ",\n";
  stream << "    \"timestamp_period_nanoseconds\": ";
  writeNumber(stream, environment.timestampPeriodNanoseconds);
  stream << ",\n";
  stream << "    \"scene_content_hash\": \"" << hexadecimal(environment.sceneContentHash) << "\",\n";
  stream << "    \"scene_build_settings_hash\": \"" << hexadecimal(environment.sceneBuildSettingsHash) << "\"\n";
  stream << "  },\n";

  stream << "  \"memory\": {\n";
  stream << "    \"occupancy_base_resident_bytes\": " << memory.occupancyBaseResidentBytes << ",\n";
  stream << "    \"occupancy_base_persistent_bytes\": " << memory.occupancyBasePersistentBytes << ",\n";
  stream << "    \"occupancy_pyramid_resident_bytes\": " << memory.occupancyPyramidResidentBytes << ",\n";
  stream << "    \"occupancy_pyramid_persistent_bytes\": " << memory.occupancyPyramidPersistentBytes << ",\n";
  stream << "    \"occupancy_total_persistent_bytes\": " << memory.occupancyTotalPersistentBytes << ",\n";
  stream << "    \"tree64_directory_bytes\": " << memory.tree64DirectoryBytes << ",\n";
  stream << "    \"tree64_node_pool_reserved_bytes\": " << memory.tree64NodePoolReservedBytes << ",\n";
  stream << "    \"tree64_build_scratch_bytes\": " << memory.tree64BuildScratchBytes << ",\n";
  stream << "    \"tree64_total_persistent_bytes\": " << memory.tree64TotalPersistentBytes << ",\n";
  stream << "    \"sky_guide_cdf_bytes\": " << memory.skyGuideCDFBytes << ",\n";
  stream << "    \"sky_guide_feedback_bytes\": " << memory.skyGuideFeedbackBytes << ",\n";
  stream << "    \"sky_guide_total_bytes\": " << memory.skyGuideTotalBytes << ",\n";
  stream << "    \"brick_micro_record_buffer_bytes\": "
         << memory.brickMicroRecordBufferBytes << ",\n";
  stream << "    \"brick_micro_residual_buffer_bytes\": "
         << memory.brickMicroResidualBufferBytes << ",\n";
  stream << "    \"brick_micro_dispatch_buffer_bytes\": "
         << memory.brickMicroDispatchBufferBytes << ",\n";
  stream << "    \"brick_micro_draw_args_buffer_bytes\": "
         << memory.brickMicroDrawArgsBufferBytes << ",\n";
  stream << "    \"brick_micro_total_buffer_bytes\": "
         << memory.brickMicroTotalBufferBytes << "\n";
  stream << "  },\n";

  stream << "  \"trace\": {\n";
  stream << "    \"output_hash\": \"" << hexadecimal(trace.outputHash) << "\",\n";
  stream << "    \"ray_hash\": \"" << hexadecimal(trace.rayHash) << "\",\n";
  stream << "    \"result_hash\": \"" << hexadecimal(trace.resultHash) << "\",\n";
  stream << "    \"ray_count\": " << trace.rayCount << ",\n";
  stream << "    \"status_counts\": [" << trace.statusCounts[0] << ", " << trace.statusCounts[1] << ", " << trace.statusCounts[2] << ", " << trace.statusCounts[3] << "],\n";
  stream << "    \"maximum_queries\": " << trace.maximumQueries << ",\n";
  stream << "    \"maximum_base_steps\": " << trace.maximumBaseSteps << "\n";
  stream << "  },\n";

  stream << "  \"occluder_invalidation\": {\n";
  stream << "    \"valid\": "
         << (occluderInvalidation.valid ? "true" : "false") << ",\n";
  stream << "    \"step_frame\": " << occluderInvalidation.stepFrame << ",\n";
  stream << "    \"frames_to_visibility_change\": "
         << occluderInvalidation.framesToVisibilityChange << ",\n";
  stream << "    \"frames_to_tile_wake\": "
         << occluderInvalidation.framesToTileWake << ",\n";
  stream << "    \"settled_tiles_before_step\": "
         << occluderInvalidation.settledTilesBeforeStep << ",\n";
  stream << "    \"awake_tiles_before_step\": "
         << occluderInvalidation.awakeTilesBeforeStep << ",\n";
  stream << "    \"peak_awake_tiles_after_step\": "
         << occluderInvalidation.peakAwakeTilesAfterStep << ",\n";
  stream << "    \"converged_tiles_at_peak\": "
         << occluderInvalidation.convergedTilesAtPeak << ",\n";
  stream << "    \"peak_change_attributed_rays\": "
         << occluderInvalidation.peakChangeAttributedRays << ",\n";
  stream << "    \"scheduled_rays_at_change_peak\": "
         << occluderInvalidation.scheduledRaysAtChangePeak << ",\n";
  stream << "    \"frames_to_resettle\": "
         << occluderInvalidation.framesToResettle << ",\n";
  stream << "    \"total_visibility_changes\": "
         << occluderInvalidation.totalVisibilityChanges << "\n";
  stream << "  },\n";

  stream << "  \"frames\": [\n";
  for (size_t index = 0u; index < orderedFrames.size(); ++index)
  {
    const auto &sample = orderedFrames[index];
    stream << "    {\n";
    stream << "      \"frame_index\": " << sample.frameIndex << ",\n";
    stream << "      \"cpu_total_ms\": ";
    writeNumber(stream, sample.cpuTotalMilliseconds);
    stream << ",\n      \"gpu_total_ms\": ";
    if (configuration.gpuTimingsAvailable)
      writeNumber(stream, sample.gpuTotalMilliseconds);
    else
      stream << "null";
    stream << ",\n      \"brick_phase0_total_ms\": ";
    if (configuration.gpuTimingsAvailable)
      writeNumber(stream, brickPhase0Total(sample));
    else
      stream << "null";
    stream << ",\n      \"occupancy_source_ms\": ";
    writeNumber(stream, sample.occupancySourceMilliseconds);
    stream << ",\n      \"occupancy_planning_ms\": ";
    writeNumber(stream, sample.occupancyPlanningMilliseconds);
    stream << ",\n      \"occupancy_upload_ms\": ";
    writeNumber(stream, sample.occupancyUploadMilliseconds);
    stream << ",\n      \"tree64_planning_ms\": ";
    writeNumber(stream, sample.tree64PlanningMilliseconds);
    stream << ",\n      \"tree64_upload_ms\": ";
    writeNumber(stream, sample.tree64UploadMilliseconds);
    stream << ",\n      \"cpu_sections\": ";
    writeNamedTimings(stream, sample.cpuSections, "      ");
    stream << ",\n      \"gpu_passes\": ";
    if (configuration.gpuTimingsAvailable)
      writeNamedTimings(stream, sample.gpuPasses, "      ");
    else
      stream << "{}";
    if (sample.rayBudget.valid)
    {
      const auto &budget = sample.rayBudget;
      stream << ",\n      \"ray_budget\": {"
             << "\"covered_tiles\": " << budget.coveredTiles
             << ", \"tiles_awake\": " << budget.tilesAwake
             << ", \"tiles_converged\": " << budget.tilesConverged
             << ", \"total_budget\": " << budget.totalBudget
             << ", \"rays_scheduled\": " << budget.raysScheduled
             << ", \"heartbeat_rays\": " << budget.heartbeatRays
             << ", \"disocclusion_rays\": " << budget.disocclusionRays
             << ", \"motion_rays\": " << budget.motionRays
             << ", \"lighting_change_rays\": " << budget.lightingChangeRays
             << ", \"noise_rays\": " << budget.noiseRays
             << ", \"unstable_tile_rays\": " << budget.unstableTileRays
             << ", \"resolved_rays\": " << budget.resolvedRays
             << ", \"visibility_changes\": " << budget.visibilityChanges
             << ", \"total_demand\": " << budget.totalDemand
             << ", \"covered_pixels\": " << budget.coveredPixels
             << "}";
    }
    if (sample.screenTrace.valid)
    {
      const auto &screen = sample.screenTrace;
      stream << ",\n      \"screen_trace\": {"
             << "\"active_rays\": " << screen.activeRays
             << ", \"node_visits\": " << screen.nodeVisits
             << ", \"leaf_visits\": " << screen.leafVisits
             << ", \"max_leaf_visits\": " << screen.maximumLeafVisits
             << ", \"hits\": " << screen.hits
             << ", \"miss_tmax\": " << screen.missTMax
             << ", \"escaped_scene\": " << screen.escapedScene
             << ", \"short_segments\": " << screen.shortSegments
             << "}";
    }
    if (sample.terrainFeedback.valid)
    {
      const auto &feedback = sample.terrainFeedback;
      stream << ",\n      \"terrain_feedback\": {"
             << "\"readback_submitted\": "
             << (feedback.readbackSubmitted ? "true" : "false")
             << ", \"readback_consumed\": "
             << (feedback.readbackConsumed ? "true" : "false")
             << ", \"skipped_for_cadence\": "
             << (feedback.skippedForCadence ? "true" : "false")
             << ", \"skipped_for_back_pressure\": "
             << (feedback.skippedForBackPressure ? "true" : "false")
             << ", \"age_frames\": " << feedback.ageFrames
             << ", \"request_count\": " << feedback.requestCount
             << ", \"queued_request_count\": "
             << feedback.queuedRequestCount
             << "}";
    }
    if (sample.terrainStreaming.valid)
    {
      const auto &streaming = sample.terrainStreaming;
      stream << ",\n      \"terrain_streaming\": {"
             << "\"resident_page_count\": "
             << streaming.residentPageCount
             << ", \"resident_page_capacity\": "
             << streaming.residentPageCapacity
             << ", \"pending_refinement_request_count\": "
             << streaming.pendingRefinementRequestCount
             << ", \"upload_submitted\": "
             << (streaming.uploadSubmitted ? "true" : "false")
             << ", \"upload_bytes_submitted\": "
             << streaming.uploadBytesSubmitted
             << ", \"upload_section_count\": "
             << streaming.uploadSectionCount
             << ", \"upload_publication_count\": "
             << streaming.uploadPublicationCount
             << ", \"upload_batch_index\": "
             << streaming.uploadBatchIndex
             << ", \"upload_batch_count\": "
             << streaming.uploadBatchCount
             << ", \"pending_upload_bytes\": "
             << streaming.pendingUploadBytes
             << ", \"pending_upload_batch_count\": "
             << streaming.pendingUploadBatchCount
             << ", \"upload_queue_latency_frames\": "
             << streaming.uploadQueueLatencyFrames
             << ", \"committed_tree64_page_count\": "
             << streaming.committedTree64PageCount
             << "}";
    }
    stream << "\n    }";
    if (index + 1u < orderedFrames.size())
      stream << ",";
    stream << "\n";
  }
  stream << "  ],\n";

  stream << "  \"aggregates\": {\n";
  stream << "    \"cpu_total_ms\": ";
  writeStatistics(stream, statistics(cpuTotals));
  stream << ",\n    \"gpu_total_ms\": ";
  if (configuration.gpuTimingsAvailable)
    writeStatistics(stream, statistics(gpuTotals));
  else
    stream << "null";
  stream << ",\n    \"brick_phase0_total_ms\": ";
  if (configuration.gpuTimingsAvailable)
    writeStatistics(stream, statistics(brickPhase0Totals));
  else
    stream << "null";
  stream << ",\n    \"occupancy_source_ms\": ";
  writeStatistics(stream, statistics(occupancySource));
  stream << ",\n    \"occupancy_planning_ms\": ";
  writeStatistics(stream, statistics(occupancyPlanning));
  stream << ",\n    \"occupancy_upload_ms\": ";
  writeStatistics(stream, statistics(occupancyUpload));
  stream << ",\n    \"tree64_planning_ms\": ";
  writeStatistics(stream, statistics(tree64Planning));
  stream << ",\n    \"tree64_upload_ms\": ";
  writeStatistics(stream, statistics(tree64Upload));
  stream << ",\n    \"terrain_upload_bytes_submitted\": ";
  writeStatistics(stream, statistics(terrainUploadBytes));
  stream << ",\n    \"cpu_sections\": ";
  writeNamedAggregates(stream, collectNamedTimings(orderedFrames, true));
  stream << ",\n    \"gpu_passes\": ";
  if (configuration.gpuTimingsAvailable)
    writeNamedAggregates(stream, collectNamedTimings(orderedFrames, false));
  else
    stream << "{}";
  stream << "\n  }\n";
  stream << "}\n";
  stream.flush();
  if (!stream)
  {
    if (error != nullptr)
      *error = "Could not finish writing benchmark output";
    return false;
  }
  return true;
}
} // namespace sparse_voxel_sample
