#include "procedural/geom2/Offset.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::geom2 {

namespace {

bool fail(std::string *error, const char *text)
{
  if (error != nullptr)
  {
    *error = text;
  }

  return false;
}

struct Vector
{
  double x = 0;
  double y = 0;
};

Vector metresOf(const Point &p)
{
  return {toMetres(p.x), toMetres(p.y)};
}

// Offsetting constructs new coordinates - normals need a square root - so it
// cannot be exact the way the arrangement is. That is the documented division
// of labour: exactness fixes decisions, not constructions. The construction is
// plain double arithmetic with no fast-math, which is reproducible across
// targets, and every decision made about the constructed points afterwards
// goes through the exact predicates on the grid.
bool normalise(const Vector &v, Vector &out)
{
  const double length = std::sqrt(v.x * v.x + v.y * v.y);

  if (!(length > 0.0))
  {
    return false;
  }

  out = {v.x / length, v.y / length};

  return true;
}

// Convex hull of a small point set, monotone chain on the grid with the exact
// orientation predicate. Used to close each vertex join: taking the hull of
// the join's candidate points guarantees the gap is covered whatever the
// join style computed, so a bad miter can only be too blunt, never a hole.
Ring hullOf(std::vector<Point> points)
{
  std::sort(points.begin(), points.end());
  points.erase(std::unique(points.begin(), points.end()), points.end());

  if (points.size() < 3)
  {
    return {};
  }

  Ring hull;

  for (int pass = 0; pass < 2; ++pass)
  {
    const size_t start = hull.size();

    for (const auto &p : pass == 0 ? points : Ring(points.rbegin(), points.rend()))
    {
      while (hull.size() >= start + 2 && orientation(hull[hull.size() - 2], hull[hull.size() - 1], p) <= 0)
      {
        hull.pop_back();
      }

      hull.push_back(p);
    }

    hull.pop_back();
  }

  return hull.size() >= 3 ? hull : Ring{};
}

bool quantise(const Vector &v, Point &out)
{
  if (!representable(v.x) || !representable(v.y))
  {
    return false;
  }

  out = {toUnit(v.x), toUnit(v.y)};

  return true;
}

// The band of points within `distance` of a boundary, as a set of rings to be
// unioned. Symmetric - it covers both sides - which is what lets growing and
// shrinking share one construction: growing is the polygon united with the
// band, shrinking is the polygon less the band.
//
// That symmetry is not a trick. Dilating a set by a disc adds exactly the part
// of the band outside it; eroding removes exactly the part inside. Both are the
// same band.
bool buildBand(const std::vector<Ring> &rings, const double distance, const OffsetOptions &options, std::vector<Ring> &out)
{
  const double d = std::abs(distance);

  if (!(d > 0.0))
  {
    return true;
  }

  // Segments per full turn for a round join, from the chord tolerance. Clamped
  // so a tight tolerance on a large offset cannot explode the segment count.
  const double tolerance = std::max(options.arcToleranceMetres, 1.0e-4);
  const double ratio = std::max(1.0 - tolerance / d, -1.0);
  const int steps = std::min(std::max(int(std::ceil(3.14159265358979323846 / std::acos(std::min(ratio, 1.0 - 1e-12)))), 4), 256);

  for (const auto &ring : rings)
  {
    const size_t n = ring.size();

    if (n < 2)
    {
      continue;
    }

    for (size_t i = 0; i < n; ++i)
    {
      const Vector a = metresOf(ring[i]);
      const Vector b = metresOf(ring[(i + 1) % n]);

      Vector direction;

      if (!normalise({b.x - a.x, b.y - a.y}, direction))
      {
        continue;
      }

      const Vector normal{direction.y, -direction.x};

      // The edge sleeve: the rectangle of half-width d centred on the edge.
      Point p0, p1, p2, p3;

      if (!quantise({a.x + normal.x * d, a.y + normal.y * d}, p0) || !quantise({b.x + normal.x * d, b.y + normal.y * d}, p1) || !quantise({b.x - normal.x * d, b.y - normal.y * d}, p2) ||
          !quantise({a.x - normal.x * d, a.y - normal.y * d}, p3))
      {
        return false;
      }

      Ring sleeve{p0, p1, p2, p3};

      if (!counterClockwise(sleeve))
      {
        reverse(sleeve);
      }

      if (doubleSignedArea(sleeve) != 0)
      {
        out.push_back(std::move(sleeve));
      }
    }

    // The vertex join, covering the gap the two sleeves leave at a corner.
    for (size_t i = 0; i < n; ++i)
    {
      const Vector previous = metresOf(ring[(i + n - 1) % n]);
      const Vector current = metresOf(ring[i]);
      const Vector next = metresOf(ring[(i + 1) % n]);

      Vector incoming, outgoing;

      if (!normalise({current.x - previous.x, current.y - previous.y}, incoming) || !normalise({next.x - current.x, next.y - current.y}, outgoing))
      {
        continue;
      }

      std::vector<Point> candidates;
      Point p;

      // A reversal - the end of an open path, where the walk turns back on
      // itself - has no bisector and no gap for a miter to span. Only a disc
      // covers it, so the join style falls back to round there. That is what
      // gives a ribbon its end caps.
      const bool reversal = incoming.x * outgoing.x + incoming.y * outgoing.y < -1.0 + 1.0e-9;

      if (options.join == JoinStyle::Round || reversal)
      {
        for (int k = 0; k < steps; ++k)
        {
          const double angle = 2.0 * 3.14159265358979323846 * double(k) / double(steps);

          // Circumscribed, so the polygon covers the disc rather than sitting
          // inside it and leaving slivers of the gap uncovered.
          const double radius = d / std::cos(3.14159265358979323846 / double(steps));

          if (!quantise({current.x + std::cos(angle) * radius, current.y + std::sin(angle) * radius}, p))
          {
            return false;
          }

          candidates.push_back(p);
        }
      }
      else
      {
        const Vector normalIn{incoming.y, -incoming.x};
        const Vector normalOut{outgoing.y, -outgoing.x};

        for (const double side : {1.0, -1.0})
        {
          if (!quantise({current.x + normalIn.x * d * side, current.y + normalIn.y * d * side}, p))
          {
            return false;
          }

          candidates.push_back(p);

          if (!quantise({current.x + normalOut.x * d * side, current.y + normalOut.y * d * side}, p))
          {
            return false;
          }

          candidates.push_back(p);

          if (options.join != JoinStyle::Miter)
          {
            continue;
          }

          // The miter apex is on the bisector of the two offset lines. Its
          // distance from the corner grows without bound as the corner
          // sharpens, so the limit is what keeps a near-spike from throwing a
          // join across the map.
          Vector bisector;

          if (!normalise({normalIn.x + normalOut.x, normalIn.y + normalOut.y}, bisector))
          {
            continue;
          }

          const double cosHalf = bisector.x * normalIn.x + bisector.y * normalIn.y;

          if (!(cosHalf > 1.0e-9))
          {
            continue;
          }

          const double reach = d / cosHalf;

          if (reach > options.miterLimit * d)
          {
            continue;
          }

          if (!quantise({current.x + bisector.x * reach * side, current.y + bisector.y * reach * side}, p))
          {
            return false;
          }

          candidates.push_back(p);
        }
      }

      Ring join = hullOf(std::move(candidates));

      if (join.size() >= 3)
      {
        if (!counterClockwise(join))
        {
          reverse(join);
        }

        out.push_back(std::move(join));
      }
    }
  }

  return true;
}

} // namespace

bool offsetPolygons(const std::vector<Polygon> &polygons, const double metres, const OffsetOptions &options, std::vector<Polygon> &out, std::string *error)
{
  out.clear();

  if (!std::isfinite(metres))
  {
    return fail(error, "offset distance is not finite");
  }

  std::vector<Ring> rings;

  for (const auto &polygon : polygons)
  {
    rings.push_back(polygon.outer);

    for (const auto &hole : polygon.holes)
    {
      rings.push_back(hole);
    }
  }

  if (rings.empty())
  {
    return true;
  }

  if (metres == 0.0)
  {
    BooleanOptions pass;
    pass.limits = options.limits;

    Operand operand;
    operand.rings = rings;

    return booleanOp({operand}, Operation::Union, pass, out, error);
  }

  std::vector<Ring> band;

  if (!buildBand(rings, metres, options, band))
  {
    return fail(error, "offset ran outside the representable grid");
  }

  Operand subject;
  subject.rings = rings;

  Operand sleeve;
  sleeve.rings = std::move(band);

  BooleanOptions pass;
  pass.limits = options.limits;

  // Growing is the polygon united with the band; shrinking is the polygon less
  // the band. Every awkward outcome - a U splitting in two when its notch
  // closes, two arms merging into one ring, a shape vanishing entirely - is
  // the winding resolution doing its ordinary job, not a case handled here.
  return booleanOp({subject, sleeve}, metres > 0.0 ? Operation::Union : Operation::Difference, pass, out, error);
}

bool offsetPolygon(const Polygon &polygon, const double metres, const OffsetOptions &options, std::vector<Polygon> &out, std::string *error)
{
  return offsetPolygons({polygon}, metres, options, out, error);
}

bool offsetPolyline(const std::vector<Point> &path, const double halfWidthMetres, const OffsetOptions &options, std::vector<Polygon> &out, std::string *error)
{
  out.clear();

  if (path.size() < 2)
  {
    return fail(error, "a polyline needs at least two points");
  }

  if (!(halfWidthMetres > 0.0))
  {
    return fail(error, "a ribbon needs a positive half width");
  }

  // The band of an open path is the ribbon itself: every point within the half
  // width of the centreline. Building it from the same sleeve-and-join
  // construction means a road and a river get their width the same way, and a
  // path that doubles back on itself merges rather than self-intersecting.
  std::vector<Ring> centre;
  Ring closed = path;

  // Walked out and back, so the sleeve construction covers every segment once
  // in each direction and the joins close at both ends.
  //
  // The two endpoints are not repeated. Repeating one leaves a zero-length
  // edge whose direction cannot be normalised, and the join at that vertex is
  // then skipped - which silently costs the ribbon one of its two end caps.
  for (size_t i = path.size() - 1; i > 1; --i)
  {
    closed.push_back(path[i - 1]);
  }

  centre.push_back(std::move(closed));

  std::vector<Ring> band;

  if (!buildBand(centre, halfWidthMetres, options, band))
  {
    return fail(error, "ribbon ran outside the representable grid");
  }

  Operand sleeve;
  sleeve.rings = std::move(band);

  BooleanOptions pass;
  pass.limits = options.limits;

  return booleanOp({sleeve}, Operation::Union, pass, out, error);
}

bool offsetPolylineTapered(const std::vector<Point> &path, const std::vector<double> &halfWidths, const OffsetOptions &options, std::vector<Polygon> &out, std::string *error)
{
  out.clear();

  if (path.size() < 2 || halfWidths.size() != path.size())
  {
    return fail(error, "a tapered ribbon needs a half width for every point");
  }

  for (const double w : halfWidths)
  {
    if (!std::isfinite(w) || w < 0)
    {
      return fail(error, "a tapered ribbon needs finite non-negative half widths");
    }
  }

  // Walk one bank from the head to the mouth and the other back, offsetting
  // each point by its own half width along the bisector of its two edges. One
  // ring for the whole course, so the two banks meet at the ends and nothing
  // has to be stitched afterwards.
  //
  // The bisector rather than the segment normal: at a bend, offsetting by the
  // normal of each segment separately leaves a wedge open on the outside and a
  // crossing on the inside. The crossings that remain at a hairpin are resolved
  // by the union below, which is the same machinery every other offset uses.
  std::vector<Vector> left, right;

  for (size_t i = 0; i < path.size(); ++i)
  {
    const Vector here = metresOf(path[i]);

    Vector incoming{0, 0}, outgoing{0, 0};
    bool hasIncoming = false, hasOutgoing = false;

    if (i > 0)
    {
      const Vector previous = metresOf(path[i - 1]);
      hasIncoming = normalise({here.x - previous.x, here.y - previous.y}, incoming);
    }

    if (i + 1 < path.size())
    {
      const Vector next = metresOf(path[i + 1]);
      hasOutgoing = normalise({next.x - here.x, next.y - here.y}, outgoing);
    }

    if (!hasIncoming && !hasOutgoing)
    {
      continue;
    }

    const Vector direction = !hasIncoming ? outgoing : (!hasOutgoing ? incoming : Vector{incoming.x + outgoing.x, incoming.y + outgoing.y});

    Vector unit;

    if (!normalise(direction, unit))
    {
      unit = hasOutgoing ? outgoing : incoming;
    }

    Vector normal{unit.y, -unit.x};

    // At a bend the bisector is shorter than the normal, so the offset has to
    // be lengthened by 1/cos(half the turn) to keep the bank the right
    // distance out. Clamped, because at a hairpin that factor runs away.
    double stretch = 1;

    if (hasIncoming && hasOutgoing)
    {
      const double cosine = incoming.x * unit.x + incoming.y * unit.y;
      stretch = cosine > .2 ? 1.0 / cosine : 5.0;
    }

    const double reach = halfWidths[i] * std::min(stretch, 5.0);

    left.push_back({here.x + normal.x * reach, here.y + normal.y * reach});
    right.push_back({here.x - normal.x * reach, here.y - normal.y * reach});
  }

  if (left.size() < 2)
  {
    return fail(error, "a tapered ribbon collapsed to a point");
  }

  Ring ring;

  const auto push = [&](const Vector &v) {
    Point p;

    if (quantise(v, p) && (ring.empty() || ring.back() != p))
    {
      ring.push_back(p);
    }
  };

  for (const auto &v : left)
  {
    push(v);
  }

  for (size_t i = right.size(); i > 0; --i)
  {
    push(right[i - 1]);
  }

  while (ring.size() > 1 && ring.front() == ring.back())
  {
    ring.pop_back();
  }

  if (ring.size() < 3)
  {
    return fail(error, "a tapered ribbon collapsed to a line");
  }

  if (!counterClockwise(ring))
  {
    reverse(ring);
  }

  // The ring may cross itself where the course doubles back on a bend tighter
  // than its own width. A positive-winding union resolves that the same way
  // every other offset resolves it.
  Operand operand;
  operand.rings.push_back(std::move(ring));

  BooleanOptions pass;
  pass.limits = options.limits;

  return booleanOp({operand}, Operation::Union, pass, out, error);
}

bool cutPolygon(const Polygon &polygon, const Point &a, const Point &b, std::vector<Polygon> &out, std::string *error)
{
  out.clear();

  if (a == b)
  {
    return fail(error, "a cut needs two distinct points");
  }

  const std::string fault = polygonFault(polygon);

  if (!fault.empty())
  {
    return fail(error, "cut subject is not a valid polygon");
  }

  // Extend the line well past the polygon, so the caller can name two points
  // anywhere on it without working out how far the cut has to run.
  const Box box = bounds(polygon);
  const double span = (toMetres(box.maxX - box.minX) + toMetres(box.maxY - box.minY)) * 2.0 + 1.0;

  const Vector from = metresOf(a);
  const Vector to = metresOf(b);

  Vector direction;

  if (!normalise({to.x - from.x, to.y - from.y}, direction))
  {
    return fail(error, "a cut needs two distinct points");
  }

  Point start, end;

  if (!quantise({from.x - direction.x * span, from.y - direction.y * span}, start) || !quantise({from.x + direction.x * span, from.y + direction.y * span}, end))
  {
    return fail(error, "cut ran outside the representable grid");
  }

  std::vector<InputRing> rings;
  rings.push_back({polygon.outer, 0, false});

  for (const auto &hole : polygon.holes)
  {
    rings.push_back({hole, 0, false});
  }

  rings.push_back({{start, end}, 1, true});

  Arrangement arrangement;

  if (!buildArrangement(rings, {}, arrangement, error))
  {
    return false;
  }

  return extractRegions(
      arrangement,
      [](const std::vector<int32_t> &winding) {
        return winding[0] != 0;
      },
      out,
      error);
}

bool cutPolygonBySegment(const Polygon &polygon, const Point &a, const Point &b, std::vector<Polygon> &out, std::string *error)
{
  return cutPolygon(polygon, a, b, out, error);
}

} // namespace procedural::geom2
