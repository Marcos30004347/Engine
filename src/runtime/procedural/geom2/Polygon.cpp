#include "procedural/geom2/Polygon.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace procedural::geom2 {

namespace {

// The adaptive predicates derive their error bounds once. Doing it here, on
// first use, keeps every entry point into this library safe to call without the
// caller knowing the rule.
bool initialise()
{
  initialisePredicates();

  return true;
}

int sign(const double value)
{
  if (value > 0.0)
  {
    return 1;
  }

  if (value < 0.0)
  {
    return -1;
  }

  return 0;
}

// Cross product of (b - a) and (c - a) as an exact integer. Coordinates are
// bounded by 2^43, so the differences fit in 2^44 and the products in 2^88:
// inside __int128, outside a double's mantissa, which is why the sign tests
// below go through the predicates instead of this value.
__int128 crossExact(const Point &a, const Point &b, const Point &c)
{
  const __int128 abx = __int128(b.x) - a.x;
  const __int128 aby = __int128(b.y) - a.y;
  const __int128 acx = __int128(c.x) - a.x;
  const __int128 acy = __int128(c.y) - a.y;

  return abx * acy - aby * acx;
}

// Whether c lies within the axis-aligned box of ab, given the three are already
// known to be collinear. The companion to an orientation test, exact on
// integers.
bool onSegmentCollinear(const Point &a, const Point &b, const Point &c)
{
  return c.x >= std::min(a.x, b.x) && c.x <= std::max(a.x, b.x) && c.y >= std::min(a.y, b.y) && c.y <= std::max(a.y, b.y);
}

size_t nextIndex(const size_t i, const size_t n)
{
  return i + 1 < n ? i + 1 : 0;
}

} // namespace

int orientation(const Point &a, const Point &b, const Point &c)
{
  static const bool ready = initialise();
  (void)ready;

  return sign(orient2d(exact(a), exact(b), exact(c)));
}

__int128 doubleSignedArea(const Ring &ring)
{
  if (ring.size() < 3)
  {
    return 0;
  }

  // Translated to the first vertex so every term is a product of coordinate
  // differences rather than of coordinates, which keeps the running sum far
  // from the __int128 ceiling on a ring that sits away from the origin.
  __int128 total = 0;

  for (size_t i = 1; i + 1 < ring.size(); ++i)
  {
    total += crossExact(ring[0], ring[i], ring[i + 1]);
  }

  return total;
}

double areaMetres(const Ring &ring)
{
  const double doubled = double(doubleSignedArea(ring));

  return doubled * 0.5 * kMetresPerUnit * kMetresPerUnit;
}

double areaMetres(const Polygon &polygon)
{
  double total = areaMetres(polygon.outer);

  for (const auto &hole : polygon.holes)
  {
    // Holes are clockwise, so their signed area is already negative and adding
    // it subtracts. Taking the magnitude here instead would give the wrong
    // answer for a hole that arrived with the wrong orientation, and hiding
    // that is exactly what polygonFault exists to prevent.
    total += areaMetres(hole);
  }

  return total;
}

bool counterClockwise(const Ring &ring)
{
  return doubleSignedArea(ring) > 0;
}

void reverse(Ring &ring)
{
  std::reverse(ring.begin(), ring.end());
}

bool convex(const Ring &ring)
{
  const size_t n = ring.size();

  if (n < 3)
  {
    return false;
  }

  // Two independent conditions. Consistent turn direction alone admits a star
  // polygon, whose turns all agree while it winds twice; bounding the number of
  // direction reversals along each axis to two excludes exactly that, so this
  // answers correctly without first proving the ring simple.
  int turn = 0;
  int reversalsX = 0;
  int reversalsY = 0;
  int previousX = 0;
  int previousY = 0;

  for (size_t i = 0; i < n; ++i)
  {
    const Point &a = ring[i];
    const Point &b = ring[nextIndex(i, n)];
    const Point &c = ring[nextIndex(nextIndex(i, n), n)];

    const int side = orientation(a, b, c);

    if (side != 0)
    {
      if (turn == 0)
      {
        turn = side;
      }
      else if (side != turn)
      {
        return false;
      }
    }

    const int stepX = b.x != a.x ? (b.x > a.x ? 1 : -1) : 0;
    const int stepY = b.y != a.y ? (b.y > a.y ? 1 : -1) : 0;

    if (stepX != 0)
    {
      if (previousX != 0 && stepX != previousX)
      {
        ++reversalsX;
      }

      previousX = stepX;
    }

    if (stepY != 0)
    {
      if (previousY != 0 && stepY != previousY)
      {
        ++reversalsY;
      }

      previousY = stepY;
    }
  }

  return turn != 0 && reversalsX <= 2 && reversalsY <= 2;
}

Box bounds(const Ring &ring)
{
  if (ring.empty())
  {
    return {0, 0, -1, -1};
  }

  Box box{ring[0].x, ring[0].y, ring[0].x, ring[0].y};

  for (const auto &p : ring)
  {
    box.minX = std::min(box.minX, p.x);
    box.minY = std::min(box.minY, p.y);
    box.maxX = std::max(box.maxX, p.x);
    box.maxY = std::max(box.maxY, p.y);
  }

  return box;
}

Box bounds(const Polygon &polygon)
{
  return bounds(polygon.outer);
}

int windingNumber(const Ring &ring, const Point &point)
{
  const size_t n = ring.size();

  if (n < 3)
  {
    return 0;
  }

  // Sunday's crossing count, with the orientation predicate deciding each
  // crossing instead of a computed intersection. The half-open comparison on y
  // is what makes a vertex exactly at the ray's height count once rather than
  // twice or not at all.
  int winding = 0;

  for (size_t i = 0; i < n; ++i)
  {
    const Point &a = ring[i];
    const Point &b = ring[nextIndex(i, n)];

    if (a.y <= point.y)
    {
      if (b.y > point.y && orientation(a, b, point) > 0)
      {
        ++winding;
      }
    }
    else if (b.y <= point.y && orientation(a, b, point) < 0)
    {
      --winding;
    }
  }

  return winding;
}

bool onBoundary(const Ring &ring, const Point &point)
{
  const size_t n = ring.size();

  if (n < 2)
  {
    return false;
  }

  for (size_t i = 0; i < n; ++i)
  {
    const Point &a = ring[i];
    const Point &b = ring[nextIndex(i, n)];

    if (orientation(a, b, point) == 0 && onSegmentCollinear(a, b, point))
    {
      return true;
    }
  }

  return false;
}

bool contains(const Ring &ring, const Point &point)
{
  if (onBoundary(ring, point))
  {
    return true;
  }

  return windingNumber(ring, point) != 0;
}

bool contains(const Polygon &polygon, const Point &point)
{
  if (!contains(polygon.outer, point))
  {
    return false;
  }

  for (const auto &hole : polygon.holes)
  {
    // A point on a hole's edge is on the polygon's boundary, so it stays
    // inside; only the hole's strict interior is removed.
    if (contains(hole, point) && !onBoundary(hole, point))
    {
      return false;
    }
  }

  return true;
}

bool segmentsIntersect(const Point &a, const Point &b, const Point &c, const Point &d)
{
  const int o1 = orientation(a, b, c);
  const int o2 = orientation(a, b, d);
  const int o3 = orientation(c, d, a);
  const int o4 = orientation(c, d, b);

  if (o1 != o2 && o3 != o4)
  {
    return true;
  }

  // Collinear touching and overlap. Enumerated rather than folded into the
  // general case because these are the configurations an arrangement actually
  // trips over, and a test that treats them as "nearly not intersecting" is the
  // bug this library exists to avoid.
  if (o1 == 0 && onSegmentCollinear(a, b, c))
  {
    return true;
  }

  if (o2 == 0 && onSegmentCollinear(a, b, d))
  {
    return true;
  }

  if (o3 == 0 && onSegmentCollinear(c, d, a))
  {
    return true;
  }

  if (o4 == 0 && onSegmentCollinear(c, d, b))
  {
    return true;
  }

  return false;
}

bool segmentsProperlyCross(const Point &a, const Point &b, const Point &c, const Point &d)
{
  const int o1 = orientation(a, b, c);
  const int o2 = orientation(a, b, d);
  const int o3 = orientation(c, d, a);
  const int o4 = orientation(c, d, b);

  // Transversal: each segment has the other's endpoints strictly either side.
  if (o1 != 0 && o2 != 0 && o3 != 0 && o4 != 0 && o1 != o2 && o3 != o4)
  {
    return true;
  }

  if (o1 != 0 || o2 != 0)
  {
    return false;
  }

  // Collinear. An overlap of positive length means one segment has a point of
  // the other strictly in its interior, rather than only at an endpoint.
  const auto strictlyInside = [](const Point &p, const Point &q, const Point &r) {
    if (r == p || r == q)
    {
      return false;
    }

    return r.x >= std::min(p.x, q.x) && r.x <= std::max(p.x, q.x) && r.y >= std::min(p.y, q.y) && r.y <= std::max(p.y, q.y);
  };

  return strictlyInside(a, b, c) || strictlyInside(a, b, d) || strictlyInside(c, d, a) || strictlyInside(c, d, b);
}

std::string ringFault(const Ring &ring)
{
  const size_t n = ring.size();

  if (n < 3)
  {
    return "ring has fewer than three vertices";
  }

  for (const auto &p : ring)
  {
    if (std::abs(p.x) > kMaximumUnit || std::abs(p.y) > kMaximumUnit)
    {
      return "ring vertex outside the representable grid";
    }
  }

  for (size_t i = 0; i < n; ++i)
  {
    if (ring[i] == ring[nextIndex(i, n)])
    {
      return "ring has a repeated consecutive vertex";
    }
  }

  // A vertex visited twice is a pinch: the ring touches itself, so it is not
  // simple even when no two edges cross.
  Ring sorted = ring;
  std::sort(sorted.begin(), sorted.end());

  if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
  {
    return "ring visits a vertex twice";
  }

  if (doubleSignedArea(ring) == 0)
  {
    return "ring encloses no area";
  }

  for (size_t i = 0; i < n; ++i)
  {
    const Point &a = ring[i];
    const Point &b = ring[nextIndex(i, n)];

    for (size_t j = i + 1; j < n; ++j)
    {
      const Point &c = ring[j];
      const Point &d = ring[nextIndex(j, n)];

      const bool adjacent = nextIndex(i, n) == j || nextIndex(j, n) == i;

      if (adjacent)
      {
        // Consecutive edges legitimately share their common vertex. They are a
        // fault only when they are collinear and fold back along each other,
        // which is a zero-width spike rather than a corner.
        if (orientation(a, b, d) == 0 && orientation(a, b, c) == 0)
        {
          const Point &far = nextIndex(i, n) == j ? d : c;

          if (onSegmentCollinear(a, b, far))
          {
            return "ring folds back on itself";
          }
        }

        continue;
      }

      if (segmentsIntersect(a, b, c, d))
      {
        return "ring edges intersect";
      }
    }
  }

  return {};
}

std::string polygonFault(const Polygon &polygon)
{
  // The canonical form, which every operation in this library produces and
  // every operation accepts. Inputs that merely touch - a hole meeting its
  // outer ring at one point - are legal arrangement *input*; they are resolved
  // into this form on the way out, and are a fault only here.
  const std::string outerFault = ringFault(polygon.outer);

  if (!outerFault.empty())
  {
    return "outer " + outerFault;
  }

  if (!counterClockwise(polygon.outer))
  {
    return "outer ring is not counter-clockwise";
  }

  for (size_t h = 0; h < polygon.holes.size(); ++h)
  {
    const Ring &hole = polygon.holes[h];
    const std::string fault = ringFault(hole);

    if (!fault.empty())
    {
      return "hole " + fault;
    }

    if (counterClockwise(hole))
    {
      return "hole ring is not clockwise";
    }

    for (const auto &p : hole)
    {
      if (!contains(polygon.outer, p))
      {
        return "hole escapes the outer ring";
      }
    }

    for (size_t i = 0; i < hole.size(); ++i)
    {
      const Point &a = hole[i];
      const Point &b = hole[nextIndex(i, hole.size())];

      for (size_t j = 0; j < polygon.outer.size(); ++j)
      {
        const Point &c = polygon.outer[j];
        const Point &d = polygon.outer[nextIndex(j, polygon.outer.size())];

        if (segmentsProperlyCross(a, b, c, d))
        {
          return "hole crosses the outer ring";
        }
      }
    }

    for (size_t k = 0; k < h; ++k)
    {
      const Ring &other = polygon.holes[k];

      for (const auto &p : hole)
      {
        if (contains(other, p) && !onBoundary(other, p))
        {
          return "holes overlap";
        }
      }

      for (const auto &p : other)
      {
        if (contains(hole, p) && !onBoundary(hole, p))
        {
          return "holes overlap";
        }
      }

      for (size_t i = 0; i < hole.size(); ++i)
      {
        const Point &a = hole[i];
        const Point &b = hole[nextIndex(i, hole.size())];

        for (size_t j = 0; j < other.size(); ++j)
        {
          const Point &c = other[j];
          const Point &d = other[nextIndex(j, other.size())];

          if (segmentsProperlyCross(a, b, c, d))
          {
            return "holes overlap";
          }
        }
      }
    }
  }

  return {};
}

bool toRing(const std::vector<Point2> &metres, Ring &ring)
{
  ring.clear();
  ring.reserve(metres.size());

  for (const auto &p : metres)
  {
    if (!representable(p.x) || !representable(p.y))
    {
      ring.clear();

      return false;
    }

    const Point q = toPoint(p);

    // Two distinct metre coordinates can quantise onto the same grid point. The
    // zero-length edge between them carries no information, so it is dropped
    // here rather than surfacing as a ring fault the caller cannot act on.
    if (!ring.empty() && ring.back() == q)
    {
      continue;
    }

    ring.push_back(q);
  }

  while (ring.size() > 1 && ring.front() == ring.back())
  {
    ring.pop_back();
  }

  return true;
}

std::vector<Point2> toMetres(const Ring &ring)
{
  std::vector<Point2> out;
  out.reserve(ring.size());

  for (const auto &p : ring)
  {
    out.push_back(toPoint2(p));
  }

  return out;
}

namespace {

// Perpendicular distance from b to the line through a and c, in units. Zero
// when a and c coincide, which leaves the collinearity test to the weld.
double offLine(const Point &a, const Point &b, const Point &c)
{
  const double ex = double(c.x - a.x), ey = double(c.y - a.y);
  const double length = std::sqrt(ex * ex + ey * ey);

  if (length < 1e-9)
  {
    return 0;
  }

  return std::abs(double(b.x - a.x) * ey - double(b.y - a.y) * ex) / length;
}

double lengthOf(const Point &a, const Point &b)
{
  const double ex = double(b.x - a.x), ey = double(b.y - a.y);

  return std::sqrt(ex * ex + ey * ey);
}

} // namespace

bool simplifyRing(Ring &ring, const double toleranceMetres, const double shortestEdgeMetres)
{
  if (ring.size() < 3 || toleranceMetres < 0 || shortestEdgeMetres < 0)
  {
    return false;
  }

  const double tolerance = toleranceMetres * kUnitsPerMetre;
  const double shortest = shortestEdgeMetres * kUnitsPerMetre;

  Ring out = ring;

  // Welding first, on its own: it is the only rule that cannot make the ring
  // worse, and running it once up front means the other two never look at a
  // zero length edge.
  for (size_t i = 0; i < out.size() && out.size() > 3;)
  {
    if (lengthOf(out[i], out[(i + 1) % out.size()]) <= tolerance)
    {
      out.erase(out.begin() + long((i + 1) % out.size()));
    }
    else
    {
      ++i;
    }
  }

  for (bool changed = true; changed && out.size() > 3;)
  {
    changed = false;

    // A wall shorter than a doorway is not a wall. Collapsing to the midpoint
    // rather than to either end keeps the outline centred on where it was.
    for (size_t i = 0; i < out.size() && out.size() > 3; ++i)
    {
      const size_t j = (i + 1) % out.size();

      if (lengthOf(out[i], out[j]) >= shortest)
      {
        continue;
      }

      out[i] = Point{(out[i].x + out[j].x) / 2, (out[i].y + out[j].y) / 2};
      out.erase(out.begin() + long(j));
      changed = true;

      break;
    }

    if (changed)
    {
      continue;
    }

    for (size_t i = 0; i < out.size() && out.size() > 3; ++i)
    {
      const Point &a = out[(i + out.size() - 1) % out.size()];
      const Point &c = out[(i + 1) % out.size()];

      if (offLine(a, out[i], c) >= tolerance)
      {
        continue;
      }

      out.erase(out.begin() + long(i));
      changed = true;

      break;
    }
  }

  // Did it finish, or did it run out of ring?
  //
  // The loops stop at three vertices whatever the thresholds still say, so a
  // shape smaller than the thresholds - a court the width of the tolerance, a
  // sliver a boolean left behind - comes out of them as a triangle that no
  // rule was allowed to touch, rather than as a settled outline. That is the
  // one case where the debris *is* the shape, and turning a small square into
  // a smaller triangle is not an improvement to anything.
  //
  // So the exit condition is that no rule still applies. After a normal run
  // none does, because the loops ran to a fixed point; if one does, the size
  // guard stopped them and the ring is the caller's to keep.
  bool unsettled = false;

  for (size_t i = 0; i < out.size() && !unsettled; ++i)
  {
    const Point &a = out[(i + out.size() - 1) % out.size()];
    const Point &c = out[(i + 1) % out.size()];

    unsettled = lengthOf(out[i], c) < shortest || offLine(a, out[i], c) < tolerance;
  }

  // Simplifying is an improvement, never a requirement: a ring that came out
  // of this self-touching or inside out is thrown away, not returned.
  if (unsettled || out.size() < 3 || !ringFault(out).empty()
      || (areaMetres(out) > 0) != (areaMetres(ring) > 0))
  {
    return false;
  }

  ring = std::move(out);

  return true;
}

void simplifyPolygon(Polygon &polygon, const double toleranceMetres, const double shortestEdgeMetres)
{
  simplifyRing(polygon.outer, toleranceMetres, shortestEdgeMetres);

  for (size_t i = polygon.holes.size(); i-- > 0;)
  {
    if (!simplifyRing(polygon.holes[i], toleranceMetres, shortestEdgeMetres) && polygon.holes[i].size() < 3)
    {
      polygon.holes.erase(polygon.holes.begin() + long(i));
    }
  }
}

bool largestInscribedRectangle(const Polygon &polygon,
                               const double axisX,
                               const double axisY,
                               const double resolutionMetres,
                               double &lowU,
                               double &lowV,
                               double &highU,
                               double &highV)
{
  if (polygon.outer.size() < 3 || resolutionMetres <= 0)
  {
    return false;
  }

  const double length = std::sqrt(axisX * axisX + axisY * axisY);
  const double ux = length > 1e-9 ? axisX / length : 1;
  const double uy = length > 1e-9 ? axisY / length : 0;

  // The polygon in the caller's frame, as a box to raster over.
  double loU = 1e300, hiU = -1e300, loV = 1e300, hiV = -1e300;

  for (const Point &q : polygon.outer)
  {
    const double x = toMetres(q.x), y = toMetres(q.y);
    const double u = x * ux + y * uy, v = -x * uy + y * ux;

    loU = std::min(loU, u);
    hiU = std::max(hiU, u);
    loV = std::min(loV, v);
    hiV = std::max(hiV, v);
  }

  const uint32_t columns = uint32_t(std::min(2048., std::max(1., std::ceil((hiU - loU) / resolutionMetres))));
  const uint32_t rows = uint32_t(std::min(2048., std::max(1., std::ceil((hiV - loV) / resolutionMetres))));
  const double stepU = (hiU - loU) / double(columns), stepV = (hiV - loV) / double(rows);

  // Ground, by the corners rather than the centre. A cell whose centre is in
  // the lot may still have a lot edge through it, and a rectangle built out of
  // such cells hangs over the boundary by up to a cell - which is the whole
  // thing this is meant to avoid.
  const auto in = [&](const double u, const double v) {
    const double x = u * ux - v * uy, y = u * uy + v * ux;

    return contains(polygon, Point{toUnit(x), toUnit(y)});
  };

  std::vector<uint8_t> corner(size_t(columns + 1) * (rows + 1), 0);

  for (uint32_t j = 0; j <= rows; ++j)
  {
    for (uint32_t i = 0; i <= columns; ++i)
    {
      corner[size_t(j) * (columns + 1) + i] = in(loU + stepU * double(i), loV + stepV * double(j)) ? 1 : 0;
    }
  }

  // The largest rectangle under the histogram, by the stack method, run once
  // per row with the heights carried down.
  std::vector<uint32_t> heights(columns, 0);
  uint32_t bestCells = 0, bestI0 = 0, bestI1 = 0, bestJ0 = 0, bestJ1 = 0;

  for (uint32_t j = 0; j < rows; ++j)
  {
    for (uint32_t i = 0; i < columns; ++i)
    {
      const bool ground = corner[size_t(j) * (columns + 1) + i] != 0
                          && corner[size_t(j) * (columns + 1) + i + 1] != 0
                          && corner[size_t(j + 1) * (columns + 1) + i] != 0
                          && corner[size_t(j + 1) * (columns + 1) + i + 1] != 0;

      heights[i] = ground ? heights[i] + 1 : 0;
    }

    std::vector<uint32_t> stack;

    for (uint32_t i = 0; i <= columns; ++i)
    {
      const uint32_t height = i < columns ? heights[i] : 0;

      while (!stack.empty() && heights[stack.back()] >= height)
      {
        const uint32_t top = stack.back();
        stack.pop_back();

        const uint32_t left = stack.empty() ? 0 : stack.back() + 1;
        const uint32_t cells = heights[top] * (i - left);

        if (cells > bestCells)
        {
          bestCells = cells;
          bestI0 = left;
          bestI1 = i;
          bestJ0 = j + 1 - heights[top];
          bestJ1 = j + 1;
        }
      }

      stack.push_back(i);
    }
  }

  if (bestCells == 0)
  {
    return false;
  }

  lowU = loU + stepU * double(bestI0);
  highU = loU + stepU * double(bestI1);
  lowV = loV + stepV * double(bestJ0);
  highV = loV + stepV * double(bestJ1);

  return true;
}

} // namespace procedural::geom2
