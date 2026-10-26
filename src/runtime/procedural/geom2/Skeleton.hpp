#pragma once

#include "Polygon.hpp"

#include <string>
#include <vector>

namespace procedural::geom2 {

// Continuous equal-pitch roofs over polygonal footprints (including courts).
// Convex footprints use the lower envelope of their inward edge planes.
// Orthogonal footprints use the L-infinity distance to finite eaves, whose
// inward offsets produce the straight skeleton. Gabled eaves are omitted.
// For oblique concave plans the same finite-eave distance construction is a
// polygonal roof metric, not the general Euclidean straight skeleton.
//
// Build a convex subdivision of the continuous height field, share all boundary
// vertices, then triangulate each cell without removing collinear vertices.
struct RoofTriangle
{
  // Metres, in the polygon's own frame; y is height above the eaves.
  double ax, ay, az;
  double bx, by, bz;
  double cx, cy, cz;

  // The wall of the outline whose face this belongs to.
  uint32_t edge = 0;

  // True when the face is vertical: a gable wall, not a slope.
  bool gable = false;
};

struct RoofOptions
{
  // Height gained per metre the outline shrinks. Ignored when `ridgeHeight` is
  // positive, which sets the pitch so the highest point lands there instead.
  double slope = .5;

  // If positive, the pitch is chosen so the roof's highest point is exactly
  // this far above the eaves. A roof's height is a property of the building,
  // and the skeleton is what decides where its peak falls.
  double ridgeHeight = 0;

  // Which walls of `outline` the roof rises to rather than falls from: a gable
  // end, or a party wall shared with a taller mass. Empty leaves every face
  // hipped.
  //
  // Indexed as the walls are walked: the outer ring first, then each hole in
  // turn, skipping any zero-length edge. A request to raise every wall is
  // refused: a roof must have at least one eave.
  std::vector<bool> gables;
};

// Builds the roof over `ground`, in metres.
//
// Courtyards remain open. A building wrapped round a courtyard is an
// annulus, and the range between its outer wall and the court is roofed by
// those two walls meeting - which needs no special case once both are walls,
// and is the reason this takes a polygon rather than a ring.
bool roofFromSkeleton(const Polygon &ground, const RoofOptions &, std::vector<RoofTriangle> &out, std::string *error = nullptr);

// The same, for a plain outline with no court.
bool roofFromSkeleton(const std::vector<std::pair<double, double>> &outline, const RoofOptions &, std::vector<RoofTriangle> &out, std::string *error = nullptr);

} // namespace procedural::geom2
