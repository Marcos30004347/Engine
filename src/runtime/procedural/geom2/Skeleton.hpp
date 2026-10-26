#pragma once

#include "Polygon.hpp"

#include <string>
#include <vector>

namespace procedural::geom2 {

// Equal-pitch roofs over polygonal footprints, courts included: the straight
// skeleton, as the L-infinity distance to the eaves.
//
// The height over a point is the distance to the nearest eave measured in that
// eave's own frame. Aichholzer et al. (1995) give the exact condition under
// which that is the straight skeleton - "if P is rectilinear then the straight
// skeleton is the medial axis of P for the L-infinity metric" - and Ahn et al.
// (2013) restate it as z(p) = d_inf(p, boundary). A convex footprint is the
// lower envelope of its inward edge planes, which is the straight skeleton too.
//
// So this takes plans whose every REFLEX CORNER IS A RIGHT ANGLE. Convex
// corners may be any angle, and the plan may sit at any rotation: what the
// construction needs is that a wall's claim, cut off square to itself at its
// ends, is cut where the angular bisector with its neighbour would have cut it,
// and at a square corner those are the same line however the plan is turned.
// A plan with an oblique reflex corner is REFUSED, not approximated. There is
// no local repair for it - the same paper proves a roof is not the lower
// envelope of functions defined locally by each edge, since moving a distant
// reflex vertex changes which roof is correct here - and the approximation it
// replaced put the ridge of a sheared L 1.8 m out on a 4 m roof.
//
// Gabled eaves are omitted from the distance rather than projected onto the
// facade, so the ridge runs out to the gable wall instead of stopping a
// half-width short of it.
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

// The highest point of a finished roof, and how high.
//
// Where the ridge peaks, and - applied to a ROOM's polygon rather than a
// building's - the point deepest inside that room, which is a much better
// answer to "where does the ceiling light go" than a centroid on any plan that
// is not convex. Watabou uses his skeleton for exactly that.
bool roofSummit(const std::vector<RoofTriangle> &, double &x, double &z, double &height);

// Whether every reflex corner of the polygon is a right angle.
//
// The precondition `roofFromSkeleton` refuses without, offered separately so a
// caller can check before building rather than after failing. A polyomino
// outline satisfies it by construction; what can break it is a caller that has
// offset or simplified the ring into something oblique first, and that is the
// thing worth catching.
bool rectilinearReflex(const Polygon &);

} // namespace procedural::geom2
