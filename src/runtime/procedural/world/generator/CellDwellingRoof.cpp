#include "procedural/world/generator/CellDwellingRoof.hpp"

#include "procedural/geom2/CellShape.hpp"
#include "procedural/geom2/Offset.hpp"
#include "procedural/geom2/Skeleton.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <utility>

namespace procedural::world::generator {

using geom2::Board;
using geom2::Chain;
using geom2::Dir;
using geom2::kNoCell;
using geom2::Point;
using geom2::Polygon;
using geom2::Ring;

namespace {

// A board split into pieces a roof can be built on.
//
// The surface is laid over a contour, and a contour that visits a node twice is
// not one a surface can be laid over - the piece pinches at a corner, where two
// lobes meet diagonally. That happens often once a storey has more than one
// height in it: taking the tall rooms out of the band below, or the stair tower
// out of the top floor, can leave the remainder joined at a point.
//
// It used to be dropped, with a line in the ledger. A dropped piece is a HOLE IN
// THE ROOF - measured, one such piece was a hundred and fifty cells and the top
// of a castle simply had none - so it is split instead: one cell of the pinch is
// taken out, which separates the lobes, and that cell is roofed on its own.
std::vector<Board> roofable(const geom2::CellGrid &grid, const Board &area)
{
  std::vector<Board> out;
  std::vector<Board> queue;

  if (!area.empty())
  {
    queue.push_back(area);
  }

  for (uint32_t round = 0; round < 128 && !queue.empty(); ++round)
  {
    const Board todo = queue.back();
    queue.pop_back();

    Board left = todo;

    while (!left.empty())
    {
      uint32_t i = 0;
      uint32_t j = 0;
      left.first(i, j);

      const Board piece = grid.flood(left, grid.cell(int32_t(i), int32_t(j)));
      left = left & ~piece;

      const std::vector<Chain> loops = grid.outline(piece);

      if (!loops.empty() && grid.simpleOutline(loops))
      {
        out.push_back(piece);

        continue;
      }

      // The pinch: two cells of the piece meeting only at a corner. Taking one
      // of them out separates what they joined.
      uint32_t cut = geom2::kNoCell;

      for (const auto &[pi, pj] : geom2::cells(piece))
      {
        const int32_t a = int32_t(pi);
        const int32_t b = int32_t(pj);

        for (const int32_t step : {1, -1})
        {
          const uint32_t diagonal = grid.cell(a + 1, b + step);
          const uint32_t side = grid.cell(a + 1, b);
          const uint32_t end = grid.cell(a, b + step);

          const bool joined = diagonal != geom2::kNoCell && grid.contains(piece, diagonal) &&
                              (side == geom2::kNoCell || !grid.contains(piece, side)) &&
                              (end == geom2::kNoCell || !grid.contains(piece, end));

          if (joined)
          {
            cut = diagonal;

            break;
          }
        }

        if (cut != geom2::kNoCell)
        {
          break;
        }
      }

      if (cut == geom2::kNoCell)
      {
        // Not a corner pinch after all. Keep it whole rather than lose it.
        out.push_back(piece);

        continue;
      }

      Board one;
      grid.add(one, cut);
      out.push_back(one);

      Board rest = piece;
      grid.remove(rest, cut);

      if (!rest.empty())
      {
        queue.push_back(rest);
      }
    }
  }

  return out;
}

// A ring grown outward edge by edge, so that some edges may not move.
//
// `offsetPolygon` grows every edge by the same amount, which is right for eaves
// and wrong for a gable: an eave oversails because it is the bottom of a slope
// with nothing under it, while a gable is the TOP OF A WALL, and a wall does
// not move. Grown uniformly, the triangle where a roof dies into a taller wall
// was built half a metre outside that wall and stood free in the air - visible
// in the round as a grey wedge hanging off the side of a house.
//
// The rings here are rectilinear, so consecutive edges are perpendicular and
// the offset lines always meet: each new vertex is the intersection of its two
// neighbouring edges' supporting lines. Empty when that is not true of this
// ring, and the caller then leaves it as it was.
Ring oversailed(const Ring &ring, const std::vector<bool> &gables, const size_t from, const double eaves)
{
  const size_t n = ring.size();

  if (n < 3 || from + n > gables.size())
  {
    return Ring();
  }

  // Each edge's supporting line, pushed out by its own amount. The ring is
  // counter-clockwise, so the interior is on the left of an edge and outward is
  // the quarter turn to the right.
  std::vector<double> nx(n);
  std::vector<double> ny(n);
  std::vector<double> at(n);

  for (size_t k = 0; k < n; ++k)
  {
    const double ax = geom2::toMetres(ring[k].x);
    const double ay = geom2::toMetres(ring[k].y);
    const double bx = geom2::toMetres(ring[(k + 1) % n].x);
    const double by = geom2::toMetres(ring[(k + 1) % n].y);

    const double dx = bx - ax;
    const double dy = by - ay;
    const double length = std::sqrt(dx * dx + dy * dy);

    if (length < 1e-9)
    {
      return Ring();
    }

    nx[k] = dy / length;
    ny[k] = -dx / length;
    at[k] = ax * nx[k] + ay * ny[k] + (gables[from + k] ? 0. : eaves);
  }

  Ring out;

  for (size_t k = 0; k < n; ++k)
  {
    const size_t previous = (k + n - 1) % n;
    const double determinant = nx[previous] * ny[k] - ny[previous] * nx[k];

    if (std::abs(determinant) < 1e-6)
    {
      // Cell boundaries retain intermediate points on straight walls. Slide
      // those points with the shared supporting line instead of rejecting the
      // entire roof. Changes in setback must remain flush because the
      // roof skeleton requires square reflex corners.
      if (nx[previous] * nx[k] + ny[previous] * ny[k] < .999 ||
          std::abs(at[previous]-at[k]) > .001)
      {
        return Ring();
      }
      const double shift = gables[from + k] || gables[from + previous] ? 0. : eaves;
      out.push_back(geom2::Point{
        geom2::toUnit(geom2::toMetres(ring[k].x) + nx[k] * shift),
        geom2::toUnit(geom2::toMetres(ring[k].y) + ny[k] * shift)});
      continue;
    }

    const double x = (at[previous] * ny[k] - at[k] * ny[previous]) / determinant;
    const double y = (nx[previous] * at[k] - nx[k] * at[previous]) / determinant;

    out.push_back(geom2::Point{geom2::toUnit(x), geom2::toUnit(y)});
  }

  return out;
}


// Triangles of one roof plane welded into the polygon they cover.
//
// The skeleton answers in triangles, and every triangle carries the wall whose
// plane it lies in. A wall's face is therefore ONE flat piece cut up into
// three or four parts by the skeleton's internal events, and emitting the
// parts is what put a hundred and sixty faces on a roof that has eleven sides.
// It is also where the slivers came from: an event a millimetre from a ridge
// leaves a triangle a millimetre wide, invisible as geometry and unmissable as
// a seam once a renderer draws its edges.
//
// Welding is exact and needs no tolerance beyond a vertex weld: an interior
// edge of a triangulated region is walked once each way, a boundary edge once,
// so cancelling the pairs leaves precisely the boundary. Collinear vertices -
// every point where a cut met the outline - are dropped on the way out, which
// is what turns four triangles back into one trapezoid.
std::vector<std::vector<rewrite::Position>> weldPlane(const std::vector<std::array<rewrite::Position, 3>> &parts)
{
  using Key = std::array<int64_t, 3>;

  const auto key = [](const rewrite::Position &p) {
    return Key{std::llround(p.x * 1e6), std::llround(p.y * 1e6), std::llround(p.z * 1e6)};
  };

  std::map<Key, rewrite::Position> points;
  std::map<std::pair<Key, Key>, int> directed;

  for (const auto &part : parts)
  {
    for (size_t k = 0; k < 3; ++k)
    {
      const Key a = key(part[k]);
      const Key b = key(part[(k + 1) % 3]);

      if (a == b)
      {
        continue;
      }

      points[a] = part[k];
      points[b] = part[(k + 1) % 3];
      ++directed[{a, b}];
      --directed[{b, a}];
    }
  }

  std::map<Key, std::vector<Key>> next;

  for (const auto &[edge, balance] : directed)
  {
    for (int n = 0; n < balance; ++n)
    {
      next[edge.first].push_back(edge.second);
    }
  }

  std::vector<std::vector<rewrite::Position>> rings;

  for (auto &[from, tos] : next)
  {
    while (!tos.empty())
    {
      std::vector<Key> loop{from};
      Key at = tos.back();
      tos.pop_back();

      // Bounded by the edge count: every step consumes one directed edge.
      for (size_t guard = 0; at != from && guard <= directed.size(); ++guard)
      {
        auto it = next.find(at);

        if (it == next.end() || it->second.empty())
        {
          loop.clear();
          break;
        }

        loop.push_back(at);
        at = it->second.back();
        it->second.pop_back();
      }

      if (loop.size() < 3)
      {
        continue;
      }

      std::vector<rewrite::Position> ring;

      for (const Key &node : loop)
      {
        ring.push_back(points[node]);
      }

      // Drop the collinear vertices the cuts left behind.
      std::vector<rewrite::Position> simple;

      for (size_t k = 0; k < ring.size(); ++k)
      {
        const rewrite::Position &a = ring[(k + ring.size() - 1) % ring.size()];
        const rewrite::Position &b = ring[k];
        const rewrite::Position &c = ring[(k + 1) % ring.size()];
        const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
        const double vx = c.x - b.x, vy = c.y - b.y, vz = c.z - b.z;
        const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;

        if (std::sqrt(nx * nx + ny * ny + nz * nz) > 1e-7)
        {
          simple.push_back(b);
        }
      }

      if (simple.size() >= 3)
      {
        rings.push_back(std::move(simple));
      }
    }
  }

  return rings;
}

bool fail(std::string *error, const char *text)
{
  if (error != nullptr)
  {
    *error = text;
  }

  return false;
}

// A contour turned into a ring, merging collinear edges only while they agree
// about being a gable.
//
// Splitting a straight wall where the mass above it stops is the whole point: a
// run that is part gable and part eave has to become two, or the roof either
// dies into a wall that is not there or runs down into one that is.
void ringAndGables(const CellGrid &grid,
                   const CellFrame &frame,
                   const Chain &contour,
                   const Board &above,
                   const std::vector<uint32_t> &party,
                   Ring &ring,
                   std::vector<bool> &gables)
{
  ring.clear();
  gables.clear();

  if (contour.empty())
  {
    return;
  }

  const size_t n = contour.size();
  const size_t start = geom2::firstCorner(grid, contour);

  // A gable where there is mass beyond the wall: the storey above, the taller
  // room next door - or the neighbour's house, which stands on ground this
  // grid cannot see and is named by its edges instead.
  const auto isGable = [&](const uint32_t e) {
    if (std::binary_search(party.begin(), party.end(), e))
    {
      return true;
    }

    const uint32_t outside = grid.edgeCell(grid.twin(e));

    return outside != kNoCell && grid.contains(above, outside);
  };

  for (size_t step = 0; step < n; ++step)
  {
    const size_t k = (start + step) % n;
    const size_t previous = (start + step + n - 1) % n;
    const bool turned = grid.decode(contour[k]).dir != grid.decode(contour[previous]).dir;
    const bool changed = isGable(contour[k]) != isGable(contour[previous]);

    if (turned || changed)
    {
      double x = 0;
      double z = 0;
      geom2::nodePoint(grid, frame, grid.decode(contour[k]).node, x, z);
      ring.push_back(Point{geom2::toUnit(x), geom2::toUnit(z)});
      gables.push_back(isGable(contour[k]));
    }
  }

}

} // namespace

bool planRoofs(const CellHouse &house,
               const CellDwellingSettings &settings,
               Stream &stream,
               CellRoofs &out,
               Ledger &ledger,
               std::string *error)
{
  out = CellRoofs();

  if (house.floors.empty())
  {
    return fail(error, "no floors to roof");
  }

  const double storey = house.gridClass.storeyMetres;

  // A fortified roof is not a roof. It is a deck with the wall carried up round
  // it: no pitch, no gables, no eaves, and a parapet you can stand behind. The
  // surface is still built by the skeleton - a flat slope is a slope of nought,
  // and going through the same construction is what keeps holes, courts and
  // stepped storeys working - but nothing about it falls.
  const bool fortified = std::find(house.tags.begin(), house.tags.end(), "fortified") != house.tags.end();

  // The roof is built in the PLAN's frame, not the world's.
  //
  // Everything downstream of `roofFromSkeleton` turns on right angles being
  // right: the L-infinity wavefront is the correct roof only for an orthogonal
  // plan, the eaves offset intersects supporting lines that are assumed
  // perpendicular, and the one-face-per-plane merge compares plane equations
  // with no tolerance at all because on a thirty metre face a tolerance is
  // micrometres of height. A lot turned 0.6 rad gave the ring world
  // coordinates whose corners are right to about 1e-7 rad, and all three
  // silently degraded: faces that are one plane stopped merging, so a wall's
  // roof arrived as slabs with seams down it, and planes that should meet in a
  // ridge crossed instead.
  //
  // In the plan's own frame a node lands on an exact multiple of the cell, the
  // corners are bit-exact right angles, and the construction is the exact one
  // it was written to be. The finished surface is turned into the world at the
  // end - a rotation, which changes nothing about it.
  geom2::CellFrame flat;
  flat.cellMetres = house.frame.cellMetres;
  flat.originX = 0;
  flat.originZ = 0;
  flat.axisX = 1;
  flat.axisZ = 0;

  const auto toWorld = [&](const rewrite::Position &p) {
    double x = 0;
    double z = 0;
    geom2::framePoint(house.frame, p.x / house.frame.cellMetres, p.z / house.frame.cellMetres, x, z);

    return rewrite::Position{x, p.y, z};
  };

  for (size_t level = 0; level < house.floors.size(); ++level)
  {
    const CellFloor &floor = house.floors[level];
    const Board storeyAbove = level + 1 < house.floors.size() ? house.floors[level + 1].area : Board();

    out.summitHeight.push_back(0);

    // A storey may have more than one height in it.
    //
    // One storey with a nave at six and a half metres and a transept at three
    // and a quarter is not two storeys, and the roof has to be built band by
    // band: each height is roofed at its own level, and what it dies into is
    // ANYTHING TALLER - the storey above, or the nave next door. That second
    // clause is the whole generalisation; the rule was always "a roof dies into
    // a taller mass" and it only ever looked at the floor above because a floor
    // used to have one height.
    std::vector<double> bands;

    // Heights within half a metre are one band.
    //
    // A band is roofed at its own level and anything taller is what its roof
    // DIES INTO - a vertical wall rising through the tiles. That is right for
    // a nave beside its aisles and wrong for two rooms whose ceilings differ
    // by a few centimetres, which is what an ordinary house has: every such
    // difference put a gable wall through the middle of its roof, and a
    // roofscape of houses with walls standing up out of them is what those
    // stepped, wedge-shaped roofs are.
    const double together = .5;

    for (uint32_t r = 0; r < uint32_t(floor.roomAreas.size()); ++r)
    {
      const double own = r < floor.roomHeight.size() && floor.roomHeight[r] > 0 ? floor.roomHeight[r] : storey;
      bool joined = false;

      for (double &band : bands)
      {
        if (std::abs(band - own) < together)
        {
          // The tallest in the group: a roof may sit below a ceiling, never
          // through it.
          band = std::max(band, own);
          joined = true;
          break;
        }
      }

      if (!joined)
      {
        bands.push_back(own);
      }
    }

    if (bands.empty())
    {
      bands.push_back(storey);
    }

    std::sort(bands.begin(), bands.end());

    for (const double band : bands)
    {
      Board mine;
      Board taller;

      for (uint32_t r = 0; r < uint32_t(floor.roomAreas.size()); ++r)
      {
        const double own = r < floor.roomHeight.size() && floor.roomHeight[r] > 0 ? floor.roomHeight[r] : storey;

        // Three cases, and the third is the one that was missing.
        //
        // A room TALLER than this band is what the band's roof dies into. A
        // room at the band is what it covers. A room SHORTER belongs to another
        // band entirely and is neither - lumping it in with "mine" roofed it a
        // second time, at a height that is not its own, which is where the
        // duplicated and floating roof faces over a church's transepts came
        // from.
        // ...to the same tolerance the bands were grouped by, or a room half
        // a metre under its own band's head reads as a taller mass next door
        // and the grouping buys nothing.
        if (own > band + together)
        {
          taller |= floor.roomAreas[r];
        }
        else if (own > band - together)
        {
          mine |= floor.roomAreas[r];
        }
      }

      if (floor.roomAreas.empty())
      {
        mine = floor.area;
      }

      mine = mine & floor.area;

      // No roof over the bell tower.
      //
      // The head of that shaft is a flat DECK with a hole in it and a drum
      // standing on it, not a roof, and the skeleton has no way to say either.
      // So the tower's cells are taken out of every band and the deck is built
      // where the drum is - one place that knows the tower's whole section
      // rather than two that each know half of it.
      if (floor.level == 0)
      {
        for (const CellBellTower &tower : house.bellTowers)
        {
          mine = mine & ~tower.cells;
        }
      }

      const Board above = storeyAbove | taller;
      const Board exposed = mine & ~above;

      if (exposed.empty())
      {
        continue;
      }

      for (const Board &piece : roofable(house.grid, exposed))
      {
      const std::vector<Chain> loops = house.grid.outline(piece);

      if (loops.empty() || !house.grid.simpleOutline(loops))
      {
        ledger.add(LedgerKind::Omitted, 0, "a piece of roof had no simple outline and was left off");
        continue;
      }

      Polygon ground;
      std::vector<bool> gables;
      ringAndGables(house.grid, flat, loops.front(), above, house.partyEdges, ground.outer, gables);

      for (size_t k = 1; k < loops.size(); ++k)
      {
        Ring hole;
        std::vector<bool> holeGables;
        ringAndGables(house.grid, flat, loops[k], above, house.partyEdges, hole, holeGables);

        if (hole.size() >= 3)
        {
          ground.holes.push_back(std::move(hole));
          gables.insert(gables.end(), holeGables.begin(), holeGables.end());
        }
      }

      if (ground.outer.size() < 3)
      {
        continue;
      }

      if (!counterClockwise(ground.outer))
      {
        std::reverse(ground.outer.begin(), ground.outer.end());
        std::reverse(gables.begin(), gables.begin() + long(ground.outer.size()));
      }

      for (Ring &hole : ground.holes)
      {
        if (counterClockwise(hole))
        {
          std::reverse(hole.begin(), hole.end());
        }
      }

      // The eaves oversail the wall, so the ring grows before the skeleton runs.
      // A parapet does not oversail anything - it stands on the wall - so a
      // fortified piece keeps the ring it was built from.
      Polygon widened = ground;

      if (!fortified && settings.eavesOverhangMetres > 0)
      {
        Polygon grown;
        grown.outer = oversailed(ground.outer, gables, 0, settings.eavesOverhangMetres);

        size_t from = ground.outer.size();
        bool ok = !grown.outer.empty();

        for (const Ring &hole : ground.holes)
        {
          // A hole is wound the other way, so its own outward - away from the
          // building and into the court - is already what `oversailed` takes as
          // outward. Nothing to flip.
          Ring wider = ok ? oversailed(hole, gables, from, settings.eavesOverhangMetres) : Ring();

          ok = ok && !wider.empty();

          if (ok)
          {
            grown.holes.push_back(std::move(wider));
          }

          from += hole.size();
        }

        // Only when the grow kept the shape. A mitre that folded the ring over
        // itself has changed which wall is which, and the gable flags would then
        // be answering about walls that no longer exist.
        if (ok && geom2::polygonFault(grown).empty())
        {
          widened = std::move(grown);
        }
      }

      // An occupied pocket bounded entirely by taller/party walls is not a
      // light well: real courts were already removed from `mine`. Give it a
      // hipped cap without oversail; the higher-wall clearance below still
      // limits its ridge. Never expose an interior slab for lack of an eave.
      const bool anyEave = std::find(gables.begin(), gables.end(), false) != gables.end();


      // The pitch this band is roofed at.
      //
      // Proportional to how many storeys the rooms under it stand, so a hall,
      // a nave and a chancel each get a steeper roof than the ranges around
      // them and the apex climbs with the walls. A deck has no pitch at all.
      const double storeys = storey > 1e-9 ? std::max(1., band / storey) : 1.;
      const double pitch = std::min(settings.roofPitchCeiling, settings.roofPitch * storeys);

      geom2::RoofOptions options;
      options.slope = fortified ? 0. : pitch;
      options.gables = fortified || !anyEave || settings.roofAlwaysHipped
        ? std::vector<bool>(gables.size(), false) : gables;

      std::vector<geom2::RoofTriangle> triangles;
      std::string why;

      if (!geom2::roofFromSkeleton(fortified ? ground : widened, options, triangles, &why))
      {
        // The signal that matters. Our construction is exact on a rectilinear
        // plan, so a refusal means the ring stopped being one - which is the
        // offset's doing, not the skeleton's.
        ledger.add(LedgerKind::Rejected, 0, "roof refused: " + why);

        return fail(error, "a roof could not be built");
      }

      double summitX = 0;
      double summitZ = 0;
      double summit = 0;

      geom2::roofSummit(triangles, summitX, summitZ, summit);

      const double base = double(floor.level) * storey + band;

      // How high the mass this piece dies into already rises.
      //
      // A gable is the triangle that closes the gap between a roof surface and
      // the taller wall it runs into. In this model there is usually no gap:
      // a wall is as tall as the tallest room it divides, so the wall ALREADY
      // fills that triangle, and emitting a gable face there lays a second
      // surface in the same plane. Coplanar duplicates are what a painter's
      // algorithm cannot sort, and they render as slabs of wall lying across
      // the roof - which is exactly what they looked like.
      double covered = 1e300;

      for (uint32_t r = 0; r < uint32_t(floor.roomAreas.size()); ++r)
      {
        const double own = r < floor.roomHeight.size() && floor.roomHeight[r] > 0 ? floor.roomHeight[r] : storey;

        if (own > band + together && !(floor.roomAreas[r] & above).empty())
        {
          covered = std::min(covered, double(floor.level) * storey + own);
        }
      }

      if (!storeyAbove.empty())
      {
        covered = std::min(covered, double(floor.level + 1) * storey + storey);
      }

      // A setback roof must die into the wall above, not rise through its
      // eave and roof. Large estate wings can have a run much wider than a
      // storey is tall. Preserve their complete skeleton and planar faces,
      // reducing the pitch of the whole piece instead of clipping triangles.
      bool touchesAbove = false;
      for (const auto &[i, j] : geom2::cells(piece))
      {
        for (const auto &[di, dj] : {std::pair{1, 0}, std::pair{-1, 0},
                                    std::pair{0, 1}, std::pair{0, -1}})
        {
          const uint32_t near = house.grid.cell(int32_t(i) + di, int32_t(j) + dj);
          touchesAbove = touchesAbove || (near != kNoCell && house.grid.contains(above, near));
        }
      }
      const double availableRise = std::max(0., covered - base);
      // What the ridge may reach: the mass it dies into, and the cap.
      double ceiling = settings.roofRidgeMetres * storeys;

      if (touchesAbove)
      {
        ceiling = std::min(ceiling, availableRise);
      }

      if (summit > ceiling && summit > 1e-9)
      {
        const double scale = ceiling / summit;
        for (auto &triangle : triangles)
        {
          triangle.ay *= scale;
          triangle.by *= scale;
          triangle.cy *= scale;
        }
        summit = ceiling;
      }
      out.summitHeight[level] = std::max(out.summitHeight[level], summit);

      if (fortified)
      {
        // The parapet, from the ring the deck was built on and from every court
        // in it. Its own wall, above the storey's, so the mass reads as a wall
        // that did not stop rather than as a lid with a lip.
        const auto band = [&](const Ring &ring) {
          for (size_t k = 0; k < ring.size(); ++k)
          {
            const geom2::Point &from = ring[k];
            const geom2::Point &to = ring[(k + 1) % ring.size()];

            rewrite::DwellingWall wall;
            wall.floor = uint32_t(level + (house.basement.has_value() ? 1 : 0));
            wall.a = toWorld(rewrite::Position{geom2::toMetres(from.x), base, geom2::toMetres(from.y)});
            wall.b = toWorld(rewrite::Position{geom2::toMetres(to.x), base, geom2::toMetres(to.y)});
            wall.thickness = settings.parapetThicknessMetres;
            wall.height = settings.parapetHeightMetres;
            wall.exterior = true;
            wall.kind = rewrite::WallKind::Solid;
            out.parapets.push_back(wall);
          }
        };

        band(ground.outer);

        for (const Ring &hole : ground.holes)
        {
          band(hole);
        }
      }

      // One face per roof PLANE, not one per skeleton triangle.
      //
      // A slope is bucketed by the wall whose plane it lies in and the bucket
      // welded into the polygon it covers. A gable is kept as it came - see
      // below for why - and each one stands on its own.
      struct Plane
      {
        bool gable = false;
        double covered = 0;
        std::vector<std::array<rewrite::Position, 3>> parts;
      };

      std::map<std::pair<uint32_t, int64_t>, Plane> planes;
      std::vector<Plane> loose;

      for (const geom2::RoofTriangle &triangle : triangles)
      {
        // Splitting a wall run where the mass above it stops leaves collinear
        // vertices in the ring, and the triangulation dutifully makes a
        // zero-area triangle out of each. They carry nothing and they count as
        // degenerate faces to anything checking the mesh, so they are dropped
        // here rather than left for a renderer to trip over.
        const double ux = triangle.bx - triangle.ax, uy = triangle.by - triangle.ay, uz = triangle.bz - triangle.az;
        const double vx = triangle.cx - triangle.ax, vy = triangle.cy - triangle.ay, vz = triangle.cz - triangle.az;
        const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;

        if (std::sqrt(nx * nx + ny * ny + nz * nz) * .5 < 1e-6)
        {
          continue;
        }

        // Coverage belongs to this boundary, not to the smallest taller room
        // anywhere in the building. In particular, `covered == infinity`
        // means no higher mass was found, NOT an infinitely tall party wall.
        double gableCovered=base;
        if (triangle.gable)
        {
          const double x[3]={triangle.ax,triangle.bx,triangle.cx};
          const double z[3]={triangle.az,triangle.bz,triangle.cz};
          size_t a=0,b=1;double span=0;
          for(size_t i=0;i<3;++i)for(size_t j=i+1;j<3;++j){const double d=std::hypot(x[j]-x[i],z[j]-z[i]);if(d>span){span=d;a=i;b=j;}}
          if(span>1e-9)for(double side:{-1.,1.}) {
            const double offset=house.frame.cellMetres*.1*side;
            const auto cell=geom2::cellAtPoint(house.grid,flat,(x[a]+x[b])*.5-(z[b]-z[a])/span*offset,
                                                                        (z[a]+z[b])*.5+(x[b]-x[a])/span*offset);
            if(cell==kNoCell || house.grid.contains(piece,cell))continue;
            if(house.grid.contains(storeyAbove,cell))gableCovered=std::max(gableCovered,double(floor.level+2)*storey);
            for(size_t r=0;r<floor.roomAreas.size();++r)if(house.grid.contains(floor.roomAreas[r],cell)) {
              const double own=r<floor.roomHeight.size() && floor.roomHeight[r]>0 ? floor.roomHeight[r] : storey;
              gableCovered=std::max(gableCovered,double(floor.level)*storey+own);
            }
          }
          const double apex = base + std::max(triangle.ay, std::max(triangle.by, triangle.cy));

          if (apex <= gableCovered + 1e-6)
          {
            continue;
          }
        }

        const std::array<rewrite::Position, 3> part{
          rewrite::Position{triangle.ax, base + triangle.ay, triangle.az},
          rewrite::Position{triangle.bx, base + triangle.by, triangle.bz},
          rewrite::Position{triangle.cx, base + triangle.cy, triangle.cz}};

        // A gable is left in triangles on purpose.
        //
        // What it is clipped against is a horizontal line - how far up the
        // mass beside it already covers it - and the clip keeps the corners
        // above the line plus the crossings, in order. A triangle crosses a
        // horizontal line at most twice, so that is exact; a welded polygon
        // over a stepped wall can cross it four times and the same walk
        // returns a ring that crosses itself. Gables are a handful of faces
        // and none of the polygon count is in them.
        if (triangle.gable)
        {
          Plane one;
          one.gable = true;
          one.covered = gableCovered;
          one.parts.push_back(part);
          loose.push_back(std::move(one));

          continue;
        }

        Plane &plane = planes[{triangle.edge, 0}];
        plane.gable = false;
        plane.covered = base;
        plane.parts.push_back(part);
      }

      const auto emit = [&](const Plane &plane, std::vector<std::vector<rewrite::Position>> rings) {
        for (std::vector<rewrite::Position> &ring : rings)
        {
          rewrite::DwellingRoofFace face;
          face.wing = uint32_t(level);
          face.gable = plane.gable;
          face.polygon = std::move(ring);

          if(plane.gable && plane.covered>base+1e-6) {
            std::vector<rewrite::Position> exposed;
            for(size_t k=0;k<face.polygon.size();++k){const auto a=face.polygon[k],b=face.polygon[(k+1)%face.polygon.size()];
              if(a.y>=plane.covered)exposed.push_back(a);
              if((a.y<plane.covered)!=(b.y<plane.covered)){const double t=(plane.covered-a.y)/(b.y-a.y);exposed.push_back({a.x+(b.x-a.x)*t,plane.covered,a.z+(b.z-a.z)*t});}
            }
            face.polygon=std::move(exposed);
          }

          if (face.polygon.size() >= 3)
          {
            for (rewrite::Position &corner : face.polygon)
            {
              corner = toWorld(corner);
            }

            out.faces.push_back(std::move(face));
          }
        }
      };

      for (auto &[bucket, plane] : planes)
      {
        emit(plane, weldPlane(plane.parts));
      }
      // Finish the roof's actual oversail. The wall and grown rings retain
      // matching vertices, including mitred corners and courtyard returns.
      const auto trim=[&](const Ring &inner,const Ring &outer,size_t offset) {
        if(inner.size()!=outer.size())return;
        const auto at=[&](geom2::Point p,double y){return rewrite::Position{geom2::toMetres(p.x),y,geom2::toMetres(p.y)};};
        const auto panel=[&](std::vector<rewrite::Position> points) {
          const auto same=[](const auto &a,const auto &b) {
            return std::abs(a.x-b.x)+std::abs(a.y-b.y)+std::abs(a.z-b.z)<1e-8;
          };
          points.erase(std::unique(points.begin(),points.end(),same),points.end());
          if(points.size()>1 && same(points.front(),points.back()))points.pop_back();
          if(points.size()<3)return;
          rewrite::DwellingRoofFace face;face.wing=uint32_t(level);face.soffit=true;
          std::reverse(points.begin(),points.end());
          for(auto p:points)face.polygon.push_back(toWorld(p));out.faces.push_back(std::move(face));
        };
        for(size_t k=0;k<inner.size();++k) {
          if(gables[offset+k])continue;
          const size_t next=(k+1)%inner.size();
          auto a=at(inner[k],base-.12),b=at(inner[next],base-.12);
          auto c=at(outer[next],base-.12),d=at(outer[k],base-.12);
          const double run=std::hypot(b.x-a.x,b.z-a.z);
          if(run<.1 || std::max(std::hypot(d.x-a.x,d.z-a.z),std::hypot(c.x-b.x,c.z-b.z))<.05)continue;
          // Down-facing soffit; the outer fascia closes its edge to the tile.
          panel({a,b,c,d});auto C=c,D=d;C.y=D.y=base;
          panel({d,c,C,D});
          auto A=a,B=b;A.y=B.y=base;
          if(gables[offset+(k+inner.size()-1)%inner.size()])panel({a,d,D,A});
          if(gables[offset+next])panel({c,b,B,C});
          const unsigned count=std::max(1u,unsigned(std::ceil(run/.9)));
          for(unsigned j=0;j<count;++j) {
            const double t=(j+.5)/count,half=.05/run;
            const auto point=[&](bool outside,double station,double y) {
              const auto p=outside?d:a,q=outside?c:b;
              return rewrite::Position{p.x+(q.x-p.x)*station,y,p.z+(q.z-p.z)*station};
            };
            auto A=point(false,t-half,base-.25),B=point(false,t+half,base-.25);
            auto E=point(true,t-half,base-.25),F=point(true,t+half,base-.25);
            auto a=A,b=B,e=E,f=F;a.y=b.y=e.y=f.y=base-.12;
            panel({A,B,F,E});panel({a,e,f,b});panel({A,a,b,B});
            panel({E,F,f,e});panel({A,E,e,a});panel({B,b,f,F});
          }
        }
      };
      if(!fortified && settings.eavesOverhangMetres>0) {
        trim(ground.outer,widened.outer,0);size_t offset=ground.outer.size();
        for(size_t h=0;h<ground.holes.size();++h){trim(ground.holes[h],widened.holes[h],offset);offset+=ground.holes[h].size();}
      }


      for (const Plane &plane : loose)
      {
        emit(plane, {{plane.parts.front()[0], plane.parts.front()[1], plane.parts.front()[2]}});
      }
      }
    }
  }

  if (!settings.chimneys)
  {
    return true;
  }

  // The chimneys. A stack has to come up through open roof, so its cell must be
  // clear of the storey above in all eight directions - otherwise it rises
  // through a wall.
  std::vector<std::pair<uint32_t, uint32_t>> stacks;

  for (size_t level = 0; level < house.floors.size(); ++level)
  {
    const CellFloor &floor = house.floors[level];
    Board candidates = floor.area;

    if (house.spiral.has_value())
    {
      house.grid.remove(candidates, house.spiral->landing);
    }

    if (level + 1 < house.floors.size())
    {
      house.grid.remove(candidates, house.entranceLanding);
      const Board &above = house.floors[level + 1].area;
      candidates = candidates & ~above;

      Board clear;

      for (const auto &[i, j] : geom2::cells(candidates))
      {
        bool open = true;

        for (const auto &[di, dj] : geom2::kRose)
        {
          const uint32_t near = house.grid.cell(int32_t(i) + di, int32_t(j) + dj);

          if (near != kNoCell && house.grid.contains(above, near))
          {
            open = false;
            break;
          }
        }

        if (open)
        {
          clear.set(i, j);
        }
      }

      candidates = clear;
    }

    if (candidates.empty())
    {
      continue;
    }

    const std::vector<std::pair<uint32_t, uint32_t>> cells = geom2::cells(candidates);
    const auto &[i, j] = cells[stream.below(uint32_t(cells.size()))];
    stacks.emplace_back(house.grid.cell(int32_t(i), int32_t(j)), uint32_t(level));
  }

  // About one per sixteen cells of ground, at least one.
  const int32_t wanted =
    std::max(1, stream.roundStochastic(double(house.floors.front().area.count()) / 16.));

  while (int32_t(stacks.size()) > wanted)
  {
    stacks.erase(stacks.begin() + stream.below(uint32_t(stacks.size())));
  }

  for (const auto &[cell, level] : stacks)
  {
    double cx = 0;
    double cz = 0;
    geom2::cellPoint(house.grid, house.frame, cell, cx, cz);
    const double half = settings.chimneySideMetres * .5;

    rewrite::DwellingPillar stack;
    stack.base = 0;
    stack.top = double(level) * storey + storey + out.summitHeight[level] + settings.chimneyAboveRidgeMetres;
    stack.footprint = {rewrite::Position{cx - half, 0, cz - half},
                       rewrite::Position{cx + half, 0, cz - half},
                       rewrite::Position{cx + half, 0, cz + half},
                       rewrite::Position{cx - half, 0, cz + half}};
    out.chimneys.push_back(std::move(stack));
  }

  return true;
}

} // namespace procedural::world::generator
