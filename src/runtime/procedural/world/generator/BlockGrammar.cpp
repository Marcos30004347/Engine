#include "BlockGrammar.hpp"

#include "PatchTiling.hpp"
#include "procedural/world/WorldGenerationContracts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>

namespace procedural::world::generator {

namespace {

struct Face
{
  std::vector<uint32_t> vertices; // 3 or 4, counter-clockwise.
};

double rnd(uint64_t seed, uint64_t a, uint64_t b, uint64_t c)
{
  return double(rewrite::featureSeed(seed, 9, a, b, c) % 1000003) / 1000003.;
}

} // namespace

bool planBlockGrammar(
    uint64_t block,
    const ConvexRing &outline,
    const std::vector<bool> &streetSides,
    const std::function<double(double, double)> &terrain,
    const BlockGrammarSettings &s,
    BlockPlan &out,
    Ledger &ledger,
    std::string *error)
{
  auto fail = [&](const std::string &message) {
    if (error != nullptr)
    {
      *error = message;
    }

    return false;
  };

  if (!block || outline.size() < 3 || outline.size() > 64 || streetSides.size() != outline.size() || !terrain)
  {
    return fail("block grammar needs a block id, a ring of 3 to 64 vertices, one street flag per edge and a terrain callback");
  }

  if (!(s.cellSpacingMetres >= 3) || !(s.cellSpacingMetres <= 30) || s.relaxationPasses > 20 || s.maximumRestarts == 0 || s.maximumRestarts > 256)
  {
    return fail("invalid block grammar settings");
  }

  double minX = outline[0].x, maxX = minX, minZ = outline[0].z, maxZ = minZ;

  for (auto p : outline)
  {
    minX = std::min(minX, p.x);
    maxX = std::max(maxX, p.x);
    minZ = std::min(minZ, p.z);
    maxZ = std::max(maxZ, p.z);
  }

  const double spacing = s.cellSpacingMetres * 2; // Subdivided once into quads of about the requested size.
  const double rowHeight = spacing * .8660254;
  const uint32_t columns = uint32_t(std::ceil((maxX - minX) / spacing)) + 2, rows = uint32_t(std::ceil((maxZ - minZ) / rowHeight)) + 2;

  if (columns * rows > 4096)
  {
    return fail("block too large for the lattice budget");
  }

  // Jittered triangular lattice; vertices outside the block are kept for the
  // triangulation and clipped by discarding faces whose centre is outside.
  std::vector<Position> points;

  for (uint32_t r = 0; r < rows; ++r)
  {
    for (uint32_t c = 0; c < columns; ++c)
    {
      const double jx = (rnd(s.seed, block, r * columns + c, 1) - .5) * spacing * .35;
      const double jz = (rnd(s.seed, block, r * columns + c, 2) - .5) * spacing * .35;
      points.push_back({minX - spacing + c * spacing + (r % 2 ? spacing * .5 : 0) + jx, 0, minZ - rowHeight + r * rowHeight + jz});
    }
  }

  auto at = [&](uint32_t r, uint32_t c) {
    return r * columns + c;
  };

  std::vector<Face> faces;

  for (uint32_t r = 0; r + 1 < rows; ++r)
  {
    for (uint32_t c = 0; c + 1 < columns; ++c)
    {
      // Two triangles per lattice cell; the row parity decides the diagonal.
      const uint32_t a = at(r, c), b = at(r, c + 1), d = at(r + 1, c), e = at(r + 1, c + 1);

      if (r % 2 == 0)
      {
        faces.push_back({{a, b, d}});
        faces.push_back({{b, e, d}});
      }
      else
      {
        faces.push_back({{a, e, d}});
        faces.push_back({{a, b, e}});
      }
    }
  }

  // Merge seeded pairs of adjacent triangles into quads (Townscaper's first step).
  std::vector<bool> used(faces.size(), false);
  std::vector<Face> merged;
  std::map<std::pair<uint32_t, uint32_t>, std::vector<size_t>> byEdge;

  for (size_t i = 0; i < faces.size(); ++i)
  {
    for (size_t k = 0; k < 3; ++k)
    {
      auto key = std::minmax(faces[i].vertices[k], faces[i].vertices[(k + 1) % 3]);
      byEdge[key].push_back(i);
    }
  }

  for (size_t i = 0; i < faces.size(); ++i)
  {
    if (used[i])
    {
      continue;
    }

    if (rnd(s.seed, block, i, 3) < .55)
    {
      for (size_t k = 0; k < 3 && !used[i]; ++k)
      {
        auto key = std::minmax(faces[i].vertices[k], faces[i].vertices[(k + 1) % 3]);

        for (auto j : byEdge[key])
        {
          if (j == i || used[j])
          {
            continue;
          }

          // Quad = both triangles around the shared edge.
          uint32_t apexJ = 0;

          for (auto v : faces[j].vertices)
          {
            if (v != key.first && v != key.second)
            {
              apexJ = v;
            }
          }

          Face quad;
          quad.vertices = {faces[i].vertices[k], apexJ, faces[i].vertices[(k + 1) % 3], faces[i].vertices[(k + 2) % 3]};
          used[i] = used[j] = true;
          merged.push_back(quad);
          break;
        }
      }
    }

    if (!used[i])
    {
      used[i] = true;
      merged.push_back(faces[i]);
    }
  }

  // Midpoint subdivision: every face becomes quads around its centre.
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> edgeMidpoints;

  auto midpoint = [&](uint32_t a, uint32_t b) {
    auto key = std::minmax(a, b);
    auto it = edgeMidpoints.find(key);

    if (it != edgeMidpoints.end())
    {
      return it->second;
    }

    uint32_t id = uint32_t(points.size());
    points.push_back({(points[a].x + points[b].x) * .5, 0, (points[a].z + points[b].z) * .5});
    edgeMidpoints[key] = id;

    return id;
  };

  std::vector<Face> quads;

  for (const auto &face : merged)
  {
    Position centre;

    for (auto v : face.vertices)
    {
      centre.x += points[v].x / face.vertices.size();
      centre.z += points[v].z / face.vertices.size();
    }

    const uint32_t c = uint32_t(points.size());
    points.push_back(centre);
    const size_t n = face.vertices.size();

    for (size_t k = 0; k < n; ++k)
    {
      const uint32_t v = face.vertices[k];
      const uint32_t before = midpoint(face.vertices[(k + n - 1) % n], v), after = midpoint(v, face.vertices[(k + 1) % n]);
      quads.push_back({{v, after, c, before}});
    }
  }

  // Relaxation: interior vertices move toward the mean of their neighbours;
  // vertices near the block boundary are projected onto it so cells stay
  // inside.
  std::vector<std::set<uint32_t>> links(points.size());

  for (const auto &q : quads)
  {
    for (size_t k = 0; k < 4; ++k)
    {
      links[q.vertices[k]].insert(q.vertices[(k + 1) % 4]);
      links[q.vertices[(k + 1) % 4]].insert(q.vertices[k]);
    }
  }

  for (uint32_t pass = 0; pass < s.relaxationPasses; ++pass)
  {
    std::vector<Position> next = points;

    for (uint32_t v = 0; v < points.size(); ++v)
    {
      if (links[v].empty())
      {
        continue;
      }

      Position mean;

      for (auto n : links[v])
      {
        mean.x += points[n].x / links[v].size();
        mean.z += points[n].z / links[v].size();
      }

      next[v] = {points[v].x * .4 + mean.x * .6, 0, points[v].z * .4 + mean.z * .6};
    }

    points = std::move(next);
  }

  // Keep the quads whose centre lies inside the block; clamp their outside
  // vertices onto the outline so every cell stays within the block.
  std::vector<uint32_t> cellFace;

  for (size_t i = 0; i < quads.size(); ++i)
  {
    Position centre;

    for (auto v : quads[i].vertices)
    {
      centre.x += points[v].x * .25;
      centre.z += points[v].z * .25;
    }

    if (insideEvenOdd(outline, centre))
    {
      cellFace.push_back(uint32_t(i));
    }
  }

  if (cellFace.empty())
  {
    return fail("block holds no cell at this spacing");
  }

  std::set<uint32_t> clamped;

  for (auto i : cellFace)
  {
    for (auto v : quads[i].vertices)
    {
      if (insideEvenOdd(outline, points[v]) || clamped.count(v))
      {
        continue;
      }

      double best = INFINITY;
      Position projected = points[v];

      for (size_t k = 0; k < outline.size(); ++k)
      {
        auto a = outline[k], b = outline[(k + 1) % outline.size()];
        double dx = b.x - a.x, dz = b.z - a.z, l2 = dx * dx + dz * dz;
        double t = l2 > 0 ? std::clamp(((points[v].x - a.x) * dx + (points[v].z - a.z) * dz) / l2, 0., 1.) : 0;
        Position q{a.x + dx * t, 0, a.z + dz * t};
        double d = length(points[v], q);

        if (d < best)
        {
          best = d;
          projected = q;
        }
      }

      points[v] = projected;
      clamped.insert(v);
    }
  }

  BlockPlan plan;
  plan.block = block;
  std::map<uint32_t, uint32_t> faceToCell;

  for (auto i : cellFace)
  {
    BlockCell cell;
    cell.id = uint32_t(plan.cells.size());
    faceToCell[i] = cell.id;

    for (auto v : quads[i].vertices)
    {
      cell.ring.push_back(points[v]);
    }

    if (signedArea(cell.ring) < 0)
    {
      std::reverse(cell.ring.begin(), cell.ring.end());
    }

    cell.centre = centroid(cell.ring);
    cell.elevation = terrain(cell.centre.x, cell.centre.z);
    plan.cells.push_back(cell);
  }

  std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> cellEdges;

  for (auto i : cellFace)
  {
    for (size_t k = 0; k < 4; ++k)
    {
      cellEdges[std::minmax(quads[i].vertices[k], quads[i].vertices[(k + 1) % 4])].push_back(faceToCell[i]);
    }
  }

  for (auto &[edge, owners] : cellEdges)
  {
    for (auto a : owners)
    {
      for (auto b : owners)
      {
        if (a != b)
        {
          plan.cells[a].neighbours.push_back(b);
        }
      }
    }
  }

  for (auto &cell : plan.cells)
  {
    std::sort(cell.neighbours.begin(), cell.neighbours.end());
    cell.neighbours.erase(std::unique(cell.neighbours.begin(), cell.neighbours.end()), cell.neighbours.end());
  }

  // Street cells: cells with a vertex on a street side of the outline.
  for (auto &cell : plan.cells)
  {
    for (auto v : cell.ring)
    {
      for (size_t k = 0; k < outline.size(); ++k)
      {
        if (streetSides[k] && segmentDistance(v, outline[k], outline[(k + 1) % outline.size()]) < 1e-6)
        {
          cell.tile = BlockTile::Street;
        }
      }
    }
  }

  const bool anyStreet = std::any_of(plan.cells.begin(), plan.cells.end(), [](const BlockCell &c) {
    return c.tile == BlockTile::Street;
  });

  if (!anyStreet)
  {
    return fail("block has no street side; nothing can be accessed");
  }

  // Simplified wave-function collapse over {Alley, Lot, Court}: a lot needs an
  // alley or street neighbour, a court may not touch a street, an alley must
  // reach a street through alleys (audited after collapse). Bounded restarts.
  enum Option : uint8_t
  {
    OptAlley = 1,
    OptLot = 2,
    OptCourt = 4
  };

  const size_t n = plan.cells.size();
  std::vector<BlockTile> best;
  uint32_t bestUnresolved = UINT32_MAX;

  for (uint32_t restart = 0; restart < s.maximumRestarts; ++restart)
  {
    std::vector<uint8_t> domain(n, OptAlley | OptLot | OptCourt);
    std::vector<BlockTile> tile(n, BlockTile::Unresolved);

    for (size_t i = 0; i < n; ++i)
    {
      if (plan.cells[i].tile == BlockTile::Street)
      {
        tile[i] = BlockTile::Street;
        domain[i] = 0;
      }
    }

    for (size_t i = 0; i < n; ++i)
    {
      if (tile[i] != BlockTile::Street)
      {
        for (auto nb : plan.cells[i].neighbours)
        {
          if (tile[nb] == BlockTile::Street)
          {
            domain[i] &= ~OptCourt; // Courts never touch the street.
          }
        }
      }
    }

    auto canAccess = [&](size_t i) {
      for (auto nb : plan.cells[i].neighbours)
      {
        if (tile[nb] == BlockTile::Street || tile[nb] == BlockTile::Alley || (tile[nb] == BlockTile::Unresolved && (domain[nb] & OptAlley)))
        {
          return true;
        }
      }

      return false;
    };

    bool contradiction = false;

    for (uint32_t step = 0; step < n && !contradiction; ++step)
    {
      // Propagate: lots without possible access lose the lot option.
      bool changed = true;

      while (changed && !contradiction)
      {
        changed = false;

        for (size_t i = 0; i < n; ++i)
        {
          if (tile[i] != BlockTile::Unresolved)
          {
            continue;
          }

          if ((domain[i] & OptLot) && !canAccess(i))
          {
            domain[i] &= ~OptLot;
            changed = true;
          }

          if (domain[i] == 0)
          {
            contradiction = true;
            break;
          }
        }
      }

      if (contradiction)
      {
        break;
      }

      // Observe: the cell with the fewest options, ties by seeded noise.
      size_t pick = n;
      double bestScore = INFINITY;

      for (size_t i = 0; i < n; ++i)
      {
        if (tile[i] != BlockTile::Unresolved)
        {
          continue;
        }

        const int options = ((domain[i] & OptAlley) != 0) + ((domain[i] & OptLot) != 0) + ((domain[i] & OptCourt) != 0);
        const double score = options + rnd(s.seed + restart, block, i, 4) * .5;

        if (score < bestScore)
        {
          bestScore = score;
          pick = i;
        }
      }

      if (pick == n)
      {
        break;
      }

      double wa = (domain[pick] & OptAlley) ? s.alleyWeight : 0, wl = (domain[pick] & OptLot) ? s.lotWeight : 0, wc = (domain[pick] & OptCourt) ? s.courtWeight : 0;

      // Alleys are favoured beside a street so the network starts there.
      for (auto nb : plan.cells[pick].neighbours)
      {
        if (tile[nb] == BlockTile::Street)
        {
          wa *= 1.6;
        }
      }

      const double roll = rnd(s.seed + restart, block, pick, 5) * (wa + wl + wc);
      tile[pick] = roll < wa ? BlockTile::Alley : roll < wa + wl ? BlockTile::Lot : BlockTile::Court;
      domain[pick] = 0;
    }

    if (contradiction)
    {
      ++plan.restarts;
      continue;
    }

    // Audit: alleys must reach a street through alleys; lots need access.
    std::vector<bool> reached(n, false);
    std::vector<size_t> frontier;

    for (size_t i = 0; i < n; ++i)
    {
      if (tile[i] == BlockTile::Street)
      {
        reached[i] = true;
        frontier.push_back(i);
      }
    }

    while (!frontier.empty())
    {
      auto i = frontier.back();
      frontier.pop_back();

      for (auto nb : plan.cells[i].neighbours)
      {
        if (!reached[nb] && tile[nb] == BlockTile::Alley)
        {
          reached[nb] = true;
          frontier.push_back(nb);
        }
      }
    }

    uint32_t unresolved = 0;

    for (size_t i = 0; i < n; ++i)
    {
      if (tile[i] == BlockTile::Alley && !reached[i])
      {
        tile[i] = BlockTile::Court; // A dead alley is a court, not a path.
      }
    }

    for (size_t i = 0; i < n; ++i)
    {
      if (tile[i] != BlockTile::Lot)
      {
        continue;
      }

      bool access = false;

      for (auto nb : plan.cells[i].neighbours)
      {
        access |= tile[nb] == BlockTile::Street || (tile[nb] == BlockTile::Alley && reached[nb]);
      }

      if (!access)
      {
        ++unresolved;
      }
    }

    if (unresolved < bestUnresolved)
    {
      bestUnresolved = unresolved;
      best = tile;
    }

    if (unresolved == 0)
    {
      break;
    }

    ++plan.restarts;
  }

  if (best.empty())
  {
    return fail("block grammar found no consistent tiling within the restart budget");
  }

  for (size_t i = 0; i < n; ++i)
  {
    plan.cells[i].tile = best[i];
  }

  // Bounded repair: a lot without access takes an alley through the lot
  // beside it that already touches the network; when no neighbour can carry
  // an alley the lot becomes a court. Each pass fixes one lot.
  {
    auto reachedPath = [&]() {
      std::vector<bool> reached(n, false);
      std::vector<size_t> frontier;

      for (size_t i = 0; i < n; ++i)
      {
        if (plan.cells[i].tile == BlockTile::Street)
        {
          reached[i] = true;
          frontier.push_back(i);
        }
      }

      while (!frontier.empty())
      {
        auto i = frontier.back();
        frontier.pop_back();

        for (auto nb : plan.cells[i].neighbours)
        {
          if (!reached[nb] && plan.cells[nb].tile == BlockTile::Alley)
          {
            reached[nb] = true;
            frontier.push_back(nb);
          }
        }
      }

      return reached;
    };

    for (uint32_t pass = 0; pass < n; ++pass)
    {
      auto reached = reachedPath();
      size_t stranded = n;

      for (size_t i = 0; i < n && stranded == n; ++i)
      {
        if (plan.cells[i].tile != BlockTile::Lot)
        {
          continue;
        }

        bool access = false;

        for (auto nb : plan.cells[i].neighbours)
        {
          access |= plan.cells[nb].tile == BlockTile::Street || (plan.cells[nb].tile == BlockTile::Alley && reached[nb]);
        }

        if (!access)
        {
          stranded = i;
        }
      }

      if (stranded == n)
      {
        break;
      }

      size_t carrier = n;

      for (auto nb : plan.cells[stranded].neighbours)
      {
        if (plan.cells[nb].tile != BlockTile::Lot)
        {
          continue;
        }

        for (auto nn : plan.cells[nb].neighbours)
        {
          if (plan.cells[nn].tile == BlockTile::Street || (plan.cells[nn].tile == BlockTile::Alley && reached[nn]))
          {
            carrier = nb;
          }
        }
      }

      if (carrier != n)
      {
        plan.cells[carrier].tile = BlockTile::Alley;
        ledger.add(LedgerKind::Repaired, block, "block " + std::to_string(block) + ": lot " + std::to_string(carrier) + " became an alley to reach lot " + std::to_string(stranded));
      }
      else
      {
        plan.cells[stranded].tile = BlockTile::Court;
        ledger.add(LedgerKind::Omitted, block, "block " + std::to_string(block) + ": lot " + std::to_string(stranded) + " has no possible access and became a court");
      }
    }
  }

  // Stairs: alleys that climb more than the step between neighbouring path cells.
  for (auto &cell : plan.cells)
  {
    if (cell.tile != BlockTile::Alley)
    {
      continue;
    }

    for (auto nb : cell.neighbours)
    {
      const auto &other = plan.cells[nb];

      if ((other.tile == BlockTile::Alley || other.tile == BlockTile::Street || other.tile == BlockTile::Stair) && std::abs(other.elevation - cell.elevation) > s.stairStepMetres)
      {
        cell.tile = BlockTile::Stair;
      }
    }
  }

  for (auto &cell : plan.cells)
  {
    bool access = false;

    for (auto nb : cell.neighbours)
    {
      access |= plan.cells[nb].tile == BlockTile::Street || plan.cells[nb].tile == BlockTile::Alley || plan.cells[nb].tile == BlockTile::Stair;
    }

    cell.accessible = cell.tile == BlockTile::Lot ? access : cell.tile != BlockTile::Court;

    switch (cell.tile)
    {
    case BlockTile::Street:
      ++plan.streets;
      break;
    case BlockTile::Alley:
      ++plan.alleys;
      break;
    case BlockTile::Stair:
      ++plan.stairs;
      break;
    case BlockTile::Lot:
      ++plan.lots;
      plan.inaccessibleLots += !access;
      break;
    case BlockTile::Court:
      ++plan.courts;
      break;
    default:
      ++plan.unresolved;
      break;
    }
  }

  plan.solved = plan.inaccessibleLots == 0 && plan.unresolved == 0;

  if (!plan.solved)
  {
    ledger.add(LedgerKind::Rejected, block, "block " + std::to_string(block) + ": " + std::to_string(plan.inaccessibleLots) + " lots without alley or street access after " + std::to_string(plan.restarts) + " restarts");
  }

  if (error != nullptr)
  {
    error->clear();
  }

  out = std::move(plan);

  return true;
}

} // namespace procedural::world::generator
