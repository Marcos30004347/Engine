#include "procedural/world/generator/CellDwellingClutter.hpp"

#include "procedural/geom2/CellShape.hpp"
#include "procedural/geom2/Skeleton.hpp"

#include <algorithm>
#include <cmath>

namespace procedural::world::generator {

using geom2::Board;
using geom2::Chain;
using geom2::Dir;
using geom2::kNoCell;

namespace {

struct Point2
{
  double x = 0, z = 0;
};

// A closed polyline a piece can come to rest against.
struct Abris
{
  std::vector<Point2> points;
  std::vector<std::pair<uint32_t, uint32_t>> segments;
};

// The four corners of a piece, in the room's frame.
std::vector<Point2> cornersOf(const ClutterPiece &piece)
{
  const double c = std::cos(piece.angle);
  const double s = std::sin(piece.angle);
  const double a = piece.halfLong;
  const double b = piece.halfShort;

  std::vector<Point2> out;

  for (const auto &[u, v] : std::vector<std::pair<double, double>>{{-a, -b}, {a, -b}, {a, b}, {-a, b}})
  {
    out.push_back(Point2{piece.x + u * c - v * s, piece.z + u * s + v * c});
  }

  return out;
}

// How far along `dir` the point `p` travels before it crosses the segment ab.
// Negative or no crossing gives nothing.
bool travelTo(const Point2 &p, const double dx, const double dz, const Point2 &a, const Point2 &b, double &t)
{
  const double ex = b.x - a.x;
  const double ez = b.z - a.z;
  const double denominator = dx * ez - dz * ex;

  if (std::abs(denominator) < 1e-12)
  {
    return false;
  }

  const double qx = a.x - p.x;
  const double qz = a.z - p.z;
  t = (qx * ez - qz * ex) / denominator;
  const double u = (qx * dz - qz * dx) / denominator;

  return t > 1e-9 && u >= 0 && u <= 1;
}

// The minimum travel along `dir` before the shape first touches the polyline.
//
// Every vertex of the shape cast forward against every segment of the polyline,
// and every vertex of the polyline cast BACKWARD against every edge of the
// shape. Both halves are needed: a wall corner can be the first thing a wide
// piece meets without any of the piece's own corners reaching a wall segment.
double sweep(const Abris &abris, const std::vector<Point2> &shape, const double dx, const double dz)
{
  double best = 1e300;

  for (const auto &[i, j] : abris.segments)
  {
    for (const Point2 &p : shape)
    {
      double t = 0;

      if (travelTo(p, dx, dz, abris.points[i], abris.points[j], t))
      {
        best = std::min(best, t);
      }
    }
  }

  for (size_t k = 0; k < shape.size(); ++k)
  {
    const Point2 &a = shape[k];
    const Point2 &b = shape[(k + 1) % shape.size()];

    for (const Point2 &q : abris.points)
    {
      double t = 0;

      if (travelTo(q, -dx, -dz, a, b, t))
      {
        best = std::min(best, t);
      }
    }
  }

  return best;
}

} // namespace

ClutterRule clutterFor(const rewrite::RoomKind kind)
{
  // Watabou's own tuples, read off his source. Read `observatory` as the
  // telescope - jitter nought, so it slides all the way to a wall - and
  // `greenhouse` as planting: never aligned, never rectangular, ten per cell.
  switch (kind)
  {
    case rewrite::RoomKind::Bedroom:
      return ClutterRule{.5, 1, 1, .8, 1, .9, .3};
    case rewrite::RoomKind::Kitchen:
      return ClutterRule{.4, .5, 3, .8, 1, .8, 1};
    case rewrite::RoomKind::Workshop:
      return ClutterRule{.35, 1, 3, .8, 1, .8, 1};
    case rewrite::RoomKind::Library:
      return ClutterRule{.35, 1, 1, .5, 1, 1, 1};
    case rewrite::RoomKind::LivingRoom:
      return ClutterRule{.5, .4, 2, .9, .2, .7, .3};
    case rewrite::RoomKind::Observatory:
      return ClutterRule{.5, 0, 10, .9, 1, .4, .2};
    case rewrite::RoomKind::Cellar:
      return ClutterRule{.25, .5, 1, .2, 0, .1, 10};
    case rewrite::RoomKind::Conservatory:
      return ClutterRule{.25, .2, 10, 1, 0, 0, 10};
    case rewrite::RoomKind::Landing:
      return ClutterRule{.3, 1, 3, .8, 1, .8, .1};
    case rewrite::RoomKind::Passage:
    case rewrite::RoomKind::Staircase:
    case rewrite::RoomKind::Closet:
      return ClutterRule{0, 1, 1, .8, 1, .8, 0};
    default:
      return ClutterRule{};
  }
}

bool furnishRoom(const CellGrid &grid,
                 const CellHouse &house,
                 const CellFloor &floor,
                 const uint32_t room,
                 const ClutterRule &rule,
                 Stream &stream,
                 std::vector<ClutterPiece> &out,
                 const double outerWallMetres,
                 const double innerWallMetres)
{
  out.clear();

  if (room >= floor.roomAreas.size())
  {
    return false;
  }

  const Board &area = floor.roomAreas[room];

  // The obstacles: the room's wall, and a spur half a cell inward at every
  // opening. The spur is what keeps furniture out of a doorway, and it is two
  // points.
  Abris abris;
  std::vector<uint32_t> doorEdges;

  for (const CellDoor &door : floor.doors)
  {
    if (door.a == room)
    {
      doorEdges.push_back(door.edge);
    }
    else if (door.b == room)
    {
      doorEdges.push_back(grid.twin(door.edge));
    }
  }

  // The wall itself, unbroken.
  //
  // The spur at a doorway is an EXTRA bar across the opening, not a detour in
  // the wall line. Threaded into the polyline it replaces the wall there with
  // two segments going inward and back, and a piece sliding at the door goes
  // straight out through the gap - which is what it did.
  const uint32_t wallFrom = uint32_t(abris.points.size());

  const auto &contour=floor.roomContours[room];
  const auto inset=[&](uint32_t e) {
    double x=0,z=0,cx=0,cz=0;
    geom2::edgePoint(grid,house.frame,e,.5,x,z);
    geom2::cellPoint(grid,house.frame,grid.edgeCell(e),cx,cz);
    const double length=std::hypot(cx-x,cz-z);
    // Use the same classified boundary as wall realisation, including courts
    // and reserved cells that are on the board but not occupied room space.
    const double depth=(exteriorEdge(floor,e) ? outerWallMetres : innerWallMetres*.5)+.03;
    return Point2{(cx-x)/length*depth,(cz-z)/length*depth};
  };
  for (size_t k=0;k<contour.size();++k)
  {
    const uint32_t e=contour[k];
    double x = 0;
    double z = 0;
    geom2::nodePoint(grid, house.frame, grid.decode(e).node, x, z);
    const auto current=inset(e),previous=inset(contour[(k+contour.size()-1)%contour.size()]);
    // Orthogonal lattice edges: sum perpendicular offsets at a corner;
    // collinear edges share one offset. This is the inner, not centre, line.
    const bool corner=std::abs(current.x*previous.x+current.z*previous.z)<1e-10;
    const double scale=corner?1.:std::max(1.,std::hypot(previous.x,previous.z)/std::hypot(current.x,current.z));
    abris.points.push_back(Point2{x+current.x*scale+(corner?previous.x:0),z+current.z*scale+(corner?previous.z:0)});
  }

  const uint32_t wallTo = uint32_t(abris.points.size());

  for (uint32_t k = wallFrom; k + 1 < wallTo; ++k)
  {
    abris.segments.emplace_back(k, k + 1);
  }

  if (wallTo > wallFrom + 2)
  {
    abris.segments.emplace_back(wallTo - 1, wallFrom);
  }

  geom2::Ring interior;
  for(uint32_t k=wallFrom;k<wallTo;++k)interior.push_back({geom2::toUnit(abris.points[k].x),geom2::toUnit(abris.points[k].z)});
  const auto fitsInside=[&](const ClutterPiece &piece) {
    const auto corners=cornersOf(piece);
    for(auto p:corners)if(!geom2::contains(interior,{geom2::toUnit(p.x),geom2::toUnit(p.z)}))return false;
    // Corner tests alone miss a box bridging a concave room notch.
    for(size_t k=0;k<corners.size();++k)for(uint32_t j=wallFrom;j<wallTo;++j) {
      auto a=corners[k],b=corners[(k+1)%corners.size()];double t=0;
      if(travelTo(a,b.x-a.x,b.z-a.z,abris.points[j],abris.points[j+1==wallTo?wallFrom:j+1],t) && t<1.-1e-7)return false;
    }
    return true;
  };

  // And a bar across each opening, half a cell in.
  std::vector<uint32_t> barred = doorEdges;

  if (std::find(floor.roomContours[room].begin(), floor.roomContours[room].end(), house.entrance) !=
      floor.roomContours[room].end())
  {
    barred.push_back(house.entrance);
  }

  // A flight is a thing in the room, not merely a cell nothing spawns on: a
  // chest slid across the floor stops at the bottom step.
  for (const CellStair &stair : floor.stairs)
  {
    for (const uint32_t cell : wellCells(grid, stair))
    {
      if (!grid.contains(area, cell))
      {
        continue;
      }

      const uint32_t at = uint32_t(abris.points.size());

      for (uint32_t c = 0; c < 4; ++c)
      {
        double x = 0;
        double z = 0;
        geom2::nodePoint(grid, house.frame,
                         grid.node(int32_t(grid.cellRow(cell)) + int32_t(c / 2),
                                   int32_t(grid.cellColumn(cell)) + int32_t(c % 2 == (c / 2) ? 0 : 1)),
                         x, z);
        abris.points.push_back(Point2{x, z});
      }

      for (uint32_t c = 0; c < 4; ++c)
      {
        abris.segments.emplace_back(at + c, at + (c + 1) % 4);
      }
    }
  }

  for (const uint32_t e : barred)
  {
    const Dir inward = cw(grid.decode(e).dir);
    double ox = 0, oz = 0, ix = 0, iz = 0;
    geom2::framePoint(house.frame, 0, 0, ox, oz);
    geom2::framePoint(house.frame, double(geom2::deltaJ(inward)) * .5, double(geom2::deltaI(inward)) * .5, ix, iz);

    double ax = 0, az = 0, bx = 0, bz = 0;
    geom2::edgePoint(grid, house.frame, e, 0, ax, az);
    geom2::edgePoint(grid, house.frame, e, 1, bx, bz);

    const uint32_t at = uint32_t(abris.points.size());
    abris.points.push_back(Point2{ax + ix - ox, az + iz - oz});
    abris.points.push_back(Point2{bx + ix - ox, bz + iz - oz});
    abris.segments.emplace_back(at, at + 1);
  }

  // Where a cluster may spawn: not on a door landing, a stair, or the way in.
  Board spawnable = area;

  for (const uint32_t e : doorEdges)
  {
    grid.remove(spawnable, grid.edgeCell(e));
  }

  for (const CellStair &stair : floor.stairs)
  {
    for (const uint32_t cell : wellCells(grid, stair))
    {
      grid.remove(spawnable, cell);
    }
  }

  grid.remove(spawnable, house.entranceLanding);

  if (house.spiral.has_value())
  {
    grid.remove(spawnable, house.spiral->landing);
  }

  if (spawnable.empty() || rule.density <= 0)
  {
    return true;
  }

  // Where the circulation is, which is what everything slides away from.
  std::vector<Point2> traffic;

  for (const uint32_t e : doorEdges)
  {
    double x = 0;
    double z = 0;
    geom2::edgePoint(grid, house.frame, e, .5, x, z);
    traffic.push_back(Point2{x, z});
  }

  for (const CellStair &stair : floor.stairs)
  {
    for (const uint32_t cell : wellCells(grid, stair))
    {
      if (grid.contains(area, cell))
      {
        double x = 0;
        double z = 0;
        geom2::cellPoint(grid, house.frame, cell, x, z);
        traffic.push_back(Point2{x, z});
      }
    }
  }

  if (grid.contains(area, house.entranceLanding))
  {
    double x = 0;
    double z = 0;
    geom2::edgePoint(grid, house.frame, house.entrance, .5, x, z);
    traffic.push_back(Point2{x, z});
  }

  const double cell = house.frame.cellMetres;
  const double base = rule.size > 0 ? rule.size : .05 + .4 * (1. - std::abs(stream.bell4()));

  // A piece may not be bigger than the room it stands in.
  //
  // The size is a fraction of a CELL, which is right for a room of several and
  // wrong for a room of one: a bed two and a half metres long in a two-metre
  // closet starts outside its own walls and then slides further out. Capped by
  // the largest rectangle that fits, which is the room's own measure of how
  // much space it really has.
  geom2::CellBounds box;
  double roomLimit = cell * .45;

  if (geom2::largestRectangle(grid, area, box))
  {
    roomLimit = std::min(roomLimit, double(std::min(box.rows(), box.columns())) * cell * .4);
  }

  const std::vector<std::pair<uint32_t, uint32_t>> cells = geom2::cells(spawnable);
  const uint32_t low = uint32_t(std::lround(.5 * rule.density * double(area.count())));
  const uint32_t high = uint32_t(std::lround(2. + rule.density * double(area.count())));
  const uint32_t wanted = std::min<uint32_t>(uint32_t(cells.size()), low + stream.below(std::max(1u, high - low + 1)));

  std::vector<std::pair<uint32_t, uint32_t>> chosen = stream.shuffled(cells);
  uint32_t clusterId = 0;

  for (uint32_t k = 0; k < wanted; ++k)
  {
    const uint32_t c = grid.cell(int32_t(chosen[k].first), int32_t(chosen[k].second));
    double cx = 0;
    double cz = 0;
    geom2::cellPoint(grid, house.frame, c, cx, cz);

    const double force =
      rule.press <= 0. ? 0. : (rule.press >= 1. ? 1. : std::min(stream.unit() / (1. - rule.press), 1.));
    const double angle = (force >= 1. || stream.unit() < rule.align)
                           ? 0.
                           : (1. - rule.align) * (3.14159265358979 / 4.) * (stream.bell3() * 2. - 1.);

    const uint32_t count = 1 + stream.below(std::max(1u, rule.pieces));
    std::vector<ClutterPiece> cluster;

    for (uint32_t p = 0; p < count; ++p)
    {
      const double falloff = std::pow(rule.shrink, double(count - p));
      const double size = std::min(base * (1. - falloff + falloff * stream.unit()) * cell, roomLimit);

      ClutterPiece piece;
      piece.cluster = clusterId;
      piece.angle = angle;
      piece.round = stream.unit() >= std::sqrt(std::clamp(rule.round, 0., 1.));
      piece.halfLong = piece.round ? .8 * size * .5 : size;
      piece.halfShort = piece.round ? .8 * size * .5 : size * .5 * (1. + std::abs(stream.bell4()));

      // Loose in the cell, with a margin so it starts inside.
      const double margin = std::min(piece.halfLong, cell * .45);
      piece.x = cx + (stream.unit() * 2. - 1.) * (cell * .5 - margin);
      piece.z = cz + (stream.unit() * 2. - 1.) * (cell * .5 - margin);
      if(!fitsInside(piece)){piece.x=cx;piece.z=cz;}
      if(fitsInside(piece))cluster.push_back(piece);
    }

    ++clusterId;
    if(cluster.empty())continue;

    // Which way to slide: away from the traffic, weighted by distance SQUARED,
    // so the FAR features dominate and the vector points into the quietest
    // corner rather than merely off the nearest door.
    double dx = 0;
    double dz = 0;

    for (const Point2 &q : traffic)
    {
      const double ux = cluster.front().x - q.x;
      const double uz = cluster.front().z - q.z;
      const double d2 = ux * ux + uz * uz;
      dx += ux * d2;
      dz += uz * d2;
    }

    double length = std::sqrt(dx * dx + dz * dz);

    if (length < 1e-9)
    {
      const double a = stream.unit() * 2. * 3.14159265358979;
      dx = std::cos(a);
      dz = std::sin(a);
      length = 1;
    }

    dx /= length;
    dz /= length;

    // How far the whole cluster can go before its first member touches
    // something. The minimum, so the cluster keeps its arrangement.
    double travel = 1e300;

    for (const ClutterPiece &piece : cluster)
    {
      travel = std::min(travel, sweep(abris, cornersOf(piece), dx, dz));
    }

    if (travel < 1e299)
    {
      const double moved = std::max(0.,travel * force-.003);

      for (ClutterPiece &piece : cluster)
      {
        piece.x += dx * moved;
        piece.z += dz * moved;
      }

      // A small piece sliding diagonally at a corner can pass between the two
      // wall segments that meet there: neither ray crosses, so nothing stops
      // it. One in a thousand did, by up to a third of a metre. The sweep is
      // the right mechanism and this is the guard it needs - if the slide put
      // a centre outside the room, it did not happen.
      bool escaped = false;

      for (const ClutterPiece &piece : cluster)
      {
        const double dxm = piece.x - house.frame.originX;
        const double dzm = piece.z - house.frame.originZ;
        const int32_t j = int32_t(std::floor((dxm * house.frame.axisX + dzm * house.frame.axisZ) / cell));
        const int32_t i = int32_t(std::floor((-dxm * house.frame.axisZ + dzm * house.frame.axisX) / cell));

        if (!grid.contains(area, grid.cell(i, j)))
        {
          escaped = true;
          break;
        }
      }

      if (escaped)
      {
        for (ClutterPiece &piece : cluster)
        {
          piece.x -= dx * moved;
          piece.z -= dz * moved;
        }
      }
    }

    // Belt and braces, and cheap: whatever the sweep missed, a piece whose
    // centre is not in a cell of its own room goes back to the middle of the
    // cell it was spawned in.
    for (ClutterPiece &piece : cluster)
    {
      const double dxm = piece.x - house.frame.originX;
      const double dzm = piece.z - house.frame.originZ;
      const double u = dxm * house.frame.axisX + dzm * house.frame.axisZ;
      const double v = -dxm * house.frame.axisZ + dzm * house.frame.axisX;

      if (!grid.contains(area, grid.cell(int32_t(std::floor(v / cell)), int32_t(std::floor(u / cell)))))
      {
        piece.x = cx;
        piece.z = cz;
      }
    }

    for (ClutterPiece &piece : cluster)
    {
      if(fitsInside(piece))out.push_back(piece);
    }
  }

  return true;
}

bool lightRoom(const CellGrid &grid,
               const CellHouse &house,
               const CellFloor &floor,
               const uint32_t room,
               RoomLight &out)
{
  out = RoomLight();

  if (room >= floor.roomAreas.size())
  {
    return false;
  }

  const Board &area = floor.roomAreas[room];
  out.radius = std::sqrt(double(area.count()) / 2.) * house.frame.cellMetres;

  uint32_t doors = 0;

  for (const CellDoor &door : floor.doors)
  {
    doors += door.a == room || door.b == room ? 1 : 0;
  }

  const bool corridor = geom2::narrow(grid, area).count() == area.count() && doors > 1;

  if (floor.level < 0 || corridor)
  {
    // The darkest corner: furthest, in inverse square, from everything that
    // gives light or carries traffic.
    out.onWall = true;
    double best = 1e300;

    for (const uint32_t e : floor.roomContours[room])
    {
      double x = 0;
      double z = 0;
      geom2::nodePoint(grid, house.frame, grid.decode(e).node, x, z);

      double score = 0;

      const auto against = [&](const double qx, const double qz) {
        const double d2 = (x - qx) * (x - qx) + (z - qz) * (z - qz);
        score += d2 < 1e-9 ? 1e9 : 1. / d2;
      };

      for (const uint32_t w : floor.windows)
      {
        if (std::find(floor.roomContours[room].begin(), floor.roomContours[room].end(), w) !=
            floor.roomContours[room].end())
        {
          double wx = 0;
          double wz = 0;
          geom2::edgePoint(grid, house.frame, w, .5, wx, wz);
          against(wx, wz);
        }
      }

      for (const CellDoor &door : floor.doors)
      {
        if (door.a != room && door.b != room)
        {
          continue;
        }

        double dx = 0;
        double dz = 0;
        geom2::edgePoint(grid, house.frame, door.edge, .5, dx, dz);
        against(dx, dz);
      }

      if (score < best)
      {
        best = score;
        out.x = x;
        out.z = z;
      }
    }

    return best < 1e299;
  }

  // The point deepest inside the room: the summit of its own straight skeleton.
  geom2::Polygon poly;

  if (!geom2::polygonOf(grid, house.frame, area, poly, nullptr))
  {
    geom2::cellPoint(grid, house.frame, grid.cell(0, 0), out.x, out.z);

    return false;
  }

  geom2::RoofOptions options;
  options.slope = 1;
  std::vector<geom2::RoofTriangle> triangles;

  double height = 0;

  if (geom2::roofFromSkeleton(poly, options, triangles, nullptr) &&
      geom2::roofSummit(triangles, out.x, out.z, height))
  {
    return true;
  }

  // A room the skeleton will not take gets the centre of its largest rectangle,
  // which is the same answer for a convex room and a safe one otherwise.
  geom2::CellBounds box;

  if (geom2::largestRectangle(grid, area, box))
  {
    double x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    geom2::nodePoint(grid, house.frame, grid.node(int32_t(box.i0), int32_t(box.j0)), x0, z0);
    geom2::nodePoint(grid, house.frame, grid.node(int32_t(box.i1) + 1, int32_t(box.j1) + 1), x1, z1);
    out.x = (x0 + x1) * .5;
    out.z = (z0 + z1) * .5;
  }

  return true;
}

bool furnishHouse(const CellHouse &house,
                  const CellDwellingSettings &settings,
                  Stream &stream,
                  std::vector<rewrite::DwellingFurniture> &furniture,
                  std::vector<RoomLight> &lights,
                  std::string *error)
{
  furniture.clear();
  lights.clear();

  std::vector<const CellFloor *> all;

  for (const CellFloor &floor : house.floors)
  {
    all.push_back(&floor);
  }

  if (house.basement.has_value())
  {
    all.push_back(&house.basement.value());
  }

  for (const CellFloor *floor : all)
  {
    const uint32_t storey = uint32_t(floor->level + (house.basement.has_value() ? 1 : 0));

    for (uint32_t r = 0; r < uint32_t(floor->roomAreas.size()); ++r)
    {
      // Nothing is put out in the court. Furniture stands on a floor, and a
      // court has none.
      if (r < floor->roomOutdoor.size() && floor->roomOutdoor[r] != 0)
      {
        continue;
      }

      const rewrite::RoomKind kind = r < floor->roomKinds.size() ? floor->roomKinds[r] : rewrite::RoomKind::Count;

      std::vector<ClutterPiece> pieces;
      furnishRoom(house.grid, house, *floor, r, clutterFor(kind), stream, pieces, settings.outerWallMetres, settings.innerWallMetres);

      for (const ClutterPiece &piece : pieces)
      {
        rewrite::DwellingFurniture item;
        item.room = r;
        item.floor = storey;
        item.kind = kind == rewrite::RoomKind::Bedroom     ? rewrite::FurnitureKind::Bed
                    : kind == rewrite::RoomKind::Kitchen   ? rewrite::FurnitureKind::Hearth
                    : kind == rewrite::RoomKind::Workshop  ? rewrite::FurnitureKind::Workbench
                    : kind == rewrite::RoomKind::Store     ? rewrite::FurnitureKind::Chest
                    : kind == rewrite::RoomKind::Cellar    ? rewrite::FurnitureKind::Chest
                                                           : rewrite::FurnitureKind::Table;

        for (const Point2 &corner : cornersOf(piece))
        {
          item.footprint.push_back(rewrite::Position{corner.x, double(floor->level) * house.gridClass.storeyMetres, corner.z});
        }

        furniture.push_back(std::move(item));
      }

      RoomLight light;

      if (lightRoom(house.grid, house, *floor, r, light))
      {
        lights.push_back(light);
      }
    }
  }

  (void)error;

  return true;
}

} // namespace procedural::world::generator
