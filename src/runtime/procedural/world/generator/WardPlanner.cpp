#include "WardPlanner.hpp"

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

ConvexRing clipTo(ConvexRing p, const ConvexRing &q)
{
  for (size_t i = 0; i < q.size() && !p.empty(); ++i)
  {
    const Position a = q[i], b = q[(i + 1) % q.size()];
    p = clipHalfPlane(p, b.z - a.z, a.x - b.x, (b.z - a.z) * a.x + (a.x - b.x) * a.z);
  }

  return p;
}

bool onLand(const std::vector<ConvexRing> &usable, Position p)
{
  for (const auto &piece : usable)
  {
    if (strictlyInsideConvex(piece, p) || onRing(piece, p, 1e-6))
    {
      return true;
    }
  }

  return false;
}

} // namespace

WardPlanner::WardPlanner(uint64_t domain) : Generator("city.ward-planner", domain, "city.ward-plan-input", "city.ward-plan", GenerationScope::Patch)
{
}

bool WardPlanner::run(const WardPlanInput &input, WardPlan &output, Ledger &ledger, std::string *error)
{
  return plan(input, output, ledger, error);
}

bool WardPlanner::plan(const WardPlanInput &s, WardPlan &r, Ledger &ledger, std::string *error)
{
  auto fail = [&](const char *message) {
    if (error != nullptr)
    {
      *error = message;
    }

    return false;
  };

  if (!s.random || !s.wardSeed || s.domain.size() < 3 || s.usable.empty() || s.siteSpacing < 5 || s.targetBlockArea < s.minimumBlockArea)
  {
    return fail("ward planner needs a domain, usable land, random callbacks and sane block sizes");
  }

  std::vector<ConvexRing> cells;

  if (s.recipe == WardRecipe::Organic)
  {
    // Poisson sites on the usable land, then a Voronoi partition by clipping.
    for (uint64_t attempt = 0; attempt < 8192 && r.sites.size() < s.maximumSites; ++attempt)
    {
      const Position p{s.minX + (s.maxX - s.minX) * s.random(attempt, 1), 0, s.minZ + (s.maxZ - s.minZ) * s.random(attempt, 2)};

      if (!onLand(s.usable, p))
      {
        continue;
      }

      bool spaced = true;

      for (auto q : r.sites)
      {
        if (distance(p, q) < s.siteSpacing)
        {
          spaced = false;
          break;
        }
      }

      if (spaced)
      {
        r.sites.push_back(p);
      }
    }

    if (r.sites.size() < 4)
    {
      // A hamlet's land is too small for a Voronoi of wards: one box, bisected.
      ledger.add(LedgerKind::Repaired, 0, "ward planner: fewer than four sites; the land is bisected as one ward");
      r.sites.clear();
      cells.clear();
      cells.push_back({{s.minX, 0, s.minZ}, {s.maxX, 0, s.minZ}, {s.maxX, 0, s.maxZ}, {s.minX, 0, s.maxZ}});
    }
    else
    {
      auto voronoi = [&]() {
        cells.clear();

        for (size_t i = 0; i < r.sites.size(); ++i)
        {
          ConvexRing p{{s.minX, 0, s.minZ}, {s.maxX, 0, s.minZ}, {s.maxX, 0, s.maxZ}, {s.minX, 0, s.maxZ}};
          const Position a = r.sites[i];

          for (size_t j = 0; j < r.sites.size(); ++j)
          {
            if (j != i)
            {
              const Position b = r.sites[j];
              p = clipHalfPlane(p, b.x - a.x, b.z - a.z, (b.x * b.x + b.z * b.z - a.x * a.x - a.z * a.z) * .5);
            }
          }

          cells.push_back(p);
        }
      };

      voronoi();

      // Lloyd relaxation: each site moves to the centroid of its cell clipped
      // to the usable land (Watabou relaxes its central patches three times).
      for (uint32_t iteration = 0; iteration < s.siteRelaxations; ++iteration)
      {
        for (size_t i = 0; i < r.sites.size(); ++i)
        {
          double ax = 0, az = 0, aw = 0;

          for (const auto &land : s.usable)
          {
            const ConvexRing piece = clipTo(cells[i], land);
            const double w = signedArea(piece);

            if (w <= 1e-9)
            {
              continue;
            }

            const Position c = centroid(piece);
            ax += c.x * w;
            az += c.z * w;
            aw += w;
          }

          if (aw > 1e-9 && onLand(s.usable, {ax / aw, 0, az / aw}))
          {
            r.sites[i] = {ax / aw, 0, az / aw};
          }
        }

        voronoi();
      }
    }
  }

  // Refine the coarse patches: the site budget controls neighbourhood
  // structure, the block target controls the walkable block scale.
  std::vector<ConvexRing> coarse = std::move(cells);
  cells.clear();
  bool planned = s.recipe == WardRecipe::Planned;

  std::function<void(ConvexRing, uint32_t, uint64_t, double)> split = [&](ConvexRing p, uint32_t depth, uint64_t id, double districtAxis) {
    if (cells.size() > 256)
    {
      return; // The caller reports exhaustion atomically.
    }

    // A planned ward inside an organic city is gridded to a third of the
    // organic ward size (Might of Merchants mixes square and irregular blocks).
    const double blockTarget = planned && s.recipe == WardRecipe::Organic ? s.targetBlockArea * .3 : s.targetBlockArea;
    const double pieceArea = signedArea(p);

    // Narrowest extent across any edge normal: a block deeper than two lot
    // rows keeps interior lots that no street reaches.
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

    const bool tooManySides = !planned && s.maximumSides > 0 && p.size() > s.maximumSides;
    const bool tooDeep = !planned && s.maximumBlockDepth > 0 && narrowest > s.maximumBlockDepth;
    const bool wanted = pieceArea > blockTarget || ((tooManySides || tooDeep) && pieceArea >= 2 * s.minimumBlockArea);

    if (!wanted || depth == s.maximumSplitDepth)
    {
      if (pieceArea > blockTarget)
      {
        ++r.unrefinedBlocks;
      }

      cells.push_back(p);
      return;
    }

    ++r.splitAttempts;

    auto hasWorkingDepth = [&](ConvexRing q) {
      // A useful block supports opposing frontage rows and a small interior
      // court; area alone accepts thin triangles that become mostly street.
      const ConvexRing boundary = q;

      for (size_t k = 0; k < boundary.size(); ++k)
      {
        const Position u = boundary[k], v = boundary[(k + 1) % boundary.size()];
        const double ex = v.z - u.z, ez = u.x - v.x;
        q = clipHalfPlane(q, ex, ez, ex * u.x + ez * u.z - s.frontageInset * distance(u, v));
      }

      return signedArea(q) >= s.minimumWidth * s.minimumWidth;
    };

    auto compactness = [](const ConvexRing &q) {
      double perimeter = 0;

      for (size_t k = 0; k < q.size(); ++k)
      {
        perimeter += distance(q[k], q[(k + 1) % q.size()]);
      }

      return perimeter > 1e-9 ? 4 * 3.14159265358979 * signedArea(q) / (perimeter * perimeter) : 0;
    };

    double nx = 0, nz = 0, c = 0;
    ConvexRing a, b;

    if (planned)
    {
      if (depth == 2)
      {
        districtAxis += (s.random(id, 91) - .5) * 2 * s.plannedDistrictTurnRadians;
      }

      const double ux = std::cos(districtAxis), uz = std::sin(districtAxis);
      double u0 = 1e30, u1 = -1e30, v0 = 1e30, v1 = -1e30;

      for (auto q : p)
      {
        const double u = q.x * ux + q.z * uz, v = -q.x * uz + q.z * ux;
        u0 = std::min(u0, u);
        u1 = std::max(u1, u);
        v0 = std::min(v0, v);
        v1 = std::max(v1, v);
      }

      const double theta = districtAxis + ((u1 - u0) / (v1 - v0) > s.plannedBlockAspect ? 0 : 1.5707963267948966);
      nx = std::cos(theta);
      nz = std::sin(theta);
      double lo = 1e30, hi = -1e30;

      for (auto v : p)
      {
        lo = std::min(lo, nx * v.x + nz * v.z);
        hi = std::max(hi, nx * v.x + nz * v.z);
      }

      c = lo + (hi - lo) * (.46 + s.random(id, 4) * .08);
      a = clipHalfPlane(p, nx, nz, c);
      b = clipHalfPlane(p, -nx, -nz, -c);

      if (signedArea(a) < s.minimumBlockArea || signedArea(b) < s.minimumBlockArea || !hasWorkingDepth(a) || !hasWorkingDepth(b))
      {
        ++r.unrefinedBlocks;
        cells.push_back(p);
        return;
      }
    }
    else if (s.maximumSides == 0 && s.maximumBlockDepth == 0)
    {
      // Legacy organic cut: across the long axis with a random tilt.
      double loX = 1e30, hiX = -1e30, loZ = 1e30, hiZ = -1e30;

      for (auto v : p)
      {
        loX = std::min(loX, v.x);
        hiX = std::max(hiX, v.x);
        loZ = std::min(loZ, v.z);
        hiZ = std::max(hiZ, v.z);
      }

      const double theta = (hiX - loX > hiZ - loZ ? 0 : 1.57079632679) + (s.random(id, 3) - .5) * .45;
      nx = std::cos(theta);
      nz = std::sin(theta);
      double lo = 1e30, hi = -1e30;

      for (auto v : p)
      {
        lo = std::min(lo, nx * v.x + nz * v.z);
        hi = std::max(hi, nx * v.x + nz * v.z);
      }

      c = lo + (hi - lo) * (.4 + s.random(id, 4) * .2);
      a = clipHalfPlane(p, nx, nz, c);
      b = clipHalfPlane(p, -nx, -nz, -c);

      if (signedArea(a) < s.minimumBlockArea || signedArea(b) < s.minimumBlockArea || !hasWorkingDepth(a) || !hasWorkingDepth(b))
      {
        ++r.unrefinedBlocks;
        cells.push_back(p);
        return;
      }
    }
    else
    {
      // Scored organic cut: every chord between non-adjacent corners and a few
      // cuts across the longest edges compete; the cut whose worse half is the
      // most compact and balanced wins, and halves with too many sides lose.
      struct Candidate
      {
        ConvexRing a, b;
        double score;
      };

      std::vector<Candidate> candidates;

      auto consider = [&](ConvexRing left, ConvexRing right) {
        const double areaLeft = signedArea(left), areaRight = signedArea(right);

        if (left.size() < 3 || right.size() < 3 || areaLeft < s.minimumBlockArea || areaRight < s.minimumBlockArea || !hasWorkingDepth(left) || !hasWorkingDepth(right))
        {
          return;
        }

        double score = std::min(compactness(left), compactness(right)) * std::sqrt(std::min(areaLeft, areaRight) / std::max(areaLeft, areaRight));

        for (size_t sides : {left.size(), right.size()})
        {
          if (s.maximumSides > 0 && sides > s.maximumSides)
          {
            score *= .8;
          }
        }

        candidates.push_back({std::move(left), std::move(right), score});
      };

      const size_t n = p.size();

      for (size_t i = 0; i < n; ++i)
      {
        for (size_t j = i + 2; j < n; ++j)
        {
          if (i == 0 && j == n - 1)
          {
            continue;
          }

          ConvexRing left(p.begin() + i, p.begin() + j + 1);
          ConvexRing right(p.begin() + j, p.end());
          right.insert(right.end(), p.begin(), p.begin() + i + 1);
          consider(left, right);
        }
      }

      std::vector<size_t> edges(n);

      for (size_t k = 0; k < n; ++k)
      {
        edges[k] = k;
      }

      std::sort(edges.begin(), edges.end(), [&](size_t x, size_t y) {
        return distance(p[x], p[(x + 1) % n]) > distance(p[y], p[(y + 1) % n]);
      });

      for (size_t e = 0; e < std::min<size_t>(3, n); ++e)
      {
        const Position u = p[edges[e]], v = p[(edges[e] + 1) % n];
        const double l = distance(u, v), tx = (v.x - u.x) / l, tz = (v.z - u.z) / l;

        for (double fraction : {.42 + s.random(id, 4) * .16, .5})
        {
          const double offset = tx * (u.x + (v.x - u.x) * fraction) + tz * (u.z + (v.z - u.z) * fraction);
          consider(clipHalfPlane(p, tx, tz, offset), clipHalfPlane(p, -tx, -tz, -offset));
        }
      }

      if (candidates.empty())
      {
        ++r.unrefinedBlocks;
        cells.push_back(p);
        return;
      }

      const auto best = std::max_element(candidates.begin(), candidates.end(), [](const Candidate &x, const Candidate &y) {
        return x.score < y.score;
      });
      a = best->a;
      b = best->b;
    }

    if (planned)
    {
      r.plannedCuts.push_back({nx, nz, c, depth});
    }

    split(a, depth + 1, id * 2, districtAxis);
    split(b, depth + 1, id * 2 + 1, districtAxis);
  };

  // Split before domain clipping, so holes and concave boundaries survive.
  if (s.recipe == WardRecipe::Recursive)
  {
    coarse.push_back({{s.minX, 0, s.minZ}, {s.maxX, 0, s.minZ}, {s.maxX, 0, s.maxZ}, {s.minX, 0, s.maxZ}});
  }

  if (s.recipe == WardRecipe::Planned)
  {
    const double ux = std::cos(s.plannedAxisRadians), uz = std::sin(s.plannedAxisRadians);
    double u0 = 1e30, u1 = -1e30, v0 = 1e30, v1 = -1e30;

    for (auto q : s.domain)
    {
      const double u = q.x * ux + q.z * uz, v = -q.x * uz + q.z * ux;
      u0 = std::min(u0, u);
      u1 = std::max(u1, u);
      v0 = std::min(v0, v);
      v1 = std::max(v1, v);
    }

    auto at = [&](double u, double v) {
      return Position{ux * u - uz * v, 0, uz * u + ux * v};
    };

    coarse.push_back({at(u0, v0), at(u1, v0), at(u1, v1), at(u0, v1)});
  }

  if (s.recipe == WardRecipe::Roads)
  {
    coarse = s.usable;
  }

  if (s.recipe == WardRecipe::Given)
  {
    coarse = s.givenCells;
  }

  for (size_t i = 0; i < coarse.size(); ++i)
  {
    planned = s.recipe == WardRecipe::Planned || (s.mixedBlockGrammar && s.recipe == WardRecipe::Organic && s.plannedAt && s.plannedAt(coarse[i]));
    r.anyPlanned |= planned;

    if (s.recipe == WardRecipe::Roads)
    {
      cells.push_back(coarse[i]);
      continue;
    }

    if (s.recipe == WardRecipe::Given)
    {
      // A given ward larger than the block target is cut like an organic one.
      split(coarse[i], 0, s.wardSeed(i + 1), s.plannedAxisRadians);
      continue;
    }

    const double axis = s.plannedAxisRadians + (planned ? (s.random(i + 1, 92) - .5) * 2 * s.plannedDistrictTurnRadians : 0);
    split(coarse[i], 0, s.recipe == WardRecipe::Organic ? s.wardSeed(i + 1) : 1, axis);
  }

  if (cells.size() > 256)
  {
    ledger.add(LedgerKind::Capacity, 0, "ward planner: district subdivision budget exceeded");
    return fail("district subdivision budget exceeded");
  }

  if (r.unrefinedBlocks > 0)
  {
    ledger.add(LedgerKind::Repaired, 0, "ward planner: " + std::to_string(r.unrefinedBlocks) + " oversized blocks kept for working depth or split budget");
  }

  r.cells = std::move(cells);
  return true;
}

} // namespace procedural::world::generator
