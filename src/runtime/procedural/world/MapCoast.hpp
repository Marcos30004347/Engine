#pragma once

#include "procedural/world/MapGraph.hpp"

#include <string>
#include <vector>

namespace procedural::world
{

// An ordered run of coastline. Closed when the run returns to its start, which
// is the normal case for an island; open runs occur where a coast leaves the
// world through the border.
struct CoastPolyline
{
  std::vector<Point2> points;
  bool closed = false;
};

// Clips every cell polygon to the world rectangle [0, extent] x [0, extent].
//
// Cells on the convex hull of the site set have unbounded Voronoi regions, so
// buildPartition emits them from whatever incident circumcentres exist and they
// can reach past the world boundary. Clipping is what makes the border finite
// and is required before anything renders or measures area.
//
// Voronoi cells are convex, so Sutherland-Hodgman against the four half-planes
// is exact here rather than an approximation.
// Sutherland-Hodgman against an arbitrary convex polygon, wound
// counter-clockwise. Exact rather than approximate, because both the subject
// (a Voronoi cell) and the window (its parent cell) are convex.
//
// Returns an empty polygon when the subject lies wholly outside.
std::vector<Point2> clipPolygonToConvex(
    const std::vector<Point2> &subject, const std::vector<Point2> &window);

bool clipCellsToExtent(
    MapGraph &graph, double extentMetres, std::string *error = nullptr);

// Extracts the land/water boundary as ordered polylines.
//
// Segments are the Voronoi edges shared by a land cell and a water cell. They
// are chained end to end so a caller gets coastlines rather than a soup of
// unordered segments, which is what a renderer and a later fjord or beach pass
// both need.
std::vector<CoastPolyline> extractCoastline(
    const MapGraph &graph, double toleranceMetres);

} // namespace procedural::world
