#include "procedural/world/MapSurface.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world {

namespace {

MapSurfaceSample blendSample(const MapGraph &graph, const std::vector<BlendWeight> &weights)
{
  MapSurfaceSample sample{};
  for (const BlendWeight &weight : weights)
  {
    const NodeIntent &intent = graph.node(weight.node).intent;
    sample.elevationMetres += weight.weight * intent.elevationMetres;
    sample.upliftRateMetresPerYear += weight.weight * intent.upliftRate;
    sample.reliefAmplitudeMetres += weight.weight * intent.reliefAmplitude;
    sample.roughness += weight.weight * intent.roughness;
  }
  return sample;
}

MapSurfaceSample interpolate(const MapSurfaceSample &a, const MapSurfaceSample &b, const MapSurfaceSample &c, const double wa, const double wb, const double wc)
{
  return MapSurfaceSample{
    a.elevationMetres * wa + b.elevationMetres * wb + c.elevationMetres * wc,
    a.upliftRateMetresPerYear * wa + b.upliftRateMetresPerYear * wb + c.upliftRateMetresPerYear * wc,
    a.reliefAmplitudeMetres * wa + b.reliefAmplitudeMetres * wb + c.reliefAmplitudeMetres * wc,
    a.roughness * wa + b.roughness * wb + c.roughness * wc};
}

} // namespace

bool MapSurfaceField::build(const MapGraph &graph, const SiteIndex &index, const BlendSettings &blend, const MapSurfaceSettings &settings, std::string *error)
{
  const auto fail = [&](const std::string &message) {
    gridSide_ = 0u;
    extentMetres_ = 0.0;
    spacingMetres_ = 0.0;
    samples_.clear();
    if (error != nullptr)
    {
      *error = message;
    }
    return false;
  };

  if (graph.nodeCount() == 0u || index.empty())
  {
    return fail("map surface requires a populated graph and site index");
  }
  const double extent = graph.brief().extentMetres;
  if (!(extent > 0.0) || !std::isfinite(extent))
  {
    return fail("map surface extent must be finite and positive");
  }
  if (!std::isfinite(settings.sampleSpacingMetres) || settings.sampleSpacingMetres < 0.0 || !std::isfinite(settings.automaticSpacingScale) || !(settings.automaticSpacingScale > 0.0))
  {
    return fail("map surface spacing settings are invalid");
  }
  if (settings.minimumGridSide < 2u || settings.maximumGridSide < settings.minimumGridSide)
  {
    return fail("map surface grid-side limits are invalid");
  }

  uint32_t leaves = 0u;
  for (const MapNode &node : graph.nodes())
  {
    leaves += !node.refined() ? 1u : 0u;
  }
  if (leaves == 0u)
  {
    return fail("map surface found no graph leaves");
  }
  const double automatic = extent / std::sqrt(static_cast<double>(leaves)) * settings.automaticSpacingScale;
  const double wanted = settings.sampleSpacingMetres > 0.0 ? settings.sampleSpacingMetres : automatic;
  gridSide_ = std::clamp(static_cast<uint32_t>(std::ceil(extent / std::max(1.0, wanted))) + 1u, settings.minimumGridSide, settings.maximumGridSide);
  extentMetres_ = extent;
  spacingMetres_ = extent / static_cast<double>(gridSide_ - 1u);
  samples_.assign(static_cast<size_t>(gridSide_) * gridSide_, MapSurfaceSample{});

  std::vector<BlendWeight> weights;
  for (uint32_t y = 0u; y < gridSide_; ++y)
  {
    for (uint32_t x = 0u; x < gridSide_; ++x)
    {
      const Point2 position(static_cast<double>(x) * spacingMetres_, static_cast<double>(y) * spacingMetres_);
      blendAt(graph, index, position, blend, weights);
      if (weights.empty())
      {
        return fail("map surface could not sample the graph");
      }
      samples_[static_cast<size_t>(y) * gridSide_ + x] = blendSample(graph, weights);
    }
  }
  return true;
}

const MapSurfaceSample &MapSurfaceField::at(const uint32_t x, const uint32_t y) const
{
  return samples_[static_cast<size_t>(y) * gridSide_ + x];
}

MapSurfaceSample MapSurfaceField::sample(Point2 position) const
{
  if (empty() || gridSide_ < 2u)
  {
    return {};
  }
  position.x = std::clamp(position.x, 0.0, extentMetres_);
  position.y = std::clamp(position.y, 0.0, extentMetres_);
  const double gx = position.x / spacingMetres_;
  const double gy = position.y / spacingMetres_;
  const uint32_t x = std::min(static_cast<uint32_t>(std::floor(gx)), gridSide_ - 2u);
  const uint32_t y = std::min(static_cast<uint32_t>(std::floor(gy)), gridSide_ - 2u);
  const double tx = std::clamp(gx - static_cast<double>(x), 0.0, 1.0);
  const double ty = std::clamp(gy - static_cast<double>(y), 0.0, 1.0);
  const MapSurfaceSample &p00 = at(x, y);
  const MapSurfaceSample &p10 = at(x + 1u, y);
  const MapSurfaceSample &p11 = at(x + 1u, y + 1u);
  const MapSurfaceSample &p01 = at(x, y + 1u);
  // Same lower-left to upper-right diagonal used by contour marching.
  if (ty <= tx)
  {
    return interpolate(p00, p10, p11, 1.0 - tx, tx - ty, ty);
  }
  return interpolate(p00, p11, p01, 1.0 - ty, tx, ty - tx);
}

} // namespace procedural::world
