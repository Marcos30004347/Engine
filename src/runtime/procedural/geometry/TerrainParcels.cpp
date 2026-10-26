#include "procedural/geometry/TerrainParcels.hpp"
#include "procedural/geometry/TerrainParcelStrategy.hpp"
#include "procedural/geometry/TerrainTownLayout.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>
#include <stdexcept>

namespace procedural::geometry
{
static const bool gDiagnose = std::getenv("PARCEL_DIAGNOSE") != nullptr;
static std::atomic<double> gDepthCap{0}, gSpread{0}, gRefused{0}, gRefusedRatio{0}, gCount{0}, gRatio{0}, gRatioN{0}, gTarget{0};
static void bump(std::atomic<double> &at, const double by)
{ double now = at.load(std::memory_order_relaxed);
  while (!at.compare_exchange_weak(now, now + by, std::memory_order_relaxed)) {} }
namespace
{
constexpr double kPi = 3.14159265358979323846;
uint64_t mix(uint64_t n)
{
  n ^= n >> 30; n *= 0xbf58476d1ce4e5b9ull; n ^= n >> 27;
  n *= 0x94d049bb133111ebull; return n ^ (n >> 31);
}
double unitRandom(uint64_t n) { return double(mix(n) >> 11) / double(1ull << 53); }
double range(uint64_t n, double lo, double hi) { return lo + (hi - lo) * unitRandom(n); }
double smoothstep(double t) { t = std::clamp(t, 0.0, 1.0); return t * t * (3 - 2 * t); }
// Value noise on a lattice, smooth and deterministic. The grain is built from
// this: the whole point is that it varies slowly and continuously, so that
// neighbouring fields agree and distant ones do not.
double valueNoise(const double x, const double y, const uint64_t salt)
{
  const auto corner = [salt](const int64_t a, const int64_t b)
  {
    uint64_t n = uint64_t(a) * 0x9E3779B97F4A7C15ull ^ uint64_t(b) * 0xC2B2AE3D27D4EB4Full ^ salt;
    n ^= n >> 29; n *= 0xBF58476D1CE4E5B9ull; n ^= n >> 32;
    return double(n >> 11) / double(1ull << 53);
  };
  const double fx = std::floor(x), fy = std::floor(y);
  const auto ix = int64_t(fx), iy = int64_t(fy);
  const double tx = smoothstep(x - fx), ty = smoothstep(y - fy);
  const double a = corner(ix, iy), b = corner(ix + 1, iy);
  const double c = corner(ix, iy + 1), d = corner(ix + 1, iy + 1);
  return (a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty;
}
double smoothstep(const double value, const double lo, const double hi)
{ return hi > lo ? smoothstep((value - lo) / (hi - lo)) : (value >= hi ? 1.0 : 0.0); }

double signedArea(const std::vector<Point2> &polygon)
{
  double total = 0;
  for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++)
    total += polygon[j].x * polygon[i].y - polygon[i].x * polygon[j].y;
  return .5 * total;
}
Point2 centroid(const std::vector<Point2> &polygon)
{
  const double area = signedArea(polygon);
  if (std::abs(area) < 1e-9)
  {
    Point2 mean{};
    for (const auto &p : polygon) { mean.x += p.x / polygon.size(); mean.y += p.y / polygon.size(); }
    return mean;
  }
  Point2 c{};
  for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++)
  {
    const double cross = polygon[j].x * polygon[i].y - polygon[i].x * polygon[j].y;
    c.x += (polygon[j].x + polygon[i].x) * cross;
    c.y += (polygon[j].y + polygon[i].y) * cross;
  }
  return {c.x / (6 * area), c.y / (6 * area)};
}
bool contains(const std::vector<Point2> &polygon, const Point2 p)
{
  if (polygon.size() < 3) return false;
  bool inside = false;
  for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++)
  {
    const auto &a = polygon[i], &b = polygon[j];
    if ((a.y > p.y) != (b.y > p.y) && p.x < a.x + (b.x - a.x) * (p.y - a.y) / (b.y - a.y)) inside = !inside;
  }
  return inside;
}
// Longest chord direction, used to choose the cut axis. Cutting across the long
// axis is what keeps parcels from degenerating into slivers as they subdivide.
void principalAxis(const std::vector<Point2> &polygon, const Point2 centre, double &axisX, double &axisY)
{
  double xx = 0, xy = 0, yy = 0;
  for (const auto &p : polygon)
  {
    const double dx = p.x - centre.x, dy = p.y - centre.y;
    xx += dx * dx; xy += dx * dy; yy += dy * dy;
  }
  // Principal eigenvector of the 2x2 covariance.
  const double trace = xx + yy, det = xx * yy - xy * xy;
  const double root = std::sqrt(std::max(0.0, trace * trace * .25 - det));
  const double major = trace * .5 + root;
  if (std::abs(xy) > 1e-12) { axisX = major - yy; axisY = xy; }
  else { axisX = xx >= yy ? 1.0 : 0.0; axisY = xx >= yy ? 0.0 : 1.0; }
  const double length = std::hypot(axisX, axisY);
  if (length > 1e-12) { axisX /= length; axisY /= length; }
  else { axisX = 1; axisY = 0; }
}

struct Crossing { size_t edge; double t; Point2 point; double along; };

double thickness(const std::vector<Point2> &polygon)
{
  double longest = 0;
  for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++)
    longest = std::max(longest, distance(polygon[j], polygon[i]));
  return longest > 1e-9 ? std::abs(signedArea(polygon)) / longest : 0.0;
}
double shortestEdge(const std::vector<Point2> &polygon)
{
  double shortest = std::numeric_limits<double>::max();
  for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++)
    shortest = std::min(shortest, distance(polygon[j], polygon[i]));
  return shortest;
}
// Sharpest interior corner, in degrees. Two edges meeting at a thin angle are a
// spike, and a spike triangulates into the same slivers a needle cell does -
// the corner is where they start.
double sharpestCorner(const std::vector<Point2> &polygon)
{
  double sharpest = 180.0;
  const size_t n = polygon.size();
  for (size_t i = 0; i < n; ++i)
  {
    const Point2 previous = polygon[(i + n - 1u) % n], at = polygon[i], next = polygon[(i + 1u) % n];
    const double ax = previous.x - at.x, ay = previous.y - at.y;
    const double bx = next.x - at.x, by = next.y - at.y;
    const double la = std::hypot(ax, ay), lb = std::hypot(bx, by);
    if (!(la > 1e-9) || !(lb > 1e-9)) return 0.0;
    const double cosine = std::clamp((ax * bx + ay * by) / (la * lb), -1.0, 1.0);
    sharpest = std::min(sharpest, std::acos(cosine) * 180.0 / kPi);
  }
  return sharpest;
}
bool segmentsCross(const Point2 a, const Point2 b, const Point2 c, const Point2 d)
{
  const auto side = [](const Point2 p, const Point2 q, const Point2 r)
  { return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x); };
  const double d1 = side(a, b, c), d2 = side(a, b, d), d3 = side(c, d, a), d4 = side(c, d, b);
  return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}
// Splits along a diagonal between two existing vertices. A straight line can
// fail to cross a non-convex outline exactly twice, and then no cut is possible
// at all - which left a core of blocks that never subdivided, 4.8% of cells
// holding 29% of the farmland. A simple polygon always admits an internal
// diagonal, so this is the fallback that always terminates.
bool splitOnDiagonal(const std::vector<Point2> &polygon, const double snap,
    const double minimumCornerDegrees, std::vector<Point2> &left, std::vector<Point2> &right)
{
  const size_t n = polygon.size();
  if (n < 4u) return false;
  double bestScore = 0;
  size_t bestA = 0, bestB = 0;
  for (size_t i = 0; i < n; ++i)
    for (size_t j = i + 2u; j < n; ++j)
    {
      if (i == 0 && j == n - 1u) continue;          // adjacent around the loop
      const Point2 a = polygon[i], b = polygon[j];
      const Point2 middle((a.x + b.x) * .5, (a.y + b.y) * .5);
      if (!contains(polygon, middle)) continue;      // leaves the polygon
      bool blocked = false;
      for (size_t e = 0, f = n - 1u; e < n && !blocked; f = e++)
      {
        if (e == i || e == j || f == i || f == j) continue;
        blocked = segmentsCross(a, b, polygon[f], polygon[e]);
      }
      if (blocked) continue;
      // Prefer a diagonal that halves the polygon rather than clipping a corner.
      const double balance = std::min(double(j - i), double(n - (j - i)));
      const double score = balance * distance(a, b);
      if (score > bestScore) { bestScore = score; bestA = i; bestB = j; }
    }
  if (bestScore <= 0) return false;
  left.clear(); right.clear();
  for (size_t k = bestA; k != bestB; k = (k + 1u) % n) left.push_back(polygon[k]);
  left.push_back(polygon[bestB]);
  for (size_t k = bestB; k != bestA; k = (k + 1u) % n) right.push_back(polygon[k]);
  right.push_back(polygon[bestA]);
  if (left.size() < 3u || right.size() < 3u) return false;
  if (std::abs(signedArea(left)) < 1e-6 || std::abs(signedArea(right)) < 1e-6) return false;
  if (thickness(left) < snap || thickness(right) < snap) return false;
  if (sharpestCorner(left) < minimumCornerDegrees || sharpestCorner(right) < minimumCornerDegrees)
    return false;
  return true;
}

// Splits `polygon` with the infinite line through `origin` along (dirX, dirY)'s
// normal, bent by `bend` at its midpoint. Returns false when the line does not
// produce two usable pieces, which is the caller's signal to stop subdividing.
// A piece is usable only if it has real width. Area over longest edge is the
// average thickness of a polygon, and a value near zero is a needle - which
// triangulates into slivers whose long edge streaks across several parcels and
// takes the material of whichever one holds its centroid.
// Offset a counter-clockwise polygon inwards by `d`, mitring at the corners.
// Returns false when the shape collapses, which is the signal to leave a small
// ward undivided rather than peel a street it has no room for.
bool insetPolygon(const std::vector<Point2> &polygon, const double d, std::vector<Point2> &inner)
{
  const size_t n = polygon.size();
  if (n < 3u || !(d > 0)) return false;
  inner.clear(); inner.reserve(n);
  for (size_t i = 0; i < n; ++i)
  {
    const Point2 &u = polygon[(i + n - 1u) % n], &v = polygon[i], &w = polygon[(i + 1u) % n];
    const double ax = v.x - u.x, ay = v.y - u.y, bx = w.x - v.x, by = w.y - v.y;
    const double la = std::hypot(ax, ay), lb = std::hypot(bx, by);
    if (!(la > 1e-9) || !(lb > 1e-9)) return false;
    // Interior is to the left of a counter-clockwise edge, so the inward
    // normal of (dx, dy) is (-dy, dx).
    const double n1x = -ay / la, n1y = ax / la, n2x = -by / lb, n2y = bx / lb;
    const double dot = n1x * n2x + n1y * n2y;
    // The mitre blows up as the turn approaches a reversal; clamp it rather
    // than emit a spike, which is what sliver triangles are made of.
    const double scale = 1.0 + std::max(dot, -0.8);
    inner.push_back({v.x + d * (n1x + n2x) / scale, v.y + d * (n1y + n2y) / scale});
  }
  if (signedArea(inner) < std::abs(signedArea(polygon)) * .12) return false;
  // A mitre that crossed its neighbour folds the outline; reject those.
  for (size_t i = 0; i < n; ++i)
  {
    const Point2 &a = inner[i], &b = inner[(i + 1u) % n];
    const Point2 &oa = polygon[i], &ob = polygon[(i + 1u) % n];
    if ((b.x - a.x) * (ob.x - oa.x) + (b.y - a.y) * (ob.y - oa.y) <= 0) return false;
  }
  return true;
}

// The same mitre, with a distance per edge rather than one for the polygon.
// Watabou insets a main street by more than a lane, which is why his streets
// vary in width down one block; a single distance everywhere is what makes a
// town read as a honeycomb. Vertex count is preserved, so the ring between the
// two outlines still tiles exactly as quads.
bool insetPolygonVarying(const std::vector<Point2> &polygon,
    const std::vector<double> &distances, std::vector<Point2> &inner)
{
  const size_t n = polygon.size();
  if (n < 3u || distances.size() < n) return false;
  inner.clear(); inner.reserve(n);
  for (size_t i = 0; i < n; ++i)
  {
    const Point2 &u = polygon[(i + n - 1u) % n], &v = polygon[i], &w = polygon[(i + 1u) % n];
    const double ax = v.x - u.x, ay = v.y - u.y, bx = w.x - v.x, by = w.y - v.y;
    const double la = std::hypot(ax, ay), lb = std::hypot(bx, by);
    if (!(la > 1e-9) || !(lb > 1e-9)) return false;
    const double n1x = -ay / la, n1y = ax / la, n2x = -by / lb, n2y = bx / lb;
    const double da = distances[(i + n - 1u) % n], db = distances[i];
    // The mitre is the point at `da` from the incoming edge and `db` from the
    // outgoing one: two linear equations, solved directly.
    const double determinant = n1x * n2y - n1y * n2x;
    if (std::abs(determinant) < 1e-6)
    { inner.push_back({v.x + n1x * da, v.y + n1y * da}); continue; }
    const double px = (da * n2y - db * n1y) / determinant;
    const double py = (db * n1x - da * n2x) / determinant;
    // A near-reversal sends the mitre to infinity; cap the reach rather than
    // emit a spike.
    const double reach = std::hypot(px, py), cap = 3.0 * std::max(da, db);
    const double scale = reach > cap && reach > 1e-9 ? cap / reach : 1.0;
    inner.push_back({v.x + px * scale, v.y + py * scale});
  }
  if (signedArea(inner) < std::abs(signedArea(polygon)) * .10) return false;
  for (size_t i = 0; i < n; ++i)
  {
    const Point2 &a = inner[i], &b = inner[(i + 1u) % n];
    const Point2 &oa = polygon[i], &ob = polygon[(i + 1u) % n];
    if ((b.x - a.x) * (ob.x - oa.x) + (b.y - a.y) * (ob.y - oa.y) <= 0) return false;
  }
  return true;
}

// A straight cut by half-plane clipping. Always succeeds on a convex piece, and
// a convex piece stays convex, which is the property the whole subdivision
// depends on.
//
// `splitPolygon` below exists to police the *quality* of a cut - it refuses one
// that snaps to a vertex or leaves too sharp a corner - and those gates are
// right for a cut that may be bent or rotated. For a straight cut of a convex
// block they refuse cuts that are perfectly good, and a refused cut emits the
// piece whole: measured, 301.9 km² of the island, at a mean of 3.5 times the
// target area. That is where the large wedges in the farmland came from.
bool clipPolygon(const std::vector<Point2> &polygon, const Point2 at,
    const double normalX, const double normalY, std::vector<Point2> &left,
    std::vector<Point2> &right)
{
  const size_t n = polygon.size();
  if (n < 3u) return false;
  left.clear(); right.clear();
  const auto side = [&](const Point2 q)
  { return (q.x - at.x) * normalX + (q.y - at.y) * normalY; };
  for (size_t i = 0; i < n; ++i)
  {
    const Point2 a = polygon[i], b = polygon[(i + 1u) % n];
    const double da = side(a), db = side(b);
    if (da <= 0) left.push_back(a);
    if (da >= 0) right.push_back(a);
    if ((da < 0 && db > 0) || (da > 0 && db < 0))
    {
      const double t = da / (da - db);
      const Point2 crossing{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
      left.push_back(crossing);
      right.push_back(crossing);
    }
  }
  // Drop the duplicates a cut through a vertex leaves behind.
  const auto tidy = [](std::vector<Point2> &ring)
  {
    ring.erase(std::unique(ring.begin(), ring.end(), [](const Point2 &a, const Point2 &b)
        { return std::abs(a.x - b.x) < 1e-7 && std::abs(a.y - b.y) < 1e-7; }), ring.end());
    if (ring.size() >= 2u && std::abs(ring.front().x - ring.back().x) < 1e-7 &&
        std::abs(ring.front().y - ring.back().y) < 1e-7) ring.pop_back();
  };
  tidy(left); tidy(right);
  // Both sides must be real polygons, or the line missed the interior and this
  // was not a cut at all.
  const double whole = std::abs(signedArea(polygon));
  if (left.size() < 3u || right.size() < 3u) return false;
  if (std::abs(signedArea(left)) < whole * 1e-4 ||
      std::abs(signedArea(right)) < whole * 1e-4) return false;
  return true;
}

bool splitPolygon(const std::vector<Point2> &polygon, const Point2 origin,
    const double normalX, const double normalY, const double bend, const double snap,
    const double minimumCornerDegrees, std::vector<Point2> &left, std::vector<Point2> &right)
{
  // Signed distance of each vertex from the cut line.
  const size_t n = polygon.size();
  std::vector<double> side(n);
  for (size_t i = 0; i < n; ++i)
    side[i] = (polygon[i].x - origin.x) * normalX + (polygon[i].y - origin.y) * normalY;
  std::vector<Crossing> crossings;
  for (size_t i = 0, j = n - 1; i < n; j = i++)
  {
    if ((side[j] > 0) == (side[i] > 0)) continue;
    const double t = side[j] / (side[j] - side[i]);
    if (!std::isfinite(t)) continue;
    crossings.push_back({j, t, Point2(polygon[j].x + (polygon[i].x - polygon[j].x) * t,
        polygon[j].y + (polygon[i].y - polygon[j].y) * t), 0.0});
  }
  // A straight cut through a non-convex polygon crosses the outline more than
  // twice, and refusing those outright was the single largest cause of
  // oversized cells: 59% of the allocated area sat in cells over eight times
  // the target, because one refused cut ends that branch of the recursion for
  // good. Warping the block boundaries made it worse, since a warped block is
  // wiggly by construction.
  //
  // The fix rests on a standard fact: sorted along the cut line, the crossings
  // alternate entering and leaving, so a consecutive pair (0,1), (2,3) and so
  // on spans a chord lying wholly inside the polygon - and any chord of a
  // simple polygon divides it into exactly two simple polygons. The remaining
  // crossings are then just ordinary vertices on the pieces. Taking the longest
  // such chord makes the most even split available.
  if (crossings.size() < 2u || (crossings.size() & 1u)) return false;
  size_t first = 0u;
  if (crossings.size() > 2u)
  {
    const double dirX = -normalY, dirY = normalX;
    std::vector<size_t> order(crossings.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](const size_t l, const size_t r)
    { return crossings[l].point.x * dirX + crossings[l].point.y * dirY <
          crossings[r].point.x * dirX + crossings[r].point.y * dirY; });
    double longest = -1;
    for (size_t k = 0; k + 1 < order.size(); k += 2u)
    {
      const double span = distance(crossings[order[k]].point, crossings[order[k + 1u]].point);
      if (span > longest) { longest = span; first = k; }
    }
    const Crossing lo = crossings[order[first]], hi = crossings[order[first + 1u]];
    crossings.clear(); crossings.push_back(lo); crossings.push_back(hi);
    // Restore boundary order, which is what the walk below indexes by.
    if (crossings[0].edge > crossings[1].edge) std::swap(crossings[0], crossings[1]);
    first = 0u;
  }

  // The cut runs between the two crossings. Bending it means inserting a
  // midpoint pushed sideways; the offset is kept small so the cut cannot leave
  // a convex-ish polygon, and it is what hides the recursion.
  const Point2 a = crossings[0].point, b = crossings[1].point;
  const double cutX = b.x - a.x, cutY = b.y - a.y;
  const double cutLength = std::hypot(cutX, cutY);
  if (!(cutLength > 1e-6)) return false;
  std::vector<Point2> cut{a};
  if (std::abs(bend) > 1e-6)
  {
    const Point2 middle((a.x + b.x) * .5 + normalX * bend * cutLength,
        (a.y + b.y) * .5 + normalY * bend * cutLength);
    if (contains(polygon, middle)) cut.push_back(middle);
  }
  cut.push_back(b);

  // Walk the boundary from one crossing to the other each way, closing each
  // side with the cut. Shared vertices come from the same arithmetic on both
  // sides, so the two pieces meet exactly.
  const auto walk = [&](const size_t fromEdge, const size_t toEdge, std::vector<Point2> &out, bool forward)
  {
    out.clear();
    for (const auto &p : cut) out.push_back(p);
    if (!forward) std::reverse(out.begin(), out.end());
    size_t at = (fromEdge + 1u) % n;
    for (size_t guard = 0; guard <= n; ++guard)
    {
      if (((at + n - 1u) % n) == toEdge) break;
      out.push_back(polygon[at]);
      at = (at + 1u) % n;
    }
  };
  walk(crossings[1].edge, crossings[0].edge, left, true);
  walk(crossings[0].edge, crossings[1].edge, right, false);
  if (left.size() < 3u || right.size() < 3u) return false;
  if (std::abs(signedArea(left)) < 1e-6 || std::abs(signedArea(right)) < 1e-6) return false;
  // The snap rule, which the plan called not optional and the first
  // implementation omitted entirely. A cut that passes within `snap` of an
  // existing corner leaves a needle behind, and a needle cell becomes a run of
  // sliver triangles: measured before this, 30% of faces in a district had an
  // aspect ratio over 8 and the worst was 2.5e8. Refuse the cut instead; the
  // parent stays whole, which is a larger field and not a broken one.
  if (thickness(left) < snap || thickness(right) < snap) return false;
  if (shortestEdge(left) < snap * .5 || shortestEdge(right) < snap * .5) return false;
  if (sharpestCorner(left) < minimumCornerDegrees || sharpestCorner(right) < minimumCornerDegrees)
    return false;
  return true;
}
} // namespace

uint32_t ParcelLayer::cellAt(const Point2 point) const
{
  if (buckets_.empty() || !(extent_ > 0)) return kNoParcel;
  const auto cell = [&](double v)
  { return uint32_t(std::clamp(v / extent_ * side_, 0.0, double(side_ - 1))); };
  for (const uint32_t id : buckets_[size_t(cell(point.y)) * side_ + cell(point.x)])
    if (contains(cells[id].boundary, point)) return id;
  return kNoParcel;
}
void ParcelLayer::buildIndex(const double extentMetres)
{
  extent_ = extentMetres;
  side_ = uint32_t(std::clamp(std::sqrt(double(std::max<size_t>(1u, cells.size()))) * .5, 8.0, 512.0));
  buckets_.assign(size_t(side_) * side_, {});
  if (!(extent_ > 0)) return;
  const auto cell = [&](double v)
  { return int64_t(std::clamp(std::floor(v / extent_ * side_), 0.0, double(side_ - 1))); };
  for (uint32_t id = 0; id < cells.size(); ++id)
  {
    Point2 lo = cells[id].boundary.front(), hi = lo;
    for (const auto &p : cells[id].boundary)
    { lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); }
    for (int64_t y = cell(lo.y); y <= cell(hi.y); ++y)
      for (int64_t x = cell(lo.x); x <= cell(hi.x); ++x)
        buckets_[size_t(y) * side_ + size_t(x)].push_back(id);
  }
}

ParcelLayer allocateParcels(const double extentMetres, const std::vector<std::vector<Point2>> &blocks,
    const std::vector<std::vector<Point2>> &infrastructure, const ParcelFields &fields,
    const SettlementNetwork &settlements, const ParcelSettings &settings,
    const std::vector<uint8_t> &wardBlocks)
{
  ParcelLayer layer;
  if (!settings.enabled) return layer;
  if (!(extentMetres > 0)) throw std::invalid_argument("invalid parcel domain");
  if (!fields.slope || !fields.waterType) throw std::invalid_argument("parcels need slope and water type");
  const auto sample = [](const std::function<double(Point2)> &fn, Point2 p, double fallback)
  { return fn ? fn(p) : fallback; };

  // --- road corridors ------------------------------------------------------
  // The road edges arrive as cutters, so the carriageway already falls out of
  // the subdivision as its own cells. What is needed here is to recognise them,
  // which is a proximity test against the centre-lines - indexed, because there
  // are a thousand roads and the question is asked once per finished cell.
  struct RoadSpan { Point2 a, b; double half; };
  std::vector<RoadSpan> corridors;
  for (const auto &road : settlements.roads)
    for (size_t k = 1; k < road.path.size(); ++k)
      corridors.push_back({road.path[k - 1], road.path[k],
          std::max(1.5, road.widthMetres * settings.roadHalfWidths)});
  const uint32_t roadSide = 512u;
  const double roadCell = extentMetres / roadSide;
  std::vector<std::vector<uint32_t>> roadBuckets(size_t(roadSide) * roadSide);
  const auto roadBucket = [&](const double v)
  { return size_t(std::clamp(v / roadCell, 0.0, double(roadSide - 1))); };
  for (uint32_t i = 0; i < corridors.size(); ++i)
  {
    const auto &span = corridors[i];
    for (size_t y = roadBucket(std::min(span.a.y, span.b.y) - span.half);
         y <= roadBucket(std::max(span.a.y, span.b.y) + span.half); ++y)
      for (size_t x = roadBucket(std::min(span.a.x, span.b.x) - span.half);
           x <= roadBucket(std::max(span.a.x, span.b.x) + span.half); ++x)
        roadBuckets[y * roadSide + x].push_back(i);
  }
  const auto onRoad = [&](const Point2 p)
  {
    for (const uint32_t i : roadBuckets[roadBucket(p.y) * roadSide + roadBucket(p.x)])
    {
      const auto &span = corridors[i];
      const double dx = span.b.x - span.a.x, dy = span.b.y - span.a.y;
      const double length = dx * dx + dy * dy;
      if (length < 1e-9) continue;
      const double t = std::clamp(((p.x - span.a.x) * dx + (p.y - span.a.y) * dy) / length, 0.0, 1.0);
      if (std::hypot(p.x - span.a.x - t * dx, p.y - span.a.y - t * dy) <= span.half) return true;
    }
    return false;
  };
  const auto nearRoad = [&](const Point2 p, const double reach)
  {
    const auto lo = roadBucket(std::max(0.0, p.x - reach)), hi = roadBucket(p.x + reach);
    const auto loY = roadBucket(std::max(0.0, p.y - reach)), hiY = roadBucket(p.y + reach);
    for (size_t y = loY; y <= hiY; ++y)
      for (size_t x = lo; x <= hi; ++x)
        if (!roadBuckets[y * roadSide + x].empty()) return true;
    return false;
  };

  // --- organic block boundaries -------------------------------------------
  // Blocks arrive from the map graph, which is a coarse planar partition: its
  // cells have edges hundreds of metres long and dead straight. Land use is
  // decided per parcel, but the parcels stop at those edges, so the map graph
  // reads straight through the allocation as long straight biome boundaries -
  // exactly what this layer exists to hide.
  //
  // Each long edge is replaced by recursive midpoint displacement. The
  // displacement is hashed from the two endpoints in a canonical order, so the
  // two blocks that share an edge generate the identical warped chain and the
  // partition stays closed. Ordering by magnitude alone is not enough: the two
  // sides walk the edge in opposite directions, which mirrors the offset.
  std::vector<std::vector<Point2>> warped;
  warped.reserve(blocks.size());
  {
    const auto displace = [&](const Point2 lo, const Point2 hi, const uint32_t depth,
        std::vector<Point2> &into, const auto &self) -> void
    {
      const double span = distance(lo, hi);
      if (depth == 0u || span < settings.blockWarpMinimumMetres * 2.0) return;
      const uint64_t key = mix(settings.seed ^ 0x626c6f636b7761ull ^
          uint64_t(std::llround(lo.x * 8)) ^ (uint64_t(std::llround(lo.y * 8)) << 16u) ^
          (uint64_t(std::llround(hi.x * 8)) << 29u) ^ (uint64_t(std::llround(hi.y * 8)) << 45u));
      const double dx = (hi.x - lo.x) / span, dy = (hi.y - lo.y) / span;
      const double offset = (unitRandom(key) * 2.0 - 1.0) * settings.blockWarpRoughness * span;
      const Point2 middle((lo.x + hi.x) * .5 - dy * offset, (lo.y + hi.y) * .5 + dx * offset);
      self(lo, middle, depth - 1u, into, self);
      into.push_back(middle);
      self(middle, hi, depth - 1u, into, self);
    };
    for (const auto &block : blocks)
    {
      if (block.size() < 3u) { warped.push_back(block); continue; }
      std::vector<Point2> shape;
      shape.reserve(block.size() * 4u);
      for (size_t i = 0, n = block.size(); i < n; ++i)
      {
        const Point2 a = block[i], b = block[(i + 1u) % n];
        shape.push_back(a);
        const bool flip = std::tie(a.x, a.y) > std::tie(b.x, b.y);
        std::vector<Point2> interior;
        displace(flip ? b : a, flip ? a : b, settings.blockWarpDepth, interior, displace);
        if (flip) std::reverse(interior.begin(), interior.end());
        shape.insert(shape.end(), interior.begin(), interior.end());
      }
      warped.push_back(std::move(shape));
    }
  }
  const std::vector<std::vector<Point2>> &sourceBlocks = settings.blockWarpRoughness > 0 ? warped : blocks;

  // Infrastructure is not cut, it cuts. Each segment that crosses a block is
  // offered as a split before any generated cut, so parcels come out bounded by
  // the lane and the river rather than laid across them.
  struct Line { Point2 a, b; };
  std::vector<Line> cutters;
  for (const auto &path : infrastructure)
    for (size_t i = 1; i < path.size(); ++i) cutters.push_back({path[i - 1], path[i]});
  // Indexed, because the river network alone is hundreds of thousands of
  // segments and the subdivision asks this question once per recursion node.
  // Unindexed it is blocks x depth x segments, which is where the first run of
  // this went: 10^11 tests and no output.
  const uint32_t cutterSide = uint32_t(std::clamp(std::sqrt(double(std::max<size_t>(1u, cutters.size()))) * .5, 8.0, 512.0));
  std::vector<std::vector<uint32_t>> cutterBuckets(size_t(cutterSide) * cutterSide);
  const auto cutterCell = [&](const double v)
  { return int64_t(std::clamp(std::floor(v / extentMetres * cutterSide), 0.0, double(cutterSide - 1))); };
  for (uint32_t i = 0; i < cutters.size(); ++i)
  {
    const auto &line = cutters[i];
    for (int64_t y = cutterCell(std::min(line.a.y, line.b.y)); y <= cutterCell(std::max(line.a.y, line.b.y)); ++y)
      for (int64_t x = cutterCell(std::min(line.a.x, line.b.x)); x <= cutterCell(std::max(line.a.x, line.b.x)); ++x)
        cutterBuckets[size_t(y) * cutterSide + size_t(x)].push_back(i);
  }

  for (uint32_t blockId = 0; blockId < sourceBlocks.size(); ++blockId)
  {
    const auto &block = sourceBlocks[blockId];
    if (block.size() < 3u) continue;
    const Point2 blockCentre = centroid(block);
    // Reject a block only when the *whole* of it fails, not when its centre
    // does. A map-graph cell is hundreds of metres across and one straddling a
    // shore is half land; testing its centre made that a coin flip, so the
    // coastal fringe came out as a scatter of parcelled patches separated by
    // unparcelled holes for kilometres inland. The per-cell tests below still
    // drop whatever genuinely is water, steep or bare.
    {
      bool anyUsable = false;
      const auto usable = [&](const Point2 at)
      { return fields.waterType(at) == 0u && fields.slope(at) <= settings.maximumSlope &&
            !(fields.exposedRock && fields.exposedRock(at)); };
      anyUsable = usable(blockCentre);
      for (size_t i = 0; i < block.size() && !anyUsable; ++i)
      {
        // Just inside the corner, so a vertex shared with a water cell is not
        // read as water itself.
        const Point2 &corner = block[i];
        anyUsable = usable({corner.x + (blockCentre.x - corner.x) * .12,
            corner.y + (blockCentre.y - corner.y) * .12});
      }
      if (!anyUsable) continue;
    }

    // Which settlement, if any, owns a given point. Ownership is tested against
    // the plan's own outline, per polygon, not against a radius around the
    // block centre: a map-graph block is kilometres across, so a radius test on
    // its centre declared the whole block built and subdivided square
    // kilometres of open country at building scale.
    std::vector<const SettlementPlan *> nearPlans;
    {
      double span = 0;
      for (const Point2 &q : block) span = std::max(span, distance(q, blockCentre));
      for (const auto &plan : settlements.plans)
      {
        const auto site = std::find_if(settlements.sites.begin(), settlements.sites.end(),
            [&](const SettlementSite &s) { return s.id == plan.site; });
        if (site == settlements.sites.end()) continue;
        if (distance(site->position, blockCentre) < span + plan.radiusMetres * 1.6) nearPlans.push_back(&plan);
      }
    }
    const auto settlementAt = [&](const Point2 at)
    {
      for (const SettlementPlan *plan : nearPlans)
        if (plan->outline.size() >= 3u && contains(plan->outline, at)) return plan->site;
      return kNoParcel;
    };
    const uint32_t settlement = settlementAt(blockCentre);
    // Land use decides the grain. A wood is not divided into fields, so a
    // wooded block is allocated coarsely and stays one stand; a settlement is
    // divided far finer than any field.
    const double blockTrees = sample(fields.treeCover, blockCentre, .2);
    // Field size grades with distance from the nearest settlement. Ground that
    // is walked to every day is worked in small closes; ground an hour out is
    // worked in big open fields. It is the clearest structure in an aerial
    // photograph of settled country - the hedge network thickens around every
    // village - and it falls out of the parcel size rather than needing its own
    // machinery, because more boundaries is exactly what a smaller field means.
    //
    // Evaluated per polygon, not once per block. A block is a map-graph cell,
    // kilometres across, so grading it at its centre gave every field in it the
    // same size: a block whose centre was two kilometres out stayed coarse
    // right up to the town wall, which is why the farm patches came out larger
    // than the city blocks they abut. The distance that matters is the
    // distance of the field, not of the district it happens to fall in.
    const auto gradeAt = [&](const Point2 at)
    {
      if (settlements.sites.empty()) return 1.0;
      double toSettlement = std::numeric_limits<double>::max();
      for (const auto &site : settlements.sites)
        toSettlement = std::min(toSettlement, distance(at, site.position));
      return smoothstep(toSettlement, 300.0, settings.settlementGradeMetres);
    };
    // Field size ramps the whole way out, rather than saturating a couple of
    // kilometres from the village. Ground that is walked to every day is worked
    // in small closes; the outfield is open field; beyond that nobody is
    // ploughing at all and the grain is that of rough grazing and woodland.
    const auto targetFor = [&](const Point2 at, const bool built)
    {
      // Density around a feature is a ramp, not a step.
      //
      // A feature needs cells small enough to follow it - a 95 m shore band cut
      // from 200 m cells is not a shore, it is a row of squares that happen to
      // be sand. But refining only *inside* the feature left a wall: fine cells
      // in the band and 340 m cells immediately behind it, which is what makes
      // the hinterland read as a handful of enormous triangular wedges beside a
      // delicate beach. The eye reads the discontinuity, not the detail.
      const double coast = fields.coastDistance ? fields.coastDistance(at) : 1e30;
      const double band = settings.shoreBandMetres;
      double nearness = 1.0 - smoothstep(coast, band, band * settings.featureFalloff);
      // Rivers by distance, not by inside-ness. A cell only lands its centroid
      // in a 20 m channel once it is already small, so testing inside-ness to
      // decide whether to make it small never fired: the channel has to be
      // approached from outside.
      if (fields.riverDistance)
        nearness = std::max(nearness,
            1.0 - smoothstep(fields.riverDistance(at), band * .4, band * settings.featureFalloff));
      else if (fields.riverChannel && fields.riverChannel(at)) nearness = 1.0;
      if (nearRoad(at, band)) nearness = std::max(nearness, 1.0);
      else if (nearRoad(at, band * settings.featureFalloff))
        nearness = std::max(nearness, .55);
      const double fine = std::max(settings.minimumAreaSquareMetres * 4.0,
          settings.targetAreaSquareMetres * settings.featureRefinement);
      if (built) return nearness > 0 ? std::min(settings.settlementAreaSquareMetres, fine)
          : settings.settlementAreaSquareMetres;
      const double reach = gradeAt(at);
      const double side = settings.closeFieldMetres +
          (settings.farFieldMetres - settings.closeFieldMetres) * reach;
      double target = side * side * (settings.targetAreaSquareMetres / (110.0 * 110.0));
      if (blockTrees > .55) target *= settings.woodlandCoarseness;
      // Blend toward the fine target across the falloff, in the square root so
      // the *side* ramps evenly rather than the area.
      if (nearness > 0)
      {
        const double coarse = std::sqrt(target), close = std::sqrt(fine);
        const double blended = coarse + (close - coarse) * nearness;
        target = blended * blended;
      }
      return target;
    };

    // Recursive bent-cut subdivision.
    // A furlong carries its own cut direction once it enters strip mode, so
    // every cut in it is parallel and the pieces come out as long selions
    // rather than as squares. Recursive bisection across the long axis always
    // converges on squares, and squares are the one thing the open-field
    // pattern in the reference is not.
    struct Job { std::vector<Point2> polygon; uint32_t depth; double stripX = 0, stripY = 0;
        uint64_t group = 0u; };
    // A ward gets its street peeled off the rim before anything is built on
    // it: the ring between the ward outline and its inset is the street, and
    // the plots are cut from what is left. Watabou removes that ring and leaves
    // a gap; here it is kept as cells, because the parcel layer has to remain a
    // closed partition for the triangulator. Mitred offsets mean the quads
    // (a, b, b', a') tile the ring exactly, with no gap at the corners.
    std::vector<Point2> interior = block;
    const uint8_t wardKind = blockId < wardBlocks.size() ? wardBlocks[blockId] : 0u;
    const bool isWard = wardKind != 0u;
    const auto district = isWard ? DistrictKind(wardKind - 1u) : DistrictKind::Count;
    // Whether a ward edge carries a main street. Watabou insets by
    // MAIN_STREET/2 where the block borders the wall or an artery and by
    // REGULAR_STREET/2 otherwise, so the streets of a town differ in width by
    // what they are. An edge here is "on a street" when both its endpoints lie
    // close to one of the plan's arteries.
    std::vector<double> wardInset;
    if (isWard)
    {
      wardInset.assign(block.size(), kRegularStreetMetres * .5);
      const SettlementPlan *plan = nullptr;
      const Point2 blockCentre = centroid(block);
      for (const auto &candidate : settlements.plans)
        if (candidate.outline.size() >= 3u && contains(candidate.outline, blockCentre))
        { plan = &candidate; break; }
      if (plan)
      {
        const auto nearArtery = [&](const Point2 q)
        {
          for (const auto &artery : plan->arteries)
            for (const auto &at : artery)
              if (distance(at, q) < kMainStreetMetres) return true;
          for (const Point2 &at : plan->wall)
            if (distance(at, q) < kMainStreetMetres * 1.5) return true;
          return false;
        };
        for (size_t i = 0, n = block.size(); i < n; ++i)
          if (nearArtery(block[i]) && nearArtery(block[(i + 1u) % n]))
            wardInset[i] = kMainStreetMetres * .5;
      }
    }
    if (isWard && !wardInset.empty() &&
        std::sqrt(std::abs(signedArea(block))) > settings.streetMinimumWardMetres)
    {
      std::vector<Point2> inner;
      if (insetPolygonVarying(block, wardInset, inner))
      {
        for (size_t i = 0, n = block.size(); i < n; ++i)
        {
          std::vector<Point2> quad{block[i], block[(i + 1u) % n], inner[(i + 1u) % n], inner[i]};
          if (signedArea(quad) < 0) std::reverse(quad.begin(), quad.end());
          const double span = std::abs(signedArea(quad));
          if (!(span > 1.0)) continue;
          ParcelCell lane;
          lane.boundary = std::move(quad);
          lane.centre = centroid(lane.boundary);
          lane.areaSquareMetres = span;
          lane.kind = ParcelKind::Street;
          lane.blockId = blockId;
          lane.settlementId = settlementAt(lane.centre);
          lane.tone = uint8_t(mix(settings.seed ^ uint64_t(blockId) ^ (uint64_t(i) << 21u)) % kParcelToneCount);
          layer.cells.push_back(std::move(lane));
        }
        interior.swap(inner);
        // The ward is laid by the port of `Ward.createAlleys`, not by the
        // general subdivision. The general one has to cope with any polygon, so
        // it retries, rotates and falls back to a diagonal - and every one of
        // those produced houses that were triangles or 30:1 slivers. The port
        // keeps the block convex and cuts perpendicular to its longest edge, so
        // a failed cut is impossible and a plot is always square to its block.
        const uint64_t wardSeed = mix(settings.seed ^ 0x77617264ull ^
            (uint64_t(blockId) * 0x9E3779B97F4A7C15ull));
        const WardRecipe recipe = wardRecipe(district, wardSeed);
        if (recipe.laid)
        {
          // How far out this ward sits, for the outskirts thinning. A ward
          // beyond the plan's own radius is a suburb and should be mostly
          // gardens; one at the middle is solid.
          double outskirts = 0.0;
          for (const auto &plan : settlements.plans)
          {
            if (plan.radiusMetres <= 0) continue;
            const auto site = std::find_if(settlements.sites.begin(), settlements.sites.end(),
                [&](const SettlementSite &s) { return s.id == plan.site; });
            if (site == settlements.sites.end()) continue;
            const double reach = distance(site->position, centroid(block));
            if (reach > plan.radiusMetres * 2.2) continue;
            outskirts = std::clamp((reach / plan.radiusMetres - .55) / .85, 0.0, 1.0);
            break;
          }
          for (auto &piece : layWard(interior, recipe, wardSeed, outskirts))
          {
            const double span = std::abs(signedArea(piece.shape));
            if (!(span > settings.minimumAreaSquareMetres)) continue;
            if (signedArea(piece.shape) < 0)
              std::reverse(piece.shape.begin(), piece.shape.end());
            ParcelCell plot;
            plot.centre = centroid(piece.shape);
            plot.boundary = std::move(piece.shape);
            plot.areaSquareMetres = span;
            plot.kind = piece.kind == TownPieceKind::Building ? ParcelKind::Building
                : piece.kind == TownPieceKind::Alley ? ParcelKind::Street : ParcelKind::Yard;
            plot.blockId = blockId;
            plot.settlementId = settlementAt(plot.centre);
            plot.tone = uint8_t(mix(settings.seed ^ 0x706c6f74ull ^
                uint64_t(std::llround(plot.centre.x * 8)) ^
                (uint64_t(std::llround(plot.centre.y * 8)) << 19u)) % kParcelToneCount);
            layer.cells.push_back(std::move(plot));
          }
          continue;   // this block is finished; the general subdivision is not used
        }
      }
    }
    std::vector<Job> pending{{interior, 0u}};
    // A furlong's selions are one field, not twenty independent ones. Rolling
    // the land use per cell gave every strip its own colour, so an open field
    // came out as a barcode; the group id makes the whole sheaf take one roll
    // and one tone while keeping the strip boundaries as hedge lines.
    std::vector<std::pair<std::vector<Point2>, uint64_t>> finished;
    while (!pending.empty())
    {
      Job job = std::move(pending.back());
      pending.pop_back();
      const double area = std::abs(signedArea(job.polygon));
      const Point2 centre = centroid(job.polygon);
      const uint64_t seed = mix(settings.seed ^ (uint64_t(blockId) << 32u) ^
          uint64_t(std::llround(centre.x * 4)) ^ (uint64_t(std::llround(centre.y * 4)) << 17u));
      const double spread = range(seed, settings.areaSpreadLow, settings.areaSpreadHigh);
      const bool built = settlementAt(centre) != kNoParcel;
      const double target = targetFor(centre, built);
      const bool striping = job.stripX != 0 || job.stripY != 0;
      // Arable ground is worked in strips. A block enters strip mode when it
      // reaches furlong size and then only ever cuts parallel, so it ends as a
      // sheaf of long narrow selions - the hatched patches in the reference.
      const double stripTarget = target * settings.stripAspect;
      if (job.depth >= settings.maximumDepth ||
          area <= (striping ? stripTarget : target) * spread)
      {
        if (gDiagnose) { if (job.depth >= settings.maximumDepth) bump(gDepthCap, area);
          else { bump(gSpread, area); bump(gRatio, area / std::max(1.0, target)); bump(gRatioN, 1);
                 bump(gTarget, target); } }
        finished.emplace_back(std::move(job.polygon), job.group); continue;
      }

      double axisX, axisY;
      principalAxis(job.polygon, centre, axisX, axisY);
      // Cut across the long axis, so the normal is the axis itself.
      double normalX = axisX, normalY = axisY;
      // The grain: a slowly varying direction that the whole district shares.
      // Blended against the piece's own axis rather than replacing it, so a
      // long thin remnant is still halved the sensible way instead of being cut
      // lengthwise forever.
      if (!built && !striping && settings.grainStrength > 0)
      {
        const double base = valueNoise(centre.x / settings.grainScaleMetres,
            centre.y / settings.grainScaleMetres, 0x677261696eull) * kPi;
        const double waver = (valueNoise(centre.x / settings.grainWaverScaleMetres,
            centre.y / settings.grainWaverScaleMetres, 0x77617665ull) - .5) * settings.grainWaver;
        const double angle = base + waver;
        double grainX = std::cos(angle), grainY = std::sin(angle);
        // Two cut directions per district - along the grain and across it - and
        // the piece takes whichever is closer to its own long axis. That is
        // what makes a block of fields rectangular rather than a fan.
        const double along = std::abs(grainX * axisX + grainY * axisY);
        if (along < .70710678) { const double t = grainX; grainX = -grainY; grainY = t; }
        const double weight = settings.grainStrength;
        normalX = axisX * (1 - weight) + grainX * weight;
        normalY = axisY * (1 - weight) + grainY * weight;
        const double length = std::hypot(normalX, normalY);
        if (length > 1e-9) { normalX /= length; normalY /= length; }
        else { normalX = axisX; normalY = axisY; }
      }
      if (striping) { normalX = job.stripX; normalY = job.stripY; }
      // Striping is gated on being near a settlement. Open-field farming is
      // something a village does to the ground it walks to; left ungated it
      // hatched every flat treeless acre in the world.
      else if (!built && settings.stripAspect < 1.0 && blockTrees <= .55 &&
          area <= target * settings.furlongBlocks * spread &&
          fields.slope(centre) < settings.maximumSlope * .6 &&
          gradeAt(centre) < settings.stripedReach &&
          unitRandom(seed + 707u) < settings.stripedFraction)
      { job.stripX = normalX; job.stripY = normalY;
        job.group = mix(seed ^ 0x6675726c6f6e67ull); }
      Point2 origin(centre.x + axisX * range(seed + 1u, -settings.cutJitter, settings.cutJitter) * std::sqrt(area),
          centre.y + axisY * range(seed + 2u, -settings.cutJitter, settings.cutJitter) * std::sqrt(area));

      // Prefer an infrastructure line that actually crosses this polygon: it
      // becomes the boundary rather than being crossed by one. Only the
      // segments whose cells the polygon touches are considered.
      Point2 lo = job.polygon.front(), hi = lo;
      for (const auto &q : job.polygon)
      { lo.x = std::min(lo.x, q.x); lo.y = std::min(lo.y, q.y); hi.x = std::max(hi.x, q.x); hi.y = std::max(hi.y, q.y); }
      bool cutByInfrastructure = false;
      for (int64_t cy = cutterCell(lo.y); cy <= cutterCell(hi.y) && !cutByInfrastructure; ++cy)
        for (int64_t cx = cutterCell(lo.x); cx <= cutterCell(hi.x) && !cutByInfrastructure; ++cx)
          for (const uint32_t index : cutterBuckets[size_t(cy) * cutterSide + size_t(cx)])
          {
            const auto &line = cutters[index];
            const double dx = line.b.x - line.a.x, dy = line.b.y - line.a.y;
            const double length = std::hypot(dx, dy);
            if (!(length > 1e-6)) continue;
            // Take the part of the segment that lies inside this piece, and cut
            // through the middle of *that*.
            //
            // Testing whether the segment's own midpoint is inside only finds a
            // cutter shorter than the piece it crosses. A road drawn as one
            // long span crosses many pieces and has its midpoint inside exactly
            // one of them, so every other piece it crossed was never offered
            // the cut and came out straddling the road - which is the whole of
            // the "cells lie across the carriageway" artefact, and it gets
            // worse the coarser the road polyline is.
            double low = 2.0, high = -1.0;
            if (contains(job.polygon, line.a)) { low = 0.0; high = std::max(high, 0.0); }
            if (contains(job.polygon, line.b)) { low = std::min(low, 1.0); high = 1.0; }
            for (size_t k = 0, n = job.polygon.size(); k < n; ++k)
            {
              const Point2 &p = job.polygon[k], &q = job.polygon[(k + 1u) % n];
              const double ex = q.x - p.x, ey = q.y - p.y;
              const double denominator = dx * ey - dy * ex;
              if (std::abs(denominator) < 1e-12) continue;
              const double t = ((p.x - line.a.x) * ey - (p.y - line.a.y) * ex) / denominator;
              const double u = ((p.x - line.a.x) * dy - (p.y - line.a.y) * dx) / denominator;
              if (t < 0.0 || t > 1.0 || u < 0.0 || u > 1.0) continue;
              low = std::min(low, t); high = std::max(high, t);
            }
            if (high < low) continue;
            const double middle = (low + high) * .5;
            normalX = -dy / length; normalY = dx / length;
            origin = {line.a.x + dx * middle, line.a.y + dy * middle};
            cutByInfrastructure = true;
            break;
          }

      // Infrastructure is authoritative: a road or a river bank is a boundary
      // whatever shape it leaves, and a narrow verge beside one is a real
      // parcel. Only generated cuts have to justify themselves.
      const double snap = cutByInfrastructure ? 0.0 : settings.snapDistanceMetres;
      const double corner = cutByInfrastructure ? 0.0 : settings.minimumCornerDegrees;
      // Which rule divides this piece. Ground is not subdivided one way, and
      // every time these four regimes shared one cut, one of them inherited a
      // rule written for another - see TerrainParcelStrategy.hpp.
      const ParcelStrategyKind regime = cutByInfrastructure ? ParcelStrategyKind::Infrastructure
          : built ? ParcelStrategyKind::Township
          : striping ? ParcelStrategyKind::OpenField
          : ParcelStrategyKind::Enclosure;
      const ParcelStrategy &strategy = parcelStrategy(regime);
      ParcelCutRequest request;
      request.polygon = &job.polygon;
      request.centre = centre;
      request.areaSquareMetres = area;
      request.targetSquareMetres = target;
      request.axisX = axisX; request.axisY = axisY;
      request.givenX = normalX; request.givenY = normalY;
      request.givenOrigin = origin;
      request.seed = seed;
      request.settings = &settings;
      // Retry before giving up. `splitPolygon` only accepts a line that crosses
      // the outline exactly twice, and a bent cut leaves pieces that are not
      // convex, so one refused attempt used to end the subdivision then and
      // there - 4.6% of cells ended up over sixteen times the target and
      // covered 56% of the farmland. What a retry is allowed to change is the
      // strategy's business: a field may be re-cut at any angle, a house may
      // only slide its cut along the block.
      std::vector<Point2> left, right;
      bool split = false;
      ParcelCutPlan plan;
      for (uint32_t attempt = 0; attempt < 6u && !split; ++attempt)
      {
        request.attempt = attempt;
        plan = strategy.planCut(request);
        // A straight cut is clipped; only a bent one needs the quality gates.
        split = plan.bend == 0.0
            ? clipPolygon(job.polygon, plan.at, plan.axisX, plan.axisY, left, right)
            : splitPolygon(job.polygon, plan.at, plan.axisX, plan.axisY, plan.bend,
                  snap, corner, left, right);
        // A clip can only fail by missing the interior, which means the point
        // the strategy chose is not inside this piece. The centroid always is,
        // for anything convex, so the direction is kept and only the point is
        // replaced - rather than losing the cut and emitting the piece whole.
        if (!split && plan.bend == 0.0)
          split = clipPolygon(job.polygon, centre, plan.axisX, plan.axisY, left, right);
        if (split && cutByInfrastructure && attempt > 0) break;
        // Refuse a cut that leaves a large triangle. A straight cut of a
        // triangle always yields another triangle, so once a piece has gone
        // three-sided it stays that way however deep the recursion goes - and
        // far from a settlement, where the target is a quarter of a kilometre,
        // that is a 250 m triangular wedge sitting in the middle of farmland.
        // Small triangles are fine; they read as odd corners of fields. Large
        // ones read as the subdivision showing through, which is exactly what
        // they are. Keep looking while there are attempts left.
        if (split && plan.refuseLargeTriangles && attempt + 1u < 6u)
        {
          const auto oversized = [&](const std::vector<Point2> &piece)
          { return piece.size() <= 3u && std::abs(signedArea(piece)) > target * settings.triangleTolerance; };
          if (oversized(left) || oversized(right)) split = false;
        }
      }
      // The diagonal fallback makes a triangle by construction, which is fine
      // for an odd corner of a field and never right for a house.
      if (!split && plan.allowDiagonalFallback)
        split = splitOnDiagonal(job.polygon, snap, corner, left, right);
      if (!split)
      {
        if (gDiagnose) { bump(gRefused, area); bump(gRefusedRatio, area / std::max(1.0, target)); bump(gCount, 1); }
        finished.emplace_back(std::move(job.polygon), job.group); continue;
      }
      pending.push_back({std::move(left), job.depth + 1u, job.stripX, job.stripY, job.group});
      pending.push_back({std::move(right), job.depth + 1u, job.stripX, job.stripY, job.group});
    }

    for (auto &[polygon, group] : finished)
    {
      if (signedArea(polygon) < 0) std::reverse(polygon.begin(), polygon.end());
      ParcelCell cell;
      cell.centre = centroid(polygon);
      if (fields.waterType(cell.centre) != 0u) continue;
      cell.areaSquareMetres = std::abs(signedArea(polygon));
      // A collapsed cell - zero area with a long perimeter - is a fold, not a
      // parcel. Sixty-three of them survived into the settlements, where they
      // contribute nothing but degenerate constraints for the triangulator.
      if (!(cell.areaSquareMetres > settings.minimumAreaSquareMetres)) continue;
      cell.boundary = std::move(polygon);
      cell.blockId = blockId;
      cell.settlementId = settlementAt(cell.centre);
      // One roll per field, not per cell. A furlong's strips share a group, so
      // they take one land use and one tone together and read as a single
      // worked field divided by hedges.
      const uint64_t seed = group != 0u ? mix(settings.seed ^ group)
          : mix(settings.seed ^ uint64_t(std::llround(cell.centre.x * 8)) ^
              (uint64_t(std::llround(cell.centre.y * 8)) << 19u) ^ 0x7061726365ull);
      // The *use* is coherent over a holding, the *tone* is not.
      //
      // Land use was rolled from the cell's own position, so every field drew
      // independently of the one beside it and the country came out as
      // confetti: arable, pasture and meadow alternating cell by cell with no
      // structure at any scale. A holding is several adjacent fields worked
      // together and under the same use, and that is what gives farmland its
      // blocks. Tone stays per cell, so one field still reads differently from
      // its neighbour inside the same block - which is the contrast the aerial
      // references show.
      const double holding = std::max(1.0, settings.holdingMetres);
      const uint64_t holdingSeed = mix(settings.seed ^ 0x686f6c64696e67ull ^
          uint64_t(int64_t(std::floor(cell.centre.x / holding)) * 73856093ll) ^
          (uint64_t(int64_t(std::floor(cell.centre.y / holding)) * 19349663ll) << 13u));
      const double trees = sample(fields.treeCover, cell.centre, .2);
      const double soil = sample(fields.soilQuality, cell.centre, .5);
      const double wet = sample(fields.moisture, cell.centre, .5);
      const double slope = fields.slope(cell.centre);
      // Land use follows the ground, but the mix has to have contrast in it:
      // what makes the aerial reference legible is tilled ochre against grazed
      // green, not a field of one hue. Gentle ground is ploughed, the rest is
      // grazed, and the roll only chooses between neighbours of equal merit.
      // A little per-cell jitter on top, so a holding boundary is not a hard
      // line every time and the occasional field breaks ranks.
      const double roll = std::clamp(unitRandom(holdingSeed) +
          (unitRandom(seed + 5u) - .5) * settings.holdingJitter, 0.0, 1.0);
      // Arable suitability is continuous. Branching on a landform enum put a
      // hard edge wherever the map graph changed cell type, so whole districts
      // came out one use and the underlying map read straight through the
      // fields - which is the one thing the parcel layer exists to hide.
      const double farmable = sample(fields.cultivable, cell.centre, 1.0);
      const double arable = std::clamp(
          (1.0 - smoothstep(slope, .07, .24)) * (.35 + .65 * std::clamp(soil, 0.0, 1.0)), 0.0, 1.0) * farmable;
      // Grazing dominates and tillage is the minority, which is the balance the
      // aerial reference shows: a green ground with ochre fields set in it.
      // Neighbouring fields differ because farmers rotate, so the per-cell roll
      // has to be able to outvote the terrain almost everywhere.
      // Infield and outfield. Tillage is what a settlement can walk to and
      // carry muck to, so it falls away with distance; grazing reaches
      // further; past that is waste, which is rough and wood. Without this the
      // whole world was farmland right out to the wilderness.
      const double reach = gradeAt(cell.centre);
      const double husbandry = 1.0 - smoothstep(reach, settings.tillageFalloff, 1.0);
      // Grazing follows the biome too: a salt flat carries neither crop nor
      // pasture, and what it carries instead is itself.
      const double tilled = (.10 + .46 * arable) * husbandry * farmable;
      // An orchard is a village's own ground, so it sits between the tillage
      // and the grazing rather than being whatever is left over. As the
      // leftover it took 30% of the land, most of it in the deep country.
      const double planted = tilled + .09 * husbandry;
      const double grazed = planted + (.30 + .14 * (1.0 - arable)) * (.25 + .75 * husbandry) * farmable;
      const double mown = grazed + .16 * husbandry;
      // Inside a ward the plot's use comes from what the ward is for, and a
      // third of the plots in an ordinary ward are left unbuilt so a block
      // reads as houses with crofts between them rather than as solid ground.
      // The plates come first, because a road, a shore and a river bed are what
      // the ground *is* there - whatever the biome or the land use would
      // otherwise have made of it.
      if (onRoad(cell.centre)) cell.kind = ParcelKind::Road;
      else if (fields.riverChannel && fields.riverChannel(cell.centre))
        cell.kind = ParcelKind::Riverbank;
      else if (fields.inShoreBand ? fields.inShoreBand(cell.centre)
          : (fields.coastDistance && fields.coastDistance(cell.centre) < settings.shoreBandMetres))
        cell.kind = ParcelKind::Shore;
      else if (isWard && district == DistrictKind::Market) cell.kind = ParcelKind::Market;
      else if (isWard && district == DistrictKind::Castle) cell.kind = ParcelKind::Citadel;
      else if (cell.settlementId != kNoParcel)
        cell.kind = unitRandom(seed + 53u) < settings.yardFraction
            ? ParcelKind::Yard : ParcelKind::Settlement;
      // A built plot outside a laid ward - a steading by a village, a block the
      // map graph put half inside a town - is laid the same way a ward is,
      // rather than by insetting the plot to make a collar.
      //
      // Insetting produces exactly one building per plot, of whatever shape the
      // plot had: a 36,349 m² plot became a 36,349 m² "house", and a plot that
      // was a sliver became a sliver. Running the port over it instead divides
      // it into plots of the right size first, and can only ever emit convex
      // pieces cut square to their own longest edge.
      if (cell.kind == ParcelKind::Settlement && cell.areaSquareMetres > 1.0)
      {
        const uint64_t plotSeed = mix(seed ^ 0x706c6174ull);
        WardRecipe recipe = wardRecipe(DistrictKind::Common, plotSeed);
        recipe.emptyProbability = std::max(recipe.emptyProbability,
            double(settings.buildingEmptyFraction));
        auto pieces = layWard(cell.boundary, recipe, plotSeed);
        if (!pieces.empty())
        {
          // The first piece becomes this cell so the partition keeps its
          // identity; the rest are appended.
          bool taken = false;
          for (auto &piece : pieces)
          {
            const double span = std::abs(signedArea(piece.shape));
            if (!(span > settings.minimumAreaSquareMetres)) continue;
            if (signedArea(piece.shape) < 0)
              std::reverse(piece.shape.begin(), piece.shape.end());
            const ParcelKind kind = piece.kind == TownPieceKind::Building ? ParcelKind::Building
                : piece.kind == TownPieceKind::Alley ? ParcelKind::Street : ParcelKind::Yard;
            if (!taken)
            {
              cell.boundary = std::move(piece.shape);
              cell.centre = centroid(cell.boundary);
              cell.areaSquareMetres = span;
              cell.kind = kind;
              taken = true;
              continue;
            }
            ParcelCell plot;
            plot.centre = centroid(piece.shape);
            plot.boundary = std::move(piece.shape);
            plot.areaSquareMetres = span;
            plot.kind = kind;
            plot.blockId = blockId;
            plot.settlementId = cell.settlementId;
            plot.tone = uint8_t(mix(seed + 71u + uint64_t(layer.cells.size())) % kParcelToneCount);
            layer.cells.push_back(std::move(plot));
          }
          if (!taken) cell.kind = ParcelKind::Yard;
        }
      }
      // The waste goes back to wood. Tree cover is read as higher the further
      // the ground is from anyone who would clear it, which is why the deep
      // country is forest and the village is ringed by fields.
      // Woodland is cleared around a settlement and reclaims the waste, so the
      // cover is read down close in and up far out. Without the clearance term
      // the villages sat in forest and the wilderness did not, which is
      // backwards.
      else if (trees + settings.wildwoodGain * reach -
          settings.clearanceNear * (1.0 - reach) > .55) cell.kind = ParcelKind::Woodland;
      else if (slope > settings.maximumSlope * .75) cell.kind = ParcelKind::Rough;
      else if (roll < tilled) cell.kind = ParcelKind::Field;
      else if (roll < planted) cell.kind = ParcelKind::Orchard;
      else if (roll < grazed) cell.kind = ParcelKind::Pasture;
      else if (roll < mown) cell.kind = ParcelKind::Meadow;
      // The waste is not uniformly bare. What is not grazed reverts according
      // to the ground: wet or already-treed ground goes back to wood, dry
      // ground stays moor. Sending all of it to Rough left the deep country 87%
      // bare rock, and `wet` was being sampled and thrown away.
      else cell.kind = trees + settings.wildwoodGain * reach > .34 || wet > .52
          ? ParcelKind::Woodland : ParcelKind::Rough;
      // Tone is what makes one field read differently from the next while both
      // stay the same material.
      // Enough variants that two neighbouring parcels rarely land on the same
      // one. At six, a district of sixty fields carried seventeen materials,
      // so parcels merged into blobs four times their size and the allocation
      // stopped being visible at all.
      // A stand of trees is one colour across many cells. Making the cells
      // themselves enormous was the wrong way to say "this is all one wood" -
      // it left the mesh with nothing to work with and produced the huge flat
      // regions. Quantising the tone seed to the stand size says it instead,
      // and costs nothing.
      if (cell.kind == ParcelKind::Woodland)
      {
        const double stand = std::max(1.0, settings.standToneMetres);
        cell.tone = uint8_t(mix(settings.seed ^ uint64_t(int64_t(std::floor(cell.centre.x / stand)) * 73856093ll) ^
            uint64_t(int64_t(std::floor(cell.centre.y / stand)) * 19349663ll)) % kParcelToneCount);
      }
      else cell.tone = uint8_t(mix(seed + 11u) % kParcelToneCount);
      layer.cells.push_back(std::move(cell));
    }
  }

  if (gDiagnose)
    std::fprintf(stderr, "terminated by: depth-cap %.4g km2  area-target %.4g km2  split-refused %.4g km2 (mean %.1fx target)\n",
        gDepthCap.load() / 1e6, gSpread.load() / 1e6, gRefused.load() / 1e6,
        gCount.load() > 0 ? gRefusedRatio.load() / gCount.load() : 0.0),
    std::fprintf(stderr, "area-target cells: %.0f, mean area/target %.2f, mean target %.0f m2 (%.0f m)\n",
        gRatioN.load(), gRatio.load() / std::max(1.0, gRatioN.load()),
        gTarget.load() / std::max(1.0, gRatioN.load()),
        std::sqrt(gTarget.load() / std::max(1.0, gRatioN.load())));

  // --- close the T-junctions ----------------------------------------------
  // A cut made on one side of an existing boundary inserts a vertex that the
  // other side never hears about, so cell A carries (p,r) where B carries
  // (p,q),(q,r) and the two never weld: the run before this split reported
  // 2.3 M edges of which half looked like outer boundaries. Insert every
  // boundary vertex that lies on the interior of another cell's segment, which
  // is the same repair `buildTile` performs on its own constraint set - and it
  // matters twice over, because a T-junction is also what the triangulator
  // cannot resolve.
  {
    const double weld = 1e-3;
    const uint32_t side = uint32_t(std::clamp(std::sqrt(double(std::max<size_t>(1u, layer.cells.size()))), 8.0, 1024.0));
    std::vector<std::vector<Point2>> vertexBuckets(size_t(side) * side);
    const auto bucketOf = [&](const Point2 p)
    {
      const auto x = uint32_t(std::clamp(p.x / extentMetres * side, 0.0, double(side - 1)));
      const auto y = uint32_t(std::clamp(p.y / extentMetres * side, 0.0, double(side - 1)));
      return size_t(y) * side + x;
    };
    for (const auto &cell : layer.cells)
      for (const auto &p : cell.boundary) vertexBuckets[bucketOf(p)].push_back(p);
    for (auto &cell : layer.cells)
    {
      std::vector<Point2> refined;
      refined.reserve(cell.boundary.size() * 2u);
      const size_t n = cell.boundary.size();
      for (size_t i = 0; i < n; ++i)
      {
        const Point2 a = cell.boundary[i], b = cell.boundary[(i + 1u) % n];
        refined.push_back(a);
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double lengthSquared = dx * dx + dy * dy;
        if (!(lengthSquared > weld * weld)) continue;
        std::vector<std::pair<double, Point2>> inserts;
        const auto lo = Point2(std::min(a.x, b.x), std::min(a.y, b.y));
        const auto hi = Point2(std::max(a.x, b.x), std::max(a.y, b.y));
        const auto cellOf = [&](double v)
        { return int64_t(std::clamp(std::floor(v / extentMetres * side), 0.0, double(side - 1))); };
        for (int64_t by = cellOf(lo.y); by <= cellOf(hi.y); ++by)
          for (int64_t bx = cellOf(lo.x); bx <= cellOf(hi.x); ++bx)
            for (const Point2 &q : vertexBuckets[size_t(by) * side + size_t(bx)])
            {
              const double t = ((q.x - a.x) * dx + (q.y - a.y) * dy) / lengthSquared;
              if (t <= 1e-9 || t >= 1.0 - 1e-9) continue;
              if (std::hypot(q.x - (a.x + t * dx), q.y - (a.y + t * dy)) > weld) continue;
              inserts.emplace_back(t, q);
            }
        std::sort(inserts.begin(), inserts.end(), [](const auto &l, const auto &r) { return l.first < r.first; });
        for (const auto &[t, q] : inserts)
          if (refined.empty() || distance(refined.back(), q) > weld) refined.push_back(q);
      }
      if (refined.size() >= 3u) cell.boundary.swap(refined);
    }
  }

  // --- break up the long boundaries ---------------------------------------
  // The displacement is a function of the *segment*, under a canonical ordering
  // of its endpoints, so both cells sharing a boundary compute identical points
  // without having to agree about anything.
  //
  // It has to run *after* the T-junction pass, not before. Before it, two cells
  // can hold different segmentations of the same boundary - one sees (a,b), the
  // other (a,m) and (m,b) - and each wanders its own version, so nothing welds:
  // that ordering took the edge count from 1.1 M to 2.6 M with 1.8 M of them
  // looking like outer boundaries.
  if (settings.maximumEdgeMetres > 0)
  {
    // Returns the inserted point itself, built entirely from the canonical
    // ordering of the segment's endpoints. Returning only a magnitude is not
    // enough: the two cells walk a shared boundary in opposite directions, so
    // the perpendicular flips with them and the wander comes out mirrored - the
    // points then differ, nothing welds, and 1.4 M interior edges present as
    // outer boundaries.
    const auto wanderPoint = [&](const Point2 a, const Point2 b, const uint32_t step,
        const uint32_t steps)
    {
      const bool flip = std::tie(a.x, a.y) > std::tie(b.x, b.y);
      const Point2 lo = flip ? b : a, hi = flip ? a : b;
      const uint32_t at = flip ? steps - step : step;
      const uint64_t key = mix(settings.seed ^ 0x656467657769ull ^
          uint64_t(std::llround(lo.x * 16)) ^ (uint64_t(std::llround(lo.y * 16)) << 17u) ^
          (uint64_t(std::llround(hi.x * 16)) << 31u) ^ (uint64_t(std::llround(hi.y * 16)) << 47u));
      const double span = distance(lo, hi);
      if (!(span > 1e-9)) return lo;
      const double dx = (hi.x - lo.x) / span, dy = (hi.y - lo.y) / span;
      const double t = double(at) / steps;
      // Zero at both ends, so the corners stay exactly where the cells agree.
      //
      // The displacement follows a smooth curve along the edge rather than an
      // independent draw per inserted point. Drawing independently makes
      // consecutive points alternate either side of the chord, so a long
      // boundary came out as a regular sawtooth with a tooth per subdivision
      // step - which is what every big region edge looked like. Sampling a
      // continuous function of the position along the edge gives a wander,
      // which is what a hedge line actually does.
      double wave = 0, amplitude = 1, frequency = 1.0;
      for (int harmonic = 0; harmonic < 3; ++harmonic)
      {
        const double phase = double(mix(key + 31u + uint64_t(harmonic)) % 4096u) *
            (6.283185307179586 / 4096.0);
        wave += std::sin(6.283185307179586 * frequency * t + phase) * amplitude;
        amplitude *= .5; frequency *= 2.17;
      }
      const double offset = std::sin(t * kPi) * wave * .62 *
          settings.edgeWander * span / std::sqrt(double(steps));
      return Point2(lo.x + dx * span * t - dy * offset, lo.y + dy * span * t + dx * offset);
    };
    // A hedge line wanders; a road does not, and neither does a river bank or a
    // shore. This pass ran over every boundary a cell had, so the one edge in
    // the whole layer that was authoritative - the infrastructure cut - was
    // displaced off the thing it was cutting, and the cell came out lying
    // across the carriageway that had just bounded it.
    const auto onInfrastructure = [&](const Point2 a, const Point2 b)
    {
      if (cutters.empty()) return false;
      const Point2 middle((a.x + b.x) * .5, (a.y + b.y) * .5);
      const int64_t cx = cutterCell(middle.x), cy = cutterCell(middle.y);
      for (const uint32_t index : cutterBuckets[size_t(cy) * cutterSide + size_t(cx)])
      {
        const auto &line = cutters[index];
        const double dx = line.b.x - line.a.x, dy = line.b.y - line.a.y;
        const double length2 = dx * dx + dy * dy;
        if (!(length2 > 1e-12)) continue;
        const double t = std::clamp(((middle.x - line.a.x) * dx + (middle.y - line.a.y) * dy) / length2, 0.0, 1.0);
        if (std::hypot(middle.x - (line.a.x + dx * t), middle.y - (line.a.y + dy * t)) < .75) return true;
      }
      return false;
    };
    for (auto &cell : layer.cells)
    {
      std::vector<Point2> refined;
      const size_t n = cell.boundary.size();
      refined.reserve(n * 2u);
      for (size_t i = 0; i < n; ++i)
      {
        const Point2 a = cell.boundary[i], b = cell.boundary[(i + 1u) % n];
        refined.push_back(a);
        const double span = distance(a, b);
        if (span <= settings.maximumEdgeMetres) continue;
        const auto steps = uint32_t(std::ceil(span / settings.maximumEdgeMetres));
        if (onInfrastructure(a, b))
        {
          // Still divided, so the mesh has vertices to work with - but straight.
          for (uint32_t step = 1; step < steps; ++step)
          {
            const double t = double(step) / steps;
            refined.push_back({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t});
          }
          continue;
        }
        for (uint32_t step = 1; step < steps; ++step)
          refined.push_back(wanderPoint(a, b, step, steps));
      }
      if (refined.size() >= 3u) cell.boundary.swap(refined);
    }
  }

  // --- edges ---------------------------------------------------------------
  // Cells were produced independently, so adjacency is recovered by welding
  // coincident boundary segments. With the T-junctions closed above, two cells
  // that share a boundary now share the same segmentation of it.
  struct Key { int64_t ax, ay, bx, by; bool operator<(const Key &o) const
  { return std::tie(ax, ay, bx, by) < std::tie(o.ax, o.ay, o.bx, o.by); } };
  const double quantum = 1e-3;
  const auto key = [&](const Point2 a, const Point2 b)
  {
    Key k{int64_t(std::llround(a.x / quantum)), int64_t(std::llround(a.y / quantum)),
        int64_t(std::llround(b.x / quantum)), int64_t(std::llround(b.y / quantum))};
    if (std::tie(k.ax, k.ay) > std::tie(k.bx, k.by))
    { std::swap(k.ax, k.bx); std::swap(k.ay, k.by); }
    return k;
  };
  std::map<Key, std::pair<uint32_t, uint32_t>> shared;
  std::map<Key, std::pair<Point2, Point2>> geometry;
  for (uint32_t id = 0; id < layer.cells.size(); ++id)
  {
    const auto &boundary = layer.cells[id].boundary;
    for (size_t i = 0, j = boundary.size() - 1; i < boundary.size(); j = i++)
    {
      const Key k = key(boundary[j], boundary[i]);
      auto [it, added] = shared.emplace(k, std::make_pair(id, kNoParcel));
      if (added) geometry.emplace(k, std::make_pair(boundary[j], boundary[i]));
      else if (it->second.second == kNoParcel && it->second.first != id) it->second.second = id;
    }
  }
  const double dryLimit = .35;
  for (const auto &[k, pair] : shared)
  {
    ParcelEdge edge;
    edge.cellA = pair.first; edge.cellB = pair.second;
    const auto &ends = geometry.at(k);
    edge.path = {ends.first, ends.second};
    // Owned by the lower index, so whatever stands here is instantiated once.
    edge.owner = std::min(pair.first, pair.second == kNoParcel ? pair.first : pair.second);
    const ParcelKind a = layer.cells[pair.first].kind;
    const ParcelKind b = pair.second == kNoParcel ? ParcelKind::Rough : layer.cells[pair.second].kind;
    const Point2 middle((ends.first.x + ends.second.x) * .5, (ends.first.y + ends.second.y) * .5);
    const double wet = sample(fields.moisture, middle, .5);
    const double slope = fields.slope(middle);
    // The made features come first. A road's verge, a shoreline and a river
    // bank are boundaries the mesh must honour whatever lies either side of
    // them, and the rule below would have called a road edge a hedgerow - which
    // is both wrong for instancing and, where both sides matched, dropped it
    // from the constraints entirely, so the carriageway would have had no edge
    // in the mesh at all.
    const auto made = [](const ParcelKind k)
    { return k == ParcelKind::Road || k == ParcelKind::Shore || k == ParcelKind::Riverbank; };
    if (made(a) != made(b)) edge.type = ParcelEdgeType::Lane;
    else if (made(a) && made(b) && a != b) edge.type = ParcelEdgeType::Lane;
    // A treeline is the *edge* of a wood, not every seam inside one.
    else if ((a == ParcelKind::Woodland) != (b == ParcelKind::Woodland)) edge.type = ParcelEdgeType::Treeline;
    else if (a == ParcelKind::Woodland && b == ParcelKind::Woodland) edge.type = ParcelEdgeType::None;
    else if (a == ParcelKind::Settlement || b == ParcelKind::Settlement) edge.type = ParcelEdgeType::Lane;
    else if (pair.second == kNoParcel) edge.type = ParcelEdgeType::Lane;
    else if (a == b && layer.cells[pair.first].blockId == layer.cells[pair.second].blockId &&
        layer.cells[pair.first].tone == layer.cells[pair.second].tone)
      edge.type = ParcelEdgeType::None;   // same use, same owner: a material change only
    else if (slope > .16) edge.type = ParcelEdgeType::Wall;
    else if (wet < dryLimit) edge.type = ParcelEdgeType::Fence;
    else edge.type = ParcelEdgeType::Hedgerow;
    layer.edges.push_back(std::move(edge));
  }
  layer.buildIndex(extentMetres);
  return layer;
}
}
