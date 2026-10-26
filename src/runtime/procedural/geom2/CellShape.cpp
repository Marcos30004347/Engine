#include "procedural/geom2/CellShape.hpp"

#include <vector>

namespace procedural::geom2 {

CellBounds bounds(const Board &b)
{
  CellBounds out;
  bool any = false;

  for (uint32_t i = 0; i < Board::kMaxRows; ++i)
  {
    if (b.row[i] == 0)
    {
      continue;
    }

    const uint32_t low = uint32_t(__builtin_ctzll(b.row[i]));
    const uint32_t high = 63u - uint32_t(__builtin_clzll(b.row[i]));

    if (!any)
    {
      out.i0 = i;
      out.j0 = low;
      out.j1 = high;
      any = true;
    }
    else
    {
      out.j0 = out.j0 < low ? out.j0 : low;
      out.j1 = out.j1 > high ? out.j1 : high;
    }

    out.i1 = i;
  }

  if (!any)
  {
    return CellBounds{1, 1, 0, 0};
  }

  return out;
}

double compactness(const Board &b)
{
  const CellBounds box = bounds(b);

  if (box.empty())
  {
    return 0;
  }

  return double(b.count()) / (double(box.rows()) * double(box.columns()));
}

Board narrow(const CellGrid &grid, const Board &area)
{
  // A cell is wide exactly when some two-by-two block of the area contains it.
  // `full` marks the north-west corner of every such block; a cell is then wide
  // if it is one of those corners or lies one step south, east, or south-east
  // of one.
  const Board full = area & grid.shifted(area, Dir::East) & grid.shifted(area, Dir::South) &
                     grid.shifted(grid.shifted(area, Dir::South), Dir::East);

  const Board wide = full | grid.shifted(full, Dir::West) | grid.shifted(full, Dir::North) |
                     grid.shifted(grid.shifted(full, Dir::North), Dir::West);

  return area & ~wide;
}

bool removalKeepsConnected(const CellGrid &grid, const Board &area, const uint32_t cell)
{
  if (!grid.contains(area, cell))
  {
    return false;
  }

  Board rest = area;
  grid.remove(rest, cell);

  return grid.connected(rest);
}

bool largestRectangle(const CellGrid &grid, const Board &area, CellBounds &out)
{
  out = CellBounds{1, 1, 0, 0};

  if (!grid.valid() || area.empty())
  {
    return false;
  }

  const uint32_t w = grid.width();
  const uint32_t h = grid.height();

  // Heights of the run of occupied cells ending at each column of the current
  // row, then the classic stack pass over that histogram.
  std::vector<uint32_t> heights(w, 0);
  std::vector<uint32_t> stack;
  uint32_t best = 0;

  for (uint32_t i = 0; i < h; ++i)
  {
    for (uint32_t j = 0; j < w; ++j)
    {
      heights[j] = area.test(i, j) ? heights[j] + 1 : 0;
    }

    stack.clear();

    for (uint32_t j = 0; j <= w; ++j)
    {
      const uint32_t height = j < w ? heights[j] : 0;

      while (!stack.empty() && heights[stack.back()] >= height)
      {
        const uint32_t top = stack.back();
        stack.pop_back();
        const uint32_t left = stack.empty() ? 0 : stack.back() + 1;
        const uint32_t span = j - left;
        const uint32_t tall = heights[top];
        const uint32_t area2 = span * tall;

        if (area2 > best && tall > 0)
        {
          best = area2;
          out.i0 = i + 1 - tall;
          out.i1 = i;
          out.j0 = left;
          out.j1 = j - 1;
        }
      }

      stack.push_back(j);
    }
  }

  return best > 0;
}

} // namespace procedural::geom2
