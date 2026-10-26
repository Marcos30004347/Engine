#include "CurtainWall.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace procedural::world::generator {

namespace {

bool convexAfter(const ConvexRing &poly, Position from, Position to)
{
  ConvexRing q = poly;

  for (auto &p : q)
  {
    if (length(p, from) < 1e-6)
    {
      p = to;
    }
  }

  for (size_t k = 0; k < q.size(); ++k)
  {
    if (cross(q[k], q[(k + 1) % q.size()], q[(k + 2) % q.size()]) < 1e-7)
    {
      return false;
    }
  }

  return true;
}

} // namespace

CurtainWall::CurtainWall(uint64_t domain) : Generator("city.curtain-wall", domain, "city.curtain-wall-input", "city.curtain-wall", GenerationScope::Patch)
{
}

bool CurtainWall::run(const CurtainWallInput &input, CurtainWallPlan &output, Ledger &ledger, std::string *error)
{
  return plan(input, output, ledger, error);
}

void CurtainWall::remap(const CurtainWallPlan &plan, ConvexRing &polygon)
{
  for (auto &p : polygon)
  {
    for (const auto &[from, to] : plan.moves)
    {
      if (length(p, from) < 1e-6)
      {
        p = to;
        break;
      }
    }
  }
}

bool CurtainWall::plan(const CurtainWallInput &input, CurtainWallPlan &output, Ledger &ledger, std::string *error)
{
  if (input.outline.size() < 3 || input.sectionMetres <= 0)
  {
    if (error != nullptr)
    {
      *error = "curtain wall needs a closed outline and a positive section length";
    }

    return false;
  }

  ConvexRing outline = input.outline;
  const size_t n = outline.size();
  output.smoothing = std::min(1., 40. / double(std::max<size_t>(1, input.coreCells)));
  std::vector<ConvexRing> cells = input.cells;

  auto pinnedVertex = [&](Position v) {
    for (const auto &ring : input.pinnedRings)
    {
      for (auto p : ring)
      {
        if (length(p, v) < 1e-6)
        {
          return true;
        }
      }
    }

    return false;
  };

  for (size_t i = 0; i < n; ++i)
  {
    const Position prev = outline[(i + n - 1) % n], v = outline[i], next = outline[(i + 1) % n];

    if (pinnedVertex(v))
    {
      ++output.pinned;
      continue;
    }

    const double f = output.smoothing;
    Position to{(prev.x + f * v.x + next.x) / (2 + f), 0, (prev.z + f * v.z + next.z) / (2 + f)};
    bool accepted = false;

    for (int attempt = 0; attempt < 4 && !accepted; ++attempt)
    {
      accepted = true;

      for (const auto &cell : cells)
      {
        const bool shares = std::any_of(cell.begin(), cell.end(), [&](Position p) {
          return length(p, v) < 1e-6;
        });

        if (accepted && shares)
        {
          accepted = convexAfter(cell, v, to);
        }
      }

      if (!accepted)
      {
        to = {(v.x + to.x) * .5, 0, (v.z + to.z) * .5};
        ++output.halvings;
      }
    }

    if (!accepted)
    {
      continue;
    }

    for (auto &cell : cells)
    {
      for (auto &p : cell)
      {
        if (length(p, v) < 1e-6)
        {
          p = to;
        }
      }
    }

    output.moves.push_back({v, to});
    ++output.smoothed;
  }

  remap(output, outline);

  // Same-sized straight sections: the smoothed circumference is resampled by
  // arc length into chords no longer than the section, and that ring is the
  // wall and the boundary.
  {
    const size_t m = outline.size();
    std::vector<double> arc(m + 1, 0);

    for (size_t i = 0; i < m; ++i)
    {
      arc[i + 1] = arc[i] + length(outline[i], outline[(i + 1) % m]);
    }

    const double perimeter = arc[m];
    const unsigned count = std::max(input.minimumSections, unsigned(std::ceil(perimeter / input.sectionMetres)));
    ConvexRing ring;

    for (unsigned k = 0; k < count; ++k)
    {
      const double u = perimeter * k / count;
      size_t i = 0;

      while (i + 1 < m && arc[i + 1] < u)
      {
        ++i;
      }

      const double l = arc[i + 1] - arc[i], t = l > 1e-9 ? (u - arc[i]) / l : 0;
      const Position a = outline[i], b = outline[(i + 1) % m];
      ring.push_back({a.x + (b.x - a.x) * t, 0, a.z + (b.z - a.z) * t});
    }

    if (ring.size() >= 3 && signedArea(ring) > 0)
    {
      outline = ring;
    }
    else
    {
      ledger.add(LedgerKind::Repaired, 0, "curtain wall: the section ring degenerated; the smoothed outline is used");
    }
  }

  output.ring = std::move(outline);
  ledger.add(
      LedgerKind::Repaired,
      0,
      "curtain wall: " + std::to_string(output.smoothed) + " of " + std::to_string(n) + " vertices smoothed, " + std::to_string(output.pinned) + " pinned, " + std::to_string(output.halvings) + " halvings for convexity");
  return true;
}

} // namespace procedural::world::generator
