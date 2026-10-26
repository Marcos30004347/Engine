#pragma once

#include "procedural/geom2/Polygon.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace procedural::geom2 {

// An orthogonal cell grid, and areas on it as bit boards.
//
// The layer between "a set of integer cells" and "a polygon", which this
// library did not have. Every rectilinear subdivision written here so far -
// lots, blocks, wings - converted to floating point polygons immediately and
// then fought the consequences: a wall that should have been one line became
// two that nearly met, and "is this cell inside that room" became a point in
// polygon test on a shape derived from the answer.
//
// So an area is a set of cells, a boundary is a cyclic chain of DIRECTED
// lattice edges, and a cut, a door and a window are all the same object - an
// edge. That is the representation Watabou's Dwellings works in, and the reason
// its rooms come out as rooms rather than as slivers.
//
// Nothing here is floating point. Metres arrive only at the bridge in
// `CellFrame`, at the bottom of this header.

// ---------------------------------------------------------------------------
// Directions
// ---------------------------------------------------------------------------

// Row-major: `i` is the row and increases south, `j` is the column and
// increases east. A plan is read from the north, so this is the order an image
// is indexed and the order the renderer expects.
enum class Dir : uint8_t
{
  North = 0,
  East = 1,
  South = 2,
  West = 3
};

inline constexpr std::array<Dir, 4> kCardinal = {Dir::North, Dir::East, Dir::South, Dir::West};

// The eight-neighbourhood, as offsets rather than as directions: there is no
// `Dir` for a diagonal, because nothing in this library ever steps diagonally.
// Only the narrowness test looks at one, and it looks at all four.
inline constexpr std::array<std::pair<int32_t, int32_t>, 8> kRose = {
  {{-1, 0}, {0, 1}, {1, 0}, {0, -1}, {-1, 1}, {1, 1}, {1, -1}, {-1, -1}}};

inline constexpr int32_t deltaI(const Dir d)
{
  constexpr std::array<int32_t, 4> table = {-1, 0, 1, 0};

  return table[uint8_t(d)];
}

inline constexpr int32_t deltaJ(const Dir d)
{
  constexpr std::array<int32_t, 4> table = {0, 1, 0, -1};

  return table[uint8_t(d)];
}

// Quarter turns and the reverse. Written as tables rather than as arithmetic on
// the enum value: the arithmetic is correct and the tables are readable, and
// every use of these is in code that is hard enough to follow already.
inline constexpr Dir cw(const Dir d)
{
  constexpr std::array<Dir, 4> table = {Dir::East, Dir::South, Dir::West, Dir::North};

  return table[uint8_t(d)];
}

inline constexpr Dir ccw(const Dir d)
{
  constexpr std::array<Dir, 4> table = {Dir::West, Dir::North, Dir::East, Dir::South};

  return table[uint8_t(d)];
}

inline constexpr Dir opposite(const Dir d)
{
  constexpr std::array<Dir, 4> table = {Dir::South, Dir::West, Dir::North, Dir::East};

  return table[uint8_t(d)];
}

const char *name(Dir);

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------
//
// Stable integers, not pointers and not values. Watabou's implementation
// compares cells and edges by reference identity throughout, which a port
// cannot do and which would not hash; an index into the grid is the same
// guarantee with none of the cost.

inline constexpr uint32_t kNoCell = 0xFFFFFFFFu;
inline constexpr uint32_t kNoNode = 0xFFFFFFFFu;
inline constexpr uint32_t kNoEdge = 0xFFFFFFFFu;

// A directed lattice edge: the node it leaves, and the way it goes. Encoded as
// `node * 4 + direction`, so the twin is one subtraction away and an edge fits
// in the same word as a cell.
struct Edge
{
  uint32_t node = kNoNode;
  Dir dir = Dir::North;
};

// A cyclic chain of directed edges. A contour is one of these; so is an inner
// wall. The closing step from back() to front() is implicit for a contour and
// absent for a wall, and which one a chain is depends only on where it came
// from - there is no flag, because every caller already knows.
using Chain = std::vector<uint32_t>;

// ---------------------------------------------------------------------------
// Boards
// ---------------------------------------------------------------------------

// A set of cells, as bits.
//
// One row per `uint64_t`, so that no operation ever carries across a word:
// stepping north or south moves whole words and touches no bits, stepping east
// or west shifts bits and touches no words. That is what makes the tier
// irrelevant, and it is why the row is 64 bits wide rather than 16.
//
// Fixed at 64 rows because a board is 512 bytes and a building is not worth
// templating over. Watabou's grid is capped at 11 on a side; ours has to hold a
// castle ward, and 64 cells at three metres is 190, which is larger than any
// single mass we plan. Tiering this to 16 or 32 rows is a change confined to
// this struct if a batch ever wants the cache back.
struct Board
{
  static constexpr uint32_t kMaxRows = 64;
  static constexpr uint32_t kMaxColumns = 64;

  uint64_t row[kMaxRows] = {};

  bool test(const uint32_t i, const uint32_t j) const
  {
    return i < kMaxRows && j < kMaxColumns && ((row[i] >> j) & 1u) != 0;
  }

  void set(const uint32_t i, const uint32_t j)
  {
    if (i < kMaxRows && j < kMaxColumns)
    {
      row[i] |= uint64_t(1) << j;
    }
  }

  void clear(const uint32_t i, const uint32_t j)
  {
    if (i < kMaxRows && j < kMaxColumns)
    {
      row[i] &= ~(uint64_t(1) << j);
    }
  }

  uint32_t count() const;
  bool empty() const;

  // The first cell in row-major order, which is the canonical one: bit order is
  // the iteration order, and it does not depend on insertion or on a hash.
  bool first(uint32_t &i, uint32_t &j) const;

  bool operator==(const Board &) const;
  bool operator!=(const Board &other) const
  {
    return !(*this == other);
  }

  Board &operator|=(const Board &);
  Board &operator&=(const Board &);
  Board &operator^=(const Board &);
};

Board operator|(Board, const Board &);
Board operator&(Board, const Board &);
Board operator^(Board, const Board &);
Board operator~(Board);

// Every cell of the board, in row-major order. For the places that genuinely
// need a list - a weighted draw, a render - rather than a set.
std::vector<std::pair<uint32_t, uint32_t>> cells(const Board &);

// ---------------------------------------------------------------------------
// The grid
// ---------------------------------------------------------------------------

class CellGrid
{
public:
  CellGrid() = default;
  CellGrid(uint32_t width, uint32_t height);

  uint32_t width() const
  {
    return width_;
  }

  uint32_t height() const
  {
    return height_;
  }

  bool valid() const
  {
    return width_ > 0 && height_ > 0;
  }

  // ---- identity ----------------------------------------------------------

  uint32_t cell(int32_t i, int32_t j) const;
  uint32_t node(int32_t i, int32_t j) const;

  uint32_t cellRow(const uint32_t c) const
  {
    return c / width_;
  }

  uint32_t cellColumn(const uint32_t c) const
  {
    return c % width_;
  }

  uint32_t nodeRow(const uint32_t n) const
  {
    return n / (width_ + 1);
  }

  uint32_t nodeColumn(const uint32_t n) const
  {
    return n % (width_ + 1);
  }

  uint32_t neighbour(uint32_t c, Dir) const;

  // ---- edges -------------------------------------------------------------

  uint32_t edge(uint32_t fromNode, Dir) const;
  Edge decode(uint32_t e) const;
  uint32_t twin(uint32_t e) const;
  uint32_t edgeEndNode(uint32_t e) const;

  // The cell a directed edge bounds: the one on its right, which makes a
  // contour walked with the interior on the right turn clockwise at a convex
  // corner and counter-clockwise at a concave one. Every corner test in the
  // subdivision depends on that convention, so it is fixed here and nowhere
  // else.
  uint32_t edgeCell(uint32_t e) const;

  // That cell's side, directed consistently with the above. The returned edge
  // runs in `cw(side)`, which is the identity `edgeCell(cellEdge(c, d)) == c`
  // relies on.
  uint32_t cellEdge(uint32_t c, Dir side) const;

  // ---- areas -------------------------------------------------------------

  // The set of cells that have a neighbour in `d` inside `b`. Shifting the
  // board, in other words, and the primitive every set operation below is
  // written in terms of.
  Board shifted(const Board &b, Dir d) const;

  Board mask() const;

  bool contains(const Board &b, uint32_t c) const;
  void add(Board &b, uint32_t c) const;
  void remove(Board &b, uint32_t c) const;

  // Everything reachable from `from` within `within`, four-connected. Grows a
  // frontier by whole words until it stops changing, so the cost is the area's
  // diameter and not its size.
  Board flood(const Board &within, uint32_t from) const;

  bool connected(const Board &) const;

  // ---- contours ----------------------------------------------------------

  // Every closed loop of the area's boundary, outer ring first.
  //
  // Holes are returned rather than dropped. Watabou keeps only the outer ring,
  // which is safe for his shapes and not for ours: a courtyard, a rock the
  // building stands around, a castle ward are all areas with a hole in them,
  // and the roof construction here already takes a polygon with holes.
  std::vector<Chain> outline(const Board &) const;

  // The outer loop alone, for the callers that have already established there
  // is only one.
  Chain outerLoop(const Board &) const;

  // The area a contour encloses: a flood fill from the cell the first edge
  // bounds, refusing to cross any edge of the contour. The inverse of
  // `outline`, and how a split room's cells are found - the subdivision never
  // partitions cells directly, it cuts the boundary and re-derives them.
  Board contourArea(const Chain &) const;

  // The same for an area with holes: the court's loop is a wall too, and a
  // fill that only knows the outer ring runs straight into it.
  Board contourArea(const std::vector<Chain> &loops) const;

  // Whether the boundary touches itself at a node.
  //
  // Two cells meeting only at a corner leave an area that is four-connected
  // and whose outline is NOT a simple polygon: the ring visits that node
  // twice. Legal as a set of cells, and not something a wall, a roof or a
  // boolean can be built on, so the footprint generator refuses it rather
  // than discovering it three phases later.
  bool simpleOutline(const std::vector<Chain> &loops) const;

  // The same wall seen from the other side.
  Chain reverseChain(const Chain &) const;

private:
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

// ---------------------------------------------------------------------------
// Chains
// ---------------------------------------------------------------------------

// The index of the edge in `chain` that starts at `node`, if any.
std::optional<size_t> findEdgeFrom(const CellGrid &, const Chain &, uint32_t node);

// The cyclic sub-chain from the edge starting at `from` up to, but not
// including, the edge starting at `to`.
Chain sliceChain(const CellGrid &, const Chain &, uint32_t fromNode, uint32_t toNode);

// The first edge whose predecessor turns: an arbitrary but stable place to
// start walking a contour, so that runs of collinear edges are not split by
// where the loop happens to begin.
size_t firstCorner(const CellGrid &, const Chain &);

// Consecutive collinear edges merged into one segment each, as node indices in
// order. What a ring is made of; `Chain` is what a wall is made of.
std::vector<uint32_t> mergeCollinear(const CellGrid &, const Chain &);

bool isRectangle(const CellGrid &, const Chain &);

// ---------------------------------------------------------------------------
// Metres
// ---------------------------------------------------------------------------

// Where a plan sits in the world, and how big its cells are.
//
// A plan is authored on the lattice and placed by this and nothing else, so
// every question about orientation and scale has one answer in one place. The
// U axis is the grid's column direction and V its row direction; `frameLot`
// points U along the street.
struct CellFrame
{
  double cellMetres = 2.2;

  // The world position of node (0, 0).
  double originX = 0;
  double originZ = 0;

  // The unit U axis in world space. V is its left perpendicular, so that the
  // frame is right-handed in the plan's own reading.
  double axisX = 1;
  double axisZ = 0;
};

// A point in the grid's own frame, in cells, placed in world metres. `u` runs
// along columns and `v` down rows, both fractional - which a jetty needs,
// because it moves a wall by less than a cell.
void framePoint(const CellFrame &, double u, double v, double &x, double &z);

// A lattice node in world metres.
void nodePoint(const CellGrid &, const CellFrame &, uint32_t node, double &x, double &z);

// The inverse: the cell a world point lies in, or `kNoCell` off the board.
//
// Solved against `framePoint` itself rather than written out from the frame's
// axes, so whatever convention the frame uses for its second axis this is its
// exact inverse by construction and cannot drift from it.
uint32_t cellAtPoint(const CellGrid &, const CellFrame &, double x, double z);

// A point along a directed edge: `t` runs 0 at its start node to 1 at its end.
// Watabou's `Edge.point(t)`, which is how a door's leaf and a window's reveal
// are placed without ever naming a cell.
void edgePoint(const CellGrid &, const CellFrame &, uint32_t edge, double t, double &x, double &z);

// The centre of a cell.
void cellPoint(const CellGrid &, const CellFrame &, uint32_t cell, double &x, double &z);

// A contour as a ring in metres, collinear runs merged so the ring carries
// corners and not lattice steps.
bool ringOf(const CellGrid &, const CellFrame &, const Chain &, Ring &out, std::string *error = nullptr);

// An area as a polygon in metres, outer ring first and every hole kept.
bool polygonOf(const CellGrid &, const CellFrame &, const Board &, Polygon &out, std::string *error = nullptr);

} // namespace procedural::geom2
