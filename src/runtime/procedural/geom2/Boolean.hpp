#pragma once

#include "procedural/geom2/Arrangement.hpp"

#include <functional>

namespace procedural::geom2 {

// Boolean operations, all four from one arrangement.
//
// Nothing here is convex-only, and nothing rejects concave input. The operands
// may overlap, touch, share edges, contain holes or be disjoint.

enum class FillRule : uint8_t
{
  // A face is inside an input when its winding for that input is non-zero.
  // The right rule for shapes assembled from rings that may nest or repeat,
  // which is what a settlement's allocated cells are.
  NonZero,

  // Inside when the winding is odd. The right rule when a caller has supplied
  // rings whose orientation is not trustworthy.
  EvenOdd
};

enum class Operation : uint8_t
{
  Union,
  Intersection,
  Difference, // In the first operand and in no other.
  Xor         // In an odd number of operands.
};

struct BooleanOptions
{
  FillRule rule = FillRule::NonZero;
  ArrangementLimits limits;
};

// One operand: any number of rings, which may include holes and disconnected
// parts. A polygon converts to one of these directly.
struct Operand
{
  std::vector<Ring> rings;
};

Operand toOperand(const Polygon &);

Operand toOperand(const Ring &);

// The result, as polygons in canonical form: outer rings counter-clockwise,
// holes clockwise and nested in their containing outer ring. Every returned
// polygon satisfies polygonFault.
bool booleanOp(const std::vector<Operand> &operands, Operation, const BooleanOptions &, std::vector<Polygon> &out, std::string *error = nullptr);

// Convenience forms.
bool unionOf(const std::vector<Operand> &, std::vector<Polygon> &out, std::string *error = nullptr);

bool intersectionOf(const std::vector<Operand> &, std::vector<Polygon> &out, std::string *error = nullptr);

bool differenceOf(const Operand &subject, const std::vector<Operand> &clips, std::vector<Polygon> &out, std::string *error = nullptr);

// The general form: an arbitrary predicate on the winding vector.
//
// Exposed because the interesting questions this generator asks are not the
// four named operations. "The face is inside the cell, inside the road band and
// outside every building" is one predicate over the winding vector, answered
// from the same arrangement, and the A | ribbon | B split of a widthed edge is
// another.
bool selectFaces(const std::vector<Operand> &operands, const std::function<bool(const std::vector<int32_t> &winding)> &inside, const BooleanOptions &, std::vector<Polygon> &out, std::string *error = nullptr);

// Selects faces from an arrangement already built, so several questions can be
// asked of one sweep rather than one sweep each.
bool extractFaces(const Arrangement &, const std::function<bool(const std::vector<int32_t> &winding)> &inside, std::vector<Polygon> &out, std::string *error = nullptr);

// As extractFaces, but emits one polygon per connected piece rather than
// merging everything the predicate selected.
//
// Two selected faces belong to the same piece when they share an edge that is
// not `hard`. A hard edge comes from an open path - a cut line, a zero-width
// centreline - so this is what makes a cut return the pieces it produced
// instead of one polygon with an invisible seam through it.
bool extractRegions(const Arrangement &, const std::function<bool(const std::vector<int32_t> &winding)> &inside, std::vector<Polygon> &out, std::string *error = nullptr);

} // namespace procedural::geom2
