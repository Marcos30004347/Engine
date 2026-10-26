#pragma once

#include "procedural/geom2/Boolean.hpp"

namespace procedural::geom2 {

// Cutting and offsetting, both built on the boolean rather than beside it.
//
// That is not an implementation convenience. A naive offset self-intersects the
// moment a reflex notch closes, and the robust resolution of those loops is a
// winding-number union - which is the boolean. Building offset on top is what
// lets it survive concave input instead of quietly producing a folded ring.

enum class JoinStyle : uint8_t
{
  // Extend both offset edges to their intersection. A corner sharper than the
  // miter limit is bevelled instead, so a near-spike cannot throw the corner
  // an unbounded distance from the original.
  Miter,

  // Cut the corner straight across.
  Bevel,

  // Approximate the arc with segments no further than the tolerance from it.
  Round
};

struct OffsetOptions
{
  JoinStyle join = JoinStyle::Miter;

  // Multiples of the offset distance. A corner whose miter would exceed this
  // is bevelled.
  double miterLimit = 2.5;

  // Largest deviation of a round join's chords from the true arc, in metres.
  double arcToleranceMetres = 0.05;

  ArrangementLimits limits;
};

// Offsets a polygon by `metres`: positive grows it, negative shrinks it.
//
// The result may have more parts than the input, or fewer, or none:
//
//  - shrinking a U past its notch width splits it into two;
//  - shrinking any shape far enough empties it, which is a result and not a
//    failure;
//  - growing a U enough merges its two arms into one ring, closing the notch
//    into a hole and then closing the hole.
//
// All four are ordinary outcomes of the winding resolution, not special cases.
bool offsetPolygon(const Polygon &, double metres, const OffsetOptions &, std::vector<Polygon> &out, std::string *error = nullptr);

bool offsetPolygons(const std::vector<Polygon> &, double metres, const OffsetOptions &, std::vector<Polygon> &out, std::string *error = nullptr);

// Offsets an open polyline into a closed band of half-width `metres`.
//
// This is the widthed edge of the region model: a road or a river centreline
// becomes the ribbon that splits a region into side A, the ribbon, and side B.
bool offsetPolyline(const std::vector<Point> &path, double halfWidthMetres, const OffsetOptions &, std::vector<Polygon> &out, std::string *error = nullptr);

// The same, with a half width per point: the ribbon tapers along its length.
//
// This is what a river is. Discharge grows downstream, so a channel enters a
// reach at one width and leaves it at another, and the width a reach leaves at
// is the width the next reach enters at. The whole course is therefore one
// tapered ribbon, not a chain of separately-widened segments - and it has to be
// built as one polygon, because subtracting each segment's own quad from the
// land in turn leaves a notch at every bend where two quads of different width
// meet.
//
// `halfWidths` has one entry per point of `path`.
bool offsetPolylineTapered(const std::vector<Point> &path, const std::vector<double> &halfWidthsMetres, const OffsetOptions &, std::vector<Polygon> &out, std::string *error = nullptr);

// Cuts a polygon with a line through `a` and `b`, returning every piece.
//
// N pieces, not two. A chord across a concave polygon can produce three or
// more, and a two-output signature would be a convexity assumption in the API -
// which is exactly how the existing LotSplitter came to refuse the cases it
// cannot express.
bool cutPolygon(const Polygon &, const Point &a, const Point &b, std::vector<Polygon> &out, std::string *error = nullptr);

// Cuts with a chord of finite extent: the segment is extended to the polygon's
// bounding box first, so a caller can name two points on the boundary without
// computing how far the cut must run.
bool cutPolygonBySegment(const Polygon &, const Point &a, const Point &b, std::vector<Polygon> &out, std::string *error = nullptr);

} // namespace procedural::geom2
