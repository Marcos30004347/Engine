#include "procedural/Geometry.hpp"

#include <algorithm>
#include <cmath>

namespace procedural
{

double distance(const Point2 &a, const Point2 &b)
{
  const double dx = a.x - b.x;
  const double dy = a.y - b.y;
  return std::sqrt(dx * dx + dy * dy);
}

double dotProduct(const Point2 &a, const Point2 &b)
{
  return a.x * b.x + a.y * b.y;
}

int signOf(const double value)
{
  if (value > 0.0) return 1;
  if (value < 0.0) return -1;
  return 0;
}

bool nearlyEqual(const double a, const double b)
{
  return std::fabs(a - b) < kSegmentEpsilon;
}

bool onSegment(const Point2 &p, const Point2 &q, const Point2 &r)
{
  return q.x <= std::fmax(p.x, r.x) && q.x >= std::fmin(p.x, r.x) &&
      q.y <= std::fmax(p.y, r.y) && q.y >= std::fmin(p.y, r.y);
}

bool segmentIntersection(
    const Point2 &p1, const Point2 &q1, const Point2 &p2, const Point2 &q2, Point2 &result)
{
  // Signs come from the exact predicate, so "do these cross" is decided
  // combinatorially and only the crossing *position* is computed in ordinary
  // arithmetic. Deciding the first question with the second's arithmetic is how
  // a segment ends up both crossing and not crossing its neighbour.
  const int o1 = signOf(orient2d(p1, q1, p2));
  const int o2 = signOf(orient2d(p1, q1, q2));
  const int o3 = signOf(orient2d(p2, q2, p1));
  const int o4 = signOf(orient2d(p2, q2, q1));

  if (o1 != o2 && o3 != o4)
  {
    const double x1 = p1.x, y1 = p1.y;
    const double x2 = q1.x, y2 = q1.y;
    const double x3 = p2.x, y3 = p2.y;
    const double x4 = q2.x, y4 = q2.y;

    const double denominator = (y4 - y3) * (x2 - x1) - (x4 - x3) * (y2 - y1);
    if (denominator == 0.0) return false;

    const double ua = ((x4 - x3) * (y1 - y3) - (y4 - y3) * (x1 - x3)) / denominator;
    result = Point2(x1 + ua * (x2 - x1), y1 + ua * (y2 - y1));
    return true;
  }

  // Collinear touching cases. Each needs the orientation to be exactly zero,
  // which is a question only the exact predicate can answer.
  if (o1 == 0 && onSegment(p1, p2, q1))
  {
    result = p2;
    return true;
  }
  if (o2 == 0 && onSegment(p1, q2, q1))
  {
    result = q2;
    return true;
  }
  if (o3 == 0 && onSegment(p2, p1, q2))
  {
    result = p1;
    return true;
  }
  if (o4 == 0 && onSegment(p2, q1, q2))
  {
    result = q1;
    return true;
  }

  return false;
}

bool segmentsOverlap(const Point2 &p1, const Point2 &q1, const Point2 &p2, const Point2 &q2)
{
  if (orient2d(p1, q1, p2) != orient2d(p1, q1, q2)) return false;
  if (orient2d(p2, q2, p1) != orient2d(p2, q2, q1)) return false;
  if (orient2d(p1, q1, p2) != 0.0) return false;

  return onSegment(p1, p2, q1) || onSegment(p1, q2, q1) || onSegment(p2, p1, q2) ||
      onSegment(p2, q1, q2);
}

void sortPoints(std::vector<Point2> &points)
{
  std::sort(
      points.begin(), points.end(),
      [](const Point2 &a, const Point2 &b)
      {
        if (a.x != b.x) return a.x < b.x;
        return a.y < b.y;
      });
}

size_t removeDuplicatePoints(std::vector<Point2> &points)
{
  sortPoints(points);
  const auto last = std::unique(
      points.begin(), points.end(),
      [](const Point2 &a, const Point2 &b)
      {
        return std::fabs(a.x - b.x) < kPointEpsilon && std::fabs(a.y - b.y) < kPointEpsilon;
      });
  const size_t removed = static_cast<size_t>(std::distance(last, points.end()));
  points.erase(last, points.end());
  return removed;
}

} // namespace procedural
