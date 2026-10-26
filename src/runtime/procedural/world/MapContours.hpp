#pragma once

#include "procedural/world/MapGraph.hpp"
#include "procedural/world/MapSurface.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace procedural::world {

struct ContourSettings
{
  // Vertical distance between neighbouring isolines.
  double intervalMetres = 100.0;
  // Every Nth line is an index contour and may be drawn more heavily.
  uint32_t majorEvery = 5u;
  double minimumElevationMetres = 0.0;
  double maximumElevationMetres = std::numeric_limits<double>::infinity();
  bool includeMinimum = false;
  // Marching segments shorter than this are treated as collapsed. Joining is
  // topological—by shared triangulation-edge identity—not distance based. Zero
  // selects a scale from the world extent.
  double minimumSegmentMetres = 0.0;
  // Samples emitted along each input span after the topological contour graph
  // has been joined. One preserves the piecewise-linear marching result;
  // values above one use interpolating centripetal Catmull-Rom curves.
  uint32_t curveSamplesPerSpan = 4u;
  double curveTension = 0.5;
};

// An isoline in world metres. The elevation is retained with the geometry so
// this is useful beyond drawing: voxel material bands, snow lines and distance
// effects can consume the same derived graph without sampling a raster.
struct ContourPolyline
{
  double elevationMetres = 0.0;
  std::vector<Point2> points;
  bool closed = false;
  bool major = false;
};

struct ContourSet
{
  std::vector<ContourPolyline> lines;
  double intervalMetres = 0.0;

  size_t pointCount() const;

  bool empty() const
  {
    return lines.empty();
  }
};

// Convenience path for callers which only own a graph. It constructs the
// default shared scalar surface and delegates to the preferred overload below.
bool extractElevationContours(const MapGraph &graph, const ContourSettings &settings, ContourSet &contours, std::string *error = nullptr);

// Preferred path: contours march the exact same watertight triangle grid
// sampled by the heightmap and uplift renderers. Crossings join by shared-edge
// identity into deterministic closed polylines. Curve sampling is a final
// presentation step; the unsmoothed points remain exact isolines of surface.
bool extractElevationContours(const MapSurfaceField &surface, const ContourSettings &settings, ContourSet &contours, std::string *error = nullptr);

} // namespace procedural::world
