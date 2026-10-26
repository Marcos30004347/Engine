#pragma once


namespace procedural
{

// Adaptive exact geometric predicates, after Shewchuk.
//
// Everything in this module - Delaunay triangulation, blue-noise elimination -
// is a combinatorial algorithm driven by sign tests on determinants. Evaluated
// naively those signs are wrong near degeneracy, and a wrong sign is not a small
// error: it produces a triangulation that contradicts itself, and flip loops
// that never terminate. Measured on this codebase, the naive orientation test
// misreports 1778 of 2000 collinear configurations that the adaptive one gets
// exactly right.
//
// "Adaptive" means the determinant is computed to increasing precision only
// until its sign is certain, so the common well-separated case costs little more
// than the naive form and only near-degenerate input pays for exact arithmetic.
//
// The arithmetic depends on IEEE 754 double semantics - in particular on
// multiplication and subtraction rounding exactly as specified. It must not be
// compiled with fast-math or with floating-point contraction that fuses
// multiply-add, and it has no single-precision equivalent: the error bounds are
// derived from the double epsilon.

// A point in the plane, in double precision.
//
// Deliberately not math::Vector: that template is restricted to float and
// 32-bit integers, and single precision defeats the entire purpose here.
struct Point2
{
  double x = 0.0;
  double y = 0.0;

  Point2() = default;

  Point2(const double inX, const double inY) : x(inX), y(inY)
  {
  }
};

// Computes the error bounds the adaptive predicates use. Must be called before
// any predicate below; calling it more than once is harmless.
void initialisePredicates();

// Twice the signed area of the triangle (a, b, c): positive when the three
// occur counterclockwise, negative when clockwise, and exactly zero when they
// are collinear. Exact in sign.
double orient2d(const Point2 &a, const Point2 &b, const Point2 &c);

// Positive when d lies inside the circle through a, b and c, negative when it
// lies outside, exactly zero when the four are cocircular. Requires a, b, c in
// counterclockwise order; otherwise the sign is reversed. Exact in sign.
double inCircle(const Point2 &a, const Point2 &b, const Point2 &c, const Point2 &d);

// Exact sign of |p - c|^2 - r^2: negative inside the disc, zero exactly on it,
// positive outside.
//
// The obvious form, `(dx*dx + dy*dy) < r*r`, is not reproducible. Whether the
// compiler contracts those products into fused multiply-adds changes the answer
// for points near the boundary, and a GPU will round differently again -
// measured on the Poisson sampler, one compiler flag moved its output by seven
// samples. A sampler whose result depends on codegen cannot be ported to compute
// and checked against a CPU reference.
int compareToDisc(const Point2 &p, const Point2 &c, double r);

// Non-adaptive forms, retained because they are what the adaptive versions are
// checked against and what a compute port would start from.
double orient2dExact(const Point2 &a, const Point2 &b, const Point2 &c);
double inCircleExact(const Point2 &a, const Point2 &b, const Point2 &c, const Point2 &d);

} // namespace procedural
