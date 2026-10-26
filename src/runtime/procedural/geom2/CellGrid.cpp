#include "procedural/geom2/CellGrid.hpp"

#include <cmath>

#include <algorithm>

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

} // namespace

const char *name(const Dir d)
{
  switch (d)
  {
    case Dir::North:
      return "north";
    case Dir::East:
      return "east";
    case Dir::South:
      return "south";
    case Dir::West:
      return "west";
  }

  return "?";
}

// ---------------------------------------------------------------------------
// Board
// ---------------------------------------------------------------------------

uint32_t Board::count() const
{
  uint32_t total = 0;

  for (const uint64_t word : row)
  {
    total += uint32_t(__builtin_popcountll(word));
  }

  return total;
}

bool Board::empty() const
{
  for (const uint64_t word : row)
  {
    if (word != 0)
    {
      return false;
    }
  }

  return true;
}

bool Board::first(uint32_t &i, uint32_t &j) const
{
  for (uint32_t r = 0; r < kMaxRows; ++r)
  {
    if (row[r] != 0)
    {
      i = r;
      j = uint32_t(__builtin_ctzll(row[r]));

      return true;
    }
  }

  return false;
}

bool Board::operator==(const Board &other) const
{
  for (uint32_t r = 0; r < kMaxRows; ++r)
  {
    if (row[r] != other.row[r])
    {
      return false;
    }
  }

  return true;
}

Board &Board::operator|=(const Board &other)
{
  for (uint32_t r = 0; r < kMaxRows; ++r)
  {
    row[r] |= other.row[r];
  }

  return *this;
}

Board &Board::operator&=(const Board &other)
{
  for (uint32_t r = 0; r < kMaxRows; ++r)
  {
    row[r] &= other.row[r];
  }

  return *this;
}

Board &Board::operator^=(const Board &other)
{
  for (uint32_t r = 0; r < kMaxRows; ++r)
  {
    row[r] ^= other.row[r];
  }

  return *this;
}

Board operator|(Board a, const Board &b)
{
  a |= b;

  return a;
}

Board operator&(Board a, const Board &b)
{
  a &= b;

  return a;
}

Board operator^(Board a, const Board &b)
{
  a ^= b;

  return a;
}

Board operator~(Board a)
{
  for (uint32_t r = 0; r < Board::kMaxRows; ++r)
  {
    a.row[r] = ~a.row[r];
  }

  return a;
}

std::vector<std::pair<uint32_t, uint32_t>> cells(const Board &b)
{
  std::vector<std::pair<uint32_t, uint32_t>> out;

  for (uint32_t i = 0; i < Board::kMaxRows; ++i)
  {
    uint64_t word = b.row[i];

    while (word != 0)
    {
      const uint32_t j = uint32_t(__builtin_ctzll(word));
      out.emplace_back(i, j);
      word &= word - 1;
    }
  }

  return out;
}

// ---------------------------------------------------------------------------
// CellGrid
// ---------------------------------------------------------------------------

CellGrid::CellGrid(const uint32_t width, const uint32_t height)
{
  if (width == 0 || height == 0 || width > Board::kMaxColumns || height > Board::kMaxRows)
  {
    return;
  }

  width_ = width;
  height_ = height;
}

uint32_t CellGrid::cell(const int32_t i, const int32_t j) const
{
  if (i < 0 || j < 0 || uint32_t(i) >= height_ || uint32_t(j) >= width_)
  {
    return kNoCell;
  }

  return uint32_t(i) * width_ + uint32_t(j);
}

uint32_t CellGrid::node(const int32_t i, const int32_t j) const
{
  if (i < 0 || j < 0 || uint32_t(i) > height_ || uint32_t(j) > width_)
  {
    return kNoNode;
  }

  return uint32_t(i) * (width_ + 1) + uint32_t(j);
}

uint32_t CellGrid::neighbour(const uint32_t c, const Dir d) const
{
  if (c == kNoCell)
  {
    return kNoCell;
  }

  return cell(int32_t(cellRow(c)) + deltaI(d), int32_t(cellColumn(c)) + deltaJ(d));
}

uint32_t CellGrid::edge(const uint32_t fromNode, const Dir d) const
{
  if (fromNode == kNoNode)
  {
    return kNoEdge;
  }

  // The far end has to exist, or the edge leaves the lattice.
  if (node(int32_t(nodeRow(fromNode)) + deltaI(d), int32_t(nodeColumn(fromNode)) + deltaJ(d)) == kNoNode)
  {
    return kNoEdge;
  }

  return fromNode * 4u + uint32_t(d);
}

Edge CellGrid::decode(const uint32_t e) const
{
  if (e == kNoEdge)
  {
    return {};
  }

  return {e / 4u, Dir(e % 4u)};
}

uint32_t CellGrid::edgeEndNode(const uint32_t e) const
{
  if (e == kNoEdge)
  {
    return kNoNode;
  }

  const Edge d = decode(e);

  return node(int32_t(nodeRow(d.node)) + deltaI(d.dir), int32_t(nodeColumn(d.node)) + deltaJ(d.dir));
}

uint32_t CellGrid::twin(const uint32_t e) const
{
  if (e == kNoEdge)
  {
    return kNoEdge;
  }

  const Edge d = decode(e);

  return edge(edgeEndNode(e), opposite(d.dir));
}

uint32_t CellGrid::edgeCell(const uint32_t e) const
{
  if (e == kNoEdge)
  {
    return kNoCell;
  }

  const Edge d = decode(e);
  const int32_t i = int32_t(nodeRow(d.node));
  const int32_t j = int32_t(nodeColumn(d.node));

  switch (d.dir)
  {
    case Dir::East:
      return cell(i, j);
    case Dir::South:
      return cell(i, j - 1);
    case Dir::West:
      return cell(i - 1, j - 1);
    case Dir::North:
      return cell(i - 1, j);
  }

  return kNoCell;
}

uint32_t CellGrid::cellEdge(const uint32_t c, const Dir side) const
{
  if (c == kNoCell)
  {
    return kNoEdge;
  }

  const int32_t i = int32_t(cellRow(c));
  const int32_t j = int32_t(cellColumn(c));

  switch (side)
  {
    case Dir::North:
      return edge(node(i, j), Dir::East);
    case Dir::East:
      return edge(node(i, j + 1), Dir::South);
    case Dir::South:
      return edge(node(i + 1, j + 1), Dir::West);
    case Dir::West:
      return edge(node(i + 1, j), Dir::North);
  }

  return kNoEdge;
}

Board CellGrid::mask() const
{
  Board out;

  if (!valid())
  {
    return out;
  }

  const uint64_t columns = width_ == 64 ? ~uint64_t(0) : ((uint64_t(1) << width_) - 1);

  for (uint32_t i = 0; i < height_; ++i)
  {
    out.row[i] = columns;
  }

  return out;
}

Board CellGrid::shifted(const Board &b, const Dir d) const
{
  Board out;

  if (!valid())
  {
    return out;
  }

  switch (d)
  {
    case Dir::North:
      // A cell has a northern neighbour in `b` when the row above it is in `b`.
      for (uint32_t i = 1; i < height_; ++i)
      {
        out.row[i] = b.row[i - 1];
      }
      break;

    case Dir::South:
      for (uint32_t i = 0; i + 1 < height_; ++i)
      {
        out.row[i] = b.row[i + 1];
      }
      break;

    case Dir::East:
      for (uint32_t i = 0; i < height_; ++i)
      {
        out.row[i] = b.row[i] >> 1;
      }
      break;

    case Dir::West:
      for (uint32_t i = 0; i < height_; ++i)
      {
        out.row[i] = b.row[i] << 1;
      }
      break;
  }

  return out & mask();
}

bool CellGrid::contains(const Board &b, const uint32_t c) const
{
  if (c == kNoCell)
  {
    return false;
  }

  return b.test(cellRow(c), cellColumn(c));
}

void CellGrid::add(Board &b, const uint32_t c) const
{
  if (c != kNoCell)
  {
    b.set(cellRow(c), cellColumn(c));
  }
}

void CellGrid::remove(Board &b, const uint32_t c) const
{
  if (c != kNoCell)
  {
    b.clear(cellRow(c), cellColumn(c));
  }
}

Board CellGrid::flood(const Board &within, const uint32_t from) const
{
  Board out;

  if (!contains(within, from))
  {
    return out;
  }

  add(out, from);

  for (;;)
  {
    Board grown = out;
    grown |= shifted(out, Dir::North);
    grown |= shifted(out, Dir::East);
    grown |= shifted(out, Dir::South);
    grown |= shifted(out, Dir::West);
    grown &= within;

    if (grown == out)
    {
      return out;
    }

    out = grown;
  }
}

bool CellGrid::connected(const Board &b) const
{
  uint32_t i = 0;
  uint32_t j = 0;

  if (!b.first(i, j))
  {
    // The empty set is connected, and saying otherwise makes every caller
    // check for it first.
    return true;
  }

  return flood(b, cell(int32_t(i), int32_t(j))) == b;
}

// ---------------------------------------------------------------------------
// Contours
// ---------------------------------------------------------------------------

std::vector<Chain> CellGrid::outline(const Board &area) const
{
  std::vector<Chain> loops;

  if (!valid() || area.empty())
  {
    return loops;
  }

  // Which boundary edges exist, indexed by (node, direction). An edge leaving a
  // node is determined by its direction, so this is a presence bit and not a
  // list, and the walk below never searches.
  const size_t slots = size_t(width_ + 1) * size_t(height_ + 1) * 4u;
  std::vector<uint8_t> loose(slots, 0);
  size_t remaining = 0;

  for (const auto &[i, j] : cells(area))
  {
    const uint32_t c = cell(int32_t(i), int32_t(j));

    for (const Dir d : kCardinal)
    {
      if (!contains(area, neighbour(c, d)))
      {
        const uint32_t e = cellEdge(c, d);

        if (e != kNoEdge && loose[e] == 0)
        {
          loose[e] = 1;
          ++remaining;
        }
      }
    }
  }

  while (remaining > 0)
  {
    uint32_t start = kNoEdge;

    for (uint32_t e = 0; e < slots; ++e)
    {
      if (loose[e] != 0)
      {
        start = e;
        break;
      }
    }

    if (start == kNoEdge)
    {
      break;
    }

    Chain loop;
    uint32_t current = start;
    loose[current] = 0;
    --remaining;
    loop.push_back(current);

    for (;;)
    {
      const uint32_t at = edgeEndNode(current);

      if (at == kNoNode)
      {
        break;
      }

      // Clockwise first, then straight on, then counter-clockwise. Preferring
      // the clockwise turn is what makes the walk hug the shape: at a pinch
      // where two cells meet only at a corner it takes the turn that keeps the
      // interior on its right, so the two lobes come out as one loop rather
      // than cutting across each other.
      const Edge d = decode(current);
      uint32_t next = kNoEdge;

      for (const Dir turn : {cw(d.dir), d.dir, ccw(d.dir)})
      {
        const uint32_t candidate = edge(at, turn);

        if (candidate != kNoEdge && loose[candidate] != 0)
        {
          next = candidate;
          break;
        }
      }

      if (next == kNoEdge)
      {
        break;
      }

      loose[next] = 0;
      --remaining;
      loop.push_back(next);
      current = next;

      if (edgeEndNode(current) == decode(loop.front()).node)
      {
        break;
      }
    }

    loops.push_back(std::move(loop));
  }

  // The outer ring first. It is the one enclosing the most area, and on a
  // lattice that is simply the longest loop except for shapes this generator
  // does not make, so it is measured rather than assumed.
  if (loops.size() > 1)
  {
    const auto enclosed = [this](const Chain &loop) {
      double twice = 0;

      for (const uint32_t e : loop)
      {
        const Edge d = decode(e);
        const uint32_t b = edgeEndNode(e);
        const double x0 = double(nodeColumn(d.node));
        const double y0 = double(nodeRow(d.node));
        const double x1 = double(nodeColumn(b));
        const double y1 = double(nodeRow(b));
        twice += x0 * y1 - x1 * y0;
      }

      return twice;
    };

    std::stable_sort(loops.begin(), loops.end(), [&](const Chain &a, const Chain &b) {
      return enclosed(a) > enclosed(b);
    });
  }

  return loops;
}

Chain CellGrid::outerLoop(const Board &area) const
{
  const std::vector<Chain> loops = outline(area);

  return loops.empty() ? Chain{} : loops.front();
}

Board CellGrid::contourArea(const Chain &contour) const
{
  Board out;

  if (!valid() || contour.empty())
  {
    return out;
  }

  const size_t slots = size_t(width_ + 1) * size_t(height_ + 1) * 4u;
  std::vector<uint8_t> wall(slots, 0);

  for (const uint32_t e : contour)
  {
    if (e < slots)
    {
      wall[e] = 1;
    }
  }

  const uint32_t seed = edgeCell(contour.front());

  if (seed == kNoCell)
  {
    return out;
  }

  std::vector<uint32_t> stack;
  stack.push_back(seed);
  add(out, seed);

  while (!stack.empty())
  {
    const uint32_t c = stack.back();
    stack.pop_back();

    for (const Dir d : kCardinal)
    {
      const uint32_t n = neighbour(c, d);

      if (n == kNoCell || contains(out, n))
      {
        continue;
      }

      const uint32_t crossing = cellEdge(c, d);

      if (crossing != kNoEdge && crossing < slots && wall[crossing] != 0)
      {
        continue;
      }

      add(out, n);
      stack.push_back(n);
    }
  }

  return out;
}

Board CellGrid::contourArea(const std::vector<Chain> &loops) const
{
  Board out;

  if (!valid() || loops.empty())
  {
    return out;
  }

  const size_t slots = size_t(width_ + 1) * size_t(height_ + 1) * 4u;
  std::vector<uint8_t> wall(slots, 0);

  for (const Chain &loop : loops)
  {
    for (const uint32_t e : loop)
    {
      if (e < slots)
      {
        wall[e] = 1;
      }
    }
  }

  const uint32_t seed = edgeCell(loops.front().front());

  if (seed == kNoCell)
  {
    return out;
  }

  std::vector<uint32_t> stack;
  stack.push_back(seed);
  add(out, seed);

  while (!stack.empty())
  {
    const uint32_t c = stack.back();
    stack.pop_back();

    for (const Dir d : kCardinal)
    {
      const uint32_t n = neighbour(c, d);

      if (n == kNoCell || contains(out, n))
      {
        continue;
      }

      const uint32_t crossing = cellEdge(c, d);

      if (crossing != kNoEdge && crossing < slots && wall[crossing] != 0)
      {
        continue;
      }

      add(out, n);
      stack.push_back(n);
    }
  }

  return out;
}

bool CellGrid::simpleOutline(const std::vector<Chain> &loops) const
{
  if (!valid())
  {
    return false;
  }

  std::vector<uint8_t> seen(size_t(width_ + 1) * size_t(height_ + 1), 0);

  for (const Chain &loop : loops)
  {
    for (const uint32_t e : loop)
    {
      const uint32_t n = decode(e).node;

      if (n == kNoNode || seen[n] != 0)
      {
        return false;
      }

      seen[n] = 1;
    }
  }

  return true;
}

Chain CellGrid::reverseChain(const Chain &chain) const
{
  Chain out;
  out.reserve(chain.size());

  for (size_t k = chain.size(); k-- > 0;)
  {
    out.push_back(twin(chain[k]));
  }

  return out;
}

// ---------------------------------------------------------------------------
// Chains
// ---------------------------------------------------------------------------

std::optional<size_t> findEdgeFrom(const CellGrid &grid, const Chain &chain, const uint32_t node)
{
  for (size_t k = 0; k < chain.size(); ++k)
  {
    if (grid.decode(chain[k]).node == node)
    {
      return k;
    }
  }

  return std::nullopt;
}

Chain sliceChain(const CellGrid &grid, const Chain &chain, const uint32_t fromNode, const uint32_t toNode)
{
  const std::optional<size_t> from = findEdgeFrom(grid, chain, fromNode);
  const std::optional<size_t> to = findEdgeFrom(grid, chain, toNode);

  if (!from.has_value() || !to.has_value())
  {
    return {};
  }

  Chain out;

  for (size_t k = *from; k != *to; k = (k + 1) % chain.size())
  {
    out.push_back(chain[k]);
  }

  return out;
}

size_t firstCorner(const CellGrid &grid, const Chain &chain)
{
  const size_t n = chain.size();

  for (size_t k = 0; k < n; ++k)
  {
    if (grid.decode(chain[k]).dir != grid.decode(chain[(k + n - 1) % n]).dir)
    {
      return k;
    }
  }

  return 0;
}

std::vector<uint32_t> mergeCollinear(const CellGrid &grid, const Chain &chain)
{
  std::vector<uint32_t> out;

  if (chain.empty())
  {
    return out;
  }

  const size_t n = chain.size();
  const size_t start = firstCorner(grid, chain);

  for (size_t step = 0; step < n; ++step)
  {
    const size_t k = (start + step) % n;
    const size_t previous = (k + n - 1) % n;

    if (grid.decode(chain[k]).dir != grid.decode(chain[previous]).dir)
    {
      out.push_back(grid.decode(chain[k]).node);
    }
  }

  return out;
}

bool isRectangle(const CellGrid &grid, const Chain &chain)
{
  return mergeCollinear(grid, chain).size() == 4;
}

// ---------------------------------------------------------------------------
// Metres
// ---------------------------------------------------------------------------

namespace {

void place(const CellFrame &frame, const double u, const double v, double &x, double &z)
{
  // V is the U axis turned a quarter left, so a plan drawn with `i` down the
  // page and `j` across it comes out the way it is read.
  x = frame.originX + frame.axisX * u - frame.axisZ * v;
  z = frame.originZ + frame.axisZ * u + frame.axisX * v;
}

} // namespace

void framePoint(const CellFrame &frame, const double u, const double v, double &x, double &z)
{
  place(frame, u * frame.cellMetres, v * frame.cellMetres, x, z);
}

uint32_t cellAtPoint(const CellGrid &grid, const CellFrame &frame, const double x, const double z)
{
  double ox = 0, oz = 0, ux = 0, uz = 0, vx = 0, vz = 0;
  framePoint(frame, 0, 0, ox, oz);
  framePoint(frame, 1, 0, ux, uz);
  framePoint(frame, 0, 1, vx, vz);
  ux -= ox;
  uz -= oz;
  vx -= ox;
  vz -= oz;

  const double det = ux * vz - vx * uz;

  if (std::abs(det) < 1e-12)
  {
    return kNoCell;
  }

  const double dx = x - ox;
  const double dz = z - oz;
  const double u = (dx * vz - vx * dz) / det;
  const double v = (ux * dz - dx * uz) / det;

  if (u < 0 || v < 0)
  {
    return kNoCell;
  }

  const int32_t j = int32_t(std::floor(u));
  const int32_t i = int32_t(std::floor(v));

  if (i >= int32_t(grid.height()) || j >= int32_t(grid.width()))
  {
    return kNoCell;
  }

  return grid.cell(i, j);
}

void nodePoint(const CellGrid &grid, const CellFrame &frame, const uint32_t n, double &x, double &z)
{
  place(frame, double(grid.nodeColumn(n)) * frame.cellMetres, double(grid.nodeRow(n)) * frame.cellMetres, x, z);
}

void edgePoint(const CellGrid &grid, const CellFrame &frame, const uint32_t e, const double t, double &x, double &z)
{
  const Edge d = grid.decode(e);
  const uint32_t b = grid.edgeEndNode(e);
  const double u0 = double(grid.nodeColumn(d.node));
  const double v0 = double(grid.nodeRow(d.node));
  const double u1 = double(grid.nodeColumn(b));
  const double v1 = double(grid.nodeRow(b));

  place(frame, (u0 + (u1 - u0) * t) * frame.cellMetres, (v0 + (v1 - v0) * t) * frame.cellMetres, x, z);
}

void cellPoint(const CellGrid &grid, const CellFrame &frame, const uint32_t c, double &x, double &z)
{
  place(frame,
        (double(grid.cellColumn(c)) + .5) * frame.cellMetres,
        (double(grid.cellRow(c)) + .5) * frame.cellMetres,
        x,
        z);
}

bool ringOf(const CellGrid &grid, const CellFrame &frame, const Chain &chain, Ring &out, std::string *error)
{
  out.clear();

  const std::vector<uint32_t> corners = mergeCollinear(grid, chain);

  if (corners.size() < 3)
  {
    return fail(error, "contour has fewer than three corners");
  }

  out.reserve(corners.size());

  for (const uint32_t n : corners)
  {
    double x = 0;
    double z = 0;
    nodePoint(grid, frame, n, x, z);

    if (!representable(x) || !representable(z))
    {
      return fail(error, "contour leaves the representable grid");
    }

    out.push_back(Point(toUnit(x), toUnit(z)));
  }

  return true;
}

bool polygonOf(const CellGrid &grid, const CellFrame &frame, const Board &area, Polygon &out, std::string *error)
{
  out.outer.clear();
  out.holes.clear();

  const std::vector<Chain> loops = grid.outline(area);

  if (loops.empty())
  {
    return fail(error, "area has no boundary");
  }

  if (!grid.simpleOutline(loops))
  {
    return fail(error, "area pinches: its boundary touches itself at a corner");
  }

  if (!ringOf(grid, frame, loops.front(), out.outer, error))
  {
    return false;
  }

  if (!counterClockwise(out.outer))
  {
    reverse(out.outer);
  }

  for (size_t k = 1; k < loops.size(); ++k)
  {
    Ring hole;

    if (!ringOf(grid, frame, loops[k], hole, error))
    {
      return false;
    }

    if (counterClockwise(hole))
    {
      reverse(hole);
    }

    out.holes.push_back(std::move(hole));
  }

  return true;
}

} // namespace procedural::geom2
