#pragma once
#include "procedural/Geometry.hpp"
#include <cstdint>
#include <functional>
#include <vector>

namespace procedural::geometry
{
// Contours for the regions that had none.
//
// The mesher gives a face the material at its centroid. Where a region's edge
// is a constraint, no triangle can straddle it and the edge comes out clean;
// where it is not, triangles sit astride the edge and the boundary degenerates
// into a zigzag of triangle edges. That is the whole of the triangular-tongue
// artefact along shores and biome boundaries, and the two regions that show it
// are exactly the two defined by a sample test rather than by a polygon.
//
// Farm patches, city blocks, roads and river banks already carry polygons and
// already read clean. This gives the same to the sand band and to the biome
// boundaries: marching squares over the scalar that defines the region, then
// simplified and smoothed, so the result is a coastline rather than a staircase
// of grid cells.
struct RegionContourSettings
{
  // Grid the field is marched on. Finer resolves a bay; coarser is faster. At
  // 2048 over a 256 km world a cell is 125 m, which is the coarse map's own
  // resolution and the most it can say.
  uint32_t resolution = 2048u;
  // Douglas-Peucker tolerance. Marching squares emits a vertex per cell
  // crossing, which is far more than the shape carries.
  double simplifyMetres = 26.0;
  // Chord-smoothing passes applied after simplification, and how far each
  // vertex moves toward the midpoint of its neighbours. This is where a
  // believable shoreline is won: raw marching-squares output is axis-aligned.
  uint32_t smoothingRounds = 3u;
  double smoothing = .38;
  // Rings shorter than this are dropped - a two-cell island is noise, not a
  // feature, and it costs the triangulator more than it is worth.
  double minimumPerimeterMetres = 420.0;
};

// Extracts the boundary of `{p : inside(p)}` as closed rings, counter-clockwise
// around the inside. Deterministic for a given predicate and grid.
std::vector<std::vector<Point2>> extractRegionContours(double extentMetres,
    const std::function<bool(Point2)> &inside, const RegionContourSettings &settings = {});

// Iso-contours of a scalar field at `level`, with the crossing interpolated
// along each grid edge - so a shore band follows the actual distance contour
// rather than a half-cell lattice. Use this wherever the field is continuous;
// `extractRegionContours` is the boolean convenience form.
std::vector<std::vector<Point2>> extractIsoContours(double extentMetres,
    const std::function<double(Point2)> &field, double level,
    const RegionContourSettings &settings = {});

// The same, for a field that takes more than two values: one set of rings per
// boundary between differing values. `label` must be cheap; it is called once
// per grid vertex. Categorical, so crossings stay at edge midpoints - there is
// no sense in which a boundary is "40% of the way into a biome".
std::vector<std::vector<Point2>> extractLabelContours(double extentMetres,
    const std::function<uint8_t(Point2)> &label, const RegionContourSettings &settings = {});
}
