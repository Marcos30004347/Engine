#pragma once

#include "procedural/Predicates.hpp"

#include <cmath>
#include <cstdint>

namespace procedural::geom2 {

// The fixed integer grid every geometry operation works on.
//
// Coordinates are quantised once, on entry, and every later decision is made on
// integers. That is what makes the arrangement reproducible: on the grid,
// equality is equality, welding needs no tolerance, and the exact predicates are
// exact about the points actually being processed rather than about points the
// caller nearly supplied.
//
// The scale is part of the generation contract. It matches
// `PatchTiling::tilingKey`, which already quantises to micrometres for welding,
// so a polygon that survives one is unchanged by the other. Changing it changes
// which lots exist in a generated city, so it is a world-format change and not a
// tuning knob.
inline constexpr double kUnitsPerMetre = 1.0e6;

inline constexpr double kMetresPerUnit = 1.0 / kUnitsPerMetre;

// Largest coordinate accepted, in grid units.
//
// Not the int64 limit, and not set by the predicates either. The binding
// constraint is the arrangement: locating the intersection of two segments
// multiplies a coordinate difference by a cross product, which is a product of
// *three* differences. With coordinates bounded by 2^41 the differences reach
// 2^42 and that triple product reaches 2^126, which is the largest value that
// still fits an __int128.
//
// 2^41 micrometres is about 2,200 km, two orders of magnitude beyond the 64 km
// worlds this generator builds. Trading unused range for an exact intersection
// is the right way round: the alternative is 256-bit arithmetic in the hottest
// loop of the library to serve a world size nothing asks for.
inline constexpr int64_t kMaximumUnit = int64_t(1) << 41;

// A point on the grid. Integer, so two of these are equal or they are not.
struct Point
{
  int64_t x = 0;
  int64_t y = 0;

  Point() = default;

  Point(const int64_t inX, const int64_t inY) : x(inX), y(inY)
  {
  }

  bool operator==(const Point &other) const
  {
    return x == other.x && y == other.y;
  }

  bool operator!=(const Point &other) const
  {
    return !(*this == other);
  }

  // Lexicographic, which is the sweep order: by x, then by y.
  bool operator<(const Point &other) const
  {
    return x != other.x ? x < other.x : y < other.y;
  }
};

// Whether a metre value lands inside the representable grid once scaled.
//
// Checked rather than clamped: a coordinate outside the grid means the caller
// has a bug or a unit mismatch, and silently folding it to the boundary would
// produce a plausible wrong polygon instead of an error.
inline bool representable(const double metres)
{
  if (!std::isfinite(metres))
  {
    return false;
  }

  return std::abs(metres) * kUnitsPerMetre <= double(kMaximumUnit);
}

// Metres to grid units, rounding half away from zero.
//
// Symmetric about zero on purpose: banker's rounding would make the quantised
// image of a polygon depend on which side of the origin it sits, so translating
// a shape by a whole number of metres could change its shape.
inline int64_t toUnit(const double metres)
{
  return int64_t(std::round(metres * kUnitsPerMetre));
}

inline double toMetres(const int64_t unit)
{
  return double(unit) * kMetresPerUnit;
}

inline Point toPoint(const Point2 &p)
{
  return {toUnit(p.x), toUnit(p.y)};
}

inline Point2 toPoint2(const Point &p)
{
  return {toMetres(p.x), toMetres(p.y)};
}

// The predicates take doubles, and every grid coordinate up to kMaximumUnit is
// exactly representable as a double, so this conversion is lossless and the
// exact predicates stay exact.
inline Point2 exact(const Point &p)
{
  return {double(p.x), double(p.y)};
}

} // namespace procedural::geom2
