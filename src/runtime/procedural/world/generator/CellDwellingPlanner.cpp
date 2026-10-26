#include "DwellingStylePlanner.hpp"
#include "procedural/world/generator/CellDwellingPlanner.hpp"
#include "procedural/world/generator/CellPassage.hpp"
#include "CellDwellingSite.hpp"

#include "procedural/geom2/CellShape.hpp"
#include "procedural/geom2/Offset.hpp"
#include "procedural/world/generator/CellDwellingClasses.hpp"
#include "procedural/world/generator/CellDwellingDoors.hpp"
#include "procedural/world/generator/CellDwellingOpenings.hpp"
#include "procedural/world/generator/CellDwellingProgramme.hpp"
#include "procedural/world/generator/CellDwellingStairs.hpp"
#include "procedural/world/generator/CellSubdivision.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <cstdio>
#include <cstdlib>

namespace procedural::world::generator {

using geom2::Board;
using geom2::CellBounds;
using geom2::CellGrid;
using geom2::kNoCell;
using geom2::kNoEdge;
using geom2::Point;
using geom2::Polygon;
using geom2::Ring;

namespace {

// A board with its nubs trimmed: cells hanging off the mass by a single side.
//
// Rendered, a nub is a piece of building too small to be anything - a metre of
// wall with a roof on it - and it is never what the shape was for. Two passes,
// so a one or two cell nub goes and a genuine thin wing stays.
Board trimmed(const CellGrid &grid, const Board &area, const uint32_t passes)
{
  Board out = area;

  for (uint32_t pass = 0; pass < passes; ++pass)
  {
    Board keep = out;

    for (const auto &[i, j] : geom2::cells(out))
    {
      const uint32_t c = grid.cell(int32_t(i), int32_t(j));
      uint32_t touching = 0;

      for (const Dir d : geom2::kCardinal)
      {
        touching += grid.contains(out, grid.neighbour(c, d)) ? 1u : 0u;
      }

      if (touching <= 1)
      {
        grid.remove(keep, c);
      }
    }

    if (keep.empty() || keep == out)
    {
      break;
    }

    out = keep;
  }

  return out;
}

// The cells of `b`, taken once, so a walk may add to the board it is reading.
std::vector<uint32_t> members(const CellGrid &grid, const Board &b)
{
  std::vector<uint32_t> out;

  for (const auto &[i, j] : geom2::cells(b))
  {
    out.push_back(grid.cell(int32_t(i), int32_t(j)));
  }

  return out;
}



// Whether a contour edge's outward normal faces the street.
bool facesStreet(const CellGrid &grid, const uint32_t e, const double streetJ, const double streetI)
{
  const double length = std::sqrt(streetJ * streetJ + streetI * streetI);

  if (length < 1e-9)
  {
    return false;
  }

  const Dir outward = ccw(grid.decode(e).dir);

  return (double(deltaJ(outward)) * streetJ + double(deltaI(outward)) * streetI) / length > .5;
}

// The longest unbroken run of contour that faces the street: where the front
// wall is, and how much of it there is.
//
// One notion, used twice and deliberately so. It is what decides whether a
// building has a front worth calling one, and it is where the door goes - and
// those two answers have to come from the same measurement or a formal facade
// gets its door round the side of its own portico.
bool frontRun(const CellGrid &grid,
              const Chain &contour,
              const double streetJ,
              const double streetI,
              size_t &start,
              size_t &length)
{
  const size_t n = contour.size();

  start = 0;
  length = 0;

  if (n == 0)
  {
    return false;
  }

  std::vector<uint8_t> front(n, 0);
  size_t facing = 0;

  for (size_t k = 0; k < n; ++k)
  {
    front[k] = facesStreet(grid, contour[k], streetJ, streetI) ? 1 : 0;
    facing += front[k];
  }

  if (facing == 0)
  {
    return false;
  }

  if (facing == n)
  {
    start = 0;
    length = n;

    return true;
  }

  // Runs wrap, so the walk starts at a break.
  size_t origin = 0;

  while (front[origin] != 0)
  {
    ++origin;
  }

  size_t here = 0;

  for (size_t step = 0; step < n; ++step)
  {
    const size_t k = (origin + step) % n;

    if (front[k] == 0)
    {
      here = 0;

      continue;
    }

    ++here;

    if (here > length)
    {
      length = here;
      start = (k + n + 1 - here) % n;
    }
  }

  return length > 0;
}


// The bounding extent of a cell list.
void extentOf(const std::vector<std::pair<int32_t, int32_t>> &cells, int32_t &rows, int32_t &columns)
{
  rows = 0;
  columns = 0;

  if (cells.empty())
  {
    return;
  }

  int32_t lowI = cells.front().first;
  int32_t lowJ = cells.front().second;
  int32_t highI = lowI;
  int32_t highJ = lowJ;

  for (const auto &[i, j] : cells)
  {
    lowI = std::min(lowI, i);
    lowJ = std::min(lowJ, j);
    highI = std::max(highI, i);
    highJ = std::max(highJ, j);
  }

  rows = highI - lowI + 1;
  columns = highJ - lowJ + 1;
}

// The loops of an area with the outer one first.
//
// Several places take `loops.front()` to mean "the outside of this floor", and
// on a plain floor there is only one so it is always right. On a courtyard floor
// there are two and the order is whatever the boundary walk happened to find
// first, which is how a court gets mistaken for a building and a building for a
// court. Ordered once, here, rather than guessed at four call sites.
std::vector<Chain> orderedLoops(const CellGrid &grid, const Board &area)
{
  std::vector<Chain> loops = grid.outline(area);

  if (loops.size() < 2)
  {
    return loops;
  }

  // Asked of `outerLoop`, which knows, rather than measured with `contourArea`,
  // which does not.
  //
  // A hole is walked the other way round, and the area a clockwise chain
  // encloses is everything OUTSIDE it - so measuring "which loop holds the most"
  // picked the court every time. Downstream that is the whole building: the
  // front door is chosen from `loops.front()`, so a castle with a court was
  // choosing its gate from the court's own wall, finding every candidate faced
  // inward, and giving the court up to get a door at all. A third of them did.
  const Chain outer = grid.outerLoop(area);

  if (outer.empty())
  {
    return loops;
  }

  for (size_t k = 0; k < loops.size(); ++k)
  {
    if (std::find(loops[k].begin(), loops[k].end(), outer.front()) != loops[k].end())
    {
      std::swap(loops[0], loops[k]);

      break;
    }
  }

  return loops;
}


bool fail(std::string *error, const char *text)
{
  if (error != nullptr)
  {
    *error = text;
  }

  return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Stream
// ---------------------------------------------------------------------------

uint64_t Stream::next()
{
  // splitmix64. Watabou's is a 31-bit Lehmer generator with visible structure;
  // the shape that matters is one stream that can be snapshotted, not his
  // particular bits.
  state += 0x9E3779B97F4A7C15ull;
  uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;

  return z ^ (z >> 31);
}

double Stream::unit()
{
  return double(next() >> 11) * (1. / 9007199254740992.);
}

double Stream::bell3()
{
  return (unit() + unit() + unit()) / 3.;
}

double Stream::bell4()
{
  return (unit() + unit() + unit() + unit()) / 2. - 1.;
}

int32_t Stream::roundStochastic(const double value)
{
  const double floored = std::floor(value);

  return int32_t(floored) + (unit() < value - floored ? 1 : 0);
}

uint32_t Stream::below(const uint32_t limit)
{
  return limit == 0 ? 0 : uint32_t(next() % limit);
}

int32_t Stream::range(const int32_t low, const int32_t high)
{
  return high <= low ? low : low + int32_t(below(uint32_t(high - low + 1)));
}

// ---------------------------------------------------------------------------
// Framing the lot
// ---------------------------------------------------------------------------

namespace {

struct Axis
{
  double ux = 1, uz = 0;

  void toLocal(const double x, const double z, double &u, double &v) const
  {
    u = x * ux + z * uz;
    v = -x * uz + z * ux;
  }

  void toWorld(const double u, const double v, double &x, double &z) const
  {
    x = u * ux - v * uz;
    z = u * uz + v * ux;
  }
};

double ringArea(const Ring &ring)
{
  return std::abs(geom2::areaMetres(ring));
}

// Whether the axis-aligned cell square meets the polygon at all: a corner
// inside, a polygon vertex inside the square, or a pair of edges crossing.
// Used for obstacles, where any contact at all disqualifies the cell.
bool squareMeetsPolygon(const Polygon &poly, const double x0, const double z0, const double x1, const double z1)
{
  const Point corners[4] = {Point{geom2::toUnit(x0), geom2::toUnit(z0)},
                            Point{geom2::toUnit(x1), geom2::toUnit(z0)},
                            Point{geom2::toUnit(x1), geom2::toUnit(z1)},
                            Point{geom2::toUnit(x0), geom2::toUnit(z1)}};

  for (const Point &p : corners)
  {
    if (geom2::contains(poly, p))
    {
      return true;
    }
  }

  const auto sweep = [&](const Ring &ring) {
    for (size_t k = 0; k < ring.size(); ++k)
    {
      const Point &a = ring[k];
      const Point &b = ring[(k + 1) % ring.size()];

      if (a.x >= corners[0].x && a.x <= corners[1].x && a.y >= corners[0].y && a.y <= corners[2].y)
      {
        return true;
      }

      for (uint32_t s = 0; s < 4; ++s)
      {
        if (geom2::segmentsIntersect(a, b, corners[s], corners[(s + 1) % 4]))
        {
          return true;
        }
      }
    }

    return false;
  };

  if (sweep(poly.outer))
  {
    return true;
  }

  for (const Ring &hole : poly.holes)
  {
    if (sweep(hole))
    {
      return true;
    }
  }

  return false;
}

// Whether the cell square lies wholly inside the polygon. All four corners in,
// and no boundary edge cutting across. Full containment rather than a coverage
// fraction, because a cell is where a WALL may stand and a wall half outside
// the setback is outside the setback.
bool squareInsidePolygon(const Polygon &poly, const double x0, const double z0, const double x1, const double z1)
{
  const Point corners[4] = {Point{geom2::toUnit(x0), geom2::toUnit(z0)},
                            Point{geom2::toUnit(x1), geom2::toUnit(z0)},
                            Point{geom2::toUnit(x1), geom2::toUnit(z1)},
                            Point{geom2::toUnit(x0), geom2::toUnit(z1)}};

  for (const Point &p : corners)
  {
    if (!geom2::contains(poly, p))
    {
      return false;
    }
  }

  const auto crosses = [&](const Ring &ring) {
    for (size_t k = 0; k < ring.size(); ++k)
    {
      const Point &a = ring[k];
      const Point &b = ring[(k + 1) % ring.size()];

      for (uint32_t s = 0; s < 4; ++s)
      {
        if (geom2::segmentsIntersect(a, b, corners[s], corners[(s + 1) % 4]))
        {
          return true;
        }
      }
    }

    return false;
  };

  if (crosses(poly.outer))
  {
    return false;
  }

  for (const Ring &hole : poly.holes)
  {
    if (crosses(hole))
    {
      return false;
    }
  }

  return true;
}

} // namespace

bool frameLot(const CellDwellingBrief &brief,
              const CellDwellingSettings &settings,
              CellGrid &grid,
              geom2::CellFrame &frame,
              SiteBoard &site,
              Ledger &ledger,
              std::string *error)
{
  grid = CellGrid();
  site = SiteBoard();

  const std::string fault = geom2::polygonFault(brief.lot);

  if (!fault.empty())
  {
    return fail(error, "lot is not a valid polygon");
  }

  const double cell = settings.grid.cellMetres;

  if (!(cell > 0) || !std::isfinite(cell))
  {
    return fail(error, "cell size must be positive");
  }

  // 1. Orientation. A building faces its street, and so must its grid.
  Axis axis;

  {
    const Ring &outer = brief.lot.outer;
    size_t edgeIndex = 0;

    if (!brief.frontage.empty() && brief.frontage.front() < outer.size())
    {
      edgeIndex = brief.frontage.front();
    }
    else
    {
      // No frontage given: take the longest edge, which on a lot cut from a
      // block is the street side more often than not.
      double best = -1;

      for (size_t k = 0; k < outer.size(); ++k)
      {
        const Point &a = outer[k];
        const Point &b = outer[(k + 1) % outer.size()];
        const double dx = geom2::toMetres(b.x - a.x);
        const double dz = geom2::toMetres(b.y - a.y);
        const double length = dx * dx + dz * dz;

        if (length > best)
        {
          best = length;
          edgeIndex = k;
        }
      }
    }

    const Point &a = brief.lot.outer[edgeIndex];
    const Point &b = brief.lot.outer[(edgeIndex + 1) % brief.lot.outer.size()];
    const double dx = geom2::toMetres(b.x - a.x);
    const double dz = geom2::toMetres(b.y - a.y);
    const double length = std::sqrt(dx * dx + dz * dz);

    if (length > 1e-9)
    {
      axis.ux = dx / length;
      axis.uz = dz / length;
    }
  }

  if(brief.sharedGrid) {
    const auto &shared=*brief.sharedGrid;
    if(!std::isfinite(shared.originX) || !std::isfinite(shared.originZ) ||
       !std::isfinite(shared.axisX) || !std::isfinite(shared.axisZ) ||
       std::abs(std::hypot(shared.axisX,shared.axisZ)-1)>1e-6)return fail(error,"invalid shared dwelling lattice");
    axis.ux=shared.axisX;axis.uz=shared.axisZ;
  }

  // 2. The buildable region: the lot pulled in by its setback.
  Polygon region = brief.lot;

  if (brief.setbackMetres > 0)
  {
    std::vector<Polygon> inset;

    if (!geom2::offsetPolygon(brief.lot, -brief.setbackMetres, geom2::OffsetOptions{}, inset, error) || inset.empty())
    {
      ledger.add(LedgerKind::Rejected, PatchId(brief.id), "lot vanishes under its setback");

      return fail(error, "lot vanishes under its setback");
    }

    size_t best = 0;

    for (size_t k = 1; k < inset.size(); ++k)
    {
      if (ringArea(inset[k].outer) > ringArea(inset[best].outer))
      {
        best = k;
      }
    }

    region = inset[best];
  }

  if(!std::isfinite(brief.frontageSetbackMetres) || brief.frontageSetbackMetres<0)
    return fail(error,"invalid frontage setback");
  if(brief.frontageSetbackMetres>0) {
    std::vector<geom2::Operand> strips;
    for(auto edge:brief.frontage) {
      std::vector<Polygon> bands;
      const auto a=brief.lot.outer[edge%brief.lot.outer.size()],b=brief.lot.outer[(edge+1)%brief.lot.outer.size()];
      if(!geom2::offsetPolyline({a,b},brief.frontageSetbackMetres,{},bands,error))return false;
      for(const auto &p:bands)strips.push_back(geom2::toOperand(p));
    }
    if(!strips.empty()) {
      std::vector<Polygon> buildable;
      if(!geom2::differenceOf(geom2::toOperand(region),strips,buildable,error) || buildable.empty())
        return fail(error,"lot vanishes under its frontage reservation");
      region=*std::max_element(buildable.begin(),buildable.end(),[](const auto &a,const auto &b){return geom2::areaMetres(a)<geom2::areaMetres(b);});
    }
  }

  // 3. The anchor: the largest rectangle that fits, in the street's frame.
  double lowU = 0, lowV = 0, highU = 0, highV = 0;
  bool exactRow=brief.capacity && (brief.capacity->attachedRow || cell<=1.5) && region.outer.size()==4 && region.holes.empty();
  if (exactRow)
  {
    lowU=lowV=1e100; highU=highV=-1e100;
    for (const auto &p:region.outer) {
      double u=0,v=0; axis.toLocal(geom2::toMetres(p.x),geom2::toMetres(p.y),u,v);
      lowU=std::min(lowU,u); highU=std::max(highU,u); lowV=std::min(lowV,v); highV=std::max(highV,v);
    }
    exactRow=std::abs((highU-lowU)*(highV-lowV)-ringArea(region.outer))<1e-4;
  }
  // The sampled inscribed-rectangle search can lose an entire grid column
  // on rotated frontage. An already rectangular terrace lot has an exact
  // anchor; its obstacle mask is still checked cell by cell below.
  if (!exactRow && !geom2::largestInscribedRectangle(region, axis.ux, axis.uz, cell * settings.anchorResolution, lowU, lowV, highU, highV))
  {
    ledger.add(LedgerKind::Rejected, PatchId(brief.id), "no rectangle fits inside the lot");

    return fail(error, "no rectangle fits inside the lot");
  }

  if (highU - lowU < cell || highV - lowV < cell)
  {
    ledger.add(LedgerKind::Rejected, PatchId(brief.id), "lot holds less than one cell");

    return fail(error, "lot holds less than one cell");
  }

  // Centre the lattice in the core, so what does not divide evenly is split
  // between the two ends rather than piling up at one.
  //
  // With a hair of tolerance before the truncation. A lot that is an exact
  // multiple of the cell - every plot a street cuts, every wing a compound
  // lays against its wall - measured 2.9999999996 cells and truncated to two,
  // and the leftover cell was then split half to each end: the lattice sat a
  // half cell off the lot, every edge cell straddled the boundary, and a
  // three-by-four plot came out with six buildable cells of its twelve.
  const uint32_t coreColumns = uint32_t(std::floor((highU - lowU) / cell + 1e-6));
  const uint32_t coreRows = uint32_t(std::floor((highV - lowV) / cell + 1e-6));
  double originU = lowU + ((highU - lowU) - double(coreColumns) * cell) * .5;
  double originV = lowV + ((highV - lowV) - double(coreRows) * cell) * .5;
  if(brief.sharedGrid) {
    double u,v;axis.toLocal(brief.sharedGrid->originX,brief.sharedGrid->originZ,u,v);
    originU=u+std::floor((lowU-u)/cell)*cell;originV=v+std::floor((lowV-v)/cell)*cell;
  }

  // 4. The extent: the whole region, not the core. Gridding only the rectangle
  // would make every building on every awkward lot rectangular.
  double minU = 1e300, maxU = -1e300, minV = 1e300, maxV = -1e300;

  for (const Point &p : (settings.entranceFlightCells ? brief.lot.outer : region.outer))
  {
    double u = 0, v = 0;
    axis.toLocal(geom2::toMetres(p.x), geom2::toMetres(p.y), u, v);
    minU = std::min(minU, u);
    maxU = std::max(maxU, u);
    minV = std::min(minV, v);
    maxV = std::max(maxV, v);
  }

  const int32_t growWest = int32_t(std::ceil((originU - minU) / cell));
  const int32_t growNorth = int32_t(std::ceil((originV - minV) / cell));
  originU -= double(std::max(0, growWest)) * cell;
  originV -= double(std::max(0, growNorth)) * cell;

  int32_t columns = int32_t(std::ceil((maxU - originU) / cell));
  int32_t rows = int32_t(std::ceil((maxV - originV) / cell));
  // Clamped to the class's own tier as well as to the board: a cottage on a
  // huge lot should not silently become a grid a castle's size.
  const int32_t tier = int32_t(std::min<uint32_t>(std::max(4u, settings.grid.rows), Board::kMaxRows));
  columns = std::clamp(columns, 1, int32_t(Board::kMaxColumns));
  rows = std::clamp(rows, 1, tier);

  grid = CellGrid(uint32_t(columns), uint32_t(rows));

  if (!grid.valid())
  {
    return fail(error, "lot does not fit the grid");
  }

  frame.cellMetres = cell;
  frame.axisX = axis.ux;
  frame.axisZ = axis.uz;
  axis.toWorld(originU, originV, frame.originX, frame.originZ);

  // Classification must happen in the cell's frame. Two opposite corners of
  // a rotated square do NOT bound it (at 45 degrees one extent can vanish).
  // That old test admitted walls outside angled lots and missed neighbours.
  const auto localPolygon = [&](Polygon p) {
    const auto rotate = [&](Ring &ring) {
      for (auto &point:ring) {
        double u=0,v=0;
        axis.toLocal(geom2::toMetres(point.x),geom2::toMetres(point.y),u,v);
        point={geom2::toUnit(u),geom2::toUnit(v)};
      }
    };
    rotate(p.outer);
    for (auto &hole:p.holes) rotate(hole);
    return p;
  };
  const Polygon localRegion=localPolygon(region);
  std::vector<Polygon> localObstacles;
  for (const auto &obstacle:brief.obstacles) localObstacles.push_back(localPolygon(obstacle));

  // 5. Classify every cell once.
  for (uint32_t i = 0; i < grid.height(); ++i)
  {
    for (uint32_t j = 0; j < grid.width(); ++j)
    {
      double x0 = 0, z0 = 0, x1 = 0, z1 = 0;
      geom2::nodePoint(grid, frame, grid.node(int32_t(i), int32_t(j)), x0, z0);
      geom2::nodePoint(grid, frame, grid.node(int32_t(i) + 1, int32_t(j) + 1), x1, z1);
      axis.toLocal(x0,z0,x0,z0);
      axis.toLocal(x1,z1,x1,z1);
      const double lowX = std::min(x0, x1), highX = std::max(x0, x1);
      const double lowZ = std::min(z0, z1), highZ = std::max(z0, z1);

      bool blockedByObstacle = false;

      for (const Polygon &obstacle : localObstacles)
      {
        if (squareMeetsPolygon(obstacle, lowX, lowZ, highX, highZ))
        {
          blockedByObstacle = true;
          break;
        }
      }

      if (blockedByObstacle)
      {
        site.obstacle.set(i, j);
      }
      else if (squareInsidePolygon(localRegion, lowX + 1e-3, lowZ + 1e-3, highX - 1e-3, highZ - 1e-3))
      {
        // Tested a millimetre inside itself, so a cell whose edge IS the lot's
        // edge counts as ground. Read strictly, the cells along a whole-cell
        // lot's boundary were all refused for touching it.
        site.buildable.set(i, j);
      }
    }
  }

  site.blocked = grid.mask() & ~site.buildable;

  if (site.buildable.empty())
  {
    ledger.add(LedgerKind::Rejected, PatchId(brief.id), "no buildable cell on the lot");

    return fail(error, "no buildable cell on the lot");
  }

  // 6. The building has to stand on connected ground.
  if (!grid.connected(site.buildable))
  {
    Board best;
    Board rest = site.buildable;

    while (!rest.empty())
    {
      uint32_t i = 0, j = 0;
      rest.first(i, j);
      const Board part = grid.flood(rest, grid.cell(int32_t(i), int32_t(j)));

      if (part.count() > best.count())
      {
        best = part;
      }

      rest = rest & ~part;
    }

    ledger.add(LedgerKind::Omitted, PatchId(brief.id), "lot split by obstacles; largest piece used");
    site.buildable = best;
    site.blocked = grid.mask() & ~site.buildable;
  }

  if (site.buildable.count() < settings.grid.minimumCells)
  {
    ledger.add(LedgerKind::Rejected, PatchId(brief.id), "buildable ground smaller than the class minimum");

    return fail(error, "buildable ground smaller than the class minimum");
  }

  return true;
}

// ---------------------------------------------------------------------------
// The footprint
// ---------------------------------------------------------------------------

const char *const kPolyominoStencils[12] = {
  " x xxx",
  "x  xxx",
  "xx xx ",
  "xx  xx",
  " xxxx  x ",
  "xx xx x  ",
  "xxx x  x ",
  "x xxxx",
  "x  x  xxx",
  "x  xx  xx",
  " x xxx x ",
  "xx  x  xx"};

namespace {

// One stencil, stretched and jittered, as cells on a local canvas.
std::vector<std::pair<int32_t, int32_t>> drawPolyomino(const uint32_t low, const uint32_t high, Stream &stream)
{
  const bool mirrorX = stream.unit() < .5;
  const bool mirrorY = stream.unit() < .5;
  const bool transpose = stream.unit() < .5;

  uint32_t columns[3] = {0, 0, 0};
  uint32_t rows[3] = {0, 0, 0};

  for (uint32_t k = 0; k < 3; ++k)
  {
    columns[k] = uint32_t(stream.range(int32_t(low), int32_t(high)));
    rows[k] = uint32_t(stream.range(int32_t(low), int32_t(high)));
  }

  const char *stencil = kPolyominoStencils[stream.below(12)];
  const size_t length = std::char_traits<char>::length(stencil);

  std::vector<std::pair<int32_t, int32_t>> out;
  Board canvas;

  for (size_t g = 0; g < length; ++g)
  {
    if (stencil[g] == ' ')
    {
      continue;
    }

    uint32_t bx = uint32_t(g % 3);
    uint32_t by = uint32_t(g / 3);

    if (mirrorX)
    {
      bx = 2 - bx;
    }

    if (mirrorY)
    {
      by = 2 - by;
    }

    if (transpose)
    {
      std::swap(bx, by);
    }

    int32_t x0 = 1;
    int32_t y0 = 1;

    for (uint32_t k = 0; k < bx; ++k)
    {
      x0 += int32_t(columns[k]);
    }

    for (uint32_t k = 0; k < by; ++k)
    {
      y0 += int32_t(rows[k]);
    }

    // One time in three per side. These four independent pushes are what stop
    // the stencil's blocks meeting flush: they overshoot into each other, and
    // the offsets and ledges that come out are the whole reason the silhouette
    // reads as a building rather than as an arrangement of rectangles.
    const auto jitter = [&]() { return stream.below(3) == 2 ? 1 : 0; };

    const int32_t left = x0 - jitter();
    const int32_t right = x0 + jitter() + int32_t(columns[bx]);
    const int32_t top = y0 - jitter();
    const int32_t bottom = y0 + jitter() + int32_t(rows[by]);

    for (int32_t i = top; i < bottom; ++i)
    {
      for (int32_t j = left; j < right; ++j)
      {
        if (i >= 0 && j >= 0 && i < int32_t(Board::kMaxRows) && j < int32_t(Board::kMaxColumns))
        {
          canvas.set(uint32_t(i), uint32_t(j));
        }
      }
    }
  }

  const CellBounds box = geom2::bounds(canvas);

  for (const auto &[i, j] : geom2::cells(canvas))
  {
    out.emplace_back(int32_t(i - box.i0), int32_t(j - box.j0));
  }

  return out;
}

std::vector<std::pair<int32_t, int32_t>> drawBox(const uint32_t minimum, const uint32_t maximum, Stream &stream)
{
  for (uint32_t attempt = 0; attempt < 64; ++attempt)
  {
    const uint32_t w = uint32_t(std::lround(2. + 6. * stream.unit()));
    const uint32_t h = uint32_t(std::lround(2. + 6. * stream.unit()));

    if (w * h >= minimum && w * h <= maximum)
    {
      std::vector<std::pair<int32_t, int32_t>> out;

      for (uint32_t i = 0; i < h; ++i)
      {
        for (uint32_t j = 0; j < w; ++j)
        {
          out.emplace_back(int32_t(i), int32_t(j));
        }
      }

      return out;
    }
  }

  return {};
}

} // namespace

double frontSetbackShare(const PlotStanding standing)
{
  // Read straight off the standing's own description.
  switch (standing)
  {
    case PlotStanding::Slum:
      // Built over, nothing else on the ground: hard against the street.
      return 0;
    case PlotStanding::Modest:
      // A craftsman's plot, most of it house and the yard behind it.
      return 0;
    case PlotStanding::Comfortable:
      // A merchant's: a garden, but the house still addresses the street.
      return .08;
    case PlotStanding::Wealthy:
      // A patrician's, on a third of the plot with grounds around it.
      return .28;
    case PlotStanding::Grand:
      // A mansion on a quarter of it and the rest laid out - which is laid out
      // IN FRONT, because that is the whole point of it.
      return .40;
    case PlotStanding::Rural:
      // A cottage standing in its own pen rather than on any street line.
      return .30;
  }

  return 0;
}

namespace {

// A footprint grown out of ranges, rather than stamped out of a stencil.
//
// The twelve 3x3 stencils are twelve silhouettes. At a cottage that reads as
// variety; at a manor the same twelve shapes are visible AS twelve shapes, and
// the only articulation they have is four independent coin flips per block - a
// wing that steps because a die came up two rather than because it is a wing.
//
// This builds the way the building was built: a RANGE, and then another range
// against it, and another against that. Each piece is a rectangle at least two
// cells across, so nothing that comes out of it is a corridor pretending to be
// a wing; each is attached along a real stretch of the last one's wall, so
// every step in the outline is the join between two ranges and can be pointed
// at; and the vocabulary is unbounded because it is a composition rather than a
// catalogue.
//
// Score, then sample - not draw, then filter. The subdivision was moved off
// reject-and-retry and is better for it, and the same argument holds here: on
// an awkward lot most blind draws die and the survivors are whatever happened
// to fit, which biases the answer toward the lot's bounding box in a way
// nothing measures.
bool growFootprint(const CellGrid &grid,
                   const SiteBoard &site,
                   const GridClass &gridClass,
                   const double streetJ,
                   const double streetI,
                   const double frontShare,
                   const bool frontal,
                   Stream &stream,
                   Board &out)
{
  out = Board();

  if (site.buildable.empty())
  {
    return false;
  }

  // How big, and how deep a range is. A range is a room and a half across -
  // three or four cells at this scale - because that is what a floor one room
  // deep with a corridor beside it measures, and it is why a building reads as
  // a building and not as a block.
  const uint32_t target = gridClass.minimumCells +
                          uint32_t(stream.next() % uint64_t(std::max(1u, gridClass.maximumCells -
                                                                           gridClass.minimumCells + 1u)));
  const uint32_t deepest = std::clamp(uint32_t(std::lround(std::sqrt(double(target)) * .8)), 2u, 7u);

  // How many ranges it takes to make a building of this size, and no fewer than
  // two: one range is a rectangle, and a generator that returns rectangles has
  // stopped articulating - which is the exact failure this whole approach
  // exists to avoid.
  const uint32_t ranges =
    std::clamp(uint32_t(std::lround(double(target) / double(deepest * deepest))), 2u, 12u);

  // Where the front of the ground is, so a range can be scored on how far back
  // it stands. The standing asked for a share of the depth and this is the only
  // place that can honour it while the shape is still being decided.
  const double streetLength = std::sqrt(streetJ * streetJ + streetI * streetI);
  double front = -1e300;
  double back = 1e300;

  for (const auto &[i, j] : geom2::cells(site.buildable))
  {
    const double along = double(j) * streetJ + double(i) * streetI;
    front = std::max(front, along);
    back = std::min(back, along);
  }

  const double depth = streetLength > 1e-9 ? (front - back) / streetLength : 0.;
  const double stand = front - depth * frontShare * streetLength;

  // One candidate range: a rectangle somewhere on the buildable ground.
  struct Range
  {
    Board cells;
    uint32_t added = 0;
    uint32_t shared = 0;
    double score = 0;
  };

  const auto rectangle = [&](const int32_t i0, const int32_t j0, const int32_t rows, const int32_t columns) {
    Board box;

    for (int32_t i = i0; i < i0 + rows; ++i)
    {
      for (int32_t j = j0; j < j0 + columns; ++j)
      {
        if (i < 0 || j < 0 || i >= int32_t(grid.height()) || j >= int32_t(grid.width()))
        {
          return Board();
        }

        box.set(uint32_t(i), uint32_t(j));
      }
    }

    return box & site.buildable;
  };

  for (uint32_t attempt = 0; attempt < 24; ++attempt)
  {
    Board grown;

    for (uint32_t range = 0; range < ranges; ++range)
    {
      // The first range is a part of the building, not the whole of it. Aiming
      // the first draw at the full size is how the grower kept returning after
      // one rectangle.
      const uint32_t want = range + 1 == ranges
                              ? target
                              : uint32_t(double(target) * (double(range) + 1.) / double(ranges));

      if (grown.count() >= target)
      {
        break;
      }

      std::vector<Range> options;

      for (uint32_t draw = 0; draw < 96 && options.size() < 32; ++draw)
      {
        // A range is longer one way than the other, and which way is the
        // stream's to say: a building is ranges at right angles to each other.
        const int32_t across = int32_t(2 + stream.next() % uint64_t(deepest - 1));
        const int32_t along = int32_t(across + 1 + stream.next() % uint64_t(2 + deepest));
        const bool turned = (stream.next() & 1) != 0;
        const int32_t rows = turned ? along : across;
        const int32_t columns = turned ? across : along;

        if (rows > int32_t(grid.height()) || columns > int32_t(grid.width()))
        {
          continue;
        }

        const int32_t i0 = int32_t(stream.next() % uint64_t(grid.height() - uint32_t(rows) + 1));
        const int32_t j0 = int32_t(stream.next() % uint64_t(grid.width() - uint32_t(columns) + 1));

        const Board box = rectangle(i0, j0, rows, columns);

        // Masked to the ground, a range may have lost too much of itself to be
        // one. Half is the line: below it the rectangle is a fiction and what
        // is left is whatever the lot's edge carved.
        if (box.count() * 2 < uint32_t(rows * columns))
        {
          continue;
        }

        const Board fresh = box & ~grown;

        if (fresh.empty())
        {
          continue;
        }

        Range option;
        option.cells = grown | box;
        option.added = fresh.count();

        if (option.cells.count() > gridClass.maximumCells)
        {
          continue;
        }

        // How it joins what is already there. The first range joins nothing;
        // every later one must share a real stretch of wall, or the building is
        // two buildings.
        if (!grown.empty())
        {
          uint32_t shared = 0;

          for (const auto &[i, j] : geom2::cells(fresh))
          {
            const uint32_t c = grid.cell(int32_t(i), int32_t(j));

            for (const Dir d : geom2::kCardinal)
            {
              shared += grid.contains(grown, grid.neighbour(c, d)) ? 1u : 0u;
            }
          }

          if (shared < 2 && (box & grown).empty())
          {
            continue;
          }

          option.shared = shared;
        }

        if (!grid.connected(option.cells))
        {
          continue;
        }

        // Scored on three things and nothing else: how close it brings the
        // whole to the size wanted, how far back it stands against the
        // standing's setback, and how narrow it leaves the plan.
        const double size = double(option.cells.count());
        const double miss = std::abs(size - double(want)) / std::max(1., double(want));

        // Where the WHOLE shape now reaches toward the street. Scoring the new
        // range's own middle instead let a building creep forward one wing at a
        // time, and a standing is a statement about the building's front rather
        // than about any part of it.
        double reach = -1e300;

        for (const auto &[i, j] : geom2::cells(option.cells))
        {
          reach = std::max(reach, double(j) * streetJ + double(i) * streetI);
        }

        const double setback = streetLength > 1e-9 ? std::abs(reach - stand) / streetLength : 0.;
        const double pinched = double(geom2::narrow(grid, option.cells).count()) / std::max(1., size);

        // A formal front is one straight wall with the door in the middle of
        // it. Measured as the length of the shape's front-most run against the
        // width it spans: a range set back behind another breaks it.
        double formal = 1.;

        {
          // How much of the frontage is ONE wall: the longest unbroken run of
          // cells standing on the front-most line, against how wide the
          // building is across the street. A straight front spanning the whole
          // width scores one; two wings with a recess between them do not, and
          // that is the distinction the word formal is carrying.
          const bool alongColumns = std::abs(streetI) <= std::abs(streetJ);

          std::vector<int32_t> line;
          int32_t leastAcross = INT32_MAX;
          int32_t mostAcross = INT32_MIN;

          for (const auto &[i, j] : geom2::cells(option.cells))
          {
            const double along = double(j) * streetJ + double(i) * streetI;
            const int32_t across = alongColumns ? int32_t(i) : int32_t(j);

            leastAcross = std::min(leastAcross, across);
            mostAcross = std::max(mostAcross, across);

            if (along > reach - 1e-9)
            {
              line.push_back(across);
            }
          }

          std::sort(line.begin(), line.end());

          size_t best = 0;
          size_t here = 0;

          for (size_t k = 0; k < line.size(); ++k)
          {
            here = k > 0 && line[k] == line[k - 1] + 1 ? here + 1 : 1;
            best = std::max(best, here);
          }

          const double width = mostAcross >= leastAcross ? double(mostAcross - leastAcross + 1) : 1.;
          const double share = std::min(1., double(best) / std::max(1., width));

          // Both ways, because a formal front is a POSITIVE decision and its
          // absence is one too. A guildhall's front is one designed wall and is
          // scored toward that; a cottage grew, and a cottage whose street
          // elevation is a single unbroken plane looks municipal. So the
          // ordinary case is pulled toward a front with a step in it rather
          // than merely left alone - which is also why the two populations
          // separate at all, rather than both drifting to whatever the range
          // rule happens to produce.
          //
          // Not a target for the ordinary case, only a ceiling: pulling a
          // cottage TOWARD a particular share also pulled it off the street
          // line, because the two scores were fighting over the same cells.
          // What is asked is that its front has a step in it, which is a bound
          // on one end and nothing at all on the other.
          formal = frontal ? .02 + .98 * share * share * share : (share > .72 ? .08 : 1.);
        }

        option.score = std::max(.01, (1. - miss) * (1. - pinched * .9) * formal / (1. + setback * 2.6));
        options.push_back(std::move(option));
      }

      if (options.empty())
      {
        break;
      }

      std::vector<uint32_t> indices;
      std::vector<double> weights;

      for (uint32_t k = 0; k < uint32_t(options.size()); ++k)
      {
        indices.push_back(k);
        weights.push_back(options[k].score);
      }

      grown = options[stream.weighted(indices, weights)].cells;
    }

    // Slid onto the standing's line.
    //
    // Scoring each range on how far forward it brought the whole was a pull and
    // not a placement, and a pull loses to every other term in the score often
    // enough that a craftsman's house ended up a sixth of the plot back from
    // the street it is supposed to stand on. Where the building STANDS is not a
    // property of its shape, so it is settled after the shape is, by sliding
    // the finished mass along the street axis to the line the standing asked
    // for - which is exact, and cannot be outvoted.
    {
      const bool alongRows = std::abs(streetI) > std::abs(streetJ);
      const double step = alongRows ? streetI : streetJ;

      Board best = grown;
      double nearest = 1e300;

      for (int32_t shift = -int32_t(grid.height() + grid.width()); shift <= int32_t(grid.height() + grid.width());
           ++shift)
      {
        Board moved;
        bool ok = true;

        for (const auto &[i, j] : geom2::cells(grown))
        {
          const int32_t ti = int32_t(i) + (alongRows ? shift : 0);
          const int32_t tj = int32_t(j) + (alongRows ? 0 : shift);

          if (ti < 0 || tj < 0 || ti >= int32_t(grid.height()) || tj >= int32_t(grid.width()))
          {
            ok = false;
            break;
          }

          moved.set(uint32_t(ti), uint32_t(tj));
        }

        if (!ok || (moved & ~site.buildable).count() != 0)
        {
          continue;
        }

        double reach = -1e300;

        for (const auto &[i, j] : geom2::cells(moved))
        {
          reach = std::max(reach, double(j) * streetJ + double(i) * streetI);
        }

        const double miss = std::abs(reach - stand);

        if (miss < nearest)
        {
          nearest = miss;
          best = std::move(moved);
        }
      }

      (void)step;
      grown = std::move(best);
    }

    const Board area = trimmed(grid, grown, 2);

    if (area.count() < gridClass.minimumCells || area.count() > gridClass.maximumCells)
    {
      continue;
    }

    if (!grid.connected(area) || grid.outline(area).size() != 1)
    {
      continue;
    }

    // The same habitability floor the rest of the planner works to: a footprint
    // that is mostly cells belonging to no two-by-two block is a plan of
    // corridors.
    if (geom2::narrow(grid, area).count() * 4 > area.count())
    {
      continue;
    }

    // And the front is the front the class asked for.
    //
    // Checked on the FINISHED shape rather than preferred while it is being
    // grown, because a preference is no preference when every candidate in a
    // round shares the fault: the score is relative, so penalising all of them
    // equally changes nothing and the term quietly does no work. Measured
    // instead, and the draw taken again if it is wrong - with the requirement
    // dropped near the end, since a lot that can only carry one shape must
    // still get a building.
    if (attempt < 18)
    {
      const bool alongColumns = std::abs(streetI) <= std::abs(streetJ);

      double reach = -1e300;

      for (const auto &[i, j] : geom2::cells(area))
      {
        reach = std::max(reach, double(j) * streetJ + double(i) * streetI);
      }

      std::vector<int32_t> line;
      int32_t leastAcross = INT32_MAX;
      int32_t mostAcross = INT32_MIN;

      for (const auto &[i, j] : geom2::cells(area))
      {
        const int32_t across = alongColumns ? int32_t(i) : int32_t(j);

        leastAcross = std::min(leastAcross, across);
        mostAcross = std::max(mostAcross, across);

        if (double(j) * streetJ + double(i) * streetI > reach - 1e-9)
        {
          line.push_back(across);
        }
      }

      std::sort(line.begin(), line.end());

      size_t best = 0;
      size_t here = 0;

      for (size_t k = 0; k < line.size(); ++k)
      {
        here = k > 0 && line[k] == line[k - 1] + 1 ? here + 1 : 1;
        best = std::max(best, here);
      }

      const double width = mostAcross >= leastAcross ? double(mostAcross - leastAcross + 1) : 1.;
      const double share = std::min(1., double(best) / std::max(1., width));

      if (frontal ? share < .92 : share > .68)
      {
        continue;
      }
    }

    out = area;

    return true;
  }

  return false;
}

} // namespace

bool planFootprint(const CellGrid &grid,
                   const SiteBoard &site,
                   const GridClass &gridClass,
                   const std::vector<std::string> &tags,
                   const PlotStanding standing,
                   const bool formalFront,
                   const double streetJ,
                   const double streetI,
                   Stream &stream,
                   Board &out,
                   Ledger &ledger,
                   std::string *error)
{
  out = Board();

  if (!grid.valid())
  {
    return fail(error, "no grid");
  }

  // The tag overrides the class: `square` is an authored request for a plain
  // rectangle and it wins over whatever the class would have drawn.
  const FootprintKind kind = std::find(tags.begin(), tags.end(), "square") != tags.end()
                               ? FootprintKind::Box
                               : gridClass.footprint;

  if (gridClass.fitToLot && kind==FootprintKind::Box)
  {
    // Exhaustive rectangles on the usable cell mask. Authored ranges maximize
    // area; city policies prefer balanced proportions while retaining 90% of
    // feasible area. The selected whole-cell rectangle is the actual blueprint.
    std::vector<std::vector<bool>> usable(grid.height(),std::vector<bool>(grid.width(),false));
    for (const auto &[i,j]:cells(site.buildable)) usable[i][j]=true;
    uint32_t best=0,maximumArea=0,bi=0,bj=0,bh=0,bw=0;
    double bestFront=-1e100;
    double bestAspect=1e100, bestScore=-1;
    for (uint32_t i=0;i<grid.height();++i) {
      std::vector<bool> columns(grid.width(),true);
      for (uint32_t end=i;end<grid.height();++end) {
        const uint32_t height=end-i+1;
        for (uint32_t j=0;j<grid.width();++j) columns[j]=columns[j] && usable[end][j];
        if (height<gridClass.minimumSpanCells) continue;
        uint32_t run=0;
        for (uint32_t j=0;j<grid.width();++j) {
          run=columns[j] ? run+1 : 0;
          const uint32_t width=std::min({run,gridClass.maximumCells/height,
              gridClass.maximumAspectRatio>0 ? uint32_t(std::floor(height*gridClass.maximumAspectRatio)) : run}),area=width*height;
          if (width<gridClass.minimumSpanCells || area<gridClass.minimumCells) continue;
          const double front=(double(j)-double(width-1)*.5)*streetJ+(double(i)+double(height-1)*.5)*streetI;
          const double aspect=double(std::max(width,height))/std::min(width,height);
          if (gridClass.maximumAspectRatio>0 && aspect>gridClass.maximumAspectRatio) continue;
          maximumArea=std::max(maximumArea,area);
          const uint32_t keepArea=gridClass.maximumAspectRatio>0 ? uint32_t(std::ceil(maximumArea*.9)) : maximumArea;
          if(area<keepArea) continue;
          const double score=area/(1+(gridClass.maximumAspectRatio>0 ? gridClass.compactnessPenalty*std::max(0.,aspect-2.) : 0.));
          if (best<keepArea || score>bestScore || (score==bestScore && (aspect<bestAspect || (aspect==bestAspect && front>bestFront)))) {
            bestScore=score;best=area; bi=i; bj=j-width+1; bh=height; bw=width; bestFront=front; bestAspect=aspect;
          }
        }
      }
    }
    if (best) {
      // The largest rectangle is where the plan STARTS, not what it is.
      //
      // Taking it whole is what made every house in the town a box: measured,
      // a hundred per cent of them had exactly four corners and a median
      // aspect of 1.11, which is to say the same near-square, over and over.
      // Pantin's town-house plans are not boxes - they are a range on the
      // street with a wing off the back, a range and a half, an L round a
      // corner, or a court - and the corpus in assets names exactly those.
      // So the rectangle is the envelope and a plan type is cut out of it.
      //
      // Every type falls back to the plain range if what it cuts leaves too
      // little, breaks the piece in two, or lands off the usable ground: a
      // house that is a box is a worse house than one that is not, and a
      // house that is nothing at all is worse than either.
      const auto raise = [&](const uint32_t skipI0, const uint32_t skipI1,
                             const uint32_t skipJ0, const uint32_t skipJ1) {
        Board shaped;
        uint32_t count = 0;

        for (uint32_t i = bi; i < bi + bh; ++i)
        {
          for (uint32_t j = bj; j < bj + bw; ++j)
          {
            if (i >= skipI0 && i < skipI1 && j >= skipJ0 && j < skipJ1)
            {
              continue;
            }

            shaped.set(i, j);
            ++count;
          }
        }

        // A decorative notch cannot spend space already lost to setbacks
        // and whole-cell fitting. Keep at least 85% of the requested capacity,
        // or the entire available rectangle when the lot is smaller than that.
        const uint32_t keep=gridClass.cellMetres<=1.5 ? std::min(best,uint32_t(std::ceil(gridClass.maximumCells*.85))) : 4u;
        if (count < std::max({gridClass.minimumCells,4u,keep}) || !grid.connected(shaped))
        {
          return false;
        }

        out = shaped;

        return true;
      };

      // Only a rectangle with room to lose a piece is worth cutting.
      const bool roomy = bh >= 2 * gridClass.minimumSpanCells && bw >= 2 * gridClass.minimumSpanCells;
      const uint32_t plan = roomy ? uint32_t(stream.next() % 100) : 100;
      // How deep and how wide the cut piece is: a third to a half, so what is
      // left still reads as a range and not as a scrap.
      const uint32_t cutI = std::max(1u, bh / 3), cutJ = std::max(1u, bw / 3);
      bool shaped = false;

      if (plan < 26)
      {
        // Right angle: a wing off the back, so the plan turns a corner.
        shaped = raise(bi + bh - cutI, bi + bh, bj + bw - cutJ, bj + bw);
      }
      else if (plan < 46)
      {
        // The same, handed the other way.
        shaped = raise(bi + bh - cutI, bi + bh, bj, bj + cutJ);
      }
      else if (plan < 60)
      {
        // Contracted: the back half is narrower than the street range.
        shaped = raise(bi + bh - cutI, bi + bh, bj + bw - cutJ, bj + bw) ||
                 raise(bi, bi + cutI, bj + bw - cutJ, bj + bw);
      }
      else if (plan < 70 && bh >= 3 * gridClass.minimumSpanCells && bw >= 3 * gridClass.minimumSpanCells)
      {
        // A court, cut out of the middle and open to the back.
        shaped = raise(bi + cutI, bi + bh, bj + cutJ, bj + bw - cutJ);
      }

      if (!shaped)
      {
        for (uint32_t i=bi;i<bi+bh;++i) for (uint32_t j=bj;j<bj+bw;++j) out.set(i,j);
      }

      return true;
    }
    if (gridClass.minimumSpanCells>2)
      return fail(error, "lot cannot fit the required square dwelling base");
    // Awkward lots may still admit a non-rectangular ordinary footprint.
  }

  // A slum is the lot.
  //
  // There is no stencil and no jitter, and that is the whole of what the
  // standing says: "built over; one or two storeys; nothing else on the
  // ground". A slum where every house is an articulated polyomino with a yard
  // round it is not a slum - it is a modest plot drawn small - and the awkward
  // corners of the lot ARE the building, which is the thing the cell model
  // exists to be able to say and a stencil can never produce.
  if (standing == PlotStanding::Slum && kind != FootprintKind::Axial)
  {
    const Board whole = trimmed(grid, site.buildable, 2);

    if (grid.connected(whole) && whole.count() >= gridClass.minimumCells &&
        whole.count() <= gridClass.maximumCells && grid.outline(whole).size() == 1)
    {
      out = whole;
      ledger.add(LedgerKind::Repaired, 0, "a slum lot was built over whole");

      return true;
    }

    // Too big, pinched or in pieces: fall through and draw one the ordinary
    // way rather than refuse. A standing is a preference about shape and the
    // building still has to stand somewhere.
  }

  // Grown, not stamped. See `growFootprint`: this is the ordinary path for
  // every class whose shape is nobody's to name, and the stencil below stands
  // as the fallback for the lots it cannot serve.
  if (kind == FootprintKind::Polyomino)
  {
    Board grown;

    if (growFootprint(grid, site, gridClass, streetJ, streetI, frontSetbackShare(standing), formalFront,
                      stream, grown))
    {
      out = grown;

      return true;
    }

    ledger.add(LedgerKind::Repaired, 0, "a footprint could not be grown and was stamped instead");
  }

  const ClassProfile *profile = nullptr;

  // Watabou's stretch range. For a medium house this pins both to two, so all
  // the variety comes from the stencil and the jitter rather than from the
  // stretch - which is deliberate and worth not "fixing".
  // How big a block of the stencil is, in cells.
  //
  // The stencil is three blocks by three, of which a shape uses five to seven,
  // so its area goes as the SQUARE of the block size - which is why the first
  // derivation, a tenth of the band, broke the moment the band stopped being a
  // cottage's. At a castle's hundred and fifty to four hundred cells it asked
  // for blocks fifteen to forty-two across, a silhouette a hundred and twenty
  // cells wide, and nothing that could ever be placed. Hence the square roots;
  // the constants are chosen so a Dwelling's band still gives one to three,
  // exactly what it gave before, and everything larger scales with it.
  //
  // Not capped by the board. A stencil uses five to seven of its nine blocks
  // and which ones is the seed's business, so a block size that looks too big
  // for the grid often is not - and the placement loop below already refuses
  // anything that will not stand on the ground, which is the honest test.
  const uint32_t high = std::max(1u, uint32_t(std::lround(std::sqrt(double(gridClass.maximumCells) / 3.2))));
  const uint32_t low = std::clamp(uint32_t(std::lround(std::sqrt(double(gridClass.minimumCells) / 9.))), 1u, high);

  for (uint32_t attempt = 0; attempt < 64; ++attempt)
  {
    std::vector<std::pair<int32_t, int32_t>> shape;

    if (kind == FootprintKind::Polyomino)
    {
      shape = drawPolyomino(std::max(1u, low), std::max(1u, high), stream);
    }
    else if (kind == FootprintKind::Box && gridClass.maximumCells <= 34)
    {
      // A small box keeps Watabou's own rejection sampling, which gives the
      // two-to-eight proportions a house actually has. A large one is sized
      // from the band instead, because rejection sampling a warehouse out of
      // dice rolls almost never lands.
      shape = drawBox(gridClass.minimumCells, gridClass.maximumCells, stream);
    }
    else
    {
      // Drawn for the ground there is. `grid` is the board the lot actually
      // framed, which on a tight plot is a good deal smaller than the tier the
      // class is entitled to.
      // Nothing is mirrored here. A caller that reaches this branch has settled
      // the shape itself - a range against a wall, a nave - and reflecting it
      // about its own middle is how a nave gained a second set of transepts and
      // became a letter H.
      shape = archetype(kind, gridClass, grid.height(), grid.width(), stream);

      // A church is turned to face the street, and both halves of that matter.
      //
      // The nave runs AWAY from the road, so the long axis is the one across
      // the frontage rather than along it; and the chancel goes at the far end,
      // so the door is the west door at the near one. Left unturned, the cross
      // lands whichever way the archetype happened to draw it and the entrance
      // rule then does the only thing it can, which is to put the front door in
      // the end of a transept.
      if (kind == FootprintKind::Axial && !shape.empty())
      {
        // Turned on a copy. Facing the street is a preference and standing on
        // the ground is a requirement, so a cross that only fits the way the
        // archetype drew it keeps that way rather than being refused.
        std::vector<std::pair<int32_t, int32_t>> turned = shape;

        int32_t rows = 0;
        int32_t columns = 0;
        extentOf(turned, rows, columns);

        // The long axis has to be the one pointing away from the street.
        const bool streetAlongRows = std::abs(streetI) > std::abs(streetJ);

        if ((rows > columns) != streetAlongRows)
        {
          for (auto &[i, j] : turned)
          {
            std::swap(i, j);
          }

          std::swap(rows, columns);
        }

        // And the heavy end - the crossing, the transepts, the chancel - goes
        // away from the road.
        double centre = 0;

        for (const auto &[i, j] : turned)
        {
          centre += double(streetAlongRows ? i : j);
        }

        centre /= double(turned.size());

        const double middle = double(streetAlongRows ? rows : columns) * .5;
        const double toward = streetAlongRows ? streetI : streetJ;

        if ((centre - middle) * toward > 0)
        {
          const int32_t span = (streetAlongRows ? rows : columns) - 1;

          for (auto &[i, j] : turned)
          {
            (streetAlongRows ? i : j) = span - (streetAlongRows ? i : j);
          }
        }

        if (rows <= int32_t(grid.height()) && columns <= int32_t(grid.width()))
        {
          shape = std::move(turned);
        }
      }
    }

    if (shape.empty())
    {
      continue;
    }

    int32_t rows = 0;
    int32_t columns = 0;

    for (const auto &[i, j] : shape)
    {
      rows = std::max(rows, i + 1);
      columns = std::max(columns, j + 1);
    }

    // Where it could stand. Shuffled and then tried in order, so a footprint
    // that only fits in one corner still finds it without the search being
    // biased toward the origin.
    std::vector<std::pair<int32_t, int32_t>> offsets;

    for (int32_t i = 0; i + rows <= int32_t(grid.height()); ++i)
    {
      for (int32_t j = 0; j + columns <= int32_t(grid.width()); ++j)
      {
        offsets.emplace_back(i, j);
      }
    }

    if (offsets.empty())
    {
      continue;
    }

    offsets = stream.shuffled(offsets);

    const size_t tried = std::min<size_t>(offsets.size(), 96);

    // How far along the street direction a set of cells reaches, and how deep
    // the buildable ground is. A building's standing is expressed as a share of
    // that depth, so a shallow plot and a deep one both read correctly.
    const double streetLength = std::sqrt(streetJ * streetJ + streetI * streetI);
    const auto reach = [&](const Board &board) {
      double most = -1e300;

      for (const auto &[i, j] : cells(board))
      {
        most = std::max(most, double(j) * streetJ + double(i) * streetI);
      }

      return most;
    };

    double siteFront = 0;
    double siteDepth = 0;

    if (streetLength > 1e-9)
    {
      double least = 1e300;

      for (const auto &[i, j] : cells(site.buildable))
      {
        least = std::min(least, double(j) * streetJ + double(i) * streetI);
      }

      siteFront = reach(site.buildable);
      siteDepth = std::max(0., (siteFront - least) / streetLength);
    }

    const double target = siteDepth * frontSetbackShare(standing);

    std::vector<Board> accepted;
    std::vector<double> fit;

    for (size_t k = 0; k < tried && accepted.size() < 24; ++k)
    {
      Board placed;

      for (const auto &[i, j] : shape)
      {
        placed.set(uint32_t(i + offsets[k].first), uint32_t(j + offsets[k].second));
      }

      // Masked, then the nubs trimmed off.
      const Board area = trimmed(grid, placed & site.buildable, 2);
      const uint32_t size = area.count();

      if (size < gridClass.minimumCells || size > gridClass.maximumCells)
      {
        continue;
      }

      if (!grid.connected(area))
      {
        continue;
      }

      // One loop, unless a court was asked for.
      //
      // A hole is a courtyard, and a courtyard is chosen rather than stumbled
      // into: a stencil that happened to close a ring is a mistake, while a
      // castle without a bailey is not a castle. So a ring is allowed exactly
      // where the class drew one - and it has to be a court rather than a light
      // well, because a hole two cells across is a hole in the plan.
      const std::vector<Chain> loops = grid.outline(area);

      if (loops.empty())
      {
        continue;
      }

      if (kind == FootprintKind::Courtyard)
      {
        if (loops.size() != 2 || (grid.contourArea(loops.front()) & ~area).count() < 4)
        {
          continue;
        }
      }
      else if (loops.size() != 1)
      {
        continue;
      }

      // And it must not pinch. Masking a stencil to awkward ground can leave
      // two lobes joined at a single corner: four-connected, so it passes the
      // test above, and not a shape a wall can be built on.
      if (!grid.simpleOutline(loops))
      {
        continue;
      }

      // A building is not a corridor.
      //
      // The stencil can draw a block one cell wide, and masked to a narrow strip
      // of ground it comes back as a hairline - a chapel three metres across and
      // thirty long, which passes connectivity, the contour test and the size
      // band, and is a passage with a door. `narrow` is the habitability
      // primitive the whole generator already leans on: a cell is narrow when it
      // belongs to no two-by-two block, and a plan whose every cell is narrow is
      // not a plan.
      if (geom2::narrow(grid, area) == area)
      {
        continue;
      }

      // A formal building shows the street ONE wall.
      //
      // This is all that is asked of it, and asking no more is the point. The
      // plan behind the front is still whatever the seed drew, which is what
      // stops a formal class collapsing onto the handful of shapes a mirrored
      // stencil can make.
      // Only where the seed drew the shape. Where a caller settled it - a
      // compound wing, the `square` tag - the shape is already the answer and
      // second-guessing it only refuses buildings.
      if (formalFront && kind == FootprintKind::Polyomino)
      {
        size_t start = 0;
        size_t run = 0;

        if (!frontRun(grid, loops.front(), streetJ, streetI, start, run))
        {
          continue;
        }

        const CellBounds box = geom2::bounds(area);
        const uint32_t across =
          std::abs(streetI) > std::abs(streetJ) ? box.j1 - box.j0 + 1 : box.i1 - box.i0 + 1;

        if (run * 4 < size_t(across) * 3)
        {
          continue;
        }
      }

      // How far back from the street this placement stands, against where its
      // household would put it. Still a draw rather than a choice, so a street
      // of cottages does not come out ruled.
      double weight = 1;

      if (streetLength > 1e-9 && siteDepth > 0)
      {
        const double gap = (siteFront - reach(area)) / streetLength;
        weight = 1. / (1. + std::abs(gap - target));
        weight *= weight;
      }

      accepted.push_back(area);
      fit.push_back(weight);
    }

    if (!accepted.empty())
    {
      out = stream.weighted(accepted, fit);

      return true;
    }
  }

  ledger.add(LedgerKind::Rejected, 0, "no footprint fits the buildable ground");

  return fail(error, "no footprint fits the buildable ground");
}

// ---------------------------------------------------------------------------
// The entrance
// ---------------------------------------------------------------------------

uint32_t pickEntrance(const CellGrid &grid,
                      const Chain &contour,
                      const SiteBoard &site,
                      const Board &courts,
                      const double streetJ,
                      const double streetI,
                      const bool centred,
                      Stream &stream)
{
  if (contour.empty())
  {
    return kNoEdge;
  }

  const size_t n = contour.size();
  std::vector<uint32_t> candidates;
  std::vector<double> weights;
  std::vector<uint32_t> frontCandidates;
  std::vector<double> frontWeights;
  std::vector<double> facing; // How far each candidate's outward turns to the street.

  const double streetLength = std::sqrt(streetJ * streetJ + streetI * streetI);

  for (size_t k = 0; k < n; ++k)
  {
    const uint32_t e = contour[k];
    const Dir previous = grid.decode(contour[(k + n - 1) % n]).dir;
    const Dir here = grid.decode(e).dir;
    const Dir next = grid.decode(contour[(k + 1) % n]).dir;

    double weight = 0;

    if (previous == here && here == next)
    {
      // The middle of a straight wall, which is where a door goes.
      weight = 5;
    }
    else if (cw(previous) == here && cw(here) == next)
    {
      weight = 3;
    }
    else if (ccw(previous) != here && ccw(here) != next)
    {
      weight = 1;
    }

    if (weight == 0)
    {
      continue;
    }

    // Outward is the quarter turn anticlockwise: the contour is walked with
    // the interior on its right.
    const Dir outward = ccw(here);
    const uint32_t outside = grid.neighbour(grid.edgeCell(e), outward);

    if (outside != kNoCell && grid.contains(site.obstacle, outside))
    {
      // A door may open onto the garden. It may not open into a rock.
      continue;
    }

    // Nor into a court.
    //
    // A court is inside the building - it was cut out of the footprint - so a
    // wall between a room and a court is an interior wall however the contour
    // reads. Where a court reaches the outer face the floor becomes a U and
    // those edges join the outer loop, which is how a castle came out with its
    // front door opening into its own ward and no way from there to the street.
    // The gate goes in the curtain; the doors onto the ward are the ranges'
    // own, and `connectRooms` draws those.
    if (outside != kNoCell && grid.contains(courts, outside))
    {
      continue;
    }

    double towardsStreet = -1;

    if (streetLength > 1e-9)
    {
      const double dj = double(deltaJ(outward));
      const double di = double(deltaI(outward));
      towardsStreet = (dj * streetJ + di * streetI) / streetLength;
    }

    if (towardsStreet > .5)
    {
      frontCandidates.push_back(e);
      frontWeights.push_back(weight);
    }

    facing.push_back(towardsStreet);
    candidates.push_back(e);
    weights.push_back(weight);
  }

  // The front door goes on the front. Weighting the street side up rather than
  // selecting for it was not enough: a footprint has a dozen ordinary walls and
  // two or three facing the road, so any finite preference still put most doors
  // round the back. If the building has a wall facing the street at all, the
  // door is on one of those, and the shape weights only decide which.
  // A footprint whose front is oblique to the street has no wall within
  // sixty degrees of it, and every one of those houses put its door round the
  // side or the back - one city house in eight. The threshold is what a front
  // wall IS, not what one is allowed to be: when nothing clears it, the walls
  // that turn towards the street at all are the front, and the best of them
  // wins.
  if (frontCandidates.empty())
  {
    double best = 1e-3;

    for (size_t k = 0; k < candidates.size(); ++k)
    {
      best = std::max(best, facing[k]);
    }

    for (size_t k = 0; k < candidates.size(); ++k)
    {
      if (facing[k] >= best - 1e-9)
      {
        frontCandidates.push_back(candidates[k]);
        frontWeights.push_back(weights[k]);
      }
    }
  }

  if (!frontCandidates.empty())
  {
    // And it goes in the MIDDLE of the front.
    //
    // Measured across the street rather than along the contour, which matters
    // on the ordinary houses: their frontage is broken into two or three runs
    // by the stencil's ledges, so "the middle of the longest run" is the middle
    // of whichever lobe happened to be widest, and the door lands wherever the
    // shape weights put it. The centre of the FRONTAGE is a fixed point whether
    // the front is one wall or four.
    const bool alongColumns = std::abs(streetI) <= std::abs(streetJ);

    const auto across = [&](const uint32_t e) {
      const uint32_t node = grid.decode(e).node;
      const uint32_t other = grid.edgeEndNode(e);

      return alongColumns ? (double(grid.nodeRow(node)) + double(grid.nodeRow(other))) * .5
                          : (double(grid.nodeColumn(node)) + double(grid.nodeColumn(other))) * .5;
    };

    double lowest = 1e300;
    double highest = -1e300;

    for (const uint32_t e : frontCandidates)
    {
      lowest = std::min(lowest, across(e));
      highest = std::max(highest, across(e));
    }

    const double centre = (lowest + highest) * .5;
    const double half = std::max(.5, (highest - lowest) * .5);

    if (centred)
    {
      // A formal front takes the middle outright. Ties go to the wall the shape
      // weights liked best, so a door still lands on a straight run rather than
      // on the one cell of jamb nearest the axis.
      uint32_t best = frontCandidates.front();
      double bestScore = -1e300;

      for (size_t k = 0; k < frontCandidates.size(); ++k)
      {
        const double score = frontWeights[k] - 16. * std::abs(across(frontCandidates[k]) - centre);

        if (score > bestScore)
        {
          bestScore = score;
          best = frontCandidates[k];
        }
      }

      return best;
    }

    // Every other door keeps its draw, with the middle as its mode. The fourth
    // power is not decoration: a square barely moved the mean off fifty per
    // cent, because a facade six cells wide has most of its candidates at a
    // similar distance and a gentle prior cannot separate them.
    for (size_t k = 0; k < frontCandidates.size(); ++k)
    {
      const double offset = std::abs(across(frontCandidates[k]) - centre) / half;
      const double fall = 1. + 6. * offset * offset * offset * offset;

      frontWeights[k] /= fall;
    }

    return stream.weighted(frontCandidates, frontWeights);
  }

  if (candidates.empty())
  {
    // No fallback. A building with no wall a door can open onto is a building
    // that should be declined, and the caller is the one that can say so.
    return kNoEdge;
  }

  return stream.weighted(candidates, weights);
}

// ---------------------------------------------------------------------------
// Phases 0 and 1 together
// ---------------------------------------------------------------------------

namespace {

bool shell(const CellDwellingBrief &brief,
           const CellDwellingSettings &settings,
           CellHouse &house,
           Stream &stream,
           Ledger &ledger,
           std::string *error)
{
  house = CellHouse();
  house.gridClass = settings.grid;
  house.tags = brief.tags;
  house.building = brief.building;
  house.standing = brief.standing;

  if (!frameLot(brief, settings, house.grid, house.frame, house.site, ledger, error))
  {
    return false;
  }

  // The ground a covered passage stands on is buildable, by request.
  //
  // A gateway house spans a lane: its passage is the lane, and a lane is
  // exactly the ground `frameLot` takes OUT of the lot - it is the road
  // reservation, subtracted from the legal lot before the brief was built,
  // and it is an obstacle besides. So the passage cells were not buildable,
  // every later step that intersected with `buildable` dropped them again,
  // and the reservation refused the passage for leaving the footprint -
  // nineteen of its twenty cells. The caller asking for a passage here is
  // what makes that ground available; nothing else in the lot changes.
  if (brief.coveredPassage && !brief.coveredPassage->polygon.outer.empty())
  {
    for (auto [i, j] : geom2::cells(house.grid.mask()))
    {
      double x = 0, z = 0;
      geom2::cellPoint(house.grid, house.frame, house.grid.cell(i, j), x, z);

      if (!geom2::contains(brief.coveredPassage->polygon, {geom2::toUnit(x), geom2::toUnit(z)}))
      {
        continue;
      }

      house.site.buildable.set(i, j);
      house.site.blocked.clear(i, j);
      house.site.obstacle.clear(i, j);
    }
  }

  Board area;

  // Which way the street is, in the grid's own frame. Needed before the
  // footprint, not after it: where a building stands on its plot is decided
  // with the footprint, and only the street direction can say where that is.
  double streetJ = 0;
  double streetI = 0;

  if (!brief.lot.outer.empty())
  {
    double centreX = 0;
    double centreZ = 0;

    for (const Point &p : brief.lot.outer)
    {
      centreX += geom2::toMetres(p.x);
      centreZ += geom2::toMetres(p.y);
    }

    centreX /= double(brief.lot.outer.size());
    centreZ /= double(brief.lot.outer.size());

    const size_t edgeIndex = brief.frontage.empty() ? 0 : brief.frontage.front() % brief.lot.outer.size();
    const Point &a = brief.lot.outer[edgeIndex];
    const Point &b = brief.lot.outer[(edgeIndex + 1) % brief.lot.outer.size()];
    const double midX = (geom2::toMetres(a.x) + geom2::toMetres(b.x)) * .5;
    const double midZ = (geom2::toMetres(a.y) + geom2::toMetres(b.y)) * .5;

    Axis axis;
    axis.ux = house.frame.axisX;
    axis.uz = house.frame.axisZ;
    double u = 0, v = 0;
    axis.toLocal(midX - centreX, midZ - centreZ, u, v);

    const double length = std::sqrt(u * u + v * v);

    if (length > 1e-9)
    {
      streetJ = u / length;
      streetI = v / length;
    }
  }

  if (!planFootprint(house.grid, house.site, settings.grid, brief.tags, brief.standing, settings.formalFront, streetJ, streetI, stream, area, ledger, error))
  {
    return false;
  }

  // The court, cut out of the mass the stencil grew.
  //
  // Here rather than later, and that ordering is the point: every floor above
  // is a subset of this one, so a court taken out now is open to the sky by
  // construction and no storey, roof or ceiling has to be told about it.
  // A small keep needs its footprint for rooms and circulation. A court is
  // optional space, not a reason to reduce every surrounding range to a cell.
  const bool compactKeep = house.building == BuildingClass::Castle && area.count() < 100;
  if (settings.courts > 0 && !compactKeep)
  {
    Board built;
    Board courts;

    const bool carved = brief.architecture==rewrite::DwellingArchitecture::Roman
      ? carveRomanCourt(house.grid,area,settings.courtShare,built,courts)
      : carveCourts(house.grid, area, settings.courtShare, settings.courts, streetJ, streetI, stream, built,courts);
    if (carved)
    {
      area = built;
      house.courts = courts;
    }
    else
    {
      ledger.add(LedgerKind::Omitted, PatchId(brief.id), "no court could be cut from this footprint");
    }
  }

  // The yard, the same way and for the same reason: cut out before anything
  // stands on it.
  if (settings.yards > 0 && !compactKeep)
  {
    Board yard;

    if (carveYard(house.grid, area, house.site.buildable, settings.yardShare, streetJ, streetI, stream, yard))
    {
      house.courts |= yard;
    }
    else
    {
      ledger.add(LedgerKind::Omitted, PatchId(brief.id), "no yard could be cut from this footprint");
    }
  }

  // The pen (P27): cut like a yard, against the house or the yard, and
  // remembered as a pen so it is fenced in boards with a gate. Then the
  // shed: one buildable cell touching the pen, outside everything else,
  // as far from the lane as the pen allows.
  house.pens = Board();
  house.sheds.clear();

  if (settings.pens > 0)
  {
    Board pen;

    if (carveYard(house.grid, area | house.courts, house.site.buildable, settings.penShare, streetJ, streetI, stream, pen))
    {
      house.pens = pen;
      house.courts |= pen;

      if (settings.sheds > 0)
      {
        const Board taken = area | house.courts;
        uint32_t best = geom2::kNoCell;
        double bestAlong = 1e300;

        for (const auto &[i, j] : geom2::cells(pen))
        {
          for (const Dir d : geom2::kCardinal)
          {
            const uint32_t n = house.grid.neighbour(house.grid.cell(int32_t(i), int32_t(j)), d);

            if (n == geom2::kNoCell || !house.grid.contains(house.site.buildable, n) || house.grid.contains(taken, n))
            {
              continue;
            }

            const double along = double(house.grid.cellColumn(n)) * streetJ + double(house.grid.cellRow(n)) * streetI;

            if (along < bestAlong)
            {
              bestAlong = along;
              best = n;
            }
          }
        }

        if (best != geom2::kNoCell)
        {
          house.sheds.push_back(best);
        }
        else
        {
          ledger.add(LedgerKind::Omitted, PatchId(brief.id), "no cell beside the pen for a shed");
        }
      }
    }
    else
    {
      ledger.add(LedgerKind::Omitted, PatchId(brief.id), "no pen could be cut from this lot");
    }
  }

  house.streetJ = streetJ;
  house.streetI = streetI;
  house.jettyMetres = brief.jettyMetres;
  house.twinTowers = settings.grid.footprint == FootprintKind::Axial && stream.unit() < settings.twinTowerChance;

  // A porch and an apse, put on against the STREET rather than drawn by the
  // archetype: which end of a cross faces the road is the placement's
  // decision, and drawn into the shape they ended up at whichever end the
  // turn left them - a porch on the chancel, an apse round the door, and a
  // back tower cut round a porch cell into an L.
  if (settings.grid.footprint == FootprintKind::Axial)
  {
    const bool porch = stream.unit() < .35;
    const bool apse = stream.unit() < .4;

    const CellBounds box = geom2::bounds(area);
    const bool longAlongRows = (box.i1 - box.i0) >= (box.j1 - box.j0);
    const bool frontAtLow = longAlongRows ? (streetI < 0) : (streetJ < 0);
    const uint32_t lowSlice = longAlongRows ? box.i0 : box.j0;
    const uint32_t highSlice = longAlongRows ? box.i1 : box.j1;
    const uint32_t frontSlice = frontAtLow ? lowSlice : highSlice;
    const uint32_t backSlice = frontAtLow ? highSlice : lowSlice;

    const auto extentOfSlice = [&](const uint32_t slice, uint32_t &least, uint32_t &most) {
      least = UINT32_MAX;
      most = 0;

      for (const auto &[i, j] : geom2::cells(area))
      {
        if ((longAlongRows ? i : j) == slice)
        {
          least = std::min(least, longAlongRows ? j : i);
          most = std::max(most, longAlongRows ? j : i);
        }
      }
    };

    const auto cellOf = [&](const uint32_t slice, const uint32_t across) {
      const int32_t ci = int32_t(longAlongRows ? slice : across);
      const int32_t cj = int32_t(longAlongRows ? across : slice);

      if (ci < 0 || cj < 0 || ci >= int32_t(house.grid.height()) || cj >= int32_t(house.grid.width()))
      {
        return geom2::kNoCell;
      }

      return house.grid.cell(ci, cj);
    };

    const auto stillABuilding = [&](const Board &board) {
      const std::vector<Chain> loops = house.grid.outline(board);

      return house.grid.connected(board) && loops.size() == 1 && house.grid.simpleOutline(loops);
    };

    // The porch: a cell or two stepping out toward the road from the middle
    // of the front, on ground the lot has.
    if (porch)
    {
      uint32_t least = 0, most = 0;
      extentOfSlice(frontSlice, least, most);
      const bool frontExists = least != UINT32_MAX && (frontAtLow ? frontSlice > 0 : true);

      if (frontExists && most >= least + 2)
      {
        const uint32_t ahead = frontAtLow ? frontSlice - 1 : frontSlice + 1;
        const uint32_t width = most - least + 1;
        const uint32_t from = least + (width - (width % 2 == 0 ? 2u : 1u)) / 2;
        Board bumped = area;
        bool ok = true;

        for (uint32_t across = from; across < from + (width % 2 == 0 ? 2u : 1u); ++across)
        {
          const uint32_t c = cellOf(ahead, across);
          ok = ok && c != geom2::kNoCell && house.grid.contains(house.site.buildable, c);

          if (ok)
          {
            house.grid.add(bumped, c);
          }
        }

        if (ok && stillABuilding(bumped))
        {
          area = bumped;
        }
      }
    }

    // The apse: the back slice drawn in by a cell each side.
    if (apse)
    {
      uint32_t least = 0, most = 0;
      extentOfSlice(backSlice, least, most);

      if (least != UINT32_MAX && most >= least + 3)
      {
        Board narrowed = area;
        house.grid.remove(narrowed, cellOf(backSlice, least));
        house.grid.remove(narrowed, cellOf(backSlice, most));

        if (stillABuilding(narrowed) && geom2::narrow(house.grid, narrowed).count() <= geom2::narrow(house.grid, area).count())
        {
          area = narrowed;
        }
      }
    }
  }

  // A west front is wider than its nave.
  //
  // Two towers two cells square with a door bay between them is five cells
  // across, and a nave here is four. So where the seed drew twin towers, the
  // first bays inside the door are widened by a cell each side - the towers
  // stand PROUD of the nave, which is what a pair of west towers does - on the
  // ground that is there for it. Where it is not, the church keeps its single
  // tower at the back, and says so.
  if (house.twinTowers)
  {
    const CellBounds box = geom2::bounds(area);
    const bool longAlongRows = (box.i1 - box.i0) >= (box.j1 - box.j0);

    // Which end faces the street: the slices with the greatest reach along it.
    double frontReach = -1e300;
    double backReach = 1e300;

    for (const auto &[i, j] : geom2::cells(area))
    {
      const double along = double(j) * streetJ + double(i) * streetI;
      frontReach = std::max(frontReach, along);
      backReach = std::min(backReach, along);
    }

    const bool frontAtLow = longAlongRows ? (streetI < 0) : (streetJ < 0);
    const uint32_t lowSlice = longAlongRows ? box.i0 : box.j0;
    const uint32_t highSlice = longAlongRows ? box.i1 : box.j1;

    Board widened = area;
    bool ok = true;

    // The first two FULL bays inside the door, by the same rule the partition
    // uses to find them: a porch is a narrow bay standing in front, and
    // widening it made the towers an L of three cells.
    std::vector<uint32_t> sliceWidth(highSlice - lowSlice + 1, 0);

    for (const auto &[i, j] : geom2::cells(area))
    {
      ++sliceWidth[(longAlongRows ? i : j) - lowSlice];
    }

    // The nave's width is the first slice from the road that is a bay and
    // not a porch; the front bays are the first two slices of exactly that
    // width, and a WIDER slice before two are found is the crossing, which
    // ends the search - taking "the fullest of the first four" on a short
    // nave took the crossing, and the towers stood on the transepts.
    uint32_t naveWidth = 0;
    std::vector<uint32_t> frontSlices;

    for (uint32_t k = 0; k < sliceWidth.size() && frontSlices.size() < 2; ++k)
    {
      const uint32_t index = frontAtLow ? k : uint32_t(sliceWidth.size()) - 1 - k;
      const uint32_t w = sliceWidth[index];

      if (naveWidth == 0)
      {
        if (w >= 3)
        {
          naveWidth = w;
          frontSlices.push_back(lowSlice + index);
        }

        continue;
      }

      if (w == naveWidth)
      {
        frontSlices.push_back(lowSlice + index);
      }
      else
      {
        break;
      }
    }

    ok = frontSlices.size() == 2;

    for (uint32_t bay = 0; bay < frontSlices.size() && ok; ++bay)
    {
      const uint32_t slice = frontSlices[bay];
      uint32_t least = UINT32_MAX, most = 0;

      for (const auto &[i, j] : geom2::cells(area))
      {
        if ((longAlongRows ? i : j) != slice)
        {
          continue;
        }

        least = std::min(least, longAlongRows ? j : i);
        most = std::max(most, longAlongRows ? j : i);
      }

      if (least == UINT32_MAX || least == 0)
      {
        ok = false;
        break;
      }

      for (const uint32_t across : {least - 1, most + 1})
      {
        const int32_t ci = int32_t(longAlongRows ? slice : across);
        const int32_t cj = int32_t(longAlongRows ? across : slice);

        if (ci < 0 || cj < 0 || ci >= int32_t(house.grid.height()) || cj >= int32_t(house.grid.width()))
        {
          ok = false;
          break;
        }

        const uint32_t c = house.grid.cell(ci, cj);

        if (!house.grid.contains(house.site.buildable, c))
        {
          ok = false;
          break;
        }

        house.grid.add(widened, c);
      }
    }

    (void)frontReach;
    (void)backReach;

    if (ok && house.grid.connected(widened) && house.grid.outline(widened).size() == 1 &&
        house.grid.simpleOutline(house.grid.outline(widened)))
    {
      area = widened;
      house.westFrontSlices = frontSlices;
    }
    else
    {
      house.twinTowers = false;
      house.westFrontSlices.clear();
      ledger.add(LedgerKind::Omitted, PatchId(brief.id), "no ground for a west front; one tower at the back");
    }
  }

  // The party walls, as contour edges: every edge of the footprint whose
  // middle lies on a lot edge the brief named as shared with a neighbour.
  // Settled here, once, so the windows and the roof read the same list.
  house.partyEdges.clear();
  house.partyGaps.clear();

  if (!brief.party.empty())
  {
    ++CellHouse::partyTally().briefs;
    const Ring &ring = brief.lot.outer;

    for (const uint32_t e : house.grid.outerLoop(area))
    {
      double mx = 0;
      double mz = 0;
      geom2::edgePoint(house.grid, house.frame, e, .5, mx, mz);

      for (const uint32_t k : brief.party)
      {
        if (k >= ring.size())
        {
          continue;
        }

        const Point &a = ring[k];
        const Point &b = ring[(k + 1) % ring.size()];
        const double ax = geom2::toMetres(a.x), az = geom2::toMetres(a.y);
        const double bx = geom2::toMetres(b.x), bz = geom2::toMetres(b.y);
        const double dx = bx - ax, dz = bz - az;
        const double length = std::sqrt(dx * dx + dz * dz);

        if (length < 1e-9)
        {
          continue;
        }

        // Distance from the segment, and along it.
        const double t = ((mx - ax) * dx + (mz - az) * dz) / (length * length);
        const double px = ax + dx * t, pz = az + dz * t;
        const double off = std::hypot(mx - px, mz - pz);

        // On the lot line, or within a cell of it and facing it: the edge is
        // the party wall either way, and the second case is the ordinary one.
        if (t >= -1e-6 && t <= 1. + 1e-6 && off < house.frame.cellMetres + .02)
        {
          const Dir outward = ccw(house.grid.decode(e).dir);
          double ox = 0, oz = 0;
          geom2::edgePoint(house.grid, house.frame, e, .5, ox, oz);
          double sx = 0, sz = 0;
          geom2::framePoint(house.frame, 0, 0, sx, sz);
          double tx = 0, tz = 0;
          geom2::framePoint(house.frame, double(geom2::deltaJ(outward)), double(geom2::deltaI(outward)), tx, tz);
          const double nx = tx - sx, nz = tz - sz;
          const double towards = ((px - mx) * nx + (pz - mz) * nz);

          // Only walls parallel to the shared lot edge are party walls.
          // A front-wall cell near its corner must still receive windows and
          // an eave; proximity alone was marking those cells as shared too.
          const double normalLength=std::hypot(nx,nz);
          const bool parallel=normalLength>1e-9 &&
            std::abs(nx*dx+nz*dz)/(normalLength*length)<.1;
          if (parallel && (off < .02 || towards > 0))
          {
            house.partyEdges.push_back(e);
            house.partyGaps.push_back(off < .02 ? 0. : off);
            break;
          }
        }
      }
    }

    // Sorted together, by edge.
    std::vector<size_t> order(house.partyEdges.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return house.partyEdges[a] < house.partyEdges[b]; });
    std::vector<uint32_t> edges;
    std::vector<double> gaps;
    for (size_t i : order) { edges.push_back(house.partyEdges[i]); gaps.push_back(house.partyGaps[i]); }
    house.partyEdges = std::move(edges);
    house.partyGaps = std::move(gaps);
    if (!house.partyEdges.empty()) ++CellHouse::partyTally().houses;
  }

  // A covered passage is part of the footprint, and the footprint is only
  // final here.
  //
  // `reserveCoveredPassage` runs after the floors are stacked and refuses
  // whatever it is given unless the passage already lies inside the ground
  // floor - and the stencil, which knows nothing about it, put the outline
  // where it liked. Every gateway house the city asked for was refused:
  // "passage leaves the occupied building footprint". Added at the top of
  // this function the cells were cut away again by the court carving and the
  // narrowing that follow it; this is after all of them. The strip either
  // side goes in with the passage, because the passage needs a room on both
  // sides to carry the storey over it.
  if (brief.coveredPassage && !brief.coveredPassage->polygon.outer.empty())
  {
    Board wanted;

    for (auto [i, j] : geom2::cells(house.grid.mask()))
    {
      double x = 0, z = 0;
      geom2::cellPoint(house.grid, house.frame, house.grid.cell(i, j), x, z);

      if (geom2::contains(brief.coveredPassage->polygon, {geom2::toUnit(x), geom2::toUnit(z)}))
      {
        wanted.set(i, j);
      }
    }

    // Only the part of the lane the house stands either side of.
    //
    // A passage needs a room on both sides to carry the storey over it, and
    // the lane runs the whole depth of the lot while the house does not. The
    // strip is cut back to the run where both sides are built, and the rest
    // of the lane stays outside the footprint - which is also what leaves the
    // passage's two ends open onto it.
    uint32_t loI = UINT32_MAX, loJ = UINT32_MAX, hiI = 0, hiJ = 0;

    for (auto [i, j] : geom2::cells(wanted))
    {
      loI = std::min(loI, i); loJ = std::min(loJ, j);
      hiI = std::max(hiI, i); hiJ = std::max(hiJ, j);
    }

    Board carried, flanks;

    if (!wanted.empty())
    {
      const bool alongRows = hiI - loI >= hiJ - loJ;
      const uint32_t from = alongRows ? loI : loJ, to = alongRows ? hiI : hiJ;
      const int32_t before = alongRows ? int32_t(loJ) - 1 : int32_t(loI) - 1;
      const int32_t after = alongRows ? int32_t(hiJ) + 1 : int32_t(hiI) + 1;

      // Both sides of the strip standable, at this station along it.
      //
      // Buildable, not already built: the stencil grows its outline wherever
      // it likes in the merged lot and has no reason to cross the lane, so
      // asking for the flanks to be there already found nothing, every time.
      // The house is made to straddle instead - the lane's two flanking
      // columns join the footprint with the lane itself.
      const auto supported = [&](const uint32_t at) {
        for (const int32_t side : {before, after})
        {
          const uint32_t near = alongRows ? house.grid.cell(int32_t(at), side) : house.grid.cell(side, int32_t(at));

          if (near == geom2::kNoCell || !house.grid.contains(house.site.buildable, near))
          {
            return false;
          }
        }

        return true;
      };

      // The longest run of supported stations.
      uint32_t bestFrom = 0, bestLength = 0, runFrom = from, runLength = 0;

      for (uint32_t at = from; at <= to; ++at)
      {
        if (supported(at))
        {
          if (runLength == 0) runFrom = at;
          ++runLength;

          if (runLength > bestLength) { bestLength = runLength; bestFrom = runFrom; }
        }
        else
        {
          runLength = 0;
        }
      }

      if (bestLength >= 2)
      {
        for (auto [i, j] : geom2::cells(wanted))
        {
          const uint32_t at = alongRows ? i : j;

          if (at >= bestFrom && at < bestFrom + bestLength)
          {
            carried.set(i, j);
          }
        }

        // ...and the rooms that carry it, one column either side.
        for (uint32_t at = bestFrom; at < bestFrom + bestLength; ++at)
        {
          for (const int32_t side : {before, after})
          {
            const uint32_t near = alongRows ? house.grid.cell(int32_t(at), side) : house.grid.cell(side, int32_t(at));

            if (near != geom2::kNoCell)
            {
              flanks.set(house.grid.cellRow(near), house.grid.cellColumn(near));
            }
          }
        }
      }
    }

    const Board grown = area | carried | flanks;

    // Only if it is still one building: a passage whose flanks cannot be
    // reached leaves an island, and an island is two houses.
    if (!carried.empty() && house.grid.connected(grown) && house.grid.outline(grown).size() == 1 &&
        house.grid.simpleOutline(house.grid.outline(grown)))
    {
      area = grown;
      house.courts = house.courts & ~grown;
      house.passageReserve = carried;
    }

    if (std::getenv("CITY_DEBUG"))
      std::fprintf(stderr,"passage: %u wanted, %u carried, %u flanks, footprint %u -> %u%s\n",
        uint32_t(wanted.count()),uint32_t(carried.count()),uint32_t(flanks.count()),uint32_t(area.count()),uint32_t(grown.count()),
        (!carried.empty() && house.grid.connected(grown) && house.grid.outline(grown).size()==1 &&
         house.grid.simpleOutline(house.grid.outline(grown))) ? "" : " REFUSED");
  }

  // The door, and the court is given up if it is what stands in the way.
  //
  // A court cannot be worth a building. The carving already refuses one that
  // would swallow the outer face, but "enough exterior survives" and "a door
  // can actually be put somewhere" are not the same question - the second
  // depends on the obstacles, the street and the shape weights - so it is asked
  // outright, and the answer that matters is the one the door gives.
  for (uint32_t attempt = 0; attempt < 2; ++attempt)
  {
    house.floors.clear();

    CellFloor ground;
    ground.level = 0;
    ground.area = area;
    ground.loops = orderedLoops(house.grid, area);
    house.floors.push_back(std::move(ground));

    house.entrance = pickEntrance(house.grid, house.floors.front().loops.front(), house.site, house.courts,
                                  streetJ, streetI, settings.formalFront, stream);

    if (house.entrance != kNoEdge)
    {
      house.entranceLanding = house.grid.edgeCell(house.entrance);

      return true;
    }

    if (house.courts.empty())
    {
      break;
    }

    ledger.add(LedgerKind::Repaired, PatchId(brief.id), "a court was given up so the building could have a door");

    area |= house.courts;
    house.courts = Board();
  }

  ledger.add(LedgerKind::Rejected, PatchId(brief.id), "no wall the front door can open onto");

  return fail(error, "no wall the front door can open onto");
}

} // namespace

bool planCellDwellingShell(const CellDwellingBrief &brief,
                           const CellDwellingSettings &settings,
                           CellHouse &house,
                           Ledger &ledger,
                           std::string *error)
{
  Stream stream(brief.seed);

  return shell(brief, settings, house, stream, ledger, error);
}

// ---------------------------------------------------------------------------
// The vertical scheme
// ---------------------------------------------------------------------------

const char *name(const StairScheme scheme)
{
  switch (scheme)
  {
    case StairScheme::None:
      return "none";
    case StairScheme::Spiral:
      return "spiral";
    case StairScheme::Stairwell:
      return "stairwell";
    case StairScheme::PerFloor:
      return "per floor";
  }

  return "?";
}

namespace {

// Whether a floor can give this cell up to the stair.
//
// Connected afterwards, and the remainder still a single simple loop - because
// what is left is the contour the subdivision is handed, and a ring or a pinch
// is not something it can cut.
// Whether a floor can give this block up to the stair.
bool stairwellFits(const CellGrid &grid, const Board &area, const Board &block, const size_t courtsAllowed)
{
  if (block.empty() || !(block & ~area).empty())
  {
    return false;
  }

  Board rest = area & ~block;

  if (rest.empty() || !grid.connected(rest))
  {
    return false;
  }

  const std::vector<Chain> loops = grid.outline(rest);

  return !loops.empty() && loops.size() <= courtsAllowed + 1 && grid.simpleOutline(loops);
}

} // namespace

bool rollStairs(CellHouse &house, const std::vector<std::string> &tags, const CellDwellingSettings &settings, Stream &stream)
{
  house.scheme = StairScheme::None;
  house.spiral.reset();
  house.stairwell.reset();

  if (house.floors.empty() || house.plannedFloors <= 1)
  {
    return true;
  }

  const bool wantsSpiral = std::find(tags.begin(), tags.end(), "spiral") != tags.end();
  const bool wantsStairwell = std::find(tags.begin(), tags.end(), "stairwell") != tags.end();

  bool spiral = false;
  bool stairwell = false;

  if (wantsSpiral)
  {
    spiral = true;
  }
  else if (wantsStairwell || settings.preferStraightStairs)
  {
    stairwell = true;
  }
  else
  {
    // A floor big enough to be given a planned spine needs a climb the spine
    // can anchor on, and the stairwell is the only scheme that fixes one before
    // the rooms are cut. Rolling a per-floor flight on a manor puts the stair
    // wherever phase 6 can find room for it, which is not where a stair goes.
    //
    // The short circuits matter: a house below the threshold draws exactly the
    // two numbers it drew before large classes existed, so nothing downstream
    // of this moved.
    const bool large = house.floors.front().area.count() >= settings.circulationThresholdCells;

    // A turret is rarer on a two storey house than on a tall one: it is a
    // thing you build because you have a height to climb.
    // Large castles previously excluded the circular stair-tower scheme
    // altogether. Let some choose it, retaining convex-corner validation and
    // the ordinary stairwell fallback when no safe corner exists.
    spiral = house.building==BuildingClass::Castle ? stream.unit()<.55 :
      !large && stream.unit() < (house.plannedFloors == 2 ? .1 : .3);

    if (!spiral)
    {
      stairwell = large || stream.unit() < double(house.plannedFloors - 1) / double(house.plannedFloors + 1);
    }
  }

  const CellFloor &ground = house.floors.front();

  // A circulation tower must be part of the cell plan. The old corner drum
  // was a closed pillar with an unrelated helix intersecting the house.
  // Route tower requests through the reserved, room-connected stair builder.
  const bool integratedTower = spiral;
  if (integratedTower) stairwell = true;

  if (stairwell)
  {
    // Include a level approach row as well as the flight run. Both ordinary
    // stair rooms and towers must keep their doorway cells outside the well.
    const uint32_t across = std::max({2u, settings.stairTowerCells,
      uint32_t(std::ceil((house.gridClass.storeyMetres * .5 / .18 * .25 + .85) / house.frame.cellMetres)) + 1u});

    std::vector<Board> candidates;
    std::vector<double> weights;

    // A one-cell reservation is a line of TWO cells, either way round: a
    // storey's climb does not fit one cell of this size at a tread you can
    // stand on (P29), and a stairwell that cannot be walked up is a shaft. A
    // tower is a square block as before.
    struct Shape
    {
      uint32_t rows = 1, columns = 1;
    };

    std::vector<Shape> shapes;

    if (settings.preferStraightStairs && !integratedTower) {
      const double cell=house.frame.cellMetres;
      const auto [length,width]=straightStairRoom(house.gridClass.storeyMetres,cell,settings);
      if(length>0) {
        shapes.push_back({width,length});
        shapes.push_back({length,width});
      }
    }

    for (uint32_t size = across; size >= 2; --size)
    {
      shapes.push_back(Shape{size, size});
    }

    // A line of cells long enough to be WALKED, not two cells because two
    // cells was long enough when a cell was two and a half metres. On a city
    // house's one metre cell a two cell well is a ladder shaft.
    const uint32_t run = std::max(2u, uint32_t(std::ceil(3.4 / std::max(.25, settings.grid.cellMetres))));

    for (uint32_t length = integratedTower ? 0u : run; length >= 2; --length)
    {
      shapes.push_back(Shape{1, length});
      shapes.push_back(Shape{length, 1});
    }

    for (const Shape &shape : shapes)
    {
      for (const auto &[i, j] : geom2::cells(ground.area))
      {
        Board block;
        bool solid = true;

        for (uint32_t di = 0; di < shape.rows && solid; ++di)
        {
          for (uint32_t dj = 0; dj < shape.columns && solid; ++dj)
          {
            const uint32_t c = house.grid.cell(int32_t(i + di), int32_t(j + dj));
            // ...and never over the lane of a gateway house: that ground is
            // open air, and the storey above it is carried, not stood on.
            solid = c != geom2::kNoCell && house.grid.contains(ground.area, c) && c != house.entranceLanding &&
                    !house.grid.contains(house.passageReserve, c);

            if (solid)
            {
              block.set(i + di, j + dj);
            }
          }
        }

        if (!solid)
        {
          continue;
        }

        // It has to touch the outside, and it has to have somewhere to let
        // onto.
        //
        // A block with every neighbour inside is in the MIDDLE of the floor,
        // and reserving one there leaves the rest as a ring with a hole in it -
        // which is not a contour the subdivision can cut, and which would come
        // back as one annular room. Watabou's weight of `4 - occupied` makes
        // such a cell weight zero and so unreachable; saying it outright is
        // clearer than relying on a weight that happens to vanish.
        uint32_t open = 0;
        uint32_t outside = 0;

        for (const uint32_t c : members(house.grid, block))
        {
          for (const Dir d : geom2::kCardinal)
          {
            const uint32_t n = house.grid.neighbour(c, d);

            if (house.grid.contains(block, n))
            {
              continue;
            }

            if (n != geom2::kNoCell && house.grid.contains(ground.area, n))
            {
              ++open;
            }
            else
            {
              ++outside;
            }
          }
        }

        if (open == 0 || outside == 0)
        {
          continue;
        }

        if (!stairwellFits(house.grid, ground.area, block, ground.loops.size() - 1))
        {
          continue;
        }

        candidates.push_back(block);

        // Fewer inward neighbours is better: a stair belongs in a nook, not in
        // the middle of an open span where it would stand in the way of
        // everything. A tower is judged the same way, per cell of its edge.
        weights.push_back(1. + double(outside) / double(open + outside));
      }

      // A tower that does not fit comes down a size rather than being given up
      // on. A castle with a two-cell stair is a castle; one with a per-floor
      // flight found afterwards is not what was asked for. The two lines are
      // one size and are tried together.
      if (!candidates.empty() && (shape.rows != 1 || shape.columns != 2))
      {
        break;
      }
    }

    if (!candidates.empty())
    {
      const Board block = stream.weighted(candidates, weights);

      // The flight goes in the cell of the block with a way out of it, and the
      // rest of the block is landing.
      std::vector<uint32_t> feet;
      std::vector<Dir> ways;

      for (const uint32_t c : members(house.grid, block))
      {
        for (const Dir d : geom2::kCardinal)
        {
          const uint32_t n = house.grid.neighbour(c, d);

          if (!house.grid.contains(block, n) && n != geom2::kNoCell && house.grid.contains(ground.area, n))
          {
            feet.push_back(c);
            ways.push_back(d);
          }
        }
      }

      // A line's exit is at one of its ends when an end has somewhere to let
      // onto, so the flights run the line's length from the door: an exit on
      // the long side leaves a well one cell long, which is a trapdoor.
      std::vector<uint32_t> endFeet;
      std::vector<Dir> endWays;

      for (size_t f = 0; f < feet.size(); ++f)
      {
        const uint32_t along = house.grid.neighbour(feet[f], opposite(ways[f]));

        if (house.grid.contains(block, along))
        {
          endFeet.push_back(feet[f]);
          endWays.push_back(ways[f]);
        }
      }

      if (!endFeet.empty())
      {
        feet = endFeet;
        ways = endWays;
      }

      const uint32_t k = stream.below(uint32_t(feet.size()));

      Stairwell reserved;
      reserved.block = block;
      reserved.integratedTower = integratedTower;
      reserved.stair = feet[k];
      reserved.exit = ways[k];
      reserved.landing = house.grid.neighbour(reserved.stair, reserved.exit);

      house.stairwell = reserved;
      house.scheme = StairScheme::Stairwell;

      return true;
    }
  }

  house.scheme = StairScheme::PerFloor;

  return true;
}

namespace {

constexpr uint32_t kUnreached = 0xFFFFFFFFu;

// Whether any four-neighbour of `c` is in `b`.
bool adjoins(const CellGrid &grid, const Board &b, const uint32_t c)
{
  for (const geom2::Dir d : geom2::kCardinal)
  {
    const uint32_t n = grid.neighbour(c, d);

    if (n != geom2::kNoCell && grid.contains(b, n))
    {
      return true;
    }
  }

  return false;
}

// Shortest four-connected walk from `from` to the nearest cell of `into`, never
// leaving `open`. Returned far end first - index zero is `from` and the last
// entry is the cell of `into` it arrived at - because a branch is laid from the
// room it serves back towards the spine it joins, not the other way.
std::vector<uint32_t> walkTo(const CellGrid &grid, const Board &open, const Board &into, const uint32_t from)
{
  const size_t cells = size_t(grid.width()) * size_t(grid.height());

  if (from >= cells)
  {
    return {};
  }

  if (grid.contains(into, from))
  {
    return {from};
  }

  std::vector<uint32_t> parent(cells, geom2::kNoCell);
  std::vector<uint32_t> queue{from};

  parent[from] = from;

  uint32_t arrived = geom2::kNoCell;

  for (size_t head = 0; head < queue.size() && arrived == geom2::kNoCell; ++head)
  {
    const uint32_t c = queue[head];

    for (const geom2::Dir d : geom2::kCardinal)
    {
      const uint32_t n = grid.neighbour(c, d);

      if (n == geom2::kNoCell || parent[n] != geom2::kNoCell || !grid.contains(open, n))
      {
        continue;
      }

      parent[n] = c;

      if (grid.contains(into, n))
      {
        arrived = n;
        break;
      }

      queue.push_back(n);
    }
  }

  if (arrived == geom2::kNoCell)
  {
    return {};
  }

  std::vector<uint32_t> walk;

  for (uint32_t c = arrived;; c = parent[c])
  {
    walk.push_back(c);

    if (c == from)
    {
      break;
    }
  }

  std::reverse(walk.begin(), walk.end());

  return walk;
}

// How far every cell of `open` is from the nearest cell of `seed`, measured
// inside `open`. What the spine is grown against: a cell further from the
// corridor than a room is deep can never open onto it.
std::vector<uint32_t> spread(const CellGrid &grid, const Board &open, const Board &seed)
{
  std::vector<uint32_t> distance(size_t(grid.width()) * size_t(grid.height()), kUnreached);
  std::vector<uint32_t> queue = members(grid, seed);

  for (const uint32_t c : queue)
  {
    distance[c] = 0;
  }

  for (size_t head = 0; head < queue.size(); ++head)
  {
    const uint32_t c = queue[head];

    for (const geom2::Dir d : geom2::kCardinal)
    {
      const uint32_t n = grid.neighbour(c, d);

      if (n == geom2::kNoCell || distance[n] != kUnreached || !grid.contains(open, n))
      {
        continue;
      }

      distance[n] = distance[c] + 1;
      queue.push_back(n);
    }
  }

  return distance;
}

// Lays a walk down as corridor, from its far end, stopping at the first cell
// that touches what is already there.
//
// Stopping is the whole point. A branch that ran its full length would meet the
// spine at both ends and the corridor would become a ring - and a ring encloses
// ground, which is how a floor ends up with a room the subdivision cannot cut.
// One contact point, one tree, no rings.
void layDown(const CellGrid &grid, Board &spine, const std::vector<uint32_t> &walk)
{
  // Against the spine as it stood before this branch was laid, not against the
  // branch itself. Testing against the growing board means the second cell of
  // every walk touches the first, the walk stops two cells in, and the spine
  // comes out as a handful of stubs that look like corridor and connect
  // nothing.
  const Board before = spine;

  for (const uint32_t c : walk)
  {
    if (grid.contains(before, c))
    {
      return;
    }

    const bool meets = adjoins(grid, before, c);

    grid.add(spine, c);

    if (meets)
    {
      return;
    }
  }
}

// Fills every corner where two corridor cells meet diagonally and nowhere else.
//
// That configuration is exactly what makes a contour visit a node twice, and a
// room whose contour pinches is one the wall builder cannot walk. Filling the
// corner costs a cell and keeps the corridor a corridor. False when a corner
// sits against ground the corridor may not take, which is a spine to discard
// rather than one to repair.
bool unpinch(const CellGrid &grid, const Board &open, Board &spine)
{
  for (uint32_t pass = 0; pass < 16; ++pass)
  {
    bool changed = false;

    // Each diagonal pair is visited once, from the cell in the lower row.
    for (const uint32_t c : members(grid, spine))
    {
      const int32_t i = int32_t(grid.cellRow(c));
      const int32_t j = int32_t(grid.cellColumn(c));

      for (const int32_t step : {1, -1})
      {
        const uint32_t diagonal = grid.cell(i + 1, j + step);

        if (diagonal == geom2::kNoCell || !grid.contains(spine, diagonal))
        {
          continue;
        }

        const uint32_t a = grid.cell(i + 1, j);
        const uint32_t b = grid.cell(i, j + step);

        if ((a != geom2::kNoCell && grid.contains(spine, a)) || (b != geom2::kNoCell && grid.contains(spine, b)))
        {
          continue;
        }

        const uint32_t fill = a != geom2::kNoCell && grid.contains(open, a) ? a
                              : (b != geom2::kNoCell && grid.contains(open, b) ? b : geom2::kNoCell);

        if (fill == geom2::kNoCell)
        {
          return false;
        }

        grid.add(spine, fill);
        changed = true;
      }
    }

    if (!changed)
    {
      return true;
    }
  }

  return false;
}

// Absorbs anything the spine has closed a small pocket around, and cuts the ring
// wherever it has closed around more than that.
//
// A ring of corridor is a mistake in two directions at once. The pocket inside
// it is a room with no outside wall, so it can have no window; and the ring
// itself has a hole, so it is not a contour a room can be built from - a room
// carries one loop and a ring needs two. A pocket of a cell or two becomes
// corridor. Anything larger means the spine went somewhere it should not have,
// and the cure is to cut the ring: one cell removed from a cycle makes it a tree
// again, and a tree encloses nothing.
bool untangle(const CellGrid &grid, Board &spine, const std::vector<uint32_t> &anchors)
{
  for (uint32_t pass = 0; pass < 32; ++pass)
  {
    const std::vector<Chain> loops = grid.outline(spine);

    if (loops.size() <= 1)
    {
      return true;
    }

    const Board pockets = grid.contourArea(grid.outerLoop(spine)) & ~spine;

    if (!pockets.empty() && pockets.count() <= 2)
    {
      spine |= pockets;

      continue;
    }

    uint32_t drop = geom2::kNoCell;

    for (const uint32_t c : members(grid, spine))
    {
      if (std::find(anchors.begin(), anchors.end(), c) != anchors.end())
      {
        continue;
      }

      if (!geom2::removalKeepsConnected(grid, spine, c))
      {
        continue;
      }

      Board trial = spine;
      grid.remove(trial, c);

      if (grid.outline(trial).size() < loops.size())
      {
        drop = c;

        break;
      }
    }

    if (drop == geom2::kNoCell)
    {
      return false;
    }

    grid.remove(spine, drop);
  }

  return false;
}

// Ground a new branch may not cross: cells that touch the spine at a corner and
// nowhere else.
//
// This is where rings come from. A branch is stopped the moment it touches the
// spine squarely, so it can only meet it once - but nothing stops it running
// diagonally PAST the spine, and the corner filler that comes later then joins
// the two into a cycle. Forbidding the diagonal contact up front is the repair
// that does not have to be undone.
Board cornersOf(const CellGrid &grid, const Board &spine)
{
  const Board north = grid.shifted(spine, geom2::Dir::North);
  const Board south = grid.shifted(spine, geom2::Dir::South);

  const Board square = north | south | grid.shifted(spine, geom2::Dir::East) | grid.shifted(spine, geom2::Dir::West);

  const Board all = square | grid.shifted(north, geom2::Dir::East) | grid.shifted(north, geom2::Dir::West) |
                    grid.shifted(south, geom2::Dir::East) | grid.shifted(south, geom2::Dir::West);

  return all & ~square & ~spine;
}

// The cells of `open` that stand against the OUTSIDE of the floor.
//
// `filled` is the floor with its courts filled in, and passing it rather than
// the floor itself is the whole point: on a courtyard plan the inner face of the
// range is an edge too, and a rim that counts it thinks every cell of a
// two-deep range is already on the outside - so the spine is told it has
// nothing to reach for and the ring never gets cut.
Board rimOf(const CellGrid &grid, const Board &filled, const Board &open)
{
  Board rim;

  for (const uint32_t c : members(grid, open))
  {
    for (const geom2::Dir d : geom2::kCardinal)
    {
      const uint32_t n = grid.neighbour(c, d);

      if (n == geom2::kNoCell || !grid.contains(filled, n))
      {
        grid.add(rim, c);
        break;
      }
    }
  }

  return rim;
}

} // namespace

bool exteriorEdge(const CellFloor &floor, const uint32_t edge)
{
  for (const Chain &loop : floor.loops)
  {
    if (std::find(loop.begin(), loop.end(), edge) != loop.end())
    {
      return true;
    }
  }

  return false;
}

bool carveCourts(const CellGrid &grid,
                 const Board &area,
                 const double share,
                 const uint32_t wanted,
                 const double streetJ,
                 const double streetI,
                 Stream &stream,
                 Board &out,
                 Board &courts)
{
  out = area;
  courts = Board();

  if (wanted == 0 || share <= 0 || area.empty())
  {
    return false;
  }

  const CellBounds whole = geom2::bounds(area);

  for (uint32_t k = 0; k < wanted; ++k)
  {
    // Each court is drawn against what is left, so a second one is smaller than
    // the first and lands somewhere else - which is what the castle plan shows
    // and what one court repeated twice would not.
    const double want = double(out.count()) * share;

    if (want < 4)
    {
      break;
    }

    std::vector<Board> candidates;
    std::vector<double> weights;

    // How many loops the mass already has. A court adds at most one hole, and
    // the cap has to be counted from here rather than fixed at two - fixed, the
    // SECOND court was unbuildable by construction, because the first had
    // already spent the allowance. A castle is two courts of unequal size and
    // it was coming out as one.
    const size_t already = grid.outline(out).size();

    const auto accept = [&](const Board &court) {
            const Board rest = out & ~court;

            // What is left has to be a building: one piece, with at most the
            // one hole this court just made, and enough of it to be worth
            // roofing.
            if (rest.count() * 3 < out.count() || !grid.connected(rest))
            {
              return;
            }

            // And it has to stay a building that is two cells thick.
            //
            // Cutting a court out can leave a strip of ground one cell wide
            // between it and the next court, or between it and the outside.
            // Nothing downstream refuses that - the strip is connected, its
            // contour is simple, it becomes a room - and it renders as a lone
            // wall standing across the courtyard with no roof and no building
            // attached to it. `narrow` is the same primitive the room floor
            // uses: a cell is narrow when it belongs to no two-by-two block of
            // its own, so a court may not make the mass narrower than it found
            // it.
            if (geom2::narrow(grid, rest).count() > geom2::narrow(grid, out).count())
            {
              return;
            }

            const std::vector<Chain> loops = grid.outline(rest);

            if (loops.empty() || loops.size() > already + 1 || !grid.simpleOutline(loops))
            {
              return;
            }


            // How enclosed it is: the share of its own perimeter that has
            // building on the far side. A court open on every side is a gap
            // between two wings, and one enclosed on all four is a light well
            // you cannot get into - so the middle of the range is wanted, and
            // three sides of four is where a motor court sits.
            uint32_t walled = 0;
            uint32_t edges = 0;

            for (const auto &[i, j] : geom2::cells(court))
            {
              const uint32_t c = grid.cell(int32_t(i), int32_t(j));

              for (const geom2::Dir d : geom2::kCardinal)
              {
                const uint32_t n = grid.neighbour(c, d);

                if (n != geom2::kNoCell && grid.contains(court, n))
                {
                  continue;
                }

                ++edges;
                walled += n != geom2::kNoCell && grid.contains(rest, n) ? 1u : 0u;
              }
            }

            if (edges == 0)
            {
              return;
            }

            const double enclosed = double(walled) / double(edges);

            // Mostly enclosed, and the floor matters more than it looks. A
            // court is a hole in a mass; one that opens along a whole side is a
            // gap between two wings, and enough of those and the building has
            // no outer face left to put a front door in - measured, a third of
            // castles were giving their court up at the door for exactly that.
            if (enclosed < .7)
            {
              return;
            }

            // Scored, then sampled. Three quarters enclosed is the peak - a
            // court wants a way in - and a court near the middle of the plan
            // beats one in a corner, measured against the whole footprint so
            // the second court is judged by the building and not by the
            // remainder it was cut from.
            const double offEnclosed = enclosed - .78;
            const double middleI = (double(whole.i0) + double(whole.i1)) * .5;
            const double middleJ = (double(whole.j0) + double(whole.j1)) * .5;
            const CellBounds courtBox = geom2::bounds(court);
            const double ci = (double(courtBox.i0) + double(courtBox.i1)) * .5 - middleI;
            const double cj = (double(courtBox.j0) + double(courtBox.j1)) * .5 - middleJ;
            const double span = std::max(1., double(whole.i1 - whole.i0 + whole.j1 - whole.j0) * .5);
            const double offCentre = std::sqrt(ci * ci + cj * cj) / span;

            // Squarish, because a court two cells wide and eight long is a
            // light well whatever its area.
            const double squareness =
              double(std::min(courtBox.i1 - courtBox.i0 + 1, courtBox.j1 - courtBox.j0 + 1)) /
              double(std::max(courtBox.i1 - courtBox.i0 + 1, courtBox.j1 - courtBox.j0 + 1));

            const double score = std::exp(-offEnclosed * offEnclosed * 18.) *
                                 std::exp(-offCentre * offCentre * 3.) * squareness * squareness;

            if (score <= 1e-6)
            {
              return;
            }

            candidates.push_back(court);
            weights.push_back(score);
    };

    // Grow courts from seed cells instead of enumerating rectangles. Each
    // addition is chosen from the current frontier: shared edges make the
    // region compact, distance to the footprint centre keeps it useful, and a
    // stable cell hash breaks ties so different seeds do not all become the
    // same stamped box. Legality is still judged on the finished artifact by
    // `accept` above.
    const std::vector<uint32_t> seeds = members(grid, out);

    for (const uint32_t seed : seeds)
    {
      for (const double scale : {.75, 1., 1.25})
      {
        const uint32_t target = std::max(4u, uint32_t(std::llround(want * scale)));
        Board court;
        grid.add(court, seed);

        while (court.count() < target)
        {
          std::set<uint32_t> frontier;

          for (const uint32_t c : members(grid, court))
          {
            for (const geom2::Dir d : geom2::kCardinal)
            {
              const uint32_t n = grid.neighbour(c, d);
              if (n != geom2::kNoCell && grid.contains(out, n) && !grid.contains(court, n)) frontier.insert(n);
            }
          }

          if (frontier.empty()) break;

          uint32_t best = geom2::kNoCell;
          double bestScore = -1e300;

          for (const uint32_t n : frontier)
          {
            uint32_t neighbours = 0;
            for (const geom2::Dir d : geom2::kCardinal)
            {
              const uint32_t q = grid.neighbour(n, d);
              neighbours += q != geom2::kNoCell && grid.contains(court, q) ? 1u : 0u;
            }

            const double di = double(grid.cellRow(n)) - (double(whole.i0) + double(whole.i1)) * .5;
            const double dj = double(grid.cellColumn(n)) - (double(whole.j0) + double(whole.j1)) * .5;
            uint64_t hash = uint64_t(n) * 0x9E3779B97F4A7C15ull ^ uint64_t(seed) * 0xBF58476D1CE4E5B9ull ^ uint64_t(court.count());
            hash ^= hash >> 30;
            const double tie = double(hash & 0xffffu) / 65535.;
            const double score = double(neighbours) * 8. - (di * di + dj * dj) * .04 + tie;

            if (score > bestScore)
            {
              bestScore = score;
              best = n;
            }
          }

          if (best == geom2::kNoCell) break;
          grid.add(court, best);
        }

        if (court.count() >= uint32_t(std::floor(want * .6)) && court.count() <= uint32_t(std::ceil(want * 1.6)))
        {
          accept(court);
        }
      }
    }

    if (candidates.empty())
    {
      break;
    }

    const Board chosen = stream.weighted(candidates, weights);

    courts |= chosen;
    out = out & ~chosen;
  }

  (void)streetJ;
  (void)streetI;

  return !courts.empty();
}


// A yard laid against the back of the mass.
//
// The same construction as a court and the opposite acceptance: a court is a
// hole and must be mostly enclosed, a yard is a piece of ground held by the
// house on two or three sides and open to the lot on the rest. It goes at the
// back because that is where a yard goes: the front of a house is its frontage
// and is worth money, and the work happens behind.
//
// Laid AGAINST the footprint rather than bitten out of it. Biting was tried
// and it is wrong at the bottom of the scale, which is exactly where yards
// matter: a cottage of twelve cells cannot give four of them up and still be
// a cottage, so the small classes - the ones whose plots are worked from the
// yard - were the ones that never got one. The yard is ground the lot has and
// the house does not stand on, chosen for how the house embraces it, so the
// footprint keeps its band and the yard is as big as the ground allows.
//
// It joins `courts` because downstream it is the same thing: cells that are
// not floor, so nothing stands over them and nothing roofs them, and a room in
// the door graph so the house opens onto it. Which of the two an outdoor room
// IS - a yard or a court - is read back from its edges by whoever needs to
// know, which is honest: the difference between them is exactly whether one
// side is open.
bool carveYard(const CellGrid &grid,
               const Board &area,
               const Board &buildable,
               const double share,
               const double streetJ,
               const double streetI,
               Stream &stream,
               Board &yard)
{
  yard = Board();

  if (share <= 0 || area.empty())
  {
    return false;
  }

  // As big as the house's share says, and never smaller than a two-by-two: a
  // yard one cell wide is a passage.
  const double want = std::max(4., double(area.count()) * share);

  const double streetLength = std::sqrt(streetJ * streetJ + streetI * streetI);

  double front = -1e300;
  double back = 1e300;

  for (const auto &[i, j] : geom2::cells(area))
  {
    const double along = double(j) * streetJ + double(i) * streetI;
    front = std::max(front, along);
    back = std::min(back, along);
  }

  // Where it may lie: the lot's ground the house is not on, within a yard's
  // reach of the house.
  const Board open = buildable & ~area;

  if (open.empty())
  {
    return false;
  }

  const CellBounds box = geom2::bounds(open);

  std::vector<Board> candidates;
  std::vector<double> weights;

  for (uint32_t i0 = box.i0; i0 <= box.i1; ++i0)
  {
    for (uint32_t j0 = box.j0; j0 <= box.j1; ++j0)
    {
      for (uint32_t rows = 2; i0 + rows - 1 <= box.i1 && rows <= 8; ++rows)
      {
        for (uint32_t columns = 2; j0 + columns - 1 <= box.j1 && columns <= 8; ++columns)
        {
          const double size = double(rows) * double(columns);

          if (size < want * .6 || size > want * 1.8)
          {
            continue;
          }

          Board piece;
          bool ground = true;

          for (uint32_t i = i0; i < i0 + rows && ground; ++i)
          {
            for (uint32_t j = j0; j < j0 + columns && ground; ++j)
            {
              ground = grid.contains(open, grid.cell(int32_t(i), int32_t(j)));

              if (ground)
              {
                piece.set(i, j);
              }
            }
          }

          if (!ground)
          {
            continue;
          }

          // How held it is: the share of its own edge with the house beyond.
          // Two sides of four is an L embracing it, three is a U; one side is
          // a strip along a wall, and four is a court and not this.
          uint32_t walled = 0;
          uint32_t edges = 0;
          double along = 0;
          double nearest = -1e300;

          for (const auto &[i, j] : geom2::cells(piece))
          {
            const uint32_t c = grid.cell(int32_t(i), int32_t(j));
            along += double(j) * streetJ + double(i) * streetI;
            nearest = std::max(nearest, double(j) * streetJ + double(i) * streetI);

            for (const geom2::Dir d : geom2::kCardinal)
            {
              const uint32_t n = grid.neighbour(c, d);

              if (n != geom2::kNoCell && grid.contains(piece, n))
              {
                continue;
              }

              ++edges;
              walled += n != geom2::kNoCell && grid.contains(area, n) ? 1u : 0u;
            }
          }

          if (edges == 0)
          {
            continue;
          }

          along /= double(piece.count());

          const double held = double(walled) / double(edges);

          // At least a wall's worth of house against it, or the house cannot
          // open onto it; and not embraced on every side, or it is a court.
          if (walled < 2 || held < .25 || held > .85)
          {
            continue;
          }

          // Behind the front of the house - every cell of it, not its middle.
          // A yard on the street is a gap in the frontage, and a yard down the
          // side that reaches the street is the same gap seen from the corner,
          // so anything whose nearest cell comes within half a cell of the
          // house's front is refused, and the further back the better.
          if (nearest > front - .5)
          {
            continue;
          }

          const double depth = streetLength > 1e-9 ? (front - back) / streetLength : 1.;
          const double setback = streetLength > 1e-9 ? (front - along) / streetLength : 0.;

          const double squareness = double(std::min(rows, columns)) / double(std::max(rows, columns));
          const double offHeld = held - .55;
          const double score = std::exp(-offHeld * offHeld * 10.) * squareness *
                               (1. + std::min(1., setback / std::max(1., depth)));

          if (score <= 1e-6)
          {
            continue;
          }

          candidates.push_back(piece);
          weights.push_back(score);
        }
      }
    }
  }

  if (candidates.empty())
  {
    return false;
  }

  yard = stream.weighted(candidates, weights);

  return true;
}

// The arms of a cross, as rooms.
//
// An axial building is not subdivided, and that is the whole point of it being
// a kind of its own. A nave is ONE space the length of the building; a chancel
// is one space at the end of it; and running the ordinary subdivision over a
// church produces - measured, before this existed - a kitchen, two bedrooms, a
// library and a living room inside a cruciform footprint.
//
// The partition is read off the shape rather than remembered from the archetype,
// because by the time the rooms are cut the footprint has been masked to the
// buildable ground and is no longer the cross that was drawn. A slice across the
// long axis is WIDE where the transepts are and narrow along the nave, and that
// one measurement names every arm: what is short of the transepts is the nave,
// what is beyond them is the chancel, and what stands off them to either side
// is a transept.
//
// False when the shape is not read as a cross - masked to awkward ground it may
// not be one - and the caller then cuts it the ordinary way.
bool partitionAxial(const CellGrid &grid,
                    const Board &area,
                    const uint32_t entranceLanding,
                    std::vector<Board> &pieces,
                    std::vector<rewrite::RoomKind> &kinds,
                    const BellTowers towers,
                    const std::vector<uint32_t> *westFrontSlices)
{
  pieces.clear();
  kinds.clear();

  if (area.count() < 12)
  {
    return false;
  }

  const CellBounds box = geom2::bounds(area);
  const bool longAlongRows = (box.i1 - box.i0) >= (box.j1 - box.j0);

  const uint32_t low = longAlongRows ? box.i0 : box.j0;
  const uint32_t high = longAlongRows ? box.i1 : box.j1;

  if (high <= low + 3)
  {
    return false;
  }

  // How wide the building is at each step along its length.
  std::vector<uint32_t> width(high - low + 1, 0);

  for (const auto &[i, j] : geom2::cells(area))
  {
    ++width[(longAlongRows ? i : j) - low];
  }

  // A west front is wider than the nave, and it is not a crossing. Where the
  // towers are at the front, the two bays inside the door are left out of the
  // search for the transept band, or the front reads as transepts and the
  // chancel goes to the wrong end. Which end the door is at is asked of the
  // door against the middle of the LENGTH here, because the crossing is not
  // known yet.
  std::vector<bool> frontBay(width.size(), false);

  // The slices the shell widened for the west front, told rather than
  // inferred: inferring them from the widths found a crossing on a short
  // church and put the towers on the transepts.
  if (towers == BellTowers::TwinFront && westFrontSlices != nullptr)
  {
    for (const uint32_t slice : *westFrontSlices)
    {
      if (slice >= low && slice <= high)
      {
        frontBay[slice - low] = true;
      }
    }
  }

  uint32_t widest = 0;
  uint32_t narrowest = 0xFFFFFFFFu;

  for (size_t k = 0; k < width.size(); ++k)
  {
    if (frontBay[k])
    {
      continue;
    }

    widest = std::max(widest, width[k]);
    narrowest = std::min(narrowest, width[k]);
  }

  // No transepts is a church too.
  //
  // An aisle-less church is a bar: entrance bay, chancel, tower. It used to be
  // refused here and fall through to the ordinary subdivision, which put a
  // kitchen and two bedrooms in it - so a plan with no wide band gets an empty
  // band rather than a refusal, and everything below works unchanged.
  const bool crossed = widest > narrowest + 1;
  const uint32_t bar = (widest + narrowest) / 2 + 1;

  size_t first = width.size();
  size_t last = 0;

  if (crossed)
  {
    for (size_t k = 0; k < width.size(); ++k)
    {
      if (!frontBay[k] && width[k] >= bar)
      {
        first = std::min(first, k);
        last = std::max(last, k);
      }
    }
  }

  if (crossed && first > last)
  {
    return false;
  }

  // Which end the door is at, asked of the DOOR.
  //
  // It was inferred from the street vector, on the grounds that the footprint
  // had been turned to face the road - two inferences chained, and they came
  // out backwards: the long arm was being called the nave and the short one the
  // chancel. The entrance landing is a cell, and which side of the crossing it
  // falls on is not a matter of interpretation.
  bool doorAtLow = true;

  if (entranceLanding != geom2::kNoCell && grid.contains(area, entranceLanding))
  {
    const uint32_t at = longAlongRows ? grid.cellRow(entranceLanding) : grid.cellColumn(entranceLanding);

    doorAtLow = crossed ? size_t(at - low) <= (first + last) / 2 : size_t(at - low) * 2 < width.size();
  }

  // The nave's own width, taken from the slices that are not the crossing, so
  // the crossing can be told from the transepts standing off it.
  uint32_t naveLow = 0xFFFFFFFFu;
  uint32_t naveHigh = 0;

  for (const auto &[i, j] : geom2::cells(area))
  {
    const size_t k = (longAlongRows ? i : j) - low;

    if ((crossed && k >= first && k <= last) || frontBay[k])
    {
      continue;
    }

    const uint32_t across = longAlongRows ? j : i;
    naveLow = std::min(naveLow, across);
    naveHigh = std::max(naveHigh, across);
  }

  if (naveLow > naveHigh)
  {
    return false;
  }

  // The congregation occupies the broad, undivided main hall. Reserve the
  // final third for the chancel instead of making the nave a tiny vestibule.
  const size_t length = width.size();
  const size_t naveShare = std::clamp<size_t>(size_t(double(length)*.60),1,length-1);
  const size_t cut = doorAtLow ? naveShare : length - 1 - naveShare;

  // A square tower at the far end, centred on the axis.
  //
  // Square in plan and small: as many bays deep as it is wide, and never more
  // than three, because a tower is a tower and not a room. Taking the FULL
  // width made it sixteen cells on a four-wide church - bigger than the
  // chancel, which is the one thing a church's proportions may not do - so the
  // cells of those bays outside the tower go back to the chancel, which then
  // wraps it on three sides.
  const size_t belfryBays = length >= 6 ? 2 : 0;

  // Centred on the ground the tower actually stands on, not on the nave.
  //
  // The nave is at the other end of the building and its width is whatever
  // survived the masking there; centring against it put the tower off to one
  // side of the chancel. What matters is the bays the tower occupies, so the
  // middle is taken across THOSE.
  uint32_t towerLow = 0xFFFFFFFFu;
  uint32_t towerHigh = 0;

  for (const auto &[i, j] : geom2::cells(area))
  {
    const size_t k = (longAlongRows ? i : j) - low;
    const bool back = doorAtLow ? k + belfryBays >= length : k < belfryBays;

    if (belfryBays > 0 && back)
    {
      const uint32_t at = longAlongRows ? j : i;
      towerLow = std::min(towerLow, at);
      towerHigh = std::max(towerHigh, at);
    }
  }

  const size_t across = towerLow <= towerHigh ? size_t(towerHigh - towerLow + 1) : 0;
  const size_t side = std::min<size_t>(across, 2);
  const uint32_t belfryLow = towerLow + uint32_t((across - side) / 2);
  const uint32_t belfryHigh = belfryLow + uint32_t(side > 0 ? side - 1 : 0);


  // Two towers at the front: the corners of the first bays inside the door,
  // one each side of the nave. Two cells square where the nave is five or more
  // across, and none below that. Not one cell square: a room one cell wide is
  // entirely narrow, the corridor merge takes it for a passage, and the tower's
  // lantern was left standing on its ladder with no shaft under it.
  uint32_t frontLow = 0xFFFFFFFFu;
  uint32_t frontHigh = 0;

  for (const auto &[i, j] : geom2::cells(area))
  {
    const size_t k = (longAlongRows ? i : j) - low;

    if (belfryBays > 0 && frontBay[k] && !(crossed && k >= first && k <= last))
    {
      const uint32_t at = longAlongRows ? j : i;
      frontLow = std::min(frontLow, at);
      frontHigh = std::max(frontHigh, at);
    }
  }

  const size_t frontAcross = frontLow <= frontHigh ? size_t(frontHigh - frontLow + 1) : 0;
  const size_t twinSide = frontAcross >= 5 ? 2 : 0;
  const bool twin = towers == BellTowers::TwinFront && belfryBays > 0 && twinSide > 0;

  Board nave;
  Board chancel;
  Board north;
  Board south;
  Board belfry;
  Board belfry2;

  for (const auto &[i, j] : geom2::cells(area))
  {
    const size_t k = (longAlongRows ? i : j) - low;
    const uint32_t across = longAlongRows ? j : i;

    if (twin)
    {
      const bool front = frontBay[k];

      if (front && !(crossed && k >= first && k <= last))
      {
        if (across < frontLow + twinSide)
        {
          belfry.set(i, j);

          continue;
        }

        // The right tower: the last `twinSide` cells across.
        if (across + twinSide > frontHigh)
        {
          belfry2.set(i, j);

          continue;
        }
      }
    }

    if (k >= first && k <= last && across < naveLow)
    {
      north.set(i, j);

      continue;
    }

    if (k >= first && k <= last && across > naveHigh)
    {
      south.set(i, j);

      continue;
    }

    // The belfry is the far END of the chancel, not the crossing.
    //
    // The crossing was tried first and it cannot work: the transept band sits
    // between the two arms, so taking it out leaves whichever arm straddles it
    // in two pieces, and a room in two pieces is not a room. The back of the
    // church has no such problem - the tower is the last bay of the chancel,
    // the chancel stays in one piece behind it, and a tower at the east end
    // with the bells in it is what was asked for.
    const uint32_t across2 = longAlongRows ? j : i;
    const bool atTheBack = !twin && belfryBays > 0 && side > 0 &&
                           (doorAtLow ? k + belfryBays >= length : k < belfryBays) && across2 >= belfryLow &&
                           across2 <= belfryHigh;

    if (atTheBack)
    {
      belfry.set(i, j);

      continue;
    }

    (k <= cut ? (doorAtLow ? nave : chancel) : (doorAtLow ? chancel : nave)).set(i, j);
  }

  const auto keep = [&](Board &piece, const rewrite::RoomKind kind) {
    if (piece.empty())
    {
      return true;
    }

    const std::vector<Chain> loops = grid.outline(piece);

    if (loops.size() != 1 || !grid.simpleOutline(loops) || !grid.connected(piece))
    {
      return false;
    }

    pieces.push_back(piece);
    kinds.push_back(kind);

    return true;
  };

  // The shape settles three things and no more: the entrance bay, the
  // chancel and the belfry. Everything else - the arms off the crossing, and
  // whatever an apse or a porch left over - is handed back UNNAMED, for the
  // subdivision to cut into the small rooms a church has and the church
  // programme to name. Naming the arms here made every church the same church
  // with the same two rooms in the same two places.
  if (!keep(nave, rewrite::RoomKind::Nave) || !keep(chancel, rewrite::RoomKind::Chancel) ||
      !keep(belfry, rewrite::RoomKind::Belfry) || !keep(belfry2, rewrite::RoomKind::Belfry) ||
      !keep(north, rewrite::RoomKind::Count) || !keep(south, rewrite::RoomKind::Count))
  {
    pieces.clear();
    kinds.clear();

    return false;
  }

  return pieces.size() >= 2;
}

Board reserveCirculation(const CellHouse &house,
                         const CellFloor &floor,
                         const CellDwellingSettings &settings,
                         Ledger &ledger)
{
  // Inert below the threshold, and every house-sized class is below it. A
  // cottage's corridors emerge from the subdivision, which is what the
  // `hallways` tag is for; reserving a one-cell spine across twenty cells would
  // eat the plan instead of serving it.
  const CellGrid &grid = house.grid;

  // A floor with a court in it always has planned circulation, whatever its
  // size, and not because it is large. A ring cannot be subdivided - the
  // subdivision cuts simple regions and a ring is not one - so the ring has to
  // be cut somewhere, and where it is cut is a decision rather than an
  // accident. In a castle it is the gate passage.
  const bool ringed = grid.outline(floor.area).size() > 1;

  const auto circulationThreshold=house.building==BuildingClass::Castle ?
    std::max<uint32_t>(32,settings.circulationThresholdCells*2/3) : settings.circulationThresholdCells;
  if (!ringed && floor.area.count() < circulationThreshold)
  {
    return Board();
  }

  // The stairwell is already its own room on every floor. The spine may run up
  // to it and never through it.
  Board open = floor.area;

  if (house.stairwell.has_value())
  {
    open = open & ~house.stairwell->block;
  }

  if (open.empty())
  {
    return Board();
  }

  // A court is circulation.
  //
  // The reference plans are explicit about this and the first version of the
  // spine was not: in a castle you reach everything through a courtyard, and
  // the passage that exists besides is a small part of the plan. Measured
  // against the drawings, a spine that ignored the courts took a third of the
  // storey where the references spend between a twentieth and an eighth. So a
  // piece of floor that touches a court is already served, and the corridor is
  // only dug for the ground that is not.
  const Board courtBank = house.courts.empty() ? Board() : grid.shifted(house.courts, geom2::Dir::North) |
                                                             grid.shifted(house.courts, geom2::Dir::South) |
                                                             grid.shifted(house.courts, geom2::Dir::East) |
                                                             grid.shifted(house.courts, geom2::Dir::West);



  // The fixed points. Before a single room exists a floor has at most two: the
  // cell inside the front door, and the foot of the climb. A corridor that does
  // not join them is decoration.
  std::vector<uint32_t> anchors;

  if (floor.level == 0 && house.entranceLanding != geom2::kNoCell && grid.contains(open, house.entranceLanding))
  {
    anchors.push_back(house.entranceLanding);
  }

  if (house.stairwell.has_value() && house.stairwell->landing != geom2::kNoCell &&
      grid.contains(open, house.stairwell->landing))
  {
    anchors.push_back(house.stairwell->landing);
  }

  if (anchors.empty())
  {
    // No fixed point on this floor: an upper storey climbed by a per-floor
    // flight that phase 6 has not placed yet. The spine still has to start
    // somewhere, and the cell nearest the middle of the floor is the least
    // arbitrary place there is.
    double centreI = 0;
    double centreJ = 0;
    uint32_t count = 0;

    for (const auto &[i, j] : geom2::cells(open))
    {
      centreI += double(i);
      centreJ += double(j);
      ++count;
    }

    centreI /= double(count);
    centreJ /= double(count);

    uint32_t best = geom2::kNoCell;
    double bestReach = 0;

    for (const auto &[i, j] : geom2::cells(open))
    {
      const double di = double(i) - centreI;
      const double dj = double(j) - centreJ;
      const double reach = di * di + dj * dj;

      if (best == geom2::kNoCell || reach < bestReach)
      {
        best = grid.cell(int32_t(i), int32_t(j));
        bestReach = reach;
      }
    }

    anchors.push_back(best);
  }

  Board spine;
  grid.add(spine, anchors.front());

  for (size_t k = 1; k < anchors.size(); ++k)
  {
    const std::vector<uint32_t> walk = walkTo(grid, open & ~cornersOf(grid, spine), spine, anchors[k]);

    if (walk.empty())
    {
      ledger.add(LedgerKind::Repaired, 0, "a floor's circulation could not reach the foot of its climb");

      continue;
    }

    layDown(grid, spine, walk);
  }

  // The spine has to touch the edge of the floor, and the reason is topological
  // rather than architectural: a connected set that reaches the boundary cannot
  // enclose anything, so every piece left to subdivide is guaranteed to be a
  // region with no hole in it. A spine that floats in the middle of a storey
  // leaves an annulus, and an annulus is not a room.
  {
    const Board rim = rimOf(grid, grid.contourArea(grid.outerLoop(floor.area)), open);
    const std::vector<uint32_t> distance = spread(grid, open, spine);

    uint32_t nearest = geom2::kNoCell;
    uint32_t nearestReach = kUnreached;

    for (const uint32_t c : members(grid, rim))
    {
      if (distance[c] < nearestReach)
      {
        nearest = c;
        nearestReach = distance[c];
      }
    }

    if (nearest != geom2::kNoCell && nearestReach > 0)
    {
      layDown(grid, spine, walkTo(grid, open & ~cornersOf(grid, spine), spine, nearest));
    }
  }

  // The court's own edge, reached across the range. With the spine touching both
  // the outside and the court, what is left of the ring is a strip with no hole
  // in it - which is the whole reason the cut is made here rather than left for
  // the subdivision to fail on.
  if (ringed)
  {
    // EVERY court, one at a time. A castle has two of unequal size, and cutting
    // to one of them leaves the other still enclosed by an unbroken ring - a
    // piece the subdivision cannot cut, which comes back as one three-hundred
    // cell room with three loops in it.
    Board pockets = grid.contourArea(grid.outerLoop(floor.area)) & ~floor.area;

    while (!pockets.empty())
    {
      uint32_t i = 0;
      uint32_t j = 0;
      pockets.first(i, j);

      const Board court = grid.flood(pockets, grid.cell(int32_t(i), int32_t(j)));
      pockets = pockets & ~court;

      Board bank;

      for (const uint32_t c : members(grid, open))
      {
        if (adjoins(grid, court, c))
        {
          grid.add(bank, c);
        }
      }

      const std::vector<uint32_t> distance = spread(grid, open, spine);

      uint32_t nearest = geom2::kNoCell;
      uint32_t nearestReach = kUnreached;

      for (const uint32_t c : members(grid, bank))
      {
        if (distance[c] < nearestReach)
        {
          nearest = c;
          nearestReach = distance[c];
        }
      }

      if (nearest != geom2::kNoCell && nearestReach > 0)
      {
        layDown(grid, spine, walkTo(grid, open & ~cornersOf(grid, spine), spine, nearest));
      }
    }
  }


  // The court's edge, where this floor has one. Reached for the same reason the
  // outside is: a corridor that never meets the court is a corridor nobody
  // walks, because the court is how the building is crossed.
  if (!courtBank.empty() && (spine & courtBank).empty())
  {
    const std::vector<uint32_t> distance = spread(grid, open, spine);

    uint32_t nearest = geom2::kNoCell;
    uint32_t nearestReach = kUnreached;

    for (const uint32_t c : members(grid, open & courtBank))
    {
      if (distance[c] < nearestReach)
      {
        nearest = c;
        nearestReach = distance[c];
      }
    }

    if (nearest != geom2::kNoCell && nearestReach > 0)
    {
      layDown(grid, spine, walkTo(grid, open & ~cornersOf(grid, spine), spine, nearest));
    }
  }

  // Everything up to here is the part the floor cannot do without: the hall
  // from the door to the stair, the reach to the outside wall, and the cut
  // across the range into the court. What follows only serves the plan better.
  const Board mandatory = spine;

  // What the spine is grown against, and it is not "every cell within reach of a
  // corridor".
  //
  // That was the first answer and it is wrong by a factor of four. A room needs
  // ONE of its cells on the corridor, not all of them, so reaching every cell
  // digs a corridor beside every wall - measured, it took three fifths of the
  // floor. What actually has to be true is that no piece of floor is left so
  // large that the subdivision cutting it up produces rooms with no route out.
  // Half a dozen rooms is a suite off a passage, which is a plan; fifteen is a
  // warren. Serving fewer than that directly is over-serving: measured at three
  // and a half rooms the splits cost half the storey in corridor, because each
  // one has to cross its piece and the pieces it leaves need crossing too.
  const uint32_t largestPiece = std::max<uint32_t>(16, uint32_t(std::lround(settings.averageRoomCells *
    (house.building==BuildingClass::Castle ? 4. : 6.))));

  for (uint32_t branch = 0; branch < 64; ++branch)
  {
    // The largest piece of floor that neither the corridor nor a court reaches.
    Board biggest;

    {
      Board left = open & ~spine;

      while (!left.empty())
      {
        uint32_t i = 0;
        uint32_t j = 0;
        left.first(i, j);

        const Board piece = grid.flood(left, grid.cell(int32_t(i), int32_t(j)));
        left = left & ~piece;

        if (!(piece & courtBank).empty())
        {
          continue;
        }

        if (piece.count() > biggest.count())
        {
          biggest = piece;
        }
      }
    }

    if (biggest.count() <= largestPiece)
    {
      break;
    }

    // The cell of it furthest from the corridor. The walk back from there
    // crosses the piece, which is what splits it.
    const std::vector<uint32_t> distance = spread(grid, open, spine);

    uint32_t worst = geom2::kNoCell;
    uint32_t worstReach = 0;

    for (const uint32_t c : members(grid, biggest))
    {
      if (distance[c] != kUnreached && distance[c] > worstReach)
      {
        worst = c;
        worstReach = distance[c];
      }
    }

    if (worst == geom2::kNoCell)
    {
      break;
    }

    const std::vector<uint32_t> walk = walkTo(grid, open & ~cornersOf(grid, spine), spine, worst);

    if (walk.size() < 2)
    {
      break;
    }

    const uint32_t before = spine.count();

    layDown(grid, spine, walk);

    if (spine.count() == before)
    {
      break;
    }
  }

  // Widened, where the class walks two abreast.
  //
  // Dilated after the run is laid rather than dug two cells wide, and that is
  // not a shortcut: the walk that chose the route is the same walk either way,
  // so a gallery follows exactly the line a passage would have, and the second
  // cell is taken from whichever side has floor to give. A cell that would come
  // from outside the storey is simply not taken, so a widened corridor stops
  // being wide rather than pushing through a wall.
  if (settings.corridorCells >= 2 && !spine.empty())
  {
    Board wider = spine;

    for (const geom2::Dir d : geom2::kCardinal)
    {
      wider |= grid.shifted(spine, d) & open;
    }

    // Only if it leaves a building behind. Widening doubles the corridor's
    // area, so the bar has to be set against the WIDENED figure and not the
    // narrow one: measured at two thirds it let a mansion come out
    // four-tenths corridor, which is a building made of gallery. A quarter is
    // the most a plan can spend on circulation and still be rooms.
    if (wider.count() * 4 < open.count())
    {
      spine = wider;
    }
    else if (house.frame.cellMetres < settings.minimumCorridorMetres)
    {
      // It would not fit widened, so it does not go in narrow.
      //
      // A corridor is a width before it is a route, and the width is METRES:
      // a city house plans on a one metre cell, so one cell is a gap nobody
      // can pass another person in and the floor is better off with its rooms
      // opening onto each other. A manor's cell is nearly three metres and one
      // of them is already a gallery - the class asks for two out of ambition,
      // not necessity, and it keeps what it can afford.
      spine = Board{};
    }
  }

  // A pocket the spine has cut off that is too small to be a room joins the
  // spine instead of becoming one. One and two cell rooms are the commonest
  // artefact of laying circulation before the subdivision runs, and they are
  // not rooms.
  {
    Board left = open & ~spine;

    while (!left.empty())
    {
      uint32_t i = 0;
      uint32_t j = 0;
      left.first(i, j);

      const Board piece = grid.flood(left, grid.cell(int32_t(i), int32_t(j)));
      left = left & ~piece;

      // Only a pocket that touches the spine. A piece that does not is not the
      // spine's doing - it is ground the stairwell cut off from the rest of the
      // floor - and swallowing it would leave the corridor in two halves with
      // no way between them.
      if (piece.count() >= 3)
      {
        continue;
      }

      bool touches = false;

      for (const uint32_t c : members(grid, piece))
      {
        touches = touches || adjoins(grid, spine, c);
      }

      if (touches)
      {
        spine |= piece;
      }
    }
  }


  // Corner filling and ring cutting each undo a little of the other - a filled
  // corner can close a cycle, and the cell cut out of a cycle can open a corner
  // - so they run until neither has anything left to do.
  const auto settle = [&](Board &candidate, const bool required) {
    bool done = false;

    for (uint32_t round = 0; round < 6 && !done; ++round)
    {
      if (!unpinch(grid, open, candidate))
      {
        return false;
      }

      if (grid.outline(candidate).size() == 1)
      {
        done = true;

        break;
      }

      if (!untangle(grid, candidate, anchors))
      {
        return false;
      }
    }

    if (!done || !grid.connected(candidate))
    {
      return false;
    }

    // And what it leaves has to be cuttable. This is the contract, not a
    // nicety: the subdivision cuts simple regions, so every piece of floor the
    // corridor leaves behind must be one, or the plan falls back to keeping a
    // whole wing as a single room. On a plain floor it is free; on a ringed one
    // it is the thing the cut into the court was made for, and checking it here
    // is what stops a spine that did not actually cut anything being accepted.
    {
      Board left = open & ~candidate;

      while (!left.empty())
      {
        uint32_t i = 0;
        uint32_t j = 0;
        left.first(i, j);

        const Board piece = grid.flood(left, grid.cell(int32_t(i), int32_t(j)));
        left = left & ~piece;

        const std::vector<Chain> edges = grid.outline(piece);

        if (edges.size() != 1 || !grid.simpleOutline(edges))
        {
          return false;
        }
      }
    }

    // A spine that has eaten half its storey is not circulation: better no
    // reservation and a subdivision that has to find its own corridors. The
    // exception is the cut across a ringed floor, which is not an improvement
    // to the plan but the thing that makes the plan cuttable at all.
    return required || candidate.count() * 2 < floor.area.count();
  };

  // Measured on the finished spine, not on the intent.
  //
  // Widening takes the second cell from whichever side has floor to give, and
  // where no side has any the corridor stays one cell - so a spine that asked
  // for two came out a metre wide down part of its length. `narrow` names
  // exactly those cells: any of them and this is not the corridor the class
  // asked for, so the floor goes without one and its rooms open onto each
  // other instead.
  const auto wideEnough = [&](const Board &candidate) {
    if (settings.corridorCells < 2 || candidate.empty() || house.frame.cellMetres >= settings.minimumCorridorMetres)
    {
      return true;
    }

    return geom2::narrow(grid, candidate).empty();
  };

  if (settle(spine, false) && wideEnough(spine))
  {
    return spine;
  }

  // The grown spine did not come out as a single simple run, or it took too much
  // of the floor. The mandatory part still has to be laid on a ringed floor -
  // without the cut the ring is not a region the subdivision can cut - so it is
  // tried on its own before anything is given up.
  Board fallback = mandatory;

  if (ringed && settle(fallback, true) && wideEnough(fallback))
  {
    ledger.add(LedgerKind::Repaired, 0, "a floor's circulation was cut back to the passage into its court");

    return fallback;
  }

  ledger.add(LedgerKind::Repaired, 0, "a floor's circulation could not be made a single simple run and was dropped");

  return Board();
}

// ---------------------------------------------------------------------------
// The floor stack
// ---------------------------------------------------------------------------

uint32_t randomFloorCount(const std::vector<std::string> &tags,
                          const uint32_t groundCells,
                          const uint32_t maximum,
                          Stream &stream)
{
  const bool low = std::find(tags.begin(), tags.end(), "low") != tags.end();
  const bool tall = std::find(tags.begin(), tags.end(), "tall") != tags.end();
  const double base = std::sqrt(double(groundCells)) - 1.;

  double count = 0;

  if (low)
  {
    count = base * std::abs(stream.bell4());
  }
  else if (tall)
  {
    count = base * (1. + .5 * std::abs(stream.bell4()));
  }
  else
  {
    count = base * stream.bell3();
  }

  const int32_t rounded = int32_t(std::lround(count));

  return uint32_t(std::clamp(rounded, 1, int32_t(maximum)));
}

namespace {

// Whether a room touches the floor's outer wall. A setback has to take ground
// off the OUTSIDE of the building: removing a room buried in the middle would
// punch a light well through it, which is a different thing entirely and not
// one the roof could take.
bool touchesOuterWall(const CellGrid &grid, const Chain &roomContour, const Chain &floorContour)
{
  for (const uint32_t e : roomContour)
  {
    if (std::find(floorContour.begin(), floorContour.end(), e) != floorContour.end())
    {
      return true;
    }
  }

  return false;
}

// A floor area that a building can actually stand on: connected, one loop, no
// pinch, and big enough to be a storey.
// A floor that can be built on: enough ground, in one piece, and with no more
// courts in it than the floor below had.
//
// The court count is the point of the third argument. A hole is either a
// courtyard the class asked for, in which case it continues up the building
// like any other part of the plan, or it is a ring a setback happened to leave
// behind - and that second one is a floor with a light well nobody designed,
// which is exactly the artefact this refuses.
// Whether a board is wide enough to hold a room of `span` cells anywhere: it
// contains a span-by-span square. A floor narrower than that is a strip, and
// its "rooms" are corridors nobody meant to draw.
bool holdsSquare(const CellGrid &grid, const Board &area, const uint32_t span)
{
  Board block = area;

  for (uint32_t step = 1; step < std::max(1u, span) && !block.empty(); ++step)
  {
    block = block & grid.shifted(block, geom2::Dir::East) & grid.shifted(block, geom2::Dir::South) & grid.shifted(grid.shifted(block, geom2::Dir::South), geom2::Dir::East);
  }

  return !block.empty();
}

bool usableFloor(const CellGrid &grid,
                 const Board &area,
                 const uint32_t minimumCells,
                 const size_t courtsAllowed,
                 std::vector<Chain> &loops,
                 const uint32_t minimumSpan = 1)
{
  if (area.count() < minimumCells || !grid.connected(area) || !holdsSquare(grid, area, minimumSpan))
  {
    return false;
  }

  loops = orderedLoops(grid, area);

  return !loops.empty() && loops.size() <= courtsAllowed + 1 && grid.simpleOutline(loops);
}

} // namespace

bool planFloorStack(CellHouse &house,
                    const CellDwellingBrief &brief,
                    const CellDwellingSettings &settings,
                    Stream &stream,
                    Ledger &ledger,
                    std::string *error)
{
  if (house.floors.empty())
  {
    return fail(error, "no ground floor to build on");
  }

  const bool slab = std::find(brief.tags.begin(), brief.tags.end(), "slab") != brief.tags.end();
  const SubdivisionSettings coarse = subdivisionFromTags(brief.tags, settings.coarseRoomCells);

  const uint32_t wanted = house.plannedFloors;

  for (uint32_t level = 1; level < wanted; ++level)
  {
    const CellFloor &below = house.floors.back();
    Board candidate = below.area;

    if (!slab)
    {
      std::vector<CellRoomShape> rooms;
      std::vector<Chain> walls;

      if (below.loops.size() == 1 &&
          subdivide(house.grid, below.loops.front(), coarse, stream, rooms, walls, nullptr) && rooms.size() > 1)
      {
        const CellRoomShape *best = nullptr;
        double bestScore = 0;

        for (const CellRoomShape &room : rooms)
        {
          if (!touchesOuterWall(house.grid, room.contour, below.loops.front()))
          {
            continue;
          }

          // Never the reserved stair, nor the cell it lets onto. Take either
          // away and the floors above it have no way up, which every other
          // invariant here would still pass.
          if (house.stairwell.has_value() &&
              (!(room.area & house.stairwell->block).empty() ||
               house.grid.contains(room.area, house.stairwell->landing)))
          {
            continue;
          }

          if (house.spiral.has_value() && house.grid.contains(room.area, house.spiral->landing))
          {
            continue;
          }

          const Board remainder = below.area & ~room.area;
          std::vector<Chain> loops;

          if (!usableFloor(house.grid, remainder, settings.minimumFloorCells, below.loops.size() - 1, loops, settings.minimumRoomCells))
          {
            continue;
          }

          const double score = geom2::compactness(remainder);

          if (score >= bestScore)
          {
            bestScore = score;
            best = &room;
          }
        }

        if (best != nullptr)
        {
          // Accepted more readily the more it would improve the floor, and
          // hardly at all when the floor below is already compact. A tower
          // keeps its shape; a sprawling ground floor nearly always sheds a
          // wing.
          const double already = geom2::compactness(below.area);
          const double chance = already > 0 ? bestScore * bestScore / already : 0.;

          if (stream.unit() < chance)
          {
            candidate = below.area & ~best->area;
          }
        }
      }
    }

    // NOT trimmed here, though the setback leaves nubs too.
    //
    // Tried, and it breaks three things at once: `slab` promises every floor is
    // the same shape, an explicit floor count stops being honourable when a
    // storey shrinks under the minimum, and the turret's landing can be trimmed
    // out from under it. A storey is a subset of the one below by contract, and
    // quietly shaving it is not a subset operation the rest of the planner was
    // told about.
    std::vector<Chain> loops;

    if (!usableFloor(house.grid, candidate, settings.minimumFloorCells, below.loops.size() - 1, loops, settings.minimumRoomCells))
    {
      ledger.add(LedgerKind::Capacity, PatchId(brief.id), "floor stack stopped short: no usable ground above");
      break;
    }

    // The reservation has to work on THIS floor too, not only on the ground.
    //
    // A cell that sits in a nook of the ground floor can sit in the middle of
    // the floor above it once a setback has taken the wing beside it away, and
    // then the remainder is a ring. Checked per floor rather than assumed from
    // the ground, and a floor that cannot take the reservation is where the
    // stack stops.
    if (house.stairwell.has_value() && !stairwellFits(house.grid, candidate, house.stairwell->block, below.loops.size() - 1))
    {
      ledger.add(LedgerKind::Capacity, PatchId(brief.id), "floor stack stopped short: the reserved stair would not fit above");
      break;
    }

    CellFloor floor;
    floor.level = int32_t(level);
    floor.area = candidate;
    floor.loops = std::move(loops);
    house.floors.push_back(std::move(floor));
  }

  // The tower carries on past the top floor.
  //
  // Which is what makes it a tower rather than a wide stairwell: a castle's
  // stair is the thing you see over the wall, and the climb keeps going after
  // the mass it serves has stopped and comes out under its own roof.
  //
  // Nothing new is needed to say that. A storey ABOVE is already a storey whose
  // area is a subset of the one below, so a tower storey is a floor whose area
  // is the tower block - and every rule downstream then does the right thing
  // without being told: the storeys stack, the setback invariant holds because
  // the block is inside every floor, the top floor's roof is cut around the
  // tower because a roof covers what the storey above does not, and the tower
  // gets its own roof because it is the top of something.
  if (house.stairwell.has_value() && settings.stairTowerAbove > 0 && house.stairwell->block.count() >= 4 &&
      house.floors.size() >= 2)
  {
    for (uint32_t extra = 0; extra < settings.stairTowerAbove; ++extra)
    {
      const CellFloor &below = house.floors.back();
      const Board candidate = house.stairwell->block;

      std::vector<Chain> loops;

      if (!usableFloor(house.grid, candidate, settings.minimumFloorCells, 0, loops))
      {
        break;
      }

      CellFloor floor;
      floor.level = below.level + 1;
      floor.area = candidate;
      floor.loops = std::move(loops);
      house.floors.push_back(std::move(floor));
    }
  }

  // The basement, from a coarse partition of the ground floor: its footprint is
  // the largest room of that pass, which is why it always lands on a plausible
  // part of the house rather than under all of it.
  const bool forced = std::find(brief.tags.begin(), brief.tags.end(), "basement") != brief.tags.end();
  const double chance = double(house.floors.size()) / double(house.floors.size() + 1);

  const bool wantsCellar = forced || stream.unit() < chance;

  if (settings.cellars && wantsCellar)
  {
    std::vector<CellRoomShape> rooms;
    std::vector<Chain> walls;

    if (house.floors.front().loops.size() == 1 &&
        subdivide(house.grid, house.floors.front().loops.front(), coarse, stream, rooms, walls, nullptr))
    {
      const CellRoomShape *biggest = nullptr;

      for (const CellRoomShape &room : rooms)
      {
        if (biggest == nullptr || room.area.count() > biggest->area.count())
        {
          biggest = &room;
        }
      }

      std::vector<Chain> loops;

      if (biggest != nullptr && usableFloor(house.grid, biggest->area, settings.minimumFloorCells, 0, loops))
      {
        CellFloor cellar;
        cellar.level = -1;
        cellar.area = biggest->area;
        cellar.loops = std::move(loops);
        house.basement = std::move(cellar);
      }
      else
      {
        ledger.add(LedgerKind::Omitted, PatchId(brief.id), "no room under the house big enough for a cellar");
      }
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// The rooms
// ---------------------------------------------------------------------------

namespace {

// The corner towers, as rooms that keep going up.
//
// A castle has towers at the angles of its walls, and they are not extra
// storeys: they are the rooms in the corners of the top floor, carried two
// storeys past the eaves. Saying it that way means nothing new has to be built
// - the wall round such a room is already as tall as the room, the main roof
// already dies into anything taller than itself as a gable, and the tower gets
// its own roof at its own height because the roof is built band by band.
//
// A corner is a cell of the top floor with two of its four neighbours outside
// it: the angle of the plan. A room holding one, and standing on the outside
// wall, is a room a tower can be.
void raiseTowers(CellHouse &house, const CellDwellingSettings &settings)
{
  if (settings.cornerTowers == 0 || house.floors.empty())
  {
    return;
  }

  // Never on an axial plan. A corner room of a cross is a TRANSEPT, and raising
  // the transepts two storeys above the nave stands a church on its head - the
  // long arm came out low and the short arms came out as towers, which is what
  // it looked like. A church's tower goes over the crossing and is built there.
  if (house.gridClass.footprint == FootprintKind::Axial)
  {
    return;
  }

  // The highest ORDINARY storey, which on a castle is not the top one: the
  // stair tower already stands two or three floors above it and is a tower in
  // its own right. Raising that would stack one tower on another; the corner
  // towers belong on the mass they stand at the angles of.
  size_t top = house.floors.size();

  for (size_t k = house.floors.size(); k-- > 0;)
  {
    if (house.stairwell.has_value() && house.floors[k].area == house.stairwell->block)
    {
      continue;
    }

    top = k;

    break;
  }

  if (top >= house.floors.size())
  {
    return;
  }

  CellFloor &floor = house.floors[top];

  std::vector<uint32_t> corners;

  for (uint32_t r = 0; r < uint32_t(floor.roomAreas.size()); ++r)
  {
    if (r < floor.roomOutdoor.size() && floor.roomOutdoor[r] != 0)
    {
      continue;
    }

    if (!floor.circulation.empty() && floor.roomAreas[r] == floor.circulation)
    {
      continue;
    }

    if (r < floor.roomHeight.size() && floor.roomHeight[r] > 0)
    {
      continue;
    }

    bool onACorner = false;

    for (const auto &[i, j] : geom2::cells(floor.roomAreas[r]))
    {
      const uint32_t c = house.grid.cell(int32_t(i), int32_t(j));
      uint32_t outside = 0;

      for (const Dir d : geom2::kCardinal)
      {
        const uint32_t n = house.grid.neighbour(c, d);

        outside += n == geom2::kNoCell || !house.grid.contains(floor.area, n) ? 1u : 0u;
      }

      onACorner = onACorner || outside >= 2;
    }

    if (onACorner)
    {
      corners.push_back(r);
    }
  }

  // The biggest corner rooms, so a tower is a tower and not a turret over a
  // cupboard.
  std::sort(corners.begin(), corners.end(), [&](const uint32_t a, const uint32_t b) {
    return floor.roomAreas[a].count() > floor.roomAreas[b].count();
  });

  const double height = house.gridClass.storeyMetres * (1. + settings.cornerTowerStoreys);

  floor.roomHeight.resize(floor.roomAreas.size(), 0.);

  Board raised;
  uint32_t built = 0;

  for (uint32_t k = 0; k < corners.size() && built < settings.cornerTowers; ++k)
  {
    // A tower may not break the roof of the mass it stands on.
    //
    // Raising a room takes it out of the band below, and what is left has to
    // stay a region a roof can be built on - connected, and not pinched at a
    // corner. Two towers at the ends of the same range can leave the range
    // between them joined to the rest by a single corner, which is four-
    // connected and not a shape a roof surface can be laid over: measured, one
    // such piece of a hundred and fifty cells was being dropped whole, and the
    // top of the castle simply had no roof over it.
    const Board trial = raised | floor.roomAreas[corners[k]];
    const Board rest = floor.area & ~trial;

    if (rest.empty() || !house.grid.connected(rest))
    {
      continue;
    }

    const std::vector<Chain> loops = house.grid.outline(rest);

    if (loops.empty() || !house.grid.simpleOutline(loops))
    {
      continue;
    }

    raised = trial;
    floor.roomHeight[corners[k]] = height;
    ++built;
  }
}

} // namespace

bool planRooms(CellHouse &house,
               const CellDwellingSettings &settings,
               Stream &stream,
               Ledger &ledger,
               std::string *error)
{
  SubdivisionSettings rule = subdivisionFromTags(house.tags, settings.averageRoomCells);
  rule.minimumRoomCells = settings.minimumRoomCells;

  std::vector<CellFloor *> all;

  for (CellFloor &floor : house.floors)
  {
    all.push_back(&floor);
  }

  if (house.basement.has_value())
  {
    all.push_back(&house.basement.value());
  }

  for (CellFloor *floor : all)
  {
    floor->roomAreas.clear();
    floor->roomContours.clear();
    floor->roomOutdoor.clear();
    floor->roomFixed.clear();
    floor->roomHeight.clear();
    floor->innerWalls.clear();
    floor->circulation = floor->level==0 && !house.coveredPassage.empty() ? Board{} : reserveCirculation(house, *floor, settings, ledger);

    Board remainder = floor->area;
    if(floor->level==0 && !house.coveredPassage.empty()) {
      floor->roomAreas.push_back(house.coveredPassage);
      floor->roomContours.push_back(house.grid.outerLoop(house.coveredPassage));
      floor->roomOutdoor.push_back(0);floor->roomFixed.push_back(1);floor->roomHeight.push_back(0);
      remainder=remainder & ~house.coveredPassage;
    }

    // The courts first, so they are rooms before any door is drawn. Only on the
    // ground: they were cut out of the footprint, so no floor above has them.
    if (floor->level == 0 && !house.courts.empty())
    {
      // A pen beside the yard is its own room: the fence between them is the
      // point of it. Flooded within the pens and within the rest separately.
      Board left = house.courts;

      while (!left.empty())
      {
        uint32_t i = 0;
        uint32_t j = 0;
        left.first(i, j);

        const uint32_t seed = house.grid.cell(int32_t(i), int32_t(j));
        const Board within = house.grid.contains(house.pens, seed) ? (left & house.pens) : (left & ~house.pens);
        const Board piece = house.grid.flood(within, seed);
        left = left & ~piece;

        floor->roomAreas.push_back(piece);
        floor->roomContours.push_back(house.grid.outerLoop(piece));
        floor->roomOutdoor.push_back(1);
        floor->roomFixed.push_back(0);
        floor->roomHeight.push_back(0);
      }
    }

    // The reservation, honoured before anything is cut.
    //
    // The reserved stair becomes its own room and the rest of the floor is
    // subdivided around it. That is what makes the stairwell scheme work: the
    // same ground is a room on every floor, in the same place, so a flight can
    // run straight up through all of them. At castle scale that ground is a
    // block rather than a cell and nothing else here changes - the tower is a
    // room like the stairwell was.
    if (house.stairwell.has_value() && !(remainder & house.stairwell->block).empty())
    {
      const Board one = remainder & house.stairwell->block;
      floor->roomAreas.push_back(one);
      floor->roomContours.push_back(house.grid.outerLoop(one));
      floor->roomOutdoor.push_back(0);
      floor->roomFixed.push_back(0);
      floor->roomHeight.push_back(0);
      remainder = remainder & ~one;
    }

    // Circulation, where a floor was large enough to have any reserved.
    //
    // The spine is a room, and it has to be: the rooms partition the floor, a
    // door is drawn between two rooms, and a corridor nothing can have a door
    // onto is a corridor nobody can walk.
    if (!floor->circulation.empty())
    {
      remainder = remainder & ~floor->circulation;

      floor->roomAreas.push_back(floor->circulation);
      floor->roomContours.push_back(house.grid.outerLoop(floor->circulation));
      floor->roomOutdoor.push_back(0);
      floor->roomFixed.push_back(0);
      floor->roomHeight.push_back(0);
    }

    if (remainder.empty())
    {
      continue;
    }

    // What is left may have come apart into pieces; each is cut on its own.
    Board left = remainder;

    while (!left.empty())
    {
      uint32_t i = 0;
      uint32_t j = 0;
      left.first(i, j);
      Board within=left;
      if(house.architecture==rewrite::DwellingArchitecture::Roman && !house.courts.empty()) {
        // Four inhabited ranges around the atrium. A holed region cannot be
        // passed to the simple-contour room cutter as one giant room.
        const auto court=geom2::bounds(house.courts);
        const auto wing=[&](int x,int z){return x<int(court.i0)?0:x>int(court.i1)?1:z<int(court.j0)?2:3;};
        const int selected=wing(i,j);within=Board{};
        for(auto [x,z]:geom2::cells(left))if(wing(x,z)==selected)house.grid.add(within,house.grid.cell(x,z));
      }
      const Board piece = house.grid.flood(within, house.grid.cell(int32_t(i), int32_t(j)));
      left = left & ~piece;

      const std::vector<Chain> loops = house.grid.outline(piece);

      if (loops.size() != 1 || !house.grid.simpleOutline(loops))
      {
        // A piece with a hole in it, or one that pinches at a corner, is not a
        // contour the subdivision can cut. It becomes a single room, and it is
        // RECORDED: this branch firing means something upstream reserved ground
        // it should not have, and a silent one-room floor is exactly the kind
        // of wrong answer that looks fine in a render.
        ledger.add(LedgerKind::Repaired, 0, "a floor piece was not a simple region and was kept whole");
        floor->roomAreas.push_back(piece);
        floor->roomContours.push_back(loops.empty() ? Chain{} : loops.front());
        floor->roomOutdoor.push_back(0);
        floor->roomFixed.push_back(0);
        floor->roomHeight.push_back(0);
        continue;
      }

      // An axial building is not subdivided. A nave is one space the length of
      // the building and a chancel is one space at the end of it, and cutting
      // them into parlours is the failure this kind exists to prevent - before
      // this, a church came out with a kitchen, two bedrooms and a library in
      // it.
      if (house.gridClass.footprint == FootprintKind::Axial)
      {
        std::vector<Board> arms;
        std::vector<rewrite::RoomKind> named;

        // One tower at the back or two at the front, drawn once per building
        // and the same on every storey the partition runs on.
        const BellTowers towers = house.twinTowers ? BellTowers::TwinFront : BellTowers::Back;

        if (partitionAxial(house.grid, piece, house.entranceLanding, arms, named, towers, &house.westFrontSlices))
        {
          // You come in at the nave.
          //
          // The door was picked on the footprint, before any room existed, by
          // the middle of a straight street-facing wall - and on a west front
          // the straightest street-facing walls are the towers' own. So once
          // the nave is known, a door that opens into anything else is moved
          // to the nave's street wall, nearest the middle of it. Nothing
          // downstream has read the old one yet: doors, windows and the
          // realised opening all come after this.
          if (floor->level == 0)
          {
            for (size_t k = 0; k < arms.size(); ++k)
            {
              if (named[k] != rewrite::RoomKind::Nave || house.grid.contains(arms[k], house.entranceLanding))
              {
                continue;
              }

              double bestReach = 1e300;
              uint32_t bestEdge = geom2::kNoEdge;

              double middleI = 0, middleJ = 0;
              uint32_t count = 0;

              for (const auto &[i, j] : geom2::cells(arms[k]))
              {
                middleI += double(i);
                middleJ += double(j);
                ++count;
              }

              middleI /= double(std::max(1u, count));
              middleJ /= double(std::max(1u, count));

              for (const uint32_t e : house.grid.outerLoop(arms[k]))
              {
                const uint32_t twin = house.grid.twin(e);
                const uint32_t own = house.grid.edgeCell(e);
                const uint32_t beyond = twin == geom2::kNoEdge ? geom2::kNoCell : house.grid.edgeCell(twin);

                if (own == geom2::kNoCell || (beyond != geom2::kNoCell && house.grid.contains(floor->area, beyond)))
                {
                  continue;
                }

                // Facing the street.
                if (!facesStreet(house.grid, e, house.streetJ, house.streetI))
                {
                  continue;
                }

                const double reach = std::hypot(double(house.grid.cellRow(own)) - middleI, double(house.grid.cellColumn(own)) - middleJ);

                if (reach < bestReach)
                {
                  bestReach = reach;
                  bestEdge = e;
                }
              }

              if (bestEdge != geom2::kNoEdge)
              {
                house.entrance = bestEdge;
                house.entranceLanding = house.grid.edgeCell(bestEdge);
                ledger.add(LedgerKind::Repaired, 0, "the door was moved to the nave's street wall");
              }
            }
          }

          for (size_t k = 0; k < arms.size(); ++k)
          {
            // An unnamed piece is the subdivision's to cut, at a small room's
            // size: a transept holds a vestry and a chapel, not a hall.
            if (named[k] == rewrite::RoomKind::Count)
            {
              SubdivisionSettings small = rule;
              small.averageRoomCells = std::max(4., rule.averageRoomCells / 3.);

              std::vector<CellRoomShape> cut;
              std::vector<Chain> cutWalls;
              std::string why;

              if (!subdivide(house.grid, house.grid.outerLoop(arms[k]), small, stream, cut, cutWalls, &why) || cut.empty())
              {
                CellRoomShape whole;
                whole.area = arms[k];
                whole.contour = house.grid.outerLoop(arms[k]);
                cut = {whole};
              }

              for (CellRoomShape &room : cut)
              {
                floor->roomAreas.push_back(room.area);
                floor->roomContours.push_back(std::move(room.contour));
                floor->roomOutdoor.push_back(0);
                floor->roomFixed.push_back(0);
                floor->roomHeight.push_back(0);
                floor->roomKinds.resize(floor->roomAreas.size(), rewrite::RoomKind::Count);
              }

              for (Chain &wall : cutWalls)
              {
                floor->innerWalls.push_back(std::move(wall));
              }

              continue;
            }

            floor->roomAreas.push_back(arms[k]);
            floor->roomContours.push_back(house.grid.outerLoop(arms[k]));
            floor->roomOutdoor.push_back(0);
            floor->roomFixed.push_back(1);

            // The nave and the chancel are the tall ones, and the transepts
            // beside them are not. That is what a cruciform church looks like
            // and it is the reason a room carries its own height at all: one
            // storey with two heights in it, rather than two storeys.
            // The chancel stands taller than the nave, and the nave taller than
            // the transepts beside it. Three heights in one storey, which is
            // what a cross section of a church actually is.
            // Above ground only. The partition runs on every storey a floor
            // stack has, the cellar included, and a cellar has no nave in it -
            // but it was being given one, and a belfry with it: fourteen metres
            // of tower starting three and a quarter metres BELOW the ground,
            // which came up through the church's own roofs. Measured, that was
            // every wall in the building that clipped through something.
            const double storeys = floor->level < 0                          ? 0.
                                   : named[k] == rewrite::RoomKind::Belfry   ? settings.tallRoomStoreys * 2.2
                                   : named[k] == rewrite::RoomKind::Chancel  ? settings.tallRoomStoreys * 1.2
                                   : named[k] == rewrite::RoomKind::Nave     ? settings.tallRoomStoreys
                                                                            : 0.;

            const double metres = storeys > 0 ? house.gridClass.storeyMetres * storeys : 0.;

            floor->roomHeight.push_back(metres);
            floor->roomKinds.resize(floor->roomAreas.size(), rewrite::RoomKind::Count);
            floor->roomKinds.back() = named[k];

          }

          // The tower clears the ridges around it.
          //
          // A bell tower is not a tall room, it is the TALLEST thing here, and
          // a fixed multiple of the storey does not guarantee that: the nave is
          // roofed in proportion to its own height, so a steeper pitch raised
          // its apex past the belfry's wall head and swallowed the tower whole
          // - the churches came out with no tower at all and nobody had touched
          // the tower. What the shaft has to reach is settled from what it
          // stands next to: the highest apex around it, plus the stage where
          // the bells hang and a course of wall over that.
          if (floor->level >= 0)
          {
            std::vector<uint32_t> belfries;
            double highest = 0;

            for (uint32_t k = 0; k < uint32_t(floor->roomAreas.size()); ++k)
            {
              if (floor->roomKinds[k] == rewrite::RoomKind::Belfry)
              {
                belfries.push_back(k);
                continue;
              }

              const double own =
                floor->roomHeight[k] > 0 ? floor->roomHeight[k] : house.gridClass.storeyMetres;

              // Where that room's own roof peaks: half its narrow way across,
              // at the pitch its height earns it. The same rule the roof
              // builder uses, asked here so the two agree.
              uint32_t leastI = UINT32_MAX, mostI = 0, leastJ = UINT32_MAX, mostJ = 0;

              for (const auto &[ri, rj] : geom2::cells(floor->roomAreas[k]))
              {
                leastI = std::min(leastI, ri);
                mostI = std::max(mostI, ri);
                leastJ = std::min(leastJ, rj);
                mostJ = std::max(mostJ, rj);
              }

              if (leastI == UINT32_MAX)
              {
                continue;
              }

              const double across =
                double(std::min(mostI - leastI, mostJ - leastJ) + 1) * house.frame.cellMetres;
              const double pitch = std::min(settings.roofPitchCeiling,
                                            settings.roofPitch * std::max(1., own / house.gridClass.storeyMetres));

              highest = std::max(highest, own + across * .5 * pitch);
            }

            // The square shaft stops one storey above everything around it,
            // and that is where the deck goes. Not an arbitrary margin: it is
            // the height at which the tower has cleared the ridges beside it
            // and the stage above can be seen from all six ways at once. Two
            // towers get the same deck, because they clear the same ridges.
            const bool ladder = stream.unit() < settings.towerLadderChance;

            for (const uint32_t belfry : belfries)
            {
              const double deck = std::max(floor->roomHeight[belfry], highest + house.gridClass.storeyMetres);

              floor->roomHeight[belfry] = deck;

              CellBellTower tower;
              tower.cells = floor->roomAreas[belfry];
              tower.deckMetres = deck;
              tower.topMetres = deck + settings.bellStageMetres + settings.bellStageHeadMetres;
              tower.ladder = ladder;

              // The circle the drum stands on, settled here so the deck, the
              // hole through it and the drum itself cannot disagree. Inscribed
              // in the shaft rather than around it: a drum wider than the mass
              // under it is a drum with nothing holding it up.
              double sumX = 0;
              double sumZ = 0;
              uint32_t count = 0;
              double leastX = 1e300, mostX = -1e300, leastZ = 1e300, mostZ = -1e300;

              for (const auto &[ti, tj] : geom2::cells(tower.cells))
              {
                double x = 0;
                double z = 0;
                geom2::cellPoint(house.grid, house.frame, house.grid.cell(int32_t(ti), int32_t(tj)), x, z);

                sumX += x;
                sumZ += z;
                ++count;
                // Measure the shaft in its lattice frame, not the world AABB:
                // rotating a square must not enlarge its bell chamber.
                const double u=x*house.frame.axisX+z*house.frame.axisZ;
                const double v=-x*house.frame.axisZ+z*house.frame.axisX;
                leastX = std::min(leastX, u);
                mostX = std::max(mostX, u);
                leastZ = std::min(leastZ, v);
                mostZ = std::max(mostZ, v);
              }

              if (count == 0)
              {
                continue;
              }

              const double side = std::min(mostX - leastX, mostZ - leastZ) + house.frame.cellMetres;

              tower.x = sumX / double(count);
              tower.z = sumZ / double(count);
              tower.radius = std::max(.8, side * .5 - settings.outerWallMetres * .5);
              house.bellTowers.push_back(std::move(tower));
            }
          }

          continue;
        }

        ledger.add(LedgerKind::Repaired, 0, "an axial plan did not read as a cross and was cut the ordinary way");
      }

      std::vector<CellRoomShape> rooms;
      std::vector<Chain> walls;

      if (!subdivide(house.grid, loops.front(), rule, stream, rooms, walls, error))
      {
        ledger.add(LedgerKind::Capacity, 0, "a floor could not be cut into rooms");

        return false;
      }

      for (CellRoomShape &room : rooms)
      {
        floor->roomAreas.push_back(room.area);
        floor->roomContours.push_back(std::move(room.contour));
        floor->roomOutdoor.push_back(0);
        floor->roomFixed.push_back(0);
        floor->roomHeight.push_back(0);
      }

      for (Chain &wall : walls)
      {
        floor->innerWalls.push_back(std::move(wall));
      }
    }
  }

  raiseTowers(house, settings);

  return true;
}

std::array<double, 6> &planningPhaseMs()
{
  static std::array<double, 6> totals{};

  return totals;
}

bool planCellDwelling(const CellDwellingBrief &inputBrief,
                      const CellDwellingSettings &settings,
                      CellHouse &house,
                      Ledger &ledger,
                      std::string *error)
{
  // Entrance reservation is part of dwelling planning, including standalone
  // previews. Roof, columns, landing and stair flight must share this frontage.
  CellDwellingBrief brief=inputBrief;
  const bool temple=std::find(brief.tags.begin(),brief.tags.end(),"roman-temple")!=brief.tags.end();
  if(temple || (brief.architecture==rewrite::DwellingArchitecture::Roman && brief.party.empty() && geom2::areaMetres(brief.lot)>200))
    brief.frontageSetbackMetres=std::max(brief.frontageSetbackMetres,5.2);
  Stream stream(brief.seed);

  // Where a plan's time goes, by phase; `DWELLING_TIMING=1` reads it back
  // through `planningPhaseMs`.
  const bool timing = std::getenv("DWELLING_TIMING") != nullptr;
  const auto mark = [] { return std::chrono::steady_clock::now(); };
  const auto ms = [](const std::chrono::steady_clock::time_point &from) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count();
  };
  auto phase = mark();

  // The class first, because everything below is measured on the board it
  // chooses. A class owns the grid tier, the cell, the size band, the shape and
  // the room size; the caller keeps the tolerances, the stair geometry, the
  // glazing and the jetty envelope, which are properties of the street rather
  // than of what kind of building stands on it.
  CellDwellingSettings tuned = classed(settings, brief.building, stream);
  const ClassProfile &profile = profileFor(brief.building);

  if (brief.grid.has_value())
  {
    tuned.grid = *brief.grid;
  }

  // Castle class defaults describe a sprawling compound. On a small parcel
  // use a solid keep instead; retain the caller's explicit grid and capacity.
  const bool compactCastle = brief.building == BuildingClass::Castle &&
    std::abs(geom2::areaMetres(brief.lot)) < 900.;
  if (compactCastle && !brief.grid && !brief.capacity)
  {
    tuned.grid.footprint = FootprintKind::Box;
    tuned.grid.minimumCells = 16;
    // Leave a projection strip for corbels/turrets rather than occupying all
    // buildable cells with the keep itself. The fit still validates the lot.
    const double span = std::max(0.,std::sqrt(std::abs(geom2::areaMetres(brief.lot)))-
                                   2*brief.setbackMetres-4.);
    tuned.grid.maximumCells = std::clamp(uint32_t(span*span/(tuned.grid.cellMetres*tuned.grid.cellMetres)),16u,80u);
    tuned.grid.fitToLot = false;
    tuned.courts = tuned.yards = tuned.pens = tuned.sheds = 0;
  }

  if (brief.capacity)
  {
    const auto &budget = *brief.capacity;
    if (!std::isfinite(budget.preferredFootprintMetres2) || !std::isfinite(budget.roomAreaMetres2) ||
        budget.preferredFootprintMetres2 < 4 || budget.preferredFootprintMetres2 > 4096 ||
        budget.desiredRooms == 0 || budget.desiredRooms > 128 || budget.roomAreaMetres2 < 4 ||
        budget.minimumSpanCells<2 || budget.minimumSpanCells>16 || budget.minimumFloors<1 ||
        budget.minimumFloors>128 || budget.roomAreaMetres2>4096 ||
        !std::isfinite(budget.maximumAspectRatio) || (budget.maximumAspectRatio!=0 && budget.maximumAspectRatio<1) ||
        !std::isfinite(budget.compactnessPenalty) || budget.compactnessPenalty<0 || budget.compactnessPenalty>1)
      return fail(error, "invalid dwelling capacity budget");
    // District policy supersedes stylistic class limits. Zero removes the
    // policy cap; finite room demand still bounds this individual build.
    tuned.maximumFloors=settings.maximumFloors ? settings.maximumFloors :
      std::max({profile.minimumStoreys,brief.floors,budget.minimumFloors,
                uint32_t(std::ceil(budget.desiredRooms*budget.roomAreaMetres2/4.))});
    // Metre cells resolve small urban plots without shrinking doors, stairs,
    // or storey heights: what is human-sized is written in metres and turned
    // into a whole number of cells below - a corridor is as many cells as 1.4
    // metres needs, a stair well as many as its flight needs across.
    if (!std::isfinite(settings.capacityCellMetres) || settings.capacityCellMetres<1. || settings.capacityCellMetres>4.)
      return fail(error,"capacity cell pitch must be between one and four metres");
    tuned.grid.cellMetres = settings.capacityCellMetres;
    // Fit whole modules to a frontage. Leftover width is a setback or party
    // infill; it must never rescale every doorway and room on the street.
    tuned.grid.fitToLot = budget.fillLot;
    if (budget.fillLot) tuned.grid.rows=Board::kMaxRows;
    tuned.grid.minimumSpanCells=budget.minimumSpanCells;
    tuned.grid.maximumAspectRatio=budget.maximumAspectRatio;
    tuned.grid.compactnessPenalty=budget.compactnessPenalty;
    tuned.grid.minimumCells = budget.minimumSpanCells*budget.minimumSpanCells;
    const double cellArea=tuned.grid.cellMetres*tuned.grid.cellMetres;
    tuned.grid.maximumCells = uint32_t(std::floor(std::min(budget.preferredFootprintMetres2,
                                                       std::abs(geom2::areaMetres(brief.lot)))/cellArea+1e-6));
    if (tuned.grid.maximumCells < tuned.grid.minimumCells)
      return fail(error, "lot smaller than minimum dwelling footprint");
    if(profile.grid.footprint!=FootprintKind::Axial) tuned.grid.footprint = FootprintKind::Box;
    tuned.averageRoomCells = budget.roomAreaMetres2/cellArea;
    tuned.coarseRoomCells = budget.roomAreaMetres2 * 2/cellArea;
    tuned.minimumRoomCells = 2;
    tuned.courts = tuned.yards = tuned.pens = tuned.sheds = 0;
    tuned.cornerTowers = tuned.stairTowerAbove = 0;
    tuned.cellars = false;
  }

  if(brief.sharedGrid) {
    tuned.grid.cellMetres=brief.sharedGrid->cellMetres;
    if(!std::isfinite(tuned.grid.cellMetres) || tuned.grid.cellMetres<.5)return fail(error,"invalid shared grid cell size");
    if(brief.capacity) {
      const double area=tuned.grid.cellMetres*tuned.grid.cellMetres;
      tuned.grid.maximumCells=uint32_t(std::floor(std::min(brief.capacity->preferredFootprintMetres2,std::abs(geom2::areaMetres(brief.lot)))/area+1e-6));
      tuned.averageRoomCells=brief.capacity->roomAreaMetres2/area;tuned.coarseRoomCells=2*tuned.averageRoomCells;
    }
  }

  tuneDwellingStyle(brief,tuned);

  // Human dimensions, converted against the cell this build actually got.
  //
  // The class profiles count CELLS, and a city house plans on a one metre
  // cell: `corridorCells = 1` is a three metre gallery on a castle's cell and
  // a one metre squeeze on a city lot, and `minimumRoomCells = 2` is a two
  // metre room. What is actually being said - how wide you walk, how small a
  // room stops being a room - is metres, so it is written in metres and
  // converted here. Small lots simplify their programme; they do not shrink these minima.
  {
    const double cell = std::max(.25, tuned.grid.cellMetres);
    const auto cellsFor = [&](const double metres) { return uint32_t(std::ceil(metres / cell - 1e-9)); };

    tuned.corridorCells = std::max(tuned.corridorCells, cellsFor(tuned.minimumCorridorMetres+tuned.innerWallMetres));
    tuned.minimumRoomCells = std::max(tuned.minimumRoomCells, cellsFor(tuned.minimumRoomSpanMetres));

  }

  phase = mark();

  if (!shell(brief, tuned, house, stream, ledger, error))
  {
    return false;
  }

  if (timing) planningPhaseMs()[0] += ms(phase);

  // How tall, then how it is climbed, then the storeys, then the rooms. The
  // scheme has to be settled before the stack, because the setback may not take
  // the reserved stair away, and before the rooms, because the reservation is
  // an input to how they are cut.
  house.plannedFloors = brief.floors > 0
                          ? std::min(brief.floors, tuned.maximumFloors)
                          : randomFloorCount(brief.tags, house.floors.front().area.count(), tuned.maximumFloors, stream);

  if (compactCastle && brief.floors == 0 && !brief.capacity)
    house.plannedFloors = std::min(tuned.maximumFloors, std::max(4u, house.plannedFloors));

  if (brief.capacity && brief.floors == 0)
  {
    const double footprint = house.floors.front().area.count() * tuned.grid.cellMetres * tuned.grid.cellMetres;
    const double requestedArea = brief.capacity->desiredRooms * brief.capacity->roomAreaMetres2;
    house.plannedFloors = std::clamp(uint32_t(std::ceil(requestedArea / footprint)), 1u, tuned.maximumFloors);
  }

  // A chapel is one storey and a manor is never one. The floor in the class
  // profile is a floor: it is raised to, not negotiated with, and only the
  // ceiling the caller set can pull it back down.
  house.plannedFloors = std::min(std::max(house.plannedFloors, profile.minimumStoreys), tuned.maximumFloors);
  if (brief.capacity)
    house.plannedFloors=std::min(tuned.maximumFloors,std::max(house.plannedFloors,brief.capacity->minimumFloors));

  // The climb, the storeys, the rooms, the doors and the flights, together.
  //
  // Together because they are one decision: how a house is climbed changes how
  // its floors are cut, and whether a flight fits is only known once they are.
  // A per-floor stair is found AFTER the rooms exist and in a small house
  // there is often nowhere left for one, which used to end in a ladder nobody
  // drew and now ends in a storey being dropped. So when that happens the
  // house is planned again with the stair RESERVED before the rooms are cut,
  // which is the whole reason Watabou's stairwell scheme exists; only if that
  // also fails does the storey go.
  house.architecture=brief.architecture;house.architecturalAgeYears=brief.architecturalAgeYears;house.architecturalLot=brief.lot;
  const CellHouse shelled = house;
  const uint32_t wanted = house.plannedFloors;

  for (unsigned attempt = 0; attempt < 2; ++attempt)
  {
    std::vector<std::string> tags = brief.tags;

    if (attempt == 1)
    {
      house = shelled;
      house.plannedFloors = wanted;
      tags.push_back("stairwell");
    }

    if (!rollStairs(house, tags, tuned, stream))
    {
      return false;
    }

    phase = mark();

    if (!planFloorStack(house, brief, tuned, stream, ledger, error))
    {
      return false;
    }

    if (timing) planningPhaseMs()[1] += ms(phase);

    if(brief.coveredPassage && !reserveCoveredPassage(*brief.coveredPassage,house,error))return false;

    phase = mark();

    if (!planRooms(house, tuned, stream, ledger, error))
    {
      return false;
    }

    if (timing) planningPhaseMs()[2] += ms(phase);

    phase = mark();

    if (!planDoors(house, tuned, stream, ledger, error))
    {
      return false;
    }

    if (timing) planningPhaseMs()[3] += ms(phase);

    phase = mark();

    if (!planStairs(house, tuned, stream, ledger, error))
    {
      return false;
    }

    if (timing) planningPhaseMs()[4] += ms(phase);

    // Every storey the ground allowed is there and reachable, or the scheme
    // that reserves its stair has already been tried.
    if (house.floors.size() >= std::min<size_t>(wanted, shelled.plannedFloors) || house.scheme == StairScheme::Stairwell || wanted <= 1)
    {
      break;
    }
  }
  if(brief.coveredPassage && !reserveCoveredPassage(*brief.coveredPassage,house,error))return false;

  if (!assignProgramme(house, stream, ledger, error))
  {
    return false;
  }

  if(std::find(brief.tags.begin(),brief.tags.end(),"roman-temple")!=brief.tags.end())
    for(auto &floor:house.floors)for(size_t i=0;i<floor.roomKinds.size();++i)
      if((i>=floor.roomOutdoor.size()||!floor.roomOutdoor[i])&&floor.roomKinds[i]!=rewrite::RoomKind::Staircase)
        floor.roomKinds[i]=rewrite::RoomKind::Sanctuary;

  // A monastery has a dedicated side chapel off the nave. Keep the axial
  // room geometry and its doors; purpose must not flatten the church to a hall.
  if(std::find(brief.tags.begin(),brief.tags.end(),"monastery-church")!=brief.tags.end())
    for(auto &floor:house.floors) {
      auto chapel=std::find(floor.roomKinds.begin(),floor.roomKinds.end(),rewrite::RoomKind::Transept);
      if(chapel!=floor.roomKinds.end()) *chapel=rewrite::RoomKind::SideChapel;
    }

  phase = mark();
  if(!planOpenings(house, tuned, stream, ledger, error)) return false;
  if (timing) planningPhaseMs()[5] += ms(phase);
  if(house.coveredPassage.empty())planCellDwellingSite(brief,tuned,house);
  house.architecture=brief.architecture;house.architecturalAgeYears=brief.architecturalAgeYears;house.architecturalLot=brief.lot;
  return true;
}


CellHouse::PartyTally &CellHouse::partyTally()
{
  static PartyTally tally;
  return tally;
}
} // namespace procedural::world::generator
