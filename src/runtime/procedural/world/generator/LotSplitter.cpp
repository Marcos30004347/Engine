#include "LotSplitter.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world::generator {

namespace {

using Ring = std::vector<Position>;

double distance(Position a, Position b)
{
  return std::hypot(b.x - a.x, b.z - a.z);
}

Position lerp(Position a, Position b, double t)
{
  return {a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t};
}

double area(const Ring &p)
{
  double a = 0;

  for (size_t i = 0; i < p.size(); ++i)
  {
    a += p[i].x * p[(i + 1) % p.size()].z - p[i].z * p[(i + 1) % p.size()].x;
  }

  return a * .5;
}

// Area over the smallest rectangle aligned with one of the piece's edges.
double rectangularity(const Ring &p)
{
  const double a = std::abs(area(p));

  if (p.size() < 3 || a < 1e-9)
  {
    return 0;
  }

  double smallest = 1e30;

  for (size_t k = 0; k < p.size(); ++k)
  {
    const Position u = p[k], v = p[(k + 1) % p.size()];
    const double l = distance(u, v);

    if (l < 1e-9)
    {
      continue;
    }

    const double ex = (v.x - u.x) / l, ez = (v.z - u.z) / l;
    double lo = 1e30, hi = -1e30, nlo = 1e30, nhi = -1e30;

    for (auto q : p)
    {
      const double along = ex * q.x + ez * q.z, across = -ez * q.x + ex * q.z;
      lo = std::min(lo, along);
      hi = std::max(hi, along);
      nlo = std::min(nlo, across);
      nhi = std::max(nhi, across);
    }

    smallest = std::min(smallest, (hi - lo) * (nhi - nlo));
  }

  return smallest > 1e-9 ? std::min(1., a / smallest) : 0;
}

// Narrowest extent across any edge normal (for concave rings an upper bound of the true width).
double narrowestWidth(const Ring &p)
{
  double narrowest = 1e30;

  for (size_t k = 0; k < p.size(); ++k)
  {
    const Position u = p[k], v = p[(k + 1) % p.size()];
    const double l = distance(u, v);

    if (l < 1e-9)
    {
      continue;
    }

    const double ex = (v.z - u.z) / l, ez = (u.x - v.x) / l;
    double lo = 1e30, hi = -1e30;

    for (auto q : p)
    {
      lo = std::min(lo, ex * q.x + ez * q.z);
      hi = std::max(hi, ex * q.x + ez * q.z);
    }

    narrowest = std::min(narrowest, hi - lo);
  }

  return narrowest;
}

double sharpestCorner(const Ring &p)
{
  double sharpest = 3.14159265358979;

  for (size_t k = 0; k < p.size(); ++k)
  {
    const Position u = p[(k + p.size() - 1) % p.size()], v = p[k], w = p[(k + 1) % p.size()];
    const double ax = u.x - v.x, az = u.z - v.z, bx = w.x - v.x, bz = w.z - v.z;
    const double d = std::max(1e-9, std::hypot(ax, az) * std::hypot(bx, bz));
    const double angle = std::acos(std::clamp((ax * bx + az * bz) / d, -1., 1.));

    // Only convex corners count; a reflex corner is not a sharp tip.
    if (cross(u, v, w) < 0)
    {
      sharpest = std::min(sharpest, angle);
    }
  }

  return sharpest;
}

struct Piece
{
  Ring ring;
  std::vector<LotEdgeKind> kinds;
};

// The first boundary point the ray from `origin` (on edge `fromEdge`) reaches
// in direction (dx, dz), excluding the origin's own edge and its neighbours' shared vertices.
bool firstHit(const Ring &p, size_t fromEdge, Position origin, double dx, double dz, size_t &hitEdge, Position &hit)
{
  double best = 1e30;
  bool found = false;

  for (size_t e = 0; e < p.size(); ++e)
  {
    if (e == fromEdge)
    {
      continue;
    }

    const Position a = p[e], b = p[(e + 1) % p.size()];
    const double ex = b.x - a.x, ez = b.z - a.z;
    const double denominator = dx * ez - dz * ex;

    if (std::abs(denominator) < 1e-12)
    {
      continue;
    }

    const double t = ((a.x - origin.x) * ez - (a.z - origin.z) * ex) / denominator;
    const double u = ((a.x - origin.x) * dz - (a.z - origin.z) * dx) / denominator;

    if (t <= 1e-6 || u < -1e-9 || u > 1 + 1e-9)
    {
      continue;
    }

    if (t < best)
    {
      best = t;
      found = true;
      hitEdge = e;
      hit = {origin.x + dx * t, 0, origin.z + dz * t};
    }
  }

  return found;
}

// Even-odd containment: a block is concave, so the convex test lies about it.
bool insideRing(const Ring &p, Position q)
{
  bool in = false;

  for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
  {
    if ((p[i].z > q.z) != (p[j].z > q.z) && q.x < (p[j].x - p[i].x) * (q.z - p[i].z) / (p[j].z - p[i].z) + p[i].x)
    {
      in = !in;
    }
  }

  return in;
}

struct Splitter
{
  const LotSplitInput &s;
  LotSplitPlan &r;
  uint64_t draws = 0;

  double next()
  {
    return s.random(s.key, ++draws);
  }

  bool shapely(const Ring &p) const
  {
    return p.size() >= 3 && std::abs(area(p)) > 1 && narrowestWidth(p) >= s.minimumWidth && sharpestCorner(p) >= s.minimumCorner;
  }

  double frontage(const Piece &p) const
  {
    for (size_t i = 0; i < p.kinds.size(); ++i)
    {
      if ((p.kinds[i] == LotEdgeKind::Road || (s.chordsAreRoads && p.kinds[i] == LotEdgeKind::Chord)) && distance(p.ring[i], p.ring[(i + 1) % p.ring.size()]) >= 3)
      {
        return 1;
      }
    }

    return 0;
  }

  // Which kind each edge of a half carries: an edge lying on one of the
  // parent's edges keeps that edge's kind, an edge on the cut is a chord.
  std::vector<LotEdgeKind> kindsOf(const Piece &p, const Ring &half, Position from, Position to) const
  {
    std::vector<LotEdgeKind> k(half.size(), LotEdgeKind::None);

    for (size_t i = 0; i < half.size(); ++i)
    {
      const Position u = half[i], v = half[(i + 1) % half.size()];
      const Position m = lerp(u, v, .5);

      if (segmentDistance(m, from, to) < 1e-6 && distance(u, v) > 1e-6)
      {
        k[i] = LotEdgeKind::Chord;

        continue;
      }

      for (size_t j = 0; j < p.ring.size(); ++j)
      {
        if (segmentDistance(m, p.ring[j], p.ring[(j + 1) % p.ring.size()]) < 1e-6)
        {
          k[i] = j < p.kinds.size() ? p.kinds[j] : LotEdgeKind::Road;

          break;
        }
      }
    }

    return k;
  }

  void split(const Piece &p, uint32_t depth)
  {
    const double a = std::abs(area(p.ring));

    if (p.ring.size() < 3 || a < 1e-6)
    {
      return;
    }

    bool concave = false;

    for (size_t k = 0; s.convexLots && k < p.ring.size(); ++k)
    {
      concave |= cross(p.ring[k], p.ring[(k + 1) % p.ring.size()], p.ring[(k + 2) % p.ring.size()]) < -1e-6;
    }

    if ((a <= s.targetArea * (1 + s.spread * (next() - .5)) && !concave) || depth >= s.maximumDepth)
    {
      r.lots.push_back({p.ring, p.kinds});
      return;
    }

    double longest = 0;

    for (size_t e = 0; e < p.ring.size(); ++e)
    {
      longest = std::max(longest, distance(p.ring[e], p.ring[(e + 1) % p.ring.size()]));
    }

    Piece bestFirst, bestSecond;
    std::pair<Position, Position> bestChord;
    double bestScore = -1e30;

    for (size_t e = 0; e < p.ring.size(); ++e)
    {
      const Position ea = p.ring[e], eb = p.ring[(e + 1) % p.ring.size()];
      const double el = distance(ea, eb);

      if (el < longest * .4 || el < s.minimumWidth * 2)
      {
        continue;
      }

      // Inward normal of a CCW ring.
      const double nx = -(eb.z - ea.z) / el, nz = (eb.x - ea.x) / el;

      for (double t : {.35, .42, .5, .58, .65})
      {
        const Position from = lerp(ea, eb, t);
        size_t hitEdge = 0;
        Position to;

        if (!firstHit(p.ring, e, from, nx, nz, hitEdge, to))
        {
          continue;
        }

        Ring first, second;

        if (!LotSplitter::cut(p.ring, e, from, hitEdge, to, first, second))
        {
          continue;
        }

        ++r.cuts;

        if (!shapely(first) || !shapely(second))
        {
          ++r.refused;
          continue;
        }

        // Edge kinds of the halves: original edges keep theirs, the chord is a chord.
        auto kinds = [&](const Ring &half) {
          return kindsOf(p, half, from, to);
        };
        Piece one{first, kinds(first)}, two{second, kinds(second)};
        const double a1 = std::abs(area(first)), a2 = std::abs(area(second));
        double score = rectangularity(first) + rectangularity(second) - s.balanceWeight * std::abs(a1 - a2) / (a1 + a2) + s.frontageWeight * (frontage(one) + frontage(two)) + .02 * next();

        if (score > bestScore)
        {
          bestScore = score;
          bestFirst = std::move(one);
          bestSecond = std::move(two);
          bestChord = {from, to};
        }
      }
    }

    // Nothing scored, and the piece is concave: cut it at a reflex corner.
    //
    // A concave lot is not a lot - a house is inset from its boundary and the
    // inset of a re-entrant corner is not a boundary at all - so it was being
    // thrown away by the caller a stage later, two hundred of them a town. A
    // cut from the reflex vertex inward removes that vertex by construction,
    // which is the one move that always makes progress on a concave piece.
    if (bestScore <= -1e29 && concave)
    {
      for (size_t k = 0; k < p.ring.size() && bestScore <= -1e29; ++k)
      {
        const Position previous = p.ring[(k + p.ring.size() - 1) % p.ring.size()], v = p.ring[k], next = p.ring[(k + 1) % p.ring.size()];

        if (cross(previous, v, next) > -1e-6)
        {
          continue;
        }

        const double pl = distance(previous, v), nl = distance(next, v);

        if (pl < 1e-9 || nl < 1e-9)
        {
          continue;
        }

        double dx = (previous.x - v.x) / pl + (next.x - v.x) / nl;
        double dz = (previous.z - v.z) / pl + (next.z - v.z) / nl;
        const double dl = std::hypot(dx, dz);

        if (dl < 1e-9)
        {
          // The two edges double back: cut square to the incoming one instead.
          dx = -(v.z - previous.z) / pl;
          dz = (v.x - previous.x) / pl;
        }
        else
        {
          dx /= dl;
          dz /= dl;
        }

        for (const double sign : {-1., 1.})
        {
          if (!insideRing(p.ring, {v.x + dx * sign * .05, 0, v.z + dz * sign * .05}))
          {
            continue;
          }

          size_t hitEdge = 0;
          Position to;

          if (!firstHit(p.ring, k, v, dx * sign, dz * sign, hitEdge, to))
          {
            continue;
          }

          Ring first, second;

          if (!LotSplitter::cut(p.ring, k, v, hitEdge, to, first, second))
          {
            continue;
          }

          ++r.cuts;

          // Only the shape rules a piece must keep to be cut again; the lot
          // rules are applied to its children, which is where they belong.
          if (first.size() < 3 || second.size() < 3 || std::abs(area(first)) < 8 || std::abs(area(second)) < 8)
          {
            ++r.refused;

            continue;
          }

          bestFirst = Piece{first, kindsOf(p, first, v, to)};
          bestSecond = Piece{second, kindsOf(p, second, v, to)};
          bestChord = {v, to};
          bestScore = 0;

          break;
        }
      }
    }

    if (bestScore <= -1e29)
    {
      r.lots.push_back({p.ring, p.kinds});
      return;
    }

    r.chords.push_back(bestChord);
    split(bestFirst, depth + 1);
    split(bestSecond, depth + 1);
  }
};

} // namespace

LotSplitter::LotSplitter(uint64_t domain) : Generator("city.lot-splitter", domain, "city.lot-split-input", "city.lot-split-plan", GenerationScope::Patch)
{
}

bool LotSplitter::cut(const std::vector<Position> &polygon, size_t edgeA, Position a, size_t edgeB, Position b, std::vector<Position> &first, std::vector<Position> &second)
{
  const size_t n = polygon.size();

  if (n < 3 || edgeA >= n || edgeB >= n || edgeA == edgeB)
  {
    return false;
  }

  // Walk from a (on edge A) forward to b (on edge B), then from b forward to a.
  auto walk = [&](size_t fromEdge, Position from, size_t toEdge, Position to) {
    Ring ring{from};
    size_t e = fromEdge;

    for (size_t guard = 0; guard < n; ++guard)
    {
      e = (e + 1) % n;
      const Position v = polygon[e];

      if (distance(ring.back(), v) > 1e-9)
      {
        ring.push_back(v);
      }

      if (e == toEdge)
      {
        break;
      }
    }

    if (distance(ring.back(), to) > 1e-9)
    {
      ring.push_back(to);
    }

    return ring;
  };

  first = walk(edgeA, a, edgeB, b);
  second = walk(edgeB, b, edgeA, a);

  if (first.size() < 3 || second.size() < 3)
  {
    return false;
  }

  // The chord must lie inside: both halves keep a positive area of the whole.
  const double whole = std::abs(area(polygon));
  return area(first) > 1e-6 && area(second) > 1e-6 && std::abs(area(first) + area(second) - whole) < whole * 1e-3 + 1e-6;
}

bool LotSplitter::plan(const LotSplitInput &s, LotSplitPlan &r, Ledger &ledger, std::string *error)
{
  auto fail = [&](const std::string &message) {
    ledger.add(LedgerKind::Rejected, 0, "lot splitter: " + message);

    if (error != nullptr)
    {
      *error = message;
    }

    return false;
  };

  r = LotSplitPlan{};

  if (s.polygon.size() < 3 || !s.random || s.targetArea <= 0)
  {
    return fail("a split needs a polygon, a target area and a random source");
  }

  if (!s.edgeKinds.empty() && s.edgeKinds.size() != s.polygon.size())
  {
    return fail("one edge kind per polygon edge");
  }

  Piece root{s.polygon, s.edgeKinds.empty() ? std::vector<LotEdgeKind>(s.polygon.size(), LotEdgeKind::Road) : s.edgeKinds};

  if (area(root.ring) < 0)
  {
    std::reverse(root.ring.begin(), root.ring.end());
    std::reverse(root.kinds.begin(), root.kinds.end());
    // Edge i of the reversed ring is edge n-1-i of the original, shifted by one.
    std::rotate(root.kinds.begin(), root.kinds.begin() + 1, root.kinds.end());
  }

  Splitter splitter{s, r, 0};
  splitter.split(root, 0);
  ledger.add(LedgerKind::Repaired, 0, "lot splitter: " + std::to_string(r.lots.size()) + " lots from " + std::to_string(r.chords.size()) + " chords (" + std::to_string(r.refused) + " cuts refused)");
  return true;
}

bool LotSplitter::run(const LotSplitInput &input, LotSplitPlan &output, Ledger &ledger, std::string *error)
{
  return plan(input, output, ledger, error);
}

} // namespace procedural::world::generator
