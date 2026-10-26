#include "procedural/geom2/Arrangement.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <map>

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

__int128 cross128(const Point &a, const Point &b, const Point &c)
{
  const __int128 abx = __int128(b.x) - a.x;
  const __int128 aby = __int128(b.y) - a.y;
  const __int128 acx = __int128(c.x) - a.x;
  const __int128 acy = __int128(c.y) - a.y;

  return abx * acy - aby * acx;
}

bool withinBox(const Point &a, const Point &b, const Point &p)
{
  return p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x) && p.y >= std::min(a.y, b.y) && p.y <= std::max(a.y, b.y);
}

// Rounds num/den to the nearest integer, halves away from zero. Symmetric for
// the same reason the grid itself is: a shape must not change when the world
// origin moves.
int64_t divideRounded(const __int128 num, const __int128 den)
{
  const __int128 sign = (num < 0) != (den < 0) ? -1 : 1;
  const __int128 a = num < 0 ? -num : num;
  const __int128 b = den < 0 ? -den : den;

  return int64_t(sign * ((2 * a + b) / (2 * b)));
}

// Whether segment ab passes through the grid pixel centred on p: the unit
// square [p - 1/2, p + 1/2] in each axis.
//
// This, and not exact collinearity, is the test that decides where a segment
// must be split. The intersection of two integer segments is rational and has
// to be snapped to the grid, and the snapped point almost never lies exactly on
// either segment - it lands exactly on an axis-aligned one and half a
// micrometre off a slanted one. Splitting only where the point is exactly
// collinear therefore splits one of the two crossing segments and not the
// other, and the curves pass straight through each other with no shared
// vertex: an arrangement in which two overlapping shapes are reported disjoint.
//
// Snap rounding instead re-routes each segment through every hot pixel it
// passes, bending it by at most half a unit, so both segments genuinely meet at
// the shared vertex.
//
// Exact on integers: doubling puts the half-unit pixel bounds on whole numbers,
// and the products stay inside 2^86.
bool passesThroughPixel(const Point &a, const Point &b, const Point &p)
{
  const __int128 ax = __int128(a.x) * 2, ay = __int128(a.y) * 2;
  const __int128 bx = __int128(b.x) * 2, by = __int128(b.y) * 2;
  const __int128 lowX = __int128(p.x) * 2 - 1, highX = __int128(p.x) * 2 + 1;
  const __int128 lowY = __int128(p.y) * 2 - 1, highY = __int128(p.y) * 2 + 1;

  if (std::max(ax, bx) < lowX || std::min(ax, bx) > highX || std::max(ay, by) < lowY || std::min(ay, by) > highY)
  {
    return false;
  }

  const __int128 dx = bx - ax;
  const __int128 dy = by - ay;
  const __int128 cornerX[4] = {lowX, highX, highX, lowX};
  const __int128 cornerY[4] = {lowY, lowY, highY, highY};

  // The pixel is open: a line that only grazes one of its corners does not
  // pass through it.
  //
  // Counting a corner exactly on the line as a crossing is what made a
  // one-unit diagonal (x, y)-(x+1, y+1) split at the other two corners of its
  // own unit square, whose squares it touches at a point. The middle piece is
  // then the opposite diagonal, which the next round splits at the original
  // two corners, and so on without end: a dense block ran out its whole
  // budget with the vertex count fixed and the segment count rising by two a
  // round. A line through a pixel's interior always has corners strictly on
  // both sides of it - a diagonal through the centre included - so nothing
  // that should pass is lost.
  int sign = 0;

  for (int k = 0; k < 4; ++k)
  {
    const __int128 side = dx * (cornerY[k] - ay) - dy * (cornerX[k] - ax);

    if (side == 0)
    {
      continue;
    }

    const int current = side > 0 ? 1 : -1;

    if (sign == 0)
    {
      sign = current;
    }
    else if (current != sign)
    {
      return true;
    }
  }

  return false;
}

// Position of p along ab, as an exact integer. Orders a segment's split points:
// once a segment may bend through a pixel its points are no longer exactly
// collinear, so only the projection puts them in order along it.
__int128 alongSegment(const Point &a, const Point &b, const Point &p)
{
  return (__int128(b.x) - a.x) * (__int128(p.x) - a.x) + (__int128(b.y) - a.y) * (__int128(p.y) - a.y);
}

// One segment of the arrangement, before it becomes half-edges.
struct Segment
{
  Point a, b;
  uint32_t input = 0;
  int32_t direction = 1; // +1 when the ring edge runs a->b, -1 when b->a.
  bool hard = false;     // From an open path: divides faces, winds nothing.
};

// The proper crossing of ab and cd, snapped to the grid.
//
// Proper means the two cross at an interior point of both; shared endpoints and
// collinear overlap produce no new vertex here, because every endpoint is
// already a vertex and the splitting pass below cuts each segment at every
// vertex lying on it. Handling those cases here as well would add the same
// point twice under two different roundings.
bool properCrossing(const Point &a, const Point &b, const Point &c, const Point &d, Point &out)
{
  const __int128 d1x = __int128(b.x) - a.x;
  const __int128 d1y = __int128(b.y) - a.y;
  const __int128 d2x = __int128(d.x) - c.x;
  const __int128 d2y = __int128(d.y) - c.y;

  const __int128 denominator = d1x * d2y - d1y * d2x;

  if (denominator == 0)
  {
    return false;
  }

  const __int128 ecx = __int128(c.x) - a.x;
  const __int128 ecy = __int128(c.y) - a.y;

  const __int128 tNumerator = ecx * d2y - ecy * d2x;
  const __int128 uNumerator = ecx * d1y - ecy * d1x;

  // Strictly interior to both, so a touch at an endpoint is left to the
  // splitting pass. Comparisons are on the rational parameters without
  // dividing, so they stay exact.
  const bool positive = denominator > 0;
  const __int128 low = positive ? __int128(0) : denominator;
  const __int128 high = positive ? denominator : __int128(0);

  if (tNumerator <= low || tNumerator >= high || uNumerator <= low || uNumerator >= high)
  {
    return false;
  }

  // The triple product below is why kMaximumUnit is 2^41: tNumerator reaches
  // 2^84 and the difference 2^42, so the product reaches 2^126.
  out.x = a.x + divideRounded(tNumerator * d1x, denominator);
  out.y = a.y + divideRounded(tNumerator * d1y, denominator);

  return true;
}

// Compares two directions around a shared origin, counter-clockwise from the
// positive x axis. Exact: a half test then a cross product, never an angle.
struct DirectionLess
{
  static int half(const int64_t dx, const int64_t dy)
  {
    return dy < 0 || (dy == 0 && dx < 0) ? 1 : 0;
  }

  bool operator()(const std::pair<int64_t, int64_t> &a, const std::pair<int64_t, int64_t> &b) const
  {
    const int ha = half(a.first, a.second);
    const int hb = half(b.first, b.second);

    if (ha != hb)
    {
      return ha < hb;
    }

    const __int128 turn = __int128(a.first) * b.second - __int128(a.second) * b.first;

    if (turn != 0)
    {
      return turn > 0;
    }

    // Same direction: order by length so the comparator is a strict weak
    // ordering even for duplicated directions, which cannot survive the edge
    // deduplication but can appear while sorting.
    const __int128 la = __int128(a.first) * a.first + __int128(a.second) * a.second;
    const __int128 lb = __int128(b.first) * b.first + __int128(b.second) * b.second;

    return la < lb;
  }
};

} // namespace

bool buildArrangement(const std::vector<InputRing> &rings, const ArrangementLimits &limits, Arrangement &out, std::string *error)
{
  Arrangement result;

  for (const auto &ring : rings)
  {
    result.inputs = std::max(result.inputs, ring.input + 1);
  }

  if (result.inputs == 0)
  {
    return fail(error, "arrangement needs at least one ring");
  }

  std::vector<Segment> segments;

  for (const auto &entry : rings)
  {
    if (entry.ring.size() < (entry.open ? 2u : 3u))
    {
      return fail(error, entry.open ? "arrangement path has fewer than two vertices" : "arrangement ring has fewer than three vertices");
    }

    const size_t edges = entry.open ? entry.ring.size() - 1 : entry.ring.size();

    for (size_t i = 0; i < edges; ++i)
    {
      const Point &a = entry.ring[i];
      const Point &b = entry.ring[i + 1 < entry.ring.size() ? i + 1 : 0];

      if (std::abs(a.x) > kMaximumUnit || std::abs(a.y) > kMaximumUnit)
      {
        return fail(error, "arrangement vertex outside the representable grid");
      }

      if (a == b)
      {
        continue;
      }

      // Stored in a canonical direction so that two rings tracing the same
      // boundary in opposite directions produce one edge whose crossings
      // cancel, rather than two edges that fail to weld.
      if (b < a)
      {
        segments.push_back({b, a, entry.input, entry.open ? 0 : -1, entry.open});
      }
      else
      {
        segments.push_back({a, b, entry.input, entry.open ? 0 : 1, entry.open});
      }
    }
  }

  if (segments.empty())
  {
    return fail(error, "arrangement has no usable edges");
  }

  // Snap iteration. Splitting at a snapped crossing can move a segment across a
  // vertex it did not previously meet, so the whole pass repeats until nothing
  // new is produced. It terminates because splits only ever add vertices on a
  // finite grid inside the input's bounding box, and segments only get shorter.
  for (result.snapRounds = 1;; ++result.snapRounds)
  {
    if (segments.size() > limits.maximumSegments)
    {
      return fail(error, "arrangement segment budget exceeded");
    }

    std::vector<Point> points;
    points.reserve(segments.size() * 2);

    for (const auto &s : segments)
    {
      points.push_back(s.a);
      points.push_back(s.b);
    }

    // Candidate crossings. Segments are ordered by their lower x so that the
    // inner loop can stop as soon as a segment starts beyond the current one's
    // right edge; that prune is what keeps this usable without the full
    // machinery of a sweep, which is far harder to make correct at degeneracy.
    std::sort(segments.begin(), segments.end(), [](const Segment &l, const Segment &r) {
      return l.a.x != r.a.x ? l.a.x < r.a.x : l.a.y < r.a.y;
    });

    for (size_t i = 0; i < segments.size(); ++i)
    {
      const int64_t rightI = std::max(segments[i].a.x, segments[i].b.x);

      for (size_t j = i + 1; j < segments.size(); ++j)
      {
        if (segments[j].a.x > rightI)
        {
          break;
        }

        Point hit;

        if (properCrossing(segments[i].a, segments[i].b, segments[j].a, segments[j].b, hit))
        {
          points.push_back(hit);
        }
      }
    }

    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());

    if (points.size() > limits.maximumVertices)
    {
      return fail(error, "arrangement vertex budget exceeded");
    }

    // Split every segment at every vertex lying on it. This is what resolves
    // collinear overlap and T junctions as well as proper crossings: all three
    // are just "a vertex sits in this segment's interior".
    std::vector<Segment> split;
    split.reserve(segments.size());
    bool changed = false;

    for (const auto &s : segments)
    {
      // Widened by one unit, because a pixel reaches half a unit either side.
      const auto first = std::lower_bound(points.begin(), points.end(), Point{std::min(s.a.x, s.b.x) - 1, std::numeric_limits<int64_t>::min()});
      const auto last = std::upper_bound(points.begin(), points.end(), Point{std::max(s.a.x, s.b.x) + 1, std::numeric_limits<int64_t>::max()});

      std::vector<Point> interior;

      // A vertex is interior to a segment only if it projects strictly between
      // the segment's endpoints.
      //
      // passesThroughPixel asks whether the segment's line crosses the pixel's
      // square, and a pixel whose square touches the segment just past an
      // endpoint answers yes while its centre projects beyond that endpoint.
      // Ordered along the segment it then lands *after* b, and the split
      // produces a fragment (b, p) under a unit long. The next round finds the
      // next pixel of the same junction cluster touching that fragment and
      // splits it again, and so on: a dense town block ran 96 rounds with the
      // vertex count fixed at 173 and the segment count rising by exactly two
      // each round. The endpoints are vertices already, so a pixel at or past
      // an end has nothing to reroute.
      const __int128 extent = alongSegment(s.a, s.b, s.b);

      // A segment of one unit step has nowhere to bend: every pixel it could
      // reach is one of its own endpoints' neighbours.
      if (extent <= 2)
      {
        split.push_back(s);

        continue;
      }

      for (auto it = first; it != last; ++it)
      {
        if (*it == s.a || *it == s.b)
        {
          continue;
        }

        const __int128 along = alongSegment(s.a, s.b, *it);

        if (along <= 0 || along >= extent)
        {
          continue;
        }

        if (passesThroughPixel(s.a, s.b, *it))
        {
          interior.push_back(*it);
        }
      }

      if (interior.empty())
      {
        split.push_back(s);

        continue;
      }

      changed = true;

      // Ordered along the segment, not lexicographically: a bent segment's
      // points are no longer collinear, so only the projection orders them.
      std::sort(interior.begin(), interior.end(), [&s](const Point &l, const Point &r) {
        const __int128 dl = alongSegment(s.a, s.b, l);
        const __int128 dr = alongSegment(s.a, s.b, r);

        return dl != dr ? dl < dr : l < r;
      });

      interior.insert(interior.begin(), s.a);
      interior.push_back(s.b);
      interior.erase(std::unique(interior.begin(), interior.end()), interior.end());

      for (size_t k = 0; k + 1 < interior.size(); ++k)
      {
        if (interior[k] != interior[k + 1])
        {
          split.push_back({interior[k], interior[k + 1], s.input, s.direction, s.hard});
        }
      }
    }

    segments.swap(split);

    if (!changed)
    {
      result.vertices = std::move(points);

      break;
    }

    if (result.snapRounds >= limits.maximumSnapRounds)
    {
      return fail(error, "arrangement did not settle within the snap round budget");
    }
  }

  std::map<Point, uint32_t> index;

  for (uint32_t i = 0; i < result.vertices.size(); ++i)
  {
    index.emplace(result.vertices[i], i);
  }

  // Deduplicate into undirected edges, accumulating the crossing vector. Two
  // rings that share a boundary become one edge carrying both contributions,
  // which is what lets the winding propagate correctly across a shared wall.
  std::map<std::pair<uint32_t, uint32_t>, std::vector<int32_t>> edges;
  std::map<std::pair<uint32_t, uint32_t>, bool> hardness;

  for (const auto &s : segments)
  {
    if (s.a == s.b)
    {
      continue;
    }

    const uint32_t u = index.at(s.a);
    const uint32_t v = index.at(s.b);
    const auto key = std::make_pair(std::min(u, v), std::max(u, v));

    auto it = edges.find(key);

    if (it == edges.end())
    {
      it = edges.emplace(key, std::vector<int32_t>(result.inputs, 0)).first;
    }

    // `direction` restored the ring's own sense; `u < v` says whether this
    // segment is stored along the canonical edge or against it.
    it->second[s.input] += u < v ? s.direction : -s.direction;

    if (s.hard)
    {
      hardness[key] = true;
    }
  }

  if (edges.empty())
  {
    return fail(error, "arrangement collapsed to no edges");
  }

  std::vector<std::vector<uint32_t>> outgoing(result.vertices.size());

  for (const auto &edge : edges)
  {
    const uint32_t u = edge.first.first;
    const uint32_t v = edge.first.second;

    const bool hard = hardness.count(edge.first) != 0;

    HalfEdge forward;
    forward.origin = u;
    forward.crossing = edge.second;
    forward.hard = hard;

    HalfEdge backward;
    backward.origin = v;
    backward.hard = hard;
    backward.crossing.assign(result.inputs, 0);

    for (uint32_t i = 0; i < result.inputs; ++i)
    {
      backward.crossing[i] = -edge.second[i];
    }

    const uint32_t forwardIndex = uint32_t(result.halfEdges.size());
    forward.twin = forwardIndex + 1;
    backward.twin = forwardIndex;

    result.halfEdges.push_back(std::move(forward));
    result.halfEdges.push_back(std::move(backward));

    outgoing[u].push_back(forwardIndex);
    outgoing[v].push_back(forwardIndex + 1);
  }

  // Link `next`. Around the destination vertex, the next half-edge of the same
  // face is the one immediately clockwise from this edge's twin; that rule is
  // what makes every bounded face come out counter-clockwise with its interior
  // on the left.
  for (uint32_t v = 0; v < result.vertices.size(); ++v)
  {
    auto &fan = outgoing[v];

    if (fan.empty())
    {
      continue;
    }

    std::sort(fan.begin(), fan.end(), [&](const uint32_t l, const uint32_t r) {
      const Point &origin = result.vertices[v];
      const Point &lp = result.vertices[result.halfEdges[result.halfEdges[l].twin].origin];
      const Point &rp = result.vertices[result.halfEdges[result.halfEdges[r].twin].origin];

      return DirectionLess{}({lp.x - origin.x, lp.y - origin.y}, {rp.x - origin.x, rp.y - origin.y});
    });

    for (size_t k = 0; k < fan.size(); ++k)
    {
      // fan is counter-clockwise, so the previous entry is the clockwise
      // neighbour, wrapping at the start.
      const uint32_t incoming = result.halfEdges[fan[k]].twin;
      const uint32_t clockwise = fan[k == 0 ? fan.size() - 1 : k - 1];

      result.halfEdges[incoming].next = clockwise;
    }
  }

  // Walk cycles. Each is a closed chain of half-edges with the interior on the
  // left; a positive area means it bounds a face from outside, a negative one
  // means it bounds a hole from inside.
  constexpr uint32_t unassigned = 0xffffffffu;

  for (auto &halfEdge : result.halfEdges)
  {
    halfEdge.face = unassigned;
  }

  std::vector<uint32_t> cycleStart;
  std::vector<uint32_t> cycleOf(result.halfEdges.size(), unassigned);

  for (uint32_t start = 0; start < result.halfEdges.size(); ++start)
  {
    if (cycleOf[start] != unassigned)
    {
      continue;
    }

    const uint32_t cycleIndex = uint32_t(cycleStart.size());
    cycleStart.push_back(start);

    uint32_t current = start;

    do
    {
      cycleOf[current] = cycleIndex;
      current = result.halfEdges[current].next;
    } while (current != start);
  }

  std::vector<double> cycleArea(cycleStart.size(), 0.0);

  for (uint32_t c = 0; c < cycleStart.size(); ++c)
  {
    cycleArea[c] = cycleAreaMetres(result, cycleStart[c]);
  }

  // The outermost cycle is the one enclosing the most negative area. Every
  // other negative cycle bounds a hole inside some face.
  uint32_t outermost = 0;

  for (uint32_t c = 1; c < cycleStart.size(); ++c)
  {
    if (cycleArea[c] < cycleArea[outermost])
    {
      outermost = c;
    }
  }

  // Attach each inner cycle to the cycle whose face contains it. A ray is cast
  // leftward from the cycle's leftmost vertex; the first edge it meets bounds
  // the containing face. Ray casting rather than point-in-polygon because the
  // arrangement can be disconnected - an island in a lake - and a hole's
  // vertices lie on a boundary, where containment has no answer.
  //
  // The edge found may itself belong to another inner cycle, in which case both
  // lie in the same face, so the links are resolved transitively below.
  std::vector<uint32_t> parent(cycleStart.size(), unassigned);

  for (uint32_t c = 0; c < cycleStart.size(); ++c)
  {
    // Zero-area cycles are included. An open path that meets nothing - a cut
    // line that misses its target - walks out and back with no area, and still
    // has to be placed in the face that contains it.
    if (c == outermost || cycleArea[c] > 0.0)
    {
      continue;
    }

    Point leftmost = result.vertices[result.halfEdges[cycleStart[c]].origin];
    uint32_t current = cycleStart[c];

    do
    {
      const Point &p = result.vertices[result.halfEdges[current].origin];

      if (p < leftmost)
      {
        leftmost = p;
      }

      current = result.halfEdges[current].next;
    } while (current != cycleStart[c]);

    bool hit = false;
    __int128 bestNumerator = 0;
    __int128 bestDenominator = 1;
    uint32_t bestEdge = 0;

    for (uint32_t h = 0; h < result.halfEdges.size(); h += 2)
    {
      const Point &p = result.vertices[result.halfEdges[h].origin];
      const Point &q = result.vertices[result.halfEdges[result.halfEdges[h].twin].origin];

      // Half-open in y, so a vertex exactly at the ray's height is counted by
      // one of its two edges and never by both.
      const bool straddles = (p.y <= leftmost.y && q.y > leftmost.y) || (q.y <= leftmost.y && p.y > leftmost.y);

      if (!straddles)
      {
        continue;
      }

      __int128 denominator = __int128(q.y) - p.y;
      __int128 numerator = __int128(p.x) * denominator + (__int128(leftmost.y) - p.y) * (__int128(q.x) - p.x);

      if (denominator < 0)
      {
        denominator = -denominator;
        numerator = -numerator;
      }

      // Strictly left of the origin, so the cycle's own edges cannot be hit.
      if (numerator >= __int128(leftmost.x) * denominator)
      {
        continue;
      }

      if (!hit || numerator * bestDenominator > bestNumerator * denominator)
      {
        hit = true;
        bestNumerator = numerator;
        bestDenominator = denominator;
        bestEdge = h;
      }
    }

    if (!hit)
    {
      parent[c] = outermost;

      continue;
    }

    // Of the two half-edges, the one pointing downward has its face on the
    // side the ray came from, which is the side the inner cycle sits on.
    const Point &p = result.vertices[result.halfEdges[bestEdge].origin];
    const Point &q = result.vertices[result.halfEdges[result.halfEdges[bestEdge].twin].origin];
    const uint32_t downward = q.y < p.y ? bestEdge : result.halfEdges[bestEdge].twin;

    parent[c] = cycleOf[downward];
  }

  // Resolve the chains, then build one face per outer cycle plus the unbounded
  // face, with every inner cycle attached to the face that contains it.
  std::vector<uint32_t> faceOfCycle(cycleStart.size(), unassigned);

  for (uint32_t c = 0; c < cycleStart.size(); ++c)
  {
    if (cycleArea[c] > 0.0 || c == outermost)
    {
      Face face;
      face.winding.assign(result.inputs, 0);
      face.cycles.push_back(cycleStart[c]);
      face.unbounded = c == outermost;

      faceOfCycle[c] = uint32_t(result.faces.size());
      result.faces.push_back(std::move(face));

      if (c == outermost)
      {
        result.unboundedFace = faceOfCycle[c];
      }
    }
  }

  for (uint32_t c = 0; c < cycleStart.size(); ++c)
  {
    if (faceOfCycle[c] != unassigned)
    {
      continue;
    }

    uint32_t root = c;
    size_t guard = 0;

    while (faceOfCycle[root] == unassigned)
    {
      if (parent[root] == unassigned || ++guard > cycleStart.size())
      {
        return fail(error, "arrangement could not place an inner cycle in a face");
      }

      root = parent[root];
    }

    faceOfCycle[c] = faceOfCycle[root];
    result.faces[faceOfCycle[c]].cycles.push_back(cycleStart[c]);
  }

  for (uint32_t h = 0; h < result.halfEdges.size(); ++h)
  {
    result.halfEdges[h].face = faceOfCycle[cycleOf[h]];
  }

  // Propagate winding outward from the unbounded face, which is outside
  // everything. Crossing a half-edge into the face on its left adds that
  // edge's crossing vector: a counter-clockwise ring has its interior on the
  // left, so entering it raises the winding by one.
  std::vector<bool> visited(result.faces.size(), false);
  std::vector<uint32_t> queue{result.unboundedFace};
  visited[result.unboundedFace] = true;

  while (!queue.empty())
  {
    const uint32_t faceIndex = queue.back();
    queue.pop_back();

    for (const uint32_t cycle : result.faces[faceIndex].cycles)
    {
      uint32_t current = cycle;

      do
      {
        const HalfEdge &halfEdge = result.halfEdges[current];
        const HalfEdge &twin = result.halfEdges[halfEdge.twin];

        if (!visited[twin.face])
        {
          visited[twin.face] = true;

          for (uint32_t i = 0; i < result.inputs; ++i)
          {
            result.faces[twin.face].winding[i] = result.faces[faceIndex].winding[i] + twin.crossing[i];
          }

          queue.push_back(twin.face);
        }

        current = halfEdge.next;
      } while (current != cycle);
    }
  }

  for (uint32_t f = 0; f < result.faces.size(); ++f)
  {
    if (!visited[f])
    {
      return fail(error, "arrangement face graph is not connected through its edges");
    }
  }

  out = std::move(result);

  return true;
}

double cycleAreaMetres(const Arrangement &arrangement, const uint32_t halfEdge)
{
  __int128 doubled = 0;
  const Point &origin = arrangement.vertices[arrangement.halfEdges[halfEdge].origin];

  uint32_t current = halfEdge;

  do
  {
    const Point &a = arrangement.vertices[arrangement.halfEdges[current].origin];
    const uint32_t next = arrangement.halfEdges[current].next;
    const Point &b = arrangement.vertices[arrangement.halfEdges[next].origin];

    doubled += cross128(origin, a, b);
    current = next;
  } while (current != halfEdge);

  return double(doubled) * 0.5 * kMetresPerUnit * kMetresPerUnit;
}

double faceAreaMetres(const Arrangement &arrangement, const uint32_t face)
{
  double total = 0.0;

  for (const uint32_t cycle : arrangement.faces[face].cycles)
  {
    total += cycleAreaMetres(arrangement, cycle);
  }

  return total;
}

std::string arrangementFault(const Arrangement &arrangement)
{
  const size_t n = arrangement.halfEdges.size();

  if (n == 0 || n % 2 != 0)
  {
    return "arrangement has no half-edges, or an odd number of them";
  }

  for (uint32_t i = 0; i < n; ++i)
  {
    const HalfEdge &halfEdge = arrangement.halfEdges[i];

    if (halfEdge.twin >= n || arrangement.halfEdges[halfEdge.twin].twin != i)
    {
      return "twin is not an involution";
    }

    if (halfEdge.next >= n)
    {
      return "next is out of range";
    }

    if (halfEdge.origin >= arrangement.vertices.size())
    {
      return "origin is out of range";
    }

    // The next half-edge must start where this one ends.
    if (arrangement.halfEdges[halfEdge.next].origin != arrangement.halfEdges[halfEdge.twin].origin)
    {
      return "next does not begin where the edge ends";
    }

    if (halfEdge.face >= arrangement.faces.size())
    {
      return "face is out of range";
    }
  }

  // Every half-edge belongs to exactly one cycle, and that cycle's face is its
  // own. Walking from each start and marking is enough to prove both.
  std::vector<uint32_t> visits(n, 0);

  for (const auto &face : arrangement.faces)
  {
    for (const uint32_t cycle : face.cycles)
    {
      uint32_t current = cycle;
      size_t guard = 0;

      do
      {
        if (++guard > n)
        {
          return "face cycle does not close";
        }

        ++visits[current];
        current = arrangement.halfEdges[current].next;
      } while (current != cycle);
    }
  }

  for (uint32_t i = 0; i < n; ++i)
  {
    if (visits[i] != 1)
    {
      return visits[i] == 0 ? "half-edge belongs to no face cycle" : "half-edge belongs to more than one face cycle";
    }
  }

  size_t unbounded = 0;

  for (const auto &face : arrangement.faces)
  {
    if (face.unbounded)
    {
      ++unbounded;
    }

    if (face.winding.size() != arrangement.inputs)
    {
      return "face winding vector has the wrong width";
    }
  }

  if (unbounded != 1)
  {
    return "arrangement does not have exactly one unbounded face";
  }

  if (cycleAreaMetres(arrangement, arrangement.faces[arrangement.unboundedFace].cycles.front()) >= 0.0)
  {
    return "the unbounded face does not enclose negative area";
  }

  for (uint32_t f = 0; f < arrangement.faces.size(); ++f)
  {
    if (f == arrangement.unboundedFace)
    {
      continue;
    }

    if (cycleAreaMetres(arrangement, arrangement.faces[f].cycles.front()) <= 0.0)
    {
      return "a bounded face does not enclose positive area";
    }
  }

  return {};
}

} // namespace procedural::geom2
