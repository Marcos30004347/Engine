#pragma once

#include "procedural/Predicates.hpp"

#include <cstddef>
#include <vector>

namespace procedural
{

// Tolerances the segment predicates compare against.
//
// These are not the predicates' error bounds - those are derived internally and
// are exact. They are the tolerances for questions that are genuinely about
// proximity rather than about sign, such as whether two endpoints should be
// treated as the same point.
inline constexpr double kPointEpsilon = 1.0e-9;
inline constexpr double kSegmentEpsilon = 1.0e-7;

double distance(const Point2 &a, const Point2 &b);
double dotProduct(const Point2 &a, const Point2 &b);

// -1, 0 or 1. Zero only for an exact zero, so this is safe to use on a
// predicate result and unsafe to use on a difference of two measurements.
int signOf(double value);

bool nearlyEqual(double a, double b);

// Whether q lies on the segment pr, assuming the three are already known to be
// collinear. This is the standard companion to an orientation test and does not
// itself test collinearity.
bool onSegment(const Point2 &p, const Point2 &q, const Point2 &r);

// Intersection of segments p1q1 and p2q2. Returns false when they do not cross;
// `result` is written only when it returns true.
bool segmentIntersection(
    const Point2 &p1, const Point2 &q1, const Point2 &p2, const Point2 &q2, Point2 &result);

// Whether two collinear segments share more than a single point.
bool segmentsOverlap(const Point2 &p1, const Point2 &q1, const Point2 &p2, const Point2 &q2);

// Sorts by x then y, and removes duplicates within kPointEpsilon.
//
// Duplicate points are not a cosmetic problem for a triangulator: two coincident
// vertices give a zero-area triangle, which has no orientation, so every
// subsequent sign test on it is meaningless.
void sortPoints(std::vector<Point2> &points);
size_t removeDuplicatePoints(std::vector<Point2> &points);

} // namespace procedural
