#include "procedural/world/generator/FloorPlanOptimiser.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace procedural::world::generator {

namespace {

constexpr int32_t kOutside = -1;

struct Rng
{
  uint64_t state;

  explicit Rng(const uint64_t seed) : state(seed ^ 0x853C49E6748FEA9Bull) {}

  uint64_t next()
  {
    state += 0x9E3779B97F4A7C15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;

    return z ^ (z >> 31);
  }

  double unit() { return double(next() >> 11) * 0x1.0p-53; }
  uint32_t below(const uint32_t n) { return n == 0 ? 0 : uint32_t(next() % n); }

  // Box-Muller, for the slide distance d ~ N(0, sigma^2).
  double normal()
  {
    const double u = std::max(unit(), 1e-12);
    const double v = unit();

    return std::sqrt(-2. * std::log(u)) * std::cos(6.283185307179586 * v);
  }
};

// One floor, as a grid of labelled cells.
//
// The grid lines are the wall coordinates. A room is the set of cells carrying
// its label, so it is rectilinear however the lines fall, and sliding a wall is
// inserting a line and relabelling the cells between the old line and the new.
struct Grid
{
  std::vector<double> xs, ys;
  std::vector<int32_t> label;  // (xs.size()-1) by (ys.size()-1), row-major in x.

  uint32_t columns() const { return uint32_t(xs.size()) - 1; }
  uint32_t rows() const { return uint32_t(ys.size()) - 1; }

  int32_t at(const uint32_t i, const uint32_t j) const
  {
    return label[size_t(j) * columns() + i];
  }

  void set(const uint32_t i, const uint32_t j, const int32_t value)
  {
    label[size_t(j) * columns() + i] = value;
  }

  double width(const uint32_t i) const { return xs[i + 1] - xs[i]; }
  double height(const uint32_t j) const { return ys[j + 1] - ys[j]; }
  double cellArea(const uint32_t i, const uint32_t j) const { return width(i) * height(j); }

  // Inserts a coordinate, splitting whichever cell column or row contains it,
  // and returns the index of the line. Existing lines within `epsilon` are
  // reused rather than duplicated - the grid's own resolution.
  uint32_t insertX(const double x, const double epsilon)
  {
    for (uint32_t i = 0; i < xs.size(); ++i)
    {
      if (std::abs(xs[i] - x) <= epsilon)
      {
        return i;
      }
    }

    const auto where = std::lower_bound(xs.begin(), xs.end(), x);
    const uint32_t index = uint32_t(where - xs.begin());

    // Beyond either end the arrangement *grows*, with the new column outside.
    //
    // Refusing to - which is what this did - is the difference between a plan
    // that can take the ground it is standing on and one that can only ever
    // subdivide the box it started in. No outer wall could move outward, so
    // the building's outline was decided by the canonical start and every
    // iteration after it only rearranged the inside.
    if (index == 0 || index >= xs.size())
    {
      const bool low = index == 0;
      std::vector<int32_t> widened(size_t(columns() + 1) * rows(), kOutside);

      for (uint32_t j = 0; j < rows(); ++j)
      {
        for (uint32_t i = 0; i < columns(); ++i)
        {
          widened[size_t(j) * (columns() + 1) + (low ? i + 1 : i)] = at(i, j);
        }
      }

      xs.insert(low ? xs.begin() : xs.end(), x);
      label = std::move(widened);

      return low ? 0u : uint32_t(xs.size()) - 1;
    }

    // The column being split is index-1; every row gains a copy of its label.
    std::vector<int32_t> grown(size_t(columns() + 1) * rows());

    for (uint32_t j = 0; j < rows(); ++j)
    {
      for (uint32_t i = 0; i < columns(); ++i)
      {
        const uint32_t target = i < index - 1 ? i : i + 1;
        grown[size_t(j) * (columns() + 1) + target] = at(i, j);
      }

      grown[size_t(j) * (columns() + 1) + (index - 1)] = at(index - 1, j);
    }

    xs.insert(where, x);
    label = std::move(grown);

    return index;
  }

  uint32_t insertY(const double y, const double epsilon)
  {
    for (uint32_t j = 0; j < ys.size(); ++j)
    {
      if (std::abs(ys[j] - y) <= epsilon)
      {
        return j;
      }
    }

    const auto where = std::lower_bound(ys.begin(), ys.end(), y);
    const uint32_t index = uint32_t(where - ys.begin());

    // As insertX: beyond either end the arrangement grows, outside-first.
    if (index == 0 || index >= ys.size())
    {
      const bool low = index == 0;
      const uint32_t across = columns();
      std::vector<int32_t> taller(size_t(across) * (rows() + 1), kOutside);

      for (uint32_t j = 0; j < rows(); ++j)
      {
        for (uint32_t i = 0; i < across; ++i)
        {
          taller[size_t(low ? j + 1 : j) * across + i] = at(i, j);
        }
      }

      ys.insert(low ? ys.begin() : ys.end(), y);
      label = std::move(taller);

      return low ? 0u : uint32_t(ys.size()) - 1;
    }

    const uint32_t wide = columns();
    std::vector<int32_t> grown(size_t(wide) * (rows() + 1));

    for (uint32_t j = 0; j < rows(); ++j)
    {
      const uint32_t target = j < index - 1 ? j : j + 1;

      for (uint32_t i = 0; i < wide; ++i)
      {
        grown[size_t(target) * wide + i] = at(i, j);
      }
    }

    // The row being split is index-1; both halves keep its labels.
    for (uint32_t i = 0; i < wide; ++i)
    {
      grown[size_t(index - 1) * wide + i] = at(i, index - 1);
    }

    ys.insert(where, y);
    label = std::move(grown);

    return index;
  }
};

// Drop every grid line that no wall stands on.
//
// The paper's state is "a set of n rooms, each a rectilinear polygon enclosed
// by wall segments" - not a grid. A labelled cell complex over the arrangement
// of the *actual* wall lines is exactly that set of polygons, stored so that a
// slide is a line insertion rather than a polygon repair. What it is not is a
// mesh, and without this it becomes one: a slide inserts a line spanning the
// whole floor, so every move leaves behind a line that only one wall ever
// used, and after a few thousand accepted moves the arrangement is far finer
// than the layout it encodes. Measured before this existed: ninety-five lines
// and a thousand cells to hold a layout with eighty-six corners in it.
//
// A line with the same label on both sides everywhere is not a wall, so it is
// not part of the state.
void coalesce(Grid &grid)
{
  for (uint32_t i = grid.columns(); i-- > 1;)
  {
    bool wall = false;

    for (uint32_t j = 0; j < grid.rows() && !wall; ++j)
    {
      wall = grid.at(i - 1, j) != grid.at(i, j);
    }

    if (wall)
    {
      continue;
    }

    // Column i is a copy of column i-1; drop the line between them.
    std::vector<int32_t> shrunk(size_t(grid.columns() - 1) * grid.rows());

    for (uint32_t j = 0; j < grid.rows(); ++j)
    {
      for (uint32_t k = 0, target = 0; k < grid.columns(); ++k)
      {
        if (k == i)
        {
          continue;
        }

        shrunk[size_t(j) * (grid.columns() - 1) + target++] = grid.at(k, j);
      }
    }

    grid.xs.erase(grid.xs.begin() + long(i));
    grid.label = std::move(shrunk);
  }

  for (uint32_t j = grid.rows(); j-- > 1;)
  {
    bool wall = false;

    for (uint32_t i = 0; i < grid.columns() && !wall; ++i)
    {
      wall = grid.at(i, j - 1) != grid.at(i, j);
    }

    if (wall)
    {
      continue;
    }

    std::vector<int32_t> shrunk(size_t(grid.columns()) * (grid.rows() - 1));

    for (uint32_t k = 0, target = 0; k < grid.rows(); ++k)
    {
      if (k == j)
      {
        continue;
      }

      for (uint32_t i = 0; i < grid.columns(); ++i)
      {
        shrunk[size_t(target) * grid.columns() + i] = grid.at(i, k);
      }

      ++target;
    }

    grid.ys.erase(grid.ys.begin() + long(j));
    grid.label = std::move(shrunk);
  }
}

// Everything the optimiser carries between iterations.
struct State
{
  std::vector<int32_t> levels;       // Programme floor level per grid.
  std::vector<Grid> grids;           // One per level, in `levels` order.
  std::vector<uint32_t> roomFloor;   // Grid index per room.
  std::vector<uint32_t> roomOfLabel; // Label is the room index directly.
};

double areaOf(const Grid &grid, const int32_t label)
{
  double area = 0;

  for (uint32_t j = 0; j < grid.rows(); ++j)
  {
    for (uint32_t i = 0; i < grid.columns(); ++i)
    {
      if (grid.at(i, j) == label)
      {
        area += grid.cellArea(i, j);
      }
    }
  }

  return area;
}

// The bounding rectangle of a label, which is what the paper's aspect ratio is
// measured on: "aspect ratio of its bounding rectangle".
bool boundsOf(const Grid &grid, const int32_t label, double &lowX, double &highX,
              double &lowY, double &highY)
{
  bool any = false;
  lowX = lowY = 1e30;
  highX = highY = -1e30;

  for (uint32_t j = 0; j < grid.rows(); ++j)
  {
    for (uint32_t i = 0; i < grid.columns(); ++i)
    {
      if (grid.at(i, j) != label)
      {
        continue;
      }

      any = true;
      lowX = std::min(lowX, grid.xs[i]);
      highX = std::max(highX, grid.xs[i + 1]);
      lowY = std::min(lowY, grid.ys[j]);
      highY = std::max(highY, grid.ys[j + 1]);
    }
  }

  return any;
}

// Total length of wall shared by two labels, and the longest single run of it.
//
// The run's extent and not just its middle, because §6 places a door where it
// wants to be along the wall rather than in the middle of it.
struct WallRun
{
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  double length = 0;
};

double sharedWall(const Grid &grid, const int32_t a, const int32_t b, WallRun &longest)
{
  double total = 0;
  longest = WallRun{};

  const auto consider = [&](const double length, const double x0, const double y0,
                            const double x1, const double y1) {
    total += length;

    if (length > longest.length)
    {
      longest = {x0, y0, x1, y1, length};
    }
  };

  // Vertical walls: between column i-1 and i.
  for (uint32_t i = 1; i < grid.columns(); ++i)
  {
    double run = 0, runStart = 0;

    for (uint32_t j = 0; j <= grid.rows(); ++j)
    {
      const bool touching =
        j < grid.rows()
        && ((grid.at(i - 1, j) == a && grid.at(i, j) == b)
            || (grid.at(i - 1, j) == b && grid.at(i, j) == a));

      if (touching)
      {
        runStart = run == 0 ? grid.ys[j] : runStart;
        run += grid.height(j);

        continue;
      }

      if (run > 0)
      {
        consider(run, grid.xs[i], runStart, grid.xs[i], runStart + run);
        run = 0;
      }
    }
  }

  // Horizontal walls: between row j-1 and j.
  for (uint32_t j = 1; j < grid.rows(); ++j)
  {
    double run = 0, runStart = 0;

    for (uint32_t i = 0; i <= grid.columns(); ++i)
    {
      const bool touching =
        i < grid.columns()
        && ((grid.at(i, j - 1) == a && grid.at(i, j) == b)
            || (grid.at(i, j - 1) == b && grid.at(i, j) == a));

      if (touching)
      {
        runStart = run == 0 ? grid.xs[i] : runStart;
        run += grid.width(i);

        continue;
      }

      if (run > 0)
      {
        consider(run, runStart, grid.ys[j], runStart + run, grid.ys[j]);
        run = 0;
      }
    }
  }

  return total;
}

// Edges along the outline of a label: the number of maximal collinear boundary
// runs, which is the paper's e(S).
uint32_t outlineEdges(const Grid &grid, const int32_t label)
{
  uint32_t edges = 0;

  const auto isLabel = [&](const int32_t i, const int32_t j) {
    return i >= 0 && j >= 0 && i < int32_t(grid.columns()) && j < int32_t(grid.rows())
           && grid.at(uint32_t(i), uint32_t(j)) == label;
  };

  for (uint32_t i = 0; i <= grid.columns(); ++i)
  {
    bool inRun = false;

    for (uint32_t j = 0; j < grid.rows(); ++j)
    {
      const bool left = isLabel(int32_t(i) - 1, int32_t(j));
      const bool right = isLabel(int32_t(i), int32_t(j));
      const bool boundary = left != right;

      edges += boundary && !inRun ? 1 : 0;
      inRun = boundary;
    }
  }

  for (uint32_t j = 0; j <= grid.rows(); ++j)
  {
    bool inRun = false;

    for (uint32_t i = 0; i < grid.columns(); ++i)
    {
      const bool below = isLabel(int32_t(i), int32_t(j) - 1);
      const bool above = isLabel(int32_t(i), int32_t(j));
      const bool boundary = below != above;

      edges += boundary && !inRun ? 1 : 0;
      inRun = boundary;
    }
  }

  return edges;
}

// Area of the convex hull of a label's cells, for the paper's Mc(S).
double hullArea(const Grid &grid, const int32_t label)
{
  std::vector<std::pair<double, double>> points;

  for (uint32_t j = 0; j < grid.rows(); ++j)
  {
    for (uint32_t i = 0; i < grid.columns(); ++i)
    {
      if (grid.at(i, j) != label)
      {
        continue;
      }

      points.emplace_back(grid.xs[i], grid.ys[j]);
      points.emplace_back(grid.xs[i + 1], grid.ys[j]);
      points.emplace_back(grid.xs[i], grid.ys[j + 1]);
      points.emplace_back(grid.xs[i + 1], grid.ys[j + 1]);
    }
  }

  if (points.size() < 3)
  {
    return 0;
  }

  std::sort(points.begin(), points.end());
  points.erase(std::unique(points.begin(), points.end()), points.end());

  const auto cross = [](const std::pair<double, double> &o,
                        const std::pair<double, double> &a,
                        const std::pair<double, double> &b) {
    return (a.first - o.first) * (b.second - o.second)
           - (a.second - o.second) * (b.first - o.first);
  };

  std::vector<std::pair<double, double>> hull(points.size() * 2);
  size_t k = 0;

  for (size_t i = 0; i < points.size(); ++i)
  {
    while (k >= 2 && cross(hull[k - 2], hull[k - 1], points[i]) <= 0)
    {
      --k;
    }

    hull[k++] = points[i];
  }

  for (size_t i = points.size() - 1, t = k + 1; i > 0; --i)
  {
    while (k >= t && cross(hull[k - 2], hull[k - 1], points[i - 1]) <= 0)
    {
      --k;
    }

    hull[k++] = points[i - 1];
  }

  hull.resize(k > 0 ? k - 1 : 0);

  double twice = 0;

  for (size_t i = 0; i < hull.size(); ++i)
  {
    const auto &a = hull[i];
    const auto &b = hull[(i + 1) % hull.size()];
    twice += a.first * b.second - b.first * a.second;
  }

  return std::abs(twice) * .5;
}


// Is every cell of a label reachable from every other, moving between cells
// that share an edge?
//
// The paper says a room is a rectilinear polygon, and a polygon is connected.
// A slide or a swap can cut a room in two, and then the outline tracer follows
// one piece and reports the other's area as well - which is how this was found.
bool connected(const Grid &grid, const int32_t label)
{
  const uint32_t columns = grid.columns();
  const uint32_t rows = grid.rows();

  uint32_t total = 0;
  uint32_t startI = 0, startJ = 0;
  bool found = false;

  for (uint32_t j = 0; j < rows; ++j)
  {
    for (uint32_t i = 0; i < columns; ++i)
    {
      if (grid.at(i, j) != label)
      {
        continue;
      }

      ++total;

      if (!found)
      {
        startI = i;
        startJ = j;
        found = true;
      }
    }
  }

  if (!found)
  {
    return true;
  }

  std::vector<uint8_t> seen(size_t(columns) * rows, 0);
  std::vector<std::pair<uint32_t, uint32_t>> stack{{startI, startJ}};
  seen[size_t(startJ) * columns + startI] = 1;
  uint32_t reached = 1;

  while (!stack.empty())
  {
    const auto cell = stack.back();
    stack.pop_back();

    const int32_t offsets[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

    for (const auto &offset : offsets)
    {
      const int32_t ni = int32_t(cell.first) + offset[0];
      const int32_t nj = int32_t(cell.second) + offset[1];

      if (ni < 0 || nj < 0 || ni >= int32_t(columns) || nj >= int32_t(rows))
      {
        continue;
      }

      const size_t index = size_t(nj) * columns + size_t(ni);

      if (seen[index] || grid.at(uint32_t(ni), uint32_t(nj)) != label)
      {
        continue;
      }

      seen[index] = 1;
      ++reached;
      stack.emplace_back(uint32_t(ni), uint32_t(nj));
    }
  }

  return reached == total;
}

// Does the label's boundary touch itself at a vertex?
//
// Two cells of one room meeting only at a corner make a shape that is
// connected in the sense of "you can get there" and is not a simple polygon:
// its boundary passes through that vertex twice, so an outline tracer keyed on
// the vertex has two outgoing edges to choose from and takes the wrong one.
// The paper's rooms are rectilinear *polygons*, and a polygon does not pinch,
// so this is a layout to reject rather than a tracer to make cleverer.
//
// Coalescing made these common - fewer, larger cells means more chances for
// two of them to meet at a corner - which is how it was found.
// How wide a room is where it is narrowest, near enough.
//
// Twice the area over the perimeter: for a w by h rectangle that is wh/(w+h),
// which is w to within a few per cent when h is much the larger, and half the
// side of a square. It is not the largest inscribed circle - that would be the
// exact answer and cost a distance transform - but it separates a passage you
// can walk down from a gap between two walls, which is what it is for.

// A room somebody is in, as opposed to one they reach into.
//
// The distinction is width, not area. A larder two metres by one is a larder; a
// bedroom two metres by one is a corridor with a bed in it. So the rooms people
// occupy carry a wider floor than the general one, and the rooms that are
// fittings - a closet, a privy, a scullery, a store - do not, because being
// small is what they are for.
bool habitable(const RoomUse use)
{
  switch (use)
  {
  case RoomUse::Kitchen:
  case RoomUse::Parlour:
  case RoomUse::LivingRoom:
  case RoomUse::DiningRoom:
  case RoomUse::DrawingRoom:
  case RoomUse::BreakfastRoom:
  case RoomUse::Study:
  case RoomUse::Library:
  case RoomUse::Bedroom:
  case RoomUse::Nursery:
  case RoomUse::Office:
  case RoomUse::Shop:
  case RoomUse::Workshop:
  case RoomUse::MorningRoom:
  case RoomUse::Boudoir:
  case RoomUse::Conservatory:
  case RoomUse::ServantsHall:
  case RoomUse::Bar:
  case RoomUse::TapRoom:
  case RoomUse::ClubRoom:
  case RoomUse::CoffeeRoom:
  case RoomUse::BilliardRoom:
  case RoomUse::SchoolRoom:
  case RoomUse::Dispensary:
  case RoomUse::WaitingRoom:
  case RoomUse::ConsultingRoom:
  case RoomUse::Nave:
  case RoomUse::Chancel:
  case RoomUse::Transept:
  case RoomUse::Sanctuary:
  case RoomUse::SideChapel:
  case RoomUse::ChapterHouse:
  case RoomUse::Refectory:
  case RoomUse::Dormitory:
  case RoomUse::GreatHall:
  case RoomUse::Solar:
  case RoomUse::Guardroom:
  case RoomUse::Barracks:
  case RoomUse::Cell:
    return true;

  default:
    return false;
  }
}

double widthOf(const Grid &grid, const int32_t label)
{
  double area = 0, perimeter = 0;

  const auto at = [&](const int32_t i, const int32_t j) {
    return i >= 0 && j >= 0 && i < int32_t(grid.columns()) && j < int32_t(grid.rows())
             ? grid.at(uint32_t(i), uint32_t(j))
             : kOutside;
  };

  for (uint32_t j = 0; j < grid.rows(); ++j)
  {
    for (uint32_t i = 0; i < grid.columns(); ++i)
    {
      if (grid.at(i, j) != label)
      {
        continue;
      }

      const double wide = grid.xs[i + 1] - grid.xs[i];
      const double tall = grid.ys[j + 1] - grid.ys[j];
      area += wide * tall;

      perimeter += at(int32_t(i) - 1, int32_t(j)) == label ? 0. : tall;
      perimeter += at(int32_t(i) + 1, int32_t(j)) == label ? 0. : tall;
      perimeter += at(int32_t(i), int32_t(j) - 1) == label ? 0. : wide;
      perimeter += at(int32_t(i), int32_t(j) + 1) == label ? 0. : wide;
    }
  }

  return perimeter > 1e-9 ? 2. * area / perimeter : 0.;
}

// Whether everything on this floor is one piece.
//
// Each room is kept connected and each is a polygon, and that is not the same
// as the floor being one building: two groups of rooms can drift apart and
// leave a gap, and nothing in the cost function objects because every room is
// still a fine room. What it produces downstream is worse than a bad plan. The
// storey plate is traced as the largest loop of the floor's boundary, so the
// smaller piece loses its walls entirely, and the roof built over "the plate
// minus the floor above" then covers the ground where that piece stands - a
// detached box with a roof under it, floating beside the house.
bool floorConnected(const Grid &grid)
{
  const uint32_t columns = grid.columns(), rows = grid.rows();
  std::vector<uint8_t> seen(size_t(columns) * rows, 0);
  std::vector<std::pair<uint32_t, uint32_t>> stack;

  for (uint32_t j = 0; j < rows && stack.empty(); ++j)
  {
    for (uint32_t i = 0; i < columns && stack.empty(); ++i)
    {
      if (grid.at(i, j) != kOutside)
      {
        seen[size_t(j) * columns + i] = 1;
        stack.push_back({i, j});
      }
    }
  }

  if (stack.empty())
  {
    return true;
  }

  const auto push = [&](const uint32_t i, const uint32_t j) {
    if (grid.at(i, j) == kOutside || seen[size_t(j) * columns + i] != 0)
    {
      return;
    }

    seen[size_t(j) * columns + i] = 1;
    stack.push_back({i, j});
  };

  while (!stack.empty())
  {
    const auto [i, j] = stack.back();
    stack.pop_back();

    if (i > 0)
    {
      push(i - 1, j);
    }

    if (i + 1 < columns)
    {
      push(i + 1, j);
    }

    if (j > 0)
    {
      push(i, j - 1);
    }

    if (j + 1 < rows)
    {
      push(i, j + 1);
    }
  }

  for (uint32_t j = 0; j < rows; ++j)
  {
    for (uint32_t i = 0; i < columns; ++i)
    {
      if (grid.at(i, j) != kOutside && seen[size_t(j) * columns + i] == 0)
      {
        return false;
      }
    }
  }

  return true;
}

// Whether the floor touches itself at a corner.
//
// Two parts of a floor meeting corner to corner are connected - you can get
// from one to the other, diagonally, through a point - and every room in both
// is a fine room. The boundary of that floor is a figure of eight, and the
// trace that turns it into a storey plate returns a ring that crosses itself.
// Extruded into walls, a self-crossing ring is a slab through the middle of
// the building: the grey planes cutting across the upper storeys.
//
// Same test as `pinched`, applied to the floor rather than to one room.
bool floorPinched(const Grid &grid)
{
  for (uint32_t j = 0; j + 1 < grid.rows(); ++j)
  {
    for (uint32_t i = 0; i + 1 < grid.columns(); ++i)
    {
      const bool a = grid.at(i, j) != kOutside;
      const bool b = grid.at(i + 1, j) != kOutside;
      const bool c = grid.at(i, j + 1) != kOutside;
      const bool d = grid.at(i + 1, j + 1) != kOutside;

      if ((a && d && !b && !c) || (b && c && !a && !d))
      {
        return true;
      }
    }
  }

  return false;
}

// Whether a room wraps all the way around something else.
//
// A rectilinear polygon has no holes, and a room drawn as one - traced from
// its outer boundary - would be exported swallowing whatever it encloses: two
// rooms occupying the same ground, which is not a plan of anything. Found by
// flooding everything that is not this room inward from the border: what the
// flood cannot reach is what the room has closed around.
bool encloses(const Grid &grid, const int32_t label)
{
  const uint32_t columns = grid.columns(), rows = grid.rows();

  if (columns == 0 || rows == 0)
  {
    return false;
  }

  std::vector<uint8_t> seen(size_t(columns) * rows, 0);
  std::vector<std::pair<uint32_t, uint32_t>> stack;

  const auto push = [&](const uint32_t i, const uint32_t j) {
    if (grid.at(i, j) == label || seen[size_t(j) * columns + i] != 0)
    {
      return;
    }

    seen[size_t(j) * columns + i] = 1;
    stack.push_back({i, j});
  };

  for (uint32_t i = 0; i < columns; ++i)
  {
    push(i, 0);
    push(i, rows - 1);
  }

  for (uint32_t j = 0; j < rows; ++j)
  {
    push(0, j);
    push(columns - 1, j);
  }

  while (!stack.empty())
  {
    const auto [i, j] = stack.back();
    stack.pop_back();

    if (i > 0)
    {
      push(i - 1, j);
    }

    if (i + 1 < columns)
    {
      push(i + 1, j);
    }

    if (j > 0)
    {
      push(i, j - 1);
    }

    if (j + 1 < rows)
    {
      push(i, j + 1);
    }
  }

  for (uint32_t j = 0; j < rows; ++j)
  {
    for (uint32_t i = 0; i < columns; ++i)
    {
      if (grid.at(i, j) != label && seen[size_t(j) * columns + i] == 0)
      {
        return true;
      }
    }
  }

  return false;
}

bool pinched(const Grid &grid, const int32_t label)
{
  const auto isLabel = [&](const int32_t i, const int32_t j) {
    return i >= 0 && j >= 0 && i < int32_t(grid.columns()) && j < int32_t(grid.rows())
           && grid.at(uint32_t(i), uint32_t(j)) == label;
  };

  for (uint32_t i = 1; i < grid.columns(); ++i)
  {
    for (uint32_t j = 1; j < grid.rows(); ++j)
    {
      const bool lowerLeft = isLabel(int32_t(i) - 1, int32_t(j) - 1);
      const bool lowerRight = isLabel(int32_t(i), int32_t(j) - 1);
      const bool upperLeft = isLabel(int32_t(i) - 1, int32_t(j));
      const bool upperRight = isLabel(int32_t(i), int32_t(j));

      const bool oneDiagonal = lowerLeft && upperRight && !lowerRight && !upperLeft;
      const bool otherDiagonal = lowerRight && upperLeft && !lowerLeft && !upperRight;

      if (oneDiagonal || otherDiagonal)
      {
        return true;
      }
    }
  }

  return false;
}

// Trace the rectilinear outline of a label: every boundary edge, directed so
// the room is on the left, chained head to tail. Collinear runs are merged so
// the result is the room's corners and not its cells.
// Every loop of the boundary of `label`, largest first.
//
// A floor that wraps round a court has two: its outer wall and the wall of the
// court. Keeping only the largest is what left a guild hall and a temple with
// a hole in the middle and no wall around it - the plate had no inner ring, so
// nothing was extruded there and the roof was laid straight over the court.
std::vector<std::vector<std::pair<double, double>>> traceLoops(const Grid &grid,
                                                               int32_t label);

std::vector<std::pair<double, double>> traceOutline(const Grid &grid, const int32_t label)
{
  const auto isLabel = [&](const int32_t i, const int32_t j) {
    return i >= 0 && j >= 0 && i < int32_t(grid.columns()) && j < int32_t(grid.rows())
           && grid.at(uint32_t(i), uint32_t(j)) == label;
  };

  using Corner = std::pair<double, double>;
  std::map<Corner, Corner> next;

  for (uint32_t j = 0; j < grid.rows(); ++j)
  {
    for (uint32_t i = 0; i < grid.columns(); ++i)
    {
      if (!isLabel(int32_t(i), int32_t(j)))
      {
        continue;
      }

      const double x0 = grid.xs[i], x1 = grid.xs[i + 1];
      const double y0 = grid.ys[j], y1 = grid.ys[j + 1];

      // Counter clockwise around a cell, keeping the interior on the left.
      if (!isLabel(int32_t(i), int32_t(j) - 1))
      {
        next[{x0, y0}] = {x1, y0};
      }

      if (!isLabel(int32_t(i) + 1, int32_t(j)))
      {
        next[{x1, y0}] = {x1, y1};
      }

      if (!isLabel(int32_t(i), int32_t(j) + 1))
      {
        next[{x1, y1}] = {x0, y1};
      }

      if (!isLabel(int32_t(i) - 1, int32_t(j)))
      {
        next[{x0, y1}] = {x0, y0};
      }
    }
  }

  if (next.empty())
  {
    return {};
  }

  // The boundary is a set of loops, not one loop: a floor whose rooms have
  // come apart has two, and a floor wrapped around a light well has an inner
  // one. Walk them all and keep the largest, which is the body of the thing.
  // Taking the first loop in scan order instead silently returns a courtyard
  // or a detached corner and reports it as the building.
  std::vector<Corner> ring;
  double bestArea = -1;

  std::map<Corner, bool> walked;

  for (const auto &entry : next)
  {
    if (walked[entry.first])
    {
      continue;
    }

    std::vector<Corner> loop;
    const Corner start = entry.first;
    Corner at = start;

    for (size_t guard = 0; guard < next.size() + 2; ++guard)
    {
      loop.push_back(at);
      walked[at] = true;

      const auto step = next.find(at);

      if (step == next.end())
      {
        break;
      }

      at = step->second;

      if (at == start)
      {
        break;
      }
    }

    double twice = 0;

    for (size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++)
    {
      twice += loop[j].first * loop[i].second - loop[i].first * loop[j].second;
    }

    if (std::abs(twice) * .5 > bestArea)
    {
      bestArea = std::abs(twice) * .5;
      ring = std::move(loop);
    }
  }

  // Drop the middle of every collinear run: a room's outline is its corners.
  std::vector<Corner> corners;

  for (size_t i = 0; i < ring.size(); ++i)
  {
    const Corner &a = ring[(i + ring.size() - 1) % ring.size()];
    const Corner &b = ring[i];
    const Corner &c = ring[(i + 1) % ring.size()];

    const double cross = (b.first - a.first) * (c.second - b.second)
                         - (b.second - a.second) * (c.first - b.first);

    if (std::abs(cross) > 1e-9)
    {
      corners.push_back(b);
    }
  }

  return corners.size() >= 3 ? corners : ring;
}

double useWeight(RoomUse use);

// --- the cost function, §5.2 ---------------------------------------------

// One floor's worth of ground, rasterised at about a hand's breadth.
struct LotMask
{
  double lowX = 0, highX = 0, lowY = 0, highY = 0;
  uint32_t columns = 0, rows = 0;
  std::vector<uint8_t> mask;

  bool inside(const double x, const double y) const
  {
    if (x < lowX || x > highX || y < lowY || y > highY)
    {
      return false;
    }

    if (mask.empty())
    {
      return true;
    }

    const double u = (x - lowX) / std::max(highX - lowX, 1e-9);
    const double v = (y - lowY) / std::max(highY - lowY, 1e-9);
    const uint32_t i = std::min(columns - 1, uint32_t(u * double(columns)));
    const uint32_t j = std::min(rows - 1, uint32_t(v * double(rows)));

    return mask[size_t(j) * columns + i] != 0;
  }
};

// Rasterises a lot polygon, by crossing count at each cell centre.
LotMask rasterise(const std::vector<std::pair<double, double>> &lot)
{
  LotMask out;

  out.lowX = out.highX = lot.front().first;
  out.lowY = out.highY = lot.front().second;

  for (const auto &point : lot)
  {
    out.lowX = std::min(out.lowX, point.first);
    out.highX = std::max(out.highX, point.first);
    out.lowY = std::min(out.lowY, point.second);
    out.highY = std::max(out.highY, point.second);
  }

  const double spanX = std::max(out.highX - out.lowX, 1e-6);
  const double spanY = std::max(out.highY - out.lowY, 1e-6);

  out.columns = std::min(512u, std::max(8u, uint32_t(spanX / .1)));
  out.rows = std::min(512u, std::max(8u, uint32_t(spanY / .1)));
  out.mask.assign(size_t(out.columns) * out.rows, 0);

  const auto within = [&lot](const double x, const double y) {
    bool in = false;

    for (size_t e = 0, f = lot.size() - 1; e < lot.size(); f = e++)
    {
      const bool straddles = (lot[e].second > y) != (lot[f].second > y);

      if (straddles
          && x < (lot[f].first - lot[e].first) * (y - lot[e].second)
                   / (lot[f].second - lot[e].second)
                 + lot[e].first)
      {
        in = !in;
      }
    }

    return in;
  };

  const double cellX = spanX / double(out.columns);
  const double cellY = spanY / double(out.rows);

  for (uint32_t j = 0; j < out.rows; ++j)
  {
    const double y = out.lowY + spanY * (double(j) + .5) / double(out.rows);

    for (uint32_t i = 0; i < out.columns; ++i)
    {
      const double x = out.lowX + spanX * (double(i) + .5) / double(out.columns);

      // Every corner of the cell, not its centre. A mask taken at cell centres
      // is the lot *grown* by half a cell, and a plan allowed to fill it hangs
      // that much over every boundary - which on a wedge, whose edges are
      // diagonal and long, is several per cent of a small building. Conceding
      // the last hand's breadth of ground costs nothing and is the difference
      // between "inside to within the raster" and inside.
      const bool covered = within(x, y) && within(x - cellX * .5, y - cellY * .5)
                           && within(x + cellX * .5, y - cellY * .5)
                           && within(x - cellX * .5, y + cellY * .5)
                           && within(x + cellX * .5, y + cellY * .5);

      out.mask[size_t(j) * out.columns + i] = covered ? 1 : 0;
    }
  }

  return out;
}

double outsideWall(const Grid &, int32_t label, double streetX, double streetZ, WallRun &,
                   double &normalX, double &normalZ);

struct Context
{
  const Programme *programme = nullptr;
  const FloorPlanSettings *settings = nullptr;

  // Room index -> grid index, and the level of each grid.
  std::vector<uint32_t> gridOf;
  std::vector<int32_t> levels;

  // The programme's links, restricted to same-floor pairs of placed rooms.
  struct Link
  {
    uint32_t a = 0, b = 0;
    bool open = false;
  };

  std::vector<Link> links;

  // Rooms that must reach the outside: the entrance, and anything fronting
  // the street. The paper's "the entrance, patios, and garage must have access
  // to the outside".
  std::vector<uint32_t> mustReachOutside;

  // Rooms that must reach the outside *on the street side*. A shop is not a
  // room that happens to have an outside wall: facing the street is what makes
  // it a shop, and a counting house with its trading window on the yard is a
  // store room. Ca already knows "must reach the outside"; this is the same
  // requirement with a direction on it.
  std::vector<uint32_t> mustFaceStreet;

  // The vertical circulation, by grid: the staircase on the ground floor and
  // the landing you arrive at on each floor above it.
  //
  // §4: "Multi-story spaces, such as stairwells and atria, are indicated as
  // such", and §7 names them as the thing that couples the optimisation across
  // floors - the paper's own failure case is a staircase that does not line up
  // and is "blocked on the second story". A stair whose landing is at the far
  // end of the floor above is not a stair; it is a hole in one ceiling and a
  // hole in another.
  std::vector<uint32_t> climbs;

  // The ground each floor may stand on, rasterised, by level.
  //
  // The ground a massed building hands down is an L or a T as often as it is
  // a rectangle, and against a bounding box the plan spreads into the yard
  // between two wings - where there is no wall, no floor and no roof. A mask
  // reads in constant time, which is what Cf needs: it asks this once per
  // cell per proposal, a hundred thousand times over.
  //
  // A level with no lot is not unconstrained: it still has to stand over the
  // floor below it. Only the bottom floor with no lot is free, which is the
  // paper's case of a building whose own outline is the thing being decided.
  std::map<int32_t, LotMask> lots;

  const LotMask *lotFor(const int32_t level) const
  {
    const auto found = lots.find(level);

    return found == lots.end() ? nullptr : &found->second;
  }

  // The ground a floor stands on: its own plate, or failing that the nearest
  // one below it. A storey the massing never gave a plate to - the programme
  // asked for three and the building carries one - still may not oversail the
  // lot, because the lot is where the street stops.
  const LotMask *groundFor(int32_t level) const
  {
    for (int32_t step = 0; step < 8; ++step, --level)
    {
      if (const LotMask *found = lotFor(level))
      {
        return found;
      }
    }

    return lots.empty() ? nullptr : &lots.begin()->second;
  }
};
// The largest axis-aligned rectangle that fits inside the lot.
//
// The canonical start packs equal rooms on a grid, and a grid laid over the
// bounding box of an L starts a third of the building in the yard. Starting
// inside the ground is not the same as constraining it - the optimiser may
// still grow into the arms, and does - it just does not begin by having to
// climb out of a hole it was put in.
void inscribedBox(const LotMask &lot,
                  double &lowX,
                  double &highX,
                  double &lowY,
                  double &highY)
{
  lowX = lot.lowX;
  highX = lot.highX;
  lowY = lot.lowY;
  highY = lot.highY;

  if (lot.mask.empty())
  {
    return;
  }

  const uint32_t columns = lot.columns, rows = lot.rows;
  std::vector<uint32_t> heights(columns, 0);

  uint32_t bestArea = 0, bestI0 = 0, bestI1 = 0, bestJ0 = 0, bestJ1 = 0;

  for (uint32_t j = 0; j < rows; ++j)
  {
    for (uint32_t i = 0; i < columns; ++i)
    {
      heights[i] = lot.mask[size_t(j) * columns + i] != 0 ? heights[i] + 1 : 0;
    }

    // The largest rectangle under the histogram, by the stack method.
    std::vector<uint32_t> stack;

    for (uint32_t i = 0; i <= columns; ++i)
    {
      const uint32_t height = i < columns ? heights[i] : 0;

      while (!stack.empty() && heights[stack.back()] >= height)
      {
        const uint32_t top = stack.back();
        stack.pop_back();

        const uint32_t left = stack.empty() ? 0 : stack.back() + 1;
        const uint32_t area = heights[top] * (i - left);

        if (area > bestArea)
        {
          bestArea = area;
          bestI0 = left;
          bestI1 = i;
          bestJ0 = j + 1 - heights[top];
          bestJ1 = j + 1;
        }
      }

      stack.push_back(i);
    }
  }

  if (bestArea == 0)
  {
    return;
  }

  const double spanX = lot.highX - lot.lowX;
  const double spanY = lot.highY - lot.lowY;

  lowX = lot.lowX + spanX * double(bestI0) / double(columns);
  highX = lot.lowX + spanX * double(bestI1) / double(columns);
  lowY = lot.lowY + spanY * double(bestJ0) / double(rows);
  highY = lot.lowY + spanY * double(bestJ1) / double(rows);
}

std::vector<std::vector<std::pair<double, double>>> traceLoops(const Grid &grid,
                                                               const int32_t label)
{
  const auto isLabel = [&](const int32_t i, const int32_t j) {
    return i >= 0 && j >= 0 && i < int32_t(grid.columns()) && j < int32_t(grid.rows())
           && grid.at(uint32_t(i), uint32_t(j)) == label;
  };

  using Corner = std::pair<double, double>;
  std::map<Corner, std::vector<Corner>> next;

  for (uint32_t j = 0; j < grid.rows(); ++j)
  {
    for (uint32_t i = 0; i < grid.columns(); ++i)
    {
      if (!isLabel(int32_t(i), int32_t(j)))
      {
        continue;
      }

      const double x0 = grid.xs[i], x1 = grid.xs[i + 1];
      const double y0 = grid.ys[j], y1 = grid.ys[j + 1];

      if (!isLabel(int32_t(i), int32_t(j) - 1))
      {
        next[{x0, y0}].push_back({x1, y0});
      }

      if (!isLabel(int32_t(i) + 1, int32_t(j)))
      {
        next[{x1, y0}].push_back({x1, y1});
      }

      if (!isLabel(int32_t(i), int32_t(j) + 1))
      {
        next[{x1, y1}].push_back({x0, y1});
      }

      if (!isLabel(int32_t(i) - 1, int32_t(j)))
      {
        next[{x0, y1}].push_back({x0, y0});
      }
    }
  }

  std::vector<std::vector<Corner>> loops;
  using Edge = std::pair<Corner, Corner>;
  std::set<Edge> walked;

  for (const auto &entry : next)
  {
    for (const Corner &first : entry.second)
    {
      if (walked.count({entry.first, first}))
      {
        continue;
      }
      std::vector<Corner> loop;
      const Corner start = entry.first;
      Corner previous = start, at = first;
      loop.push_back(start);
      walked.insert({start, first});
      while (at != start)
      {
        loop.push_back(at);
        const auto outgoing = next.find(at);
        if (outgoing == next.end())
        {
          break;
        }
        bool found = false;
        Corner chosen;
        double bestTurn = -4.;
        for (const Corner &candidate : outgoing->second)
        {
          if (walked.count({at, candidate}))
          {
            continue;
          }
          const double ux = at.first - previous.first, uy = at.second - previous.second;
          const double vx = candidate.first - at.first, vy = candidate.second - at.second;
          const double turn = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
          if (!found || turn > bestTurn)
          {
            found = true;
            bestTurn = turn;
            chosen = candidate;
          }
        }
        if (!found)
        {
          break;
        }
        walked.insert({at, chosen});
        previous = at;
        at = chosen;
      }
      if (at != start)
      {
        continue;
      }

      // Corners only, as traceOutline does.
      std::vector<Corner> corners;

      for (size_t i = 0; i < loop.size(); ++i)
      {
        const Corner &a = loop[(i + loop.size() - 1) % loop.size()];
        const Corner &b = loop[i];
        const Corner &c = loop[(i + 1) % loop.size()];

        const bool straight = (std::abs(a.first - b.first) < 1e-9 && std::abs(b.first - c.first) < 1e-9) || (std::abs(a.second - b.second) < 1e-9 && std::abs(b.second - c.second) < 1e-9);

        if (!straight)
        {
          corners.push_back(b);
        }
      }

      if (corners.size() >= 3)
      {
        loops.push_back(std::move(corners));
      }
    }
  }

  std::sort(loops.begin(), loops.end(), [](const auto &a, const auto &b) {
    const auto area = [](const std::vector<Corner> &ring) {
      double twice = 0;

      for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
      {
        twice += ring[j].first * ring[i].second - ring[i].first * ring[j].second;
      }

      return std::abs(twice);
    };

    return area(a) > area(b);
  });

  return loops;
}

bool touchesOutside(const Grid &grid, const int32_t label)
{
  for (uint32_t j = 0; j < grid.rows(); ++j)
  {
    for (uint32_t i = 0; i < grid.columns(); ++i)
    {
      if (grid.at(i, j) != label)
      {
        continue;
      }

      const bool edge = i == 0 || j == 0 || i + 1 == grid.columns() || j + 1 == grid.rows();

      if (edge || grid.at(i - 1, j) == kOutside || grid.at(i + 1, j) == kOutside
          || grid.at(i, j - 1) == kOutside || grid.at(i, j + 1) == kOutside)
      {
        return true;
      }
    }
  }

  return false;
}

// Is the point inside any room of this grid?
// The area-weighted middle of a room's cells.
bool centroidOf(const Grid &grid, const int32_t label, double &outX, double &outY)
{
  double area = 0;
  outX = 0;
  outY = 0;

  for (uint32_t j = 0; j < grid.rows(); ++j)
  {
    for (uint32_t i = 0; i < grid.columns(); ++i)
    {
      if (grid.at(i, j) != label)
      {
        continue;
      }

      const double piece = grid.cellArea(i, j);
      outX += (grid.xs[i] + grid.xs[i + 1]) * .5 * piece;
      outY += (grid.ys[j] + grid.ys[j + 1]) * .5 * piece;
      area += piece;
    }
  }

  if (area <= 1e-9)
  {
    return false;
  }

  outX /= area;
  outY /= area;

  return true;
}

// Which room is at a point, or kOutside for none and for off the grid.
int32_t labelAt(const Grid &grid, const double x, const double y)
{
  if (x < grid.xs.front() || x > grid.xs.back() || y < grid.ys.front()
      || y > grid.ys.back())
  {
    return kOutside;
  }

  const uint32_t i = uint32_t(std::min<size_t>(
    grid.columns() - 1,
    size_t(std::lower_bound(grid.xs.begin(), grid.xs.end(), x) - grid.xs.begin())
      - (x > grid.xs.front() ? 1 : 0)));
  const uint32_t j = uint32_t(std::min<size_t>(
    grid.rows() - 1,
    size_t(std::lower_bound(grid.ys.begin(), grid.ys.end(), y) - grid.ys.begin())
      - (y > grid.ys.front() ? 1 : 0)));

  return grid.at(i, j);
}

bool occupied(const Grid &grid, const double x, const double y)
{
  if (x < grid.xs.front() || x > grid.xs.back() || y < grid.ys.front()
      || y > grid.ys.back())
  {
    return false;
  }

  const uint32_t i = uint32_t(std::min<size_t>(
    grid.columns() - 1,
    size_t(std::lower_bound(grid.xs.begin(), grid.xs.end(), x) - grid.xs.begin())
      - (x > grid.xs.front() ? 1 : 0)));
  const uint32_t j = uint32_t(std::min<size_t>(
    grid.rows() - 1,
    size_t(std::lower_bound(grid.ys.begin(), grid.ys.end(), y) - grid.ys.begin())
      - (y > grid.ys.front() ? 1 : 0)));

  return grid.at(i, j) != kOutside;
}

LayoutCost evaluate(const std::vector<Grid> &grids, const Context &context)
{
  const Programme &programme = *context.programme;
  const FloorPlanSettings &settings = *context.settings;

  LayoutCost cost;

  // --- Ca, accessibility ---
  //
  // "the number of missing connections plus the number of enclosed patios,
  // entrances, and garages."
  //
  // Not every programme is fully realisable, which is why this is a cost and
  // not a constraint. The class gallery delivers 96% of the connections its
  // programmes ask for, and what is left is not near-misses: every one of the
  // sixteen shares *nought* metres of wall, because the rooms are nowhere near
  // each other. Five of them are a single mansion hall the network linked to
  // thirteen rooms, and a rectilinear room cannot share a doorway's width with
  // thirteen others - there is no wall left to share.
  //
  // Three ways of closing that gap were measured and all three were worse: a
  // proposal move aimed at unsatisfied links (93%), capping how many rooms one
  // room may be asked to touch (18 failures rather than 16), and spreading the
  // connectivity repair over the least-connected host instead of the first
  // (93%). The last is the interesting one - a hall is where it is because
  // everything is arranged around it, so it is the best room to take another
  // door, not the worst.
  for (const Context::Link &link : context.links)
  {
    const Grid &grid = grids[context.gridOf[link.a]];
    WallRun run;
    sharedWall(grid, int32_t(link.a), int32_t(link.b), run);

    cost.accessibility += run.length >= settings.doorWidthMetres ? 0. : 1.;
  }

  for (const uint32_t room : context.mustReachOutside)
  {
    cost.accessibility +=
      touchesOutside(grids[context.gridOf[room]], int32_t(room)) ? 0. : 1.;
  }

  // An outdoor room's front is open air.
  //
  // This is Ca as §5.2 defines it - "the number of missing connections plus the
  // number of *enclosed* patios, entrances, and garages" - applied to the three
  // rooms in this vocabulary that are outdoor by nature. A balcony boxed in on
  // three sides is a room somebody forgot to roof; a porch boxed in is a lobby;
  // a veranda boxed in is a corridor. The paper's own example plan carries two
  // porches, front and back, and neither is a room you pass through to reach
  // another.
  //
  // Counted per cell face rather than per room, so one open on two sides and
  // blocked on one is scored a third wrong rather than wholly wrong. A balcony
  // is held to one neighbour because the programme has already made it a leaf;
  // a porch may have two - the hall it serves and the street it faces - so its
  // host is whatever it is linked to, whichever that is.
  for (uint32_t r = 0; r < programme.rooms.size(); ++r)
  {
    const RoomUse outdoors = programme.rooms[r].use;

    if (r >= context.gridOf.size()
        || (outdoors != RoomUse::Balcony && outdoors != RoomUse::Porch
            && outdoors != RoomUse::Veranda))
    {
      continue;
    }

    const uint32_t g = context.gridOf[r];

    if (g >= grids.size())
    {
      continue;
    }

    // The rooms it belongs to, which are the only things allowed against it.
    std::vector<int32_t> hosts;

    for (const Context::Link &link : context.links)
    {
      if (link.a == r)
      {
        hosts.push_back(int32_t(link.b));
      }
      else if (link.b == r)
      {
        hosts.push_back(int32_t(link.a));
      }
    }

    const Grid &grid = grids[g];
    double sides = 0, blocked = 0;

    for (uint32_t j = 0; j < grid.rows(); ++j)
    {
      for (uint32_t i = 0; i < grid.columns(); ++i)
      {
        if (grid.at(i, j) != int32_t(r))
        {
          continue;
        }

        for (int32_t d = 0; d < 4; ++d)
        {
          const int32_t ni = int32_t(i) + (d == 0 ? -1 : d == 1 ? 1 : 0);
          const int32_t nj = int32_t(j) + (d == 2 ? -1 : d == 3 ? 1 : 0);

          sides += 1.;

          if (ni < 0 || nj < 0 || ni >= int32_t(grid.columns()) || nj >= int32_t(grid.rows()))
          {
            continue;
          }

          const int32_t there = grid.at(uint32_t(ni), uint32_t(nj));

          const bool mine = there == kOutside || there == int32_t(r)
                            || std::find(hosts.begin(), hosts.end(), there) != hosts.end();

          blocked += mine ? 0. : 1.;
        }
      }
    }

    cost.accessibility += sides > 0 ? blocked / sides : 0.;
  }

  for (const uint32_t room : context.mustFaceStreet)
  {
    WallRun run;
    double normalX = 0, normalZ = 0;

    const double length = outsideWall(grids[context.gridOf[room]], int32_t(room),
                                      settings.streetX, settings.streetZ, run, normalX,
                                      normalZ);

    // Its longest outside wall has to be the one the street can see. Facing
    // the yard is not a smaller version of facing the street - it is a
    // different building - so this is a whole missing connection's worth of
    // cost and not a nudge.
    const double facing = normalX * settings.streetX + normalZ * settings.streetZ;
    cost.accessibility += length >= settings.doorWidthMetres && facing > .5 ? 0. : 1.;
  }

  // --- Cd, dimensions ---
  //
  // "let l_a(x) be the log-likelihood of the observed area of room i in layout
  // x and let l_as(x) be the log-likelihood of the corresponding aspect ratio."
  // Not a target value: the distribution itself.
  for (uint32_t r = 0; r < programme.rooms.size(); ++r)
  {
    if (r >= context.gridOf.size() || context.gridOf[r] == uint32_t(-1))
    {
      continue;
    }

    const Grid &grid = grids[context.gridOf[r]];
    const double area = areaOf(grid, int32_t(r));

    if (area <= 0)
    {
      cost.dimensions += 20.;

      continue;
    }

    // Weighted by how much of the building's life the room carries.
    //
    // Cd scores every room against its own distribution and sums them, so a
    // closet's four square metres count for exactly as much as a great hall's
    // fifty. When the plan cannot satisfy everything - and it never can, the
    // areas are drawn independently and the ground is finite - the optimiser
    // has no reason to prefer getting the hall right over getting the larder
    // right, and it splits the difference.
    //
    // A building is not used evenly. The hall, the kitchen and the room the
    // household sits in carry it; the store, the closet and the pantry are
    // where the compromises should land. This is a departure from §5.2, which
    // weights the sum uniformly, and it is the smallest one that says so.
    const double matters = useWeight(programme.rooms[r].use);

    cost.dimensions -= matters * areaLogLikelihood(programme, r, area);

    // ...and circulation pays for the slack it absorbs.
    //
    // A floor has to tile its plate, so area no room wants is taken by
    // whichever is cheapest to grow. That is always circulation: the area
    // likelihood is broad and weighted 1 against accessibility's 12 and the
    // floor term's 90, so a staircase at ten times its drawn size costs less
    // than a tenth of a missing door. A church asked for five square metres of
    // stair and was given fifty three; a manor asked for ten and got eighty
    // four, wrapped round three sides of its great hall.
    //
    // Priced rather than forbidden. Forbidding it as a rule does not work -
    // the gate admits a candidate that breaks no *more* rules than the state it
    // came from, so an oversized stair simply stays oversized - and there is no
    // hard line to draw anyway. What there is is a direction: a stair twice its
    // size is a generous stair and one ten times its size is a hall somebody
    // mislabelled, so the excess is charged, squared, and steeply enough to
    // outweigh the room that would otherwise have had to shrink.
    // Circulation, and the outdoor rooms, which are small by nature too.
    //
    // Pricing the stair moved the slack rather than removing it: a floor still
    // has to tile its plate, so what the stair stopped absorbing went to the
    // next cheapest room, and that was the porch. A church came back with a
    // porch of fifty four square metres, nine metres by twelve, which is a
    // courtyard with a roof over it. A porch is a threshold, a veranda is a
    // stride of shade and a balcony is a stride of deck; none of them grows
    // with the building, and all of them are cheap to grow for exactly the
    // reason the stair was.
    const RoomUse use = programme.rooms[r].use;
    const bool circulation = use == RoomUse::Staircase || use == RoomUse::Landing
                             || use == RoomUse::Passage || use == RoomUse::Lobby
                             || use == RoomUse::ScreensPassage
                             || use == RoomUse::Porch || use == RoomUse::Veranda
                             || use == RoomUse::Balcony;

    if (circulation && programme.rooms[r].areaMetres > 0)
    {
      const double over = area / (programme.rooms[r].areaMetres * 2.);

      cost.dimensions += over > 1. ? (over - 1.) * (over - 1.) * 12. : 0.;
    }

    double lowX = 0, highX = 0, lowY = 0, highY = 0;

    if (boundsOf(grid, int32_t(r), lowX, highX, lowY, highY))
    {
      const double a = std::max(highX - lowX, highY - lowY);
      const double b = std::max(std::min(highX - lowX, highY - lowY), 1e-6);
      cost.dimensions -= aspectLogLikelihood(programme, r, a / b);
    }
  }

  // --- Cf, floors ---
  //
  // "Each floor must be contained within the envelope of the floor beneath."
  double unsupported = 0, total = 0;

  for (size_t g = 0; g < grids.size(); ++g)
  {
    const Grid &grid = grids[g];

    for (uint32_t j = 0; j < grid.rows(); ++j)
    {
      for (uint32_t i = 0; i < grid.columns(); ++i)
      {
        if (grid.at(i, j) == kOutside)
        {
          continue;
        }

        const double area = grid.cellArea(i, j);
        total += area;

        const double x = (grid.xs[i] + grid.xs[i + 1]) * .5;
        const double y = (grid.ys[j] + grid.ys[j + 1]) * .5;

        // An upper floor stands on the floor below it, and that is a rule and
        // not a preference.
        //
        // Cf charges for the part that does not, which is the paper's own
        // treatment and is right when the building outline is given: there the
        // floors are cut from one envelope and the term only has to discourage
        // the last few square metres. Ours decides its own outline, and a soft
        // term let first floors come out standing 46% to 87% over the ground
        // floor - a bedroom hanging in the air over the yard, with a wall
        // under nothing and a roof over nothing. Held to the same refusal the
        // lot gets, a floor can still step in from the one below, which is
        // what an upper floor usually does; it simply cannot step *out*.
        // ...which is the floor at the level below, and only for a floor that
        // has one. The bottom floor stands on the ground - "the bottom floor
        // Fx1 does not need to be supported" - and a ground floor held up over
        // its own cellar, which is smaller than the house and usually under
        // part of it, cannot satisfy anything.
        int32_t under = -1;

        for (size_t g2 = 0; g2 < grids.size(); ++g2)
        {
          under = context.levels[g2] + 1 == context.levels[g] ? int32_t(g2) : under;
        }

        if (context.levels[g] > 0 && under >= 0)
        {
          uint32_t hanging = 0, tested = 0;

          for (uint32_t sy = 0; sy < 3; ++sy)
          {
            const double py =
              grid.ys[j] + (grid.ys[j + 1] - grid.ys[j]) * (double(sy) + .5) / 3.;

            for (uint32_t sx = 0; sx < 3; ++sx)
            {
              const double px =
                grid.xs[i] + (grid.xs[i + 1] - grid.xs[i]) * (double(sx) + .5) / 3.;

              hanging += occupied(grids[size_t(under)], px, py) ? 0u : 1u;
              ++tested;
            }
          }

          cost.outsideGround += tested > 0 ? area * double(hanging) / double(tested) : 0.;
        }

        if (const LotMask *ground = context.groundFor(context.levels[g]))
        {
          // The share of the cell that is off the ground, not whether its
          // middle is. A cell straddling the edge is the usual case after a
          // wall slide, and calling it wholly inside makes the last half
          // metre of the street free to build on.
          //
          // Sampled at a fixed spacing rather than a fixed count, and out to
          // the cell's own corners: a room is one cell until something splits
          // it, so a fixed three by three over a five-metre cell is a sample
          // every metre and a half, and the corner that poked into the street
          // fell between two of them.
          const double wide = grid.xs[i + 1] - grid.xs[i];
          const double tall = grid.ys[j + 1] - grid.ys[j];
          const uint32_t across =
            std::min(8u, std::max(2u, uint32_t(std::ceil(wide / .3))));
          const uint32_t down = std::min(8u, std::max(2u, uint32_t(std::ceil(tall / .3))));

          uint32_t off = 0, taken = 0;

          for (uint32_t sy = 0; sy < down; ++sy)
          {
            const double py = grid.ys[j] + tall * double(sy) / double(down - 1);

            for (uint32_t sx = 0; sx < across; ++sx)
            {
              const double px = grid.xs[i] + wide * double(sx) / double(across - 1);

              off += ground->inside(px, py) ? 0u : 1u;
              ++taken;
            }
          }

          cost.outsideGround += taken > 0 ? area * double(off) / double(taken) : 0.;
        }

        const LotMask *lot = context.lotFor(context.levels[g]);

        if (lot != nullptr)
        {
          // The ground this floor was given, which for the bottom floor is
          // the paper's Fx0 and for an upper floor is the storey plate the
          // massing laid down - the part of the building that is that tall.
          double off = lot->inside(x, y) ? 0. : area;

          // ...and the floor below it, which this used to stop asking about
          // the moment a plate existed. A plate is only where a floor *may*
          // stand; standing over the floor below is a different question, and
          // with it unasked an upper floor could sit in a corner of its own
          // plate and pay nothing for it. A temple came out with its upper
          // floor twenty eight per cent over the one beneath.
          //
          // Not the bottom floor: a ground floor is not held up by its cellar,
          // the cellar is dug under part of it. Paired by level rather than by
          // position in the vector, because with a cellar present the grid
          // before this one is two levels down as often as one.
          if (context.levels[g] > 0)
          {
            for (size_t h = 0; h < grids.size(); ++h)
            {
              if (context.levels[h] == context.levels[g] - 1)
              {
                off = std::max(off, occupied(grids[h], x, y) ? 0. : area);

                break;
              }
            }
          }

          unsupported += off;

          continue;
        }

        if (g == 0)
        {
          continue;
        }

        unsupported += occupied(grids[g - 1], x, y) ? 0. : area;
      }
    }
  }

  cost.floors = total > 0 ? unsupported / total : 0.;

  // ...and the stair has to arrive somewhere. The share of each landing that
  // is not over the flight below it, which is zero when they line up.
  for (size_t g = 1; g < grids.size() && g < context.climbs.size(); ++g)
  {
    if (context.climbs[g] == uint32_t(-1) || context.climbs[g - 1] == uint32_t(-1))
    {
      continue;
    }

    const Grid &above = grids[g];
    const Grid &below = grids[g - 1];
    const int32_t arriving = int32_t(context.climbs[g]);
    const int32_t leaving = int32_t(context.climbs[g - 1]);

    double landing = 0, over = 0;

    for (uint32_t j = 0; j < above.rows(); ++j)
    {
      for (uint32_t i = 0; i < above.columns(); ++i)
      {
        if (above.at(i, j) != arriving)
        {
          continue;
        }

        const double area = above.cellArea(i, j);
        const double x = (above.xs[i] + above.xs[i + 1]) * .5;
        const double y = (above.ys[j] + above.ys[j + 1]) * .5;

        landing += area;
        over += labelAt(below, x, y) == leaving ? area : 0.;
      }
    }

    cost.floors += landing > 1e-6 ? 1. - over / landing : 0.;
  }

  // ...and a shaft has to be the same shaft on every floor.
  //
  // A tower is one room repeated the whole height of the building at the same
  // size and in the same place - that is what distinguishes it from a column
  // of similar rooms, and it is why a church can run a stair up it and hang a
  // bell at the top. The programme now emits one instance per floor, but
  // nothing asked them to line up: a temple came out with twenty nine square
  // metres of tower on the ground and nineteen on the floor above it, seven
  // metres away.
  //
  // Scored the same way the landing is, and paired by level rather than by
  // position in the vector, since a cellar makes those differ.
  for (size_t g = 0; g < grids.size(); ++g)
  {
    int32_t shaft = kOutside, under = kOutside;
    size_t below = grids.size();

    for (uint32_t r = 0; r < programme.rooms.size(); ++r)
    {
      if (r >= context.gridOf.size() || !isShaft(programme.rooms[r].use))
      {
        continue;
      }

      if (context.gridOf[r] == g)
      {
        shaft = int32_t(r);
      }
    }

    if (shaft == kOutside)
    {
      continue;
    }

    for (size_t h = 0; h < grids.size(); ++h)
    {
      if (context.levels[h] == context.levels[g] - 1)
      {
        below = h;
      }
    }

    if (below >= grids.size())
    {
      continue;
    }

    for (uint32_t r = 0; r < programme.rooms.size(); ++r)
    {
      if (r < context.gridOf.size() && context.gridOf[r] == uint32_t(below)
          && isShaft(programme.rooms[r].use))
      {
        under = int32_t(r);
      }
    }

    if (under == kOutside)
    {
      continue;
    }

    const Grid &here = grids[g];
    double mine = 0, aligned = 0;

    for (uint32_t j = 0; j < here.rows(); ++j)
    {
      for (uint32_t i = 0; i < here.columns(); ++i)
      {
        if (here.at(i, j) != shaft)
        {
          continue;
        }

        const double area = here.cellArea(i, j);
        const double x = (here.xs[i] + here.xs[i + 1]) * .5;
        const double y = (here.ys[j] + here.ys[j + 1]) * .5;

        mine += area;
        aligned += labelAt(grids[below], x, y) == under ? area : 0.;
      }
    }

    cost.floors += mine > 1e-6 ? 1. - aligned / mine : 0.;
  }

  // The extent of a floor's built ground, which is not its grid: the grid may
  // have grown columns outside the building.
  const auto floorBounds = [](const Grid &grid, double &lowX, double &highX, double &lowY,
                              double &highY) {
    bool any = false;

    for (uint32_t j = 0; j < grid.rows(); ++j)
    {
      for (uint32_t i = 0; i < grid.columns(); ++i)
      {
        if (grid.at(i, j) == kOutside)
        {
          continue;
        }

        lowX = any ? std::min(lowX, grid.xs[i]) : grid.xs[i];
        highX = any ? std::max(highX, grid.xs[i + 1]) : grid.xs[i + 1];
        lowY = any ? std::min(lowY, grid.ys[j]) : grid.ys[j];
        highY = any ? std::max(highY, grid.ys[j + 1]) : grid.ys[j + 1];
        any = true;
      }
    }

    return any;
  };

  // --- symmetry of the frontage ---
  //
  // The outline of each floor, mirrored about the axis through its middle
  // running away from the street, and scored on how much of it fails to match.
  // Sampled rather than clipped: a boolean of a rectilinear polygon against its
  // own mirror is exact and slow, and this is evaluated once per proposal.
  if (settings.symmetryWeight > 0)
  {
    const double sx = settings.streetX, sz = settings.streetZ;
    const double px = -sz, pz = sx;  // across the street, the axis to mirror in

    for (const Grid &grid : grids)
    {
      double lowX = 0, highX = 0, lowY = 0, highY = 0;

      if (!floorBounds(grid, lowX, highX, lowY, highY))
      {
        continue;
      }

      const double midX = (lowX + highX) * .5, midY = (lowY + highY) * .5;
      uint32_t inside = 0, matched = 0;

      for (uint32_t j = 0; j < 24; ++j)
      {
        const double y = lowY + (highY - lowY) * (double(j) + .5) / 24.;

        for (uint32_t i = 0; i < 24; ++i)
        {
          const double x = lowX + (highX - lowX) * (double(i) + .5) / 24.;

          if (labelAt(grid, x, y) == kOutside)
          {
            continue;
          }

          ++inside;

          // The same point reflected across the axis: keep the component along
          // the street, flip the one across it.
          const double dx = x - midX, dy = y - midY;
          const double across = dx * px + dy * pz;
          const double along = dx * sx + dy * sz;
          const double rx = midX + along * sx - across * px;
          const double ry = midY + along * sz - across * pz;

          matched += labelAt(grid, rx, ry) != kOutside ? 1u : 0u;
        }
      }

      cost.shape += inside > 0 ? settings.symmetryWeight * (1. - double(matched) / double(inside))
                               : 0.;
    }
  }

  // --- Cs, shapes ---
  //
  // Mc(S) = (A(H(S)) - A(S)) / A(S) + e(S), with hallways and stairways
  // exempt from the convexity part, groups of rooms joined by open walls
  // measured together, and the floor outline penalised for irregularity.
  const auto measure = [](const double area, const double hull, const uint32_t edges) {
    return area > 1e-6 ? (hull - area) / area + double(edges) : double(edges);
  };

  double rooms = 0;

  for (uint32_t r = 0; r < programme.rooms.size(); ++r)
  {
    if (r >= context.gridOf.size() || context.gridOf[r] == uint32_t(-1))
    {
      continue;
    }

    const RoomUse use = programme.rooms[r].use;
    const bool hallway = use == RoomUse::Hall || use == RoomUse::Passage
                         || use == RoomUse::Landing || use == RoomUse::Staircase
                         || use == RoomUse::Lobby || use == RoomUse::Gallery
                         || use == RoomUse::ScreensPassage;

    if (hallway)
    {
      continue;
    }

    const Grid &grid = grids[context.gridOf[r]];
    const double area = areaOf(grid, int32_t(r));

    rooms += measure(area, hullArea(grid, int32_t(r)), outlineEdges(grid, int32_t(r)));
  }

  // Groups joined by open walls are experienced as one space, so they are
  // measured as one shape.
  std::vector<int32_t> group(programme.rooms.size(), -1);
  int32_t groups = 0;

  for (const Context::Link &link : context.links)
  {
    if (!link.open)
    {
      continue;
    }

    const int32_t a = group[link.a];
    const int32_t b = group[link.b];

    if (a < 0 && b < 0)
    {
      group[link.a] = group[link.b] = groups++;
    }
    else if (a < 0)
    {
      group[link.a] = b;
    }
    else if (b < 0)
    {
      group[link.b] = a;
    }
    else if (a != b)
    {
      for (int32_t &entry : group)
      {
        entry = entry == b ? a : entry;
      }
    }
  }

  double grouped = 0;

  for (int32_t g = 0; g < groups; ++g)
  {
    double area = 0, hull = 0;
    uint32_t edges = 0;
    uint32_t members = 0;

    for (uint32_t r = 0; r < group.size(); ++r)
    {
      if (group[r] != g || context.gridOf[r] == uint32_t(-1))
      {
        continue;
      }

      const Grid &grid = grids[context.gridOf[r]];
      area += areaOf(grid, int32_t(r));
      hull += hullArea(grid, int32_t(r));
      edges += outlineEdges(grid, int32_t(r));
      ++members;
    }

    grouped += members >= 2 ? measure(area, hull, edges) : 0.;
  }

  double outlines = 0;

  for (const Grid &grid : grids)
  {
    outlines += double(outlineEdges(grid, kOutside));
  }

  cost.shape = settings.roomShapeWeight * rooms + settings.groupShapeWeight * grouped
               + settings.outlineWeight * outlines;

  // --- and the building's own size, which is a variable of the programme ---

  if (settings.totalAreaWeight > 0 && total > 1e-6)
  {
    double wanted = 0;

    for (uint32_t r = 0; r < programme.rooms.size(); ++r)
    {
      if (context.gridOf[r] != uint32_t(-1))
      {
        wanted += programme.rooms[r].areaMetres;
      }
    }

    if (wanted > 1e-6)
    {
      const double ratio = std::log(total / wanted);
      cost.dimensions += settings.totalAreaWeight * ratio * ratio;
    }
  }

  cost.total = settings.accessibilityWeight * cost.accessibility
               + settings.dimensionWeight * cost.dimensions
               + settings.floorWeight * cost.floors + settings.shapeWeight * cost.shape;

  return cost;
}


// --- §5.1, the proposal moves --------------------------------------------

// One maximal run of collinear wall segments between the same two rooms: the
// paper's wall. Restricting a run to a constant pair of labels is what makes
// "move the wall" mean something - a run whose neighbours change along it is
// two walls that happen to be collinear.
struct Wall
{
  bool vertical = true;
  uint32_t line = 0;    // Index into xs (vertical) or ys (horizontal).
  uint32_t from = 0;    // First cell index along the wall.
  uint32_t to = 0;      // One past the last.
  int32_t lower = 0;    // Label on the low side...
  int32_t upper = 0;    // ...and on the high side.
};

void collectWalls(const Grid &grid, std::vector<Wall> &out)
{
  out.clear();

  for (uint32_t i = 1; i < grid.columns(); ++i)
  {
    uint32_t start = 0;
    bool open = false;
    int32_t lower = 0, upper = 0;

    for (uint32_t j = 0; j <= grid.rows(); ++j)
    {
      const bool same = j < grid.rows() && open && grid.at(i - 1, j) == lower
                        && grid.at(i, j) == upper;

      if (same)
      {
        continue;
      }

      if (open)
      {
        out.push_back({true, i, start, j, lower, upper});
        open = false;
      }

      if (j < grid.rows() && grid.at(i - 1, j) != grid.at(i, j))
      {
        open = true;
        start = j;
        lower = grid.at(i - 1, j);
        upper = grid.at(i, j);
      }
    }
  }

  for (uint32_t j = 1; j < grid.rows(); ++j)
  {
    uint32_t start = 0;
    bool open = false;
    int32_t lower = 0, upper = 0;

    for (uint32_t i = 0; i <= grid.columns(); ++i)
    {
      const bool same = i < grid.columns() && open && grid.at(i, j - 1) == lower
                        && grid.at(i, j) == upper;

      if (same)
      {
        continue;
      }

      if (open)
      {
        out.push_back({false, j, start, i, lower, upper});
        open = false;
      }

      if (i < grid.columns() && grid.at(i, j - 1) != grid.at(i, j))
      {
        open = true;
        start = i;
        lower = grid.at(i, j - 1);
        upper = grid.at(i, j);
      }
    }
  }
}

// "We randomly choose a wall and a split point along the wall. The wall is
// split into two collinear walls. One remains in place while the other is
// moved forward or backward a distance d ~ N(0, sigma^2). The split point has
// a strictly positive probability of being one of the end-points of the wall,
// in which case the entire wall is moved without splitting."
//
// And then: "we snap a moved wall to an existing one if, after a split move,
// the two walls lie within distance eps... We similarly align the wall with
// walls on neighbouring floors, to increase consistency across floors."
bool slideWall(Grid &grid, const FloorPlanSettings &settings, Rng &rng,
               const std::vector<double> &neighbourX,
               const std::vector<double> &neighbourY)
{
  std::vector<Wall> walls;
  collectWalls(grid, walls);

  if (walls.empty())
  {
    return false;
  }

  const Wall wall = walls[rng.below(uint32_t(walls.size()))];

  // The split: with good probability the whole wall moves; otherwise a
  // sub-run does, and that is what introduces a concave corner.
  uint32_t from = wall.from;
  uint32_t to = wall.to;

  if (to - from > 1 && rng.unit() > .45)
  {
    const uint32_t split = from + 1 + rng.below(to - from - 1);

    if (rng.unit() < .5)
    {
      to = split;
    }
    else
    {
      from = split;
    }
  }

  const double sigma = settings.slideSigmaMetres;
  const double d = rng.normal() * sigma;

  if (std::abs(d) < settings.gridEpsilonMetres)
  {
    return false;
  }

  const std::vector<double> &lines = wall.vertical ? grid.xs : grid.ys;
  const double here = lines[wall.line];
  double target = here + d;

  // Snap: to a line on this floor, or to one on a neighbouring floor.
  double best = target;
  double bestGap = settings.snapMetres;

  for (const double candidate : lines)
  {
    if (std::abs(candidate - target) < bestGap)
    {
      bestGap = std::abs(candidate - target);
      best = candidate;
    }
  }

  for (const double candidate : wall.vertical ? neighbourX : neighbourY)
  {
    if (std::abs(candidate - target) < bestGap)
    {
      bestGap = std::abs(candidate - target);
      best = candidate;
    }
  }

  target = best;

  if (std::abs(target - here) < settings.gridEpsilonMetres)
  {
    return false;
  }

  // A wall may now leave the floor's current extent, which is how a room
  // grows into ground the plan is not yet standing on. It may not run away:
  // a single move is bounded to a few times sigma, which is what the normal
  // draw would have given it anyway before snapping.
  const double reach = settings.slideSigmaMetres * 6.;

  if (target < lines.front() - reach || target > lines.back() + reach)
  {
    return false;
  }

  // The room that gains ground is the one the wall moved away from.
  const int32_t gains = target > here ? wall.lower : wall.upper;

  if (wall.vertical)
  {
    const uint32_t line = grid.insertX(target, settings.gridEpsilonMetres);
    const uint32_t old = grid.insertX(here, settings.gridEpsilonMetres);
    const uint32_t lowColumn = std::min(line, old);
    const uint32_t highColumn = std::max(line, old);

    // `from`/`to` were row indices before any insertion; rows are unchanged by
    // an x insertion, so they still address the same rows.
    for (uint32_t j = from; j < to && j < grid.rows(); ++j)
    {
      for (uint32_t i = lowColumn; i < highColumn; ++i)
      {
        grid.set(i, j, gains);
      }
    }

    return true;
  }

  const uint32_t line = grid.insertY(target, settings.gridEpsilonMetres);
  const uint32_t old = grid.insertY(here, settings.gridEpsilonMetres);
  const uint32_t lowRow = std::min(line, old);
  const uint32_t highRow = std::max(line, old);

  for (uint32_t i = from; i < to && i < grid.columns(); ++i)
  {
    for (uint32_t j = lowRow; j < highRow; ++j)
    {
      grid.set(i, j, gains);
    }
  }

  return true;
}

// Move this floor's stair over the flight below it, by exchanging labels with
// whatever is standing there.
//
// The third move, and the reason there is one. §7: multi-story spaces "couple
// the optimization on multiple floors" and are "the chief cause for these
// unsuccessful optimizations" - a landing at the wrong end of the floor cannot
// walk to the right one by sliding walls, because every intermediate state is
// worse than both ends and the cost is flat in between. It is the same
// situation the paper's own swap move exists for, so it gets the same answer:
// a label exchange, which changes the layout drastically in one step.
bool stackTheStair(Grid &above, const Grid &below, const int32_t arriving,
                   const int32_t leaving, const FloorPlanSettings &settings,
                   const bool widen)
{
  double atX = 0, atZ = 0;

  if (!centroidOf(below, leaving, atX, atZ))
  {
    return false;
  }

  const double area = areaOf(above, arriving);

  if (area < settings.minimumRoomMetres)
  {
    return false;
  }

  // The landing has to *cover the flight*, not sit near it: what has to line
  // up is the hole in the floor, and you climb through that hole onto the
  // landing. A landing the size of the staircase, centred on it, is half over
  // it and half over the bedroom next door - which is a stair arriving into a
  // wall, drawn as though it were fine.
  //
  // So the claim is the flight's own extent, widened to the landing's area
  // where the landing is the larger of the two.
  double lowX = 0, highX = 0, lowZ = 0, highZ = 0;

  if (!boundsOf(below, leaving, lowX, highX, lowZ, highZ))
  {
    return false;
  }

  const double flight = (highX - lowX) * (highZ - lowZ);

  if (widen && flight > 1e-6 && area > flight)
  {
    // Grown about its middle, keeping the flight's proportions.
    const double grow = std::sqrt(area / flight);
    const double midX = (lowX + highX) * .5, midZ = (lowZ + highZ) * .5;
    lowX = midX - (midX - lowX) * grow;
    highX = midX + (highX - midX) * grow;
    lowZ = midZ - (midZ - lowZ) * grow;
    highZ = midZ + (highZ - midZ) * grow;
  }

  lowX = std::max(lowX, above.xs.front());
  highX = std::min(highX, above.xs.back());
  lowZ = std::max(lowZ, above.ys.front());
  highZ = std::min(highZ, above.ys.back());

  if (highX - lowX < settings.minimumWidthMetres
      || highZ - lowZ < settings.minimumWidthMetres)
  {
    return false;
  }

  const uint32_t x0 = above.insertX(lowX, settings.gridEpsilonMetres);
  const uint32_t x1 = above.insertX(highX, settings.gridEpsilonMetres);
  const uint32_t z0 = above.insertY(lowZ, settings.gridEpsilonMetres);
  const uint32_t z1 = above.insertY(highZ, settings.gridEpsilonMetres);

  if (x1 <= x0 || z1 <= z0)
  {
    return false;
  }

  // Whatever the landing is leaving behind goes to the room it touches most,
  // so the floor stays covered and the donor stays in one piece.
  int32_t heir = kOutside;
  std::map<int32_t, double> touching;

  for (uint32_t j = 0; j < above.rows(); ++j)
  {
    for (uint32_t i = 0; i < above.columns(); ++i)
    {
      if (above.at(i, j) != arriving)
      {
        continue;
      }

      const int32_t left = i > 0 ? above.at(i - 1, j) : kOutside;
      const int32_t right = i + 1 < above.columns() ? above.at(i + 1, j) : kOutside;
      const int32_t down = j > 0 ? above.at(i, j - 1) : kOutside;
      const int32_t up = j + 1 < above.rows() ? above.at(i, j + 1) : kOutside;

      for (const int32_t side : {left, right, down, up})
      {
        if (side != kOutside && side != arriving)
        {
          touching[side] += above.cellArea(i, j);
        }
      }
    }
  }

  for (const auto &entry : touching)
  {
    if (heir == kOutside || entry.second > touching[heir])
    {
      heir = entry.first;
    }
  }

  if (heir == kOutside)
  {
    return false;
  }

  for (int32_t &label : above.label)
  {
    label = label == arriving ? heir : label;
  }

  for (uint32_t j = z0; j < z1; ++j)
  {
    for (uint32_t i = x0; i < x1; ++i)
    {
      if (above.at(i, j) != kOutside)
      {
        above.set(i, j, arriving);
      }
    }
  }

  return true;
}

// "Two rooms are selected at random and their labels are interchanged."
bool swapRooms(Grid &grid, Rng &rng)
{
  std::vector<int32_t> present;

  for (const int32_t label : grid.label)
  {
    if (label != kOutside
        && std::find(present.begin(), present.end(), label) == present.end())
    {
      present.push_back(label);
    }
  }

  if (present.size() < 2)
  {
    return false;
  }

  const int32_t a = present[rng.below(uint32_t(present.size()))];
  int32_t b = present[rng.below(uint32_t(present.size()))];

  if (a == b)
  {
    b = present[(std::find(present.begin(), present.end(), a) - present.begin() + 1)
                % present.size()];
  }

  for (int32_t &label : grid.label)
  {
    label = label == a ? b : label == b ? a : label;
  }

  return true;
}

// Put this floor's stair over the one below it.
//
// `FloorPlanSettings::stackChance` has described this move since the optimiser
// was written, and the annealing loop's own comment reasons about what it does
// to the rooms around it - "stacking it takes cells from whatever was standing
// there, and that is exactly the kind of move that can cut a room in half".
// The setting was never read. The move was never proposed.
//
// What it cost is not obvious from a plan of any one floor, because every
// floor was individually good: the rooms are the right size and they connect.
// It is only the *stack* that was wrong, and only a third of the way. The
// stair itself is built over the ground floor's staircase and runs the height
// of the house, so a landing that drifted off it is a flight arriving in a
// bedroom - and that had to happen by luck alone, which it did, in fourteen
// of forty eight buildings.
//
// A slide can align two floors when a line happens to be shared - that is what
// the neighbour lines passed to `slideWall` are for - but it moves one wall at
// a time, and moving a room bodily across a plan is not a sequence of slides
// that anything downhill of the cost would accept. This is the global move for
// the vertical direction, the way `swapRooms` is the global move within a
// floor.
//
// Crude on purpose: it claims the cells the room below stands on, and hands
// whatever it displaces to a neighbour. Everything that decides whether the
// result is a plan at all - rooms in one piece, wide enough, connected - is
// checked by the caller afterwards, so the move only has to be worth trying.
bool stackStair(Grid &grid, const Grid &below, const int32_t here, const int32_t there,
                const double epsilon, const bool mayGrow)
{
  if (here == kOutside || there == kOutside)
  {
    return false;
  }

  // The cells the room below stands on, not the box around them.
  //
  // A box was enough while this only moved stairs, which are compact. It is
  // wrong for a shaft: a church has three - nave, chancel and tower - and the
  // chancel's bounding box spanned nineteen metres of a twenty metre building,
  // so locking it claimed the nave and the tower with it. Whichever was
  // processed last won and the other two were left unlocked, which is exactly
  // what the church showed: a tower identical on both floors and a nave of
  // ninety eight square metres over one of thirty nine.
  //
  // Cell by cell there is no such overlap, because the rooms below do not
  // overlap each other.
  double lowX = 0, highX = 0, lowY = 0, highY = 0;
  bool any = false;

  for (uint32_t j = 0; j < below.rows(); ++j)
  {
    for (uint32_t i = 0; i < below.columns(); ++i)
    {
      if (below.at(i, j) != there)
      {
        continue;
      }

      lowX = any ? std::min(lowX, below.xs[i]) : below.xs[i];
      highX = any ? std::max(highX, below.xs[i + 1]) : below.xs[i + 1];
      lowY = any ? std::min(lowY, below.ys[j]) : below.ys[j];
      highY = any ? std::max(highY, below.ys[j + 1]) : below.ys[j + 1];
      any = true;
    }
  }

  if (!any || highX - lowX <= epsilon || highY - lowY <= epsilon)
  {
    return false;
  }

  // Lines from the room below, so this floor can express its shape exactly.
  for (uint32_t i = 0; i < below.xs.size(); ++i)
  {
    if (below.xs[i] >= lowX - epsilon && below.xs[i] <= highX + epsilon)
    {
      grid.insertX(below.xs[i], epsilon);
    }
  }

  for (uint32_t j = 0; j < below.ys.size(); ++j)
  {
    if (below.ys[j] >= lowY - epsilon && below.ys[j] <= highY + epsilon)
    {
      grid.insertY(below.ys[j], epsilon);
    }
  }

  const auto under = [&](const double x, const double y) {
    return labelAt(below, x, y) == there;
  };

  bool moved = false;

  for (uint32_t j = 0; j < grid.rows(); ++j)
  {
    for (uint32_t i = 0; i < grid.columns(); ++i)
    {
      const double cx = (grid.xs[i] + grid.xs[i + 1]) * .5;
      const double cy = (grid.ys[j] + grid.ys[j + 1]) * .5;

      // Outside counts too, when the room being stacked is a shaft.
      //
      // A shaft is present on every floor by definition, so where the floor
      // above has been cut back - `openToOutside` takes bites out of corners -
      // the floor has to reach out and hold it. Refusing to claim an outside
      // cell left a church tower as a four-cornered room of sixty six square
      // metres on the ground and a six-cornered one of forty eight above it:
      // the same tower with a corner missing, which is not the same tower.
      //
      // A stair does not get this: growing a floor to reach a stair would put
      // floor where the building has none, and the stair can simply move.
      const int32_t label = grid.at(i, j);
      const bool takeable = label != here && (label != kOutside || mayGrow);

      if (under(cx, cy) && takeable)
      {
        grid.set(i, j, here);
        moved = true;
      }
    }
  }

  if (!moved)
  {
    return false;
  }

  // What it was standing on goes to a neighbour, or the plan is left with a
  // hole where a room used to be.
  for (uint32_t j = 0; j < grid.rows(); ++j)
  {
    for (uint32_t i = 0; i < grid.columns(); ++i)
    {
      const double cx = (grid.xs[i] + grid.xs[i + 1]) * .5;
      const double cy = (grid.ys[j] + grid.ys[j + 1]) * .5;

      if (grid.at(i, j) != here || under(cx, cy))
      {
        continue;
      }

      int32_t taker = kOutside;

      for (int32_t d = 0; d < 4 && taker == kOutside; ++d)
      {
        const int32_t ni = int32_t(i) + (d == 0 ? -1 : d == 1 ? 1 : 0);
        const int32_t nj = int32_t(j) + (d == 2 ? -1 : d == 3 ? 1 : 0);

        if (ni < 0 || nj < 0 || ni >= int32_t(grid.columns()) || nj >= int32_t(grid.rows()))
        {
          continue;
        }

        const int32_t label = grid.at(uint32_t(ni), uint32_t(nj));

        taker = label != here && label != kOutside ? label : taker;
      }

      grid.set(i, j, taker);
    }
  }

  return true;
}

// Give a corner of the floor back to the outside.
//
// The counterpart to a wall slide growing the plan outward: this is how it
// shrinks, and it is what lets a building be an L or a T rather than whatever
// rectangle it was handed. Without it a floor is its plate for ever - the
// canonical start fills every cell and no other move makes an outside cell - so
// the area none of the rooms wants is absorbed by whichever one holds it.
//
// A corner and a rectangle, not one cell. Releasing single cells cut one-cell
// notches into the outline and the roof cannot cover those. Sized in metres and
// not in cells, because the grid's lines are wall coordinates and a cell can be
// centimetres: a bite of "one cell" took a slice too thin to roof.
bool openToOutside(Grid &grid, Rng &rng)
{
  if (grid.columns() < 3 || grid.rows() < 3)
  {
    return false;
  }

  constexpr double leastMetres = 1.5;

  const bool fromRight = rng.unit() < .5, fromTop = rng.unit() < .5;

  uint32_t wide = 1 + rng.below(std::max(1u, grid.columns() / 3));
  uint32_t tall = 1 + rng.below(std::max(1u, grid.rows() / 3));

  const auto reach = [](const std::vector<double> &lines, const uint32_t count, const bool fromEnd) {
    const size_t last = lines.size() - 1;

    return fromEnd ? lines[last] - lines[last - count] : lines[count] - lines[0];
  };

  while (wide + 1 < grid.columns() && reach(grid.xs, wide, fromRight) < leastMetres)
  {
    ++wide;
  }

  while (tall + 1 < grid.rows() && reach(grid.ys, tall, fromTop) < leastMetres)
  {
    ++tall;
  }

  bool changed = false;

  for (uint32_t dj = 0; dj < tall; ++dj)
  {
    for (uint32_t di = 0; di < wide; ++di)
    {
      const uint32_t i = fromRight ? grid.columns() - 1 - di : di;
      const uint32_t j = fromTop ? grid.rows() - 1 - dj : dj;

      if (grid.at(i, j) == kOutside)
      {
        continue;
      }

      grid.set(i, j, kOutside);
      changed = true;
    }
  }

  return changed;
}

// Hold every shaft to the same plan on every floor.
//
// A tower is one room repeated the whole height of the building at the same
// size and in the same place. A cost term can pull two floors' towers toward
// each other and does, but pulling is not holding: scored against everything
// else on the plan it settled at fifty three square metres on one floor and
// sixty four on the next, which is a pair of similar rooms and not a shaft.
//
// So it is a constraint rather than a preference. The lowest floor's tower is
// the shaft; every other floor's is put where that one is, taking the cells
// from whatever was standing there, and the caller's own validity checks then
// decide whether what is left is still a plan. That is the same bargain every
// other move in this optimiser makes.
bool lockShafts(std::vector<Grid> &grids, const Programme &programme, const Context &context,
                const double epsilon)
{
  // Grouped by what the shaft *is*, not by which floor it is on.
  //
  // This was keyed by level, which worked while a tower was the only shaft in
  // the vocabulary and broke the moment there were three: a church has a nave,
  // a chancel and a tower, all of them shafts, and one entry per level keeps
  // whichever was seen last - so floor zero's tower was being snapped onto
  // floor one's nave. The church came out with a nave of eight square metres
  // on the ground and eighty above it.
  //
  // A shaft's instances are emitted in floor order, so the use identifies the
  // shaft and the floor orders it.
  std::map<RoomUse, std::map<int32_t, std::pair<size_t, int32_t>>> shafts;

  for (uint32_t r = 0; r < programme.rooms.size(); ++r)
  {
    if (!isShaft(programme.rooms[r].use) || r >= context.gridOf.size())
    {
      continue;
    }

    const uint32_t g = context.gridOf[r];

    if (g < grids.size() && g < context.levels.size())
    {
      shafts[programme.rooms[r].use][context.levels[g]] = {size_t(g), int32_t(r)};
    }
  }

  bool moved = false;

  for (const auto &[use, byLevel] : shafts)
  {
    (void)use;

    if (byLevel.size() < 2)
    {
      continue;
    }

    const auto master = byLevel.begin();

    for (auto it = std::next(byLevel.begin()); it != byLevel.end(); ++it)
    {
      // A copy, because the move reads the master while writing the other.
      const Grid below = grids[master->second.first];

      moved = stackStair(grids[it->second.first], below, it->second.second,
                         master->second.second, epsilon, true)
              || moved;
    }
  }

  return moved;
}

// How private a room is, which is what decides the kind of opening into it.
// How much of the building's life a room carries, as a multiplier on its
// dimension cost. One is an ordinary room.
//
// Not the same question as privacy, which is about who is admitted and decides
// whether an opening is a door or an arch. This is about how much it matters
// that the room comes out the size it asked for.
double useWeight(const RoomUse use)
{
  switch (use)
  {
    // The rooms a household spends its day in, and the ones a building exists
    // for: get these right first.
    case RoomUse::Hall:
    case RoomUse::LivingRoom:
    case RoomUse::Kitchen:
    case RoomUse::Parlour:
    case RoomUse::DiningRoom:
    case RoomUse::DrawingRoom:
    case RoomUse::Bedroom:
    case RoomUse::Shop:
    case RoomUse::TapRoom:
    case RoomUse::Workshop:
    case RoomUse::Nave:
    case RoomUse::GreatHall:
      return 1.6;

    // Where the compromises should land: a larder half a metre short is a
    // larder, a hall half a metre short is a corridor.
    case RoomUse::Closet:
    case RoomUse::Store:
    case RoomUse::Larder:
    case RoomUse::Pantry:
    case RoomUse::Scullery:
    case RoomUse::WaterCloset:
      return .65;

    default:
      return 1.;
  }
}

uint32_t privacy(const RoomUse use)
{
  switch (use)
  {
    // Public: the rooms a visitor is taken to, and the ways to them.
    case RoomUse::Hall:
    case RoomUse::Passage:
    case RoomUse::Landing:
    case RoomUse::Staircase:
    case RoomUse::Lobby:
    case RoomUse::Porch:
    case RoomUse::Narthex:
    case RoomUse::ScreensPassage:
    case RoomUse::Gallery:
    case RoomUse::Cloister:
    case RoomUse::Aisle:
    case RoomUse::DiningRoom:
    case RoomUse::DrawingRoom:
    case RoomUse::Parlour:
    case RoomUse::LivingRoom:
    case RoomUse::Saloon:
    case RoomUse::Ballroom:
    case RoomUse::GreatHall:
    case RoomUse::Nave:
    case RoomUse::Shop:
    case RoomUse::Bar:
    case RoomUse::TapRoom:
    case RoomUse::SchoolRoom:
    case RoomUse::WaitingRoom:
      return 0;

    // Private: bedrooms and what opens off them, and the sanitary rooms.
    case RoomUse::Bedroom:
    case RoomUse::DressingRoom:
    case RoomUse::Nursery:
    case RoomUse::Boudoir:
    case RoomUse::Closet:
    case RoomUse::WaterCloset:
    case RoomUse::Bathroom:
    case RoomUse::Solar:
    case RoomUse::Cell:
    case RoomUse::StrongRoom:
      return 2;

    // Semi-private: everything else - the study, the library, the service
    // rooms, the conservatory.
    default:
      return 1;
  }
}

PassageType passageFor(const RoomUse a, const RoomUse b, const bool open)
{
  const uint32_t most = std::max(privacy(a), privacy(b));

  if (most >= 2)
  {
    // Anything touching a private room gets a door, whatever the programme
    // asked: an open wall onto a bedroom is not a plan.
    return PassageType::Door;
  }

  if (most == 0)
  {
    return open ? PassageType::WidePassage : PassageType::Door;
  }

  return open ? PassageType::FrenchDoor : PassageType::Door;
}

// §6: "It is customary to place passageways near room corners. ('When [a door]
// is in the middle of the wall, it almost always creates a pattern of movement
// which breaks the room in two, destroys the center, and leaves no single area
// which is large enough to use' [Alexander et al. 1977].) We place the door in
// a location that minimizes the sum of the distances from the door to the
// nearest corner in each of the adjacent rooms."

// The longest run of a room's wall that faces outside, preferring the side the
// street lies on.
//
// The same maximal-run idea as sharedWall, against kOutside rather than
// against another room: a front door goes in the front wall, and the front
// wall is the one the street can see.
double outsideWall(const Grid &grid,
                   const int32_t label,
                   const double streetX,
                   const double streetZ,
                   WallRun &best,
                   double &normalX,
                   double &normalZ)
{
  double bestScore = -1;

  const auto consider = [&](const WallRun &run, const double nx, const double nz) {
    if (run.length < 1e-6)
    {
      return;
    }

    // Length, and a bonus for facing the street: a long back wall is not a
    // front, and a short front is still the front.
    const double facing = nx * streetX + nz * streetZ;
    const double score = run.length * (1. + std::max(0., facing) * 1.5);

    if (score > bestScore)
    {
      bestScore = score;
      best = run;
      normalX = nx;
      normalZ = nz;
    }
  };

  const auto at = [&](const int32_t i, const int32_t j) {
    return i >= 0 && j >= 0 && i < int32_t(grid.columns()) && j < int32_t(grid.rows())
             ? grid.at(uint32_t(i), uint32_t(j))
             : kOutside;
  };

  // Vertical runs, then horizontal.
  for (uint32_t i = 0; i <= grid.columns(); ++i)
  {
    for (int32_t side = -1; side <= 1; side += 2)
    {
      uint32_t j = 0;

      while (j < grid.rows())
      {
        const bool wall = side < 0 ? at(int32_t(i), int32_t(j)) == label
                                       && at(int32_t(i) - 1, int32_t(j)) == kOutside
                                   : at(int32_t(i) - 1, int32_t(j)) == label
                                       && at(int32_t(i), int32_t(j)) == kOutside;

        if (!wall)
        {
          ++j;

          continue;
        }

        const uint32_t from = j;

        while (j < grid.rows()
               && (side < 0 ? at(int32_t(i), int32_t(j)) == label
                                && at(int32_t(i) - 1, int32_t(j)) == kOutside
                            : at(int32_t(i) - 1, int32_t(j)) == label
                                && at(int32_t(i), int32_t(j)) == kOutside))
        {
          ++j;
        }

        WallRun run;
        run.x0 = run.x1 = grid.xs[i];
        run.y0 = grid.ys[from];
        run.y1 = grid.ys[j];
        run.length = run.y1 - run.y0;

        consider(run, side < 0 ? -1. : 1., 0.);
      }
    }
  }

  for (uint32_t j = 0; j <= grid.rows(); ++j)
  {
    for (int32_t side = -1; side <= 1; side += 2)
    {
      uint32_t i = 0;

      while (i < grid.columns())
      {
        const auto isWall = [&](const uint32_t at_i) {
          return side < 0 ? at(int32_t(at_i), int32_t(j)) == label
                              && at(int32_t(at_i), int32_t(j) - 1) == kOutside
                          : at(int32_t(at_i), int32_t(j) - 1) == label
                              && at(int32_t(at_i), int32_t(j)) == kOutside;
        };

        if (!isWall(i))
        {
          ++i;

          continue;
        }

        const uint32_t from = i;

        while (i < grid.columns() && isWall(i))
        {
          ++i;
        }

        WallRun run;
        run.y0 = run.y1 = grid.ys[j];
        run.x0 = grid.xs[from];
        run.x1 = grid.xs[i];
        run.length = run.x1 - run.x0;

        consider(run, 0., side < 0 ? -1. : 1.);
      }
    }
  }

  return bestScore < 0 ? 0. : best.length;
}

void placeDoor(const WallRun &run,
               const std::vector<std::pair<double, double>> &cornersA,
               const std::vector<std::pair<double, double>> &cornersB,
               const double doorWidth, double &outX, double &outZ)
{
  const double dx = run.x1 - run.x0;
  const double dy = run.y1 - run.y0;
  const double length = std::sqrt(dx * dx + dy * dy);

  if (length < 1e-9)
  {
    outX = run.x0;
    outZ = run.y0;

    return;
  }

  // The door has to fit in the wall, so its centre cannot reach the ends.
  const double margin = std::min(doorWidth * .5, length * .45);
  const double low = margin / length;
  const double high = 1. - margin / length;

  const auto nearestCorner = [](const std::vector<std::pair<double, double>> &corners,
                                const double x, const double y) {
    double best = 1e30;

    for (const auto &corner : corners)
    {
      const double ax = corner.first - x;
      const double ay = corner.second - y;
      best = std::min(best, std::sqrt(ax * ax + ay * ay));
    }

    return corners.empty() ? 0. : best;
  };

  double bestT = (low + high) * .5;
  double bestCost = 1e30;

  // Sampled rather than solved: the objective is a sum of two piecewise-conic
  // functions of position along the wall, and thirty-three samples over a wall
  // a few metres long put the door within a few centimetres of the optimum -
  // finer than the corner it is being placed near.
  const uint32_t samples = 33;

  for (uint32_t k = 0; k <= samples; ++k)
  {
    const double t = low + (high - low) * double(k) / double(samples);
    const double x = run.x0 + dx * t;
    const double y = run.y0 + dy * t;
    const double cost = nearestCorner(cornersA, x, y) + nearestCorner(cornersB, x, y);

    if (cost < bestCost)
    {
      bestCost = cost;
      bestT = t;
    }
  }

  outX = run.x0 + dx * bestT;
  outZ = run.y0 + dy * bestT;
}

}  // namespace

const char *name(const PassageType passage)
{
  switch (passage)
  {
    case PassageType::Door: return "door";
    case PassageType::FrenchDoor: return "french doors";
    case PassageType::WidePassage: return "wide passage";
  }

  return "?";
}


bool optimiseFloorPlan(const Programme &programme,
                       const std::map<int32_t, std::vector<std::pair<double, double>>> &lots,
                       const uint64_t seed,
                       const FloorPlanSettings &settings,
                       FloorPlan &out,
                       std::string *error,
                       const std::vector<std::pair<double, double>> &site)
{
  const auto fail = [error](const char *why) {
    if (error != nullptr)
    {
      *error = why;
    }

    return false;
  };

  out = FloorPlan{};
  out.programme = programme;

  // --- which rooms are on which floor -------------------------------------

  std::map<int32_t, std::vector<uint32_t>> byFloor;

  for (uint32_t r = 0; r < programme.rooms.size(); ++r)
  {
    // The yard is outside; it is a room of the programme and not of the plan.
    if (enclosesFloorArea(programme.rooms[r].use))
    {
      byFloor[programme.rooms[r].floor].push_back(r);
    }
  }

  if (byFloor.empty())
  {
    return fail("the programme has no rooms to place");
  }

  Context context;
  context.programme = &programme;
  context.settings = &settings;
  context.gridOf.assign(programme.rooms.size(), uint32_t(-1));

  for (const auto &entry : lots)
  {
    if (entry.second.size() >= 3)
    {
      context.lots[entry.first] = rasterise(entry.second);
    }
  }

  if (site.size() >= 3)
  {
    const LotMask plot = rasterise(site);

    if (context.lots.empty())
    {
      int32_t bottom = byFloor.begin()->first;
      context.lots[bottom] = plot;
    }
    else
    {
      // Every plate, trimmed to the plot. The plate is what the building may
      // be; the plot is where it may be; a floor has to satisfy both.
      for (auto &entry : context.lots)
      {
        LotMask &mask = entry.second;

        for (uint32_t j = 0; j < mask.rows; ++j)
        {
          const double y =
            mask.lowY + (mask.highY - mask.lowY) * (double(j) + .5) / double(mask.rows);

          for (uint32_t i = 0; i < mask.columns; ++i)
          {
            const double x =
              mask.lowX + (mask.highX - mask.lowX) * (double(i) + .5) / double(mask.columns);

            if (!plot.inside(x, y))
            {
              mask.mask[size_t(j) * mask.columns + i] = 0;
            }
          }
        }
      }
    }
  }

  // --- the canonical high-cost start --------------------------------------
  //
  // "The optimization begins from a canonical high-cost configuration, in
  // which equally-sized rectangular rooms are packed on a grid on each floor."

  std::vector<Grid> grids;
  Rng rng(seed ^ 0x2545F4914F6CDD1Dull);

  // Where each floor was packed, so the floor above can be packed inside it.
  struct Start
  {
    double originX = 0, originZ = 0, spanX = 0, spanZ = 0;
  };

  std::map<int32_t, Start> starts;

  double widest = 0;

  for (const auto &entry : byFloor)
  {
    double asked = 0;

    for (const uint32_t r : entry.second)
    {
      asked += programme.rooms[r].areaMetres;
    }

    widest = std::max(widest, asked);
  }

  const double aspect =
    programme.footprintZ > 1e-6 ? programme.footprintX / programme.footprintZ : 1.;


  for (const auto &entry : byFloor)
  {
    const std::vector<uint32_t> &rooms = entry.second;

    double asked = 0;

    for (const uint32_t r : rooms)
    {
      asked += programme.rooms[r].areaMetres;
    }

    // Big enough for every room to exist in. A start that already has rooms
    // under the minimum is not a start at all: every move is refused for
    // killing a room that was dead on arrival, and the plan never moves.
    const double area =
      std::max({asked, 6., double(rooms.size()) * settings.minimumRoomMetres * 1.4});
    double spanX = std::sqrt(area * aspect);
    double spanZ = area / std::max(spanX, 1e-6);

    double originX = -spanX * .5;
    double originZ = -spanZ * .5;

    // ...and inside the floor below it.
    //
    // The canonical start packs each floor into a box sized for its own rooms,
    // and an upper floor that asks for more area than the ground floor - or
    // for a different shape - starts hanging over the edge of it. Measured,
    // that start had forty to two hundred square metres in the air before a
    // single move, and neither the cost term nor the refusal can recover it:
    // the term is diluted across the whole building and the refusal only stops
    // the overhang *growing*. The floors are built in order from the bottom,
    // so the fix is to hand each one the box the one below it settled on.
    // Only a floor that has a storey under it. A ground floor is not packed
    // inside its own cellar - the cellar is under part of the house, not the
    // other way round - and clamping it there was what produced a house with a
    // ground floor the size of its coal store.
    const auto under = starts.find(entry.first - 1);

    if (entry.first > 0 && under != starts.end())
    {
      const Start &below = under->second;

      spanX = std::min(spanX, below.spanX);
      spanZ = std::min(spanZ, below.spanZ);
      originX = std::min(std::max(originX, below.originX), below.originX + below.spanX - spanX);
      originZ = std::min(std::max(originZ, below.originZ), below.originZ + below.spanZ - spanZ);
    }

    // The ground this floor starts on. It is the lot a move may never leave,
    // so the start has to be inside it - and it decides how much of the
    // building the search can ever reach, because a wall slide can grow a room
    // into ground the plan already stands on and nowhere else.
    const LotMask *ground = context.groundFor(entry.first);

    if (ground != nullptr)
    {
      // Sized for what the rooms ask for and laid over the whole plate, not
      // over the largest rectangle inside it. An L-shaped plate's inscribed
      // rectangle is about half of it, and a plan that starts there stays
      // there: to reach an arm a room must first grow across ground that is
      // not its own, which the lot constraint refuses. Starting over the
      // whole plate and cutting away what is off it puts the plan in both
      // arms from the first iteration.
      spanX = std::min(spanX, ground->highX - ground->lowX);
      spanZ = std::min(spanZ, ground->highY - ground->lowY);
      originX = (ground->lowX + ground->highX) * .5 - spanX * .5;
      originZ = (ground->lowY + ground->highY) * .5 - spanZ * .5;

    }

    // Where to put that box. Against the street first, centred on the ground
    // second: a house stands on its street rather than in the middle of its
    // plot, but a frontage that costs the plan a room is not worth having, so
    // the centred placement is there to fall back on.
    std::vector<std::pair<double, double>> origins{{originX, originZ}};

    if (ground != nullptr
        && (std::abs(settings.streetX) > 1e-9 || std::abs(settings.streetZ) > 1e-9))
    {
      std::pair<double, double> frontage{originX, originZ};

      if (std::abs(settings.streetX) > std::abs(settings.streetZ))
      {
        frontage.first = settings.streetX > 0 ? ground->highX - spanX : ground->lowX;
      }
      else
      {
        frontage.second = settings.streetZ > 0 ? ground->highY - spanZ : ground->lowY;
      }

      origins.insert(origins.begin(), frontage);
    }

    const uint32_t count = uint32_t(rooms.size());
    const uint32_t columns = std::max(1u, uint32_t(std::ceil(std::sqrt(double(count)))));
    const uint32_t rowCount = std::max(1u, (count + columns - 1) / columns);

    starts[entry.first] = {origins.front().first, origins.front().second, spanX, spanZ};

    // One boustrophedon packing of equal rooms, at the given corner.
    const auto pack = [&](const double atX, const double atZ) {
      Grid grid;

      for (uint32_t i = 0; i <= columns; ++i)
      {
        grid.xs.push_back(atX + spanX * double(i) / double(columns));
      }

      for (uint32_t j = 0; j <= rowCount; ++j)
      {
        grid.ys.push_back(atZ + spanZ * double(j) / double(rowCount));
      }

      grid.label.assign(size_t(columns) * rowCount, kOutside);

      // Boustrophedon, so consecutive rooms are neighbours and the programme's
      // doors start with somewhere to be.
      uint32_t placed = 0;

      for (uint32_t j = 0; j < rowCount && placed < count; ++j)
      {
        for (uint32_t k = 0; k < columns && placed < count; ++k)
        {
          const uint32_t i = j % 2 == 0 ? k : columns - 1 - k;
          grid.set(i, j, int32_t(rooms[placed]));
          context.gridOf[rooms[placed]] = uint32_t(grids.size());
          ++placed;
        }
      }

      // Any cell left over goes to the room before it, so the floor is solid.
      for (uint32_t j = 0; j < rowCount; ++j)
      {
        for (uint32_t i = 0; i < columns; ++i)
        {
          if (grid.at(i, j) != kOutside)
          {
            continue;
          }

          const int32_t left = i > 0 ? grid.at(i - 1, j) : kOutside;
          const int32_t below = j > 0 ? grid.at(i, j - 1) : kOutside;
          grid.set(i, j, left != kOutside ? left : below);
        }
      }

      return grid;
    };

    Grid grid = pack(origins.front().first, origins.front().second);

    // Cut away what is off the ground: a cell not wholly on the plate becomes
    // outside, which is the plan reaching into the arms of an L and stopping
    // at their edges. Tried at each candidate corner in turn.
    if (ground != nullptr)
    {
      bool standing = false;

      for (const auto &origin : origins)
      {
        Grid clipped = pack(origin.first, origin.second);

        // Refined first, because a start cell is a quarter of a building and
        // a cell is kept or dropped whole: against the raw start grid,
        // "inside" means the middle of a room is on the plate and the rest of
        // it hangs over the street. The extra lines cost nothing for long -
        // the first accepted move coalesces away every one the plan does not
        // use.
        for (double x = clipped.xs.front() + .4; x < clipped.xs.back(); x += .4)
        {
          clipped.insertX(x, settings.gridEpsilonMetres);
        }

        for (double y = clipped.ys.front() + .4; y < clipped.ys.back(); y += .4)
        {
          clipped.insertY(y, settings.gridEpsilonMetres);
        }

        for (uint32_t j = 0; j < clipped.rows(); ++j)
        {
          for (uint32_t i = 0; i < clipped.columns(); ++i)
          {
            // Every corner, not the centre: a cell kept because its middle is
            // on the plate hangs half its width over the edge, and half a
            // cell is a wall's worth of building in the street.
            const bool covered =
              ground->inside(clipped.xs[i], clipped.ys[j])
              && ground->inside(clipped.xs[i + 1], clipped.ys[j])
              && ground->inside(clipped.xs[i], clipped.ys[j + 1])
              && ground->inside(clipped.xs[i + 1], clipped.ys[j + 1]);

            if (!covered)
            {
              clipped.set(i, j, kOutside);
            }
          }
        }

        // ...unless it costs a room its existence: a start that has already
        // lost a room cannot be recovered from, because every move that would
        // give it back is refused for killing it again.
        bool alive = true;

        for (const uint32_t r : rooms)
        {
          alive = alive && areaOf(clipped, int32_t(r)) >= settings.minimumRoomMetres
                  && widthOf(clipped, int32_t(r)) >= settings.minimumWidthMetres
                  && connected(clipped, int32_t(r)) && !pinched(clipped, int32_t(r))
                  && !encloses(clipped, int32_t(r));
        }

        if (alive)
        {
          coalesce(clipped);
          grid = std::move(clipped);
          standing = true;

          break;
        }
      }

      if (!standing)
      {
        // Fall back to the largest rectangle that fits the ground. It is a
        // worse start - the plan can never reach past it - but it is a legal
        // one, and the alternative is a building that begins in the street
        // and is only ever forbidden from going further into it.
        double boxLowX = 0, boxHighX = 0, boxLowY = 0, boxHighY = 0;
        inscribedBox(*ground, boxLowX, boxHighX, boxLowY, boxHighY);

        // ...still inside the floor below. The inscribed box is taken from
        // this floor's own plate, which for an upper floor is as large as the
        // ground floor's; falling back to it undid the containment the start
        // had just been clamped into, and put the floor back in the air.
        if (entry.first > 0 && under != starts.end())
        {
          boxLowX = std::max(boxLowX, under->second.originX);
          boxHighX = std::min(boxHighX, under->second.originX + under->second.spanX);
          boxLowY = std::max(boxLowY, under->second.originZ);
          boxHighY = std::min(boxHighY, under->second.originZ + under->second.spanZ);
        }

        const double fitX = std::min(spanX, std::max(boxHighX - boxLowX, 1e-6));
        const double fitZ = std::min(spanZ, std::max(boxHighY - boxLowY, 1e-6));

        grid = pack((boxLowX + boxHighX) * .5 - fitX * .5, (boxLowY + boxHighY) * .5 - fitZ * .5);

        for (uint32_t i = 0; i < grid.xs.size(); ++i)
        {
          grid.xs[i] = (boxLowX + boxHighX) * .5 - fitX * .5
                       + fitX * double(i) / double(grid.xs.size() - 1);
        }

        for (uint32_t j = 0; j < grid.ys.size(); ++j)
        {
          grid.ys[j] = (boxLowY + boxHighY) * .5 - fitZ * .5
                       + fitZ * double(j) / double(grid.ys.size() - 1);
        }
      }
    }

    // The stair is one shaft, so start it as one.
    //
    // §7 names multi-story spaces as "the chief cause for these unsuccessful
    // optimizations... which couple the optimization on multiple floors", and
    // the failure it shows is a staircase blocked on the second story. The
    // boustrophedon start puts each floor's stair wherever that floor's room
    // order happens to place it, which is a different corner on every floor -
    // so the search begins by having to walk a stair the length of the
    // building, one wall slide at a time, against a cost that is flat until
    // it arrives. Stacking them costs one label swap.
    if (!grids.empty())
    {
      const auto climbOf = [&](const std::vector<uint32_t> &of) {
        uint32_t found = uint32_t(-1);

        for (const uint32_t r : of)
        {
          const RoomUse use = programme.rooms[r].use;

          if (use != RoomUse::Staircase && use != RoomUse::Landing)
          {
            continue;
          }

          if (found == uint32_t(-1)
              || (use == RoomUse::Staircase
                  && programme.rooms[found].use == RoomUse::Landing))
          {
            found = r;
          }
        }

        return found;
      };

      std::vector<uint32_t> belowRooms;

      for (uint32_t r = 0; r < programme.rooms.size(); ++r)
      {
        if (context.gridOf[r] == uint32_t(grids.size() - 1))
        {
          belowRooms.push_back(r);
        }
      }

      const uint32_t here = climbOf(rooms);
      const uint32_t under = climbOf(belowRooms);

      if (here != uint32_t(-1) && under != uint32_t(-1))
      {
        double atX = 0, atZ = 0;

        if (centroidOf(grids.back(), int32_t(under), atX, atZ))
        {
          const int32_t standing = labelAt(grid, atX, atZ);

          if (standing != kOutside && standing != int32_t(here))
          {
            // Swap the two labels, which keeps both rooms' cells and moves the
            // stair over the flight below it.
            for (int32_t &label : grid.label)
            {
              label = label == standing         ? int32_t(here)
                      : label == int32_t(here)  ? standing
                                                : label;
            }
          }
        }
      }
    }

    context.levels.push_back(entry.first);
    grids.push_back(std::move(grid));
  }

  // --- what the cost function is measured against --------------------------

  for (const Adjacency &link : programme.links)
  {
    if (link.a >= context.gridOf.size() || link.b >= context.gridOf.size())
    {
      continue;
    }

    const uint32_t ga = context.gridOf[link.a];
    const uint32_t gb = context.gridOf[link.b];

    // A link between floors is made by the stair, not by a wall.
    if (ga == uint32_t(-1) || gb == uint32_t(-1) || ga != gb)
    {
      continue;
    }

    context.links.push_back({link.a, link.b, !link.door});
  }

  for (uint32_t r = 0; r < programme.rooms.size(); ++r)
  {
    if (context.gridOf[r] == uint32_t(-1))
    {
      continue;
    }

    const RoomUse use = programme.rooms[r].use;

    if (programme.rooms[r].frontsStreet || use == RoomUse::Porch
        || (use == RoomUse::Hall && programme.rooms[r].floor == 0))
    {
      context.mustReachOutside.push_back(r);
    }

    if (programme.rooms[r].frontsStreet && programme.rooms[r].floor == 0
        && (std::abs(settings.streetX) > 1e-9 || std::abs(settings.streetZ) > 1e-9))
    {
      context.mustFaceStreet.push_back(r);
    }
  }

  if (context.mustReachOutside.empty())
  {
    // No porch, no hall, nothing fronting the street: a cottage. Its front
    // door is in the room you walk into, which is the house-place, and the
    // house-place is the largest room on the ground floor. Every building has
    // a way in, and one with none is not a modest result - it is a mistake
    // that everything downstream inherits.
    uint32_t best = uint32_t(-1);

    for (uint32_t r = 0; r < programme.rooms.size(); ++r)
    {
      if (context.gridOf[r] == uint32_t(-1) || programme.rooms[r].floor != 0
          || !enclosesFloorArea(programme.rooms[r].use))
      {
        continue;
      }

      const bool circulation = programme.rooms[r].use == RoomUse::Staircase
                               || programme.rooms[r].use == RoomUse::Landing;

      if (circulation)
      {
        continue;
      }

      if (best == uint32_t(-1)
          || programme.rooms[r].areaMetres > programme.rooms[best].areaMetres)
      {
        best = r;
      }
    }

    if (best != uint32_t(-1))
    {
      context.mustReachOutside.push_back(best);
    }
  }

  // The vertical circulation of each floor, in grid order.
  context.climbs.assign(grids.size(), uint32_t(-1));

  for (uint32_t r = 0; r < programme.rooms.size(); ++r)
  {
    const uint32_t which = context.gridOf[r];

    if (which == uint32_t(-1) || which >= context.climbs.size())
    {
      continue;
    }

    const RoomUse use = programme.rooms[r].use;

    if (use != RoomUse::Staircase && use != RoomUse::Landing)
    {
      continue;
    }

    // One per floor: the staircase where there is one, the landing otherwise.
    if (context.climbs[which] == uint32_t(-1)
        || (use == RoomUse::Staircase
            && programme.rooms[context.climbs[which]].use == RoomUse::Landing))
    {
      context.climbs[which] = r;
    }
  }

  out.doorsAsked = uint32_t(context.links.size());

  // --- the Metropolis loop -------------------------------------------------
  //
  //   f(x) = exp(-B C(x)),  a(x*|x) = min(1, exp(B (C(x) - C(x*))))
  //
  // with B annealed, because "annealing beta over time facilitates a more
  // rapid exploration of the space".

  LayoutCost cost = evaluate(grids, context);
  out.startCost = cost;

  std::vector<Grid> best = grids;
  LayoutCost bestCost = cost;

  const auto violations = [&](const std::vector<Grid> &floors) {
    uint32_t broken = 0;

    for (const Grid &floor : floors)
    {
      broken += floorConnected(floor) ? 0u : 1u;
      broken += floorPinched(floor) ? 1u : 0u;
    }

    for (uint32_t r = 0; r < programme.rooms.size(); ++r)
    {
      const uint32_t on = context.gridOf[r];

      if (on == uint32_t(-1) || on >= floors.size())
      {
        continue;
      }

      const RoomUse use = programme.rooms[r].use;
      const bool circulation = use == RoomUse::Passage || use == RoomUse::Landing
                               || use == RoomUse::Hall || use == RoomUse::Lobby
                               || use == RoomUse::ScreensPassage;
      const double narrowest =
        circulation ? std::max(settings.minimumWidthMetres, settings.minimumPassageMetres)
        : habitable(use)
          ? std::max(settings.minimumWidthMetres, settings.minimumHabitableMetres)
          : settings.minimumWidthMetres;

      const double got = areaOf(floors[on], int32_t(r));

      // A stair is the size of a stair.
      //
      // The floor has to tile its plate, so whatever area the rooms do not
      // want is absorbed by whichever of them is cheapest to grow - and that
      // is circulation, because the dimension term is weighted 1 against
      // accessibility's 12 and the floor term's 90, so a staircase ten times
      // its drawn size costs less than a tenth of one missing door. A church
      // asked for five square metres of stair and got fifty three; a manor
      // asked for ten and got eighty four, wrapped round three sides of its
      // great hall.
      //
      // Stated as a rule rather than paid for as a cost, because it is not a
      // preference - there is no plan in which a fifty three square metre
      // staircase is the right answer. Safe to state now that violations are
      // counted rather than flagged: a start that breaks it can walk out.
      if (circulation || use == RoomUse::Staircase)
      {
        const double asked = programme.rooms[r].areaMetres;
        const double most = std::max(asked * 2.5, settings.minimumRoomMetres * 3.);

        broken += asked > 0 && got > most ? 1u : 0u;
      }

      // A staircase has to hold a flight.
      //
      // Its shape is not a matter of taste: a storey of about three metres is
      // fifteen risers, and fifteen goings is near four metres of run, so a
      // stair is a room about a metre by four, or half that twice over for a
      // dog-leg. The inn's came out 8.97 by 0.95 - a ratio of nine and a half -
      // which is not a staircase, it is a corridor with a name, and nothing can
      // climb it. The aspect likelihood cannot say so on its own: it is
      // weighted 1 against accessibility's 12, the same reason the areas
      // drifted until they were priced.
      // ...and a room somebody lives in has to be a room, not a ribbon.
      //
      // `minimumWidthMetres` is the *harmonic* width, twice the area over the
      // perimeter, and a long thin room defeats it: a farmstead's kitchen came
      // out 19.83 by 1.06 metres, a ratio of nearly nineteen, and scored 1.005
      // against a habitable minimum of 1.0. It passed by five thousandths of a
      // metre while being a corridor with a stove in it.
      //
      // Circulation is exempt because a passage *is* long and thin - that is
      // what a passage is - and so are the service rooms, where a slot of a
      // larder is ordinary. The rule is for the rooms a person is in.
      if (habitable(use) && !circulation)
      {
        double lowX = 0, highX = 0, lowY = 0, highY = 0;

        if (boundsOf(floors[on], int32_t(r), lowX, highX, lowY, highY))
        {
          const double wide = highX - lowX, tall = highY - lowY;
          const double shorter = std::min(wide, tall);

          broken += shorter > 1e-6 && std::max(wide, tall) / shorter > 3.5 ? 1u : 0u;
        }
      }

      // A tower is square, or near enough.
      //
      // Not a preference: a tower is a shaft carried the height of a building
      // and then some, and what carries it is four walls of roughly the same
      // length buttressing each other. Stretched it is a wall with a roof on -
      // a church's came out 14.35 by 3.31 metres, a ratio of four and a third,
      // standing beside a nave it was three times the length of.
      //
      // The aspect likelihood cannot hold it, for the reason the areas drifted
      // until they were priced: it is weighted 1 against accessibility's 12.
      if (use == RoomUse::Tower)
      {
        double lowX = 0, highX = 0, lowY = 0, highY = 0;

        if (boundsOf(floors[on], int32_t(r), lowX, highX, lowY, highY))
        {
          const double wide = highX - lowX, tall = highY - lowY;
          const double shorter = std::min(wide, tall);

          broken += shorter > 1e-6 && std::max(wide, tall) / shorter > 1.4 ? 1u : 0u;

          // ...and a rectangle, not a shape with a square-ish box around it.
          //
          // Holding the bounding box near square is not enough: an L of two
          // arms has a square box, and once a shaft was allowed to reach out
          // and claim the ground it needs on each floor, that is exactly what
          // it grew into - a twelve-cornered blob eighteen metres by thirteen,
          // measuring 1.37 on the aspect rule and passing it. Four walls
          // buttressing each other is what carries a tower, and four walls make
          // a rectangle.
          broken += got < wide * tall * .92 ? 1u : 0u;
        }
      }

      if (use == RoomUse::Staircase)
      {
        double lowX = 0, highX = 0, lowY = 0, highY = 0;

        if (boundsOf(floors[on], int32_t(r), lowX, highX, lowY, highY))
        {
          const double wide = highX - lowX, tall = highY - lowY;
          const double longer = std::max(wide, tall), shorter = std::min(wide, tall);

          broken += shorter > 1e-6 && longer / shorter > 4. ? 1u : 0u;
          broken += longer > 6. ? 1u : 0u;
        }
      }

      broken += got >= settings.minimumRoomMetres ? 0u : 1u;
      broken += widthOf(floors[on], int32_t(r)) >= narrowest ? 0u : 1u;
      broken += connected(floors[on], int32_t(r)) ? 0u : 1u;
      broken += pinched(floors[on], int32_t(r)) ? 1u : 0u;
      broken += encloses(floors[on], int32_t(r)) ? 1u : 0u;
    }

    return broken;
  };

  // The current layout's count, kept rather than recomputed.
  //
  // It only changes when a move is accepted, and recomputing it every
  // iteration walks every cell of every floor twice per step - which took the
  // hundred thousand iterations the paper runs from two and a half seconds to
  // nearly seven.
  // The shafts are locked before the search begins, not only on each candidate.
  //
  // Locking candidates alone is not enough to guarantee a locked result: a
  // candidate that is rejected leaves the previous state standing, and the
  // canonical start has never been locked, so a run whose proposals are mostly
  // refused ends where it began - with a nave of a hundred and twenty two
  // square metres on one floor and seventy nine on the next.
  //
  // Locked at the start and on every candidate, every state the chain can be in
  // is locked, by induction: the first is, and each later one is either a
  // locked candidate or the state before it.
  lockShafts(grids, programme, context, settings.gridEpsilonMetres);

  uint32_t broken = violations(grids);

  for (uint32_t step = 0; step < settings.iterations; ++step)
  {
    const double t = double(step) / double(std::max(1u, settings.iterations - 1));
    const double beta = settings.betaStart * std::pow(settings.betaEnd / settings.betaStart, t);

    const uint32_t which = rng.below(uint32_t(grids.size()));
    Grid proposal = grids[which];

    // Lines on the floors either side, so a slide can align across floors.
    std::vector<double> neighbourX, neighbourY;

    for (size_t g = 0; g < grids.size(); ++g)
    {
      if (g == which)
      {
        continue;
      }

      neighbourX.insert(neighbourX.end(), grids[g].xs.begin(), grids[g].xs.end());
      neighbourY.insert(neighbourY.end(), grids[g].ys.begin(), grids[g].ys.end());
    }

    // Which floor stands under this one, and the circulation on each - the
    // stack move needs both, and on a single storey plan there is no such
    // floor, so it costs nothing to look.
    uint32_t under = uint32_t(-1);

    for (size_t g = 0; g < grids.size(); ++g)
    {
      under = context.levels[g] == context.levels[which] - 1 ? uint32_t(g) : under;
    }

    // What this floor stacks by: its tower if it has one, else its stair.
    //
    // A tower is a shaft - the same room, the same size, on every floor - so
    // it is the strongest thing on a plan to align, and aligning it is the
    // whole of what makes it a tower rather than a column of similar rooms.
    // Failing that, the stair, which is the case this move was written for.
    const auto circulationOn = [&](const uint32_t g) {
      int32_t found = kOutside, stair = kOutside;

      for (uint32_t r = 0; r < programme.rooms.size(); ++r)
      {
        if (r >= context.gridOf.size() || context.gridOf[r] != g)
        {
          continue;
        }

        const RoomUse use = programme.rooms[r].use;

        if (use == RoomUse::Tower)
        {
          return int32_t(r);
        }

        stair = use == RoomUse::Staircase ? int32_t(r) : stair;
        found = use == RoomUse::Landing ? int32_t(r) : found;
      }

      return stair != kOutside ? stair : found;
    };

    const double roll = rng.unit();

    const bool stacking = roll < settings.stackChance && under != uint32_t(-1)
                          && circulationOn(which) != kOutside && circulationOn(under) != kOutside;

    const bool moved = stacking
                         ? stackStair(proposal, grids[under], circulationOn(which),
                                      circulationOn(under), settings.gridEpsilonMetres, false)
                       : roll < settings.stackChance + settings.swapChance
                         ? swapRooms(proposal, rng)
                       : roll < settings.stackChance + settings.swapChance + settings.openChance
                         ? openToOutside(proposal, rng)
                         : slideWall(proposal, settings, rng, neighbourX, neighbourY);

    if (!moved)
    {
      continue;
    }

    // Back to the arrangement of the walls, before anything measures it.
    coalesce(proposal);

    std::vector<Grid> candidate = grids;
    candidate[which] = std::move(proposal);

    // Whatever the move did, the shafts still line up afterwards.
    lockShafts(candidate, programme, context, settings.gridEpsilonMetres);

    // A room squeezed out of existence is not a layout, and neither is a room
    // cut in two - the paper's rooms are rectilinear polygons, and a polygon
    // is one piece. Checked after the stair is stacked, because stacking it
    // takes cells from whatever was standing there and that is exactly the
    // kind of move that can cut a room in half.
    // How many rules the layout breaks, rather than whether it breaks any.
    //
    // This was a boolean, and a boolean freezes the search whenever the
    // canonical start is itself illegal. It packs equally-sized rooms on a
    // grid, so on a plan with many rooms and little ground the cells come out
    // narrow: the inn's were 1.3 by 4.1 metres, a harmonic width of 0.987
    // against a habitable minimum of 1.0. Every candidate inherited the
    // violation, every candidate was refused, and thirty thousand iterations
    // later the "optimised" plan was the start - cost 372.3 in and 372.3 out,
    // with a third of its doors delivered. The shop house was the same.
    //
    // Counted, the rule still binds - a move that breaks more than the state
    // it came from is refused, so the search can only ever walk toward a legal
    // plan - and a start that is already illegal can walk out of it instead of
    // standing there.
    bool alive = violations(candidate) <= broken;


    for (uint32_t r = 0; r < 0; ++r)
    {
      const uint32_t on = context.gridOf[r];

      if (on == uint32_t(-1) || on >= candidate.size())
      {
        continue;
      }

      // Circulation is held to its own width: you have to be able to walk
      // down it, and a passage that is a room's minimum wide is a gap between
      // two walls.
      const RoomUse use = programme.rooms[r].use;
      const bool circulation = use == RoomUse::Passage || use == RoomUse::Landing
                               || use == RoomUse::Hall || use == RoomUse::Lobby
                               || use == RoomUse::ScreensPassage;
      const double narrowest =
        circulation ? std::max(settings.minimumWidthMetres, settings.minimumPassageMetres)
        : habitable(use)
          ? std::max(settings.minimumWidthMetres, settings.minimumHabitableMetres)
          : settings.minimumWidthMetres;

      alive = areaOf(candidate[on], int32_t(r)) >= settings.minimumRoomMetres
              && widthOf(candidate[on], int32_t(r)) >= narrowest
              && connected(candidate[on], int32_t(r)) && !pinched(candidate[on], int32_t(r))
              && !encloses(candidate[on], int32_t(r));
    }

    if (!alive)
    {
      continue;
    }

    const LayoutCost next = evaluate(candidate, context);
    ++out.proposed;

    // The lot is a constraint and not a term. The canonical start is inside
    // it by construction, so refusing every move that puts more of the
    // building off its ground keeps it there for good.
    if (next.outsideGround > cost.outsideGround + 1e-9)
    {
      continue;
    }

    const double delta = cost.total - next.total;

    if (delta >= 0 || rng.unit() < std::exp(beta * delta))
    {
      grids = std::move(candidate);
      cost = next;
      ++out.accepted;
      broken = violations(grids);

      if (cost.total < bestCost.total)
      {
        bestCost = cost;
        best = grids;
      }
    }
  }

  grids = std::move(best);

  // The stair is one space, and now it is made one.
  //
  // §4 has multi-story spaces "indicated as such" in the programme, and §7
  // names them as what couples the optimisation across floors and causes most
  // of its failures - "in roughly 1 out of 20 optimization instances for
  // two-story buildings, and in 1 out of 5 three-story optimizations".
  //
  // Three ways of asking the search to solve it were tried and none did. As a
  // cost term alone the landing lands over the flight 55% of the time: a
  // landing at the wrong end of a floor cannot walk to the right end by
  // sliding walls, because every state in between is worse than both ends. As
  // a random relocation move it was accepted three times in a hundred, since
  // taking a bite out of whatever stands over the flight buys nothing until
  // the rest of the floor has rearranged around it. Imposed on every candidate
  // it cost a third of the dimension likelihood, because the search then spent
  // itself repairing a floor that was rearranged under it every iteration.
  //
  // So the shaft is cut once, through the finished plan, taking its own area
  // back from whichever room stands over the flight. That is a departure from
  // optimising it, and it is recorded as one - but a floor you cannot reach is
  // not a floor, and the cost term stays in so the search still prefers plans
  // where the cut takes less.
  for (size_t g = 1; g < grids.size() && g < context.climbs.size(); ++g)
  {
    if (context.climbs[g] == uint32_t(-1) || context.climbs[g - 1] == uint32_t(-1))
    {
      continue;
    }

    const int32_t arriving = int32_t(context.climbs[g]);
    const int32_t leaving = int32_t(context.climbs[g - 1]);

    // How much of the flight is already covered. Where the search found a
    // plan whose stair arrives, it is left alone: the cut is a repair, and a
    // repair applied to something that is not broken only breaks the doors
    // that were built around it.
    double under = 0, covered = 0;

    for (uint32_t j = 0; j < grids[g - 1].rows(); ++j)
    {
      for (uint32_t i = 0; i < grids[g - 1].columns(); ++i)
      {
        if (grids[g - 1].at(i, j) != leaving)
        {
          continue;
        }

        const double piece = grids[g - 1].cellArea(i, j);
        const double x = (grids[g - 1].xs[i] + grids[g - 1].xs[i + 1]) * .5;
        const double y = (grids[g - 1].ys[j] + grids[g - 1].ys[j + 1]) * .5;

        under += piece;
        covered += labelAt(grids[g], x, y) == arriving ? piece : 0.;
      }
    }

    if (under <= 1e-6 || covered / under > .7)
    {
      continue;
    }

    // Widened to the landing's own area first; failing that, the flight's own
    // extent, which is the smallest cut that still lands you somewhere.
    for (const bool widen : {true, false})
    {
      Grid stacked = grids[g];

      if (!stackTheStair(stacked, grids[g - 1], arriving, leaving, settings, widen))
      {
        continue;
      }

      // Only if it leaves a layout: a shaft that cuts a room in two has not
      // helped, and the cost term already preferred the plans where it does
      // not have to.
      bool alive = true;

      for (uint32_t r = 0; r < programme.rooms.size() && alive; ++r)
      {
        if (context.gridOf[r] != uint32_t(g))
        {
          continue;
        }

        alive = areaOf(stacked, int32_t(r)) >= settings.minimumRoomMetres
                && connected(stacked, int32_t(r)) && !pinched(stacked, int32_t(r))
                && !encloses(stacked, int32_t(r));
      }

      if (alive)
      {
        coalesce(stacked);
        grids[g] = std::move(stacked);

        break;
      }
    }
  }
  out.cost = bestCost;

  for (const Grid &grid : grids)
  {
    out.gridLines += uint32_t(grid.xs.size() + grid.ys.size());
    out.gridCells += grid.columns() * grid.rows();
  }

  // --- read the layout back ------------------------------------------------

  std::map<uint32_t, uint32_t> placedOf;

  for (uint32_t r = 0; r < programme.rooms.size(); ++r)
  {
    const uint32_t which = context.gridOf[r];

    if (which == uint32_t(-1))
    {
      continue;
    }

    const Grid &grid = grids[which];
    const double area = areaOf(grid, int32_t(r));

    if (area < settings.minimumRoomMetres)
    {
      continue;
    }

    LayoutRoom room;
    room.programmeRoom = r;
    room.use = programme.rooms[r].use;
    room.floor = context.levels[which];
    room.areaMetres = area;
    room.outline = traceOutline(grid, int32_t(r));

    double lowX = 0, highX = 0, lowY = 0, highY = 0;

    if (boundsOf(grid, int32_t(r), lowX, highX, lowY, highY))
    {
      const double a = std::max(highX - lowX, highY - lowY);
      const double b = std::max(std::min(highX - lowX, highY - lowY), 1e-6);
      room.aspect = a / b;
    }

    placedOf[r] = uint32_t(out.rooms.size());
    out.rooms.push_back(std::move(room));
  }

  for (const Context::Link &link : context.links)
  {
    const auto a = placedOf.find(link.a);
    const auto b = placedOf.find(link.b);

    if (a == placedOf.end() || b == placedOf.end())
    {
      continue;
    }

    const Grid &grid = grids[context.gridOf[link.a]];
    WallRun run;
    sharedWall(grid, int32_t(link.a), int32_t(link.b), run);

    if (run.length < settings.doorWidthMetres)
    {
      if (std::getenv("DOORS") != nullptr)
      {
        uint32_t degA = 0, degB = 0;

        for (const Context::Link &other : context.links)
        {
          degA += other.a == link.a || other.b == link.a ? 1u : 0u;
          degB += other.a == link.b || other.b == link.b ? 1u : 0u;
        }

        std::fprintf(stderr, "      NODOOR %s(deg %u) - %s(deg %u) on floor %d\n",
                     name(programme.rooms[link.a].use), degA,
                     name(programme.rooms[link.b].use), degB, programme.rooms[link.a].floor);
      }

      continue;
    }

    LayoutDoor door;
    door.a = a->second;
    door.b = b->second;
    door.door = !link.open;
    door.wallLengthMetres = run.length;
    door.passage = passageFor(programme.rooms[link.a].use, programme.rooms[link.b].use,
                              link.open);

    placeDoor(run, out.rooms[a->second].outline, out.rooms[b->second].outline,
              settings.doorWidthMetres, door.x, door.z);

    out.doors.push_back(door);
    ++out.doorsDelivered;
  }

  // --- the way in ----------------------------------------------------------
  //
  // §6: the entrance goes in the room designated as the entrance. Ours is the
  // ground-floor room the programme says fronts the street, or its porch, or
  // its hall - the same rooms Ca made reach the outside, so there is an
  // exterior wall to put it in by construction.
  for (const uint32_t r : context.mustReachOutside)
  {
    const auto placedRoom = placedOf.find(r);

    if (placedRoom == placedOf.end() || programme.rooms[r].floor != 0)
    {
      continue;
    }

    const Grid &grid = grids[context.gridOf[r]];
    WallRun run;
    double normalX = 0, normalZ = 0;

    if (outsideWall(grid, int32_t(r), settings.streetX, settings.streetZ, run, normalX, normalZ)
        < settings.doorWidthMetres)
    {
      continue;
    }

    LayoutEntrance entrance;
    entrance.room = placedRoom->second;
    entrance.normalX = normalX;
    entrance.normalZ = normalZ;
    entrance.wallLengthMetres = run.length;

    // Away from the corners, like every other door - a front door hard against
    // the return of a wall is a service hatch.
    placeDoor(run, out.rooms[placedRoom->second].outline, {}, settings.doorWidthMetres,
              entrance.x, entrance.z);

    out.entrances.push_back(entrance);

    // One way in is a house; the shop's own door is the exception, and it has
    // its own street-fronting room to put it in.
    if (out.entrances.size() >= 2)
    {
      break;
    }
  }

  // A room too small to place leaves no floor behind it. Its cells are still
  // labelled with it, and an outline traced over them would report ground the
  // building does not have - walls standing around nothing, which is what the
  // storey plates are built from.
  for (uint32_t r = 0; r < programme.rooms.size(); ++r)
  {
    if (context.gridOf[r] == uint32_t(-1) || placedOf.count(r) > 0)
    {
      continue;
    }

    Grid &grid = grids[context.gridOf[r]];

    for (int32_t &label : grid.label)
    {
      label = label == int32_t(r) ? kOutside : label;
    }
  }

  for (size_t g = 0; g < grids.size(); ++g)
  {
    // The floor's own outline, which is the boundary of everything that is
    // not outside - not the boundary of the outside, which is a different
    // region and need not be one loop at all.
    Grid solid = grids[g];

    for (int32_t &label : solid.label)
    {
      label = label == kOutside ? kOutside : 0;
    }

    const std::vector<std::vector<std::pair<double, double>>> loops = traceLoops(solid, 0);

    // Winding identifies courts. A second positive loop is a separate
    // component of the floor, not a hole in the largest component.
    for (const auto &loop : loops)
    {
      double twice = 0;
      for (size_t i = 0; i < loop.size(); ++i)
      {
        const auto &a = loop[i], &b = loop[(i + 1) % loop.size()];
        twice += a.first * b.second - a.second * b.first;
      }
      if (twice > 0)
      {
        out.outlines.emplace_back(context.levels[g], loop);
      }
      else if (twice < 0)
      {
        out.courts.emplace_back(context.levels[g], loop);
      }
    }
  }

  double totalError = 0;
  uint32_t measured = 0;

  for (const LayoutRoom &room : out.rooms)
  {
    const double asked = programme.rooms[room.programmeRoom].areaMetres;

    if (asked > 0)
    {
      totalError += std::abs(room.areaMetres - asked) / asked;
      ++measured;
    }
  }

  out.areaError = measured > 0 ? totalError / double(measured) : 0.;

  // What the programme's own draw would have cost, against the same
  // distributions, so the layout can be compared with it rather than with a
  // target it was never given.
  for (const LayoutRoom &room : out.rooms)
  {
    const RoomSpec &spec = programme.rooms[room.programmeRoom];
    out.sampledDimensionCost -= areaLogLikelihood(programme, room.programmeRoom,
                                                  spec.areaMetres);
    out.sampledDimensionCost -= aspectLogLikelihood(programme, room.programmeRoom,
                                                    spec.aspect);
  }

  // Nothing laid is a failure, and it needs to say so.
  //
  // This was a bare `return !out.rooms.empty()`, which is a silent refusal:
  // eleven of the town's houses came back from a full hundred thousand
  // iterations with no rooms at all, the caller reported "no programme fits
  // the house", and there the trail stopped. The programme asking for more
  // room than the ground can hold is the ordinary reason, and it is worth
  // saying which of the two ran out.
  if (out.rooms.empty())
  {
    double wanted = 0;

    for (const auto &[level, list] : byFloor)
    {
      (void)level;

      for (const uint32_t r : list)
      {
        wanted += std::max(programme.rooms[r].areaMetres, settings.minimumRoomMetres);
      }
    }

    return fail(("the optimiser laid no rooms: the programme wants " + std::to_string(int(wanted))
                 + " m2 over " + std::to_string(byFloor.size()) + " floor(s) and nothing fitted")
                  .c_str());
  }

  return true;
}


bool optimiseFloorPlan(const Programme &programme,
                       const std::vector<std::pair<double, double>> &lot,
                       const uint64_t seed,
                       const FloorPlanSettings &settings,
                       FloorPlan &out,
                       std::string *error)
{
  std::map<int32_t, std::vector<std::pair<double, double>>> lots;

  if (lot.size() >= 3)
  {
    // The bottom floor of the programme, which is not always level zero: a
    // house with a cellar starts at -1, and the cellar is what stands on the
    // ground that was dug.
    int32_t bottom = 0;
    bool first = true;

    for (const RoomSpec &room : programme.rooms)
    {
      if (enclosesFloorArea(room.use))
      {
        bottom = first ? room.floor : std::min(bottom, room.floor);
        first = false;
      }
    }

    lots[bottom] = lot;
  }

  return optimiseFloorPlan(programme, lots, seed, settings, out, error);
}

}  // namespace procedural::world::generator
