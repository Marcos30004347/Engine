#include "RingLotPlanner.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world::generator {

namespace {

double distance(Position a, Position b)
{
  return std::hypot(b.x - a.x, b.z - a.z);
}

Position mix(Position a, Position b, double t)
{
  return {a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t};
}

// Keep the half-plane nx x + nz z <= c of a convex ring.
ConvexRing clipHalfPlane(const ConvexRing &p, double nx, double nz, double c)
{
  ConvexRing q;

  for (size_t i = 0; i < p.size(); ++i)
  {
    const Position a = p[i], b = p[(i + 1) % p.size()];
    const double da = nx * a.x + nz * a.z - c, db = nx * b.x + nz * b.z - c;

    if (da <= 0)
    {
      q.push_back(a);
    }

    if ((da < 0) != (db < 0))
    {
      q.push_back(mix(a, b, da / (da - db)));
    }
  }

  return q;
}

bool convexRing(const ConvexRing &p)
{
  if (p.size() < 3 || signedArea(p) <= 1e-6)
  {
    return false;
  }

  for (size_t i = 0; i < p.size(); ++i)
  {
    if (cross(p[i], p[(i + 1) % p.size()], p[(i + 2) % p.size()]) < -1e-6)
    {
      return false;
    }
  }

  return true;
}

// Drop repeated and collinear vertices.
ConvexRing tidy(const ConvexRing &p)
{
  ConvexRing q;

  for (size_t i = 0; i < p.size(); ++i)
  {
    if (q.empty() || distance(q.back(), p[i]) > 1e-6)
    {
      q.push_back(p[i]);
    }
  }

  while (q.size() > 1 && distance(q.front(), q.back()) < 1e-6)
  {
    q.pop_back();
  }

  ConvexRing r;

  for (size_t i = 0; i < q.size(); ++i)
  {
    const Position a = q[(i + q.size() - 1) % q.size()], b = q[i], c = q[(i + 1) % q.size()];

    if (std::abs(cross(a, b, c)) > 1e-3 * std::max(1., distance(a, c)))
    {
      r.push_back(b);
    }
  }

  return r;
}

// Arrangement outlines carry very short edges and near-collinear vertices;
// a ring needs edges long enough for a lot. The least important vertex (the
// smallest triangle with its neighbours) goes while it is unimportant or an
// adjacent edge is shorter than the minimum; a convex ring stays convex.
ConvexRing simplify(ConvexRing p, double minimumEdge)
{
  const double total = std::abs(signedArea(p));

  while (p.size() > 3)
  {
    size_t worst = p.size();
    double least = 1e30;

    for (size_t i = 0; i < p.size(); ++i)
    {
      const Position a = p[(i + p.size() - 1) % p.size()], b = p[i], c = p[(i + 1) % p.size()];
      const double importance = std::abs(cross(a, b, c)) * .5;
      const bool shortEdge = distance(a, b) < minimumEdge || distance(b, c) < minimumEdge;

      if ((importance < total * .004 || shortEdge) && importance < least)
      {
        least = importance;
        worst = i;
      }
    }

    if (worst == p.size())
    {
      break;
    }

    p.erase(p.begin() + worst);
  }

  return p;
}

// The inner copy: every outer vertex maps to one miter point, or to two
// chamfer points when the corner is sharp. inner[i] lists them in ring order.
struct InnerCopy
{
  std::vector<std::vector<Position>> inner;
  ConvexRing ring;
  uint32_t chamfers = 0;
};

bool innerCopy(const ConvexRing &block, double depth, double maximumMiter, InnerCopy &copy)
{
  const size_t n = block.size();
  copy = InnerCopy{};
  copy.inner.assign(n, {});

  std::vector<double> ex(n), ez(n), nx(n), nz(n), el(n);

  for (size_t i = 0; i < n; ++i)
  {
    const Position a = block[i], b = block[(i + 1) % n];
    el[i] = distance(a, b);

    if (el[i] < 1e-6)
    {
      return false;
    }

    ex[i] = (b.x - a.x) / el[i];
    ez[i] = (b.z - a.z) / el[i];
    nx[i] = -ez[i]; // Inward for a CCW ring.
    nz[i] = ex[i];
  }

  for (size_t i = 0; i < n; ++i)
  {
    const size_t h = (i + n - 1) % n;
    const Position v = block[i];
    // Miter: intersection of the two inward offset lines.
    const double sx = nx[h] + nx[i], sz = nz[h] + nz[i];
    const double d = 1 + nx[h] * nx[i] + nz[h] * nz[i]; // |s|^2 / 2
    const double miter = d > 1e-9 ? depth / d : 1e30;
    const Position m{v.x + sx * miter, 0, v.z + sz * miter};

    if (std::hypot(sx * miter, sz * miter) <= maximumMiter * depth)
    {
      copy.inner[i].push_back(m);
      continue;
    }

    // Chamfer: the point on each offset line at the miter limit from the
    // corner, never past the middle of either edge.
    const double s = std::min({std::sqrt(std::max(0., maximumMiter * maximumMiter - 1)) * depth, el[h] * .45, el[i] * .45});
    copy.inner[i].push_back({v.x + nx[h] * depth - ex[h] * s, 0, v.z + nz[h] * depth - ez[h] * s});
    copy.inner[i].push_back({v.x + nx[i] * depth + ex[i] * s, 0, v.z + nz[i] * depth + ez[i] * s});
    ++copy.chamfers;
  }

  for (size_t i = 0; i < n; ++i)
  {
    for (const auto &p : copy.inner[i])
    {
      copy.ring.push_back(p);
    }
  }

  copy.ring = tidy(copy.ring);

  if (!convexRing(copy.ring) || signedArea(copy.ring) < 4)
  {
    return false;
  }

  // Every inner point stays clear of every outer edge by nearly the depth,
  // and every inner edge keeps a positive length along its outer edge.
  for (size_t i = 0; i < n; ++i)
  {
    for (const auto &p : copy.inner[i])
    {
      for (size_t k = 0; k < n; ++k)
      {
        if (segmentDistance(p, block[k], block[(k + 1) % n]) < depth * .85)
        {
          return false;
        }
      }
    }

    const Position a = copy.inner[i].back(), b = copy.inner[(i + 1) % n].front();

    if ((b.x - a.x) * ex[i] + (b.z - a.z) * ez[i] < .5)
    {
      return false;
    }
  }

  return true;
}

// Convex bisection along the longer of the block's axis and its normal.
void cutInterior(const ConvexRing &court, double axis, double targetArea, std::vector<ConvexRing> &lots, uint32_t depth)
{
  const double a = signedArea(court);

  if (court.size() < 3 || a <= 1e-6)
  {
    return;
  }

  if (a <= targetArea * 1.5 || depth > 10)
  {
    lots.push_back(court);
    return;
  }

  double best = -1, bx = 0, bz = 0, blo = 0, bhi = 0;

  for (double turn : {0., 1.5707963267948966})
  {
    const double nx = std::cos(axis + turn), nz = std::sin(axis + turn);
    double lo = 1e30, hi = -1e30;

    for (const auto &p : court)
    {
      lo = std::min(lo, nx * p.x + nz * p.z);
      hi = std::max(hi, nx * p.x + nz * p.z);
    }

    if (hi - lo > best)
    {
      best = hi - lo;
      bx = nx;
      bz = nz;
      blo = lo;
      bhi = hi;
    }
  }

  const double c = (blo + bhi) * .5;
  const ConvexRing left = tidy(clipHalfPlane(court, bx, bz, c)), right = tidy(clipHalfPlane(court, -bx, -bz, -c));

  if (left.size() < 3 || right.size() < 3)
  {
    lots.push_back(court);
    return;
  }

  cutInterior(left, axis, targetArea, lots, depth + 1);
  cutInterior(right, axis, targetArea, lots, depth + 1);
}

// Only pieces that hold a small house are lots: no triangles, nothing
// under 30 m2, narrower than 3.5 m or sharper than 30 degrees.
bool holdsHouse(const ConvexRing &lot)
{
  if (lot.size() < 4 || signedArea(lot) < 30)
  {
    return false;
  }

  double narrowest = 1e30, sharpest = 3.14159265358979;

  for (size_t k = 0; k < lot.size(); ++k)
  {
    const Position u = lot[k], v = lot[(k + 1) % lot.size()], w = lot[(k + 2) % lot.size()];
    const double l = distance(u, v);

    if (l > 1e-9)
    {
      const double ex = (v.z - u.z) / l, ez = (u.x - v.x) / l;
      double lo = 1e30, hi = -1e30;

      for (auto q : lot)
      {
        lo = std::min(lo, ex * q.x + ez * q.z);
        hi = std::max(hi, ex * q.x + ez * q.z);
      }

      narrowest = std::min(narrowest, hi - lo);
    }

    const double ax = u.x - v.x, az = u.z - v.z, bx = w.x - v.x, bz = w.z - v.z;
    const double d = std::max(1e-9, std::hypot(ax, az) * std::hypot(bx, bz));
    sharpest = std::min(sharpest, std::acos(std::clamp((ax * bx + az * bz) / d, -1., 1.)));
  }

  return narrowest >= 3.5 && sharpest >= .52;
}

struct Ringer
{
  const RingLotInput &s;
  RingLotPlan &r;

  bool ring(const ConvexRing &block, const std::vector<bool> &fronted, uint32_t splits, uint64_t key)
  {
    const size_t n = block.size();
    InnerCopy copy;
    double depth = s.depth;

    while (depth >= s.minimumDepth && !innerCopy(block, depth, s.maximumMiter, copy))
    {
      depth *= .8;
    }

    if (depth < s.minimumDepth)
    {
      return false;
    }

    r.depthUsed = std::max(r.depthUsed, depth);
    r.chamfers += copy.chamfers;

    // A block with a large court splits through its middle first (an alley
    // between two rings): the poorer the block, the smaller the court it keeps.
    if (s.divideCourtAbove > 0 && splits < s.maximumSplits && signedArea(copy.ring) > s.divideCourtAbove)
    {
      double best = -1, bx = 0, bz = 0, blo = 0, bhi = 0;

      for (double turn : {0., 1.5707963267948966})
      {
        const double nx = std::cos(s.axisRadians + turn), nz = std::sin(s.axisRadians + turn);
        double lo = 1e30, hi = -1e30;

        for (const auto &p : block)
        {
          lo = std::min(lo, nx * p.x + nz * p.z);
          hi = std::max(hi, nx * p.x + nz * p.z);
        }

        if (hi - lo > best)
        {
          best = hi - lo;
          bx = nx;
          bz = nz;
          blo = lo;
          bhi = hi;
        }
      }

      const double c = blo + (bhi - blo) * (.45 + .1 * (s.random ? s.random(key, 71) : .5));
      const ConvexRing left = tidy(clipHalfPlane(block, bx, bz, c - s.alleyWidth * .5)), right = tidy(clipHalfPlane(block, -bx, -bz, -(c + s.alleyWidth * .5)));

      if (left.size() >= 3 && right.size() >= 3 && signedArea(left) > s.targetArea * 2 && signedArea(right) > s.targetArea * 2)
      {
        // Edges on the alley are fronted; edges inherited from the block keep their flag.
        auto flags = [&](const ConvexRing &half) {
          std::vector<bool> f(half.size(), true);

          for (size_t i = 0; i < half.size(); ++i)
          {
            const Position a = half[i], b = half[(i + 1) % half.size()];
            const Position m = mix(a, b, .5);

            for (size_t k = 0; k < n; ++k)
            {
              if (segmentDistance(m, block[k], block[(k + 1) % n]) < 1e-3)
              {
                f[i] = fronted.empty() || fronted[k];
              }
            }
          }

          return f;
        };
        const RingLotPlan saved = r;
        ++r.splits;

        if (ring(left, flags(left), splits + 1, key * 2 + 1) && ring(right, flags(right), splits + 1, key * 2 + 2))
        {
          return true;
        }

        r = saved; // Both halves or neither: the whole block is rung instead.
      }
    }

    // One strip per edge, cut at equal fractions of both edges.
    std::vector<ConvexRing> lots, open;
    std::vector<bool> lotFronted;

    for (size_t i = 0; i < n; ++i)
    {
      const Position a = block[i], b = block[(i + 1) % n];
      const Position ia = copy.inner[i].back(), ib = copy.inner[(i + 1) % n].front();
      const bool front = fronted.empty() || fronted[i];
      const ConvexRing strip{a, b, ib, ia};
      const double stripArea = signedArea(strip);
      // Cuts perpendicular to the street between the projections of the inner
      // ends, so the lots between the corners are equal rectangles and the
      // corner lots take the corners.
      const double el = distance(a, b), ex = (b.x - a.x) / el, ez = (b.z - a.z) / el;
      const double sa = (ia.x - a.x) * ex + (ia.z - a.z) * ez, sb = (ib.x - a.x) * ex + (ib.z - a.z) * ez;
      const double width = std::max(3., s.targetArea / depth);
      const unsigned count = sb - sa > 1 ? std::max(1u, unsigned(std::lround((sb - sa) / width))) : 1u;
      auto outerAt = [&](double v) {
        return Position{a.x + ex * v, 0, a.z + ez * v};
      };
      auto innerAt = [&](double v) {
        return mix(ia, ib, (v - sa) / (sb - sa));
      };

      for (unsigned j = 0; j < count; ++j)
      {
        const double s0 = sa + (sb - sa) * j / count, s1 = sa + (sb - sa) * (j + 1) / count;
        ConvexRing lot{j == 0 ? a : outerAt(s0), j + 1 == count ? b : outerAt(s1), j + 1 == count ? ib : innerAt(s1), j == 0 ? ia : innerAt(s0)};

        // The first lot of an edge after a chamfered corner takes the corner wedge.
        if (j == 0 && copy.inner[i].size() == 2)
        {
          lot.push_back(copy.inner[i].front());
        }

        lot = tidy(lot);

        if (lot.size() < 3 || signedArea(lot) < 1)
        {
          continue;
        }

        // A tiny single lot on a short edge joins the previous lot.
        if (count == 1 && stripArea < s.targetArea * .45 && !lots.empty() && lotFronted.back() == front)
        {
          ConvexRing &previous = lots.back();
          // previous = (..., a, ia); replace the closing edge with (a, b, ib, ia).
          size_t at = 0;

          for (size_t k = 0; k < previous.size(); ++k)
          {
            if (distance(previous[k], a) < 1e-6)
            {
              at = k;
            }
          }

          ConvexRing merged;

          for (size_t k = 0; k <= at; ++k)
          {
            merged.push_back(previous[k]);
          }

          for (size_t k = 1; k + 1 < lot.size(); ++k)
          {
            merged.push_back(lot[k]);
          }

          for (size_t k = at + 1; k < previous.size(); ++k)
          {
            merged.push_back(previous[k]);
          }

          previous = tidy(merged);
          continue;
        }

        lots.push_back(lot);
        lotFronted.push_back(front);
      }
    }

    for (size_t k = 0; k < lots.size(); ++k)
    {
      (lotFronted[k] && holdsHouse(lots[k]) ? r.lots : r.open).push_back(lots[k]);
    }

    if (s.fillInterior || (s.fillCourtBelow > 0 && signedArea(copy.ring) < s.fillCourtBelow))
    {
      std::vector<ConvexRing> interior;
      cutInterior(copy.ring, s.axisRadians, s.targetArea, interior, 0);

      for (const auto &lot : interior)
      {
        (holdsHouse(lot) ? r.lots : r.open).push_back(lot);
      }
    }
    else
    {
      r.courts.push_back(copy.ring);
    }

    return true;
  }
};

} // namespace

RingLotPlanner::RingLotPlanner(uint64_t domain) : Generator("city.ring-lot-planner", domain, "city.ring-lot-input", "city.ring-lot-plan", GenerationScope::Patch)
{
}

bool RingLotPlanner::plan(const RingLotInput &input, RingLotPlan &output, Ledger &ledger, std::string *error)
{
  auto fail = [&](const std::string &message) {
    ledger.add(LedgerKind::Rejected, 0, "ring lots: " + message);

    if (error != nullptr)
    {
      *error = message;
    }

    return false;
  };

  output = RingLotPlan{};
  ConvexRing block = tidy(input.block);

  if (block.size() < 3 || input.depth <= 0 || input.targetArea <= 0)
  {
    return fail("a ring needs a block, a depth and a target area");
  }

  if (signedArea(block) < 0)
  {
    std::reverse(block.begin(), block.end());
  }

  if (!convexRing(block))
  {
    return fail("the block is not convex");
  }

  block = simplify(block, std::max(2., input.depth * .6));

  if (block.size() < 3)
  {
    return fail("the block vanished in simplification");
  }

  if (!input.fronted.empty() && input.fronted.size() != input.block.size())
  {
    return fail("one frontage flag per edge");
  }

  // The flags follow the tidied ring: an edge keeps the flag of the outer edge it lies on.
  std::vector<bool> fronted;

  if (!input.fronted.empty())
  {
    fronted.assign(block.size(), true);

    for (size_t i = 0; i < block.size(); ++i)
    {
      const Position m = mix(block[i], block[(i + 1) % block.size()], .5);
      double best = 1e30;

      for (size_t k = 0; k < input.block.size(); ++k)
      {
        const double d = segmentDistance(m, input.block[k], input.block[(k + 1) % input.block.size()]);

        if (d < best)
        {
          best = d;
          fronted[i] = input.fronted[k];
        }
      }
    }
  }

  Ringer ringer{input, output};

  if (!ringer.ring(block, fronted, 0, input.key))
  {
    output = RingLotPlan{};

    if (!input.solidFallback)
    {
      return fail("the block is too narrow for a ring at the minimum depth");
    }

    // A sliver: convex bisection along the block's axes into lots of the
    // target area, every lot a convex piece of the block.
    std::vector<ConvexRing> whole;
    cutInterior(block, input.axisRadians, input.targetArea, whole, 0);

    for (const auto &lot : whole)
    {
      (holdsHouse(lot) ? output.lots : output.open).push_back(lot);
    }
    ledger.add(LedgerKind::Repaired, 0, "ring lots: too narrow for a ring, cut whole into " + std::to_string(output.lots.size()) + " lots");
    return true;
  }

  ledger.add(
      LedgerKind::Repaired,
      0,
      "ring lots: " + std::to_string(output.lots.size()) + " lots, " + std::to_string(output.open.size()) + " open, " + std::to_string(output.courts.size()) + " courts, " + std::to_string(output.splits) + " splits");
  return true;
}

bool RingLotPlanner::run(const RingLotInput &input, RingLotPlan &output, Ledger &ledger, std::string *error)
{
  return plan(input, output, ledger, error);
}

} // namespace procedural::world::generator
