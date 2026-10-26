#include "LotPlanner.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace procedural::world::generator {

namespace {

double distance(Position a, Position b)
{
  return std::hypot(b.x - a.x, b.z - a.z);
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
      const double t = da / (da - db);
      q.push_back({a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t});
    }
  }

  return q;
}

// Polygon.shrinkEq: every edge moved inward by d.
ConvexRing shrink(ConvexRing p, double d)
{
  const ConvexRing boundary = p;

  for (size_t k = 0; k < boundary.size() && !p.empty(); ++k)
  {
    const Position u = boundary[k], v = boundary[(k + 1) % boundary.size()];
    const double l = distance(u, v);

    if (l < 1e-9)
    {
      continue;
    }

    const double nx = (v.z - u.z) / l, nz = (u.x - v.x) / l; // Outward for a CCW ring.
    p = clipHalfPlane(p, nx, nz, nx * u.x + nz * u.z - d);
  }

  return p;
}

// Cutter.bisect: the cut runs through the point at `ratio` along a straight
// run of the outline (Watabou's edge; our outlines follow curved streets, so
// nearly collinear edges are merged into one run first), perpendicular to
// that run turned by `angle`; the halves are pulled back by half the gap on
// either side of the cut (Polygon.cut with a gap peels both halves).
struct Run
{
  Position a, b;
  double length = 0;
};

std::vector<Run> straightRuns(const ConvexRing &poly)
{
  std::vector<Run> runs;
  const size_t n = poly.size();
  size_t start = 0;

  // Start at a real corner so a run never wraps across the seam.
  for (size_t k = 0; k < n; ++k)
  {
    const Position u = poly[(k + n - 1) % n], v = poly[k], w = poly[(k + 1) % n];
    const double ax = v.x - u.x, az = v.z - u.z, bx = w.x - v.x, bz = w.z - v.z;
    const double angle = std::acos(std::clamp((ax * bx + az * bz) / std::max(1e-9, std::hypot(ax, az) * std::hypot(bx, bz)), -1., 1.));

    if (angle > .26)
    {
      start = k;
      break;
    }
  }

  Run current{poly[start], poly[start], 0};

  for (size_t step = 0; step < n; ++step)
  {
    const size_t k = (start + step) % n;
    const Position v = poly[k], w = poly[(k + 1) % n];

    if (step > 0)
    {
      const Position u = poly[(k + n - 1) % n];
      const double ax = v.x - u.x, az = v.z - u.z, bx = w.x - v.x, bz = w.z - v.z;
      const double angle = std::acos(std::clamp((ax * bx + az * bz) / std::max(1e-9, std::hypot(ax, az) * std::hypot(bx, bz)), -1., 1.));

      if (angle > .26)
      {
        current.b = v;
        current.length = distance(current.a, current.b);
        runs.push_back(current);
        current = {v, v, 0};
      }
    }

    (void)w;
  }

  current.b = poly[start];
  current.length = distance(current.a, current.b);

  if (current.length > 1e-6)
  {
    runs.push_back(current);
  }

  return runs;
}

void bisectRun(const ConvexRing &poly, const Run &run, double ratio, double angle, double gap, ConvexRing &left, ConvexRing &right)
{
  const Position p1{run.a.x + (run.b.x - run.a.x) * ratio, 0, run.a.z + (run.b.z - run.a.z) * ratio};
  const double dx = run.b.x - run.a.x, dz = run.b.z - run.a.z;
  const double cosB = std::cos(angle), sinB = std::sin(angle);
  const double vx = dx * cosB - dz * sinB, vz = dz * cosB + dx * sinB;
  const double l = std::hypot(vx, vz);

  if (l < 1e-9)
  {
    left = poly;
    right.clear();
    return;
  }

  // The cut line is perpendicular to the turned run: its normal is the run.
  const double nx = vx / l, nz = vz / l;
  const double c = nx * p1.x + nz * p1.z;
  left = clipHalfPlane(poly, nx, nz, c - gap * .5);
  right = clipHalfPlane(poly, -nx, -nz, -(c + gap * .5));
}

// A half-plane clip of a concave ring joins its parts with zero-width
// bridges (repeated vertices); the parts are separated here.
std::vector<ConvexRing> splitBridges(const ConvexRing &ring)
{
  std::vector<ConvexRing> parts;
  ConvexRing current = ring;

  for (unsigned guard = 0; guard < 16 && current.size() >= 3; ++guard)
  {
    bool split = false;

    for (size_t i = 0; i < current.size() && !split; ++i)
    {
      for (size_t j = i + 2; j < current.size() && !split; ++j)
      {
        if (distance(current[i], current[j]) < 1e-6 && !(i == 0 && j + 1 == current.size()))
        {
          ConvexRing inner(current.begin() + i, current.begin() + j);
          ConvexRing outer(current.begin(), current.begin() + i);
          outer.insert(outer.end(), current.begin() + j, current.end());

          if (inner.size() >= 3 && std::abs(signedArea(inner)) > 1e-6)
          {
            parts.push_back(inner);
          }

          current = outer;
          split = true;
        }
      }
    }

    if (!split)
    {
      break;
    }
  }

  if (current.size() >= 3 && std::abs(signedArea(current)) > 1e-6)
  {
    parts.push_back(current);
  }

  // Drop degenerate spikes left by collinear bridge points.
  for (auto &part : parts)
  {
    ConvexRing cleaned;

    for (size_t k = 0; k < part.size(); ++k)
    {
      if (cleaned.empty() || distance(cleaned.back(), part[k]) > 1e-6)
      {
        cleaned.push_back(part[k]);
      }
    }

    while (cleaned.size() > 1 && distance(cleaned.front(), cleaned.back()) < 1e-6)
    {
      cleaned.pop_back();
    }

    part = cleaned;
  }

  return parts;
}

// Narrowest extent across any edge normal.
double narrowestWidth(const ConvexRing &p)
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

double sharpestAngle(const ConvexRing &p)
{
  double sharpest = 3.14159265358979;

  for (size_t k = 0; k < p.size(); ++k)
  {
    const Position u = p[(k + p.size() - 1) % p.size()], v = p[k], w = p[(k + 1) % p.size()];
    const double ax = u.x - v.x, az = u.z - v.z, bx = w.x - v.x, bz = w.z - v.z;
    const double angle = std::acos(std::clamp((ax * bx + az * bz) / std::max(1e-9, std::hypot(ax, az) * std::hypot(bx, bz)), -1., 1.));
    sharpest = std::min(sharpest, angle);
  }

  return sharpest;
}

// Trim every sharp tip: the lot is clipped across the corner's bisector where
// the lot is `width` wide, so no lot keeps a needle. The tip stays open land.
ConvexRing trimTips(ConvexRing p, double width, double minimumAngle)
{
  for (unsigned pass = 0; pass < 4; ++pass)
  {
    bool trimmed = false;

    for (size_t k = 0; k < p.size() && !trimmed; ++k)
    {
      const Position u = p[(k + p.size() - 1) % p.size()], v = p[k], w = p[(k + 1) % p.size()];
      const double al = distance(u, v), bl = distance(w, v);

      if (al < 1e-9 || bl < 1e-9)
      {
        continue;
      }

      const double ax = (u.x - v.x) / al, az = (u.z - v.z) / al, bx = (w.x - v.x) / bl, bz = (w.z - v.z) / bl;
      const double angle = std::acos(std::clamp(ax * bx + az * bz, -1., 1.));

      if (angle >= minimumAngle)
      {
        continue;
      }

      double dx = ax + bx, dz = az + bz;
      const double dl = std::hypot(dx, dz);

      if (dl < 1e-9)
      {
        continue;
      }

      dx /= dl;
      dz /= dl;
      const double reach = width / (2 * std::max(1e-3, std::sin(angle * .5)));
      // Keep the far side of the line perpendicular to the bisector at `reach`.
      p = clipHalfPlane(p, -dx, -dz, -(dx * (v.x + dx * reach) + dz * (v.z + dz * reach)));
      trimmed = true;
    }

    if (!trimmed)
    {
      break;
    }
  }

  return p;
}

} // namespace

LotPlanner::LotPlanner(uint64_t domain) : Generator("city.lot-planner", domain, "city.lot-plan-input", "city.lot-plan", GenerationScope::Patch)
{
}

bool LotPlanner::run(const LotPlanInput &input, LotPlan &output, Ledger &ledger, std::string *error)
{
  return plan(input, output, ledger, error);
}

bool LotPlanner::plan(const LotPlanInput &s, LotPlan &r, Ledger &ledger, std::string *error)
{
  if (s.block.size() < 3 || !s.random || s.minimumArea <= 0)
  {
    if (error != nullptr)
    {
      *error = "lot planner needs a block, a random callback and a positive minimum area";
    }

    return false;
  }

  // getCityBlock: the street clearance comes off every edge. Nearly collinear
  // vertices of the outline are dropped first (an arrangement face carries
  // many), so the longest edge and the axes are the real ones.
  ConvexRing simplified;

  for (size_t k = 0; k < s.block.size(); ++k)
  {
    const Position u = s.block[(k + s.block.size() - 1) % s.block.size()], v = s.block[k], w = s.block[(k + 1) % s.block.size()];
    const double al = distance(u, v), bl = distance(w, v);

    if (al < 1e-6)
    {
      continue;
    }

    const double deviation = bl > 1e-9 ? std::abs((v.x - u.x) * (w.z - v.z) - (v.z - u.z) * (w.x - v.x)) / bl : 0;

    if (deviation > .15 || bl < 1e-6)
    {
      simplified.push_back(v);
    }
  }

  const ConvexRing &outline = simplified.size() >= 3 ? simplified : s.block;
  ConvexRing block = s.inset > 0 ? shrink(outline, s.inset) : outline;

  if (block.size() < 3 || signedArea(block) < s.minimumArea * .5)
  {
    ledger.add(LedgerKind::Omitted, 0, "lot planner: the block is too small after its street clearance");
    return true;
  }

  uint64_t draws = 0;

  auto random = [&]() {
    return s.random(s.key, 200 + draws++);
  };

  const double minimumWidth = std::max(2.2, s.alleyWidth * 1.8);
  const double minimumAngle = .5; // About thirty degrees: sharper corners are trimmed.

  auto sliver = [&](const ConvexRing &half) {
    return half.size() < 3 || narrowestWidth(half) < minimumWidth || sharpestAngle(half) < minimumAngle;
  };

  auto finish = [&](ConvexRing lot) {
    if (!s.orthogonal)
    {
      lot = trimTips(std::move(lot), minimumWidth, minimumAngle);
    }

    if (lot.size() < 3 || (!s.orthogonal && (signedArea(lot) < s.minimumArea * .3 || narrowestWidth(lot) < minimumWidth)))
    {
      if (lot.size() >= 3)
      {
        r.open.push_back(lot);
      }

      return;
    }

    if (random() < s.emptyProbability)
    {
      r.open.push_back(lot);
    }
    else
    {
      r.lots.push_back(lot);
    }
  };

  // Ward.createAlleys.
  std::function<void(ConvexRing, bool, uint32_t)> createAlleys = [&](ConvexRing p, bool split, uint32_t depth) {
    // A wedge is never fanned: an acute corner is cut off first, across its
    // bisector where the piece is one lot wide, and the tip is its own lot.
    if (sharpestAngle(p) < .7 && signedArea(p) > s.minimumArea * 2)
    {
      size_t sharp = 0;
      double sharpest = 1e30;

      for (size_t k = 0; k < p.size(); ++k)
      {
        const Position u = p[(k + p.size() - 1) % p.size()], v = p[k], w = p[(k + 1) % p.size()];
        const double al = distance(u, v), bl = distance(w, v);

        if (al < 1e-9 || bl < 1e-9)
        {
          continue;
        }

        const double angle = std::acos(std::clamp(((u.x - v.x) * (w.x - v.x) + (u.z - v.z) * (w.z - v.z)) / (al * bl), -1., 1.));

        if (angle < sharpest)
        {
          sharpest = angle;
          sharp = k;
        }
      }

      const Position u = p[(sharp + p.size() - 1) % p.size()], v = p[sharp], w = p[(sharp + 1) % p.size()];
      const double al = distance(u, v), bl = distance(w, v);
      double dx = (u.x - v.x) / al + (w.x - v.x) / bl, dz = (u.z - v.z) / al + (w.z - v.z) / bl;
      const double dl = std::hypot(dx, dz);

      if (dl > 1e-9)
      {
        dx /= dl;
        dz /= dl;
        const double width = std::max(minimumWidth * 1.6, std::sqrt(s.minimumArea));
        const double reach = width / (2 * std::max(1e-3, std::sin(sharpest * .5)));
        const double c = dx * (v.x + dx * reach) + dz * (v.z + dz * reach);
        const ConvexRing tip = clipHalfPlane(p, dx, dz, c), rest = clipHalfPlane(p, -dx, -dz, -c);

        if (tip.size() >= 3 && rest.size() >= 3 && signedArea(rest) > s.minimumArea)
        {
          ++r.cuts;
          finish(tip);
          createAlleys(rest, split, depth + 1);
          return;
        }
      }
    }

    auto runs = straightRuns(p);

    if (runs.empty())
    {
      finish(p);
      return;
    }

    std::sort(runs.begin(), runs.end(), [](const Run &x, const Run &y) {
      return x.length > y.length;
    });

    const double spread = .8 * s.gridChaos;
    const double ratio = (1 - spread) * .5 + random() * spread;
    // Buildings stay rectangular even in chaotic wards: no angle on small pieces.
    // No angle on small pieces, and none inside a wedge: angled cuts converge
    // at the wedge's apex and fan the lots.
    const double angleSpread = 3.14159265358979 / 6 * s.gridChaos * (signedArea(p) < s.minimumArea * 4 || sharpestAngle(p) < 1. ? 0. : 1.);
    const double angle = (random() - .5) * angleSpread;

    // The longest run is cut, as Watabou does; a cut that leaves a needle or
    // a strip narrower than a house tries the next runs, then a plain
    // perpendicular cut, and a small piece that cannot be cut cleanly stays
    // whole.
    ConvexRing halves[2];
    bool clean = false;

    for (size_t attempt = 0; attempt < std::min<size_t>(3, runs.size()) && !clean; ++attempt)
    {
      bisectRun(p, runs[attempt], ratio, attempt == 0 ? angle : 0., split ? s.alleyWidth : 0., halves[0], halves[1]);
      clean = !sliver(halves[0]) && !sliver(halves[1]);
    }

    if (!clean)
    {
      bisectRun(p, runs[0], .5, 0., split ? s.alleyWidth : 0., halves[0], halves[1]);
      clean = !sliver(halves[0]) && !sliver(halves[1]);
    }

    if (!clean && signedArea(p) < s.minimumArea * 3)
    {
      finish(p);
      return;
    }

    ++r.cuts;
    r.alleys += split;

    for (auto &half : halves)
    {
      if (half.size() < 3)
      {
        continue;
      }

      const double area = signedArea(half);

      if (area < 1e-6)
      {
        continue;
      }

      if (area < s.minimumArea * std::pow(2., 4 * s.sizeChaos * (random() - .5)) || depth + 1 >= s.maximumDepth)
      {
        finish(half);
      }
      else
      {
        const double u = std::max(1e-6, random()), v = std::max(1e-6, random());
        createAlleys(half, area > s.minimumArea / (u * v), depth + 1);
      }
    }
  };

  // Ward.createOrthoBuilding: the axes are the block's longest edge and its
  // perpendicular; each piece is cut through a point at 0.4..0.6 of its
  // longest edge along whichever axis is closer to perpendicular to that
  // edge. A cut that would leave a needle or a strip narrower than a house
  // tries the other axis, and if that fails too the piece stays one lot: no
  // land is thrown away and no lot is a fan.
  double c1x = 1, c1z = 0;

  if (s.axisGiven)
  {
    c1x = std::cos(s.axisRadians);
    c1z = std::sin(s.axisRadians);
  }
  else
  {
    double longest = -1;

    for (size_t k = 0; k < block.size(); ++k)
    {
      const double len = distance(block[k], block[(k + 1) % block.size()]);

      if (len > longest)
      {
        longest = len;
        c1x = (block[(k + 1) % block.size()].x - block[k].x) / len;
        c1z = (block[(k + 1) % block.size()].z - block[k].z) / len;
      }
    }
  }

  const double c2x = -c1z, c2z = c1x;
  std::function<void(ConvexRing, bool, uint32_t)> createOrtho = [&](ConvexRing p, bool split, uint32_t depth) {
    // An acute corner is cut off first, across its bisector where the piece
    // is one house wide; the tip is its own lot and the rest has no needle.
    if (sharpestAngle(p) < .7 && signedArea(p) > s.minimumArea * 1.5)
    {
      size_t sharp = 0;
      double sharpest = 1e30;

      for (size_t k = 0; k < p.size(); ++k)
      {
        const Position u = p[(k + p.size() - 1) % p.size()], v = p[k], w = p[(k + 1) % p.size()];
        const double al = distance(u, v), bl = distance(w, v);

        if (al < 1e-9 || bl < 1e-9)
        {
          continue;
        }

        const double angle = std::acos(std::clamp(((u.x - v.x) * (w.x - v.x) + (u.z - v.z) * (w.z - v.z)) / (al * bl), -1., 1.));

        if (angle < sharpest)
        {
          sharpest = angle;
          sharp = k;
        }
      }

      const Position u = p[(sharp + p.size() - 1) % p.size()], v = p[sharp], w = p[(sharp + 1) % p.size()];
      const double al = distance(u, v), bl = distance(w, v);
      double dx = (u.x - v.x) / al + (w.x - v.x) / bl, dz = (u.z - v.z) / al + (w.z - v.z) / bl;
      const double dl = std::hypot(dx, dz);

      if (dl > 1e-9)
      {
        dx /= dl;
        dz /= dl;
        const double width = std::max(minimumWidth * 1.6, std::sqrt(s.minimumArea));
        const double reach = width / (2 * std::max(1e-3, std::sin(sharpest * .5)));
        const double c = dx * (v.x + dx * reach) + dz * (v.z + dz * reach);
        // A concave outline may fall into several parts on either side.
        const auto tips = splitBridges(clipHalfPlane(p, dx, dz, c));
        auto rests = splitBridges(clipHalfPlane(p, -dx, -dz, -c));
        double restArea = 0;

        for (const auto &rest : rests)
        {
          restArea += signedArea(rest);
        }

        if (!tips.empty() && !rests.empty() && restArea > s.minimumArea * .8)
        {
          ++r.cuts;

          for (const auto &tip : tips)
          {
            finish(tip);
          }

          for (auto &rest : rests)
          {
            createOrtho(rest, split, depth + 1);
          }

          return;
        }
      }
    }

    const double area = signedArea(p);
    // Random.normal() * 2 - 1, approximated by three draws.
    const double normal = (random() + random() + random()) / 3 * 2 - 1;

    if (area < s.minimumArea * std::pow(2., normal) || depth >= s.maximumDepth)
    {
      finish(p);
      return;
    }

    size_t vertex = 0;
    double longest = -1;

    for (size_t k = 0; k < p.size(); ++k)
    {
      const double len = distance(p[k], p[(k + 1) % p.size()]);

      if (len > longest)
      {
        longest = len;
        vertex = k;
      }
    }

    const Position a = p[vertex], b = p[(vertex + 1) % p.size()];
    const double ratio = .4 + random() * .2;
    const Position p1{a.x + (b.x - a.x) * ratio, 0, a.z + (b.z - a.z) * ratio};
    const double ex = (b.x - a.x) / longest, ez = (b.z - a.z) / longest;
    // The axis closer to perpendicular to the edge cuts across it.
    const bool firstAxis = std::abs(c1x * ex + c1z * ez) < std::abs(c2x * ex + c2z * ez);
    const double gap = split ? s.alleyWidth : 0.;
    std::vector<ConvexRing> parts;
    bool clean = false;

    for (int attempt = 0; attempt < 2 && !clean; ++attempt)
    {
      const bool useFirst = attempt == 0 ? firstAxis : !firstAxis;
      // The cut runs along the axis; its normal is the other axis. A concave
      // piece may fall into several parts on either side.
      const double nx = useFirst ? c2x : c1x, nz = useFirst ? c2z : c1z;
      const double c = nx * p1.x + nz * p1.z;
      parts = splitBridges(clipHalfPlane(p, nx, nz, c - gap * .5));
      const auto other = splitBridges(clipHalfPlane(p, -nx, -nz, -(c + gap * .5)));
      parts.insert(parts.end(), other.begin(), other.end());
      clean = parts.size() >= 2;

      for (const auto &part : parts)
      {
        clean &= !sliver(part);
      }
    }

    if (!clean)
    {
      // Keep the cut when it has a good part and the piece is large: the
      // sliver parts (bridges, spikes of a concave outline) stay open. A small
      // piece that cannot be cut cleanly stays one lot.
      size_t good = 0;

      for (const auto &part : parts)
      {
        good += !sliver(part);
      }

      if (good == 0 || area < s.minimumArea * 3)
      {
        finish(p);
        return;
      }

      for (auto &part : parts)
      {
        if (sliver(part) && part.size() >= 3)
        {
          r.open.push_back(part);
        }
      }

      parts.erase(
          std::remove_if(
              parts.begin(),
              parts.end(),
              [&](const ConvexRing &part) {
                return sliver(part);
              }),
          parts.end());
    }

    ++r.cuts;
    r.alleys += split;

    for (auto &part : parts)
    {
      if (part.size() >= 3 && signedArea(part) > 1e-6)
      {
        const double u = std::max(1e-6, random()), v = std::max(1e-6, random());
        createOrtho(part, signedArea(part) > s.minimumArea / (u * v), depth + 1);
      }
    }
  };

  if (s.orthogonal)
  {
    createOrtho(block, true, 0);
  }
  else
  {
    createAlleys(block, true, 0);
  }

  if (r.lots.empty())
  {
    ledger.add(LedgerKind::Omitted, 0, "lot planner: no lot came out of the block");
  }

  return true;
}

} // namespace procedural::world::generator
