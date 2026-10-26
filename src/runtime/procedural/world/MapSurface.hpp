#pragma once

#include "procedural/world/MapGraph.hpp"
#include "procedural/world/land/Blend.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace procedural::world {

struct MapSurfaceSample
{
  double elevationMetres = 0.0;
  double upliftRateMetresPerYear = 0.0;
  double reliefAmplitudeMetres = 0.0;
  double roughness = 0.0;
};

struct MapSurfaceSettings
{
  // Zero derives a spacing from mean leaf-cell size. The default samples more
  // finely than the graph without manufacturing terrain detail below map
  // resolution. The hard side limit bounds memory on very large worlds.
  double sampleSpacingMetres = 0.0;
  double automaticSpacingScale = 0.65;
  uint32_t minimumGridSide = 16u;
  uint32_t maximumGridSide = 512u;
};

// Shared continuous scalar surface for contours and both renderers.
//
// Samples are authored once from the graph's smooth Shepard field, then
// evaluated as a watertight piecewise-linear grid. Contour marching uses the
// same triangle diagonal as sample(), so an unsmoothed isoline and a rendered
// height are two views of exactly the same surface.
class MapSurfaceField
{
public:
  bool build(const MapGraph &graph, const SiteIndex &index, const BlendSettings &blend, const MapSurfaceSettings &settings = {}, std::string *error = nullptr);

  MapSurfaceSample sample(Point2 position) const;
  const MapSurfaceSample &at(uint32_t x, uint32_t y) const;

  bool empty() const
  {
    return samples_.empty();
  }

  uint32_t gridSide() const
  {
    return gridSide_;
  }

  double extentMetres() const
  {
    return extentMetres_;
  }

  double spacingMetres() const
  {
    return spacingMetres_;
  }

private:
  uint32_t gridSide_ = 0u;
  double extentMetres_ = 0.0;
  double spacingMetres_ = 0.0;
  std::vector<MapSurfaceSample> samples_;
};

} // namespace procedural::world
