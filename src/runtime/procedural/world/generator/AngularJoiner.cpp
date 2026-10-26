#include "procedural/world/generator/AngularJoiner.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world::generator {

namespace {
using rewrite::Position;
double cross(const Position &a, const Position &b, const Position &c)
{
  return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}
bool same(const Position &a, const Position &b)
{
  return std::hypot(a.x - b.x, a.z - b.z) < 1e-6;
}
}

bool buildAngularJoiner(const Position &a0,
                        const Position &a1,
                        const Position &b0,
                        const Position &b1,
                        AngularJoiner &out,
                        std::string *error)
{
  out = AngularJoiner{};
  const double y = (a0.y + a1.y + b0.y + b1.y) * .25;
  if (std::max({std::abs(a0.y - y), std::abs(a1.y - y), std::abs(b0.y - y), std::abs(b1.y - y)}) > .05)
  {
    if (error) *error = "an angular joiner needs both connection edges at one level";
    return false;
  }
  out.a = {a0, a1}; out.b = {b0, b1};
  std::vector<Position> points{a0, a1, b0, b1};
  for (Position &p : points) p.y = y;
  std::sort(points.begin(), points.end(), [](const Position &p, const Position &q) {
    return p.x < q.x - 1e-9 || (std::abs(p.x - q.x) < 1e-9 && p.z < q.z);
  });
  points.erase(std::unique(points.begin(), points.end(), same), points.end());
  if (points.size() < 3)
  {
    if (error) *error = "the two connection edges do not enclose a joiner";
    return false;
  }
  std::vector<Position> hull(points.size() * 2);
  size_t n = 0;
  for (const Position &p : points)
  {
    while (n >= 2 && cross(hull[n - 2], hull[n - 1], p) <= 1e-9) --n;
    hull[n++] = p;
  }
  const size_t lower = n;
  for (size_t i = points.size() - 1; i-- > 0;)
  {
    const Position &p = points[i];
    while (n > lower && cross(hull[n - 2], hull[n - 1], p) <= 1e-9) --n;
    hull[n++] = p;
  }
  if (n > 1) --n;
  hull.resize(n);
  if (hull.size() < 3)
  {
    if (error) *error = "the angular joiner is degenerate";
    return false;
  }
  out.polygon = hull;
  for (size_t k = 1; k + 1 < hull.size(); ++k) out.triangles.push_back({hull[0], hull[k], hull[k + 1]});
  const double adx = a1.x - a0.x, adz = a1.z - a0.z;
  const double bdx = b1.x - b0.x, bdz = b1.z - b0.z;
  out.turnRadians = std::atan2(adx * bdz - adz * bdx, adx * bdx + adz * bdz);
  return true;
}

} // namespace procedural::world::generator
