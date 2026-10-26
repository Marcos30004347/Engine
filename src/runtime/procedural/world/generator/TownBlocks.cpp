#include "TownBlocks.hpp"

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

  ConvexRing r;

  for (auto v : q)
  {
    if (r.empty() || distance(r.back(), v) > 1e-7)
    {
      r.push_back(v);
    }
  }

  while (r.size() > 1 && distance(r.front(), r.back()) < 1e-7)
  {
    r.pop_back();
  }

  return r;
}

// Whether the segment (c, d) runs along the edge (a, b): parallel within
// about ten degrees, within `tolerance` of it, overlapping it in projection.
bool alongEdge(Position a, Position b, Position c, Position d, double tolerance)
{
  const double l = distance(a, b), m = distance(c, d);

  if (l < 1e-9 || m < 1e-9)
  {
    return false;
  }

  if (std::abs((b.x - a.x) * (d.z - c.z) - (b.z - a.z) * (d.x - c.x)) / (l * m) > .17)
  {
    return false;
  }

  if (std::abs(cross(a, b, c)) / l > tolerance || std::abs(cross(a, b, d)) / l > tolerance)
  {
    return false;
  }

  const double t0 = ((c.x - a.x) * (b.x - a.x) + (c.z - a.z) * (b.z - a.z)) / (l * l);
  const double t1 = ((d.x - a.x) * (b.x - a.x) + (d.z - a.z) * (b.z - a.z)) / (l * l);
  return std::max(t0, t1) > 1e-3 && std::min(t0, t1) < 1 - 1e-3;
}

bool segmentThrough(const ConvexRing &p, Position a, Position b)
{
  for (int k = 1; k < 8; ++k)
  {
    const Position q = mix(a, b, k / 8.);

    if (!strictlyInsideConvex(p, q))
    {
      continue;
    }

    double edge = 1e30;

    for (size_t e = 0; e < p.size(); ++e)
    {
      edge = std::min(edge, segmentDistance(q, p[e], p[(e + 1) % p.size()]));
    }

    if (edge > .3)
    {
      return true;
    }
  }

  return false;
}

} // namespace

TownBlocks::TownBlocks(uint64_t domain) : Generator("town.blocks", domain, "town.blocks-input", "town.blocks-plan", GenerationScope::Patch)
{
}

bool TownBlocks::plan(const TownBlocksInput &s, TownBlocksPlan &r, Ledger &ledger, std::string *error)
{
  auto fail = [&](const std::string &message) {
    ledger.add(LedgerKind::Rejected, 0, "town blocks: " + message);

    if (error != nullptr)
    {
      *error = message;
    }

    return false;
  };

  r = TownBlocksPlan{};

  if (s.patches.empty() || !s.random)
  {
    return fail("blocks need patches and a random source");
  }

  auto random = [&](uint64_t id, uint64_t purpose) {
    return s.random(id, purpose);
  };

  auto onWall = [&](Position a, Position b) {
    for (size_t i = 0; i < s.wallRing.size(); ++i)
    {
      // An outer patch edge may sit a couple of metres off the smoothed wall
      // (its vertices did not move with the core's): still the wall side.
      if (alongEdge(s.wallRing[i], s.wallRing[(i + 1) % s.wallRing.size()], a, b, 2.5) || alongEdge(a, b, s.wallRing[i], s.wallRing[(i + 1) % s.wallRing.size()], 2.5))
      {
        return true;
      }
    }

    return false;
  };

  auto onArtery = [&](Position a, Position b) {
    for (const auto &artery : s.arteries)
    {
      for (size_t i = 1; i < artery.size(); ++i)
      {
        if (alongEdge(a, b, artery[i - 1], artery[i], 1.))
        {
          return true;
        }
      }
    }

    return false;
  };

  const TownPatch *plaza = nullptr;

  for (const auto &patch : s.patches)
  {
    if (patch.plaza)
    {
      plaza = &patch;
    }
  }

  auto onPlaza = [&](Position a, Position b) {
    if (plaza == nullptr)
    {
      return false;
    }

    for (size_t i = 0; i < plaza->ring.size(); ++i)
    {
      if (alongEdge(plaza->ring[i], plaza->ring[(i + 1) % plaza->ring.size()], a, b, .05))
      {
        return true;
      }
    }

    return false;
  };

  // The patch across an edge: the patch (other than `self`) with an edge along it.
  auto neighbour = [&](const TownPatch &self, Position a, Position b) -> const TownPatch * {
    for (const auto &other : s.patches)
    {
      if (other.id == self.id)
      {
        continue;
      }

      for (size_t i = 0; i < other.ring.size(); ++i)
      {
        if (alongEdge(other.ring[i], other.ring[(i + 1) % other.ring.size()], a, b, .05))
        {
          return &other;
        }
      }
    }

    return nullptr;
  };

  auto enclosed = [&](const TownPatch &patch) {
    if (patch.withinWalls)
    {
      return true;
    }

    if (!patch.outskirts)
    {
      return false;
    }

    for (size_t i = 0; i < patch.ring.size(); ++i)
    {
      const TownPatch *other = neighbour(patch, patch.ring[i], patch.ring[(i + 1) % patch.ring.size()]);

      if (other == nullptr || !(other->withinWalls || other->outskirts))
      {
        return false;
      }
    }

    return true;
  };

  for (const auto &patch : s.patches)
  {
    if (patch.plaza || !(patch.withinWalls || patch.outskirts))
    {
      continue;
    }

    // A compound's own patch is its yard.
    bool yard = false;

    for (const auto &compound : s.compounds)
    {
      Position c{0, 0, 0};

      for (auto v : compound)
      {
        c.x += v.x / compound.size();
        c.z += v.z / compound.size();
      }

      yard |= strictlyInsideConvex(patch.ring, c);
    }

    if (yard)
    {
      ++r.yards;
      continue;
    }

    // Pieces: the patch cut by every artery segment running through it.
    std::vector<ConvexRing> pieces{patch.ring};

    for (const auto &artery : s.arteries)
    {
      for (size_t i = 1; i < artery.size(); ++i)
      {
        const Position sa = artery[i - 1], sb = artery[i];
        const double l = distance(sa, sb);

        if (l < 1e-6)
        {
          continue;
        }

        std::vector<ConvexRing> next;

        for (const auto &piece : pieces)
        {
          if (!segmentThrough(piece, sa, sb))
          {
            next.push_back(piece);
            continue;
          }

          const double nx = (sb.z - sa.z) / l, nz = (sa.x - sb.x) / l, c = nx * sa.x + nz * sa.z;
          ConvexRing left = clipHalfPlane(piece, nx, nz, c), right = clipHalfPlane(piece, -nx, -nz, -c);
          ++r.cuts;

          for (auto &half : {left, right})
          {
            if (half.size() >= 3 && signedArea(half) > 1)
            {
              next.push_back(half);
            }
          }
        }

        pieces = std::move(next);
      }
    }

    for (const auto &piece : pieces)
    {
      TownBlock block;
      block.patch = patch.id;
      block.withinWalls = patch.withinWalls;
      block.outskirts = patch.outskirts;
      block.use = patch.use;

      // getCityBlock: every edge moved inward by its clearance.
      ConvexRing ring = piece;

      for (size_t i = 0; i < piece.size() && !ring.empty(); ++i)
      {
        const Position a = piece[i], b = piece[(i + 1) % piece.size()];
        const double l = distance(a, b);

        if (l < 1e-9)
        {
          continue;
        }

        double inset = patch.withinWalls ? s.regularStreet * .5 : s.alley * .5;

        if (onWall(a, b) || onPlaza(a, b))
        {
          inset = s.mainStreet * .5;
        }
        else if (onArtery(a, b))
        {
          inset = s.arteryHalfWidth;
        }

        const double nx = (b.z - a.z) / l, nz = (a.x - b.x) / l; // Outward for CCW.
        ring = clipHalfPlane(ring, nx, nz, nx * a.x + nz * a.z - inset);
      }

      if (ring.size() < 3 || signedArea(ring) < 20)
      {
        continue;
      }

      block.ring = ring;

      // Outskirts: populated edges and vertex densities of the piece.
      if (patch.outskirts)
      {
        for (size_t i = 0; i < piece.size(); ++i)
        {
          const Position a = piece[i], b = piece[(i + 1) % piece.size()];

          if (onArtery(a, b))
          {
            block.populated.push_back({a, b, 1});
            continue;
          }

          const TownPatch *other = neighbour(patch, a, b);

          if (other != nullptr && (other->withinWalls || other->outskirts))
          {
            block.populated.push_back({a, b, enclosed(*other) ? 1. : .4});
          }
        }

        std::vector<double> pieceDensity;

        for (size_t i = 0; i < piece.size(); ++i)
        {
          const Position v = piece[i];
          bool gate = false;

          for (auto g : s.gates)
          {
            gate |= distance(g, v) < 1.5;
          }

          bool cityOnly = true;

          for (const auto &other : s.patches)
          {
            bool has = false;

            for (auto q : other.ring)
            {
              has |= distance(q, v) < 1e-3;
            }

            if (has && !(other.withinWalls || other.outskirts))
            {
              cityOnly = false;
            }
          }

          pieceDensity.push_back(gate ? 1 : cityOnly ? 2 * random(patch.id * 131 + i, 104) : 0);
        }

        for (auto v : ring)
        {
          size_t nearest = 0;

          for (size_t i = 1; i < piece.size(); ++i)
          {
            if (distance(piece[i], v) < distance(piece[nearest], v))
            {
              nearest = i;
            }
          }

          block.vertexDensity.push_back(pieceDensity[nearest]);
        }
      }

      r.blocks.push_back(std::move(block));
    }
  }

  ledger.add(
      LedgerKind::Repaired,
      0,
      "town blocks: " + std::to_string(r.blocks.size()) + " blocks from " + std::to_string(s.patches.size()) + " patches, " + std::to_string(r.cuts) + " road cuts, " + std::to_string(r.yards) + " compound yards");
  return true;
}

bool TownBlocks::run(const TownBlocksInput &input, TownBlocksPlan &output, Ledger &ledger, std::string *error)
{
  return plan(input, output, ledger, error);
}

} // namespace procedural::world::generator
