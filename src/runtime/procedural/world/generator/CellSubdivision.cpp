#include "procedural/world/generator/CellSubdivision.hpp"

#include "procedural/geom2/CellShape.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world::generator {

using geom2::Board;
using geom2::Chain;
using geom2::Dir;
using geom2::kNoCell;
using geom2::kNoEdge;

namespace {

bool fail(std::string *error, const char *text)
{
  if (error != nullptr)
  {
    *error = text;
  }

  return false;
}

bool tagged(const std::vector<std::string> &tags, const char *what)
{
  return std::find(tags.begin(), tags.end(), what) != tags.end();
}

// A cut, ready to be scored: the wall itself and the two contours it leaves.
struct Candidate
{
  Chain wall;
  Chain left;
  Chain right;
  double weight = 0;
};

// How habitable a piece is.
//
// Note the inversion. With `noNooks` the narrow cells count against a piece;
// without it, and only while the piece is still smaller than a room, they count
// FOR it. That is not a quirk - it is the corridor generator. A piece that is
// all corridor scores well, survives the draw, and comes out as a corridor.
double quality(const geom2::CellGrid &grid, const Chain &contour, const SubdivisionSettings &settings)
{
  const Board area = grid.contourArea(contour);
  const uint32_t total = area.count();

  if (total == 0)
  {
    return 0;
  }

  const uint32_t thin = geom2::narrow(grid, area).count();

  // The hard floor, before any of the scoring. A piece that is mostly narrow
  // cannot be cut into anything but narrower pieces, so refusing it here is
  // refusing it at the only point where refusing it does any good - once the
  // wall is drawn, the sliver exists and the recursion can only subdivide it.
  //
  // Not under `hallways`. That tag inverts the narrow rule on purpose - narrow
  // pieces are how a corridor is generated rather than planned - so a floor
  // that asked for corridors may have them, and a floor that did not may not
  // have slivers. Applying the floor to both was a quiet regression: it made
  // every emergent corridor illegal, and the lighting rule that puts a bracket
  // on a passage wall stopped firing anywhere in the corpus, which is how it
  // was caught.
  if (settings.minimumRoomCells >= 2 && settings.noNooks)
  {
    if (total < settings.minimumRoomCells * settings.minimumRoomCells || thin * 2 > total)
    {
      return 0;
    }

    // ...and it has to be that wide SOMEWHERE.
    //
    // `narrow` asks about a two-by-two block and nothing else, so a minimum of
    // three cells bought nine cells of area and no width at all: on a city
    // house's metre cell every room came out two metres across, which is a
    // passage somebody has called a room. A room must hold a square of its
    // own minimum.
    if (settings.minimumRoomCells > 2 && std::getenv("NO_SQUARE_RULE") == nullptr)
    {
      Board block = area;

      for (uint32_t step = 1; step < settings.minimumRoomCells && !block.empty(); ++step)
      {
        block = block & grid.shifted(block, geom2::Dir::East) & grid.shifted(block, geom2::Dir::South) & grid.shifted(grid.shifted(block, geom2::Dir::South), geom2::Dir::East);
      }

      if (block.empty())
      {
        return 0;
      }
    }
  }

  if (settings.noNooks)
  {
    return double(total - thin + 1) / double(total + 1);
  }

  if (double(total) > settings.averageRoomCells)
  {
    return 1;
  }

  return double(thin + 1) / double(total + 1);
}

// Where a cut may start.
//
// A seed is a (node, direction) pair on the contour; the wall then runs from
// that node a quarter turn CLOCKWISE, which is into the plan. Straight-wall
// seeds give walls perpendicular to the facade, so rooms echo the silhouette;
// corner seeds square off the outline's re-entrant notches, so the partition
// comes out rectangular. Those are the two tag behaviours, and they are the
// same code with a different pool.
std::vector<uint32_t> seeds(const geom2::CellGrid &grid, const Chain &contour, const SubdivisionSettings &settings)
{
  const Board area = grid.contourArea(contour);
  const Board thin = geom2::narrow(grid, area);

  const auto isThin = [&](const uint32_t c) { return c == kNoCell || grid.contains(thin, c); };

  const size_t n = contour.size();
  std::vector<uint32_t> straights;
  std::vector<uint32_t> corners;

  for (size_t k = 0; k < n; ++k)
  {
    const uint32_t here = contour[k];
    const uint32_t previous = contour[(k + n - 1) % n];
    const Dir hereDir = grid.decode(here).dir;
    const Dir previousDir = grid.decode(previous).dir;
    const uint32_t at = grid.decode(here).node;

    if (hereDir == previousDir)
    {
      // The middle of a straight wall. Skipped where both its cells are
      // already narrow: there is nothing left to divide there.
      if (!(isThin(grid.edgeCell(previous)) && isThin(grid.edgeCell(here))))
      {
        straights.push_back(here);
      }

      continue;
    }

    if (hereDir != ccw(previousDir))
    {
      // A convex corner, which is never a seed: a cut from there leaves the
      // plan immediately.
      continue;
    }

    const uint32_t inward = grid.edge(at, cw(hereDir));
    const uint32_t diagonal = inward == kNoEdge ? kNoCell : grid.edgeCell(inward);

    if (!(isThin(grid.edgeCell(here)) && isThin(diagonal)))
    {
      corners.push_back(here);
    }

    // The same corner taken the other way round: a wall along the previous
    // edge's direction rather than this one's.
    const uint32_t other = grid.edge(at, previousDir);

    if (other != kNoEdge && !(isThin(diagonal) && isThin(grid.edgeCell(previous))))
    {
      corners.push_back(other);
    }
  }

  if (settings.preferCorners && !corners.empty())
  {
    return corners;
  }

  if (settings.preferWalls && !straights.empty())
  {
    return straights;
  }

  std::vector<uint32_t> pool = straights;
  pool.insert(pool.end(), corners.begin(), corners.end());

  if (pool.empty())
  {
    return settings.preferCorners ? straights : corners;
  }

  return pool;
}

// Runs a wall from `from` in `dir` until it reaches the contour.
bool runWall(const geom2::CellGrid &grid, const Chain &contour, const uint32_t from, const Dir dir, Chain &wall)
{
  uint32_t at = from;
  const uint32_t limit = grid.width() + grid.height() + 2;

  for (uint32_t step = 0; step < limit; ++step)
  {
    const uint32_t e = grid.edge(at, dir);

    if (e == kNoEdge)
    {
      return false;
    }

    wall.push_back(e);
    at = grid.edgeEndNode(e);

    if (geom2::findEdgeFrom(grid, contour, at).has_value())
    {
      return true;
    }
  }

  return false;
}

bool buildCandidate(const geom2::CellGrid &grid,
                    const Chain &contour,
                    const uint32_t seed,
                    const int32_t bend,
                    Candidate &out)
{
  const geom2::Edge decoded = grid.decode(seed);
  const uint32_t start = decoded.node;
  const Dir into = cw(decoded.dir);

  if (geom2::findEdgeFrom(grid, contour, start) == std::nullopt && bend == 0)
  {
    // A seed must sit on the contour; the synthetic corner seeds do.
  }

  Chain wall;

  if (!runWall(grid, contour, start, into, wall) || wall.empty())
  {
    return false;
  }

  if (bend != 0)
  {
    if (wall.size() < 2)
    {
      return false;
    }

    // Half way, then turn. A long cut across a big room bends; a short one
    // across a small one has nothing to gain by it.
    wall.resize(std::max<size_t>(1, wall.size() / 2));
    const Dir turned = bend > 0 ? cw(into) : ccw(into);

    if (!runWall(grid, contour, grid.edgeEndNode(wall.back()), turned, wall))
    {
      return false;
    }
  }

  const uint32_t head = grid.decode(wall.front()).node;
  const uint32_t tail = grid.edgeEndNode(wall.back());

  if (head == tail)
  {
    return false;
  }

  Chain left = geom2::sliceChain(grid, contour, tail, head);
  Chain right = geom2::sliceChain(grid, contour, head, tail);

  if (left.empty() || right.empty())
  {
    return false;
  }

  left.insert(left.end(), wall.begin(), wall.end());
  const Chain reversed = grid.reverseChain(wall);
  right.insert(right.end(), reversed.begin(), reversed.end());

  out.wall = std::move(wall);
  out.left = std::move(left);
  out.right = std::move(right);

  return true;
}

} // namespace

SubdivisionSettings subdivisionFromTags(const std::vector<std::string> &tags, const double averageRoomCells)
{
  SubdivisionSettings out;
  out.averageRoomCells = averageRoomCells;
  out.preferCorners = tagged(tags, "mechanical");
  out.preferWalls = tagged(tags, "organic");
  out.noNooks = !tagged(tags, "hallways");

  return out;
}

bool subdivide(const geom2::CellGrid &grid,
               const Chain &contour,
               const SubdivisionSettings &settings,
               Stream &stream,
               std::vector<CellRoomShape> &rooms,
               std::vector<Chain> &innerWalls,
               std::string *error)
{
  rooms.clear();
  innerWalls.clear();

  if (!grid.valid() || contour.empty())
  {
    return fail(error, "nothing to subdivide");
  }

  const double span = std::pow(2., settings.roomSizeChaos);

  std::vector<Chain> pending;
  pending.push_back(contour);

  while (!pending.empty())
  {
    if (rooms.size() + pending.size() > settings.maximumRooms)
    {
      return fail(error, "subdivision produced more rooms than the budget allows");
    }

    const Chain piece = pending.back();
    pending.pop_back();

    const Board area = grid.contourArea(piece);
    const uint32_t size = area.count();

    const bool small = settings.regularRooms
                         ? geom2::isRectangle(grid, piece) && double(size) < settings.averageRoomCells * span
                         : double(size) <= settings.averageRoomCells * std::pow(span, stream.bell3());

    if (size <= 1 || small)
    {
      rooms.push_back(CellRoomShape{area, piece});
      continue;
    }

    // Every legal cut, scored once.
    std::vector<Candidate> candidates;

    for (const uint32_t seed : seeds(grid, piece, settings))
    {
      for (const int32_t bend : {0, 1, -1})
      {
        Candidate candidate;

        if (!buildCandidate(grid, piece, seed, bend, candidate))
        {
          continue;
        }

        const double leftQuality = quality(grid, candidate.left, settings);
        const double rightQuality = quality(grid, candidate.right, settings);

        if (leftQuality <= 0 || rightQuality <= 0)
        {
          continue;
        }

        const double shape = leftQuality * rightQuality;
        double prior = 1;

        if (bend != 0)
        {
          // Watabou bends with probability chainLength / averageRoomSize. As a
          // prior on the draw rather than a coin flip before it, that is the
          // same preference without the reroll.
          prior = std::min(1., double(candidate.wall.size()) / settings.averageRoomCells);
        }

        candidate.weight = shape * shape * prior;

        if (candidate.weight > 0)
        {
          candidates.push_back(std::move(candidate));
        }
      }
    }

    if (candidates.empty())
    {
      // No cut this piece can take. An answer, not a reroll.
      rooms.push_back(CellRoomShape{area, piece});
      continue;
    }

    std::vector<double> weights;
    weights.reserve(candidates.size());

    for (const Candidate &candidate : candidates)
    {
      weights.push_back(candidate.weight);
    }

    std::vector<uint32_t> indices;
    indices.reserve(candidates.size());

    for (uint32_t k = 0; k < uint32_t(candidates.size()); ++k)
    {
      indices.push_back(k);
    }

    const Candidate &chosen = candidates[stream.weighted(indices, weights)];
    innerWalls.push_back(chosen.wall);
    pending.push_back(chosen.left);
    pending.push_back(chosen.right);
  }

  return !rooms.empty();
}

} // namespace procedural::world::generator
